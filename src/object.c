// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "object.h"
#include "memory.h"
#include "table.h"
#include "gc.h"
#include "vm.h"
#include "scanner.h"     /*  phase 2 : scanner_canonicalize_identifier */
#include "ui_decl.h"     /*  : OwlControlDecl::kind dans obj_new_ui_control_ref */
#include "ui_catalog.h"  /*  : owl_control_catalog_find pour cle canon headless */
#include "owl_stdlib.h"  /* owl_value_is_uuid / owl_uuid_format_canonical */
#include "owl_name_normalize.h"  /* owl_bcp47_canon */
#include "sentinels.h"   /*  : defauts OptionsEntraînement (optim/perte) */
#include <ctype.h>       /* tolower ( headless_key_normalize) */
#include <math.h>        /*  : exp/INFINITY (obj_tensor_softmax) */
#include "mpdecimal.h"   /* OBJ_NUMERIC : mpd_qnew / mpd_del */

#ifdef OWL_INDETECTABLE
#  ifdef _WIN32
#    include <windows.h>
#    include <bcrypt.h>
#  endif
#endif

/* Head of the linked list of ALL heap objects (for GC) */
Obj* owl_objects = NULL;

/* Global string interning table */
extern Table owl_strings;

/* ================================================================
 * UNDETECTABLE STRING MASKING (opt-in at build time)
 *
 * Compiled only when `-DOWL_INDETECTABLE` is set (see `make secure`).
 * Default `owl.exe` omits the XOR masking machinery entirely so the
 * binary stays free of crypter-like heuristic patterns that Windows
 * Defender flags. When the feature is OFF, the `<indétectable>`
 * attribute parses without error but produces no masking (the string
 * behaves like any other). See docs/SECURITY.md.
 * ================================================================ */

#ifdef OWL_INDETECTABLE

#define OWL_MASK_KEY_LEN 16
static unsigned char g_mask_key[OWL_MASK_KEY_LEN];
static bool g_mask_key_initialized = false;

/* Securely zero a buffer (resists compiler optimization). */
static void secure_zero(void* p, size_t n) {
#ifdef _WIN32
    SecureZeroMemory(p, n);
#else
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n--) *v++ = 0;
#endif
}

void obj_init_mask_key(void) {
    if (g_mask_key_initialized) return;
#ifdef _WIN32
    if (BCryptGenRandom(NULL, g_mask_key, OWL_MASK_KEY_LEN,
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        fprintf(stderr, "owl: failed to seed undetectable key\n");
        exit(74);
    }
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(g_mask_key, 1, OWL_MASK_KEY_LEN, f) != OWL_MASK_KEY_LEN) {
        fprintf(stderr, "owl: failed to seed undetectable key\n");
        exit(74);
    }
    fclose(f);
#endif
    g_mask_key_initialized = true;
}

/* In-place XOR with the cycling key. Same op masks and unmasks. */
static void mask_buffer(char* buf, int len) {
    for (int i = 0; i < len; i++)
        buf[i] = (char)((unsigned char)buf[i] ^ g_mask_key[i % OWL_MASK_KEY_LEN]);
}

char* obj_string_unmask_dup(const ObjString* s) {
    char* out = (char*)malloc((size_t)s->length + 1);
    if (!out) { fprintf(stderr, "owl: OOM (unmask)\n"); exit(74); }
    memcpy(out, s->chars, s->length);
    if (s->undetectable) mask_buffer(out, s->length);
    out[s->length] = '\0';
    return out;
}

void obj_string_secure_free(char* buf, int len) {
    if (!buf) return;
    secure_zero(buf, (size_t)len);
    free(buf);
}

#else  /* OWL_INDETECTABLE not set: provide harmless fallbacks */

void obj_init_mask_key(void) { /* no-op: masking is a build-time opt-in */ }

char* obj_string_unmask_dup(const ObjString* s) {
    /* Plain duplication. Without the feature, strings are never masked,
     * so "unmask" is just memcpy. */
    char* out = (char*)malloc((size_t)s->length + 1);
    if (!out) { fprintf(stderr, "owl: OOM\n"); exit(74); }
    memcpy(out, s->chars, s->length);
    out[s->length] = '\0';
    return out;
}

void obj_string_secure_free(char* buf, int len) {
    (void)len;
    free(buf);
}

#endif /* OWL_INDETECTABLE */

/* ================================================================
 * OBJECT ALLOCATION HELPERS
 * ================================================================ */

static Obj* allocate_object(size_t size, ObjType type) {
    Obj* object = (Obj*)owl_realloc(NULL, 0, size);
    object->type = type;
    object->isMarked = false;
    object->next = owl_objects;
    owl_objects = object;

#ifdef DEBUG_LOG_GC
    printf("  alloc %p type=%d size=%zu\n", (void*)object, type, size);
#endif

    return object;
}

#define ALLOCATE_OBJ(type, objectType) \
    (type*)allocate_object(sizeof(type), objectType)

/* ================================================================
 * STRING HASHING (FNV-1a)
 * ================================================================ */

static uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

/* ================================================================
 * STRING CREATION
 * ================================================================ */

static ObjString* allocate_string(char* chars, int length, uint32_t hash) {
    ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->capacity = length + 1;  /* exact-fit for interned strings */
    string->chars = chars;
    string->hash = hash;
    string->interned = true;
    string->undetectable = false;
    string->ascii_state = 0;
    string->cp_count = -1;
    string->last_cp_index = -1;
    string->last_byte_offset = 0;
    string->frozen_cache = NULL;

    /* Protect from GC during interning (table_set may trigger collection) */
    vm_push(OBJ_VAL(string));
    /* Intern the string: store in global table for deduplication */
    table_set(&owl_strings, string, NIL_VAL);
    vm_pop();

    return string;
}

ObjString* obj_copy_string(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);

    /* Check if this string already exists (interning) */
    ObjString* interned = table_find_string(&owl_strings, chars, length, hash);
    if (interned != NULL) return interned;

    char* heapChars = ALLOCATE(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocate_string(heapChars, length, hash);
}

ObjString* obj_take_string(char* chars, int length) {
    uint32_t hash = hash_string(chars, length);

    ObjString* interned = table_find_string(&owl_strings, chars, length, hash);
    if (interned != NULL) {
        FREE_ARRAY(char, chars, length + 1);
        return interned;
    }

    return allocate_string(chars, length, hash);
}

#ifdef OWL_INDETECTABLE

/* Allocator for undetectable strings: bypasses interning entirely,
 * masks the chars buffer in place. The hash is computed on the cleartext
 * (so equality comparisons still work via obj_string_unmask_dup). */
static ObjString* allocate_string_undetectable(char* chars, int length, uint32_t hash) {
    ObjString* string = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    string->length = length;
    string->capacity = length + 1;
    string->chars = chars;
    string->hash = hash;
    string->interned = false;       /* bypass owl_strings table entirely */
    string->undetectable = true;
    string->ascii_state = 0;
    string->cp_count = -1;
    string->last_cp_index = -1;
    string->last_byte_offset = 0;
    string->frozen_cache = NULL;
    /* Mask in place. The buffer now holds masked bytes; the trailing \0
     * is preserved so unmask_dup can copy length bytes safely, but any
     * raw read of `chars` will see noise. */
    mask_buffer(string->chars, string->length);
    return string;
}

ObjString* obj_copy_string_undetectable(const char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    char* heapChars = ALLOCATE(char, length + 1);
    memcpy(heapChars, chars, length);
    heapChars[length] = '\0';
    return allocate_string_undetectable(heapChars, length, hash);
}

ObjString* obj_take_string_undetectable(char* chars, int length) {
    uint32_t hash = hash_string(chars, length);
    return allocate_string_undetectable(chars, length, hash);
}

#else  /* OWL_INDETECTABLE not set: fall back to regular allocation */

ObjString* obj_copy_string_undetectable(const char* chars, int length) {
    /* Feature disabled: the resulting string is a plain interned string
     * with undetectable=false. The `<indétectable>` attribute becomes a
     * declaration-level hint with no runtime effect. */
    return obj_copy_string(chars, length);
}

ObjString* obj_take_string_undetectable(char* chars, int length) {
    return obj_take_string(chars, length);
}

#endif /* OWL_INDETECTABLE */

ObjString* obj_concat_strings(ObjString* a, ObjString* b) {
    int length = a->length + b->length;
    /* Propagate the undetectable flag: if either input is protected,
     * the result must be protected too -- otherwise concatenation is
     * an easy bypass to leak the value into a non-protected variable. */
    bool resultUndetectable = a->undetectable || b->undetectable;

    if (!resultUndetectable) {
        char* chars = ALLOCATE(char, length + 1);
        memcpy(chars, a->chars, a->length);
        memcpy(chars + a->length, b->chars, b->length);
        chars[length] = '\0';
        return obj_take_string(chars, length);
    }

    /* Undetectable path: unmask sources into temps, concat into a fresh
     * cleartext buffer, then hand off to the undetectable allocator
     * (which masks in place). Tmps are securely scrubbed. */
    char* aClear = obj_string_unmask_dup(a);
    char* bClear = obj_string_unmask_dup(b);
    char* chars = ALLOCATE(char, length + 1);
    memcpy(chars, aClear, a->length);
    memcpy(chars + a->length, bClear, b->length);
    chars[length] = '\0';
    obj_string_secure_free(aClear, a->length);
    obj_string_secure_free(bClear, b->length);
    return obj_take_string_undetectable(chars, length);
}

/* ----------------------------------------------------------------
 * STRING BUILDER mode (interned=false, capacity > length+1).
 * Created on demand by OP_APPEND_IN_PLACE the first time a local
 * string slot enters builder mode. NOT registered in owl_strings.
 * ---------------------------------------------------------------- */

ObjString* obj_string_to_buffer(ObjString* s) {
    /* Reserve at least 16 bytes, and 2x current length, whichever is bigger,
     * to amortize subsequent appends. */
    int initial_cap = s->length * 2 + 16;
    if (initial_cap < 16) initial_cap = 16;

    char* chars = ALLOCATE(char, initial_cap);
    /* Propagate undetectable masking byte-for-byte: source chars are
     * already masked (or not), the destination keeps the same flag. */
    if (s->length > 0) memcpy(chars, s->chars, s->length);
    chars[s->length] = '\0';

    ObjString* buf = ALLOCATE_OBJ(ObjString, OBJ_STRING);
    buf->length   = s->length;
    buf->capacity = initial_cap;
    buf->chars    = chars;
    buf->hash     = 0;       /* invalid; recomputed on freeze if needed */
    buf->interned = false;
    buf->undetectable = s->undetectable;
    buf->ascii_state = 0;
    buf->cp_count = -1;
    buf->last_cp_index = -1;
    buf->last_byte_offset = 0;
    buf->frozen_cache = NULL;
    /* NOT inserted into owl_strings: builders are never table keys. */
    return buf;
}

/* ================================================================
 * FUNCTION CREATION
 * ================================================================ */

ObjFunction* obj_new_function(void) {
    ObjFunction* function = ALLOCATE_OBJ(ObjFunction, OBJ_FUNCTION);
    function->arity = 0;
    function->minArity = 0;
    function->name = NULL;
    function->filename = NULL;
    function->chunk = NULL; /* will be set by compiler */
    function->defaults = NULL;
    function->defaultCount = 0;
    function->localNames = NULL;
    function->localNameCount = 0;
    function->localNameCapacity = 0;
    return function;
}

/*  — table de noms de locales (debug info). */
void obj_function_add_local_name(ObjFunction* fn, const char* name, int length,
                                 int slot, int ip_start) {
    if (!fn || !name || length <= 0) return;
    /* Strip accents : forme canon stockee (casse preservee). Le matching
     * accent/casse-insensible se fait a la resolution via canonicalize. */
    char stripped[256];
    int slen = scanner_strip_accents(name, length, stripped, sizeof(stripped));
    /* Root du nom fresh-alloc : la fonction proprietaire est rootee par le
     * compiler (current->function) pendant l'emission, donc l'ObjString
     * survit au GC potentiel de obj_copy_string. On ne re-push pas ici. */
    ObjString* nameStr = obj_copy_string(stripped, slen);
    if (fn->localNameCount >= fn->localNameCapacity) {
        int newCap = fn->localNameCapacity == 0 ? 8 : fn->localNameCapacity * 2;
        LocalName* grown = (LocalName*)realloc(fn->localNames,
                                               (size_t)newCap * sizeof(LocalName));
        if (!grown) return;
        fn->localNames = grown;
        fn->localNameCapacity = newCap;
    }
    fn->localNames[fn->localNameCount].name = nameStr;
    fn->localNames[fn->localNameCount].slot = slot;
    /*  — plage de validite ouverte a la declaration. Fermee a
     * end_scope via obj_function_close_local_names. */
    fn->localNames[fn->localNameCount].ip_start = ip_start;
    fn->localNames[fn->localNameCount].ip_end = -1;
    fn->localNameCount++;
}

/*  — verifie si l'IP de pause est dans la plage de validite d'un nom.
 * ip < 0 (pas d'IP fournie) = match permissif (compat appelants sans IP). */
static bool local_name_ip_in_range(const LocalName* ln, int ip) {
    if (ip < 0) return true;
    if (ip < ln->ip_start) return false;
    if (ln->ip_end >= 0 && ip >= ln->ip_end) return false;
    return true;
}

void obj_function_close_local_names(ObjFunction* fn, int fromSlot, int ip_end) {
    if (!fn || !fn->localNames) return;
    for (int i = 0; i < fn->localNameCount; i++) {
        if (fn->localNames[i].ip_end < 0 && fn->localNames[i].slot >= fromSlot) {
            fn->localNames[i].ip_end = ip_end;
        }
    }
}

int obj_function_resolve_local(const ObjFunction* fn, const char* name,
                               int length, int ip) {
    if (!fn || !fn->localNames || length <= 0) return -1;
    char canonQ[256];
    int qlen = scanner_canonicalize_identifier(name, length, canonQ, sizeof(canonQ));
    /*  — itere a l'envers (scope le plus interne d'abord) et filtre sur
     * l'IP de pause : un nom dont le scope est deja sorti ne resout plus. */
    for (int i = fn->localNameCount - 1; i >= 0; i--) {
        ObjString* s = fn->localNames[i].name;
        if (!s || !s->chars) continue;
        if (!local_name_ip_in_range(&fn->localNames[i], ip)) continue;
        char canonS[256];
        int slen = scanner_canonicalize_identifier(s->chars, s->length,
                                                   canonS, sizeof(canonS));
        if (slen == qlen && memcmp(canonQ, canonS, (size_t)qlen) == 0) {
            return fn->localNames[i].slot;
        }
    }
    return -1;
}

const char* obj_function_local_name_at(const ObjFunction* fn, int slot, int ip) {
    if (!fn || !fn->localNames) return NULL;
    /*  — itere a l'envers pour preferer le nom le plus recemment
     * declare (scope le plus interne) en cas de plages qui se chevauchent.
     * Filtre sur l'IP du point de pause : un slot recycle entre deux scopes
     * sequentiels ne renvoie plus le nom du scope deja sorti. */
    for (int i = fn->localNameCount - 1; i >= 0; i--) {
        if (fn->localNames[i].slot != slot) continue;
        if (!local_name_ip_in_range(&fn->localNames[i], ip)) continue;
        ObjString* s = fn->localNames[i].name;
        return (s && s->chars) ? s->chars : NULL;
    }
    return NULL;
}

ObjNative* obj_new_native(NativeFn fn, int arity) {
    ObjNative* native = ALLOCATE_OBJ(ObjNative, OBJ_NATIVE);
    native->function = fn;
    native->arity = arity;
    native->firstOutParamArg = -1;  /* défaut : pas d'out-params */
    native->lastOutParamArg  = -1;  /* défaut : variadic jusqu'à la fin */
    return native;
}

/* ================================================================
 * ARRAY CREATION
 * ================================================================ */

ObjArray* obj_new_array(void) {
    ObjArray* array = ALLOCATE_OBJ(ObjArray, OBJ_ARRAY);
    value_array_init(&array->elements);
    array->elementStructType = NULL;
    array->elementPrimKind = 0;
    array->growthMode = 0;
    array->is_group = false;
    array->owner_window = NULL;
    return array;
}

/*  — Vecteur : enveloppe GC + double[] contigu (malloc, pas GC).
 * ALLOCATE_OBJ peut declencher un GC, mais le vecteur n'est reference par
 * rien a ce stade et le malloc du buffer ne touche pas le GC : ordre sur. */
ObjVector* obj_new_vector(int count) {
    if (count < 0) count = 0;
    ObjVector* v = ALLOCATE_OBJ(ObjVector, OBJ_VECTOR);
    v->count = count;
    v->capacity = count;
    v->data = NULL;
    if (count > 0) {
        v->data = (double*)malloc(sizeof(double) * (size_t)count);
        for (int i = 0; i < count; i++) v->data[i] = 0.0;
        owl_gc_account_ext((int64_t)sizeof(double) * count);  /*  */
    }
    return v;
}

/*  — xlsDocument : enveloppe GC + premiere feuille vide. Le C-heap
 * (sheets/cells/merges/path) ne touche pas le GC ; l'objet n'etant reference
 * par rien pendant les malloc post-ALLOCATE_OBJ, l'ordre est sur (cf.
 * ObjMatrix). */
ObjXlsDocument* obj_new_xls_document(void) {
    ObjXlsDocument* d = ALLOCATE_OBJ(ObjXlsDocument, OBJ_XLS_DOCUMENT);
    d->sheets    = NULL;
    d->sheetCount = 0;
    d->sheetCap  = 0;
    d->current   = 0;
    d->path      = NULL;
    d->preservedNames = NULL;  /*  — parts non modelisees d'un fichier ouvert */
    d->preservedData  = NULL;
    d->preservedLens  = NULL;
    d->preservedCount = 0;
    d->preservedCap   = 0;
    /* Premiere feuille par defaut ("Feuil1") — un classeur n'est jamais vide. */
    d->sheets = (XlsSheet*)malloc(sizeof(XlsSheet));
    d->sheetCap = 1;
    d->sheetCount = 1;
    XlsSheet* s = &d->sheets[0];
    s->name = (char*)malloc(7);
    memcpy(s->name, "Feuil1", 7);
    s->cells = NULL;  s->cellCount = 0;  s->cellCap = 0;
    s->merges = NULL; s->mergeCount = 0; s->mergeCap = 0;
    s->rowDims = NULL; s->rowDimCount = 0; s->rowDimCap = 0;  /*  */
    s->colDims = NULL; s->colDimCount = 0; s->colDimCap = 0;  /*  */
    return d;
}

/*  — reference de style (cellule/ligne/colonne). Enveloppe GC simple :
 * pointe le classeur, non liee jusqu'au subscript. ALLOCATE_OBJ peut GC mais
 * `doc` est deja roote par l'appelant (receveur de la propriete). */
