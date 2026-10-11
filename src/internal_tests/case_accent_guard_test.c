// SPDX-License-Identifier: AGPL-3.0-only
/* case_accent_guard_test.c —  : garde fmt-immune de l'invariant 1
 * (resolution des identifiants casse-insensible ET accent-insensible au
 * runtime, cf. CLAUDE.md regle 6 invariants 1 & 2).
 *
 * Pourquoi en C et pas en .owl : `owl fmt` (invariant 4, declenche a chaque
 * save VSCode) canonicalise la casse ET les accents des identifiants. Un
 * scenario .owl ecrit en casse melangee (`fdisqueencours()`, `httpRequête`)
 * pour prouver la casse-insensibilite est donc reecrit au canon au premier
 * save : l'assertion « casse-insensible » devient identique a l'assertion
 * « bilingue » et ne teste plus rien de specifique a la casse. Le formatter
 * ne touche QUE les .owl ; un snippet porte par une chaine C litterale y est
 * immunise. Le harnais reprend le vehicule de multilingue_print_test.c :
 * vm_interpret(source) + capture stdout via dup/dup2/fichier temp.
 *
 * Ce n'est PAS un test-first rouge-avant-vert : l'invariant 1 fonctionne deja
 * (fix historiques sur locales/globales/procedures). La garde re-teste la
 * resolution de facon fmt-immune ; elle PASSE des sa pose. Un rouge ici
 * signale une regression reelle de resolution, pas un manque a implementer.
 *
 * Chemins de resolution couverts (miroir des 2 matrices .owl neutralisees par
 * fmt : test_identifier_case_accents.owl + test_dispatcher_case_insensible_doubles.owl) :
 *   1. native            — casse + accent omis + forme EN resolvent au canon
 *   2. procedure user    — declaration canon, appel casse+accent melange
 *   3. variable locale   — declaration puis lecture ET ecriture casse+accent
 *   4. variable globale  — idem, chemin de resolution distinct (top-level)
 *   5. mot-cle           — si/SI, tantque/TANTQUE equivalents
 *   6. double type/native — `langue`/`Langue`, `xmldocument`/`XMLDocument`
 *                           casse incorrecte au call site ET en annotation type
 *   7. sites annexes     — appel cross-procedure user, interpolation `[%..%]`,
 *                          constante registry (Vrai) en casse variante
 *   8. identifiant long  — nom > 50 chars relu en casse variante (anti-cap buffer)
 *   9. multi-accents     — 2+ diacritiques dont cedille (variable + procedure),
 *                          tous accents omis a la relecture/appel
 *
 * Deterministe et sans reseau (aucun HTTPRequête reel — le double type/native
 * est exerce par Langue et XMLDocument, meme chemin de dispatch call-site,
 * cf. mapping de couverture du rapport ).
 *
 * Retourne 0 si tous les cas passent, sinon le nombre d'echecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../vm.h"
#include "../ui_catalog.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_open   open
  #define owl_unlink unlink
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

static int  g_pass = 0;
static int  g_fail = 0;
static char g_buf[16 * 1024];

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Capture stdout dans g_buf le temps d'un vm_interpret(source). Pattern
 * dup/dup2/fichier temp (cf. multilingue_print_test.c) — portable MinGW/MSVCRT.
 * Nom de fichier temp propre au processus (fixture_unique) pour ne pas
 * collisionner avec une execution parallele. */
static InterpretResult capture_interpret(const char* source) {
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_case_accent_guard_", ".tmp");
    g_buf[0] = '\0';

    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return INTERPRET_RUNTIME_ERROR;

    int tmp_fd = owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return INTERPRET_RUNTIME_ERROR; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd); owl_close(saved_fd);
        return INTERPRET_RUNTIME_ERROR;
    }
    owl_close(tmp_fd);

    InterpretResult res = vm_interpret(source);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_buf, 1, sizeof(g_buf) - 1, r);
        g_buf[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    return res;
}

/* ============================================================
 * 1. Native — casse + accent omis + forme EN resolvent au canon
 *    Répète (FR, accent) / RepeatString (EN). Un identifiant en casse
 *    melangee accent-omis (`r\xc3\xa9p\xc3\xa8te` -> `repete`) et la forme
 *    EN en majuscules doivent dispatcher sur la meme native.
 * ============================================================ */
static const char* SRC_NATIVE =
    "canon est une cha\xc3\xaene = R\xc3\xa9p\xc3\xa8te(\"ab\", 3)\n"
    "Trace(\"NC=\" + canon)\n"
    "sansAccent est une cha\xc3\xaene = repete(\"ab\", 3)\n"     /* casse + accent omis */
    "Trace(\"NA=\" + sansAccent)\n"
    "formeEN est une cha\xc3\xaene = REPEATSTRING(\"ab\", 3)\n"   /* EN en majuscules */
    "Trace(\"NE=\" + formeEN)\n";

