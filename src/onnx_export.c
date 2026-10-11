// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* onnx_export.c — writer protobuf minimal (, lot 2).
 * Cf. onnx_export.h pour le contrat et le sous-schéma émis.
 *
 * Deux temps :
 *   1. Un builder mutable (OnnxExport) accumule nodes, initializers, I/O — rempli
 *      par le mapping couche→op de stdlib_iamodele.c.
 *   2. onnx_export_write sérialise ce builder en protobuf. Les sous-messages
 *      (node, tensor, graph...) sont construits chacun dans un tampon dédié puis
 *      recopiés dans le parent PRÉFIXÉS DE LEUR LONGUEUR (wire type 2). C'est
 *      l'écriture « en deux passes » : on connaît la taille d'un enfant une fois
 *      complet, avant de l'insérer.
 *
 * Aucune allocation OWL / accès VM ici (symétrie avec onnx_import.c).
 */
#include "onnx_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire types protobuf. */
#define PB_WIRE_VARINT 0
#define PB_WIRE_32BIT  5
#define PB_WIRE_LEN    2

/* TensorProto.data_type / TypeProto elem_type : FLOAT. */
#define ONNX_DT_FLOAT 1

/* AttributeProto.AttributeType (champ type=20). */
#define ONNX_ATTR_FLOAT 1
#define ONNX_ATTR_INT   2
#define ONNX_ATTR_INTS  7

#define ONNX_EXPORT_MAX_DIMS 8

/* Version d'opset et d'IR émises : opset 13 -> IR version 7 (cohérent avec les
 * fixtures torch importées). */
#define ONNX_IR_VERSION 7
#define ONNX_OPSET      13

/* ================================================================
 * Modèle mutable (builder)
 * ================================================================ */

typedef struct {
    char*    name;
    int      kind;    /* ONNX_ATTR_INT / ONNX_ATTR_INTS / ONNX_ATTR_FLOAT */
    int64_t  i;
    double   f;
    int64_t* ints;
    int      nints;
} ExpAttr;

struct OnnxExportNode {
    char*    op_type;
    char**   inputs;   int ninputs;  int incap;
    char**   outputs;  int noutputs; int outcap;
    ExpAttr* attrs;    int nattrs;   int atcap;
};

typedef struct {
    char*   name;
    int64_t dims[ONNX_EXPORT_MAX_DIMS];
    int     ndim;
    double* data;
    int64_t count;
} ExpInit;

/* Une dimension résolue de ValueInfo : `param` non-NULL => dim_param symbolique,
 * sinon dim_value = `value`. `param` est une copie possédée. */
typedef struct {
    int64_t value;
    char*   param;
} ExpDim;

struct OnnxExport {
    OnnxExportNode* nodes;  int nnodes;  int nodecap;
    ExpInit*        inits;  int ninits;  int initcap;
    char*           input;
    char*           output;
    ExpDim          in_dims[ONNX_EXPORT_MAX_DIMS];  int in_ndim;
    ExpDim          out_dims[ONNX_EXPORT_MAX_DIMS]; int out_ndim;
    bool            oom;     /* sticky : une allocation a échoué en construction */
};

