// SPDX-License-Identifier: AGPL-3.0-only
/* aws_sigv4_smoketest.c — validation byte-exacte du coeur AWS Signature V4.
 *
 * Oracle : l'exemple canonique documente par AWS pour le processus de
 * signature SigV4 (la requete de reference GET sur le service iam,
 * region us-east-1). AWS publie CHAQUE valeur intermediaire de cet
 * exemple : la canonical request, son hash SHA-256, le string-to-sign,
 * et la signature finale. On les encode ici en assertions byte-exactes.
 *
 * Parametres de l'exemple (cle secrete/accessKey de test publiques AWS) :
 *   accessKeyId = AKIDEXAMPLE
 *   secretKey   = wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY
 *   region      = us-east-1
 *   service     = iam
 *   amzDate     = 20150830T123600Z   dateStamp = 20150830
 *   requete     = GET https://iam.amazonaws.com/?Action=ListUsers&Version=2010-05-08
 *   headers     = content-type ; host ; x-amz-date   (corps vide)
 *
 * Aucun framework : exit 0 si tout passe, != 0 + message sinon (pattern
 * argon2_smoketest / zint_smoketest). Le smoke test COMPILE avant l'impl
 * (digue test-first) et rougit tant que owl_aws_sigv4_sign renvoie != 0
 * ou une valeur divergente.
 */

#include <stdio.h>
#include <string.h>
#include "stdlib_aws.h"

/* SHA-256 du corps vide (universellement connu). */
#define EMPTY_SHA256 \
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

/* Hash SHA-256 de la canonical request de l'exemple (valeur AWS documentee). */
#define EXPECTED_CANONICAL_REQUEST_SHA256 \
    "f536975d06c0309214f805bb90ccff089219ecd68b2577efef23edd43b7e1a59"

/* Signature finale de l'exemple (valeur AWS documentee). */
#define EXPECTED_SIGNATURE \
    "5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7"

/* Header Authorization complet attendu. */
#define EXPECTED_AUTHORIZATION \
    "AWS4-HMAC-SHA256 " \
    "Credential=AKIDEXAMPLE/20150830/us-east-1/iam/aws4_request, " \
    "SignedHeaders=content-type;host;x-amz-date, " \
    "Signature=5d672d79c15b13162d9279b0855cfba6789a8edb4c82c400e06b5924a6f2b5d7"

static int check_eq(const char* label, const char* got, const char* expected,
                    int base_code) {
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "aws_sigv4_smoketest: %s divergent\n"
                        "  attendu : %s\n  obtenu  : %s\n",
                label, expected, got);
        return base_code;
    }
    printf("  %s OK\n", label);
    return 0;
}

static int test_canonical_example(void) {
    OwlAwsSigV4Request req;
    OwlAwsSigV4Headers out;
    memset(&req, 0, sizeof req);
    memset(&out, 0, sizeof out);

    req.method       = "GET";
    req.canonicalUri = "/";
    /* Query deja triee/encodee. */
    req.canonicalQuery = "Action=ListUsers&Version=2010-05-08";
    /* Bloc de headers canoniques : chaque ligne finit par '\n'. */
    req.canonicalHeaders =
        "content-type:application/x-www-form-urlencoded; charset=utf-8\n"
        "host:iam.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n";
    req.signedHeaders = "content-type;host;x-amz-date";
    req.body          = NULL;
    req.bodyLen       = 0;
    req.payloadMode   = OWL_AWS_PAYLOAD_SIGNED;

    req.accessKeyId     = "AKIDEXAMPLE";
    req.secretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    req.sessionToken    = NULL;
    req.region          = "us-east-1";
    req.service         = "iam";
    req.amzDate         = "20150830T123600Z";
    req.dateStamp       = "20150830";

    int rc = owl_aws_sigv4_sign(&req, &out);
    if (rc != 0) {
        fprintf(stderr, "aws_sigv4_smoketest: owl_aws_sigv4_sign rc=%d\n", rc);
        return 10;
    }

    int e = 0;
    /* Corps vide -> X-Amz-Content-Sha256 = SHA-256("") */
    e = check_eq("X-Amz-Content-Sha256 (corps vide)", out.contentSha256,
                 EMPTY_SHA256, 11); if (e) return e;
    e = check_eq("X-Amz-Date", out.amzDate, "20150830T123600Z", 12); if (e) return e;
    e = check_eq("hash canonical request", out.canonicalRequestSha256,
                 EXPECTED_CANONICAL_REQUEST_SHA256, 13); if (e) return e;
    e = check_eq("signature", out.signature, EXPECTED_SIGNATURE, 14); if (e) return e;
    e = check_eq("Authorization", out.authorization,
                 EXPECTED_AUTHORIZATION, 15); if (e) return e;

    if (out.hasSecurityToken != 0) {
        fprintf(stderr, "aws_sigv4_smoketest: hasSecurityToken=%d, attendu 0 "
                        "(pas de jeton de session)\n", out.hasSecurityToken);
        return 16;
    }
    printf("  pas de X-Amz-Security-Token (aucun jeton fourni) OK\n");
    return 0;
}

