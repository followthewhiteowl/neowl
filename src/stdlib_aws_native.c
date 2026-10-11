// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_aws_native.c — surface OWL de la famille AWS.
 *
 * Couche VM-aware au-dessus de deux briques pures :
 *   - le signeur SigV4 (stdlib_aws.c) : crypto mbedTLS, aucune dependance VM ;
 *   - la table de modeles de service (aws_service_models.c) : donnee generee.
 *
 * Expose la native AWSRequête et l'enum awsService. AWSRequête construit un
 * httpRequête, le signe (AWS4-HMAC-SHA256), puis delegue l'envoi reseau a
 * HTTPEnvoie (mockable) et restitue la reponse decodee en JSON.
 *
 * Le cadrage protocole (json / query / rest-json / ec2) est derive de la table
 * de modeles : Content-Type, X-Amz-Target, methode + URI, endpoint par defaut.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "owl_methods.h"
#include "vm.h"
#include "object.h"
#include "memory.h"

#include "sentinels.h"
#include "stdlib_aws.h"
#include "aws_service_models.h"

#include "mbedtls/sha256.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

extern InterpretResult vm_call_reentrant(Value callee, int argCount);
extern ObjJson* json_parse_cstr(const char* src, int len);

/* ================================================================
 * Enum awsService — valeur = index dans AWS_SERVICES (local au build).
 * ================================================================ */

/* Valeurs (E_awsService_values) et constantes (AWS_CONSTS) generees depuis la
 * table de modeles — cf. aws_service_models.{c,h}, produit par
 * tools/aws/aws_models_gen.owl. L'enum pointe le tableau genere : ajouter un
 * service = regenerer, sans toucher ce fichier. Valeur = index dans
 * AWS_SERVICES (petits entiers 0..N ; sert de sous-script direct de la table
 * au runtime). Zone sentinel volontairement non appliquee (force_small_int). */
const OwlEnum E_awsService = {
    .name="awsService", .nameAscii="awsService", .nameEN="awsService", .category="AWS",
    .descFR="Services AWS ciblables par AWSRequête.",
    .descEN="AWS services targetable by AWSRequest.",
    .values=E_awsService_values, .combinable=false,
    .force_small_int=true
};

static const OwlEnum* const AWS_ENUMS[] = { &E_awsService };

/* ================================================================
 * Helpers
 * ================================================================ */

/* Lecture d'un champ chaine d'une instance (accent/casse via storage ASCII).
 * Le pointeur retourne vit dans inst->fields ; valide tant que inst est root
 * (ici toujours : c'est un argument de la native, donc sur la pile VM). */
static const char* aws_inst_str(ObjInstance* inst, const char* name) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    Value v;
    if (table_get(&inst->fields, k, &v) && IS_STRING(v)) return AS_STRING(v)->chars;
    return "";
}

/* hex(SHA-256(data)) dans out (>= 65). Duplique la primitive du signeur : le
 * signeur ne publie pas le hash avant de construire la canonical request, or
 * on en a besoin AVANT pour poser x-amz-content-sha256 en en-tete signe. */
static void aws_sha256_hex(const unsigned char* data, size_t len, char out[65]) {
    unsigned char digest[32];
    const unsigned char empty = 0;
    if (data == NULL) data = &empty;
    mbedtls_sha256(data, len, digest, 0);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[(digest[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out[64] = '\0';
}

/* Extrait "host[:port]" d'une URL d'endpoint (retire le schema et le chemin). */
static void aws_extract_host(const char* url, char* out, size_t cap) {
    const char* p = url;
    const char* scheme = strstr(url, "://");
    if (scheme) p = scheme + 3;
    size_t n = 0;
    while (p[n] && p[n] != '/' && n + 1 < cap) { out[n] = p[n]; n++; }
    out[n] = '\0';
}

/* Serialise une valeur JSON en chaine via la native JSONVersChaîne (reuse du
 * serialiseur canon). Retourne un buffer heap-owned (a free par l'appelant) et
 * pose *outLen. Effet net nul sur la pile VM. */
static char* aws_dup(const char* s, size_t* outLen) {
    size_t n = strlen(s);
    char* b = (char*)malloc(n + 1);
    memcpy(b, s, n + 1);
    if (outLen) *outLen = n;
    return b;
}
static char* aws_serialize_json(Value jsonVal, size_t* outLen) {
    ObjString* key = obj_copy_string("JSONVersChaine", 14);
    vm_push(OBJ_VAL(key));
    Value fn;
    bool ok = table_get(&vm.globals, key, &fn);
    vm_pop();
    if (!ok) return aws_dup("{}", outLen);

    vm_push(fn);
    vm_push(jsonVal);
    if (vm_call_reentrant(fn, 1) != INTERPRET_OK) return aws_dup("{}", outLen);
    Value res = vm_pop();
    if (!IS_STRING(res)) return aws_dup("{}", outLen);

    ObjString* s = AS_STRING(res);
    char* b = (char*)malloc((size_t)s->length + 1);
    memcpy(b, s->chars, (size_t)s->length);
    b[s->length] = '\0';
    if (outLen) *outLen = (size_t)s->length;
    return b;
}

/* Clone du modele httpRequête (vm.types), incluant le clone de l'assoc Entête.
 * Miroir de http clone_template — garde ce module independant de stdlib_http. */
static ObjInstance* aws_clone_httprequete(void) {
    ObjString* k = obj_copy_string("httpRequete", 11);
    Value v;
    if (!table_get(&vm.types, k, &v) || !IS_INSTANCE(v)) return NULL;
    ObjInstance* tpl = AS_INSTANCE(v);
    ObjInstance* out = obj_new_instance(tpl->structName);
    vm_push(OBJ_VAL(out));
    for (int i = 0; i < tpl->fields.capacity; i++) {
        Entry* e = &tpl->fields.entries[i];
        if (e->key == NULL) continue;
        Value fv = e->value;
        if (IS_ASSOC_ARRAY(fv)) fv = OBJ_VAL(obj_assoc_clone(AS_ASSOC_ARRAY(fv)));
        obj_inst_set_kv(out, e->key, fv);
    }
    vm_pop();
    return out;
}

/* Pose un en-tete "name: value" dans l'assoc Entête d'une requete (rooté). */
static void aws_header_set(ObjAssocArray* entete, const char* name, const char* value) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    ObjString* v = obj_copy_string(value, (int)strlen(value));
    obj_assoc_set(entete, k, OBJ_VAL(v));
    vm_pop();
}

/* Défini plus bas (partagé avec le cadrage S3/Bedrock). */
static void aws_uri_encode_segment(const char* s, char* out, size_t cap);
static Value aws_decode_json_response(Value rep);
static Value aws_decode_xml_response(Value rep, const AwsXmlOutDescr* descr);

/* Sérialisation JSON -> XML des corps rest-xml (miroir de aws_xml_node_to_json).
 * Descripteur = table générée (aws_service_models.h). Définis plus bas. */
static const AwsXmlSerDescr* aws_find_ser_descr(const char* service, const char* action);
static char* aws_json_to_xml(Value corps, const AwsXmlSerDescr* descr, size_t* outLen);

/* Décodage XML -> JSON des réponses (miroir côté lecture des descripteurs de
 * sérialisation). Descripteur de sortie shape-driven pour lever l'ambiguïté des
 * listes (0/1/N éléments -> tableau déterministe). Défini plus bas. */
static const AwsXmlOutDescr* aws_find_out_descr(const char* service, const char* action);

/* Comparaison ASCII casse-insensible (noms de placeholder / clés JSON, tous
 * ASCII côté botocore). Cohérente avec le lookup casse-insensible des membres
 * JSON (obj_json_object_get). */
static bool aws_ascii_ci_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return false;
    }
    return *a == *b;
}

/* Substitue les placeholders {nom} de `uri` par la valeur du champ homonyme du
 * corps JSON (URI-encodée via aws_uri_encode_segment). Écrit l'URI résolue dans
 * `out` (bornée par cap) et collecte les noms de placeholder consommés dans
 * `consumed` (pour les retirer ensuite du corps sérialisé). Retourne false et
 * pose une erreur native si un placeholder requis n'a pas de valeur scalaire
 * dans le corps. Une URI sans `{` est recopiée telle quelle. */
static bool aws_resolve_path(const char* uri, Value corps, char* out, size_t cap,
                             char consumed[][128], int consumedMax, int* consumedCount) {
    *consumedCount = 0;
    ObjJson* src = (IS_JSON(corps) && AS_JSON(corps)->jtype == JSON_T_OBJECT)
                       ? AS_JSON(corps) : NULL;
    size_t o = 0;
    const char* p = uri;
    while (*p && o + 1 < cap) {
        if (*p != '{') { out[o++] = *p++; continue; }
        const char* end = strchr(p, '}');
        if (end == NULL) { out[o++] = *p++; continue; }  /* accolade non fermée : littérale */

        char name[128];
        size_t nlen = (size_t)(end - p - 1);
        if (nlen >= sizeof name) nlen = sizeof name - 1;
        memcpy(name, p + 1, nlen);
        name[nlen] = '\0';

        const char* scalar = NULL;
        char numBuf[32];
        if (src) {
            ObjString* k = obj_copy_string(name, (int)strlen(name));
            ObjJson* v = obj_json_object_get(src, k);
            if (v != NULL) {
                if (v->jtype == JSON_T_STRING) {
                    scalar = v->as.strVal->chars;
                } else if (v->jtype == JSON_T_INT) {
                    snprintf(numBuf, sizeof numBuf, "%lld", (long long)v->as.intVal);
                    scalar = numBuf;
                }
            }
        }
        if (scalar == NULL) {
            owl_emit_native_error(E_NATIVE_ARG_MISSING, "AWSRequete", "arg", name, NULL);
            return false;
        }

        char enc[512];
        aws_uri_encode_segment(scalar, enc, sizeof enc);
        for (const char* e = enc; *e && o + 1 < cap; e++) out[o++] = *e;
        if (*consumedCount < consumedMax) {
            snprintf(consumed[*consumedCount], 128, "%s", name);
            (*consumedCount)++;
        }
        p = end + 1;
    }
    out[o] = '\0';
    return true;
}

/* Un parametre de query canonicalisee : nom et valeur deja URI-encodes. */
typedef struct { char name[256]; char value[512]; } AwsQueryParam;

