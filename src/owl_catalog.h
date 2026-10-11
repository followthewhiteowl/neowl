// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_catalog_h
#define owl_catalog_h

/* ================================================================
 * LSP CATALOG SHAPE
 * ================================================================
 * Legacy data type kept for the standalone LSP executable, which
 * cannot read the runtime owl_registry directly (it doesn't link the
 * OWL runtime/stdlib).
 *
 * The actual OWL_NATIVE_CATALOG[] array is auto-generated into
 * lsp/lsp_catalog_gen.c by:
 *
 *   owl --dump-lsp-catalog > lsp/lsp_catalog_gen.c
 *
 * Regenerate whenever stdlib_*.c DEFS arrays change. The main owl
 * binary does NOT depend on this data — it walks the live registry.
 * ================================================================ */

/* Une parametre d'une signature : nom + type + description + contraintes
 * d'enum (si la valeur du parametre doit appartenir a un set ferme). */
typedef struct {
    const char* name;
    const char* type;
    const char* typeEN;             /* nom EN du type (invariant 3), == type si pas de
                                     * forme EN distincte. Resolu au dump via le registry. */
    const char* description;
    const char* allowedBaseEnum;    /* enum name, NULL if none */
    const char* allowedOptionsEnum; /* combinable enum name, NULL if none */
    /* Mirror de OwlParam.acceptsLiterals (cf owl_registry.h). Si true,
     * le LSP suggere l'enum + le catalogue general en plus. */
    int acceptsLiterals;
} OwlNativeParamInfo;

/* Une signature individuelle. Une native peut en avoir plusieurs si le
 * canon expose plusieurs syntaxes (ex: Agrege avec/sans valeur
 * initiale, iImage avec 1/3/5/7 args). Le LSP signatureHelp itere
 * cette liste pour proposer toutes les variantes. */
typedef struct {
    const char* signature;    /* "Gauche(s, n)" — display string canonique */
    const char* returnType;   /* "chaine", "entier", "rien"… */
    const char* returnTypeEN; /* nom EN du type de retour (invariant 3 ; "rien"→
                               * "nothing"). == returnType si pas de forme EN. */
    int paramCount;
    OwlNativeParamInfo params[12]; /* Max 12 params (iCadreArrondi a 9) */
} OwlNativeSigInfo;

typedef struct {
    const char* name;         /* Primary name (FR, accented) */
    const char* nameEN;       /* English alias */
    const char* nameAscii;    /* FR name accent-stripped: "ChaineCompare" */
    const char* description;  /* French description */
    const char* descriptionEN;/* English description */
    const char* category;     /* Category for grouping */
    int signatureCount;       /* Nombre d'entries dans signatures[] */
    const OwlNativeSigInfo* signatures; /* Pointer vers array sized signatureCount.
                                         * Garanti >= 1 (au moins une sig par native).
                                         * signatures[0] = sig "canonique" / primaire
                                         * pour les consumers mono-sig (legacy). */
    const char* const* aliases; /* Liste NULL-terminee d'alias, NULL si aucun.
                                 * Parite formatter : owl_registry_native_
                                 * canonical_for matche d->aliases cote registre,
                                 * le backend catalogue doit faire pareil. */
} OwlNativeInfo;

extern const OwlNativeInfo OWL_NATIVE_CATALOG[];
extern const int OWL_NATIVE_CATALOG_COUNT;

/* ================================================================
 * CONSTANT CATALOG — runtime constants exposed to user code.
 * Populated from the registry (OwlConstDef) by dump_lsp_catalog().
 * ================================================================ */

typedef struct {
    const char* name;         /* "httpGet", "RC", "ValPI"… */
    const char* nameEN;       /* English alias canon (invariant 3), NULL si absent */
    const char* category;     /* "HTTP", "Constantes diverses"… */
    const char* description;  /* FR description */
} OwlConstInfo;

extern const OwlConstInfo OWL_CONST_CATALOG[];
extern const int OWL_CONST_CATALOG_COUNT;

/* ================================================================
 * TYPE CATALOG — builtin struct-like types with their fields.
 * Used for completion after `..` on typed variables (properties).
 * ================================================================ */

