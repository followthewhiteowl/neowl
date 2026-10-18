// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL registry — unified declaration store for natives, types, constants.
 * See owl_registry.h for intent.
 */

#include "owl_registry.h"
#include "owl_name_normalize.h"
#include "owl_methods.h"
#include "owl_stdlib.h"
#include "owl_errors.h"
#include "object.h"
#include "table.h"
#include "vm.h"
#include "scanner.h"
#include "scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================================================================
 * Storage — three flat growable arrays holding POINTERS to the
 * static const defs owned by each module.
 * ================================================================ */

typedef struct {
    const void** items;
    int count;
    int capacity;
} PtrArray;

static PtrArray g_natives = {NULL, 0, 0};
static PtrArray g_types   = {NULL, 0, 0};
static PtrArray g_consts  = {NULL, 0, 0};
static PtrArray g_enums   = {NULL, 0, 0};

/* Enregistre une chaine globale immuable dans vm.globals (ex: constantes
 * MIME de stdlib_http, codes BCP-47 de stdlib_i18n). Vit ici (owl_core)
 * et non dans stdlib_builtin_types.c qui est compile par target. */
void owl_register_global_string(const char* name, const char* val, int len) {
    /*  : delegue au canon obj_globals_set (object.c). On push v
     * AVANT obj_globals_set pour qu'elle survive a un GC declenche au
     * moment du copy de k dans obj_globals_set. */
    ObjString* v = obj_copy_string(val, len);
    vm_push(OBJ_VAL(v));
    obj_globals_set(name, OBJ_VAL(v));
    vm_pop();
}

static void ptr_array_push(PtrArray* a, const void* item) {
    if (a->count >= a->capacity) {
        int newCap = a->capacity < 32 ? 32 : a->capacity * 2;
        a->items = (const void**)realloc((void*)a->items, sizeof(void*) * (size_t)newCap);
        a->capacity = newCap;
    }
    a->items[a->count++] = item;
}

/* ================================================================
 * Name normalization for case+accent-insensitive lookup.
 * Operates on UTF-8 bytes; handles only the Latin-1 supplement range
 * (C0-FF) which covers every accented char in identifiers.
 * ================================================================ */

/* normalize_name / name_matches : extraits dans owl_name_normalize.{c,h},
 * partages avec le backend catalogue du formatter (owl-lsp) pour garantir
 * une normalisation byte-identique entre les deux backends. */

/* ================================================================
 * Native function registration.
 *
 * For each def we call the existing owl_define_native_wl for:
 *   - canonical (UTF-8 accented official form)
 *   - canonicalAscii (if distinct from canonical)
 *   - nameEN (if set)
 *   - every alias in aliases[]
 *
 * This keeps runtime behaviour byte-identical to the hand-written
 * owl_define_native_wl() calls the modules used to make.
 * ================================================================ */

static int def_arity(const OwlNativeDef* d) {
    /* Runtime arity for the native dispatch. If the def declares multiple
     * signatures (overloaded), accept any count (the C impl handles the
     * dispatch). A single signature with arity >= 0 constrains strictly. */
    if (d->signatures == NULL || d->signatures[0].returnType == NULL) return -1;
    /* Count valid signatures. */
    int n = 0;
    for (const OwlSignature* s = d->signatures; s->returnType != NULL; s++) n++;
    if (n > 1) return -1;
    return d->signatures[0].arity;
}

static void register_name_alias(const OwlNativeDef* d, const char* alias) {
    if (alias == NULL || *alias == '\0') return;
    owl_define_native_wl(alias, d->impl, def_arity(d));
}

/* Map a receiver type name ("chaine", "MockNative", "string", "diskfile"…)
 * vers son enum slot. Case-insensitive ASCII. Source unique de verite =
 * METHOD_RECEIVER_INFO[] cote owl_methods.c — pas de table parallele ici.
 * Renvoie -1 si le nom ne matche aucun receveur connu. */
