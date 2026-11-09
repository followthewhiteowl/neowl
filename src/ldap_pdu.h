// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ldap_pdu_h
#define owl_ldap_pdu_h

/* ================================================================
 * LDAP PDU — encodage des requetes / parsing des reponses (RFC 4511)
 *
 * Couche au-dessus de ldap_ber : elle compose les protocolOp LDAP
 * (BindRequest, SearchRequest, UnbindRequest) en LDAPMessage complets
 * prets a emettre, et extrait des reponses serveur les informations
 * utiles au chemin lecture (resultCode, DN, attributs mono/multi-value).
 *
 * Perimetre MVP (cf. ) : simple bind, recherche recursive,
 * lecture d'attributs. Pas de SASL, pas de controls, pas de referrals.
 *
 * Deux contrats forts herites de ldap_ber :
 *   - UTF-8 brut partout : les LDAPString/LDAPDN sont transportees
 *     telles quelles, aucun re-encodage, la longueur compte les octets.
 *   - Entree serveur NON FIABLE : bornage strict (via mbedtls_asn1_get_*),
 *     rejet propre (false) des PDU malformees, JAMAIS d'abort().
 *
 * Le PARSING est zero-copie : les pointeurs exposes (DN, type d'attribut,
 * valeurs) pointent DANS le buffer source passe en entree. Celui-ci doit
 * rester valide tant que la structure de resultat est utilisee.
 *
 * References : RFC 4511 section 4.2 (Bind), 4.5.1 (SearchRequest),
 * 4.5.2 (SearchResultEntry/Done), 4.1.9 (LDAPResult).
 * ================================================================ */

#include "common.h"
#include "ldap_ber.h"

/* ================================================================
 * Tags protocolOp LDAP (RFC 4511 annexe B)
 *
 * Exposes pour que la couche socket (lot ulterieur) dispatche la
 * reponse sur OwlLdapMessageHeader.protocolOpTag.
 * ================================================================ */

#define OWL_LDAP_OP_BIND_REQUEST        0x60  /* [APPLICATION 0] constructed */
#define OWL_LDAP_OP_BIND_RESPONSE       0x61  /* [APPLICATION 1] */
#define OWL_LDAP_OP_UNBIND_REQUEST      0x42  /* [APPLICATION 2] primitif (NULL) */
#define OWL_LDAP_OP_SEARCH_REQUEST      0x63  /* [APPLICATION 3] constructed */
#define OWL_LDAP_OP_SEARCH_RESULT_ENTRY 0x64  /* [APPLICATION 4] constructed */
#define OWL_LDAP_OP_SEARCH_RESULT_DONE  0x65  /* [APPLICATION 5] constructed */

/* scope de SearchRequest (RFC 4511 section 4.5.1.2). */
typedef enum {
    OWL_LDAP_SCOPE_BASE_OBJECT   = 0,
    OWL_LDAP_SCOPE_SINGLE_LEVEL  = 1,
    OWL_LDAP_SCOPE_WHOLE_SUBTREE = 2,  /* defaut : LDAPRecherche est recursif */
} OwlLdapScope;

/* Quelques resultCode LDAP courants (RFC 4511 section 4.1.9).
 * La liste n'est pas exhaustive : le resultCode brut est toujours
 * expose tel quel, ces noms ne servent qu'a la lisibilite des tests. */
typedef enum {
    OWL_LDAP_SUCCESS                 = 0,
    OWL_LDAP_OPERATIONS_ERROR        = 1,
    OWL_LDAP_SASL_BIND_IN_PROGRESS   = 14,  /* bind SASL multi-etapes : tour suivant attendu */
    OWL_LDAP_INVALID_CREDENTIALS     = 49,
} OwlLdapResultCode;

/* ================================================================
 * Encodage des requetes
 *
 * Chaque fonction compose un LDAPMessage COMPLET (messageID + protocolOp,
 * enveloppe SEQUENCE comprise) dans le writer, pret a etre emis tel quel.
 * Precondition : writer fraichement initialise (vide). Renvoie false sur
 * OOM/overflow ; le writer passe alors en etat condamne.
 * ================================================================ */

