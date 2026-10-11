// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* onnx_import.c — lecteur protobuf minimal et sécurisé (, lot 1).
 * Cf. onnx_import.h pour le contrat et le sous-schéma décodé.
 *
 * SÉCURITÉ (fichier externe non fiable) : toute primitive de lecture opère sur
 * un segment [pos, end) et vérifie ses bornes AVANT de déréférencer. Un varint
 * est borné à 10 octets (overflow détecté). La récursion des sous-messages est
 * bornée par OWL_ONNX_MAX_DEPTH. Les tableaux croissent par realloc, donc leur
 * taille est intrinsèquement bornée par la taille du fichier (chaque élément
 * consomme au moins un octet). Aucune allocation OWL / accès VM ici.
 */
#include "onnx_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Wire types protobuf. */
#define PB_WIRE_VARINT 0
#define PB_WIRE_64BIT  1
#define PB_WIRE_LEN    2
#define PB_WIRE_32BIT  5

#define OWL_ONNX_MAX_DEPTH 32

/* Types de données TensorProto pris en charge (sous-ensemble). */
#define ONNX_DT_FLOAT  1
#define ONNX_DT_INT64  7
#define ONNX_DT_DOUBLE 11

/* ================================================================
 * Primitives protobuf bornées
 * ================================================================ */

/* Lit un varint LEB128 depuis d[*pos], borné par `end` et par 10 octets max
 * (taille d'un varint 64 bits). Avance *pos. Rend false sur dépassement de
 * borne ou overflow (11e octet). */
static bool pb_varint(const uint8_t* d, size_t* pos, size_t end, uint64_t* out) {
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < 10; i++) {
        if (*pos >= end) return false;
        uint8_t b = d[*pos];
        (*pos)++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) { *out = v; return true; }
        shift += 7;
    }
    return false;   /* 10 octets tous continués -> varint invalide */
}

/* Lit un tag (field_number, wire_type). Rend false sur borne ou field 0. */
static bool pb_tag(const uint8_t* d, size_t* pos, size_t end,
                   uint32_t* field, uint32_t* wire) {
    uint64_t key;
    if (!pb_varint(d, pos, end, &key)) return false;
    *field = (uint32_t)(key >> 3);
    *wire  = (uint32_t)(key & 0x7);
    return *field != 0;
}

/* Pour un champ length-delimited : lit la longueur puis vérifie qu'elle tient
 * dans [pos, end). Renvoie le segment [*fstart, *fend) et avance *pos jusqu'à
 * *fend. Rend false si la longueur déborde. */
static bool pb_len_field(const uint8_t* d, size_t* pos, size_t end,
                         size_t* fstart, size_t* fend) {
    uint64_t len;
    if (!pb_varint(d, pos, end, &len)) return false;
    if (len > (uint64_t)(end - *pos)) return false;   /* débordement */
    *fstart = *pos;
    *fend   = *pos + (size_t)len;
    *pos    = *fend;
    return true;
}

/* Saute un champ de wire type quelconque en vérifiant les bornes. Les groupes
 * (wire 3/4, dépréciés) sont refusés. */
static bool pb_skip(const uint8_t* d, size_t* pos, size_t end, uint32_t wire) {
    switch (wire) {
        case PB_WIRE_VARINT: {
            uint64_t tmp;
            return pb_varint(d, pos, end, &tmp);
        }
        case PB_WIRE_64BIT:
            if (end - *pos < 8) return false;
            *pos += 8;
            return true;
        case PB_WIRE_LEN: {
            size_t s, e;
            return pb_len_field(d, pos, end, &s, &e);
        }
        case PB_WIRE_32BIT:
            if (end - *pos < 4) return false;
            *pos += 4;
            return true;
        default:
            return false;   /* groupes (3/4) et wire types inconnus : refus */
    }
}

/* Reconstruit un float/double depuis 4/8 octets little-endian, sans supposer
 * l'endianness de l'hôte (assemblage bit-à-bit puis memcpy). */
