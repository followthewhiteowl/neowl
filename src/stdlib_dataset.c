// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_dataset.c — Donnees images + Dataset.
 *
 * Trois briques, au-dessus du socle Image (owl_image) et Tenseur N-D
 * :
 *
 *   - ImageVersTenseur(img) / ImageToTensor : convertit une Image en Tenseur
 *     C×H×W (channels-first), pixels normalises [0,1] (÷255), 3 canaux RGB
 *     (alpha ignore). Pont C direct depuis owl_image_pixels (pas de boucle
 *     pixel OWL). Lecture seule sur l'image.
 *
 *   - Type ImageÉtiquetée / LabeledImage : un exemple etiquete (chemin +
 *     etiquette + Image materialisee paresseusement — cf object.h /
 *     obj_image_etiquetee_materialize). Proprietes cablees cote VM
 *     (OP_STRUCT_GET).
 *
 *   - Type Dataset (de ImageÉtiquetée) : collection ordonnee d'exemples, avec
 *     .Mélange / .NombreLots / .LotEntrées / .LotCibles. Chargee par
 *     ChargeImagesÉtiquetéesDepuisDossier (dossier-par-classe) ou
 *     ChargeImagesÉtiquetéesDepuisCSV (manifeste chemin;etiquette). Indexation
 *     ds[i] et proprietes cablees cote VM.
 *
 * Les lots empilent ImageVersTenseur sur chaque exemple : X en B×C×H×W, Y en
 * indices de classe (0-based, ordre alphabetique des classes). Aucun nouveau
 * mecanisme de stockage : on reutilise obj_new_tensor_nd.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_methods.h"
#include "owl_errors.h"
#include "owl_image.h"
#include "vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef _WIN32
  #define OWL_DS_SEP '\\'
#else
  #define OWL_DS_SEP '/'
#endif

/* image_make_instance : cree une ObjInstance Image a partir d'un OwlImage*
 * (gere son propre rooting GC). Non-static dans stdlib_image.c, extern-only ici
 * (meme convention que stdlib_ui.c). owl_image_handle_from_value : OwlImage*
 * d'une Value Image. */
extern ObjInstance* image_make_instance(OwlImage* img);
extern OwlImage*    owl_image_handle_from_value(Value v);

/* ================================================================
 * Levee d'exception rattrapable depuis une native / methode
 * ================================================================ */

/* Rend le template `code` dans la langue active (paires cle/valeur terminees
 * par NULL) puis arme un throw rattrapable (owl_vm_raise_native). Le message
 * rendu vit dans un buffer partage du registry — owl_vm_raise_native le copie.
 * A appeler suivi de `return NIL_VAL` : le dispatch OP_CALL/OP_INVOKE detecte
 * le throw en attente et le route vers QUAND EXCEPTION. */
static void ds_raise(OwlErrorCode code, const char* origin, ...) {
    va_list ap;
    va_start(ap, origin);
    const char* msg = owl_error_render_active_va(code, ap);
    va_end(ap);
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    owl_vm_raise_native(origin, d != NULL ? d->name : "", msg);
}

/* ================================================================
 * ImageVersTenseur — pont pixels RGBA8 (HWC) -> Tenseur C×H×W [0,1]
 * ================================================================ */

/* Ecrit la representation C×H×W normalisee d'une image (valide) dans `out`
 * (3*H*W doubles, ordre plat row-major : out[c*H*W + y*W + x]). Le buffer
 * source est RGBA8 HWC top-down (R,G,B,A par pixel) : on lit R/G/B, on
 * ignore l'alpha, on divise par 255. */
static void ds_image_fill_chw(const OwlImage* img, double* out) {
    int W = owl_image_width(img);
    int H = owl_image_height(img);
    const uint8_t* px = owl_image_pixels(img);
    if (px == NULL) return;
    for (int c = 0; c < 3; c++) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                out[c * H * W + y * W + x] =
                    (double)px[(y * W + x) * 4 + c] / 255.0;
            }
        }
    }
}

/* Cree un Tenseur C×H×W a partir d'une Image deja materialisee. `img` DOIT
 * etre valide. Rend NULL si l'alloc echoue (jamais en pratique). Le Tenseur
 * revient rooté sur la pile par obj_new_tensor_nd puis pop — le caller le
 * repush aussitot s'il empile d'autres allocations. */
static ObjTensor* ds_image_to_tensor(const OwlImage* img) {
    int H = owl_image_height(img);
    int W = owl_image_width(img);
    int shape[3] = { 3, H, W };
    ObjTensor* t = obj_new_tensor_nd(3, shape);
    ds_image_fill_chw(img, tensor_cells(t->data));
    return t;
}

/* ImageVersTenseur(img) / ImageToTensor(img). Image invalide -> exception
 * rattrapable (jamais de plantage). */
static Value fn_image_vers_tenseur(int argCount, Value* args) {
    (void)argCount;
    OwlImage* img = owl_image_handle_from_value(args[0]);
    if (img == NULL || !owl_image_valid(img)) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, "ImageVersTenseur",
                 "arg", "image",
                 "expected", "Image valide",
                 "actual", "Image invalide", NULL);
        return NIL_VAL;
    }
    /* Aucune allocation OWL apres obj_new_tensor_nd (remplissage = doubles),
     * le Tenseur reste sain sans repush. */
    return OBJ_VAL(ds_image_to_tensor(img));
}

/* ================================================================
 * ImageÉtiquetée — materialisation paresseuse (decl object.h)
 * ================================================================ */

ObjInstance* obj_image_etiquetee_materialize(ObjImageEtiquetee* ie) {
    if (ie->image != NULL) return (ObjInstance*)ie->image;
    if (ie->path == NULL) return NULL;
    OwlImage* img = owl_image_load_from_file(ie->path->chars);
    if (img == NULL) return NULL;
    /* image_make_instance gere son propre rooting ; `ie` est roote par le
     * caller VM pendant cet appel. Aucune allocation OWL entre le retour et
     * l'assignation -> l'instance reste saine, puis reachable via ie->image. */
    ObjInstance* inst = image_make_instance(img);
    ie->image = (Obj*)inst;
    return inst;
}

ObjInstance* obj_image_annotee_materialize(ObjImageAnnotee* ia) {
    if (ia->image != NULL) return (ObjInstance*)ia->image;
    if (ia->path == NULL) return NULL;
    OwlImage* img = owl_image_load_from_file(ia->path->chars);
    if (img == NULL) return NULL;
    ObjInstance* inst = image_make_instance(img);
    ia->image = (Obj*)inst;
    return inst;
}

/* ================================================================
 * Accesseurs d'exemple generiques (dispatch sur le type d'exemple)
 * ================================================================ */

