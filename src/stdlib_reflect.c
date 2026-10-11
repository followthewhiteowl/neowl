// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * Reflection and introspection.
 *
 * TypeVar(v)                    -> int (owl* constant)
 * TypeEstNullable(v_or_code)    -> bool (always Faux in v1 — nullable opt-in
 *                                  comes in phase 2)
 * EstNul(v)                     -> bool
 * RécupèreDéfinition(v)         -> Définition (type info about the VALUE)
 * RécupèreDéfinitionVariable(v) -> Définition (declared type — same as
 *                                  RécupèreDéfinition in v1, no dyn objects)
 * RécupèreDéfinitionType(nom)   -> Définition (lookup by type name)
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "vm.h"
#include "table.h"
#include "ui_decl.h"
#include "etat_decl.h"
#include "scanner.h"
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

/* ================================================================
 * OWL type codes — codes numeriques retournes par TypeVar.
 *
 * Valeurs canon pour les types de base confirmes
 * (entier=8, chaine=16, booleen=1) afin de preserver la
 * compatibilite des exemples portes. Codes >=60 reservés extensions OWL
 * (JSON, Procedure, Etat, Fenetre, Champ*).
 *
 * Naming : les constantes globales exposees au runtime portent le prefixe
 * "owl*" (cf.  : OWL est un langage distinct). Les macros C ci-dessous restent
 * "WL_*" car internes, jamais exposees.
 *
 * Type-system : OWL ne reifie qu'un sous-ensemble des types canon — entiers
 * non dimensionnes / non signés, reel unique 64 bits, chaine UTF-8 unique,
 * pas de Variant ni de Caractere. Les codes correspondants ont ete retires
 * (cf. ).
 * ================================================================ */

#define WL_PAS_DEFINI          0

#define WL_BOOLEEN             1

#define WL_REEL                7   /* reel 64 bits (canon OWL) */
#define WL_ENTIER              8   /* entier 64 bits signé (canon OWL) */
#define WL_MONETAIRE           13
#define WL_NUMERIQUE           14

#define WL_CHAINE              16  /* chaine UTF-8 unique (canon OWL) */
#define WL_BUFFER              25
#define WL_UUID                26

#define WL_DATE                30
#define WL_HEURE               31
#define WL_DATE_HEURE          32
#define WL_DUREE               33

#define WL_TABLEAU             40
#define WL_TABLEAU_ASSOCIATIF  41
#define WL_FILE                42
#define WL_PILE                43
#define WL_LISTE               44

#define WL_STRUCTURE           50
#define WL_CLASSE              51
#define WL_ENUMERATION         52
#define WL_COMBINAISON         53
#define WL_INSTANCE            54

#define WL_JSON                61

/*  — type Vecteur/Vector (extension OWL, hors numerotation canon,
 * d'ou la zone >=60). Distinct de tableau (40) et buffer (25). */
#define WL_VECTEUR             62

/*  — type Matrice/Matrix (extension OWL, meme zone >=60). Distinct
 * du Vecteur (62), du tableau (40) et du buffer (25). */
#define WL_MATRICE             63

/*  — type Tenseur/Tensor (extension OWL, meme zone >=60). Enveloppe
 * differentiable au-dessus d'un Vecteur, distinct du Vecteur (62). */
#define WL_TENSEUR             64

/*  — type IAModèle/AIModel (extension OWL, meme zone >=60). Modele
 * de reseau de neurones (pile de couches), distinct du Tenseur (64). */
#define WL_IAMODELE            65

#define WL_PROCEDURE           70

/* OWL ext >=80 : DSL Etat + DSL Fenetre (champs UI). Hors zone canon
 * pour eviter toute collision avec une numerotation future. */
#define WL_ETAT                80
#define WL_FENETRE             81
#define WL_CHAMP_LIBELLE       82
#define WL_CHAMP_BOUTON        83
#define WL_CHAMP_SAISIE        84
#define WL_CHAMP_TABLE         85
#define WL_CHAMP_CONTENEUR     86
#define WL_CHAMP_IMAGE         87
#define WL_CHAMP_LISTE         88
#define WL_CHAMP_COMBO         89
#define WL_CHAMP_SELECTEUR     90
#define WL_CHAMP_INTERRUPTEUR  91
#define WL_CHAMP_ZONE_REPETEE  92
#define WL_CHAMP_JAUGE         93

/* FamilleDéfinition — classification broad enough to dispatch on. */
#define FAM_PAS_DEFINI     0
#define FAM_TYPE_SIMPLE    1
#define FAM_TYPE_AVANCE    2
#define FAM_TABLEAU        3
#define FAM_COLLECTION     4
#define FAM_STRUCTURE      5
#define FAM_CLASSE         6
#define FAM_ENUMERATION    7
#define FAM_COMBINAISON    8
#define FAM_JSON           9
#define FAM_PROCEDURE      11
/* OWL extensions */
#define FAM_ETAT           12
#define FAM_FENETRE        13
#define FAM_CHAMP          14

/* ================================================================
 * Value -> canonical type code
 * ================================================================ */

/* Forward decl : utilise pour mapper un ObjUiControl vers son code owl*
 * (defini plus bas dans la section builders Etat/Fenetre). */
static int control_kind_to_code(int kind);

