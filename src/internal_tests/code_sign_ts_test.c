// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test coeur RFC 3161 (horodatage de signature, , tranche 1).
 *
 * Module sous test : src/code_sign.{c,h} :
 *   - owl_code_sign_tsreq_build()   : construit une TimeStampReq DER.
 *   - owl_code_sign_tsresp_parse()  : parse une TimeStampResp, extrait le
 *                                     timeStampToken (ContentInfo DER).
 *   - owl_code_sign_embed_tstoken() : embarque ce token comme attribut
 *                                     NON signe dans un SignerInfo PKCS#7.
 *   - owl_code_sign_tsa_post()      : transport HTTP de la TimeStampReq vers
 *                                     une TSA (tranche 2), teste contre un
 *                                     pair mock loopback (net_mock_peer).
 *
 * 100% offline : les fixtures ne requierent pas de reseau, et le test de
 * transport ecoute sur 127.0.0.1 (port ephemere) -- aucune sortie reelle.
 *   - tests/internal_fixtures/codesign/throwaway_codesign.pfx.b64 (deja
 *     utilisee par code_sign_test.c) : signer jetable pour produire un
 *     PKCS#7 a horodater.
 *   - tests/internal_fixtures/codesign/rfc3161_digicert_response.tsr.b64 :
 *     une VRAIE reponse TimeStampResp (DigiCert, status Granted, SHA-256),
 *     capturee hors ligne. Son messageImprint connu (SHA-256 de
 *     "owl-timestamp-fixture-content") sert d'oracle pour prouver que le
 *     parse descend bien jusqu'au contenu reel, pas juste "ca ne crash pas".
 *
 * MBEDTLS_ALLOW_PRIVATE_ACCESS est defini ICI (fichier de test uniquement)
 * pour parcourir a la main les octets ASN.1 produits/consommes par ces 3
 * fonctions : aucune des structures RFC 3161 (TimeStampReq, TimeStampResp,
 * MessageImprint) n'existe comme type mbedTLS de haut niveau -- on verifie
 * donc directement le DER avec les primitives mbedtls_asn1_get_*.
 *
 * Piege evite sur le cas "embed" (cf. doc en tete de mbedtls/pkcs7.h) :
 * mbedtls_pkcs7_parse_der() assume que les attributs non-authentifies du
 * SignerInfo sont VIDES ("it is assumed these fields are empty"). Un
 * SignerInfo porteur d'un unsignedAttrs [1] pourrait donc etre refuse par ce
 * parseur une fois le token embarque. Le test N'UTILISE DONC PAS
 * mbedtls_pkcs7_parse_der() sur le PKCS#7 APRES embed : il verifie par
 * recherche d'octets (TLV complet, pas une sous-chaine ambigue) que
 * l'encryptedDigest d'origine (extrait du PKCS#7 AVANT embed, lui bien
 * reparsable) est present tel quel dans le PKCS#7 APRES embed, et que l'OID
 * id-aa-timeStampToken + le token lui-meme y apparaissent aussi. C'est une
 * preuve plus robuste et independante des choix de tolerance du parseur
 * mbedTLS -- pas un raccourci pour eviter de verifier.
 */

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code_sign.h"
#include "net_mock_peer.h"   /* transport TSA (tranche 2) : pair HTTP loopback */

#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs7.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/asn1.h"
#include "mbedtls/oid.h"

static int g_pass = 0;
static int g_fail = 0;

#define CS_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

#define FIXTURE_PFX_PATH "tests/internal_fixtures/codesign/throwaway_codesign.pfx.b64"
#define FIXTURE_PFX_PASSWORD "owltest"
#define FIXTURE_TSRESP_PATH "tests/internal_fixtures/codesign/rfc3161_digicert_response.tsr.b64"

/* Contenu arbitraire, deterministe, signe pour produire le PKCS#7 de depart
 * du cas "embed" (independant du contenu signe par code_sign_test.c, mais
 * meme principe : fixe, reproductible). */
static const unsigned char CONTENU_A_SIGNER[24] = {
    'o','w','l','-','r','f','c','3','1','6','1',
    '-','e','m','b','e','d','-','t','e','s','t','!','!'
};

