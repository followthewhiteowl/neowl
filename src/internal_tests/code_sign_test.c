// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test coeur crypto signature de code (, tranche 1).
 *
 * Module sous test : src/code_sign.{c,h} (a creer par l'implementeur).
 * Oracle : mbedTLS vendore (vendor/mbedtls), 100% local, deterministe.
 *
 * MBEDTLS_ALLOW_PRIVATE_ACCESS est defini ICI (fichier de test uniquement,
 * jamais dans le runtime) pour lire directement les champs internes de
 * mbedtls_pkcs7 (signed_data, signers, certs, sig) : le header ne fournit
 * aucun accesseur public, et ces champs sont le seul moyen d'extraire la
 * signature brute + le certificat du signataire pour une verification
 * manuelle (cf. piege "ne pas se reposer sur mbedtls_pkcs7_signed_data_verify
 * seul" ci-dessous).
 *
 * Strategie du cas 4 (round-trip, le coeur du test) :
 *   1. owl_code_sign_pkcs7() produit un DER SignedData.
 *   2. mbedtls_pkcs7_parse_der() reparse ce DER (prouve que c'est un
 *      SignedData valide, structurellement, avec 1 signataire).
 *   3. On recalcule nous-memes le SHA-256 du contenu signe et on verifie
 *      la signature du signataire avec mbedtls_pk_verify() contre la cle
 *      publique du certificat embarque dans le SignedData. C'est cette
 *      etape qui est l'oracle autoritaire : mbedtls_pkcs7_signed_data_verify()
 *      est etroit (suppose authenticatedAttributes vide, cf. commentaire
 *      en tete de mbedtls/pkcs7.h) et n'est qu'une assertion bonus ici.
 *   4. Cas 5 (tamper) : on flippe 1 octet du contenu signe et on prouve
 *      que la meme signature ne verifie plus rien (anti-tautologie).
 */

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code_sign.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs7.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

static int g_pass = 0;
static int g_fail = 0;

#define CS_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* Fixture : certificat PKCS#12 jetable "DO NOT TRUST" (cf.
 * tests/internal_fixtures/codesign/README.md), stocke en base64 car les
 * .pfx bruts sont gitignores. Mot de passe correct : "owltest". */
#define FIXTURE_PATH "tests/internal_fixtures/codesign/throwaway_codesign.pfx.b64"
#define FIXTURE_PASSWORD "owltest"

/* Contenu arbitraire a signer : deterministe, 32 octets fixes (pas de
 * generation aleatoire -> round-trip reproductible a l'identique). */
static const unsigned char CONTENU_TEST[32] = {
    'o','w','l','-','c','o','d','e','-','s','i','g','n',
    '-','t','r','a','n','c','h','e','-','1','-','c','o','r','e',
    '!','!','!'
};

/* Charge la fixture .pfx.b64 en memoire et decode le base64 en DER brut.
 * Renvoie 1 si OK (out_der/out_len remplis, out_der a liberer par free()),
 * 0 sinon. */
static int load_fixture_pkcs12_der(uint8_t** out_der, size_t* out_len) {
    FILE* f = fopen(FIXTURE_PATH, "rb");
    if (!f) {
        fprintf(stderr, "  impossible d'ouvrir %s\n", FIXTURE_PATH);
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

    /* Retire un eventuel retour a la ligne final (fixture stockee sur une
     * seule ligne, mais robuste si un editeur ajoute un newline). */
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

/* Cas 1 : chargement PKCS#12 avec le bon mot de passe doit reussir. */
static void test_load_pkcs12_ok(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    int ok = load_fixture_pkcs12_der(&der, &der_len);
    CS_CHECK(ok, "fixture .pfx.b64 chargee + decodee en DER");
    if (!ok) return;

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, FIXTURE_PASSWORD, &s);
    CS_CHECK(st == OWL_CODE_SIGN_OK, "load_pkcs12 bon mot de passe -> OK");
    CS_CHECK(s != NULL, "load_pkcs12 bon mot de passe -> signer non NULL");

    if (s) owl_code_sign_free(s);
    free(der);
}

/* Cas 2 : mauvais mot de passe -> echec propre, pas de crash, s reste NULL. */
static void test_load_pkcs12_wrong_password(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    int ok = load_fixture_pkcs12_der(&der, &der_len);
    CS_CHECK(ok, "fixture .pfx.b64 chargee + decodee en DER (cas mdp errone)");
    if (!ok) return;

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, "wrong", &s);
    CS_CHECK(st != OWL_CODE_SIGN_OK, "load_pkcs12 mauvais mot de passe -> statut != OK");
    CS_CHECK(st == OWL_CODE_SIGN_EPKCS12, "load_pkcs12 mauvais mot de passe -> EPKCS12");
    CS_CHECK(s == NULL, "load_pkcs12 mauvais mot de passe -> signer reste NULL");

    free(der);
}

/* Cas 3 : construction PKCS#7 SignedData sur un contenu fixe -> OK,
 * buffer DER non vide. */
static void test_pkcs7_build_ok(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    if (!load_fixture_pkcs12_der(&der, &der_len)) {
        CS_CHECK(0, "fixture chargee (prerequis test_pkcs7_build_ok)");
        return;
    }

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, FIXTURE_PASSWORD, &s);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis pkcs7)");
    free(der);
    if (!s) return;

    uint8_t* p7_der = NULL;
    size_t p7_len = 0;
    st = owl_code_sign_pkcs7(s, CONTENU_TEST, sizeof(CONTENU_TEST), &p7_der, &p7_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK, "pkcs7 sur contenu fixe -> OK");
    CS_CHECK(p7_der != NULL, "pkcs7 -> buffer DER non NULL");
    CS_CHECK(p7_len > 0, "pkcs7 -> longueur DER > 0");

    free(p7_der);
    owl_code_sign_free(s);
}

