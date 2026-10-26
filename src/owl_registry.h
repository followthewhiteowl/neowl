// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_registry_h
#define owl_registry_h

/* ================================================================
 * UNIFIED DECLARATION REGISTRY
 *
 * Single source of truth for native functions, builtin types, and
 * runtime constants. Every stdlib_*.c module declares a static
 * OwlNativeDef[] array and hands it to the registry at init time.
 *
 * Consumers that read from the registry:
 *   - owl_define_native_wl (runtime registration via register_module)
 *   - formatter.c (fmt_load_from_registry — accent/casing norms)
 *   - main.c --doc, --vscode-data
 *   - lsp/lsp_catalog.c (LSP hover/completion/signature)
 *
 * Design intent: adding a new native means editing ONE file — the
 * stdlib_*.c where it lives — with one entry in its DEFS[] array.
 * ================================================================ */

#include "common.h"
#include "value.h"
#include "object.h"

/* -- Genre grammatical du nom FR d'un type ---------------------
 * Donnee PASSIVE consommee UNIQUEMENT par `owl translate` pour choisir
 * l'article francais (un/une) lors de la traduction EN->FR d'une declaration.
 * Le COMPILATEUR est gender-agnostic (accepte un/une indifferemment) et le
 * FORMATTER ne consomme PAS ce champ. Init zero = OWL_GENRE_NONE (fallback). */
typedef enum {
    OWL_GENRE_NONE = 0,  /* non specifie -> translate garde l'article ecrit */
    OWL_GENRE_M,         /* masculin : "un" (un entier, un Bouton) */
    OWL_GENRE_F          /* feminin  : "une" (une chaine, une Fenetre) */
} OwlGenre;

/* -- Enumerated value sets (restrict a param/field to named constants) -- */

/* An OwlEnum names a set of constant-identifier names. Members are still
 * regular OwlConstDef globals — the enum only stores name references.
 *
 * `combinable = true` means the values can be OR-ed together with `+` or
 * `|` in source code (e.g. tccSansCasse + tccSansAccent).
 *
 * Declared once per stdlib_*.c module that owns the constants. Consumed
 * by the LSP (strict completion), the compiler (compile-time validation
 * of RHS identifiers against allowedBase/allowedOptions), and --doc. */
typedef struct OwlEnum {
    const char*         name;           /* "httpMéthodes", "errInfo"… */
    const char*         nameAscii;      /* ASCII fallback, NULL if name is ASCII */
    const char*         category;       /* "HTTP", "Erreur"… */
    const char*         descFR;
    const char*         descEN;
    const char* const*  values;         /* NULL-terminated constant names */
    bool                combinable;
    /* Opt-out explicite de la zone sentinel. Par defaut, tout enum
     * combinable=false DOIT avoir ses valeurs en zone INT32_MIN+offset
     * (verifie par self-test rule 20). Le flag `force_small_int=true`
     * autorise des valeurs entieres positives QUAND la valeur entiere
     * elle-meme porte une semantique runtime (ex : enum Couleur ou
     * Blanc=0xFFFFFF est l'encodage RGB direct passe au backend Qt).
     * A reserver aux cas ou sentinel-er detruirait la semantique. */
    bool                force_small_int;
    /* nameEN : alias EN canon du nom d'enum (invariant bilingue n°3).
     * Ajoute en fin de struct → retro-compatible C99 (initializers
     * positionnels existants laissent nameEN=NULL). NULL autorise tant
     * que la couverture n'est pas complete (pas de self-test bloquant
     * encore). Le nom d'enum n'est pas un symbole runtime resolvable ;
     * ce champ sert l'expo bilingue LSP. */
    const char*         nameEN;
} OwlEnum;

/* -- Native function metadata -------------------------------------- */

