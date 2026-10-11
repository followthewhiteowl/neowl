// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "scanner.h"
#include "vm.h"
#include "sentinels.h"
#include "string_compare_opts.h"
#include <string.h>
#include <ctype.h>

/* ================================================================
 * ARRAY FUNCTIONS
 * All positions are BASE 1!
 * ================================================================ */

/* --- Trie/Sort mode constants (match values registered in stdlib_array_init) ---
 *
 * Le mode est un entier bit-packed :
 *   - low byte (0x00FF)   : direction/mode primaire (mutex)
 *       1=ttCroissant, 2=ttDecroissant,
 *       3=ttColonne (2D multi-col via string spec),
 *       4=ttMembre  (struct via string spec),
 *       5=ttFonction (procedure custom)
 *   - bits 8-15 (0xFF00)  : tcc* options (combinables avec direction)
 *   - bit 16 (0x10000)    : ttCle — sur assoc, tri sur clés (sinon valeurs)
 */
#define TT_CROISSANT   1
#define TT_DECROISSANT 2
#define TT_COLONNE     3
#define TT_MEMBRE      4
#define TT_FONCTION    5

#define TCC_SANS_CASSE           0x0100
#define TCC_SANS_ACCENT          0x0200
#define TCC_SANS_ESPACE          0x0400
#define TCC_SANS_ESPACE_INT      0x0800
#define TCC_SANS_PONCTUATION     0x1000
#define TCC_RESPECTE_NUMERIQUE   0x2000
#define TCC_ORDRE_LEXICOGRAPHIQUE 0x4000

#define TT_CLE                   0x10000

/* --- TableauCherche mode constants --- */
#define TC_LINEAIRE          1
#define TC_LINEAIRE_PREMIER  2
#define TC_LINEAIRE_SUIVANT  3
#define TC_LINEAIRE_DERNIER  4
#define TC_LINEAIRE_PRECEDENT 5
#define TC_DICHOTOMIQUE      6

/* --- TableauInfo type constants (doc 3075005) ---
 * Sentinelles centralisees dans sentinels.h. */
#include "sentinels.h"
#define TI_NOMBRE_TOTAL        OWL_TI_NOMBRE_TOTAL
#define TI_NOMBRE_DIMENSIONS   OWL_TI_NOMBRE_DIMENSIONS
#define TI_NOMBRE_LIGNES       OWL_TI_NOMBRE_LIGNES
#define TI_NOMBRE_COLONNES     OWL_TI_NOMBRE_COLONNES
#define TI_DIMENSION           OWL_TI_DIMENSION
#define TI_DYNAMIQUE           OWL_TI_DYNAMIQUE
#define TI_TAILLE_ELEMENT      OWL_TI_TAILLE_ELEMENT
#define TI_TAILLE_TOTALE       OWL_TI_TAILLE_TOTALE
#define TI_TYPE_ELEMENT        OWL_TI_TYPE_ELEMENT
#define TI_TYPE_CLE            OWL_TI_TYPE_CLE
#define TI_ASSOCIATIF_DOUBLON  OWL_TI_ASSOCIATIF_DOUBLON
#define TI_DEFINITION_ELEMENT  12

/* Traversal state for tcLineaireSuivant/tcLineairePrecedent */
static int g_cherche_pos = -1;  /* last found index (0-based), -1 = no state */

/* Compare two values for sort. Returns <0, 0, >0. */
static int compare_values_sort(Value a, Value b, int opts) {
    if (IS_INTEGER(a) && IS_INTEGER(b)) {
        int64_t ia = AS_INTEGER(a), ib = AS_INTEGER(b);
        return (ia > ib) - (ia < ib);
    }
    if ((IS_INTEGER(a) || IS_REAL(a)) && (IS_INTEGER(b) || IS_REAL(b))) {
        double da = IS_INTEGER(a) ? (double)AS_INTEGER(a) : AS_REAL(a);
        double db = IS_INTEGER(b) ? (double)AS_INTEGER(b) : AS_REAL(b);
        return (da > db) - (da < db);
    }
    if (IS_STRING(a) && IS_STRING(b)) {
        /* Delegue au helper unique partage avec ChaineCompare. La conversion
         * TCC_* (high-byte) -> OWLCMP_* (low-byte) gere aussi le swap
         * historique lex<->num entre les deux jeux de constantes. */
        int cmp_flags = owl_tcc_to_cmp_flags(opts);
        return owl_string_compare_with_opts(AS_CSTRING(a), AS_CSTRING(b), cmp_flags);
    }
    return 0;
}

/* Ajoute(tab, val) / Add(tab, val) - append to array */

/* Modele d'instance d'un type d'element, quel que soit son namespace.
 *
 * Les types composes du registre (Police, Cadre, httpRequête…) vivent dans
 * vm.types ; les structures declarees en OWL restent dans vm.globals. Le
 * lookup ne visait que la seconde table, anterieure a la separation des
 * namespaces — d'ou une coercion morte sur la totalite des types
 * du registre. Les deux tables sont interrogees via leur index canon, pour
 * que `tableau de police` vaille `tableau de Police` (invariants 1 et 2). */
static bool lookup_element_template(ObjString* typeName, Value* out) {
    char canonBuf[256];
    int canonLen = scanner_canonicalize_identifier(typeName->chars,
                                                   typeName->length,
                                                   canonBuf, sizeof(canonBuf));
    ObjString* canonKey = obj_copy_string(canonBuf, canonLen);
    vm_push(OBJ_VAL(canonKey));
    Value verbatim;
    bool found = false;
    if (table_get(&vm.types_canon_idx, canonKey, &verbatim)) {
        found = table_get(&vm.types, AS_STRING(verbatim), out);
    }
    if (!found && table_get(&vm.globals_canon_idx, canonKey, &verbatim)) {
        found = table_get(&vm.globals, AS_STRING(verbatim), out);
    }
    vm_pop(); /* canonKey */
    return found;
}

/* Nombre de champs declares du type, et acces au i-eme nom de champ.
 *
 * Deux sources d'ordre, exclusives : `fieldOrder`, rempli par le compilateur
 * pour les structures ecrites en OWL ; le registre pour les types composes,
 * dont les modeles sont construits en C par inst_field_set_* et n'ont donc
 * pas de fieldOrder. Sans cette seconde source la coercion resterait morte
 * sur ces types meme apres correction du namespace. */
static int template_field_count(ObjInstance* templ, const OwlTypeDef* def) {
    if (templ->fieldOrder != NULL) return templ->fieldOrder->elements.count;
    int n = 0;
    while (def->fields[n].name != NULL) n++;
    return n;
}

static ObjString* template_field_name(ObjInstance* templ, const OwlTypeDef* def, int i) {
    if (templ->fieldOrder != NULL) {
        Value fname = templ->fieldOrder->elements.values[i];
        return IS_STRING(fname) ? AS_STRING(fname) : NULL;
    }
    const char* name = def->fields[i].name;
    return obj_copy_string(name, (int)strlen(name));
}

/* Coerce an ObjArray literal into a struct instance by positional field match.
 * Returns a new ObjInstance* filled with array[i] -> field[i], or NULL on failure. */
static ObjInstance* coerce_array_to_struct(ObjArray* src, ObjString* structName,
                                           bool* isComposite) {
    *isComposite = false;
    if (src == NULL || structName == NULL) return NULL;
    Value templVal;
    if (!lookup_element_template(structName, &templVal)) return NULL;
    if (!IS_INSTANCE(templVal)) return NULL;
    ObjInstance* templ = AS_INSTANCE(templVal);
    /* A partir d'ici le type d'element est bien un type compose : la coercion
     * s'applique, donc son echec est une anomalie a signaler et non un cas ou
     * la concatenation serait le comportement attendu. */
    *isComposite = true;

    const OwlTypeDef* def = NULL;
    if (templ->fieldOrder == NULL) {
        def = owl_registry_type_find(structName->chars);
        if (def == NULL || def->fields == NULL) return NULL;
    }
    int nFields = template_field_count(templ, def);
    int nVals   = src->elements.count;
    /* Plus de valeurs que de champs : les valeurs en trop seraient perdues
     * sans un mot. Le caller signale. */
    if (nFields == 0 || nVals > nFields) return NULL;

    /* Copie profonde du modele plutot que reconstruction champ par champ :
     * les defauts composes (sous-instances Police/Cadre d'un Style, tableaux
     * membres) doivent appartenir a l'element, pas etre partages avec le
     * modele du type ni entre elements. */
    Value instVal = owl_value_deep_clone(templVal);
    ObjInstance* inst = AS_INSTANCE(instVal);
    /*  : root inst pendant les iterations (canon obj_inst_set_kv
     * push inst par appel mais pas ENTRE deux iterations — un GC entre
     * deux table_set peut collecter inst). */
    vm_push(instVal);
    inst->fieldOrder = templ->fieldOrder;

    for (int i = 0; i < nVals; i++) {
        ObjString* fname = template_field_name(templ, def, i);
        if (fname == NULL) continue;
        obj_inst_set_kv(inst, fname, src->elements.values[i]);
    }
    vm_pop(); /* inst */
    return inst;
}

/* Renvoie le tableau receveur (args[0]) pour permettre le chainage,
 * cote forme fonction comme cote forme methode. Divergence assumee :
 * doc historique renvoyait l'indice de l'element ajoute.  trace
 * la decision et le pattern famille. */
static Value fn_ajoute(int argCount, Value* args) {
    (void)argCount;
    if (!IS_ARRAY(args[0])) return args[0];
    ObjArray* array = AS_ARRAY(args[0]);

    /* If the target array is typed `tableau de <Structure>` AND the value is
     * an array literal, coerce it positionnally into a struct instance. */
    if (IS_ARRAY(args[1]) && array->elementStructType != NULL) {
        bool isComposite = false;
        ObjInstance* inst = coerce_array_to_struct(AS_ARRAY(args[1]),
                                                   array->elementStructType,
                                                   &isComposite);
        if (inst != NULL) {
            obj_array_push(array, OBJ_VAL(inst));
            return args[0];
        }
        /* Echec sur un type compose : le repli sur la concatenation rendait un
         * tableau peuple de morceaux du litteral, sans un mot. C'est ce silence
         * qui a masque le mauvais namespace. */
        if (isComposite) {
            owl_emit_native_error(E_NATIVE_ARRAY_ELEMENT_COERCE_FAILED, "Ajoute",
                                  "type", array->elementStructType->chars,
                                  NULL);
            return args[0];
        }
        /* Type d'element sans modele d'instance (classe, enumeration) : rien a
         * coercer, la concatenation reste le comportement. */
    }

    /* Si la valeur passee est elle-meme un tableau, on concatene ses
     * elements (flatten un niveau) plutot que d'imbriquer.
     *  : obj_array_push push values (potentiellement sub-arrays)
     * + array safe pendant les grows successifs. */
    if (IS_ARRAY(args[1])) {
        ObjArray* other = AS_ARRAY(args[1]);
        for (int i = 0; i < other->elements.count; i++) {
            obj_array_push(array, other->elements.values[i]);
        }
        return args[0];
    }

    obj_array_push(array, args[1]);
    return args[0];
}

/* Supprime(tab, indice|clé) / Delete / TableauSupprime
 * 3 formes :
 *   - tableau 1D    : Supprime(tab, indice 1-based)
 *   - tableau assoc : Supprime(assoc, clé)
 *   - liste         : Supprime(liste, indice) (déjà géré par méthode prefix)
 *
 * Sentinels acceptés en 2e argument :
 *   - ÉlémentCourant : utilise vm.iterCurrentKey du POUR TOUT actif
 *   - PremierÉlément : indice 1
 *   - DernierÉlément : indice = .Occurrence
 *
 * Renvoie le tableau receveur (args[0]) pour permettre le chainage,
 * cote forme fonction comme cote forme methode. Divergence assumee :
 * canon historique renvoyait un Entier "toujours 1".  trace la
 * decision (Supprime = 2e cas B). Pour mesurer un delta, le pattern
 * documente est `..Occurrence` avant/apres l'appel. */
