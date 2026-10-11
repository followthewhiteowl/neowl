// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* onnx_import.h — lecteur protobuf MINIMAL et SÉCURISÉ pour fichiers `.onnx`
 * (, lot 1). Décode le sous-schéma ONNX strictement nécessaire à
 * l'import d'un réseau de classification séquentiel :
 *
 *   ModelProto (champ 7 = graph)
 *     → GraphProto (champ 1 = node[], champ 5 = initializer[], champ 11 = input[])
 *       → NodeProto (input[]=1, output[]=2, name=3, op_type=4, attribute[]=5)
 *       → TensorProto (dims=1, data_type=2, float_data=4, int64_data=7, name=8,
 *                      raw_data=9, double_data=10)
 *       → AttributeProto (name=1, f=2, i=3, s=4, t=5(TensorProto), floats=7, ints=8, type=20)
 *
 * Le fichier `.onnx` est une entrée EXTERNE NON FIABLE : chaque lecture vérifie
 * ses bornes (offset < fin, longueur cohérente), la profondeur de récursion est
 * bornée, un varint ne dépasse pas 10 octets. Un fichier tronqué / malformé /
 * hostile produit une erreur propre — jamais de lecture hors-buffer, de crash,
 * ni de boucle infinie.
 *
 * Ce module ne dépend PAS de la VM / du GC : il produit une représentation
 * neutre en mémoire C-heap (OnnxModel). Le mapping ops → couches IAModèle et
 * l'allocation des Tenseurs OWL vivent dans stdlib_iamodele.c (concerns GC
 * isolés là-bas). onnx_model_free libère tout l'arbre.
 */
#ifndef OWL_ONNX_IMPORT_H
#define OWL_ONNX_IMPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define OWL_ONNX_MAX_DIMS 8

/* Un initializer (poids/biais/stats), aplati en double quel que soit son type
 * ONNX d'origine (FLOAT/DOUBLE), forme conservée dans dims[]. */
typedef struct {
    char*   name;                       /* nom référencé par les inputs des nodes */
    int     ndim;
    int64_t dims[OWL_ONNX_MAX_DIMS];
    double* data;                       /* count éléments, row-major */
    int64_t count;
} OnnxTensor;

/* Un attribut d'un node. On ne retient que ce qui sert au mapping : le scalaire
 * entier `i`, le scalaire flottant `f`, la liste d'entiers `ints`, et la valeur
 * chaîne `s` (auto_pad, mode de pooling…). */
typedef struct {
    char*    name;
    int64_t  i;         /* AttributeProto.i (INT) */
    double   f;         /* AttributeProto.f (FLOAT) */
    int64_t* ints;      /* AttributeProto.ints (INTS) */
    int      nints;
    char*    s;         /* AttributeProto.s (STRING/bytes), null-terminée ; NULL si absente */
    OnnxTensor* t;      /* AttributeProto.t (TENSOR) — value d'un Constant ; NULL si absent */
    bool     has_i;
    bool     has_f;
} OnnxAttr;

typedef struct {
    char*      op_type;
    char**     inputs;
    int        ninputs;
    char**     outputs;
    int        noutputs;
    OnnxAttr*  attrs;
    int        nattrs;
} OnnxNode;

typedef struct {
    OnnxNode*   nodes;
    int         nnodes;
    OnnxTensor* inits;
    int         ninits;
    /* Forme declaree de la PREMIERE entree du graphe (GraphProto.input[0] ->
     * TypeProto.tensor_type.shape). NCHW attendu pour un reseau d'images. Une
     * dimension symbolique (batch dynamique, dim_param sans dim_value) vaut 0.
     * input_ndim == 0 si le graphe ne declare pas d'entree typee. */
    int64_t     input_dims[OWL_ONNX_MAX_DIMS];
    int         input_ndim;
    /* Nom de la PREMIERE sortie declaree du graphe (GraphProto.output[0].name).
     * C'est la sortie du reseau au sens ONNX : l'executeur doit rendre CE
     * tenseur, pas une sortie devinee par heuristique. NULL si le graphe ne
     * declare aucune sortie (modele degenere -> fallback heuristique). */
    char*       output_name;
} OnnxGraph;

typedef struct {
    OnnxGraph graph;
} OnnxModel;

/* Lit et parse `path`. En succès : *out pointe un OnnxModel fraîchement alloué
 * (à libérer via onnx_model_free), rend true. En échec : remplit `err` (message
 * court, tronqué à errlen) et rend false, *out reste NULL. Ne crashe jamais,
 * quel que soit le contenu du fichier. */
bool onnx_parse_file(const char* path, OnnxModel** out, char* err, size_t errlen);

/* Libère un OnnxModel et tout son arbre (tolère NULL). */
void onnx_model_free(OnnxModel* m);

/* Cherche un initializer par nom (NULL si absent). */
const OnnxTensor* onnx_find_init(const OnnxGraph* g, const char* name);

/* Cherche un attribut d'un node par nom (NULL si absent). */
const OnnxAttr* onnx_node_attr(const OnnxNode* n, const char* name);

#endif /* OWL_ONNX_IMPORT_H */
