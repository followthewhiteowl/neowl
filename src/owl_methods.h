// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_methods_h
#define owl_methods_h

#include "value.h"
#include "object.h"

/* ================================================================
 * EXTENSION METHOD DISPATCH
 * Allows primitive types to have methods: s.Majuscule(), n.Abs(), etc.
 * ================================================================ */

typedef enum {
    METHOD_TYPE_STRING,
    METHOD_TYPE_INTEGER,
    METHOD_TYPE_REAL,
    METHOD_TYPE_ARRAY,
    METHOD_TYPE_JSON,
    METHOD_TYPE_COLLECTION_QUEUE,
    METHOD_TYPE_COLLECTION_STACK,
    METHOD_TYPE_COLLECTION_LIST,
    METHOD_TYPE_DATE,
    METHOD_TYPE_TIME,
    METHOD_TYPE_DATETIME,
    METHOD_TYPE_DURATION,
    METHOD_TYPE_CHRONO,        /* ObjInstance with structName == "Chrono" */
    METHOD_TYPE_TIMER,         /* ObjInstance with structName == "Timer" */
    METHOD_TYPE_FICHIERDISQUE, /* ObjInstance with structName == "FichierDisque" */
    METHOD_TYPE_MOCK_NATIVE,   /* ObjMockNative (extension OWL) */
    METHOD_TYPE_XML_DOCUMENT,  /* ObjInstance with structName == "xmlDocument" */
    METHOD_TYPE_XML_NODE,      /* ObjInstance with structName == "xmlNoeud" */
    METHOD_TYPE_XML_ATTRIBUT,  /* ObjInstance with structName == "xmlAttribut" */
    METHOD_TYPE_XML_NAMESPACE, /* ObjInstance with structName == "xmlNamespace" */
    METHOD_TYPE_UI_LISTE,      /* ObjUiControl with kind == OWL_CONTROL_LISTE */
    METHOD_TYPE_UI_COMBO,      /* ObjUiControl with kind == OWL_CONTROL_COMBO */
    METHOD_TYPE_UI_SELECTEUR,  /* ObjUiControl with kind == OWL_CONTROL_SELECTEUR */
    METHOD_TYPE_UI_INTERRUPTEUR, /* ObjUiControl with kind == OWL_CONTROL_INTERRUPTEUR */
    METHOD_TYPE_UI_ZONE_REPETEE, /* ObjUiControl with kind == OWL_CONTROL_ZONE_REPETEE */
    METHOD_TYPE_UI_TABLE,      /* ObjUiControl with kind == OWL_CONTROL_TABLE */
    METHOD_TYPE_SQL_CONNEXION, /* ObjInstance with structName == "sqlConnexion" */
    METHOD_TYPE_UUID,          /* ObjInstance with structName == "UUID" (128 bits) */
    METHOD_TYPE_ZIP_ARCHIVE,   /* ObjZipArchive (OBJ_ZIP_ARCHIVE) */
    METHOD_TYPE_BUFFER,        /* ObjBuffer (OBJ_BUFFER) — accès octet fluent */
    METHOD_TYPE_CODEBARRES,    /* ObjInstance with structName == "CodeBarres" */
    METHOD_TYPE_VECTOR,        /* ObjVector (OBJ_VECTOR) — arithmetique fluent */
    METHOD_TYPE_MATRIX,        /* ObjMatrix (OBJ_MATRIX) — arithmetique fluent */
    METHOD_TYPE_TENSOR,        /* ObjTensor (OBJ_TENSOR) — Retropropage/Backpropagate */
    METHOD_TYPE_IAMODELE,      /* ObjIAModele (OBJ_IAMODELE) — AjouteCouche, Infère, Poids, Biais */
    METHOD_TYPE_LLM,           /* ObjLLM (OBJ_LLM) — Charge, Génère */
    METHOD_TYPE_DATASET,       /* ObjDataset (OBJ_DATASET) — Mélange, NombreLots, LotEntrées, LotCibles */
    METHOD_TYPE_AWS_S3_CLIENT, /* ObjInstance with structName == "AwsS3Client" */
    METHOD_TYPE_AWS_BEDROCK_CLIENT, /* ObjInstance with structName == "AwsBedrockClient" */
    METHOD_TYPE_AWS_LAMBDA_CLIENT, /* ObjInstance with structName == "AwsLambdaClient" */
    METHOD_TYPE_XLS_DOCUMENT,  /* ObjXlsDocument (OBJ_XLS_DOCUMENT) — classeur Excel */
    METHOD_TYPE_FTP_CLIENT,    /* ObjInstance with structName == "ftpClient" */
    METHOD_TYPE_MQTT_SESSION,  /* ObjInstance with structName == "mqttSession" */
    METHOD_TYPE_AMQP_CONNECTION, /* ObjInstance with structName == "amqpConnection" */
    METHOD_TYPE_STOMP_SESSION, /* ObjInstance with structName == "stompSession" */
    METHOD_TYPE_NATS_CONNECTION, /* ObjInstance with structName == "natsConnexion" */
    METHOD_TYPE_MLARBRE,       /* ObjMLArbre (OBJ_MLARBRE) — Entraîne, Prédit */
    METHOD_TYPE_MLFORET,       /* ObjMLForet (OBJ_MLFORET) — Entraîne, Prédit */
    METHOD_TYPE_MLBOOSTING,    /* ObjMLBoosting (OBJ_MLBOOSTING) — Entraîne, Prédit */
    METHOD_TYPE_MLLINEAIRE,    /* ObjMLLineaire (OBJ_MLLINEAIRE) — Entraîne, Prédit */
    METHOD_TYPE_MLLOGISTIQUE,  /* ObjMLLogistique (OBJ_MLLOGISTIQUE) — Entraîne, Prédit */
    METHOD_TYPE_MLKMEANS,      /* ObjMLKMeans (OBJ_MLKMEANS) — Entraîne, Prédit */
    METHOD_TYPE_COUNT
} MethodReceiverType;

