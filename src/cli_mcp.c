// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* cli_mcp.c — boucle MCP stdio, V1 squelette.
 *
 * JSON-RPC 2.0 NDJSON : chaque message = une ligne sur stdin (requête
 * du client) ou stdout (réponse du serveur). Pas de framing
 * Content-Length comme en LSP — convention MCP standard.
 *
 * stderr est réservé aux logs. Tout printf parasite vers stdout casse
 * le protocole côté client (impossible de parser un JSON cassé en cours
 * de ligne). Discipline absolue : aucune écriture stdout hors de
 * `send_response`. */

#include "cli_mcp.h"
#include "cli_doc.h"        /* cli_doc_explain et autres handlers CLI */
#include "formatter.h"      /* owl_format_file pour owl_format */
#include "owl_registry.h"   /* lookup direct pour owl_signature */
#include "owl_limits.h"     /* owl_limits tool */
#include "embedded_docs.h"  /* EMBEDDED_PUBLIC_DOCS / EMBEDDED_SCENARIOS */
#include "owl_new.h"        /* owl_new_generate pour le tool owl_new */
#include "lsp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>       /* _mkdir / _rmdir */
  #include <process.h>      /* _getpid pour tmp filename unique */
  #define owl_fileno  _fileno
  #define owl_setmode _setmode
  #define owl_dup     _dup
  #define owl_dup2    _dup2
  #define owl_close   _close
  #define owl_open    _open
  #define owl_unlink  _unlink
  #define owl_getpid  _getpid
  #define owl_mkdir(p) _mkdir(p)
  #define owl_rmdir   _rmdir
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_fileno  fileno
  #define owl_dup     dup
  #define owl_dup2    dup2
  #define owl_close   close
  #define owl_open    open
  #define owl_unlink  unlink
  #define owl_getpid  getpid
  #define owl_mkdir(p) mkdir((p), 0700)
  #define owl_rmdir   rmdir
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ----------------------------------------------------------------
 * Bac a sable temporaire
 *
 * Les outils snippet (owl_check, owl_format) doivent poser la source
 * du LLM sur disque avant de la donner au compilateur ou au formateur.
 * Ecrire ce fichier a la racine du repertoire temporaire du systeme le
 * dote d'un VOISINAGE : OWL resout le perimetre d'un fichier par son
 * dossier, donc tout .owl laisse la par un autre processus entre dans
 * le perimetre du snippet. Ce repertoire est partage et vivant (autres
 * outils, autres sessions), le verdict devenait donc dependant du bruit
 * ambiant. Et un arret brutal y laissait un orphelin de plus, qui
 * empoisonnait le suivant.
 *
 * Regle : un repertoire prive par appel, un seul fichier dedans, efface
 * immediatement. Le menage ne touche QUE ce que l'appel a cree — jamais
 * le contenu de la racine temporaire, qui appartient a d'autres.
 * ---------------------------------------------------------------- */

/* Racine temporaire du systeme, sans separateur final. Pas de chemin en
 * dur : Windows n'expose pas les memes variables qu'un Unix. */
static const char* mcp_tmp_root(void) {
    const char* d = getenv("TMP");
    if (!d || !*d) d = getenv("TEMP");
    if (!d || !*d) d = getenv("TMPDIR");
    if (!d || !*d) d = "/tmp";
    return d;
}

/* Repertoires prives encore ouverts. Un handler n'en detient qu'un a la
 * fois, mais le filet de sortie (atexit) doit pouvoir balayer celui qui
 * etait en cours si l'appel a saute son nettoyage. */
#define MCP_SCRATCH_MAX 8
static char  g_scratch[MCP_SCRATCH_MAX][1024];
static int   g_scratch_atexit = 0;

/* Vide puis supprime le repertoire prive du slot. L'enumeration reste
 * bornee a ce repertoire — cree par nous, plat, sans voisin — donc le
 * menage ne peut pas mordre sur le fichier de travail d'un tiers. */
static void mcp_scratch_wipe_slot(int i) {
    if (g_scratch[i][0] == '\0') return;
    DIR* d = opendir(g_scratch[i]);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", g_scratch[i], e->d_name);
            owl_unlink(p);
        }
        closedir(d);
    }
    owl_rmdir(g_scratch[i]);
    g_scratch[i][0] = '\0';
}

/* Filet de sortie : un arret entre la creation et le nettoyage nominal ne
 * doit rien laisser derriere lui. C'est l'interruption, pas le chemin
 * nominal, qui a produit les orphelins accumules. */
static void mcp_scratch_wipe_all(void) {
    for (int i = 0; i < MCP_SCRATCH_MAX; i++) mcp_scratch_wipe_slot(i);
}

/* Cree un repertoire prive et y compose le chemin d'un fichier unique.
 * `leaf` est le nom du fichier a l'interieur (il n'a aucun voisin, donc
 * son nom est libre). Rend l'index du slot, ou -1. */
static int mcp_scratch_open(const char* tag, const char* leaf,
                            char* out_file, size_t out_sz) {
    int slot = -1;
    for (int i = 0; i < MCP_SCRATCH_MAX; i++) {
        if (g_scratch[i][0] == '\0') { slot = i; break; }
    }
    if (slot < 0) return -1;

    if (!g_scratch_atexit) {
        atexit(mcp_scratch_wipe_all);
        g_scratch_atexit = 1;
    }

    static unsigned long seq = 0;
    const char* root = mcp_tmp_root();
    for (int essai = 0; essai < 64; essai++) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/owl_%s_%lu_%lu", root, tag,
                 (unsigned long)owl_getpid(), seq++);
        if (owl_mkdir(dir) != 0) continue;   /* deja pris : suivant */
        snprintf(g_scratch[slot], sizeof g_scratch[slot], "%s", dir);
        snprintf(out_file, out_sz, "%s/%s", dir, leaf);
        return slot;
    }
    return -1;
}

static void mcp_scratch_close(int slot) {
    if (slot >= 0 && slot < MCP_SCRATCH_MAX) mcp_scratch_wipe_slot(slot);
}

/* ----------------------------------------------------------------
 * Version du protocole MCP que ce serveur implémente.
 * Le client peut proposer une autre — on renvoie celle-ci, à lui
 * de décider s'il continue ou se déconnecte.
 * ---------------------------------------------------------------- */
#define OWL_MCP_PROTOCOL_VERSION "2025-06-18"
#define OWL_MCP_SERVER_NAME      "owl"
#define OWL_MCP_SERVER_VERSION   "1.0.0"

/* ----------------------------------------------------------------
 * I/O
 * ---------------------------------------------------------------- */

/* Lit une ligne complète sur stdin (NDJSON). Retourne le buffer
 * malloc'd (à free par l'appelant) ou NULL si EOF ou erreur.
 * Le \n final n'est PAS inclus ; un \r éventuel est trimmé. */
static char* read_line_alloc(FILE* in, size_t* out_len) {
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    int c;
    int got_any = 0;
    while ((c = fgetc(in)) != EOF) {
        got_any = 1;
        if (c == '\n') break;
        if (len + 1 >= cap) {
            cap *= 2;
            char* nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    if (!got_any) { free(buf); return NULL; }
    /* Trim trailing \r (mode texte Windows ou client qui en envoie). */
    while (len > 0 && buf[len - 1] == '\r') len--;
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* Sérialise un JsonValue et l'écrit sur stdout suivi d'un \n.
 * Flush immédiat — le client peut bloquer en attente sinon. */
static void send_message(JsonValue* msg) {
    int len = 0;
    char* str = json_serialize(msg, &len);
    if (!str) return;
    fwrite(str, 1, (size_t)len, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(str);
}

/* ----------------------------------------------------------------
 * Capture stdout + stderr d'un sous-appel CLI
 *
 * Le serveur MCP réutilise les handlers `cli_doc_*` qui impriment sur
 * stdout (output nominal) ET stderr (erreurs d'usage). Or stdout est le
 * canal protocole côté MCP. On redirige les DEUX vers un même fichier
 * temp le temps de l'appel — équivalent à `2>&1` en shell — puis on
 * restaure et on lit. Ainsi les messages d'erreur sont remontés au LLM
 * dans le content MCP, pas perdus dans le vide.
 *
 * Retourne une malloc'd string contenant l'output combiné (caller free)
 * et stocke le code retour dans *rc_out. NULL sur erreur d'I/O. */
static char* capture_cli(int (*fn)(int, char**), int argc, char** argv,
                         int* rc_out) {
    static const char* TMP = "_owl_mcp_capture.tmp";

    fflush(stdout); fflush(stderr);
    int saved_out = owl_dup(owl_fileno(stdout));
    int saved_err = owl_dup(owl_fileno(stderr));
    if (saved_out < 0 || saved_err < 0) {
        if (saved_out >= 0) owl_close(saved_out);
        if (saved_err >= 0) owl_close(saved_err);
        return NULL;
    }

    int tmp_fd = owl_open(TMP,
                          O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                          0644);
    if (tmp_fd < 0) {
        owl_close(saved_out); owl_close(saved_err);
        return NULL;
    }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0 ||
        owl_dup2(tmp_fd, owl_fileno(stderr)) < 0) {
        owl_close(tmp_fd);
        owl_dup2(saved_out, owl_fileno(stdout));
        owl_dup2(saved_err, owl_fileno(stderr));
        owl_close(saved_out); owl_close(saved_err);
        return NULL;
    }
    owl_close(tmp_fd);

    int rc = fn(argc, argv);
    fflush(stdout); fflush(stderr);

    /* Restaure les deux flux AVANT de lire le fichier. */
    owl_dup2(saved_out, owl_fileno(stdout));
    owl_dup2(saved_err, owl_fileno(stderr));
    owl_close(saved_out); owl_close(saved_err);

    FILE* r = fopen(TMP, "rb");
    char* buf = NULL;
    if (r) {
        fseek(r, 0, SEEK_END);
        long sz = ftell(r);
        fseek(r, 0, SEEK_SET);
        if (sz >= 0) {
            buf = (char*)malloc((size_t)sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, (size_t)sz, r);
                buf[n] = '\0';
            }
        }
        fclose(r);
    }
    owl_unlink(TMP);
    if (rc_out) *rc_out = rc;
    return buf;
}

/* ----------------------------------------------------------------
 * Helpers JSON-RPC
 * ---------------------------------------------------------------- */

/* Construit l'enveloppe `{"jsonrpc":"2.0","id":<id>,"result":<result>}`.
 * Transfère ownership de `result` au message — ne pas le libérer après. */
static JsonValue* make_response(JsonValue* id, JsonValue* result) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "jsonrpc", json_new_string("2.0"));
    if (id) json_object_set(msg, "id", id);
    json_object_set(msg, "result", result);
    return msg;
}

