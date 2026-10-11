// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du depth tracker de prescan_file() ( Lot C +
 *  consolidation) :
 *
 * Reactive une couverture perdue lors du chantier canon strict
 *. L'ancien test OWL `window_constante_with_post_code`
 * exercait ce bug via "code top-level apres FIN Fenetre" — interdit
 * sous canon strict. On le rejoue ici en C pur en appelant
 * directement prescan_file() sur une source forgee.
 *
 * Cas vises :
 *  1. Blocs declaratifs imbriques dans une Fenetre (CONSTANTE,
 *     STRUCTURE, ENUMERATION, COMBINAISON). Le FIN du bloc imbrique
 *     NE DOIT PAS etre confondu avec le FIN de la Fenetre. Si le
 *     depth tracker se trompe, le depth tombe a 0 prematurement et
 *     un sous-widget BTN_OK declare DANS le bloc Fenetre serait
 *     re-catalogue comme global au depth 0 (SYM_VARIABLE). De meme,
 *     les membres des blocs imbriques (FOO, nVal, VAL_A…) ne doivent
 *     jamais apparaitre comme symboles top-level.
 *
 *  2.  — Blocs de controle lexicaux (SI/POUR/TANTQUE/SELON/
 *     BOUCLE/QUAND) dans le body d'une methode de classe ou d'une
 *     procedure globale. Leur FIN ne doit pas faire dribbler le
 *     depth et faire ressortir la procedure/methode suivante comme
 *     SYM_PROCEDURE top-level fantome. Test du symtable global : la
 *     2e methode/procedure ne doit PAS y figurer (cas methode) ou
 *     doit y figurer une SEULE fois (cas globale, pas en double).
 *
 *  3.  — Forme inline du SI (`SI cond ALORS stmt`, sans
 *     FIN) ne doit PAS incrementer depth, contrairement a la forme
 *     bloc (`SI cond ALORS\n stmt\n FIN`). Seul le SI a une forme
 *     inline legale (verifie empiriquement coordinator/--check :
 *     POUR/TANTQUE/SELON/BOUCLE/QUAND rejettent leur forme inline
 *     au parsing). Pattern peek inline existant pour CONSTANTE
 *     (prescan.c L990-1000), generalise par .
 *
 * 4 blocs declaratifs sont autorises dans le body Fenetre (cf
 * compiler.c  : enum/struct/combinaison hisses au scope
 * enclosing, plus CONSTANTE) : CONSTANTE / STRUCTURE / ENUMERATION
 * / COMBINAISON. CLASSE et INTERFACE sont rejetees en amont par
 * E_SEM_DSL_TYPE_REJECTED_IN_WINDOW — pas besoin de tracker dans le
 * prescan. La forme inline `CONSTANTE FOO = 1` (sans FIN) ne doit
 * pas non plus incrementer depth.
 *
 * Validation par mutation : forcer le bug dans prescan.c (par ex.
 * supprimer la garde sur les blocs imbriques, ou ajouter un
 * `depth--` parasite au TOKEN_CONSTANTE) doit faire rougir
 * l'assertion BTN_OK_<cas> (BTN_OK apparait alors comme
 * SYM_VARIABLE global). De meme, retirer le tracking POUR/SI/etc.
 * dans une methode fait rougir les assertions de la section 
 * (methode_dribblee apparait comme SYM_PROCEDURE top-level).
 *
 * Pas de vm_init — prescan_file() ne touche que le scanner et son
 * symtable local. Pattern inspire de go_local_test.c. */

#include "../prescan.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/*  : helpers pour les cas "body methode/procedure avec bloc
 * de controle lexical". On verifie le symtable top-level et la liste
 * classMethods : ce qui est dans une classe doit aller dans
 * classMethods (pas dans symbols top-level) ; ce qui est une
 * procedure globale doit y aller exactement une fois. */