static int aws_query_param_cmp(const void* a, const void* b) {
    return strcmp(((const AwsQueryParam*)a)->name, ((const AwsQueryParam*)b)->name);
}

/* Canonicalise une query SigV4 (partie apres le '?' d'une requestUri rest-*).
 * Decoupe sur '&', separe nom/valeur sur le premier '=' (pas de '=' -> valeur
 * vide), URI-encode nom ET valeur en RFC3986 via aws_uri_encode_segment ('/'
 * non exempte, espace -> %20), TRIE par nom (ordre d'octets), puis rejoint par
 * '&' sous forme `nom=valeur` (valeur vide -> `nom=`). Query vide -> out vide. */
static void aws_canonicalize_query(const char* raw, char* out, size_t cap) {
    AwsQueryParam params[32];
    int count = 0;
    const char* p = raw;
    while (*p && count < 32) {
        const char* amp    = strchr(p, '&');
        size_t      seglen = amp ? (size_t)(amp - p) : strlen(p);
        const char* eq     = (const char*)memchr(p, '=', seglen);

        char rawName[256], rawVal[512];
        size_t nlen = eq ? (size_t)(eq - p) : seglen;
        if (nlen >= sizeof rawName) nlen = sizeof rawName - 1;
        memcpy(rawName, p, nlen);
        rawName[nlen] = '\0';
        if (eq) {
            size_t vlen = seglen - (size_t)(eq - p) - 1;
            if (vlen >= sizeof rawVal) vlen = sizeof rawVal - 1;
            memcpy(rawVal, eq + 1, vlen);
            rawVal[vlen] = '\0';
        } else {
            rawVal[0] = '\0';
        }

        aws_uri_encode_segment(rawName, params[count].name,  sizeof params[count].name);
        aws_uri_encode_segment(rawVal,  params[count].value, sizeof params[count].value);
        count++;

        if (!amp) break;
        p = amp + 1;
    }

    qsort(params, (size_t)count, sizeof params[0], aws_query_param_cmp);

    size_t o = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0 && o + 1 < cap) out[o++] = '&';
        for (const char* c = params[i].name;  *c && o + 1 < cap; c++) out[o++] = *c;
        if (o + 1 < cap) out[o++] = '=';
        for (const char* c = params[i].value; *c && o + 1 < cap; c++) out[o++] = *c;
    }
    out[o] = '\0';
}

/* Buffer heap croissant pour le corps form-urlencoded des protocoles query/ec2 :
 * le nombre de paramètres (listes, structures imbriquées) est arbitraire, donc
 * pas de borne fixe. Alloue/agrandit *buf, maintient *len et *cap. */
static void aws_qbuf_append(char** buf, size_t* len, size_t* cap, const char* s) {
    size_t sl = strlen(s);
    if (*len + sl + 1 > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (*len + sl + 1 > nc) nc *= 2;
        *buf = (char*)realloc(*buf, nc);
        *cap = nc;
    }
    memcpy(*buf + *len, s, sl);
    *len += sl;
    (*buf)[*len] = '\0';
}

/* Émet une paire "&<prefix>=<valeur URI-encodée>" dans le corps query. */
static void aws_qbuf_pair(char** buf, size_t* len, size_t* cap,
                          const char* prefix, const char* rawVal) {
    char enc[1024];
    aws_uri_encode_segment(rawVal, enc, sizeof enc);
    aws_qbuf_append(buf, len, cap, "&");
    aws_qbuf_append(buf, len, cap, prefix);
    aws_qbuf_append(buf, len, cap, "=");
    aws_qbuf_append(buf, len, cap, enc);
}

/* Aplatit récursivement un noeud JSON en paires form-urlencoded AWS, sous un
 * préfixe de chemin. Règles :
 *   - scalaire (string/int/bool/réel) -> "&<prefix>=<valeur>" ;
 *   - objet {k:v} -> récursion préfixe "<prefix>.<k>" (racine : "<k>" sans point
 *     de tête, prefix vide) ;
 *   - liste [e1,e2] (1-indexée) -> query : préfixe "<prefix>.member.<i>" ;
 *                                  ec2   : préfixe "<prefix>.<i>" (sans .member) ;
 *   - null : ignoré (aucune paire émise).
 * L'ordre du corps est libre (signé via le hash de payload, pas la canonical
 * query), on émet donc dans l'ordre d'itération du JSON. */
static void aws_query_flatten(ObjJson* node, const char* prefix, int protocol,
                              char** buf, size_t* len, size_t* cap) {
    if (node == NULL) return;
    switch (node->jtype) {
        case JSON_T_NULL:
            return;
        case JSON_T_STRING:
            aws_qbuf_pair(buf, len, cap, prefix, node->as.strVal->chars);
            return;
        case JSON_T_INT: {
            char num[32];
            snprintf(num, sizeof num, "%lld", (long long)node->as.intVal);
            aws_qbuf_pair(buf, len, cap, prefix, num);
            return;
        }
        case JSON_T_BOOL:
            aws_qbuf_pair(buf, len, cap, prefix, node->as.boolVal ? "true" : "false");
            return;
        case JSON_T_REAL: {
            char num[64];
            snprintf(num, sizeof num, "%.17g", node->as.realVal);
            aws_qbuf_pair(buf, len, cap, prefix, num);
            return;
        }
        case JSON_T_OBJECT:
            for (int i = 0; i < node->as.objectVal.count; i++) {
                char child[512];
                const char* k = node->as.objectVal.keys[i]->chars;
                if (prefix[0]) snprintf(child, sizeof child, "%s.%s", prefix, k);
                else           snprintf(child, sizeof child, "%s", k);
                aws_query_flatten(node->as.objectVal.values[i], child, protocol,
                                  buf, len, cap);
            }
            return;
        case JSON_T_ARRAY:
            for (int i = 0; i < node->as.arrayVal.count; i++) {
                char child[512];
                if (protocol == AWS_PROTO_EC2)
                    snprintf(child, sizeof child, "%s.%d", prefix, i + 1);
                else
                    snprintf(child, sizeof child, "%s.member.%d", prefix, i + 1);
                aws_query_flatten(node->as.arrayVal.items[i], child, protocol,
                                  buf, len, cap);
            }
            return;
        default:
            /* CURRENCY / NUMERIC : types monétaires/haute précision, hors input
             * query attendu (les nombres d'un corps query arrivent en INT/REAL).
             * Ignorés plutôt qu'émis dans une forme non spécifiée. */
            return;
    }
}

/* ================================================================
 * AWSRequête(session, service, action, corps)
 * ================================================================ */

