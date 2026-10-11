// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_object_h
#define owl_object_h

#include "common.h"
#include "value.h"

/* ================================================================
 * OBJECT TYPES
 * Every heap-allocated OWL value is an Obj with a type tag.
 * ================================================================ */

typedef enum {
    OBJ_STRING,
    OBJ_BUFFER,
    OBJ_NUMERIC,        /* modele de nombre v2 : bignum decimal (mpdecimal) */
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_ARRAY,
    OBJ_ASSOC_ARRAY,
    OBJ_INSTANCE,
    OBJ_CLASS,
    OBJ_COLLECTION,
    OBJ_JSON,
    OBJ_DATE,
    OBJ_TIME,
    OBJ_DATETIME,
    OBJ_DURATION,
    OBJ_PROC_CHAIN,
    OBJ_ZIP_ARCHIVE,
    OBJ_MOCK_NATIVE,
    OBJ_PATTERN,
    OBJ_UI_CONTROL,     /* DSL Fenetre v1 : wrapper d'un widget Qt (Phase E.X) */
    OBJ_MULTILINGUE,    /*  lot B : chaineMultilingue first-class */
    OBJ_WINDOW_INSTANCE,    /*  : instance runtime d'une fenetre (multi-instance) */
    OBJ_UI_CONTROL_INSTANCE,/*  : instance runtime d'un sous-controle */
    OBJ_UI_CONTROL_REF,     /*  : proxy global vers un OwlControlDecl (resolution dynamique) */
    OBJ_WINDOW_REF,         /*  lot B : proxy global d'une fenetre (resolution dynamique vers l'instance top-of-stack) */
    OBJ_ETAT_INSTANCE,      /*  lot D : instance runtime d'un Etat (Etats vivants) */
    OBJ_ICONE,              /*  : Icone first-class (catalogue zstd + SVG custom) */
    OBJ_ICONES_NAMESPACE,   /*  Lot 2e-1 : singleton expose comme `icones` / `icons` (catalogue lookup) */
    OBJ_THEME_NAMESPACE,    /*  Lot 2b+3 : singleton expose comme `theme` / `Theme` (resolution styles nommes) */
    OBJ_VECTOR,             /*  : type Vecteur/Vector — double[] contigu, element = reel IEEE-754 */
    OBJ_MATRIX,             /*  : type Matrice/Matrix — double[] row-major contigu, element = reel IEEE-754 */
    OBJ_MATRIX_ROW,         /*  : vue transitoire d'une ligne de Matrice (m[i]) pour chainer [j], lecture/ecriture */
    OBJ_TENSOR,             /*  : type Tenseur/Tensor — donnee + gradient + noeud de graphe (autograd) */
    OBJ_TENSOR_VIEW,        /*  : vue transitoire d'une sous-tranche de Tenseur N-D (t..Valeur[i]...) pour chainer les crochets */
    OBJ_IAMODELE,           /*  : type IAModèle/AIModel — pile ordonnee de couches (dense + activations) */
    OBJ_OPTIONS_ENTRAINEMENT,/*  : type OptionsEntraînement/TrainingOptions — config d'un entrainement (feuille, champs scalaires) */
    OBJ_LLM,                /*  : type LLM — modele GGUF charge (handles llama C-heap, finalizer GC) */
    OBJ_OPTIONS_GENERATION, /*  : type OptionsGénération/GenerationOptions — reglages d'un appel de generation (feuille + callback SurToken) */
    OBJ_MESSAGE_LLM,        /*  : type MessageLLM/LLMMessage — un message de conversation (rôle + contenu), entree de .Chat */
    OBJ_RESPONSE_LLM,       /*  : type RéponseLLM/LLMResponse — sortie de .Chat (contenu + raison d'arret + compteurs de jetons) */
    OBJ_IMAGE_ETIQUETEE,    /*  : type ImageÉtiquetée/LabeledImage — chemin + etiquette + Image materialisee paresseusement */
    OBJ_IMAGE_ANNOTEE,      /*  : type ImageAnnotée/AnnotatedImage — chemin + boites (tableau) + Image materialisee paresseusement (exemple de detection) */
    OBJ_DATASET,            /*  : type Dataset de ImageÉtiquetée (+ ImageAnnotée ) — collection d'exemples (lots, melange), generique sur le type d'exemple */
    OBJ_XLS_DOCUMENT,       /*  : type xlsDocument — classeur Excel (.xlsx) en memoire (feuilles + cellules + fusions), C-heap pur, feuille GC */
    OBJ_XLS_REF,            /*  : reference indexee de style (cellule/ligne/colonne) dans un xlsDocument — enveloppe GC (doc + kind + coords) */
    OBJ_DETECTION,          /*  : type Détection/Detection — une detection d'objet (boite + classe + score), sortie de IAModèle.Détecte */
    OBJ_MLARBRE,            /*  : type MLArbreDécision/MLDecisionTree — arbre de décision CART (arbre interne C-heap pur, feuille GC) */
    OBJ_MLFORET,            /*  : type MLForêtAléatoire/MLRandomForest — ensemble d'arbres CART (bagging + variables aléatoires), tableau d'arbres C-heap purs, feuille GC */
    OBJ_MLBOOSTING,         /*  : type MLGradientBoosting — ensemble séquentiel d'arbres CART de régression (boosting de gradient), tableau d'arbres C-heap purs, feuille GC */
    OBJ_MLLINEAIRE,         /*  : type MLRégressionLinéaire — moindres carrés ordinaires (forme close), poids C-heap purs, feuille GC */
    OBJ_MLLOGISTIQUE,       /*  : type MLRégressionLogistique — régression softmax par descente de gradient, poids C-heap purs, feuille GC */
    OBJ_MLKMEANS,           /*  : type MLKMeans — clustering non-supervisé (Lloyd + k-means++), centroïdes C-heap purs, feuille GC */
} ObjType;

/* Base object header - every heap object starts with this */
struct Obj {
    ObjType type;
    bool isMarked;      /* for GC mark phase */
    struct Obj* next;   /* linked list of ALL objects (for GC sweep) */
};

/* ================================================================
 * STRING OBJECT
 * ================================================================ */

struct ObjString {
    Obj obj;            /* "inherits" from Obj */
    int length;         /* logical length in bytes (excluding terminator) */
    int capacity;       /* allocated bytes in `chars` (>= length+1 for the \0) */
    char* chars;
    uint32_t hash;      /* cached hash for fast table lookups (0 = invalid, recompute) */
    bool interned;      /* true = immutable + present in owl_strings table.
                         * false = mutable buffer (string-builder mode) used by
                         * the OP_APPEND_IN_PLACE optimization. Buffers must
                         * never be used as table keys (their hash is invalid
                         * after each append). */
    bool undetectable;  /* true = chars is XOR-masked with the per-process key.
                         * Bypasses interning. Scrubbed on free. Propagates
                         * through concat/substring/copy. See docs/SECURITY.md
                         * "indétectable attribute" for the threat model. */
    /* UTF-8 codepoint optimization caches (lazy, invalidated on mutation).
     * Used by Milieu/Gauche/Droite/Taille to avoid O(n) walks per call.
     * For ASCII strings (the common case), all lookups become O(1). For
     * multibyte strings, an incremental loop pattern is O(n) total instead
     * of O(n²) thanks to the last-position cache. */
    unsigned char ascii_state;  /* 0=unknown, 1=pure ASCII, 2=multibyte */
    int cp_count;               /* -1 = not yet computed; codepoint count otherwise */
    int last_cp_index;          /* -1 = no cached position */
    int last_byte_offset;       /* byte offset matching last_cp_index */
    /* Frozen snapshot of a buffer-mode string. Populated lazily by
     * OP_GET_GLOBAL/OP_GET_LOCAL on first read of an `interned=false`
     * string. Subsequent reads return the cached pointer instead of
     * re-copying the (potentially huge) buffer each time. Invalidated
     * to NULL by every mutation (OP_APPEND_IN_PLACE / _GLOBAL). Marked
     * as a GC child of the buffer ObjString. */
    struct ObjString* frozen_cache;
};

/* ================================================================
 * NUMERIC OBJECT (modele de nombre v2 — bignum decimal a precision
 * arbitraire, adosse a mpdecimal). Le type `numerique` ne tient plus
 * inline dans Value : il devient un Obj heap-manage par le GC. `dec`
 * pointe vers un mpd_t alloue sur malloc/free standard (PAS le GC) ;
 * le sweep le libere via mpd_del (cf object_free). Feuille GC : aucune
 * reference OWL sortante. Cf  (cahier des charges modele v2).
 * `struct mpd_t` est forward-declare ici pour ne pas tirer mpdecimal.h
 * dans tous les TU ; les .c qui manipulent dec incluent mpdecimal.h.
 * ================================================================ */
struct mpd_t;

typedef struct ObjNumeric {
    Obj obj;
    struct mpd_t* dec;
} ObjNumeric;

/* ================================================================
 * BUFFER OBJECT (Buffer — binary-safe byte array)
 * ================================================================ */

typedef struct {
    Obj obj;
    int length;
    unsigned char* bytes;   /* length bytes, NOT null-terminated */
    int capacity;
} ObjBuffer;

/* ================================================================
 * VECTOR OBJECT ( — type Vecteur/Vector)
 *
 * Collection homogene de reels IEEE-754 stockes dans un tableau C
 * contigu (double[]). Un element se comporte octet-pour-octet comme
 * un scalaire `reel` : aucune 5e semantique de nombre. Le tableau
 * `data` est alloue via malloc/free standard (PAS le GC) — feuille GC
 * comme ObjBuffer (aucune reference OWL sortante). `count` = nombre
 * d'elements, `capacity` = doubles alloues (== count en T1, taille
 * figee a la construction).
 * ================================================================ */

typedef struct {
    Obj obj;
    int count;
    int capacity;
    double* data;   /* count doubles contigus, malloc'd (pas GC) */
} ObjVector;

/* ================================================================
 * MATRIX OBJECT ( — type Matrice/Matrix)
 *
 * Grille rectangulaire de reels IEEE-754 stockes dans un tableau C
 * contigu row-major (`data[(row-1)*cols + (col-1)]`). Pendant 2D du
 * Vecteur : un element se comporte octet-pour-octet comme un scalaire
 * `reel`. `data` est malloc'd (PAS le GC) — feuille GC comme ObjVector.
 * Invariant : rows >= 1 ET cols >= 1 (une Matrice n'est jamais vide).
 * ================================================================ */

typedef struct {
    Obj obj;
    int rows;
    int cols;
    double* data;   /* rows*cols doubles contigus row-major, malloc'd (pas GC) */
} ObjMatrix;

/* Vue transitoire d'une ligne de Matrice, produite par `m[i]` pour que
 * `m[i][j]` chaine une seconde indexation en lecture ET en ecriture
 * (l'ecriture atteint le stockage de la matrice via `mat`). N'est jamais
 * stockee par l'utilisateur : elle ne vit que sur la pile VM entre les
 * deux crochets. NON-feuille GC : reference `mat`, tracee par le GC. */
typedef struct {
    Obj obj;
    ObjMatrix* mat;   /* matrice source (marquee par le GC) */
    int row;          /* index de ligne 1-based, valide 1..mat->rows */
} ObjMatrixRow;

/* ================================================================
 * TENSOR OBJECT ( — type Tenseur/Tensor, autograd reverse-mode)
 *
 * Enveloppe differentiable au-dessus d'un Vecteur : elle porte la donnee
 * (un Vecteur), le gradient (un Vecteur de meme forme, initialise a zero)
 * et un noeud de graphe de calcul — l'operation qui l'a produite et ses
 * Tenseurs parents. C'est le PREMIER Obj OWL a references internes
 * non-triviales : NON-feuille GC. `data`, `grad` et chaque parent doivent
 * etre traces par le GC (cf gc.c trace_references), sinon un parent
 * collecte pendant que le graphe vit provoque un use-after-free.
 * ================================================================ */

typedef enum {
    TENSOR_OP_LEAF = 0,   /* feuille : pas d'op productrice (constante / entree) */
    TENSOR_OP_ADD,        /* c = a + b (element-par-element) */
    TENSOR_OP_MUL,        /* c = a * b (element-par-element) */
    TENSOR_OP_MATMUL,     /* c = a . b (produit scalaire vecteur.vecteur -> scalaire) */
    TENSOR_OP_MATVEC,     /* c = W @ x (matrice m×n . vecteur n -> vecteur m ; parents[0] 2D, parents[1] 1D) */
    TENSOR_OP_MATMUL2D,   /* c = A @ B (matrice p×q . matrice q×r -> matrice p×r ; parents 2D ; forward = kernel rapide owl_matmul) */
    TENSOR_OP_RELU,       /* c = max(0, a) (element-par-element, 1 parent) */
    TENSOR_OP_SIGMOID,    /* c = 1/(1+e^-a) (element-par-element, 1 parent) */
    TENSOR_OP_TANH,       /* c = tanh(a) (element-par-element, 1 parent) */
    TENSOR_OP_MSE,        /* c = moyenne((pred-cible)^2) -> scalaire (2 parents ; seul pred recoit un gradient) */
    TENSOR_OP_CROSS_ENTROPY, /* c = -Somme(cible*log(softmax(pred))) -> scalaire (softmax fusionnee ; 2 parents ; seul pred recoit un gradient) */
    TENSOR_OP_RESHAPE,    /*  : c = a redimensionne (1 parent ; ordre plat preserve, backward = copie plate du gradient) */
    TENSOR_OP_PERMUTE,    /*  : c = a avec axes reordonnes (1 parent ; backward reordonne le gradient par la permutation inverse). Transpose(axe1,axe2) est le cas d'une permutation qui echange deux axes */
    TENSOR_OP_SUM_AXIS,   /*  : c = somme de a le long d'un axe (1 parent ; rang-1 ; backward rediffuse le gradient sur l'axe reduit) */
    TENSOR_OP_MEAN_AXIS,  /*  : c = moyenne de a le long d'un axe (1 parent ; rang-1 ; backward rediffuse gradient/taille_axe) */
    TENSOR_OP_CONV2D,     /*  : c = conv2D(entree B×Cin×H×W, poids Cout×Cin×kH×kW) -> B×Cout×H'×W' (correlation croisee ; 2 parents ; backward dPoids + dEntree par scatter) */
    TENSOR_OP_MAXPOOL,    /*  : c = max-pooling non chevauchant (1 parent ; backward route le gradient vers l'argmax de chaque fenetre, recalcule depuis la donnee du parent) */
    TENSOR_OP_AVGPOOL,    /*  : c = average-pooling non chevauchant (1 parent ; backward repartit le gradient /taille² sur chaque cellule de la fenetre) */
    TENSOR_OP_BATCHNORM,  /*  : c = normalisation par canal (x-moyenne)/racine(var+eps) sur B,H,W (1 parent ; backward = formule BN standard, moyenne/variance recalculees depuis la donnee du parent). Le scale/shift gamma/beta est cable par des ops MUL/ADD separees en amont. */
    TENSOR_OP_CONCAT_CH,  /*  : c = concatenation de a et b sur l'axe canal (index 1 de [N,C,...]) -> [N, Ca+Cb, ...] (2 parents ; memes axes sauf l'axe 1 ; backward = split du gradient par tranche de canaux, [0:Ca] vers a, [Ca:Ca+Cb] vers b) */
    TENSOR_OP_SLICE_CH,   /*  : c = tranche de canaux [op_axis:op_axis+C] de a sur l'axe 1 (inverse du CONCAT_CH ; 1 parent ; op_axis = canal de depart ; backward = scatter du gradient dans la tranche source, 0 ailleurs). Sert le nœud Split de l'import ONNX (DAG a fan-out). */
    TENSOR_OP_BCE_LOGITS, /*  : c = moyenne(y*softplus(-x) + (1-y)*softplus(x)) -> scalaire (sigmoide fusionnee ; 2 parents ; seul le receveur/logits recoit un gradient (sigmoide(x)-y)/N) */
    TENSOR_OP_L1,         /*  : c = moyenne(|pred - cible|) -> scalaire (2 parents ; seul le receveur/pred recoit un gradient signe(pred-cible)/N) */
    TENSOR_OP_SELECT,     /*  : c = sous-Tenseur mat[i] = tranche `i` (base 1) le long de l'axe 0, rang N-1 (1 parent ; op_axis = index 0-based selectionne ; backward = scatter du gradient dans la tranche `i` du parent, 0 ailleurs, motif SLICE_CH). Distinct de l'acces brut ..Valeur[i] (vue transitoire ObjTensorView). */
} TensorOp;

/*  — rang maximum d'un Tenseur N-D. Les tableaux shape/strides/perm
 * sont inline dans l'enveloppe (pas de malloc a liberer) : ce cap borne leur
 * taille. 8 axes couvrent largement le batch×canaux×H×W (4D) vise et au-dela. */
