// SPDX-License-Identifier: AGPL-3.0-only
/* Coeur crypto — signature de code. Voir code_sign.h pour le contrat.
 *
 * Reperes ASN.1 (les deux structures manipulees ici) :
 *
 * PKCS#12 (RFC 7292) :
 *   PFX ::= SEQUENCE { version INTEGER(3), authSafe ContentInfo, macData OPTIONAL }
 *   authSafe = ContentInfo(data) dont l'OCTET STRING contient
 *     AuthenticatedSafe ::= SEQUENCE OF ContentInfo
 *   Chaque ContentInfo est soit `data` (SafeContents en clair) soit
 *   `encryptedData` (SafeContents chiffre PBE).
 *   SafeContents ::= SEQUENCE OF SafeBag ; bags utiles :
 *     pkcs8ShroudedKeyBag -> EncryptedPrivateKeyInfo (PBE) -> PKCS#8 -> pk_parse_key
 *     certBag             -> CertBag -> OCTET STRING -> x509_crt_parse_der
 *
 * PKCS#7 SignedData (RFC 2315) produit ici — forme DETACHEE, sans
 * authenticatedAttributes : le SignerInfo porte directement la signature
 * RSA PKCS#1 v1.5 de SHA-256(contenu), le contentInfo interne (type data)
 * n'embarque aucun eContent (le verifieur recoit le contenu de l'exterieur).
 */

#include "code_sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_tcp.h"   /* owl_code_sign_tsa_post : transport HTTP vers la TSA */

#include "mbedtls/asn1.h"
#include "mbedtls/asn1write.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs12.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

struct OwlSigner {
    mbedtls_pk_context       pk;   /* cle privee du signataire */
    mbedtls_x509_crt         cert; /* chaine, premier = certif du signataire */
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    int                      have_key;
    int                      have_cert;
};

/* OIDs de bags PKCS#12 absents de oid.h : on les compare en brut. */
static const unsigned char OID_PKCS8_SHROUDED_KEY_BAG[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x02
};
static const unsigned char OID_KEY_BAG[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x01
};
static const unsigned char OID_CERT_BAG[] = {
    0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x0C, 0x0A, 0x01, 0x03
};

#define CONTEXT0 (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED)

static int oid_eq(const unsigned char* p, size_t len,
                  const unsigned char* oid, size_t oid_len) {
    return len == oid_len && memcmp(p, oid, oid_len) == 0;
}

static int oid_eq_str(const unsigned char* p, size_t len, const char* oid, size_t oid_len) {
    return len == oid_len && memcmp(p, oid, oid_len) == 0;
}

/* --- Dechiffrement PBE d'un bloc PKCS#12 --------------------------------
 * `p` pointe sur l'AlgorithmIdentifier PBE (SEQUENCE { OID, PbeParams }),
 * suivi dans la structure appelante par les donnees chiffrees. Lit l'algo,
 * puis dechiffre [enc, enc_len] -> buffer frais (*out / *out_len). */
