// SPDX-License-Identifier: AGPL-3.0-only
#define _GNU_SOURCE
/* Self-test du serveur MCP (cli_mcp.c). Couvre :
 *
 *   - Handshake initialize + ping
 *   - tools/list : présence des 6 tools exposés
 *   - tools/call : un par tool, cas nominal + cas d'erreur applicative
 *   - resources/list : 13 entrées (9 fondations + 4 catalogues)
 *   - resources/read : public_doc, catalog, native, type, scenario, 404
 *   - resources/templates/list : 3 templates URI
 *   - Erreurs protocole : flag inconnu, JSON malformé, tool inconnu
 *
 * In-process via `cli_mcp_handle_request(line, len)` qui traite une
 * requête JSON-RPC et retourne la réponse sérialisée. Pas de pipe ni de
 * sous-processus — toute la logique tient dans le même owl.exe que celui
 * qui contient le registry et l'embedded doc.
 *
 * Branché sur `owl --self-test-mcp` (cf. src/main.c), lancé par le
 * Makefile cible `all` après les autres self-tests. Retour : 0 si tous
 * pass, n si n échecs. */

#include "cli_mcp.h"
#include "cli_doc.h"          /* cli_doc_primitive_count/at */
#include "embedded_docs.h"    /* EMBEDDED_PUBLIC_DOCS */
#include "owl_registry.h"     /* owl_registry_count — verif idempotence stdlib_init */
#include "owl_stdlib.h"       /* stdlib_init — verif idempotence */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#ifdef _WIN32
  #include <io.h>
  #include <process.h>
  #define owl_test_unlink _unlink
  #define owl_test_getpid _getpid
#else
  #include <unistd.h>
  #define owl_test_unlink unlink
  #define owl_test_getpid getpid
#endif

/* ================================================================
 * Infra
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; \
        fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); \
    } \
} while (0)

#define CHECK_HAS(needle, label) \
    CHECK(g_response && strstr(g_response, (needle)) != NULL, label)
#define CHECK_NO(needle, label) \
    CHECK(g_response && strstr(g_response, (needle)) == NULL, label)

/* Population attendue des outils MCP exposes par tools/list. Fige
 * volontairement : un ajout d'outil est legitime et doit passer ici. Le mode
 * d'emploi est dans le message d'echec, pas ici. */
#define EXPECTED_MCP_TOOLS 12

static char* g_response = NULL;

static void send_req(const char* json) {
    free(g_response);
    g_response = cli_mcp_handle_request(json, (int)strlen(json));
}

static int count_occurrences(const char* needle) {
    if (!g_response) return 0;
    int n = 0;
    size_t nlen = strlen(needle);
    const char* p = g_response;
    while ((p = strstr(p, needle)) != NULL) { n++; p += nlen; }
    return n;
}

/* ================================================================
 * Tests
 * ================================================================ */

static void test_initialize(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
             "\"params\":{\"protocolVersion\":\"2025-06-18\","
             "\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");
    CHECK_HAS("\"protocolVersion\":\"2025-06-18\"", "init protocolVersion");
    CHECK_HAS("\"serverInfo\"", "init serverInfo present");
    CHECK_HAS("\"name\":\"owl\"", "init serverInfo.name = owl");
    CHECK_HAS("\"capabilities\"", "init capabilities present");
    CHECK_HAS("\"tools\"", "init annonce caps tools");
    CHECK_HAS("\"resources\"", "init annonce caps resources");
}

/*  — champ instructions MCP documentant le périmètre serveur. */
static void test_initialize_instructions(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":800,\"method\":\"initialize\","
             "\"params\":{\"protocolVersion\":\"2025-06-18\","
             "\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}");
    CHECK_HAS("\"instructions\"", "init : champ instructions present");
    CHECK_HAS("comprendre",       "instructions : mentionne comprendre");
    CHECK_HAS("explorer",         "instructions : mentionne explorer");
    CHECK_HAS("diagnostiquer",    "instructions : mentionne diagnostiquer");
    CHECK_HAS("n'exécute pas",    "instructions : indique pas d'exécution");
}

static void test_ping(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}");
    CHECK_HAS("\"result\":{}", "ping returns empty result");
}

static void test_notification_no_response(void) {
    char* r = cli_mcp_handle_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", -1);
    /* line_len < 0 → la fonction doit retourner NULL silencieusement. */
    CHECK(r == NULL, "negative len returns NULL");

    r = cli_mcp_handle_request(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}",
        (int)strlen("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"));
    CHECK(r == NULL, "notification returns no response");
    free(r);
}

static void test_unknown_method(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"nonexistent\"}");
    CHECK_HAS("\"error\"", "unknown method returns error");
    CHECK_HAS("-32601", "unknown method code -32601");
}

static void test_parse_error(void) {
    send_req("not even json");
    CHECK_HAS("\"error\"", "parse error returns error");
    CHECK_HAS("-32700", "parse error code -32700");
}

static void test_tools_list(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
    CHECK_HAS("\"name\":\"owl_explain\"",   "tools/list contient owl_explain");
    CHECK_HAS("\"name\":\"owl_search\"",    "tools/list contient owl_search");
    CHECK_HAS("\"name\":\"owl_examples\"",  "tools/list contient owl_examples");
    CHECK_HAS("\"name\":\"owl_signature\"", "tools/list contient owl_signature");
    CHECK_HAS("\"name\":\"owl_format\"",    "tools/list contient owl_format");
    CHECK_HAS("\"name\":\"owl_check\"",     "tools/list contient owl_check");
    CHECK_HAS("\"name\":\"owl_where\"",      "tools/list contient owl_where");
    CHECK_HAS("\"name\":\"owl_template\"",   "tools/list contient owl_template");
    CHECK_HAS("\"name\":\"owl_new\"",        "tools/list contient owl_new");
    CHECK_HAS("\"name\":\"owl_limits\"",     "tools/list contient owl_limits");
    CHECK_HAS("\"name\":\"owl_translate\"",  "tools/list contient owl_translate");
    CHECK_HAS("\"name\":\"owl_list\"",       "tools/list contient owl_list");
    CHECK_HAS("\"inputSchema\"", "tools exposent inputSchema");
    CHECK_HAS("\"required\"",    "schemas déclarent required[]");
    int nTools = count_occurrences("\"inputSchema\"");
    CHECK(nTools == EXPECTED_MCP_TOOLS,
          "compte_fige_des_outils_mcp_exposes");
    if (nTools != EXPECTED_MCP_TOOLS) {
        fprintf(stderr,
"\n"
"  tools/list expose %d outils, ce controle en attend %d.\n"
"  Le nombre est fige volontairement ; ajouter ou retirer un outil est\n"
"  legitime. Verifier que son entree de la table TOOLS[] (src/cli_mcp.c)\n"
"  declare bien name, schema_builder et handler, ajouter sa ligne\n"
"  CHECK_HAS(\"\\\"name\\\":\\\"owl_xxx\\\"\", ...) juste au-dessus, puis porter le\n"
"  compte a %d : src/internal_tests/cli_mcp_test.c, #define EXPECTED_MCP_TOOLS\n"
"\n",
                nTools, EXPECTED_MCP_TOOLS, nTools);
    }
}

static void test_tool_explain_ok(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Trace\"}}}");
    CHECK_HAS("\"content\"",    "explain ok : content present");
    CHECK_HAS("\"type\":\"text\"", "explain ok : text block");
    CHECK_HAS("Signatures",     "explain ok : section signatures rendue");
    CHECK_NO("\"isError\":true", "explain ok : pas d'isError");
}

static void test_tool_explain_unknown(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"ZzZz_inconnu\"}}}");
    CHECK_HAS("\"isError\":true", "explain inconnu : isError set");
    CHECK_HAS("inconnu",          "explain inconnu : message propagé");
}

static void test_tool_explain_bad_flag(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Trace\",\"cat\":\"bogus\"}}}");
    CHECK_HAS("\"isError\":true", "explain --cat bogus : isError");
    CHECK_HAS("bucket",           "message d'erreur explicite");
}

