// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test fmt-canon — confrontation des deux sources de canon.
 *
 * Deux tables portent la forme canonique d'un mot du langage :
 *
 *   1. la fiche keyword (owl_keyword_catalog.c + token_type_name du scanner),
 *      qui alimente `owl explain` et le survol de l'editeur ;
 *   2. fmt_keywords[] (formatter.c), qui decide de la forme ECRITE dans le
 *      fichier au save.
 *
 * Rien ne verifiait qu'elles etaient d'accord. Cas mesure : la fiche affiche
 * LIBÉRER, le formatter ecrivait Liberer — l'utilisateur lit une forme dans
 * la doc, l'ecrit, enregistre, et l'editeur en pose une autre. Invariant 4
 * (le formatter RESTAURE le canon) rompu par un canon concurrent.
 *
 *  comparait le seul PROFIL D'ACCENTS, en supposant la casse
 * contextuelle donc incomparable. Mesure faite depuis : sur les
 * 14 entrees ou fiche et formatter divergeaient en casse, les 14 etaient des
 * bugs de fiche — la documentation publique donnait raison au formatter a
 * chaque fois (`x est un entier`, `Hérite de cAnimal`, `PROCEDURE
 * Constructeur()`, `implemente iAffichable`, `= Vrai`). Le seul mot dont la
 * casse depend vraiment du contexte est `de` (minuscule en construction de
 * type, `DE` en iteration `POUR TOUT x DE t`) : la MAJUSCULE y est produite
 * par une branche dediee de format_source, pas par fmt_keywords[], donc la
 * confrontation table-a-table reste exacte. R5 compare desormais la chaine
 * entiere.
 *
 * Cinq regles :
 *
 *   R1 fmt_canon_accent_divergence
 *      Pour chaque entree fmt_keywords[] qui designe le MEME mot que la
 *      fiche (egalite apres accent-fold + lowercase), la forme du formatter
 *      et celle de la fiche doivent porter les memes accents. Les entrees
 *      d'une autre langue (`free` -> Free face a la fiche LIBÉRER) ne
 *      designent pas le meme mot : hors perimetre, la fiche n'expose pas de
 *      canon EN.
 *
 *   R5 fmt_canon_case_divergence
 *      Meme appariement que R1, egalite byte-a-byte au lieu du seul profil
 *      d'accents. Ce qui empeche la fiche de titrer LIBÉRER quand le save
 *      ecrit Libérer. Source de verite : fmt_keywords[] — c'est la table qui
 *      decide de ce qui atterrit dans le fichier de l'utilisateur.
 *
 *   R3 fmt_canon_dsl_divergence
 *      Meme confrontation pour les mots-cles de bloc du DSL (Fenêtre, État,
 *      ZoneRépétée, MenuSéparateur...). Leur canon n'est pas porte par une
 *      fiche keyword mais par CONTROL_KINDS (ui_decl.c), que `owl explain`
 *      et le hover affichent. Le formatter en garde une copie dans
 *      fmt_keywords[] : troisieme table, meme risque de derive.
 *
 *   R4 fmt_canon_not_idempotent
 *      Garde-fou sur la table elle-meme : la forme emise doit etre une cle
 *      de fmt_keywords[] rendant exactement cette forme. Sinon le canon
 *      n'est pas un point fixe et un second save reecrit autre chose. Une
 *      forme mal encodee (double-encodage UTF-8) tombe ici immediatement,
 *      la ou R1/R3 se contentent de ne plus reconnaitre le mot.
 *
 *   R2 fmt_canon_keyword_unhandled (informatif, non bloquant)
 *      Un mot dont la fiche affiche une forme accentuee mais que le
 *      formatter ne traite pas du tout : la forme survit par ABSENCE de
 *      traitement, pas par canonisation. C'est exactement comment `État`
 *      restait correct avant  — et comment il a diverge des qu'on a
 *      enregistre le mot. Non bloquant : l'absence n'est pas un desaccord,
 *      c'est un angle mort.
 *
 * Branche sur `owl --self-test-fmt-canon` (cf src/main.c), controle
 * `fmt-canon` de scripts/run_all_checks.sh. Retour : nombre d'echecs
 * bloquants (R1 + R5 + R3 + R4 ; R2 est un avertissement). */

#include "formatter.h"
#include "owl_keyword_catalog.h"
#include "owl_name_normalize.h"
#include "scanner.h"
#include "ui_decl.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;
static int g_warn = 0;

/* Minuscule EN PRESERVANT les accents. ASCII A-Z, plus le supplement
 * Latin-1 encode en UTF-8 sur 2 octets (C3 80..9E -> C3 A0..BE, hors
 * C3 97 = signe multiplie qui n'est pas une lettre). Suffit : les
 * identifiants du langage vivent dans cette plage. */