#define OWL_TENSOR_MAX_RANK 8

typedef struct ObjTensor {
    Obj obj;
    Obj* data;                   /* la donnee, stockage plat contigu row-major : ObjVector (rang 1 ou >= 3) OU ObjMatrix (rang 2), marquee par le GC */
    Obj* grad;                   /* le gradient (meme forme que data, zero-init), marque par le GC */
    /*  — metadonnees de forme N-D. Le stockage est TOUJOURS un buffer
     * plat contigu row-major (via ObjVector, sauf rang 2 qui reste un ObjMatrix
     * pour conserver les kernels rapides MATVEC/MATMUL2D et rendre une Matrice
     * en ..Valeur). ndim + shape decrivent la vue N-D par-dessus ce buffer ; les
     * strides row-major se deduisent de shape (donnee contigue) et sont calcules
     * a la volee. Ces tableaux sont inline (pas de free au sweep). */
    int ndim;                    /* nombre d'axes (>= 1) */
    int shape[OWL_TENSOR_MAX_RANK];   /* taille de chaque axe */
    int op_axis;                 /* SUM_AXIS/MEAN_AXIS : axe reduit (0-based) */
    int op_perm[OWL_TENSOR_MAX_RANK]; /* PERMUTE : permutation appliquee (op_perm[i] = axe source de l'axe i de sortie) */
    /*  — hyperparametres des ops spatiales, inline (pas de malloc).
     * Selon op :
     *   CONV2D          : conv_stride = pas, conv_pad = remplissage (kH/kW
     *                     deduits des poids parents[1])
     *   MAXPOOL/AVGPOOL : conv_stride = taille de fenetre, conv_pad = pas entre
     *                     deux fenetres (defaut = fenetre -> non chevauchant ;
     *                     pas < fenetre -> fenetres chevauchantes)
     *   BATCHNORM       : bn_eps = epsilon ajoute a la variance avant la racine */
    int conv_stride;
    int conv_pad;
    double bn_eps;
    TensorOp op;                 /* op qui a produit ce Tenseur */
    struct ObjTensor* parents[2];/* Tenseurs parents (max 2), marques par le GC */
    int nParents;                /* 0 (feuille) ou 1/2 (op unaire/binaire) */
    bool visited;                /* marqueur transitoire du tri topologique (Retropropage), remis a faux en fin de passe */
} ObjTensor;

/*  — vue transitoire d'une sous-tranche d'un Tenseur de rang >= 3,
 * produite par `t..Valeur[i]` (ou `..Gradient[i]`) pour chainer les crochets
 * jusqu'a l'element scalaire. Chaque `[i]` supplementaire descend d'un axe :
 * une vue de rang k rend une vue de rang k-1, une vue de rang 1 rend le reel.
 * NON-feuille GC : reference `backing` (l'ObjVector/ObjMatrix qui possede le
 * double* — marque par le GC). `offset` est l'index element de depart dans ce
 * buffer plat, `strides` sont en elements. Jamais stockee par l'utilisateur :
 * ne vit que sur la pile VM entre deux crochets. */
typedef struct {
    Obj obj;
    Obj* backing;   /* ObjVector/ObjMatrix proprietaire du buffer plat (marque par le GC) */
    int  offset;    /* index element de depart dans backing */
    int  ndim;      /* axes restants a indexer */
    int  shape[OWL_TENSOR_MAX_RANK];
    int  strides[OWL_TENSOR_MAX_RANK];  /* pas en elements pour chaque axe */
} ObjTensorView;

/* Un Tenseur 1D stocke sa donnee/gradient dans un ObjVector, un Tenseur 2D dans
 * un ObjMatrix. Ces deux stockages exposent un buffer `double*` plat contigu et
 * un nombre total d'elements : ces helpers unifient l'acces element-par-element
 * (ops elementwise, reset des gradients, etc.) sans dupliquer le branchement. */
static inline int tensor_numel(const Obj* o) {
    return o->type == OBJ_MATRIX
        ? ((const ObjMatrix*)o)->rows * ((const ObjMatrix*)o)->cols
        : ((const ObjVector*)o)->count;
}
static inline double* tensor_cells(Obj* o) {
    return o->type == OBJ_MATRIX ? ((ObjMatrix*)o)->data : ((ObjVector*)o)->data;
}
static inline bool tensor_is_2d(const ObjTensor* t) {
    return t->data->type == OBJ_MATRIX;
}

/*  — broadcasting N-D (regles numpy/torch). Deux formes s'alignent
 * PAR LA DROITE : sur chaque axe, une taille 1 est etiree a la taille de
 * l'autre, un axe absent (rang plus court) compte comme 1. Ces helpers sont
 * partages par le forward (obj_tensor_binop) et le backward (reduce-sum). */

/* Rend le rang de sortie et remplit outShape avec la forme broadcast, ou -1 si
 * incompatible (un axe ou les deux tailles different sans qu'aucune soit 1). */
static inline int tensor_broadcast_shape(int ndimA, const int* shapeA,
                                         int ndimB, const int* shapeB,
                                         int* outShape) {
    int ndimOut = ndimA > ndimB ? ndimA : ndimB;
    for (int d = 0; d < ndimOut; d++) {
        int posA = d - (ndimOut - ndimA);
        int posB = d - (ndimOut - ndimB);
        int sa = posA >= 0 ? shapeA[posA] : 1;
        int sb = posB >= 0 ? shapeB[posB] : 1;
        if (sa == sb)     outShape[d] = sa;
        else if (sa == 1) outShape[d] = sb;
        else if (sb == 1) outShape[d] = sa;
        else return -1;
    }
    return ndimOut;
}

/* Strides row-major "effectifs" d'un operande (ndim,shape) alignes par la
 * droite sur une sortie de rang ndimOut : 0 sur un axe absent ou etire (taille
 * 1), sinon le stride row-major reel. Un index de sortie sur un axe a stride 0
 * ne bouge pas l'operande -> c'est ce qui realise la diffusion (et, au
 * backward, la reduce-sum : plusieurs index de sortie retombent sur le meme
 * index d'operande, dont les contributions s'additionnent). */
static inline void tensor_broadcast_strides(int ndim, const int* shape,
                                            int ndimOut, int* bstr) {
    int real[OWL_TENSOR_MAX_RANK];
    int acc = 1;
    for (int i = ndim - 1; i >= 0; i--) { real[i] = acc; acc *= shape[i]; }
    for (int d = 0; d < ndimOut; d++) {
        int pos = d - (ndimOut - ndim);
        bstr[d] = (pos < 0 || shape[pos] == 1) ? 0 : real[pos];
    }
}

static inline bool tensor_shapes_equal(const ObjTensor* x, const ObjTensor* y) {
    if (x->ndim != y->ndim) return false;
    for (int i = 0; i < x->ndim; i++)
        if (x->shape[i] != y->shape[i]) return false;
    return true;
}

/* ================================================================
 * IA MODELE ( — API reseau de neurones haut-niveau)
 *
 * Un IAModèle tient une LISTE ORDONNEE de couches (collecte explicite : le
 * modele connait ses couches, pas de scan magique). Une couche est soit
 * DENSE (poids Tenseur 2D in×out + biais Tenseur 1D out), soit une
 * ACTIVATION (ReLU / Sigmoide / Tanh, sans poids). `.Infère` fait passer
 * une entree a travers chaque couche dans l'ordre. Les Tenseurs poids/biais
 * sont des Obj GC references depuis le tableau `layers` (C-heap) : ils sont
 * traces par trace_references (sinon collecte pendant que le modele vit).
 * ================================================================ */
typedef enum {
    IA_LAYER_DENSE = 0,   /* poids (Tenseur 2D) + biais (Tenseur 1D) */
    IA_LAYER_RELU,        /* activation ReLU, sans poids */
    IA_LAYER_SIGMOID,     /* activation Sigmoide, sans poids */
    IA_LAYER_TANH,        /* activation Tanh, sans poids */
    IA_LAYER_SILU,        /*  : activation SiLU (Swish), x·sigmoide(x), sans poids.
                           * Motif dominant des reseaux de detection modernes ; a l'import
                           * ONNX, reconnu depuis le couple Sigmoid + Mul(t, Sigmoid(t)). */
    IA_LAYER_CONV2D,      /*  : convolution 2D (poids Cout×Cin×kH×kW + biais 1×Cout×1×1) */
    IA_LAYER_MAXPOOL,     /*  : max-pooling non chevauchant (window, sans poids) */
    IA_LAYER_AVGPOOL,     /*  : average-pooling non chevauchant (window, sans poids) */
    IA_LAYER_BATCHNORM,   /*  : normalisation par lot (gamma/beta par canal + eps) */
    IA_LAYER_FLATTEN,     /*  : aplatissement B×C×H×W -> B×(C·H·W), sans poids */
    IA_LAYER_BRANCH_BEGIN,   /*  : ouverture d'un bloc a branches paralleles — capture
                              * l'activation courante x (point de divergence) et ouvre la 1re
                              * branche. Sans poids, ni entrainable ni concerne par les optimiseurs. */
    IA_LAYER_BRANCH_NEXT,    /*  : clot la branche courante, rouvre une branche repartant
                              * du MEME x. Sans poids. Une branche vide = identite (laisse passer x). */
    IA_LAYER_MERGE_ADD,      /*  : clot le bloc, fusionne toutes les branches par addition
                              * element-par-element (op add tenseur existante). Sans poids. */
    IA_LAYER_MERGE_CONCAT,   /*  : clot le bloc, fusionne toutes les branches en concatenant
                              * leurs sorties sur l'axe canal (index 1) — [N,C1,H,W]+[N,C2,H,W] -> [N,C1+C2,H,W].
                              * Exige memes N,H,W (seuls les canaux different). Sans poids. */
} IALayerKind;

typedef struct {
    IALayerKind  kind;
    ObjTensor*   weights;   /* dense (in×out) ou conv (Cout×Cin×kH×kW), sinon NULL */
    ObjTensor*   bias;      /* dense (out) ou conv (1×Cout×1×1), sinon NULL */
    /*  — parametres/hyperparametres des couches spatiales. */
    ObjTensor*   gamma;     /* batchnorm : echelle par canal (Cout), sinon NULL */
    ObjTensor*   beta;      /* batchnorm : decalage par canal (Cout), sinon NULL */
    /*  — statistiques mobiles par canal (C), accumulees en entrainement
     * (EMA) et utilisees a l'inference (mode eval). Permettent une inference
     * correcte image par image (lot de 1) au lieu des stats du lot courant. */
    ObjTensor*   running_mean; /* batchnorm : moyenne mobile par canal (C), sinon NULL */
    ObjTensor*   running_var;  /* batchnorm : variance mobile par canal (C), sinon NULL */
    int          stride;    /* conv : pas */
    int          pad;       /* conv : remplissage */
    int          window;    /* pooling : taille de fenetre (= pas) */
    double       eps;       /* batchnorm : epsilon */
    bool         frozen;    /*  : couche gelee -> l'optimiseur saute sa
                             * mise a jour (le forward la traverse normalement).
                             * Defaut faux ; persiste dans .Sauve/.Charge. */
} IALayer;

/* ================================================================
 * GRAPHE GENERAL D'IMPORT ONNX
 *
 * Le modele sequentiel (layers[]) ne represente qu'une chaine lineaire + blocs
 * a branches (chaque branche = sous-chaine). Il ne peut PAS representer un DAG
 * a FAN-OUT : un tenseur intermediaire lu par N consommateurs a des profondeurs
 * differentes (motif Split/C2f des detecteurs modernes). Pour ces graphes, on
 * bascule sur une representation explicite : des nœuds en ordre topologique
 * lisant/ecrivant une TABLE DE VALEURS (slot = tenseur ONNX). Le fan-out est
 * natif (un slot relu par plusieurs nœuds). Chemin de FALLBACK : n'est construit
 * que si le mapping sequentiel echoue ; les modeles existants gardent le chemin
 * layers[] a l'identique (transfert/entrainement inchanges).
 * ================================================================ */
#define IA_GRAPH_MAX_ARITY 16   /* entrees/sorties max d'un nœud (Concat large, Split) */

typedef enum {
    IA_GOP_LAYER = 0,   /* 1 entree -> 1 sortie : delegue a ia_layer_forward (via `layer`) */
    IA_GOP_SPLIT,       /* 1 entree -> N sorties : tranches de canaux (axe 1), tailles dans split[] */
    IA_GOP_CONCAT,      /* N entrees -> 1 sortie : concatenation sur l'axe canal (axe 1) */
    IA_GOP_ADD,         /* N entrees -> 1 sortie : addition element-par-element */
    IA_GOP_REDUCEMEAN,  /* 1 entree -> 1 sortie : moyenne sur axes[] (spatiaux), keepdims */
    IA_GOP_CONST,       /* 0 entree -> 1 sortie : Tenseur constant porte par layer.weights (Constant ONNX) */
    IA_GOP_RESIZE,      /* 2 entrees (X, scales) -> 1 sortie : upsample nearest spatial (asymmetric/floor) */
    IA_GOP_TRANSPOSE,   /*  : 1 entree -> 1 sortie : permutation d'axes quelconque (perm dans axes[], naxes=rang ; naxes=0 -> inversion des axes) */
    IA_GOP_SOFTMAX,     /*  : 1 entree -> 1 sortie : softmax stable sur l'axe axes[0] (negatif accepte) */
    IA_GOP_SLICE,       /*  : 1 entree -> 1 sortie : sous-plage sur naxes axes (axes[], sl_start/sl_end/sl_step), pas >= 1 */
    IA_GOP_ELTWISE,     /*  : element-par-element (Add/Sub/Mul/Div, elt_op) avec diffusion ; un operande peut etre une constante (layer.weights, elt_const_side) */
    IA_GOP_RESHAPE,     /*  : 1 entree -> 1 sortie : reshape a la forme cible (axes[0..naxes) ; 0 = garde l'axe, -1 = deduit) */
} IAGraphOp;

/*  — sous-op arithmetique element-par-element pour IA_GOP_ELTWISE. Ces
 * valeurs sont aussi le parametre `op` de obj_tensor_eltwise. */
typedef enum {
    IA_ELT_ADD = 0,
    IA_ELT_SUB = 1,
    IA_ELT_MUL = 2,
    IA_ELT_DIV = 3,
} IAEltwiseOp;

typedef struct {
    IAGraphOp op;
    IALayer   layer;    /* params quand op == IA_GOP_LAYER (Conv/Dense/activation/pool/BN/flatten) */
    int inputs[IA_GRAPH_MAX_ARITY];   /* slots d'entree dans la table de valeurs */
    int ninputs;
    int outputs[IA_GRAPH_MAX_ARITY];  /* slots de sortie dans la table de valeurs */
    int noutputs;
    int split[IA_GRAPH_MAX_ARITY];    /* IA_GOP_SPLIT : taille canal de chaque sortie */
    int axes[OWL_TENSOR_MAX_RANK];    /* IA_GOP_REDUCEMEAN : axes 0-based a reduire (tries desc).
                                       *  : reutilise par TRANSPOSE (perm), SOFTMAX/CONCAT
                                       * (axes[0] = axe), SLICE (axes tranches) */
    int naxes;
    bool keepdims;                    /* IA_GOP_REDUCEMEAN */
    /*  — IA_GOP_SLICE : bornes par axe tranche (index k aligne sur axes[k]). */
    int sl_start[OWL_TENSOR_MAX_RANK];
    int sl_end[OWL_TENSOR_MAX_RANK];
    int sl_step[OWL_TENSOR_MAX_RANK];
    /*  — IA_GOP_ELTWISE : sous-op (IAEltwiseOp) et cote de la constante
     * (0 aucune -> 2 slots ; 1 const a gauche ; 2 const a droite, dans layer.weights). */
    int elt_op;
    int elt_const_side;
} IAGraphNode;

typedef struct ObjIAModele {
    Obj      obj;
    IALayer* layers;    /* tableau C-heap (realloc geometrique), Obj traces par GC */
    int      count;
    int      capacity;
    /*  — profondeur de blocs a branches ouverts a la CONSTRUCTION
     * (incremente par .DébutBranches, decremente par .FusionneParAddition).
     * Sert a rejeter immediatement un .BrancheSuivante / .FusionneParAddition
     * sans DébutBranches ouvert. Le desequilibre inverse (Début jamais fusionne)
     * est detecte au forward. */
    int      branchDepth;
    /*  — graphe general (DAG a fan-out). NULL/0 => modele sequentiel
     * classique (chemin layers[]). Non-NULL => `.Infère` execute le graphe. Les
     * Tenseurs poids/biais des nœuds sont traces par le GC via ces nœuds. */
    IAGraphNode* gnodes;
    int          ngnodes;
    int          gcapacity;    /* capacite allouee du tableau gnodes (C-heap) */
    int          nvalues;      /* nombre de slots de la table de valeurs */
    int          graphInSlot;  /* slot de l'entree du graphe (-1 si absent) */
    int          graphOutSlot; /* slot de la sortie du graphe (-1 si absent) */
    /*  — forme d'entree declaree (NCHW) du modele importe via ONNX :
     * hauteur/largeur attendues du tenseur d'entree. 0 si non importe depuis un
     * ONNX ou si la forme n'est pas declaree. `.Détecte` redimensionne l'image a
     * inputW*inputH avant d'inferer (S = inputH pour un modele carre type YOLO). */
    int          inputH;
    int          inputW;
} ObjIAModele;