static int pbe_decrypt(unsigned char** p, unsigned char* end_alg,
                       const unsigned char* enc, size_t enc_len,
                       const char* pwd, size_t pwdlen,
                       unsigned char** out, size_t* out_len) {
    size_t len;
    mbedtls_asn1_buf oid_buf;
    mbedtls_asn1_buf pbe_params;
    mbedtls_md_type_t md;
    mbedtls_cipher_type_t cipher;
    unsigned char* buf;
    int ret;

    if (mbedtls_asn1_get_tag(p, end_alg, &len, MBEDTLS_ASN1_OID) != 0) {
        return -1;
    }
    oid_buf.tag = MBEDTLS_ASN1_OID;
    oid_buf.p = *p;
    oid_buf.len = len;
    *p += len;
    if (mbedtls_oid_get_pkcs12_pbe_alg(&oid_buf, &md, &cipher) != 0) {
        return -1;
    }
    /* PbeParams ::= SEQUENCE { salt OCTET STRING, iterations INTEGER }.
     * mbedtls_pkcs12_pbe_ext veut un asn1_buf pointant sur le CONTENU de
     * ce SEQUENCE (le tag salt en tete). */
    if (mbedtls_asn1_get_tag(p, end_alg, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    pbe_params.tag = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
    pbe_params.p = *p;
    pbe_params.len = len;
    *p += len;

    buf = (unsigned char*)malloc(enc_len ? enc_len : 1);
    if (!buf) {
        return MBEDTLS_ERR_PKCS12_BAD_INPUT_DATA; /* traite comme echec PBE */
    }
    ret = mbedtls_pkcs12_pbe_ext(&pbe_params, MBEDTLS_PKCS12_PBE_DECRYPT,
                                 cipher, md,
                                 (const unsigned char*)pwd, pwdlen,
                                 enc, enc_len, buf, enc_len, out_len);
    if (ret != 0) {
        /* Mot de passe errone ou PBE non supporte : padding PKCS#7 invalide. */
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

/* SafeContents / bags : declare avant usage mutuel. */
static int parse_safe_contents(OwlSigner* s, unsigned char* p, unsigned char* end,
                               const char* pwd, size_t pwdlen);

/* Reconstruit un PrivateKeyInfo PKCS#8 sans son champ optionnel
 * `attributes [0]`. Les exports Windows (New-SelfSignedCertificate) ajoutent
 * ces attributs apres l'OCTET STRING privateKey ; or le parseur DER de
 * mbedTLS exige que l'OCTET STRING soit le dernier element (sinon
 * LENGTH_MISMATCH -> KEY_INVALID_FORMAT). On garde seulement
 * { version, algorithm, privateKey } et on re-enveloppe en SEQUENCE. */
static int sanitize_pkcs8(const unsigned char* key, size_t key_len,
                          unsigned char** out, size_t* out_len) {
    unsigned char* p = (unsigned char*)key;
    unsigned char* end = p + key_len;
    unsigned char* seq_end;
    unsigned char* inner_start;
    size_t len;
    size_t inner_len;
    unsigned char* buf;
    unsigned char* wp;
    unsigned char* start;
    size_t total = 0;
    int ret;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    seq_end = p + len;
    inner_start = p; /* version || algorithm || privateKey sont contigus */
    /* version INTEGER */
    if (mbedtls_asn1_get_tag(&p, seq_end, &len, MBEDTLS_ASN1_INTEGER) != 0) {
        return -1;
    }
    p += len;
    /* privateKeyAlgorithm SEQUENCE */
    if (mbedtls_asn1_get_tag(&p, seq_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    p += len;
    /* privateKey OCTET STRING */
    if (mbedtls_asn1_get_tag(&p, seq_end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -1;
    }
    p += len;
    inner_len = (size_t)(p - inner_start); /* exclut d'eventuels attributs [0] */

    buf = (unsigned char*)malloc(inner_len + 16);
    if (!buf) {
        return -1;
    }
    start = buf;
    wp = buf + inner_len + 16;
    ret = mbedtls_asn1_write_raw_buffer(&wp, start, inner_start, inner_len);
    if (ret < 0) { free(buf); return -1; }
    total += (size_t)ret;
    ret = mbedtls_asn1_write_len(&wp, start, total);
    if (ret < 0) { free(buf); return -1; }
    total += (size_t)ret;
    ret = mbedtls_asn1_write_tag(&wp, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (ret < 0) { free(buf); return -1; }
    total += (size_t)ret;

    *out = (unsigned char*)malloc(total);
    if (!*out) { free(buf); return -1; }
    memcpy(*out, wp, total);
    *out_len = total;
    free(buf);
    return 0;
}

/* Parse une cle PKCS#8 (clair) dans s->pk, en neutralisant d'abord les
 * attributs optionnels que mbedTLS refuse. */
static int load_pkcs8_key(OwlSigner* s, const unsigned char* der, size_t der_len) {
    unsigned char* clean = NULL;
    size_t clean_len = 0;
    int ret;

    if (sanitize_pkcs8(der, der_len, &clean, &clean_len) != 0) {
        return -1;
    }
    ret = mbedtls_pk_parse_key(&s->pk, clean, clean_len, NULL, 0,
                               mbedtls_ctr_drbg_random, &s->drbg);
    free(clean);
    if (ret != 0) {
        return -1;
    }
    s->have_key = 1;
    return 0;
}

/* EncryptedPrivateKeyInfo ::= SEQUENCE { AlgorithmIdentifier, OCTET STRING } */
static int parse_shrouded_key(OwlSigner* s, unsigned char* p, unsigned char* end,
                              const char* pwd, size_t pwdlen) {
    size_t len;
    unsigned char* epki_end;
    unsigned char* alg_oid;  /* position de l'OID dans l'AlgorithmIdentifier */
    unsigned char* alg_end;  /* fin du contenu de l'AlgorithmIdentifier */
    unsigned char* key_plain = NULL;
    size_t key_len = 0;
    int ret;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    epki_end = p + len;
    /* AlgorithmIdentifier SEQUENCE : on retient la position de l'OID et la
     * fin de l'algo, puis on saute a l'OCTET STRING des donnees chiffrees. */
    if (mbedtls_asn1_get_tag(&p, epki_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    alg_oid = p;
    alg_end = p + len;
    p = alg_end;
    if (mbedtls_asn1_get_tag(&p, epki_end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -1;
    }
    {
        unsigned char* alg = alg_oid;
        ret = pbe_decrypt(&alg, alg_end, p, len, pwd, pwdlen, &key_plain, &key_len);
    }
    if (ret != 0) {
        return ret;
    }
    ret = load_pkcs8_key(s, key_plain, key_len);
    free(key_plain);
    return ret;
}

/* CertBag ::= SEQUENCE { certId OID, certValue [0] EXPLICIT } ; certValue
 * (pour x509Certificate) est un OCTET STRING contenant le cert DER. On recoit
 * le contenu du bagValue [0], soit la SEQUENCE CertBag elle-meme. */
static int parse_cert_bag(OwlSigner* s, unsigned char* p, unsigned char* end) {
    size_t len;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OID) != 0) {
        return -1;
    }
    p += len; /* certId : on suppose x509Certificate */
    if (mbedtls_asn1_get_tag(&p, end, &len, CONTEXT0) != 0) {
        return -1;
    }
    if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -1;
    }
    /* Append a la chaine (le premier reste le signataire). */
    if (mbedtls_x509_crt_parse_der(&s->cert, p, len) != 0) {
        return -1;
    }
    s->have_cert = 1;
    return 0;
}

/* SafeBag ::= SEQUENCE { bagId OID, bagValue [0] EXPLICIT, attrs SET OPTIONAL } */
static int parse_safe_bag(OwlSigner* s, unsigned char** pp, unsigned char* end,
                          const char* pwd, size_t pwdlen) {
    unsigned char* p = *pp;
    size_t len;
    unsigned char* end_bag;
    const unsigned char* oid;
    size_t oid_len;
    int ret = 0;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    end_bag = p + len;
    if (mbedtls_asn1_get_tag(&p, end_bag, &len, MBEDTLS_ASN1_OID) != 0) {
        return -1;
    }
    oid = p;
    oid_len = len;
    p += len;
    if (mbedtls_asn1_get_tag(&p, end_bag, &len, CONTEXT0) != 0) {
        return -1;
    }
    /* p..p+len = bagValue */
    if (oid_eq(oid, oid_len, OID_PKCS8_SHROUDED_KEY_BAG, sizeof(OID_PKCS8_SHROUDED_KEY_BAG))) {
        ret = parse_shrouded_key(s, p, p + len, pwd, pwdlen);
    } else if (oid_eq(oid, oid_len, OID_KEY_BAG, sizeof(OID_KEY_BAG))) {
        /* PKCS#8 en clair */
        ret = load_pkcs8_key(s, p, len);
    } else if (oid_eq(oid, oid_len, OID_CERT_BAG, sizeof(OID_CERT_BAG))) {
        ret = parse_cert_bag(s, p, p + len);
    }
    /* bag inconnu : ignore silencieusement */

    *pp = end_bag; /* saute d'eventuels bagAttributes */
    return ret;
}

static int parse_safe_contents(OwlSigner* s, unsigned char* p, unsigned char* end,
                               const char* pwd, size_t pwdlen) {
    size_t len;
    unsigned char* end_seq;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    end_seq = p + len;
    while (p < end_seq) {
        int ret = parse_safe_bag(s, &p, end_seq, pwd, pwdlen);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

/* ContentInfo de type encryptedData : dechiffre la SafeContents puis la parse.
 * EncryptedData ::= SEQUENCE { version INTEGER(0),
 *   EncryptedContentInfo ::= SEQUENCE { contentType OID,
 *     contentEncryptionAlgorithm AlgorithmIdentifier(PBE),
 *     encryptedContent [0] IMPLICIT OCTET STRING } } */
static int parse_encrypted_data(OwlSigner* s, unsigned char* p, unsigned char* end,
                                const char* pwd, size_t pwdlen) {
    size_t len;
    int ver;
    unsigned char* end_eci;
    unsigned char* alg_oid;
    unsigned char* alg_end;
    const unsigned char* enc;
    size_t enc_len;
    unsigned char* plain = NULL;
    size_t plain_len = 0;
    int ret;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    if (mbedtls_asn1_get_int(&p, end, &ver) != 0) {
        return -1;
    }
    (void)ver;
    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    end_eci = p + len;
    if (mbedtls_asn1_get_tag(&p, end_eci, &len, MBEDTLS_ASN1_OID) != 0) {
        return -1; /* contentType = data */
    }
    p += len;
    /* contentEncryptionAlgorithm : AlgorithmIdentifier (SEQUENCE). */
    if (mbedtls_asn1_get_tag(&p, end_eci, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    alg_oid = p;
    alg_end = p + len;
    p = alg_end;
    /* encryptedContent [0] IMPLICIT (primitif) OCTET STRING. */
    if (mbedtls_asn1_get_tag(&p, end_eci, &len,
                             MBEDTLS_ASN1_CONTEXT_SPECIFIC | 0) != 0) {
        return -1;
    }
    enc = p;
    enc_len = len;
    {
        unsigned char* alg = alg_oid;
        ret = pbe_decrypt(&alg, alg_end, enc, enc_len, pwd, pwdlen, &plain, &plain_len);
    }
    if (ret != 0) {
        return ret;
    }
    ret = parse_safe_contents(s, plain, plain + plain_len, pwd, pwdlen);
    free(plain);
    return ret;
}

/* AuthenticatedSafe ::= SEQUENCE OF ContentInfo */
static int parse_authsafe(OwlSigner* s, unsigned char* p, unsigned char* end,
                          const char* pwd, size_t pwdlen) {
    size_t len;
    unsigned char* end_seq;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    end_seq = p + len;
    while (p < end_seq) {
        unsigned char* end_ci;
        const unsigned char* oid;
        size_t oid_len;

        if (mbedtls_asn1_get_tag(&p, end_seq, &len,
                                 MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
            return -1;
        }
        end_ci = p + len;
        if (mbedtls_asn1_get_tag(&p, end_ci, &len, MBEDTLS_ASN1_OID) != 0) {
            return -1;
        }
        oid = p;
        oid_len = len;
        p += len;
        /* content [0] EXPLICIT */
        if (mbedtls_asn1_get_tag(&p, end_ci, &len, CONTEXT0) != 0) {
            return -1;
        }
        if (oid_eq_str(oid, oid_len, MBEDTLS_OID_PKCS7_DATA,
                       sizeof(MBEDTLS_OID_PKCS7_DATA) - 1)) {
            size_t olen;
            unsigned char* q = p;
            if (mbedtls_asn1_get_tag(&q, end_ci, &olen, MBEDTLS_ASN1_OCTET_STRING) != 0) {
                return -1;
            }
            if (parse_safe_contents(s, q, q + olen, pwd, pwdlen) != 0) {
                return -1;
            }
        } else if (oid_eq_str(oid, oid_len, MBEDTLS_OID_PKCS7_ENCRYPTED_DATA,
                              sizeof(MBEDTLS_OID_PKCS7_ENCRYPTED_DATA) - 1)) {
            int ret = parse_encrypted_data(s, p, end_ci, pwd, pwdlen);
            if (ret != 0) {
                return ret;
            }
        }
        p = end_ci;
    }
    return 0;
}

/* PFX ::= SEQUENCE { version INTEGER(3), authSafe ContentInfo, macData OPTIONAL } */
static int parse_pfx(OwlSigner* s, const unsigned char* der, size_t der_len,
                     const char* pwd, size_t pwdlen) {
    unsigned char* p = (unsigned char*)der;
    unsigned char* end = p + der_len;
    size_t len;
    int ver;
    unsigned char* end_ci;
    const unsigned char* oid;
    size_t oid_len;
    unsigned char* authsafe;
    size_t authsafe_len;

    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    if (mbedtls_asn1_get_int(&p, end, &ver) != 0) {
        return -1;
    }
    (void)ver;
    /* authSafe ContentInfo(data) */
    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return -1;
    }
    end_ci = p + len;
    if (mbedtls_asn1_get_tag(&p, end_ci, &len, MBEDTLS_ASN1_OID) != 0) {
        return -1;
    }
    oid = p;
    oid_len = len;
    p += len;
    if (!oid_eq_str(oid, oid_len, MBEDTLS_OID_PKCS7_DATA,
                    sizeof(MBEDTLS_OID_PKCS7_DATA) - 1)) {
        return -1;
    }
    if (mbedtls_asn1_get_tag(&p, end_ci, &len, CONTEXT0) != 0) {
        return -1;
    }
    if (mbedtls_asn1_get_tag(&p, end_ci, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) {
        return -1;
    }
    authsafe = p;
    authsafe_len = len;
    return parse_authsafe(s, authsafe, authsafe + authsafe_len, pwd, pwdlen);
}

OwlCodeSignStatus owl_code_sign_load_pkcs12(const uint8_t* pfx_der, size_t pfx_len,
                                            const char* password, OwlSigner** out) {
    OwlSigner* s;
    const char* pwd;
    size_t pwdlen;
    int ret;

    if (!out) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *out = NULL;
    if (!pfx_der || pfx_len == 0) {
        return OWL_CODE_SIGN_EBADARG;
    }
    pwd = password ? password : "";
    pwdlen = strlen(pwd);

    s = (OwlSigner*)calloc(1, sizeof(*s));
    if (!s) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    mbedtls_pk_init(&s->pk);
    mbedtls_x509_crt_init(&s->cert);
    mbedtls_entropy_init(&s->ent);
    mbedtls_ctr_drbg_init(&s->drbg);
    if (mbedtls_ctr_drbg_seed(&s->drbg, mbedtls_entropy_func, &s->ent,
                              (const unsigned char*)"owl-code-sign", 13) != 0) {
        owl_code_sign_free(s);
        return OWL_CODE_SIGN_EPKCS12;
    }

    ret = parse_pfx(s, pfx_der, pfx_len, pwd, pwdlen);
    if (ret != 0 || !s->have_key || !s->have_cert) {
        owl_code_sign_free(s);
        return OWL_CODE_SIGN_EPKCS12;
    }

    *out = s;
    return OWL_CODE_SIGN_OK;
}

void owl_code_sign_free(OwlSigner* s) {
    if (!s) {
        return;
    }
    mbedtls_pk_free(&s->pk);
    mbedtls_x509_crt_free(&s->cert);
    mbedtls_ctr_drbg_free(&s->drbg);
    mbedtls_entropy_free(&s->ent);
    free(s);
}

/* ------------------------------------------------------------------------
 * Construction PKCS#7 SignedData (ecriture ASN.1 a rebours : les helpers
 * mbedtls_asn1_write_* ecrivent du haut du buffer vers le bas, donc on emet
 * les champs dans l'ordre inverse de leur apparition).
 * ---------------------------------------------------------------------- */
OwlCodeSignStatus owl_code_sign_pkcs7(OwlSigner* s, const uint8_t* content, size_t content_len,
                                      uint8_t** p7_der, size_t* p7_len) {
    unsigned char digest[32];
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    unsigned char* buf = NULL;
    size_t buf_size;
    unsigned char* p;
    unsigned char* start;
    size_t len = 0;
    size_t si_len = 0;
    size_t field_len = 0;
    mbedtls_x509_crt* cert;
    int ret;
    unsigned char* result;

    if (!s || !content || !p7_der || !p7_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *p7_der = NULL;
    *p7_len = 0;
    cert = &s->cert;

    if (mbedtls_sha256(content, content_len, digest, 0) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    /* Signature RSA PKCS#1 v1.5 sur le digest SHA-256 du contenu (detache). */
    if (mbedtls_pk_sign(&s->pk, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                        sig, sizeof(sig), &sig_len,
                        mbedtls_ctr_drbg_random, &s->drbg) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }

    /* Buffer large : cert (<= qq Ko) + signature + enveloppe. */
    buf_size = cert->raw.len + sig_len + 1024;
    buf = (unsigned char*)malloc(buf_size);
    if (!buf) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    start = buf;
    p = buf + buf_size;

#define CHK(f) do { if ((ret = (f)) < 0) { goto fail; } } while (0)

    /* ---- SignerInfos ::= SET OF SignerInfo (un seul) ---- */
    /* encryptedDigest OCTET STRING = signature */
    CHK(mbedtls_asn1_write_octet_string(&p, start, sig, sig_len));
    si_len = (size_t)ret;
    /* digestEncryptionAlgorithm = rsaEncryption, NULL */
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_PKCS1_RSA,
                                                sizeof(MBEDTLS_OID_PKCS1_RSA) - 1, 0));
    si_len += (size_t)ret;
    /* digestAlgorithm = sha256, NULL */
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0));
    si_len += (size_t)ret;
    /* issuerAndSerialNumber ::= SEQUENCE { Name issuer, INTEGER serial } */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->serial.p, cert->serial.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, cert->serial.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, cert->serial.tag));
    field_len += (size_t)ret;
    /* issuer_raw contient deja le Name complet (tag SEQUENCE inclus). */
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->issuer_raw.p, cert->issuer_raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    field_len += (size_t)ret;
    si_len += field_len;
    /* version INTEGER 1 */
    CHK(mbedtls_asn1_write_int(&p, start, 1));
    si_len += (size_t)ret;
    /* wrap SignerInfo SEQUENCE */
    CHK(mbedtls_asn1_write_len(&p, start, si_len));
    si_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    si_len += (size_t)ret;
    /* wrap SignerInfos SET */
    CHK(mbedtls_asn1_write_len(&p, start, si_len));
    si_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    si_len += (size_t)ret;
    len += si_len;

    /* ---- certificates [0] IMPLICIT { cert } (un seul, le signataire) ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->raw.p, cert->raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, cert->raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- contentInfo ::= SEQUENCE { OID data } (detache, sans eContent) ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_oid(&p, start, MBEDTLS_OID_PKCS7_DATA,
                              sizeof(MBEDTLS_OID_PKCS7_DATA) - 1));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- digestAlgorithms ::= SET { SEQUENCE { sha256, NULL } } ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- version INTEGER 1 ---- */
    CHK(mbedtls_asn1_write_int(&p, start, 1));
    len += (size_t)ret;

    /* ---- wrap SignedData SEQUENCE ---- */
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    len += (size_t)ret;

    /* ---- ContentInfo exterieur : [0] EXPLICIT { SignedData } + OID ---- */
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, MBEDTLS_OID_PKCS7_SIGNED_DATA,
                              sizeof(MBEDTLS_OID_PKCS7_SIGNED_DATA) - 1));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    len += (size_t)ret;

#undef CHK

    result = (unsigned char*)malloc(len);
    if (!result) {
        free(buf);
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(result, p, len);
    free(buf);
    *p7_der = result;
    *p7_len = len;
    return OWL_CODE_SIGN_OK;

fail:
    free(buf);
    return OWL_CODE_SIGN_ESIGN;
}

/* ========================================================================
 * Signature Authenticode d'un executable PE (code_sign.h::owl_code_sign_pe)
 *
 * Trois briques nouvelles par rapport a la forme detachee ci-dessus :
 *
 *  1. Le hash Authenticode : SHA-256 de TOUT le PE SAUF le champ CheckSum
 *     (4 octets), l'entree Security de la data directory (8 octets) et la
 *     table de certificats appondue en fin de fichier. cf. pe_authenticode_digest.
 *
 *  2. Le SignerInfo porte des attributs authentifies (contentType +
 *     messageDigest) : la signature RSA ne couvre plus le contenu directement
 *     mais le SHA-256 de ces attributs REENCODES en SET universel (0x31), la
 *     ou le fil les transporte en [0] IMPLICIT (0xA0). cf. le re-tag plus bas.
 *
 *  3. Le contenu signe est un SpcIndirectDataContent (OID SPC_INDIRECT_DATA)
 *     embarquant le hash Authenticode, pas un simple `data`.
 *
 * mbedtls_pkcs7_* ne sachant pas lire un SignerInfo a attributs authentifies,
 * tout est ecrit a la main via mbedtls_asn1_write_* (ecriture a rebours, du
 * haut du buffer vers le bas) comme owl_code_sign_pkcs7.
 * ======================================================================== */

/* OIDs bruts (absents de oid.h sous forme exploitable ici). */
static const unsigned char OID_SPC_INDIRECT_DATA_OBJ[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 /* 1.3.6.1.4.1.311.2.1.4 */
};
static const unsigned char OID_ATTR_CONTENT_TYPE[] = {
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x03 /* 1.2.840.113549.1.9.3 */
};
static const unsigned char OID_ATTR_MESSAGE_DIGEST[] = {
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x04 /* 1.2.840.113549.1.9.4 */
};
/* Attributs authentifies exiges par Windows pour valider la signature. */
static const unsigned char OID_SPC_STATEMENT_TYPE[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0b /* 1.3.6.1.4.1.311.2.1.11 */
};
static const unsigned char OID_SPC_SP_OPUS_INFO[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0c /* 1.3.6.1.4.1.311.2.1.12 */
};
static const unsigned char OID_SPC_INDIVIDUAL_SP_KEY_PURPOSE[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x15 /* 1.3.6.1.4.1.311.2.1.21 */
};

/* SpcAttributeTypeAndOptionalValue (champ `data` du SpcIndirectDataContent),
 * forme "obsolete" canonique produite par les outils standard : OID
 * SPC_PE_IMAGE_DATA (1.3.6.1.4.1.311.2.1.15) + SpcPeImageData minimal dont le
 * SpcLink pointe une SpcString BMP "<<<Obsolete>>>". Le contenu interne n'est
 * pas contraint par le format ; cette forme fixe maximise la compatibilite
 * avec les verificateurs standard. */
static const unsigned char SPC_PE_IMAGE_DATA_BLOB[] = {
    0x30, 0x33,
      0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x0f,
      0x30, 0x25,
        0x03, 0x01, 0x00,                   /* flags : BIT STRING vide */
        0xa0, 0x20,                         /* file [0] EXPLICIT */
          0xa2, 0x1e,                       /* SpcLink file [2] EXPLICIT */
            0x80, 0x1c,                     /* SpcString unicode [0] IMPLICIT BMPString */
              0x00, 0x3c, 0x00, 0x3c, 0x00, 0x3c, 0x00, 0x4f, 0x00, 0x62, 0x00, 0x73,
              0x00, 0x6f, 0x00, 0x6c, 0x00, 0x65, 0x00, 0x74, 0x00, 0x65, 0x00, 0x3e,
              0x00, 0x3e, 0x00, 0x3e        /* "<<<Obsolete>>>" en UTF-16BE */
};

static uint16_t pe_rd_u16(const unsigned char* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t pe_rd_u32(const unsigned char* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static void pe_wr_u16(unsigned char* b, uint16_t v) { b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF); }
static void pe_wr_u32(unsigned char* b, uint32_t v) {
    b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF); b[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Repere les offsets fichier des champs mobiles du PE (CheckSum, entree
 * Security de la data directory). Gere PE32 (magic 0x10B, data directory a
 * +96 dans l'optional header) et PE32+ (0x20B, +112) ; le champ CheckSum est
 * a +64 dans les deux. Renvoie 0 si la structure est exploitable. */
static int pe_locate(const unsigned char* pe, size_t pe_len,
                     size_t* opt_off, size_t* checksum_off, size_t* secdir_off,
                     uint32_t* sizeof_headers, uint16_t* num_sections, size_t* sections_off) {
    if (pe_len < 0x40) return -1;
    uint32_t e_lfanew = pe_rd_u32(pe + 0x3C);
    if ((size_t)e_lfanew + 4 + 20 + 2 > pe_len) return -1;
    if (memcmp(pe + e_lfanew, "PE\0\0", 4) != 0) return -1;
    const unsigned char* coff = pe + e_lfanew + 4;
    uint16_t nsec = pe_rd_u16(coff + 2);
    uint16_t size_opt = pe_rd_u16(coff + 16);
    size_t opt = (size_t)(coff - pe) + 20;
    if (opt + 2 > pe_len) return -1;
    uint16_t magic = pe_rd_u16(pe + opt);
    size_t datadir_rel;
    if (magic == 0x020B) datadir_rel = 112;      /* PE32+ */
    else if (magic == 0x010B) datadir_rel = 96;  /* PE32 */
    else return -1;
    size_t sec_dir = opt + datadir_rel + 4u * 8u; /* Security = index 4 */
    if (sec_dir + 8 > pe_len) return -1;
    if (opt + 64 > pe_len) return -1;
    /* NumberOfRvaAndSizes (@ opt+datadir_rel-4) doit couvrir l'entree Security
     * (index 4). Un PE legitime vaut 16 ; en deca de 5, l'entree Security
     * n'existe pas et la lire serait un acces a une data directory tronquee. */
    uint32_t num_rva = pe_rd_u32(pe + opt + datadir_rel - 4);
    if (num_rva < 5) return -1;
    /* Le hash Authenticode trie les sections dans un tableau borne (96). Un
     * nombre de sections au-dela ne peut pas etre traite sans troncature
     * silencieuse -> rejet propre plutot que digest faux. */
    if (nsec > 96) return -1;

    *opt_off = opt;
    *checksum_off = opt + 64;
    *secdir_off = sec_dir;
    *sizeof_headers = pe_rd_u32(pe + opt + 60);
    *num_sections = nsec;
    *sections_off = opt + size_opt;
    return 0;
}

/* Hash Authenticode (SHA-256). Saute le champ CheckSum, l'entree Security de
 * la data directory et la table de certificats finale, dans l'ordre impose
 * par la spec : debut -> CheckSum, -> Security dir, -> fin des headers, puis
 * les sections dans l'ordre de leur offset fichier, puis l'eventuel trailing
 * situe avant la table de certificats. */
static int pe_authenticode_digest(const unsigned char* pe, size_t pe_len, unsigned char out[32]) {
    size_t opt_off, checksum_off, secdir_off, sections_off;
    uint32_t sizeof_headers;
    uint16_t num_sections;
    if (pe_locate(pe, pe_len, &opt_off, &checksum_off, &secdir_off,
                  &sizeof_headers, &num_sections, &sections_off) != 0) {
        return -1;
    }
    if ((size_t)sizeof_headers > pe_len || secdir_off + 8 > sizeof_headers) return -1;

    uint32_t sec_va = pe_rd_u32(pe + secdir_off);

    /* Sections ordonnees par PointerToRawData (la table d'en-tetes n'est pas
     * garantie triee par offset fichier). */
    typedef struct { uint32_t ptr, size; } SecRange;
    SecRange ranges[96];
    size_t n = 0;
    for (uint16_t i = 0; i < num_sections && n < 96; i++) {
        size_t shp = sections_off + (size_t)i * 40u;
        if (shp + 40 > pe_len) return -1;
        uint32_t raw_size = pe_rd_u32(pe + shp + 16);
        uint32_t raw_ptr  = pe_rd_u32(pe + shp + 20);
        if (raw_size == 0 || raw_ptr == 0) continue;
        if ((size_t)raw_ptr + raw_size > pe_len) return -1;
        size_t j = n;
        while (j > 0 && ranges[j - 1].ptr > raw_ptr) { ranges[j] = ranges[j - 1]; j--; }
        ranges[j].ptr = raw_ptr; ranges[j].size = raw_size;
        n++;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) { mbedtls_sha256_free(&ctx); return -1; }

    mbedtls_sha256_update(&ctx, pe, checksum_off);                                           /* debut -> CheckSum (exclu) */
    mbedtls_sha256_update(&ctx, pe + checksum_off + 4, secdir_off - (checksum_off + 4));     /* -> Security dir (exclu) */
    mbedtls_sha256_update(&ctx, pe + secdir_off + 8, (size_t)sizeof_headers - (secdir_off + 8)); /* -> fin des headers */

    size_t last_end = sizeof_headers;
    for (size_t i = 0; i < n; i++) {
        mbedtls_sha256_update(&ctx, pe + ranges[i].ptr, ranges[i].size);
        size_t end = (size_t)ranges[i].ptr + ranges[i].size;
        if (end > last_end) last_end = end;
    }

    size_t tail_end = (sec_va != 0) ? (size_t)sec_va : pe_len;
    if (tail_end > pe_len) tail_end = pe_len;
    if (tail_end > last_end) mbedtls_sha256_update(&ctx, pe + last_end, tail_end - last_end);

    int ret = mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    return ret == 0 ? 0 : -1;
}

/* CheckSum PE : somme de mots 16 bits (champ CheckSum lui-meme traite comme
 * nul) avec repli de retenue, + taille fichier. Couvre TOUT le fichier, table
 * de certificats comprise. */
static uint32_t pe_compute_checksum(const unsigned char* buf, size_t len, size_t checksum_off) {
    uint64_t sum = 0;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        uint32_t word;
        if (i == checksum_off || i == checksum_off + 2) {
            word = 0;
        } else {
            word = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8);
        }
        sum += word;
        if (sum > 0xFFFFu) sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (i < len) {
        sum += buf[i];
        if (sum > 0xFFFFu) sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum += (uint32_t)len;
    return (uint32_t)sum;
}

/* SpcIndirectDataContent ::= SEQUENCE { data SpcAttributeTypeAndOptionalValue,
 *                                       messageDigest DigestInfo }
 * DigestInfo embarque le hash Authenticode. Buffer standalone (tag SEQUENCE
 * inclus) alloue par malloc : il sert d'eContent ET de base au SHA-256 porte
 * par l'attribut messageDigest. */
static int build_spc_indirect_data(const unsigned char pe_hash[32],
                                   unsigned char** out, size_t* out_len) {
    size_t cap = sizeof(SPC_PE_IMAGE_DATA_BLOB) + 128;
    unsigned char* buf = (unsigned char*)malloc(cap);
    if (!buf) return -1;
    unsigned char* start = buf;
    unsigned char* p = buf + cap;
    size_t di = 0, total = 0;
    int ret;
#define CHK(f) do { if ((ret = (f)) < 0) { free(buf); return -1; } } while (0)
    /* DigestInfo ::= SEQUENCE { AlgorithmIdentifier(sha256), OCTET STRING hash } */
    CHK(mbedtls_asn1_write_octet_string(&p, start, pe_hash, 32)); di += (size_t)ret;
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0)); di += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, di)); di += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); di += (size_t)ret;
    /* data (DER fige) precede DigestInfo */
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, SPC_PE_IMAGE_DATA_BLOB, sizeof(SPC_PE_IMAGE_DATA_BLOB)));
    total = di + sizeof(SPC_PE_IMAGE_DATA_BLOB);
    /* wrap SpcIndirectDataContent SEQUENCE */
    CHK(mbedtls_asn1_write_len(&p, start, total)); total += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); total += (size_t)ret;
#undef CHK
    unsigned char* r = (unsigned char*)malloc(total);
    if (!r) { free(buf); return -1; }
    memcpy(r, p, total);
    free(buf);
    *out = r;
    *out_len = total;
    return 0;
}

/* Un attribut authentifie encode (SEQUENCE { OID, SET { value } }), copie dans
 * son propre buffer heap pour pouvoir trier avant concatenation. */
typedef struct {
    unsigned char* data;
    size_t len;
} AuthAttrEnc;

/* Ordre canonique DER d'un SET OF (X.690 §11.6) : comparaison octet-a-octet des
 * encodages complets ; sur prefixe egal, le plus court (complete de zeros en
 * queue) vient avant. 0x00 etant l'octet minimal, "plus court d'abord" suffit. */
static int auth_attr_der_cmp(const void* pa, const void* pb) {
    const AuthAttrEnc* a = (const AuthAttrEnc*)pa;
    const AuthAttrEnc* b = (const AuthAttrEnc*)pb;
    size_t m = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, m);
    if (c != 0) return c;
    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

/* Contenu (sans tag englobant) des attributs authentifies, en ordre canonique
 * DER d'un SET OF : contentType, messageDigest (PKCS#9), SpcStatementType,
 * SpcSpOpusInfo (SPC). Windows est strict sur cet ordre (tri par encodage
 * octet, pas par OID) sous peine de statut NotSigned. Chaque attribut est
 * construit a rebours dans un buffer dedie, puis l'ensemble est trie et
 * concatene. L'appelant coiffe le tout d'un tag [0] IMPLICIT (0xA0) pour le fil
 * et d'un tag SET (0x31) pour le hash signe. */
static int build_auth_attrs(const unsigned char msg_digest[32],
                            unsigned char** out, size_t* out_len) {
    AuthAttrEnc attrs[4];
    size_t n = 0, i;
    unsigned char buf[512];
    unsigned char* start = buf;
    unsigned char* p;
    size_t a;
    int ret, rc = -1;
#define CHK(f) do { if ((ret = (f)) < 0) goto cleanup; } while (0)
#define SNAP() do {                                                       \
        attrs[n].len = a;                                                 \
        attrs[n].data = (unsigned char*)malloc(a ? a : 1);               \
        if (!attrs[n].data) goto cleanup;                                 \
        memcpy(attrs[n].data, p, a);                                     \
        n++;                                                             \
    } while (0)
    /* contentType : SEQUENCE { OID contentType, SET { OID SPC_INDIRECT_DATA } } */
    p = buf + sizeof(buf); a = 0;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_SPC_INDIRECT_DATA_OBJ,
                               sizeof(OID_SPC_INDIRECT_DATA_OBJ))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_ATTR_CONTENT_TYPE,
                               sizeof(OID_ATTR_CONTENT_TYPE))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    SNAP();
    /* messageDigest : SEQUENCE { OID messageDigest, SET { OCTET STRING md } } */
    p = buf + sizeof(buf); a = 0;
    CHK(mbedtls_asn1_write_octet_string(&p, start, msg_digest, 32)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_ATTR_MESSAGE_DIGEST,
                               sizeof(OID_ATTR_MESSAGE_DIGEST))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    SNAP();
    /* SpcStatementType : SEQUENCE { OID SpcStatementType,
     *   SET { SEQUENCE { OID SPC_INDIVIDUAL_SP_KEY_PURPOSE } } } */
    p = buf + sizeof(buf); a = 0;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_SPC_INDIVIDUAL_SP_KEY_PURPOSE,
                               sizeof(OID_SPC_INDIVIDUAL_SP_KEY_PURPOSE))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_SPC_STATEMENT_TYPE,
                               sizeof(OID_SPC_STATEMENT_TYPE))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    SNAP();
    /* SpcSpOpusInfo : SEQUENCE { OID SpcSpOpusInfo, SET { SEQUENCE {} } }.
     * Champs tous optionnels -> SEQUENCE vide (30 00). */
    p = buf + sizeof(buf); a = 0;
    CHK(mbedtls_asn1_write_len(&p, start, 0)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_SPC_SP_OPUS_INFO,
                               sizeof(OID_SPC_SP_OPUS_INFO))); a += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, a)); a += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)); a += (size_t)ret;
    SNAP();