static MethodReceiverType receiver_type_from_name(const char* name) {
    if (!name) return (MethodReceiverType)-1;
    /* Normalise (strip accents + minuscule) exactement comme le scanner le fait
     * pour les identifiants (invariant 6.2 accent-insensible). Sans le strip,
     * un type au nom accentué (MLArbreDécision) ne résoud pas vers sa forme
     * ASCII canon côté METHOD_RECEIVER_INFO ("MLArbreDecision") : ses méthodes
     * .Entraîne/.Prédit sont alors rejetées dès qu'on déclare des `fields`. */
    char buf[128];
    scanner_canonicalize_identifier(name, (int)strlen(name), buf, sizeof(buf));

    for (int t = 0; t < METHOD_TYPE_COUNT; t++) {
        const MethodReceiverInfo* info = method_receiver_info((MethodReceiverType)t);
        if (!info || !info->canonical) continue;
        char ref[128];
        scanner_canonicalize_identifier(info->canonical, (int)strlen(info->canonical),
                                        ref, sizeof(ref));
        if (strcmp(buf, ref) == 0) return (MethodReceiverType)t;
        /* Compare contre nameEN si distinct. */
        if (info->nameEN) {
            scanner_canonicalize_identifier(info->nameEN, (int)strlen(info->nameEN),
                                            ref, sizeof(ref));
            if (strcmp(buf, ref) == 0) return (MethodReceiverType)t;
        }
    }
    return (MethodReceiverType)-1;
}

/* Public wrapper pour le self-test regle 16 — expose la validation du nom
 * de receiverType sans exposer l'enum interne MethodReceiverType. */
bool owl_method_receiver_type_is_known(const char* name) {
    return (int)receiver_type_from_name(name) >= 0;
}

/* Lookup d'une methode par nom de type (chaîne) — utilise par le compiler
 * pour discriminer propriete vs methode au compile time. */
bool owl_method_exists_for_type_name(const char* typeName,
                                       const char* methodName, int methodNameLen) {
    MethodReceiverType t = receiver_type_from_name(typeName);
    if ((int)t < 0) return false;
    int arity = 0;
    return method_lookup(t, methodName, methodNameLen, &arity) != NULL;
}

/* Auto-register prefix methods declared via OwlNativeDef.methodBindings. */
static void register_method_bindings(const OwlNativeDef* d) {
    if (!d->methodBindings) return;
    for (const OwlMethodBinding* b = d->methodBindings;
         b->receiverType != NULL && b->methodName != NULL; b++) {
        MethodReceiverType type = receiver_type_from_name(b->receiverType);
        if ((int)type < 0) continue; /* unknown receiver — silently skip */
        owl_register_method(type, b->methodName, d->impl, b->arity);
    }
}

void owl_registry_register_module(const OwlNativeDef* defs, int count) {
    for (int i = 0; i < count; i++) {
        const OwlNativeDef* d = &defs[i];
        /* impl == NULL tolere : un module de donnees (ex. natives
         * Etats/PDF cote console) annonce ses signatures sans porter
         * l'implementation Qt. On enregistre quand meme nom + aliases + arite +
         * out-params + la def (le compilateur resout la native a l'identique) ;
         * seul register_method_bindings est saute plus bas (il relaie d->impl a
         * owl_register_method). Le dispatch runtime n'a jamais lieu dans ces
         * contextes compile-only (--check, portail de compile d'owl-build). */
        if (d->canonical == NULL) continue;

        /* Canonical name — the fmt-preferred form. */
        owl_define_native_wl(d->canonical, d->impl, def_arity(d));

        /* ASCII fallback :
         *   - si canonicalAscii fourni et different : utiliser tel quel
         *     (cas explicite, ex: 'ChaineCompare' alias propre)
         *   - sinon, si le canonical contient des accents : auto-strip
         *     pour generer l'alias ASCII (eviter l'oubli systematique
         *     d'un alias ASCII sur une native au nom accentue). */
        if (d->canonicalAscii && strcmp(d->canonicalAscii, d->canonical) != 0) {
            register_name_alias(d, d->canonicalAscii);
        } else if (!d->canonicalAscii) {
            char ascii[256];
            int alen = scanner_strip_accents(d->canonical, (int)strlen(d->canonical),
                                              ascii, (int)sizeof(ascii));
            if (alen > 0 && strcmp(ascii, d->canonical) != 0) {
                register_name_alias(d, ascii);
            }
        }

        /* English alias. */
        if (d->nameEN && strcmp(d->nameEN, d->canonical) != 0) {
            register_name_alias(d, d->nameEN);
        }

        /* Extra aliases (legacy spellings, variants). */
        if (d->aliases) {
            for (const char* const* a = d->aliases; *a; a++) {
                register_name_alias(d, *a);
            }
        }

        /* Auto-register prefix-method bindings if declared (impl requis :
         * owl_register_method stocke d->impl pour le dispatch de methode). */
        if (d->impl != NULL) register_method_bindings(d);

        /* Propage firstOutParamArg/lastOutParamArg si la native a des out-params. */
        if (d->flags & OWL_NAT_OUT_PARAMS) {
            owl_native_set_out_params(d->canonical, d->firstOutParamArg, d->lastOutParamArg);
            if (d->canonicalAscii && strcmp(d->canonicalAscii, d->canonical) != 0)
                owl_native_set_out_params(d->canonicalAscii, d->firstOutParamArg, d->lastOutParamArg);
            if (d->nameEN && strcmp(d->nameEN, d->canonical) != 0)
                owl_native_set_out_params(d->nameEN, d->firstOutParamArg, d->lastOutParamArg);
            if (d->aliases) {
                for (const char* const* a = d->aliases; *a; a++)
                    owl_native_set_out_params(*a, d->firstOutParamArg, d->lastOutParamArg);
            }
        }

        ptr_array_push(&g_natives, d);
    }
}

