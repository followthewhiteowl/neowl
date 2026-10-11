/* Benchmark : mesure le temps moyen d'un handle_completion sur un
 * fichier .owl reel. Fixture par defaut (workspace bundle, relatif au
 * repo) : tests/lsp/fixtures/bigoutput/ — workspace riche (40 classes)
 * concu pour produire une grosse completion. Surchargeable :
 *   - argv[1]            : fichier .owl a profiler (mode principal)
 *   - OWL_BENCH_ROOT env : racine de workspace pour --scan-time
 * Aucun chemin machine absolu : l'outil reste portable d'un poste a l'autre. */
#include "lsp_server.h"
#include "lsp_json.h"
#include "owl_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);
    return buf;
}

static void debug_assign_couleur(void) {
    /* Reproduit test_couleur_completion_in_assignment et dump les kinds. */
    for (int i = 0; i < lspServer.documentCount; i++) {
        if (lspServer.documents[i].content) free(lspServer.documents[i].content);
    }
    const char* src = "c est une Couleur = \n";
    lspServer.documentCount = 1;
    snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri), "file:///x.owl");
    lspServer.documents[0].content = strdup(src);
    lspServer.documents[0].contentLen = (int)strlen(src);
    lspServer.documents[0].version = 99;

    JsonValue* params = json_new_object();
    JsonValue* td = json_new_object();
    json_object_set(td, "uri", json_new_string("file:///x.owl"));
    json_object_set(params, "textDocument", td);
    JsonValue* pos = json_new_object();
    json_object_set(pos, "line", json_new_int(0));
    json_object_set(pos, "character", json_new_int(20));
    json_object_set(params, "position", pos);
    JsonValue* r = handle_completion(params);

    JsonValue* arr = r;
    if (r && r->type == JSON_OBJECT) arr = json_get(r, "items");
    int n = (arr && arr->type == JSON_ARRAY) ? arr->as.arrayVal.count : -1;
    printf("\n[debug] c est une Couleur = (%d items, top struct=%s)\n",
           n, r && r->type == JSON_OBJECT ? "CompletionList" : "Array");
    int kindCounts[30] = {0};
    bool sawRouge = false;
    for (int i = 0; i < n; i++) {
        JsonValue* it = arr->as.arrayVal.items[i];
        int k = (int)json_get_int(it, "kind", 0);
        if (k >= 0 && k < 30) kindCounts[k]++;
        const char* lbl = json_get_string(it, "label");
        if (lbl && strcmp(lbl, "RougeClair") == 0) sawRouge = true;
        if (i < 5) printf("  [%d] kind=%d label=%s\n", i, k, lbl);
    }
    printf("  RougeClair seen: %s\n", sawRouge ? "YES" : "NO");
    for (int k = 0; k < 30; k++) {
        if (kindCounts[k]) printf("  kind=%d : %d\n", k, kindCounts[k]);
    }
    json_free(r); json_free(params);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--scan-time") == 0) {
        /* Force scan_user_symbols() with rootPath = projet OWL, mesure
         * combien de temps il met (filesystem walk inclus). */
        memset(&lspServer, 0, sizeof(lspServer));
        const char* root = getenv("OWL_BENCH_ROOT");
        if (!root || !*root) root = "tests/lsp/fixtures/bigoutput";
        snprintf(lspServer.rootPath, sizeof(lspServer.rootPath), "%s", root);
        char path2buf[1024];
        snprintf(path2buf, sizeof(path2buf), "%s/classe_01.owl", root);
        const char* path2 = path2buf;
        char* src2 = read_file(path2);
        lspServer.documentCount = 1;
        snprintf(lspServer.documents[0].uri, sizeof(lspServer.documents[0].uri),
                 "file:///%s", path2);
        lspServer.documents[0].content = src2;
        lspServer.documents[0].contentLen = (int)strlen(src2);
        lspServer.documents[0].version = 1;
        double t0 = now_ms();
        /* Trigger publish_diagnostics → scan_user_symbols force scan */
        publish_diagnostics(lspServer.documents[0].uri);
        double dt = now_ms() - t0;
        printf("publish_diagnostics + scan : %.2f ms (rootPath=%s)\n",
               dt, lspServer.rootPath);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--debug-couleur") == 0) {
        memset(&lspServer, 0, sizeof(lspServer));
        debug_assign_couleur();
        return 0;
    }
    const char* path = argc > 1 ? argv[1]
        : "tests/lsp/fixtures/bigoutput/classe_01.owl";
    char* src = read_file(path);
    if (!src) return 1;
    int len = (int)strlen(src);

    /* Setup LSP doc(s) — duplique le doc 20 fois pour simuler un
     * workspace charge (chaque doc compte dans scan_user_symbols). */
    memset(&lspServer, 0, sizeof(lspServer));
    int NDOC = (argc > 2) ? atoi(argv[2]) : 1;
    if (NDOC < 1) NDOC = 1;
    if (NDOC > 50) NDOC = 50;
    lspServer.documentCount = NDOC;
    for (int i = 0; i < NDOC; i++) {
        snprintf(lspServer.documents[i].uri, sizeof(lspServer.documents[i].uri),
                 "file:///doc%d_%s", i, path);
        lspServer.documents[i].content = i == 0 ? src : strdup(src);
        lspServer.documents[i].contentLen = len;
        lspServer.documents[i].version = 1;
    }
    printf("Workspace : %d document(s)\n", NDOC);

    /* Warmup : initialise les caches */
    {
        JsonValue* params = json_new_object();
        JsonValue* td = json_new_object();
        json_object_set(td, "uri", json_new_string(lspServer.documents[0].uri));
        json_object_set(params, "textDocument", td);
        JsonValue* pos = json_new_object();
        json_object_set(pos, "line", json_new_int(15));
        json_object_set(pos, "character", json_new_int(10));
        json_object_set(params, "position", pos);
        JsonValue* r = handle_completion(params);
        json_free(r); json_free(params);
    }

    int N = 100;
    double t0 = now_ms();
    int totalItems = 0;
    int maxItems = 0;
    int callsHugeItems = 0;
    for (int i = 0; i < N; i++) {
        int line = 5 + (i % 60);
        int ch   = 4 + (i % 20);
        JsonValue* params = json_new_object();
        JsonValue* td = json_new_object();
        json_object_set(td, "uri", json_new_string(lspServer.documents[0].uri));
        json_object_set(params, "textDocument", td);
        JsonValue* pos = json_new_object();
        json_object_set(pos, "line", json_new_int(line));
        json_object_set(pos, "character", json_new_int(ch));
        json_object_set(params, "position", pos);
        JsonValue* r = handle_completion(params);
        if (r && r->type == JSON_ARRAY) {
            int n = r->as.arrayVal.count;
            totalItems += n;
            if (n > maxItems) maxItems = n;
            if (n > 200) callsHugeItems++;
        }
        json_free(r); json_free(params);
    }
    double dt = now_ms() - t0;
    printf("File: %s (%d bytes)\n", path, len);
    printf("handle_completion x %d : total %.2f ms, avg %.3f ms/call\n",
           N, dt, dt / N);
    printf("Items: total %d, max %d/call, calls>200 items: %d\n",
           totalItems, maxItems, callsHugeItems);

    /* Mesure cas "rien tape" (line vide hors comment) */
    {
        JsonValue* params = json_new_object();
        JsonValue* td = json_new_object();
        json_object_set(td, "uri", json_new_string(lspServer.documents[0].uri));
        json_object_set(params, "textDocument", td);
        JsonValue* pos = json_new_object();
        json_object_set(pos, "line", json_new_int(15));   /* dans cadre1 brace */
        json_object_set(pos, "character", json_new_int(0));
        json_object_set(params, "position", pos);
        double t1 = now_ms();
        JsonValue* r = handle_completion(params);
        double dt1 = now_ms() - t1;
        JsonValue* arr = r;
        if (r && r->type == JSON_OBJECT) arr = json_get(r, "items");
        int n = (arr && arr->type == JSON_ARRAY) ? arr->as.arrayVal.count : 0;
        bool inc = (r && r->type == JSON_OBJECT);
        printf("Brace literal Ctrl+Space: %d items in %.3f ms (incomplete=%s)\n",
               n, dt1, inc ? "true" : "false");
        json_free(r); json_free(params);
    }
    return 0;
}