static void test_native(void) {
    InterpretResult res = capture_interpret(SRC_NATIVE);
    CHECK(res == INTERPRET_OK, "native_interpret_ok");
    CHECK(strstr(g_buf, "NC=ababab") != NULL, "native_canon");
    CHECK(strstr(g_buf, "NA=ababab") != NULL, "native_casse_accent_omis");
    CHECK(strstr(g_buf, "NE=ababab") != NULL, "native_forme_en");
}

/* ============================================================
 * 2. Procedure user — declaree canon (Incrémente), appelee en casse
 *    melangee + accent omis (incremente).
 * ============================================================ */
static const char* SRC_PROC =
    "PROCEDURE Incr\xc3\xa9mente(n est un entier)\n"
    "\tRENVOYER n + 1\n"
    "FIN\n"
    "resultat est un entier = incremente(41)\n"   /* casse + accent omis */
    "Trace(\"PU=\" + resultat)\n";

static void test_procedure_user(void) {
    InterpretResult res = capture_interpret(SRC_PROC);
    CHECK(res == INTERPRET_OK, "proc_interpret_ok");
    CHECK(strstr(g_buf, "PU=42") != NULL, "proc_casse_accent_omis");
}

/* ============================================================
 * 3. Variable locale — declaree canon (nClé) dans une procedure, lue
 *    accent omis (nCle) puis ecrite casse + accent (NCLÉ).
 * ============================================================ */
static const char* SRC_LOCALE =
    "PROCEDURE TestLocale()\n"
    "\tnCl\xc3\xa9 est un entier = 700\n"
    "\tTrace(\"LR=\" + nCle)\n"          /* lecture accent omis */
    "\tNCL\xc3\x89 = 900\n"              /* ecriture casse + accent */
    "\tTrace(\"LW=\" + nCl\xc3\xa9)\n"
    "FIN\n"
    "TestLocale()\n";

static void test_variable_locale(void) {
    InterpretResult res = capture_interpret(SRC_LOCALE);
    CHECK(res == INTERPRET_OK, "locale_interpret_ok");
    CHECK(strstr(g_buf, "LR=700") != NULL, "locale_lecture_accent_omis");
    CHECK(strstr(g_buf, "LW=900") != NULL, "locale_ecriture_casse_accent");
}

/* ============================================================
 * 4. Variable globale — declaree canon au top-level (gClé), lue casse +
 *    accent omis (GCLE) puis reecrite. Chemin de resolution distinct des
 *    locales (compile->runtime, cf. header test_identifier_case_accents.owl).
 * ============================================================ */
static const char* SRC_GLOBALE =
    "gCl\xc3\xa9 est un entier = 4242\n"
    "Trace(\"GR=\" + GCLE)\n"           /* lecture casse + accent omis */
    "gcl\xc3\xa9 = 7373\n"              /* ecriture casse variante */
    "Trace(\"GW=\" + gCl\xc3\xa9)\n";

static void test_variable_globale(void) {
    InterpretResult res = capture_interpret(SRC_GLOBALE);
    CHECK(res == INTERPRET_OK, "globale_interpret_ok");
    CHECK(strstr(g_buf, "GR=4242") != NULL, "globale_lecture_casse_accent_omis");
    CHECK(strstr(g_buf, "GW=7373") != NULL, "globale_ecriture_casse");
}

/* ============================================================
 * 5. Mot-cle — si/SI et tantque/TANTQUE equivalents (scanner normalize).
 * ============================================================ */
static const char* SRC_MOTCLE =
    "compteur est un entier = 0\n"
    "tantque compteur < 3\n"            /* mot-cle minuscule */
    "\tcompteur = compteur + 1\n"
    "FIN\n"
    "si compteur = 3 alors\n"           /* mot-cle minuscule */
    "\tTrace(\"MC=ok\")\n"
    "FIN\n"
    "TANTQUE compteur < 5\n"            /* mot-cle majuscule */
    "\tcompteur = compteur + 1\n"
    "FIN\n"
    "Trace(\"MK=\" + compteur)\n";

static void test_mot_cle(void) {
    InterpretResult res = capture_interpret(SRC_MOTCLE);
    CHECK(res == INTERPRET_OK, "motcle_interpret_ok");
    CHECK(strstr(g_buf, "MC=ok") != NULL, "motcle_si_minuscule");
    CHECK(strstr(g_buf, "MK=5") != NULL, "motcle_tantque_casse");
}

/* ============================================================
 * 6. Double type/native — le pattern "nom" = a la fois un type et une
 *    native (cf. invariant 5). Le call site en casse incorrecte doit
 *    dispatcher sur la native, l'annotation type en casse incorrecte doit
 *    resoudre le type. Coeur de test_dispatcher_case_insensible_doubles.owl.
 *    On exerce Langue et XMLDocument (deterministes, sans reseau) : le
 *    dispatcher call-site est native-agnostique, donc ce chemin couvre aussi
 *    le double HTTPRequête de la matrice .owl.
 * ============================================================ */