static float le_float(const uint8_t* p) {
    uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
               | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static double le_double(const uint8_t* p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= (uint64_t)p[i] << (8 * i);
    double dv;
    memcpy(&dv, &u, 8);
    return dv;
}

/* Copie [start, end) en une chaîne C null-terminée (les chaînes protobuf ne le
 * sont pas). Rend NULL si l'allocation échoue. */
static char* pb_dup_str(const uint8_t* d, size_t start, size_t end) {
    size_t n = end - start;
    char* s = malloc(n + 1);
    if (s == NULL) return NULL;
    memcpy(s, d + start, n);
    s[n] = '\0';
    return s;
}

/* Ajoute un élément à un tableau dynamique générique (double la capacité).
 * `*arr` (void*), `*count`/`*cap` (int), `elem_size`. Rend false si realloc
 * échoue.
 *
 * Le nouveau slot est zéro-initialisé AVANT l'incrément du compteur : realloc
 * ne zéro-initialise pas, donc un slot fraîchement rendu contient du garbage.
 * Un parseur de sous-message (parse_node/parse_tensor/parse_attr) peut sortir
 * par un retour précoce (garde de profondeur) AVANT son propre memset ; le
 * libérateur parent repasserait alors sur ce slot déjà compté. Zéro-initialiser
 * ici rend tout retour précoce sûr en un seul point (pointeurs NULL,
 * free(NULL) inoffensif). Le memset redondant côté parseurs reste sans effet. */
static bool grow_push(void** arr, int* count, int* cap, size_t elem_size) {
    if (*count >= *cap) {
        int newcap = (*cap < 4) ? 4 : (*cap * 2);
        void* p = realloc(*arr, (size_t)newcap * elem_size);
        if (p == NULL) return false;
        *arr = p;
        *cap = newcap;
    }
    memset((char*)*arr + (size_t)*count * elem_size, 0, elem_size);
    (*count)++;
    return true;
}

/* ================================================================
 * Parse des sous-messages (chacun borné à son segment [start, end))
 * ================================================================ */

static bool parse_tensor(const uint8_t* d, size_t start, size_t end,
                         OnnxTensor* t, int depth);

/* Libérateurs (définis plus bas) — déclarés ici pour que les chemins d'échec
 * des parseurs puissent nettoyer les structures partiellement construites. */
static void tensor_free(OnnxTensor* t);
static void node_free(OnnxNode* n);
static void graph_free(OnnxGraph* g);

/* TensorProto : dims(1,varint,repeated), data_type(2,varint), float_data(4,
 * packed), int64_data(7, varint repeated OU packed), name(8,string),
 * raw_data(9,bytes), double_data(10,packed). */
static bool parse_tensor(const uint8_t* d, size_t start, size_t end,
                         OnnxTensor* t, int depth) {
    if (depth > OWL_ONNX_MAX_DEPTH) return false;
    memset(t, 0, sizeof(*t));
    int data_type = 0;
    size_t raw_start = 0, raw_end = 0;
    size_t fd_start = 0, fd_end = 0;   /* float_data packed */
    size_t dd_start = 0, dd_end = 0;   /* double_data packed */
    /* int64_data : accumulé au fil de l'eau (varint repeated OU packed) dans un
     * tableau C-heap, car ses éléments ne font pas une taille d'octet fixe. */
    int64_t* id_vals = NULL;
    int      id_count = 0, id_cap = 0;

    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) goto fail;
        if (field == 1 && wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) goto fail;
            if (t->ndim < OWL_ONNX_MAX_DIMS) t->dims[t->ndim] = (int64_t)v;
            t->ndim++;
        } else if (field == 1 && wire == PB_WIRE_LEN) {
            /* dims packed (rare mais licite). */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            size_t p = s;
            while (p < e) {
                uint64_t v;
                if (!pb_varint(d, &p, e, &v)) goto fail;
                if (t->ndim < OWL_ONNX_MAX_DIMS) t->dims[t->ndim] = (int64_t)v;
                t->ndim++;
            }
        } else if (field == 2 && wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) goto fail;
            data_type = (int)v;
        } else if (field == 4 && wire == PB_WIRE_LEN) {
            if (!pb_len_field(d, &pos, end, &fd_start, &fd_end)) goto fail;
        } else if (field == 7 && wire == PB_WIRE_VARINT) {
            /* int64_data (élément unique, encodage non-packed). */
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) goto fail;
            if (!grow_push((void**)&id_vals, &id_count, &id_cap, sizeof(int64_t)))
                goto fail;
            id_vals[id_count - 1] = (int64_t)v;
        } else if (field == 7 && wire == PB_WIRE_LEN) {
            /* int64_data packed (cas usuel). */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            size_t p = s;
            while (p < e) {
                uint64_t v;
                if (!pb_varint(d, &p, e, &v)) goto fail;
                if (!grow_push((void**)&id_vals, &id_count, &id_cap, sizeof(int64_t)))
                    goto fail;
                id_vals[id_count - 1] = (int64_t)v;
            }
        } else if (field == 8 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            free(t->name);
            t->name = pb_dup_str(d, s, e);
            if (t->name == NULL) goto fail;
        } else if (field == 9 && wire == PB_WIRE_LEN) {
            if (!pb_len_field(d, &pos, end, &raw_start, &raw_end)) goto fail;
        } else if (field == 10 && wire == PB_WIRE_LEN) {
            if (!pb_len_field(d, &pos, end, &dd_start, &dd_end)) goto fail;
        } else {
            if (!pb_skip(d, &pos, end, wire)) goto fail;
        }
    }
    if (t->ndim > OWL_ONNX_MAX_DIMS) goto fail;   /* forme trop profonde */

    /* Matérialise les données en double, selon la source disponible. Les entiers
     * 64 bits (shape-tensors de Reshape, indices…) sont aplatis en double comme
     * les flottants : les valeurs utiles (dimensions, petits entiers) sont
     * exactement représentables. */
    if (raw_end > raw_start) {
        size_t nbytes = raw_end - raw_start;
        const uint8_t* p = d + raw_start;
        if (data_type == ONNX_DT_FLOAT) {
            int64_t n = (int64_t)(nbytes / 4);
            t->data = malloc((size_t)n * sizeof(double) + 1);
            if (t->data == NULL) goto fail;
            for (int64_t k = 0; k < n; k++) t->data[k] = (double)le_float(p + k * 4);
            t->count = n;
        } else if (data_type == ONNX_DT_DOUBLE) {
            int64_t n = (int64_t)(nbytes / 8);
            t->data = malloc((size_t)n * sizeof(double) + 1);
            if (t->data == NULL) goto fail;
            for (int64_t k = 0; k < n; k++) t->data[k] = le_double(p + k * 8);
            t->count = n;
        } else if (data_type == ONNX_DT_INT64) {
            int64_t n = (int64_t)(nbytes / 8);
            t->data = malloc((size_t)n * sizeof(double) + 1);
            if (t->data == NULL) goto fail;
            for (int64_t k = 0; k < n; k++) {
                uint64_t u = 0;
                for (int b = 0; b < 8; b++) u |= (uint64_t)p[k * 8 + b] << (8 * b);
                t->data[k] = (double)(int64_t)u;   /* little-endian, complément à deux */
            }
            t->count = n;
        } else {
            goto fail;   /* type de poids non pris en charge (int8, etc.) */
        }
    } else if (fd_end > fd_start) {
        size_t nbytes = fd_end - fd_start;
        int64_t n = (int64_t)(nbytes / 4);
        t->data = malloc((size_t)n * sizeof(double) + 1);
        if (t->data == NULL) goto fail;
        for (int64_t k = 0; k < n; k++) t->data[k] = (double)le_float(d + fd_start + k * 4);
        t->count = n;
    } else if (dd_end > dd_start) {
        size_t nbytes = dd_end - dd_start;
        int64_t n = (int64_t)(nbytes / 8);
        t->data = malloc((size_t)n * sizeof(double) + 1);
        if (t->data == NULL) goto fail;
        for (int64_t k = 0; k < n; k++) t->data[k] = le_double(d + dd_start + k * 8);
        t->count = n;
    } else if (id_count > 0) {
        t->data = malloc((size_t)id_count * sizeof(double) + 1);
        if (t->data == NULL) goto fail;
        for (int k = 0; k < id_count; k++) t->data[k] = (double)id_vals[k];
        t->count = id_count;
    } else {
        /* Aucune donnée : tenseur vide (0 éléments), toléré. */
        t->count = 0;
        t->data = NULL;
    }
    free(id_vals);
    return true;
