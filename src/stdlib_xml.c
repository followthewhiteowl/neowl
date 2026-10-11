// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_xml.c — Fonctions natives XML pour OWL
 *
 * Implémente les types xmlDocument, xmlNoeud, xmlAttribut, xmlNamespace
 * et les fonctions associées, en utilisant libxml2 comme moteur DOM.
 *
 * Types OWL :
 *   xmlDocument  → ObjInstance(structName="xmlDocument",  _ptr=xmlDocPtr)
 *   xmlNoeud     → ObjInstance(structName="xmlNoeud",     _ptr=xmlNodePtr)
 *   xmlAttribut  → ObjInstance(structName="xmlAttribut",  _ptr=xmlAttrPtr)
 *   xmlNamespace → ObjInstance(structName="xmlNamespace", _ptr=xmlNsPtr)
 *
 * Cycle de vie :
 *   xmlDocPtr  : alloué par XMLOuvre/XMLDocument, libéré par XMLTermine().
 *   xmlNodePtr / xmlAttrPtr / xmlNsPtr : pointeurs dans l'arbre du document,
 *   JAMAIS libérés indépendamment (libxml2 les libère avec le document).
 */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "vm.h"
#include "object.h"
#include "value.h"
#include "memory.h"
#include "owl_registry.h"
#include "owl_methods.h"
#include "owl_stdlib.h"
#include "owl_errors.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/xmlsave.h>
#include <libxml/entities.h>
#include <libxml/xmlschemas.h>

/* ================================================================
 * Helpers internes
 * ================================================================ */

static ObjInstance* xml_make_instance(const char* structName, void* nativePtr) {
    ObjString* sname = obj_copy_string(structName, (int)strlen(structName));
    vm_push(OBJ_VAL(sname));
    ObjInstance* inst = obj_new_instance(sname);
    vm_push(OBJ_VAL(inst));
    /*  : delegue au canon inst_field_set_int. */
    inst_field_set_int(inst, "_ptr", (int64_t)(intptr_t)nativePtr);
    vm_pop(); /* inst */
    vm_pop(); /* sname */
    return inst;
}

static void* xml_get_ptr(ObjInstance* inst) {
    /*  lot 6 : root inst pendant obj_copy_string (C-stack-local). */
    vm_push(OBJ_VAL(inst));
    ObjString* key = obj_copy_string("_ptr", 4);
    vm_pop();
    Value v;
    if (!table_get(&inst->fields, key, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (void*)(intptr_t)AS_INTEGER(v);
}

static void xml_invalidate_ptr(ObjInstance* inst) {
    /*  lot 6 : delegate to canonical helper (roots inst+key). */
    inst_field_set_int(inst, "_ptr", 0);
}

static bool xml_is_type(Value v, const char* structName) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    return inst->structName != NULL &&
           strcmp(inst->structName->chars, structName) == 0;
}

static Value xml_str(const xmlChar* s) {
    if (!s) return OBJ_VAL(obj_copy_string("", 0));
    return OBJ_VAL(obj_copy_string((const char*)s, (int)strlen((const char*)s)));
}

/* ================================================================
 * Constantes de types de nœuds et modes d'ouverture
 * ================================================================ */

#define XML_MODE_FICHIER  1
#define XML_MODE_CHAINE   2
#define XML_MODE_URL      3

#define XML_TYPE_BALISE   1
#define XML_TYPE_TEXTE    2
#define XML_TYPE_CDATA    3
#define XML_TYPE_COMMENT  4
#define XML_TYPE_PI       5
#define XML_TYPE_ATTR     6

/* Enum xmlSourceMode : les modes depuis* acceptes par le parametre `mode`
 * de XMLOuvre. Valeurs entieres positives (1/2/3) lues par un switch runtime
 * (cf. fn_xml_ouvre : if (mode == XML_MODE_CHAINE)...), donc la valeur porte
 * une semantique runtime -> force_small_int=true (opt-out rule 20 ; sentinel-er
 * casserait la lecture du mode). Modes mutuellement exclusifs -> combinable=false.
 * Les noms doivent matcher exactement les canon de XML_CONSTS (rule 5). */
static const char* const E_xmlSourceMode_values[] = {
    "depuisFichier", "depuisCha\xc3\xae" "ne", "depuisURL", NULL
};
static const OwlEnum E_xmlSourceMode = {
    .name="xmlSourceMode", .nameAscii="xmlSourceMode", .nameEN="xmlSourceMode",
    .category="XML",
    .descFR="Mode de chargement d'un document XML par XMLOuvre : depuis un fichier (ou une URL), depuis une chaîne XML en mémoire, ou depuis une URL HTTP/HTTPS. Le mode oriente l'interprétation de la source fournie.",
    .descEN="Loading mode for an XML document opened by XMLOpen: from a file (or URL), from an in-memory XML string, or from an HTTP/HTTPS URL. The mode drives how the supplied source is interpreted.",
    .values=E_xmlSourceMode_values, .combinable=false, .force_small_int=true
};
static const OwlEnum* const XML_ENUMS[] = { &E_xmlSourceMode };

static int libxml2_type_to_owl(xmlElementType t) {
    switch (t) {
        case XML_ELEMENT_NODE:       return XML_TYPE_BALISE;
        case XML_TEXT_NODE:          return XML_TYPE_TEXTE;
        case XML_CDATA_SECTION_NODE: return XML_TYPE_CDATA;
        case XML_COMMENT_NODE:       return XML_TYPE_COMMENT;
        case XML_PI_NODE:            return XML_TYPE_PI;
        case XML_ATTRIBUTE_NODE:     return XML_TYPE_ATTR;
        default:                     return (int)t;
    }
}

/* ================================================================
 * Helpers navigation
 * ================================================================ */

static xmlNodePtr first_element_child(xmlNodePtr node) {
    xmlNodePtr c = node->children;
    while (c && c->type != XML_ELEMENT_NODE) c = c->next;
    return c;
}
static xmlNodePtr last_element_child(xmlNodePtr node) {
    xmlNodePtr c = node->last;
    while (c && c->type != XML_ELEMENT_NODE) c = c->prev;
    return c;
}
static xmlNodePtr next_element_sibling(xmlNodePtr node) {
    xmlNodePtr c = node->next;
    while (c && c->type != XML_ELEMENT_NODE) c = c->next;
    return c;
}
static xmlNodePtr prev_element_sibling(xmlNodePtr node) {
    xmlNodePtr c = node->prev;
    while (c && c->type != XML_ELEMENT_NODE) c = c->prev;
    return c;
}

/* Résout un nœud xmlNoeud ou xmlDocument → xmlNodePtr (racine pour document). */
static xmlNodePtr xml_coerce_to_node(Value v) {
    if (xml_is_type(v, "xmlNoeud"))
        return (xmlNodePtr)xml_get_ptr(AS_INSTANCE(v));
    if (xml_is_type(v, "xmlDocument")) {
        xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(v));
        return doc ? xmlDocGetRootElement(doc) : NULL;
    }
    return NULL;
}

/* ================================================================
 * XMLOuvre / XMLOpen
 * args[0]: source (chaîne)
 * args[1]: mode (entier, optionnel) — depuisFichier / depuisChaine / depuisURL
 * ================================================================ */
static Value fn_xml_ouvre(int argCount, Value* args) {
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLOuvre",
                              "native", "XMLOuvre",
                              "arg", "1",
                              "expected", "chaîne",
                              "actual", "non chaîne",
                              NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    int mode = XML_MODE_FICHIER;
    if (argCount >= 2 && IS_INTEGER(args[1]))
        mode = (int)AS_INTEGER(args[1]);

    const char* src = AS_STRING(args[0])->chars;
    xmlDocPtr doc = NULL;
    if (mode == XML_MODE_CHAINE) {
        doc = xmlParseDoc((const xmlChar*)src);
    } else {
        /* fichier ou URL — libxml2 gère les deux via xmlParseFile */
        doc = xmlParseFile(src);
    }
    if (!doc) {
        const xmlError* xe = xmlGetLastError();
        owl_emit_native_error(E_NATIVE_XML_PARSE_ERROR, "XMLOuvre",
                              "native", "XMLOuvre",
                              "detail", (xe && xe->message) ? xe->message
                                                            : "document XML invalide",
                              NULL);
        return NIL_VAL;
    }
    return OBJ_VAL(xml_make_instance("xmlDocument", doc));
}

/* ================================================================
 * XMLDocument
 * args[0]: version (chaîne, optionnel, défaut "1.0")
 * ================================================================ */
static Value fn_xml_document(int argCount, Value* args) {
    const char* version = "1.0";
    if (argCount >= 1 && IS_STRING(args[0]))
        version = AS_STRING(args[0])->chars;
    owl_clear_error();
    xmlDocPtr doc = xmlNewDoc((const xmlChar*)version);
    if (!doc) {
        owl_emit_native_error(E_NATIVE_XML_NODE_CREATE_FAILURE, "XMLDocument",
                              "native", "XMLDocument",
                              "detail", "échec création document",
                              NULL);
        return NIL_VAL;
    }
    return OBJ_VAL(xml_make_instance("xmlDocument", doc));
}

/* ================================================================
 * XMLTermine / XMLClose
 * args[0]: xmlDocument
 * ================================================================ */
static Value fn_xml_termine(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLTermine",
                              "native", "XMLTermine",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(inst);
    if (doc) {
        xmlFreeDoc(doc);
        xml_invalidate_ptr(inst);
    }
    return NIL_VAL;
}

/* ================================================================
 * XMLConstruitChaîne / XMLBuildString
 * args[0]: xmlDocument
 * ================================================================ */
static Value fn_xml_construit_chaine(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLConstruitChaîne",
                              "native", "XMLConstruitChaîne",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return OBJ_VAL(obj_copy_string("", 0));
    xmlChar* buf = NULL; int sz = 0;
    xmlDocDumpFormatMemoryEnc(doc, &buf, &sz, "UTF-8", 1);
    if (!buf) return OBJ_VAL(obj_copy_string("", 0));
    Value result = OBJ_VAL(obj_copy_string((const char*)buf, sz));
    xmlFree(buf);
    return result;
}

