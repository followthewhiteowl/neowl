// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* onnx_export.h — writer protobuf MINIMAL pour fichiers `.onnx` (,
 * lot 2). Inverse du lecteur onnx_import.c : sérialise un graphe de
 * classification en ModelProto ONNX (opset 13) lisible par l'outillage de
 * référence ET ré-importable par onnx_import.c (round-trip OWL→ONNX→OWL).
 *
 * Contrairement au lecteur (entrée externe non fiable, bornes strictes), le
 * writer opère sur une STRUCTURE QU'ON CONTRÔLE : pas de validation d'entrée
 * hostile. Il expose un builder incrémental — le mapping couche IAModèle → op
 * ONNX vit dans stdlib_iamodele.c et remplit ce builder, puis onnx_export_write
 * émet le protobuf.
 *
 * Ce module ne dépend PAS de la VM / du GC : il ne connaît que des tableaux C
 * plats (doubles, int64, chaînes). L'allocation des Tenseurs OWL et leur lecture
 * restent côté stdlib_iamodele.c.
 *
 * Sous-schéma émis (numéros de champ, cf. onnx.proto officiel, proto2) :
 *   ModelProto : ir_version=1, opset_import=8, producer_name=2, graph=7
 *   OperatorSetIdProto : domain=1, version=2
 *   GraphProto : node=1, name=2, initializer=5, input=11, output=12
 *   NodeProto : input=1, output=2, name=3, op_type=4, attribute=5
 *   AttributeProto : name=1, f=2, i=3, type=20, floats=7, ints=8
 *   TensorProto : dims=1, data_type=2, raw_data=9, name=8
 *   ValueInfoProto : name=1, type=2 ; TypeProto : tensor_type=1 ;
 *   TypeProto.Tensor : elem_type=1, shape=2 ;
 *   TensorShapeProto : dim=1 ; TensorShapeProto.Dimension : dim_value=1,
 *   dim_param=2
 *
 * Le champ `shape` du tensor_type est REQUIS par onnx.checker (sans lui tout
 * outil tiers rejette le fichier). Chaque dimension est soit numérique
 * (dim_value), soit symbolique (dim_param, ex. "N"/"H"/"W" pour des tailles non
 * stockées dans l'IAModèle).
 */
#ifndef OWL_ONNX_EXPORT_H
#define OWL_ONNX_EXPORT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct OnnxExport OnnxExport;
typedef struct OnnxExportNode OnnxExportNode;

/* Une dimension de forme de ValueInfo : numérique si `param == NULL` (émise en
 * dim_value), symbolique sinon (émise en dim_param, nom copié). */
typedef struct {
    int64_t     value;
    const char* param;
} OnnxDim;

/* Crée un builder vide (NULL si mémoire insuffisante). */
OnnxExport* onnx_export_new(void);

/* Libère le builder et tout son contenu (tolère NULL). */
void onnx_export_free(OnnxExport* e);

/* Nomme l'entrée et la sortie du graphe (ValueInfoProto) avec leur forme. Les
 * noms et les noms de dims symboliques sont copiés. Le champ `shape` est
 * toujours émis (requis par onnx.checker) — même avec ndim == 0 (shape vide
 * mais présent). `in_dims`/`out_dims` peuvent être NULL si le ndim associé est
 * 0. */
void onnx_export_set_io(OnnxExport* e,
                        const char* input, const OnnxDim* in_dims, int in_ndim,
                        const char* output, const OnnxDim* out_dims, int out_ndim);

/* Ajoute un node `op_type` au graphe (dans l'ordre). Rend le node pour y greffer
 * entrées / sorties / attributs, ou NULL si mémoire insuffisante (l'échec est
 * aussi mémorisé et propagé par onnx_export_write). */
OnnxExportNode* onnx_export_add_node(OnnxExport* e, const char* op_type);

/* Greffe une entrée / sortie (nom de tenseur) sur un node. Tolère node NULL. */
void onnx_node_add_input(OnnxExportNode* n, const char* name);
void onnx_node_add_output(OnnxExportNode* n, const char* name);

/* Attributs de node. `_int` : scalaire entier (transB, axis) ; `_ints` : liste
 * d'entiers (kernel_shape, strides, pads) ; `_float` : scalaire flottant
 * (epsilon). Tolèrent node NULL. */
void onnx_node_attr_int(OnnxExportNode* n, const char* name, int64_t v);
void onnx_node_attr_ints(OnnxExportNode* n, const char* name,
                         const int64_t* v, int cnt);
void onnx_node_attr_float(OnnxExportNode* n, const char* name, double f);

/* Ajoute un initializer (poids/biais/stats) : TensorProto FLOAT, forme `dims`
 * (ndim axes), `count` valeurs row-major. Les données sont copiées et écrites en
 * raw_data little-endian float32. */
void onnx_export_add_initializer(OnnxExport* e, const char* name,
                                 const int64_t* dims, int ndim,
                                 const double* data, int64_t count);

/* Sérialise le graphe et l'écrit dans `path`. Rend true en succès. En échec
 * (mémoire, écriture disque, ou builder déjà en erreur) : remplit `err` (message
 * court tronqué à errlen) et rend false. */
bool onnx_export_write(OnnxExport* e, const char* path, char* err, size_t errlen);

#endif /* OWL_ONNX_EXPORT_H */
