/*  — digue anti-fuite linguistique pour le rendu CLI doc
 * (`owl explain` / `owl list` / `owl examples`). Compagnon de 
 * (meme classe de bug, mais LSP completion) : ici la surface est le
 * texte imprime sur stdout par src/cli_doc.c quand --lang en est
 * demande.
 *
 * Principe : on invoque le VRAI binaire owl.exe (chemin passe en argv[1],
 * ou OWL_BIN, ou ../../build/owl.exe par defaut) sur un echantillon
 * representatif de commandes en --lang en, on capture stdout, et on
 * scanne le texte contre une liste noire de labels/mots-cles FR connus.
 * Zero occurrence attendue en mode EN — chaque hit est une vraie fuite
 * (le texte de la digue est construit pour eviter les faux positifs,
 * cf. commentaire sur is_word_byte plus bas, calque sur
 * tests/lsp/lsp_test_main.c::count_word_leaks, ).
 *
 * Ce n'est PAS un self-test compile dans owl.exe (on ne touche pas a
 * src/) : c'est un harness boite noire independant, compile a la volee
 * par tests/cli/doc_lang/run.sh.
 *
 * Exit code : 0 si aucune fuite, 1 si au moins une fuite detectee
 * (rouge attendu tant que src/cli_doc.c n'est pas corrige par
 * l'implementer).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

/* ---- capture d'une commande ------------------------------------- */

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char* buf = (char*)malloc(cap);
    buf[0] = '\0';
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap + n + 1) * 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
        buf[len] = '\0';
    }
    PCLOSE(fp);
    return buf;
}

/* ---- scanner frontiere-de-mot, calque sur lsp_test_main.c -------- */
/* Un octet est "de mot" s'il est alnum/'_' ou si son bit haut est
 * positionne (continuation UTF-8 d'un caractere accentue). Un token
 * FR compte comme fuite seulement si encadre des DEUX cotes par des
 * octets non-mot (ou debut/fin de texte). Evite de couper un mot
 * accentue au milieu et de matcher "un"/"une" a l'interieur d'un
 * identifiant EN legitime. */
static int is_word_byte(unsigned char c) {
    return isalnum(c) || c == '_' || (c & 0x80) != 0;
}

typedef struct {
    const char* cmd;     /* commande executee (rapport humain) */
    const char* token;   /* fuite detectee */
    char snippet[121];   /* contexte autour du hit, tronque */
} Leak;

#define MAX_LEAKS 512
static Leak g_leaks[MAX_LEAKS];
static int g_leak_count = 0;

static void record_leak(const char* cmd, const char* token, const char* at) {
    if (g_leak_count >= MAX_LEAKS) return;
    Leak* l = &g_leaks[g_leak_count++];
    l->cmd = cmd;
    l->token = token;
    int n = 0;
    const char* p = at;
    /* recule un peu pour donner du contexte, sans sortir du buffer
     * (on ne connait pas le debut exact ici, donc contexte = a partir
     * du hit vers l'avant, suffisant pour identifier le site). */
    while (*p && *p != '\n' && n < 120) { l->snippet[n++] = *p++; }
    l->snippet[n] = '\0';
}

/*  — trois exemptions, toutes de la meme famille que skipHeaderLines
 * : ce qui est cite est le nom FR CANON du symbole demande, ou un
 * champ qui n'a pas de contrepartie EN au registre. Ce n'est pas une fuite du
 * rendu, c'est la reponse a la question posee.
 *
 *  1. ECHO DE COMMANDE — la ligne cite une commande copiable qui re-cite
 *     l'argument tel qu'il a ete tape (« docs  found in 17 docs (owl search
 *     entier) »). Sans ca, `explain entier --lang en` rougit sur son propre
 *     argument. Reconnu par « (owl » dans la ligne.
 *
 *  2. EN-TETE DE SENS — un symbole peut avoir plusieurs sens (type composé,
 *     énumération, constante) ; chaque sens ouvre par sa propre ligne
 *     d'en-tete « couleur (enumeration) », hors de portee de skipHeaderLines
 *     qui ne couvre que le premier. Reconnu par la fin de ligne.
 *
 *  3. NOM DE PARAMETRE — dans « Trie(tableau : array) », `tableau` est le NOM
 *     du parametre, pas son type. OwlParam ne porte pas de champ nameEN : il
 *     n'existe aucune forme anglaise a rendre, et en inventer une releve du
 *     dev, pas du rendu (trou remonte au coordinator, ticket separe). Reconnu
 *     par le « : » qui suit immediatement. La COLONNE DE TYPE, elle, reste
 *     scannee : la digue rougit toujours si un type redevient francais. */
static int line_ends_with(const char* ls, const char* le, const char* suffix) {
    size_t n = strlen(suffix);
    return (size_t)(le - ls) >= n && memcmp(le - n, suffix, n) == 0;
}

static int hit_is_symbol_echo(const char* text, const char* at, size_t tlen) {
    const char* ls = at;
    while (ls > text && ls[-1] != '\n') ls--;
    const char* le = at;
    while (*le && *le != '\n') le++;
    /* 1 */
    for (const char* p = ls; p + 5 <= le; p++) {
        if (strncmp(p, "(owl ", 5) == 0) return 1;
    }
    /* 2 */
    static const char* const SENSE_SUFFIX[] = {
        " (enumeration)", " (composite type)", " (constant)",
        " (error code)", " (keyword)", NULL
    };
    for (int i = 0; SENSE_SUFFIX[i]; i++) {
        if (line_ends_with(ls, le, SENSE_SUFFIX[i])) return 1;
    }
    /* 3 */
    if (at + tlen + 3 <= le && memcmp(at + tlen, " : ", 3) == 0) return 1;
    return 0;
}

/* Frontiere de mot pour un match de `token` trouve a la position `p`
 * dans `text` (tlen = strlen(token)). La regle ne s'applique que du cote
 * ou le token lui-meme se termine par un octet "de mot". Si le token se
 * termine deja par de la ponctuation (ex: "Propriétés ("), il n'y a pas
 * de mot a proteger de la coupure — la frontiere est automatiquement
 * satisfaite de ce cote. Sans ce garde, un token comme "Propriétés ("
 * ne matcherait jamais "Propriétés (6)" car le caractere suivant ("6")
 * est un octet de mot. */
static int at_word_boundary(const char* text, const char* p,
                            const char* token, size_t tlen) {
    int tokenStartsWord = is_word_byte((unsigned char)token[0]);
    int tokenEndsWord   = is_word_byte((unsigned char)token[tlen - 1]);
    int leftOk  = !tokenStartsWord || (p == text) || !is_word_byte((unsigned char)p[-1]);
    int rightOk = !tokenEndsWord   || !is_word_byte((unsigned char)p[tlen]);
    return leftOk && rightOk;
}

/* Predicat PUR : 1 si `token` apparait a frontiere de mot dans `text`,
 * sinon 0. Meme regle de frontiere que scan_text (via at_word_boundary),
 * mais AUCUN effet de bord : ni record_leak, ni comptage, ni filtre
 * hit_is_symbol_echo (specifique a la detection de fuite, hors sujet ici).
 *
 * Sert a valider les formes EN attendues (expect). Un simple strstr y
 * acceptait une forme EN contenue dans sa contrepartie FR — verrou mort a
 * la pose : "CONTINUE" satisfait par "CONTINUER", "total)" satisfait par
 * "au total)". La frontiere de mot refuse le premier (E suivi de R : deux
 * octets de mot). Elle reste sans effet sur le second (token finissant par
 * ")" et precede d'un espace = vraie frontiere) — limitation mesuree,
 * hors de portee de la frontiere de mot. */