/* Compte les occurrences (case-sensible) d'un nom dans symbols. */
static int count_top_level(SymbolTable* t, const char* name, int len) {
    int c = 0;
    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if ((int)strlen(s->name) == len && memcmp(s->name, name, len) == 0) {
            c++;
        }
    }
    return c;
}

/* Verifie qu'un nom n'est PAS catalogue comme SYM_PROCEDURE ou
 * SYM_FUNCTION top-level (i.e. methode mal-cataloguee par dribble). */
static void check_not_top_level_proc(const char* caseLabel,
                                     SymbolTable* t,
                                     const char* name, int len) {
    char buf[160];
    int c = 0;
    for (int i = 0; i < t->count; i++) {
        ProjectSymbol* s = &t->symbols[i];
        if ((s->kind == SYM_PROCEDURE || s->kind == SYM_FUNCTION) &&
            (int)strlen(s->name) == len && memcmp(s->name, name, len) == 0) {
            c++;
        }
    }
    snprintf(buf, sizeof(buf),
             "[%s] %s absent du symtable top-level "
             "(methode mal-cataloguee si fail)",
             caseLabel, name);
    CHECK(c == 0, buf);
}

/* Helper : prescan une source forgee, verifie que FEN_Demo est
 * present comme SYM_WINDOW, que BTN_OK est absent (sous-widget),
 * et que le membre du bloc imbrique (memberName) est absent. */
static void check_window_with_nested_block(const char* caseLabel,
                                            const char* source,
                                            const char* memberName,
                                            int memberLen) {
    SymbolTable* t = symtable_create();
    prescan_file(t, source, 0);

    bool ambig = false;
    ProjectSymbol* fen = symtable_find(t, "FEN_Demo", 8, &ambig);
    char buf[128];
    snprintf(buf, sizeof(buf), "[%s] FEN_Demo present", caseLabel);
    CHECK(fen != NULL, buf);
    if (fen != NULL) {
        snprintf(buf, sizeof(buf), "[%s] FEN_Demo kind == SYM_WINDOW", caseLabel);
        CHECK(fen->kind == SYM_WINDOW, buf);
    }

    /* BTN_OK : ne doit jamais etre catalogue comme global (depth > 0). */
    ProjectSymbol* btn = symtable_find(t, "BTN_OK", 6, &ambig);
    snprintf(buf, sizeof(buf),
             "[%s] BTN_OK absent (sous-widget dans Fenetre, pas global)",
             caseLabel);
    CHECK(btn == NULL, buf);

    /* Membre du bloc imbrique : ne doit jamais apparaitre comme global. */
    if (memberName != NULL) {
        ProjectSymbol* mem = symtable_find(t, memberName, memberLen, &ambig);
        snprintf(buf, sizeof(buf),
                 "[%s] %s absent (membre bloc imbrique, pas global)",
                 caseLabel, memberName);
        CHECK(mem == NULL, buf);
    }

    symtable_destroy(t);
}