ObjXlsRef* obj_new_xls_ref(ObjXlsDocument* doc, XlsRefKind kind) {
    ObjXlsRef* r = ALLOCATE_OBJ(ObjXlsRef, OBJ_XLS_REF);
    r->doc   = doc;
    r->kind  = kind;
    r->row   = 0;
    r->col   = 0;
    r->bound = false;
    return r;
}

/*  — Matrice : enveloppe GC + double[] row-major (malloc, zero).
 * ALLOCATE_OBJ peut declencher un GC, mais la matrice n'est referencee par
 * rien a ce stade et le malloc du buffer ne touche pas le GC : ordre sur. */
ObjMatrix* obj_new_matrix(int rows, int cols) {
    if (rows < 0) rows = 0;
    if (cols < 0) cols = 0;
    ObjMatrix* m = ALLOCATE_OBJ(ObjMatrix, OBJ_MATRIX);
    m->rows = rows;
    m->cols = cols;
    m->data = NULL;
    size_t n = (size_t)rows * (size_t)cols;
    if (n > 0) {
        m->data = (double*)malloc(sizeof(double) * n);
        for (size_t i = 0; i < n; i++) m->data[i] = 0.0;
        owl_gc_account_ext((int64_t)(sizeof(double) * n));  /*  */
    }
    return m;
}

/*  — vue de ligne : simple enveloppe GC pointant `mat`. ALLOCATE_OBJ
 * peut GC ; le caller garde `mat` roote (peek de la pile pendant `m[i]`). */
ObjMatrixRow* obj_new_matrix_row(ObjMatrix* mat, int row) {
    ObjMatrixRow* r = ALLOCATE_OBJ(ObjMatrixRow, OBJ_MATRIX_ROW);
    r->mat = mat;
    r->row = row;
    return r;
}

/*  — Tenseur feuille. On alloue l'enveloppe puis on la ROOTE sur la
 * pile VM avant d'allouer donnee + gradient : chaque obj_new_vector peut
 * declencher un GC, et le premier Vecteur (data) doit survivre a l'alloc du
 * second (grad). Le tenseur rooté est trace par le GC, qui suit data (deja
 * pose) et grad (encore NULL au 1er cycle — ignore). */
ObjTensor* obj_new_tensor(int count) {
    if (count < 0) count = 0;
    ObjTensor* t = ALLOCATE_OBJ(ObjTensor, OBJ_TENSOR);
    t->data = NULL;
    t->grad = NULL;
    t->ndim = 1;
    t->shape[0] = count;
    t->op_axis = 0;
    t->conv_stride = 0;
    t->conv_pad = 0;
    t->bn_eps = 0.0;
    t->op = TENSOR_OP_LEAF;
    t->parents[0] = NULL;
    t->parents[1] = NULL;
    t->nParents = 0;
    t->visited = false;
    vm_push(OBJ_VAL(t));
    t->data = (Obj*)obj_new_vector(count);
    /*  : sous no-grad (inference), pas de buffer grad — aucun backward
     * ne le lira, grad reste NULL (le GC le tolere). */
    if (!g_no_grad)
        t->grad = (Obj*)obj_new_vector(count);   /* zero-init par obj_new_vector */
    vm_pop();
    return t;
}

/*  — Tenseur feuille 2D : donnee + gradient dans deux ObjMatrix.
 * Meme rooting que obj_new_tensor : l'enveloppe est rootee sur la pile pendant
 * l'alloc des deux matrices (la premiere doit survivre a l'alloc de la seconde). */
ObjTensor* obj_new_tensor_matrix(int rows, int cols) {
    if (rows < 0) rows = 0;
    if (cols < 0) cols = 0;
    ObjTensor* t = ALLOCATE_OBJ(ObjTensor, OBJ_TENSOR);
    t->data = NULL;
    t->grad = NULL;
    t->ndim = 2;
    t->shape[0] = rows;
    t->shape[1] = cols;
    t->op_axis = 0;
    t->conv_stride = 0;
    t->conv_pad = 0;
    t->bn_eps = 0.0;
    t->op = TENSOR_OP_LEAF;
    t->parents[0] = NULL;
    t->parents[1] = NULL;
    t->nParents = 0;
    t->visited = false;
    vm_push(OBJ_VAL(t));
    t->data = (Obj*)obj_new_matrix(rows, cols);
    if (!g_no_grad)   /*  : pas de grad sous no-grad (inference) */
        t->grad = (Obj*)obj_new_matrix(rows, cols);   /* zero-init par obj_new_matrix */
    vm_pop();
    return t;
}

/*  — Tenseur feuille de rang arbitraire. Rang 2 delegue au
 * constructeur Matrice (kernels rapides + rendu Matrice). Sinon buffer plat
 * ObjVector de produit(shape) elements. Meme rooting que obj_new_tensor. */
ObjTensor* obj_new_tensor_nd(int ndim, const int* shape) {
    if (ndim < 1) ndim = 1;
    if (ndim > OWL_TENSOR_MAX_RANK) ndim = OWL_TENSOR_MAX_RANK;
    if (ndim == 2) return obj_new_tensor_matrix(shape[0], shape[1]);

    int numel = 1;
    for (int i = 0; i < ndim; i++) numel *= (shape[i] < 0 ? 0 : shape[i]);

    ObjTensor* t = ALLOCATE_OBJ(ObjTensor, OBJ_TENSOR);
    t->data = NULL;
    t->grad = NULL;
    t->ndim = ndim;
    for (int i = 0; i < ndim; i++) t->shape[i] = shape[i];
    t->op_axis = 0;
    t->conv_stride = 0;
    t->conv_pad = 0;
    t->bn_eps = 0.0;
    t->op = TENSOR_OP_LEAF;
    t->parents[0] = NULL;
    t->parents[1] = NULL;
    t->nParents = 0;
    t->visited = false;
    vm_push(OBJ_VAL(t));
    t->data = (Obj*)obj_new_vector(numel);
    if (!g_no_grad)   /*  : pas de grad sous no-grad (inference) */
        t->grad = (Obj*)obj_new_vector(numel);   /* zero-init par obj_new_vector */
    vm_pop();
    return t;
}

/*  — vue transitoire (sous-tranche N-D). Simple enveloppe GC pointant
 * `backing`. ALLOCATE_OBJ peut GC ; le caller garde `backing` (et le Tenseur
 * source) roote pendant l'alloc (peek pile au chainage `[i]`). */
ObjTensorView* obj_new_tensor_view(Obj* backing, int offset, int ndim,
                                   const int* shape, const int* strides) {
    ObjTensorView* v = ALLOCATE_OBJ(ObjTensorView, OBJ_TENSOR_VIEW);
    v->backing = backing;
    v->offset = offset;
    v->ndim = ndim;
    for (int i = 0; i < ndim; i++) {
        v->shape[i] = shape[i];
        v->strides[i] = strides[i];
    }
    return v;
}

ObjTensor* obj_tensor_binop(TensorOp op, ObjTensor* a, ObjTensor* b) {
    /*  — broadcasting N-D generalise (numpy/torch) pour `+` et `*`.
     * Le cas biais 2D+1D n'est plus special : c'est un sous-cas de
     * ces regles (b de rang plus court, axe des lignes absent -> diffuse). La
     * forme de sortie est le broadcast des deux formes ; formes incompatibles
     * -> NULL -> l'appelant VM leve une erreur claire (mecanisme inchange). */
    int outShape[OWL_TENSOR_MAX_RANK];
    int ndimOut = tensor_broadcast_shape(a->ndim, a->shape,
                                         b->ndim, b->shape, outShape);
    if (ndimOut < 0) return NULL;

    /* a et b rootes par le caller pendant cet unique point d'allocation OWL. */
    ObjTensor* t = obj_new_tensor_nd(ndimOut, outShape);
    /* A partir d'ici, plus aucune allocation OWL : ecriture de doubles et de
     * pointeurs parents seulement. `t` reste sain jusqu'au retour. */
    double* ad = tensor_cells(a->data);
    double* bd = tensor_cells(b->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(t->data);

    if (tensor_shapes_equal(a, b)) {
        /* Formes identiques (cas dominant) : correspondance 1-1, pas
         * d'arithmetique d'index de broadcast. */
        for (int i = 0; i < numel; i++)
            td[i] = (op == TENSOR_OP_ADD) ? (ad[i] + bd[i]) : (ad[i] * bd[i]);
    } else {
        int bsA[OWL_TENSOR_MAX_RANK], bsB[OWL_TENSOR_MAX_RANK];
        tensor_broadcast_strides(a->ndim, a->shape, ndimOut, bsA);
        tensor_broadcast_strides(b->ndim, b->shape, ndimOut, bsB);
        for (int fo = 0; fo < numel; fo++) {
            /* Decompose l'index plat de sortie en multi-index row-major puis
             * mappe vers a et b via leurs strides broadcast (0 = axe diffuse). */
            int rem = fo, ia = 0, ib = 0;
            for (int d = ndimOut - 1; d >= 0; d--) {
                int idx = rem % outShape[d];
                rem /= outShape[d];
                ia += idx * bsA[d];
                ib += idx * bsB[d];
            }
            td[fo] = (op == TENSOR_OP_ADD) ? (ad[ia] + bd[ib]) : (ad[ia] * bd[ib]);
        }
    }
    /*  : sous no-grad, le noeud reste feuille (op LEAF, sans parents) ->
     * les intermediaires deviennent collectables. Le forward est deja calcule
     * ci-dessus, seul l'enregistrement du graphe est saute. */
    if (!g_no_grad) {
        t->op = op;
        t->parents[0] = a;
        t->parents[1] = b;
        t->nParents = 2;
    }
    return t;
}

/*  — noeud CONCAT_CH : concatene a et b sur l'axe canal (index 1).
 * Rend NULL si formes incompatibles (rang < 2, rangs differents, ou un axe
 * autre que l'axe 1 differe) : le caller leve alors l'erreur claire. a et b
 * rootes par le caller pendant l'unique point d'allocation (obj_new_tensor_nd) ;
 * ensuite copie de doubles, aucune allocation OWL, t reste sain jusqu'au retour. */
ObjTensor* obj_tensor_concat_ch(ObjTensor* a, ObjTensor* b) {
    if (a->ndim < 2 || a->ndim != b->ndim) return NULL;
    for (int d = 0; d < a->ndim; d++) {
        if (d == 1) continue;                       /* l'axe canal a le droit de differer */
        if (a->shape[d] != b->shape[d]) return NULL;
    }
    int Ca = a->shape[1], Cb = b->shape[1];
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < a->ndim; d++) outShape[d] = a->shape[d];
    outShape[1] = Ca + Cb;

    /* Blocs contigus : `outer` = produit des axes avant l'axe 1 (= N), `inner` =
     * produit des axes apres (le spatial). Chaque canal est un bloc de `inner`
     * elements ; on pose les canaux de a puis ceux de b, par tranche de N. */
    int outer = a->shape[0];
    int inner = 1;
    for (int d = 2; d < a->ndim; d++) inner *= a->shape[d];
    int Cout = Ca + Cb;

    ObjTensor* t = obj_new_tensor_nd(a->ndim, outShape);
    double* ad = tensor_cells(a->data);
    double* bd = tensor_cells(b->data);
    double* td = tensor_cells(t->data);
    for (int n = 0; n < outer; n++) {
        for (int c = 0; c < Ca; c++)
            for (int k = 0; k < inner; k++)
                td[(n * Cout + c) * inner + k] = ad[(n * Ca + c) * inner + k];
        for (int c = 0; c < Cb; c++)
            for (int k = 0; k < inner; k++)
                td[(n * Cout + Ca + c) * inner + k] = bd[(n * Cb + c) * inner + k];
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (cf. obj_tensor_binop) */
        t->op = TENSOR_OP_CONCAT_CH;
        t->parents[0] = a;
        t->parents[1] = b;
        t->nParents = 2;
    }
    return t;
}

/*  — noeud SLICE_CH : tranche de canaux [cstart, cstart+ccount) de src
 * sur l'axe 1. Inverse exact du CONCAT_CH. `src` roote par le caller pendant
 * l'unique point d'allocation (obj_new_tensor_nd) ; ensuite copie de doubles,
 * aucune allocation OWL. Meme decoupage en blocs contigus qu'au concat
 * (outer=N, inner=spatial). Rend NULL si bornes invalides. */
ObjTensor* obj_tensor_slice_ch(ObjTensor* src, int cstart, int ccount) {
    if (src->ndim < 2 || cstart < 0 || ccount <= 0) return NULL;
    int Cin = src->shape[1];
    if (cstart + ccount > Cin) return NULL;

    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < src->ndim; d++) outShape[d] = src->shape[d];
    outShape[1] = ccount;

    int outer = src->shape[0];
    int inner = 1;
    for (int d = 2; d < src->ndim; d++) inner *= src->shape[d];

    ObjTensor* t = obj_new_tensor_nd(src->ndim, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    for (int nn = 0; nn < outer; nn++) {
        for (int c = 0; c < ccount; c++)
            for (int k = 0; k < inner; k++)
                td[(nn * ccount + c) * inner + k] =
                    sd[(nn * Cin + cstart + c) * inner + k];
    }
    if (!g_no_grad) {   /*  : feuille sous no-grad (cf. obj_tensor_concat_ch) */
        t->op = TENSOR_OP_SLICE_CH;
        t->op_axis = cstart;
        t->parents[0] = src;
        t->nParents = 1;
    }
    return t;
}

/*  — sous-Tenseur mat[i] : selection de la tranche `index1` (base 1) le
 * long de l'axe 0. Rend un Tenseur de rang N-1 dont les valeurs sont la copie
 * contigue de cette tranche (stockage plat row-major : la tranche `i` occupe le
 * bloc [i*inner, (i+1)*inner)). `src` roote par le caller pendant l'unique point
 * d'allocation (obj_new_tensor_nd) ; ensuite copie de doubles, aucune allocation
 * OWL. Rend NULL si rang < 2 ou index hors [1, forme[0]]. */
ObjTensor* obj_tensor_select(ObjTensor* src, int index1) {
    if (src->ndim < 2) return NULL;
    int i = index1 - 1;
    if (i < 0 || i >= src->shape[0]) return NULL;

    int outNdim = src->ndim - 1;
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < outNdim; d++) outShape[d] = src->shape[d + 1];

    int inner = 1;   /* numel de la tranche = produit des axes 1..N-1 */
    for (int d = 1; d < src->ndim; d++) inner *= src->shape[d];

    ObjTensor* t = obj_new_tensor_nd(outNdim, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    for (int k = 0; k < inner; k++) td[k] = sd[i * inner + k];

    if (!g_no_grad) {   /*  : feuille sous no-grad (cf. obj_tensor_slice_ch) */
        t->op = TENSOR_OP_SELECT;
        t->op_axis = i;   /* index 0-based selectionne le long de l'axe 0 */
        t->parents[0] = src;
        t->nParents = 1;
    }
    return t;
}

/*  — upsample spatial nearest-neighbor (asymmetric/floor). Duplique
 * chaque element source sy fois en hauteur et sx fois en largeur (2 derniers
 * axes) : out[.., y, x] = src[.., y/sy, x/sx]. `src` roote par le caller pendant
 * l'unique point d'allocation (obj_new_tensor_nd) ; ensuite copie de doubles,
 * aucune allocation OWL. Feuille (forward-only) : un graphe ONNX importe ne se
 * retropropage pas. Rend NULL si bornes invalides. */
ObjTensor* obj_tensor_upsample_nearest(ObjTensor* src, int sy, int sx) {
    if (src->ndim < 2 || sy <= 0 || sx <= 0) return NULL;
    int H = src->shape[src->ndim - 2];
    int W = src->shape[src->ndim - 1];
    int OH = H * sy, OW = W * sx;

    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < src->ndim; d++) outShape[d] = src->shape[d];
    outShape[src->ndim - 2] = OH;
    outShape[src->ndim - 1] = OW;

    /* outer = produit des axes avant les 2 derniers (N*C...) : chaque carte HxW
     * est un bloc contigu upsample independamment. */
    int outer = 1;
    for (int d = 0; d < src->ndim - 2; d++) outer *= src->shape[d];

    ObjTensor* t = obj_new_tensor_nd(src->ndim, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    for (int o = 0; o < outer; o++) {
        const double* sp = sd + (size_t)o * H * W;
        double* tp = td + (size_t)o * OH * OW;
        for (int y = 0; y < OH; y++) {
            const double* srow = sp + (size_t)(y / sy) * W;   /* floor, asymmetric */
            double* trow = tp + (size_t)y * OW;
            for (int x = 0; x < OW; x++) trow[x] = srow[x / sx];
        }
    }
    return t;   /* feuille : forward-only, pas de nœud d'op */
}

/*  — Transpose general (permutation d'axes quelconque). Forward-only :
 * l'import ONNX n'est pas retropropage. `src` roote par le caller (obj_new_tensor_nd
 * est l'unique alloc), ensuite copie de doubles. Rend NULL si rang hors bornes. */
ObjTensor* obj_tensor_transpose(ObjTensor* src, const int* perm) {
    int nd = src->ndim;
    if (nd < 1 || nd > OWL_TENSOR_MAX_RANK) return NULL;
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int i = 0; i < nd; i++) outShape[i] = src->shape[perm[i]];
    int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
    int acc = 1;
    for (int i = nd - 1; i >= 0; i--) { inStr[i] = acc; acc *= src->shape[i]; }
    acc = 1;
    for (int i = nd - 1; i >= 0; i--) { outStr[i] = acc; acc *= outShape[i]; }
    ObjTensor* t = obj_new_tensor_nd(nd, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(src->data);
    for (int outFlat = 0; outFlat < numel; outFlat++) {
        /* pour chaque position de sortie, retrouver la source : l'axe de sortie
         * m vient de l'axe source perm[m]. */
        int rem = outFlat, inFlat = 0;
        for (int m = 0; m < nd; m++) {
            int im = rem / outStr[m];
            rem %= outStr[m];
            inFlat += im * inStr[perm[m]];
        }
        td[outFlat] = sd[inFlat];
    }
    return t;   /* feuille : forward-only */
}

/*  — Softmax stable sur `axis`. Soustraction du max le long de l'axe
 * avant exp (parite onnxruntime), puis normalisation. Forward-only. */
