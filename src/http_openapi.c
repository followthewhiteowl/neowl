// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* http_openapi.c — voir http_openapi.h.
 *
 * Le document est bati mecaniquement depuis l'OwlServiceDecl :
 *   - chemins  = UrlBase + Chemin (segments litteraux, params -> "{id}")
 *   - methode  = Methode (httpGet -> "get", ...)
 *   - params   = Chemin (path, required) + Requete (query, required derive de
 *                la presence d'une valeur par defaut sur le param de surRequete)
 *   - body     = corps structure (application/json) ou upload (multipart/form-data)
 *   - reponses = Reponses -> code->reponse, Problem Details pour les erreurs,
 *                500 implicite toujours present
 *   - securite = Authentification -> securityScheme ; httpAuthAucune -> security:[]
 *
 * Format : indentation 2 espaces, PAS d'espace apres le deux-points (lisible +
 * `"openapi":"3.x"` compact attendu cote route serveur). */

#include "http_openapi.h"

#include "owl_registry.h"
#include "scanner.h"
#include "value.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---------- tampon de chaine growable ---------- */

typedef struct { char* data; size_t len, cap; } SB;

static int sb_init(SB* b) {
    b->cap = 8192; b->len = 0;
    b->data = (char*)malloc(b->cap);
    if (!b->data) return 0;
    b->data[0] = '\0';
    return 1;
}
static int sb_ensure(SB* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t nc = b->cap;
    while (b->len + extra + 1 > nc) nc *= 2;
    char* nd = (char*)realloc(b->data, nc);
    if (!nd) return 0;
    b->data = nd; b->cap = nc;
    return 1;
}
static void sb_putc(SB* b, char c) {
    if (!sb_ensure(b, 1)) return;
    b->data[b->len++] = c; b->data[b->len] = '\0';
}
static void sb_puts(SB* b, const char* s) {
    size_t n = strlen(s);
    if (!sb_ensure(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n; b->data[b->len] = '\0';
}
static void sb_indent(SB* b, int depth) {
    sb_putc(b, '\n');
    for (int i = 0; i < depth; i++) { sb_putc(b, ' '); sb_putc(b, ' '); }
}
/* Ecrit une chaine JSON echappee (entre guillemets). */
static void sb_jstr(SB* b, const char* s) {
    sb_putc(b, '"');
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') { sb_putc(b, '\\'); sb_putc(b, (char)c); }
        else if (c == '\n') sb_puts(b, "\\n");
        else if (c == '\r') sb_puts(b, "\\r");
        else if (c == '\t') sb_puts(b, "\\t");
        else if (c < 0x20)  { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); sb_puts(b, t); }
        else sb_putc(b, (char)c);
    }
    sb_putc(b, '"');
}
/* "key":  (sans espace apres le deux-points). */
static void sb_key(SB* b, const char* k) { sb_jstr(b, k); sb_putc(b, ':'); }

/* ---------- helpers de mapping AST -> OpenAPI ---------- */