typedef struct {
    const char*    name;             /* "s" */
    const char*    type;             /* "chaîne|tableau" */
    const char*    desc;             /* "Chaîne ou tableau source" */
    const OwlEnum* allowedBase;      /* NULL or the enum this param must pick from */
    const OwlEnum* allowedOptions;   /* NULL or a combinable enum OR-ed with allowedBase */
    /* Si true, le LSP propose les valeurs de l'enum en suggestion mais
     * NE court-circuite PAS le catalogue general — les variables locales,
     * literals et autres expressions restent eligibles. Pour les params
     * du genre "destination = indice 1-based OU constante tdSuivant".
     * Defaut false : enum strict. */
    bool           acceptsLiterals;
    /* Si true, un argument identifiant NU prefixe "aws" a cette position est
     * resolu a la compilation en son nom d'action (symbole aws<Service><Action>
     * -> litteral "<Action>"). Un identifiant "aws"-prefixe introuvable devient
     * une erreur compile ; un littéral / une variable non prefixee reste une
     * expression normale. Voir compiler.c (resolution symbole d'action AWS). */
    bool           awsAction;
    /* Opt-in couverture scenario par valeur d'enum. Si true ET que
     * le param est contraint par un enum (allowedBase), le self-test (regle 61)
     * exige qu'au moins un scenario public @sujet:<native> reference CHAQUE
     * valeur de l'enum. Cible les enums a branchement (service/protocole) dont
     * un cas non exerce donne un vert trompeur (classe du bug de cadrage EC2).
     * A NE PAS activer sur les enums descriptifs (typeMime, algoHachage…) :
     * deluge de faux positifs. Retro-compatible C99 (champ en fin de struct,
     * defaut false pour tous les initializers positionnels existants). */
    bool           requireEnumValueCoverage;
} OwlParam;

typedef struct {
    int                arity;       /* declared parameter count, -1 = variadic */
    const OwlParam*    params;      /* terminated by {NULL,NULL,NULL} */
    const char*        returnType;  /* "entier", "chaîne", "rien"… */
    const char*        returnDesc;  /* what the return means */
} OwlSignature;

/* Flag bits */
#define OWL_NAT_DEPRECATED   (1u << 0)
#define OWL_NAT_IS_PROPERTY  (1u << 1)  /* e.g. Chrono..Valeur */
#define OWL_NAT_INTERNAL     (1u << 2)  /* hidden from --doc/--vscode-data */
#define OWL_NAT_PURE         (1u << 3)  /* deterministe, sans effet de bord, eval-able a compile-time */
#define OWL_NAT_OUT_PARAMS   (1u << 4)  /* a des out-params variadic, cf firstOutParamArg */

/* Declarative prefix-method binding.
 *
 * When declaring a native in OwlNativeDef.methodBindings, the registry will
 * auto-call owl_register_method() at init time. This removes the need for a
 * separate manual owl_register_method() call in stdlib_*_init() — a single
 * DEFS entry captures both the global function AND its prefix methods.
 *
 * Example:
 *   static const OwlMethodBinding CHRONO_DEBUT_METHODS[] = {
 *       {"Chrono", "Debut", -1},
 *       {"Chrono", "Start", -1},
 *       {NULL, NULL, 0}
 *   };
 *   { .canonical = "ChronoDébut", ..., .impl = fn_chrono_debut,
 *     .methodBindings = CHRONO_DEBUT_METHODS }
 */
typedef struct {
    const char* receiverType;  /* "chaine", "MockNative", "Chrono", "tableau", etc. */
    const char* methodName;    /* "Quand", "Debut", "VersDate" (ASCII, stripped) */
    int         arity;         /* total args INCLUDING receiver; -1 = variadic */
} OwlMethodBinding;