static Value fn_aws_requete(int argCount, Value* args) {
    if (argCount != 4) return NIL_VAL;
    if (!IS_INSTANCE(args[0]) || !IS_INTEGER(args[1]) || !IS_STRING(args[2])) return NIL_VAL;

    ObjInstance* sess   = AS_INSTANCE(args[0]);
    int64_t      svcIdx = AS_INTEGER(args[1]);
    if (svcIdx < 0 || svcIdx >= AWS_SERVICE_COUNT) return NIL_VAL;
    const AwsService* svc = &AWS_SERVICES[svcIdx];
    const char* action = AS_STRING(args[2])->chars;

    const char* region    = aws_inst_str(sess, "Region");
    const char* accessKey = aws_inst_str(sess, "CleAcces");
    const char* secretKey = aws_inst_str(sess, "CleSecrete");
    const char* token     = aws_inst_str(sess, "Jeton");
    const char* endpoint  = aws_inst_str(sess, "Endpoint");

    /* --- Endpoint / host --- */
    char host[512];
    char urlBuf[1024];
    const char* method     = "POST";
    const char* requestUri = "/";

    /* Placeholders de chemin {nom} consommés depuis le corps (rest-*). */
    char uriBuf[1024];
    char consumed[16][128];
    int  consumedCount = 0;

    /* rest-* : methode + URI portees par l'action. */
    if (svc->protocol == AWS_PROTO_REST_JSON || svc->protocol == AWS_PROTO_REST_XML) {
        for (int a = 0; a < svc->actionCount; a++) {
            if (strcmp(svc->actions[a].actionName, action) == 0) {
                if (svc->actions[a].method[0])     method     = svc->actions[a].method;
                if (svc->actions[a].requestUri[0]) requestUri = svc->actions[a].requestUri;
                break;
            }
        }
        /* Substitution des placeholders {nom} par corps.nom (URI-encodé) ;
         * champ consommé retiré du corps sérialisé plus bas. */
        if (!aws_resolve_path(requestUri, args[3], uriBuf, sizeof uriBuf,
                              consumed, 16, &consumedCount)) {
            return NIL_VAL;  /* placeholder requis sans valeur : erreur déjà posée */
        }
        requestUri = uriBuf;
    }

    /* Services globaux (partition aws) : hote fixe sans segment de region, et
     * signature dans la region portee par les donnees (credentialScope, en
     * pratique us-east-1). L'override endpoint de session garde priorite.
     * Un global sans region de signature resolue retombe sur la region de
     * session (cas degenere de endpoints.json sans credentialScope). */
    bool isGlobal = svc->globalEndpoint[0] != '\0';
    const char* signRegion = (isGlobal && svc->signingRegion[0]) ? svc->signingRegion : region;

    if (endpoint[0] != '\0') {
        aws_extract_host(endpoint, host, sizeof host);
        snprintf(urlBuf, sizeof urlBuf, "%s%s", endpoint,
                 (strcmp(requestUri, "/") == 0) ? "" : requestUri);
    } else if (isGlobal) {
        snprintf(host, sizeof host, "%s", svc->globalEndpoint);
        snprintf(urlBuf, sizeof urlBuf, "https://%s%s", host, requestUri);
    } else {
        snprintf(host, sizeof host, "%s.%s.amazonaws.com", svc->endpointPrefix, region);
        snprintf(urlBuf, sizeof urlBuf, "https://%s%s", host, requestUri);
    }

    /* --- Corps + Content-Type + cible selon protocole --- */
    char*  body = NULL;
    size_t bodyLen = 0;
    char   contentType[128];
    char   target[256];
    target[0] = '\0';

    switch (svc->protocol) {
        case AWS_PROTO_JSON:
            body = aws_serialize_json(args[3], &bodyLen);
            snprintf(contentType, sizeof contentType, "application/x-amz-json-%s",
                     svc->jsonVersion[0] ? svc->jsonVersion : "1.0");
            snprintf(target, sizeof target, "%s.%s", svc->targetPrefix, action);
            break;
        case AWS_PROTO_QUERY:
        case AWS_PROTO_EC2: {
            /* Protocoles de type query : corps form-urlencoded débutant par
             * Action=...&Version=..., puis les champs du corps JSON aplatis en
             * paires &<chemin>=<valeur> (récursif, encodage RFC3986). EC2 partage
             * ce cadrage, avec une convention de liste distincte (sans .member).
             * Corps vide/nil : aucune paire ajoutée (Action/Version seuls). */
            char header[256];
            snprintf(header, sizeof header, "Action=%s&Version=%s", action, svc->apiVersion);
            size_t qcap = 0;
            body = NULL;
            bodyLen = 0;
            aws_qbuf_append(&body, &bodyLen, &qcap, header);
            if (IS_JSON(args[3]) && AS_JSON(args[3])->jtype == JSON_T_OBJECT) {
                aws_query_flatten(AS_JSON(args[3]), "", (int)svc->protocol,
                                  &body, &bodyLen, &qcap);
            }
            snprintf(contentType, sizeof contentType, "application/x-www-form-urlencoded");
            break;
        }
        case AWS_PROTO_REST_JSON: {
            /* Methode + URI portees par l'action (resolues plus haut) ; corps JSON.
             * Les champs consommes par les placeholders de chemin sont retires du
             * corps serialise (copie superficielle filtree, source non mutee). */
            Value bodyVal = args[3];
            bool filteredPushed = false;
            if (consumedCount > 0 && IS_JSON(args[3]) &&
                AS_JSON(args[3])->jtype == JSON_T_OBJECT) {
                ObjJson* srcJ = AS_JSON(args[3]);
                ObjJson* filtered = obj_new_json(JSON_T_OBJECT);
                vm_push(OBJ_VAL(filtered));  /* root pendant la construction */
                filteredPushed = true;
                for (int i = 0; i < srcJ->as.objectVal.count; i++) {
                    ObjString* key = srcJ->as.objectVal.keys[i];
                    bool skip = false;
                    for (int c = 0; c < consumedCount; c++) {
                        if (aws_ascii_ci_eq(key->chars, consumed[c])) { skip = true; break; }
                    }
                    if (!skip)
                        obj_json_object_set(filtered, key, srcJ->as.objectVal.values[i]);
                }
                bodyVal = OBJ_VAL(filtered);
            }
            body = aws_serialize_json(bodyVal, &bodyLen);
            if (filteredPushed) vm_pop();
            snprintf(contentType, sizeof contentType, "application/json");
            break;
        }
        case AWS_PROTO_REST_XML: {
            /* Methode + URI portees par l'action (resolues plus haut). Ecriture :
             * si l'action porte un descripteur de serialisation (input shape a
             * corps, genere depuis les modeles), on serialise le corps JSON en
             * XML shape-driven. Sinon (lecture, pas de corps) : corps vide. Dans
             * les deux cas x-amz-content-sha256 est calcule sur le corps reel
             * plus bas. La reponse XML est decodee en JSON generique. */
            const AwsXmlSerDescr* d = aws_find_ser_descr(svc->serviceName, action);
            body = (d != NULL) ? aws_json_to_xml(args[3], d, &bodyLen) : NULL;
            if (body == NULL) body = aws_dup("", &bodyLen);
            snprintf(contentType, sizeof contentType, "application/xml");
            break;
        }
        default:
            /* AWS_PROTO_UNKNOWN ou protocole non gere : pas de cadrage
             * silencieux hasardeux, on remonte une erreur claire. Inatteignable
             * pour les services livres (json/query/ec2/rest-json/rest-xml). */
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "AWSRequete", NULL);
            return NIL_VAL;
    }

    /* --- Horodatage UTC : AAAAMMJJTHHMMSSZ + AAAAMMJJ --- */
    char amzDate[32], dateStamp[16];
    {
        time_t now = time(NULL);
        struct tm g;
#if defined(_WIN32)
        gmtime_s(&g, &now);
#else
        gmtime_r(&now, &g);
#endif
        strftime(amzDate,  sizeof amzDate,  "%Y%m%dT%H%M%SZ", &g);
        strftime(dateStamp, sizeof dateStamp, "%Y%m%d",        &g);
    }

    /* --- Empreinte du corps (en-tete signe x-amz-content-sha256) --- */
    char payloadHash[65];
    aws_sha256_hex((const unsigned char*)body, bodyLen, payloadHash);

    /* --- Content-MD5 (ecritures rest-xml a corps non vide) ---
     * S3 exige un checksum de corps (Content-MD5 OU x-amz-checksum-*) sur une
     * classe d'ecritures (PutBucketTagging, PutBucketAcl, DeleteObjects...).
     * On pose Content-MD5 = base64(MD5(corps)) et on l'inclut dans la signature.
     * Scope strict : rest-xml + corps non vide. Les lectures rest-xml (corps
     * vide) et les autres protocoles ne changent pas de signature. */
    bool hasMd5 = (svc->protocol == AWS_PROTO_REST_XML && bodyLen > 0);
    char md5b64[25];
    md5b64[0] = '\0';
    if (hasMd5) {
        unsigned char md5raw[16];
        if (mbedtls_md5((const unsigned char*)body, bodyLen, md5raw) != 0) {
            free(body);
            return NIL_VAL;
        }
        size_t olen = 0;
        if (mbedtls_base64_encode((unsigned char*)md5b64, sizeof md5b64, &olen,
                                  md5raw, sizeof md5raw) != 0) {
            free(body);
            return NIL_VAL;
        }
    }

    /* --- Canonical headers + signed headers (ordre lexical minuscule) --- */
    bool hasToken  = (token[0] != '\0');
    bool hasTarget = (target[0] != '\0');

    /* content-md5 precede content-type dans l'ordre lexical SigV4
     * (prefixe commun "content-", puis 'm' < 't'). */
    size_t canonCap = strlen(contentType) + strlen(host) + strlen(payloadHash) +
                      strlen(amzDate) + strlen(token) + strlen(target) +
                      strlen(md5b64) + 256;
    char* canonHeaders = (char*)malloc(canonCap);
    char signedHeaders[256];
    if (canonHeaders == NULL) { free(body); return NIL_VAL; }

    int cn = 0;
    if (hasMd5)
        cn += snprintf(canonHeaders + cn, canonCap - (size_t)cn,
                       "content-md5:%s\n", md5b64);
    cn += snprintf(canonHeaders + cn, canonCap - (size_t)cn,
                   "content-type:%s\n"
                   "host:%s\n"
                   "x-amz-content-sha256:%s\n"
                   "x-amz-date:%s\n",
                   contentType, host, payloadHash, amzDate);
    if (hasToken)
        cn += snprintf(canonHeaders + cn, canonCap - (size_t)cn,
                       "x-amz-security-token:%s\n", token);
    if (hasTarget)
        cn += snprintf(canonHeaders + cn, canonCap - (size_t)cn,
                       "x-amz-target:%s\n", target);

    snprintf(signedHeaders, sizeof signedHeaders,
             "%scontent-type;host;x-amz-content-sha256;x-amz-date%s%s",
             hasMd5    ? "content-md5;"           : "",
             hasToken  ? ";x-amz-security-token"  : "",
             hasTarget ? ";x-amz-target"          : "");

    /* --- Path / query pour la signature ---
     * SigV4 exige canonicalUri (chemin seul) et canonicalQuery separes. Une
     * requestUri rest-* peut porter une sous-ressource S3 (`/<bucket>?tagging`) :
     * on splitte sur le PREMIER '?', le chemin va en canonicalUri, la query est
     * canonicalisee (encodee, triee) en canonicalQuery. Les protocoles non-rest
     * gardent requestUri = "/" (pas de '?') -> path = "/", query = "". L'URL
     * emise (urlBuf) conserve `chemin?query` d'origine (deja construite plus haut). */
    char pathBuf[1024];
    char queryBuf[1024];
    const char* canonUri   = requestUri;
    const char* canonQuery = "";
    {
        const char* q = strchr(requestUri, '?');
        if (q != NULL) {
            size_t plen = (size_t)(q - requestUri);
            if (plen >= sizeof pathBuf) plen = sizeof pathBuf - 1;
            memcpy(pathBuf, requestUri, plen);
            pathBuf[plen] = '\0';
            aws_canonicalize_query(q + 1, queryBuf, sizeof queryBuf);
            canonUri   = pathBuf;
            canonQuery = queryBuf;
        }
    }

    /* --- Signature SigV4 --- */
    OwlAwsSigV4Request sr;
    memset(&sr, 0, sizeof sr);
    sr.method          = method;
    sr.canonicalUri    = canonUri;
    sr.canonicalQuery  = canonQuery;
    sr.canonicalHeaders = canonHeaders;
    sr.signedHeaders   = signedHeaders;
    sr.body            = (const unsigned char*)body;
    sr.bodyLen         = bodyLen;
    sr.payloadMode     = OWL_AWS_PAYLOAD_SIGNED;
    sr.accessKeyId     = accessKey;
    sr.secretAccessKey = secretKey;
    sr.sessionToken    = hasToken ? token : NULL;
    sr.region          = signRegion;
    sr.service         = svc->signingName[0] ? svc->signingName : svc->endpointPrefix;
    sr.amzDate         = amzDate;
    sr.dateStamp       = dateStamp;

    OwlAwsSigV4Headers sigOut;
    int rc = owl_aws_sigv4_sign(&sr, &sigOut);
    free(canonHeaders);
    if (rc != 0) { free(body); return NIL_VAL; }

    /* --- Construction du httpRequête signe --- */
    ObjInstance* req = aws_clone_httprequete();
    if (req == NULL) { free(body); return NIL_VAL; }
    vm_push(OBJ_VAL(req));

    inst_field_set_str(req, "URL",         urlBuf);
    /* httpMéthodes string-valued : le champ porte le verbe HTTP. */
    inst_field_set_str(req, "Methode",     method);
    inst_field_set_str(req, "ContentType", contentType);
    inst_field_set_str(req, "Contenu",     body);

    {
        ObjString* ek = obj_copy_string("Entete", 6);
        Value ev;
        if (table_get(&req->fields, ek, &ev) && IS_ASSOC_ARRAY(ev)) {
            ObjAssocArray* entete = AS_ASSOC_ARRAY(ev);
            aws_header_set(entete, "Authorization",        sigOut.authorization);
            aws_header_set(entete, "X-Amz-Date",           sigOut.amzDate);
            aws_header_set(entete, "X-Amz-Content-Sha256", sigOut.contentSha256);
            if (hasMd5) aws_header_set(entete, "Content-MD5", md5b64);
            /* Host n'est PAS pose ici : la couche d'envoi (HTTPEnvoie) le derive
             * de l'URL, l'ajouter dupliquerait l'en-tete. Il reste bien signe
             * (present dans canonHeaders / signedHeaders passes au signeur). */
            if (hasToken)  aws_header_set(entete, "X-Amz-Security-Token", token);
            if (hasTarget) aws_header_set(entete, "X-Amz-Target",         target);
        }
    }
    free(body);

    /* --- Delegation reseau via HTTPEnvoie (mockable) --- */
    ObjString* sendKey = obj_copy_string("HTTPEnvoie", 10);
    vm_push(OBJ_VAL(sendKey));
    Value sendFn;
    bool okSend = table_get(&vm.globals, sendKey, &sendFn);
    vm_pop();
    if (!okSend) { vm_pop(); return NIL_VAL; }  /* req */

    vm_push(sendFn);
    vm_push(OBJ_VAL(req));
    if (vm_call_reentrant(sendFn, 1) != INTERPRET_OK) { vm_pop(); return NIL_VAL; }
    Value rep = vm_pop();   /* httpRéponse */
    vm_pop();               /* req */

    /* --- Decodage de la reponse (corps String OU Buffer) ---
     * rest-xml / query / ec2 : corps XML -> JSON generique ;
     * json / rest-json : corps JSON. */
    if (svc->protocol == AWS_PROTO_REST_XML ||
        svc->protocol == AWS_PROTO_QUERY ||
        svc->protocol == AWS_PROTO_EC2) {
        /* Descripteur de sortie de l'opération (NULL si non couverte ou sans
         * liste) : pilote le décodage des listes en tableaux déterministes. */
        const AwsXmlOutDescr* outd = aws_find_out_descr(svc->serviceName, action);
        return aws_decode_xml_response(rep, outd);
    }
    return aws_decode_json_response(rep);
}

