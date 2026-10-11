// SPDX-License-Identifier: AGPL-3.0-only
/* Garde de coherence « ce mot-cle ouvre-t-il un bloc ferme par FIN ? »
 *. Execute par `owl --self-test-block-openers`.
 *
 * CE QUE LE GARDE PROTEGE
 * -----------------------
 * Quatre constructions du langage s'ouvrent sur un mot-cle qui, ailleurs, ouvre
 * un bloc ferme par FIN — mais elles n'ont PAS de FIN :
 *   1. `SI cond ALORS <instruction>`   (forme mono-ligne)
 *   2. `CONSTANTE K = 3`               (forme inline)
 *   3. `p est une Procedure`           (annotation de type post-copule)
 *   4. `f est une Fonction`            (idem)
 * Tout scan lexical qui suit une profondeur de blocs doit les reconnaitre. S'il
 * les compte comme ouvrants, le FIN de l'element englobant referme la fausse
 * ouverture : le scan continue au-dela de la fin de l'element et tout ce qui
 * suit la construction sort du niveau attendu. Les pannes qui en decoulent sont
 * MUETTES ou trompeuses — « fonction/procedure inconnue » sur un source legal,
 * ou un refus d'auto-ouverture accompagne d'un message qui accuse le source.
 *
 * Trois tickets en deux jours (, , ) ont corrige un
 * site chacun, alors que la regle correcte existait deja ailleurs dans l'arbre.
 *  a fait de `prescan_depth_delta` (prescan.h) la source de verite
 * unique et branche dessus le detecteur GO (main.c) et la pre-passe d'element
 * (compiler.c) ; ce garde empeche un futur site de repartir en clone.
 *
 * DEUX ETAGES
 * -----------
 * ETAGE 1 — la regle elle-meme. Appel direct de prescan_depth_delta sur chaque
 * construction du corpus : les 4 formes sans FIN doivent rendre 0, leurs
 * jumelles a bloc +1, et aucun des mots-cles sains ne doit jamais etre classe
 * « inline ».
 *
 * ETAGE 2 — les CONSOMMATEURS, sur le meme corpus. Chaque construction est
 * placee dans un corps de Fenetre, entre un handler qui appelle un helper et la
 * declaration de ce helper. Deux verdicts sont confrontes :
 *   - le compilateur doit compiler PROPRE (la pre-passe d'element doit encore
 *     voir le helper declare APRES la construction) ;
 *   - go_local_detect_fenetre doit reconnaitre le fichier-fenetre (le detecteur
 *     doit encore trouver le FIN qui referme la Fenetre).
 * Une divergence dans un sens comme dans l'autre est attrapee : sur-comptage ->
 * helper introuvable + fenetre jamais refermee ; sous-comptage -> le FIN de la
 * construction est pris pour celui de la Fenetre, du code apparait « apres la
 * fenetre » et le detecteur refuse.
 *
 * CE QUE LE GARDE N'AFFIRME PAS (divergences legitimes, mesurees)
 * --------------------------------------------------------------
 *  - CLASSE / INTERFACE / FENETRE : rejetes dans un corps de Fenetre
 *    (E_SEM_DSL_TYPE_REJECTED_IN_WINDOW, 1 classe = 1 fichier). Ils n'arrivent
 *    aux scans qu'apres la copule (`X est une Classe`), branche traitee au site
 *    d'appel — pas par la regle lexicale. Etage 2 ne peut pas les exercer.
 *  - AVEC : le token sert AUSSI de modificateur sans FIN a l'interieur d'autres
 *    instructions (`POUR TOUT … AVEC <cond>`, `Position(…) AVEC <options>`).
 *    Son discriminant n'est ni le token suivant ni la copule mais la POSITION
 *    dans la ligne (instruction = en tete de ligne), donc il n'est pas
 *    exprimable dans la regle lexicale, qui ne le classe pas. Etage 2 exerce
 *    les DEUX emplois (`avec` et `avec_modif`) : c'est la seule facon de
 *    verrouiller le site qui, lui, doit les distinguer (le detecteur GO).
 *
 * VALIDATION PAR MUTATION (mesuree, cf. rapport )
 * ------------------------------------------------------
 *  - Recompter TOKEN_CONSTANTE inconditionnellement dans la pre-passe
 *    d'element (l'etat d'avant ) -> etage 2 rouge sur const_inline.
 *  - Retirer le peek du SI dans prescan_depth_delta -> etage 1 rouge sur
 *    si_inline, etage 2 rouge sur si_inline (compile + detecteur).
 *  - Retirer TOKEN_POUR de prescan_depth_delta (sous-comptage) -> etage 2
 *    rouge sur pour.
 *
 * VM fraiche par cas (comme le harnais compile-error) : le compilateur ecrit
 * dans des tables globales, deux cas ne doivent pas se polluer. */