#undef CHK
#undef SNAP
    /* Tri canonique DER SET OF puis concatenation dans l'ordre trie. */
    qsort(attrs, n, sizeof(attrs[0]), auth_attr_der_cmp);
    {
        size_t total = 0, off = 0;
        unsigned char* r;
        for (i = 0; i < n; i++) total += attrs[i].len;
        r = (unsigned char*)malloc(total ? total : 1);
        if (!r) goto cleanup;
        for (i = 0; i < n; i++) {
            memcpy(r + off, attrs[i].data, attrs[i].len);
            off += attrs[i].len;
        }
        *out = r;
        *out_len = total;
        rc = 0;
    }
cleanup:
    for (i = 0; i < n; i++) free(attrs[i].data);
    return rc;
}

/* Construit le PKCS#7 SignedData Authenticode (DER) dans *p7 / *p7_len.
 * Interne : l'appel public injecte ensuite le resultat dans le PE. */
static OwlCodeSignStatus build_authenticode_pkcs7(OwlSigner* s, const unsigned char pe_hash[32],
                                                  unsigned char** p7, size_t* p7_len) {
    unsigned char* spc = NULL; size_t spc_len = 0;
    unsigned char* attrs = NULL; size_t attrs_len = 0;
    unsigned char md_attr[32];
    unsigned char signed_digest[32];
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    unsigned char* buf = NULL;
    size_t buf_size;
    unsigned char* start;
    unsigned char* p;
    size_t len = 0, si_len = 0, field_len = 0;
    mbedtls_x509_crt* cert = &s->cert;
    int ret;
    unsigned char* result;

    if (build_spc_indirect_data(pe_hash, &spc, &spc_len) != 0) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    /* messageDigest = SHA-256 du CONTENU de SpcIndirectDataContent, sans son
     * tag SEQUENCE englobant : Authenticode hash la concatenation des deux
     * membres (data SpcAttributeTypeAndOptionalValue + DigestInfo), pas le
     * wrapper. Hasher la SEQUENCE entiere fait rejeter la signature par
     * Windows (le verifieur recalcule sur le contenu). */
    {
        unsigned char* q = spc;
        size_t clen;
        if (mbedtls_asn1_get_tag(&q, spc + spc_len, &clen,
                                 MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
            mbedtls_sha256(q, clen, md_attr, 0) != 0) {
            free(spc);
            return OWL_CODE_SIGN_ESIGN;
        }
    }
    if (build_auth_attrs(md_attr, &attrs, &attrs_len) != 0) {
        free(spc);
        return OWL_CODE_SIGN_ENOMEM;
    }

    /* La signature porte sur le SHA-256 des attributs REENCODES en SET
     * universel (0x31), PAS sur leur encodage [0] IMPLICIT du fil : seul le
     * tag de tete change, longueur et contenu sont identiques bit a bit. */
    {
        unsigned char hdr[16];
        unsigned char* hstart = hdr;
        unsigned char* hp = hdr + sizeof(hdr);
        int r1, r2;
        mbedtls_sha256_context sc;
        if ((r1 = mbedtls_asn1_write_len(&hp, hstart, attrs_len)) < 0 ||
            (r2 = mbedtls_asn1_write_tag(&hp, hstart,
                                         MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET)) < 0) {
            free(spc); free(attrs);
            return OWL_CODE_SIGN_ESIGN;
        }
        (void)r1; (void)r2;
        mbedtls_sha256_init(&sc);
        if (mbedtls_sha256_starts(&sc, 0) != 0) {
            mbedtls_sha256_free(&sc); free(spc); free(attrs);
            return OWL_CODE_SIGN_ESIGN;
        }
        mbedtls_sha256_update(&sc, hp, (size_t)((hdr + sizeof(hdr)) - hp));
        mbedtls_sha256_update(&sc, attrs, attrs_len);
        if (mbedtls_sha256_finish(&sc, signed_digest) != 0) {
            mbedtls_sha256_free(&sc); free(spc); free(attrs);
            return OWL_CODE_SIGN_ESIGN;
        }
        mbedtls_sha256_free(&sc);
    }
    if (mbedtls_pk_sign(&s->pk, MBEDTLS_MD_SHA256, signed_digest, sizeof(signed_digest),
                        sig, sizeof(sig), &sig_len,
                        mbedtls_ctr_drbg_random, &s->drbg) != 0) {
        free(spc); free(attrs);
        return OWL_CODE_SIGN_ESIGN;
    }

    buf_size = cert->raw.len + spc_len + attrs_len + sig_len + 1024;
    buf = (unsigned char*)malloc(buf_size);
    if (!buf) {
        free(spc); free(attrs);
        return OWL_CODE_SIGN_ENOMEM;
    }
    start = buf;
    p = buf + buf_size;

#define CHK(f) do { if ((ret = (f)) < 0) { goto fail; } } while (0)

    /* ---- SignerInfo (un seul) ---- */
    /* encryptedDigest OCTET STRING = signature RSA */
    CHK(mbedtls_asn1_write_octet_string(&p, start, sig, sig_len));
    si_len = (size_t)ret;
    /* digestEncryptionAlgorithm = rsaEncryption, NULL */
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_PKCS1_RSA,
                                                sizeof(MBEDTLS_OID_PKCS1_RSA) - 1, 0));
    si_len += (size_t)ret;
    /* authenticatedAttributes [0] IMPLICIT (meme contenu que la forme signee) */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, attrs, attrs_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, attrs_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0)); /* 0xA0 */
    field_len += (size_t)ret;
    si_len += field_len;
    /* digestAlgorithm = sha256, NULL */
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0));
    si_len += (size_t)ret;
    /* issuerAndSerialNumber ::= SEQUENCE { Name issuer, INTEGER serial } */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->serial.p, cert->serial.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, cert->serial.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, cert->serial.tag));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->issuer_raw.p, cert->issuer_raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    field_len += (size_t)ret;
    si_len += field_len;
    /* version INTEGER 1 */
    CHK(mbedtls_asn1_write_int(&p, start, 1));
    si_len += (size_t)ret;
    /* wrap SignerInfo SEQUENCE */
    CHK(mbedtls_asn1_write_len(&p, start, si_len));
    si_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    si_len += (size_t)ret;
    /* wrap SignerInfos SET */
    CHK(mbedtls_asn1_write_len(&p, start, si_len));
    si_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    si_len += (size_t)ret;
    len += si_len;

    /* ---- certificates [0] IMPLICIT { cert } ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, cert->raw.p, cert->raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, cert->raw.len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- contentInfo ::= SEQUENCE { OID SPC_INDIRECT_DATA,
     *                                 content [0] EXPLICIT SpcIndirectDataContent } ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, spc, spc_len)); /* eContent (SEQUENCE inclus) */
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, spc_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)OID_SPC_INDIRECT_DATA_OBJ,
                               sizeof(OID_SPC_INDIRECT_DATA_OBJ)));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- digestAlgorithms ::= SET { SEQUENCE { sha256, NULL } } ---- */
    field_len = 0;
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, field_len));
    field_len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    field_len += (size_t)ret;
    len += field_len;

    /* ---- version INTEGER 1 ---- */
    CHK(mbedtls_asn1_write_int(&p, start, 1));
    len += (size_t)ret;

    /* ---- wrap SignedData SEQUENCE ---- */
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    len += (size_t)ret;

    /* ---- ContentInfo exterieur : OID signedData + [0] EXPLICIT { SignedData } ---- */
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, MBEDTLS_OID_PKCS7_SIGNED_DATA,
                               sizeof(MBEDTLS_OID_PKCS7_SIGNED_DATA) - 1));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    len += (size_t)ret;