ObjTensor* obj_tensor_softmax(ObjTensor* src, int axis) {
    int nd = src->ndim;
    if (axis < 0) axis += nd;
    if (axis < 0 || axis >= nd) return NULL;
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int i = 0; i < nd; i++) outShape[i] = src->shape[i];
    ObjTensor* t = obj_new_tensor_nd(nd, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(src->data);
    int axLen = src->shape[axis];
    int axStr = 1;                          /* stride de l'axe (row-major) */
    for (int i = nd - 1; i > axis; i--) axStr *= src->shape[i];
    int blockSize = axLen * axStr;
    int outerBlocks = (blockSize > 0) ? numel / blockSize : 0;
    for (int o = 0; o < outerBlocks; o++) {
        for (int k = 0; k < axStr; k++) {
            int base = o * blockSize + k;
            double mx = -INFINITY;
            for (int j = 0; j < axLen; j++) {
                double v = sd[base + j * axStr];
                if (v > mx) mx = v;
            }
            double sum = 0.0;
            for (int j = 0; j < axLen; j++) {
                double e = exp(sd[base + j * axStr] - mx);
                td[base + j * axStr] = e;
                sum += e;
            }
            for (int j = 0; j < axLen; j++) td[base + j * axStr] /= sum;
        }
    }
    return t;   /* feuille : forward-only */
}

/*  — Slice ONNX general. Forward-only. Voir object.h pour la semantique
 * (indices negatifs, clamp, pas >= 1). */
ObjTensor* obj_tensor_slice(ObjTensor* src, int nax, const int* axes,
                            const int* starts, const int* ends, const int* steps) {
    int nd = src->ndim;
    /* start/step/count effectifs par axe (defaut : tout l'axe, pas 1). */
    int st[OWL_TENSOR_MAX_RANK], sp[OWL_TENSOR_MAX_RANK], cnt[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < nd; d++) { st[d] = 0; sp[d] = 1; cnt[d] = src->shape[d]; }
    for (int i = 0; i < nax; i++) {
        int ax = axes[i];
        if (ax < 0) ax += nd;
        if (ax < 0 || ax >= nd) return NULL;
        int dim = src->shape[ax];
        int step = steps[i];
        if (step <= 0) return NULL;         /* pas negatif/nul hors perimetre */
        int s = starts[i], e = ends[i];
        if (s < 0) s += dim;
        if (e < 0) e += dim;
        if (s < 0) s = 0;  if (s > dim) s = dim;
        if (e < 0) e = 0;  if (e > dim) e = dim;
        int c = (e > s) ? (e - s + step - 1) / step : 0;
        st[ax] = s; sp[ax] = step; cnt[ax] = c;
    }
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < nd; d++) outShape[d] = cnt[d];
    int inStr[OWL_TENSOR_MAX_RANK], outStr[OWL_TENSOR_MAX_RANK];
    int acc = 1;
    for (int d = nd - 1; d >= 0; d--) { inStr[d] = acc; acc *= src->shape[d]; }
    acc = 1;
    for (int d = nd - 1; d >= 0; d--) { outStr[d] = acc; acc *= outShape[d]; }
    ObjTensor* t = obj_new_tensor_nd(nd, outShape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(t->data);
    for (int outFlat = 0; outFlat < numel; outFlat++) {
        int rem = outFlat, inFlat = 0;
        for (int d = 0; d < nd; d++) {
            int idx = rem / outStr[d];
            rem %= outStr[d];
            inFlat += (st[d] + idx * sp[d]) * inStr[d];
        }
        td[outFlat] = sd[inFlat];
    }
    return t;   /* feuille : forward-only */
}

/*  — Concat sur un axe quelconque (generalise obj_tensor_concat_ch).
 * Forward-only. Meme decoupage en blocs contigus (outer avant l'axe, inner apres). */
ObjTensor* obj_tensor_concat_axis(ObjTensor* a, ObjTensor* b, int axis) {
    if (a->ndim != b->ndim) return NULL;
    int nd = a->ndim;
    if (axis < 0) axis += nd;
    if (axis < 0 || axis >= nd) return NULL;
    for (int d = 0; d < nd; d++) {
        if (d == axis) continue;
        if (a->shape[d] != b->shape[d]) return NULL;
    }
    int Aa = a->shape[axis], Ab = b->shape[axis];
    int outShape[OWL_TENSOR_MAX_RANK];
    for (int d = 0; d < nd; d++) outShape[d] = a->shape[d];
    outShape[axis] = Aa + Ab;
    int outer = 1; for (int d = 0; d < axis; d++)  outer *= a->shape[d];
    int inner = 1; for (int d = axis + 1; d < nd; d++) inner *= a->shape[d];
    int Cout = Aa + Ab;
    ObjTensor* t = obj_new_tensor_nd(nd, outShape);
    double* ad = tensor_cells(a->data);
    double* bd = tensor_cells(b->data);
    double* td = tensor_cells(t->data);
    for (int o = 0; o < outer; o++) {
        for (int c = 0; c < Aa; c++)
            for (int k = 0; k < inner; k++)
                td[(o * Cout + c) * inner + k] = ad[(o * Aa + c) * inner + k];
        for (int c = 0; c < Ab; c++)
            for (int k = 0; k < inner; k++)
                td[(o * Cout + Aa + c) * inner + k] = bd[(o * Ab + c) * inner + k];
    }
    return t;   /* feuille : forward-only */
}

/*  — element-par-element avec diffusion (broadcast). Forward-only :
 * l'import ONNX n'est pas retropropage (Add/Mul gardent leur variante
 * differentiable dediee, obj_tensor_binop). Reutilise la meme arithmetique
 * d'index broadcast que obj_tensor_binop. */
ObjTensor* obj_tensor_eltwise(int op, ObjTensor* a, ObjTensor* b) {
    int outShape[OWL_TENSOR_MAX_RANK];
    int ndimOut = tensor_broadcast_shape(a->ndim, a->shape,
                                         b->ndim, b->shape, outShape);
    if (ndimOut < 0) return NULL;
    ObjTensor* t = obj_new_tensor_nd(ndimOut, outShape);
    double* ad = tensor_cells(a->data);
    double* bd = tensor_cells(b->data);
    double* td = tensor_cells(t->data);
    int numel = tensor_numel(t->data);
    if (tensor_shapes_equal(a, b)) {
        for (int i = 0; i < numel; i++) {
            double x = ad[i], y = bd[i];
            td[i] = (op == IA_ELT_ADD) ? x + y
                  : (op == IA_ELT_SUB) ? x - y
                  : (op == IA_ELT_MUL) ? x * y : x / y;
        }
    } else {
        int bsA[OWL_TENSOR_MAX_RANK], bsB[OWL_TENSOR_MAX_RANK];
        tensor_broadcast_strides(a->ndim, a->shape, ndimOut, bsA);
        tensor_broadcast_strides(b->ndim, b->shape, ndimOut, bsB);
        for (int fo = 0; fo < numel; fo++) {
            int rem = fo, ia = 0, ib = 0;
            for (int d = ndimOut - 1; d >= 0; d--) {
                int idx = rem % outShape[d];
                rem /= outShape[d];
                ia += idx * bsA[d];
                ib += idx * bsB[d];
            }
            double x = ad[ia], y = bd[ib];
            td[fo] = (op == IA_ELT_ADD) ? x + y
                   : (op == IA_ELT_SUB) ? x - y
                   : (op == IA_ELT_MUL) ? x * y : x / y;
        }
    }
    return t;   /* feuille : forward-only */
}

/*  — noeud RESHAPE differentiable. `src` roote par le caller pendant
 * l'unique point d'allocation (obj_new_tensor_nd) ; ensuite copie plate des
 * doubles, aucune allocation OWL. Le produit des dimensions DOIT egaler le
 * nombre d'elements de src (garanti par le caller). */
ObjTensor* obj_tensor_reshape(ObjTensor* src, int ndim, const int* shape) {
    ObjTensor* t = obj_new_tensor_nd(ndim, shape);
    double* sd = tensor_cells(src->data);
    double* td = tensor_cells(t->data);
    int n = tensor_numel(src->data);
    for (int i = 0; i < n; i++) td[i] = sd[i];
    if (!g_no_grad) {   /*  : feuille sous no-grad (cf. obj_tensor_binop) */
        t->op = TENSOR_OP_RESHAPE;
        t->parents[0] = src;
        t->nParents = 1;
    }
    return t;
}

/*  — IAModèle vide. Aucune allocation OWL secondaire (le tableau
 * de couches est C-heap, alloue au premier ajout), donc pas de rooting. */
ObjIAModele* obj_new_iamodele(void) {
    ObjIAModele* m = ALLOCATE_OBJ(ObjIAModele, OBJ_IAMODELE);
    m->layers = NULL;
    m->count = 0;
    m->capacity = 0;
    m->branchDepth = 0;   /*  */
    /*  — pas de graphe general par defaut (modele sequentiel). */
    m->gnodes = NULL;
    m->ngnodes = 0;
    m->gcapacity = 0;
    m->nvalues = 0;
    m->graphInSlot = -1;
    m->graphOutSlot = -1;
    m->inputH = 0;   /*  — forme d'entree inconnue tant qu'aucun ONNX importe */
    m->inputW = 0;
    return m;
}

void obj_iamodele_push_layer(ObjIAModele* m, IALayerKind kind,
                             ObjTensor* weights, ObjTensor* bias) {
    if (m->count >= m->capacity) {
        int newCap = (m->capacity == 0) ? 4 : (m->capacity * 2);
        m->layers = (IALayer*)realloc(m->layers,
                                      (size_t)newCap * sizeof(IALayer));
        m->capacity = newCap;
    }
    m->layers[m->count].kind    = kind;
    m->layers[m->count].weights = weights;
    m->layers[m->count].bias    = bias;
    /*  — champs des couches spatiales : defauts neutres, remplis apres
     * coup par les fonctions d'ajout conv/pooling/batchnorm. */
    m->layers[m->count].gamma   = NULL;
    m->layers[m->count].beta    = NULL;
    m->layers[m->count].running_mean = NULL;   /*  — stats mobiles BN */
    m->layers[m->count].running_var  = NULL;
    m->layers[m->count].stride  = 0;
    m->layers[m->count].pad     = 0;
    m->layers[m->count].window  = 0;
    m->layers[m->count].eps     = 0.0;
    m->layers[m->count].frozen  = false;   /*  : entrainable par defaut */
    m->count++;
}

ObjMLArbre* obj_new_mlarbre(void) {
    ObjMLArbre* a = ALLOCATE_OBJ(ObjMLArbre, OBJ_MLARBRE);
    a->objective       = 0;   /* non réglé : .Entraîne exige un objectif explicite */
    a->maxDepth        = 0;   /* 0 = profondeur illimitée */
    a->minSamplesSplit = 2;
    a->minSamplesLeaf  = 1;
    a->root            = NULL;
    return a;
}

void obj_mlarbre_free_tree(MLTreeNode* node) {
    if (node == NULL) return;
    obj_mlarbre_free_tree(node->left);
    obj_mlarbre_free_tree(node->right);
    free(node);
}

ObjMLForet* obj_new_mlforet(void) {
    ObjMLForet* f = ALLOCATE_OBJ(ObjMLForet, OBJ_MLFORET);
    f->objective       = 0;    /* non réglé : .Entraîne exige un objectif explicite */
    f->maxDepth        = 0;    /* 0 = profondeur illimitée */
    f->minSamplesSplit = 2;
    f->minSamplesLeaf  = 1;
    f->numTrees        = 100;  /* défaut : ensemble de 100 arbres */
    f->maxFeatures     = 0;    /* 0 = auto (√nb en classif, toutes en régression) */
    f->seed            = 0;    /* 0 = graine fixe reproductible */
    f->trees           = NULL;
    f->treeCount       = 0;
    f->nClasses        = 1;
    return f;
}

void obj_mlforet_free_trees(ObjMLForet* f) {
    if (f->trees == NULL) return;
    for (int i = 0; i < f->treeCount; i++) obj_mlarbre_free_tree(f->trees[i]);
    free(f->trees);
    f->trees = NULL;
    f->treeCount = 0;
}

ObjMLBoosting* obj_new_mlboosting(void) {
    ObjMLBoosting* b = ALLOCATE_OBJ(ObjMLBoosting, OBJ_MLBOOSTING);
    b->objective       = 0;      /* non réglé : .Entraîne exige un objectif explicite */
    b->numTrees        = 100;    /* défaut : 100 étapes de boosting */
    b->learningRate    = 0.1;    /* défaut : shrinkage 0.1 */
    b->maxDepth        = 3;      /* défaut : arbres peu profonds (≠ 0 illimité de l'arbre) */
    b->minSamplesSplit = 2;
    b->minSamplesLeaf  = 1;
    b->init0           = NULL;
    b->trees           = NULL;
    b->treeCount       = 0;
    b->nSteps          = 0;
    b->nClasses        = 1;
    return b;
}

void obj_mlboosting_free_trees(ObjMLBoosting* b) {
    if (b->trees != NULL) {
        for (int i = 0; i < b->treeCount; i++) obj_mlarbre_free_tree(b->trees[i]);
        free(b->trees);
        b->trees = NULL;
    }
    free(b->init0);
    b->init0 = NULL;
    b->treeCount = 0;
    b->nSteps = 0;
}

ObjMLLineaire* obj_new_mllineaire(void) {
    ObjMLLineaire* r = ALLOCATE_OBJ(ObjMLLineaire, OBJ_MLLINEAIRE);
    r->nCoef     = 0;
    r->coef      = NULL;
    r->intercept = 0.0;
    r->trained   = false;
    return r;
}

void obj_mllineaire_free_weights(ObjMLLineaire* r) {
    free(r->coef);
    r->coef = NULL;
    r->nCoef = 0;
}

ObjMLKMeans* obj_new_mlkmeans(void) {
    ObjMLKMeans* k = ALLOCATE_OBJ(ObjMLKMeans, OBJ_MLKMEANS);
    k->numClusters   = 8;     /* défaut ..NbClusters (comme sklearn) */
    k->numIterations = 300;   /* défaut ..NbItérations (max Lloyd) */
    k->seed          = 0;     /* défaut ..Graine (graine fixe reproductible) */
    k->nFeatures     = 0;
    k->centroids     = NULL;
    k->trained       = false;
    return k;
}

void obj_mlkmeans_free_centroids(ObjMLKMeans* k) {
    free(k->centroids);
    k->centroids = NULL;
    k->nFeatures = 0;
}

ObjMLLogistique* obj_new_mllogistique(void) {
    ObjMLLogistique* r = ALLOCATE_OBJ(ObjMLLogistique, OBJ_MLLOGISTIQUE);
    r->learningRate  = 0.1;   /* défaut ..TauxApprentissage */
    r->numIterations = 500;   /* défaut ..NbItérations */
    r->nClasses      = 0;
    r->nFeatures     = 0;
    r->W             = NULL;
    r->bias          = NULL;
    r->trained       = false;
    return r;
}

void obj_mllogistique_free_weights(ObjMLLogistique* r) {
    free(r->W);
    free(r->bias);
    r->W = NULL;
    r->bias = NULL;
    r->nClasses = 0;
    r->nFeatures = 0;
}

ObjOptionsEntrainement* obj_new_options_entrainement(void) {
    ObjOptionsEntrainement* o =
        ALLOCATE_OBJ(ObjOptionsEntrainement, OBJ_OPTIONS_ENTRAINEMENT);
    o->epochs       = 1;
    o->batchSize    = 32;
    o->learningRate = 0.01;
    o->optimizer    = OWL_OPTIM_SGD;
    o->loss         = OWL_PERTE_ENTROPIE_CROISEE;
    o->lrSchedule   = OWL_PLAN_CONSTANT;   /*  : taux fixe par defaut */
    o->weightDecay  = 0.0;                 /*  : L2 desactive par defaut */
    o->validationFraction = 0.0;           /*  : split validation off par defaut */
    o->keepBestModel      = 0;             /*  : save-best off par defaut */
    o->stopPatience       = 0;             /*  : early-stopping off par defaut */
    o->onEpoch      = NIL_VAL;
    return o;
}

/*  — ImageÉtiquetée. path/label DOIVENT etre rootes par le caller :
 * ALLOCATE_OBJ peut declencher un GC avant qu'ils soient references par
 * l'enveloppe. image reste NULL (materialisation paresseuse cote VM). */
ObjImageEtiquetee* obj_new_image_etiquetee(ObjString* path, ObjString* label) {
    ObjImageEtiquetee* ie =
        ALLOCATE_OBJ(ObjImageEtiquetee, OBJ_IMAGE_ETIQUETEE);
    ie->path  = path;
    ie->label = label;
    ie->image = NULL;
    return ie;
}

/*  — ImageAnnotée. path/boxes DOIVENT etre rootes par le caller :
 * ALLOCATE_OBJ peut declencher un GC avant qu'ils soient references par
 * l'enveloppe. image reste NULL (materialisation paresseuse cote VM). */
ObjImageAnnotee* obj_new_image_annotee(ObjString* path, Obj* boxes) {
    ObjImageAnnotee* ia =
        ALLOCATE_OBJ(ObjImageAnnotee, OBJ_IMAGE_ANNOTEE);
    ia->path  = path;
    ia->boxes = boxes;
    ia->image = NULL;
    return ia;
}

/*  — Dataset vide. Tableaux C-heap alloues au premier push. */
ObjDataset* obj_new_dataset(void) {
    ObjDataset* ds = ALLOCATE_OBJ(ObjDataset, OBJ_DATASET);
    ds->items      = NULL;
    ds->itemType   = OBJ_IMAGE_ETIQUETEE;  /* defaut classification, pose par le chargeur */
    ds->count      = 0;
    ds->capacity   = 0;
    ds->classes    = NULL;
    ds->classCount = 0;
    ds->source     = NULL;
    ds->flipHorizontalActif = false;
    ds->margeRecadrage      = 0;
    return ds;
}

void obj_dataset_push_item(ObjDataset* ds, Obj* item) {
    if (ds->count >= ds->capacity) {
        int newCap = (ds->capacity == 0) ? 8 : (ds->capacity * 2);
        ds->items = (Obj**)realloc(
            ds->items, (size_t)newCap * sizeof(Obj*));
        ds->capacity = newCap;
    }
    ds->items[ds->count++] = item;
}

void obj_dataset_push_class(ObjDataset* ds, ObjString* name) {
    /* Tableau de classes dimensionne exactement (peu d'entrees, croissance +1
     * acceptable : une classe de plus par appel, jamais dans une boucle
     * chaude). */
    ds->classes = (ObjString**)realloc(
        ds->classes, (size_t)(ds->classCount + 1) * sizeof(ObjString*));
    ds->classes[ds->classCount++] = name;
}

/*  — callback de liberation des handles llama (decouplage object.c
 * <-> llama, cf ObjZipArchive/g_zip_backend_close). NULL tant que stdlib_llm
 * n'a pas tourne : dans ce cas les handles restent NULL (aucun .Charge n'a pu
 * reussir sans stdlib_llm), donc rien a liberer. */
static LlmBackendFreeFn g_llm_backend_free = NULL;

void obj_llm_set_backend_free(LlmBackendFreeFn fn) {
    g_llm_backend_free = fn;
}

void obj_llm_free_handles(ObjLLM* l) {
    if (l == NULL) return;
    if (g_llm_backend_free != NULL) g_llm_backend_free(l);
    l->ctx = NULL;
    l->model = NULL;
    l->vocab = NULL;
}