static int contains_word(const char* text, const char* token) {
    if (!text || !token || !*token) return 0;
    size_t tlen = strlen(token);
    const char* p = text;
    while ((p = strstr(p, token)) != NULL) {
        if (at_word_boundary(text, p, token, tlen)) return 1;
        p += tlen;
    }
    return 0;
}

static int scan_text(const char* cmd, const char* text, const char* token) {
    if (!text || !token || !*token) return 0;
    int hits = 0;
    size_t tlen = strlen(token);
    const char* p = text;
    while ((p = strstr(p, token)) != NULL) {
        if (at_word_boundary(text, p, token, tlen) && !hit_is_symbol_echo(text, p, tlen)) {
            hits++;
            record_leak(cmd, token, p);
        }
        p += tlen;
    }
    return hits;
}

/* Auto-preuve du predicat expect : prouve mecaniquement qu'un verrou mort
 * (forme EN sous-chaine de sa contrepartie FR) est desormais REFUSE, sans
 * introduire de faux negatif sur un vrai match. Chaque assertion cite le
 * delta avec l'ancien strstr nu. Retourne 0 si tout passe, 1 sinon. */
static int self_test_contains_word(void) {
    struct { const char* text; const char* token; int expect; const char* proves; } CASES[] = {
        /* Verrou mort refuse : "CONTINUE" ne matche plus "CONTINUER"
         * (E suivi de R, deux octets de mot). strstr rendait vrai. */
        { "CONTINUER", "CONTINUE", 0, "frontiere droite cassee (sous-mot rejete)" },
        /* Frontiere gauche : "total" precede de "S" (octet de mot). */
        { "SOUStotal", "total", 0, "frontiere gauche cassee (sous-mot rejete)" },
        /* Pas de faux negatif : vrai match encadre d'espaces. */
        { "press CONTINUE now", "CONTINUE", 1, "vrai match a frontiere (pas de faux negatif)" },
        /* Frontieres aux bords du buffer (debut = text, fin = '\0'). */
        { "CONTINUE", "CONTINUE", 1, "match au bord du buffer accepte" },
        /* Limitation mesuree : token finissant par ')' (non-mot) precede
         * d'un espace reste un vrai match — la frontiere de mot ne resout
         * PAS ce cas, comme strstr. Documente, hors scope. */
        { "au total)", "total)", 1, "token a ponctuation finale : frontiere sans effet (= strstr)" },
        { NULL, NULL, 0, NULL }
    };
    int failed = 0;
    for (int i = 0; CASES[i].text; i++) {
        int got = contains_word(CASES[i].text, CASES[i].token);
        if (got != CASES[i].expect) {
            fprintf(stderr,
                "SELF-TEST FAIL: contains_word(\"%s\", \"%s\") = %d, attendu %d [%s]\n",
                CASES[i].text, CASES[i].token, got, CASES[i].expect, CASES[i].proves);
            failed = 1;
        }
    }
    /* Confirme le delta : l'ancien strstr nu acceptait bien le verrou mort. */
    if (strstr("CONTINUER", "CONTINUE") == NULL) {
        fprintf(stderr, "SELF-TEST FAIL: premisse cassee, strstr n'accepte plus CONTINUE dans CONTINUER\n");
        failed = 1;
    }
    return failed;
}

/* ---- liste noire FR (verifiee en mode --lang en) ----------------- */
/* Volontairement des labels/phrases STATIQUES connus du rendu CLI doc
 * (headers de section, mentions figees), pas des mots ambigus qui
 * pourraient legitimement apparaitre dans une description EN. Les
 * articles generiques "un"/"une" sont gardes (frontiere de mot stricte
 * les rend surs, cf. ) mais en dernier recours pour la
 * couverture, pas comme signal principal. */
static const char* FR_LEAK_TOKENS[] = {
    /* headers / labels de section fixes */
    "(type composé)",
    "[Type composé]",
    "[Fonction native]",
    "[Forme méthode]",
    "Type primitif intégré à la VM",
    "Types primitifs (intégrés à la VM)",
    "Types composés du registry",
    "Propriétés (",
    "Méthodes (",
    /* "Signatures" est identique FR/EN — retiré de la blacklist (faux positif).
     * Le header EN utilise "Call signatures (N)". */
    "Voir aussi",
    "présent dans",
    "forme méthode —",
    "natives correspondantes",
    "Aucun scénario public n'illustre ce symbole",
    "Fonctions natives — catégorie",
    "aucune fonction dans cette catégorie",
    "Catégories disponibles",
    "Détail d'une fonction",
    /* fragments de description de parametre figes (natives) */
    "pour permettre le chaînage",
    "type de tri",
    "trié en place",
    "trié selon le mode",
    "cible",
    "défaut",
    /*  — labels de RENDU (pas de contenu registre) : libelle
     * d'enumeration, en-tete de la liste de valeurs, pluriel en dur de
     * la ligne « See also / where », et litteraux booleens des valeurs
     * par defaut. Tous scannes sur TOUTES les commandes (pas seulement
     * la famille rapport) : ce sont des labels generiques du rendu. */
    "(énumération",
    "Valeurs (",
    "champ", "champs",
    "Vrai", "Faux",
    /*  — libelles de fiche restes en dur. « (code d'erreur) » est
     * l'en-tete de la fiche d'un code du registre owl_errors ; « (constante) »
     * celui d'une constante ; les autres sont les libelles de la fiche de
     * propriete DSL et les en-tetes des blocs « Acceptée par » / « exposé
     * par ». Scannes sur TOUTES les commandes : ce sont des labels de rendu
     * generiques, pas du contenu de registre. */
    "(code d'erreur)", "(constante)", "(limite compile-time)",
    "propriété", "Propriété", "Type :", "Défaut :",
    "Acceptée par", "exposé par",
    "Propriété par défaut", "équivaut",
    /* articles generiques, frontiere de mot stricte */
    "un", "une",
};
#define FR_LEAK_TOKENS_COUNT ((int)(sizeof(FR_LEAK_TOKENS)/sizeof(FR_LEAK_TOKENS[0])))

/*  — formes canoniques FR des mots-cles, scannees sur TOUTES les
 * commandes SAUF les fiches de mot-cle (keywordFiche=1). Une fiche `explain SI`
 * cite legitimement la syntaxe canon du mot-cle (`SI ... ALORS ... FIN`) et son
 * exemple : ce ne sont pas des fuites de rendu, c'est le sujet meme de la fiche
 * (meme famille que l'echo de symbole de hit_is_symbol_echo). Partout ailleurs,
 * ces formes en mode EN restent des fuites (le catalogue keyword rend IF/THEN,
 * cf. ). */
static const char* FR_KEYWORD_FORM_TOKENS[] = {
    "FIN", "SI", "ALORS", "SINON", "POUR", "TANTQUE", "RETOUR",
};
#define FR_KEYWORD_FORM_TOKENS_COUNT \
    ((int)(sizeof(FR_KEYWORD_FORM_TOKENS)/sizeof(FR_KEYWORD_FORM_TOKENS[0])))