/* ================================================================
 * AwsS3Client — type composé fluent (s3 est un AwsS3Client ; s3.Upload(...))
 *
 * S3 est l'outlier de la famille AWS : corps binaire (Buffer), protocole
 * rest-xml, URL path-style. Le générique AWSRequête (retour JSON, corps JSON)
 * ne sait pas le porter. On réutilise ici les mêmes briques de signature que
 * fn_aws_requete (aws_sha256_hex, aws_clone_httprequete, aws_header_set) mais
 * avec un cadrage propre à S3 : host s3.<région>.amazonaws.com, chemin
 * /<bucket>/<clé>, service de signature "s3", empreinte réelle du corps.
 * ================================================================ */

/* Lecture d'un champ (Value brute) d'une instance. false si absent. */
static bool aws_inst_get(ObjInstance* inst, const char* name, Value* out) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    return table_get(&inst->fields, k, out);
}

/* Session (ObjInstance AWSSession) portée par un client couche 2 (AwsS3Client,
 * AwsBedrockClient…), NULL si absente. */
static ObjInstance* aws_client_session(ObjInstance* client) {
    Value v;
    if (aws_inst_get(client, "Session", &v) && IS_INSTANCE(v)) return AS_INSTANCE(v);
    return NULL;
}

/* Octets d'une valeur Buffer OU chaîne (binary-safe). Pose out et outLen. */
static void aws_bytes_of(Value v, const unsigned char** out, size_t* outLen) {
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        *out = b->bytes; *outLen = (size_t)b->length;
    } else if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        *out = (const unsigned char*)s->chars; *outLen = (size_t)s->length;
    } else {
        *out = NULL; *outLen = 0;
    }
}

/* URI-encode un chemin S3 en préservant '/'. Les caractères non réservés
 * (RFC 3986 : A-Z a-z 0-9 - _ . ~) passent tels quels, le reste est
 * pourcent-encodé. Écrit une C-string dans out (borné par cap). */
static void s3_uri_encode_path(const char* s, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (unreserved) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* URI-encode un segment unique (RFC 3986) : seuls les caractères non réservés
 * (A-Z a-z 0-9 - _ . ~) passent tels quels, tout le reste — '/' inclus — est
 * pourcent-encodé. Contrairement à s3_uri_encode_path, ne préserve pas '/' :
 * destiné à un composant de chemin qui doit rester un seul segment dans la
 * canonical URI SigV4 (ex. le modelId Bedrock). */
static void aws_uri_encode_segment(const char* s, char* out, size_t cap) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0xF];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* Signe (SigV4) une requête path-style et la délègue à HTTPEnvoie (mockable).
 * Retourne la httpRéponse (Value) ou NIL_VAL en cas d'échec.
 *   `hostPrefix`  : sous-domaine régional, ex "s3" ou "bedrock-runtime" ;
 *                   l'hôte devient <hostPrefix>.<région>.amazonaws.com.
 *   `signingName` : nom de service SigV4, ex "s3" ou "bedrock" (peut différer
 *                   du sous-domaine — Bedrock signe "bedrock", pas "bedrock-runtime").
 *   `canonicalUri` doit déjà être URI-encodé ; `query` est la canonical query
 *   (déjà encodée, triée) ou "" ; `bodyVal` est le corps (Buffer/chaîne) posé
 *   tel quel dans req..Contenu, ou NIL_VAL pour un corps vide ; `contentType`
 *   = NULL pour ne pas signer/poser de Content-Type. */
static Value aws_sign_send(ObjInstance* client, const char* method,
                           const char* canonicalUri, const char* query,
                           Value bodyVal, const char* contentType,
                           const char* hostPrefix, const char* signingName) {
    ObjInstance* sess = aws_client_session(client);
    if (sess == NULL) return NIL_VAL;

    const char* region    = aws_inst_str(sess, "Region");
    const char* accessKey = aws_inst_str(sess, "CleAcces");
    const char* secretKey = aws_inst_str(sess, "CleSecrete");
    const char* token     = aws_inst_str(sess, "Jeton");

    char host[512];
    snprintf(host, sizeof host, "%s.%s.amazonaws.com", hostPrefix, region);

    char urlBuf[2048];
    if (query && query[0])
        snprintf(urlBuf, sizeof urlBuf, "https://%s%s?%s", host, canonicalUri, query);
    else
        snprintf(urlBuf, sizeof urlBuf, "https://%s%s", host, canonicalUri);

    /* Empreinte réelle du corps (en-tête signé x-amz-content-sha256). */
    const unsigned char* body = NULL;
    size_t bodyLen = 0;
    aws_bytes_of(bodyVal, &body, &bodyLen);
    char payloadHash[65];
    aws_sha256_hex(body, bodyLen, payloadHash);

    /* Horodatage UTC : AAAAMMJJTHHMMSSZ + AAAAMMJJ. */
    char amzDate[32], dateStamp[16];
    {
        time_t now = time(NULL);
        struct tm g;
#if defined(_WIN32)
        gmtime_s(&g, &now);
#else
        gmtime_r(&now, &g);
#endif
        strftime(amzDate,   sizeof amzDate,   "%Y%m%dT%H%M%SZ", &g);
        strftime(dateStamp, sizeof dateStamp, "%Y%m%d",         &g);
    }

    /* Canonical headers + signed headers (minuscules, triés lexicalement).
     * content-type n'est signé que s'il est fourni (upload). */
    bool hasCT    = (contentType != NULL && contentType[0] != '\0');
    bool hasToken = (token[0] != '\0');
    char canonHeaders[1400];
    char signedHeaders[256];
    int cn = 0;
    if (hasCT)
        cn += snprintf(canonHeaders + cn, sizeof canonHeaders - (size_t)cn,
                       "content-type:%s\n", contentType);
    cn += snprintf(canonHeaders + cn, sizeof canonHeaders - (size_t)cn,
                   "host:%s\n"
                   "x-amz-content-sha256:%s\n"
                   "x-amz-date:%s\n",
                   host, payloadHash, amzDate);
    if (hasToken)
        cn += snprintf(canonHeaders + cn, sizeof canonHeaders - (size_t)cn,
                       "x-amz-security-token:%s\n", token);
    snprintf(signedHeaders, sizeof signedHeaders,
             "%shost;x-amz-content-sha256;x-amz-date%s",
             hasCT ? "content-type;" : "",
             hasToken ? ";x-amz-security-token" : "");

    OwlAwsSigV4Request sr;
    memset(&sr, 0, sizeof sr);
    sr.method           = method;
    sr.canonicalUri     = canonicalUri;
    sr.canonicalQuery   = query ? query : "";
    sr.canonicalHeaders = canonHeaders;
    sr.signedHeaders    = signedHeaders;
    sr.body             = body;
    sr.bodyLen          = bodyLen;
    sr.payloadMode      = OWL_AWS_PAYLOAD_SIGNED;
    sr.accessKeyId      = accessKey;
    sr.secretAccessKey  = secretKey;
    sr.sessionToken     = hasToken ? token : NULL;
    sr.region           = region;
    sr.service          = signingName;
    sr.amzDate          = amzDate;
    sr.dateStamp        = dateStamp;

    OwlAwsSigV4Headers sigOut;
    if (owl_aws_sigv4_sign(&sr, &sigOut) != 0) return NIL_VAL;

    /* Construction du httpRequête signé. */
    ObjInstance* req = aws_clone_httprequete();
    if (req == NULL) return NIL_VAL;
    vm_push(OBJ_VAL(req));

    inst_field_set_str  (req, "URL",     urlBuf);
    /* httpMéthodes string-valued : le champ Méthode porte le verbe HTTP
     * (comparable a httpGet="GET"…, cf. ). `method` est deja le
     * verbe majuscule ("GET"/"PUT"/…). */
    inst_field_set_str  (req, "Methode", method);
    if (hasCT) inst_field_set_str(req, "ContentType", contentType);
    /* Corps posé tel quel : le Buffer conserve son identité (pas de retour
     * polymorphe, pas de conversion chaîne). */
    inst_field_set_value(req, "Contenu", IS_NIL(bodyVal) ? OBJ_VAL(obj_copy_string("", 0)) : bodyVal);

    {
        ObjString* ek = obj_copy_string("Entete", 6);
        Value ev;
        if (table_get(&req->fields, ek, &ev) && IS_ASSOC_ARRAY(ev)) {
            ObjAssocArray* entete = AS_ASSOC_ARRAY(ev);
            aws_header_set(entete, "Authorization",        sigOut.authorization);
            aws_header_set(entete, "X-Amz-Date",           sigOut.amzDate);
            aws_header_set(entete, "X-Amz-Content-Sha256", sigOut.contentSha256);
            if (hasToken) aws_header_set(entete, "X-Amz-Security-Token", token);
        }
    }

    /* Délégation réseau via HTTPEnvoie (mockable). */
    ObjString* sendKey = obj_copy_string("HTTPEnvoie", 10);
    vm_push(OBJ_VAL(sendKey));
    Value sendFn;
    bool okSend = table_get(&vm.globals, sendKey, &sendFn);
    vm_pop();
    if (!okSend) { vm_pop(); return NIL_VAL; }  /* req */

    vm_push(sendFn);
    vm_push(OBJ_VAL(req));
    if (vm_call_reentrant(sendFn, 1) != INTERPRET_OK) { vm_pop(); return NIL_VAL; }
    Value rep = vm_pop();   /* httpRéponse */
    vm_pop();               /* req */
    return rep;
}

