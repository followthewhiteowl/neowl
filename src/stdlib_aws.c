// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_aws.c — AWS Signature Version 4 signing core (internal C helper).
 * Voir stdlib_aws.h pour le contrat et le contexte (cascade HMAC binaire).
 *
 * Depend UNIQUEMENT de mbedTLS + libc : ni VM ni registre, pour rester
 * lien-able isolement (smoke test standalone). Aucune surface OWL exposee.
 */

#include "stdlib_aws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/sha256.h"
#include "mbedtls/md.h"

#define AWS_SHA256_LEN 32           /* octets d'un digest SHA-256 */
#define AWS_HEX_LEN    (AWS_SHA256_LEN * 2)

static void aws_hex_encode(const unsigned char* in, size_t len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[len * 2] = '\0';
}

/* hex(SHA-256(data)) dans out (>= AWS_HEX_LEN+1). data peut etre NULL si len 0. */
static int aws_sha256_hex(const unsigned char* data, size_t len, char* out) {
    unsigned char digest[AWS_SHA256_LEN];
    const unsigned char empty = 0;
    if (data == NULL) data = &empty; /* input non-NULL meme pour len 0 */
    if (mbedtls_sha256(data, len, digest, 0) != 0) return -1;
    aws_hex_encode(digest, AWS_SHA256_LEN, out);
    return 0;
}

/* HMAC-SHA-256 binaire : out (>= 32 octets) = HMAC(key, msg). Aucun hex :
 * c'est la primitive de la cascade de derivation (sortie N = cle N+1). */
static int aws_hmac_sha256(const unsigned char* key, size_t keyLen,
                           const unsigned char* msg, size_t msgLen,
                           unsigned char out[AWS_SHA256_LEN]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) return -1;
    return mbedtls_md_hmac(info, key, keyLen, msg, msgLen, out);
}

int owl_aws_sigv4_sign(const OwlAwsSigV4Request* req, OwlAwsSigV4Headers* out) {
    if (req == NULL || out == NULL) return 1;
    if (!req->method || !req->canonicalUri || !req->canonicalQuery ||
        !req->canonicalHeaders || !req->signedHeaders ||
        !req->accessKeyId || !req->secretAccessKey ||
        !req->region || !req->service || !req->amzDate || !req->dateStamp) {
        return 1;
    }

    memset(out, 0, sizeof(*out));

    /* --- 1. Hash du payload -> X-Amz-Content-Sha256 --- */
    if (req->payloadMode == OWL_AWS_PAYLOAD_UNSIGNED) {
        snprintf(out->contentSha256, sizeof out->contentSha256, "UNSIGNED-PAYLOAD");
    } else {
        if (aws_sha256_hex(req->body, req->bodyLen, out->contentSha256) != 0) return 2;
    }

    /* --- 2. Canonical request ---
     * method\n uri\n query\n canonicalHeaders(\n-termine)\n signedHeaders\n payloadHash
     * canonicalHeaders finit deja par '\n' : le "%s\n" suivant ajoute le
     * separateur avant signedHeaders. */
    size_t crCap = strlen(req->method) + strlen(req->canonicalUri) +
                   strlen(req->canonicalQuery) + strlen(req->canonicalHeaders) +
                   strlen(req->signedHeaders) + strlen(out->contentSha256) + 8;
    char* cr = (char*)malloc(crCap);
    if (cr == NULL) return 3;
    int crn = snprintf(cr, crCap, "%s\n%s\n%s\n%s\n%s\n%s",
                       req->method, req->canonicalUri, req->canonicalQuery,
                       req->canonicalHeaders, req->signedHeaders,
                       out->contentSha256);
    if (crn < 0 || (size_t)crn >= crCap) { free(cr); return 3; }

    if (aws_sha256_hex((const unsigned char*)cr, (size_t)crn,
                       out->canonicalRequestSha256) != 0) {
        free(cr);
        return 4;
    }
    free(cr);

    /* --- 3. String to sign --- */
    char scope[256];
    int scn = snprintf(scope, sizeof scope, "%s/%s/%s/aws4_request",
                       req->dateStamp, req->region, req->service);
    if (scn < 0 || (size_t)scn >= sizeof scope) return 5;

    char sts[512];
    int stsn = snprintf(sts, sizeof sts, "AWS4-HMAC-SHA256\n%s\n%s\n%s",
                        req->amzDate, scope, out->canonicalRequestSha256);
    if (stsn < 0 || (size_t)stsn >= sizeof sts) return 5;

    /* --- 4. Cascade de derivation de cle (BINAIRE, sans hexer) --- */
    unsigned char kSecret[4 + 256];
    size_t secLen = strlen(req->secretAccessKey);
    if (secLen > 256) return 6;
    memcpy(kSecret, "AWS4", 4);
    memcpy(kSecret + 4, req->secretAccessKey, secLen);

    unsigned char kDate[AWS_SHA256_LEN];
    unsigned char kRegion[AWS_SHA256_LEN];
    unsigned char kService[AWS_SHA256_LEN];
    unsigned char kSigning[AWS_SHA256_LEN];

    if (aws_hmac_sha256(kSecret, 4 + secLen,
                        (const unsigned char*)req->dateStamp,
                        strlen(req->dateStamp), kDate) != 0) return 6;
    if (aws_hmac_sha256(kDate, AWS_SHA256_LEN,
                        (const unsigned char*)req->region,
                        strlen(req->region), kRegion) != 0) return 6;
    if (aws_hmac_sha256(kRegion, AWS_SHA256_LEN,
                        (const unsigned char*)req->service,
                        strlen(req->service), kService) != 0) return 6;
    if (aws_hmac_sha256(kService, AWS_SHA256_LEN,
                        (const unsigned char*)"aws4_request", 12,
                        kSigning) != 0) return 6;

    /* --- 5. Signature = hex(HMAC(kSigning, stringToSign)) --- */
    unsigned char sig[AWS_SHA256_LEN];
    if (aws_hmac_sha256(kSigning, AWS_SHA256_LEN,
                        (const unsigned char*)sts, (size_t)stsn, sig) != 0) return 7;
    aws_hex_encode(sig, AWS_SHA256_LEN, out->signature);

    /* --- 6. Header Authorization --- */
    int an = snprintf(out->authorization, sizeof out->authorization,
                      "AWS4-HMAC-SHA256 Credential=%s/%s, "
                      "SignedHeaders=%s, Signature=%s",
                      req->accessKeyId, scope, req->signedHeaders, out->signature);
    if (an < 0 || (size_t)an >= sizeof out->authorization) return 8;

    /* --- 7. Headers annexes --- */
    if (snprintf(out->amzDate, sizeof out->amzDate, "%s", req->amzDate)
            >= (int)sizeof out->amzDate) {
        return 8;
    }
    if (req->sessionToken != NULL && req->sessionToken[0] != '\0') {
        if (snprintf(out->securityToken, sizeof out->securityToken, "%s",
                     req->sessionToken) >= (int)sizeof out->securityToken) {
            return 8;
        }
        out->hasSecurityToken = 1;
    } else {
        out->securityToken[0] = '\0';
        out->hasSecurityToken = 0;
    }

    return 0;
}