/* ================================================================
 * XMLSauve / XMLSave
 * args[0]: xmlDocument, args[1]: chemin fichier
 * ================================================================ */
static Value fn_xml_sauve(int argCount, Value* args) {
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLSauve",
                              "native", "XMLSauve",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    OWL_REQUIRE_ARG("XMLSauve", "chemin", 1, IS_STRING(args[1]),
                    "chaîne", BOOL_VAL(false));
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return BOOL_VAL(false);
    int ret = xmlSaveFormatFileEnc(AS_STRING(args[1])->chars, doc, "UTF-8", 1);
    if (ret < 0) {
        owl_emit_native_error(E_NATIVE_XML_SAVE_FAILURE, "XMLSauve",
                              "native", "XMLSauve",
                              "detail", "écriture fichier impossible",
                              NULL);
        return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

/* ================================================================
 * XMLRacine / XMLRoot
 * args[0]: xmlDocument
 * ================================================================ */
static Value fn_xml_racine(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLRacine",
                              "native", "XMLRacine",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return NIL_VAL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", root));
}

/* ================================================================
 * Navigation : Premier / Dernier / Suivant / Précédent / Parent / Fils
 * ================================================================ */
static Value fn_xml_premier(int argCount, Value* args) {
    (void)argCount;
    xmlNodePtr node = xml_coerce_to_node(args[0]);
    if (!node) return NIL_VAL;
    xmlNodePtr child = first_element_child(node);
    if (!child) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", child));
}

static Value fn_xml_dernier(int argCount, Value* args) {
    (void)argCount;
    xmlNodePtr node = xml_coerce_to_node(args[0]);
    if (!node) return NIL_VAL;
    xmlNodePtr child = last_element_child(node);
    if (!child) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", child));
}

static Value fn_xml_suivant(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return NIL_VAL;
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    xmlNodePtr next = next_element_sibling(node);
    if (!next) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", next));
}

static Value fn_xml_precedent(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return NIL_VAL;
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    xmlNodePtr prev = prev_element_sibling(node);
    if (!prev) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", prev));
}

static Value fn_xml_parent(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return NIL_VAL;
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node || !node->parent || node->parent->type == XML_DOCUMENT_NODE) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlNoeud", node->parent));
}

static Value fn_xml_fils(int argCount, Value* args) {
    (void)argCount;
    xmlNodePtr node = xml_coerce_to_node(args[0]);
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    if (node) {
        for (xmlNodePtr c = node->children; c; c = c->next) {
            if (c->type != XML_ELEMENT_NODE) continue;
            ObjInstance* child = xml_make_instance("xmlNoeud", c);
            obj_array_push(arr, OBJ_VAL(child));
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* ================================================================
 * XMLNomElément / XMLNomParent / XMLTypeElement
 * ================================================================ */
static Value fn_xml_nom_element(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return OBJ_VAL(obj_copy_string("", 0));
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node || !node->name) return OBJ_VAL(obj_copy_string("", 0));
    return xml_str(node->name);
}

static Value fn_xml_nom_parent(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return OBJ_VAL(obj_copy_string("", 0));
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node || !node->parent || node->parent->type == XML_DOCUMENT_NODE)
        return OBJ_VAL(obj_copy_string("", 0));
    return xml_str(node->parent->name);
}

static Value fn_xml_type_element(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return INTEGER_VAL(0);
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return INTEGER_VAL(0);
    return INTEGER_VAL(libxml2_type_to_owl(node->type));
}

/* ================================================================
 * XMLLit / XMLEcrit / XMLModifie / XMLRenomme
 * ================================================================ */
static Value fn_xml_lit(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) return OBJ_VAL(obj_copy_string("", 0));
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return OBJ_VAL(obj_copy_string("", 0));
    xmlChar* content = xmlNodeGetContent(node);
    if (!content) return OBJ_VAL(obj_copy_string("", 0));
    Value v = xml_str(content);
    xmlFree(content);
    return v;
}

static Value fn_xml_ecrit(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    if (!xml_is_type(args[0], "xmlNoeud")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLEcrit",
                              "native", "XMLEcrit",
                              "arg", "1",
                              "expected", "xmlNoeud",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    const char* val = IS_STRING(args[1]) ? AS_STRING(args[1])->chars : "";
    xmlNodeSetContent(node, (const xmlChar*)val);
    return NIL_VAL;
}

static Value fn_xml_modifie(int argCount, Value* args) {
    return fn_xml_ecrit(argCount, args);
}

static Value fn_xml_renomme(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) return NIL_VAL;
    if (!xml_is_type(args[0], "xmlNoeud")) return NIL_VAL;
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    xmlNodeSetName(node, (const xmlChar*)AS_STRING(args[1])->chars);
    return NIL_VAL;
}

/* ================================================================
 * XMLAjouteFils / XMLSupprime
 * ================================================================ */
static Value fn_xml_ajoute_fils(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLAjouteFils", "nom", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    xmlNodePtr parent = NULL;
    xmlDocPtr  doc    = NULL;
    if (xml_is_type(args[0], "xmlNoeud")) {
        parent = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
        if (parent) doc = parent->doc;
    } else if (xml_is_type(args[0], "xmlDocument")) {
        doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLAjouteFils",
                              "native", "XMLAjouteFils",
                              "arg", "1",
                              "expected", "xmlNoeud ou xmlDocument",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    const xmlChar* name    = (const xmlChar*)AS_STRING(args[1])->chars;
    const xmlChar* content = (argCount >= 3 && IS_STRING(args[2]))
                             ? (const xmlChar*)AS_STRING(args[2])->chars : NULL;
    xmlNodePtr newNode = xmlNewNode(NULL, name);
    if (!newNode) {
        owl_emit_native_error(E_NATIVE_XML_NODE_CREATE_FAILURE, "XMLAjouteFils",
                              "native", "XMLAjouteFils",
                              "detail", "libxml2 a refusé la création",
                              NULL);
        return NIL_VAL;
    }
    if (content) xmlNodeSetContent(newNode, content);
    if (parent) {
        xmlAddChild(parent, newNode);
    } else if (doc) {
        xmlDocSetRootElement(doc, newNode);
    }
    return OBJ_VAL(xml_make_instance("xmlNoeud", newNode));
}

static Value fn_xml_supprime(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNoeud")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLSupprime",
                              "native", "XMLSupprime",
                              "arg", "1",
                              "expected", "xmlNoeud",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (node) {
        xmlUnlinkNode(node);
        xmlFreeNode(node);
        xml_invalidate_ptr(AS_INSTANCE(args[0]));
    }
    return NIL_VAL;
}

/* ================================================================
 * XMLAjouteAttribut
 * args[0]: xmlNoeud, args[1]: nom, args[2]: valeur
 * ================================================================ */
static Value fn_xml_ajoute_attribut(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLAjouteAttribut", "nom", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    OWL_REQUIRE_ARG("XMLAjouteAttribut", "valeur", 2, IS_STRING(args[2]),
                    "chaîne", NIL_VAL);
    if (!xml_is_type(args[0], "xmlNoeud")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLAjouteAttribut",
                              "native", "XMLAjouteAttribut",
                              "arg", "1",
                              "expected", "xmlNoeud",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    xmlAttrPtr attr = xmlSetProp(node,
        (const xmlChar*)AS_STRING(args[1])->chars,
        (const xmlChar*)AS_STRING(args[2])->chars);
    if (!attr) return NIL_VAL;
    return OBJ_VAL(xml_make_instance("xmlAttribut", attr));
}

/* ================================================================
 * XMLFilsExiste
 * args[0]: xmlNoeud/xmlDocument, args[1]: nom
 * ================================================================ */
static Value fn_xml_fils_existe(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) return BOOL_VAL(false);
    xmlNodePtr node = xml_coerce_to_node(args[0]);
    if (!node) return BOOL_VAL(false);
    const xmlChar* name = (const xmlChar*)AS_STRING(args[1])->chars;
    for (xmlNodePtr c = node->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, name) == 0)
            return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
}

/* ================================================================
 * XMLExécuteXPath — nodeset ou chaîne
 * args[0]: xmlDocument, args[1]: xpath
 * ================================================================ */
static Value fn_xml_execute_xpath(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLExécuteXPath", "xpath", 1, IS_STRING(args[1]),
                    "chaîne", OBJ_VAL(obj_new_array()));
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLExécuteXPath",
                              "native", "XMLExécuteXPath",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_new_array());
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return OBJ_VAL(obj_new_array());

    owl_clear_error();
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) return OBJ_VAL(obj_new_array());

    const xmlChar* expr = (const xmlChar*)AS_STRING(args[1])->chars;
    xmlXPathObjectPtr result = xmlXPathEvalExpression(expr, ctx);
    xmlXPathFreeContext(ctx);

    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    if (result) {
        if (result->type == XPATH_NODESET && result->nodesetval) {
            xmlNodeSetPtr ns = result->nodesetval;
            for (int i = 0; i < ns->nodeNr; i++) {
                if (!ns->nodeTab[i]) continue;
                obj_array_push(arr,
                    OBJ_VAL(xml_make_instance("xmlNoeud", ns->nodeTab[i])));
            }
        } else if (result->type == XPATH_STRING) {
            obj_array_push(arr, xml_str(result->stringval));
        }
        xmlXPathFreeObject(result);
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* ================================================================
 * XMLExécuteXPathNS — XPath avec bindings de namespaces
 * args[0]: xmlDocument, args[1]: xpath
 * args[2]: tableau de préfixes (chaînes), args[3]: tableau d'URIs
 * ================================================================ */
static Value fn_xml_execute_xpath_ns(int argCount, Value* args) {
    if (argCount < 4 || !IS_STRING(args[1])) {
        /* Fallback sans NS */
        return fn_xml_execute_xpath(argCount, args);
    }
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLExécuteXPathNS",
                              "native", "XMLExécuteXPathNS",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_new_array());
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return OBJ_VAL(obj_new_array());

    owl_clear_error();
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) return OBJ_VAL(obj_new_array());

    /* Enregistre les namespaces */
    if (IS_ARRAY(args[2]) && IS_ARRAY(args[3])) {
        ObjArray* prefixes = AS_ARRAY(args[2]);
        ObjArray* uris     = AS_ARRAY(args[3]);
        int n = prefixes->elements.count < uris->elements.count
              ? prefixes->elements.count : uris->elements.count;
        for (int i = 0; i < n; i++) {
            Value pv = prefixes->elements.values[i];
            Value uv = uris->elements.values[i];
            if (IS_STRING(pv) && IS_STRING(uv)) {
                xmlXPathRegisterNs(ctx,
                    (const xmlChar*)AS_STRING(pv)->chars,
                    (const xmlChar*)AS_STRING(uv)->chars);
            }
        }
    }

    const xmlChar* expr = (const xmlChar*)AS_STRING(args[1])->chars;
    xmlXPathObjectPtr result = xmlXPathEvalExpression(expr, ctx);
    xmlXPathFreeContext(ctx);

    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    if (result) {
        if (result->type == XPATH_NODESET && result->nodesetval) {
            xmlNodeSetPtr ns = result->nodesetval;
            for (int i = 0; i < ns->nodeNr; i++) {
                if (!ns->nodeTab[i]) continue;
                obj_array_push(arr,
                    OBJ_VAL(xml_make_instance("xmlNoeud", ns->nodeTab[i])));
            }
        } else if (result->type == XPATH_STRING) {
            obj_array_push(arr, xml_str(result->stringval));
        }
        xmlXPathFreeObject(result);
    }
    vm_pop();
    return OBJ_VAL(arr);
}