typedef struct OwlNativeDef {
    const char*         canonical;       /* UTF-8 accented official form — what fmt writes */
    const char*         canonicalAscii;  /* ASCII-only fallback for lookup (NULL if canonical is ASCII) */
    const char*         nameEN;          /* English alias (registered as runtime alias) */
    const char* const*  aliases;         /* NULL-terminated list of additional accepted names */
    const char*         category;        /* "Chaines", "HTTP", … */
    const char*         family;          /* optional subgroup, e.g. "Conversion" */
    const char*         descFR;
    const char*         descEN;
    const OwlSignature* signatures;      /* NULL-terminated (sig.arity == -2 marks end) */
    const char* const*  examples;        /* NULL-terminated source snippets */
    NativeFn            impl;            /* C implementation */
    unsigned            flags;
    const OwlMethodBinding* methodBindings; /* NULL-terminated; NULL if no methods */
    /* Out-params (canon, cf VISION §5bis).
     * Activé uniquement si flag OWL_NAT_OUT_PARAMS set. Sinon les champs
     * sont ignorés -> par défaut tous les DEFS héritent du comportement
     * standard (par valeur). Quand activé : les args dont l'index i
     * vérifie `i >= firstOutParamArg && (lastOutParamArg < 0 || i <= lastOutParamArg)`
     * sont par référence -- la native peut muter `args[i] = newValue`, le
     * compilateur valide lvalue + émet le writeback vers la variable du
     * caller.
     *   - lastOutParamArg = -1 (par défaut)  → variadic « jusqu'à la fin »
     *     (utilisé par RegexVérifie / RegexCherche pour les groupes capturés).
     *   - lastOutParamArg = N >= firstOutParamArg → fenêtre fixe [first..last].
     *     Utilisé par Sérialise (1..1) et Désérialise (0..0). */
    int                 firstOutParamArg;
    int                 lastOutParamArg;
    /* Natives polymorphes : nombre de formes/cas d'operandes reellement geres
     * par le dispatch runtime (0 = non applicable, defaut). Opt-in : declarer
     * formCount >= 2 active la regle self-test 58, qui verifie que descFR/descEN
     * mentionnent bien ce nombre de "formes"/"forms" (ou "cas"/"cases"). Garde-fou
     * anti-derive : quand le code gagne une forme, bumper formCount ici force la
     * mise a jour coherente de la description user-facing (sinon `owl explain`
     * ment). Voir registry_self_test.c regle 58. */
    int                 formCount;
} OwlNativeDef;

/* -- Builtin types (Chrono, httpRequête, Definition, httpRéponse…) - */

typedef struct {
    const char*    name;             /* canon ASCII storage, ex: "URL", "Methode" */
    const char*    nameAccented;     /* forme accentuée FR alternative, ex: "Méthode" — NULL si name == accentué */
    const char*    type;             /* "chaîne", "entier", "tableau associatif de chaînes"… */
    const char*    defaultValue;     /* source-form default, e.g. "\"\"", "1", "Vrai" */
    const char*    descFR;
    const char*    descEN;
    const OwlEnum* allowedBase;      /* NULL or the enum this field must pick from */
    const OwlEnum* allowedOptions;   /* NULL or combinable enum OR-ed with allowedBase */
    /* nameEN : alias EN canonique, ex: "Method", "Headers", "Database".
     * Obligatoire — rule 11 strict absolu sur tous les types reifies.
     * Permet l'invariant bilingue n°3 : `req.Method = httpGet` resout vers
     * le storage `Methode` via le runtime fallback EN->FR (cf vm.c). */
    const char*    nameEN;
    /* Propriete calculee a la volee : si non-NULL, OP_STRUCT_GET ignore
     * le storage et appelle ce getter avec args[0] = instance. Permet aux
     * champs declares (visibles LSP + valides au compile-time via la voie
     * OwlField standard) de retourner une valeur live et non un snapshot
     * stale. Cf  (Chrono..Valeur desync). Le getter doit retourner
     * la valeur typee comme declaree par `type`. */
    NativeFn       computeGetter;
    /*  — signature contractuelle d'un field de type "procédure"
     * (callback ..SurToken / ..SurÉpoque / futures). Liste declarative de
     * types canon separes par virgule, ex "chaîne,booléen" ou
     * "entier,entier,réel". Lue par le compilateur au site d'affectation
     * `recv..prop = MaProc` : arite + types de params EXACTS exiges quand le
     * RHS resout vers une PROCEDURE top-level connue. Obligatoire sur tout
     * field type:"procédure" — garde d'integrite dans registry_self_test
     * (rule 60). NULL pour tout autre field (defaut C99 zero-init). */
    const char*    callbackSig;
} OwlField;

