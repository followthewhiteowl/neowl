// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_detection.c — primitives de detection d'objets.
 *
 * Deux briques algorithmiques pures, socle d'un pipeline de detection :
 *
 *   - IoU(boiteA, boiteB) : intersection sur union de deux rectangles. Mesure
 *     de recouvrement dans [0,1] (0 = disjoints, 1 = identiques). Acronyme
 *     universel (Intersection over Union) -> canon partage FR=EN (aucune forme
 *     francaise pertinente, meme patron que Dataset).
 *
 *   - SuppressionNonMax / NonMaxSuppression(boites, scores, seuilIoU) : filtre
 *     un lot de boites notees en supprimant celles qui recouvrent trop une
 *     boite mieux notee deja conservee. Rend les indices (base 1) conserves,
 *     tries par score decroissant.
 *
 * Une boite = 4 reels [x, y, largeur, hauteur] (coin haut-gauche + taille).
 * L'IoU convertit en coins [x1,y1,x2,y2] en interne. `boites` est un tableau
 * de (tableau de 4 reels) ; `scores` un tableau de reels de meme longueur.
 *
 * Modules neutres : aucune math nouvelle, pas de dependance externe.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "object.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================
 * Lecture d'une boite [x, y, largeur, hauteur] depuis un tableau OWL
 * ================================================================ */

/* Extrait les 4 reels d'une boite dans out[4]. Rend true si `v` est un tableau
 * de 4 nombres, false sinon (boite mal formee). N'alloue rien. */
static bool det_read_box(Value v, double out[4]) {
    if (!IS_ARRAY(v)) return false;
    ObjArray* a = AS_ARRAY(v);
    if (a->elements.count != 4) return false;
    for (int i = 0; i < 4; i++) {
        Value e = a->elements.values[i];
        if (!IS_NUMBER(e)) return false;
        out[i] = AS_NUMBER(e);
    }
    return true;
}

/* Extrait les 5 reels d'une boite annotee [classe, cx, cy, largeur, hauteur]
 * dans out[5]. Rend true si `v` est un tableau de >= 5 nombres (les elements
 * au-dela du 5e sont ignores), false sinon. N'alloue rien. */
static bool det_read_box5(Value v, double out[5]) {
    if (!IS_ARRAY(v)) return false;
    ObjArray* a = AS_ARRAY(v);
    if (a->elements.count < 5) return false;
    for (int i = 0; i < 5; i++) {
        Value e = a->elements.values[i];
        if (!IS_NUMBER(e)) return false;
        out[i] = AS_NUMBER(e);
    }
    return true;
}

/* Extrait une prediction [classe, score, x, y, largeur, hauteur] (6 reels) :
 * classe dans *cls, score dans *score, boite [x,y,l,h] dans box[4]. Rend true si
 * `v` est un tableau d'exactement 6 nombres, false sinon. N'alloue rien. */
static bool map_read_pred6(Value v, double* cls, double* score, double box[4]) {
    if (!IS_ARRAY(v)) return false;
    ObjArray* a = AS_ARRAY(v);
    if (a->elements.count != 6) return false;
    for (int i = 0; i < 6; i++) {
        if (!IS_NUMBER(a->elements.values[i])) return false;
    }
    *cls   = AS_NUMBER(a->elements.values[0]);
    *score = AS_NUMBER(a->elements.values[1]);
    box[0] = AS_NUMBER(a->elements.values[2]);
    box[1] = AS_NUMBER(a->elements.values[3]);
    box[2] = AS_NUMBER(a->elements.values[4]);
    box[3] = AS_NUMBER(a->elements.values[5]);
    return true;
}

/* Extrait une verite terrain [classe, x, y, largeur, hauteur] (exactement 5
 * reels) : classe dans *cls, boite [x,y,l,h] dans box[4]. Rend true si `v` est un
 * tableau d'exactement 5 nombres, false sinon. N'alloue rien. */
static bool map_read_gt5(Value v, double* cls, double box[4]) {
    if (!IS_ARRAY(v)) return false;
    ObjArray* a = AS_ARRAY(v);
    if (a->elements.count != 5) return false;
    for (int i = 0; i < 5; i++) {
        if (!IS_NUMBER(a->elements.values[i])) return false;
    }
    *cls   = AS_NUMBER(a->elements.values[0]);
    box[0] = AS_NUMBER(a->elements.values[1]);
    box[1] = AS_NUMBER(a->elements.values[2]);
    box[2] = AS_NUMBER(a->elements.values[3]);
    box[3] = AS_NUMBER(a->elements.values[4]);
    return true;
}

/* IoU de deux boites [x,y,w,h] -> [x1,y1,x2,y2]. Union nulle (boites
 * degenerees) -> 0. Expose (owl_det_iou4) : partage avec IAModèle.Détecte. */
