// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_chunk_h
#define owl_chunk_h

#include "common.h"
#include "value.h"

/* ================================================================
 * OPCODES - Instructions for the VM
 * ================================================================ */

/* Largeur, en octets, d'un operande "index dans le pool de constantes".
 *  : 16 bits SYSTEMATIQUES sur tous les opcodes concernes, pas de
 * variante _LONG. Le pool monte a 65535 entrees ; la taille d'instruction
 * reste fixe par opcode, ce qui garde les tables de tailles et les peepholes
 * tenables. Cf. . */
#define OWL_CONST_IDX_BYTES 2

typedef enum {
    /* --- Constants and literals --- */
    OP_CONSTANT,        /* Load constant from pool (index de constante) */
    OP_NIL,             /* Push Null */
    OP_TRUE,            /* Push Vrai */
    OP_FALSE,           /* Push Faux */

    /* --- Arithmetic --- */
    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_POWER,
    OP_NEGATE,          /* Unary minus */
    OP_INCREMENT,       /* ++ */
    OP_DECREMENT,       /* -- */

    /* --- Comparison --- */
    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,

    /* --- Logical --- */
    OP_NOT,             /* PAS / NOT */
    OP_AND_FULL,        /* a ET b — full-eval booléen. Stack [a, b] -> [a && b].
                         * Coerce comme OP_VERIFY_BOOL_COND : bool / entier /
                         * réel / nil / monétaire / numérique acceptés ; sinon
                         * runtime_error "Opérateur ET/OU appliqué à une
                         * expression non booléenne." */
    OP_OR_FULL,         /* a OU b — full-eval booléen. Stack [a, b] -> [a || b].
                         * Mêmes règles de coercion. */

    /* --- Bitwise (canon : opérateurs binaires) ---
     * Stack: [a, b] -> [a op b] pour les binaires, [a] -> [op a] pour NOT/shifts unaires.
     * Coerce les opérandes vers int64 (entier/réel/booléen). */
    OP_BIT_AND,         /* a & b  (canon : ETBinaire) */
    OP_BIT_OR,          /* a | b  (canon : OUBinaire) */
    OP_BIT_XOR,         /* a || b (canon : OUExclusifBinaire) */
    OP_BIT_NOT,         /* ~a     (canon : NONBinaire) */
    OP_BIT_SHL,         /* a bitDécaleGauche b */
    OP_BIT_SHR,         /* a bitDécaleDroite b */

    /* --- String --- */
    OP_CONCAT,          /* String concatenation (+ on strings) */
    OP_STRING_MATCH,    /* Flexible string match (1-byte operand: opkind|flags)
                         * operand bits[0..2]: 0=LIKE, 1=STARTS, 2=ENDS, 3=CONTAINS
                         * operand bit 3   : case-insensitive
                         * operand bit 4   : accent-insensitive */

    /* --- Variables --- */
    OP_DEFINE_GLOBAL,   /* Define a global variable (1-byte name index) */
    OP_GET_GLOBAL,      /* Read a global variable */
    OP_GET_GLOBAL_CALLEE,/* Same as GET_GLOBAL, but error says "function/procedure" on miss */
    OP_GET_ELEMENT_GLOBAL,/*  : lecture qualifiee `FEN_X.gNom` / `ETAT_X.gCode`
                          * d'une globale scopee a un element UI. NIL-tolerant par
                          * construction (miss => NIL, jamais d'erreur) : l'element
                          * peut etre endormi (init pas encore tourne). 1-byte name
                          * index = la cle mangled `<ELEM>$<var>` dans vm.globals. */
    OP_GET_TYPE_GLOBAL, /*  : lookup d'un nom de type dans vm.types
                         * (separe de vm.globals). Emis par le compilo au site
                         * `est un X` / `__StructClone <name>`. Miss => erreur
                         * "type inconnu". */
    OP_SET_GLOBAL,      /* Set a global variable */
    OP_GET_LOCAL,       /* Read a local variable (1-byte slot index) */
    OP_SET_LOCAL,       /* Set a local variable */

    /* --- Control flow --- */
    OP_JUMP,            /* Unconditional jump (2-byte offset) */
    OP_JUMP_IF_FALSE,   /* Jump if top of stack is falsy */
    OP_LOOP,            /* Jump backwards (for loops) */
    OP_VERIFY_BOOL_COND,/* Vérifie que top-of-stack est une condition booléenne
                         * valide (bool / entier / réel / nil / monétaire /
                         * numérique). Sinon runtime_error. Émis par SI /
                         * TANTQUE / POUR pour aligner canon strict
                         * "expression non booléenne". */

    /* --- Functions --- */
    OP_CALL,            /* Call function (1-byte arg count) */
    OP_RETURN,          /* Return from function */

    /* --- Stack --- */
    OP_POP,             /* Discard top of stack */
    OP_POPN,            /* Discard N values (1-byte count) */
    OP_DUP,             /* Duplicate top of stack */
    OP_SWAP,            /* Swap top two values. Stack [a, b] -> [b, a]. */

    /* --- Arrays --- */
    OP_ARRAY,           /* Create empty array */
    OP_ARRAY_PRESIZE,   /* Create array pre-sized to N (operand: 1 byte count) */
    OP_ARRAY_PRESIZE_ND,/* Create nested N-D array: operand=dimCount (1 byte),
                         * then dimCount bytes for each dimension size. The
                         * outermost dim has `dim[0]` rows, each containing a
                         * sub-array of `dim[1]` rows, etc. Innermost cells=NIL. */
    OP_ARRAY_GROWTH,    /* Set growth mode on top-of-stack array (1 byte operand).
                         * canon <agrandissement=N>: 0=none, 1=grow by 1, 2=grow to N. */
    OP_ARRAY_TYPE_STRUCT,/* Tag top-of-stack array with struct element type
                         * (operand: constant index of struct name string).
                         * Used for `tableau de <Structure>` to enable Ajoute
                         * coercion of [v1,v2,...] into structure instances. */
    OP_ARRAY_FILL_DEFAULT,/* Remplit les cells NIL_VAL de l'array top-of-stack
                          * avec la zero-value du type (operand: 1 byte kind).
                          * Kinds : 1=chaine "" / 2=entier 0 / 3=reel 0.0 /
                          * 4=booleen Faux / 5=monetaire / 6=numerique.
                          * Émis après OP_ARRAY_PRESIZE pour `tableau de N <type>`
                          * (canon : cells = valeur par défaut). */
    OP_ARRAY_FILL_STRUCT,/* Pendant de OP_ARRAY_FILL_DEFAULT pour un type NOMMÉ
                          * (structure utilisateur ou type objet du registry) :
                          * remplit les cells NIL_VAL de l'array top-of-stack avec
                          * N instances DISTINCTES clonées du template.
                          * Operand: index de constante du nom du type.
                          * Template cherché dans vm.types puis vm.globals ;
                          * absent ou non-instance -> no-op (le tableau garde ses
                          * cells Null, comportement d'avant). */
    OP_ARRAY_MARK_GROUP, /*  — tag top-of-stack ObjArray comme Groupe
                          * (is_group = true). Emis apres l'init d'un
                          * `GR_X est un Groupe [= [...]]`. Active le dispatch
                          * broadcast specialise dans OP_STRUCT_GET/SET. */
    OP_STRUCT_FIELD_ORDER,/* Append a field name to top-of-stack ObjInstance's
                           * fieldOrder (operand: constant index of field name).
                           * Emitted during struct_declaration. Does not pop. */
    OP_ARRAY_GET,       /* Get element by index (base 1) */
    OP_TABLE_CELL_DESC, /*  — `TABLE_X.COL_Y[ligne]` suivi d'une
                         * propriete de mise en forme. Meme pile que
                         * OP_ARRAY_GET, mais rend le DESCRIPTEUR de cellule au
                         * lieu de sa valeur. La discrimination est purement
                         * syntaxique (ce qui suit le `]`), donc compile-time :
                         * a l'execution il est trop tard, la valeur est lue. */
    OP_ARRAY_SET,       /* Set element by index (base 1) */
    OP_ARRAY_GET_2D,    /* Get with 2 indices (Stack: [recv][x][y] -> [val]).
                         * Dispatch runtime : ObjInstance Image -> pixel ;
                         * sinon comportement legacy = recv[x][y]. Doc :
                         * Multimedia/Images/1000019650 §"Accès aux pixels". */
    OP_ARRAY_SET_2D,    /* Set with 2 indices (Stack: [recv][x][y][val] -> []).
                         * Symétrique de OP_ARRAY_GET_2D. */
    OP_ARRAY_LENGTH,    /* Get .Occurrence property */
    OP_ITER_KEY,        /* For iteration: pop (iterable, 1-based counter) ->
                         * push counter (arrays/collections) or assoc key string */

    /* --- Structures --- */
    OP_STRUCT_GET,      /* Get field by name */
    OP_STRUCT_SET,      /* Set field by name */

    /* --- Exceptions --- */
    OP_TRY,             /* Push exception handler (2-byte jump offset) */
    OP_END_TRY,         /* Pop exception handler */
    OP_THROW,           /* Throw exception (pops message from stack) */
    OP_RERAISE,         /* Re-raise the current exception (uses currentException) */
    OP_EXC_RESTORE,     /* Restore the enclosing exception context on normal
                         * handler exit (pops the save stack — cf. vm.c) */

    /* --- OOP --- */
    OP_CLASS,           /* Create a new ObjClass */
    OP_METHOD,          /* Add method to a class */
    OP_INVOKE,          /* Optimized method call */
    OP_INHERIT,         /* Set up inheritance */
    OP_GET_THIS,        /* Push slot 0 (this reference) */
    OP_MOIMEME,         /*  — push widget emetteur courant. Lit la
                         * stack runtime des contextes (event_sender_stack puis
                         * etat_stack puis window_stack). Stack vide => soft
                         * error E_RT_MOIMEME_NO_WIDGET, push NIL. */
    OP_MONCHAMP,        /*  — push le champ courant, STRICT (sans repli).
                         * Lit event_sender_top() : seul un champ (ObjUiControlRef)
                         * est accepte ; une fenetre ou stack vide => soft error
                         * E_RT_MONCHAMP_NO_CONTROL, push ref null. */
    OP_MAFENETRE,       /*  — push la fenetre courante. Resout via
                         * vm_window_stack_top() (proxy ObjWindowRef global).
                         * Aucune fenetre ouverte => soft error
                         * E_RT_MAFENETRE_NO_WINDOW, push ref null. */
    OP_SET_FIELD_THIS,  /* Set field on this: :field = value */

    /* --- Enumerations --- */
    OP_ENUM_VALUE,      /* Store value in class defaults: stack [class] [value], operand: name */
    OP_STATIC_FIELD,    /* : store value in class staticFields (GLOBAL member).
                         * Stack: [class] [value] (class stays). Operand: name const idx. */
    OP_MARK_CLASS_KIND, /* Tag the ObjClass on top of stack with a ClassKind. operand: 0=class, 1=enum, 2=combinaison */

    /* --- Collections --- */
    OP_COLLECTION,      /* Create a new collection: operand = CollectionType (0=Queue,1=Stack,2=List) */

    /* --- Multiple return --- */
    OP_ARRAY_PACK,      /* Pop N values from stack, create array (operand = N) */
    OP_ARRAY_UNPACK,    /* Pop array, push N elements onto stack (operand = N) */

    /* --- Out-params variadic (cf VISION §5bis) ---
     * Émis par le compilateur APRÈS un OP_CALL d'une native ayant
     * OWL_NAT_OUT_PARAMS. Consomme une valeur du buffer
     * vm.outParamReturns[idx] et l'écrit dans la variable lvalue du
     * caller. Le compilateur garantit que les indices sont émis dans
     * l'ordre 0..N-1 et que la dest est bien une lvalue (validé au
     * compile-time). */
    OP_OUT_PARAM_TO_LOCAL,   /* operand: slot (1 byte), idx (1 byte) */
    OP_OUT_PARAM_TO_GLOBAL,  /* operand: nameConst (1 byte), idx (1 byte) */

    /* --- Itération POUR TOUT (track ÉlémentCourant pour Supprime) ---
     * OP_ITER_BEGIN     : Stack [iterable] -> [iterable]. Push iterable
     *                     dans vm.iterTarget, NIL dans vm.iterCurrentKey.
     * OP_ITER_TRACK_KEY : Stack [key] -> [key]. Set vm.iterCurrentKey[top].
     * OP_ITER_END       : Pop le contexte. */
    OP_ITER_BEGIN,
    OP_ITER_TRACK_KEY,
    OP_ITER_END,

    /* --- Destructor --- */
    OP_INVOKE_SAFE,     /* Like OP_INVOKE but silently skips if method doesn't exist */

    /* --- Built-in operations --- */
    OP_PRINT,           /* Trace() / Info() */

    /* --- Deep copy (<= operator) --- */
    OP_DEEP_COPY,       /* Replace top-of-stack with a deep clone */

    /* --- <indétectable> attribute conversion --- */
    OP_TO_UNDETECTABLE, /* Replace top-of-stack with an undetectable variant.
                         * No-op for non-string values or already-undetectable
                         * strings. Emitted after assigning to a variable
                         * declared `chaîne <indétectable>`. */

    /* --- String/buffer slicing (s[[i]] / s[[i À j]] / s[[i À]] / s[[i SUR n]]) --- */
    OP_STRING_CHAR_AT,       /* Stack [s, i]    -> 1-char substring (base 1) ;
                              * sur Buffer -> entier 0-255 (octet brut). */
    OP_BUFFER_BYTE_SET,      /* Stack [buf, i, v] -> v. Ecrit 1 octet (base 1)
                              * strict [0,255] sur Buffer ; no-op sur chaine. */
    OP_STRING_SLICE,         /* Stack [s, i, j] -> substring i À j inclusive (base 1) */
    OP_STRING_SLICE_TO_END,  /* Stack [s, i]    -> substring i À Taille(s) (base 1) */
    OP_STRING_SLICE_LEN,     /* Stack [s, i, n] -> n chars depuis i (1-based) */

    /* --- Indirection ({expr}, obj.{expr}) --- */
    OP_INDIRECT_GLOBAL_GET, /* Stack [nameStr] -> value (Null if missing) */
    OP_INDIRECT_GLOBAL_SET, /* Stack [value, nameStr] -> leaves value on stack */
    OP_INDIRECT_FIELD_GET,  /* Stack [instance, nameStr] -> value */
    OP_INDIRECT_FIELD_SET,  /* Stack [instance, nameStr, value] -> leaves value */

    /* --- Null-safe operators --- */
    OP_JUMP_IF_NIL,         /* Peek top of stack; if NIL, jump (2-byte offset).
                             * Leaves the value on the stack unchanged either way. */
    OP_JUMP_IF_NOT_NIL,     /* Same but jumps on non-NIL. Used for coalesce. */

    /* --- Procedure chain Avant/Après hooks (p.Avant / p.Après) ---
     * Stack: [receiver, value] -> [updated_chain]. Operand byte: 0=Avant, 1=Apres.
     * Converts a plain function to a chain if needed. */
    OP_PROC_SET_HOOK,

    /* --- String builder optimization ---
     * Compile-time peephole replacement for the common pattern
     *   s = s + expr     (or)    s += expr
     * where s is a local string variable. Operand: 1-byte slot index.
     * Stack: [..., rhs] -> [..., new_string]. Side effect: updates locals[slot]
     * in place when possible (mutable buffer, amortized growth) for O(n)
     * total cost instead of O(n²). Falls back to standard concat on type
     * mismatch (e.g. integers). */
    OP_APPEND_IN_PLACE,
    /* Same idea for global variables. Operand: 1-byte constant index for
     * the variable name (same encoding as OP_GET_GLOBAL/OP_SET_GLOBAL). */
    OP_APPEND_IN_PLACE_GLOBAL,

    /* --- POO super-call (Ancetre.Method()) —  ---
     * Stack: [this] [arg1] ... [argN] -> [result]. Operands: methodName
     * constant index, argCount. Lookup demarre dans
     * instance->klass->superclass (saute la classe courante, force la
     * version parente). */
    OP_INVOKE_SUPER,

    /* --- POO super-call by-name (cAncestor.Method()) —  ---
     * Stack: [this] [arg1] ... [argN] -> [result]. Operands: className
     * constant index, methodName constant index, argCount.
     * Lookup direct dans targetClass->methods sans walk virtual : permet
     * de cibler un ancetre transitif specifique (skip d'eventuels niveaux
     * intermediaires). Dispatch statique resolu compile-time — aucune
     * recursion possible meme si la cible et le sender partagent le nom
     * de methode. Emis par dot_access quand le receveur est un nom de
     * classe ancetre strict de currentClass. */
    OP_INVOKE_DIRECT,

    /* --- POO operateur d'introspection (obj est cClasse) —  ---
     * Stack: [obj] -> [bool]. Operand: classNameStr constant index.
     * Renvoie Vrai si obj est instance d'une classe egale ou derivee de
     * classNameStr (walk superclass chain). Renvoie Faux pour non-instance
     * ou classe inconnue. */
    OP_IS_CLASS,

    /* --- POO declaration d'interface implementee ---
     * Stack: [class] -> [class] (peek, pas de pop). Operand: ifaceName
     * constant index. Inscrit le nom d'interface dans klass->interfaces.
     * Emis par class_declaration pour chaque ligne `implemente iX`, apres
     * que la classe ait ete posee/repoussee sur la pile (avant le OP_POP
     * final qui clot la declaration). Consomme par OP_IS_CLASS qui
     * accepte un nom de classe OU d'interface a droite de `est`. */
    OP_CLASS_DECLARE_INTERFACE,

    /* --- Auto-vivification de chemins JSON profonds en ECRITURE ---
     * Variantes "get-or-create" d'OP_STRUCT_GET / OP_ARRAY_GET, emises UNIQUEMENT
     * pour les segments INTERMEDIAIRES d'une cible d'affectation (LVALUE) chainee
     * (`j.a.b = x`, `j.a[1].b = x`). Meme forme/largeur que l'opcode de lecture
     * dont elles derivent (promotion in-place d'un octet par le compilateur) :
     *   - _OBJ : le segment suivant est un acces membre -> materialise un objet
     *     JSON si le conteneur est absent ;
     *   - _ARR : le segment suivant est un acces indexe -> materialise un tableau
     *     JSON si le conteneur est absent.
     * Hors conteneur JSON absent, comportement STRICTEMENT identique a l'opcode
     * de lecture d'origine (la lecture ne materialise jamais rien). */
    OP_STRUCT_GET_OR_CREATE_OBJ,   /* comme OP_STRUCT_GET (SHAPE 1,0) */
    OP_STRUCT_GET_OR_CREATE_ARR,
    OP_ARRAY_GET_OR_CREATE_OBJ,    /* comme OP_ARRAY_GET (SHAPE 0,0) */
    OP_ARRAY_GET_OR_CREATE_ARR,

    /* Sentinelle. N'ajoutez PAS d'opcode aprés OP_COUNT.
     * Sert au _Static_assert de coherence dispatch ↔ enum et au
     * range-init `[0 ... OP_COUNT-1] = &&L_BAD_OP` de la table de
     * dispatch computed-goto dans vm.c. */
    OP_COUNT
} OpCode;