static Value fn_supprime(int argCount, Value* args) {
    (void)argCount;

    /* Résoudre les sentinels d'itération en clé concrète. */
    Value key = args[1];
    if (IS_INTEGER(key)) {
        int64_t k = AS_INTEGER(key);
        if (k == OWL_ELEMENT_COURANT) {
            if (vm.iterDepth > 0) {
                key = vm.iterCurrentKey[vm.iterDepth - 1];
            } else {
                return args[0];  /* pas de POUR TOUT actif */
            }
        } else if (k == OWL_PREMIER_ELEMENT) {
            key = INTEGER_VAL(1);
        } else if (k == OWL_DERNIER_ELEMENT) {
            int n = 0;
            if (IS_ARRAY(args[0])) n = AS_ARRAY(args[0])->elements.count;
            else if (IS_ASSOC_ARRAY(args[0])) n = AS_ASSOC_ARRAY(args[0])->count;
            else if (IS_COLLECTION(args[0])) n = AS_COLLECTION(args[0])->elements.count;
            if (n == 0) return args[0];
            key = INTEGER_VAL(n);
        }
    }

    /* Tableau associatif : suppression par clé (string). */
    if (IS_ASSOC_ARRAY(args[0])) {
        if (!IS_STRING(key)) return args[0];
        obj_assoc_remove(AS_ASSOC_ARRAY(args[0]), AS_STRING(key));
        return args[0];
    }
    if (!IS_ARRAY(args[0])) return args[0];
    ObjArray* array = AS_ARRAY(args[0]);

    int64_t pos = 1;
    if (IS_INTEGER(key)) pos = AS_INTEGER(key);
    else if (IS_REAL(key)) pos = (int64_t)AS_REAL(key);

    int idx = (int)(pos - 1); /* base-1 to base-0 */
    if (idx < 0 || idx >= array->elements.count) return args[0];

    for (int i = idx; i < array->elements.count - 1; i++) {
        array->elements.values[i] = array->elements.values[i + 1];
    }
    array->elements.count--;
    return args[0];
}

/* Insere(tab, pos, val) / Insert(tab, pos, val) - insert at position (base 1).
 * Renvoie le tableau receveur (pattern famille chainable, ). */
static Value fn_insere(int argCount, Value* args) {
    (void)argCount;
    /* Tableau associatif : Insere(tab, cle, valeur) -- alias de TableauAjoute */
    if (IS_ASSOC_ARRAY(args[0])) {
        if (!IS_STRING(args[1])) return args[0];
        obj_assoc_set(AS_ASSOC_ARRAY(args[0]), AS_STRING(args[1]), args[2]);
        return args[0];
    }
    if (!IS_ARRAY(args[0])) return args[0];
    ObjArray* array = AS_ARRAY(args[0]);

    int64_t pos = 1;
    if (IS_INTEGER(args[1])) pos = AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) pos = (int64_t)AS_REAL(args[1]);

    int idx = (int)(pos - 1); /* base-1 to base-0 */
    if (idx < 0) idx = 0;
    if (idx > array->elements.count) idx = array->elements.count;

    /* Grow array by 1 */
    value_array_write(&array->elements, NIL_VAL); /* placeholder */
    /* Shift elements right */
    for (int i = array->elements.count - 1; i > idx; i--) {
        array->elements.values[i] = array->elements.values[i - 1];
    }
    array->elements.values[idx] = args[2];
    return args[0];
}

/* Cherche(tab, val) / Seek(tab, val) - find position (base 1), 0 if not found */
/* Cherche — doc 1000017365. Plusieurs formes :
 *   Forme 1: Cherche(tab, val)                                  — 1D direct
 *   Forme 1b: Cherche(tab, mode, val)                           — 1D + mode (mode ignoré)
 *   Forme 4: Cherche(tab, mode, fieldName, val)                 — 1D structs, par membre
 * Le mode (tcLineaire* ou tcDichotomique) est ignoré : toutes les recherches
 * sont linéaires en OWL, même résultat modulo perf.
 * Retour: position base-1, -1 si absent. */
static Value fn_cherche(int argCount, Value* args) {
    /*  — garde défensive : la forme minimale est Cherche(tab, val)
     * = 2 args. Sous-application (ex. tab.Cherche() compilé hors validation,
     * ou appel dynamique) → pas de lecture hors-borne de args[1]. */
    if (argCount < 2) return INTEGER_VAL(-1);
    if (!IS_ARRAY(args[0])) return INTEGER_VAL(-1);
    ObjArray* array = AS_ARRAY(args[0]);

    /* Forme 4: tab, mode, fieldName(string), val → recherche par champ de structure */
    if (argCount >= 4 && IS_INTEGER(args[1]) && IS_STRING(args[2])) {
        ObjString* fieldName = AS_STRING(args[2]);
        Value needle = args[3];
        for (int i = 0; i < array->elements.count; i++) {
            Value elem = array->elements.values[i];
            if (IS_INSTANCE(elem)) {
                ObjInstance* inst = AS_INSTANCE(elem);
                Value fieldVal;
                if (table_get(&inst->fields, fieldName, &fieldVal)) {
                    if (values_equal(fieldVal, needle)) {
                        return INTEGER_VAL(i + 1);
                    }
                }
            }
        }
        return INTEGER_VAL(-1);
    }

    /* Forme 1b: Cherche(tab, mode, val) — mode ignoré */
    Value needle;
    if (argCount >= 3 && IS_INTEGER(args[1])) {
        needle = args[2];
    } else {
        needle = args[1];
    }

    for (int i = 0; i < array->elements.count; i++) {
        if (values_equal(array->elements.values[i], needle))
            return INTEGER_VAL(i + 1);
    }
    return INTEGER_VAL(-1);
}

/* Inverse(tab)    - reverse array in place (alias TableauInverse).
 * Inverse(chaine) - per-codepoint XOR 0xFFFF complement.
 *                   Implemented in stdlib_string.c::fn_inverse_chaine; we
 *                   delegate via owl_stdlib lookup to keep the behaviour single-source. */
extern Value fn_inverse_chaine(int argCount, Value* args);
extern Value fn_inverse_xor(int argCount, Value* args);
/* Helper: detect nb dimensions of a nested OWL array.
 * If no element is itself an array: 1D.
 * If all elements are arrays: 1 + max depth of sub-arrays. */
static int array_nb_dimensions(ObjArray* arr) {
    if (arr->elements.count == 0) return 1;
    int maxSubDepth = 0;
    bool anyNested = false;
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (IS_ARRAY(v)) {
            anyNested = true;
            int d = array_nb_dimensions(AS_ARRAY(v));
            if (d > maxSubDepth) maxSubDepth = d;
        }
    }
    return anyNested ? 1 + maxSubDepth : 1;
}

/* Helper: total element count (leaf cells) for nested array. */
static int array_total_leaf(ObjArray* arr) {
    int total = 0;
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (IS_ARRAY(v)) total += array_total_leaf(AS_ARRAY(v));
        else total++;
    }
    return total;
}

/* TableauInfo(tab, infoType [, param]) — doc 3075005.
 * Retourne des infos sur la structure du tableau. */
static Value fn_tableau_info(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return INTEGER_VAL(0);
    ObjArray* arr = AS_ARRAY(args[0]);
    if (argCount < 2 || !IS_INTEGER(args[1])) return INTEGER_VAL(0);
    int infoType = (int)AS_INTEGER(args[1]);
    int param = (argCount >= 3 && IS_INTEGER(args[2])) ? (int)AS_INTEGER(args[2]) : 1;

    switch (infoType) {
        case TI_NOMBRE_TOTAL:
            return INTEGER_VAL(array_total_leaf(arr));
        case TI_NOMBRE_DIMENSIONS:
            return INTEGER_VAL(array_nb_dimensions(arr));
        case TI_NOMBRE_LIGNES: {
            /* Only meaningful for 2D arrays */
            int dims = array_nb_dimensions(arr);
            if (dims == 2) return INTEGER_VAL(arr->elements.count);
            return INTEGER_VAL(0);
        }
        case TI_NOMBRE_COLONNES: {
            int dims = array_nb_dimensions(arr);
            if (dims == 2 && arr->elements.count > 0 &&
                IS_ARRAY(arr->elements.values[0])) {
                return INTEGER_VAL(AS_ARRAY(arr->elements.values[0])->elements.count);
            }
            return INTEGER_VAL(0);
        }
        case TI_DIMENSION: {
            /* nb elements in given dimension (param = 1-indexed dim) */
            if (param < 1) return INTEGER_VAL(0);
            ObjArray* cur = arr;
            for (int d = 1; d < param; d++) {
                if (cur->elements.count == 0 || !IS_ARRAY(cur->elements.values[0])) {
                    return INTEGER_VAL(0);
                }
                cur = AS_ARRAY(cur->elements.values[0]);
            }
            return INTEGER_VAL(cur->elements.count);
        }
        case TI_DYNAMIQUE:
            /* OWL n'a pas de distinction explicite fixe/dynamique;
             * growthMode != 0 signifie qu'il peut grandir automatiquement. */
            return BOOL_VAL(arr->growthMode != 0);
        case TI_TAILLE_ELEMENT:
            /* Taille mémoire par élément : non calculable en OWL (Value = union). */
            return INTEGER_VAL(sizeof(Value));
        case TI_TAILLE_TOTALE:
            return INTEGER_VAL((int)(arr->elements.count * sizeof(Value)));
        case TI_ASSOCIATIF_DOUBLON:
            return BOOL_VAL(false); /* ObjArray n'est pas associatif */
        default:
            return INTEGER_VAL(0);
    }
}

/* Inverse(tab) renvoie le tableau receveur (pattern famille chainable,
 * ). Branche chaine reste inchangee : Inverse(chaine) renvoie une
 * nouvelle chaine (XOR codepoint 0xFFFF). */
static Value fn_inverse(int argCount, Value* args) {
    if (IS_STRING(args[0])) {
        /* Fonction globale Inverse(chaîne) = XOR codepoint 0xFFFF (doc
         * 3024028, mode Unicode). Different de la methode chaîne.Inverse()
         * qui inverse l'ordre (alias de ChaîneInverse). */
        return fn_inverse_xor(argCount, args);
    }
    if (!IS_ARRAY(args[0])) return args[0];
    ObjArray* array = AS_ARRAY(args[0]);
    int n = array->elements.count;
    for (int i = 0; i < n / 2; i++) {
        Value tmp = array->elements.values[i];
        array->elements.values[i] = array->elements.values[n - 1 - i];
        array->elements.values[n - 1 - i] = tmp;
    }
    return args[0];
}

/* Trie(tab [, mode]) — sort array in place.
 * mode = ttCroissant (default) | ttDecroissant, combinable with tcc* flags. */
/* --- Sort context et compare central pour TableauTrie/Trie. ---
 * Le SortContext capte le mode + spec multi-clés / callback / opts. */

#define MAX_SORT_KEYS 8

typedef struct {
    int  colIdx;               /* base 1 ; 0 = non utilisé (ttMembre) */
    char fieldName[64];        /* pour ttMembre */
    bool descending;
} SortKey;

typedef struct {
    int     mode;              /* TT_CROISSANT/DECROISSANT/COLONNE/MEMBRE/FONCTION */
    int     opts;              /* tcc* flags (bits 8-15) */
    bool    sortByKey;         /* assoc : trier selon clés (bit TT_CLE) */
    bool    descending;        /* pour TT_CROISSANT/TT_DECROISSANT */
    SortKey keys[MAX_SORT_KEYS];
    int     nKeys;
    Value   callback;          /* pour TT_FONCTION */
    bool    aborted;           /* TT_FONCTION : le callback réentrant a levé une
                                * erreur non gérée localement -> arrêt du tri au
                                * 1er échec (une seule diag, pas de relance). */
} SortContext;

/* Parse une string "col;-col;+col2" → SortKey[] (cas TT_COLONNE).
 * Retourne le nombre de clés extraites. */
static int parse_columns_spec(const char* spec, SortKey out[], int max) {
    int n = 0;
    const char* p = spec;
    while (*p && n < max) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        bool desc = false;
        if (*p == '-') { desc = true; p++; }
        else if (*p == '+') { p++; }
        int col = 0;
        while (*p >= '0' && *p <= '9') {
            col = col * 10 + (*p - '0');
            p++;
        }
        if (col > 0) {
            out[n].colIdx = col;
            out[n].fieldName[0] = '\0';
            out[n].descending = desc;
            n++;
        }
        while (*p && *p != ';') p++;
    }
    return n;
}

/* Parse "Membre;-Membre2;+Membre3" → SortKey[] (cas TT_MEMBRE). */
static int parse_members_spec(const char* spec, SortKey out[], int max) {
    int n = 0;
    const char* p = spec;
    while (*p && n < max) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        bool desc = false;
        if (*p == '-') { desc = true; p++; }
        else if (*p == '+') { p++; }
        int len = 0;
        while (*p && *p != ';' && len < (int)sizeof(out[n].fieldName) - 1) {
            out[n].fieldName[len++] = *p++;
        }
        out[n].fieldName[len] = '\0';
        if (len > 0) {
            out[n].colIdx = 0;
            out[n].descending = desc;
            n++;
        }
        while (*p && *p != ';') p++;
    }
    return n;
}

/* Forward declarations utilisées par sort_compare_ctx (callbacks user). */
extern InterpretResult vm_call_reentrant(Value callee, int argCount);
extern void  vm_push(Value v);
extern Value vm_pop(void);
static int   callable_arity(Value cb);

/* Comparaison universelle pilotée par le SortContext.
 * Renvoie <0, 0, >0 (avec polarité descending déjà appliquée).
 * Pour TT_FONCTION : invoque la callback user (1D : 2 args, return entier).
 * Pour assoc + ttCle : a/b doivent être les CLES, key1/key2 les VALEURS
 * (pour secondary sort) ; sinon a/b = valeurs et key1/key2 = clés. */
