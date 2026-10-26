// SPDX-License-Identifier: AGPL-3.0-only
#include "lsp_server.h"
#include "lsp_catalog.h"
#include "lsp_scope.h"                    /*  — moteur de portee/liaison */
#include "../src/owl_event_catalog.h"
#include "../src/ui_decl.h"
#include "../src/owl_keyword_catalog.h"   /*  — fiches mots-cles bilingues */
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#include <io.h>
#include <process.h>
extern unsigned long long __stdcall GetTickCount64(void);
/* Declaration locale pour eviter <windows.h> (conflit TokenType avec scanner.h,
 * cf. lsp_proc.c). HMODULE -> void*, LPSTR -> char*, DWORD -> unsigned long. */
extern unsigned long __stdcall GetModuleFileNameA(void* hModule, char* lpFilename,
                                                  unsigned long nSize);
#define LSP_NOW_MS() ((int64_t)GetTickCount64())
#else
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
static inline int64_t LSP_NOW_MS(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* Chemin du binaire owl-lsp COURANT (owl-lsp.exe et owl.exe sont distribues
 * ensemble : le voisin est le bon owl.exe dans tous les cas — release comme
 * arbre de dev). lsp_main.c est `int main(void)`, argv[0] indisponible : on
 * passe par les API OS. Remplit `out` avec le dossier du binaire (sans
 * separateur final). Retourne 1 si trouve, 0 sinon. */
static int lsp_self_exe_dir(char* out, size_t cap) {
    char buf[4096];
    buf[0] = '\0';
#ifdef _WIN32
    unsigned long n = GetModuleFileNameA(NULL, buf, (unsigned long)sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return 0;
    buf[n] = '\0';
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) != 0) return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0 || (size_t)n >= sizeof(buf)) return 0;
    buf[n] = '\0';
#endif
    /* Tronque au dernier separateur pour ne garder que le dossier. */
    char* lastSep = NULL;
    for (char* p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    if (!lastSep) return 0;
    *lastSep = '\0';
    size_t len = strlen(buf);
    if (len == 0 || len >= cap) return 0;
    memcpy(out, buf, len + 1);
    return 1;
}

/*  — exécution BORNÉE de `owl --check` (spawn isolé dans lsp_proc.c
 * pour ne pas polluer ce fichier avec windows.h, qui entre en conflit avec
 * scanner.h::TokenType). */
#include "lsp_proc.h"

/*  /  — un check qui depasse son delai est tue, mais on NE
 * republie RIEN sur textDocument/publishDiagnostics. Republier un unique
 * avertissement REMPLACERAIT (semantique LSP) les derniers bons diagnostics du
 * fichier actif : un projet simplement lent effacerait alors ses vraies erreurs
 * (fail silencieux). On preserve donc le dernier set affiche cote client (le
 * cache --check reste coherent) et on se contente d'un signal d'INFORMATION
 * discret et throttle — pas d'alarme « boucle » pour un gros projet lent.
 * L'auto-completion (independante de --check) reste active. */
#define LSP_TIMEOUT_NOTICE_THROTTLE_MS 30000  /* un rappel toutes les 30 s au plus */
static int64_t g_lastTimeoutNoticeMs = 0;

/*  — spy test-only : compte les entrees dans le chemin timeout
 * d'apply_check_result, independamment du throttle de notification
 * ci-dessus. Sert au harness a verifier qu'un test a bien exerce le chemin
 * LSP_RUN_TIMEOUT (pas de faux-positif par coincidence). */
static int lsp__check_timeout_branch_count = 0;
int lsp_check_timeout_branch_count(void) { return lsp__check_timeout_branch_count; }
void lsp_check_timeout_branch_reset(void) { lsp__check_timeout_branch_count = 0; }

static void lsp_notify_check_slow(void) {
    int64_t now = LSP_NOW_MS();
    if (g_lastTimeoutNoticeMs != 0 &&
        now - g_lastTimeoutNoticeMs < LSP_TIMEOUT_NOTICE_THROTTLE_MS) {
        return;  /* deja signale recemment : on ne spamme pas le client */
    }
    g_lastTimeoutNoticeMs = now;

    const bool en = (strcmp(lspServer.language, "en") == 0);
    JsonValue* mp = json_new_object();
    json_object_set(mp, "type", json_new_int(3)); /* MessageType.Info */
    json_object_set(mp, "message", json_new_string(en
        ? "Project verification is taking longer than expected \342\200\224 "
          "the latest results stay displayed."
        : "Verification du projet plus longue que prevu \342\200\224 "
          "les derniers resultats restent affiches."));
    lsp_send_notification("window/showMessage", mp);
}

/*  — `LSP_NOW_MS()` est utilise inline dans les handlers
 * (publish_diagnostics, handle_semantic_tokens, lsp_ensure_project_scan)
 * pour logger les timings de demarrage et identifier les goulots.
 * Seuil silencieux 5ms pour ne pas spammer le journal en regime normal. */

/* Include OWL scanner for tokenization and diagnostics */
#include "../src/scanner.h"
#include "../src/prescan.h"         /*  — source unique du comptage de profondeur */
#include "../src/formatter.h"       /* owl_format_ctx + owl_format_build_context */
#include "../src/project.h"         /* resolve_project_root, ProjectMode */
#include "../src/icons_catalog.h"   /*  — completion `icones.<prefix>` */

/* ================================================================
 * HELPERS
 * ================================================================ */

static void uri_to_filepath(const char* uri, char* outPath, int outSize);
static void filepath_to_uri(const char* path, char* outUri, int outSize);
static bool lsp_is_collection_kw(const char* start, int length);  /*  */

/*  — article EN entre la copule `est/is` et le type DSL.
 * En FR `un/une` est un filler jete par le scanner ; en EN `a` -> TOKEN_A
 * et `an` -> IDENTIFIER 2 chars, donc l'article s'intercale dans le flux
 * et casse toute detection par adjacence `prev == TOKEN_EST`. Les sites de
 * scan ci-dessous traitent cet article comme transparent : quand le token
 * courant est un article et que prev == EST, on NE met PAS a jour prev,
 * laissant le prochain token significatif voir prev == EST. Aligne sur la
 * passe prescan.c. */
/*  — « ce token ouvre-t-il un bloc ferme par FIN ? », rendu par la
 * source unique (prescan.h) et non plus par une liste recopiee.
 *
 * Le LSP en portait SEPT, une par boucle de profondeur, et toutes comptaient
 * la forme mono-ligne `SI c ALORS instr` et la forme inline `CONSTANTE K = 3`
 * comme des blocs a FIN. Le FIN englobant refermait alors la fausse ouverture,
 * la boucle sur-consommait le corps, et les membres declares plus bas
 * disparaissaient de la coloration et du go-to-definition — exactement la
 * panne que  avait corrigee pour `SINON SI`, une boucle a la fois.
 *
 * On passe par la porte TEXTE : elle vaut aussi pour les boucles qui marchent
 * sur un tableau de tokens deja scanne, la ou aucun Scanner vivant n'est a
 * portee. `tok.start` pointe dans le buffer du document, donc le peek voit la
 * suite de la ligne.
 *
 * Rend +1 / 0 / -1, ou 0 si la table ne connait pas le token : le site garde
 * alors la main sur ses propres extras (genres de widget, structurels
 * post-copule, AVEC — cf. prescan.h pour la raison de leur absence). */
static int lsp_depth_delta(Token tok) {
    int delta = 0;
    prescan_depth_delta_text(tok.start, &delta);
    return delta;
}

/* La table a-t-elle un avis sur ce token ? Sert a laisser passer les tokens
 * qu'elle ne connait pas vers la liste d'extras du site. */
static bool lsp_depth_table_knows(Token tok) {
    return prescan_token_in_depth_table(tok.type);
}

static bool tok_is_en_article(Token t) {
    if (t.type == TOKEN_A) return true;  /* `a` / `to` */
    if (t.type == TOKEN_IDENTIFIER && t.length == 2 &&
        (t.start[0] == 'a' || t.start[0] == 'A') &&
        (t.start[1] == 'n' || t.start[1] == 'N')) return true;  /* `an` */
    return false;
}

/*  — POINT DE VERITE UNIQUE du prefixe d'une declaration de champ :
 * le triplet de modificateurs `Champ` (FR) / `Field` / `Control` (les deux
 * nameEN canon du type Champ, ) et le filler optionnel `de` / `of`
 * qui peut le suivre (`X est un Champ de Saisie` == `X est un Champ Saisie`,
 *  ; le scanner mappe `de` ET `of` sur TOKEN_DE).
 *
 * Les deux se traitent pareil cote scan LSP : `continue` SANS mettre a jour
 * `prev`, pour que le genre reel (Saisie/Bouton/...) soit lu avec prev == EST
 * au tour suivant. Le kind retourne distingue les deux pour le seul call-site
 * qui en a besoin (find_dsl_block_end marque le modificateur comme preuve
 * qu'on ouvre un bloc widget, et pas une variable homonyme d'un genre).
 *
 * `wasEst` fait partie du predicat, il n'est pas optionnel : un `de` de
 * collection (`tab est un tableau de Image`) arrive avec prev == TOKEN_TABLEAU
 * et ne doit PAS etre traverse — sinon `Image` serait lu comme un genre et
 * ouvrirait un bloc fantome jamais ferme.
 *
 * Miroir de token_is_champ_modifier / skip_optional_de_after_champ
 * (compiler.c). Cinq scans de ce fichier en dependent ; la regle y etait
 * dupliquee et chaque correctif de la serie 0513 / 0688 / 1290 / 1378 / 1385
 * n'en a repare qu'une partie (contexte juste mais bornes fausses, ou
 * l'inverse). Toute evolution (nouveau modificateur, nouveau filler, nouvelle
 * langue) se fait ICI et nulle part ailleurs. */
typedef enum {
    LSP_CHAMP_PREFIX_NONE = 0,
    LSP_CHAMP_PREFIX_MODIFIER,  /* Champ / Field / Control */
    LSP_CHAMP_PREFIX_FILLER     /* de / of */
} LspChampPrefix;

/* Le triplet seul, au niveau mot brut : pour les sites qui travaillent sur du
 * texte decoupe a la main (mot partiel sous le curseur) et non sur des tokens. */
static bool lsp_is_champ_modifier_word(const char* w, int len) {
    if (!w || len <= 0) return false;
    if (len == 5 && (strncasecmp(w, "Champ", 5) == 0
                  || strncasecmp(w, "Field", 5) == 0)) return true;
    if (len == 7 && strncasecmp(w, "Control", 7) == 0) return true;
    return false;
}

static LspChampPrefix lsp_champ_prefix_kind(Token t, bool wasEst) {
    if (!wasEst) return LSP_CHAMP_PREFIX_NONE;
    if (t.type == TOKEN_DE) return LSP_CHAMP_PREFIX_FILLER;
    if (t.type != TOKEN_IDENTIFIER) return LSP_CHAMP_PREFIX_NONE;
    if (lsp_is_champ_modifier_word(t.start, t.length))
        return LSP_CHAMP_PREFIX_MODIFIER;
    return LSP_CHAMP_PREFIX_NONE;
}

/*  — scan-arriere bilingue de la copule+article dans le source brut.
 * A partir de src[pos] (premier octet non-blanc apres le nom de variable),
 * reconnait la copule `est` (FR) ou `is` (EN) suivie d'un article
 * `un`/`une`/`a`/`an` (formes mixtes `is un`, `est a` incluses). L'article
 * doit etre suivi d'un blanc (frontiere de mot : evite `is answer`).
 * Retourne l'offset du premier octet du token type (blancs apres l'article
 * consommes), ou -1 si aucun match. Version bytes du couple copule/article
 * gere ailleurs au niveau token (tok_is_en_article). */
static int scan_copule_article_bytes(const char* src, int pos) {
    int p;
    if (strncasecmp(src + pos, "est", 3) == 0 &&
        (src[pos + 3] == ' ' || src[pos + 3] == '\t')) {
        p = pos + 3;
    } else if (strncasecmp(src + pos, "is", 2) == 0 &&
               (src[pos + 2] == ' ' || src[pos + 2] == '\t')) {
        p = pos + 2;
    } else {
        return -1;
    }
    while (src[p] == ' ' || src[p] == '\t') p++;
    /* article : plus long d'abord (une avant un, an avant a). */
    int alen;
    if (strncasecmp(src + p, "une", 3) == 0)      alen = 3;
    else if (strncasecmp(src + p, "un", 2) == 0)  alen = 2;
    else if (strncasecmp(src + p, "an", 2) == 0)  alen = 2;
    else if (strncasecmp(src + p, "a", 1) == 0)   alen = 1;
    else return -1;
    char nb = src[p + alen];
    if (nb != ' ' && nb != '\t') return -1;  /* frontiere de mot obligatoire */
    p += alen;
    while (src[p] == ' ' || src[p] == '\t') p++;
    return p;
}

/* Extract the word at a given line/character position in source text */
/* Returns true for bytes that are part of an OWL identifier:
 * ASCII alnum/underscore, or any UTF-8 continuation/leading byte (>= 0x80)
 * so that accented names like "ChaîneCompare" are captured whole. */
#define IS_ID_BYTE(b) (isalnum((unsigned char)(b)) || (b) == '_' || (unsigned char)(b) >= 0x80)

/*  — convertit une position `character` (unités UTF-16, convention LSP)
 * en offset OCTET dans la ligne. Défini plus bas ; forward-declaré ici car
 * extract_word_at doit convertir la colonne avant d'indexer les octets. */
static int lsp_utf16_to_byte_in_line(const char* lineStart, int character);

static int extract_word_at(const char* src, int srcLen, int line, int character,
                           char* outWord, int outWordSize) {
    int curLine = 0;
    int lineStart = 0;
    for (int i = 0; i < srcLen && curLine < line; i++) {
        if (src[i] == '\n') { curLine++; lineStart = i + 1; }
    }

    /* `character` est en unités UTF-16 : un identifiant accentué avant le
     * curseur (`Thème`, `é` = 2 octets) décale l'offset octet. Sans cette
     * conversion, `Thème = ThèmeDémo` place le curseur dans le blanc et le mot
     * extrait est vide (go-to-def / hover renvoient null). ASCII : identité. */
    int byteInLine = lsp_utf16_to_byte_in_line(src + lineStart, character);
    int wordStart = lineStart + byteInLine;
    int wordEnd = wordStart;
    if (wordStart >= srcLen) return 0;

    while (wordStart > lineStart && IS_ID_BYTE(src[wordStart - 1]))
        wordStart--;
    while (wordEnd < srcLen && IS_ID_BYTE(src[wordEnd]))
        wordEnd++;

    int wordLen = wordEnd - wordStart;
    if (wordLen <= 0 || wordLen >= outWordSize) return 0;
    memcpy(outWord, src + wordStart, wordLen);
    outWord[wordLen] = '\0';
    return wordLen;
}

/* Find document index by URI */
static int find_doc(const char* uri) {
    for (int i = 0; i < lspServer.documentCount; i++) {
        if (strcmp(lspServer.documents[i].uri, uri) == 0) return i;
    }
    return -1;
}

/*  — `lsp_is_official_event` : vrai si `name` (forme source brute)
 * designe un event officiel. Utilise par le handler semantic tokens pour
 * NE PAS emettre de token method (vert) sur la declaration
 * `PROCEDURE <event>(` — la grammar TextMate prend alors le relais et
 * colore bleu (support.function.event.owl).
 *
 * 4 catalogues C consultes (source unique, plus de table statique LSP) :
 *   1. owl_event_catalog (widget events typés : surClic, surMolette, ...)
 *   2. owl_etat_event_catalog (events bloc Etat : Filtre/Lecture/Fermeture)
 *   3. owl_etat_subcontrol_event_catalog (sub-controle Etat : AvantImpression)
 *   4. owl_window_lifecycle_catalog (Fenetre : FinInitialisation/...)
 *
 * Consequence directe du retrait de la table LSP_LIFECYCLE_EVENTS :
 * `AprèsImpression` (jamais enregistre dans aucun catalogue) cesse d'etre
 * reconnu comme event officiel. C'etait un fantome (compilo le rejette).
 *
 * Comparaison casse+accent insensible (deleguee aux find des catalogues),
 * coherente avec le scanner OWL. */
static bool lsp_is_official_event(const char* name) {
    if (!name || !name[0]) return false;

    /* (1) Catalogues widget : iter sur tous les OwlControlKind connus. */
    for (int kind = OWL_CONTROL_LABEL; kind <= OWL_CONTROL_WINDOW; kind++) {
        if (owl_event_catalog_find((OwlControlKind)kind, name)) return true;
    }
    /* (2) Bloc Etat top-level. */
    if (owl_etat_event_catalog_find(name)) return true;
    /* (3) Sous-controle Etat. */
    if (owl_etat_subcontrol_event_catalog_find(name)) return true;
    /* (4) Lifecycle Fenetre. */
    if (owl_window_lifecycle_catalog_find(name)) return true;
    /* (5) Events de niveau ServiceREST / OpérationHTTP. */
    if (owl_service_event_catalog_find(name)) return true;
    if (owl_operation_event_catalog_find(name)) return true;

    return false;
}

/*  — recupere le premier OwlEventDef correspondant a `name`
 * (canon OU nameEN, match loose delegue aux find des catalogues) sur les
 * 4 sous-categories. Utilise par le hover events. NULL si introuvable.
 * L'ambiguite cross-kind (meme nom d'event sur plusieurs widgets) est
 * benigne ici : la descFR/descEN d'un event partagee est portee par la
 * meme famille (FAM_*), donc la premiere occurrence suffit pour le hover.
 * EXCEPTION : Fermeture/Closing existe dans le catalogue Etat ET dans le
 * lifecycle Fenetre avec des descs divergentes (rapport vs fenetre). Cette
 * collision inter-catalogues est desambiguisee au call-site du hover
 * (handle_hover, ) selon le bloc DSL englobant — pas ici. */
static const OwlEventDef* lsp_find_event_def_any(const char* name) {
    if (!name || !name[0]) return NULL;
    for (int kind = OWL_CONTROL_LABEL; kind <= OWL_CONTROL_WINDOW; kind++) {
        const OwlEventDef* d = owl_event_catalog_find((OwlControlKind)kind, name);
        if (d) return d;
    }
    const OwlEventDef* d;
    if ((d = owl_etat_event_catalog_find(name)))            return d;
    if ((d = owl_etat_subcontrol_event_catalog_find(name))) return d;
    if ((d = owl_window_lifecycle_catalog_find(name)))      return d;
    if ((d = owl_collection_event_catalog_find(name)))      return d;  /*  */
    if ((d = owl_service_event_catalog_find(name)))         return d;  /*  */
    if ((d = owl_operation_event_catalog_find(name)))       return d;  /*  */
    return NULL;
}

/*  — events de cycle de vie du PROJET. Un `PROCEDURE Fermeture()`
 * declare au top-level d'un projet.owl (en sibling du bloc `IDENT est un
 * Projet`, pas a l'interieur) est l'event de teardown du projet, appele une
 * fois en fin d'execution apres le point d'entree. Distinct de la Fermeture
 * d'une Fenetre (fermeture d'une fenetre) et de la Fermeture d'un Etat (fin
 * de rapport), avec lesquelles il partage le nom. Catalogue LSP minimal :
 * l'auto-call runtime vit dans project_run.c, ce catalogue ne sert que la
 * completion + le hover. Bilingue (invariant 3). */
static const OwlEventDef PROJET_LIFECYCLE_EVENTS[] = {
    { .canonical = "Fermeture", .nameEN = "Closing",
      .descFR = "Tir\xc3\xa9 une fois en fin d'ex\xc3\xa9""cution du projet, "
                "apr\xc3\xa8s le point d'entr\xc3\xa9""e \xe2\x80\x94 dernier "
                "point pour lib\xc3\xa9rer les ressources globales du projet "
                "(fermeture de connexions, journalisation finale, sauvegarde "
                "d'\xc3\xa9tat). La fermeture du projet ne peut plus \xc3\xaatre "
                "annul\xc3\xa9""e \xc3\xa0 ce stade.",
      .descEN = "Fires once at the end of the project run, after the entry "
                "point \xe2\x80\x94 the last place to release the project's "
                "global resources (close connections, final logging, save "
                "state). Project teardown cannot be cancelled at this point.",
      .paramCount = 0, .params = NULL },
};

/*  — resolution d'un event de cycle de vie projet par nom canon FR
 * ou alias EN (casse-insensible). NULL si aucun. */
static const OwlEventDef* projet_lifecycle_find(const char* name) {
    if (!name || !name[0]) return NULL;
    size_t n = sizeof(PROJET_LIFECYCLE_EVENTS) / sizeof(PROJET_LIFECYCLE_EVENTS[0]);
    for (size_t i = 0; i < n; i++) {
        const OwlEventDef* d = &PROJET_LIFECYCLE_EVENTS[i];
        if ((d->canonical && strcasecmp(name, d->canonical) == 0)
         || (d->nameEN    && strcasecmp(name, d->nameEN)    == 0))
            return d;
    }
    return NULL;
}

/*  — vrai si le document declare un bloc Projet au top-level
 * (`IDENT est un Projet` FR / `IDENT is a Project` EN, col 0). Sert a
 * reconnaitre un document projet.owl pour la completion/hover des events
 * de cycle de vie du projet, declares en sibling top-level du bloc. */
static bool source_declares_projet(const char* src) {
    if (!src) return false;
    const char* p = src;
    while (*p) {
        const char* lineStart = p;
        while (*p == ' ' || *p == '\t') p++;
        /* Declaration de bloc top-level uniquement (col 0), pas de ligne
         * indentee ni de commentaire. */
        if (p == lineStart && *p != '\n' && *p != '\r' && *p != '\0'
            && !(*p == '/' && p[1] == '/')) {
            const char* idStart = p;
            while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                || (*p >= '0' && *p <= '9') || *p == '_'
                || (unsigned char)*p >= 0x80) p++;
            if (p != idStart) {
                while (*p == ' ' || *p == '\t') p++;
                /*  — copule+article bilingue (`est un[e]` FR /
                 * `is a[n]` EN / formes mixtes) via la source de verite
                 * commune, puis nom du type bilingue `Projet`/`Project`. */
                int off = scan_copule_article_bytes(p, 0);
                if (off >= 0) {
                    const char* q = p + off;
                    if (strncasecmp(q, "Projet", 6) == 0) {
                        char a = q[6];
                        if (a==' '||a=='\t'||a=='\r'||a=='\n'||a=='\0') return true;
                    } else if (strncasecmp(q, "Project", 7) == 0) {
                        char a = q[7];
                        if (a==' '||a=='\t'||a=='\r'||a=='\n'||a=='\0') return true;
                    }
                }
            }
        }
        /* Ligne suivante. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return false;
}

/*  — nom EN d'un type primitif pour le hover event.
 *
 * Les params d'event (OwlEventDef.params[].typeName) portent un nom de
 * type FR (entier, chaîne…). Contrairement aux natives/types/champs, ces
 * types ne transitent pas par OWL_TYPE_CATALOG (qui ne contient que les
 * types composes), et le LSP standalone ne linke pas le registry — d'ou ce
 * micro-mapping des primitifs scalaires (miroir de leur nameEN registry).
 * Tous les types d'event V1 sont des primitifs ; fallback = le nom FR. */
static const char* lsp_primitive_type_en(const char* fr) {
    if (!fr || !*fr) return fr;
    static const struct { const char* fr; const char* en; } MAP[] = {
        {"entier", "integer"}, {"r\xc3\xa9""el", "real"}, {"reel", "real"},
        {"cha\xc3\xaene", "string"}, {"chaine", "string"},
        {"bool\xc3\xa9""en", "boolean"}, {"booleen", "boolean"},
        {"dur\xc3\xa9""e", "duration"}, {"duree", "duration"},
        {"date", "date"}, {"heure", "time"}, {"dateheure", "datetime"},
        {NULL, NULL}
    };
    for (int i = 0; MAP[i].fr; i++)
        if (strcasecmp(MAP[i].fr, fr) == 0) return MAP[i].en;
    return fr;
}

/*  — formate la signature attendue d'un field callback pour le hover
 * LSP. `raw` = liste de types canon FR separes par virgule ("chaîne,booléen"),
 * "" = callback sans parametre. Rend "(chaîne, booléen)" (chaque type traduit
 * en EN si wantEN) ou "(aucun paramètre)" / "(no parameter)". */
static void fmt_callback_sig(char* dst, size_t n, const char* raw, bool wantEN) {
    if (n == 0) return;
    if (!raw || !*raw) {
        snprintf(dst, n, "(%s)", wantEN ? "no parameter" : "aucun param\xc3\xa8tre");
        return;
    }
    size_t off = 0;
    off += (size_t)snprintf(dst + off, n - off, "(");
    const char* p = raw;
    bool first = true;
    while (*p && off < n) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char tok[64];
        if (len >= sizeof(tok)) len = sizeof(tok) - 1;
        memcpy(tok, p, len);
        tok[len] = '\0';
        const char* t = tok;
        while (*t == ' ') t++;                 /* trim espace de tete */
        const char* disp = wantEN ? lsp_primitive_type_en(t) : t;
        off += (size_t)snprintf(dst + off, n - off, "%s%s",
                                first ? "" : ", ", disp);
        first = false;
        if (!comma) break;
        p = comma + 1;
    }
    if (off < n) snprintf(dst + off, n - off, ")");
}

/*  — Types primitifs : source de verite unique partagee par les DEUX
 * points d'emission de la completion (contexte type-declaration `x est un
 * <cursor>` ET dump general top-level). Evite la derive entre deux listes
 * jadis divergentes (l'une accentuee avec paires FR/EN, l'autre ASCII FR-only).
 *
 *   name    = forme FR canon accentuee (source scanner).
 *   nameEN  = forme EN canon (source scanner) quand elle differe ; NULL si pas
 *             de forme EN distincte (date/buffer/uuid) -> fallback
 *             FR a l'identique dans les deux langues.
 *   article = article FR pour l'insertText du contexte type-declaration.
 *   desc    = detail neutre (contexte type-declaration uniquement).
 *
 * Le filtrage par langue (lspServer.language via wantEN) est fait au point
 * d'emission : mode EN -> nameEN si distinct sinon name ; mode FR -> name.
 * Le runtime reste bilingue (entier ET integer valides) ; seule la completion
 * segmente. */
typedef struct { const char* name; const char* nameEN;
                 const char* article; const char* desc;
                 const char* descEN; } OwlPrimType;
static const OwlPrimType OWL_PRIM_TYPES[] = {
    {"entier",      "integer",   "un",  "Type entier 64-bit signé",             "Signed 64-bit integer type"},
    {"chaîne",      "string",    "une", "Type chaîne UTF-8 (raw, doublage canon)","UTF-8 string type (raw, canonical doubling)"},
    {"booléen",     "boolean",   "un",  "Type booléen (Vrai/Faux)",             "Boolean type (True/False)"},
    {"réel",        "real",      "un",  "Type réel double-précision",           "Double-precision real type"},
    {"monétaire",   "currency",  "un",  "Type monétaire (cents, scale 4)",      "Currency type (cents, scale 4)"},
    {"numérique",   "numeric",   "un",  "Type numérique 128-bit (38 chiffres)", "128-bit numeric type (38 digits)"},
    {"caractère",   "character", "un",  "Type caractère",                       "Character type"},
    {"date",        NULL,        "une", "Type date (AAAAMMJJ)",                 "Date type (YYYYMMDD)"},
    {"heure",       "time",      "une", "Type heure (HHMMSSCC)",                "Time type (HHMMSSCC)"},
    {"dateheure",   "datetime",  "une", "Type date+heure (AAAAMMJJHHMMSS)",     "Date+time type (YYYYMMDDHHMMSS)"},
    {"durée",       "duration",  "une", "Type durée (intervalle, ms)",          "Duration type (interval, ms)"},
    {"buffer",      NULL,        "un",  "Type buffer binaire (octets)",         "Binary buffer type (bytes)"},
    {"uuid",        NULL,        "un",  "Type UUID",                            "UUID type"},
    {"tableau",     "array",     "un",  "Type tableau",                         "Array type"},
    {NULL, NULL, NULL, NULL, NULL}
};

/*  — label du type primitif filtre par langue. Mode EN : nameEN si une
 * forme EN distincte existe, sinon fallback FR (name). Mode FR : name. */
static const char* owl_prim_label(const OwlPrimType* p, bool wantEN) {
    bool hasEN = (p->nameEN && *p->nameEN && strcmp(p->nameEN, p->name) != 0);
    return (wantEN && hasEN) ? p->nameEN : p->name;
}

/*  — vrai si `name` designe un type primitif (forme FR canon accentuee,
 * ASCII sans accent, ou forme EN), casse-insensible. Sert a reconnaitre un
 * decl-assign de type primitif (`n est un entier = `) que owl_type_info_find
 * ignore (primitifs hors OWL_TYPE_CATALOG). */
static bool owl_prim_type_is(const char* name) {
    if (!name || !*name) return false;
    char sb[64];
    scanner_strip_accents(name, (int)strlen(name), sb, sizeof(sb));
    for (int i = 0; OWL_PRIM_TYPES[i].name; i++) {
        char pb[64];
        scanner_strip_accents(OWL_PRIM_TYPES[i].name,
                              (int)strlen(OWL_PRIM_TYPES[i].name), pb, sizeof(pb));
        if (strcasecmp(sb, pb) == 0) return true;
        if (OWL_PRIM_TYPES[i].nameEN && strcasecmp(sb, OWL_PRIM_TYPES[i].nameEN) == 0)
            return true;
    }
    return false;
}

/*  — mot-cle de fermeture de bloc canon selon la langue active.
 * Source de verite unique pour tous les corps de snippet auto-fermants
 * emis par la completion (events lifecycle widget/projet/classe, fixtures
 * test, ...). Evite de re-hardcoder `FIN` FR sur chaque site (support #158 :
 * en mode EN les snippets d'event fermaient encore par FIN au lieu de END). */
static const char* snippet_end_kw(bool wantEN) { return wantEN ? "END" : "FIN"; }

/* Vrai si `path` est `ancestor` ou un descendant (comparaison prefixe,
 * separateurs / et \ unifies). Borne la resolution projet au workspace. */
static bool fmt_path_within(const char* path, const char* ancestor) {
    if (!ancestor || !ancestor[0]) return true;  /* pas de borne */
    for (size_t i = 0; ancestor[i]; i++) {
        char pc = path[i], ac = ancestor[i];
        if (pc == '\\') pc = '/';
        if (ac == '\\') ac = '/';
        if (pc == '\0' || pc != ac) return false;
    }
    return true;
}

/* textDocument/formatting (ticket 0326) — formate le document complet via le
 * formatter heberge (backend catalogue), avec contexte de casse cross-fichier
 * borne au workspace. Renvoie un TextEdit unique couvrant tout le
 * document, ou null si rien a faire / doc inconnu. */
JsonValue* handle_formatting(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    const char* uri = td ? json_get_string(td, "uri") : NULL;
    if (!uri) return json_new_null();
    int d = find_doc(uri);
    if (d < 0 || !lspServer.documents[d].content) return json_new_null();
    const char* content = lspServer.documents[d].content;

    char filepath[1024];
    uri_to_filepath(uri, filepath, sizeof(filepath));

    /* Frontiere projet bornee au workspace : on ne resout/scanne jamais
     * au-dessus de rootPath (un projet.owl egare haut ou un fichier hors
     * workspace ne doit pas declencher le scan d'un disque entier). */
    char root[1024];
    ProjectMode mode = resolve_project_root(filepath, root, sizeof(root));
    bool recursive = (mode == PROJECT_MODE_PROJECT);
    if (lspServer.rootPath[0] && !fmt_path_within(root, lspServer.rootPath)) {
        /* Resolution echappee du workspace -> confine au dossier du fichier. */
        snprintf(root, sizeof(root), "%s", filepath);
        char* slash = strrchr(root, '/');
        char* back  = strrchr(root, '\\');
        if (back && (!slash || back > slash)) slash = back;
        if (slash) *slash = '\0';
        recursive = false;
    }

    const char* base = owl_format_path_basename(filepath);
    bool isTest = owl_format_basename_is_test(base);
    OwlFormatContext* ctx = owl_format_build_context(root, base, isTest, recursive);
    char* formatted = owl_format_ctx(content, ctx);
    owl_format_context_free(ctx);
    if (!formatted) return json_new_null();

    /* Si le format ne change rien, renvoyer un tableau vide (pas d'edit). */
    if (strcmp(formatted, content) == 0) {
        free(formatted);
        return json_new_array();
    }

    /* Fin du document : nb de '\n' = derniere ligne, + bytes apres le dernier
     * '\n'. En bytes (>= unites UTF-16) -> surestime, le client clampe. */
    int endLine = 0, endChar = 0;
    for (const char* p = content; *p; p++) {
        if (*p == '\n') { endLine++; endChar = 0; }
        else endChar++;
    }

    JsonValue* start = json_new_object();
    json_object_set(start, "line", json_new_int(0));
    json_object_set(start, "character", json_new_int(0));
    JsonValue* end = json_new_object();
    json_object_set(end, "line", json_new_int(endLine));
    json_object_set(end, "character", json_new_int(endChar));
    JsonValue* range = json_new_object();
    json_object_set(range, "start", start);
    json_object_set(range, "end", end);
    JsonValue* edit = json_new_object();
    json_object_set(edit, "range", range);
    json_object_set(edit, "newText", json_new_string(formatted));
    JsonValue* edits = json_new_array();
    json_array_push(edits, edit);

    free(formatted);
    return edits;
}

/* Scan a document and collect user-defined symbols (procedures, variables, structs...) */
typedef struct {
    char name[128];
    int line;         /* 0-based */
    int character;    /* 0-based */
    int kind;         /* LSP CompletionItemKind: 3=Function, 6=Variable, 22=Struct, 13=Enum */
    char detail[256]; /* Extra info */
    int docIndex;     /* Which document it came from */
    bool isGlobal;    /* true si declare au top-level fichier (Main/Collection),
                       * hors PROCEDURE/FONCTION/CLASSE/STRUCTURE/etc.
                       * Permet la coloration "variable globale" specifique. */
} UserSymbol;

static UserSymbol userSymbols[1024];
static int userSymbolCount = 0;

/*  — Instances globales top-level (`IDENT est un <Type>`) captees
 * depuis un fichier disque NON ouvert (docIndex==-1). Le prescan des docs
 * ouverts les met dans userSymbols[] (isGlobal), mais userSymbols n'indexe
 * que les documents ouverts (docIndex>=0). Cet index disque porte le nom +
 * l'URI file:// + la position pour que la coloration (variable globale cyan)
 * et le go-to-definition cross-fichier fonctionnent meme quand le compagnon
 * declarant la globale est FERME. Dedup naturel au grain fichier : le scan
 * disque skip les fichiers deja ouverts (path_matches_open_doc, priorite
 * buffer), donc une globale d'un fichier ouvert n'apparait jamais ici —
 * meme discipline que userClasses[] pour les classes cross-fichier. */
typedef struct {
    char name[128];
    char type[128]; /*  — type declare (`est un <Type>`) pour la
                     * completion de valeur typee (instances de Thème, etc.) */
    char fileUri[1024];
    int line;       /* 0-based ligne de la declaration */
    int character;  /* 0-based colonne du nom */
} DiskGlobalInfo;
static DiskGlobalInfo diskGlobals[256];
static int diskGlobalCount = 0;

static DiskGlobalInfo* disk_global_lookup(const char* name) {
    for (int i = 0; i < diskGlobalCount; i++) {
        if (strcasecmp(diskGlobals[i].name, name) == 0) return &diskGlobals[i];
    }
    return NULL;
}

/*  — Localisation de declaration des Fenetres/Etats
 * (`IDENT est une Fenetre` / `IDENT est un Etat`). userWindows[] ne porte
 * que le NOM (coloration class + completion) ; cet index parallele porte
 * nom + position + source pour que le go-to-definition saute a la
 * declaration : docIndex >= 0 pour un doc OUVERT (Location via
 * documents[docIndex].uri), fileUri renseigne pour un compagnon disque
 * FERME (docIndex == -1). Meme discipline que diskGlobals[]/userClasses[] :
 * rempli au scan des sources (open + disque via
 * scan_one_source_for_user_symbols), reset a chaque scan_user_symbols_force,
 * dedup naturel au grain fichier (le scan disque skip les fichiers ouverts,
 * priorite buffer). */
typedef struct {
    char name[128];
    char fileUri[1024]; /* "" si doc ouvert (Location via documents[docIndex]) */
    int line;           /* 0-based ligne de la declaration */
    int character;      /* 0-based colonne du nom */
    int docIndex;       /* >=0 doc ouvert, -1 fichier disque */
    bool isWindow;      /*  — true = `est une Fenêtre`, false = `est un Etat`.
                         * Distingue les candidats pour Projet..FenêtreDémarrage. */
} WindowDefInfo;
static WindowDefInfo windowDefs[256];
static int windowDefCount = 0;

static WindowDefInfo* window_def_lookup(const char* name) {
    for (int i = 0; i < windowDefCount; i++) {
        if (strcasecmp(windowDefs[i].name, name) == 0) return &windowDefs[i];
    }
    return NULL;
}

/*  — noms de blocs CollectionDeProcedures declares (open + disque).
 * Un nom de collection se colore class (vert fonce) comme un nom de
 * Fenetre/Etat, a la declaration comme en receveur d'acces qualifie
 * (`COL.membre`), et resout en go-to-def vers sa declaration. Meme
 * discipline que windowDefs[] : rempli au scan des sources, reset a chaque
 * scan_user_symbols_force, cross-file via fileUri (compagnon ferme). */
typedef struct {
    char name[128];
    char fileUri[1024]; /* "" si doc ouvert (Location via documents[docIndex]) */
    int line;
    int character;
    int docIndex;       /* >=0 doc ouvert, -1 fichier disque */
} CollectionDefInfo;
static CollectionDefInfo userCollections[256];
static int userCollectionCount = 0;

static CollectionDefInfo* collection_def_lookup(const char* name) {
    for (int i = 0; i < userCollectionCount; i++) {
        if (strcasecmp(userCollections[i].name, name) == 0) return &userCollections[i];
    }
    return NULL;
}

/*  — procedures/fonctions user declarees dans un fichier disque NON
 * ouvert. userSymbols[] n'indexe que les docs ouverts (docIndex>=0), donc un
 * appel `Doubler(...)` d'une procedure de collection (ou de tout compagnon
 * ferme) n'etait ni colore method ni resolu en go-to-def tant que le fichier
 * n'etait pas ouvert. Cet index disque comble le trou, meme discipline que
 * diskGlobals[]. */
typedef struct {
    char name[128];
    char fileUri[1024];
    int line;
    int character;
} DiskProcInfo;
static DiskProcInfo diskProcs[256];
static int diskProcCount = 0;

static DiskProcInfo* disk_proc_lookup(const char* name) {
    for (int i = 0; i < diskProcCount; i++) {
        if (strcasecmp(diskProcs[i].name, name) == 0) return &diskProcs[i];
    }
    return NULL;
}

/*  — vrai si un item de label `name` est deja present dans `items`
 * (dedup des instances collectees depuis plusieurs sources). */
static bool completion_has_label(JsonValue* items, const char* name) {
    if (!items || items->type != JSON_ARRAY) return false;
    for (int i = 0; i < items->as.arrayVal.count; i++) {
        const char* l = json_get_string(items->as.arrayVal.items[i], "label");
        if (l && strcasecmp(l, name) == 0) return true;
    }
    return false;
}

/*  — emet un item de completion pour une instance nommee (valeur d'un
 * champ metadonnee de bloc Projet). kind Variable (6), rang en tete. */
static void emit_instance_item(JsonValue* items, const char* name,
                               const char* typeLabel) {
    if (completion_has_label(items, name)) return;
    JsonValue* item = json_new_object();
    json_object_set(item, "label", json_new_string(name));
    json_object_set(item, "kind", json_new_int(6)); /* Variable */
    if (typeLabel && *typeLabel)
        json_object_set(item, "detail", json_new_string(typeLabel));
    char sortBuf[160];
    snprintf(sortBuf, sizeof(sortBuf), "0_%.150s", name);
    json_object_set(item, "sortText", json_new_string(sortBuf));
    json_array_push(items, item);
}

/*  — collecte les instances globales `IDENT est un <targetTypeAscii>`
 * (type strippe accent/casse-insensible) depuis les docs ouverts (userSymbols)
 * et les compagnons fermes (diskGlobals). Sert la completion de valeur du champ
 * Projet..Thème. `typeLabel` sert de detail sur les items emis. */
static void emit_typed_instances(JsonValue* items, const char* targetTypeAscii,
                                 const char* typeLabel) {
    for (int i = 0; i < userSymbolCount; i++) {
        if (userSymbols[i].kind != 6 || !userSymbols[i].isGlobal) continue;
        char strip[128];
        scanner_strip_accents(userSymbols[i].detail,
                              (int)strlen(userSymbols[i].detail),
                              strip, sizeof(strip));
        if (strcasecmp(strip, targetTypeAscii) != 0) continue;
        emit_instance_item(items, userSymbols[i].name, typeLabel);
    }
    for (int i = 0; i < diskGlobalCount; i++) {
        char strip[128];
        scanner_strip_accents(diskGlobals[i].type,
                              (int)strlen(diskGlobals[i].type),
                              strip, sizeof(strip));
        if (strcasecmp(strip, targetTypeAscii) != 0) continue;
        emit_instance_item(items, diskGlobals[i].name, typeLabel);
    }
}

/*  — collecte les instances de Fenêtre (`IDENT est une Fenêtre`) du
 * projet depuis l'index windowDefs (docs ouverts + compagnons disque). Filtre
 * les Etats (isWindow==false). Sert la completion de Projet..FenêtreDémarrage. */
static void emit_window_instances(JsonValue* items, const char* typeLabel) {
    for (int i = 0; i < windowDefCount; i++) {
        if (!windowDefs[i].isWindow) continue;
        emit_instance_item(items, windowDefs[i].name, typeLabel);
    }
}

/* Enregistre la declaration d'une Fenetre/Etat reperee au scan. `nameTok`
 * est l'IDENT du nom declare. docIndex/filePath suivent la convention de
 * scan_one_source_for_user_symbols (doc ouvert : docIndex>=0, filePath NULL ;
 * fichier disque : docIndex==-1, filePath renseigne). */
static void window_def_record(const Token* nameTok, int docIndex,
                              const char* filePath, bool isWindow) {
    if (windowDefCount >= 256) return;
    WindowDefInfo* w = &windowDefs[windowDefCount];
    int len = nameTok->length < 127 ? nameTok->length : 127;
    memcpy(w->name, nameTok->start, len);
    w->name[len] = '\0';
    w->line = nameTok->line - 1;
    w->character = nameTok->column > 0 ? nameTok->column - 1 : 0;
    w->docIndex = docIndex;
    w->isWindow = isWindow;
    w->fileUri[0] = '\0';
    if (docIndex < 0 && filePath && filePath[0] != '\0') {
        filepath_to_uri(filePath, w->fileUri, (int)sizeof(w->fileUri));
    }
    windowDefCount++;
}

/* Per-struct field registry: lets single-dot completion propose the
 * fields of a user-declared structure on `var.` when var's declared
 * type matches. */
typedef struct {
    char name[64];
    char type[64];
} UserStructField;
typedef struct {
    char name[128];
    UserStructField fields[64];
    int fieldCount;
    int docIndex;   /*  : doc source pour go-to-def, -1 si fichier disque */
    int line;       /*  : 0-based ligne de la declaration */
    int character;  /*  : 0-based colonne du nom */
} UserStructInfo;
static UserStructInfo userStructs[256];
static int userStructCount = 0;

static UserStructInfo* user_struct_lookup(const char* name) {
    for (int i = 0; i < userStructCount; i++) {
        if (strcasecmp(userStructs[i].name, name) == 0) return &userStructs[i];
    }
    return NULL;
}

/*  — Per-class field/method registry: lets ':' and 'obj.'
 * completion propose the members of a user-declared class. Members
 * carry a visibility flag so 'obj.' can filter to PUBLIC only while
 * ':' inside a method body sees the full set. */
typedef struct {
    char name[64];
    char type[64];      /* declared type for fields, "" for methods */
    char vis;           /* 'U' = public, 'R' = private, 'P' = protected */
    bool isMethod;      /* false = field, true = method (PROCEDURE/FONCTION) */
    int line;           /*  : 0-based ligne de la declaration du membre */
    int character;      /*  : 0-based colonne du nom du membre */
} UserClassMember;
typedef struct {
    char name[128];
    UserClassMember members[96];
    int memberCount;
    int docIndex;
    int classLine;      /* 0-based line of "cName est une Classe" */
    int classEndLine;   /* 0-based line of the closing FIN of the class */
    char baseClass[128];/*  : nom de la classe de base (Herite de X), "" si aucune */
    /*  — URI file:// du fichier disque source pour une classe captee
     * par le FS walk (docIndex == -1). Vide ("") pour une classe d'un doc
     * ouvert : dans ce cas la Location se construit via documents[docIndex].uri.
     * Permet le go-to-definition cross-fichier vers un .owl NON ouvert. */
    char fileUri[1024];
} UserClassInfo;
static UserClassInfo userClasses[64];
static int userClassCount = 0;

/*  — Cache fichier-level des classes du projet NON ouvertes.
 * Rempli UNE fois par le walk filesystem (gardé par g_projectScanDone),
 * puis merge dans userClasses[] a chaque scan_user_symbols_force. Permet
 * la coloration classe/methode cross-fichier sans re-walker le FS a chaque
 * frappe. Premier gagne en cas de collision projet/projet (un nom de
 * classe captee une fois n'est plus reecrite). */
static UserClassInfo g_projectClasses[64];
static int g_projectClassCount = 0;

static UserClassInfo* user_class_lookup(const char* name) {
    for (int i = 0; i < userClassCount; i++) {
        if (strcasecmp(userClasses[i].name, name) == 0) return &userClasses[i];
    }
    return NULL;
}

/*  — true si `name` est une methode (PROCEDURE/FONCTION) declaree
 * dans une classe user, toutes classes confondues. Sert a colorier les appels
 * obj.Methode() en vert (token semantique method).
 *
 *  — comparaison casse+accent insensible (invariant 6.2). Les noms
 * stockes (scan_class_body : memcpy depuis le token source) ET `name` (texte
 * brut du token d'appel) CONSERVENT leurs accents ; un strcasecmp nu ratait le
 * cas `Réinitialise` (decl) vs `Reinitialise` (appel), laissant la grammar
 * TextMate colorer bleu une methode homonyme d'une native accentuee
 * (MockNative.Réinitialise). On desaccentue les deux cotes avant compare. */
static bool user_class_method_exists(const char* name) {
    char nameA[128];
    scanner_strip_accents(name, (int)strlen(name), nameA, sizeof(nameA));
    for (int i = 0; i < userClassCount; i++) {
        UserClassInfo* ci = &userClasses[i];
        for (int j = 0; j < ci->memberCount; j++) {
            if (!ci->members[j].isMethod) continue;
            char membA[128];
            scanner_strip_accents(ci->members[j].name,
                                  (int)strlen(ci->members[j].name),
                                  membA, sizeof(membA));
            if (strcasecmp(membA, nameA) == 0) return true;
        }
    }
    return false;
}

/*  — Parse le corps d'une classe a partir de `sc`, positionne
 * JUSTE APRES le mot-cle CLASSE (le scanner a deja consomme l'eventuel
 * article un/une). `classNameTok` est l'IDENT du nom de classe (prev2 dans
 * le scan fichier, `tok` dans la boucle docs ouverts) ; `docIndex` identifie
 * le doc ouvert source, ou -1 pour une classe captee depuis un fichier projet
 * non ouvert (jamais matchee par user_class_enclosing, qui ne raisonne que
 * sur les docs ouverts). `filePath` est le chemin disque du fichier source
 * pour une classe captee par le FS walk (converti en out->fileUri), ou NULL
 * pour un doc ouvert. Remplit *out (deja alloue par le caller) avec
 * name/members/memberCount/docIndex/classLine/classEndLine/fileUri et laisse
 * `sc` positionne juste apres le FIN matchant de la classe.
 *
 * NB : ne touche PAS au scope-stack du caller. Le pop SCOPE_ISOLATE pousse a
 * l'ouverture de la Classe reste a la charge de la boucle principale (le scan
 * fichier projet n'a pas de scope-stack). */
static void scan_class_body(Scanner* sc, Token classNameTok, int docIndex,
                            const char* filePath, UserClassInfo* out) {
    int nlen = classNameTok.length < 127 ? classNameTok.length : 127;
    memcpy(out->name, classNameTok.start, nlen);
    out->name[nlen] = '\0';
    out->memberCount = 0;
    out->docIndex = docIndex;
    out->classLine = classNameTok.line - 1;
    out->classEndLine = -1;
    out->baseClass[0] = '\0';
    /*  — classe disque (docIndex<0) : on memorise l'URI du fichier
     * source pour que le go-to-definition cross-fichier puisse pointer dessus
     * sans que le fichier soit ouvert. Doc ouvert (filePath NULL) : reste vide,
     * la Location passe par documents[docIndex].uri. */
    out->fileUri[0] = '\0';
    if (filePath && filePath[0] != '\0') {
        filepath_to_uri(filePath, out->fileUri, (int)sizeof(out->fileUri));
    }
    char curVis = 'U'; /* default PUBLIC */
    int depth = 1;     /* on est entres dans le body class */
    Token ct;
    /*  — Heritage : `cX est une Classe Herite de cBase`. `sc` est
     * positionne juste apres CLASSE, donc le(s) premier(s) token(s) peuvent
     * etre HERITE DE <base>. On consomme ces tokens AVANT la boucle de corps
     * pour ne pas confondre le nom de base avec un champ, et on capte le nom
     * de base pour la remontee hierarchique du go-to-def. Le scanner n'a pas
     * de push-back, donc on memorise un eventuel token deja scanne (cas sans
     * heritage) dans `primed` pour le traiter au premier tour de boucle. */
    Token primed; bool havePrimed = false;
    {
        Token h = scanner_scan_token(sc);
        if (h.type == TOKEN_HERITE) {
            Token de = scanner_scan_token(sc);
            if (de.type == TOKEN_DE) {
                Token bt = scanner_scan_token(sc);
                if (bt.type == TOKEN_IDENTIFIER) {
                    int bl = bt.length < 127 ? bt.length : 127;
                    memcpy(out->baseClass, bt.start, bl);
                    out->baseClass[bl] = '\0';
                }
            }
        } else {
            primed = h; havePrimed = true; /* token de corps deja lu */
        }
    }
    for (;;) {
        if (havePrimed) { ct = primed; havePrimed = false; }
        else            { ct = scanner_scan_token(sc); }
        if (ct.type == TOKEN_EOF) break;
        if (ct.type == TOKEN_PUBLIC)  { curVis = 'U'; continue; }
        if (ct.type == TOKEN_PRIVE)   { curVis = 'R'; continue; }
        if (ct.type == TOKEN_PROTEGE) { curVis = 'P'; continue; }
        /* Nested PROCEDURE/FONCTION = methode : enregistrer puis descendre
         * dans son body jusqu'a son FIN. */
        if (ct.type == TOKEN_PROCEDURE || ct.type == TOKEN_FONCTION) {
            Token mn = scanner_scan_token(sc);
            /* Skip 'Constructeur'/'Destructeur' modifiers deja en
             * TOKEN_IDENTIFIER : tous canalisent ici. */
            if (mn.type == TOKEN_IDENTIFIER && out->memberCount < 96) {
                UserClassMember* m = &out->members[out->memberCount++];
                int ml = mn.length < 63 ? mn.length : 63;
                memcpy(m->name, mn.start, ml);
                m->name[ml] = '\0';
                m->type[0] = '\0';
                m->vis = curVis;
                m->isMethod = true;
                m->line = mn.line - 1;
                m->character = mn.column > 0 ? mn.column - 1 : 0;
            }
            /* Avance jusqu'au FIN matchant de la methode. On compte les
             * ouvertures de bloc pour ne pas prendre un FIN interne pour le
             * FIN de la methode.
             *
             *  — `SINON SI` (else-if, tokens SINON puis SI) REUTILISE
             * le FIN du SI ouvrant : il ne faut PAS compter ce SI comme un
             * nouveau bloc, sinon methDepth ne redescend jamais a 0 au vrai
             * FIN de la methode. La boucle sur-consommait alors le corps, le
             * FIN de la classe et les methodes suivantes jusqu'a EOF — les
             * membres declares apres une methode contenant un `SINON SI`
             * (ex. la derniere methode de la classe) etaient perdus, cassant
             * coloration + go-to-definition cross-fichier. */
            int methDepth = 1;
            Token mt;
            TokenType prevMt = TOKEN_EOF;
            for (;;) {
                mt = scanner_scan_token(sc);
                if (mt.type == TOKEN_EOF) break;
                bool isElseIf = (mt.type == TOKEN_SI && prevMt == TOKEN_SINON);
                if (isElseIf) {
                    /* rien : la chaine SI/SINON SI est fermee par un seul FIN */
                } else if (lsp_depth_table_knows(mt)) {
                    methDepth += lsp_depth_delta(mt);
                    if (methDepth == 0) break;
                } else if (mt.type == TOKEN_AVEC) {
                    methDepth++;
                }
                prevMt = mt.type;
            }
            if (mt.type == TOKEN_EOF) break;
            continue;
        }
        /* IDENTIFIER EST [UN/UNE] type → champ classe. */
        if (ct.type == TOKEN_IDENTIFIER) {
            Token est = scanner_scan_token(sc);
            if (est.type == TOKEN_EST && out->memberCount < 96) {
                Token tt = scanner_scan_token(sc);
                /*  — copule EN `is a/an <type>` : saute l'article
                 * pour que m->type capture le vrai type, pas "a"/"an". */
                if (tok_is_en_article(tt)) tt = scanner_scan_token(sc);
                UserClassMember* m = &out->members[out->memberCount++];
                int fl = ct.length < 63 ? ct.length : 63;
                memcpy(m->name, ct.start, fl);
                m->name[fl] = '\0';
                int tl = tt.length < 63 ? tt.length : 63;
                if (tt.type != TOKEN_EOF) {
                    memcpy(m->type, tt.start, tl);
                    m->type[tl] = '\0';
                } else {
                    m->type[0] = '\0';
                }
                m->vis = curVis;
                m->isMethod = false;
                m->line = ct.line - 1;
                m->character = ct.column > 0 ? ct.column - 1 : 0;
            }
            continue;
        }
        if (ct.type == TOKEN_FIN) {
            depth--;
            if (depth == 0) {
                out->classEndLine = ct.line - 1;
                break;
            }
        }
    }
}

/*  — Renvoie la classe dont le body englobe (docIdx, line),
 * ou NULL si le curseur n'est pas dans un body class. */
static UserClassInfo* user_class_enclosing(int docIdx, int line) {
    for (int i = 0; i < userClassCount; i++) {
        UserClassInfo* ci = &userClasses[i];
        if (ci->docIndex != docIdx) continue;
        if (line <= ci->classLine) continue;
        if (ci->classEndLine >= 0 && line > ci->classEndLine) continue;
        return ci;
    }
    return NULL;
}

/* Per-enum value registry: lets assignment completion propose the
 * values of a user-declared enum/combinaison when `var = ` where
 * var's type matches. */
typedef struct {
    char name[128];         /* "EEnum", "Options", etc. */
    char values[64][64];    /* ["enumTyp1", "enumTyp2", ...] */
    int valueLine[64];      /*  : 0-based ligne de chaque membre */
    int valueChar[64];      /*  : 0-based colonne de chaque membre */
    int valueCount;
    bool isCombination;     /* true if Combinaison, false if Enumeration */
    int docIndex;           /*  : doc source pour go-to-def, -1 si fichier disque */
    char fileUri[1024];     /*  : URI file:// si compagnon disque ferme (docIndex<0) */
    int line;               /*  : 0-based ligne de la declaration */
    int character;          /*  : 0-based colonne du nom */
    char ownerCollection[128]; /*  : nom du bloc CollectionDeProcedures
                                * englobant, "" si enum top-level. Sert a scoper
                                * un acces qualifie `COL_X.Enum.membre` a l'enum
                                * de CETTE collection quand deux collections
                                * declarent un enum homonyme. */
} UserEnumInfo;
static UserEnumInfo userEnums[256];
static int userEnumCount = 0;

static UserEnumInfo* user_enum_lookup(const char* name) {
    for (int i = 0; i < userEnumCount; i++) {
        if (strcasecmp(userEnums[i].name, name) == 0) return &userEnums[i];
    }
    return NULL;
}

/*  rev3 — Suivi du nom des Interfaces user (par nom seul, pas de
 * membre). Sert a colorier le nom d'interface en token 'type' (violet) en
 * declaration `iTruc est une Interface` ET usage `x est un iTruc`, comme les
 * enum/combinaison. Pas de capture de corps (les methodes d'interface ne
 * sont pas des appels resolvables comme les methodes de classe). */
typedef struct {
    char name[128];
    int docIndex;   /*  : doc source pour go-to-def, -1 si fichier disque */
    int line;       /*  : 0-based ligne de la declaration */
    int character;  /*  : 0-based colonne du nom */
} UserInterfaceInfo;
static UserInterfaceInfo userInterfaces[128];
static int userInterfaceCount = 0;

static bool user_interface_lookup(const char* name) {
    for (int i = 0; i < userInterfaceCount; i++) {
        if (strcasecmp(userInterfaces[i].name, name) == 0) return true;
    }
    return false;
}

/*  — predicat de compatibilite d'affectation cote LSP, miroir des
 * regles du compilateur. `value_type_assignable_to(candidate, lhs)` est vrai
 * si une valeur de type `candidate` peut alimenter un LHS de type `lhs` en
 * position valeur d'un decl-assign (`x est un <lhs> = <candidate>`).
 *
 * REGLE DE SURETE (impérative, cf. ticket §Risque) : on ne masque QUE le
 * PROUVE incompatible. Tout type non classe avec certitude (registry,
 * struct, date, type a coerce scalaire Couleur/Icone/UUID, ou inconnu)
 * retombe sur "proposer" (true). Un faux negatif frustre plus qu'un faux
 * positif. */
typedef enum {
    VT_OTHER = 0,   /* non classe : registry/struct/coerce/date/inconnu -> permissif */
    VT_BOOL,
    VT_NUM,         /* entier/reel/monetaire/numerique + enum (integer-backed) */
    VT_STR,         /* chaine <-> chaineMultilingue */
    VT_CLASS        /* classe user (heritage) */
} ValTypeCat;

static ValTypeCat value_type_category(const char* typeName) {
    if (!typeName || !*typeName) return VT_OTHER;
    char sb[128];
    scanner_strip_accents(typeName, (int)strlen(typeName), sb, sizeof(sb));

    if (strcasecmp(sb, "booleen") == 0 || strcasecmp(sb, "boolean") == 0)
        return VT_BOOL;

    if (strcasecmp(sb, "entier") == 0 || strcasecmp(sb, "reel") == 0
        || strcasecmp(sb, "monetaire") == 0 || strcasecmp(sb, "numerique") == 0
        || strcasecmp(sb, "integer") == 0 || strcasecmp(sb, "real") == 0
        || strcasecmp(sb, "currency") == 0 || strcasecmp(sb, "numeric") == 0)
        return VT_NUM;

    if (strcasecmp(sb, "chaine") == 0 || strcasecmp(sb, "chainemultilingue") == 0
        || strcasecmp(sb, "string") == 0 || strcasecmp(sb, "multilingualstring") == 0)
        return VT_STR;

    /* enum (registry ou user) : integer-backed -> compatible numerique */
    if (owl_enum_info_find(typeName) || user_enum_lookup(typeName))
        return VT_NUM;

    if (user_class_lookup(typeName))
        return VT_CLASS;

    return VT_OTHER;
}

/* Vrai si la classe `cand` est `target` ou en herite (walk de la chaine
 * baseClass, garde anti-cycle). */
static bool class_is_subtype_of(const char* cand, const char* target) {
    UserClassInfo* c = user_class_lookup(cand);
    int guard = 0;
    while (c && guard++ < 64) {
        if (strcasecmp(c->name, target) == 0) return true;
        if (c->baseClass[0] == '\0') break;
        c = user_class_lookup(c->baseClass);
    }
    return false;
}

/*  — un type VT_OTHER peut etre le variant `tout` (accepte tout,
 * donc assignable a n'importe quel LHS, y compris scalaire : GARDE, option A).
 * Le catalogue LSP stocke les return_type en forme FR canon ; on couvre aussi
 * la graphie EN/ascii par surete. */
static bool is_variant_type_name(const char* t) {
    if (!t || !*t) return false;
    char sb[64];
    scanner_strip_accents(t, (int)strlen(t), sb, sizeof(sb));
    return strcasecmp(sb, "tout") == 0
        || strcasecmp(sb, "variant") == 0
        || strcasecmp(sb, "all") == 0;
}

/*  — un type VT_OTHER est-il CONNU comme non-scalaire ? Sert au
 * filtrage agressif d'un LHS scalaire : on n'exclut un candidat VT_OTHER que
 * s'il est PROUVE non-scalaire (tableau, `rien`/void, primitif non-scalaire
 * comme date, type compose/registry Couleur/Icone..., struct user). Un nom
 * non resolu (typo, type inconnu) N'est PAS connu -> reste propose (surete).
 * Le variant `tout` est traite en amont (is_variant_type_name) : jamais ici. */
static bool is_known_nonscalar_type(const char* t) {
    if (!t || !*t) return false;
    char sb[64];
    scanner_strip_accents(t, (int)strlen(t), sb, sizeof(sb));
    if (strcasecmp(sb, "tableau") == 0 || strcasecmp(sb, "array") == 0) return true;
    if (strcasecmp(sb, "rien") == 0 || strcasecmp(sb, "void") == 0
        || strcasecmp(sb, "nothing") == 0) return true;
    /* date, buffer... sont des primitifs VT_OTHER (les scalaires ont deja ete
     * classes par value_type_category en amont). */
    if (owl_prim_type_is(t)) return true;
    if (owl_type_info_find(t)) return true;
    if (user_struct_lookup(t)) return true;
    return false;
}

static bool value_type_assignable_to(const char* candidateType, const char* lhsType) {
    /* Surete : cote inconnu -> proposer. */
    if (!candidateType || !*candidateType) return true;
    if (!lhsType || !*lhsType) return true;

    /* Identite (casse + accent insensible). */
    char candA[128], lhsA[128];
    scanner_strip_accents(candidateType, (int)strlen(candidateType), candA, sizeof(candA));
    scanner_strip_accents(lhsType, (int)strlen(lhsType), lhsA, sizeof(lhsA));
    if (strcasecmp(candA, lhsA) == 0) return true;

    ValTypeCat cc = value_type_category(candidateType);
    ValTypeCat lc = value_type_category(lhsType);

    /*  — LHS scalaire (BOOL/NUM/STR) : filtrage AGRESSIF. On distingue,
     * dans VT_OTHER, le variant `tout` et l'inconnu (a garder) du connu-non-
     * scalaire (a exclure). Hors LHS scalaire : comportement permissif inchange
     * (cf. plus bas), pour ne pas sur-filtrer classes/enums/types exotiques. */
    if (lc == VT_BOOL || lc == VT_NUM || lc == VT_STR) {
        if (cc == lc) return true;                 /* meme categorie scalaire */
        if (cc == VT_BOOL || cc == VT_NUM || cc == VT_STR) return false; /* autre scalaire */
        if (cc == VT_CLASS) return false;          /* classe face a scalaire */
        /* cc == VT_OTHER : variant/inconnu -> garder ; connu non-scalaire -> exclure. */
        if (is_variant_type_name(candidateType)) return true;
        if (is_known_nonscalar_type(candidateType)) return false;
        return true;                               /* non resolu -> surete : proposer */
    }

    /* Surete : au moins un cote non classe avec certitude -> proposer. */
    if (cc == VT_OTHER || lc == VT_OTHER) return true;

    /* Heritage objet : candidat assignable a un LHS classe s'il est la meme
     * classe ou un sous-type. Un scalaire/enum face a une classe (ou
     * l'inverse) est prouve incompatible. */
    if (lc == VT_CLASS || cc == VT_CLASS) {
        if (lc == VT_CLASS && cc == VT_CLASS)
            return class_is_subtype_of(candidateType, lhsType);
        return false;
    }

    /* Categories scalaires (BOOL/NUM/STR) : mutuellement exclusives. La
     * hierarchie numerique (entier/reel/monetaire/numerique) et la coercion
     * chaine<->chaineMultilingue sont deja repliees dans une meme categorie ;
     * enum est traite comme NUM (integer-backed). */
    return cc == lc;
}

/* Vrai si AU MOINS une signature de la native retourne un type assignable a
 * `lhsType`. Surete : si aucune info de retour exploitable -> proposer. */
static bool native_return_assignable_to(const NativeInfo* fn, const char* lhsType) {
    if (!fn || fn->signatureCount <= 0) return true;
    ValTypeCat lc = value_type_category(lhsType);
    bool lhsScalar = (lc == VT_BOOL || lc == VT_NUM || lc == VT_STR);
    bool anyKnown = false;
    for (int s = 0; s < fn->signatureCount; s++) {
        const char* rt = fn->signatures[s].returnType;
        if (!rt || !*rt) continue;
        anyKnown = true;
        if (value_type_assignable_to(rt, lhsType)) return true;
    }
    /*  — aucun retour connu (native sans retour / return_type vide) :
     * pour un LHS scalaire, une native sans valeur exploitable est PROUVEE
     * inutile -> exclure. Hors scalaire, on reste permissif (proposer). */
    if (!anyKnown) return !lhsScalar;
    return false; /* tous les retours connus sont prouves incompatibles */
}

/*  — true si `name` est un type compose builtin (Police, Cadre,
 * Style, Coin, Trait...) declare dans le catalogue auto-genere. Sert a la
 * coloration contextuelle des semantic tokens : un type compose utilise comme
 * PROP dans un bloc PROPRIETES (`Police = ...` / `Police..Nom = ...`) doit
 * etre colore property (vert), pas type (violet). Casse-insensible. */
static bool is_composed_type_name(const char* name) {
    /*  — EN-aware sans elargir l'ensemble : le catalogue compose ne
     * porte que la forme FR (typeName). On resout d'abord `name` vers son nom
     * canon FR via owl_type_info_find (matche FR/ascii/EN), puis on teste
     * l'appartenance sur ce canon. Sans ca, `X est un Font` (EN de Police)
     * n'etait pas colore type/violet alors que `X est un Police` l'etait. */
    const OwlTypeInfo* ti = owl_type_info_find(name);
    const char* canon = ti ? ti->name : name;
    for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
        if (strcasecmp(OWL_COMPOSED_TYPE_CATALOG[k].typeName, canon) == 0)
            return true;
    }
    return false;
}

/*  — true si `name` est un type de sous-bloc d'Etat canon (DSL V2 :
 * EnTêteDocument / HautDePage / Corps / BasDePage / FinDeDocument / Itération).
 * Le LSP ne link pas `etat_decl.c` (cf. CMakeLists section owl-lsp) donc on
 * duplique la liste ici — meme source de verite que `owl_etat_block_kind_from_name`.
 * Sert a 2 endroits :
 *   - scan : capturer `INTRO est un EnTêteDocument` comme sub-bloc kind='E'.
 *   - classification semantic tokens : colorer le TYPE lui-meme en violet
 *     (tokenType=4) car le scanner le voit comme IDENT, pas comme keyword
 *     reserve. Sans ca le type reste neutre (defaut grammar TextMate, qui
 *     ne reconnait pas non plus ces noms).
 *  — accent-insensible : depuis l'accentuation du canon FR
 * (EnTêteDocument / Itération), la comparaison ASCII stricte rejetait
 * `k->nameFR` de CONTROL_KINDS et faisait disparaitre l'entree entiere des
 * boucles filtrees par ce predicat — y compris sa forme anglaise. Meme patron
 * que is_menu_block_type_name ci-dessous. */
static bool is_etat_block_type_name(const char* name) {
    char norm[64];
    int n = scanner_strip_accents(name, (int)strlen(name), norm, sizeof(norm));
    if (n <= 0) {
        n = (int)strlen(name);
        if (n >= (int)sizeof(norm)) n = (int)sizeof(norm) - 1;
        memcpy(norm, name, (size_t)n);
    }
    norm[n] = '\0';
    /* Formes FR + EN canon (cf. CONTROL_KINDS dans ui_decl.c), en graphie
     * ASCII puisque `norm` est deja depouille de ses accents. */
    return strcasecmp(norm, "EnTeteDocument") == 0
        || strcasecmp(norm, "DocumentHeader") == 0
        || strcasecmp(norm, "HautDePage")     == 0
        || strcasecmp(norm, "PageHeader")     == 0
        || strcasecmp(norm, "Corps")          == 0
        || strcasecmp(norm, "Body")           == 0
        || strcasecmp(norm, "BasDePage")      == 0
        || strcasecmp(norm, "PageFooter")     == 0
        || strcasecmp(norm, "FinDeDocument")  == 0
        || strcasecmp(norm, "DocumentFooter") == 0
        || strcasecmp(norm, "Iteration")      == 0;
}

/*  — true si le nom (forme ecrite) designe un genre d'element de menu :
 * MenuPrincipal / SousMenu / OptionMenu / MenuSeparateur (CONTROL_KINDS,
 * is_widget=false cote runtime) + MenuContextuel (, hors table). Ces
 * elements ne sont pas des widgets posables mais sont colores comme des
 * elements de widget (meme couleur que BTN_X/SAI_X). Accent-insensible : le
 * nom entrant peut porter l'accent de MenuSeparateur. */
static bool is_menu_block_type_name(const char* name) {
    char norm[64];
    int n = scanner_strip_accents(name, (int)strlen(name), norm, sizeof(norm));
    if (n <= 0) {
        n = (int)strlen(name);
        if (n >= (int)sizeof(norm)) n = (int)sizeof(norm) - 1;
        memcpy(norm, name, (size_t)n);
    }
    norm[n] = '\0';
    return strcasecmp(norm, "MenuPrincipal")  == 0
        || strcasecmp(norm, "MainMenu")       == 0
        || strcasecmp(norm, "SousMenu")       == 0
        || strcasecmp(norm, "Submenu")        == 0
        || strcasecmp(norm, "OptionMenu")     == 0
        || strcasecmp(norm, "MenuOption")     == 0
        || strcasecmp(norm, "MenuSeparateur") == 0
        || strcasecmp(norm, "MenuSeparator")  == 0
        || strcasecmp(norm, "MenuContextuel") == 0
        || strcasecmp(norm, "ContextMenu")    == 0;
}

/*  — true si le nom (forme ecrite) designe un sous-bloc Volet d'un
 * Champ Onglet. `Volet`/`Pane` sont is_widget=false cote runtime (sous-bloc
 * structurel, pas de prefixe `Champ`), donc owl_control_kind_from_token les
 * rejette : detection par nom canon (cf. CONTROL_KINDS dans ui_decl.c). Le
 * Volet est un point de recursion facon Conteneur : il porte des champs
 * enfants et son nom est colore comme un element de widget. Accent-insensible
 * par symetrie avec is_menu_block_type_name (Volet/Pane sont ASCII, mais on
 * garde la meme robustesse). */
static bool is_volet_block_type_name(const char* name) {
    char norm[64];
    int n = scanner_strip_accents(name, (int)strlen(name), norm, sizeof(norm));
    if (n <= 0 || n >= (int)sizeof(norm)) {
        n = (int)strlen(name);
        if (n >= (int)sizeof(norm)) n = (int)sizeof(norm) - 1;
        memcpy(norm, name, (size_t)n);
    }
    norm[n] = '\0';
    return strcasecmp(norm, "Volet") == 0
        || strcasecmp(norm, "Pane")  == 0;
}

/*  — char de contexte DSL d'un bloc menu, sinon 0. Les blocs menu
 * sont is_widget=false (owl_control_kind_from_token les rejette), donc pas de
 * char via la table widget : mapping local. 'M' = MenuPrincipal (root du
 * `Style` typé styleMenuPrincipal), 'x' = MenuContextuel (styleMenuContextuel).
 * 'u'/'o'/'r' = SousMenu/OptionMenu/MenuSeparateur : purement structurels
 * (équilibrent les FIN et scopent leurs propres PROPRIETES), pas de prop
 * `Style`. Accent-insensible (MenuSéparateur). */
static char menu_block_kind_char(const char* start, int len) {
    if (!start || len <= 0) return 0;
    char norm[64];
    int n = scanner_strip_accents(start, len, norm, sizeof(norm));
    if (n <= 0 || n >= (int)sizeof(norm)) return 0;
    norm[n] = '\0';
    if (strcasecmp(norm, "MenuPrincipal")  == 0 || strcasecmp(norm, "MainMenu")      == 0) return 'M';
    if (strcasecmp(norm, "MenuContextuel") == 0 || strcasecmp(norm, "ContextMenu")   == 0) return 'x';
    if (strcasecmp(norm, "SousMenu")       == 0 || strcasecmp(norm, "Submenu")       == 0) return 'u';
    if (strcasecmp(norm, "OptionMenu")     == 0 || strcasecmp(norm, "MenuOption")    == 0) return 'o';
    if (strcasecmp(norm, "MenuSeparateur") == 0 || strcasecmp(norm, "MenuSeparator") == 0) return 'r';
    return 0;
}

/*  — type de style typé rooté par le char de contexte d'un bloc menu.
 * 'M' → styleMenuPrincipal, 'x' → styleMenuContextuel. NULL hors bloc menu
 * porteur d'un `Style` (les sous-blocs u/o/r n'ont pas de prop Style propre). */
static const char* menu_kind_style_type(char k) {
    if (k == 'M') return "styleMenuPrincipal";
    if (k == 'x') return "styleMenuContextuel";
    return NULL;
}

/*  — true si le token designe le type conteneur `Projet`/`Project`.
 * `Projet` n'a pas de token scanner dedie (type compose vu comme IDENTIFIER),
 * donc on compare le texte du token. Pas d'accent sur Projet -> strncasecmp
 * suffit. Sert a colorer le NOM d'une declaration `X est un Projet` comme un
 * nom de Fenetre/Etat (userWindows -> tokenType=1, class, vert fonce). */
static bool is_projet_type_token(const Token* t) {
    return (t->length == 6 && strncasecmp(t->start, "Projet",  6) == 0)
        || (t->length == 7 && strncasecmp(t->start, "Project", 7) == 0);
}

/*  — types du DSL de service HTTP declares comme IDENT (pas de token
 * scanner dedie), reconnus par texte casse+accent-insensible. `ServiceREST`
 * est language-neutral (une seule forme) ; `OpérationHTTP`/`HTTPOperation`
 * portent la paire FR⇄EN (cf. formatter.c CAT_DECL, compiler.c service_ident_is).
 * Sert a colorer le nom declare (`X est un ServiceREST`, `Y est une
 * OpérationHTTP`) en class (vert fonce) comme Fenetre/Etat/Projet/Collection,
 * sans en faire une cible navigable d'Ouvre (skip window_def, cf. Projet). */
static bool is_service_type_token(const Token* t) {
    if (t->length <= 0 || t->length >= 64) return false;
    char probe[64];
    int n = scanner_strip_accents(t->start, t->length, probe, sizeof(probe));
    if (n <= 0 || n >= (int)sizeof(probe)) return false;
    probe[n] = '\0';
    return strcasecmp(probe, "ServiceREST") == 0
        || strcasecmp(probe, "OperationHTTP") == 0
        || strcasecmp(probe, "HTTPOperation") == 0;
}

/*  — char de contexte DSL du bloc ouvert par un type de service HTTP
 * declare comme IDENT : 's' pour `ServiceREST`, 'h' pour
 * `OpérationHTTP`/`HTTPOperation`, 0 sinon. Casse+accent insensible (le scanner
 * ne donne pas de token dedie a ces types). Sert a empiler le bon contexte dans
 * detect_dsl_context_ex / find_dsl_block_start pour router la completion et le
 * bornage des events vers les catalogues service/operation. */
static char service_block_kind_char(const char* start, int length) {
    if (!start || length <= 0 || length >= 64) return 0;
    char probe[64];
    int n = scanner_strip_accents(start, length, probe, sizeof(probe));
    if (n <= 0 || n >= (int)sizeof(probe)) return 0;
    probe[n] = '\0';
    if (strcasecmp(probe, "ServiceREST") == 0) return 's';
    if (strcasecmp(probe, "OperationHTTP") == 0
     || strcasecmp(probe, "HTTPOperation") == 0) return 'h';
    return 0;
}

/*  — true si le token designe le type conteneur `Thème`/`Theme`.
 * Comme `Projet`, `Thème` est un opener de bloc vu comme IDENTIFIER (pas de
 * token scanner dedie), donc on compare le texte du token. Contrairement a
 * `Projet` (ASCII), `Thème` porte un accent (é = 2 octets UTF-8) : on passe
 * par scanner_strip_accents pour un compare casse+accent-insensible
 * (`Thème`/`Theme`/`thème`/`theme` matchent tous, cf. is_theme_receiver_name).
 * Sert a colorer le TYPE d'une declaration `X est un Thème` en type (violet),
 * aligne sur le traitement de `Projet`. */
static bool is_theme_type_token(const Token* t) {
    if (t->length <= 0 || t->length >= 32) return false;
    char ascii[32];
    scanner_strip_accents(t->start, t->length, ascii, sizeof(ascii));
    return strcasecmp(ascii, "theme") == 0;
}

/* DSL Fenetre v1 : noms declarees dans le code user pour la coloration
 * contextuelle ET l'autocomplete des props via 'ctrl..'.
 *
 * userControls[] stocke aussi le kind ('L'/'B'/'S') pour que la completion
 * 'SAI_User..' propose les props du kind correct (Saisie ici, pas Date). */
/*  — isWindow distingue `est une Fenêtre` (true) de `est un État`
 * (false, ou nom de Projet). Sert a filtrer les Etats au 1er arg des natives
 * ouvre-fenetre (Ouvre/OuvreFille/OuvreSoeur) : un Etat s'ouvre via ImprimeEtat,
 * pas Ouvre. Hors ce contexte, Fenetres ET Etats restent proposes. */
typedef struct { char name[128]; bool isWindow; } UserDslName;
typedef struct { char name[128]; char kind; /* 'L'/'B'/'S' */ } UserDslControl;
static UserDslName    userWindows[256];
static int            userWindowCount = 0;
static UserDslControl userControls[1024];
static int            userControlCount = 0;
/*  — Groupes de champs declares ('GR_X est un Groupe').
 * Stocke aussi le compte de membres detectes au scan pour le hover. */
typedef struct { char name[128]; int memberCount; } UserDslGroup;
static UserDslGroup   userGroups[256];
static int            userGroupCount = 0;
/*  — colonnes declarees dans un Champ Table (`COL_X est une
 * Colonne`). Elles ne sont pas des controles (pas dans userControls), donc
 * rien ne les proposait apres `TABLE_X.` ni `TABLE_X[1].` alors que le nom
 * de colonne y est la seule suggestion sensee. On retient le Table
 * proprietaire, sinon les colonnes d'une autre Table du meme fichier
 * fuiteraient dans la liste. */
typedef struct { char table[128]; char name[128]; } UserDslColumn;
static UserDslColumn  userColumns[1024];
static int            userColumnCount = 0;

static void scan_project_dsl_names(void);

/*  — forme colonne-first `TABLE_X.COL_A[1]..`. Le walk des crochets
 * de la completion s'arrete sur le nom de colonne (`start` pointe son premier
 * caractere) ; les colonnes ne sont pas enregistrees comme controles, donc le
 * receveur ne resout rien. On remonte le segment precedent et on verifie qu'il
 * s'agit d'un Champ Table declare. Exiger cette racine evite de proposer les
 * proprietes de cellule sur `COL_A[1]..` seul, que le compilateur refuse. */
static bool table_colfirst_root_is_table(const char* lp, int start) {
    int q = start;
    while (q > 0 && lp[q - 1] == '.') q--;
    if (q == start) return false;          /* pas de segment avant */
    int r = q;
    while (r > 0 && ((lp[r-1] >= 'a' && lp[r-1] <= 'z') ||
                     (lp[r-1] >= 'A' && lp[r-1] <= 'Z') ||
                     (lp[r-1] >= '0' && lp[r-1] <= '9') ||
                     lp[r-1] == '_' || (unsigned char)lp[r-1] >= 0x80))
        r--;
    if (r >= q || q - r >= 128) return false;
    char rootName[128];
    memcpy(rootName, lp + r, (size_t)(q - r));
    rootName[q - r] = '\0';
    for (int i = 0; i < userControlCount; i++) {
        if (userControls[i].kind == 'T'
            && strcasecmp(rootName, userControls[i].name) == 0)
            return true;
    }
    return false;
}

/* Cache de scan : evite de re-tokeniser tous les documents a chaque
 * completion/hover si rien n'a change. On utilise les pointeurs de
 * contenu + tailles : a chaque didChange/setup_doc le contenu est
 * realloc/strdup, donc le pointeur change. Pas de risque de collision
 * de hash. */
static uint64_t g_lastScanFingerprint = 0;
static int      g_lastScanDocCount    = -1;

/*  — Forward decls : compute_doc_fingerprint hashe le workspace via
 * hash_workspace_files, scan_user_symbols_force ratisse le workspace via
 * scan_workspace_files_for_user_symbols. Le flag `recursive` ( —
 * doctrine SCRIPT/ENTRYPOINT/PROJECT) borne le walk au dossier seul quand
 * `false` ; les sous-dossiers sont alors ignores. Utilise par les deux
 * callers via collect_open_doc_scopes (par-document, plus de walk global
 * inconditionnel depuis lspServer.rootPath). */
static uint64_t hash_workspace_files(const char* dir, int depth,
                                     bool recursive, uint64_t fp);
static void scan_workspace_files_for_user_symbols(const char* dir, int depth,
                                                  bool recursive);
static void scan_one_source_for_user_symbols(const char* source, int docIndex,
                                             const char* filePath);

/* Resout les scopes a hasher/scanner pour les buffers ouverts via
 * resolve_project_root :
 *  - SCRIPT     : dossier du fichier, non recursif
 *  - ENTRYPOINT : dossier du fichier, non recursif
 *  - PROJECT    : dossier ancetre porteur du projet.owl, recursif
 * Bornage : un scope hors de lspServer.rootPath est ignore (cohabite avec
 * handle_formatting/publish_compiler_diagnostics : on ne walk jamais
 * au-dessus du workspace VSCode). Dedup sur chemin exact.
 * Retourne le nombre de scopes ecrits dans `outRoots`/`outRecursive`. */
#define LSP_SCAN_SCOPES_MAX 16
static int collect_open_doc_scopes(char outRoots[][1024],
                                   bool outRecursive[],
                                   int maxScopes);

static int collect_open_doc_scopes(char outRoots[][1024],
                                   bool outRecursive[],
                                   int maxScopes) {
    int count = 0;
    for (int d = 0; d < lspServer.documentCount; d++) {
        /*  — un doc de scheme != file:// (ex owl-bloc://, vue isolee)
         * n'a pas de fichier disque : uri_to_filepath rendrait un chemin
         * aberrant (l'URI brute) que resolve_project_root walkerait. On le
         * traite comme un script sans scope disque -- son contexte voisins/
         * globales vient du scan cross-file des docs file:// ouverts. */
        if (strncmp(lspServer.documents[d].uri, "file://", 7) != 0) continue;
        char filepath[1024];
        uri_to_filepath(lspServer.documents[d].uri, filepath, sizeof(filepath));
        char root[1024];
        ProjectMode mode = resolve_project_root(filepath, root, sizeof(root));
        if (lspServer.rootPath[0] != '\0' &&
            !fmt_path_within(root, lspServer.rootPath)) {
            continue;  /* hors workspace VSCode : ignore (idem handle_formatting) */
        }
        bool seen = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(outRoots[i], root) == 0) { seen = true; break; }
        }
        if (!seen && count < maxScopes) {
            snprintf(outRoots[count], 1024, "%s", root);
            outRecursive[count] = (mode == PROJECT_MODE_PROJECT);
            count++;
        }
    }
    return count;
}

static uint64_t compute_doc_fingerprint(void) {
    uint64_t fp = (uint64_t)lspServer.documentCount * 0x9E3779B97F4A7C15ULL;
    for (int d = 0; d < lspServer.documentCount; d++) {
        const char* c = lspServer.documents[d].content;
        int len = lspServer.documents[d].contentLen;
        fp ^= (uint64_t)len * 0xC2B2AE3D27D4EB4FULL;
        fp ^= (uint64_t)lspServer.documents[d].version << 32;
        /* Hash 64 premiers + 64 derniers + 64 du milieu : robuste contre
         * reuse d'adresse heap (strdup peut renvoyer le meme pointeur
         * qu'un buffer just-free) tout en restant O(1) par doc. */
        if (c && len > 0) {
            int n = len < 64 ? len : 64;
            for (int i = 0; i < n; i++) {
                fp = ((fp << 5) ^ (fp >> 27)) ^ (uint64_t)(unsigned char)c[i];
            }
            if (len > 64) {
                int from = len - 64;
                for (int i = 0; i < 64; i++) {
                    fp = ((fp << 5) ^ (fp >> 27)) ^ (uint64_t)(unsigned char)c[from + i];
                }
            }
            if (len > 192) {
                int mid = len / 2 - 32;
                for (int i = 0; i < 64; i++) {
                    fp = ((fp << 5) ^ (fp >> 27)) ^ (uint64_t)(unsigned char)c[mid + i];
                }
            }
        }
        fp = (fp ^ (fp >> 27)) * 0x94D049BB133111EBULL;
    }
    /* fix : empreinte par scope au lieu d'un walk recursif
     * inconditionnel depuis rootPath. Pour chaque buffer ouvert on resout
     * son scope SCRIPT/ENTRYPOINT/PROJECT et on hashe les .owl correspondants
     * (stat() seulement, pas de lecture). Un fichier-script isole ne fait
     * stat que son dossier voisin (non recursif) -> O(taille dossier),
     * pas O(taille workspace). */
    char roots[LSP_SCAN_SCOPES_MAX][1024];
    bool recursive[LSP_SCAN_SCOPES_MAX];
    int nScopes = collect_open_doc_scopes(roots, recursive, LSP_SCAN_SCOPES_MAX);
    for (int i = 0; i < nScopes; i++) {
        fp = hash_workspace_files(roots[i], 0, recursive[i], fp);
    }
    return fp;
}

static void scan_user_symbols_force(void);

/*  — un buffer ouvert dont le chemin ne finit pas par .owl (typiquement
 * un fichier renomme en .owlNOT pour l'exclure du projet mais reste ouvert dans
 * un onglet) ne doit pas injecter ses symboles dans les tables partagees qui
 * alimentent la completion cross-documents. Suffixe strict, casse-insensible :
 * `.owl`/`.OWL` acceptes, `.owlNOT`/`.txt` exclus (pas un strstr qui matcherait
 * `.owl` au milieu de `.owlNOT`). Les schemes virtuels non file:// (owl-bloc://,
 * vue isolee d'un parent .owl — ) restent des contextes OWL legitimes
 * et contribuent. */
static bool doc_uri_is_owl_source(const char* uri) {
    if (!uri) return false;
    if (strncmp(uri, "file://", 7) != 0) return true;
    size_t n = strlen(uri);
    return n >= 4 && strcasecmp(uri + n - 4, ".owl") == 0;
}

static void scan_user_symbols(void) {
    /* Skip si rien n'a change (cache hit). */
    uint64_t fp = compute_doc_fingerprint();
    if (g_lastScanDocCount == lspServer.documentCount && fp == g_lastScanFingerprint) {
        return;
    }
    g_lastScanFingerprint = fp;
    g_lastScanDocCount = lspServer.documentCount;
    scan_user_symbols_force();
}

static void scan_user_symbols_force(void) {
    userSymbolCount = 0;
    diskGlobalCount = 0;
    windowDefCount = 0;
    userCollectionCount = 0;  /*  */
    diskProcCount = 0;        /*  */
    userStructCount = 0;
    userEnumCount = 0;
    userInterfaceCount = 0;
    userClassCount = 0;
    userWindowCount = 0;
    userControlCount = 0;
    userGroupCount = 0;
    userColumnCount = 0;

    for (int d = 0; d < lspServer.documentCount; d++) {
        /*  — skip les buffers ouverts non-.owl (ex .owlNOT) : leurs
         * symboles ne doivent pas polluer la completion cross-documents. */
        if (!doc_uri_is_owl_source(lspServer.documents[d].uri)) continue;
        scan_one_source_for_user_symbols(lspServer.documents[d].content, d, NULL);
    }

    /*  — Scan workspace disque : types/classes/structs/enums/
     * interfaces declares dans des .owl du rootPath mais non ouverts dans
     * l'editeur. Priorite buffer > disque : les fichiers deja ouverts sont
     * skippes (la version buffer prime, peut etre non sauvee).
     *
     * Scope par buffer au lieu d'un walk inconditionnel depuis
     * rootPath : un fichier-script isole declenche un scan local non
     * recursif (~10 ms), un fichier sous projet.owl declenche le scan
     * recursif depuis projet.owl. Aligne sur handle_formatting et
     * publish_compiler_diagnostics qui resolvent deja par scope. */
    char roots[LSP_SCAN_SCOPES_MAX][1024];
    bool recursive[LSP_SCAN_SCOPES_MAX];
    int nScopes = collect_open_doc_scopes(roots, recursive, LSP_SCAN_SCOPES_MAX);
    for (int i = 0; i < nScopes; i++) {
        scan_workspace_files_for_user_symbols(roots[i], 0, recursive[i]);
    }

    /* Cross-file : scan les .owl du rootPath qui ne sont pas dans les
     * documents ouverts, pour exposer les noms FEN_/ETAT_ aux fichiers
     * du meme projet.
     *
     *  — Lazy au lieu d'eager : le walk filesystem coute ~1.5s
     * (mesure dev 2026-06-09, log timing [scan_user_symbols: 1484ms]).
     * On NE LE FAIT PLUS au 1er didOpen pour debloquer le rendu initial.
     * Le scan est triggered a la 1ere requete qui en a besoin
     * (handle_completion / handle_definition cross-file), via le helper
     * lsp_ensure_project_scan(). Tradeoff : la coloration semantique
     * cross-file (references FEN_X declarees dans un autre file) peut
     * etre absente avant la 1ere completion. Cas dominant (declarations
     * locales) inchange.
     *
     * IMPORTANT historique : ce scan walk le filesystem complet (~11s
     * sur le repo OWL, 1000+ .owl files). On ne peut PAS l'appeler a
     * chaque didChange. */

    /*  — Merge des classes du cache projet dans userClasses[].
     * userClasses[] a deja ete reset+rempli pour les docs ouverts ci-dessus ;
     * le walk FS (gardé par g_projectScanDone) a rempli g_projectClasses[]
     * une fois. On copie ici a CHAQUE scan car userClassCount=0 au debut de
     * la fonction. Docs ouverts priment : on skip toute classe du cache dont
     * le nom est deja present (sa version en memoire, potentiellement non
     * sauvee, fait foi). Cout = copie de ≤64 structs → negligeable. */
    for (int i = 0; i < g_projectClassCount && userClassCount < 64; i++) {
        if (user_class_lookup(g_projectClasses[i].name) != NULL) continue;
        userClasses[userClassCount++] = g_projectClasses[i];
    }

    lsp_log("Scanned %d user symbols across %d documents",
            userSymbolCount, lspServer.documentCount);

    /*  — reconstruit l'arbre de portees + la table de bindings pour
     * le go-to-definition scope-correct. Ancre ici : gratuit vis a vis du
     * cache fingerprint (scan_user_symbols ne force le rescan que si un buffer
     * a change),  §2. */
    scope_build();
}

/*  — copie `src` (len octets) dans `dst` en reduisant toute
 * sequence de blancs (espace, tab, CR, LF) a un espace unique et en
 * elaguant les bords. Preserve les octets non-ASCII (UTF-8 des types
 * accentues : `chaine`/`chaîne`). */
static void proc_sig_normalize_ws(const char* src, size_t len,
                                  char* dst, size_t dstSize) {
    size_t o = 0;
    bool pendingSpace = false, started = false;
    for (size_t i = 0; i < len && o + 1 < dstSize; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (started) pendingSpace = true;
            continue;
        }
        if (pendingSpace && o + 1 < dstSize) { dst[o++] = ' '; pendingSpace = false; }
        if (o + 1 < dstSize) { dst[o++] = (char)c; started = true; }
    }
    dst[o] = '\0';
}

/*  — construit la signature (liste de parametres + type de retour)
 * d'une PROCEDURE/FONCTION pour le `detail` de hover/completion. Lit en
 * AVANCE via un clone du scanner (`peek` passe par valeur) positionne juste
 * apres le nom : le scanner principal n'est PAS avance, donc la suite du
 * scan (notamment la capture des parametres comme declarations locales)
 * reste strictement inchangee. Format :
 *   "Procedure(x est un entier, y est une chaine)"
 *   "Fonction(a est un entier) : entier"
 * Borne a `outSize` ; ellipse ASCII si depassement (cas rare). */
static void build_proc_signature(Scanner peek, bool isFunction,
                                 char* out, size_t outSize) {
    const char* kindWord = isFunction ? "Fonction" : "Procedure";
    char params[256]; params[0] = '\0';
    char ret[128];    ret[0] = '\0';

    Token t = scanner_scan_token(&peek);
    if (t.type == TOKEN_LEFT_PAREN) {
        const char* paramStart = t.start + t.length; /* premier octet apres '(' */
        const char* paramEnd = NULL;
        int depth = 1;
        for (;;) {
            Token p = scanner_scan_token(&peek);
            if (p.type == TOKEN_EOF) { paramEnd = p.start; break; }
            if (p.type == TOKEN_LEFT_PAREN) depth++;
            else if (p.type == TOKEN_RIGHT_PAREN) {
                if (--depth == 0) { paramEnd = p.start; break; }
            }
        }
        if (paramEnd && paramEnd >= paramStart) {
            proc_sig_normalize_ws(paramStart, (size_t)(paramEnd - paramStart),
                                  params, sizeof(params));
        }
        /* Type de retour `: Type` (meme ligne, apres la parenthese fermante). */
        if (isFunction) {
            Token c = scanner_scan_token(&peek);
            if (c.type == TOKEN_COLON) {
                const char* rStart = NULL;
                const char* rEnd = NULL;
                for (;;) {
                    Token r = scanner_scan_token(&peek);
                    if (r.type == TOKEN_NEWLINE || r.type == TOKEN_EOF) break;
                    if (!rStart) rStart = r.start;
                    rEnd = r.start + r.length;
                }
                if (rStart && rEnd && rEnd > rStart) {
                    proc_sig_normalize_ws(rStart, (size_t)(rEnd - rStart),
                                          ret, sizeof(ret));
                }
            }
        }
    }

    int written = (ret[0] != '\0')
        ? snprintf(out, outSize, "%s(%s) : %s", kindWord, params, ret)
        : snprintf(out, outSize, "%s(%s)", kindWord, params);
    if (written < 0 || (size_t)written >= outSize) {
        if (outSize >= 4) {
            out[outSize - 4] = '.';
            out[outSize - 3] = '.';
            out[outSize - 2] = '.';
            out[outSize - 1] = '\0';
        }
    }
}

/*  — Body du scan d'une source (open doc ou fichier disque).
 * docIndex >= 0 : doc ouvert dans lspServer.documents[docIndex].
 * docIndex == -1 : fichier disque (pas d'entree dans userSymbols pour la
 * resolution go-to-definition qui indexe lspServer.documents directement).
 * Les containers cross-file (userClasses, userStructs, userEnums,
 * userInterfaces, userWindows, userControls, userGroups) acceptent les
 * deux sources. `filePath` est le chemin disque du fichier source quand
 * docIndex == -1 (sert a renseigner UserClassInfo.fileUri pour le
 * go-to-definition cross-fichier), NULL pour un doc ouvert. */
static void scan_one_source_for_user_symbols(const char* source, int docIndex,
                                             const char* filePath) {
    {
        Scanner sc;
        scanner_init(&sc, source);

        /* DSL Phase G v2 : syntaxe 'IDENT est un/une Fenetre/Libelle/...
         * On garde une mini-state machine sur les 3 derniers tokens. */
        Token prev2 = (Token){0};
        Token prev1 = (Token){0};

        /* Stack de scopes pour determiner isGlobal sur chaque declaration.
         *  - SCOPE_BLOCK   = SI/POUR/TANTQUE/SELON/BOUCLE/AVEC + blocs DSL
         *                    transparents (Fenetre/Etat/Projet, PROPRIETES) :
         *                    transparent pour la globalite (vars dedans =
         *                    globales si pas dans une proc/type/widget).
         *  - SCOPE_ISOLATE = PROCEDURE/FONCTION/CLASSE/STRUCTURE/etc. ET les
         *                    blocs widget (`IDENT est un Champ ...`, Libelle,
         *                    Colonne, sous-blocs Etat, ...) : les declarations
         *                    dedans ne sont PAS globales.
         *
         *  — equilibre du scope-stack sur les blocs DSL termines par
         * FIN. Chaque bloc DSL traverse par la boucle principale (Fenetre,
         * Etat, PROPRIETES, widget, Colonne, sous-bloc Etat) pousse un scope a
         * l'ouverture ; son FIN (vu par la boucle) le depile. Sans cet
         * equilibre, le FIN d'un PROPRIETES interne a un widget depilait
         * prematurement le scope du widget (les vars suivantes du widget
         * redevenaient globales). Les blocs auto-consommes (Structure/Classe/
         * Enumeration/... dont le corps + FIN est lu par un sous-scanner) ne
         * poussent RIEN ici : ils sont deja equilibres (0 push / 0 pop). */
        enum { SCOPE_BLOCK = 0, SCOPE_ISOLATE = 1 };
        signed char scopeStack[64];
        int scopeTop = 0;
        /*  — Champ Table dont le corps est en cours de traversee ;
         * proprietaire des blocs Colonne rencontres ensuite. */
        char currentTableName[128] = "";
        /*  — mode de portee top-level courant (bascule GLOBAL/LOCAL
         * seule sur une ligne). En LOCAL, une variable top-level est confinee :
         * elle N'est PAS globale -> token variable sans modifier `global` (vert
         * #4CAF50 au lieu de cyan). Reset a GLOBAL sur chaque FIN (sortie de
         * contexte). Ignore dans une classe (GLOBAL/GLOBALE y sont des
         * membres/methodes statiques : IS_TOPLEVEL_GLOBAL() y est faux). */
        bool confineMode = false;
        /*  — collection englobante courante (pour attribuer un enum
         * declare dans un bloc CollectionDeProcedures a sa collection). Set a
         * l'entree du bloc (scopeTop == curCollectionDepth), efface quand le
         * FIN ramene scopeTop a ce niveau. Le mot-cle de type d'enum
         * (Enumeration) est consomme via `sc` dans la detection, pas par le
         * scope-tracking outer, donc scopeTop reste equilibre. */
        char curCollection[128] = "";
        int curCollectionDepth = -1;
        #define IS_TOPLEVEL_GLOBAL() ({ \
            bool _g = true; \
            for (int _i = 0; _i < scopeTop; _i++) { \
                if (scopeStack[_i] == SCOPE_ISOLATE) { _g = false; break; } \
            } \
            _g; \
        })

        for (;;) {
            Token tok = scanner_scan_token(&sc);
            if (tok.type == TOKEN_EOF) break;

            /* Scope tracking : ouvertures isolantes / transparentes / fermeture */
            if (tok.type == TOKEN_PROCEDURE || tok.type == TOKEN_FONCTION ||
                tok.type == TOKEN_CLASSE    || tok.type == TOKEN_STRUCTURE ||
                tok.type == TOKEN_ENUMERATION || tok.type == TOKEN_COMBINAISON ||
                tok.type == TOKEN_INTERFACE) {
                if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_ISOLATE;
            } else if (tok.type == TOKEN_AVEC) {
                if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_BLOCK;
            } else if (lsp_depth_table_knows(tok) && tok.type != TOKEN_FIN
                       && tok.type != TOKEN_PROPRIETES) {
                /*  — SI / CONSTANTE ne poussent que sous leur forme
                 * bloc : la source unique tranche. Un push parasite laissait
                 * le scope ouvert jusqu'a EOF. */
                if (lsp_depth_delta(tok) > 0 && scopeTop < 64)
                    scopeStack[scopeTop++] = SCOPE_BLOCK;
            } else if (tok.type == TOKEN_PROPRIETES) {
                /*  — bloc PROPRIETES : transparent pour la globalite
                 * mais DOIT pousser pour que son FIN n'ampute pas le scope du
                 * widget/fenetre englobant. */
                if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_BLOCK;
            } else if (tok.type == TOKEN_FIN) {
                if (scopeTop > 0) scopeTop--;
                /*  — le FIN qui ferme le bloc collection ramene scopeTop
                 * a la profondeur d'entree : la collection courante n'englobe
                 * plus les declarations suivantes. */
                if (curCollectionDepth >= 0 && scopeTop <= curCollectionDepth) {
                    curCollection[0] = '\0';
                    curCollectionDepth = -1;
                }
                confineMode = false;  /*  : sortie de contexte -> GLOBAL */
            } else if ((tok.type == TOKEN_GLOBAL || tok.type == TOKEN_LOCAL)
                       && IS_TOPLEVEL_GLOBAL()) {
                /*  — bascule de portee hors classe (au top-level d'un
                 * script / element / collection). Dans une classe, GLOBAL/GLOBALE
                 * sont des membres statiques et IS_TOPLEVEL_GLOBAL() est faux. */
                confineMode = (tok.type == TOKEN_LOCAL);
            }

            /* Pattern : prev2 = IDENT, prev1 = EST, tok = Fenetre/Etat/...
             * Le nom declare est prev2. */
            if ((tok.type == TOKEN_FENETRE || tok.type == TOKEN_ETAT)
                && prev1.type == TOKEN_EST
                && prev2.type == TOKEN_IDENTIFIER
                && userWindowCount < 256) {
                int n = prev2.length < 127 ? prev2.length : 127;
                memcpy(userWindows[userWindowCount].name, prev2.start, n);
                userWindows[userWindowCount].name[n] = '\0';
                userWindows[userWindowCount].isWindow = (tok.type == TOKEN_FENETRE);
                userWindowCount++;
                /*  — localisation pour le go-to-definition. */
                window_def_record(&prev2, docIndex, filePath,
                                  tok.type == TOKEN_FENETRE);
            } else if ((tok.type == TOKEN_LIBELLE || tok.type == TOKEN_BOUTON
                        || tok.type == TOKEN_SAISIE)
                       && prev1.type == TOKEN_EST
                       && prev2.type == TOKEN_IDENTIFIER
                       && userControlCount < 1024) {
                int n = prev2.length < 127 ? prev2.length : 127;
                memcpy(userControls[userControlCount].name, prev2.start, n);
                userControls[userControlCount].name[n] = '\0';
                userControls[userControlCount].kind =
                      tok.type == TOKEN_LIBELLE ? 'L'
                    : tok.type == TOKEN_BOUTON  ? 'B' : 'S';
                userControlCount++;
            } else if (tok.type == TOKEN_IDENTIFIER
                       && prev1.type == TOKEN_EST
                       && prev2.type == TOKEN_IDENTIFIER
                       && userControlCount < 1024) {
                /* DSL Etat V2 : 'IDENT est un EnTeteDocument/HautDePage/Corps/...'
                 * Le 'tok' est ici un IDENTIFIER (pas un keyword reserve).
                 * Match les 6 types de blocs canon Etat. */
                int klen = tok.length;
                if (klen > 0 && klen < 32) {
                    char kindBuf[32];
                    memcpy(kindBuf, tok.start, klen);
                    kindBuf[klen] = '\0';
                    bool isEtatBlockType = is_etat_block_type_name(kindBuf);
                    if (isEtatBlockType) {
                        int n = prev2.length < 127 ? prev2.length : 127;
                        memcpy(userControls[userControlCount].name, prev2.start, n);
                        userControls[userControlCount].name[n] = '\0';
                        userControls[userControlCount].kind = 'E';  /* Etat block */
                        userControlCount++;
                    }
                    /*  — 'IDENT est un Groupe [= [...]]'.
                     * Capture le nom et compte les membres '['..']' qui
                     * suivent (best-effort scan ; le hover utilise ce
                     * compte). */
                    bool isGroupeKind = (strcasecmp(kindBuf, "Groupe") == 0) ||
                                        (strcasecmp(kindBuf, "Group") == 0);
                    if (isGroupeKind && userGroupCount < 256) {
                        int n = prev2.length < 127 ? prev2.length : 127;
                        memcpy(userGroups[userGroupCount].name, prev2.start, n);
                        userGroups[userGroupCount].name[n] = '\0';
                        /* Compte de membres : scan en avant jusqu'a ']' si
                         * '=' [' suit. Sinon Groupe vide (count=0). */
                        int memberCount = 0;
                        Scanner peek = sc;
                        Token t = scanner_scan_token(&peek);
                        if (t.type == TOKEN_EQUAL) {
                            t = scanner_scan_token(&peek);
                            if (t.type == TOKEN_LEFT_BRACKET) {
                                /* Compte les identifiers separes par
                                 * virgules jusqu'a ']'. */
                                for (;;) {
                                    Token m = scanner_scan_token(&peek);
                                    if (m.type == TOKEN_RIGHT_BRACKET ||
                                        m.type == TOKEN_EOF) break;
                                    if (m.type == TOKEN_IDENTIFIER) {
                                        memberCount++;
                                    }
                                    /* Skip jusqu'au prochain ',' ou ']' */
                                    while (m.type != TOKEN_COMMA &&
                                           m.type != TOKEN_RIGHT_BRACKET &&
                                           m.type != TOKEN_EOF) {
                                        Token next = scanner_scan_token(&peek);
                                        if (next.type == TOKEN_EOF ||
                                            next.type == TOKEN_RIGHT_BRACKET) {
                                            m = next;
                                            break;
                                        }
                                        m = next;
                                    }
                                    if (m.type == TOKEN_RIGHT_BRACKET ||
                                        m.type == TOKEN_EOF) break;
                                }
                            }
                        }
                        userGroups[userGroupCount].memberCount = memberCount;
                        userGroupCount++;
                    }
                }
            }

            /*  — article EN transparent : `IDENT is a <Type>` laisse
             * prev1=EST en place pour que <Type> matche l'adjacence. */
            if (tok_is_en_article(tok) && prev1.type == TOKEN_EST) {
                continue;
            }

            prev2 = prev1;
            prev1 = tok;
            (void)prev2; (void)prev1; /* suppress unused warning */
            /* Reset prev2/prev1 si tok est statement-boundary, pour ne pas
             * matcher accidentellement a travers des newlines. Mais comme
             * advance_token consume les NEWLINE silencieusement, on ne
             * voit jamais TOKEN_NEWLINE ici → c'est OK pour l'instant. */

            /* PROCEDURE / FONCTION name */
            /* CONSTANTE block — register each declared name as a Constant
             * symbol so it shows up in autocomplete. */
            if (tok.type == TOKEN_CONSTANTE) {
                for (;;) {
                    Token nameTok = scanner_scan_token(&sc);
                    if (nameTok.type == TOKEN_EOF || nameTok.type == TOKEN_FIN) break;
                    if (nameTok.type != TOKEN_IDENTIFIER) continue;
                    /* Skip = <value> */
                    Token eq = scanner_scan_token(&sc);
                    if (eq.type == TOKEN_EQUAL) {
                        scanner_scan_token(&sc); /* the value */
                    }
                    if (docIndex >= 0 && userSymbolCount < 1024) {
                        UserSymbol* sym = &userSymbols[userSymbolCount++];
                        int n = nameTok.length < 127 ? nameTok.length : 127;
                        memcpy(sym->name, nameTok.start, n);
                        sym->name[n] = '\0';
                        sym->line = nameTok.line - 1;
                        sym->character = nameTok.column > 0 ? nameTok.column - 1 : 0;
                        sym->kind = 21; /* LSP Constant */
                        snprintf(sym->detail, sizeof(sym->detail), "Constante");
                        sym->docIndex = docIndex;
                    }
                }
            }

            if (tok.type == TOKEN_PROCEDURE || tok.type == TOKEN_FONCTION) {
                /* Skip visibility modifiers */
                Token next = scanner_scan_token(&sc);
                while (next.type == TOKEN_PUBLIC || next.type == TOKEN_PRIVE ||
                       next.type == TOKEN_PROTEGE || next.type == TOKEN_GLOBALE) {
                    next = scanner_scan_token(&sc);
                }
                if (next.type == TOKEN_IDENTIFIER && docIndex >= 0 && userSymbolCount < 1024) {
                    UserSymbol* sym = &userSymbols[userSymbolCount++];
                    int len = next.length < 127 ? next.length : 127;
                    memcpy(sym->name, next.start, len);
                    sym->name[len] = '\0';
                    sym->line = next.line - 1;
                    sym->character = next.column > 0 ? next.column - 1 : 0;
                    sym->kind = 3; /* Function */
                    /*  — signature (params + type de retour) dans le
                     * detail : le hover montre `Procedure Nom(...)` plutot que
                     * le seul genre. Lecture en avance sur un clone du scanner,
                     * `sc` n'est pas avance (params toujours scannes ensuite
                     * comme declarations locales — zero regression). */
                    build_proc_signature(sc, tok.type == TOKEN_FONCTION,
                                         sym->detail, sizeof(sym->detail));
                    sym->docIndex = docIndex;
                }
                else if (next.type == TOKEN_IDENTIFIER && docIndex < 0
                         && filePath && filePath[0] != '\0' && diskProcCount < 256) {
                    /*  — procedure/fonction d'un compagnon FERME :
                     * userSymbols n'indexe que les docs ouverts. On memorise
                     * nom + fileUri + position pour colorer l'appel `Nom(...)`
                     * en method (vert) et resoudre le go-to-def cross-fichier
                     * d'emblee, sans dependre de l'ouverture du compagnon. */
                    char pname[128];
                    int len = next.length < 127 ? next.length : 127;
                    memcpy(pname, next.start, len);
                    pname[len] = '\0';
                    if (disk_proc_lookup(pname) == NULL) {
                        DiskProcInfo* dp = &diskProcs[diskProcCount++];
                        memcpy(dp->name, pname, (size_t)len + 1);
                        dp->line = next.line - 1;
                        dp->character = next.column > 0 ? next.column - 1 : 0;
                        filepath_to_uri(filePath, dp->fileUri, (int)sizeof(dp->fileUri));
                    }
                }
            }

            /* identifier EST ... -> detect type declarations and variable declarations */
            if (tok.type == TOKEN_IDENTIFIER) {
                Token maybe_est = scanner_scan_token(&sc);
                if (maybe_est.type == TOKEN_EST) {
                    Token typeToken = scanner_scan_token(&sc);
                    int kind = 0;
                    const char* detail = NULL;

                    /*  — copule EN `is a/an <type>` : saute l'article
                     * sinon typeToken reste sur "a"/"an" et la detection de
                     * widget/type echoue en EN. */
                    if (tok_is_en_article(typeToken)) {
                        typeToken = scanner_scan_token(&sc);
                    }

                    /* Modificateur de champ optionnel : consomme, puis le
                     * filler `de`/`of` qui peut le suivre — le token suivant
                     * est le vrai genre. Ici la copule vient d'etre lue, donc
                     * le predicat "prev == EST" du helper est vrai par
                     * construction. Cf. lsp_champ_prefix_kind. */
                    bool sawChampPrefix = false;
                    if (lsp_champ_prefix_kind(typeToken, true)
                        == LSP_CHAMP_PREFIX_MODIFIER) {
                        Token afterChamp = scanner_scan_token(&sc);
                        if (lsp_champ_prefix_kind(afterChamp, true)
                            == LSP_CHAMP_PREFIX_FILLER) {
                            afterChamp = scanner_scan_token(&sc);
                        }
                        typeToken = afterChamp;
                        sawChampPrefix = true;
                    }

                    /* Detection unifiee : IDENT est-il un widget kind canon ?
                     * Source unique : owl_control_kind_from_token (CONTROL_KINDS).
                     * On extrait l'OwlControlKind si oui ; les keywords scanner
                     * (TOKEN_LIBELLE, TOKEN_BOUTON, etc.) restent traites
                     * separement.
                     *
                     *  — Image/Liste sont ambigus (widget vs type
                     * collection/runtime). Desambiguation par peek du token
                     * suivant via clone de Scanner :
                     *   - prefixe `Champ` (sawChampPrefix=true)        → widget
                     *   - peek bloc `PROPRIETES`                       → widget
                     *   - peek `de`/`of` (collection)                  → pas widget
                     *   - autre cas (variable simple, EOF, ...)        → pas widget
                     */
                    OwlControlKind tokenKind = (OwlControlKind)-1;
                    bool tokenIsWidget = false;
                    if (typeToken.type == TOKEN_IDENTIFIER) {
                        tokenIsWidget = owl_control_kind_from_token(
                            typeToken.start, typeToken.length, &tokenKind, NULL);
                    }
                    bool isImageType = tokenIsWidget && tokenKind == OWL_CONTROL_IMAGE;
                    bool isListeType = (typeToken.type == TOKEN_LISTE);
                    bool sawProprietesPeek = false;
                    if ((isImageType || isListeType) && !sawChampPrefix) {
                        Scanner peek = sc;
                        /* Skip les TOKEN_NEWLINE significatifs entre la fin
                         * de la ligne `est une Liste/Image` et le bloc
                         * `PROPRIETES` qui suit (typique : indent puis
                         * PROPRIETES sur la ligne suivante). */
                        Token p;
                        do { p = scanner_scan_token(&peek); }
                        while (p.type == TOKEN_NEWLINE);
                        if (p.type == TOKEN_PROPRIETES) {
                            sawProprietesPeek = true;
                        }
                        /* p.type == TOKEN_DE → collection ; tout autre token
                         * (autre statement, EOF, '='...) → variable simple.
                         * Dans ces deux cas, sawProprietesPeek reste false. */
                    }
                    bool isImageKind = isImageType
                        && (sawChampPrefix || sawProprietesPeek);
                    bool isListeKind = isListeType
                        && (sawChampPrefix || sawProprietesPeek);
                    bool isComboKind        = tokenIsWidget && tokenKind == OWL_CONTROL_COMBO;
                    bool isSelecteurKind    = tokenIsWidget && tokenKind == OWL_CONTROL_SELECTEUR;
                    bool isInterrupteurKind = tokenIsWidget && tokenKind == OWL_CONTROL_INTERRUPTEUR;
                    bool isZrKind           = tokenIsWidget && tokenKind == OWL_CONTROL_ZONE_REPETEE;
                    bool isJaugeKind        = tokenIsWidget && tokenKind == OWL_CONTROL_JAUGE;
                    /*  — Champ Onglet : widget (is_widget=true) reconnu
                     * par owl_control_kind_from_token. Sans cette detection,
                     * `ONG_X est un Champ Onglet` retombait dans le filet
                     * sawChampPrefix (L~1930) qui pousse un scope mais N'enregistre
                     * PAS le nom -> ONG_X colore variable (vert clair) au lieu de
                     * property (vert fonce) comme tout autre champ. */
                    bool isOngletKind       = tokenIsWidget && tokenKind == OWL_CONTROL_ONGLET;
                    bool typeIsProjet = (typeToken.type == TOKEN_IDENTIFIER)
                                        && is_projet_type_token(&typeToken);
                    /*  — `X est un ServiceREST` / `Y est une
                     * OpérationHTTP` : type IDENT, nom colore class (comme
                     * Projet), bloc transparent ferme par FIN. Pas une cible
                     * d'Ouvre -> skip window_def_record (cf. typeIsProjet). */
                    bool typeIsService = (typeToken.type == TOKEN_IDENTIFIER)
                                         && is_service_type_token(&typeToken);
                    /*  — `IDENT est une CollectionDeProcedures` : le type
                     * est un IDENT (pas de token dedie). On memorise le nom de
                     * collection (tok) pour le colorer class (vert fonce) comme
                     * un nom de Fenetre/Etat, et le resoudre en go-to-def. Le
                     * bloc est transparent pour la globalite (ses globales
                     * restent globales) mais pousse un SCOPE_BLOCK pour equilibrer
                     * son FIN, comme Fenetre/Etat. */
                    bool typeIsCollection = (typeToken.type == TOKEN_IDENTIFIER)
                                            && lsp_is_collection_kw(typeToken.start,
                                                                    typeToken.length);
                    if (typeIsCollection) {
                        if (userCollectionCount < 256) {
                            char cname[128];
                            int cn = tok.length < 127 ? tok.length : 127;
                            memcpy(cname, tok.start, cn);
                            cname[cn] = '\0';
                            if (collection_def_lookup(cname) == NULL) {
                                CollectionDefInfo* c = &userCollections[userCollectionCount++];
                                memcpy(c->name, cname, (size_t)cn + 1);
                                c->line = tok.line - 1;
                                c->character = tok.column > 0 ? tok.column - 1 : 0;
                                c->docIndex = docIndex;
                                c->fileUri[0] = '\0';
                                if (docIndex < 0 && filePath && filePath[0] != '\0')
                                    filepath_to_uri(filePath, c->fileUri,
                                                    (int)sizeof(c->fileUri));
                            }
                        }
                        /*  — marque la collection englobante courante
                         * (avant le push : scopeTop == niveau de la collection).
                         * Les enums declares dans son corps herisent ce nom. */
                        {
                            int cn = tok.length < 127 ? tok.length : 127;
                            memcpy(curCollection, tok.start, cn);
                            curCollection[cn] = '\0';
                            curCollectionDepth = scopeTop;
                        }
                        if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_BLOCK;
                    } else if ((typeToken.type == TOKEN_FENETRE || typeToken.type == TOKEN_ETAT
                         || typeIsProjet || typeIsService)
                        && userWindowCount < 256) {
                        /*  — TOKEN_ETAT (`ETAT_X est un Etat`) capture
                         * symetrique a TOKEN_FENETRE : nom top-level declare
                         * comme objet nomme du DSL (couleur class, vert fonce
                         * dans le theme). Sans TOKEN_ETAT ici, ETAT_Mensuel
                         * restait non scanne donc non colore.
                         *  — `X est un Projet/Project` : `Projet` est un
                         * IDENTIFIER (pas de token dedie), reconnu par texte. Le
                         * nom rejoint userWindows -> meme couleur class que
                         * Fenetre/Etat. La copule EN (`is a Project`) a deja
                         * saute l'article, donc typeToken pointe sur le type. */
                        int n = tok.length < 127 ? tok.length : 127;
                        memcpy(userWindows[userWindowCount].name, tok.start, n);
                        userWindows[userWindowCount].name[n] = '\0';
                        /*  — seule une vraie Fenetre est une cible Ouvre.
                         * Etat + Projet -> isWindow=false (exclus de Ouvre arg 0). */
                        userWindows[userWindowCount].isWindow =
                            (typeToken.type == TOKEN_FENETRE);
                        userWindowCount++;
                        /*  — localisation pour le go-to-definition,
                         * limitee aux Fenetres/Etats (une ref de Projet ne
                         * navigue pas vers une declaration de fenetre). */
                        if (!typeIsProjet && !typeIsService) {
                            window_def_record(&tok, docIndex, filePath,
                                              typeToken.type == TOKEN_FENETRE);
                        }
                        /*  — bloc Fenetre/Etat/Projet : transparent
                         * (ses vars top-level restent globales) mais pousse pour
                         * equilibrer son FIN de fermeture. */
                        if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_BLOCK;
                    } else if ((typeToken.type == TOKEN_LIBELLE
                             || typeToken.type == TOKEN_BOUTON
                             || typeToken.type == TOKEN_SAISIE
                             || typeToken.type == TOKEN_TABLE
                             || typeToken.type == TOKEN_CONTENEUR
                             || isImageKind
                             || isListeKind
                             || isComboKind
                             || isSelecteurKind
                             || isInterrupteurKind
                             || isZrKind
                             || isJaugeKind
                             || isOngletKind)
                               && userControlCount < 1024) {
                        int n = tok.length < 127 ? tok.length : 127;
                        memcpy(userControls[userControlCount].name, tok.start, n);
                        userControls[userControlCount].name[n] = '\0';
                        userControls[userControlCount].kind =
                              typeToken.type == TOKEN_LIBELLE   ? 'L'
                            : typeToken.type == TOKEN_BOUTON    ? 'B'
                            : typeToken.type == TOKEN_SAISIE    ? 'S'
                            : typeToken.type == TOKEN_TABLE     ? 'T'
                            : typeToken.type == TOKEN_CONTENEUR ? 'C'
                            : isImageKind                       ? 'I'
                            : isListeKind                       ? 'Z'
                            : isComboKind                       ? 'K'
                            : isSelecteurKind                   ? 'R'
                            : isInterrupteurKind                ? 'Q'
                            : isZrKind                          ? 'Y'
                            : isJaugeKind                       ? 'J'
                            : isOngletKind                      ? 'N'
                            : '?';
                        userControlCount++;
                        /*  — memorise le dernier Champ Table ouvert :
                         * c'est le proprietaire des blocs Colonne qui suivent
                         * (le scan est lineaire, les colonnes vivent dans le
                         * corps du Table). */
                        if (typeToken.type == TOKEN_TABLE) {
                            snprintf(currentTableName, sizeof(currentTableName),
                                     "%s", userControls[userControlCount - 1].name);
                        } else {
                            currentTableName[0] = '\0';  /* on a quitte le Table */
                        }
                        /*  — bloc widget : isolant pour la globalite
                         * (les vars declarees directement dedans sont LOCALES au
                         * widget). Le corps + FIN est traverse par la boucle. */
                        if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_ISOLATE;
                    } else if (typeToken.type == TOKEN_COLONNE
                               && scopeTop < 64) {
                        /*  — enregistre la colonne sous son Table. */
                        if (currentTableName[0] != '\0'
                            && userColumnCount < 1024) {
                            int n = tok.length < 127 ? tok.length : 127;
                            memcpy(userColumns[userColumnCount].name, tok.start, n);
                            userColumns[userColumnCount].name[n] = '\0';
                            snprintf(userColumns[userColumnCount].table,
                                     sizeof(userColumns[userColumnCount].table),
                                     "%s", currentTableName);
                            userColumnCount++;
                        }
                        /*  — bloc Colonne (`COL_X est une Colonne` dans
                         * un Champ Table) : bloc DSL termine par FIN, traverse
                         * par la boucle. Il faut pousser pour equilibrer son FIN,
                         * sinon celui-ci depile prematurement le scope du Table
                         * englobant. Isolant (une var de colonne = locale). */
                        scopeStack[scopeTop++] = SCOPE_ISOLATE;
                    } else if (typeToken.type == TOKEN_IDENTIFIER
                               && userControlCount < 1024) {
                        /*  — sub-blocs Etat (DSL V2) :
                         *   INTRO est un EnTeteDocument  ...  FIN
                         *   CORPS est un Corps           ...  FIN
                         * Le type est un IDENT (pas un keyword scanner), donc
                         * le bloc precedent ne le voit pas. On le reconnait
                         * par nom canon (cf. is_etat_block_type_name). Stocke
                         * en kind='E' pour que la classif tokens emette
                         * tokenType=1 (class, meme couleur que ETAT_X parent). */
                        int tlen = typeToken.length < 31 ? typeToken.length : 31;
                        char typeBuf[32];
                        memcpy(typeBuf, typeToken.start, tlen);
                        typeBuf[tlen] = '\0';
                        if (is_etat_block_type_name(typeBuf)) {
                            int n = tok.length < 127 ? tok.length : 127;
                            memcpy(userControls[userControlCount].name, tok.start, n);
                            userControls[userControlCount].name[n] = '\0';
                            userControls[userControlCount].kind = 'E';
                            userControlCount++;
                            /*  — sous-bloc Etat (EnTeteDocument, Corps,
                             * ...) : bloc DSL termine par FIN, traverse par la
                             * boucle. Isolant (var de bande = locale) + equilibre
                             * du FIN. */
                            if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_ISOLATE;
                        } else if (is_menu_block_type_name(typeBuf)) {
                            /*  — element de menu (MNU_/SM_/OPT_/SEP_) :
                             * colore comme un element de widget (kind 'M' ->
                             * tokenType 2), pas comme une variable. Bloc DSL
                             * termine par FIN, isolant + equilibre du FIN comme
                             * un widget. */
                            int n = tok.length < 127 ? tok.length : 127;
                            memcpy(userControls[userControlCount].name, tok.start, n);
                            userControls[userControlCount].name[n] = '\0';
                            userControls[userControlCount].kind = 'M';
                            userControlCount++;
                            if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_ISOLATE;
                        } else if (is_volet_block_type_name(typeBuf)) {
                            /*  +  — sous-bloc Volet d'un Champ
                             * Onglet (`VOL_X est un Volet` / `is a Pane`) :
                             * is_widget=false cote runtime (pas de prefixe
                             * `Champ`), donc reconnu par nom canon.
                             * Le NOM declare est enregistre en kind 'V'
                             * (OWL_CONTROL_VOLET, cf. DSL_KIND_CHARS) -> token
                             * property comme tout nom de champ. Le TYPE `Volet`
                             * lui reste hors userControls (couleur type). Le
                             * push de scope isolant reste indispensable : sans
                             * lui, le FIN du Volet depile le scope du Champ
                             * Onglet englobant (desequilibre en cascade sur la
                             * suite du fichier). Les champs enfants du Volet
                             * sont enregistres naturellement par la boucle
                             * (leur propre `est un Champ <Genre>`). */
                            int n = tok.length < 127 ? tok.length : 127;
                            memcpy(userControls[userControlCount].name, tok.start, n);
                            userControls[userControlCount].name[n] = '\0';
                            userControls[userControlCount].kind = 'V';
                            userControlCount++;
                            if (scopeTop < 64) scopeStack[scopeTop++] = SCOPE_ISOLATE;
                        } else if (sawChampPrefix && scopeTop < 64) {
                            /*  — filet : `IDENT est un Champ <Genre>` ou
                             * le genre n'est pas (encore) un kind reconnu ci-dessus
                             * (widget exotique). Le prefixe `Champ`/`Field`/`Control`
                             * garantit un bloc widget termine par FIN. On pousse un
                             * scope isolant pour equilibrer et localiser ses vars. */
                            scopeStack[scopeTop++] = SCOPE_ISOLATE;
                        }
                    }

                    /* Composite type declarations */
                    if (typeToken.type == TOKEN_STRUCTURE)   {
                        kind = 22; detail = "Structure";
                        /* Capture field list for single-dot completion. */
                        if (userStructCount < 256) {
                            UserStructInfo* si = &userStructs[userStructCount++];
                            int nlen = tok.length < 127 ? tok.length : 127;
                            memcpy(si->name, tok.start, nlen);
                            si->name[nlen] = '\0';
                            si->fieldCount = 0;
                            si->docIndex = docIndex;            /*  */
                            si->line = tok.line - 1;            /*  */
                            si->character = tok.column > 0 ? tok.column - 1 : 0;
                            for (;;) {
                                Token ft = scanner_scan_token(&sc);
                                if (ft.type == TOKEN_EOF || ft.type == TOKEN_FIN) break;
                                if (ft.type != TOKEN_IDENTIFIER) continue;
                                Token est = scanner_scan_token(&sc);
                                if (est.type != TOKEN_EST) continue;
                                Token tt = scanner_scan_token(&sc);
                                if (si->fieldCount >= 64) {
                                    /* Drain to FIN. */
                                    while (tt.type != TOKEN_EOF && tt.type != TOKEN_FIN)
                                        tt = scanner_scan_token(&sc);
                                    break;
                                }
                                UserStructField* f = &si->fields[si->fieldCount++];
                                int fl = ft.length < 63 ? ft.length : 63;
                                memcpy(f->name, ft.start, fl);
                                f->name[fl] = '\0';
                                int tl = tt.length < 63 ? tt.length : 63;
                                if (tt.type != TOKEN_EOF) {
                                    memcpy(f->type, tt.start, tl);
                                    f->type[tl] = '\0';
                                } else {
                                    f->type[0] = '\0';
                                }
                            }
                            /* sc consumed FIN already; control returns. */
                        }
                    }
                    else if (typeToken.type == TOKEN_CLASSE)  {
                        kind = 5;  detail = "Classe";
                        /*  — Scan body class : champs + methodes avec
                         * tracking visibility. Parsing factorise dans
                         * scan_class_body (partage avec le scan projet
                         * cross-fichier). On consomme tokens jusqu'au FIN
                         * matchant. */
                        if (userClassCount < 64) {
                            scan_class_body(&sc, tok, docIndex, filePath, &userClasses[userClassCount++]);
                            /* sc consomme le FIN ; on pop le SCOPE_ISOLATE
                             * pousse par la boucle principale a l'ouverture de
                             * la Classe — sans ca le scope-stack reste
                             * desequilibre pour la suite du fichier. */
                            if (scopeTop > 0) scopeTop--;
                        }
                    }
                    else if (typeToken.type == TOKEN_ENUMERATION || typeToken.type == TOKEN_COMBINAISON) {
                        kind = 13;
                        detail = (typeToken.type == TOKEN_ENUMERATION) ? "Enumeration" : "Combinaison";
                        /* Capture enum/combination values for assignment completion. */
                        if (userEnumCount < 256) {
                            UserEnumInfo* ei = &userEnums[userEnumCount++];
                            int nlen = tok.length < 127 ? tok.length : 127;
                            memcpy(ei->name, tok.start, nlen);
                            ei->name[nlen] = '\0';
                            ei->valueCount = 0;
                            ei->isCombination = (typeToken.type == TOKEN_COMBINAISON);
                            ei->docIndex = docIndex;            /*  */
                            ei->fileUri[0] = '\0';              /*  */
                            if (docIndex < 0 && filePath && filePath[0] != '\0')
                                filepath_to_uri(filePath, ei->fileUri, (int)sizeof(ei->fileUri));
                            ei->line = tok.line - 1;            /*  */
                            ei->character = tok.column > 0 ? tok.column - 1 : 0;
                            /*  — collection englobante (ou "" si
                             * top-level). Disambigue les enums homonymes. */
                            {
                                size_t ocn = strlen(curCollection);
                                if (ocn >= sizeof(ei->ownerCollection))
                                    ocn = sizeof(ei->ownerCollection) - 1;
                                memcpy(ei->ownerCollection, curCollection, ocn);
                                ei->ownerCollection[ocn] = '\0';
                            }
                            for (;;) {
                                Token vt = scanner_scan_token(&sc);
                                if (vt.type == TOKEN_EOF || vt.type == TOKEN_FIN) break;
                                /* Enum values are identifiers; skip everything else (like EQUAL for explicit values) */
                                if (vt.type != TOKEN_IDENTIFIER) continue;
                                /*  — memorise la position du membre AVANT
                                 * de consommer le peek EQUAL (le go-to-def d'un
                                 * membre d'enum saute ici). */
                                int vLine = vt.line - 1;
                                int vChar = vt.column > 0 ? vt.column - 1 : 0;
                                /* Check if next token is EQUAL (explicit value assignment) */
                                Token peek = scanner_scan_token(&sc);
                                if (peek.type == TOKEN_EQUAL) {
                                    /* Skip the explicit value expression (assume single token for simplicity) */
                                    scanner_scan_token(&sc);
                                }
                                /* Store the value name */
                                if (ei->valueCount >= 64) {
                                    /* Drain to FIN. */
                                    while (peek.type != TOKEN_EOF && peek.type != TOKEN_FIN)
                                        peek = scanner_scan_token(&sc);
                                    break;
                                }
                                int vlen = vt.length < 63 ? vt.length : 63;
                                memcpy(ei->values[ei->valueCount], vt.start, vlen);
                                ei->values[ei->valueCount][vlen] = '\0';
                                ei->valueLine[ei->valueCount] = vLine;
                                ei->valueChar[ei->valueCount] = vChar;
                                ei->valueCount++;
                            }
                            /* sc consumed FIN already; control returns. */
                        }
                    }
                    else if (typeToken.type == TOKEN_INTERFACE) {
                        kind = 8; detail = "Interface";
                        /*  rev3 — Capture le nom d'interface pour la
                         * coloration 'type' (violet). On ne descend pas dans le
                         * corps (pas de membre a resoudre). */
                        if (userInterfaceCount < 128) {
                            UserInterfaceInfo* ii = &userInterfaces[userInterfaceCount++];
                            int nlen = tok.length < 127 ? tok.length : 127;
                            memcpy(ii->name, tok.start, nlen);
                            ii->name[nlen] = '\0';
                            ii->docIndex = docIndex;            /*  */
                            ii->line = tok.line - 1;            /*  */
                            ii->character = tok.column > 0 ? tok.column - 1 : 0;
                        }
                    }

                    /* Variable declarations: "identifier EST [UN/UNE] type" */
                    if (kind == 0) {
                        /* The type token might be a primitive type directly,
                         * or a filler word (un/une) was consumed by the scanner
                         * and we already have the type token. */
                        const char* typeName = NULL;
                        switch (typeToken.type) {
                            case TOKEN_ENTIER:    typeName = "entier"; break;
                            case TOKEN_REEL:      typeName = "reel"; break;
                            case TOKEN_CHAINE:    typeName = "chaine"; break;
                            case TOKEN_BOOLEEN:   typeName = "booleen"; break;
                            case TOKEN_DATE:      typeName = "date"; break;
                            case TOKEN_HEURE:     typeName = "heure"; break;
                            case TOKEN_DATEHEURE: typeName = "dateheure"; break;
                            case TOKEN_DUREE:     typeName = "duree"; break;
                            case TOKEN_MONETAIRE: typeName = "monetaire"; break;
                            case TOKEN_NUMERIQUE: typeName = "numerique"; break;
                            case TOKEN_CARACTERE: typeName = "caractere"; break;
                            case TOKEN_BUFFER:    typeName = "buffer"; break;
                            case TOKEN_JSON_KW:   typeName = "json"; break;
                            case TOKEN_TABLEAU:   typeName = "tableau"; break;
                            case TOKEN_FILE:      typeName = "file"; break;
                            case TOKEN_PILE:      typeName = "pile"; break;
                            case TOKEN_LISTE:     typeName = "liste"; break;
                            case TOKEN_UUID:      typeName = "uuid"; break;
                            case TOKEN_IDENTIFIER: typeName = NULL; break; /* struct type name - already handled by struct instantiation */
                            default: break;
                        }
                        if (typeName != NULL) {
                            kind = 6; /* Variable */
                            detail = typeName;
                        } else if (typeToken.type == TOKEN_IDENTIFIER) {
                            /* Could be a struct type or a registered builtin
                             * type (httpRequête, Chrono…). Capture the actual
                             * type name so property completion can resolve
                             * its fields. */
                            static char typeBuf[128];
                            int tlen = typeToken.length < 127 ? typeToken.length : 127;
                            memcpy(typeBuf, typeToken.start, tlen);
                            typeBuf[tlen] = '\0';
                            kind = 6;
                            detail = typeBuf;
                        }
                    }

                    if (kind != 0 && docIndex >= 0 && userSymbolCount < 1024) {
                        /* userSymbols[] indexe lspServer.documents par
                         * docIndex (go-to-definition, hover). Skip pour les
                         * fichiers disque (docIndex == -1) — les containers
                         * cross-file (userClasses/userStructs/...) capturent
                         * deja l'info utile pour la completion. */
                        UserSymbol* sym = &userSymbols[userSymbolCount++];
                        int len = tok.length < 127 ? tok.length : 127;
                        memcpy(sym->name, tok.start, len);
                        sym->name[len] = '\0';
                        sym->line = tok.line - 1;
                        sym->character = tok.column > 0 ? tok.column - 1 : 0;
                        sym->kind = kind;
                        snprintf(sym->detail, sizeof(sym->detail), "%s", detail);
                        sym->docIndex = docIndex;
                        /*  — une variable confinee (mode LOCAL) n'est
                         * PAS globale : token variable sans modifier `global`. */
                        sym->isGlobal = (kind == 6) && IS_TOPLEVEL_GLOBAL() && !confineMode;
                    }
                    else if (kind == 6 && docIndex < 0 && filePath && filePath[0] != '\0'
                             && IS_TOPLEVEL_GLOBAL() && !confineMode && diskGlobalCount < 256) {
                        /*  — instance globale d'un fichier disque NON
                         * ouvert : userSymbols n'indexe que les docs ouverts,
                         * on la memorise dans l'index disque (nom + fileUri +
                         * position) pour la coloration/go-to-def cross-fichier
                         * fenetre fermee. */
                        DiskGlobalInfo* g = &diskGlobals[diskGlobalCount++];
                        int len = tok.length < 127 ? tok.length : 127;
                        memcpy(g->name, tok.start, len);
                        g->name[len] = '\0';
                        snprintf(g->type, sizeof(g->type), "%s", detail ? detail : "");
                        g->line = tok.line - 1;
                        g->character = tok.column > 0 ? tok.column - 1 : 0;
                        filepath_to_uri(filePath, g->fileUri, (int)sizeof(g->fileUri));
                    }
                }
            }
        }
        #undef IS_TOPLEVEL_GLOBAL
    }
}

/*  — Vrai si `path` (chemin disque, normalisé / ou \) couvre l'un des
 * documents actuellement ouverts dans l'editeur. Comparaison par suffix-match
 * sur les separateurs normalises (tolere C:/x vs C:\x, dossier de travail
 * relatif vs absolu). Utilise pour skipper le scan disque des fichiers deja
 * dans le buffer (priorite buffer > disque). */
static bool path_matches_open_doc(const char* diskPath) {
    char diskNorm[1024];
    int n = 0;
    for (int i = 0; diskPath[i] && n < (int)sizeof(diskNorm) - 1; i++) {
        char c = diskPath[i];
        diskNorm[n++] = (c == '\\') ? '/' : c;
    }
    diskNorm[n] = '\0';
    size_t dl = (size_t)n;

    for (int i = 0; i < lspServer.documentCount; i++) {
        char openPath[1024];
        uri_to_filepath(lspServer.documents[i].uri, openPath, sizeof(openPath));
        /* Normalise separateurs pour le compare. */
        char openNorm[1024];
        int on = 0;
        for (int j = 0; openPath[j] && on < (int)sizeof(openNorm) - 1; j++) {
            char c = openPath[j];
            openNorm[on++] = (c == '\\') ? '/' : c;
        }
        openNorm[on] = '\0';
        size_t ol = (size_t)on;
        if (ol == 0) continue;
        /* Comparaison case-insensitive (Windows). Suffix-match dans les 2 sens
         * : un des deux est potentiellement absolu, l'autre relatif. */
        if (ol <= dl && strcasecmp(diskNorm + (dl - ol), openNorm) == 0)
            return true;
        if (dl <= ol && strcasecmp(openNorm + (ol - dl), diskNorm) == 0)
            return true;
    }
    return false;
}

/*  — Walk le rootPath et applique scan_one_source_for_user_symbols
 * sur chaque .owl non ouvert. Profondeur max 6 (workspaces avec arbo
 * core/models/views/... raisonnable). Buffer max 1 MB par fichier
 * (defensive). Cout amortie par le cache fingerprint. */
static void scan_workspace_files_for_user_symbols(const char* dir, int depth,
                                                  bool recursive) {
    if (depth > 6) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* Doctrine  : SCRIPT/ENTRYPOINT ne recursent pas. Seul
             * un fichier sous projet.owl (PROJECT) walk les sous-dossiers. */
            if (!recursive) continue;
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            scan_workspace_files_for_user_symbols(path, depth + 1, recursive);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        /* Priorite buffer > disque : skip si le fichier est deja ouvert. */
        if (path_matches_open_doc(path)) continue;
        /* Lire le contenu (defensive : taille max 1 MB). */
        if (st.st_size <= 0 || st.st_size > 1024 * 1024) continue;
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char* buf = (char*)malloc((size_t)st.st_size + 1);
        if (!buf) { fclose(f); continue; }
        size_t r = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        buf[r] = '\0';
        scan_one_source_for_user_symbols(buf, /*docIndex*/ -1, path);
        free(buf);
    }
    closedir(d);
}

/*  — Mixe path + mtime + size de chaque .owl du workspace dans le
 * fingerprint courant. Pas d'ouverture de fichier, just stat(). Permet de
 * detecter qu'un .owl du workspace a change sur disque (save d'un autre
 * editeur, git pull, etc.) et de re-declencher un rescan. */
static uint64_t hash_workspace_files(const char* dir, int depth,
                                     bool recursive, uint64_t fp) {
    if (depth > 6) return fp;
    DIR* d = opendir(dir);
    if (!d) return fp;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* Symetrique avec scan_workspace_files_for_user_symbols : meme
             * doctrine de recursion pour que le fingerprint reflete le scope
             * effectivement scanne. */
            if (!recursive) continue;
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            fp = hash_workspace_files(path, depth + 1, recursive, fp);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        for (size_t i = 0; i < nl; i++) {
            fp = ((fp << 5) ^ (fp >> 27)) ^ (uint64_t)(unsigned char)e->d_name[i];
        }
        fp ^= (uint64_t)st.st_size * 0xC2B2AE3D27D4EB4FULL;
        fp ^= (uint64_t)st.st_mtime * 0x9E3779B97F4A7C15ULL;
        fp = (fp ^ (fp >> 27)) * 0x94D049BB133111EBULL;
    }
    closedir(d);
    return fp;
}

/* Scan minimal des .owl du rootPath pour extraire uniquement les noms
 * de Fenetre/Etat top-level. Buffer max 1 MB par fichier (defensive),
 * profondeur max 3. Skip les fichiers deja dans documents[] (eux sont
 * scannes par scan_user_symbols). */
static void scan_one_file_for_dsl(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';

    Scanner sc;
    scanner_init(&sc, buf);
    Token prev2 = (Token){0};
    Token prev1 = (Token){0};
    for (;;) {
        Token tok = scanner_scan_token(&sc);
        if (tok.type == TOKEN_EOF) break;
        if ((tok.type == TOKEN_FENETRE || tok.type == TOKEN_ETAT)
            && prev1.type == TOKEN_EST
            && prev2.type == TOKEN_IDENTIFIER
            && userWindowCount < 256) {
            /* Skip si le nom est deja enregistre. */
            bool dup = false;
            for (int i = 0; i < userWindowCount; i++) {
                if ((int)strlen(userWindows[i].name) == prev2.length &&
                    strncmp(userWindows[i].name, prev2.start, (size_t)prev2.length) == 0) {
                    dup = true; break;
                }
            }
            if (!dup) {
                int n = prev2.length < 127 ? prev2.length : 127;
                memcpy(userWindows[userWindowCount].name, prev2.start, n);
                userWindows[userWindowCount].name[n] = '\0';
                userWindows[userWindowCount].isWindow = (tok.type == TOKEN_FENETRE);
                userWindowCount++;
            }
        }
        /*  — classe declaree dans un fichier projet non ouvert :
         * 'IDENT est une Classe'. L'article un/une est consomme par le
         * scanner, donc tok==CLASSE suit directement EST. On capte le corps
         * dans le cache projet (docIndex=-1) pour merge ulterieur. Premier
         * gagne : on skip si une classe du meme nom est deja dans le cache.
         * scan_class_body avance `sc` jusqu'au FIN matchant : on reset
         * prev1/prev2 derriere pour ne pas matcher a travers le corps. */
        if (tok.type == TOKEN_CLASSE
            && prev1.type == TOKEN_EST
            && prev2.type == TOKEN_IDENTIFIER
            && g_projectClassCount < 64) {
            bool dup = false;
            for (int i = 0; i < g_projectClassCount; i++) {
                if ((int)strlen(g_projectClasses[i].name) == prev2.length &&
                    strncasecmp(g_projectClasses[i].name, prev2.start,
                                (size_t)prev2.length) == 0) {
                    dup = true; break;
                }
            }
            if (!dup) {
                scan_class_body(&sc, prev2, -1, path,
                                &g_projectClasses[g_projectClassCount++]);
            } else {
                /* Drain le corps de la classe dupliquee dans un slot jetable
                 * pour repositionner `sc` apres son FIN (pas d'URI : jetable). */
                UserClassInfo discard;
                scan_class_body(&sc, prev2, -1, NULL, &discard);
            }
            prev2 = (Token){0};
            prev1 = (Token){0};
            continue;
        }
        /*  — article EN transparent (cf. tok_is_en_article). */
        if (tok_is_en_article(tok) && prev1.type == TOKEN_EST) {
            continue;
        }
        prev2 = prev1;
        prev1 = tok;
    }
    free(buf);
}

static void scan_project_dsl_walk(const char* dir, int depth, bool recursive) {
    if (depth > 3) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* Script (non recursif) : on ignore les sous-dossiers, seul le
             * dossier voisin compte ( « Workspace != projet »). Seul
             * un projet (frontiere projet.owl) descend dans son arbre. */
            if (!recursive) continue;
            /* Skip build dirs evidents. */
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            scan_project_dsl_walk(path, depth + 1, recursive);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        /* Skip si ce path est un document deja ouvert. */
        bool open = false;
        for (int i = 0; i < lspServer.documentCount; i++) {
            char openPath[1024];
            uri_to_filepath(lspServer.documents[i].uri, openPath, sizeof(openPath));
            /* Comparaison naïve : si le suffix du path matche, on considere ouvert. */
            size_t pl = strlen(path);
            size_t opl = strlen(openPath);
            if (opl > 0 && opl <= pl &&
                strcmp(path + (pl - opl), openPath) == 0) {
                open = true; break;
            }
            if (pl > 0 && pl <= opl &&
                strcmp(openPath + (opl - pl), path) == 0) {
                open = true; break;
            }
        }
        if (!open) scan_one_file_for_dsl(path);
    }
    closedir(d);
}

static void scan_project_dsl_names(void) {
    if (lspServer.rootPath[0] == '\0') return;
    /* Scope par buffer au lieu d'un walk recursif inconditionnel
     * depuis rootPath (le workspace VSCode entier). Un fichier-script isole
     * ne scanne que son dossier voisin (non recursif) ; un fichier sous
     * projet.owl scanne l'arbre du projet (recursif). Aligne sur
     * scan_user_symbols_force qui resout deja les scopes par document. */
    char roots[LSP_SCAN_SCOPES_MAX][1024];
    bool recursive[LSP_SCAN_SCOPES_MAX];
    int nScopes = collect_open_doc_scopes(roots, recursive, LSP_SCAN_SCOPES_MAX);
    for (int i = 0; i < nScopes; i++) {
        scan_project_dsl_walk(roots[i], 0, recursive[i]);
    }
}

/*  — Lazy trigger pour le scan workspace cross-file. Appele
 * par les handlers qui dependent des noms FEN_/ETAT_/struct/class
 * declares dans des .owl non-ouverts (completion cross-file,
 * go-to-definition cross-file). Idempotent : une fois fait, ne
 * refait pas le walk. */
static bool g_lspProjectScanDone = false;
static void lsp_ensure_project_scan(void) {
    if (g_lspProjectScanDone) return;
    int64_t _t0 = LSP_NOW_MS();
    scan_project_dsl_names();
    g_lspProjectScanDone = true;
    int64_t _dt = LSP_NOW_MS() - _t0;
    if (_dt >= 5) {
        lsp_log("[timing] lazy project scan (1st call): %lldms",
                (long long)_dt);
    }
}

/* ================================================================
 * LIFECYCLE HANDLERS
 * ================================================================ */

JsonValue* handle_initialize(JsonValue* params) {
    lspServer.initialized = true;

    const char* rootUri = json_get_string(params, "rootUri");
    if (rootUri) {
        strncpy(lspServer.rootUri, rootUri, sizeof(lspServer.rootUri) - 1);
        uri_to_filepath(rootUri, lspServer.rootPath, sizeof(lspServer.rootPath));
    }
    lsp_log("Initialized. Root: %s -> %s", lspServer.rootUri, lspServer.rootPath);

    /* Langue preferee pour les completions (cf cahier des charges §4).
     * Defaut "fr". Le client peut passer initializationOptions.language. */
    strcpy(lspServer.language, "fr");
    JsonValue* initOpts = json_get(params, "initializationOptions");
    if (initOpts) {
        const char* lang = json_get_string(initOpts, "language");
        if (lang && (strcasecmp(lang, "en") == 0 || strcasecmp(lang, "english") == 0)) {
            strcpy(lspServer.language, "en");
        }
    }
    lsp_log("Completion language: %s", lspServer.language);

    /* Resolve owl executable for --check diagnostics.
     * owl.exe est all-in-one (interpreter + GUI Qt6) depuis le merge,
     * plus de owl-gui.exe separe.
     * Ordre de priorite :
     *   1. owl.exe voisin du binaire owl-lsp COURANT (distribues ensemble :
     *      release/owl.exe a cote de release/owl-lsp.exe ; build/owl.exe a
     *      cote de build/owl-lsp.exe en dev). Le voisin est le bon dans tous
     *      les cas — aucune reference a un dossier `build` en dur.
     *   2. PATH systeme : owl */
    lspServer.owlExePath[0] = '\0';
#ifdef _WIN32
    const char* exts_pathsys[] = {"\\owl.exe", "\\owl", NULL};
    const char* selfName       = "\\owl.exe";
    const char* sep = ";";
#else
    const char* exts_pathsys[] = {"/owl", NULL};
    const char* selfName       = "/owl";
    const char* sep = ":";
#endif
    /* Etape 1 : owl.exe voisin du binaire owl-lsp en cours d'execution. */
    {
        char selfDir[sizeof(lspServer.owlExePath)];
        if (lsp_self_exe_dir(selfDir, sizeof(selfDir))) {
            char candidate[sizeof(lspServer.owlExePath)];
            int n = snprintf(candidate, sizeof(candidate), "%s%s", selfDir, selfName);
            if (n > 0 && n < (int)sizeof(candidate)) {
                FILE* f = fopen(candidate, "rb");
                if (f) {
                    fclose(f);
                    memcpy(lspServer.owlExePath, candidate, (size_t)n + 1);
                }
            }
        }
    }
    /* Etape 2 : PATH systeme. */
    if (!lspServer.owlExePath[0]) {
        const char* pathEnv = getenv("PATH");
        if (pathEnv) {
            char pathCopy[8192];
            strncpy(pathCopy, pathEnv, sizeof(pathCopy) - 1);
            pathCopy[sizeof(pathCopy) - 1] = '\0';
            char* dir = strtok(pathCopy, sep);
            while (dir) {
                size_t dirLen = strlen(dir);
                if (dirLen >= sizeof(lspServer.owlExePath) - 32) {
                    dir = strtok(NULL, sep); continue;
                }
                for (int e = 0; exts_pathsys[e]; e++) {
                    char candidate[sizeof(lspServer.owlExePath)];
                    int n = snprintf(candidate, sizeof(candidate), "%s%s", dir, exts_pathsys[e]);
                    if (n <= 0 || n >= (int)sizeof(candidate)) continue;
                    FILE* f = fopen(candidate, "rb");
                    if (f) {
                        fclose(f);
                        memcpy(lspServer.owlExePath, candidate, (size_t)n + 1);
                        break;
                    }
                }
                if (lspServer.owlExePath[0]) break;
                dir = strtok(NULL, sep);
            }
        }
    }
    if (lspServer.owlExePath[0]) {
        lsp_log("Found owl executable: %s", lspServer.owlExePath);
    } else {
        lsp_log("WARNING: owl executable not found next to owl-lsp or in PATH. Compiler diagnostics disabled.");
    }

    JsonValue* caps = json_new_object();

    /* Text document sync: full content */
    JsonValue* textDocSync = json_new_object();
    json_object_set(textDocSync, "openClose", json_new_bool(true));
    json_object_set(textDocSync, "change", json_new_int(1)); /* Full */
    json_object_set(textDocSync, "save", json_new_bool(true));
    json_object_set(caps, "textDocumentSync", textDocSync);

    /* Completion */
    JsonValue* completionOpts = json_new_object();
    JsonValue* triggerChars = json_new_array();
    json_array_push(triggerChars, json_new_string("."));
    json_array_push(triggerChars, json_new_string(":"));
    /*  — '(' declenche l'auto-popup en contexte d'arg. Le handler
     * (via params.context) ne renvoie une liste que pour les contextes
     * focalises (fenetres au 1er arg d'Ouvre, membres enum) ; sur un '('
     * generique il renvoie vide (le signatureHelp reste, lui, sur '(' via
     * son propre provider). */
    json_array_push(triggerChars, json_new_string("("));
    /*  — ',' declenche l'auto-popup au 2nd terme d'une indirection
     * `{ nom, | }` (liste indXxx, branche ). Meme discipline que '(' :
     * hors contexte focalise, le handler renvoie vide (garde commaTrigger). */
    json_array_push(triggerChars, json_new_string(","));
    json_object_set(completionOpts, "triggerCharacters", triggerChars);
    json_object_set(caps, "completionProvider", completionOpts);

    /* No documentFormattingProvider: the VS Code extension wires its own
     * formatter via registerDocumentFormattingEditProvider (calls
     * `owl fmt`, which delivers the full canonicalisation set — keywords,
     * accents, user symbols). Advertising a second formatter from the
     * LSP makes VS Code arbitrate silently and strip the save-time format.
     * Editors without an OWL extension (Helix, Neovim, Zed) will still get
     * the plugin's behaviour by calling `owl fmt` directly if configured. */

    /* Signature Help */
    JsonValue* sigHelpOpts = json_new_object();
    JsonValue* sigTrigger = json_new_array();
    json_array_push(sigTrigger, json_new_string("("));
    json_array_push(sigTrigger, json_new_string(","));
    json_object_set(sigHelpOpts, "triggerCharacters", sigTrigger);
    json_object_set(caps, "signatureHelpProvider", sigHelpOpts);

    /* Hover */
    json_object_set(caps, "hoverProvider", json_new_bool(true));

    /* Go to definition */
    json_object_set(caps, "definitionProvider", json_new_bool(true));

    /*  Lot 1 -- Find all references, scope-aware */
    json_object_set(caps, "referencesProvider", json_new_bool(true));

    /*  Lot 2a -- Rename, scope-aware (locales/parametres) */
    JsonValue* renameOpts = json_new_object();
    json_object_set(renameOpts, "prepareProvider", json_new_bool(true));
    json_object_set(caps, "renameProvider", renameOpts);

    /* Document formatting (ticket 0326) — le LSP heberge le formatter (backend
     * catalogue), formatage instantane (process chaud) au lieu de spawn owl.exe.
     * Le plugin VSCode lit cette capability au handshake et desactive son propre
     * provider (qui spawnait owl.exe) pour eviter l'arbitrage de deux formatters. */
    json_object_set(caps, "documentFormattingProvider", json_new_bool(true));

    /*  — structure d'un fichier fenetre : outline (documentSymbol)
     * + pliage de code (foldingRange) par champ et par procedure d'evenement. */
    json_object_set(caps, "documentSymbolProvider", json_new_bool(true));
    json_object_set(caps, "foldingRangeProvider", json_new_bool(true));

    /* Semantic tokens — legend (ordre important, indices figes) :
     *   0 = enumMember  (valeurs d'enum user-defined)
     *   1 = class       (noms de Fenetre - colores comme classes)
     *   2 = property    (noms de sous-champs Bouton/Libelle/Saisie)
     *   3 = variable    (variables) — modifier 0 = "global"
     *   4 = type        (references a une classe user : decl ET usage)
     *   5 = method      (appels de methode resolus sur une classe user)
     * Modifiers (bitmask) :
     *   0 = global      (variable au top-level Main / Collection) */
    JsonValue* semanticTokensOpts = json_new_object();
    JsonValue* legend = json_new_object();
    JsonValue* tokenTypes = json_new_array();
    json_array_push(tokenTypes, json_new_string("enumMember"));  /* 0 */
    json_array_push(tokenTypes, json_new_string("class"));       /* 1 - Fenetre name */
    json_array_push(tokenTypes, json_new_string("property"));    /* 2 - control name */
    json_array_push(tokenTypes, json_new_string("variable"));    /* 3 - vars (modifier "global") */
    json_array_push(tokenTypes, json_new_string("type"));        /* 4 - classe user */
    json_array_push(tokenTypes, json_new_string("method"));      /* 5 - methode user */
    json_object_set(legend, "tokenTypes", tokenTypes);
    JsonValue* tokenModifiers = json_new_array();
    json_array_push(tokenModifiers, json_new_string("global"));     /* 0 - top-level scope */
    json_array_push(tokenModifiers, json_new_string("namespace"));  /* 1 - receveur `icones` */
    json_object_set(legend, "tokenModifiers", tokenModifiers);
    json_object_set(semanticTokensOpts, "legend", legend);
    JsonValue* fullOpts = json_new_object();
    json_object_set(fullOpts, "delta", json_new_bool(false));
    json_object_set(semanticTokensOpts, "full", fullOpts);
    json_object_set(caps, "semanticTokensProvider", semanticTokensOpts);

    JsonValue* result = json_new_object();
    json_object_set(result, "capabilities", caps);

    JsonValue* serverInfo = json_new_object();
    json_object_set(serverInfo, "name", json_new_string("owl-lsp"));
    json_object_set(serverInfo, "version", json_new_string("0.1.0"));
    json_object_set(result, "serverInfo", serverInfo);

    return result;
}

void handle_initialized(JsonValue* params) {
    (void)params;
    lsp_log("Client confirmed initialization.");

    /*  Lot 2a — allume le résident de check TOUT DE SUITE, pas au
     * premier didChange : son démarrage coûte jusqu'à ~5 s à froid (scan d'un
     * binaire de ~100 Mo) et ce coût doit être payé ici, pendant que
     * l'utilisateur n'a encore rien tapé. Après `initialize` (la réponse est
     * déjà partie), donc sans retarder l'ouverture de l'éditeur. */
    if (lspServer.owlExePath[0]) {
        if (lsp_check_server_warmup(lspServer.owlExePath))
            lsp_log("Check server resident demarre : %s --check-server", lspServer.owlExePath);
        else
            lsp_log("Check server resident indisponible — repli sur un spawn par check.");
    }
}

void handle_shutdown(int64_t id) {
    lspServer.shutdown_requested = true;
    /* Le résident meurt avec son client : jamais d'`owl` orphelin. */
    lsp_check_server_stop();
    lsp_send_response(id, json_new_null());
    lsp_log("Shutdown requested.");
}

/* ================================================================
 * DIAGNOSTICS
 * ================================================================ */

/* Defini plus bas (section compiler diagnostics) ; utilise par le merge
 * du cache dans publish_diagnostics. */
static JsonValue* make_lsp_diag(int line1based, int col1based,
                                const char* msg, int severity,
                                int startCharOverride, int endCharOverride);

/* ================================================================
 *  — cache per-URI des diagnostics compilateur (--check)
 *
 * publish_compiler_diagnostics (didSave) peuple ce cache pour chaque URI
 * qu'il publie, y compris les URIs cleared (count 0). publish_diagnostics
 * (didOpen/didChange, scanner-only) fusionne les diags caches avec les
 * erreurs lexicales fraiches au lieu de publier un set semantique vide.
 *
 * Compromis assume (cf. ticket ) : les positions des diags caches
 * ne sont PAS reprojetees pendant l'edition (didChange) ; elles restent
 * a leur emplacement du dernier save et sont rafraichies au prochain save.
 * ================================================================ */

#define LSP_DIAG_CACHE_MAX_URIS    64    /* meme borne que diagnosedUris[] */
#define LSP_DIAG_CACHE_MAX_PER_URI 256   /* diags semantiques caches par URI */
#define LSP_DIAG_CACHE_MSG_MAX     512   /* message tronque a cette taille */

typedef struct {
    int  line;       /* 1-based, comme renvoye par --check */
    int  column;     /* 1-based */
    int  severity;   /* 1 = error, 2 = warning */
    /*  — etendue du mot en erreur, en unites UTF-16 (protocole LSP),
     * 0-based ; -1 = repli 1-char (make_lsp_diag utilise column..column+1).
     * Fige au dernier --check, comme line/column (compromis  : pas de
     * reprojection pendant l'edition). */
    int  startChar;
    int  endChar;
    char message[LSP_DIAG_CACHE_MSG_MAX];
} CachedDiag;

typedef struct {
    char       uri[1024];
    bool       used;       /* slot occupe (URI connu du cache) */
    int        count;      /* nb de diags valides (peut etre 0 = clean) */
    CachedDiag diags[LSP_DIAG_CACHE_MAX_PER_URI];
} DiagCacheEntry;

static DiagCacheEntry g_diagCache[LSP_DIAG_CACHE_MAX_URIS];
static int g_diagCacheNext = 0;  /* round-robin pour l'eviction quand plein */

/* Trouve l'entree pour cet URI, ou NULL si absente. */
static DiagCacheEntry* diag_cache_find(const char* uri) {
    for (int i = 0; i < LSP_DIAG_CACHE_MAX_URIS; i++) {
        if (g_diagCache[i].used && strcmp(g_diagCache[i].uri, uri) == 0) {
            return &g_diagCache[i];
        }
    }
    return NULL;
}

/* Trouve l'entree pour cet URI, en cree une (eviction round-robin si plein).
 * L'entree retournee a son count remis a 0 si nouvellement allouee. */
static DiagCacheEntry* diag_cache_get_or_create(const char* uri) {
    DiagCacheEntry* e = diag_cache_find(uri);
    if (e) return e;

    /* Cherche un slot libre. */
    for (int i = 0; i < LSP_DIAG_CACHE_MAX_URIS; i++) {
        if (!g_diagCache[i].used) {
            e = &g_diagCache[i];
            e->used = true;
            e->count = 0;
            snprintf(e->uri, sizeof(e->uri), "%s", uri);
            return e;
        }
    }

    /* Plein : eviction round-robin (cache borne, pas de fuite memoire). */
    e = &g_diagCache[g_diagCacheNext];
    g_diagCacheNext = (g_diagCacheNext + 1) % LSP_DIAG_CACHE_MAX_URIS;
    e->used = true;
    e->count = 0;
    snprintf(e->uri, sizeof(e->uri), "%s", uri);
    return e;
}

/* Reset le cache d'un URI a 0 diag (cree l'entree si absente). Utilise par
 * publish_compiler_diagnostics avant de re-remplir, et par le chemin clean. */
static void diag_cache_reset(const char* uri) {
    DiagCacheEntry* e = diag_cache_get_or_create(uri);
    e->count = 0;
}

/* Ajoute un diag au cache d'un URI (cree l'entree si absente). `startChar`/
 * `endChar` = etendue UTF-16 du mot ou -1/-1 pour le repli 1-char. */
static void diag_cache_add(const char* uri, int line, int column,
                           int severity, const char* msg,
                           int startChar, int endChar) {
    DiagCacheEntry* e = diag_cache_get_or_create(uri);
    if (e->count >= LSP_DIAG_CACHE_MAX_PER_URI) return;  /* borne par URI */
    CachedDiag* d = &e->diags[e->count++];
    d->line = line;
    d->column = column;
    d->severity = severity;
    d->startChar = startChar;
    d->endChar = endChar;
    snprintf(d->message, sizeof(d->message), "%s", msg ? msg : "");
}

/* --- API exposee au harness (cf. lsp_server.h) --- */

int lsp_get_compiler_diag_count(const char* uri) {
    DiagCacheEntry* e = diag_cache_find(uri);
    return e ? e->count : -1;  /* -1 = URI inconnu (jamais peuple) */
}

int lsp_get_compiler_diag_line(const char* uri, int index) {
    DiagCacheEntry* e = diag_cache_find(uri);
    if (!e || index < 0 || index >= e->count) return -1;  /* URI inconnu ou index hors bornes */
    return e->diags[index].line;
}

int lsp_get_compiler_diag_column(const char* uri, int index) {
    DiagCacheEntry* e = diag_cache_find(uri);
    if (!e || index < 0 || index >= e->count) return -1;  /* URI inconnu ou index hors bornes */
    return e->diags[index].column;
}

/*  — end.character (UTF-16, 0-based) du diag ; -1 si repli 1-char. */
int lsp_get_compiler_diag_end_char(const char* uri, int index) {
    DiagCacheEntry* e = diag_cache_find(uri);
    if (!e || index < 0 || index >= e->count) return -2;  /* URI inconnu / hors bornes */
    return e->diags[index].endChar;
}

void lsp_populate_compiler_diag_cache(const char* uri, int count) {
    diag_cache_reset(uri);
    for (int i = 0; i < count; i++) {
        diag_cache_add(uri, i + 1, 1, 1, "diagnostic semantique (test)", -1, -1);
    }
}

void lsp_clear_compiler_diag_cache(const char* uri) {
    /* L'entree reste connue (count 0) : distingue "propre" de "inconnu". */
    diag_cache_reset(uri);
}

/*  Lot 2 — garde de réentrance du check-overlay déclenché à l'édition
 * (voir publish_diagnostics) : neutralise les publish_diagnostics de repli
 * appelés depuis le démarrage d'un check, évitant toute récursion. */
static int g_overlay_check_in_progress = 0;

/*  — planification 2 niveaux (mono-thread, ni thread ni sleep bloquant) :
 *  - check fichier ACTIF a la frappe : contexte projet hydrate mais SEUL le
 *    fichier edite est compile+rapporte, et on ne republie que son URI (rapide,
 *    anti-flicker : les diagnostics des voisins restent affiches) ;
 *  - check FULL de fond : debounce ~1s apres l'arret de frappe et au save,
 *    rapport multi-fichiers, reconcilie/efface les URIs voisins.
 * Un SEUL check en vol a la fois ; l'actif est prioritaire (feedback immediat),
 * le full part quand son echeance echoit et qu'aucun actif n'est en attente.
 * Etat + declarations avancees ici : publish_diagnostics (didChange) les touche
 * bien avant la definition des fonctions de check plus bas. */
typedef enum { CHECK_KIND_FULL, CHECK_KIND_ACTIVE } CheckKind;

static LspCheckJob* g_checkJob = NULL;      /* check en vol, NULL si aucun */
static char g_checkJobUri[1024] = "";       /* URI ayant déclenché ce check */
static int  g_checkJobStale = 0;            /* le contenu a bougé depuis le lancement */
static CheckKind g_checkJobKind = CHECK_KIND_FULL;  /* nature du check en vol */

#define LSP_FULL_CHECK_DEBOUNCE_MS 1000
static char    g_activeReqUri[1024] = "";   /* check actif demande ("" = aucun) */
static char    g_fullReqUri[1024]   = "";   /* URI de reference du full de fond */
static int64_t g_fullDeadline       = 0;    /* echeance du full de fond (0 = aucun) */

static int  start_compiler_check(const char* uri, CheckKind kind);
static void lsp_check_schedule(void);
static void apply_check_result(const char* uri, LspRunStatus rs,
                               char* output, int total, CheckKind kind);

void publish_diagnostics(const char* uri) {
    int64_t _t_pd = LSP_NOW_MS();  /*  timing */
    int idx = find_doc(uri);
    if (idx < 0) return;

    const char* source = lspServer.documents[idx].content;
    JsonValue* diagnostics = json_new_array();

    /*  — Republie d'abord les diagnostics semantiques (--check) du
     * dernier save, pour qu'ils PERSISTENT a travers didOpen/didChange.
     * Sans ca, ce set scanner-only ecraserait les erreurs --check (flicker :
     * naviguer vers un fichier effacait son erreur jusqu'au prochain save).
     * Les positions ne sont pas reprojetees pendant l'edition (compromis
     * assume) ; elles sont rafraichies au prochain --check (didSave). */
    DiagCacheEntry* cached = diag_cache_find(uri);
    if (cached) {
        for (int i = 0; i < cached->count; i++) {
            CachedDiag* d = &cached->diags[i];
            json_array_push(diagnostics, make_lsp_diag(
                d->line, d->column, d->message, d->severity,
                d->startChar, d->endChar));
        }
    }

    Scanner diagScanner;
    scanner_init(&diagScanner, source);

    for (;;) {
        Token tok = scanner_scan_token(&diagScanner);
        if (tok.type == TOKEN_EOF) break;
        if (tok.type == TOKEN_ERROR) {
            JsonValue* diag = json_new_object();
            JsonValue* range = json_new_object();
            JsonValue* start = json_new_object();
            json_object_set(start, "line", json_new_int(tok.line > 0 ? tok.line - 1 : 0));
            json_object_set(start, "character", json_new_int(tok.column > 0 ? tok.column - 1 : 0));
            JsonValue* end = json_new_object();
            json_object_set(end, "line", json_new_int(tok.line > 0 ? tok.line - 1 : 0));
            json_object_set(end, "character", json_new_int(tok.column > 0 ? tok.column : 1));
            json_object_set(range, "start", start);
            json_object_set(range, "end", end);
            json_object_set(diag, "range", range);
            json_object_set(diag, "severity", json_new_int(1));
            json_object_set(diag, "source", json_new_string("owl"));
            json_object_set(diag, "message", json_new_string(tok.start));
            json_array_push(diagnostics, diag);
        }
    }

    JsonValue* params = json_new_object();
    json_object_set(params, "uri", json_new_string(uri));
    json_object_set(params, "diagnostics", diagnostics);
    lsp_send_notification("textDocument/publishDiagnostics", params);

    /* Rescan user symbols on every change */
    int64_t _t_scan = LSP_NOW_MS();
    scan_user_symbols();
    int64_t _dt_scan = LSP_NOW_MS() - _t_scan;
    if (_dt_scan >= 5) {
        lsp_log("[timing] scan_user_symbols: %lldms",
                (long long)_dt_scan);
    }
    int64_t _dt_pd = LSP_NOW_MS() - _t_pd;
    if (_dt_pd >= 5) {
        lsp_log("[timing] publish_diagnostics total: %lldms",
                (long long)_dt_pd);
    }

    /*  Lot 2 — le check overlay voit l'état RÉEL des buffers ouverts
     * (fichiers non-sauvés inclus), pas le disque périmé. La garde de réentrance
     * neutralise les appels publish_diagnostics de repli faits DEPUIS le
     * démarrage du check (pas d'exe, chemin rejeté, spawn fail), évitant toute
     * récursion. Inerte sans owlExePath (cas de la plupart des tests in-process)
     * ou hors scheme file://.
     *
     *  — planification 2 niveaux à la frappe (aucun spawn ici : c'est
     * le pump qui lance, cf. lsp_check_schedule) :
     *   1. check fichier ACTIF (rapide) demandé à chaque didChange ; coalescé
     *      naturellement (une seule requête en attente, la plus récente) — la
     *      cadence est bornée par la durée du check, pas par le rythme de frappe.
     *   2. check FULL de fond, échéance (re)armée à now + ~1 s : il ne part
     *      qu'après l'arrêt de la frappe (échéance repoussée à chaque frappe),
     *      pour rattraper les erreurs cross-fichier dans les AUTRES fichiers. */
    if (!g_overlay_check_in_progress && lspServer.owlExePath[0] &&
        strncmp(uri, "file://", 7) == 0) {
        snprintf(g_activeReqUri, sizeof(g_activeReqUri), "%s", uri);
        snprintf(g_fullReqUri, sizeof(g_fullReqUri), "%s", uri);
        g_fullDeadline = LSP_NOW_MS() + LSP_FULL_CHECK_DEBOUNCE_MS;
        /* Le check en vol sur CE fichier décrit un état déjà périmé : on le jette
         * (anti-flicker) — l'actif qu'on vient de demander le remplacera. */
        if (g_checkJob && strcmp(g_checkJobUri, uri) == 0) g_checkJobStale = 1;
        lsp_check_schedule();
    }
}

/*  — forme EN d'un mot-cle a partir de sa forme FR (ou de
 * n'importe quel alias). Memes regles que le dump --vscode-data
 * (cf. src/main.c) : on cherche, parmi tous les alias du meme TokenType,
 * le premier dont la forme ascii-minuscule differe du canon FR, et on le
 * remet en majuscules (les mots-cles s'affichent en majuscules). Ecrit
 * dans `out` (taille `outsz`). Retourne false si pas de forme EN distincte
 * (out laisse vide).
 *
 *  — resolution du TokenType decouplee du keyword catalog. On
 * resout le token du mot d'entree directement depuis le scanner (1ere
 * passe), car certains mots-cles reconnus par le scanner sont volontairement
 * exclus du catalog (ex: TOKEN_PROPRIETES, DSL Fenetre) et n'auraient
 * jamais de traduction sinon. */
static bool keyword_en_form(const char* frWord, char* out, size_t outsz) {
    if (!frWord || !*frWord || outsz == 0) return false;
    out[0] = '\0';

    /* canon ascii-minuscule du mot FR de reference. */
    char canonLow[64];
    int cn = scanner_strip_accents(frWord, (int)strlen(frWord),
                                   canonLow, (int)sizeof(canonLow));
    for (int i = 0; i < cn; i++)
        if (canonLow[i] >= 'A' && canonLow[i] <= 'Z') canonLow[i] += 32;
    canonLow[cn] = '\0';

    /* Keywords contextuels a canon EN MIXTE (MySelf/MyControl/MyWindow).
     * La derivation generique ci-dessous uppercase l'alias scanner (myself
     * -> MYSELF), correct pour les mots-cles all-caps (IF/WHILE/FOR) mais
     * faux pour ces references systeme dont le canon est PascalCase. On
     * retourne la forme canon mixte directement (miroir des partners EN de
     * la table fmt_keywords). */
    static const struct { const char* fr; const char* en; } KW_EN_MIXED[] = {
        {"moimeme",   "MySelf"},
        {"monchamp",  "MyControl"},
        {"mafenetre", "MyWindow"},
    };
    for (size_t m = 0; m < sizeof(KW_EN_MIXED)/sizeof(KW_EN_MIXED[0]); m++) {
        if (strcmp(canonLow, KW_EN_MIXED[m].fr) == 0) {
            snprintf(out, outsz, "%s", KW_EN_MIXED[m].en);
            return true;
        }
    }

    /* 1ere passe : TokenType du mot d'entree via le scanner (canon ascii). */
    TokenType want = TOKEN_EOF;
    bool found = false;
    for (int a = 0; ; a++) {
        TokenType at;
        const char* alias = scanner_keyword_at(a, &at);
        if (!alias) break;
        char al[64];
        int an = scanner_strip_accents(alias, (int)strlen(alias),
                                       al, (int)sizeof(al));
        char alLow[64];
        for (int j = 0; j < an; j++)
            alLow[j] = (al[j] >= 'A' && al[j] <= 'Z') ? (char)(al[j] + 32) : al[j];
        alLow[an] = '\0';
        if (strcmp(alLow, canonLow) == 0) { want = at; found = true; break; }
    }
    if (!found) return false;

    /* 2e passe : alias du meme token dont le canon differe (= forme EN). */
    for (int a = 0; ; a++) {
        TokenType at;
        const char* alias = scanner_keyword_at(a, &at);
        if (!alias) break;
        if (at != want) continue;
        char al[64];
        int an = scanner_strip_accents(alias, (int)strlen(alias),
                                       al, (int)sizeof(al));
        char alLow[64];
        for (int j = 0; j < an; j++)
            alLow[j] = (al[j] >= 'A' && al[j] <= 'Z') ? (char)(al[j] + 32) : al[j];
        alLow[an] = '\0';
        if (strcmp(alLow, canonLow) != 0) {
            size_t e = 0;
            for (int j = 0; j < an && e + 1 < outsz; j++)
                out[e++] = (al[j] >= 'a' && al[j] <= 'z') ? (char)(al[j] - 32) : al[j];
            out[e] = '\0';
            return e > 0;
        }
    }
    return false;
}

/* ================================================================
 * COMPLETION -- keywords + types + natives + user symbols
 * ================================================================ */

/* ================================================================
 * DSL FENETRE / ETAT — AUTOCOMPLETE DES PROPRIETES
 *
 * Source unique : OWL_DSL_PROP_CATALOG[] (genere par dump_lsp_catalog
 * depuis src/ui_catalog.c). Plus de tables hardcodees ici — toute
 * synchro est automatique au build.
 *
 * Les contextes DSL utilisent un char compact (kindChar) ; on le mappe
 * a la string utilisee dans le catalogue ("Fenetre", "Libelle", etc.).
 * ================================================================ */

/* Mapping char compact (LSP context stack) <-> OwlControlKind. La table
 * unique vit ici car les chars sont specifiques au LSP — l'encodage dense
 * permet un stack[64] borne sans bumper le memory budget par token.
 *
 * Pour les kinds widgets, on derive le nom canon depuis CONTROL_KINDS
 * (via owl_control_kind_name). Pour les sous-blocs Etat (E/e/l), nom
 * en dur car ils n'ont pas d'OwlControlKind. */
typedef struct { char ch; OwlControlKind kind; } DslKindCharMap;
static const DslKindCharMap DSL_KIND_CHARS[] = {
    {'F', OWL_CONTROL_WINDOW},
    {'L', OWL_CONTROL_LABEL},
    {'B', OWL_CONTROL_BUTTON},
    {'S', OWL_CONTROL_INPUT},
    {'T', OWL_CONTROL_TABLE},
    {'O', OWL_CONTROL_CONTAINER},   /* O = cOnteneur (C pris par colonne) */
    {'I', OWL_CONTROL_IMAGE},
    {'Z', OWL_CONTROL_LISTE},
    {'K', OWL_CONTROL_COMBO},
    {'R', OWL_CONTROL_SELECTEUR},
    {'Q', OWL_CONTROL_INTERRUPTEUR},
    {'Y', OWL_CONTROL_ZONE_REPETEE},
    {'J', OWL_CONTROL_JAUGE},
    {'D', OWL_CONTROL_CODEBARRE},    /* D = coDebarres (C pris par colonne) */
    {'N', OWL_CONTROL_ONGLET},       /* N = oNglet (O pris par cOnteneur) */
    {'V', OWL_CONTROL_VOLET},        /* V = Volet */
};
#define DSL_KIND_CHARS_N (int)(sizeof(DSL_KIND_CHARS)/sizeof(DSL_KIND_CHARS[0]))

/* Non-static : expose pour le harness lsp_test_main (Lot F). */
const char* dsl_kind_string(char k) {
    for (int i = 0; i < DSL_KIND_CHARS_N; i++) {
        if (DSL_KIND_CHARS[i].ch == k) {
            return owl_control_kind_name(DSL_KIND_CHARS[i].kind);
        }
    }
    /* Sous-blocs Etat (pas de kind enum) — strings en dur. */
    switch (k) {
        case 'C': return "Colonne";
        case 'E': return "Etat";
        case 'e': return "blocEtat";
        case 'l': return "LibelleEtat";
        case 'm': return "ImageEtat";   /* Image WYSIWYG dans un bloc Etat */
        case 'd': return "CodeBarresEtat"; /* CodeBarres WYSIWYG dans un bloc Etat */
        /*  — genre synthetique : la cellule (et la ligne) d'une Table
         * n'ouvre AUCUN bloc, elle n'existe qu'en receveur indexe
         * (`TABLE_X[ligne][colonne]..`). Pas de detection de contexte a cabler
         * donc, mais le char doit exister : c'est par lui que le catalogue de
         * props est adressable. 'k' minuscule ('K' majuscule = Combo). */
        case 'k': return "CelluleTable";
        /* Noeuds du menu classique de fenetre (pas de kind enum). La detection
         * de contexte au curseur reste a cabler ; le mapping existe des
         * maintenant pour que le lookup de props par kind soit resoluble. */
        case 'M': return "MenuPrincipal";
        case 'x': return "MenuContextuel";
        case 'u': return "SousMenu";
        case 'o': return "OptionMenu";
        case 'r': return "MenuSeparateur";
        /*  — blocs du DSL de service HTTP. Detection de contexte au
         * curseur a cabler (comme les noeuds de menu) ; le mapping existe des
         * maintenant pour que le lookup de props par kind soit resoluble. */
        case 's': return "ServiceREST";
        case 'h': return "OperationHTTP";
        default:  return NULL;
    }
}

/*  — ce char de pile designe-t-il un genre DSL adressable dans le
 * catalogue de props ? Derive de dsl_kind_string (DSL_KIND_CHARS + les
 * sous-blocs Etat/menu qui n'ont pas d'OwlControlKind) : tout genre ajoute
 * au catalogue est couvert sans retoucher les call-sites. Les chars
 * purement structurels de la pile de contexte ('p' PROPRIETES, 'P'
 * procedure, 'X' bloc neutre, 'G' collection) n'ont pas de nom de genre
 * et sont donc exclus d'office.
 *
 * Sans ce predicat, la resolution du genre englobant enumerait les chars a
 * la main : les genres absents de la liste (Onglet 'N', Volet 'V') etaient
 * traverses par la boucle, qui retombait sur la Fenetre -> completion des
 * props de la Fenetre dans un bloc PROPRIETES d'Onglet. */
static bool dsl_kind_char_has_props(char k) {
    return dsl_kind_string(k) != NULL;
}

/* Mapping char -> OwlControlKind. Retourne -1 si non widget runtime. */
static int dsl_kind_char_to_control_kind(char k) {
    for (int i = 0; i < DSL_KIND_CHARS_N; i++) {
        if (DSL_KIND_CHARS[i].ch == k) return (int)DSL_KIND_CHARS[i].kind;
    }
    return -1;
}

/* Inverse : OwlControlKind -> char. Retourne 0 si pas dans la table. */
static char dsl_char_for_kind(OwlControlKind k) {
    for (int i = 0; i < DSL_KIND_CHARS_N; i++) {
        if (DSL_KIND_CHARS[i].kind == k) return DSL_KIND_CHARS[i].ch;
    }
    return 0;
}

/*  — char de contexte DSL du bloc ouvert par le token de genre d'une
 * declaration `IDENT est un [Champ] <Genre>`, 0 si le token n'ouvre pas de
 * bloc de controle. Source unique = CONTROL_KINDS (owl_control_kind_from_token
 * -> DSL_KIND_CHARS) + le sous-bloc structurel Volet (is_widget=false cote
 * runtime, donc rejete par owl_control_kind_from_token). Les deux genres
 * porteurs d'un token scanner dedie (Liste, Conteneur) sont mappes ici aussi
 * pour que les call-sites qui bornent un bloc n'aient qu'un point d'entree.
 *
 * Sans ce point d'entree unique, chaque helper de bornage (find_dsl_block_start
 * / find_dsl_block_end) listait a la main quelques genres : tous les genres
 * absents de la liste (Onglet, Combo, Selecteur, ...) n'etaient jamais
 * reconnus comme ouverture de bloc -> bornage faux. */
static char dsl_block_open_char(TokenType type, const char* start, int length) {
    if (type == TOKEN_LISTE)     return 'Z';
    if (type == TOKEN_CONTENEUR) return 'O';
    if (type != TOKEN_IDENTIFIER || !start || length <= 0) return 0;
    OwlControlKind k;
    if (owl_control_kind_from_token(start, length, &k, NULL))
        return dsl_char_for_kind(k);
    char buf[64];
    int n = length < (int)sizeof(buf) - 1 ? length : (int)sizeof(buf) - 1;
    memcpy(buf, start, (size_t)n);
    buf[n] = '\0';
    if (is_volet_block_type_name(buf)) return 'V';
    return 0;
}

/* Recherche d'une prop DSL par (kind, name). Match case + accent insensible
 * sur name/nameAscii/nameEN. Retourne NULL si introuvable. */
static const OwlDSLPropInfo* dsl_prop_find(char kindChar, const char* propName) {
    const char* kind = dsl_kind_string(kindChar);
    if (!kind || !propName) return NULL;
    for (int i = 0; i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
        const OwlDSLPropInfo* p = &OWL_DSL_PROP_CATALOG[i];
        if (strcmp(p->kind, kind) != 0) continue;
        if ((p->name      && strcasecmp(propName, p->name)      == 0) ||
            (p->nameAscii && strcasecmp(propName, p->nameAscii) == 0) ||
            (p->nameEN    && strcasecmp(propName, p->nameEN)    == 0))
            return p;
    }
    return NULL;
}

/* Detecte le contexte DSL au curseur en scannant en arriere depuis l'offset.
 * Retourne 'F' (fenetre), 'L' (libelle), 'B' (bouton), 'S' (saisie), ou 0
 * si on n'est pas dans un body DSL.
 *
 * DSL Phase G v2 : pattern post-fixed 'IDENT est un/une Fenetre/Libelle/...
 * On track prev1 = TOKEN_EST pour distinguer declaration (block opener) de
 * propriete ('Libelle = "..."'). */
/* Detecte le contexte DSL courant au cursor.
 *   widget : 'F'/'L'/'B'/'S'/0 — Fenetre/Libelle/Bouton/Saisie/aucun
 *   inProprietes : true si dans 'PROPRIETES ... FIN' du widget courant
 *
 * Quand inProprietes=true, l'autocomplete doit etre STRICTEMENT
 * limite aux props du widget (pas de fonctions natives etc.). */
typedef struct {
    char widget;         /* enclosing widget si on est inside PROPRIETES */
    bool inProprietes;
    bool parentIsGrille; /*  — le parent direct du widget englobant
                          * declare-t-il Disposition = dispGrille ? Sert a
                          * filtrer ColSpan/RowSpan de la completion (sans
                          * effet hors grille). */
    bool saisieIsMultiligne; /*  — le widget Saisie englobant declare-t-il
                          * TypeSaisie = typSaisieMultiligne ? Sert a masquer la
                          * valeur RTF (formatTexteRiche) de FormatAffichage hors
                          * saisie multi-ligne, miroir du filtre designer
                          * (ui_presentation.c visible_if). Defaut Faux (le defaut
                          * typSaisieTexte n'est pas multi-ligne). */
} DslCtx;

/*  — resout le sous-type d'un field `instance` dans un type composé
 * donné. Retourne le subTypeName (ex parent=styleBouton, field=Image →
 * styleImageBouton) ou NULL si le field n'existe pas / n'est pas une instance.
 * Casse-insensible ; couvre le fieldName accentué du catalogue. */
static const char* composed_field_subtype(const char* parentType,
                                          const char* fieldName) {
    if (!parentType || !fieldName) return NULL;
    char fnAscii[64];
    scanner_strip_accents(fieldName, (int)strlen(fieldName),
                          fnAscii, sizeof(fnAscii));
    for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
        const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
        if (strcasecmp(f->typeName, parentType) != 0) continue;
        char catAscii[64];
        scanner_strip_accents(f->fieldName, (int)strlen(f->fieldName),
                              catAscii, sizeof(catAscii));
        if (strcasecmp(f->fieldName, fieldName) != 0 &&
            strcasecmp(catAscii, fnAscii) != 0) continue;
        if (f->subTypeName && strcmp(f->fieldType, "instance") == 0)
            return f->subTypeName;
        return NULL; /* field trouvé mais scalaire/enum : pas de sous-chaine */
    }
    return NULL;
}

/*  — libelle d'un champ de type compose dans la langue active. La
 * forme EN vient du catalogue (OwlField.nameEN propage a la generation) ;
 * fallback FR quand le champ n'a pas d'alias EN declare. */
static const char* composed_field_label(const OwlComposedTypeField* f,
                                        bool wantEN) {
    if (wantEN && f->nameEN && f->nameEN[0]) return f->nameEN;
    return f->fieldName;
}

/*  — un nom ecrit designe-t-il ce champ ? En mode EN, la forme EN est
 * acceptee en plus de la forme FR (le fichier peut melanger les deux). */
static bool composed_field_name_eq(const OwlComposedTypeField* f,
                                   const char* name, int len, bool wantEN) {
    if (!name || len <= 0) return false;
    if ((int)strlen(f->fieldName) == len
        && strncasecmp(name, f->fieldName, (size_t)len) == 0) return true;
    if (wantEN && f->nameEN && (int)strlen(f->nameEN) == len
        && strncasecmp(name, f->nameEN, (size_t)len) == 0) return true;
    return false;
}

/*  — vrai si le token designe le mot-cle de bloc collection
 * `CollectionDeProcedures` (FR canon) / `ProcedureCollection` (EN). Le
 * scanner ne lui donne pas de TOKEN dedie : c'est un TOKEN_IDENTIFIER
 * reconnu ici par comparaison (accent + casse insensibles), miroir de
 * `token_is_collection_kw` cote compilateur. */
static bool lsp_is_collection_kw(const char* start, int length) {
    if (!start || length <= 0) return false;
    char probe[64];
    int n = scanner_strip_accents(start, length, probe, (int)sizeof(probe));
    if (n <= 0 || n >= (int)sizeof(probe)) return false;
    probe[n] = '\0';
    return (n == 22 && strncasecmp(probe, "CollectionDeProcedures", 22) == 0)
        || (n == 19 && strncasecmp(probe, "ProcedureCollection", 19) == 0);
}

static DslCtx detect_dsl_context_ex(const char* source, int cursor_offset) {
    Scanner sc;
    scanner_init(&sc, source);
    char stack[64]; int top = 0;
    /*  — layout par bloc widget : true si le bloc a declare
     * Disposition = dispGrille. Parallele a `stack`, reset a chaque push. */
    bool grille[64]; for (int gi = 0; gi < 64; gi++) grille[gi] = false;
    bool sawDispName = false;   /* token precedent = Disposition/Layout */
    bool awaitDispVal = false;  /* on a vu `Disposition =`, le prochain token
                                 * est la valeur du layout. */
    /*  — multi-ligne par bloc widget : true si le bloc Saisie a declare
     * TypeSaisie = typSaisieMultiligne. Parallele a `stack`, reset a chaque push. */
    bool multiligne[64]; for (int mi = 0; mi < 64; mi++) multiligne[mi] = false;
    bool sawTypeSaisieName = false; /* token precedent = TypeSaisie/InputType */
    bool awaitTypeSaisieVal = false;
    Token prev = (Token){0};

    while (1) {
        Token t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        int tokOff = (int)(t.start - source);
        if (tokOff >= cursor_offset) break;

        bool wasEst = (prev.type == TOKEN_EST);
        int topBefore = top;

        /*  — detection `Disposition/Layout = dispGrille`. La valeur est
         * assignee au bloc widget qui possede le PROPRIETES courant (celui
         * directement sous le 'p' au sommet). */
        if (awaitDispVal) {
            awaitDispVal = false;
            bool isGrid = (t.length == 10 && strncasecmp(t.start, "dispGrille", 10) == 0)
                       || (t.length == 8  && strncasecmp(t.start, "dispGrid", 8) == 0);
            if (isGrid) {
                for (int i = top - 1; i >= 0; i--) {
                    char k = stack[i];
                    if (k == 'p' || k == 'P' || k == 'X') continue;
                    grille[i] = true;
                    break;
                }
            }
        } else if (sawDispName && t.type == TOKEN_EQUAL) {
            awaitDispVal = true;
        }
        sawDispName = (t.type == TOKEN_IDENTIFIER
                       && ((t.length == 11 && strncasecmp(t.start, "Disposition", 11) == 0)
                        || (t.length == 6  && strncasecmp(t.start, "Layout", 6) == 0)));

        /*  — detection `TypeSaisie/InputType = typSaisieMultiligne`. La
         * valeur est assignee au bloc widget qui possede la prop (le premier
         * slot non-structurel sous le sommet, comme Disposition ci-dessus). */
        if (awaitTypeSaisieVal) {
            awaitTypeSaisieVal = false;
            bool isMulti = (t.length == 19 && strncasecmp(t.start, "typSaisieMultiligne", 19) == 0)
                        || (t.length == 18 && strncasecmp(t.start, "typeInputMultiline", 18) == 0);
            if (isMulti) {
                for (int i = top - 1; i >= 0; i--) {
                    char k = stack[i];
                    if (k == 'p' || k == 'P' || k == 'X') continue;
                    multiligne[i] = true;
                    break;
                }
            }
        } else if (sawTypeSaisieName && t.type == TOKEN_EQUAL) {
            awaitTypeSaisieVal = true;
        }
        sawTypeSaisieName = (t.type == TOKEN_IDENTIFIER
                       && ((t.length == 10 && strncasecmp(t.start, "TypeSaisie", 10) == 0)
                        || (t.length == 9  && strncasecmp(t.start, "InputType", 9) == 0)));

        /* Prefixe de declaration de champ (modificateur + filler) : traverse
         * en preservant prev = EST. Cf. lsp_champ_prefix_kind. */
        if (lsp_champ_prefix_kind(t, wasEst) != LSP_CHAMP_PREFIX_NONE) {
            continue;
        }

        if (t.type == TOKEN_FENETRE && wasEst && top < 64) {
            stack[top++] = 'F';
        } else if (t.type == TOKEN_ETAT && wasEst && top < 64) {
            stack[top++] = 'E';
        } else if ((t.type == TOKEN_LIBELLE || t.type == TOKEN_BOUTON
                    || t.type == TOKEN_SAISIE || t.type == TOKEN_TABLE
                    || t.type == TOKEN_CONTENEUR)
                   && wasEst && top < 64) {
            stack[top++] = (t.type == TOKEN_LIBELLE)   ? 'L'
                         : (t.type == TOKEN_BOUTON)    ? 'B'
                         : (t.type == TOKEN_TABLE)     ? 'T'
                         : (t.type == TOKEN_CONTENEUR) ? 'O' : 'S';
        } else if (t.type == TOKEN_LISTE && wasEst && top < 64) {
            /* 'IDENT est un Champ Liste' : le modificateur `Champ` a ete
             * traverse en tete de boucle (prev=EST preserve, cf.
             * lsp_champ_prefix_kind). Sans lui, TOKEN_LISTE = collection
             * (cf compiler.c) — pas un widget, donc on n'empile pas. */
            stack[top++] = 'Z';
        } else if (t.type == TOKEN_COLONNE && wasEst && top < 64) {
            stack[top++] = 'C';
        } else if (t.type == TOKEN_IDENTIFIER && wasEst && top < 64) {
            /* IDENT apres `est un` : un widget DSL ou un sous-bloc Etat ?
             * Source unique : owl_control_kind_from_token + nom canon.
             * Le mapping kind -> char (LSP context stack) est local. */
            OwlControlKind k;
            const OwlControlKindInfo* info = NULL;
            /*  — `IDENT est un Projet` : type composé top-level
             * (pas un widget/OwlControlKind). Char dédié 'J' (proJet). */
            bool isProjet =
                (t.length == 6 && strncasecmp(t.start, "Projet", 6) == 0)
             || (t.length == 7 && strncasecmp(t.start, "Project", 7) == 0);
            char menuCh = menu_block_kind_char(t.start, t.length);
            char svcCh  = service_block_kind_char(t.start, t.length);
            if (menuCh) {
                /*  — bloc menu (is_widget=false) : char dédié pour
                 * rooter `Style..` (M/x) et équilibrer les FIN des sous-blocs
                 * (u/o/r). Sans ça, la résolution `Style` remonte au widget
                 * englobant (Fenetre → styleConteneur). */
                stack[top++] = menuCh;
            } else if (svcCh) {
                /*  — bloc ServiceREST ('s') ou OpérationHTTP ('h') :
                 * char dédié pour router la completion des events (PROCEDURE +
                 * Ctrl+Espace) vers le bon catalogue et équilibrer les FIN. */
                stack[top++] = svcCh;
            } else if (isProjet) {
                stack[top++] = 'J';
            } else if (lsp_is_collection_kw(t.start, t.length)) {
                /*  — bloc CollectionDeProcedures : char dedie 'G'. */
                stack[top++] = 'G';
            } else if (owl_control_kind_from_token(t.start, t.length, &k, &info)) {
                /* Widget runtime — push son char via la table unique. */
                char ch = dsl_char_for_kind(k);
                if (ch) stack[top++] = ch;
            } else if (dsl_block_open_char(t.type, t.start, t.length) == 'V') {
                /*  — sous-bloc `VOL_X est un Volet` d'un Champ Onglet :
                 * is_widget=false cote runtime, donc rejete par
                 * owl_control_kind_from_token. Il n'etait donc pas empile,
                 * alors que son FIN, lui, depilait : le Champ Onglet englobant
                 * sautait de la pile et tout ce qui suivait le premier Volet
                 * etait attribue a la Fenetre (completion des events apres
                 * `PROCEDURE ` -> catalogue Fenetre au lieu d'Onglet). */
                stack[top++] = 'V';
            } else if (info && !info->is_widget) {
                /* Sous-bloc DSL Etat (Iteration, Corps, ...). */
                stack[top++] = 'e';
            } else {
                /* Test sous-bloc Etat sans passer par CONTROL_KINDS (le
                 * helper ne retourne pas info pour les non-widgets sans
                 * match canon — au cas ou). */
                int klen = t.length;
                if (klen > 0 && klen < 32) {
                    char kindBuf[32];
                    memcpy(kindBuf, t.start, klen);
                    kindBuf[klen] = '\0';
                    if (is_etat_block_type_name(kindBuf)) {
                        stack[top++] = 'e';
                    }
                }
            }
        } else if (t.type == TOKEN_PROPRIETES && top < 64) {
            /* DSL v2 : bloc statique des props. */
            stack[top++] = 'p';
        } else if (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION) {
            if (top < 64) stack[top++] = 'P';
        } else if (t.type == TOKEN_FIN && top > 0) {
            top--;
        } else if (top > 0 && t.type == TOKEN_AVEC) {
            if (top < 64) stack[top++] = 'X';
        } else if (top > 0 && lsp_depth_table_knows(t)
                   && lsp_depth_delta(t) > 0) {
            if (top < 64) stack[top++] = 'X';
        } else if (wasEst && (t.type == TOKEN_CLASSE || t.type == TOKEN_STRUCTURE
                            || t.type == TOKEN_INTERFACE
                            || t.type == TOKEN_ENUMERATION
                            || t.type == TOKEN_COMBINAISON)) {
            if (top < 64) stack[top++] = 'X';
        }

        /*  — tout slot fraichement pousse demarre sans layout grille
         * (evite qu'une valeur GRILLE d'un bloc frere ferme fuite via l'index
         * reutilise). */
        for (int s = topBefore; s < top; s++) grille[s] = false;
        /*  — idem : un slot neuf demarre non-multi-ligne. */
        for (int s = topBefore; s < top; s++) multiligne[s] = false;

        /*  — article EN transparent : `IDENT is a <Type>` garde
         * prev=EST pour que <Type> declenche wasEst au tour suivant. */
        if (tok_is_en_article(t) && prev.type == TOKEN_EST) {
            continue;
        }

        prev = t;
    }

    DslCtx out = { 0, false };
    /* Si top = 'p' → on est dans un bloc PROPRIETES. L'enclosing widget est
     * le premier element en dessous qui porte un genre du catalogue de props
     * (dsl_kind_char_has_props, ) — les chars structurels ('X', 'P',
     * 'G') sont traverses.
     * Special-case : un Libellé inside un bloc Etat ('e' below 'L') a son
     * propre kind LibelleEtat ('l') avec PROPS_LIBELLE_ETAT (X/Y/Police/
     * Alignement, pas Bulle/CadrageH/Etat etc.). */
    if (top > 0 && stack[top-1] == 'p') {
        out.inProprietes = true;
        for (int i = top - 2; i >= 0; i--) {
            char k = stack[i];
            if (dsl_kind_char_has_props(k)) {
                out.widget = k;
                out.saisieIsMultiligne = multiligne[i]; /*  */
                /*  — layout du parent direct du widget englobant : le
                 * bloc widget/conteneur immediatement en dessous (les chars
                 * structurels p/P/X sont traverses). Sert au filtre
                 * ColSpan/RowSpan (sans effet hors dispGrille). */
                for (int pj = i - 1; pj >= 0; pj--) {
                    char pk = stack[pj];
                    if (pk == 'p' || pk == 'P' || pk == 'X') continue;
                    out.parentIsGrille = grille[pj];
                    break;
                }
                /* Libellé inside Etat block ? Cherche un E/e plus bas. */
                if (k == 'L') {
                    for (int j = i - 1; j >= 0; j--) {
                        if (stack[j] == 'E' || stack[j] == 'e') {
                            out.widget = 'l';
                            break;
                        }
                        if (stack[j] == 'F') break;  /* dans une Fenetre, pas un Etat */
                    }
                } else if (k == 'I') {
                    /* Image inside Etat block ? Promote -> ImageEtat ('m'). */
                    for (int j = i - 1; j >= 0; j--) {
                        if (stack[j] == 'E' || stack[j] == 'e') {
                            out.widget = 'm';
                            break;
                        }
                        if (stack[j] == 'F') break;
                    }
                } else if (k == 'D') {
                    /* CodeBarres inside Etat block ? Promote -> CodeBarresEtat ('d'). */
                    for (int j = i - 1; j >= 0; j--) {
                        if (stack[j] == 'E' || stack[j] == 'e') {
                            out.widget = 'd';
                            break;
                        }
                        if (stack[j] == 'F') break;
                    }
                }
                break;
            }
        }
        return out;
    }
    /* Sinon : top = widget DSL ou autre. Skip P/X. */
    while (top > 0 && (stack[top-1] == 'X' || stack[top-1] == 'P')) top--;
    out.widget = top > 0 ? stack[top-1] : 0;
    out.saisieIsMultiligne = (top > 0) ? multiligne[top-1] : false; /*  */
    /* Promote Libellé to LibelleEtat outside PROPRIETES too (pour completion
     * apres un point sur LIB_X qui vit dans un Etat). */
    if (out.widget == 'L') {
        for (int i = top - 2; i >= 0; i--) {
            if (stack[i] == 'E' || stack[i] == 'e') { out.widget = 'l'; break; }
            if (stack[i] == 'F') break;
        }
    } else if (out.widget == 'I') {
        /* Idem pour Image -> ImageEtat. */
        for (int i = top - 2; i >= 0; i--) {
            if (stack[i] == 'E' || stack[i] == 'e') { out.widget = 'm'; break; }
            if (stack[i] == 'F') break;
        }
    } else if (out.widget == 'D') {
        /* Idem pour CodeBarres -> CodeBarresEtat. */
        for (int i = top - 2; i >= 0; i--) {
            if (stack[i] == 'E' || stack[i] == 'e') { out.widget = 'd'; break; }
            if (stack[i] == 'F') break;
        }
    }
    return out;
}

/* Wrapper legacy : retourne juste le widget. Garde l'API existante pour
 * les call-sites qui n'ont pas besoin du flag inProprietes. */
__attribute__((unused))
static char detect_dsl_context(const char* source, int cursor_offset) {
    return detect_dsl_context_ex(source, cursor_offset).widget;
}

/*  — offset (octet) de la declaration `IDENT est une Fenetre/Etat`
 * qui englobe `cursor_offset`, ou -1 si le point est hors de toute fenetre
 * (top-level fichier). Mirroir des ouvertures/fermetures de detect_dsl_context_ex
 * (equilibrage strict des FIN) ; on ne retient que le flag "fenetre" + l'offset
 * d'ouverture de chaque bloc. Sert a SCOPER les globales top-level d'une
 * fenetre proposees en RHS de prop : deux fenetres du meme fichier sont des
 * blocs freres (jamais imbriques), donc une globale dont la fenetre englobante
 * differe de celle du curseur n'est pas offerte (elle serait un faux candidat —
 * le runtime la mangle FEN_X$var, resoluble seulement dans FEN_X). */
static int enclosing_window_start(const char* source, int cursor_offset) {
    Scanner sc;
    scanner_init(&sc, source);
    struct { int off; bool win; } stack[64];
    int top = 0;
    Token prev = (Token){0};
    while (1) {
        Token t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        int tokOff = (int)(t.start - source);
        if (tokOff >= cursor_offset) break;
        bool wasEst = (prev.type == TOKEN_EST);
        bool opens = false, isWin = false;

        /* Prefixe de declaration de champ : traverse (rien a empiler, la
         * declaration s'ouvre sur le genre qui suit). Cf.
         * lsp_champ_prefix_kind. */
        if (lsp_champ_prefix_kind(t, wasEst) != LSP_CHAMP_PREFIX_NONE) {
            continue;
        }

        if ((t.type == TOKEN_FENETRE || t.type == TOKEN_ETAT) && wasEst) {
            opens = true; isWin = true;
        } else if ((t.type == TOKEN_LIBELLE || t.type == TOKEN_BOUTON
                    || t.type == TOKEN_SAISIE || t.type == TOKEN_TABLE
                    || t.type == TOKEN_CONTENEUR) && wasEst) {
            opens = true;
        } else if (t.type == TOKEN_LISTE && wasEst) {
            opens = true;
        } else if (t.type == TOKEN_COLONNE && wasEst) {
            opens = true;
        } else if (t.type == TOKEN_IDENTIFIER && wasEst) {
            OwlControlKind k;
            const OwlControlKindInfo* info = NULL;
            bool isProjet =
                (t.length == 6 && strncasecmp(t.start, "Projet", 6) == 0)
             || (t.length == 7 && strncasecmp(t.start, "Project", 7) == 0);
            if (isProjet) {
                opens = true;
            } else if (owl_control_kind_from_token(t.start, t.length, &k, &info)) {
                opens = true;
            } else if (info && !info->is_widget) {
                opens = true;
            } else {
                int klen = t.length;
                if (klen > 0 && klen < 32) {
                    char kb[32];
                    memcpy(kb, t.start, klen);
                    kb[klen] = '\0';
                    if (is_etat_block_type_name(kb)) opens = true;
                }
            }
        } else if (t.type == TOKEN_PROPRIETES) {
            opens = true;
        } else if (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION) {
            opens = true;
        } else if (top > 0 && t.type == TOKEN_AVEC) {
            opens = true;
        } else if (top > 0 && lsp_depth_table_knows(t)
                   && lsp_depth_delta(t) > 0) {
            opens = true;
        } else if (wasEst && (t.type == TOKEN_CLASSE || t.type == TOKEN_STRUCTURE
                            || t.type == TOKEN_INTERFACE
                            || t.type == TOKEN_ENUMERATION
                            || t.type == TOKEN_COMBINAISON)) {
            opens = true;
        } else if (t.type == TOKEN_FIN && top > 0) {
            top--;
        }

        if (opens && top < 64) {
            stack[top].off = tokOff;
            stack[top].win = isWin;
            top++;
        }

        if (tok_is_en_article(t) && prev.type == TOKEN_EST) continue;
        prev = t;
    }
    for (int i = top - 1; i >= 0; i--) {
        if (stack[i].win) return stack[i].off;
    }
    return -1;
}

/* Vrai si 'prop' apparait deja comme 'prop = ' dans le body courant. */
static bool prop_already_used(const char* source, int cursor_offset,
                                int dsl_block_start, const char* prop) {
    /* Recherche brute : "\n[ws]<prop>[ws]=" dans la fenetre [start, cursor]. */
    int plen = (int)strlen(prop);
    for (int i = dsl_block_start; i < cursor_offset; i++) {
        /* Match start of statement (after newline or block start) */
        const char* p = source + i;
        if (i > 0 && source[i-1] != '\n' && source[i-1] != '\r') continue;
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (strncasecmp(q, prop, plen) != 0) continue;
        const char* afterProp = q + plen;
        while (*afterProp == ' ' || *afterProp == '\t') afterProp++;
        if (*afterProp == '=') return true;
    }
    return false;
}

/*  — Sniff léger : vrai si `src` débute par une declaration
 * de Classe au top-level (motif canon `<Ident> est une Classe` ou
 * alias EN `<Ident> is a Class`). On scanne les premières lignes
 * utiles (skip commentaires `//` et lignes vides). Une déclaration
 * de Classe est obligatoirement au top-level (col 0), seule au
 * fichier (convention règle 5 OWL : 1 fichier = 1 classe). */
static bool is_class_file_source(const char* src) {
    if (!src) return false;
    const char* p = src;
    int linesScanned = 0;
    while (*p && linesScanned < 50) {
        /* Skip whitespace en début de ligne. */
        const char* lineStart = p;
        while (*p == ' ' || *p == '\t') p++;
        /* Ligne vide ou commentaire ligne `//` : skip. */
        if (*p == '\n' || *p == '\r' || *p == '\0') {
            if (*p == '\r') p++;
            if (*p == '\n') p++;
            linesScanned++;
            continue;
        }
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            linesScanned++;
            continue;
        }
        /* Ligne utile : la declaration de classe doit etre en col 0
         * (top-level). Si on est indenté → ce n'est pas un body classe
         * (autre construction, on abandonne). */
        if (p != lineStart) return false;
        /* Skip identifier. */
        const char* idStart = p;
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9') || *p == '_'
            || (unsigned char)*p >= 0x80) p++;
        if (p == idStart) return false;
        /* Skip ws. */
        while (*p == ' ' || *p == '\t') p++;
        /*  — copule+article bilingue (`est un[e]` FR / `is a[n]`
         * EN / formes mixtes `is une`, `est a`) via la source de verite
         * commune, puis nom du type bilingue `Classe`/`Class`. */
        int off = scan_copule_article_bytes(p, 0);
        if (off >= 0) {
            const char* q = p + off;
            if (strncasecmp(q, "Classe", 6) == 0) {
                char after = q[6];
                if (after == ' ' || after == '\t' || after == '\r'
                    || after == '\n' || after == ',' || after == '\0')
                    return true;
            } else if (strncasecmp(q, "Class", 5) == 0) {
                char after = q[5];
                if (after == ' ' || after == '\t' || after == '\r'
                    || after == '\n' || after == ',' || after == '\0')
                    return true;
            }
        }
        /* Première ligne utile et ce n'est pas une Classe → ce n'est
         * pas un fichier classe. */
        return false;
    }
    return false;
}

/*  — vrai si le prochain caractere non-blanc apres `cursor_offset`,
 * sur la MEME ligne, est `(`. Sert a detecter que la structure parenthesee
 * (typiquement posee par le snippet keyword PROCEDURE) existe deja, et
 * qu'un snippet event qui re-poserait `()...FIN` produirait un double FIN.
 * Si on rencontre `\n` ou EOF avant un non-blanc, retourne false. */
static bool lsp_paren_follows(const char* source, int cursor_offset) {
    if (!source) return false;
    const char* p = source + cursor_offset;
    while (*p == ' ' || *p == '\t') p++;
    return *p == '(';
}

/* Vrai si le nom `name` apparait derriere un PROCEDURE/FONCTION dans la
 * source a l'offset `q` avec une word boundary correcte. Helper interne
 * de event_already_declared. */
static bool lsp_proc_name_matches(const char* q, const char* name) {
    if (!name || !*name) return false;
    int len = (int)strlen(name);
    if (strncasecmp(q, name, len) != 0) return false;
    char after = q[len];
    return !((after >= 'a' && after <= 'z') ||
             (after >= 'A' && after <= 'Z') ||
             (after >= '0' && after <= '9') ||
             after == '_' || (unsigned char)after >= 0x80);
}

/* Vrai si 'PROCEDURE <evt>' apparait deja dans la zone [block_start, block_end).
 *  — scope etendu : on scanne TOUT le bloc widget (jusqu'a sa `FIN`),
 * pas seulement jusqu'au curseur. Sinon un event declare APRES le curseur dans
 * le meme widget n'est pas detecte → re-propose a tort.
 *
 *  — bilingue : un event a deux formes canon (FR `evt` et alias EN
 * `alias`). En mode EN le snippet de completion insere la forme EN ; le
 * filtre doit donc reconnaitre l'event qu'il ait ete declare en FR OU en EN,
 * quelle que soit la langue active. On matche la forme FR OU l'alias EN
 * (peut etre NULL/identique). Sans ca, une proc declaree en EN restait
 * invisible au filtre et donc reproposee (double declaration possible).
 *
 * Le parametre `cursor_offset` n'est plus utilise pour borner le scan mais
 * conserve pour future heuristique (eg. ignorer la PROCEDURE pile au curseur
 * si jamais l'user a deja tape un debut de nom). Pour l'instant le boundary
 * wordEnd suffit a eviter les faux positifs sur `PROCEDURE \n` (pas de nom). */
static bool event_already_declared(const char* source, int cursor_offset,
                                     int block_start, int block_end,
                                     const char* evt, const char* alias) {
    (void)cursor_offset;
    for (int i = block_start; i < block_end; i++) {
        if (i > 0 && source[i-1] != '\n' && source[i-1] != '\r') continue;
        const char* p = source + i;
        while (*p == ' ' || *p == '\t') p++;
        /* Skip 'PROCEDURE' / 'FONCTION' / 'FUNCTION'. */
        const char* kw = p;
        while ((*kw >= 'a' && *kw <= 'z') || (*kw >= 'A' && *kw <= 'Z')) kw++;
        int kwLen = (int)(kw - p);
        bool isProc = (kwLen == 9 && strncasecmp(p, "PROCEDURE", 9) == 0)
                   || (kwLen == 8 && strncasecmp(p, "FONCTION", 8) == 0)
                   || (kwLen == 8 && strncasecmp(p, "FUNCTION", 8) == 0);
        if (!isProc) continue;
        const char* q = kw;
        while (*q == ' ' || *q == '\t') q++;
        if (lsp_proc_name_matches(q, evt)) return true;
        if (lsp_proc_name_matches(q, alias)) return true;
    }
    return false;
}

/* Trouve l'offset du debut du bloc DSL le plus proche en remontant.
 *
 * `block_start` doit pointer sur l'offset du token IDENT qui DECLARE
 * le sous-controle (`IDENT` dans `IDENT est un [Champ] X`). On garde
 * `lastIdentOff` (offset du dernier identifiant rencontre) puis on
 * commit dans `lastBlockStart` quand on detecte le pattern complet.
 * Avant  on stockait `off` du token "X" (Fenetre/Image/...) :
 * find_dsl_block_end re-scannait a partir de la et ratait le `est un`
 * — depthSeeded restait faux, scope = src_len. */
static int find_dsl_block_start(const char* source, int cursor_offset, char ctxKind) {
    Scanner sc;
    scanner_init(&sc, source);
    int lastBlockStart = 0;
    int lastIdentOff = -1;
    Token prev = (Token){0};
    while (1) {
        Token t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        int off = (int)(t.start - source);
        if (off >= cursor_offset) break;
        bool wasEst = (prev.type == TOKEN_EST);
        /*  — article EN transparent : `IDENT is a <Type>`. On ne
         * touche ni lastIdentOff ni prev pour que <Type> voie wasEst. */
        if (wasEst && tok_is_en_article(t)) {
            continue;
        }
        /* Prefixe de declaration de champ (modificateur + filler) : traverse
         * sans toucher ni a `lastIdentOff` (le `Champ` n'est pas l'IDENT du
         * sous-controle) ni a `prev`. Cf. lsp_champ_prefix_kind. */
        if (lsp_champ_prefix_kind(t, wasEst) != LSP_CHAMP_PREFIX_NONE) {
            continue;
        }
        if (wasEst &&
            ((ctxKind == 'F' && t.type == TOKEN_FENETRE)
          || (ctxKind == 'L' && t.type == TOKEN_LIBELLE)
          || (ctxKind == 'l' && t.type == TOKEN_LIBELLE)  /*  — LibelleEtat */
          || (ctxKind == 'B' && t.type == TOKEN_BOUTON)
          || (ctxKind == 'S' && t.type == TOKEN_SAISIE)
          || (ctxKind == 'T' && t.type == TOKEN_TABLE)
          || (ctxKind == 'C' && t.type == TOKEN_COLONNE)
          || (ctxKind == 'E' && t.type == TOKEN_ETAT))) {
            lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
        }
        /*  — genres sans token scanner dedie (Onglet, Volet, Combo,
         * Selecteur, Interrupteur, ZoneRepetee, Jauge, ...) : reconnus par le
         * catalogue via dsl_block_open_char. Sans ca, ctxKind='N' ne matchait
         * aucune ouverture -> lastBlockStart restait 0 et le filtre
         * "event deja declare" scannait toute la fenetre : un
         * `PROCEDURE surChangementVolet()` pose dans ONG_A supprimait la
         * proposition dans ONG_B (liste de completion vide). */
        if (wasEst && dsl_block_open_char(t.type, t.start, t.length) == ctxKind) {
            lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
        }
        /*  — ImageEtat ('m') : `IDENT est un Image`. Image n'a pas
         * de TOKEN dedie (cf. scanner) → reconnu comme TOKEN_IDENTIFIER
         * "Image". Le bornage du sous-controle Image dans un Etat reposait
         * sur block_start=0 (faux) → toute la chaine event_already_declared
         * scannait le fichier entier.
         *
         *  — la forme reelle est `est un Champ Image` (le prefixe
         * `Champ` est traite ci-dessus). Ici on accepte les deux. */
        if (wasEst && ctxKind == 'm' && t.type == TOKEN_IDENTIFIER) {
            if (t.length == 5 && strncasecmp(t.start, "Image", 5) == 0) {
                lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
            }
        }
        /* CodeBarresEtat ('d') : `IDENT est un CodeBarres`. Comme Image,
         * CodeBarres n'a pas de TOKEN dedie -> TOKEN_IDENTIFIER "CodeBarres".
         * Forme reelle `est un Champ CodeBarres` (prefixe Champ traite plus haut). */
        if (wasEst && ctxKind == 'd' && t.type == TOKEN_IDENTIFIER) {
            if (t.length == 10 && strncasecmp(t.start, "CodeBarres", 10) == 0) {
                lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
            }
        }
        /*  — bloc Projet : `IDENT est un Projet`. Projet n'a pas de
         * TOKEN dedie (TOKEN_IDENTIFIER "Projet"/"Project"). */
        if (wasEst && ctxKind == 'J' && t.type == TOKEN_IDENTIFIER) {
            if ((t.length == 6 && strncasecmp(t.start, "Projet", 6) == 0)
             || (t.length == 7 && strncasecmp(t.start, "Project", 7) == 0)) {
                lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
            }
        }
        /*  — bloc CollectionDeProcedures : `IDENT est une
         * CollectionDeProcedures` (pas de TOKEN dedie, cf. lsp_is_collection_kw). */
        if (wasEst && ctxKind == 'G' && t.type == TOKEN_IDENTIFIER) {
            if (lsp_is_collection_kw(t.start, t.length)) {
                lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
            }
        }
        /*  — bloc ServiceREST ('s') / OpérationHTTP ('h') : `IDENT est
         * un ServiceREST` / `IDENT est une OpérationHTTP` (types declares comme
         * IDENT, pas de TOKEN dedie). Bornage du bloc pour filtrer les events
         * deja declares (completion non-encore-utilises). */
        if (wasEst && (ctxKind == 's' || ctxKind == 'h')
            && t.type == TOKEN_IDENTIFIER
            && service_block_kind_char(t.start, t.length) == ctxKind) {
            lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
        }
        /* DSL Etat V2 sub-bloc : 'IDENT est un EnTeteDocument/Corps/...' */
        if (wasEst && ctxKind == 'e' && t.type == TOKEN_IDENTIFIER) {
            int klen = t.length;
            if (klen > 0 && klen < 32) {
                char kindBuf[32];
                memcpy(kindBuf, t.start, klen);
                kindBuf[klen] = '\0';
                if (is_etat_block_type_name(kindBuf)) {
                    lastBlockStart = (lastIdentOff >= 0) ? lastIdentOff : off;
                }
            }
        }
        /* Memorise l'IDENT courant pour usage en tant que `lastIdentOff`
         * (debut du `IDENT est un ...`) ssi le prochain token est EST. On
         * met a jour pour tout IDENT en debut potentiel de declaration
         * (prev=EOL/SEMICOLON ou token-de-bloc). En pratique : tout
         * IDENT qui n'est PAS un IDENT-de-type post-EST. */
        if (t.type == TOKEN_IDENTIFIER && !wasEst) {
            lastIdentOff = off;
        } else if (t.type != TOKEN_EST) {
            /* Autre token (EST, FIN, ponctuation, keyword, etc.) : on
             * invalide l'IDENT memorise pour eviter de pointer sur un
             * IDENT obsolete d'une declaration anterieure. */
            lastIdentOff = -1;
        }
        prev = t;
    }
    return lastBlockStart;
}

/*  — trouve l'offset de la `FIN` qui ferme le bloc widget ouvert a
 * `block_start`. Matching de profondeur : on incremente sur les ouvertures
 * (PROCEDURE/FONCTION/PROPRIETES, sous-bloc `est un <X>`, structures de
 * controle SI/POUR/TANTQUE/BOUCLE/SELON/AVEC/QUAND) et on decremente sur
 * TOKEN_FIN. La premiere FIN qui ramene depth a 0 est la FIN du widget.
 *
 * `block_start` pointe sur le debut de ligne d'une declaration `IDENT est
 * un <Widget>` (renvoye par find_dsl_block_start) ou 0 (cas classe/source
 * entier). Le scan demarre a block_start ; la profondeur initiale est 0
 * et passe a 1 quand on voit le premier `est un <Widget>`.
 *
 * Retourne :
 *   - L'offset du token FIN qui ferme le widget (exclus du scan event).
 *   - source_len si pas trouve (source malforme / pas de FIN) → permet a
 *     event_already_declared de scanner jusqu'a EOF sans deborder.
 *
 *  — `cursor_offset` : position du curseur user (passe par
 * handle_completion). Permet d'ignorer le TOKEN_PROCEDURE/FONCTION qui
 * est PILE au curseur (cas `PROCEDURE \n` non encore complete par l'user)
 * → sans ca, l'ouverture incomplete avale le FIN du widget parent et le
 * scope deborde sur le frere suivant. */
static int find_dsl_block_end(const char* source, int block_start, int cursor_offset) {
    if (!source) return 0;
    int src_len = (int)strlen(source);
    if (block_start < 0 || block_start >= src_len) return src_len;
    Scanner sc;
    scanner_init(&sc, source + block_start);
    int depth = 0;
    bool depthSeeded = false; /* true des qu'on a vu le `est un Widget` ouvrant */
    /*  — vrai quand le modificateur `Champ`/`Field` vient d'etre
     * traverse : la declaration en cours est forcement un bloc widget (donc
     * terminee par FIN), et pas une variable dont le type porte le meme nom
     * qu'un genre (`img est une Image`, `l est une Liste`). */
    bool sawChampPrefix = false;
    Token prev = (Token){0};
    while (1) {
        Token t = scanner_scan_token(&sc);
        if (t.type == TOKEN_EOF) break;
        int off = block_start + (int)(t.start - (source + block_start));
        bool wasEst = (prev.type == TOKEN_EST);
        /*  — article EN transparent : ne pas update prev pour que
         * le token de bloc voie wasEst. */
        if (wasEst && tok_is_en_article(t)) {
            continue;
        }
        /*  — PROCEDURE/FONCTION incomplete au curseur : on est sur
         * la ligne `PROCEDURE ` que l'user tape, pas de nom ni `()` encore.
         * Si on l'incremente, son FIN attendu n'existe pas et on consomme
         * a tort le FIN du widget englobant → scope deborde sur le frere
         * suivant (faux positif event_already_declared). On detecte ca via
         * `off <= cursor_offset` ET token suivant != IDENTIFIER (= pas de
         * nom de procedure encore tape). */
        bool isProcAtCursor = false;
        if ((t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION)
            && off <= cursor_offset) {
            /* Peek le prochain token significatif (skip NEWLINE). Une
             * PROCEDURE complete est `PROCEDURE Nom(...)` → next = IDENT
             * suivi de `(`. Une PROCEDURE incomplete (en train d'etre
             * tapee par l'user) n'a pas de nom encore ou n'a pas de `(` :
             * next sera NEWLINE-puis-FIN, ou IDENT-puis-EST (= IDENT de
             * la declaration suivante, pas le nom de la proc). */
            Scanner peek = sc;
            Token nx;
            do { nx = scanner_scan_token(&peek); }
            while (nx.type == TOKEN_NEWLINE);
            if (nx.type == TOKEN_EOF || nx.type == TOKEN_FIN
                || nx.type == TOKEN_PROCEDURE || nx.type == TOKEN_FONCTION) {
                isProcAtCursor = true;
            } else if (nx.type == TOKEN_IDENTIFIER) {
                Token nx2;
                do { nx2 = scanner_scan_token(&peek); }
                while (nx2.type == TOKEN_NEWLINE);
                /* Si suivi de `est`, c'est une declaration de la ligne
                 * suivante → PROCEDURE incomplete. Si suivi de `(`,
                 * c'est une PROCEDURE complete avec nom. */
                if (nx2.type == TOKEN_EST) {
                    isProcAtCursor = true;
                }
            }
        }
        /* Prefixe de declaration de champ (modificateur + filler) : traverse
         * sans update de `prev`, cf. lsp_champ_prefix_kind. Le modificateur
         * arme en plus `sawChampPrefix` — c'est la preuve qu'on ouvre un bloc
         * widget (donc terminee par FIN) et pas une variable dont le type
         * porte le nom d'un genre (`img est une Image`). Le filler ne l'arme
         * pas mais son `continue` le preserve (pas de reset de fin de boucle). */
        LspChampPrefix champPrefix = lsp_champ_prefix_kind(t, wasEst);
        if (champPrefix != LSP_CHAMP_PREFIX_NONE) {
            if (champPrefix == LSP_CHAMP_PREFIX_MODIFIER) sawChampPrefix = true;
            continue;
        }
        /* Ouverture widget (top-level pour ce bloc ou sous-widget imbrique).
         *  — dsl_block_open_char couvre en plus les genres sans token
         * scanner dedie (Onglet, Combo, Selecteur, ...) quand le modificateur
         * `Champ` garantit qu'il s'agit d'un bloc, plus le sous-bloc Volet qui
         * n'a jamais de prefixe. Sans eux, un bloc borne sur `ONG_X est un
         * Champ Onglet` ne seedait jamais la profondeur (retour = EOF) et le
         * FIN de son sous-bloc Volet fermait le bloc trop tot. */
        char blockCh = dsl_block_open_char(t.type, t.start, t.length);
        bool isWidgetOpen = wasEst && (
               t.type == TOKEN_FENETRE  || t.type == TOKEN_LIBELLE
            || t.type == TOKEN_BOUTON   || t.type == TOKEN_SAISIE
            || t.type == TOKEN_TABLE    || t.type == TOKEN_COLONNE
            || t.type == TOKEN_ETAT
            || (blockCh != 0 && (sawChampPrefix || blockCh == 'V')));
        /* Sous-bloc DSL Etat (Iteration/Corps/...) reconnu par mot-cle ident.
         *  — `Image` ajoute : sous-controle widget d'un Etat sans
         * TOKEN dedie (lexique : IDENTIFIER). Sans ca, depthSeeded restait
         * faux quand block_start pointait sur `IMG_X est un Image`, et la
         * fonction renvoyait src_len → scope event_already_declared = EOF. */
        bool isEtatSubOpen = false;
        if (wasEst && t.type == TOKEN_IDENTIFIER && t.length > 0 && t.length < 32) {
            char buf[32];
            memcpy(buf, t.start, t.length);
            buf[t.length] = '\0';
            if (is_etat_block_type_name(buf) ||
                strcasecmp(buf, "Image") == 0) {
                isEtatSubOpen = true;
            }
        }
        /*  — ouverture d'un bloc CollectionDeProcedures. */
        bool isCollectionOpen = wasEst && t.type == TOKEN_IDENTIFIER
            && lsp_is_collection_kw(t.start, t.length);
        /*  — ouverture d'un bloc ServiceREST ou OpérationHTTP. */
        bool isServiceOpen = wasEst && t.type == TOKEN_IDENTIFIER
            && service_block_kind_char(t.start, t.length) != 0;
        if (isWidgetOpen || isEtatSubOpen || isCollectionOpen || isServiceOpen) {
            depth++;
            depthSeeded = true;
        } else if (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION
                || t.type == TOKEN_PROPRIETES) {
            if (depthSeeded && !isProcAtCursor) depth++;
        } else if (t.type == TOKEN_AVEC) {
            if (depthSeeded) depth++;
        } else if (t.type != TOKEN_FIN && lsp_depth_table_knows(t)
                   && lsp_depth_delta(t) > 0) {
            if (depthSeeded) depth++;
        } else if (t.type == TOKEN_FIN) {
            if (depthSeeded) {
                depth--;
                if (depth <= 0) return off;
            }
        }
        sawChampPrefix = false;
        prev = t;
    }
    return src_len;
}

/* Plafond du nombre de valeurs au-dela duquel on retombe sur un
 * placeholder editable plutot qu'un dropdown. VSCode affiche les
 * choices snippet `${N|v1,v2|}` dans un menu deroulant ; au-dela de
 * ~10 entrees ca devient un scroll inutilisable. Les enums tres
 * larges (couleurs predef, ~30 valeurs) restent en placeholder. */
#define SNIPPET_ENUM_CHOICES_MAX 10

/* Construit l'insertText snippet pour une native multi-overload.
 * Choisit la signature avec le plus petit paramCount (= params obli-
 * gatoires uniquement). L'utilisateur peut taper une virgule pour
 * etendre vers un overload plus long ; signatureHelp prend le relais
 * pour montrer les variantes.
 *
 * Format LSP InsertTextFormat.Snippet (2) :
 *   - `${N:nom}`         pour un param libre — placeholder editable
 *   - `${N|v1,v2,v3|}`   pour un param contraint a un enum non-
 *     combinable de <=10 valeurs — VSCode affiche un dropdown au
 *     moment ou le curseur arrive sur le tabstop. Plus jamais de
 *     nom fictif a effacer pour saisir la vraie valeur enum.
 *
 * Les enums combinables (cf iCadreArrondi avec `+`) restent en
 * placeholder car le dropdown VSCode ne supporte pas la composition.
 *
 * `displayName` peut etre fn->name (FR) ou fn->nameEN (EN alias), permet
 * de reutiliser le helper pour les 2 items de completion. */
/* Variante générale : émet le snippet en sautant les `startParam` premiers
 * params ( — forme chaînée `recv.Méthode(...)` : startParam=1 pour
 * droper le receveur tout en GARDANT les params restants avec leurs vrais
 * noms ; les tabstops sont renumérotés à partir de 1). startParam=0 = forme
 * native classique. */
/*  : nb de params d'une signature portant un enum (allowedBaseEnum) —
 * tie-break pour choisir la forme la plus riche en dropdowns. */
static int sig_enum_count(const OwlNativeSigInfo* sig) {
    int n = 0;
    for (int p = 0; p < sig->paramCount; p++) {
        if (sig->params[p].allowedBaseEnum) n++;
    }
    return n;
}

/* Resout la forme d'affichage d'une valeur d'enum selon la langue courante :
 * en mode EN, remonte la forme EN canon du membre via OWL_CONST_CATALOG (meme
 * mecanisme que emit_enum_value_item), sinon rend la forme brute (FR canon).
 * `values` de OwlEnumInfo ne stocke que la forme FR — sans cette resolution le
 * snippet inserait enumFenetre en mode EN (support #299). */
static const char* snippet_enum_value_label(const char* valueName, bool wantEN) {
    if (!wantEN || !valueName) return valueName;
    for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
        if (strcasecmp(OWL_CONST_CATALOG[k].name, valueName) == 0) {
            if (OWL_CONST_CATALOG[k].nameEN && *OWL_CONST_CATALOG[k].nameEN)
                return OWL_CONST_CATALOG[k].nameEN;
            break;
        }
    }
    return valueName;
}

static void build_native_snippet_skip(char* out, size_t outSize,
                                       const OwlNativeInfo* fn,
                                       const char* displayName,
                                       int startParam, bool wantEN) {
    if (!fn || fn->signatureCount <= 0) {
        snprintf(out, outSize, "%s()$0", displayName);
        return;
    }
    /*  : forme la plus RICHE = le plus de params (tie-break : le plus
     * d'enums portes). Choisir la plus courte perdait les dropdowns d'enum des
     * params optionnels (ex ToastAffiche : duree/cadrageV/cadrageH). */
    const OwlNativeSigInfo* shortest = &fn->signatures[0];
    int bestEnums = sig_enum_count(shortest);
    for (int s = 1; s < fn->signatureCount; s++) {
        const OwlNativeSigInfo* cand = &fn->signatures[s];
        int candEnums = sig_enum_count(cand);
        if (cand->paramCount > shortest->paramCount ||
            (cand->paramCount == shortest->paramCount && candEnums > bestEnums)) {
            shortest = cand;
            bestEnums = candEnums;
        }
    }
    if (shortest->paramCount <= startParam) {
        snprintf(out, outSize, "%s()$0", displayName);
        return;
    }
    int off = snprintf(out, outSize, "%s(", displayName);
    int slot = 0; /* tabstop renuméroté depuis 1 (indépendant de p) */
    for (int p = startParam; p < shortest->paramCount && off < (int)outSize - 16; p++) {
        if (p > startParam)
            off += snprintf(out + off, outSize - off, ", ");
        slot++;

        const OwlNativeParamInfo* pi = &shortest->params[p];
        const OwlEnumInfo* e = pi->allowedBaseEnum
                               ? owl_enum_info_find(pi->allowedBaseEnum)
                               : NULL;
        bool useChoices = e && e->values && !e->combinable
                          && e->valueCount > 0
                          && e->valueCount <= SNIPPET_ENUM_CHOICES_MAX;
        if (useChoices) {
            off += snprintf(out + off, outSize - off, "${%d|", slot);
            for (const char* const* v = e->values;
                 *v && off < (int)outSize - 32; v++) {
                if (v != e->values)
                    off += snprintf(out + off, outSize - off, ",");
                off += snprintf(out + off, outSize - off, "%s",
                                snippet_enum_value_label(*v, wantEN));
            }
            if (off < (int)outSize - 4)
                off += snprintf(out + off, outSize - off, "|}");
        } else {
            off += snprintf(out + off, outSize - off,
                            "${%d:%s}", slot, pi->name);
        }
    }
    if (off < (int)outSize - 4)
        snprintf(out + off, outSize - off, ")$0");
}

static void build_native_completion_snippet(char* out, size_t outSize,
                                             const OwlNativeInfo* fn,
                                             const char* displayName,
                                             bool wantEN) {
    build_native_snippet_skip(out, outSize, fn, displayName, 0, wantEN);
}

/*  — liste plate des noms de params (séparés ", ") en sautant les
 * `startParam` premiers, pour le `detail` de complétion et le hover. Ex:
 * `Cherche(tableau, valeur)` skip 1 -> "valeur". Vide si aucun param
 * restant. */
static void build_native_plain_params(char* out, size_t outSize,
                                       const OwlNativeInfo* fn,
                                       int startParam) {
    out[0] = '\0';
    if (!fn || fn->signatureCount <= 0) return;
    const OwlNativeSigInfo* shortest = &fn->signatures[0];
    for (int s = 1; s < fn->signatureCount; s++)
        if (fn->signatures[s].paramCount < shortest->paramCount)
            shortest = &fn->signatures[s];
    int off = 0;
    for (int p = startParam; p < shortest->paramCount && off < (int)outSize - 8; p++) {
        if (p > startParam)
            off += snprintf(out + off, outSize - off, ", ");
        off += snprintf(out + off, outSize - off, "%s", shortest->params[p].name);
    }
}

/* === POUR TOUTE POSITION ... AVEC ... context detection ==========
 *
 * Le statement « POUR TOUTE POSITION <var> DE <motif> DANS <chaine>
 * [AVEC <options>] » accepte un mask combinable d'optionsRechercheChaine.
 * Le mot-cle AVEC est ici contextuel (pas un bloc AVEC..FIN), et n'est
 * lie a aucune native — donc le LSP doit detecter l'endroit
 * specifiquement pour proposer les bonnes constantes.
 *
 * Syntaxe mono-ligne (multi-ligne pas autorise pour cette forme),
 * donc on scanne la ligne courante uniquement.
 */
typedef enum {
    POS_CTX_NONE,
    POS_CTX_AVEC,    /* curseur dans le mask AVEC (ou apres +/| en chain) */
    POS_CTX_SUFFIX   /* curseur apres l'expr DANS, AVEC pas encore tape */
} PositionStmtCtx;

/* Verifie qu'a la position 'at' commence un token mot-cle de longueur 'len'
 * (matching ASCII case-insensitive) suivi d'un separateur (espace/EOL/...). */
static bool kw_match_at(const char* lp, int at, int lineEnd, const char* kw, int len) {
    if (at + len > lineEnd) return false;
    if (strncasecmp(lp + at, kw, len) != 0) return false;
    /* Le suivant doit ne pas etre un caractere d'identifiant */
    if (at + len < lineEnd) {
        unsigned char c = (unsigned char)lp[at + len];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) {
            return false;
        }
    }
    /* Le precedent doit ne pas etre un caractere d'identifiant */
    if (at > 0) {
        unsigned char c = (unsigned char)lp[at - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80) {
            return false;
        }
    }
    return true;
}

static PositionStmtCtx detect_pour_toute_position_ctx(const char* lp, int character) {
    if (!lp || character <= 0) return POS_CTX_NONE;

    /* Borne de la ligne (s'arrete au curseur ou newline). */
    int lineEnd = character;
    for (int i = 0; i < character; i++) {
        if (lp[i] == '\n' || lp[i] == '\0') { lineEnd = i; break; }
    }

    /* Cherche les mots-cles POSITION, DANS, AVEC dans l'ordre.
     * On ne valide pas l'enchainement complet (POUR TOUTE POSITION ...)
     * pour rester tolerant — la presence simultanee de POSITION + DANS
     * suffit pour identifier le contexte. */
    int posPosition = -1, posDans = -1, posAvec = -1;
    for (int i = 0; i < lineEnd; i++) {
        if (posPosition < 0 && kw_match_at(lp, i, lineEnd, "POSITION", 8))
            posPosition = i;
        else if (posPosition >= 0 && posDans < 0
                 && kw_match_at(lp, i, lineEnd, "DANS", 4))
            posDans = i;
        else if (posDans >= 0 && posAvec < 0
                 && kw_match_at(lp, i, lineEnd, "AVEC", 4))
            posAvec = i;
    }

    if (posPosition < 0 || posDans < 0) return POS_CTX_NONE;
    if (posAvec >= 0) return POS_CTX_AVEC;
    return POS_CTX_SUFFIX;
}

/* Vrai si receiverType est un type primitif portant des proprietes dans
 * OWL_BUILTIN_PROP_CATALOG. Liste alignee sur compiler.c::PROPS_BY_KIND
 * (tableau/file/pile/liste/JSON/date/heure/dateheure/duree). Comparaison
 * casse- et accent-insensible : receiverType arrive tel que tape par
 * l'utilisateur (Date, durée, Durée...). Gate le fallback generique pour
 * eviter de cracher Annee/EnJours sur un type registry non-primitif sans
 * fields (ex: chaineMultilingue). */
/*  — rend en plus la clef de kind normalisee (ASCII minuscule), qui
 * est la forme emise dans la colonne `kinds` du catalogue. Sans elle on sait
 * seulement QUE le receveur est primitif, pas LEQUEL — et la completion
 * servait les 17 proprietes des neuf kinds a chacun d'eux. */
static bool primitive_prop_kind_key(const char* receiverType,
                                    char* out, size_t outSize) {
    if (out && outSize) out[0] = '\0';
    if (!receiverType || receiverType[0] == '\0') return false;
    static const char* const PRIMITIVE_PROP_TYPES[] = {
        "tableau", "file", "pile", "liste", "JSON",
        "date", "heure", "dateheure", "duree",
    };
    char stripped[64];
    int len = scanner_strip_accents(receiverType, (int)strlen(receiverType),
                                    stripped, sizeof(stripped));
    int n = (int)(sizeof(PRIMITIVE_PROP_TYPES)/sizeof(PRIMITIVE_PROP_TYPES[0]));
    for (int i = 0; i < n; i++) {
        if (strcasecmp(stripped, PRIMITIVE_PROP_TYPES[i]) != 0) continue;
        if (out && outSize) {
            for (int c = 0; c < len && (size_t)c + 1 < outSize; c++) {
                char ch = stripped[c];
                out[c] = (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
                out[c + 1] = '\0';
            }
        }
        return true;
    }
    return false;
}

static bool is_primitive_prop_type(const char* receiverType) {
    return primitive_prop_kind_key(receiverType, NULL, 0);
}

/* Helper partage  (completion) +  (semantic token sur
 * membre) +  (semantic token sur receveur). Renvoie true si le
 * lexeme `[name, name+len)` matche `icones` apres strip accents + casse-
 * insensible. Le receveur du namespace iconify est un nom reserve : pas
 * de gate user-defined, coloration statique sur la forme. */
static bool is_icones_receiver_name(const char* name, int len) {
    if (!name || len <= 0 || len >= 32) return false;
    char ascii[32];
    scanner_strip_accents(name, len, ascii, sizeof(ascii));
    return strcasecmp(ascii, "icones") == 0;
}

/*  — meme mecanique que `is_icones_receiver_name` pour le namespace
 * `theme` (styles nommes declares dans un bloc Theme, resolus via
 * `Projet..Theme`). Renvoie true si le lexeme `[name, name+len)` matche
 * `theme` apres strip accents + casse-insensible (`theme`/`Theme`/`thème`/
 * `Thème` — la forme accentuee fait 6 octets UTF-8, le strip la ramene a 5).
 * Nom de namespace reserve : pas de gate user-defined, coloration statique
 * sur la forme, comme `icones`. */
static bool is_theme_receiver_name(const char* name, int len) {
    if (!name || len <= 0 || len >= 32) return false;
    char ascii[32];
    scanner_strip_accents(name, len, ascii, sizeof(ascii));
    return strcasecmp(ascii, "theme") == 0;
}

/*  — receveur du crochet d'etat `Etats[<etat>]` / `States[<etat>]`
 * (membre collection d'un style stateful, cf. src/compiler.c STYLE_PALETTES).
 * Sert a classer le NOM d'etat entre crochets comme enumMember (jaune
 * italique), meme couleur que les valeurs d'enum. Compare accent/casse
 * insensible (memes formes canon FR/EN, sans accent -> strip inoffensif). */
static bool is_etats_receiver_name(const char* name, int len) {
    if (!name || len <= 0 || len >= 32) return false;
    char ascii[32];
    scanner_strip_accents(name, len, ascii, sizeof(ascii));
    return strcasecmp(ascii, "etats") == 0 || strcasecmp(ascii, "states") == 0;
}

/*  B2 — cache des styles nommes du theme actif (bloc `est un Thème`
 * du fichier designe par `Projet..Thème`). Peuple par un scan disque unique
 * (meme mecanique que scan_project_dsl_walk pour windows/classes) et consomme
 * par la completion `theme.<prefix>`. Le nom canonique est conserve avec
 * accents (forme source) ; le type styleXxx sert le filtrage best-effort par
 * type de champ (`BTN..Style = theme.` → seulement les styleBouton). */
typedef struct {
    char name[128];        /* nom du style (forme source, accents conserves) */
    char styleType[64];    /* type styleXxx (accent-strippe pour compare) */
} ThemeStyleInfo;
static ThemeStyleInfo g_themeStyles[128];
static int g_themeStyleCount = 0;
/* Racine projet du dernier scan : un changement de projet (autre document,
 * autre workspace) re-declenche le scan. Evite la staleness cross-test in
 * process ET reflete un changement de contexte projet en prod. */
static char g_themeScanRoot[1024] = "";

/*  — extrait le NOM de l'instance de theme designee par `Projet..Thème`
 * depuis un buffer projet.owl. Le theme est desormais designe par une reference
 * typee (`Thème = ThèmeDémo`), plus par un nom de fichier. Scan token : cherche
 * un IDENT strippe "Theme" suivi de `=` puis d'un IDENTIFIER (le nom de
 * l'instance). Retourne true + copie le nom (forme source, accents conserves)
 * dans out. */
static bool theme_extract_designated_instance(const char* buf, char* out, int outSize) {
    if (!buf || outSize <= 0) return false;
    Scanner sc;
    scanner_init(&sc, buf);
    Token prev1 = (Token){0};
    Token prev2 = (Token){0};
    for (;;) {
        Token tok = scanner_scan_token(&sc);
        if (tok.type == TOKEN_EOF) break;
        /* Motif `Thème = <Instance>` : prev2 = IDENT "Theme", prev1 = '=',
         * tok = IDENT (nom de l'instance). */
        if (tok.type == TOKEN_IDENTIFIER && prev1.type == TOKEN_EQUAL
            && prev2.type == TOKEN_IDENTIFIER && prev2.length < 32) {
            char probe[32];
            scanner_strip_accents(prev2.start, prev2.length, probe, sizeof(probe));
            if (strcasecmp(probe, "Theme") == 0) {
                int n = tok.length < outSize - 1 ? tok.length : outSize - 1;
                if (n <= 0) return false;
                memcpy(out, tok.start, (size_t)n);
                out[n] = '\0';
                return true;
            }
        }
        prev2 = prev1;
        prev1 = tok;
    }
    return false;
}

/* Scan un fichier theme (buffer) pour les styles nommes du bloc
 * `<designatedInstance> est un Thème`. Motif corps : `<styleName> est un
 * <styleXxx> = {`. Le scanner consomme l'article un/une, donc apres EST vient
 * directement le type. On capte prev(styleName IDENT) EST tok(styleType IDENT).
 *  — le bloc n'est retenu que si son nom matche l'instance designee par
 * Projet..Theme (casse + accent-insensible) : plusieurs blocs `est un Thème`
 * peuvent coexister dans le projet, seul le designe alimente la completion. */
static void theme_scan_styles_from_buffer(const char* buf,
                                          const char* designatedInstance) {
    if (!buf || !designatedInstance || !designatedInstance[0]) return;
    char wantCanon[128];
    scanner_strip_accents(designatedInstance, (int)strlen(designatedInstance),
                          wantCanon, sizeof(wantCanon));
    Scanner sc;
    scanner_init(&sc, buf);
    Token prev2 = (Token){0};
    Token prev1 = (Token){0};
    bool inThemeBlock = false;
    for (;;) {
        Token tok = scanner_scan_token(&sc);
        if (tok.type == TOKEN_EOF) break;
        /* Entree bloc : `<IDENT> est un Thème` (le scanner strippe l'article,
         * "Thème" arrive en IDENT strippe "Theme" apres EST). Le nom du bloc
         * (prev2) doit matcher l'instance designee. */
        if (!inThemeBlock && tok.type == TOKEN_IDENTIFIER
            && prev1.type == TOKEN_EST && prev2.type == TOKEN_IDENTIFIER
            && tok.length < 16) {
            char probe[16];
            scanner_strip_accents(tok.start, tok.length, probe, sizeof(probe));
            char blockCanon[128];
            scanner_strip_accents(prev2.start, prev2.length,
                                  blockCanon, sizeof(blockCanon));
            if (strcasecmp(probe, "Theme") == 0
                && strcasecmp(blockCanon, wantCanon) == 0) {
                inThemeBlock = true;
                prev2 = prev1; prev1 = tok;
                continue;
            }
        }
        if (inThemeBlock) {
            if (tok.type == TOKEN_FIN) break;
            /* Declaration de style : prev2 = styleName IDENT, prev1 = EST,
             * tok = styleType IDENT (styleBouton, styleSaisie, ...). Le nom
             * du type commence par "style" (accent-strippe). */
            if (tok.type == TOKEN_IDENTIFIER && prev1.type == TOKEN_EST
                && prev2.type == TOKEN_IDENTIFIER
                && prev2.length < 128 && tok.length < 64
                && g_themeStyleCount < 128) {
                char typeProbe[64];
                scanner_strip_accents(tok.start, tok.length,
                                      typeProbe, sizeof(typeProbe));
                if (strncasecmp(typeProbe, "style", 5) == 0) {
                    ThemeStyleInfo* s = &g_themeStyles[g_themeStyleCount];
                    int n = prev2.length < 127 ? prev2.length : 127;
                    memcpy(s->name, prev2.start, (size_t)n);
                    s->name[n] = '\0';
                    snprintf(s->styleType, sizeof(s->styleType), "%s", typeProbe);
                    g_themeStyleCount++;
                }
            }
        }
        /*  — copule EN `is a <Type>` : article transparent pour le
         * prev-tracking (miroir ) afin que `X is a Theme` et
         * `fond is a styleContainer` matchent l'adjacence prev1=EST. */
        if (tok_is_en_article(tok) && prev1.type == TOKEN_EST) {
            continue;
        }
        prev2 = prev1;
        prev1 = tok;
    }
}

/* Lit un fichier disque (max 1 Mo) dans un buffer alloue (caller free). */
static char* theme_read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

/*  — walk recursif des .owl du projet a la recherche du bloc
 * `<designatedInstance> est un Thème`. Le theme etant designe par reference
 * typee (pas par nom de fichier), on ne connait plus le fichier a l'avance :
 * on scanne tous les compagnons jusqu'a trouver le bloc dont le nom matche
 * l'instance designee (theme_scan_styles_from_buffer gate sur le nom). S'arrete
 * des que des styles sont trouves. Meme idiome de walk que scan_project_dsl_walk. */
static void theme_scan_walk(const char* dir, const char* designatedInstance,
                            int depth) {
    if (depth > 3 || g_themeStyleCount > 0) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            theme_scan_walk(path, designatedInstance, depth + 1);
            if (g_themeStyleCount > 0) break;
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        char* buf = theme_read_file(path);
        if (!buf) continue;
        theme_scan_styles_from_buffer(buf, designatedInstance);
        free(buf);
        if (g_themeStyleCount > 0) break;
    }
    closedir(d);
}

/*  B2 — peuple g_themeStyles[] : trouve le projet du fichier courant,
 * lit l'instance designee par `Projet..Thème` dans projet.owl, puis walk le
 * projet pour scanner le bloc `<instance> est un Thème`. Silencieux si pas de
 * projet / pas de theme designe / bloc introuvable — la completion offre alors
 * une liste vide (l'ancre `theme.` reste utile). `curFilePath` = fichier disque
 * du document courant, sert de point de remontee pour resolve_project_root. */
static void theme_ensure_scan(const char* curFilePath) {
    if (!curFilePath || !curFilePath[0]) return;
    char projectRoot[1024];
    ProjectMode mode = resolve_project_root(curFilePath, projectRoot,
                                            sizeof(projectRoot));
    if (mode != PROJECT_MODE_PROJECT || projectRoot[0] == '\0') {
        /* Hors projet : rien a offrir, memorise l'etat pour eviter un re-scan
         * a chaque frappe sur le meme script. */
        g_themeStyleCount = 0;
        g_themeScanRoot[0] = '\0';
        return;
    }
    /* Meme projet que le dernier scan : cache valide. */
    if (strcmp(g_themeScanRoot, projectRoot) == 0) return;
    snprintf(g_themeScanRoot, sizeof(g_themeScanRoot), "%s", projectRoot);
    g_themeStyleCount = 0;

    /* Lit projet.owl (le marqueur de frontiere). */
    char projetPath[1200];
    snprintf(projetPath, sizeof(projetPath), "%s/projet.owl", projectRoot);
    char* projetBuf = theme_read_file(projetPath);
    if (!projetBuf) {
        /* Bilingue : project.owl. */
        snprintf(projetPath, sizeof(projetPath), "%s/project.owl", projectRoot);
        projetBuf = theme_read_file(projetPath);
        if (!projetBuf) return;
    }
    char instance[128];
    bool found = theme_extract_designated_instance(projetBuf, instance,
                                                   sizeof(instance));
    free(projetBuf);
    if (!found || instance[0] == '\0') return;

    /* Le bloc `<instance> est un Thème` vit dans un compagnon du projet :
     * walk recursif pour le localiser (nom de fichier libre). */
    theme_scan_walk(projectRoot, instance, 0);
}

/*  B2 — Walk-back depuis `character` pour detecter `theme.<prefix>`.
 * Miroir de detect_icones_prefix_context : mot a gauche du `.` (single, pas
 * `..`) = `theme` (casse-/accent-insensible). Prefix tape copie dans
 * outPrefix. */
static bool detect_theme_prefix_context(const char* lp, int character,
                                        char* outPrefix, int outPrefixSize) {
    if (!lp || character < 1 || outPrefixSize <= 0) return false;
    outPrefix[0] = '\0';
    #define IS_IDCH_982(x) (((x) >= 'a' && (x) <= 'z') || \
                            ((x) >= 'A' && (x) <= 'Z') || \
                            ((x) >= '0' && (x) <= '9') || \
                            (x) == '_' || (unsigned char)(x) >= 0x80)
    int c = character - 1;
    int prefixEnd = c + 1;
    while (c >= 0 && IS_IDCH_982(lp[c])) c--;
    int prefixStart = c + 1;
    if (c < 0 || lp[c] != '.') return false;
    if (c >= 1 && lp[c-1] == '.') return false;  /* exclure `..` */
    c--;
    int varEnd = c + 1;
    while (c >= 0 && IS_IDCH_982(lp[c])) c--;
    int varStart = c + 1;
    if (varEnd <= varStart || varEnd - varStart >= 32) return false;
    if (!is_theme_receiver_name(lp + varStart, varEnd - varStart)) return false;
    int plen = prefixEnd - prefixStart;
    if (plen >= outPrefixSize) plen = outPrefixSize - 1;
    if (plen > 0) memcpy(outPrefix, lp + prefixStart, plen);
    outPrefix[plen] = '\0';
    #undef IS_IDCH_982
    return true;
}

/*  — Determine le type styleXxx attendu par la prop `Style` du champ
 * recepteur dans `<CHAMP>..Style = theme.<prefix>`, pour filtrer la completion
 * `theme.` aux styles compatibles (`BTN..Style = theme.` → styleBouton). Walk-
 * back depuis le curseur : saute le prefix tape + `theme.`, exige `= <ws>
 * ..Style` precede d'un nom de champ. Le kind du champ vient de userControls[]
 * (rempli par scan_user_symbols), le styleType du couple (kind, Style) vient
 * du catalogue DSL (objectType). Le type est accent-strippe pour matcher
 * g_themeStyles[].styleType. Retourne true + remplit out si determinable ;
 * false sinon → le caller retombe sur NULL (tous les styles, fallback). */
static bool theme_derive_filter_style_type(const char* lp, int character,
                                           const char* src, int cursorOffset,
                                           char* out, int outSize) {
    if (!lp || character < 1 || outSize <= 0) return false;
    out[0] = '\0';
    #define IS_IDCH_1009(x) (((x) >= 'a' && (x) <= 'z') || \
                             ((x) >= 'A' && (x) <= 'Z') || \
                             ((x) >= '0' && (x) <= '9') || \
                             (x) == '_' || (unsigned char)(x) >= 0x80)
    int c = character - 1;
    /* Prefix tape apres `theme.`. */
    while (c >= 0 && IS_IDCH_1009(lp[c])) c--;
    /* `.` du `theme.` (single, pas `..`). */
    if (c < 0 || lp[c] != '.') return false;
    if (c >= 1 && lp[c-1] == '.') return false;
    c--;
    /* Nom du receveur : doit etre `theme`. */
    int varEnd = c + 1;
    while (c >= 0 && IS_IDCH_1009(lp[c])) c--;
    int varStart = c + 1;
    if (varEnd <= varStart) return false;
    if (!is_theme_receiver_name(lp + varStart, varEnd - varStart)) return false;
    /* Avant `theme` : whitespace puis `=` d'affectation. */
    c = varStart - 1;
    while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
    if (c < 0 || lp[c] != '=') return false;
    /* Rejeter operateurs composes (==, >=, <=, <>, !=, +=, -=, *=, /=). */
    char prevOp = c > 0 ? lp[c-1] : ' ';
    if (prevOp == '=' || prevOp == '<' || prevOp == '>' || prevOp == '!' ||
        prevOp == '+' || prevOp == '-' || prevOp == '*' || prevOp == '/')
        return false;
    c--;
    while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
    /* Nom du field (attendu `Style`). */
    int fieldEnd = c + 1;
    while (c >= 0 && IS_IDCH_1009(lp[c])) c--;
    int fieldStart = c + 1;
    if (fieldEnd <= fieldStart || fieldEnd - fieldStart >= 64) return false;
    char fieldName[64];
    memcpy(fieldName, lp + fieldStart, (size_t)(fieldEnd - fieldStart));
    fieldName[fieldEnd - fieldStart] = '\0';
    /* Deux formes pour resoudre le champ recepteur :
     *  - PLATE `CHAMP..Style = theme.` : `..` + nom de champ sur la meme
     *    ligne → kind via userControls[] ;
     *  - BLOC `PROPRIETES ... Style = theme.` (usage courant, cf. demo) :
     *    pas de `..` devant `Style`, le champ englobant vient du contexte
     *    DSL (detect_dsl_context_ex, qui empile le widget du bloc). Sans
     *    cette branche, la forme bloc retombait en fallback → tous les
     *    styles proposes, dont ceux du mauvais type. */
    char kindChar = 0;
    if (c >= 1 && lp[c] == '.' && lp[c-1] == '.') {
        c -= 2;
        int champEnd = c + 1;
        while (c >= 0 && IS_IDCH_1009(lp[c])) c--;
        int champStart = c + 1;
        if (champEnd > champStart && champEnd - champStart < 128) {
            char champName[128];
            memcpy(champName, lp + champStart, (size_t)(champEnd - champStart));
            champName[champEnd - champStart] = '\0';
            for (int i = 0; i < userControlCount; i++) {
                if (strcasecmp(champName, userControls[i].name) == 0) {
                    kindChar = userControls[i].kind;
                    break;
                }
            }
        }
    } else {
        /* Bare `Style =` : le widget vient du bloc PROPRIETES englobant. */
        if (src) kindChar = detect_dsl_context_ex(src, cursorOffset).widget;
    }
    #undef IS_IDCH_1009
    if (kindChar == 0) return false;
    /* styleType via le catalogue DSL (kind, Style) → objectType. */
    const OwlDSLPropInfo* p = dsl_prop_find(kindChar, fieldName);
    if (!p || !p->objectType || p->objectType[0] == '\0') return false;
    char stripped[64];
    scanner_strip_accents(p->objectType, (int)strlen(p->objectType),
                          stripped, sizeof(stripped));
    /* Ne filtrer que sur un type de style (styleXxx) : seuls ceux-la vivent
     * dans le theme. Un autre objectType composite (Cadre, ...) retombe en
     * fallback plutot que de vider la liste. */
    if (strncasecmp(stripped, "style", 5) != 0) return false;
    snprintf(out, (size_t)outSize, "%s", stripped);
    return true;
}

/*  B2 — Construit les completion items pour `theme.<prefix>`. Filtre
 * g_themeStyles[] par prefix (casse-/accent-insensible) et, si
 * `filterStyleType` non-NULL, par type de style (best-effort widget-type :
 * `BTN..Style = theme.` → seulement les styleBouton). filterStyleType est
 * attendu accent-strippe. */
static void build_theme_prefix_items(const char* prefixRaw,
                                     const char* filterStyleType,
                                     JsonValue* items) {
    char prefixAscii[128];
    scanner_strip_accents(prefixRaw, (int)strlen(prefixRaw),
                          prefixAscii, sizeof(prefixAscii));
    size_t plen = strlen(prefixAscii);
    for (int i = 0; i < g_themeStyleCount; i++) {
        const ThemeStyleInfo* s = &g_themeStyles[i];
        if (filterStyleType && filterStyleType[0]
            && strcasecmp(s->styleType, filterStyleType) != 0) continue;
        if (plen > 0) {
            char nameAscii[128];
            scanner_strip_accents(s->name, (int)strlen(s->name),
                                  nameAscii, sizeof(nameAscii));
            if (strncasecmp(nameAscii, prefixAscii, plen) != 0) continue;
        }
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(s->name));
        json_object_set(item, "kind", json_new_int(20)); /* EnumMember */
        json_object_set(item, "insertText", json_new_string(s->name));
        json_object_set(item, "detail", json_new_string(s->styleType));
        json_array_push(items, item);
    }
}

/*  — Walk-back depuis `character` sur la ligne `lp` pour
 * detecter le pattern `icones.<prefix><cursor>`. Renvoie true si le
 * mot a gauche du `.` (single, pas `..`) est `icones` (casse-/accent-
 * insensible). Le prefix tape entre `.` et le curseur est copie dans
 * outPrefix (vide si curseur juste apres `.`). */
static bool detect_icones_prefix_context(const char* lp, int character,
                                         char* outPrefix, int outPrefixSize) {
    if (!lp || character < 1 || outPrefixSize <= 0) return false;
    outPrefix[0] = '\0';
    #define IS_IDCH_375(x) (((x) >= 'a' && (x) <= 'z') || \
                            ((x) >= 'A' && (x) <= 'Z') || \
                            ((x) >= '0' && (x) <= '9') || \
                            (x) == '_' || (unsigned char)(x) >= 0x80)
    int c = character - 1;
    int prefixEnd = c + 1;
    while (c >= 0 && IS_IDCH_375(lp[c])) c--;
    int prefixStart = c + 1;
    if (c < 0 || lp[c] != '.') return false;
    /* Exclure `..` (double-dot) */
    if (c >= 1 && lp[c-1] == '.') return false;
    c--;
    int varEnd = c + 1;
    while (c >= 0 && IS_IDCH_375(lp[c])) c--;
    int varStart = c + 1;
    if (varEnd <= varStart || varEnd - varStart >= 32) return false;
    char vn[32];
    int vlen = varEnd - varStart;
    memcpy(vn, lp + varStart, vlen);
    vn[vlen] = '\0';
    char vnAscii[32];
    scanner_strip_accents(vn, vlen, vnAscii, sizeof(vnAscii));
    if (strcasecmp(vnAscii, "icones") != 0) return false;
    int plen = prefixEnd - prefixStart;
    if (plen >= outPrefixSize) plen = outPrefixSize - 1;
    if (plen > 0) memcpy(outPrefix, lp + prefixStart, plen);
    outPrefix[plen] = '\0';
    #undef IS_IDCH_375
    return true;
}

/*  — Normalise un prefix OWL-tape vers la forme catalog
 * (Iconify utilise `-`, OWL accepte `_` cote source). Lowercase + `_`→`-`. */
static void icones_normalize_prefix(const char* in, char* out, int outSize) {
    int i = 0;
    while (in[i] && i < outSize - 1) {
        char ch = in[i];
        if (ch == '_') ch = '-';
        else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        out[i] = ch;
        i++;
    }
    out[i] = '\0';
}

/*  — Mapping set_id → categorie OWL exposable a l'utilisateur
 * final (regle 7). Duplique localement la mini-table de
 * `src/cli_gen_assets.c` (gen_set_id_to_category). 6 categories OWL.
 * Si l'id ne matche aucune categorie : retourne "?". */
typedef struct {
    const char* owl_name;
    signed char set_ids[8];   /* set_id Iconify membres ; -1 = end */
} LspIconCategoryDef;

static const LspIconCategoryDef LSP_ICON_CATEGORIES[] = {
    /* set_id : 0=material-symbols, 1=mdi, 2=fluent, 3=vscode-icons,
     *          4=logos, 5=devicon, 6=fluent-emoji-flat, 7=carbon,
     *          8=game-icons, 9=simple-icons, 10=line-md, 11=flat-color-icons */
    { "ui",         {0, 1, 2, 7, 10, -1, -1, -1} },
    { "fichiers",   {3, -1, -1, -1, -1, -1, -1, -1} },
    { "marques",    {4, 5, 9, -1, -1, -1, -1, -1} },
    { "emoji",      {6, -1, -1, -1, -1, -1, -1, -1} },
    { "jeu",        {8, -1, -1, -1, -1, -1, -1, -1} },
    { "classiques", {11, -1, -1, -1, -1, -1, -1, -1} },
};
#define LSP_ICON_CATEGORIES_COUNT \
    ((int)(sizeof(LSP_ICON_CATEGORIES) / sizeof(LSP_ICON_CATEGORIES[0])))

static const char* lsp_set_id_to_owl_category(int set_id) {
    for (int c = 0; c < LSP_ICON_CATEGORIES_COUNT; c++) {
        const signed char* ids = LSP_ICON_CATEGORIES[c].set_ids;
        for (int i = 0; i < 8 && ids[i] >= 0; i++) {
            if (ids[i] == set_id) return LSP_ICON_CATEGORIES[c].owl_name;
        }
    }
    return "?";
}

/*  — Construit la liste de completion items pour `icones.<prefix>`.
 * Filtre OWL_ICONS_CATALOG par prefix (canonise `_`→`-`, lowercase),
 * limite a 50 items, sort alphabetique grace a l'ordre catalog (trie
 * (set_id, name) — pas strict alpha mais "raisonnable" en pratique,
 * et de toute facon le client VSCode re-trie sur le label).
 *
 * - label       : nom Iconify canon (avec `-`)
 * - insertText  : meme nom avec `-` → `_` (forme OWL legale)
 * - kind        : 20 (EnumMember)
 * - detail      : categorie OWL (`ui`, `marques`, `emoji`, `jeu`,
 *                 `fichiers`, `classiques`) — , regle 7,
 *                 jamais le nom du set Iconify natif.
 */
static void build_icones_prefix_items(const char* prefixRaw, JsonValue* items) {
    char prefixNorm[128];
    icones_normalize_prefix(prefixRaw, prefixNorm, sizeof(prefixNorm));
    size_t plen = strlen(prefixNorm);
    int emitted = 0;
    const int LIMIT = 50;
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT && emitted < LIMIT; i++) {
        const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[i];
        if (!e->name) continue;
        if (plen > 0 && strncmp(e->name, prefixNorm, plen) != 0) continue;
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(e->name));
        json_object_set(item, "kind", json_new_int(20)); /* EnumMember */
        /* insertText : `-` → `_` (forme OWL-legale, l'utilisateur ne peut
         * pas taper `-` dans un identifiant). */
        char insertBuf[128];
        int k = 0;
        for (const char* p = e->name; *p && k < (int)sizeof(insertBuf) - 1; p++) {
            insertBuf[k++] = (*p == '-') ? '_' : *p;
        }
        insertBuf[k] = '\0';
        json_object_set(item, "insertText", json_new_string(insertBuf));
        /* detail : categorie OWL. Surface utilisateur final
         * → jamais le nom du set Iconify natif. */
        const char* cat = lsp_set_id_to_owl_category(e->set_id);
        json_object_set(item, "detail", json_new_string(cat));
        json_array_push(items, item);
        emitted++;
    }
}

/*  — Convertit une position `character` (unités UTF-16, convention
 * LSP) en offset OCTET dans la ligne pointée par `lineStart`. La détection
 * de contexte de la complétion scanne la ligne en octets (`lp[c]`) ; sans
 * cette conversion, un caractère multi-octets avant le curseur (`Icône`,
 * `ô` = 2 octets) décale tous les `lp[character-1]` et fait gagner le
 * mauvais contexte. ASCII : offset octet == character (identité, 0
 * régression). Caractères astraux (U+10000+) = 2 unités UTF-16. */
static int lsp_utf16_to_byte_in_line(const char* lineStart, int character) {
    int units = 0, bytes = 0;
    while (lineStart[bytes] && lineStart[bytes] != '\n' && units < character) {
        unsigned char b = (unsigned char)lineStart[bytes];
        int blen = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
        int u = (blen == 4) ? 2 : 1;
        if (units + u > character) break;
        bytes += blen;
        units += u;
    }
    return bytes;
}

/*  : emet un item de completion pour une VALEUR d'enum (membre).
 * Factorise les 6 contextes enum-value (composed-type field, propValue
 * fieldBase/fieldOpts, simple-assign, POUR TOUTE...AVEC, call-arg).
 *
 * En mode EN (`wantEN`), le label est la forme EN du membre (nameEN
 * resolu au const-catalog, comme  pour DSL-PROPRIETES), avec
 * fallback FR si pas de nameEN distinct. filterText combine les deux
 * formes pour que le fuzzy matche peu importe la frappe. Le detail
 * suit la meme langue : detailFR / detailEN selon wantEN.
 *
 * `valueName` est la forme FR canon du membre (telle qu'elle vit dans
 * l'enum). `enumName` est le nom de l'enum (pour le detail). `kind`
 * est le CompletionItemKind LSP (13 EnumMember, 21 Constant).
 * `sortDigit` prefixe le sortText ('0' base, '1' option combinable) ;
 * si `sortOverride` est non-NULL il est utilise verbatim a la place
 * (ex: "!" pour la priorite max du contexte composed-type field). */
static void emit_enum_value_item(JsonValue* arr, const char* valueName,
                                 const char* enumName, int kind,
                                 char sortDigit, const char* sortOverride,
                                 const char* detailFR, const char* detailEN,
                                 bool wantEN) {
    const char* vLabel = valueName;
    if (wantEN) {
        for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
            if (strcasecmp(OWL_CONST_CATALOG[k].name, valueName) == 0) {
                if (OWL_CONST_CATALOG[k].nameEN && *OWL_CONST_CATALOG[k].nameEN)
                    vLabel = OWL_CONST_CATALOG[k].nameEN;
                break;
            }
        }
    }
    JsonValue* item = json_new_object();
    json_object_set(item, "label", json_new_string(vLabel));
    json_object_set(item, "kind", json_new_int(kind));
    /* filterText combine EN+FR si distinct (fuzzy peu importe la frappe). */
    if (vLabel != valueName && strcmp(vLabel, valueName) != 0) {
        char ftBuf[256];
        snprintf(ftBuf, sizeof(ftBuf), "%s %s", vLabel, valueName);
        json_object_set(item, "filterText", json_new_string(ftBuf));
    }
    char detailBuf[256];
    snprintf(detailBuf, sizeof(detailBuf), "%s %s",
             wantEN ? detailEN : detailFR, enumName);
    json_object_set(item, "detail", json_new_string(detailBuf));
    if (sortOverride) {
        json_object_set(item, "sortText", json_new_string(sortOverride));
    } else {
        char sortBuf[200];
        snprintf(sortBuf, sizeof(sortBuf), "%c_%.180s", sortDigit, vLabel);
        json_object_set(item, "sortText", json_new_string(sortBuf));
    }
    json_array_push(arr, item);
}

/* Recherche de sous-chaine insensible a la casse. haystack et needle sont
 * deja desaccentues par l'appelant (formes ASCII). Pas de strcasestr
 * (non portable), on balaye les offsets avec strncasecmp. */
static bool lsp_ci_contains(const char* haystack, const char* needle,
                            int needleLen) {
    if (!haystack || needleLen <= 0) return false;
    int hLen = (int)strlen(haystack);
    for (int off = 0; off + needleLen <= hLen; off++) {
        if (strncasecmp(haystack + off, needle, (size_t)needleLen) == 0) {
            return true;
        }
    }
    return false;
}

/*  — le nom de type ECRIT par l'utilisateur (`written`) désigne-t-il
 * la forme EN canon du type `ti` (et non sa forme FR) ? Sert à choisir la
 * langue des clés proposées dans un littéral typé `{ }` : `d is a duration`
 * → clés EN, `d est une durée` → clés FR. Comparaison accent+casse-insensible.
 * EN uniquement si le nom matche nameEN ET ne matche PAS la forme FR (name /
 * nameAscii) — sinon un type dont nameEN == nameFR resterait FR par défaut. */
static bool type_name_written_is_en(const OwlTypeInfo* ti, const char* written) {
    if (!ti || !ti->nameEN || !written || !*written) return false;
    char stripped[128];
    scanner_strip_accents(written, (int)strlen(written), stripped, sizeof(stripped));
    bool matchEN = (strcasecmp(stripped, ti->nameEN) == 0);
    bool matchFR = (strcasecmp(stripped, ti->name) == 0) ||
                   (ti->nameAscii && strcasecmp(stripped, ti->nameAscii) == 0);
    return matchEN && !matchFR;
}

/* Defini plus bas (menu_style_chain_hover & co). Forward-declare pour l'usage
 * dans le resolveur `Etats[` de la completion ci-dessous. */
static const OwlFieldInfo* owl_type_field_find(const OwlTypeInfo* ti,
                                               const char* name);

/*  — resout le type collection styleXxxEtats designe par un curseur
 * pose dans `...Etats[` (ou `...States[`) : walk arriere de la chaine
 * `..`-separee jusqu'a la racine (mot-cle `Style` d'un bloc widget/menu, ou une
 * variable de controle explicite `BTN_U..Style..`), puis resolution avant hop
 * par hop via le catalogue de types. Retourne true + le nom du type collection
 * dans outType. NULL/false si hors contexte `Etats[`. `cursorOff` = offset octet
 * absolu juste apres le `[`. */
static bool resolve_etats_bracket_collection_type(const char* src, int cursorOff,
                                                  char* outType, size_t outCap) {
    if (!src || cursorOff < 1 || !outType || outCap == 0) return false;
    #define IS_ID_1228(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || \
                           ((x) >= '0' && (x) <= '9') || (x) == '_' || (unsigned char)(x) >= 0x80)
    int c = cursorOff - 1;
    if (src[c] != '[') return false;
    c--; /* avant '[' */
    int end = c + 1;
    while (c >= 0 && IS_ID_1228(src[c])) c--;
    int start = c + 1;
    if (end <= start || end - start >= 64) return false;
    /* Le receveur du crochet doit etre Etats/States. */
    if (!is_etats_receiver_name(src + start, end - start)) return false;

    /* Collecte des segments `..`-separes en arriere : segsRev[0] = Etats. */
    char segsRev[8][64];
    int nseg = 0;
    memcpy(segsRev[nseg], src + start, (size_t)(end - start));
    segsRev[nseg][end - start] = '\0';
    nseg++;
    int pos = start - 1;
    while (nseg < 8 && pos >= 1 && src[pos] == '.' && src[pos-1] == '.') {
        pos -= 2;
        int e = pos + 1;
        while (pos >= 0 && IS_ID_1228(src[pos])) pos--;
        int s = pos + 1;
        if (e <= s || e - s >= 64) break;
        memcpy(segsRev[nseg], src + s, (size_t)(e - s));
        segsRev[nseg][e - s] = '\0';
        nseg++;
        pos = s - 1;
    }
    #undef IS_ID_1228

    /* Resolution avant : seg[0] = racine ... seg[nseg-1] = Etats. */
    const char* curType = NULL;
    char pendingKind = 0;
    bool haveControlRoot = false;
    for (int i = 0; i < nseg; i++) {
        const char* seg = segsRev[nseg - 1 - i];
        char segAscii[64];
        scanner_strip_accents(seg, (int)strlen(seg), segAscii, sizeof(segAscii));
        if (curType == NULL) {
            if (strcasecmp(segAscii, "Style") == 0) {
                if (haveControlRoot) {
                    const OwlDSLPropInfo* dp = dsl_prop_find(pendingKind, seg);
                    if (dp && dp->objectType && *dp->objectType)
                        curType = dp->objectType;
                } else {
                    /* Racine implicite : bloc menu (`Style` typé) ou widget. */
                    char menuChar = detect_dsl_context(src, cursorOff);
                    const char* rt = menu_kind_style_type(menuChar);
                    if (rt) {
                        curType = rt;
                    } else {
                        char wctx = detect_dsl_context_ex(src, cursorOff).widget;
                        const OwlDSLPropInfo* dp = dsl_prop_find(wctx, seg);
                        if (dp && dp->objectType && *dp->objectType)
                            curType = dp->objectType;
                    }
                }
                if (curType == NULL) return false;
            } else {
                /* Racine explicite : variable de controle (BTN_U..Style..). */
                bool found = false;
                for (int k = 0; k < userControlCount; k++) {
                    if (strcasecmp(seg, userControls[k].name) == 0) {
                        pendingKind = userControls[k].kind;
                        haveControlRoot = true;
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
        } else {
            const OwlTypeInfo* ti = owl_type_info_find(curType);
            const OwlFieldInfo* f = owl_type_field_find(ti, seg);
            if (!f || !f->type || !*f->type) return false;
            curType = f->type;
        }
    }
    if (curType == NULL) return false;
    snprintf(outType, outCap, "%s", curType);
    return true;
}

/*  — le curseur est-il en position du 2nd terme (scope) d'une
 * indirection `{ <nom>, | }` ? Heuristique sur la ligne courante : dans
 * l'accolade ouverte la plus interne, au moins une virgule a son niveau direct
 * et AUCUN '=' (ce qui exclut le littéral d'instance `{Champ = valeur, ...}`).
 * Chaînes et parentheses/crochets imbriques ignores. Sert a ne proposer que
 * les constantes indXxx au lieu du catalogue general (support #210). */
static bool lsp_in_indirection_scope(const char* line, int cursorByte) {
    struct { bool comma; bool eq; } fr[32];
    int sp = 0;      /* profondeur accolade */
    int pd = 0;      /* profondeur ( / [ dans le niveau accolade courant */
    bool inStr = false;
    for (int c = 0; c < cursorByte && line[c] && line[c] != '\n'; c++) {
        char ch = line[c];
        if (ch == '"' && (c == 0 || line[c-1] != '\\')) { inStr = !inStr; continue; }
        if (inStr) continue;
        if (ch == '{') { if (sp < 32) { fr[sp].comma = false; fr[sp].eq = false; } sp++; pd = 0; }
        else if (ch == '}') { if (sp > 0) sp--; }
        else if (ch == '(' || ch == '[') pd++;
        else if (ch == ')' || ch == ']') { if (pd > 0) pd--; }
        else if (sp > 0 && sp <= 32 && pd == 0) {
            if (ch == ',') fr[sp-1].comma = true;
            else if (ch == '=') fr[sp-1].eq = true;
        }
    }
    if (sp < 1 || sp > 32) return false;
    return fr[sp-1].comma && !fr[sp-1].eq;
}

/*  — forme a afficher d'un litteral booleen selon la langue : en mode
 * EN, Vrai->True / Faux->False (Null identique EN/FR). Les 3 sites de completion
 * qui emettaient ces litteraux en dur ignoraient wantEN (support #208). */
static const char* bool_literal_label(const char* fr, bool wantEN) {
    if (!wantEN) return fr;
    if (strcmp(fr, "Vrai") == 0) return "True";
    if (strcmp(fr, "Faux") == 0) return "False";
    return fr;  /* Null : identique EN/FR */
}

JsonValue* handle_completion(JsonValue* params) {
    JsonValue* items = json_new_array();

    /* Mode langue : "en" => sert les formes EN (nameEN). Calcule tot car
     * les contextes enum-value (composed-type field, fieldBase/Opts,
     * simple-assign, POUR TOUTE, call-arg) en ont besoin avant le bloc
     * natives. cf. . */
    bool wantEN = (strcmp(lspServer.language, "en") == 0);

    /*  — contexte de declenchement LSP (CompletionContext).
     * triggerKind 2 = TriggerCharacter ; on isole le declenchement auto par
     * '(' (parenTrigger). Sur '(' generique, on ne veut PAS de popup
     * bruyante : seuls les contextes focalises (fenetres arg0 d'Ouvre,
     * membres enum) renvoient leur liste, le reste renvoie vide. Le
     * Ctrl-Space (Invoked, triggerKind 1 ou contexte absent) garde la liste
     * generale complete partout. */
    bool parenTrigger = false;
    bool commaTrigger = false;
    {
        JsonValue* ctx = json_get(params, "context");
        if (ctx) {
            int triggerKind = (int)json_get_int(ctx, "triggerKind", 0);
            const char* triggerChar = json_get_string(ctx, "triggerCharacter");
            if (triggerKind == 2 && triggerChar && strcmp(triggerChar, "(") == 0)
                parenTrigger = true;
            /*  — ',' auto-trigger : meme discipline que '(' (liste
             * uniquement en contexte focalise deja en early-return, dont le
             * scope d'indirection ; sinon vide via le garde plus bas). */
            if (triggerKind == 2 && triggerChar && strcmp(triggerChar, ",") == 0)
                commaTrigger = true;
        }
    }

    /*  — Trigger lazy le scan workspace cross-file. didOpen ne
     * le fait plus (gain 1.5s startup) ; la completion a besoin des
     * noms FEN_/struct/classes cross-file, donc on le declenche ici
     * au 1er appel. Idempotent. */
    lsp_ensure_project_scan();

    /* Garantir que userSymbols/userWindows/userControls sont a jour. En
     * prod, c'est appele via didChange→publish_diagnostics et semantic
     * tokens, mais le harness in-process (tests/lsp/) appelle direct
     * handle_completion — sans ca userControls reste vide. */
    scan_user_symbols();

    /* Detect context: string, dot, double-dot */
    bool doubleDotContext = false;
    bool singleDotContext = false;
    /* b — offset du `.` (single) / 1er `.` (double) qui ouvre l'accès
     * membre, quand un mot partiel est déjà tapé après le point
     * (`tab.Cherche‸`). -1 = curseur collé au point (cas immédiat, end calculé
     * via character-dotLen). Sans ça, `tab.Cherche` perdait le contexte dot
     * -> complétion générale -> signature native AVEC le receveur. */
    int dotReceiverEnd = -1;
    bool inStringContext = false;
    bool propValueContext = false;   /* after `=` that follows `var..Field` */
    bool simpleAssignContext = false; /* after `=` that follows `var` (no ..) */
    bool boolLiteralOnly = false;    /*  — decl-assign d'un booleen : ne
                                      * proposer que les litteraux Vrai/Faux cote
                                      * contribution CONSTANTES (natives/vars intacts) */
    /*  — position valeur d'un decl-assign generalisee (`<var> est un
     * <type> = <curseur>`), TOUS types confondus (primitif, compose registry,
     * enum, struct/classe user), pas juste booleen. Fondation etape B :
     * `declAssignType` porte le nom de type LHS capture ; l'etape B filtrera
     * natives/champs/variables/constantes par ce type (owl_type_info_find /
     * owl_prim_type_is / owl_enum_info_find sur declAssignType donnent la
     * categorie attendue). Etape A se contente de retirer, dans cette position,
     * les contributions mots-cles + noms de types (jamais des valeurs). */
    bool inDeclAssignValue = false;
    char declAssignType[128] = "";
    bool colonMemberContext = false; /*  — ':' dans body class = this. */
    UserClassInfo* colonClass = NULL;
    UserClassInfo* enclosingClass = NULL; /* classe englobant le curseur, sc 3 */
    bool callArgContext   = false;   /* inside `fn(..., <cursor>)` */
    bool typeDeclContext  = false;   /* `nom est [un|une] <cursor>` — propose types */
    bool typeDeclHasArticle = false; /* true si "un/une" déjà présent → ne pas le re-suggérer */
    bool controlModifierContext = false; /*  — `est un Champ|Control|Field <cursor>` → propose les types de contrôle (Bouton/Saisie/…) au lieu des primitifs */
    bool controlModifierGlued = false;   /*  — sous-cas "collé" (`est un Champ|` sans espace) : textEdit d'insertion pour ne pas être masqué par le filtrage client sur le mot "Champ" */
    bool collTypeContext  = false;   /*  — `tableau de <cursor>` — propose types nus */
    bool iconesPrefixContext = false; /*  — `icones.<prefix>` lookup catalog */
    char iconesPrefix[128] = "";     /* prefix tape apres `icones.` (vide si juste apres `.`) */
    bool themePrefixContext = false; /*  B2 — `theme.<prefix>` styles du theme actif */
    char themePrefix[128] = "";      /* prefix tape apres `theme.` (vide si juste apres `.`) */
    char themeFilterStyleType[64] = ""; /*  — styleXxx attendu par `..Style` recepteur (vide = tous) */
    PositionStmtCtx pourTouteCtx = POS_CTX_NONE;  /* POUR TOUTE POSITION ... AVEC <cursor> */
    char receiverType[64] = "";
    /*  — racine du receveur avant tout crochet (`TABLE_X` pour
     * `TABLE_X[1].`) et nombre de groupes `[...]` traverses. Le second
     * distingue une LIGNE (1 groupe, ou le nom de colonne a un sens) d'une
     * CELLULE (2 groupes, ou il n'en a plus). */
    char receiverRootName[128] = "";
    int  receiverBracketGroups = 0;
    char propFieldName[128] = "";
    char propReceiverType[64] = "";
    char simpleAssignVarType[128] = "";  /* type of var in `var = <cursor>` */
    char callFnName[128] = "";
    int  callArgIndex = 0;
    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (td && pos) {
        const char* uri = json_get_string(td, "uri");
        int line = (int)json_get_int(pos, "line", 0);
        int character = (int)json_get_int(pos, "character", 0);
        int idx = find_doc(uri);

        /* Detect if cursor is inside a string (but not in [%...%] interpolation) */
        if (idx >= 0) {
            const char* src = lspServer.documents[idx].content;
            int curLine = 0;
            const char* lp = src;
            while (*lp && curLine < line) {
                if (*lp == '\n') curLine++;
                lp++;
            }
            /*  — character (UTF-16) -> offset octet dans la ligne.
             * Persiste pour les sous-blocs suivants (même ligne courante). */
            character = lsp_utf16_to_byte_in_line(lp, character);
            /* Scan line up to cursor position to determine if we're in a string */
            bool inStr = false;
            bool inInterp = false;
            for (int c = 0; c < character && lp[c] != '\0' && lp[c] != '\n'; c++) {
                if (lp[c] == '"' && (c == 0 || lp[c-1] != '\\')) {
                    inStr = !inStr;
                    inInterp = false;
                } else if (inStr && lp[c] == '[' && c+1 < character && lp[c+1] == '%') {
                    inInterp = true;
                    c++; /* skip % */
                } else if (inInterp && lp[c] == '%' && c+1 < character && lp[c+1] == ']') {
                    inInterp = false;
                    c++; /* skip ] */
                }
            }
            inStringContext = inStr && !inInterp;
        }

        if (inStringContext) {
            /* Inside a string (not in interpolation) - no completion */
            return items; /* empty array */
        }

        if (idx >= 0 && character >= 1) {
            const char* src = lspServer.documents[idx].content;
            int curLine = 0;
            const char* lp = src;
            while (*lp && curLine < line) {
                if (*lp == '\n') curLine++;
                lp++;
            }

            /*  — curseur dans `...Etats[` : propose les etats valides du
             * sous-type de style resolu (survol/focus/appuyé/grisé/lectureSeule,
             * selon le sous-type). Parite stricte compilateur : la liste vient du
             * catalogue (indexStates), lui-meme derive des tables compiler.c, donc
             * on ne propose jamais un etat que le compilo rejetterait. Court-
             * circuite le reste (dans `[`, aucun autre contexte n'a de sens). */
            if (lp[character-1] == '[') {
                int cursorOff = (int)(lp - src) + character;
                char collType[64];
                if (resolve_etats_bracket_collection_type(src, cursorOff,
                                                          collType, sizeof(collType))) {
                    const OwlTypeInfo* eti = owl_type_info_find(collType);
                    if (eti && eti->indexStates && eti->indexStateCount > 0) {
                        for (int s = 0; s < eti->indexStateCount; s++) {
                            const OwlStateInfo* st = &eti->indexStates[s];
                            const char* label = (wantEN && st->en && *st->en)
                                                ? st->en : st->fr;
                            if (!label || !*label) continue;
                            JsonValue* item = json_new_object();
                            json_object_set(item, "label", json_new_string(label));
                            json_object_set(item, "kind", json_new_int(20)); /* EnumMember */
                            char sortBuf[80];
                            snprintf(sortBuf, sizeof(sortBuf), "0_%.70s", label);
                            json_object_set(item, "sortText", json_new_string(sortBuf));
                            json_array_push(items, item);
                        }
                    }
                    return items;
                }
                /* Type non resolu : `[` est probablement une indexation tableau
                 * quelconque -> pas de completion d'etat, on laisse le flux normal. */
            }

            /*  — 2nd terme (scope) d'une indirection `{ <nom>, | }` :
             * proposer UNIQUEMENT les constantes de scope (categorie
             * "Indirection" : indFenêtre/indChamp/indVariable, formes EN comprises),
             * sans le bruit du catalogue general. Court-circuite comme `[`. */
            if (lsp_in_indirection_scope(lp, character)) {
                for (int i = 0; i < OWL_CONST_CATALOG_COUNT; i++) {
                    const OwlConstInfo* c = &OWL_CONST_CATALOG[i];
                    if (!c->category || strcmp(c->category, "Indirection") != 0)
                        continue;
                    const char* label = (wantEN && c->nameEN && *c->nameEN)
                                        ? c->nameEN : c->name;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(label));
                    json_object_set(item, "kind", json_new_int(21)); /* Constant */
                    char detailBuf[256];
                    snprintf(detailBuf, sizeof(detailBuf), "[Indirection] %s",
                             c->description ? c->description : "");
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    char sortBuf[80];
                    snprintf(sortBuf, sizeof(sortBuf), "0_%.70s", label);
                    json_object_set(item, "sortText", json_new_string(sortBuf));
                    json_array_push(items, item);
                }
                return items;
            }

            /* POUR TOUTE POSITION ... AVEC ... statement context */
            pourTouteCtx = detect_pour_toute_position_ctx(lp, character);
            /*  — Detecter classe englobant le curseur (pour sc 1 & 3). */
            enclosingClass = user_class_enclosing(idx, line);

            if (character >= 1 && lp[character-1] == ':' &&
                (character < 2 || lp[character-2] != ':') &&
                enclosingClass != NULL) {
                /*  — ':' dans body class = acces a un membre de la
                 * classe courante. Exclure le ':' de return-type
                 * `PROCEDURE Foo() : chaine` : on regarde si juste avant
                 * (en sautant les espaces) il y a un ')'. */
                int c = character - 2;
                while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                if (c < 0 || lp[c] != ')') {
                    colonMemberContext = true;
                    colonClass = enclosingClass;
                }
            }
            if (colonMemberContext) {
                /* ne pas evaluer les autres contextes */
            } else if (character >= 2 && lp[character-1] == '.' && lp[character-2] == '.') {
                doubleDotContext = true;
                dotReceiverEnd = character - 2;
            } else if (lp[character-1] == '.' && (character < 2 || lp[character-2] != '.')) {
                singleDotContext = true;
                dotReceiverEnd = character - 1;
            } else {
                /* Property-value context: cursor is after `=` that follows
                 * `var..Field`. Walk back, skipping the partial word being
                 * typed, whitespace, then require `=`, then `..Field`. */
                int c = character - 1;
                #define IS_IDCH(x) (((x) >= 'a' && (x) <= 'z') || \
                                    ((x) >= 'A' && (x) <= 'Z') || \
                                    ((x) >= '0' && (x) <= '9') || \
                                    (x) == '_' || (unsigned char)(x) >= 0x80)
                while (c >= 0 && IS_IDCH(lp[c])) c--;          /* partial word */
                while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                /* b — `<receiver>[.|..]<word>‸` : accès membre/méthode
                 * avec un préfixe déjà tapé. Sans ça, taper un mot après le `.`
                 * perdait le contexte dot -> complétion générale (et pour une
                 * méthode fluent, signature native AVEC le receveur). On
                 * réarme le contexte dot ; le `=` check ci-dessous ne matchera
                 * pas (lp[c] == '.'). */
                if (c >= 1 && lp[c] == '.' && lp[c-1] == '.') {
                    doubleDotContext = true;
                    dotReceiverEnd = c - 1;
                } else if (c >= 1 && lp[c] == '.' && lp[c-1] != '.'
                           && IS_IDCH(lp[c-1])) {
                    singleDotContext = true;
                    dotReceiverEnd = c;
                }
                if (c >= 0 && lp[c] == '=') {
                    /* Reject compound/comparison operators: ==, >=, <=, <>, !=, +=, -=, *=, /= */
                    char prev = c > 0 ? lp[c-1] : ' ';
                    if (prev != '=' && prev != '<' && prev != '>' && prev != '!' &&
                        prev != '+' && prev != '-' && prev != '*' && prev != '/') {
                        c--;
                        while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                        int fieldEnd = c + 1;
                        while (c >= 0 && IS_IDCH(lp[c])) c--;
                        int fieldStart = c + 1;
                        if (c >= 1 && lp[c] == '.' && lp[c-1] == '.' &&
                            fieldEnd > fieldStart && fieldEnd - fieldStart < 128) {
                            memcpy(propFieldName, lp + fieldStart, fieldEnd - fieldStart);
                            propFieldName[fieldEnd - fieldStart] = '\0';
                            c -= 2;
                            int varEnd = c + 1;
                            while (c >= 0 && IS_IDCH(lp[c])) c--;
                            int varStart = c + 1;
                            if (varEnd > varStart && varEnd - varStart < 128) {
                                char varName[128];
                                memcpy(varName, lp + varStart, varEnd - varStart);
                                varName[varEnd - varStart] = '\0';
                                for (int i = 0; i < userSymbolCount; i++) {
                                    if (userSymbols[i].kind == 6 &&
                                        strcasecmp(varName, userSymbols[i].name) == 0) {
                                        strncpy(propReceiverType, userSymbols[i].detail,
                                                sizeof(propReceiverType) - 1);
                                        break;
                                    }
                                }
                                /* Chain `..Cadre..Field` : varName est un type
                                 * composé. Set propReceiverType pour resoudre
                                 * le RHS de field via OWL_COMPOSED_TYPE_CATALOG. */
                                if (propReceiverType[0] == '\0') {
                                    for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                        const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                                        if (strcasecmp(f->typeName, varName) == 0) {
                                            strncpy(propReceiverType, f->typeName,
                                                    sizeof(propReceiverType) - 1);
                                            break;
                                        }
                                    }
                                }
                                propValueContext = true;
                            }
                        } else if (fieldEnd > fieldStart && fieldEnd - fieldStart < 128) {
                            /* No `..` before `=` → simple assignment: `var = <cursor>` */
                            char varName[128];
                            memcpy(varName, lp + fieldStart, fieldEnd - fieldStart);
                            varName[fieldEnd - fieldStart] = '\0';
                            /* Lookup var type in userSymbols */
                            for (int i = 0; i < userSymbolCount; i++) {
                                if (userSymbols[i].kind == 6 &&
                                    strcasecmp(varName, userSymbols[i].name) == 0) {
                                    strncpy(simpleAssignVarType, userSymbols[i].detail,
                                            sizeof(simpleAssignVarType) - 1);
                                    simpleAssignContext = true;
                                    break;
                                }
                            }
                            /* Cas decl-and-assign : `c est une Couleur = `.
                             * Le walk-back attrape le TYPE (`Couleur`) avant
                             * `=`, pas le var (`c`). Si le mot est un type
                             * connu, on l'utilise comme simpleAssignVarType
                             * pour proposer ses valeurs (utile quand l'enum
                             * est registry, ex: Couleur, ou user-struct). */
                            if (!simpleAssignContext) {
                                if (owl_enum_info_find(varName)
                                    || user_enum_lookup(varName)
                                    || owl_type_info_find(varName)) {
                                    snprintf(simpleAssignVarType,
                                             sizeof(simpleAssignVarType),
                                             "%.*s", (int)(sizeof(simpleAssignVarType) - 1),
                                             varName);
                                    simpleAssignContext = true;
                                }
                            }
                            /*  — generalisation de la detection : si le
                             * mot avant `=` n'est pas une variable connue mais un
                             * TYPE (primitif, compose registry, enum, struct ou
                             * classe user), on est en position valeur d'un
                             * decl-assign. On capture le type LHS (fondation
                             * etape B) et on arme inDeclAssignValue pour retirer
                             * plus bas les mots-cles + noms de types de cette
                             * position. booleen et les enums/types
                             * registry (simpleAssignContext) en sont des cas
                             * particuliers deja geres. */
                            if (owl_prim_type_is(varName)
                                || owl_type_info_find(varName)
                                || owl_enum_info_find(varName)
                                || user_enum_lookup(varName)
                                || user_struct_lookup(varName)
                                || user_class_lookup(varName)) {
                                inDeclAssignValue = true;
                                snprintf(declAssignType, sizeof(declAssignType),
                                         "%.*s", (int)(sizeof(declAssignType) - 1),
                                         varName);
                            }
                            /*  — `b est un booleen = <curseur>` : booleen
                             * est un type PRIMITIF (hors OWL_TYPE_CATALOG), donc
                             * owl_type_info_find echoue et simpleAssignContext
                             * reste faux → le chemin general dumpait TOUT le
                             * catalogue de constantes (AlignH & co) au lieu des
                             * seuls litteraux booleens. On restreint la seule
                             * contribution CONSTANTES aux litteraux Vrai/Faux ;
                             * natives et variables assignables restent proposees
                             * (categorie distincte, legitime). */
                            if (!simpleAssignContext) {
                                char sb[64];
                                scanner_strip_accents(varName, (int)strlen(varName),
                                                      sb, sizeof(sb));
                                if (strcasecmp(sb, "booleen") == 0
                                    || strcasecmp(sb, "boolean") == 0)
                                    boolLiteralOnly = true;
                            }
                        }
                    }
                }
                #undef IS_IDCH
            }

            /*  — `icones.<prefix>` : detection independante, peut
             * coexister avec singleDotContext (curseur juste apres `.`).
             * Si match, on ecrasera le traitement standard plus bas. */
            if (detect_icones_prefix_context(lp, character,
                                             iconesPrefix, sizeof(iconesPrefix))) {
                iconesPrefixContext = true;
            }

            /*  B2 — `theme.<prefix>` : detection independante (idem
             * icones). Court-circuite le traitement standard plus bas pour
             * offrir les styles nommes du theme actif. */
            if (detect_theme_prefix_context(lp, character,
                                            themePrefix, sizeof(themePrefix))) {
                themePrefixContext = true;
                /*  — filtrer par type de champ si `..Style = theme.`.
                 *  — src + offset pour resoudre aussi la forme bloc
                 * `PROPRIETES ... Style = theme.` (widget englobant). */
                int themeCursorOffset = (int)(lp - src) + character;
                theme_derive_filter_style_type(lp, character, src,
                                               themeCursorOffset,
                                               themeFilterStyleType,
                                               sizeof(themeFilterStyleType));
            }

            /* Call-argument context: walk back, skipping balanced parens,
             * counting top-level commas, until we hit the opening `(`.
             * The identifier just before `(` is the function name. */
            if (!doubleDotContext && !singleDotContext && !propValueContext) {
                #define IS_IDCH(x) (((x) >= 'a' && (x) <= 'z') || \
                                    ((x) >= 'A' && (x) <= 'Z') || \
                                    ((x) >= '0' && (x) <= '9') || \
                                    (x) == '_' || (unsigned char)(x) >= 0x80)
                int c = character - 1;
                int depth = 0;
                int commas = 0;
                bool inStr = false;
                while (c >= 0) {
                    char ch = lp[c];
                    if (ch == '\n') break;
                    if (ch == '"' && (c == 0 || lp[c-1] != '\\')) { inStr = !inStr; c--; continue; }
                    if (inStr) { c--; continue; }
                    if (ch == ')')      depth++;
                    else if (ch == '(') {
                        if (depth == 0) break;  /* found opener */
                        depth--;
                    }
                    else if (ch == ',' && depth == 0) commas++;
                    c--;
                }
                if (c >= 0 && lp[c] == '(') {
                    /* Identifier just before `(` (skip optional whitespace). */
                    int idEnd = c;
                    while (idEnd > 0 && (lp[idEnd-1] == ' ' || lp[idEnd-1] == '\t')) idEnd--;
                    int idStart = idEnd;
                    while (idStart > 0 && IS_IDCH(lp[idStart-1])) idStart--;
                    /*  (support #213) — une SIGNATURE de PROCEDURE/FONCTION
                     * n'est PAS un appel : `PROCEDURE foo(param est <curseur>)`.
                     * Si l'identifiant devant `(` est lui-meme precede de
                     * PROCEDURE/FONCTION/FUNCTION, on laisse callArgContext faux
                     * pour que le contexte type-declaration (`param est <curseur>`)
                     * s'active et propose types + structures/classes, au lieu du
                     * dump de mots-cles (SI/hérite/implémente). */
                    int kwEnd = idStart;
                    while (kwEnd > 0 && (lp[kwEnd-1] == ' ' || lp[kwEnd-1] == '\t')) kwEnd--;
                    int kwStart = kwEnd;
                    while (kwStart > 0 && IS_IDCH(lp[kwStart-1])) kwStart--;
                    int kwLen = kwEnd - kwStart;
                    bool isSignature =
                        (kwLen == 9 && strncasecmp(lp + kwStart, "PROCEDURE", 9) == 0) ||
                        (kwLen == 8 && strncasecmp(lp + kwStart, "FONCTION",  8) == 0) ||
                        (kwLen == 8 && strncasecmp(lp + kwStart, "FUNCTION",  8) == 0);
                    if (!isSignature && idEnd > idStart && idEnd - idStart < 128) {
                        memcpy(callFnName, lp + idStart, idEnd - idStart);
                        callFnName[idEnd - idStart] = '\0';
                        callArgIndex   = commas;
                        callArgContext = true;
                    }
                }
                #undef IS_IDCH
            }

            /* Type-declaration context: `nom est [un|une] <cursor>`.
             * Stratégie: collecter jusqu'à 3 mots précédents (en sautant
             * whitespace), puis examiner les positions trailing.
             * Cas pris en compte:
             *   `nom est`           → typeDecl, no article  (cursor après est)
             *   `nom est un`        → typeDecl, with article
             *   `nom est une`       → typeDecl, with article
             *   `nom est T<partial>`→ typeDecl, no article  (T en cours de frappe)
             *   `nom est un T<partial>` → typeDecl, with article
             */
            if (!doubleDotContext && !singleDotContext &&
                !propValueContext && !callArgContext) {
                #define IS_IDCH(x) (((x) >= 'a' && (x) <= 'z') || \
                                    ((x) >= 'A' && (x) <= 'Z') || \
                                    ((x) >= '0' && (x) <= '9') || \
                                    (x) == '_' || (unsigned char)(x) >= 0x80)
                /* Collecter jusqu'à 3 mots avant le curseur (du plus proche
                 * au plus loin). Chaque mot stocké en lowercase. */
                char words[3][16];
                int  nwords = 0;
                int c = character - 1;
                while (nwords < 3 && c >= 0) {
                    /* Skip whitespace before word */
                    while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                    if (c < 0) break;
                    if (!IS_IDCH(lp[c])) break;  /* non-id char (newline, ()…) */
                    int wEnd = c + 1;
                    while (c >= 0 && IS_IDCH(lp[c])) c--;
                    int wStart = c + 1;
                    int wLen = wEnd - wStart;
                    if (wLen > 15) wLen = 15;
                    for (int i = 0; i < wLen; i++) {
                        char ch = lp[wStart + i];
                        if (ch >= 'A' && ch <= 'Z') ch += 32;
                        words[nwords][i] = ch;
                    }
                    words[nwords][wLen] = '\0';
                    nwords++;
                }
                /* Vérifier les patterns. words[0] = mot le plus proche du curseur.
                 * Cas:
                 *  - curseur après identifiant: words[0] = partial en cours.
                 *    On vérifie le partial ET le mot d'avant.
                 *  - curseur après espace/non-id: words[0] = dernier mot avant
                 *    le curseur. */
                bool cursorAfterIdent = (character > 0 && IS_IDCH(lp[character - 1]));

                /* Helper pour matcher un mot par index. */
                #define WMATCH(i, s) ((i) < nwords && strcmp(words[i], (s)) == 0)
                /* Mot-cle de declaration : `est` (FR) ou `is` (EN). */
                #define WKW(i) (WMATCH(i, "est") || WMATCH(i, "is"))
                /* Article : `un`/`une` (FR) ou `a`/`an` (EN). N'est valable
                 * qu'adjacent au mot-cle de declaration (garde-fou applique au
                 * site d'appel) — `a` seul est ambigu (borne de boucle). */
                #define WART(i) (WMATCH(i, "un") || WMATCH(i, "une") || \
                                 WMATCH(i, "a")  || WMATCH(i, "an"))
                /*  — modificateur widget optionnel entre l'article et le
                 * type. Apres lui, on propose les types de contrôle. Triplet
                 * canon : cf. lsp_is_champ_modifier_word (point de verite unique). */
                #define WMOD(i) ((i) < nwords && \
                                 lsp_is_champ_modifier_word(words[i], \
                                                            (int)strlen(words[i])))

                if (cursorAfterIdent) {
                    /* Curseur sur partial. Patterns reconnus:
                     *  - partial="est"/"is",   words[1]=identifiant → typeDecl no article
                     *    (l'utilisateur tape `b est` — on propose les types ;
                     *    si en fait il tapait `estimation` la complétion classique
                     *    sera préférée car il continuera à taper)
                     *  - partial=article, words[1]=mot-cle         → typeDecl with article
                     *  - partial=type-name, words[1]=mot-cle       → typeDecl no article
                     *  - partial=type-name, words[1]=article, words[2]=mot-cle → with article
                     */
                    if (WKW(0) && nwords >= 2) {
                        typeDeclContext = true;
                        typeDeclHasArticle = false;
                    } else if (WART(0) && WKW(1)) {
                        typeDeclContext = true;
                        typeDeclHasArticle = true;
                    } else if (WMOD(0) && WKW(1)) {
                        /*  (support #431) — forme EN SANS article, collée :
                         * `is Control|` / `is Field|` (words = [modif, copule], pas
                         * d'article). Doit primer sur le cas type-partiel `WKW(1)`
                         * suivant (qui proposerait les primitifs). controlModifierGlued
                         * -> textEdit d'insertion (évite le masquage client sur le
                         * mot "Control"). Cf. parité avec la forme avec article
                         * (WMOD(0) && WART(1) && WKW(2), ). */
                        typeDeclContext = true;
                        controlModifierContext = true;
                        controlModifierGlued = true;
                        typeDeclHasArticle = false;
                    } else if (WKW(1)) {
                        typeDeclContext = true;
                        typeDeclHasArticle = false;
                    } else if (WMOD(0) && WART(1) && WKW(2)) {
                        /*  (support #167) — curseur COLLÉ après le
                         * modificateur, sans espace : `est un Champ|` /
                         * `is a Control|`. words = [modif, article, copule].
                         * Config symétrique du cas "espace" (branche else)
                         * jamais couverte par  : sans ça on tombait sur
                         * le cas type-partiel (WART(1)&&WKW(2)) → primitifs. Le
                         * flag `controlModifierGlued` déclenche un textEdit
                         * d'insertion à l'émission (évite le masquage par
                         * filtrage client sur le mot "Champ"). */
                        typeDeclContext = true;
                        controlModifierContext = true;
                        controlModifierGlued = true;
                        typeDeclHasArticle = true;
                    } else if (WART(1) && WKW(2)) {
                        typeDeclContext = true;
                        typeDeclHasArticle = true;
                    } else if (WMOD(1) && WART(2)) {
                        /*  — `est un Champ Bou<partial>` : modificateur en
                         * words[1], article en words[2] (copule au-delà des 3 mots
                         * collectés, assumée). Propose les types de contrôle. */
                        typeDeclContext = true;
                        controlModifierContext = true;
                        typeDeclHasArticle = true;
                    }
                } else {
                    /* Curseur après séparateur. words[0] = dernier mot complet. */
                    if (WKW(0)) {
                        typeDeclContext = true;
                        typeDeclHasArticle = false;
                    } else if (WART(0) && WKW(1)) {
                        typeDeclContext = true;
                        typeDeclHasArticle = true;
                    } else if (WMOD(0) && WART(1) && WKW(2)) {
                        /*  — `est un Champ <cursor>` / `is a Control <cursor>`
                         * (curseur après le séparateur suivant le modificateur) :
                         * words = [modificateur, article, copule]. C'est LA branche
                         * qui manquait (asymétrie support #167) → types de contrôle. */
                        typeDeclContext = true;
                        controlModifierContext = true;
                        typeDeclHasArticle = true;
                    } else if (WMOD(0) && WKW(1)) {
                        /*  (support #431) — forme EN SANS article :
                         * `is Control <cursor>` / `is Field <cursor>` (words =
                         * [modificateur, copule], pas d'article). Parité avec la
                         * forme avec article ci-dessus → types de contrôle. */
                        typeDeclContext = true;
                        controlModifierContext = true;
                        typeDeclHasArticle = false;
                    }
                }
                #undef WMOD
                #undef WART
                #undef WKW
                #undef WMATCH
                #undef IS_IDCH
            }

            /*  C3 — Contexte `<collection> de <cursor>` : le `de`
             * d'un type generique (tableau/liste/file/pile de <TYPE>) doit
             * proposer des noms de TYPES, pas etre folde en prefixe `dé`
             * (Défile/Dépile). Walk-back : skip mot partiel, skip blancs,
             * exige le mot `de`, puis exige une collection avant. Prime
             * sur typeDecl (ecrase si les deux matchent). */
            if (!doubleDotContext && !singleDotContext &&
                !propValueContext && !callArgContext) {
                #define IS_IDCH(x) (((x) >= 'a' && (x) <= 'z') || \
                                    ((x) >= 'A' && (x) <= 'Z') || \
                                    ((x) >= '0' && (x) <= '9') || \
                                    (x) == '_' || (unsigned char)(x) >= 0x80)
                int c = character - 1;
                /* skip mot partiel en cours de frappe */
                while (c >= 0 && IS_IDCH(lp[c])) c--;
                while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                int deEnd = c + 1;
                while (c >= 0 && IS_IDCH(lp[c])) c--;
                int deStart = c + 1;
                if (deEnd - deStart == 2 &&
                    strncasecmp(lp + deStart, "de", 2) == 0) {
                    /* le mot juste avant doit etre une collection generique */
                    while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                    int collEnd = c + 1;
                    while (c >= 0 && IS_IDCH(lp[c])) c--;
                    int collStart = c + 1;
                    int collLen = collEnd - collStart;
                    if (collLen > 0 && collLen < 16) {
                        char coll[16];
                        char collAscii[16];
                        memcpy(coll, lp + collStart, collLen);
                        coll[collLen] = '\0';
                        scanner_strip_accents(coll, collLen,
                                              collAscii, sizeof(collAscii));
                        if (strcasecmp(collAscii, "tableau") == 0 ||
                            strcasecmp(collAscii, "liste") == 0 ||
                            strcasecmp(collAscii, "file") == 0 ||
                            strcasecmp(collAscii, "pile") == 0) {
                            collTypeContext = true;
                            typeDeclContext = false; /* prime sur typeDecl */
                        }
                    }
                }
                #undef IS_IDCH
            }

            /* Extract variable name before the dot(s) to find its type.
             * Supports method chaining: `mock.Quand().Avec(<cursor>)` walks
             * back through `)` `(` ... to find the root identifier. */
            if (doubleDotContext || singleDotContext) {
                int dotLen = doubleDotContext ? 2 : 1;
                /* b — si un mot partiel est tapé après le point, le
                 * receveur finit au `.` (dotReceiverEnd), pas à character-dotLen. */
                int end = (dotReceiverEnd >= 0) ? dotReceiverEnd
                                                : (character - dotLen);

                /* If previous char is ')', walk back through the balanced call:
                 * skip ')'...'(' and the identifier before '(' (the method name).
                 * Repeat until we land on a plain identifier. */
                while (end > 0 && lp[end-1] == ')') {
                    int depth = 1;
                    int c = end - 2;
                    while (c >= 0 && depth > 0) {
                        if (lp[c] == ')') depth++;
                        else if (lp[c] == '(') depth--;
                        if (depth > 0) c--;
                    }
                    if (c < 0) break;
                    /* c is at the '(' — skip it and the method name before */
                    c--;
                    while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
                    /* Now skip the identifier (method name) */
                    while (c >= 0 && ((lp[c] >= 'a' && lp[c] <= 'z') ||
                           (lp[c] >= 'A' && lp[c] <= 'Z') ||
                           (lp[c] >= '0' && lp[c] <= '9') ||
                           lp[c] == '_' || (unsigned char)lp[c] >= 0x80))
                        c--;
                    /* Now c should be on '.' or start of identifier */
                    if (c >= 0 && lp[c] == '.') {
                        end = c; /* continue walking back through the chain */
                    } else {
                        end = c + 1; /* landed on root identifier end */
                        break;
                    }
                }

                /*  — symetrique du walk `)` ci-dessus pour les crochets :
                 * `TABLE_X[1][2]..` finissait sur un `]`, qui n'est pas un
                 * caractere d'identifiant — le receveur n'etait donc JAMAIS
                 * extrait (start == end) et la completion rendait 0 item.
                 * On saute chaque groupe equilibre `[...]` de droite a gauche,
                 * jusqu'a retomber sur la fin d'un identifiant.
                 *
                 * Un `]` non apparie (l'utilisateur en pleine frappe :
                 * `TABLE_X[1][..`) fait sortir la boucle sans consommer : la
                 * recherche du `[` s'arrete a c < 0 et on `break`. Le compteur
                 * ne peut pas repartir en arriere, donc pas de boucle infinie
                 * ni de lecture hors bornes. */
                bool bracketWalked = false;
                while (end > 0 && lp[end-1] == ']') {
                    int depth = 1;
                    int c = end - 2;
                    while (c >= 0 && depth > 0) {
                        if (lp[c] == ']') depth++;
                        else if (lp[c] == '[') depth--;
                        if (depth > 0) c--;
                    }
                    if (c < 0) break;   /* crochet non apparie : on abandonne */
                    end = c;            /* c est sur le `[` */
                    bracketWalked = true;
                    receiverBracketGroups++;
                }

                int start = end;
                while (start > 0 && ((lp[start-1] >= 'a' && lp[start-1] <= 'z') ||
                       (lp[start-1] >= 'A' && lp[start-1] <= 'Z') ||
                       (lp[start-1] >= '0' && lp[start-1] <= '9') ||
                       lp[start-1] == '_' || (unsigned char)lp[start-1] >= 0x80))
                    start--;
                if (end > start && end - start < 128) {
                    char varName[128];
                    memcpy(varName, lp + start, end - start);
                    varName[end - start] = '\0';
                    snprintf(receiverRootName, sizeof(receiverRootName),
                             "%s", varName);

                    /*  — receveur reflexif MoiMême / MySelf : designe le
                     * champ courant. Le plus souvent MoiMême vit dans une
                     * procedure agnostique (partagee par plusieurs champs) : le
                     * type reel n'est connu qu'au runtime. On ne tente donc aucune
                     * resolution contextuelle et on marque le receveur pour
                     * proposer le socle commun des proprietes de champ (cf bloc
                     * __SELF__ plus bas). Comparaison canon : accents strippes +
                     * casse-insensible (MoiMême == moimeme == MoiMeme). */
                    {
                        char vnAscii[128];
                        scanner_strip_accents(varName, (int)strlen(varName),
                                              vnAscii, sizeof(vnAscii));
                        if (strcasecmp(vnAscii, "MoiMeme") == 0 ||
                            strcasecmp(vnAscii, "MySelf") == 0) {
                            /*  — MoiMême = champ ∪ Fenêtre ∪ État (aligne
                             * la validation  et la résolution runtime
                             * ). ReceiverType DISTINCT de MonChamp : emet
                             * l'union (socle champ + props Fenetre + props État),
                             * là où MonChamp reste sur le socle champ seul. */
                            strncpy(receiverType, "__SELF_WIDE__",
                                    sizeof(receiverType)-1);
                        }
                        /*  — receveurs reflexifs figés MonChamp / MyControl :
                         * comme MoiMême, designent le champ courant (variante stricte
                         * sans repli fenetre). Meme socle générique de champ que
                         * __SELF__ (type reel resolu au runtime). */
                        else if (strcasecmp(vnAscii, "MonChamp") == 0 ||
                                 strcasecmp(vnAscii, "MyControl") == 0) {
                            strncpy(receiverType, "__SELF__",
                                    sizeof(receiverType)-1);
                        }
                        /*  — receveur reflexif figé MaFenêtre / MyWindow :
                         * designe la fenetre courante → props de Fenetre (kind
                         * Fenetre du catalogue, comme FEN_X.. → DSL_F). */
                        else if (strcasecmp(vnAscii, "MaFenetre") == 0 ||
                                 strcasecmp(vnAscii, "MyWindow") == 0) {
                            strncpy(receiverType, "DSL_F",
                                    sizeof(receiverType)-1);
                        }
                    }

                    /*  — chaine style inline `<racine>..Style..<sousElem>..` :
                     * le sous-element (Image, Libellé, CadreExtérieur...) doit
                     * resoudre le SOUS-TYPE du type de style du widget courant,
                     * PAS un type global homonyme (`Image` = type bitmap). La
                     * collision de noms fait que le fallback native/type global
                     * (plus bas) capture `Image` avant qu'on ait le bon contexte.
                     * On resout ici, en priorite : on remonte le segment parent
                     * (juste avant `..varName`) et, s'il designe un type composé
                     * (soit directement, soit via la prop-widget réflexive
                     * `Style`→styleBouton), on cherche varName comme field
                     * `instance` de ce parent → subTypeName. */
                    if (start >= 2 && lp[start-1] == '.' && lp[start-2] == '.') {
                        int pe = start - 2; /* fin du segment parent */
                        int ps = pe;
                        while (ps > 0 && ((lp[ps-1] >= 'a' && lp[ps-1] <= 'z') ||
                               (lp[ps-1] >= 'A' && lp[ps-1] <= 'Z') ||
                               (lp[ps-1] >= '0' && lp[ps-1] <= '9') ||
                               lp[ps-1] == '_' || (unsigned char)lp[ps-1] >= 0x80))
                            ps--;
                        if (pe > ps && pe - ps < 64) {
                            char parentSeg[64];
                            memcpy(parentSeg, lp + ps, pe - ps);
                            parentSeg[pe - ps] = '\0';
                            /* Resoudre parentSeg -> type composé, en PRIORISANT
                             * le contexte composite. Deux candidats parent :
                             *  (A) prop-widget réflexive : parentSeg est une prop
                             *      du widget courant dont l'objectType est un type
                             *      composé (Style→styleBouton sur un Bouton). Ce
                             *      candidat porte les sous-elements widget-spécifiques
                             *      (Image, Libellé, CadreExtérieur).
                             *  (B) type composé homonyme direct (parentSeg == nom
                             *      de type composé, ex Cadre, ou le `Style` générique).
                             * On tente (A) d'abord et on ne retient que si le field
                             * varName y existe comme instance ; sinon (B). Cela
                             * empeche le `Style` générique (sans Image) de masquer
                             * le styleBouton (avec Image), et écarte le type global
                             * bitmap homonyme. */
                            const char* sub = NULL;
                            int co = 0, cl = 0;
                            const char* pp = src;
                            while (*pp && cl < line) {
                                if (*pp == '\n') cl++;
                                pp++; co++;
                            }
                            co += character;
                            char wctx = detect_dsl_context_ex(src, co).widget;
                            const OwlDSLPropInfo* dp = dsl_prop_find(wctx, parentSeg);
                            if (dp && dp->objectType && *dp->objectType &&
                                is_composed_type_name(dp->objectType)) {
                                sub = composed_field_subtype(dp->objectType, varName);
                            }
                            if (!sub && is_composed_type_name(parentSeg)) {
                                sub = composed_field_subtype(parentSeg, varName);
                            }
                            if (sub)
                                strncpy(receiverType, sub, sizeof(receiverType)-1);
                        }
                    }

                    /* DSL Fenetre v1 : check si varName est une fenetre
                     * ('FEN_X est une Fenetre' → DSL_F) ou un sous-champ
                     * ('SAI_User est une Saisie' → DSL_S). */
                    for (int i = 0; i < userWindowCount; i++) {
                        if (strcasecmp(varName, userWindows[i].name) == 0) {
                            snprintf(receiverType, sizeof(receiverType), "DSL_F");
                            break;
                        }
                    }
                    if (receiverType[0] == '\0') {
                        for (int i = 0; i < userControlCount; i++) {
                            if (strcasecmp(varName, userControls[i].name) == 0) {
                                snprintf(receiverType, sizeof(receiverType),
                                         "DSL_%c", userControls[i].kind);
                                break;
                            }
                        }
                    }
                    /*  — receiver est un Groupe (GR_X..) */
                    if (receiverType[0] == '\0') {
                        for (int i = 0; i < userGroupCount; i++) {
                            if (strcasecmp(varName, userGroups[i].name) == 0) {
                                strncpy(receiverType, "DSL_GROUP",
                                        sizeof(receiverType)-1);
                                break;
                            }
                        }
                    }
                    /*  — receveur colonne nu `COL_A..`. La colonne n'est
                     * pas un controle (absente de userControls) mais un
                     * sous-element de Table. Le compilateur desucre `COL_A..prop`
                     * vers le proxy-colonne et n'y accepte QUE Couleur/CouleurFond
                     * ; la completion propose donc le genre synthetique
                     * CelluleTable (ces deux props exactement), pas le catalogue
                     * Colonne complet — sinon elle promettrait Libellé/Visible/...
                     * que l'ecriture nue refuse. Hors crochet seulement (la forme
                     * `COL_A[1]..` est geree/refusee par le bloc bracketWalked) et
                     * si la colonne appartient a exactement une Table (sinon
                     * ambigu -> refus compilateur, pas de promesse). */
                    if (receiverType[0] == '\0' && !bracketWalked) {
                        const char* ownerTable = NULL;
                        bool ambiguousCol = false;
                        for (int i = 0; i < userColumnCount; i++) {
                            if (strcasecmp(varName, userColumns[i].name) != 0)
                                continue;
                            if (ownerTable == NULL) {
                                ownerTable = userColumns[i].table;
                            } else if (strcasecmp(ownerTable,
                                                  userColumns[i].table) != 0) {
                                ambiguousCol = true;
                                break;
                            }
                        }
                        if (ownerTable != NULL && !ambiguousCol) {
                            strncpy(receiverType, "DSL_k",
                                    sizeof(receiverType)-1);
                        }
                    }
                    /* Look up variable type in userSymbols */
                    if (receiverType[0] == '\0') {
                        for (int i = 0; i < userSymbolCount; i++) {
                            if (userSymbols[i].kind == 6 &&
                                strcasecmp(varName, userSymbols[i].name) == 0) {
                                strncpy(receiverType, userSymbols[i].detail, sizeof(receiverType)-1);
                                break;
                            }
                        }
                    }
                    /* Fallback: if the identifier is a native function that returns
                     * a builtin type (e.g. MockCrée → MockNative), propose methods
                     * of that return type. */
                    if (receiverType[0] == '\0') {
                        const NativeInfo* fn = catalog_find_native(varName);
                        if (fn && fn->signatureCount > 0 &&
                            fn->signatures[0].returnType &&
                            fn->signatures[0].returnType[0]) {
                            strncpy(receiverType, fn->signatures[0].returnType,
                                    sizeof(receiverType)-1);
                        }
                    }
                    /*  — 1er hop `Style..` racine (bloc PROPRIETES) :
                     * amorcer la descente avec le type de style RÉFLEXIF du
                     * widget courant (styleSaisie, styleBouton...) et non le
                     * type composé homonyme GÉNÉRIQUE `Style`. Symétrique au 2e
                     * hop (~5547) et à la racine implicite `Etats` (~4767) :
                     * `dsl_prop_find(wctx, varName)->objectType` porte le type
                     * réflexif (styleSaisie), là où le fallback générique (bloc
                     * suivant) capturerait le composé homonyme (10 champs
                     * Couleur/CurseurSouris/Survol...). Parité menu via
                     * menu_kind_style_type. Le fallback générique ne s'applique
                     * QUE si aucune prop-widget réflexive ne matche (préserve un
                     * vrai récepteur composé nommé `Style` hors contexte widget). */
                    if (receiverType[0] == '\0') {
                        int co = 0, cl = 0;
                        const char* pp = src;
                        while (*pp && cl < line) {
                            if (*pp == '\n') cl++;
                            pp++; co++;
                        }
                        co += character;
                        const char* rt = NULL;
                        char vnAsc[128];
                        scanner_strip_accents(varName, (int)strlen(varName),
                                              vnAsc, sizeof(vnAsc));
                        if (strcasecmp(vnAsc, "Style") == 0) {
                            char menuChar = detect_dsl_context(src, co);
                            rt = menu_kind_style_type(menuChar);
                        }
                        if (!rt) {
                            char wctx = detect_dsl_context_ex(src, co).widget;
                            const OwlDSLPropInfo* dp = dsl_prop_find(wctx, varName);
                            if (dp && dp->objectType && *dp->objectType &&
                                is_composed_type_name(dp->objectType))
                                rt = dp->objectType;
                        }
                        if (rt)
                            strncpy(receiverType, rt, sizeof(receiverType)-1);
                    }
                    /* Chain `widget..Cadre..` ou `inst..Police..` : si varName
                     * est le nom d'un type composé (Cadre, Style, Police, Coin,
                     * Trait), on a affaire à une sous-prop typée. receiverType
                     * = nom du type composé pour proposer ses fields. */
                    if (receiverType[0] == '\0') {
                        for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                            const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                            if (strcasecmp(f->typeName, varName) == 0) {
                                strncpy(receiverType, f->typeName, sizeof(receiverType)-1);
                                break;
                            }
                        }
                    }
                    /* Bug #5 : varName est une *constante* d'enum (BordBas,
                     * RougeFonce, ...) → marqueur "constant" pour bloquer
                     * la completion en aval. */
                    if (receiverType[0] == '\0') {
                        for (int e = 0; e < OWL_ENUM_CATALOG_COUNT; e++) {
                            if (owl_enum_info_contains(&OWL_ENUM_CATALOG[e], varName)) {
                                strncpy(receiverType, "__CONST__", sizeof(receiverType)-1);
                                break;
                            }
                        }
                    }
                    /* Bug #D : varName est un fieldName d'un type composé
                     * et ce field est *scalaire* (int/bool/string/enum) →
                     * pas de chain `..` sensible derrière (ex: `..Bords..`,
                     * `..Epaisseur..`). Marqueur __CONST__ pour bloquer. */
                    if (receiverType[0] == '\0') {
                        for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                            const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                            if (strcasecmp(f->fieldName, varName) != 0) continue;
                            if (strcmp(f->fieldType, "instance") != 0) {
                                strncpy(receiverType, "__CONST__", sizeof(receiverType)-1);
                                break;
                            }
                        }
                    }
                    /*  — receveur atteint a travers des crochets. Seul
                     * un Champ Table porte des proprietes a ce niveau
                     * (`TABLE_X[ligne][colonne]..`, `TABLE_X[ligne]..`) : on
                     * bascule sur le genre synthetique CelluleTable. Pour tout
                     * autre receveur indexe (tableau, ZoneRepetee, variable),
                     * on efface le type resolu : l'element indexe n'est PAS le
                     * receveur, et proposer ses proprietes serait une fausse
                     * surface. Sans crochet parcouru, rien ne change. */
                    if (bracketWalked) {
                        if (strcmp(receiverType, "DSL_T") == 0) {
                            strncpy(receiverType, "DSL_k",
                                    sizeof(receiverType)-1);
                        } else if (table_colfirst_root_is_table(lp, start)) {
                            /*  — forme colonne-first
                             * `TABLE_X.COL_A[1]..` : le walk des crochets
                             * s'arrete sur le nom de COLONNE, qui n'est pas un
                             * controle declare — d'ou receiverType vide. On
                             * remonte d'un segment : si la racine est un Champ
                             * Table, le receveur est bien une cellule. */
                            strncpy(receiverType, "DSL_k",
                                    sizeof(receiverType)-1);
                        } else {
                            receiverType[0] = '\0';
                        }
                    }
                }
            }
        }
    }

    /*  — `icones.<prefix>` : court-circuite tout le reste.
     * Le catalog est large (~47k) ; le filtre prefix + limite 50 garantit
     * la coupure. Le client VSCode re-filtrera quand l'utilisateur
     * continuera de taper. */
    if (iconesPrefixContext) {
        build_icones_prefix_items(iconesPrefix, items);
        return items;
    }

    /*  B2 — `theme.<prefix>` : court-circuite le reste. Resout le
     * theme actif (projet du fichier courant → Projet..Thème → fichier de
     * styles) et propose les styles nommes, filtres best-effort par type de
     * champ si un `..Style = theme.` est detectable (non filtre sinon). */
    if (themePrefixContext) {
        JsonValue* tdT = json_get(params, "textDocument");
        const char* uriT = tdT ? json_get_string(tdT, "uri") : NULL;
        char curPath[1024] = "";
        if (uriT) uri_to_filepath(uriT, curPath, sizeof(curPath));
        theme_ensure_scan(curPath);
        build_theme_prefix_items(themePrefix,
                                 themeFilterStyleType[0] ? themeFilterStyleType : NULL,
                                 items);
        return items;
    }

    /* Type-declaration context: après `nom est [un|une]` — propose
     * uniquement les types (primitives + builtin types + user-defined types
     * détectés dans le document). Si l'article `un/une` est absent, l'insertion
     * le préfixe automatiquement. */
    /*  C3 — `tableau de <cursor>` reutilise l'emission de types du
     * typeDecl, mais SANS prefixer d'article (`tableau de entier`, pas
     * `tableau de un entier`). On force donc l'article comme "deja present". */
    if (collTypeContext) typeDeclHasArticle = true;

    if (controlModifierContext) {
        /*  — après `est un Champ`/`is a Control|Field`, proposer les types
         * de contrôle concrets (CONTROL_KINDS is_widget=true : Bouton, Saisie,
         * Libellé, Table…), PAS les primitifs. Label FR canon (mode FR) / EN canon
         * (mode EN) comme le reste de la complétion. Source de vérité unique :
         * owl_control_kinds_table() (ui_decl.c CONTROL_KINDS). */
        /*  — sous-cas "collé" (`est un Champ|`) : le mot courant côté
         * client est le modificateur → il masquerait les types par filtrage.
         * On pose un textEdit d'insertion à la position du curseur (préfixe de
         * filtre vide) qui appose " <Type>" après le modificateur. Position en
         * UTF-16 brut (repère LSP), relue depuis params. */
        int cmLine = 0, cmChar = 0;
        if (controlModifierGlued) {
            JsonValue* pcm = json_get(params, "position");
            cmLine = (int)json_get_int(pcm, "line", 0);
            cmChar = (int)json_get_int(pcm, "character", 0);
        }
        for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
            if (!k->is_widget) continue;
            const char* label = (wantEN && k->nameEN && *k->nameEN) ? k->nameEN : k->nameFR;
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(22)); /* Struct/Class */
            /* filterText ASCII strippé (matcher `Libelle` en tapant sans accent). */
            char ftBuf[64];
            int ftLen = scanner_strip_accents(label, (int)strlen(label), ftBuf, sizeof(ftBuf));
            if (ftLen > 0 && (size_t)ftLen != strlen(label))
                json_object_set(item, "filterText", json_new_string(ftBuf));
            const char* det = (wantEN && k->descEN && *k->descEN) ? k->descEN
                            : (k->descFR && *k->descFR ? k->descFR
                               : (wantEN ? "Control type" : "Type de contrôle"));
            json_object_set(item, "detail", json_new_string(det));
            /*  — squelette leger auto-fermant : le widget ouvre son
             * bloc PROPRIETES puis son propre FIN (comme `owl new champ`, version
             * minimale). `$0` place le curseur dans le PROPRIETES. Le contexte
             * `est un Champ ...` leve l'ambiguite variable/widget → snippet sur. */
            const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
            const char* endKw  = wantEN ? "END" : "FIN";
            char wbody[160];
            snprintf(wbody, sizeof wbody, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                     label, propKw, endKw, endKw);
            if (controlModifierGlued) {
                /* Range zéro-large au curseur → préfixe de filtre vide (tout
                 * s'affiche) ; newText appose " <Type>" + squelette après le
                 * modificateur. */
                JsonValue* st = json_new_object();
                json_object_set(st, "line", json_new_int(cmLine));
                json_object_set(st, "character", json_new_int(cmChar));
                JsonValue* en = json_new_object();
                json_object_set(en, "line", json_new_int(cmLine));
                json_object_set(en, "character", json_new_int(cmChar));
                JsonValue* rg = json_new_object();
                json_object_set(rg, "start", st);
                json_object_set(rg, "end", en);
                JsonValue* te = json_new_object();
                json_object_set(te, "range", rg);
                char nt[176];
                snprintf(nt, sizeof nt, " %s", wbody);
                json_object_set(te, "newText", json_new_string(nt));
                json_object_set(item, "textEdit", te);
            } else {
                json_object_set(item, "insertText", json_new_string(wbody));
            }
            json_object_set(item, "insertTextFormat", json_new_int(2)); /* snippet */
            json_array_push(items, item);
        }
        /*  — court-circuit : après `est un Champ`, on ne propose QUE les
         * types de contrôle. Sans ce return, l'exécution retombait sur la
         * complétion générale en aval qui, en cas "collé" (mot courant =
         * "Champ"), filtrait par ce préfixe et éliminait tous les types de
         * contrôle (aucun ne contient "champ"). Aligne le comportement sur la
         * branche typeDecl (qui retourne déjà). */
        return items;
    } else if (typeDeclContext || collTypeContext) {
        /* Primitive types — source de verite unique OWL_PRIM_TYPES,
         * partagee avec le dump general. Emission d'UNE seule forme selon la
         * langue active (fin de la double-emission bilingue) : mode EN ->
         * forme EN canon si distincte sinon fallback FR ; mode FR -> forme FR
         * canon. Le runtime reste bilingue ; seule la completion segmente. */
        for (int i = 0; OWL_PRIM_TYPES[i].name; i++) {
            const OwlPrimType* p = &OWL_PRIM_TYPES[i];
            const char* label = owl_prim_label(p, wantEN);
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(25)); /* TypeParameter */
            json_object_set(item, "detail",
                json_new_string(wantEN && p->descEN ? p->descEN : p->desc));
            /* filterText : forme ASCII strippee du label, pour matcher sans
             * accent. */
            size_t labelLen = strlen(label);
            char ftBuf[64];
            int ftLen = scanner_strip_accents(label, (int)labelLen,
                                              ftBuf, sizeof(ftBuf));
            if (ftLen > 0 && (size_t)ftLen != labelLen) {
                json_object_set(item, "filterText", json_new_string(ftBuf));
            }
            /* Si l'article manque : en FR on insère l'article de confort
             * ("un booléen"), correct car géré par genre. En EN on n'insère
             * plus d'article (, support #271) : le "a" hardcodé était
             * grammaticalement faux pour les types à son voyelle ("a integer").
             * insertText = type seul. Le runtime accepte l'article SI tapé. */
            if (!typeDeclHasArticle) {
                char insertText[160];
                if (wantEN) {
                    snprintf(insertText, sizeof(insertText), "%s", label);
                } else {
                    snprintf(insertText, sizeof(insertText), "%s %s",
                             p->article, label);
                }
                json_object_set(item, "insertText", json_new_string(insertText));
            }
            json_array_push(items, item);
        }
        /* Builtin registered types (Chrono, FichierDisque, httpRequête…) */
        for (int i = 0; i < OWL_TYPE_CATALOG_COUNT; i++) {
            const OwlTypeInfo* t = &OWL_TYPE_CATALOG[i];
            /*  — bascule EN comme les primitifs au-dessus et le dump
             * general des types (~L5720) : en mode EN, label = nameEN si une
             * forme EN distincte existe, sinon fallback FR (canon accentue). */
            bool tHasEN = (t->nameEN && *t->nameEN && strcmp(t->name, t->nameEN) != 0);
            const char* tLabel = (wantEN && tHasEN) ? t->nameEN : t->name;
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(tLabel));
            /*  /  — filterText combine toutes les formes
             * (label affiche + canon FR + ascii + EN) pour matcher que l'on
             * tape `multi` (EN) ou `chaine` (FR ascii) quel que soit le label. */
            {
                char ft[320];
                int off = snprintf(ft, sizeof(ft), "%s %s", tLabel, t->name);
                if (t->nameAscii && *t->nameAscii &&
                    strcmp(t->nameAscii, t->name) != 0)
                    off += snprintf(ft + off, sizeof(ft) - off, " %s", t->nameAscii);
                if (tHasEN)
                    snprintf(ft + off, sizeof(ft) - off, " %s", t->nameEN);
                json_object_set(item, "filterText", json_new_string(ft));
            }
            json_object_set(item, "kind", json_new_int(22)); /* Struct */
            char detailBuf[256];
            const char* tDesc = (wantEN && t->descriptionEN && *t->descriptionEN)
                                ? t->descriptionEN
                                : (t->description ? t->description : "");
            snprintf(detailBuf, sizeof(detailBuf), "[%s] %s",
                     t->category ? t->category : (wantEN ? "Misc" : "Divers"),
                     tDesc);
            json_object_set(item, "detail", json_new_string(detailBuf));
            if (!typeDeclHasArticle) {
                /*  — pas d'article de confort en EN, FR inchangé. */
                char insertText[256];
                if (wantEN) {
                    snprintf(insertText, sizeof(insertText), "%s", tLabel);
                } else {
                    snprintf(insertText, sizeof(insertText), "un %s", tLabel);
                }
                json_object_set(item, "insertText", json_new_string(insertText));
            }
            json_array_push(items, item);
        }
        /* User-defined struct types from the current document */
        for (int i = 0; i < userStructCount; i++) {
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(userStructs[i].name));
            json_object_set(item, "kind", json_new_int(22));
            json_object_set(item, "detail", json_new_string(
                wantEN ? "User type (structure/class)"
                       : "Type utilisateur (structure/classe)"));
            if (!typeDeclHasArticle) {
                /*  — pas d'article de confort en EN, FR inchangé. */
                char insertText[400];
                snprintf(insertText, sizeof(insertText),
                         wantEN ? "%.128s" : "un %.128s", userStructs[i].name);
                json_object_set(item, "insertText", json_new_string(insertText));
            }
            json_array_push(items, item);
        }
        /*  — types DSL top-level (Fenetre/Etat) : emis avec un snippet
         * auto-fermant (bloc + FIN/END). Le support #120 demandait la fermeture
         * auto comme PROCEDURE/CONSTANTE ont deja. Scope limite a ces deux
         * ouvreurs de bloc top-level (les autres control kinds Bouton/Saisie/...
         * ne se declarent pas seuls au top-level). Corps de snippet locale-aware :
         * FR canon (PROPRIETES/FIN) ou EN canon (PROPERTIES/END). Formes EN du
         * type derivees de ui_decl.c CONTROL_KINDS (Fenetre->Window,
         * Etat->Report). Un Etat exige obligatoirement un bloc Corps -> le
         * scaffold minimal l'inclut pour compiler clean. */
        static const struct {
            const char* labelFR;   /* canon FR accentue */
            const char* labelEN;   /* canon EN */
            const char* articleFR; /* article FR devant le type */
            const char* bodyFR;    /* corps snippet FR (sans l'article) */
            const char* bodyEN;    /* corps snippet EN (sans l'article) */
            const char* detailFR;
            const char* detailEN;
        } DSL_TYPE_SNIPPETS[] = {
            {"Fen\xc3\xaatre", "Window", "une",
             "Fen\xc3\xaatre(${1})\n\tPROPRIETES\n\t\t$0\n\tFIN\nFIN",
             "Window(${1})\n\tPROPERTIES\n\t\t$0\n\tEND\nEND",
             "Fen\xc3\xaatre d'interface (bloc PROPRIETES)",
             "Interface window (PROPERTIES block)"},
            {"\xc3\x89tat", "Report", "un",
             "\xc3\x89tat\n\t${1:corps} est un Corps\n\t\t$0\n\tFIN\nFIN",
             "Report\n\t${1:body} is a Body\n\t\t$0\n\tEND\nEND",
             "Document imprimable (bloc Corps obligatoire)",
             "Printable report (mandatory Body block)"},
            /*  (support #215) — Structure et Classe manquaient aux
             * candidats type-decl alors que `X est une Structure`/`Classe` est
             * une declaration valide. Meme mecanique auto-fermante que ci-dessus.
             * Structure = meme mot FR/EN (scanner: seul "structure") ; Classe ->
             * EN "Class" (scanner accepte "class"). Article "une" (feminin) pour
             * les deux. Corps minimal valide (parse clean, verifie empiriquement) :
             * la Structure liste des champs ; la Classe montre l'idiome PUBLIC. */
            {"Structure", "Structure", "une",
             "Structure\n\t${1:champ} est une cha\xc3\xaene\n\t$0\nFIN",
             "Structure\n\t${1:field} is a string\n\t$0\nEND",
             "Structure de donn\xc3\xa9""es (champs nomm\xc3\xa9s)",
             "Data structure (named fields)"},
            {"Classe", "Class", "une",
             "Classe\n\tPUBLIC\n\t\t${1:champ} est une cha\xc3\xaene\n\t\t$0\nFIN",
             "Class\n\tPUBLIC\n\t\t${1:field} is a string\n\t\t$0\nEND",
             "Classe (membres + m\xc3\xa9thodes, visibilit\xc3\xa9 PUBLIC/PRIV\xc3\x89)",
             "Class (members + methods, PUBLIC/PRIVATE visibility)"},
            /*  — Th\xc3\xa8me : conteneur de styles nomm\xc3\xa9s.
             * Corps = liste de styles (`nom est un styleXxx = { ... }`), PAS un
             * bloc PROPRIETES. Article masculin "un". */
            {"Th\xc3\xa8me", "Theme", "un",
             "Th\xc3\xa8me\n\t${1:styleBtn} est un styleBouton = { $0 }\nFIN",
             "Theme\n\t${1:styleBtn} is a styleButton = { $0 }\nEND",
             "Conteneur de styles nomm\xc3\xa9s (r\xc3\xa9utilisables par les champs)",
             "Container of named styles (reusable by controls)"},
        };
        for (size_t d = 0; d < sizeof(DSL_TYPE_SNIPPETS)/sizeof(DSL_TYPE_SNIPPETS[0]); d++) {
            const char* label = wantEN ? DSL_TYPE_SNIPPETS[d].labelEN
                                       : DSL_TYPE_SNIPPETS[d].labelFR;
            const char* body  = wantEN ? DSL_TYPE_SNIPPETS[d].bodyEN
                                       : DSL_TYPE_SNIPPETS[d].bodyFR;
            const char* detail = wantEN ? DSL_TYPE_SNIPPETS[d].detailEN
                                        : DSL_TYPE_SNIPPETS[d].detailFR;
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(22)); /* Struct */
            json_object_set(item, "detail", json_new_string(detail));
            /* filterText ascii pour matcher sans accent (Fenetre -> Fenêtre),
             * meme pattern que le bloc primitifs ci-dessus. */
            size_t labelLen = strlen(label);
            char ftBuf[64];
            int ftLen = scanner_strip_accents(label, (int)labelLen, ftBuf, sizeof(ftBuf));
            if (ftLen > 0 && (size_t)ftLen != labelLen) {
                json_object_set(item, "filterText", json_new_string(ftBuf));
            }
            /* insertText = snippet bloc auto-fermant. Prefixe l'article FR si
             * absent du contexte (`X est <cursor>` -> "une Fenetre..."). En EN
             * on n'insère plus d'article de confort. */
            char insertText[256];
            if (typeDeclHasArticle || wantEN) {
                snprintf(insertText, sizeof(insertText), "%s", body);
            } else {
                snprintf(insertText, sizeof(insertText), "%s %s",
                         DSL_TYPE_SNIPPETS[d].articleFR, body);
            }
            json_object_set(item, "insertText", json_new_string(insertText));
            json_object_set(item, "insertTextFormat", json_new_int(2)); /* snippet */
            json_array_push(items, item);
        }
        /*  — ouvreurs de bloc menu en position typeDecl
         * (`MNU_X est un <cursor>`). Ce ne sont pas des widgets posables
         * (is_widget=false) donc absents des boucles au-dessus, mais on les
         * declare exactement comme un type (`est un MenuPrincipal`). Source de
         * verite : owl_control_kinds_table() filtree par is_menu_block_type_name
         * (MenuPrincipal/SousMenu/OptionMenu/MenuSeparateur) + MenuContextuel
         * (hors table). Labels FR/EN canon, meme bascule wantEN que le reste. */
        for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
            if (!is_menu_block_type_name(k->nameFR)) continue;
            const char* label = (wantEN && k->nameEN && *k->nameEN) ? k->nameEN : k->nameFR;
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(22)); /* Struct/Class */
            char ftBuf[64];
            int ftLen = scanner_strip_accents(label, (int)strlen(label), ftBuf, sizeof(ftBuf));
            if (ftLen > 0 && (size_t)ftLen != strlen(label))
                json_object_set(item, "filterText", json_new_string(ftBuf));
            const char* det = (wantEN && k->descEN && *k->descEN) ? k->descEN
                            : (k->descFR && *k->descFR ? k->descFR
                               : (wantEN ? "Menu element" : "Élément de menu"));
            json_object_set(item, "detail", json_new_string(det));
            /*  — squelette leger auto-fermant : bloc menu +
             * PROPRIETES + FIN (un MenuPrincipal/SousMenu/OptionMenu/
             * MenuSeparateur accepte un bloc PROPRIETES, verifie a la compil). */
            {
                const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
                const char* endKw  = wantEN ? "END" : "FIN";
                char body[192];
                snprintf(body, sizeof body, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                         label, propKw, endKw, endKw);
                char insertText[224];
                if (typeDeclHasArticle || wantEN) {
                    /*  — pas d'article de confort en EN, FR inchangé. */
                    snprintf(insertText, sizeof insertText, "%s", body);
                } else {
                    snprintf(insertText, sizeof insertText, "un %s", body);
                }
                json_object_set(item, "insertText", json_new_string(insertText));
                json_object_set(item, "insertTextFormat", json_new_int(2));
            }
            json_array_push(items, item);
        }
        /*  — sous-blocs d'un Etat en position typeDecl
         * (`INTRO est un <cursor>` dans un bloc Etat). Comme les blocs menu :
         * pas des widgets posables (is_widget=false) donc absents des boucles
         * au-dessus, mais declares exactement comme un type. Source de verite :
         * owl_control_kinds_table() filtree par is_etat_block_type_name. Gate
         * sur le contexte Etat direct pour ne pas polluer la completion des
         * fenetres (ces blocs n'y ont aucun sens). */
        bool inEtatBlock = false;
        /*  (support #309) — contexte Champ Table ('T') pour offrir le
         * sous-element `Colonne`/`Column` (voir l'emission plus bas). Meme calcul
         * de contexte DSL que le gate Etat, factorise en un seul char. */
        char typeDeclDslCtx = 0;
        if (td && pos) {
            const char* uriE = json_get_string(td, "uri");
            int lineE = (int)json_get_int(pos, "line", 0);
            int charE = (int)json_get_int(pos, "character", 0);
            int idxE = find_doc(uriE);
            if (idxE >= 0) {
                const char* srcE = lspServer.documents[idxE].content;
                int offE = 0, curLineE = 0;
                const char* pE = srcE;
                while (*pE && curLineE < lineE) {
                    if (*pE == '\n') curLineE++;
                    pE++; offE++;
                }
                offE += lsp_utf16_to_byte_in_line(pE, charE);
                typeDeclDslCtx = detect_dsl_context_ex(srcE, offE).widget;
                inEtatBlock = (typeDeclDslCtx == 'E');
            }
        }
        if (inEtatBlock) {
            for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
                if (!is_etat_block_type_name(k->nameFR)) continue;
                const char* label = (wantEN && k->nameEN && *k->nameEN) ? k->nameEN
                                                                       : k->nameFR;
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(label));
                json_object_set(item, "kind", json_new_int(22)); /* Struct/Class */
                char ftBuf[64];
                int ftLen = scanner_strip_accents(label, (int)strlen(label),
                                                  ftBuf, sizeof(ftBuf));
                if (ftLen > 0 && (size_t)ftLen != strlen(label))
                    json_object_set(item, "filterText", json_new_string(ftBuf));
                const char* det = (wantEN && k->descEN && *k->descEN) ? k->descEN
                                : (k->descFR && *k->descFR ? k->descFR
                                   : (wantEN ? "Report block" : "Bloc d'état"));
                json_object_set(item, "detail", json_new_string(det));
                /*  — squelette leger auto-fermant : sous-bloc Etat +
                 * PROPRIETES + FIN (un sous-bloc d'Etat accepte un bloc
                 * PROPRIETES, ex. `PROPRIETES Hauteur=30 FIN`). */
                {
                    const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
                    const char* endKw  = wantEN ? "END" : "FIN";
                    char body[192];
                    snprintf(body, sizeof body, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                             label, propKw, endKw, endKw);
                    char insertText[224];
                    if (typeDeclHasArticle || wantEN) {
                        /*  — pas d'article de confort en EN, FR inchangé. */
                        snprintf(insertText, sizeof insertText, "%s", body);
                    } else {
                        snprintf(insertText, sizeof insertText, "un %s", body);
                    }
                    json_object_set(item, "insertText", json_new_string(insertText));
                    json_object_set(item, "insertTextFormat", json_new_int(2));
                }
                json_array_push(items, item);
            }
        }
        /*  (support #309) — `COL_X est une <cursor>` dans le corps d'un
         * Champ Table : proposer le sous-element `Colonne`/`Column`. Colonne est
         * is_widget=false (sous-element de Table, pas un widget posable) donc
         * absente de la boucle CONTROL_KINDS ci-dessus -> jamais offerte, alors
         * que c'est la seule declaration sensee a ce niveau. Gate sur le contexte
         * Table ('T') pour ne pas polluer la completion ailleurs (le compilateur
         * refuse une Colonne hors Table). Source de verite : owl_control_kinds_table()
         * filtree sur le nom canon Colonne. Squelette auto-fermant (bloc PROPRIETES
         * + FIN), meme mecanique que les blocs menu/Etat. */
        if (typeDeclDslCtx == 'T') {
            for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
                if (strcasecmp(k->nameFR, "Colonne") != 0) continue;
                const char* label = (wantEN && k->nameEN && *k->nameEN) ? k->nameEN
                                                                        : k->nameFR;
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(label));
                json_object_set(item, "kind", json_new_int(22)); /* Struct/Class */
                char ftBuf[64];
                int ftLen = scanner_strip_accents(label, (int)strlen(label),
                                                  ftBuf, sizeof(ftBuf));
                if (ftLen > 0 && (size_t)ftLen != strlen(label))
                    json_object_set(item, "filterText", json_new_string(ftBuf));
                const char* det = (wantEN && k->descEN && *k->descEN) ? k->descEN
                                : (k->descFR && *k->descFR ? k->descFR
                                   : (wantEN ? "Table column" : "Colonne de Table"));
                json_object_set(item, "detail", json_new_string(det));
                {
                    const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
                    const char* endKw  = wantEN ? "END" : "FIN";
                    char body[192];
                    snprintf(body, sizeof body, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                             label, propKw, endKw, endKw);
                    char insertText[224];
                    if (typeDeclHasArticle || wantEN) {
                        /*  — pas d'article de confort en EN. Colonne est
                         * feminin -> "une" en FR. */
                        snprintf(insertText, sizeof insertText, "%s", body);
                    } else {
                        snprintf(insertText, sizeof insertText, "une %s", body);
                    }
                    json_object_set(item, "insertText", json_new_string(insertText));
                    json_object_set(item, "insertTextFormat", json_new_int(2));
                }
                json_array_push(items, item);
                break;
            }
        }
        /*  — miroir de  pour `VOL_X est un <cursor>` dans le
         * corps d'un Champ Onglet : proposer le sous-bloc `Volet`/`Pane`. Volet
         * est is_widget=false (sous-bloc d'un Onglet, comme Colonne l'est pour
         * Table) donc absent de la boucle CONTROL_KINDS -> jamais offert. Gate
         * sur le contexte Onglet ('N', cf. detect_dsl_context_ex) pour ne pas
         * polluer ailleurs (le compilateur refuse un Volet hors Onglet). Volet
         * est MASCULIN -> article de confort "un" en FR (Colonne : "une"). */
        if (typeDeclDslCtx == 'N') {
            for (const OwlControlKindInfo* k = owl_control_kinds_table(); k->nameFR; k++) {
                if (strcasecmp(k->nameFR, "Volet") != 0) continue;
                const char* label = (wantEN && k->nameEN && *k->nameEN) ? k->nameEN
                                                                        : k->nameFR;
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(label));
                json_object_set(item, "kind", json_new_int(22)); /* Struct/Class */
                char ftBuf[64];
                int ftLen = scanner_strip_accents(label, (int)strlen(label),
                                                  ftBuf, sizeof(ftBuf));
                if (ftLen > 0 && (size_t)ftLen != strlen(label))
                    json_object_set(item, "filterText", json_new_string(ftBuf));
                const char* det = (wantEN && k->descEN && *k->descEN) ? k->descEN
                                : (k->descFR && *k->descFR ? k->descFR
                                   : (wantEN ? "Tab pane" : "Volet d'un Champ Onglet"));
                json_object_set(item, "detail", json_new_string(det));
                {
                    const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
                    const char* endKw  = wantEN ? "END" : "FIN";
                    char body[192];
                    snprintf(body, sizeof body, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                             label, propKw, endKw, endKw);
                    char insertText[224];
                    if (typeDeclHasArticle || wantEN) {
                        /*  — pas d'article de confort en EN. Volet est
                         * masculin -> "un" en FR. */
                        snprintf(insertText, sizeof insertText, "%s", body);
                    } else {
                        snprintf(insertText, sizeof insertText, "un %s", body);
                    }
                    json_object_set(item, "insertText", json_new_string(insertText));
                    json_object_set(item, "insertTextFormat", json_new_int(2));
                }
                json_array_push(items, item);
                break;
            }
        }
        /* MenuContextuel/ContextMenu — reconnu par is_menu_block_type_name mais
         * absent de CONTROL_KINDS. Emission manuelle. */
        {
            const char* label = wantEN ? "ContextMenu" : "MenuContextuel";
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(22));
            json_object_set(item, "detail", json_new_string(
                wantEN ? "Contextual (right-click) menu of a window."
                       : "Menu contextuel (clic droit) d'une fenêtre."));
            /*  — squelette leger auto-fermant (idem blocs menu). */
            {
                const char* propKw = wantEN ? "PROPERTIES" : "PROPRIETES";
                const char* endKw  = wantEN ? "END" : "FIN";
                char body[160];
                snprintf(body, sizeof body, "%s\n\t%s\n\t\t$0\n\t%s\n%s",
                         label, propKw, endKw, endKw);
                char insertText[192];
                if (typeDeclHasArticle || wantEN) {
                    /*  — pas d'article de confort en EN, FR inchangé. */
                    snprintf(insertText, sizeof insertText, "%s", body);
                } else {
                    snprintf(insertText, sizeof insertText, "un %s", body);
                }
                json_object_set(item, "insertText", json_new_string(insertText));
                json_object_set(item, "insertTextFormat", json_new_int(2));
            }
            json_array_push(items, item);
        }
        return items;
    }

    if (doubleDotContext) {
        /* Bug #5 : chain depuis une constante = aucune completion.
         * Bug #8 : retourner `null` (pas `[]`) pour que VSCode masque
         * la popup "Loading..." plutot que d'afficher rien. */
        if (strcmp(receiverType, "__CONST__") == 0) {
            json_free(items);
            return json_new_null();
        }

        /*  — receveur MoiMême / MySelf : socle commun « champ utile ».
         * MoiMême est generique (type reel resolu au runtime), on ne connait pas
         * le kind vise → on emet un socle usable. Court-circuit (return items)
         * comme les receveurs DSL_.
         *
         * Derivation du socle (pas de liste hardcodee → pas de derive au
         * prochain ajout de prop) :
         *   1. « champ » = widget reel du catalogue (owl_control_kinds_table,
         *      is_widget) hors « Fenetre » (conteneur racine, pas un champ).
         *   2. socle = props exposees par >= 2 kinds champ. Choix produit :
         *      l'intersection stricte (100% des kinds) excluait
         *      Libellé/Valeur/Couleur/État/Grisé (non universelles) et ne
         *      laissait que la geometrie → socle inutile. Le seuil >= 2 inclut
         *      ces props tout en excluant le kind-specific pur (present sur 1
         *      seul kind, ex. TypeBouton). MoiMême etant resolu au runtime,
         *      le faux positif (proposer une prop absente du type runtime reel)
         *      est assume : usabilite > exhaustivite stricte. */
        if (strcmp(receiverType, "__SELF__") == 0 ||
            strcmp(receiverType, "__SELF_WIDE__") == 0) {
            /*  — MoiMême (__SELF_WIDE__) elargit MonChamp (__SELF__) :
             * meme socle champ, PLUS les props de Fenetre et d'État en union
             * (dédup par nom). */
            bool wide = strcmp(receiverType, "__SELF_WIDE__") == 0;
            const char* seen[1024];
            int nSeen = 0;
            const char* champKinds[64];
            int nChamp = 0;
            for (const OwlControlKindInfo* k = owl_control_kinds_table();
                 k->nameFR; k++) {
                if (!k->is_widget) continue;
                const char* kn = k->nameAscii ? k->nameAscii : k->nameFR;
                if (strcmp(kn, "Fenetre") == 0) continue;
                if (nChamp < 64) champKinds[nChamp++] = kn;
            }
            if (nChamp > 0) {
                /* On parcourt tout le catalogue, on ne garde que les props d'un
                 * kind champ, on deduplique par nom canon, et on retient celles
                 * exposees par >= 2 kinds champ distincts. */
                for (int i = 0; i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
                    const OwlDSLPropInfo* p = &OWL_DSL_PROP_CATALOG[i];
                    bool isChamp = false;
                    for (int c = 0; c < nChamp; c++)
                        if (strcmp(p->kind, champKinds[c]) == 0) { isChamp = true; break; }
                    if (!isChamp) continue;
                    bool already = false;
                    for (int s = 0; s < nSeen; s++)
                        if (strcmp(seen[s], p->name) == 0) { already = true; break; }
                    if (already) continue;
                    if (nSeen < 1024) seen[nSeen++] = p->name;
                    /* compte les kinds champ distincts qui exposent ce nom */
                    int kindCount = 0;
                    for (int c = 0; c < nChamp; c++) {
                        for (int j = 0; j < OWL_DSL_PROP_CATALOG_COUNT; j++) {
                            const OwlDSLPropInfo* q = &OWL_DSL_PROP_CATALOG[j];
                            if (strcmp(q->kind, champKinds[c]) != 0) continue;
                            if (strcmp(q->name, p->name) == 0) { kindCount++; break; }
                        }
                    }
                    if (kindCount < 2) continue;
                    /*  — libellé dans la langue active (même bascule
                     * que le bloc PROPRIETES) ; filterText combine les formes. */
                    bool pHasEN = wantEN && p->nameEN
                                  && strcmp(p->nameEN, p->name) != 0;
                    const char* pLabel = pHasEN ? p->nameEN : p->name;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(pLabel));
                    json_object_set(item, "kind", json_new_int(10)); /* Property */
                    json_object_set(item, "sortText", json_new_string("!"));
                    if (pHasEN) {
                        char ft[256];
                        snprintf(ft, sizeof(ft), "%s %s", p->nameEN, p->name);
                        json_object_set(item, "filterText", json_new_string(ft));
                    } else if (p->nameAscii && strcmp(p->nameAscii, p->name) != 0)
                        json_object_set(item, "filterText", json_new_string(p->nameAscii));
                    char detailBuf[160];
                    snprintf(detailBuf, sizeof(detailBuf), "%s — champ",
                             p->type ? p->type : "?");
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_array_push(items, item);
                }
            }
            /*  — MoiMême only : ajoute les props Fenetre puis État en
             * union du socle champ, dédup contre `seen` (ne pas reproposer une
             * prop deja emise, ex Visible/Grisé/Style presents sur Fenetre ET
             * sur le socle champ). */
            if (wide) {
                static const char* wideKinds[] = { "Fenetre", "Etat" };
                for (int w = 0; w < 2; w++) {
                    for (int i = 0; i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
                        const OwlDSLPropInfo* p = &OWL_DSL_PROP_CATALOG[i];
                        if (strcmp(p->kind, wideKinds[w]) != 0) continue;
                        bool already = false;
                        for (int s = 0; s < nSeen; s++)
                            if (strcmp(seen[s], p->name) == 0) { already = true; break; }
                        if (already) continue;
                        if (nSeen < 1024) seen[nSeen++] = p->name;
                        bool pHasEN = wantEN && p->nameEN
                                      && strcmp(p->nameEN, p->name) != 0;
                        const char* pLabel = pHasEN ? p->nameEN : p->name;
                        JsonValue* item = json_new_object();
                        json_object_set(item, "label", json_new_string(pLabel));
                        json_object_set(item, "kind", json_new_int(10)); /* Property */
                        json_object_set(item, "sortText", json_new_string("!"));
                        if (pHasEN) {
                            char ft[256];
                            snprintf(ft, sizeof(ft), "%s %s", p->nameEN, p->name);
                            json_object_set(item, "filterText", json_new_string(ft));
                        } else if (p->nameAscii && strcmp(p->nameAscii, p->name) != 0)
                            json_object_set(item, "filterText", json_new_string(p->nameAscii));
                        char detailBuf[160];
                        snprintf(detailBuf, sizeof(detailBuf), "%s — %s",
                                 p->type ? p->type : "?", wideKinds[w]);
                        json_object_set(item, "detail", json_new_string(detailBuf));
                        json_array_push(items, item);
                    }
                }
            }
            return items;
        }

        /*  — receiver = Groupe (GR_X..). Propose les props/methodes
         * pertinentes : broadcast Visible/Grisé/EnSaisie + props tableau
         * (Occurrence/Vide/Empty) + methodes tableau (Ajoute/Supprime/
         * SupprimeTout/Cherche). */
        if (strcmp(receiverType, "DSL_GROUP") == 0) {
            static const struct {
                const char* label;
                const char* detail;
                int kind;  /* 10=Property, 2=Method */
            } GROUP_ITEMS[] = {
                { "Occurrence",  "entier — nombre de membres du Groupe", 10 },
                { "Vide",        "booleen — Vrai si Groupe sans membre",  10 },
                { "Empty",       "booleen — alias EN de Vide",            10 },
                { "Visible",     "broadcast booleen aux champs membres",  10 },
                { "Gris\xc3\xa9", "broadcast booleen aux champs membres", 10 },
                { "Grise",       "broadcast booleen (alias sans accent)", 10 },
                { "EnSaisie",    "broadcast booleen (Saisies)",           10 },
                { "Valeur",      "broadcast chaine (Saisies)",            10 },
                { "Ajoute",      "Ajoute(grp, champ) — append membre",     2 },
                { "Supprime",    "Supprime(grp, indice) — retire membre",  2 },
                { "SupprimeTout","SupprimeTout(grp) — vide le Groupe",     2 },
                { "Cherche",     "Cherche(grp, champ) — indice ou 0",      2 },
            };
            int n = (int)(sizeof(GROUP_ITEMS)/sizeof(GROUP_ITEMS[0]));
            for (int i = 0; i < n; i++) {
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(GROUP_ITEMS[i].label));
                json_object_set(item, "kind", json_new_int(GROUP_ITEMS[i].kind));
                json_object_set(item, "detail", json_new_string(GROUP_ITEMS[i].detail));
                json_object_set(item, "sortText", json_new_string("!"));
                json_array_push(items, item);
            }
            return items;
        }

        /* DSL Fenetre v1 : si le receiver est un control/fenetre DSL,
         * proposer les props du kind ('FEN_X..' → DSL_F = Fenetre, etc.). */
        if (strncmp(receiverType, "DSL_", 4) == 0) {
            char dslKind = receiverType[4];
            const char* kindLabel = dsl_kind_string(dslKind);
            if (kindLabel) {
                bool any = false;
                for (int i = 0; i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
                    const OwlDSLPropInfo* h = &OWL_DSL_PROP_CATALOG[i];
                    if (strcmp(h->kind, kindLabel) != 0) continue;
                    any = true;
                    /*  — libellé dans la langue active, comme le bloc
                     * PROPRIETES qui expose les mêmes props. */
                    bool hHasEN = wantEN && h->nameEN
                                  && strcmp(h->nameEN, h->name) != 0;
                    const char* hLabel = hHasEN ? h->nameEN : h->name;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(hLabel));
                    json_object_set(item, "kind", json_new_int(10)); /* Property */
                    json_object_set(item, "sortText", json_new_string("!"));
                    if (hHasEN) {
                        char ft[256];
                        snprintf(ft, sizeof(ft), "%s %s", h->nameEN, h->name);
                        json_object_set(item, "filterText", json_new_string(ft));
                    } else if (h->nameAscii && strcmp(h->nameAscii, h->name) != 0)
                        json_object_set(item, "filterText", json_new_string(h->nameAscii));
                    char detailBuf[160];
                    snprintf(detailBuf, sizeof(detailBuf), "%s — %s",
                             h->type ? h->type : "?", kindLabel);
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_array_push(items, item);
                }
                if (any) return items;
            }
        }

        /* If the receiver's declared type matches a registered builtin
         * type (httpRequête, Chrono, Definition…), propose its fields.
         *  — sauf pour les types PRIMITIFS a catalogue de props
         * builtin (durée, Date, Heure…) : ceux-ci exposent leurs accesseurs
         * (`.EnJours`, `.EnHeures`, `.Jour`…) via OWL_BUILTIN_PROP_CATALOG
         * plus bas. La metadonnee registry de durée (fields de CONSTRUCTION
         * du litteral `{}`) ne doit pas court-circuiter ce catalogue en
         * `..` — sinon `var..` ne proposerait que les composantes de
         * construction et perdrait les totaux accesseurs. */
        if (receiverType[0] != '\0' && !is_primitive_prop_type(receiverType)) {
            const OwlTypeInfo* ti = owl_type_info_find(receiverType);
            if (ti && ti->fields && ti->fieldCount > 0) {
                for (int i = 0; i < ti->fieldCount; i++) {
                    const OwlFieldInfo* f = &ti->fields[i];
                    /*  — bascule EN : idiome deja en place sur le bloc
                     * Projet (nameEN/descriptionEN/typeEN peuples au catalogue,
                     * ex Police). Le nameAscii/nameEN sert de filterText pour que
                     * la frappe FR matche encore un label EN affiche. */
                    bool fHasEN = wantEN && f->nameEN
                                  && strcmp(f->nameEN, f->name) != 0;
                    const char* label = fHasEN ? f->nameEN : f->name;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(label));
                    json_object_set(item, "kind", json_new_int(10)); /* Property */
                    if (fHasEN) {
                        char ft[256];
                        snprintf(ft, sizeof(ft), "%s %s", f->nameEN, f->name);
                        json_object_set(item, "filterText", json_new_string(ft));
                    }
                    const char* fType = (wantEN && f->typeEN) ? f->typeEN
                                        : (f->type ? f->type : "?");
                    const char* fDesc = (wantEN && f->descriptionEN)
                                        ? f->descriptionEN
                                        : (f->description ? f->description : "");
                    char detailBuf[256];
                    snprintf(detailBuf, sizeof(detailBuf), "%s : %s", fType, fDesc);
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_array_push(items, item);
                }
                return items;
            }
        }

        /* Type composé (Cadre, Style, Police, Coin, Trait) : propose les
         * fields du catalogue OWL_COMPOSED_TYPE_CATALOG (auto-généré).
         * Couvre `var..Cadre..` (chain widget..composed) et `cadre1..` direct.
         *  — bascule EN : le catalogue porte maintenant nameEN (repris
         * de la déclaration OwlField du type, aucune forme inventée). Fallback
         * FR quand le champ n'a pas d'alias EN déclaré. */
        if (receiverType[0] != '\0') {
            bool anyComposed = false;
            for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                if (strcasecmp(f->typeName, receiverType) != 0) continue;
                anyComposed = true;
                const char* fLabel = composed_field_label(f, wantEN);
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(fLabel));
                json_object_set(item, "kind", json_new_int(10)); /* Property */
                if (fLabel != f->fieldName) {
                    char ft[256];
                    snprintf(ft, sizeof(ft), "%s %s", fLabel, f->fieldName);
                    json_object_set(item, "filterText", json_new_string(ft));
                }
                char detailBuf[256];
                if (f->subTypeName)
                    snprintf(detailBuf, sizeof(detailBuf), "%s : %s",
                             f->fieldType, f->subTypeName);
                else
                    snprintf(detailBuf, sizeof(detailBuf), "%s", f->fieldType);
                json_object_set(item, "detail", json_new_string(detailBuf));
                if (f->descFR)
                    json_object_set(item, "documentation", json_new_string(f->descFR));
                json_object_set(item, "sortText", json_new_string("!"));
                json_array_push(items, item);
            }
            if (anyComposed) return items;
        }

        /* Fallback : proprietes generiques des types primitifs (Date,
         * Heure, DateHeure, Duree, Tableau, JSON, collections). Catalog
         * auto-genere depuis compiler.c::PROPS_* via owl --dump-lsp-catalog
         * — zero duplication, drift impossible. Gate sur type primitif :
         * un type registry non-primitif sans fields (chaineMultilingue)
         * ne doit PAS heriter des props Date/Duree.
         *
         *  — et parmi les proprietes primitives, seules celles du
         * kind du receveur. Le catalogue est dedupe par nom (une ligne
         * `Jour` pour date/dateheure/duree) : c'est sa colonne `kinds` qui
         * porte l'appartenance, et non plus l'ordre de la table. Sans ce
         * filtre, un tableau d'entiers se voyait proposer `PartieDate` et
         * `EnJours` — de la completion qui ne compile pas. */
        char kindKey[64];
        if (!primitive_prop_kind_key(receiverType, kindKey, sizeof(kindKey)))
            return items;
        char kindNeedle[68];
        snprintf(kindNeedle, sizeof(kindNeedle), " %s ", kindKey);
        for (int i = 0; i < OWL_BUILTIN_PROP_CATALOG_COUNT; i++) {
            const OwlBuiltinPropInfo* p = &OWL_BUILTIN_PROP_CATALOG[i];
            if (!p->kinds || strstr(p->kinds, kindNeedle) == NULL) continue;
            /*  — bascule EN : nameEN/descEN peuples au catalogue
             * (Count/Empty/Year/InDays...). filterText = nameFR pour matcher la
             * frappe FR sur un label EN. Le prefixe "Property"/"Propriete" et le
             * valueType (chaine/entier — pas de colonne EN) suivent la langue. */
            bool pHasEN = wantEN && p->nameEN && strcmp(p->nameEN, p->nameFR) != 0;
            const char* label = pHasEN ? p->nameEN : p->nameFR;
            const char* desc  = (wantEN && p->descEN) ? p->descEN
                                : (p->descFR ? p->descFR : p->nameFR);
            const char* propKw = wantEN ? "Property" : "Propri\xc3\xa9t\xc3\xa9";
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(label));
            json_object_set(item, "kind", json_new_int(10)); /* Property */
            if (pHasEN)
                json_object_set(item, "filterText", json_new_string(p->nameFR));
            char detailBuf[256];
            if (p->valueType && p->valueType[0])
                snprintf(detailBuf, sizeof(detailBuf), "%s : %s (%s)",
                         propKw, desc, p->valueType);
            else
                snprintf(detailBuf, sizeof(detailBuf), "%s : %s", propKw, desc);
            json_object_set(item, "detail", json_new_string(detailBuf));
            json_array_push(items, item);
        }
        return items;
    }

    /*  — ':' dans body class : propose tous les champs et methodes
     * de la classe courante (visibility ignoree : on est DANS la classe). */
    if (colonMemberContext && colonClass) {
        for (int i = 0; i < colonClass->memberCount; i++) {
            const UserClassMember* m = &colonClass->members[i];
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(m->name));
            json_object_set(item, "kind",
                            json_new_int(m->isMethod ? 2 /* Method */ : 5 /* Field */));
            char detailBuf[200];
            if (m->isMethod) {
                snprintf(detailBuf, sizeof(detailBuf), "%s.%s() [%s]",
                         colonClass->name, m->name,
                         m->vis == 'U' ? "public" : m->vis == 'R' ? "prive" : "protege");
            } else {
                snprintf(detailBuf, sizeof(detailBuf), "%s : %s [%s]",
                         m->name, m->type[0] ? m->type : "?",
                         m->vis == 'U' ? "public" : m->vis == 'R' ? "prive" : "protege");
            }
            json_object_set(item, "detail", json_new_string(detailBuf));
            json_array_push(items, item);
        }
        return items;
    }

    if (singleDotContext) {
        /*  — point simple sur un Champ Table : le membre attendu est
         * un NOM DE COLONNE, dans les deux formes livrees —
         * `TABLE_X[1].COL_A` (acces valeur) et `TABLE_X.COL_A[1]..Couleur`
         * (designateur colonne-first). La completion y rendait 0 item.
         * Deux crochets designent une cellule : plus aucun nom de colonne
         * n'y a de sens, on n'en propose donc pas. */
        if (receiverRootName[0] != '\0' && receiverBracketGroups <= 1) {
            bool rootIsTable = false;
            for (int i = 0; i < userControlCount; i++) {
                if (userControls[i].kind == 'T'
                    && strcasecmp(receiverRootName, userControls[i].name) == 0) {
                    rootIsTable = true;
                    break;
                }
            }
            if (rootIsTable) {
                bool anyCol = false;
                for (int i = 0; i < userColumnCount; i++) {
                    if (strcasecmp(userColumns[i].table, receiverRootName) != 0)
                        continue;
                    anyCol = true;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label",
                                    json_new_string(userColumns[i].name));
                    json_object_set(item, "kind", json_new_int(5)); /* Field */
                    char detailBuf[288];
                    snprintf(detailBuf, sizeof(detailBuf),
                             wantEN ? "Column of %s" : "Colonne de %s",
                             receiverRootName);
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_object_set(item, "sortText", json_new_string("!"));
                    json_array_push(items, item);
                }
                if (anyCol) return items;
            }
        }

        /*  — Si le receiver est une classe user-declared, proposer
         * uniquement les membres PUBLIC (encapsulation depuis l'exterieur). */
        if (receiverType[0]) {
            UserClassInfo* ci = user_class_lookup(receiverType);
            if (ci && ci->memberCount > 0) {
                for (int i = 0; i < ci->memberCount; i++) {
                    const UserClassMember* m = &ci->members[i];
                    if (m->vis != 'U') continue; /* filtre PRIVE/PROTEGE */
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(m->name));
                    json_object_set(item, "kind",
                                    json_new_int(m->isMethod ? 2 : 5));
                    char detailBuf[200];
                    if (m->isMethod)
                        snprintf(detailBuf, sizeof(detailBuf), "%s.%s()",
                                 ci->name, m->name);
                    else
                        snprintf(detailBuf, sizeof(detailBuf), "%s : %s",
                                 m->name, m->type[0] ? m->type : "?");
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_array_push(items, item);
                }
                return items;
            }
        }
        /* If the receiver is a user-declared structure, propose its
         * fields. Convention: single-dot = struct member. */
        if (receiverType[0]) {
            UserStructInfo* si = user_struct_lookup(receiverType);
            if (si && si->fieldCount > 0) {
                for (int i = 0; i < si->fieldCount; i++) {
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(si->fields[i].name));
                    json_object_set(item, "kind", json_new_int(5)); /* Field */
                    char detailBuf[160];
                    snprintf(detailBuf, sizeof(detailBuf), "%s : %s",
                             si->fields[i].name,
                             si->fields[i].type[0] ? si->fields[i].type : "?");
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    json_array_push(items, item);
                }
                return items;
            }
        }
        /* After ".", propose extension methods from OWL_METHOD_CATALOG (auto-generated). */
        if (receiverType[0] == '\0') return items;

        for (int i = 0; i < OWL_METHOD_CATALOG_COUNT; i++) {
            const OwlMethodInfo* m = &OWL_METHOD_CATALOG[i];
            if (!m->name || !m->receiverType) continue;
            if (strcasecmp(m->receiverType, receiverType) != 0) continue;
            /* Skip EN aliases / duplicate impls to declutter the suggestions.
             * The aliases remain resolvable at call time (runtime lookup). */
            if (m->isAlias) continue;
            /*  — résout la native sous-jacente pour exposer la vraie
             * signature MOINS le receveur (param 0). Sans ça la méthode
             * fluent perdait tous ses arguments (`Cherche()` au lieu de
             * `Cherche(valeur)`). */
            const NativeInfo* mnat = m->nativeName
                                     ? catalog_find_native(m->nativeName) : NULL;
            /*  — bascule EN. Les formes EN des méthodes fluent sont
             * enregistrées comme alias (filtrés juste au-dessus pour ne pas
             * doubler la liste), donc jamais proposées en langue EN. On rend
             * la forme EN de la native sous-jacente — même appariement que le
             * formatter en mode translate — et seulement si elle est bien
             * enregistrée comme méthode sur ce receveur (donc appelable). */
            const char* mLabel = m->name;
            if (wantEN && mnat && mnat->nameEN && *mnat->nameEN
                && strcasecmp(mnat->nameEN, m->name) != 0) {
                for (int a = 0; a < OWL_METHOD_CATALOG_COUNT; a++) {
                    const OwlMethodInfo* al = &OWL_METHOD_CATALOG[a];
                    if (!al->isAlias || !al->name || !al->receiverType) continue;
                    if (strcasecmp(al->receiverType, receiverType) != 0) continue;
                    if (strcasecmp(al->name, mnat->nameEN) != 0) continue;
                    mLabel = al->name;
                    break;
                }
            }
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(mLabel));
            json_object_set(item, "kind", json_new_int(2)); /* Method */
            if (mLabel != m->name) {
                char ft[256];
                snprintf(ft, sizeof(ft), "%s %s", mLabel, m->name);
                json_object_set(item, "filterText", json_new_string(ft));
            }
            char detailBuf[256];
            char insertBuf[256];
            if (mnat) {
                char plain[200];
                build_native_plain_params(plain, sizeof(plain), mnat, 1);
                snprintf(detailBuf, sizeof(detailBuf), ".%s(%s)", mLabel, plain);
                build_native_snippet_skip(insertBuf, sizeof(insertBuf),
                                          mnat, mLabel, 1, wantEN);
            } else {
                snprintf(detailBuf, sizeof(detailBuf), ".%s()", mLabel);
                snprintf(insertBuf, sizeof(insertBuf), "%s($0)", mLabel);
            }
            json_object_set(item, "detail", json_new_string(detailBuf));
            json_object_set(item, "insertText", json_new_string(insertBuf));
            json_object_set(item, "insertTextFormat", json_new_int(2));
            json_array_push(items, item);
        }
        return items;
    }

    /* Property-value context (`var..Field = <cursor>`): narrow the
     * suggestions to things that can actually be assigned to the field —
     * boolean/null literals, constants (prioritised by the field type's
     * category), plus user-defined variables. No functions, no keywords. */
    if (propValueContext) {
        /* Bug #4 : `..Cadre..Bords = <cursor>` — si receiver est un type
         * composé, chercher le field dans OWL_COMPOSED_TYPE_CATALOG et
         * proposer ses valeurs d'enum. */
        if (propReceiverType[0] && propFieldName[0]) {
            for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                if (strcasecmp(f->typeName, propReceiverType) != 0) continue;
                if (strcasecmp(f->fieldName, propFieldName) != 0) continue;
                if (strcmp(f->fieldType, "enum") == 0 && f->subTypeName) {
                    const OwlEnumInfo* ei = owl_enum_info_find(f->subTypeName);
                    if (ei && ei->values) {
                        JsonValue* enumItems = json_new_array();
                        for (const char* const* v = ei->values; *v; v++) {
                            emit_enum_value_item(enumItems, *v, ei->name, 13,
                                                 '0', "!", "Valeur de",
                                                 "Value of", wantEN);
                        }
                        json_free(items);
                        return enumItems;
                    }
                }
                break;
            }
        }
        const OwlTypeInfo* ti = propReceiverType[0]
            ? owl_type_info_find(propReceiverType) : NULL;
        const char* preferredCategory = ti ? ti->category : NULL;

        /* Look up the field's declared type on the receiver, so we can
         * suggest only boolean literals for a booléen field, or only
         * the members of allowedBase when set. The field name may have
         * been typed with accents (Méthode) so we strip to ASCII before
         * matching against the registered name. */
        const char* fieldType          = NULL;
        const OwlEnumInfo* fieldBase   = NULL;
        const OwlEnumInfo* fieldOpts   = NULL;
        if (ti && ti->fields && propFieldName[0]) {
            char stripped[128];
            scanner_strip_accents(propFieldName, (int)strlen(propFieldName),
                                  stripped, sizeof(stripped));
            for (int i = 0; i < ti->fieldCount; i++) {
                /* The catalog stores the display (accented) form, so strip
                 * accents on that side too before the case-insensitive
                 * compare. Otherwise "Methode" never matches "Méthode". */
                char fieldStripped[128];
                scanner_strip_accents(ti->fields[i].name,
                                      (int)strlen(ti->fields[i].name),
                                      fieldStripped, sizeof(fieldStripped));
                if (strcasecmp(stripped, fieldStripped) == 0) {
                    fieldType = ti->fields[i].type;
                    fieldBase = owl_enum_info_find(ti->fields[i].allowedBaseEnum);
                    fieldOpts = owl_enum_info_find(ti->fields[i].allowedOptionsEnum);
                    break;
                }
            }
        }

        /*  C2 — le RHS attend une icone du catalogue : soit le champ
         * source par defaut du type Icône (`icn..Icone`, type chaine qui
         * accepte une entree catalogue), soit un champ dont le type declare
         * EST une Icône. On propose le namespace `icones.` en ancre plutot
         * que de noyer l'utilisateur dans tout le catalogue de constantes. */
        {
            bool fieldIsIconeCatalog = false;
            if (fieldType) {
                char ftStripped[64];
                scanner_strip_accents(fieldType, (int)strlen(fieldType),
                                      ftStripped, sizeof(ftStripped));
                if (strcasecmp(ftStripped, "Icone") == 0)
                    fieldIsIconeCatalog = true;
            }
            /* Champ source par defaut `Icone` du type Icône. */
            if (!fieldIsIconeCatalog && ti && propFieldName[0]) {
                char tnStripped[64], fnStripped[128];
                scanner_strip_accents(ti->name, (int)strlen(ti->name),
                                      tnStripped, sizeof(tnStripped));
                scanner_strip_accents(propFieldName, (int)strlen(propFieldName),
                                      fnStripped, sizeof(fnStripped));
                if (strcasecmp(tnStripped, "Icone") == 0 &&
                    strcasecmp(fnStripped, "Icone") == 0)
                    fieldIsIconeCatalog = true;
            }
            if (fieldIsIconeCatalog) {
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string("icones"));
                json_object_set(item, "kind", json_new_int(9)); /* Module */
                json_object_set(item, "detail",
                    json_new_string("Espace de noms du catalogue d'ic\xc3\xb4nes embarqu\xc3\xa9"));
                /* insertText `icones.` declenche la completion catalog. */
                json_object_set(item, "insertText", json_new_string("icones."));
                json_object_set(item, "sortText", json_new_string("0_icones"));
                /* command : re-trigger la completion apres insertion du `.` */
                JsonValue* cmd = json_new_object();
                json_object_set(cmd, "title", json_new_string("Suggest"));
                json_object_set(cmd, "command",
                                json_new_string("editor.action.triggerSuggest"));
                json_object_set(item, "command", cmd);
                json_array_push(items, item);
                return items; /* strict : juste l'ancre namespace */
            }
        }

        /* Strict filtering: if the field declares an allowedBase enum,
         * propose only its members (and optionally the combinable options). */
        if (fieldBase) {
            for (const char* const* v = fieldBase->values; *v; v++) {
                emit_enum_value_item(items, *v, fieldBase->name, 21, '0', NULL,
                                     "Membre de", "Member of", wantEN);
            }
            if (fieldOpts && fieldOpts->combinable) {
                for (const char* const* v = fieldOpts->values; *v; v++) {
                    emit_enum_value_item(items, *v, fieldOpts->name, 21, '1',
                                         NULL, "Option combinable de",
                                         "Combinable option of", wantEN);
                }
            }
            return items;  /* strict — no other suggestions */
        }

        bool wantBool = fieldType && (strstr(fieldType, "bool") != NULL);

        /* Boolean / null literals. Vrai+Faux only for booleen fields;
         * otherwise all three are harmless fallbacks. */
        static const struct { const char* name; bool boolLike; } lits[] = {
            {"Vrai", true}, {"Faux", true}, {"Null", false},
        };
        for (size_t i = 0; i < sizeof(lits)/sizeof(lits[0]); i++) {
            if (wantBool && !lits[i].boolLike) continue;
            const char* lbl = bool_literal_label(lits[i].name, wantEN);  /*  */
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(lbl));
            json_object_set(item, "kind", json_new_int(21));
            char sortBuf[32];
            snprintf(sortBuf, sizeof(sortBuf), "0_%s", lbl);
            json_object_set(item, "sortText", json_new_string(sortBuf));
            json_array_push(items, item);
        }

        /* Constants — full list, but same-category ones float on top. */
        if (!wantBool) {
            for (int i = 0; i < OWL_CONST_CATALOG_COUNT; i++) {
                const OwlConstInfo* c = &OWL_CONST_CATALOG[i];
                bool sameCat = preferredCategory && c->category &&
                               strcmp(preferredCategory, c->category) == 0;
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(c->name));
                json_object_set(item, "kind", json_new_int(21));
                char detailBuf[256];
                snprintf(detailBuf, sizeof(detailBuf), "[%s] %s",
                         c->category ? c->category : "Divers",
                         c->description ? c->description : "");
                json_object_set(item, "detail", json_new_string(detailBuf));
                char sortBuf[300];
                snprintf(sortBuf, sizeof(sortBuf), "%c_%s",
                         sameCat ? '1' : '2', c->name);
                json_object_set(item, "sortText", json_new_string(sortBuf));
                json_array_push(items, item);
            }
        }

        /* User-defined variables (anything the user might reference as RHS). */
        for (int i = 0; i < userSymbolCount; i++) {
            if (userSymbols[i].kind != 6) continue;
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(userSymbols[i].name));
            json_object_set(item, "kind", json_new_int(6));
            json_object_set(item, "detail", json_new_string(userSymbols[i].detail));
            char sortBuf[160];
            snprintf(sortBuf, sizeof(sortBuf), "3_%.120s", userSymbols[i].name);
            json_object_set(item, "sortText", json_new_string(sortBuf));
            json_array_push(items, item);
        }
        return items;
    }

    /* Simple assignment context (`var = <cursor>`): if var's type is un
     * enum/combinaison user OU registry, propose ses valeurs. */
    if (simpleAssignContext && simpleAssignVarType[0]) {
        UserEnumInfo* uei = user_enum_lookup(simpleAssignVarType);
        if (uei) {
            /* Enum user : pas de nameEN au const-catalog, le helper
             * retombe proprement sur la forme FR pour le label ; seul le
             * detail bascule en EN. */
            for (int i = 0; i < uei->valueCount; i++) {
                emit_enum_value_item(items, uei->values[i], uei->name, 13,
                                     '0', NULL, "Valeur de", "Value of",
                                     wantEN);
            }
            return items;
        }
        const OwlEnumInfo* rei = owl_enum_info_find(simpleAssignVarType);
        if (rei && rei->values) {
            /*  (support #313) — FormatAffichage sur un Champ Saisie :
             * la valeur RTF (formatTexteRiche / EN formatRichText) n'a d'effet
             * qu'en saisie multi-ligne. Hors typSaisieMultiligne (mot de passe,
             * texte simple, defaut), on la masque — miroir exact du filtre
             * designer (ui_presentation.c visible_if TypeSaisie=typSaisieMultiligne).
             * Le nom de prop FormatAffichage coincide avec l'enum formatAffichage
             * (casse-insensible), d'ou la resolution via simpleAssignContext. Le
             * filtre est strict au genre Saisie : un Libelle au format RTF reste
             * valide en permanence. */
            bool hideRTF = false;
            if (strcmp(rei->name, "formatAffichage") == 0 && td && pos) {
                /* Re-derive doc + offset octet depuis params (le `src`/offset
                 * du walk-back n'est plus en scope ici), comme le bloc
                 * PROCEDURE plus bas ( pour l'offset UTF-16->octet). */
                const char* uri = json_get_string(td, "uri");
                int line = (int)json_get_int(pos, "line", 0);
                int character = (int)json_get_int(pos, "character", 0);
                int idx = find_doc(uri);
                if (idx >= 0) {
                    const char* src = lspServer.documents[idx].content;
                    const char* lp = src;
                    int cl = 0;
                    while (*lp && cl < line) { if (*lp == '\n') cl++; lp++; }
                    character = lsp_utf16_to_byte_in_line(lp, character);
                    int co = (int)(lp - src) + character;
                    DslCtx d = detect_dsl_context_ex(src, co);
                    if (d.widget == 'S' && !d.saisieIsMultiligne) hideRTF = true;
                }
            }
            for (const char* const* v = rei->values; *v; v++) {
                if (hideRTF && strcmp(*v, "formatTexteRiche") == 0) continue;
                emit_enum_value_item(items, *v, rei->name, 13, '0', NULL,
                                     "Valeur de", "Value of", wantEN);
            }
            return items;
        }
    }

    /* POUR TOUTE POSITION ... [AVEC <mask>] context : propose les
     * constantes de optionsRechercheChaine (et le mot-cle AVEC en mode
     * suffix, si pas encore tape). */
    if (pourTouteCtx != POS_CTX_NONE) {
        const OwlEnumInfo* en =
            owl_enum_info_find("optionsRechercheCha\xc3\xae" "ne");
        if (en && en->values) {
            if (pourTouteCtx == POS_CTX_SUFFIX) {
                /* Mode suffix : propose AVEC en premier (Keyword=14),
                 * puis les 4 valeurs combinables (peuvent etre tapees
                 * directement comme suffixe positionnel pour DepuisFin
                 * / DepuisDebut). */
                JsonValue* avec = json_new_object();
                json_object_set(avec, "label", json_new_string("AVEC"));
                json_object_set(avec, "kind", json_new_int(14));
                json_object_set(avec, "detail",
                    json_new_string("Introduit le mask d'options combinables"));
                json_object_set(avec, "sortText", json_new_string("0_AVEC"));
                json_array_push(items, avec);
            }
            for (const char* const* v = en->values; *v; v++) {
                emit_enum_value_item(items, *v, en->name, 21, '1', NULL,
                                     "Option combinable de",
                                     "Combinable option of", wantEN);
            }
            return items;
        }
    }

    /* Call-argument context (`fn(arg, <cursor>)`): if the function's
     * param at this position has an allowedBase enum, emit ONLY its
     * members (plus optional combinable options). Otherwise fall
     * through to the general list. */
    if (callArgContext && callFnName[0]) {
        const NativeInfo* fn = catalog_find_native(callFnName);
        /* Parcours de TOUTES les signatures : une native surchargee peut
         * porter la contrainte enum sur une signature alternative. Ex.
         * fChargeTexte : signatures[0] = (chemin) 1 param,
         * signatures[1] = (chemin, encodage:owlEncodage) 2 params. Au 2e
         * arg (callArgIndex=1), seule signatures[1] a le param enum ; ne
         * regarder que signatures[0] laissait tomber dans le catalogue
         * general. On retient la PREMIERE signature (ordre de declaration)
         * ou le param a l'index callArgIndex existe ET porte un enum
         * base/options. En cas de sigs divergentes au meme index (aucun
         * cas reel connu), la premiere trouvee gagne — deterministe. */
        const OwlNativeParamInfo* argParam = NULL;
        if (fn && callArgIndex >= 0) {
            for (int s = 0; s < fn->signatureCount && !argParam; s++) {
                const OwlNativeSigInfo* sig = &fn->signatures[s];
                if (callArgIndex >= sig->paramCount) continue;
                const OwlNativeParamInfo* p = &sig->params[callArgIndex];
                if (p->allowedBaseEnum || p->allowedOptionsEnum) argParam = p;
            }
        }
        if (argParam) {
            const char* baseName = argParam->allowedBaseEnum;
            const char* optsName = argParam->allowedOptionsEnum;
            int acceptsLits      = argParam->acceptsLiterals;
            const OwlEnumInfo* paramBase = owl_enum_info_find(baseName);
            const OwlEnumInfo* paramOpts = owl_enum_info_find(optsName);
            if (paramBase || paramOpts) {
                if (paramBase) {
                    for (const char* const* v = paramBase->values; *v; v++) {
                        emit_enum_value_item(items, *v, paramBase->name, 21,
                                             '0', NULL, "Membre de",
                                             "Member of", wantEN);
                    }
                }
                if (paramOpts && paramOpts->combinable) {
                    for (const char* const* v = paramOpts->values; *v; v++) {
                        emit_enum_value_item(items, *v, paramOpts->name, 21,
                                             '1', NULL, "Option combinable de",
                                             "Combinable option of", wantEN);
                    }
                }
                /* Si acceptsLiterals=true, on ne court-circuite PAS : le
                 * catalogue general (variables, natives, literals) reste
                 * propose en plus, l'enum apparaissant juste en tete via
                 * sortText "0_" / "1_". Cas type : indice numerique OU
                 * cible relative (tdSuivant, tdPremier...). */
                if (!acceptsLits) {
                    return items;
                }
            }
        }
    }

    /*  — au 1er arg d'une native ouvre-fenetre (Ouvre/OuvreFille/
     * OuvreSoeur, canon resolu via catalog_find_native pour couvrir les alias
     * EN Open/OpenChild/OpenSibling), n'exposer QUE les vraies Fenetres —
     * comme un contexte d'arg enum n'expose que ses membres. Early-return
     * calque sur le bloc enum ci-dessus : coupe le catalogue natif (~1500
     * items) qui noyait la seule fenetre. Un Etat s'ouvre via ImprimeEtat,
     * pas Ouvre : filtre isWindow. Item identique au bloc DSL Fenetre v1. */
    if (callArgContext && callArgIndex == 0 && callFnName[0]) {
        const NativeInfo* ofn = catalog_find_native(callFnName);
        if (ofn && ofn->name
            && (strcmp(ofn->name, "Ouvre") == 0
                || strcmp(ofn->name, "OuvreFille") == 0
                || strcmp(ofn->name, "OuvreSoeur") == 0)) {
            for (int i = 0; i < userWindowCount; i++) {
                if (!userWindows[i].isWindow) continue;
                JsonValue* item = json_new_object();
                json_object_set(item, "label", json_new_string(userWindows[i].name));
                json_object_set(item, "kind", json_new_int(8)); /* Class */
                json_object_set(item, "detail", json_new_string("Fenetre OWL"));
                json_object_set(item, "sortText", json_new_string("\""));
                json_array_push(items, item);
            }
            return items;
        }
    }

    /*  — auto-popup declenche par '(' hors contexte focalise : pas de
     * popup generale. Les early-returns focalises ci-dessus (fenetres arg0,
     * membres enum) ont deja renvoye leur liste ; ici on est sur un '(' d'une
     * native quelconque -> liste vide (le signatureHelp s'affiche seul via
     * son propre provider). Ctrl-Space (Invoked) ne passe pas par ce garde et
     * conserve la liste generale complete. */
    if (parenTrigger || commaTrigger) {
        return items; /* vide */
    }

    /* Native functions from catalog (kind=3 = Function).
     * `documentation` is emitted as a MarkupContent (markdown) so rich
     * clients (VS Code, Jupyter kernel, Zed, Neovim with a decent LSP
     * frontend) render signature + return type + description as a nice
     * tooltip. Clients without markdown support receive the raw text. */
    /* Filtre langue : par defaut "fr" (cf cahier des charges §4 LSP unilingue
     * paramétrable). Le client peut switcher en envoyant
     * initializationOptions.language = "en". Le serveur filtre les
     * propositions de completion mais comprend toujours les deux.
     * `wantEN` est calcule en tete de handle_completion (cf. ). */
    #define FN_MD_MAX 1024
    for (int i = 0; i < NATIVE_CATALOG_COUNT; i++) {
        const NativeInfo* fn = &NATIVE_CATALOG[i];

        /*  — position valeur d'un decl-assign : ne proposer que les
         * natives dont le type de retour est assignable au type LHS. */
        if (inDeclAssignValue && !native_return_assignable_to(fn, declAssignType))
            continue;

        bool hasEN  = (fn->nameEN && *fn->nameEN && strcmp(fn->name, fn->nameEN) != 0);
        bool pushFR = !wantEN || !hasEN;     /* mode FR : toujours. mode EN : seulement si pas d'EN */
        bool pushEN = hasEN && (wantEN || false);  /* mode EN : oui. mode FR : jamais */

        char md[FN_MD_MAX];
        int off = 0;
        off += snprintf(md+off, FN_MD_MAX-off, "```owl\n%s\n```\n\n", fn->signatures[0].signature);
        if (fn->signatures[0].returnType && *fn->signatures[0].returnType)
            off += snprintf(md+off, FN_MD_MAX-off, "**\xe2\x86\x92** `%s`\n\n",
                            fn->signatures[0].returnType);
        if (fn->description && *fn->description)
            off += snprintf(md+off, FN_MD_MAX-off, "%s\n", fn->description);
        if (fn->category && *fn->category)
            off += snprintf(md+off, FN_MD_MAX-off, "\n*Cat\xc3\xa9gorie :* %s", fn->category);
        if (hasEN)
            off += snprintf(md+off, FN_MD_MAX-off, " \xc2\xb7 *EN :* `%s`", fn->nameEN);
        (void)off;

        if (pushFR) {
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(fn->name));
            /* filterText combines accented + ASCII so both "ChaîneCompa" and
             * "ChaineCompa" match the same entry in VSCode's fuzzy filter. */
            if (fn->nameAscii && *fn->nameAscii &&
                    strcmp(fn->nameAscii, fn->name) != 0) {
                char ft[512];
                snprintf(ft, sizeof(ft), "%s %s", fn->name, fn->nameAscii);
                json_object_set(item, "filterText", json_new_string(ft));
            }
            json_object_set(item, "kind", json_new_int(3));
            json_object_set(item, "detail", json_new_string(fn->signatures[0].signature));
            JsonValue* doc = json_new_object();
            json_object_set(doc, "kind",  json_new_string("markdown"));
            json_object_set(doc, "value", json_new_string(md));
            json_object_set(item, "documentation", doc);
            char insert[512];
            build_native_completion_snippet(insert, sizeof(insert), fn, fn->name, wantEN);
            json_object_set(item, "insertText", json_new_string(insert));
            json_object_set(item, "insertTextFormat", json_new_int(2));
            json_array_push(items, item);
        }

        if (pushEN) {
            char mdEN[FN_MD_MAX];
            int oe = 0;
            oe += snprintf(mdEN+oe, FN_MD_MAX-oe, "```owl\n%s\n```\n\n", fn->signatures[0].signature);
            if (fn->signatures[0].returnType && *fn->signatures[0].returnType)
                oe += snprintf(mdEN+oe, FN_MD_MAX-oe, "**\xe2\x86\x92** `%s`\n\n",
                               fn->signatures[0].returnType);
            if (fn->descriptionEN && *fn->descriptionEN)
                oe += snprintf(mdEN+oe, FN_MD_MAX-oe, "%s\n", fn->descriptionEN);
            oe += snprintf(mdEN+oe, FN_MD_MAX-oe, "\n*FR alias :* `%s`", fn->name);
            (void)oe;

            JsonValue* itemEN = json_new_object();
            json_object_set(itemEN, "label", json_new_string(fn->nameEN));
            json_object_set(itemEN, "kind", json_new_int(3));
            json_object_set(itemEN, "detail", json_new_string(fn->signatures[0].signature));
            JsonValue* docEN = json_new_object();
            json_object_set(docEN, "kind",  json_new_string("markdown"));
            json_object_set(docEN, "value", json_new_string(mdEN));
            json_object_set(itemEN, "documentation", docEN);
            char insertEN[512];
            build_native_completion_snippet(insertEN, sizeof(insertEN), fn, fn->nameEN, wantEN);
            json_object_set(itemEN, "insertText", json_new_string(insertEN));
            json_object_set(itemEN, "insertTextFormat", json_new_int(2));
            json_array_push(items, itemEN);
        }
    }
    #undef FN_MD_MAX

    /* Keywords (kind=14).  — jamais des valeurs assignables : retires
     * en position valeur d'un decl-assign (`<var> est un <type> = <curseur>`).
     * Ailleurs (debut d'instruction, etc.) inchanges. */
    static const char* kw[] = {
        "SI", "ALORS", "SINON", "FIN", "POUR", "TOUT", "TANTQUE",
        "BOUCLE", "FAIRE", "SELON", "CAS", "AUTRE", "SORTIR",
        "CONTINUER", "RENVOYER", "RETOUR", "AVEC", "QUAND", "EXCEPTION",
        "PROCEDURE", "FONCTION", "CLASSE", "STRUCTURE", "ENUMERATION",
        "COMBINAISON", "INTERFACE", "HERITE", "IMPLEMENTE",
        "PUBLIC", "PRIVE", "PROTEGE", "GLOBAL", "LOCAL", "CONSTANTE",
        "PROPRIETES",  /* DSL v2 : bloc statique des props widget */
        /* Mots-cles contextuels emetteur/fenetre courante : label FR canon
         * mixte verbatim (le label EN suit via keyword_en_form, la doc hover
         * via owl_keyword_find_by_word). Pas de snippet block-opener. */
        "MoiM\xc3\xaame", "MonChamp", "MaFen\xc3\xaatre",
        NULL
    };
    /* Block-opener snippets : tape le mot-cle + Enter -> bloc structure
     * complet avec FIN auto + curseur au bon endroit ($0).
     *
     *  — corps locale-aware : colonne `fr` (canon FR) + colonne `en`
     * (canon EN). En locale EN le corps insere utilise les formes EN canon
     * des mots-cles embarques (IF/THEN/END, FOR/TO/END, WHILE, SWITCH/CASE,
     * CONSTANT, COMBINATION, CLASS, FUNCTION, LOOP, WITH). Formes verifiees
     * sur la table scanner (cf. scanner.c keywords[]). STRUCTURE / ENUMERATION
     * / INTERFACE / PROCEDURE n'ont pas d'alias EN distinct -> meme corps. */
    static const struct {
        const char* kw;  /* cle = forme FR canon (label brut de kw[]) */
        const char* fr;
        const char* en;
    } KW_SNIPPETS[] = {
        {"CONSTANTE",    "CONSTANTE\n\t$0\nFIN",                    "CONSTANT\n\t$0\nEND"},
        {"PROPRIETES",   "PROPRIETES\n\t$0\nFIN",                   "PROPERTIES\n\t$0\nEND"},
        {"STRUCTURE",    "STRUCTURE\n\t$0\nFIN",                    "STRUCTURE\n\t$0\nEND"},
        {"ENUMERATION",  "ENUMERATION\n\t$0\nFIN",                  "ENUMERATION\n\t$0\nEND"},
        {"COMBINAISON",  "COMBINAISON\n\t$0\nFIN",                  "COMBINATION\n\t$0\nEND"},
        {"CLASSE",       "CLASSE\n\t$0\nFIN",                       "CLASS\n\t$0\nEND"},
        {"INTERFACE",    "INTERFACE\n\t$0\nFIN",                    "INTERFACE\n\t$0\nEND"},
        {"PROCEDURE",    "PROCEDURE ${1:nom}()\n\t$0\nFIN",         "PROCEDURE ${1:name}()\n\t$0\nEND"},
        {"FONCTION",     "FONCTION ${1:nom}()\n\t$0\nFIN",          "FUNCTION ${1:name}()\n\t$0\nEND"},
        {"BOUCLE",       "BOUCLE\n\t$0\nFIN",                       "LOOP\n\t$0\nEND"},
        {"TANTQUE",      "TANTQUE ${1:condition}\n\t$0\nFIN",       "WHILE ${1:condition}\n\t$0\nEND"},
        {"POUR",         "POUR ${1:i} = ${2:1} A ${3:N}\n\t$0\nFIN","FOR ${1:i} = ${2:1} TO ${3:N}\n\t$0\nEND"},
        {"SI",           "SI ${1:condition} ALORS\n\t$0\nFIN",      "IF ${1:condition} THEN\n\t$0\nEND"},
        {"SELON",        "SELON ${1:valeur}\n\tCAS ${2:val1}: $0\nFIN", "SWITCH ${1:value}\n\tCASE ${2:val1}: $0\nEND"},
        {"AVEC",         "AVEC ${1:objet}\n\t$0\nFIN",              "WITH ${1:object}\n\t$0\nEND"},
        /*  — QUAND ouvre le handler d'exception `QUAND EXCEPTION DANS
         * ... FAIRE ... FIN` (cf. 08_exceptions.md). EXCEPTION seul (re-levee)
         * n'ouvre pas de bloc → pas de snippet. */
        {"QUAND",        "QUAND EXCEPTION DANS\n\t$0\nFAIRE\n\t\nFIN",
                         "WHEN EXCEPTION IN\n\t$0\nDO\n\t\nEND"},
    };
    for (int i = 0; !inDeclAssignValue && kw[i]; i++) {
        /*  — en mode EN, le label devient la forme EN du mot-cle
         * (IF pour SI, WHILE pour TANTQUE…) quand elle existe, fallback FR
         * sinon. La doc suit la langue : syntax + descEN/descFR via le
         * catalogue keyword (label_en/descEN par sens). */
        char enForm[64];
        bool useEN = wantEN && keyword_en_form(kw[i], enForm, sizeof(enForm));
        const char* label = useEN ? enForm : kw[i];

        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(label));
        json_object_set(item, "kind", json_new_int(14));

        const OwlKeywordDef* kdef = owl_keyword_find_by_word(kw[i]);
        if (kdef && kdef->senses_count > 0) {
            const OwlKeywordSense* sn = &kdef->senses[0];
            const char* sdesc = (wantEN && sn->descEN && *sn->descEN)
                                ? sn->descEN : sn->descFR;
            if (sn->syntax) json_object_set(item, "detail", json_new_string(sn->syntax));
            if (sdesc)      json_object_set(item, "documentation", json_new_string(sdesc));
        } else {
            /* Fallback : keyword sans fiche catalogue (ex: PROPRIETES) ->
             * detail/doc FR du catalogue LSP historique. */
            const KeywordInfo* info = catalog_find_keyword(kw[i]);
            if (info) {
                json_object_set(item, "detail", json_new_string(info->syntax));
                json_object_set(item, "documentation", json_new_string(info->description));
            }
        }
        /* filterText : combine EN+FR pour que le fuzzy matche peu importe
         * la frappe (un user EN tape 'if', un user FR 'si'). */
        if (useEN) {
            char ft[160];
            snprintf(ft, sizeof(ft), "%s %s", enForm, kw[i]);
            json_object_set(item, "filterText", json_new_string(ft));
        }
        /* Snippet : si le keyword ouvre un bloc, attache un insertText
         * qui genere bloc + FIN auto. Keye sur la forme FR canon. */
        for (size_t s = 0; s < sizeof(KW_SNIPPETS)/sizeof(KW_SNIPPETS[0]); s++) {
            if (strcmp(kw[i], KW_SNIPPETS[s].kw) == 0) {
                const char* body = useEN ? KW_SNIPPETS[s].en : KW_SNIPPETS[s].fr;
                json_object_set(item, "insertText", json_new_string(body));
                json_object_set(item, "insertTextFormat", json_new_int(2)); /* snippet */
                break;
            }
        }
        json_array_push(items, item);
    }

    /* Types (kind=25) — meme source de verite OWL_PRIM_TYPES que le contexte
     * type-declaration, filtree par langue : mode EN -> forme EN
     * canon si distincte sinon fallback FR ; mode FR -> forme FR canon. Le
     * label est desormais la forme canon accentuee (`chaîne`), avec filterText
     * ASCII pour rester matchable sans accent (`chaine`).  — un nom de
     * type n'est jamais une valeur : retire en position valeur decl-assign. */
    for (int i = 0; !inDeclAssignValue && OWL_PRIM_TYPES[i].name; i++) {
        const OwlPrimType* p = &OWL_PRIM_TYPES[i];
        const char* label = owl_prim_label(p, wantEN);
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(label));
        json_object_set(item, "kind", json_new_int(25));
        size_t labelLen = strlen(label);
        char ftBuf[64];
        int ftLen = scanner_strip_accents(label, (int)labelLen,
                                          ftBuf, sizeof(ftBuf));
        if (ftLen > 0 && (size_t)ftLen != labelLen) {
            json_object_set(item, "filterText", json_new_string(ftBuf));
        }
        json_array_push(items, item);
    }

    /* Constants (kind=21) — boolean + nil literals, then all runtime
     * constants from the catalog (httpGet, RC, CRLF, ValPI, psd*…). */
    static const char* coreConsts[] = { "Vrai", "Faux", "Null", NULL };
    for (int i = 0; coreConsts[i]; i++) {
        /*  — decl-assign booleen : Null n'est pas un litteral
         * booleen, on le retire (seuls Vrai/Faux/True/False attendus). */
        if (boolLiteralOnly && strcmp(coreConsts[i], "Null") == 0) continue;
        JsonValue* item = json_new_object();
        json_object_set(item, "label",
                        json_new_string(bool_literal_label(coreConsts[i], wantEN)));  /*  */
        json_object_set(item, "kind", json_new_int(21));
        /*  — en position valeur d'un decl-assign booleen, les
         * litteraux du type (Vrai/Faux) remontent EN TETE, avant natives et
         * variables. sortText "!" (0x21) precede tout label (lettres) et tout
         * item sans sortText (VSCode retombe alors sur le label). */
        if (boolLiteralOnly)
            json_object_set(item, "sortText", json_new_string("!"));
        json_array_push(items, item);
    }
    /*  — en contexte decl-assign booleen, on ne dumpe PAS le
     * catalogue runtime de constantes (les litteraux Vrai/Faux ci-dessus
     * suffisent). Natives/types/variables restent emis plus bas. */
    for (int i = 0; !boolLiteralOnly && i < OWL_CONST_CATALOG_COUNT; i++) {
        const OwlConstInfo* c = &OWL_CONST_CATALOG[i];
        /* Bascule EN comme les natives : en mode EN, label = nameEN si une
         * forme EN distincte existe, sinon fallback FR. Mode FR inchange. */
        bool cHasEN = (c->nameEN && *c->nameEN && strcmp(c->name, c->nameEN) != 0);
        const char* cLabel = (wantEN && cHasEN) ? c->nameEN : c->name;
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(cLabel));
        json_object_set(item, "kind", json_new_int(21));
        char detailBuf[256];
        snprintf(detailBuf, sizeof(detailBuf), "[%s] %s",
                 c->category ? c->category : "Divers",
                 c->description ? c->description : "");
        json_object_set(item, "detail", json_new_string(detailBuf));
        /*  — sortText deterministe : sans lui, VSCode retombe sur
         * l'ordre de declaration du catalogue et les langues sortent
         * incoherentes (Da/No/Fi/Gr/Tu/Ru/...). On trie alphabetiquement sur
         * le label affiche (meme forme EN/FR que cLabel), en minuscule pour un
         * ordre insensible a la casse. Pas de prefixe : toute lettre suit le
         * "!" (0x21) des variables/litteraux, qui restent donc en tete (miroir
         * du chemin Langues= ~L5344, sans la notion base/combinable). */
        char cSort[200];
        size_t sn = 0;
        for (const char* p = cLabel; *p && sn + 1 < sizeof(cSort); p++)
            cSort[sn++] = (char)tolower((unsigned char)*p);
        cSort[sn] = '\0';
        json_object_set(item, "sortText", json_new_string(cSort));
        json_array_push(items, item);
    }

    /* Registered builtin types (kind=22 Struct) — httpRequête, Chrono…
     *  — idem primitifs : retires en position valeur decl-assign. */
    for (int i = 0; !inDeclAssignValue && i < OWL_TYPE_CATALOG_COUNT; i++) {
        const OwlTypeInfo* t = &OWL_TYPE_CATALOG[i];
        /* Bascule EN comme les natives : en mode EN, label = nameEN si une
         * forme EN distincte existe, sinon fallback FR (forme canon accentuee). */
        bool tHasEN = (t->nameEN && *t->nameEN && strcmp(t->name, t->nameEN) != 0);
        const char* tLabel = (wantEN && tHasEN) ? t->nameEN : t->name;
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(tLabel));
        /*  — filterText combine label + canon FR + ascii + EN (miroir
         * du dump typeDecl ~L4940) pour matcher que l'on tape `Font` (EN) ou
         * `Police` (FR) quel que soit le label. Sans la forme EN, taper `Font`
         * ne suggerait pas Police (label FR, nameAscii == name → aucun
         * filterText auparavant). */
        {
            char ft[320];
            int off = snprintf(ft, sizeof(ft), "%s %s", tLabel, t->name);
            if (t->nameAscii && *t->nameAscii &&
                strcmp(t->nameAscii, t->name) != 0)
                off += snprintf(ft + off, sizeof(ft) - off, " %s", t->nameAscii);
            if (tHasEN)
                snprintf(ft + off, sizeof(ft) - off, " %s", t->nameEN);
            json_object_set(item, "filterText", json_new_string(ft));
        }
        json_object_set(item, "kind", json_new_int(22));
        char detailBuf[256];
        snprintf(detailBuf, sizeof(detailBuf), "[%s] %s",
                 t->category ? t->category : "Divers",
                 t->description ? t->description : "");
        json_object_set(item, "detail", json_new_string(detailBuf));
        json_array_push(items, item);
    }

    /*  — Scenario 3 : dans le body d'une methode inline, les champs
     * de la classe englobante sont visibles comme noms simples (acces
     * implicite ':'). On les injecte dans le pool de completion d'identifier. */
    if (enclosingClass) {
        for (int i = 0; i < enclosingClass->memberCount; i++) {
            const UserClassMember* m = &enclosingClass->members[i];
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(m->name));
            json_object_set(item, "kind",
                            json_new_int(m->isMethod ? 2 : 5));
            char detailBuf[200];
            if (m->isMethod)
                snprintf(detailBuf, sizeof(detailBuf), "%s.%s() [%s]",
                         enclosingClass->name, m->name,
                         m->vis == 'U' ? "public" : m->vis == 'R' ? "prive" : "protege");
            else
                snprintf(detailBuf, sizeof(detailBuf), "%s : %s [%s]",
                         m->name, m->type[0] ? m->type : "?",
                         m->vis == 'U' ? "public" : m->vis == 'R' ? "prive" : "protege");
            json_object_set(item, "detail", json_new_string(detailBuf));
            json_array_push(items, item);
        }
    }

    /*  — LHS scalaire (BOOL/NUM/STR) en position valeur decl-assign :
     * le type de retour des procedures (kind 3) et le type-valeur des champs
     * (userControls) ne sont pas captures au scan LSP. Filtrage fin impossible
     * -> exclusion EN BLOC, comme les mots-cles/types. Hors LHS
     * scalaire (classe/objet/enum/exotique) : permissif inchange. */
    bool lhsScalarDeclAssign = false;
    if (inDeclAssignValue) {
        ValTypeCat lcDA = value_type_category(declAssignType);
        lhsScalarDeclAssign = (lcDA == VT_BOOL || lcDA == VT_NUM || lcDA == VT_STR);
    }

    /*  (support #415) — portee des variables LOCALES. Une locale
     * (kind 6, non globale) n'est proposee que si SA declaration est celle
     * visible a la position du curseur, via le moteur de portee lexical
     * (, ). Les locales d'AUTRES procedures (meme fichier ou
     * autre doc ouvert), homonymes compris, sont masquees — ce qui ecarte
     * aussi les doublons de nom hors-portee. Globales (isGlobal), procedures,
     * types, structs, enums ont une portee fichier/projet : non filtres. */
    const char* scopeUri = td ? json_get_string(td, "uri") : NULL;
    int scopeLine = pos ? (int)json_get_int(pos, "line", 0) : 0;
    int scopeChar = pos ? (int)json_get_int(pos, "character", 0) : 0;

    /* User-defined symbols — filterText includes accent-stripped form so
     * "ResultatCompare" matches "RésultatCompare" typed without accents. */
    for (int i = 0; i < userSymbolCount; i++) {
        /*  — filtre de portee des locales (cf. note ci-dessus). */
        if (scopeUri && userSymbols[i].kind == 6 && !userSymbols[i].isGlobal) {
            ScopeResolveResult sr;
            if (!scope_resolve_at(scopeUri, scopeLine, scopeChar,
                                  userSymbols[i].name, &sr)
                || !sr.found
                || sr.docIndex != userSymbols[i].docIndex
                || sr.line != userSymbols[i].line
                || sr.character != userSymbols[i].character)
                continue;  /* locale hors de la portee du curseur */
        }
        /*  — position valeur d'un decl-assign : filtrer les variables
         * (kind 6) dont le type declare est prouve incompatible avec le LHS.
         * Les autres symboles (types, enums) restent intacts. */
        if (inDeclAssignValue && userSymbols[i].kind == 6
            && !value_type_assignable_to(userSymbols[i].detail, declAssignType))
            continue;
        /*  — procedures (kind 3) exclues pour un LHS scalaire. */
        if (lhsScalarDeclAssign && userSymbols[i].kind == 3)
            continue;
        JsonValue* item = json_new_object();
        json_object_set(item, "label", json_new_string(userSymbols[i].name));
        char stripped[256];
        scanner_strip_accents(userSymbols[i].name, (int)strlen(userSymbols[i].name),
                              stripped, sizeof(stripped));
        if (strcmp(stripped, userSymbols[i].name) != 0) {
            char ft[512]; snprintf(ft, sizeof(ft), "%s %s", userSymbols[i].name, stripped);
            json_object_set(item, "filterText", json_new_string(ft));
        }
        json_object_set(item, "kind", json_new_int(userSymbols[i].kind));
        json_object_set(item, "detail", json_new_string(userSymbols[i].detail));
        if (userSymbols[i].kind == 3) {
            char insertBuf[160];
            snprintf(insertBuf, sizeof(insertBuf), "%.127s($0)", userSymbols[i].name);
            json_object_set(item, "insertText", json_new_string(insertBuf));
            json_object_set(item, "insertTextFormat", json_new_int(2));
        }
        json_array_push(items, item);
    }

    /* ============================================================
     * DSL Fenetre v1 — autocomplete des proprietes (Phase F)
     *
     * Si le curseur est dans le body d'une 'Fenetre/Libelle/Bouton/
     * Saisie ... FIN', proposer les props du kind concerne. Les props
     * deja presentes plus haut dans le block sont filtrees.
     * ============================================================ */
    /* DSL v2 : autocomplete des events apres 'PROCEDURE ' dans body widget.
     * Ex : tape `PROCEDURE sur<ctrl-space>` dans body Saisie -> propose
     * surEntr\xc3\xa9e, surSortie, surAChaqueModification, surValidation.
     * Lifecycle 'Initialisation' / 'FinInitialisation' / 'Fermeture' aussi. */
    if (td && pos && !inStringContext && !singleDotContext && !doubleDotContext
        && !propValueContext && !simpleAssignContext && !callArgContext
        && !typeDeclContext) {
        const char* uri = json_get_string(td, "uri");
        int line = (int)json_get_int(pos, "line", 0);
        int character = (int)json_get_int(pos, "character", 0);
        int idx = find_doc(uri);
        if (idx >= 0) {
            const char* src = lspServer.documents[idx].content;
            const char* lp = src;
            int curL = 0;
            while (*lp && curL < line) { if (*lp == '\n') curL++; lp++; }
            character = lsp_utf16_to_byte_in_line(lp, character); /*  */

            /* Walk back depuis le curseur : skip mot partiel + whitespace,
             * verifie qu'on tombe sur 'PROCEDURE' ou 'FONCTION'. */
            int c = character - 1;
            #define IS_IDCH2(x) (((x) >= 'a' && (x) <= 'z') || \
                                  ((x) >= 'A' && (x) <= 'Z') || \
                                  ((x) >= '0' && (x) <= '9') || \
                                  (x) == '_' || (unsigned char)(x) >= 0x80)
            while (c >= 0 && IS_IDCH2(lp[c])) c--;
            while (c >= 0 && (lp[c] == ' ' || lp[c] == '\t')) c--;
            int kwEnd = c + 1;
            while (c >= 0 && IS_IDCH2(lp[c])) c--;
            int kwStart = c + 1;
            #undef IS_IDCH2
            int kwLen = kwEnd - kwStart;
            bool isProcedureKw = (kwLen == 9 && strncasecmp(lp + kwStart, "PROCEDURE", 9) == 0);
            bool isFunctionKw  = (kwLen == 8 && strncasecmp(lp + kwStart, "FONCTION", 8) == 0)
                              || (kwLen == 8 && strncasecmp(lp + kwStart, "FUNCTION", 8) == 0);
            bool afterProc = isProcedureKw || isFunctionKw;

            if (afterProc) {
                /* Calcule cursor_offset pour DslCtx. */
                int co = 0; int cl = 0; const char* pp = src;
                while (*pp && cl < line) { if (*pp == '\n') cl++; pp++; co++; }
                co += character;
                DslCtx dctx = detect_dsl_context_ex(src, co);
                char ctxw = dctx.widget;

                /* Si on est dans un body widget (mais pas dans PROPRIETES),
                 * on retourne la liste des events autorises. La table
                 * DSL_KIND_CHARS couvre tous les widget kinds runtime ; on
                 * accepte aussi 'E' (Etat top-level), 'e' (sous-bloc Etat),
                 * 'l' (LibelleEtat), 'm' (ImageEtat) qui ont leurs
                 * lifecycle events propres (Filtre/Lecture/AvantImpression).
                 *  Option A : 'l'/'m' ajoutes pour reparer
                 * autocomplete sous-controle Etat. */
                bool isWidgetCtx = (dsl_kind_char_to_control_kind(ctxw) >= 0)
                                || ctxw == 'E' || ctxw == 'e'
                                || ctxw == 'l' || ctxw == 'm' || ctxw == 'd'
                                || ctxw == 'G'   /*  : bloc collection */
                                || ctxw == 's'   /*  : bloc ServiceREST */
                                || ctxw == 'h';  /*  : bloc OpérationHTTP */
                if (isWidgetCtx && !dctx.inProprietes) {
                    /*  — source unique catalogues C. Plus de table
                     * statique LSP_LIFECYCLE_EVENTS : on route selon ctxw
                     * vers le bon catalogue lifecycle.
                     *   'F'         -> owl_window_lifecycle_catalog
                     *   'E'         -> owl_etat_event_catalog (top Etat)
                     *   'e'/'l'/'m' -> owl_etat_subcontrol_event_catalog
                     *   widget standard -> owl_event_catalog(kind)
                     *
                     * `Initialisation` n'est dans aucun catalogue ( :
                     * l'init implicite vit au top-level du widget/bloc Etat),
                     * `AprèsImpression` non plus (fantome elimine, ). */
                    /* Filtre les events deja declares dans le widget courant. */
                    int blockStart = find_dsl_block_start(src, co, ctxw);
                    /*  — scan TOUT le bloc widget (jusqu'a sa FIN),
                     * pas seulement [block_start, cursor). Sinon un event
                     * declare APRES le curseur n'est pas detecte. */
                    int blockEnd = find_dsl_block_end(src, blockStart, co);
                    /*  — si la structure `()` existe deja apres
                     * le curseur (cas snippet keyword PROCEDURE pose par
                     * Tab), insertText = juste le nom canon. Sinon snippet
                     * complet `nom()\n\t$0\nFIN`. */
                    bool parenFollows = lsp_paren_follows(src, co);
                    JsonValue* filtered = json_new_array();
                    /* Helper inline : pousse un completion item lifecycle.
                     *  — bilingue : en mode EN (`wantEN`) le label, le
                     * detail et le nom insere basculent sur la forme EN
                     * (nameEN / descEN) quand elle existe ; fallback FR sinon
                     * (jamais de vide).
                     *  — le filtre "deja declare" recoit la forme FR
                     * canon ET l'alias EN, pour reconnaitre une proc declaree
                     * dans l'une ou l'autre langue (cf. boucle widget typee). */
                    #define PUSH_LIFECYCLE_ITEM(evDef, frFallback) do {          \
                        const OwlEventDef* _d = (evDef);                         \
                        bool _hasEN = (_d->nameEN && *_d->nameEN &&              \
                                       strcmp(_d->canonical, _d->nameEN) != 0);  \
                        const char* _name = (wantEN && _hasEN)                   \
                                            ? _d->nameEN : _d->canonical;        \
                        const char* _det;                                        \
                        if (wantEN && _d->descEN && *_d->descEN) _det = _d->descEN; \
                        else if (_d->descFR && *_d->descFR)      _det = _d->descFR; \
                        else                                     _det = (frFallback); \
                        if (!event_already_declared(src, co, blockStart, blockEnd, _d->canonical, _d->nameEN)) { \
                            JsonValue* it = json_new_object();                   \
                            json_object_set(it, "label", json_new_string(_name)); \
                            json_object_set(it, "kind", json_new_int(3));        \
                            json_object_set(it, "detail", json_new_string(_det)); \
                            json_object_set(it, "sortText", json_new_string("!")); \
                            char snip[160];                                      \
                            if (parenFollows) {                                  \
                                snprintf(snip, sizeof(snip), "%s", _name);       \
                            } else {                                             \
                                snprintf(snip, sizeof(snip), "%s()\n\t$0\n%s", _name, snippet_end_kw(wantEN)); \
                            }                                                    \
                            json_object_set(it, "insertText", json_new_string(snip)); \
                            json_object_set(it, "insertTextFormat", json_new_int(2)); \
                            json_array_push(filtered, it);                       \
                        }                                                        \
                    } while (0)
                    if (ctxw == 'F') {
                        int n = owl_window_lifecycle_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_window_lifecycle_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event Fenetre");
                        }
                    } else if (ctxw == 'E') {
                        int n = owl_etat_event_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_etat_event_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event Etat");
                        }
                    } else if (ctxw == 'e' || ctxw == 'l' || ctxw == 'm' || ctxw == 'd') {
                        int n = owl_etat_subcontrol_event_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_etat_subcontrol_event_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event sous-controle Etat");
                        }
                    } else if (ctxw == 'G') {
                        /*  — bloc CollectionDeProcedures : Terminaison. */
                        int n = owl_collection_event_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_collection_event_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event collection");
                        }
                    } else if (ctxw == 's') {
                        /*  — bloc ServiceREST : hooks niveau service. */
                        int n = owl_service_event_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_service_event_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event service");
                        }
                    } else if (ctxw == 'h') {
                        /*  — bloc OpérationHTTP : surRequête. */
                        int n = owl_operation_event_catalog_count();
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d = owl_operation_event_catalog_at(i);
                            if (!d || !d->canonical) continue;
                            PUSH_LIFECYCLE_ITEM(d, "Event operation");
                        }
                    }
                    #undef PUSH_LIFECYCLE_ITEM
                    /* Events typés du catalogue (par widget kind). */
                    int controlKind = dsl_kind_char_to_control_kind(ctxw);
                    if (controlKind >= 0) {
                        int n = owl_event_catalog_count((OwlControlKind)controlKind);
                        for (int i = 0; i < n; i++) {
                            const OwlEventDef* d =
                                owl_event_catalog_at((OwlControlKind)controlKind, i);
                            if (!d || !d->canonical) continue;
                            /*  — filtre FR canon OU alias EN. */
                            if (event_already_declared(src, co, blockStart, blockEnd, d->canonical, d->nameEN))
                                continue;
                            /*  — bilingue : label / detail / nom insere
                             * basculent sur la forme EN en mode EN (fallback FR
                             * si pas de nameEN/descEN). */
                            bool evHasEN = (d->nameEN && *d->nameEN &&
                                            strcmp(d->canonical, d->nameEN) != 0);
                            const char* evName = (wantEN && evHasEN)
                                                 ? d->nameEN : d->canonical;
                            JsonValue* item = json_new_object();
                            json_object_set(item, "label", json_new_string(evName));
                            json_object_set(item, "kind", json_new_int(3));
                            /* Detail : desc courte selon la langue active. */
                            const char* detail;
                            if (wantEN && d->descEN && *d->descEN) detail = d->descEN;
                            else if (d->descFR && *d->descFR)      detail = d->descFR;
                            else                                   detail = "Event";
                            json_object_set(item, "detail", json_new_string(detail));
                            json_object_set(item, "sortText", json_new_string("!"));
                            /* Documentation (hover) : signature complète typée
                             * + desc (EN priorisee en mode EN) + détail params. */
                            const char* docDescPrimary =
                                (wantEN && d->descEN && *d->descEN) ? d->descEN
                                : (d->descFR ? d->descFR : "");
                            const char* docDescAlt =
                                (wantEN && d->descEN && *d->descEN)
                                ? (d->descFR ? d->descFR : "")
                                : (d->descEN ? d->descEN : "");
                            char doc[2048];
                            int off = snprintf(doc, sizeof(doc),
                                "**`PROCEDURE %s", evName);
                            for (int p = 0; p < d->paramCount && off < (int)sizeof(doc) - 64; p++) {
                                off += snprintf(doc + off, sizeof(doc) - off,
                                    "%s%s est un %s",
                                    p == 0 ? "(" : ", ",
                                    d->params[p].name,
                                    d->params[p].typeName);
                            }
                            off += snprintf(doc + off, sizeof(doc) - off,
                                "%s`**\n\n%s\n\n*%s: %s*",
                                d->paramCount > 0 ? ")" : "()",
                                docDescPrimary,
                                wantEN ? "FR" : "EN",
                                docDescAlt);
                            for (int p = 0; p < d->paramCount && off < (int)sizeof(doc) - 200; p++) {
                                off += snprintf(doc + off, sizeof(doc) - off,
                                    "\n\n- **`%s`** (`%s`) : %s",
                                    d->params[p].name,
                                    d->params[p].typeName,
                                    d->params[p].descFR ? d->params[p].descFR : "");
                            }
                            JsonValue* docObj = json_new_object();
                            json_object_set(docObj, "kind", json_new_string("markdown"));
                            json_object_set(docObj, "value", json_new_string(doc));
                            json_object_set(item, "documentation", docObj);
                            /* Snippet : signature complete typee avec placeholders.
                             *  — si `()` existe deja apres le curseur
                             * (snippet keyword PROCEDURE pose par Tab), on
                             * n'emet que le nom canon (sinon `FIN()` parasite
                             * + double `FIN`). Les params eventuels (rares :
                             * surClic/surDoubleClic ont 0 param) seront a
                             * completer a la main entre les `()` existants. */
                            char snip[512];
                            if (parenFollows) {
                                snprintf(snip, sizeof(snip), "%s", evName);
                            } else {
                                int soff = snprintf(snip, sizeof(snip), "%s", evName);
                                for (int p = 0; p < d->paramCount && soff < (int)sizeof(snip) - 64; p++) {
                                    soff += snprintf(snip + soff, sizeof(snip) - soff,
                                        "%s${%d:%s} est un %s",
                                        p == 0 ? "(" : ", ",
                                        p + 1,
                                        d->params[p].name,
                                        d->params[p].typeName);
                                }
                                soff += snprintf(snip + soff, sizeof(snip) - soff,
                                    "%s\n\t$0\n%s",
                                    d->paramCount > 0 ? ")" : "()",
                                    snippet_end_kw(wantEN));
                            }
                            json_object_set(item, "insertText", json_new_string(snip));
                            json_object_set(item, "insertTextFormat", json_new_int(2));
                            json_array_push(filtered, item);
                        }
                    }
                    json_free(items);
                    return filtered;
                }

                /*  — top-level d'un document projet.owl : proposer
                 * les events de cycle de vie du PROJET (Fermeture). Ils sont
                 * declares en sibling top-level du bloc `IDENT est un Projet`
                 * (hors body widget) — d'ou le placement apres la branche
                 * isWidgetCtx. Minimal : uniquement le catalogue projet, pas
                 * de fuite du catalogue Fenetre (FinInitialisation & co). */
                if (isProcedureKw && !isWidgetCtx && source_declares_projet(src)) {
                    bool parenFollowsProj = lsp_paren_follows(src, co);
                    int projBlockEnd = (int)strlen(src);
                    JsonValue* projItems = json_new_array();
                    size_t np = sizeof(PROJET_LIFECYCLE_EVENTS)
                              / sizeof(PROJET_LIFECYCLE_EVENTS[0]);
                    for (size_t i = 0; i < np; i++) {
                        const OwlEventDef* d = &PROJET_LIFECYCLE_EVENTS[i];
                        if (event_already_declared(src, co, 0, projBlockEnd,
                                                   d->canonical, d->nameEN))
                            continue;
                        bool evHasEN = (d->nameEN && *d->nameEN &&
                                        strcmp(d->canonical, d->nameEN) != 0);
                        const char* evName = (wantEN && evHasEN)
                                             ? d->nameEN : d->canonical;
                        const char* det;
                        if (wantEN && d->descEN && *d->descEN) det = d->descEN;
                        else if (d->descFR && *d->descFR)      det = d->descFR;
                        else                                   det = "Event Projet";
                        JsonValue* it = json_new_object();
                        json_object_set(it, "label", json_new_string(evName));
                        json_object_set(it, "kind", json_new_int(3));
                        json_object_set(it, "detail", json_new_string(det));
                        json_object_set(it, "sortText", json_new_string("!"));
                        char snip[160];
                        if (parenFollowsProj)
                            snprintf(snip, sizeof(snip), "%s", evName);
                        else
                            snprintf(snip, sizeof(snip), "%s()\n\t$0\n%s", evName, snippet_end_kw(wantEN));
                        json_object_set(it, "insertText", json_new_string(snip));
                        json_object_set(it, "insertTextFormat", json_new_int(2));
                        json_array_push(projItems, it);
                    }
                    json_free(items);
                    return projItems;
                }

                /*  +  — Fallback hors contexte widget :
                 * router par contexte de fichier réel.
                 *
                 *   1. Body classe (source contient `<ident> est une Classe`
                 *      en tête)        → snippets Constructeur / Destructeur
                 *                        filtrés par event_already_declared.
                 *   2. Fichier test — règle d'or OWL : basename matche
                 *      `test_*.owl` UNIQUEMENT (cf. ). Un tag
                 *      `@sujet` en en-tête NE classifie PAS un fichier
                 *      comme test côté outils utilisateur ; il reste
                 *      strictement une métadonnée interne des scénarios
                 *      PUB du projet C OWL.
                 *                       → fixtures runner + préfixes
                 *                         scenario_/test_/should_.
                 *   3. Module standalone → liste user déjà collectée dans
                 *                         `items` (pas d'injection de
                 *                         patterns runtime spécifiques).
                 *
                 * Pré-calcul du contexte. */
                bool isClassBody = is_class_file_source(src);
                bool isTestFile = false;
                if (!isClassBody) {
                    char filepath[1024] = {0};
                    uri_to_filepath(uri, filepath, sizeof(filepath));
                    const char* fbase = owl_format_path_basename(filepath);
                    isTestFile = owl_format_basename_is_test(fbase);
                }

                if (isClassBody) {
                    /*  : `FONCTION ` en body classe — un constructeur
                     * ou destructeur n'a pas de type de retour, proposer
                     * Constructeur/Destructeur ici induit l'user en erreur.
                     * On laisse la liste user (symboles deja collectes) ;
                     * l'utilisateur tape librement le nom de sa fonction. */
                    if (!isProcedureKw) {
                        return items;
                    }
                    /* Body classe : Constructeur / Destructeur (les deux
                     * seuls "events" lifecycle propres à une Classe). */
                    int blockStart = find_dsl_block_start(src, co, ctxw);
                    /* find_dsl_block_start avec ctxw='\0' renvoie 0 (pas
                     * de bloc widget) — on filtre alors sur tout le source
                     * ( : EOF, pas cursor — un Constructeur declare
                     * apres le curseur doit aussi etre filtre). */
                    (void)blockStart;
                    int classBlockEnd = (int)strlen(src);
                    static const char* CLASS_EVENTS[] = {"Constructeur", "Destructeur"};
                    /*  : prefixe `detail` aligne sur lifecycle widget
                     * (cf. LIFECYCLE_EV plus haut : "Lifecycle : ..."). */
                    static const char* CLASS_EVENTS_DETAIL[] = {
                        "Lifecycle : appelee a l'instanciation",
                        "Lifecycle : appelee a la destruction"
                    };
                    JsonValue* classItems = json_new_array();
                    for (size_t i = 0; i < 2; i++) {
                        /* Body classe : snippet toujours FR (Constructeur/
                         * Destructeur), pas de gap bilingue -> alias NULL. */
                        if (event_already_declared(src, co, 0, classBlockEnd, CLASS_EVENTS[i], NULL))
                            continue;
                        JsonValue* item = json_new_object();
                        json_object_set(item, "label", json_new_string(CLASS_EVENTS[i]));
                        json_object_set(item, "kind", json_new_int(3));
                        json_object_set(item, "detail", json_new_string(CLASS_EVENTS_DETAIL[i]));
                        json_object_set(item, "sortText", json_new_string("!"));
                        char snip[160];
                        snprintf(snip, sizeof(snip), "%s()\n\t$0\n%s", CLASS_EVENTS[i], snippet_end_kw(wantEN));
                        json_object_set(item, "insertText", json_new_string(snip));
                        json_object_set(item, "insertTextFormat", json_new_int(2));
                        json_array_push(classItems, item);
                    }
                    json_free(items);
                    return classItems;
                }

                if (!isTestFile) {
                    /* Module standalone : pas d'injection de fixtures test
                     * ni de Constructeur/Destructeur. On retourne la liste
                     * `items` (symboles user déjà collectés). L'utilisateur
                     * tape librement son nom de procédure. */
                    return items;
                }

                /* Fichier test confirmé → fixtures runner + préfixes
                 * scenario_/test_/should_.
                 *
                 * Filtre langue : on respecte lspServer.language (FR par
                 * défaut, EN si initializationOptions.language="en"). Cf
                 * cahier des charges §4 LSP unilingue paramétrable. */
                bool fxWantEN = (strcmp(lspServer.language, "en") == 0);
                /* `fr` flag : 1 = nom FR canon ; 0 = alias EN. */
                static const struct {
                    const char* name;
                    const char* detail;
                    int isFR;
                } TEST_FIXTURES[] = {
                    {"avantToutTest",   "Fixture test runner : exécutée une seule fois avant tous les scénarios", 1},
                    {"avantChaqueTest", "Fixture test runner : exécutée avant chaque scénario", 1},
                    {"apresChaqueTest", "Fixture test runner : exécutée après chaque scénario", 1},
                    {"apresToutTest",   "Fixture test runner : exécutée une seule fois après tous les scénarios", 1},
                    {"beforeAllTests",  "Test runner fixture: runs once before all scenarios", 0},
                    {"beforeEachTest",  "Test runner fixture: runs before each scenario", 0},
                    {"afterEachTest",   "Test runner fixture: runs after each scenario", 0},
                    {"afterAllTests",   "Test runner fixture: runs once after all scenarios", 0},
                };
                JsonValue* fixtures = json_new_array();
                for (size_t i = 0; i < sizeof(TEST_FIXTURES)/sizeof(TEST_FIXTURES[0]); i++) {
                    /* En mode FR (défaut) : skip les alias EN. En mode EN : skip les FR. */
                    if (fxWantEN && TEST_FIXTURES[i].isFR) continue;
                    if (!fxWantEN && !TEST_FIXTURES[i].isFR) continue;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(TEST_FIXTURES[i].name));
                    json_object_set(item, "kind", json_new_int(3));  /* Function */
                    json_object_set(item, "detail", json_new_string(TEST_FIXTURES[i].detail));
                    json_object_set(item, "sortText", json_new_string("!"));
                    char snip[160];
                    snprintf(snip, sizeof(snip), "%s()\n\t$0\n%s", TEST_FIXTURES[i].name, snippet_end_kw(fxWantEN));
                    json_object_set(item, "insertText", json_new_string(snip));
                    json_object_set(item, "insertTextFormat", json_new_int(2));
                    json_array_push(fixtures, item);
                }
                /* Préfixe scenario_ : snippet avec placeholder pour le suffixe.
                 * Les 3 préfixes restent disponibles dans les deux langues
                 * (scenario_ est un nom technique, pas une traduction). */
                static const char* SCEN_PREFIXES[] = {"scenario_", "test_", "should_"};
                for (size_t i = 0; i < 3; i++) {
                    JsonValue* item = json_new_object();
                    char label[64];
                    snprintf(label, sizeof(label), "%snom", SCEN_PREFIXES[i]);
                    json_object_set(item, "label", json_new_string(label));
                    json_object_set(item, "kind", json_new_int(3));
                    json_object_set(item, "detail",
                        json_new_string(fxWantEN
                            ? "Test runner scenario (charter §4)"
                            : "Scénario du test runner (charte §4)"));
                    json_object_set(item, "sortText", json_new_string("!"));
                    char snip[160];
                    snprintf(snip, sizeof(snip), "%s${1:nom}()\n\t$0\n%s",
                             SCEN_PREFIXES[i], snippet_end_kw(fxWantEN));
                    json_object_set(item, "insertText", json_new_string(snip));
                    json_object_set(item, "insertTextFormat", json_new_int(2));
                    json_array_push(fixtures, item);
                }
                json_free(items);
                return fixtures;
            }
        }
    }

    if (td && pos && !inStringContext && !singleDotContext && !doubleDotContext
        && !propValueContext && !simpleAssignContext && !callArgContext
        && !typeDeclContext) {
        const char* uri = json_get_string(td, "uri");
        int line = (int)json_get_int(pos, "line", 0);
        int character = (int)json_get_int(pos, "character", 0);
        int idx = find_doc(uri);
        if (idx >= 0) {
            const char* src = lspServer.documents[idx].content;
            /* Calcule cursor_offset depuis (line, character). */
            int curLine = 0;
            int cursor_offset = 0;
            const char* p = src;
            while (*p && curLine < line) {
                if (*p == '\n') curLine++;
                p++;
                cursor_offset++;
            }
            /* c — `character` est en unités UTF-16 (convention LSP), pas
             * en octets. Le convertir en offset OCTET dans la ligne en marchant
             * l'UTF-8. Sans ça, un caractère multi-octets avant le curseur (ex.
             * `Icône`) décale cursor_offset et casse la détection de brace
             * literal `= {‸}`. ASCII : identique, donc aucune régression. */
            {
                int units = 0;
                while (*p && *p != '\n' && units < character) {
                    unsigned char b = (unsigned char)*p;
                    int blen = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 :
                               (b >= 0xC0) ? 2 : 1;
                    int u = (blen == 4) ? 2 : 1;  /* astral = 2 unités UTF-16 */
                    if (units + u > character) break;
                    p += blen;
                    cursor_offset += blen;
                    units += u;
                }
            }

            DslCtx dctx = detect_dsl_context_ex(src, cursor_offset);
            char ctx = dctx.widget;
            const char* kindStr = dsl_kind_string(ctx);

            /*  — bloc Projet (type composé top-level, pas un widget ;
             * ctx=='J'). Deux sous-contextes dans le PROPRIETES :
             *   (1) `Langues = <cursor>` (scalaire ou `[`) → constantes langueXxx.
             *   (2) sinon → noms de champs du catalogue du type Projet, filtrés
             *       (parité widgets : champ déjà posé non re-proposé).
             * Return early : le set est autonome (pas de fusion avec les
             * completions widget/top-level). */
            if (ctx == 'J' && dctx.inProprietes) {
                /* Mot de tête de la ligne courante + présence d'un `=`. */
                int lineStart = cursor_offset;
                while (lineStart > 0 && src[lineStart-1] != '\n') lineStart--;
                const char* lp = src + lineStart;
                const char* lend = src + cursor_offset;
                while (lp < lend && (*lp == ' ' || *lp == '\t')) lp++;
                const char* wordStart = lp;
                while (lp < lend && ((*lp >= 'a' && *lp <= 'z')
                                   || (*lp >= 'A' && *lp <= 'Z')
                                   || (*lp >= '0' && *lp <= '9')
                                   || *lp == '_' || (unsigned char)*lp >= 0x80)) lp++;
                int wlen = (int)(lp - wordStart);
                while (lp < lend && (*lp == ' ' || *lp == '\t')) lp++;
                bool hasEqual = (lp < lend && *lp == '=');
                bool isLanguesValue = false;
                if (hasEqual && wlen > 0 && wlen < 32) {
                    char w[32];
                    memcpy(w, wordStart, wlen);
                    w[wlen] = '\0';
                    isLanguesValue = (strcasecmp(w, "Langues") == 0
                                   || strcasecmp(w, "Languages") == 0);
                }

                if (isLanguesValue) {
                    /* (1) Constantes langueXxx (source unique : enum registry,
                     * la même que la validation compile du champ Langues).
                     * Couvre le scalaire `Langues = ` ET la liste `Langues = [`
                     * (détection identique : mot de tête + `=`). */
                    JsonValue* langItems = json_new_array();
                    const OwlEnumInfo* le = owl_enum_info_find("langueXxx");
                    if (le && le->values) {
                        for (const char* const* v = le->values; *v; v++) {
                            const char* vLabel = *v;
                            if (wantEN) {
                                for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
                                    if (strcasecmp(OWL_CONST_CATALOG[k].name, *v) == 0) {
                                        if (OWL_CONST_CATALOG[k].nameEN
                                            && *OWL_CONST_CATALOG[k].nameEN)
                                            vLabel = OWL_CONST_CATALOG[k].nameEN;
                                        break;
                                    }
                                }
                            }
                            JsonValue* item = json_new_object();
                            json_object_set(item, "label", json_new_string(vLabel));
                            json_object_set(item, "kind", json_new_int(21)); /* Constant */
                            if (vLabel != *v && strcmp(vLabel, *v) != 0) {
                                char ft[256];
                                snprintf(ft, sizeof(ft), "%s %s", vLabel, *v);
                                json_object_set(item, "filterText", json_new_string(ft));
                            }
                            json_object_set(item, "detail", json_new_string("[langueXxx]"));
                            char sortBuf[200];
                            snprintf(sortBuf, sizeof(sortBuf), "0_%.180s", vLabel);
                            json_object_set(item, "sortText", json_new_string(sortBuf));
                            json_array_push(langItems, item);
                        }
                    }
                    json_free(items);
                    return langItems;
                }

                /*  — hasEqual (contexte VALEUR) mais champ non-Langues :
                 * basculer en completion de valeur TYPEE selon le type declare du
                 * champ Projet. `Thème` → instances `est un Thème` du projet ;
                 * `FenêtreDémarrage` (type Fenêtre) → instances `est une Fenêtre` ;
                 * champ scalaire (Description/Auteur/Licence) → set vide (jamais de
                 * nom de champ apres `=`). Return early dans tous les cas. */
                if (hasEqual) {
                    JsonValue* valItems = json_new_array();
                    const OwlTypeInfo* pj = owl_type_info_find("Projet");
                    if (pj && wlen > 0 && wlen < 128) {
                        char fieldName[128];
                        memcpy(fieldName, wordStart, wlen);
                        fieldName[wlen] = '\0';
                        char fnStrip[128];
                        scanner_strip_accents(fieldName, (int)strlen(fieldName),
                                              fnStrip, sizeof(fnStrip));
                        const char* fieldType = NULL;
                        for (int i = 0; i < pj->fieldCount; i++) {
                            const OwlFieldInfo* f = &pj->fields[i];
                            char nStrip[128];
                            scanner_strip_accents(f->name, (int)strlen(f->name),
                                                  nStrip, sizeof(nStrip));
                            if (strcasecmp(fnStrip, nStrip) == 0
                                || (f->nameAscii && strcasecmp(fnStrip, f->nameAscii) == 0)
                                || (f->nameEN && strcasecmp(fnStrip, f->nameEN) == 0)) {
                                fieldType = f->type;
                                break;
                            }
                        }
                        if (fieldType) {
                            char ftStrip[64];
                            scanner_strip_accents(fieldType, (int)strlen(fieldType),
                                                  ftStrip, sizeof(ftStrip));
                            if (strcasecmp(ftStrip, "Theme") == 0)
                                emit_typed_instances(valItems, "Theme", fieldType);
                            else if (strcasecmp(ftStrip, "Fenetre") == 0)
                                emit_window_instances(valItems, fieldType);
                        }
                    }
                    json_free(items);
                    return valItems;
                }

                /* (2) Noms de champs du catalogue Projet (kind 10 Property). */
                const OwlTypeInfo* projet = owl_type_info_find("Projet");
                JsonValue* fieldItems = json_new_array();
                if (projet) {
                    int blockStart = find_dsl_block_start(src, cursor_offset, 'J');
                    for (int i = 0; i < projet->fieldCount; i++) {
                        const OwlFieldInfo* f = &projet->fields[i];
                        if (prop_already_used(src, cursor_offset, blockStart, f->name)) continue;
                        if (f->nameAscii && strcmp(f->nameAscii, f->name) != 0
                            && prop_already_used(src, cursor_offset, blockStart, f->nameAscii)) continue;
                        if (f->nameEN
                            && prop_already_used(src, cursor_offset, blockStart, f->nameEN)) continue;
                        bool fHasEN = wantEN && f->nameEN
                                      && strcmp(f->nameEN, f->name) != 0;
                        const char* label = fHasEN ? f->nameEN : f->name;
                        JsonValue* item = json_new_object();
                        json_object_set(item, "label", json_new_string(label));
                        json_object_set(item, "kind", json_new_int(10)); /* Property */
                        json_object_set(item, "sortText", json_new_string("!"));
                        if (fHasEN) {
                            char ft[256];
                            snprintf(ft, sizeof(ft), "%s %s", f->nameEN, f->name);
                            json_object_set(item, "filterText", json_new_string(ft));
                        } else if (f->nameAscii && strcmp(f->nameAscii, f->name) != 0) {
                            json_object_set(item, "filterText", json_new_string(f->nameAscii));
                        }
                        char detail[160];
                        snprintf(detail, sizeof(detail), "%s — Projet",
                                 (wantEN && f->typeEN) ? f->typeEN
                                 : (f->type ? f->type : "?"));
                        json_object_set(item, "detail", json_new_string(detail));
                        char insert[160];
                        snprintf(insert, sizeof(insert), "%s = $0", label);
                        json_object_set(item, "insertText", json_new_string(insert));
                        json_object_set(item, "insertTextFormat", json_new_int(2));
                        json_array_push(fieldItems, item);
                    }
                }
                json_free(items);
                return fieldItems;
            }

            /* V1.5-C : detection brace literal `var = { <cursor> ... }`.
             * Walk en arriere depuis cursor_offset pour trouver le `{`
             * matching non-fermé. Si on est dans un brace literal, lookup
             * le type cible via le LHS du `=` qui le precede. */
            int braceOpenOff = -1;
            char braceTypeBuf[64];
            int  braceTypeLen = 0;
            /*  : si le LHS du `=` est un type registry avec un
             * literalKeysEnum (ex chaineMultilingue), on propose les valeurs
             * de cet enum comme cles du literal {}. Distinct des types
             * composes (Cadre/Police) traites via OWL_COMPOSED_TYPE_CATALOG. */
            const OwlEnumInfo* literalKeysEnum = NULL;
            bool literalTypeKnownNoEnum = false;  /* type registry connu mais sans literalKeysEnum */
            /*  — LHS = type registry a fields (Icône, durée…) : propose
             * ses sous-proprietes comme cles du literal {}. */
            const OwlTypeInfo* literalFieldsType = NULL;
            /*  — langue d'emission des cles du literal typé : EN si le
             * mode langue LSP est en (wantEN) OU si le nom de type écrit est la
             * forme EN canon (`duration`, `Icon`). Sinon FR canon. */
            bool literalFieldsEN = false;
            {
                /* Scan en arriere : track depth de '{}' et trouve le '{'
                 * non clos qui ouvre notre brace literal courant. */
                int depth = 0;
                bool inString = false;
                for (int i = cursor_offset - 1; i >= 0; i--) {
                    char ch = src[i];
                    if (ch == '"' && (i == 0 || src[i-1] != '\\')) {
                        inString = !inString;
                        continue;
                    }
                    if (inString) continue;
                    if (ch == '}') depth++;
                    else if (ch == '{') {
                        if (depth == 0) { braceOpenOff = i; break; }
                        depth--;
                    }
                }
                if (braceOpenOff >= 0) {
                    /* Walk avant le `{` : skip espaces, on doit trouver `=`. */
                    int j = braceOpenOff - 1;
                    while (j >= 0 && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j--;
                    if (j >= 0 && src[j] == '=') {
                        /* Avant `=` : skip espaces puis lit l'IDENT du LHS
                         * (prop name OU nom de variable apres `est un T`). */
                        j--;
                        while (j >= 0 && (src[j] == ' ' || src[j] == '\t')) j--;
                        int identEnd = j + 1;
                        while (j >= 0 && ((src[j] >= 'a' && src[j] <= 'z') ||
                                          (src[j] >= 'A' && src[j] <= 'Z') ||
                                          (src[j] >= '0' && src[j] <= '9') ||
                                          src[j] == '_' || (unsigned char)src[j] >= 0x80)) j--;
                        int identStart = j + 1;
                        if (identEnd > identStart) {
                            char lhsName[64];
                            int  lhsLen = identEnd - identStart;
                            if (lhsLen >= (int)sizeof(lhsName)) lhsLen = sizeof(lhsName) - 1;
                            memcpy(lhsName, src + identStart, lhsLen);
                            lhsName[lhsLen] = '\0';
                            /* Heuristique pour deduire le type cible :
                             *   1. Si le LHS est un nom de prop DSL connue (Cadre,
                             *      Police, Style) → le type est ce nom.
                             *   2. Sinon scan en arriere pour `IDENT est un T` puis
                             *      dans le bloc { de plus haut niveau, on prend T.
                             * V1.5 : on couvre cas 1 via lookup catalog DSL props
                             *        + matching nom-de-type direct. */
                            for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                                if (strcasecmp(f->typeName, lhsName) == 0) {
                                    /* lhsName matche un type composé = cas
                                     * `prop = { ... }` où la prop s'appelle Cadre. */
                                    int copy = (int)strlen(f->typeName);
                                    if (copy >= (int)sizeof(braceTypeBuf)) copy = sizeof(braceTypeBuf) - 1;
                                    memcpy(braceTypeBuf, f->typeName, copy);
                                    braceTypeBuf[copy] = '\0';
                                    braceTypeLen = copy;
                                    break;
                                }
                            }
                            /* Cas 2 : LHS est un sous-objet (ex CoinHautGauche dans
                             * un brace Cadre). On regarde si lhsName est un fieldName
                             * d'un type composé connu, on prend son subTypeName.
                             *  : comparaison accent-insensible (miroir de
                             * composed_field_subtype). Le catalogue stocke les
                             * fieldName accent-strippés (`CadreExterieur`) alors que
                             * la source canonique est accentuée (`CadreExtérieur`) —
                             * sans strip, le sous-type ne se résout pas et la
                             * complétion est vide (invariant #2). */
                            if (braceTypeLen == 0) {
                                char lhsAscii[64];
                                scanner_strip_accents(lhsName, (int)strlen(lhsName),
                                                      lhsAscii, sizeof(lhsAscii));
                                for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                    const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                                    char fnAscii[64];
                                    scanner_strip_accents(f->fieldName, (int)strlen(f->fieldName),
                                                          fnAscii, sizeof(fnAscii));
                                    if ((strcasecmp(f->fieldName, lhsName) == 0
                                         || strcasecmp(fnAscii, lhsAscii) == 0)
                                        && f->subTypeName) {
                                        int copy = (int)strlen(f->subTypeName);
                                        if (copy >= (int)sizeof(braceTypeBuf)) copy = sizeof(braceTypeBuf) - 1;
                                        memcpy(braceTypeBuf, f->subTypeName, copy);
                                        braceTypeBuf[copy] = '\0';
                                        braceTypeLen = copy;
                                        break;
                                    }
                                }
                            }
                            /*  — LHS = prop widget de type OBJECT
                             * (ex Style sur une Saisie → styleSaisie). La prop
                             * elle-même n'est pas un type composé ; on résout
                             * son objectType via le catalogue DSL du widget
                             * courant (ctx), puis on vérifie qu'il désigne bien
                             * un type composé connu. Débloque l'autocomplete des
                             * champs de styleSaisie dans `Style = { <cursor> }`. */
                            if (braceTypeLen == 0 && ctx) {
                                const OwlDSLPropInfo* dprop = dsl_prop_find(ctx, lhsName);
                                if (dprop && dprop->objectType && *dprop->objectType) {
                                    for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                        if (strcasecmp(OWL_COMPOSED_TYPE_CATALOG[k].typeName,
                                                       dprop->objectType) == 0) {
                                            int copy = (int)strlen(dprop->objectType);
                                            if (copy >= (int)sizeof(braceTypeBuf)) copy = sizeof(braceTypeBuf) - 1;
                                            memcpy(braceTypeBuf, dprop->objectType, copy);
                                            braceTypeBuf[copy] = '\0';
                                            braceTypeLen = copy;
                                            break;
                                        }
                                    }
                                }
                            }
                            /*  : LHS = type registry avec
                             * literalKeysEnum (chaineMultilingue → langues) ou
                             * fields (Icône, durée…). Resolu seulement si pas
                             * deja un type compose.
                             *
                             *  — durée passe désormais par ce chemin
                             * générique (registry-driven, bilingue) : plus de cas
                             * spécial hardcodé FR-only. Le nom de type écrit
                             * (`duration`/`durée`) est capturé dans
                             * `resolvedTypeName` pour choisir la langue des
                             * composantes émises (forme EN → clés EN). */
                            if (braceTypeLen == 0) {
                                char resolvedTypeName[64];
                                snprintf(resolvedTypeName, sizeof(resolvedTypeName),
                                         "%s", lhsName);
                                const OwlTypeInfo* ti = owl_type_info_find(lhsName);
                                /*  — lhsName est un nom de variable
                                 * (declaration separee de l'affectation :
                                 * `m est une chaineMultilingue` puis `m = {`).
                                 * owl_type_info_find("m") == NULL ; on scanne
                                 * en arriere `m est un[e] <T>` pour resoudre le
                                 * type declare, puis on retente le lookup.
                                 * Mirror du scan-arriere durée. */
                                if (!ti) {
                                    for (int li = identStart - 1; li >= 0; li--) {
                                        if (li + lhsLen > cursor_offset) continue;
                                        if (strncasecmp(src + li, lhsName, lhsLen) != 0) continue;
                                        /* Frontiere gauche : evite de matcher
                                         * lhsName a l'interieur d'un identifiant
                                         * plus long (ex "m" dans "nom"). */
                                        if (li > 0) {
                                            unsigned char pc = (unsigned char)src[li-1];
                                            if ((pc >= 'a' && pc <= 'z') || (pc >= 'A' && pc <= 'Z')
                                                || (pc >= '0' && pc <= '9') || pc == '_' || pc >= 0x80)
                                                continue;
                                        }
                                        int after = li + lhsLen;
                                        while (src[after] == ' ' || src[after] == '\t') after++;
                                        /*  — copule+article bilingue
                                         * (`est un[e]` FR / `is a[n]` EN /
                                         * formes mixtes `is un`, `est a`). */
                                        int q = scan_copule_article_bytes(src, after);
                                        if (q < 0) continue;
                                        /* lit le token type declare (ident UTF-8). */
                                        int te = q;
                                        while (src[te] && ((src[te] >= 'a' && src[te] <= 'z') ||
                                                           (src[te] >= 'A' && src[te] <= 'Z') ||
                                                           (src[te] >= '0' && src[te] <= '9') ||
                                                           src[te] == '_' ||
                                                           (unsigned char)src[te] >= 0x80)) te++;
                                        int tlen = te - q;
                                        if (tlen > 0 && tlen < 64) {
                                            char tname[64];
                                            memcpy(tname, src + q, tlen);
                                            tname[tlen] = '\0';
                                            ti = owl_type_info_find(tname);
                                            if (ti)
                                                snprintf(resolvedTypeName,
                                                         sizeof(resolvedTypeName),
                                                         "%s", tname);
                                        }
                                        break;
                                    }
                                }
                                if (ti && ti->literalKeysEnum) {
                                    literalKeysEnum =
                                        owl_enum_info_find(ti->literalKeysEnum);
                                } else if (ti && ti->fields && ti->fieldCount > 0) {
                                    /*  — type registry a fields (Icône,
                                     * durée…) : propose ses sous-proprietes comme
                                     * cles du literal {}. */
                                    literalFieldsType = ti;
                                    /*  — langue des cles emises : EN si
                                     * mode langue LSP=en, OU si le nom de type
                                     * ecrit est la forme EN canon (`duration`,
                                     * `Icon`) et non la forme FR. */
                                    literalFieldsEN =
                                        wantEN ||
                                        type_name_written_is_en(ti, resolvedTypeName);
                                } else {
                                    /* LHS d'un literal {} typé qui n'est ni un
                                     * type composé (Cadre/Police) ni un type a
                                     * literalKeysEnum (chaineMultilingue) ni un
                                     * type a fields : pas de cles proposables.
                                     * On ne pollue pas le literal avec le
                                     * catalogue general. Couvre les types
                                     * primitifs (Date) et inconnus. */
                                    literalTypeKnownNoEnum = true;
                                }
                            }
                        }
                    }
                }
            }

            /*  : literal {} d'un type a literalKeysEnum — propose
             * les valeurs de l'enum (kind Constant). Robuste au contenu deja
             * tape : on n'emet pas de filtre par cle, l'utilisateur peut
             * reproposer la meme langue (override de traduction). */
            if (literalKeysEnum && literalKeysEnum->values) {
                JsonValue* enumItems = json_new_array();
                for (const char* const* v = literalKeysEnum->values; *v; v++) {
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(*v));
                    json_object_set(item, "kind", json_new_int(13)); /* EnumMember */
                    char detailBuf[256];
                    snprintf(detailBuf, sizeof(detailBuf), "Cle de %s",
                             literalKeysEnum->name);
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    char sortBuf[200];
                    snprintf(sortBuf, sizeof(sortBuf), "0_%.180s", *v);
                    json_object_set(item, "sortText", json_new_string(sortBuf));
                    json_array_push(enumItems, item);
                }
                json_free(items);
                return enumItems;
            }

            /*  — literal {} d'un type registry a fields (Icône, durée…) :
             * propose ses sous-proprietes (Couleur, Rotation… / Jour, Heure…),
             * filtre celles deja saisies dans le brace courant.
             *  — bilingue : `literalFieldsEN` choisit la forme EN
             * (`Hour`, `Color`) quand le mode langue est en ou que le nom de
             * type ecrit est la forme EN. Filtre accepte les deux formes. */
            if (literalFieldsType && braceOpenOff >= 0) {
                JsonValue* fieldItems = json_new_array();
                for (int fi = 0; fi < literalFieldsType->fieldCount; fi++) {
                    const OwlFieldInfo* f = &literalFieldsType->fields[fi];
                    if (!f->name) continue;
                    const char* label =
                        (literalFieldsEN && f->nameEN && *f->nameEN)
                            ? f->nameEN : f->name;
                    /* filtre : field deja saisi (IDENT = ...) dans le brace ?
                     * Match sur la forme FR ou EN (l'utilisateur peut avoir
                     * saisi l'une ou l'autre). */
                    bool present = false;
                    int j = braceOpenOff + 1;
                    while (j < cursor_offset && !present) {
                        while (j < cursor_offset &&
                               (src[j] == ' ' || src[j] == '\t' ||
                                src[j] == '\n' || src[j] == ',')) j++;
                        int ws = j;
                        while (j < cursor_offset &&
                               ((src[j] >= 'a' && src[j] <= 'z') ||
                                (src[j] >= 'A' && src[j] <= 'Z') ||
                                (src[j] >= '0' && src[j] <= '9') ||
                                src[j] == '_' || (unsigned char)src[j] >= 0x80)) j++;
                        int we = j;
                        int wl = we - ws;
                        if (wl > 0 &&
                            ((wl == (int)strlen(f->name)
                              && strncasecmp(src + ws, f->name, (size_t)wl) == 0)
                             || (f->nameEN && wl == (int)strlen(f->nameEN)
                              && strncasecmp(src + ws, f->nameEN, (size_t)wl) == 0))) {
                            present = true;
                        }
                        if (we == ws) j++;
                        else {
                            while (j < cursor_offset &&
                                   src[j] != ',' && src[j] != '\n') j++;
                        }
                    }
                    if (present) continue;
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(label));
                    json_object_set(item, "kind", json_new_int(10)); /* Property */
                    const char* dType = (literalFieldsEN && f->typeEN && *f->typeEN)
                                            ? f->typeEN : f->type;
                    const char* dDesc = (literalFieldsEN && f->descriptionEN && *f->descriptionEN)
                                            ? f->descriptionEN : f->description;
                    char detailBuf[256];
                    snprintf(detailBuf, sizeof(detailBuf), "%s : %s",
                             dType ? dType : "?",
                             dDesc ? dDesc : "");
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    if (f->nameAscii && *f->nameAscii
                        && strcmp(f->nameAscii, label) != 0)
                        json_object_set(item, "filterText",
                                        json_new_string(f->nameAscii));
                    char insertBuf[80];
                    snprintf(insertBuf, sizeof(insertBuf), "%s = ", label);
                    json_object_set(item, "insertText", json_new_string(insertBuf));
                    json_object_set(item, "sortText", json_new_string("!"));
                    json_array_push(fieldItems, item);
                }
                json_free(items);
                return fieldItems;
            }

            /*  : type registry connu sans literalKeysEnum dans un
             * literal {} → pas de cles a proposer, completion vide (anti
             * pollution du catalogue general). */
            if (literalTypeKnownNoEnum && braceOpenOff >= 0) {
                json_free(items);
                return json_new_array();
            }

            /* Si on a un brace literal avec type cible, propose les fields
             * filtres (ceux non encore presents dans le brace courant). */
            if (braceTypeLen > 0 && braceOpenOff >= 0) {
                /* already_set indexé par l'index GLOBAL du catalogue (0..COUNT),
                 * pas par un compteur local : dimensionner à COUNT sinon
                 * over-read stack pour les types dont les champs sont au-delà
                 * de l'ancien plafond (les scalaires de `Style` étaient dropés
                 * car lus hors bornes → octets pile non nuls). */
                bool already_set[OWL_COMPOSED_TYPE_CATALOG_COUNT];
                memset(already_set, 0, sizeof(already_set));
                int braceContentStart = braceOpenOff + 1;
                /* Parse simpliste : pour chaque "IDENT =" dans le brace
                 * content (jusqu'au cursor), marque le field comme setté. */
                {
                    int j = braceContentStart;
                    while (j < cursor_offset) {
                        /* skip whitespace + virgules + newlines */
                        while (j < cursor_offset &&
                               (src[j] == ' ' || src[j] == '\t' ||
                                src[j] == '\n' || src[j] == ',')) j++;
                        if (j >= cursor_offset) break;
                        int ws = j;
                        while (j < cursor_offset &&
                               ((src[j] >= 'a' && src[j] <= 'z') ||
                                (src[j] >= 'A' && src[j] <= 'Z') ||
                                (src[j] >= '0' && src[j] <= '9') ||
                                src[j] == '_' || (unsigned char)src[j] >= 0x80)) j++;
                        int we = j;
                        if (we == ws) { j++; continue; }
                        while (j < cursor_offset && (src[j] == ' ' || src[j] == '\t')) j++;
                        if (j < cursor_offset && src[j] == '=') {
                            /* Field nommé : enregistre. */
                            for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                                if (strcasecmp(f->typeName, braceTypeBuf) == 0
                                    && composed_field_name_eq(f, src + ws,
                                                              we - ws, wantEN)) {
                                    already_set[k] = true;
                                    break;
                                }
                            }
                            /* skip jusqu'a la prochaine virgule ou newline ou
                             * fermeture brace (au top-level, pas dans nested). */
                            int braceDepth = 0;
                            j++;
                            while (j < cursor_offset) {
                                if (src[j] == '{') braceDepth++;
                                else if (src[j] == '}') {
                                    if (braceDepth == 0) break;
                                    braceDepth--;
                                } else if ((src[j] == ',' || src[j] == '\n')
                                           && braceDepth == 0) break;
                                j++;
                            }
                        }
                    }
                }

                /* Detecte si on est en RHS de field (`field = <cursor>`).
                 * Walk en *arriere* depuis cursor pour trouver le `=` le
                 * plus proche, puis le nom du field juste avant. Couvre
                 * les cas inline `{ a = 1, b = 2, c = <cursor> }`.
                 * - Si field est de type enum → propose les valeurs.
                 * - Si field est non-enum → ne PAS proposer la liste des
                 *   autres fields (Bug E : "Nom = " ne doit pas suggerer
                 *   Barré/Couleur/Gras). Retourne null. */
                bool rhsHandled = false;
                {
                    int j = cursor_offset - 1;
                    /* skip mot partiel (eventuellement vide) + espaces */
                    while (j >= 0 && ((src[j] >= 'a' && src[j] <= 'z') ||
                                      (src[j] >= 'A' && src[j] <= 'Z') ||
                                      (src[j] >= '0' && src[j] <= '9') ||
                                      src[j] == '_' || (unsigned char)src[j] >= 0x80)) j--;
                    while (j >= 0 && (src[j] == ' ' || src[j] == '\t')) j--;
                    bool hasEqual = (j >= 0 && src[j] == '=');
                    /* Eviter `==`, `<=`, `>=`, etc. */
                    if (hasEqual && j > 0) {
                        char prev = src[j-1];
                        if (prev == '=' || prev == '<' || prev == '>' ||
                            prev == '!' || prev == '+' || prev == '-' ||
                            prev == '*' || prev == '/') hasEqual = false;
                    }
                    int wordEndOff = 0, wordStartOff = 0;
                    int wlen = 0;
                    if (hasEqual) {
                        j--;  /* avant `=` */
                        while (j >= 0 && (src[j] == ' ' || src[j] == '\t')) j--;
                        wordEndOff = j + 1;
                        while (j >= 0 && ((src[j] >= 'a' && src[j] <= 'z') ||
                                          (src[j] >= 'A' && src[j] <= 'Z') ||
                                          (src[j] >= '0' && src[j] <= '9') ||
                                          src[j] == '_' || (unsigned char)src[j] >= 0x80)) j--;
                        wordStartOff = j + 1;
                        wlen = wordEndOff - wordStartOff;
                    }
                    if (hasEqual && wlen > 0 && wlen < 63) {
                        char fieldName[64];
                        memcpy(fieldName, src + wordStartOff, wlen);
                        fieldName[wlen] = 0;
                        for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                            const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                            if (strcasecmp(f->typeName, braceTypeBuf) != 0) continue;
                            if (!composed_field_name_eq(f, fieldName, wlen, wantEN))
                                continue;
                            rhsHandled = true;  /* on est sur un field connu */
                            if (strcmp(f->fieldType, "enum") == 0 && f->subTypeName) {
                                const OwlEnumInfo* ei = owl_enum_info_find(f->subTypeName);
                                if (ei && ei->values) {
                                    JsonValue* enumItems = json_new_array();
                                    for (const char* const* v = ei->values; *v; v++) {
                                        JsonValue* item = json_new_object();
                                        json_object_set(item, "label", json_new_string(*v));
                                        json_object_set(item, "kind", json_new_int(13));
                                        char detailBuf[256];
                                        snprintf(detailBuf, sizeof(detailBuf),
                                                 "Valeur de %s", ei->name);
                                        json_object_set(item, "detail", json_new_string(detailBuf));
                                        char sortBuf[200];
                                        snprintf(sortBuf, sizeof(sortBuf), "0_%.180s", *v);
                                        json_object_set(item, "sortText", json_new_string(sortBuf));
                                        json_array_push(enumItems, item);
                                    }
                                    json_free(items);
                                    return enumItems;
                                }
                            }
                            break;
                        }
                    }
                }
                /* RHS sur field non-enum (string/int/bool/instance) :
                 * pas de proposition pertinente → retourne null. */
                if (rhsHandled) {
                    json_free(items);
                    return json_new_null();
                }

                /* RHS de field non-enum OU pas de field detecte → propose
                 * les noms de fields filtres (non setes). */
                JsonValue* braceItems = json_new_array();
                for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                    const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                    if (strcasecmp(f->typeName, braceTypeBuf) != 0) continue;
                    /*  — slots internes préfixés `_` (états réifiés
                     * _Survol/_Focus/_Grise/_LectureSeule de styleSaisie, ou
                     * champs d'implémentation _handle/_ptr d'autres composites)
                     * hors surface publique : jamais proposés. */
                    if (f->fieldName && f->fieldName[0] == '_') continue;
                    if (already_set[k]) continue;
                    /*  — libellé dans la langue active (nameEN au
                     * catalogue). filterText combine les deux formes pour que
                     * la frappe FR matche encore un label EN affiché. */
                    const char* fLabel = composed_field_label(f, wantEN);
                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(fLabel));
                    json_object_set(item, "kind", json_new_int(5));  /* Field */
                    if (fLabel != f->fieldName) {
                        char ft[256];
                        snprintf(ft, sizeof(ft), "%s %s", fLabel, f->fieldName);
                        json_object_set(item, "filterText", json_new_string(ft));
                    }
                    char detailBuf[256];
                    if (f->subTypeName)
                        snprintf(detailBuf, sizeof(detailBuf), "%s : %s",
                                 f->fieldType, f->subTypeName);
                    else
                        snprintf(detailBuf, sizeof(detailBuf), "%s", f->fieldType);
                    json_object_set(item, "detail", json_new_string(detailBuf));
                    if (f->descFR)
                        json_object_set(item, "documentation", json_new_string(f->descFR));
                    /* Insert text avec le `= ` pour faciliter la frappe. */
                    char insertBuf[128];
                    snprintf(insertBuf, sizeof(insertBuf), "%s = ", fLabel);
                    json_object_set(item, "insertText", json_new_string(insertBuf));
                    char sortBuf[200];
                    snprintf(sortBuf, sizeof(sortBuf), "0_%.180s", fLabel);
                    json_object_set(item, "sortText", json_new_string(sortBuf));
                    json_array_push(braceItems, item);
                }
                json_free(items);
                return braceItems;
            }

            if (kindStr) {
                /* DSL prop value context : si la ligne courante matche
                 * 'propName \s* = \s* <cursor>', on resout la prop bare via
                 * dsl_prop_find(kind, prop) et on propose STRICTEMENT les
                 * valeurs selon le type de la prop :
                 *   - enum  -> valeurs de l'enum (ex: TypeBouton = TBValider)
                 *   - color -> constantes couleur (ex: CouleurFond = Rouge)
                 *   - bool  -> Vrai/Faux
                 *   - autre -> on tombe sur l'autocomplete des noms de props.
                 *
                 *  — valable dans le body du widget ET dans un bloc
                 * PROPRIETES. Sans ca, une prop bare COLOR (CouleurFond) sans
                 * collision avec un nom de type/enum retombait sur la liste
                 * des noms de props (AlignH/Marge/...).
                 *
                 * La detection de ligne est locale ; on ne distingue pas
                 * inProprietes car le pattern `prop = <cur>` est identique. */
                {
                    int lineStart = cursor_offset;
                    while (lineStart > 0 && src[lineStart-1] != '\n') lineStart--;
                    const char* lp = src + lineStart;
                    const char* lend = src + cursor_offset;
                    while (lp < lend && (*lp == ' ' || *lp == '\t')) lp++;
                    const char* wordStart = lp;
                    while (lp < lend && ((*lp >= 'a' && *lp <= 'z')
                                       || (*lp >= 'A' && *lp <= 'Z')
                                       || (*lp >= '0' && *lp <= '9')
                                       || *lp == '_' || (unsigned char)*lp >= 0x80)) lp++;
                    const char* wordEnd = lp;
                    while (lp < lend && (*lp == ' ' || *lp == '\t')) lp++;
                    bool hasEqual = (lp < lend && *lp == '=');
                    int wlen = (int)(wordEnd - wordStart);
                    if (hasEqual && wlen > 0 && wlen < 63) {
                        char propName[64];
                        memcpy(propName, wordStart, wlen);
                        propName[wlen] = 0;
                        const OwlDSLPropInfo* h = dsl_prop_find(ctx, propName);
                        /* Prop COLOR : propose strictement les constantes de
                         * categorie "Couleur" (label EN en mode EN). */
                        if (h && h->type && strcmp(h->type, "color") == 0) {
                            JsonValue* colorItems = json_new_array();
                            for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
                                const OwlConstInfo* c = &OWL_CONST_CATALOG[k];
                                if (!c->category
                                    || strcmp(c->category, "Couleur") != 0) continue;
                                const char* lbl = (wantEN && c->nameEN && *c->nameEN)
                                                  ? c->nameEN : c->name;
                                JsonValue* item = json_new_object();
                                json_object_set(item, "label", json_new_string(lbl));
                                json_object_set(item, "kind", json_new_int(21)); /* Constant */
                                if (lbl != c->name && strcmp(lbl, c->name) != 0) {
                                    char ftBuf[128];
                                    snprintf(ftBuf, sizeof(ftBuf), "%s %s", lbl, c->name);
                                    json_object_set(item, "filterText", json_new_string(ftBuf));
                                }
                                char detailBuf[256];
                                snprintf(detailBuf, sizeof(detailBuf), "[Couleur] %s",
                                         c->description ? c->description : "");
                                json_object_set(item, "detail", json_new_string(detailBuf));
                                char sortBuf[160];
                                snprintf(sortBuf, sizeof(sortBuf), "0_%.140s", lbl);
                                json_object_set(item, "sortText", json_new_string(sortBuf));
                                json_array_push(colorItems, item);
                            }
                            json_free(items);
                            return colorItems;
                        }
                        /* Prop booleen : Vrai / Faux uniquement. */
                        if (h && h->type && strcmp(h->type, "bool") == 0) {
                            JsonValue* boolItems = json_new_array();
                            static const char* const BOOLS[] = { "Vrai", "Faux" };
                            for (int b = 0; b < 2; b++) {
                                const char* blbl = bool_literal_label(BOOLS[b], wantEN);  /*  */
                                JsonValue* item = json_new_object();
                                json_object_set(item, "label", json_new_string(blbl));
                                json_object_set(item, "kind", json_new_int(21));
                                char sortBuf[16];
                                snprintf(sortBuf, sizeof(sortBuf), "0_%s", blbl);
                                json_object_set(item, "sortText", json_new_string(sortBuf));
                                json_array_push(boolItems, item);
                            }
                            json_free(items);
                            return boolItems;
                        }
                        if (h && h->enumName) {
                            const OwlEnumInfo* ei = owl_enum_info_find(h->enumName);
                            if (ei && ei->values) {
                                JsonValue* enumItems = json_new_array();
                                for (const char* const* v = ei->values; *v; v++) {
                                    /*  : en mode EN, emettre la forme
                                     * EN de la valeur (nameEN du membre, resolu
                                     * au const-catalog comme le hover ),
                                     * fallback FR si pas de nameEN. Pas de champ
                                     * valuesEN sur OwlEnum. */
                                    const char* vLabel = *v;
                                    if (wantEN) {
                                        for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
                                            if (strcasecmp(OWL_CONST_CATALOG[k].name, *v) == 0) {
                                                if (OWL_CONST_CATALOG[k].nameEN &&
                                                    *OWL_CONST_CATALOG[k].nameEN) {
                                                    vLabel = OWL_CONST_CATALOG[k].nameEN;
                                                }
                                                break;
                                            }
                                        }
                                    }
                                    JsonValue* item = json_new_object();
                                    json_object_set(item, "label", json_new_string(vLabel));
                                    json_object_set(item, "kind", json_new_int(13));
                                    /* filterText combine EN+FR si distinct pour
                                     * matcher le fuzzy peu importe la frappe. */
                                    if (vLabel != *v && strcmp(vLabel, *v) != 0) {
                                        char ftBuf[256];
                                        snprintf(ftBuf, sizeof(ftBuf), "%s %s", vLabel, *v);
                                        json_object_set(item, "filterText", json_new_string(ftBuf));
                                    }
                                    char detailBuf[256];
                                    snprintf(detailBuf, sizeof(detailBuf),
                                             "Valeur de %s", ei->name);
                                    json_object_set(item, "detail", json_new_string(detailBuf));
                                    char sortBuf[200];
                                    snprintf(sortBuf, sizeof(sortBuf), "0_%.180s", vLabel);
                                    json_object_set(item, "sortText", json_new_string(sortBuf));
                                    json_array_push(enumItems, item);
                                }
                                json_free(items);
                                return enumItems;
                            }
                        }
                        /*  — prop a valeur LIBRE (chaine/entier/...) :
                         * ni color, ni bool, ni enum. Le RHS accepte une
                         * reference de variable. On propose les globales
                         * top-level de la fenetre englobante (le runtime les
                         * resout via le mangling FEN_X$var, cf ), plus
                         * les globales de niveau fichier. Scope strict : pas les
                         * globales d'une AUTRE fenetre (faux candidat runtime),
                         * pas les vars locales d'un widget frere (isGlobal=Faux).
                         * Sans ca, `Contenu = <cursor>` retombait sur la liste
                         * des NOMS de props (X/Y/Largeur...) — absurde en RHS.
                         *
                         *  (support #376) — `h` est une prop widget
                         * connue en position VALEUR (`Prop = <cursor>`) sans jeu
                         * de valeurs (MargeBas/Libellé/ColSpan = entier/texte).
                         * On retourne TOUJOURS ce set (globales du scope, ou
                         * VIDE), meme sans candidat : sinon le fall-through
                         * dumpait soit le catalogue general complet (forme bare,
                         * hors filtre PROPRIETES-strict), soit les 38 autres
                         * noms de props (dans un bloc PROPRIETES) — deux listes
                         * "sans rapport" avec la valeur attendue. La position
                         * valeur d'une prop libre n'admet ni mot-cle/native ni
                         * nom de prop frere. */
                        if (h) {
                            int curWinStart =
                                enclosing_window_start(src, cursor_offset);
                            JsonValue* varItems = json_new_array();
                            for (int i = 0; i < userSymbolCount; i++) {
                                if (userSymbols[i].kind != 6) continue;
                                if (!userSymbols[i].isGlobal) continue;
                                if (userSymbols[i].docIndex != idx) continue;
                                /* Offset de la ligne de declaration -> fenetre
                                 * englobante de la globale. */
                                int symOff = 0, ln = 0;
                                const char* sp = src;
                                while (*sp && ln < userSymbols[i].line) {
                                    if (*sp == '\n') ln++;
                                    sp++; symOff++;
                                }
                                int symWin = enclosing_window_start(src, symOff);
                                /* Meme fenetre que le curseur, ou globale de
                                 * niveau fichier (symWin == -1). */
                                if (symWin != curWinStart && symWin != -1) continue;
                                JsonValue* item = json_new_object();
                                json_object_set(item, "label",
                                    json_new_string(userSymbols[i].name));
                                json_object_set(item, "kind", json_new_int(6));
                                json_object_set(item, "detail",
                                    json_new_string(userSymbols[i].detail));
                                char sortBuf[160];
                                snprintf(sortBuf, sizeof(sortBuf), "0_%.120s",
                                         userSymbols[i].name);
                                json_object_set(item, "sortText",
                                    json_new_string(sortBuf));
                                json_array_push(varItems, item);
                            }
                            /*  — return inconditionnel (cf. bloc plus
                             * haut) : le set peut etre vide, il ne doit JAMAIS
                             * retomber sur le dump general / noms de props. */
                            json_free(items);
                            return varItems;
                        }
                    }
                }

                int blockStart = find_dsl_block_start(src, cursor_offset, ctx);

                /*  — suite  : en contexte decl-assign booleen
                 * (`b est un booleen = <cur>`) DANS un bloc widget, on ne dumpe
                 * PAS les noms de props (AlignH/AlignV & co) par dessus les
                 * litteraux Vrai/Faux.  avait gate la seule contribution
                 * CONSTANTES (L7795) ; cette boucle-ci, sur OWL_DSL_PROP_CATALOG,
                 * est un chemin distinct qui ignorait le flag. Miroir exact :
                 * gate sur !boolLiteralOnly. La vraie position nom-de-prop (debut
                 * de ligne, pas un decl-assign) laisse boolLiteralOnly faux, donc
                 * les props y restent proposees. */
                for (int i = 0; !boolLiteralOnly && i < OWL_DSL_PROP_CATALOG_COUNT; i++) {
                    const OwlDSLPropInfo* h = &OWL_DSL_PROP_CATALOG[i];
                    if (strcmp(h->kind, kindStr) != 0) continue;

                    /*  (support #378) — ColSpan/RowSpan (fusion de
                     * cellules) n'ont d'effet que si le parent est en
                     * Disposition = dispGrille. Hors grille, ne pas les
                     * proposer en completion de nom de prop (ignores au
                     * rendu). Le catalogue porte le canon FR ; nameEN
                     * (ColumnSpan) suit via la meme entree. */
                    if (!dctx.parentIsGrille
                        && (strcmp(h->name, "ColSpan") == 0
                            || strcmp(h->name, "RowSpan") == 0)) {
                        continue;
                    }

                    /* Filtre : prop deja utilisee (FR ou ASCII ou EN). */
                    if (prop_already_used(src, cursor_offset, blockStart, h->name)) continue;
                    if (h->nameAscii && strcmp(h->nameAscii, h->name) != 0
                        && prop_already_used(src, cursor_offset, blockStart, h->nameAscii)) continue;
                    if (h->nameEN    && prop_already_used(src, cursor_offset, blockStart, h->nameEN))    continue;

                    /*  : en mode EN, servir la forme EN de la prop
                     * (nameEN), comme la completion des natives/const/type
                     *. Fallback FR si pas de nameEN distinct. Le
                     * label pilote l'affichage, filterText combine les deux
                     * formes pour que le fuzzy matche peu importe la frappe. */
                    bool hPropHasEN = wantEN && h->nameEN
                                      && strcmp(h->nameEN, h->name) != 0;
                    const char* propLabel = hPropHasEN ? h->nameEN : h->name;

                    JsonValue* item = json_new_object();
                    json_object_set(item, "label", json_new_string(propLabel));
                    json_object_set(item, "kind", json_new_int(10)); /* CompletionItemKind.Property */
                    /* sortText force le tri en tete (avant 'a' = priorite max). */
                    json_object_set(item, "sortText", json_new_string("!"));
                    /* filterText : combine label EN/FR + ASCII pour que le
                     * fuzzy matche les frappes accentuees comme non. */
                    {
                        char ft[256];
                        if (hPropHasEN) {
                            snprintf(ft, sizeof(ft), "%s %s", h->nameEN, h->name);
                            json_object_set(item, "filterText", json_new_string(ft));
                        } else if (h->nameAscii && strcmp(h->nameAscii, h->name) != 0) {
                            json_object_set(item, "filterText", json_new_string(h->nameAscii));
                        } else {
                            json_object_set(item, "filterText", json_new_string(h->name));
                        }
                    }
                    char detail[160];
                    snprintf(detail, sizeof(detail), "%s — DSL %s",
                             h->type ? h->type : "?", kindStr);
                    json_object_set(item, "detail", json_new_string(detail));
                    char insert[160];
                    snprintf(insert, sizeof(insert), "%s = $0", propLabel);
                    json_object_set(item, "insertText", json_new_string(insert));
                    json_object_set(item, "insertTextFormat", json_new_int(2)); /* snippet */
                    json_array_push(items, item);
                }
                /* DSL v2 : a l'interieur d'un bloc PROPRIETES, l'autocomplete
                 * doit etre STRICTEMENT borne aux props du widget. Le
                 * filtrage final (avant return items) supprime tout ce
                 * qui n'est pas kind=Property — voir bas de la fonction. */
            }
        }
    }

    /* ============================================================
     * DSL Fenetre v1 — completion des noms de Fenetres + Controls
     *
     * Permet de taper 'SAI_<cursor>' et voir 'SAI_User' propose.
     * Actif partout sauf contextes specialises (string, dot, type-decl,
     * etc. — ces blocs ont deja return). On peuple en parallele des
     * autres completions standards.
     * ============================================================ */
    if (!inStringContext && !singleDotContext && !doubleDotContext
        && !propValueContext && !typeDeclContext) {
        /* Cas general (hors arg0 d'une ouvre-fenetre, deja court-circuite plus
         * haut par un early-return fenetres-only, cf. ) : on propose
         * Fenetres ET Etats, les controles restant utiles pour taper
         * `SAI_<cursor>` n'importe ou. */
        for (int i = 0; i < userWindowCount; i++) {
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(userWindows[i].name));
            json_object_set(item, "kind", json_new_int(8)); /* CompletionItemKind.Class */
            json_object_set(item, "detail",
                            json_new_string(userWindows[i].isWindow
                                            ? "Fenetre OWL" : "Etat OWL"));
            json_object_set(item, "sortText", json_new_string("\""));  /* avant '!' = priorite max */
            json_array_push(items, item);
        }
        /*  — LHS scalaire en position valeur decl-assign : le
         * type-valeur d'un champ n'est pas capture au scan -> exclusion en bloc
         * des controles (comme procedures/mots-cles). LHS non-scalaire :
         * permissif inchange (un champ peut y etre une valeur legitime). */
        for (int i = 0; !lhsScalarDeclAssign && i < userControlCount; i++) {
            char detail[80];
            if (userControls[i].kind == 'M') {
                /*  — element de menu, pas un Champ. */
                snprintf(detail, sizeof(detail), "%s", "Element de menu");
            } else {
                const char* kindLabel =
                      userControls[i].kind == 'L' ? "Libelle"
                    : userControls[i].kind == 'B' ? "Bouton"
                    :                               "Saisie";
                snprintf(detail, sizeof(detail), "Champ %s", kindLabel);
            }
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(userControls[i].name));
            json_object_set(item, "kind", json_new_int(13)); /* CompletionItemKind.Variable */
            json_object_set(item, "detail", json_new_string(detail));
            json_object_set(item, "sortText", json_new_string("\""));
            json_array_push(items, item);
        }
        /*  — Groupes de champs declares. */
        for (int i = 0; i < userGroupCount; i++) {
            JsonValue* item = json_new_object();
            json_object_set(item, "label", json_new_string(userGroups[i].name));
            json_object_set(item, "kind", json_new_int(13)); /* Variable */
            char detail[80];
            snprintf(detail, sizeof(detail), "Groupe — %d membres",
                     userGroups[i].memberCount);
            json_object_set(item, "detail", json_new_string(detail));
            json_object_set(item, "sortText", json_new_string("\""));
            json_array_push(items, item);
        }
    }

    /* DSL v2 : si on est dans un bloc PROPRIETES, on filtre l'items pour
     * ne garder QUE les props (kind == 10). Le user a explicitement
     * demande de pas voir les fonctions natives ni autre dans cette zone
     * — c'est de la declaration pure. */
    if (td && pos) {
        const char* uri = json_get_string(td, "uri");
        int line = (int)json_get_int(pos, "line", 0);
        int character = (int)json_get_int(pos, "character", 0);
        int idx = find_doc(uri);
        if (idx >= 0) {
            const char* src = lspServer.documents[idx].content;
            int cursor_offset = 0;
            int curLine = 0;
            const char* p = src;
            while (*p && curLine < line) {
                if (*p == '\n') curLine++;
                p++;
                cursor_offset++;
            }
            cursor_offset += lsp_utf16_to_byte_in_line(p, character); /*  */
            DslCtx dctx = detect_dsl_context_ex(src, cursor_offset);
            if (dctx.inProprietes) {
                JsonValue* filtered = json_new_array();
                for (int i = 0; i < items->as.arrayVal.count; i++) {
                    JsonValue* it = items->as.arrayVal.items[i];
                    int kind = (int)json_get_int(it, "kind", 0);
                    if (kind == 10) {
                        /* Property — on garde. */
                        json_array_push(filtered, it);
                    }
                    /* Sinon : on libere pas (les items sont owned par
                     * items array qu'on va detruire entier). */
                }
                /* On detache les items conserves de l'ancien array pour
                 * pas qu'ils soient free deux fois. Strategy: copier les
                 * pointeurs, vider items, free items, retourner filtered. */
                /* Plus simple : remplacer items->as.arrayVal par filtered's. */
                /* Hack : transferer les items en duplicant l'array. */
                /* Simplest safe : detacher les items kept, free items,
                 * push them dans filtered. Mais arrayVal.items est owned.
                 * On va juste vider items directement, car json_free
                 * traverse l'array. Pour ne pas double-free, on doit
                 * NULL-out les slots transferes dans items->as.arrayVal. */
                /* Re-implementation propre : */
                for (int i = 0; i < items->as.arrayVal.count; i++) {
                    JsonValue* it = items->as.arrayVal.items[i];
                    int kind = (int)json_get_int(it, "kind", 0);
                    if (kind != 10) {
                        json_free(it);
                    }
                    items->as.arrayVal.items[i] = NULL;
                }
                free(items->as.arrayVal.items);
                free(items);
                items = filtered;
            }
        }
    }

    /* Optim perf : filtre server-side par fragment tape (avec
     * isIncomplete=true pose au wrap pour que VSCode re-query au prochain
     * keystroke). Sans ca on envoyait jusqu'a 1100 items au top-level
     * Ctrl+Espace -- payload JSON ~200KB, render VSCode sub-secondaire.
     * Aucun cap/troncature : on garde TOUS les items qui matchent. */
    int prefixLen = 0;
    char prefixAscii[64];
    prefixAscii[0] = '\0';
    if (items && items->type == JSON_ARRAY && td && pos) {
        const char* uri2 = json_get_string(td, "uri");
        int line2 = (int)json_get_int(pos, "line", 0);
        int char2 = (int)json_get_int(pos, "character", 0);
        int idx2 = find_doc(uri2);
        if (idx2 >= 0) {
            const char* src2 = lspServer.documents[idx2].content;
            const char* lp = src2;
            int curL = 0;
            while (*lp && curL < line2) { if (*lp == '\n') curL++; lp++; }
            char2 = lsp_utf16_to_byte_in_line(lp, char2); /*  */
            int c = char2 - 1;
            #define IS_IDF(x) (((x) >= 'a' && (x) <= 'z') || \
                               ((x) >= 'A' && (x) <= 'Z') || \
                               ((x) >= '0' && (x) <= '9') || \
                               (x) == '_' || (unsigned char)(x) >= 0x80)
            while (c >= 0 && IS_IDF(lp[c])) c--;
            int wStart = c + 1;
            int wlen = char2 - wStart;
            #undef IS_IDF
            if (wlen > 0 && wlen < 64) {
                prefixLen = scanner_strip_accents(lp + wStart, wlen,
                                                   prefixAscii, sizeof(prefixAscii));
            }
        }
    }
    /* Filtre par fragment (case+accent insensible) : "commence par" d'abord
     * PUIS "contient". Reduit ~1000 items a quelques uns des qu'un char est
     * tape. Ordre garanti via sortText (rang 0 = match exact, rang 1 = commence
     * par, rang 2 = contient seulement), l'index d'origine departageant
     * intra-bucket.
     * NB: ne s'active QUE quand >300 items pour ne jamais perturber les
     * contextes specialises (Assign Couleur, propValue, etc.) qui retournent
     * <200 items deja pertinents — VSCode filtre alors lui-meme.
     * Garde-fou volume : le repli "contient" ne s'active qu'a partir de 2
     * caracteres tapes ; a 1 char, on reste en prefixe pur. */
    if (prefixLen > 0 && items && items->type == JSON_ARRAY
        && items->as.arrayVal.count > 300) {
        JsonValue* filtered = json_new_array();
        for (int i = 0; i < items->as.arrayVal.count; i++) {
            JsonValue* it = items->as.arrayVal.items[i];
            const char* lbl = json_get_string(it, "label");
            /*  : 3 tiers. 0 = match exact (case+accent insensible),
             * 1 = commence par, 2 = contient seulement, -1 = jete. Le tier
             * exact evite qu'un mot-cle/natif dont le label EGALE le fragment
             * tape (ex. `end` -> END) soit enterre derriere ses homonymes
             * (EndsWith, EndProgram...) qui partagent le tier "commence par". */
            int rank = -1;
            char lblAscii[128];
            int laLen = 0;
            if (lbl) {
                laLen = scanner_strip_accents(lbl, (int)strlen(lbl),
                                              lblAscii, sizeof(lblAscii));
                if (laLen == prefixLen
                    && strncasecmp(lblAscii, prefixAscii, (size_t)prefixLen) == 0) {
                    rank = 0; /* match exact : priorite maximale */
                } else if (laLen >= prefixLen
                    && strncasecmp(lblAscii, prefixAscii, (size_t)prefixLen) == 0) {
                    rank = 1; /* prefixe : priorite haute */
                } else if (prefixLen >= 2 && lsp_ci_contains(lblAscii, prefixAscii, prefixLen)) {
                    rank = 2; /* contient (label desaccentue) : priorite basse */
                }
            }
            /*  — le filtre ne regardait que le label. Consulter aussi le
             * filterText (formes bilingues/ascii separees par espace, ex.
             * "Police Police Font") : sans ca, taper la forme EN `Font` ne
             * surface jamais le type FR-labelise `Police` dans le pool general
             * (>300), l'item etant jete cote serveur avant que VSCode ne filtre
             * sur le filterText. Token-prefix uniquement (pas de "contient"
             * large) pour eviter le sur-matching. Ameliore le rang, jamais ne le
             * degrade. */
            if (rank != 0) {
                const char* ft = json_get_string(it, "filterText");
                if (ft) {
                    char ftAscii[256];
                    scanner_strip_accents(ft, (int)strlen(ft), ftAscii, sizeof(ftAscii));
                    for (const char* p = ftAscii; *p; ) {
                        if (strncasecmp(p, prefixAscii, (size_t)prefixLen) == 0) {
                            if (rank < 0 || rank > 1) rank = 1;
                            break;
                        }
                        while (*p && *p != ' ') p++;  /* saute le token courant */
                        while (*p == ' ') p++;         /* et les espaces */
                    }
                }
            }
            if (rank >= 0) {
                /* rang domine (0 < 1), index d'origine departage pour
                 * conserver l'ordre d'insertion a l'interieur du bucket. */
                char sortBuf[16];
                snprintf(sortBuf, sizeof(sortBuf), "%d%06d", rank, i);
                json_object_set(it, "sortText", json_new_string(sortBuf));
                /*  — match EXACT (rang 0 : le label desaccentue EGALE le
                 * fragment tape, ex. `end` -> END). Le sortText serveur classe
                 * deja l'exact en tete, mais VSCode en popup AUTO (pendant la
                 * frappe) applique son propre score fuzzy et peut focus un
                 * homonyme prefixe (EndsWith, EndProgram) au lieu de l'exact,
                 * de sorte qu'une validation directe insere le mauvais mot.
                 * preselect force l'item exact comme selection par defaut cote
                 * client -> taper `end` puis valider insere END. */
                if (rank == 0) {
                    json_object_set(it, "preselect", json_new_bool(true));
                    /*  (support #278) — VSCode en popup auto score la
                     * requete contre le filterText avec un BONUS de casse
                     * exacte. Avec le filterText desaccentue canonique majuscule
                     * ("LOOP"), taper `loop` (minuscules) ne matche aucun char
                     * en casse exacte, alors que le label d'une native prefixee
                     * ("LooperAddLine") matche "oop" en casse exacte -> les
                     * Looper* gagnent le bonus et restent devant. Sur un match
                     * EXACT (rang 0), on pose le filterText = fragment REELLEMENT
                     * TAPE (casse preservee, prefixAscii) : VSCode voit un
                     * full-match casse-exacte (`loop` vs "loop") -> score maximal
                     * -> le mot-cle exact remonte en tete ET devient l'item
                     * selectionne. */
                    if (prefixLen > 0)
                        json_object_set(it, "filterText",
                                        json_new_string(prefixAscii));
                }
                json_array_push(filtered, it);
            } else {
                json_free(it);
            }
            items->as.arrayVal.items[i] = NULL;
        }
        free(items->as.arrayVal.items);
        free(items);
        items = filtered;
    }
    /* Note perf : aucun cap/troncature ici — on garde TOUS les items.
     * Le signal isIncomplete=true est pose une seule fois par lsp_server.c
     * au wrap du CompletionList (VSCode re-query au prochain keystroke,
     * ce qui avec le filtre prefix (a) reduit drastiquement des 1 char).
     * handle_completion retourne TOUJOURS un tableau nu (ou null si vide) :
     * wrapper ici produirait un double-nesting result.items={items:[...]}
     * -> "No Suggestions" cote VSCode. */

    /* Bug #8 : VSCode affiche "Loading..." sur tableau vide. Retourner
     * null fait disparaitre la popup proprement. */
    if (items && items->type == JSON_ARRAY && items->as.arrayVal.count == 0) {
        json_free(items);
        return json_new_null();
    }
    return items;
}

/* ================================================================
 * HOVER -- rich info from catalog
 * ================================================================ */

/*  — field d'un OwlTypeInfo par nom (FR/ascii/EN), NULL sinon. */
static const OwlFieldInfo* owl_type_field_find(const OwlTypeInfo* ti,
                                               const char* name) {
    if (!ti || !name) return NULL;
    for (int i = 0; i < ti->fieldCount; i++) {
        const OwlFieldInfo* f = &ti->fields[i];
        if ((f->name      && strcasecmp(name, f->name)      == 0)
         || (f->nameAscii && strcasecmp(name, f->nameAscii) == 0)
         || (f->nameEN    && strcasecmp(name, f->nameEN)    == 0))
            return f;
    }
    return NULL;
}

/*  — construit un hover Markdown JSON pour un mot survolé qui vit dans
 * la chaîne `Style..<surface>..<feuille>` d'un bloc menu (MenuPrincipal /
 * MenuContextuel). Retourne NULL si le contexte n'est pas un bloc menu ou si la
 * chaîne ne démarre pas par `Style` / ne résout pas — le caller retombe alors
 * sur la résolution générique (native, var user, ...).
 *
 * Root = styleMenuPrincipal / styleMenuContextuel (les blocs menu sont
 * is_widget=false, donc pas de prop `Style` cataloguée : mapping local). Walk
 * multi-hop borné au chemin menu, réutilisant OWL_TYPE_CATALOG pour chaque
 * saut. Gère l'index d'état `Etats[survol]` (descente vers le sous-type Base). */
static JsonValue* menu_style_chain_hover(const char* src, int line,
                                         int character, const char* word,
                                         bool wantEN) {
    if (!src || !word) return NULL;

    /* Contexte DSL au curseur : doit être un bloc menu porteur de `Style`. */
    int cursor_offset = 0, cl = 0;
    const char* pp = src;
    while (*pp && cl < line) { if (*pp == '\n') cl++; pp++; cursor_offset++; }
    cursor_offset += character;
    char menuChar = detect_dsl_context(src, cursor_offset);
    const char* rootType = menu_kind_style_type(menuChar);
    if (!rootType) return NULL;

    /* Début de la ligne courante. */
    const char* lp = src;
    int curLine = 0;
    while (*lp && curLine < line) { if (*lp == '\n') curLine++; lp++; }

    #define IS_ID_1214(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || \
                           ((x) >= '0' && (x) <= '9') || (x) == '_' || (unsigned char)(x) >= 0x80)
    /* Début du mot survolé sur la ligne. */
    int wc = character - 1;
    while (wc >= 0 && IS_ID_1214(lp[wc])) wc--;
    int wordStart = wc + 1;

    /* Collecte des segments `..seg..seg..` en arrière depuis le mot survolé.
     * segsRev[0] = mot survolé ; on remonte jusqu'à `Style`. `hadBracket`
     * marque un index d'état `[survol]` attaché au segment (ex `Etats[...]`). */
    struct { char name[64]; bool hadBracket; } segsRev[8];
    int nseg = 0;
    /* Segment 0 = le mot survolé lui-même. */
    snprintf(segsRev[nseg].name, sizeof(segsRev[nseg].name), "%s", word);
    segsRev[nseg].hadBracket = false;
    nseg++;

    int pos = wordStart - 1;
    while (nseg < 8 && pos >= 1 && lp[pos] == '.' && lp[pos-1] == '.') {
        pos -= 2;
        bool hadBracket = false;
        if (pos >= 0 && lp[pos] == ']') {
            while (pos >= 0 && lp[pos] != '[') pos--;
            if (pos < 0) break;   /* index malformé */
            pos--;                /* passe avant '[' */
            hadBracket = true;
        }
        int end = pos + 1;
        while (pos >= 0 && IS_ID_1214(lp[pos])) pos--;
        int start = pos + 1;
        if (end <= start || end - start >= 64) break;
        memcpy(segsRev[nseg].name, lp + start, (size_t)(end - start));
        segsRev[nseg].name[end - start] = '\0';
        segsRev[nseg].hadBracket = hadBracket;
        nseg++;
        pos = start - 1;
    }
    #undef IS_ID_1214

    /* Ordre avant : segs[0] = `Style` racine ... segs[nseg-1] = mot survolé. */
    if (nseg < 1 || nseg > 8) return NULL;
    if (strcasecmp(segsRev[nseg-1].name, "Style") != 0) return NULL;

    /* typeAt[i] = type composé résolu après le segment i (typeAt[0] = root). */
    const char* typeAt[8];
    typeAt[0] = rootType;
    for (int i = 1; i < nseg; i++) {
        /* segment forward i correspond à segsRev[nseg-1-i]. */
        const char* segName = segsRev[nseg-1-i].name;
        bool bracket = segsRev[nseg-1-i].hadBracket;
        const OwlTypeInfo* parentTi = owl_type_info_find(typeAt[i-1]);
        const OwlFieldInfo* f = owl_type_field_find(parentTi, segName);
        if (!f || !f->type) return NULL;
        const char* nt = f->type;
        if (bracket) {
            /* `..Etats[survol]..` : descente vers le sous-type plat Base
             * (styleXxxEtats → styleXxxBase). */
            size_t ln = strlen(nt);
            if (ln > 5 && strcmp(nt + ln - 5, "Etats") == 0) {
                static char baseBuf[80];
                if (ln - 5 + 4 < sizeof(baseBuf)) {
                    memcpy(baseBuf, nt, ln - 5);
                    memcpy(baseBuf + ln - 5, "Base", 5);
                    nt = baseBuf;
                }
            }
        }
        typeAt[i] = nt;
    }

    JsonValue* hover = json_new_object();
    JsonValue* contents = json_new_object();
    char md[1600];

    if (nseg == 1) {
        /* Racine `Style` → le type de style typé du menu. */
        const OwlTypeInfo* ti = owl_type_info_find(rootType);
        const char* title = (wantEN && ti && ti->nameEN && *ti->nameEN)
                            ? ti->nameEN : rootType;
        const char* desc = ti
            ? ((wantEN && ti->descriptionEN && *ti->descriptionEN)
               ? ti->descriptionEN : (ti->description ? ti->description : ""))
            : "";
        const char* kindLabel = (menuChar == 'x')
            ? (wantEN ? "ContextMenu" : "MenuContextuel")
            : (wantEN ? "MainMenu"    : "MenuPrincipal");
        snprintf(md, sizeof(md),
            "```owl\nStyle : <%s>\n```\n\n%s\n\n---\n*[%s %s]*",
            title, desc, wantEN ? "Style of" : "Style de", kindLabel);
    } else {
        /* Feuille / surface : field du type parent (typeAt[nseg-2]). */
        const char* parentType = typeAt[nseg-2];
        const OwlTypeInfo* parentTi = owl_type_info_find(parentType);
        const OwlFieldInfo* f = owl_type_field_find(parentTi, word);
        if (!f) { json_free(hover); json_free(contents); return NULL; }
        bool fHasEN = (f->nameEN && *f->nameEN && strcmp(f->name, f->nameEN) != 0);
        const char* title = (wantEN && fHasEN) ? f->nameEN : f->name;
        const char* ftype = (wantEN && f->typeEN && *f->typeEN)
                            ? f->typeEN : (f->type ? f->type : "?");
        const char* fdesc =
            (wantEN && f->descriptionEN && *f->descriptionEN) ? f->descriptionEN
            : (f->description ? f->description : "");
        snprintf(md, sizeof(md),
            "```owl\n%s : <%s>\n```\n\n%s\n\n---\n*[%s %s]*",
            title, ftype, fdesc, wantEN ? "Field of" : "Champ de", parentType);
    }

    json_object_set(contents, "kind", json_new_string("markdown"));
    json_object_set(contents, "value", json_new_string(md));
    json_object_set(hover, "contents", contents);
    return hover;
}

JsonValue* handle_hover(JsonValue* params) {
    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (!td || !pos) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);

    int idx = find_doc(uri);
    if (idx < 0) return json_new_null();

    /* Scan user symbols pour la priorite var-user sur catalogs natifs.
     * Idempotent : protege par fingerprint si rien n'a change. */
    scan_user_symbols();

    /*  — hover full English quand le client a initialise
     * language=en (calque le wantEN de la complétion). Chaque sous-cas sert
     * le champ EN du catalogue s'il existe, avec fallback FR sinon (jamais
     * de vide). Le mode FR (defaut) reste inchangé. */
    bool wantEN = (strcmp(lspServer.language, "en") == 0);

    char word[256];
    if (!extract_word_at(lspServer.documents[idx].content,
                         lspServer.documents[idx].contentLen,
                         line, character, word, sizeof(word)))
        return json_new_null();

    /*  — context `icones.<id>` : early-exit avant tout lookup
     * global. Sans ce check, le hover sur `icones.Delete` retournait la
     * doc de la native EN homonyme `Supprime/Delete` (au lieu de l'info
     * icone). Detection : single-dot avant le word, et le mot a gauche
     * du `.` est `icones` (strip accent / casse-insensible). On lookup
     * dans le catalog ; si hit, on renvoie un Hover Markdown avec nom
     * canon + categorie OWL. Si miss (id inconnu), on renvoie null
     * (volontaire : l'utilisateur a tape `icones.X` invalide, le hover
     * de la native homonyme n'a pas sa place ici). */
    {
        const char* src = lspServer.documents[idx].content;
        int curLine = 0;
        const char* lp = src;
        while (*lp && curLine < line) {
            if (*lp == '\n') curLine++;
            lp++;
        }
        int c = character - 1;
        #define IS_ID_F383(x) (((x) >= 'a' && (x) <= 'z') || \
                               ((x) >= 'A' && (x) <= 'Z') || \
                               ((x) >= '0' && (x) <= '9') || \
                               (x) == '_' || (unsigned char)(x) >= 0x80)
        while (c >= 0 && IS_ID_F383(lp[c])) c--;
        /* Single-dot precedent (pas `..`), puis IDENT a gauche */
        if (c >= 0 && lp[c] == '.' && (c == 0 || lp[c-1] != '.')) {
            int rc = c - 1;
            int rEnd = c;
            while (rc >= 0 && IS_ID_F383(lp[rc])) rc--;
            int rStart = rc + 1;
            int rLen = rEnd - rStart;
            if (rLen > 0 && rLen < 32 &&
                is_icones_receiver_name(lp + rStart, rLen)) {
                /* Context `icones.<word>` confirme. Lookup catalog.
                 * owl_icones_catalog_find (stdlib_icones.c) n'est pas
                 * link cote owl-lsp → scan lineaire local sur
                 * OWL_ICONS_CATALOG (47k entries, ~5ms, OK pour un
                 * hover one-time). Translate `_→-` pour matcher la
                 * forme stockee (cf. ). */
                char lookup[128];
                {
                    int wlen = (int)strlen(word);
                    if (wlen >= (int)sizeof(lookup)) wlen = (int)sizeof(lookup) - 1;
                    for (int i = 0; i < wlen; ++i) {
                        char c = word[i];
                        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                        if (c == '_') c = '-';
                        lookup[i] = c;
                    }
                    lookup[wlen] = '\0';
                }
                const OwlIconCatalogEntry* entry = NULL;
                for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; ++i) {
                    if (strcmp(OWL_ICONS_CATALOG[i].name, lookup) == 0) {
                        entry = &OWL_ICONS_CATALOG[i];
                        break;
                    }
                }
                if (entry != NULL) {
                    /* Build hover markdown : nom canon + categorie OWL +
                     * lien CTA ouvrir catalog. Pas de mention set
                     * Iconify (regle 7).
                     *
                     *  (pivot post decoration approach) :
                     * lien `command:owl.openIconBrowser?<id>` cliquable
                     * dans le hover. Necessite `isTrusted=true` cote
                     * MarkdownString, gere par middleware hover du
                     * plugin TS (extension.js). */
                    const char* cat = lsp_set_id_to_owl_category(entry->set_id);
                    char md[768];
                    snprintf(md, sizeof(md),
                        "**icones.%s**\n\n"
                        "categorie : `%s`\n\n"
                        "Icone du catalogue embarque OWL.\n\n"
                        "[Ouvrir dans le catalogue]"
                        "(command:owl.openIconBrowser?%%5B%%22%s%%22%%5D)",
                        word, cat, word);
                    JsonValue* hover = json_new_object();
                    JsonValue* contents = json_new_object();
                    json_object_set(contents, "kind",
                                    json_new_string("markdown"));
                    json_object_set(contents, "value", json_new_string(md));
                    json_object_set(hover, "contents", contents);
                    return hover;
                }
                /* Miss : ne pas tomber sur la doc native homonyme.
                 * Renvoie null = pas de hover affiche. */
                return json_new_null();
            }
        }
        #undef IS_ID_F383
    }

    /*  — hover sur un event DSL (surClic, FinInitialisation, …).
     * Doit primer sur la resolution var-user : `PROCEDURE surClic()` est
     * declaree comme procedure projet, mais le hover utile est la fiche de
     * l'event canon (signature + desc bilingue). Sert l'EN quand wantEN, FR
     * sinon, avec fallback (jamais de vide). Branche placee AVANT le scan
     * method/native/var-user pour avoir la priorite. */
    {
        const OwlEventDef* ev = lsp_find_event_def_any(word);
        /*  — desambiguisation contextuelle de la collision
         * Fermeture/Closing. Cet event est declare a la fois dans le
         * catalogue Etat (desc rapport) ET dans le lifecycle Fenetre (desc
         * fenetre), avec des descs divergentes. lsp_find_event_def_any sert
         * l'Etat (ordre de recherche), mais le hover doit suivre le bloc DSL
         * englobant : contexte Etat-family -> desc rapport ; sinon (Fenetre,
         * widget, ou hors-bloc) -> desc fenetre. Le defaut hors-bloc = fenetre
         * couvre le cas dominant (code colle au top-level d'un main.owl).
         * Seul event a collision inter-catalogues a desc divergente. */
        if (ev) {
            const OwlEventDef* evEtat = owl_etat_event_catalog_find(word);
            const OwlEventDef* evWin  = owl_window_lifecycle_catalog_find(word);
            if (evEtat && evWin) {
                const char* hsrc = lspServer.documents[idx].content;
                int cursor_offset = 0, cl = 0;
                for (const char* pp = hsrc; *pp && cl < line; pp++) {
                    if (*pp == '\n') cl++;
                    cursor_offset++;
                }
                cursor_offset += character;
                char wctx = detect_dsl_context_ex(hsrc, cursor_offset).widget;
                bool etatFamily = (wctx == 'E' || wctx == 'e' || wctx == 'l'
                                   || wctx == 'm' || wctx == 'd' || wctx == 'C');
                /*  — Fermeture au top-level d'un document projet.owl
                 * (hors bloc widget/etat) designe l'event de teardown du
                 * PROJET, avec sa propre desc. Prime sur la desambiguisation
                 * Etat/Fenetre. */
                bool inBlockCtx = (dsl_kind_char_to_control_kind(wctx) >= 0)
                                  || wctx == 'F' || etatFamily;
                const OwlEventDef* evProjet = projet_lifecycle_find(word);
                if (evProjet && !inBlockCtx && source_declares_projet(hsrc))
                    ev = evProjet;
                else
                    ev = etatFamily ? evEtat : evWin;
            }
        }
        if (ev) {
            bool evHasEN = (ev->nameEN && *ev->nameEN &&
                            strcmp(ev->canonical, ev->nameEN) != 0);
            const char* title = (wantEN && evHasEN) ? ev->nameEN : ev->canonical;
            /* Corps dans la langue active uniquement (pas de double-langue,
             * cf. ticket  : titre + desc selon wantEN). Fallback FR
             * si descEN vide en mode EN. */
            const char* descPrimary =
                (wantEN && ev->descEN && *ev->descEN) ? ev->descEN
                : (ev->descFR ? ev->descFR : "");

            char md[2048];
            int off = snprintf(md, sizeof(md), "**`PROCEDURE %s", title);
            for (int p = 0; p < ev->paramCount && off < (int)sizeof(md) - 64; p++) {
                /*  — type du param suit la langue. Le nom du param et
                 * sa description restent FR (content gap, hors-scope). */
                const char* ptype = wantEN
                    ? lsp_primitive_type_en(ev->params[p].typeName)
                    : ev->params[p].typeName;
                off += snprintf(md + off, sizeof(md) - off,
                    "%s%s est un %s",
                    p == 0 ? "(" : ", ",
                    ev->params[p].name,
                    ptype);
            }
            off += snprintf(md + off, sizeof(md) - off,
                "%s`**\n\n%s",
                ev->paramCount > 0 ? ")" : "()",
                descPrimary);
            for (int p = 0; p < ev->paramCount && off < (int)sizeof(md) - 200; p++) {
                const char* pdesc =
                    (wantEN && ev->params[p].descEN && *ev->params[p].descEN)
                    ? ev->params[p].descEN
                    : (ev->params[p].descFR ? ev->params[p].descFR : "");
                const char* ptype2 = wantEN
                    ? lsp_primitive_type_en(ev->params[p].typeName)
                    : ev->params[p].typeName;
                off += snprintf(md + off, sizeof(md) - off,
                    "\n\n- **`%s`** (`%s`) : %s",
                    ev->params[p].name, ptype2, pdesc);
            }
            JsonValue* hover = json_new_object();
            JsonValue* contents = json_new_object();
            json_object_set(contents, "kind", json_new_string("markdown"));
            json_object_set(contents, "value", json_new_string(md));
            json_object_set(hover, "contents", contents);
            return hover;
        }
    }

    /*  — hover sur un champ du bloc Projet (Langues/Description/
     * Auteur/Licence). Le type Projet est composé (pas un widget), donc
     * dsl_prop_find ne le couvre pas : on résout via le catalogue de type
     * (owl_type_info_find). Parité  : titre canon + desc bilingue,
     * placée AVANT le scan native/var-user. */
    {
        const char* hsrc = lspServer.documents[idx].content;
        int cursor_offset = 0, cl = 0;
        const char* pp = hsrc;
        while (*pp && cl < line) { if (*pp == '\n') cl++; pp++; cursor_offset++; }
        cursor_offset += character;
        if (detect_dsl_context(hsrc, cursor_offset) == 'J') {
            const OwlTypeInfo* projet = owl_type_info_find("Projet");
            if (projet) {
                const OwlFieldInfo* f = NULL;
                for (int i = 0; i < projet->fieldCount; i++) {
                    const OwlFieldInfo* c = &projet->fields[i];
                    if ((c->name      && strcasecmp(word, c->name)      == 0)
                     || (c->nameAscii && strcasecmp(word, c->nameAscii) == 0)
                     || (c->nameEN    && strcasecmp(word, c->nameEN)    == 0)) {
                        f = c; break;
                    }
                }
                if (f) {
                    bool fHasEN = (f->nameEN && *f->nameEN
                                   && strcmp(f->name, f->nameEN) != 0);
                    const char* title = (wantEN && fHasEN) ? f->nameEN : f->name;
                    const char* ftype = (wantEN && f->typeEN) ? f->typeEN
                                        : (f->type ? f->type : "?");
                    const char* fdesc =
                        (wantEN && f->descriptionEN && *f->descriptionEN) ? f->descriptionEN
                        : (f->description ? f->description : "");
                    char md[1024];
                    snprintf(md, sizeof(md),
                        "```owl\n%s = <%s>\n```\n\n%s\n\n---\n*[%s Projet]*",
                        title, ftype, fdesc, wantEN ? "Field of" : "Champ de");
                    JsonValue* hover = json_new_object();
                    JsonValue* contents = json_new_object();
                    json_object_set(contents, "kind", json_new_string("markdown"));
                    json_object_set(contents, "value", json_new_string(md));
                    json_object_set(hover, "contents", contents);
                    return hover;
                }
            }
        }
    }

    /*  — hover sur une propriete DSL (Titre/Title, Largeur/Width, …)
     * a l'interieur d'un body widget. Source unique : OWL_DSL_PROP_CATALOG
     * (nameEN/descEN deja generes). Sert le titre canon + la desc dans la
     * langue active (descEN si wantEN, fallback FR). Placee AVANT le scan
     * native/var-user : une prop comme `Largeur` ne doit pas tomber sur une
     * native homonyme. Bornee au contexte DSL : hors body widget, on laisse
     * passer (detect_dsl_context renvoie 0 -> pas de hover prop). */
    {
        const char* hsrc = lspServer.documents[idx].content;
        int cursor_offset = 0;
        int cl = 0;
        const char* pp = hsrc;
        while (*pp && cl < line) { if (*pp == '\n') cl++; pp++; cursor_offset++; }
        cursor_offset += character;
        char widget = detect_dsl_context(hsrc, cursor_offset);
        if (widget) {
            const OwlDSLPropInfo* prop = dsl_prop_find(widget, word);
            if (prop) {
                bool pHasEN = (prop->nameEN && *prop->nameEN
                               && strcmp(prop->name, prop->nameEN) != 0);
                const char* title = (wantEN && pHasEN) ? prop->nameEN : prop->name;
                const char* pdesc =
                    (wantEN && prop->descEN && *prop->descEN) ? prop->descEN
                    : (prop->descFR ? prop->descFR : "");
                char md[1024];
                snprintf(md, sizeof(md),
                    "```owl\n%s = <%s>\n```\n\n%s\n\n---\n*[%s %s]*",
                    title, prop->type ? prop->type : "?", pdesc,
                    wantEN ? "Property of" : "Propriete de", prop->kind);
                JsonValue* hover = json_new_object();
                JsonValue* contents = json_new_object();
                json_object_set(contents, "kind", json_new_string("markdown"));
                json_object_set(contents, "value", json_new_string(md));
                json_object_set(hover, "contents", contents);
                return hover;
            }
        }
    }

    /*  — hover menu-aware : chaîne `Style..<surface>..<feuille>` dans
     * un bloc MenuPrincipal/MenuContextuel. Résout sur le type de style typé du
     * menu et ses sous-surfaces, PAS sur le styleConteneur de la fenetre ni sur
     * une native homonyme (Arrondi). Placé AVANT le scan native/var-user pour
     * primer sur `Arrondi()` & co. NULL si hors chaîne menu → passe la main. */
    {
        JsonValue* mh = menu_style_chain_hover(lspServer.documents[idx].content,
                                               line, character, word, wantEN);
        if (mh) return mh;
    }

    /* Check if the word is preceded by a '.' → method call context.
     * In that case, extension methods take priority over keywords/natives.
     * Aussi : detecter le chain `..ParentType..word` pour resoudre word
     * comme field d'un type composé (Cadre/Style/Police/...). */
    bool isMethodContext = false;
    char chainParentType[64] = "";
    {
        const char* src = lspServer.documents[idx].content;
        int curLine = 0;
        const char* lp = src;
        while (*lp && curLine < line) {
            if (*lp == '\n') curLine++;
            lp++;
        }
        /* Walk back from cursor to start of current word, then check char */
        int c = character - 1;
        #define IS_ID2(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || \
                          ((x) >= '0' && (x) <= '9') || (x) == '_' || (unsigned char)(x) >= 0x80)
        while (c >= 0 && IS_ID2(lp[c])) c--;
        if (c >= 0 && lp[c] == '.' && (c == 0 || lp[c-1] != '.')) {
            isMethodContext = true;
        }
        /* Chain `..ParentType..word` : 2 dots avant word, puis ident, puis 2 dots. */
        if (c >= 1 && lp[c] == '.' && lp[c-1] == '.') {
            int pc = c - 2;
            int pEnd = pc + 1;
            while (pc >= 0 && IS_ID2(lp[pc])) pc--;
            int pStart = pc + 1;
            if (pEnd > pStart && pEnd - pStart < 64) {
                /* Verifie que c'est bien un type composé */
                char candidate[64];
                memcpy(candidate, lp + pStart, pEnd - pStart);
                candidate[pEnd - pStart] = '\0';
                for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                    if (strcasecmp(OWL_COMPOSED_TYPE_CATALOG[k].typeName,
                                   candidate) == 0) {
                        strncpy(chainParentType, OWL_COMPOSED_TYPE_CATALOG[k].typeName,
                                sizeof(chainParentType) - 1);
                        break;
                    }
                }
            }
        }
        #undef IS_ID2

        /* Hover dans un brace literal `{ field = ... }` : on detecte qu'on
         * est entre `{` et `}`, on remonte au LHS du `=` qui precede le `{`
         * pour deduire le type cible (Cadre/Style/...) puis on verifie que
         * `word` est un field connu. */
        if (chainParentType[0] == '\0') {
            int cursor_offset = 0;
            int cl = 0;
            const char* pp = src;
            while (*pp && cl < line) { if (*pp == '\n') cl++; pp++; cursor_offset++; }
            cursor_offset += character;
            int braceOpenOff = -1;
            int depth = 0;
            bool inStr2 = false;
            for (int i = cursor_offset - 1; i >= 0; i--) {
                char ch = src[i];
                if (ch == '"' && (i == 0 || src[i-1] != '\\')) {
                    inStr2 = !inStr2; continue;
                }
                if (inStr2) continue;
                if (ch == '}') depth++;
                else if (ch == '{') {
                    if (depth == 0) { braceOpenOff = i; break; }
                    depth--;
                }
            }
            if (braceOpenOff >= 0) {
                int j = braceOpenOff - 1;
                while (j >= 0 && (src[j] == ' ' || src[j] == '\t' || src[j] == '\n')) j--;
                if (j >= 0 && src[j] == '=') {
                    j--;
                    while (j >= 0 && (src[j] == ' ' || src[j] == '\t')) j--;
                    int identEnd = j + 1;
                    #define IS_ID3(x) (((x) >= 'a' && (x) <= 'z') || ((x) >= 'A' && (x) <= 'Z') || \
                                       ((x) >= '0' && (x) <= '9') || (x) == '_' || (unsigned char)(x) >= 0x80)
                    while (j >= 0 && IS_ID3(src[j])) j--;
                    int identStart = j + 1;
                    #undef IS_ID3
                    if (identEnd > identStart && identEnd - identStart < 64) {
                        char lhsName[64];
                        int lhsLen = identEnd - identStart;
                        memcpy(lhsName, src + identStart, lhsLen);
                        lhsName[lhsLen] = '\0';
                        for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                            const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                            if (strcasecmp(f->typeName, lhsName) == 0) {
                                strncpy(chainParentType, f->typeName,
                                        sizeof(chainParentType) - 1);
                                break;
                            }
                        }
                        if (chainParentType[0] == '\0') {
                            for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
                                const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
                                if (strcasecmp(f->fieldName, lhsName) == 0
                                    && f->subTypeName) {
                                    strncpy(chainParentType, f->subTypeName,
                                            sizeof(chainParentType) - 1);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Bug #2 : `..Cadre..Arrondi` — hover sur le field Arrondi de Cadre,
     * pas sur la fonction native Arrondi(). */
    if (chainParentType[0]) {
        for (int k = 0; k < OWL_COMPOSED_TYPE_CATALOG_COUNT; k++) {
            const OwlComposedTypeField* f = &OWL_COMPOSED_TYPE_CATALOG[k];
            if (strcasecmp(f->typeName, chainParentType) != 0) continue;
            if (strcasecmp(f->fieldName, word) != 0) continue;
            char hover[2048];
            int off = 0;
            off += snprintf(hover + off, sizeof(hover) - off,
                "```owl\n%s..%s : %s%s%s\n```\n\n",
                f->typeName, f->fieldName,
                f->fieldType,
                f->subTypeName ? " " : "",
                f->subTypeName ? f->subTypeName : "");
            if (f->descFR) {
                off += snprintf(hover + off, sizeof(hover) - off,
                    "%s\n\n", f->descFR);
            }
            if (strcmp(f->fieldType, "enum") == 0 && f->subTypeName) {
                const OwlEnumInfo* ei = owl_enum_info_find(f->subTypeName);
                if (ei && ei->values) {
                    off += snprintf(hover + off, sizeof(hover) - off,
                        "**Valeurs %s** : ", ei->name);
                    for (const char* const* v = ei->values; *v; v++) {
                        off += snprintf(hover + off, sizeof(hover) - off,
                            "`%s`%s", *v, *(v+1) ? ", " : "");
                    }
                    off += snprintf(hover + off, sizeof(hover) - off, "\n");
                }
            }
            JsonValue* result = json_new_object();
            JsonValue* contents = json_new_object();
            json_object_set(contents, "kind", json_new_string("markdown"));
            json_object_set(contents, "value", json_new_string(hover));
            json_object_set(result, "contents", contents);
            return result;
        }
    }

    /*  — hover sur une propriété callback `recepteur..SurToken` : si le
     * receveur est une variable dont le type déclaré est un type composé
     * enregistré et que le field porte une signature de callback (callbackSig
     * != NULL), afficher la signature attendue en plus de la description.
     * Gardé strictement sur callbackSig != NULL : ne touche QUE les champs
     * callback (SurToken, SurÉpoque, Procédure d'un Timer…), les autres `..field`
     * retombent sur la résolution existante (aucune régression). Placé avant le
     * scan native/mot-clé pour primer sur un homonyme (`Procédure` mot-clé). */
    {
        const char* csrc = lspServer.documents[idx].content;
        const char* lp = csrc;
        int curLine = 0;
        while (*lp && curLine < line) { if (*lp == '\n') curLine++; lp++; }
        #define IS_ID1711(x) (((x)>='a'&&(x)<='z')||((x)>='A'&&(x)<='Z')|| \
                              ((x)>='0'&&(x)<='9')||(x)=='_'||(unsigned char)(x)>=0x80)
        int c = character - 1;
        while (c >= 0 && IS_ID1711(lp[c])) c--;      /* début du mot survolé */
        if (c >= 1 && lp[c] == '.' && lp[c-1] == '.') {
            int rc = c - 2;
            int rEnd = rc + 1;
            while (rc >= 0 && IS_ID1711(lp[rc])) rc--;
            int rStart = rc + 1;
            if (rEnd > rStart && rEnd - rStart < 64) {
                char recv[64];
                memcpy(recv, lp + rStart, (size_t)(rEnd - rStart));
                recv[rEnd - rStart] = '\0';
                /* Type du receveur : variable user (kind 6) puis retour native. */
                char recvType[64] = "";
                for (int i = 0; i < userSymbolCount; i++) {
                    if (userSymbols[i].kind == 6 &&
                        strcasecmp(recv, userSymbols[i].name) == 0) {
                        strncpy(recvType, userSymbols[i].detail, sizeof(recvType)-1);
                        break;
                    }
                }
                if (recvType[0] == '\0') {
                    const NativeInfo* fn = catalog_find_native(recv);
                    if (fn && fn->signatureCount > 0 && fn->signatures[0].returnType)
                        strncpy(recvType, fn->signatures[0].returnType,
                                sizeof(recvType)-1);
                }
                if (recvType[0]) {
                    const OwlTypeInfo* ti = owl_type_info_find(recvType);
                    const OwlFieldInfo* f = owl_type_field_find(ti, word);
                    if (f && f->callbackSig) {
                        bool fHasEN = (f->nameEN && *f->nameEN
                                       && strcmp(f->name, f->nameEN) != 0);
                        const char* title = (wantEN && fHasEN) ? f->nameEN : f->name;
                        const char* ftype = (wantEN && f->typeEN && *f->typeEN)
                                            ? f->typeEN : (f->type ? f->type : "?");
                        const char* fdesc =
                            (wantEN && f->descriptionEN && *f->descriptionEN)
                            ? f->descriptionEN
                            : (f->description ? f->description : "");
                        const char* tTitle = (wantEN && ti->nameEN && *ti->nameEN)
                                             ? ti->nameEN : ti->name;
                        char sig[256];
                        fmt_callback_sig(sig, sizeof(sig), f->callbackSig, wantEN);
                        char md[1600];
                        snprintf(md, sizeof(md),
                            "```owl\n%s : <%s>\n```\n\n%s\n\n**%s** : `%s`\n\n"
                            "---\n*[%s %s]*",
                            title, ftype, fdesc,
                            wantEN ? "Expected signature" : "Signature attendue",
                            sig,
                            wantEN ? "Field of" : "Champ de", tTitle);
                        JsonValue* hover = json_new_object();
                        JsonValue* contents = json_new_object();
                        json_object_set(contents, "kind", json_new_string("markdown"));
                        json_object_set(contents, "value", json_new_string(md));
                        json_object_set(hover, "contents", contents);
                        return hover;
                    }
                }
            }
        }
        #undef IS_ID1711
    }

    if (isMethodContext) {
        /* Search in method catalog first */
        for (int i = 0; i < OWL_METHOD_CATALOG_COUNT; i++) {
            const OwlMethodInfo* m = &OWL_METHOD_CATALOG[i];
            if (!m->name) continue;
            if (strcasecmp(word, m->name) == 0) {
                char hover[768];
                /*  — signature réelle moins le receveur + desc de la
                 * native sous-jacente (au lieu de `.Cherche()` vide). */
                const NativeInfo* mnat = m->nativeName
                                         ? catalog_find_native(m->nativeName) : NULL;
                char plain[200] = "";
                const char* mdesc = NULL;
                if (mnat) {
                    build_native_plain_params(plain, sizeof(plain), mnat, 1);
                    /*  : desc EN de la native sous-jacente en mode en
                     * (fallback FR si pas de descriptionEN). */
                    mdesc = (wantEN && mnat->descriptionEN && *mnat->descriptionEN)
                            ? mnat->descriptionEN : mnat->description;
                }
                const char* onLabel = wantEN ? "Method on " : "M\xc3\xa9thode sur ";
                const char* tagLabel = wantEN ? "*[Extension method]*"
                                              : "*[M\xc3\xa9thode d'extension]*";
                snprintf(hover, sizeof(hover),
                    "```owl\n.%s(%s)\n```\n\n%s%s`%s`.\n\n---\n%s",
                    m->name, plain,
                    mdesc ? mdesc : "",
                    mdesc ? (wantEN ? "\n\nMethod on " : "\n\nM\xc3\xa9thode sur ") : onLabel,
                    m->receiverType ? m->receiverType : "?",
                    tagLabel);
                JsonValue* result = json_new_object();
                JsonValue* contents = json_new_object();
                json_object_set(contents, "kind", json_new_string("markdown"));
                json_object_set(contents, "value", json_new_string(hover));
                json_object_set(result, "contents", contents);
                return result;
            }
        }
    }

    /*  — hover sur un Groupe declare ('GR_X est un Groupe').
     * Doit precder userSymbols car le scan DSL ne pousse pas les Groupes
     * dans userSymbols. */
    if (!isMethodContext) {
        for (int i = 0; i < userGroupCount; i++) {
            if (strcasecmp(word, userGroups[i].name) == 0) {
                char hover[512];
                snprintf(hover, sizeof(hover),
                    "```owl\n%s est un Groupe\n```\n\n"
                    "Groupe de champs : %d membre%s.\n\n"
                    "Broadcast d'une propriete : `%s..Visible = Faux`. "
                    "Lecture des membres : `%s..Occurrence`.",
                    userGroups[i].name,
                    userGroups[i].memberCount,
                    userGroups[i].memberCount > 1 ? "s" : "",
                    userGroups[i].name, userGroups[i].name);
                JsonValue* result = json_new_object();
                JsonValue* contents = json_new_object();
                json_object_set(contents, "kind", json_new_string("markdown"));
                json_object_set(contents, "value", json_new_string(hover));
                json_object_set(result, "contents", contents);
                return result;
            }
        }
    }

    /* User symbols win over registry catalogs : si le user a declare `tab
     * est un tableau`, le hover sur `tab` doit montrer la variable user,
     * pas la constante TAB du registry. Aligne sur la resolution runtime
     * (OP_GET_LOCAL/userglobal gagne sur globals registry).
     *
     * On preference le doc courant : si plusieurs docs ont un symbole
     * homonyme (TAB declare comme variable dans 2 fichiers ouverts), on
     * privilegie celui du document actuel. */
    {
        int matchIdx = -1;
        for (int i = 0; i < userSymbolCount; i++) {
            if (strcasecmp(word, userSymbols[i].name) != 0) continue;
            if (userSymbols[i].docIndex == idx) { matchIdx = i; break; }
            if (matchIdx < 0) matchIdx = i; /* fallback cross-doc */
        }
        if (matchIdx >= 0) {
            UserSymbol* sym = &userSymbols[matchIdx];
            /* Libelle du kind LSP pour rendre le hover explicite (sinon
             * un user lit "tab -- tableau" sans savoir si c'est var/type). */
            const char* kindLabel;
            switch (sym->kind) {
                case 3:  kindLabel = "fonction"; break;
                case 6:  kindLabel = "variable"; break;
                case 8:  kindLabel = "interface"; break;
                case 10: kindLabel = "classe"; break;
                case 13: kindLabel = "enumeration"; break;
                case 21: kindLabel = "constante"; break;
                case 22: kindLabel = "structure"; break;
                default: kindLabel = "symbole"; break;
            }
            char hover[1024];
            snprintf(hover, sizeof(hover),
                "```owl\n%s %.127s : %.127s\n```\n\n*[Defini dans le projet]*",
                kindLabel, sym->name, sym->detail);
            JsonValue* result = json_new_object();
            JsonValue* contents = json_new_object();
            json_object_set(contents, "kind", json_new_string("markdown"));
            json_object_set(contents, "value", json_new_string(hover));
            json_object_set(result, "contents", contents);
            return result;
        }
    }

    /* Check constants (httpGet, RC, CRLF, ValPI…) */
    for (int i = 0; i < OWL_CONST_CATALOG_COUNT; i++) {
        const OwlConstInfo* c = &OWL_CONST_CATALOG[i];
        if (strcasecmp(word, c->name) == 0) {
            char hover[2048];
            int off = 0;
            /*  : titre = nom EN en mode en (la desc n'a pas de champ
             * EN au catalogue → fallback FR, jamais de vide). */
            const char* cTitle = (wantEN && c->nameEN && *c->nameEN)
                                 ? c->nameEN : c->name;
            off += snprintf(hover + off, sizeof(hover) - off,
                "```owl\n%s %s\n```\n\n%s\n\n",
                wantEN ? "constant" : "constante",
                cTitle,
                c->description ? c->description : "");
            /* Bug #3 : si la constante appartient a un enum, lister
             * les autres valeurs pour faciliter le choix. */
            for (int e = 0; e < OWL_ENUM_CATALOG_COUNT; e++) {
                const OwlEnumInfo* ei = &OWL_ENUM_CATALOG[e];
                if (!owl_enum_info_contains(ei, c->name)) continue;
                off += snprintf(hover + off, sizeof(hover) - off,
                    wantEN ? "**Member of %s** — other values: "
                           : "**Membre de %s** — autres valeurs : ", ei->name);
                for (const char* const* v = ei->values; *v; v++) {
                    if (strcasecmp(*v, c->name) == 0) continue;
                    /*  : en mode EN, emettre le nameEN de la valeur
                     * soeur (resolu au catalogue des constantes), fallback FR
                     * si pas de nameEN. Calque la bascule du titre. */
                    const char* vName = *v;
                    if (wantEN) {
                        for (int k = 0; k < OWL_CONST_CATALOG_COUNT; k++) {
                            if (strcasecmp(OWL_CONST_CATALOG[k].name, *v) == 0) {
                                if (OWL_CONST_CATALOG[k].nameEN &&
                                    *OWL_CONST_CATALOG[k].nameEN) {
                                    vName = OWL_CONST_CATALOG[k].nameEN;
                                }
                                break;
                            }
                        }
                    }
                    off += snprintf(hover + off, sizeof(hover) - off,
                        "`%s`%s", vName, *(v+1) ? ", " : "");
                }
                off += snprintf(hover + off, sizeof(hover) - off, "\n\n");
                break;
            }
            off += snprintf(hover + off, sizeof(hover) - off,
                "---\n*[%s]*",
                c->category ? c->category : "Divers");
            JsonValue* result = json_new_object();
            JsonValue* contents = json_new_object();
            json_object_set(contents, "kind", json_new_string("markdown"));
            json_object_set(contents, "value", json_new_string(hover));
            json_object_set(result, "contents", contents);
            return result;
        }
    }

    /* Check registered builtin types (httpRequête, Chrono, Definition…) */
    {
        const OwlTypeInfo* t = owl_type_info_find(word);
        if (t) {
            char hover[4096];
            int off = 0;
            /*  : titre = nom EN en mode en.  : le corps suit
             * la langue (descEN du type + nameEN/descEN/typeEN des champs). */
            const char* tTitle = (wantEN && t->nameEN && *t->nameEN)
                                 ? t->nameEN : t->name;
            const char* tDesc = (wantEN && t->descriptionEN && *t->descriptionEN)
                                 ? t->descriptionEN
                                 : (t->description ? t->description : "");
            off += snprintf(hover + off, sizeof(hover) - off,
                wantEN ? "```owl\n%s is a Structure\n```\n\n%s\n\n"
                       : "```owl\n%s est une Structure\n```\n\n%s\n\n",
                tTitle, tDesc);
            if (t->fieldCount > 0 && t->fields) {
                off += snprintf(hover + off, sizeof(hover) - off,
                    wantEN ? "**Fields** :\n" : "**Champs** :\n");
                for (int i = 0; i < t->fieldCount; i++) {
                    const OwlFieldInfo* f = &t->fields[i];
                    const char* fName = (wantEN && f->nameEN && *f->nameEN)
                                        ? f->nameEN : f->name;
                    const char* fType = wantEN
                        ? (f->typeEN && *f->typeEN ? f->typeEN
                           : (f->type ? f->type : "?"))
                        : (f->type ? f->type : "?");
                    const char* fDesc = (wantEN && f->descriptionEN && *f->descriptionEN)
                                        ? f->descriptionEN
                                        : (f->description ? f->description : "");
                    off += snprintf(hover + off, sizeof(hover) - off,
                        "- `%s` *(%s)* — %s\n", fName, fType, fDesc);
                }
            }
            off += snprintf(hover + off, sizeof(hover) - off,
                "\n---\n*[%s]*", t->category ? t->category : "Divers");
            JsonValue* result = json_new_object();
            JsonValue* contents = json_new_object();
            json_object_set(contents, "kind", json_new_string("markdown"));
            json_object_set(contents, "value", json_new_string(hover));
            json_object_set(result, "contents", contents);
            return result;
        }
    }

    /* Check native catalog. Si la native a plusieurs signatures, le
     * hover les liste toutes en bloc owl, puis detaille les parametres
     * de la signature primaire (sig[0]) pour rester concis. Le tooltip
     * signatureHelp (au moment de la frappe) montrera l'overload actif. */
    const NativeInfo* fn = catalog_find_native(word);
    if (fn) {
        char hover[2048];
        int off = 0;
        off += snprintf(hover + off, sizeof(hover) - off, "```owl\n");
        /*  — en mode en, la signature canon (FR) prefixe le nom FR
         * (DonneFocus(champ)). On substitue le prefixe nom-de-fonction par
         * nameEN (SetFocus). Les noms de params restent FR (content gap acte
         * cf ). Fallback sur si la signature ne commence pas par le
         * nom canon (cas improbable). */
        bool swapName = wantEN && fn->nameEN && *fn->nameEN
                        && strcmp(fn->name, fn->nameEN) != 0;
        size_t nameLen = strlen(fn->name);
        for (int s = 0; s < fn->signatureCount; s++) {
            const char* sig = fn->signatures[s].signature;
            if (swapName && sig && strncmp(sig, fn->name, nameLen) == 0) {
                off += snprintf(hover + off, sizeof(hover) - off,
                    "%s%s\n", fn->nameEN, sig + nameLen);
            } else {
                off += snprintf(hover + off, sizeof(hover) - off,
                    "%s\n", sig);
            }
        }
        off += snprintf(hover + off, sizeof(hover) - off, "```\n");

        const OwlNativeSigInfo* sig0 = &fn->signatures[0];
        if (sig0->returnType) {
            /*  — le mot du type de retour suit la langue (rien→nothing,
             * entier→integer…). returnTypeEN est resolu au dump via le registry. */
            const char* rt = (wantEN && sig0->returnTypeEN && *sig0->returnTypeEN)
                             ? sig0->returnTypeEN : sig0->returnType;
            off += snprintf(hover + off, sizeof(hover) - off,
                "%s : `%s`\n\n",
                wantEN ? "**Returns**" : "**Retourne**", rt);
        }
        /*  : desc EN en mode en (fallback FR si pas de descriptionEN). */
        const char* fdesc = (wantEN && fn->descriptionEN && *fn->descriptionEN)
                            ? fn->descriptionEN : fn->description;
        off += snprintf(hover + off, sizeof(hover) - off,
            "%s\n\n", fdesc);
        if (sig0->paramCount > 0) {
            off += snprintf(hover + off, sizeof(hover) - off, "%s :\n",
                wantEN ? "**Parameters**" : "**Parametres**");
            for (int p = 0; p < sig0->paramCount; p++) {
                /*  — type du param suit la langue (typeEN au dump). Le
                 * nom du param et sa description restent FR (content gap). */
                const char* pt = (wantEN && sig0->params[p].typeEN
                                  && *sig0->params[p].typeEN)
                                 ? sig0->params[p].typeEN : sig0->params[p].type;
                off += snprintf(hover + off, sizeof(hover) - off,
                    "- `%s` *(%s)* -- %s\n",
                    sig0->params[p].name, pt,
                    sig0->params[p].description);
            }
        }
        if (fn->signatureCount > 1) {
            off += snprintf(hover + off, sizeof(hover) - off,
                wantEN
                  ? "\n*%d more signatures — see signatureHelp while typing.*\n"
                  : "\n*%d signatures supplementaires — voir signatureHelp pendant la frappe.*\n",
                fn->signatureCount - 1);
        }
        /* En mode en : montrer l'alias FR ; en FR : l'alias EN (historique). */
        if (strcmp(fn->name, fn->nameEN) != 0) {
            off += snprintf(hover + off, sizeof(hover) - off,
                wantEN ? "\n*FR alias: %s*\n" : "\n*Alias EN : %s*\n",
                wantEN ? fn->name : fn->nameEN);
        }
        off += snprintf(hover + off, sizeof(hover) - off,
            "\n---\n*[%s]*", fn->category);

        JsonValue* result = json_new_object();
        JsonValue* contents = json_new_object();
        json_object_set(contents, "kind", json_new_string("markdown"));
        json_object_set(contents, "value", json_new_string(hover));
        json_object_set(result, "contents", contents);
        return result;
    }

    /* Check extension methods (for hover on .VersChaine, .Annee, etc.) */
    {
        static const struct { const char* name; const char* sig; const char* desc; const char* ret; } ext_methods[] = {
            {"VersChaine",     ".VersChaine(format?)",      "Convertit en chaine. Format optionnel: \"JJ/MM/AAAA\", \"HHMMSS\", etc.", "chaine"},
            {"VersEntier",     ".VersEntier()",             "Convertit en entier (chaine->entier, date->jours depuis 1800).", "entier"},
            {"VersReel",       ".VersReel()",               "Convertit une chaine en reel.", "reel"},
            {"VersDate",       ".VersDate()",               "Convertit une chaine AAAAMMJJ en Date.", "Date"},
            {"VersHeure",      ".VersHeure()",              "Convertit une chaine HHMMSS en Heure.", "Heure"},
            {"VersJSON",       ".VersJSON()",               "Parse une chaine JSON.", "JSON"},
            {"VersBase64",     ".VersBase64()",             "Encode en Base64.", "chaine"},
            {"VersHexa",       ".VersHexa()",               "Encode en hexadecimal.", "chaine"},
            {"Annee",          ".Annee()",                  "Extrait l'annee d'une date.", "entier"},
            {"Mois",           ".Mois()",                   "Extrait le mois d'une date (1-12).", "entier"},
            {"Jour",           ".Jour()",                   "Extrait le jour d'une date (1-31).", "entier"},
            {"JourDeLaSemaine",".JourDeLaSemaine()",        "Jour de la semaine (1=Lundi, 7=Dimanche).", "entier"},
            {"JourEnLettre",   ".JourEnLettre()",           "Nom du jour en francais (\"Lundi\", \"Mardi\", ...).", "chaine"},
            {"MoisEnLettre",   ".MoisEnLettre()",           "Nom du mois en francais (\"Janvier\", \"Fevrier\", ...).", "chaine"},
            {"EstValide",      ".EstValide()",              "Verifie si la date est valide.", "booleen"},
            {"Majuscule",      ".Majuscule()",              "Convertit en majuscules.", "chaine"},
            {"Minuscule",      ".Minuscule()",              "Convertit en minuscules.", "chaine"},
            {"Gauche",         ".Gauche(n)",                "Retourne les n premiers caracteres.", "chaine"},
            {"Droite",         ".Droite(n)",                "Retourne les n derniers caracteres.", "chaine"},
            {"SansEspace",     ".SansEspace()",             "Supprime les espaces en debut et fin.", "chaine"},
            {"Position",       ".Position(recherche)",      "Position de la sous-chaine (base 1, 0 si absent).", "entier"},
            {"Remplace",       ".Remplace(ancien, nouveau)","Remplace toutes les occurrences.", "chaine"},
            {"Taille",         ".Taille()",                 "Longueur de la chaine ou du tableau.", "entier"},
            {"Occurrence",     ".Occurrence(recherche)",    "Nombre d'occurrences de la sous-chaine.", "entier"},
            {"CommencePar",    ".CommencePar(prefixe)",     "Teste si la chaine commence par le prefixe.", "booleen"},
            {"FinitPar",       ".FinitPar(suffixe)",        "Teste si la chaine finit par le suffixe.", "booleen"},
            {"Contient",       ".Contient(recherche)",      "Teste si la chaine contient la sous-chaine.", "booleen"},
            {"Abs",            ".Abs()",                    "Valeur absolue.", "numerique"},
            {"Arrondi",        ".Arrondi(decimales?)",      "Arrondi a N decimales.", "numerique"},
            {"Ajoute",         ".Ajoute(valeur)",           "Ajoute un element au tableau.", "tableau"},
            {"Trie",           ".Trie()",                   "Trie le tableau.", "tableau"},
            {"Inverse",        ".Inverse()",                "Inverse l'ordre du tableau.", "tableau"},
            {"Cherche",        ".Cherche(valeur)",          "Cherche un element (retourne position, 0 si absent).", "entier"},
            {NULL, NULL, NULL, NULL}
        };
        for (int i = 0; ext_methods[i].name; i++) {
            if (strcasecmp(word, ext_methods[i].name) == 0) {
                char hover[1024];
                snprintf(hover, sizeof(hover),
                    "```owl\n%s\n```\n**Retourne** : `%s`\n\n%s\n\n---\n*[Extension method]*",
                    ext_methods[i].sig, ext_methods[i].ret, ext_methods[i].desc);
                JsonValue* result = json_new_object();
                JsonValue* contents = json_new_object();
                json_object_set(contents, "kind", json_new_string("markdown"));
                json_object_set(contents, "value", json_new_string(hover));
                json_object_set(result, "contents", contents);
                return result;
            }
        }
    }

    /* Check keyword catalog.  — source unique owl_keyword_catalog
     * (label_en + descEN par sens) pour servir le hover bilingue. Un
     * keyword multi-sens liste tous ses sens. Fallback sur le catalogue
     * LSP historique (FR) pour les keywords sans fiche (ex: PROPRIETES). */
    const OwlKeywordDef* kdefH = owl_keyword_find_by_word(word);
    if (kdefH && kdefH->senses_count > 0) {
        char hover[2048];
        int off = 0;
        for (int s = 0; s < kdefH->senses_count; s++) {
            const OwlKeywordSense* sn = &kdefH->senses[s];
            const char* label = (wantEN && sn->label_en && *sn->label_en)
                                ? sn->label_en : sn->label_fr;
            const char* desc  = (wantEN && sn->descEN && *sn->descEN)
                                ? sn->descEN : sn->descFR;
            if (kdefH->senses_count > 1)
                off += snprintf(hover + off, sizeof(hover) - off,
                    "%s**%s**\n\n", s == 0 ? "" : "\n---\n", label ? label : "");
            else
                off += snprintf(hover + off, sizeof(hover) - off,
                    "**%s**\n\n", label ? label : "");
            if (sn->syntax)
                off += snprintf(hover + off, sizeof(hover) - off,
                    "```owl\n%s\n```\n\n", sn->syntax);
            if (desc && *desc)
                off += snprintf(hover + off, sizeof(hover) - off, "%s", desc);
        }
        JsonValue* result = json_new_object();
        JsonValue* contents = json_new_object();
        json_object_set(contents, "kind", json_new_string("markdown"));
        json_object_set(contents, "value", json_new_string(hover));
        json_object_set(result, "contents", contents);
        return result;
    }
    const KeywordInfo* kw = catalog_find_keyword(word);
    if (kw) {
        char hover[1024];
        snprintf(hover, sizeof(hover),
            "```owl\n%s\n```\n\n%s", kw->syntax, kw->description);

        JsonValue* result = json_new_object();
        JsonValue* contents = json_new_object();
        json_object_set(contents, "kind", json_new_string("markdown"));
        json_object_set(contents, "value", json_new_string(hover));
        json_object_set(result, "contents", contents);
        return result;
    }

    /* Note : le check user symbols a ete deplace AVANT le check constants,
     * pour aligner sur la resolution runtime (variable user shadow la
     * constante du registry). Cf le bloc plus haut autour de matchIdx. */

    return json_new_null();
}

/* ================================================================
 * SIGNATURE HELP -- show active parameter
 * ================================================================ */

JsonValue* handle_signature_help(JsonValue* params) {
    bool wantEN = (strcmp(lspServer.language, "en") == 0);
    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (!td || !pos) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);

    int idx = find_doc(uri);
    if (idx < 0) return json_new_null();

    const char* src = lspServer.documents[idx].content;
    int srcLen = lspServer.documents[idx].contentLen;

    /* Find the line start */
    int curLine = 0;
    int lineStart = 0;
    for (int i = 0; i < srcLen && curLine < line; i++) {
        if (src[i] == '\n') { curLine++; lineStart = i + 1; }
    }

    int cursorPos = lineStart + character;
    if (cursorPos > srcLen) cursorPos = srcLen;

    /* Walk backwards from cursor to find the opening '(' and count commas */
    int parenDepth = 0;
    int commaCount = 0;
    int funcNameEnd = -1;

    for (int i = cursorPos - 1; i >= lineStart; i--) {
        char c = src[i];
        if (c == ')') parenDepth++;
        else if (c == '(') {
            if (parenDepth == 0) {
                funcNameEnd = i;
                break;
            }
            parenDepth--;
        }
        else if (c == ',' && parenDepth == 0) {
            commaCount++;
        }
    }

    if (funcNameEnd < 0) return json_new_null();

    /* Extract function name before the '(' — supporte les noms accentues
     * (UTF-8 lead/continuation bytes >= 0x80) : Agrege/Agrège, ChaineCompare/
     * ChaîneCompare, etc. isalnum standard ne capte pas l'UTF-8 et coupait
     * le nom au premier byte accentue. */
    int nameEnd = funcNameEnd;
    int nameStart = nameEnd - 1;
    while (nameStart >= lineStart) {
        unsigned char uc = (unsigned char)src[nameStart];
        if (isalnum(uc) || uc == '_' || uc >= 0x80) {
            nameStart--;
        } else {
            break;
        }
    }
    nameStart++;

    if (nameStart >= nameEnd) return json_new_null();

    char funcName[256];
    int nameLen = nameEnd - nameStart;
    if (nameLen >= 256) nameLen = 255;
    memcpy(funcName, src + nameStart, nameLen);
    funcName[nameLen] = '\0';

    /* Look up in catalog */
    const NativeInfo* fn = catalog_find_native(funcName);
    if (!fn || fn->signatureCount <= 0) return json_new_null();

    /* Itere toutes les signatures de la native. activeSignature pointe
     * vers la "meilleure" sig pour le contexte courant (nombre de
     * virgules deja saisies) ; activeParameter pointe vers le param
     * en cours de saisie dans cette sig.
     *
     * Heuristique de choix de la sig active :
     *   1. Si une sig a paramCount > commaCount, elle est candidate
     *      (le user est en train d'ecrire un param qui existe).
     *   2. Parmi les candidates, on prend la plus courte (la plus
     *      specifique) — ex. Agrege(t, op) gagne sur Agrege(t, op, vi)
     *      tant que commaCount == 1 (le user n'a pas tape la 2e virgule).
     *   3. Si aucune sig n'a assez de params (commaCount > toutes les
     *      arites), on prend la sig la plus large (max paramCount)
     *      avec activeParameter clamp sur le dernier. */
    int activeSig = 0;
    int bestParamCount = INT_MAX;
    for (int s = 0; s < fn->signatureCount; s++) {
        int pc = fn->signatures[s].paramCount;
        if (pc > commaCount && pc < bestParamCount) {
            activeSig = s;
            bestParamCount = pc;
        }
    }
    if (bestParamCount == INT_MAX) {
        /* Aucune sig n'a assez de slots : prendre la plus large. */
        int maxPc = -1;
        for (int s = 0; s < fn->signatureCount; s++) {
            if (fn->signatures[s].paramCount > maxPc) {
                maxPc = fn->signatures[s].paramCount;
                activeSig = s;
            }
        }
    }

    JsonValue* result = json_new_object();
    JsonValue* signatures = json_new_array();

    for (int s = 0; s < fn->signatureCount; s++) {
        const OwlNativeSigInfo* sigInfo = &fn->signatures[s];
        JsonValue* sigObj = json_new_object();
        json_object_set(sigObj, "label", json_new_string(sigInfo->signature));
        /* La description de la native est partagee entre toutes les
         * sigs (au niveau OwlNativeInfo). VSCode l'affiche en sous-
         * titre du tooltip pour chaque variante. Bascule FR/EN comme
         * le hover : descriptionEN quand la langue serveur est EN et
         * que la variante EN est peuplee, sinon fallback FR. */
        const char* sigDoc = (wantEN && fn->descriptionEN && *fn->descriptionEN)
                             ? fn->descriptionEN : fn->description;
        json_object_set(sigObj, "documentation", json_new_string(sigDoc));

        JsonValue* sigParams = json_new_array();
        for (int p = 0; p < sigInfo->paramCount; p++) {
            JsonValue* param = json_new_object();
            json_object_set(param, "label", json_new_string(sigInfo->params[p].name));
            char paramDoc[256];
            snprintf(paramDoc, sizeof(paramDoc), "*(%s)* -- %s",
                     sigInfo->params[p].type, sigInfo->params[p].description);
            json_object_set(param, "documentation", json_new_string(paramDoc));
            json_array_push(sigParams, param);
        }
        json_object_set(sigObj, "parameters", sigParams);
        json_array_push(signatures, sigObj);
    }

    /* activeParameter clamp sur la sig active. */
    const OwlNativeSigInfo* active = &fn->signatures[activeSig];
    int activeParam = commaCount < active->paramCount
                      ? commaCount
                      : (active->paramCount > 0 ? active->paramCount - 1 : 0);

    json_object_set(result, "signatures", signatures);
    json_object_set(result, "activeSignature", json_new_int(activeSig));
    json_object_set(result, "activeParameter", json_new_int(activeParam));

    return result;
}

/* ================================================================
 * SEMANTIC TOKENS
 * ================================================================ */

/* LSP exige les positions en codepoints UTF-16 ; le scanner OWL retourne
 * tok.column en bytes UTF-8. Pour un IDENT precede de caracteres
 * accentues (ex. 'Fenêtre FEN_Login' ou 'Libellé lblUser'), 'ê' = 2 bytes
 * UTF-8 mais 1 char UTF-16 — donc decalage de 1 par accent.
 *
 * Cette fonction parcourt depuis line_start jusqu'a (line_start + byte_off)
 * en comptant les codepoints UTF-16. Hypothese V1 : pas de surrogate pair
 * (codepoint > U+FFFF) — suffisant pour le code source. */
static int utf8_byte_offset_to_utf16_col(const char* line_start, int byte_off) {
    int col = 0;
    int i = 0;
    while (i < byte_off) {
        unsigned char c = (unsigned char)line_start[i];
        if (c < 0x80)              { col++; i++; }
        else if ((c & 0xE0) == 0xC0) { col++; i += 2; }
        else if ((c & 0xF0) == 0xE0) { col++; i += 3; }
        else if ((c & 0xF8) == 0xF0) { col += 2; i += 4; }
        else                         { col++; i++; }
    }
    return col;
}

/*  — longueur d'un token en unites UTF-16 (le protocole LSP compte les
 * longueurs de semantic tokens dans le meme encodage que les positions, ici
 * UTF-16). tok.length est en octets UTF-8 : sur un identifiant accentue
 * (`ThèmeDémo`) la longueur en octets deborde de la longueur reelle et le token
 * mord sur le mot suivant (le `e` de `est` colore comme le nom de bloc). On
 * reutilise le compteur byte->UTF-16 en traitant tok.start comme debut de
 * "ligne" et tok.length comme offset. */
static int tok_utf16_length(Token tok) {
    return utf8_byte_offset_to_utf16_col(tok.start, tok.length);
}

/* Trouve le debut de la ligne contenant tok_start dans le source. */
static const char* find_line_start(const char* source, const char* tok_start) {
    const char* p = tok_start;
    while (p > source && p[-1] != '\n') p--;
    return p;
}

JsonValue* handle_semantic_tokens(JsonValue* params) {
    int64_t _t_st = LSP_NOW_MS();  /*  timing */
    JsonValue* td = json_get(params, "textDocument");
    if (!td) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int idx = find_doc(uri);
    if (idx < 0) return json_new_null();

    const char* source = lspServer.documents[idx].content;

    /* Scan and rescan user symbols to have fresh enum info */
    scan_user_symbols();

    /* Build a lookup table of all enum values */
    typedef struct { char name[64]; } EnumValueEntry;
    EnumValueEntry enumValues[512];
    int enumValueCount = 0;

    for (int i = 0; i < userEnumCount && enumValueCount < 512; i++) {
        UserEnumInfo* ei = &userEnums[i];
        for (int v = 0; v < ei->valueCount && enumValueCount < 512; v++) {
            strncpy(enumValues[enumValueCount].name, ei->values[v], 63);
            enumValues[enumValueCount].name[63] = '\0';
            enumValueCount++;
        }
    }
    /* Inclut aussi les noms de constantes user-defined (CONSTANTE block) :
     * meme classification visuelle qu'une valeur d'enum (jaune dore italique). */
    for (int s = 0; s < userSymbolCount && enumValueCount < 512; s++) {
        if (userSymbols[s].kind == 21) {  /* LSP CompletionItemKind.Constant */
            const char* nm = userSymbols[s].name;
            int nlen = (int)strlen(nm);
            if (nlen >= 64) nlen = 63;
            memcpy(enumValues[enumValueCount].name, nm, nlen);
            enumValues[enumValueCount].name[nlen] = '\0';
            enumValueCount++;
        }
    }

    /* Tokenize the document — 1er passe : collecte tous les tokens en
     * memoire pour pouvoir peeker prev/next dans le 2e passe. Cela permet
     * une classification contextuelle (cf. LSP semantic tokens enumMember
     * qui doit ignorer les declarations de variable et les member access). */
    Scanner scanner;
    scanner_init(&scanner, source);

    Token allTokens[8192];
    int   allCount = 0;
    while (allCount < 8192) {
        Token t = scanner_scan_token(&scanner);
        allTokens[allCount++] = t;
        if (t.type == TOKEN_EOF) break;
    }

    typedef struct { int line, startChar, length, tokenType, tokenModifiers; } SemanticToken;
    SemanticToken tokens[4096];
    int tokenCount = 0;

    /*  — passe 1.5 : scope tracking par index de token.
     *
     * Objectif : distinguer 4 contextes pour chaque token IDENT :
     *   SCOPE_TOPLEVEL : top-level fichier → declaration = globale (mods=1)
     *   SCOPE_CLASS    : body Classe → declaration = champ (property, type=2)
     *   SCOPE_PROC     : body Procedure/Fonction → declaration locale (mods=0)
     *   SCOPE_PARAMS   : entre `(...)` du header proc → param local (mods=0)
     *
     * Algorithme :
     *  - Stack de scopes. Push SCOPE_CLASS sur TOKEN_CLASSE, SCOPE_PROC sur
     *    TOKEN_PROCEDURE/TOKEN_FONCTION. Sur '(' qui suit l'IDENT de nom de
     *    proc, push SCOPE_PARAMS (pop sur ')' matchant).
     *  - Sub-depth counter pour SI/POUR/TANTQUE/SELON/BOUCLE/AVEC : ces
     *    keywords ouvrent un FIN mais ne changent pas le scope. On les
     *    compte par frame de scope ; le FIN qui pop le scope est celui qui
     *    apparait quand sub-depth==0.
     *  - STRUCTURE/ENUMERATION/COMBINAISON/INTERFACE : push SCOPE_TOPLEVEL
     *    isolant (declarations dedans ne sont pas notre cible — pas de
     *    confusion avec champ classe ; mais on doit consommer leur FIN).
     *
     * On collecte aussi, pour chaque PROCEDURE/FONCTION, la liste des
     * noms declares dedans (params + locales). Sert a colorier les
     * USAGES en variable locale (mods=0) et a ne pas les écraser par la
     * regle globale plus bas. */
    enum { SCOPE_TOPLEVEL = 0, SCOPE_CLASS, SCOPE_PROC, SCOPE_PARAMS, SCOPE_OTHER_ISOLATE };
    typedef struct {
        int kind;
        int subDepth;       /* nb de SI/POUR/... ouverts dans cette frame */
        int procIndex;      /* index dans procLocals[] si kind==PROC ou PARAMS, sinon -1 */
        bool paramListSeen; /* PROC seulement : true des qu'on a vu la 1re ( apres le nom */
    } ScopeFrame;
    ScopeFrame scopeStack2[64];
    int scopeTop2 = 0;
    signed char scopeAtToken[8192];  /* parallel a allTokens[] */

    typedef struct {
        int  startIdx;        /* index dans allTokens du token PROCEDURE/FONCTION */
        int  endIdx;          /* index du FIN matchant (inclus) */
        char locals[64][64];  /* params + variables locales declarees */
        int  localCount;
    } ProcLocalInfo;
    ProcLocalInfo procLocals[64];
    int procLocalCount = 0;

    for (int ti = 0; ti < allCount; ti++) {
        Token t = allTokens[ti];
        /* Scope courant = top de stack (avant l'effet du token courant) */
        int curScope = scopeTop2 > 0 ? scopeStack2[scopeTop2 - 1].kind : SCOPE_TOPLEVEL;
        scopeAtToken[ti] = (signed char)curScope;

        /* Ouverture de scope */
        if (t.type == TOKEN_CLASSE) {
            if (scopeTop2 < 64) {
                scopeStack2[scopeTop2].kind         = SCOPE_CLASS;
                scopeStack2[scopeTop2].subDepth     = 0;
                scopeStack2[scopeTop2].procIndex    = -1;
                scopeStack2[scopeTop2].paramListSeen = false;
                scopeTop2++;
            }
            continue;
        }
        if (t.type == TOKEN_PROCEDURE || t.type == TOKEN_FONCTION) {
            int procIdx = -1;
            if (procLocalCount < 64) {
                procIdx = procLocalCount++;
                procLocals[procIdx].startIdx   = ti;
                procLocals[procIdx].endIdx     = -1;
                procLocals[procIdx].localCount = 0;
            }
            if (scopeTop2 < 64) {
                scopeStack2[scopeTop2].kind         = SCOPE_PROC;
                scopeStack2[scopeTop2].subDepth     = 0;
                scopeStack2[scopeTop2].procIndex    = procIdx;
                scopeStack2[scopeTop2].paramListSeen = false;
                scopeTop2++;
            }
            continue;
        }
        if (t.type == TOKEN_STRUCTURE   || t.type == TOKEN_ENUMERATION ||
            t.type == TOKEN_COMBINAISON || t.type == TOKEN_INTERFACE) {
            if (scopeTop2 < 64) {
                scopeStack2[scopeTop2].kind         = SCOPE_OTHER_ISOLATE;
                scopeStack2[scopeTop2].subDepth     = 0;
                scopeStack2[scopeTop2].procIndex    = -1;
                scopeStack2[scopeTop2].paramListSeen = false;
                scopeTop2++;
            }
            continue;
        }
        /* Push SCOPE_PARAMS sur la 1re '(' apres le nom de proc.
         * Critere robuste : frame courante est SCOPE_PROC, paramListSeen=false,
         * et prev token est l'IDENT du nom de proc. La condition prev=IDENT
         * evite les () de control flow (SI ... ALORS n'ouvre pas avec '(').
         * Une fois paramListSeen=true, on ne re-push plus, donc les `(` du
         * body (appels de fonction) ne sont pas confondus avec params. */
        if (t.type == TOKEN_LEFT_PAREN
            && scopeTop2 > 0
            && scopeStack2[scopeTop2 - 1].kind == SCOPE_PROC
            && !scopeStack2[scopeTop2 - 1].paramListSeen
            && ti > 0 && allTokens[ti - 1].type == TOKEN_IDENTIFIER) {
            scopeStack2[scopeTop2 - 1].paramListSeen = true;
            if (scopeTop2 < 64) {
                scopeStack2[scopeTop2].kind         = SCOPE_PARAMS;
                scopeStack2[scopeTop2].subDepth     = 0;
                scopeStack2[scopeTop2].procIndex    = scopeStack2[scopeTop2 - 1].procIndex;
                scopeStack2[scopeTop2].paramListSeen = false;
                scopeTop2++;
            }
            scopeAtToken[ti] = SCOPE_PARAMS;
            continue;
        }
        /* Fermeture SCOPE_PARAMS sur ')' */
        if (t.type == TOKEN_RIGHT_PAREN && scopeTop2 > 0 &&
            scopeStack2[scopeTop2 - 1].kind == SCOPE_PARAMS) {
            scopeTop2--;
            continue;
        }

        /* Sub-block keywords : incrementent subDepth de la frame courante.
         * PROCEDURE/FONCTION imbriques deja gere ci-dessus (push frame). */
        if (t.type == TOKEN_AVEC) {
            if (scopeTop2 > 0) scopeStack2[scopeTop2 - 1].subDepth++;
            continue;
        }
        if (t.type != TOKEN_FIN && lsp_depth_table_knows(t)) {
            if (lsp_depth_delta(t) > 0 && scopeTop2 > 0)
                scopeStack2[scopeTop2 - 1].subDepth++;
            continue;
        }

        if (t.type == TOKEN_FIN) {
            if (scopeTop2 > 0) {
                if (scopeStack2[scopeTop2 - 1].subDepth > 0) {
                    scopeStack2[scopeTop2 - 1].subDepth--;
                } else {
                    /* Pop scope. Si c'etait SCOPE_PROC, recorder endIdx. */
                    ScopeFrame popped = scopeStack2[scopeTop2 - 1];
                    scopeTop2--;
                    if (popped.kind == SCOPE_PROC && popped.procIndex >= 0 &&
                        popped.procIndex < procLocalCount) {
                        procLocals[popped.procIndex].endIdx = ti;
                    }
                }
            }
            continue;
        }

        /* Collecte des declarations dans SCOPE_PARAMS/SCOPE_PROC pour
         * marquer ensuite les usages. Pattern : IDENT EST. */
        if (t.type == TOKEN_IDENTIFIER && (curScope == SCOPE_PARAMS || curScope == SCOPE_PROC)) {
            TokenType nx = (ti + 1 < allCount) ? allTokens[ti + 1].type : TOKEN_EOF;
            if (nx == TOKEN_EST) {
                int pi = scopeStack2[scopeTop2 - 1].procIndex;
                if (pi >= 0 && pi < procLocalCount &&
                    procLocals[pi].localCount < 64) {
                    int n = t.length < 63 ? t.length : 63;
                    memcpy(procLocals[pi].locals[procLocals[pi].localCount], t.start, n);
                    procLocals[pi].locals[procLocals[pi].localCount][n] = '\0';
                    procLocals[pi].localCount++;
                }
            }
        }
    }

    /* Helper : retrouve l'index proc englobant un token (ou -1). */
    #define FIND_ENCLOSING_PROC(_ti) ({ \
        int _r = -1; \
        for (int _k = 0; _k < procLocalCount; _k++) { \
            int _e = procLocals[_k].endIdx >= 0 ? procLocals[_k].endIdx : allCount; \
            if ((_ti) > procLocals[_k].startIdx && (_ti) <= _e) { _r = _k; } \
        } \
        _r; \
    })

    /*  — suivi du bloc PROPRIETES pour colorer les noms de props user
     * (hors registry) en property. inPropBlock = on est entre un PROPRIETES et
     * son FIN ; propInnerDepth = profondeur des blocs FIN-fermables ouverts a
     * l'interieur (SI/POUR/... — pas de bloc `est un` dans un corps PROPRIETES,
     * qui n'est qu'une liste d'affectations). Le FIN au niveau 0 ferme le
     * PROPRIETES. */
    bool inPropBlock = false;
    int  propInnerDepth = 0;

    /* 2e passe : classifie chaque IDENTIFIER selon le contexte. */
    for (int ti = 0; ti < allCount && tokenCount < 4096; ti++) {
        Token tok = allTokens[ti];

        /*  — mise a jour du contexte PROPRIETES avant tout `continue`. */
        if (tok.type == TOKEN_PROPRIETES) {
            inPropBlock = true; propInnerDepth = 0;
        } else if (tok.type == TOKEN_FIN) {
            if (inPropBlock) {
                if (propInnerDepth == 0) inPropBlock = false;
                else propInnerDepth--;
            }
        } else if (inPropBlock && tok.type == TOKEN_AVEC) {
            propInnerDepth++;
        } else if (inPropBlock && lsp_depth_table_knows(tok)
                   && lsp_depth_delta(tok) > 0) {
            propInnerDepth++;
        }

        /*  — `icones.<keyword>` : certains ids du catalog Iconify
         * collisionnent avec des alias EN de keywords scanner OWL. Cas
         * historique : `Delete` est tokenise TOKEN_LIBERER (scanner L328,
         * alias EN de `liberer`/`free`). Sans cette branche, le membre
         * `Delete` est filtre par le `if (tok.type != TOKEN_IDENTIFIER)
         * continue;` plus bas, donc  (qui annote enumMember sur
         * `icones.<id>`) ne le voit jamais. Resultat : mot retombe sur
         * defaut TextMate `source.owl = #4CAF50` (vert).
         *
         * Solution : detection precoce du pattern `icones.<X>` ou X est
         * un token non-IDENTIFIER (keyword OWL). Si receveur = icones,
         * on emet enumMember (jaune italique) malgre le type token de X.
         * La branche  plus bas reste active pour les ids
         * tokenises en IDENTIFIER normaux (home, Save, etc.). */
        if (tok.type != TOKEN_IDENTIFIER && ti >= 2 &&
            allTokens[ti-1].type == TOKEN_DOT &&
            allTokens[ti-2].type == TOKEN_IDENTIFIER &&
            (is_icones_receiver_name(allTokens[ti-2].start,
                                     allTokens[ti-2].length) ||
             is_theme_receiver_name(allTokens[ti-2].start,
                                    allTokens[ti-2].length))) {
            const char* lineStartK = find_line_start(source, tok.start);
            int byteOffK = (int)(tok.start - lineStartK);
            int utf16ColK = utf8_byte_offset_to_utf16_col(lineStartK,
                                                          byteOffK);
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16ColK;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 0;       /* enumMember */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — mot-cle de TYPE en position de declaration
         * (`X est une Énumération|Structure|Interface|Combinaison`). Ces
         * mots-cles ne sont pas des IDENTIFIER (donc filtres par le `continue`
         * juste apres) : sans token semantique, leur couleur depend entierement
         * de la grammar TextMate, qui les recolore keyword (orange) a
         * l'interieur d'un bloc CollectionDeProcedures. On emet ici un token
         * `type` (4, violet) — la meme couleur que hors collection — pour rendre
         * la coloration deterministe et identique dans les deux contextes. Gate
         * stricte sur la copule immediate (EST en FR, l'article un/une est un
         * filler jete par le scanner ; TOKEN_A en EN) : ces mots-cles
         * n'apparaissent que la, mais on reste conservateur. */
        if ((tok.type == TOKEN_ENUMERATION || tok.type == TOKEN_COMBINAISON ||
             tok.type == TOKEN_STRUCTURE   || tok.type == TOKEN_INTERFACE)) {
            TokenType kwPrev = (ti > 0) ? allTokens[ti-1].type : TOKEN_EOF;
            if (kwPrev == TOKEN_EST || kwPrev == TOKEN_A) {
                const char* lineStartT = find_line_start(source, tok.start);
                int byteOffT = (int)(tok.start - lineStartT);
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar =
                    utf8_byte_offset_to_utf16_col(lineStartT, byteOffT);
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 4;  /* type (violet) */
                tokens[tokenCount].tokenModifiers = 0;
                tokenCount++;
                continue;
            }
        }

        /*  — mot-cle de declaration de controle (TOKEN_LIBELLE, ...)
         * utilise comme NOM DE PROP dans un bloc PROPRIETES. Cas support #301 :
         * la prop d'en-tete d'une Colonne ecrite `Label`/`Libellé` est scannee
         * TOKEN_LIBELLE (widget), donc filtree par le `continue` juste apres :
         * sans token semantique, la grammar TextMate la colore keyword (orange)
         * au lieu de property (vert) comme ses voisines (Width, X...). On emet
         * ici property (2) quand le keyword est en position de prop stricte :
         * dans le PROPRIETES au niveau 0 (propInnerDepth==0), debut de statement
         * (prev = NEWLINE|PROPRIETES) et suivi de `=` ou `..`. Meme patron que
         * la branche IDENT  plus bas, transpose aux tokens keyword.
         * La DECLARATION `LBL_x est un Libellé` ne matche pas (le type suit une
         * copule EST/A et precede un NEWLINE), donc elle reste keyword. */
        if (inPropBlock && propInnerDepth == 0
            && (tok.type == TOKEN_LIBELLE || tok.type == TOKEN_BOUTON
                || tok.type == TOKEN_SAISIE || tok.type == TOKEN_TABLE
                || tok.type == TOKEN_COLONNE || tok.type == TOKEN_CONTENEUR
                || tok.type == TOKEN_LISTE)) {
            TokenType kwPrev = (ti > 0)            ? allTokens[ti-1].type : TOKEN_EOF;
            TokenType kwNext = (ti + 1 < allCount) ? allTokens[ti+1].type : TOKEN_EOF;
            if ((kwPrev == TOKEN_NEWLINE || kwPrev == TOKEN_PROPRIETES)
                && (kwNext == TOKEN_EQUAL || kwNext == TOKEN_DOUBLE_DOT)) {
                const char* lineStartK = find_line_start(source, tok.start);
                int byteOffK = (int)(tok.start - lineStartK);
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar =
                    utf8_byte_offset_to_utf16_col(lineStartK, byteOffK);
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 2;  /* property (vert) */
                tokens[tokenCount].tokenModifiers = 0;
                tokenCount++;
                continue;
            }
        }

        if (tok.type != TOKEN_IDENTIFIER) continue;

        /* Extract token text */
        char tokName[256];
        int len = tok.length;
        if (len >= 256) len = 255;
        memcpy(tokName, tok.start, len);
        tokName[len] = '\0';

        /* Conversion byte→codepoint UTF-16 pour le startChar. */
        const char* lineStart = find_line_start(source, tok.start);
        int byteOff = (int)(tok.start - lineStart);
        int utf16Col = utf8_byte_offset_to_utf16_col(lineStart, byteOff);

        /*  — Type de sous-bloc Etat (EnTeteDocument / HautDePage /
         * Corps / BasDePage / FinDeDocument / Iteration). Le scanner les voit
         * comme IDENT, donc sans cette branche la grammar TextMate ne les
         * coloriait pas en violet (defaut neutre). Critere strict : prev token
         * est EST (`X est un Corps`). Ce check passe AVANT isControlName car
         * la collision casse-insensible est inevitable (`corps est un Corps` :
         * le nom canon `corps` est en userControls, l'usage du TYPE `Corps`
         * matcherait sans ce filtre). */
        TokenType _prevTypeEarly = (ti > 0) ? allTokens[ti-1].type : TOKEN_EOF;
        /*  — copule EN `is a <Type>` : l'article `a` (TOKEN_A) se
         * glisse entre EST et le type, donc le prev immediat est TOKEN_A en
         * EN. Accepter les deux copules (miroir des branches Projet/Thème
         * ci-dessous), sinon `X is a Corps` retombe neutre. */
        if ((_prevTypeEarly == TOKEN_EST || _prevTypeEarly == TOKEN_A)
            && is_etat_block_type_name(tokName)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 4;  /* type (violet) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — Type conteneur `Projet`/`Project` en position de type de
         * declaration (`X est un Projet` / `X is a Project`). Comme les types
         * de sous-blocs Etat, le scanner le voit comme IDENT : sans cette
         * branche il retombe neutre (cyan). Le prev token est EST en FR (copule
         * `est`, l'article `un/une` est un filler consomme), TOKEN_A en EN
         * (copule `is a`, `a` produit TOKEN_A). Critere strict sur ces deux
         * copules -> un identifiant `Projet` ailleurs (variable, usage) ne
         * devient pas violet. Le NOM (1er token) reste class via userWindows
         *. */
        if ((_prevTypeEarly == TOKEN_EST || _prevTypeEarly == TOKEN_A)
            && is_projet_type_token(&tok)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 4;  /* type (violet) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — type conteneur `Thème`/`Theme` en position de type de
         * declaration (`X est un Thème` / `X is a Theme`). Meme cas que
         * `Projet` : scanner IDENT, sans cette branche il retombe neutre
         * (defaut TextMate, vert clair). Copule EST (FR) ou TOKEN_A (EN).
         * Le compare est accent-insensible (is_theme_type_token). */
        if ((_prevTypeEarly == TOKEN_EST || _prevTypeEarly == TOKEN_A)
            && is_theme_type_token(&tok)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 4;  /* type (violet) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — type compose (styleBouton/styleSaisie/Police/...) en
         * position de DECLARATION `X est un styleBouton = {` (corps d'un bloc
         * `est un Thème`) : colorer TYPE (4, violet), pas property. Le `= {`
         * qui suit ferait matcher la branche prop compose  (property,
         * vert) sinon — mais ici le mot est un TYPE, pas un nom de prop. Le
         * discriminant est la copule `est` juste avant (`X est un <Type>`),
         * jamais presente devant une prop `Police = { ... }` d'un bloc
         * PROPRIETES. */
        /*  — copule EN `is a styleXxx` : prev immediat = TOKEN_A
         * (article `a`), pas TOKEN_EST. Accepter les deux (miroir des
         * branches Projet/Thème), sinon le type compose EN retombe neutre. */
        if ((_prevTypeEarly == TOKEN_EST || _prevTypeEarly == TOKEN_A)
            && is_composed_type_name(tokName)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 4;  /* type (violet) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /* DSL Fenetre v1 : verifier si IDENT matche un nom declare comme
         * Fenetre / Bouton / Libelle / Saisie. Si oui, emettre un
         * semantic token contextuel (class/property). */
        bool isWindowName = false;
        for (int i = 0; i < userWindowCount; i++) {
            if (strcasecmp(tokName, userWindows[i].name) == 0) { isWindowName = true; break; }
        }
        if (isWindowName) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 1;  /* class */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }
        bool isControlName = false;
        char controlKind = 0;
        for (int i = 0; i < userControlCount; i++) {
            if (strcasecmp(tokName, userControls[i].name) == 0) {
                isControlName = true;
                controlKind = userControls[i].kind;
                break;
            }
        }
        if (isControlName) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            /* DSL Etat V2 : sub-blocs (kind 'E') colorisés comme le nom de
             * l'Etat parent (tokenType 1 = class, vert). Coherence visuelle :
             * le nom d'Etat et ses sub-blocs sont des objets nommes du DSL.
             * Pour Fenetre (L/B/S), on garde property (turquoise). */
            tokens[tokenCount].tokenType = (controlKind == 'E') ? 1 : 2;
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        TokenType prevType = (ti > 0)            ? allTokens[ti-1].type : TOKEN_EOF;
        TokenType nextType = (ti + 1 < allCount) ? allTokens[ti+1].type : TOKEN_EOF;

        bool isDeclaration  = (nextType == TOKEN_EST);
        /*  — TOKEN_COLON inclus : `:ident` est le self member access
         * OWL (acces au champ classe depuis une methode, equivalent `this.x`
         * dans d'autres langages). Sans TOKEN_COLON, `:nGlobal` etait
         * classifie 'variable globale' (cyan) quand un homonyme global
         * existait par ailleurs, au lieu d'etre laisse a la grammar
         * TextMate comme `obj.ident` / `obj..ident`. */
        bool isMemberAccess = (prevType == TOKEN_DOT
                            || prevType == TOKEN_DOUBLE_DOT
                            || prevType == TOKEN_COLON);

        /*  — nom d'etat dans `Etats[<etat>]` (survol/focus/appuyé/
         * grisé/lectureSeule, FR ou EN) : token enumMember (0, jaune italique
         * #B1C33A), meme classe visuelle que les valeurs d'enum. Vaut PARTOUT
         * ou un `..Etats[...]` apparait (style de WIDGET *et* de MENU), pas de
         * cas particulier. Sans cette branche l'ident d'etat retombe vert par
         * defaut (aucun autre lookup ne le capte). Detection structurelle (pas
         * de liste d'etats en dur) : token courant encadre par `[` .. `]`, le
         * receveur immediat du crochet == Etats/States. Robuste aux futurs
         * etats sans maintenance de liste, et le gate strict sur le receveur
         * Etats evite de colorer une indexation tableau `tab[i]` quelconque. */
        if (prevType == TOKEN_LEFT_BRACKET && nextType == TOKEN_RIGHT_BRACKET
            && ti >= 2 && allTokens[ti-2].type == TOKEN_IDENTIFIER
            && is_etats_receiver_name(allTokens[ti-2].start,
                                      allTokens[ti-2].length)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 0;  /* enumMember */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — nom de bloc CollectionDeProcedures : token class (index 1,
         * vert fonce), comme un nom de Fenetre/Etat. Couvre la DECLARATION
         * `COL est une CollectionDeProcedures` et le RECEVEUR d'un acces
         * qualifie `COL.membre`. On exclut le member access (`obj.COL`) : un
         * nom de collection n'apparait jamais apres un point. Passe AVANT la
         * regle variable globale (le nom est aussi capte comme globale kind 6)
         * pour l'emporter, en miroir de userWindows (class > globale). */
        if (!isMemberAccess && collection_def_lookup(tokName) != NULL) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 1;  /* class (vert fonce) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — `:champ` dans body methode classe → property (vert fonce).
         * Cas typique : `:nSolde = :nSolde + rMontant` dans la methode
         * `cCompte.Credite`. Sans cette branche,  skippait la
         * classification (member access) mais aucune autre regle n'emettait
         * tokenType=2, donc le membre restait neutre (defaut grammar) ou pire
         * cyan via une autre regle. Lookup : `user_class_enclosing(idx, line)`
         * trouve la classe dont le body englobe la position courante ; on
         * cherche `tokName` dans ses fields (members[].isMethod==false).
         * Casse-insensible (canon : `:nsolde` == `:nSolde`).
         *
         * Ne s'applique qu'au prefixe `:` (self member access). `obj.field`
         * (TOKEN_DOT) reste gere par la grammar TextMate / autres branches
         * (member access generique sans lookup). */
        if (prevType == TOKEN_COLON) {
            UserClassInfo* enclosing = user_class_enclosing(idx, tok.line - 1);
            bool emittedSelfMember = false;
            if (enclosing != NULL) {
                for (int m = 0; m < enclosing->memberCount; m++) {
                    const UserClassMember* memb = &enclosing->members[m];
                    if (!memb->isMethod &&
                        strcasecmp(tokName, memb->name) == 0) {
                        tokens[tokenCount].line = tok.line - 1;
                        tokens[tokenCount].startChar = utf16Col;
                        tokens[tokenCount].length = tok_utf16_length(tok);
                        tokens[tokenCount].tokenType = 2;  /* property */
                        tokens[tokenCount].tokenModifiers = 0;
                        tokenCount++;
                        emittedSelfMember = true;
                        break;
                    }
                }
            }
            if (emittedSelfMember) continue;
        }

        /*  — type compose builtin (Police/Cadre/Style...) utilise
         * comme PROP dans un bloc PROPRIETES. La grammar TextMate colore ces
         * noms en violet (type) de maniere inconditionnelle ; ici on emet un
         * semantic token 'property' (index 2, vert) qui l'emporte, quand le
         * nom est en position de PROP : suivi de `=` (litteral `Police = {..}`)
         * ou de `..` (cascade `Police..Nom = ...`), en debut de statement.
         * On exclut le member access (`LIB..Police` est deja un field, pas une
         * prop declarative). Quand le type compose est utilise comme TYPE
         * (`x est un Police`), nextType n'est ni `=` ni `..` → on laisse la
         * grammar le colorer en violet (pas de token emis ici). */
        if (!isMemberAccess
            && (nextType == TOKEN_EQUAL || nextType == TOKEN_DOUBLE_DOT)
            && is_composed_type_name(tokName)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 2;  /* property (vert) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — nom de propriete user (hors registry) dans un bloc
         * PROPRIETES : `Langues = ...`, `Thème = ...` (bloc `est un Projet`),
         * ou toute prop d'un Fenetre/Etat non couverte par la liste flat de la
         * grammar. La grammar TextMate ne colore en property (vert fonce) que
         * les noms connus du registry ; un nom user retombe en identifiant
         * generique (vert clair). Ici on emet property (2) pour l'IDENT en tete
         * de ligne, directement dans le PROPRIETES (propInnerDepth==0), suivi de
         * `=` ou `..`. Exclut le member access (RHS `= x.y`). */
        if (inPropBlock && propInnerDepth == 0 && !isMemberAccess
            && (nextType == TOKEN_EQUAL || nextType == TOKEN_DOUBLE_DOT)
            && (prevType == TOKEN_NEWLINE || prevType == TOKEN_PROPRIETES)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 2;  /* property (vert fonce) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  +  — receveur `icones` dans `icones.<id>` : token
         * type=3 (variable) avec modifier 'namespace' (bit 1) pour rendu
         * blanc/neutre via la regle theme `variable.namespace` du package.json
         * (cf. le package.json du plugin vscode d'integration). 
         * emettait mods=0 en supposant que l'absence de regle theme donnerait
         * un defaut neutre — faux : sans regle semantic specifique, le mot
         * retombait sur le defaut TextMate `source.owl = #4CAF50` (vert). Le
         * modifier dedie permet d'ancrer une regle theme ciblee sans toucher
         * a `variable` simple (qui ne doit pas regresser ailleurs). Pattern :
         * IDENT courant suivi de TOKEN_DOT, pas en position de member access
         * lui-meme, et strip(name) == "icones". Le membre apres le point
         * reste enumMember(0) via la branche  placee plus bas.
         * Passe AVANT user_class_lookup pour qu'un user qui declarerait
         * `Icones` comme classe ne court-circuite pas la coloration namespace
         * (le nom est reserve cote OWL pour le catalog Iconify). */
        if (!isMemberAccess && nextType == TOKEN_DOT &&
            (is_icones_receiver_name(tok.start, tok.length) ||
             is_theme_receiver_name(tok.start, tok.length))) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 3;       /* variable */
            tokens[tokenCount].tokenModifiers = 1u << 1; /* 'namespace' (bit 1) → theme variable.namespace */
            tokenCount++;
            continue;
        }

        /*  — reference a une classe user. On exclut le member access
         * (`obj.cCompte` serait un field, pas une reference de type). Deux cas :
         *   - SITE DE DECLARATION `cCompte est une Classe` → token 'class'
         *     (index 1, cible vert fonce) : entite nommee declaree, meme couleur
         *     que les Fenetres/Etats. Signature : l'IDENT est suivi de EST puis
         *     directement de CLASSE. Les articles (un/une) sont des fillers
         *     consommes par le scanner et ne produisent pas de token, donc
         *     allTokens[ti+2] est bien le mot-cle CLASSE (et pas l'article).
         *     Ce peek a 2 crans elimine le faux positif `cCompte est un entier`
         *     (variable homonyme : ti+2 serait un type, pas CLASSE).
         *   - SITE D'USAGE `est un cCompte()`, `de cCompte`, type de retour →
         *     token 'type' (index 4, cible violet) : type utilise. */
        if (!isMemberAccess && user_class_lookup(tokName) != NULL) {
            /*  — copule EN `is a/an Class` : l'article (TOKEN_A ou
             * `an`) s'intercale entre EST et CLASSE (en FR `un/une` est un
             * filler jete par le scanner, donc allTokens[ti+2]==CLASSE
             * directement). On saute l'article pour que la DECLARATION EN soit
             * detectee comme telle -> class(1), a parite avec la forme FR. Sans
             * ca la decl EN retombe en usage -> type(4, violet). */
            int typeIdx = ti + 2;
            if (typeIdx < allCount && tok_is_en_article(allTokens[typeIdx]))
                typeIdx++;
            bool isClassDecl = (nextType == TOKEN_EST
                                && typeIdx < allCount
                                && allTokens[typeIdx].type == TOKEN_CLASSE);
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = isClassDecl ? 1   /* class (decl) */
                                                       : 4;  /* type (usage) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  rev3 — nom d'Enumeration / Combinaison / Interface user :
         * token 'type' (index 4, violet) en DECLARATION ET USAGE, comme le
         * mot-cle. On exclut le member access (`obj.Statut` serait un field).
         * IMPORTANT : seul le NOM du type matche ici (user_enum_lookup /
         * user_interface_lookup raisonnent sur le nom declare). Les MEMBRES
         * d'enum (`actif`, `inactif`) vivent dans values[] et sont traites plus
         * bas en enumMember (index 0) — pas de confusion possible. */
        if (!isMemberAccess &&
            (user_enum_lookup(tokName) != NULL || user_interface_lookup(tokName))) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 4;  /* type (enum/combi/interface) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — acces qualifie `icones.<id>` (catalog Iconify ) :
         * le receveur `icones` n'est pas un enum user-defined (donc pas couvert
         * par la branche rev4 ci-dessous), mais semantiquement les <id> sont des
         * constantes (kind=EnumMember cote completion). On les colorise comme
         * les autres enumMember (jaune italique #B1C33A) pour coherence visuelle.
         * Detection : receveur (allTokens[ti-2]) est un IDENT qui, accent-strippe,
         * matche "icones" casse-insensible. Pas de gate sur la presence dans le
         * catalog : coloration statique sur la forme (un id inconnu reste annote).
         * Passe AVANT rev4/method pour eviter les conflits si un id homonyme a
         * une methode user du meme nom. */
        if (prevType == TOKEN_DOT && ti >= 2 &&
            allTokens[ti-2].type == TOKEN_IDENTIFIER &&
            (is_icones_receiver_name(allTokens[ti-2].start,
                                     allTokens[ti-2].length) ||
             is_theme_receiver_name(allTokens[ti-2].start,
                                    allTokens[ti-2].length))) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 0;  /* enumMember */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — acces qualifie `COL.membre` ou COL est un bloc
         * CollectionDeProcedures : on colore le MEMBRE selon sa nature, en
         * miroir des couleurs qu'il aurait en nom nu.
         *   - membre = enum/combinaison  → type (4, violet)   ex `COL.Statut`
         *   - membre = procedure/fonction → method (5, vert)    ex `COL.Doubler`
         *   - sinon (globale du bloc)      → variable globale (3, cyan) ex `COL.gMode`
         * Le receveur COL lui-meme est deja class (branche  ci-dessus).
         * Passe AVANT rev4 (receiver enum) et la logique method : le
         * discriminant est « le receveur est-il une collection ? ». Pour
         * `COL.Statut.actif`, seul `Statut` matche ici (receiver COL) ; `actif`
         * a pour receiver `Statut` (enum) et tombe dans rev4. */
        if (prevType == TOKEN_DOT && ti >= 2 &&
            allTokens[ti-2].type == TOKEN_IDENTIFIER) {
            char cRecv[256];
            int crl = allTokens[ti-2].length;
            if (crl >= 256) crl = 255;
            memcpy(cRecv, allTokens[ti-2].start, crl);
            cRecv[crl] = '\0';
            if (collection_def_lookup(cRecv) != NULL) {
                int tt, mods = 0;
                if (user_enum_lookup(tokName) != NULL || user_interface_lookup(tokName)) {
                    tt = 4;  /* type (enum/combi) */
                } else {
                    bool isProc = (disk_proc_lookup(tokName) != NULL);
                    for (int s = 0; !isProc && s < userSymbolCount; s++) {
                        if (userSymbols[s].kind == 3 &&
                            strcasecmp(tokName, userSymbols[s].name) == 0) isProc = true;
                    }
                    if (isProc) { tt = 5; }        /* method */
                    else        { tt = 3; mods = 1; }  /* variable globale (cyan) */
                }
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = tt;
                tokens[tokenCount].tokenModifiers = mods;
                tokenCount++;
                continue;
            }
        }

        /*  rev4 — acces qualifie `EnumType.membre` (ex `Statut.actif`) :
         * quand le receiver (token AVANT le point, allTokens[ti-2]) est un type
         * enum/combinaison user connu, le membre apres le point est une VALEUR
         * d'enum, pas une methode. On NE skippe PAS le member access : on emet
         * 'enumMember' (index 0, jaune) pour le membre. Le receiver lui-meme
         * (Statut) n'est pas member access → deja colore 'type' par le bloc
         * rev3 ci-dessus. Ce bloc passe AVANT la logique method pour que
         * `EnumType.x` n'emette jamais un token method (discriminant : receiver
         * est-il un type enum connu ?). */
        if (prevType == TOKEN_DOT && ti >= 2 &&
            allTokens[ti-2].type == TOKEN_IDENTIFIER) {
            char recvName[256];
            int rlen = allTokens[ti-2].length;
            if (rlen >= 256) rlen = 255;
            memcpy(recvName, allTokens[ti-2].start, rlen);
            recvName[rlen] = '\0';
            if (user_enum_lookup(recvName) != NULL) {
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 0;  /* enumMember (membre apres .) */
                tokens[tokenCount].tokenModifiers = 0;
                tokenCount++;
                continue;
            }
        }

        /*  — appel de methode user `obj.Methode(...)` : token 'method'
         * (index 5, cible vert fonce). Resolu uniquement sur une methode d'une
         * classe user (les methodes-formes natives type `<zipArchive>.Ajoute()`
         * ne matchent pas → restent bleu via grammar).
         * CONTRAINTE FORTE : ne JAMAIS emettre pour Constructeur/Destructeur
         * (FR) ni Constructor/Destructor (EN) — ils sont colores orange par la
         * grammar et un token method (vert) les ecraserait. */
        if (prevType == TOKEN_DOT &&
            strcasecmp(tokName, "Constructeur") != 0 &&
            strcasecmp(tokName, "Destructeur")  != 0 &&
            strcasecmp(tokName, "Constructor")  != 0 &&
            strcasecmp(tokName, "Destructor")   != 0 &&
            user_class_method_exists(tokName)) {
            tokens[tokenCount].line = tok.line - 1;
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 5;  /* method (methode user) */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
            continue;
        }

        /*  — appel direct d'une procedure/fonction user non qualifie
         * `Calcule(10, 20)` : token 'method' (index 5, vert fonce), comme le
         * cas qualifie `obj.Methode(...)` traite plus haut. Le scan
         * scan_user_symbols capture deja toutes les PROCEDURE/FONCTION user
         * comme userSymbols[k].kind == 3 (cf. plus haut dans ce fichier).
         * Conditions :
         *   - pas de member access (`obj.Calcule` est deja traite ou est un
         *     field — pas notre affaire)
         *   - suivi de `(` (sinon c'est une reference de fonction, pas un
         *     appel — pour l'instant on s'aligne sur le ticket : seul l'appel
         *     est colore method)
         *   - tokName matche une entree userSymbols avec kind==3
         * Les natives stdlib (Trace, HTTPRequete...) ne sont pas dans
         * userSymbols et restent donc colorees par la grammar TextMate. */
        if (!isMemberAccess && nextType == TOKEN_LEFT_PAREN) {
            bool isUserProc = false;
            /*  — proc/fonction d'un compagnon FERME (index disque) :
             * colore l'appel method d'emblee, sans dependre de l'ouverture du
             * fichier declarant (cas d'une procedure de collection appelee en
             * nom nu depuis un autre fichier du projet). */
            if (disk_proc_lookup(tokName) != NULL) isUserProc = true;
            for (int s = 0; !isUserProc && s < userSymbolCount; s++) {
                if (userSymbols[s].kind == 3 &&
                    strcasecmp(tokName, userSymbols[s].name) == 0) {
                    isUserProc = true;
                    break;
                }
            }
            /*  — Bug B : la declaration `PROCEDURE surClic(`
             * enregistre `surClic` dans userSymbols[kind==3], mais c'est
             * un event officiel, pas une procedure user. NE PAS emettre
             * de token method (vert) : laisser la grammar TextMate prendre
             * la main et colorer bleu (support.function.event.owl). Les
             * procedures user normales (`PROCEDURE maProcPerso()`) ne sont
             * pas dans les catalogues events → bascule pas. */
            if (isUserProc && lsp_is_official_event(tokName)) {
                isUserProc = false;
            }
            /*  — Terminaison n'est un event (donc PAS colore method)
             * QUE dans un bloc CollectionDeProcedures. Hors de ce contexte,
             * c'est une procedure user ordinaire (token method vert). Le
             * contexte DSL n'est calcule que pour les noms du catalogue
             * collection (rares) -> cout nul sur le cas general. */
            if (isUserProc && owl_collection_event_catalog_find(tokName)) {
                int evOff = (int)(tok.start - source);
                if (detect_dsl_context_ex(source, evOff).widget == 'G') {
                    isUserProc = false;
                }
            }
            /*  — nom d'entree `Main` en contexte DECLARATION :
             * la grammar TextMate (owl-integration :
             * syntaxes/owl.tmLanguage.json, pattern procedure-declaration)
             * liste `Main` parmi les entry-points colores
             * support.function.event.owl (bleu), au meme titre que
             * Fermeture/Closing. `Main` n'est dans aucun catalogue d'events
             * (lsp_is_official_event le rate), donc le token method (vert)
             * ci-dessous ecraserait le bleu de la grammar → incoherence.
             * On mirror l'intention de la grammar : sur `PROCEDURE Main` /
             * `FONCTION Main`, on n'emet pas de token method, la grammar
             * reprend la main (bleu). Scope declaration uniquement : un
             * appel `Main()` (prevType != PROCEDURE/FONCTION) reste vert,
             * comme la grammar qui ne colore bleu que la declaration.
             * Couplage cross-repo : le nom `Main` est duplique cote grammar ;
             * ici on reflete cette liste, pas de source partagee. `Main`
             * n'a ni variante accentuee ni forme EN distincte. */
            if (isUserProc && strcasecmp(tokName, "Main") == 0 &&
                (prevType == TOKEN_PROCEDURE || prevType == TOKEN_FONCTION)) {
                isUserProc = false;
            }
            if (isUserProc) {
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 5;  /* method (procedure/fonction user) */
                tokens[tokenCount].tokenModifiers = 0;
                tokenCount++;
                continue;
            }
        }

        /* enumMember : match case-insensitive (canon). Mais on ignore
         * deux contextes ou IDENT n'est PAS un usage de constante :
         *   1. Declaration : 'foo est un/une Type'  → 'foo' est une variable.
         *   2. Member access : 'obj.foo' / 'obj..foo' → field d'instance.
         * Sans ce check, une variable nommee 'actif', 'faux', 'titre'...
         * serait coloree comme la constante canon homonyme. */

        /*  — scope-aware classification (champs classe / params /
         * locales). On consulte scopeAtToken[ti] pre-calcule par la passe 1.5.
         *
         *   - Declaration `IDENT est ...` dans SCOPE_CLASS → property (type=2,
         *     mods=0), cf. cProduit.sNom / cCompte.sTitulaire. PAS cyan.
         *   - Declaration dans SCOPE_PARAMS ou SCOPE_PROC → variable locale
         *     (type=3, mods=0). Distinguer du modifier "global" (mods=1) qui
         *     ne s'applique qu'au top-level. cf. Calcule(nA est un entier)
         *     et le body `nResultat est un entier = ...`.
         *   - Usage d'un IDENT dans SCOPE_PROC/PARAMS qui matche un local
         *     declare dans la meme proc → variable locale (type=3, mods=0).
         *     Sinon on retombe sur les regles globalVar / enumValue ci-dessous. */
        if (!isMemberAccess) {
            int curScope = scopeAtToken[ti];
            if (isDeclaration && curScope == SCOPE_CLASS) {
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 2;  /* property */
                tokens[tokenCount].tokenModifiers = 0;
                tokenCount++;
                continue;
            }
            if (isDeclaration && (curScope == SCOPE_PARAMS || curScope == SCOPE_PROC)) {
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 3;  /* variable */
                tokens[tokenCount].tokenModifiers = 0;  /* local */
                tokenCount++;
                continue;
            }
            /* Usage dans une proc : matche un local declare ? */
            if (!isDeclaration && (curScope == SCOPE_PROC || curScope == SCOPE_PARAMS)) {
                int procIdx = FIND_ENCLOSING_PROC(ti);
                if (procIdx >= 0) {
                    bool isLocal = false;
                    for (int k = 0; k < procLocals[procIdx].localCount; k++) {
                        if (strcasecmp(tokName, procLocals[procIdx].locals[k]) == 0) {
                            isLocal = true;
                            break;
                        }
                    }
                    if (isLocal) {
                        tokens[tokenCount].line = tok.line - 1;
                        tokens[tokenCount].startChar = utf16Col;
                        tokens[tokenCount].length = tok_utf16_length(tok);
                        tokens[tokenCount].tokenType = 3;
                        tokens[tokenCount].tokenModifiers = 0;
                        tokenCount++;
                        continue;
                    }
                }
            }
        }

        /* Variable user (kind==6) : on colore AUSSI a la declaration (canon :
         * gMaVar visible en bleu des sa decl `gMaVar est un entier`).
         * Member access skippe (s'il existe une struct.gMaVar c'est un field).
         *
         *  — une var GLOBALE (top-level Fenetre/fichier) porte le
         * modifier global (mods=1, cyan) ; une var LOCALE a un bloc widget
         * (isGlobal=false cote scan) reste un token variable SANS ce modifier
         * (mods=0). Avant, seules les globales etaient emises ici : une var de
         * bloc widget, une fois reclassee non-globale, ne recevait plus AUCUN
         * token. On emet donc le token dans les deux cas, le modifier reflete
         * la globalite. (Les params/locales de procedure sont deja traites plus
         * haut par le scope-stack et ne parviennent pas ici.) */
        if (!isMemberAccess) {
            bool matchGlobal = false, matchLocal = false;
            for (int s = 0; s < userSymbolCount; s++) {
                if (userSymbols[s].kind == 6 &&
                    strcasecmp(tokName, userSymbols[s].name) == 0) {
                    if (userSymbols[s].isGlobal) matchGlobal = true;
                    else                         matchLocal = true;
                }
            }
            /*  — globale d'instance d'un compagnon FERME : absente de
             * userSymbols (docs ouverts only), presente dans l'index disque.
             * On la colore comme une variable globale (cyan), coherent avec
             * la meme ref quand le compagnon est ouvert. */
            if (!matchGlobal && !matchLocal && disk_global_lookup(tokName)) {
                matchGlobal = true;
            }
            if (matchGlobal || matchLocal) {
                tokens[tokenCount].line = tok.line - 1;
                tokens[tokenCount].startChar = utf16Col;
                tokens[tokenCount].length = tok_utf16_length(tok);
                tokens[tokenCount].tokenType = 3;     /* variable */
                tokens[tokenCount].tokenModifiers = matchGlobal ? 1 : 0;
                tokenCount++;
                continue;
            }
        }

        /* enumMember user : on continue de skip declaration et member access
         * (sinon `enumTyp1` dans la decl ENUMERATION serait colore alors
         * qu'il est en cours de DEFINITION, pas d'usage). */
        if (isDeclaration || isMemberAccess) continue;

        bool isEnumValue = false;
        for (int e = 0; e < enumValueCount; e++) {
            if (strcasecmp(tokName, enumValues[e].name) == 0) {
                isEnumValue = true;
                break;
            }
        }

        if (isEnumValue) {
            tokens[tokenCount].line = tok.line - 1;  /* 0-based */
            tokens[tokenCount].startChar = utf16Col;
            tokens[tokenCount].length = tok_utf16_length(tok);
            tokens[tokenCount].tokenType = 0;  /* enumMember is index 0 in our legend */
            tokens[tokenCount].tokenModifiers = 0;
            tokenCount++;
        }
    }

    /* Convert to LSP semantic tokens format (delta-encoded) */
    JsonValue* data = json_new_array();
    int prevLine = 0, prevStartChar = 0;

    for (int i = 0; i < tokenCount; i++) {
        int deltaLine = tokens[i].line - prevLine;
        int deltaStart = (deltaLine == 0) ? (tokens[i].startChar - prevStartChar) : tokens[i].startChar;

        json_array_push(data, json_new_int(deltaLine));
        json_array_push(data, json_new_int(deltaStart));
        json_array_push(data, json_new_int(tokens[i].length));
        json_array_push(data, json_new_int(tokens[i].tokenType));
        json_array_push(data, json_new_int(tokens[i].tokenModifiers));

        prevLine = tokens[i].line;
        prevStartChar = tokens[i].startChar;
    }

    JsonValue* result = json_new_object();
    json_object_set(result, "data", data);

    int64_t _dt_st = LSP_NOW_MS() - _t_st;
    if (_dt_st >= 5) {
        lsp_log("[timing] handle_semantic_tokens: %lldms (tokens=%d)",
                (long long)_dt_st, tokenCount);
    }
    return result;
}

/* ================================================================
 * GO TO DEFINITION
 * ================================================================ */

/*  — Construit une Location LSP pour une declaration user.
 * Deux sources d'URI :
 *   - docIndex >= 0 : document ouvert → documents[docIndex].uri.
 *   - docIndex < 0  : declaration captee depuis un fichier disque NON ouvert
 *     (FS walk) → `fileUri` (URI file:// du fichier source). Permet le
 *     go-to-definition cross-fichier vers un .owl pas ouvert dans l'editeur.
 * Renvoie NULL si aucune source d'URI exploitable (docIndex invalide ET
 * fileUri vide). */
static JsonValue* def_make_location(int docIndex, const char* fileUri,
                                    int line, int character, int nameLen) {
    const char* uri = NULL;
    if (docIndex >= 0 && docIndex < lspServer.documentCount) {
        uri = lspServer.documents[docIndex].uri;
    } else if (fileUri && fileUri[0] != '\0') {
        uri = fileUri;
    }
    if (!uri) return NULL;
    JsonValue* location = json_new_object();
    json_object_set(location, "uri", json_new_string(uri));
    JsonValue* range = json_new_object();
    JsonValue* start = json_new_object();
    json_object_set(start, "line", json_new_int(line));
    json_object_set(start, "character", json_new_int(character));
    JsonValue* end = json_new_object();
    json_object_set(end, "line", json_new_int(line));
    json_object_set(end, "character", json_new_int(character + nameLen));
    json_object_set(range, "start", start);
    json_object_set(range, "end", end);
    json_object_set(location, "range", range);
    return location;
}

/*  — Extrait l'identifiant receiver d'un acces membre `recv.word`
 * ou `recv:word` (ou `recv..word`) sur la ligne `line`, le mot etant a la
 * colonne `character`. Renvoie la longueur du receiver (0 si pas d'acces
 * membre detecte). Le receiver permet de resoudre le type du membre. */
static int def_extract_receiver(const char* src, int srcLen, int line, int character,
                                char* out, int outSize) {
    int curLine = 0, lineStart = 0;
    for (int i = 0; i < srcLen && curLine < line; i++) {
        if (src[i] == '\n') { curLine++; lineStart = i + 1; }
    }
    int wordStart = lineStart + character;
    if (wordStart > srcLen) return 0;
    /* Remonter au debut du mot sous le curseur. */
    while (wordStart > lineStart && IS_ID_BYTE(src[wordStart - 1])) wordStart--;
    /* Sauter les separateurs d'acces membre ('.' ou ':') juste avant. */
    int p = wordStart;
    while (p > lineStart && (src[p - 1] == '.' || src[p - 1] == ':')) p--;
    if (p == wordStart) return 0; /* pas de separateur → pas un acces membre */
    int recvEnd = p;
    int recvStart = recvEnd;
    while (recvStart > lineStart && IS_ID_BYTE(src[recvStart - 1])) recvStart--;
    int len = recvEnd - recvStart;
    if (len <= 0 || len >= outSize) return 0;
    memcpy(out, src + recvStart, len);
    out[len] = '\0';
    return len;
}

/*  — extrait le receveur de 2e niveau d'un acces `A.B.word` (ou `:`),
 * c.-a-d. `A`, le receveur du receveur immediat. Renvoie sa longueur, 0 si
 * l'acces n'a pas deux niveaux sur la ligne. Sert a scoper la resolution d'un
 * membre d'enum qualifie `COL.Enum.membre` a la collection `COL`. */
static int def_extract_receiver2(const char* src, int srcLen, int line, int character,
                                 char* out, int outSize) {
    int curLine = 0, lineStart = 0;
    for (int i = 0; i < srcLen && curLine < line; i++) {
        if (src[i] == '\n') { curLine++; lineStart = i + 1; }
    }
    int wordStart = lineStart + character;
    if (wordStart > srcLen) return 0;
    while (wordStart > lineStart && IS_ID_BYTE(src[wordStart - 1])) wordStart--;
    /* separateur avant le mot */
    int p = wordStart;
    while (p > lineStart && (src[p - 1] == '.' || src[p - 1] == ':')) p--;
    if (p == wordStart) return 0;
    /* receveur immediat (B) */
    int recv1Start = p;
    while (recv1Start > lineStart && IS_ID_BYTE(src[recv1Start - 1])) recv1Start--;
    if (recv1Start == p) return 0;
    /* separateur avant le receveur immediat */
    int q = recv1Start;
    while (q > lineStart && (src[q - 1] == '.' || src[q - 1] == ':')) q--;
    if (q == recv1Start) return 0;  /* un seul niveau : pas qualifie collection */
    /* receveur de 2e niveau (A) */
    int recv2Start = q;
    while (recv2Start > lineStart && IS_ID_BYTE(src[recv2Start - 1])) recv2Start--;
    int len = q - recv2Start;
    if (len <= 0 || len >= outSize) return 0;
    memcpy(out, src + recv2Start, len);
    out[len] = '\0';
    return len;
}

JsonValue* handle_definition(JsonValue* params) {
    /*  — Trigger lazy le scan workspace cross-file (gain 1.5s
     * startup, voir scan_user_symbols_force). go-to-def cross-file en
     * a besoin. Idempotent. */
    lsp_ensure_project_scan();
    /*  — Rafraichit le registre des symboles ouverts (userSymbols,
     * userClasses, userStructs, userEnums, userInterfaces) depuis les buffers
     * courants. Idempotent via cache fingerprint : no-op si rien n'a change. */
    scan_user_symbols();
    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (!td || !pos) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);

    int idx = find_doc(uri);
    if (idx < 0) return json_new_null();

    char word[256];
    int wordLen = extract_word_at(lspServer.documents[idx].content,
                                   lspServer.documents[idx].contentLen,
                                   line, character, word, sizeof(word));
    if (wordLen <= 0) return json_new_null();

    /*  — inference de l'axe namespace au site d'usage :
     * un mot precede de `.` / `:` / `..` est un acces MEMBRE, resolu par le
     * chemin recepteur->type plus bas (section (e)). Sinon axe VALUE, servi
     * en priorite par le moteur de portee scope-correct, avec repli sur la
     * resolution plate (userSymbols) + les index disque cross-fichier. Les
     * trois axes ne sont JAMAIS fusionnes (invariant 5, ). */
    char defRecvProbe[256];
    bool isMemberAccess = def_extract_receiver(lspServer.documents[idx].content,
                                               lspServer.documents[idx].contentLen,
                                               line, character,
                                               defRecvProbe, sizeof(defRecvProbe)) > 0;

    if (!isMemberAccess) {
        /* Moteur de portee : occurrence -> declaration lexicale precise. Deux
         * procedures sœurs declarant chacune un `x` ne sont plus confondues ;
         * une variable accentuee referencee sans accent resout via canonKey. */
        ScopeResolveResult sr;
        if (scope_resolve_at(uri, line, character, word, &sr) && sr.found) {
            JsonValue* loc = def_make_location(sr.docIndex, NULL,
                                               sr.line, sr.character, sr.nameLen);
            if (loc) return loc;
        }

        /* Repli 1 : resolution plate par nom sur les docs ouverts (globale
         * cross-fichier d'un compagnon OUVERT, mono-homonyme). */
        for (int i = 0; i < userSymbolCount; i++) {
            if (strcasecmp(word, userSymbols[i].name) == 0) {
                JsonValue* location = json_new_object();
                json_object_set(location, "uri",
                    json_new_string(lspServer.documents[userSymbols[i].docIndex].uri));

                JsonValue* range = json_new_object();
                JsonValue* start = json_new_object();
                json_object_set(start, "line", json_new_int(userSymbols[i].line));
                json_object_set(start, "character", json_new_int(userSymbols[i].character));
                JsonValue* end = json_new_object();
                json_object_set(end, "line", json_new_int(userSymbols[i].line));
                json_object_set(end, "character",
                    json_new_int(userSymbols[i].character + (int)strlen(userSymbols[i].name)));
                json_object_set(range, "start", start);
                json_object_set(range, "end", end);
                json_object_set(location, "range", range);

                return location;
            }
        }
    }

    /* Index disque cross-fichier : servent aussi bien un usage VALUE nu qu'un
     * acces en SYNTAXE membre resolvant vers un global de compagnon FERME
     * (ex `COL_Utils.gMode` -> global d'une collection non ouverte). On ne les
     * garde donc PAS derriere le filtre isMemberAccess (contrairement a la
     * boucle plate userSymbols, court-circuitee pour laisser un vrai membre de
     * classe atteindre la resolution recepteur->type).
     *
     *  — globale d'instance d'un compagnon NON ouvert (nom + fileUri +
     * position). Dedup au grain fichier : le scan disque skip les fichiers
     * ouverts (priorite buffer preservee). Avant la resolution POO : une
     * globale est une variable, prioritaire sur les noms de types. */
    {
        DiskGlobalInfo* g = disk_global_lookup(word);
        if (g) {
            JsonValue* loc = def_make_location(-1, g->fileUri, g->line,
                                               g->character, (int)strlen(g->name));
            if (loc) return loc;
        }
    }

    /*  — procedure/fonction declaree dans un compagnon FERME (index
     * disque). Sans cela, un appel `Doubler(...)` d'une procedure de collection
     * ne resolvait pas tant que le compagnon n'etait pas ouvert. */
    {
        DiskProcInfo* dp = disk_proc_lookup(word);
        if (dp) {
            JsonValue* loc = def_make_location(-1, dp->fileUri, dp->line,
                                               dp->character, (int)strlen(dp->name));
            if (loc) return loc;
        }
    }

    /*  — Resolution POO. Ordre : classes → structs/enums/interfaces
     * (noms de types) → membre (classe du receveur ou englobante, puis
     * remontee de la hierarchie via baseClass). */

    /* (a) Nom de classe → declaration. */
    {
        UserClassInfo* ci = user_class_lookup(word);
        if (ci) {
            JsonValue* loc = def_make_location(ci->docIndex, ci->fileUri,
                                               ci->classLine, 0,
                                               (int)strlen(ci->name));
            if (loc) return loc;
        }
    }

    /* (b) Nom de structure. */
    {
        UserStructInfo* si = user_struct_lookup(word);
        if (si) {
            JsonValue* loc = def_make_location(si->docIndex, NULL, si->line, si->character,
                                               (int)strlen(si->name));
            if (loc) return loc;
        }
    }

    /* (c) Nom d'enumeration / combinaison.  : passe ei->fileUri pour
     * resoudre aussi vers un compagnon FERME (docIndex<0). */
    {
        UserEnumInfo* ei = user_enum_lookup(word);
        if (ei) {
            JsonValue* loc = def_make_location(ei->docIndex, ei->fileUri,
                                               ei->line, ei->character,
                                               (int)strlen(ei->name));
            if (loc) return loc;
        }
    }

    /* (c2)  — MEMBRE d'enumeration/combinaison (`actif`, `inactif`,
     * `rouge`...) → sa ligne de definition. Couvre le go-to-def sur un membre
     * qualifie `COL.Statut.actif` comme sur un membre nu.
     *
     * Scope collection : quand l'acces est qualifie par une collection
     * (`COL_X.Enum.membre`), on restreint la resolution aux enums declares
     * DANS cette collection (ownerCollection == COL_X). Sans ce filtre, deux
     * collections declarant un enum homonyme (`Statut`) renverraient toutes
     * deux vers la premiere trouvee, ignorant le prefixe `COL_X`. En acces nu
     * (`Statut.actif`) ou qualifie par autre chose qu'une collection, le
     * receveur collection est vide -> comportement inchange (1er homonyme). */
    {
        char collRecv[128];
        int collLen = def_extract_receiver2(lspServer.documents[idx].content,
                                            lspServer.documents[idx].contentLen,
                                            line, character, collRecv, sizeof(collRecv));
        bool scopeToCollection = (collLen > 0
                                  && collection_def_lookup(collRecv) != NULL);
        for (int e = 0; e < userEnumCount; e++) {
            UserEnumInfo* ei = &userEnums[e];
            if (scopeToCollection
                && strcasecmp(ei->ownerCollection, collRecv) != 0) continue;
            for (int v = 0; v < ei->valueCount; v++) {
                if (strcasecmp(word, ei->values[v]) != 0) continue;
                JsonValue* loc = def_make_location(ei->docIndex, ei->fileUri,
                                                   ei->valueLine[v], ei->valueChar[v],
                                                   (int)strlen(ei->values[v]));
                if (loc) return loc;
            }
        }
    }

    /* (d) Nom d'interface. */
    for (int i = 0; i < userInterfaceCount; i++) {
        if (strcasecmp(word, userInterfaces[i].name) == 0) {
            JsonValue* loc = def_make_location(userInterfaces[i].docIndex, NULL,
                                               userInterfaces[i].line,
                                               userInterfaces[i].character,
                                               (int)strlen(userInterfaces[i].name));
            if (loc) return loc;
            break;
        }
    }

    /*  — Nom de Fenetre/Etat -> sa declaration `X est une Fenetre`
     * / `X est un Etat`. windowDefs[] porte la position + la source (doc
     * ouvert via docIndex, compagnon disque FERME via fileUri), meme
     * priorite buffer que les classes/globales cross-fichier. */
    {
        WindowDefInfo* w = window_def_lookup(word);
        if (w) {
            JsonValue* loc = def_make_location(w->docIndex, w->fileUri,
                                               w->line, w->character,
                                               (int)strlen(w->name));
            if (loc) return loc;
        }
    }

    /*  — Nom de bloc CollectionDeProcedures -> sa declaration
     * `COL est une CollectionDeProcedures`, cross-file (compagnon ferme via
     * fileUri) comme les Fenetres/Etats. */
    {
        CollectionDefInfo* c = collection_def_lookup(word);
        if (c) {
            JsonValue* loc = def_make_location(c->docIndex, c->fileUri,
                                               c->line, c->character,
                                               (int)strlen(c->name));
            if (loc) return loc;
        }
    }

    /* (e) Membre (champ/methode). On determine la classe de contexte :
     *   - via le type du receveur `recv.word` / `recv:word` (recv declare
     *     `recv est un cClass` → userSymbols.detail porte le type) ;
     *   - sinon via la classe englobant le curseur (appel nu dans un body).
     * Puis on cherche le membre dans la classe ; s'il est absent, on remonte
     * la hierarchie via baseClass (membre herite). */
    {
        UserClassInfo* ctxClass = NULL;

        char recv[256];
        int recvLen = def_extract_receiver(lspServer.documents[idx].content,
                                           lspServer.documents[idx].contentLen,
                                           line, character, recv, sizeof(recv));
        if (recvLen > 0) {
            /* Type du receveur : variable declaree (detail = type) ou nom de
             * classe directement (acces statique cClass.membre). */
            for (int i = 0; i < userSymbolCount; i++) {
                if (userSymbols[i].kind == 6 &&
                    strcasecmp(userSymbols[i].name, recv) == 0) {
                    ctxClass = user_class_lookup(userSymbols[i].detail);
                    if (ctxClass) break;
                }
            }
            if (!ctxClass) ctxClass = user_class_lookup(recv);
        }

        if (!ctxClass) ctxClass = user_class_enclosing(idx, line);

        /* Remontee hierarchie : on borne la profondeur pour eviter toute
         * boucle sur un cycle d'heritage mal forme. */
        UserClassInfo* c = ctxClass;
        for (int hops = 0; c && hops < 64; hops++) {
            for (int m = 0; m < c->memberCount; m++) {
                if (strcasecmp(c->members[m].name, word) == 0) {
                    /* Classe disque (docIndex<0) : la Location passe par
                     * c->fileUri ; doc ouvert : par documents[docIndex].uri. */
                    JsonValue* loc = def_make_location(c->docIndex, c->fileUri,
                                                       c->members[m].line,
                                                       c->members[m].character,
                                                       (int)strlen(c->members[m].name));
                    if (loc) return loc;
                    /* Membre trouve mais aucune source d'URI exploitable :
                     * on arrete (null propre, pas de remontee plus haut). */
                    return json_new_null();
                }
            }
            if (c->baseClass[0] == '\0') break;
            c = user_class_lookup(c->baseClass);
        }
    }

    return json_new_null();
}

/*  Lot 1 ( §3, §4) -- textDocument/references scope-correct.
 * Collecte les occurrences liees (declaration + usages) d'une valeur via le
 * moteur de portee lsp_scope : les homonymes de procedures sœurs restent
 * isoles. Cross-fichier hors perimetre de ce lot. */
#define SCOPE_REFS_MAX 512  /* plafond de Location renvoyees en une reponse */
JsonValue* handle_references(JsonValue* params) {
    /* Idempotents (cache fingerprint) : rebuild seulement si un buffer change. */
    lsp_ensure_project_scan();
    scan_user_symbols();
    scope_build();

    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (!td || !pos) return json_new_array();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);

    int idx = find_doc(uri);
    if (idx < 0) return json_new_array();

    /* Garde : hors d'un identifiant -> pas de references. */
    char word[256];
    if (extract_word_at(lspServer.documents[idx].content,
                        lspServer.documents[idx].contentLen,
                        line, character, word, sizeof(word)) <= 0)
        return json_new_array();

    JsonValue* context = json_get(params, "context");
    bool includeDeclaration = context
        ? json_get_bool(context, "includeDeclaration", true) : true;

    ScopeReference refs[SCOPE_REFS_MAX];
    int nRefs = scope_references_of(uri, line, character, includeDeclaration,
                                    refs, SCOPE_REFS_MAX);

    JsonValue* result = json_new_array();
    for (int i = 0; i < nRefs; i++) {
        JsonValue* loc = def_make_location(refs[i].docIndex, NULL,
                                           refs[i].line, refs[i].character,
                                           refs[i].nameLen);
        if (loc) json_array_push(result, loc);
    }
    return result;
}

/* Construit un Range LSP { start:{line,char}, end:{line,char+nameLen} }
 * couvrant un mot sur une seule ligne (colonnes octet, comme def_make_location). */
static JsonValue* rename_make_range(int line, int character, int nameLen) {
    JsonValue* start = json_new_object();
    json_object_set(start, "line", json_new_int(line));
    json_object_set(start, "character", json_new_int(character));
    JsonValue* end = json_new_object();
    json_object_set(end, "line", json_new_int(line));
    json_object_set(end, "character", json_new_int(character + nameLen));
    JsonValue* range = json_new_object();
    json_object_set(range, "start", start);
    json_object_set(range, "end", end);
    return range;
}

/* Ajoute au WorkspaceEdit `changes` une entree { uri: [TextEdit...] } a partir
 * des occurrences `occ` (chacune -> un TextEdit, newText = newName VERBATIM).
 * No-op si aucune occurrence ou si l'URI est deja present (evite un doublon
 * quand un fichier serait atteint par deux chemins). */
static void rename_add_edits_for_uri(JsonValue* changes, const char* uri,
                                     const ScopeReference* occ, int nOcc,
                                     const char* newName) {
    if (nOcc <= 0 || !uri || !uri[0]) return;
    if (json_get(changes, uri) != NULL) return;
    JsonValue* edits = json_new_array();
    for (int i = 0; i < nOcc; i++) {
        JsonValue* edit = json_new_object();
        json_object_set(edit, "range",
                        rename_make_range(occ[i].line, occ[i].character,
                                          occ[i].nameLen));
        json_object_set(edit, "newText", json_new_string(newName));
        json_array_push(edits, edit);
    }
    json_object_set(changes, uri, edits);
}

/*  Lot 2b — walk disque des .owl FERMES d'un scope projet ( :
 * recursif seulement en mode PROJECT) pour collecter les occurrences VALUE de la
 * globale `canonKey`. Miroir de scan_workspace_files_for_user_symbols (meme
 * bornage profondeur / taille / skip build,.git). Les fichiers deja ouverts sont
 * skippes (le buffer prime, traite par l'appelant). */
static void rename_walk_dir_for_global(const char* dir, int depth, bool recursive,
                                       const char* canonKey, const char* newName,
                                       JsonValue* changes) {
    if (depth > 6) return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!recursive) continue;
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            rename_walk_dir_for_global(path, depth + 1, recursive,
                                       canonKey, newName, changes);
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl < 5 || strcmp(e->d_name + nl - 4, ".owl") != 0) continue;
        if (path_matches_open_doc(path)) continue;  /* buffer prime */
        if (st.st_size <= 0 || st.st_size > 1024 * 1024) continue;
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        char* buf = (char*)malloc((size_t)st.st_size + 1);
        if (!buf) { fclose(f); continue; }
        size_t r = fread(buf, 1, (size_t)st.st_size, f);
        fclose(f);
        buf[r] = '\0';
        ScopeReference occ[SCOPE_REFS_MAX];
        int n = scope_collect_value_occurrences(buf, canonKey, occ, SCOPE_REFS_MAX);
        free(buf);
        if (n > 0) {
            char uri[1024];
            filepath_to_uri(path, uri, sizeof(uri));
            rename_add_edits_for_uri(changes, uri, occ, n, newName);
        }
    }
    closedir(d);
}

/*  Lot 2b — rename d'une globale sur TOUT le projet ( : une
 * globale VALUE est unique projet-wide, toutes ses occurrences non-membres sont
 * le meme symbole). Collecte : (a) chaque buffer OUVERT via son contenu ;
 * (b) chaque compagnon FERME via un walk disque borne a la frontiere projet
 *. Remplit `changes` { uri: [TextEdit...] }. */
static void rename_collect_global(const char* canonKey, const char* newName,
                                  JsonValue* changes) {
    ScopeReference occ[SCOPE_REFS_MAX];

    /* (a) documents ouverts : le buffer prime (peut etre non sauve). */
    for (int dd = 0; dd < lspServer.documentCount; dd++) {
        if (!doc_uri_is_owl_source(lspServer.documents[dd].uri)) continue;
        int n = scope_collect_value_occurrences(lspServer.documents[dd].content,
                                                canonKey, occ, SCOPE_REFS_MAX);
        rename_add_edits_for_uri(changes, lspServer.documents[dd].uri,
                                 occ, n, newName);
    }

    /* (b) compagnons fermes : walk disque par scope projet. */
    char roots[LSP_SCAN_SCOPES_MAX][1024];
    bool recursive[LSP_SCAN_SCOPES_MAX];
    int nScopes = collect_open_doc_scopes(roots, recursive, LSP_SCAN_SCOPES_MAX);
    for (int i = 0; i < nScopes; i++) {
        rename_walk_dir_for_global(roots[i], 0, recursive[i],
                                   canonKey, newName, changes);
    }
}

/*  ( §3, §4) — textDocument/rename scope-aware. Construit un
 * WorkspaceEdit { changes: { uri: [TextEdit...] } }, newText = newName VERBATIM
 * (la canonicalisation revient au formatter au save). Locale / parametre :
 * mono-fichier (Lot 2a). Globale : PROJET entier, compagnons fermes inclus
 * (Lot 2b, /). Membre, native, mot-cle -> null (refuses). */
JsonValue* handle_rename(JsonValue* params) {
    lsp_ensure_project_scan();
    scan_user_symbols();
    scope_build();

    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    const char* newName = json_get_string(params, "newName");
    if (!td || !pos || !newName || !newName[0]) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);
    if (!uri) return json_new_null();

    bool renamable = false, isGlobal = false;
    int nameLen = 0;
    char canonKey[128];
    ScopeReference refs[SCOPE_REFS_MAX];
    int nRefs = scope_rename_query(uri, line, character, &renamable, &nameLen,
                                   &isGlobal, canonKey, (int)sizeof(canonKey),
                                   refs, SCOPE_REFS_MAX);
    if (!renamable || nRefs <= 0) return json_new_null();

    JsonValue* changes = json_new_object();
    if (isGlobal) {
        /* Projet entier :  rend la globale unique projet-wide. */
        rename_collect_global(canonKey, newName, changes);
    } else {
        /* Locale / parametre : mono-fichier (occurrences du doc courant). */
        JsonValue* edits = json_new_array();
        for (int i = 0; i < nRefs; i++) {
            JsonValue* edit = json_new_object();
            json_object_set(edit, "range",
                            rename_make_range(refs[i].line, refs[i].character,
                                              refs[i].nameLen));
            json_object_set(edit, "newText", json_new_string(newName));
            json_array_push(edits, edit);
        }
        json_object_set(changes, uri, edits);
    }

    JsonValue* result = json_new_object();
    json_object_set(result, "changes", changes);
    return result;
}

/*  — textDocument/prepareRename : autorise le renommage
 * d'une locale / parametre / globale user-declared (Lot 2b) en renvoyant le
 * Range du mot sous le curseur ; refus (null) pour membre, native, mot-cle. */
JsonValue* handle_prepare_rename(JsonValue* params) {
    lsp_ensure_project_scan();
    scan_user_symbols();
    scope_build();

    JsonValue* td = json_get(params, "textDocument");
    JsonValue* pos = json_get(params, "position");
    if (!td || !pos) return json_new_null();

    const char* uri = json_get_string(td, "uri");
    int line = (int)json_get_int(pos, "line", 0);
    int character = (int)json_get_int(pos, "character", 0);
    if (!uri) return json_new_null();

    bool renamable = false;
    int nameLen = 0;
    ScopeReference refs[SCOPE_REFS_MAX];
    int nRefs = scope_rename_query(uri, line, character, &renamable, &nameLen,
                                   NULL, NULL, 0, refs, SCOPE_REFS_MAX);
    if (!renamable || nRefs <= 0) return json_new_null();

    /* Range de l'occurrence couvrant le curseur (le curseur peut tomber au
     * milieu du mot) ; a defaut, la premiere occurrence. */
    int rLine = refs[0].line, rChar = refs[0].character;
    for (int i = 0; i < nRefs; i++) {
        if (refs[i].line == line &&
            character >= refs[i].character &&
            character < refs[i].character + refs[i].nameLen) {
            rLine = refs[i].line;
            rChar = refs[i].character;
            break;
        }
    }
    return rename_make_range(rLine, rChar, nameLen);
}

/* ================================================================
 * COMPILER DIAGNOSTICS -- run owl --check on save
 * ================================================================ */

static void filepath_to_uri(const char* path, char* outUri, int outSize) {
    int i = 0;
    /* Windows: C:\foo\bar → file:///C:/foo/bar */
#ifdef _WIN32
    i += snprintf(outUri, outSize, "file:///");
#else
    i += snprintf(outUri, outSize, "file://");
#endif
    for (const char* p = path; *p && i < outSize - 4; p++) {
        unsigned char c = (unsigned char)*p;
        if (*p == '\\') {
            outUri[i++] = '/';
        } else if (c <= 0x20 || c > 0x7E || *p == ' ' || *p == '#' || *p == '%' || *p == '?') {
            i += snprintf(outUri + i, outSize - i, "%%%02X", c);
        } else {
            outUri[i++] = *p;
        }
    }
    outUri[i] = '\0';
}

static void uri_to_filepath(const char* uri, char* outPath, int outSize) {
    const char* p = uri;
    if (strncmp(p, "file:///", 8) == 0) p += 8;
    else if (strncmp(p, "file://", 7) == 0) p += 7;

    int i = 0;
    while (*p && i < outSize - 1) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], '\0' };
            outPath[i++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            outPath[i++] = *p++;
        }
    }
    outPath[i] = '\0';
}

/*  — etendue UTF-16 du mot en erreur a (line1based, col1based) sur
 * `content`. La colonne d'un diagnostic --check est un offset OCTET 1-based
 * (le scanner OWL compte ses colonnes par octet, pas par point de code ni par
 * unite UTF-16). On etend a droite tant qu'on lit des octets d'identifiant,
 * puis on convertit debut ET fin en unites UTF-16 (encodage de position du
 * protocole LSP). Renvoie false — laissant `make_lsp_diag` sur son repli
 * 1-char — si le buffer est absent, la ligne hors bornes, la position hors
 * d'un mot (operateur/ponctuation), ou le mot tient sur un seul octet. */
static bool compute_word_range_utf16(const char* content, int line1based,
                                     int col1based, int* outStartChar,
                                     int* outEndChar) {
    if (!content) return false;
    int l = line1based > 0 ? line1based - 1 : 0;
    int c = col1based  > 0 ? col1based  - 1 : 0;   /* offset OCTET dans la ligne */
    int i = 0, curLine = 0;
    while (content[i] && curLine < l) {
        if (content[i] == '\n') curLine++;
        i++;
    }
    if (curLine < l) return false;                 /* ligne au-dela du buffer */
    const char* lineStart = content + i;
    int lineLen = 0;
    while (lineStart[lineLen] && lineStart[lineLen] != '\n') lineLen++;
    if (c >= lineLen) return false;                /* colonne hors ligne */
    if (!IS_ID_BYTE(lineStart[c])) return false;   /* pas sur un mot */
    int endByte = c;
    while (endByte < lineLen && IS_ID_BYTE(lineStart[endByte])) endByte++;
    if (endByte - c <= 1) return false;            /* mot d'un octet -> defaut */
    int startU16 = utf8_byte_offset_to_utf16_col(lineStart, c);
    int endU16   = utf8_byte_offset_to_utf16_col(lineStart, endByte);
    if (endU16 <= startU16) return false;
    *outStartChar = startU16;
    *outEndChar   = endU16;
    return true;
}

/*  — hook de test : end.character UTF-16 du mot a (line,col), -1 si
 * repli. Expose la logique pure sans passer par le pipeline de publication. */
int lsp_diag_word_end_char(const char* content, int line1based, int col1based) {
    int s, e;
    if (compute_word_range_utf16(content, line1based, col1based, &s, &e)) return e;
    return -1;
}

/*  (reouverture) — normalise un chemin disque pour comparaison
 * tolerante : slashes unifies (`\` -> `/`) et casse repliee (le FS Windows est
 * casse-insensible, et VSCode envoie parfois `c:` la ou --check derive `C:`). */
static void diag_normalize_path(const char* in, char* out, int outSize) {
    int i = 0;
    for (const char* p = in; *p && i < outSize - 1; p++) {
        char c = *p;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i++] = c;
    }
    out[i] = '\0';
}

/*  (reouverture) — resolution de document tolerante a l'encodage
 * d'URI. VSCode percent-encode le `:` du drive (file:///c%3A/...) alors que
 * `filepath_to_uri` (chemin --check multi-fichiers) le laisse cru
 * (file:///c:/...) : un strcmp exact (find_doc) echoue, le buffer ouvert n'est
 * pas retrouve et la plage du mot n'est jamais calculee (repli 1-char dans le
 * vrai editeur). On decode donc les deux cotes en chemin disque via
 * `uri_to_filepath` (qui gere le %XX general) puis on compare apres
 * normalisation. Scope: uniquement le chemin word-range, `find_doc` (utilise
 * partout ailleurs) reste inchange. Le fast-path exact evite tout cout dans le
 * cas nominal. */
static int find_doc_uri_tolerant(const char* uri) {
    int d = find_doc(uri);
    if (d >= 0) return d;
    char wantPath[1024], wantNorm[1024];
    uri_to_filepath(uri, wantPath, sizeof(wantPath));
    diag_normalize_path(wantPath, wantNorm, sizeof(wantNorm));
    for (int i = 0; i < lspServer.documentCount; i++) {
        char docPath[1024], docNorm[1024];
        uri_to_filepath(lspServer.documents[i].uri, docPath, sizeof(docPath));
        diag_normalize_path(docPath, docNorm, sizeof(docNorm));
        if (strcmp(wantNorm, docNorm) == 0) return i;
    }
    return -1;
}

/*  — resout l'etendue UTF-16 du mot pour un diagnostic (line,col) en
 * cherchant le buffer ouvert de `uri`. Repli -1/-1 si le fichier n'est pas
 * ouvert (cas d'un voisin non charge lors d'un check projet) ou hors mot. */
static void diag_word_range_for_uri(const char* uri, int line, int col,
                                    int* outStart, int* outEnd) {
    *outStart = -1;
    *outEnd = -1;
    int d = find_doc_uri_tolerant(uri);
    if (d < 0) return;
    compute_word_range_utf16(lspServer.documents[d].content, line, col,
                             outStart, outEnd);
}

static JsonValue* make_lsp_diag(int line1based, int col1based, const char* msg,
                                int severity, int startCharOverride,
                                int endCharOverride) {
    int l = line1based > 0 ? line1based - 1 : 0;
    int c = col1based  > 0 ? col1based  - 1 : 0;
    /*  — plage etendue au mot si fournie (UTF-16) ; sinon repli
     * historique 1-char (start = col-1, end = start+1). */
    int startCh = startCharOverride >= 0 ? startCharOverride : c;
    int endCh   = endCharOverride > startCh ? endCharOverride : startCh + 1;
    JsonValue* diag  = json_new_object();
    JsonValue* range = json_new_object();
    JsonValue* start = json_new_object();
    JsonValue* end   = json_new_object();
    json_object_set(start, "line",      json_new_int(l));
    json_object_set(start, "character", json_new_int(startCh));
    json_object_set(end,   "line",      json_new_int(l));
    json_object_set(end,   "character", json_new_int(endCh));
    json_object_set(range, "start", start);
    json_object_set(range, "end",   end);
    json_object_set(diag, "range",    range);
    json_object_set(diag, "severity", json_new_int(severity));
    json_object_set(diag, "source",   json_new_string("owl"));
    json_object_set(diag, "message",  json_new_string(msg));
    return diag;
}

/*  — spy de la garde scheme (cf. lsp_server.h). */
static int lsp__diag_scheme_guard_fired = 0;
int lsp_diag_scheme_was_rejected(void) { return lsp__diag_scheme_guard_fired; }
void lsp_diag_scheme_reset(void) { lsp__diag_scheme_guard_fired = 0; }

/*  Lot 2 — construit l'overlay JSON {"overlays":[{path,content}]} à
 * partir de TOUS les buffers ouverts (scheme file://). `path` = chemin disque
 * du document (via uri_to_filepath), `content` = son buffer courant (potentiel-
 * lement non-sauvé). Le check canonicalise les paths des deux côtés, on passe
 * donc le chemin tel quel. Renvoie une chaîne malloc'd (à free par l'appelant)
 * ou NULL si aucun document overlayable (→ le check retombe sur le disque,
 * comportement Lot 1 inchangé). */
static char* build_open_buffers_overlay(void) {
    JsonValue* arr = json_new_array();
    int n = 0;
    for (int d = 0; d < lspServer.documentCount; d++) {
        if (strncmp(lspServer.documents[d].uri, "file://", 7) != 0) continue;
        if (!lspServer.documents[d].content) continue;
        char path[1024];
        uri_to_filepath(lspServer.documents[d].uri, path, sizeof(path));
        JsonValue* entry = json_new_object();
        json_object_set(entry, "path", json_new_string(path));
        json_object_set(entry, "content", json_new_string(lspServer.documents[d].content));
        json_array_push(arr, entry);
        n++;
    }
    if (n == 0) { json_free(arr); return NULL; }
    JsonValue* root = json_new_object();
    json_object_set(root, "overlays", arr);
    int len = 0;
    char* s = json_serialize(root, &len);
    json_free(root);
    return s;
}

/* ================================================================
 *  Lot 1 — le check compilateur ne bloque plus la boucle
 *
 * Le LSP est mono-thread. Attendre la fin d'`owl --check` dans la boucle
 * d'événements la fige pour toute la durée du spawn : ~115 ms à chaud, ~5 s
 * à froid (chargement + scan antivirus d'un binaire de ~100 Mo). Comme le
 * check tourne aussi PENDANT LA FRAPPE (débouncé 400 ms,  Lot 2),
 * cela se traduisait par une complétion et un hover en à-coups.
 *
 * Le check est donc découpé en trois temps :
 *   - `start_compiler_check`  : préflight + spawn, rend la main aussitôt ;
 *   - `lsp_check_pump`        : récolte non bloquante + publication (appelée
 *                               par la boucle, y compris au repos) ;
 *   - `apply_check_result`    : traitement du verdict, inchangé.
 *
 * Un seul check en vol à la fois (pas de spawns empilés) : une demande
 * arrivée pendant un check est mémorisée et relancée à la récolte.
 * ================================================================ */

/* /2133 — somme (bornee) la taille en octets des fichiers .owl sous
 * <dir>, recursivement, en ignorant les dossiers de build habituels. La taille
 * de source est un meilleur predicteur du cout du check que le nb de fichiers
 * (mesure ~15 ms/Ko). Profondeur et total bornes : la somme est saturee au-dela
 * de 8 Mo, ce qui suffit -- le plafond de timeout est deja atteint bien avant
 * (~1,84 Mo). */
static size_t lsp_sum_owl_bytes(const char* dir, int depth) {
    if (depth > 3) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    size_t total = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (strcmp(e->d_name, "build") == 0 ||
                strcmp(e->d_name, "build_debug") == 0 ||
                strcmp(e->d_name, "build_static") == 0 ||
                strcmp(e->d_name, "node_modules") == 0 ||
                strcmp(e->d_name, ".git") == 0) continue;
            total += lsp_sum_owl_bytes(path, depth + 1);
            if (total >= (size_t)8 * 1024 * 1024) break;  /* mesure saturee */
            continue;
        }
        size_t nl = strlen(e->d_name);
        if (nl >= 5 && strcmp(e->d_name + nl - 4, ".owl") == 0)
            total += (size_t)st.st_size;
    }
    closedir(d);
    return total;
}

/*  — coeur PUR (zero I/O) : convertit la taille de source en budget de
 * temps. Calibration conservatrice validee dev : +25 ms par Ko (1024 octets),
 * plancher 15 s (projet vide/quasi-vide), plafond 60 s (ecrete au-dela de
 * ~1,84 Mo de source). Arrondi au ms le plus proche via (num + 512) / 1024.
 * Garde anti-overflow : au-dela du seuil ou le plafond est de toute facon
 * atteint, on clampe sans calculer (bytes vient d'un size_t potentiellement
 * enorme). */
int lsp_adaptive_timeout_for_bytes(size_t bytes) {
    const int64_t base    = LSP_CHECK_TIMEOUT_MS;  /* plancher 15000 ms */
    const int64_t ceiling = 60000;                 /* plafond 60 s */
    if (bytes > (size_t)1843200) return (int)ceiling;  /* seuil d'ecretage */
    int64_t delta = ((int64_t)25 * (int64_t)bytes + 512) / 1024;  /* round(25 * Ko) */
    int64_t t = base + delta;
    if (t < base)    t = base;
    if (t > ceiling) t = ceiling;
    return (int)t;
}

/*  — override test-only du timeout adaptatif : 0 = desactive (defaut
 * production, comportement inchange), sinon impose la valeur en ms. Permet au
 * harness de test de forcer deterministiquement un LSP_RUN_TIMEOUT (chemin
 * no-clobber d'apply_check_result) sans attendre le plancher reel de 15 s. */
static int g_lsp_test_timeout_override_ms = 0;
void lsp_test_set_timeout_override_ms(int ms) { g_lsp_test_timeout_override_ms = ms; }

/* /2133 — timeout adaptatif : echelle avec la taille du scope projet
 * pour ne pas tuer prematurement un gros projet legitimement lent. L'override
 * test (> 0) gagne toujours ; sinon on somme les octets du scope et on delegue
 * au coeur pur. */
static int lsp_adaptive_check_timeout(const char* projectRoot) {
    if (g_lsp_test_timeout_override_ms > 0) return g_lsp_test_timeout_override_ms;
    size_t bytes = (projectRoot && projectRoot[0])
                       ? lsp_sum_owl_bytes(projectRoot, 0) : 0;
    return lsp_adaptive_timeout_for_bytes(bytes);
}

/* Préflight + spawn. `kind` = FULL (rapport projet complet) ou ACTIVE (fichier
 * edite seul, contexte projet hydrate — ). Renvoie 1 si un check est
 * parti (g_checkJob non NULL), 0 si la demande a été traitée sans spawn (garde
 * scheme, pas d'exe, chemin rejeté, échec de spawn) — dans ce cas le repli a
 * déjà publié ce qu'il faut. */
static int start_compiler_check(const char* uri, CheckKind kind) {
    /*  — un doc dont le scheme n'est pas file:// (ex owl-bloc://, vue
     * de code isolee) n'a pas de fichier disque : spawn `owl --check <path>`
     * produirait un chemin garbage et de faux diagnostics. Court-circuit AVANT
     * tout spawn ET toute publication (aucune notification emise). */
    if (strncmp(uri, "file://", 7) != 0) {
        lsp__diag_scheme_guard_fired = 1;
        return 0;
    }

    if (!lspServer.owlExePath[0]) {
        publish_diagnostics(uri);
        return 0;
    }

    char filepath[1024];
    uri_to_filepath(uri, filepath, sizeof(filepath));

    /* Reject paths with shell-dangerous characters to prevent command injection.
     * Valid file paths don't need these characters anyway. */
    for (const char* p = filepath; *p; p++) {
        if (*p == '"' || *p == '\'' || *p == '`' || *p == '$' ||
            *p == '|' || *p == '&' || *p == ';' || *p == '\n' || *p == '\r') {
            lsp_log("Rejected path with dangerous chars: %s", filepath);
            publish_diagnostics(uri);
            return 0;
        }
    }
    /* Same for owlExePath, in case it's compromised */
    for (const char* p = lspServer.owlExePath; *p; p++) {
        if (*p == '"' || *p == '\'' || *p == '`' || *p == '$' ||
            *p == '|' || *p == '&' || *p == ';' || *p == '\n' || *p == '\r') {
            lsp_log("owlExePath has dangerous chars");
            publish_diagnostics(uri);
            return 0;
        }
    }

    /*  (modele 2 niveaux,  ; scope script ) — Au
     * save, on lance `owl --check` sur le perimetre du fichier :
     *  - frontiere PROJECT (projet.owl ancetre) : --check <projetRoot>
     *    -> scan RECURSIF, rapport multi-fichiers, diags cross-file routes.
     *  - frontiere SCRIPT (aucun projet.owl)     : --check <dossier fichier>
     *    -> scan NON-RECURSIF des voisins directs (canon ) : une
     *       erreur dans un fichier voisin remonte aussi. Le compilo gere la
     *       (non-)recursivite via la (des)absence de projet.owl -- on ne
     *       passe aucun flag recursif ici.
     * Resolution bornee au workspace LSP comme handle_formatting : un
     * projet.owl egare au-dessus du workspace, ou un fichier ouvert HORS
     * workspace, ne doit pas declencher un scan disque. Dans ces cas on
     * retombe sur le fichier seul (securite). */
    char scriptDir[1024];
    const char* checkTarget = filepath;
    const char* contextDir = NULL;   /*  : racine de contexte du check actif */
    char projectRoot[1024];
    ProjectMode pmode = resolve_project_root(filepath, projectRoot,
                                             sizeof(projectRoot));
    bool useProjectScope = (pmode == PROJECT_MODE_PROJECT);
    if (useProjectScope && lspServer.rootPath[0] &&
        !fmt_path_within(projectRoot, lspServer.rootPath)) {
        /* Resolution echappee du workspace -> retombe en mode mono. */
        useProjectScope = false;
    }
    if (useProjectScope) {
        checkTarget = projectRoot;
        contextDir = projectRoot;
    } else {
        /* Mode SCRIPT : cible = dossier parent du fichier (voisins directs),
         * a condition qu'il reste dans le workspace. Sinon fichier seul. */
        snprintf(scriptDir, sizeof(scriptDir), "%s", filepath);
        char* slash = strrchr(scriptDir, '/');
        char* back  = strrchr(scriptDir, '\\');
        if (back && (!slash || back > slash)) slash = back;
        if (slash) {
            *slash = '\0';
            if (!lspServer.rootPath[0] ||
                fmt_path_within(scriptDir, lspServer.rootPath)) {
                checkTarget = scriptDir;
                contextDir = scriptDir;
            }
        }
    }

    /*  — check fichier ACTIF : compile UNIQUEMENT le fichier edite avec
     * le contexte projet <contextDir> (rapide) et ne publie que son URI. Sans
     * contexte resolu (fichier hors workspace), repli sur le fichier seul
     * (projArg NULL). Le check FULL garde la cible large (dossier/projet) et
     * projArg NULL (dispatch legacy multi-fichiers). */
    const char* projArg = NULL;
    if (kind == CHECK_KIND_ACTIVE) {
        checkTarget = filepath;
        projArg = (contextDir != NULL && contextDir[0]) ? contextDir : NULL;
    }

    /*  — diagnostics dans la langue du client : en mode `en`, on
     * passe `--lang en` au check spawné pour des messages d'erreur anglais. */
    const char* checkLang = (strcmp(lspServer.language, "en") == 0) ? "en" : NULL;

    /*  Lot 2 — overlay des buffers ouverts non-sauvés : le check voit
     * l'état RÉEL de l'éditeur (y compris les fichiers AUTRES que celui sauvé,
     * modifiés mais pas encore écrits sur disque), pas le disque périmé. NULL
     * si aucun buffer overlayable -> check disque-only (comportement Lot 1). */
    char* overlayJson = build_open_buffers_overlay();

    /*  — exécution BORNÉE EN TEMPS : un compilo qui boucle ne doit
     * jamais figer le LSP. Au-delà du délai, on tue le process et on signale.
     *  Lot 1 — le lancement rend la main immédiatement ; la sortie est
     * récoltée par lsp_check_pump.
     *  Lot 2a — servi par le process résident quand il tourne (pas de
     * rechargement du binaire) ; repli sur un spawn dédié sinon. */
    bool hadOverlay = (overlayJson != NULL);
    /* /2133 — timeout adaptatif : echelle avec la taille de source du
     * scope (cf. lsp_adaptive_check_timeout). */
    int checkTimeoutMs = lsp_adaptive_check_timeout(projectRoot);
    g_checkJob = lsp_check_start(lspServer.owlExePath, checkTarget, projArg,
                                 checkLang, overlayJson, checkTimeoutMs);
    free(overlayJson);  /* copié dans le pipe stdin par le runner */
    /* Journalisé APRÈS le lancement : le mode réellement employé (résident ou
     * repli) n'est connu qu'une fois le résident sollicité. */
    lsp_log("Running check (%s, %s, bounded %dms): %s--check \"%s\"%s%s",
            lsp_check_server_is_alive() ? "resident" : "spawn dedie",
            kind == CHECK_KIND_ACTIVE ? "actif" : "full",
            checkTimeoutMs,
            checkLang ? "--lang en " : "", checkTarget,
            projArg ? " --project <contexte>" : "",
            hadOverlay ? " --overlay-stdin" : "");
    if (!g_checkJob) {
        lsp_log("Failed to spawn owl --check");
        publish_diagnostics(uri);
        return 0;
    }
    snprintf(g_checkJobUri, sizeof(g_checkJobUri), "%s", uri);
    g_checkJobStale = 0;
    g_checkJobKind = kind;
    return 1;
}

/* Lance le prochain check si aucun n'est en vol. L'actif est prioritaire
 * (feedback immediat sur le fichier edite) ; le full de fond ne part que quand
 * son echeance de debounce est atteinte ET qu'aucun actif n'attend. Aucun spawn
 * bloquant : appelee depuis le pump. */
static void lsp_check_schedule(void) {
    if (g_checkJob) return;
    if (g_activeReqUri[0]) {
        char uri[1024];
        snprintf(uri, sizeof(uri), "%s", g_activeReqUri);
        g_activeReqUri[0] = '\0';
        start_compiler_check(uri, CHECK_KIND_ACTIVE);
        return;
    }
    if (g_fullDeadline != 0 && LSP_NOW_MS() >= g_fullDeadline) {
        char uri[1024];
        snprintf(uri, sizeof(uri), "%s", g_fullReqUri);
        g_fullDeadline = 0;
        start_compiler_check(uri, CHECK_KIND_FULL);
        return;
    }
}

/* Récolte non bloquante du check en vol + publication, PUIS planification du
 * suivant. Appelée par la boucle d'événements (y compris au repos, cf. lsp_run)
 * et par la variante synchrone ci-dessous. Le pump doit tourner même sans job
 * en vol : c'est lui qui déclenche le full de fond quand son échéance échoit. */
void lsp_check_pump(void) {
    if (g_checkJob) {
        LspRunStatus rs = LSP_RUN_OK;
        char* output = NULL;
        int total = 0;
        if (lsp_check_poll(g_checkJob, &rs, &output, &total)) {
            g_checkJob = NULL;  /* récolté ET libéré par lsp_check_poll */

            int stale = g_checkJobStale;
            g_checkJobStale = 0;
            CheckKind kind = g_checkJobKind;
            char uri[1024];
            snprintf(uri, sizeof(uri), "%s", g_checkJobUri);

            /* Garde de réentrance : les replis publish_diagnostics (timeout,
             * JSON illisible) relanceraient sinon un check depuis la récolte. */
            int prevGuard = g_overlay_check_in_progress;
            g_overlay_check_in_progress = 1;
            if (stale) {
                /* Le contenu a bougé depuis le lancement : ce verdict décrit un
                 * état périmé et ne doit pas écraser un état plus récent. On le
                 * jette — la planification ci-dessous republiera à jour. */
                lsp_log("owl --check obsolete (document modifie depuis le lancement) — verdict jete");
                free(output);
            } else {
                apply_check_result(uri, rs, output, total, kind);  /* free(output) inclus */
            }
            g_overlay_check_in_progress = prevGuard;
        }
    }
    lsp_check_schedule();
}

int lsp_check_is_pending(void) {
    return g_checkJob != NULL || g_activeReqUri[0] != '\0' || g_fullDeadline != 0;
}

/* Check FULL immédiat (didSave + appelants synchrones / tests). Coalesce avec un
 * check en vol : s'il tourne, il devient périmé et le full repart à sa récolte ;
 * sinon il part tout de suite. Garde la sémantique historique (rapport projet
 * complet, réconciliation multi-fichiers). */
void publish_compiler_diagnostics_async(const char* uri) {
    if (g_checkJob) {
        snprintf(g_fullReqUri, sizeof(g_fullReqUri), "%s", uri);
        g_fullDeadline = LSP_NOW_MS();   /* dû : dès la récolte du job courant */
        g_checkJobStale = 1;
        return;
    }
    g_fullDeadline = 0;
    g_fullReqUri[0] = '\0';
    start_compiler_check(uri, CHECK_KIND_FULL);
}

/* Variante SYNCHRONE : lance le check et attend sa publication. Réservée aux
 * appelants qui n'ont PAS de boucle d'événements pour appeler lsp_check_pump
 * (harness de test in-process). Le serveur, lui, passe par la variante async. */
void publish_compiler_diagnostics(const char* uri) {
    publish_compiler_diagnostics_async(uri);
    while (lsp_check_is_pending()) {
        lsp_proc_sleep_ms(5);
        lsp_check_pump();
    }
}

static void apply_check_result(const char* uri, LspRunStatus rs,
                               char* output, int total, CheckKind kind) {
    char filepath[1024];
    uri_to_filepath(uri, filepath, sizeof(filepath));

    if (rs == LSP_RUN_TIMEOUT) {
        lsp_log("owl --check TIMEOUT on %s — process tue ; dernier bon set preserve",
                filepath);
        free(output);
        /*  — spy test-only : prouve qu'un test rouge/vert a bien
         * emprunte CE chemin (et pas un hasard) avant tout throttle de
         * notification. */
        lsp__check_timeout_branch_count++;
        /*  — no-clobber : aucune republication (ni sur l'URI actif ni
         * sur les voisins), juste un signal Info throttle. Le `return` protege
         * en prime les voisins du clearing des URI plus bas. */
        lsp_notify_check_slow();
        return;
    }
    if (rs == LSP_RUN_SPAWN_FAIL || !output) {
        lsp_log("owl --check : sortie indisponible");
        free(output);
        publish_diagnostics(uri);
        return;
    }

    lsp_log("owl --check output (%d bytes): %.200s", total, output);

    JsonValue* result = json_parse(output, total);
    free(output);  /* json_parse copie les chaînes -> buffer libérable ici */
    if (!result) {
        lsp_log("Failed to parse --check JSON output");
        publish_diagnostics(uri);
        return;
    }

    /* New format: {"files":[{"path":"...","errors":[...],"warnings":[...]}, ...]}
     * Each file gets its own publishDiagnostics notification to the correct URI. */
    JsonValue* filesArr = json_get(result, "files");
    if (filesArr && filesArr->type == JSON_ARRAY) {
        /* Track which URIs we publish to so we can clear stale ones next time */
        char newDiagnosedUris[64][1024];
        int newDiagnosedCount = 0;

        /* Clear URIs that are no longer in the result */
        for (int k = 0; k < lspServer.diagnosedUriCount; k++) {
            bool found = false;
            for (int m = 0; m < filesArr->as.arrayVal.count && !found; m++) {
                JsonValue* fe = filesArr->as.arrayVal.items[m];
                const char* fpath = json_get_string(fe, "path");
                if (!fpath) continue;
                char furi[1024];
                filepath_to_uri(fpath, furi, sizeof(furi));
                if (strcmp(lspServer.diagnosedUris[k], furi) == 0) found = true;
            }
            if (!found) {
                /* File no longer in results → clear its diagnostics */
                JsonValue* clearParams = json_new_object();
                json_object_set(clearParams, "uri", json_new_string(lspServer.diagnosedUris[k]));
                json_object_set(clearParams, "diagnostics", json_new_array());
                lsp_send_notification("textDocument/publishDiagnostics", clearParams);
                lsp_log("Cleared stale diagnostics for %s", lspServer.diagnosedUris[k]);
                /*  — cache vide pour cet URI redevenu propre. */
                diag_cache_reset(lspServer.diagnosedUris[k]);
            }
        }

        for (int fi = 0; fi < filesArr->as.arrayVal.count; fi++) {
            JsonValue* fe = filesArr->as.arrayVal.items[fi];
            const char* fpath = json_get_string(fe, "path");
            if (!fpath) continue;

            char furi[1024];
            filepath_to_uri(fpath, furi, sizeof(furi));

            JsonValue* diagnostics = json_new_array();
            /*  — remet le cache a 0 pour cet URI, puis le repeuple
             * exactement avec ce qu'on publie (clean si aucun diag). */
            diag_cache_reset(furi);

            JsonValue* errorsArr = json_get(fe, "errors");
            if (errorsArr && errorsArr->type == JSON_ARRAY) {
                for (int i = 0; i < errorsArr->as.arrayVal.count; i++) {
                    JsonValue* err = errorsArr->as.arrayVal.items[i];
                    const char* msg = json_get_string(err, "message");
                    if (!msg) msg = "Unknown error";
                    int line = (int)json_get_int(err, "line", 1);
                    int col  = (int)json_get_int(err, "column", 1);
                    int sC, eC;
                    diag_word_range_for_uri(furi, line, col, &sC, &eC);
                    json_array_push(diagnostics, make_lsp_diag(line, col, msg, 1, sC, eC));
                    diag_cache_add(furi, line, col, 1, msg, sC, eC);
                }
            }
            JsonValue* warningsArr = json_get(fe, "warnings");
            if (warningsArr && warningsArr->type == JSON_ARRAY) {
                for (int i = 0; i < warningsArr->as.arrayVal.count; i++) {
                    JsonValue* warn = warningsArr->as.arrayVal.items[i];
                    const char* msg = json_get_string(warn, "message");
                    if (!msg) msg = "Warning";
                    int line = (int)json_get_int(warn, "line", 1);
                    int col  = (int)json_get_int(warn, "column", 1);
                    int sC, eC;
                    diag_word_range_for_uri(furi, line, col, &sC, &eC);
                    json_array_push(diagnostics, make_lsp_diag(line, col, msg, 2, sC, eC));
                    diag_cache_add(furi, line, col, 2, msg, sC, eC);
                }
            }

            JsonValue* notifParams = json_new_object();
            json_object_set(notifParams, "uri", json_new_string(furi));
            json_object_set(notifParams, "diagnostics", diagnostics);
            lsp_send_notification("textDocument/publishDiagnostics", notifParams);
            lsp_log("Published %d diagnostics for %s",
                    diagnostics->as.arrayVal.count, fpath);

            if (newDiagnosedCount < 64) {
                snprintf(newDiagnosedUris[newDiagnosedCount], 1024, "%s", furi);
                newDiagnosedCount++;
            }
        }

        /* Update tracked URIs */
        lspServer.diagnosedUriCount = newDiagnosedCount;
        for (int k = 0; k < newDiagnosedCount; k++) {
            memcpy(lspServer.diagnosedUris[k], newDiagnosedUris[k], 1024);
            lspServer.diagnosedUris[k][1023] = '\0';
        }

        json_free(result);
        scan_user_symbols();
        return;
    }

    /* Single-file format {"errors":[...],"warnings":[...]}.
     *  - kind FULL (repli fichier-seul d'un check projet, ou mode CTRL+S) : on
     *    clear les diagnostics des AUTRES fichiers publies par un check projet
     *    anterieur, pour eviter des erreurs fantomes sur des fichiers qu'on ne
     *    touche plus, et on ne suit plus que l'URI courant.
     *  - kind ACTIVE (, check fichier actif a la frappe) : anti-flicker.
     *    On NE touche PAS aux autres URIs — leurs erreurs cross-fichier restent
     *    affichees jusqu'au full de fond — on s'assure juste que l'URI courant
     *    est suivi (pour qu'un full ulterieur puisse le reconcilier). */
    if (kind == CHECK_KIND_ACTIVE) {
        bool tracked = false;
        for (int k = 0; k < lspServer.diagnosedUriCount; k++) {
            if (strcmp(lspServer.diagnosedUris[k], uri) == 0) { tracked = true; break; }
        }
        if (!tracked && lspServer.diagnosedUriCount < 64) {
            snprintf(lspServer.diagnosedUris[lspServer.diagnosedUriCount], 1024, "%s", uri);
            lspServer.diagnosedUriCount++;
        }
    } else {
        for (int k = 0; k < lspServer.diagnosedUriCount; k++) {
            if (strcmp(lspServer.diagnosedUris[k], uri) == 0) continue;
            JsonValue* clearParams = json_new_object();
            json_object_set(clearParams, "uri", json_new_string(lspServer.diagnosedUris[k]));
            json_object_set(clearParams, "diagnostics", json_new_array());
            lsp_send_notification("textDocument/publishDiagnostics", clearParams);
            /*  — cache vide pour ces URIs qu'on ne suit plus. */
            diag_cache_reset(lspServer.diagnosedUris[k]);
        }
        /* Track only the current URI from now on */
        strncpy(lspServer.diagnosedUris[0], uri, 1023);
        lspServer.diagnosedUris[0][1023] = '\0';
        lspServer.diagnosedUriCount = 1;
    }

    JsonValue* errorsArr = json_get(result, "errors");
    JsonValue* warningsArr = json_get(result, "warnings");
    JsonValue* diagnostics = json_new_array();
    /*  — remet le cache de l'URI courant a 0 puis le repeuple. */
    diag_cache_reset(uri);

    if (errorsArr && errorsArr->type == JSON_ARRAY) {
        for (int i = 0; i < errorsArr->as.arrayVal.count; i++) {
            JsonValue* err = errorsArr->as.arrayVal.items[i];
            const char* msg = json_get_string(err, "message");
            if (!msg) msg = "Unknown error";
            int line = (int)json_get_int(err, "line", 1);
            int col  = (int)json_get_int(err, "column", 1);
            int sC, eC;
            diag_word_range_for_uri(uri, line, col, &sC, &eC);
            json_array_push(diagnostics, make_lsp_diag(line, col, msg, 1, sC, eC));
            diag_cache_add(uri, line, col, 1, msg, sC, eC);
        }
    }
    if (warningsArr && warningsArr->type == JSON_ARRAY) {
        for (int i = 0; i < warningsArr->as.arrayVal.count; i++) {
            JsonValue* warn = warningsArr->as.arrayVal.items[i];
            const char* msg = json_get_string(warn, "message");
            if (!msg) msg = "Warning";
            int line = (int)json_get_int(warn, "line", 1);
            int col  = (int)json_get_int(warn, "column", 1);
            int sC, eC;
            diag_word_range_for_uri(uri, line, col, &sC, &eC);
            json_array_push(diagnostics, make_lsp_diag(line, col, msg, 2, sC, eC));
            diag_cache_add(uri, line, col, 2, msg, sC, eC);
        }
    }
    json_free(result);

    JsonValue* notifParams = json_new_object();
    json_object_set(notifParams, "uri", json_new_string(uri));
    json_object_set(notifParams, "diagnostics", diagnostics);
    lsp_send_notification("textDocument/publishDiagnostics", notifParams);

    lsp_log("Published %d compiler diagnostics for %s",
            diagnostics->as.arrayVal.count, filepath);

    scan_user_symbols();
}