#undef CHK

    result = (unsigned char*)malloc(len);
    if (!result) {
        free(buf); free(spc); free(attrs);
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(result, p, len);
    free(buf); free(spc); free(attrs);
    *p7 = result;
    *p7_len = len;
    return OWL_CODE_SIGN_OK;

fail:
    free(buf); free(spc); free(attrs);
    return OWL_CODE_SIGN_ESIGN;
}

/* Appond un PKCS#7 (deja construit, horodate ou non) a un PE non signe :
 * table de certificats WIN_CERTIFICATE alignee 8, Security DataDirectory
 * renseignee, CheckSum recalcule. Mutualise entre la signature simple et la
 * variante horodatee pour ne pas dupliquer la mecanique d'injection. */
static OwlCodeSignStatus inject_pkcs7_into_pe(const uint8_t* pe_in, size_t pe_in_len,
                                              const uint8_t* p7, size_t p7_len,
                                              uint8_t** pe_out, size_t* pe_out_len) {
    size_t opt_off, checksum_off, secdir_off, sections_off;
    uint32_t sizeof_headers;
    uint16_t num_sections;

    /* La table de certificats s'appond en fin de fichier : son offset doit
     * etre aligne 8 octets (exigence Authenticode), donc la taille du PE non
     * signe doit l'etre aussi (un vrai PE respecte deja FileAlignment). */
    if (pe_locate(pe_in, pe_in_len, &opt_off, &checksum_off, &secdir_off,
                  &sizeof_headers, &num_sections, &sections_off) != 0) {
        return OWL_CODE_SIGN_EBADARG;
    }
    if ((pe_in_len % 8) != 0) {
        return OWL_CODE_SIGN_EBADARG;
    }

    /* WIN_CERTIFICATE : dwLength (entete 8o + blob + padding), aligne 8. */
    size_t base = 8 + p7_len;
    size_t pad = (8 - (base % 8)) % 8;
    uint32_t dw_length = (uint32_t)(base + pad);
    size_t out_len = pe_in_len + dw_length;

    unsigned char* out = (unsigned char*)calloc(1, out_len);
    if (!out) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(out, pe_in, pe_in_len);

    /* Security DataDirectory (index 4) : VirtualAddress = OFFSET FICHIER de la
     * table (cas special, pas une RVA), Size = dwLength. */
    pe_wr_u32(out + secdir_off, (uint32_t)pe_in_len);
    pe_wr_u32(out + secdir_off + 4, dw_length);

    /* Entete WIN_CERTIFICATE + blob PKCS#7 (padding deja a zero via calloc). */
    unsigned char* wc = out + pe_in_len;
    pe_wr_u32(wc + 0, dw_length);
    pe_wr_u16(wc + 4, 0x0200); /* WIN_CERT_REVISION_2_0 */
    pe_wr_u16(wc + 6, 0x0002); /* WIN_CERT_TYPE_PKCS_SIGNED_DATA */
    memcpy(wc + 8, p7, p7_len);

    /* CheckSum recalcule sur le fichier complet (table de certifs incluse). */
    pe_wr_u32(out + checksum_off, 0);
    uint32_t cksum = pe_compute_checksum(out, out_len, checksum_off);
    pe_wr_u32(out + checksum_off, cksum);

    *pe_out = out;
    *pe_out_len = out_len;
    return OWL_CODE_SIGN_OK;
}