static int sort_compare_ctx(SortContext* ctx,
                            Value a, Value b,
                            Value key1, Value key2) {
    switch (ctx->mode) {
        case TT_CROISSANT:
        case TT_DECROISSANT: {
            int cmp = compare_values_sort(a, b, ctx->opts);
            return ctx->descending ? -cmp : cmp;
        }
        case TT_COLONNE: {
            for (int k = 0; k < ctx->nKeys; k++) {
                int col = ctx->keys[k].colIdx;
                Value va = NIL_VAL, vb = NIL_VAL;
                if (IS_ARRAY(a)) {
                    ObjArray* ra = AS_ARRAY(a);
                    if (col >= 1 && col <= ra->elements.count) va = ra->elements.values[col - 1];
                }
                if (IS_ARRAY(b)) {
                    ObjArray* rb = AS_ARRAY(b);
                    if (col >= 1 && col <= rb->elements.count) vb = rb->elements.values[col - 1];
                }
                int cmp = compare_values_sort(va, vb, ctx->opts);
                if (ctx->keys[k].descending) cmp = -cmp;
                if (cmp != 0) return cmp;
            }
            return 0;
        }
        case TT_MEMBRE: {
            for (int k = 0; k < ctx->nKeys; k++) {
                Value va = NIL_VAL, vb = NIL_VAL;
                if (IS_INSTANCE(a)) {
                    ObjString* fname = obj_copy_string(ctx->keys[k].fieldName,
                                                       (int)strlen(ctx->keys[k].fieldName));
                    table_get(&AS_INSTANCE(a)->fields, fname, &va);
                }
                if (IS_INSTANCE(b)) {
                    ObjString* fname = obj_copy_string(ctx->keys[k].fieldName,
                                                       (int)strlen(ctx->keys[k].fieldName));
                    table_get(&AS_INSTANCE(b)->fields, fname, &vb);
                }
                int cmp = compare_values_sort(va, vb, ctx->opts);
                if (ctx->keys[k].descending) cmp = -cmp;
                if (cmp != 0) return cmp;
            }
            return 0;
        }
        case TT_FONCTION: {
            int ar = callable_arity(ctx->callback);
            int n = (ar == 4) ? 4 : 2;
            vm_push(ctx->callback);
            vm_push(a);
            if (n == 4) {
                vm_push(key1);
                vm_push(b);
                vm_push(key2);
            } else {
                vm_push(b);
            }
            InterpretResult r = vm_call_reentrant(ctx->callback, n);
            if (r != INTERPRET_OK) {
                /* Le callback a levé une erreur non rattrapée localement
                 * (exception en vol vers un ancêtre, ou cas terminal sans
                 * handler). On signale l'abandon : la boucle de tri s'arrête
                 * au 1er échec au lieu de relancer le comparateur, ce qui
                 * évite un second diagnostic pour la même racine. */
                ctx->aborted = true;
                return 0;
            }
            Value ret = vm_pop();
            if (IS_INTEGER(ret)) return (int)AS_INTEGER(ret);
            if (IS_REAL(ret))    return (AS_REAL(ret) > 0) - (AS_REAL(ret) < 0);
            return 0;
        }
    }
    return 0;
}

/* Construit le SortContext à partir des arguments de TableauTrie/Trie. */
static bool build_sort_context(int argCount, Value* args, bool isAssoc,
                               SortContext* ctx) {
    ctx->mode = TT_CROISSANT;
    ctx->opts = 0;
    ctx->sortByKey = false;
    ctx->descending = false;
    ctx->nKeys = 0;
    ctx->callback = NIL_VAL;
    ctx->aborted = false;

    int rawMode = TT_CROISSANT;
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        rawMode = (int)AS_INTEGER(args[1]);
    }
    int direction = (rawMode & 0xFF);
    ctx->opts = rawMode & 0xFF00;
    ctx->sortByKey = (rawMode & TT_CLE) != 0;

    /* Forme courte 2D : TableauTrie(arr, ttCroissant|ttDecroissant, col_int) */
    if (argCount == 3 && IS_INTEGER(args[2]) &&
        (direction == TT_CROISSANT || direction == TT_DECROISSANT)) {
        ctx->mode = TT_COLONNE;
        ctx->keys[0].colIdx = (int)AS_INTEGER(args[2]);
        ctx->keys[0].fieldName[0] = '\0';
        ctx->keys[0].descending = (direction == TT_DECROISSANT);
        ctx->nKeys = 1;
        return true;
    }

    if (direction == TT_COLONNE) {
        if (argCount < 3 || !IS_STRING(args[2])) return false;
        ctx->mode = TT_COLONNE;
        ctx->nKeys = parse_columns_spec(AS_CSTRING(args[2]), ctx->keys, MAX_SORT_KEYS);
        return ctx->nKeys > 0;
    }
    if (direction == TT_MEMBRE) {
        if (argCount < 3 || !IS_STRING(args[2])) return false;
        ctx->mode = TT_MEMBRE;
        ctx->nKeys = parse_members_spec(AS_CSTRING(args[2]), ctx->keys, MAX_SORT_KEYS);
        return ctx->nKeys > 0;
    }
    if (direction == TT_FONCTION) {
        if (argCount < 3) return false;
        Value cb = args[2];
        if (!IS_FUNCTION(cb) && !IS_NATIVE(cb)) return false;
        ctx->mode = TT_FONCTION;
        ctx->callback = cb;
        return true;
    }

    /* ttCroissant / ttDecroissant simple (1D ou assoc) */
    ctx->mode = TT_CROISSANT;
    ctx->descending = (direction == TT_DECROISSANT);
    (void)isAssoc;
    return true;
}

/* Tri d'un tableau 1D in-place (insertion sort stable). */
static void sort_array_1d(ObjArray* array, SortContext* ctx) {
    int n = array->elements.count;
    for (int i = 1; i < n; i++) {
        Value key = array->elements.values[i];
        int j = i - 1;
        while (j >= 0) {
            int cmp = sort_compare_ctx(ctx, array->elements.values[j], key,
                                       INTEGER_VAL(j + 1), INTEGER_VAL(i + 1));
            if (ctx->aborted) return;   /* callback en échec : arrêt net du tri */
            if (cmp <= 0) break;
            array->elements.values[j + 1] = array->elements.values[j];
            j--;
        }
        array->elements.values[j + 1] = key;
    }
}

/* Tri d'un assoc in-place : on sort les paires (keys[i], values[i]) ensemble. */
static void sort_assoc(ObjAssocArray* assoc, SortContext* ctx) {
    int n = assoc->count;
    for (int i = 1; i < n; i++) {
        ObjString* sk = assoc->keys[i];
        Value      sv = assoc->values[i];
        int j = i - 1;
        while (j >= 0) {
            Value a, b;
            if (ctx->sortByKey) {
                a = OBJ_VAL(assoc->keys[j]);
                b = OBJ_VAL(sk);
            } else {
                a = assoc->values[j];
                b = sv;
            }
            int cmp = sort_compare_ctx(ctx, a, b,
                                       OBJ_VAL(assoc->keys[j]), OBJ_VAL(sk));
            if (ctx->aborted) return;   /* callback en échec : arrêt net du tri */
            if (cmp == 0) {
                /* secondary key : si on a trié par valeurs, secondary par clés ;
                 * inversement si on a trié par clés. Toujours croissant pour
                 * la stabilité, sauf si descending → secondary aussi descending. */
                if (ctx->sortByKey) {
                    cmp = compare_values_sort(assoc->values[j], sv, ctx->opts);
                } else {
                    cmp = compare_values_sort(OBJ_VAL(assoc->keys[j]),
                                              OBJ_VAL(sk), ctx->opts);
                }
                if (ctx->descending) cmp = -cmp;
            }
            if (cmp <= 0) break;
            assoc->keys[j + 1]   = assoc->keys[j];
            assoc->values[j + 1] = assoc->values[j];
            j--;
        }
        assoc->keys[j + 1]   = sk;
        assoc->values[j + 1] = sv;
    }
}

/* Trie renvoie le receveur (pattern famille chainable, ). */
static Value fn_trie(int argCount, Value* args) {
    SortContext ctx;
    bool isAssoc = IS_ASSOC_ARRAY(args[0]);
    if (!build_sort_context(argCount, args, isAssoc, &ctx)) return args[0];

    if (isAssoc) {
        sort_assoc(AS_ASSOC_ARRAY(args[0]), &ctx);
        return args[0];
    }
    if (!IS_ARRAY(args[0])) return args[0];
    sort_array_1d(AS_ARRAY(args[0]), &ctx);
    return args[0];
}

/* SupprimeTout(tab) / DeleteAll(tab) - clear array or collection.
 * Renvoie le receveur (pattern famille chainable, ). */
static Value fn_supprime_tout(int argCount, Value* args) {
    (void)argCount;
    if (IS_ARRAY(args[0])) {
        AS_ARRAY(args[0])->elements.count = 0;
    } else if (IS_COLLECTION(args[0])) {
        AS_COLLECTION(args[0])->elements.count = 0;
    }
    return args[0];
}

/* ================================================================
 * COLLECTION FUNCTIONS (File/Pile/Liste)
 * ================================================================ */

/* Enfile(file, val) / Enqueue(queue, val) - add to end of queue.
 * Renvoie le receveur (pattern famille chainable, ). */
static Value fn_enfile(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return args[0];
    ObjCollection* coll = AS_COLLECTION(args[0]);
    value_array_write(&coll->elements, args[1]);
    return args[0];
}

/* Defile(file) / Dequeue(queue) - remove and return from front of queue */
static Value fn_defile(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return NIL_VAL;
    ObjCollection* coll = AS_COLLECTION(args[0]);
    if (coll->elements.count == 0) return NIL_VAL;

    Value front = coll->elements.values[0];
    /* Shift all elements left */
    for (int i = 0; i < coll->elements.count - 1; i++) {
        coll->elements.values[i] = coll->elements.values[i + 1];
    }
    coll->elements.count--;
    return front;
}

/* Empile(pile, val) / Push(stack, val) - push to top of stack.
 * Renvoie le receveur (pattern famille chainable, ). */
static Value fn_empile(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return args[0];
    ObjCollection* coll = AS_COLLECTION(args[0]);
    value_array_write(&coll->elements, args[1]);
    return args[0];
}

/* Depile(pile) / Pop(stack) - pop and return from top of stack */
static Value fn_depile(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return NIL_VAL;
    ObjCollection* coll = AS_COLLECTION(args[0]);
    if (coll->elements.count == 0) return NIL_VAL;

    Value top = coll->elements.values[coll->elements.count - 1];
    coll->elements.count--;
    return top;
}

/* ListeAjoute(liste, val) / ListAdd(list, val) - append to list.
 * Renvoie le receveur (pattern famille chainable, ).
 * NB : exposee uniquement en methode prefix (le nom global ListeAjoute est
 * reserve aux champs UI Liste/Combo, cf. note L1503). */
static Value fn_liste_ajoute(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return args[0];
    ObjCollection* coll = AS_COLLECTION(args[0]);
    value_array_write(&coll->elements, args[1]);
    return args[0];
}

/* ListeInsere(liste, pos, val) / ListInsert(list, pos, val) - insert at
 * position (base 1). Renvoie le receveur (pattern famille chainable). */
static Value fn_liste_insere(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return args[0];
    ObjCollection* coll = AS_COLLECTION(args[0]);

    int64_t pos = 1;
    if (IS_INTEGER(args[1])) pos = AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) pos = (int64_t)AS_REAL(args[1]);

    int idx = (int)(pos - 1);
    if (idx < 0) idx = 0;
    if (idx > coll->elements.count) idx = coll->elements.count;

    value_array_write(&coll->elements, NIL_VAL); /* grow */
    for (int i = coll->elements.count - 1; i > idx; i--) {
        coll->elements.values[i] = coll->elements.values[i - 1];
    }
    coll->elements.values[idx] = args[2];
    return args[0];
}

/* ListeSupprime(liste, pos) / ListDelete(list, pos) - remove at position
 * (base 1). Renvoie le receveur (pattern famille chainable). */
static Value fn_liste_supprime(int argCount, Value* args) {
    (void)argCount;
    if (!IS_COLLECTION(args[0])) return args[0];
    ObjCollection* coll = AS_COLLECTION(args[0]);

    int64_t pos = 1;
    if (IS_INTEGER(args[1])) pos = AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) pos = (int64_t)AS_REAL(args[1]);

    int idx = (int)(pos - 1);
    if (idx < 0 || idx >= coll->elements.count) return args[0];

    for (int i = idx; i < coll->elements.count - 1; i++) {
        coll->elements.values[i] = coll->elements.values[i + 1];
    }
    coll->elements.count--;
    return args[0];
}