static const char* SRC_DOUBLE =
    /* double 1 : langue (type) + Langue (native) */
    "lng est une langue = langueFran\xc3\xa7" "ais\n"           /* annotation type casse incorrecte */
    "avant est une cha\xc3\xaene = langue(langueFran\xc3\xa7" "ais)\n" /* call site native casse incorrecte */
    "Trace(\"DL=\" + avant)\n"
    "Langue(avant)\n"                                            /* restore langue courante */
    /* double 2 : xmldocument (type) + XMLDocument (native) */
    "doc est un xmldocument = xmldocument()\n"                  /* type + call site casse incorrecte */
    "si doc <> Null alors\n"
    "\tTrace(\"DX=ok\")\n"
    "FIN\n";

static void test_double_type_native(void) {
    InterpretResult res = capture_interpret(SRC_DOUBLE);
    CHECK(res == INTERPRET_OK, "double_interpret_ok");
    /* langueFrançais -> tag BCP 47 "fr-FR" */
    CHECK(strstr(g_buf, "DL=fr-FR") != NULL, "double_langue_call_site_casse");
    CHECK(strstr(g_buf, "DX=ok") != NULL, "double_xmldocument_call_site_casse");
}

/* ============================================================
 * 7. Sites de resolution annexes couverts par les 2 matrices .owl :
 *    - appel d'une procedure user DEPUIS une autre procedure user, casse +
 *      accent omis (call site different du top-level) ;
 *    - identifiant en casse melangee DANS une interpolation `[%...%]` ;
 *    - constante registry (Vrai) en casse variante (vrai/VRAI).
 * ============================================================ */
static const char* SRC_ANNEXES =
    "PROCEDURE Salu\xc3\xa9()\n"
    "\tTrace(\"CP=ok\")\n"
    "FIN\n"
    "PROCEDURE Appelant()\n"
    "\tsalue()\n"                       /* appel cross-procedure casse + accent omis */
    "FIN\n"
    "Appelant()\n"
    "nPrixInterp est un entier = 123\n"
    "msg est une cha\xc3\xaene = `[%nprixinterp%]`\n" /* interpolation casse variante */
    "Trace(\"IP=\" + msg)\n"
    "si vrai alors\n"                   /* constante registry casse variante */
    "\tTrace(\"CO=ok\")\n"
    "FIN\n";

static void test_sites_annexes(void) {
    InterpretResult res = capture_interpret(SRC_ANNEXES);
    CHECK(res == INTERPRET_OK, "annexes_interpret_ok");
    CHECK(strstr(g_buf, "CP=ok") != NULL, "annexe_appel_cross_procedure_casse_accent");
    CHECK(strstr(g_buf, "IP=123") != NULL, "annexe_interpolation_casse");
    CHECK(strstr(g_buf, "CO=ok") != NULL, "annexe_constante_registry_casse");
}

/* ============================================================
 * 8. Identifiant long (> 50 chars) + variante de casse — garde contre un
 *    eventuel cap de buffer dans la canonicalisation (le nom fait 59 chars).
 *    Declaration canon, relecture tout en majuscules : la resolution doit
 *    rester exacte, pas de troncature silencieuse.
 *    Reproduit scenario_edge_identifiant_long_casse de la matrice .owl.
 * ============================================================ */
static const char* SRC_LONG_ID =
    "nIdentificateurTresLongAvecPlusieursMotsSupplementaires_01 est un entier = 555\n"
    "Trace(\"IL=\" + NIDENTIFICATEURTRESLONGAVECPLUSIEURSMOTSSUPPLEMENTAIRES_01)\n"; /* relecture majuscules */

static void test_identifiant_long_casse(void) {
    InterpretResult res = capture_interpret(SRC_LONG_ID);
    CHECK(res == INTERPRET_OK, "long_id_interpret_ok");
    CHECK(strstr(g_buf, "IL=555") != NULL, "long_id_casse_variante");
}

/* ============================================================
 * 9. Multi-accents (2+ diacritiques dont cedille) — reproduit
 *    edge_accent_multiple_locale + procedure_accent_compose.
 *    - variable sChâteauForêt (â + ê) declaree, lue tous accents omis
 *      (schateauforet) ;
 *    - procedure ChâteauFrançais (â + ç cedille) declaree, appelee accents
 *      omis (chateaufrancais).
 * ============================================================ */