/* Pre-validation structurelle d'un PE signable : structure localisable et
 * taille alignee 8 (prerequis de l'appension de la table de certificats). Rend
 * EBADARG sur un PE malforme, AVANT tout calcul de digest -- distingue "pas un
 * PE signable" (argument) d'un echec de signature (OWL_CODE_SIGN_ESIGN). */
static OwlCodeSignStatus pe_validate_signable(const uint8_t* pe_in, size_t pe_in_len) {
    size_t opt_off, checksum_off, secdir_off, sections_off;
    uint32_t sizeof_headers;
    uint16_t num_sections;
    if (pe_in_len < 0x40) {
        return OWL_CODE_SIGN_EBADARG;
    }
    if (pe_locate(pe_in, pe_in_len, &opt_off, &checksum_off, &secdir_off,
                  &sizeof_headers, &num_sections, &sections_off) != 0) {
        return OWL_CODE_SIGN_EBADARG;
    }
    if ((pe_in_len % 8) != 0) {
        return OWL_CODE_SIGN_EBADARG;
    }
    return OWL_CODE_SIGN_OK;
}

OwlCodeSignStatus owl_code_sign_pe(OwlSigner* s, const uint8_t* pe_in, size_t pe_in_len,
                                   uint8_t** pe_out, size_t* pe_out_len) {
    unsigned char pe_hash[32];
    unsigned char* p7 = NULL;
    size_t p7_len = 0;
    OwlCodeSignStatus st;

    if (!s || !pe_in || !pe_out || !pe_out_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *pe_out = NULL;
    *pe_out_len = 0;
    st = pe_validate_signable(pe_in, pe_in_len);
    if (st != OWL_CODE_SIGN_OK) {
        return st;
    }

    if (pe_authenticode_digest(pe_in, pe_in_len, pe_hash) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    st = build_authenticode_pkcs7(s, pe_hash, &p7, &p7_len);
    if (st != OWL_CODE_SIGN_OK) {
        return st;
    }
    st = inject_pkcs7_into_pe(pe_in, pe_in_len, p7, p7_len, pe_out, pe_out_len);
    free(p7);
    return st;
}

/* ========================================================================
 * Horodatage RFC 3161 (code_sign.h) — operations structurelles sur le DER.
 *
 * Aucune des structures RFC 3161 (TimeStampReq, TimeStampResp,
 * MessageImprint) n'a d'equivalent haut-niveau dans mbedTLS : tout passe par
 * les primitives asn1 / asn1write, en ecriture a rebours comme les PKCS#7
 * ci-dessus (du haut du buffer vers le bas).
 * ======================================================================== */

/* id-aa-timeStampToken = 1.2.840.113549.1.9.16.2.14 (RFC 3161 §3.3.2) :
 * OID de l'attribut non signe portant le token pour le CMS detache
 * (verifiable openssl). */
static const unsigned char OID_ID_AA_TIMESTAMP_TOKEN[] = {
    0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x02, 0x0e
};

/* szOID_RFC3161_counterSign = 1.3.6.1.4.1.311.3.3.1 : OID Microsoft requis
 * pour horodater RFC 3161 une signature Authenticode (cf. signtool /tr,
 * osslsigncode). Windows (Get-AuthenticodeSignature.TimeStamperCertificate)
 * n'inspecte que cet attribut ; l'OID CMS standard ci-dessus n'est PAS
 * reconnu sur le chemin Authenticode. */
static const unsigned char OID_RFC3161_COUNTER_SIGN[] = {
    0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x03, 0x03, 0x01
};

/* TimeStampReq ::= SEQUENCE {
 *   version          INTEGER(1),
 *   messageImprint   SEQUENCE { hashAlgorithm AlgorithmIdentifier(SHA-256),
 *                               hashedMessage OCTET STRING },
 *   nonce            INTEGER (aleatoire),
 *   certReq          BOOLEAN TRUE }
 * reqPolicy et extensions optionnels sont omis. */
OwlCodeSignStatus owl_code_sign_tsreq_build(const uint8_t* sig_value, size_t sig_len,
                                            uint8_t** req_der, size_t* req_len) {
    unsigned char hash[32];
    unsigned char nonce[8];
    unsigned char buf[256];
    unsigned char* start = buf;
    unsigned char* p = buf + sizeof(buf);
    size_t len = 0, mi = 0;
    int ret;
    unsigned char* result;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;

    if (!sig_value || !req_der || !req_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *req_der = NULL;
    *req_len = 0;

    if (mbedtls_sha256(sig_value, sig_len, hash, 0) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }

    /* Nonce aleatoire. Premier octet force positif et non nul : un INTEGER DER
     * minimal exige que le bit de poids fort du premier octet soit a 0 (sinon
     * valeur negative). */
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-tsreq", 9) != 0 ||
        mbedtls_ctr_drbg_random(&drbg, nonce, sizeof(nonce)) != 0) {
        mbedtls_ctr_drbg_free(&drbg);
        mbedtls_entropy_free(&ent);
        return OWL_CODE_SIGN_ESIGN;
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    nonce[0] = (unsigned char)((nonce[0] & 0x7F) | 0x01);

#define CHK(f) do { if ((ret = (f)) < 0) { return OWL_CODE_SIGN_ESIGN; } } while (0)
    /* certReq BOOLEAN TRUE (dernier champ) */
    CHK(mbedtls_asn1_write_bool(&p, start, 1));
    len += (size_t)ret;
    /* nonce INTEGER (ecrit a la main : positif, longueur fixe) */
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, nonce, sizeof(nonce)));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, sizeof(nonce)));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_INTEGER));
    len += (size_t)ret;
    /* messageImprint SEQUENCE { AlgorithmIdentifier(SHA-256), OCTET STRING } */
    CHK(mbedtls_asn1_write_octet_string(&p, start, hash, sizeof(hash)));
    mi += (size_t)ret;
    CHK(mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_DIGEST_ALG_SHA256,
                                                sizeof(MBEDTLS_OID_DIGEST_ALG_SHA256) - 1, 0));
    mi += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, mi));
    mi += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    mi += (size_t)ret;
    len += mi;
    /* version INTEGER 1 */
    CHK(mbedtls_asn1_write_int(&p, start, 1));
    len += (size_t)ret;
    /* wrap TimeStampReq SEQUENCE */
    CHK(mbedtls_asn1_write_len(&p, start, len));
    len += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    len += (size_t)ret;