/* Chemin disque d'un exemple, quel que soit son type (classification ou
 * detection). Commun aux deux : c'est ce dont dependent les lots d'entrees et la
 * materialisation d'Image. NULL si le type d'exemple n'expose pas de chemin. */
static ObjString* ds_item_path(Obj* item) {
    if (item == NULL) return NULL;
    switch (item->type) {
        case OBJ_IMAGE_ETIQUETEE: return ((ObjImageEtiquetee*)item)->path;
        case OBJ_IMAGE_ANNOTEE:   return ((ObjImageAnnotee*)item)->path;
        default: return NULL;
    }
}

/* Etiquette de classe d'un exemple de classification. NULL pour un exemple de
 * detection (pas d'etiquette scalaire — cibles = boites, hors perimetre des
 * lots de cibles classification). */
static ObjString* ds_item_label(Obj* item) {
    if (item != NULL && item->type == OBJ_IMAGE_ETIQUETEE)
        return ((ObjImageEtiquetee*)item)->label;
    return NULL;
}

/* ================================================================
 * Chargeurs : construction d'un Dataset
 * ================================================================ */

/* Extensions image reconnues par le chargeur dossier (case-insensible). Le
 * contenu n'est PAS validé a l'import (materialisation paresseuse) : seul le
 * suffixe filtre les fichiers annexes (README, .csv...). */
static bool ds_has_image_ext(const char* name) {
    const char* dot = strrchr(name, '.');
    if (dot == NULL) return false;
    static const char* const exts[] = {
        "png", "jpg", "jpeg", "bmp", "gif", "svg", "webp", "tga", NULL
    };
    for (int i = 0; exts[i] != NULL; i++) {
        if (strcasecmp(dot + 1, exts[i]) == 0) return true;
    }
    return false;
}

/* Vrai si `path` designe un repertoire. */
static bool ds_is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

/* Comparateur qsort de chaines C (ordre lexicographique par octet). */
static int ds_cmp_cstr(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

/* strdup portable (evite _strdup Windows / la variante POSIX non-standard). */
static char* ds_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p != NULL) memcpy(p, s, n);
    return p;
}

/* Liste triee (alpha) des noms d'entrees d'un repertoire selon un predicat.
 * Rend un tableau C-heap de chaines C-heap (a liberer par ds_free_names), ou
 * NULL si le repertoire est illisible. `*outCount` recoit le nombre d'entrees.
 * `wantDir` : true = sous-dossiers, false = fichiers a extension image. */
static char** ds_list_dir(const char* dirPath, bool wantDir, int* outCount) {
    DIR* d = opendir(dirPath);
    if (d == NULL) { *outCount = 0; return NULL; }
    char** names = NULL;
    int count = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char full[4096];
        int n = snprintf(full, sizeof(full), "%s%c%s",
                         dirPath, OWL_DS_SEP, e->d_name);
        if (n <= 0 || n >= (int)sizeof(full)) continue;
        if (wantDir) {
            if (!ds_is_dir(full)) continue;
        } else {
            if (ds_is_dir(full)) continue;
            if (!ds_has_image_ext(e->d_name)) continue;
        }
        if (count >= cap) {
            cap = (cap == 0) ? 16 : cap * 2;
            names = (char**)realloc(names, (size_t)cap * sizeof(char*));
        }
        names[count++] = ds_strdup(e->d_name);
    }
    closedir(d);
    if (names != NULL) qsort(names, (size_t)count, sizeof(char*), ds_cmp_cstr);
    *outCount = count;
    return names;
}