/* strdup local (évite la dépendance POSIX). Rend NULL si s NULL ou OOM. */
static char* exp_strdup(const char* s) {
    if (s == NULL) return NULL;
    size_t n = strlen(s);
    char* p = malloc(n + 1);
    if (p == NULL) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* Ajoute un élément à un tableau dynamique générique (double la capacité). Rend
 * false si realloc échoue. */
static bool exp_grow(void** arr, int* count, int* cap, size_t elem) {
    if (*count >= *cap) {
        int nc = (*cap < 4) ? 4 : (*cap * 2);
        void* p = realloc(*arr, (size_t)nc * elem);
        if (p == NULL) return false;
        *arr = p;
        *cap = nc;
    }
    (*count)++;
    return true;
}

OnnxExport* onnx_export_new(void) {
    OnnxExport* e = calloc(1, sizeof(OnnxExport));
    return e;
}

/* Copie `n` dims source vers `dst` (params dupliqués). Rend false si un strdup
 * échoue. */
static bool exp_copy_dims(ExpDim* dst, const OnnxDim* src, int n) {
    for (int k = 0; k < n; k++) {
        dst[k].value = src[k].value;
        dst[k].param = exp_strdup(src[k].param);
        if (src[k].param != NULL && dst[k].param == NULL) return false;
    }
    return true;
}

void onnx_export_set_io(OnnxExport* e,
                        const char* input, const OnnxDim* in_dims, int in_ndim,
                        const char* output, const OnnxDim* out_dims, int out_ndim) {
    if (e == NULL) return;
    free(e->input);
    free(e->output);
    for (int k = 0; k < e->in_ndim; k++)  { free(e->in_dims[k].param);  e->in_dims[k].param = NULL; }
    for (int k = 0; k < e->out_ndim; k++) { free(e->out_dims[k].param); e->out_dims[k].param = NULL; }
    e->in_ndim = e->out_ndim = 0;

    e->input  = exp_strdup(input);
    e->output = exp_strdup(output);
    if ((input != NULL && e->input == NULL) ||
        (output != NULL && e->output == NULL)) e->oom = true;

    if (in_ndim  > ONNX_EXPORT_MAX_DIMS) in_ndim  = ONNX_EXPORT_MAX_DIMS;
    if (out_ndim > ONNX_EXPORT_MAX_DIMS) out_ndim = ONNX_EXPORT_MAX_DIMS;
    if (in_ndim  > 0 && !exp_copy_dims(e->in_dims,  in_dims,  in_ndim))  e->oom = true;
    if (out_ndim > 0 && !exp_copy_dims(e->out_dims, out_dims, out_ndim)) e->oom = true;
    e->in_ndim  = in_ndim;
    e->out_ndim = out_ndim;
}

OnnxExportNode* onnx_export_add_node(OnnxExport* e, const char* op_type) {
    if (e == NULL) return NULL;
    if (!exp_grow((void**)&e->nodes, &e->nnodes, &e->nodecap, sizeof(OnnxExportNode))) {
        e->oom = true;
        return NULL;
    }
    OnnxExportNode* n = &e->nodes[e->nnodes - 1];
    memset(n, 0, sizeof(*n));
    n->op_type = exp_strdup(op_type);
    if (op_type != NULL && n->op_type == NULL) e->oom = true;
    return n;
}

/* Fabrique de champ tableau-de-chaînes (inputs / outputs), factorisée. */
static void node_add_name(char*** arr, int* count, int* cap,
                          const char* name, OnnxExportNode* owner) {
    (void)owner;
    if (!exp_grow((void**)arr, count, cap, sizeof(char*))) return;
    (*arr)[*count - 1] = exp_strdup(name);
}

void onnx_node_add_input(OnnxExportNode* n, const char* name) {
    if (n == NULL) return;
    node_add_name(&n->inputs, &n->ninputs, &n->incap, name, n);
}

void onnx_node_add_output(OnnxExportNode* n, const char* name) {
    if (n == NULL) return;
    node_add_name(&n->outputs, &n->noutputs, &n->outcap, name, n);
}

static ExpAttr* node_new_attr(OnnxExportNode* n, const char* name, int kind) {
    if (!exp_grow((void**)&n->attrs, &n->nattrs, &n->atcap, sizeof(ExpAttr)))
        return NULL;
    ExpAttr* a = &n->attrs[n->nattrs - 1];
    memset(a, 0, sizeof(*a));
    a->name = exp_strdup(name);
    a->kind = kind;
    return a;
}

void onnx_node_attr_int(OnnxExportNode* n, const char* name, int64_t v) {
    if (n == NULL) return;
    ExpAttr* a = node_new_attr(n, name, ONNX_ATTR_INT);
    if (a != NULL) a->i = v;
}

void onnx_node_attr_ints(OnnxExportNode* n, const char* name,
                         const int64_t* v, int cnt) {
    if (n == NULL) return;
    ExpAttr* a = node_new_attr(n, name, ONNX_ATTR_INTS);
    if (a == NULL) return;
    if (cnt > 0) {
        a->ints = malloc((size_t)cnt * sizeof(int64_t));
        if (a->ints == NULL) return;
        memcpy(a->ints, v, (size_t)cnt * sizeof(int64_t));
        a->nints = cnt;
    }
}

void onnx_node_attr_float(OnnxExportNode* n, const char* name, double f) {
    if (n == NULL) return;
    ExpAttr* a = node_new_attr(n, name, ONNX_ATTR_FLOAT);
    if (a != NULL) a->f = f;
}

void onnx_export_add_initializer(OnnxExport* e, const char* name,
                                 const int64_t* dims, int ndim,
                                 const double* data, int64_t count) {
    if (e == NULL) return;
    if (ndim > ONNX_EXPORT_MAX_DIMS) { e->oom = true; return; }
    if (!exp_grow((void**)&e->inits, &e->ninits, &e->initcap, sizeof(ExpInit))) {
        e->oom = true;
        return;
    }
    ExpInit* t = &e->inits[e->ninits - 1];
    memset(t, 0, sizeof(*t));
    t->name = exp_strdup(name);
    if (name != NULL && t->name == NULL) { e->oom = true; return; }
    t->ndim = ndim;
    for (int k = 0; k < ndim; k++) t->dims[k] = dims[k];
    t->count = count;
    if (count > 0) {
        t->data = malloc((size_t)count * sizeof(double));
        if (t->data == NULL) { e->oom = true; return; }
        memcpy(t->data, data, (size_t)count * sizeof(double));
    }
}

static void attr_free(ExpAttr* a) {
    free(a->name);
    free(a->ints);
}
static void node_free(OnnxExportNode* n) {
    free(n->op_type);
    for (int i = 0; i < n->ninputs; i++) free(n->inputs[i]);
    free(n->inputs);
    for (int i = 0; i < n->noutputs; i++) free(n->outputs[i]);
    free(n->outputs);
    for (int i = 0; i < n->nattrs; i++) attr_free(&n->attrs[i]);
    free(n->attrs);
}

void onnx_export_free(OnnxExport* e) {
    if (e == NULL) return;
    for (int i = 0; i < e->nnodes; i++) node_free(&e->nodes[i]);
    free(e->nodes);
    for (int i = 0; i < e->ninits; i++) { free(e->inits[i].name); free(e->inits[i].data); }
    free(e->inits);
    free(e->input);
    free(e->output);
    for (int k = 0; k < e->in_ndim; k++)  free(e->in_dims[k].param);
    for (int k = 0; k < e->out_ndim; k++) free(e->out_dims[k].param);
    free(e);
}

/* ================================================================
 * Tampon protobuf croissant + primitives d'écriture
 * ================================================================ */

typedef struct { uint8_t* d; size_t len; size_t cap; bool ok; } PbBuf;

static void pbuf_init(PbBuf* b) { b->d = NULL; b->len = 0; b->cap = 0; b->ok = true; }
static void pbuf_free(PbBuf* b) { free(b->d); b->d = NULL; b->len = 0; b->cap = 0; }

static bool pbuf_reserve(PbBuf* b, size_t extra) {
    if (!b->ok) return false;
    if (b->len + extra <= b->cap) return true;
    size_t nc = (b->cap != 0) ? b->cap : 64;
    while (nc < b->len + extra) nc *= 2;
    uint8_t* p = realloc(b->d, nc);
    if (p == NULL) { b->ok = false; return false; }
    b->d = p; b->cap = nc;
    return true;
}

static void pbuf_byte(PbBuf* b, uint8_t v) {
    if (pbuf_reserve(b, 1)) b->d[b->len++] = v;
}
static void pbuf_raw(PbBuf* b, const uint8_t* p, size_t n) {
    if (pbuf_reserve(b, n)) { memcpy(b->d + b->len, p, n); b->len += n; }
}
static void pbuf_varint(PbBuf* b, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v != 0) byte |= 0x80;
        pbuf_byte(b, byte);
    } while (v != 0);
}
static void pbuf_tag(PbBuf* b, uint32_t field, uint32_t wire) {
    pbuf_varint(b, ((uint64_t)field << 3) | wire);
}
static void pbuf_field_varint(PbBuf* b, uint32_t field, uint64_t v) {
    pbuf_tag(b, field, PB_WIRE_VARINT);
    pbuf_varint(b, v);
}
static void pbuf_field_len(PbBuf* b, uint32_t field, const uint8_t* p, size_t n) {
    pbuf_tag(b, field, PB_WIRE_LEN);
    pbuf_varint(b, (uint64_t)n);
    pbuf_raw(b, p, n);
}
static void pbuf_field_string(PbBuf* b, uint32_t field, const char* s) {
    pbuf_field_len(b, field, (const uint8_t*)(s != NULL ? s : ""),
                   (s != NULL) ? strlen(s) : 0);
}
static void pbuf_field_float(PbBuf* b, uint32_t field, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint8_t le[4] = { (uint8_t)(u & 0xff), (uint8_t)((u >> 8) & 0xff),
                      (uint8_t)((u >> 16) & 0xff), (uint8_t)((u >> 24) & 0xff) };
    pbuf_tag(b, field, PB_WIRE_32BIT);
    pbuf_raw(b, le, 4);
}