/* fn_supprime_chain supprime : fn_supprime renvoie deja le receveur depuis
 *  (, 2e cas B). Toute la famille chainable de manipulation
 * in-place est desormais homogene. */

/* ================================================================
 * LINQ-LIKE CHAIN OPERATIONS — Filtre / Transforme / Distinct / Union /
 * Intersection. Doc 1000024845 (Filtre), 1000024734 (Transforme),
 * 1000025406-25408 (Distinct/Intersection/Union).
 *
 * Tous les chain methods retournent un nouveau tableau (immutables vis-a-vis
 * de la source), permettant l'enchainement fluide :
 *   tab.Filtre(estPair).Transforme(double).Distinct()
 * ================================================================ */

extern InterpretResult vm_call_reentrant(Value callee, int argCount);
extern void vm_push(Value v);
extern Value vm_pop(void);

/* Detecte l'arite du callable :
 *  ObjFunction -> function->arity
 *  ObjNative   -> native->arity
 *  Autres / inconnu -> -1 (variadic) */
static int callable_arity(Value cb) {
    if (IS_FUNCTION(cb)) return AS_FUNCTION(cb)->arity;
    if (IS_NATIVE(cb))   return ((ObjNative*)AS_OBJ(cb))->arity;
    return -1;
}

/* Invoque cb(value [, key]). Si arity du cb >= 2 OU variadic (-1), on pousse
 * la cle aussi (canon 2024 : cb 1 arg ou 2 args optionnels).
 * Si arity == 1 : on pousse la valeur seule. */
static Value invoke_callback_kv(Value callable, Value val, Value key) {
    int ar = callable_arity(callable);
    int n = (ar == 1) ? 1 : 2;
    vm_push(callable);
    vm_push(val);
    if (n == 2) vm_push(key);
    InterpretResult r = vm_call_reentrant(callable, n);
    if (r != INTERPRET_OK) return NIL_VAL;
    return vm_pop();
}

/* Test "truthy" pour le retour d'une condition Filtre.
 * canon : Vrai = booleen Vrai, ou entier != 0. */
static bool is_truthy_filter(Value v) {
    if (IS_BOOL(v))    return AS_BOOL(v);
    if (IS_INTEGER(v)) return AS_INTEGER(v) != 0;
    return false;
}

/* Filtre(tab, condition) — nouveau tableau (ou tableau associatif) avec les
 * elements pour lesquels cb renvoie Vrai. Doc 1000024845 + nouveaute 2024
 * support assoc. cb recoit (valeur [, cle]) ; cle = indice base 1 pour 1D ou
 * cle d'indexation pour assoc. */
static Value fn_filtre(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    Value cb = args[1];

    /* Cas tableau associatif (nouveaute 2024) */
    if (IS_ASSOC_ARRAY(args[0])) {
        ObjAssocArray* src = AS_ASSOC_ARRAY(args[0]);
        ObjAssocArray* out = obj_new_assoc(src->keyType, src->options, src->valueKind);
        vm_push(OBJ_VAL(out));
        for (int i = 0; i < src->count; i++) {
            ObjString* k = src->keys[i];
            Value v = src->values[i];
            Value res = invoke_callback_kv(cb, v, OBJ_VAL(k));
            if (is_truthy_filter(res)) {
                obj_assoc_set(out, k, v);
            }
        }
        vm_pop();
        return OBJ_VAL(out);
    }

    /* Cas tableau 1D simple */
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* src = AS_ARRAY(args[0]);
    ObjArray* out = obj_new_array();
    vm_push(OBJ_VAL(out));
    for (int i = 0; i < src->elements.count; i++) {
        Value elem = src->elements.values[i];
        Value res = invoke_callback_kv(cb, elem, INTEGER_VAL(i + 1)); /* base 1 */
        if (is_truthy_filter(res)) {
            value_array_write(&out->elements, elem);
        }
    }
    vm_pop();
    return OBJ_VAL(out);
}

/* Transforme(tab, op) — nouveau tableau (ou assoc) avec les retours de cb.
 * Doc 1000024734 + nouveaute 2024 support assoc. */
static Value fn_transforme(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    Value cb = args[1];

    /* Cas tableau associatif */
    if (IS_ASSOC_ARRAY(args[0])) {
        ObjAssocArray* src = AS_ASSOC_ARRAY(args[0]);
        ObjAssocArray* out = obj_new_assoc(src->keyType, src->options, src->valueKind);
        vm_push(OBJ_VAL(out));
        for (int i = 0; i < src->count; i++) {
            ObjString* k = src->keys[i];
            Value v = src->values[i];
            Value res = invoke_callback_kv(cb, v, OBJ_VAL(k));
            obj_assoc_set(out, k, res);
        }
        vm_pop();
        return OBJ_VAL(out);
    }

    /* Cas tableau 1D simple */
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* src = AS_ARRAY(args[0]);
    ObjArray* out = obj_new_array();
    vm_push(OBJ_VAL(out));
    for (int i = 0; i < src->elements.count; i++) {
        Value elem = src->elements.values[i];
        Value res = invoke_callback_kv(cb, elem, INTEGER_VAL(i + 1));
        /*  : obj_array_push push res si Obj (canon ferme le cas
         * Value-in-variable contournant le lint Pattern 2). */
        obj_array_push(out, res);
    }
    vm_pop();
    return OBJ_VAL(out);
}

/* Test d'egalite "shallow" pour Distinct/Union/Intersection.
 * Reuse values_equal pour types primitifs et strings interned. */
extern bool values_equal(Value a, Value b);

static bool array_contains(ObjArray* arr, Value needle) {
    for (int i = 0; i < arr->elements.count; i++) {
        if (values_equal(arr->elements.values[i], needle)) return true;
    }
    return false;
}

/* Distinct(tab) / tab.Distinct() — nouveau tableau sans doublons.
 * Premier occurrence preservee (ordre stable). */
static Value fn_distinct(int argCount, Value* args) {
    (void)argCount;
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* src = AS_ARRAY(args[0]);

    ObjArray* out = obj_new_array();
    vm_push(OBJ_VAL(out));

    for (int i = 0; i < src->elements.count; i++) {
        Value elem = src->elements.values[i];
        if (!array_contains(out, elem)) {
            value_array_write(&out->elements, elem);
        }
    }
    vm_pop();
    return OBJ_VAL(out);
}

/* Union(tab1, tab2) — nouveau tableau union sans doublons (= concat + distinct). */
static Value fn_union(int argCount, Value* args) {
    if (argCount < 2 || !IS_ARRAY(args[0]) || !IS_ARRAY(args[1])) return NIL_VAL;
    ObjArray* a = AS_ARRAY(args[0]);
    ObjArray* b = AS_ARRAY(args[1]);

    ObjArray* out = obj_new_array();
    vm_push(OBJ_VAL(out));

    for (int i = 0; i < a->elements.count; i++) {
        Value elem = a->elements.values[i];
        if (!array_contains(out, elem)) {
            value_array_write(&out->elements, elem);
        }
    }
    for (int i = 0; i < b->elements.count; i++) {
        Value elem = b->elements.values[i];
        if (!array_contains(out, elem)) {
            value_array_write(&out->elements, elem);
        }
    }
    vm_pop();
    return OBJ_VAL(out);
}

/* Intersection(tab1, tab2) — nouveau tableau des elements presents dans les
 * deux. Sans doublons. Ordre = celui de tab1. */
static Value fn_intersection(int argCount, Value* args) {
    if (argCount < 2 || !IS_ARRAY(args[0]) || !IS_ARRAY(args[1])) return NIL_VAL;
    ObjArray* a = AS_ARRAY(args[0]);
    ObjArray* b = AS_ARRAY(args[1]);

    ObjArray* out = obj_new_array();
    vm_push(OBJ_VAL(out));

    for (int i = 0; i < a->elements.count; i++) {
        Value elem = a->elements.values[i];
        if (array_contains(b, elem) && !array_contains(out, elem)) {
            value_array_write(&out->elements, elem);
        }
    }
    vm_pop();
    return OBJ_VAL(out);
}

/* ================================================================
 * TERMINAUX (Phase 3) : Applique / Agrège / Somme / Moyenne /
 * VérifieTous{Vrai,Faux} / VérifieUn{Vrai,Faux}
 * Doc 1000024856 / 1000024744 / 1000019892 / 1000019891 /
 *        1000024857-60. Tous : 1D + assoc (sauf Somme/Moyenne et
 *        Vérifie* qui sont 1D seulement dans la doc).
 * ================================================================ */

/* Helper : invoque cb(val [, cle], acc) -> nouvel acc. Pour Agrège. */
static Value invoke_callback_acc(Value callable, Value val, Value key, Value acc) {
    int ar = callable_arity(callable);
    int n = (ar == 3) ? 3 : 2;
    vm_push(callable);
    vm_push(val);
    if (n == 3) vm_push(key);
    vm_push(acc);
    InterpretResult r = vm_call_reentrant(callable, n);
    if (r != INTERPRET_OK) return NIL_VAL;
    return vm_pop();
}

/* Applique(tab, op) / tab.Applique(op) — execute op sur chaque element (forEach).
 * Semantique : effets de bord du callback, source NON mutee.
 * Retour : le tableau source lui-meme (this/self), pour chainage fluent
 *   avec Transforme/Filtre/Distinct/Union/Intersection.
 *   Divergence canon assumee, . v2024 : 1D + assoc.
 * Callback : (valeur [, cle]). */
static Value fn_applique(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    Value cb = args[1];
    if (IS_ASSOC_ARRAY(args[0])) {
        ObjAssocArray* src = AS_ASSOC_ARRAY(args[0]);
        for (int i = 0; i < src->count; i++) {
            invoke_callback_kv(cb, src->values[i], OBJ_VAL(src->keys[i]));
        }
        return args[0];
    }
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* src = AS_ARRAY(args[0]);
    for (int i = 0; i < src->elements.count; i++) {
        invoke_callback_kv(cb, src->elements.values[i], INTEGER_VAL(i + 1));
    }
    return args[0];
}

/* Agrège(tab, op [, init]) / tab.Agrège(op [, init]) — reduce.
 * Callback arity 2 : (valeur, accumulateur) -> nouvel acc
 * Callback arity 3 : (valeur, cle, accumulateur) -> nouvel acc
 * Premier appel : acc = init (ou Null si init absent).
 * Retour : accumulateur final. v2024 : 1D + assoc. */
static Value fn_agrege(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    Value cb = args[1];
    Value acc = (argCount >= 3) ? args[2] : NIL_VAL;
    if (IS_ASSOC_ARRAY(args[0])) {
        ObjAssocArray* src = AS_ASSOC_ARRAY(args[0]);
        for (int i = 0; i < src->count; i++) {
            acc = invoke_callback_acc(cb, src->values[i], OBJ_VAL(src->keys[i]), acc);
        }
        return acc;
    }
    if (!IS_ARRAY(args[0])) return NIL_VAL;
    ObjArray* src = AS_ARRAY(args[0]);
    for (int i = 0; i < src->elements.count; i++) {
        acc = invoke_callback_acc(cb, src->elements.values[i], INTEGER_VAL(i + 1), acc);
    }
    return acc;
}

/* Local helper : convertit Value en double pour Somme/Moyenne. */
static double arr_to_double(Value v) {
    if (IS_INTEGER(v))   return (double)AS_INTEGER(v);
    if (IS_REAL(v))      return AS_REAL(v);
    if (IS_BOOL(v))      return AS_BOOL(v) ? 1.0 : 0.0;
    return 0.0;
}

/* Somme(tab) ou Somme(v1, v2, ...) ou tab.Somme().
 * Retourne entier si tous integers, reel sinon. */
static Value fn_somme(int argCount, Value* args) {
    if (argCount == 1 && IS_ARRAY(args[0])) {
        ObjArray* arr = AS_ARRAY(args[0]);
        bool allInt = true;
        int64_t intSum = 0;
        double realSum = 0.0;
        for (int i = 0; i < arr->elements.count; i++) {
            Value v = arr->elements.values[i];
            if (IS_INTEGER(v)) {
                intSum += AS_INTEGER(v);
                realSum += (double)AS_INTEGER(v);
            } else {
                allInt = false;
                realSum += arr_to_double(v);
            }
        }
        return allInt ? INTEGER_VAL(intSum) : REAL_VAL(realSum);
    }
    bool allInt = true;
    int64_t intSum = 0;
    double realSum = 0.0;
    for (int i = 0; i < argCount; i++) {
        if (IS_INTEGER(args[i])) {
            intSum += AS_INTEGER(args[i]);
            realSum += (double)AS_INTEGER(args[i]);
        } else {
            allInt = false;
            realSum += arr_to_double(args[i]);
        }
    }
    return allInt ? INTEGER_VAL(intSum) : REAL_VAL(realSum);
}

