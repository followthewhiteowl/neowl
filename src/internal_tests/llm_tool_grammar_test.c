// SPDX-License-Identifier: AGPL-3.0-only
/* llm_tool_grammar_test.c — convertisseur schema JSON -> GBNF.
 *
 * Ticket : 
 * Sujet  : la Voie B (convertisseur maison borne) qui traduit les schemas de
 *          function OpenAI d'une requete `tools` en GBNF forcant un appel
 *          d'outil valide. C'est le differenciateur « on FORCE un appel valide
 *          par grammaire » (plus fiable qu'ollama, meme sur un 7B).
 *
 * DIGUE TEST-FIRST (niveau C, MODEL-FREE). Ces tests fixent le CONTRAT du
 * convertisseur `owl_tool_schema_to_gbnf`. Aucun modele, aucun llama : on parse
 * un schema JSON (json_parse_cstr -> ObjJson), on convertit, puis on asserte sur
 * la GBNF produite. Tant que l'impl vit en STUB (stdlib_llm.c « STUB  »
 * -> renvoie NULL), ces tests sont ROUGES.
 *
 * ASSERTIONS PAR SOUS-CHAINES. On ne compare pas la GBNF octet-a-octet : le
 * nommage des regles (rN) et l'alignement des espaces appartiennent a
 * l'implementer. On verifie des INVARIANTS STRUCTURELS robustes : presence des
 * regles racine `root ::=` / `call_0 ::=`, litteraux de cles verrouilles
 * (`\"location\"`), branches d'enum (`\"celsius\"` / `\"fahrenheit\"`), regle
 * prelude `integer`, regle d'objet imbrique, presence/absence de `freetext`
 * selon tool_choice, et le refus explicite (NULL + err) des schemas non couverts.
 *
 * PIEGE D'ANTI-FAUX-VERT : une assertion d'ABSENCE de sous-chaine (« pas de
 * freetext ») est TRIVIALEMENT vraie quand la GBNF est NULL (stub). Toute
 * assertion d'absence est donc gardee par `g != NULL && ...` pour rester ROUGE
 * tant que le convertisseur ne produit rien.
 *
 * Execute via `owl --self-test-net` (cf. main.c), a cote de serve_llm_self_test_run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"          /* vm_init / vm_free (json_parse_cstr alloue du GC) */
#include "../object.h"      /* ObjJson */
#include "../stdlib_llm.h"  /* owl_tool_schema_to_gbnf, OwlToolGbnfError, OWL_TC_* */

/* Parser JSON interne (stdlib_json.c) : rend NULL sur JSON errone/incomplet.
 * Pas de header dedie ; declare extern comme dans serve_llm.c. */
extern ObjJson* json_parse_cstr(const char* src, int len);

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

static void compte(int ok, const char* label, int ligne) {
    if (ok) g_pass++;
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (ligne %d)\n", label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* Presence d'une sous-chaine, NULL-safe (une GBNF NULL ne matche rien). */
static int has(const char* hay, const char* needle) {
    return hay != NULL && strstr(hay, needle) != NULL;
}

static ObjJson* parse_json(const char* json) {
    return json_parse_cstr(json, (int)strlen(json));
}

/* ================================================================
 * Schemas de tools (JSON compact, echappe pour le C)
 * ================================================================ */

/* Un outil couvrant tous les cas d'un coup : string, enum, integer, objet
 * imbrique, et une propriete `required`. */
static const char SCHEMA_WEATHER[] =
    "[{\"type\":\"function\",\"function\":{"
      "\"name\":\"get_weather\","
      "\"description\":\"Renvoie la meteo\","
      "\"parameters\":{\"type\":\"object\",\"properties\":{"
        "\"location\":{\"type\":\"string\"},"
        "\"unit\":{\"type\":\"string\",\"enum\":[\"celsius\",\"fahrenheit\"]},"
        "\"days\":{\"type\":\"integer\"},"
        "\"detail\":{\"type\":\"object\",\"properties\":{"
            "\"note\":{\"type\":\"string\"}}}"
      "},\"required\":[\"location\"]}"
    "}}]";

/* Deux outils -> routeur multi-outils `root ::= ws ( call_0 | call_1 )`. */
static const char SCHEMA_MULTI[] =
    "[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\","
       "\"parameters\":{\"type\":\"object\",\"properties\":{"
         "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}},"
     "{\"type\":\"function\",\"function\":{\"name\":\"set_alarm\","
       "\"parameters\":{\"type\":\"object\",\"properties\":{"
         "\"time\":{\"type\":\"string\"}},\"required\":[\"time\"]}}}]";

/* Un seul outil minimal (pour les cas tool_choice). */
static const char SCHEMA_SIMPLE[] =
    "[{\"type\":\"function\",\"function\":{\"name\":\"get_weather\","
       "\"parameters\":{\"type\":\"object\",\"properties\":{"
         "\"location\":{\"type\":\"string\"}},\"required\":[\"location\"]}}}]";

/* ================================================================
 * FAMILLE A — string / enum / integer / required / objet imbrique
 * ================================================================ */

static void fam_string_enum_required_nested(void) {
    ObjJson* t = parse_json(SCHEMA_WEATHER);
    CHECK(t != NULL, "weather.json_parse");

    OwlToolGbnfError err; memset(&err, 0, sizeof(err));
    char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);

    CHECK(has(g, "root ::="),   "weather.root_rule");
    CHECK(has(g, "call-0 ::="), "weather.call_0_rule");
    /* Cle d'objet verrouillee au litteral `\"location\"` (le modele ne peut pas
     * l'inventer ni la renommer). */
    CHECK(has(g, "\\\"location\\\""), "weather.location_key_locked");
    /* Enum : deux branches litterales. */
    CHECK(has(g, "\\\"celsius\\\""),    "weather.enum_celsius");
    CHECK(has(g, "\\\"fahrenheit\\\""), "weather.enum_fahrenheit");
    /* Type integer -> regle prelude `integer`. */
    CHECK(has(g, "integer"), "weather.integer_rule");
    /* Objet imbrique -> sa propre regle, avec la cle du sous-champ. */
    CHECK(has(g, "\\\"note\\\""), "weather.nested_note_key");

    free(g);
}