/* ================================================================
 * CHUNK - A sequence of bytecode + associated data
 * ================================================================ */

typedef struct Chunk {
    int count;
    int capacity;
    uint8_t* code;      /* bytecode array */
    int* lines;         /* line number for each byte (for error reporting) */
    ValueArray constants; /* constant pool */
} Chunk;

/* ================================================================
 * FORME DES OPCODES — table canonique unique
 *
 * Une seule description par opcode alimente TOUT ce qui doit connaitre la
 * largeur d'une instruction : l'emission (compiler.c), le calcul de taille
 * (chunk_opcode_size), les peepholes, le desassembleur, les self-tests.
 * Avant  la meme information vivait recopiee dans quatre switch
 * distincts, qui divergeaient.
 * ================================================================ */

typedef struct {
    uint8_t constIdx;    /* nombre d'operandes "index dans le pool" */
    uint8_t raw;         /* nombre d'octets d'operandes bruts (slot, argCount…) */
    uint8_t varRaw;      /* 1 = taille variable, lue dans le bytecode (PRESIZE_ND) */
} OpcodeShape;

/* Renvoie false pour un opcode inconnu (slice probablement corrompu). */
bool chunk_opcode_shape(uint8_t op, OpcodeShape* out);

/* Taille en octets d'une instruction complete (opcode + operandes).
 * -1 pour un opcode inconnu ou un slice tronque. `offset` designe la position
 * de l'opcode dans `chunk` (necessaire aux opcodes a taille variable). */