static void ds_free_names(char** names, int count) {
    if (names == NULL) return;
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* ChargeImagesÉtiquetéesDepuisDossier(chemin) / LoadLabeledImagesFromFolder.
 * Arborescence « un dossier = une classe » : chaque sous-dossier devient une
 * classe (etiquette = nom du dossier), chaque fichier image qu'il contient un
 * exemple. Classes triees alphabetiquement (= index de classe). Dossier
 * inexistant -> exception rattrapable. */
static Value fn_charge_dossier(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH,
                 "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisDossier",
                 "arg", "chemin", "expected", "chaine",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjString* rootStr = AS_STRING(args[0]);
    const char* root = rootStr->chars;

    int classCount = 0;
    char** classNames = ds_list_dir(root, true, &classCount);
    if (classNames == NULL) {
        ds_raise(E_NATIVE_IO_FAILURE,
                 "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisDossier",
                 "native", "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisDossier",
                 "path", root, "detail", "dossier introuvable ou illisible",
                 NULL);
        return NIL_VAL;
    }

    ObjDataset* ds = obj_new_dataset();
    vm_push(OBJ_VAL(ds));               /* root pendant toute la construction */
    ds->itemType = OBJ_IMAGE_ETIQUETEE; /* exemples de classification */
    ds->source = rootStr;               /* exact chemin passe (deja roote args[0]) */

    for (int ci = 0; ci < classCount; ci++) {
        /* Nom de classe partage par tous ses exemples (une seule ObjString). */
        ObjString* classStr =
            obj_copy_string(classNames[ci], (int)strlen(classNames[ci]));
        obj_dataset_push_class(ds, classStr);   /* trace via ds -> survivant */

        char classDir[4096];
        int cn = snprintf(classDir, sizeof(classDir), "%s%c%s",
                          root, OWL_DS_SEP, classNames[ci]);
        if (cn <= 0 || cn >= (int)sizeof(classDir)) continue;

        int fileCount = 0;
        char** files = ds_list_dir(classDir, false, &fileCount);
        for (int fi = 0; fi < fileCount; fi++) {
            char full[4096];
            int fn = snprintf(full, sizeof(full), "%s%c%s",
                              classDir, OWL_DS_SEP, files[fi]);
            if (fn <= 0 || fn >= (int)sizeof(full)) continue;
            ObjString* pathStr = obj_copy_string(full, fn);
            vm_push(OBJ_VAL(pathStr));  /* root pendant l'alloc de l'exemple */
            ObjImageEtiquetee* item = obj_new_image_etiquetee(pathStr, classStr);
            obj_dataset_push_item(ds, (Obj*)item);  /* trace via ds */
            vm_pop();                   /* pathStr — retenu par item (dans ds) */
        }
        ds_free_names(files, fileCount);
    }
    ds_free_names(classNames, classCount);

    vm_pop();                           /* ds */
    return OBJ_VAL(ds);
}

/* Cherche `label` dans ds->classes ; l'ajoute (fin de tableau) si absent. Rend
 * l'ObjString de classe (celui du tableau, partageable). `label` et `ds`
 * DOIVENT etre rootes par le caller. */
static ObjString* ds_intern_class(ObjDataset* ds, ObjString* label) {
    for (int i = 0; i < ds->classCount; i++) {
        if (ds->classes[i] != NULL
            && ds->classes[i]->length == label->length
            && memcmp(ds->classes[i]->chars, label->chars,
                      (size_t)label->length) == 0) {
            return ds->classes[i];
        }
    }
    obj_dataset_push_class(ds, label);
    return label;
}

/* Tri alpha des classes APRES construction (pour un index de classe
 * deterministe 0-based en ordre alphabetique, comme le chargeur dossier). Le
 * tableau `classes` est petit ; tri par insertion sur les ObjString*. */
static void ds_sort_classes(ObjDataset* ds) {
    for (int i = 1; i < ds->classCount; i++) {
        ObjString* key = ds->classes[i];
        int j = i - 1;
        while (j >= 0 && strcmp(ds->classes[j]->chars, key->chars) > 0) {
            ds->classes[j + 1] = ds->classes[j];
            j--;
        }
        ds->classes[j + 1] = key;
    }
}

/* ChargeImagesÉtiquetéesDepuisCSV(cheminCSV [, séparateur]) /
 * LoadLabeledImagesFromCSV. Manifeste « chemin<sep>etiquette », une ligne par
 * exemple, ordre des lignes PRESERVE. Separateur ";" par defaut. Fichier
 * inexistant -> exception rattrapable. */
static Value fn_charge_csv(int argCount, Value* args) {
    if (!IS_STRING(args[0])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH,
                 "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisCSV",
                 "arg", "cheminCSV", "expected", "chaine",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjString* csvStr = AS_STRING(args[0]);
    const char* csvPath = csvStr->chars;
    char sep = ';';
    if (argCount >= 2 && IS_STRING(args[1]) && AS_STRING(args[1])->length >= 1) {
        sep = AS_STRING(args[1])->chars[0];
    }

    FILE* f = fopen(csvPath, "rb");
    if (f == NULL) {
        ds_raise(E_NATIVE_IO_FAILURE,
                 "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisCSV",
                 "native", "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisCSV",
                 "path", csvPath, "detail", "fichier introuvable ou illisible",
                 NULL);
        return NIL_VAL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); size = 0; }
    char* buf = (char*)malloc((size_t)size + 1);
    size_t rd = (size > 0) ? fread(buf, 1, (size_t)size, f) : 0;
    buf[rd] = '\0';
    fclose(f);

    ObjDataset* ds = obj_new_dataset();
    vm_push(OBJ_VAL(ds));               /* root pendant la construction */
    ds->itemType = OBJ_IMAGE_ETIQUETEE; /* exemples de classification */
    ds->source = csvStr;                /* exact chemin CSV passe */

    /* Parcours ligne a ligne : un manifeste chemin<sep>etiquette. Le decoupage
     * se fait sur le PREMIER separateur (un chemin ne contient pas le
     * separateur dans les cas cibles) ; le \r final (CRLF) est retire. */
    char* p = buf;
    while (*p != '\0') {
        char* lineStart = p;
        while (*p != '\0' && *p != '\n') p++;
        char* lineEnd = p;
        if (*p == '\n') p++;
        if (lineEnd > lineStart && *(lineEnd - 1) == '\r') lineEnd--;
        int lineLen = (int)(lineEnd - lineStart);
        if (lineLen <= 0) continue;     /* ligne vide ignoree */

        /* Premier separateur. */
        int sepPos = -1;
        for (int i = 0; i < lineLen; i++) {
            if (lineStart[i] == sep) { sepPos = i; break; }
        }
        const char* pathBeg; int pathLen;
        const char* labBeg;  int labLen;
        if (sepPos < 0) {
            /* Pas de separateur : toute la ligne est le chemin, etiquette vide. */
            pathBeg = lineStart; pathLen = lineLen;
            labBeg = lineStart + lineLen; labLen = 0;
        } else {
            pathBeg = lineStart; pathLen = sepPos;
            labBeg = lineStart + sepPos + 1; labLen = lineLen - sepPos - 1;
        }

        ObjString* pathStr = obj_copy_string(pathBeg, pathLen);
        vm_push(OBJ_VAL(pathStr));      /* root pendant les allocs suivantes */
        ObjString* labStr = obj_copy_string(labBeg, labLen);
        vm_push(OBJ_VAL(labStr));
        ObjString* classStr = ds_intern_class(ds, labStr); /* label ou existant */
        ObjImageEtiquetee* item = obj_new_image_etiquetee(pathStr, classStr);
        obj_dataset_push_item(ds, (Obj*)item);
        vm_pop();                       /* labStr — retenu si classe neuve, sinon libre */
        vm_pop();                       /* pathStr — retenu par item */
    }
    free(buf);

    ds_sort_classes(ds);                /* index de classe alphabetique */

    vm_pop();                           /* ds */
    return OBJ_VAL(ds);
}

/* ChargeImagesAnnotéesDepuisYOLO(chemin) / LoadAnnotatedImagesFromYOLO. Un
 * dossier « images + labels YOLO-txt » : chaque fichier image devient un
 * exemple, ses boites sont lues dans le `.txt` de meme nom de base (une ligne
 * par boite `classe cx cy largeur hauteur`, coordonnees normalisees 0..1). Les
 * images sont enumerees par ordre alphabetique. Une image sans `.txt` (ou
 * `.txt` vide) est un exemple valide a 0 boite (fond). Dossier inexistant ->
 * exception rattrapable. Les coordonnees sont conservees telles que lues (pas
 * de conversion pixels). */
static Value fn_charge_yolo(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH,
                 "ChargeImagesAnnot\xc3\xa9""esDepuisYOLO",
                 "arg", "chemin", "expected", "chaine",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjString* rootStr = AS_STRING(args[0]);
    const char* root = rootStr->chars;

    int fileCount = 0;
    char** files = ds_list_dir(root, false, &fileCount);
    if (files == NULL) {
        ds_raise(E_NATIVE_IO_FAILURE,
                 "ChargeImagesAnnot\xc3\xa9""esDepuisYOLO",
                 "native", "ChargeImagesAnnot\xc3\xa9""esDepuisYOLO",
                 "path", root, "detail", "dossier introuvable ou illisible",
                 NULL);
        return NIL_VAL;
    }

    ObjDataset* ds = obj_new_dataset();
    vm_push(OBJ_VAL(ds));               /* root pendant toute la construction */
    ds->itemType = OBJ_IMAGE_ANNOTEE;   /* exemples de detection */
    ds->source = rootStr;               /* exact chemin passe (deja roote args[0]) */

    for (int fi = 0; fi < fileCount; fi++) {
        char full[4096];
        int fn = snprintf(full, sizeof full, "%s%c%s",
                          root, OWL_DS_SEP, files[fi]);
        if (fn <= 0 || fn >= (int)sizeof full) continue;

        /* Chemin du fichier d'annotation : meme nom de base, extension .txt. */
        char txt[4096];
        const char* dot = strrchr(files[fi], '.');
        int baseLen = (dot != NULL) ? (int)(dot - files[fi])
                                    : (int)strlen(files[fi]);
        int tn = snprintf(txt, sizeof txt, "%s%c%.*s.txt",
                          root, OWL_DS_SEP, baseLen, files[fi]);
        bool txtOk = (tn > 0 && tn < (int)sizeof txt);

        ObjString* pathStr = obj_copy_string(full, fn);
        vm_push(OBJ_VAL(pathStr));      /* root pendant les allocs suivantes */
        ObjArray* boxes = obj_new_array();
        vm_push(OBJ_VAL(boxes));        /* root pendant le remplissage */

        FILE* tf = txtOk ? fopen(txt, "rb") : NULL;
        if (tf != NULL) {
            fseek(tf, 0, SEEK_END);
            long sz = ftell(tf);
            fseek(tf, 0, SEEK_SET);
            if (sz < 0) sz = 0;
            char* tbuf = (char*)malloc((size_t)sz + 1);
            size_t rd = (sz > 0) ? fread(tbuf, 1, (size_t)sz, tf) : 0;
            tbuf[rd] = '\0';
            fclose(tf);

            /* Une ligne par boite : `classe cx cy largeur hauteur`, separateurs
             * espaces/tabulations. \r final (CRLF) retire. Ligne blanche
             * ignoree (0 boite). Les valeurs sont lues telles quelles. */
            char* p = tbuf;
            while (*p != '\0') {
                char* ls = p;
                while (*p != '\0' && *p != '\n') p++;
                char* le = p;
                if (*p == '\n') p++;
                if (le > ls && *(le - 1) == '\r') le--;

                double vals[5];
                int nv = 0;
                const char* q = ls;
                while (nv < 5 && q < le) {
                    while (q < le && (*q == ' ' || *q == '\t')) q++;
                    if (q >= le) break;
                    char* endp = NULL;
                    double v = strtod(q, &endp);
                    if (endp == q) break;   /* pas un nombre : stop de ligne */
                    vals[nv++] = v;
                    q = endp;
                }
                if (nv == 0) continue;      /* ligne blanche -> pas de boite */

                ObjArray* box = obj_new_array();
                vm_push(OBJ_VAL(box));      /* root pendant le remplissage + append */
                for (int i = 0; i < 5; i++)
                    value_array_write(&box->elements,
                                      REAL_VAL(i < nv ? vals[i] : 0.0));
                obj_array_push(boxes, OBJ_VAL(box));
                vm_pop();                   /* box — retenu par boxes */
            }
            free(tbuf);
        }

        ObjImageAnnotee* item = obj_new_image_annotee(pathStr, (Obj*)boxes);
        obj_dataset_push_item(ds, (Obj*)item);  /* trace via ds */
        vm_pop();                       /* boxes — retenu par item */
        vm_pop();                       /* pathStr — retenu par item */
    }
    ds_free_names(files, fileCount);

    vm_pop();                           /* ds */
    return OBJ_VAL(ds);
}

/* ================================================================
 * Methodes du Dataset
 * ================================================================ */

/* Index de classe (0-based) d'une etiquette dans le tableau trie des classes.
 * -1 si absente (ne devrait pas arriver : chaque exemple a une classe interne). */
static int ds_class_index(ObjDataset* ds, ObjString* label) {
    if (label == NULL) return -1;
    for (int i = 0; i < ds->classCount; i++) {
        if (ds->classes[i] != NULL
            && ds->classes[i]->length == label->length
            && memcmp(ds->classes[i]->chars, label->chars,
                      (size_t)label->length) == 0) {
            return i;
        }
    }
    return -1;
}

/* .Mélange() / .Shuffle() — rebat l'ordre des exemples (Fisher-Yates), en
 * place, sans perte ni doublon. Ne change ni ..Taille ni l'ensemble des
 * exemples. Rend le receveur. */
static Value fn_dataset_melange(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    for (int i = ds->count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        Obj* tmp = ds->items[i];
        ds->items[i] = ds->items[j];
        ds->items[j] = tmp;
    }
    return args[0];
}

/* .RetournementHorizontalAléatoire() / .RandomHorizontalFlip() — active le
 * flip horizontal aleatoire (miroir gauche/droite, proba 0,5 par image et par
 * epoque), applique au vol dans .LotEntrées. Opt-in : sans appel, aucune
 * augmentation. Rend le receveur (chainable). */
static Value fn_dataset_flip_horizontal(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    ds->flipHorizontalActif = true;
    return args[0];
}

/* .RecadrageAléatoire(marge) / .RandomCrop(margin) — active le recadrage
 * aleatoire : zero-padding de `marge` px sur H et W puis crop aleatoire
 * ramenant a la taille d'origine (decalage uniforme dans [0, 2*marge]).
 * marge = 0 desactive ; marge < 0 -> exception. Applique au vol dans
 * .LotEntrées. Rend le receveur (chainable). */
static Value fn_dataset_recadrage(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    if (!IS_INTEGER(args[1]) && !IS_REAL(args[1])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, "RecadrageAl\xc3\xa9""atoire",
                 "arg", "marge", "expected", "entier",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int marge = IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1])
                                    : (int)AS_REAL(args[1]);
    if (marge < 0) {
        ds_raise(E_NATIVE_ARG_OUT_OF_RANGE, "RecadrageAl\xc3\xa9""atoire",
                 "arg", "marge", NULL);
        return NIL_VAL;
    }
    ds->margeRecadrage = marge;
    return args[0];
}