/* MessageImprint connu de la fixture TimeStampResp (cf.
 * tests/internal_fixtures/codesign/RFC3161_README.md) : SHA-256 des octets
 * ASCII "owl-timestamp-fixture-content". Sert d'oracle de non-tautologie :
 * prouve que le parse redescend jusqu'au vrai contenu de la vraie reponse,
 * pas juste jusqu'a une structure qui "a la bonne forme". */
static const unsigned char MESSAGE_IMPRINT_FIXTURE_HEX[32] = {
    0x77,0xaa,0xa3,0x23,0x05,0xd3,0x3e,0x61,0x27,0xa8,0xac,0x2f,
    0x37,0x5e,0x6c,0x8d,0x8d,0xb9,0x02,0x9d,0x09,0x45,0x12,0x9c,
    0x08,0x79,0xcd,0x98,0x4f,0xcd,0xc6,0x76
};

/* --------------------------------------------------------------------
 * Helpers de fixture / recherche d'octets
 * -------------------------------------------------------------------- */

/* Charge un fichier texte base64 (une ligne, eventuel newline final
 * tolere) et decode son contenu en DER brut. Renvoie 1 si OK (out_der a
 * liberer par free()), 0 sinon. Generique : sert pour le .pfx.b64 ET le
 * .tsr.b64, memes contraintes de format. */
static int load_b64_file_der(const char* path, uint8_t** out_der, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "  impossible d'ouvrir %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }

    unsigned char* b64 = (unsigned char*)malloc((size_t)sz);
    if (!b64) { fclose(f); return 0; }
    size_t rd = fread(b64, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(b64); return 0; }

    while (rd > 0 && (b64[rd - 1] == '\n' || b64[rd - 1] == '\r')) rd--;

    size_t der_len = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &der_len, b64, rd);
    if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        free(b64);
        return 0;
    }
    unsigned char* der = (unsigned char*)malloc(der_len);
    if (!der) { free(b64); return 0; }
    size_t written = 0;
    rc = mbedtls_base64_decode(der, der_len, &written, b64, rd);
    free(b64);
    if (rc != 0) { free(der); return 0; }

    *out_der = der;
    *out_len = written;
    return 1;
}

/* Recherche naive d'un motif binaire dans un buffer (memmem non portable
 * mingw, cf. precedent etabli dans xls_preservation_test.c). */
