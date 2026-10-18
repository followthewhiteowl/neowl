// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test simple_query MySQL via mock server.
 * Valide : COM_QUERY -> ColumnCount -> ColumnDef -> EOF -> DataRow -> EOF. */

#include "sql_mysql_query.h"
#include "sql_mysql_conn.h"
#include "sql_mysql_mock_server.h"

#include "vm.h"
#include "object.h"
#include "table.h"
#include "value.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ================================================================
 *  — parametres nommes sur SQLEnvoie (MySQL).
 *
 * Contrairement a test_simple_select_42 (qui parle directement au wire
 * via owl_mysql_simple_query), ces cas passent par le VRAI chemin OWL
 * (vm_interpret + SQLEnvoie), pour asserter au niveau comportemental
 * (..Succès, ..LignesAffectées, ..Occurrence) plutot que sur les octets
 * du protocole. envoie_mysql (stdlib_sql.c) rejette aujourd'hui tout
 * parametre AVANT d'ecrire sur le socket -> rouge legitime sans besoin
 * d'etendre le mock. Une fois le binding server-side implemente (+ mock
 * etendu pour repondre PREPARE/EXECUTE), ces memes scenarios doivent
 * passer au vert sans modification.
 * ================================================================ */

static bool lit_globale(const char* nom, Value* out) {
    ObjString* k = obj_copy_string(nom, (int)strlen(nom));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&vm.globals, k, out);
    vm_pop();
    return ok;
}

static const char* lit_globale_chaine(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return NULL;
    if (!IS_STRING(v)) return NULL;
    return AS_CSTRING(v);
}

static long lit_globale_entier(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return -1;
    if (IS_NUMBER(v)) return (long)AS_NUMBER(v);
    return -1;
}

static int lit_globale_bool(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return -1;
    if (!IS_BOOL(v)) return -1;
    return AS_BOOL(v) ? 1 : 0;
}

/* Demarre un mock pret pour le handshake natif alice/s3cret/testdb —
 * meme reglage que test_simple_select_42 et sql_mysql_e2e_test.c. */
static MysqlMockServer* param_mock_start(void) {
    return mysql_mock_server_start(MYSQL_MOCK_AUTH_OK, MYSQL_MOCK_PLUGIN_NATIVE,
                                    "s3cret");
}

static void test_param_insert_basic(void) {
    MysqlMockServer* m = param_mock_start();
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup (param insert basic)");
        if (m) mysql_mock_server_free(m);
        return;
    }

    static const char* FMT =
        "paramOk est un booléen = Faux\n"
        "paramLignes est un entier = 0\n"
        "paramErr est une chaîne = \"\"\n"
        "\n"
        "PROCEDURE paramExecute()\n"
        "    cnx est une sqlConnexion\n"
        "    cnx..Provider      = sqlMySQL\n"
        "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
        "    cnx..Utilisateur   = \"alice\"\n"
        "    cnx..MotDePasse    = \"s3cret\"\n"
        "    cnx..BaseDeDonnées = \"testdb\"\n"
        "    cnx..ModeTLS       = tlsDesactive\n"
        "    cnx.Ouvre()\n"
        "    req est une sqlRequête\n"
        "    req..Code              = \"insert into dummy (dummytxt) values ({txt})\"\n"
        "    req..Paramètres[\"txt\"] = \"abc\"\n"
        "    rsp est une sqlRéponse = SQLEnvoie(cnx, req)\n"
        "    paramOk     = rsp..Succès\n"
        "    paramLignes = rsp..LignesAffectées\n"
        "    paramErr    = ErreurInfo()\n"
        "    cnx.Ferme()\n"
        "FIN\n";
    char src[2048];
    snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m));

    vm_init();
    InterpretResult r = vm_interpret(src);
    CHECK(r == INTERPRET_OK, "paramExecute compile (insert basic)");
    if (r == INTERPRET_OK) {
        InterpretResult call = vm_call_global("paramExecute");
        CHECK(call == INTERPRET_OK, "paramExecute s'execute sans crash (insert basic)");
        /* Cas exact du user : {txt} + Paramètres["txt"] = "abc" doit inserer
         * la ligne et renvoyer Succès=Vrai + 1 ligne affectee. */
        CHECK(lit_globale_bool("paramOk") == 1,
              "SQLEnvoie (MySQL) INSERT parametre -> Succès = Vrai");
        CHECK(lit_globale_entier("paramLignes") == 1,
              "SQLEnvoie (MySQL) INSERT parametre -> LignesAffectées = 1");
    }
    vm_free();

    mysql_mock_server_join(m);
    mysql_mock_server_free(m);
}

