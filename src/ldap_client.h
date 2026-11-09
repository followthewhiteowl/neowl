// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_client_h
#define owl_ldap_client_h

/* ================================================================
 * LDAP client — orchestration reseau (RFC 4511, chemin lecture)
 *
 * Assemble les couches pures ldap_ber / ldap_pdu / ldap_filter au-dessus
 * du transport (net_tcp en clair sur 389, net_tcp_tls en LDAPS sur 636).
 * Une instance = une session LDAP : une connexion, un compteur de
 * messageID incremental, un resultat de recherche courant.
 *
 * Perimetre MVP (cf. ) : simple bind, recherche (scope
 * configurable), lecture d'attributs d'un objet. Pas de SASL, pas de
 * referrals, pas de paged results, pas de controls.
 *
 * ---- Contrat de duree de vie (zero-copie) ----
 * Le parsing PDU est zero-copie : les champs exposes d'une entree
 * (DN, type d'attribut, octets de valeur) pointent DANS un buffer de
 * reponse. Ce client POSSEDE ce buffer et le garde vivant tant que le
 * resultat courant est expose. Chaque nouvelle recherche (ou un reset,
 * ou la fermeture) libere le resultat precedent ET son buffer : tout
 * pointeur rendu par une recherche anterieure devient alors invalide.
 * Un appelant qui veut conserver une valeur au-dela doit la copier.
 *
 * C pur : aucune dependance VM/GC. La couche natives (LDAPSession et les
 * fonctions LDAPxxx) se cable au-dessus, dans un lot ulterieur.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"
#include "ldap_pdu.h"

typedef struct {
    bool          connected;
    bool          use_tls;
    OwlTcpSocket  tcp;            /* transport clair (389) */
    OwlTlsSocket  tls;            /* transport chiffre (LDAPS 636) */
    int           next_message_id;  /* compteur incremental (demarre a 1) */
    int           timeout_ms;    /* I/O socket (0/negatif => defaut OS) */
    char          last_error[256];

    /* Resultat de recherche courant + son buffer de reponse possede.
     * Les entrees de `result` pointent DANS `resp_buf` (zero-copie). */
    OwlLdapSearchResult result;
    bool          has_result;
    unsigned char* resp_buf;     /* copie stable des octets de reponse */
    size_t        resp_len;
    size_t        resp_cap;
} OwlLdapClient;

/* Initialise une session vide. A appeler avant tout autre op. */
void owl_ldap_client_init(OwlLdapClient* c);

/* ================================================================
 * Bind SASL multi-etapes (GSS-SPNEGO) — infrastructure backend-agnostique
 *
 * Un bind SASL negocie (GSS-SPNEGO) est un echange de jetons en plusieurs
 * tours : le client emet un BindRequest portant un jeton, le serveur repond
 * soit saslBindInProgress(14) + un serverSaslCreds a reinjecter, soit
 * success(0), soit une erreur. La generation des jetons est deleguee a un
 * fournisseur abstrait : la boucle ci-dessous ne connait ni Kerberos ni
 * SSPI/GSSAPI, elle orchestre juste le ping-pong.
 * ================================================================ */

/* Resultat d'une etape du fournisseur de jetons. */
typedef struct {
    bool                 ok;     /* false => erreur fatale du fournisseur */
    bool                 done;   /* true => plus aucun jeton a produire apres
                                    celui-ci (contexte de securite etabli) */
    const unsigned char* token;  /* jeton a emettre ; possede par le
                                    fournisseur, valide jusqu'au prochain
                                    appel. NULL/0 autorise (jeton vide). */
    size_t               token_len;
} OwlLdapSaslToken;

/* Fournisseur de jetons SASL, implemente par un backend (S2 : SSPI Windows,
 * GSSAPI POSIX ; S1 : mock de test). Backend-agnostique. */
typedef struct {
    /* Produit le premier jeton. channel_binding : donnee opaque
     * (tls-server-end-point, RFC 5929) transmise telle quelle au backend,
     * NULL/0 si absente. */
    OwlLdapSaslToken (*init)(void* self,
                            const unsigned char* channel_binding, size_t cb_len);
    /* Consomme le serverSaslCreds du tour precedent (server_token, qui peut
     * etre NULL/0) et produit le jeton suivant. */
    OwlLdapSaslToken (*step)(void* self,
                            const unsigned char* server_token,
                            size_t server_token_len);
    void* self;
} OwlLdapTokenProvider;

/* Ouvre le transport (TCP clair ou LDAPS) SANS effectuer de bind. Memes
 * parametres de connexion que owl_ldap_client_connect (host/port/TLS/certs).
 * Prerequis d'un bind SASL dont le channel binding derive du certificat
 * serveur (extrait apres le handshake). Renvoie false sur echec reseau/TLS
 * (last_error rempli, session fermee). */
bool owl_ldap_client_open(OwlLdapClient* c, const char* host, int port,
                          bool use_tls, int verify_level,
                          const char* extra_ca_path,
                          const char* client_cert_path,
                          const char* client_key_path,
                          int timeout_ms);

