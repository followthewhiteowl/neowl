// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du channel binding tls-server-end-point (RFC 5929), morceau
 * hermetique du backend Negotiate (ldap_negotiate.{c,h},  S2).
 *
 * Execute via `owl --self-test-net`. Aucun reseau, aucune API plateforme :
 * on fabrique des certificats mbedtls, on parse leur DER, et on verifie que
 * owl_ldap_channel_binding_endpoint produit bien
 *   "tls-server-end-point:" || hash(DER)
 * avec l'algo de hachage de la signature du cert — SHA-256 pour un cert
 * signe SHA-256, et SHA-256 (promotion, section 4.1) pour un cert signe
 * SHA-1. Le hash attendu est recalcule par un chemin independant
 * (mbedtls_md direct sur le DER parse), ce qui rend le vecteur verifiable.
 *
 * Le vrai bind Negotiate (SSPI/GSSAPI contre un AD) n'est PAS testable
 * hermetiquement (exige un KDC) : c'est le ressort du e2e.
 */

#include "ldap_negotiate.h"

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/x509_crt.h>

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static const char  kPrefix[]   = "tls-server-end-point:";
static const size_t kPrefixLen = sizeof(kPrefix) - 1;

/* Genere un certificat auto-signe avec l'algo de signature `md_alg`, le parse
 * dans `out`. Renvoie true sur succes. */
static bool gen_cert(mbedtls_md_type_t md_alg, mbedtls_x509_crt* out) {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       key;
    mbedtls_x509write_cert   crt;
    char                     pem[4096];
    unsigned char            serie[1] = { 1 };
    bool                     ok = false;

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-cb-test", 11) != 0) goto fin;
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto fin;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=cb.example.com") != 0) goto fin;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=cb.example.com") != 0) goto fin;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, md_alg);
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serie, sizeof serie) != 0) goto fin;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000",
                                           "20350101000000") != 0) goto fin;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) != 0) goto fin;
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)pem, sizeof pem,
                                  mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;

    mbedtls_x509_crt_init(out);
    if (mbedtls_x509_crt_parse(out, (const unsigned char*)pem,
                               strlen(pem) + 1) != 0) goto fin;
    ok = true;
fin:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* Verifie le token CB d'un cert donne contre un hash recalcule independamment
 * avec l'algo `expect_md`. */
static void check_cb(mbedtls_md_type_t sig_alg, mbedtls_md_type_t expect_md,
                     const char* label) {
    mbedtls_x509_crt crt;
    if (!gen_cert(sig_alg, &crt)) { CHECK(false, label); return; }

    unsigned char out[128];
    size_t out_len = 0;
    bool ok = owl_ldap_channel_binding_endpoint(&crt, out, sizeof out, &out_len);
    CHECK(ok, label);

    if (ok) {
        const mbedtls_md_info_t* md = mbedtls_md_info_from_type(expect_md);
        size_t hlen = mbedtls_md_get_size(md);

        CHECK(out_len == kPrefixLen + hlen, "cb: longueur = prefixe + hash");
        CHECK(memcmp(out, kPrefix, kPrefixLen) == 0, "cb: prefixe tls-server-end-point:");

        unsigned char expect[64];
        int mrc = mbedtls_md(md, crt.raw.p, crt.raw.len, expect);
        CHECK(mrc == 0, "cb: hash de reference calcule");
        CHECK(mrc == 0 && memcmp(out + kPrefixLen, expect, hlen) == 0,
              "cb: hash = hash(DER) avec le bon algo");
    }
    mbedtls_x509_crt_free(&crt);
}

static void test_cb_sha256(void) {
    /* Cert signe SHA-256 -> CB hashe en SHA-256. */
    check_cb(MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA256, "cb: cert SHA-256 produit un token");
}

static void test_cb_sha384(void) {
    /* Cert signe SHA-384 -> CB hashe en SHA-384 (algo de la signature, pas de
     * promotion car ni MD5 ni SHA-1). */
    check_cb(MBEDTLS_MD_SHA384, MBEDTLS_MD_SHA384, "cb: cert SHA-384 produit un token");
}

static void test_cb_sha1_promotion(void) {
    /* Cert signe SHA-1 -> CB promu en SHA-256 (RFC 5929 section 4.1). Le hash
     * attendu fait 32 octets (SHA-256), pas 20 (SHA-1). */
    check_cb(MBEDTLS_MD_SHA1, MBEDTLS_MD_SHA256, "cb: cert SHA-1 promu SHA-256");

    /* Verifie explicitement que la longueur n'est PAS celle d'un SHA-1. */
    mbedtls_x509_crt crt;
    if (gen_cert(MBEDTLS_MD_SHA1, &crt)) {
        unsigned char out[128];
        size_t out_len = 0;
        if (owl_ldap_channel_binding_endpoint(&crt, out, sizeof out, &out_len)) {
            CHECK(out_len == kPrefixLen + 32, "cb: SHA-1 -> hash 32 octets (SHA-256)");
            CHECK(out_len != kPrefixLen + 20, "cb: SHA-1 non utilise tel quel (20 octets)");
        } else {
            CHECK(false, "cb: SHA-1 token produit");
        }
        mbedtls_x509_crt_free(&crt);
    }
}

static void test_cb_garde(void) {
    /* Cert NULL -> refus. */
    unsigned char out[128];
    size_t out_len = 0;
    CHECK(!owl_ldap_channel_binding_endpoint(NULL, out, sizeof out, &out_len),
          "cb: cert NULL refuse");

    /* Buffer trop petit -> refus (pas de debordement). */
    mbedtls_x509_crt crt;
    if (gen_cert(MBEDTLS_MD_SHA256, &crt)) {
        unsigned char tiny[8];
        size_t n = 0;
        CHECK(!owl_ldap_channel_binding_endpoint(&crt, tiny, sizeof tiny, &n),
              "cb: buffer trop petit refuse");
        mbedtls_x509_crt_free(&crt);
    } else {
        CHECK(false, "cb: garde cert genere");
    }
}

int ldap_negotiate_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_cb_sha256();
    test_cb_sha384();
    test_cb_sha1_promotion();
    test_cb_garde();

    printf("ldap_negotiate self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