/* Forme ASCII minuscule d'un nom (strip accents + tolower). */
static void ascii_lower(const char* s, char* out, int cap) {
    scanner_strip_accents(s ? s : "", (int)(s ? strlen(s) : 0), out, cap);
    for (char* p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

/* Verbe HTTP OpenAPI depuis une valeur httpMethodes ("httpGet" -> "get"). */
static void emit_method_verb(SB* b, const char* method) {
    char a[64];
    ascii_lower(method ? method : "get", a, sizeof a);
    const char* v = a;
    if (strncmp(a, "http", 4) == 0) v = a + 4; /* saute le prefixe "http" */
    if (!*v) v = "get";
    sb_puts(b, v);
}

/* Code numerique d'une valeur httpCodeEtat via le registre de constantes
 * (case+accent insensible). 0 si inconnue. */
static int http_code_num(const char* name) {
    const OwlConstDef* c = owl_registry_const_find(name);
    if (c && IS_INTEGER(c->value)) return (int)AS_INTEGER(c->value);
    return 0;
}
/* Description standard d'une valeur httpCodeEtat (descEN "200 OK", ...). */
static const char* http_code_desc(const char* name, char* fallback, int cap) {
    const OwlConstDef* c = owl_registry_const_find(name);
    if (c && c->descEN && c->descEN[0]) return c->descEN;
    snprintf(fallback, cap, "%s", name ? name : "");
    return fallback;
}

/* Ecrit le schema OpenAPI d'un type OWL scalaire / upload, inline :
 * {"type":"integer"} etc. `typeName` peut etre accentue ou ASCII. */
static void emit_type_schema(SB* b, const char* typeName, int isFile) {
    char a[64];
    ascii_lower(typeName ? typeName : "", a, sizeof a);
    if (isFile || strcmp(a, "fichierteleverse") == 0 || strcmp(a, "uploadedfile") == 0) {
        sb_puts(b, "{\"type\":\"string\",\"format\":\"binary\"}");
        return;
    }
    const char* t;
    if (strcmp(a, "entier") == 0 || strcmp(a, "integer") == 0)       t = "integer";
    else if (strcmp(a, "reel") == 0 || strcmp(a, "real") == 0 ||
             strcmp(a, "numeric") == 0)                              t = "number";
    else if (strcmp(a, "booleen") == 0 || strcmp(a, "boolean") == 0 ||
             strcmp(a, "bool") == 0)                                 t = "boolean";
    else if (strcmp(a, "chaine") == 0 || strcmp(a, "string") == 0)   t = "string";
    else                                                            t = "string";
    sb_puts(b, "{\"type\":\""); sb_puts(b, t); sb_puts(b, "\"}");
}

/* Cherche le parametre de surRequete portant `name` (accent+casse insensible). */
static const OwlServiceProcParam* find_proc_param(const OwlOperationDecl* op,
                                                  const char* name) {
    if (!op || !name) return NULL;
    char qa[128]; ascii_lower(name, qa, sizeof qa);
    for (int i = 0; i < op->procParamCount; i++) {
        const OwlServiceProcParam* p = &op->procParams[i];
        if (!p->name) continue;
        char pa[128]; ascii_lower(p->name, pa, sizeof pa);
        if (strcmp(qa, pa) == 0) return p;
    }
    return NULL;
}

/* Vrai si `name` figure dans les params de query declares (Requete). */
static int is_query_param(const OwlOperationDecl* op, const char* name) {
    char qa[128]; ascii_lower(name, qa, sizeof qa);
    for (int i = 0; i < op->queryParamCount; i++) {
        if (!op->queryParams[i]) continue;
        char pa[128]; ascii_lower(op->queryParams[i], pa, sizeof pa);
        if (strcmp(qa, pa) == 0) return 1;
    }
    return 0;
}

/* Construit la chaine de chemin "/api/clients/{id}" dans out. */
static void build_path(const OwlServiceDecl* svc, const OwlOperationDecl* op,
                       char* out, int cap) {
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < svc->urlBaseCount && svc->urlBase; i++) {
        n += snprintf(out + n, (n < cap) ? cap - n : 0, "/%s",
                      svc->urlBase[i] ? svc->urlBase[i] : "");
    }
    for (int i = 0; i < op->pathSegmentCount; i++) {
        const char* seg = op->pathSegments[i] ? op->pathSegments[i] : "";
        int isParam = op->pathSegmentIsParam ? op->pathSegmentIsParam[i] : 0;
        if (isParam)
            n += snprintf(out + n, (n < cap) ? cap - n : 0, "/{%s}", seg);
        else
            n += snprintf(out + n, (n < cap) ? cap - n : 0, "/%s", seg);
    }
    if (out[0] == '\0') { out[0] = '/'; out[1] = '\0'; }
}

/* ---------- securityScheme derive de Authentification ---------- */

typedef struct {
    int         present;      /* 1 = schema protecteur derive */
    const char* name;         /* cle du securityScheme (ex "bearerAuth") */
} AuthInfo;

static AuthInfo auth_info(const OwlServiceDecl* svc) {
    AuthInfo a; a.present = 0; a.name = NULL;
    if (!svc->authentication) return a;
    char v[64]; ascii_lower(svc->authentication, v, sizeof v);
    if      (strcmp(v, "httpauthbearer") == 0) { a.present = 1; a.name = "bearerAuth"; }
    else if (strcmp(v, "httpauthjwt") == 0)    { a.present = 1; a.name = "bearerAuth"; }
    else if (strcmp(v, "httpauthbasique") == 0){ a.present = 1; a.name = "basicAuth"; }
    else if (strcmp(v, "httpauthcleapi") == 0) { a.present = 1; a.name = "apiKeyAuth"; }
    /* httpauthaucune / inconnu -> pas de schema */
    return a;
}

/* Ecrit l'objet securityScheme (valeur) pour le schema du service. */
static void emit_security_scheme(SB* b, const OwlServiceDecl* svc, int depth) {
    char v[64]; ascii_lower(svc->authentication ? svc->authentication : "", v, sizeof v);
    sb_putc(b, '{');
    if (strcmp(v, "httpauthcleapi") == 0) {
        sb_indent(b, depth + 1); sb_key(b, "type");   sb_jstr(b, "apiKey");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "in");     sb_jstr(b, "header");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "name");   sb_jstr(b, "X-API-Key");
    } else if (strcmp(v, "httpauthbasique") == 0) {
        sb_indent(b, depth + 1); sb_key(b, "type");   sb_jstr(b, "http");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "scheme"); sb_jstr(b, "basic");
    } else if (strcmp(v, "httpauthjwt") == 0) {
        sb_indent(b, depth + 1); sb_key(b, "type");   sb_jstr(b, "http");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "scheme"); sb_jstr(b, "bearer");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "bearerFormat"); sb_jstr(b, "JWT");
    } else { /* bearer */
        sb_indent(b, depth + 1); sb_key(b, "type");   sb_jstr(b, "http");
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "scheme"); sb_jstr(b, "bearer");
    }
    sb_indent(b, depth); sb_putc(b, '}');
}

