// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "service_decl.h"
#include "scanner.h"   /* scanner_strip_accents : cle canon ASCII */

#include <stdlib.h>
#include <string.h>

/* Registre process-global de la compilation courante. Reset par
 * owl_service_registry_reset() (compiler_reset_poo_state). */
static OwlServiceDecl** g_services = NULL;
static int g_service_count = 0;
static int g_service_capacity = 0;

static char* dup_cstr(const char* s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    memcpy(out, s, n + 1);
    return out;
}

/* Cle canon ASCII (accents strippes, minuscule) dans `out`. */
static void canon_key(const char* name, char* out, int cap) {
    char stripped[256];
    int n = scanner_strip_accents(name, (int)strlen(name),
                                  stripped, sizeof(stripped));
    if (n <= 0) {
        n = (int)strlen(name);
        if (n > (int)sizeof(stripped) - 1) n = (int)sizeof(stripped) - 1;
        memcpy(stripped, name, (size_t)n);
    }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) {
        char c = stripped[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = '\0';
}

OwlServiceDecl* owl_service_decl_new(const char* name, int line) {
    OwlServiceDecl* svc = (OwlServiceDecl*)calloc(1, sizeof(OwlServiceDecl));
    svc->name = dup_cstr(name);
    svc->line = line;
    return svc;
}

OwlOperationDecl* owl_service_decl_add_operation(OwlServiceDecl* svc,
                                                 const char* name, int line) {
    if (svc->operationCount >= svc->operationCapacity) {
        int cap = svc->operationCapacity == 0 ? 4 : svc->operationCapacity * 2;
        svc->operations = (OwlOperationDecl**)realloc(
            svc->operations, (size_t)cap * sizeof(OwlOperationDecl*));
        svc->operationCapacity = cap;
    }
    OwlOperationDecl* op = (OwlOperationDecl*)calloc(1, sizeof(OwlOperationDecl));
    op->name = dup_cstr(name);
    op->line = line;
    svc->operations[svc->operationCount++] = op;
    return op;
}

static void free_operation(OwlOperationDecl* op) {
    if (op == NULL) return;
    free(op->name);
    free(op->method);
    free(op->codeEtat);
    free(op->flux);
    for (int i = 0; i < op->pathSegmentCount; i++) free(op->pathSegments[i]);
    free(op->pathSegments);
    free(op->pathSegmentIsParam);
    for (int i = 0; i < op->responseCount; i++) free(op->responses[i]);
    free(op->responses);
    for (int i = 0; i < op->queryParamCount; i++) free(op->queryParams[i]);
    free(op->queryParams);
    for (int i = 0; i < op->procParamCount; i++) {
        free(op->procParams[i].name);
        free(op->procParams[i].typeName);
    }
    free(op->procParams);
    free(op->procName);
    free(op->procReturnType);
    free(op->handlerGlobal);
    free(op);
}

static void free_service(OwlServiceDecl* svc) {
    if (svc == NULL) return;
    free(svc->name);
    for (int i = 0; i < svc->urlBaseCount; i++) free(svc->urlBase[i]);
    free(svc->urlBase);
    free(svc->authentication);
    free(svc->authHandlerGlobal);
    free(svc->errorHandlerGlobal);
    free(svc->authzHandlerGlobal);
    free(svc->jwtPublicKeyPem);
    free(svc->jwtIssuer);
    free(svc->jwtJwksUrl);
    free(svc->jwtAudience);
    for (int i = 0; i < svc->operationCount; i++) free_operation(svc->operations[i]);
    free(svc->operations);
    free(svc);
}

void owl_service_registry_reset(void) {
    for (int i = 0; i < g_service_count; i++) free_service(g_services[i]);
    free(g_services);
    g_services = NULL;
    g_service_count = 0;
    g_service_capacity = 0;
}

int owl_service_registry_count(void) { return g_service_count; }

OwlServiceDecl* owl_service_registry_at(int index) {
    if (index < 0 || index >= g_service_count) return NULL;
    return g_services[index];
}

OwlServiceDecl* owl_service_registry_find_ascii(const char* asciiName) {
    if (asciiName == NULL) return NULL;
    char want[256];
    canon_key(asciiName, want, sizeof(want));
    for (int i = 0; i < g_service_count; i++) {
        char have[256];
        canon_key(g_services[i]->name, have, sizeof(have));
        if (strcmp(have, want) == 0) return g_services[i];
    }
    return NULL;
}

OwlServiceDecl* owl_service_registry_add(const char* name, int line) {
    if (owl_service_registry_find_ascii(name) != NULL) return NULL; /* doublon */
    if (g_service_count >= g_service_capacity) {
        int cap = g_service_capacity == 0 ? 8 : g_service_capacity * 2;
        g_services = (OwlServiceDecl**)realloc(
            g_services, (size_t)cap * sizeof(OwlServiceDecl*));
        g_service_capacity = cap;
    }
    OwlServiceDecl* svc = owl_service_decl_new(name, line);
    g_services[g_service_count++] = svc;
    return svc;
}