/* ---- liste noire famille rapport ---------------------- */
/* La prose EN de la famille Etat citait ses blocs, mots-cles et types
 * sous leur forme FR : un anglophone lisait « Ouverture / Corps /
 * Fermeture », mots absents de son propre code (Opening / Body /
 * Closing). Ces tokens ne sont scannes que sur les commandes marquees
 * ETAT_FAMILY, et seulement APRES la ligne d'en-tete : l'en-tete
 * affiche legitimement le nom canon FR du symbole (« Corps / Body »).
 *
 * Volontairement ABSENTS : Filtre / Lecture / Fermeture. La section
 * Events les affiche comme noms de symbole (« Filtre()  / Filter »),
 * ce qui est legitime — les blacklister produirait du faux positif.
 * Leurs occurrences en prose sont couvertes indirectement par Corps /
 * Vrai / Faux, presents dans les memes phrases. */
static const char* const ETAT_FR_LEAK_TOKENS[] = {
    "sous-bloc", "fait partie",
    "Etat", "État",
    "Corps", "EnTeteDocument", "EnTêteDocument",
    "HautDePage", "BasDePage", "FinDeDocument",
    "PROPRIETES", "Ouverture",
    /* Vrai / Faux : promus dans FR_LEAK_TOKENS par  (le rendu des
     * valeurs par defaut les imprimait sur tous les types, pas seulement
     * la famille rapport) — plus besoin de les dupliquer ici. */
};
#define ETAT_FR_LEAK_TOKENS_COUNT \
    ((int)(sizeof(ETAT_FR_LEAK_TOKENS)/sizeof(ETAT_FR_LEAK_TOKENS[0])))

/* ---- liste noire « noms de types » (, generalisee ) --- */
/*  avait restreint ce groupe aux commandes marquees `dslRender` :
 * les signatures de natives citaient alors legitimement les memes mots
 * (`Trie(tableau, mode : entier)`), le contenu de registre n'ayant pas
 * encore de rendu localise.  a leve cette cause — les types de
 * signature sont derives de leur nom EN au registre — donc l'exception
 * disparait avec elle : le groupe est desormais scanne sur TOUTES les
 * commandes, apres skipHeaderLines (l'en-tete affiche legitimement le nom
 * canon FR du symbole demande). */
static const char* const TYPE_FR_LEAK_TOKENS[] = {
    "entier", "booleen", "booléen", "chaine", "chaîne",
    "valeur d'enum", "descripteur compose", "chaîneMultilingue",
    "couleur (entier RGB)", "position (entier, entier)",
    /* noms de widget kinds sous leur forme FR dans le bloc « Accepted by » */
    "Libelle", "Selecteur", "ZoneRepetee", "Fenetre",
    /*  — vocabulaire de type des signatures de natives : noms de
     * types primitifs/composes et mots de forme conteneur (« tableau de »,
     * « tableau associatif ») places dans la colonne de type d'un parametre
     * ou d'un type de retour. */
    "tableau", "tableaux", "chaînes", "procédure", "couleur",
    "associatif", "rien", "tout", "varié", "ou",
    /* formes de receveur des methodes prefix (« <ChampCombo>.Trie() ») */
    "ChampListe", "ChampCombo", "ChampSelecteur",
    "ChampInterrupteur", "ChampZoneRepetee",
};
#define TYPE_FR_LEAK_TOKENS_COUNT \
    ((int)(sizeof(TYPE_FR_LEAK_TOKENS)/sizeof(TYPE_FR_LEAK_TOKENS[0])))

/* ---- echantillon representatif de commandes ---------------------- */
/* Types composes, primitifs, natives, list, examples — cf. brief
 * . Chemin de owl.exe passe en argv[1] par run.sh. */
/*  — volet POSITIF de la digue. Une liste noire seule ne peut pas
 * verrouiller un rendu qui doit AJOUTER la forme EN a cote de la forme FR
 * (cas des noms de proprietes DSL, affiches « ..MargeHaut / ..TopMargin »
 * comme la section Events affiche « Filtre() / Filter »). Absence de la
 * forme EN attendue = regression, au meme titre qu'une fuite FR. */
/*  — volet NEGATIF cible. Le volet POSITIF (expect) verrouille la
 * presence d'une forme EN, mais rien n'empeche le rendu de citer EN PLUS
 * l'ancienne forme FR. `forbid` nomme, commande par commande, les formes FR
 * precises qui ne doivent plus apparaitre : formes de symboles citees en
 * prose par un message d'erreur (« 'Ancetre' », « dDébutDessin »), trop
 * specifiques pour vivre dans une liste noire globale. */
typedef struct {
    const char* label;
    const char* argsFmt;  /* %s = chemin owl.exe */
    int etatFamily;       /* 1 = scanne aussi ETAT_FR_LEAK_TOKENS */
    int skipHeaderLines;  /* lignes d'en-tete ignorees (noms canon FR) */
    const char* const* expect; /* NULL-terminee : formes EN obligatoires */
    const char* const* forbid; /* NULL-terminee : formes FR interdites */
    int keywordFiche;     /* 1 = fiche de mot-cle : exempte des tokens de forme
                             mot-cle (le corps cite la syntaxe canon FR) */
} Cmd;

/*  — noms de proprietes DSL sous leur forme EN (cas 2). */
static const char* const EXPECT_REPORT[] = {
    "..TopMargin", "..BottomMargin", "..LeftMargin", "..RightMargin", NULL
};
/*  — defauts booleens localises (cas 1) + pluriel localise (cas 4). */
static const char* const EXPECT_FONT[] = {
    "= False", "fields accept", NULL
};
/*  — libelle d'enumeration et en-tete des valeurs (cas 3). */
static const char* const EXPECT_ETATFORMAT[] = {
    "(enumeration)", "Values (", NULL
};

/* ================================================================
 *  — familles A (messages EN citant un nom FR) et B (rendu).
 * Les formes EN viennent du registre (owl explain --json : name_en) ou de
 * la table canon bilingue du formatter (src/formatter.c::fmt_keywords),
 * jamais d'une traduction inventee.
 * ================================================================ */

/* --- A : messages d'erreur EN, un couple forbid/expect par code --- */
static const char* const F_ANCETRE[]  = { "'Ancetre'", NULL };
static const char* const E_ANCETRE[]  = { "'Ancestor'", NULL };
static const char* const F_CHENC[]    = { "'fixe'", NULL };
static const char* const E_CHENC[]    = { "'fixed'", NULL };
static const char* const F_CANVAS[]   = { "dDébutDessin", NULL };
static const char* const E_CANVAS[]   = { "dStartDrawing", NULL };
static const char* const F_VOLETLIB[] = { "Volet", "Libellé", NULL };
static const char* const E_VOLETLIB[] = { "Pane", "Caption", NULL };
static const char* const F_XYLIBRE[]  = { "Etire", "Marge", "Alignement",
                                          "Disposition", NULL };
static const char* const E_XYLIBRE[]  = { "Stretch", "Margin", "Alignment",
                                          "Layout", NULL };
