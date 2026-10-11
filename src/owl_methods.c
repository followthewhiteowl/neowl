// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_methods.h"
#include "scanner.h"
#include "ui_decl.h"   /* OwlControlKind enum */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ================================================================
 * STORAGE: one table per receiver type, dynamically grown.
 *
 * Initial capacity = INITIAL_CAP. When a table fills, it doubles
 * (geometric growth) so registration is amortized O(1). Tables are
 * never freed — they live for the program duration.
 *
 * Lookup is O(n) linear with accent-stripping. Acceptable up to a few
 * hundred entries per type given how rarely we hit hot loops on method
 * dispatch alone.
 * ================================================================ */

#define INITIAL_CAP 16

static MethodEntry* method_tables[METHOD_TYPE_COUNT];
static int          method_counts[METHOD_TYPE_COUNT];
static int          method_capacities[METHOD_TYPE_COUNT];

/* ================================================================
 * INIT / REGISTER
 * ================================================================ */

void methods_init(void) {
    /* Free any existing tables (idempotent: support re-init in tests). */
    for (int t = 0; t < METHOD_TYPE_COUNT; t++) {
        if (method_tables[t]) free(method_tables[t]);
        method_tables[t]     = NULL;
        method_counts[t]     = 0;
        method_capacities[t] = 0;
    }
}

void owl_register_method(MethodReceiverType type, const char* name,
                         NativeFn fn, int arity) {
    if ((int)type < 0 || (int)type >= METHOD_TYPE_COUNT) return;
    int idx = method_counts[type];
    if (idx >= method_capacities[type]) {
        int newCap = method_capacities[type] == 0
                     ? INITIAL_CAP
                     : method_capacities[type] * 2;
        MethodEntry* nt = (MethodEntry*)realloc(method_tables[type],
                                                (size_t)newCap * sizeof(MethodEntry));
        if (!nt) return;  /* OOM — silently drop */
        method_tables[type]     = nt;
        method_capacities[type] = newCap;
    }
    method_tables[type][idx].name     = name;
    method_tables[type][idx].function = fn;
    method_tables[type][idx].arity    = arity;
    method_counts[type]++;
}

/* ================================================================
 * LOOKUP (case-insensitive + accent-insensitive)
 * ================================================================ */