/* ---------- reponses ---------- */

/* Content-type de la reponse SUCCES selon le mode Flux (, valeur
 * canon de httpFlux). NULL = pas de flux -> reponse bufferisee application/json
 * classique. Le serveur sert reellement ce media-type (fluxSSE = SSE,
 * fluxOctets = octets bruts) : le document OpenAPI doit le refleter, sinon le
 * contrat ment. Ne concerne que le succes ; les erreurs restent en
 * application/problem+json. */
static const char* flux_success_media(const char* flux) {
    if (!flux) return NULL;
    char v[64]; ascii_lower(flux, v, sizeof v);
    if (strcmp(v, "fluxsse") == 0)    return "text/event-stream";
    if (strcmp(v, "fluxoctets") == 0) return "application/octet-stream";
    return NULL;
}

/* Ecrit une reponse (valeur) : Problem Details pour les erreurs (>=400),
 * sinon fluxMedia (content-type de flux) si fourni, application/json a defaut.
 * depth = niveau de l'objet reponse. fluxMedia est ignore pour les erreurs. */
static void emit_response(SB* b, const char* codeName, int code, int depth,
                          const char* fluxMedia) {
    char fb[64];
    const char* desc = http_code_desc(codeName, fb, sizeof fb);
    int isErr = (code >= 400);
    const char* media = isErr ? "application/problem+json"
                              : (fluxMedia ? fluxMedia : "application/json");
    /* Schema : object pour les erreurs, binaire pour un flux d'octets,
     * string sinon (SSE = flux de texte, corps bufferise = string). */
    const char* schema;
    if (isErr)
        schema = "{\"type\":\"object\"}";
    else if (fluxMedia && strcmp(fluxMedia, "application/octet-stream") == 0)
        schema = "{\"type\":\"string\",\"format\":\"binary\"}";
    else
        schema = "{\"type\":\"string\"}";
    sb_putc(b, '{');
    sb_indent(b, depth + 1); sb_key(b, "description"); sb_jstr(b, desc);
    sb_putc(b, ',');
    sb_indent(b, depth + 1); sb_key(b, "content"); sb_putc(b, '{');
    sb_indent(b, depth + 2); sb_key(b, media); sb_putc(b, '{');
    sb_indent(b, depth + 3); sb_key(b, "schema");
    sb_puts(b, schema);
    sb_indent(b, depth + 2); sb_putc(b, '}');
    sb_indent(b, depth + 1); sb_putc(b, '}');
    sb_indent(b, depth); sb_putc(b, '}');
}

/* ---------- une operation (valeur du verbe) ---------- */