ObjLLM* obj_new_llm(void) {
    ObjLLM* l = ALLOCATE_OBJ(ObjLLM, OBJ_LLM);
    l->model = NULL;
    l->ctx = NULL;
    l->vocab = NULL;
    return l;
}

ObjOptionsGeneration* obj_new_options_generation(void) {
    ObjOptionsGeneration* o =
        ALLOCATE_OBJ(ObjOptionsGeneration, OBJ_OPTIONS_GENERATION);
    o->temperature = 0.8;
    o->topK        = 40;
    o->topP        = 0.95;
    o->maxTokens   = 512;
    /* Graine aléatoire par défaut (une génération non-greedy varie d'un run a
     * l'autre tant que l'utilisateur ne fixe pas ..Graine). En mode greedy
     * (température 0) la graine n'a aucun effet : la reproductibilité tient. */
    o->seed        = (int64_t)(unsigned)rand();
    o->onToken     = NIL_VAL;
    o->constraint  = NIL_VAL;   /* contrainteLibre : aucune contrainte de sortie */
    return o;
}

ObjMessageLLM* obj_new_message_llm(void) {
    ObjMessageLLM* m = ALLOCATE_OBJ(ObjMessageLLM, OBJ_MESSAGE_LLM);
    m->role    = NIL_VAL;
    m->content = NIL_VAL;
    return m;
}

ObjResponseLLM* obj_new_response_llm(void) {
    ObjResponseLLM* r = ALLOCATE_OBJ(ObjResponseLLM, OBJ_RESPONSE_LLM);
    r->content      = NIL_VAL;
    r->reason       = OWL_STOP_NATUREL;  /* valeur neutre par defaut */
    r->inputTokens  = 0;
    r->outputTokens = 0;
    return r;
}

ObjDetection* obj_new_detection(Value box, int classIndex, double score) {
    /* box passe en argument : le caller le garde roote pendant ALLOCATE_OBJ (qui
     * peut GC). L'assignation de det->box le rend ensuite reachable par le GC. */
    ObjDetection* d = ALLOCATE_OBJ(ObjDetection, OBJ_DETECTION);
    d->box        = box;
    d->classIndex = classIndex;
    d->score      = score;
    return d;
}

/* Retourne la valeur par défaut pour un elementPrimKind donné.
 * 0 / inconnu -> NIL_VAL (Variant-like). Utilisé par les extensions
 * de tableau pour fill les cells intermédiaires avec la zero-value
 * du type déclaré. */
Value obj_array_default_for_kind(int kind) {
    switch (kind) {
        case 1: return OBJ_VAL(obj_copy_string("", 0));    /* chaîne */
        case 2: return INTEGER_VAL(0);                      /* entier */
        case 3: return REAL_VAL(0.0);                       /* réel */
        case 4: return BOOL_VAL(false);                     /* booléen */
        case 5: return CURRENCY_VAL(0);                     /* monétaire */
        case 6: return numeric_from_i64(0);                 /* numérique */
        default: return NIL_VAL;
    }
}

/* ================================================================
 * BUFFER
 * ================================================================ */

ObjBuffer* obj_new_buffer(const void* bytes, int length) {
    ObjBuffer* b = ALLOCATE_OBJ(ObjBuffer, OBJ_BUFFER);
    b->length = length;
    b->capacity = length > 0 ? length : 0;
    b->bytes = NULL;
    if (b->capacity > 0) {
        b->bytes = (unsigned char*)malloc((size_t)b->capacity);
        if (bytes && length > 0) memcpy(b->bytes, bytes, (size_t)length);
    }
    return b;
}

/* OBJ_NUMERIC : alloue l'enveloppe GC + un mpd_t neuf (= NaN tant qu'aucune
 * valeur n'est posee ; les helpers numeric_from_* l'initialisent aussitot).
 * mpd_qnew vit sur malloc/free standard, libere au sweep par object_free. */
ObjNumeric* obj_new_numeric(void) {
    ObjNumeric* n = ALLOCATE_OBJ(ObjNumeric, OBJ_NUMERIC);
    n->dec = mpd_qnew();
    return n;
}

ObjBuffer* obj_buffer_empty(int initial_capacity) {
    ObjBuffer* b = ALLOCATE_OBJ(ObjBuffer, OBJ_BUFFER);
    b->length = 0;
    b->capacity = initial_capacity > 0 ? initial_capacity : 0;
    b->bytes = b->capacity > 0 ? (unsigned char*)malloc((size_t)b->capacity) : NULL;
    return b;
}

void obj_buffer_append(ObjBuffer* buf, const void* data, int len) {
    if (len <= 0) return;
    if (buf->length + len > buf->capacity) {
        int nc = buf->capacity > 0 ? buf->capacity * 2 : 16;
        while (nc < buf->length + len) nc *= 2;
        unsigned char* nb = (unsigned char*)realloc(buf->bytes, (size_t)nc);
        if (!nb) return;
        buf->bytes = nb;
        buf->capacity = nc;
    }
    memcpy(buf->bytes + buf->length, data, (size_t)len);
    buf->length += len;
}

ObjZipArchive* obj_new_zip_archive(ZipArchiveFormat fmt, const char* path,
                                   ZipArchiveMode mode) {
    ObjZipArchive* z = ALLOCATE_OBJ(ObjZipArchive, OBJ_ZIP_ARCHIVE);
    z->format = fmt;
    z->mode   = mode;
    /* Duplicate the path so the struct owns it (caller may free the source).
     * NULL path is allowed for future in-memory archives. */
    if (path) {
        size_t n = strlen(path);
        z->path = (char*)malloc(n + 1);
        if (z->path) memcpy(z->path, path, n + 1);
    } else {
        z->path = NULL;
    }
    z->backend = NULL;
    z->password = NULL;
    z->compressionLevel = 6;
    z->open = false;
    z->lastError = 0;
    z->lastErrorMsg[0] = '\0';
    z->fileCount = 0;
    return z;
}

/* ================================================================
 * MOCK NATIVE CREATION
 * ================================================================ */

ObjMockNative* obj_new_mock_native(int mockIndex) {
    ObjMockNative* mock = ALLOCATE_OBJ(ObjMockNative, OBJ_MOCK_NATIVE);
    mock->mockIndex = mockIndex;
    return mock;
}

/* ================================================================
 * PATTERN CREATION
 * ================================================================ */

ObjPattern* obj_new_pattern(int patternType, Value value) {
    /*  : push value (si Obj) pendant ALLOCATE_OBJ. */
    bool valueIsObj = IS_OBJ(value);
    if (valueIsObj) vm_push(value);
    ObjPattern* pattern = ALLOCATE_OBJ(ObjPattern, OBJ_PATTERN);
    if (valueIsObj) vm_pop();
    pattern->patternType = patternType;
    pattern->value = value;
    return pattern;
}

/*  — cle canonique unique pour le headless_store. Resout la prop
 * via le catalogue lang-aware (casse/accent/FR<->EN-insensible) et indexe
 * sur le nom canon (nameAscii, sinon nameFR), lowercase. Garantit qu'une
 * meme prop ecrite en FR (`Largeur`) et lue en EN (`Width`) tape la MEME
 * cle. Le SET et le GET passent tous deux par ce seul helper : pas de
 * desync possible. Generalise le pattern  (deja en place pour les
 * props composees Cadre/Police) a TOUTES les props headless.
 *
 * Fallback : prop hors-catalog (clé interne, kind inconnu) -> on garde le
 * comportement historique (nom tape, lowercase ASCII). Les accents sont
 * supposes deja strippes par scanner_strip_accents cote compiler.
 *
 * Cohérence avec  : les sites composes calculent deja explicitement
 * `pdef->nameAscii ? : nameFR` avant d'appeler ces helpers. Re-resoudre ce
 * nom canon via le catalog est idempotent (le catalog le retrouve et renvoie
 * le meme nom canon) -> aucun double-mecanisme conflictuel. */
static int headless_key_normalize_canon(int kind, const char* src,
                                         char* buf, int bufSize) {
    const char* key = src;
    const OwlControlPropDef* pdef =
        owl_control_catalog_find((OwlControlKind)kind, src);
    if (pdef != NULL) {
        key = pdef->nameAscii ? pdef->nameAscii : pdef->nameFR;
    }
    int i = 0;
    while (key[i] != '\0' && i < bufSize - 1) {
        unsigned char c = (unsigned char)key[i];
        buf[i] = (c < 0x80) ? (char)tolower(c) : (char)c;
        i++;
    }
    buf[i] = '\0';
    return i;
}

/*  — ..Valeur (et toute prop chaine) d'un Champ de Saisie est figee
 * chaine au runtime. Une affectation par programme d'un type non-chaine
 * (`SAI..Valeur = 42`) coerce en chaine via value_to_string, pour aligner le
 * store sur le type chaine declare au catalogue ET sur la saisie clavier (qui
 * rend deja une chaine). Sans ca, `SAI..Valeur + 1` valait 43 (entier brut
 * conserve) alors que la meme Saisie tapee rendait "42" -> "421".
 *
 * Strictement borne au kind Saisie (OWL_CONTROL_INPUT) : les widgets de
 * selection (Liste/Combo/Selecteur) portent une Valeur = index/clef interne,
 * traitee ailleurs, jamais coercee ici. Les props OBJECT (Cadre/Police) ne
 * matchent pas OWL_PROP_TYPE_STRING -> intactes (materialisation composite
 * safe). Booleen -> "1"/"0" (coercion chaine OWL standard, cf `"" + Vrai`). */
Value ui_input_coerce_string_prop(int kind, const char* prop, Value v) {
    if (kind != OWL_CONTROL_INPUT) return v;
    if (IS_STRING(v)) return v;
    const OwlControlPropDef* pdef =
        owl_control_catalog_find((OwlControlKind)kind, prop);
    if (pdef == NULL) return v;
    /*  ( §3) — ..Valeur / ..ContenuInitial d'une Saisie sont
     * retypees MULTILINGUE : elles RETIENNENT un vrai litteral multilingue
     * (re-resolu a la lecture, hot-switch de langue) mais FIGENT tout scalaire
     * (entier/reel/booleen) en chaine sur la langue courante, de sorte que la
     * relecture rende toujours une chaine ( : `SAI..Valeur = 42` puis
     * `..Valeur + 1` vaut "421", pas 43). Les autres slots MULTILINGUE d'une
     * Saisie (Indication/Libelle, texte d'UI indexable) restent intacts. */
    if (pdef->type == OWL_PROP_TYPE_MULTILINGUE) {
        if (pdef->nameFR != NULL &&
            (strcmp(pdef->nameFR, "Valeur") == 0 ||
             strcmp(pdef->nameFR, "ContenuInitial") == 0)) {
            if (IS_MULTILINGUE(v)) return v;      /* retient le litteral */
            return OBJ_VAL(value_to_string(v));   /* fige le scalaire en chaine */
        }
        return v;
    }
    if (pdef->type != OWL_PROP_TYPE_STRING) return v;
    return OBJ_VAL(value_to_string(v));
}

bool obj_ui_control_headless_get(ObjUiControl* c, const char* prop, Value* out) {
    if (!c || !c->headless_store || !prop) return false;
    char buf[128];
    int len = headless_key_normalize_canon(c->kind, prop, buf, sizeof(buf));
    ObjString* key = obj_copy_string(buf, len);
    Value v;
    if (table_get(c->headless_store, key, &v)) {
        if (out) *out = v;
        return true;
    }
    return false;
}

void obj_ui_control_headless_set(ObjUiControl* c, const char* prop, Value v) {
    if (!c || !prop) return;
    if (!c->headless_store) {
        c->headless_store = (Table*)malloc(sizeof(Table));
        table_init(c->headless_store);
    }
    v = ui_input_coerce_string_prop(c->kind, prop, v); /*  */
    /*   : la coercion peut allouer une ObjString fraiche
     * (value_to_string). La rooter AVANT obj_copy_string(key) qui peut
     * grow → GC, sinon v est collecte/poisonne avant le store. */
    if (IS_OBJ(v)) vm_push(v);
    char buf[128];
    int len = headless_key_normalize_canon(c->kind, prop, buf, sizeof(buf));
    /*   : push key + v pendant table_set. Cf
     * obj_ui_control_instance_headless_set. */
    ObjString* key = obj_copy_string(buf, len);
    vm_push(OBJ_VAL(key));
    table_set(c->headless_store, key, v);
    vm_pop(); /* key */
    if (IS_OBJ(v)) vm_pop(); /* v coerce */
}

/*  — pendants ObjUiControlInstance des helpers headless. Meme
 * normalisation canonique de cle pour que FR/EN/casse/accent
 * tapent la meme cle. Utilises par le dispatch OP_STRUCT_GET/SET sur
 * ObjUiControlRef (modele C ). */
bool obj_ui_control_instance_headless_get(ObjUiControlInstance* c,
                                          const char* prop, Value* out) {
    if (!c || !c->headless_store || !prop || !c->decl) return false;
    char buf[128];
    int len = headless_key_normalize_canon((int)c->decl->kind, prop,
                                            buf, sizeof(buf));
    ObjString* key = obj_copy_string(buf, len);
    Value v;
    if (table_get(c->headless_store, key, &v)) {
        if (out) *out = v;
        return true;
    }
    return false;
}

void obj_ui_control_instance_headless_set(ObjUiControlInstance* c,
                                          const char* prop, Value v) {
    if (!c || !prop || !c->decl) return;
    if (!c->headless_store) {
        c->headless_store = (Table*)malloc(sizeof(Table));
        table_init(c->headless_store);
    }
    v = ui_input_coerce_string_prop((int)c->decl->kind, prop, v); /*  */
    /*   : la coercion peut allouer une ObjString fraiche
     * (value_to_string). La rooter AVANT obj_copy_string(key) qui peut
     * grow → GC, sinon v est collecte/poisonne avant le store. */
    if (IS_OBJ(v)) vm_push(v);
    char buf[128];
    int len = headless_key_normalize_canon((int)c->decl->kind, prop,
                                            buf, sizeof(buf));
    /*   : push key + v pendant table_set qui peut grow → GC.
     * Sans ces push, key (fresh ObjString) ou v (si Obj) sont collectes,
     * la clef inseree est poisonnee et le lookup ulterieur retourne NIL
     * (libelle vide au lieu de la valeur declaree). */
    ObjString* key = obj_copy_string(buf, len);
    vm_push(OBJ_VAL(key));
    table_set(c->headless_store, key, v);
    /*  — pose la back-ref (controle, cle canon) sur l'instance
     * composite seedee : un `..Cadre..Couleur =` ulterieur (OP_STRUCT_SET sur
     * la sous-instance) pourra alors marquer la prop Cadre override cote
     * controle, empechant le reseed par bloc de la clobber. */
    if (IS_INSTANCE(v)) {
        ObjInstance* seeded = AS_INSTANCE(v);
        seeded->ui_owner = c;
        seeded->ui_owner_key = key;
    }
    vm_pop(); /* key */
    if (IS_OBJ(v)) vm_pop(); /* v coerce */
}

/*  — marque la prop comme override imperatif. Meme normalisation de
 * cle canonique que le store pour que FR/EN/casse/accent tapent la
 * meme entree. Set lazy : valeur BOOL_VAL(true) marqueur, seule la presence
 * compte. */
void obj_ui_control_instance_mark_override(ObjUiControlInstance* c,
                                           const char* prop) {
    if (!c || !prop || !c->decl) return;
    if (!c->headless_override_keys) {
        c->headless_override_keys = (Table*)malloc(sizeof(Table));
        table_init(c->headless_override_keys);
    }
    char buf[128];
    int len = headless_key_normalize_canon((int)c->decl->kind, prop,
                                            buf, sizeof(buf));
    /*  : key fresh, push pendant table_set qui peut grow -> GC. */
    ObjString* key = obj_copy_string(buf, len);
    vm_push(OBJ_VAL(key));
    table_set(c->headless_override_keys, key, BOOL_VAL(true));
    vm_pop(); /* key */
}

bool obj_ui_control_instance_is_override(ObjUiControlInstance* c,
                                         const char* prop) {
    if (!c || !c->headless_override_keys || !prop || !c->decl) return false;
    char buf[128];
    int len = headless_key_normalize_canon((int)c->decl->kind, prop,
                                            buf, sizeof(buf));
    ObjString* key = obj_copy_string(buf, len);
    Value v;
    return table_get(c->headless_override_keys, key, &v);
}

/*  lot B — normalisation cle pour les props fenetre.
 * Equivalent de headless_key_normalize_canon mais via owl_window_catalog_find
 * (catalogue dedie fenetre). Fallback lowercase ASCII si prop hors-catalog —
 * meme convention que les sous-controles. */
static int headless_key_normalize_window(const char* src,
                                          char* buf, int bufSize) {
    const char* key = src;
    const OwlControlPropDef* pdef = owl_window_catalog_find(src);
    if (pdef != NULL) {
        key = pdef->nameAscii ? pdef->nameAscii : pdef->nameFR;
    }
    int i = 0;
    while (key[i] != '\0' && i < bufSize - 1) {
        unsigned char c = (unsigned char)key[i];
        buf[i] = (c < 0x80) ? (char)tolower(c) : (char)c;
        i++;
    }
    buf[i] = '\0';
    return i;
}

bool obj_window_instance_headless_get(ObjWindowInstance* w,
                                      const char* prop, Value* out) {
    if (!w || !w->headless_store || !prop) return false;
    char buf[128];
    int len = headless_key_normalize_window(prop, buf, sizeof(buf));
    ObjString* key = obj_copy_string(buf, len);
    Value v;
    if (table_get(w->headless_store, key, &v)) {
        if (out) *out = v;
        return true;
    }
    return false;
}

void obj_window_instance_headless_set(ObjWindowInstance* w,
                                      const char* prop, Value v) {
    if (!w || !prop) return;
    if (!w->headless_store) {
        w->headless_store = (Table*)malloc(sizeof(Table));
        table_init(w->headless_store);
    }
    char buf[128];
    int len = headless_key_normalize_window(prop, buf, sizeof(buf));
    /*   : push key + v pendant table_set. Cf
     * obj_ui_control_instance_headless_set. */
    ObjString* key = obj_copy_string(buf, len);
    vm_push(OBJ_VAL(key));
    if (IS_OBJ(v)) vm_push(v);
    table_set(w->headless_store, key, v);
    if (IS_OBJ(v)) vm_pop();
    vm_pop(); /* key */
}

ObjUiControl* obj_new_ui_control(int kind, void* widget, const char* name) {
    ObjUiControl* c = ALLOCATE_OBJ(ObjUiControl, OBJ_UI_CONTROL);
    c->kind = kind;
    c->widget = widget;
    if (name) {
        size_t n = strlen(name);
        c->name = (char*)malloc(n + 1);
        memcpy(c->name, name, n + 1);
    } else {
        c->name = NULL;
    }
    /*  — lazy alloc : pas de Table tant qu'aucune prop n'est ecrite. */
    c->headless_store = NULL;
    return c;
}