/* BindRequest [APPLICATION 0].
 *   version INTEGER, name LDAPDN, authentication AuthenticationChoice.
 * version : numero de protocole encode dans le BindRequest (la validation
 * des versions acceptees est faite par l'appelant). dn : UTF-8 nul-termine
 * (NULL => "").
 *
 * sasl_mechanism :
 *   - NULL      => simple bind : authentication [0] simple = password (octets
 *                  bruts, peut etre vide pour un bind anonyme/non authentifie).
 *   - non NULL  => SASL bind : authentication [3] sasl SaslCredentials {
 *                  mechanism LDAPString = sasl_mechanism, credentials OMIS }.
 *                  Mode single-step (EXTERNAL) : aucun credential n'est porte
 *                  par le bind, l'identite vient du transport (cert client /
 *                  mTLS). password/password_len sont alors ignores. */
bool owl_ldap_encode_bind_request(OwlBerWriter* w, int message_id, int version,
                                  const char* dn,
                                  const unsigned char* password,
                                  size_t password_len,
                                  const char* sasl_mechanism);

/* BindRequest [APPLICATION 0] SASL generique (mecanisme + credentials).
 *   authentication [3] sasl SaslCredentials ::= SEQUENCE {
 *     mechanism LDAPString, credentials OCTET STRING OPTIONAL }.
 *
 * mechanism : nom du mecanisme SASL (ex. "GSS-SPNEGO", "EXTERNAL"), non NULL.
 * credentials :
 *   - NULL      => champ credentials OMIS (bind SASL single-step, type EXTERNAL
 *                  ou premier tour sans jeton) ;
 *   - non NULL  => credentials present (OCTET STRING, meme vide) : porte le
 *                  jeton d'un bind SASL multi-etapes (GSS-SPNEGO).
 * dn : LDAPDN name (NULL => "" ; vide pour un bind SASL).
 *
 * C'est la primitive generale ; owl_ldap_encode_bind_request y delegue pour
 * le cas EXTERNAL (credentials omis). */
bool owl_ldap_encode_bind_request_sasl(OwlBerWriter* w, int message_id,
                                       int version, const char* dn,
                                       const char* mechanism,
                                       const unsigned char* credentials,
                                       size_t credentials_len);

/* SearchRequest [APPLICATION 3].
 *   baseObject = base_dn, scope, derefAliases=neverDerefAliases(0),
 *   sizeLimit=0, timeLimit=0, typesOnly=FALSE, filter, attributes.
 *
 * Le FILTRE est fourni comme un bloc BER DEJA ENCODE opaque (produit par
 * la couche filtres) : (filter_ber, filter_len) est insere tel quel a sa
 * place. filter_ber NULL ou filter_len 0 => echec (le filtre est
 * obligatoire cote wire LDAP).
 *
 * attributes : tableau de attr_count noms d'attributs UTF-8 nul-termines
 * (attr_count 0 => SEQUENCE OF vide = "tous les attributs utilisateur"). */
bool owl_ldap_encode_search_request(OwlBerWriter* w, int message_id,
                                    const char* base_dn,
                                    int scope,
                                    const unsigned char* filter_ber,
                                    size_t filter_len,
                                    const char* const* attributes,
                                    size_t attr_count);

/* UnbindRequest [APPLICATION 2] NULL — contenu vide. */
bool owl_ldap_encode_unbind_request(OwlBerWriter* w, int message_id);

/* ================================================================
 * Parsing des reponses
 * ================================================================ */

/* LDAPResult : resultCode + diagnosticMessage + (pour un BindResponse SASL)
 * le serverSaslCreds. matchedDN / referral eventuels sont ignores.
 *
 * diagnosticMessage : message texte du serveur (RFC 4511 section 4.1.9).
 * Pointe DANS le buffer source (zero-copie). Porte souvent un sous-code
 * diagnostique cle (ex. AD : "data 52e" = identifiants invalides,
 * "data 80090346" = channel binding refuse). NULL/0 si absent ou vide.
 *
 * serverSaslCreds : champ [7] OCTET STRING OPTIONAL d'un BindResponse
 * (RFC 4511 section 4.2.2). Pointe DANS le buffer source (zero-copie),
 * valide tant que la source vit. NULL si absent (cas usuel : simple bind,
 * SearchResultDone, ou bind SASL sans jeton serveur). */