static void emit_operation(SB* b, const OwlServiceDecl* svc,
                           const OwlOperationDecl* op, int depth) {
    (void)svc;
    sb_putc(b, '{');
    int need_comma = 0;

    /* operationId */
    sb_indent(b, depth + 1); sb_key(b, "operationId");
    sb_jstr(b, op->name ? op->name : "");
    need_comma = 1;

    /* --- parameters : path (required) + query (required derive du defaut) --- */
    int pathParamCount = 0;
    for (int i = 0; i < op->pathSegmentCount; i++)
        if (op->pathSegmentIsParam && op->pathSegmentIsParam[i]) pathParamCount++;
    int hasParams = (pathParamCount > 0) || (op->queryParamCount > 0);

    if (hasParams) {
        if (need_comma) sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "parameters"); sb_putc(b, '[');
        int first = 1;
        /* path params, dans l'ordre des segments */
        for (int i = 0; i < op->pathSegmentCount; i++) {
            int segFlag = op->pathSegmentIsParam ? op->pathSegmentIsParam[i] : 0;
            if (!segFlag) continue;
            const char* nm = op->pathSegments[i] ? op->pathSegments[i] : "";
            const OwlServiceProcParam* pp = find_proc_param(op, nm);
            if (!first) sb_putc(b, ',');
            first = 0;
            sb_indent(b, depth + 2); sb_putc(b, '{');
            sb_indent(b, depth + 3); sb_key(b, "name");     sb_jstr(b, nm);
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "in");       sb_jstr(b, "path");
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "required"); sb_puts(b, "true");
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "schema");
            emit_type_schema(b, pp ? pp->typeName : NULL, 0);
            /* segFlag == 2 : catch-all glouton (cf. service_decl.h, ).
             * OpenAPI 3 n'a pas de catch-all natif : on garde le param {nom}
             * mais on signale sa nature variable-longueur pour ne pas laisser
             * croire a un segment unique — description humaine + extension
             * vendor machine-lisible. Le param ordinaire (segFlag == 1) reste
             * strictement inchange. */
            if (segFlag == 2) {
                sb_putc(b, ',');
                sb_indent(b, depth + 3); sb_key(b, "description");
                sb_jstr(b, "Greedy path segment: captures the remaining path "
                           "(one or more trailing segments, joined by /).");
                sb_putc(b, ',');
                sb_indent(b, depth + 3); sb_key(b, "x-owl-catch-all");
                sb_puts(b, "true");
            }
            sb_indent(b, depth + 2); sb_putc(b, '}');
        }
        /* query params */
        for (int i = 0; i < op->queryParamCount; i++) {
            const char* nm = op->queryParams[i] ? op->queryParams[i] : "";
            const OwlServiceProcParam* pp = find_proc_param(op, nm);
            int required = pp ? (pp->hasDefault ? 0 : 1) : 1;
            if (!first) sb_putc(b, ',');
            first = 0;
            sb_indent(b, depth + 2); sb_putc(b, '{');
            sb_indent(b, depth + 3); sb_key(b, "name");     sb_jstr(b, nm);
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "in");       sb_jstr(b, "query");
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "required"); sb_puts(b, required ? "true" : "false");
            sb_putc(b, ',');
            sb_indent(b, depth + 3); sb_key(b, "schema");
            emit_type_schema(b, pp ? pp->typeName : NULL, 0);
            sb_indent(b, depth + 2); sb_putc(b, '}');
        }
        sb_indent(b, depth + 1); sb_putc(b, ']');
    }

    /* --- requestBody : multipart (upload) ou corps structure (json) --- */
    if (op->contentTypeMultipart) {
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "requestBody"); sb_putc(b, '{');
        sb_indent(b, depth + 2); sb_key(b, "required"); sb_puts(b, "true");
        sb_putc(b, ',');
        sb_indent(b, depth + 2); sb_key(b, "content"); sb_putc(b, '{');
        sb_indent(b, depth + 3); sb_key(b, "multipart/form-data"); sb_putc(b, '{');
        sb_indent(b, depth + 4); sb_key(b, "schema"); sb_putc(b, '{');
        sb_indent(b, depth + 5); sb_key(b, "type"); sb_jstr(b, "object");
        sb_putc(b, ',');
        sb_indent(b, depth + 5); sb_key(b, "properties"); sb_putc(b, '{');
        int first = 1;
        for (int i = 0; i < op->procParamCount; i++) {
            const OwlServiceProcParam* p = &op->procParams[i];
            if (!p->name) continue;
            /* champs de formulaire = tout param qui n'est ni path ni query */
            if (is_query_param(op, p->name)) continue;
            int isPath = 0;
            char qa[128]; ascii_lower(p->name, qa, sizeof qa);
            for (int s = 0; s < op->pathSegmentCount; s++) {
                if (!(op->pathSegmentIsParam && op->pathSegmentIsParam[s])) continue;
                char pa[128]; ascii_lower(op->pathSegments[s] ? op->pathSegments[s] : "", pa, sizeof pa);
                if (strcmp(qa, pa) == 0) { isPath = 1; break; }
            }
            if (isPath) continue;
            if (!first) sb_putc(b, ',');
            first = 0;
            sb_indent(b, depth + 6); sb_key(b, p->name);
            emit_type_schema(b, p->typeName, p->isFileUpload);
        }
        sb_indent(b, depth + 5); sb_putc(b, '}');   /* properties */
        sb_indent(b, depth + 4); sb_putc(b, '}');   /* schema */
        sb_indent(b, depth + 3); sb_putc(b, '}');   /* media */
        sb_indent(b, depth + 2); sb_putc(b, '}');   /* content */
        sb_indent(b, depth + 1); sb_putc(b, '}');   /* requestBody */
    } else {
        /* corps structure : premier param scalaire structure, ni path ni query */
        const OwlServiceProcParam* body = NULL;
        for (int i = 0; i < op->procParamCount; i++) {
            const OwlServiceProcParam* p = &op->procParams[i];
            if (!p->name || !p->isStructured) continue;
            if (is_query_param(op, p->name)) continue;
            body = p; break;
        }
        if (body) {
            sb_putc(b, ',');
            sb_indent(b, depth + 1); sb_key(b, "requestBody"); sb_putc(b, '{');
            sb_indent(b, depth + 2); sb_key(b, "required"); sb_puts(b, "true");
            sb_putc(b, ',');
            sb_indent(b, depth + 2); sb_key(b, "content"); sb_putc(b, '{');
            sb_indent(b, depth + 3); sb_key(b, "application/json"); sb_putc(b, '{');
            sb_indent(b, depth + 4); sb_key(b, "schema"); sb_puts(b, "{\"type\":\"object\"}");
            sb_indent(b, depth + 3); sb_putc(b, '}');
            sb_indent(b, depth + 2); sb_putc(b, '}');
            sb_indent(b, depth + 1); sb_putc(b, '}');
        }
    }

    /* --- security : opt-out par operation (httpAuthAucune) --- */
    if (op->authOptOut) {
        sb_putc(b, ',');
        sb_indent(b, depth + 1); sb_key(b, "security"); sb_puts(b, "[]");
    }

    /* --- responses : Reponses declarees + 500 implicite --- */
    /*  — content-type de la reponse succes selon le mode Flux de
     * l'operation (fluxSSE -> text/event-stream, fluxOctets ->
     * application/octet-stream). NULL = reponse bufferisee json classique. */
    const char* fluxMedia = flux_success_media(op->flux);
    sb_putc(b, ',');
    sb_indent(b, depth + 1); sb_key(b, "responses"); sb_putc(b, '{');
    int seen[64]; int seenCount = 0; int has500 = 0;
    int firstR = 1;
    for (int i = 0; i < op->responseCount; i++) {
        const char* rn = op->responses[i];
        if (!rn) continue;
        int code = http_code_num(rn);
        int dup = 0;
        for (int k = 0; k < seenCount; k++) if (seen[k] == code) { dup = 1; break; }
        if (dup) continue;
        if (seenCount < 64) seen[seenCount++] = code;
        if (code == 500) has500 = 1;
        char cs[16]; snprintf(cs, sizeof cs, "%d", code);
        if (!firstR) sb_putc(b, ',');
        firstR = 0;
        sb_indent(b, depth + 2); sb_key(b, cs);
        emit_response(b, rn, code, depth + 2, fluxMedia);
    }
    if (!has500) {
        if (!firstR) sb_putc(b, ',');
        firstR = 0;
        sb_indent(b, depth + 2); sb_key(b, "500");
        emit_response(b, "httpErreurServeur", 500, depth + 2, fluxMedia);
    }
    sb_indent(b, depth + 1); sb_putc(b, '}');   /* responses */

    sb_indent(b, depth); sb_putc(b, '}');       /* operation */
}