static int value_type_code(Value v) {
    /* UUID est reifie comme ObjInstance (cf. stdlib_uuid.c) — detection
     * dediee AVANT le case OBJ_INSTANCE generique pour ne pas retomber
     * sur owlStructure / owlInstance. */
    if (owl_value_is_uuid(v)) return WL_UUID;

    switch (v.type) {
        case VAL_NIL:      return WL_PAS_DEFINI;
        case VAL_BOOL:     return WL_BOOLEEN;
        case VAL_INTEGER:  return WL_ENTIER;
        case VAL_REAL:     return WL_REEL;
        case VAL_CURRENCY: return WL_MONETAIRE;
        case VAL_OBJ: {
            switch (OBJ_TYPE(v)) {
                case OBJ_STRING:       return WL_CHAINE;
                case OBJ_BUFFER:       return WL_BUFFER;
                case OBJ_NUMERIC:      return WL_NUMERIQUE;
                case OBJ_FUNCTION:     return WL_PROCEDURE;
                case OBJ_NATIVE:       return WL_PROCEDURE;
                case OBJ_PROC_CHAIN:   return WL_PROCEDURE;
                case OBJ_ARRAY:        return WL_TABLEAU;
                case OBJ_ASSOC_ARRAY:  return WL_TABLEAU_ASSOCIATIF;
                case OBJ_INSTANCE: {
                    /* klass != NULL  -> instance d'une classe user (owlInstance)
                     * klass == NULL  -> structure (owlStructure). */
                    ObjInstance* ii = AS_INSTANCE(v);
                    return ii->klass != NULL ? WL_INSTANCE : WL_STRUCTURE;
                }
                case OBJ_CLASS: {
                    ObjClass* k = AS_CLASS(v);
                    switch (k->kind) {
                        case CLASS_KIND_ENUM:        return WL_ENUMERATION;
                        case CLASS_KIND_COMBINATION: return WL_COMBINAISON;
                        case CLASS_KIND_CLASS:
                        default:                     return WL_CLASSE;
                    }
                }
                case OBJ_COLLECTION: {
                    ObjCollection* c = AS_COLLECTION(v);
                    switch (c->collType) {
                        case COLLECTION_QUEUE: return WL_FILE;
                        case COLLECTION_STACK: return WL_PILE;
                        case COLLECTION_LIST:  return WL_LISTE;
                    }
                    return WL_TABLEAU;
                }
                case OBJ_JSON:     return WL_JSON;
                case OBJ_VECTOR:   return WL_VECTEUR;   /*  */
                case OBJ_MATRIX:   return WL_MATRICE;   /*  */
                case OBJ_MATRIX_ROW: return WL_PAS_DEFINI; /*  : vue interne transitoire, jamais exposee */
                case OBJ_TENSOR:   return WL_TENSEUR;   /*  */
                case OBJ_TENSOR_VIEW: return WL_PAS_DEFINI; /*  : vue interne transitoire, jamais exposee */
                case OBJ_IAMODELE: return WL_IAMODELE;  /*  */
                case OBJ_OPTIONS_ENTRAINEMENT: return WL_PAS_DEFINI; /*  : config, pas de code canonique */
                case OBJ_LLM: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_OPTIONS_GENERATION: return WL_PAS_DEFINI; /*  : config, pas de code canonique */
                case OBJ_MESSAGE_LLM: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_RESPONSE_LLM: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_DETECTION: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLARBRE: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLFORET: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLBOOSTING: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLLINEAIRE: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLLOGISTIQUE: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_MLKMEANS: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_IMAGE_ETIQUETEE: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_IMAGE_ANNOTEE: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_DATASET: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_XLS_DOCUMENT: return WL_PAS_DEFINI; /*  : type OWL, pas de code canonique */
                case OBJ_XLS_REF: return WL_PAS_DEFINI; /*  : ref de style transitoire, jamais exposee */
                case OBJ_DATE:     return WL_DATE;
                case OBJ_TIME:     return WL_HEURE;
                case OBJ_DATETIME: return WL_DATE_HEURE;
                case OBJ_DURATION: return WL_DUREE;
                case OBJ_ZIP_ARCHIVE: return WL_PAS_DEFINI; /* pas encore un code canonique */
                case OBJ_MOCK_NATIVE: return WL_PAS_DEFINI; /* Mock system internal */
                case OBJ_PATTERN:     return WL_PAS_DEFINI; /* Mock system internal */
                case OBJ_MULTILINGUE: return WL_PAS_DEFINI; /*  lot B : code canonique a poser quand stable */
                case OBJ_UI_CONTROL: {
                    ObjUiControl* c = AS_UI_CONTROL(v);
                    return control_kind_to_code(c->kind);
                }
                /*  : infra multi-instance pas encore exposee user-facing.
                 * Pas de code canonique pose tant que stdlib_ui ne consomme pas
                 * la triade (sous-tickets +). */
                case OBJ_WINDOW_INSTANCE:
                case OBJ_UI_CONTROL_INSTANCE:
                case OBJ_UI_CONTROL_REF:
                case OBJ_WINDOW_REF:
                case OBJ_ETAT_INSTANCE:
                    return WL_PAS_DEFINI;
                /*  Lot 2b — code canonique pose en Lot 2c quand
                 * le type Icone entre au registry. Pas defini en attendant. */
                case OBJ_ICONE:
                    return WL_PAS_DEFINI;
                /*  Lot 2e-1 — namespace singleton, opaque cote
                 * reflection (l'utilisateur final manipule des Icone, jamais
                 * le namespace lui-meme). */
                case OBJ_ICONES_NAMESPACE:
                    return WL_PAS_DEFINI;
                /*  Lot 2b+3 — namespace styles, opaque cote
                 * reflection (l'utilisateur manipule des instances de style,
                 * jamais le namespace lui-meme). */
                case OBJ_THEME_NAMESPACE:
                    return WL_PAS_DEFINI;
            }
            return WL_PAS_DEFINI;
        }
    }
    return WL_PAS_DEFINI;
}

static int type_code_family(int code) {
    switch (code) {
        case WL_BOOLEEN:
        case WL_ENTIER: case WL_REEL:
        case WL_MONETAIRE: case WL_NUMERIQUE:
        case WL_CHAINE:
        case WL_DATE: case WL_HEURE: case WL_DATE_HEURE: case WL_DUREE:
            return FAM_TYPE_SIMPLE;
        case WL_BUFFER:
        case WL_UUID:
        case WL_TENSEUR:
        case WL_IAMODELE:
            return FAM_TYPE_AVANCE;
        case WL_TABLEAU: case WL_TABLEAU_ASSOCIATIF: case WL_VECTEUR:
        case WL_MATRICE:
            return FAM_TABLEAU;
        case WL_FILE: case WL_PILE: case WL_LISTE:
            return FAM_COLLECTION;
        case WL_STRUCTURE: case WL_INSTANCE: return FAM_STRUCTURE;
        case WL_CLASSE:                      return FAM_CLASSE;
        case WL_ENUMERATION:                 return FAM_ENUMERATION;
        case WL_COMBINAISON:                 return FAM_COMBINAISON;
        case WL_JSON:                        return FAM_JSON;
        case WL_PROCEDURE:                   return FAM_PROCEDURE;
        case WL_ETAT:                        return FAM_ETAT;
        case WL_FENETRE:                     return FAM_FENETRE;
        case WL_CHAMP_LIBELLE:
        case WL_CHAMP_BOUTON:
        case WL_CHAMP_SAISIE:
        case WL_CHAMP_TABLE:
        case WL_CHAMP_CONTENEUR:
        case WL_CHAMP_IMAGE:
        case WL_CHAMP_LISTE:
        case WL_CHAMP_COMBO:
        case WL_CHAMP_SELECTEUR:
        case WL_CHAMP_INTERRUPTEUR:          return FAM_CHAMP;
        case WL_CHAMP_ZONE_REPETEE:          return FAM_CHAMP;
        case WL_CHAMP_JAUGE:                 return FAM_CHAMP;
    }
    return FAM_PAS_DEFINI;
}