/* ================================================================
 * Type + constant registration.
 *
 * The actual creation of templates / globals stays in
 * stdlib_builtin_types.c (or wherever the module lives). The
 * registry just stores the pointers so --vscode-data / --doc can
 * enumerate them.
 * ================================================================ */

void owl_registry_register_types(const OwlTypeDef* types, int count) {
    for (int i = 0; i < count; i++) {
        if (types[i].canonical == NULL) continue;
        ptr_array_push(&g_types, &types[i]);
    }
}

/* Detecte si un nom contient au moins un caractere non-ASCII (accent).
 * Sert a decider si on doit auto-generer un alias ASCII. */
static bool _name_has_non_ascii(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p >= 0x80) return true;
    }
    return false;
}

void owl_registry_register_consts(const OwlConstDef* consts, int count) {
    for (int i = 0; i < count; i++) {
        if (consts[i].name == NULL) continue;
        ptr_array_push(&g_consts, &consts[i]);
        /* Auto-register dans vm.globals avec le nom canonique : sans cela
         * le code utilisateur 'Trace(maConst)' ne trouve pas la constante
         * (la VM lit vm.globals, pas le registre).
         *
         *  : delegue au canon obj_globals_set (discipline GC roots
         * centralisee dans object.c). */
        obj_globals_set(consts[i].name, consts[i].value);
        /* Si le canonical contient des accents, auto-generer l'alias ASCII
         * et l'enregistrer en runtime-only (pas dans le registre consts —
         * sinon doublon LSP cf CLAUDE.md). Resout l'invariant n°4 du
         * cahier des charges sans intervention manuelle du dev. */
        if (_name_has_non_ascii(consts[i].name)) {
            char ascii[256];
            int alen = scanner_strip_accents(consts[i].name,
                                              (int)strlen(consts[i].name),
                                              ascii, sizeof(ascii));
            if (alen > 0 && strcmp(ascii, consts[i].name) != 0) {
                obj_globals_set(ascii, consts[i].value);
            }
        }
        /* Invariant bilingue n°3 : si la const porte un nameEN, enregistrer
         * la forme EN comme alias global runtime-only pointant sur la meme
         * Value (mirror exact de l'alias ASCII ci-dessus). PAS de 2e entree
         * dans le registre consts → pas de doublon LSP/autocomplete. Le
         * runtime lit vm.globals, le LSP/formatter lit le registre. */
        if (consts[i].nameEN != NULL) {
            obj_globals_set(consts[i].nameEN, consts[i].value);
        }
    }
}

void owl_registry_register_ascii_alias(const char* canonical, const char* alias) {
    if (!canonical || !alias) return;
    const OwlConstDef* c = owl_registry_const_find(canonical);
    if (!c) {
        fprintf(stderr,
            "[registry] WARN: ascii_alias('%s', '%s') — canonical '%s' "
            "absent du registre. Alias non enregistre.\n",
            canonical, alias, canonical);
        return;
    }
    /*  : delegue au canon obj_globals_set. */
    obj_globals_set(alias, c->value);
}