static const char* const F_MULTI[]    = { "chaîneMultilingue", NULL };
static const char* const E_MULTI[]    = { "multilingualString", NULL };
static const char* const F_INIT[]     = { "'Initialisation'", NULL };
static const char* const E_INIT[]     = { "'Initialization'", NULL };
static const char* const F_TABGEN[]   = { "tableau", NULL };
static const char* const E_TABGEN[]   = { "'array<T>'", "'array of <T>'", NULL };
static const char* const F_COLONNE[]  = { "colonne", "Champ", NULL };
static const char* const E_COLONNE[]  = { "'column'", "Control Table", NULL };
static const char* const F_VOLETONG[] = { "volet", "Onglet", "Champ", NULL };
static const char* const E_VOLETONG[] = { "'pane'", "Control Tab", NULL };
static const char* const F_CHPREF[]   = { "Champ", "FIN", NULL };
static const char* const E_CHPREF[]   = { "'Control'", "END", NULL };
static const char* const F_KINDWIN[]  = { "'Fenetre'", NULL };
static const char* const E_KINDWIN[]  = { "'Window'", NULL };
static const char* const F_OBJET[]    = { "'objet'", NULL };
static const char* const E_OBJET[]    = { "'object'", NULL };
static const char* const F_PROJBLOC[] = { "'projet.owl'", "Projet", "FIN", NULL };
static const char* const E_PROJBLOC[] = { "'project.owl'", "'is a Project ... END'", NULL };
static const char* const F_EXCORDER[] = { "FAIRE", "SINON", "ENFIN", "DANS", "FIN", NULL };
static const char* const E_EXCORDER[] = { "DO", "ELSE", "FINALLY", "IN", "END", NULL };
static const char* const F_STEPREV[]  = { "'PAS'", "'_PAS_'", NULL };
static const char* const E_STEPREV[]  = { "'STEP'", "'_STEP_'", NULL };
static const char* const F_BOUNDREV[] = { "'A'", "'_A_'", NULL };
static const char* const E_BOUNDREV[] = { "'TO'", "'_TO_'", NULL };
static const char* const F_LOGIC[]    = { "ET/OU", "_ET_", "_OU_", "Taille",
                                          "Occurrence", NULL };
static const char* const E_LOGIC[]    = { "AND/OR", "_AND_", "_OR_", "Length",
                                          "Count", NULL };
static const char* const F_SORTIR[]   = { "SORTIR", "SELON", NULL };
static const char* const E_SORTIR[]   = { "BREAK", "SWITCH", NULL };
static const char* const F_CONTINUER[]= { "CONTINUER", NULL };
static const char* const E_CONTINUER[]= { "CONTINUE", NULL };
static const char* const F_CONSTBLK[] = { "CONSTANTE", NULL };
static const char* const E_CONSTBLK[] = { "CONSTANT", NULL };
static const char* const F_PROPRT[]   = { "PROPRIETES", NULL };
static const char* const E_PROPRT[]   = { "PROPERTIES", NULL };
static const char* const F_DEFLIT[]   = { "Vrai", "Faux", NULL };
static const char* const E_DEFLIT[]   = { "True/False", NULL };
static const char* const F_SUBFIELD[] = { "Libelle", "Bouton", "Saisie", "Conteneur", NULL };
static const char* const E_SUBFIELD[] = { "Label", "Button", "Input", "Container", NULL };
static const char* const F_ICONE[]    = { "..Couleur", NULL };
static const char* const E_ICONE[]    = { "..Color", NULL };
static const char* const F_FENTOP[]   = { "FinInitialisation", NULL };
static const char* const E_FENTOP[]   = { "EndInitialization", NULL };
static const char* const F_LIBERER[]  = { "'Liberer'", NULL };
static const char* const E_LIBERER[]  = { "'Free'", NULL };
static const char* const F_ABSTR[]    = { "ABSTRAITE", NULL };
static const char* const E_ABSTR[]    = { "ABSTRACT", NULL };
static const char* const F_QUANDEXC[] = { "QUAND EXCEPTION", "DANS", "FAIRE", "FIN", NULL };
static const char* const E_QUANDEXC[] = { "'WHEN EXCEPTION'", NULL };
static const char* const F_TABMULTI[] = { "'chaîne'", NULL };
static const char* const E_TABMULTI[] = { "'string'", NULL };
static const char* const F_ONGVAL[]   = { "Valeur", "volets", NULL };
static const char* const E_ONGVAL[]   = { "Value", "panes", NULL };

/* --- B : rendu de `explain` / `where` --------------------------- */
/* Fiche de propriete DSL : libelles + nom de type primitif + defaut. */
static const char* const EXPECT_PROP_ETIRE[] = {
    "property", "Type:", "Default: False", "boolean", NULL
};
/* `owl where` : bloc « Accepted by (N fields) » + kinds sous forme EN. */
static const char* const EXPECT_WHERE_ETIRE[] = {
    "Accepted by", "fields)", "Label", "RadioGroup", "Looper", NULL
};
/* Fiche de widget : ligne de propriete par defaut + colonne de type. */
static const char* const EXPECT_ONGLET[] = {
    "Default property", "is equivalent to", "integer", "enum value",
    "composite descriptor", NULL
};
/* Fiche d'evenement : « exposed by N field(s) », pluriel localise. */
static const char* const EXPECT_EVENT[] = { "exposed by", "field", NULL };
/* Fiche de constante : libelle + forme EN adjointe au canon FR. */
static const char* const EXPECT_CONST_GRISE[] = {
    "(constant)", "/ Grayed", NULL
};
/* Valeurs d'enumeration sous leur forme EN. */
static const char* const EXPECT_ENUM_ALIGN[] = {
    "alignStart", "alignCenter", NULL
};
/* Defaut qui est un nom de constante d'enumeration : forme EN du registre,
 * pas une traduction de prose. */
static const char* const EXPECT_PROP_ETAT[] = { "Default:", "Active", NULL };
static const char* const FORBID_PROP_ETAT[] = { "Actif", NULL };
/* Fiche d'un code d'erreur : en-tete localise. */
static const char* const EXPECT_ERRCARD[] = { "(error code)", NULL };

/* ================================================================
 *  — signatures de natives. Les types de parametre et de retour
 * sont derives de leur forme EN au registre (types primitifs, types
 * composes, enums) ; les formes conteneur (« tableau de X », « tableau
 * associatif ») et les formes de receveur (« <ChampCombo> ») suivent le
 * meme chemin. Les NOMS DE PARAMETRE restent FR : OwlParam ne porte pas de
 * champ nameEN — trou remonte au coordinator, pas invente ici.
 * ================================================================ */
static const char* const E_SIG_TRIE[] = {
    "(tableau : array)", "→ array",
    "mode : integer ∈ sortType",
    "colonne : integer|string|procedure",
    /* formes de receveur du bloc « Method form » */
    "<List>.", "<ComboBox>.", "<array>.", NULL
};
static const char* const E_SIG_TABLEAUAJOUTE[] = {
    "tableau : array, valeur : any", "→ nothing",
    "tableau : associative array", "clé : string|integer", NULL
};
static const char* const E_SIG_XMLPREMIER[] = {
    "parent : xmlNode or xmlDocument", "→ xmlNode", NULL
};
static const char* const E_SIG_DPOLYGONE[] = {
    "image : Image", "points : varies", "fond : color", "trait : color", NULL
};
static const char* const E_SIG_COMMENCEPAR[] = {
    "chaîne : string", "motif : string|array of string", "→ integer", NULL
};
static const char* const E_SIG_TRACE[] = {
    "expressions : any", "→ nothing", NULL
};
static const char* const E_SIG_AJOUTE[] = {
    "tableau : array, valeur : any", "→ array",
    "<zipArchive>.", "<List>.", "<ComboBox>.",
    "<RadioGroup>.", "<CheckBox>.", "<array>.", NULL
};

/*  — famille de recherche dans les chaines. Ces constantes portent
 * une forme EN au registre ; la fiche doit la rendre, et le corps de fiche
 * ne doit plus citer la forme FR (l'en-tete, lui, affiche legitimement le
 * couple « DepuisFin / FromEnd » -> skipHeaderLines=1). */
static const char* const E_DEPUISFIN[]  = { "FromEnd", NULL };
static const char* const F_DEPUISFIN[]  = { "DepuisFin", NULL };
static const char* const E_SANSCASSE[]  = { "IgnoreCase", NULL };
static const char* const F_SANSCASSE[]  = { "SansCasse", NULL };
static const char* const E_MOTCOMPLET[] = { "WholeWord", NULL };
static const char* const F_MOTCOMPLET[] = { "MotComplet", NULL };
static const char* const E_SSCDROITE[]  = { "sscRight", NULL };
static const char* const F_SSCDROITE[]  = { "sscDroite", NULL };