fail:
    free(id_vals);
    return false;
}

/* AttributeProto : name(1,string), f(2,32bit), i(3,varint), s(4,bytes),
 * t(5,TensorProto — value d'un Constant), floats(7,packed), ints(8, varint
 * repeated OU packed), type(20,varint, ignoré : on infère du champ peuplé). */
static bool parse_attr(const uint8_t* d, size_t start, size_t end,
                       OnnxAttr* a, int depth) {
    if (depth > OWL_ONNX_MAX_DEPTH) return false;
    memset(a, 0, sizeof(*a));
    int intsCap = 0;

    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) goto fail;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            free(a->name);
            a->name = pb_dup_str(d, s, e);
            if (a->name == NULL) goto fail;
        } else if (field == 2 && wire == PB_WIRE_32BIT) {
            if (end - pos < 4) goto fail;
            a->f = (double)le_float(d + pos);
            a->has_f = true;
            pos += 4;
        } else if (field == 3 && wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) goto fail;
            a->i = (int64_t)v;
            a->has_i = true;
        } else if (field == 4 && wire == PB_WIRE_LEN) {
            /* s (bytes/string) : auto_pad (VALID/SAME_UPPER/…), mode de pool… */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            free(a->s);
            a->s = pb_dup_str(d, s, e);
            if (a->s == NULL) goto fail;
        } else if (field == 5 && wire == PB_WIRE_LEN) {
            /* t (TensorProto) : value d'un nœud Constant (facteurs Resize…). */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (a->t != NULL) { tensor_free(a->t); free(a->t); a->t = NULL; }
            a->t = (OnnxTensor*)calloc(1, sizeof(OnnxTensor));
            if (a->t == NULL) goto fail;
            if (!parse_tensor(d, s, e, a->t, depth + 1)) goto fail;
        } else if (field == 8 && wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) goto fail;
            if (!grow_push((void**)&a->ints, &a->nints, &intsCap, sizeof(int64_t)))
                goto fail;
            a->ints[a->nints - 1] = (int64_t)v;
        } else if (field == 8 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            size_t p = s;
            while (p < e) {
                uint64_t v;
                if (!pb_varint(d, &p, e, &v)) goto fail;
                if (!grow_push((void**)&a->ints, &a->nints, &intsCap, sizeof(int64_t)))
                    goto fail;
                a->ints[a->nints - 1] = (int64_t)v;
            }
        } else {
            if (!pb_skip(d, &pos, end, wire)) goto fail;
        }
    }
    return true;