typedef struct {
    int                  resultCode;
    const unsigned char* diagnosticMessage;    /* [pointe DANS la source] */
    size_t               diagnosticMessageLen;
    const unsigned char* serverSaslCreds;
    size_t               serverSaslCredsLen;
} OwlLdapResult;

/* Lit le resultCode (premier champ ENUMERATED) d'un contenu de
 * BindResponse / SearchResultDone (= le opContent d'un LDAPMessage dont
 * le protocolOpTag vaut 0x61 ou 0x65). Extrait aussi, en best-effort, le
 * serverSaslCreds [7] s'il est present (bind SASL multi-etapes) ; son
 * absence n'est pas une erreur. Renvoie false seulement si le resultCode
 * lui-meme est illisible (PDU malformee). */
bool owl_ldap_parse_result(const unsigned char* content, size_t len,
                           OwlLdapResult* out);

/* Une valeur d'attribut : octets UTF-8 bruts, pointe DANS la source. */
typedef struct {
    const unsigned char* value;
    size_t               len;
} OwlLdapValue;

/* Un attribut : nom (type) + ses valeurs (SET OF), ordre serveur
 * preserve. type pointe DANS la source ; values est un tableau heap
 * possede par l'entree (libere par owl_ldap_search_entry_free). */
typedef struct {
    const unsigned char* type;
    size_t               type_len;
    OwlLdapValue*        values;
    size_t               value_count;
} OwlLdapAttribute;

/* Une SearchResultEntry : objectName (DN) + liste d'attributs.
 * dn et les octets des valeurs pointent DANS la source ; attributes et
 * les tableaux values sont heap (liberes par le free). */
typedef struct {
    const unsigned char* dn;
    size_t               dn_len;
    OwlLdapAttribute*    attributes;
    size_t               attribute_count;
} OwlLdapSearchEntry;

/* Parse un contenu de SearchResultEntry (= opContent d'un LDAPMessage
 * dont protocolOpTag vaut 0x64) en remplissant out. Les attributs a SET
 * vide sont rejetes. Sur succes, out doit etre libere par
 * owl_ldap_search_entry_free. Sur echec (false), out est deja nettoye. */
bool owl_ldap_parse_search_entry(const unsigned char* content, size_t len,
                                 OwlLdapSearchEntry* out);

/* Libere les tableaux heap d'une entree (jamais les octets source). */
void owl_ldap_search_entry_free(OwlLdapSearchEntry* entry);

/* ================================================================
 * Agregation d'une reponse de recherche
 *
 * Une reponse de recherche est une SEQUENCE sur le wire de N
 * SearchResultEntry (0x64) suivies d'un SearchResultDone (0x65). La
 * couche socket lit les LDAPMessage un a un, dispatche sur le
 * protocolOpTag, et accumule ici. Les octets source de CHAQUE entree
 * doivent rester valides jusqu'au free.
 * ================================================================ */

typedef struct {
    OwlLdapSearchEntry* entries;
    size_t              entry_count;
    size_t              entry_cap;    /* interne (croissance) */
    int                 resultCode;   /* du SearchResultDone ; -1 tant qu'absent */
    bool                done;         /* true quand le Done a ete integre */
} OwlLdapSearchResult;

void owl_ldap_search_result_init(OwlLdapSearchResult* r);

/* Parse un contenu de SearchResultEntry et l'ajoute. false si parse ou
 * allocation echoue (le resultat agrege reste coherent et liberable). */
bool owl_ldap_search_result_add_entry(OwlLdapSearchResult* r,
                                      const unsigned char* content, size_t len);

/* Integre le SearchResultDone : extrait resultCode, marque done. */
bool owl_ldap_search_result_set_done(OwlLdapSearchResult* r,
                                     const unsigned char* content, size_t len);

void owl_ldap_search_result_free(OwlLdapSearchResult* r);

#endif /* owl_ldap_pdu_h */