/* Moyenne(tab) ou Moyenne(v1, v2, ...) ou tab.Moyenne(). Vide -> 0. */
static Value fn_moyenne(int argCount, Value* args) {
    if (argCount == 1 && IS_ARRAY(args[0])) {
        ObjArray* arr = AS_ARRAY(args[0]);
        int n = arr->elements.count;
        if (n == 0) return REAL_VAL(0.0);
        double sum = 0.0;
        for (int i = 0; i < n; i++) sum += arr_to_double(arr->elements.values[i]);
        return REAL_VAL(sum / (double)n);
    }
    if (argCount == 0) return REAL_VAL(0.0);
    double sum = 0.0;
    for (int i = 0; i < argCount; i++) sum += arr_to_double(args[i]);
    return REAL_VAL(sum / (double)argCount);
}

/* Truthy test : Bool true OU integer != 0. */
static bool value_is_truthy(Value v) {
    if (IS_BOOL(v))    return AS_BOOL(v);
    if (IS_INTEGER(v)) return AS_INTEGER(v) != 0;
    return false;
}

/* VerifieTousVrai(tab [, op]) -- vacuous truth sur tableau vide.
 * Sans op : verifie directement les valeurs (doivent etre booleens/entiers). */
static Value fn_verifie_tous_vrai(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return BOOL_VAL(false);
    ObjArray* arr = AS_ARRAY(args[0]);
    bool hasCb = (argCount >= 2);
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (hasCb) v = invoke_callback_kv(args[1], v, INTEGER_VAL(i + 1));
        if (!value_is_truthy(v)) return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value fn_verifie_tous_faux(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return BOOL_VAL(false);
    ObjArray* arr = AS_ARRAY(args[0]);
    bool hasCb = (argCount >= 2);
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (hasCb) v = invoke_callback_kv(args[1], v, INTEGER_VAL(i + 1));
        if (value_is_truthy(v)) return BOOL_VAL(false);
    }
    return BOOL_VAL(true);
}

static Value fn_verifie_un_vrai(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return BOOL_VAL(false);
    ObjArray* arr = AS_ARRAY(args[0]);
    bool hasCb = (argCount >= 2);
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (hasCb) v = invoke_callback_kv(args[1], v, INTEGER_VAL(i + 1));
        if (value_is_truthy(v)) return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
}

static Value fn_verifie_un_faux(int argCount, Value* args) {
    if (!IS_ARRAY(args[0])) return BOOL_VAL(false);
    ObjArray* arr = AS_ARRAY(args[0]);
    bool hasCb = (argCount >= 2);
    for (int i = 0; i < arr->elements.count; i++) {
        Value v = arr->elements.values[i];
        if (hasCb) v = invoke_callback_kv(args[1], v, INTEGER_VAL(i + 1));
        if (!value_is_truthy(v)) return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
}

/* ================================================================
 * ASSOCIATIVE ARRAYS (tableaux associatifs)
 * ================================================================ */

static int arg_to_int(Value v) {
    if (IS_INTEGER(v)) return (int)AS_INTEGER(v);
    if (IS_REAL(v))    return (int)AS_REAL(v);
    return 0;
}

/* __AssocNew(keyType, options, valueKind [, valueTypeName]) — invoked by
 * compiler on declaration "tab est un tableau associatif de <type>".
 *  : le 4e argument (optionnel) porte le nom d'un type de valeur NON
 * primitif ; stocke sur l'assoc, il pilote l'auto-vivification objet a la
 * lecture/ecriture d'une cle absente. Absent ou vide -> valueTypeName NULL
 * (variant/primitif : pas de vivification en lecture). */
static Value fn_assoc_new(int argCount, Value* args) {
    int keyType    = argCount >= 1 ? arg_to_int(args[0]) : ASSOC_KEY_STRING;
    int options    = argCount >= 2 ? arg_to_int(args[1]) : 0;
    int valueKind  = argCount >= 3 ? arg_to_int(args[2]) : ASSOC_VALUE_VARIANT;
    ObjAssocArray* a = obj_new_assoc((AssocKeyType)keyType, options, valueKind);
    if (argCount >= 4 && IS_STRING(args[3]) && AS_STRING(args[3])->length > 0) {
        a->valueTypeName = AS_STRING(args[3]);
    }
    return OBJ_VAL(a);
}

/* TableauAjoute(tab, cle, val) — insert or update */
static Value fn_tableau_ajoute(int argCount, Value* args) {
    (void)argCount;
    if (!IS_ASSOC_ARRAY(args[0])) {
        /* Fallback to regular array behavior: Ajoute(tab, val) */
        return fn_ajoute(argCount, args);
    }
    if (!IS_STRING(args[1])) return BOOL_VAL(false);
    obj_assoc_set(AS_ASSOC_ARRAY(args[0]), AS_STRING(args[1]), args[2]);
    return BOOL_VAL(true);
}

/* TableauSupprime(tab, cle) — remove by key. Returns Vrai if removed. */
static Value fn_tableau_supprime(int argCount, Value* args) {
    /* Alias canonique de Supprime (doc 3075008). Délégation pure pour
     * que le traitement des sentinels (ÉlémentCourant, PremierÉlément,
     * DernierÉlément) et des assoc soit identique entre les deux noms. */
    return fn_supprime(argCount, args);
}

/* TableauSupprimeTout(tab) — clear */
static Value fn_tableau_supprime_tout(int argCount, Value* args) {
    (void)argCount;
    if (IS_ASSOC_ARRAY(args[0])) {
        obj_assoc_clear(AS_ASSOC_ARRAY(args[0]));
        return BOOL_VAL(true);
    }
    return fn_supprime_tout(argCount, args);
}

/* TableauCherche(tab, mode, val [, start]) — canon-conformant search with tc* constants.
 * On associative array: falls back to key lookup (2-arg form). */
static Value fn_tableau_cherche(int argCount, Value* args) {
    if (argCount < 2) return INTEGER_VAL(-1); /* défense : lit args[1] */
    /* Associative array: TableauCherche(assoc, key) → bool */
    if (IS_ASSOC_ARRAY(args[0])) {
        if (!IS_STRING(args[1])) return BOOL_VAL(false);
        int exists = 0;
        obj_assoc_get(AS_ASSOC_ARRAY(args[0]), AS_STRING(args[1]), &exists);
        return BOOL_VAL(exists);
    }

    if (!IS_ARRAY(args[0])) return INTEGER_VAL(-1);
    ObjArray* array = AS_ARRAY(args[0]);
    int n = array->elements.count;

    /* 2-arg form: TableauCherche(tab, val) → simple linear like Cherche */
    if (argCount < 3) {
        for (int i = 0; i < n; i++) {
            if (values_equal(array->elements.values[i], args[1]))
                return INTEGER_VAL(i + 1);
        }
        return INTEGER_VAL(-1);
    }

    int mode  = IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1]) : TC_LINEAIRE;
    Value val = args[2];
    int start = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) - 1 : 0;

    switch (mode) {
        case TC_LINEAIRE_PREMIER:
            g_cherche_pos = -1;
            /* fall through to forward scan from 0 */
            start = 0;
            /* FALL THROUGH */
        case TC_LINEAIRE:
        case TC_LINEAIRE_SUIVANT: {
            int from = (mode == TC_LINEAIRE_SUIVANT) ? g_cherche_pos + 1 : start;
            if (from < 0) from = 0;
            for (int i = from; i < n; i++) {
                if (values_equal(array->elements.values[i], val)) {
                    g_cherche_pos = i;
                    return INTEGER_VAL(i + 1);
                }
            }
            g_cherche_pos = n; /* exhausted */
            return INTEGER_VAL(-1);
        }
        case TC_LINEAIRE_DERNIER: {
            for (int i = n - 1; i >= 0; i--) {
                if (values_equal(array->elements.values[i], val)) {
                    g_cherche_pos = i;
                    return INTEGER_VAL(i + 1);
                }
            }
            return INTEGER_VAL(-1);
        }
        case TC_LINEAIRE_PRECEDENT: {
            int from = (g_cherche_pos > 0) ? g_cherche_pos - 1 : n - 1;
            for (int i = from; i >= 0; i--) {
                if (values_equal(array->elements.values[i], val)) {
                    g_cherche_pos = i;
                    return INTEGER_VAL(i + 1);
                }
            }
            return INTEGER_VAL(-1);
        }
        case TC_DICHOTOMIQUE: {
            int lo = 0, hi = n - 1;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                int cmp = compare_values_sort(array->elements.values[mid], val, 0);
                if (cmp == 0) return INTEGER_VAL(mid + 1);
                if (cmp < 0) lo = mid + 1; else hi = mid - 1;
            }
            return INTEGER_VAL(-1);
        }
        default:
            return INTEGER_VAL(-1);
    }
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* ================================================================
 * Registry declarations — Arrays / Collections family.
 * Canonical names (accents where official): Ajoute, Supprime, Insère,
 * Cherche, Inverse, Trie, SupprimeTout, Empile, Dépile, Enfile, Défile.
 * Aliases: TableauXxx / ArrayXxx, ListeXxx / ListXxx.
 * ================================================================ */

/* --- Enums (file scope : référencés par les OwlParam ci-dessous) ---
 *
 * Le mode de Trie/TableauTrie est bit-packed (cf. commentaire en tête de
 * fichier) : direction sur l'octet faux (mutex) OR-ée avec des bits hauts
 * combinables. On le modélise donc en deux enums distincts —
 *   - triType   : direction de base, exclusive (force_small_int car le
 *                 runtime switch sur rawMode & 0xFF, valeurs 1..5).
 *   - triOptions: modificateurs combinables (tcc* high-byte + ttClé bit 16),
 *                 combinable=true → rule 20 skip.
 * câblés via .allowedBase=&triType + .allowedOptions=&triOptions sur le
 * param mode. */
static const char* const E_triType_values[] = {
    "ttCroissant", "ttDecroissant", "ttColonne", "ttMembre", "ttFonction", NULL
};
static const OwlEnum E_triType = {
    .name="triType", .nameAscii="triType", .nameEN="sortType", .category="Tableaux",
    .descFR="Direction de tri (base, exclusive) pour Trie et TableauTrie.",
    .descEN="Sort direction (base, mutually exclusive) for Trie and TableauTrie.",
    .values=E_triType_values, .combinable=false, .force_small_int=true
};
static const char* const E_triOptions_values[] = {
    "tccSansCasse", "tccSansAccent", "tccSansEspace",
    "tccSansEspaceInt\xc3\xa9rieur",          /* tccSansEspaceIntérieur */
    "tccSansPonctuationNiEspace",
    "tccRespecteNum\xc3\xa9rique",            /* tccRespecteNumérique */
    "tccOrdreLexicographique",
    "ttCl\xc3\xa9",                           /* ttClé */
    NULL
};
static const OwlEnum E_triOptions = {
    .name="triOptions", .nameAscii="triOptions", .nameEN="sortOptions", .category="Tableaux",
    .descFR="Modificateurs combinables du tri (casse/accent/espace/numérique, ou tri par clés d'un associatif) OR-és par-dessus la direction.",
    .descEN="Combinable sort modifiers (case/accent/space/numeric, or sort by associative keys) OR-ed on top of the direction.",
    .values=E_triOptions_values, .combinable=true
};

/* Modes de recherche pour Cherche/TableauCherche — exclusifs, valeurs 1..6
 * comparées en switch côté runtime → force_small_int. */
static const char* const E_rechercheTableau_values[] = {
    "tcLineaire", "tcLineairePremier", "tcLineaireSuivant",
    "tcLineaireDernier", "tcLineairePrecedent", "tcDichotomique", NULL
};
static const OwlEnum E_rechercheTableau = {
    .name="rechercheTableau", .nameAscii="rechercheTableau", .nameEN="arraySeekMode", .category="Tableaux",
    .descFR="Mode de recherche (base, exclusif) pour Cherche et TableauCherche.",
    .descEN="Search mode (base, mutually exclusive) for Cherche and TableauCherche.",
    .values=E_rechercheTableau_values, .combinable=false, .force_small_int=true
};

/* Types d'information de TableauInfo — exclusifs. Les valeurs ti* sont déjà
 * en zone sentinel (OWL_TI_BASE = INT32_MIN+offset), donc pas de
 * force_small_int : rule 20 est satisfaite naturellement. */
static const char* const E_tableauInfo_values[] = {
    "tiNombreTotal", "tiNombreDimensions", "tiNombreLignes", "tiNombreColonnes",
    "tiDimension", "tiDynamique", "tiTailleElement", "tiTailleTotale",
    "tiAssociatifAvecDoublon", NULL
};
static const OwlEnum E_tableauInfo = {
    .name="tableauInfo", .nameAscii="tableauInfo", .nameEN="arrayInfo", .category="Tableaux",
    .descFR="Type d'information (base, exclusif) demandé à TableauInfo.",
    .descEN="Information type (base, mutually exclusive) requested from TableauInfo.",
    .values=E_tableauInfo_values, .combinable=false
};