fail:
    free(a->name);
    free(a->ints);
    free(a->s);
    if (a->t != NULL) { tensor_free(a->t); free(a->t); }
    memset(a, 0, sizeof(*a));
    return false;
}

/* NodeProto : input(1,string,repeated), output(2,string,repeated), name(3),
 * op_type(4,string), attribute(5,AttributeProto,repeated). */
static bool parse_node(const uint8_t* d, size_t start, size_t end,
                       OnnxNode* n, int depth) {
    if (depth > OWL_ONNX_MAX_DEPTH) return false;
    memset(n, 0, sizeof(*n));
    int inCap = 0, outCap = 0, atCap = 0;

    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) goto fail;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (!grow_push((void**)&n->inputs, &n->ninputs, &inCap, sizeof(char*)))
                goto fail;
            n->inputs[n->ninputs - 1] = pb_dup_str(d, s, e);
            if (n->inputs[n->ninputs - 1] == NULL) goto fail;
        } else if (field == 2 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (!grow_push((void**)&n->outputs, &n->noutputs, &outCap, sizeof(char*)))
                goto fail;
            n->outputs[n->noutputs - 1] = pb_dup_str(d, s, e);
            if (n->outputs[n->noutputs - 1] == NULL) goto fail;
        } else if (field == 4 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            free(n->op_type);
            n->op_type = pb_dup_str(d, s, e);
            if (n->op_type == NULL) goto fail;
        } else if (field == 5 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (!grow_push((void**)&n->attrs, &n->nattrs, &atCap, sizeof(OnnxAttr)))
                goto fail;
            if (!parse_attr(d, s, e, &n->attrs[n->nattrs - 1], depth + 1))
                goto fail;
        } else {
            if (!pb_skip(d, &pos, end, wire)) goto fail;
        }
    }
    return true;
fail:
    node_free(n);
    memset(n, 0, sizeof(*n));   /* le libérateur du graphe ne repassera pas dessus */
    return false;
}

/* TensorShapeProto.Dimension : dim_value(1,varint), dim_param(2,string). Une
 * dimension symbolique (dim_param, batch dynamique) n'a pas de dim_value -> 0. */
