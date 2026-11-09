// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_negotiate_h
#define owl_ldap_negotiate_h

/* ================================================================
 * Backend d'authentification negociee (GSS-SPNEGO) pour LDAP.
 *
 * Fournit un OwlLdapTokenProvider (cf. ldap_client.h) adosse au
 * fournisseur de securite integre de l'OS :
 *   - Windows : SSPI (secur32, paquet "Negotiate") — credentials de la
 *     session ouverte, zero vendoring (import lib systeme, meme tier que
 *     ws2_32/bcrypt).
 *   - POSIX   : GSSAPI (mecanisme SPNEGO) via la bibliotheque systeme,
 *     chargee a l'execution (dlopen). Non-autonome par nature : exige un
 *     hote joint au domaine (krb5.conf + ticket cache). create() echoue
 *     proprement si la bibliotheque n'est pas disponible.
 *
 * Le contexte produit les jetons SPNEGO (init = premier jeton, step =
 * jeton suivant a partir du serverSaslCreds) et consomme le channel
 * binding tls-server-end-point transmis par la boucle SASL.
 *
 * C pur : aucune dependance VM/GC. La couche natives (authNegotiate) se
 * cable au-dessus.
 * ================================================================ */

#include "common.h"
#include "ldap_client.h"
#include <mbedtls/x509_crt.h>

/* Contexte Negotiate opaque (implementation specifique a la plateforme). */
typedef struct OwlLdapNegotiate OwlLdapNegotiate;

/* Cree un contexte Negotiate cible sur le service `spn` ("ldap/<fqdn>").
 * Acquiert les credentials de la session OS courante (SSPI) / prepare le
 * nom de service GSSAPI. Renvoie NULL + err rempli sur echec (fournisseur
 * OS indisponible, credentials inaccessibles). err peut etre NULL. */
OwlLdapNegotiate* owl_ldap_negotiate_create(const char* spn,
                                            char* err, size_t err_len);

/* Expose le contexte comme OwlLdapTokenProvider (init/step/self). Le
 * provider reste valide tant que le contexte n'est pas detruit. */
OwlLdapTokenProvider owl_ldap_negotiate_provider(OwlLdapNegotiate* n);

/* Derniere erreur du backend (jeton init/step en echec, etc.). */
const char* owl_ldap_negotiate_last_error(const OwlLdapNegotiate* n);

/* Libere le contexte (handles SSPI/GSSAPI, buffers de jetons). Idempotent
 * sur NULL. */
void owl_ldap_negotiate_destroy(OwlLdapNegotiate* n);

/* ================================================================
 * Channel binding tls-server-end-point (RFC 5929)
 *
 * Donnee a injecter dans la couche GSS/SSPI pour franchir un controleur
 * de domaine durci (LdapEnforceChannelBinding). Format :
 *   "tls-server-end-point:" || hash(DER du certificat serveur)
 * L'algorithme de hachage est celui de la signature du certificat, SAUF
 * MD5 / SHA-1 qui sont promus a SHA-256 (RFC 5929 section 4.1).
 *
 * peer_cert : certificat serveur (owl_tls_peer_cert). out : buffer de
 * sortie (prevoir >= 21 + 64 octets pour couvrir SHA-512). Renvoie true +
 * *out_len rempli, false si cert NULL / algo indisponible / buffer trop
 * petit.
 * ================================================================ */
bool owl_ldap_channel_binding_endpoint(const mbedtls_x509_crt* peer_cert,
                                       unsigned char* out, size_t out_cap,
                                       size_t* out_len);

#endif /* owl_ldap_negotiate_h */
