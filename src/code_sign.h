// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_code_sign_h
#define owl_code_sign_h

/* ================================================================
 * Coeur crypto — signature de code (PKCS#12 + PKCS#7 SignedData)
 *
 * Brique bas-niveau, 100% mbedTLS vendore, sans aucune dependance
 * externe. Deux operations :
 *
 *   1. owl_code_sign_load_pkcs12() : charge une cle privee + sa chaine
 *      de certificats depuis un conteneur PKCS#12 (.pfx) protege par
 *      mot de passe.
 *
 *   2. owl_code_sign_pkcs7() : produit une structure PKCS#7 SignedData
 *      (forme DETACHEE, sans attributs authentifies) signant le digest
 *      SHA-256 d'un contenu arbitraire avec cette cle.
 *
 * mbedTLS ne fournit pas de parseur PKCS#12 haut-niveau dans cette
 * version : le chargement descend la structure ASN.1 a la main et
 * s'appuie sur mbedtls_pkcs12_pbe_ext() pour les blocs PBE chiffres.
 * ================================================================ */

#include <stddef.h>
#include <stdint.h>

typedef struct OwlSigner OwlSigner; /* opaque : cle privee + chaine de certifs */

typedef enum {
    OWL_CODE_SIGN_OK = 0,
    OWL_CODE_SIGN_EBADARG, /* argument invalide (pointeur NULL, longueur nulle) */
    OWL_CODE_SIGN_EPKCS12, /* PKCS#12 illisible ou mot de passe errone */
    OWL_CODE_SIGN_ESIGN,   /* echec de la signature / construction PKCS#7 */
    OWL_CODE_SIGN_ENOMEM   /* allocation impossible */
} OwlCodeSignStatus;

/* Charge une cle privee et sa chaine de certificats depuis un PKCS#12
 * en DER. `password` peut etre NULL (equivaut a vide). En cas de succes,
 * *out recoit un OwlSigner a liberer par owl_code_sign_free(). En cas
 * d'echec, *out reste NULL et aucune ressource n'est laissee derriere. */
OwlCodeSignStatus owl_code_sign_load_pkcs12(const uint8_t* pfx_der, size_t pfx_len,
                                            const char* password, OwlSigner** out);

/* Produit un PKCS#7 SignedData DER (detache, sans attributs authentifies)
 * signant SHA-256(content). *p7_der recoit un buffer alloue a liberer par
 * free(), *p7_len sa longueur. */
OwlCodeSignStatus owl_code_sign_pkcs7(OwlSigner* s, const uint8_t* content, size_t content_len,
                                      uint8_t** p7_der, size_t* p7_len);

/* Signe un executable PE (Authenticode) : prend un PE non signe, renvoie via
 * *pe_out (a liberer par free()) une copie signee -- table de certificats
 * WIN_CERTIFICATE appondue, Security DataDirectory renseignee, CheckSum
 * recalcule. Le PKCS#7 embarque utilise la forme AVEC attributs authentifies
 * (contentType + messageDigest) et un SpcIndirectDataContent portant le hash
 * Authenticode SHA-256 du PE. En cas d'echec, *pe_out reste NULL. */
OwlCodeSignStatus owl_code_sign_pe(OwlSigner* s, const uint8_t* pe_in, size_t pe_in_len,
                                   uint8_t** pe_out, size_t* pe_out_len);

void owl_code_sign_free(OwlSigner* s);

/* ================================================================
 * Horodatage RFC 3161 (brique structurelle, sans reseau)
 *
 * Trois operations qui manipulent le DER a la main (aucune des
 * structures RFC 3161 n'a d'equivalent haut-niveau mbedTLS) :
 *
 *   - owl_code_sign_tsreq_build()   : construit une TimeStampReq DER
 *     horodatant SHA-256(sig_value), avec nonce aleatoire et
 *     certReq TRUE.
 *   - owl_code_sign_tsresp_parse()  : lit une TimeStampResp, verifie
 *     le statut (granted / grantedWithMods), extrait le
 *     timeStampToken (ContentInfo SignedData) en TLV complet.
 *   - owl_code_sign_embed_tstoken() : reinjecte ce token comme
 *     attribut NON signe dans le SignerInfo d'un PKCS#7 existant,
 *     sans toucher a la signature. L'OID de l'attribut est fourni
 *     par l'appelant (id-aa-timeStampToken pour le CMS detache,
 *     szOID_RFC3161_counterSign pour Authenticode).
 * ================================================================ */