int chunk_opcode_size(uint8_t op, const Chunk* chunk, int offset);

/* Vrai si l'opcode porte au moins un index de constante en tete d'operandes. */
bool chunk_opcode_has_const_index(uint8_t op);

/* Marche le chunk instruction par instruction et verifie qu'on retombe
 * exactement sur `count`. Un desaccord entre la largeur emise et la table
 * canonique decale la marche et se voit ici — c'est le filet mecanique du
 * chantier . Renvoie true si coherent. */
bool chunk_walk_is_consistent(const Chunk* chunk);

/* Position de debut de la DERNIERE instruction du chunk, obtenue en marchant
 * vers l'avant depuis `from`, qui doit etre une frontiere d'instruction connue
 * (0, ou une position relevee au moment ou l'on savait etre entre deux
 * instructions).
 *
 * Renvoie -1 si le chunk est vide a partir de `from`, si un opcode inconnu
 * arrete la marche, ou si la marche ne retombe pas EXACTEMENT sur `count` —
 * ce dernier cas signant une frontiere de depart fausse. Un appelant qui veut
 * savoir ce qui vient d'etre emis ne doit jamais reculer d'une largeur
 * supposee depuis la fin : les operandes d'index de constante ont un octet de
 * poids fort nul, egal a OP_CONSTANT, et se laissent lire comme un debut
 * d'instruction. */
int chunk_last_instruction_offset(const Chunk* chunk, int from);

void chunk_init(Chunk* chunk);
void chunk_write(Chunk* chunk, uint8_t byte, int line);

/* Insert `n` bytes at position `pos`, shifting existing bytes right.
 * Used by the compiler to retroactively wrap an already-emitted RHS
 * (e.g. inserting a coercion call prefix only when a peephole
 * optimisation has failed). Grows capacity as needed. All inserted
 * bytes share the same source line number. */
void chunk_insert_before(Chunk* chunk, int pos, const uint8_t* bytes,
                         int n, int line);

/* Add a constant to the pool, return its index */
int chunk_add_constant(Chunk* chunk, Value value);

/* Détecte la présence d'un opcode de contrôle de flux (OP_JUMP*, OP_LOOP,
 * OP_JUMP_IF_NIL, OP_JUMP_IF_NOT_NIL) dans la fenêtre [offset..offset+len[.
 * Renvoie true au premier match. Utilisé par POUR `A` / `PAS` re-eval pour
 * rejeter les bornes complexes que la stratégie memcpy ne peut pas
 * relocaliser proprement (offsets internes deviendraient invalides). */
bool chunk_slice_has_jumps(const Chunk* chunk, int offset, int len);

#endif