/* Insère un sous-message déjà sérialisé (child) comme champ length-delimited. */
static void pbuf_field_msg(PbBuf* b, uint32_t field, const PbBuf* child) {
    if (!child->ok) { b->ok = false; return; }
    pbuf_field_len(b, field, child->d, child->len);
}

/* ================================================================
 * Sérialisation du sous-schéma ONNX
 * ================================================================ */

/* AttributeProto : name(1), type(20), puis f(2)/i(3)/ints(8) selon le type. */
static void ser_attr(PbBuf* out, const ExpAttr* a) {
    PbBuf b; pbuf_init(&b);
    pbuf_field_string(&b, 1, a->name);
    pbuf_field_varint(&b, 20, (uint64_t)a->kind);
    if (a->kind == ONNX_ATTR_INT) {
        pbuf_field_varint(&b, 3, (uint64_t)a->i);
    } else if (a->kind == ONNX_ATTR_FLOAT) {
        pbuf_field_float(&b, 2, (float)a->f);
    } else if (a->kind == ONNX_ATTR_INTS) {
        /* ints packed (champ 8, length-delimited de varints). */
        PbBuf packed; pbuf_init(&packed);
        for (int i = 0; i < a->nints; i++) pbuf_varint(&packed, (uint64_t)a->ints[i]);
        pbuf_field_msg(&b, 8, &packed);
        pbuf_free(&packed);
    }
    pbuf_field_msg(out, 5, &b);   /* NodeProto.attribute = 5 */
    pbuf_free(&b);
}