static bool parse_dim(const uint8_t* d, size_t start, size_t end, int64_t* out) {
    *out = 0;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 1 && wire == PB_WIRE_VARINT) {
            uint64_t v;
            if (!pb_varint(d, &pos, end, &v)) return false;
            *out = (int64_t)v;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* TensorShapeProto : dim(1,repeated Dimension). Remplit dims[]/ndim (bornes a
 * OWL_ONNX_MAX_DIMS ; ndim compte le rang reel meme au-dela de la borne). */
static bool parse_shape(const uint8_t* d, size_t start, size_t end,
                        int64_t* dims, int* ndim) {
    *ndim = 0;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) return false;
            int64_t dv;
            if (!parse_dim(d, s, e, &dv)) return false;
            if (*ndim < OWL_ONNX_MAX_DIMS) dims[*ndim] = dv;
            (*ndim)++;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* TypeProto.Tensor : elem_type(1,varint), shape(2,TensorShapeProto). */
static bool parse_type_tensor(const uint8_t* d, size_t start, size_t end,
                              int64_t* dims, int* ndim) {
    *ndim = 0;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 2 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) return false;
            if (!parse_shape(d, s, e, dims, ndim)) return false;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* TypeProto : tensor_type(1,Tensor). Les autres variantes (sequence/map/...)
 * sont ignorees : une entree image est un tensor_type. */
static bool parse_type(const uint8_t* d, size_t start, size_t end,
                       int64_t* dims, int* ndim) {
    *ndim = 0;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) return false;
            if (!parse_type_tensor(d, s, e, dims, ndim)) return false;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* ValueInfoProto : name(1,string), type(2,TypeProto). Extrait la forme du
 * tenseur d'entree dans dims[]/ndim (ndim=0 si non typee). */
static bool parse_value_info(const uint8_t* d, size_t start, size_t end,
                             int64_t* dims, int* ndim) {
    *ndim = 0;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 2 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) return false;
            if (!parse_type(d, s, e, dims, ndim)) return false;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* ValueInfoProto : name(1,string), type(2,TypeProto). Copie le nom dans *out
 * (chaine C fraiche, a liberer par l'appelant ; NULL si absent). */
static bool parse_value_info_name(const uint8_t* d, size_t start, size_t end,
                                  char** out) {
    *out = NULL;
    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) return false;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) return false;
            free(*out);
            *out = pb_dup_str(d, s, e);
            if (*out == NULL) return false;
        } else {
            if (!pb_skip(d, &pos, end, wire)) return false;
        }
    }
    return true;
}

/* GraphProto : node(1,repeated), initializer(5,repeated TensorProto),
 * input(11,repeated ValueInfoProto), output(12,repeated ValueInfoProto).
 * Seules la forme du PREMIER input (l'entree du reseau) et le nom de la
 * PREMIERE sortie declaree (l'exécuteur doit rendre ce tenseur) sont retenus.
 * Le reste des ValueInfo (name, value_info=13...) est ignore. */
static bool parse_graph(const uint8_t* d, size_t start, size_t end,
                        OnnxGraph* g, int depth) {
    if (depth > OWL_ONNX_MAX_DEPTH) return false;
    int nodeCap = 0, initCap = 0;

    size_t pos = start;
    while (pos < end) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, end, &field, &wire)) goto fail;
        if (field == 1 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (!grow_push((void**)&g->nodes, &g->nnodes, &nodeCap, sizeof(OnnxNode)))
                goto fail;
            if (!parse_node(d, s, e, &g->nodes[g->nnodes - 1], depth + 1))
                goto fail;
        } else if (field == 11 && wire == PB_WIRE_LEN) {
            /* input[] : ne retenir que la forme du premier (l'entree du reseau).
             * Les modeles IR<4 listent aussi les initializers ici, mais l'entree
             * reelle vient en tete pour les exports standards (torch/tf). */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (g->input_ndim == 0) {
                int64_t dims[OWL_ONNX_MAX_DIMS];
                int nd = 0;
                if (!parse_value_info(d, s, e, dims, &nd)) goto fail;
                if (nd > OWL_ONNX_MAX_DIMS) nd = OWL_ONNX_MAX_DIMS;
                for (int k = 0; k < nd; k++) g->input_dims[k] = dims[k];
                g->input_ndim = nd;
            }
        } else if (field == 12 && wire == PB_WIRE_LEN) {
            /* output[] : ne retenir que le nom de la premiere sortie declaree
             * (la sortie du reseau au sens ONNX). */
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (g->output_name == NULL) {
                char* nm = NULL;
                if (!parse_value_info_name(d, s, e, &nm)) { free(nm); goto fail; }
                g->output_name = nm;   /* NULL tolere si la sortie n'a pas de nom */
            }
        } else if (field == 5 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, end, &s, &e)) goto fail;
            if (!grow_push((void**)&g->inits, &g->ninits, &initCap, sizeof(OnnxTensor)))
                goto fail;
            if (!parse_tensor(d, s, e, &g->inits[g->ninits - 1], depth + 1))
                goto fail;
        } else {
            if (!pb_skip(d, &pos, end, wire)) goto fail;
        }
    }
    return true;