/*  — explain etendu a la famille errors (registry codes E_*).
 * Le tool MCP owl_explain delegue a cli_doc_explain ; verifier que le
 * canal MCP retourne bien la fiche FR (defaut) et EN (lang=en) pour un
 * code du registry erreurs. */
static void test_tool_explain_error_fr(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"E_LEX_UNTERMINATED_STRING\"}}}");
    CHECK_HAS("E_LEX_UNTERMINATED_STRING",
              "MCP explain E_LEX_... : name dans la sortie");
    CHECK_HAS("lex",
              "MCP explain E_LEX_... : categorie rendue");
    /* Verifie le rendu FR par defaut. L'accent passe en \\u0...
     * dans le JSON envelope MCP (escape JSON standard). */
    CHECK(strstr(g_response, "Cha\\u00eene non termin\\u00e9e") != NULL ||
          strstr(g_response, "Chaîne non terminée") != NULL,
          "MCP explain E_LEX_... : template FR rendu");
    CHECK_NO("\"isError\":true",
             "MCP explain E_LEX_... : pas d'isError");
}

static void test_tool_explain_error_en(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"E_LEX_UNTERMINATED_STRING\","
             "\"lang\":\"en\"}}}");
    CHECK_HAS("Unterminated string",
              "MCP explain E_LEX_... lang=en : template EN rendu");
    CHECK_NO("\"isError\":true",
             "MCP explain E_LEX_... lang=en : pas d'isError");
}

static void test_tool_explain_error_unknown(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"E_FOOBAR_QUI_NEXISTE_PAS\"}}}");
    CHECK_HAS("\"isError\":true",
              "MCP explain E_FOOBAR... inconnu : isError set");
}

static void test_tool_search(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_search\","
             "\"arguments\":{\"query\":\"HTTP\"}}}");
    CHECK_HAS("HTTPRequête",   "search HTTP trouve HTTPRequête");
}

/*  — owl_limits MCP tool. */
static void test_tool_limits_list(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":700,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_limits\",\"arguments\":{}}}");
    CHECK_HAS("\"content\"",        "owl_limits list : content present");
    CHECK_HAS("MAX_USER_TYPES",     "owl_limits list contient MAX_USER_TYPES");
    CHECK_NO("\"isError\":true",    "owl_limits list : pas d'isError");
}

static void test_tool_limits_lookup(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":701,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_limits\","
             "\"arguments\":{\"name\":\"MAX_USER_PROC_PARAMS\"}}}");
    CHECK_HAS("MAX_USER_PROC_PARAMS","owl_limits lookup : nom present");
    CHECK_HAS("64",                  "owl_limits lookup : valeur 64 affichee");
    CHECK_NO("\"isError\":true",     "owl_limits lookup : pas d'isError");
}

/* ================================================================
 * owl_list — catalogue par bucket, filtrable par categorie
 * ================================================================ */

static void test_tool_list_categories(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":710,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"categories\"}}}");
    CHECK_HAS("\"content\"",     "owl_list categories : content present");
    CHECK_HAS("UI",              "owl_list categories : categorie UI listee");
    CHECK_HAS("Tableaux",        "owl_list categories : categorie Tableaux listee");
    CHECK_NO("\"isError\":true", "owl_list categories : pas d'isError");
}

static void test_tool_list_categories_json(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":711,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"categories\",\"format\":\"json\"}}}");
    CHECK_HAS("\\\"categories\\\":", "owl_list categories json : champ categories");
    CHECK_HAS("\\\"count\\\":",      "owl_list categories json : champ count");
    CHECK_NO("\"isError\":true",     "owl_list categories json : pas d'isError");
}

static void test_tool_list_natives_cat_ui(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":712,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"natives\",\"cat\":\"UI\"}}}");
    CHECK_HAS("Info",            "owl_list natives --cat UI : Info present");
    CHECK_NO("\"isError\":true", "owl_list natives --cat UI : pas d'isError");
    /* Filtre effectif : une native d'une autre famille ne doit pas sortir. */
    CHECK_NO("HTTPRequête",      "owl_list natives --cat UI : filtre effectif");
}

/* Insensibilite casse + acceptation du nom EN de categorie, deja assurees
 * cote CLI : le canal MCP ne doit pas les perdre. */
static void test_tool_list_natives_cat_casse(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":713,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"natives\",\"cat\":\"ui\"}}}");
    CHECK_HAS("Info",            "owl_list natives --cat ui : Info present");
    CHECK_NO("\"isError\":true", "owl_list natives --cat ui : pas d'isError");
}

static void test_tool_list_natives_cat_en(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":714,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"natives\",\"cat\":\"Arrays\"}}}");
    CHECK_HAS("Dimension",       "owl_list natives --cat Arrays : Dimension present");
    CHECK_NO("\"isError\":true", "owl_list natives --cat Arrays : pas d'isError");
}

/* Categorie non reconnue : le CLI rend rc=0 en imprimant le diagnostic.
 * Le canal MCP doit malgre tout marquer le resultat en erreur, sinon un
 * agent lit une liste vide comme une reponse valide. */
static void test_tool_list_natives_cat_inconnue(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":715,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"natives\",\"cat\":\"Affichage\"}}}");
    CHECK_HAS("\"isError\":true", "owl_list cat inconnue : isError set");
    CHECK_HAS("Affichage",        "owl_list cat inconnue : nom fautif rappele");
    CHECK_HAS("Tableaux",         "owl_list cat inconnue : categories valides listees");
    CHECK_HAS("UI",               "owl_list cat inconnue : UI dans les valides");
}

/* Le verdict « famille inconnue » ne se lit plus dans le texte affiche : il
 * vient du predicat exporte par cli_doc.c. La boucle couvre TOUS les buckets
 * qui filtrent par famille — y compris ceux dont le diagnostic ne porte pas
 * les memes mots que `natives` (« champ inconnu » sur events / props). Une
 * detection par sentinelle textuelle ne peut pas les couvrir : c'est ce qui
 * rend le couplage impossible a retablir sans que ce test rougisse. */
static void test_tool_list_cat_inconnue_tous_buckets(void) {
    static const char* const BUCKETS[] = {
        "natives", "types", "constants", "keywords", "limits",
        "events", "props", NULL
    };
    char req[320], label[192];
    for (int i = 0; BUCKETS[i]; i++) {
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":726,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_list\",\"arguments\":"
                 "{\"bucket\":\"%s\",\"cat\":\"ZzzPasUneFamille\"}}}",
                 BUCKETS[i]);
        send_req(req);
        snprintf(label, sizeof label,
                 "owl_list %s cat inconnue : isError set", BUCKETS[i]);
        CHECK_HAS("\"isError\":true", label);
        snprintf(label, sizeof label,
                 "owl_list %s cat inconnue : nom fautif rappele", BUCKETS[i]);
        CHECK_HAS("ZzzPasUneFamille", label);
    }
}

/* Symetrie : une famille valable ne prend pas isError, y compris quand elle
 * est vide (`Colonne` n'a aucun event propre). Sans ce cas, un predicat qui
 * repondrait « inconnue » a tout passerait le test precedent. */
static void test_tool_list_cat_valide_pas_d_erreur(void) {
    static const char* const PAIRES[][2] = {
        { "constants", "Clavier" },
        { "keywords",  "Exceptions" },
        { "limits",    "Compiler/Types" },
        { "props",     "Saisie" },
        { "events",    "Colonne" },   /* valable, mais sans event : pas une erreur */
        { NULL, NULL }
    };
    char req[320], label[192];
    for (int i = 0; PAIRES[i][0]; i++) {
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":727,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_list\",\"arguments\":"
                 "{\"bucket\":\"%s\",\"cat\":\"%s\"}}}",
                 PAIRES[i][0], PAIRES[i][1]);
        send_req(req);
        snprintf(label, sizeof label, "owl_list %s cat %s : pas d'isError",
                 PAIRES[i][0], PAIRES[i][1]);
        CHECK_NO("\"isError\":true", label);
    }
}

/* Familles annoncees par le schema de `owl_list`, dans l'ordre. C'est la liste
 * que le client lit pour construire son appel : le balayage ci-dessous porte
 * donc sur ce que l'outil promet, pas sur une copie tenue a la main ici. */