/* ================================================================
 * OPTIONS D'ENTRAINEMENT
 *
 * Config d'UN entrainement (`opts est un OptionsEntraînement`) : distincte du
 * modele (analogie httpRequête, la "requête" de l'action .Entraîne). Feuille
 * pure — que des champs scalaires (entiers/reel/valeurs d'enum), aucune
 * reference OWL sortante, rien a tracer cote GC. Proprietes lues/ecrites via
 * le double point (..Époques, ..TailleLot, ..TauxApprentissage, ..Optimiseur,
 * ..Perte), cablees cote VM (OP_STRUCT_GET / OP_STRUCT_SET).
 * ================================================================ */
typedef struct ObjOptionsEntrainement {
    Obj    obj;
    int    epochs;         /* ..Époques / ..Epochs */
    int    batchSize;      /* ..TailleLot / ..BatchSize */
    double learningRate;   /* ..TauxApprentissage / ..LearningRate */
    int    optimizer;      /* ..Optimiseur / ..Optimizer — sentinel optim* */
    int    loss;           /* ..Perte / ..Loss — sentinel perte* */
    int    lrSchedule;     /* ..PlanTaux / ..LRSchedule — sentinel plan* */
    double weightDecay;    /* ..DécroissancePoids / ..WeightDecay — L2, 0.0 = off */
    double validationFraction; /* ..FractionValidation / ..ValidationFraction — 0.0 = off */
    int    keepBestModel;  /* ..GardeMeilleurModèle / ..KeepBestModel — bool 0/1 */
    int    stopPatience;   /* ..PatienceArrêt / ..StopPatience — 0 = off */
    Value  onEpoch;        /* ..SurÉpoque / ..OnEpoch — reference de procedure
                            * rappelee a la fin de chaque epoque, NIL_VAL si non
                            * affectee. NON-feuille GC : tracee dans gc.c. */
} ObjOptionsEntrainement;

/* ================================================================
 * ARBRE DE DÉCISION CART ( — type MLArbreDécision/MLDecisionTree)
 *
 * Premier modèle de machine learning classique d'OWL. L'arbre appris est une
 * structure C-heap PURE (nœuds malloc'd, aucune référence OWL sortante) :
 * l'ObjMLArbre est donc une FEUILLE GC (comme ObjVector/ObjMatrix). Le seul
 * payload à libérer au sweep est l'arbre récursif (obj_mlarbre_free_tree).
 * Les hyperparamètres et l'objectif sont réglés par double point (..Objectif,
 * ..ProfondeurMax, ..MinÉchantillonsSéparation, ..MinÉchantillonsFeuille),
 * câblés côté VM (OP_STRUCT_GET/SET). .Entraîne(X, y) construit l'arbre,
 * .Prédit(X) rend un Tenseur 1D de prédictions.
 * ================================================================ */

/* Un nœud de l'arbre CART. Nœud interne : teste `x[feature] <= threshold`,
 * descend à gauche si vrai, à droite sinon. Feuille : rend `value` (classe
 * majoritaire en classification, moyenne des cibles en régression). */
typedef struct MLTreeNode {
    bool   isLeaf;
    int    feature;             /* nœud interne : colonne testée (0-based) */
    double threshold;           /* nœud interne : seuil de décision */
    struct MLTreeNode* left;    /* enfant x[feature] <= threshold */
    struct MLTreeNode* right;   /* enfant x[feature]  > threshold */
    double value;               /* feuille : classe (classif) ou moyenne (régression) */
} MLTreeNode;

typedef struct ObjMLArbre {
    Obj obj;
    int objective;        /* ..Objectif — 0 = non réglé, sinon sentinel obj* */
    int maxDepth;         /* ..ProfondeurMax — 0 = illimité (défaut) */
    int minSamplesSplit;  /* ..MinÉchantillonsSéparation — défaut 2 */
    int minSamplesLeaf;   /* ..MinÉchantillonsFeuille — défaut 1 */
    MLTreeNode* root;     /* arbre appris (C-heap), NULL avant .Entraîne */
} ObjMLArbre;

/* ================================================================
 * FORÊT ALÉATOIRE ( — type MLForêtAléatoire/MLRandomForest)
 *
 * Ensemble de `..NbArbres` arbres CART (mêmes nœuds MLTreeNode que
 * MLArbreDécision) : chaque arbre est entraîné sur un échantillon bootstrap
 * (tirage avec remise des lignes) et, à chaque séparation, sur un
 * sous-ensemble aléatoire de `..VariablesParSéparation` variables. La
 * prédiction agrège les arbres (vote majoritaire en classification, moyenne
 * en régression). Tout le hasard est piloté par un PRNG LOCAL seedé par
 * `..Graine` : à graine fixe, l'entraînement est intégralement reproductible
 * (jamais l'état global Hasard). Comme MLArbreDécision, la forêt est une
 * FEUILLE GC : les arbres sont du C-heap pur (aucune référence OWL sortante),
 * libérés récursivement au sweep.
 * ================================================================ */
typedef struct ObjMLForet {
    Obj obj;
    int objective;        /* ..Objectif — 0 = non réglé, sinon sentinel obj* */
    int maxDepth;         /* ..ProfondeurMax — 0 = illimité (défaut) */
    int minSamplesSplit;  /* ..MinÉchantillonsSéparation — défaut 2 */
    int minSamplesLeaf;   /* ..MinÉchantillonsFeuille — défaut 1 */
    int numTrees;         /* ..NbArbres — défaut 100 */
    int maxFeatures;      /* ..VariablesParSéparation — 0 = auto (√nb classif, toutes régression) */
    int64_t seed;         /* ..Graine — 0 = graine fixe reproductible (jamais "pas de graine") */
    MLTreeNode** trees;   /* numTrees arbres (C-heap), NULL avant .Entraîne */
    int treeCount;        /* nombre d'arbres effectivement construits */
    int nClasses;         /* classif : nb de classes, mémorisé pour le vote */
} ObjMLForet;

/* ================================================================
 * GRADIENT BOOSTING ( — type MLGradientBoosting)
 *
 * Ensemble SÉQUENTIEL de `..NbArbres` étapes : chaque étape fitte un (ou K en
 * classification multi-classes) arbre(s) de régression CART sur les
 * pseudo-résidus de l'ensemble courant, pondérés par `..TauxApprentissage`
 * (shrinkage). Le boosting construit TOUJOURS des arbres de RÉGRESSION en
 * interne, quel que soit l'objectif (ils fittent des gradients continus) :
 * il réutilise donc le CART régression de MLArbreDécision.
 *   - Régression : score initial F0 = moyenne(y) ; prédiction = F0 + taux·Σ hₘ(x).
 *   - Classification multi-classes (K = max(y)+1) : K scores initiaux à 0,
 *     softmax + un arbre par classe à chaque étape ; prédiction = argmax_k F_k(x).
 * DÉTERMINISTE : aucune graine, aucun tirage (le CART est déjà déterministe).
 * Comme MLArbreDécision/MLForêtAléatoire, une FEUILLE GC : les arbres et les
 * scores initiaux sont du C-heap pur (aucune référence OWL sortante), libérés
 * au sweep. Les arbres sont stockés à plat : étape m, classe k -> trees[m*K+k]
 * (K = 1 en régression, donc trees[m]).
 * ================================================================ */
typedef struct ObjMLBoosting {
    Obj obj;
    int objective;        /* ..Objectif — 0 = non réglé, sinon sentinel obj* */
    int numTrees;         /* ..NbArbres — défaut 100 (nombre d'étapes) */
    double learningRate;  /* ..TauxApprentissage — défaut 0.1 (shrinkage) */
    int maxDepth;         /* ..ProfondeurMax — défaut 3 (arbres peu profonds) */
    int minSamplesSplit;  /* ..MinÉchantillonsSéparation — défaut 2 */
    int minSamplesLeaf;   /* ..MinÉchantillonsFeuille — défaut 1 */
    double* init0;        /* scores initiaux (C-heap) : 1 (régression) ou K (multi-classes), NULL avant .Entraîne */
    MLTreeNode** trees;   /* arbres appris (C-heap) : nSteps (régr.) ou nSteps×K (multi), NULL avant .Entraîne */
    int treeCount;        /* nombre d'arbres effectivement construits (= nSteps × nClasses) */
    int nSteps;           /* nombre d'étapes de boosting construites */
    int nClasses;         /* régression : 1 ; classification : K = max(y)+1 */
} ObjMLBoosting;

/* ================================================================
 * RÉGRESSION LINÉAIRE ( — type MLRégressionLinéaire/MLLinearRegression)
 *
 * Moindres carrés ordinaires (OLS) en FORME CLOSE : résout les équations
 * normales (XᵀX)·θ = Xᵀy où X est augmentée d'une colonne de 1 (l'intercept),
 * par élimination de Gauss avec pivot partiel. Un léger ridge diagonal (1e-8)
 * rend le système toujours résoluble. ZÉRO hyperparamètre, déterministe, exact :
 * sur un jeu strictement linéaire sans bruit, retrouve les coefficients d'origine
 * au réel près. Les poids appris sont du C-heap pur (aucune référence OWL
 * sortante) : FEUILLE GC, `coef` libéré au sweep.
 * ================================================================ */
typedef struct ObjMLLineaire {
    Obj obj;
    int nCoef;          /* nombre de variables (colonnes de X) */
    double* coef;       /* nCoef poids w1..wn (C-heap), NULL avant .Entraîne */
    double intercept;   /* ordonnée à l'origine (b) */
    bool trained;       /* .Entraîne appelée au moins une fois */
} ObjMLLineaire;

/* ================================================================
 * RÉGRESSION LOGISTIQUE ( — type MLRégressionLogistique/MLLogisticRegression)
 *
 * Régression softmax (K = max(y)+1 classes ; binaire = K=2) entraînée par
 * DESCENTE DE GRADIENT sur la cross-entropy. Poids W (K×variables) et biais b (K)
 * initialisés à ZÉRO (déterminisme : pas de RNG), mis à jour échantillon par
 * échantillon sur `..NbItérations` étapes avec le pas `..TauxApprentissage`.
 * Softmax stable (soustraction du max). La prédiction rend l'argmax des scores
 * (indice de classe). Poids C-heap purs (aucune référence OWL sortante) : FEUILLE
 * GC, W et bias libérés au sweep.
 * ================================================================ */
typedef struct ObjMLLogistique {
    Obj obj;
    double learningRate;  /* ..TauxApprentissage — défaut 0.1 */
    int numIterations;    /* ..NbItérations — défaut 500 */
    int nClasses;         /* K = max(y)+1, fixé à l'entraînement */
    int nFeatures;        /* nombre de variables (colonnes de X) */
    double* W;            /* K×nFeatures poids (C-heap), NULL avant .Entraîne */
    double* bias;         /* K biais (C-heap), NULL avant .Entraîne */
    bool trained;         /* .Entraîne appelée au moins une fois */
} ObjMLLogistique;

/* ================================================================
 * K-MEANS ( — type MLKMeans, clustering non-supervisé)
 *
 * Partitionne les lignes d'une Matrice en `..NbClusters` groupes par
 * l'algorithme de Lloyd : affectation au centroïde le plus proche
 * (distance euclidienne au carré) puis recalcul des centroïdes = moyenne
 * des points affectés, jusqu'à convergence ou `..NbItérations`. L'init des
 * centroïdes suit k-means++ (choix proportionnel à la distance²), tiré par
 * un PRNG LOCAL xorshift seedé par `..Graine` (jamais l'état global Hasard)
 * — d'où la reproductibilité bit-exacte à graine fixe. Modèle NON-SUPERVISÉ :
 * `.Entraîne` ne prend QUE X (pas d'étiquettes). Les centroïdes appris sont
 * du C-heap pur (aucune référence OWL sortante) : FEUILLE GC, `centroids`
 * libéré au sweep.
 * ================================================================ */
typedef struct ObjMLKMeans {
    Obj obj;
    int numClusters;    /* ..NbClusters / ..NumClusters — défaut 8 (le k) */
    int numIterations;  /* ..NbItérations / ..NumIterations — défaut 300 (max Lloyd) */
    int64_t seed;       /* ..Graine / ..Seed — défaut 0 (graine fixe reproductible) */
    int nFeatures;      /* nombre de variables (colonnes de X), fixé à l'entraînement */
    double* centroids;  /* numClusters × nFeatures centres (C-heap), NULL avant .Entraîne */
    bool trained;       /* .Entraîne appelée au moins une fois */
} ObjMLKMeans;

/* ================================================================
 * IMAGE ETIQUETEE
 *
 * Un exemple etiquete : le lien disque (`..Chemin`), le nom de classe
 * (`..Étiquette`, grave a l'import) et l'Image (`..Image`), MATERIALISEE
 * PARESSEUSEMENT — chargee depuis le disque au premier acces de `..Image`,
 * pas a l'import. Un fichier absent/illisible ne bloque donc pas le
 * chargement du Dataset : l'exception ne tombe qu'a la lecture de `..Image`
 * sur cet exemple precis. NON-feuille GC : trace path/label/image (gc.c).
 * ================================================================ */
typedef struct ObjImageEtiquetee {
    Obj          obj;
    ObjString*   path;    /* ..Chemin / ..Path — chemin disque du fichier image */
    ObjString*   label;   /* ..Étiquette / ..Label — nom de classe */
    Obj*         image;   /* ..Image — ObjInstance Image, NULL tant que non materialisee. Obj* (ObjInstance est un typedef anonyme defini plus bas) : caster a l'usage. */
} ObjImageEtiquetee;

/* ================================================================
 * IMAGE ANNOTEE
 *
 * Un exemple de detection : le lien disque (`..Chemin`), le tableau des boites
 * annotees (`..Boîtes` / `..Boxes` — chaque boite = tableau `[classe, cx, cy,
 * l, h]` en coordonnees normalisees 0..1 telles que lues) et l'Image
 * (`..Image`), MATERIALISEE PARESSEUSEMENT comme pour ImageÉtiquetée. Une
 * image sans annotation est un exemple valide a 0 boite (fond). NON-feuille GC :
 * trace path/boxes/image (gc.c).
 * ================================================================ */
typedef struct ObjImageAnnotee {
    Obj          obj;
    ObjString*   path;    /* ..Chemin / ..Path — chemin disque du fichier image */
    Obj*         boxes;   /* ..Boîtes / ..Boxes — ObjArray de boites (chaque boite = ObjArray de reels). Obj* : caster a l'usage (ObjArray defini plus bas). */
    Obj*         image;   /* ..Image — ObjInstance Image, NULL tant que non materialisee. */
} ObjImageAnnotee;

/* ================================================================
 * DATASET de ImageÉtiquetée
 *
 * Collection ordonnee d'exemples etiquetes pour l'entrainement d'un
 * classifieur. Detient un tableau C-heap d'ImageÉtiquetée (Obj GC traces) et
 * la liste triee des classes (noms distincts, ordre alphabetique = index de
 * classe 0-based). `.NombreLots`/`.LotEntrées`/`.LotCibles` decoupent en lots
 * (X en B×C×H×W via ImageVersTenseur, Y en indices de classe). `.Mélange`
 * rebat l'ordre du tableau d'exemples sans perte ni doublon.
 * ================================================================ */
typedef struct ObjDataset {
    Obj                  obj;
    Obj**                items;         /* tableau C-heap d'exemples GC traces. Type d'exemple homogene, indique par `itemType` (OBJ_IMAGE_ETIQUETEE = classification, OBJ_IMAGE_ANNOTEE = detection). */
    ObjType              itemType;      /*  — tag du type d'exemple, pose par le chargeur / au 1er push. Permet aux methodes communes (lots/melange) de dispatcher (chemin + slot image). */
    int                  count;
    int                  capacity;
    ObjString**          classes;      /* noms de classes distincts, tries (Obj GC traces) */
    int                  classCount;
    ObjString*           source;       /* ..Source — chemin racine/CSV passe au chargeur */
    /* Augmentation opt-in, appliquee au vol dans .LotEntrées.
     * Defaut : aucune (comportement inchange). Tirage sur le RNG global
     * (rand()), le meme que .Mélange() : InitHasard(graine) rend reproductible. */
    bool                 flipHorizontalActif;  /* .RetournementHorizontalAléatoire */
    int                  margeRecadrage;       /* .RecadrageAléatoire (px) ; 0 = inactif */
} ObjDataset;