/* Récupère la Value du champ Contenu d'une httpRéponse. NIL si absent. */
static Value aws_response_body(Value rep) {
    if (!IS_INSTANCE(rep)) return NIL_VAL;
    Value cv;
    if (aws_inst_get(AS_INSTANCE(rep), "Contenu", &cv)) return cv;
    return NIL_VAL;
}

/* Décode le corps d'une httpRéponse AWS en JSON. Le corps arrive en String OU
 * en Buffer selon la couche d'envoi (le vrai HTTPEnvoie renvoie un Buffer) :
 * les deux formes sont traitées pour éviter un null silencieux. Retourne un
 * ObjJson (JSON_T_NULL si réponse absente, non-instance, ou parse impossible).
 * Roote rep pendant l'extraction + le parse (peuvent déclencher un GC ; le
 * contenu vit dans rep). Partagé par fn_aws_requete et les clients couche 2. */
static Value aws_decode_json_response(Value rep) {
    if (!IS_INSTANCE(rep)) return OBJ_VAL(obj_new_json(JSON_T_NULL));
    vm_push(rep);
    Value cv = aws_response_body(rep);
    const char* contenu = "";
    int contenuLen = 0;
    if (IS_STRING(cv))      { contenu = AS_STRING(cv)->chars;              contenuLen = AS_STRING(cv)->length; }
    else if (IS_BUFFER(cv)) { contenu = (const char*)AS_BUFFER(cv)->bytes; contenuLen = AS_BUFFER(cv)->length; }
    ObjJson* parsed = json_parse_cstr(contenu, contenuLen);
    vm_pop();  /* rep */
    if (parsed == NULL) return OBJ_VAL(obj_new_json(JSON_T_NULL));
    return OBJ_VAL(parsed);
}

/* Cherche, dans un tableau de membres de descripteur de sortie, celui dont le
 * nom XML (locationName) correspond au nom d'element du fil. NULL si absent ou
 * si le descripteur est absent (decodage generique). */
static const AwsXmlMember* aws_find_out_member(const AwsXmlMember* members,
                                               int count, const char* name) {
    if (members == NULL) return NULL;
    for (int i = 0; i < count; i++)
        if (members[i].xmlName && strcmp(members[i].xmlName, name) == 0)
            return &members[i];
    return NULL;
}

/* Construit le JSON d'un element XML. `members`/`memberCount` decrivent les
 * enfants attendus de `node` (issus du descripteur de sortie shape-driven) ;
 * NULL/0 = decodage purement generique (comportement historique). Mapping :
 *   - element a texte seul            -> valeur chaine JSON ;
 *   - element a enfants               -> objet JSON de ses enfants ;
 *   - enfants de meme nom repetes     -> tableau JSON (generique) ;
 *   - membre marque LISTE (descr)     -> tableau JSON deterministe sous le nom
 *                                        du membre (hoist boto3), le niveau
 *                                        d'element repete etant consomme :
 *                                        0 enfant -> [], 1 -> [x], N -> [x..] ;
 *   - membre STRUCTURE (descr)        -> descente parallele dans ses membres ;
 *   - attributs ignores, namespaces reduits au nom local (node->name).
 * Le noeud retourne est laisse ROOTE sur la pile VM par l'appele ; l'appelant
 * le depile apres l'avoir rattache a son parent (deja atteignable). Recursif :
 * chaque niveau maintient son objet + le noeud enfant en cours rootes pendant
 * les allocations (obj_copy_string / obj_json_* peuvent declencher un GC). */
static ObjJson* aws_xml_node_to_json(xmlNodePtr node,
                                     const AwsXmlMember* members, int memberCount) {
    bool hasElemChild = false;
    for (xmlNodePtr c = node->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE) { hasElemChild = true; break; }
    }

    if (!hasElemChild) {
        /* Feuille : contenu texte (eventuellement vide). */
        xmlChar* txt = xmlNodeGetContent(node);
        int len = txt ? (int)xmlStrlen(txt) : 0;
        ObjString* vs = obj_copy_string(txt ? (const char*)txt : "", len);
        if (txt) xmlFree(txt);
        vm_push(OBJ_VAL(vs));
        ObjJson* leaf = obj_new_json_string(vs);
        vm_pop();               /* vs */
        vm_push(OBJ_VAL(leaf)); /* laisse roote pour l'appelant */
        return leaf;
    }

    ObjJson* obj = obj_new_json(JSON_T_OBJECT);
    vm_push(OBJ_VAL(obj));      /* roote pendant la construction ET pour l'appelant */
    for (xmlNodePtr c = node->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        const char* cname = (const char*)c->name;
        const AwsXmlMember* m = aws_find_out_member(members, memberCount, cname);

        if (m != NULL && m->kind == 2) {
            /* Membre liste : hoist en tableau sous le nom du membre. */
            ObjString* ks = obj_copy_string(cname, (int)strlen(cname));
            vm_push(OBJ_VAL(ks));
            if (m->flattened) {
                /* Aplati : `c` EST un item ; accumule dans le tableau existant
                 * (les items sont des enfants repetes directs du parent). */
                ObjJson* arr = obj_json_object_get(obj, ks);
                if (arr == NULL || arr->jtype != JSON_T_ARRAY) {
                    arr = obj_new_json(JSON_T_ARRAY);
                    vm_push(OBJ_VAL(arr));
                    obj_json_object_set(obj, ks, arr);
                    vm_pop();  /* arr (desormais roote via obj) */
                    arr = obj_json_object_get(obj, ks);
                }
                ObjJson* item = aws_xml_node_to_json(c, m->children, m->childCount);
                obj_json_array_push(arr, item);
                vm_pop();  /* item */
            } else {
                /* Non aplati : `c` est le conteneur ; ses enfants sont les items. */
                ObjJson* arr = obj_new_json(JSON_T_ARRAY);
                vm_push(OBJ_VAL(arr));
                for (xmlNodePtr it = c->children; it; it = it->next) {
                    if (it->type != XML_ELEMENT_NODE) continue;
                    ObjJson* item = aws_xml_node_to_json(it, m->children, m->childCount);
                    obj_json_array_push(arr, item);
                    vm_pop();  /* item */
                }
                obj_json_object_set(obj, ks, arr);
                vm_pop();  /* arr */
            }
            vm_pop();  /* ks */
            continue;
        }

        /* Structure (descente parallele) ou generique. */
        const AwsXmlMember* childMembers = (m != NULL && m->kind == 1) ? m->children : NULL;
        int childMemberCount = (m != NULL && m->kind == 1) ? m->childCount : 0;
        ObjJson* childJson = aws_xml_node_to_json(c, childMembers, childMemberCount);
        ObjString* ks = obj_copy_string(cname, (int)strlen(cname));
        vm_push(OBJ_VAL(ks));
        ObjJson* existing = obj_json_object_get(obj, ks);
        if (existing == NULL) {
            obj_json_object_set(obj, ks, childJson);
        } else if (existing->jtype == JSON_T_ARRAY) {
            obj_json_array_push(existing, childJson);
        } else {
            /* Deuxieme occurrence du meme nom : bascule la valeur scalaire en
             * tableau [ancienne, nouvelle]. */
            ObjJson* arr = obj_new_json(JSON_T_ARRAY);
            vm_push(OBJ_VAL(arr));
            obj_json_array_push(arr, existing);
            obj_json_array_push(arr, childJson);
            obj_json_object_set(obj, ks, arr);
            vm_pop();  /* arr */
        }
        vm_pop();  /* ks */
        vm_pop();  /* childJson (rattache) */
    }
    return obj;  /* laisse roote pour l'appelant */
}

/* ================================================================
 * Serialisation JSON -> XML des corps rest-xml (ecritures).
 *
 * Miroir de aws_xml_node_to_json : au lieu de decoder un arbre XML en JSON,
 * on construit un arbre XML depuis un corps JSON en suivant le descripteur de
 * shape genere (aws_service_models.h). Racine + espace de noms + membres par
 * locationName + imbrication recursive + listes en elements repetes. libxml2
 * gere l'echappement du texte ; xmlFreeDoc / xmlBufferFree sur tous les chemins.
 * ================================================================ */

/* Recherche lineaire du descripteur de serialisation d'une action (service +
 * action). NULL si l'action n'a pas de corps a serialiser (lecture). Le nombre
 * d'operations rest-xml a corps est petit : scan direct suffisant. */
static const AwsXmlSerDescr* aws_find_ser_descr(const char* service, const char* action) {
    for (int i = 0; i < AWS_SER_DESCR_COUNT; i++) {
        if (strcmp(AWS_SER_DESCRS[i].serviceName, service) == 0 &&
            strcmp(AWS_SER_DESCRS[i].actionName, action) == 0)
            return &AWS_SER_DESCRS[i];
    }
    return NULL;
}