int prescan_depth_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    /* Cas 1 : CONSTANTE bloc imbrique. Forme historique du bug
     * (window_constante_with_post_code pre-canon strict). */
    check_window_with_nested_block("CONSTANTE bloc",
        "FEN_Demo est une Fenetre\n"
        "  CONSTANTE\n"
        "    FOO = 1\n"
        "  FIN\n"
        "  BTN_OK est un Bouton\n"
        "  FIN\n"
        "FIN\n",
        "FOO", 3);

    /* Cas 2 : STRUCTURE imbriquee (acceptee par compiler ,
     * hissee au scope enclosing). Meme piege depth tracker. */
    check_window_with_nested_block("STRUCTURE",
        "FEN_Demo est une Fenetre\n"
        "  MaStruct est une Structure\n"
        "    nVal est un entier\n"
        "  FIN\n"
        "  BTN_OK est un Bouton\n"
        "  FIN\n"
        "FIN\n",
        "nVal", 4);

    /* Cas 3 : ENUMERATION imbriquee. */
    check_window_with_nested_block("ENUMERATION",
        "FEN_Demo est une Fenetre\n"
        "  MonEnum est une Enumeration\n"
        "    VAL_A\n"
        "    VAL_B\n"
        "  FIN\n"
        "  BTN_OK est un Bouton\n"
        "  FIN\n"
        "FIN\n",
        "VAL_A", 5);

    /* Cas 4 : COMBINAISON imbriquee. */
    check_window_with_nested_block("COMBINAISON",
        "FEN_Demo est une Fenetre\n"
        "  MaComb est une Combinaison\n"
        "    FLAG_A\n"
        "    FLAG_B\n"
        "  FIN\n"
        "  BTN_OK est un Bouton\n"
        "  FIN\n"
        "FIN\n",
        "FLAG_A", 6);

    /* Cas 5 : CONSTANTE inline (forme `CONSTANTE NAME = value` sans
     * FIN, une seule ligne). Ne doit PAS incrementer depth, et FOO
     * ne doit pas etre catalogue comme global. BTN_OK doit rester
     * sous-widget de la Fenetre. */
    check_window_with_nested_block("CONSTANTE inline",
        "FEN_Demo est une Fenetre\n"
        "  CONSTANTE FOO = 1\n"
        "  BTN_OK est un Bouton\n"
        "  FIN\n"
        "FIN\n",
        "FOO", 3);

    /* ================================================================
     *  — Blocs de controle dans body methode / procedure.
     * ================================================================ */

    /* Cas 6 : POUR dans methode d'une classe. La methode suivante
     * (SuivanteMethode) ne doit PAS apparaitre comme SYM_PROCEDURE
     * top-level. Avant , le FIN du POUR faisait dribbler
     * depth a 0 et SuivanteMethode etait vue comme proc globale. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE PremiereMethode()\n"
            "    POUR i = 1 A 3\n"
            "      Trace(i)\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE SuivanteMethode()\n"
            "    RENVOYER 42\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("POUR-body methode",
            t, "SuivanteMethode", 15);
        check_not_top_level_proc("POUR-body methode",
            t, "PremiereMethode", 15);
        symtable_destroy(t);
    }

    /* Cas 7 : SI bloc dans methode (`SI x ALORS\n stmt\n FIN`). */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    SI :nVal > 0 ALORS\n"
            "      Trace(\"oui\")\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "    Trace(\"m2\")\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("SI-bloc methode", t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 8 : TANTQUE dans methode. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    TANTQUE :nVal > 0\n"
            "      :nVal = :nVal - 1\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("TANTQUE-bloc methode",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 9 : SELON dans methode. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    SELON :nVal\n"
            "      CAS 1: Trace(\"un\")\n"
            "      CAS 2: Trace(\"deux\")\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("SELON-bloc methode",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 10 : QUAND (exception handler) dans methode. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    QUAND EXCEPTION\n"
            "      Trace(\"erreur\")\n"
            "    FIN\n"
            "    Trace(\"normal\")\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("QUAND-bloc methode",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 11 : BOUCLE dans methode. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    BOUCLE\n"
            "      SI :nVal = 0 ALORS\n"
            "        SORTIR\n"
            "      FIN\n"
            "      :nVal = :nVal - 1\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("BOUCLE+SI imbriques methode",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 12 : imbrication profonde POUR > SI > TANTQUE dans
     * methode. Exerce la pile de depth, le tracker doit en sortir
     * proprement (3 ouvrants = 3 FIN apparies). */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    POUR i = 1 A 3\n"
            "      SI i > 1 ALORS\n"
            "        TANTQUE :nVal > 0\n"
            "          :nVal = :nVal - 1\n"
            "        FIN\n"
            "      FIN\n"
            "    FIN\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("POUR>SI>TANTQUE imbriques",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 13 : SI inline (`SI x ALORS y = 1`, sans FIN). Ne doit
     * PAS incrementer depth. M2 doit donc rester dans le body de la
     * classe (pas vue top-level). Avant le peek inline, un +1
     * naivement applique au TOKEN_SI consommerait le FIN suivant
     * (celui de M1) au lieu du FIN final de la classe, ce qui ferait
     * sortir M2 de la classe et l'ajouterait au top-level. */
    {
        const char* src =
            "cMa est une Classe\n"
            "PUBLIC\n"
            "  PROCEDURE M1()\n"
            "    SI :nVal > 0 ALORS :nVal = 0\n"
            "  FIN\n"
            "  PROCEDURE M2()\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        check_not_top_level_proc("SI inline methode",
            t, "M2", 2);
        symtable_destroy(t);
    }

    /* Cas 14 (positif) : procedure globale avec POUR puis seconde
     * procedure globale. Les DEUX doivent etre catalogues
     * top-level, exactement une fois chacune. Verifie qu'on n'a
     * pas casse le cas legitime en consolidant. */
    {
        const char* src =
            "PROCEDURE GlobaleUne()\n"
            "  POUR i = 1 A 3\n"
            "    Trace(i)\n"
            "  FIN\n"
            "FIN\n"
            "\n"
            "PROCEDURE GlobaleDeux()\n"
            "  Trace(\"deux\")\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        char buf[160];
        int c1 = count_top_level(t, "GlobaleUne", 10);
        int c2 = count_top_level(t, "GlobaleDeux", 11);
        snprintf(buf, sizeof(buf),
            "[positif globales] GlobaleUne presente une seule fois (%d)", c1);
        CHECK(c1 == 1, buf);
        snprintf(buf, sizeof(buf),
            "[positif globales] GlobaleDeux presente une seule fois (%d)", c2);
        CHECK(c2 == 1, buf);
        symtable_destroy(t);
    }

    /* Cas 15 (positif) : Fenetre avec sous-widget Bouton dont le
     * handler contient un POUR. Verifie que le tracker ne dribble
     * pas a l'interieur des sous-widgets, et qu'un Bouton suivant
     * reste correctement vu comme sous-widget (absent du top-level).
     * Sous-widgets bloc DSL : nettoyage attendu au prescan. */
    {
        const char* src =
            "FEN_Demo est une Fenetre\n"
            "  BTN_OK est un Bouton\n"
            "    PROCEDURE Clic()\n"
            "      POUR i = 1 A 3\n"
            "        Trace(i)\n"
            "      FIN\n"
            "    FIN\n"
            "  FIN\n"
            "  BTN_KO est un Bouton\n"
            "  FIN\n"
            "FIN\n";
        SymbolTable* t = symtable_create();
        prescan_file(t, src, 0);
        bool amb = false;
        ProjectSymbol* fen = symtable_find(t, "FEN_Demo", 8, &amb);
        CHECK(fen != NULL, "[Fenetre+handler POUR] FEN_Demo present");
        ProjectSymbol* btnOk = symtable_find(t, "BTN_OK", 6, &amb);
        CHECK(btnOk == NULL,
            "[Fenetre+handler POUR] BTN_OK absent top-level "
            "(sous-widget, pas global)");
        ProjectSymbol* btnKo = symtable_find(t, "BTN_KO", 6, &amb);
        CHECK(btnKo == NULL,
            "[Fenetre+handler POUR] BTN_KO absent top-level "
            "(sous-widget, pas global)");
        check_not_top_level_proc("Fenetre+handler POUR",
            t, "Clic", 4);
        symtable_destroy(t);
    }

    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "prescan_depth: CORPUS VIDE — 0 cas execute. Un "
                        "controle qui n'examine rien ne prouve rien.\n");
        return 1;
    }
    if (g_fail > 0) {
        fprintf(stderr, "prescan_depth: %d FAIL, %d PASS\n", g_fail, g_pass);
    } else {
        fprintf(stdout, "prescan_depth: %d PASS\n", g_pass);
    }
    return g_fail;
}