/* Augmente EN PLACE une tranche C×H×W (3 canaux, plat row-major
 * out[c*H*W + y*W + x]) selon les flags du Dataset. Ordre standard CIFAR :
 * recadrage (zero-padding `marge` px + crop aleatoire) PUIS flip horizontal.
 * Les tirages (rand()) suivent cet ordre : d'abord (dy, dx) du crop si actif,
 * puis la piece du flip si actif — aucun tirage si le flag est inactif, la
 * sequence RNG reste donc alignee sur ce qui est reellement applique. `scratch`
 * (3*H*W doubles) est fourni par le caller, requis seulement si marge > 0. */
static void ds_augment_slice(double* img, int H, int W, bool flip, int marge,
                             double* scratch) {
    int slice = 3 * H * W;
    if (marge > 0) {
        int span = 2 * marge + 1;
        int dy = rand() % span;
        int dx = rand() % span;
        for (int c = 0; c < 3; c++) {
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    int sy = y + dy - marge;
                    int sx = x + dx - marge;
                    double v = 0.0;
                    if (sy >= 0 && sy < H && sx >= 0 && sx < W)
                        v = img[c * H * W + sy * W + sx];
                    scratch[c * H * W + y * W + x] = v;
                }
            }
        }
        memcpy(img, scratch, (size_t)slice * sizeof(double));
    }
    if (flip && (rand() % 2) == 1) {
        for (int c = 0; c < 3; c++) {
            for (int y = 0; y < H; y++) {
                double* row = img + c * H * W + y * W;
                for (int x = 0; x < W / 2; x++) {
                    double tmp = row[x];
                    row[x] = row[W - 1 - x];
                    row[W - 1 - x] = tmp;
                }
            }
        }
    }
}