void owl_registry_register_enums(const OwlEnum* const* enums, int count) {
    for (int i = 0; i < count; i++) {
        if (enums[i] == NULL || enums[i]->name == NULL) continue;
        ptr_array_push(&g_enums, enums[i]);
    }
}

/* ================================================================
 * Introspection
 * ================================================================ */

int owl_registry_count(void) { return g_natives.count; }

const OwlNativeDef* owl_registry_at(int index) {
    if (index < 0 || index >= g_natives.count) return NULL;
    return (const OwlNativeDef*)g_natives.items[index];
}

const OwlNativeDef* owl_registry_find_by_fn(NativeFn fn) {
    if (fn == NULL) return NULL;
    for (int i = 0; i < g_natives.count; i++) {
        const OwlNativeDef* d = (const OwlNativeDef*)g_natives.items[i];
        if (d->impl == fn) return d;
    }
    return NULL;
}

const OwlNativeDef* owl_registry_find(const char* name) {
    if (name == NULL) return NULL;
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; i < g_natives.count; i++) {
        const OwlNativeDef* d = (const OwlNativeDef*)g_natives.items[i];
        if (d->canonical      && owl_name_matches(d->canonical,      q)) return d;
        if (d->canonicalAscii && owl_name_matches(d->canonicalAscii, q)) return d;
        if (d->nameEN         && owl_name_matches(d->nameEN,         q)) return d;
        if (d->aliases) {
            for (const char* const* a = d->aliases; *a; a++) {
                if (owl_name_matches(*a, q)) return d;
            }
        }
    }
    return NULL;
}

const char* owl_registry_native_canonical_for(const char* input) {
    if (input == NULL) return NULL;
    char q[256];
    owl_normalize_name(input, q, sizeof q);
    for (int i = 0; i < g_natives.count; i++) {
        const OwlNativeDef* d = (const OwlNativeDef*)g_natives.items[i];
        /* Match cote FR (canon ou ASCII fallback) -> renvoie canon FR
         * (canonifie l'accent automatiquement). */
        if (d->canonical      && owl_name_matches(d->canonical,      q)) return d->canonical;
        if (d->canonicalAscii && owl_name_matches(d->canonicalAscii, q)) return d->canonical;
        /* Match cote EN -> renvoie EN tel quel (preserve la langue,
         * pas de cross-language `ChronoStart` -> `ChronoDébut`). */
        if (d->nameEN         && owl_name_matches(d->nameEN,         q)) return d->nameEN;
        /* Match alias -> renvoie l'alias tel quel (l'auteur de la
         * native a choisi cet alias, on le respecte). */
        if (d->aliases) {
            for (const char* const* a = d->aliases; *a; a++) {
                if (owl_name_matches(*a, q)) return *a;
            }
        }
    }
    return NULL;
}

int owl_registry_types_count(void) { return g_types.count; }

const OwlTypeDef* owl_registry_type_at(int index) {
    if (index < 0 || index >= g_types.count) return NULL;
    return (const OwlTypeDef*)g_types.items[index];
}

const OwlTypeDef* owl_registry_type_find(const char* name) {
    if (name == NULL) return NULL;
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; i < g_types.count; i++) {
        const OwlTypeDef* t = (const OwlTypeDef*)g_types.items[i];
        if (t->canonical      && owl_name_matches(t->canonical,      q)) return t;
        if (t->canonicalAscii && owl_name_matches(t->canonicalAscii, q)) return t;
        /* Invariant 3 (bilingue FR<->EN) : la forme EN du NOM de type doit
         * resoudre au meme titre que la forme FR. Sans ca, un type dont la
         * forme EN ne vit que dans nameEN (ni dans aliases, ni via un double
         * natif/type) compile en FR mais echoue en EN ("Type inconnu"). */
        if (t->nameEN         && owl_name_matches(t->nameEN,         q)) return t;
        if (t->aliases) {
            for (const char* const* a = t->aliases; *a; a++) {
                if (owl_name_matches(*a, q)) return t;
            }
        }
    }
    return NULL;
}