static void accent_lower(const char* src, char* out, size_t cap) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)src; *p && o + 2 < cap; ) {
        if (p[0] == 0xC3 && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) {
            out[o++] = (char)0xC3;
            out[o++] = (char)(p[1] + 0x20);
            p += 2;
        } else if (p[0] >= 'A' && p[0] <= 'Z') {
            out[o++] = (char)(p[0] - 'A' + 'a');
            p += 1;
        } else {
            out[o++] = (char)p[0];
            p += 1;
        }
    }
    out[o] = '\0';
}

static bool has_accent(const char* s) {
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p >= 0x80) return true;
    }
    return false;
}

/* TokenType du mot `word` (deja lowercase ASCII, forme stockee par le
 * scanner). TOKEN_EOF si le mot n'est pas un keyword scanner. */
static TokenType token_for_word(const char* word) {
    TokenType t;
    for (int i = 0; ; i++) {
        const char* kw = scanner_keyword_at(i, &t);
        if (kw == NULL) break;
        if (strcmp(kw, word) == 0) return t;
    }
    return TOKEN_EOF;
}

/* Forme canon affichee par la fiche : canon_display prioritaire, sinon la
 * chaine du switch token_type_name (meme resolution que explain_keyword
 * dans cli_doc.c). */
static const char* fiche_canon(const OwlKeywordDef* def) {
    return def->canon_display ? def->canon_display : token_type_name(def->token);
}

static void fail(const char* rule, const char* word,
                 const char* fiche, const char* fmt) {
    g_fail++;
    fprintf(stderr, "  FAIL [%s] %s : fiche=\"%s\" formatter=\"%s\"\n",
            rule, word, fiche, fmt);
}

/* ---- R1 : fmt_keywords[] contre la fiche, sur le meme mot ---- */
static void run_accent_divergence(void) {
    int n = owl_formatter_keyword_count();
    for (int i = 0; i < n; i++) {
        const char* norm = NULL;
        const char* canon = NULL;
        int lang = 0;
        if (!owl_formatter_keyword_at(i, &norm, &canon, &lang)) continue;

        TokenType t = token_for_word(norm);
        if (t == TOKEN_EOF) continue;              /* pas un keyword scanner */
        const OwlKeywordDef* def = owl_keyword_def_for(t);
        if (def == NULL) continue;                 /* pas de fiche user-facing */

        const char* doc = fiche_canon(def);
        if (doc == NULL) continue;

        char nDoc[128], nFmt[128];
        owl_normalize_name(doc, nDoc, sizeof nDoc);
        owl_normalize_name(canon, nFmt, sizeof nFmt);
        /* Mots differents (entree de la langue opposee, synonyme rabattu sur
         * un autre canon) : la fiche n'expose pas de canon pour ce mot-la. */
        if (strcmp(nDoc, nFmt) != 0) continue;

        char aDoc[128], aFmt[128];
        accent_lower(doc, aDoc, sizeof aDoc);
        accent_lower(canon, aFmt, sizeof aFmt);
        if (strcmp(aDoc, aFmt) != 0) {
            fail("fmt_canon_accent_divergence", norm, doc, canon);
        } else {
            g_pass++;
        }
    }
}

/* ---- R5 : meme confrontation, sur la CASSE ---- */
static void run_case_divergence(void) {
    int n = owl_formatter_keyword_count();
    for (int i = 0; i < n; i++) {
        const char* norm = NULL;
        const char* canon = NULL;
        if (!owl_formatter_keyword_at(i, &norm, &canon, NULL)) continue;

        TokenType t = token_for_word(norm);
        if (t == TOKEN_EOF) continue;
        const OwlKeywordDef* def = owl_keyword_def_for(t);
        if (def == NULL) continue;

        const char* doc = fiche_canon(def);
        if (doc == NULL) continue;

        char nDoc[128], nFmt[128];
        owl_normalize_name(doc, nDoc, sizeof nDoc);
        owl_normalize_name(canon, nFmt, sizeof nFmt);
        if (strcmp(nDoc, nFmt) != 0) continue;   /* mot different : hors perimetre */

        /* Meme mot, meme langue : une seule forme ecrite possible. Egalite
         * byte-a-byte — accents (couverts aussi par R1) ET casse. */
        if (strcmp(doc, canon) != 0) {
            fail("fmt_canon_case_divergence", norm, doc, canon);
        } else {
            g_pass++;
        }
    }
}