static const OwlEnum* const ARRAY_ENUMS[] = {
    &E_triType, &E_triOptions, &E_rechercheTableau, &E_tableauInfo
};

static const OwlParam p_arr_v[]    = { {"tableau", "tableau",  "cible"},  {"valeur", "tout", "élément (type du tableau)"}, {0} };
static const OwlParam p_arr_i[]    = { {"tableau", "tableau",  "cible"},  {"indice", "entier", "base 1"}, {0} };
static const OwlParam p_arr_iv[]   = { {"tableau", "tableau",  "cible"},  {"indice", "entier", "base 1"}, {"valeur", "tout", "élément (type du tableau)"}, {0} };
static const OwlParam p_arr[]      = { {"tableau", "tableau",  "cible"}, {0} };
static const OwlParam p_queue_v[]  = { {"file",    "file",     "file FIFO"}, {"valeur", "tout", "élément (type de la file)"}, {0} };
static const OwlParam p_queue[]    = { {"file",    "file",     "file FIFO"}, {0} };
static const OwlParam p_stack_v[]  = { {"pile",    "pile",     "pile LIFO"}, {"valeur", "tout", "élément (type de la pile)"}, {0} };
static const OwlParam p_stack[]    = { {"pile",    "pile",     "pile LIFO"}, {0} };
static const OwlParam p_assoc_kv[] = { {"tableau", "tableau associatif", "cible"}, {"clé", "chaîne|entier", "clé"}, {"valeur", "tout", "valeur (type du tableau)"}, {0} };
static const OwlParam p_assoc_k[]  = { {"tableau", "tableau associatif", "cible"}, {"clé", "chaîne|entier", "clé"}, {0} };
/* Cherche — formes discrètes. Le mode (tcLineaire/tcDichotomique)
 * est accepté mais ignoré (recherche toujours linéaire en OWL). */
static const OwlParam p_arr_mv[]  = { {"tableau", "tableau", "cible"}, {"mode", "entier", "mode de recherche (constante tc*, sans effet)", &E_rechercheTableau}, {"valeur", "tout", "élément recherché"}, {0} };
static const OwlParam p_arr_mfv[] = { {"tableau", "tableau", "cible"}, {"mode", "entier", "mode de recherche (constante tc*, sans effet)", &E_rechercheTableau}, {"champ", "chaîne", "nom du membre de structure"}, {"valeur", "tout", "valeur recherchée sur ce membre"}, {0} };

static const OwlSignature sig_add[]   = { {2, p_arr_v,  "tableau", "le tableau modifié, pour permettre le chaînage (`.Ajoute(x).Ajoute(y)`)."}, {0} };
static const OwlSignature sig_del[]   = { {2, p_arr_i,  "tableau", "le tableau modifié, pour permettre le chaînage. Pour mesurer le nombre d'éléments supprimés : `..Occurrence` avant/après l'appel."}, {0} };
static const OwlSignature sig_ins[]   = { {3, p_arr_iv, "tableau", "le tableau modifié, pour permettre le chaînage."}, {0} };
/* TableauInfo — formes discrètes. 2 args (tab, type) ou 3 args
 * (tab, type, paramètre supplémentaire pour ttiDimension). L'impl ne lit
 * jamais au-delà de args[2]. */
static const OwlParam p_tinfo[]   = { {"tableau", "tableau", "cible"}, {"information", "entier", "type d'information (constante ti*)", &E_tableauInfo}, {0} };
static const OwlParam p_tinfo_p[] = { {"tableau", "tableau", "cible"}, {"information", "entier", "type d'information (constante ti*)", &E_tableauInfo}, {"paramètre", "entier", "dimension visée (base 1) pour tiDimension"}, {0} };
static const OwlSignature sig_find[]  = {
    {2, p_tinfo,   "entier", "information sur la structure du tableau"},
    {3, p_tinfo_p, "entier", "information sur une dimension précise (paramètre = numéro de dimension)"},
    {0}
};
/*  — Cherche : 3 syntaxes discrètes (2/3/4 args) au lieu d'un -1
 * fourre-tout. Permet la validation compile-time « Aucune syntaxe ne
 * correspond » sur les formes natives ET chaînées. */
static const OwlSignature sig_cherche[] = {
    {2, p_arr_v,   "entier", "position base 1, -1 si absent"},
    {3, p_arr_mv,  "entier", "position base 1, -1 si absent (mode ignoré)"},
    {4, p_arr_mfv, "entier", "position base 1, -1 si absent (recherche par membre de structure)"},
    {0}
};
static const OwlSignature sig_rev[]   = { {1, p_arr,    "tableau", "le tableau modifié (ordre inversé en place), pour permettre le chaînage."}, {0} };
/* Trie — formes discrètes. 1 arg (tab seul, ttCroissant par
 * défaut), 2 args (tab, mode), 3 args (tab, mode, colonne/spec/procédure).
 * Le multi-colonnes/multi-membres passe par une chaîne "col;-col2" en 3e
 * argument, pas par des args supplémentaires : l'impl ne lit jamais
 * au-delà de args[2]. */
static const OwlParam p_arr_mode[]    = { {"tableau", "tableau", "cible"}, {"mode", "entier", "type de tri (constante tt*, optionnel, défaut ttCroissant)", &E_triType, &E_triOptions}, {0} };
static const OwlParam p_arr_mode_col[] = { {"tableau", "tableau", "cible"}, {"mode", "entier", "type de tri (constante tt*)", &E_triType, &E_triOptions}, {"colonne", "entier|chaîne|procédure", "colonne(s)/membre(s)/procédure de tri selon le mode"}, {0} };
static const OwlSignature sig_sort[]  = {
    {1, p_arr,          "tableau", "le tableau trié en place (ttCroissant), pour permettre le chaînage."},
    {2, p_arr_mode,     "tableau", "le tableau trié selon le mode, pour permettre le chaînage."},
    {3, p_arr_mode_col, "tableau", "le tableau trié selon le mode et la colonne/membre/procédure, pour permettre le chaînage."},
    {0}
};
static const OwlSignature sig_clear[] = { {1, p_arr,    "tableau", "le tableau vidé, pour permettre le chaînage."}, {0} };

/* --- LINQ-like chain operations --- */
static const OwlParam p_filtre[]    = { {"tableau", "tableau", "tableau source (1D ou associatif)"}, {"condition", "procédure", "cb(valeur [, clé]) -> booléen"}, {0} };
static const OwlParam p_transforme[] = { {"tableau", "tableau", "tableau source (1D ou associatif)"}, {"opération", "procédure", "cb(valeur [, clé]) -> nouvelle valeur"}, {0} };
static const OwlParam p_set_op[]    = { {"tableau1", "tableau", "premier tableau 1D"}, {"tableau2", "tableau", "second tableau 1D"}, {0} };
static const OwlSignature sig_filtre[]    = { {2, p_filtre,    "tableau", "nouveau tableau filtré"}, {0} };
static const OwlSignature sig_transforme[] = { {2, p_transforme, "tableau", "nouveau tableau transformé"}, {0} };
static const OwlSignature sig_distinct[]  = { {1, p_arr,        "tableau", "nouveau tableau sans doublons"}, {0} };
static const OwlSignature sig_set_op[]    = { {2, p_set_op,    "tableau", "nouveau tableau (union ou intersection)"}, {0} };

/* --- Terminaux Phase 3 --- */
static const OwlParam p_applique[]  = { {"tableau", "tableau", "tableau (1D ou associatif)"}, {"opération", "procédure", "cb(valeur [, clé]) — effets de bord"}, {0} };
static const OwlParam p_agrege[]    = { {"tableau", "tableau", "tableau (1D ou associatif)"}, {"opération", "procédure", "cb(valeur, [clé,] acc) -> nouvel acc"}, {"valeur initiale", "tout", "valeur initiale (optionnel, défaut Null)"}, {0} };
static const OwlParam p_verifie[]   = { {"tableau", "tableau", "tableau 1D"}, {"opération", "procédure", "cb(valeur) -> booléen (optionnel)"}, {0} };
static const OwlSignature sig_applique[] = { {2, p_applique, "tableau", "exécute op sur chaque élément, retourne le tableau source pour chaînage."}, {0} };
static const OwlSignature sig_agrege[]   = {
    { 2, p_agrege, "tout", "réduction sans valeur initiale (type accumulateur)" },
    { 3, p_agrege, "tout", "réduction avec valeur initiale (type accumulateur)" },
    {0}
};
static const OwlSignature sig_somme[]    = { {-1, p_arr, "numérique", "somme du tableau ou des valeurs"}, {0} };
static const OwlSignature sig_moyenne[]  = { {-1, p_arr, "numérique", "moyenne du tableau ou des valeurs"}, {0} };
/* VérifieTousVrai/TousFaux/UnVrai/UnFaux — formes discrètes.
 * 1 arg (tableau, teste directement les valeurs) ou 2 args (tableau +
 * procédure de prédicat). L'impl ne lit jamais au-delà de args[1]. */
static const OwlSignature sig_verifie[]  = {
    {1, p_arr,     "booléen", "vérification logique directe sur les valeurs du tableau"},
    {2, p_verifie, "booléen", "vérification logique via une procédure appliquée à chaque élément"},
    {0}
};

static const OwlSignature sig_enq[]   = { {2, p_queue_v, "file", "la file modifiée, pour permettre le chaînage."}, {0} };
static const OwlSignature sig_deq[]   = { {1, p_queue,   "tout", "élément défilé selon type de la file (Null si vide)"}, {0} };
static const OwlSignature sig_push[]  = { {2, p_stack_v, "pile", "la pile modifiée, pour permettre le chaînage."}, {0} };
static const OwlSignature sig_pop[]   = { {1, p_stack,   "tout", "élément dépilé selon type de la pile (Null si vide)"}, {0} };

/* TableauAjoute accepte 2 OU 3 args :
 *  - 2 args : tableau 1D + valeur (alias canon d'Ajoute)
 *  - 3 args : tableau associatif + clé + valeur */
static const OwlSignature sig_tadd[]  = {
    {2, p_arr_v,   "rien", "élément ajouté en fin de tableau 1D"},
    {3, p_assoc_kv,"rien", "entrée clé/valeur ajoutée à un tableau associatif"},
    {0}
};
static const OwlSignature sig_tdel[]  = { {2, p_assoc_k,  "tableau associatif", "le tableau modifié, pour permettre le chaînage. Pour vérifier la présence d'une clé : `..Occurrence` ou indexation avant/après l'appel."}, {0} };
/* TableauCherche — formes discrètes. 2 args (tab, valeur OU
 * assoc, clé), 3 args (tab, mode, valeur), 4 args (tab, mode, valeur,
 * indice de départ). L'impl ne lit jamais au-delà de args[3]. Les formes
 * multi-valeurs/multi-membres de la spec canon ne sont pas implémentées
 * en OWL (recherche mono-valeur uniquement) ; bornes alignées sur le réel. */