#undef CHK

    result = (unsigned char*)malloc(len);
    if (!result) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(result, p, len);
    *req_der = result;
    *req_len = len;
    return OWL_CODE_SIGN_OK;
}

/* TimeStampResp ::= SEQUENCE {
 *   status          PKIStatusInfo ::= SEQUENCE { status INTEGER, ... },
 *   timeStampToken  ContentInfo OPTIONAL }
 * Statut granted (0) / grantedWithMods (1) requis ; le token est extrait en
 * TLV complet (il sera reembarque tel quel). */
OwlCodeSignStatus owl_code_sign_tsresp_parse(const uint8_t* resp_der, size_t resp_len,
                                             uint8_t** token_der, size_t* token_len) {
    unsigned char* p;
    unsigned char* end;
    unsigned char* resp_end;
    unsigned char* pki_end;
    unsigned char* tok_start;
    size_t len;
    int status;
    unsigned char* result;

    if (!token_der || !token_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *token_der = NULL;
    *token_len = 0;
    if (!resp_der || resp_len == 0) {
        return OWL_CODE_SIGN_EBADARG;
    }

    p = (unsigned char*)resp_der;
    end = p + resp_len;

    /* TimeStampResp SEQUENCE */
    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    resp_end = p + len;
    /* PKIStatusInfo SEQUENCE */
    if (mbedtls_asn1_get_tag(&p, resp_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    pki_end = p + len;
    if (mbedtls_asn1_get_int(&p, pki_end, &status) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    /* 0 = granted, 1 = grantedWithMods ; tout autre statut = rejet. */
    if (status != 0 && status != 1) {
        return OWL_CODE_SIGN_EBADARG;
    }
    /* Reste de PKIStatusInfo (statusString / failInfo) ignore : saute au
     * timeStampToken, premier element apres PKIStatusInfo dans la reponse. */
    p = pki_end;
    if (p >= resp_end) {
        return OWL_CODE_SIGN_ESIGN; /* statut granted mais aucun token */
    }
    /* timeStampToken = ContentInfo SEQUENCE : extrait en TLV complet
     * (tag + longueur + contenu). */
    tok_start = p;
    if (mbedtls_asn1_get_tag(&p, resp_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    {
        size_t tlen = (size_t)((p + len) - tok_start);
        result = (unsigned char*)malloc(tlen);
        if (!result) {
            return OWL_CODE_SIGN_ENOMEM;
        }
        memcpy(result, tok_start, tlen);
        *token_der = result;
        *token_len = tlen;
    }
    return OWL_CODE_SIGN_OK;
}

/* Reinjecte un timeStampToken comme attribut NON signe dans le SignerInfo
 * unique du PKCS#7 `p7_in`. Le DER d'entree est descendu positionnellement
 * jusqu'au SignerInfo, puis le PKCS#7 est re-emis integralement : le contenu
 * existant (prefixe du SignedData + contenu du SignerInfo) est reinjecte brut
 * et l'unsignedAttrs [1] est appondu juste apres l'encryptedDigest. asn1write
 * recalcule toutes les longueurs englobantes ; la signature n'est pas touchee.
 *
 * mbedtls_pkcs7_parse_der() ne sait pas relire un SignerInfo porteur
 * d'unauthenticatedAttributes (cf. doc pkcs7.h) : on ne repasse donc jamais par
 * les structures haut-niveau, l'insertion est purement structurelle sur le DER. */
OwlCodeSignStatus owl_code_sign_embed_tstoken(const uint8_t* p7_in, size_t p7_in_len,
                                              const uint8_t* token_der, size_t token_len,
                                              const uint8_t* attr_oid, size_t attr_oid_len,
                                              uint8_t** p7_out, size_t* p7_out_len) {
    unsigned char* p;
    unsigned char* end;
    unsigned char* sd_start;
    unsigned char* sd_end;
    unsigned char* q;
    unsigned char* si_set;        /* position du tag SET signerInfos */
    unsigned char* prefix_start;  /* debut du contenu de SignedData */
    size_t prefix_len;
    unsigned char* si_content;    /* contenu du SignerInfo (version..encryptedDigest) */
    size_t si_content_len;
    size_t len;
    unsigned char* buf;
    size_t buf_size;
    unsigned char* start;
    int ret;
    size_t uattr, attr_seq, si_total, sd, total;
    unsigned char* result;

    if (!p7_out || !p7_out_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *p7_out = NULL;
    *p7_out_len = 0;
    if (!p7_in || p7_in_len == 0 || !token_der || token_len == 0 ||
        !attr_oid || attr_oid_len == 0) {
        return OWL_CODE_SIGN_EBADARG;
    }

    /* --- Descente positionnelle du DER d'entree --- */
    p = (unsigned char*)p7_in;
    end = p + p7_in_len;
    /* ContentInfo SEQUENCE */
    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    /* contentType OID (signedData) : saute, on re-emet le notre */
    if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OID) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    p += len;
    /* content [0] EXPLICIT */
    if (mbedtls_asn1_get_tag(&p, end, &len, CONTEXT0 | 0) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    /* SignedData SEQUENCE */
    if (mbedtls_asn1_get_tag(&p, end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    sd_start = p;
    sd_end = p + len;

    /* Prefixe du SignedData = version, digestAlgorithms, contentInfo,
     * puis certificates [0] et crls [1] optionnels. signerInfos SET est le
     * dernier element : tout ce qui le precede est reinjecte brut. */
    q = sd_start;
    if (mbedtls_asn1_get_tag(&q, sd_end, &len, MBEDTLS_ASN1_INTEGER) != 0) {
        return OWL_CODE_SIGN_ESIGN; /* version */
    }
    q += len;
    if (mbedtls_asn1_get_tag(&q, sd_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) {
        return OWL_CODE_SIGN_ESIGN; /* digestAlgorithms */
    }
    q += len;
    if (mbedtls_asn1_get_tag(&q, sd_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN; /* contentInfo */
    }
    q += len;
    if (q < sd_end && *q == (CONTEXT0 | 0)) { /* certificates [0] IMPLICIT */
        if (mbedtls_asn1_get_tag(&q, sd_end, &len, CONTEXT0 | 0) != 0) {
            return OWL_CODE_SIGN_ESIGN;
        }
        q += len;
    }
    if (q < sd_end && *q == (CONTEXT0 | 1)) { /* crls [1] IMPLICIT */
        if (mbedtls_asn1_get_tag(&q, sd_end, &len, CONTEXT0 | 1) != 0) {
            return OWL_CODE_SIGN_ESIGN;
        }
        q += len;
    }
    prefix_start = sd_start;
    prefix_len = (size_t)(q - sd_start);

    /* signerInfos SET (un seul SignerInfo attendu) */
    si_set = q;
    if (mbedtls_asn1_get_tag(&q, sd_end, &len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    (void)si_set;
    /* SignerInfo SEQUENCE : son contenu (version..encryptedDigest) est repris
     * brut, l'unsignedAttrs sera appondu a la fin. */
    if (mbedtls_asn1_get_tag(&q, sd_end, &si_content_len,
                             MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) {
        return OWL_CODE_SIGN_ESIGN;
    }
    si_content = q;

    /* --- Re-emission complete (ecriture a rebours) --- */
    buf_size = p7_in_len + token_len + 256;
    buf = (unsigned char*)malloc(buf_size);
    if (!buf) {
        return OWL_CODE_SIGN_ENOMEM;
    }
    start = buf;
    p = buf + buf_size;

#define CHK(f) do { if ((ret = (f)) < 0) { free(buf); return OWL_CODE_SIGN_ESIGN; } } while (0)
    /* unsignedAttrs [1] IMPLICIT SET OF Attribute, un seul attribut :
     *   Attribute ::= SEQUENCE { OID attr_oid, SET { token } }
     * attr_oid distingue le chemin : id-aa-timeStampToken (CMS detache) ou
     * szOID_RFC3161_counterSign (Authenticode PE) -- cf. appelants. */
    uattr = 0;
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, token_der, token_len));
    uattr += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, uattr));           /* SET { token } */
    uattr += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    uattr += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, (const char*)attr_oid, attr_oid_len));
    uattr += (size_t)ret;
    attr_seq = uattr;
    CHK(mbedtls_asn1_write_len(&p, start, attr_seq));        /* wrap Attribute SEQUENCE */
    attr_seq += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    attr_seq += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, attr_seq));        /* wrap unsignedAttrs [1] */
    attr_seq += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 1));    /* 0xA1 */
    attr_seq += (size_t)ret;

    /* SignerInfo content existant (precede l'unsignedAttrs), puis wrap. */
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, si_content, si_content_len));
    si_total = attr_seq + si_content_len;
    CHK(mbedtls_asn1_write_len(&p, start, si_total));        /* wrap SignerInfo SEQUENCE */
    si_total += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    si_total += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, si_total));        /* wrap signerInfos SET */
    si_total += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET));
    si_total += (size_t)ret;

    /* Prefixe du SignedData (inchange), puis wrap SignedData / [0] / OID /
     * ContentInfo. */
    CHK(mbedtls_asn1_write_raw_buffer(&p, start, prefix_start, prefix_len));
    sd = si_total + prefix_len;
    CHK(mbedtls_asn1_write_len(&p, start, sd));              /* wrap SignedData SEQUENCE */
    sd += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    sd += (size_t)ret;
    CHK(mbedtls_asn1_write_len(&p, start, sd));              /* wrap content [0] EXPLICIT */
    sd += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, CONTEXT0 | 0));
    sd += (size_t)ret;
    CHK(mbedtls_asn1_write_oid(&p, start, MBEDTLS_OID_PKCS7_SIGNED_DATA,
                               sizeof(MBEDTLS_OID_PKCS7_SIGNED_DATA) - 1));
    sd += (size_t)ret;
    total = sd;
    CHK(mbedtls_asn1_write_len(&p, start, total));           /* wrap ContentInfo SEQUENCE */
    total += (size_t)ret;
    CHK(mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE));
    total += (size_t)ret;