typedef struct OwlTypeDef {
    const char*         canonical;       /* "httpRequête" */
    const char*         canonicalAscii;  /* "httpRequete" */
    const char* const*  aliases;
    const char*         category;        /* "HTTP" */
    const char*         descFR;
    const char*         descEN;
    const OwlField*     fields;          /* NULL-terminated */
    /* Optional: called after the template is cloned, to initialise
     * fields whose value can't be expressed as a source literal
     * (associative arrays, buffers, nested instances…). */
    void              (*buildInstance)(ObjInstance*);
    /* True si le type accepte une coercion declaration-pilote
     * (`var est un T = <expr>` parse <expr> vers T via le registre
     * owl_register_instance_coerce). Le parser correspondant doit
     * etre enregistre dans le stdlib_<module>_init() — verifie par
     * registry_self_test rule 26.
     *
     * Default false (la struct C99 zero-init met false partout). Mettre
     * a true UNIQUEMENT pour les types qui ont un parser declare et
     * une semantique de coerce documentee (cf doc owl_registry.h
     * §INSTANCE COERCE). */
    bool                coercible;
    /* Valeur d'init par defaut pour une declaration `var est un T` sans
     * init explicite. Utilise par les types alias scalaire registry
     * (Couleur, zipArchive, MockNative) qui n'ont pas de template
     * runtime (fields == NULL) -- le compilateur emet `defaultValue`
     * directement au lieu de `__StructClone <name>` qui crashait
     * faute de global enregistre. Pour les types composes avec fields
     * non-NULL, on garde le chemin `__StructClone` historique.
     *
     * hasDefault est le sentinel "defini par le type" -- un
     * defaultValue == NIL_VAL legitime (zipArchive, MockNative) est
     * distinct de "pas de defaut declare" (cas par defaut, hasDefault
     * == false, car Value zero-init vaut VAL_BOOL faux et pas VAL_NIL,
     * donc on ne peut pas se baser sur la valeur seule). */
    bool                hasDefault;
    Value               defaultValue;
    /* Optionnel : enum dont les VALEURS sont les cles legitimes d'un
     * literal `{}` de ce type. Analogue type-level de OwlField.allowedBase
     * (qui contraint un field a un enum). Quand set, le LSP propose ces
     * valeurs en autocomplete dans `var est un T = { <curseur> }`. Compte
     * comme reference d'enum pour le self-test rule 18. Ex :
     * chaineMultilingue.literalKeysEnum = &E_langueXxx (langueFrancais...). */
    const OwlEnum*      literalKeysEnum;
    /*  — Marque une entree de type comme metadonnee de littoral d'un
     * type PRIMITIF (mot-cle scanner + stockage natif VM), pas un vrai type
     * compose. Cas unique : durée (TOKEN_DUREE), dont les fields decrivent la
     * CONSTRUCTION du littoral `{ Heure = 2 }` pour la completion LSP. explain
     * fusionne cette entree dans la section primitif au lieu de rendre une
     * section [Type composé] autonome (label semantiquement faux). Le flag est
     * explain-only : la completion LSP consomme les fields inchanges.
     * Default false (C99 zero-init). */
    bool                primitiveLiteral;
    /*  — Propriete par defaut (scalar unwrap). Quand non-NULL, une
     * ObjInstance de ce type assignee a un slot scalaire (entier/reel/...)
     * ou utilisee en arithmetique avec un scalaire deballe automatiquement
     * sur ce field. Doit etre le nom canon ASCII d'un OwlField du type.
     *
     * NULL = pas de deballage scalaire (defaut C99 zero-init, comportement
     * historique de tous les types composites preserve : Police, Cadre,
     * httpRequete, etc. restent inchanges). */
    const char*         defaultProperty;
    /*  — Callback post-write par type. Appele apres chaque
     * ecriture reussie sur un field de l'instance via OP_STRUCT_SET.
     * Recoit l'instance et le nom canon ASCII FR du field qui vient d'etre
     * ecrit. Permet a un type de rebalancer ses fields derives (ex: Couleur
     * recalcule R/V/B <-> packed <-> T/S/L selon le field mute).
     *
     * NULL = no-op (defaut C99 zero-init, aucun side-effect sur les types
     * existants). L'implementeur doit eviter la recursion infinie (le hook
     * re-ecrit des fields qui re-declencheraient le hook) via un flag de
     * re-entrance ou un set direct sur le storage interne. */
    void              (*onFieldSet)(ObjInstance*, const char* fieldName);
    /* nameEN : alias EN canon du nom de type (invariant bilingue n°3).
     * Ajoute en fin de struct → retro-compatible C99 (initializers
     * positionnels existants laissent nameEN=NULL). NULL autorise tant
     * que la couverture n'est pas complete (pas de self-test bloquant
     * encore). Sert l'expo bilingue LSP + future resolution EN→FR des
     * noms de types reifies. */
    const char*         nameEN;
    /* genre : genre grammatical du nom FR. Donnee PASSIVE lue
     * UNIQUEMENT par `owl translate` (article EN->FR : a/an -> un/une selon
     * le genre du type qui suit). Ajoute en fin de struct -> retro-compatible
     * C99 (initializers existants laissent genre=OWL_GENRE_NONE). Le compilo
     * et le formatter ignorent ce champ. */
    OwlGenre            genre;
    /*  — factory native pour une declaration `var est un T` SANS
     * init explicite, quand le type est un OBJET MUTABLE natif (pas un alias
     * scalaire a defaultValue constant, ni une struct a template clonable).
     * Non-NULL + hasDefault=true : le compilateur emet GET_GLOBAL <factory>
     * + CALL 0 (instance fraiche par declaration) au lieu d'emettre la
     * constante defaultValue partagee. Symetrique generique du cas
     * chaineMultilingue (__MultilingueNew), qui restait code en dur. La
     * factory doit etre un native arite 0 enregistre (typiquement INTERNAL,
     * prefixe __). Ajoute en fin de struct -> retro-compatible C99. */
    const char*         defaultFactoryNative;
    /*  — type ACCESSEUR non declarable seul. NULL (defaut C99) = type
     * declarable normalement. Non-NULL = le compilateur REJETTE toute
     * declaration standalone `var est un T` avec un message oriente solution,
     * la valeur portant le hint d'obtention specifique au type (ex.
     * "doc..Cellule[ligne, colonne]"). Reservee aux sous-objets obtenus
     * uniquement par indexation sur un objet porteur (xlsCellule/xlsLigne/
     * xlsColonne) : ils ont des fields mais aucun template runtime dans
     * vm.types ni factory -> `__StructClone` crashait ("Variable inconnue").
     * Le rejet au compile remplace ce crash runtime. */
    const char*         notDeclarableHint;
} OwlTypeDef;

