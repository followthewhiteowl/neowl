/*  — gate sur le rendu EN *traduit* de `owl examples`. 
 * avait colmate la fuite linguistique (mode EN servait du FR brut) en
 * passant a "scenarios: []" + bandeau/note de repli vers --lang fr. Ce
 * ticket va plus loin : en mode EN, le CODE (`body`) de chaque scenario est
 * desormais passe par `owl_translate(body, EN)` (formatter.h L93, meme
 * moteur que `owl translate --to en`) — les symboles passent en EN canon
 * (`fRepExe`->`fExeDir`, `chaîne`->`string`, `Taille`->`Length`,
 * `dbgVérifieVrai`->`dbgCheckTrue`, `ExtraitChaîne`->`ExtractString`,
 * `dbgVérifieEgalité`->`dbgCheckEqual`, `Caract`->`Charact`...), les
 * commentaires `//` (prose FR) sont retires, et les chaines litterales
 * (libelles d'assertion, donnees du programme) sont conservees telles
 * quelles.
 *
 * Digue complementaire (cf. remontee reviewer post-impl 316e181fe) : l'impl
 * ne retire les libelles d'assertion FR que pour les appels `dbg*(...)`
 * tenant sur UNE ligne. Les appels `dbg*` MULTI-LIGNES (tres frequents dans
 * le corpus — `dbgVérifieVrai(\n expr,\n "libelle"\n)`) laissent passer la
 * prose FR du libelle telle quelle. Bloc 5 ci-dessous verrouille ce trou sur
 * UUID (corpus `tests/scenarios/types/test_UUID.owl`), confirme multi-ligne.
 *
 * Ce scanner verrouille, en invoquant le VRAI owl.exe, sur 2 natives a
 * scenarios (fRepExe/fExeDir — a commentaires `//` dans le corps ;
 * ExtraitChaîne/ExtractString — sans commentaire dans le corps, pour
 * prouver la translitteration de symboles seule), plus un 3e bloc (UUID) qui
 * verrouille la digue multi-ligne :
 *
 *   1. `examples fRepExe --lang en` (TEXTE) : PRESENCE des symboles EN
 *      canon du corps (`fExeDir(`, `Length(`, `dbgCheckTrue(`, `Right(`,
 *      `dbgCheckFalse(`), ABSENCE des symboles FR canon (`fRepExe(`,
 *      `cha\xc3\xaene`, `Taille(`, `dbgV\xc3\xa9rifieVrai(`, `Droite(`,
 *      `dbgV\xc3\xa9rifieFaux(`), ABSENCE du commentaire FR stable
 *      ("r\xc3\xa9pertoire", present dans tous les commentaires `//` du
 *      corpus fRepExe), ABSENCE du bandeau de repli V0 ("source corpus in
 *      French" — le ticket "remplace le bandeau", cf. decision L38).
 *      ROUGE attendu avant impl (le CLI ne traduit pas encore, sert le
 *      bandeau V0 sans aucun corps).
 *   2. `examples fRepExe --json --lang en` : idem en JSON — PRESENCE
 *      champ `"body"` et des memes symboles EN, ABSENCE des symboles FR
 *      et du commentaire FR, ABSENCE de `"scenarios": []` ( servait
 *      vide ; ce ticket sert les scenarios reels traduits).
 *      ROUGE attendu avant impl (JSON sert aujourd'hui scenarios:[] + note,
 *      sans aucun body).
 *   3. Systemique sur une 2e native SANS commentaire dans le corps
 *      (ExtraitChaîne) : memes assertions symboles (`ExtractString(`,
 *      `dbgCheckEqual(`, `Charact(` presents ; `ExtraitChaîne(`,
 *      `dbgV\xc3\xa9rifieEgalit\xc3\xa9(`, `Caract(` absents), TEXTE+JSON,
 *      PLUS preuve que les chaines litterales sont conservees (le libelle
 *      "pomme" du corpus doit survivre a la traduction — ce n'est pas un
 *      symbole, donc owl_translate ne doit pas y toucher).
 *      ROUGE attendu avant impl.
 *   5. UUID (dbg* multi-lignes) : `examples UUID --lang en` (TEXTE et
 *      --json) doit ABSENT les marqueurs de prose FR qui ne vivent QUE dans
 *      les libelles d'assertion multi-lignes du corpus ("respecte",
 *      "séparateur", "round-trip exact", "consécutifs" — aucun n'est un nom
 *      de symbole translitterable, donc leur presence prouve une fuite de
 *      LIBELLE, pas un residu de code non traduit), tout en gardant PRESENT
 *      au moins un symbole EN canon ("GetUUID(", "ToString(",
 *      "dbgCheckTrue(", "dbgCheckDifferent(") prouvant que la
 *      translitteration du code tourne malgre la fuite. ROUGE attendu
 *      contre l'impl actuelle (316e181fe) — c'est le but de ce complement.
 *   4. Non-regression FR stricte (VERT des le depart, les 2 natives,
 *      TEXTE+JSON) : mode par defaut (sans --lang, et --lang fr explicite)
 *      sert integralement les symboles ET commentaires FR canon, sans le
 *      moindre residu EN ni bandeau de repli. Verrouille que ce chantier
 *      ne touche pas au rendu FR existant.
 *
 * Limite assumee : ce scanner ne recompile PAS le corps EN produit (un
 * corps de scenario est un fragment de procedure, pas un fichier complet —
 * tenter un `--check` dessus serait un bricolage hors-perimetre). La preuve
 * de "validite EN" passe par l'absence de TOUT symbole FR canon connu du
 * corpus (liste blanche courte, assertion 1-3) plus la non-regression du
 * moteur `owl_translate` lui-meme (deja exerce par ailleurs via
 * `owl translate --to en`, hors scope de cette lane boite noire).
 *
 * Harness boite noire compile a la volee par tests/cli/doc_lang/run.sh
 * (calque examples_json_lang_scan.c / cli_learn_lang_scan.c). Invoque le
 * VRAI owl.exe (chemin en argv[1]).
 *
 * Exit code : 0 si toutes les assertions tiennent, 1 sinon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define POPEN _popen
#define PCLOSE _pclose
#else
#define POPEN popen
#define PCLOSE pclose
#endif

static char* run_capture(const char* cmdline) {
    FILE* fp = POPEN(cmdline, "r");
    if (!fp) return NULL;
    size_t cap = 65536, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { PCLOSE(fp); return NULL; }
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

static char* run_owl(const char* owlBin, const char* args) {
    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline), "\"\"%s\" %s 2>&1\"", owlBin, args);
    return run_capture(cmdline);
}

static int must_contain(const char* out, const char* needle, const char* ctx) {
    if (strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : attendu present mais absent : %s\n", ctx, needle);
    return 1;
}

static int must_not_contain(const char* out, const char* needle, const char* ctx) {
    if (!strstr(out, needle)) return 0;
    fprintf(stderr, "FAIL: %s : residu interdit trouve : %s\n", ctx, needle);
    return 1;
}

/* Verifie une liste de marqueurs presents et une liste de marqueurs
 * absents dans la sortie d'une invocation. Retourne le nombre de fails. */