static const char* SRC_MULTI_ACCENT =
    "sCh\xc3\xa2teauFor\xc3\xaat est une cha\xc3\xaene = \"medieval\"\n"
    "Trace(\"MA=\" + schateauforet)\n"                /* tous accents omis */
    "PROCEDURE Ch\xc3\xa2teauFran\xc3\xa7" "ais()\n"
    "\tTrace(\"MP=ok\")\n"
    "FIN\n"
    "chateaufrancais()\n";                            /* accents + cedille omis */

static void test_multi_accent(void) {
    InterpretResult res = capture_interpret(SRC_MULTI_ACCENT);
    CHECK(res == INTERPRET_OK, "multi_accent_interpret_ok");
    CHECK(strstr(g_buf, "MA=medieval") != NULL, "multi_accent_variable_accents_omis");
    CHECK(strstr(g_buf, "MP=ok") != NULL, "multi_accent_procedure_cedille_omis");
}

/* ============================================================
 * 10. Double HTTP (HTTPRequête type + native) — preuve DIRECTE, compile-only.
 *     vm_interpret compile tout le source AVANT d'executer : un appel en casse
 *     melangee accent omis (hTTPRequete) place sous une garde jamais executee
 *     (SI Faux) prouve que la resolution/compilation du double HTTP reussit,
 *     sans jamais lancer de requete reseau. Le Trace("HT=ok") en fin, lui,
 *     s'execute : sa presence atteste que la compilation a passe le call site.
 * ============================================================ */
static const char* SRC_HTTP_COMPILE =
    "req est un hTTPRequete\n"                        /* annotation type (double) casse melangee */
    "si Faux alors\n"
    "\treq..URL = \"http://example.org\"\n"            /* propriete du type (double), jamais execute */
    "\tcorps est une cha\xc3\xaene = hTTPRequete(req..URL)\n" /* call site native (double), jamais execute */
    "fin\n"
    "Trace(\"HT=ok\")\n";

static void test_double_http_compile_only(void) {
    InterpretResult res = capture_interpret(SRC_HTTP_COMPILE);
    CHECK(res == INTERPRET_OK, "http_compile_only_interpret_ok");
    CHECK(strstr(g_buf, "HT=ok") != NULL, "http_compile_only_double_resolu");
}

/* ============================================================
 * 11. Propriete widget (accessible via `..`) — casse-insensible.
 *      : reprend la couverture retiree de
 *     tests/runtime/objuicontrol_headless/table_mode_loupe/test_table_mode_loupe.owl
 *     (assertion sur `TAB_Donnees..modeloupe` / `..MODELOUPE` en casse
 *     melangee, neutralisee par owl fmt au premier save — meme piege que
 *     les 2 matrices .owl retirees en 1967). La resolution nom-de-propriete
 *     widget passe par owl_dsl_prop_find (src/ui_catalog.c), un chemin de
 *     lookup DISTINCT des natives/types/procedures deja couverts ci-dessus
 *     (catalogue PROPS_<KIND> par widget, pas le registry natives/types).
 *     Appel direct de l'API C : pas besoin de compiler/ouvrir une vraie
 *     Fenetre, la resolution du nom de propriete est statique au catalogue.
 * ============================================================ */
static void test_widget_property_casse(void) {
    const OwlControlPropDef* canon = owl_dsl_prop_find("Table", "ModeLoupe");
    CHECK(canon != NULL, "widget_prop_modeloupe_canon_existe");

    const OwlControlPropDef* minuscule = owl_dsl_prop_find("Table", "modeloupe");
    CHECK(minuscule == canon, "widget_prop_modeloupe_minuscule");

    const OwlControlPropDef* majuscule = owl_dsl_prop_find("Table", "MODELOUPE");
    CHECK(majuscule == canon, "widget_prop_modeloupe_majuscule");

    /* Alias EN (MagnifierMode) doit resoudre vers la MEME def que le FR,
     * en plus d'etre lui-meme casse-insensible. */
    const OwlControlPropDef* enCanon = owl_dsl_prop_find("Table", "MagnifierMode");
    CHECK(enCanon == canon, "widget_prop_magnifiermode_alias_en");

    const OwlControlPropDef* enCasseMelangee = owl_dsl_prop_find("Table", "magnifiermode");
    CHECK(enCasseMelangee == canon, "widget_prop_magnifiermode_casse_melangee");
}

int case_accent_guard_test_main(void) {
    fprintf(stderr, "\n==== CASE/ACCENT INSENSIBILITE (invariant 1) — fix/1967 ====\n");

    test_native();
    test_procedure_user();
    test_variable_locale();
    test_variable_globale();
    test_mot_cle();
    test_double_type_native();
    test_sites_annexes();
    test_identifiant_long_casse();
    test_multi_accent();
    test_double_http_compile_only();
    test_widget_property_casse();

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);

    /* Garde ensemble vide : un controle qui n'examine rien ne prouve rien. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "case-accent-guard: CORPUS VIDE — 0 cas execute.\n");
        return g_fail + 1;
    }

    return g_fail;
}