/*  — categorie inconnue. Un nom que le registre ne porte pas doit
 * etre annonce comme inconnu, dans la langue demandee, et la reponse doit
 * proposer les categories valides. Le rendu precedent repondait « no
 * function in this category » : une reponse a une question comprise, alors
 * que le nom n'avait pas ete reconnu. */
static const char* const E_CAT_INCONNUE[] = {
    "unknown category", "Available categories", NULL
};
/* Interdit la reponse mensongere elle-meme, dans sa forme EN : la forme FR
 * est deja couverte par FR_LEAK_TOKENS. */
static const char* const F_CAT_INCONNUE[] = {
    "no function in this category", NULL
};

/*  — cle de categorie bilingue. La categorie est la CLE du filtre,
 * mais une cle monolingue laissait un en-tete moitie-moitie : prose anglaise,
 * categorie francaise. Les deux formes doivent resoudre vers la meme
 * categorie, et l'en-tete doit citer la forme de la langue demandee. Les
 * formes EN sont celles de la table centrale des categories, la meme que le
 * self-test du registre exige pour toute categorie portee au registre.
 *
 * Le volet `forbid` est le verrou de non-regression : si l'en-tete anglais
 * redevient francais, la forme FR reapparait et le scanner rougit. */
static const char* const E_CAT_TABLEAUX[] = { "category Arrays", NULL };
static const char* const F_CAT_TABLEAUX[] = { "Tableaux", NULL };
static const char* const E_CAT_SYSTEME[]  = { "category System", NULL };
static const char* const F_CAT_SYSTEME[]  = { "Système", "Systeme", NULL };
/* Categorie scindee : la chaine unique « File/Queue » disparait, chaque
 * langue prend sa forme (FR « File », EN « Queue »). */
static const char* const E_CAT_FILE[]     = { "category Queue", NULL };
static const char* const F_CAT_FILE[]     = { "File/Queue", "category File", NULL };

/*  — les quatre commandes `list` que  avait volontairement
 * ecartees de l'echantillon : leurs en-tetes, lignes de compte, pieds et
 * colonne cle sortaient en francais en dur, les ajouter aurait rougi. Elles
 * entrent ici AVEC leur correctif, sinon le correctif se defait au premier
 * refactor (lecon de  et ).
 *
 * Le volet `expect` verrouille la forme EN attendue, le volet `forbid` la
 * forme FR precise qui disparait. skipHeaderLines = 0 partout : sur une
 * commande de LISTE, la premiere ligne est un libelle de rendu, pas le nom
 * canon d'un symbole demande — elle doit suivre la langue comme le reste. */

/* Noms de categorie : la meme cle rend deja « category Arrays » sur
 * `list natives --cat Tableaux --lang en` (owl_category_display, ).
 * Deux commandes voisines ne peuvent pas diverger sur la meme cle. */
static const char* const E_LIST_CATEGORIES[] = {
    "Native function categories", "function(s) in total",
    "Arrays", "Strings", "Reports", "Queue", "System",
    "functions)", "Details of a category", "<name>", NULL
};
static const char* const F_LIST_CATEGORIES[] = {
    "Catégories de fonctions natives", "au total",
    "Tableaux", "Chaines", "Etats", "Système",
    "fonctions)", "Détail d'une catégorie", "<nom>", NULL
};

/* Colonne cle du catalogue : `list types --lang en` rend deja `string` /
 * `integer`, et la fiche d'une constante rend deja sa forme EN
 * (« DepuisFin / FromEnd », ). La liste suit. */
/* Volontairement PAS « total) » : « au total) » le contient, l'assertion
 * serait vraie avant comme apres (mesure : absente du rapport MISSING du
 * rouge initial). Une forme EN qui ne discrimine pas ne verrouille rien. */
static const char* const E_LIST_CONSTANTS[] = {
    "Registry constants",
    "sscRight", "FromEnd", "IgnoreCase", NULL
};
static const char* const F_LIST_CONSTANTS[] = {
    "Constantes du registry", "au total",
    "sscDroite", "DepuisFin", "SansCasse", NULL
};

/* Invariant 3 du langage (bilingue FR⇄EN), dont SI/IF est l'exemple
 * litteral : un catalogue de mots-cles rendu en anglais qui affiche `SI`
 * viole le contrat. Les formes EN viennent de la table canon bilingue du
 * formatter (fmt_keywords[].partner), jamais d'une traduction inventee. */
static const char* const E_LIST_KEYWORDS[] = {
    "Language keywords", "documented",
    /* « CONTINUE » est exclu : « CONTINUER » le contient (mesure : absent du
     * rapport MISSING du rouge initial). Sa contrepartie FR reste verrouillee
     * cote `forbid`, ou la frontiere de mot s'applique. */
    "IF", "THEN", "ELSE", "END", "FOR", "WHILE", "SWITCH", "CASE",
    "BREAK", "RETURN", "True", "False",
    /*  — PAS a deux canons EN selon le sens : le listing standalone
     * les NOMME tous les deux (« NOT / STEP »), sans jamais les composer. Le
     * compose PAS DANS reste « NOT IN » (le sens negation, jamais « STEP IN »
     * — verrouille cote forbid). */
    "NOT / STEP", "NOT IN",
    "senses)", "Compound", "Reserved", "reserved word", NULL
};
static const char* const F_LIST_KEYWORDS[] = {
    "Keywords du langage", "documentés",
    /*  — en-tetes de categorie de mots-cles, restes FR en --lang en
     * (la cle bilingue existe desormais dans owl_categories.def). */
    "Contrôle", "Logique", "Déclaration", "Visibilité", "Constantes",
    "SI", "ALORS", "SINON", "FIN", "POUR", "TANTQUE", "SELON", "CAS",
    "SORTIR", "CONTINUER", "RETOUR", "Vrai", "Faux",
    /*  — le bug d'origine : composer le canon EN de boucle avec le
     * canon EN de negation. « STEP IN » ne doit jamais etre publie. */
    "STEP IN",
    "sens)", "Composés", "Réservés", "mot réservé", NULL
};

/*  — la cinquieme `list` de la meme famille. En-tete, ligne de
 * compte, separateurs de categorie et « (N sens) » etaient FR en dur.
 * Les formes EN des 6 categories viennent d'owl_categories.def (bilingue). */
static const char* const E_LIST_OPERATORS[] = {
    "Language operators", "documented",
    "String comparison", "Null-safe", "Loose equality",
    "Assignment", "Arithmetic", "Comparison",
    "senses)", NULL
};
static const char* const F_LIST_OPERATORS[] = {
    "Opérateurs du langage", "documentés",
    /* Categories FR : « Null-safe » est identique dans les deux langues, donc
     * absent d'ici (non-fuite). « Comparaison » couvre aussi « Comparaison
     * chaîne » (sous-chaine, frontiere de mot respectee). */
    "Comparaison chaîne", "Égalité souple", "Affectation", "Arithmétique",
    "Comparaison",
    "sens)", NULL
};

static const char* const E_LIST_LIMITS[] = {
    "Compile-time limits", "result(s)", "Details:", NULL
};
static const char* const F_LIST_LIMITS[] = {
    "Limites compile-time", "au total", "résultat(s)", "Détail :",
    /*  — la descEN de MAX_BREAKS citait les formes FR SORTIR / CONTINUER
     * (comme MAX_LOOP_DEPTH cite deja FOR / WHILE / LOOP en EN). */
    "SORTIR", "CONTINUER", NULL
};