double owl_det_iou4(const double a[4], const double b[4]) {
    double ax1 = a[0], ay1 = a[1], ax2 = a[0] + a[2], ay2 = a[1] + a[3];
    double bx1 = b[0], by1 = b[1], bx2 = b[0] + b[2], by2 = b[1] + b[3];

    double ix = (ax2 < bx2 ? ax2 : bx2) - (ax1 > bx1 ? ax1 : bx1);
    double iy = (ay2 < by2 ? ay2 : by2) - (ay1 > by1 ? ay1 : by1);
    if (ix < 0.0) ix = 0.0;
    if (iy < 0.0) iy = 0.0;
    double inter = ix * iy;

    double areaA = a[2] * a[3];
    double areaB = b[2] * b[3];
    double uni = areaA + areaB - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

/* ================================================================
 * IoU(boiteA, boiteB)
 * ================================================================ */

static Value fn_iou(int argCount, Value* args) {
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "IoU",
                              "arg", "boiteB", NULL);
        return NIL_VAL;
    }
    double a[4], b[4];
    if (!det_read_box(args[0], a) || !det_read_box(args[1], b)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "IoU",
                              "arg", "boite",
                              "expected", "tableau de 4 reels [x, y, largeur, hauteur]",
                              "actual", "tableau mal forme", NULL);
        return NIL_VAL;
    }
    return REAL_VAL(owl_det_iou4(a, b));
}

/* ================================================================
 * SuppressionNonMax(boites, scores, seuilIoU)
 * ================================================================ */

/* Contexte de tri : indices ordonnes par score decroissant, tie-break par
 * indice croissant (tri deterministe et stable). */
static const double* g_det_scores = NULL;
static int det_cmp_desc(const void* pa, const void* pb) {
    int ia = *(const int*)pa;
    int ib = *(const int*)pb;
    double sa = g_det_scores[ia];
    double sb = g_det_scores[ib];
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return ia < ib ? -1 : 1;   /* egalite de score : indice croissant */
}

/* NMS gloutonne sur un sous-ensemble de candidats (cf. owl_stdlib.h). Le tri
 * par score et le filtre par recouvrement sont ici l'unique implementation,
 * partagee par SuppressionNonMax (candidats = tout le lot) et par
 * IAModèle.Détecte (candidats = une classe a la fois). */
int owl_det_nms_greedy(const double* boxbuf, const double* scbuf,
                       const int* cand, int ncand, double seuil, int* kept) {
    if (ncand <= 0) return 0;
    int* order = (int*)malloc((size_t)ncand * sizeof(int));
    if (order == NULL) return 0;
    for (int i = 0; i < ncand; i++) order[i] = cand[i];

    /* Tri par score decroissant (deterministe : tie-break index croissant). */
    g_det_scores = scbuf;
    qsort(order, (size_t)ncand, sizeof(int), det_cmp_desc);
    g_det_scores = NULL;

    /* Greedy : garder un candidat si son IoU avec TOUS les deja-gardes <= seuil. */
    int nkept = 0;
    for (int k = 0; k < ncand; k++) {
        int c = order[k];
        const double* cbox = &boxbuf[c * 4];
        bool suppress = false;
        for (int j = 0; j < nkept; j++) {
            const double* kbox = &boxbuf[kept[j] * 4];
            if (owl_det_iou4(cbox, kbox) > seuil) { suppress = true; break; }
        }
        if (!suppress) kept[nkept++] = c;
    }
    free(order);
    return nkept;
}