/* ================================================================
 * XLS DOCUMENT ( — type xlsDocument, classeur Excel .xlsx)
 *
 * Classeur Excel manipule en memoire : une pile ordonnee de feuilles, chaque
 * feuille portant ses cellules (valeur = chaine UTF-8 malloc'd) et ses plages
 * fusionnees. Aucune reference OWL sortante (les valeurs de cellule sont des
 * char* C, pas des Value) -> feuille GC comme ObjZipArchive, avec un finalizer
 * C au sweep qui libere feuilles/cellules/fusions/chemin. L'ecriture disque
 * serialise l'archive OOXML (miniz + XML genere), la lecture reparse un .xlsx
 * ecrit par ce meme moteur (round-trip cellules texte + feuilles).
 * ================================================================ */

typedef struct {
    int   row;      /* 1-based */
    int   col;      /* 1-based */
    char* value;    /* UTF-8 malloc'd, NUL-terminated (jamais NULL une fois posee) */
    bool  isNumber; /* true = cellule numerique OOXML (<v> nu, sommable), false = sharedString */
    /*  — style de cellule stocke en memoire (generation styles.xml =
     * lot suivant). Scalaires plats + deux composites materialises paresseusement
     * (Police / Cadre), stockes comme Value OWL et donc TRACES par le GC (cf.
     * OBJ_XLS_DOCUMENT dans gc.c, non-feuille depuis ). */
    int64_t bgColor;     bool hasBg;        /* CouleurFond (entier couleur) */
    int     alignH;      bool hasAlignH;    /* E_CadrageH */
    int     alignV;      bool hasAlignV;    /* E_CadrageV */
    bool    multiline;   bool hasMultiline; /* retour a la ligne */
    char*   mask;        /* format Excel (chaine malloc'd) ou NULL */
    Value   font;        /* ObjInstance Police ou NIL_VAL */
    Value   border;      /* ObjInstance Cadre ou NIL_VAL */
} XlsCell;

typedef struct {
    int firstRow, firstCol;   /* coin haut-gauche, 1-based */
    int lastRow,  lastCol;    /* coin bas-droite, 1-based */
} XlsMerge;

/*  — hauteur de ligne / largeur de colonne, indexees 1-based. Tableaux
 * creux (seules les lignes/colonnes stylees existent). */
typedef struct { int index; double value; } XlsDim;

typedef struct {
    char*     name;           /* nom de feuille, malloc'd */
    XlsCell*  cells;          /* tableau plat, malloc'd */
    int       cellCount;
    int       cellCap;
    XlsMerge* merges;
    int       mergeCount;
    int       mergeCap;
    XlsDim*   rowDims;        /*  — hauteurs de ligne (px), creux */
    int       rowDimCount;
    int       rowDimCap;
    XlsDim*   colDims;        /*  — largeurs de colonne (px), creux */
    int       colDimCount;
    int       colDimCap;
} XlsSheet;

typedef struct ObjXlsDocument {
    Obj       obj;
    XlsSheet* sheets;
    int       sheetCount;
    int       sheetCap;
    int       current;        /* index 0-based de la feuille en cours */
    char*     path;           /* chemin associe (xlsOuvre/xlsSauve), malloc'd ou NULL */
    /*  — parts ZIP d'un classeur OUVERT qu'OWL ne modelise pas (macros
     * vbaProject, graphiques, medias, drawings, themes...). Capturees verbatim a
     * l'ouverture, reinjectees telles quelles a xlsSauve pour ne pas les perdre.
     * Vides (NULL/0) pour un classeur cree de zero : regeneration integrale. Ce
     * sont des octets bruts C-heap, aucune Value OWL -> pas de trace GC. */
    char**    preservedNames; /* noms d'entree zip, chacun malloc'd */
    void**    preservedData;  /* buffers bruts, chacun malloc'd */
    size_t*   preservedLens;  /* longueur de chaque buffer */
    int       preservedCount;
    int       preservedCap;
} ObjXlsDocument;

/*  — reference de style dans un classeur. `doc..Cellule[l,c]`,
 * `doc..Colonne[c]`, `doc..Ligne[l]` produisent une reference : d'abord non
 * liee (renvoyee par la propriete), puis liee par le subscript. Les proprietes
 * de style (`..CouleurFond`, `..Police`, `..Largeur`...) lisent/ecrivent le
 * modele du classeur via cette reference. */
typedef enum {
    XLS_REF_CELL,     /* ..Cellule : liee par subscript 2D [ligne, colonne] */
    XLS_REF_ROW,      /* ..Ligne   : liee par subscript 1D [ligne] */
    XLS_REF_COLUMN,   /* ..Colonne : liee par subscript 1D [colonne] */
} XlsRefKind;

typedef struct ObjXlsRef {
    Obj              obj;
    ObjXlsDocument*  doc;     /* classeur cible (trace par le GC) */
    XlsRefKind       kind;
    int              row;     /* 1-based, 0 si non liee */
    int              col;     /* 1-based, 0 si non liee */
    bool             bound;   /* true apres le subscript */
} ObjXlsRef;

/* ================================================================
 * LLM
 *
 * Modele de langage GGUF charge en memoire. Detient deux handles C-heap
 * opaques issus de llama.cpp (llama_model* + llama_context*) plus le vocab
 * (emprunte au modele, non-owned). Les handles sont stockes en `void*` pour
 * ne pas tirer llama.h dans object.h (compile partout, y compris cibles sans
 * llama) — meme decouplage que le backend opaque d'ObjZipArchive.
 *
 * Feuille GC (aucune reference OWL sortante) MAIS avec finalizer C : a la
 * collection, les handles doivent etre liberes (llama_free + llama_model_free).
 * object.c ne connait pas llama : il delegue a un callback enregistre par
 * stdlib_llm (obj_llm_set_backend_free), comme g_zip_backend_close.
 * ================================================================ */
typedef struct ObjLLM {
    Obj   obj;
    void* model;   /* llama_model*   — NULL tant que .Charge n'a pas reussi */
    void* ctx;     /* llama_context* — NULL tant que non charge */
    void* vocab;   /* const llama_vocab* — emprunte au modele, non-owned */
} ObjLLM;

/* ================================================================
 * OPTIONS DE GENERATION
 *
 * Reglages d'UN appel `.Génère` (analogie OptionsEntraînement pour
 * l'entrainement). Feuille scalaire + un callback de flux `..SurToken`
 * (reference de procedure, comme `..SurÉpoque`). Proprietes lues/ecrites au
 * double point, cablees cote VM (OP_STRUCT_GET / OP_STRUCT_SET).
 * ================================================================ */
typedef struct ObjOptionsGeneration {
    Obj    obj;
    double temperature;   /* ..Température / ..Temperature — 0 = greedy deterministe */
    int    topK;          /* ..TopK */
    double topP;          /* ..TopP */
    int    maxTokens;     /* ..MaxTokens */
    int64_t seed;         /* ..Graine / ..Seed */
    Value  onToken;       /* ..SurToken / ..OnToken — reference de procedure
                           * rappelee a chaque token genere, NIL_VAL si non
                           * affectee. NON-feuille GC : tracee dans gc.c. */
    Value  constraint;    /* ..ContrainteDeSortie / ..OutputConstraint — open-enum
                           * (ObjString) ou NIL_VAL si vide. Vide = aucune contrainte ;
                           * "json" (casse-insensible) = grammaire JSON integree ;
                           * toute autre chaine = grammaire GBNF brute. NON-feuille GC
                           * des qu'une chaine y est posee : tracee dans gc.c. */
} ObjOptionsGeneration;

/* ================================================================
 * MESSAGE LLM
 *
 * Un message d'une conversation multi-tours, entree de `LLM.Chat`. Deux
 * proprietes lues/ecrites au double point : `..Rôle` (enum ouverte E_rôleLLM :
 * rôleSystème/rôleUtilisateur/rôleAssistant/rôleOutil ou chaine libre) et
 * `..Contenu` (le texte). Construit au litteral d'accolade nomme
 * (`{Rôle = ..., Contenu = ...}`). Les deux champs sont des ObjString (ou NIL
 * avant affectation) — NON-feuille GC : traces dans gc.c.
 * ================================================================ */
typedef struct ObjMessageLLM {
    Obj   obj;
    Value role;     /* ..Rôle / ..Role — ObjString ou NIL_VAL */
    Value content;  /* ..Contenu / ..Content — ObjString ou NIL_VAL */
} ObjMessageLLM;

/* ================================================================
 * REPONSE LLM
 *
 * Sortie de `LLM.Chat` (symetrie httpRequête/httpRéponse). Proprietes en
 * lecture seule (renseignees par le moteur, pas de setter user) :
 *   - `..Contenu` (ObjString) : le texte de reponse de l'assistant.
 *   - `..Raison` (entier, enum stricte E_raisonStop) : OWL_STOP_NATUREL ou
 *     OWL_STOP_MAX_TOKENS.
 *   - `..JetonsEntrée` / `..JetonsSortie` (entiers) : compteurs de jetons.
 * content est un ObjString (ou NIL) — NON-feuille GC : trace dans gc.c.
 * ================================================================ */
typedef struct ObjResponseLLM {
    Obj   obj;
    Value content;      /* ..Contenu / ..Content — ObjString ou NIL_VAL */
    int   reason;       /* ..Raison / ..Reason — OWL_STOP_* */
    int   inputTokens;  /* ..JetonsEntrée / ..InputTokens */
    int   outputTokens; /* ..JetonsSortie / ..OutputTokens */
} ObjResponseLLM;

/* ================================================================
 * DÉTECTION D'OBJET
 *
 * Une detection produite par `IAModèle.Détecte`. Proprietes en lecture seule
 * (double point), renseignees par le decodage de la tete de detection :
 *   - `..Boîte` / `..Box` (ObjArray de 4 reels [x, y, largeur, hauteur], en
 *     pixels de l'image d'origine, coin haut-gauche puis taille).
 *   - `..Classe` / `..Class` (entier, index de classe 0-based).
 *   - `..Score` / `..Score` (reel [0,1]).
 * box est un ObjArray (ou NIL) — NON-feuille GC : trace dans gc.c.
 * ================================================================ */
typedef struct ObjDetection {
    Obj    obj;
    Value  box;         /* ..Boîte / ..Box — ObjArray de 4 reels, ou NIL_VAL */
    int    classIndex;  /* ..Classe / ..Class — index 0-based */
    double score;       /* ..Score — confiance [0,1] */
} ObjDetection;

/* ================================================================
 * ZIP ARCHIVE OBJECT (zipArchive — unified handle for
 * ZIP/WDZ/TAR/TGZ/7z). The archive backend is allocated opaquely by
 * the format dispatcher in stdlib_compression* and lives under the
 * `backend` pointer; the GC only tracks this struct and delegates
 * cleanup to a per-format close callback via obj_zip_archive_close().
 * ================================================================ */

typedef enum {
    ZIP_FMT_ZIP = 0,
    ZIP_FMT_CAB = 1,
    ZIP_FMT_RAR = 2,
    ZIP_FMT_WDZ = 3,
    ZIP_FMT_7Z  = 4,
    ZIP_FMT_TAR = 5,
    ZIP_FMT_TARGZ = 6
} ZipArchiveFormat;

typedef enum {
    ZIP_MODE_READ   = 1,
    ZIP_MODE_WRITE  = 2,
    ZIP_MODE_CREATE = 4
} ZipArchiveMode;

typedef struct {
    Obj    obj;
    ZipArchiveFormat format;
    ZipArchiveMode   mode;
    char*  path;               /* strdup'd file path, NULL when in-memory */
    void*  backend;            /* opaque: mz_zip_archive* | tar_state* | CSzArEx* */
    char*  password;           /* NULL when archive is not encrypted */
    int    compressionLevel;   /* 0-9, default 6 */
    bool   open;               /* false after zipFerme */
    int    lastError;          /* styled error code */
    char   lastErrorMsg[256];
    int    fileCount;          /* cached, refreshed after add/remove */
} ObjZipArchive;

/* ================================================================
 * MOCK NATIVE OBJECT (for Mock system)
 * ================================================================ */

typedef struct {
    Obj obj;
    int mockIndex;  /* Index into g_mockRegistry */
} ObjMockNative;

/* ================================================================
 * PATTERN OBJECT (for Mock argument matching)
 * ================================================================ */

typedef struct {
    Obj obj;
    int patternType;  /* PatternType from mock.h */
    Value value;      /* The value to match against */
} ObjPattern;

/* ================================================================
 * FUNCTION OBJECT (user-defined procedures)
 * ================================================================ */

/*  — debug info : nom canonique d'une locale/parametre -> son slot
 * dans la frame d'appel. Le nom est une ObjString internee (rootee par la
 * fonction, cf gc.c OBJ_FUNCTION) stockee sous forme accent-strippee (casse
 * preservee pour l'affichage user). Permet au debogueur (owl_debug_eval /
 * owl_debug_get_locals) de resoudre un identifiant par nom dans la frame
 * courante au lieu de retomber sur un homonyme global/registry. */
typedef struct {
    ObjString* name;    /* nom canonique (accents strippes, casse preservee) */
    int        slot;    /* index dans frame->slots[] */
    /*  — plage de validite bytecode du nom dans le chunk de la
     * fonction. Les slots sont recycles par end_scope : deux scopes
     * sequentiels peuvent partager un slot avec des noms differents. La
     * resolution par slot doit donc filtrer sur l'IP du point de pause.
     * ip_start = offset a la declaration ; ip_end = offset a la sortie de
     * scope, ou -1 tant que le scope est encore ouvert (jusqu'a la fin de
     * fonction). Validite = [ip_start, ip_end). */
    int        ip_start;
    int        ip_end;
} LocalName;

typedef struct {
    Obj obj;
    int arity;          /* total number of declared parameters */
    int minArity;       /* required parameters (those without a default value) */
    struct Chunk* chunk;    /* the function's bytecode */
    ObjString* name;
    ObjString* filename;    /* source file (NULL in single-file mode) */
    /* Default values for optional params, indexed by (paramIdx - minArity).
     * defaultCount == arity - minArity. NULL when no defaults. Values are
     * captured at definition time (literals only — evaluated later). */
    Value* defaults;
    int    defaultCount;
    /*  — table de noms de locales/params pour le debogueur. Emise
     * a la compile (add_local). NULL tant qu'aucune locale nommee n'a ete
     * declaree. localNames est realloc'd (owned), libere avec la fonction ;
     * les ObjString name sont gerees par le GC. */
    LocalName* localNames;
    int        localNameCount;
    int        localNameCapacity;
} ObjFunction;

/* ================================================================
 * NATIVE FUNCTION (C builtins like Trace, Taille, etc.)
 * ================================================================ */

typedef Value (*NativeFn)(int argCount, Value* args);

typedef struct {
    Obj obj;
    NativeFn function;
    int arity;             /* -1 = variadic */
    int firstOutParamArg;  /* -1 si pas d'out-params, sinon index 0-based depuis lequel les args sont par référence */
    int lastOutParamArg;   /* -1 = variadic (jusqu'à argCount-1), sinon borne haute incluse */
} ObjNative;

/* ================================================================
 * ARRAY OBJECT
 * ================================================================ */

typedef struct {
    Obj obj;
    ValueArray elements;
    ObjString* elementStructType; /* NULL if untyped; else name of the target
                                   * structure for `tableau de <Structure>`.
                                   * Used by Ajoute to coerce [v1,v2,...] literals
                                   * into structure instances (positional match). */
    int elementPrimKind;         /* 0=untyped (Variant-like, cells = NIL); else
                                   * kind primitif (1=chaine, 2=entier, 3=reel,
                                   * 4=booleen, 5=monetaire, 6=numerique). Drive
                                   * la valeur par défaut des cells créées par
                                   * extension (agrandissement, presize, etc.). */
    int growthMode;              /* canon <agrandissement>: 0=none (default),
                                   * 1=grow by 1, 2=grow to N. Set at decl time. */
    /*  — Groupe de Champs.
     * is_group=true marque cet ObjArray comme un Groupe (sous-type tableau de
     * Champ + dispatch broadcast). Set via OP_ARRAY_MARK_GROUP au moment de
     * la decla `GR_X est un Groupe [= [...]]`. Effets runtime :
     *   - OP_STRUCT_SET broadcast `..Prop = val` aux membres ObjUiControl
     *     qui exposent la prop (no-op silencieux sur les autres) ;
     *   - OP_STRUCT_GET cote ObjUiControl intercepte `..Groupe` et collecte
     *     les Groupes contenant le controle (parcours vm.windows).
     * owner_window = nom de la fenetre top-level proprietaire (NULL si decla
     * en local procedure — le Groupe broadcaste toujours mais n'apparait pas
     * dans `champ..Groupe`, qui ne voit que les Groupes top-level fenetre). */
    bool is_group;
    ObjString* owner_window;
} ObjArray;