/* -- Runtime constants (httpGet, RC, ValPI, psdMiseEnForme…) ------- */

typedef struct OwlConstDef {
    const char* name;            /* "httpGet", "CRLF", "ValPI" */
    const char* category;        /* "HTTP", "Constantes diverses" */
    Value       value;           /* INTEGER_VAL(1), OBJ_VAL(string…), REAL_VAL(…) */
    const char* descFR;
    const char* descEN;
    /* nameEN : alias EN canon (invariant bilingue n°3). NULL autorise
     * tant que la couverture n'est pas complete (pas de self-test bloquant
     * sur ce champ pour l'instant). Quand non-NULL, enregistre en
     * runtime-only dans vm.globals (mirror de l'alias ASCII auto-genere)
     * sans doublonner l'entree registre — le LSP ne voit qu'une entree. */
    const char* nameEN;
} OwlConstDef;

/* -- Registry API -------------------------------------------------- */

/* Registration. Called once per module from its stdlib_*_init().
 * defs must be alive for the program's lifetime (usually static const).
 * Each module:
 *   - calls owl_define_native_wl() for the canonical name + nameEN + aliases
 *   - appends the definitions to the flat registry for later introspection
 */
void owl_registry_register_module(const OwlNativeDef* defs,  int count);
void owl_registry_register_types (const OwlTypeDef*   types, int count);
void owl_registry_register_consts(const OwlConstDef*  consts, int count);
/* Enum tables — takes an array of OwlEnum POINTERS (nil-terminated is not
 * used; pass count explicitly). The registry stores each pointer. */