static void test_param_bilingual_aliases(void) {
    /* 4 combinaisons FR/EN : ..Paramètres vs ..Parameters, SQLEnvoie vs
     * SQLSend. Les quatre doivent se comporter identiquement. */
    static const char* COMBOS[4][2] = {
        { "Paramètres",  "SQLEnvoie" },
        { "Paramètres",  "SQLSend"   },
        { "Parameters",  "SQLEnvoie" },
        { "Parameters",  "SQLSend"   },
    };

    for (int i = 0; i < 4; i++) {
        MysqlMockServer* m = param_mock_start();
        if (!m || !mysql_mock_server_is_ready(m)) {
            CHECK(false, "mock setup (bilingue)");
            if (m) mysql_mock_server_free(m);
            continue;
        }

        static const char* FMT =
            "paramOk est un booléen = Faux\n"
            "paramLignes est un entier = 0\n"
            "\n"
            "PROCEDURE paramExecute()\n"
            "    cnx est une sqlConnexion\n"
            "    cnx..Provider      = sqlMySQL\n"
            "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
            "    cnx..Utilisateur   = \"alice\"\n"
            "    cnx..MotDePasse    = \"s3cret\"\n"
            "    cnx..BaseDeDonnées = \"testdb\"\n"
            "    cnx..ModeTLS       = tlsDesactive\n"
            "    cnx.Ouvre()\n"
            "    req est une sqlRequête\n"
            "    req..Code         = \"insert into dummy (dummytxt) values ({txt})\"\n"
            "    req..%s[\"txt\"] = \"abc\"\n"
            "    rsp est une sqlRéponse = %s(cnx, req)\n"
            "    paramOk     = rsp..Succès\n"
            "    paramLignes = rsp..LignesAffectées\n"
            "    cnx.Ferme()\n"
            "FIN\n";
        char src[2048];
        snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m),
                 COMBOS[i][0], COMBOS[i][1]);

        char label[128];
        snprintf(label, sizeof(label), "bilingue %s/%s -> Succès = Vrai",
                 COMBOS[i][0], COMBOS[i][1]);

        vm_init();
        InterpretResult r = vm_interpret(src);
        if (r == INTERPRET_OK) {
            vm_call_global("paramExecute");
            CHECK(lit_globale_bool("paramOk") == 1, label);
        } else {
            CHECK(false, label);
        }
        vm_free();

        mysql_mock_server_join(m);
        mysql_mock_server_free(m);
    }
}

static void test_param_types_coverage(void) {
    /* Une valeur liee par type : chaine, entier, reel, booleen, NULL. */
    MysqlMockServer* m = param_mock_start();
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup (types)");
        if (m) mysql_mock_server_free(m);
        return;
    }

    static const char* FMT =
        "paramOk est un booléen = Faux\n"
        "paramLignes est un entier = 0\n"
        "\n"
        "PROCEDURE paramExecute()\n"
        "    cnx est une sqlConnexion\n"
        "    cnx..Provider      = sqlMySQL\n"
        "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
        "    cnx..Utilisateur   = \"alice\"\n"
        "    cnx..MotDePasse    = \"s3cret\"\n"
        "    cnx..BaseDeDonnées = \"testdb\"\n"
        "    cnx..ModeTLS       = tlsDesactive\n"
        "    cnx.Ouvre()\n"
        "    req est une sqlRequête\n"
        "    req..Code = \"insert into t (ps, pi, pr, pb, pn) values ({ps}, {pi}, {pr}, {pb}, {pn})\"\n"
        "    req..Paramètres[\"ps\"] = \"abc\"\n"
        "    req..Paramètres[\"pi\"] = 42\n"
        "    req..Paramètres[\"pr\"] = 3.14\n"
        "    req..Paramètres[\"pb\"] = Vrai\n"
        "    req..Paramètres[\"pn\"] = Null\n"
        "    rsp est une sqlRéponse = SQLEnvoie(cnx, req)\n"
        "    paramOk     = rsp..Succès\n"
        "    paramLignes = rsp..LignesAffectées\n"
        "    cnx.Ferme()\n"
        "FIN\n";
    char src[2048];
    snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m));

    vm_init();
    InterpretResult r = vm_interpret(src);
    if (r == INTERPRET_OK) {
        vm_call_global("paramExecute");
        CHECK(lit_globale_bool("paramOk") == 1,
              "bind chaine+entier+reel+booleen+NULL -> Succès = Vrai");
        CHECK(lit_globale_entier("paramLignes") == 1,
              "bind multi-types -> LignesAffectées = 1");
    } else {
        CHECK(false, "paramExecute compile (types)");
    }
    vm_free();

    mysql_mock_server_join(m);
    mysql_mock_server_free(m);
}