static int buf_find(const unsigned char* hay, size_t hlen,
                     const unsigned char* needle, size_t nlen,
                     size_t* out_pos) {
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) {
            if (out_pos) *out_pos = i;
            return 1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------
 * Cas 1 : construction d'une TimeStampReq sur une valeur de signature fixe.
 * Verifie version==1, hashAlgorithm==SHA-256, hashedMessage==SHA-256(valeur),
 * presence d'un nonce (INTEGER) et certReq==TRUE -- en reparsant le DER a
 * la main (pas de type TimeStampReq de haut niveau dans mbedTLS).
 * -------------------------------------------------------------------- */
static void test_tsreq_build_structure(void) {
    static const unsigned char SIG_VALUE[16] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
        0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10
    };

    uint8_t* req_der = NULL;
    size_t req_len = 0;
    OwlCodeSignStatus st = owl_code_sign_tsreq_build(SIG_VALUE, sizeof(SIG_VALUE), &req_der, &req_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK, "tsreq_build sur valeur de signature fixe -> OK");
    CS_CHECK(req_der != NULL && req_len > 0, "tsreq_build -> buffer DER non vide");
    if (!req_der) return;

    unsigned char expected_hash[32];
    int rc = mbedtls_sha256(SIG_VALUE, sizeof(SIG_VALUE), expected_hash, 0);
    CS_CHECK(rc == 0, "SHA-256 de la valeur de signature calcule par le test");

    unsigned char* p = req_der;
    unsigned char* end = req_der + req_len;
    size_t len = 0;

    /* TimeStampReq ::= SEQUENCE { version INTEGER(1), messageImprint, ... } */
    int tag_rc = mbedtls_asn1_get_tag(&p, end, &len,
                                       MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    CS_CHECK(tag_rc == 0, "TimeStampReq -> SEQUENCE top-level reconnue");
    if (tag_rc != 0) { free(req_der); return; }
    unsigned char* seq_end = p + len;

    int version = -1;
    int irc = mbedtls_asn1_get_int(&p, seq_end, &version);
    CS_CHECK(irc == 0, "TimeStampReq.version -> INTEGER lu");
    CS_CHECK(irc == 0 && version == 1, "TimeStampReq.version == 1");

    /* MessageImprint ::= SEQUENCE { hashAlgorithm AlgorithmIdentifier,
     *                               hashedMessage OCTET STRING } */
    size_t mi_len = 0;
    tag_rc = mbedtls_asn1_get_tag(&p, seq_end, &mi_len,
                                   MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    CS_CHECK(tag_rc == 0, "MessageImprint -> SEQUENCE reconnue");
    if (tag_rc != 0) { free(req_der); return; }
    unsigned char* mi_end = p + mi_len;

    mbedtls_asn1_buf alg_oid;
    memset(&alg_oid, 0, sizeof(alg_oid));
    int alg_rc = mbedtls_asn1_get_alg_null(&p, mi_end, &alg_oid);
    CS_CHECK(alg_rc == 0, "MessageImprint.hashAlgorithm -> AlgorithmIdentifier parsee");
    CS_CHECK(alg_rc == 0 && MBEDTLS_OID_CMP(MBEDTLS_OID_DIGEST_ALG_SHA256, &alg_oid) == 0,
             "MessageImprint.hashAlgorithm == OID SHA-256");

    size_t hashed_len = 0;
    int oct_rc = mbedtls_asn1_get_tag(&p, mi_end, &hashed_len, MBEDTLS_ASN1_OCTET_STRING);
    CS_CHECK(oct_rc == 0, "MessageImprint.hashedMessage -> OCTET STRING reconnue");
    CS_CHECK(oct_rc == 0 && hashed_len == 32, "MessageImprint.hashedMessage fait 32 octets");
    CS_CHECK(oct_rc == 0 && hashed_len == 32 && memcmp(p, expected_hash, 32) == 0,
             "MessageImprint.hashedMessage == SHA-256(valeur de signature)");
    if (oct_rc == 0) p += hashed_len;

    /* Champs restants de TimeStampReq : on s'attend a au moins un nonce
     * (INTEGER) et un certReq (BOOLEAN), dans un ordre ou l'autre selon
     * reqPolicy/extensions eventuellement absents. On scanne sans imposer
     * un ordre rigide au-dela de ce que le contrat ticket demande. */
    int found_nonce = 0;
    int found_certreq = 0;
    int certreq_val = 0;
    unsigned char* scan = p;
    while (scan < seq_end) {
        unsigned char* probe = scan;
        size_t flen = 0;
        if (mbedtls_asn1_get_tag(&probe, seq_end, &flen, MBEDTLS_ASN1_INTEGER) == 0) {
            found_nonce = 1;
            scan = probe + flen;
            continue;
        }
        probe = scan;
        int bval = 0;
        if (mbedtls_asn1_get_bool(&probe, seq_end, &bval) == 0) {
            found_certreq = 1;
            certreq_val = bval;
            scan = probe;
            continue;
        }
        /* Champ non reconnu par ce scan minimal (ex: reqPolicy OID,
         * extensions [1]) : hors du contrat verifie ici, on s'arrete. */
        break;
    }
    CS_CHECK(found_nonce, "TimeStampReq contient un champ nonce (INTEGER)");
    CS_CHECK(found_certreq, "TimeStampReq contient un champ certReq (BOOLEAN)");
    CS_CHECK(found_certreq && certreq_val != 0, "TimeStampReq.certReq == TRUE");

    free(req_der);
}

/* --------------------------------------------------------------------
 * Cas 2 : parse d'une VRAIE TimeStampResp (fixture DigiCert, status
 * Granted). Verifie le statut, l'extraction du timeStampToken (ContentInfo
 * SEQUENCE { OID signedData, [0] contenu }), et surtout que le
 * messageImprint embarque DANS la vraie reponse correspond au hash connu
 * de la fixture -- preuve anti-tautologie que le parse descend reellement
 * dans la structure et pas seulement dans une forme generique.
 * -------------------------------------------------------------------- */
static void test_tsresp_parse_fixture_granted(void) {
    uint8_t* resp_der = NULL;
    size_t resp_len = 0;
    int ok = load_b64_file_der(FIXTURE_TSRESP_PATH, &resp_der, &resp_len);
    CS_CHECK(ok, "fixture TimeStampResp .tsr.b64 chargee + decodee en DER");
    if (!ok) return;

    uint8_t* token_der = NULL;
    size_t token_len = 0;
    OwlCodeSignStatus st = owl_code_sign_tsresp_parse(resp_der, resp_len, &token_der, &token_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK, "tsresp_parse sur reponse DigiCert reelle -> OK (status granted)");
    CS_CHECK(token_der != NULL && token_len > 0, "tsresp_parse -> timeStampToken extrait, non vide");
    CS_CHECK(token_der == NULL || token_len < resp_len,
             "timeStampToken extrait est strictement plus petit que la reponse complete");

    if (token_der) {
        /* Le timeStampToken est un ContentInfo :
         * SEQUENCE { contentType OID (signedData), [0] EXPLICIT content } */
        unsigned char* p = token_der;
        unsigned char* end = token_der + token_len;
        size_t len = 0;
        int tag_rc = mbedtls_asn1_get_tag(&p, end, &len,
                                           MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
        CS_CHECK(tag_rc == 0, "timeStampToken -> SEQUENCE (ContentInfo) reconnue");
        if (tag_rc == 0) {
            unsigned char* ci_end = p + len;
            size_t oid_len = 0;
            int oid_rc = mbedtls_asn1_get_tag(&p, ci_end, &oid_len, MBEDTLS_ASN1_OID);
            CS_CHECK(oid_rc == 0, "ContentInfo.contentType -> OID reconnu");
            CS_CHECK(oid_rc == 0 && MBEDTLS_OID_CMP_RAW(MBEDTLS_OID_PKCS7_SIGNED_DATA, p, oid_len) == 0,
                     "ContentInfo.contentType == OID signedData (1.2.840.113549.1.7.2)");
        }

        /* Oracle anti-tautologie : le messageImprint connu de la fixture
         * (SHA-256 de "owl-timestamp-fixture-content") doit apparaitre tel
         * quel (32 octets contigus) dans le token extrait -- preuve que le
         * token pointe bien vers le contenu signe REEL de cette reponse,
         * pas vers un buffer vide ou tronque. */
        size_t pos = 0;
        int found = buf_find(token_der, token_len,
                              MESSAGE_IMPRINT_FIXTURE_HEX, sizeof(MESSAGE_IMPRINT_FIXTURE_HEX),
                              &pos);
        CS_CHECK(found,
                 "timeStampToken embarque le messageImprint connu de la fixture (SHA-256 du contenu horodate)");

        free(token_der);
    }

    free(resp_der);
}

/* --------------------------------------------------------------------
 * Cas 3 : embed du token comme attribut NON signe dans un SignerInfo
 * PKCS#7. Verifie :
 *   - presence de l'OID id-aa-timeStampToken dans le resultat (absente de
 *     l'entree, sanity anti-coincidence) ;
 *   - presence du token lui-meme dans le resultat ;
 *   - l'encryptedDigest (la signature) EXTRAITE DE L'ENTREE (reparsable,
 *     sans unsignedAttrs) est presente TELLE QUELLE dans la sortie --
 *     preuve valeur-exacte que l'attribut non-signe ne touche pas la
 *     signature.
 * -------------------------------------------------------------------- */
static void test_embed_tstoken_preserves_signature(void) {
    /* Deux OID d'attribut selon le chemin (fournis par le ticket, ne jamais
     * inventer un OID soi-meme) :
     *   - id-aa-timeStampToken   = 1.2.840.113549.1.9.16.2.14  (CMS detache)
     *   - szOID_RFC3161_counterSign = 1.3.6.1.4.1.311.3.3.1    (Authenticode) */
    static const unsigned char OID_TIMESTAMP_TOKEN[] = {
        0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x09,0x10,0x02,0x0e
    };
    static const unsigned char OID_RFC3161_COUNTER_SIGN[] = {
        0x2b,0x06,0x01,0x04,0x01,0x82,0x37,0x03,0x03,0x01
    };

    /* --- Prerequis : un signer + un PKCS#7 d'origine (sans horodatage). */
    uint8_t* pfx_der = NULL;
    size_t pfx_len = 0;
    int ok = load_b64_file_der(FIXTURE_PFX_PATH, &pfx_der, &pfx_len);
    CS_CHECK(ok, "fixture .pfx.b64 chargee + decodee en DER (prerequis embed)");
    if (!ok) return;

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx_der, pfx_len, FIXTURE_PFX_PASSWORD, &s);
    free(pfx_der);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis embed)");
    if (!s) return;

    uint8_t* p7_in = NULL;
    size_t p7_in_len = 0;
    st = owl_code_sign_pkcs7(s, CONTENU_A_SIGNER, sizeof(CONTENU_A_SIGNER), &p7_in, &p7_in_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK && p7_in != NULL && p7_in_len > 0,
             "pkcs7 d'origine construit (prerequis embed)");
    owl_code_sign_free(s);
    if (!p7_in) return;

    /* L'encryptedDigest d'origine : extrait en reparsant le PKCS#7 AVANT
     * embed (celui-la est garanti sans unsignedAttrs, donc reparsable par
     * mbedtls_pkcs7_parse_der sans ambiguite -- cf. note en tete de
     * fichier sur la tolerance du parseur). */
    mbedtls_pkcs7 pkcs7_in;
    mbedtls_pkcs7_init(&pkcs7_in);
    int type_in = mbedtls_pkcs7_parse_der(&pkcs7_in, p7_in, p7_in_len);
    CS_CHECK(type_in == MBEDTLS_PKCS7_SIGNED_DATA,
             "PKCS#7 d'origine reparse -> SIGNED_DATA (prerequis embed)");
    CS_CHECK(type_in == MBEDTLS_PKCS7_SIGNED_DATA && pkcs7_in.signed_data.no_of_signers == 1,
             "PKCS#7 d'origine -> exactement 1 signataire (prerequis embed)");

    unsigned char* orig_sig_p = NULL;
    size_t orig_sig_len = 0;
    if (type_in == MBEDTLS_PKCS7_SIGNED_DATA && pkcs7_in.signed_data.no_of_signers == 1) {
        orig_sig_p = pkcs7_in.signed_data.signers.MBEDTLS_PRIVATE(sig).p;
        orig_sig_len = pkcs7_in.signed_data.signers.MBEDTLS_PRIVATE(sig).len;
    }
    CS_CHECK(orig_sig_p != NULL && orig_sig_len > 0,
             "encryptedDigest d'origine extrait (prerequis embed)");

    /* --- Un token "plausible" a embarquer : on reutilise celui extrait de
     * la vraie fixture TimeStampResp (cas 2). Pas besoin qu'il soit lie au
     * PKCS#7 ci-dessus (embed est une operation structurelle, pas une
     * verification cryptographique de coherence temporelle) -- mais c'est
     * un VRAI ContentInfo SignedData DigiCert, pas un buffer bidon. */
    uint8_t* resp_der = NULL;
    size_t resp_len = 0;
    ok = load_b64_file_der(FIXTURE_TSRESP_PATH, &resp_der, &resp_len);
    CS_CHECK(ok, "fixture TimeStampResp chargee (prerequis embed, source du token)");
    uint8_t* token_der = NULL;
    size_t token_len = 0;
    if (ok) {
        st = owl_code_sign_tsresp_parse(resp_der, resp_len, &token_der, &token_len);
        CS_CHECK(st == OWL_CODE_SIGN_OK && token_der != NULL && token_len > 0,
                 "token extrait de la fixture TimeStampResp (prerequis embed)");
        free(resp_der);
    }

    if (orig_sig_p && token_der) {
        /* L'embed prend desormais l'OID de l'attribut en parametre : on
         * couvre les DEUX chemins (CMS detache et Authenticode). Chacun doit
         * embarquer SON OID et SON token, sans toucher a la signature. */
        struct { const unsigned char* oid; size_t oid_len; const char* label; } oids[] = {
            { OID_TIMESTAMP_TOKEN, sizeof(OID_TIMESTAMP_TOKEN), "id-aa-timeStampToken (CMS detache)" },
            { OID_RFC3161_COUNTER_SIGN, sizeof(OID_RFC3161_COUNTER_SIGN),
              "szOID_RFC3161_counterSign (Authenticode)" }
        };
        size_t oi;
        for (oi = 0; oi < sizeof(oids) / sizeof(oids[0]); ++oi) {
            uint8_t* p7_out = NULL;
            size_t p7_out_len = 0;
            st = owl_code_sign_embed_tstoken(p7_in, p7_in_len, token_der, token_len,
                                             oids[oi].oid, oids[oi].oid_len,
                                             &p7_out, &p7_out_len);
            CS_CHECK(st == OWL_CODE_SIGN_OK, "embed_tstoken -> OK");
            CS_CHECK(p7_out != NULL && p7_out_len > 0, "embed_tstoken -> buffer DER non vide");
            CS_CHECK(p7_out == NULL || p7_out_len > p7_in_len,
                     "PKCS#7 horodate est strictement plus grand que l'original (attribut ajoute)");

            if (p7_out) {
                /* OID demande : absent de l'entree (sanity anti-coincidence),
                 * present dans la sortie. */
                size_t pos = 0;
                int oid_in_input = buf_find(p7_in, p7_in_len,
                                             oids[oi].oid, oids[oi].oid_len, &pos);
                CS_CHECK(!oid_in_input,
                         "OID d'attribut ABSENT du PKCS#7 d'origine (sanity anti-coincidence)");

                int oid_in_output = buf_find(p7_out, p7_out_len,
                                              oids[oi].oid, oids[oi].oid_len, &pos);
                CS_CHECK(oid_in_output,
                         "OID d'attribut demande PRESENT dans le PKCS#7 horodate");

                /* Le token lui-meme doit apparaitre integralement dans la
                 * sortie (contenu embarque, pas tronque). */
                int token_in_output = buf_find(p7_out, p7_out_len, token_der, token_len, &pos);
                CS_CHECK(token_in_output,
                         "timeStampToken embarque integralement dans le PKCS#7 horodate");

                /* L'assertion centrale du cas 3 : l'encryptedDigest d'origine
                 * (valeur exacte, extraite du PKCS#7 AVANT embed) est present
                 * tel quel dans le PKCS#7 APRES embed. L'attribut non-signe ne
                 * doit PAS avoir touche a la signature. */
                int sig_in_output = buf_find(p7_out, p7_out_len, orig_sig_p, orig_sig_len, &pos);
                CS_CHECK(sig_in_output,
                         "encryptedDigest d'origine INCHANGE (meme valeur exacte) dans le PKCS#7 horodate");

                free(p7_out);
            }
        }
    }

    free(token_der);
    free(p7_in);
    mbedtls_pkcs7_free(&pkcs7_in);
}

/* --------------------------------------------------------------------
 * Cas 4 : robustesse sur statut != granted. DER minimal, structurellement
 * valide : TimeStampResp = SEQUENCE { PKIStatusInfo = SEQUENCE { status
 * INTEGER = 1 (rejection) } }, sans timeStampToken du tout (status en
 * echec, rien a extraire). Encode comme 30 05 30 03 02 01 01 : SEQUENCE
 * longueur 5 contenant une SEQUENCE longueur 3 contenant un INTEGER de
 * valeur 1. Doit echouer proprement, sans crash ; les sorties token_der
 * et token_len doivent rester respectivement NULL et 0.
 * -------------------------------------------------------------------- */
static void test_tsresp_parse_rejection_status(void) {
    static const unsigned char RESP_REJECTION[] = {
        0x30, 0x05,             /* SEQUENCE, len 5 : TimeStampResp */
        0x30, 0x03,             /*   SEQUENCE, len 3 : PKIStatusInfo */
        0x02, 0x01, 0x01        /*     INTEGER status = 1 (rejection) */
    };

    uint8_t* token_der = (uint8_t*)0x1; /* sentinelle non-NULL : doit redevenir NULL en cas d'echec propre */
    size_t token_len = 999;
    OwlCodeSignStatus st = owl_code_sign_tsresp_parse(RESP_REJECTION, sizeof(RESP_REJECTION),
                                                      &token_der, &token_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsresp_parse sur statut rejection -> statut != OK");
    CS_CHECK(token_der == NULL, "tsresp_parse sur statut rejection -> *token_der remis a NULL");
    CS_CHECK(token_len == 0, "tsresp_parse sur statut rejection -> *token_len remis a 0");

    if (token_der != NULL) free(token_der); /* ne doit jamais s'executer si le contrat est respecte */
}

/* Cas 5 (edge) : DER tronque / vide -> echec propre, pas de crash. Pousse
 * un peu plus loin la robustesse que le cas 4 (qui est structurellement
 * valide mais semantiquement en echec) : ici la structure elle-meme est
 * invalide. */
static void test_tsresp_parse_truncated_input(void) {
    static const unsigned char RESP_TRUNCATED[] = { 0x30, 0x7F }; /* longueur annoncee > donnees reelles */

    uint8_t* token_der = (uint8_t*)0x1;
    size_t token_len = 999;
    OwlCodeSignStatus st = owl_code_sign_tsresp_parse(RESP_TRUNCATED, sizeof(RESP_TRUNCATED),
                                                      &token_der, &token_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsresp_parse sur DER tronque -> statut != OK");
    CS_CHECK(token_der == NULL, "tsresp_parse sur DER tronque -> *token_der remis a NULL");
    CS_CHECK(token_len == 0, "tsresp_parse sur DER tronque -> *token_len remis a 0");

    if (token_der != NULL) free(token_der);

    /* Buffer vide : meme exigence. */
    token_der = (uint8_t*)0x1;
    token_len = 999;
    st = owl_code_sign_tsresp_parse(NULL, 0, &token_der, &token_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsresp_parse sur entree vide/NULL -> statut != OK");
    CS_CHECK(token_der == NULL, "tsresp_parse sur entree vide/NULL -> *token_der remis a NULL");

    if (token_der != NULL) free(token_der);
}

/* --------------------------------------------------------------------
 * Cas 6 (tranche 2) : transport HTTP de owl_code_sign_tsa_post contre un
 * pair mock loopback (net_mock_peer, role ECOUTE). 100% offline : le pair
 * ecoute sur 127.0.0.1, port ephemere -- aucune sortie reseau reelle.
 *
 * Le pair sert une reponse HTTP/1.1 200 dont le CORPS est le DER de la vraie
 * fixture TimeStampResp (meme octets que le cas 2). On verifie DEUX choses :
 *   1. que le POST emis est bien forme (ligne POST, Content-Type
 *      application/timestamp-query, Content-Length = taille de la req) --
 *      mesure sur les octets reellement recus par le pair ;
 *   2. que le corps DER de la reponse est extrait OCTET POUR OCTET (valeur
 *      exacte : memcmp contre la fixture) et reparsable en timeStampToken.
 * -------------------------------------------------------------------- */
static void test_tsa_post_transport_mock(void) {
    /* Corps de la reponse = DER de la fixture TimeStampResp (= un
     * application/timestamp-reply valide, cf. cas 2). */
    uint8_t* der = NULL;
    size_t der_len = 0;
    int ok = load_b64_file_der(FIXTURE_TSRESP_PATH, &der, &der_len);
    CS_CHECK(ok, "fixture TimeStampResp chargee (corps de la reponse HTTP du pair)");
    if (!ok) return;

    /* Reponse HTTP complete : entetes + corps DER (binaire, NUL inclus). */
    char header[256];
    int hn = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/timestamp-reply\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n", der_len);
    CS_CHECK(hn > 0 && (size_t)hn < sizeof(header), "entetes HTTP de reponse formees");
    size_t payload_len = (size_t)hn + der_len;
    unsigned char* payload = (unsigned char*)malloc(payload_len);
    CS_CHECK(payload != NULL, "buffer reponse HTTP alloue");
    if (!payload) { free(der); return; }
    memcpy(payload, header, (size_t)hn);
    memcpy(payload + hn, der, der_len);

    /* Pair mock : ECOUTE, lit la requete jusqu'a la fin des entetes puis emet
     * la reponse et ferme. */
    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets = (const char*)payload;
    cfg.octets_len = payload_len;
    cfg.decoupe = OWL_MOCK_DECOUPE_BLOC;
    cfg.sentinelle_lecture = "\r\n\r\n";
    cfg.timeout_lecture_ms = 4000;

    OwlMockPeer* peer = owl_mock_peer_demarre(&cfg);
    free(payload);
    CS_CHECK(peer != NULL, "pair mock TSA demarre (role ECOUTE)");
    if (!peer) { free(der); return; }
    int port = owl_mock_peer_port(peer);
    CS_CHECK(port > 0, "pair mock sur un port ephemere valide");

    /* Requete a poster : une vraie TimeStampReq sur une valeur de signature
     * fixe (le contenu importe peu pour un test de transport). */
    static const unsigned char SIG_VALUE[20] = {
        0xde,0xad,0xbe,0xef,0x00,0x11,0x22,0x33,0x44,0x55,
        0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    uint8_t* req = NULL;
    size_t req_len = 0;
    OwlCodeSignStatus st = owl_code_sign_tsreq_build(SIG_VALUE, sizeof(SIG_VALUE), &req, &req_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK && req != NULL, "TimeStampReq construite pour le POST");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);

    uint8_t* resp = NULL;
    size_t resp_len = 0;
    if (req) {
        st = owl_code_sign_tsa_post(url, req, req_len, &resp, &resp_len);
        CS_CHECK(st == OWL_CODE_SIGN_OK, "tsa_post contre le pair mock -> OK");
        CS_CHECK(resp != NULL && resp_len == der_len,
                 "corps DER extrait a la taille exacte de la fixture");
        CS_CHECK(resp != NULL && resp_len == der_len && memcmp(resp, der, der_len) == 0,
                 "corps DER extrait OCTET POUR OCTET identique a la fixture timestamp-reply");

        /* Le corps extrait est bien un TimeStampResp reparsable (round-trip). */
        if (resp) {
            uint8_t* token = NULL;
            size_t token_len = 0;
            OwlCodeSignStatus pst = owl_code_sign_tsresp_parse(resp, resp_len, &token, &token_len);
            CS_CHECK(pst == OWL_CODE_SIGN_OK && token != NULL && token_len > 0,
                     "corps DER extrait reparse en timeStampToken (round-trip transport->parse)");
            free(token);
        }
    }

    owl_mock_peer_joint(peer);

    /* Mesure du POST reellement emis (octets vus par le pair). */
    size_t recu_len = 0;
    const char* recu = owl_mock_peer_recu(peer, &recu_len);
    CS_CHECK(recu != NULL && strstr(recu, "POST / HTTP/1.1") != NULL,
             "le pair a recu une ligne de requete 'POST / HTTP/1.1'");
    CS_CHECK(recu != NULL && strstr(recu, "Content-Type: application/timestamp-query") != NULL,
             "le POST porte Content-Type: application/timestamp-query");
    CS_CHECK(recu != NULL && strstr(recu, "Host: 127.0.0.1") != NULL,
             "le POST porte l'entete Host");
    {
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu", req_len);
        CS_CHECK(recu != NULL && strstr(recu, cl) != NULL,
                 "le POST annonce Content-Length == taille exacte de la TimeStampReq");
    }

    owl_mock_peer_libere(peer);
    free(resp);
    free(req);
    free(der);
}

/* --------------------------------------------------------------------
 * Cas 7 (tranche 2) : rejets d'URL sans aucun reseau. https:// n'est pas
 * supporte en v1 (echec explicite), une URL non http:// est refusee. Les
 * sorties doivent rester NULL/0 (pas de buffer fuite ni laisse pendouiller).
 * -------------------------------------------------------------------- */
static void test_tsa_post_url_rejections(void) {
    static const unsigned char REQ[4] = { 0x30, 0x02, 0x05, 0x00 };
    uint8_t* resp = (uint8_t*)0x1;
    size_t resp_len = 999;

    OwlCodeSignStatus st = owl_code_sign_tsa_post("https://timestamp.example/", REQ, sizeof(REQ),
                                                  &resp, &resp_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsa_post sur https:// -> echec (non supporte en v1)");
    CS_CHECK(resp == NULL, "tsa_post sur https:// -> *resp_der remis a NULL");
    CS_CHECK(resp_len == 0, "tsa_post sur https:// -> *resp_len remis a 0");

    resp = (uint8_t*)0x1;
    resp_len = 999;
    st = owl_code_sign_tsa_post("ftp://nope/", REQ, sizeof(REQ), &resp, &resp_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsa_post sur schema non-http -> echec");
    CS_CHECK(resp == NULL, "tsa_post sur schema non-http -> *resp_der remis a NULL");

    resp = (uint8_t*)0x1;
    resp_len = 999;
    st = owl_code_sign_tsa_post(NULL, REQ, sizeof(REQ), &resp, &resp_len);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "tsa_post sur URL NULL -> echec");
    CS_CHECK(resp == NULL, "tsa_post sur URL NULL -> *resp_der remis a NULL");
}

int code_sign_ts_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_tsreq_build_structure();
    test_tsresp_parse_fixture_granted();
    test_embed_tstoken_preserves_signature();
    test_tsresp_parse_rejection_status();
    test_tsresp_parse_truncated_input();
    test_tsa_post_transport_mock();
    test_tsa_post_url_rejections();

    printf("code_sign_ts self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
