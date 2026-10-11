// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "http_service_runtime.h"
#include "jwks_cache.h"   /*  : le cache JWKS suit le cycle de reset des routes */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Registre process-global des routes runtime. Volontairement independant du
 * cycle de la VM et du reset de compilation POO : cf. http_service_runtime.h. */
static OwlServiceRuntimeRoute* g_routes = NULL;
static int g_route_count = 0;
static int g_route_capacity = 0;

static char* dup_cstr(const char* s) {
    if (s == NULL) s = "";
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

void owl_service_runtime_reset(void) {
    for (int i = 0; i < g_route_count; i++) {
        OwlServiceRuntimeRoute* r = &g_routes[i];
        free(r->verb);
        free(r->path);
        free(r->handlerGlobal);
        free(r->authScheme);
        free(r->authHandlerGlobal);
        free(r->errorHandlerGlobal);
        free(r->authzHandlerGlobal);
        free(r->jwtPublicKeyPem);
        free(r->jwtJwksUrl);
        free(r->jwtIssuer);
        free(r->jwtAudience);
        free(r->flux);
        for (int s = 0; s < r->segCount; s++) free(r->segments[s]);
        free(r->segments);
        free(r->segIsParam);
        for (int p = 0; p < r->paramCount; p++) {
            free(r->params[p].name);
            free(r->params[p].typeName);
        }
        free(r->params);
    }
    free(g_routes);
    g_routes = NULL;
    g_route_count = 0;
    g_route_capacity = 0;
    owl_jwks_cache_reset();   /*  : les cles cachees suivent les routes */
}

int owl_service_runtime_add(const char* verb, const char* path,
                            const char* handlerGlobal, int hasParam) {
    if (verb == NULL || path == NULL || handlerGlobal == NULL) return -1;
    if (g_route_count >= g_route_capacity) {
        int cap = g_route_capacity == 0 ? 8 : g_route_capacity * 2;
        g_routes = (OwlServiceRuntimeRoute*)realloc(
            g_routes, (size_t)cap * sizeof(OwlServiceRuntimeRoute));
        g_route_capacity = cap;
    }
    int idx = g_route_count++;
    OwlServiceRuntimeRoute* r = &g_routes[idx];
    memset(r, 0, sizeof(*r));
    r->verb          = dup_cstr(verb);
    r->path          = dup_cstr(path);
    r->handlerGlobal = dup_cstr(handlerGlobal);
    r->hasParam      = hasParam;
    return idx;
}

void owl_service_runtime_add_segment(int routeIdx, const char* segment,
                                     int isParam) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    int n = r->segCount + 1;
    r->segments   = (char**)realloc(r->segments, (size_t)n * sizeof(char*));
    r->segIsParam = (unsigned char*)realloc(r->segIsParam, (size_t)n);
    r->segments[r->segCount]   = dup_cstr(segment);
    /* isParam porte la NATURE du segment : 0 = litteral, 1 = parametre nomme,
     * 2 = parametre catch-all glouton. Valeur brute conservee : la
     * distinction 1 vs 2 pilote le matching longueur-variable et le binding. */
    r->segIsParam[r->segCount] = (unsigned char)(isParam < 0 ? 0 :
                                                 isParam > 2 ? 2 : isParam);
    r->segCount = n;
}

void owl_service_runtime_add_param(int routeIdx, const char* name, int source,
                                   const char* typeName, int isStructured,
                                   int hasDefault, int pathIndex, int fileUpload) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    int n = r->paramCount + 1;
    r->params = (OwlServiceRuntimeParam*)realloc(
        r->params, (size_t)n * sizeof(OwlServiceRuntimeParam));
    OwlServiceRuntimeParam* p = &r->params[r->paramCount];
    p->name         = dup_cstr(name);
    p->source       = source;
    p->typeName     = typeName ? dup_cstr(typeName) : NULL;
    p->isStructured = isStructured;
    p->hasDefault   = hasDefault;
    p->pathIndex    = pathIndex;
    p->fileUpload   = fileUpload;
    r->paramCount   = n;
}