static void test_param_select_basic(void) {
    /* SELECT paramétré : exercera le decodage binary resultset une fois
     * le mock etendu pour COM_STMT_EXECUTE. */
    MysqlMockServer* m = param_mock_start();
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup (select)");
        if (m) mysql_mock_server_free(m);
        return;
    }

    static const char* FMT =
        "paramOk est un booléen = Faux\n"
        "paramOccurrence est un entier = 0\n"
        "\n"
        "PROCEDURE paramExecute()\n"
        "    cnx est une sqlConnexion\n"
        "    cnx..Provider      = sqlMySQL\n"
        "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
        "    cnx..Utilisateur   = \"alice\"\n"
        "    cnx..MotDePasse    = \"s3cret\"\n"
        "    cnx..BaseDeDonnées = \"testdb\"\n"
        "    cnx..ModeTLS       = tlsDesactive\n"
        "    cnx.Ouvre()\n"
        "    req est une sqlRequête\n"
        "    req..Code             = \"select dummytxt from dummy where dummytxt = {p}\"\n"
        "    req..Paramètres[\"p\"] = \"abc\"\n"
        "    rsp est une sqlRéponse = SQLEnvoie(cnx, req)\n"
        "    paramOk         = rsp..Succès\n"
        "    paramOccurrence = rsp..Occurrence\n"
        "    cnx.Ferme()\n"
        "FIN\n";
    char src[2048];
    snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m));

    vm_init();
    InterpretResult r = vm_interpret(src);
    if (r == INTERPRET_OK) {
        vm_call_global("paramExecute");
        CHECK(lit_globale_bool("paramOk") == 1,
              "SELECT parametre (MySQL) -> Succès = Vrai");
        CHECK(lit_globale_entier("paramOccurrence") >= 1,
              "SELECT parametre (MySQL) -> au moins 1 ligne decodee");
    } else {
        CHECK(false, "paramExecute compile (select)");
    }
    vm_free();

    mysql_mock_server_join(m);
    mysql_mock_server_free(m);
}

static void test_param_type_unsupported_message(void) {
    /* Type non bindable (tableau) : le message doit nommer le parametre,
     * pas rester un "argument invalide" generique. */
    MysqlMockServer* m = param_mock_start();
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup (type non supporte)");
        if (m) mysql_mock_server_free(m);
        return;
    }

    static const char* FMT =
        "paramOk est un booléen = Faux\n"
        "paramErr est une chaîne = \"\"\n"
        "\n"
        "PROCEDURE paramExecute()\n"
        "    cnx est une sqlConnexion\n"
        "    cnx..Provider      = sqlMySQL\n"
        "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
        "    cnx..Utilisateur   = \"alice\"\n"
        "    cnx..MotDePasse    = \"s3cret\"\n"
        "    cnx..BaseDeDonnées = \"testdb\"\n"
        "    cnx..ModeTLS       = tlsDesactive\n"
        "    cnx.Ouvre()\n"
        "    req est une sqlRequête\n"
        "    req..Code = \"select {p}\"\n"
        "    t est un tableau de chaînes = [\"a\", \"b\"]\n"
        "    req..Paramètres[\"p\"] = t\n"
        "    rsp est une sqlRéponse = SQLEnvoie(cnx, req)\n"
        "    paramOk  = rsp..Succès\n"
        "    paramErr = ErreurInfo()\n"
        "    cnx.Ferme()\n"
        "FIN\n";
    char src[2048];
    snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m));

    vm_init();
    InterpretResult r = vm_interpret(src);
    if (r == INTERPRET_OK) {
        vm_call_global("paramExecute");
        CHECK(lit_globale_bool("paramOk") == 0,
              "bind tableau (non supporte) -> Succès = Faux");
        const char* msg = lit_globale_chaine("paramErr");
        CHECK(msg != NULL && strstr(msg, "type non supporte") != NULL,
              "message explicite 'bind {p}: type non supporte' (pas 'argument invalide' nu)");
    } else {
        CHECK(false, "paramExecute compile (type non supporte)");
    }
    vm_free();

    mysql_mock_server_join(m);
    mysql_mock_server_free(m);
}