/* Recherche lineaire du descripteur de sortie d'une action (service + action).
 * NULL si l'operation n'a pas de descripteur (pas de liste dans sa reponse, ou
 * operation non couverte) : le decodage retombe alors sur le generique. */
static const AwsXmlOutDescr* aws_find_out_descr(const char* service, const char* action) {
    for (int i = 0; i < AWS_OUT_DESCR_COUNT; i++) {
        if (strcmp(AWS_OUT_DESCRS[i].serviceName, service) == 0 &&
            strcmp(AWS_OUT_DESCRS[i].actionName, action) == 0)
            return &AWS_OUT_DESCRS[i];
    }
    return NULL;
}

/* Rend le texte XML d'une valeur scalaire JSON. Les nombres sont formates dans
 * buf ; les chaines pointent directement dans l'ObjString (valide tant que la
 * valeur reste rootee, ce qui est le cas : elle vit dans le corps argument). */
static const char* aws_json_scalar_text(ObjJson* v, char* buf, size_t cap) {
    switch (v->jtype) {
        case JSON_T_STRING: return v->as.strVal->chars;
        case JSON_T_INT:    snprintf(buf, cap, "%lld", (long long)v->as.intVal); return buf;
        case JSON_T_BOOL:   return v->as.boolVal ? "true" : "false";
        case JSON_T_REAL:   snprintf(buf, cap, "%.17g", v->as.realVal); return buf;
        default:            return "";
    }
}

/* Serialise un membre du descripteur sous `parent`. `obj` est l'objet JSON
 * courant dont on lit la valeur du membre (par nom JSON). Membre absent = omis.
 * Recursif pour structures et listes de structures. `ns` (eventuellement NULL)
 * est l'espace de noms par defaut applique a tous les elements. */
static void aws_member_to_xml(xmlNodePtr parent, xmlNsPtr ns,
                              const AwsXmlMember* m, ObjJson* obj) {
    ObjString* k = obj_copy_string(m->jsonName, (int)strlen(m->jsonName));
    ObjJson* v = obj_json_object_get(obj, k);
    if (v == NULL) return;

    if (m->kind == 1) {  /* structure */
        xmlNodePtr node = xmlNewChild(parent, ns, (const xmlChar*)m->xmlName, NULL);
        if (v->jtype == JSON_T_OBJECT)
            for (int i = 0; i < m->childCount; i++)
                aws_member_to_xml(node, ns, &m->children[i], v);
    } else if (m->kind == 2) {  /* liste */
        if (v->jtype != JSON_T_ARRAY) return;
        /* Aplati : les items se repetent directement sous le parent, sous le
         * nom du membre. Sinon : conteneur = nom du membre, items = listItemName. */
        xmlNodePtr container = m->flattened
            ? parent : xmlNewChild(parent, ns, (const xmlChar*)m->xmlName, NULL);
        const char* itemName = m->flattened ? m->xmlName : m->listItemName;
        for (int i = 0; i < v->as.arrayVal.count; i++) {
            ObjJson* item = v->as.arrayVal.items[i];
            if (m->itemKind == 1) {
                xmlNodePtr inode = xmlNewChild(container, ns, (const xmlChar*)itemName, NULL);
                if (item->jtype == JSON_T_OBJECT)
                    for (int c = 0; c < m->childCount; c++)
                        aws_member_to_xml(inode, ns, &m->children[c], item);
            } else {
                char nb[64];
                xmlNewTextChild(container, ns, (const xmlChar*)itemName,
                                (const xmlChar*)aws_json_scalar_text(item, nb, sizeof nb));
            }
        }
    } else if (m->scalarFmt == 1 && v->jtype == JSON_T_STRING) {  /* blob -> base64 */
        ObjString* sv = v->as.strVal;
        if (sv->length == 0) {
            /* base64("") == "" : blob vide = element vide. mbedtls_base64_encode
             * retourne 0 sans ecrire dans dst quand slen==0 (base64.c) : ne pas
             * lire le buffer non initialise, emettre directement un contenu vide. */
            xmlNewTextChild(parent, ns, (const xmlChar*)m->xmlName, (const xmlChar*)"");
        } else {
            size_t need = 4 * (((size_t)sv->length + 2) / 3) + 1;
            char* enc = (char*)malloc(need);
            size_t olen = 0;
            if (enc != NULL &&
                mbedtls_base64_encode((unsigned char*)enc, need, &olen,
                                      (const unsigned char*)sv->chars, (size_t)sv->length) == 0) {
                enc[olen] = '\0';  /* borne explicite : chaine C pour xmlNewTextChild */
                xmlNewTextChild(parent, ns, (const xmlChar*)m->xmlName, (const xmlChar*)enc);
            }
            free(enc);
        }
    } else {  /* scalaire */
        char nb[64];
        xmlNewTextChild(parent, ns, (const xmlChar*)m->xmlName,
                        (const xmlChar*)aws_json_scalar_text(v, nb, sizeof nb));
    }
}

/* Construit le corps XML d'une requete rest-xml depuis le corps JSON et le
 * descripteur de shape. Retourne un buffer heap-owned (a free) et pose *outLen,
 * ou NULL si le corps n'est pas un objet JSON. Le rendu est compact (sans
 * prologue <?xml?> ni indentation) : c'est le corps de requete brut. */
static char* aws_json_to_xml(Value corps, const AwsXmlSerDescr* descr, size_t* outLen) {
    if (!IS_JSON(corps) || AS_JSON(corps)->jtype != JSON_T_OBJECT) return NULL;
    ObjJson* obj = AS_JSON(corps);

    /* Membre payload restructure en racine : les membres du descripteur decrivent
     * le shape du payload, pas le request-shape. Il faut donc marcher ces membres
     * contre corps[payloadName], pas contre le top-level (ou vivent les membres
     * portes par l'URI/l'entete comme Bucket). Absent ou non-objet = corps vide. */
    if (descr->payloadName && descr->payloadName[0]) {
        ObjString* pk = obj_copy_string(descr->payloadName, (int)strlen(descr->payloadName));
        ObjJson* pv = obj_json_object_get(obj, pk);
        if (pv == NULL || pv->jtype != JSON_T_OBJECT) return NULL;
        obj = pv;
    }

    xmlDocPtr  doc  = xmlNewDoc((const xmlChar*)"1.0");
    xmlNodePtr root = xmlNewNode(NULL, (const xmlChar*)descr->rootName);
    xmlDocSetRootElement(doc, root);

    xmlNsPtr ns = NULL;
    if (descr->xmlNamespace && descr->xmlNamespace[0]) {
        ns = xmlNewNs(root, (const xmlChar*)descr->xmlNamespace, NULL);  /* ns par defaut */
        xmlSetNs(root, ns);
    }

    for (int i = 0; i < descr->memberCount; i++)
        aws_member_to_xml(root, ns, &descr->members[i], obj);

    xmlBufferPtr xbuf = xmlBufferCreate();
    xmlNodeDump(xbuf, doc, root, 0, 0);  /* element seul, sans prologue ni format */
    int   len  = xmlBufferLength(xbuf);
    char* body = (char*)malloc((size_t)len + 1);
    memcpy(body, xmlBufferContent(xbuf), (size_t)len);
    body[len] = '\0';
    if (outLen) *outLen = (size_t)len;

    xmlBufferFree(xbuf);
    xmlFreeDoc(doc);
    return body;
}

/* Decode le corps XML d'une httpReponse AWS (rest-xml) en JSON generique. Le
 * corps arrive en String OU Buffer selon la couche d'envoi. Le document est
 * enveloppe sous une cle = nom de l'element racine ({ "Result": { ... } }),
 * calque du style AWS. Retourne un ObjJson (objet vide si reponse absente,
 * non-instance, corps vide ou XML illisible). Roote rep + l'objet racine
 * pendant l'extraction et la construction. xmlFreeDoc sur tous les chemins. */