/* ================================================================
 * XMLLitXPath — raccourci XPath → chaîne (valeur du premier nœud)
 * args[0]: xmlDocument, args[1]: xpath
 * ================================================================ */
static Value fn_xml_lit_xpath(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLLitXPath", "xpath", 1, IS_STRING(args[1]),
                    "chaîne", OBJ_VAL(obj_copy_string("", 0)));
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLLitXPath",
                              "native", "XMLLitXPath",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return OBJ_VAL(obj_copy_string("", 0));

    owl_clear_error();
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) return OBJ_VAL(obj_copy_string("", 0));

    const xmlChar* expr = (const xmlChar*)AS_STRING(args[1])->chars;
    xmlXPathObjectPtr result = xmlXPathEvalExpression(expr, ctx);
    xmlXPathFreeContext(ctx);

    Value out = OBJ_VAL(obj_copy_string("", 0));
    if (result) {
        if (result->type == XPATH_STRING && result->stringval) {
            out = xml_str(result->stringval);
        } else if (result->type == XPATH_NODESET && result->nodesetval
                   && result->nodesetval->nodeNr > 0) {
            xmlChar* content = xmlNodeGetContent(result->nodesetval->nodeTab[0]);
            if (content) { out = xml_str(content); xmlFree(content); }
        }
        xmlXPathFreeObject(result);
    }
    return out;
}

/* ================================================================
 * XMLEcritXPath — écrit/crée par XPath simple (/a/b/c)
 * args[0]: xmlDocument, args[1]: xpath, args[2]: valeur
 * ================================================================ */

/* Helper: cherche un nœud par XPath, retourne NULL si absent. */
static xmlNodePtr xpath_find_first(xmlDocPtr doc, const char* expr) {
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx) return NULL;
    xmlXPathObjectPtr res = xmlXPathEvalExpression((const xmlChar*)expr, ctx);
    xmlXPathFreeContext(ctx);
    if (!res) return NULL;
    xmlNodePtr node = NULL;
    if (res->type == XPATH_NODESET && res->nodesetval && res->nodesetval->nodeNr > 0)
        node = res->nodesetval->nodeTab[0];
    xmlXPathFreeObject(res);
    return node;
}

/* Helper: chemin simple /root/a/b/c → crée les nœuds manquants, retourne la feuille. */
static xmlNodePtr xml_ensure_simple_path(xmlDocPtr doc, const char* path) {
    /* Essaie d'abord de trouver le nœud */
    xmlNodePtr existing = xpath_find_first(doc, path);
    if (existing) return existing;

    /* Parse le chemin : /a/b/c → ["a","b","c"] */
    char buf[1024];
    strncpy(buf, path, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
    const char* parts[32];
    int nparts = 0;
    char* p = buf;
    if (*p == '/') p++;  /* skip leading slash */
    while (*p && nparts < 32) {
        /* Hors périmètre : prédicats, //, @, fonctions → abandon */
        if (*p == '[' || (*p == '/' && *(p+1) == '/') || *p == '@' || *p == '(')
            return NULL;
        parts[nparts++] = p;
        char* slash = strchr(p, '/');
        if (!slash) break;
        *slash = '\0';
        p = slash + 1;
    }
    if (nparts == 0) return NULL;

    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (!root) {
        root = xmlNewNode(NULL, (const xmlChar*)parts[0]);
        xmlDocSetRootElement(doc, root);
        xmlNodePtr cur = root;
        for (int i = 1; i < nparts; i++)
            cur = xmlNewChild(cur, NULL, (const xmlChar*)parts[i], NULL);
        return cur;
    }

    if (xmlStrcmp(root->name, (const xmlChar*)parts[0]) != 0) return NULL;

    xmlNodePtr cur = root;
    for (int i = 1; i < nparts; i++) {
        xmlNodePtr found = NULL;
        for (xmlNodePtr c = cur->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE &&
                xmlStrcmp(c->name, (const xmlChar*)parts[i]) == 0) {
                found = c; break;
            }
        }
        cur = found ? found : xmlNewChild(cur, NULL, (const xmlChar*)parts[i], NULL);
    }
    return cur;
}

static Value fn_xml_ecrit_xpath(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "XMLEcritXPath",
                              "native", "XMLEcritXPath",
                              "arg", "doc, xpath, valeur",
                              NULL);
        return BOOL_VAL(false);
    }
    if (!xml_is_type(args[0], "xmlDocument") || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLEcritXPath",
                              "native", "XMLEcritXPath",
                              "arg", "1 et 2",
                              "expected", "xmlDocument et chaîne",
                              "actual", "autres types",
                              NULL);
        return BOOL_VAL(false);
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return BOOL_VAL(false);

    const char* xpath = AS_STRING(args[1])->chars;
    const char* val   = IS_STRING(args[2]) ? AS_STRING(args[2])->chars : "";

    owl_clear_error();
    xmlNodePtr target = xpath_find_first(doc, xpath);
    if (!target) target = xml_ensure_simple_path(doc, xpath);
    if (!target) {
        owl_emit_native_error(E_NATIVE_NOT_FOUND, "XMLEcritXPath",
                              "native", "XMLEcritXPath",
                              "name", "nœud (chemin complexe non géré)",
                              NULL);
        return BOOL_VAL(false);
    }
    xmlNodeSetContent(target, (const xmlChar*)val);
    return BOOL_VAL(true);
}

/* ================================================================
 * XMLInsèreFils — insère un nouveau fils à une position donnée
 * args[0]: xmlNoeud ou xmlDocument (parent)
 * args[1]: nom (chaîne)
 * args[2]: position (entier, 1-based) — optionnel, 0/absent = append
 * ================================================================ */
static Value fn_xml_insere_fils(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLInsèreFils", "nom", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    xmlNodePtr parent = NULL;
    xmlDocPtr  doc    = NULL;
    if (xml_is_type(args[0], "xmlNoeud")) {
        parent = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
        if (parent) doc = parent->doc;
    } else if (xml_is_type(args[0], "xmlDocument")) {
        doc    = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
        parent = doc ? xmlDocGetRootElement(doc) : NULL;
    }
    if (!parent) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "XMLInsèreFils",
                              "native", "XMLInsèreFils",
                              NULL);
        return NIL_VAL;
    }

    int position = 0;
    if (argCount >= 3 && IS_INTEGER(args[2]))
        position = (int)AS_INTEGER(args[2]);

    xmlNodePtr newNode = xmlNewNode(NULL, (const xmlChar*)AS_STRING(args[1])->chars);
    if (!newNode) {
        owl_emit_native_error(E_NATIVE_XML_NODE_CREATE_FAILURE, "XMLInsèreFils",
                              "native", "XMLInsèreFils",
                              "detail", "libxml2 a refusé la création",
                              NULL);
        return NIL_VAL;
    }

    if (position <= 0) {
        xmlAddChild(parent, newNode);
    } else {
        int idx = 0;
        xmlNodePtr ref = NULL;
        for (xmlNodePtr c = parent->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE && ++idx == position) { ref = c; break; }
        }
        if (ref) xmlAddPrevSibling(ref, newNode);
        else     xmlAddChild(parent, newNode);
    }
    return OBJ_VAL(xml_make_instance("xmlNoeud", newNode));
}

/* ================================================================
 * XMLAjouteCommentaire / XMLAjouteCDATA
 * ================================================================ */
static Value fn_xml_ajoute_commentaire(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLAjouteCommentaire", "texte", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    xmlNodePtr parent = xml_coerce_to_node(args[0]);
    if (!parent) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLAjouteCommentaire",
                              "native", "XMLAjouteCommentaire",
                              "arg", "1",
                              "expected", "xmlNoeud ou xmlDocument",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    const xmlChar* txt = (const xmlChar*)AS_STRING(args[1])->chars;
    xmlNodePtr comment = xmlNewComment(txt);
    if (!comment) return NIL_VAL;
    xmlAddChild(parent, comment);
    return OBJ_VAL(xml_make_instance("xmlNoeud", comment));
}

static Value fn_xml_ajoute_cdata(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLAjouteCDATA", "texte", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    xmlNodePtr parent = xml_coerce_to_node(args[0]);
    if (!parent || !parent->doc) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLAjouteCDATA",
                              "native", "XMLAjouteCDATA",
                              "arg", "1",
                              "expected", "xmlNoeud ou xmlDocument",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    const char* txt = AS_STRING(args[1])->chars;
    xmlNodePtr cdata = xmlNewCDataBlock(parent->doc, (const xmlChar*)txt, (int)strlen(txt));
    if (!cdata) return NIL_VAL;
    xmlAddChild(parent, cdata);
    return OBJ_VAL(xml_make_instance("xmlNoeud", cdata));
}

/* ================================================================
 * Attributs : XMLLitAttribut / XMLListeAttributs / XMLAttributExiste
 * ================================================================ */