static void test_param_injection_quote(void) {
    /* Valeur contenant une apostrophe : doit passer par le binding
     * server-side sans jamais etre concatenee dans le texte SQL. */
    MysqlMockServer* m = param_mock_start();
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup (injection)");
        if (m) mysql_mock_server_free(m);
        return;
    }

    static const char* FMT =
        "paramOk est un booléen = Faux\n"
        "paramLignes est un entier = 0\n"
        "\n"
        "PROCEDURE paramExecute()\n"
        "    cnx est une sqlConnexion\n"
        "    cnx..Provider      = sqlMySQL\n"
        "    cnx..Serveur       = \"127.0.0.1:%d\"\n"
        "    cnx..Utilisateur   = \"alice\"\n"
        "    cnx..MotDePasse    = \"s3cret\"\n"
        "    cnx..BaseDeDonnées = \"testdb\"\n"
        "    cnx..ModeTLS       = tlsDesactive\n"
        "    cnx.Ouvre()\n"
        "    req est une sqlRequête\n"
        "    req..Code              = \"insert into dummy (dummytxt) values ({txt})\"\n"
        "    req..Paramètres[\"txt\"] = `O'Brien`\n"
        "    rsp est une sqlRéponse = SQLEnvoie(cnx, req)\n"
        "    paramOk     = rsp..Succès\n"
        "    paramLignes = rsp..LignesAffectées\n"
        "    cnx.Ferme()\n"
        "FIN\n";
    char src[2048];
    snprintf(src, sizeof(src), FMT, mysql_mock_server_port(m));

    vm_init();
    InterpretResult r = vm_interpret(src);
    if (r == INTERPRET_OK) {
        vm_call_global("paramExecute");
        CHECK(lit_globale_bool("paramOk") == 1,
              "bind \"O'Brien\" (apostrophe) -> Succès = Vrai, pas de requete cassee");
        CHECK(lit_globale_entier("paramLignes") == 1,
              "bind \"O'Brien\" -> LignesAffectées = 1");
    } else {
        CHECK(false, "paramExecute compile (injection)");
    }
    vm_free();

    mysql_mock_server_join(m);

    CHECK(strstr(mysql_mock_server_last_prepare_sql(m), "?") != NULL,
          "mock a recu un PREPARE parametre (template '?')");
    CHECK(strstr(mysql_mock_server_last_prepare_sql(m), "O'Brien") == NULL,
          "la valeur \"O'Brien\" n'est pas concatenee dans le SQL prepare");
    CHECK(strcmp(mysql_mock_server_last_bind_param0(m), "O'Brien") == 0,
          "mock a recu EXECUTE param0 = \"O'Brien\" (binding server-side)");

    mysql_mock_server_free(m);
}

static void test_simple_select_42(void) {
    MysqlMockServer* m = mysql_mock_server_start(MYSQL_MOCK_AUTH_OK,
                                                  MYSQL_MOCK_PLUGIN_NATIVE,
                                                  "s3cret");
    if (!m || !mysql_mock_server_is_ready(m)) {
        CHECK(false, "mock setup");
        if (m) mysql_mock_server_free(m);
        return;
    }
    OwlMysqlConn c;
    owl_mysql_conn_init(&c);
    bool ok = owl_mysql_conn_open(&c, "127.0.0.1", mysql_mock_server_port(m),
                                    "alice", "s3cret", "testdb",
                                    OWL_SQL_TLS_DESACTIVE, NULL);
    CHECK(ok, "conn_open OK");
    if (!ok) { mysql_mock_server_free(m); return; }

    OwlMysqlResult r;
    owl_mysql_result_init(&r);
    bool qok = owl_mysql_simple_query(&c, "SELECT n FROM x", &r);
    CHECK(qok,          "simple_query reussit");
    CHECK(r.success,    "r.success = true");
    CHECK(r.column_count == 1,   "1 colonne");
    CHECK(r.columns && strcmp(r.columns[0].name, "n") == 0,
                                 "column.name = 'n'");
    CHECK(r.columns && r.columns[0].type == 0x03 /* MYSQL_TYPE_LONG */,
                                 "column.type = LONG (INT4)");
    CHECK(r.row_count == 1,      "1 ligne");
    CHECK(r.rows && r.rows[0][0].length == 2,
                                 "row[0][0].length = 2 (chars '42')");
    CHECK(r.rows && r.rows[0][0].data &&
          memcmp(r.rows[0][0].data, "42", 2) == 0,
                                 "row[0][0].data = '42'");

    owl_mysql_result_free(&r);
    owl_mysql_conn_close(&c);
    mysql_mock_server_join(m);
    CHECK(strcmp(mysql_mock_server_last_query(m), "SELECT n FROM x") == 0,
                                 "mock saw the right query");
    mysql_mock_server_free(m);
}

int sql_mysql_query_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    test_simple_select_42();
    test_param_insert_basic();
    test_param_bilingual_aliases();
    test_param_types_coverage();
    test_param_select_basic();
    test_param_type_unsupported_message();
    test_param_injection_quote();
    printf("sql_mysql_query self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