/* ================================================================
 * ASSOCIATIVE ARRAY OBJECT (tableau associatif)
 * Key/value pairs with user-controlled key type and options.
 * v1: string keys only, SansDoublon mode implicit.
 * ================================================================ */

typedef enum {
    ASSOC_OPT_NONE         = 0,
    ASSOC_OPT_AVEC_DOUBLON = 1 << 0,
    ASSOC_OPT_SANS_CASSE   = 1 << 1,
    ASSOC_OPT_SANS_ACCENT  = 1 << 2,
    ASSOC_OPT_SANS_ESPACE  = 1 << 3,
} AssocOptions;

typedef enum {
    ASSOC_KEY_STRING = 0,  /* owlChaine — v1 */
    ASSOC_KEY_INT    = 1,  /* owlEntier — v2 */
    ASSOC_KEY_DATE   = 2,  /* owlDate — v3 */
} AssocKeyType;

/* Declared value kind — drives the default returned on missing-key reads. */
typedef enum {
    ASSOC_VALUE_VARIANT   = 0,   /* default: NIL */
    ASSOC_VALUE_CHAINE    = 1,   /* "" */
    ASSOC_VALUE_ENTIER    = 2,   /* 0 */
    ASSOC_VALUE_REEL      = 3,   /* 0.0 */
    ASSOC_VALUE_BOOLEEN   = 4,   /* Faux */
    ASSOC_VALUE_MONETAIRE = 5,   /* 0 currency */
    ASSOC_VALUE_NUMERIQUE = 6,   /* 0 numeric */
} AssocValueKind;

typedef struct {
    Obj          obj;
    AssocKeyType keyType;
    int          options;       /* bitmask AssocOptions */
    int          valueKind;     /* default value kind (VAL_*), for empty-key reads */
    ObjString**  keys;          /* [count] — v1 keys are always strings */
    Value*       values;        /* [count] — matching values */
    int          count;
    int          capacity;
    /*  — nom du type de valeur NON primitif (ex. "httpRequête"),
     * NULL pour variant/primitifs. Pilote l'auto-vivification objet : lire
     * OU ecrire une cle absente clone le template resolu par ce nom
     * (vm.types puis vm.globals), symetrique a . Les scalaires
     * (valueTypeName == NULL) ne vivifient qu'a l'ecriture — asymetrie canon
     * mesuree a l'oracle. */
    ObjString*   valueTypeName;
} ObjAssocArray;

/* ================================================================
 * MULTILINGUE OBJECT ( lot B — chaineMultilingue first-class)
 * Dictionnaire tag BCP 47 -> texte traduit. Indexable par constante
 * langueXxx (resolue cote VM en string "fr-FR" via vm.globals) ou par
 * tag string libre. Storage delegue a un ObjAssocArray interne pour
 * reutiliser l'API string-keyed avec options SansCasse (les sous-tags
 * BCP 47 sont case-insensitive par RFC 5646 — "fr-FR" == "fr-fr").
 * Dispatch GC dedie : on marque le storage uniquement.
 * ================================================================ */

typedef struct {
    Obj obj;
    ObjAssocArray* storage;  /* tag -> texte (ASSOC_KEY_STRING + SansCasse) */
} ObjMultilingue;

/* ================================================================
 * CLASS OBJECT
 * ================================================================ */

/* Forward declaration of Table from table.h - included via value.h chain */
#include "table.h"

/* Distingue les 3 sous-formes canon compilées comme ObjClass au runtime
 * (cf. compiler.c::class_declaration / enum_declaration / combination_declaration).
 * Sert à TypeVar() et au Définition.Type pour retourner le bon code wl*. */
typedef enum {
    CLASS_KIND_CLASS = 0,    /* défaut : Classe user-defined */
    CLASS_KIND_ENUM,         /* Enumeration */
    CLASS_KIND_COMBINATION   /* Combinaison */
} ClassKind;

typedef struct ObjClass {
    Obj obj;
    ObjString* name;
    Table methods;
    Table defaults;          /* default field values for members */
    Table staticFields;      /*  : membres GLOBAL (un exemplaire
                              * par classe, accessible cClasse.nom sans
                              * instance — pas copies dans les instances). */
    Table interfaces;        /*  : set des interfaces implementees par
                              * cette classe (cle = nom d'interface ObjString,
                              * valeur = NIL_VAL, juste un set). Peuplee par
                              * OP_CLASS_DECLARE_INTERFACE au moment de la
                              * declaration. Consultee par OP_IS_CLASS pour
                              * resoudre `obj est iInterface`. */
    struct ObjClass* superclass;
    ClassKind kind;
} ObjClass;

/* ================================================================
 * STRUCT / CLASS INSTANCE OBJECT
 * ================================================================ */

typedef struct {
    Obj obj;
    ObjString* structName;  /* struct type name */
    Table fields;           /* field name -> value */
    ObjArray* fieldOrder;   /* ordered list of field names as declared (NULL
                             * if not tracked). Used for positional coercion
                             * [v1,v2,...] -> struct instance. */
    ObjClass* klass;        /* class (NULL for plain structs) */
    /*  — back-ref vers le controle d'Etat proprietaire quand cette
     * instance composite (Cadre/Police/Style...) est la valeur d'une cle de
     * son headless_store. Posee par obj_ui_control_instance_headless_set au
     * moment du seed. Sert a marquer la prop composite override cote controle
     * quand un sous-champ est ecrit imperativement (`..Cadre..Couleur =`),
     * pour que le reseed par bloc ne clobbe plus l'ecriture.
     * Reference FORTE (marquee au GC) : l'instance ne survit normalement que
     * via le store du uici, mais si elle est capturee dans une variable elle
     * garderait sinon un pointeur pendouillant vers un uici collecte. */
    struct ObjUiControlInstance* ui_owner;
    ObjString*                   ui_owner_key;  /* cle canon de la prop composite */
} ObjInstance;

/* ================================================================
 * COLLECTION OBJECT (File/Queue, Pile/Stack, Liste/List)
 * ================================================================ */

typedef enum {
    COLLECTION_QUEUE,   /* File / Queue - FIFO */
    COLLECTION_STACK,   /* Pile / Stack - LIFO */
    COLLECTION_LIST,    /* Liste / List - doubly-linked (stored as dynamic array) */
} CollectionType;

typedef struct {
    Obj obj;
    CollectionType collType;
    ValueArray elements;
} ObjCollection;

/* ================================================================
 * PROCEDURE CHAIN OBJECT
 * Holds multiple procedures that run in sequence when the chain is
 * called. Plus optional Avant/Après hooks. Produced when a Procédure
 * variable is combined with another via += or when .Avant/.Après are set.
 * ================================================================ */

typedef struct ObjProcChain {
    Obj obj;
    Value* procs;     /* callable values: ObjFunction or ObjNative */
    int count;
    int capacity;
    Value avant;       /* NIL_VAL if unset */
    Value apres;       /* NIL_VAL if unset */
} ObjProcChain;

/* ================================================================
 * JSON OBJECT
 * ================================================================ */

typedef enum {
    JSON_T_NULL,
    JSON_T_BOOL,
    JSON_T_INT,
    JSON_T_REAL,
    JSON_T_CURRENCY,  /* __int128 scale 10^6 — preserve exact decimal (monetaire) */
    JSON_T_NUMERIC,   /* ObjNumeric* — preserve exact arbitrary precision (numerique) */
    JSON_T_STRING,
    JSON_T_ARRAY,
    JSON_T_OBJECT,
} JsonNodeType;

typedef struct ObjJson {
    Obj obj;
    JsonNodeType jtype;
    union {
        bool boolVal;
        int64_t intVal;
        double realVal;
        __int128 currencyVal;  /* scaled by 10^6 */
        struct ObjNumeric* numericVal;  /* GC child (marque par OBJ_JSON) */
        ObjString* strVal;
        struct { struct ObjJson** items; int count; int capacity; } arrayVal;
        struct { ObjString** keys; struct ObjJson** values; int count; int capacity; } objectVal;
    } as;
} ObjJson;

/* DSL Fenetre v1 (Phase E.X) — wrapper d'un widget Qt visible cote OWL.
 *
 * Cree par stdlib_ui::build_control quand une fenetre est ouverte. Pose
 * dans vm.globals[ctrlName] (remplace temporairement la string declarative).
 * Supporte 'IDENT..PropName' via OP_STRUCT_GET (dispatch sur kind+prop).
 *
 * Le widget Qt n'est PAS proprietaire de OWL — sa duree de vie est geree
 * par le parent QMainWindow. ObjUiControl ne fait que tenir le pointer
 * comme handle. Quand la fenetre est fermee, le widget est detruit ; on
 * doit alors restaurer la string dans vm.globals (cf stdlib_ui).
 *
 * 1er champ doit etre Obj (heritage GC standard). */
typedef struct ObjUiControl {
    Obj          obj;
    int          kind;          /* OwlControlKind cast en int (evite include circulaire) */
    void*        widget;        /* OwlControl* opaque (cf owl_ui.h). NULL = headless. */
    char*        name;          /* nom OWL declare ('saiUser', 'btnOK', ...) */
    /*  — storage virtuel des props quand widget == NULL.
     * Materialise au compile-time par le parser fenetre, pope par
     * OP_STRUCT_SET/GET tant qu'aucun widget Qt n'est attache.
     * Pointeur Table : NULL tant qu'aucune prop n'est ecrite (lazy). */
    Table*       headless_store;
} ObjUiControl;

/* ================================================================
 * MULTI-INSTANCE FENETRE ( — infra, pas encore consomme)
 *
 * Sous-ticket #1/4 du chantier multi-instance (parent ). 3
 * nouveaux Obj forment la triade runtime :
 *
 *   - ObjWindowInstance      : instance vivante d'une fenetre (peut
 *                              exister N fois en parallele pour une
 *                              meme decl). Trace son widget Qt opaque
 *                              + son tableau de sous-controles + sa
 *                              valeur de retour (ValeurRenvoyee).
 *
 *   - ObjUiControlInstance   : instance d'un sous-controle, owned par
 *                              une ObjWindowInstance (back-ref). Conserve
 *                              un headless_store comme ObjUiControl pour
 *                              les props ecrites avant attach widget.
 *
 *   - ObjUiControlRef        : proxy global vers un OwlControlDecl pose
 *                              dans vm.globals. Resolution dynamique au
 *                              runtime contre la fenetre top-of-stack
 *                              (vm.window_stack). kind cache evite un
 *                              load de decl->kind sur chaque dispatch.
 *
 * Forward declarations vers ui_decl.h — ui_decl.h inclut object.h en
 * retour. On utilise le tag `struct OwlWindowDecl` (sans typedef alias)
 * pour eviter une redefinition de typedef et conserver la compat avec
 * le typedef defini dans ui_decl.h.
 * ================================================================ */

struct OwlWindowDecl;
struct OwlControlDecl;
struct ObjUiControlInstance;

typedef struct ObjWindowInstance {
    Obj          obj;
    struct OwlWindowDecl* decl;     /* non-const : le decl peut etre mute par
                                     * un live-reload (feat live preview) ; au
                                     * runtime normal il reste immutable. */
    void*        widget;            /* OwlWindow* opaque, NULL en headless */
    int          instance_id;       /* unique au sein du process (monotone) */
    struct ObjUiControlInstance** controls;
    int          control_count;
    int          control_capacity;
    Value        valeur_renvoyee;   /* canon : valeur de retour de Ferme(val) */
    /*  — indice du controle ayant le focus dans controls[] (-1 =
     * aucun). Trace l'etat focus simule en headless ET le focus reel en GUI
     * (source unique pour DonneFocus/SetFocus + focus initial automatique). */
    int          focused_index;
    /*  lot B — casier de proprietes fenetre (miroir du
     * headless_store d'ObjUiControlInstance, ). Lazy : NULL tant
     * qu'aucune prop n'a ete seeded/ecrite. Cle canonisee via
     * owl_window_catalog_find (fallback lowercase ASCII). Source de verite
     * unique pour MaFenetre..Prop = ... / lecture FEN_X..Prop apres
     * modification. */
    Table*       headless_store;
} ObjWindowInstance;

typedef struct ObjUiControlInstance {
    Obj                obj;
    struct OwlControlDecl* decl;
    void*              widget;          /* OwlControl* opaque, NULL en headless */
    Table*             headless_store;  /* lazy, meme convention que ObjUiControl */
    /*  — set de cles (props) ecrites imperativement par code
     * (`..prop =`), distinct du seed/reseed d'expression PROPRIETES. Lazy
     * (NULL tant qu'aucun override). La re-evaluation par rendu de bloc d'un
     * Etat ne re-seede QUE les props ABSENTES de ce set : un override
     * imperatif est sticky quelle que soit sa valeur (plus de temoin par
     * valeur, cf. run_etat_block dans stdlib_pdf.c). Valeur = BOOL_VAL(true)
     * marqueur ; seule la presence de la cle compte. */
    Table*             headless_override_keys;
    ObjWindowInstance* owner;           /* back-ref vers l'instance fenetre */
} ObjUiControlInstance;

typedef struct ObjUiControlRef {
    Obj             obj;
    /*  — `decl` est conserve comme cache de resolution pour la
     * fenetre top-of-stack si elle est en cours de parse (cas exceptionnel
     * — Initialisation ne devrait pas s'auto-monter). En pratique le decl
     * pointe vers une OwlControlDecl freed apres window_parse_subfield
     * (cf. owl_window_decl_add_control qui shallow-copy puis free). La
     * resolution stable se fait via `name` (interne, ObjString stable). */
    struct OwlControlDecl* decl;
    int             kind;           /* cache de decl->kind pour dispatch */
    /*  — nom strippe stable du sous-controle, utilise pour
     * matcher inst->controls[i]->decl->name (lui-meme strippe au parse).
     * NULL = ref inutilisable (defensive). */
    struct ObjString* name;
} ObjUiControlRef;

/*  lot B — proxy global d'une fenetre (miroir d'ObjUiControlRef).
 * Pose au compile-time par window_declaration sous une cle interne distincte
 * (preffixee `__win__`) pour ne pas entrer en conflit avec
 * vm.globals[FEN_X] = ObjUiControl(kind=WINDOW) reinstalle par build_window
 * a chaque Ouvre(). L'alias compile-time `MaFenetre` resout vers cette cle.
 * La resolution runtime tombe sur vm_window_stack_top() (la fenetre courante)
 * — pas besoin d'un nom pour matcher, contrairement aux sous-champs. Le
 * champ `name` est garde pour les messages d'erreur et la symetrie. */
typedef struct ObjWindowRef {
    Obj             obj;
    struct OwlWindowDecl* decl;     /* cache, jamais owned (vit dans vm.windows) */
    struct ObjString* name;         /* nom strippe de la fenetre (diagnostic) */
    /*  lot B — store de repli quand aucune instance n'est sur la
     * stack (FEN_X..Prop = ... ecrit AVANT Ouvre). Lazy. Preserve la
     * semantique historique du ObjUiControl global qui survivait au-dela
     * d'Ouvre/Ferme. Quand une instance est sur la stack, celle-ci prend
     * le relais (le ref ne consulte plus ce store). */
    Table*          fallback_store;
} ObjWindowRef;

/*  lot D — instance runtime d'un Etat (Etats vivants).
 *
 * Miroir strict d'ObjWindowInstance : un decl statique (OwlEtatDecl) devient
 * un objet vivant le temps d'un iImprimeEtat, avec un tableau de sous-champs
 * materialises en ObjUiControlInstance (un par OwlControlDecl de chaque bloc).
 *
 * Push sur vm.etat_stack a l'ouverture (debut fn_i_imprime_etat), pop a la fin
 * (succes ou erreur). resolve_ui_ref consulte vm.etat_stack en repli quand
 * vm.window_stack est vide ou ne resout pas — c'est ce qui permet aux
 * sous-champs d'Etat d'etre adressables par leur nom (LIB_X..Libelle).
 *
 * decl n'est jamais owned (vit dans vm.etats). controls[] sont des
 * ObjUiControlInstance independantes (allouees a iImprimeEtat, GC les
 * collecte apres pop). */
/*  lot D : OwlEtatDecl est un typedef struct anonyme (cf etat_decl.h),
 * pas reachable via `struct OwlEtatDecl*` ici sans inclure le header (cycle :
 * etat_decl.h inclut object.h). On store decl en void* + cast cote stdlib_pdf.c
 * a l'usage. Pas de deref dans object.{c,h}/gc.c/vm.c. */
typedef struct ObjEtatInstance {
    Obj                   obj;
    void*                 decl;          /* OwlEtatDecl*, non-owned, vit dans vm.etats */
    struct ObjUiControlInstance** controls;
    int                   control_count;
    int                   control_capacity;
} ObjEtatInstance;