/* ================================================================
 *  — Allocators multi-instance fenetre.
 *
 * Triade ObjWindowInstance / ObjUiControlInstance / ObjUiControlRef.
 * Infra pure : ces objets sont alloues mais aucune native ne les
 * produit encore (/0136 viendront cabler). Le GC les supporte
 * deja (cf gc.c switches).
 * ================================================================ */

/* Compteur monotone d'instance_id, partage par TOUTES les fenetres
 * dans le process. Tient sur 2^31 instances soit ~2 milliards — assez
 * pour tout cas humainement realiste. Reset a 0 dans vm_init via
 * vm.next_instance_id (cf vm.c). */

ObjWindowInstance* obj_new_window_instance(struct OwlWindowDecl* decl) {
    ObjWindowInstance* w = ALLOCATE_OBJ(ObjWindowInstance, OBJ_WINDOW_INSTANCE);
    /* Init *avant* toute exposition au GC : si une alloc downstream
     * declenche un cycle, on doit avoir des champs valides. */
    w->decl             = decl;
    w->widget           = NULL;
    w->instance_id      = vm.next_instance_id++;
    w->controls         = NULL;
    w->control_count    = 0;
    w->control_capacity = 0;
    w->valeur_renvoyee  = NIL_VAL;
    w->focused_index    = -1;    /*  — aucun focus avant l'ouverture */
    w->headless_store   = NULL;  /*  lot B — lazy comme uici */
    return w;
}

/*  lot B — allocator du proxy global fenetre. */
ObjWindowRef* obj_new_window_ref(struct OwlWindowDecl* decl, ObjString* name) {
    /*  : push name pendant ALLOCATE_OBJ qui peut grow → GC. */
    if (name != NULL) vm_push(OBJ_VAL(name));
    ObjWindowRef* r = ALLOCATE_OBJ(ObjWindowRef, OBJ_WINDOW_REF);
    if (name != NULL) vm_pop();
    r->decl = decl;
    r->name = name;
    r->fallback_store = NULL;
    return r;
}

ObjUiControlInstance* obj_new_ui_control_instance(struct OwlControlDecl* decl,
                                                   ObjWindowInstance* owner) {
    ObjUiControlInstance* c = ALLOCATE_OBJ(ObjUiControlInstance, OBJ_UI_CONTROL_INSTANCE);
    c->decl                   = decl;
    c->widget                 = NULL;
    c->headless_store         = NULL;  /* lazy comme ObjUiControl */
    c->headless_override_keys = NULL;  /*  — lazy, cf. mark_override */
    c->owner                  = owner;
    return c;
}

/*  — Append d'une ObjUiControlInstance dans le tableau dynamique
 * inst->controls. Croit la capacite par doublement (init = 4). Le caller
 * conserve la GC root sur uici (typiquement vm_push avant l'append). */
void obj_window_instance_add_control(ObjWindowInstance* inst,
                                      ObjUiControlInstance* uici) {
    if (!inst || !uici) return;
    if (inst->control_count >= inst->control_capacity) {
        int new_cap = inst->control_capacity < 4 ? 4 : inst->control_capacity * 2;
        inst->controls = (ObjUiControlInstance**)realloc(
            inst->controls,
            sizeof(ObjUiControlInstance*) * (size_t)new_cap);
        inst->control_capacity = new_cap;
    }
    inst->controls[inst->control_count++] = uici;
}

/*  lot D — allocator ObjEtatInstance. Miroir d'obj_new_window_instance.
 * decl jamais owned (vit dans vm.etats), controls[] grossit via
 * obj_etat_instance_add_control (doubling init = 4). decl typo OwlEtatDecl*,
 * passe en void* pour eviter le cycle etat_decl<->object cote header. */
ObjEtatInstance* obj_new_etat_instance(void* decl) {
    ObjEtatInstance* e = ALLOCATE_OBJ(ObjEtatInstance, OBJ_ETAT_INSTANCE);
    e->decl             = decl;
    e->controls         = NULL;
    e->control_count    = 0;
    e->control_capacity = 0;
    return e;
}

/*  lot D — append d'une ObjUiControlInstance dans etat->controls[]. */
void obj_etat_instance_add_control(ObjEtatInstance* inst,
                                    ObjUiControlInstance* uici) {
    if (!inst || !uici) return;
    if (inst->control_count >= inst->control_capacity) {
        int new_cap = inst->control_capacity < 4 ? 4 : inst->control_capacity * 2;
        inst->controls = (ObjUiControlInstance**)realloc(
            inst->controls,
            sizeof(ObjUiControlInstance*) * (size_t)new_cap);
        inst->control_capacity = new_cap;
    }
    inst->controls[inst->control_count++] = uici;
}

/*  — allocator ObjIcone. Lot 2b : structure + defauts uniquement.
 * La couche resolution catalogue (Lot 2d) appellera obj_new_icone() puis
 * remplira catalog_hash + svg_raw via affectation directe. Pareil pour les
 * SVG custom : le chargement (Lot 2e) renseignera svg_raw / valide. */
ObjIcone* obj_new_icone(void) {
    ObjIcone* icn = ALLOCATE_OBJ(ObjIcone, OBJ_ICONE);
    icn->catalog_hash       = 0;
    icn->svg_raw            = NULL;
    icn->couleur            = -1;   /* unset : ..Couleur jamais ecrit */
    icn->couleur_fond       = -1;   /*  : -1 = transparent (defaut) */
    icn->rotation           = 0;
    icn->forme              = 0;    /* formeAucune */
    icn->couleur_modifiable = false;
    icn->valide             = true;
    return icn;
}

ObjIconesNamespace* obj_new_icones_namespace(void) {
    ObjIconesNamespace* ns = ALLOCATE_OBJ(ObjIconesNamespace, OBJ_ICONES_NAMESPACE);
    return ns;
}

ObjThemeNamespace* obj_new_theme_namespace(void) {
    ObjThemeNamespace* ns = ALLOCATE_OBJ(ObjThemeNamespace, OBJ_THEME_NAMESPACE);
    return ns;
}

ObjUiControlRef* obj_new_ui_control_ref(struct OwlControlDecl* decl) {
    ObjUiControlRef* r = ALLOCATE_OBJ(ObjUiControlRef, OBJ_UI_CONTROL_REF);
    r->decl = decl;
    /* Cache de decl->kind : evite un load via decl* a chaque dispatch
     * runtime (kind est immutable apres parse). decl peut etre NULL en
     * theorie mais en pratique le compilo ne fabrique pas de ref sans
     * decl resolu — on garde le garde defensif quand meme. */
    r->kind = decl ? (int)decl->kind : 0;
    r->name = NULL;  /*  — set par obj_new_ui_control_ref_named */
    return r;
}

/*  — variante qui capture aussi le nom strippe pour resolution
 * dynamique stable (decl peut etre freed apres window_parse_subfield, cf.
 * owl_window_decl_add_control). Caller : pre-strippe + intern le nom. */
ObjUiControlRef* obj_new_ui_control_ref_named(struct OwlControlDecl* decl,
                                               ObjString* name) {
    /*  : push name pendant obj_new_ui_control_ref qui alloue. */
    if (name != NULL) vm_push(OBJ_VAL(name));
    ObjUiControlRef* r = obj_new_ui_control_ref(decl);
    if (name != NULL) vm_pop();
    r->name = name;
    return r;
}

/* Per-format close hook. Backends register themselves at init time so
 * the GC sweep can release their state without object.c knowing the
 * concrete type of `backend`. Until a backend lands for a given format
 * slot the hook is NULL and we merely free(backend) as a fallback — the
 * struct is plain malloc'd so that's always safe.
 * Typedef ZipBackendCloseFn defini dans object.h. */
static ZipBackendCloseFn g_zip_backend_close[7] = {0}; /* indexed by ZipArchiveFormat */

void owl_zip_register_backend_close(int format, ZipBackendCloseFn fn) {
    if (format >= 0 && format < 7) g_zip_backend_close[format] = fn;
}

void obj_zip_archive_close(ObjZipArchive* z) {
    if (!z) return;
    if (z->open) {
        ZipBackendCloseFn fn = g_zip_backend_close[z->format];
        if (fn) fn(z);
        else    free(z->backend); /* trivial fallback */
        z->backend = NULL;
        z->open = false;
    }
    if (z->path)     { free(z->path);     z->path = NULL; }
    if (z->password) {
        /* wipe before free to avoid leaking passwords in cold pages */
        size_t n = strlen(z->password);
        memset(z->password, 0, n);
        free(z->password);
        z->password = NULL;
    }
}

int value_as_bytes(Value v, const unsigned char** out_bytes, int* out_len) {
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        if (out_bytes) *out_bytes = (const unsigned char*)s->chars;
        if (out_len)   *out_len = s->length;
        return 1;
    }
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        if (out_bytes) *out_bytes = b->bytes;
        if (out_len)   *out_len = b->length;
        return 1;
    }
    return 0;
}

/* ================================================================
 * ASSOCIATIVE ARRAY
 * ================================================================ */

ObjAssocArray* obj_new_assoc(AssocKeyType keyType, int options, int valueKind) {
    ObjAssocArray* a = ALLOCATE_OBJ(ObjAssocArray, OBJ_ASSOC_ARRAY);
    a->keyType   = keyType;
    a->options   = options;
    a->valueKind = valueKind;
    a->keys      = NULL;
    a->values    = NULL;
    a->count     = 0;
    a->capacity  = 0;
    a->valueTypeName = NULL;
    return a;
}

Value obj_assoc_default_for_kind(int valueKind) {
    switch (valueKind) {
        case ASSOC_VALUE_CHAINE: {
            ObjString* empty = obj_copy_string("", 0);
            return OBJ_VAL(empty);
        }
        case ASSOC_VALUE_ENTIER:    return INTEGER_VAL(0);
        case ASSOC_VALUE_REEL:      return REAL_VAL(0.0);
        case ASSOC_VALUE_BOOLEEN:   return BOOL_VAL(false);
        case ASSOC_VALUE_MONETAIRE: return CURRENCY_VAL(0);
        case ASSOC_VALUE_NUMERIQUE:
            return numeric_from_i64(0);
        case ASSOC_VALUE_VARIANT:
        default:
            return NIL_VAL;
    }
}

static void assoc_grow_if_needed(ObjAssocArray* a) {
    if (a->count + 1 > a->capacity) {
        int new_cap = a->capacity < 4 ? 4 : a->capacity * 2;
        a->keys   = (ObjString**)realloc(a->keys,   sizeof(ObjString*) * new_cap);
        a->values = (Value*)     realloc(a->values, sizeof(Value)      * new_cap);
        a->capacity = new_cap;
    }
}

/* v1: string keys only. Returns index, or -1 if not found. */
static int assoc_find_index(const ObjAssocArray* a, const ObjString* key) {
    bool ci = (a->options & ASSOC_OPT_SANS_CASSE) != 0;  /* en-tetes HTTP : lookup casse-insensible */
    for (int i = 0; i < a->count; i++) {
        ObjString* k = a->keys[i];
        if (k == key) return i;                 /* interned same ptr */
        if (k->length == key->length &&
            (ci ? strncasecmp(k->chars, key->chars, (size_t)key->length) == 0
                : memcmp(k->chars, key->chars, (size_t)key->length) == 0)) return i;
    }
    return -1;
}

int obj_assoc_set(ObjAssocArray* a, ObjString* key, Value value) {
    int idx = assoc_find_index(a, key);
    if (idx >= 0) {
        a->values[idx] = value;
        return 0;
    }
    /*  : push a + key + value (si Obj) avant assoc_grow_if_needed
     * qui peut realloc keys/values → GC → collecte key et/ou value si
     * Obj fresh non-rooted (cas typique : value depuis callback retour). */
    vm_push(OBJ_VAL(a));
    vm_push(OBJ_VAL(key));
    bool valueIsObj = IS_OBJ(value);
    if (valueIsObj) vm_push(value);
    assoc_grow_if_needed(a);
    a->keys[a->count]   = key;
    a->values[a->count] = value;
    a->count++;
    if (valueIsObj) vm_pop();
    vm_pop(); /* key */
    vm_pop(); /* a */
    return 1;
}

Value obj_assoc_get(ObjAssocArray* a, ObjString* key, int* out_exists) {
    int idx = assoc_find_index(a, key);
    if (idx >= 0) {
        if (out_exists) *out_exists = 1;
        return a->values[idx];
    }
    if (out_exists) *out_exists = 0;
    return obj_assoc_default_for_kind(a->valueKind);
}

int obj_assoc_remove(ObjAssocArray* a, ObjString* key) {
    int idx = assoc_find_index(a, key);
    if (idx < 0) return 0;
    /* Shift tail down */
    for (int i = idx; i < a->count - 1; i++) {
        a->keys[i]   = a->keys[i + 1];
        a->values[i] = a->values[i + 1];
    }
    a->count--;
    return 1;
}

void obj_assoc_clear(ObjAssocArray* a) {
    a->count = 0;
}

ObjAssocArray* obj_assoc_clone(const ObjAssocArray* src) {
    ObjAssocArray* dst = obj_new_assoc(src->keyType, src->options, src->valueKind);
    dst->valueTypeName = src->valueTypeName;
    for (int i = 0; i < src->count; i++) {
        obj_assoc_set(dst, src->keys[i], src->values[i]);
    }
    return dst;
}

/* ================================================================
 * MULTILINGUE CREATION ( lot B)
 * ================================================================ */

ObjMultilingue* obj_new_multilingue(void) {
    /* Le storage est alloue AVANT le wrapper et root sur la stack
     * pendant ALLOCATE_OBJ : si owl_realloc declenche un GC entre les
     * deux allocations, le storage non-root serait sweep et le wrapper
     * pointerait dans le vide. Pattern identique a obj_new_instance pour
     * structName. */
    ObjAssocArray* storage = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
    vm_push(OBJ_VAL(storage));
    ObjMultilingue* m = ALLOCATE_OBJ(ObjMultilingue, OBJ_MULTILINGUE);
    vm_pop();
    m->storage = storage;
    return m;
}

/*  — insere/maj une entree multilingue avec cle canonicalisee BCP 47.
 * Les tags sont case-insensibles (RFC 5646) : on stocke toujours la forme
 * canonique pour que store et lookup s'accordent quelle que soit la casse
 * saisie (litteral inline, cast, m[tag] = "..."). Le caller garantit m rooté
 * (operande sur la pile VM) ; on ne roote ici que la cle canonique fraiche. */
void multilingue_store(ObjMultilingue* m, const char* tag, Value val) {
    if (m == NULL || tag == NULL) return;
    char canon[64];
    owl_bcp47_canon(tag, canon, sizeof(canon));
    const char* keyStr = (canon[0] != '\0') ? canon : tag;
    if (IS_OBJ(val)) vm_push(val);
    ObjString* key = obj_copy_string(keyStr, (int)strlen(keyStr));
    vm_push(OBJ_VAL(key));
    obj_assoc_set(m->storage, key, val);
    vm_pop();  /* key */
    if (IS_OBJ(val)) vm_pop();  /* val */
}

Value multilingue_resolve(ObjMultilingue* m, const char* lang, const char* langueSource) {
    /* Defense : pointeurs Null. La langue est obligatoire ; la source par
     * defaut "fr-FR" si null/empty. */
    if (m == NULL) return OBJ_VAL(obj_copy_string("", 0));
    if (langueSource == NULL || *langueSource == '\0') langueSource = "fr-FR";
    if (lang == NULL) lang = langueSource;

    /*  — canonicalise cle demandee et source : les entrees sont
     * stockees en forme canonique BCP 47, le lookup doit l'etre aussi pour
     * etre case-insensible (m["en-gb"] == m["en-GB"] == m[langueAnglaisRoyaumeUni]). */
    char langCanon[64], srcCanon[64];
    owl_bcp47_canon(lang, langCanon, sizeof(langCanon));
    owl_bcp47_canon(langueSource, srcCanon, sizeof(srcCanon));
    const char* langKey = (langCanon[0] != '\0') ? langCanon : lang;
    const char* srcKeyStr = (srcCanon[0] != '\0') ? srcCanon : langueSource;

    /* 1) Lookup direct : si la cle est presente (meme avec valeur ""),
     *    on retourne sa valeur — pas de fallback. Distinction "entree posee
     *    vide" vs "absente" requise par scenario_fallback_entree_vide_vs_absente. */
    ObjString* key = obj_copy_string(langKey, (int)strlen(langKey));
    int exists = 0;
    Value direct = obj_assoc_get(m->storage, key, &exists);
    if (exists) return direct;

    /* 2) Miss : fallback langue source si differente. */
    if (strcmp(langKey, srcKeyStr) != 0) {
        ObjString* srcKey = obj_copy_string(srcKeyStr, (int)strlen(srcKeyStr));
        Value fallback = obj_assoc_get(m->storage, srcKey, &exists);
        if (exists) return fallback;
    }

    /* 3) Source elle-meme absente : "". */
    return OBJ_VAL(obj_copy_string("", 0));
}

/* ================================================================
 * INSTANCE CREATION
 * ================================================================ */

ObjInstance* obj_new_instance(ObjString* structName) {
    /* Protect structName from GC during ALLOCATE_OBJ : sans ça, si le caller
     * a juste créé structName via obj_copy_string sans le rooter, un GC
     * déclenché par owl_realloc dans ALLOCATE_OBJ le sweep et instance->structName
     * pointerait sur de la mémoire libérée. */
    if (structName != NULL) vm_push(OBJ_VAL(structName));
    ObjInstance* instance = ALLOCATE_OBJ(ObjInstance, OBJ_INSTANCE);
    if (structName != NULL) vm_pop();
    instance->structName = structName;
    instance->klass = NULL;
    instance->fieldOrder = NULL;
    instance->ui_owner = NULL;      /*  */
    instance->ui_owner_key = NULL;  /*  */
    table_init(&instance->fields);
    return instance;
}

/* ----------------------------------------------------------------
 * Helpers GC-safe pour poser un champ dans inst->fields.
 *
 * Pattern  lot 5 — factorisation du canon `vm_push(inst) +
 * obj_copy_string(key) [+ vm_push(val si Obj)] + table_set + vm_pop`.
 * Sans ces protections, sous DEBUG_STRESS_GC un GC déclenché par
 * obj_copy_string ou par le grow interne de table_set collecterait
 * inst / key / val transitoires (use-after-free, corruption heap).
 *
 * Précédent : helpers static `set_field_*` dans stdlib_sql.c et
 * stdlib_builtin_types.c (canon ). Promus ici car le pattern
 * se répète maintenant dans stdlib_reflect.c et stdlib_ui.c (~25 sites).
 * ---------------------------------------------------------------- */