#undef CHK

    result = (unsigned char*)malloc(total);
    if (!result) {
        free(buf);
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(result, p, total);
    free(buf);
    *p7_out = result;
    *p7_out_len = total;
    return OWL_CODE_SIGN_OK;
}

/* ========================================================================
 * Horodatage RFC 3161 — transport HTTP + orchestration (code_sign.h).
 *
 * owl_code_sign_tsa_post() parle HTTP/1.1 minimal sur net_tcp (owl_core).
 * owl_code_sign_timestamp_pkcs7() enchaine extraction de signature -> req ->
 * POST -> parse -> embed. owl_code_sign_pe_timestamped() horodate le PKCS#7
 * Authenticode AVANT de l'injecter dans le PE (une seule injection).
 *
 * Offline-safe : rien ici n'est atteint tant que l'appelant ne fournit pas
 * d'URL de TSA (le CLI ne le fait que sur --timestamp explicite).
 * ======================================================================== */

/* Derniere cause d'echec d'horodatage, pour un message CLI actionnable.
 * Raisons techniques courtes en anglais (stables, lisibles dans les deux
 * langues de owl-build). Mono-thread : la signature owl-build est sequentielle. */
static char g_tsa_err[256] = {0};

static void tsa_set_err(const char* msg) {
    snprintf(g_tsa_err, sizeof(g_tsa_err), "%s", msg ? msg : "unknown error");
}

const char* owl_code_sign_last_tsa_error(void) {
    return g_tsa_err[0] ? g_tsa_err : "no error recorded";
}

OwlCodeSignStatus owl_code_sign_tsa_post(const char* url,
                                         const uint8_t* req_der, size_t req_len,
                                         uint8_t** resp_der, size_t* resp_len) {
    if (!resp_der || !resp_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *resp_der = NULL;
    *resp_len = 0;
    if (!url || !req_der || req_len == 0) {
        tsa_set_err("invalid arguments");
        return OWL_CODE_SIGN_EBADARG;
    }

    /* --- Parse d'URL http://host[:port][/path] (https:// non supporte) --- */
    if (strncasecmp(url, "https://", 8) == 0) {
        tsa_set_err("https TSA not supported (use an http:// TSA URL)");
        return OWL_CODE_SIGN_EBADARG;
    }
    if (strncasecmp(url, "http://", 7) != 0) {
        tsa_set_err("invalid URL (expected http://...)");
        return OWL_CODE_SIGN_EBADARG;
    }
    const char* p = url + 7;
    char host[256];
    size_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi < sizeof(host) - 1) {
        host[hi++] = *p++;
    }
    host[hi] = '\0';
    if (hi == 0) {
        tsa_set_err("invalid URL (empty host)");
        return OWL_CODE_SIGN_EBADARG;
    }
    int port = 80;
    if (*p == ':') {
        p++;
        int v = 0, any = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = 1; if (v > 65535) break; }
        if (!any || v <= 0 || v > 65535) {
            tsa_set_err("invalid URL (bad port)");
            return OWL_CODE_SIGN_EBADARG;
        }
        port = v;
    }
    char path[1024];
    if (*p == '/') {
        snprintf(path, sizeof(path), "%s", p);
    } else {
        snprintf(path, sizeof(path), "/");
    }

    /* --- Connexion + POST --- */
    owl_tcp_global_init(); /* idempotent (WSAStartup sous Windows) */
    OwlTcpSocket sock;
    owl_tcp_init(&sock);
    if (!owl_tcp_connect(&sock, host, port, 15000)) {
        char e[256];
        snprintf(e, sizeof(e), "connection to %s:%d failed: %s", host, port, owl_tcp_last_error(&sock));
        tsa_set_err(e);
        owl_tcp_close(&sock);
        return OWL_CODE_SIGN_ESIGN;
    }

    char hdr[1536];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Content-Type: application/timestamp-query\r\n"
                      "Accept: application/timestamp-reply\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n", path, host, req_len);
    if (hn <= 0 || (size_t)hn >= sizeof(hdr)) {
        tsa_set_err("request headers too large (URL too long?)");
        owl_tcp_close(&sock);
        return OWL_CODE_SIGN_ESIGN;
    }
    if (owl_tcp_send(&sock, hdr, (size_t)hn) < 0 ||
        owl_tcp_send(&sock, req_der, req_len) < 0) {
        char e[256];
        snprintf(e, sizeof(e), "send failed: %s", owl_tcp_last_error(&sock));
        tsa_set_err(e);
        owl_tcp_close(&sock);
        return OWL_CODE_SIGN_ESIGN;
    }

    /* --- Lecture de la reponse jusqu'a fermeture (Connection: close) --- */
    size_t cap = 65536, used = 0;
    unsigned char* resp = (unsigned char*)malloc(cap);
    if (!resp) {
        tsa_set_err("out of memory");
        owl_tcp_close(&sock);
        return OWL_CODE_SIGN_ENOMEM;
    }
    for (;;) {
        if (used + 4096 > cap) {
            if (cap >= 8u * 1024u * 1024u) {
                free(resp);
                tsa_set_err("response too large");
                owl_tcp_close(&sock);
                return OWL_CODE_SIGN_ESIGN;
            }
            cap *= 2;
            unsigned char* n = (unsigned char*)realloc(resp, cap);
            if (!n) {
                free(resp);
                tsa_set_err("out of memory");
                owl_tcp_close(&sock);
                return OWL_CODE_SIGN_ENOMEM;
            }
            resp = n;
        }
        int r = owl_tcp_recv(&sock, resp + used, 4096);
        if (r < 0) {
            char e[256];
            snprintf(e, sizeof(e), "recv failed: %s", owl_tcp_last_error(&sock));
            tsa_set_err(e);
            free(resp);
            owl_tcp_close(&sock);
            return OWL_CODE_SIGN_ESIGN;
        }
        if (r == 0) break; /* peer a ferme (FIN) */
        used += (size_t)r;
    }
    owl_tcp_close(&sock);

    /* --- Statut HTTP + extraction du corps --- */
    if (used < 12 || memcmp(resp, "HTTP/", 5) != 0) {
        free(resp);
        tsa_set_err("invalid HTTP response");
        return OWL_CODE_SIGN_ESIGN;
    }
    size_t i = 0;
    while (i < used && resp[i] != ' ' && resp[i] != '\r') i++;
    while (i < used && resp[i] == ' ') i++;
    int code = 0, got = 0;
    while (i < used && resp[i] >= '0' && resp[i] <= '9') { code = code * 10 + (resp[i] - '0'); i++; got = 1; }
    if (!got || code != 200) {
        char e[64];
        snprintf(e, sizeof(e), "HTTP status %d (expected 200)", got ? code : 0);
        free(resp);
        tsa_set_err(e);
        return OWL_CODE_SIGN_ESIGN;
    }

    /* Transfer-Encoding: chunked non gere en v1 : le corps serait decoupe en
     * trames et le DER extrait serait corrompu. On refuse explicitement plutot
     * que de rendre des octets faux. Les en-tetes (jusqu'au double CRLF) sont
     * en ASCII, memmem textuel sur cette tranche est sur. */
    size_t boundary = 0;
    int found_sep = 0;
    for (size_t k = 0; k + 3 < used; k++) {
        if (resp[k] == '\r' && resp[k + 1] == '\n' && resp[k + 2] == '\r' && resp[k + 3] == '\n') {
            boundary = k + 4;
            found_sep = 1;
            break;
        }
    }
    if (!found_sep) {
        free(resp);
        tsa_set_err("malformed HTTP response (no header terminator)");
        return OWL_CODE_SIGN_ESIGN;
    }
    {
        /* Recherche case-insensitive de "transfer-encoding" dans les en-tetes. */
        static const char TE[] = "transfer-encoding:";
        for (size_t k = 0; k + sizeof(TE) - 1 < boundary; k++) {
            if (strncasecmp((const char*)resp + k, TE, sizeof(TE) - 1) == 0) {
                free(resp);
                tsa_set_err("chunked/streamed HTTP body not supported");
                return OWL_CODE_SIGN_ESIGN;
            }
        }
    }

    size_t body_len = used - boundary;
    if (body_len == 0) {
        free(resp);
        tsa_set_err("empty response body");
        return OWL_CODE_SIGN_ESIGN;
    }
    unsigned char* body = (unsigned char*)malloc(body_len);
    if (!body) {
        free(resp);
        tsa_set_err("out of memory");
        return OWL_CODE_SIGN_ENOMEM;
    }
    memcpy(body, resp + boundary, body_len);
    free(resp);
    *resp_der = body;
    *resp_len = body_len;
    tsa_set_err("ok");
    return OWL_CODE_SIGN_OK;
}