void owl_registry_register_enums (const OwlEnum* const* enums, int count);

/* Enregistre un alias ASCII (sans accents) d'une constante deja declaree
 * dans le registre via owl_registry_register_consts.
 *
 * Garantit l'invariant "tout alias ASCII a un canonical accentue connu" :
 *   - lookup du canonical via owl_registry_const_find (case+accent
 *     insensible)
 *   - si trouve : copie sa Value dans vm.globals sous la clef `alias`
 *   - si absent : log un warning sur stderr et n'enregistre rien
 *
 * Pourquoi ne pas enregistrer l'alias dans le registre lui-meme :
 * doublonner consts dans le registre crée des doublons LSP/autocomplete
 * (cf CLAUDE.md "Aliases ASCII des constantes : runtime-only"). Le
 * runtime utilise vm.globals, le LSP/grammar/formatter utilisent le
 * registre — chacun voit ce qu'il doit voir. */
void owl_registry_register_ascii_alias(const char* canonical, const char* alias);

/* Verifie si `methodName` est une methode enregistree pour le type
 * `typeName` (ex: "sqlConnexion", "chaine", "tableau"). Utilise par
 * le compiler pour distinguer propriete vs methode au compile. */
bool owl_method_exists_for_type_name(const char* typeName,
                                      const char* methodName, int methodNameLen);

/* Introspection — iterated by --doc, --vscode-data, LSP, formatter. */
int  owl_registry_count(void);
const OwlNativeDef* owl_registry_at(int index);
const OwlNativeDef* owl_registry_find(const char* name);  /* case + accent insensitive */
const OwlNativeDef* owl_registry_find_by_fn(NativeFn fn);  /* reverse lookup by impl pointer */

/* Cherche `input` dans le registre des natives et renvoie le nom
 * d'affichage approprié au formattage :
 *   - match `canonical` (canon FR accent) ou `canonicalAscii` → renvoie
 *     `canonical` (canon FR avec accent, restauration automatique)
 *   - match `nameEN` → renvoie `nameEN` tel quel (préserve EN, PAS de
 *     cross-language FR↔EN au format)
 *   - match un `alias` → renvoie l'alias matché tel quel
 * Renvoie NULL si rien ne matche. Utilisé par formatter.c et LSP. */
const char* owl_registry_native_canonical_for(const char* input);

int  owl_registry_types_count(void);
const OwlTypeDef*   owl_registry_type_at(int index);
const OwlTypeDef*   owl_registry_type_find(const char* name);

int  owl_registry_consts_count(void);
const OwlConstDef*  owl_registry_const_at(int index);
const OwlConstDef*  owl_registry_const_find(const char* name);
/* Variante bilingue : matche le nom canon FR puis, a defaut, le nameEN. Utilise
 * par le const-folder du DSL widget pour qu'une valeur d'enum en forme EN
 * (`Layout = dispFree`) se replie en constante au meme titre que la forme FR. */
const OwlConstDef*  owl_registry_const_find_bilingual(const char* name);

int  owl_registry_enums_count(void);
const OwlEnum*  owl_registry_enum_at(int index);
const OwlEnum*  owl_registry_enum_find(const char* name);
/* Case + accent insensible membership check. Returns false if e is NULL. */
bool owl_enum_contains(const OwlEnum* e, const char* constantName);