fail:
    graph_free(g);
    memset(g, 0, sizeof(*g));   /* onnx_model_free ne repassera pas dessus */
    return false;
}

/* ModelProto : graph(7, GraphProto). Le reste (ir_version, producer_*, opset)
 * est ignoré. */
static bool parse_model(const uint8_t* d, size_t len, OnnxModel* m) {
    memset(m, 0, sizeof(*m));
    bool haveGraph = false;

    size_t pos = 0;
    while (pos < len) {
        uint32_t field, wire;
        if (!pb_tag(d, &pos, len, &field, &wire)) return false;
        if (field == 7 && wire == PB_WIRE_LEN) {
            size_t s, e;
            if (!pb_len_field(d, &pos, len, &s, &e)) return false;
            if (!parse_graph(d, s, e, &m->graph, 1)) return false;
            haveGraph = true;
        } else {
            if (!pb_skip(d, &pos, len, wire)) return false;
        }
    }
    return haveGraph;
}

/* ================================================================
 * Cycle de vie / accès
 * ================================================================ */

static void tensor_free(OnnxTensor* t) {
    free(t->name);
    free(t->data);
}
static void attr_free(OnnxAttr* a) {
    free(a->name);
    free(a->ints);
    free(a->s);
    if (a->t != NULL) { tensor_free(a->t); free(a->t); }
}
static void node_free(OnnxNode* n) {
    free(n->op_type);
    for (int i = 0; i < n->ninputs; i++) free(n->inputs[i]);
    free(n->inputs);
    for (int i = 0; i < n->noutputs; i++) free(n->outputs[i]);
    free(n->outputs);
    for (int i = 0; i < n->nattrs; i++) attr_free(&n->attrs[i]);
    free(n->attrs);
}

static void graph_free(OnnxGraph* g) {
    for (int i = 0; i < g->nnodes; i++) node_free(&g->nodes[i]);
    free(g->nodes);
    for (int i = 0; i < g->ninits; i++) tensor_free(&g->inits[i]);
    free(g->inits);
    free(g->output_name);
}

void onnx_model_free(OnnxModel* m) {
    if (m == NULL) return;
    graph_free(&m->graph);
    free(m);
}

const OnnxTensor* onnx_find_init(const OnnxGraph* g, const char* name) {
    if (name == NULL) return NULL;
    for (int i = 0; i < g->ninits; i++) {
        if (g->inits[i].name != NULL && strcmp(g->inits[i].name, name) == 0)
            return &g->inits[i];
    }
    return NULL;
}

const OnnxAttr* onnx_node_attr(const OnnxNode* n, const char* name) {
    for (int i = 0; i < n->nattrs; i++) {
        if (n->attrs[i].name != NULL && strcmp(n->attrs[i].name, name) == 0)
            return &n->attrs[i];
    }
    return NULL;
}

static void set_err(char* err, size_t errlen, const char* msg) {
    if (err == NULL || errlen == 0) return;
    strncpy(err, msg, errlen - 1);
    err[errlen - 1] = '\0';
}

bool onnx_parse_file(const char* path, OnnxModel** out, char* err, size_t errlen) {
    *out = NULL;

    FILE* f = fopen(path, "rb");
    if (f == NULL) { set_err(err, errlen, "fichier introuvable ou illisible"); return false; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); set_err(err, errlen, "fichier illisible"); return false; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); set_err(err, errlen, "fichier vide"); return false; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); set_err(err, errlen, "fichier illisible"); return false; }

    uint8_t* buf = malloc((size_t)size);
    if (buf == NULL) { fclose(f); set_err(err, errlen, "memoire insuffisante"); return false; }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) { free(buf); set_err(err, errlen, "lecture partielle"); return false; }

    OnnxModel* m = malloc(sizeof(OnnxModel));
    if (m == NULL) { free(buf); set_err(err, errlen, "memoire insuffisante"); return false; }

    bool ok = parse_model(buf, (size_t)size, m);
    free(buf);
    if (!ok) {
        onnx_model_free(m);
        set_err(err, errlen, "protobuf ONNX invalide ou tronque");
        return false;
    }
    *out = m;
    return true;
}