static Value fn_xml_lit_attribut(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLLitAttribut", "nom", 1, IS_STRING(args[1]),
                    "chaîne", OBJ_VAL(obj_copy_string("", 0)));
    if (!xml_is_type(args[0], "xmlNoeud")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLLitAttribut",
                              "native", "XMLLitAttribut",
                              "arg", "1",
                              "expected", "xmlNoeud",
                              "actual", "autre type",
                              NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return OBJ_VAL(obj_copy_string("", 0));
    owl_clear_error();
    xmlChar* val = xmlGetProp(node, (const xmlChar*)AS_STRING(args[1])->chars);
    if (!val) return OBJ_VAL(obj_copy_string("", 0));
    Value result = xml_str(val);
    xmlFree(val);
    return result;
}

static Value fn_xml_liste_attributs(int argCount, Value* args) {
    (void)argCount;
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    if (xml_is_type(args[0], "xmlNoeud")) {
        xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
        if (node) {
            for (xmlAttrPtr a = node->properties; a; a = a->next)
                obj_array_push(arr,
                    OBJ_VAL(xml_make_instance("xmlAttribut", a)));
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

static Value fn_xml_attribut_existe(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) return BOOL_VAL(false);
    if (!xml_is_type(args[0], "xmlNoeud"))    return BOOL_VAL(false);
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return BOOL_VAL(false);
    xmlChar* val = xmlGetProp(node, (const xmlChar*)AS_STRING(args[1])->chars);
    if (!val) return BOOL_VAL(false);
    xmlFree(val);
    return BOOL_VAL(true);
}

/* Méthodes sur xmlAttribut : nom et valeur */
static Value fn_xml_attribut_nom(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlAttribut")) return OBJ_VAL(obj_copy_string("", 0));
    xmlAttrPtr attr = (xmlAttrPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!attr || !attr->name) return OBJ_VAL(obj_copy_string("", 0));
    return xml_str(attr->name);
}

static Value fn_xml_attribut_valeur(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlAttribut")) return OBJ_VAL(obj_copy_string("", 0));
    xmlAttrPtr attr = (xmlAttrPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!attr) return OBJ_VAL(obj_copy_string("", 0));
    xmlChar* content = xmlNodeGetContent((xmlNodePtr)attr);
    if (!content) return OBJ_VAL(obj_copy_string("", 0));
    Value v = xml_str(content);
    xmlFree(content);
    return v;
}

/* ================================================================
 * Namespaces : XMLAjouteEspaceNom / XMLListeEspacesNom
 *              + méthodes sur xmlNamespace
 * ================================================================ */
static Value fn_xml_ajoute_espace_nom(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLAjouteEspaceNom", "préfixe", 1, IS_STRING(args[1]),
                    "chaîne", NIL_VAL);
    OWL_REQUIRE_ARG("XMLAjouteEspaceNom", "uri", 2, IS_STRING(args[2]),
                    "chaîne", NIL_VAL);
    if (!xml_is_type(args[0], "xmlNoeud")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLAjouteEspaceNom",
                              "native", "XMLAjouteEspaceNom",
                              "arg", "1",
                              "expected", "xmlNoeud",
                              "actual", "autre type",
                              NULL);
        return NIL_VAL;
    }
    xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!node) return NIL_VAL;
    const xmlChar* prefix = (const xmlChar*)AS_STRING(args[1])->chars;
    const xmlChar* uri    = (const xmlChar*)AS_STRING(args[2])->chars;
    /* préfixe vide = namespace par défaut */
    if (prefix[0] == '\0') prefix = NULL;
    xmlNsPtr ns = xmlNewNs(node, uri, prefix);
    if (!ns) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "XMLAjouteEspaceNom",
                              "native", "XMLAjouteEspaceNom",
                              NULL);
        return NIL_VAL;
    }
    return OBJ_VAL(xml_make_instance("xmlNamespace", ns));
}

static Value fn_xml_liste_espaces_nom(int argCount, Value* args) {
    (void)argCount;
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    if (xml_is_type(args[0], "xmlNoeud")) {
        xmlNodePtr node = (xmlNodePtr)xml_get_ptr(AS_INSTANCE(args[0]));
        if (node) {
            for (xmlNsPtr ns = node->nsDef; ns; ns = ns->next)
                obj_array_push(arr,
                    OBJ_VAL(xml_make_instance("xmlNamespace", ns)));
        }
    }
    vm_pop();
    return OBJ_VAL(arr);
}

static Value fn_xml_namespace_prefixe(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNamespace")) return OBJ_VAL(obj_copy_string("", 0));
    xmlNsPtr ns = (xmlNsPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!ns || !ns->prefix) return OBJ_VAL(obj_copy_string("", 0));
    return xml_str(ns->prefix);
}

static Value fn_xml_namespace_uri(int argCount, Value* args) {
    (void)argCount;
    if (!xml_is_type(args[0], "xmlNamespace")) return OBJ_VAL(obj_copy_string("", 0));
    xmlNsPtr ns = (xmlNsPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!ns || !ns->href) return OBJ_VAL(obj_copy_string("", 0));
    return xml_str(ns->href);
}

/* ================================================================
 * XMLDocumentValide — validation XSD
 * args[0]: xmlDocument, args[1]: chemin vers le fichier XSD
 * ================================================================ */
static Value fn_xml_document_valide(int argCount, Value* args) {
    OWL_REQUIRE_ARG("XMLDocumentValide", "cheminXSD", 1, IS_STRING(args[1]),
                    "chaîne", BOOL_VAL(false));
    if (!xml_is_type(args[0], "xmlDocument")) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "XMLDocumentValide",
                              "native", "XMLDocumentValide",
                              "arg", "1",
                              "expected", "xmlDocument",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    xmlDocPtr doc = (xmlDocPtr)xml_get_ptr(AS_INSTANCE(args[0]));
    if (!doc) return BOOL_VAL(false);

    owl_clear_error();
    const char* schemaPath = AS_STRING(args[1])->chars;

    xmlSchemaParserCtxtPtr pctxt = xmlSchemaNewParserCtxt(schemaPath);
    if (!pctxt) {
        owl_emit_native_error(E_NATIVE_XML_SCHEMA_ERROR, "XMLDocumentValide",
                              "native", "XMLDocumentValide",
                              "detail", "impossible de charger le schéma XSD",
                              NULL);
        return BOOL_VAL(false);
    }
    xmlSchemaPtr schema = xmlSchemaParse(pctxt);
    xmlSchemaFreeParserCtxt(pctxt);
    if (!schema) {
        owl_emit_native_error(E_NATIVE_XML_SCHEMA_ERROR, "XMLDocumentValide",
                              "native", "XMLDocumentValide",
                              "detail", "schéma XSD invalide",
                              NULL);
        return BOOL_VAL(false);
    }

    xmlSchemaValidCtxtPtr vctxt = xmlSchemaNewValidCtxt(schema);
    if (!vctxt) { xmlSchemaFree(schema); return BOOL_VAL(false); }

    int ret = xmlSchemaValidateDoc(vctxt, doc);
    xmlSchemaFreeValidCtxt(vctxt);
    xmlSchemaFree(schema);

    if (ret != 0) {
        owl_emit_native_error(E_NATIVE_XML_SCHEMA_ERROR, "XMLDocumentValide",
                              "native", "XMLDocumentValide",
                              "detail", "document non conforme au schéma",
                              NULL);
    }
    return BOOL_VAL(ret == 0);
}

/* ================================================================
 * TexteVersXML / XMLVersTexte — escape / unescape entités
 * ================================================================ */
static Value fn_texte_vers_xml(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return args[0];
    const char* s = AS_STRING(args[0])->chars;
    int len = (int)strlen(s);
    int outLen = 0;
    for (int i = 0; i < len; i++) {
        switch ((unsigned char)s[i]) {
            case '&':  outLen += 5; break;
            case '<':  outLen += 4; break;
            case '>':  outLen += 4; break;
            case '"':  outLen += 6; break;
            case '\'': outLen += 6; break;
            default:   outLen += 1; break;
        }
    }
    char* buf = ALLOCATE(char, outLen + 1);
    char* dst = buf;
    for (int i = 0; i < len; i++) {
        switch ((unsigned char)s[i]) {
            case '&':  memcpy(dst, "&amp;",  5); dst += 5; break;
            case '<':  memcpy(dst, "&lt;",   4); dst += 4; break;
            case '>':  memcpy(dst, "&gt;",   4); dst += 4; break;
            case '"':  memcpy(dst, "&quot;", 6); dst += 6; break;
            case '\'': memcpy(dst, "&apos;", 6); dst += 6; break;
            default:   *dst++ = s[i]; break;
        }
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, outLen));
}