/* ================================================================
 * ICONE OBJECT ( — Icone first-class)
 *
 * Une Icone est soit une entree du catalogue zstd embarque (identifiee
 * par catalog_hash xxhash3 64-bit, content-addressable), soit un SVG
 * "custom" (chargé depuis disque ou string inline). Les deux cas
 * coexistent : catalog_hash != 0 -> entree catalogue, svg_raw eventuel
 * pour cache decompresse. catalog_hash == 0 -> custom, svg_raw porte
 * obligatoirement le contenu SVG.
 *
 * Props mutables runtime : Couleur (apply au render via QPainter),
 * Rotation (degrees libres), Forme (constante formeXxx). couleur_modifiable
 * est derive du SVG (heuristique : tous fill/stroke = currentColor).
 * valide est false si chargement SVG custom KO. Couleur = -1 = unset.
 *
 * Lot 2b : structure + GC. La logique de resolution catalogue, le rendu
 * Qt et l'heuristique CouleurModifiable arrivent en 2d/2e.
 * ================================================================ */
typedef struct ObjIcone {
    Obj         obj;
    uint64_t    catalog_hash;       /* xxhash3 SVG body, 0 = pas catalogue (custom) */
    ObjString*  svg_raw;            /* SVG raw (custom) OU cache decompresse (catalogue). NULL autorise. */
    int64_t     couleur;            /* -1 = unset, sinon RGB packed (compat Couleur canon) */
    int64_t     couleur_fond;       /*  — -1 = transparent (defaut), sinon RGB. Visible si forme != formeAucune. */
    int         rotation;           /* degres (0..359), default 0 */
    int         forme;              /* enum formeXxx, default 0 = formeAucune */
    bool        couleur_modifiable; /* derive heuristique SVG, RO cote OWL */
    bool        valide;             /* false si chargement SVG custom KO */
} ObjIcone;

/*  Lot 2e-1 — Namespace singleton expose globalement sous les
 * noms `icones` (FR) et `icons` (EN). N'a aucun champ propre : sert
 * uniquement de cible pour OP_STRUCT_GET, qui dispatche vers le catalogue
 * (stdlib_icones) et construit un ObjIcone neuf a la volee.
 *
 * Unique instance par VM (alloue dans stdlib_icones_init, ancrage GC via
 * vm.globals). Le type a son propre tag pour que la branche dispatch dans
 * vm.c soit O(1) sans pattern matching de chaine sur l'identifiant. */
typedef struct ObjIconesNamespace {
    Obj obj;
} ObjIconesNamespace;

/*  Lot 2b+3 — Namespace singleton expose globalement sous les
 * noms `theme` (FR) et `Theme` (EN). Comme ObjIconesNamespace, aucun champ
 * propre : sert uniquement de cible pour OP_STRUCT_GET. Le dispatch (vm.c)
 * resout `theme.<nom>` vers l'instance de style nommee declaree par
 * l'utilisateur (globals). L'accent (`theme.<nom>` vs read strippe) est
 * gere par la resolution canon des globals (casse + accents insensibles). */
typedef struct ObjThemeNamespace {
    Obj obj;
} ObjThemeNamespace;

/* --- Type checks on Values --- */
#define OBJ_TYPE(value)     (AS_OBJ(value)->type)
#define IS_STRING(value)    (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_STRING)
#define IS_BUFFER(value)    (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_BUFFER)
#define IS_NUMERIC(value)   (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_NUMERIC)
/* Tous les types numeriques scalaires + le bignum heap. */
#define IS_NUMBER(value)    (IS_INTEGER(value) || IS_REAL(value) || \
                             IS_CURRENCY(value) || IS_NUMERIC(value))
/* Covers both chaine and buffer: binary-safe byte-string-like values. */
#define IS_BYTES(value)     (IS_STRING(value) || IS_BUFFER(value))
#define IS_FUNCTION(value)  (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_FUNCTION)
#define IS_NATIVE(value)    (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_NATIVE)
#define IS_ARRAY(value)      (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ARRAY)
#define IS_ASSOC_ARRAY(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ASSOC_ARRAY)
#define IS_INSTANCE(value)   (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_INSTANCE)
#define IS_CLASS(value)      (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_CLASS)
#define IS_COLLECTION(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_COLLECTION)
#define IS_JSON(value)       (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_JSON)
#define IS_PROC_CHAIN(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_PROC_CHAIN)
#define IS_ZIP_ARCHIVE(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ZIP_ARCHIVE)
#define IS_MOCK_NATIVE(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MOCK_NATIVE)
#define IS_PATTERN(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_PATTERN)
#define IS_UI_CONTROL(value)  (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_UI_CONTROL)
#define IS_MULTILINGUE(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MULTILINGUE)
#define IS_WINDOW_INSTANCE(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_WINDOW_INSTANCE)
#define IS_UI_CONTROL_INSTANCE(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_UI_CONTROL_INSTANCE)
#define IS_UI_CONTROL_REF(value)      (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_UI_CONTROL_REF)
#define IS_WINDOW_REF(value)          (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_WINDOW_REF)
#define IS_ETAT_INSTANCE(value)       (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ETAT_INSTANCE)
#define IS_ICONE(value)               (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ICONE)
#define IS_ICONES_NAMESPACE(value)    (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_ICONES_NAMESPACE)
#define IS_THEME_NAMESPACE(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_THEME_NAMESPACE)
#define IS_VECTOR(value)              (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_VECTOR)
#define IS_MATRIX(value)              (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MATRIX)
#define IS_MATRIX_ROW(value)          (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MATRIX_ROW)
#define IS_TENSOR(value)              (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_TENSOR)
#define IS_TENSOR_VIEW(value)         (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_TENSOR_VIEW)
#define IS_IAMODELE(value)            (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_IAMODELE)
#define IS_OPTIONS_ENTRAINEMENT(value)(IS_OBJ(value) && OBJ_TYPE(value) == OBJ_OPTIONS_ENTRAINEMENT)
#define IS_LLM(value)                 (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_LLM)
#define IS_OPTIONS_GENERATION(value)  (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_OPTIONS_GENERATION)
#define IS_MESSAGE_LLM(value)         (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MESSAGE_LLM)
#define IS_RESPONSE_LLM(value)        (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_RESPONSE_LLM)
#define IS_DETECTION(value)           (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_DETECTION)
#define IS_MLARBRE(value)             (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLARBRE)
#define IS_MLFORET(value)             (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLFORET)
#define IS_MLBOOSTING(value)          (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLBOOSTING)
#define IS_MLLINEAIRE(value)          (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLLINEAIRE)
#define IS_MLKMEANS(value)            (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLKMEANS)
#define IS_MLLOGISTIQUE(value)        (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_MLLOGISTIQUE)
#define IS_IMAGE_ETIQUETEE(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_IMAGE_ETIQUETEE)
#define IS_IMAGE_ANNOTEE(value)       (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_IMAGE_ANNOTEE)
#define IS_DATASET(value)             (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_DATASET)
#define IS_XLS_DOCUMENT(value)        (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_XLS_DOCUMENT)
#define IS_XLS_REF(value)             (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_XLS_REF)
#define IS_CALLABLE(value)   (IS_FUNCTION(value) || IS_NATIVE(value) || IS_PROC_CHAIN(value))

/* --- Extract object pointers --- */
#define AS_STRING(value)    ((ObjString*)AS_OBJ(value))
#define AS_CSTRING(value)   (((ObjString*)AS_OBJ(value))->chars)
#define AS_BUFFER(value)    ((ObjBuffer*)AS_OBJ(value))
#define AS_NUMERIC(value)   ((ObjNumeric*)AS_OBJ(value))
#define AS_FUNCTION(value)  ((ObjFunction*)AS_OBJ(value))
#define AS_NATIVE(value)    (((ObjNative*)AS_OBJ(value))->function)
#define AS_ARRAY(value)     ((ObjArray*)AS_OBJ(value))
#define AS_ASSOC_ARRAY(value) ((ObjAssocArray*)AS_OBJ(value))
#define AS_INSTANCE(value)   ((ObjInstance*)AS_OBJ(value))
#define AS_CLASS(value)      ((ObjClass*)AS_OBJ(value))
#define AS_COLLECTION(value) ((ObjCollection*)AS_OBJ(value))
#define AS_JSON(value)       ((ObjJson*)AS_OBJ(value))
#define AS_PROC_CHAIN(value) ((ObjProcChain*)AS_OBJ(value))
#define AS_UI_CONTROL(value) ((ObjUiControl*)AS_OBJ(value))
#define AS_ZIP_ARCHIVE(value) ((ObjZipArchive*)AS_OBJ(value))
#define AS_MOCK_NATIVE(value) ((ObjMockNative*)AS_OBJ(value))
#define AS_PATTERN(value)     ((ObjPattern*)AS_OBJ(value))
#define AS_MULTILINGUE(value) ((ObjMultilingue*)AS_OBJ(value))
#define AS_WINDOW_INSTANCE(value)     ((ObjWindowInstance*)AS_OBJ(value))
#define AS_UI_CONTROL_INSTANCE(value) ((ObjUiControlInstance*)AS_OBJ(value))
#define AS_UI_CONTROL_REF(value)      ((ObjUiControlRef*)AS_OBJ(value))
#define AS_WINDOW_REF(value)          ((ObjWindowRef*)AS_OBJ(value))
#define AS_ETAT_INSTANCE(value)       ((ObjEtatInstance*)AS_OBJ(value))
#define AS_ICONE(value)               ((ObjIcone*)AS_OBJ(value))
#define AS_ICONES_NAMESPACE(value)    ((ObjIconesNamespace*)AS_OBJ(value))
#define AS_THEME_NAMESPACE(value)     ((ObjThemeNamespace*)AS_OBJ(value))
#define AS_VECTOR(value)              ((ObjVector*)AS_OBJ(value))
#define AS_MATRIX(value)              ((ObjMatrix*)AS_OBJ(value))
#define AS_MATRIX_ROW(value)          ((ObjMatrixRow*)AS_OBJ(value))
#define AS_TENSOR(value)              ((ObjTensor*)AS_OBJ(value))
#define AS_TENSOR_VIEW(value)         ((ObjTensorView*)AS_OBJ(value))
#define AS_IAMODELE(value)            ((ObjIAModele*)AS_OBJ(value))
#define AS_MLARBRE(value)             ((ObjMLArbre*)AS_OBJ(value))
#define AS_MLFORET(value)             ((ObjMLForet*)AS_OBJ(value))
#define AS_MLBOOSTING(value)          ((ObjMLBoosting*)AS_OBJ(value))
#define AS_MLLINEAIRE(value)          ((ObjMLLineaire*)AS_OBJ(value))
#define AS_MLLOGISTIQUE(value)        ((ObjMLLogistique*)AS_OBJ(value))
#define AS_MLKMEANS(value)            ((ObjMLKMeans*)AS_OBJ(value))
#define AS_OPTIONS_ENTRAINEMENT(value)((ObjOptionsEntrainement*)AS_OBJ(value))
#define AS_LLM(value)                 ((ObjLLM*)AS_OBJ(value))
#define AS_OPTIONS_GENERATION(value)  ((ObjOptionsGeneration*)AS_OBJ(value))
#define AS_MESSAGE_LLM(value)         ((ObjMessageLLM*)AS_OBJ(value))
#define AS_RESPONSE_LLM(value)        ((ObjResponseLLM*)AS_OBJ(value))
#define AS_DETECTION(value)           ((ObjDetection*)AS_OBJ(value))
#define AS_IMAGE_ETIQUETEE(value)     ((ObjImageEtiquetee*)AS_OBJ(value))
#define AS_IMAGE_ANNOTEE(value)       ((ObjImageAnnotee*)AS_OBJ(value))
#define AS_DATASET(value)             ((ObjDataset*)AS_OBJ(value))
#define AS_XLS_DOCUMENT(value)        ((ObjXlsDocument*)AS_OBJ(value))
#define AS_XLS_REF(value)             ((ObjXlsRef*)AS_OBJ(value))

/* ================================================================
 * OBJECT FUNCTIONS
 * ================================================================ */

/* String creation - takes ownership of chars or copies */
ObjString* obj_copy_string(const char* chars, int length);
ObjString* obj_take_string(char* chars, int length);

/* Variants for `<indétectable>` strings: bypass interning, store the
 * chars buffer XOR-masked with the per-process key. */
ObjString* obj_copy_string_undetectable(const char* chars, int length);
ObjString* obj_take_string_undetectable(char* chars, int length);

/* Initialize the per-process undetectable XOR key. Called from vm_init. */
void obj_init_mask_key(void);

/* Allocate a fresh cleartext copy of a string (handles both undetectable
 * and plain). Caller must release via obj_string_secure_free(). */
char* obj_string_unmask_dup(const ObjString* s);

/* Securely zero then free a buffer returned by obj_string_unmask_dup. */
void obj_string_secure_free(char* buf, int len);

/* Concatenate two strings */
ObjString* obj_concat_strings(ObjString* a, ObjString* b);

/* Promote an interned (immutable) string to a fresh mutable buffer with
 * extra capacity reserved for future appends. Used by OP_APPEND_IN_PLACE
 * the first time a slot transitions into "string builder" mode. */
ObjString* obj_string_to_buffer(ObjString* s);

/* Numeric (bignum decimal) creation. obj_new_numeric alloue un ObjNumeric
 * dont le dec (mpd_t*) vaut 0. Les helpers numeric_from_* (value.h) sont la
 * voie normale de construction depuis le runtime. */
ObjNumeric* obj_new_numeric(void);

/* Coercion uniforme vers double : tout nombre -> double. Le bignum passe par
 * numeric_to_double (perte de precision assumee au-dela de ~17 chiffres). */
#define AS_NUMBER(value) \
    (IS_INTEGER(value)  ? (double)AS_INTEGER(value) : \
     IS_CURRENCY(value) ? (double)AS_CURRENCY(value) / 1000000.0 : \
     IS_NUMERIC(value)  ? numeric_to_double(value) : \
     AS_REAL(value))

/* Buffer creation. obj_new_buffer copies the bytes; obj_buffer_empty allocates
 * a zero-length buffer with optional initial capacity. */
ObjBuffer* obj_new_buffer(const void* bytes, int length);
ObjBuffer* obj_buffer_empty(int initial_capacity);
/* Append bytes to an existing buffer (grows as needed). */
void obj_buffer_append(ObjBuffer* buf, const void* data, int len);

/* Zip archive creation / teardown. Allocated in the unopened state;
 * the caller (format backend in stdlib_compression*) initialises
 * `backend` and flips `open = true`. obj_zip_archive_close is
 * idempotent and called both by explicit `zipFerme` and by the GC on
 * sweep. It dispatches on `format` to release the backend correctly. */
ObjZipArchive* obj_new_zip_archive(ZipArchiveFormat fmt, const char* path,
                                   ZipArchiveMode mode);
void obj_zip_archive_close(ObjZipArchive* z);

/* Register a per-format close callback. Called once at stdlib init by
 * each backend (compress_zip.c, compress_tar.c, compress_7z.c). The
 * callback must release the backend resources but NOT free() the
 * ObjZipArchive struct itself — the GC handles that. */
typedef void (*ZipBackendCloseFn)(ObjZipArchive*);
void owl_zip_register_backend_close(int format, ZipBackendCloseFn fn);
/* Get bytes + length from a chaine OR buffer. Returns 1 if value is a byte-string,
 * 0 otherwise. out_bytes/out_len are filled only on success. */
int  value_as_bytes(Value v, const unsigned char** out_bytes, int* out_len);

/* Function creation */
ObjFunction* obj_new_function(void);

/*  — enregistre un nom de locale/parametre -> slot dans la table de
 * debug info de la fonction. `name`/`length` pointent dans le source (copies
 * en interne via obj_copy_string, accents strippes pour le matching canon). Pas
 * de dedup : le compiler garantit l'unicite (OWL interdit l'ombrage et la
 * redeclaration homonyme dans une meme procedure). */
void obj_function_add_local_name(ObjFunction* fn, const char* name, int length,
                                 int slot, int ip_start);

/*  /  — resout un nom (accent/casse-insensible) dans la table.
 * Retourne le slot, ou -1 si absent. `ip` filtre sur la plage de validite du
 * nom (offset bytecode du point de pause) pour ne pas resoudre un nom hors
 * scope qui partage un slot recycle ; ip < 0 = pas de filtre (match permissif). */
int obj_function_resolve_local(const ObjFunction* fn, const char* name,
                               int length, int ip);

/*  /  — nom canonique de la locale a un slot donne, valide
 * a l'IP de pause donne (offset bytecode dans le chunk de la fonction).
 * Filtre sur la plage de validite [ip_start, ip_end) pour ne pas renvoyer le
 * nom d'un scope deja sorti qui partageait le slot. NULL si absent. */
const char* obj_function_local_name_at(const ObjFunction* fn, int slot, int ip);