int owl_registry_consts_count(void) { return g_consts.count; }

const OwlConstDef* owl_registry_const_at(int index) {
    if (index < 0 || index >= g_consts.count) return NULL;
    return (const OwlConstDef*)g_consts.items[index];
}

const OwlConstDef* owl_registry_const_find(const char* name) {
    if (name == NULL) return NULL;
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; i < g_consts.count; i++) {
        const OwlConstDef* c = (const OwlConstDef*)g_consts.items[i];
        if (c->name && owl_name_matches(c->name, q)) return c;
    }
    return NULL;
}

const OwlConstDef* owl_registry_const_find_bilingual(const char* name) {
    if (name == NULL) return NULL;
    const OwlConstDef* fr = owl_registry_const_find(name);
    if (fr) return fr;
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; i < g_consts.count; i++) {
        const OwlConstDef* c = (const OwlConstDef*)g_consts.items[i];
        if (c->nameEN && owl_name_matches(c->nameEN, q)) return c;
    }
    return NULL;
}

int owl_registry_enums_count(void) { return g_enums.count; }

const OwlEnum* owl_registry_enum_at(int index) {
    if (index < 0 || index >= g_enums.count) return NULL;
    return (const OwlEnum*)g_enums.items[index];
}

const OwlEnum* owl_registry_enum_find(const char* name) {
    if (name == NULL) return NULL;
    char q[256];
    owl_normalize_name(name, q, sizeof q);
    for (int i = 0; i < g_enums.count; i++) {
        const OwlEnum* e = (const OwlEnum*)g_enums.items[i];
        if (e->name      && owl_name_matches(e->name,      q)) return e;
        if (e->nameAscii && owl_name_matches(e->nameAscii, q)) return e;
    }
    return NULL;
}

bool owl_enum_contains(const OwlEnum* e, const char* constantName) {
    if (e == NULL || e->values == NULL || constantName == NULL) return false;
    char q[256];
    owl_normalize_name(constantName, q, sizeof q);
    for (const char* const* v = e->values; *v; v++) {
        if (owl_name_matches(*v, q)) return true;
    }
    return false;
}

bool owl_enum_contains_bilingual(const OwlEnum* e, const char* constantName) {
    if (e == NULL || e->values == NULL || constantName == NULL) return false;
    /* Forme FR (les `values` listent les noms FR canon des constantes). */
    if (owl_enum_contains(e, constantName)) return true;
    /* Forme EN : chaque valeur d'enum est une constante du registre qui porte
     * un nameEN bilingue. On resout chaque membre et on matche son alias EN. */
    char q[256];
    owl_normalize_name(constantName, q, sizeof q);
    for (const char* const* v = e->values; *v; v++) {
        const OwlConstDef* c = owl_registry_const_find(*v);
        if (c == NULL || c->nameEN == NULL) continue;
        if (owl_name_matches(c->nameEN, q)) return true;
    }
    return false;
}

const OwlEnum* owl_registry_enum_owner_of_const(const char* constantName) {
    if (constantName == NULL) return NULL;
    for (int i = 0; i < g_enums.count; i++) {
        const OwlEnum* e = (const OwlEnum*)g_enums.items[i];
        if (owl_enum_contains(e, constantName)) return e;
    }
    return NULL;
}

const char* owl_enum_label_for_int(const OwlEnum* e, int64_t target) {
    if (e == NULL || e->values == NULL) return NULL;
    /* Itère les noms de l'enum, lookup chacun dans le registre des consts,
     * matche par valeur. O(values_count * consts_count) théorique mais
     * acceptable en debug — chaque enum a 5-30 valeurs typiques et le
     * registre a ~500 consts. */
    for (const char* const* v = e->values; *v; v++) {
        const OwlConstDef* c = owl_registry_const_find(*v);
        if (c == NULL) continue;
        if (!IS_INTEGER(c->value)) continue;
        if (AS_INTEGER(c->value) == target) return *v;
    }
    return NULL;
}

