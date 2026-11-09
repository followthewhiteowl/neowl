// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_mock_server_h
#define owl_ldap_mock_server_h

/* ================================================================
 * Mini-serveur LDAP mock — pour tests CI deterministes.
 *
 * Modele sur ftp_mock_server, bati sur net_tcp_server (listener + accept
 * dans un thread). Implemente juste assez du cote serveur LDAPv3 (RFC
 * 4511) pour valider le client OwlLdapClient :
 *   - BindRequest  -> BindResponse : succes si (dn,password) == jeu de
 *     creds configure, sinon resultCode 49 (invalidCredentials).
 *   - SearchRequest -> une SearchResultEntry canned (DN + attributs mono
 *     ET multi-values) quand le baseObject est non vide, puis
 *     SearchResultDone(0). Le filtre est ignore (matching grossier : le
 *     but est de tester le client, pas de reimplementer un annuaire).
 *   - UnbindRequest -> ferme la connexion.
 *
 * Clair uniquement (389-like). Le transport TLS est deja prouve par le
 * self-test net_tcp_tls ; LDAPS est donc couvert indirectement.
 *
 * Lance en thread, port assigne dynamiquement (bind 0). Single-shot :
 * une connexion de controle, puis fin.
 *
 * Entree canned fixe (l'objet "bob") :
 *   DN              : CN=bob,OU=users,DC=example,DC=com
 *   sAMAccountName  : "bob"                              (mono)
 *   displayName     : "Bob Smith"                        (mono)
 *   memberOf        : "CN=Admins,DC=example,DC=com",
 *                     "CN=Users,DC=example,DC=com"       (multi, ordre serveur)
 * ================================================================ */

#include <stddef.h>

typedef struct LdapMockServer LdapMockServer;

typedef struct {
    const char* bind_dn;        /* DN attendu pour un bind reussi (NULL => "") */
    const char* bind_password;  /* mot de passe attendu (NULL => "") */
} LdapMockConfig;

/* Config neutre : creds vides (bind anonyme accepte). */
void ldap_mock_config_defaut(LdapMockConfig* cfg);

/* Demarre le thread serveur. Retourne NULL sur echec. Le port est
 * lisible des que ready == 1 (poller jusqu'a 2s). */
LdapMockServer* ldap_mock_server_start(const LdapMockConfig* cfg);

int  ldap_mock_server_port(const LdapMockServer* m);
int  ldap_mock_server_is_ready(const LdapMockServer* m);

/* Attend la fin du thread (le serveur traite UNE connexion puis sort). */
void ldap_mock_server_join(LdapMockServer* m);
void ldap_mock_server_free(LdapMockServer* m);

/* Inspection apres join (preuve du dialogue cote serveur). */
const char* ldap_mock_server_last_bind_dn(const LdapMockServer* m);
int         ldap_mock_server_saw_bind(const LdapMockServer* m);
int         ldap_mock_server_saw_unbind(const LdapMockServer* m);
int         ldap_mock_server_search_count(const LdapMockServer* m);
/* baseObject du dernier SearchRequest recu. */
const char* ldap_mock_server_last_base(const LdapMockServer* m);

#endif /* owl_ldap_mock_server_h */