/* Variante bilingue : accepte la forme FR (nom canon du membre) OU la forme EN
 * (nameEN de la constante membre, resolu via le registre). Utilise par la
 * validation des valeurs d'enum en assignation DSL widget pour qu'une fenetre
 * ecrite en anglais (`Layout = dispVertical`) compile au meme titre que FR. */
bool owl_enum_contains_bilingual(const OwlEnum* e, const char* constantName);

/* Reverse lookup : retourne le premier OwlEnum dont la liste `values` contient
 * `constantName`. NULL si aucun match. Utilise par le compilateur pour le check
 * compile-time enum-cross-type : sait dire si une constante donnee
 * appartient a un autre enum que celui attendu. */
const OwlEnum* owl_registry_enum_owner_of_const(const char* constantName);

/* Validation des methodBindings : true si `name` est un receiverType reconnu
 * par register_method_bindings. Sinon le binding est silencieusement ignore
 * a l'init (cf register_method_bindings dans owl_registry.c). Utilise par
 * registry_self_test regle 16 pour attraper les typos avant le runtime. */
bool owl_method_receiver_type_is_known(const char* name);

/* Reverse lookup : retourne le nom de la première constante de l'enum dont
 * la valeur runtime matche `target`. NULL si aucun match (ou enum NULL).
 * Utilisé par le debugger pour afficher `Méthode = httpGet` au lieu de la
 * valeur brute quand le field a un allowedBase. */
const char* owl_enum_label_for_int(const OwlEnum* e, int64_t target);

/* Décompose un bitmask `target` en somme de constantes nommées de l'enum
 * (qui doit être combinable). Écrit dans outBuf le résultat sous forme
 * "labelA + labelB + labelC". Retourne true si l'enum est combinable et
 * que au moins une représentation a pu être écrite. Pour target=0 sur un
 * enum combinable, écrit "0" (pas de label canon "aucun" en doc). */
bool owl_enum_decompose_bitmask(const OwlEnum* e, int64_t target,
                                 char* outBuf, int outBufSize);

/* Pour un type reifie identifie par son structName (canon ASCII), si
 * `requestedName` matche le `nameEN` d'un OwlField, retourne son `name`
 * canon ASCII (le storage runtime). Sinon NULL.
 *
 * Permet le fallback EN->FR au runtime : `req.Method` -> `Methode` quand
 * le compilateur OWL n'a pas resolu le nom au compile-time. Comparaison
 * case + accent insensible (via normalize_name interne).
 *
 * Appele depuis vm.c (OP_STRUCT_GET/OP_STRUCT_SET) seulement si le
 * lookup direct dans inst->fields a echoue. Pas d'overhead pour les
 * acces FR canon (cas majoritaire). */
const char* owl_registry_resolve_field_en_to_canon(const char* structName,
                                                    const char* requestedName);

/* ================================================================
 * INSTANCE COERCE REGISTRY — coercion declarative pour types composites
 *
 * Pattern : `var est un T = <expr>` declenche une conversion automatique
 * depuis le type naturel d'entree vers le type composite T. Exemples
 * canon :
 *   monImage est une Image = "fichier.png"  → dChargeImage(path)
 *   monFic   est un FichierDisque = fOuvre(...) → wrap int handle
 *   u        est un UUID = "550e8400-..."   → parse hex → UUID
 *
 * IMPORTANT — perimetre strict (pas du casting aberrant) :
 *   - Coerce declenche UNIQUEMENT sur declaration-pilote
 *     (`var est un T = expr` ou `var = expr` quand var deja typee T).
 *     JAMAIS implicitement en argument de native.
 *   - Unidirectionnel : `string → Image` mais pas `Image → string`
 *     (pour l'inverse, native explicite : dSauveImagePNG, Trace, etc.).
 *   - Le parser DOIT pass-through si v est deja du bon type, et
 *     retourner NIL_VAL + ErreurDetectee sur conversion invalide.
 *
 * Le compilo emet `__InstanceCoerce(v, "TypeName")` qui dispatch vers
 * le parser enregistre. Un type composite sans parser enregistre N'A
 * PAS de coerce — l'utilisateur doit assigner explicitement le bon
 * type (pas de magie cachee).
 * ================================================================ */

