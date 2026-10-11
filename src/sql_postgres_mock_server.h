// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sql_postgres_mock_server_h
#define owl_sql_postgres_mock_server_h

/* ================================================================
 * Mini-serveur PostgreSQL mock — pour tests CI deterministes
 *
 * Implemente une portion suffisante du wire protocol cote serveur
 * pour valider le client OwlPostgresConn :
 *   - StartupMessage -> AuthenticationOK / Cleartext / Error
 *   - ParameterStatus + BackendKeyData + ReadyForQuery
 *   - Simple Query : repond avec un script canon (RowDescription
 *     fixe "n int4" + DataRow "42" + CommandComplete "SELECT 1")
 *   - Terminate
 *
 * Lance en thread (pthread / CreateThread Win32). Port assigne
 * dynamiquement par l'OS (bind sur 0) → pas de conflit en CI.
 * Single-shot : accepte UNE connexion puis se termine.
 *
 * Pas SCRAM cote serveur (tres lourd à coder, ~500 lignes). Sera
 * teste contre un vrai PG en V2.1.
 * ================================================================ */

#include "common.h"

typedef enum {
    POSTGRES_MOCK_AUTH_OK,         /* AuthenticationOK direct (pas de password) */
    POSTGRES_MOCK_AUTH_CLEARTEXT,  /* AuthenticationCleartextPassword + valide  */
    POSTGRES_MOCK_AUTH_FAIL,       /* ErrorResponse "auth failed" */
} PostgresMockAuthMode;

typedef struct PostgresMockServer PostgresMockServer;

/* Lance le thread server. Retourne 0 si OK, -1 sinon.
 * Apres retour :
 *   m->port    : port TCP assigne (utiliser pour client connect)
 *   m->ready   : 1 quand listen() prêt (poller pendant up to 2s) */
PostgresMockServer* postgres_mock_server_start(PostgresMockAuthMode mode,
                                    const char* expected_password);

/* Recupere le port (apres start, lorsque ready). */
int  postgres_mock_server_port(const PostgresMockServer* m);
int  postgres_mock_server_is_ready(const PostgresMockServer* m);

/* Attend jusqu'a la fin du thread (le serveur traite UN client puis
 * exit). Apres ce retour, le serveur a fini son script. */
void postgres_mock_server_join(PostgresMockServer* m);

/* Libere les ressources. Doit etre appele apres join. */
void postgres_mock_server_free(PostgresMockServer* m);

/* Helpers d'inspection (apres join) :
 *   Last query recue, ou "" si pas de query.
 *   Last password recu (mode CLEARTEXT), ou "" sinon. */
const char* postgres_mock_server_last_query(const PostgresMockServer* m);
const char* postgres_mock_server_last_password(const PostgresMockServer* m);

/* Extended query helpers : sql parse + premier param text recus.
 * "" si pas de Parse/Bind recu. */
const char* postgres_mock_server_last_parse_sql(const PostgresMockServer* m);
const char* postgres_mock_server_last_bind_param0(const PostgresMockServer* m);

#endif