static int mcp_list_buckets(char out[][64], int max) {
    if (!g_response) return -1;
    const char* p = strstr(g_response, "\"bucket\":{");
    if (!p) return -1;
    p = strstr(p, "\"enum\":[");
    if (!p) return -1;
    p += 8;
    int n = 0;
    while (*p && *p != ']' && n < max) {
        if (*p != '"') { p++; continue; }
        p++;
        int k = 0;
        while (*p && *p != '"' && k < 63) out[n][k++] = *p++;
        out[n][k] = '\0';
        if (*p) p++;
        n++;
    }
    return n;
}

/* Aucune famille du catalogue n'avale `cat`. Le canal MCP est celui ou le
 * silence coute le plus cher : un modele qui recoit la liste complete sous un
 * `cat` ignore n'a aucun moyen de savoir qu'il n'a pas filtre, et conclut.
 *
 * Deux sorts possibles selon le bucket, et le predicat exporte par cli_doc.c
 * dit lequel : famille hors domaine signalee, ou `cat` refuse faute de
 * familles. Dans les deux cas isError et la valeur fautive rappelee. */
static void test_tool_list_cat_verdict_sur_tous_les_buckets(void) {
    static char buckets[32][64];
    char req[320], label[224];

    send_req("{\"jsonrpc\":\"2.0\",\"id\":728,\"method\":\"tools/list\"}");
    int n = mcp_list_buckets(buckets, 32);
    CHECK(n == 11, "schema owl_list : 11 buckets (garde anti-vacuite)");

    int filtrants = 0, refusants = 0;
    for (int i = 0; i < n; i++) {
        int filtre = cli_doc_list_bucket_filters_by_category(buckets[i]);
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":729,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_list\",\"arguments\":"
                 "{\"bucket\":\"%s\",\"cat\":\"ZzzPasUneFamille\"}}}",
                 buckets[i]);
        send_req(req);
        snprintf(label, sizeof label, "owl_list %s cat bidon : isError set",
                 buckets[i]);
        CHECK_HAS("\"isError\":true", label);
        snprintf(label, sizeof label,
                 "owl_list %s cat bidon : valeur fautive rappelee", buckets[i]);
        CHECK_HAS("ZzzPasUneFamille", label);
        if (!filtre) {
            snprintf(label, sizeof label,
                     "owl_list %s : refus qui nomme l'option", buckets[i]);
            CHECK_HAS("--cat", label);
            refusants++;
        } else {
            filtrants++;
        }

        /* Sans `cat`, le meme bucket repond normalement : le refus ne deborde
         * pas sur l'appel que le client fait le plus souvent. */
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":730,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_list\",\"arguments\":"
                 "{\"bucket\":\"%s\"}}}", buckets[i]);
        send_req(req);
        snprintf(label, sizeof label, "owl_list %s sans cat : pas d'isError",
                 buckets[i]);
        CHECK_NO("\"isError\":true", label);
    }
    CHECK(filtrants == 7, "7 buckets filtrent par famille");
    CHECK(refusants == 4, "4 buckets refusent `cat`");
}

static void test_tool_list_bucket_manquant(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":716,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\",\"arguments\":{}}}");
    CHECK_HAS("\"isError\":true", "owl_list sans bucket : isError set");
    CHECK_HAS("categories",       "owl_list sans bucket : buckets acceptes nommes");
    CHECK_HAS("globalvars",       "owl_list sans bucket : liste complete");
    /* Surtout pas de dump implicite du catalogue complet. */
    CHECK_NO("HTTPRequête",       "owl_list sans bucket : aucun dump");
}

static void test_tool_list_bucket_invalide(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":717,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"bidules\"}}}");
    CHECK_HAS("\"isError\":true", "owl_list bucket invalide : isError set");
    CHECK_HAS("bidules",          "owl_list bucket invalide : valeur fautive rappelee");
    CHECK_HAS("natives",          "owl_list bucket invalide : buckets acceptes nommes");
}

static void test_tool_list_lang_en(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":718,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_list\","
             "\"arguments\":{\"bucket\":\"categories\",\"lang\":\"en\"}}}");
    CHECK_HAS("Arrays",          "owl_list lang=en : libelle EN des categories");
    CHECK_NO("\"isError\":true", "owl_list lang=en : pas d'isError");
}

static void test_tools_list_owl_list_schema(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":719,\"method\":\"tools/list\"}");
    CHECK_HAS("\"bucket\"",     "schema owl_list : prop bucket");
    CHECK_HAS("\"globalvars\"", "schema owl_list : enum bucket complet");
    CHECK_HAS("\"cat\"",        "schema owl_list : prop cat");
}

/*  — l'enum owl_new du schema MCP retardait sur les vrais types de
 * `owl new` : il manquait `service` parmi d'autres. */
static void test_tools_list_owl_new_schema(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":740,\"method\":\"tools/list\"}");
    CHECK_HAS("\"fenetre\"", "schema owl_new : enum kind contient fenetre");
    CHECK_HAS("\"service\"", "schema owl_new : enum kind contient service");
}

static void test_resources_list_categories(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":720,\"method\":\"resources/list\"}");
    CHECK_HAS("owl://catalog/categories",
              "resources/list : index des categories present");
}

static void test_resource_read_catalog_categories(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":721,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://catalog/categories\"}}");
    CHECK_HAS("\"contents\"", "read catalog/categories : contents");
    CHECK_HAS("UI",           "read catalog/categories : UI listee");
    CHECK_HAS("Tableaux",     "read catalog/categories : Tableaux listee");
}

static void test_templates_list_category(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":722,"
             "\"method\":\"resources/templates/list\"}");
    CHECK_HAS("owl://category/{name}", "template category expose");
}

static void test_resource_read_category(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":723,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://category/UI\"}}");
    CHECK_HAS("\"contents\"", "read category/UI : contents");
    CHECK_HAS("Info",         "read category/UI : Info present");
    CHECK_NO("\"error\"",     "read category/UI : pas d'erreur");
}

static void test_resource_read_category_inconnue(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":724,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://category/Affichage\"}}");
    CHECK_HAS("\"error\"", "read category inconnue : erreur");
    CHECK_HAS("-32002",    "read category inconnue : code ressource");
    CHECK_HAS("Affichage", "read category inconnue : nom fautif rappele");
}

static void test_resource_read_category_vide(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":725,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://category/\"}}");
    CHECK_HAS("\"error\"", "read category sans nom : erreur");
    CHECK_HAS("-32002",    "read category sans nom : code ressource");
}

static void test_tool_examples(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_examples\","
             "\"arguments\":{\"symbol\":\"HTTPRequête\"}}}");
    CHECK_HAS("scenario_", "examples HTTPRequête trouve un scénario");
}

static void test_tool_signature(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_signature\","
             "\"arguments\":{\"symbol\":\"Trace\"}}}");
    CHECK_HAS("Trace(", "signature Trace rendue");
    /* La signature inclut la flèche → escapée → dans le JSON. */
    CHECK(strstr(g_response, "\\u2192") != NULL ||
          strstr(g_response, "→") != NULL,
          "signature inclut la flèche de retour");
}