/* Cas 4 : ROUND-TRIP VERIFIE. Le coeur du test. Reparse le SignedData
 * produit, recalcule le SHA-256 du contenu nous-memes, et verifie la
 * signature RSA PKCS#1 v1.5 contre la cle publique du certificat embarque
 * -- sans dependre du verifieur etroit mbedtls_pkcs7_signed_data_verify. */
static void test_pkcs7_roundtrip_verified(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    if (!load_fixture_pkcs12_der(&der, &der_len)) {
        CS_CHECK(0, "fixture chargee (prerequis roundtrip)");
        return;
    }

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, FIXTURE_PASSWORD, &s);
    free(der);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis roundtrip)");
    if (!s) return;

    uint8_t* p7_der = NULL;
    size_t p7_len = 0;
    st = owl_code_sign_pkcs7(s, CONTENU_TEST, sizeof(CONTENU_TEST), &p7_der, &p7_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK && p7_der != NULL, "pkcs7 OK (prerequis roundtrip)");
    if (!p7_der) { owl_code_sign_free(s); return; }

    mbedtls_pkcs7 pkcs7;
    mbedtls_pkcs7_init(&pkcs7);
    int type = mbedtls_pkcs7_parse_der(&pkcs7, p7_der, p7_len);
    CS_CHECK(type == MBEDTLS_PKCS7_SIGNED_DATA,
             "reparse du DER produit -> type SIGNED_DATA reconnu");

    CS_CHECK(pkcs7.signed_data.no_of_signers == 1,
             "SignedData contient exactement 1 signataire");
    CS_CHECK(pkcs7.signed_data.no_of_certs >= 1,
             "SignedData embarque au moins 1 certificat (le signataire)");

    if (type == MBEDTLS_PKCS7_SIGNED_DATA &&
        pkcs7.signed_data.no_of_signers == 1 &&
        pkcs7.signed_data.no_of_certs >= 1) {

        mbedtls_pkcs7_signer_info* signer = &pkcs7.signed_data.signers;
        mbedtls_x509_crt* cert = &pkcs7.signed_data.certs;

        unsigned char digest[32];
        int rc = mbedtls_sha256(CONTENU_TEST, sizeof(CONTENU_TEST), digest, 0);
        CS_CHECK(rc == 0, "SHA-256 du contenu signe recalcule par le test");

        /* Oracle autoritaire : verification manuelle PK contre le certificat
         * embarque dans le SignedData (pas de dependance au verifieur PKCS#7
         * etroit de mbedTLS). */
        int vrc = mbedtls_pk_verify(&cert->pk, MBEDTLS_MD_SHA256,
                                     digest, sizeof(digest),
                                     signer->sig.p, signer->sig.len);
        CS_CHECK(vrc == 0,
                 "mbedtls_pk_verify : signature RSA valide contre la cle publique du certificat");

        /* Bonus (pas l'oracle) : le verifieur PKCS#7 natif de mbedTLS
         * doit lui aussi accepter la signature sur le meme contenu. */
        int bonus_rc = mbedtls_pkcs7_signed_data_verify(&pkcs7, cert,
                                                         CONTENU_TEST, sizeof(CONTENU_TEST));
        CS_CHECK(bonus_rc == 0,
                 "(bonus) mbedtls_pkcs7_signed_data_verify accepte aussi la signature");
    }

    mbedtls_pkcs7_free(&pkcs7);
    free(p7_der);
    owl_code_sign_free(s);
}

/* Cas 5 : TAMPER. Meme signature, contenu altere d'1 octet -> la
 * verification DOIT echouer. Anti-tautologie : prouve que le test teste
 * reellement le contenu signe, pas juste la forme du PKCS#7. */