/* Construit l'enveloppe d'erreur `{"jsonrpc":"2.0","id":<id>,"error":{...}}`. */
static JsonValue* make_error(JsonValue* id, int code, const char* message) {
    JsonValue* msg = json_new_object();
    json_object_set(msg, "jsonrpc", json_new_string("2.0"));
    if (id) json_object_set(msg, "id", id);
    JsonValue* err = json_new_object();
    json_object_set(err, "code",    json_new_int(code));
    json_object_set(err, "message", json_new_string(message));
    json_object_set(msg, "error", err);
    return msg;
}

/* Récupère et duplique l'id JSON du message — peut être int ou string
 * selon le client. On le copie pour pouvoir le placer dans la réponse
 * sans toucher au message d'origine. NULL si absent (= notification). */
static JsonValue* clone_id(JsonValue* req) {
    JsonValue* id = json_get(req, "id");
    if (!id) return NULL;
    if (id->type == JSON_INT)    return json_new_int(id->as.intVal);
    if (id->type == JSON_STRING) return json_new_string(id->as.stringVal.str);
    /* MCP autorise id null, mais on a déjà NULL plus haut comme
     * signe de notification — on ne devrait pas tomber ici. */
    return json_new_null();
}

/* ----------------------------------------------------------------
 * Handlers
 * ---------------------------------------------------------------- */

static JsonValue* handle_initialize(JsonValue* id, JsonValue* params) {
    (void)params; /* TODO V1.1 : log la version client + ses capabilities */

    JsonValue* result = json_new_object();
    json_object_set(result, "protocolVersion",
                    json_new_string(OWL_MCP_PROTOCOL_VERSION));

    /* Capabilities : minimum pour l'étape 1. On annonce tools +
     * resources + prompts car le LLM doit savoir qu'il peut nous
     * appeler dessus, même si les listes sont vides pour l'instant. */
    JsonValue* caps = json_new_object();
    JsonValue* tools_cap = json_new_object();
    json_object_set(tools_cap, "listChanged", json_new_bool(false));
    json_object_set(caps, "tools", tools_cap);

    JsonValue* res_cap = json_new_object();
    json_object_set(res_cap, "subscribe",   json_new_bool(false));
    json_object_set(res_cap, "listChanged", json_new_bool(false));
    json_object_set(caps, "resources", res_cap);

    JsonValue* prompts_cap = json_new_object();
    json_object_set(prompts_cap, "listChanged", json_new_bool(false));
    json_object_set(caps, "prompts", prompts_cap);

    json_object_set(result, "capabilities", caps);

    JsonValue* info = json_new_object();
    json_object_set(info, "name",    json_new_string(OWL_MCP_SERVER_NAME));
    json_object_set(info, "version", json_new_string(OWL_MCP_SERVER_VERSION));
    json_object_set(result, "serverInfo", info);

    /* Périmètre du serveur : il sert à raisonner SUR le code/framework,
     * pas à l'exécuter. Cadre l'usage côté client (rejet implicite des
     * attentes type run/test via MCP). */
    json_object_set(result, "instructions", json_new_string(
        "Le serveur owl sert à comprendre, explorer et diagnostiquer le "
        "langage et le framework OWL : explain, search, examples, check, "
        "signature, limits, template. Il n'exécute pas de programme : pour "
        "lancer du code ou des tests, utiliser la CLI hors MCP "
        "(owl <fichier>, owl test)."));

    return make_response(id, result);
}

static JsonValue* handle_ping(JsonValue* id) {
    /* MCP : ping retourne un objet vide. */
    return make_response(id, json_new_object());
}

/* ----------------------------------------------------------------
 * Tools — table statique + dispatch
 * ---------------------------------------------------------------- */

/* Construit un block content text MCP, optionnellement marqué erreur. */
static JsonValue* tool_text_result(const char* text, int is_error) {
    JsonValue* result = json_new_object();
    JsonValue* content = json_new_array();
    JsonValue* block = json_new_object();
    json_object_set(block, "type", json_new_string("text"));
    json_object_set(block, "text", json_new_string(text ? text : ""));
    json_array_push(content, block);
    json_object_set(result, "content", content);
    if (is_error) json_object_set(result, "isError", json_new_bool(true));
    return result;
}

/* Petit helper pour construire une JSON Schema enum array depuis un
 * tableau de chaînes terminé par NULL. */
static JsonValue* schema_enum_strings(const char* const* values) {
    JsonValue* arr = json_new_array();
    for (int i = 0; values[i]; i++)
        json_array_push(arr, json_new_string(values[i]));
    return arr;
}

/* Ajoute la property `format` (enum text/json, defaut text) au noeud
 * `properties` d'un InputSchema. Mutualise sur les tools qui produisent
 * a la fois du texte humain et du JSON structure (explain/search/
 * examples/where). Defaut `text` car les LLMs digerent mieux la prose
 * formatee — le JSON est pour les agents qui parsent. */
static void schema_add_format_prop(JsonValue* props) {
    JsonValue* fmt = json_new_object();
    json_object_set(fmt, "type", json_new_string("string"));
    static const char* FORMAT_VALUES[] = { "text", "json", NULL };
    json_object_set(fmt, "enum", schema_enum_strings(FORMAT_VALUES));
    json_object_set(fmt, "description",
        json_new_string("Forme de la sortie. 'text' (defaut) = prose "
                        "lisible avec couleurs ANSI desactivees. 'json' "
                        "= objet structure (schema stable, consommable "
                        "par un agent qui parse). Le bloc retourne reste "
                        "type text/plain ; en mode 'json' il contient du "
                        "JSON valide."));
    json_object_set(props, "format", fmt);
}

/* Renvoie 1 si le client a demande format='json', 0 sinon. Tolere
 * variantes de casse — le client MCP peut envoyer 'JSON' ou 'Json'. */
static int args_want_json(JsonValue* arguments) {
    const char* fmt = json_get_string(arguments, "format");
    if (!fmt || !*fmt) return 0;
    return (fmt[0] == 'j' || fmt[0] == 'J') &&
           (fmt[1] == 's' || fmt[1] == 'S') &&
           (fmt[2] == 'o' || fmt[2] == 'O') &&
           (fmt[3] == 'n' || fmt[3] == 'N') && fmt[4] == '\0';
}

/* Drapeau de mise en forme a injecter dans l'argv CLI selon le mode. */
static const char* format_flag(int want_json) {
    return want_json ? "--json" : "--plain";
}

/* InputSchema de owl_explain. Aligné sur la sémantique de la sous-commande
 * `owl explain` côté CLI : un nom de symbole obligatoire, un filtre `cat`
 * optionnel par bucket, une langue optionnelle. */
static JsonValue* schema_owl_explain(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));

    JsonValue* props = json_new_object();

    JsonValue* sym = json_new_object();
    json_object_set(sym, "type", json_new_string("string"));
    json_object_set(sym, "description",
        json_new_string("Nom du symbole OWL à expliquer (fonction, type, "
                        "énumération, constante, type primitif, ou code "
                        "d'erreur E_*). Casse et accents insensibles."));
    json_object_set(props, "symbol", sym);

    JsonValue* cat = json_new_object();
    json_object_set(cat, "type", json_new_string("string"));
    static const char* CAT_VALUES[] = {
        "all", "primitives", "types", "natives", "enums", "constants",
        "errors", NULL
    };
    json_object_set(cat, "enum", schema_enum_strings(CAT_VALUES));
    json_object_set(cat, "description",
        json_new_string("Filtre par bucket sémantique. 'all' (défaut) renvoie "
                        "toutes les correspondances ; 'types' couvre primitifs "
                        "+ composés."));
    json_object_set(props, "cat", cat);

    JsonValue* lang = json_new_object();
    json_object_set(lang, "type", json_new_string("string"));
    static const char* LANG_VALUES[] = { "fr", "en", NULL };
    json_object_set(lang, "enum", schema_enum_strings(LANG_VALUES));
    json_object_set(lang, "description",
        json_new_string("Langue de la description ('fr' par défaut)."));
    json_object_set(props, "lang", lang);

    schema_add_format_prop(props);

    json_object_set(schema, "properties", props);

    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("symbol"));
    json_object_set(schema, "required", required);

    return schema;
}

/* Handler owl_explain : capture la sortie de `cli_doc_explain` et la
 * renvoie en bloc texte MCP. Code retour CLI != 0 → marque isError.
 * En mode format='json', le bloc texte contient du JSON structure. */