/* Valide et lit une taille de lot (arg entier >= 1). Rend false + leve si
 * invalide. */
static bool ds_read_batch_size(Value v, const char* origin, int* out) {
    if (!IS_INTEGER(v) && !IS_REAL(v)) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, origin,
                 "arg", "taille", "expected", "entier",
                 "actual", "type non pris en charge", NULL);
        return false;
    }
    int size = IS_INTEGER(v) ? (int)AS_INTEGER(v) : (int)AS_REAL(v);
    if (size < 1) {
        ds_raise(E_NATIVE_ARG_OUT_OF_RANGE, origin, "arg", "taille", NULL);
        return false;
    }
    *out = size;
    return true;
}

/* .NombreLots(taille) / .BatchCount(size) — nombre de lots pour couvrir tous
 * les exemples (division plafond, dernier lot partiel compte). taille est un
 * parametre (jamais stocke dans le Dataset). */
static Value fn_dataset_nombre_lots(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    int size;
    if (!ds_read_batch_size(args[1], "NombreLots", &size)) return NIL_VAL;
    int lots = (ds->count + size - 1) / size;   /* plafond ; 0 si count == 0 */
    return INTEGER_VAL(lots);
}

/* Bornes du lot i (1-based) pour une taille donnee : offset de depart et
 * nombre d'exemples reels (dernier lot partiel = plus petit, pas de drop). Rend
 * false + leve si i hors plage. */
static bool ds_batch_bounds(ObjDataset* ds, int size, int index1,
                            const char* origin, int* outOffset, int* outB) {
    if (index1 < 1) {
        ds_raise(E_NATIVE_ARG_OUT_OF_RANGE, origin, "arg", "lot", NULL);
        return false;
    }
    int offset = (index1 - 1) * size;
    if (offset >= ds->count) {
        ds_raise(E_NATIVE_ARG_OUT_OF_RANGE, origin, "arg", "lot", NULL);
        return false;
    }
    int B = size;
    if (offset + B > ds->count) B = ds->count - offset;
    *outOffset = offset;
    *outB = B;
    return true;
}

/* .LotEntrées(taille, i) / .BatchInputs(size, i) — X du lot i : Tenseur
 * B×C×H×W (B = exemples reels du lot, C×H×W = ImageVersTenseur de chaque
 * exemple). Les images du lot doivent partager les memes dimensions (celles du
 * premier exemple) ; une image de taille differente -> exception rattrapable
 * (redimensionnement a faire explicitement en amont, pas d'etirement muet). Un
 * fichier absent/illisible -> exception rattrapable. */
static Value fn_dataset_lot_entrees(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    int size;
    if (!ds_read_batch_size(args[1], "LotEntr\xc3\xa9""es", &size)) return NIL_VAL;
    if (!IS_INTEGER(args[2]) && !IS_REAL(args[2])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, "LotEntr\xc3\xa9""es",
                 "arg", "lot", "expected", "entier",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int index1 = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2])
                                     : (int)AS_REAL(args[2]);
    int offset, B;
    if (!ds_batch_bounds(ds, size, index1, "LotEntr\xc3\xa9""es", &offset, &B))
        return NIL_VAL;

    /* Dimensions du lot = celles du premier exemple. Chargement C-heap (aucune
     * allocation OWL avant l'alloc du Tenseur). */
    OwlImage* im0 = owl_image_load_from_file(ds_item_path(ds->items[offset])->chars);
    if (im0 == NULL || !owl_image_valid(im0)) {
        if (im0 != NULL) owl_image_free(im0);
        ds_raise(E_NATIVE_IMAGE_LOAD_FAILURE, "LotEntr\xc3\xa9""es",
                 "native", "LotEntr\xc3\xa9""es",
                 "path", ds_item_path(ds->items[offset])->chars,
                 "detail", "fichier absent ou format non reconnu", NULL);
        return NIL_VAL;
    }
    int W = owl_image_width(im0);
    int H = owl_image_height(im0);
    int shape[4] = { B, 3, H, W };
    ObjTensor* t = obj_new_tensor_nd(4, shape);  /* zero-init */
    /* args[0] (ds) roote par le dispatch ; plus aucune allocation OWL en aval
     * (chargement image = C-heap, remplissage = doubles) -> `t` reste sain. */
    double* buf = tensor_cells(t->data);
    int slice = 3 * H * W;

    /* Augmentation opt-in, appliquee au vol a chaque exemple. Le
     * scratch C-heap ne sert qu'au recadrage (buffer de destination du crop).
     * Aucune allocation OWL en aval -> `t` reste sain sans repush. */
    bool augFlip = ds->flipHorizontalActif;
    int  augMarge = ds->margeRecadrage;
    bool augActif = augFlip || augMarge > 0;
    double* scratch = (augMarge > 0)
        ? (double*)malloc((size_t)slice * sizeof(double)) : NULL;

    ds_image_fill_chw(im0, buf + 0 * slice);
    owl_image_free(im0);
    if (augActif) ds_augment_slice(buf + 0 * slice, H, W, augFlip, augMarge, scratch);

    for (int k = 1; k < B; k++) {
        OwlImage* im = owl_image_load_from_file(ds_item_path(ds->items[offset + k])->chars);
        if (im == NULL || !owl_image_valid(im)) {
            if (im != NULL) owl_image_free(im);
            free(scratch);
            ds_raise(E_NATIVE_IMAGE_LOAD_FAILURE, "LotEntr\xc3\xa9""es",
                     "native", "LotEntr\xc3\xa9""es",
                     "path", ds_item_path(ds->items[offset + k])->chars,
                     "detail", "fichier absent ou format non reconnu", NULL);
            return NIL_VAL;
        }
        if (owl_image_width(im) != W || owl_image_height(im) != H) {
            /* Lot heterogene : empiler des images de tailles differentes
             * exigerait un choix de redimensionnement (etirement muet =
             * deformation + dataset mal prepare masque). On refuse ; le
             * redimensionnement doit etre explicite en amont. */
            char expected[32], actual[32];
            snprintf(expected, sizeof expected, "%dx%d", H, W);
            snprintf(actual, sizeof actual, "%dx%d",
                     owl_image_height(im), owl_image_width(im));
            owl_image_free(im);
            free(scratch);
            ds_raise(E_NATIVE_DATASET_BATCH_DIM_MISMATCH, "LotEntr\xc3\xa9""es",
                     "native", "LotEntr\xc3\xa9""es",
                     "expected", expected, "actual", actual, NULL);
            return NIL_VAL;
        }
        ds_image_fill_chw(im, buf + (size_t)k * slice);
        owl_image_free(im);
        if (augActif)
            ds_augment_slice(buf + (size_t)k * slice, H, W, augFlip, augMarge, scratch);
    }
    free(scratch);
    return OBJ_VAL(t);
}