static void test_pkcs7_tamper_detected(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    if (!load_fixture_pkcs12_der(&der, &der_len)) {
        CS_CHECK(0, "fixture chargee (prerequis tamper)");
        return;
    }

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, FIXTURE_PASSWORD, &s);
    free(der);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis tamper)");
    if (!s) return;

    uint8_t* p7_der = NULL;
    size_t p7_len = 0;
    st = owl_code_sign_pkcs7(s, CONTENU_TEST, sizeof(CONTENU_TEST), &p7_der, &p7_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK && p7_der != NULL, "pkcs7 OK (prerequis tamper)");
    if (!p7_der) { owl_code_sign_free(s); return; }

    mbedtls_pkcs7 pkcs7;
    mbedtls_pkcs7_init(&pkcs7);
    int type = mbedtls_pkcs7_parse_der(&pkcs7, p7_der, p7_len);
    CS_CHECK(type == MBEDTLS_PKCS7_SIGNED_DATA,
             "reparse du DER produit -> type SIGNED_DATA reconnu (cas tamper)");

    if (type == MBEDTLS_PKCS7_SIGNED_DATA &&
        pkcs7.signed_data.no_of_signers == 1 &&
        pkcs7.signed_data.no_of_certs >= 1) {

        mbedtls_pkcs7_signer_info* signer = &pkcs7.signed_data.signers;
        mbedtls_x509_crt* cert = &pkcs7.signed_data.certs;

        /* Contenu altere : 1 octet flippe par rapport a CONTENU_TEST. */
        unsigned char tampered[32];
        memcpy(tampered, CONTENU_TEST, sizeof(tampered));
        tampered[0] ^= 0xFF;

        unsigned char digest_tampered[32];
        int rc = mbedtls_sha256(tampered, sizeof(tampered), digest_tampered, 0);
        CS_CHECK(rc == 0, "SHA-256 du contenu altere calcule");

        int vrc = mbedtls_pk_verify(&cert->pk, MBEDTLS_MD_SHA256,
                                     digest_tampered, sizeof(digest_tampered),
                                     signer->sig.p, signer->sig.len);
        CS_CHECK(vrc != 0,
                 "mbedtls_pk_verify rejette la signature sur contenu altere (anti-tautologie)");

        /* Bonus : le verifieur PKCS#7 natif doit lui aussi rejeter. */
        int bonus_rc = mbedtls_pkcs7_signed_data_verify(&pkcs7, cert,
                                                         tampered, sizeof(tampered));
        CS_CHECK(bonus_rc != 0,
                 "(bonus) mbedtls_pkcs7_signed_data_verify rejette aussi le contenu altere");
    }

    mbedtls_pkcs7_free(&pkcs7);
    free(p7_der);
    owl_code_sign_free(s);
}

/* Cas 6 : ROUND-TRIP FICHIER `.p7s`. La signature detachee non-
 * Windows est ecrite TELLE QUELLE (DER) dans `<out>.p7s` a cote du binaire.
 * Ce test prouve que le DER produit survit a un aller-retour disque : on
 * l'ecrit, on le relit, on le reparse et on verifie la signature contre le
 * contenu d'origine. C'est exactement le chemin exerce par owl-build cote
 * cible linux/macOS. */
