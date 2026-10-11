// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* GÉNÉRÉ — NE PAS ÉDITER À LA MAIN.
 * Produit par tools/aws/aws_models_gen.owl depuis les modèles botocore.
 * Régénération : scripts/refresh_aws_models.sh
 */
#ifndef OWL_AWS_SERVICE_MODELS_H
#define OWL_AWS_SERVICE_MODELS_H

#include "owl_registry.h"

typedef enum {
    AWS_PROTO_UNKNOWN = 0,
    AWS_PROTO_JSON,
    AWS_PROTO_REST_JSON,
    AWS_PROTO_REST_XML,
    AWS_PROTO_QUERY,
    AWS_PROTO_EC2
} AwsProtocol;

typedef struct {
    const char* actionName;
    const char* method;
    const char* requestUri;
} AwsAction;

typedef struct {
    const char* serviceName;
    const char* symbolName;
    const char* endpointPrefix;
    AwsProtocol protocol;
    const char* jsonVersion;
    const char* targetPrefix;
    const char* signingName;
    const char* apiVersion;
    const char* signatureVersion;
    const char* globalEndpoint;
    const char* signingRegion;
    const AwsAction* actions;
    int actionCount;
} AwsService;

extern const AwsService AWS_SERVICES[];
extern const int AWS_SERVICE_COUNT;

/* Descripteur de serialisation JSON->XML d'un corps de requete rest-xml.
 * kind : 0=scalaire, 1=structure, 2=liste. Pour une structure, children
 * pointe le tableau de ses membres. Pour une liste, xmlName est l'element
 * conteneur (ou l'element repete si flattened), listItemName l'element de
 * chaque item, itemKind le type des items (0=scalaire, 1=structure) et
 * children les membres de l'item quand itemKind=1. scalarFmt precise le
 * format d'un scalaire de corps (kind=0) a l'ecriture : 0=texte brut,
 * 1=blob (encode base64). Ignore hors chemin d'ecriture. */
typedef struct AwsXmlMember {
    const char* jsonName;
    const char* xmlName;
    int kind;
    const struct AwsXmlMember* children;
    int childCount;
    const char* listItemName;
    int flattened;
    int itemKind;
    int scalarFmt;
} AwsXmlMember;

typedef struct {
    const char* serviceName;
    const char* actionName;
    const char* rootName;
    const char* xmlNamespace;
    const char* payloadName;    /* cle JSON du membre payload dans laquelle descendre, "" = top-level */
    const AwsXmlMember* members;
    int memberCount;
} AwsXmlSerDescr;

extern const AwsXmlSerDescr AWS_SER_DESCRS[];
extern const int AWS_SER_DESCR_COUNT;

/* Descripteur de decodage XML->JSON d'une reponse (rest-xml/query/ec2).
 * members decrit les enfants attendus de l'element racine du fil : les
 * membres marques liste (AwsXmlMember.kind=2) decodent en tableau
 * deterministe (forme boto3 hoist), les structures guident la descente.
 * Seules les operations dont la reponse porte au moins une liste ont un
 * descripteur ; les autres decodent generiquement. */
typedef struct {
    const char* serviceName;
    const char* actionName;
    const AwsXmlMember* members;
    int memberCount;
} AwsXmlOutDescr;

extern const AwsXmlOutDescr AWS_OUT_DESCRS[];
extern const int AWS_OUT_DESCR_COUNT;

/* Enum awsService + constantes de service (valeur = index dans AWS_SERVICES). */
extern const char* const E_awsService_values[];
extern const OwlConstDef AWS_CONSTS[];
extern const int AWS_CONST_COUNT;

#endif