/* NodeProto : input(1,repeated), output(2,repeated), op_type(4), attribute(5). */
static void ser_node(PbBuf* out, const OnnxExportNode* n) {
    PbBuf b; pbuf_init(&b);
    for (int i = 0; i < n->ninputs; i++)  pbuf_field_string(&b, 1, n->inputs[i]);
    for (int i = 0; i < n->noutputs; i++) pbuf_field_string(&b, 2, n->outputs[i]);
    pbuf_field_string(&b, 4, n->op_type);
    for (int i = 0; i < n->nattrs; i++) ser_attr(&b, &n->attrs[i]);
    pbuf_field_msg(out, 1, &b);   /* GraphProto.node = 1 */
    pbuf_free(&b);
}

/* TensorProto : dims(1,repeated varint), data_type(2), raw_data(9), name(8).
 * Écrit les valeurs en raw_data little-endian float32 — le lecteur relit ce
 * format (raw_data + data_type FLOAT), round-trip exact des poids float. */
static void ser_init(PbBuf* out, const ExpInit* t) {
    PbBuf b; pbuf_init(&b);
    for (int k = 0; k < t->ndim; k++) pbuf_field_varint(&b, 1, (uint64_t)t->dims[k]);
    pbuf_field_varint(&b, 2, ONNX_DT_FLOAT);

    PbBuf raw; pbuf_init(&raw);
    for (int64_t k = 0; k < t->count; k++) {
        float f = (float)t->data[k];
        uint32_t u;
        memcpy(&u, &f, 4);
        uint8_t le[4] = { (uint8_t)(u & 0xff), (uint8_t)((u >> 8) & 0xff),
                          (uint8_t)((u >> 16) & 0xff), (uint8_t)((u >> 24) & 0xff) };
        pbuf_raw(&raw, le, 4);
    }
    pbuf_field_msg(&b, 9, &raw);
    pbuf_free(&raw);

    pbuf_field_string(&b, 8, t->name);
    pbuf_field_msg(out, 5, &b);   /* GraphProto.initializer = 5 */
    pbuf_free(&b);
}

/* TensorShapeProto : dim(1,repeated Dimension). Chaque Dimension : dim_value(1)
 * si numérique, sinon dim_param(2) symbolique. Émis même vide (shape présent
 * mais 0 dim) : onnx.checker exige la présence du champ, pas son contenu. */