static void test_tool_signature_variadic(void) {
    /*  : owl_signature doit refleter le marqueur `...` pour les
     * natives variadiques (arity == -1), comme le fait owl_explain. */
    /*  : exemple variadique = Trace (intrinsèquement variadique,
     * jamais migrée en formes discrètes). L'ancien exemple DateVersChaîne
     * est passé en formes bornées {1,2}. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":901,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_signature\","
             "\"arguments\":{\"symbol\":\"Trace\"}}}");
    CHECK_HAS("Trace(", "signature Trace rendue");
    CHECK_HAS(", ...)", "signature variadique inclut le marqueur ...");
    CHECK_NO("\"isError\":true", "signature variadique : pas d'isError");
}

static void test_tool_signature_non_variadic(void) {
    /* Garde-fou : les signatures non-variadiques ne doivent pas gagner
     * de marqueur `...` au passage. Jour(date) est arity > 0 fixe. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":902,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_signature\","
             "\"arguments\":{\"symbol\":\"Jour\"}}}");
    CHECK_HAS("Jour(", "signature Jour rendue");
    CHECK_NO(", ...)", "signature non-variadique sans marqueur ...");
}

static void test_tool_signature_unknown(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_signature\","
             "\"arguments\":{\"symbol\":\"ZzZz\"}}}");
    CHECK_HAS("\"isError\":true", "signature inconnue : isError");
}

static void test_tool_format(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_format\","
             "\"arguments\":{\"code\":\"si vrai alors trace(\\\"ok\\\") fin\"}}}");
    CHECK_HAS("SI",    "format canonifie 'si' → 'SI'");
    CHECK_HAS("ALORS", "format canonifie 'alors' → 'ALORS'");
    CHECK_HAS("Trace", "format canonifie 'trace' → 'Trace'");
}

static void test_tool_check_valid(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_check\","
             "\"arguments\":{\"code\":\"Trace(\\\"ok\\\")\"}}}");
    CHECK_HAS("\"errors\\\":[]",   "check valide : pas d'erreur");
    CHECK_NO("\"isError\":true", "check valide : pas d'isError");
}

static void test_tool_check_invalid(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_check\","
             "\"arguments\":{\"code\":\"SI vrai foo bar\"}}}");
    CHECK_HAS("\"isError\":true", "check invalide : isError");
    CHECK_HAS("line",             "check invalide : diagnostic avec ligne");
}

/* Verrou  (surface ) — DEUX owl_check du MEME extrait dans
 * une seule session du serveur d'outils doivent rendre le MEME resultat.
 *
 * Le serveur partage un seul VM sur toute la session et n'appelle pas
 * vm_init/vm_free entre deux tools/call. Les registres de DECLARATION
 * Fenetre (vm.windows) et Etat (vm.etats) vivent dans la VM, pas dans
 * l'unite de compilation : sans nettoyage a l'entree du check, le 2e appel
 * retrouve le nom deja enregistre et emet un faux E_SEM_WINDOW_DUPLICATE
 * (+ cascade "bloc ferme trop tot"). Meme id JSON-RPC des deux cotes ->
 * les deux reponses doivent etre octet pour octet identiques.
 *
 * Rougit si vm_windows_clear() / vm_etats_clear() sont retires de
 * handle_check_command_isolated (verifie par neutralisation). */
static void test_fix1455_check_twice_same_snippet(void) {
    static const char* REQ_FEN =
        "{\"jsonrpc\":\"2.0\",\"id\":1455,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"owl_check\",\"arguments\":{\"code\":"
        "\"FEN_1455 est une Fenetre\\nPROPRIETES\\nTitre = \\\"V\\\"\\n"
        "Disposition = dispLibre\\nFIN\\nFIN\\n\"}}}";
    static const char* REQ_ETAT =
        "{\"jsonrpc\":\"2.0\",\"id\":1455,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"owl_check\",\"arguments\":{\"code\":"
        "\"ETAT_1455 est un Etat\\nPROPRIETES\\nFormat = formatA4\\nFIN\\n"
        "corps est un Corps\\nFIN\\nFIN\\n\"}}}";

    /* Volet Fenetre. */
    send_req(REQ_FEN);
    CHECK_HAS("\"errors\\\":[]", "1455 Fenetre : 1er check sans erreur");
    CHECK_NO("\"isError\":true", "1455 Fenetre : 1er check sans isError");
    char* first = g_response ? strdup(g_response) : NULL;

    send_req(REQ_FEN);
    CHECK_HAS("\"errors\\\":[]", "1455 Fenetre : 2e check sans erreur");
    CHECK_NO("\"isError\":true", "1455 Fenetre : 2e check sans isError");
    CHECK(first && g_response && strcmp(first, g_response) == 0,
          "1455 Fenetre : 2e check octet-identique au 1er");
    if (first && g_response && strcmp(first, g_response) != 0) {
        fprintf(stderr, "    [1455] 1er : %s\n    [1455] 2e  : %s\n",
                first, g_response);
    }
    free(first);

    /* Volet Etat (vm.etats fuit de la meme facon que vm.windows). */
    send_req(REQ_ETAT);
    CHECK_HAS("\"errors\\\":[]", "1455 Etat : 1er check sans erreur");
    char* firstEtat = g_response ? strdup(g_response) : NULL;

    send_req(REQ_ETAT);
    CHECK_HAS("\"errors\\\":[]", "1455 Etat : 2e check sans erreur");
    CHECK(firstEtat && g_response && strcmp(firstEtat, g_response) == 0,
          "1455 Etat : 2e check octet-identique au 1er");
    if (firstEtat && g_response && strcmp(firstEtat, g_response) != 0) {
        fprintf(stderr, "    [1455] Etat 1er : %s\n    [1455] Etat 2e  : %s\n",
                firstEtat, g_response);
    }
    free(firstEtat);
}

/* ================================================================
 *  — isolement du fichier temporaire des outils snippet
 *
 * owl_check et owl_format posent la source du client sur disque avant
 * de la donner au compilateur / formateur. Ce fichier ne doit avoir
 * AUCUN voisin : le perimetre d'un fichier OWL est son dossier, et la
 * racine temporaire du systeme est un espace partage ou n'importe quel
 * processus depose des .owl. Deux exigences verrouillees ici :
 *
 *   1. rien de neuf a la racine temporaire apres un appel — un residu
 *      laisse la devient le voisin empoisonne de l'appel suivant ;
 *   2. rien de PREEXISTANT n'y disparait — le menage ne doit porter que
 *      sur ce que l'appel a cree, jamais sur le fichier de travail d'un
 *      voisin.
 *
 * Portee mesuree : le compilateur derriere owl_check ne scanne PAS le
 * dossier (variante isolee), et le formateur s'est montre insensible au
 * voisinage sur les cas essayes. Ces deux handlers etaient donc des
 * PRODUCTEURS d'orphelins plutot que des victimes ; c'est la production
 * que ce test verrouille, plus le determinisme du rendu vis-a-vis du
 * bruit ambiant (assertion 3).
 * ================================================================ */

static const char* mcp_test_tmp_root(void) {
    const char* d = getenv("TMP");
    if (!d || !*d) d = getenv("TEMP");
    if (!d || !*d) d = getenv("TMPDIR");
    if (!d || !*d) d = "/tmp";
    return d;
}

/* Compte les entrees de la racine temporaire que CE processus a pu creer.
 *
 * Surtout pas un instantane du repertoire entier : mesure faite en ecrivant
 * ce test, la racine temporaire de la machine contenait 13748 entrees. Une
 * comparaison avant/apres plafonnee (la premiere version coupait a 4096) est
 * tronquee donc aveugle — elle a laisse passer une mutation qui retirait le
 * nettoyage. On cible donc nos propres noms, prefixes par notre PID : ce sont
 * les seuls dont on puisse affirmer qu'ils sont a nous. */
static int mcp_tmp_count_own_residue(void) {
    char pref_check[64], pref_fmt[64];
    snprintf(pref_check, sizeof pref_check, "owl_mcp_check_%lu_",
             (unsigned long)owl_test_getpid());
    snprintf(pref_fmt, sizeof pref_fmt, "owl_mcp_fmt_%lu_",
             (unsigned long)owl_test_getpid());

    DIR* d = opendir(mcp_test_tmp_root());
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, pref_check, strlen(pref_check)) == 0 ||
            strncmp(e->d_name, pref_fmt, strlen(pref_fmt)) == 0) {
            n++;
            fprintf(stderr, "    [1514] residu : %s\n", e->d_name);
        }
    }
    closedir(d);
    return n;
}

