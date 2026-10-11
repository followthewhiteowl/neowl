// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_aws.h — AWS Signature Version 4 signing core (internal C helper).
 *
 * Fondation crypto du chantier AWS : calcule une signature SigV4 complete
 * (canonical request -> string-to-sign -> derivation de cle en cascade
 * binaire -> header Authorization), sur la crypto mbedTLS deja vendoree.
 *
 * AUCUNE surface OWL publique ici : pas de native exposee. Les natives qui
 * consomment ce helper viennent dans un ticket ulterieur. Ce header ne
 * depend que de <stddef.h> et l'implementation que de mbedTLS + libc — il
 * ne tire ni la VM ni le registre, pour rester lien-able isolement (smoke
 * test standalone).
 *
 * Contrainte cle (le "trou HMAC binaire") : la cascade
 *   kDate    = HMAC(kSecret,  dateStamp)
 *   kRegion  = HMAC(kDate,    region)
 *   kService = HMAC(kRegion,  service)
 *   kSigning = HMAC(kService, "aws4_request")
 * se fait entierement en binaire (sortie HMAC N = cle HMAC N+1), via
 * mbedtls_md_hmac, SANS hexer entre les etapes. Aucun octet binaire de cle
 * ne transite par une string : tout reste en unsigned char[].
 */

#ifndef OWL_STDLIB_AWS_H
#define OWL_STDLIB_AWS_H

#include <stddef.h>

/* Mode de signature du payload -> valeur du header X-Amz-Content-Sha256. */
typedef enum {
    OWL_AWS_PAYLOAD_SIGNED   = 0, /* hex(SHA256(body)) */
    OWL_AWS_PAYLOAD_UNSIGNED = 1  /* litteral "UNSIGNED-PAYLOAD" */
} OwlAwsPayloadMode;

/* Entree du signeur. Toutes les chaines sont des C-strings NUL-terminees.
 *
 * Le coeur construit lui-meme la canonical request a partir des morceaux
 * ci-dessous. Pour rester deterministe et minimal, il attend le bloc de
 * headers canoniques DEJA normalise par l'appelant :
 *   - canonicalHeaders : suite de lignes "name:value\n", noms en minuscules,
 *     tries par nom, valeurs trimmees ; CHAQUE ligne finit par '\n' (le
 *     coeur ajoute lui-meme le '\n' separateur avant signedHeaders).
 *   - signedHeaders    : liste des noms signes, minuscules, tries,
 *     joints par ';' (ex: "content-type;host;x-amz-date").
 * Le tri / la normalisation des headers relevent d'une couche superieure
 * (ticket ulterieur) — hors scope de ce coeur crypto. */
typedef struct {
    const char* method;           /* "GET", "POST", ... */
    const char* canonicalUri;     /* ex "/" (deja URI-encode) */
    const char* canonicalQuery;   /* ex "Action=ListUsers&Version=..." ou "" */
    const char* canonicalHeaders; /* lignes "name:value\n" (voir ci-dessus) */
    const char* signedHeaders;    /* "content-type;host;x-amz-date" */
    const unsigned char* body;    /* corps de la requete (NULL si bodyLen==0) */
    size_t bodyLen;
    OwlAwsPayloadMode payloadMode;

    const char* accessKeyId;      /* "AKIDEXAMPLE" */
    const char* secretAccessKey;  /* "wJalr..." */
    const char* sessionToken;     /* optionnel ; NULL si absent */
    const char* region;           /* "us-east-1" */
    const char* service;          /* "iam" */
    const char* amzDate;          /* "20150830T123600Z" (ISO8601 basic) */
    const char* dateStamp;        /* "20150830" (YYYYMMDD) */
} OwlAwsSigV4Request;

/* Sortie : les headers signes a ajouter a la requete HTTP.
 * Buffers possedes par l'appelant ; le signeur ecrit des C-strings.
 * canonicalRequestSha256 et signature sont exposes pour l'observabilite
 * (log/debug) et la validation contre les vecteurs officiels. */
typedef struct {
    char authorization[1024];         /* header Authorization complet */
    char amzDate[32];                 /* echo de amzDate */
    char contentSha256[80];           /* hex(SHA256(body)) ou "UNSIGNED-PAYLOAD" */
    char securityToken[4096];         /* copie de sessionToken, vide si absent */
    int  hasSecurityToken;            /* 1 si un jeton de session est present */

    char canonicalRequestSha256[80];  /* hex du hash de la canonical request */
    char signature[80];               /* hex de la signature finale */
} OwlAwsSigV4Headers;

/* Signe la requete. Retourne 0 en cas de succes, non-zero sinon
 * (argument manquant / echec crypto / debordement de buffer). */
int owl_aws_sigv4_sign(const OwlAwsSigV4Request* req, OwlAwsSigV4Headers* out);

#endif /* OWL_STDLIB_AWS_H */