/* .LotCibles(taille, i) / .BatchTargets(size, i) — Y du lot i : Tenseur 1D des
 * indices de classe (0-based, ordre alphabetique) de chaque exemple du lot. */
static Value fn_dataset_lot_cibles(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    /* Sur un Dataset de detection (ImageAnnotee) il n'y a pas d'etiquette de
     * classe scalaire : ds_item_label rendrait NULL -> ds_class_index -1 ->
     * 0.0, soit un Tenseur de zeros indistinguable de la classe 0 legitime.
     * On refuse explicitement plutot que de fabriquer une cible silencieuse. */
    if (ds->itemType != OBJ_IMAGE_ETIQUETEE) {
        ds_raise(E_NATIVE_DATASET_TARGETS_UNAVAILABLE_ON_DETECTION,
                 "LotCibles", NULL);
        return NIL_VAL;
    }
    int size;
    if (!ds_read_batch_size(args[1], "LotCibles", &size)) return NIL_VAL;
    if (!IS_INTEGER(args[2]) && !IS_REAL(args[2])) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, "LotCibles",
                 "arg", "lot", "expected", "entier",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int index1 = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2])
                                     : (int)AS_REAL(args[2]);
    int offset, B;
    if (!ds_batch_bounds(ds, size, index1, "LotCibles", &offset, &B))
        return NIL_VAL;

    ObjTensor* t = obj_new_tensor(B);
    /* Aucune allocation OWL en aval -> `t` reste sain sans repush. */
    double* buf = tensor_cells(t->data);
    for (int k = 0; k < B; k++) {
        int ci = ds_class_index(ds, ds_item_label(ds->items[offset + k]));
        buf[k] = (ci < 0) ? 0.0 : (double)ci;
    }
    return OBJ_VAL(t);
}

/* .SousEnsemble(fractionDébut, fractionFin) / .Subset(startFraction,
 * endFraction) — rend un NOUVEAU Dataset couvrant la tranche
 * [fractionDébut, fractionFin[ des exemples (fractions reelles dans [0,1],
 * début < fin). Les bornes en index sont round(fraction * ..Taille) : deux
 * tranches consecutives qui partagent une fraction partagent l'index de
 * coupure, donc partitionnent sans trou ni recouvrement. Les exemples de la
 * tranche sont PARTAGES (memes ObjImageEtiquetee que le parent — pas de copie
 * profonde : ce sont des Obj GC, chaque Dataset possede seulement son propre
 * tableau C-heap de pointeurs) ; les classes et la source du parent sont
 * conservees telles quelles (index de classe stables d'une tranche a l'autre).
 * Ne melange PAS : deterministe, a charge de l'appelant de faire .Mélange()
 * avant s'il veut de l'aleatoire. Fractions hors [0,1] ou début >= fin ->
 * exception rattrapable. */