#include "../vm.h"
#include "../compiler.h"
#include "../prescan.h"
#include "../scanner.h"
#include "../formatter.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Detecteur GO : static-free, declare dans main.c (meme convention que
 * go_local_test.c). */
extern bool go_local_detect_fenetre(const char* source, char* outName,
                                    int outCapacity);

/*  — sondes des surfaces qui suivent une profondeur de blocs.
 * Non-static chez elles pour etre exercables ici, comme go_local_detect_fenetre. */
extern int repl_count_block_depth(const char* source);
extern int registry_self_test_rule54_probe_depth(const char* source);

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 * ETAGE 1 — la regle lexicale
 * ================================================================ */

typedef struct {
    const char* label;
    const char* src;      /* commence par le token a classer */
    int         expected; /* delta attendu */
} RuleCase;

/* Les sources partent SUR le mot-cle : le harnais lit le premier token puis
 * interroge la regle avec le scanner positionne juste apres, exactement comme
 * un consommateur en pleine boucle. */
static const RuleCase RULE_CASES[] = {
    /* Les 4 constructions sans FIN. PROCEDURE/FONCTION post-copule ne sont pas
     * visibles de la regle (aucun lookbehind) : la regle les classe ouvrants,
     * la neutralisation est au site d'appel — c'est l'etage 2 qui le prouve. */
    { "si_inline",        "SI n > 0 ALORS Trace(\"x\")\n",       0 },
    { "const_inline",     "CONSTANTE K = 3\n",                   0 },
    /* Jumelles a bloc : meme mot-cle, verdict oppose. */
    { "si_bloc",          "SI n > 0 ALORS\n  Trace(\"x\")\nFIN\n", +1 },
    { "const_bloc",       "CONSTANTE\n  K = 3\nFIN\n",           +1 },
    /* Defaut conservateur sur source tronquee / malformee : plutot compter un
     * bloc que droper le FIN du contexte englobant. */
    { "si_sans_alors",    "SI n > 0\n",                          +1 },
    { "si_alors_eof",     "SI n > 0 ALORS",                      +1 },
    { "const_eof",        "CONSTANTE",                           +1 },
    /* FIN referme. */
    { "fin",              "FIN\n",                               -1 },
    /* Mots-cles sains classes par la regle : jamais 0. */
    { "pour",             "POUR i = 1 A 3\n  Trace(i)\nFIN\n",   +1 },
    { "tantque",          "TANTQUE Faux\n  Trace(1)\nFIN\n",     +1 },
    { "boucle",           "BOUCLE\n  SORTIR\nFIN\n",             +1 },
    { "selon",            "SELON n\n  CAS 1 : Trace(1)\nFIN\n",  +1 },
    { "quand",            "QUAND EXCEPTION DANS\n  Trace(1)\nFIN\n", +1 },
    { "proprietes",       "PROPRIETES\n  Titre = \"t\"\nFIN\n",  +1 },
    { "procedure",        "PROCEDURE p()\nFIN\n",                +1 },
    { "fonction",         "FONCTION f()\nFIN\n",                 +1 },
};