/* Variante avec jeton de session + UNSIGNED-PAYLOAD : on ne connait pas la
 * signature oracle de ce cas, donc on n'asserte QUE ce qui est deterministe
 * et connu a 100 % — la propagation du jeton et le litteral UNSIGNED-PAYLOAD.
 * Pas d'assertion crypto inventee ici. */
static int test_session_token_and_unsigned(void) {
    OwlAwsSigV4Request req;
    OwlAwsSigV4Headers out;
    memset(&req, 0, sizeof req);
    memset(&out, 0, sizeof out);

    req.method         = "GET";
    req.canonicalUri   = "/";
    req.canonicalQuery = "";
    req.canonicalHeaders =
        "host:example.amazonaws.com\n"
        "x-amz-date:20150830T123600Z\n";
    req.signedHeaders = "host;x-amz-date";
    req.body          = NULL;
    req.bodyLen       = 0;
    req.payloadMode   = OWL_AWS_PAYLOAD_UNSIGNED;

    req.accessKeyId     = "AKIDEXAMPLE";
    req.secretAccessKey = "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY";
    req.sessionToken    = "AQoDYXdzEXAMPLETOKEN123";
    req.region          = "us-east-1";
    req.service         = "iam";
    req.amzDate         = "20150830T123600Z";
    req.dateStamp       = "20150830";

    int rc = owl_aws_sigv4_sign(&req, &out);
    if (rc != 0) {
        fprintf(stderr, "aws_sigv4_smoketest: sign (token/unsigned) rc=%d\n", rc);
        return 20;
    }

    int e = 0;
    e = check_eq("X-Amz-Content-Sha256 (UNSIGNED-PAYLOAD)", out.contentSha256,
                 "UNSIGNED-PAYLOAD", 21); if (e) return e;
    if (out.hasSecurityToken != 1) {
        fprintf(stderr, "aws_sigv4_smoketest: hasSecurityToken=%d, attendu 1\n",
                out.hasSecurityToken);
        return 22;
    }
    e = check_eq("X-Amz-Security-Token", out.securityToken,
                 "AQoDYXdzEXAMPLETOKEN123", 23); if (e) return e;
    /* La signature doit exister (64 hex) meme si on ne connait pas sa valeur. */
    if (strlen(out.signature) != 64) {
        fprintf(stderr, "aws_sigv4_smoketest: signature len=%zu, attendu 64\n",
                strlen(out.signature));
        return 24;
    }
    printf("  jeton de session propage + UNSIGNED-PAYLOAD OK\n");
    return 0;
}

int main(void) {
    int rc;
    rc = test_canonical_example();
    if (rc) return rc;
    rc = test_session_token_and_unsigned();
    if (rc) return rc;
    printf("aws_sigv4_smoketest OK (vecteur officiel AWS SigV4, byte-exact)\n");
    return 0;
}