static int mcp_file_exists(const char* p) {
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void test_fix1514_snippet_sans_voisin(void) {
    /* Voisin seme par nos soins a la racine temporaire : il porte le meme
     * nom de fenetre que le snippet envoye aux outils. Avant l'isolement,
     * c'est exactement cette configuration qui rendait un refus sur nom
     * duplique — ou pire, un diagnostic attribue au voisin. */
    char voisin[1024];
    snprintf(voisin, sizeof voisin, "%s/owl_1514_voisin_%lu.owl",
             mcp_test_tmp_root(), (unsigned long)owl_test_getpid());
    FILE* v = fopen(voisin, "wb");
    if (v) {
        fputs("FEN_1514 est une Fenetre\nPROPRIETES\nDisposition = dispLibre\n"
              "FIN\nFIN\n", v);
        fclose(v);
    }
    CHECK(v != NULL, "1514 : voisin seme a la racine temporaire");

    static const char* REQ_CHECK =
        "{\"jsonrpc\":\"2.0\",\"id\":1514,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"owl_check\",\"arguments\":{\"code\":"
        "\"FEN_1514 est une Fenetre\\nPROPRIETES\\nDisposition = dispLibre\\n"
        "FIN\\nFIN\\n\"}}}";
    static const char* REQ_FMT =
        "{\"jsonrpc\":\"2.0\",\"id\":1514,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"owl_format\",\"arguments\":{\"code\":"
        "\"FEN_1514 est une Fenetre\\nPROPRIETES\\nDisposition = dispLibre\\n"
        "FIN\\nFIN\\n\"}}}";

    CHECK(mcp_tmp_count_own_residue() == 0,
          "1514 : aucun residu a nous avant les appels");

    send_req(REQ_CHECK);
    CHECK_HAS("\"errors\\\":[]", "1514 : check propre malgre le voisin homonyme");
    char* fmt1 = NULL;
    send_req(REQ_FMT);
    CHECK_NO("\"isError\":true", "1514 : format sans erreur malgre le voisin");
    if (g_response) fmt1 = strdup(g_response);

    /* 1. Aucun residu : tout ce que les handlers ont cree a disparu tout
     * de suite. Un residu ici est le voisin empoisonne de l'appel suivant. */
    CHECK(mcp_tmp_count_own_residue() == 0,
          "1514 : aucun residu a la racine temporaire apres les appels");

    /* 2. Aucun degat collateral : le fichier d'un tiers ne bouge pas. */
    CHECK(mcp_file_exists(voisin),
          "1514 : le fichier du voisin survit au menage");

    /* 3. Determinisme : le rendu ne doit pas dependre du voisinage. On
     * retire le voisin et on redemande le meme formatage. */
    owl_test_unlink(voisin);
    send_req(REQ_FMT);
    CHECK(fmt1 && g_response && strcmp(fmt1, g_response) == 0,
          "1514 : format identique avec et sans voisin");
    free(fmt1);
}

static void test_tool_translate_fr_to_en(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":120,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_translate\","
             "\"arguments\":{\"code\":\"si vrai alors trace(\\\"ok\\\") fin\","
             "\"to\":\"en\"}}}");
    CHECK_HAS("IF",   "translate FR->EN : 'si' -> 'IF'");
    CHECK_HAS("THEN", "translate FR->EN : 'alors' -> 'THEN'");
    CHECK_HAS("END",  "translate FR->EN : 'fin' -> 'END'");
    CHECK_NO("\"isError\":true", "translate FR->EN : pas d'isError");
}

static void test_tool_translate_en_to_fr(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":121,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_translate\","
             "\"arguments\":{\"code\":\"IF TRUE THEN Trace(\\\"ok\\\") END\","
             "\"to\":\"fr\"}}}");
    CHECK_HAS("SI",    "translate EN->FR : 'IF' -> 'SI'");
    CHECK_HAS("ALORS", "translate EN->FR : 'THEN' -> 'ALORS'");
    CHECK_HAS("FIN",   "translate EN->FR : 'END' -> 'FIN'");
    CHECK_NO("\"isError\":true", "translate EN->FR : pas d'isError");
}

static void test_tool_translate_bad_to(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":122,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_translate\","
             "\"arguments\":{\"code\":\"Trace(\\\"ok\\\")\","
             "\"to\":\"de\"}}}");
    CHECK_HAS("\"isError\":true", "translate 'to' invalide : isError");
}

static void test_tool_translate_missing_to(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":123,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_translate\","
             "\"arguments\":{\"code\":\"Trace(\\\"ok\\\")\"}}}");
    CHECK_HAS("\"isError\":true", "translate 'to' manquant : isError");
}

static void test_tool_unknown(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_doesnt_exist\",\"arguments\":{}}}");
    CHECK_HAS("\"error\"", "unknown tool : erreur protocole");
    CHECK_HAS("-32602",    "unknown tool : code -32602");
}

static void test_resources_list(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"resources/list\"}");
    /* Au moins 9 fondations + 4 catalogues = 13 URIs. */
    CHECK(count_occurrences("\"uri\":") >= 13,
          "resources/list >= 13 entrées");
    CHECK_HAS("owl://public_doc/", "public_docs listées");
    CHECK_HAS("owl://catalog/natives",  "catalogue natives présent");
    CHECK_HAS("owl://catalog/types",    "catalogue types présent");
}

static void test_resources_templates(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":16,"
             "\"method\":\"resources/templates/list\"}");
    CHECK_HAS("owl://native/{name}",   "template native exposé");
    CHECK_HAS("owl://type/{name}",     "template type exposé");
    CHECK_HAS("owl://scenario/{name}", "template scenario exposé");
}

static void test_resource_read_public_doc(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://public_doc/00_apercu_du_langage\"}}");
    CHECK_HAS("\"contents\"",          "read public_doc : contents");
    CHECK_HAS("\"mimeType\":\"text/markdown\"", "read public_doc : mime");
    CHECK_HAS("Aper", "read public_doc : contenu du markdown");
}

static void test_resource_read_native(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://native/Trace\"}}");
    CHECK_HAS("Trace",      "read native : nom du symbole");
    CHECK_HAS("Signatures", "read native : section signatures");
}

static void test_resource_read_catalog(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://catalog/types\"}}");
    CHECK_HAS("primitif", "read catalog types : section primitifs");
    CHECK_HAS("Cadre",    "read catalog types : Cadre listé");
}

/*  — les enumerations ne sont plus une famille navigable :
 * `owl://catalog/enums` ne doit plus etre annonce ni accepte. Il rendait le
 * diagnostic « sous-commande inconnue » en guise de contenu, sans aucun signal
 * d'echec — un client le lisait comme une reponse valide.
 *
 * Le verdict machine (-32002) suffit a le verrouiller : `enums` est ecarte par
 * CATALOG_FAMILIES avant tout appel au CLI, donc aucun diagnostic ne peut plus
 * remonter jusqu'ici. Une sentinelle sur le texte de ce diagnostic ne pourrait
 * pas rougir — elle a ete retiree plutot que gardee pour la forme. */
static void test_catalog_enums_retire(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":1575,\"method\":\"resources/list\"}");
    CHECK_NO("owl://catalog/enums",
             "resources/list : catalogue enums retire");
    CHECK_HAS("owl://catalog/types",
              "resources/list : catalogue types toujours annonce");

    send_req("{\"jsonrpc\":\"2.0\",\"id\":1576,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://catalog/enums\"}}");
    CHECK_HAS("\"error\"", "read catalog/enums : erreur");
    CHECK_HAS("-32002",    "read catalog/enums : code ressource");
    CHECK_NO("types, enums",
             "read catalog/enums : enums absent de l'enumeration des valides");
}

/* Contre-epreuve : seule la famille disparait. La fiche d'une enumeration
 * par son nom reste resolvable, gabarit compris. */
static void test_fiche_enum_preservee(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":1577,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://enum/typeMime\"}}");
    CHECK_HAS("\"contents\"",   "read enum/typeMime : contents");
    CHECK_HAS("typeMimeJSON",   "read enum/typeMime : valeurs listees");
    CHECK_NO("\"error\"",       "read enum/typeMime : pas d'erreur");

    send_req("{\"jsonrpc\":\"2.0\",\"id\":1578,"
             "\"method\":\"resources/templates/list\"}");
    CHECK_HAS("owl://enum/{name}", "templates/list : gabarit enum conserve");
}

/* ----------------------------------------------------------------
 * Verrou de symetrie des familles de catalogue
 * ----------------------------------------------------------------
 * Trois surfaces decrivent le meme ensemble : ce que resources/list
 * annonce, ce que resources/read accepte, ce que l'outil owl_list sert.
 * Le test ne fige aucune liste — il lit les deux enumerations que le
 * serveur publie a l'execution, exige qu'elles coincident, puis exerce
 * chaque famille sur les deux canaux. Une famille ajoutee d'un seul cote
 * rougit ici, quel que soit le cote oublie ; une famille presente partout
 * mais que `cli_doc_list` ne dispatche pas rougit aussi — son code retour
 * vaut 1, que les deux canaux traduisent en erreur (cf. , ou ce
 * meme cas servait le diagnostic comme contenu).
 * ---------------------------------------------------------------- */