static const char* type_code_name(int code) {
    switch (code) {
        case WL_BOOLEEN:             return "booleen";
        case WL_ENTIER:              return "entier";
        case WL_REEL:                return "reel";
        case WL_MONETAIRE:           return "monetaire";
        case WL_NUMERIQUE:           return "numerique";
        case WL_CHAINE:              return "chaine";
        case WL_BUFFER:              return "buffer";
        case WL_UUID:                return "uuid";
        case WL_DATE:                return "date";
        case WL_HEURE:               return "heure";
        case WL_DATE_HEURE:          return "dateheure";
        case WL_DUREE:               return "duree";
        case WL_TABLEAU:             return "tableau";
        case WL_VECTEUR:             return "vecteur";
        case WL_MATRICE:             return "matrice";
        case WL_TENSEUR:             return "tenseur";
        case WL_IAMODELE:            return "iamodele";
        case WL_TABLEAU_ASSOCIATIF:  return "tableau associatif";
        case WL_FILE:                return "file";
        case WL_PILE:                return "pile";
        case WL_LISTE:               return "liste";
        case WL_STRUCTURE:           return "structure";
        case WL_CLASSE:              return "classe";
        case WL_ENUMERATION:         return "enumeration";
        case WL_COMBINAISON:         return "combinaison";
        case WL_INSTANCE:            return "instance";
        case WL_JSON:                return "json";
        case WL_PROCEDURE:           return "procedure";
        case WL_ETAT:                return "etat";
        case WL_FENETRE:             return "fenetre";
        case WL_CHAMP_LIBELLE:       return "champ libelle";
        case WL_CHAMP_BOUTON:        return "champ bouton";
        case WL_CHAMP_SAISIE:        return "champ saisie";
        case WL_CHAMP_TABLE:         return "champ table";
        case WL_CHAMP_CONTENEUR:     return "champ conteneur";
        case WL_CHAMP_IMAGE:         return "champ image";
        case WL_CHAMP_LISTE:         return "champ liste";
        case WL_CHAMP_COMBO:         return "champ combo";
        case WL_CHAMP_SELECTEUR:     return "champ selecteur";
        case WL_CHAMP_INTERRUPTEUR:  return "champ interrupteur";
        case WL_CHAMP_ZONE_REPETEE:  return "champ zone repetee";
        case WL_CHAMP_JAUGE:         return "champ jauge";
    }
    return "pas defini";
}

/* ================================================================
 * Définition builder
 * ================================================================ */

/* Cree une instance vide a partir du template Definition (clone-based)
 * pour partir des champs par defaut declares dans stdlib_builtin_types.
 * Si le template n'existe pas (cas degrade), instancie un struct vide. */
static ObjInstance* new_definition_instance(void) {
    ObjString* tplName = obj_copy_string("Definition", 10);
    Value tplVal;
    /*  : template Definition vit maintenant dans vm.types. */
    if (table_get(&vm.types, tplName, &tplVal) && IS_INSTANCE(tplVal)) {
        ObjInstance* tpl = AS_INSTANCE(tplVal);
        ObjInstance* inst = obj_new_instance(tpl->structName);
        /*  lot 6 : push inst+tpl pendant la copie ; table_set peut
         * realloc (GC) et inst fresh n'est sur aucune root. tpl est protege
         * par vm.types mais on push par symetrie/lisibilite. */
        vm_push(OBJ_VAL(inst));
        vm_push(OBJ_VAL(tpl));
        for (int i = 0; i < tpl->fields.capacity; i++) {
            Entry* e = &tpl->fields.entries[i];
            if (e->key == NULL) continue;
            /*  : delegue au canon obj_inst_set_kv. */
            obj_inst_set_kv(inst, e->key, e->value);
        }
        vm_pop(); /* tpl */
        vm_pop(); /* inst */
        return inst;
    }
    return obj_new_instance(obj_copy_string("Definition", 10));
}

/* Helpers de population des champs canon de Definition.
 * Délègue à inst_field_set_* (object.h) qui rootent inst/key/val avant
 * obj_copy_string + table_set. Cf.  lot 5. */
static void def_set_nom(ObjInstance* inst, const char* nom) {
    inst_field_set_str(inst, "Nom", nom);
}
static void def_set_type_fam(ObjInstance* inst, int code, int fam) {
    inst_field_set_int (inst, "Type", code);
    inst_field_set_int (inst, "FamilleDefinition", fam);
    inst_field_set_bool(inst, "Nullable", false);
}
static void def_set_champs_array(ObjInstance* inst, ObjArray* champs) {
    inst_field_set_value(inst, "ChampsNoms", OBJ_VAL(champs));
}

/* Build a Définition instance populated for a given value. */
static ObjInstance* build_definition_for_value(Value v) {
    ObjInstance* inst = new_definition_instance();
    /*  : inst + champs victimes potentielles (allocs suivantes
     * dans la boucle obj_copy_string pour les keys). */
    vm_push(OBJ_VAL(inst));

    int code = value_type_code(v);
    int fam  = type_code_family(code);

    /* Nom: for user-defined types -> the user name (struct/class/enum/combinaison).
     * For primitives and untagged values -> the type kind ("entier", "chaine"...).
     *  - Structure  -> ObjInstance template with structName = user name
     *  - Classe     -> ObjClass with name = user name
     *  - Enumeration / Combinaison -> ObjClass too (currently same as classe;
     *    distinguished via kind field for the .Type code, not for .Nom). */
    const char* nom = type_code_name(code);
    if (IS_INSTANCE(v)) {
        ObjInstance* ii = AS_INSTANCE(v);
        if (ii->structName != NULL) nom = ii->structName->chars;
    } else if (IS_CLASS(v)) {
        ObjClass* kk = AS_CLASS(v);
        if (kk->name != NULL) nom = kk->name->chars;
    }
    def_set_nom(inst, nom);
    def_set_type_fam(inst, code, fam);

    /* Populate ChampsNoms (array of string) for struct/class instances. */
    ObjArray* champs = obj_new_array();
    vm_push(OBJ_VAL(champs));
    if (IS_INSTANCE(v)) {
        ObjInstance* ii = AS_INSTANCE(v);
        for (int i = 0; i < ii->fields.capacity; i++) {
            Entry* e = &ii->fields.entries[i];
            if (e->key == NULL) continue;
            obj_array_push(champs,
                OBJ_VAL(obj_copy_string(e->key->chars, e->key->length)));
        }
    }
    def_set_champs_array(inst, champs);
    vm_pop(); /* champs */
    vm_pop(); /* inst */
    return inst;
}