static Value fn_suppression_non_max(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SuppressionNonMax",
                              "arg", "seuilIoU", NULL);
        return NIL_VAL;
    }
    if (!IS_ARRAY(args[0]) || !IS_ARRAY(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SuppressionNonMax",
                              "arg", "boites/scores",
                              "expected", "deux tableaux (boites et scores)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SuppressionNonMax",
                              "arg", "seuilIoU",
                              "expected", "un reel (seuil de recouvrement)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    ObjArray* boxes  = AS_ARRAY(args[0]);
    ObjArray* scores = AS_ARRAY(args[1]);
    double seuil = AS_NUMBER(args[2]);

    int n = boxes->elements.count;
    if (scores->elements.count != n) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SuppressionNonMax",
                              "arg", "boites/scores",
                              "expected", "autant de scores que de boites",
                              "actual", "tailles incoherentes", NULL);
        return NIL_VAL;
    }

    /* Aucune allocation OWL au-dela du tableau resultat : on lit les boites/
     * scores dans des buffers C bruts, on delegue le tri + filtre au helper
     * partage, puis on remplit le tableau resultat en une passe (rooté). */
    double* boxbuf = NULL;   /* n*4 coords brutes */
    double* scbuf  = NULL;   /* n scores */
    int*    cand   = NULL;   /* candidats = tout le lot [0..n) */
    int*    kept   = NULL;   /* indices conserves (0-based, ordre de sortie) */
    if (n > 0) {
        boxbuf = (double*)malloc((size_t)n * 4 * sizeof(double));
        scbuf  = (double*)malloc((size_t)n * sizeof(double));
        cand   = (int*)malloc((size_t)n * sizeof(int));
        kept   = (int*)malloc((size_t)n * sizeof(int));
        if (!boxbuf || !scbuf || !cand || !kept) {
            free(boxbuf); free(scbuf); free(cand); free(kept);
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "SuppressionNonMax", NULL);
            return NIL_VAL;
        }
    }

    for (int i = 0; i < n; i++) {
        double box[4];
        if (!det_read_box(boxes->elements.values[i], box)) {
            free(boxbuf); free(scbuf); free(cand); free(kept);
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SuppressionNonMax",
                                  "arg", "boite",
                                  "expected", "tableau de 4 reels [x, y, largeur, hauteur]",
                                  "actual", "tableau mal forme", NULL);
            return NIL_VAL;
        }
        boxbuf[i * 4 + 0] = box[0];
        boxbuf[i * 4 + 1] = box[1];
        boxbuf[i * 4 + 2] = box[2];
        boxbuf[i * 4 + 3] = box[3];
        Value sv = scores->elements.values[i];
        if (!IS_NUMBER(sv)) {
            free(boxbuf); free(scbuf); free(cand); free(kept);
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SuppressionNonMax",
                                  "arg", "scores",
                                  "expected", "un tableau de reels",
                                  "actual", "valeur non numerique", NULL);
            return NIL_VAL;
        }
        scbuf[i] = AS_NUMBER(sv);
        cand[i]  = i;
    }

    int nkept = owl_det_nms_greedy(boxbuf, scbuf, cand, n, seuil, kept);

    /* Tableau resultat : indices base 1 dans l'ordre de sortie (score
     * decroissant). Rooté au sommet de la pile pendant les push (obj_array_push
     * peut grow -> GC). Les valeurs poussees sont des entiers (non-Obj), seul le
     * tableau a besoin d'etre rooté. */
    ObjArray* result = obj_new_array();
    vm_push(OBJ_VAL(result));
    for (int j = 0; j < nkept; j++) {
        obj_array_push(result, INTEGER_VAL(kept[j] + 1));
    }
    vm_pop();

    free(boxbuf); free(scbuf); free(cand); free(kept);
    return OBJ_VAL(result);
}

/* ================================================================
 * AssocieCiblesDetection(boites, hauteurGrille, largeurGrille, nbClasses)
 * ================================================================ */

/* Transforme un lot de boites annotees [classe, cx, cy, largeur, hauteur]
 * (normalisees 0..1) en un Tenseur cible [H*W, 4+1+K] aligne sur une tete de
 * detection par cellule, en ordre ligne-major (index = row*W + col). Pour
 * chaque boite, la cellule responsable est celle qui contient son centre :
 * col = plancher(cx*W), row = plancher(cy*H), bornes a [0,W-1]/[0,H-1]. Cette
 * cellule recoit la boite (canaux 0..3), objectness = 1 (canal 4) et la classe
 * en one-hot (canaux 5..5+K-1). Toute autre cellule reste a 0 (fond).
 *
 * Le centre (canaux 0..1) est stocke RELATIF a la cellule responsable :
 * cx_rel = cx*W - col, cy_rel = cy*H - row (dans [0,1[). C'est la convention
 * des tetes convolutives a poids partages (equivariance a la translation) :
 * une cible absolue n'est pas apprenable par une tete partagee. Largeur et
 * hauteur (canaux 2..3) restent une fraction de l'image entiere. Le decodage
 * reconstruit l'absolu : cx = (col + cx_rel)/W, cy = (row + cy_rel)/H. */