#define MAX_CATALOG_FAMILIES 32
#define CATALOG_FAMILY_LEN   32

typedef char CatalogFamilyName[CATALOG_FAMILY_LEN];

/* Longueur du premier bloc `"text":"..."` de la reponse, echappements
 * compris. 0 si la reponse n'en porte pas. Sert a distinguer « a repondu » de
 * « a repondu quelque chose » : un handler qui sert une chaine vide passe
 * sinon tous les controles de forme. */
static size_t response_text_len(void) {
    if (!g_response) return 0;
    const char* p = strstr(g_response, "\"text\":\"");
    if (!p) return 0;
    p += sizeof "\"text\":\"" - 1;
    const char* q = p;
    while (*q && *q != '"') {
        if (*q == '\\' && q[1]) q++;
        q++;
    }
    return (size_t)(q - p);
}

static int family_known(CatalogFamilyName* fams, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (strcmp(fams[i], name) == 0) return 1;
    return 0;
}

static int family_push(CatalogFamilyName* fams, int n, const char* s,
                       size_t len) {
    if (n >= MAX_CATALOG_FAMILIES || len == 0 || len >= CATALOG_FAMILY_LEN)
        return n;
    memcpy(fams[n], s, len);
    fams[n][len] = '\0';
    return n + 1;
}

/* Familles annoncees : chaque `"uri":"owl://catalog/<f>"` de resources/list. */
static int collect_announced_families(CatalogFamilyName* out) {
    if (!g_response) return 0;
    static const char PREFIX[] = "\"uri\":\"owl://catalog/";
    int n = 0;
    const char* p = g_response;
    while ((p = strstr(p, PREFIX)) != NULL) {
        p += sizeof PREFIX - 1;
        const char* q = strchr(p, '"');
        if (!q) break;
        n = family_push(out, n, p, (size_t)(q - p));
        p = q;
    }
    return n;
}

/* Familles servies par l'outil : enum de la propriete `bucket` du schema
 * owl_list dans tools/list. Cible ce schema precisement — l'enum `cat` de
 * owl_explain contient `enums` (fiche par nom, legitime) et fausserait tout
 * scan global. */
static int collect_tool_families(CatalogFamilyName* out) {
    if (!g_response) return 0;
    const char* tool = strstr(g_response, "\"name\":\"owl_list\"");
    if (!tool) return 0;
    const char* prop = strstr(tool, "\"bucket\":{");
    if (!prop) return 0;
    const char* e = strstr(prop, "\"enum\":[");
    if (!e) return 0;
    e += sizeof "\"enum\":[" - 1;
    const char* end = strchr(e, ']');
    if (!end) return 0;
    int n = 0;
    const char* p = e;
    for (;;) {
        const char* a = strchr(p, '"');
        if (!a || a >= end) break;
        const char* b = strchr(a + 1, '"');
        if (!b || b > end) break;
        n = family_push(out, n, a + 1, (size_t)(b - a - 1));
        p = b + 1;
    }
    return n;
}

static void test_catalog_familles_symetrie(void) {
    CatalogFamilyName annonce[MAX_CATALOG_FAMILIES];
    CatalogFamilyName outil[MAX_CATALOG_FAMILIES];
    CatalogFamilyName toutes[MAX_CATALOG_FAMILIES];
    char req[320], label[224];

    send_req("{\"jsonrpc\":\"2.0\",\"id\":1583,\"method\":\"resources/list\"}");
    int n_annonce = collect_announced_families(annonce);

    send_req("{\"jsonrpc\":\"2.0\",\"id\":1584,\"method\":\"tools/list\"}");
    int n_outil = collect_tool_families(outil);

    /* Anti-vacuite : un extracteur casse rendrait 0 et validerait tout. */
    CHECK(n_annonce >= 10, "symetrie : familles annoncees non vides");
    CHECK(n_outil >= 10,   "symetrie : familles servies par owl_list non vides");
    CHECK(n_annonce == n_outil,
          "symetrie : autant de familles annoncees que servies par owl_list");

    int n_toutes = 0;
    for (int i = 0; i < n_annonce; i++) {
        n_toutes = family_push(toutes, n_toutes, annonce[i],
                               strlen(annonce[i]));
        snprintf(label, sizeof label,
                 "symetrie : famille annoncee '%s' servie par owl_list",
                 annonce[i]);
        CHECK(family_known(outil, n_outil, annonce[i]), label);
    }
    for (int i = 0; i < n_outil; i++) {
        if (!family_known(toutes, n_toutes, outil[i]))
            n_toutes = family_push(toutes, n_toutes, outil[i],
                                   strlen(outil[i]));
        snprintf(label, sizeof label,
                 "symetrie : famille owl_list '%s' annoncee par resources/list",
                 outil[i]);
        CHECK(family_known(annonce, n_annonce, outil[i]), label);
    }

    /* Chaque famille vue quelque part repond partout, et rend du contenu —
     * pas le diagnostic d'une sous-commande que le dispatch ignore.
     *
     * Le constat passe par les canaux machine seuls (code d'erreur cote
     * ressource, drapeau isError cote outil) : depuis  les deux
     * exploitent le code retour de `cli_doc_list`, qui vaut 1 sur une famille
     * non dispatchee. La sentinelle posee sur le texte du diagnostic est donc
     * retiree — c'etait la derniere dependance d'un verrou a une phrase
     * destinee a l'oeil. */
    for (int i = 0; i < n_toutes; i++) {
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":1585,\"method\":\"resources/read\","
                 "\"params\":{\"uri\":\"owl://catalog/%s\"}}", toutes[i]);
        send_req(req);
        snprintf(label, sizeof label,
                 "symetrie : read owl://catalog/%s rend un contenu", toutes[i]);
        CHECK_HAS("\"contents\"", label);
        snprintf(label, sizeof label,
                 "symetrie : read owl://catalog/%s sans erreur", toutes[i]);
        CHECK_NO("-32002", label);
        /* Et le contenu servi n'est pas une coquille : le plus petit catalogue
         * rendu (globalvars) pese 111 octets, le seuil est pose sous cette
         * mesure. Une famille dispatchee qui n'imprimerait rien passerait
         * autrement les deux assertions ci-dessus. */
        snprintf(label, sizeof label,
                 "symetrie : read owl://catalog/%s sert un contenu non vide",
                 toutes[i]);
        CHECK(response_text_len() >= 64, label);

        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":1586,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_list\",\"arguments\":"
                 "{\"bucket\":\"%s\"}}}", toutes[i]);
        send_req(req);
        snprintf(label, sizeof label,
                 "symetrie : owl_list bucket='%s' repond", toutes[i]);
        CHECK_NO("\"isError\":true", label);
        snprintf(label, sizeof label,
                 "symetrie : owl_list bucket='%s' sert un contenu non vide",
                 toutes[i]);
        CHECK(response_text_len() >= 64, label);
    }

    /* Le message d'erreur nomme les familles reellement acceptees : il est
     * derive de la meme source, pas recopie a cote. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":1587,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://catalog/zzzpasunefamille\"}}");
    CHECK_HAS("-32002", "famille inconnue : code ressource");
    for (int i = 0; i < n_toutes; i++) {
        snprintf(label, sizeof label,
                 "famille inconnue : message nomme '%s'", toutes[i]);
        CHECK_HAS(toutes[i], label);
    }

    /*  : les enumerations ne sont pas une famille de catalogue. La
     * source unique ne doit pas les reintroduire par la bande. */
    CHECK(!family_known(annonce, n_annonce, "enums"),
          "ADR_0096 : enums absent des familles annoncees");
    CHECK(!family_known(outil, n_outil, "enums"),
          "ADR_0096 : enums absent des familles servies par owl_list");
    CHECK_NO("enums", "famille inconnue : enums absent des valeurs acceptees");
}