void inst_field_set_str(ObjInstance* inst, const char* name, const char* val) {
    /*  : v doit etre push aussi (table_set peut grow → GC). */
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    ObjString* v = obj_copy_string(val ? val : "", val ? (int)strlen(val) : 0);
    vm_push(OBJ_VAL(v));
    table_set(&inst->fields, k, OBJ_VAL(v));
    vm_pop(); /* v */
    vm_pop(); /* k */
    vm_pop(); /* inst */
}

void inst_field_set_int(ObjInstance* inst, const char* name, int64_t val) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    table_set(&inst->fields, k, INTEGER_VAL(val));
    vm_pop(); /* k */
    vm_pop(); /* inst */
}

void inst_field_set_bool(ObjInstance* inst, const char* name, bool val) {
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    table_set(&inst->fields, k, BOOL_VAL(val));
    vm_pop(); /* k */
    vm_pop(); /* inst */
}

void inst_field_set_value(ObjInstance* inst, const char* name, Value val) {
    /*  : push val (si Obj) AVANT obj_copy_string. Sinon l'alloc
     * de k peut grow → GC → collecte val (passe par valeur, non root).
     * Bug detecte par la regression refacto helpers canon. */
    vm_push(OBJ_VAL(inst));
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    table_set(&inst->fields, k, val);
    vm_pop(); /* k */
    if (valIsObj) vm_pop();
    vm_pop(); /* inst */
}

/*  phase 2 — insertion bilaterale dans une table principale +
 * son index secondaire canon. La graphie user (`verbatim`) reste cle
 * dans `mainTable`, preservant l'invariant "casse de declaration =
 * forme affichee" et la coexistence des homographes. L'index
 * `canonIdx` mappe la forme canon (strip_accents + tolower) vers
 * l'ObjString verbatim, pour le lookup tolerant au runtime.
 *
 * Discipline GC : 3 ObjString* eventuellement transitoires (verbatim,
 * canon, val si Obj) gardes vivants pendant les deux table_set qui
 * peuvent grow → GC. Modele : owl_define_native / obj_inst_set_kv. */
static void globals_set_bilateral(Table* mainTable, Table* canonIdx,
                                  ObjString* verbatim, Value val) {
    /* verbatim deja rooted par caller (push avant appel). */
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);

    /* Forme canon : derive du verbatim, allocation transitoire rooted. */
    char canonBuf[256];
    int canonLen = scanner_canonicalize_identifier(verbatim->chars,
                                                   verbatim->length,
                                                   canonBuf, sizeof(canonBuf));
    ObjString* canonKey = obj_copy_string(canonBuf, canonLen);
    vm_push(OBJ_VAL(canonKey));

    /* Insertion principale : verbatim -> val. */
    table_set(mainTable, verbatim, val);
    /* Insertion index : canon -> OBJ_VAL(verbatim). Si la cle canon existe
     * deja avec une autre verbatim (cas double-decl detecte au compile),
     * cet ecrasement est volontaire — la derniere graphie gagne, ce qui
     * est sans consequence puisque le compilo bloque le cas avant. */
    table_set(canonIdx, canonKey, OBJ_VAL(verbatim));

    vm_pop(); /* canonKey */
    if (valIsObj) vm_pop();
}

/*  +  phase 2 : canon obj_globals_set bilaterale.
 *
 * CRITIQUE GC : push val (si Obj) AVANT obj_copy_string(name) — car alloc
 * de k peut grow → GC → collecte val si non-root (val passe par valeur
 * sur la C-stack, jamais vue par le scan de roots). Le bug a regress
 * 30/30 → 6/30 lors de la phase de refacto avant ce fix. */
void obj_globals_set(const char* name, Value val) {
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    globals_set_bilateral(&vm.globals, &vm.globals_canon_idx, k, val);
    vm_pop(); /* k */
    if (valIsObj) vm_pop();
}

/*  : variantes "key ObjString* deja allouee".
 * Pas d'obj_copy_string interne — la key vient du caller (typiquement
 * iteration sur table existante, ou AS_STRING d'un Value). Le canon
 * push quand meme inst+k+val pour le table_set qui peut grow → GC. */
void obj_inst_set_kv(ObjInstance* inst, ObjString* k, Value val) {
    vm_push(OBJ_VAL(inst));
    vm_push(OBJ_VAL(k));
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);
    table_set(&inst->fields, k, val);
    if (valIsObj) vm_pop();
    vm_pop(); /* k */
    vm_pop(); /* inst */
}

/*  phase 2 : variante key-deja-allouee — meme bilateralite. */
void obj_globals_set_kv(ObjString* k, Value val) {
    vm_push(OBJ_VAL(k));
    globals_set_bilateral(&vm.globals, &vm.globals_canon_idx, k, val);
    vm_pop(); /* k */
}

/*  — miroir de obj_globals_set ciblant vm.types. Memes precautions
 * GC : push val (si Obj) AVANT obj_copy_string(name) car alloc de k peut
 * grow → GC → collecter val (passe par valeur sur la C-stack, non vu par
 * le scan de roots). Phase 2 : insertion bilaterale dans vm.types_canon_idx. */
void vm_type_register(const char* name, Value val) {
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    globals_set_bilateral(&vm.types, &vm.types_canon_idx, k, val);
    vm_pop(); /* k */
    if (valIsObj) vm_pop();
}

/*  : canon obj_array_push. value_array_write peut grow → GC ;
 * sans push prealable, val (si Obj fresh) est collecte et l'array recoit
 * une Value pointant sur freed memory. Push arr + val (si Obj) en interne. */
void obj_array_push(ObjArray* arr, Value val) {
    vm_push(OBJ_VAL(arr));
    bool valIsObj = IS_OBJ(val);
    if (valIsObj) vm_push(val);
    value_array_write(&arr->elements, val);
    if (valIsObj) vm_pop();
    vm_pop(); /* arr */
}

/*  : canon {cle cstr -> value} auto-rootant. Voir object.h pour le
 * motif UAF. Ordre CRITIQUE : push dest + value AVANT obj_copy_string(name),
 * car l'alloc de la cle peut grow → GC → collecter value si elle est un Obj
 * fresh non-roote (cas mssql/odbc : valeur materialisee avant la cle). Puis
 * obj_assoc_set roote a son tour cle + value pour son propre grow. */
void obj_assoc_put_cstr(ObjAssocArray* dest, const char* name, int namelen, Value value) {
    vm_push(OBJ_VAL(dest));
    bool valIsObj = IS_OBJ(value);
    if (valIsObj) vm_push(value);
    ObjString* key = obj_copy_string(name, namelen);
    vm_push(OBJ_VAL(key));
    obj_assoc_set(dest, key, value);
    vm_pop(); /* key */
    if (valIsObj) vm_pop();
    vm_pop(); /* dest */
}

/* ================================================================
 * CLASS CREATION
 * ================================================================ */

ObjClass* obj_new_class(ObjString* name) {
    /*  : push name pendant ALLOCATE_OBJ qui peut grow → GC. */
    if (name != NULL) vm_push(OBJ_VAL(name));
    ObjClass* klass = ALLOCATE_OBJ(ObjClass, OBJ_CLASS);
    if (name != NULL) vm_pop();
    klass->name = name;
    klass->superclass = NULL;
    klass->kind = CLASS_KIND_CLASS;  /* défaut : ré-écrasé après si Enum/Combinaison */
    table_init(&klass->methods);
    table_init(&klass->defaults);
    table_init(&klass->staticFields);
    table_init(&klass->interfaces);  /*  */
    return klass;
}

/* ================================================================
 * COLLECTION CREATION
 * ================================================================ */

ObjCollection* obj_new_collection(CollectionType collType) {
    ObjCollection* coll = ALLOCATE_OBJ(ObjCollection, OBJ_COLLECTION);
    coll->collType = collType;
    value_array_init(&coll->elements);
    return coll;
}

/* ================================================================
 * PROCEDURE CHAIN
 * ================================================================ */

ObjProcChain* obj_new_proc_chain(void) {
    ObjProcChain* chain = ALLOCATE_OBJ(ObjProcChain, OBJ_PROC_CHAIN);
    chain->procs = NULL;
    chain->count = 0;
    chain->capacity = 0;
    chain->avant = NIL_VAL;
    chain->apres = NIL_VAL;
    return chain;
}

ObjProcChain* obj_proc_chain_of(Value v) {
    if (IS_PROC_CHAIN(v)) return AS_PROC_CHAIN(v);
    ObjProcChain* chain = obj_new_proc_chain();
    if (IS_FUNCTION(v) || IS_NATIVE(v)) {
        chain->capacity = 4;
        chain->procs = (Value*)malloc(sizeof(Value) * (size_t)chain->capacity);
        if (chain->procs == NULL) {
            fprintf(stderr, "owl: out of memory (proc chain)\n");
            exit(74);
        }
        chain->procs[chain->count++] = v;
    }
    return chain;
}

void obj_proc_chain_add(ObjProcChain* chain, Value callable) {
    if (!IS_FUNCTION(callable) && !IS_NATIVE(callable) && !IS_PROC_CHAIN(callable)) return;
    /* If the incoming value is itself a chain, splice its procs in order. */
    if (IS_PROC_CHAIN(callable)) {
        ObjProcChain* src = AS_PROC_CHAIN(callable);
        for (int i = 0; i < src->count; i++) obj_proc_chain_add(chain, src->procs[i]);
        return;
    }
    if (chain->count + 1 > chain->capacity) {
        int newCap = chain->capacity < 4 ? 4 : chain->capacity * 2;
        chain->procs = (Value*)realloc(chain->procs, sizeof(Value) * (size_t)newCap);
        chain->capacity = newCap;
    }
    chain->procs[chain->count++] = callable;
}

int obj_proc_chain_remove(ObjProcChain* chain, Value callable) {
    int removed = 0;
    int j = 0;
    for (int i = 0; i < chain->count; i++) {
        if (values_equal(chain->procs[i], callable)) {
            removed++;
            continue;
        }
        chain->procs[j++] = chain->procs[i];
    }
    chain->count = j;
    return removed;
}

/* ================================================================
 * JSON CREATION & MANIPULATION
 * ================================================================ */

ObjJson* obj_new_json(JsonNodeType jtype) {
    ObjJson* json = ALLOCATE_OBJ(ObjJson, OBJ_JSON);
    json->jtype = jtype;
    memset(&json->as, 0, sizeof(json->as));
    return json;
}

ObjJson* obj_new_json_string(ObjString* str) {
    /*  : str doit etre rooted pendant ALLOCATE_OBJ (via obj_new_json)
     * qui peut grow → GC → str collected si pas root. Push interne pour
     * que le canon soit safe par construction sans contrainte caller. */
    if (str != NULL) vm_push(OBJ_VAL(str));
    ObjJson* j = obj_new_json(JSON_T_STRING);
    if (str != NULL) vm_pop();
    j->as.strVal = str;
    return j;
}

ObjJson* obj_new_json_int(int64_t val) {
    ObjJson* j = obj_new_json(JSON_T_INT);
    j->as.intVal = val;
    return j;
}

ObjJson* obj_new_json_real(double val) {
    ObjJson* j = obj_new_json(JSON_T_REAL);
    j->as.realVal = val;
    return j;
}

ObjJson* obj_new_json_currency(__int128 val) {
    ObjJson* j = obj_new_json(JSON_T_CURRENCY);
    j->as.currencyVal = val;
    return j;
}

ObjJson* obj_new_json_numeric(Value val) {
    /* val (OBJ_NUMERIC) doit survivre a l'alloc d'obj_new_json (peut GC). */
    vm_push(val);
    ObjJson* j = obj_new_json(JSON_T_NUMERIC);
    j->as.numericVal = AS_NUMERIC(val);
    vm_pop();
    return j;
}

ObjJson* obj_new_json_bool(bool val) {
    ObjJson* j = obj_new_json(JSON_T_BOOL);
    j->as.boolVal = val;
    return j;
}

/* Case-insensitive key equality for JSON object members.
 * Canon : j.userName == j.username == j.UserName.
 * ASCII-only ; pas de fold accent ni snake_case → camelCase. */