static int check_markers(const char* out, const char* ctx,
                          const char** must_present, int n_present,
                          const char** must_absent, int n_absent) {
    int fails = 0;
    int i;
    for (i = 0; i < n_present; i++) fails += must_contain(out, must_present[i], ctx);
    for (i = 0; i < n_absent; i++) fails += must_not_contain(out, must_absent[i], ctx);
    return fails;
}

int main(int argc, char** argv) {
    const char* owlBin = (argc > 1) ? argv[1] : "../../build/owl.exe";
    int fails = 0;

    /* Corpus fRepExe/fExeDir : corps avec commentaires `//`. */
    const char* fr_fExeDir[] = {
        "fRepExe(", "cha\xc3\xaene", "Taille(", "dbgV\xc3\xa9rifieVrai(",
        "Droite(", "dbgV\xc3\xa9rifieFaux(", "r\xc3\xa9pertoire"
    };
    const char* en_fExeDir[] = {
        "fExeDir(", "Length(", "dbgCheckTrue(", "Right(", "dbgCheckFalse("
    };
    const char* banner_v0 = "source corpus in French";

    /* Corpus ExtraitChaîne/ExtractString : corps SANS commentaire, prouve
     * la seule translitteration de symboles + conservation des litteraux. */
    const char* fr_extrait[] = {
        "ExtraitCha\xc3\xaene(", "dbgV\xc3\xa9rifieEgalit\xc3\xa9(", "Caract(", "Taille("
    };
    const char* en_extrait[] = {
        "ExtractString(", "dbgCheckEqual(", "Charact("
    };
    const char* literal_pomme = "pomme";

    /* --- 1. fRepExe : TEXTE EN, symboles + anti-commentaire + anti-bandeau --- */
    {
        char* out = run_owl(owlBin, "examples fRepExe --lang en");
        const char* ctx = "examples fRepExe --lang en (texte)";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            fails += check_markers(out, ctx, en_fExeDir, 5, fr_fExeDir, 7);
            fails += must_not_contain(out, banner_v0, ctx);
        }
        free(out);
    }

    /* --- 2. fRepExe : JSON EN, body present + symboles + anti-scenarios-vides --- */
    {
        char* out = run_owl(owlBin, "examples fRepExe --json --lang en");
        const char* ctx = "examples fRepExe --json --lang en";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            fails += must_contain(out, "\"body\"", ctx);
            fails += must_not_contain(out, "\"scenarios\": []", ctx);
            fails += check_markers(out, ctx, en_fExeDir, 5, fr_fExeDir, 7);
        }
        free(out);
    }

    /* --- 3a. ExtraitChaîne : TEXTE EN, symboles + litteral conserve --- */
    {
        char* out = run_owl(owlBin, "examples ExtraitChaine --lang en");
        const char* ctx = "examples ExtraitChaine --lang en (texte)";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            fails += check_markers(out, ctx, en_extrait, 3, fr_extrait, 4);
            fails += must_contain(out, literal_pomme, ctx);
        }
        free(out);
    }

    /* --- 3b. ExtraitChaîne : JSON EN, symboles + litteral conserve --- */
    {
        char* out = run_owl(owlBin, "examples ExtraitChaine --json --lang en");
        const char* ctx = "examples ExtraitChaine --json --lang en";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            fails += must_contain(out, "\"body\"", ctx);
            fails += must_not_contain(out, "\"scenarios\": []", ctx);
            fails += check_markers(out, ctx, en_extrait, 3, fr_extrait, 4);
            fails += must_contain(out, literal_pomme, ctx);
        }
        free(out);
    }

    /* --- 5. UUID : digue multi-ligne, labels FR etales dans dbg*(...) --- */
    /* Marqueurs de prose FR qui ne vivent QUE dans des libelles d'assertion
     * multi-lignes du corpus (pas des symboles translitterables). Leur
     * presence en sortie EN prouve une fuite de LIBELLE. */
    const char* fr_label_leak_uuid[] = {
        "respecte", "s\xc3\xa9parateur", "round-trip exact", "cons\xc3\xa9" "cutifs"
    };
    /* Symboles EN canon attendus dans le corps traduit (preuve que la
     * translitteration du CODE tourne malgre la fuite de libelle). */
    const char* en_symbols_uuid[] = {
        "GetUUID(", "ToString(", "dbgCheckTrue(", "dbgCheckDifferent("
    };

    {
        char* out = run_owl(owlBin, "examples UUID --lang en");
        const char* ctx = "examples UUID --lang en (texte, digue multi-ligne)";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            int i;
            for (i = 0; i < 4; i++) fails += must_contain(out, en_symbols_uuid[i], ctx);
            for (i = 0; i < 4; i++) fails += must_not_contain(out, fr_label_leak_uuid[i], ctx);
        }
        free(out);
    }

    {
        char* out = run_owl(owlBin, "examples UUID --json --lang en");
        const char* ctx = "examples UUID --json --lang en (digue multi-ligne)";
        if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
        else {
            int i;
            fails += must_contain(out, "\"body\"", ctx);
            for (i = 0; i < 4; i++) fails += must_contain(out, en_symbols_uuid[i], ctx);
            for (i = 0; i < 4; i++) fails += must_not_contain(out, fr_label_leak_uuid[i], ctx);
        }
        free(out);
    }

    /* --- 4. Non-regression FR stricte (defaut ET --lang fr explicite) --- */
    {
        const char* cmds_fExeDir[] = {
            "examples fRepExe", "examples fRepExe --lang fr",
            "examples fRepExe --json", "examples fRepExe --json --lang fr"
        };
        int i;
        for (i = 0; i < 4; i++) {
            char* out = run_owl(owlBin, cmds_fExeDir[i]);
            char ctx[128];
            snprintf(ctx, sizeof(ctx), "%s (non-regression FR)", cmds_fExeDir[i]);
            if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
            else {
                fails += check_markers(out, ctx, fr_fExeDir, 7, en_fExeDir, 5);
                fails += must_not_contain(out, banner_v0, ctx);
            }
            free(out);
        }
        const char* cmds_extrait[] = {
            "examples ExtraitChaine", "examples ExtraitChaine --lang fr",
            "examples ExtraitChaine --json", "examples ExtraitChaine --json --lang fr"
        };
        for (i = 0; i < 4; i++) {
            char* out = run_owl(owlBin, cmds_extrait[i]);
            char ctx[128];
            snprintf(ctx, sizeof(ctx), "%s (non-regression FR)", cmds_extrait[i]);
            if (!out || !*out) { fprintf(stderr, "FAIL: sortie vide : %s\n", ctx); fails++; }
            else {
                fails += check_markers(out, ctx, fr_extrait, 4, en_extrait, 3);
                fails += must_contain(out, literal_pomme, ctx);
            }
            free(out);
        }
    }

    printf("=== feat/2286 traduction EN du code des exemples (owl examples) ===\n");
    if (fails > 0) {
        printf("RESULT: RED (%d assertion(s) en echec)\n", fails);
        return 1;
    }
    printf("RESULT: GREEN (examples --lang en sert le code traduit, FR inchange)\n");
    return 0;
}