static void run_rule_stage(void) {
    int n = (int)(sizeof RULE_CASES / sizeof RULE_CASES[0]);
    for (int i = 0; i < n; i++) {
        const RuleCase* c = &RULE_CASES[i];
        Scanner sc;
        scanner_init(&sc, c->src);
        Token tok = scanner_scan_token(&sc);
        Scanner peek = sc;
        int got = prescan_depth_delta(tok, &peek);
        char lbl[160];
        snprintf(lbl, sizeof lbl, "regle/%s : delta attendu %+d, obtenu %+d",
                 c->label, c->expected, got);
        CHECK(got == c->expected, lbl);
        /* La regle ne doit jamais laisser le scanner du caller avance : les
         * consommateurs passent une copie et repartent de leur propre position.
         * Un peek non restaure ferait sauter des tokens au caller. */
        snprintf(lbl, sizeof lbl, "regle/%s : peek restaure", c->label);
        CHECK(peek.current == sc.current, lbl);
    }
}

/* ================================================================
 * ETAGE 2 — les consommateurs, sur le meme corpus
 * ================================================================ */

/* Ou inserer la construction dans le fichier-fenetre forge. */
typedef enum {
    AT_BODY,     /* niveau body de la Fenetre (declarations) */
    AT_HANDLER   /* dans le corps du premier handler (instructions) */
} InsertAt;

typedef struct {
    const char* label;
    const char* fragment;   /* deja indente pour sa position */
    InsertAt    where;
} SiteCase;

static const SiteCase SITE_CASES[] = {
    /* Les 4 constructions sans FIN — le coeur du garde. */
    { "const_inline",  "    CONSTANTE K = 3\n",                             AT_BODY },
    { "si_inline",     "        SI Vrai ALORS Trace(\"x\")\n",              AT_HANDLER },
    { "proc_copule",   "    p est une Procedure\n",                         AT_BODY },
    { "fonc_copule",   "    f est une Fonction\n",                          AT_BODY },
    /* Jumelles a bloc. */
    { "const_bloc",    "    CONSTANTE\n        K = 3\n    FIN\n",           AT_BODY },
    { "si_bloc",       "        SI Vrai ALORS\n            Trace(\"x\")\n        FIN\n", AT_HANDLER },
    /* Mots-cles sains, forme bloc, dans le contexte ou ils sont legaux. */
    { "pour",          "        POUR i = 1 A 3\n            Trace(i)\n        FIN\n", AT_HANDLER },
    { "tantque",       "        TANTQUE Faux\n            Trace(\"x\")\n        FIN\n", AT_HANDLER },
    { "boucle",        "        BOUCLE\n            SORTIR\n        FIN\n", AT_HANDLER },
    { "selon",         "        SELON 1\n            CAS 1 : Trace(\"un\")\n        FIN\n", AT_HANDLER },
    { "quand",         "        QUAND EXCEPTION DANS\n            Trace(\"x\")\n"
                       "        FAIRE\n            Trace(\"y\")\n        FIN\n", AT_HANDLER },
    { "avec",          "        AVEC MoiMeme\n            Trace(\"x\")\n        FIN\n", AT_HANDLER },
    /* Le MEME mot-cle en modificateur, sans FIN : une ligne, un seul FIN a
     * equilibrer. Compter les deux emplois pareil fait +2 pour 1 FIN. */
    { "avec_modif",    "        tb est un tableau de 3 entiers\n"
                       "        POUR TOUT v DE tb AVEC v > 0\n"
                       "            Trace(v)\n        FIN\n",           AT_HANDLER },
    { "structure",     "    st est une Structure\n        n est un entier\n    FIN\n", AT_BODY },
    { "enumeration",   "    e est une Enumeration\n        Rouge\n        Vert\n    FIN\n", AT_BODY },
    { "combinaison",   "    cb est une Combinaison\n        Xx\n        Yy\n    FIN\n", AT_BODY },
    /* Sous-widget : le PROPRIETES imbrique et le FIN du widget doivent
     * s'equilibrer comme le reste. */
    { "widget",        "    LIB_A est un Champ Libelle\n        PROPRIETES\n"
                       "            Libelle = \"a\"\n        FIN\n    FIN\n", AT_BODY },
};

/* Forge le fichier-fenetre du cas. La construction est placee ENTRE l'appel du
 * helper et sa declaration : c'est ce qui rend un sur-comptage observable (la
 * pre-passe d'element ne voit plus le helper) au lieu de rester latent. */