bool owl_enum_decompose_bitmask(const OwlEnum* e, int64_t target,
                                 char* outBuf, int outBufSize) {
    if (e == NULL || e->values == NULL || outBuf == NULL || outBufSize <= 0) {
        return false;
    }
    if (!e->combinable) return false;
    /* Tenter d'abord un match exact contre les constantes nommées de l'enum.
     * Couvre le cas où le défaut a un label canon (ex: ProtocoleDefaut=0,
     * ccNormal=0). Si pas de match, on tombe sur la décomposition bitmask. */
    {
        const char* exact = owl_enum_label_for_int(e, target);
        if (exact) {
            snprintf(outBuf, outBufSize, "%s", exact);
            return true;
        }
    }
    /* target=0 sans constante nommée à 0 (ex: httpIgnoreErreur,
     * fExtraitPartie) : affiche "0" brut, sémantique "aucun bit set". */
    if (target == 0) {
        snprintf(outBuf, outBufSize, "0");
        return true;
    }
    /* Itère les noms, et pour chaque bit présent dans target qui matche
     * une constante, joint avec " + " (canon pour combinaison). */
    int64_t remaining = target;
    int written = 0;
    outBuf[0] = '\0';
    for (const char* const* v = e->values; *v && remaining != 0; v++) {
        const OwlConstDef* c = owl_registry_const_find(*v);
        if (c == NULL || !IS_INTEGER(c->value)) continue;
        int64_t bit = AS_INTEGER(c->value);
        if (bit == 0) continue;
        if ((remaining & bit) == bit) {
            int n;
            if (written == 0) {
                n = snprintf(outBuf, outBufSize, "%s", *v);
            } else {
                n = snprintf(outBuf + written, outBufSize - written, " + %s", *v);
            }
            if (n < 0 || n >= outBufSize - written) break;
            written += n;
            remaining &= ~bit;
        }
    }
    /* Bits restants non couverts : ajoute la valeur résiduelle pour ne
     * pas masquer de l'info. Cas rare : value qui contient des bits hors
     * de l'enum déclaré. */
    if (remaining != 0 && written < outBufSize - 16) {
        snprintf(outBuf + written, outBufSize - written,
                 written == 0 ? "%lld" : " + %lld", (long long)remaining);
    }
    return written > 0 || target == 0;
}

/* ================================================================
 * Fallback bilingue : resolution EN -> canon ASCII pour fields.
 * Appele par vm.c (OP_STRUCT_GET/SET) quand le lookup direct echoue.
 * ================================================================ */

const char* owl_registry_resolve_field_en_to_canon(const char* structName,
                                                    const char* requestedName) {
    if (!structName || !requestedName) return NULL;
    /* Lookup du type via le registre (case + accent insensible). */
    const OwlTypeDef* t = owl_registry_type_find(structName);
    if (!t || !t->fields) return NULL;

    /* Normalize la cle recherchee pour comparaison. */
    char normReq[128];
    owl_normalize_name(requestedName, normReq, sizeof normReq);

    for (const OwlField* f = t->fields; f->name != NULL; f++) {
        if (!f->nameEN || f->nameEN[0] == '\0') continue;
        if (owl_name_matches(f->nameEN, normReq)) {
            return f->name;  /* canon ASCII storage */
        }
    }
    return NULL;
}

/* ================================================================
 * Instance coerce registry (cf owl_registry.h §INSTANCE COERCE).
 * Table lineaire : peu d'entrees en pratique (<50 a long terme).
 * Lookup case-sensitive sur typeName canon ASCII.
 * ================================================================ */

#define OWL_COERCE_MAX 64
typedef struct {
    char                 typeName[64];
    OwlInstanceCoerceFn  parser;
} CoerceEntry;
static CoerceEntry g_coerce_table[OWL_COERCE_MAX];
static int         g_coerce_count = 0;

void owl_register_instance_coerce(const char* typeName,
                                   OwlInstanceCoerceFn parser) {
    if (!typeName || !parser) return;
    size_t nlen = strlen(typeName);
    if (nlen == 0 || nlen >= sizeof(g_coerce_table[0].typeName)) return;
    /* Dedup : overwrite si meme typeName deja enregistre. */
    for (int i = 0; i < g_coerce_count; i++) {
        if (strcmp(g_coerce_table[i].typeName, typeName) == 0) {
            g_coerce_table[i].parser = parser;
            return;
        }
    }
    if (g_coerce_count >= OWL_COERCE_MAX) return;
    memcpy(g_coerce_table[g_coerce_count].typeName, typeName, nlen);
    g_coerce_table[g_coerce_count].typeName[nlen] = '\0';
    g_coerce_table[g_coerce_count].parser = parser;
    g_coerce_count++;
}

