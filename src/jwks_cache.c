// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* jwks_cache.c — cache de cles JWKS + fetch cache-first borne.
 *
 * Voir jwks_cache.h pour le contrat. Le module tourne EXCLUSIVEMENT sur le
 * thread VM (chemin d'auth), donc aucun verrou : le cache global est touche
 * par un seul thread, le single-flight est automatique. Il tient les cles RSA
 * deja parsees (mbedtls_pk_context, C-heap) — le hot path est 0 reseau + 0
 * parse.
 *
 * SECURITE (fail-closed) : n'importe quelle anomalie (fetch en echec, JSON
 * malforme, kty non-RSA, use=enc, base64url hostile, kid absent) reduit la
 * resolution a JWKS_KID_INCONNU / JWKS_INJOIGNABLE — jamais une cle acceptee a
 * tort. La verification de signature/exp/iss reste faite par l'appelant.
 */
#include "jwks_cache.h"
#include "owl_stdlib.h"   /* owl_http_get_borne */
#include "object.h"       /* ObjJson / ObjString / Value / OBJ_VAL */
#include "vm.h"           /* vm_push / vm_pop (rooting GC pendant le parse JSON) */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mbedtls/rsa.h"

/* parse JSON en C (meme seam que http_exec.c) : declare ici pour garder la TU
 * minimale (stdlib_json.h aspirerait d'autres dependances). */
extern ObjJson* json_parse_cstr(const char* src, int len);

/* Fenetres temporelles (secondes). TTL = fenetre de fraicheur pur-cache ;
 * rate-limit = anti-tempete de fetch sur kid-miss ; hard-max = peremption dure
 * du serve-stale (au-dela, fail-closed meme avec un cache). */
#define JWKS_TTL_SEC          3600      /* 1 h */
#define JWKS_RATE_LIMIT_SEC   60        /* 1 fetch / 60 s / URL */
#define JWKS_HARD_MAX_SEC     86400     /* 24 h */

#define JWKS_FETCH_TIMEOUT_MS 5000      /* timeout dur du GET (chemin d'auth) */
#define JWKS_MAX_N_BYTES      1024      /* modulus jusqu'a 8192 bits */
#define JWKS_MAX_E_BYTES      16        /* exposant public (typiquement 3 octets) */

typedef struct {
    char*              kid;
    mbedtls_pk_context pk;   /* cle RSA publique parsee, possedee */
} JwksKey;

typedef struct {
    char*    url;               /* cle du cache */
    JwksKey* keys;
    int      keyCount;
    time_t   fetchedAt;         /* dernier fetch REUSSI */
    time_t   lastFetchAttempt;  /* dernier fetch TENTE (rate-limit) */
    int      populated;         /* au moins un fetch reussi dans la vie de l'entree */
} JwksEntry;

static JwksEntry* g_entries = NULL;
static int        g_count   = 0;
static int        g_cap     = 0;

static time_t jwks_now(void) { return time(NULL); }

/* ---- base64url strict (RFC 4648 §5, sans '+' '/', padding '=' optionnel) ----
 * Local et self-contained : le module ne partage pas le decodeur de http_exec.c
 * (static la-bas). Toute anomalie -> -1, jamais d'ecriture hors `out`. */
static int jwks_b64url_decode(const char* in, size_t inlen,
                              unsigned char* out, size_t outcap, size_t* outlen) {
    unsigned int acc = 0;
    int nbits = 0, ngroups = 0, npad = 0;
    size_t j = 0;
    for (size_t i = 0; i < inlen; i++) {
        char ch = in[i];
        if (ch == '=') { npad++; continue; }
        if (npad > 0) return -1;
        int v;
        if      (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '-')              v = 62;
        else if (ch == '_')              v = 63;
        else return -1;
        acc = (acc << 6) | (unsigned int)v;
        nbits += 6;
        ngroups++;
        if (nbits >= 8) {
            nbits -= 8;
            if (j >= outcap) return -1;
            out[j++] = (unsigned char)((acc >> nbits) & 0xFF);
        }
    }
    if (npad > 2) return -1;
    if (ngroups % 4 == 1) return -1;
    *outlen = j;
    return 0;
}

static void jwks_key_free(JwksKey* k) {
    free(k->kid);
    k->kid = NULL;
    mbedtls_pk_free(&k->pk);
}

static void jwks_entry_free_keys(JwksEntry* e) {
    for (int i = 0; i < e->keyCount; i++) jwks_key_free(&e->keys[i]);
    free(e->keys);
    e->keys = NULL;
    e->keyCount = 0;
}

void owl_jwks_cache_reset(void) {
    for (int i = 0; i < g_count; i++) {
        jwks_entry_free_keys(&g_entries[i]);
        free(g_entries[i].url);
    }
    free(g_entries);
    g_entries = NULL;
    g_count = 0;
    g_cap = 0;
}