static void forge_source(char* out, size_t cap, const char* fenName,
                         const SiteCase* c) {
    snprintf(out, cap,
        "%s est une Fenetre\n"
        "    PROPRIETES\n"
        "        Titre = \"garde\"\n"
        "    FIN\n"
        "\n"
        "    PROCEDURE surOuverture()\n"
        "%s"
        "        aideDuCas()\n"
        "    FIN\n"
        "\n"
        "%s"
        "    PROCEDURE aideDuCas()\n"
        "        Trace(\"ok\")\n"
        "    FIN\n"
        "FIN\n",
        fenName,
        c->where == AT_HANDLER ? c->fragment : "",
        c->where == AT_BODY    ? c->fragment : "");
}

static void run_site_stage(void) {
    int n = (int)(sizeof SITE_CASES / sizeof SITE_CASES[0]);
    for (int i = 0; i < n; i++) {
        const SiteCase* c = &SITE_CASES[i];
        char fenName[32];
        snprintf(fenName, sizeof fenName, "FEN_G%02d", i);
        char src[4096];
        forge_source(src, sizeof src, fenName, c);

        /* Consommateurs 1 et 2 — pre-scan de fichier (prescan.c) puis pre-passe
         * d'element (compiler.c) : le helper declare APRES la construction doit
         * rester resolu, donc l'appel plus haut doit compiler.
         *
         * Le pre-scan est monte comme en production (`owl --check`, `owl test`) :
         * un harnais qui compilerait sans symtable serait plus strict que le
         * chemin reel et accuserait des sites qu'aucun utilisateur n'atteint —
         * un garde qui crie sur du code legitime coute plus cher que pas de
         * garde. VM fraiche par cas : le compilateur ecrit des globales. */
        vm_init();
        compiler_reset_poo_state();
        SymbolTable* syms = symtable_create();
        prescan_file(syms, src, 0);
        ObjFunction* fn = compile_in_project(src, fenName, syms, 0);
        bool compiled = (fn != NULL);
        symtable_destroy(syms);
        vm_free();

        char lbl[192];
        snprintf(lbl, sizeof lbl,
                 "compilo/%s : compile propre (helper declare apres la "
                 "construction encore resolu)", c->label);
        CHECK(compiled, lbl);

        /* Consommateur 2 — detecteur GO (main.c) : le fichier doit rester
         * reconnu comme fichier-fenetre pur, avec le bon nom. */
        char detected[128] = {0};
        bool isFen = go_local_detect_fenetre(src, detected, (int)sizeof detected);
        snprintf(lbl, sizeof lbl,
                 "detecteur/%s : fichier-fenetre reconnu", c->label);
        CHECK(isFen, lbl);
        snprintf(lbl, sizeof lbl,
                 "detecteur/%s : nom %s attendu, obtenu %s",
                 c->label, fenName, detected[0] ? detected : "(vide)");
        CHECK(strcmp(detected, fenName) == 0, lbl);
    }
}

/* ================================================================
 * ETAGE 3 — UN VERROU PAR SURFACE
 * ================================================================
 *
 *  puis  ont supprime quatre clones ;  les vingt-six
 * survivants. L'etage 2 ci-dessus n'exerce que DEUX consommateurs (la pre-passe
 * d'element et le detecteur GO) : les autres surfaces pouvaient repartir en
 * clone sans que rien ne rougisse. Chacune a desormais son verrou, sur le meme
 * corpus de constructions.
 *
 * Ce que chaque verrou attrape, mesure par mutation (cf. rapport ) :
 *  - REPL      : recompter `SI c ALORS instr` -> profondeur 1 au lieu de 0,
 *                l'invite de continuation ne retombe jamais et `quitter` est
 *                avale avec le reste du tampon (sortie 0, session perdue).
 *  - EXCEPTION : recompter le meme SI dans un corps ENFIN -> le FIN du QUAND
 *                est avale, « 'FIN' attendu apres le bloc de traitement
 *                d'exception » sur une source legale.
 *  - FORMATEUR : recompter -> la ligne suivante gagne un cran d'indentation
 *                qui ne se referme jamais, tout le reste du fichier decale.
 *  - REGLE 54  : recompter -> le cadre de la procedure n'est jamais depile,
 *                et une assertion reellement flottante passe pour couverte.
 * ================================================================ */