/*  /  — ferme les plages de validite (ip_end) des noms de
 * locales encore ouverts pour un scope qui se termine. Appele par le compiler
 * a end_scope avec l'offset bytecode courant et le slot le plus bas du scope
 * sortant : tout nom ouvert dont le slot >= fromSlot voit son ip_end fixe. */
void obj_function_close_local_names(ObjFunction* fn, int fromSlot, int ip_end);

/* Native function creation */
ObjNative* obj_new_native(NativeFn function, int arity);

/* Mock native creation (for Mock system) */
ObjMockNative* obj_new_mock_native(int mockIndex);

/* Pattern creation (for Mock argument matching) */
ObjPattern* obj_new_pattern(int patternType, Value value);

/* Phase E.X : UI control wrapper. kind = OwlControlKind cast en int. */
ObjUiControl* obj_new_ui_control(int kind, void* widget, const char* name);

/*  — headless store helpers.
 * get : retourne true + remplit *out si la prop a ete ecrite. false sinon
 * (caller calcule un default).
 * set : ecrit, alloue la Table lazy si NULL. Toujours true.
 * Tous deux case-insensitive sur le nom (lookup via ObjString cle interne). */
bool obj_ui_control_headless_get(ObjUiControl* c, const char* prop, Value* out);
void obj_ui_control_headless_set(ObjUiControl* c, const char* prop, Value v);
/*  — pendants ObjUiControlInstance (modele C ). */
bool obj_ui_control_instance_headless_get(ObjUiControlInstance* c,
                                          const char* prop, Value* out);
void obj_ui_control_instance_headless_set(ObjUiControlInstance* c,
                                          const char* prop, Value v);
/*  — marque/interroge le set des props ecrites imperativement
 * (`..prop =`). mark : appele APRES un headless_set imperatif (site vm.c),
 * jamais par le seed/reseed d'expression. is : consulte par run_etat_block
 * pour ne pas re-seeder une prop overridee (sticky, cle-based, pas valeur). */
void obj_ui_control_instance_mark_override(ObjUiControlInstance* c,
                                           const char* prop);
bool obj_ui_control_instance_is_override(ObjUiControlInstance* c,
                                         const char* prop);

/*  lot B — pendants ObjWindowInstance. Cle canonisee via
 * owl_window_catalog_find (fallback lowercase ASCII si prop hors-catalog). */
bool obj_window_instance_headless_get(ObjWindowInstance* w,
                                      const char* prop, Value* out);
void obj_window_instance_headless_set(ObjWindowInstance* w,
                                      const char* prop, Value v);

/*  — allocators triade multi-instance. Aucun consommateur a ce
 * stade (infra dormante). Les /0136 cableront ces objets sur
 * les natives Ouvre/OuvreFille/OuvreSoeur. */
ObjWindowInstance*    obj_new_window_instance(struct OwlWindowDecl* decl);
ObjUiControlInstance* obj_new_ui_control_instance(struct OwlControlDecl* decl,
                                                   ObjWindowInstance* owner);
ObjUiControlRef*      obj_new_ui_control_ref(struct OwlControlDecl* decl);
/*  — variante avec capture de nom strippe pour resolution stable. */
ObjUiControlRef*      obj_new_ui_control_ref_named(struct OwlControlDecl* decl,
                                                    ObjString* name);
/*  lot B — proxy global de fenetre. Pose une seule fois par
 * window_declaration (compile-time), survit aux re-Ouvre. */
ObjWindowRef*         obj_new_window_ref(struct OwlWindowDecl* decl,
                                          ObjString* name);
/*  — Helper d'append d'instance dans window->controls[]. */
void obj_window_instance_add_control(ObjWindowInstance* inst,
                                      ObjUiControlInstance* uici);

/*  lot D — allocator + helper d'append pour ObjEtatInstance.
 * `decl` est typo OwlEtatDecl* (void* pour eviter le cycle etat_decl<->object). */
ObjEtatInstance* obj_new_etat_instance(void* decl);
void obj_etat_instance_add_control(ObjEtatInstance* inst,
                                    ObjUiControlInstance* uici);

/*  — Icone first-class. Lot 2b : allocator vierge.
 * catalog_hash == 0 + svg_raw == NULL = etat indetermine (sera renseigne
 * par la couche resolution compiler ou la stdlib_icones en Lot 2d/2e).
 * Les props sont initialisees au defaut : couleur=-1 (unset), rotation=0,
 * forme=0 (formeAucune), couleur_modifiable=false, valide=true. */
ObjIcone* obj_new_icone(void);

/*  Lot 2e-1 — allocator du singleton namespace `icones`. Appele
 * une seule fois depuis stdlib_icones_init. Pas d'etat interne : la
 * resolution se fait via le catalogue de stdlib_icones. */
ObjIconesNamespace* obj_new_icones_namespace(void);

/*  Lot 2b+3 — allocator du singleton namespace `theme`. Appele une
 * seule fois depuis stdlib_theme_init. Pas d'etat interne : la resolution
 * `theme.<nom>` se fait via les globals (styles nommes declares par
 * l'utilisateur dans le fichier designe par Projet..Theme). */
ObjThemeNamespace* obj_new_theme_namespace(void);

/* Array creation */
ObjArray* obj_new_array(void);

/*  — xlsDocument creation. Alloue l'enveloppe GC + une premiere
 * feuille vide ("Feuil1"). Les feuilles/cellules/fusions sont du C-heap pur
 * (aucune Value OWL) libere par le finalizer GC. */
ObjXlsDocument* obj_new_xls_document(void);
ObjXlsRef*      obj_new_xls_ref(ObjXlsDocument* doc, XlsRefKind kind);

/*  — Vecteur creation. Alloue l'enveloppe GC + un double[] de
 * `count` elements (malloc, zero-initialise). count <= 0 -> vecteur vide
 * (data == NULL). Le caller remplit ->data[] apres coup (pas d'allocation
 * OWL intermediaire, donc pas de rooting a prevoir pour le remplissage). */
ObjVector* obj_new_vector(int count);

/*  — Matrice creation. Alloue l'enveloppe GC + un double[] de
 * `rows*cols` elements row-major (malloc, zero-initialise). Le caller remplit
 * ->data[] apres coup (pas d'allocation OWL intermediaire). Une vue de ligne
 * (obj_new_matrix_row) reference `mat` — le caller garde `mat` roote pendant
 * l'alloc de la vue (peek de la pile VM au moment du chainage `m[i]`). */
ObjMatrix* obj_new_matrix(int rows, int cols);
ObjMatrixRow* obj_new_matrix_row(ObjMatrix* mat, int row);

/*  — Tenseur creation (feuille). Alloue l'enveloppe GC puis, en la
 * gardant rootee sur la pile VM, deux Vecteur de `count` elements (donnee +
 * gradient, tous deux zero-init). op = TENSOR_OP_LEAF, aucun parent. Le
 * caller remplit ->data->data[] apres coup (pas d'allocation OWL
 * intermediaire). */
ObjTensor* obj_new_tensor(int count);

/*  — Tenseur feuille 2D : donnee + gradient stockes dans deux
 * ObjMatrix `rows`×`cols` (zero-init). Meme discipline de rooting que
 * obj_new_tensor. Le caller remplit ->data (via tensor_cells) apres coup. */
ObjTensor* obj_new_tensor_matrix(int rows, int cols);

/*  — Tenseur feuille de rang arbitraire (>= 1) decrit par `shape`
 * (ndim tailles). Le stockage est un buffer plat contigu de produit(shape)
 * elements : ObjMatrix si ndim == 2 (preserve les kernels rapides et le rendu
 * Matrice), ObjVector sinon. ndim/shape sont poses sur l'enveloppe. Meme
 * discipline de rooting que obj_new_tensor. Le caller remplit ->data apres coup
 * (via tensor_cells) — l'ordre plat est row-major (dernier axe le plus rapide). */
ObjTensor* obj_new_tensor_nd(int ndim, const int* shape);

/*  — vue transitoire d'une sous-tranche de Tenseur N-D. Reference
 * `backing` (le buffer plat), garde par le caller pendant l'alloc (peek pile).*/
ObjTensorView* obj_new_tensor_view(Obj* backing, int offset, int ndim,
                                   const int* shape, const int* strides);

/*  — Tenseur resultat d'une op differentiable binaire (`+` ou `*`).
 * Calcule le forward element-par-element et enregistre op + parents. `a` et
 * `b` DOIVENT etre rootes par le caller (sur la pile VM) pendant l'appel :
 * obj_new_tensor y alloue. Cas special de `+` : broadcast d'un biais 1D
 * (taille n) sur un Tenseur 2D (B×n), sortie 2D (B×n). Rend NULL si les
 * tailles sont incompatibles (l'op element-par-element est indefinie). */
ObjTensor* obj_tensor_binop(TensorOp op, ObjTensor* a, ObjTensor* b);

/*  — noeud CONCAT_CH differentiable : concatene `a` et `b` sur l'axe
 * canal (index 1 de [N,C,...]). `a` et `b` DOIVENT etre rootes par le caller
 * (obj_new_tensor_nd y alloue). Precondition : meme rang (>= 2) et memes tailles
 * sur tous les axes SAUF l'axe 1 (seuls les canaux different). Sortie de forme
 * identique avec shape[1] = a.shape[1] + b.shape[1]. Rend NULL si les formes
 * sont incompatibles (le caller leve alors une erreur claire). Backward = split
 * du gradient par tranche de canaux. */
ObjTensor* obj_tensor_concat_ch(ObjTensor* a, ObjTensor* b);

/*  — tranche de canaux : rend [N, ccount, ...spatial] copie des canaux
 * [cstart, cstart+ccount) de `src` (axe 1). `src` DOIT etre roote par le caller
 * (obj_new_tensor_nd y alloue). Noeud SLICE_CH differentiable (op_axis = cstart),
 * inverse du CONCAT_CH. Rend NULL si bornes invalides (rang < 2, tranche hors
 * canaux). Sert le nœud Split de l'import ONNX. */
ObjTensor* obj_tensor_slice_ch(ObjTensor* src, int cstart, int ccount);

/*  — sous-Tenseur first-class : rend la tranche `index1` (base 1) le
 * long de l'axe 0 de `src`, un Tenseur de rang N-1 (copie contigue). `src` DOIT
 * etre roote par le caller (obj_new_tensor_nd y est l'unique alloc OWL). Noeud
 * SELECT differentiable (op_axis = index 0-based), backward = scatter du gradient
 * dans la tranche source, 0 ailleurs. Rend NULL si rang < 2 ou index hors
 * [1, forme[0]] (le caller leve alors une exception rattrapable). Distinct de
 * l'acces brut ..Valeur[i] (vue transitoire, non stockable). */
ObjTensor* obj_tensor_select(ObjTensor* src, int index1);

/*  — upsample spatial nearest-neighbor (asymmetric/floor) par facteurs
 * entiers sy,sx sur les 2 derniers axes : rend [.., H*sy, W*sx] avec
 * out[..,y,x] = src[.., y/sy, x/sx] (division entiere). `src` DOIT etre roote par
 * le caller (obj_new_tensor_nd y alloue). Forward-only (feuille) : les graphes
 * ONNX importes sont en inference seule, pas de retropropagation a
 * travers un Resize. Rend NULL si rang < 2 ou facteur <= 0. Sert le nœud Resize
 * de l'import ONNX (cou FPN des detecteurs). */
ObjTensor* obj_tensor_upsample_nearest(ObjTensor* src, int sy, int sx);

/*  — ops tenseur de la tete DFL des detecteurs, forward-only (feuilles :
 * un graphe ONNX importe est en inference seule, cf. ). `src`/`a`/`b`
 * DOIVENT etre rootes par le caller (obj_new_tensor_nd y est l'unique alloc). */

/* Transpose general : `perm[i]` = axe source (0-based) de l'axe i de sortie.
 * `perm` couvre les src->ndim axes (permutation valide). Rend NULL si rang hors
 * bornes. */
ObjTensor* obj_tensor_transpose(ObjTensor* src, const int* perm);

/* Softmax numeriquement stable (soustraction du max le long de l'axe avant exp)
 * sur `axis` (0-based, negatif compte depuis la fin). Rend NULL si axis hors
 * bornes. */
ObjTensor* obj_tensor_softmax(ObjTensor* src, int axis);

/* Slice ONNX : pour chaque i, l'axe axes[i] (0-based, negatif accepte) est
 * tranche de starts[i] a ends[i] (exclu) au pas steps[i] (>= 1). Indices
 * negatifs comptes depuis la fin, bornes clampees. Rend NULL si un pas <= 0
 * (pas negatif hors perimetre) ou un axe hors bornes. */
ObjTensor* obj_tensor_slice(ObjTensor* src, int nax, const int* axes,
                            const int* starts, const int* ends, const int* steps);

/* Concatenation de `a` et `b` sur `axis` (0-based, negatif accepte) quelconque :
 * generalise obj_tensor_concat_ch. Memes tailles sur tous les axes sauf `axis`.
 * Rend NULL si formes incompatibles. */
ObjTensor* obj_tensor_concat_axis(ObjTensor* a, ObjTensor* b, int axis);

/* Element-par-element avec diffusion (broadcast numpy/torch, alignement par la
 * droite) : `op` est un IAEltwiseOp (Add/Sub/Mul/Div). Rend NULL si les formes
 * sont incompatibles. */
ObjTensor* obj_tensor_eltwise(int op, ObjTensor* a, ObjTensor* b);

/*  — noeud RESHAPE differentiable (meme donnee, meme ordre plat,
 * nouvelle forme). `src` DOIT etre roote par le caller (obj_new_tensor_nd y
 * alloue) ; le resultat n'est pas roote (a rooter par le caller avant toute
 * autre allocation). Backward = copie plate du gradient (RESHAPE existant). */
ObjTensor* obj_tensor_reshape(ObjTensor* src, int ndim, const int* shape);

/*  — forward complet de la normalisation par lot, assemble en graphe
 * (noeud normalize-only + gamma/beta reshapes + MUL/ADD), avec gestion du mode
 * et des statistiques mobiles. `x`, `gamma`, `beta` DOIVENT etre rootes par le
 * caller. `runMean`/`runVar` (par canal, taille C) peuvent etre NULL :
 *   - eval=false, runMean NULL : normalise avec les stats du LOT (chemin de la
 *     native publique `TenseurNormalisationLot`, comportement historique).
 *   - eval=false, runMean non-NULL : stats du lot POUR normaliser + mise a jour
 *     EMA (momentum) des stats mobiles EN PLACE (mode entrainement d'une couche).
 *   - eval=true, runMean non-NULL : normalise avec les stats MOBILES (mode
 *     inference d'une couche) ; le noeud xhat reste une feuille (pas de backward).
 * Rend NULL (avec erreur emise) sur forme invalide. */
ObjTensor* tensor_batchnorm_forward(ObjTensor* x, ObjTensor* gamma, ObjTensor* beta,
                                    double eps, ObjTensor* runMean, ObjTensor* runVar,
                                    double momentum, bool eval);

/*  — IAModèle vide (aucune couche). Enveloppe GC pure sans
 * allocation OWL secondaire : pas de rooting requis a la creation. Le
 * tableau `layers` est alloue paresseusement au premier ajout. */
ObjIAModele* obj_new_iamodele(void);

/*  — MLArbreDécision neuf : objectif non réglé (0), hyperparamètres
 * aux défauts (ProfondeurMax 0 = illimité, MinÉchantillonsSéparation 2,
 * MinÉchantillonsFeuille 1), arbre vide (root NULL). Feuille GC pure : aucune
 * allocation OWL secondaire, pas de rooting requis à la création. */
ObjMLArbre* obj_new_mlarbre(void);

/*  — libère récursivement l'arbre C-heap (appelé au sweep et au
 * ré-entraînement). NULL-safe. */
void obj_mlarbre_free_tree(MLTreeNode* node);

/*  — MLForêtAléatoire neuve : objectif non réglé (0), hyperparamètres
 * aux défauts (NbArbres 100, VariablesParSéparation 0 = auto, Graine 0 = fixe,
 * ProfondeurMax 0, MinÉchantillonsSéparation 2, MinÉchantillonsFeuille 1),
 * ensemble vide. Feuille GC pure : aucune allocation OWL secondaire. */
ObjMLForet* obj_new_mlforet(void);

/*  — libère le tableau d'arbres C-heap de la forêt (chaque arbre via
 * obj_mlarbre_free_tree) puis le tableau lui-même. NULL-safe. */
void obj_mlforet_free_trees(ObjMLForet* f);

/*  — MLGradientBoosting neuf : objectif non réglé (0), hyperparamètres
 * aux défauts (NbArbres 100, TauxApprentissage 0.1, ProfondeurMax 3,
 * MinÉchantillonsSéparation 2, MinÉchantillonsFeuille 1), ensemble vide.
 * Feuille GC pure : aucune allocation OWL secondaire. */