static void test_pkcs7_detached_file_roundtrip(void) {
    uint8_t* der = NULL;
    size_t der_len = 0;
    if (!load_fixture_pkcs12_der(&der, &der_len)) {
        CS_CHECK(0, "fixture chargee (prerequis roundtrip fichier .p7s)");
        return;
    }

    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(der, der_len, FIXTURE_PASSWORD, &s);
    free(der);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis roundtrip fichier)");
    if (!s) return;

    uint8_t* p7_der = NULL;
    size_t p7_len = 0;
    st = owl_code_sign_pkcs7(s, CONTENU_TEST, sizeof(CONTENU_TEST), &p7_der, &p7_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK && p7_der != NULL && p7_len > 0,
             "pkcs7 OK (prerequis roundtrip fichier)");
    if (!p7_der) { owl_code_sign_free(s); return; }

    /* Ecrit le DER dans un `.p7s` temporaire (cwd = racine repo pendant le
     * self-test), puis le relit integralement. */
    const char* p7s_path = "owl_codesign_roundtrip_tmp.p7s";
    FILE* wf = fopen(p7s_path, "wb");
    CS_CHECK(wf != NULL, ".p7s ouvert en ecriture");
    if (!wf) { free(p7_der); owl_code_sign_free(s); return; }
    size_t wr = fwrite(p7_der, 1, p7_len, wf);
    int wclose = fclose(wf);
    CS_CHECK(wr == p7_len && wclose == 0, ".p7s ecrit integralement sur disque");

    FILE* rf = fopen(p7s_path, "rb");
    CS_CHECK(rf != NULL, ".p7s relu depuis le disque");
    uint8_t* reread = NULL;
    size_t reread_len = 0;
    if (rf) {
        fseek(rf, 0, SEEK_END);
        long rsz = ftell(rf);
        fseek(rf, 0, SEEK_SET);
        if (rsz > 0) {
            reread = (uint8_t*)malloc((size_t)rsz);
            if (reread) reread_len = fread(reread, 1, (size_t)rsz, rf);
        }
        fclose(rf);
    }
    remove(p7s_path);

    /* Les octets relus doivent etre identiques a ceux produits (aucune
     * alteration a l'ecriture/lecture binaire). */
    CS_CHECK(reread != NULL && reread_len == p7_len
             && memcmp(reread, p7_der, p7_len) == 0,
             ".p7s relu identique octet pour octet au DER produit");

    /* Et le DER relu reste une signature valide : reparse + verification
     * manuelle contre le contenu d'origine. */
    if (reread != NULL && reread_len == p7_len) {
        mbedtls_pkcs7 pkcs7;
        mbedtls_pkcs7_init(&pkcs7);
        int type = mbedtls_pkcs7_parse_der(&pkcs7, reread, reread_len);
        CS_CHECK(type == MBEDTLS_PKCS7_SIGNED_DATA,
                 ".p7s relu -> reparse SIGNED_DATA OK");
        if (type == MBEDTLS_PKCS7_SIGNED_DATA &&
            pkcs7.signed_data.no_of_signers == 1 &&
            pkcs7.signed_data.no_of_certs >= 1) {
            mbedtls_pkcs7_signer_info* si = &pkcs7.signed_data.signers;
            mbedtls_x509_crt* cert = &pkcs7.signed_data.certs;
            unsigned char digest[32];
            mbedtls_sha256(CONTENU_TEST, sizeof(CONTENU_TEST), digest, 0);
            int vrc = mbedtls_pk_verify(&cert->pk, MBEDTLS_MD_SHA256,
                                         digest, sizeof(digest),
                                         si->sig.p, si->sig.len);
            CS_CHECK(vrc == 0, ".p7s relu depuis le disque -> signature toujours valide");
        }
        mbedtls_pkcs7_free(&pkcs7);
    }

    free(reread);
    free(p7_der);
    owl_code_sign_free(s);
}

/* Cas 7 : FORMAT SHA256SUMS. A cote du `.p7s`, owl-build ecrit un
 * `<out>.sha256` au format standard `<hex_minuscule>␠␠<nom_fichier>\n` (deux
 * espaces), consommable par `sha256sum -c`. Ce test verrouille l'algorithme
 * (SHA-256 des octets du contenu) ET le formatage exact de la ligne : valeur
 * de hash exacte, minuscule, 64 caracteres, double espace separateur. */
static void test_sha256sums_line_format(void) {
    unsigned char digest[32];
    int rc = mbedtls_sha256(CONTENU_TEST, sizeof(CONTENU_TEST), digest, 0);
    CS_CHECK(rc == 0, "SHA-256 du contenu calcule (format SHA256SUMS)");

    /* Formatage hex minuscule, identique a celui de owl-build. */
    char hex[65];
    static const char* HEXCHARS = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[i * 2]     = HEXCHARS[(digest[i] >> 4) & 0xF];
        hex[i * 2 + 1] = HEXCHARS[digest[i] & 0xF];
    }
    hex[64] = '\0';

    /* Valeur exacte attendue (oracle : sha256sum du contenu 32 octets fixe). */
    CS_CHECK(strcmp(hex, "77a76688a7b302a21a4d8b83955e3a67897ed3c0c09bb943fc4b12050d5d6673") == 0,
             "hex SHA-256 du contenu == valeur oracle attendue");
    CS_CHECK(strlen(hex) == 64, "hex SHA-256 fait exactement 64 caracteres");

    /* Ligne SHA256SUMS complete : <hex>␠␠<nom>\n (deux espaces). */
    char line[128];
    snprintf(line, sizeof(line), "%s  %s\n", hex, "owltest-app");
    CS_CHECK(strcmp(line,
             "77a76688a7b302a21a4d8b83955e3a67897ed3c0c09bb943fc4b12050d5d6673  owltest-app\n") == 0,
             "ligne SHA256SUMS == '<hex>  <nom>\\n' (double espace, format standard)");
}

int code_sign_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_load_pkcs12_ok();
    test_load_pkcs12_wrong_password();
    test_pkcs7_build_ok();
    test_pkcs7_roundtrip_verified();
    test_pkcs7_tamper_detected();
    test_pkcs7_detached_file_roundtrip();
    test_sha256sums_line_format();

    printf("code_sign self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