static Value fn_associe_cibles_detection(int argCount, Value* args) {
    if (argCount < 4) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "AssocieCiblesD\xc3\xa9tection",
                              "arg", "nbClasses", NULL);
        return NIL_VAL;
    }
    if (!IS_ARRAY(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AssocieCiblesD\xc3\xa9tection",
                              "arg", "boites",
                              "expected", "un tableau de boites",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[1]) || !IS_NUMBER(args[2]) || !IS_NUMBER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AssocieCiblesD\xc3\xa9tection",
                              "arg", "hauteurGrille/largeurGrille/nbClasses",
                              "expected", "trois entiers (H, W, K)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    int H = (int)AS_NUMBER(args[1]);
    int W = (int)AS_NUMBER(args[2]);
    int K = (int)AS_NUMBER(args[3]);
    if (H <= 0 || W <= 0 || K <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AssocieCiblesD\xc3\xa9tection",
                              "arg", "hauteurGrille/largeurGrille/nbClasses",
                              "expected", "trois entiers strictement positifs",
                              "actual", "valeur <= 0", NULL);
        return NIL_VAL;
    }

    ObjArray* boxes = AS_ARRAY(args[0]);
    int nBoxes = boxes->elements.count;
    int cols = 4 + 1 + K;   /* boite + objectness + one-hot classes */

    /* Valide toutes les boites AVANT d'allouer le Tenseur : ainsi une boite mal
     * formee sort en soft-error sans laisser d'objet a moitie construit. */
    for (int i = 0; i < nBoxes; i++) {
        double b[5];
        if (!det_read_box5(boxes->elements.values[i], b)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AssocieCiblesD\xc3\xa9tection",
                                  "arg", "boite",
                                  "expected", "tableau de >= 5 reels [classe, cx, cy, largeur, hauteur]",
                                  "actual", "tableau mal forme", NULL);
            return NIL_VAL;
        }
        int classe = (int)b[0];
        if (classe < 0 || classe >= K) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AssocieCiblesD\xc3\xa9tection",
                                  "arg", "classe",
                                  "expected", "un indice de classe dans [0, nbClasses-1]",
                                  "actual", "classe hors bornes", NULL);
            return NIL_VAL;
        }
    }

    /* Tenseur cible zero-rempli [H*W, cols]. obj_new_tensor_matrix zero-init la
     * donnee ; roote pendant le remplissage (aucune alloc OWL apres, mais on
     * garde la discipline canon). */
    ObjTensor* t = obj_new_tensor_matrix(H * W, cols);
    vm_push(OBJ_VAL(t));
    double* cells = tensor_cells(t->data);

    for (int i = 0; i < nBoxes; i++) {
        double b[5];
        det_read_box5(boxes->elements.values[i], b);   /* deja valide ci-dessus */
        double classe = b[0], cx = b[1], cy = b[2], l = b[3], h = b[4];

        int col = (int)(cx * W);
        int row = (int)(cy * H);
        if (col < 0) col = 0; else if (col > W - 1) col = W - 1;
        if (row < 0) row = 0; else if (row > H - 1) row = H - 1;

        int idx = row * W + col;   /* cellule responsable (0-based) */
        double* cell = &cells[(size_t)idx * cols];
        cell[0] = cx * W - col;        /* centre relatif a la cellule (dans [0,1[) */
        cell[1] = cy * H - row;
        cell[2] = l;
        cell[3] = h;
        cell[4] = 1.0;                 /* objectness (fait aussi office de masque) */
        cell[5 + (int)classe] = 1.0;   /* one-hot classe */
    }

    vm_pop();
    return OBJ_VAL(t);
}

/* ================================================================
 * PrécisionMoyenne(détections, véritésTerrain, seuilIoU)
 * ================================================================ */

/* mean Average Precision (mAP@seuilIoU) d'un detecteur d'objets.
 *
 * `détections` = tableau de [classe, score, x, y, largeur, hauteur] (6 reels,
 * boite = elements 2..5) ; `véritésTerrain` = tableau de [classe, x, y, largeur,
 * hauteur] (5 reels, boite = elements 1..4). Boites en coin, meme espace que IoU.
 *
 * Par classe presente dans les verites terrain : les predictions de la classe
 * sont triees par score decroissant et appariees de facon gloutonne a la
 * meilleure verite terrain non encore prise (IoU >= seuilIoU -> vrai positif,
 * sinon faux positif ; chaque GT appariee au plus une fois). Precision et rappel
 * cumules le long de la liste donnent une courbe dont l'aire (interpolation
 * "all-point" : AP = Σ (r_k − r_{k−1})·max{p : rappel >= r_k}) est l'AP de la
 * classe. mAP = moyenne des AP sur les classes ayant au moins une GT ; aucune
 * classe avec GT -> 0.0.
 *
 * Buffers de travail en tas C (aucune allocation OWL) : la native rend un reel. */