static Value fn_xml_vers_texte(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return args[0];
    const char* s = AS_STRING(args[0])->chars;
    int len = (int)strlen(s);
    char* buf = ALLOCATE(char, len + 1);
    char* dst = buf;
    const char* p = s;
    while (*p) {
        if (*p == '&') {
            if      (strncmp(p, "&amp;",  5) == 0) { *dst++ = '&';  p += 5; }
            else if (strncmp(p, "&lt;",   4) == 0) { *dst++ = '<';  p += 4; }
            else if (strncmp(p, "&gt;",   4) == 0) { *dst++ = '>';  p += 4; }
            else if (strncmp(p, "&quot;", 6) == 0) { *dst++ = '"';  p += 6; }
            else if (strncmp(p, "&apos;", 6) == 0) { *dst++ = '\''; p += 6; }
            else if (strncmp(p, "&#", 2)    == 0) {
                p += 2;
                int cp = 0;
                if (*p == 'x' || *p == 'X') {
                    p++;
                    while (*p && *p != ';') {
                        cp = cp * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
                        p++;
                    }
                } else {
                    while (*p && *p != ';') { cp = cp * 10 + (*p - '0'); p++; }
                }
                if (*p == ';') p++;
                if (cp < 0x80) {
                    *dst++ = (char)cp;
                } else if (cp < 0x800) {
                    *dst++ = (char)(0xC0 | (cp >> 6));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                } else {
                    *dst++ = (char)(0xE0 | (cp >> 12));
                    *dst++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                    *dst++ = (char)(0x80 | (cp & 0x3F));
                }
            } else {
                *dst++ = *p++;
            }
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    return OBJ_VAL(obj_take_string(buf, (int)(dst - buf)));
}

/* ================================================================
 * INIT
 * ================================================================ */
void stdlib_xml_init(void) {
    xmlInitParser();

    /* -- Paramètres réutilisables -- */
    static const OwlParam p_doc[]         = { {"doc","xmlDocument","document XML"}, {0} };
    static const OwlParam p_doc_path[]    = { {"doc","xmlDocument","document XML"}, {"chemin","cha\xc3\xae" "ne","chemin du fichier"}, {0} };
    static const OwlParam p_src_mode[]    = { {"source","cha\xc3\xae" "ne","fichier, URL ou cha\xc3\xae" "ne XML"}, {"mode","entier","depuisFichier/depuisCha\xc3\xae" "ne/depuisURL (optionnel)", .allowedBase=&E_xmlSourceMode}, {0} };
    static const OwlParam p_node[]        = { {"noeud","xmlNoeud","n\xc5\x93ud XML"}, {0} };
    static const OwlParam p_node_s[]      = { {"noeud","xmlNoeud","n\xc5\x93ud XML"}, {"valeur","cha\xc3\xae" "ne","valeur"}, {0} };
    static const OwlParam p_nodeordoc[]   = { {"parent","xmlNoeud ou xmlDocument","parent"}, {0} };
    static const OwlParam p_parent_name[] = { {"parent","xmlNoeud ou xmlDocument","parent"}, {"nom","cha\xc3\xae" "ne","nom du fils"}, {0} };
    static const OwlParam p_parent_add[]  = { {"parent","xmlNoeud ou xmlDocument","parent"}, {"nom","cha\xc3\xae" "ne","nom"}, {"contenu","cha\xc3\xae" "ne","contenu texte (optionnel)"}, {0} };
    static const OwlParam p_parent_pos[]  = { {"parent","xmlNoeud ou xmlDocument","parent"}, {"nom","cha\xc3\xae" "ne","nom"}, {"position","entier","1=avant le 1er fils, 0=dernier (optionnel)"}, {0} };
    static const OwlParam p_parent_txt[]  = { {"parent","xmlNoeud ou xmlDocument","parent"}, {"texte","cha\xc3\xae" "ne","contenu"}, {0} };
    static const OwlParam p_node_attr[]   = { {"noeud","xmlNoeud","n\xc5\x93ud"}, {"nom","cha\xc3\xae" "ne","nom attribut"}, {"valeur","cha\xc3\xae" "ne","valeur"}, {0} };
    static const OwlParam p_node_attrrd[] = { {"noeud","xmlNoeud","n\xc5\x93ud"}, {"nom","cha\xc3\xae" "ne","nom attribut"}, {0} };
    static const OwlParam p_doc_xpath[]   = { {"doc","xmlDocument","document XML"}, {"xpath","cha\xc3\xae" "ne","expression XPath"}, {0} };
    static const OwlParam p_doc_xpath_v[] = { {"doc","xmlDocument","document XML"}, {"xpath","cha\xc3\xae" "ne","expression XPath"}, {"valeur","cha\xc3\xae" "ne","valeur \xc3\xa0 \xc3\xa9" "crire"}, {0} };
    static const OwlParam p_doc_xpath_ns[]= { {"doc","xmlDocument","document XML"}, {"xpath","cha\xc3\xae" "ne","expression XPath"}, {"prefixes","tableau","tableau de pr\xc3\xa9" "fixes"}, {"uris","tableau","tableau d'URIs"}, {0} };
    static const OwlParam p_doc_xsd[]     = { {"doc","xmlDocument","document XML"}, {"xsd","cha\xc3\xae" "ne","chemin du fichier XSD"}, {0} };
    static const OwlParam p_node_ns[]     = { {"noeud","xmlNoeud","n\xc5\x93ud"}, {"prefixe","cha\xc3\xae" "ne","pr\xc3\xa9" "fixe (vide = namespace par d\xc3\xa9" "faut)"}, {"uri","cha\xc3\xae" "ne","URI du namespace"}, {0} };
    static const OwlParam p_attr[]        = { {"attribut","xmlAttribut","attribut XML"}, {0} };
    static const OwlParam p_ns[]          = { {"ns","xmlNamespace","namespace XML"}, {0} };
    static const OwlParam p_s[]           = { {"s","cha\xc3\xae" "ne","cha\xc3\xae" "ne source"}, {0} };
    static const OwlParam p_ver[]         = { {"version","cha\xc3\xae" "ne","version XML (\"1.0\" ou \"1.1\")"}, {0} };
    /* Listes dediees pour les formes discretes (chaque arite a sa liste). */
    static const OwlParam p_src[]         = { {"source","cha\xc3\xae" "ne","fichier, URL ou cha\xc3\xae" "ne XML"}, {0} };
    static const OwlParam p_parent_nom2[] = { {"parent","xmlNoeud ou xmlDocument","parent"}, {"nom","cha\xc3\xae" "ne","nom"}, {0} };

    /* -- Signatures -- */
    /* XMLOuvre(source [, mode]) : source obligatoire, mode optionnel -> formes 1..2. */
    static const OwlSignature sig_open[]      = {
        {1, p_src,      "xmlDocument","document ouvert ou NIL si \xc3\xa9" "chec"},
        {2, p_src_mode, "xmlDocument","document ouvert ou NIL si \xc3\xa9" "chec"},
        {0} };
    /* XMLDocument([version]) : version optionnelle -> formes 0..1. */
    static const OwlSignature sig_new[]       = {
        {0, NULL,  "xmlDocument","document XML vide"},
        {1, p_ver, "xmlDocument","document XML vide"},
        {0} };
    static const OwlSignature sig_close[]     = { {1,  p_doc,         "rien",       "lib\xc3\xa8re le document"}, {0} };
    static const OwlSignature sig_tostr[]     = { {1,  p_doc,         "cha\xc3\xae" "ne","contenu XML s\xc3\xa9rialis\xc3\xa9"}, {0} };
    static const OwlSignature sig_save[]      = { {2,  p_doc_path,    "bool\xc3\xa9" "en","Vrai si succ\xc3\xa8s"}, {0} };
    static const OwlSignature sig_node1doc[]  = { {1,  p_doc,         "xmlNoeud",   "n\xc5\x93ud ou NIL"}, {0} };
    static const OwlSignature sig_node1any[]  = { {1,  p_nodeordoc,   "xmlNoeud",   "n\xc5\x93ud ou NIL"}, {0} };
    static const OwlSignature sig_nav[]       = { {1,  p_node,        "xmlNoeud",   "n\xc5\x93ud voisin ou NIL"}, {0} };
    static const OwlSignature sig_children[]  = { {1,  p_nodeordoc,   "tableau",    "tableau de xmlNoeud"}, {0} };
    static const OwlSignature sig_name[]      = { {1,  p_node,        "cha\xc3\xae" "ne","nom du n\xc5\x93ud"}, {0} };
    static const OwlSignature sig_type[]      = { {1,  p_node,        "entier",     "type : XMLBalise=1, XMLTexte=2, XMLCDATA=3"}, {0} };
    static const OwlSignature sig_read[]      = { {1,  p_node,        "cha\xc3\xae" "ne","contenu texte"}, {0} };
    static const OwlSignature sig_write[]     = { {2,  p_node_s,      "rien",       "\xc3\xa9" "crit le contenu texte"}, {0} };
    static const OwlSignature sig_rename[]    = { {2,  p_node_s,      "rien",       "renomme le n\xc5\x93ud"}, {0} };
    /* XMLAjouteFils(parent, nom [, contenu]) : contenu optionnel -> formes 2..3. */
    static const OwlSignature sig_addch[]     = {
        {2, p_parent_nom2, "xmlNoeud", "nouveau n\xc5\x93ud fils"},
        {3, p_parent_add,  "xmlNoeud", "nouveau n\xc5\x93ud fils"},
        {0} };
    /* XMLIns\xc3\xa8reFils(parent, nom [, position]) : position optionnelle -> formes 2..3. */
    static const OwlSignature sig_insrtch[]   = {
        {2, p_parent_nom2, "xmlNoeud", "nouveau n\xc5\x93ud ins\xc3\xa9r\xc3\xa9"},
        {3, p_parent_pos,  "xmlNoeud", "nouveau n\xc5\x93ud ins\xc3\xa9r\xc3\xa9"},
        {0} };
    static const OwlSignature sig_del[]       = { {1,  p_node,        "rien",       "supprime et lib\xc3\xa8re le n\xc5\x93ud"}, {0} };
    static const OwlSignature sig_addtxt[]    = { {2,  p_parent_txt,  "xmlNoeud",   "n\xc5\x93ud cr\xc3\xa9\xc3\xa9"}, {0} };
    static const OwlSignature sig_attr[]      = { {3,  p_node_attr,   "xmlAttribut","attribut cr\xc3\xa9\xc3\xa9 ou modifi\xc3\xa9"}, {0} };
    static const OwlSignature sig_attrrd[]    = { {2,  p_node_attrrd, "cha\xc3\xae" "ne","valeur de l'attribut ou \"\" si absent"}, {0} };
    static const OwlSignature sig_attrls[]    = { {1,  p_node,        "tableau",    "tableau de xmlAttribut"}, {0} };
    static const OwlSignature sig_attrex[]    = { {2,  p_node_attrrd, "bool\xc3\xa9" "en","Vrai si l'attribut existe"}, {0} };
    static const OwlSignature sig_attrnom[]   = { {1,  p_attr,        "cha\xc3\xae" "ne","nom de l'attribut"}, {0} };
    static const OwlSignature sig_attrval[]   = { {1,  p_attr,        "cha\xc3\xae" "ne","valeur de l'attribut"}, {0} };
    static const OwlSignature sig_childex[]   = { {2,  p_parent_name, "bool\xc3\xa9" "en","Vrai si un fils de ce nom existe"}, {0} };
    static const OwlSignature sig_xpath[]     = { {2,  p_doc_xpath,   "tableau",    "tableau de xmlNoeud correspondants"}, {0} };
    static const OwlSignature sig_xpathns[]   = { {4,  p_doc_xpath_ns,"tableau",    "tableau de xmlNoeud (avec namespaces)"}, {0} };
    static const OwlSignature sig_xpathstr[]  = { {2,  p_doc_xpath,   "cha\xc3\xae" "ne","valeur texte du premier r\xc3\xa9sultat"}, {0} };
    static const OwlSignature sig_xpathwrt[]  = { {3,  p_doc_xpath_v, "bool\xc3\xa9" "en","Vrai si \xc3\xa9" "criture OK"}, {0} };
    static const OwlSignature sig_xsd[]       = { {2,  p_doc_xsd,     "bool\xc3\xa9" "en","Vrai si le document est valide"}, {0} };
    static const OwlSignature sig_addns[]     = { {3,  p_node_ns,     "xmlNamespace","namespace cr\xc3\xa9\xc3\xa9"}, {0} };
    static const OwlSignature sig_listns[]    = { {1,  p_node,        "tableau",    "tableau de xmlNamespace"}, {0} };
    static const OwlSignature sig_nspref[]    = { {1,  p_ns,          "cha\xc3\xae" "ne","pr\xc3\xa9" "fixe du namespace"}, {0} };
    static const OwlSignature sig_nsuri[]     = { {1,  p_ns,          "cha\xc3\xae" "ne","URI du namespace"}, {0} };
    static const OwlSignature sig_escape[]    = { {1,  p_s,           "cha\xc3\xae" "ne","cha\xc3\xae" "ne avec entit\xc3\xa9s XML \xc3\xa9" "chapp\xc3\xa9" "es"}, {0} };
    static const OwlSignature sig_unescape[]  = { {1,  p_s,           "cha\xc3\xae" "ne","cha\xc3\xae" "ne avec entit\xc3\xa9s XML d\xc3\xa9" "cod\xc3\xa9" "es"}, {0} };

    static const OwlNativeDef DEFS[] = {
        /* --- Documents --- */
        { .canonical="XMLOuvre",      .canonicalAscii="XMLOuvre",
          .nameEN="XMLOpen",          .category="XML",
          .descFR="Charge un document XML depuis un fichier, une URL ou une cha\xc3\xae" "ne.",
          .descEN="Open an XML document from a file, URL or string.",
          .signatures=sig_open,       .impl=fn_xml_ouvre },

        { .canonical="XMLDocument",   .canonicalAscii="XMLDocument",
          .nameEN="XMLDocument",      .category="XML",
          .descFR="Cr\xc3\xa9\xc3\xa9 un document XML vide.",
          .descEN="Create an empty XML document.",
          .signatures=sig_new,        .impl=fn_xml_document },

        { .canonical="XMLTermine",    .canonicalAscii="XMLTermine",
          .nameEN="XMLClose",         .category="XML",
          .descFR="Lib\xc3\xa8re un document XML de la m\xc3\xa9moire.",
          .descEN="Release an XML document from memory.",
          .signatures=sig_close,      .impl=fn_xml_termine },

        { .canonical="XMLConstruitCha\xc3\xae" "ne", .canonicalAscii="XMLConstruitChaine",
          .nameEN="XMLBuildString",   .category="XML",
          .descFR="S\xc3\xa9rialise un document XML en cha\xc3\xae" "ne UTF-8.",
          .descEN="Serialize an XML document to a UTF-8 string.",
          .signatures=sig_tostr,      .impl=fn_xml_construit_chaine },

        { .canonical="XMLSauve",      .canonicalAscii="XMLSauve",
          .nameEN="XMLSave",          .category="XML",
          .descFR="Sauvegarde un document XML dans un fichier.",
          .descEN="Save an XML document to a file.",
          .signatures=sig_save,       .impl=fn_xml_sauve },

        { .canonical="XMLDocumentValide", .canonicalAscii="XMLDocumentValide",
          .nameEN="XMLDocumentValid", .category="XML",
          .descFR="Valide un document XML contre un sch\xc3\xa9ma XSD. ErreurD\xc3\xa9" "tect\xc3\xa9" "e si invalide.",
          .descEN="Validate an XML document against an XSD schema. ErrorOccurred if invalid.",
          .signatures=sig_xsd,        .impl=fn_xml_document_valide },

        /* --- Navigation --- */
        { .canonical="XMLRacine",     .canonicalAscii="XMLRacine",
          .nameEN="XMLRoot",          .category="XML",
          .descFR="Retourne le n\xc5\x93ud racine du document.",
          .descEN="Return the root element node of the document.",
          .signatures=sig_node1doc,   .impl=fn_xml_racine },

        { .canonical="XMLPremier",    .canonicalAscii="XMLPremier",
          .nameEN="XMLFirst",         .category="XML",
          .descFR="Retourne le premier fils \xc3\xa9l\xc3\xa9ment d'un n\xc5\x93ud ou document.",
          .descEN="Return the first element child of a node or document.",
          .signatures=sig_node1any,   .impl=fn_xml_premier },

        { .canonical="XMLDernier",    .canonicalAscii="XMLDernier",
          .nameEN="XMLLast",          .category="XML",
          .descFR="Retourne le dernier fils \xc3\xa9l\xc3\xa9ment d'un n\xc5\x93ud ou document.",
          .descEN="Return the last element child of a node or document.",
          .signatures=sig_node1any,   .impl=fn_xml_dernier },

        { .canonical="XMLSuivant",    .canonicalAscii="XMLSuivant",
          .nameEN="XMLNext",          .category="XML",
          .descFR="Retourne le n\xc5\x93ud fr\xc3\xa8re \xc3\xa9l\xc3\xa9ment suivant.",
          .descEN="Return the next element sibling node.",
          .signatures=sig_nav,        .impl=fn_xml_suivant },

        { .canonical="XMLPr\xc3\xa9" "c\xc3\xa9" "dent", .canonicalAscii="XMLPrecedent",
          .nameEN="XMLPrevious",      .category="XML",
          .descFR="Retourne le n\xc5\x93ud fr\xc3\xa8re \xc3\xa9l\xc3\xa9ment pr\xc3\xa9" "c\xc3\xa9" "dent.",
          .descEN="Return the previous element sibling node.",
          .signatures=sig_nav,        .impl=fn_xml_precedent },

        { .canonical="XMLParent",     .canonicalAscii="XMLParent",
          .nameEN="XMLParent",        .category="XML",
          .descFR="Retourne le n\xc5\x93ud parent.",
          .descEN="Return the parent node.",
          .signatures=sig_nav,        .impl=fn_xml_parent },

        { .canonical="XMLFils",       .canonicalAscii="XMLFils",
          .nameEN="XMLChild",         .category="XML",
          .descFR="Retourne un tableau de tous les fils \xc3\xa9l\xc3\xa9ments.",
          .descEN="Return an array of all element children.",
          .signatures=sig_children,   .impl=fn_xml_fils },

        /* --- Propriétés de nœud --- */
        { .canonical="XMLNomEl\xc3\xa9" "ment", .canonicalAscii="XMLNomElement",
          .nameEN="XMLElementName",   .category="XML",
          .descFR="Retourne le nom de l'\xc3\xa9l\xc3\xa9ment (balise).",
          .descEN="Return the element (tag) name.",
          .signatures=sig_name,       .impl=fn_xml_nom_element },

        { .canonical="XMLNomParent",  .canonicalAscii="XMLNomParent",
          .nameEN="XMLParentName",    .category="XML",
          .descFR="Retourne le nom du n\xc5\x93ud parent.",
          .descEN="Return the parent node name.",
          .signatures=sig_name,       .impl=fn_xml_nom_parent },

        { .canonical="XMLTypeEl\xc3\xa9" "ment", .canonicalAscii="XMLTypeElement",
          .nameEN="XMLElementType",   .category="XML",
          .descFR="Retourne le type du n\xc5\x93ud (XMLBalise=1, XMLTexte=2, XMLCDATA=3, XMLCommentaire=4, XMLBalisePI=5).",
          .descEN="Return the node type (XMLTag=1, XMLText=2, XMLCDATA=3, XMLComment=4, XMLPI=5).",
          .signatures=sig_type,       .impl=fn_xml_type_element },

        /* --- Lecture / Écriture de contenu --- */
        { .canonical="XMLLit",        .canonicalAscii="XMLLit",
          .nameEN="XMLRead",          .category="XML",
          .descFR="Lit le contenu texte d'un n\xc5\x93ud.",
          .descEN="Read the text content of a node.",
          .signatures=sig_read,       .impl=fn_xml_lit },

        { .canonical="XMLEcrit",      .canonicalAscii="XMLEcrit",
          .nameEN="XMLWrite",         .category="XML",
          .descFR="\xc3\x89" "crit le contenu texte d'un n\xc5\x93ud.",
          .descEN="Write the text content of a node.",
          .signatures=sig_write,      .impl=fn_xml_ecrit },

        { .canonical="XMLModifie",    .canonicalAscii="XMLModifie",
          .nameEN="XMLModify",        .category="XML",
          .descFR="Modifie le contenu texte d'un n\xc5\x93ud (alias de XMLEcrit).",
          .descEN="Modify the text content of a node (alias for XMLWrite).",
          .signatures=sig_write,      .impl=fn_xml_modifie },

        { .canonical="XMLRenomme",    .canonicalAscii="XMLRenomme",
          .nameEN="XMLRename",        .category="XML",
          .descFR="Renomme un n\xc5\x93ud.",
          .descEN="Rename a node.",
          .signatures=sig_rename,     .impl=fn_xml_renomme },

        /* --- Structure --- */
        { .canonical="XMLAjouteFils", .canonicalAscii="XMLAjouteFils",
          .nameEN="XMLAddChild",      .category="XML",
          .descFR="Ajoute un n\xc5\x93ud fils en derni\xc3\xa8re position.",
          .descEN="Add a child node at last position.",
          .signatures=sig_addch,      .impl=fn_xml_ajoute_fils },

        { .canonical="XMLIns\xc3\xa8reFils", .canonicalAscii="XMLInsereFils",
          .nameEN="XMLInsertChild",   .category="XML",
          .descFR="Ins\xc3\xa8re un fils \xc3\xa0 une position donn\xc3\xa9" "e (1=avant le 1er fils, 0=dernier).",
          .descEN="Insert a child at a given position (1=before first child, 0=last).",
          .signatures=sig_insrtch,    .impl=fn_xml_insere_fils },

        { .canonical="XMLSupprime",   .canonicalAscii="XMLSupprime",
          .nameEN="XMLDelete",        .category="XML",
          .descFR="Supprime et lib\xc3\xa8re un n\xc5\x93ud.",
          .descEN="Remove and free a node.",
          .signatures=sig_del,        .impl=fn_xml_supprime },

        { .canonical="XMLAjouteCommentaire", .canonicalAscii="XMLAjouteCommentaire",
          .nameEN="XMLAddComment",    .category="XML",
          .descFR="Ajoute un n\xc5\x93ud commentaire (<!-- ... -->).",
          .descEN="Add a comment node (<!-- ... -->).",
          .signatures=sig_addtxt,     .impl=fn_xml_ajoute_commentaire },

        { .canonical="XMLAjouteCDATA", .canonicalAscii="XMLAjouteCDATA",
          .nameEN="XMLAddCDATA",      .category="XML",
          .descFR="Ajoute une section CDATA (<![CDATA[...]]).",
          .descEN="Add a CDATA section (<![CDATA[...]]).",
          .signatures=sig_addtxt,     .impl=fn_xml_ajoute_cdata },

        { .canonical="XMLFilsExiste", .canonicalAscii="XMLFilsExiste",
          .nameEN="XMLChildExist",    .category="XML",
          .descFR="Retourne Vrai si un fils de ce nom existe.",
          .descEN="Return True if a child with this name exists.",
          .signatures=sig_childex,    .impl=fn_xml_fils_existe },

        /* --- Attributs --- */
        { .canonical="XMLAjouteAttribut", .canonicalAscii="XMLAjouteAttribut",
          .nameEN="XMLAddAttribute",  .category="XML",
          .descFR="Ajoute ou modifie un attribut sur un n\xc5\x93ud.",
          .descEN="Add or modify an attribute on a node.",
          .signatures=sig_attr,       .impl=fn_xml_ajoute_attribut },

        { .canonical="XMLLitAttribut", .canonicalAscii="XMLLitAttribut",
          .nameEN="XMLReadAttribute", .category="XML",
          .descFR="Lit la valeur d'un attribut. Retourne \"\" si absent.",
          .descEN="Read an attribute value. Returns \"\" if absent.",
          .signatures=sig_attrrd,     .impl=fn_xml_lit_attribut },

        { .canonical="XMLListeAttributs", .canonicalAscii="XMLListeAttributs",
          .nameEN="XMLListAttributes",.category="XML",
          .descFR="Retourne un tableau de tous les xmlAttribut du n\xc5\x93ud.",
          .descEN="Return an array of all xmlAttribut of the node.",
          .signatures=sig_attrls,     .impl=fn_xml_liste_attributs },

        { .canonical="XMLAttributExiste", .canonicalAscii="XMLAttributExiste",
          .nameEN="XMLAttributeExist",.category="XML",
          .descFR="Retourne Vrai si l'attribut de ce nom existe sur le n\xc5\x93ud.",
          .descEN="Return True if an attribute with this name exists on the node.",
          .signatures=sig_attrex,     .impl=fn_xml_attribut_existe },

        { .canonical="XMLAttributNom", .canonicalAscii="XMLAttributNom",
          .nameEN="XMLAttributeName", .category="XML",
          .descFR="Retourne le nom d'un xmlAttribut.",
          .descEN="Return the name of an xmlAttribut.",
          .signatures=sig_attrnom,    .impl=fn_xml_attribut_nom },

        { .canonical="XMLAttributValeur", .canonicalAscii="XMLAttributValeur",
          .nameEN="XMLAttributeValue",.category="XML",
          .descFR="Retourne la valeur d'un xmlAttribut.",
          .descEN="Return the value of an xmlAttribut.",
          .signatures=sig_attrval,    .impl=fn_xml_attribut_valeur },

        /* --- XPath --- */
        { .canonical="XMLEx\xc3\xa9" "cuteXPath", .canonicalAscii="XMLExecuteXPath",
          .nameEN="XMLExecuteXPath",  .category="XML",
          .descFR="Ex\xc3\xa9" "cute une requ\xc3\xaa" "te XPath. Retourne un tableau de xmlNoeud.",
          .descEN="Execute an XPath query. Returns an array of xmlNode.",
          .signatures=sig_xpath,      .impl=fn_xml_execute_xpath },

        { .canonical="XMLEx\xc3\xa9" "cuteXPathNS", .canonicalAscii="XMLExecuteXPathNS",
          .nameEN="XMLExecuteXPathNS",.category="XML",
          .descFR="Ex\xc3\xa9" "cute XPath avec bindings de namespaces (tableaux pr\xc3\xa9" "fixes + URIs).",
          .descEN="Execute XPath with namespace bindings (prefix array + URI array).",
          .signatures=sig_xpathns,    .impl=fn_xml_execute_xpath_ns },

        { .canonical="XMLLitXPath",   .canonicalAscii="XMLLitXPath",
          .nameEN="XMLReadXPath",     .category="XML",
          .descFR="Raccourci : \xc3\xa9value un XPath et retourne la valeur texte du premier r\xc3\xa9sultat.",
          .descEN="Shortcut: evaluate XPath and return the text value of the first result.",
          .signatures=sig_xpathstr,   .impl=fn_xml_lit_xpath },

        { .canonical="XMLEcritXPath", .canonicalAscii="XMLEcritXPath",
          .nameEN="XMLWriteXPath",    .category="XML",
          .descFR="\xc3\x89" "crit une valeur \xc3\xa0 un chemin XPath simple, cr\xc3\xa9\xc3\xa9 les n\xc5\x93uds manquants.",
          .descEN="Write a value to a simple XPath, creating missing nodes.",
          .signatures=sig_xpathwrt,   .impl=fn_xml_ecrit_xpath },

        /* --- Namespaces --- */
        { .canonical="XMLAjouteEspaceNom", .canonicalAscii="XMLAjouteEspaceNom",
          .nameEN="XMLAddNamespace",  .category="XML",
          .descFR="D\xc3\xa9" "clare un namespace sur un n\xc5\x93ud (pr\xc3\xa9" "fixe + URI).",
          .descEN="Declare a namespace on a node (prefix + URI).",
          .signatures=sig_addns,      .impl=fn_xml_ajoute_espace_nom },

        { .canonical="XMLListeEspacesNom", .canonicalAscii="XMLListeEspacesNom",
          .nameEN="XMLListNamespaces",.category="XML",
          .descFR="Retourne un tableau de xmlNamespace d\xc3\xa9" "clar\xc3\xa9s sur un n\xc5\x93ud.",
          .descEN="Return an array of xmlNamespace declared on a node.",
          .signatures=sig_listns,     .impl=fn_xml_liste_espaces_nom },

        { .canonical="XMLEspaceNomPr\xc3\xa9" "fixe", .canonicalAscii="XMLEspaceNomPrefixe",
          .nameEN="XMLNamespacePrefix",.category="XML",
          .descFR="Retourne le pr\xc3\xa9" "fixe d'un xmlNamespace.",
          .descEN="Return the prefix of an xmlNamespace.",
          .signatures=sig_nspref,     .impl=fn_xml_namespace_prefixe },

        { .canonical="XMLEspaceNomURI",  .canonicalAscii="XMLEspaceNomURI",
          .nameEN="XMLNamespaceURI",  .category="XML",
          .descFR="Retourne l'URI d'un xmlNamespace.",
          .descEN="Return the URI of an xmlNamespace.",
          .signatures=sig_nsuri,      .impl=fn_xml_namespace_uri },

        /* --- Entités --- */
        { .canonical="TexteVersXML",  .canonicalAscii="TexteVersXML",
          .nameEN="TextToXML",        .category="XML",
          .descFR="\xc3\x89" "chappe les caract\xc3\xa8res sp\xc3\xa9" "ciaux XML (&, <, >, \", ').",
          .descEN="Escape XML special characters (&, <, >, \", ').",
          .signatures=sig_escape,     .impl=fn_texte_vers_xml },

        { .canonical="XMLVersTexte",  .canonicalAscii="XMLVersTexte",
          .nameEN="XMLToText",        .category="XML",
          .descFR="D\xc3\xa9" "code les entit\xc3\xa9s XML (&amp;, &lt;, &#NNN;...).",
          .descEN="Decode XML entities (&amp;, &lt;, &#NNN;...).",
          .signatures=sig_unescape,   .impl=fn_xml_vers_texte },
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));

    /* ---- Constantes ---- */
    static const OwlConstDef XML_CONSTS[] = {
        /* Modes d'ouverture — entiers positifs simples, non combinables */
        {"depuisFichier",  "XML", INTEGER_VAL(XML_MODE_FICHIER),
         "XMLOuvre: charge depuis un fichier ou URL", "XMLOpen: load from file or URL", "fromFile"},
        {"depuisCha\xc3\xae" "ne", "XML", INTEGER_VAL(XML_MODE_CHAINE),
         "XMLOuvre: parse depuis une cha\xc3\xae" "ne XML", "XMLOpen: parse from an XML string", "fromString"},
        {"depuisURL",      "XML", INTEGER_VAL(XML_MODE_URL),
         "XMLOuvre: charge depuis une URL HTTP/HTTPS", "XMLOpen: load from an HTTP/HTTPS URL", "fromURL"},
        /* Types de nœuds */
        {"XMLBalise",      "XML", INTEGER_VAL(XML_TYPE_BALISE),
         "Type n\xc5\x93ud : \xc3\xa9l\xc3\xa9ment XML", "Node type: XML element", "XMLTag"},
        {"XMLTexte",       "XML", INTEGER_VAL(XML_TYPE_TEXTE),
         "Type n\xc5\x93ud : texte", "Node type: text", "XMLText"},
        {"XMLCDATA",       "XML", INTEGER_VAL(XML_TYPE_CDATA),
         "Type n\xc5\x93ud : section CDATA", "Node type: CDATA section", "XMLCDATA"},
        {"XMLCommentaire", "XML", INTEGER_VAL(XML_TYPE_COMMENT),
         "Type n\xc5\x93ud : commentaire", "Node type: comment", "XMLComment"},
        {"XMLBalisePI",    "XML", INTEGER_VAL(XML_TYPE_PI),
         "Type n\xc5\x93ud : instruction de traitement PI", "Node type: processing instruction", "XMLPITag"},
    };
    owl_registry_register_consts(XML_CONSTS,
        (int)(sizeof(XML_CONSTS) / sizeof(XML_CONSTS[0])));

    owl_registry_register_enums(XML_ENUMS,
        (int)(sizeof(XML_ENUMS) / sizeof(XML_ENUMS[0])));

    /* ---- Types réifiés ----
     * Handles opaques sur les pointeurs libxml2. Aucun field user-visible —
     * l'utilisateur manipule ces valeurs via les natives XMLXxx et les
     * méthodes prefix enregistrées plus bas. Pattern aligné sur zipArchive
     * et MockNative (fields = NULL). */
    static const char* const ALIAS_xmlNoeud[]    = { "xmlNode", NULL };
    static const char* const ALIAS_xmlAttribut[] = { "xmlAttribute", NULL };
    static const OwlTypeDef XML_TYPES[] = {
        { .canonical = "xmlDocument",
          .category  = "XML",
          .descFR    = "Document XML (arbre DOM). Construit via XMLDocument() ou XMLOuvre(), libéré par XMLTermine().",
          .descEN    = "XML document (DOM tree). Built via XMLDocument() or XMLOuvre(), freed by XMLTermine().",
          .fields    = NULL,
          .nameEN    = "xmlDocument",
          /*  — genre lu par l'article EN->FR de `owl translate`. Sans
           * lui la declaration revient en `est a xmlDocument` (forme mixte).
           * Articles releves dans le depot : 22 `est un`, 0 `est une`. */
          .genre     = OWL_GENRE_M },   /* un document */
        { .canonical = "xmlNoeud",
          .category  = "XML",
          .descFR    = "Nœud d'un document XML (élément, texte, CDATA, commentaire, PI). Référence un nœud dans l'arbre d'un xmlDocument — durée de vie liée au document parent.",
          .descEN    = "Node of an XML document (element, text, CDATA, comment, PI). References a node in the tree of an xmlDocument — lifetime tied to the parent document.",
          .fields    = NULL,
          .aliases   = ALIAS_xmlNoeud,
          .nameEN    = "xmlNode",
          .genre     = OWL_GENRE_M },   /* un nœud — 39 `est un` releves */
        { .canonical = "xmlAttribut",
          .category  = "XML",
          .descFR    = "Attribut d'un élément XML (paire nom/valeur). Référence un attribut dans l'arbre d'un xmlDocument — durée de vie liée au document parent.",
          .descEN    = "Attribute of an XML element (name/value pair). References an attribute in the tree of an xmlDocument — lifetime tied to the parent document.",
          .fields    = NULL,
          .aliases   = ALIAS_xmlAttribut,
          .nameEN    = "xmlAttribute",
          .genre     = OWL_GENRE_M },   /* un attribut */
        { .canonical = "xmlNamespace",
          .category  = "XML",
          .descFR    = "Déclaration d'espace de noms XML (préfixe + URI). Référence une déclaration dans l'arbre d'un xmlDocument — durée de vie liée au document parent.",
          .descEN    = "XML namespace declaration (prefix + URI). References a declaration in the tree of an xmlDocument — lifetime tied to the parent document.",
          .fields    = NULL,
          .nameEN    = "xmlNamespace",
          .genre     = OWL_GENRE_M },   /* un espace de noms */
    };
    owl_registry_register_types(XML_TYPES,
        (int)(sizeof(XML_TYPES) / sizeof(XML_TYPES[0])));

    /* ================================================================
     * Méthodes prefix sur xmlDocument
     * ================================================================ */
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "ConstruitCha\xc3\xae" "ne", fn_xml_construit_chaine, 1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "BuildString",               fn_xml_construit_chaine, 1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Sauve",                     fn_xml_sauve,             -1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Save",                      fn_xml_sauve,             -1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Termine",                   fn_xml_termine,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Close",                     fn_xml_termine,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Ex\xc3\xa9" "cuteXPath",    fn_xml_execute_xpath,     2);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "ExecuteXPath",              fn_xml_execute_xpath,     2);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Ex\xc3\xa9" "cuteXPathNS",  fn_xml_execute_xpath_ns,  -1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "ExecuteXPathNS",            fn_xml_execute_xpath_ns,  -1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "LitXPath",                  fn_xml_lit_xpath,         2);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "ReadXPath",                 fn_xml_lit_xpath,         2);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "EcritXPath",                fn_xml_ecrit_xpath,       3);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "WriteXPath",                fn_xml_ecrit_xpath,       3);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Racine",                    fn_xml_racine,            1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Root",                      fn_xml_racine,            1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Premier",                   fn_xml_premier,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "First",                     fn_xml_premier,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Dernier",                   fn_xml_dernier,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Last",                      fn_xml_dernier,           1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Fils",                      fn_xml_fils,              1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Children",                  fn_xml_fils,              1);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Valide",                    fn_xml_document_valide,   2);
    owl_register_method(METHOD_TYPE_XML_DOCUMENT, "Valid",                     fn_xml_document_valide,   2);

    /* ================================================================
     * Méthodes prefix sur xmlNoeud
     * ================================================================ */
    owl_register_method(METHOD_TYPE_XML_NODE, "Nom",             fn_xml_nom_element,    1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Name",            fn_xml_nom_element,    1);
    owl_register_method(METHOD_TYPE_XML_NODE, "NomParent",       fn_xml_nom_parent,     1);
    owl_register_method(METHOD_TYPE_XML_NODE, "ParentName",      fn_xml_nom_parent,     1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Type",            fn_xml_type_element,   1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Parent",          fn_xml_parent,         1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Suivant",         fn_xml_suivant,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Next",            fn_xml_suivant,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Pr\xc3\xa9" "c\xc3\xa9" "dent", fn_xml_precedent, 1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Previous",        fn_xml_precedent,      1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Premier",         fn_xml_premier,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "First",           fn_xml_premier,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Dernier",         fn_xml_dernier,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Last",            fn_xml_dernier,        1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Fils",            fn_xml_fils,           1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Children",        fn_xml_fils,           1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Lit",             fn_xml_lit,            1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Read",            fn_xml_lit,            1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Ecrit",           fn_xml_ecrit,          2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Write",           fn_xml_ecrit,          2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Modifie",         fn_xml_modifie,        2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Modify",          fn_xml_modifie,        2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Renomme",         fn_xml_renomme,        2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Rename",          fn_xml_renomme,        2);
    owl_register_method(METHOD_TYPE_XML_NODE, "Supprime",        fn_xml_supprime,       1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Delete",          fn_xml_supprime,       1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AjouteFils",      fn_xml_ajoute_fils,    -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AddChild",        fn_xml_ajoute_fils,    -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "Ins\xc3\xa8reFils",fn_xml_insere_fils,   -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "InsertChild",     fn_xml_insere_fils,    -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AjouteAttribut",  fn_xml_ajoute_attribut,3);
    owl_register_method(METHOD_TYPE_XML_NODE, "AddAttribute",    fn_xml_ajoute_attribut,3);
    owl_register_method(METHOD_TYPE_XML_NODE, "LitAttribut",     fn_xml_lit_attribut,   2);
    owl_register_method(METHOD_TYPE_XML_NODE, "ReadAttribute",   fn_xml_lit_attribut,   2);
    owl_register_method(METHOD_TYPE_XML_NODE, "ListeAttributs",  fn_xml_liste_attributs,1);
    owl_register_method(METHOD_TYPE_XML_NODE, "ListAttributes",  fn_xml_liste_attributs,1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AttributExiste",  fn_xml_attribut_existe,2);
    owl_register_method(METHOD_TYPE_XML_NODE, "AttributeExists", fn_xml_attribut_existe,2);
    owl_register_method(METHOD_TYPE_XML_NODE, "AjouteCommentaire",fn_xml_ajoute_commentaire,-1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AddComment",      fn_xml_ajoute_commentaire,-1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AjouteCDATA",     fn_xml_ajoute_cdata,   -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AddCDATA",        fn_xml_ajoute_cdata,   -1);
    owl_register_method(METHOD_TYPE_XML_NODE, "AjouteEspaceNom", fn_xml_ajoute_espace_nom,3);
    owl_register_method(METHOD_TYPE_XML_NODE, "AddNamespace",    fn_xml_ajoute_espace_nom,3);
    owl_register_method(METHOD_TYPE_XML_NODE, "ListeEspacesNom", fn_xml_liste_espaces_nom,1);
    owl_register_method(METHOD_TYPE_XML_NODE, "ListNamespaces",  fn_xml_liste_espaces_nom,1);

    /* ================================================================
     * Méthodes prefix sur xmlAttribut
     * ================================================================ */
    owl_register_method(METHOD_TYPE_XML_ATTRIBUT, "Nom",     fn_xml_attribut_nom,    1);
    owl_register_method(METHOD_TYPE_XML_ATTRIBUT, "Name",    fn_xml_attribut_nom,    1);
    owl_register_method(METHOD_TYPE_XML_ATTRIBUT, "Valeur",  fn_xml_attribut_valeur, 1);
    owl_register_method(METHOD_TYPE_XML_ATTRIBUT, "Value",   fn_xml_attribut_valeur, 1);

    /* ================================================================
     * Méthodes prefix sur xmlNamespace
     * ================================================================ */
    owl_register_method(METHOD_TYPE_XML_NAMESPACE, "Pr\xc3\xa9" "fixe", fn_xml_namespace_prefixe, 1);
    owl_register_method(METHOD_TYPE_XML_NAMESPACE, "Prefix",             fn_xml_namespace_prefixe, 1);
    owl_register_method(METHOD_TYPE_XML_NAMESPACE, "URI",                fn_xml_namespace_uri,     1);
}