/* ================================================================
 * FAMILLE B — multi-outils : routeur `root ::= ws ( call_0 | call_1 )`
 * ================================================================ */

static void fam_multi(void) {
    ObjJson* t = parse_json(SCHEMA_MULTI);
    CHECK(t != NULL, "multi.json_parse");

    OwlToolGbnfError err; memset(&err, 0, sizeof(err));
    char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);

    CHECK(has(g, "root ::="), "multi.root_rule");
    CHECK(has(g, "call-0"),   "multi.call_0_branch");
    CHECK(has(g, "call-1"),   "multi.call_1_branch");
    /* Chaque nom d'outil verrouille au litteral -> le modele choisit une branche
     * mais ne peut pas fabriquer un nom d'outil. */
    CHECK(has(g, "\\\"get_weather\\\""), "multi.name_get_weather_locked");
    CHECK(has(g, "\\\"set_alarm\\\""),   "multi.name_set_alarm_locked");

    free(g);
}

/* ================================================================
 * FAMILLE C — tool_choice -> grammaire (auto / required / named)
 * ================================================================ */

static void fam_tool_choice(void) {
    /* required : force un appel -> AUCUN `freetext`. */
    {
        ObjJson* t = parse_json(SCHEMA_SIMPLE);
        OwlToolGbnfError err; memset(&err, 0, sizeof(err));
        char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);
        CHECK(has(g, "root ::="),              "tc_required.root_rule");
        CHECK(g != NULL && !has(g, "freetext"), "tc_required.no_freetext");
        free(g);
    }

    /* auto : appel OU texte libre -> presence d'une branche `freetext`. */
    {
        ObjJson* t = parse_json(SCHEMA_SIMPLE);
        OwlToolGbnfError err; memset(&err, 0, sizeof(err));
        char* g = owl_tool_schema_to_gbnf(t, OWL_TC_AUTO, NULL, &err);
        CHECK(has(g, "root ::="),  "tc_auto.root_rule");
        CHECK(has(g, "freetext"),  "tc_auto.has_freetext");
        free(g);
    }

    /* named : force CET outil (parmi deux) -> root racine sur l'outil impose,
     * outil verrouille, et pas de `freetext` (forcage non negociable). */
    {
        ObjJson* t = parse_json(SCHEMA_MULTI);
        OwlToolGbnfError err; memset(&err, 0, sizeof(err));
        char* g = owl_tool_schema_to_gbnf(t, OWL_TC_NAMED, "set_alarm", &err);
        CHECK(has(g, "root ::="),               "tc_named.root_rule");
        CHECK(has(g, "\\\"set_alarm\\\""),       "tc_named.forced_tool_locked");
        CHECK(g != NULL && !has(g, "freetext"),  "tc_named.no_freetext");
        free(g);
    }
}

/* ================================================================
 * FAMILLE D — schemas NON COUVERTS -> NULL + err explicite (pas de GBNF partielle)
 * ================================================================ */