/* TimeStampReq DER sur le digest SHA-256 de `sig_value` (la valeur de
 * signature a horodater). *req_der recoit un buffer alloue a liberer
 * par free(), *req_len sa longueur. *req_der reste NULL en cas d'echec. */
OwlCodeSignStatus owl_code_sign_tsreq_build(const uint8_t* sig_value, size_t sig_len,
                                            uint8_t** req_der, size_t* req_len);

/* Parse une TimeStampResp DER. Statut granted (0) ou grantedWithMods (1)
 * requis. En cas de succes *token_der recoit le timeStampToken (ContentInfo
 * DER, TLV complet, a liberer par free()). Sur tout echec (statut != granted,
 * DER invalide, token absent), *token_der est remis a NULL et *token_len a 0. */
OwlCodeSignStatus owl_code_sign_tsresp_parse(const uint8_t* resp_der, size_t resp_len,
                                             uint8_t** token_der, size_t* token_len);

/* Reinjecte `token_der` comme attribut NON signe dans le SignerInfo du PKCS#7
 * `p7_in`. `attr_oid`/`attr_oid_len` donnent l'OID de l'attribut : il DOIT etre
 * id-aa-timeStampToken (1.2.840.113549.1.9.16.2.14) pour un CMS detache, et
 * szOID_RFC3161_counterSign (1.3.6.1.4.1.311.3.3.1) pour Authenticode -- seul
 * ce dernier est reconnu par Windows sur le chemin PE. Le PKCS#7 est re-emis
 * integralement (toutes les longueurs englobantes recalculees) :
 * l'encryptedDigest d'origine n'est pas touche. *p7_out recoit un buffer alloue
 * a liberer par free(). *p7_out reste NULL en cas d'echec. */
OwlCodeSignStatus owl_code_sign_embed_tstoken(const uint8_t* p7_in, size_t p7_in_len,
                                              const uint8_t* token_der, size_t token_len,
                                              const uint8_t* attr_oid, size_t attr_oid_len,
                                              uint8_t** p7_out, size_t* p7_out_len);

/* ================================================================
 * Horodatage RFC 3161 — transport HTTP + orchestration
 *
 * Reseau atteint UNIQUEMENT quand l'appelant fournit une URL de TSA
 * (le CLI ne le fait que sur --timestamp explicite ; sans le flag,
 * aucune de ces fonctions n'est appelee et le build reste offline).
 * ================================================================ */

/* POST HTTP/1.1 d'une TimeStampReq DER vers une TSA RFC 3161. http:// seul en
 * v1 (https:// -> echec explicite). Content-Type application/timestamp-query,
 * Connection: close. En cas de succes *resp_der recoit le CORPS de la reponse
 * (le DER application/timestamp-reply, a liberer par free()), *resp_len sa
 * longueur. *resp_der reste NULL sur tout echec (URL invalide, connexion
 * refusee, HTTP != 200, corps vide, corps en transfer-encoding non gere).
 * owl_code_sign_last_tsa_error() detaille la cause. */
OwlCodeSignStatus owl_code_sign_tsa_post(const char* url,
                                         const uint8_t* req_der, size_t req_len,
                                         uint8_t** resp_der, size_t* resp_len);

/* Horodate un PKCS#7 SignedData existant : extrait l'encryptedDigest du
 * SignerInfo, construit la TimeStampReq, la POST a `tsa_url`, parse la reponse
 * et reinjecte le token comme attribut non signe. *p7_out (a liberer par
 * free()) recoit le PKCS#7 horodate ; NULL sur echec. Applicable aux formes
 * detachee et Authenticode. */
OwlCodeSignStatus owl_code_sign_timestamp_pkcs7(const char* tsa_url,
                                                const uint8_t* p7_in, size_t p7_in_len,
                                                uint8_t** p7_out, size_t* p7_out_len);

/* Variante horodatee de owl_code_sign_pe : le PKCS#7 Authenticode est horodate
 * (via `tsa_url`) AVANT son injection dans le PE, en une seule passe. *pe_out
 * reste NULL en cas d'echec. */
OwlCodeSignStatus owl_code_sign_pe_timestamped(OwlSigner* s,
                                               const uint8_t* pe_in, size_t pe_in_len,
                                               const char* tsa_url,
                                               uint8_t** pe_out, size_t* pe_out_len);

/* Derniere cause d'echec d'horodatage (transport ou orchestration), chaine
 * statique lisible pour un message CLI actionnable. Jamais NULL. */
const char* owl_code_sign_last_tsa_error(void);

#endif /* owl_code_sign_h */