void owl_service_runtime_set_auth(int routeIdx, const char* scheme,
                                  const char* handlerGlobal, int optOut) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    free(r->authScheme);
    free(r->authHandlerGlobal);
    r->authScheme        = scheme ? dup_cstr(scheme) : NULL;
    r->authHandlerGlobal = handlerGlobal ? dup_cstr(handlerGlobal) : NULL;
    r->authOptOut        = optOut ? 1 : 0;
}

void owl_service_runtime_set_error_hook(int routeIdx, const char* handlerGlobal) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    free(r->errorHandlerGlobal);
    r->errorHandlerGlobal = handlerGlobal ? dup_cstr(handlerGlobal) : NULL;
}

void owl_service_runtime_set_authz_hook(int routeIdx, const char* handlerGlobal) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    free(r->authzHandlerGlobal);
    r->authzHandlerGlobal = handlerGlobal ? dup_cstr(handlerGlobal) : NULL;
}

void owl_service_runtime_set_jwt(int routeIdx, const char* publicKeyPem,
                                 const char* jwksUrl,
                                 const char* issuer, const char* audience) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    free(r->jwtPublicKeyPem);
    free(r->jwtJwksUrl);
    free(r->jwtIssuer);
    free(r->jwtAudience);
    r->jwtPublicKeyPem = publicKeyPem ? dup_cstr(publicKeyPem) : NULL;
    r->jwtJwksUrl      = jwksUrl ? dup_cstr(jwksUrl) : NULL;
    r->jwtIssuer       = issuer ? dup_cstr(issuer) : NULL;
    r->jwtAudience     = audience ? dup_cstr(audience) : NULL;
}

void owl_service_runtime_set_default_status(int routeIdx, int status) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    g_routes[routeIdx].defaultStatus = status;
}

void owl_service_runtime_set_flux(int routeIdx, const char* flux, int delaiMax) {
    if (routeIdx < 0 || routeIdx >= g_route_count) return;
    OwlServiceRuntimeRoute* r = &g_routes[routeIdx];
    free(r->flux);
    r->flux     = flux ? dup_cstr(flux) : NULL;
    r->delaiMax = delaiMax;
}

int owl_service_runtime_count(void) { return g_route_count; }

const OwlServiceRuntimeRoute* owl_service_runtime_at(int index) {
    if (index < 0 || index >= g_route_count) return NULL;
    return &g_routes[index];
}

/* Compare la cible d'une requete au chemin d'une route : la query string
 * (`?...`) et un slash final surnumeraire ne cassent pas l'appariement. */
static int path_matches(const char* routePath, const char* target) {
    size_t tlen = 0;
    while (target[tlen] != '\0' && target[tlen] != '?' && target[tlen] != '#') tlen++;
    size_t rlen = strlen(routePath);
    while (tlen > 1 && target[tlen - 1] == '/') tlen--;
    while (rlen > 1 && routePath[rlen - 1] == '/') rlen--;
    if (rlen != tlen) return 0;
    return strncmp(routePath, target, rlen) == 0;
}

const OwlServiceRuntimeRoute* owl_service_runtime_find(const char* verb,
                                                       const char* path) {
    if (verb == NULL || path == NULL) return NULL;
    for (int i = 0; i < g_route_count; i++) {
        OwlServiceRuntimeRoute* r = &g_routes[i];
        if (r->hasParam) continue;   /* apparie par la resolution riche, pas ici */
        if (strcasecmp(r->verb, verb) != 0) continue;
        if (!path_matches(r->path, path)) continue;
        return r;
    }
    return NULL;
}

/* ================================================================
 * Resolution riche : parametree + specificite + 308 ASCII
 * ================================================================ */