/* ---------- paths (regroupement par chemin) ---------- */

char* owl_openapi_generate(const OwlServiceDecl* svc) {
    if (!svc) return NULL;
    SB b;
    if (!sb_init(&b)) return NULL;

    AuthInfo auth = auth_info(svc);

    sb_putc(&b, '{');

    /* openapi */
    sb_indent(&b, 1); sb_key(&b, "openapi"); sb_jstr(&b, "3.0.3");
    sb_putc(&b, ',');

    /* info */
    sb_indent(&b, 1); sb_key(&b, "info"); sb_putc(&b, '{');
    sb_indent(&b, 2); sb_key(&b, "title");   sb_jstr(&b, svc->name ? svc->name : "Service");
    sb_putc(&b, ',');
    sb_indent(&b, 2); sb_key(&b, "version"); sb_jstr(&b, "1.0.0");
    sb_indent(&b, 1); sb_putc(&b, '}');
    sb_putc(&b, ',');

    /* paths — regroupees par chemin, chaque chemin porte ses methodes */
    sb_indent(&b, 1); sb_key(&b, "paths"); sb_putc(&b, '{');
    /* On marque les operations deja emises pour eviter de re-ouvrir un chemin. */
    int emitted[512];
    int nOps = svc->operationCount;
    for (int i = 0; i < nOps && i < 512; i++) emitted[i] = 0;
    int firstPath = 1;
    for (int i = 0; i < nOps; i++) {
        if (i < 512 && emitted[i]) continue;
        const OwlOperationDecl* opi = svc->operations[i];
        if (!opi) continue;
        /*  — op servie mais hors document : n'ouvre aucun chemin pour
         * elle. Un chemin partage par une op documentee sera ouvert par cette
         * derniere (l'op exclue est alors ignoree dans la boucle interne). */
        if (opi->excludeOpenAPI) continue;
        char path[512];
        build_path(svc, opi, path, sizeof path);

        if (!firstPath) sb_putc(&b, ',');
        firstPath = 0;
        sb_indent(&b, 2); sb_key(&b, path); sb_putc(&b, '{');

        /* toutes les operations partageant ce chemin */
        int firstMethod = 1;
        for (int j = i; j < nOps; j++) {
            if (j < 512 && emitted[j]) continue;
            const OwlOperationDecl* opj = svc->operations[j];
            if (!opj) continue;
            char pj[512];
            build_path(svc, opj, pj, sizeof pj);
            if (strcmp(pj, path) != 0) continue;
            /*  — op exclue partageant ce chemin : marquee traitee mais
             * jamais emise (sa methode n'apparait pas dans le document). */
            if (opj->excludeOpenAPI) { if (j < 512) emitted[j] = 1; continue; }
            if (j < 512) emitted[j] = 1;
            if (!firstMethod) sb_putc(&b, ',');
            firstMethod = 0;
            sb_indent(&b, 3);
            sb_putc(&b, '"');
            emit_method_verb(&b, opj->method);
            sb_putc(&b, '"'); sb_putc(&b, ':');
            emit_operation(&b, svc, opj, 3);
        }
        sb_indent(&b, 2); sb_putc(&b, '}');   /* pathItem */
    }
    sb_indent(&b, 1); sb_putc(&b, '}');       /* paths */
    sb_putc(&b, ',');

    /* components */
    sb_indent(&b, 1); sb_key(&b, "components"); sb_putc(&b, '{');
    if (auth.present) {
        sb_indent(&b, 2); sb_key(&b, "securitySchemes"); sb_putc(&b, '{');
        sb_indent(&b, 3); sb_key(&b, auth.name);
        emit_security_scheme(&b, svc, 3);
        sb_indent(&b, 2); sb_putc(&b, '}');
        sb_indent(&b, 1); sb_putc(&b, '}');
    } else {
        sb_putc(&b, '}');
    }

    /* security globale (si schema protecteur) */
    if (auth.present) {
        sb_putc(&b, ',');
        sb_indent(&b, 1); sb_key(&b, "security"); sb_putc(&b, '[');
        sb_indent(&b, 2); sb_putc(&b, '{');
        sb_indent(&b, 3); sb_key(&b, auth.name); sb_puts(&b, "[]");
        sb_indent(&b, 2); sb_putc(&b, '}');
        sb_indent(&b, 1); sb_putc(&b, ']');
    }

    sb_putc(&b, '\n'); sb_putc(&b, '}'); sb_putc(&b, '\n');
    return b.data;
}