static Value fn_dataset_sous_ensemble(int argCount, Value* args) {
    (void)argCount;
    ObjDataset* ds = AS_DATASET(args[0]);
    if ((!IS_INTEGER(args[1]) && !IS_REAL(args[1]))
        || (!IS_INTEGER(args[2]) && !IS_REAL(args[2]))) {
        ds_raise(E_NATIVE_ARG_TYPE_MISMATCH, "SousEnsemble",
                 "arg", "fraction", "expected", "reel",
                 "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    double f0 = IS_INTEGER(args[1]) ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
    double f1 = IS_INTEGER(args[2]) ? (double)AS_INTEGER(args[2]) : AS_REAL(args[2]);
    if (f0 < 0.0 || f0 > 1.0 || f1 < 0.0 || f1 > 1.0 || f0 >= f1) {
        ds_raise(E_NATIVE_ARG_OUT_OF_RANGE, "SousEnsemble",
                 "arg", "fraction", NULL);
        return NIL_VAL;
    }

    int start = (int)(f0 * ds->count + 0.5);   /* round : coupures adjacentes */
    int end   = (int)(f1 * ds->count + 0.5);
    if (start < 0) start = 0;
    if (end > ds->count) end = ds->count;
    if (end < start) end = start;

    ObjDataset* sub = obj_new_dataset();
    vm_push(OBJ_VAL(sub));              /* root pendant la construction */
    sub->itemType = ds->itemType;      /* meme type d'exemple que le parent */
    sub->source = ds->source;          /* source du parent conservee */
    /* Tableau de classes complet du parent (index de classe stables). Aucune
     * allocation OWL ici : obj_dataset_push_class = realloc C-heap pur. */
    for (int i = 0; i < ds->classCount; i++) {
        obj_dataset_push_class(sub, ds->classes[i]);
    }
    for (int i = start; i < end; i++) {
        obj_dataset_push_item(sub, ds->items[i]);  /* exemple partage */
    }
    vm_pop();                           /* sub */
    return OBJ_VAL(sub);
}

/* ================================================================
 * Registry
 * ================================================================ */

static const char* const ALIAS_image_etiquetee[] = { "LabeledImage", NULL };
static const char* const ALIAS_image_annotee[] = { "AnnotatedImage", NULL };

static const OwlTypeDef DATASET_TYPES[] = {
    { .canonical      = "Image\xc3\x89tiquet\xc3\xa9""e",   /* ImageÉtiquetée */
      .canonicalAscii = "ImageEtiquetee",
      .aliases        = ALIAS_image_etiquetee,
      .category       = "Donn\xc3\xa9""es",
      .descFR         = "Un exemple etiquete pour l'apprentissage : le lien "
                        "disque `..Chemin`, l'`..Étiquette` (nom de classe, "
                        "gravee a l'import) et l'`..Image`, MATERIALISEE "
                        "PARESSEUSEMENT — chargee depuis le disque au premier "
                        "acces de `..Image`, pas a l'import. Un fichier "
                        "absent ou illisible ne bloque donc pas la collection "
                        "qui la contient ; l'erreur ne tombe qu'a la lecture de "
                        "`..Image`. Element d'un `Dataset de ImageÉtiquetée`.",
      .descEN         = "One labeled example for training: the disk link "
                        "`..Path`, the `..Label` (class name, set at import) "
                        "and the `..Image`, MATERIALIZED LAZILY — loaded from "
                        "disk on first access of `..Image`, not at import. A "
                        "missing or unreadable file therefore does not block "
                        "the collection holding it; the error only surfaces "
                        "when `..Image` is read. Element of a `Dataset of "
                        "LabeledImage`.",
      .fields         = NULL,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "LabeledImage",
      .genre          = OWL_GENRE_F },   /* une ImageÉtiquetée */

    { .canonical      = "ImageAnnot\xc3\xa9""e",   /* ImageAnnotée */
      .canonicalAscii = "ImageAnnotee",
      .aliases        = ALIAS_image_annotee,
      .category       = "Donn\xc3\xa9""es",
      .descFR         = "Un exemple de detection : le lien disque `..Chemin`, "
                        "les `..Boîtes` (tableau de boites annotees, chacune "
                        "`[classe, cx, cy, largeur, hauteur]` en coordonnees "
                        "normalisees 0..1) et l'`..Image`, MATERIALISEE "
                        "PARESSEUSEMENT — chargee depuis le disque au premier "
                        "acces de `..Image`, pas a l'import. Une image sans "
                        "annotation est un exemple valide a 0 boite (fond). "
                        "Element d'un `Dataset de ImageAnnotée`.",
      .descEN         = "One detection example: the disk link `..Path`, the "
                        "`..Boxes` (array of annotated boxes, each "
                        "`[class, cx, cy, width, height]` in normalized 0..1 "
                        "coordinates) and the `..Image`, MATERIALIZED LAZILY — "
                        "loaded from disk on first access of `..Image`, not at "
                        "import. An image with no annotation is a valid example "
                        "with 0 boxes (background). Element of a `Dataset of "
                        "AnnotatedImage`.",
      .fields         = NULL,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "AnnotatedImage",
      .genre          = OWL_GENRE_F },   /* une ImageAnnotée */

    { .canonical      = "Dataset",
      .canonicalAscii = "Dataset",
      .aliases        = NULL,
      .category       = "Donn\xc3\xa9""es",
      .descFR         = "Collection ordonnee d'exemples etiquetes pour "
                        "l'entrainement d'un classifieur, declaree "
                        "`Dataset de ImageÉtiquetée`. Se charge via "
                        "`ChargeImagesÉtiquetéesDepuisDossier` (un dossier = "
                        "une classe) ou `ChargeImagesÉtiquetéesDepuisCSV` "
                        "(manifeste chemin;etiquette). Proprietes : `..Taille`, "
                        "`..NbClasses`, `..Classes` (tableau trie), `..Source`. "
                        "Indexation `ds[i]` -> une ImageÉtiquetée. Methodes : "
                        "`.Mélange()`, `.NombreLots(taille)`, "
                        "`.LotEntrées(taille, i)` (X : Tenseur B×C×H×W), "
                        "`.LotCibles(taille, i)` (Y : indices de classe) et "
                        "`.SousEnsemble(fractionDébut, fractionFin)` (tranche "
                        "[début, fin[ en un nouveau Dataset, pour un partage "
                        "train/test déterministe). Augmentation opt-in, "
                        "appliquée au vol par lot : `.RetournementHorizontalAléatoire()` "
                        "(miroir horizontal, probabilité 0,5 par image) et "
                        "`.RecadrageAléatoire(marge)` (zéro-padding de `marge` px "
                        "puis crop aléatoire à la taille d'origine). Le tirage "
                        "suit la graine (`InitHasard`), donc reproductible.",
      .descEN         = "Ordered collection of labeled examples for training a "
                        "classifier, declared `Dataset of LabeledImage`. Loaded "
                        "via `LoadLabeledImagesFromFolder` (one folder = one "
                        "class) or `LoadLabeledImagesFromCSV` (path;label "
                        "manifest). Properties: `..Count`, `..ClassCount`, "
                        "`..Classes` (sorted array), `..Source`. Indexing "
                        "`ds[i]` -> a LabeledImage. Methods: `.Shuffle()`, "
                        "`.BatchCount(size)`, `.BatchInputs(size, i)` (X: "
                        "B×C×H×W tensor), `.BatchTargets(size, i)` (Y: class "
                        "indices) and `.Subset(startFraction, endFraction)` "
                        "(the [start, end[ slice as a new Dataset, for a "
                        "deterministic train/test split). Opt-in augmentation, "
                        "applied on the fly per batch: `.RandomHorizontalFlip()` "
                        "(horizontal mirror, probability 0.5 per image) and "
                        "`.RandomCrop(margin)` (zero-padding of `margin` px then "
                        "random crop back to the original size). Draws follow the "
                        "seed (`InitRandom`), hence reproducible.",
      .fields         = NULL,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "Dataset",
      .genre          = OWL_GENRE_M },   /* un Dataset */
};

void stdlib_dataset_init(void) {
    owl_registry_register_types(DATASET_TYPES,
        (int)(sizeof(DATASET_TYPES) / sizeof(DATASET_TYPES[0])));

    /* Methodes du Dataset (receveur inclus dans l'arite). */
    owl_register_method(METHOD_TYPE_DATASET, "M\xc3\xa9lange",
                        fn_dataset_melange, 1);
    owl_register_method(METHOD_TYPE_DATASET, "Shuffle",
                        fn_dataset_melange, 1);
    owl_register_method(METHOD_TYPE_DATASET, "NombreLots",
                        fn_dataset_nombre_lots, 2);
    owl_register_method(METHOD_TYPE_DATASET, "BatchCount",
                        fn_dataset_nombre_lots, 2);
    owl_register_method(METHOD_TYPE_DATASET, "LotEntr\xc3\xa9""es",
                        fn_dataset_lot_entrees, 3);
    owl_register_method(METHOD_TYPE_DATASET, "BatchInputs",
                        fn_dataset_lot_entrees, 3);
    owl_register_method(METHOD_TYPE_DATASET, "LotCibles",
                        fn_dataset_lot_cibles, 3);
    owl_register_method(METHOD_TYPE_DATASET, "BatchTargets",
                        fn_dataset_lot_cibles, 3);
    owl_register_method(METHOD_TYPE_DATASET, "SousEnsemble",
                        fn_dataset_sous_ensemble, 3);
    owl_register_method(METHOD_TYPE_DATASET, "Subset",
                        fn_dataset_sous_ensemble, 3);
    owl_register_method(METHOD_TYPE_DATASET,
                        "RetournementHorizontalAl\xc3\xa9""atoire",
                        fn_dataset_flip_horizontal, 1);
    owl_register_method(METHOD_TYPE_DATASET,
                        "RandomHorizontalFlip",
                        fn_dataset_flip_horizontal, 1);
    owl_register_method(METHOD_TYPE_DATASET, "RecadrageAl\xc3\xa9""atoire",
                        fn_dataset_recadrage, 2);
    owl_register_method(METHOD_TYPE_DATASET, "RandomCrop",
                        fn_dataset_recadrage, 2);

    /* Native ImageVersTenseur / ImageToTensor. */
    static const OwlParam p_ivt[] = {
        { "image", "Image",
          "Image source (lecture seule).", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_ivt[] = {
        { 1, p_ivt, "Tenseur",
          "Tenseur C×H×W (3 canaux RGB), valeurs normalisees dans [0, 1]." },
        {0}
    };
    static const OwlParam p_dossier[] = {
        { "chemin", "chaine",
          "Dossier racine ; chaque sous-dossier est une classe.",
          NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_dossier[] = {
        { 1, p_dossier, "Dataset",
          "Dataset de ImageÉtiquetée (etiquette = nom du sous-dossier)." },
        {0}
    };
    static const OwlParam p_csv[] = {
        { "cheminCSV", "chaine",
          "Manifeste CSV « chemin<sep>etiquette », une ligne par exemple.",
          NULL, NULL, false },
        { "s\xc3\xa9parateur", "chaine",
          "Separateur de colonnes (\";\" par defaut).", NULL, NULL, true },
        {0}
    };
    static const OwlSignature sig_csv[] = {
        { 1, p_csv, "Dataset",
          "Dataset de ImageÉtiquetée (ordre des lignes preserve)." },
        { 2, p_csv, "Dataset",
          "Idem, avec un separateur de colonnes explicite." },
        {0}
    };
    static const OwlParam p_yolo[] = {
        { "chemin", "chaine",
          "Dossier « images + labels YOLO-txt » (un .txt par image, meme nom "
          "de base).", NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_yolo[] = {
        { 1, p_yolo, "Dataset",
          "Dataset de ImageAnnotée (boites lues dans chaque .txt)." },
        {0}
    };
    static const OwlNativeDef DEFS_DATASET[] = {
        { .canonical = "ImageVersTenseur",
          .canonicalAscii = "ImageVersTenseur",
          .nameEN     = "ImageToTensor",
          .category   = "Donn\xc3\xa9""es",
          .descFR     = "Convertit une Image en Tenseur C×H×W (channels-first) : "
                        "les valeurs de pixel sont normalisees dans [0, 1] "
                        "(÷255), sur 3 canaux RGB (le canal alpha, s'il existe, "
                        "est ignore). L'image source n'est pas modifiee. Brique "
                        "de `.LotEntrées` et conversion directe pour l'inference "
                        "sur une image seule. Image invalide -> exception.",
          .descEN     = "Converts an Image to a C×H×W (channels-first) Tensor: "
                        "pixel values are normalized to [0, 1] (÷255) over 3 RGB "
                        "channels (the alpha channel, if any, is dropped). The "
                        "source image is not modified. Building block of "
                        "`.BatchInputs` and a direct conversion for single-image "
                        "inference. Invalid image -> exception.",
          .signatures = sig_ivt,
          .impl       = fn_image_vers_tenseur,
          .flags      = 0 },
        { .canonical = "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisDossier",
          .canonicalAscii = "ChargeImagesEtiqueteesDepuisDossier",
          .nameEN     = "LoadLabeledImagesFromFolder",
          .category   = "Donn\xc3\xa9""es",
          .descFR     = "Charge une arborescence « un dossier = une classe » en "
                        "un Dataset de ImageÉtiquetée : chaque sous-dossier de "
                        "la racine devient une classe (etiquette = nom du "
                        "dossier), chaque fichier image qu'il contient un "
                        "exemple. Les classes sont enumerees par ordre "
                        "alphabetique (= index de classe). Dossier inexistant -> "
                        "exception.",
          .descEN     = "Loads a « one folder = one class » tree into a Dataset "
                        "of LabeledImage: each subfolder of the root becomes a "
                        "class (label = folder name), each image file it "
                        "contains an example. Classes are enumerated in "
                        "alphabetical order (= class index). Missing folder -> "
                        "exception.",
          .signatures = sig_dossier,
          .impl       = fn_charge_dossier,
          .flags      = 0 },
        { .canonical = "ChargeImages\xc3\x89tiquet\xc3\xa9""esDepuisCSV",
          .canonicalAscii = "ChargeImagesEtiqueteesDepuisCSV",
          .nameEN     = "LoadLabeledImagesFromCSV",
          .category   = "Donn\xc3\xa9""es",
          .descFR     = "Charge un manifeste CSV « chemin;etiquette » (une ligne "
                        "par exemple) en un Dataset de ImageÉtiquetée. Le "
                        "separateur de colonnes est \";\" par defaut, ou celui "
                        "passe en 2e argument. L'ordre des lignes est preserve "
                        "(pas de tri, pas de regroupement). Fichier inexistant "
                        "-> exception.",
          .descEN     = "Loads a « path;label » CSV manifest (one line per "
                        "example) into a Dataset of LabeledImage. The column "
                        "separator is \";\" by default, or the one passed as a "
                        "2nd argument. Line order is preserved (no sorting, no "
                        "grouping). Missing file -> exception.",
          .signatures = sig_csv,
          .impl       = fn_charge_csv,
          .flags      = 0 },
        { .canonical = "ChargeImagesAnnot\xc3\xa9""esDepuisYOLO",
          .canonicalAscii = "ChargeImagesAnnoteesDepuisYOLO",
          .nameEN     = "LoadAnnotatedImagesFromYOLO",
          .category   = "Donn\xc3\xa9""es",
          .descFR     = "Charge un dossier « images + labels YOLO-txt » en un "
                        "Dataset de ImageAnnotée : chaque fichier image devient "
                        "un exemple, ses boites sont lues dans le fichier .txt de "
                        "meme nom de base (une ligne par boite "
                        "`classe cx cy largeur hauteur`, coordonnees normalisees "
                        "0..1, conservees telles quelles). Les images sont "
                        "enumerees par ordre alphabetique. Une image sans .txt (ou "
                        ".txt vide) est un exemple valide a 0 boite. Dossier "
                        "inexistant -> exception.",
          .descEN     = "Loads an « images + YOLO-txt labels » folder into a "
                        "Dataset of AnnotatedImage: each image file becomes an "
                        "example, its boxes are read from the .txt file with the "
                        "same base name (one line per box "
                        "`class cx cy width height`, normalized 0..1 coordinates, "
                        "kept as-is). Images are enumerated in alphabetical order. "
                        "An image with no .txt (or an empty .txt) is a valid "
                        "example with 0 boxes. Missing folder -> exception.",
          .signatures = sig_yolo,
          .impl       = fn_charge_yolo,
          .flags      = 0 },
    };
    owl_registry_register_module(DEFS_DATASET,
        (int)(sizeof(DEFS_DATASET) / sizeof(DEFS_DATASET[0])));
}