/* Chaque construction hors sous-ensemble borne (D gelee) doit produire un REFUS
 * PROPRE : retour NULL, err.ok=0, et err.where/err.reason renseignes (le noeud
 * fautif). Le stub renvoie deja NULL + ok=0 (ces deux-la passeront), mais laisse
 * reason/where VIDES -> ces assertions-la restent ROUGES tant que le walk n'est
 * pas ecrit (red-for-right-reason). */
static void uncovered_one(const char* prefix, const char* json) {
    char lbl[96];

    ObjJson* t = parse_json(json);
    snprintf(lbl, sizeof(lbl), "%s.json_parse", prefix);
    CHECK(t != NULL, lbl);   /* le JSON du schema est lui-meme bien forme */

    OwlToolGbnfError err; memset(&err, 0, sizeof(err));
    char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);

    snprintf(lbl, sizeof(lbl), "%s.returns_null", prefix);
    CHECK(g == NULL, lbl);
    snprintf(lbl, sizeof(lbl), "%s.err_not_ok", prefix);
    CHECK(err.ok == 0, lbl);
    snprintf(lbl, sizeof(lbl), "%s.err_where_set", prefix);
    CHECK(err.where[0] != '\0', lbl);       /* pointe le noeud fautif */
    snprintf(lbl, sizeof(lbl), "%s.err_reason_set", prefix);
    CHECK(err.reason[0] != '\0', lbl);      /* motif lisible */

    free(g);
}

static void fam_uncovered(void) {
    /* (A) STRUCTUREL uniquement : ces constructions changent la FORME du valide,
     * sans type de base emettable -> refus dur conserve (400). Depuis ,
     * les mots-cles de CONTRAINTE (pattern, bornes, format, minLength,
     * maxLength, minItems, maxItems) ne sont plus ici : ils degradent au type
     * de base (cf. fam_degraded). */

    /* $ref : pas de resolution de reference. */
    uncovered_one("ref",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"$ref\":\"#/defs/Y\"}}}}}]");

    /* allOf : pas de composition de schemas. */
    uncovered_one("allOf",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"allOf\":[{\"type\":\"string\"}]}}}}}]");

    /* anyOf : union de schemas -> pas de type de base unique. */
    uncovered_one("anyOf",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"anyOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}}}}}]");

    /* oneOf : idem, exclusivite -> non emettable. */
    uncovered_one("oneOf",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"integer\"}]}}}}}]");
}

/* ================================================================
 * FAMILLE E — contraintes DEGRADEES au type de base
 * ================================================================ */

/* (B) CONTRAINTE : un mot-cle qui raffine un type de base emettable. Le noeud
 * a un `type` (string/number/integer/array) que schema_emit convertit deja ;
 * la contrainte est IGNOREE -> GBNF valide du type de base, PAS un 400. Une
 * borne ignoree = type non borne (structurellement valide), contrairement a
 * une structure (A) qui, elle, n'a pas de type de base. */
static void covered_one(const char* prefix, const char* json, const char* needle) {
    char lbl[96];

    ObjJson* t = parse_json(json);
    snprintf(lbl, sizeof(lbl), "%s.json_parse", prefix);
    CHECK(t != NULL, lbl);

    OwlToolGbnfError err; memset(&err, 0, sizeof(err));
    char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);

    snprintf(lbl, sizeof(lbl), "%s.not_null", prefix);
    CHECK(g != NULL, lbl);
    snprintf(lbl, sizeof(lbl), "%s.type_de_base", prefix);
    CHECK(has(g, needle), lbl);

    free(g);
}