NativeFn method_lookup(MethodReceiverType type, const char* name,
                       int nameLen, int* outArity) {
    /* Strip accents from the lookup name */
    char stripped[256];
    int slen = scanner_strip_accents(name, nameLen, stripped, sizeof(stripped));
    for (int i = 0; i < method_counts[type]; i++) {
        const MethodEntry* e = &method_tables[type][i];
        char estripped[256];
        int elen = scanner_strip_accents(e->name, (int)strlen(e->name),
                                         estripped, sizeof(estripped));
        if (slen != elen) continue;
        /* Case-insensitive compare on accent-stripped names */
        bool match = true;
        for (int j = 0; j < slen; j++) {
            if (tolower((unsigned char)stripped[j]) !=
                tolower((unsigned char)estripped[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            if (outArity) *outArity = e->arity;
            return e->function;
        }
    }
    return NULL;
}

/* ================================================================
 * RECEIVER TYPE DETECTION
 * ================================================================ */

int method_receiver_type(Value receiver) {
    if (IS_INTEGER(receiver)) return METHOD_TYPE_INTEGER;
    if (IS_REAL(receiver))    return METHOD_TYPE_REAL;
    if (!IS_OBJ(receiver))    return -1;

    switch (OBJ_TYPE(receiver)) {
        case OBJ_STRING:     return METHOD_TYPE_STRING;
        case OBJ_ARRAY:      return METHOD_TYPE_ARRAY;
        case OBJ_ASSOC_ARRAY: return METHOD_TYPE_ARRAY;  /* Filtre/Transforme partagent les methods array — fn dispatche sur IS_ASSOC_ARRAY interne. */
        case OBJ_JSON:       return METHOD_TYPE_JSON;
        case OBJ_COLLECTION: {
            ObjCollection* coll = AS_COLLECTION(receiver);
            switch (coll->collType) {
                case COLLECTION_QUEUE: return METHOD_TYPE_COLLECTION_QUEUE;
                case COLLECTION_STACK: return METHOD_TYPE_COLLECTION_STACK;
                case COLLECTION_LIST:  return METHOD_TYPE_COLLECTION_LIST;
            }
            return -1;
        }
        case OBJ_DATE:     return METHOD_TYPE_DATE;
        case OBJ_TIME:     return METHOD_TYPE_TIME;
        case OBJ_DATETIME: return METHOD_TYPE_DATETIME;
        case OBJ_DURATION: return METHOD_TYPE_DURATION;
        case OBJ_INSTANCE: {
            ObjInstance* inst = AS_INSTANCE(receiver);
            if (inst->structName != NULL) {
                if (strcmp(inst->structName->chars, "Chrono") == 0)
                    return METHOD_TYPE_CHRONO;
                if (strcmp(inst->structName->chars, "Timer") == 0)
                    return METHOD_TYPE_TIMER;
                if (strcmp(inst->structName->chars, "FichierDisque") == 0)
                    return METHOD_TYPE_FICHIERDISQUE;
                if (strcmp(inst->structName->chars, "xmlDocument") == 0)
                    return METHOD_TYPE_XML_DOCUMENT;
                if (strcmp(inst->structName->chars, "xmlNoeud") == 0)
                    return METHOD_TYPE_XML_NODE;
                if (strcmp(inst->structName->chars, "xmlAttribut") == 0)
                    return METHOD_TYPE_XML_ATTRIBUT;
                if (strcmp(inst->structName->chars, "xmlNamespace") == 0)
                    return METHOD_TYPE_XML_NAMESPACE;
                if (strcmp(inst->structName->chars, "sqlConnexion") == 0)
                    return METHOD_TYPE_SQL_CONNEXION;
                if (strcmp(inst->structName->chars, "ftpClient") == 0)
                    return METHOD_TYPE_FTP_CLIENT;
                if (strcmp(inst->structName->chars, "mqttSession") == 0)
                    return METHOD_TYPE_MQTT_SESSION;
                if (strcmp(inst->structName->chars, "amqpConnection") == 0)
                    return METHOD_TYPE_AMQP_CONNECTION;
                if (strcmp(inst->structName->chars, "stompSession") == 0)
                    return METHOD_TYPE_STOMP_SESSION;
                if (strcmp(inst->structName->chars, "natsConnexion") == 0)
                    return METHOD_TYPE_NATS_CONNECTION;
                if (strcmp(inst->structName->chars, "UUID") == 0)
                    return METHOD_TYPE_UUID;
                if (strcmp(inst->structName->chars, "CodeBarres") == 0
                    || strcmp(inst->structName->chars, "BarCodes") == 0)
                    return METHOD_TYPE_CODEBARRES;
                if (strcmp(inst->structName->chars, "AwsS3Client") == 0)
                    return METHOD_TYPE_AWS_S3_CLIENT;
                if (strcmp(inst->structName->chars, "AwsBedrockClient") == 0)
                    return METHOD_TYPE_AWS_BEDROCK_CLIENT;
                if (strcmp(inst->structName->chars, "AwsLambdaClient") == 0)
                    return METHOD_TYPE_AWS_LAMBDA_CLIENT;
            }
            return -1;
        }
        case OBJ_ZIP_ARCHIVE: return METHOD_TYPE_ZIP_ARCHIVE;
        case OBJ_VECTOR:      return METHOD_TYPE_VECTOR;
        case OBJ_MATRIX:      return METHOD_TYPE_MATRIX;
        case OBJ_TENSOR:      return METHOD_TYPE_TENSOR;
        case OBJ_IAMODELE:    return METHOD_TYPE_IAMODELE;
        case OBJ_MLARBRE:     return METHOD_TYPE_MLARBRE;
        case OBJ_MLFORET:     return METHOD_TYPE_MLFORET;
        case OBJ_MLBOOSTING:  return METHOD_TYPE_MLBOOSTING;
        case OBJ_MLLINEAIRE:  return METHOD_TYPE_MLLINEAIRE;
        case OBJ_MLLOGISTIQUE: return METHOD_TYPE_MLLOGISTIQUE;
        case OBJ_MLKMEANS:    return METHOD_TYPE_MLKMEANS;
        case OBJ_LLM:         return METHOD_TYPE_LLM;
        case OBJ_DATASET:     return METHOD_TYPE_DATASET;
        case OBJ_BUFFER:      return METHOD_TYPE_BUFFER;
        case OBJ_XLS_DOCUMENT: return METHOD_TYPE_XLS_DOCUMENT;
        case OBJ_MOCK_NATIVE: return METHOD_TYPE_MOCK_NATIVE;
        case OBJ_UI_CONTROL: {
            ObjUiControl* uic = AS_UI_CONTROL(receiver);
            if ((OwlControlKind)uic->kind == OWL_CONTROL_LISTE)
                return METHOD_TYPE_UI_LISTE;
            if ((OwlControlKind)uic->kind == OWL_CONTROL_COMBO)
                return METHOD_TYPE_UI_COMBO;
            if ((OwlControlKind)uic->kind == OWL_CONTROL_SELECTEUR)
                return METHOD_TYPE_UI_SELECTEUR;
            if ((OwlControlKind)uic->kind == OWL_CONTROL_INTERRUPTEUR)
                return METHOD_TYPE_UI_INTERRUPTEUR;
            if ((OwlControlKind)uic->kind == OWL_CONTROL_ZONE_REPETEE)
                return METHOD_TYPE_UI_ZONE_REPETEE;
            if ((OwlControlKind)uic->kind == OWL_CONTROL_TABLE)
                return METHOD_TYPE_UI_TABLE;
            return -1;
        }
        /*  — dispatch methode sur les proxies / instances de sous-controle.
         * Le kind est tire du cache ObjUiControlRef.kind (pose au compile-time par
         * obj_new_ui_control_ref) ou du decl pointe par l'instance. Cas typique :
         *   FEN_X.CMB_Test.Ouvre()  -> receiver = ObjUiControlRef (proxy global)
         * Sans ces cases, method_receiver_type renvoie -1 et OP_INVOKE tombe sur
         * E_RT_NO_METHOD_FOR_TYPE alors que l'utilisateur a clairement un widget. */
        case OBJ_UI_CONTROL_REF: {
            ObjUiControlRef* ref = AS_UI_CONTROL_REF(receiver);
            switch ((OwlControlKind)ref->kind) {
                case OWL_CONTROL_LISTE:        return METHOD_TYPE_UI_LISTE;
                case OWL_CONTROL_COMBO:        return METHOD_TYPE_UI_COMBO;
                case OWL_CONTROL_SELECTEUR:    return METHOD_TYPE_UI_SELECTEUR;
                case OWL_CONTROL_INTERRUPTEUR: return METHOD_TYPE_UI_INTERRUPTEUR;
                case OWL_CONTROL_ZONE_REPETEE: return METHOD_TYPE_UI_ZONE_REPETEE;
                case OWL_CONTROL_TABLE:        return METHOD_TYPE_UI_TABLE;
                default: return -1;
            }
        }
        case OBJ_UI_CONTROL_INSTANCE: {
            ObjUiControlInstance* uici = AS_UI_CONTROL_INSTANCE(receiver);
            if (uici->decl == NULL) return -1;
            switch ((OwlControlKind)uici->decl->kind) {
                case OWL_CONTROL_LISTE:        return METHOD_TYPE_UI_LISTE;
                case OWL_CONTROL_COMBO:        return METHOD_TYPE_UI_COMBO;
                case OWL_CONTROL_SELECTEUR:    return METHOD_TYPE_UI_SELECTEUR;
                case OWL_CONTROL_INTERRUPTEUR: return METHOD_TYPE_UI_INTERRUPTEUR;
                case OWL_CONTROL_ZONE_REPETEE: return METHOD_TYPE_UI_ZONE_REPETEE;
                case OWL_CONTROL_TABLE:        return METHOD_TYPE_UI_TABLE;
                default: return -1;
            }
        }
        default: return -1;
    }
}

/* ================================================================
 * METHOD RECEIVER METADATA — source unique de verite (cf owl_methods.h)
 * ================================================================ */

static const MethodReceiverInfo METHOD_RECEIVER_INFO[METHOD_TYPE_COUNT] = {
    [METHOD_TYPE_STRING]            = {"chaine",            "string"},
    [METHOD_TYPE_INTEGER]           = {"entier",            "integer"},
    [METHOD_TYPE_REAL]              = {"reel",              "real"},
    [METHOD_TYPE_ARRAY]             = {"tableau",           "array"},
    [METHOD_TYPE_JSON]              = {"json",              NULL},
    [METHOD_TYPE_COLLECTION_QUEUE]  = {"file",              "queue"},
    [METHOD_TYPE_COLLECTION_STACK]  = {"pile",              "stack"},
    [METHOD_TYPE_COLLECTION_LIST]   = {"liste",             "list"},
    [METHOD_TYPE_DATE]              = {"date",              NULL},
    [METHOD_TYPE_TIME]              = {"heure",             "time"},
    [METHOD_TYPE_DATETIME]          = {"dateheure",         "datetime"},
    [METHOD_TYPE_DURATION]          = {"duree",             "duration"},
    [METHOD_TYPE_CHRONO]            = {"chrono",            NULL},
    [METHOD_TYPE_TIMER]             = {"Timer",             NULL},
    [METHOD_TYPE_FICHIERDISQUE]     = {"FichierDisque",     "diskfile"},
    [METHOD_TYPE_MOCK_NATIVE]       = {"MockNative",        NULL},
    [METHOD_TYPE_XML_DOCUMENT]      = {"xmlDocument",       NULL},
    [METHOD_TYPE_XML_NODE]          = {"xmlNoeud",          NULL},
    [METHOD_TYPE_XML_ATTRIBUT]      = {"xmlAttribut",       NULL},
    [METHOD_TYPE_XML_NAMESPACE]     = {"xmlNamespace",      NULL},
    [METHOD_TYPE_UI_LISTE]          = {"ChampListe",        NULL},
    [METHOD_TYPE_UI_COMBO]          = {"ChampCombo",        NULL},
    [METHOD_TYPE_UI_SELECTEUR]      = {"ChampSelecteur",    NULL},
    [METHOD_TYPE_UI_INTERRUPTEUR]   = {"ChampInterrupteur", NULL},
    [METHOD_TYPE_UI_ZONE_REPETEE]   = {"ChampZoneRepetee",  NULL},
    [METHOD_TYPE_UI_TABLE]          = {"ChampTable",        NULL},
    [METHOD_TYPE_SQL_CONNEXION]     = {"sqlConnexion",      "sqlconnection"},
    [METHOD_TYPE_UUID]              = {"UUID",              NULL},
    [METHOD_TYPE_ZIP_ARCHIVE]       = {"zipArchive",        NULL},
    [METHOD_TYPE_BUFFER]            = {"buffer",            NULL},
    [METHOD_TYPE_CODEBARRES]        = {"CodeBarres",        "BarCodes"},
    [METHOD_TYPE_VECTOR]            = {"Vecteur",           "Vector"},
    [METHOD_TYPE_MATRIX]            = {"Matrice",           "Matrix"},
    [METHOD_TYPE_TENSOR]            = {"Tenseur",           "Tensor"},
    [METHOD_TYPE_IAMODELE]          = {"IAModele",          "AIModel"},
    [METHOD_TYPE_LLM]               = {"LLM",               "LLM"},
    [METHOD_TYPE_DATASET]           = {"Dataset",           "Dataset"},
    [METHOD_TYPE_AWS_S3_CLIENT]     = {"AwsS3Client",       "AwsS3Client"},
    [METHOD_TYPE_AWS_BEDROCK_CLIENT]= {"AwsBedrockClient",  "AwsBedrockClient"},
    [METHOD_TYPE_AWS_LAMBDA_CLIENT] = {"AwsLambdaClient",   "AwsLambdaClient"},
    [METHOD_TYPE_XLS_DOCUMENT]      = {"xlsDocument",       NULL},
    [METHOD_TYPE_FTP_CLIENT]        = {"ftpClient",         NULL},
    [METHOD_TYPE_MQTT_SESSION]      = {"mqttSession",       NULL},
    [METHOD_TYPE_AMQP_CONNECTION]   = {"amqpConnection",    NULL},
    [METHOD_TYPE_STOMP_SESSION]     = {"stompSession",      NULL},
    [METHOD_TYPE_NATS_CONNECTION]   = {"natsConnexion",     "natsConnection"},
    [METHOD_TYPE_MLARBRE]           = {"MLArbreDecision",   "MLDecisionTree"},
    [METHOD_TYPE_MLFORET]           = {"MLForetAleatoire",  "MLRandomForest"},
    [METHOD_TYPE_MLBOOSTING]        = {"MLGradientBoosting", "MLGradientBoosting"},
    [METHOD_TYPE_MLLINEAIRE]        = {"MLRegressionLineaire",   "MLLinearRegression"},
    [METHOD_TYPE_MLLOGISTIQUE]      = {"MLRegressionLogistique", "MLLogisticRegression"},
    [METHOD_TYPE_MLKMEANS]          = {"MLKMeans",           "MLKMeans"},
};

const MethodReceiverInfo* method_receiver_info(MethodReceiverType t) {
    if ((int)t < 0 || (int)t >= METHOD_TYPE_COUNT) return NULL;
    return &METHOD_RECEIVER_INFO[t];
}

/* ================================================================
 * CATALOG ENUMERATION (for dump_lsp_catalog)
 * ================================================================ */

int method_catalog_count(void) {
    int total = 0;
    for (int t = 0; t < METHOD_TYPE_COUNT; t++) total += method_counts[t];
    return total;
}

const MethodEntry* method_catalog_at(int i, const char** outTypeName) {
    for (int t = 0; t < METHOD_TYPE_COUNT; t++) {
        if (i < method_counts[t]) {
            if (outTypeName) *outTypeName = METHOD_RECEIVER_INFO[t].canonical;
            return &method_tables[t][i];
        }
        i -= method_counts[t];
    }
    return NULL;
}