static Value fn_precision_moyenne(int argCount, Value* args) {
    if (argCount < 3) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "Pr\xc3\xa9""cisionMoyenne",
                              "arg", "seuilIoU", NULL);
        return NIL_VAL;
    }
    if (!IS_ARRAY(args[0]) || !IS_ARRAY(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""cisionMoyenne",
                              "arg", "d\xc3\xa9""tections/v\xc3\xa9""rit\xc3\xa9""sTerrain",
                              "expected", "deux tableaux (detections et verites terrain)",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    if (!IS_NUMBER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""cisionMoyenne",
                              "arg", "seuilIoU",
                              "expected", "un reel dans [0,1]",
                              "actual", "type non pris en charge", NULL);
        return NIL_VAL;
    }
    double seuil = AS_NUMBER(args[2]);
    if (seuil < 0.0 || seuil > 1.0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""cisionMoyenne",
                              "arg", "seuilIoU",
                              "expected", "un reel dans [0,1]",
                              "actual", "valeur hors bornes", NULL);
        return NIL_VAL;
    }

    ObjArray* detsArr = AS_ARRAY(args[0]);
    ObjArray* gtsArr  = AS_ARRAY(args[1]);
    int nDet = detsArr->elements.count;
    int nGT  = gtsArr->elements.count;

    double* detClass = NULL; double* detScore = NULL; double* detBox = NULL;
    double* gtClass  = NULL; double* gtBox = NULL;
    int*    clsDetIdx = NULL; double* prec = NULL; double* rec = NULL; double* sm = NULL;
    unsigned char* gtMatched = NULL; double* classesArr = NULL;

    if (nDet > 0) {
        detClass  = (double*)malloc((size_t)nDet * sizeof(double));
        detScore  = (double*)malloc((size_t)nDet * sizeof(double));
        detBox    = (double*)malloc((size_t)nDet * 4 * sizeof(double));
        clsDetIdx = (int*)malloc((size_t)nDet * sizeof(int));
        prec      = (double*)malloc((size_t)nDet * sizeof(double));
        rec       = (double*)malloc((size_t)nDet * sizeof(double));
        sm        = (double*)malloc((size_t)nDet * sizeof(double));
        if (!detClass || !detScore || !detBox || !clsDetIdx || !prec || !rec || !sm) goto oom;
    }
    if (nGT > 0) {
        gtClass    = (double*)malloc((size_t)nGT * sizeof(double));
        gtBox      = (double*)malloc((size_t)nGT * 4 * sizeof(double));
        gtMatched  = (unsigned char*)malloc((size_t)nGT * sizeof(unsigned char));
        classesArr = (double*)malloc((size_t)nGT * sizeof(double));
        if (!gtClass || !gtBox || !gtMatched || !classesArr) goto oom;
    }

    /* Lecture + validation des detections (6 reels chacune). */
    for (int i = 0; i < nDet; i++) {
        double cls, score, box[4];
        if (!map_read_pred6(detsArr->elements.values[i], &cls, &score, box)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""cisionMoyenne",
                "arg", "d\xc3\xa9""tection",
                "expected", "tableau de 6 reels [classe, score, x, y, largeur, hauteur]",
                "actual", "tableau mal forme", NULL);
            goto fail;
        }
        detClass[i] = cls; detScore[i] = score;
        detBox[i * 4 + 0] = box[0]; detBox[i * 4 + 1] = box[1];
        detBox[i * 4 + 2] = box[2]; detBox[i * 4 + 3] = box[3];
    }
    /* Lecture + validation des verites terrain (5 reels chacune). */
    for (int i = 0; i < nGT; i++) {
        double cls, box[4];
        if (!map_read_gt5(gtsArr->elements.values[i], &cls, box)) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Pr\xc3\xa9""cisionMoyenne",
                "arg", "v\xc3\xa9""rit\xc3\xa9"" terrain",
                "expected", "tableau de 5 reels [classe, x, y, largeur, hauteur]",
                "actual", "tableau mal forme", NULL);
            goto fail;
        }
        gtClass[i] = cls;
        gtBox[i * 4 + 0] = box[0]; gtBox[i * 4 + 1] = box[1];
        gtBox[i * 4 + 2] = box[2]; gtBox[i * 4 + 3] = box[3];
    }

    /* Classes distinctes presentes dans les verites terrain. */
    int nClasses = 0;
    for (int i = 0; i < nGT; i++) {
        double c = gtClass[i];
        bool seen = false;
        for (int j = 0; j < nClasses; j++) { if (classesArr[j] == c) { seen = true; break; } }
        if (!seen) classesArr[nClasses++] = c;
    }

    double sumAP = 0.0;
    int nClsWithGT = 0;
    for (int ci = 0; ci < nClasses; ci++) {
        double c = classesArr[ci];
        int nGTc = 0;
        for (int g = 0; g < nGT; g++) if (gtClass[g] == c) nGTc++;
        if (nGTc == 0) continue;   /* garde-fou : les classes viennent des GT */

        for (int g = 0; g < nGT; g++) gtMatched[g] = 0;

        /* Predictions de la classe, triees par score decroissant (tie-break
         * indice croissant via det_cmp_desc). */
        int m = 0;
        for (int d = 0; d < nDet; d++) if (detClass[d] == c) clsDetIdx[m++] = d;
        if (m > 0) {
            g_det_scores = detScore;
            qsort(clsDetIdx, (size_t)m, sizeof(int), det_cmp_desc);
            g_det_scores = NULL;
        }

        /* Appariement glouton + precision/rappel cumules. */
        int cumTP = 0, cumFP = 0;
        for (int k = 0; k < m; k++) {
            const double* dbox = &detBox[clsDetIdx[k] * 4];
            double bestIoU = -1.0;
            int bestG = -1;
            for (int g = 0; g < nGT; g++) {
                if (gtClass[g] != c || gtMatched[g]) continue;
                double iou = owl_det_iou4(dbox, &gtBox[g * 4]);
                if (iou > bestIoU) { bestIoU = iou; bestG = g; }
            }
            if (bestG >= 0 && bestIoU >= seuil) { gtMatched[bestG] = 1; cumTP++; }
            else cumFP++;
            prec[k] = (double)cumTP / (double)(cumTP + cumFP);
            rec[k]  = (double)cumTP / (double)nGTc;
        }

        /* Enveloppe de precision (max a droite) puis AP all-point. */
        double ap = 0.0;
        if (m > 0) {
            for (int k = m - 1; k >= 0; k--) {
                sm[k] = prec[k];
                if (k < m - 1 && sm[k + 1] > sm[k]) sm[k] = sm[k + 1];
            }
            double prevR = 0.0;
            int k = 0;
            while (k < m) {
                double r = rec[k];
                int startk = k;
                while (k < m && rec[k] == r) k++;   /* rappel identique -> meme rationnel exact */
                if (r > prevR) { ap += (r - prevR) * sm[startk]; prevR = r; }
            }
        }
        sumAP += ap;
        nClsWithGT++;
    }

    double mAP = nClsWithGT > 0 ? sumAP / (double)nClsWithGT : 0.0;

    free(detClass); free(detScore); free(detBox);
    free(gtClass); free(gtBox);
    free(clsDetIdx); free(prec); free(rec); free(sm);
    free(gtMatched); free(classesArr);
    return REAL_VAL(mAP);