typedef struct {
    const char* name;               /* "URL", "Méthode"… */
    const char* type;               /* "chaîne", "entier"… */
    const char* description;        /* FR description */
    const char* allowedBaseEnum;    /* enum name this field must pick from, NULL if none */
    const char* allowedOptionsEnum; /* combinable enum name OR-ed with base, NULL if none */
    const char* nameAscii;          /* nom du champ accent-strip (cle de match du
                                     * formatter ; == name si deja ASCII). */
    const char* nameEN;             /* alias EN du champ (invariant 3), == name si absent */
    const char* descriptionEN;      /* description EN du champ, == description si absent */
    const char* typeEN;             /* nom EN du type du champ (resolu via registry), == type si absent */
    const char* callbackSig;        /* signature attendue d'un field callback (type "procédure") :
                                     * liste de types canon separes par virgule ("chaîne,booléen"),
                                     * "" pour un callback sans parametre, NULL pour tout autre field. */
} OwlFieldInfo;

/*  — etat indexable via ..Etats[etat] d'un type collection styleXxxEtats.
 * Derive de STYLE_PALETTES/STATES_* (widgets) + regle survol-only des menus
 * (source unique compiler.c). Sert la completion + le hover dans `Etats[`. */
typedef struct {
    const char* fr;   /* forme FR canon : "survol", "focus", "appuyé"... */
    const char* en;   /* forme EN canon : "hover", "focus", "pressed"... */
} OwlStateInfo;

typedef struct {
    const char* name;         /* Canonical accented: "httpRequête" */
    const char* nameAscii;    /* ASCII: "httpRequete" */
    const char* nameEN;       /* English alias canon (invariant 3), NULL si absent */
    const char* category;
    const char* description;
    const char* descriptionEN;/* English description, NULL si absent */
    const OwlFieldInfo* fields;
    int fieldCount;
    const char* literalKeysEnum;  /* enum name whose values are the keys of a {} literal, NULL if none */
    /*  — etats valides indexables via ..Etats[etat] (types collection
     * styleXxxEtats). NULL + 0 si le type n'en porte pas. */
    const OwlStateInfo* indexStates;
    int indexStateCount;
} OwlTypeInfo;

extern const OwlTypeInfo OWL_TYPE_CATALOG[];
extern const int OWL_TYPE_CATALOG_COUNT;

/* Lookup by name (case + accent insensitive). Returns NULL if absent. */
const OwlTypeInfo* owl_type_info_find(const char* name);

/* ================================================================
 * ENUM CATALOG — named sets of constant identifiers restricting a
 * param or field to a closed list. Populated from OwlEnum registry.
 * ================================================================ */

typedef struct {
    const char*         name;         /* "httpMéthodes", "errInfo"… */
    const char*         nameAscii;    /* ASCII fallback */
    const char*         nameEN;       /* English alias canon (invariant 3), NULL si absent */
    const char*         category;     /* "HTTP", "Erreur"… */
    const char*         description;  /* FR */
    const char* const*  values;       /* NULL-terminated member names */
    int                 valueCount;
    int                 combinable;   /* 0/1 — combinable with '+' */
} OwlEnumInfo;

extern const OwlEnumInfo OWL_ENUM_CATALOG[];
extern const int OWL_ENUM_CATALOG_COUNT;

/* Lookup by name (case + accent insensitive). Returns NULL if absent. */
const OwlEnumInfo* owl_enum_info_find(const char* name);
/* Membership check (case + accent insensitive). */
int owl_enum_info_contains(const OwlEnumInfo* e, const char* constName);

/* ================================================================
 * METHOD CATALOG — extension methods callable via dot syntax.
 * Populated from owl_methods.c method tables by dump_lsp_catalog().
 * e.g. "Majuscule" is callable as str.Majuscule() with receiverType="chaine"
 * ================================================================ */

typedef struct {
    const char* name;         /* Method name: "Majuscule", "Ajoute"… */
    const char* receiverType; /* OWL type name: "chaine", "tableau"… */
    int arity;                /* Total args INCLUDING receiver */
    int isAlias;              /* 1 if this is an alias (EN version / short form) of
                               * a prior canonical entry pointing to the same impl.
                               * The LSP uses this to filter completion suggestions
                               * (show only canonical names) while keeping aliases
                               * callable/resolvable. */
    const char* nativeName;   /*  — canonical name of the underlying native
                               * (resolved via NativeFn at dump time). Lets the LSP
                               * show the real signature minus the receiver param
                               * (`Cherche(valeur)` not `Cherche()`). NULL if the
                               * method's fn has no registry binding. */
} OwlMethodInfo;

extern const OwlMethodInfo OWL_METHOD_CATALOG[];
extern const int OWL_METHOD_CATALOG_COUNT;