/* Signature d'un parser de coerce. Recoit la valeur source brute,
 * retourne une ObjInstance du type cible (pass-through si deja bon
 * type) ou NIL_VAL + ErreurDetectee si conversion invalide. */
typedef Value (*OwlInstanceCoerceFn)(Value source);

/* Enregistre un parser pour un type composite. typeName doit etre le
 * canon ASCII (ex: "Image", "FichierDisque", "UUID"). Idempotent :
 * un appel ulterieur avec meme typeName overwrite. Appele depuis
 * stdlib_<module>_init() au boot VM. */
void owl_register_instance_coerce(const char* typeName,
                                   OwlInstanceCoerceFn parser);

/* Lookup case-sensitive (typeName doit matcher le canon exact tel
 * qu'enregistre). Retourne NULL si pas de parser. */
OwlInstanceCoerceFn owl_get_instance_coerce(const char* typeName);

/* True si un parser est enregistre pour typeName. Equivalent a
 * (owl_get_instance_coerce(name) != NULL) — utilise par compiler.c
 * pour decider d'emettre __InstanceCoerce au compile-time. */
bool owl_has_instance_coerce(const char* typeName);

/* Iteration (pour self-test, CLI doc, MCP) : liste tous les types
 * composites avec un parser enregistre. */
int owl_instance_coerce_count(void);
const char* owl_instance_coerce_name_at(int index);

/* Enregistre la native dispatcher __InstanceCoerce(v, typeName). A
 * appeler une seule fois depuis stdlib_init() apres que tous les
 * modules stdlib_xxx_init() ont enregistre leurs parsers. */
void owl_register_instance_coerce_native(void);

/*  — Deballe une ObjInstance vers sa propriete par defaut
 * quand son type definit defaultProperty (cf OwlTypeDef). Sinon
 * pass-through `v` inchange. Utilise par :
 *   - les coerces scalaires (__EntierCoerce, __ReelCoerce, ...) quand
 *     un slot scalaire recoit une instance composite ;
 *   - le path arithmetique (OP_ADD/SUB/MUL/...) pour mixer instance +
 *     entier.
 * NULL-safe : retourne v sans toucher si v n'est pas une ObjInstance
 * ou si son type n'a pas de defaultProperty.
 *
 *  — etendu aux ObjUiControlRef via un hook injecte par la VM
 * (owl_set_ui_ref_default_resolver). Un nom de widget DSL nu lu en
 * contexte scalaire (RHS typee, operande arith/compare, coerce de
 * parametre) se resout vers sa propriete par defaut (`SAI` -> `..Valeur`),
 * symetrique de l'ecriture `SAI = X` -> `SAI..Valeur = X`. Le hook vit
 * cote VM (resolution window_stack/etat_stack) ; la registry reste sans
 * dependance VM. Si aucun hook n'est pose, comportement inchange. */
Value owl_instance_default_unwrap(Value v);

/*  — hook VM resolvant un ObjUiControlRef vers la valeur de sa
 * propriete par defaut. Retourne v inchange pour tout ce qui n'est pas un
 * ref de widget a defaultProperty resoluble (ref NULL-prop, hors fenetre
 * ouverte...). Injecte par stdlib_ui_init / vm_init. */
typedef Value (*OwlUiRefDefaultResolver)(Value v);
void owl_set_ui_ref_default_resolver(OwlUiRefDefaultResolver fn);

/* Teardown — called by vm_free. Frees the flat arrays; does NOT free
 * the OwlNativeDef contents (they're static const data). */
void owl_registry_free(void);

#endif