oom:
    owl_emit_native_error(E_NATIVE_ARG_INVALID, "Pr\xc3\xa9""cisionMoyenne", NULL);
fail:
    free(detClass); free(detScore); free(detBox);
    free(gtClass); free(gtBox);
    free(clsDetIdx); free(prec); free(rec); free(sm);
    free(gtMatched); free(classesArr);
    return NIL_VAL;
}

/* ================================================================
 * Enregistrement registry
 * ================================================================ */

void stdlib_detection_init(void) {
    static const OwlParam p_iou[] = {
        { "boiteA", "tableau",
          "Premiere boite : tableau de 4 reels [x, y, largeur, hauteur].",
          NULL, NULL, false },
        { "boiteB", "tableau",
          "Seconde boite : tableau de 4 reels [x, y, largeur, hauteur].",
          NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_iou[] = {
        { 2, p_iou, "r\xc3\xa9""el",
          "Recouvrement dans [0,1] : 0 si les boites sont disjointes, 1 si "
          "elles sont identiques." },
        {0}
    };
    static const OwlParam p_nms[] = {
        { "boites", "tableau",
          "Tableau de boites, chaque boite etant un tableau de 4 reels "
          "[x, y, largeur, hauteur].",
          NULL, NULL, false },
        { "scores", "tableau",
          "Tableau de reels : le score de confiance de chaque boite (meme "
          "longueur que boites).",
          NULL, NULL, false },
        { "seuilIoU", "r\xc3\xa9""el",
          "Seuil de recouvrement : une boite est supprimee si son IoU avec une "
          "boite mieux notee deja conservee depasse ce seuil.",
          NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_nms[] = {
        { 3, p_nms, "tableau",
          "Indices (base 1) des boites conservees, tries par score decroissant." },
        {0}
    };
    static const OwlParam p_assoc[] = {
        { "bo\xc3\xae""tes", "tableau",
          "Tableau de boites annotees, chaque boite etant un tableau "
          "[classe, cx, cy, largeur, hauteur] normalise dans [0,1].",
          NULL, NULL, false },
        { "hauteurGrille", "entier",
          "Hauteur de la grille de cellules (H, nombre de lignes).",
          NULL, NULL, false },
        { "largeurGrille", "entier",
          "Largeur de la grille de cellules (W, nombre de colonnes).",
          NULL, NULL, false },
        { "nbClasses", "entier",
          "Nombre de classes (K) pour l'encodage one-hot.",
          NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_assoc[] = {
        { 4, p_assoc, "Tenseur",
          "Tenseur cible [H*W, 4+1+K] : par cellule, la boite (canaux 0..3), "
          "l'objectness (canal 4) et la classe one-hot (canaux 5..5+K-1)." },
        {0}
    };
    static const OwlParam p_map[] = {
        { "d\xc3\xa9""tections", "tableau",
          "Detections predites : chaque detection est un tableau de 6 reels "
          "[classe, score, x, y, largeur, hauteur] (boite en coin).",
          NULL, NULL, false },
        { "v\xc3\xa9""rit\xc3\xa9""sTerrain", "tableau",
          "Verites terrain : chaque annotation est un tableau de 5 reels "
          "[classe, x, y, largeur, hauteur] (meme espace de coordonnees).",
          NULL, NULL, false },
        { "seuilIoU", "r\xc3\xa9""el",
          "Seuil de recouvrement dans [0,1] : une detection est un vrai positif "
          "si son IoU avec une verite terrain non encore appariee l'atteint.",
          NULL, NULL, false },
        {0}
    };
    static const OwlSignature sig_map[] = {
        { 3, p_map, "r\xc3\xa9""el",
          "mAP dans [0,1] : moyenne des average precision par classe (aire sous "
          "la courbe precision-rappel) sur les classes ayant au moins une verite "
          "terrain. Rend 0 si aucune classe n'a de verite terrain." },
        {0}
    };

    static const OwlNativeDef DEFS_DETECTION[] = {
        { .canonical      = "IoU",
          .canonicalAscii = "IoU",
          .nameEN         = "IoU",   /* acronyme universel : canon partage FR=EN */
          .category       = "Intelligence artificielle",
          .descFR         = "Calcule l'intersection sur union (IoU) de deux "
                            "boites rectangulaires, chacune donnee par un tableau "
                            "de 4 reels [x, y, largeur, hauteur] (coin haut-gauche "
                            "puis taille). Rend un reel dans [0,1] : 0 quand les "
                            "boites ne se touchent pas, 1 quand elles coincident, "
                            "une valeur intermediaire selon leur recouvrement "
                            "(aire commune divisee par aire totale). Brique de base "
                            "pour comparer ou filtrer des detections.",
          .descEN         = "Computes the intersection over union (IoU) of two "
                            "rectangular boxes, each given as an array of 4 reals "
                            "[x, y, width, height] (top-left corner then size). "
                            "Returns a real in [0,1]: 0 when the boxes do not "
                            "overlap, 1 when they coincide, an intermediate value "
                            "according to their overlap (shared area divided by "
                            "total area). A building block to compare or filter "
                            "detections.",
          .signatures     = sig_iou,
          .impl           = fn_iou,
          .flags          = OWL_NAT_PURE },
        { .canonical      = "SuppressionNonMax",
          .canonicalAscii = "SuppressionNonMax",
          .nameEN         = "NonMaxSuppression",
          .category       = "Intelligence artificielle",
          .descFR         = "Filtre un lot de boites notees en supprimant les "
                            "doublons qui se chevauchent. Parcourt les boites de la "
                            "mieux notee a la moins bien notee ; conserve une boite "
                            "si son recouvrement (IoU) avec chaque boite deja "
                            "conservee reste sous seuilIoU, sinon la supprime. Rend "
                            "un tableau des indices (base 1) des boites conservees, "
                            "tries par score decroissant. Sur une liste vide rend un "
                            "tableau vide ; une seule boite est toujours conservee.",
          .descEN         = "Filters a batch of scored boxes by removing "
                            "overlapping duplicates. Walks the boxes from the "
                            "highest score to the lowest; keeps a box if its overlap "
                            "(IoU) with every already-kept box stays below seuilIoU, "
                            "otherwise drops it. Returns an array of the (1-based) "
                            "indices of the kept boxes, sorted by decreasing score. "
                            "An empty list yields an empty array; a single box is "
                            "always kept.",
          .signatures     = sig_nms,
          .impl           = fn_suppression_non_max,
          .flags          = 0 },
        { .canonical      = "AssocieCiblesD\xc3\xa9tection",
          .canonicalAscii = "AssocieCiblesDetection",
          .nameEN         = "AssignDetectionTargets",
          .category       = "Intelligence artificielle",
          .descFR         = "Transforme un lot de boites annotees en cibles "
                            "d'entrainement pour une tete de detection par cellule. "
                            "Chaque boite est un tableau [classe, cx, cy, largeur, "
                            "hauteur] normalise dans [0,1]. La sortie est un Tenseur "
                            "[H*W, 4+1+K] range en ordre ligne-major (index de "
                            "cellule = ligne*largeurGrille + colonne). Pour chaque "
                            "boite, la cellule responsable est celle qui contient son "
                            "centre : colonne = plancher(cx*largeurGrille), ligne = "
                            "plancher(cy*hauteurGrille), bornees a la derniere "
                            "cellule. Cette cellule recoit la boite (canaux 0 a 3), "
                            "un objectness a 1 (canal 4, qui sert aussi de masque "
                            "positif) et la classe en one-hot (canaux 5 a 5+K-1). Le "
                            "centre (canaux 0 et 1) est stocke en coordonnees "
                            "relatives a la cellule : cx*largeurGrille - colonne et "
                            "cy*hauteurGrille - ligne, dans [0,1[ ; la largeur et la "
                            "hauteur (canaux 2 et 3) restent une fraction de l'image "
                            "entiere. Le decodage reconstruit le centre absolu par "
                            "(colonne + cx_relatif)/largeurGrille et (ligne + "
                            "cy_relatif)/hauteurGrille. Les cellules de fond restent "
                            "a 0. Un lot vide rend un Tenseur entierement a 0.",
          .descEN         = "Turns a batch of annotated boxes into training targets "
                            "for a per-cell detection head. Each box is an array "
                            "[class, cx, cy, width, height] normalized in [0,1]. The "
                            "output is a [H*W, 4+1+K] tensor laid out in row-major "
                            "order (cell index = row*gridWidth + column). For each "
                            "box, the responsible cell is the one containing its "
                            "center: column = floor(cx*gridWidth), row = "
                            "floor(cy*gridHeight), clamped to the last cell. That cell "
                            "receives the box (channels 0 to 3), an objectness of 1 "
                            "(channel 4, which also acts as a positive mask) and the "
                            "one-hot class (channels 5 to 5+K-1). The center (channels "
                            "0 and 1) is stored relative to the cell: cx*gridWidth - "
                            "column and cy*gridHeight - row, in [0,1[; width and height "
                            "(channels 2 and 3) stay a fraction of the whole image. "
                            "Decoding rebuilds the absolute center via (column + "
                            "cx_rel)/gridWidth and (row + cy_rel)/gridHeight. "
                            "Background cells stay at 0. An empty batch yields an "
                            "all-zero tensor.",
          .signatures     = sig_assoc,
          .impl           = fn_associe_cibles_detection,
          .flags          = OWL_NAT_PURE },
        { .canonical      = "Pr\xc3\xa9""cisionMoyenne",
          .canonicalAscii = "PrecisionMoyenne",
          .nameEN         = "MeanAveragePrecision",
          .category       = "Intelligence artificielle",
          .descFR         = "Calcule la precision moyenne (mAP) d'un detecteur "
                            "d'objets a un seuil de recouvrement donne. Prend les "
                            "detections predites (tableau de [classe, score, x, y, "
                            "largeur, hauteur]) et les verites terrain (tableau de "
                            "[classe, x, y, largeur, hauteur]), boites en coin dans "
                            "le meme espace. Pour chaque classe presente dans les "
                            "verites terrain, les predictions sont triees par score "
                            "decroissant et appariees de facon gloutonne a la "
                            "meilleure verite terrain encore libre dont l'IoU "
                            "atteint seuilIoU (vrai positif), les autres etant des "
                            "faux positifs ; chaque verite terrain est appariee au "
                            "plus une fois. L'aire sous la courbe precision-rappel "
                            "(interpolation sur tous les points) donne l'average "
                            "precision de la classe, et la moyenne sur les classes "
                            "ayant au moins une verite terrain donne la mAP. Rend un "
                            "reel dans [0,1] ; 0 si aucune classe n'a de verite "
                            "terrain.",
          .descEN         = "Computes the mean average precision (mAP) of an object "
                            "detector at a given overlap threshold. Takes the "
                            "predicted detections (array of [class, score, x, y, "
                            "width, height]) and the ground truths (array of "
                            "[class, x, y, width, height]), corner boxes in the same "
                            "space. For each class present in the ground truths, "
                            "predictions are sorted by decreasing score and greedily "
                            "matched to the best still-free ground truth whose IoU "
                            "reaches iouThreshold (true positive), the others being "
                            "false positives; each ground truth is matched at most "
                            "once. The area under the precision-recall curve "
                            "(all-point interpolation) yields the class average "
                            "precision, and the mean over classes having at least "
                            "one ground truth yields the mAP. Returns a real in "
                            "[0,1]; 0 when no class has any ground truth.",
          .signatures     = sig_map,
          .impl           = fn_precision_moyenne,
          .flags          = OWL_NAT_PURE },
    };
    owl_registry_register_module(DEFS_DETECTION,
        (int)(sizeof(DEFS_DETECTION) / sizeof(DEFS_DETECTION[0])));
}