/* Descend positionnellement un PKCS#7 SignedData jusqu'a l'encryptedDigest du
 * SignerInfo unique (la valeur de signature a horodater). Rend un pointeur
 * dans le buffer d'entree (pas de copie). Gere la forme detachee (sans
 * authenticatedAttributes) et Authenticode (avec [0] authAttrs). */
static OwlCodeSignStatus locate_encrypted_digest(const uint8_t* p7_in, size_t p7_in_len,
                                                 const unsigned char** out_p, size_t* out_len) {
    unsigned char* p = (unsigned char*)p7_in;
    unsigned char* end = p + p7_in_len;
    unsigned char* sd_end;
    unsigned char* si_end;
    size_t len;
    const int SEQ = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
    const int SET = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET;

    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN; /* ContentInfo */
    if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_OID) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len;
    if (mbedtls_asn1_get_tag(&p, end, &len, CONTEXT0 | 0) != 0) return OWL_CODE_SIGN_ESIGN; /* [0] */
    if (mbedtls_asn1_get_tag(&p, end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN; /* SignedData */
    sd_end = p + len;
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_INTEGER) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* version */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, SET) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* digestAlgorithms */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* contentInfo */
    if (p < sd_end && *p == (CONTEXT0 | 0)) { /* certificates [0] */
        if (mbedtls_asn1_get_tag(&p, sd_end, &len, CONTEXT0 | 0) != 0) return OWL_CODE_SIGN_ESIGN;
        p += len;
    }
    if (p < sd_end && *p == (CONTEXT0 | 1)) { /* crls [1] */
        if (mbedtls_asn1_get_tag(&p, sd_end, &len, CONTEXT0 | 1) != 0) return OWL_CODE_SIGN_ESIGN;
        p += len;
    }
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, SET) != 0) return OWL_CODE_SIGN_ESIGN; /* signerInfos */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN; /* SignerInfo */
    si_end = p + len;
    if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_INTEGER) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* version */
    if (mbedtls_asn1_get_tag(&p, si_end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* issuerAndSerialNumber */
    if (mbedtls_asn1_get_tag(&p, si_end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* digestAlgorithm */
    if (p < si_end && *p == (CONTEXT0 | 0)) { /* authenticatedAttributes [0] (Authenticode) */
        if (mbedtls_asn1_get_tag(&p, si_end, &len, CONTEXT0 | 0) != 0) return OWL_CODE_SIGN_ESIGN;
        p += len;
    }
    if (mbedtls_asn1_get_tag(&p, si_end, &len, SEQ) != 0) return OWL_CODE_SIGN_ESIGN;
    p += len; /* digestEncryptionAlgorithm */
    if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) return OWL_CODE_SIGN_ESIGN;
    *out_p = p;
    *out_len = len;
    return OWL_CODE_SIGN_OK;
}

/* Corps partage : l'OID de l'attribut non signe est choisi par l'appelant
 * (OID_ID_AA_TIMESTAMP_TOKEN pour le CMS detache, OID_RFC3161_COUNTER_SIGN
 * pour Authenticode). Seul l'OID differe entre les deux chemins. */
static OwlCodeSignStatus timestamp_pkcs7_oid(const char* tsa_url,
                                             const uint8_t* p7_in, size_t p7_in_len,
                                             const unsigned char* attr_oid, size_t attr_oid_len,
                                             uint8_t** p7_out, size_t* p7_out_len) {
    if (!p7_out || !p7_out_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *p7_out = NULL;
    *p7_out_len = 0;
    if (!tsa_url || !p7_in || p7_in_len == 0) {
        tsa_set_err("invalid arguments");
        return OWL_CODE_SIGN_EBADARG;
    }

    const unsigned char* sig_p = NULL;
    size_t sig_len = 0;
    OwlCodeSignStatus st = locate_encrypted_digest(p7_in, p7_in_len, &sig_p, &sig_len);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("cannot locate the signature value in the PKCS#7");
        return st;
    }

    uint8_t* req = NULL;
    size_t req_len = 0;
    st = owl_code_sign_tsreq_build(sig_p, sig_len, &req, &req_len);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("cannot build the TimeStampReq");
        return st;
    }

    uint8_t* resp = NULL;
    size_t resp_len = 0;
    st = owl_code_sign_tsa_post(tsa_url, req, req_len, &resp, &resp_len);
    free(req);
    if (st != OWL_CODE_SIGN_OK) {
        return st; /* g_tsa_err deja renseigne par tsa_post */
    }

    uint8_t* token = NULL;
    size_t token_len = 0;
    st = owl_code_sign_tsresp_parse(resp, resp_len, &token, &token_len);
    free(resp);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("TSA response rejected or carries no timestamp token");
        return st;
    }

    st = owl_code_sign_embed_tstoken(p7_in, p7_in_len, token, token_len,
                                     attr_oid, attr_oid_len, p7_out, p7_out_len);
    free(token);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("cannot embed the timestamp token into the PKCS#7");
        return st;
    }
    return OWL_CODE_SIGN_OK;
}

/* Chemin CMS detache : attribut standard id-aa-timeStampToken. */
OwlCodeSignStatus owl_code_sign_timestamp_pkcs7(const char* tsa_url,
                                                const uint8_t* p7_in, size_t p7_in_len,
                                                uint8_t** p7_out, size_t* p7_out_len) {
    return timestamp_pkcs7_oid(tsa_url, p7_in, p7_in_len,
                               OID_ID_AA_TIMESTAMP_TOKEN, sizeof(OID_ID_AA_TIMESTAMP_TOKEN),
                               p7_out, p7_out_len);
}

OwlCodeSignStatus owl_code_sign_pe_timestamped(OwlSigner* s,
                                               const uint8_t* pe_in, size_t pe_in_len,
                                               const char* tsa_url,
                                               uint8_t** pe_out, size_t* pe_out_len) {
    unsigned char pe_hash[32];
    unsigned char* p7 = NULL;
    size_t p7_len = 0;
    unsigned char* p7_ts = NULL;
    size_t p7_ts_len = 0;
    OwlCodeSignStatus st;

    if (!s || !pe_in || !tsa_url || !pe_out || !pe_out_len) {
        return OWL_CODE_SIGN_EBADARG;
    }
    *pe_out = NULL;
    *pe_out_len = 0;
    st = pe_validate_signable(pe_in, pe_in_len);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("not a signable PE image");
        return st;
    }

    if (pe_authenticode_digest(pe_in, pe_in_len, pe_hash) != 0) {
        tsa_set_err("cannot hash the PE (not a signable PE image)");
        return OWL_CODE_SIGN_ESIGN;
    }
    st = build_authenticode_pkcs7(s, pe_hash, &p7, &p7_len);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("cannot build the Authenticode PKCS#7");
        return st;
    }
    /* Horodate le PKCS#7 AVANT injection : une seule table de certificats est
     * appondue, deja porteuse du timeStampToken. Chemin Authenticode ->
     * OID Microsoft szOID_RFC3161_counterSign (reconnu par Windows). */
    st = timestamp_pkcs7_oid(tsa_url, p7, p7_len,
                             OID_RFC3161_COUNTER_SIGN, sizeof(OID_RFC3161_COUNTER_SIGN),
                             &p7_ts, &p7_ts_len);
    free(p7);
    if (st != OWL_CODE_SIGN_OK) {
        return st; /* g_tsa_err renseigne par timestamp_pkcs7 */
    }
    st = inject_pkcs7_into_pe(pe_in, pe_in_len, p7_ts, p7_ts_len, pe_out, pe_out_len);
    free(p7_ts);
    if (st != OWL_CODE_SIGN_OK) {
        tsa_set_err("cannot inject the timestamped signature into the PE");
    }
    return st;
}