/*  — fiche d'un mot-cle en --lang en : le libelle de genre
 * (« mot-clé »), l'etiquette de syntaxe (« Syntaxe : ») et la categorie
 * (« Contrôle ») suivent la langue. Le CORPS (syntaxe canon, exemple) cite
 * legitimement les mots-cles FR -> keywordFiche=1. */
static const char* const E_EXPLAIN_SI[] = { "(keyword)", "Control", "Syntax:", NULL };
static const char* const F_EXPLAIN_SI[] = { "(mot-clé)", "Contrôle", "Syntaxe", NULL };

/*  — fiche d'une limite : la categorie « Compiler/Controle » suit la
 * langue, et la descEN de MAX_BREAKS ne cite plus SORTIR / CONTINUER. */
static const char* const E_EXPLAIN_MAXBREAKS[] = { "BREAK", NULL };
static const char* const F_EXPLAIN_MAXBREAKS[] = {
    "Compiler/Controle", "SORTIR", "CONTINUER", NULL
};

static const Cmd COMMANDS[] = {
    /*  — skipHeaderLines renseigne partout : le groupe « noms de
     * types » est desormais scanne sur toutes les commandes, et l'en-tete de
     * fiche affiche legitimement le nom canon FR du symbole demande. */
    { "explain Projet --lang en",              "\"%s\" explain Projet --lang en", 0, 1, NULL },
    { "explain Couleur --lang en",              "\"%s\" explain Couleur --lang en", 0, 2, NULL },
    { "explain styleJauge --lang en",           "\"%s\" explain styleJauge --lang en", 0, 1, NULL },
    { "explain HTTPRequete --lang en",          "\"%s\" explain HTTPRequete --lang en", 0, 3, NULL },
    { "explain entier --lang en",               "\"%s\" explain entier --lang en", 0, 1, NULL },
    { "explain chaine --lang en",                "\"%s\" explain chaine --lang en", 0, 1, NULL },
    { "explain booleen --lang en",               "\"%s\" explain booleen --lang en", 0, 1, NULL },
    { "explain Trie --lang en",                  "\"%s\" explain Trie --lang en", 0, 2, E_SIG_TRIE },
    { "list types --lang en",                    "\"%s\" list types --lang en", 0, 0, NULL },
    { "list natives --cat Chaines --lang en",    "\"%s\" list natives --cat Chaines --lang en", 0, 0, NULL },
    { "examples Trie --lang en",                 "\"%s\" examples Trie --lang en", 0, 0, NULL },
    { "examples entier --lang en",               "\"%s\" examples entier --lang en", 0, 0, NULL },
    /*  — signatures : vocabulaire de type couvert bout a bout
     * (conteneurs, union, enum-contrainte, « tout »/« rien », formes de
     * receveur des methodes prefix). */
    { "explain TableauAjoute --lang en", "\"%s\" explain TableauAjoute --lang en",
      0, 1, E_SIG_TABLEAUAJOUTE },
    { "explain XMLPremier --lang en",    "\"%s\" explain XMLPremier --lang en",
      0, 1, E_SIG_XMLPREMIER },
    { "explain dPolygone --lang en",     "\"%s\" explain dPolygone --lang en",
      0, 1, E_SIG_DPOLYGONE },
    { "explain CommencePar --lang en",   "\"%s\" explain CommencePar --lang en",
      0, 2, E_SIG_COMMENCEPAR },
    { "explain Trace --lang en",         "\"%s\" explain Trace --lang en",
      0, 1, E_SIG_TRACE },
    { "explain Ajoute --lang en",        "\"%s\" explain Ajoute --lang en",
      0, 2, E_SIG_AJOUTE },
    /*  — famille rapport : prose EN qui parlait francais. */
    { "explain Report --lang en",                "\"%s\" explain Report --lang en", 1, 1, EXPECT_REPORT },
    { "explain Body --lang en",                  "\"%s\" explain Body --lang en", 1, 1, NULL },
    { "explain DocumentHeader --lang en",        "\"%s\" explain DocumentHeader --lang en", 1, 1, NULL },
    { "explain iImprimeEtat --lang en",          "\"%s\" explain iImprimeEtat --lang en", 1, 1, NULL },
    { "explain iImprimeBloc --lang en",          "\"%s\" explain iImprimeBloc --lang en", 1, 1, NULL },
    /*  — Font entre dans l'echantillon. L'exception documentee qui
     * l'en excluait (valeurs par defaut booleennes rendues « Vrai / Faux »)
     * a disparu avec sa cause : le rendu des defauts est localise. */
    { "explain Font --lang en",                  "\"%s\" explain Font --lang en", 0, 1, EXPECT_FONT },
    /* skipHeaderLines=2 : nom du symbole + ligne de categorie. Cette 2e
     * ligne reste hors scan cible : la categorie est une CLE machine
     * (`owl list --cat Etats`), pas un libelle traduisible — cf. ,
     * point remonte au coordinator. */
    { "explain etatFormat --lang en",            "\"%s\" explain etatFormat --lang en", 1, 2, EXPECT_ETATFORMAT },
    { "explain etatOrientation --lang en",       "\"%s\" explain etatOrientation --lang en", 1, 2, NULL },

    /* ----  famille A : messages EN qui citaient un nom FR ---- */
#define ERRCMD(code, exp, forb) \
    { "explain " code " --lang en", "\"%s\" explain " code " --lang en", \
      0, 0, exp, forb }
    ERRCMD("E_SEM_ANCETRE_OUTSIDE_METHOD",        E_ANCETRE,  F_ANCETRE),
    ERRCMD("E_SEM_ANCETRE_NO_PARENT",             E_ANCETRE,  F_ANCETRE),
    ERRCMD("E_SYNTAX_TYPE_CHAINE_ENCODAGE",       E_CHENC,    F_CHENC),
    ERRCMD("E_NATIVE_IMAGE_NO_CANVAS",            E_CANVAS,   F_CANVAS),
    ERRCMD("E_SEM_DSL_VOLET_LIBELLE_MANQUANT",    E_VOLETLIB, F_VOLETLIB),
    ERRCMD("E_SEM_DSL_XY_FORBIDDEN_NOT_LIBRE",    E_XYLIBRE,  F_XYLIBRE),
    ERRCMD("E_SEM_DSL_INLINE_DICT_NOT_MULTILINGUE", E_MULTI,  F_MULTI),
    ERRCMD("E_SEM_DSL_INITIALISATION_OBSOLETE",   E_INIT,     F_INIT),
    ERRCMD("E_SYNTAX_TABLEAU_GENERIC_BRACKET",    E_TABGEN,   F_TABGEN),
    ERRCMD("E_SYNTAX_TABLEAU_INIT_MULTILIGNE",    E_TABMULTI, F_TABMULTI),
    ERRCMD("E_SEM_DSL_COLONNE_OUTSIDE_TABLE",     E_COLONNE,  F_COLONNE),
    ERRCMD("E_SEM_DSL_VOLET_OUTSIDE_ONGLET",      E_VOLETONG, F_VOLETONG),
    ERRCMD("E_SEM_DSL_ONGLET_VALEUR_HORS_BORNES", E_ONGVAL,   F_ONGVAL),
    ERRCMD("E_SEM_DSL_CHAMP_PREFIXE_REQUIS",      E_CHPREF,   F_CHPREF),
    ERRCMD("E_SEM_CONTROL_KIND_OUTSIDE_WINDOW",   E_KINDWIN,  F_KINDWIN),
    ERRCMD("E_SEM_OBJET_OUTSIDE_METHOD",          E_OBJET,    F_OBJET),
    ERRCMD("E_SEM_PROJET_BLOC_MANQUANT",          E_PROJBLOC, F_PROJBLOC),
    ERRCMD("E_SYNTAX_EXCEPTION_FAIRE_ORDER",      E_EXCORDER, F_EXCORDER),
    ERRCMD("E_SEM_QUAND_EXCEPTION_INSIDE_LOOP",   E_QUANDEXC, F_QUANDEXC),
    ERRCMD("E_SEM_FOR_STEP_REEVAL_TOO_LARGE",     E_STEPREV,  F_STEPREV),
    ERRCMD("E_SEM_FOR_BOUND_REEVAL_TOO_LARGE",    E_BOUNDREV, F_BOUNDREV),
    ERRCMD("E_RT_LOGIC_OPERAND_TYPE",             E_LOGIC,    F_LOGIC),
    ERRCMD("E_SEM_SORTIR_SELON_OUTSIDE_SELON",    E_SORTIR,   F_SORTIR),
    ERRCMD("E_SEM_CONTINUER_OUTSIDE_LOOP",        E_CONTINUER,F_CONTINUER),
    ERRCMD("E_SEM_REASSIGN_CONSTANTE",            E_CONSTBLK, F_CONSTBLK),
    ERRCMD("E_SEM_DSL_PROP_READONLY_RUNTIME",     E_PROPRT,   F_PROPRT),
    ERRCMD("E_SEM_DEFAULT_MUST_BE_LITERAL",       E_DEFLIT,   F_DEFLIT),
    ERRCMD("E_SEM_DSL_SUBFIELD_NOT_VALID_IN_KIND",E_SUBFIELD, F_SUBFIELD),
    ERRCMD("W_SEM_ICONE_NOT_COLORABLE",           E_ICONE,    F_ICONE),
    ERRCMD("E_SEM_DSL_FENETRE_TOPLEVEL_INTERDIT", E_FENTOP,   F_FENTOP),
    ERRCMD("E_SYNTAX_EXPECTED_VAR_NAME_AFTER_LIBERER", E_LIBERER, F_LIBERER),
    ERRCMD("E_SEM_ABSTRAITE_OUTSIDE_CLASS_METHOD", E_ABSTR,   F_ABSTR),
    /* En-tete de la fiche d'un code d'erreur (« (code d'erreur) »). */
    ERRCMD("E_LEX_UNTERMINATED_STRING",           EXPECT_ERRCARD, NULL),
#undef ERRCMD

    /* ----  famille B : rendu de explain / where ------------- */
    { "explain Onglet --lang en",   "\"%s\" explain Onglet --lang en",
      0, 1, EXPECT_ONGLET, NULL },
    /* Formes ASCII des symboles accentues : l'argument transite par cmd.exe
     * (popen), qui n'est pas en UTF-8 — un « Étire » litteral arrive mutile
     * cote owl.exe et la commande repond « symbole inconnu », c'est-a-dire
     * une capture qui ne scanne rien. L'invariant accent-insensible resout
     * la forme ASCII vers le meme symbole. */
    { "explain Etire --lang en",    "\"%s\" explain Etire --lang en",
      0, 1, EXPECT_PROP_ETIRE, NULL },
    { "where Etire --lang en",      "\"%s\" where Etire --lang en",
      0, 1, EXPECT_WHERE_ETIRE, NULL },
    { "explain Grise --lang en",    "\"%s\" explain Grise --lang en",
      0, 1, EXPECT_CONST_GRISE, NULL },
    { "explain alignement --lang en", "\"%s\" explain alignement --lang en",
      0, 1, EXPECT_ENUM_ALIGN, NULL },
    { "explain surChangementVolet --lang en",
      "\"%s\" explain surChangementVolet --lang en",
      0, 1, EXPECT_EVENT, NULL },
    /* Defaut = nom de constante d'enumeration (..Etat vaut Actif). */
    { "where Etat --lang en",       "\"%s\" where Etat --lang en",
      0, 1, EXPECT_PROP_ETAT, FORBID_PROP_ETAT },

    /* ----  : constantes de recherche dans les chaines ------- */
    { "explain DepuisFin --lang en",  "\"%s\" explain DepuisFin --lang en",
      0, 1, E_DEPUISFIN, F_DEPUISFIN },
    { "explain SansCasse --lang en",  "\"%s\" explain SansCasse --lang en",
      0, 1, E_SANSCASSE, F_SANSCASSE },
    { "explain MotComplet --lang en", "\"%s\" explain MotComplet --lang en",
      0, 1, E_MOTCOMPLET, F_MOTCOMPLET },
    { "explain sscDroite --lang en",  "\"%s\" explain sscDroite --lang en",
      0, 1, E_SSCDROITE, F_SSCDROITE },

    /* ----  : categorie inconnue, annoncee comme telle ------- */
    { "list natives --cat ZzzPasUneCategorie --lang en",
      "\"%s\" list natives --cat ZzzPasUneCategorie --lang en",
      0, 0, E_CAT_INCONNUE, F_CAT_INCONNUE },

    /* ----  : cle de categorie bilingue ---------------------- */
    /* La forme EN resout (elle n'etait pas reconnue du tout avant). */
    { "list natives --cat Arrays --lang en",
      "\"%s\" list natives --cat Arrays --lang en",
      0, 0, E_CAT_TABLEAUX, F_CAT_TABLEAUX },
    /* La forme FR resout aussi, et rend l'en-tete EN : meme resultat, un
     * seul rendu. */
    { "list natives --cat Tableaux --lang en",
      "\"%s\" list natives --cat Tableaux --lang en",
      0, 0, E_CAT_TABLEAUX, F_CAT_TABLEAUX },
    /* Resolution casse- ET accent-insensible (invariants 1 et 2) : « systeme »
     * en minuscules et sans accent doit rendre la categorie « Système ». */
    { "list natives --cat systeme --lang en",
      "\"%s\" list natives --cat systeme --lang en",
      0, 0, E_CAT_SYSTEME, F_CAT_SYSTEME },
    /* Categorie scindee par langue. */
    { "list natives --cat File --lang en",
      "\"%s\" list natives --cat File --lang en",
      0, 0, E_CAT_FILE, F_CAT_FILE },

    /* ----  : les quatre `list` restees muettes --------------- */
    { "list categories --lang en", "\"%s\" list categories --lang en",
      0, 0, E_LIST_CATEGORIES, F_LIST_CATEGORIES },
    { "list constants --lang en",  "\"%s\" list constants --lang en",
      0, 0, E_LIST_CONSTANTS,  F_LIST_CONSTANTS },
    { "list keywords --lang en",   "\"%s\" list keywords --lang en",
      0, 0, E_LIST_KEYWORDS,   F_LIST_KEYWORDS },
    { "list limits --lang en",     "\"%s\" list limits --lang en",
      0, 0, E_LIST_LIMITS,     F_LIST_LIMITS },

    /* ----  : la cinquieme `list` (operateurs) --------------- */
    /* keywordFiche=1 : les descriptions d'operateurs citent la syntaxe OWL
     * canon (`SI EstNul(<var>) ALORS ... FIN`, littéraux `Vrai`/`Faux`), hors
     * scope de ce ticket (descriptions deja localisees via type_to_lang). Le
     * verrou FR reste porte par F_LIST_OPERATORS (en-tete, compte, categories,
     * « sens »), inchange par ce flag. */
    { "list operators --lang en",  "\"%s\" list operators --lang en",
      0, 0, E_LIST_OPERATORS,  F_LIST_OPERATORS, 1 },

    /* ----  : categories non-natives + libelles de fiche keyword --- */
    { "explain SI --lang en",       "\"%s\" explain SI --lang en",
      0, 0, E_EXPLAIN_SI,       F_EXPLAIN_SI, 1 },
    { "explain MAX_BREAKS --lang en", "\"%s\" explain MAX_BREAKS --lang en",
      0, 0, E_EXPLAIN_MAXBREAKS, F_EXPLAIN_MAXBREAKS, 0 },
};
#define COMMANDS_COUNT ((int)(sizeof(COMMANDS)/sizeof(COMMANDS[0])))