/* Resout un nom de type declare ("chaine", "réel", "Personne") vers son
 * code owl*. Comparaison accent-insensitive case-insensitive (canon
 * scanner). Retourne WL_PAS_DEFINI si nom inconnu et pas un type
 * user-defined connu. */
static int wl_code_from_type_name(const char* name) {
    if (!name) return WL_PAS_DEFINI;

    /* Strip accents puis lowercase pour matcher les noms canon. */
    char buf[128];
    int n = scanner_strip_accents(name, (int)strlen(name), buf, (int)sizeof(buf));
    if (n <= 0) return WL_PAS_DEFINI;
    for (int i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)buf[i]);
    buf[n] = '\0';

    /* Types simples OWL canon. */
    if (strcmp(buf, "booleen") == 0)         return WL_BOOLEEN;
    if (strcmp(buf, "entier") == 0)          return WL_ENTIER;
    if (strcmp(buf, "reel") == 0)            return WL_REEL;
    if (strcmp(buf, "monetaire") == 0)       return WL_MONETAIRE;
    if (strcmp(buf, "numerique") == 0)       return WL_NUMERIQUE;
    if (strcmp(buf, "chaine") == 0)          return WL_CHAINE;
    if (strcmp(buf, "buffer") == 0)          return WL_BUFFER;
    if (strcmp(buf, "uuid") == 0)            return WL_UUID;
    if (strcmp(buf, "date") == 0)            return WL_DATE;
    if (strcmp(buf, "heure") == 0)           return WL_HEURE;
    if (strcmp(buf, "dateheure") == 0)       return WL_DATE_HEURE;
    if (strcmp(buf, "duree") == 0)           return WL_DUREE;
    if (strcmp(buf, "tableau") == 0)         return WL_TABLEAU;
    if (strcmp(buf, "json") == 0)            return WL_JSON;

    /* Types user-defined : si le nom existe en global (Structure/Classe/
     * Enumeration/Combinaison), on retourne le code correspondant via
     * value_type_code sur l'objet trouve.  : on consulte d'abord
     * vm.types (templates builtins comme Cadre/httpRequête/Chrono), puis
     * vm.globals (classes/enums user qui restent dans vm.globals).
     * Sinon WL_PAS_DEFINI. */
    ObjString* nameStr = obj_copy_string(name, (int)strlen(name));
    Value tpl;
    if (table_get(&vm.types, nameStr, &tpl)) {
        return value_type_code(tpl);
    }
    if (table_get(&vm.globals, nameStr, &tpl)) {
        return value_type_code(tpl);
    }
    return WL_PAS_DEFINI;
}

/* Construit une mini-Definition pour un parametre/champ : Nom + Type +
 * Famille + (TypeNom = chaine humaine du type, utile si user-defined). */
static ObjInstance* build_param_definition(const char* nom, const char* type_name) {
    ObjInstance* inst = new_definition_instance();
    def_set_nom(inst, nom ? nom : "");
    int code = wl_code_from_type_name(type_name);
    int fam  = type_code_family(code);
    def_set_type_fam(inst, code, fam);
    /* TypeNom : la chaine telle qu'ecrite par l'utilisateur ("Personne",
     * "tableau de Article") — non normalisee, utile pour le debug. */
    if (type_name && *type_name) {
        inst_field_set_str(inst, "TypeNom", type_name);
    }
    return inst;
}

/* Build a Definition for an Etat declaration.
 *   - ChampsNoms (string array)            : noms des parametres formels
 *   - Parametres (array of mini-Definition): paire Nom + Type pour chaque param
 */
static ObjInstance* build_definition_for_etat(const OwlEtatDecl* e) {
    ObjInstance* inst = new_definition_instance();
    /*  : push inst pendant la construction de champs/params (allocs
     * suivantes peuvent declencher GC → inst collected). */
    vm_push(OBJ_VAL(inst));
    def_set_nom(inst, e->name);
    def_set_type_fam(inst, WL_ETAT, FAM_ETAT);

    /*  : champs et params sont fresh allocs → victimes potentielles
     * si une alloc suivante (obj_copy_string ou build_param_definition)
     * declenche GC. Push les deux pour les proteger pendant la boucle. */
    ObjArray* champs = obj_new_array();
    vm_push(OBJ_VAL(champs));
    ObjArray* params = obj_new_array();
    vm_push(OBJ_VAL(params));
    for (int i = 0; i < e->param_count; i++) {
        const char* pname = e->params[i].name;
        const char* ptype = e->params[i].type_name; /* peut etre NULL */
        if (!pname) continue;
        obj_array_push(champs,
            OBJ_VAL(obj_copy_string(pname, (int)strlen(pname))));
        obj_array_push(params,
            OBJ_VAL(build_param_definition(pname, ptype)));
    }
    def_set_champs_array(inst, champs);
    inst_field_set_value(inst, "Parametres", OBJ_VAL(params));
    vm_pop(); /* params */
    vm_pop(); /* champs */
    vm_pop(); /* inst */
    return inst;
}

/* Mappe un OwlControlKind vers son code owl* (pour TypeVar sur ObjUiControl
 * et pour les ChampsNoms d'une Definition de Fenetre). */
static int control_kind_to_code(int kind /* OwlControlKind */) {
    switch (kind) {
        case OWL_CONTROL_LABEL:     return WL_CHAMP_LIBELLE;
        case OWL_CONTROL_BUTTON:    return WL_CHAMP_BOUTON;
        case OWL_CONTROL_INPUT:     return WL_CHAMP_SAISIE;
        case OWL_CONTROL_TABLE:     return WL_CHAMP_TABLE;
        case OWL_CONTROL_CONTAINER: return WL_CHAMP_CONTENEUR;
        case OWL_CONTROL_IMAGE:     return WL_CHAMP_IMAGE;
        case OWL_CONTROL_LISTE:     return WL_CHAMP_LISTE;
        case OWL_CONTROL_COMBO:     return WL_CHAMP_COMBO;
        case OWL_CONTROL_SELECTEUR: return WL_CHAMP_SELECTEUR;
        case OWL_CONTROL_INTERRUPTEUR: return WL_CHAMP_INTERRUPTEUR;
        case OWL_CONTROL_ZONE_REPETEE: return WL_CHAMP_ZONE_REPETEE;
        case OWL_CONTROL_JAUGE:     return WL_CHAMP_JAUGE;
        case OWL_CONTROL_WINDOW:    return WL_FENETRE;
    }
    return WL_PAS_DEFINI;
}

