// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_mysql_mock_server_h
#define owl_sql_mysql_mock_server_h

/* ================================================================
 * Mini-serveur MySQL mock — pour tests CI deterministes
 *
 * Implemente une portion suffisante du wire protocol cote serveur :
 *   - Initial Handshake v10 avec salt fixe et plugin selectionnable
 *   - Verification auth (recalcul du token via auth_mysql_* et compare)
 *   - OK packet -> "auth reussie"
 *   - Error packet -> "access denied"
 *   - Simple Query : repond avec ResultSet canon (1 col "n" INT4 = 42)
 *   - COM_QUIT : ferme proprement
 *
 * Single-shot : accepte UNE connexion puis se termine. Port choisi par
 * l'OS (bind sur 0) -> pas de conflit en CI parallele.
 * ================================================================ */

#include "common.h"

typedef enum {
    MYSQL_MOCK_AUTH_OK,           /* auth reussit si token correct */
    MYSQL_MOCK_AUTH_FAIL,         /* renvoie Error 1045 quelle que soit l'auth */
} MysqlMockAuthMode;

typedef enum {
    MYSQL_MOCK_PLUGIN_NATIVE,     /* annonce mysql_native_password */
    MYSQL_MOCK_PLUGIN_CACHING,    /* annonce caching_sha2_password (fast path) */
} MysqlMockPlugin;

typedef struct MysqlMockServer MysqlMockServer;

/* Demarre le mock. expected_password peut etre NULL si on n'attend pas d'auth. */
MysqlMockServer* mysql_mock_server_start(MysqlMockAuthMode mode,
                                          MysqlMockPlugin   plugin,
                                          const char*       expected_password);

int  mysql_mock_server_port    (const MysqlMockServer* m);
int  mysql_mock_server_is_ready(const MysqlMockServer* m);
void mysql_mock_server_join    (MysqlMockServer* m);
void mysql_mock_server_free    (MysqlMockServer* m);

/* Helpers d'inspection apres join. */
const char* mysql_mock_server_last_user (const MysqlMockServer* m);
const char* mysql_mock_server_last_db   (const MysqlMockServer* m);
const char* mysql_mock_server_last_query(const MysqlMockServer* m);
bool        mysql_mock_server_auth_token_matched(const MysqlMockServer* m);

/* Requetes preparees : SQL vu en COM_STMT_PREPARE + 1re valeur liee recue
 * en COM_STMT_EXECUTE (decodee en texte). Preuve testable que le binding est
 * server-side : la valeur arrive separee du template SQL (placeholders '?'),
 * jamais concatenee. "" si aucun PREPARE/EXECUTE recu. */
const char* mysql_mock_server_last_prepare_sql (const MysqlMockServer* m);
const char* mysql_mock_server_last_bind_param0 (const MysqlMockServer* m);

#endif