bool obj_json_key_eq_ci(const ObjString* a, const ObjString* b) {
    if (a == b) return true;
    if (a->length != b->length) return false;
    for (int i = 0; i < a->length; i++) {
        unsigned char ca = (unsigned char)a->chars[i];
        unsigned char cb = (unsigned char)b->chars[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

void obj_json_object_set(ObjJson* obj, ObjString* key, ObjJson* value) {
    if (obj->jtype != JSON_T_OBJECT) return;
    /* Check for existing key (case-insensitive — preserve original casing) */
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        if (obj_json_key_eq_ci(obj->as.objectVal.keys[i], key)) {
            obj->as.objectVal.values[i] = value;
            return;
        }
    }
    /* New key */
    if (obj->as.objectVal.count >= obj->as.objectVal.capacity) {
        int newCap = obj->as.objectVal.capacity < 8 ? 8 : obj->as.objectVal.capacity * 2;
        obj->as.objectVal.keys = realloc(obj->as.objectVal.keys, newCap * sizeof(ObjString*));
        obj->as.objectVal.values = realloc(obj->as.objectVal.values, newCap * sizeof(ObjJson*));
        obj->as.objectVal.capacity = newCap;
    }
    int idx = obj->as.objectVal.count++;
    obj->as.objectVal.keys[idx] = key;
    obj->as.objectVal.values[idx] = value;
}

ObjJson* obj_json_object_get(ObjJson* obj, ObjString* key) {
    if (obj->jtype != JSON_T_OBJECT) return NULL;
    for (int i = 0; i < obj->as.objectVal.count; i++) {
        if (obj_json_key_eq_ci(obj->as.objectVal.keys[i], key)) {
            return obj->as.objectVal.values[i];
        }
    }
    return NULL;
}

void obj_json_array_push(ObjJson* arr, ObjJson* value) {
    if (arr->jtype != JSON_T_ARRAY) return;
    if (arr->as.arrayVal.count >= arr->as.arrayVal.capacity) {
        int newCap = arr->as.arrayVal.capacity < 8 ? 8 : arr->as.arrayVal.capacity * 2;
        arr->as.arrayVal.items = realloc(arr->as.arrayVal.items, newCap * sizeof(ObjJson*));
        arr->as.arrayVal.capacity = newCap;
    }
    arr->as.arrayVal.items[arr->as.arrayVal.count++] = value;
}

/* Convert ObjJson leaf to OWL Value. Arrays/objects stay as ObjJson. */
Value obj_json_to_value(ObjJson* json) {
    if (!json) return NIL_VAL;
    switch (json->jtype) {
        case JSON_T_NULL:   return NIL_VAL;
        case JSON_T_BOOL:   return BOOL_VAL(json->as.boolVal);
        case JSON_T_INT:      return INTEGER_VAL(json->as.intVal);
        case JSON_T_REAL:     return REAL_VAL(json->as.realVal);
        case JSON_T_CURRENCY: return CURRENCY_VAL(json->as.currencyVal);
        case JSON_T_NUMERIC:  return OBJ_VAL(json->as.numericVal);
        case JSON_T_STRING:   return OBJ_VAL(json->as.strVal);
        case JSON_T_ARRAY:
        case JSON_T_OBJECT: return OBJ_VAL(json);
    }
    return NIL_VAL;
}

/* ================================================================
 * DATE / TIME / DATETIME / DURATION CONSTRUCTORS
 * ================================================================ */

ObjDate* obj_new_date(int year, int month, int day) {
    ObjDate* d = ALLOCATE_OBJ(ObjDate, OBJ_DATE);
    d->year = year;
    d->month = month;
    d->day = day;
    return d;
}

ObjTime* obj_new_time(int hour, int minute, int second, int ms) {
    ObjTime* t = ALLOCATE_OBJ(ObjTime, OBJ_TIME);
    t->hour = hour;
    t->minute = minute;
    t->second = second;
    t->ms = ms;
    t->cs = ms / 10;  /* centièmes dérivés des ms */
    return t;
}

ObjTime* obj_new_time_cs(int hour, int minute, int second, int cs) {
    ObjTime* t = ALLOCATE_OBJ(ObjTime, OBJ_TIME);
    t->hour = hour;
    t->minute = minute;
    t->second = second;
    t->cs = cs;
    t->ms = cs * 10;
    return t;
}

ObjDateTime* obj_new_datetime(int year, int month, int day,
                               int hour, int minute, int second, int ms) {
    ObjDateTime* dt = ALLOCATE_OBJ(ObjDateTime, OBJ_DATETIME);
    dt->year = year; dt->month = month; dt->day = day;
    dt->hour = hour; dt->minute = minute; dt->second = second; dt->ms = ms;
    return dt;
}

ObjDuration* obj_new_duration(int64_t totalMs) {
    ObjDuration* dur = ALLOCATE_OBJ(ObjDuration, OBJ_DURATION);
    dur->totalMs = totalMs;
    return dur;
}

/* ================================================================
 * PRINT
 * ================================================================ */

void object_print(Value value) {
    OwlDisplaySink sink;
    owl_display_sink_init_buffer(&sink);
    sink.out = stdout;
    object_write_display(&sink, value);
}

void object_write_display(OwlDisplaySink* sink, Value value) {
    /* Tableaux, tableaux associatifs et JSON se deroulent recursivement.
     * La garde borne la pile C sur une structure cyclique ou pathologique ;
     * aucune donnee reelle n'atteint cette profondeur, donc la sortie des
     * cas courants est inchangee. */
    if (sink->depth >= OWL_DISPLAY_MAX_DEPTH) {
        owl_display_write(sink, "...", 3);
        return;
    }
    sink->depth++;
    switch (OBJ_TYPE(value)) {
        case OBJ_STRING: {
            ObjString* s = AS_STRING(value);
            if (s->undetectable) {
                /* Briefly unmask for output, then scrub the temp. The
                 * value will still appear in the stdout buffer of the
                 * runtime -- this is a known limitation of the
                 * `<indétectable>` + ConsoleEcrit behavior. */
                char* clear = obj_string_unmask_dup(s);
                owl_display_write(sink, clear, (size_t)s->length);
                obj_string_secure_free(clear, s->length);
            } else {
                owl_display_printf(sink, "%s", s->chars);
            }
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* fn = AS_FUNCTION(value);
            if (fn->name == NULL) {
                owl_display_printf(sink, "<script>");
            } else {
                owl_display_printf(sink, "<proc %s>", fn->name->chars);
            }
            break;
        }
        case OBJ_NATIVE:
            owl_display_printf(sink, "<native fn>");
            break;
        case OBJ_NUMERIC: {
            char* s = numeric_format_alloc(value, "f");  /* notation fixe plate */
            if (s != NULL) { owl_display_printf(sink, "%s", s); owl_numeric_free_str(s); }
            else owl_display_printf(sink, "0");
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = AS_ARRAY(value);
            owl_display_printf(sink, "[");
            for (int i = 0; i < arr->elements.count; i++) {
                if (i > 0) owl_display_printf(sink, ", ");
                value_write_display(sink, arr->elements.values[i]);
            }
            owl_display_printf(sink, "]");
            break;
        }
        case OBJ_BUFFER: {
            /* Print as bytes (text if ASCII-safe). For display parity with
             * Trace on buffers. */
            ObjBuffer* b = AS_BUFFER(value);
            for (int i = 0; i < b->length; i++) {
                unsigned char c = b->bytes[i];
                if (c >= 32 && c <= 126) owl_display_putc(sink, c);
                else if (c == '\n' || c == '\r' || c == '\t') owl_display_putc(sink, c);
                else owl_display_printf(sink, "\\x%02x", c);
            }
            break;
        }
        case OBJ_ASSOC_ARRAY: {
            ObjAssocArray* a = AS_ASSOC_ARRAY(value);
            owl_display_printf(sink, "{");
            for (int i = 0; i < a->count; i++) {
                if (i > 0) owl_display_printf(sink, ", ");
                owl_display_printf(sink, "\"%s\": ", a->keys[i]->chars);
                value_write_display(sink, a->values[i]);
            }
            owl_display_printf(sink, "}");
            break;
        }
        case OBJ_MULTILINGUE: {
            /* L'affichage resout la langue courante (defaut "fr-FR"), pas le
             * dict brut : coherent avec la route canonique value_to_string
             * (concat, coercion `= chaine`, egalite). Reutilise le MEME appel.
             * L'inspection ponctuelle d'une traduction precise passe par
             * `m[langueXxx]`. */
            Value resolved = multilingue_resolve(AS_MULTILINGUE(value),
                                                 owl_i18n_langue_courante(),
                                                 owl_i18n_langue_source());
            if (IS_STRING(resolved)) {
                owl_display_printf(sink, "%s", AS_STRING(resolved)->chars);
            }
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* inst = AS_INSTANCE(value);
            /* UUID — coercion implicite vers forme canonique au print,
             * comme Date/Heure/DateHeure (rendus en chiffres lisibles
             * plutôt qu'en "<instance>"). Le helper hook vit dans
             * stdlib_uuid.c pour garder la connaissance du field privé
             * `_bytes` dans le module métier. */
            if (owl_value_is_uuid(value)) {
                ObjString* s = owl_uuid_format_canonical(value);
                if (s != NULL) {
                    owl_display_printf(sink, "%s", s->chars);
                    break;
                }
            }
            /* structName peut etre NULL pour une instance anonyme : rep-proxy
             * de ligne de Table nue (, champs indexes par nom de
             * colonne, pas de nom de structure). Le chemin display ne doit
             * pas deref — libelle neutre, meme famille que le cas nomme. */
            if (inst->structName != NULL) {
                owl_display_printf(sink, "<%s instance>", inst->structName->chars);
            } else {
                owl_display_printf(sink, "<instance>");
            }
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = AS_CLASS(value);
            owl_display_printf(sink, "<class %s>", klass->name->chars);
            break;
        }
        case OBJ_COLLECTION: {
            ObjCollection* coll = AS_COLLECTION(value);
            const char* typeName =
                coll->collType == COLLECTION_QUEUE ? "File" :
                coll->collType == COLLECTION_STACK ? "Pile" : "Liste";
            owl_display_printf(sink, "<%s %d elements>", typeName, coll->elements.count);
            break;
        }
        case OBJ_JSON: {
            ObjJson* j = AS_JSON(value);
            switch (j->jtype) {
                case JSON_T_NULL:   owl_display_printf(sink, "null"); break;
                case JSON_T_BOOL:   owl_display_printf(sink, "%s", j->as.boolVal ? "true" : "false"); break;
                case JSON_T_INT:    owl_display_printf(sink, "%lld", (long long)j->as.intVal); break;
                case JSON_T_REAL: {
                    char buf[40]; owl_real_to_str(j->as.realVal, buf, sizeof(buf));
                    owl_display_printf(sink, "%s", buf); break;
                }
                case JSON_T_CURRENCY: {
                    char buf[32]; currency_to_str(j->as.currencyVal, buf, sizeof(buf));
                    owl_display_printf(sink, "%s", buf); break;
                }
                case JSON_T_NUMERIC: {
                    char* s = numeric_format_alloc(OBJ_VAL(j->as.numericVal), "f");
                    if (s != NULL) { owl_display_printf(sink, "%s", s); owl_numeric_free_str(s); }
                    else owl_display_printf(sink, "0");
                    break;
                }
                case JSON_T_STRING: owl_display_printf(sink, "\"%s\"", j->as.strVal->chars); break;
                case JSON_T_ARRAY:
                    owl_display_printf(sink, "[");
                    for (int i = 0; i < j->as.arrayVal.count; i++) {
                        if (i > 0) owl_display_printf(sink, ",");
                        object_write_display(sink, OBJ_VAL(j->as.arrayVal.items[i]));
                    }
                    owl_display_printf(sink, "]");
                    break;
                case JSON_T_OBJECT:
                    owl_display_printf(sink, "{");
                    for (int i = 0; i < j->as.objectVal.count; i++) {
                        if (i > 0) owl_display_printf(sink, ",");
                        owl_display_printf(sink, "\"%s\":", j->as.objectVal.keys[i]->chars);
                        object_write_display(sink, OBJ_VAL(j->as.objectVal.values[i]));
                    }
                    owl_display_printf(sink, "}");
                    break;
            }
            break;
        }
        case OBJ_DATE: {
            ObjDate* d = AS_DATE(value);
            owl_display_printf(sink, "%04d%02d%02d", d->year, d->month, d->day);
            break;
        }
        case OBJ_TIME: {
            ObjTime* t = AS_TIME(value);
            owl_display_printf(sink, "%02d%02d%02d", t->hour, t->minute, t->second);
            break;
        }
        case OBJ_DATETIME: {
            ObjDateTime* dt = AS_DATETIME(value);
            owl_display_printf(sink, "%04d%02d%02d%02d%02d%02d", dt->year, dt->month, dt->day,
                   dt->hour, dt->minute, dt->second);
            break;
        }
        case OBJ_DURATION: {
            ObjDuration* dur = AS_DURATION(value);
            int64_t ms = dur->totalMs;
            if (ms < 0) { owl_display_printf(sink, "-"); ms = -ms; }
            int64_t secs = ms / 1000;
            int64_t days = secs / 86400;
            int h = (int)((secs % 86400) / 3600);
            int m = (int)((secs % 3600) / 60);
            int s = (int)(secs % 60);
            if (days > 0) owl_display_printf(sink, "%lld j ", (long long)days);
            owl_display_printf(sink, "%02d:%02d:%02d", h, m, s);
            break;
        }
        case OBJ_PROC_CHAIN: {
            ObjProcChain* chain = AS_PROC_CHAIN(value);
            owl_display_printf(sink, "<chain %d procs", chain->count);
            if (!IS_NIL(chain->avant)) owl_display_printf(sink, " +Avant");
            if (!IS_NIL(chain->apres)) owl_display_printf(sink, " +Apres");
            owl_display_printf(sink, ">");
            break;
        }
        case OBJ_ZIP_ARCHIVE: {
            ObjZipArchive* z = AS_ZIP_ARCHIVE(value);
            const char* fmt = "?";
            switch (z->format) {
                case ZIP_FMT_ZIP:   fmt = "ZIP";   break;
                case ZIP_FMT_WDZ:   fmt = "WDZ";   break;
                case ZIP_FMT_TAR:   fmt = "TAR";   break;
                case ZIP_FMT_TARGZ: fmt = "TGZ";   break;
                case ZIP_FMT_7Z:    fmt = "7z";    break;
                case ZIP_FMT_CAB:   fmt = "CAB";   break;
                case ZIP_FMT_RAR:   fmt = "RAR";   break;
            }
            owl_display_printf(sink, "<zipArchive %s %s%s>", fmt,
                   z->path ? z->path : "<mem>",
                   z->open ? "" : " (fermé)");
            break;
        }

        case OBJ_XLS_DOCUMENT: {
            /*  — rendu compact : <xlsDocument N feuille(s)>. */
            ObjXlsDocument* d = AS_XLS_DOCUMENT(value);
            owl_display_printf(sink, "<xlsDocument %d feuille%s>",
                   d->sheetCount, d->sheetCount > 1 ? "s" : "");
            break;
        }

        case OBJ_XLS_REF: {
            /*  — reference de style. */
            ObjXlsRef* r = AS_XLS_REF(value);
            const char* k = r->kind == XLS_REF_CELL ? "Cellule"
                          : r->kind == XLS_REF_ROW  ? "Ligne" : "Colonne";
            if (r->bound)
                owl_display_printf(sink, "<xls%s[%d,%d]>", k, r->row, r->col);
            else
                owl_display_printf(sink, "<xls%s>", k);
            break;
        }

        case OBJ_MOCK_NATIVE: {
            ObjMockNative* mock = AS_MOCK_NATIVE(value);
            owl_display_printf(sink, "<MockNative #%d>", mock->mockIndex);
            break;
        }

        case OBJ_PATTERN: {
            ObjPattern* pattern = AS_PATTERN(value);
            const char* typeStr = "?";
            switch (pattern->patternType) {
                case 0: typeStr = "EXACT"; break;
                case 1: typeStr = "STARTS"; break;
                case 2: typeStr = "CONTAINS"; break;
                case 3: typeStr = "ENDS"; break;
                case 4: typeStr = "ANY"; break;
                case 5: typeStr = "REGEX"; break;
            }
            owl_display_printf(sink, "<Pattern %s ", typeStr);
            value_write_display(sink, pattern->value);
            owl_display_printf(sink, ">");
            break;
        }

        case OBJ_UI_CONTROL: {
            /* Coercion implicite vers Valeur (Texte/Libelle/Titre selon
             * kind). C'est la sémantique canon : 'Trace(saiUser)' ou
             * 'Info(...+saiUser+...)' affiche la valeur du champ.
             * value_to_string gere le dispatch via owl_ui_control_get_prop. */
            ObjString* s = value_to_string(value);
            if (s) owl_display_printf(sink, "%.*s", s->length, s->chars);
            break;
        }
        /*  — rendu minimaliste tant qu'aucune semantique
         * publique n'est cablee. Affichage diagnostique uniquement. */
        case OBJ_WINDOW_INSTANCE: {
            ObjWindowInstance* w = AS_WINDOW_INSTANCE(value);
            owl_display_printf(sink, "<WindowInstance #%d>", w->instance_id);
            break;
        }
        case OBJ_UI_CONTROL_INSTANCE: {
            owl_display_printf(sink, "<UiControlInstance>");
            break;
        }
        case OBJ_UI_CONTROL_REF: {
            owl_display_printf(sink, "<UiControlRef>");
            break;
        }
        case OBJ_WINDOW_REF: {
            owl_display_printf(sink, "<WindowRef>");
            break;
        }
        case OBJ_ETAT_INSTANCE: {
            /*  lot D — affichage diagnostique minimal. */
            owl_display_printf(sink, "<EtatInstance>");
            break;
        }
        case OBJ_ICONE: {
            /*  Lot 2b — affichage diagnostique. Le canon "Trace(ICN)"
             * affichera le SVG raw une fois la stdlib Icone cablee (Lot 2e
             * fournira value_to_string pour OBJ_ICONE). Ici on reste minimal. */
            ObjIcone* icn = AS_ICONE(value);
            if (icn->catalog_hash != 0) {
                owl_display_printf(sink, "<Icone catalog #%016llx>",
                       (unsigned long long)icn->catalog_hash);
            } else if (icn->svg_raw != NULL) {
                owl_display_printf(sink, "<Icone custom %d bytes>", icn->svg_raw->length);
            } else {
                owl_display_printf(sink, "<Icone vide>");
            }
            break;
        }
        case OBJ_ICONES_NAMESPACE: {
            /*  Lot 2e-1 — singleton catalogue. */
            owl_display_printf(sink, "<icones>");
            break;
        }
        case OBJ_THEME_NAMESPACE: {
            /*  Lot 2b+3 — singleton namespace styles. */
            owl_display_printf(sink, "<theme>");
            break;
        }
        case OBJ_VECTOR: {
            /*  — rendu type tableau : [1, 2, 3]. Chaque element est
             * un reel, serialise via la forme decimale canon (owl_real_to_str). */
            ObjVector* vec = AS_VECTOR(value);
            owl_display_printf(sink, "[");
            for (int i = 0; i < vec->count; i++) {
                if (i > 0) owl_display_printf(sink, ", ");
                char buf[40];
                owl_real_to_str(vec->data[i], buf, sizeof(buf));
                owl_display_printf(sink, "%s", buf);
            }
            owl_display_printf(sink, "]");
            break;
        }
        case OBJ_MATRIX: {
            /*  — rendu 2D : [[1, 2], [3, 4]]. Chaque element est un
             * reel serialise via la forme decimale canon (owl_real_to_str). */
            ObjMatrix* mat = AS_MATRIX(value);
            owl_display_printf(sink, "[");
            for (int r = 0; r < mat->rows; r++) {
                if (r > 0) owl_display_printf(sink, ", ");
                owl_display_printf(sink, "[");
                for (int c = 0; c < mat->cols; c++) {
                    if (c > 0) owl_display_printf(sink, ", ");
                    char buf[40];
                    owl_real_to_str(mat->data[r * mat->cols + c], buf, sizeof(buf));
                    owl_display_printf(sink, "%s", buf);
                }
                owl_display_printf(sink, "]");
            }
            owl_display_printf(sink, "]");
            break;
        }
        case OBJ_MATRIX_ROW: {
            /*  — vue transitoire : rendue comme la ligne qu'elle
             * designe (jamais stockee par l'utilisateur, mais un affichage
             * accidentel reste lisible). */
            ObjMatrixRow* rv = AS_MATRIX_ROW(value);
            ObjMatrix* mat = rv->mat;
            owl_display_printf(sink, "[");
            for (int c = 0; c < mat->cols; c++) {
                if (c > 0) owl_display_printf(sink, ", ");
                char buf[40];
                owl_real_to_str(mat->data[(rv->row - 1) * mat->cols + c], buf, sizeof(buf));
                owl_display_printf(sink, "%s", buf);
            }
            owl_display_printf(sink, "]");
            break;
        }
        case OBJ_TENSOR: {
            /*  — rendu : la donnee entre crochets prefixee du type,
             * ex `Tenseur([3])`. Le gradient et le graphe restent internes. */
            ObjTensor* t = AS_TENSOR(value);
            owl_display_printf(sink, "Tenseur([");
            int nel = tensor_numel(t->data);
            double* cells = tensor_cells(t->data);
            for (int i = 0; i < nel; i++) {
                if (i > 0) owl_display_printf(sink, ", ");
                char buf[40];
                owl_real_to_str(cells[i], buf, sizeof(buf));
                owl_display_printf(sink, "%s", buf);
            }
            owl_display_printf(sink, "])");
            break;
        }
        case OBJ_TENSOR_VIEW: {
            /*  — vue transitoire (sous-tranche N-D) : rendue comme la
             * tranche plate qu'elle designe le long de son premier axe (jamais
             * stockee par l'utilisateur, mais un affichage accidentel reste
             * lisible). */
            ObjTensorView* vw = AS_TENSOR_VIEW(value);
            double* cells = tensor_cells(vw->backing);
            owl_display_printf(sink, "[");
            int n0 = vw->ndim > 0 ? vw->shape[0] : 0;
            int st0 = vw->ndim > 0 ? vw->strides[0] : 1;
            for (int i = 0; i < n0; i++) {
                if (i > 0) owl_display_printf(sink, ", ");
                char buf[40];
                owl_real_to_str(cells[vw->offset + (size_t)i * st0], buf, sizeof(buf));
                owl_display_printf(sink, "%s", buf);
            }
            owl_display_printf(sink, "]");
            break;
        }
        case OBJ_IAMODELE: {
            /*  — rendu compact : le nombre de couches empilees. Les
             * poids restent internes (ni bruyants ni utiles a l'affichage). */
            ObjIAModele* m = AS_IAMODELE(value);
            owl_display_printf(sink, "IAMod\xc3\xa8le(%d couches)", m->count);
            break;
        }
        case OBJ_MLARBRE: {
            /*  — rendu compact : état (non entraîné / entraîné). La
             * structure de l'arbre reste interne. */
            ObjMLArbre* a = AS_MLARBRE(value);
            owl_display_printf(sink, "MLArbreD\xc3\xa9""cision(%s)",
                a->root != NULL ? "entra\xc3\xaen\xc3\xa9" : "non entra\xc3\xaen\xc3\xa9");
            break;
        }
        case OBJ_MLFORET: {
            /*  — rendu compact : nombre d'arbres et état d'entraînement. */
            ObjMLForet* f = AS_MLFORET(value);
            owl_display_printf(sink, "MLFor\xc3\xaatAl\xc3\xa9""atoire(%d arbres, %s)",
                f->numTrees,
                f->trees != NULL ? "entra\xc3\xaen\xc3\xa9""e" : "non entra\xc3\xaen\xc3\xa9""e");
            break;
        }
        case OBJ_MLBOOSTING: {
            /*  — rendu compact : nombre d'étapes et état d'entraînement. */
            ObjMLBoosting* b = AS_MLBOOSTING(value);
            owl_display_printf(sink, "MLGradientBoosting(%d arbres, %s)",
                b->numTrees,
                b->trees != NULL ? "entra\xc3\xaen\xc3\xa9" : "non entra\xc3\xaen\xc3\xa9");
            break;
        }
        case OBJ_MLLINEAIRE: {
            /*  — rendu compact : nombre de variables et état. */
            ObjMLLineaire* r = AS_MLLINEAIRE(value);
            owl_display_printf(sink, "MLR\xc3\xa9gressionLin\xc3\xa9""aire(%s)",
                r->trained ? "entra\xc3\xaen\xc3\xa9""e" : "non entra\xc3\xaen\xc3\xa9""e");
            break;
        }
        case OBJ_MLLOGISTIQUE: {
            /*  — rendu compact : nombre de classes et état. */
            ObjMLLogistique* r = AS_MLLOGISTIQUE(value);
            owl_display_printf(sink, "MLR\xc3\xa9gressionLogistique(%s)",
                r->trained ? "entra\xc3\xaen\xc3\xa9""e" : "non entra\xc3\xaen\xc3\xa9""e");
            break;
        }
        case OBJ_MLKMEANS: {
            /*  — rendu compact : nombre de clusters et état. */
            ObjMLKMeans* k = AS_MLKMEANS(value);
            owl_display_printf(sink, "MLKMeans(%d clusters, %s)",
                k->numClusters,
                k->trained ? "entra\xc3\xaen\xc3\xa9" : "non entra\xc3\xaen\xc3\xa9");
            break;
        }
        case OBJ_OPTIONS_ENTRAINEMENT: {
            /*  — rendu compact de la config d'entrainement. */
            ObjOptionsEntrainement* o = AS_OPTIONS_ENTRAINEMENT(value);
            char lr[40];
            owl_real_to_str(o->learningRate, lr, sizeof(lr));
            owl_display_printf(sink,
                "OptionsEntra\xc3\xaenement(\xc3\x89poques=%d, TailleLot=%d, Taux=%s)",
                o->epochs, o->batchSize, lr);
            break;
        }
        case OBJ_LLM: {
            /*  — rendu compact : etat de chargement (les handles
             * llama restent internes). */
            ObjLLM* l = AS_LLM(value);
            owl_display_printf(sink, "LLM(%s)",
                l->model != NULL ? "charg\xc3\xa9" : "non charg\xc3\xa9");
            break;
        }
        case OBJ_OPTIONS_GENERATION: {
            /*  — rendu compact des reglages de generation. */
            ObjOptionsGeneration* o = AS_OPTIONS_GENERATION(value);
            char tp[40];
            owl_real_to_str(o->temperature, tp, sizeof(tp));
            owl_display_printf(sink,
                "OptionsG\xc3\xa9n\xc3\xa9ration(Temp\xc3\xa9rature=%s, MaxTokens=%d)",
                tp, o->maxTokens);
            break;
        }
        case OBJ_MESSAGE_LLM: {
            /*  — rendu compact : rôle + apercu du contenu. */
            ObjMessageLLM* m = AS_MESSAGE_LLM(value);
            const char* role = IS_STRING(m->role) ? AS_CSTRING(m->role) : "";
            const char* cont = IS_STRING(m->content) ? AS_CSTRING(m->content) : "";
            owl_display_printf(sink, "MessageLLM(R\xc3\xb4le=%s, Contenu=%s)",
                               role, cont);
            break;
        }
        case OBJ_RESPONSE_LLM: {
            /*  — rendu compact : compteurs de jetons + apercu. */
            ObjResponseLLM* r = AS_RESPONSE_LLM(value);
            const char* cont = IS_STRING(r->content) ? AS_CSTRING(r->content) : "";
            owl_display_printf(sink,
                "R\xc3\xa9ponseLLM(Contenu=%s, JetonsEntr\xc3\xa9""e=%d, "
                "JetonsSortie=%d)",
                cont, r->inputTokens, r->outputTokens);
            break;
        }
        case OBJ_DETECTION: {
            /*  — rendu compact : classe + score + boite [x, y, l, h]. */
            ObjDetection* d = AS_DETECTION(value);
            double b[4] = {0, 0, 0, 0};
            if (IS_ARRAY(d->box)) {
                ObjArray* a = AS_ARRAY(d->box);
                for (int i = 0; i < 4 && i < a->elements.count; i++)
                    if (IS_NUMBER(a->elements.values[i]))
                        b[i] = AS_NUMBER(a->elements.values[i]);
            }
            owl_display_printf(sink,
                "D\xc3\xa9tection(Classe=%d, Score=%.3f, "
                "Bo\xc3\xaete=[%g, %g, %g, %g])",
                d->classIndex, d->score, b[0], b[1], b[2], b[3]);
            break;
        }
        case OBJ_IMAGE_ETIQUETEE: {
            /*  — rendu compact : chemin + etiquette (l'Image reste
             * interne, materialisee a la demande). */
            ObjImageEtiquetee* ie = AS_IMAGE_ETIQUETEE(value);
            owl_display_printf(sink, "Image\xc3\x89tiquet\xc3\xa9""e(%s, %s)",
                               ie->path  ? ie->path->chars  : "",
                               ie->label ? ie->label->chars : "");
            break;
        }
        case OBJ_IMAGE_ANNOTEE: {
            /*  — rendu compact : chemin + nombre de boites annotees
             * (l'Image reste interne, materialisee a la demande). */
            ObjImageAnnotee* ia = AS_IMAGE_ANNOTEE(value);
            int nb = (ia->boxes != NULL) ? ((ObjArray*)ia->boxes)->elements.count : 0;
            owl_display_printf(sink, "ImageAnnot\xc3\xa9""e(%s, %d bo\xc3\xaetes)",
                               ia->path ? ia->path->chars : "", nb);
            break;
        }
        case OBJ_DATASET: {
            /*  — rendu compact : taille + nombre de classes. */
            ObjDataset* ds = AS_DATASET(value);
            owl_display_printf(sink, "Dataset(%d exemples, %d classes)",
                               ds->count, ds->classCount);
            break;
        }
    }
    sink->depth--;
}

/* ================================================================
 * FREE
 * ================================================================ */

void object_free(Obj* object) {
    switch (object->type) {
        case OBJ_STRING: {
            ObjString* string = (ObjString*)object;
#ifdef OWL_INDETECTABLE
            /* Securely scrub undetectable buffers before release so the
             * masked secret doesn't survive in freed heap memory. */
            if (string->undetectable && string->chars != NULL) {
                secure_zero(string->chars, (size_t)string->capacity);
            }
#endif
            /* Use capacity (not length+1) so we free the full buffer for
             * builder-mode strings. For interned strings capacity == length+1
             * so this is equivalent. */
            FREE_ARRAY(char, string->chars, string->capacity);
            FREE(ObjString, object);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* function = (ObjFunction*)object;
            /* chunk is freed separately */
            /*  — libere le tableau de noms de locales (les ObjString
             * name sont collectees par le GC, pas ici). */
            free(function->localNames);
            FREE(ObjFunction, function);
            break;
        }
        case OBJ_NATIVE:
            FREE(ObjNative, object);
            break;
        case OBJ_ARRAY: {
            ObjArray* array = (ObjArray*)object;
            value_array_free(&array->elements);
            FREE(ObjArray, object);
            break;
        }
        case OBJ_BUFFER: {
            ObjBuffer* b = (ObjBuffer*)object;
            free(b->bytes);
            FREE(ObjBuffer, object);
            break;
        }
        case OBJ_NUMERIC: {
            ObjNumeric* n = (ObjNumeric*)object;
            if (n->dec != NULL) mpd_del(n->dec);  /* libere les limbes mpdecimal */
            FREE(ObjNumeric, object);
            break;
        }
        case OBJ_ASSOC_ARRAY: {
            ObjAssocArray* a = (ObjAssocArray*)object;
            free(a->keys);
            free(a->values);
            FREE(ObjAssocArray, object);
            break;
        }
        case OBJ_MULTILINGUE: {
            /*  lot B : le storage assoc est trace separement par
             * le GC (mark scan via gc_mark_object) puis liberable a part.
             * Ici on ne free QUE le wrapper. */
            FREE(ObjMultilingue, object);
            break;
        }
        case OBJ_INSTANCE: {
            ObjInstance* instance = (ObjInstance*)object;
            table_free(&instance->fields);
            FREE(ObjInstance, object);
            break;
        }
        case OBJ_CLASS: {
            ObjClass* klass = (ObjClass*)object;
            table_free(&klass->methods);
            table_free(&klass->defaults);
            table_free(&klass->staticFields);
            table_free(&klass->interfaces);  /*  */
            FREE(ObjClass, object);
            break;
        }
        case OBJ_COLLECTION: {
            ObjCollection* coll = (ObjCollection*)object;
            value_array_free(&coll->elements);
            FREE(ObjCollection, object);
            break;
        }
        case OBJ_JSON: {
            ObjJson* j = (ObjJson*)object;
            if (j->jtype == JSON_T_ARRAY) {
                free(j->as.arrayVal.items); /* children freed by GC individually */
            } else if (j->jtype == JSON_T_OBJECT) {
                free(j->as.objectVal.keys);
                free(j->as.objectVal.values);
            }
            FREE(ObjJson, object);
            break;
        }
        case OBJ_DATE:     FREE(ObjDate, object); break;
        case OBJ_TIME:     FREE(ObjTime, object); break;
        case OBJ_DATETIME: FREE(ObjDateTime, object); break;
        case OBJ_DURATION: FREE(ObjDuration, object); break;
        case OBJ_PROC_CHAIN: {
            ObjProcChain* chain = (ObjProcChain*)object;
            free(chain->procs);
            FREE(ObjProcChain, object);
            break;
        }
        case OBJ_ZIP_ARCHIVE: {
            ObjZipArchive* z = (ObjZipArchive*)object;
            /* Release backend + path + password even if user forgot
             * zipFerme — idempotent, safe on already-closed archives. */
            obj_zip_archive_close(z);
            FREE(ObjZipArchive, object);
            break;
        }

        case OBJ_XLS_DOCUMENT: {
            /*  — libere le C-heap possede : chaque cellule (valeur
             * malloc'd), les tableaux cells/merges, le nom de feuille, le
             * tableau de feuilles et le chemin. xlsFerme laisse toujours >=1
             * feuille (invariant du type), donc sheets/sheetCount restent
             * valides ici — la boucle balaie l'integralite du C-heap. */
            ObjXlsDocument* d = (ObjXlsDocument*)object;
            for (int i = 0; i < d->sheetCount; i++) {
                XlsSheet* s = &d->sheets[i];
                for (int c = 0; c < s->cellCount; c++) {
                    free(s->cells[c].value);
                    free(s->cells[c].mask);   /*  — font/border = Value, GC */
                }
                free(s->cells);
                free(s->merges);
                free(s->rowDims);            /*  */
                free(s->colDims);            /*  */
                free(s->name);
            }
            free(d->sheets);
            free(d->path);
            /*  — parts ZIP preservees (octets bruts C-heap). */
            for (int p = 0; p < d->preservedCount; p++) {
                free(d->preservedNames[p]);
                free(d->preservedData[p]);
            }
            free(d->preservedNames);
            free(d->preservedData);
            free(d->preservedLens);
            FREE(ObjXlsDocument, object);
            break;
        }

        case OBJ_XLS_REF:
            /*  — enveloppe pure : `doc` est un Obj trace, rien a liberer. */
            FREE(ObjXlsRef, object);
            break;

        case OBJ_MOCK_NATIVE:
            FREE(ObjMockNative, object);
            break;

        case OBJ_PATTERN:
            FREE(ObjPattern, object);
            break;

        case OBJ_UI_CONTROL: {
            ObjUiControl* c = (ObjUiControl*)object;
            free(c->name);
            if (c->headless_store) {
                table_free(c->headless_store);
                free(c->headless_store);
            }
            FREE(ObjUiControl, object);
            break;
        }
        /*  — release du tableau controls (les ObjUiControlInstance
         * referencees sont liberees independamment par le GC sweep). decl
         * n'est PAS owned : il vit dans vm.windows[] et a sa propre vie. */
        case OBJ_WINDOW_INSTANCE: {
            ObjWindowInstance* w = (ObjWindowInstance*)object;
            free(w->controls);
            /*  lot B — libere le casier headless si alloue. Les
             * Values qu'il contient sont liberees independamment par le GC. */
            if (w->headless_store) {
                table_free(w->headless_store);
                free(w->headless_store);
            }
            FREE(ObjWindowInstance, object);
            break;
        }
        case OBJ_UI_CONTROL_INSTANCE: {
            ObjUiControlInstance* c = (ObjUiControlInstance*)object;
            if (c->headless_store) {
                table_free(c->headless_store);
                free(c->headless_store);
            }
            if (c->headless_override_keys) {  /*  */
                table_free(c->headless_override_keys);
                free(c->headless_override_keys);
            }
            FREE(ObjUiControlInstance, object);
            break;
        }
        case OBJ_UI_CONTROL_REF: {
            FREE(ObjUiControlRef, object);
            break;
        }
        case OBJ_WINDOW_REF: {
            /*  lot B — name libere independamment par le GC.
             * Le fallback_store (Values) est libere par le GC ; on libere
             * juste la Table elle-meme et son tableau d'entries. */
            ObjWindowRef* r = (ObjWindowRef*)object;
            if (r->fallback_store) {
                table_free(r->fallback_store);
                free(r->fallback_store);
            }
            FREE(ObjWindowRef, object);
            break;
        }
        case OBJ_ETAT_INSTANCE: {
            /*  lot D — release du tableau controls[]. Les
             * ObjUiControlInstance elles-memes sont liberees independamment
             * par le GC sweep. decl pointe vers vm.etats : non-owned. */
            ObjEtatInstance* e = (ObjEtatInstance*)object;
            free(e->controls);
            FREE(ObjEtatInstance, object);
            break;
        }
        case OBJ_ICONE: {
            /*  Lot 2b — svg_raw est un ObjString GC-managed,
             * libere independamment par le sweep apres mark. Rien d'autre
             * a relacher : catalog_hash est une valeur, les props sont
             * des scalaires. */
            FREE(ObjIcone, object);
            break;
        }
        case OBJ_ICONES_NAMESPACE: {
            /*  Lot 2e-1 — pas de payload. */
            FREE(ObjIconesNamespace, object);
            break;
        }
        case OBJ_THEME_NAMESPACE: {
            /*  Lot 2b+3 — pas de payload. */
            FREE(ObjThemeNamespace, object);
            break;
        }
        case OBJ_VECTOR: {
            /*  — le double[] est malloc'd (pas GC), a liberer ici. */
            ObjVector* vec = (ObjVector*)object;
            owl_gc_account_ext(-((int64_t)sizeof(double) * vec->capacity));  /*  */
            free(vec->data);
            FREE(ObjVector, object);
            break;
        }
        case OBJ_MATRIX: {
            /*  — le double[] row-major est malloc'd (pas GC). */
            ObjMatrix* mat = (ObjMatrix*)object;
            owl_gc_account_ext(-((int64_t)sizeof(double) * mat->rows * mat->cols));  /*  */
            free(mat->data);
            FREE(ObjMatrix, object);
            break;
        }
        case OBJ_MATRIX_ROW:
            /*  — vue sans payload propre (data appartient a `mat`). */
            FREE(ObjMatrixRow, object);
            break;
        case OBJ_TENSOR:
            /*  — data/grad/parents sont des Obj GC : rien a free ici,
             * le sweep les libere par eux-memes. shape/strides/perm sont inline
             *, pas de payload malloc propre. */
            FREE(ObjTensor, object);
            break;
        case OBJ_TENSOR_VIEW:
            /*  — vue sans payload propre (le buffer appartient a
             * `backing`, un Obj GC). Rien a free. */
            FREE(ObjTensorView, object);
            break;
        case OBJ_IAMODELE: {
            /*  — les Tenseurs poids/biais sont des Obj GC (liberes par
             * le sweep) ; seul le tableau de couches est un payload C-heap. */
            ObjIAModele* m = (ObjIAModele*)object;
            free(m->layers);
            free(m->gnodes);   /*  — nœuds du graphe general (C-heap) */
            FREE(ObjIAModele, object);
            break;
        }
        case OBJ_OPTIONS_ENTRAINEMENT:
            /*  — feuille pure : aucun payload C-heap, aucun Obj GC. */
            FREE(ObjOptionsEntrainement, object);
            break;
        case OBJ_MLARBRE: {
            /*  — l'arbre appris est du C-heap pur (nœuds malloc'd,
             * aucune ref OWL) : le libérer récursivement ici. Feuille GC. */
            ObjMLArbre* a = (ObjMLArbre*)object;
            obj_mlarbre_free_tree(a->root);
            FREE(ObjMLArbre, object);
            break;
        }
        case OBJ_MLFORET: {
            /*  — les arbres de l'ensemble sont du C-heap pur : libérer
             * chacun récursivement, puis le tableau. Feuille GC. */
            ObjMLForet* f = (ObjMLForet*)object;
            obj_mlforet_free_trees(f);
            FREE(ObjMLForet, object);
            break;
        }
        case OBJ_MLBOOSTING: {
            /*  — les arbres de régression et les scores initiaux sont du
             * C-heap pur : libérer chaque arbre, le tableau et init0. Feuille GC. */
            ObjMLBoosting* b = (ObjMLBoosting*)object;
            obj_mlboosting_free_trees(b);
            FREE(ObjMLBoosting, object);
            break;
        }
        case OBJ_MLLINEAIRE: {
            /*  — les poids OLS sont du C-heap pur : les libérer. Feuille GC. */
            ObjMLLineaire* r = (ObjMLLineaire*)object;
            obj_mllineaire_free_weights(r);
            FREE(ObjMLLineaire, object);
            break;
        }
        case OBJ_MLLOGISTIQUE: {
            /*  — les poids softmax (W et bias) sont du C-heap pur : les
             * libérer. Feuille GC. */
            ObjMLLogistique* r = (ObjMLLogistique*)object;
            obj_mllogistique_free_weights(r);
            FREE(ObjMLLogistique, object);
            break;
        }
        case OBJ_MLKMEANS: {
            /*  — les centroïdes appris sont du C-heap pur : les libérer.
             * Feuille GC. */
            ObjMLKMeans* k = (ObjMLKMeans*)object;
            obj_mlkmeans_free_centroids(k);
            FREE(ObjMLKMeans, object);
            break;
        }
        case OBJ_LLM: {
            /*  — finalizer : libere les handles llama (llama_free +
             * llama_model_free) meme si l'utilisateur n'a rien fait
             * explicitement. Idempotent, safe sur un LLM jamais charge. */
            ObjLLM* l = (ObjLLM*)object;
            obj_llm_free_handles(l);
            FREE(ObjLLM, object);
            break;
        }
        case OBJ_OPTIONS_GENERATION:
            /*  — feuille : le callback onToken est un Obj GC libere
             * independamment par le sweep, aucun payload C-heap propre. */
            FREE(ObjOptionsGeneration, object);
            break;
        case OBJ_MESSAGE_LLM:
            /*  — rôle/contenu sont des ObjString GC liberes par le
             * sweep, aucun payload C-heap propre. */
            FREE(ObjMessageLLM, object);
            break;
        case OBJ_RESPONSE_LLM:
            /*  — contenu ObjString GC libere par le sweep, reste
             * scalaire, aucun payload C-heap propre. */
            FREE(ObjResponseLLM, object);
            break;
        case OBJ_DETECTION:
            /*  — boite ObjArray GC liberee par le sweep, reste
             * scalaire, aucun payload C-heap propre. */
            FREE(ObjDetection, object);
            break;
        case OBJ_IMAGE_ETIQUETEE:
            /*  — path/label/image sont des Obj GC liberes par le
             * sweep, aucun payload C-heap propre. */
            FREE(ObjImageEtiquetee, object);
            break;
        case OBJ_IMAGE_ANNOTEE:
            /*  — path/boxes/image sont des Obj GC liberes par le
             * sweep, aucun payload C-heap propre. */
            FREE(ObjImageAnnotee, object);
            break;
        case OBJ_DATASET: {
            /*  — les exemples et noms de classes sont des Obj GC
             * (liberes par le sweep) ; seuls les deux tableaux de pointeurs
             * sont des payloads C-heap propres. */
            ObjDataset* ds = (ObjDataset*)object;
            free(ds->items);
            free(ds->classes);
            FREE(ObjDataset, object);
            break;
        }
    }
}