/* Build a mini-Definition for a sub-control of a window. Nom = nom du
 * controle, Type = wl* code du kind (Libelle/Bouton/Saisie/...), Famille
 * = familleChamp. */
static ObjInstance* build_control_definition(const OwlControlDecl* c) {
    ObjInstance* inst = new_definition_instance();
    def_set_nom(inst, c->name ? c->name : "");
    int code = control_kind_to_code(c->kind);
    int fam  = type_code_family(code);
    def_set_type_fam(inst, code, fam);
    return inst;
}

/* Build a Definition for a Window declaration.
 *   - ChampsNoms (string array)             : noms des sous-controles
 *   - Champs     (array of mini-Definition) : Nom + Type pour chaque sous-controle
 */
static ObjInstance* build_definition_for_window(const OwlWindowDecl* w) {
    ObjInstance* inst = new_definition_instance();
    /*  : inst + champsNoms + champs sont victimes potentielles
     * (allocs suivantes peuvent GC). Push tous. */
    vm_push(OBJ_VAL(inst));
    def_set_nom(inst, w->name);
    def_set_type_fam(inst, WL_FENETRE, FAM_FENETRE);

    ObjArray* champsNoms = obj_new_array();
    vm_push(OBJ_VAL(champsNoms));
    ObjArray* champs     = obj_new_array();
    vm_push(OBJ_VAL(champs));
    for (int i = 0; i < w->control_count; i++) {
        const OwlControlDecl* ctrl = &w->controls[i];
        if (!ctrl->name) continue;
        obj_array_push(champsNoms,
            OBJ_VAL(obj_copy_string(ctrl->name, (int)strlen(ctrl->name))));
        obj_array_push(champs,
            OBJ_VAL(build_control_definition(ctrl)));
    }
    def_set_champs_array(inst, champsNoms);
    inst_field_set_value(inst, "Champs", OBJ_VAL(champs));
    vm_pop(); /* champs */
    vm_pop(); /* champsNoms */
    vm_pop(); /* inst */
    return inst;
}

/* ================================================================
 * Native functions
 * ================================================================ */

/* TypeVar(v) -> entier (wl* code) */
static Value fn_type_var(int argCount, Value* args) {
    (void)argCount;
    return INTEGER_VAL(value_type_code(args[0]));
}

/* TypeEstNullable(v_or_code) -> booleen. Always Faux in v1: nullable types
 * aren't declared yet. Future: return Vrai for declarations using '?' or
 * '<nullable>'. */
static Value fn_type_est_nullable(int argCount, Value* args) {
    (void)argCount; (void)args;
    return BOOL_VAL(false);
}

/* EstNul(v) -> booleen */
static Value fn_est_nul(int argCount, Value* args) {
    (void)argCount;
    return BOOL_VAL(IS_NIL(args[0]));
}

/* VariableExiste(nom) -> booleen : une globale de ce nom existe-t-elle ? */
static Value fn_variable_existe(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    Value tmp;
    return BOOL_VAL(table_get(&vm.globals, AS_STRING(args[0]), &tmp));
}

/* RécupèreDéfinition(v) -> Définition */
static Value fn_recupere_definition(int argCount, Value* args) {
    (void)argCount;
    return OBJ_VAL(build_definition_for_value(args[0]));
}

/* RécupèreDéfinitionType(nom_type) -> Définition
 * Looks up a type by its name string. Dispatch (priorite haute -> basse) :
 *   1. DSL Etat   (vm.etats)    -> Definition Type=owlEtat,   Famille=Etat
 *   2. DSL Fenetre (vm.windows) -> Definition Type=owlFenetre, Famille=Fenetre
 *   3. vm.types   (builtins + types user) -> path principal
 *   4. vm.globals (classes/enums user qui ne sont pas dans vm.types) -> fallback
 */
static Value fn_recupere_definition_type(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;
    ObjString* name = AS_STRING(args[0]);

    const OwlEtatDecl* etat = vm_etat_lookup(name->chars);
    if (etat != NULL) {
        return OBJ_VAL(build_definition_for_etat(etat));
    }
    const OwlWindowDecl* win = vm_window_lookup(name->chars);
    if (win != NULL) {
        return OBJ_VAL(build_definition_for_window(win));
    }

    Value tpl;
    if (table_get(&vm.types, name, &tpl)) {
        return OBJ_VAL(build_definition_for_value(tpl));
    }
    if (!table_get(&vm.globals, name, &tpl)) return NIL_VAL;
    return OBJ_VAL(build_definition_for_value(tpl));
}

/* ================================================================
 * Manipulation d'énumérations — famille Énumération*
 * ================================================================
 * L'argument type est passé NU par l'utilisateur
 * (EnumérationDepuisValeur(ENuance, "R")). Le compilateur émet la lecture
 * globale de l'ObjClass de l'énumération (cf. compiler.c::call +
 * is_enum_manip_native), donc args[0] est l'ObjClass CLASS_KIND_ENUM. Chaque
 * membre est une ObjInstance dans klass->defaults (clé = nom du membre),
 * portant les champs "Nom" et "Valeur" posés par __EnumVal à la déclaration.
 */

/* Rend le template d'erreur dans la langue active (paires nom/valeur
 * terminées par NULL). Le buffer retourné est partagé par le registry — le
 * caller le copie immédiatement (owl_vm_raise_native / obj_copy_string). */
static const char* enum_render_active(OwlErrorCode code, ...) {
    va_list ap;
    va_start(ap, code);
    const char* m = owl_error_render_active_va(code, ap);
    va_end(ap);
    return m;
}

/* Nom symbolique canonique d'un code d'erreur (ex "E_RT_ENUM_VALUE_NOT_FOUND"),
 * transmis à owl_vm_raise_native pour alimenter ExceptionInfo(errCode) et
 * errCodeNum. Pointe dans le registre erreurs (stable, pas un buffer partagé).
 * "" si le code est invalide. */
static const char* enum_error_code_name(OwlErrorCode code) {
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    return d != NULL ? d->name : "";
}