/* Method tables grow dynamically (geometric realloc). Capacity is internal
 * to owl_methods.c -- callers don't see it. */

typedef struct {
    const char* name;       /* Method name: "Majuscule", "VersEntier", etc. */
    NativeFn function;
    int arity;              /* Total args INCLUDING receiver (-1 = variadic) */
} MethodEntry;

/* Initialize the method tables (call after stdlib_init). */
void methods_init(void);

/* Register a method on a type. Called from stdlib_*_init(). */
void owl_register_method(MethodReceiverType type, const char* name,
                         NativeFn fn, int arity);

/* Lookup a method by receiver type and name (case+accent insensitive).
 * Returns the NativeFn or NULL. Sets *outArity if non-NULL. */
NativeFn method_lookup(MethodReceiverType type, const char* name,
                       int nameLen, int* outArity);

/* Determine the MethodReceiverType for a Value.
 * Returns -1 if the value is not a method-dispatchable type. */
int method_receiver_type(Value receiver);

/* Enumerate all registered methods (for catalog generation).
 * Returns total count. Call with i in [0, method_catalog_count()) to iterate. */
int method_catalog_count(void);
/* Returns the i-th entry across all types. outTypeName is set to the
 * OWL type name ("chaine", "entier", "tableau", etc.). */
const MethodEntry* method_catalog_at(int i, const char** outTypeName);

/* ================================================================
 * Method-receiver metadata — source unique de verite pour chaque
 * MethodReceiverType : nom canonique FR + alias EN. Utilisee par :
 *  - method_catalog_at (LSP catalog dump) → renvoie canonical
 *  - receiver_type_from_name (registry auto-binding) → match canonical/EN
 *  - registry_self_test rule 24 → verifie que chaque enum a une entree
 *
 * Indexee par MethodReceiverType. C99 designated initializers garantissent
 * qu'un ajout d'enum non accompagne d'une entree ici est detecte (trou
 * zero-init → canonical=NULL).
 * ================================================================ */
typedef struct {
    const char* canonical;   /* "chaine", "entier", "tableau"... */
    const char* nameEN;      /* "string", "integer", "array"... NULL si identique */
} MethodReceiverInfo;

/* Accessor — retourne info ou NULL si t hors range. */
const MethodReceiverInfo* method_receiver_info(MethodReceiverType t);

#endif