/* ---- R3 : fmt_keywords[] contre CONTROL_KINDS (blocs DSL) ---- */
static void run_dsl_divergence(void) {
    int n = owl_formatter_keyword_count();
    for (int i = 0; i < n; i++) {
        const char* norm = NULL;
        const char* canon = NULL;
        if (!owl_formatter_keyword_at(i, &norm, &canon, NULL)) continue;

        const OwlControlKindInfo* info = NULL;
        if (!owl_control_kind_from_token(norm, (int)strlen(norm), NULL, &info)) continue;
        if (info == NULL) continue;

        char nFmt[128], aFmt[128];
        owl_normalize_name(canon, nFmt, sizeof nFmt);
        accent_lower(canon, aFmt, sizeof aFmt);

        /* Le canon de reference est celui de la MEME langue que l'entree :
         * on compare la forme du formatter a celle du registre DSL qui
         * designe le meme mot (nameFR ou nameEN). */
        const char* refs[2] = { info->nameFR, info->nameEN };
        for (int r = 0; r < 2; r++) {
            if (refs[r] == NULL) continue;
            char nRef[128], aRef[128];
            owl_normalize_name(refs[r], nRef, sizeof nRef);
            if (strcmp(nRef, nFmt) != 0) continue;
            accent_lower(refs[r], aRef, sizeof aRef);
            if (strcmp(aRef, aFmt) != 0) {
                fail("fmt_canon_dsl_divergence", norm, refs[r], canon);
            } else {
                g_pass++;
            }
            break;
        }
    }
}

/* ---- R4 : la forme emise doit etre une cle du formatter ---- */
static void run_idempotence(void) {
    int n = owl_formatter_keyword_count();
    for (int i = 0; i < n; i++) {
        const char* norm = NULL;
        const char* canon = NULL;
        if (!owl_formatter_keyword_at(i, &norm, &canon, NULL)) continue;

        char nCanon[128];
        owl_normalize_name(canon, nCanon, sizeof nCanon);

        /* Une entree au moins doit reconnaitre la forme emise ET rendre
         * exactement cette forme. Sinon un second save la reecrirait
         * autrement (ou pas du tout) : le canon n'est pas un point fixe. */
        bool fixed = false;
        for (int j = 0; j < n && !fixed; j++) {
            const char* jn = NULL;
            const char* jc = NULL;
            if (!owl_formatter_keyword_at(j, &jn, &jc, NULL)) continue;
            if (strcmp(jn, nCanon) == 0 && strcmp(jc, canon) == 0) fixed = true;
        }
        if (!fixed) {
            fail("fmt_canon_not_idempotent", norm, nCanon, canon);
        } else {
            g_pass++;
        }
    }
}

/* ---- R2 : mot accentue par la fiche, absent du formatter ---- */
static void run_unhandled_coverage(void) {
    for (int t = 0; t < TOKEN_COUNT; t++) {
        if (!is_keyword_user_facing((TokenType)t)) continue;
        const OwlKeywordDef* def = owl_keyword_def_for((TokenType)t);
        if (def == NULL) continue;
        const char* doc = fiche_canon(def);
        if (doc == NULL || !has_accent(doc)) continue;

        char nDoc[128];
        owl_normalize_name(doc, nDoc, sizeof nDoc);

        bool handled = false;
        int n = owl_formatter_keyword_count();
        for (int i = 0; i < n && !handled; i++) {
            const char* norm = NULL;
            if (!owl_formatter_keyword_at(i, &norm, NULL, NULL)) continue;
            if (strcmp(norm, nDoc) == 0) handled = true;
        }
        if (!handled) {
            g_warn++;
            fprintf(stderr, "  WARN [fmt_canon_keyword_unhandled] %s : "
                            "fiche=\"%s\", aucune entree fmt_keywords[] — "
                            "la forme survit par absence de traitement\n",
                    nDoc, doc);
        }
    }
}

int fmt_canon_self_test_run(void) {
    fprintf(stderr, "fmt-canon: confrontation fiche keyword <-> fmt_keywords[]\n");
    run_accent_divergence();
    run_case_divergence();
    run_dsl_divergence();
    run_idempotence();
    run_unhandled_coverage();
    fprintf(stderr, "fmt-canon: %d cas confrontes (%d pass, %d fail, %d warn)\n",
            g_pass + g_fail, g_pass, g_fail, g_warn);

    /* Garde ensemble vide : ce controle confronte deux tables
     * (fiches keyword et fmt_keywords[]). Si l'une devient vide ou n'est plus
     * reconnue, les cinq boucles tournent zero fois et le controle rend vert
     * en n'ayant confronte aucun mot. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "fmt-canon: CORPUS VIDE — 0 mot confronte. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