/* Execute un bind SASL multi-etapes sur un client DEJA connecte
 * (owl_ldap_client_open). mechanism : nom du mecanisme annonce
 * (ex. "GSS-SPNEGO"). provider : fournisseur de jetons. channel_binding :
 * donnee opaque passee au provider->init (NULL/0 si absente). version :
 * numero de protocole du BindRequest. Le nombre de tours est borne
 * (anti-boucle).
 *
 * out_result_code : resultCode final du BindResponse (0 = succes, sinon
 * erreur LDAP, ex. 49). Renvoie true quand l'echange a abouti a une reponse
 * serveur definitive — Y COMPRIS un bind refuse (out_result_code != 0).
 * Renvoie false seulement sur echec transport / PDU malformee / fournisseur
 * en erreur / depassement du nombre de tours (last_error rempli). */
bool owl_ldap_client_sasl_bind(OwlLdapClient* c, const char* mechanism,
                               OwlLdapTokenProvider* provider,
                               const unsigned char* channel_binding,
                               size_t cb_len,
                               int version, int* out_result_code);

/* Ouvre la session : connexion (TCP clair ou TLS) puis simple bind.
 *   host/port      : cible annuaire.
 *   use_tls        : true => LDAPS (owl_tls_connect), false => clair.
 *   verify_level   : OwlTlsVerifyLevel (ignore si !use_tls).
 *   extra_ca_path  : NULL/"" => bundle Mozilla ; sinon chemin PEM d'une CA
 *                    personnalisee, racine de confiance unique (ignore si
 *                    !use_tls ou verify_level == OWL_TLS_VERIFY_NONE).
 *   client_cert_path / client_key_path : cert client + cle a presenter au
 *                    handshake TLS (mTLS). NULL/"" => aucun. Requis pour un
 *                    bind SASL EXTERNAL (identite portee par le cert).
 *   dn/password    : identifiants du simple bind (dn NULL => "",
 *                    password peut etre vide pour un bind anonyme).
 *   sasl_external  : true => bind SASL EXTERNAL (name vide, aucun credential ;
 *                    l'identite vient du cert client) au lieu du simple bind.
 *   version        : numero de protocole encode dans le BindRequest.
 *   timeout_ms     : timeout connexion + I/O (0/negatif => defaut OS).
 *   out_result_code: resultCode du BindResponse (0 = succes,
 *                    49 = invalidCredentials). Non NULL requis.
 *
 * Renvoie true si la connexion, l'emission et la lecture du BindResponse
 * ont abouti — Y COMPRIS quand le bind est refuse (out_result_code=49).
 * Un bind refuse n'est PAS une erreur de transport. Renvoie false
 * seulement sur echec reseau / PDU malformee (last_error rempli) ; la
 * session reste alors fermee. */
bool owl_ldap_client_connect(OwlLdapClient* c, const char* host, int port,
                             bool use_tls, int verify_level,
                             const char* extra_ca_path,
                             const char* client_cert_path,
                             const char* client_key_path,
                             const char* dn,
                             const unsigned char* password, size_t password_len,
                             bool sasl_external,
                             int version,
                             int timeout_ms, int* out_result_code);

/* Recherche : compile filter_str (RFC 4515) en Filter BER, emet un
 * SearchRequest (baseObject=base_dn, scope, attributs demandes), puis lit
 * la suite de LDAPMessage de reponse (SearchResultEntry* puis
 * SearchResultDone). Le resultat precedent (et son buffer) est libere
 * d'abord.
 *
 * attributes : attr_count noms UTF-8 nul-termines (0 => tous les
 * attributs utilisateur).
 *
 * Renvoie le resultat agrege, POSSEDE par le client (valide jusqu'au
 * prochain search/read_object/reset/close), ou NULL sur echec
 * (last_error rempli). Le resultCode du Done est dans result->resultCode. */
const OwlLdapSearchResult* owl_ldap_client_search(
    OwlLdapClient* c, const char* base_dn, int scope,
    const char* filter_str,
    const char* const* attributes, size_t attr_count);

/* Lit les attributs de l'objet `dn` : recherche scope baseObject sur dn,
 * filtre present "(objectClass=*)", attributs demandes. C'est la
 * primitive derriere LDAPValeur/LDAPNbValeur (le lot natives y lira la
 * i-eme valeur d'un attribut de l'entree rendue). Memes regles de duree
 * de vie que owl_ldap_client_search. */
const OwlLdapSearchResult* owl_ldap_client_read_object(
    OwlLdapClient* c, const char* dn,
    const char* const* attributes, size_t attr_count);

/* Libere le resultat courant et son buffer (RAZ). La session reste
 * ouverte. Idempotent. */
void owl_ldap_client_reset(OwlLdapClient* c);

/* Ferme la session : UnbindRequest (best effort) + fermeture socket +
 * liberation du resultat/buffer. Idempotent. */
void owl_ldap_client_close(OwlLdapClient* c);

const char* owl_ldap_client_last_error(const OwlLdapClient* c);

#endif /* owl_ldap_client_h */