static JsonValue* handler_owl_explain(JsonValue* arguments) {
    const char* symbol = json_get_string(arguments, "symbol");
    if (!symbol || !*symbol) {
        return tool_text_result(
            "owl_explain: argument 'symbol' manquant ou vide.", 1);
    }
    const char* cat  = json_get_string(arguments, "cat");
    const char* lang = json_get_string(arguments, "lang");
    int want_json = args_want_json(arguments);

    /* Construit l'argv de la sous-commande CLI. --plain (defaut) force
     * la sortie sans ANSI — le canal MCP n'est pas un terminal. --json
     * bascule sur le schema structure quand le client le demande. */
    char* cli_argv[16];
    int   cli_argc = 0;
    cli_argv[cli_argc++] = "explain";
    cli_argv[cli_argc++] = (char*)symbol;
    if (cat && *cat) {
        cli_argv[cli_argc++] = "--cat";
        cli_argv[cli_argc++] = (char*)cat;
    }
    if (lang && *lang) {
        cli_argv[cli_argc++] = "--lang";
        cli_argv[cli_argc++] = (char*)lang;
    }
    cli_argv[cli_argc++] = (char*)format_flag(want_json);

    int rc = 0;
    char* out = capture_cli(cli_doc_explain, cli_argc, cli_argv, &rc);
    if (!out) {
        return tool_text_result(
            "owl_explain: erreur interne (capture stdout échouée).", 1);
    }
    /* rc=0 → succès, !=0 → erreur applicative (symbole inconnu, flag
     * invalide…). On renvoie quand même l'output qui contient le message
     * d'erreur de la CLI, et on flag isError. */
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_search — recherche transversale (public docs + registry + scénarios)
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_search(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* q = json_new_object();
    json_object_set(q, "type", json_new_string("string"));
    json_object_set(q, "description",
        json_new_string("Mot-clé à rechercher. Insensible casse, "
                        "matche dans les noms, descriptions et corps "
                        "de la doc publique, du registry et des scénarios."));
    json_object_set(props, "query", q);
    schema_add_format_prop(props);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("query"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_search(JsonValue* arguments) {
    const char* query = json_get_string(arguments, "query");
    if (!query || !*query) {
        return tool_text_result("owl_search: argument 'query' manquant.", 1);
    }
    int want_json = args_want_json(arguments);
    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "search";
    cli_argv[cli_argc++] = (char*)query;
    cli_argv[cli_argc++] = (char*)format_flag(want_json);
    int rc = 0;
    char* out = capture_cli(cli_doc_search, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_search: capture I/O échouée.", 1);
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_examples — scénarios canoniques démontrant un symbole
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_examples(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* sym = json_new_object();
    json_object_set(sym, "type", json_new_string("string"));
    json_object_set(sym, "description",
        json_new_string("Nom du symbole dont on veut les scénarios publics. "
                        "Casse et accents insensibles."));
    json_object_set(props, "symbol", sym);
    schema_add_format_prop(props);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("symbol"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_examples(JsonValue* arguments) {
    const char* symbol = json_get_string(arguments, "symbol");
    if (!symbol || !*symbol) {
        return tool_text_result("owl_examples: argument 'symbol' manquant.", 1);
    }
    int want_json = args_want_json(arguments);
    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "examples";
    cli_argv[cli_argc++] = (char*)symbol;
    cli_argv[cli_argc++] = (char*)format_flag(want_json);
    int rc = 0;
    char* out = capture_cli(cli_doc_examples, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_examples: capture I/O échouée.", 1);
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_where — reverse lookup (qui m'utilise ?)
 *
 * Pendant logique d'owl_explain : explain dit « que SUIS-JE ? »,
 * uses dit « qui M'UTILISE ? ». Pour les propriétés (Grisé, Visible)
 * → widgets utilisateurs. Pour les widgets (Bouton, Corps) → contexte
 * parent où ils peuvent être déclarés.
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_where(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* sym = json_new_object();
    json_object_set(sym, "type", json_new_string("string"));
    json_object_set(sym, "description",
        json_new_string("Symbole dont on veut le reverse lookup. "
                        "Propriété DSL (Grisé, Visible, Titre…) → "
                        "liste des champs qui l'acceptent. "
                        "Type de champ (Bouton, Corps, Fenêtre…) → "
                        "contexte parent. Préfixe `..` accepté."));
    json_object_set(props, "symbol", sym);
    schema_add_format_prop(props);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("symbol"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_where(JsonValue* arguments) {
    const char* symbol = json_get_string(arguments, "symbol");
    if (!symbol || !*symbol) {
        return tool_text_result("owl_where: argument 'symbol' manquant.", 1);
    }
    int want_json = args_want_json(arguments);
    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "where";
    cli_argv[cli_argc++] = (char*)symbol;
    cli_argv[cli_argc++] = (char*)format_flag(want_json);
    int rc = 0;
    char* out = capture_cli(cli_doc_where, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_where: capture I/O échouée.", 1);
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_signature — version token-light de explain (juste les signatures)
 *
 * Le LLM qui rédige du code n'a pas besoin de la description complète à
 * chaque appel — juste la forme `Nom(arg : type, ...) → retour`. Ce
 * tool court permet d'enchaîner plusieurs lookups sans saturer le
 * contexte. Format identique à la section Signatures de owl_explain
 * mais sans le reste de la fiche.
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_signature(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* sym = json_new_object();
    json_object_set(sym, "type", json_new_string("string"));
    json_object_set(sym, "description",
        json_new_string("Nom de la fonction native dont on veut la "
                        "signature. Casse et accents insensibles."));
    json_object_set(props, "symbol", sym);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("symbol"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_signature(JsonValue* arguments) {
    const char* symbol = json_get_string(arguments, "symbol");
    if (!symbol || !*symbol) {
        return tool_text_result("owl_signature: argument 'symbol' manquant.", 1);
    }
    const OwlNativeDef* d = owl_registry_find(symbol);
    if (!d) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "owl_signature: '%s' n'est pas une fonction native. "
                 "Essayez owl_explain pour les types, enums ou constantes.",
                 symbol);
        return tool_text_result(msg, 1);
    }
    if (!d->signatures) {
        char msg[256];
        snprintf(msg, sizeof msg,
                 "owl_signature: '%s' n'expose pas de signatures dans le "
                 "registry (cf. owl_explain pour la description).",
                 d->canonical);
        return tool_text_result(msg, 1);
    }

    /* Formatage compact :
     *   Nom(arg1 : type1, arg2 : type2, ...) → returnType
     *   Nom(autre_variante : ...)            → ...
     * Une ligne par signature. */
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return tool_text_result("owl_signature: OOM.", 1);
    buf[0] = '\0';
    #define APPEND(fmt, ...) do { \
        size_t avail = cap - len; \
        int n = snprintf(buf + len, avail, fmt, ##__VA_ARGS__); \
        if (n < 0) break; \
        if ((size_t)n >= avail) { \
            cap = (len + (size_t)n + 1) * 2; \
            char* nb = (char*)realloc(buf, cap); \
            if (!nb) break; buf = nb; \
            n = snprintf(buf + len, cap - len, fmt, ##__VA_ARGS__); \
            if (n < 0) break; \
        } \
        len += (size_t)n; \
    } while (0)

    for (const OwlSignature* s = d->signatures;
         !(s->arity == 0 && s->params == NULL &&
           s->returnType == NULL && s->returnDesc == NULL);
         s++) {
        APPEND("%s(", d->canonical);
        int p_count = 0;
        if (s->params) {
            /*  : borner a l'arite (prefixe partage). Variadique
             * (arity<0) : pas de borne, le marqueur `...` suit. */
            for (const OwlParam* p = s->params;
                 p->name && (s->arity < 0 || (p - s->params) < s->arity);
                 p++) {
                if (p_count > 0) APPEND(", ");
                APPEND("%s : %s", p->name, p->type ? p->type : "tout");
                p_count++;
            }
        }
        /* arity < 0 = variadic : marqueur `...` (cf. cli_doc.c). Sans
         * codes couleur ici — la sortie MCP est texte brut. */
        if (s->arity < 0) {
            if (p_count > 0) APPEND(", ");
            APPEND("...");
        }
        APPEND(")");
        if (s->returnType && *s->returnType) {
            APPEND(" → %s", s->returnType);
        }
        APPEND("\n");
    }
    #undef APPEND

    JsonValue* result = tool_text_result(buf, 0);
    free(buf);
    return result;
}

/* ----------------------------------------------------------------
 * owl_check — diagnostic compile-only
 *
 * Le LLM envoie du source OWL, on retourne un JSON
 * `{errors: [...], warnings: [...]}` strict. Aucune exécution, juste
 * compile + capture des diagnostics. Pattern temp-file identique à
 * owl_format pour réutiliser `handle_check_command` qui ne sait
 * compile qu'un path.
 * ---------------------------------------------------------------- */

/* Variante qui suppose vm_init() déjà fait — le serveur MCP partage un
 * seul VM/registry sur toute la durée de la session, on ne peut pas le
 * détruire entre deux appels (ce que ferait le wrapper standard).
 * isolated = compile single-file sans scan du dossier parent (snippet
 * inline du LLM = pas de cross-file légitime, et le scan parent serait
 * catastrophique si le serveur tourne depuis la racine d'un gros repo). */
extern int handle_check_command_isolated(const char* filepath);

static JsonValue* schema_owl_check(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* code = json_new_object();
    json_object_set(code, "type", json_new_string("string"));
    json_object_set(code, "description",
        json_new_string("Source OWL à valider. Retourne un JSON "
                        "`{errors:[...], warnings:[...]}` — chaque "
                        "diagnostic a `line`, `column`, `message`."));
    json_object_set(props, "code", code);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("code"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_check(JsonValue* arguments) {
    const char* code = json_get_string(arguments, "code");
    if (!code) {
        return tool_text_result("owl_check: argument 'code' manquant.", 1);
    }
    /* Snippet inline ecrit dans un repertoire prive du tmp systeme (pas le
     * cwd, pas la racine temporaire) : le fichier ne doit avoir aucun
     * voisin, sans quoi le perimetre resolu par dossier ramasse les .owl
     * des autres processus. */
    char tmp_src[1024];
    char tmp_out[1024];
    int slot = mcp_scratch_open("mcp_check", "snippet.owl",
                                tmp_src, sizeof tmp_src);
    if (slot < 0) return tool_text_result("owl_check: tmp dir creation failed.", 1);
    snprintf(tmp_out, sizeof tmp_out, "%s.out", tmp_src);

    FILE* w = fopen(tmp_src, "wb");
    if (!w) {
        mcp_scratch_close(slot);
        return tool_text_result("owl_check: tmp file open failed.", 1);
    }
    fwrite(code, 1, strlen(code), w);
    fclose(w);

    /* Capture manuelle inline — handle_check_command_isolated n'a pas
     * la signature (int argc, char**) attendue par capture_cli. On
     * duplique le squelette de redirection. */
    fflush(stdout); fflush(stderr);
    int saved_out = owl_dup(owl_fileno(stdout));
    int saved_err = owl_dup(owl_fileno(stderr));
    int tmp_fd    = owl_open(tmp_out,
                             O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (saved_out < 0 || saved_err < 0 || tmp_fd < 0) {
        if (saved_out >= 0) owl_close(saved_out);
        if (saved_err >= 0) owl_close(saved_err);
        if (tmp_fd    >= 0) owl_close(tmp_fd);
        mcp_scratch_close(slot);
        return tool_text_result("owl_check: capture init failed.", 1);
    }
    owl_dup2(tmp_fd, owl_fileno(stdout));
    owl_dup2(tmp_fd, owl_fileno(stderr));
    owl_close(tmp_fd);

    int check_rc = handle_check_command_isolated(tmp_src);
    fflush(stdout); fflush(stderr);

    owl_dup2(saved_out, owl_fileno(stdout));
    owl_dup2(saved_err, owl_fileno(stderr));
    owl_close(saved_out); owl_close(saved_err);

    FILE* r = fopen(tmp_out, "rb");
    char* output = NULL;
    if (r) {
        fseek(r, 0, SEEK_END);
        long sz = ftell(r);
        fseek(r, 0, SEEK_SET);
        if (sz >= 0) {
            output = (char*)malloc((size_t)sz + 1);
            if (output) {
                size_t n = fread(output, 1, (size_t)sz, r);
                output[n] = '\0';
            }
        }
        fclose(r);
    }
    mcp_scratch_close(slot);

    if (!output) {
        return tool_text_result("owl_check: relecture output échouée.", 1);
    }
    /* check_rc=0 si pas d'erreurs, 65 sinon. On retourne le JSON dans
     * tous les cas (le LLM le parse pour voir errors/warnings) et on
     * flag isError seulement si le compile a refusé. */
    JsonValue* result = tool_text_result(output, check_rc != 0);
    free(output);
    return result;
}

/* ----------------------------------------------------------------
 * owl_format — reformate du code OWL avec le canon
 *
 * Le LLM envoie son source brut, on retourne la version reformattée.
 * Pour réutiliser `owl_format_file` qui prend un path, on écrit dans
 * un fichier temp, on formate en place, on relit. Pas de spawn de
 * sous-processus.
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_format(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();
    JsonValue* code = json_new_object();
    json_object_set(code, "type", json_new_string("string"));
    json_object_set(code, "description",
        json_new_string("Source OWL brut à reformater. Retourne le code "
                        "avec casse, accents et indentation canon."));
    json_object_set(props, "code", code);
    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("code"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_format(JsonValue* arguments) {
    const char* code = json_get_string(arguments, "code");
    if (!code) {
        return tool_text_result("owl_format: argument 'code' manquant.", 1);
    }
    /* Repertoire prive : le formateur lit le contexte voisin du fichier
     * qu'on lui donne, donc un snippet pose a la racine temporaire se
     * ferait contextualiser par les .owl des autres processus. */
    char tmp_src[1024];
    int slot = mcp_scratch_open("mcp_fmt", "snippet.owl",
                                tmp_src, sizeof tmp_src);
    if (slot < 0) return tool_text_result("owl_format: tmp dir creation failed.", 1);

    FILE* w = fopen(tmp_src, "wb");
    if (!w) {
        mcp_scratch_close(slot);
        return tool_text_result("owl_format: tmp file write open failed.", 1);
    }
    fwrite(code, 1, strlen(code), w);
    fclose(w);

    int rc = owl_format_file(tmp_src, /*dryRun*/ false);
    /* owl_format_file : 0 = unchanged, 1 = formatted, 2 = would change
     * (dryRun seulement), <0 = error. */
    if (rc < 0) {
        mcp_scratch_close(slot);
        return tool_text_result(
            "owl_format: erreur de parsing — code malformé ?\n"
            "Conseil : passe par owl_check pour le diagnostic détaillé.", 1);
    }

    FILE* r = fopen(tmp_src, "rb");
    char* out = NULL;
    if (r) {
        fseek(r, 0, SEEK_END);
        long sz = ftell(r);
        fseek(r, 0, SEEK_SET);
        if (sz >= 0) {
            out = (char*)malloc((size_t)sz + 1);
            if (out) {
                size_t n = fread(out, 1, (size_t)sz, r);
                out[n] = '\0';
            }
        }
        fclose(r);
    }
    mcp_scratch_close(slot);

    if (!out) return tool_text_result("owl_format: relecture du tmp échouée.", 1);
    JsonValue* result = tool_text_result(out, 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_translate — réécrit la surface langage dans l'autre langue
 *
 * Le LLM envoie du source OWL + une langue cible ("en" / "fr"), on
 * retourne le même code avec mots-clés, natives, types et propriétés
 * builtin forcés dans la langue demandée. Identifiants utilisateur,
 * chaînes et commentaires restent intacts. Réutilise le moteur
 * owl_translate du formatter (string -> string, pas de fichier temp).
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_translate(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();

    JsonValue* code = json_new_object();
    json_object_set(code, "type", json_new_string("string"));
    json_object_set(code, "description",
        json_new_string("Source OWL à traduire. La surface langage "
                        "(mots-clés, natives, types, propriétés builtin) "
                        "est réécrite dans la langue cible ; identifiants, "
                        "chaînes et commentaires restent inchangés."));
    json_object_set(props, "code", code);

    JsonValue* to = json_new_object();
    json_object_set(to, "type", json_new_string("string"));
    json_object_set(to, "description",
        json_new_string("Langue cible : \"en\" (anglais) ou \"fr\" "
                        "(français)."));
    json_object_set(props, "to", to);

    json_object_set(schema, "properties", props);
    JsonValue* required = json_new_array();
    json_array_push(required, json_new_string("code"));
    json_array_push(required, json_new_string("to"));
    json_object_set(schema, "required", required);
    return schema;
}

static JsonValue* handler_owl_translate(JsonValue* arguments) {
    const char* code = json_get_string(arguments, "code");
    if (!code) {
        return tool_text_result("owl_translate: argument 'code' manquant.", 1);
    }
    const char* to = json_get_string(arguments, "to");
    if (!to) {
        return tool_text_result(
            "owl_translate: argument 'to' manquant (attendu \"en\" ou \"fr\").",
            1);
    }
    OwlTranslateLang target;
    if (strcmp(to, "en") == 0) {
        target = OWL_TRANSLATE_EN;
    } else if (strcmp(to, "fr") == 0) {
        target = OWL_TRANSLATE_FR;
    } else {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "owl_translate: 'to' invalide ('%s') — attendu \"en\" "
                 "ou \"fr\".", to);
        return tool_text_result(msg, 1);
    }

    char* out = owl_translate(code, target);
    if (!out) {
        return tool_text_result(
            "owl_translate: échec de la traduction (source non parsable ?).",
            1);
    }
    JsonValue* result = tool_text_result(out, 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_template — squelettes prêts à l'emploi
 *
 * Sans `name` : liste les templates dispo + niveau + description.
 * Avec `name` : retourne le code prêt à utiliser. Le user (LLM ou
 * humain) le colle dans un fichier et customise.
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_template(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();

    JsonValue* nm = json_new_object();
    json_object_set(nm, "type", json_new_string("string"));
    json_object_set(nm, "description",
        json_new_string("Nom du template à récupérer (ex: hello, classe, "
                        "test, fenetre). Si omis, retourne la liste des "
                        "templates disponibles avec niveau + description."));
    json_object_set(props, "name", nm);

    schema_add_format_prop(props);

    json_object_set(schema, "properties", props);
    /* name optionnel — absent = mode liste. */
    return schema;
}

static JsonValue* handler_owl_template(JsonValue* arguments) {
    const char* name = json_get_string(arguments, "name");
    int want_json = args_want_json(arguments);

    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "template";
    if (name && *name) cli_argv[cli_argc++] = (char*)name;
    cli_argv[cli_argc++] = (char*)format_flag(want_json);

    int rc = 0;
    char* out = capture_cli(cli_doc_template, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_template: capture I/O échouée.", 1);
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_new — squelettes génériques minimaux (fenetre, classe, champ).
 * Sortie validée par self-test rule 37 — compile sans erreur.
 * ---------------------------------------------------------------- */
static JsonValue* schema_owl_new(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();

    JsonValue* k = json_new_object();
    json_object_set(k, "type", json_new_string("string"));
    json_object_set(k, "description", json_new_string(
        "Type de squelette : fenetre, etat, classe, collection, theme, "
        "service, ou champ (auquel cas 'sub' est requis)."));
    JsonValue* k_enum = json_new_array();
    json_array_push(k_enum, json_new_string("fenetre"));
    json_array_push(k_enum, json_new_string("etat"));
    json_array_push(k_enum, json_new_string("classe"));
    json_array_push(k_enum, json_new_string("collection"));
    json_array_push(k_enum, json_new_string("theme"));
    json_array_push(k_enum, json_new_string("service"));
    json_array_push(k_enum, json_new_string("champ"));
    json_object_set(k, "enum", k_enum);
    json_object_set(props, "kind", k);

    JsonValue* s = json_new_object();
    json_object_set(s, "type", json_new_string("string"));
    json_object_set(s, "description", json_new_string(
        "Sous-kind quand kind=champ : bouton | libelle | saisie | table | "
        "conteneur | image | liste | combo | selecteur | interrupteur | "
        "zonerepetee."));
    json_object_set(props, "sub", s);

    JsonValue* n = json_new_object();
    json_object_set(n, "type", json_new_string("string"));
    json_object_set(n, "description", json_new_string(
        "Nom du champ/classe à utiliser dans le squelette (ex: BTN_OK, "
        "MaFen, cPersonne)."));
    json_object_set(props, "name", n);

    json_object_set(schema, "properties", props);
    JsonValue* req = json_new_array();
    json_array_push(req, json_new_string("kind"));
    json_array_push(req, json_new_string("name"));
    json_object_set(schema, "required", req);
    return schema;
}

static JsonValue* handler_owl_new(JsonValue* arguments) {
    const char* kind = json_get_string(arguments, "kind");
    const char* sub  = json_get_string(arguments, "sub");
    const char* name = json_get_string(arguments, "name");

    if (!kind || !*kind)
        return tool_text_result("owl_new: 'kind' manquant.", 1);
    if (!name || !*name)
        return tool_text_result("owl_new: 'name' manquant.", 1);
    if (sub && !*sub) sub = NULL;  /* "" équivaut à NULL */

    FILE* f = tmpfile();
    if (!f) return tool_text_result("owl_new: tmpfile failed.", 1);

    int rc = owl_new_generate(kind, sub, name, f);
    if (rc != 0) {
        fclose(f);
        char msg[256];
        if (sub) snprintf(msg, sizeof msg,
            "owl_new: combinaison '%s/%s' inconnue.", kind, sub);
        else snprintf(msg, sizeof msg,
            "owl_new: type '%s' inconnu (attendu : fenetre, classe, champ).", kind);
        return tool_text_result(msg, 1);
    }

    long size = ftell(f);
    if (size <= 0) { fclose(f); return tool_text_result("owl_new: empty output.", 1); }
    rewind(f);
    char* out = (char*)malloc((size_t)size + 1);
    if (!out) { fclose(f); return tool_text_result("owl_new: alloc failed.", 1); }
    size_t got = fread(out, 1, (size_t)size, f);
    out[got] = '\0';
    fclose(f);

    JsonValue* result = tool_text_result(out, 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * owl_limits — registry des limites compile-time du compilateur
 * ---------------------------------------------------------------- */

static JsonValue* schema_owl_limits(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();

    JsonValue* nm = json_new_object();
    json_object_set(nm, "type", json_new_string("string"));
    json_object_set(nm, "description",
        json_new_string("Optionnel. Si fourni, retourne la fiche detaillee de "
                        "cette limite (ex: 'MAX_USER_TYPES'). Sinon, retourne "
                        "la liste complete. Case-insensitive."));
    json_object_set(props, "name", nm);

    JsonValue* cat = json_new_object();
    json_object_set(cat, "type", json_new_string("string"));
    json_object_set(cat, "description",
        json_new_string("Optionnel. Filtre par categorie exacte (ex: "
                        "'Compiler/Types', 'Compiler/POO'). Ignore si 'name' "
                        "est fourni."));
    json_object_set(props, "category", cat);

    JsonValue* lang = json_new_object();
    json_object_set(lang, "type", json_new_string("string"));
    static const char* LANG_VALUES[] = { "fr", "en", NULL };
    json_object_set(lang, "enum", schema_enum_strings(LANG_VALUES));
    json_object_set(lang, "description",
        json_new_string("Langue de la description ('fr' par defaut)."));
    json_object_set(props, "lang", lang);

    schema_add_format_prop(props);
    json_object_set(schema, "properties", props);
    /* Pas de champ required : tout est optionnel. */
    return schema;
}

static JsonValue* handler_owl_limits(JsonValue* arguments) {
    const char* name = json_get_string(arguments, "name");
    const char* cat  = json_get_string(arguments, "category");
    const char* lang = json_get_string(arguments, "lang");
    int want_json = args_want_json(arguments);

    /* Mode lookup direct : `name` -> on delegue a `cli_doc_explain` qui
     * gere le bucket LIMIT proprement (formatage texte ou JSON). */
    if (name && *name) {
        char* cli_argv[8];
        int cli_argc = 0;
        cli_argv[cli_argc++] = "explain";
        cli_argv[cli_argc++] = (char*)name;
        cli_argv[cli_argc++] = "--cat";
        cli_argv[cli_argc++] = "limits";
        if (lang && *lang) {
            cli_argv[cli_argc++] = "--lang";
            cli_argv[cli_argc++] = (char*)lang;
        }
        cli_argv[cli_argc++] = (char*)format_flag(want_json);
        int rc = 0;
        char* out = capture_cli(cli_doc_explain, cli_argc, cli_argv, &rc);
        if (!out) return tool_text_result("owl_limits: capture I/O echouee.", 1);
        JsonValue* result = tool_text_result(out, rc != 0);
        free(out);
        return result;
    }

    /* Mode liste : delegue a `cli_doc_list limits` (avec --cat eventuel). */
    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "list";
    cli_argv[cli_argc++] = "limits";
    if (cat && *cat) {
        cli_argv[cli_argc++] = "--cat";
        cli_argv[cli_argc++] = (char*)cat;
    }
    if (lang && *lang) {
        cli_argv[cli_argc++] = "--lang";
        cli_argv[cli_argc++] = (char*)lang;
    }
    cli_argv[cli_argc++] = (char*)format_flag(want_json);
    int rc = 0;
    char* out = capture_cli(cli_doc_list, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_limits: capture I/O echouee.", 1);
    JsonValue* result = tool_text_result(out, rc != 0);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * Familles de catalogue — source unique
 * ----------------------------------------------------------------
 * Une seule table décide quelles familles du catalogue existent. Elle
 * alimente l'outil `owl_list` (enum du schéma + message d'erreur), la
 * ressource `owl://catalog/<famille>` (acceptation à la lecture + message
 * d'erreur) et les entrées annoncées par `resources/list`. Les trois
 * surfaces énuméraient chacune sa propre copie ; elles divergeaient dès
 * le premier ajout.
 *
 * Les noms sont ceux que `cli_doc_list` dispatche réellement (cf.
 * src/cli_doc.c) — pas ceux de la table de complétion LIST_CATS. Les
 * énumérations n'en font pas partie : concept interne, plus une famille
 * navigable. Leurs fiches restent servies par owl://enum/{name}.
 *
 * `title` et `desc` ne servent qu'à resources/list (desc facultative). */

typedef struct {
    const char* name;
    const char* title;
    const char* desc;
} CatalogFamily;

static const CatalogFamily CATALOG_FAMILIES[] = {
    { "categories", "Index des catégories de fonctions natives",
      "Familles fonctionnelles (UI, Chaines, Dates, SQL, HTTP, …) avec "
      "le nombre de fonctions de chacune. Point d'entrée vers "
      "owl://category/{name}." },
    { "natives", "Catalogue des fonctions natives",
      "Liste plate : tous les noms canon + catégorie + description courte." },
    { "types", "Catalogue des types",
      "Types primitifs intégrés à la VM + types composés du registry." },
    { "constants", "Catalogue des constantes nommées", NULL },
    { "keywords", "Catalogue des mots-clés du langage",
      "Mots-clés réservés (SI, POUR, SELON, CLASSE, …) avec catégorie sémantique." },
    { "operators", "Catalogue des opérateurs symboliques",
      "Opérateurs symboliques (?., ?\?=, [~~, ~=, <-, …) avec sens et catégorie." },
    { "controls", "Catalogue des champs DSL",
      "12 champs DSL (Fenêtre, Bouton, Saisie, Table, …) + sous-blocs État." },
    { "events", "Catalogue des events DSL",
      "Events lifecycle par champ (surClic, surValidation, …) + events État." },
    { "props", "Catalogue des propriétés DSL",
      "Propriétés DSL groupées par champ (Largeur, Visible, Grisé, …)." },
    { "globalvars", "Catalogue des variables globales prédéfinies",
      "Variables globales prédéfinies du langage (ErreurDétectée, …)." },
    { "limits", "Catalogue des limites du compilateur",
      "Plafonds structurels du langage (nombre de types, de paramètres, "
      "profondeur d'imbrication, …) avec leur valeur. Fiche détaillée "
      "d'une limite par son nom : outil owl_limits." },
    { NULL, NULL, NULL }
};

static int list_bucket_is_known(const char* bucket) {
    for (int i = 0; CATALOG_FAMILIES[i].name; i++)
        if (strcmp(CATALOG_FAMILIES[i].name, bucket) == 0) return 1;
    return 0;
}

/* Énumère les familles acceptées à la suite d'un préfixe déjà écrit dans
 * `buf`. Partagé par les messages d'erreur de l'outil et de la ressource :
 * le client doit pouvoir corriger son appel sans deviner, et la liste
 * qu'il lit est celle qui décide réellement. */
static void catalog_families_append(char* buf, size_t cap, size_t n) {
    for (int i = 0; CATALOG_FAMILIES[i].name && n < cap; i++)
        n += (size_t)snprintf(buf + n, cap - n, "%s %s",
                              i ? "," : "", CATALOG_FAMILIES[i].name);
    if (n < cap) snprintf(buf + n, cap - n, ".");
}

static void list_buckets_message(char* buf, size_t cap, const char* got) {
    size_t n;
    if (got && *got)
        n = (size_t)snprintf(buf, cap,
            "owl_list : bucket '%s' inconnu. Valeurs acceptees :", got);
    else
        n = (size_t)snprintf(buf, cap,
            "owl_list : argument 'bucket' manquant. Valeurs acceptees :");
    catalog_families_append(buf, cap, n);
}

static JsonValue* schema_catalog_families(void) {
    JsonValue* arr = json_new_array();
    for (int i = 0; CATALOG_FAMILIES[i].name; i++)
        json_array_push(arr, json_new_string(CATALOG_FAMILIES[i].name));
    return arr;
}

/* ----------------------------------------------------------------
 * owl_list — catalogue par bucket, filtrable par catégorie
 * ----------------------------------------------------------------
 * Miroir de la sous-commande CLI `owl list <bucket> [--cat X]`. */

static JsonValue* schema_owl_list(void) {
    JsonValue* schema = json_new_object();
    json_object_set(schema, "type", json_new_string("object"));
    JsonValue* props = json_new_object();

    JsonValue* bucket = json_new_object();
    json_object_set(bucket, "type", json_new_string("string"));
    json_object_set(bucket, "enum", schema_catalog_families());
    json_object_set(bucket, "description", json_new_string(
        "Famille à énumérer. 'categories' rend l'index des familles "
        "fonctionnelles de fonctions natives (nom + nombre de fonctions) : "
        "c'est le point d'entrée pour cibler ensuite bucket='natives' "
        "avec 'cat'."));
    json_object_set(props, "bucket", bucket);

    JsonValue* cat = json_new_object();
    json_object_set(cat, "type", json_new_string("string"));
    json_object_set(cat, "description", json_new_string(
        "Optionnel. Restreint la liste à une catégorie (ex. 'UI', "
        "'Chaines', 'Dates', 'SQL', ou leur libellé anglais). Insensible "
        "à la casse et aux accents. Sur bucket='natives', c'est le filtre "
        "qui évite de rapatrier les centaines de fonctions du catalogue "
        "complet. Toutes les familles ne sont pas découpées en catégories : "
        "sur celles qui ne le sont pas, l'argument est refusé plutôt "
        "qu'ignoré, et la réponse énumère celles qui l'acceptent."));
    json_object_set(props, "cat", cat);

    JsonValue* lang = json_new_object();
    json_object_set(lang, "type", json_new_string("string"));
    static const char* LANG_VALUES[] = { "fr", "en", NULL };
    json_object_set(lang, "enum", schema_enum_strings(LANG_VALUES));
    json_object_set(lang, "description",
        json_new_string("Langue des libellés et descriptions "
                        "('fr' par défaut)."));
    json_object_set(props, "lang", lang);

    schema_add_format_prop(props);
    json_object_set(schema, "properties", props);
    /* `bucket` requis : un défaut quelconque rapatrierait un catalogue
     * entier (~85 Ko sur natives) que personne n'a demandé. */
    JsonValue* req = json_new_array();
    json_array_push(req, json_new_string("bucket"));
    json_object_set(schema, "required", req);
    return schema;
}

static JsonValue* handler_owl_list(JsonValue* arguments) {
    const char* bucket = json_get_string(arguments, "bucket");
    const char* cat    = json_get_string(arguments, "cat");
    const char* lang   = json_get_string(arguments, "lang");
    int want_json = args_want_json(arguments);

    if (!bucket || !*bucket || !list_bucket_is_known(bucket)) {
        char msg[512];
        list_buckets_message(msg, sizeof msg, bucket);
        return tool_text_result(msg, 1);
    }

    char* cli_argv[8];
    int cli_argc = 0;
    cli_argv[cli_argc++] = "list";
    cli_argv[cli_argc++] = (char*)bucket;
    if (cat && *cat) {
        cli_argv[cli_argc++] = "--cat";
        cli_argv[cli_argc++] = (char*)cat;
    }
    if (lang && *lang) {
        cli_argv[cli_argc++] = "--lang";
        cli_argv[cli_argc++] = (char*)lang;
    }
    cli_argv[cli_argc++] = (char*)format_flag(want_json);
    int rc = 0;
    char* out = capture_cli(cli_doc_list, cli_argc, cli_argv, &rc);
    if (!out) return tool_text_result("owl_list: capture I/O echouee.", 1);
    /* `owl list` répond rc=0 sur un nom de famille hors domaine : la commande
     * a répondu, elle a imprimé son diagnostic. Un canal machine a besoin du
     * verdict, pas de la phrase — il interroge le prédicat de cli_doc.c. */
    int is_error = (rc != 0) ||
                   !cli_doc_list_category_is_known(bucket, cat);
    JsonValue* result = tool_text_result(out, is_error);
    free(out);
    return result;
}

/* ----------------------------------------------------------------
 * Table des tools exposés
 * ----------------------------------------------------------------
 * La conversion en JSON pour `tools/list` se fait à la volée — pas de
 * cache, c'est appelé une fois par session client. */
typedef struct {
    const char* name;
    const char* description;
    JsonValue*  (*schema_builder)(void);
    JsonValue*  (*handler)(JsonValue* arguments);
} McpTool;

static const McpTool TOOLS[] = {
    {
        "owl_explain",
        "Fiche complète d'un symbole OWL : description, signatures, "
        "propriétés, énumérations, scénarios canoniques. Multi-bucket — "
        "un même nom peut désigner à la fois un type et une fonction "
        "homonyme, les deux sont retournés.",
        schema_owl_explain,
        handler_owl_explain
    },
    {
        "owl_search",
        "Recherche transversale insensible casse dans la doc publique, "
        "le registry (natives/types/enums/constantes) et les scénarios. "
        "Utile pour trouver « qu'est-ce qui touche à X » sans connaître "
        "le nom canon.",
        schema_owl_search,
        handler_owl_search
    },
    {
        "owl_examples",
        "Liste les scénarios canoniques publics qui démontrent l'usage "
        "d'un symbole. Le body OWL est inclus tel quel — code prêt à "
        "copier/adapter.",
        schema_owl_examples,
        handler_owl_examples
    },
    {
        "owl_where",
        "Reverse lookup : qui utilise ce symbole ? Pour une propriété "
        "DSL (Grisé, Visible…), liste les champs qui l'acceptent. "
        "Pour un type de champ (Bouton, Corps…), indique le contexte "
        "parent où il vit. Complémentaire à owl_explain (« que suis-je ? »).",
        schema_owl_where,
        handler_owl_where
    },
    {
        "owl_signature",
        "Forme courte d'une fonction native : seulement les signatures "
        "`Nom(arg : type, ...) → retour`, sans description ni exemples. "
        "Tokeniquement plus léger que owl_explain quand on a juste besoin "
        "de la forme appelable.",
        schema_owl_signature,
        handler_owl_signature
    },
    {
        "owl_format",
        "Reformate du source OWL avec le canon (casse, accents, "
        "indentation). Retourne le code reformatté. Erreur si le source "
        "ne parse pas — utilise owl_check pour le diagnostic.",
        schema_owl_format,
        handler_owl_format
    },
    {
        "owl_translate",
        "Réécrit la surface langage d'un source OWL dans l'autre langue "
        "(argument 'to' : \"en\" ou \"fr\"). Mots-clés, natives, types et "
        "propriétés builtin passent dans la langue cible ; identifiants "
        "utilisateur, chaînes et commentaires restent inchangés. Le code "
        "est aussi recanonisé (casse, accents, indentation) au passage. "
        "Erreur si le source ne parse pas ou si 'to' n'est ni \"en\" ni "
        "\"fr\".",
        schema_owl_translate,
        handler_owl_translate
    },
    {
        "owl_check",
        "Diagnostic compile-only et mono-fichier sur du source OWL. "
        "Retourne un JSON `{errors:[...], warnings:[...]}` avec "
        "line/column/message. Ne voit qu'un seul snippet : ne connaît pas "
        "les autres fichiers du projet (pas de résolution cross-fichier) et "
        "n'exécute rien (ne détecte pas les erreurs runtime). Utile pour "
        "valider qu'un snippet généré compile avant de l'écrire sur disque.",
        schema_owl_check,
        handler_owl_check
    },
    {
        "owl_template",
        "Squelettes OWL prêts à l'emploi. Sans argument : liste des "
        "templates disponibles (nom, niveau, description). Avec name : "
        "retourne le code prêt à coller. Idéal pour amorcer un script, "
        "une classe, un scénario de test ou une fenêtre sans repartir "
        "de zéro.",
        schema_owl_template,
        handler_owl_template
    },
    {
        "owl_limits",
        "Registry des limites compile-time du compilateur OWL "
        "(MAX_USER_TYPES, MAX_DECLARED_METHODS, MAX_USER_PROC_PARAMS, etc.). "
        "Sans argument : liste complete. Avec 'name' : fiche detaillee. "
        "Avec 'category' : filtre par famille (Compiler/Types, Compiler/POO, "
        "Compiler/Globales, ...). Utile pour planifier la charpente d'un "
        "projet OWL en connaissant les seuils du compilateur a l'avance.",
        schema_owl_limits,
        handler_owl_limits
    },
    {
        "owl_new",
        "Squelettes OWL génériques minimaux — déclaration + props "
        "courantes + event(s) lifecycle de base. Couvre fenetre, classe, "
        "et 11 champs DSL (champ bouton/libelle/saisie/table/conteneur/"
        "image/liste/combo/selecteur/interrupteur/zonerepetee). "
        "Différent de owl_template (exemples métier riches) : owl_new "
        "donne juste l'ossature pour démarrer. Validé par self-test "
        "rule 37 — compile toujours sans erreur.",
        schema_owl_new,
        handler_owl_new
    },
    {
        "owl_list",
        "Énumère une famille du catalogue OWL : fonctions natives, types, "
        "constantes, mots-clés, opérateurs, champs DSL, events, propriétés, "
        "variables globales prédéfinies, limites du compilateur — ou "
        "l'index des catégories fonctionnelles. Point d'entrée pour "
        "découvrir ce que le langage offre dans un domaine donné : "
        "bucket='categories' donne les familles (UI, Chaines, Dates, SQL, "
        "HTTP, …), puis bucket='natives' avec cat='UI' donne les fonctions "
        "de cette famille. Complémentaire à owl_search (recherche par "
        "mot-clé) et owl_explain (fiche d'un symbole précis).",
        schema_owl_list,
        handler_owl_list
    },
};
static const int TOOLS_COUNT = (int)(sizeof(TOOLS) / sizeof(TOOLS[0]));

static JsonValue* handle_tools_list(JsonValue* id) {
    JsonValue* result = json_new_object();
    JsonValue* arr = json_new_array();
    for (int i = 0; i < TOOLS_COUNT; i++) {
        JsonValue* t = json_new_object();
        json_object_set(t, "name",        json_new_string(TOOLS[i].name));
        json_object_set(t, "description", json_new_string(TOOLS[i].description));
        json_object_set(t, "inputSchema", TOOLS[i].schema_builder());
        json_array_push(arr, t);
    }
    json_object_set(result, "tools", arr);
    return make_response(id, result);
}

static JsonValue* handle_tools_call(JsonValue* id, JsonValue* params) {
    if (!params) {
        return make_error(id, -32602,
            "Invalid params: tools/call requires {name, arguments}");
    }
    const char* name = json_get_string(params, "name");
    if (!name || !*name) {
        return make_error(id, -32602,
            "Invalid params: missing 'name'");
    }
    JsonValue* arguments = json_get(params, "arguments");
    /* arguments peut être NULL si le tool n'attend rien — les handlers
     * gèrent NULL eux-mêmes via json_get_string qui retourne NULL. */

    for (int i = 0; i < TOOLS_COUNT; i++) {
        if (strcmp(TOOLS[i].name, name) == 0) {
            JsonValue* tool_result = TOOLS[i].handler(arguments);
            return make_response(id, tool_result);
        }
    }
    /* Tool inconnu → erreur protocole (le LLM s'est trompé de nom).
     * Différent d'une erreur applicative qui passerait par isError. */
    char msg[256];
    snprintf(msg, sizeof msg, "Unknown tool: '%s'", name);
    return make_error(id, -32602, msg);
}

/* ----------------------------------------------------------------
 * Resources — public docs + catalog plats
 *
 * resources/list énumère :
 *   - 1 entrée par doc publique (~9) : owl://public_doc/{stem}
 *   - 1 catalogue plat par famille de CATALOG_FAMILIES :
 *     owl://catalog/{famille}
 *
 * Pas d'énumération exhaustive du registry (sinon 600+ entrées dans
 * resources/list → bruit côté LLM). Les fiches détaillées sont
 * accessibles via des templates URI (resources/templates/list) :
 *   - owl://native/{name}
 *   - owl://type/{name}
 *   - owl://scenario/{name}
 * ---------------------------------------------------------------- */

/* Crée une entrée resource pour resources/list. mime/desc optionnels. */
static JsonValue* res_entry(const char* uri, const char* name,
                            const char* mime, const char* desc) {
    JsonValue* r = json_new_object();
    json_object_set(r, "uri",  json_new_string(uri));
    json_object_set(r, "name", json_new_string(name));
    if (mime) json_object_set(r, "mimeType",    json_new_string(mime));
    if (desc) json_object_set(r, "description", json_new_string(desc));
    return r;
}

/* Crée un block content pour resources/read. text déjà null-terminé. */
static JsonValue* res_content(const char* uri, const char* mime,
                              const char* text) {
    JsonValue* c = json_new_object();
    json_object_set(c, "uri",      json_new_string(uri));
    json_object_set(c, "mimeType", json_new_string(mime));
    json_object_set(c, "text",     json_new_string(text ? text : ""));
    return c;
}

/* Enveloppe resources/read : {"contents": [<block>]}. */
static JsonValue* res_read_result(JsonValue* id, JsonValue* content_block) {
    JsonValue* result = json_new_object();
    JsonValue* arr = json_new_array();
    json_array_push(arr, content_block);
    json_object_set(result, "contents", arr);
    return make_response(id, result);
}

static JsonValue* handle_resources_list(JsonValue* id) {
    JsonValue* result = json_new_object();
    JsonValue* arr = json_new_array();

    /* Public docs — chacune en resource à part entière. */
    char uri[256], title[256];
    for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
        snprintf(uri,   sizeof uri,   "owl://public_doc/%s", d->name);
        snprintf(title, sizeof title, "Doc publique : %s",   d->name);
        json_array_push(arr, res_entry(uri, title, "text/markdown",
            "Fiche de doc publique OWL embarquée dans le binaire."));
    }

    /* Catalogues plats — un par famille de CATALOG_FAMILIES, donc exactement
     * ceux que owl://catalog/<famille> accepte et que owl_list sert. */
    for (const CatalogFamily* f = CATALOG_FAMILIES; f->name; f++) {
        snprintf(uri, sizeof uri, "owl://catalog/%s", f->name);
        json_array_push(arr, res_entry(uri, f->title, "text/plain", f->desc));
    }

    json_object_set(result, "resources", arr);
    return make_response(id, result);
}

static JsonValue* handle_resources_templates_list(JsonValue* id) {
    JsonValue* result = json_new_object();
    JsonValue* arr = json_new_array();

    JsonValue* t1 = json_new_object();
    json_object_set(t1, "uriTemplate", json_new_string("owl://native/{name}"));
    json_object_set(t1, "name",        json_new_string("Fiche fonction native"));
    json_object_set(t1, "mimeType",    json_new_string("text/plain"));
    json_object_set(t1, "description", json_new_string(
        "Fiche complète d'une fonction native (description, signatures, "
        "scénarios). Équivalent à owl_explain --cat natives."));
    json_array_push(arr, t1);

    JsonValue* t2 = json_new_object();
    json_object_set(t2, "uriTemplate", json_new_string("owl://type/{name}"));
    json_object_set(t2, "name",        json_new_string("Fiche type"));
    json_object_set(t2, "mimeType",    json_new_string("text/plain"));
    json_object_set(t2, "description", json_new_string(
        "Fiche complète d'un type — primitif (chaîne, entier, réel, …) "
        "ou composé (httpRequête, Cadre, …). Le nom est insensible à la "
        "casse et aux accents."));
    json_array_push(arr, t2);

    JsonValue* t3 = json_new_object();
    json_object_set(t3, "uriTemplate", json_new_string("owl://enum/{name}"));
    json_object_set(t3, "name",        json_new_string("Fiche énumération"));
    json_object_set(t3, "mimeType",    json_new_string("text/plain"));
    json_object_set(t3, "description", json_new_string(
        "Liste des valeurs d'une énumération (ex. httpMéthodes, "
        "typeMime, traitCadre)."));
    json_array_push(arr, t3);

    JsonValue* t4 = json_new_object();
    json_object_set(t4, "uriTemplate", json_new_string("owl://constant/{name}"));
    json_object_set(t4, "name",        json_new_string("Fiche constante"));
    json_object_set(t4, "mimeType",    json_new_string("text/plain"));
    json_object_set(t4, "description", json_new_string(
        "Fiche d'une constante nommée (ex. httpGet, sscDroite, "
        "TraitContinu)."));
    json_array_push(arr, t4);

    JsonValue* t5 = json_new_object();
    json_object_set(t5, "uriTemplate", json_new_string("owl://control/{name}"));
    json_object_set(t5, "name",        json_new_string("Fiche champ DSL"));
    json_object_set(t5, "mimeType",    json_new_string("text/plain"));
    json_object_set(t5, "description", json_new_string(
        "Fiche complète d'un champ du DSL Fenêtre (Bouton, Saisie, "
        "Libellé, Fenêtre, Image, Liste, Combo, …) : propriétés DSL "
        "acceptées, types attendus, énumérations contraintes, events "
        "disponibles (PROCEDURE surClic, surDoubleClic, …) avec leurs "
        "paramètres typés."));
    json_array_push(arr, t5);

    JsonValue* t6 = json_new_object();
    json_object_set(t6, "uriTemplate", json_new_string("owl://scenario/{name}"));
    json_object_set(t6, "name",        json_new_string("Scénario canon"));
    json_object_set(t6, "mimeType",    json_new_string("text/plain"));
    json_object_set(t6, "description", json_new_string(
        "Code OWL brut d'un scénario canonique (test_<name>.owl). "
        "Utile comme exemple à adapter."));
    json_array_push(arr, t6);

    JsonValue* t7 = json_new_object();
    json_object_set(t7, "uriTemplate", json_new_string("owl://keyword/{name}"));
    json_object_set(t7, "name",        json_new_string("Fiche mot-clé"));
    json_object_set(t7, "mimeType",    json_new_string("text/plain"));
    json_object_set(t7, "description", json_new_string(
        "Fiche d'un mot-clé du langage (SI, POUR, SELON, CLASSE, …) : "
        "syntaxe, sens, exemple court."));
    json_array_push(arr, t7);

    JsonValue* t8 = json_new_object();
    json_object_set(t8, "uriTemplate", json_new_string("owl://operator/{symbol}"));
    json_object_set(t8, "name",        json_new_string("Fiche opérateur"));
    json_object_set(t8, "mimeType",    json_new_string("text/plain"));
    json_object_set(t8, "description", json_new_string(
        "Fiche d'un opérateur symbolique (?., ?\?=, [~~, ~=, <-, …). "
        "URL-encode si nécessaire (ex. %3F%3F%3D pour ?\?=)."));
    json_array_push(arr, t8);

    JsonValue* t9 = json_new_object();
    json_object_set(t9, "uriTemplate", json_new_string("owl://event/{name}"));
    json_object_set(t9, "name",        json_new_string("Fiche event DSL"));
    json_object_set(t9, "mimeType",    json_new_string("text/plain"));
    json_object_set(t9, "description", json_new_string(
        "Fiche d'un event DSL (surClic, surValidation, Initialisation, "
        "Filtre, …) : description, paramètres typés, champs qui l'exposent."));
    json_array_push(arr, t9);

    JsonValue* t10 = json_new_object();
    json_object_set(t10, "uriTemplate", json_new_string("owl://prop/{name}"));
    json_object_set(t10, "name",        json_new_string("Fiche propriété DSL"));
    json_object_set(t10, "mimeType",    json_new_string("text/plain"));
    json_object_set(t10, "description", json_new_string(
        "Fiche d'une propriété DSL (Largeur, Visible, Grisé, …) : type, "
        "enum contraint si applicable, champs qui l'acceptent."));
    json_array_push(arr, t10);

    JsonValue* t11 = json_new_object();
    json_object_set(t11, "uriTemplate", json_new_string("owl://globalvar/{name}"));
    json_object_set(t11, "name",        json_new_string("Fiche variable globale prédéfinie"));
    json_object_set(t11, "mimeType",    json_new_string("text/plain"));
    json_object_set(t11, "description", json_new_string(
        "Fiche d'une variable globale prédéfinie du langage "
        "(ErreurDétectée, …) : type, lecture seule, description."));
    json_array_push(arr, t11);

    JsonValue* t12 = json_new_object();
    json_object_set(t12, "uriTemplate", json_new_string("owl://method/{name}"));
    json_object_set(t12, "name",        json_new_string("Fiche méthode primitif"));
    json_object_set(t12, "mimeType",    json_new_string("text/plain"));
    json_object_set(t12, "description", json_new_string(
        "Fiche d'une méthode sur primitif (.Taille, .VersDate, .Debut, …) : "
        "receivers supportés + natives canon équivalentes."));
    json_array_push(arr, t12);

    JsonValue* t13 = json_new_object();
    json_object_set(t13, "uriTemplate", json_new_string("owl://builtin_prop/{name}"));
    json_object_set(t13, "name",        json_new_string("Fiche propriété builtin"));
    json_object_set(t13, "mimeType",    json_new_string("text/plain"));
    json_object_set(t13, "description", json_new_string(
        "Fiche d'une propriété builtin sur primitif (.Année, .EnHeures, "
        ".Vide, …) : type, receivers (date, durée, …), description."));
    json_array_push(arr, t13);

    JsonValue* t14 = json_new_object();
    json_object_set(t14, "uriTemplate", json_new_string("owl://category/{name}"));
    json_object_set(t14, "name",        json_new_string("Fonctions d'une catégorie"));
    json_object_set(t14, "mimeType",    json_new_string("text/plain"));
    json_object_set(t14, "description", json_new_string(
        "Fonctions natives d'une catégorie fonctionnelle (UI, Chaines, "
        "Dates, SQL, HTTP, …), avec leur description courte. Le nom est "
        "insensible à la casse et accepte le libellé anglais. L'index des "
        "catégories est owl://catalog/categories."));
    json_array_push(arr, t14);

    json_object_set(result, "resourceTemplates", arr);
    return make_response(id, result);
}

/* Construit un text duppliqué null-terminé depuis (data, len). */
static char* dup_blob(const unsigned char* data, long len) {
    char* s = (char*)malloc((size_t)len + 1);
    if (!s) return NULL;
    memcpy(s, data, (size_t)len);
    s[len] = '\0';
    return s;
}

/* resources/read — dispatch par scheme `owl://`. */
static JsonValue* handle_resources_read(JsonValue* id, JsonValue* params) {
    if (!params) {
        return make_error(id, -32602,
            "Invalid params: resources/read requires {uri}");
    }
    const char* uri = json_get_string(params, "uri");
    if (!uri || !*uri) {
        return make_error(id, -32602, "Invalid params: missing 'uri'");
    }
    if (strncmp(uri, "owl://", 6) != 0) {
        return make_error(id, -32002,
            "Resource not found: only owl:// scheme is supported");
    }
    const char* rest = uri + 6;

    /* owl://public_doc/<stem> */
    if (strncmp(rest, "public_doc/", 11) == 0) {
        const char* name = rest + 11;
        for (const OwlEmbeddedDoc* d = EMBEDDED_PUBLIC_DOCS; d->name; d++) {
            if (strcmp(d->name, name) == 0) {
                char* text = dup_blob(d->data, d->len);
                JsonValue* block = res_content(uri, "text/markdown",
                                               text ? text : "");
                free(text);
                return res_read_result(id, block);
            }
        }
        char msg[256];
        snprintf(msg, sizeof msg, "Public doc not found: '%s'", name);
        return make_error(id, -32002, msg);
    }

    /* owl://catalog/<bucket> — délègue à cli_doc_list via capture. */
    if (strncmp(rest, "catalog/", 8) == 0) {
        const char* bucket = rest + 8;
        if (!list_bucket_is_known(bucket)) {
            char msg[320];
            size_t n = (size_t)snprintf(msg, sizeof msg,
                "Unknown catalog: '%s'. Valid:", bucket);
            catalog_families_append(msg, sizeof msg, n);
            return make_error(id, -32002, msg);
        }
        char* cli_argv[4];
        int cli_argc = 0;
        cli_argv[cli_argc++] = "list";
        cli_argv[cli_argc++] = (char*)bucket;
        cli_argv[cli_argc++] = "--plain";
        int rc = 0;
        char* out = capture_cli(cli_doc_list, cli_argc, cli_argv, &rc);
        if (!out) {
            return make_error(id, -32603,
                "Internal error: catalog capture failed");
        }
        /* `cli_doc_list` rend 1 sur une famille qu'il ne dispatche pas, et
         * imprime son diagnostic — que la capture rapatrie avec le reste. Sans
         * ce test, ce diagnostic partait comme contenu de la ressource
         * : une famille annoncee d'un cote et absente du dispatch de
         * l'autre servait une phrase d'erreur au client sans aucun signal. */
        if (rc != 0) {
            char msg[320];
            size_t n = (size_t)snprintf(msg, sizeof msg,
                "Catalog not available: '%s'. Valid:", bucket);
            catalog_families_append(msg, sizeof msg, n);
            free(out);
            return make_error(id, -32002, msg);
        }
        JsonValue* block = res_content(uri, "text/plain", out);
        free(out);
        return res_read_result(id, block);
    }

    /* owl://category/<name> — fonctions natives d'une famille. Une
     * catégorie n'est pas un symbole : elle passe par `list natives --cat`,
     * pas par `explain`. Testée avant le bloc de dispatch symbole pour que
     * le préfixe reste sans ambiguïté. */
    if (strncmp(rest, "category/", 9) == 0) {
        const char* name = rest + 9;
        if (!*name) {
            return make_error(id, -32002,
                "Resource not found: empty category name");
        }
        /* Nom hors domaine : refusé avant capture. Une famille valable mais
         * vide reste une ressource légitime. */
        if (!cli_doc_list_category_is_known("natives", name)) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Category not found: '%s'. See owl://catalog/categories "
                     "for the valid names.", name);
            return make_error(id, -32002, msg);
        }
        char* cli_argv[6];
        int cli_argc = 0;
        cli_argv[cli_argc++] = "list";
        cli_argv[cli_argc++] = "natives";
        cli_argv[cli_argc++] = "--cat";
        cli_argv[cli_argc++] = (char*)name;
        cli_argv[cli_argc++] = "--plain";
        int rc = 0;
        char* out = capture_cli(cli_doc_list, cli_argc, cli_argv, &rc);
        if (!out) {
            return make_error(id, -32603,
                "Internal error: category capture failed");
        }
        if (rc != 0) {
            char msg[256];
            snprintf(msg, sizeof msg,
                     "Category not found: '%s'. See owl://catalog/categories "
                     "for the valid names.", name);
            free(out);
            return make_error(id, -32002, msg);
        }
        JsonValue* block = res_content(uri, "text/plain", out);
        free(out);
        return res_read_result(id, block);
    }

    /* owl://{native,type,enum,constant,widget}/<name> — explain filtré
     * sur le bucket correspondant. `type` couvre primitifs ET composés. */
    {
        const char* bucket_cli = NULL;  /* valeur passée à --cat */
        const char* name = NULL;
        if (strncmp(rest, "native/", 7) == 0) {
            bucket_cli = "natives";   name = rest + 7;
        } else if (strncmp(rest, "type/", 5) == 0) {
            bucket_cli = "types";     name = rest + 5; /* primitifs + composés */
        } else if (strncmp(rest, "enum/", 5) == 0) {
            bucket_cli = "enums";     name = rest + 5;
        } else if (strncmp(rest, "constant/", 9) == 0) {
            bucket_cli = "constants"; name = rest + 9;
        } else if (strncmp(rest, "control/", 8) == 0) {
            bucket_cli = "controls";  name = rest + 8;
        } else if (strncmp(rest, "keyword/", 8) == 0) {
            bucket_cli = "keyword";   name = rest + 8;
        } else if (strncmp(rest, "operator/", 9) == 0) {
            bucket_cli = "operator";  name = rest + 9;
        } else if (strncmp(rest, "event/", 6) == 0) {
            bucket_cli = "event";     name = rest + 6;
        } else if (strncmp(rest, "prop/", 5) == 0) {
            bucket_cli = "prop";      name = rest + 5;
        } else if (strncmp(rest, "globalvar/", 10) == 0) {
            bucket_cli = "globalvar"; name = rest + 10;
        } else if (strncmp(rest, "method/", 7) == 0) {
            bucket_cli = "method";    name = rest + 7;
        } else if (strncmp(rest, "builtin_prop/", 13) == 0) {
            bucket_cli = "builtin-prop"; name = rest + 13;
        }
        if (bucket_cli) {
            if (!name || !*name) {
                return make_error(id, -32002,
                    "Resource not found: empty symbol name");
            }
            char* cli_argv[8];
            int cli_argc = 0;
            cli_argv[cli_argc++] = "explain";
            cli_argv[cli_argc++] = (char*)name;
            cli_argv[cli_argc++] = "--cat";
            cli_argv[cli_argc++] = (char*)bucket_cli;
            cli_argv[cli_argc++] = "--plain";
            int rc = 0;
            char* out = capture_cli(cli_doc_explain, cli_argc, cli_argv, &rc);
            if (!out) {
                return make_error(id, -32603,
                    "Internal error: explain capture failed");
            }
            if (rc != 0) {
                char msg[256];
                snprintf(msg, sizeof msg,
                         "Resource not found: '%s' (rc=%d)", uri, rc);
                free(out);
                return make_error(id, -32002, msg);
            }
            JsonValue* block = res_content(uri, "text/plain", out);
            free(out);
            return res_read_result(id, block);
        }
    }

    /* owl://scenario/<stem> — code OWL brut du scénario. */
    if (strncmp(rest, "scenario/", 9) == 0) {
        const char* name = rest + 9;
        for (const OwlEmbeddedDoc* s = EMBEDDED_SCENARIOS; s->name; s++) {
            if (strcmp(s->name, name) == 0) {
                char* text = dup_blob(s->data, s->len);
                JsonValue* block = res_content(uri, "text/plain",
                                               text ? text : "");
                free(text);
                return res_read_result(id, block);
            }
        }
        char msg[256];
        snprintf(msg, sizeof msg, "Scenario not found: '%s'", name);
        return make_error(id, -32002, msg);
    }

    char msg[256];
    snprintf(msg, sizeof msg, "Resource not found: '%s'", uri);
    return make_error(id, -32002, msg);
}

static JsonValue* handle_prompts_list(JsonValue* id) {
    JsonValue* result = json_new_object();
    json_object_set(result, "prompts", json_new_array());
    return make_response(id, result);
}

/* ----------------------------------------------------------------
 * Dispatch
 * ---------------------------------------------------------------- */

/* Traite un message JSON-RPC entrant. Si c'est une notification (pas
 * d'id), ne renvoie rien. Sinon renvoie la réponse à envoyer.
 * Exposé via cli_mcp_handle_request pour les self-tests. */
static JsonValue* dispatch(JsonValue* req) {
    const char* method = json_get_string(req, "method");
    JsonValue* id = clone_id(req);

    /* Notifications — pas de réponse. id_clone est NULL → on libère
     * rien (clone_id n'a rien alloué). */
    if (!id) {
        /* Connues, no-op silencieux : notifications/initialized,
         * notifications/cancelled, notifications/progress, etc.
         * V1 étape 1 ne traite aucune notification activement. */
        (void)method;
        return NULL;
    }

    if (!method) {
        return make_error(id, -32600, "Invalid Request: missing method");
    }

    JsonValue* params = json_get(req, "params");

    if (strcmp(method, "initialize") == 0)
        return handle_initialize(id, params);
    if (strcmp(method, "ping") == 0)
        return handle_ping(id);
    if (strcmp(method, "tools/list") == 0)
        return handle_tools_list(id);
    if (strcmp(method, "tools/call") == 0)
        return handle_tools_call(id, params);
    if (strcmp(method, "resources/list") == 0)
        return handle_resources_list(id);
    if (strcmp(method, "resources/read") == 0)
        return handle_resources_read(id, params);
    if (strcmp(method, "resources/templates/list") == 0)
        return handle_resources_templates_list(id);
    if (strcmp(method, "prompts/list") == 0)
        return handle_prompts_list(id);

    /* Méthode inconnue : code JSON-RPC standard -32601. Le client doit
     * recevoir une réponse — pas juste un silence — sinon il timeout. */
    return make_error(id, -32601, "Method not found");
}

/* Shim public pour les self-tests : prend une ligne JSON-RPC brute,
 * renvoie la réponse JSON sérialisée en malloc'd string ou NULL si la
 * requête était une notification. */
char* cli_mcp_handle_request(const char* line, int line_len) {
    if (!line || line_len <= 0) return NULL;
    JsonValue* req = json_parse(line, line_len);
    if (!req) {
        /* Parse error → on synthétise la réponse standard JSON-RPC. */
        JsonValue* err = make_error(NULL, -32700, "Parse error");
        int n = 0;
        char* s = json_serialize(err, &n);
        json_free(err);
        return s;
    }
    JsonValue* resp = dispatch(req);
    json_free(req);
    if (!resp) return NULL; /* notification, pas de réponse */
    int n = 0;
    char* s = json_serialize(resp, &n);
    json_free(resp);
    return s;
}

/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

int cli_mcp_serve(int argc, char** argv) {
    (void)argc; (void)argv;

#ifdef _WIN32
    /* Mode binaire strict sur stdin/stdout : MCP exige du LF pur
     * comme délimiteur. En mode texte Windows, stdout convertit \n
     * en \r\n → JSON cassé pour le client. */
    owl_setmode(owl_fileno(stdin),  _O_BINARY);
    owl_setmode(owl_fileno(stdout), _O_BINARY);
#endif

    fprintf(stderr, "[owl mcp] serveur démarré (proto %s, %s %s)\n",
            OWL_MCP_PROTOCOL_VERSION,
            OWL_MCP_SERVER_NAME, OWL_MCP_SERVER_VERSION);

    for (;;) {
        size_t n = 0;
        char* line = read_line_alloc(stdin, &n);
        if (!line) {
            fprintf(stderr, "[owl mcp] EOF — arrêt propre.\n");
            return 0;
        }
        if (n == 0) { free(line); continue; }

        JsonValue* req = json_parse(line, (int)n);
        free(line);
        if (!req) {
            /* Parse error — on doit répondre si possible, mais sans
             * id (qu'on n'a pas pu lire), c'est une notification
             * d'erreur. Le client gère. */
            JsonValue* err = make_error(NULL, -32700, "Parse error");
            send_message(err);
            json_free(err);
            continue;
        }

        JsonValue* resp = dispatch(req);
        if (resp) {
            send_message(resp);
            json_free(resp);
        }
        json_free(req);
    }
}