/* --- Corpus commun aux surfaces qui rendent une PROFONDEUR ------------- */

typedef struct {
    const char* label;
    const char* src;
    int         expected;   /* profondeur attendue en fin de source */
} DepthCase;

static const DepthCase DEPTH_CASES[] = {
    /* Les formes sans FIN : ne doivent rien laisser d'ouvert. */
    { "si_inline",       "SI Vrai ALORS Trace(\"ok\")\n",                      0 },
    { "const_inline",    "CONSTANTE K = 3\n",                                  0 },
    { "proc_copule",     "p est une Procedure\n",                              0 },
    /* Jumelles a bloc, non refermees : exactement un cran ouvert. */
    { "si_bloc",         "SI Vrai ALORS\n    Trace(\"ok\")\n",                 1 },
    { "const_bloc",      "CONSTANTE\n    K = 3\n",                             1 },
    /* Jumelles a bloc, refermees : retour a zero. */
    { "si_bloc_ferme",   "SI Vrai ALORS\n    Trace(\"ok\")\nFIN\n",            0 },
    { "const_bloc_ferme","CONSTANTE\n    K = 3\nFIN\n",                        0 },
    /* `SINON SI` : une seule chaine, un seul FIN. */
    { "sinon_si",        "SI Vrai ALORS\n    Trace(\"a\")\n"
                         "SINON SI Faux ALORS\n    Trace(\"b\")\nFIN\n",       0 },
    /* Mots-cles sains, forme bloc refermee. */
    { "pour",            "POUR i = 1 A 3\n    Trace(i)\nFIN\n",                0 },
    { "tantque",         "TANTQUE Faux\n    Trace(1)\nFIN\n",                  0 },
    { "boucle",          "BOUCLE\n    SORTIR\nFIN\n",                          0 },
    { "selon",           "SELON n\n    CAS 1 : Trace(1)\nFIN\n",               0 },
    { "quand",           "QUAND EXCEPTION DANS\n    Trace(1)\nFIN\n",          0 },
    { "proprietes",      "PROPRIETES\n    Titre = \"t\"\nFIN\n",               0 },
    { "procedure",       "PROCEDURE p()\n    Trace(1)\nFIN\n",                 0 },
};

/* Le meme corpus moins les cas hors perimetre d'une surface donnee. La regle 54
 * ne voit pas la copule (`p est une Procedure` la laisse en attente de nom) et
 * n'a jamais suivi PROPRIETES : ces deux cas sont exclus de SON verrou, et
 * uniquement du sien. */
static bool depth_case_applies_to_rule54(const char* label) {
    return strcmp(label, "proc_copule") != 0
        && strcmp(label, "proprietes")  != 0;
}

static void run_repl_stage(void) {
    int n = (int)(sizeof DEPTH_CASES / sizeof DEPTH_CASES[0]);
    for (int i = 0; i < n; i++) {
        const DepthCase* c = &DEPTH_CASES[i];
        int got = repl_count_block_depth(c->src);
        char lbl[192];
        snprintf(lbl, sizeof lbl,
                 "repl/%s : profondeur attendue %d, obtenue %d",
                 c->label, c->expected, got);
        CHECK(got == c->expected, lbl);
    }
    /* Le modificateur AVEC ne doit pas compter comme une instruction a bloc :
     * `POUR TOUT … AVEC cond` ferait +2 pour un seul FIN. Son jumeau
     * instruction, en tete de ligne, doit compter. Le discriminant est la
     * position, pas le jeton suivant — d'ou le cas dedie. */
    {
        int got = repl_count_block_depth(
            "t est un tableau de 3 entiers\n"
            "POUR TOUT v DE t AVEC v > 0\n    Trace(v)\nFIN\n");
        char lbl[160];
        snprintf(lbl, sizeof lbl,
                 "repl/avec_modif : profondeur attendue 0, obtenue %d", got);
        CHECK(got == 0, lbl);
    }
    {
        int got = repl_count_block_depth("AVEC MoiMeme\n    Trace(\"x\")\n");
        char lbl[160];
        snprintf(lbl, sizeof lbl,
                 "repl/avec_instr : profondeur attendue 1, obtenue %d", got);
        CHECK(got == 1, lbl);
    }
}