ObjMLBoosting* obj_new_mlboosting(void);

/*  — libère le tableau d'arbres C-heap du boosting (chaque arbre via
 * obj_mlarbre_free_tree), le tableau lui-même et les scores initiaux. NULL-safe. */
void obj_mlboosting_free_trees(ObjMLBoosting* b);

/*  — MLRégressionLinéaire neuve : non entraînée (coef NULL, trained
 * false). Feuille GC pure : aucune allocation OWL secondaire. */
ObjMLLineaire* obj_new_mllineaire(void);

/*  — libère les poids C-heap de la régression linéaire. NULL-safe. */
void obj_mllineaire_free_weights(ObjMLLineaire* r);

/*  — MLRégressionLogistique neuve : hyperparamètres aux défauts
 * (TauxApprentissage 0.1, NbItérations 500), non entraînée (W/bias NULL).
 * Feuille GC pure : aucune allocation OWL secondaire. */
ObjMLLogistique* obj_new_mllogistique(void);

/*  — libère les poids C-heap (W et bias) de la régression logistique.
 * NULL-safe. */
void obj_mllogistique_free_weights(ObjMLLogistique* r);

/*  — MLKMeans neuf : hyperparamètres aux défauts (NbClusters 8,
 * NbItérations 300, Graine 0), non entraîné (centroids NULL). Feuille GC pure :
 * aucune allocation OWL secondaire. */
ObjMLKMeans* obj_new_mlkmeans(void);

/*  — libère les centroïdes C-heap du modèle k-means. NULL-safe. */
void obj_mlkmeans_free_centroids(ObjMLKMeans* k);

/*  — empile une couche. `weights`/`bias` non-NULL pour une couche
 * dense (ils DOIVENT etre rootes par le caller pendant l'appel car le
 * realloc du tableau est C-heap mais le modele doit deja etre roote), NULL
 * pour une activation. Croissance geometrique du tableau C-heap ; aucune
 * allocation OWL, donc pas de cycle GC declenche par cet appel. */
void obj_iamodele_push_layer(ObjIAModele* m, IALayerKind kind,
                             ObjTensor* weights, ObjTensor* bias);

/*  — OptionsEntraînement avec les defauts raisonnables (1 epoque,
 * lot de 32, taux 0.01, optimiseur SGD, perte entropie croisee). Le champ
 * onEpoch est init a NIL_VAL : feuille GC tant que non affecte,
 * non-feuille des qu'une reference de procedure y est posee (tracee dans gc.c). */
ObjOptionsEntrainement* obj_new_options_entrainement(void);

/*  — ImageÉtiquetée : capture chemin + etiquette (deux ObjString qui
 * DOIVENT etre rootes par le caller pendant l'appel — l'alloc de l'enveloppe
 * peut declencher un GC). Le champ image reste NULL : materialise
 * paresseusement au premier acces de `..Image` (cote VM). NON-feuille GC. */
ObjImageEtiquetee* obj_new_image_etiquetee(ObjString* path, ObjString* label);

/*  — ImageAnnotée : capture chemin + tableau de boites (deux Obj qui
 * DOIVENT etre rootes par le caller pendant l'appel — l'alloc de l'enveloppe
 * peut declencher un GC). Le champ image reste NULL : materialise
 * paresseusement au premier acces de `..Image` (cote VM). NON-feuille GC. */
ObjImageAnnotee* obj_new_image_annotee(ObjString* path, Obj* boxes);

/*  — Dataset vide. Enveloppe GC pure : les tableaux C-heap `items` et
 * `classes` sont alloues paresseusement par les helpers push. `source` init a
 * NULL (pose par le chargeur). `itemType` init a OBJ_IMAGE_ETIQUETEE (pose par
 * le chargeur). Pas de rooting requis a la creation. */
ObjDataset* obj_new_dataset(void);

/*  — ajoute un exemple au Dataset (croissance geometrique du tableau
 * C-heap). `item` DOIT etre roote par le caller (le dataset aussi), aucune
 * allocation OWL declenchee ici. Le type d'exemple est homogene (cf.
 * ds->itemType) : c'est le chargeur qui garantit la coherence. */
void obj_dataset_push_item(ObjDataset* ds, Obj* item);

/*  — ajoute un nom de classe (croissance geometrique). `name` DOIT
 * etre roote par le caller. Aucune allocation OWL. */
void obj_dataset_push_class(ObjDataset* ds, ObjString* name);

/*  — materialise (paresseusement) l'Image d'un ImageÉtiquetée : la
 * charge depuis le disque au premier appel, la met en cache dans `ie->image`,
 * et rend l'instance Image. Rend NULL si le fichier est absent/illisible/format
 * invalide (le caller VM leve alors une exception rattrapable). `ie` DOIT etre
 * roote par le caller pendant l'appel (une allocation Image peut declencher un
 * GC). Impl dans stdlib_dataset.c (pont vers le backend image). */
ObjInstance* obj_image_etiquetee_materialize(ObjImageEtiquetee* ie);

/*  — materialise (paresseusement) l'Image d'un ImageAnnotée : miroir
 * de obj_image_etiquetee_materialize (charge au premier appel, cache dans
 * `ia->image`, rend l'instance Image ; NULL si fichier absent/illisible). `ia`
 * DOIT etre roote par le caller pendant l'appel. Impl dans stdlib_dataset.c. */
ObjInstance* obj_image_annotee_materialize(ObjImageAnnotee* ia);

/*  — LLM vide (aucun modele charge : model/ctx/vocab a NULL). Enveloppe
 * GC pure sans allocation OWL secondaire, pas de rooting requis a la creation.
 * Les handles llama sont poses par .Charge et liberes au finalizer GC. */
ObjLLM* obj_new_llm(void);

/*  — callback de liberation des handles llama, enregistre par
 * stdlib_llm au demarrage (obj_llm_set_backend_free). object.c ne connait pas
 * llama : il delegue via ce pointeur, comme g_zip_backend_close. */
typedef void (*LlmBackendFreeFn)(ObjLLM*);
void obj_llm_set_backend_free(LlmBackendFreeFn fn);

/*  — libere les handles C-heap d'un LLM (idempotent). Appele par le
 * sweep GC (object_free) et re-appelable par .Charge avant un rechargement. */
void obj_llm_free_handles(ObjLLM* l);

/*  — OptionsGénération avec les defauts sensés (température 0.8,
 * topK 40, topP 0.95, maxTokens 512, graine aléatoire). Le champ onToken est
 * init a NIL_VAL : feuille GC tant que non affecte, non-feuille des qu'une
 * reference de procedure y est posee (tracee dans gc.c). */
ObjOptionsGeneration* obj_new_options_generation(void);

/*  — MessageLLM (rôle + contenu) : champs init a NIL_VAL, renseignes
 * par le litteral d'accolade. RéponseLLM : contenu NIL, raison OWL_STOP_NATUREL,
 * compteurs de jetons a zero (valeur neutre du chemin soft-error de .Chat). */
ObjMessageLLM*  obj_new_message_llm(void);
ObjResponseLLM* obj_new_response_llm(void);

/*  — Détection d'objet (boite + classe + score), sortie de
 * `IAModèle.Détecte`. `box` doit etre un ObjArray de 4 reels (ou NIL_VAL). Le
 * caller garde `box` roote pendant cet appel (ALLOCATE_OBJ peut declencher un
 * GC avant l'assignation de det->box). */
ObjDetection* obj_new_detection(Value box, int classIndex, double score);

/*  — produit matriciel dense C(M×N) = A(M×K) · B(K×N), buffers
 * row-major contigus. C est entierement ecrit (mis a zero puis accumule).
 * Kernel rapide tuile + AXPY vectorise ; expose ici pour le forward du
 * produit matrice×matrice differentiable (Tenseur). */
void owl_matmul(const double* restrict A, const double* restrict B,
                double* restrict C, int M, int K, int N);

/* Associative array creation + helpers (v1: string keys only) */
ObjAssocArray* obj_new_assoc(AssocKeyType keyType, int options, int valueKind);
/* Returns 1 if a new key was inserted, 0 if an existing key was updated. */
int  obj_assoc_set(ObjAssocArray* tab, ObjString* key, Value value);
/* Returns the value for key, or the default value for the valueKind if absent.
 * *out_exists (if non-NULL) is set to 1 if the key was present, 0 otherwise. */
Value obj_assoc_get(ObjAssocArray* tab, ObjString* key, int* out_exists);
/* Returns 1 if removed, 0 if the key did not exist. */
int  obj_assoc_remove(ObjAssocArray* tab, ObjString* key);
void obj_assoc_clear(ObjAssocArray* tab);
/* Default value for a VAL_* kind (used for missing-key reads). */
Value obj_assoc_default_for_kind(int valueKind);
Value obj_array_default_for_kind(int kind);
/* Shallow clone: a new ObjAssocArray with the same keys/values. */
ObjAssocArray* obj_assoc_clone(const ObjAssocArray* src);

/* Multilingue creation ( lot B). Alloue une chaineMultilingue
 * vide pretes a recevoir des entries via OP_ARRAY_SET. Le storage interne
 * est un ObjAssocArray (STRING + SansCasse + valueKind=CHAINE) — assure
 * "fr-FR" == "fr-fr" cote BCP 47 et un default "" sur lecture absente. */
ObjMultilingue* obj_new_multilingue(void);

/*  — insere/maj une entree multilingue avec cle canonicalisee BCP 47
 * (case-insensible). A utiliser pour tout STORE (litteral inline, cast,
 * m[tag] = "..."). Le caller garantit m rooté. */
void multilingue_store(ObjMultilingue* m, const char* tag, Value val);

/* Multilingue resolution ( lot C). Lookup `m[lang]` avec fallback
 * sur la langue source. Si lang absent, retourne m[langueSource] ; si la
 * langue source elle-meme est absente, retourne "". Pas de cascade BCP 47
 * (decision v1). Une entree posee explicitement avec valeur "" est traitee
 * comme presente — pas de fallback dans ce cas (distinction "vide explicite"
 * vs "absente"). */
Value multilingue_resolve(ObjMultilingue* m, const char* lang, const char* langueSource);

/* Instance creation */
ObjInstance* obj_new_instance(ObjString* structName);

/* Helpers GC-safe pour poser un champ dans inst->fields.
 * Internalisent vm_push(inst)+vm_push(key)[+vm_push(val si Obj)] avant
 * obj_copy_string + table_set qui peut grow. Indispensables sous
 * DEBUG_STRESS_GC. Cf.  lot 5. */
void inst_field_set_str  (ObjInstance* inst, const char* name, const char* val);
void inst_field_set_int  (ObjInstance* inst, const char* name, int64_t val);
void inst_field_set_bool (ObjInstance* inst, const char* name, bool val);
void inst_field_set_value(ObjInstance* inst, const char* name, Value val);

/*  — coerce une valeur vers chaine pour une prop chaine d'un Champ
 * de Saisie (kind == OWL_CONTROL_INPUT). Identite pour tout autre kind, prop
 * non-chaine, ou valeur deja chaine. Reutilise par  (cellule ZR
 * Saisie). Peut allouer une ObjString (value_to_string) -> discipline GC
 * roots au call-site. */
Value ui_input_coerce_string_prop(int kind, const char* prop, Value v);

/*  : canon pour enregistrer une valeur dans vm.globals avec la
 * discipline GC roots integree. Pousse k + val (si Obj) avant table_set
 * pour survivre a un grow → GC. Remplace les helpers locaux dispersés. */
void obj_globals_set(const char* name, Value val);

/*  : variantes "key ObjString* deja allouee" pour les cas
 * iteration (e->key d'une table existante) ou AS_STRING d'un Value
 * deja rooted. Les inst_field_set_*(const char*) re-allouent une key
 * via obj_copy_string ; ces variantes _kv evitent la re-alloc redondante
 * et garantissent push correct si la key d'entree n'est pas root. */
void obj_inst_set_kv     (ObjInstance* inst, ObjString* k, Value val);
void obj_globals_set_kv  (ObjString* k, Value val);

/*  : canon pour appendre une Value a un ObjArray.
 * value_array_write(&arr->elements, val) peut grow → GC → si val est
 * un Obj* fresh-alloc non-rooted (typique : `OBJ_VAL(obj_copy_string(...))`
 * passe en argument), il est collecte et l'array recoit une Value
 * pointant vers freed memory. Le canon push arr+val (si Obj) en interne
 * avant le grow. */
void obj_array_push(ObjArray* arr, Value val);

/*  : canon pour poser {cle cstr -> value} dans un ObjAssocArray.
 * Motif UAF cible : construire une entree en allouant la cle (obj_copy_string)
 * ET la valeur en deux temps. Le premier alloue reste un C local non-roote
 * pendant que le second alloue -> un GC dans cette fenetre le collecte
 * (ObjString cle ou valeur fresh, jamais vu par le scan de racines). Le canon
 * pousse dest + value (si Obj) AVANT d'allouer la cle, puis delegue a
 * obj_assoc_set (deja auto-rootant pour son grow depuis ). SUR quel que
 * soit l'ordre cle/valeur au call-site (cle-avant-valeur comme valeur-avant-cle). */
void obj_assoc_put_cstr(ObjAssocArray* dest, const char* name, int namelen, Value value);

/* Class creation */
ObjClass* obj_new_class(ObjString* name);

/* Collection creation */
ObjCollection* obj_new_collection(CollectionType collType);

/* JSON creation */
ObjJson* obj_new_json(JsonNodeType jtype);

/* Procedure chain creation / mutation helpers. */
ObjProcChain* obj_new_proc_chain(void);
/* Ensure `v` is a procedure chain. If already one, return as-is. Otherwise
 * wrap the single procedure value in a new chain. */
ObjProcChain* obj_proc_chain_of(Value v);
/* Append a callable to the chain. No-op if non-callable. */
void obj_proc_chain_add(ObjProcChain* chain, Value callable);
/* Remove all occurrences of `callable` from the chain. Returns how many were removed. */
int obj_proc_chain_remove(ObjProcChain* chain, Value callable);
ObjJson* obj_new_json_string(ObjString* str);
ObjJson* obj_new_json_int(int64_t val);
ObjJson* obj_new_json_real(double val);
ObjJson* obj_new_json_currency(__int128 val);
ObjJson* obj_new_json_numeric(Value val);  /* val doit etre IS_NUMERIC */
ObjJson* obj_new_json_bool(bool val);
void obj_json_object_set(ObjJson* obj, ObjString* key, ObjJson* value);
ObjJson* obj_json_object_get(ObjJson* obj, ObjString* key);
bool obj_json_key_eq_ci(const ObjString* a, const ObjString* b);
void obj_json_array_push(ObjJson* arr, ObjJson* value);
Value obj_json_to_value(ObjJson* json);

/* ================================================================
 * DATE / TIME / DATETIME / DURATION OBJECTS
 * ================================================================ */

typedef struct {
    Obj obj;
    int year;
    int month;
    int day;
} ObjDate;

typedef struct {
    Obj obj;
    int hour;
    int minute;
    int second;
    int ms;  /* milliseconds (internal precision) */
    int cs;  /* centièmes de seconde 0-99 (canon HHMMSSCC format) */
} ObjTime;

typedef struct {
    Obj obj;
    int year, month, day;
    int hour, minute, second, ms;
} ObjDateTime;

typedef struct {
    Obj obj;
    int64_t totalMs; /* signed milliseconds */
} ObjDuration;

/* Type checks */
#define IS_DATE(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_DATE)
#define IS_TIME(value)     (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_TIME)
#define IS_DATETIME(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_DATETIME)
#define IS_DURATION(value) (IS_OBJ(value) && OBJ_TYPE(value) == OBJ_DURATION)

/* Casts */
#define AS_DATE(value)     ((ObjDate*)AS_OBJ(value))
#define AS_TIME(value)     ((ObjTime*)AS_OBJ(value))
#define AS_DATETIME(value) ((ObjDateTime*)AS_OBJ(value))
#define AS_DURATION(value) ((ObjDuration*)AS_OBJ(value))

/* Constructors */
ObjDate* obj_new_date(int year, int month, int day);
ObjTime* obj_new_time(int hour, int minute, int second, int ms);
ObjTime* obj_new_time_cs(int hour, int minute, int second, int cs);
ObjDateTime* obj_new_datetime(int year, int month, int day,
                               int hour, int minute, int second, int ms);
ObjDuration* obj_new_duration(int64_t totalMs);

/* Affichage d'un objet. object_print ecrit sur stdout ; les deux passent
 * par le meme parcours (cf. OwlDisplaySink dans value.h). */
void object_print(Value value);
void object_write_display(OwlDisplaySink* sink, Value value);

/* Free an object */
void object_free(Obj* object);

/* Track a new object in the GC's linked list */
void obj_register(Obj* object);

/* GC globals */
extern Obj* owl_objects;  /* head of all-objects list */

#endif