static void test_resource_read_unknown_uri(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://public_doc/inexistant\"}}");
    CHECK_HAS("\"error\"", "URI inconnu : erreur");
    CHECK_HAS("-32002",    "URI inconnu : code ressource");
}

static void test_resource_read_bad_scheme(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"https://wrong-scheme\"}}");
    CHECK_HAS("\"error\"",  "scheme invalide : erreur");
    CHECK_HAS("owl://",     "message mentionne le scheme attendu");
}

static void test_id_string(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":\"str-id\",\"method\":\"ping\"}");
    CHECK_HAS("\"id\":\"str-id\"", "id string conservé tel quel");
}

/* ----------------------------------------------------------------
 * Tests cross-référence : chaque primitif déclaré dans cli_doc.c
 * doit être routable via le MCP, et chaque fondation embarquée doit
 * être lisible. Évite la divergence entre CLI (source de vérité des
 * primitives) et MCP (consommateur).
 * ---------------------------------------------------------------- */

static void test_cross_ref_primitives(void) {
    int n = cli_doc_primitive_count();
    CHECK(n >= 10, "au moins 10 primitives déclarées dans cli_doc.c");
    for (int i = 0; i < n; i++) {
        const char* name = NULL;
        if (!cli_doc_primitive_at(i, &name, NULL, NULL)) continue;
        char req[512];
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"resources/read\","
                 "\"params\":{\"uri\":\"owl://type/%s\"}}",
                 100 + i, name);
        send_req(req);
        char label[256];
        snprintf(label, sizeof label,
                 "primitive '%s' lisible via owl://type/", name);
        CHECK_HAS("\"contents\"", label);
        snprintf(label, sizeof label,
                 "primitive '%s' contenu non-error", name);
        CHECK_NO("\"error\"", label);
    }
}

static void test_cross_ref_public_docs(void) {
    int count = 0;
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) count++;
    CHECK(count >= 5, "au moins 5 fondations embarquées");
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        char req[1024];
        snprintf(req, sizeof req,
                 "{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"resources/read\","
                 "\"params\":{\"uri\":\"owl://public_doc/%s\"}}",
                 d->name);
        send_req(req);
        char label[256];
        snprintf(label, sizeof label, "public_doc '%s' lisible", d->name);
        CHECK_HAS("\"contents\"", label);
    }
}

/* ----------------------------------------------------------------
 * Tests widgets (BUCKET_WIDGET + template owl://widget/{name})
 * ---------------------------------------------------------------- */

static void test_explain_widget_bouton(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":300,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Bouton\"}}}");
    CHECK_HAS("champ",      "Bouton reconnu comme champ");
    CHECK_HAS("Propriétés", "Bouton affiche ses propriétés");
    CHECK_HAS("Libellé",        "Bouton.Libellé présent");
    CHECK_HAS("Events",         "Bouton affiche ses events");
    CHECK_HAS("surClic",        "Bouton.surClic présent");
}

static void test_explain_widget_fenetre(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":301,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Fenêtre\"}}}");
    CHECK_HAS("champ", "Fenêtre reconnue (UTF-8)");
    CHECK_HAS("Titre",      "Fenêtre.Titre présent");
}

static void test_explain_widget_cat_filter(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":302,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Bouton\",\"cat\":\"controls\"}}}");
    CHECK_HAS("champ", "filtre --cat controls actif");
}

static void test_resource_read_widget(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":303,\"method\":\"resources/read\","
             "\"params\":{\"uri\":\"owl://control/Saisie\"}}");
    CHECK_HAS("champ",   "owl://control/Saisie résout");
    CHECK_HAS("Propriétés",   "Saisie expose ses propriétés");
}

static void test_templates_list_widget(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":304,"
             "\"method\":\"resources/templates/list\"}");
    CHECK_HAS("owl://control/{name}", "template control exposé");
}

/* ----------------------------------------------------------------
 * Tests méthodes fluent dans explain
 * ---------------------------------------------------------------- */

static void test_methods_fluent_chaine(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":305,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"chaîne\"}}}");
    CHECK_HAS("Méthodes", "section Méthodes présente pour chaîne");
    /* Au moins une méthode connue sur chaîne — MarkdownVersTexte est
     * stable dans le registry. */
    CHECK_HAS(".Markdown", "chaîne expose au moins une méthode fluent");
}

/* ----------------------------------------------------------------
 * Tests owl_where — reverse lookup (qui m'utilise ?)
 * ---------------------------------------------------------------- */

static void test_where_prop_grise(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":400,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"Grisé\"}}}");
    CHECK_HAS("Acceptée par", "Grisé : prop résolue + utilisateurs listés");
    CHECK_HAS("Bouton",       "Bouton listé comme utilisateur");
    /* Forme canon du champ, pas la cle ASCII du catalogue : ce que l'outil
     * sert au modele est ce qu'il recopiera dans du code OWL. */
    CHECK_HAS("Libellé",      "Libellé listé en forme canon");
    CHECK_NO("  Libelle\\n",  "plus de Libelle sans accent");
}

static void test_where_prop_strip_dot_prefix(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":401,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"..Visible\"}}}");
    CHECK_HAS("Acceptée par", "..Visible : prop résolue après strip");
}

static void test_where_widget_corps(void) {
    /* Corps est un sous-bloc Etat. uses → contexte parent. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":402,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"Corps\"}}}");
    CHECK_HAS("Contexte parent", "Corps : section contexte parent présente");
    CHECK_HAS("État",            "Corps : indique État comme parent");
}

static void test_where_widget_bouton(void) {
    /* Bouton est un widget runtime. uses → peut vivre dans Fenêtre etc. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":403,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"Bouton\"}}}");
    CHECK_HAS("Contexte parent", "Bouton : contexte parent listé");
    CHECK_HAS("Fenêtre",         "Bouton : Fenêtre comme contexte possible");
}

static void test_where_type_not_applicable(void) {
    /* Un type primitif n'a pas de reverse lookup utile via uses. */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":404,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"chaîne\"}}}");
    CHECK_HAS("\"isError\":true", "uses sur type primitif → isError");
    CHECK_HAS("owl explain",     "redirige vers owl explain");
}

static void test_where_unknown(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":405,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"ZzZz_inconnu\"}}}");
    CHECK_HAS("\"isError\":true", "uses inconnu → isError");
}

/* ================================================================
 * format: "json" — generalisation sur les 4 tools "doc"
 *
 * Les payloads MCP echappent les guillemets dans le content[].text, donc
 * on cherche `\"kind\":` (avec backslash) plutot que `"kind":` brut. La
 * presence des balises JSON connues atteste que la sortie n'est PAS le
 * texte humain par defaut.
 * ================================================================ */

static void test_explain_format_json(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":600,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Trace\",\"format\":\"json\"}}}");
    CHECK_HAS("\\\"kind\\\":",        "explain json : champ kind");
    CHECK_HAS("\\\"name\\\":",        "explain json : champ name");
    CHECK_HAS("\\\"related\\\":",     "explain json : maillage related propage");
    CHECK_NO("Signatures",           "explain json : pas de prose 'Signatures'");
    CHECK_NO("\"isError\":true",     "explain json : pas d'isError");
}

static void test_explain_format_text_default(void) {
    /* Sans format explicite, on doit toujours recevoir du texte (defaut). */
    send_req("{\"jsonrpc\":\"2.0\",\"id\":601,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Trace\"}}}");
    CHECK_HAS("Signatures",     "explain defaut : prose texte");
    CHECK_NO("\\\"kind\\\":",   "explain defaut : pas de JSON");
}

static void test_search_format_json(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":602,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_search\","
             "\"arguments\":{\"query\":\"HTTP\",\"format\":\"json\"}}}");
    CHECK_HAS("\\\"query\\\":",      "search json : champ query");
    CHECK_HAS("\\\"natives\\\":",    "search json : champ natives");
    CHECK_HAS("\\\"total\\\":",      "search json : champ total");
}

static void test_examples_format_json(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":603,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_examples\","
             "\"arguments\":{\"symbol\":\"HTTPRequête\",\"format\":\"json\"}}}");
    CHECK_HAS("\\\"scenarios\\\":",  "examples json : champ scenarios");
    CHECK_HAS("\\\"symbol\\\":",     "examples json : champ symbol");
}