static void run_rule54_stage(void) {
    int n = (int)(sizeof DEPTH_CASES / sizeof DEPTH_CASES[0]);
    for (int i = 0; i < n; i++) {
        const DepthCase* c = &DEPTH_CASES[i];
        if (!depth_case_applies_to_rule54(c->label)) continue;
        int got = registry_self_test_rule54_probe_depth(c->src);
        char lbl[192];
        snprintf(lbl, sizeof lbl,
                 "regle54/%s : profondeur attendue %d, obtenue %d",
                 c->label, c->expected, got);
        CHECK(got == c->expected, lbl);
    }
    /* Le cas qui rend la regle 54 aveugle : une procedure dont le corps
     * contient un SI mono-ligne doit etre DEPILEE par son FIN. Sinon son cadre
     * reste ouvert et toute assertion posee apres passe pour couverte. */
    {
        int got = registry_self_test_rule54_probe_depth(
            "PROCEDURE scenario_a()\n"
            "    SI Vrai ALORS Trace(\"x\")\n"
            "FIN\n");
        char lbl[176];
        snprintf(lbl, sizeof lbl,
                 "regle54/proc_si_inline : cadre depile (profondeur attendue 0, "
                 "obtenue %d)", got);
        CHECK(got == 0, lbl);
    }
}

/* --- Surface EXCEPTION : le corps ENFIN et le peek qui le precede ------ */

typedef struct {
    const char* label;
    const char* fragment;   /* indente pour un corps de clause (8 espaces) */
} ExcCase;

static const ExcCase EXC_CASES[] = {
    { "si_inline",  "        SI Vrai ALORS Trace(\"x\")\n" },
    { "si_bloc",    "        SI Vrai ALORS\n            Trace(\"x\")\n        FIN\n" },
    { "sinon_si",   "        SI Vrai ALORS\n            Trace(\"a\")\n"
                    "        SINON SI Faux ALORS\n            Trace(\"b\")\n        FIN\n" },
    { "pour",       "        POUR i = 1 A 3\n            Trace(i)\n        FIN\n" },
    { "quand",      "        QUAND EXCEPTION DANS\n            Trace(\"x\")\n"
                    "        FAIRE\n            Trace(\"y\")\n        FIN\n" },
};

/* Trois emplacements, trois clones distincts du compteur : le corps ENFIN
 * (enfin_capture_body), le corps TRY (le peek `hasEnfinAhead` qui cherche
 * ENFIN avant d'emettre) et le corps FAIRE. */
typedef enum { EXC_AT_TRY, EXC_AT_FAIRE, EXC_AT_ENFIN } ExcSlot;

static void forge_exception_source(char* out, size_t cap, const char* frag,
                                   ExcSlot slot) {
    snprintf(out, cap,
        "PROCEDURE gardeExc()\n"
        "    QUAND EXCEPTION DANS\n"
        "        Trace(\"try\")\n"
        "%s"
        "    FAIRE\n"
        "        Trace(\"catch\")\n"
        "%s"
        "    ENFIN\n"
        "        Trace(\"cleanup\")\n"
        "%s"
        "    FIN\n"
        "FIN\n",
        slot == EXC_AT_TRY   ? frag : "",
        slot == EXC_AT_FAIRE ? frag : "",
        slot == EXC_AT_ENFIN ? frag : "");
}

static void run_exception_stage(void) {
    static const ExcSlot SLOTS[]      = { EXC_AT_TRY, EXC_AT_FAIRE, EXC_AT_ENFIN };
    static const char*   SLOT_NAMES[] = { "try",      "faire",      "enfin"      };
    int n = (int)(sizeof EXC_CASES / sizeof EXC_CASES[0]);
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < n; i++) {
            char src[2048];
            forge_exception_source(src, sizeof src, EXC_CASES[i].fragment, SLOTS[s]);

            vm_init();
            compiler_reset_poo_state();
            SymbolTable* syms = symtable_create();
            prescan_file(syms, src, 0);
            ObjFunction* fn = compile_in_project(src, "gardeExc", syms, 0);
            bool compiled = (fn != NULL);
            symtable_destroy(syms);
            vm_free();

            char lbl[224];
            snprintf(lbl, sizeof lbl,
                     "exception/%s@%s : compile propre (le FIN du QUAND n'est "
                     "pas avale)", EXC_CASES[i].label, SLOT_NAMES[s]);
            CHECK(compiled, lbl);
        }
    }
}