/* Lit un champ ("Nom" / "Valeur") d'une instance membre d'énumération. */
static bool enum_member_field(ObjInstance* inst, const char* fieldName, Value* out) {
    ObjString* k = obj_copy_string(fieldName, (int)strlen(fieldName));
    return table_get(&inst->fields, k, out);
}

/* Retourne le membre dont le champ `field` égale `needle`, ou NIL si absent. */
static Value enum_find_by_field(ObjClass* klass, const char* field, Value needle) {
    for (int i = 0; i < klass->defaults.capacity; i++) {
        Entry* e = &klass->defaults.entries[i];
        if (e->key == NULL || !IS_INSTANCE(e->value)) continue;
        Value fv;
        if (enum_member_field(AS_INSTANCE(e->value), field, &fv)
            && values_equal(fv, needle)) {
            return e->value;
        }
    }
    return NIL_VAL;
}

/* Vrai si args[0] est bien un type d'énumération ; sinon lève. */
static bool enum_require_type(Value* args, const char* fnName) {
    if (IS_CLASS(args[0]) && AS_CLASS(args[0])->kind == CLASS_KIND_ENUM) {
        return true;
    }
    owl_vm_raise_native(fnName,
        enum_error_code_name(E_RT_ENUM_ARG_NOT_ENUM),
        enum_render_active(E_RT_ENUM_ARG_NOT_ENUM, "fn", fnName, NULL));
    return false;
}

/* EnumérationDepuisValeur(Enum, valeur [, défaut]) -> membre.
 * Recherche inverse valeur -> membre. Introuvable sans défaut : exception. */
static Value fn_enumeration_depuis_valeur(int argCount, Value* args) {
    if (!enum_require_type(args, "Enum\xc3\xa9rationDepuisValeur")) return NIL_VAL;
    ObjClass* klass = AS_CLASS(args[0]);
    Value found = enum_find_by_field(klass, "Valeur", args[1]);
    if (!IS_NIL(found)) return found;
    if (argCount >= 3) return args[2];
    owl_vm_raise_native("Enum\xc3\xa9rationDepuisValeur",
        enum_error_code_name(E_RT_ENUM_VALUE_NOT_FOUND),
        enum_render_active(E_RT_ENUM_VALUE_NOT_FOUND,
            "type", klass->name->chars,
            "value", value_to_string(args[1])->chars, NULL));
    return NIL_VAL;
}

/* EnumérationDepuisNom(Enum, nom [, défaut]) -> membre.
 * Recherche par nom de membre. Introuvable sans défaut : exception. */
static Value fn_enumeration_depuis_nom(int argCount, Value* args) {
    if (!enum_require_type(args, "Enum\xc3\xa9rationDepuisNom")) return NIL_VAL;
    ObjClass* klass = AS_CLASS(args[0]);
    Value found = enum_find_by_field(klass, "Nom", args[1]);
    if (!IS_NIL(found)) return found;
    if (argCount >= 3) return args[2];
    owl_vm_raise_native("Enum\xc3\xa9rationDepuisNom",
        enum_error_code_name(E_RT_ENUM_NAME_NOT_FOUND),
        enum_render_active(E_RT_ENUM_NAME_NOT_FOUND,
            "type", klass->name->chars,
            "name", value_to_string(args[1])->chars, NULL));
    return NIL_VAL;
}

/* EnumérationVérifieValeur(Enum, valeur) -> booléen. */
static Value fn_enumeration_verifie_valeur(int argCount, Value* args) {
    (void)argCount;
    if (!enum_require_type(args, "Enum\xc3\xa9rationV\xc3\xa9rifieValeur")) return NIL_VAL;
    return BOOL_VAL(!IS_NIL(enum_find_by_field(AS_CLASS(args[0]), "Valeur", args[1])));
}

/* EnumérationVérifieNom(Enum, nom) -> booléen. */
static Value fn_enumeration_verifie_nom(int argCount, Value* args) {
    (void)argCount;
    if (!enum_require_type(args, "Enum\xc3\xa9rationV\xc3\xa9rifieNom")) return NIL_VAL;
    return BOOL_VAL(!IS_NIL(enum_find_by_field(AS_CLASS(args[0]), "Nom", args[1])));
}

/* ================================================================
 * Registration
 * ================================================================ */

static void define_global_int(const char* name, int64_t v) {
    /*  : delegue au canon obj_globals_set. */
    obj_globals_set(name, INTEGER_VAL(v));
}

/* ================================================================
 * Registry declarations — Reflection family.
 *
 * Canonical names in this module:
 *   - TypeVar, RécupèreDéfinition, RécupèreDéfinitionType.
 * OWL extensions (absent from the canon but idiomatic):
 *   - EstNul / IsNull — null check (canon compares against Null keyword)
 *   - VariableExiste / VariableExists — runtime presence check
 *   - TypeEstNullable — whether a value's declared type accepts Null
 * ================================================================ */

static const OwlParam p_val[] = { {"valeur", "tout", "valeur à examiner"}, {0} };
static const OwlParam p_tn[]  = { {"nom",    "chaîne",  "nom d'un type"},     {0} };

/* Famille Énumération* — 1er param = type d'énumération passé NU. */
static const OwlParam p_enum_v2[] = {
    {"énumération", "Énumération", "type d'énumération"},
    {"valeur",      "tout",        "valeur recherchée parmi les membres"},
    {0}
};
static const OwlParam p_enum_v3[] = {
    {"énumération", "Énumération", "type d'énumération"},
    {"valeur",      "tout",        "valeur recherchée parmi les membres"},
    {"défaut",      "Énumération", "membre renvoyé si la valeur est introuvable"},
    {0}
};
static const OwlParam p_enum_n2[] = {
    {"énumération", "Énumération", "type d'énumération"},
    {"nom",         "chaîne",      "nom de membre recherché"},
    {0}
};
static const OwlParam p_enum_n3[] = {
    {"énumération", "Énumération", "type d'énumération"},
    {"nom",         "chaîne",      "nom de membre recherché"},
    {"défaut",      "Énumération", "membre renvoyé si le nom est introuvable"},
    {0}
};

static const OwlSignature sig_enum_from_value[] = {
    {2, p_enum_v2, "Énumération", "membre dont la valeur correspond"},
    {3, p_enum_v3, "Énumération", "membre correspondant, ou le défaut si introuvable"},
    {0}
};
static const OwlSignature sig_enum_from_name[] = {
    {2, p_enum_n2, "Énumération", "membre portant ce nom"},
    {3, p_enum_n3, "Énumération", "membre correspondant, ou le défaut si introuvable"},
    {0}
};
static const OwlSignature sig_enum_check_value[] = {
    {2, p_enum_v2, "booléen", "Vrai si un membre porte cette valeur"},
    {0}
};
static const OwlSignature sig_enum_check_name[] = {
    {2, p_enum_n2, "booléen", "Vrai si un membre porte ce nom"},
    {0}
};