static JwksEntry* jwks_entry_find_or_create(const char* url) {
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_entries[i].url, url) == 0) return &g_entries[i];
    }
    if (g_count >= g_cap) {
        int nc = g_cap == 0 ? 4 : g_cap * 2;
        JwksEntry* ne = (JwksEntry*)realloc(g_entries, (size_t)nc * sizeof(JwksEntry));
        if (ne == NULL) return NULL;
        g_entries = ne;
        g_cap = nc;
    }
    JwksEntry* e = &g_entries[g_count];
    memset(e, 0, sizeof(*e));
    e->url = (char*)malloc(strlen(url) + 1);
    if (e->url == NULL) return NULL;
    strcpy(e->url, url);
    g_count++;
    return e;
}

static JwksKey* jwks_key_find(JwksEntry* e, const char* kid) {
    if (kid == NULL) return NULL;
    for (int i = 0; i < e->keyCount; i++) {
        if (e->keys[i].kid != NULL && strcmp(e->keys[i].kid, kid) == 0)
            return &e->keys[i];
    }
    return NULL;
}

/* Lit un champ chaine d'un noeud JSON objet dans `out` (NUL-termine, tronque a
 * outcap-1). Retourne 1 si present et de type chaine, 0 sinon. La cle ObjString
 * est allouee sous le root deja pose par l'appelant. */
static int jwks_json_str(ObjJson* obj, const char* field, char* out, size_t outcap) {
    out[0] = '\0';
    if (obj == NULL || obj->jtype != JSON_T_OBJECT) return 0;
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    vm_push(OBJ_VAL(k));
    ObjJson* node = obj_json_object_get(obj, k);
    vm_pop();
    if (node == NULL || node->jtype != JSON_T_STRING || node->as.strVal == NULL)
        return 0;
    size_t n = (size_t)node->as.strVal->length;
    if (n >= outcap) n = outcap - 1;
    memcpy(out, node->as.strVal->chars, n);
    out[n] = '\0';
    return 1;
}

/* Construit une cle RSA publique depuis (n, e) base64url et l'ajoute a l'entree
 * sous `kid`. Retourne 0 en succes. Toute anomalie -> rien ajoute, != 0. */
static int jwks_add_rsa_key(JwksEntry* e, const char* kid,
                            const char* n_b64, const char* e_b64) {
    unsigned char nbuf[JWKS_MAX_N_BYTES];
    unsigned char ebuf[JWKS_MAX_E_BYTES];
    size_t nlen = 0, elen = 0;
    if (jwks_b64url_decode(n_b64, strlen(n_b64), nbuf, sizeof(nbuf), &nlen) != 0) return -1;
    if (jwks_b64url_decode(e_b64, strlen(e_b64), ebuf, sizeof(ebuf), &elen) != 0) return -1;
    if (nlen == 0 || elen == 0) return -1;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ok = 0;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
        mbedtls_rsa_context* rsa = mbedtls_pk_rsa(pk);
        if (mbedtls_rsa_import_raw(rsa, nbuf, nlen, NULL, 0, NULL, 0, NULL, 0,
                                   ebuf, elen) == 0 &&
            mbedtls_rsa_complete(rsa) == 0) {
            ok = 1;
        }
    }
    if (!ok) { mbedtls_pk_free(&pk); return -1; }

    JwksKey* nk = (JwksKey*)realloc(e->keys, (size_t)(e->keyCount + 1) * sizeof(JwksKey));
    if (nk == NULL) { mbedtls_pk_free(&pk); return -1; }
    e->keys = nk;
    JwksKey* slot = &e->keys[e->keyCount];
    slot->kid = (char*)malloc(strlen(kid) + 1);
    if (slot->kid == NULL) { mbedtls_pk_free(&pk); return -1; }
    strcpy(slot->kid, kid);
    slot->pk = pk;   /* transfert de propriete du contexte */
    e->keyCount++;
    return 0;
}

/* Parse un corps JWKS et REMPLACE les cles de l'entree par celles qu'il decrit
 * (seules les cles RSA de signature valides sont retenues ; kty!=RSA, use=enc,
 * n/e absents ou malformes sont SILENCIEUSEMENT ignores — jamais utilises comme
 * cle RSA). Retourne 0 si le corps etait un JSON JWKS exploitable (meme 0 cle
 * retenue), != 0 si le JSON est illisible (l'appelant garde alors l'ancien
 * cache). Rooting GC : l'arbre JSON est epingle pendant toute la traversee. */