static void fam_degraded(void) {
    /* Bornes numeriques -> integer de base. */
    covered_one("deg_bornes",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"integer\",\"minimum\":5,\"maximum\":10}}}}}]",
        "integer");

    /* pattern + format + minLength -> string de base. */
    covered_one("deg_string",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"string\",\"pattern\":\"^a+$\",\"format\":\"email\","
                   "\"minLength\":3}}}}}]",
        "string");

    /* array + minItems/maxItems -> array de base (items string). */
    covered_one("deg_array",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
                   "\"minItems\":1,\"maxItems\":9}}}}}]",
        "string");

    /* Cousins deja toleres (jamais dans FORBIDDEN) : exclusiveMinimum /
     * exclusiveMaximum / multipleOf -> integer de base. Confirme qu'ils ne
     * bloquent pas (regression-guard). */
    covered_one("deg_cousins",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"type\":\"integer\",\"exclusiveMinimum\":0,"
                   "\"exclusiveMaximum\":100,\"multipleOf\":2}}}}}]",
        "integer");

    /* Cas OpenCode reel : outil `read` avec des params bornes (offset/limit) +
     * un required. La conversion reussit, la GBNF force un integer valide et
     * verrouille la cle `path`. */
    {
        ObjJson* t = parse_json(
            "[{\"type\":\"function\",\"function\":{\"name\":\"read\","
              "\"description\":\"lit un fichier\","
              "\"parameters\":{\"type\":\"object\",\"properties\":{"
                "\"path\":{\"type\":\"string\"},"
                "\"offset\":{\"type\":\"integer\",\"minimum\":0},"
                "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":2000}"
              "},\"required\":[\"path\"]}}}]");
        CHECK(t != NULL, "opencode.json_parse");

        OwlToolGbnfError err; memset(&err, 0, sizeof(err));
        char* g = owl_tool_schema_to_gbnf(t, OWL_TC_REQUIRED, NULL, &err);
        CHECK(has(g, "root ::="),          "opencode.root_rule");
        CHECK(has(g, "call-0 ::="),        "opencode.call_0_rule");
        CHECK(has(g, "integer"),           "opencode.integer_rule");
        CHECK(has(g, "\\\"path\\\""),      "opencode.path_key_locked");
        free(g);
    }

    /* Schema DEGENERE : contrainte SEULE, sans `type` ni `enum`. Retirer la
     * borne de FORBIDDEN ne le sauve pas -> "type absent sans enum" -> refus
     * (NULL). C'est correct : rien d'emettable. */
    uncovered_one("deg_degenere",
        "[{\"type\":\"function\",\"function\":{\"name\":\"f\","
          "\"parameters\":{\"type\":\"object\",\"properties\":{"
            "\"x\":{\"minimum\":5}}}}}]");
}

/* ================================================================
 * FAMILLE F — stop-string sur la sortie ACCUMULEE
 * ================================================================ */

/* llm_stop_marker_scan est un helper PUR, model-free. Il scanne la FIN du buffer
 * de sortie accumule : marqueur complet en fin -> position de debut + STOP ;
 * sinon -> nb d'octets de queue formant un prefixe PARTIEL de marqueur (a retenir
 * en streaming). Couvre les 3 cas de fuite (mono-token, multi-tokens, coupe entre
 * deux tokens) que le filet par-morceau ne voyait pas. */
static void sc_complete(const char* label, const char* buf, size_t attStart) {
    size_t ms = 999, hb = 999;
    int r = llm_stop_marker_scan(buf, strlen(buf), &ms, &hb);
    compte(r == 1 && ms == attStart, label, __LINE__);
}
static void sc_partial(const char* label, const char* buf, size_t attHold) {
    size_t ms = 999, hb = 999;
    int r = llm_stop_marker_scan(buf, strlen(buf), &ms, &hb);
    compte(r == 0 && hb == attHold, label, __LINE__);
}

static void fam_stop_marker_scan(void) {
    /* Marqueur complet en fin de buffer -> position de debut (a tronquer). */
    sc_complete("scan.im_end_suffix",  "Bonjour <|im_end|>", 8);
    sc_complete("scan.eot_id_seul",    "<|eot_id|>",         0);
    sc_complete("scan.endoftext_seul", "<|endoftext|>",      0);
    sc_complete("scan.end_seul",       "<|end|>",            0);
    /* Prefixe partiel en fin -> holdback = plus long prefixe (octets a retenir). */
    sc_partial("scan.partiel_im",      "Bonjour <|im_", 5);   /* "<|im_" de "<|im_end|>" */
    sc_partial("scan.partiel_lt",      "Bonjour <",     1);   /* "<" prefixe de tous */
    /* "<|end" est prefixe de <|end|> ET <|endoftext|> -> le plus long = 5. */
    sc_partial("scan.partiel_end",     "abc<|end",      5);
    /* Aucun prefixe partiel en fin -> holdback 0. */
    sc_partial("scan.aucun",           "Bonjour",       0);
    sc_partial("scan.vide",            "",              0);
    /* Pas de faux positif : la queue " b" n'est pas un prefixe valide continu. */
    sc_partial("scan.faux_positif",    "a <| b",        0);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int llm_tool_grammar_self_test_run(void) {
    printf("== convertisseur schema->GBNF (feat/1671) ==\n");

    /* json_parse_cstr alloue des ObjJson (interning + GC) : VM requise. Le
     * convertisseur, lui, travaille en C sur l'arbre sans allouer d'Obj (regle
     * d'or), donc aucun GC ne tourne entre parse et conversion. */
    vm_init();
    fam_string_enum_required_nested();
    fam_multi();
    fam_tool_choice();
    fam_uncovered();
    fam_degraded();
    fam_stop_marker_scan();
    vm_free();

    printf("   convertisseur schema->GBNF : %d/%d\n", g_pass, g_pass + g_fail);
    return g_fail;
}