static Value aws_decode_xml_response(Value rep, const AwsXmlOutDescr* descr) {
    if (!IS_INSTANCE(rep)) return OBJ_VAL(obj_new_json(JSON_T_OBJECT));
    vm_push(rep);
    Value cv = aws_response_body(rep);
    const char* contenu = "";
    int contenuLen = 0;
    if (IS_STRING(cv))      { contenu = AS_STRING(cv)->chars;              contenuLen = AS_STRING(cv)->length; }
    else if (IS_BUFFER(cv)) { contenu = (const char*)AS_BUFFER(cv)->bytes; contenuLen = AS_BUFFER(cv)->length; }

    ObjJson* root = obj_new_json(JSON_T_OBJECT);
    vm_push(OBJ_VAL(root));  /* roote pendant la construction de l'arbre */

    /* Membres du descripteur = enfants attendus de l'element racine du fil.
     * Absent (NULL) -> decodage generique complet. */
    const AwsXmlMember* members = descr ? descr->members : NULL;
    int memberCount = descr ? descr->memberCount : 0;

    xmlDocPtr doc = (contenuLen > 0)
        ? xmlReadMemory(contenu, contenuLen, NULL, NULL,
                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
        : NULL;
    if (doc != NULL) {
        xmlNodePtr rootEl = xmlDocGetRootElement(doc);
        if (rootEl != NULL) {
            ObjJson* rootVal = aws_xml_node_to_json(rootEl, members, memberCount);  /* laisse roote */
            ObjString* ks = obj_copy_string((const char*)rootEl->name,
                                            (int)strlen((const char*)rootEl->name));
            vm_push(OBJ_VAL(ks));
            obj_json_object_set(root, ks, rootVal);
            vm_pop();  /* ks */
            vm_pop();  /* rootVal */
        }
        xmlFreeDoc(doc);
    }

    vm_pop();  /* root */
    vm_pop();  /* rep */
    return OBJ_VAL(root);
}

/* s3.Upload(bucket, clé, buffer, contentType) — PUT signé, corps = Buffer. */
static Value fn_s3_upload(int argCount, Value* args) {
    if (argCount != 5 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* bucket = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";
    const char* key    = IS_STRING(args[2]) ? AS_STRING(args[2])->chars : "";
    const char* ct     = IS_STRING(args[4]) ? AS_STRING(args[4])->chars : "application/octet-stream";

    char rawPath[1600], uri[2048];
    snprintf(rawPath, sizeof rawPath, "/%s/%s", bucket, key);
    s3_uri_encode_path(rawPath, uri, sizeof uri);

    aws_sign_send(client, "PUT", uri, "", args[3], ct, "s3", "s3");
    return NIL_VAL;
}

/* s3.Télécharge(bucket, clé) — GET signé, restitue le corps binaire (Buffer). */
static Value fn_s3_download(int argCount, Value* args) {
    if (argCount != 3 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* bucket = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";
    const char* key    = IS_STRING(args[2]) ? AS_STRING(args[2])->chars : "";

    char rawPath[1600], uri[2048];
    snprintf(rawPath, sizeof rawPath, "/%s/%s", bucket, key);
    s3_uri_encode_path(rawPath, uri, sizeof uri);

    Value rep = aws_sign_send(client, "GET", uri, "", NIL_VAL, NULL, "s3", "s3");
    vm_push(rep);  /* root : extraction du corps + conversion Buffer peuvent GC, body vit dans rep */
    Value body = aws_response_body(rep);
    Value result;
    if (IS_BUFFER(body)) {
        result = body;  /* octets bruts restitués sans conversion */
    } else if (IS_STRING(body)) {
        ObjString* s = AS_STRING(body);
        result = OBJ_VAL(obj_new_buffer(s->chars, s->length));
    } else {
        result = OBJ_VAL(obj_buffer_empty(0));
    }
    vm_pop();  /* rep */
    return result;
}

/* Pose une paire (clé -> chaîne) dans un JSON objet rooté par l'appelant. */
static void s3_json_set_str(ObjJson* obj, const char* key, const char* val, int vlen) {
    ObjString* vs = obj_copy_string(val, vlen);
    vm_push(OBJ_VAL(vs));
    ObjJson* vj = obj_new_json_string(vs);
    vm_push(OBJ_VAL(vj));
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    vm_push(OBJ_VAL(ks));
    obj_json_object_set(obj, ks, vj);
    vm_pop(); vm_pop(); vm_pop();
}

/* Pose une paire (clé -> entier) dans un JSON objet rooté par l'appelant. */
static void s3_json_set_int(ObjJson* obj, const char* key, int64_t val) {
    ObjJson* vj = obj_new_json_int(val);
    vm_push(OBJ_VAL(vj));
    ObjString* ks = obj_copy_string(key, (int)strlen(key));
    vm_push(OBJ_VAL(ks));
    obj_json_object_set(obj, ks, vj);
    vm_pop(); vm_pop();
}

/* Texte d'un élément fils direct nommé `name` (première occurrence), NULL si
 * absent. Retourne un pointeur xmlChar à libérer via xmlFree. */
static xmlChar* s3_child_text(xmlNodePtr parent, const char* name) {
    for (xmlNodePtr c = parent->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE &&
            xmlStrcmp(c->name, (const xmlChar*)name) == 0)
            return xmlNodeGetContent(c);
    }
    return NULL;
}

/* Parse une réponse ListBucketResult (rest-xml S3) vers un JSON calque AWS :
 * { "Name": ..., "Contents": [ { "Key": ..., "Size": <int> }, ... ] }. Les
 * noms de champs restent bruts (calque AWS/XML, cf ). */
static Value s3_parse_list_xml(const char* xml, int len) {
    ObjJson* root = obj_new_json(JSON_T_OBJECT);
    vm_push(OBJ_VAL(root));
    ObjJson* contents = obj_new_json(JSON_T_ARRAY);
    vm_push(OBJ_VAL(contents));
    {
        ObjString* ck = obj_copy_string("Contents", 8);
        vm_push(OBJ_VAL(ck));
        obj_json_object_set(root, ck, contents);
        vm_pop();
    }

    xmlDocPtr doc = (len > 0) ? xmlReadMemory(xml, len, NULL, NULL,
                                              XML_PARSE_NOERROR | XML_PARSE_NOWARNING)
                              : NULL;
    if (doc != NULL) {
        xmlNodePtr rootEl = xmlDocGetRootElement(doc);
        if (rootEl != NULL) {
            xmlChar* nm = s3_child_text(rootEl, "Name");
            if (nm != NULL) {
                s3_json_set_str(root, "Name", (const char*)nm, (int)xmlStrlen(nm));
                xmlFree(nm);
            }
            for (xmlNodePtr c = rootEl->children; c; c = c->next) {
                if (c->type != XML_ELEMENT_NODE ||
                    xmlStrcmp(c->name, (const xmlChar*)"Contents") != 0)
                    continue;
                ObjJson* entry = obj_new_json(JSON_T_OBJECT);
                vm_push(OBJ_VAL(entry));
                xmlChar* keyTxt = s3_child_text(c, "Key");
                if (keyTxt != NULL) {
                    s3_json_set_str(entry, "Key", (const char*)keyTxt, (int)xmlStrlen(keyTxt));
                    xmlFree(keyTxt);
                }
                xmlChar* sizeTxt = s3_child_text(c, "Size");
                if (sizeTxt != NULL) {
                    s3_json_set_int(entry, "Size", (int64_t)strtoll((const char*)sizeTxt, NULL, 10));
                    xmlFree(sizeTxt);
                }
                obj_json_array_push(contents, entry);
                vm_pop();  /* entry */
            }
        }
        xmlFreeDoc(doc);
    }

    vm_pop();  /* contents */
    vm_pop();  /* root */
    return OBJ_VAL(root);
}

/* s3.Liste(bucket, préfixe) — GET signé, réponse rest-xml parsée en JSON. */
static Value fn_s3_list(int argCount, Value* args) {
    if (argCount != 3 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* bucket = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";
    const char* prefix = IS_STRING(args[2]) ? AS_STRING(args[2])->chars : "";

    char rawPath[1600], uri[2048];
    snprintf(rawPath, sizeof rawPath, "/%s", bucket);
    s3_uri_encode_path(rawPath, uri, sizeof uri);

    /* Canonical query : prefix=<encodé> (préfixe optionnel). */
    char query[1200] = "";
    if (prefix[0]) {
        char enc[1024];
        s3_uri_encode_path(prefix, enc, sizeof enc);
        snprintf(query, sizeof query, "prefix=%s", enc);
    }

    Value rep = aws_sign_send(client, "GET", uri, query, NIL_VAL, NULL, "s3", "s3");
    vm_push(rep);  /* root : extraction du corps + parse XML peuvent GC, xml vit dans rep */
    Value body = aws_response_body(rep);
    const char* xml = "";
    int xmlLen = 0;
    if (IS_STRING(body))      { xml = AS_STRING(body)->chars; xmlLen = AS_STRING(body)->length; }
    else if (IS_BUFFER(body)) { xml = (const char*)AS_BUFFER(body)->bytes; xmlLen = AS_BUFFER(body)->length; }
    Value result = s3_parse_list_xml(xml, xmlLen);
    vm_pop();  /* rep */
    return result;
}

/* s3.Supprime(bucket, clé) — DELETE signé. */
static Value fn_s3_delete(int argCount, Value* args) {
    if (argCount != 3 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* bucket = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";
    const char* key    = IS_STRING(args[2]) ? AS_STRING(args[2])->chars : "";

    char rawPath[1600], uri[2048];
    snprintf(rawPath, sizeof rawPath, "/%s/%s", bucket, key);
    s3_uri_encode_path(rawPath, uri, sizeof uri);

    aws_sign_send(client, "DELETE", uri, "", NIL_VAL, NULL, "s3", "s3");
    return NIL_VAL;
}

/* ================================================================
 * AwsBedrockClient — type composé fluent (b est un AwsBedrockClient ;
 * b.Invoque(modelId, corps)). Service d'inférence : host bedrock-runtime,
 * signature service "bedrock", POST /model/<modelId>/invoke, corps JSON
 * transmis tel quel (client agnostique du contenu modèle), réponse décodée.
 * Réutilise le signeur générique aws_sign_send (aucune duplication crypto).
 * ================================================================ */

/* b.Invoque(modelId, corps) — POST signé sur /model/<modelId>/invoke, corps
 * JSON sérialisé, réponse décodée en JSON. */
static Value fn_bedrock_invoke(int argCount, Value* args) {
    if (argCount != 3 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* modelId = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";

    /* Corps : sérialisation JSON canonique (le client ne connaît pas le schéma
     * modèle, il transmet le JSON fourni). */
    size_t bodyLen = 0;
    char* body = aws_serialize_json(args[2], &bodyLen);
    ObjString* bodyStr = obj_copy_string(body, (int)bodyLen);
    free(body);
    vm_push(OBJ_VAL(bodyStr));  /* root pendant aws_sign_send */

    /* Canonical URI : /model/<modelId encodé segment>/invoke. */
    char enc[2048], uri[2100];
    aws_uri_encode_segment(modelId, enc, sizeof enc);
    snprintf(uri, sizeof uri, "/model/%s/invoke", enc);

    Value rep = aws_sign_send(client, "POST", uri, "", OBJ_VAL(bodyStr),
                              "application/json", "bedrock-runtime", "bedrock");
    vm_pop();  /* bodyStr */

    /* Décodage JSON de la réponse (corps String OU Buffer). */
    return aws_decode_json_response(rep);
}

/* ================================================================
 * AwsLambdaClient — type composé fluent (l est un AwsLambdaClient ;
 * l.Invoque(nomFonction, charge)). Service d'exécution de fonctions : host
 * lambda régional, signature service "lambda", POST
 * /2015-03-31/functions/<nomFonction>/invocations, corps = charge JSON
 * transmise telle quelle (client agnostique du schéma de la fonction),
 * réponse décodée. Réutilise aws_sign_send (aucune duplication crypto).
 * ================================================================ */

/* l.Invoque(nomFonction, charge) — POST signé sur
 * /2015-03-31/functions/<nomFonction>/invocations, charge JSON sérialisée,
 * réponse décodée en JSON. */
static Value fn_lambda_invoke(int argCount, Value* args) {
    if (argCount != 3 || !IS_INSTANCE(args[0])) return NIL_VAL;
    ObjInstance* client = AS_INSTANCE(args[0]);
    const char* fnName = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";

    /* Corps : la charge JSON fournie est transmise telle quelle (payload). */
    size_t bodyLen = 0;
    char* body = aws_serialize_json(args[2], &bodyLen);
    ObjString* bodyStr = obj_copy_string(body, (int)bodyLen);
    free(body);
    vm_push(OBJ_VAL(bodyStr));  /* root pendant aws_sign_send */

    /* Canonical URI : /2015-03-31/functions/<nomFonction encodé segment>/invocations. */
    char enc[2048], uri[2200];
    aws_uri_encode_segment(fnName, enc, sizeof enc);
    snprintf(uri, sizeof uri, "/2015-03-31/functions/%s/invocations", enc);

    Value rep = aws_sign_send(client, "POST", uri, "", OBJ_VAL(bodyStr),
                              "application/json", "lambda", "lambda");
    vm_pop();  /* bodyStr */

    /* Décodage JSON de la réponse (résultat de l'invocation, corps String OU Buffer). */
    return aws_decode_json_response(rep);
}

/* ================================================================
 * Type AwsS3Client — déclaration + template runtime
 * ================================================================ */

static const OwlField FIELDS_AwsS3Client[] = {
    {"Session", NULL, "AWSSession", "Null",
     "session AWS (identifiants + région) utilisée pour signer les requêtes",
     "AWS session (credentials + region) used to sign requests",
     .nameEN = "Session"},
    {0},
};

static const OwlTypeDef AWSS3_TYPES[] = {
    { .canonical      = "AwsS3Client",
      .canonicalAscii = "AwsS3Client",
      .aliases        = NULL,
      .category       = "AWS",
      .descFR         = "Client objet pour le service de stockage S3. Instancié par `s3 est un AwsS3Client`, sa session se pose via la propriété ..Session, puis les opérations s'appellent en forme fluente : s3.Upload(bucket, clé, buffer, contentType), s3.Télécharge(bucket, clé), s3.Liste(bucket, préfixe), s3.Supprime(bucket, clé).",
      .descEN         = "Object client for the S3 storage service. Instantiated with `s3 is a AwsS3Client`, its session is set through the ..Session property, then operations are called fluently: s3.Upload(bucket, key, buffer, contentType), s3.Download(bucket, key), s3.List(bucket, prefix), s3.Delete(bucket, key).",
      .fields         = FIELDS_AwsS3Client,
      .nameEN         = "AwsS3Client",
      .genre          = OWL_GENRE_M },   /* le client */
};

static ObjInstance* build_awss3client_template(void) {
    ObjString* name = obj_copy_string("AwsS3Client", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_value(tpl, "Session", NIL_VAL);
    return tpl;
}

/* ================================================================
 * Type AwsBedrockClient — déclaration + template runtime
 * ================================================================ */

static const OwlField FIELDS_AwsBedrockClient[] = {
    {"Session", NULL, "AWSSession", "Null",
     "session AWS (identifiants + région) utilisée pour signer les requêtes",
     "AWS session (credentials + region) used to sign requests",
     .nameEN = "Session"},
    {0},
};

static const OwlTypeDef AWSBEDROCK_TYPES[] = {
    { .canonical      = "AwsBedrockClient",
      .canonicalAscii = "AwsBedrockClient",
      .aliases        = NULL,
      .category       = "AWS",
      .descFR         = "Client objet pour le service d'inférence Bedrock. Instancié par `b est un AwsBedrockClient`, sa session se pose via la propriété ..Session, puis l'invocation d'un modèle s'appelle en forme fluente : b.Invoque(modelId, corps). Le corps JSON fourni est transmis tel quel et la réponse est retournée décodée en JSON.",
      .descEN         = "Object client for the Bedrock inference service. Instantiated with `b is a AwsBedrockClient`, its session is set through the ..Session property, then a model is invoked fluently: b.Invoke(modelId, body). The provided JSON body is sent as-is and the response is returned decoded as JSON.",
      .fields         = FIELDS_AwsBedrockClient,
      .nameEN         = "AwsBedrockClient",
      .genre          = OWL_GENRE_M },   /* le client */
};

static ObjInstance* build_awsbedrockclient_template(void) {
    ObjString* name = obj_copy_string("AwsBedrockClient", 16);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_value(tpl, "Session", NIL_VAL);
    return tpl;
}

/* ================================================================
 * Type AwsLambdaClient — déclaration + template runtime
 * ================================================================ */

static const OwlField FIELDS_AwsLambdaClient[] = {
    {"Session", NULL, "AWSSession", "Null",
     "session AWS (identifiants + région) utilisée pour signer les requêtes",
     "AWS session (credentials + region) used to sign requests",
     .nameEN = "Session"},
    {0},
};

static const OwlTypeDef AWSLAMBDA_TYPES[] = {
    { .canonical      = "AwsLambdaClient",
      .canonicalAscii = "AwsLambdaClient",
      .aliases        = NULL,
      .category       = "AWS",
      .descFR         = "Client objet pour le service d'exécution de fonctions Lambda. Instancié par `l est un AwsLambdaClient`, sa session se pose via la propriété ..Session, puis l'invocation d'une fonction s'appelle en forme fluente : l.Invoque(nomFonction, charge). La charge JSON fournie est transmise telle quelle et la réponse est retournée décodée en JSON.",
      .descEN         = "Object client for the Lambda function execution service. Instantiated with `l is a AwsLambdaClient`, its session is set through the ..Session property, then a function is invoked fluently: l.Invoke(functionName, payload). The provided JSON payload is sent as-is and the response is returned decoded as JSON.",
      .fields         = FIELDS_AwsLambdaClient,
      .nameEN         = "AwsLambdaClient",
      .genre          = OWL_GENRE_M },   /* le client */
};

static ObjInstance* build_awslambdaclient_template(void) {
    ObjString* name = obj_copy_string("AwsLambdaClient", 15);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_value(tpl, "Session", NIL_VAL);
    return tpl;
}

/* ================================================================
 * Registry
 * ================================================================ */

static const OwlParam p_aws_requete[] = {
    {"session", "AWSSession", "identifiants AWS (région + clés)", NULL, NULL, false},
    /* requireEnumValueCoverage : awsService a un branchement par service
     * (switch protocol) — le self-test (regle 61) exige un scenario par valeur
     * pour eviter qu'un cas non exerce tombe dans default en restant vert. */
    {"service", "entier",     "service AWS ciblé (∈ awsService)", &E_awsService, NULL,
                              false, false, .requireEnumValueCoverage=true},
    {"action",  "chaîne",     "action à invoquer (symbole aws<Service><Action> ou chaîne libre)",
                              NULL, NULL, false, .awsAction=true},
    {"corps",   "JSON",       "corps de la requête", NULL, NULL, false},
    {0}
};
static const OwlSignature sig_aws_requete[] = {
    {4, p_aws_requete, "JSON", "réponse du service décodée en JSON"},
    {0}
};

static const OwlNativeDef DEFS_AWS[] = {
    { .canonical = "AWSRequ\xc3\xaate", .canonicalAscii = "AWSRequete", .nameEN = "AWSRequest",
      .category = "AWS",
      .descFR = "Signe (AWS4-HMAC-SHA256) et envoie une requête vers un service AWS, "
                "à partir d'une AWSSession, d'un service et d'une action. "
                "Retourne la réponse décodée en JSON.",
      .descEN = "Sign (AWS4-HMAC-SHA256) and send a request to an AWS service, from an "
                "AWSSession, a service and an action. Returns the response decoded as JSON.",
      .signatures = sig_aws_requete, .impl = fn_aws_requete },
};

void stdlib_aws_init(void) {
    owl_registry_register_module(DEFS_AWS, (int)(sizeof(DEFS_AWS)/sizeof(DEFS_AWS[0])));
    owl_registry_register_enums (AWS_ENUMS, (int)(sizeof(AWS_ENUMS)/sizeof(AWS_ENUMS[0])));
    owl_registry_register_consts(AWS_CONSTS, AWS_CONST_COUNT);

    /* Type composé AwsS3Client + template runtime (__StructClone). */
    owl_registry_register_types(AWSS3_TYPES, (int)(sizeof(AWSS3_TYPES)/sizeof(AWSS3_TYPES[0])));
    ObjInstance* tpl = build_awss3client_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("AwsS3Client", OBJ_VAL(tpl));
    vm_pop();

    /* Méthodes fluentes (forme objet uniquement — aucune native statique S3).
     * Bilingues : la forme FR et la forme EN pointent la même impl. Arité =
     * receveur inclus. */
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "Upload",     fn_s3_upload,   5);
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "T\xc3\xa9l\xc3\xa9""charge", fn_s3_download, 3); /* Télécharge */
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "Download",   fn_s3_download, 3);
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "Liste",      fn_s3_list,     3);
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "List",       fn_s3_list,     3);
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "Supprime",   fn_s3_delete,   3);
    owl_register_method(METHOD_TYPE_AWS_S3_CLIENT, "Delete",     fn_s3_delete,   3);

    /* Type composé AwsBedrockClient + template runtime. */
    owl_registry_register_types(AWSBEDROCK_TYPES, (int)(sizeof(AWSBEDROCK_TYPES)/sizeof(AWSBEDROCK_TYPES[0])));
    ObjInstance* tplBedrock = build_awsbedrockclient_template();
    vm_push(OBJ_VAL(tplBedrock));
    vm_type_register("AwsBedrockClient", OBJ_VAL(tplBedrock));
    vm_pop();

    /* Méthode fluente bilingue Invoque/Invoke (receveur inclus dans l'arité). */
    owl_register_method(METHOD_TYPE_AWS_BEDROCK_CLIENT, "Invoque", fn_bedrock_invoke, 3);
    owl_register_method(METHOD_TYPE_AWS_BEDROCK_CLIENT, "Invoke",  fn_bedrock_invoke, 3);

    /* Type composé AwsLambdaClient + template runtime. */
    owl_registry_register_types(AWSLAMBDA_TYPES, (int)(sizeof(AWSLAMBDA_TYPES)/sizeof(AWSLAMBDA_TYPES[0])));
    ObjInstance* tplLambda = build_awslambdaclient_template();
    vm_push(OBJ_VAL(tplLambda));
    vm_type_register("AwsLambdaClient", OBJ_VAL(tplLambda));
    vm_pop();

    /* Méthode fluente bilingue Invoque/Invoke (receveur inclus dans l'arité). */
    owl_register_method(METHOD_TYPE_AWS_LAMBDA_CLIENT, "Invoque", fn_lambda_invoke, 3);
    owl_register_method(METHOD_TYPE_AWS_LAMBDA_CLIENT, "Invoke",  fn_lambda_invoke, 3);
}