static int jwks_parse_and_install(JwksEntry* e, const char* body, size_t bodylen) {
    ObjJson* root = json_parse_cstr(body, (int)bodylen);
    if (root == NULL) return -1;
    vm_push(OBJ_VAL(root));

    if (root->jtype != JSON_T_OBJECT) { vm_pop(); return -1; }

    ObjString* keysKey = obj_copy_string("keys", 4);
    vm_push(OBJ_VAL(keysKey));
    ObjJson* keysNode = obj_json_object_get(root, keysKey);
    vm_pop();
    if (keysNode == NULL || keysNode->jtype != JSON_T_ARRAY) { vm_pop(); return -1; }

    /* JSON valide : on adopte cette vue et on remplace le cache. */
    jwks_entry_free_keys(e);

    for (int i = 0; i < keysNode->as.arrayVal.count; i++) {
        ObjJson* jwk = keysNode->as.arrayVal.items[i];
        if (jwk == NULL || jwk->jtype != JSON_T_OBJECT) continue;

        char kty[16], use[16], alg[16], kid[256], nb[2048], eb[64];
        jwks_json_str(jwk, "kty", kty, sizeof(kty));
        if (strcmp(kty, "RSA") != 0) continue;                 /* type de cle exige */
        jwks_json_str(jwk, "use", use, sizeof(use));
        if (strcmp(use, "enc") == 0) continue;                 /* cle de chiffrement rejetee */
        jwks_json_str(jwk, "alg", alg, sizeof(alg));
        if (alg[0] != '\0' && strcmp(alg, "RS256") != 0) continue; /* alg incompatible */
        if (!jwks_json_str(jwk, "kid", kid, sizeof(kid))) continue; /* kid exige */
        if (!jwks_json_str(jwk, "n", nb, sizeof(nb)))     continue;
        if (!jwks_json_str(jwk, "e", eb, sizeof(eb)))     continue;

        jwks_add_rsa_key(e, kid, nb, eb);   /* echec -> cle simplement ignoree */
    }

    vm_pop();   /* root */
    return 0;
}

/* Fetch borne + parse. Retourne 0 si une reponse 200 JSON exploitable a
 * remplace le cache, != 0 sinon (l'appelant garde l'ancien cache). */
static int jwks_fetch(JwksEntry* e, const char* url) {
    char*  body = NULL;
    size_t blen = 0;
    int    status = 0;
    if (owl_http_get_borne(url, JWKS_FETCH_TIMEOUT_MS, &body, &blen, &status) != 0)
        return -1;
    int rc = -1;
    if (status == 200 && body != NULL) {
        rc = jwks_parse_and_install(e, body, blen);
    }
    free(body);
    return rc;
}

JwksResolution owl_jwks_resolve(const char* url, const char* kid,
                                mbedtls_pk_context** out_pk) {
    if (out_pk) *out_pk = NULL;
    if (url == NULL) return JWKS_INJOIGNABLE;

    JwksEntry* e = jwks_entry_find_or_create(url);
    if (e == NULL) return JWKS_INJOIGNABLE;

    time_t now = jwks_now();

    /* 1. Cache-hit frais : kid present ET cache dans la fenetre TTL -> 0 reseau. */
    if (e->populated && kid != NULL) {
        JwksKey* k = jwks_key_find(e, kid);
        if (k != NULL && (now - e->fetchedAt) <= JWKS_TTL_SEC) {
            if (out_pk) *out_pk = &k->pk;
            return JWKS_OK;
        }
    }

    /* 2. Refetch quand utile (cold, kid-miss, ou cache perime), rate-limite. */
    int doit_fetch = 0;
    if (!e->populated) {
        doit_fetch = 1;                                  /* cold */
    } else if (jwks_key_find(e, kid) == NULL) {
        doit_fetch = 1;                                  /* kid-miss : capter une rotation */
    } else if ((now - e->fetchedAt) > JWKS_TTL_SEC) {
        doit_fetch = 1;                                  /* perime : rafraichir */
    }
    if (doit_fetch && (e->lastFetchAttempt == 0 ||
                       (now - e->lastFetchAttempt) >= JWKS_RATE_LIMIT_SEC)) {
        e->lastFetchAttempt = now;
        if (jwks_fetch(e, url) == 0) {
            e->fetchedAt = now;
            e->populated = 1;
        }
        /* echec : l'ancien cache (s'il existe) reste en place -> serve-stale. */
    }

    /* 3. Servir depuis le cache (frais OU stale dans le hard-max de peremption). */
    if (e->populated && kid != NULL) {
        JwksKey* k = jwks_key_find(e, kid);
        if (k != NULL) {
            if ((now - e->fetchedAt) <= JWKS_HARD_MAX_SEC) {
                if (out_pk) *out_pk = &k->pk;
                return JWKS_OK;
            }
            return JWKS_INJOIGNABLE;   /* trop perime -> fail-closed */
        }
        return JWKS_KID_INCONNU;       /* fetch ok mais kid absent */
    }
    return JWKS_INJOIGNABLE;           /* aucune cle disponible */
}