static void ser_shape(PbBuf* out, const ExpDim* dims, int ndim) {
    PbBuf shape; pbuf_init(&shape);
    for (int k = 0; k < ndim; k++) {
        PbBuf dim; pbuf_init(&dim);        /* TensorShapeProto.Dimension */
        if (dims[k].param != NULL) {
            pbuf_field_string(&dim, 2, dims[k].param);
        } else {
            pbuf_field_varint(&dim, 1, (uint64_t)dims[k].value);
        }
        pbuf_field_msg(&shape, 1, &dim);
        pbuf_free(&dim);
    }
    pbuf_field_msg(out, 2, &shape);        /* TypeProto.Tensor.shape = 2 */
    pbuf_free(&shape);
}

/* ValueInfoProto : name(1), type(2 -> TypeProto.tensor_type(1) ->
 * TypeProto.Tensor{ elem_type(1)=FLOAT, shape(2) }). Le shape est TOUJOURS émis
 * (requis par onnx.checker). */
static void ser_valueinfo(PbBuf* out, uint32_t field, const char* name,
                          const ExpDim* dims, int ndim) {
    PbBuf tt; pbuf_init(&tt);             /* TypeProto.Tensor */
    pbuf_field_varint(&tt, 1, ONNX_DT_FLOAT);
    ser_shape(&tt, dims, ndim);

    PbBuf type; pbuf_init(&type);          /* TypeProto */
    pbuf_field_msg(&type, 1, &tt);
    pbuf_free(&tt);

    PbBuf vi; pbuf_init(&vi);              /* ValueInfoProto */
    pbuf_field_string(&vi, 1, name);
    pbuf_field_msg(&vi, 2, &type);
    pbuf_free(&type);

    pbuf_field_msg(out, field, &vi);
    pbuf_free(&vi);
}

/* GraphProto : node(1), name(2), initializer(5), input(11), output(12). */
static void ser_graph(PbBuf* out, const OnnxExport* e) {
    PbBuf b; pbuf_init(&b);
    for (int i = 0; i < e->nnodes; i++) ser_node(&b, &e->nodes[i]);
    pbuf_field_string(&b, 2, "owl_export");
    for (int i = 0; i < e->ninits; i++) ser_init(&b, &e->inits[i]);
    if (e->input  != NULL) ser_valueinfo(&b, 11, e->input,  e->in_dims,  e->in_ndim);
    if (e->output != NULL) ser_valueinfo(&b, 12, e->output, e->out_dims, e->out_ndim);
    pbuf_field_msg(out, 7, &b);   /* ModelProto.graph = 7 */
    pbuf_free(&b);
}

/* ModelProto : ir_version(1), opset_import(8), producer_name(2), graph(7). */
static void ser_model(PbBuf* out, const OnnxExport* e) {
    pbuf_field_varint(out, 1, ONNX_IR_VERSION);

    PbBuf opset; pbuf_init(&opset);        /* OperatorSetIdProto (domain "" par défaut) */
    pbuf_field_varint(&opset, 2, ONNX_OPSET);
    pbuf_field_msg(out, 8, &opset);
    pbuf_free(&opset);

    pbuf_field_string(out, 2, "owl");
    ser_graph(out, e);
}

/* ================================================================
 * Écriture fichier
 * ================================================================ */

bool onnx_export_write(OnnxExport* e, const char* path, char* err, size_t errlen) {
    if (err != NULL && errlen > 0) err[0] = '\0';
    if (e == NULL) {
        if (err != NULL && errlen > 0) snprintf(err, errlen, "modele nul");
        return false;
    }
    if (e->oom) {
        if (err != NULL && errlen > 0) snprintf(err, errlen, "memoire insuffisante");
        return false;
    }

    PbBuf model; pbuf_init(&model);
    ser_model(&model, e);
    if (!model.ok) {
        pbuf_free(&model);
        if (err != NULL && errlen > 0) snprintf(err, errlen, "memoire insuffisante");
        return false;
    }

    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        pbuf_free(&model);
        if (err != NULL && errlen > 0) snprintf(err, errlen, "fichier non ouvrable en ecriture");
        return false;
    }
    size_t written = fwrite(model.d, 1, model.len, f);
    fclose(f);
    bool ok = (written == model.len);
    pbuf_free(&model);
    if (!ok) {
        if (err != NULL && errlen > 0) snprintf(err, errlen, "ecriture partielle");
        return false;
    }
    return true;
}