/* --- Surface FORMATEUR : le cran d'indentation qui suit la construction - */

typedef struct {
    const char* label;
    const char* line;      /* la construction, seule sur sa ligne, non indentee */
    bool        opens;     /* ouvre-t-elle un cran ? (donc un FIN de plus) */
} FmtCase;

/* Le marqueur est pose juste APRES la construction, dans un corps de
 * procedure : niveau 1 si la construction n'ouvre rien, 2 sinon. */
static const FmtCase FMT_CASES[] = {
    { "si_inline",    "SI Vrai ALORS Trace(\"x\")", false },
    { "const_inline", "CONSTANTE K = 3",            false },
    { "si_bloc",      "SI Vrai ALORS",              true  },
    { "const_bloc",   "CONSTANTE",                  true  },
    { "pour",         "POUR i = 1 A 3",             true  },
    { "tantque",      "TANTQUE Faux",               true  },
};

/* Niveau d'indentation de la premiere ligne contenant `needle`, ou -1.
 * Le formateur canon indente a la TABULATION, un cran par niveau. */
static int indent_level_of_line_with(const char* text, const char* needle) {
    const char* hit = strstr(text, needle);
    if (hit == NULL) return -1;
    const char* lineStart = hit;
    while (lineStart > text && lineStart[-1] != '\n') lineStart--;
    int level = 0;
    while (lineStart[level] == '\t') level++;
    return level;
}

static void run_formatter_stage(void) {
    int n = (int)(sizeof FMT_CASES / sizeof FMT_CASES[0]);
    for (int i = 0; i < n; i++) {
        const FmtCase* c = &FMT_CASES[i];
        /* Source volontairement PLATE : c'est le formateur qui doit poser
         * l'indentation, on ne lui souffle pas la reponse. */
        char src[512];
        snprintf(src, sizeof src,
                 "PROCEDURE gardeFmt()\n%s\nTrace(\"marqueur\")\n%sFIN\n",
                 c->line, c->opens ? "FIN\n" : "");
        char* out = owl_format(src);
        char lbl[224];
        snprintf(lbl, sizeof lbl, "formateur/%s : owl_format rend un resultat",
                 c->label);
        CHECK(out != NULL, lbl);
        if (out == NULL) continue;
        int want = c->opens ? 2 : 1;
        int got = indent_level_of_line_with(out, "marqueur");
        snprintf(lbl, sizeof lbl,
                 "formateur/%s : marqueur attendu au niveau %d, obtenu %d",
                 c->label, want, got);
        CHECK(got == want, lbl);
        free(out);
    }
}

int block_opener_guard_test_main(void) {
    fprintf(stderr, "\n==== BLOCK OPENER GUARD (fix/1441, fix/1501) ====\n");

    run_rule_stage();
    run_site_stage();
    run_repl_stage();
    run_formatter_stage();
    run_rule54_stage();
    run_exception_stage();   /* en dernier : c'est le seul qui compile */

    int total = g_pass + g_fail;
    fprintf(stderr, "block-openers: %d cas (%d regle, %d consommateurs, "
                    "%d profondeur x2 surfaces, %d exception x3 emplacements, "
                    "%d formateur) — %d pass, %d fail\n",
            total,
            (int)(sizeof RULE_CASES / sizeof RULE_CASES[0]),
            (int)(sizeof SITE_CASES / sizeof SITE_CASES[0]),
            (int)(sizeof DEPTH_CASES / sizeof DEPTH_CASES[0]),
            (int)(sizeof EXC_CASES / sizeof EXC_CASES[0]),
            (int)(sizeof FMT_CASES / sizeof FMT_CASES[0]),
            g_pass, g_fail);

    /* Garde ensemble vide (patron ) : un controle qui n'examine rien ne
     * prouve rien. */
    if (total == 0) {
        fprintf(stderr, "block-openers: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    return g_fail;
}