/* Decoupe une cible (chemin brut, query et fragment tolerees) en segments.
 * Ecrit jusqu'a OWL_SVC_MAX_SEG segments dans `out` (chacun < 256). Retourne
 * le nombre de segments. Les slashs de tete/fin surnumeraires sont ignores. */
static int split_segments(const char* target, char out[][256]) {
    int count = 0;
    const char* p = target;
    while (*p == '/') p++;
    while (*p != '\0' && *p != '?' && *p != '#' && count < OWL_SVC_MAX_SEG) {
        const char* start = p;
        while (*p != '\0' && *p != '/' && *p != '?' && *p != '#') p++;
        size_t len = (size_t)(p - start);
        if (len >= 256) len = 255;
        memcpy(out[count], start, len);
        out[count][len] = '\0';
        count++;
        while (*p == '/') p++;
    }
    return count;
}

/* Egalite ASCII PURE, sensible a la casse. */
static int seg_eq_exact(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

/* Egalite insensible a la casse ASCII PURE : seuls A-Z <-> a-z sont replies,
 * jamais un octet >= 0x80 (garde-fou repli hors-ASCII). */
static int seg_eq_ascii_ci(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca != cb) {
            /* Ne replier que dans la plage ASCII stricte. */
            if (ca < 0x80 && cb < 0x80) {
                if (tolower(ca) != tolower(cb)) return 0;
            } else {
                return 0;
            }
        }
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* Teste si une route apparie les segments d'une cible en CASSE INSENSIBLE
 * ASCII (surensemble de l'appariement exact). Retourne 1 si oui, et pose :
 *  - *literalScore = nombre de segments litteraux (specificite, litteral >
 *    parametre) ;
 *  - *exact = 1 si TOUS les segments litteraux concordent aussi en casse
 *    sensible (appariement exact), 0 si un segment ne concorde qu'en casse
 *    insensible. */
/*  — vrai si le dernier segment de la route est un catch-all glouton. */
static int route_is_catchall(const OwlServiceRuntimeRoute* r) {
    return r->segCount > 0 && r->segIsParam[r->segCount - 1] == 2;
}

static int route_matches_segments(const OwlServiceRuntimeRoute* r,
                                  char segs[][256], int segCount,
                                  int* literalScore, int* exact) {
    if (route_is_catchall(r)) {
        /*  — le dernier segment est glouton : il capture 1+ segments
         * finaux. La cible doit avoir AU MOINS autant de segments que la route
         * (>= segCount) pour que le glouton capture au moins un segment ; un
         * segment de moins (le catch-all vide) NE matche PAS -> 404. Les
         * segments FIXES (avant le glouton) s'apparient normalement. */
        if (segCount < r->segCount) return 0;
        int fixed = r->segCount - 1;
        int lit = 0;
        int isExact = 1;
        for (int s = 0; s < fixed; s++) {
            if (r->segIsParam[s]) continue;   /* parametre nomme : joker */
            if (!seg_eq_ascii_ci(r->segments[s], segs[s])) return 0;
            if (!seg_eq_exact(r->segments[s], segs[s])) isExact = 0;
            lit++;
        }
        /* Specificite : seuls les segments litteraux du prefixe fixe comptent
         * (le glouton, comme tout parametre, ne contribue pas). Une route
         * fixe qui apparie la meme cible gagne donc toujours (score >= ou
         * longueur exacte contre glouton de score moindre). */
        *literalScore = lit;
        *exact = isExact;
        return 1;
    }
    if (r->segCount != segCount) return 0;
    int lit = 0;
    int isExact = 1;
    for (int s = 0; s < segCount; s++) {
        if (r->segIsParam[s]) continue;   /* parametre : joker */
        if (!seg_eq_ascii_ci(r->segments[s], segs[s])) return 0;
        if (!seg_eq_exact(r->segments[s], segs[s])) isExact = 0;
        lit++;
    }
    *literalScore = lit;
    *exact = isExact;
    return 1;
}

/* Reconstruit le chemin canonique d'une route pour une cible donnee : segments
 * litteraux tels qu'enregistres (casse canon), segments parametres remplaces
 * par la valeur fournie dans la cible. */
static void build_canonical(const OwlServiceRuntimeRoute* r, char segs[][256],
                            int segCount, char* out, size_t outLen) {
    size_t n = 0;
    int catchAll = route_is_catchall(r);
    for (int s = 0; s < r->segCount; s++) {
        if (catchAll && s == r->segCount - 1) {
            /*  — le glouton restitue TOUS les segments finaux fournis
             * (casse d'origine : donnee utilisateur, jamais repliee). */
            for (int k = s; k < segCount; k++) {
                int w = snprintf(out + n, outLen - n, "/%s", segs[k]);
                if (w < 0) break;
                n += (size_t)w;
                if (n >= outLen - 1) break;
            }
            break;
        }
        const char* piece = r->segIsParam[s] ? segs[s] : r->segments[s];
        int w = snprintf(out + n, outLen - n, "/%s", piece);
        if (w < 0) break;
        n += (size_t)w;
        if (n >= outLen - 1) break;
    }
    if (n == 0 && outLen > 1) { out[0] = '/'; out[1] = '\0'; }
}

void owl_service_runtime_resolve(const char* verb, const char* target,
                                 OwlServiceResolveResult* out) {
    memset(out, 0, sizeof(*out));
    out->kind = OWL_SVC_RESOLVE_NONE;
    if (verb == NULL || target == NULL) return;

    char segs[OWL_SVC_MAX_SEG][256];
    int segCount = split_segments(target, segs);

    /* Passe unique : parmi les routes qui apparient EN CASSE INSENSIBLE, la
     * plus specifique gagne (plus de segments litteraux). A specificite egale,
     * un appariement exact prime sur un appariement seulement insensible a la
     * casse. La route retenue est SERVIE si elle concordait en casse exacte,
     * sinon REDIRIGEE (308) vers sa forme canonique — un desaccord de casse ne
     * peut pas contourner une route litterale plus specifique en aval. */
    const OwlServiceRuntimeRoute* best = NULL;
    int bestScore = -1;
    int bestExact = 0;
    for (int i = 0; i < g_route_count; i++) {
        const OwlServiceRuntimeRoute* r = &g_routes[i];
        if (strcasecmp(r->verb, verb) != 0) continue;
        int score = 0, exact = 0;
        if (!route_matches_segments(r, segs, segCount, &score, &exact)) continue;
        if (score > bestScore || (score == bestScore && exact && !bestExact)) {
            best = r; bestScore = score; bestExact = exact;
        }
    }
    if (best == NULL) return;   /* aucune route -> 404 */

    if (bestExact) {
        out->kind  = OWL_SVC_RESOLVE_MATCH;
        out->route = best;
        int catchAll = route_is_catchall(best);
        for (int s = 0; s < best->segCount && s < OWL_SVC_MAX_SEG; s++) {
            if (!best->segIsParam[s]) continue;
            if (catchAll && s == best->segCount - 1) {
                /*  — le glouton capture la QUEUE jointe par '/' :
                 * segs[s..segCount-1]. Le binding serve-time la coupe a nouveau
                 * si le parametre est declare `tableau de chaine`. */
                size_t n = 0;
                for (int k = s; k < segCount; k++) {
                    int w = snprintf(out->pathValues[s] + n,
                                     sizeof(out->pathValues[s]) - n,
                                     "%s%s", (k > s ? "/" : ""), segs[k]);
                    if (w < 0) break;
                    n += (size_t)w;
                    if (n >= sizeof(out->pathValues[s]) - 1) break;
                }
            } else {
                snprintf(out->pathValues[s], sizeof(out->pathValues[s]),
                         "%s", segs[s]);
            }
        }
        return;
    }
    out->kind  = OWL_SVC_RESOLVE_REDIRECT;
    out->route = best;
    build_canonical(best, segs, segCount, out->canonical, sizeof(out->canonical));
}