/* ================================================================
 * DSL PROPERTY CATALOG — proprietes des blocs DSL (Fenetre, Libelle,
 * Bouton, Saisie, Table, colonne, Etat, blocEtat, LibelleEtat).
 * Populated from src/ui_catalog.c by dump_lsp_catalog().
 *
 * Le LSP s'en sert pour :
 *   - autocomplete dans 'PROPRIETES ... FIN' (filtre par kind)
 *   - propValueContext : si enumName != NULL ET le user tape 'P = <cur>',
 *     proposer les valeurs de l'enum correspondant
 *   - hover / signature help (descFR + type)
 *
 * Formatter consomme la meme table pour canoniser les noms (largeur →
 * Largeur, width → Width selon la langue detectee).
 * ================================================================ */

typedef struct {
    const char* kind;        /* "Fenetre", "Libelle", "Bouton", "Saisie", "Table",
                              * "colonne", "Etat", "blocEtat", "LibelleEtat" */
    const char* name;        /* Canonical FR (avec accents) */
    const char* nameAscii;   /* FR sans accent (= name si pas d'accent) */
    const char* nameEN;      /* EN alias */
    const char* type;        /* "int", "string", "bool", "color", "enum", "tout", "position" */
    const char* enumName;    /* nom de l'OwlEnum si la valeur est contrainte, NULL sinon */
    const char* descFR;
    const char* descEN;
    const char* objectType;  /* nom du type composé cible si type=="object"
                              * (ex Style→styleSaisie), NULL sinon. En fin de
                              * struct : les inits sans ce champ le laissent
                              * NULL. Permet au LSP de résoudre les champs d'un
                              * `Style = { <cursor> }` (prop→type réflexif). */
} OwlDSLPropInfo;

extern const OwlDSLPropInfo OWL_DSL_PROP_CATALOG[];
extern const int OWL_DSL_PROP_CATALOG_COUNT;

/* ================================================================
 * V1.5-C : COMPOSED TYPE CATALOG — types builtin composes (Cadre,
 * Style, Police, Coin, Trait, ...). Pour chaque type, liste de fields
 * + sous-type eventuel + enum constraint.
 *
 * Le LSP s'en sert pour autocomplete dans `var = { <cursor> }` :
 *   - liste les fields du type cible
 *   - filtre ceux deja presents dans le brace courant
 *   - propose les valeurs d'enum sur RHS de field si subType est un nom
 *     d'enum connu (pas une instance)
 * ================================================================ */

typedef struct {
    const char* typeName;     /* "Cadre", "Style", "Police", "Coin", "Trait" */
    const char* fieldName;    /* "Couleur", "Epaisseur", "CoinHautGauche"... */
    const char* nameEN;       /* Forme EN canon du field ("Color", "Thickness"),
                               * reprise de OwlField.nameEN a la generation.
                               * NULL si le type n'a pas de declaration de field
                               * cote registre (template construit a la volee). */
    const char* fieldType;    /* "int", "string", "bool", "instance", "enum" */
    const char* subTypeName;  /* Si fieldType="instance" : nom du sous-type
                               * (Coin/Trait/Police/Cadre). Si fieldType="enum" :
                               * nom de l'OwlEnum. NULL sinon. */
    const char* descFR;
} OwlComposedTypeField;

extern const OwlComposedTypeField OWL_COMPOSED_TYPE_CATALOG[];
extern const int OWL_COMPOSED_TYPE_CATALOG_COUNT;

/* ================================================================
 * BUILTIN KIND PROPERTY CATALOG — propriétés des types primitifs
 * (tableau, file/pile/liste, JSON, date, heure, dateheure, durée).
 *
 * Source canon : compiler.c::PROPS_*. Emis ici via dump_lsp_catalog
 * pour que le LSP consomme la meme source que le formatter et le
 * compileur (zero duplication, zero drift). Une entree par nom UNIQUE
 * (dedup par nameFR), sans les alias de tolerance.
 * ================================================================ */

typedef struct {
    const char* nameFR;       /* "Année", "EnJours", "Occurrence"... */
    const char* nameEN;       /* alias EN si distinct, sinon == nameFR */
    const char* descFR;
    const char* descEN;
    const char* valueType;    /* "entier", "réel", "chaine", "Date", "Heure", "booléen" */
    /* Kinds qui acceptent ce nom, en ASCII minuscule, encadres et separes par
     * des espaces : " date dateheure ". Le dedup par nameFR fait perdre le
     * kind d'origine ; sans cette colonne le consommateur ne peut que servir
     * les 17 lignes a tout le monde. Le test d'appartenance est un strstr de
     * " <kind> " — les bornes evitent qu'un prefixe matche (date/dateheure). */
    const char* kinds;
} OwlBuiltinPropInfo;

extern const OwlBuiltinPropInfo OWL_BUILTIN_PROP_CATALOG[];
extern const int OWL_BUILTIN_PROP_CATALOG_COUNT;

#endif