OwlInstanceCoerceFn owl_get_instance_coerce(const char* typeName) {
    if (!typeName) return NULL;
    for (int i = 0; i < g_coerce_count; i++) {
        if (strcmp(g_coerce_table[i].typeName, typeName) == 0) {
            return g_coerce_table[i].parser;
        }
    }
    return NULL;
}

bool owl_has_instance_coerce(const char* typeName) {
    return owl_get_instance_coerce(typeName) != NULL;
}

int owl_instance_coerce_count(void) {
    return g_coerce_count;
}

/*  — hook VM resolvant un ObjUiControlRef nu vers sa prop par
 * defaut. NULL tant que la VM n'a pas appele le setter. */
static OwlUiRefDefaultResolver g_ui_ref_default_resolver = NULL;

void owl_set_ui_ref_default_resolver(OwlUiRefDefaultResolver fn) {
    g_ui_ref_default_resolver = fn;
}

/*  — defaultProperty scalar unwrap. Voir owl_registry.h. */
Value owl_instance_default_unwrap(Value v) {
    /*  — un widget DSL nu (ObjUiControlRef) en contexte scalaire
     * resout vers sa propriete par defaut. Le hook gere le test de type
     * (IS_UI_CONTROL_REF) et la resolution runtime cote VM ; il rend v
     * inchange si ce n'est pas un ref resoluble. Le resultat n'est jamais
     * une ObjInstance composite (deja deballe vers un scalaire), donc le
     * path ObjInstance ci-dessous reste un no-op pour les refs. */
    if (g_ui_ref_default_resolver != NULL) {
        v = g_ui_ref_default_resolver(v);
    }
    if (!IS_INSTANCE(v)) return v;
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL) return v;
    const OwlTypeDef* td = owl_registry_type_find(inst->structName->chars);
    if (td == NULL || td->defaultProperty == NULL) return v;
    ObjString* k = obj_copy_string(td->defaultProperty,
                                    (int)strlen(td->defaultProperty));
    Value out;
    if (table_get(&inst->fields, k, &out)) return out;
    return v;
}

const char* owl_instance_coerce_name_at(int index) {
    if (index < 0 || index >= g_coerce_count) return NULL;
    return g_coerce_table[index].typeName;
}

/* Native __InstanceCoerce(v, typeName) — dispatcher generique.
 * Emis par compiler.c::var_declaration_after_est quand le LHS est un
 * type composite avec parser enregistre. Lookup la table, call parser.
 *
 * Erreur explicite si typeName n'a pas de parser : ne devrait jamais
 * arriver en pratique (compiler.c verifie owl_has_instance_coerce
 * avant d'emettre), mais on signale clairement plutot que silent NIL. */
static Value fn_instance_coerce(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "__InstanceCoerce",
                              "arg", "typeName",
                              "expected", "chaine",
                              NULL);
        return NIL_VAL;
    }
    const char* typeName = AS_CSTRING(args[1]);
    OwlInstanceCoerceFn parser = owl_get_instance_coerce(typeName);
    if (!parser) {
        owl_emit_native_error(E_NATIVE_NOT_FOUND, "__InstanceCoerce",
                              "name", typeName,
                              NULL);
        return NIL_VAL;
    }
    return parser(args[0]);
}

void owl_register_instance_coerce_native(void) {
    owl_define_native("__InstanceCoerce", fn_instance_coerce, 2);
}

/* ================================================================
 * Teardown
 * ================================================================ */

void owl_registry_free(void) {
    free((void*)g_natives.items);
    free((void*)g_types.items);
    free((void*)g_consts.items);
    free((void*)g_enums.items);
    g_natives.items = g_types.items = g_consts.items = g_enums.items = NULL;
    g_natives.count = g_types.count = g_consts.count = g_enums.count = 0;
    g_natives.capacity = g_types.capacity = g_consts.capacity = g_enums.capacity = 0;
    g_coerce_count = 0;
}