static void test_where_format_json(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":604,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_where\","
             "\"arguments\":{\"symbol\":\"Grisé\",\"format\":\"json\"}}}");
    CHECK_HAS("\\\"property\\\":",   "where json : champ property");
    CHECK_HAS("\\\"users\\\":",      "where json : champ users");
}

/* Tolerance casse sur la valeur de format. */
static void test_explain_format_uppercase(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":605,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"owl_explain\","
             "\"arguments\":{\"symbol\":\"Trace\",\"format\":\"JSON\"}}}");
    CHECK_HAS("\\\"kind\\\":",  "format='JSON' (majuscules) accepte");
}

/* InputSchema expose bien la prop `format` enum [text, json]. Sur
 * tools/list, le schema est emis tel quel (pas dans un content text)
 * → guillemets simples, non echappes. */
static void test_tools_list_advertises_format(void) {
    send_req("{\"jsonrpc\":\"2.0\",\"id\":606,\"method\":\"tools/list\"}");
    CHECK_HAS("\"format\":",  "tools/list : prop format documentee");
    CHECK_HAS("\"text\"",     "format enum mentionne text");
    CHECK_HAS("\"json\"",     "format enum mentionne json");
}

/*  — stdlib_init() doit être idempotent. Avant fix, un 2ème
 * appel poussait à nouveau toutes les natives/types/consts dans le
 * registry (g_natives, g_types, ...) sans dédoublonnage, doublant les
 * comptes exposés par owl_search via MCP (bug #8 beta-test). On
 * vérifie ici que owl_registry_count() est stable après ré-init. */
static void test_stdlib_init_idempotent(void) {
    int before_natives = owl_registry_count();
    int before_types   = owl_registry_types_count();
    int before_consts  = owl_registry_consts_count();
    int before_enums   = owl_registry_enums_count();

    stdlib_init();
    stdlib_init();

    CHECK(owl_registry_count()        == before_natives, "stdlib_init idempotent : natives count stable");
    CHECK(owl_registry_types_count()  == before_types,   "stdlib_init idempotent : types count stable");
    CHECK(owl_registry_consts_count() == before_consts,  "stdlib_init idempotent : consts count stable");
    CHECK(owl_registry_enums_count()  == before_enums,   "stdlib_init idempotent : enums count stable");
}

/* ================================================================
 * Entry point
 * ================================================================ */

int cli_mcp_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_initialize();
    test_initialize_instructions();
    test_ping();
    test_notification_no_response();
    test_unknown_method();
    test_parse_error();
    test_tools_list();
    test_tool_explain_ok();
    test_tool_explain_unknown();
    test_tool_explain_bad_flag();
    test_tool_explain_error_fr();
    test_tool_explain_error_en();
    test_tool_explain_error_unknown();
    test_tool_search();
    test_tool_limits_list();
    test_tool_limits_lookup();
    test_tool_list_categories();
    test_tool_list_categories_json();
    test_tool_list_natives_cat_ui();
    test_tool_list_natives_cat_casse();
    test_tool_list_natives_cat_en();
    test_tool_list_natives_cat_inconnue();
    test_tool_list_cat_inconnue_tous_buckets();
    test_tool_list_cat_valide_pas_d_erreur();
    test_tool_list_cat_verdict_sur_tous_les_buckets();
    test_tool_list_bucket_manquant();
    test_tool_list_bucket_invalide();
    test_tool_list_lang_en();
    test_tools_list_owl_list_schema();
    test_tools_list_owl_new_schema();
    test_resources_list_categories();
    test_resource_read_catalog_categories();
    test_templates_list_category();
    test_resource_read_category();
    test_resource_read_category_inconnue();
    test_resource_read_category_vide();
    test_tool_examples();
    test_tool_signature();
    test_tool_signature_variadic();
    test_tool_signature_non_variadic();
    test_tool_signature_unknown();
    test_tool_format();
    test_tool_check_valid();
    test_tool_check_invalid();
    test_fix1455_check_twice_same_snippet();
    test_fix1514_snippet_sans_voisin();
    test_tool_translate_fr_to_en();
    test_tool_translate_en_to_fr();
    test_tool_translate_bad_to();
    test_tool_translate_missing_to();
    test_tool_unknown();
    test_resources_list();
    test_resources_templates();
    test_resource_read_public_doc();
    test_resource_read_native();
    test_resource_read_catalog();
    test_catalog_enums_retire();
    test_catalog_familles_symetrie();
    test_fiche_enum_preservee();
    test_resource_read_unknown_uri();
    test_resource_read_bad_scheme();
    test_id_string();

    test_cross_ref_primitives();
    test_cross_ref_public_docs();
    test_explain_widget_bouton();
    test_explain_widget_fenetre();
    test_explain_widget_cat_filter();
    test_resource_read_widget();
    test_templates_list_widget();
    test_methods_fluent_chaine();
    test_where_prop_grise();
    test_where_prop_strip_dot_prefix();
    test_where_widget_corps();
    test_where_widget_bouton();
    test_where_type_not_applicable();
    test_where_unknown();

    /* Templates : list + show via MCP. */
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":700,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_template\","
                 "\"arguments\":{}}}");
        CHECK_HAS("Templates disponibles", "owl_template list : header");
        CHECK_HAS("hello",     "list : template hello visible");
        CHECK_HAS("classe",    "list : template classe visible");
        CHECK_HAS("test",      "list : template test visible");
        CHECK_HAS("fenetre",   "list : template fenetre visible");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":701,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_template\","
                 "\"arguments\":{\"name\":\"hello\"}}}");
        CHECK_HAS("Trace",     "show hello : code contient Trace");
        CHECK_HAS("est une cha", "show hello : code contient declaration typee");
        CHECK_NO("@nom:",      "show hello : header meta strippe");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":702,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_template\","
                 "\"arguments\":{\"name\":\"inexistant\"}}}");
        CHECK_HAS("\"isError\":true", "show inexistant : isError");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":703,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_template\","
                 "\"arguments\":{\"format\":\"json\"}}}");
        CHECK_HAS("\\\"templates\\\":", "list --json : champ templates");
        CHECK_HAS("\\\"count\\\":",     "list --json : champ count");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":704,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_template\","
                 "\"arguments\":{\"name\":\"classe\",\"format\":\"json\"}}}");
        CHECK_HAS("\\\"name\\\":", "show --json : champ name");
        CHECK_HAS("\\\"code\\\":", "show --json : champ code");
    }

    /* format: "json" — sortie structuree consommable par agent IA. */
    test_explain_format_json();
    test_explain_format_text_default();
    test_search_format_json();
    test_examples_format_json();
    test_where_format_json();
    test_explain_format_uppercase();
    test_tools_list_advertises_format();

    /*  — registry pas doublé par double init stdlib. */
    test_stdlib_init_idempotent();

    /* États DSL + catalogue events parallèle. */
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":500,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_explain\","
                 "\"arguments\":{\"symbol\":\"Etat\"}}}");
        CHECK_HAS("sous-bloc",     "Etat reconnu comme sous-bloc DSL");
        CHECK_HAS("Filtre",        "Etat expose event Filtre");
        CHECK_HAS("Lecture",       "Etat expose event Lecture");
        CHECK_HAS("[Énumération]", "Etat : multi-bucket avec enum d'affichage");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":501,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_explain\","
                 "\"arguments\":{\"symbol\":\"Corps\"}}}");
        CHECK_HAS("sous-bloc", "Corps reconnu comme sous-bloc");
        CHECK_HAS("Filtre",    "Corps catalogue Etat events visible");
    }
    {
        send_req("{\"jsonrpc\":\"2.0\",\"id\":502,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"owl_explain\","
                 "\"arguments\":{\"symbol\":\"EnTeteDocument\"}}}");
        CHECK_HAS("sous-bloc",     "EnTeteDocument reconnu");
        CHECK_HAS("DocumentHeader","alias EN exposé");
    }

    free(g_response);
    g_response = NULL;

    fprintf(stderr, "cli_mcp_self_test: %d cas executes (%d pass, %d fail)\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "cli_mcp_self_test: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