static const OwlParam p_tseek_v[]   = { {"tableau", "tableau|tableau associatif", "cible"}, {"valeur", "tout", "élément recherché (ou clé pour un associatif)"}, {0} };
static const OwlParam p_tseek_mv[]  = { {"tableau", "tableau", "cible"}, {"mode", "entier", "type de recherche (constante tc*)", &E_rechercheTableau}, {"valeur", "tout", "élément recherché"}, {0} };
static const OwlParam p_tseek_mvi[] = { {"tableau", "tableau", "cible"}, {"mode", "entier", "type de recherche (constante tc*)", &E_rechercheTableau}, {"valeur", "tout", "élément recherché"}, {"indice", "entier", "indice de départ (base 1)"}, {0} };
static const OwlSignature sig_tseek[] = {
    {2, p_tseek_v,   "entier", "position base-1 ou -1 si absent (tableau) / Vrai si présente (associatif)"},
    {3, p_tseek_mv,  "entier", "position base-1 ou -1 si absent (recherche selon le mode)"},
    {4, p_tseek_mvi, "entier", "position base-1 ou -1 si absent (recherche selon le mode à partir de l'indice donné)"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    { .canonical = "Ajoute",       .nameEN = "Add",     .category = "Tableaux",
      .descFR = "Ajoute un élément à la fin d'un tableau.",
      .descEN = "Append an element to the end of an array.",
      .signatures = sig_add, .impl = fn_ajoute },
    { .canonical = "Supprime",     .nameEN = "Delete",  .category = "Tableaux",
      .descFR = "Supprime l'élément à l'indice donné (base 1).",
      .descEN = "Delete the element at the given index (base 1).",
      .signatures = sig_del, .impl = fn_supprime },
    { .canonical = "Ins\xc3\xa8" "re",          /* Insère */
      .canonicalAscii = "Insere",
      .nameEN = "Insert",  .category = "Tableaux",
      .descFR = "Insère un élément à l'indice donné.",
      .descEN = "Insert an element at the given index.",
      .signatures = sig_ins, .impl = fn_insere },
    { .canonical = "Cherche",      .nameEN = "Seek",    .category = "Tableaux",
      .descFR = "Cherche un élément et retourne sa position (base 1), -1 si absent.",
      .descEN = "Search for an element and return its position (base 1), -1 if absent.",
      .signatures = sig_cherche, .impl = fn_cherche },
    { .canonical = "TableauInfo",  .nameEN = "ArrayInfo", .category = "Tableaux",
      .descFR = "Infos sur la structure du tableau (nb dimensions, tailles, type…).",
      .descEN = "Info about array structure (nb dimensions, sizes, type…).",
      .signatures = sig_find, .impl = fn_tableau_info },
    { .canonical = "Inverse",      .nameEN = "Reverse", .category = "Tableaux",
      .descFR = "Inverse l'ordre des éléments.",
      .descEN = "Reverse the order of elements.",
      .signatures = sig_rev, .impl = fn_inverse },
    { .canonical = "Trie",         .nameEN = "Sort",    .category = "Tableaux",
      .descFR = "Trie le tableau ou l'associatif : modes ttCroissant/ttDecroissant, ttColonne (2D \"col;-col\"), ttMembre (\"Champ;-Champ\"), ttFonction (procédure custom), ttClé (assoc), combinables avec tcc*.",
      .descEN = "Sort an array or associative array: modes ttCroissant/ttDecroissant, ttColonne (2D \"col;-col\"), ttMembre (\"Field;-Field\"), ttFonction (custom procedure), ttCle (assoc), combinable with tcc*.",
      .signatures = sig_sort, .impl = fn_trie },
    { .canonical = "SupprimeTout", .nameEN = "DeleteAll", .category = "Tableaux",
      .descFR = "Vide complètement un tableau/collection.",
      .descEN = "Clear the array/collection completely.",
      .signatures = sig_clear, .impl = fn_supprime_tout },

    /* --- File (queue FIFO) --- */
    { .canonical = "Enfile",  .nameEN = "Enqueue", .category = "File",
      .descFR = "Ajoute un élément à la fin d'une file (FIFO).",
      .descEN = "Add an element to the end of a queue (FIFO).",
      .signatures = sig_enq, .impl = fn_enfile },
    { .canonical = "D\xc3\xa9" "file",          /* Défile */
      .canonicalAscii = "Defile",
      .nameEN = "Dequeue", .category = "File",
      .descFR = "Retire et retourne le premier élément d'une file.",
      .descEN = "Remove and return the first element of a queue.",
      .signatures = sig_deq, .impl = fn_defile },

    /* --- Pile (stack LIFO) --- */
    { .canonical = "Empile",  .nameEN = "Push", .category = "Pile",
      .descFR = "Empile un élément au sommet (LIFO).",
      .descEN = "Push an element onto the top of a stack (LIFO).",
      .signatures = sig_push, .impl = fn_empile },
    { .canonical = "D\xc3\xa9" "pile",          /* Dépile */
      .canonicalAscii = "Depile",
      .nameEN = "Pop", .category = "Pile",
      .descFR = "Dépile et retourne l'élément du sommet.",
      .descEN = "Pop and return the top element.",
      .signatures = sig_pop, .impl = fn_depile },

    /* ListeAjoute / ListeInsère / ListeSupprime : retirées du registry
     * global. La doc réserve ces noms aux champs UI (Liste, Combo,
     * Carrousel, Liste image) — pas portable tant qu'OWL n'a pas ces
     * champs. Les opérations sur le type collection `Liste de chaînes`
     * passent par les méthodes prefix .Ajoute() / .Insère() / .Supprime()
     * (cf. owl_register_method sur METHOD_TYPE_COLLECTION_LIST plus bas).
     * À réintroduire lorsque le backend GUI fournira les champs Liste. */

    /* --- Tableaux associatifs --- */
    { .canonical = "TableauAjoute",       .nameEN = "ArrayAdd",       .category = "Tableaux Associatifs",
      .descFR = "Ajoute une entrée clé→valeur (ou met à jour).",
      .descEN = "Add a key→value entry (or update).",
      .signatures = sig_tadd, .impl = fn_tableau_ajoute },
    { .canonical = "TableauSupprime",     .nameEN = "ArrayDelete",    .category = "Tableaux Associatifs",
      .descFR = "Supprime l'entrée associée à une clé.",
      .descEN = "Delete the entry for a key.",
      .signatures = sig_tdel, .impl = fn_tableau_supprime },
    { .canonical = "TableauSupprimeTout", .nameEN = "ArrayDeleteAll", .category = "Tableaux Associatifs",
      .descFR = "Vide le tableau associatif.",
      .descEN = "Clear the associative array.",
      .signatures = sig_clear, .impl = fn_tableau_supprime_tout },
    { .canonical = "TableauCherche",      .nameEN = "ArraySeek",      .category = "Tableaux Associatifs",
      .descFR = "Vérifie la présence d'une clé.",
      .descEN = "Check for the presence of a key.",
      .signatures = sig_tseek, .impl = fn_tableau_cherche },

    /* --- Long names (aliases dispatchant sur tableau ou assoc) --- */
    { .canonical      = "TableauIns\xc3\xa8" "re",     /* TableauInsère */
      .canonicalAscii = "TableauInsere",
      .nameEN = "ArrayInsert",  .category = "Tableaux",
      .descFR = "Insère une valeur à une position dans un tableau.",
      .descEN = "Insert a value at a position in an array.",
      .signatures = sig_ins,  .impl = fn_insere },
    { .canonical = "TableauInverse", .nameEN = "ArrayReverse", .category = "Tableaux",
      .descFR = "Inverse l'ordre des éléments d'un tableau.",
      .descEN = "Reverse the order of elements in an array.",
      .signatures = sig_rev,  .impl = fn_inverse },
    { .canonical = "TableauTrie",    .nameEN = "ArraySort",    .category = "Tableaux",
      .descFR = "Trie un tableau en place.",
      .descEN = "Sort an array in place.",
      .signatures = sig_sort, .impl = fn_trie },

    /* --- LINQ-like chain operations (doc 1000024845 et co.) --- */
    { .canonical = "Filtre",       .nameEN = "Filter",  .category = "Tableaux",
      .descFR = "Renvoie un tableau (ou tableau associatif) avec les éléments pour lesquels la condition renvoie Vrai. Callback : (valeur [, clé]).",
      .descEN = "Returns an array (or associative array) with elements for which the condition returns True. Callback: (value [, key]).",
      .signatures = sig_filtre, .impl = fn_filtre },
    { .canonical = "Transforme",   .nameEN = "Map",     .category = "Tableaux",
      .descFR = "Renvoie un tableau (ou tableau associatif) contenant les résultats de la procédure pour chaque élément. Callback : (valeur [, clé]).",
      .descEN = "Returns an array (or associative array) with the results of the procedure for each element. Callback: (value [, key]).",
      .signatures = sig_transforme, .impl = fn_transforme },
    { .canonical = "Distinct",     .nameEN = "Distinct", .category = "Tableaux",
      .descFR = "Renvoie un nouveau tableau sans doublons (premier de chaque préservé, ordre stable).",
      .descEN = "Returns a new array without duplicates (first occurrence kept, stable order).",
      .signatures = sig_distinct, .impl = fn_distinct },
    { .canonical = "Union",        .nameEN = "Union",    .category = "Tableaux",
      .descFR = "Renvoie l'union de deux tableaux 1D, sans doublons.",
      .descEN = "Returns the union of two 1D arrays, without duplicates.",
      .signatures = sig_set_op, .impl = fn_union },
    { .canonical = "Intersection", .nameEN = "Intersect", .category = "Tableaux",
      .descFR = "Renvoie l'intersection de deux tableaux 1D, sans doublons. Ordre = celui du premier tableau.",
      .descEN = "Returns the intersection of two 1D arrays, without duplicates. Order follows the first array.",
      .signatures = sig_set_op, .impl = fn_intersection },

    /* --- Terminaux LINQ-like (doc 1000024856 / 24744 / 19891-92 / 24857-60) --- */
    { .canonical = "Applique",   .nameEN = "Apply",  .category = "Tableaux",
      .descFR = "Exécute une procédure sur chaque élément du tableau (forEach, effets de bord). Retourne le tableau source pour permettre le chaînage avec Transforme, Filtre, Distinct, Union, Intersection. Callback : (valeur [, clé]).",
      .descEN = "Runs a procedure on each element (forEach, side effects). Returns the source array to allow chaining with Map, Filter, Distinct, Union, Intersect. Callback: (value [, key]).",
      .signatures = sig_applique, .impl = fn_applique },
    { .canonical      = "Agr\xc3\xa8" "ge",   /* Agrège */
      .canonicalAscii = "Agrege",
      .nameEN = "Aggregate", .category = "Tableaux",
      .descFR = "Réduit le tableau via une procédure. Callback : (valeur, [clé,] accumulateur) -> nouveau acc.",
      .descEN = "Reduce the array via a procedure. Callback: (value, [key,] accumulator) -> new acc.",
      .signatures = sig_agrege, .impl = fn_agrege },
    { .canonical = "Somme",      .nameEN = "Sum",     .category = "Tableaux",
      .descFR = "Somme d'un tableau 1D numérique ou de N valeurs.",
      .descEN = "Sum of a 1D numeric array or of N values.",
      .signatures = sig_somme, .impl = fn_somme },
    { .canonical = "Moyenne",    .nameEN = "Average", .category = "Tableaux",
      .descFR = "Moyenne d'un tableau 1D numérique ou de N valeurs (réel).",
      .descEN = "Average of a 1D numeric array or of N values (real).",
      .signatures = sig_moyenne, .impl = fn_moyenne },
    { .canonical      = "V\xc3\xa9" "rifieTousVrai", /* VérifieTousVrai */
      .canonicalAscii = "VerifieTousVrai",
      .nameEN = "CheckAllTrue", .category = "Tableaux",
      .descFR = "Vrai si tous les éléments (ou tous les retours du callback) valent Vrai.",
      .descEN = "True if all elements (or all callback returns) are True.",
      .signatures = sig_verifie, .impl = fn_verifie_tous_vrai },
    { .canonical      = "V\xc3\xa9" "rifieTousFaux",
      .canonicalAscii = "VerifieTousFaux",
      .nameEN = "CheckAllFalse", .category = "Tableaux",
      .descFR = "Vrai si tous les éléments valent Faux.",
      .descEN = "True if all elements are False.",
      .signatures = sig_verifie, .impl = fn_verifie_tous_faux },
    { .canonical      = "V\xc3\xa9" "rifieUnVrai",
      .canonicalAscii = "VerifieUnVrai",
      .nameEN = "CheckOneTrue", .category = "Tableaux",
      .descFR = "Vrai si au moins un élément vaut Vrai.",
      .descEN = "True if at least one element is True.",
      .signatures = sig_verifie, .impl = fn_verifie_un_vrai },
    { .canonical      = "V\xc3\xa9" "rifieUnFaux",
      .canonicalAscii = "VerifieUnFaux",
      .nameEN = "CheckOneFalse", .category = "Tableaux",
      .descFR = "Vrai si au moins un élément vaut Faux.",
      .descEN = "True if at least one element is False.",
      .signatures = sig_verifie, .impl = fn_verifie_un_faux },
};

void stdlib_array_init(void) {
    /* Internal builder for `tableau associatif de X` declarations */
    owl_define_native("__AssocNew", fn_assoc_new, -1);

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));

    /* ---- Itération sentinels (cf POUR TOUT) ---- */
    static const OwlConstDef ITER_CONSTS[] = {
        {"\xc3\x89l\xc3\xa9mentCourant", "Tableaux", INTEGER_VAL(OWL_ELEMENT_COURANT),
         "Élément en cours d'itération dans un POUR TOUT (utilisable avec Supprime).",
         "Current element in a FOR EACH iteration (use with Delete).", "CurrentElement"},
        {"Premier\xc3\x89l\xc3\xa9ment", "Tableaux", INTEGER_VAL(OWL_PREMIER_ELEMENT),
         "Premier élément du tableau/liste (utilisable avec Supprime).",
         "First element (use with Delete).", "FirstElement"},
        {"Dernier\xc3\x89l\xc3\xa9ment", "Tableaux", INTEGER_VAL(OWL_DERNIER_ELEMENT),
         "Dernier élément du tableau/liste (utilisable avec Supprime).",
         "Last element (use with Delete).", "LastElement"},
    };
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII +
     * alias nameEN (CurrentElement/FirstElement/LastElement). */
    owl_registry_register_consts(ITER_CONSTS,
        (int)(sizeof(ITER_CONSTS)/sizeof(ITER_CONSTS[0])));

    /* ---- Trie mode constants (tt*) ---- */
    static const OwlConstDef TT_CONSTS[] = {
        {"ttCroissant",              "Tableaux", INTEGER_VAL(TT_CROISSANT),           "Tri croissant (défaut).",                      "Ascending sort (default).", "asAscending"},
        {"ttDecroissant",            "Tableaux", INTEGER_VAL(TT_DECROISSANT),          "Tri décroissant.",                             "Descending sort.", "asDescending"},
        {"ttColonne",                "Tableaux", INTEGER_VAL(TT_COLONNE),              "Tri 2D selon plusieurs colonnes (3e arg = \"col;-col\").",  "2D sort over multiple columns (3rd arg = \"col;-col\" string).", "asColumn"},
        {"ttMembre",                 "Tableaux", INTEGER_VAL(TT_MEMBRE),               "Tri selon membres struct/classe (3e arg = \"Membre;-Membre\").", "Sort by struct/class members (3rd arg = \"Member;-Member\" string).", "asMember"},
        {"ttFonction",               "Tableaux", INTEGER_VAL(TT_FONCTION),             "Tri custom via procédure de comparaison.",     "Custom sort via comparison procedure.", "asFunction"},
        {"ttCl\xc3\xa9",             "Tableaux", INTEGER_VAL(TT_CLE),                  "Sur tableau associatif : tri selon les clés au lieu des valeurs (combinable).", "Associative array: sort by keys instead of values (combinable).", "asKey"},
        {"tccSansCasse",             "Tableaux", INTEGER_VAL(TCC_SANS_CASSE),          "Tri sans tenir compte de la casse.",           "Sort case-insensitive.", "tccIgnoreCase"},
        {"tccSansAccent",            "Tableaux", INTEGER_VAL(TCC_SANS_ACCENT),         "Tri sans tenir compte des accents.",           "Sort accent-insensitive.", "tccIgnoreAccent"},
        {"tccSansEspace",            "Tableaux", INTEGER_VAL(TCC_SANS_ESPACE),         "Tri sans tenir compte des espaces.",           "Sort ignoring leading/trailing spaces.", "tccIgnoreSpace"},
        {"tccSansEspaceInt\xc3\xa9rieur", "Tableaux", INTEGER_VAL(TCC_SANS_ESPACE_INT), "Tri sans tenir compte des espaces intérieurs.", "Sort ignoring inner spaces.", "tccIgnoreInsideSpace"},
        {"tccSansPonctuationNiEspace","Tableaux",INTEGER_VAL(TCC_SANS_PONCTUATION),    "Tri sans tenir compte des espaces et ponctuations.", "Sort ignoring spaces and punctuation.", "tccIgnorePunctuationAndSpace"},
        {"tccRespecteNum\xc3\xa9rique","Tableaux",INTEGER_VAL(TCC_RESPECTE_NUMERIQUE), "Tri respectant l'ordre numérique dans les chaînes.", "Sort respecting numeric order in strings.", "tccRespectNumeric"},
        {"tccOrdreLexicographique",  "Tableaux", INTEGER_VAL(TCC_ORDRE_LEXICOGRAPHIQUE), "Tri linguistique.",                          "Linguistic sort order.", "tccLexicographicOrder"},
    };
    owl_registry_register_consts(TT_CONSTS, (int)(sizeof(TT_CONSTS)/sizeof(TT_CONSTS[0])));

    /* ---- TableauCherche mode constants (tc*) ---- */
    static const OwlConstDef TC_CONSTS[] = {
        {"tcLineaire",         "Tableaux", INTEGER_VAL(TC_LINEAIRE),          "Recherche linéaire depuis le début ou l'indice de départ.", "Linear search from start or given index.", "asLinear"},
        {"tcLineairePremier",  "Tableaux", INTEGER_VAL(TC_LINEAIRE_PREMIER),  "Recherche linéaire depuis le premier élément.",             "Linear search from first element.", "asLinearFirst"},
        {"tcLineaireSuivant",  "Tableaux", INTEGER_VAL(TC_LINEAIRE_SUIVANT),  "Recherche linéaire de l'élément suivant.",                  "Linear search for the next occurrence.", "asLinearNext"},
        {"tcLineaireDernier",  "Tableaux", INTEGER_VAL(TC_LINEAIRE_DERNIER),  "Recherche linéaire depuis le dernier élément.",             "Linear search from last element.", "asLinearLast"},
        {"tcLineairePrecedent","Tableaux", INTEGER_VAL(TC_LINEAIRE_PRECEDENT),"Recherche linéaire de l'élément précédent.",                "Linear search for the previous occurrence.", "asLinearPrevious"},
        {"tcDichotomique",     "Tableaux", INTEGER_VAL(TC_DICHOTOMIQUE),      "Recherche dichotomique (tableau trié).",                    "Binary search (sorted array).", "asBinary"},
    };
    owl_registry_register_consts(TC_CONSTS, (int)(sizeof(TC_CONSTS)/sizeof(TC_CONSTS[0])));

    /* TableauInfo constants (doc 3075005) */
    static const OwlConstDef TI_CONSTS[] = {
        {"tiNombreTotal",        "Tableaux", INTEGER_VAL(TI_NOMBRE_TOTAL),
         "Nombre total d'éléments du tableau.",           "Total number of elements.", "tiTotalNumber"},
        {"tiNombreDimensions",   "Tableaux", INTEGER_VAL(TI_NOMBRE_DIMENSIONS),
         "Nombre de dimensions du tableau (1 à 10).",     "Number of dimensions (1 to 10).", "tiNumberDimensions"},
        {"tiNombreLignes",       "Tableaux", INTEGER_VAL(TI_NOMBRE_LIGNES),
         "Nombre de lignes d'un tableau 2D (0 sinon).",   "Row count for 2D array (0 otherwise).", "tiNumberRows"},
        {"tiNombreColonnes",     "Tableaux", INTEGER_VAL(TI_NOMBRE_COLONNES),
         "Nombre de colonnes d'un tableau 2D (0 sinon).", "Column count for 2D array (0 otherwise).", "tiNumberColumns"},
        {"tiDimension",          "Tableaux", INTEGER_VAL(TI_DIMENSION),
         "Nombre d'éléments dans la dimension <param> (défaut 1).",
         "Number of elements in dimension <param> (default 1).", "tiDimension"},
        {"tiDynamique",          "Tableaux", INTEGER_VAL(TI_DYNAMIQUE),
         "Vrai si le tableau est dynamique.", "True if array is dynamic.", "tiDynamic"},
        {"tiTailleElement",      "Tableaux", INTEGER_VAL(TI_TAILLE_ELEMENT),
         "Taille (octets) d'un élément.", "Size in bytes of one element.", "tiElementSize"},
        {"tiTailleTotale",       "Tableaux", INTEGER_VAL(TI_TAILLE_TOTALE),
         "Taille totale (octets) du tableau.", "Total size in bytes.", "tiTotalSize"},
        {"tiAssociatifAvecDoublon","Tableaux", INTEGER_VAL(TI_ASSOCIATIF_DOUBLON),
         "Vrai si tableau associatif gérant les doublons.", "True if assoc array with duplicates.", "tiAssociativeWithDuplicate"},
    };
    owl_registry_register_consts(TI_CONSTS, (int)(sizeof(TI_CONSTS)/sizeof(TI_CONSTS[0])));

    /* Enums modélisant les params mode/information (déclarés file scope). */
    owl_registry_register_enums(ARRAY_ENUMS,
        (int)(sizeof(ARRAY_ENUMS)/sizeof(ARRAY_ENUMS[0])));

    /* === Extension methods on arrays: tab.Ajoute(x).Trie(), etc. === */
    owl_register_method(METHOD_TYPE_ARRAY, "Ajoute",       fn_ajoute,              2);
    owl_register_method(METHOD_TYPE_ARRAY, "Add",          fn_ajoute,              2);
    owl_register_method(METHOD_TYPE_ARRAY, "Supprime",     fn_supprime,            2);
    owl_register_method(METHOD_TYPE_ARRAY, "Delete",       fn_supprime,            2);
    owl_register_method(METHOD_TYPE_ARRAY, "Insere",       fn_insere,              3);
    owl_register_method(METHOD_TYPE_ARRAY, "Insert",       fn_insere,              3);
    owl_register_method(METHOD_TYPE_ARRAY, "Cherche",      fn_cherche,             -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Seek",         fn_cherche,             -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Inverse",      fn_inverse,             1);
    owl_register_method(METHOD_TYPE_ARRAY, "Reverse",      fn_inverse,             1);
    owl_register_method(METHOD_TYPE_ARRAY, "Trie",         fn_trie,                -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Sort",         fn_trie,                -1);
    owl_register_method(METHOD_TYPE_ARRAY, "SupprimeTout", fn_supprime_tout,       1);
    owl_register_method(METHOD_TYPE_ARRAY, "DeleteAll",    fn_supprime_tout,       1);

    /* LINQ-like chain methods (immutable : retournent un nouveau tableau) */
    owl_register_method(METHOD_TYPE_ARRAY, "Filtre",       fn_filtre,       2);
    owl_register_method(METHOD_TYPE_ARRAY, "Filter",       fn_filtre,       2);
    owl_register_method(METHOD_TYPE_ARRAY, "Transforme",   fn_transforme,   2);
    owl_register_method(METHOD_TYPE_ARRAY, "Map",          fn_transforme,   2);
    owl_register_method(METHOD_TYPE_ARRAY, "Distinct",     fn_distinct,     1);
    owl_register_method(METHOD_TYPE_ARRAY, "Union",        fn_union,        2);
    owl_register_method(METHOD_TYPE_ARRAY, "Intersection", fn_intersection, 2);
    owl_register_method(METHOD_TYPE_ARRAY, "Intersect",    fn_intersection, 2);

    /* Terminaux Phase 3 */
    owl_register_method(METHOD_TYPE_ARRAY, "Applique",          fn_applique,          2);
    owl_register_method(METHOD_TYPE_ARRAY, "Apply",             fn_applique,          2);
    owl_register_method(METHOD_TYPE_ARRAY, "Agr\xc3\xa8" "ge",  fn_agrege,           -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Agrege",            fn_agrege,           -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Aggregate",         fn_agrege,           -1);
    owl_register_method(METHOD_TYPE_ARRAY, "Somme",             fn_somme,             1);
    owl_register_method(METHOD_TYPE_ARRAY, "Sum",               fn_somme,             1);
    owl_register_method(METHOD_TYPE_ARRAY, "Moyenne",           fn_moyenne,           1);
    owl_register_method(METHOD_TYPE_ARRAY, "Average",           fn_moyenne,           1);
    owl_register_method(METHOD_TYPE_ARRAY, "V\xc3\xa9" "rifieTousVrai", fn_verifie_tous_vrai, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "VerifieTousVrai",          fn_verifie_tous_vrai, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "CheckAllTrue",             fn_verifie_tous_vrai, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "V\xc3\xa9" "rifieTousFaux", fn_verifie_tous_faux, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "VerifieTousFaux",          fn_verifie_tous_faux, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "CheckAllFalse",            fn_verifie_tous_faux, -1);
    owl_register_method(METHOD_TYPE_ARRAY, "V\xc3\xa9" "rifieUnVrai",  fn_verifie_un_vrai,   -1);
    owl_register_method(METHOD_TYPE_ARRAY, "VerifieUnVrai",            fn_verifie_un_vrai,   -1);
    owl_register_method(METHOD_TYPE_ARRAY, "CheckOneTrue",             fn_verifie_un_vrai,   -1);
    owl_register_method(METHOD_TYPE_ARRAY, "V\xc3\xa9" "rifieUnFaux",  fn_verifie_un_faux,   -1);
    owl_register_method(METHOD_TYPE_ARRAY, "VerifieUnFaux",            fn_verifie_un_faux,   -1);
    owl_register_method(METHOD_TYPE_ARRAY, "CheckOneFalse",            fn_verifie_un_faux,   -1);

    /* === Extension methods on queues === */
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Enfile",  fn_enfile,  2);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Enqueue", fn_enfile,  2);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Defile",  fn_defile,  1);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "Dequeue", fn_defile,  1);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "SupprimeTout", fn_supprime_tout, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_QUEUE, "DeleteAll",    fn_supprime_tout, 1);

    /* === Extension methods on stacks === */
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Empile", fn_empile, 2);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Push",   fn_empile, 2);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Depile", fn_depile, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "Pop",    fn_depile, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "SupprimeTout", fn_supprime_tout, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_STACK, "DeleteAll",    fn_supprime_tout, 1);

    /* === Extension methods on lists === */
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Ajoute",   fn_liste_ajoute,   2);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Add",      fn_liste_ajoute,   2);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Insere",   fn_liste_insere,   3);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Insert",   fn_liste_insere,   3);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Supprime", fn_liste_supprime, 2);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "Delete",   fn_liste_supprime, 2);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "SupprimeTout", fn_supprime_tout, 1);
    owl_register_method(METHOD_TYPE_COLLECTION_LIST, "DeleteAll",    fn_supprime_tout, 1);
}