static const OwlSignature sig_int[]   = { {1, p_val, "entier",     "valeur de l'énumération owlTypes (owlEntier, owlChaîne, ...)"}, {0} };
static const OwlSignature sig_bool[]  = { {1, p_val, "booléen",    "Vrai / Faux"},      {0} };
static const OwlSignature sig_def[]   = { {1, p_val, "Définition", "description du type"}, {0} };
static const OwlSignature sig_defn[]  = { {1, p_tn,  "Définition", "description du type nommé"}, {0} };

static const OwlNativeDef REFLECT_DEFS[] = {
    {
        .canonical  = "TypeVar",
        .nameEN     = "TypeVar",
        .category   = "Introspection",
        .descFR     = "Retourne le code de type d'une variable. La valeur retournée est l'une des constantes de l'énumération owlTypes (owlEntier, owlChaîne, owlBooléen, etc.).",
        .descEN     = "Return the type code of a variable. The returned value is one of the constants of the owlTypes enumeration (owlEntier, owlChaine, owlBooleen, etc.).",
        .signatures = sig_int,
        .impl       = fn_type_var,
    },
    {
        .canonical  = "EstNul",
        .nameEN     = "IsNull",
        .category   = "Introspection",
        .descFR     = "Vrai si la valeur vaut Null (extension OWL).",
        .descEN     = "True if the value is Null (OWL extension).",
        .signatures = sig_bool,
        .impl       = fn_est_nul,
    },
    {
        .canonical  = "VariableExiste",
        .nameEN     = "VariableExists",
        .category   = "Introspection",
        .descFR     = "Vrai si une variable/fonction globale porte ce nom (extension OWL).",
        .descEN     = "True if a global variable/function with this name exists (OWL extension).",
        .signatures = sig_bool,
        .impl       = fn_variable_existe,
    },
    {
        .canonical  = "TypeEstNullable",
        .nameEN     = "TypeIsNullable",
        .category   = "Introspection",
        .descFR     = "Vrai si le type déclaré accepte Null (extension OWL).",
        .descEN     = "True if the declared type accepts Null (OWL extension).",
        .signatures = sig_bool,
        .impl       = fn_type_est_nullable,
    },
    {
        .canonical      = "R\xc3\xa9""cup\xc3\xa8" "reD\xc3\xa9" "finition",       /* RécupèreDéfinition */
        .canonicalAscii = "RecupereDefinition",
        .nameEN         = "GetDefinition",
        .category       = "Introspection",
        .descFR         = "Retourne la description runtime du type d'une valeur.",
        .descEN         = "Return the runtime type description of a value.",
        .signatures     = sig_def,
        .impl           = fn_recupere_definition,
    },
    {
        .canonical      = "R\xc3\xa9""cup\xc3\xa8" "reD\xc3\xa9" "finitionType",   /* RécupèreDéfinitionType */
        .canonicalAscii = "RecupereDefinitionType",
        .nameEN         = "GetTypeDefinition",
        .category       = "Introspection",
        .descFR         = "Retourne la description d'un type nommé (structure, classe, énumération).",
        .descEN         = "Return the description of a named type (structure, class, enumeration).",
        .signatures     = sig_defn,
        .impl           = fn_recupere_definition_type,
    },
    {
        .canonical      = "Enum\xc3\xa9rationDepuisValeur",      /* EnumérationDepuisValeur */
        .canonicalAscii = "EnumerationDepuisValeur",
        .nameEN         = "EnumerationFromValue",
        .category       = "Introspection",
        .descFR         = "Retrouve le membre d'une énumération à partir de sa valeur (recherche inverse valeur vers membre). Sans valeur par défaut, une valeur introuvable lève une exception ; avec un défaut, ce défaut est renvoyé.",
        .descEN         = "Find an enumeration member from its value (reverse value-to-member lookup). Without a default, an unknown value raises an exception; with a default, that default is returned.",
        .signatures     = sig_enum_from_value,
        .impl           = fn_enumeration_depuis_valeur,
    },
    {
        .canonical      = "Enum\xc3\xa9rationDepuisNom",         /* EnumérationDepuisNom */
        .canonicalAscii = "EnumerationDepuisNom",
        .nameEN         = "EnumerationFromName",
        .category       = "Introspection",
        .descFR         = "Retrouve le membre d'une énumération à partir de son nom. Sans valeur par défaut, un nom introuvable lève une exception ; avec un défaut, ce défaut est renvoyé.",
        .descEN         = "Find an enumeration member from its name. Without a default, an unknown name raises an exception; with a default, that default is returned.",
        .signatures     = sig_enum_from_name,
        .impl           = fn_enumeration_depuis_nom,
    },
    {
        .canonical      = "Enum\xc3\xa9rationV\xc3\xa9rifieValeur",   /* EnumérationVérifieValeur */
        .canonicalAscii = "EnumerationVerifieValeur",
        .nameEN         = "EnumerationCheckValue",
        .category       = "Introspection",
        .descFR         = "Indique si une valeur correspond à un membre d'une énumération, sans faire échouer l'appelant sur une valeur inconnue.",
        .descEN         = "Report whether a value matches an enumeration member, without failing the caller on an unknown value.",
        .signatures     = sig_enum_check_value,
        .impl           = fn_enumeration_verifie_valeur,
    },
    {
        .canonical      = "Enum\xc3\xa9rationV\xc3\xa9rifieNom",      /* EnumérationVérifieNom */
        .canonicalAscii = "EnumerationVerifieNom",
        .nameEN         = "EnumerationCheckName",
        .category       = "Introspection",
        .descFR         = "Indique si un nom correspond à un membre d'une énumération, sans faire échouer l'appelant sur un nom inconnu.",
        .descEN         = "Report whether a name matches an enumeration member, without failing the caller on an unknown name.",
        .signatures     = sig_enum_check_name,
        .impl           = fn_enumeration_verifie_nom,
    },
};