/* Avance de n lignes dans le texte capture. Retourne un pointeur sur le
 * debut de la ligne n+1 (ou sur la fin du buffer si le texte est plus
 * court). */
static const char* skip_lines(const char* text, int n) {
    const char* p = text;
    for (int i = 0; i < n && *p; i++) {
        const char* nl = strchr(p, '\n');
        if (!nl) return p + strlen(p);
        p = nl + 1;
    }
    return p;
}

typedef struct { const char* cmd; const char* token; } Missing;
#define MAX_MISSING 128
static Missing g_missing[MAX_MISSING];
static int g_missing_count = 0;
static int g_empty_count = 0;

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";

    /* Auto-preuve avant tout scan : si le predicat expect regresse, la digue
     * ne vaut rien (verrou mort a la pose). On rougit AVANT de scanner. */
    if (self_test_contains_word() != 0) {
        printf("RESULT: RED — self-test contains_word en echec (voir stderr).\n");
        return 1;
    }

    int totalHits = 0;
    for (int i = 0; i < COMMANDS_COUNT; i++) {
        char cmdline[1024];
        snprintf(cmdline, sizeof(cmdline), COMMANDS[i].argsFmt, owlBin);
        /*  — enveloppe la commande entiere dans une paire de
         * guillemets supplementaire, comme internal_native_scan.c le fait
         * depuis . Sous Windows _popen appelle `cmd /c "<cmd>"` et
         * cmd.exe ne retire QUE la paire la plus externe : sans ce wrapper,
         * `"./build/owl.exe" explain X` fait chercher un programme nomme
         * `.` et la commande rend une sortie VIDE. C'est exactement le
         * chemin emprunte par scripts/run_all_checks.sh, qui passe
         * `./build/owl.exe` — ce scanner-la n'avait donc jamais rien
         * scanne depuis , et concluait au vert sur du vide. */
        char full[1200];
        snprintf(full, sizeof(full), "\"%s 2>&1\"", cmdline);
        char* out = run_capture(full);
        /* Sortie vide = commande non executee. Deuxieme verrou : une liste
         * noire sur un texte vide trouve zero fuite et conclut au vert. */
        if (!out || !*out) {
            fprintf(stderr, "ERREUR: sortie vide (commande non executee) : %s\n",
                    cmdline);
            g_empty_count++;
            free(out);
            continue;
        }
        for (int t = 0; t < FR_LEAK_TOKENS_COUNT; t++) {
            totalHits += scan_text(COMMANDS[i].label, out, FR_LEAK_TOKENS[t]);
        }
        /*  — formes de mot-cle FR, sauf sur une fiche de mot-cle qui
         * cite legitimement sa propre syntaxe canon. */
        if (!COMMANDS[i].keywordFiche) {
            for (int t = 0; t < FR_KEYWORD_FORM_TOKENS_COUNT; t++) {
                totalHits += scan_text(COMMANDS[i].label, out,
                                       FR_KEYWORD_FORM_TOKENS[t]);
            }
        }
        if (COMMANDS[i].etatFamily) {
            const char* body = skip_lines(out, COMMANDS[i].skipHeaderLines);
            for (int t = 0; t < ETAT_FR_LEAK_TOKENS_COUNT; t++) {
                totalHits += scan_text(COMMANDS[i].label, body,
                                       ETAT_FR_LEAK_TOKENS[t]);
            }
        }
        /* , generalise  — noms de types, sur toutes les
         * commandes. Hors en-tete : les premieres lignes affichent
         * legitimement le nom canon FR du symbole demande. */
        {
            const char* body = skip_lines(out, COMMANDS[i].skipHeaderLines);
            for (int t = 0; t < TYPE_FR_LEAK_TOKENS_COUNT; t++) {
                totalHits += scan_text(COMMANDS[i].label, body,
                                       TYPE_FR_LEAK_TOKENS[t]);
            }
        }
        /*  — formes FR interdites, ciblees par commande. */
        if (COMMANDS[i].forbid) {
            const char* body = skip_lines(out, COMMANDS[i].skipHeaderLines);
            for (int t = 0; COMMANDS[i].forbid[t]; t++) {
                totalHits += scan_text(COMMANDS[i].label, body,
                                       COMMANDS[i].forbid[t]);
            }
        }
        if (COMMANDS[i].expect) {
            for (int t = 0; COMMANDS[i].expect[t]; t++) {
                if (contains_word(out, COMMANDS[i].expect[t])) continue;
                if (g_missing_count < MAX_MISSING) {
                    g_missing[g_missing_count].cmd   = COMMANDS[i].label;
                    g_missing[g_missing_count].token = COMMANDS[i].expect[t];
                    g_missing_count++;
                }
            }
        }
        free(out);
    }

    printf("=== fix/1128 digue anti-fuite CLI doc (--lang en) ===\n");
    printf("Commandes echantillonnees : %d\n", COMMANDS_COUNT);
    printf("Fuites detectees : %d\n", totalHits);
    printf("Formes EN manquantes : %d\n", g_missing_count);
    printf("Commandes sans sortie : %d\n\n", g_empty_count);
    for (int i = 0; i < g_leak_count; i++) {
        printf("[LEAK %3d] cmd=\"%s\" token=\"%s\" contexte=\"%s\"\n",
               i + 1, g_leaks[i].cmd, g_leaks[i].token, g_leaks[i].snippet);
    }
    for (int i = 0; i < g_missing_count; i++) {
        printf("[MISSING %3d] cmd=\"%s\" forme EN attendue absente : \"%s\"\n",
               i + 1, g_missing[i].cmd, g_missing[i].token);
    }

    if (totalHits > 0 || g_missing_count > 0 || g_empty_count > 0) {
        printf("\nRESULT: RED (%d fuite(s), %d forme(s) EN manquante(s), "
               "%d commande(s) sans sortie) — attendu tant que src/cli_doc.c "
               "n'honore pas --lang en sur ces labels.\n",
               totalHits, g_missing_count, g_empty_count);
        return 1;
    }
    /* Garde ensemble vide :  a rendu le compte visible et
     * a fait echouer la commande SANS SORTIE. Reste le cas ou l'echantillon
     * lui-meme se vide : zero commande, zero fuite, GREEN. Chercher des
     * fuites dans du vide conclut au vert — c'est le defaut que cette digue
     * a elle-meme subi. */
    if (COMMANDS_COUNT == 0) {
        printf("\nRESULT: RED — CORPUS VIDE : 0 commande dans l'echantillon. "
               "Chercher une fuite dans du vide conclut toujours au vert.\n");
        return 1;
    }
    printf("\nRESULT: GREEN (0 fuite, 0 forme EN manquante, "
           "%d commandes reellement executees)\n", COMMANDS_COUNT);
    return 0;
}