/* ---- Enums for Definition fields (Type, FamilleDéfinition) ---- */
static const char* const E_owlTypes_values[] = {
    "owlBooleen",
    "owlEntier", "owlReel", "owlMonetaire", "owlNumerique",
    "owlChaine", "owlBuffer", "owlUUID",
    "owlDate", "owlHeure", "owlDateHeure", "owlDuree",
    "owlTableau", "owlVecteur", "owlTableauAssociatif", "owlFile", "owlPile", "owlListe",
    "owlStructure", "owlClasse", "owlEnumeration", "owlCombinaison", "owlInstance",
    "owlJSON", "owlProcedure",
    "owlEtat", "owlFenetre",
    "owlChampLibelle", "owlChampBouton", "owlChampSaisie",
    "owlChampTable", "owlChampConteneur", "owlChampImage", "owlChampListe", "owlChampCombo",
    "owlChampSelecteur", "owlChampInterrupteur", "owlChampZoneRepetee",
    NULL
};
const OwlEnum E_owlTypes = {
    .name        = "owlTypes",
    .nameAscii   = "owlTypes",
    .nameEN      = "owlTypes",
    .category    = "Introspection",
    .descFR      = "Codes de types runtime (retournés par TypeVar).",
    .descEN      = "Runtime type codes (returned by TypeVar).",
    .values      = E_owlTypes_values,
    .combinable  = false,
};

static const char* const E_familleDefinition_values[] = {
    "famillePasDefini", "familleTypeSimple", "familleTypeAvance",
    "familleTableau", "familleCollection",
    "familleStructure", "familleClasse", "familleEnumeration", "familleCombinaison",
    "familleJSON", "familleProcedure",
    "familleEtat", "familleFenetre", "familleChamp",
    NULL
};
const OwlEnum E_familleDefinition = {
    .name        = "familleD\xc3\xa9" "finition",  /* familleDéfinition */
    .nameAscii   = "familleDefinition",
    .nameEN      = "definitionFamily",
    .category    = "Introspection",
    .descFR      = "Familles de types (Definition.FamilleDéfinition).",
    .descEN      = "Type families (Definition.FamilleDefinition).",
    .values      = E_familleDefinition_values,
    .combinable  = false,
};

static const OwlEnum* const REFLECT_ENUMS[] = {
    &E_owlTypes, &E_familleDefinition
};

void stdlib_reflect_init(void) {
    owl_registry_register_module(REFLECT_DEFS, (int)(sizeof(REFLECT_DEFS)/sizeof(REFLECT_DEFS[0])));
    owl_registry_register_enums (REFLECT_ENUMS, (int)(sizeof(REFLECT_ENUMS)/sizeof(REFLECT_ENUMS[0])));

    /* owl* type constants — pas de owlPasDefini : pas de constante canon
     * exposée pour la valeur 0. Le runtime peut encore retourner
     * WL_PAS_DEFINI (=0) en interne pour les cas indéterminés (ex: TypeVar
     * sur valeur Null), mais sans label canon exposé. */
    define_global_int("owlBooleen",            WL_BOOLEEN);
    define_global_int("owlEntier",             WL_ENTIER);
    define_global_int("owlReel",               WL_REEL);
    define_global_int("owlMonetaire",          WL_MONETAIRE);
    define_global_int("owlNumerique",          WL_NUMERIQUE);
    define_global_int("owlChaine",             WL_CHAINE);
    define_global_int("owlBuffer",             WL_BUFFER);
    define_global_int("owlUUID",               WL_UUID);
    define_global_int("owlDate",               WL_DATE);
    define_global_int("owlHeure",              WL_HEURE);
    define_global_int("owlDateHeure",          WL_DATE_HEURE);
    define_global_int("owlDuree",              WL_DUREE);
    define_global_int("owlTableau",            WL_TABLEAU);
    define_global_int("owlVecteur",            WL_VECTEUR);
    define_global_int("owlTableauAssociatif",  WL_TABLEAU_ASSOCIATIF);
    define_global_int("owlFile",               WL_FILE);
    define_global_int("owlPile",               WL_PILE);
    define_global_int("owlListe",              WL_LISTE);
    define_global_int("owlStructure",          WL_STRUCTURE);
    define_global_int("owlClasse",             WL_CLASSE);
    define_global_int("owlEnumeration",        WL_ENUMERATION);
    define_global_int("owlCombinaison",        WL_COMBINAISON);
    define_global_int("owlInstance",           WL_INSTANCE);
    define_global_int("owlJSON",               WL_JSON);
    define_global_int("owlProcedure",          WL_PROCEDURE);
    define_global_int("owlEtat",               WL_ETAT);
    define_global_int("owlFenetre",            WL_FENETRE);
    define_global_int("owlChampLibelle",       WL_CHAMP_LIBELLE);
    define_global_int("owlChampBouton",        WL_CHAMP_BOUTON);
    define_global_int("owlChampSaisie",        WL_CHAMP_SAISIE);
    define_global_int("owlChampTable",         WL_CHAMP_TABLE);
    define_global_int("owlChampConteneur",     WL_CHAMP_CONTENEUR);
    define_global_int("owlChampImage",         WL_CHAMP_IMAGE);
    define_global_int("owlChampListe",         WL_CHAMP_LISTE);
    define_global_int("owlChampCombo",         WL_CHAMP_COMBO);
    define_global_int("owlChampSelecteur",     WL_CHAMP_SELECTEUR);
    define_global_int("owlChampInterrupteur",  WL_CHAMP_INTERRUPTEUR);
    define_global_int("owlChampZoneRepetee",   WL_CHAMP_ZONE_REPETEE);
    define_global_int("owlChampJauge",          WL_CHAMP_JAUGE);

    /* familleDéfinition constants */
    define_global_int("famillePasDefini",     FAM_PAS_DEFINI);
    define_global_int("familleTypeSimple",    FAM_TYPE_SIMPLE);
    define_global_int("familleTypeAvance",    FAM_TYPE_AVANCE);
    define_global_int("familleTableau",       FAM_TABLEAU);
    define_global_int("familleCollection",    FAM_COLLECTION);
    define_global_int("familleStructure",     FAM_STRUCTURE);
    define_global_int("familleClasse",        FAM_CLASSE);
    define_global_int("familleEnumeration",   FAM_ENUMERATION);
    define_global_int("familleCombinaison",   FAM_COMBINAISON);
    define_global_int("familleJSON",          FAM_JSON);
    define_global_int("familleProcedure",     FAM_PROCEDURE);
    define_global_int("familleEtat",          FAM_ETAT);
    define_global_int("familleFenetre",       FAM_FENETRE);
    define_global_int("familleChamp",         FAM_CHAMP);
}
