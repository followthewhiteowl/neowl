// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_image.c — Type Image natif.
 *
 * Natives couvertes : type Image, dChargeImage, dSauveImage,
 *                      PNG, JPEG, BMP.
 *
 * Lots :
 *   I.0  fondations : type Image au registry, 7 propriétés.
 *   I.1  load/save raster + propriété "Image = path" (sucre canon)
 *        + dCréeImage (factory programmatique).
 *   I.2+ pixel access, dessin, transformations, effets, SVG, HEIC, GUI.
 *
 * Skips explicites en I.1 (cf ROADMAP §3ter) :
 *   - Buffer (rubrique mémo binaire) en entrée — V2.
 *   - PSD/TGA en sauvegarde — stb ne les écrit pas, à voir.
 *   - GIF/ICO/TIFF write — V2 (stb n'écrit pas, libgif/libtiff).
 *   - imgConvertir : effet visible uniquement sur SVG/PDF (Lot I.6+).
 *     En I.1 la constante existe et est acceptée mais sans effet.
 *   - Couleur de transparence dSauveImagePNG : V2.
 *   - Marqueur Exif dSauveImageJPEG : V2.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "owl_image.h"
#include "vm.h"
#include "gc.h"  /*  : gc_mark_object pour stdlib_image_mark_roots */

#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>

/* Sentinelles img* / dessin* / Fond* centralisées dans sentinels.h. */
#include "sentinels.h"

/* Cablage allowedOptions sur le param attributs de dPolice — enum
 * declare dans ui_catalog.c (partage avec iCreePolice cote Etats/PDF). */
extern const OwlEnum E_IStyle;

/* Constantes — couleurs par défaut */
#define IMAGE_DEFAULT_FILL_COLOR_BGR  0x00FFFFFF  /* Blanc opaque */

/* Forward declaration : helpers définis dans la section drawing state
 * (Lot I.3a) mais utilisés plus tôt par les natives pixel access (I.2). */
static OwlImage* resolve_target_image(int argCount, Value* args, int* outArgShift);

/* ================================================================
 * Helpers : champs ObjInstance Image
 * ================================================================ */

/*  : image_set_int/real/bool supprimes — call-sites migres vers
 * inst_field_set_* (object.c). image_set_handle conserve comme wrapper
 * lisible (cast intptr_t encapsule). */

static int64_t image_get_int(const ObjInstance* inst, const char* name, int64_t dflt) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    Value v;
    if (!table_get((Table*)&inst->fields, k, &v)) return dflt;
    if (IS_INTEGER(v)) return AS_INTEGER(v);
    return dflt;
}

/* Stockage / récupération du pointeur natif OwlImage* dans _ptr,
 * intpr_t dans un INTEGER_VAL (cf pattern xml_make_instance). */
static void image_set_handle(ObjInstance* inst, OwlImage* img) {
    inst_field_set_int(inst, "_ptr", (int64_t)(intptr_t)img);
}

static OwlImage* image_get_handle(const ObjInstance* inst) {
    int64_t v = image_get_int(inst, "_ptr", 0);
    return (OwlImage*)(intptr_t)v;
}

/* Synchronise les champs publics depuis l'OwlImage* sous-jacent. */
static void image_sync_public(ObjInstance* inst, OwlImage* img) {
    inst_field_set_int (inst, "Largeur",     owl_image_width(img));
    inst_field_set_int (inst, "Hauteur",     owl_image_height(img));
    inst_field_set_bool(inst, "Valide",      owl_image_valid(img));
    inst_field_set_bool(inst, "AvecAlpha",   owl_image_has_alpha(img));
    inst_field_set_int (inst, "BitParPixel", owl_image_bits_per_pixel(img));
}

/* Crée une nouvelle ObjInstance Image avec un OwlImage* préexistant.
 * S'occupe du root GC pendant l'allocation.
 *  : non-static — consomme par stdlib_ui.c (natives dCopieImage*
 * migrees Lot 2). Le prototype est declare extern dans stdlib_ui.c. */
ObjInstance* image_make_instance(OwlImage* img) {
    ObjString* name = obj_copy_string("Image", 5);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_push(OBJ_VAL(inst));

    /* Propage la couleur de remplissage et l'échelle de dessin de
     * l'OwlImage* (qui peut avoir été modifiées avant cet appel). */
    image_set_handle(inst, img);
    inst_field_set_int(inst, "CouleurRemplissage",
                       (int64_t)(owl_image_fill_color(img) & 0xFFFFFFu));
    inst_field_set_value(inst, "EchelleDessin",
                         REAL_VAL(owl_image_draw_scale(img)));
    image_sync_public(inst, img);

    vm_pop(); /* inst */
    vm_pop(); /* name */
    return inst;
}

/* Renvoie l'OwlImage* d'une Value qui doit être un ObjInstance Image,
 * NULL sinon. */
static OwlImage* value_to_image_handle(Value v) {
    if (!IS_INSTANCE(v)) return NULL;
    ObjInstance* inst = AS_INSTANCE(v);
    if (!inst->structName || strcmp(inst->structName->chars, "Image") != 0)
        return NULL;
    return image_get_handle(inst);
}

/*  — accesseur public de l'OwlImage* d'une Value Image. Consomme par
 * stdlib_dataset.c (ImageVersTenseur : pont pixels->tenseur sans copie OWL).
 * Meme convention d'extern-only que image_make_instance. NULL si v n'est pas
 * une Image valide. */
OwlImage* owl_image_handle_from_value(Value v) {
    return value_to_image_handle(v);
}

/* ================================================================
 * Helpers d'extension de fichier (case-insensible).
 * ================================================================ */

static bool path_ends_with_ci(const char* path, const char* ext) {
    if (!path || !ext) return false;
    size_t lp = strlen(path), le = strlen(ext);
    if (le > lp) return false;
    const char* tail = path + (lp - le);
    for (size_t i = 0; i < le; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)ext[i]))
            return false;
    }
    return true;
}

/* ================================================================
 * dChargeImage(<Image> [, <Options> [, <Largeur> [, <Hauteur>]]])
 *
 * Charge un fichier raster via stb_image et
 * retourne une Variable de type Image.
 *
 * En I.1 :
 *   - chemin string accepté (canon : "rep/exec" ou absolu)
 *   - <Options> : imgDéfaut (default) / imgConvertir (no-op en raster)
 *   - <Largeur>/<Hauteur> : ignorés en raster (effet sur SVG/PDF en I.6)
 *
 * Si le chargement échoue, l'Image retournée est invalide (Valide=Faux)
 * et ErreurDétectée est posée.
 * ================================================================ */
static Value fn_dChargeImage(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dChargeImage",
                              "native", "dChargeImage",
                              "arg", "1",
                              "expected", "chaine",
                              "actual", "autre",
                              NULL);
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }
    const char* path = AS_STRING(args[0])->chars;

    /* Lecture des dimensions optionnelles (utilisées pour SVG). */
    int target_w = 0, target_h = 0;
    if (argCount > 2 && IS_INTEGER(args[2])) target_w = (int)AS_INTEGER(args[2]);
    if (argCount > 3 && IS_INTEGER(args[3])) target_h = (int)AS_INTEGER(args[3]);

    OwlImage* img = NULL;
    if (path_ends_with_ci(path, ".svg")) {
        img = owl_image_load_from_svg_file(path, target_w, target_h);
    } else if (path_ends_with_ci(path, ".heic") || path_ends_with_ci(path, ".heif")) {
        owl_emit_native_error(E_NATIVE_IMAGE_UNSUPPORTED_FORMAT, "dChargeImage",
                              "native", "dChargeImage",
                              "format", "HEIC/HEIF",
                              "path", path,
                              NULL);
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    } else {
        img = owl_image_load_from_file(path);
    }
    if (!img) {
        owl_emit_native_error(E_NATIVE_IMAGE_LOAD_FAILURE, "dChargeImage",
                              "native", "dChargeImage",
                              "path", path,
                              "detail", "fichier absent ou format non reconnu",
                              NULL);
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }
    owl_clear_error();
    return OBJ_VAL(image_make_instance(img));
}

/* ================================================================
 * dCréeImage(largeur, hauteur [, couleur])
 *
 * Extension OWL : factory programmatique pour créer une image vierge
 * remplie de la couleur indiquée. Équivalent au pattern canon classique
 * (déclarer Image puis poser Largeur/Hauteur) mais qui alloue
 * effectivement le buffer pixels.
 *
 * Couleur par défaut : Blanc (0xFFFFFF).
 * ================================================================ */
static Value fn_dCreeImage(int argCount, Value* args) {
    if (argCount < 2 || !IS_INTEGER(args[0]) || !IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dCreeImage",
                              "native", "dCreeImage", NULL);
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }
    int64_t w = AS_INTEGER(args[0]);
    int64_t h = AS_INTEGER(args[1]);
    if (w <= 0 || h <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dCreeImage",
                              "native", "dCreeImage",
                              "arg", "largeur/hauteur",
                              NULL);
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }

    /* Couleur en BGR 24-bit + alpha forcé opaque. */
    OwlImageColor color = 0xFF000000u | IMAGE_DEFAULT_FILL_COLOR_BGR;
    if (argCount >= 3 && IS_INTEGER(args[2])) {
        int64_t c = AS_INTEGER(args[2]);
        color = 0xFF000000u | ((OwlImageColor)c & 0x00FFFFFFu);
    }

    OwlImage* img = owl_image_new((int)w, (int)h, color);
    owl_clear_error();
    return OBJ_VAL(image_make_instance(img));
}

/* ================================================================
 * dSauveImagePNG(<Image>, <chemin>)
 * dSauveImageJPEG(<Image>, <chemin> [, <qualité>])
 * dSauveImageBMP(<Image>, <chemin>)
 * dSauveImage(<Image>, <chemin>)  — auto-detect par extension
 *
 * Retournent Vrai si succès, Faux + ErreurDétectée sinon.
 * ================================================================ */

static Value fn_dSauveImagePNG(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dSauveImagePNG",
                              "native", "dSauveImagePNG", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = value_to_image_handle(args[0]);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dSauveImagePNG",
                              "native", "dSauveImagePNG", NULL);
        return BOOL_VAL(false);
    }
    const char* path = AS_STRING(args[1])->chars;

    bool ok;
    if (argCount >= 3 && IS_INTEGER(args[2])) {
        /* Couleur de transparence : tous les pixels matchant cette
         * couleur (R,G,B) auront alpha=0 dans le fichier produit. */
        OwlImageColor transparent = (OwlImageColor)(AS_INTEGER(args[2]) & 0x00FFFFFFu);
        ok = owl_image_save_png_with_transparent(img, path, transparent);
    } else {
        ok = owl_image_save_png(img, path);
    }

    if (!ok) {
        owl_emit_native_error(E_NATIVE_IMAGE_SAVE_FAILURE, "dSauveImagePNG",
                              "native", "dSauveImagePNG",
                              "path", path, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_dSauveImageJPEG(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dSauveImageJPEG",
                              "native", "dSauveImageJPEG", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = value_to_image_handle(args[0]);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dSauveImageJPEG",
                              "native", "dSauveImageJPEG", NULL);
        return BOOL_VAL(false);
    }
    const char* path = AS_STRING(args[1])->chars;
    int quality = 80;  /* défaut canon */
    if (argCount >= 3 && IS_INTEGER(args[2])) {
        quality = (int)AS_INTEGER(args[2]);
    }
    bool ok = owl_image_save_jpeg(img, path, quality);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_IMAGE_SAVE_FAILURE, "dSauveImageJPEG",
                              "native", "dSauveImageJPEG",
                              "path", path, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_dSauveImageBMP(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dSauveImageBMP",
                              "native", "dSauveImageBMP", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = value_to_image_handle(args[0]);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dSauveImageBMP",
                              "native", "dSauveImageBMP", NULL);
        return BOOL_VAL(false);
    }
    const char* path = AS_STRING(args[1])->chars;
    bool ok = owl_image_save_bmp(img, path);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_IMAGE_SAVE_FAILURE, "dSauveImageBMP",
                              "native", "dSauveImageBMP",
                              "path", path, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_dSauveImage(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dSauveImage",
                              "native", "dSauveImage", NULL);
        return BOOL_VAL(false);
    }
    const char* path = AS_STRING(args[1])->chars;
    /* Dispatch sur extension. */
    if (path_ends_with_ci(path, ".png")) {
        return fn_dSauveImagePNG(argCount, args);
    }
    if (path_ends_with_ci(path, ".jpg") || path_ends_with_ci(path, ".jpeg")) {
        return fn_dSauveImageJPEG(argCount, args);
    }
    if (path_ends_with_ci(path, ".bmp")) {
        return fn_dSauveImageBMP(argCount, args);
    }
    /* Format inconnu : fallback PNG (cohérent avec la propriété Image
     * en lecture qui retourne du PNG par défaut). */
    return fn_dSauveImagePNG(argCount, args);
}

/* ================================================================
 * Pixel access (Lot I.2)
 *
 * Natives couvertes :
 *   - dPixelCouleur  — lecture couleur d'un pixel
 *   - dPixelOpacité  — lecture canal alpha
 *   - dPoint         — écriture d'un pixel coloré
 *
 * Encoding :
 *   - Couleur retournée = entier canon Couleur (BGR sur 24 bits :
 *     R + V*256 + B*65536), cohérent avec stdlib_color.c (RVB(), TSL()…).
 *   - Opacité 0..255 (0 = transparent, 255 = opaque), conforme canon.
 *   - Coordonnées (0,0) = coin haut gauche.
 *
 * Hors limites ou Image invalide : silencieux (lecture renvoie 0,
 * écriture est no-op). Pas d'erreur posée — cohérent avec le canon
 * qui clamp silencieusement.
 *
 * Skip explicite I.2 : le 1er argument optionnel `<Image>` (qui prend
 * l'image courante de dDébutDessin si omis) arrive en I.3 quand on aura
 * dDébutDessin/dFinDessin. En I.2 l'image est obligatoire en arg 1.
 * ================================================================ */

/* dPixelCouleur([<Image>,] <x>, <y>) → entier (couleur BGR).
 *
 * Forme courte (sans <Image>) : utilise l'image courante posée par
 * dDébutDessin. Renvoie 0 si aucune image n'est résolue, hors limites
 * ou invalide. */
static Value fn_dPixelCouleur(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) return INTEGER_VAL(0);
    if (argCount < shift + 2 ||
        !IS_INTEGER(args[shift]) || !IS_INTEGER(args[shift + 1])) {
        return INTEGER_VAL(0);
    }
    int x = (int)AS_INTEGER(args[shift]);
    int y = (int)AS_INTEGER(args[shift + 1]);
    OwlImageColor c = owl_image_get_pixel(img, x, y);
    /* Masquer le canal alpha pour ne renvoyer que la couleur BGR sur
     * 24 bits, conforme à l'encoding canon Couleur. */
    return INTEGER_VAL((int64_t)(c & 0x00FFFFFFu));
}

/* dPixelOpacité([<Image>,] <x>, <y>) → entier (alpha 0..255). */
static Value fn_dPixelOpacite(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) return INTEGER_VAL(0);
    if (argCount < shift + 2 ||
        !IS_INTEGER(args[shift]) || !IS_INTEGER(args[shift + 1])) {
        return INTEGER_VAL(0);
    }
    int x = (int)AS_INTEGER(args[shift]);
    int y = (int)AS_INTEGER(args[shift + 1]);
    OwlImageColor c = owl_image_get_pixel(img, x, y);
    return INTEGER_VAL((int64_t)((c >> 24) & 0xFFu));
}

/* dPoint([<Image>,] <x>, <y> [, <couleur> [, <opacité>]]).
 *
 * Si la couleur est omise, le stylo courant de l'image cible (posé par
 * dStylo) est utilisé. Si l'opacité est omise et qu'une couleur explicite
 * est passée, l'opacité courante du stylo est héritée (ou 255 par défaut
 * si aucun dStylo précédent). Opacité = 0 → pixel inchangé (no-op).
 *
 *  : verrou pen_set quand ni couleur ni stylo n'ont ete poses.
 *  : opacite 0 = no-op (n'ecrase plus le pixel avec alpha=0).
 *  : opacite explicite heritee depuis le stylo si argument omis. */
static Value fn_dPoint(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dPoint",
                              "native", "dPoint", NULL);
        return NIL_VAL;
    }
    if (argCount < shift + 2 ||
        !IS_INTEGER(args[shift]) || !IS_INTEGER(args[shift + 1])) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dPoint",
                              "native", "dPoint",
                              "arg", "x/y", NULL);
        return NIL_VAL;
    }
    int x = (int)AS_INTEGER(args[shift]);
    int y = (int)AS_INTEGER(args[shift + 1]);

    bool hasExplicitColor   = (argCount > shift + 2 && IS_INTEGER(args[shift + 2]));
    bool hasExplicitOpacity = (argCount > shift + 3 && IS_INTEGER(args[shift + 3]));
    int64_t couleurBGR;
    int alpha;

    if (hasExplicitColor) {
        couleurBGR = AS_INTEGER(args[shift + 2]) & 0x00FFFFFFu;
        if (hasExplicitOpacity) {
            int64_t a = AS_INTEGER(args[shift + 3]);
            if (a < 0)   a = 0;
            if (a > 255) a = 255;
            alpha = (int)a;
        } else if (owl_image_pen_is_set(img)) {
            /* Herite l'opacite du stylo courant si pose, sinon opaque. */
            alpha = owl_image_pen_opacity(img);
        } else {
            alpha = 255;
        }
    } else {
        /* Aucune couleur explicite : requiert un stylo pose au prealable. */
        if (!owl_image_pen_is_set(img)) {
            owl_emit_native_error(E_NATIVE_ARG_MISSING, "dPoint",
                                  "native", "dPoint",
                                  "arg", "couleur (ou stylo via dStylo)", NULL);
            return NIL_VAL;
        }
        OwlImageColor pen = owl_image_pen_color(img);
        couleurBGR = pen & 0x00FFFFFFu;
        alpha = owl_image_pen_opacity(img);
    }

    /* Opacite 0 = no-op : on n'ecrase pas le pixel courant. */
    if (alpha == 0) {
        owl_clear_error();
        return NIL_VAL;
    }
    OwlImageColor packed = ((OwlImageColor)alpha << 24) | (OwlImageColor)couleurBGR;
    if (alpha == 255) {
        /* Opaque : ecrit directement le pixel (compat dPoint existant). */
        owl_image_set_pixel(img, x, y, packed);
    } else {
        /* Semi-transparent : alpha-blend pour melanger avec le fond
         * tout en conservant l'opacite source dans le canal alpha sortie
         * (cf. plot_blend, comportement canon dPoint opacite N). */
        owl_image_blend_pixel(img, x, y, packed);
    }
    owl_clear_error();
    return NIL_VAL;
}

/* ================================================================
 * Drawing state machine (Lot I.3a)
 *
 * Natives couvertes :
 *   - dDébutDessin
 *   - dFinDessin
 *   - dStylo
 *   - dFond
 *   - dEfface
 *
 * Pattern canon : state machine globale. dDébutDessin pose une
 * image courante, les fonctions d* qui suivent peuvent omettre le 1er
 * arg <Image> et utiliseront cette image. dFinDessin libère.
 *
 * Skips explicites :
 *   - Hachures (FondHachuré + types 0..5) : exposées mais non rendues
 *     (rendu effectif via dRectangle/dPolygone).
 *   - DC retour Windows : OWL retourne juste true/false (canon Java/var).
 * ================================================================ */

/* Synchronise les champs writables (CouleurRemplissage, EchelleDessin)
 * depuis inst->fields vers l'OwlImage* sous-jacent. À appeler chaque fois
 * qu'on récupère un handle natif depuis une instance, pour respecter les
 * écritures utilisateur du type `img..CouleurRemplissage = X`. */
static void image_sync_inst_to_handle(ObjInstance* inst, OwlImage* img) {
    if (!inst || !img) return;
    /* CouleurRemplissage stockée en BGR 24-bit côté inst, en 0xAABBGGRR
     * côté OwlImage (alpha forcé opaque pour la couleur de remplissage). */
    int64_t bgr = image_get_int(inst, "CouleurRemplissage",
                                (int64_t)IMAGE_DEFAULT_FILL_COLOR_BGR);
    OwlImageColor packed = 0xFF000000u | ((OwlImageColor)bgr & 0x00FFFFFFu);
    owl_image_set_fill_color(img, packed);
    /* EchelleDessin : double, lu via field. */
    ObjString* k = obj_copy_string("EchelleDessin", 13);
    Value v;
    if (table_get(&inst->fields, k, &v)) {
        double s = IS_REAL(v)    ? AS_REAL(v)
                 : IS_INTEGER(v) ? (double)AS_INTEGER(v)
                                 : 1.0;
        owl_image_set_draw_scale(img, s);
    }
}

/* Instance Image courante posée par dDébutDessin (en plus du OwlImage*
 * stocké côté C dans owl_image_current()). Permet de resynchroniser les
 * champs inst (CouleurRemplissage, EchelleDessin) vers le OwlImage*
 * avant chaque opération qui les consomme — l'utilisateur peut modifier
 * ces champs entre dDébutDessin et la fonction de dessin. */
static ObjInstance* g_current_instance = NULL;

/* Résout l'image cible : explicite (1er arg ObjInstance Image valide)
 * ou courante (state-machine). Renvoie NULL si rien d'utilisable.
 *
 * Dans tous les cas, sync inst→handle avant de retourner pour respecter
 * les modifications utilisateur de CouleurRemplissage / EchelleDessin.
 *
 * Si outArgShift != NULL, *outArgShift est mis à 1 ssi args[0] a été
 * consommé comme image (et les arguments suivants commencent à index 1
 * au lieu de 0). */
static OwlImage* resolve_target_image(int argCount, Value* args, int* outArgShift) {
    if (outArgShift) *outArgShift = 0;
    if (argCount > 0 && IS_INSTANCE(args[0])) {
        ObjInstance* inst = AS_INSTANCE(args[0]);
        if (inst->structName && strcmp(inst->structName->chars, "Image") == 0) {
            if (outArgShift) *outArgShift = 1;
            OwlImage* img = image_get_handle(inst);
            image_sync_inst_to_handle(inst, img);
            return img;
        }
    }
    /* Fallback state-machine : on resync depuis l'inst courante stockée
     * par dDébutDessin pour répercuter les modifs intermédiaires. */
    OwlImage* img = owl_image_current();
    if (g_current_instance && img) {
        image_sync_inst_to_handle(g_current_instance, img);
    }
    return img;
}

/* dDébutDessin(<Image> [, <Options>])
 *
 * Pose l'image courante pour les fonctions d* suivantes. Si options
 * inclut OWL_DESSIN_EFFACER (défaut), l'image est effacée avec sa
 * couleur de remplissage avant le dessin. */
static Value fn_dDebutDessin(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dDebutDessin",
                              "native", "dDebutDessin", "arg", "1", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_INSTANCE(args[0]) || !stdlib_image_is_instance(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dDebutDessin",
                              "native", "dDebutDessin", "arg", "1",
                              "expected", "Image", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);  /* propage CouleurRemplissage/EchelleDessin */
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dDebutDessin",
                              "native", "dDebutDessin", NULL);
        return BOOL_VAL(false);
    }

    /* Options : par défaut OWL_DESSIN_EFFACER (efface l'image). */
    int64_t opts = OWL_DESSIN_EFFACER;
    if (argCount >= 2 && IS_INTEGER(args[1])) opts = AS_INTEGER(args[1]);

    bool sansEffacer = (opts == OWL_DESSIN_SANS_EFFACER);
    if (!sansEffacer) {
        /* Effacer avec la couleur de remplissage. */
        OwlImageColor fill = owl_image_fill_color(img);
        owl_image_clear(img, fill);
    }
    /* dAvecOpacité : actuellement no-op (alpha géré par les drivers de
     * dessin selon le format de l'image cible). */

    owl_image_set_current(img);
    g_current_instance = inst;  /* permet la resync ultérieure */
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dFinDessin([<Image>])
 *
 * Libère l'image courante. Si un argument Image est fourni, on vérifie
 * que c'est bien la courante (cohérence) puis on libère. Argument
 * optionnel (canon). */
static Value fn_dFinDessin(int argCount, Value* args) {
    (void)argCount; (void)args;
    /* On ignore l'argument explicite : la state-machine est globale. */
    owl_image_set_current(NULL);
    g_current_instance = NULL;
    owl_clear_error();
    return NIL_VAL;
}

/* dStylo([<Image>,] <Couleur> [, <Style> [, <Largeur> [, <Opacité>]]])
 *
 * Définit le stylo (couleur du trait + style + largeur + opacité) pour
 * l'image cible. */
static Value fn_dStylo(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dStylo",
                              "native", "dStylo", NULL);
        return NIL_VAL;
    }
    OWL_REQUIRE_ARG("dStylo", "couleur", shift, IS_INTEGER(args[shift]),
                    "entier", NIL_VAL);
    int64_t couleurBGR = AS_INTEGER(args[shift]) & 0x00FFFFFFu;
    int     style      = (argCount > shift + 1 && IS_INTEGER(args[shift + 1]))
                         ? (int)AS_INTEGER(args[shift + 1]) : 0;
    int     width      = (argCount > shift + 2 && IS_INTEGER(args[shift + 2]))
                         ? (int)AS_INTEGER(args[shift + 2]) : 1;
    int     opacity    = (argCount > shift + 3 && IS_INTEGER(args[shift + 3]))
                         ? (int)AS_INTEGER(args[shift + 3]) : 255;
    OwlImageColor packed = ((OwlImageColor)opacity << 24) | (OwlImageColor)couleurBGR;
    owl_image_set_pen(img, packed, style, width, opacity);
    owl_clear_error();
    return NIL_VAL;
}

/* dFond([<Image>,] <Couleur> [, <Style> [, <Hachures> [, <Opacité>]]])
 *
 * Définit le fond (couleur de remplissage + style + hachures + opacité). */
static Value fn_dFond(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dFond",
                              "native", "dFond", NULL);
        return NIL_VAL;
    }
    OWL_REQUIRE_ARG("dFond", "couleur", shift, IS_INTEGER(args[shift]),
                    "entier", NIL_VAL);
    int64_t couleurBGR = AS_INTEGER(args[shift]) & 0x00FFFFFFu;
    int     style      = (argCount > shift + 1 && IS_INTEGER(args[shift + 1]))
                         ? (int)AS_INTEGER(args[shift + 1]) : OWL_FOND_PLEIN;
    int     hatch      = (argCount > shift + 2 && IS_INTEGER(args[shift + 2]))
                         ? (int)AS_INTEGER(args[shift + 2]) : 0;
    int     opacity    = (argCount > shift + 3 && IS_INTEGER(args[shift + 3]))
                         ? (int)AS_INTEGER(args[shift + 3]) : 255;
    OwlImageColor packed = ((OwlImageColor)opacity << 24) | (OwlImageColor)couleurBGR;
    owl_image_set_fond(img, packed, style, hatch, opacity);
    owl_clear_error();
    return NIL_VAL;
}

/* dLigne([<Image>,] <X1>, <Y1>, <X2>, <Y2> [, <Couleur> [, <Largeur>]]).
 *
 * Trace une ligne entre les deux points. Si la couleur est omise, le
 * stylo courant de l'image cible est utilisé (couleur + opacité +
 * largeur). Une largeur explicite remplace celle du stylo. */
static Value fn_dLigne(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dLigne",
                              "native", "dLigne", NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 4;
    if (argCount < needArgs ||
        !IS_INTEGER(args[shift])     || !IS_INTEGER(args[shift + 1]) ||
        !IS_INTEGER(args[shift + 2]) || !IS_INTEGER(args[shift + 3])) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dLigne",
                              "native", "dLigne", "arg", "x1,y1,x2,y2", NULL);
        return NIL_VAL;
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);

    /* Couleur explicite ou stylo courant. */
    OwlImageColor color;
    int width;
    if (argCount > shift + 4 && IS_INTEGER(args[shift + 4])) {
        int64_t bgr = AS_INTEGER(args[shift + 4]) & 0x00FFFFFFu;
        int alpha = owl_image_pen_opacity(img);  /* hérite de l'opacité du stylo */
        color = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        width = (argCount > shift + 5 && IS_INTEGER(args[shift + 5]))
                ? (int)AS_INTEGER(args[shift + 5])
                : owl_image_pen_width(img);
    } else {
        /* Stylo style 5 (invisible) : pas de tracé quand on hérite du pen. */
        if (owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE) {
            owl_clear_error();
            return NIL_VAL;
        }
        color = owl_image_pen_color(img);
        width = owl_image_pen_width(img);
    }
    owl_image_draw_line(img, x1, y1, x2, y2, color, width);
    owl_clear_error();
    return NIL_VAL;
}

/* dRectangle([<Image>,] <X1>, <Y1>, <X2>, <Y2> [, <Fond> [, <Trait>]]).
 *
 * Dessine un rectangle. Le paramètre <Fond> contrôle le remplissage
 * (couleur, ou fond courant si omis) ; <Trait> contrôle le contour
 * (couleur du trait, ou stylo courant si omis). Si le fond courant
 * est FondTransparent, l'intérieur n'est pas peint — seul le contour
 * est tracé. */
static Value fn_dRectangle(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dRectangle",
                              "native", "dRectangle", NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 4;
    if (argCount < needArgs ||
        !IS_INTEGER(args[shift])     || !IS_INTEGER(args[shift + 1]) ||
        !IS_INTEGER(args[shift + 2]) || !IS_INTEGER(args[shift + 3])) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dRectangle",
                              "native", "dRectangle", "arg", "x1,y1,x2,y2", NULL);
        return NIL_VAL;
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);

    /* Fond : couleur explicite ou fond courant.
     * On respecte FondTransparent (style 1) : pas de remplissage. */
    bool fillEnabled;
    OwlImageColor fillColor;
    if (argCount > shift + 4 && IS_INTEGER(args[shift + 4])) {
        int64_t bgr = AS_INTEGER(args[shift + 4]) & 0x00FFFFFFu;
        int alpha = owl_image_fond_opacity(img);
        fillColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    } else {
        fillColor = owl_image_fond_color(img);
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    }
    if (fillEnabled) {
        owl_image_fill_rect(img, x1, y1, x2, y2, fillColor);
    }

    /* Trait : opt-in canon — tracé uniquement si arg <Trait> explicite
     * ou si dStylo a été appelé sur cette image au moins une fois.
     * Stylo style 5 (invisible) : pas de contour quand on hérite du pen. */
    bool traitExplicite = (argCount > shift + 5 && IS_INTEGER(args[shift + 5]));
    bool penHerite      = !traitExplicite && owl_image_pen_is_set(img);
    bool penInvisible   = penHerite && owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE;
    if ((traitExplicite || penHerite) && !penInvisible) {
        OwlImageColor strokeColor;
        if (traitExplicite) {
            int64_t bgr = AS_INTEGER(args[shift + 5]) & 0x00FFFFFFu;
            int alpha = owl_image_pen_opacity(img);
            strokeColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        } else {
            strokeColor = owl_image_pen_color(img);
        }
        int width = owl_image_pen_width(img);
        owl_image_draw_rect_outline(img, x1, y1, x2, y2, strokeColor, width);
    }
    owl_clear_error();
    return NIL_VAL;
}

/* dCercle([<Image>,] <X1>, <Y1>, <X2>, <Y2> [, <Fond> [, <Trait>]]).
 *
 * Dessine un cercle (ou une ellipse si la bbox n'est pas carrée)
 * inscrit dans le rectangle (x1, y1)..(x2, y2). Fond + contour
 * dispatchés comme pour dRectangle : fond courant + FondTransparent
 * supportés, stylo courant utilisé si <Trait> omis. */
static Value fn_dCercle(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dCercle",
                              "native", "dCercle", NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 4;
    if (argCount < needArgs ||
        !IS_INTEGER(args[shift])     || !IS_INTEGER(args[shift + 1]) ||
        !IS_INTEGER(args[shift + 2]) || !IS_INTEGER(args[shift + 3])) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dCercle",
                              "native", "dCercle", "arg", "x1,y1,x2,y2", NULL);
        return NIL_VAL;
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);

    /* Fond : couleur explicite ou fond courant. FondTransparent = skip. */
    bool fillEnabled;
    OwlImageColor fillColor;
    if (argCount > shift + 4 && IS_INTEGER(args[shift + 4])) {
        int64_t bgr = AS_INTEGER(args[shift + 4]) & 0x00FFFFFFu;
        int alpha = owl_image_fond_opacity(img);
        fillColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    } else {
        fillColor = owl_image_fond_color(img);
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    }
    if (fillEnabled) {
        owl_image_fill_ellipse(img, x1, y1, x2, y2, fillColor);
    }

    /* Trait : opt-in canon — tracé uniquement si arg <Trait> explicite
     * ou si dStylo a été appelé sur cette image au moins une fois.
     * Stylo style 5 (invisible) : pas de contour quand on hérite du pen. */
    bool traitExplicite = (argCount > shift + 5 && IS_INTEGER(args[shift + 5]));
    bool penHerite      = !traitExplicite && owl_image_pen_is_set(img);
    bool penInvisible   = penHerite && owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE;
    if ((traitExplicite || penHerite) && !penInvisible) {
        OwlImageColor strokeColor;
        if (traitExplicite) {
            int64_t bgr = AS_INTEGER(args[shift + 5]) & 0x00FFFFFFu;
            int alpha = owl_image_pen_opacity(img);
            strokeColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        } else {
            strokeColor = owl_image_pen_color(img);
        }
        int width = owl_image_pen_width(img);
        owl_image_draw_ellipse_outline(img, x1, y1, x2, y2, strokeColor, width);
    }
    owl_clear_error();
    return NIL_VAL;
}

/* dArc([<Image>,] <X1>, <Y1>, <X2>, <Y2>, <X3>, <Y3>, <X4>, <Y4>
 *      [, <Couleur> [, <Largeur>]]).
 *
 * Trace l'arc de cercle/ellipse défini par (x1,y1)..(x2,y2) (bbox)
 * entre les points de départ (x3,y3) et d'arrivée (x4,y4) — projetés
 * sur le contour si nécessaire. Pas de remplissage. */
static Value fn_dArc(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dArc",
                              "native", "dArc", NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 8;
    if (argCount < needArgs) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dArc",
                              "native", "dArc", "arg", "x1,y1,x2,y2,x3,y3,x4,y4", NULL);
        return NIL_VAL;
    }
    for (int i = 0; i < 8; i++) {
        if (!IS_INTEGER(args[shift + i])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dArc",
                                  "native", "dArc", "arg", "coord",
                                  "expected", "entier", "actual", "autre", NULL);
            return NIL_VAL;
        }
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);
    int x3 = (int)AS_INTEGER(args[shift + 4]);
    int y3 = (int)AS_INTEGER(args[shift + 5]);
    int x4 = (int)AS_INTEGER(args[shift + 6]);
    int y4 = (int)AS_INTEGER(args[shift + 7]);

    OwlImageColor color;
    int width;
    if (argCount > shift + 8 && IS_INTEGER(args[shift + 8])) {
        int64_t bgr = AS_INTEGER(args[shift + 8]) & 0x00FFFFFFu;
        int alpha = owl_image_pen_opacity(img);
        color = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        width = (argCount > shift + 9 && IS_INTEGER(args[shift + 9]))
                ? (int)AS_INTEGER(args[shift + 9])
                : owl_image_pen_width(img);
    } else {
        /* Stylo style 5 (invisible) : pas de tracé quand on hérite du pen. */
        if (owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE) {
            owl_clear_error();
            return NIL_VAL;
        }
        color = owl_image_pen_color(img);
        width = owl_image_pen_width(img);
    }
    owl_image_draw_arc(img, x1, y1, x2, y2, x3, y3, x4, y4, color, width);
    owl_clear_error();
    return NIL_VAL;
}

/* Helper commun pour dCorde et dPortion : 8 coordonnées + fond + trait. */
typedef enum { CHORD_OR_PIE_CHORD, CHORD_OR_PIE_PIE } ChordPieKind;

static Value chord_or_pie_impl(int argCount, Value* args,
                               const char* fnName,
                               ChordPieKind kind) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, fnName,
                              "native", fnName, NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 8;
    if (argCount < needArgs) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, fnName,
                              "native", fnName,
                              "arg", "x1,y1,x2,y2,x3,y3,x4,y4", NULL);
        return NIL_VAL;
    }
    for (int i = 0; i < 8; i++) {
        if (!IS_INTEGER(args[shift + i])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "native", fnName, "arg", "coord",
                                  "expected", "entier", "actual", "autre", NULL);
            return NIL_VAL;
        }
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);
    int x3 = (int)AS_INTEGER(args[shift + 4]);
    int y3 = (int)AS_INTEGER(args[shift + 5]);
    int x4 = (int)AS_INTEGER(args[shift + 6]);
    int y4 = (int)AS_INTEGER(args[shift + 7]);

    /* Fond. */
    bool fillEnabled;
    OwlImageColor fillColor;
    if (argCount > shift + 8 && IS_INTEGER(args[shift + 8])) {
        int64_t bgr = AS_INTEGER(args[shift + 8]) & 0x00FFFFFFu;
        int alpha = owl_image_fond_opacity(img);
        fillColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    } else {
        fillColor = owl_image_fond_color(img);
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    }
    /* Cas particulier 360° (départ == arrivée) : cercle/ellipse plein.
     * Sinon fill_chord/fill_pie traiterait l'angle nul comme 0° = rien. */
    bool isFullCircle = (x3 == x4 && y3 == y4);
    if (fillEnabled) {
        if (isFullCircle) {
            owl_image_fill_ellipse(img, x1, y1, x2, y2, fillColor);
        } else if (kind == CHORD_OR_PIE_CHORD) {
            owl_image_fill_chord(img, x1, y1, x2, y2, x3, y3, x4, y4, fillColor);
        } else {
            owl_image_fill_pie(img, x1, y1, x2, y2, x3, y3, x4, y4, fillColor);
        }
    }

    /* Trait : opt-in canon.
     * Stylo style 5 (invisible) : pas de contour quand on hérite du pen. */
    bool traitExplicite = (argCount > shift + 9 && IS_INTEGER(args[shift + 9]));
    bool penHerite      = !traitExplicite && owl_image_pen_is_set(img);
    bool penInvisible   = penHerite && owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE;
    if ((traitExplicite || penHerite) && !penInvisible) {
        OwlImageColor strokeColor;
        if (traitExplicite) {
            int64_t bgr = AS_INTEGER(args[shift + 9]) & 0x00FFFFFFu;
            int alpha = owl_image_pen_opacity(img);
            strokeColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        } else {
            strokeColor = owl_image_pen_color(img);
        }
        int width = owl_image_pen_width(img);
        if (isFullCircle) {
            owl_image_draw_ellipse_outline(img, x1, y1, x2, y2,
                                           strokeColor, width);
        } else if (kind == CHORD_OR_PIE_CHORD) {
            owl_image_draw_chord_outline(img, x1, y1, x2, y2, x3, y3, x4, y4,
                                         strokeColor, width);
        } else {
            owl_image_draw_pie_outline(img, x1, y1, x2, y2, x3, y3, x4, y4,
                                       strokeColor, width);
        }
    }
    owl_clear_error();
    return NIL_VAL;
}

/* dCorde : zone fermée par l'arc et la corde droite (xs, ys)→(xe, ye). */
static Value fn_dCorde(int argCount, Value* args) {
    return chord_or_pie_impl(argCount, args, "dCorde", CHORD_OR_PIE_CHORD);
}

/* dPortion : part de tarte (arc + 2 rayons centre→points). */
static Value fn_dPortion(int argCount, Value* args) {
    return chord_or_pie_impl(argCount, args, "dPortion", CHORD_OR_PIE_PIE);
}

/* Helper : extrait les coordonnées d'un polygone depuis les arguments
 * sous l'une des deux formes canon :
 *   forme 1 : N (entier) + N paires d'entiers en arguments
 *   forme 2 : un tableau d'entiers de longueur paire (≥ 6 = 3 points)
 *
 * Renvoie un pointeur malloc'é (caller free) ou NULL si invalide.
 * Pose *outCount au nombre de POINTS, *outNextArg à l'index du
 * premier argument qui suit les coordonnées (couleurs optionnelles).
 *
 * `firstArg` est l'index où commencent les coordonnées (après le
 * shift `<Image>` éventuel). */
static int* parse_polygon_coords(int argCount, Value* args, int firstArg,
                                 int* outCount, int* outNextArg,
                                 const char* fnName) {
    if (firstArg >= argCount) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, fnName,
                              "native", fnName, "arg", "points", NULL);
        return NULL;
    }

    /* Forme 2 : tableau d'entiers en 1er argument. */
    if (IS_ARRAY(args[firstArg])) {
        ObjArray* arr = AS_ARRAY(args[firstArg]);
        int n = arr->elements.count;
        if (n < 4 || (n % 2) != 0) {
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, fnName,
                                  "native", fnName, "arg", "tableau",
                                  NULL);
            return NULL;
        }
        int* coords = (int*)malloc(sizeof(int) * n);
        if (!coords) return NULL;
        for (int i = 0; i < n; i++) {
            Value v = arr->elements.values[i];
            if (!IS_INTEGER(v)) {
                free(coords);
                owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                      "native", fnName, "arg", "tableau[i]",
                                      "expected", "entier", "actual", "autre", NULL);
                return NULL;
            }
            coords[i] = (int)AS_INTEGER(v);
        }
        *outCount = n / 2;
        *outNextArg = firstArg + 1;
        return coords;
    }

    /* Forme 1 : N suivi des paires. */
    if (!IS_INTEGER(args[firstArg])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "native", fnName, "arg", "N",
                              "expected", "entier|tableau", "actual", "autre", NULL);
        return NULL;
    }
    int n = (int)AS_INTEGER(args[firstArg]);
    if (n < 2) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, fnName,
                              "native", fnName, "arg", "N", NULL);
        return NULL;
    }
    if (firstArg + 1 + 2 * n > argCount) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, fnName,
                              "native", fnName, "arg", "paires coords", NULL);
        return NULL;
    }
    int* coords = (int*)malloc(sizeof(int) * 2 * n);
    if (!coords) return NULL;
    for (int i = 0; i < 2 * n; i++) {
        Value v = args[firstArg + 1 + i];
        if (!IS_INTEGER(v)) {
            free(coords);
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                                  "native", fnName, "arg", "coord",
                                  "expected", "entier", "actual", "autre", NULL);
            return NULL;
        }
        coords[i] = (int)AS_INTEGER(v);
    }
    *outCount = n;
    *outNextArg = firstArg + 1 + 2 * n;
    return coords;
}

/* dPolygone([<Image>,] <N>, <X1>, <Y1>, ..., <Xn>, <Yn> [, <Fond> [, <Trait>]])
 * dPolygone([<Image>,] <Tableau de coordonnées> [, <Fond> [, <Trait>]])
 *
 * Polygone fermé (le dernier point est relié au premier). Fond + contour
 * dispatchés comme pour dRectangle. */
static Value fn_dPolygone(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dPolygone",
                              "native", "dPolygone", NULL);
        return NIL_VAL;
    }
    int nextArg = 0;
    int npoints = 0;
    int* coords = parse_polygon_coords(argCount, args, shift, &npoints, &nextArg,
                                       "dPolygone");
    if (!coords) return NIL_VAL;

    /* Fond. */
    bool fillEnabled;
    OwlImageColor fillColor;
    if (nextArg < argCount && IS_INTEGER(args[nextArg])) {
        int64_t bgr = AS_INTEGER(args[nextArg]) & 0x00FFFFFFu;
        int alpha = owl_image_fond_opacity(img);
        fillColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    } else {
        fillColor = owl_image_fond_color(img);
        fillEnabled = (owl_image_fond_style(img) != OWL_FOND_TRANSPARENT);
    }
    if (fillEnabled && npoints >= 3) {
        owl_image_fill_polygon(img, coords, npoints, fillColor);
    }

    /* Trait : opt-in canon.
     * Stylo style 5 (invisible) : pas de contour quand on hérite du pen. */
    bool traitExplicite = (nextArg + 1 < argCount && IS_INTEGER(args[nextArg + 1]));
    bool penHerite      = !traitExplicite && owl_image_pen_is_set(img);
    bool penInvisible   = penHerite && owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE;
    if ((traitExplicite || penHerite) && !penInvisible) {
        OwlImageColor strokeColor;
        if (traitExplicite) {
            int64_t bgr = AS_INTEGER(args[nextArg + 1]) & 0x00FFFFFFu;
            int alpha = owl_image_pen_opacity(img);
            strokeColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
        } else {
            strokeColor = owl_image_pen_color(img);
        }
        int width = owl_image_pen_width(img);
        owl_image_draw_polyline(img, coords, npoints, strokeColor, width, /*closed=*/true);
    }
    free(coords);
    owl_clear_error();
    return NIL_VAL;
}

/* dPolyligne([<Image>,] <N>, <X1>, <Y1>, ..., <Xn>, <Yn> [, <Couleur>])
 * dPolyligne([<Image>,] <Tableau de coordonnées> [, <Couleur>])
 *
 * Ligne brisée non fermée. Pas de remplissage. */
static Value fn_dPolyligne(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dPolyligne",
                              "native", "dPolyligne", NULL);
        return NIL_VAL;
    }
    int nextArg = 0;
    int npoints = 0;
    int* coords = parse_polygon_coords(argCount, args, shift, &npoints, &nextArg,
                                       "dPolyligne");
    if (!coords) return NIL_VAL;

    OwlImageColor color;
    if (nextArg < argCount && IS_INTEGER(args[nextArg])) {
        int64_t bgr = AS_INTEGER(args[nextArg]) & 0x00FFFFFFu;
        int alpha = owl_image_pen_opacity(img);
        color = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
    } else {
        /* Stylo style 5 (invisible) : pas de tracé quand on hérite du pen. */
        if (owl_image_pen_style(img) == OWL_PEN_STYLE_INVISIBLE) {
            free(coords);
            owl_clear_error();
            return NIL_VAL;
        }
        color = owl_image_pen_color(img);
    }
    int width = owl_image_pen_width(img);
    owl_image_draw_polyline(img, coords, npoints, color, width, /*closed=*/false);
    free(coords);
    owl_clear_error();
    return NIL_VAL;
}

/* dRemplissage([<Image>,] <X>, <Y> [, <CouleurFond> [, <CouleurBordure>]])
 *
 * Flood fill 4-connecté à partir de (x, y). Si la couleur de bordure
 * est passée, on s'arrête à cette couleur (boundary fill) ; sinon on
 * remplit tous les pixels connexes ayant la même couleur que (x, y). */
static Value fn_dRemplissage(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dRemplissage",
                              "native", "dRemplissage", NULL);
        return NIL_VAL;
    }
    if (argCount < shift + 2 ||
        !IS_INTEGER(args[shift]) || !IS_INTEGER(args[shift + 1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dRemplissage",
                              "native", "dRemplissage", "arg", "x/y",
                              "expected", "entier", "actual", "autre", NULL);
        return NIL_VAL;
    }
    int x = (int)AS_INTEGER(args[shift]);
    int y = (int)AS_INTEGER(args[shift + 1]);

    /* Couleur de remplissage : explicite ou fond courant. */
    OwlImageColor fillColor;
    if (argCount > shift + 2 && IS_INTEGER(args[shift + 2])) {
        int64_t bgr = AS_INTEGER(args[shift + 2]) & 0x00FFFFFFu;
        int alpha = owl_image_fond_opacity(img);
        fillColor = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
    } else {
        fillColor = owl_image_fond_color(img);
    }

    /* Couleur de bordure : optionnelle. */
    bool hasBorder = false;
    OwlImageColor borderColor = 0;
    if (argCount > shift + 3 && IS_INTEGER(args[shift + 3])) {
        int64_t bgr = AS_INTEGER(args[shift + 3]) & 0x00FFFFFFu;
        borderColor = (OwlImageColor)bgr;
        hasBorder = true;
    }

    owl_image_flood_fill(img, x, y, fillColor, hasBorder, borderColor);
    owl_clear_error();
    return NIL_VAL;
}

/* Helper commun : récupère et synchronise l'OwlImage* depuis un
 * argument supposé Image. Renvoie NULL et pose une erreur si invalide. */
static OwlImage* require_image_arg(Value v, const char* fnName) {
    if (!IS_INSTANCE(v) || !stdlib_image_is_instance(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "native", fnName, "arg", "1",
                              "expected", "Image", "actual", "autre", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, fnName,
                              "native", fnName, NULL);
        return NULL;
    }
    return img;
}

/*  : lit un argument opacite Groupe B en tolerant entier/reel.
 * Si argCount n'a pas d'opacite a `idx`, retourne 100.0 (defaut canon).
 * `*outValid` mis a true si le type est OK (entier|reel), false si autre
 * type a la position idx. */
static double read_opacity_percent_arg(int argCount, Value* args, int idx,
                                       bool* outValid) {
    *outValid = true;
    if (idx >= argCount) return 100.0;
    Value v = args[idx];
    if (IS_REAL(v))    return AS_REAL(v);
    if (IS_INTEGER(v)) return (double)AS_INTEGER(v);
    *outValid = false;
    return 100.0;
}

/*  : garde type propre pour un argument <Image> a la position
 * `idx` (0-based). Ordre canonique : missing -> type_mismatch -> invalid
 * handle. Pour passer un libelle d'argument lisible, `argLabel` est
 * utilise (ex: "1", "destination", "source"). Renvoie NULL si erreur. */
static OwlImage* require_image_at(int argCount, Value* args, int idx,
                                  const char* fnName, const char* argLabel) {
    if (idx >= argCount) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, fnName,
                              "native", fnName, "arg", argLabel, NULL);
        return NULL;
    }
    if (!IS_INSTANCE(args[idx])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "native", fnName, "arg", argLabel,
                              "expected", "Image", "actual", "autre", NULL);
        return NULL;
    }
    if (!stdlib_image_is_instance(args[idx])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fnName,
                              "native", fnName, "arg", argLabel,
                              "expected", "Image", "actual", "instance", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(args[idx]);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, fnName,
                              "native", fnName, NULL);
        return NULL;
    }
    return img;
}

/* dInverseCouleur(<Image>) — inverse R/G/B (alpha préservé). */
static Value fn_dInverseCouleur(int argCount, Value* args) {
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dInverseCouleur",
                              "native", "dInverseCouleur", "arg", "1", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = require_image_arg(args[0], "dInverseCouleur");
    if (!img) return BOOL_VAL(false);
    owl_image_invert_colors(img);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dIncrusteCouleur(<Image>, <Couleur> [, <Opacité>]) — teinte uniforme.
 *
 *  : opacite alignee canon = reel 0-100% optionnel (defaut
 * 100%). Conversion interne *2.55 vers byte 0-255 pour owl_image_colorize.
 * Clamp doux hors plage (canon silencieux). */
static Value fn_dIncrusteCouleur(int argCount, Value* args) {
    OwlImage* img = require_image_at(argCount, args, 0, "dIncrusteCouleur", "1");
    if (!img) return NIL_VAL;
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dIncrusteCouleur",
                              "native", "dIncrusteCouleur", "arg", "couleur", NULL);
        return NIL_VAL;
    }
    if (!IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dIncrusteCouleur",
                              "native", "dIncrusteCouleur", "arg", "couleur",
                              "expected", "entier", "actual", "autre", NULL);
        return NIL_VAL;
    }
    int64_t bgr = AS_INTEGER(args[1]) & 0x00FFFFFFu;

    bool opaValid = true;
    double percent = read_opacity_percent_arg(argCount, args, 2, &opaValid);
    if (!opaValid) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dIncrusteCouleur",
                              "native", "dIncrusteCouleur", "arg", "opacité",
                              "expected", "réel", "actual", "autre", NULL);
        return NIL_VAL;
    }
    if (percent < 0.0)   percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    int alphaByte = (int)(percent * 2.55 + 0.5);

    OwlImageColor packed = ((OwlImageColor)0xFF << 24) | (OwlImageColor)bgr;
    owl_image_colorize(img, packed, alphaByte);
    owl_clear_error();
    return NIL_VAL;
}

/* dModifieContraste(<Image>, <Variation>) — variation -100..+100.
 *
 *  : valide la plage canon strictement. Hors plage : retour
 * Faux + ErreurDetectee=Vrai (canon "<Resultat>: Vrai si modifie,
 * Faux sinon"). Accepte entier ou reel (canon = reel mais l'usage
 * courant passe un entier). */
static Value fn_dModifieContraste(int argCount, Value* args) {
    OwlImage* img = require_image_at(argCount, args, 0, "dModifieContraste", "1");
    if (!img) return BOOL_VAL(false);
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dModifieContraste",
                              "native", "dModifieContraste", "arg", "variation", NULL);
        return BOOL_VAL(false);
    }
    double variation;
    if (IS_INTEGER(args[1]))   variation = (double)AS_INTEGER(args[1]);
    else if (IS_REAL(args[1])) variation = AS_REAL(args[1]);
    else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dModifieContraste",
                              "native", "dModifieContraste", "arg", "variation",
                              "expected", "entier", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    if (!(variation >= -100.0 && variation <= 100.0)) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dModifieContraste",
                              "native", "dModifieContraste", "arg", "variation", NULL);
        return BOOL_VAL(false);
    }
    owl_image_modify_contrast(img, (int)variation);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dModifieOpacité(<Image>, <Facteur>) — facteur réel 0..100%.
 *
 *  : alignement canon. Facteur en pourcentage 0-100 (ancien
 * 0-1 obsolete). Conversion interne en facteur multiplicatif via /100.
 * Hors plage : erreur levee + no-op (canon spec stricte). */
static Value fn_dModifieOpacite(int argCount, Value* args) {
    OwlImage* img = require_image_at(argCount, args, 0, "dModifieOpacite", "1");
    if (!img) return NIL_VAL;
    if (argCount < 2) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dModifieOpacite",
                              "native", "dModifieOpacite", "arg", "facteur", NULL);
        return NIL_VAL;
    }
    double percent;
    if (IS_REAL(args[1]))         percent = AS_REAL(args[1]);
    else if (IS_INTEGER(args[1])) percent = (double)AS_INTEGER(args[1]);
    else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dModifieOpacite",
                              "native", "dModifieOpacite", "arg", "facteur",
                              "expected", "réel", "actual", "autre", NULL);
        return NIL_VAL;
    }
    if (!(percent >= 0.0 && percent <= 100.0)) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dModifieOpacite",
                              "native", "dModifieOpacite", "arg", "facteur", NULL);
        return NIL_VAL;
    }
    /* Convertit pourcent en facteur multiplicatif applique a l'alpha
     * pixel (canon : 100% = inchange, 0% = transparent total). */
    owl_image_modify_opacity(img, percent / 100.0);
    owl_clear_error();
    return NIL_VAL;
}

/* dModifieSaturation(<Image>, <Variation>) — variation -100..+100. */
static Value fn_dModifieSaturation(int argCount, Value* args) {
    if (argCount < 2 || !IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dModifieSaturation",
                              "native", "dModifieSaturation", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = require_image_arg(args[0], "dModifieSaturation");
    if (!img) return BOOL_VAL(false);
    owl_image_modify_tsl(img, 0, (int)AS_INTEGER(args[1]), 0);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dModifieTeinte(<Image>, <Variation>) — variation en degrés. */
static Value fn_dModifieTeinte(int argCount, Value* args) {
    if (argCount < 2 || !IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dModifieTeinte",
                              "native", "dModifieTeinte", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = require_image_arg(args[0], "dModifieTeinte");
    if (!img) return BOOL_VAL(false);
    owl_image_modify_tsl(img, (int)AS_INTEGER(args[1]), 0, 0);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dModifieTSL(<Image>, <dT>, <dS>, <dL>) — variations combinées. */
static Value fn_dModifieTSL(int argCount, Value* args) {
    if (argCount < 4 ||
        !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]) || !IS_INTEGER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dModifieTSL",
                              "native", "dModifieTSL", NULL);
        return BOOL_VAL(false);
    }
    OwlImage* img = require_image_arg(args[0], "dModifieTSL");
    if (!img) return BOOL_VAL(false);
    owl_image_modify_tsl(img, (int)AS_INTEGER(args[1]),
                         (int)AS_INTEGER(args[2]), (int)AS_INTEGER(args[3]));
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dFlou(<Image> [, <Rayon>]) — box blur.
 *
 *  :
 *   - rayon par defaut 10 (aligne canon, ancien 5)
 *   - rayon < 0 : erreur (canon implicite, valeur invalide)
 *   - rayon == 0 : accepte (no-op, identite). */
static Value fn_dFlou(int argCount, Value* args) {
    OwlImage* img = require_image_at(argCount, args, 0, "dFlou", "1");
    if (!img) return NIL_VAL;
    int radius = 10;
    if (argCount > 1 && IS_INTEGER(args[1])) {
        int64_t r = AS_INTEGER(args[1]);
        if (r < 0) {
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dFlou",
                                  "native", "dFlou", "arg", "rayon", NULL);
            return NIL_VAL;
        }
        radius = (int)r;
    }
    if (radius == 0) {
        /* No-op canonique : pas de moyennage si rayon nul. */
        owl_clear_error();
        return NIL_VAL;
    }
    owl_image_box_blur(img, radius);
    owl_clear_error();
    return NIL_VAL;
}

/* CouleurDominanteImage(<Image>) → entier (couleur BGR). */
static Value fn_CouleurDominanteImage(int argCount, Value* args) {
    if (argCount < 1) return INTEGER_VAL(0);
    if (!IS_INSTANCE(args[0]) || !stdlib_image_is_instance(args[0])) {
        return INTEGER_VAL(0);
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);
    if (!img || !owl_image_valid(img)) return INTEGER_VAL(0);
    OwlImageColor c = owl_image_dominant_color(img);
    return INTEGER_VAL((int64_t)(c & 0x00FFFFFFu));
}

/* dRedimensionne(<Image>, <NouvelleLargeur>, <NouvelleHauteur> [, <Option>]).
 *
 * Modifie la taille de l'image en place. Options combinables :
 * drEtiré (défaut), drHomothétique, drHauteQualité.
 * Renvoie Vrai si succès. */
static Value fn_dRedimensionne(int argCount, Value* args) {
    if (argCount < 3 || !IS_INSTANCE(args[0]) ||
        !IS_INTEGER(args[1]) || !IS_INTEGER(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dRedimensionne",
                              "native", "dRedimensionne", NULL);
        return BOOL_VAL(false);
    }
    if (!stdlib_image_is_instance(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dRedimensionne",
                              "native", "dRedimensionne", "arg", "1",
                              "expected", "Image", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dRedimensionne",
                              "native", "dRedimensionne", NULL);
        return BOOL_VAL(false);
    }
    int w = (int)AS_INTEGER(args[1]);
    int h = (int)AS_INTEGER(args[2]);

    /* Décodage des options : sentinelles drEtiré=base+1, drHomothétique
     * =base+2, drHauteQualité=base+4. Combinables bitwise par soustraction
     * de la base. drEtiré seul est une valeur "neutre" qu'on ramène à 0. */
    int flags = 0;
    if (argCount > 3 && IS_INTEGER(args[3])) {
        int opt = (int)AS_INTEGER(args[3]);
        if (opt >= OWL_RESIZE_BASE && opt < OWL_RESIZE_BASE + 16) {
            flags = opt - OWL_RESIZE_BASE;
            if (flags == 1) flags = 0;  /* drEtiré seul = pas d'options */
        }
    }
    bool ok = owl_image_resize_to(img, w, h, flags);
    if (ok) image_sync_public(inst, img);
    owl_clear_error();
    return BOOL_VAL(ok);
}

/* dRotation(<Image>, <Angle> [, <Options>]). Angle en degrés (positif
 * = horaire). Options : drDéfaut, drAdapte, drSansAgrandissement. */
static Value fn_dRotation(int argCount, Value* args) {
    if (argCount < 2 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dRotation",
                              "native", "dRotation", NULL);
        return BOOL_VAL(false);
    }
    if (!stdlib_image_is_instance(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dRotation",
                              "native", "dRotation", "arg", "1",
                              "expected", "Image", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    double angle;
    if (IS_INTEGER(args[1]))    angle = (double)AS_INTEGER(args[1]);
    else if (IS_REAL(args[1]))  angle = AS_REAL(args[1]);
    else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dRotation",
                              "native", "dRotation", "arg", "angle",
                              "expected", "entier|reel", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    OwlImage* img = image_get_handle(inst);
    image_sync_inst_to_handle(inst, img);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dRotation",
                              "native", "dRotation", NULL);
        return BOOL_VAL(false);
    }

    int rotateOpt = OWL_ROTATE_DEFAUT;
    if (argCount > 2 && IS_INTEGER(args[2])) {
        int opt = (int)AS_INTEGER(args[2]);
        if      (opt == (int)OWL_ROTATE_OPT_ADAPTE)              rotateOpt = OWL_ROTATE_ADAPTE;
        else if (opt == (int)OWL_ROTATE_OPT_SANS_AGRANDISSEMENT) rotateOpt = OWL_ROTATE_SANS_AGRANDISSEMENT;
        else                                                     rotateOpt = OWL_ROTATE_DEFAUT;
    }
    bool ok = owl_image_rotate(img, angle, rotateOpt);
    if (ok) image_sync_public(inst, img);
    owl_clear_error();
    return BOOL_VAL(ok);
}

/* dDécoupe(<Image>, <X>, <Y>, <Largeur>, <Hauteur>) → nouvelle Image.
 *
 * Retourne une nouvelle image contenant la zone
 * (x, y, largeur, hauteur) extraite de la source. La source reste
 * intacte (non-mutative).
 *
 * Erreurs (image vide retournée + ErreurDétectée posée) :
 *   - argument invalide ou type incorrect
 *   - source invalide
 *   - largeur <= 0 ou hauteur <= 0
 *   - zone entièrement hors source (rien à copier)
 *
 * Les pixels hors de la zone source mais à l'intérieur du rectangle
 * demandé (cas zone partiellement débordante) sont remplis avec la
 * CouleurRemplissage de la source. */
static Value fn_dDecoupe(int argCount, Value* args) {
    if (argCount < 5 || !IS_INSTANCE(args[0]) ||
        !IS_INTEGER(args[1]) || !IS_INTEGER(args[2]) ||
        !IS_INTEGER(args[3]) || !IS_INTEGER(args[4])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "dDecoupe",
                              "native", "dDecoupe", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    if (!stdlib_image_is_instance(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dDecoupe",
                              "native", "dDecoupe", "arg", "1",
                              "expected", "Image", "actual", "autre", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    OwlImage* src = image_get_handle(inst);
    image_sync_inst_to_handle(inst, src);
    if (!src || !owl_image_valid(src)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dDecoupe",
                              "native", "dDecoupe", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    int x = (int)AS_INTEGER(args[1]);
    int y = (int)AS_INTEGER(args[2]);
    int w = (int)AS_INTEGER(args[3]);
    int h = (int)AS_INTEGER(args[4]);
    if (w <= 0 || h <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dDecoupe",
                              "native", "dDecoupe",
                              "arg", "largeur/hauteur", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    int srcW = owl_image_width(src);
    int srcH = owl_image_height(src);
    /* Zone entièrement hors source : aucun pixel à copier. */
    if (x >= srcW || y >= srcH || x + w <= 0 || y + h <= 0) {
        owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "dDecoupe",
                              "native", "dDecoupe",
                              "arg", "zone hors source", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }

    /* Alloue la nouvelle image, pré-remplie avec la couleur de
     * remplissage de la source (cohérent avec owl_image_crop). */
    OwlImage* dst = owl_image_new(w, h, owl_image_fill_color(src));
    if (!dst) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dDecoupe",
                              "native", "dDecoupe", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    /* Copie pixel-à-pixel via l'API publique (clamp hors-source ->
     * fillColor déjà en place). */
    for (int dy = 0; dy < h; dy++) {
        int sy = y + dy;
        if (sy < 0 || sy >= srcH) continue;
        for (int dx = 0; dx < w; dx++) {
            int sx = x + dx;
            if (sx < 0 || sx >= srcW) continue;
            OwlImageColor c = owl_image_get_pixel(src, sx, sy);
            owl_image_set_pixel(dst, dx, dy, c);
        }
    }
    owl_clear_error();
    return OBJ_VAL(image_make_instance(dst));
}

/* dCopie(<Source>, <Destination> [, <Sx>, <Sy>, <Sw>, <Sh>, <Dx>, <Dy>, <Dw>, <Dh>]).
 *
 * Sans coordonnées, copie toute la source dans le coin haut-gauche
 * du destination (sans rééchantillonner). Avec coordonnées, blit avec
 * rééchantillonnage bilinéaire si tailles différentes. */
static Value fn_dCopie(int argCount, Value* args) {
    OWL_REQUIRE_ARG("dCopie", "src", 0, IS_INSTANCE(args[0]), "Image", BOOL_VAL(false));
    OWL_REQUIRE_ARG("dCopie", "dst", 1, IS_INSTANCE(args[1]), "Image", BOOL_VAL(false));
    if (!stdlib_image_is_instance(args[0]) || !stdlib_image_is_instance(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dCopie",
                              "native", "dCopie", "arg", "src/dst",
                              "expected", "Image", "actual", "autre", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* srcInst = AS_INSTANCE(args[0]);
    ObjInstance* dstInst = AS_INSTANCE(args[1]);
    OwlImage* src = image_get_handle(srcInst);
    OwlImage* dst = image_get_handle(dstInst);
    image_sync_inst_to_handle(srcInst, src);
    image_sync_inst_to_handle(dstInst, dst);
    if (!src || !owl_image_valid(src) || !dst || !owl_image_valid(dst)) {
        owl_emit_native_error(E_NATIVE_IMAGE_INVALID_HANDLE, "dCopie",
                              "native", "dCopie", NULL);
        return BOOL_VAL(false);
    }
    int sx = 0, sy = 0, sw = -1, sh = -1;
    int dx = 0, dy = 0, dw = -1, dh = -1;
    if (argCount > 5) {
        sx = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;
        sy = IS_INTEGER(args[3]) ? (int)AS_INTEGER(args[3]) : 0;
        sw = IS_INTEGER(args[4]) ? (int)AS_INTEGER(args[4]) : -1;
        sh = IS_INTEGER(args[5]) ? (int)AS_INTEGER(args[5]) : -1;
    }
    if (argCount > 9) {
        dx = IS_INTEGER(args[6]) ? (int)AS_INTEGER(args[6]) : 0;
        dy = IS_INTEGER(args[7]) ? (int)AS_INTEGER(args[7]) : 0;
        dw = IS_INTEGER(args[8]) ? (int)AS_INTEGER(args[8]) : -1;
        dh = IS_INTEGER(args[9]) ? (int)AS_INTEGER(args[9]) : -1;
    }
    bool ok = owl_image_copy_rect(src, dst, sx, sy, sw, sh, dx, dy, dw, dh);
    owl_clear_error();
    return BOOL_VAL(ok);
}

/* dFusionne(<Destination>, <Source> [, <Opacité>]). Alpha-blend source
 * dans destination avec opacité globale modulant l'alpha source.
 *
 *  : opacite alignee canon = reel 0-100% optionnel (defaut
 * 100%). Conversion interne *2.55 vers byte 0-255 pour owl_image_merge.
 * Hors plage : clampe a [0..100] (canon est silencieux sur le clamp,
 * ce test_writer accepte le clamp pour les valeurs aberrantes). */
static Value fn_dFusionne(int argCount, Value* args) {
    OwlImage* dst = require_image_at(argCount, args, 0, "dFusionne", "destination");
    if (!dst) return NIL_VAL;
    OwlImage* src = require_image_at(argCount, args, 1, "dFusionne", "source");
    if (!src) return NIL_VAL;

    bool opaValid = true;
    double percent = read_opacity_percent_arg(argCount, args, 2, &opaValid);
    if (!opaValid) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dFusionne",
                              "native", "dFusionne", "arg", "opacité",
                              "expected", "réel", "actual", "autre", NULL);
        return NIL_VAL;
    }
    /* Clamp doux pour fusionne (canon ne specifie pas de rejet sur
     * cette native ; le clamp matche le comportement des libs). */
    if (percent < 0.0)   percent = 0.0;
    if (percent > 100.0) percent = 100.0;
    int alphaByte = (int)(percent * 2.55 + 0.5);
    owl_image_merge(dst, src, alphaByte);
    owl_clear_error();
    return NIL_VAL;
}

/* dPolice([<Image>,] <Police> [, <Taille> [, <Attributs> [, <Inclinaison>]]]).
 *
 * Charge une police TTF/OTF pour le dessin de texte. <Police> est un
 * chemin de fichier (absolu ou relatif au cwd). Taille en points
 * (12 par défaut), attributs combinables (iGras + iItalique, etc.).
 * L'inclinaison du texte n'est pas appliquée au rendu en V1.
 *
 * Renvoie Vrai si la police a été chargée correctement. */
static Value fn_dPolice(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dPolice",
                              "native", "dPolice", NULL);
        return BOOL_VAL(false);
    }
    OWL_REQUIRE_ARG("dPolice", "chemin", shift, IS_STRING(args[shift]),
                    "chaîne", BOOL_VAL(false));
    const char* path = AS_STRING(args[shift])->chars;
    int point_size = 12;
    if (argCount > shift + 1 && IS_INTEGER(args[shift + 1])) {
        point_size = (int)AS_INTEGER(args[shift + 1]);
        if (point_size < 1) point_size = 1;
    }

    /* Attributs : style_in passe par les sentinelles iNormal/iGras/...
     * (cf ui_catalog.c). Flags bas = bitmap (1=gras, 2=italique, etc.)
     * — identique à OwlFontAttr. */
    int attrs_flags = 0;
    if (argCount > shift + 2 && IS_INTEGER(args[shift + 2])) {
        int style_in = (int)AS_INTEGER(args[shift + 2]);
        if (style_in >= OWL_IPOL_BASE && style_in < OWL_IPOL_BASE + 16) {
            attrs_flags = style_in - OWL_IPOL_BASE;
        }
    }
    /* Inclinaison : V2. Argument shift+3 accepté mais ignoré. */

    bool ok = owl_image_set_font_file(img, path, point_size, attrs_flags);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_IMAGE_FONT_LOAD_FAILURE, "dPolice",
                              "native", "dPolice", "path", path, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* dTexte([<Image>,] <X>, <Y>, <Texte> [, <Couleur> [, <Gestion du &>]]).
 *
 * Rend la chaîne <Texte> à la position (x, y) avec la police courante
 * (chargée par dPolice). La couleur est explicite, ou héritée du
 * stylo courant. La gestion du caractère & est ignorée en V1
 * (canon : interprète & comme indicateur de touche raccourci en
 * Champ Image UI ; hors scope ici). */
static Value fn_dTexte(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dTexte",
                              "native", "dTexte", NULL);
        return NIL_VAL;
    }
    if (argCount < shift + 3 ||
        !IS_INTEGER(args[shift]) || !IS_INTEGER(args[shift + 1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dTexte",
                              "native", "dTexte", "arg", "x/y",
                              "expected", "entier", "actual", "autre", NULL);
        return NIL_VAL;
    }
    if (!owl_image_has_font(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_FONT_REQUIRED, "dTexte",
                              "native", "dTexte", NULL);
        return NIL_VAL;
    }
    int x = (int)AS_INTEGER(args[shift]);
    int y = (int)AS_INTEGER(args[shift + 1]);

    /* Texte : on accepte string, entier, réel — convertir en chaîne. */
    char buf[64];
    const char* text;
    Value tv = args[shift + 2];
    if (IS_STRING(tv)) {
        text = AS_STRING(tv)->chars;
    } else if (IS_INTEGER(tv)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(tv));
        text = buf;
    } else if (IS_REAL(tv)) {
        owl_real_to_str(AS_REAL(tv), buf, sizeof(buf));
        text = buf;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dTexte",
                              "native", "dTexte", "arg", "texte",
                              "expected", "chaine|nombre", "actual", "autre", NULL);
        return NIL_VAL;
    }

    /* Couleur : explicite ou stylo courant. */
    OwlImageColor color;
    if (argCount > shift + 3 && IS_INTEGER(args[shift + 3])) {
        int64_t bgr = AS_INTEGER(args[shift + 3]) & 0x00FFFFFFu;
        int alpha = owl_image_pen_opacity(img);
        color = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr;
    } else {
        color = owl_image_pen_color(img);
    }
    /* Gestion du & : V2 (no-op). */

    owl_image_draw_text(img, x, y, text, color);
    owl_clear_error();
    return NIL_VAL;
}

/* dLargeurTexte([<Image>,] <Texte>) — largeur en pixels du texte dans
 * la police courante de l'image (kerning inclus). Retourne 0 et pose
 * une erreur si l'image n'est pas valide ou si aucune police n'est
 * chargée (cf dPolice). Chaîne vide → 0 sans erreur. */
static Value fn_dLargeurTexte(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dLargeurTexte",
                              "native", "dLargeurTexte", NULL);
        return INTEGER_VAL(0);
    }
    if (argCount < shift + 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dLargeurTexte",
                              "native", "dLargeurTexte", "arg", "texte", NULL);
        return INTEGER_VAL(0);
    }
    if (!owl_image_has_font(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_FONT_REQUIRED, "dLargeurTexte",
                              "native", "dLargeurTexte", NULL);
        return INTEGER_VAL(0);
    }

    /* Accepte string / entier / réel (autoconv comme dTexte). */
    char buf[64];
    const char* text;
    Value tv = args[shift];
    if (IS_STRING(tv)) {
        text = AS_STRING(tv)->chars;
    } else if (IS_INTEGER(tv)) {
        snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(tv));
        text = buf;
    } else if (IS_REAL(tv)) {
        owl_real_to_str(AS_REAL(tv), buf, sizeof(buf));
        text = buf;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dLargeurTexte",
                              "native", "dLargeurTexte", "arg", "texte",
                              "expected", "chaine|nombre", "actual", "autre", NULL);
        return INTEGER_VAL(0);
    }

    owl_clear_error();
    return INTEGER_VAL(owl_image_text_width_px(img, text));
}

/* dHauteurTexte([<Image>,] <Texte>) — hauteur en pixels de la police
 * courante de l'image. La hauteur dépend uniquement de la police, pas
 * du contenu de la chaîne (cohérent avec iHauteurTexte côté PDF) — le
 * texte est néanmoins requis pour parité de signature avec
 * dLargeurTexte et symétrie avec le canon. */
static Value fn_dHauteurTexte(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dHauteurTexte",
                              "native", "dHauteurTexte", NULL);
        return INTEGER_VAL(0);
    }
    if (argCount < shift + 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dHauteurTexte",
                              "native", "dHauteurTexte", "arg", "texte", NULL);
        return INTEGER_VAL(0);
    }
    if (!owl_image_has_font(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_FONT_REQUIRED, "dHauteurTexte",
                              "native", "dHauteurTexte", NULL);
        return INTEGER_VAL(0);
    }

    /* On valide le type du texte (cohérence avec dLargeurTexte) mais
     * on ne mesure que la métrique police. */
    Value tv = args[shift];
    if (!IS_STRING(tv) && !IS_INTEGER(tv) && !IS_REAL(tv)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "dHauteurTexte",
                              "native", "dHauteurTexte", "arg", "texte",
                              "expected", "chaine|nombre", "actual", "autre", NULL);
        return INTEGER_VAL(0);
    }

    owl_clear_error();
    return INTEGER_VAL(owl_image_text_height_px(img));
}

/* dRectangleDégradé([<Image>,] <X1>, <Y1>, <X2>, <Y2>, <Couleur1>, <Couleur2> [, <Angle>]).
 *
 * Remplit le rectangle avec un dégradé linéaire entre les deux
 * couleurs. L'angle (par défaut 0 = horizontal) est normalisé au
 * multiple de 90° le plus proche pour cette implémentation. Le
 * contour est tracé par-dessus avec le stylo courant. */
static Value fn_dRectangleDegrade(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dRectangleDegrade",
                              "native", "dRectangleDegrade", NULL);
        return NIL_VAL;
    }
    int needArgs = shift + 6;  /* x1,y1,x2,y2,c1,c2 minimum */
    if (argCount < needArgs ||
        !IS_INTEGER(args[shift])     || !IS_INTEGER(args[shift + 1]) ||
        !IS_INTEGER(args[shift + 2]) || !IS_INTEGER(args[shift + 3]) ||
        !IS_INTEGER(args[shift + 4]) || !IS_INTEGER(args[shift + 5])) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "dRectangleDegrade",
                              "native", "dRectangleDegrade",
                              "arg", "x1,y1,x2,y2,c1,c2", NULL);
        return NIL_VAL;
    }
    int x1 = (int)AS_INTEGER(args[shift]);
    int y1 = (int)AS_INTEGER(args[shift + 1]);
    int x2 = (int)AS_INTEGER(args[shift + 2]);
    int y2 = (int)AS_INTEGER(args[shift + 3]);
    /* Bbox dégénérée (largeur ou hauteur nulle) : rien à tracer. */
    if (x1 == x2 || y1 == y2) {
        owl_clear_error();
        return NIL_VAL;
    }
    int64_t bgr1 = AS_INTEGER(args[shift + 4]) & 0x00FFFFFFu;
    int64_t bgr2 = AS_INTEGER(args[shift + 5]) & 0x00FFFFFFu;
    int alpha = owl_image_fond_opacity(img);  /* opacité du fond courant */
    OwlImageColor c1 = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr1;
    OwlImageColor c2 = ((OwlImageColor)alpha << 24) | (OwlImageColor)bgr2;

    int angle = 0;
    if (argCount > shift + 6 && IS_INTEGER(args[shift + 6])) {
        angle = (int)AS_INTEGER(args[shift + 6]);
    }
    owl_image_fill_rect_gradient(img, x1, y1, x2, y2, c1, c2, angle);
    owl_clear_error();
    return NIL_VAL;
}

/* dEfface([<Image>] [, <Couleur>])
 *
 * Efface l'image (remplit avec couleur). Si couleur omise, utilise la
 * CouleurRemplissage de l'image. */
static Value fn_dEfface(int argCount, Value* args) {
    int shift = 0;
    OwlImage* img = resolve_target_image(argCount, args, &shift);
    if (!img || !owl_image_valid(img)) {
        owl_emit_native_error(E_NATIVE_IMAGE_NO_CANVAS, "dEfface",
                              "native", "dEfface", NULL);
        return NIL_VAL;
    }
    OwlImageColor color;
    if (argCount > shift && IS_INTEGER(args[shift])) {
        int64_t bgr = AS_INTEGER(args[shift]) & 0x00FFFFFFu;
        color = 0xFF000000u | (OwlImageColor)bgr;
    } else {
        color = owl_image_fill_color(img);
    }
    owl_image_clear(img, color);
    owl_clear_error();
    return NIL_VAL;
}

/* ================================================================
 * API publique pour la VM — Lot I.2.5 (syntaxe `MaImage[x,y]`)
 *
 * Ces helpers sont déclarés dans owl_stdlib.h et appelés depuis vm.c
 * (case OP_ARRAY_GET_2D / OP_ARRAY_SET_2D) pour intercepter le bracket
 * 2D quand le receveur est une Image.
 * ================================================================ */

bool stdlib_image_is_instance(Value v) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    return inst->structName != NULL
        && strcmp(inst->structName->chars, "Image") == 0;
}

int64_t stdlib_image_get_pixel(Value v, int x, int y) {
    OwlImage* img = value_to_image_handle(v);
    if (!img || !owl_image_valid(img)) return 0;
    OwlImageColor c = owl_image_get_pixel(img, x, y);
    /* Retour BGR 24-bit (alpha masqué) — cohérent avec dPixelCouleur. */
    return (int64_t)(c & 0x00FFFFFFu);
}

const uint8_t* stdlib_image_get_rgba(Value v, int* out_w, int* out_h) {
    OwlImage* img = value_to_image_handle(v);
    if (!img || !owl_image_valid(img)) return NULL;
    if (out_w) *out_w = owl_image_width(img);
    if (out_h) *out_h = owl_image_height(img);
    return owl_image_pixels(img);
}

struct OwlImage* stdlib_image_get_handle(Value v) {
    return value_to_image_handle(v);
}

/*  — Public helper : cree une Value Image OWL avec un buffer
 * RGBA8 source (copie). Permet a fn_IconeVersImage de conserver les
 * pixels rasterises au lieu de les jeter, debloquant dPixelCouleur
 * en headless. Buffer source non-owned, caller free apres l'appel. */
Value stdlib_image_value_from_rgba(const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) {
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }
    OwlImage* img = owl_image_new(w, h, 0);
    if (!img || !owl_image_valid(img)) {
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    /* Copy pixels par set_pixel (pattern existant + safe vs alignement
     * interne). 4096 calls pour 64x64 = ~negligeable perf cote rasterize
     * one-shot d'icone. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &rgba[(y * w + x) * 4];
            /* Format RGBA8 source -> OwlImageColor (0xAABBGGRR alpha haut). */
            OwlImageColor c = ((OwlImageColor)p[3] << 24)  /* A */
                            | ((OwlImageColor)p[2] << 16)  /* B */
                            | ((OwlImageColor)p[1] << 8)   /* G */
                            | ((OwlImageColor)p[0]);       /* R */
            owl_image_set_pixel(img, x, y, c);
        }
    }
    return OBJ_VAL(image_make_instance(img));
}

void stdlib_image_set_pixel(Value v, int x, int y, int64_t couleurBGR) {
    OwlImage* img = value_to_image_handle(v);
    if (!img || !owl_image_valid(img)) return;
    /* Conserver l'alpha existant si déjà posé, sinon opaque (255). */
    OwlImageColor existing = owl_image_get_pixel(img, x, y);
    OwlImageColor alpha = existing & 0xFF000000u;
    if (alpha == 0) alpha = 0xFF000000u;
    OwlImageColor packed = alpha | ((OwlImageColor)couleurBGR & 0x00FFFFFFu);
    owl_image_set_pixel(img, x, y, packed);
}

/* ================================================================
 * Parser de coerce vers Image — enregistre via le registry generique.
 *
 * Permet le canon declaration-pilote :
 *   MaImage est une Image
 *   MaImage = "fichier.png"          // string → dChargeImage(string)
 *   MaImage = autreImage             // Image → passthrough
 *
 * NE PAS appeler directement — utiliser via __InstanceCoerce(v, "Image")
 * dispatch (compiler.c emet ce pattern automatiquement). */
static Value parse_to_image(Value v) {
    /* Déjà une Image (ObjInstance structName="Image") → passthrough. */
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName && strcmp(inst->structName->chars, "Image") == 0) {
            return v;
        }
    }

    /*  — ObjIcone rasterise vers Image (RGBA). Symetrique au cas
     * widget Image/Bouton qui accepte deja Icone au SET runtime. Sans ce
     * branchement, `imgX est une Image = IMG_Y..Image` (ou .. Image stocke
     * un ObjIcone polymorphe) tombait sur le fallback Image vide. */
    if (IS_ICONE(v)) {
        unsigned char* rgba = NULL;
        int w = 0, h = 0;
        /* 64 px = OWL_ICONE_RENDER_SIZE (cf. stdlib_icones.c, meme valeur
         * que fn_IconeVersImage pour cohérence du rendu rasterise). */
        if (owl_icone_rasterize_value(v, 64,
                                      &rgba, &w, &h)) {
            Value result = stdlib_image_value_from_rgba(rgba, w, h);
            free(rgba);
            return result;
        }
        /* Rasterizer absent (CLI sans hook Qt) ou icone invalide :
         * Image vide silencieuse, pas d'erreur native (un widget headless
         * peut legitimement etre lu sans rasterizer cable). */
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }

    /* String → dChargeImage. */
    if (IS_STRING(v)) {
        Value loadArgs[1] = { v };
        return fn_dChargeImage(1, loadArgs);
    }

    /* NIL → image vide (cas `MaImage est une Image` sans valeur init). */
    if (IS_NIL(v)) {
        OwlImage* empty = owl_image_new_empty();
        return OBJ_VAL(image_make_instance(empty));
    }

    /* Autre type : on ne peut pas convertir, on retourne une image vide. */
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Image",
                          "native", "Image", "arg", "source",
                          "expected", "Image|chaine", "actual", "autre", NULL);
    OwlImage* empty = owl_image_new_empty();
    return OBJ_VAL(image_make_instance(empty));
}

/* ================================================================
 * Template builder — exporté.
 * ================================================================ */

ObjInstance* build_Image_template(void) {
    ObjString* name = obj_copy_string("Image", 5);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    inst_field_set_int  (tpl, "_ptr",               0);  /* aucun OwlImage* attaché */
    inst_field_set_int  (tpl, "Largeur",            0);
    inst_field_set_int  (tpl, "Hauteur",            0);
    inst_field_set_bool (tpl, "Valide",             false);
    inst_field_set_bool (tpl, "AvecAlpha",          false);
    inst_field_set_int  (tpl, "BitParPixel",        0);
    inst_field_set_int  (tpl, "CouleurRemplissage", IMAGE_DEFAULT_FILL_COLOR_BGR);
    inst_field_set_value(tpl, "EchelleDessin",      REAL_VAL(1.0));

    vm_pop(); /* tpl */
    vm_pop(); /* name */
    return tpl;
}

/*  Lot 2 — dCopieImageFenêtre / dCopieImageChamp / dCopieImageEcran
 * ont migre vers stdlib_ui.c (qui compile dans owl_runtime_gui avec Qt).
 * Les helpers de capture ont besoin du backend Qt pour le grab reel,
 * impossible depuis stdlib_image.c qui vit dans owl_core. Le module Image
 * conserve les natives bitmap-only (dCharge, dCree, dSauve*, dStylo, etc.). */

/* ================================================================
 * Module init
 * ================================================================ */

void stdlib_image_init(void) {
    /* Enum imgOptions — valeurs canoniques uniquement. L'alias ASCII
     * de chaque valeur est résolu au runtime via vm.globals (généré
     * automatiquement par owl_registry_register_consts ci-dessous). */
    static const char* const IMG_OPT_VALUES[] = {
        "imgD\xc3\xa9" "faut", "imgConvertir", NULL
    };
    static const OwlEnum E_imgOptions = {
        .name="imgOptions", .nameEN="imgOptions", .category="Image",
        .descFR="Options de chargement passées à dChargeImage.",
        .descEN="Load options passed to dLoadImage.",
        .values=IMG_OPT_VALUES,
        .combinable=false
    };
    /* Options de dDébutDessin (dEffacer/dSansEffacer/dAvecOpacité).
     * Valeurs sentinelles séquentielles (BASE+1/+2/+3), PAS des bits
     * distincts : le runtime fait des tests d'égalité (opts ==
     * OWL_DESSIN_SANS_EFFACER) et dAvecOpacité est no-op. Donc non
     * combinable → annoncer combinable=true mentirait au LSP sur des
     * OU que le runtime n'honore pas. Rebitting +1/+2/+4 + bitmask
     * runtime = chantier séparé si une vraie combinaison est voulue.
     * Zone sentinel → pas de force_small_int. Câblé en allowedOptions
     * (et pas allowedBase) pour tolérer les littéraux legacy 0/1 sans
     * warning, comme dPolice/E_IStyle (suppression vient de
     * allowedBase=NULL, indépendante du flag combinable). */
    static const char* const DESSIN_OPT_VALUES[] = {
        "dEffacer", "dSansEffacer", "dAvecOpacit\xc3\xa9", NULL
    };
    static const OwlEnum E_dessinOptions = {
        .name="dessinOptions", .nameEN="drawOptions", .category="Image",
        .descFR="Options de préparation du canevas passées à dDébutDessin.",
        .descEN="Canvas preparation options passed to dStartDrawing.",
        .values=DESSIN_OPT_VALUES,
        .combinable=false
    };
    /* Styles de remplissage de dFond (FondPlein/FondTransparent/FondHachuré).
     * Valeurs entières canoniques 0/1/2 portant une sémantique runtime
     * directe (l'impl compare opt == OWL_FOND_TRANSPARENT) → force_small_int. */
    static const char* const FOND_STYLE_VALUES[] = {
        "FondPlein", "FondTransparent", "FondHachur\xc3\xa9", NULL
    };
    static const OwlEnum E_fondStyle = {
        .name="fondStyle", .nameEN="backgroundStyle", .category="Image",
        .descFR="Style de remplissage du fond appliqué par dFond.",
        .descEN="Background fill style applied by dBackground.",
        .values=FOND_STYLE_VALUES,
        .combinable=false, .force_small_int=true
    };
    /* Options de dRedimensionne (drEtiré/drHomothétique/drHauteQualité).
     * Bits distincts (1/2/4) en zone sentinel → combinables par OU. */
    static const char* const REDIM_OPT_VALUES[] = {
        "drEtir\xc3\xa9", "drHomoth\xc3\xa9tique", "drHauteQualit\xc3\xa9", NULL
    };
    static const OwlEnum E_imgRedimOptions = {
        .name="imgRedimOptions", .nameEN="imgResizeOptions", .category="Image",
        .descFR="Options de redimensionnement de dRedimensionne (combinables).",
        .descEN="Resize options for dResize (combinable).",
        .values=REDIM_OPT_VALUES,
        .combinable=true
    };
    /* Options de dRotation (drDéfaut/drAdapte/drSansAgrandissement).
     * Valeurs en zone sentinel, choix exclusif du cadrage → non combinable. */
    static const char* const ROTATION_OPT_VALUES[] = {
        "drD\xc3\xa9" "faut", "drAdapte", "drSansAgrandissement", NULL
    };
    static const OwlEnum E_imgRotationOptions = {
        .name="imgRotationOptions", .nameEN="imgRotationOptions", .category="Image",
        .descFR="Options de cadrage du résultat tourné par dRotation.",
        .descEN="Framing options for the rotated result of dRotate.",
        .values=ROTATION_OPT_VALUES,
        .combinable=false
    };
    /*  Lot 2 — enum ThèmeSnapshot migre vers stdlib_ui.c avec
     * les natives dCopieImage*. */
    static const OwlEnum* const IMG_ENUMS[] = {
        &E_imgOptions, &E_dessinOptions, &E_fondStyle,
        &E_imgRedimOptions, &E_imgRotationOptions
    };

    /* Constantes — register_consts pousse dans le registre LSP, dans
     * vm.globals avec le nom canonique, et auto-génère l'alias ASCII
     * dans vm.globals (sans doublon LSP). Cf CLAUDE.md "Constantes :
     * pattern unifié". */
    static const OwlConstDef IMG_CONSTS[] = {
        /* Lot I.1 — options dChargeImage. */
        { .name = "imgD\xc3\xa9" "faut", .category = "Image", .nameEN = "imgDefault",
          .value = INTEGER_VAL(OWL_IMG_DEFAUT),
          .descFR = "Chargement par défaut (raster ou rasterisation auto pour SVG).",
          .descEN = "Default load (raster or auto-rasterize SVG)." },
        { .name = "imgConvertir", .category = "Image", .nameEN = "imgConvert",
          .value = INTEGER_VAL(OWL_IMG_CONVERTIR),
          .descFR = "Force la conversion en bitmap 24/32 bits (obligatoire pour SVG/PDF).",
          .descEN = "Force conversion to 24/32-bit bitmap (required for SVG/PDF)." },
        /* Lot I.3a — options dDébutDessin. */
        { .name = "dEffacer", .category = "Image", .nameEN = "dErase",
          .value = INTEGER_VAL(OWL_DESSIN_EFFACER),
          .descFR = "dDébutDessin: efface le dessin présent dans l'image (défaut).",
          .descEN = "dStartDrawing: clear the existing image content (default)." },
        { .name = "dSansEffacer", .category = "Image", .nameEN = "dNoErase",
          .value = INTEGER_VAL(OWL_DESSIN_SANS_EFFACER),
          .descFR = "dDébutDessin: n'efface pas le dessin existant.",
          .descEN = "dStartDrawing: do not clear existing content." },
        { .name = "dAvecOpacit\xc3\xa9", .category = "Image", .nameEN = "dWithOpacity",
          .value = INTEGER_VAL(OWL_DESSIN_AVEC_OPACITE),
          .descFR = "dDébutDessin: autorise la gestion de l'opacité (couche alpha) dans les fonctions de dessin.",
          .descEN = "dStartDrawing: enable opacity (alpha channel) management in drawing functions." },
        /* Lot I.3a — styles de fond dFond. */
        { .name = "FondPlein", .category = "Image", .nameEN = "BackgroundSolid",
          .value = INTEGER_VAL(OWL_FOND_PLEIN),
          .descFR = "dFond: remplissage uni (couleur unie, défaut).",
          .descEN = "dBackground: solid fill (default)." },
        { .name = "FondTransparent", .category = "Image", .nameEN = "BackgroundTransparent",
          .value = INTEGER_VAL(OWL_FOND_TRANSPARENT),
          .descFR = "dFond: fond transparent (la couleur est ignorée).",
          .descEN = "dBackground: transparent (color is ignored)." },
        { .name = "FondHachur\xc3\xa9", .category = "Image", .nameEN = "BackgroundHatched",
          .value = INTEGER_VAL(OWL_FOND_HACHURE),
          .descFR = "dFond: remplissage hachuré (le type de hachures est précisé en 3e argument 0..5).",
          .descEN = "dBackground: hatched fill (hatch type given as 3rd argument 0..5)." },
        /* Lot I.4 — options de redimensionnement (combinables). */
        { .name = "drEtir\xc3\xa9", .category = "Image", .nameEN = "drStretched",
          .value = INTEGER_VAL(OWL_RESIZE_OPT_ETIRE),
          .descFR = "dRedimensionne : étire l'image aux nouvelles dimensions sans préserver les proportions (défaut).",
          .descEN = "dResize: stretch the image to the new dimensions without preserving aspect ratio (default)." },
        { .name = "drHomoth\xc3\xa9tique", .category = "Image", .nameEN = "drHomothetic",
          .value = INTEGER_VAL(OWL_RESIZE_OPT_HOMOTHETIQUE),
          .descFR = "dRedimensionne : préserve les proportions de l'image en la centrant et en remplissant le reste avec CouleurRemplissage.",
          .descEN = "dResize: preserve aspect ratio by centering the image and filling the rest with CouleurRemplissage." },
        { .name = "drHauteQualit\xc3\xa9", .category = "Image", .nameEN = "drHighQuality",
          .value = INTEGER_VAL(OWL_RESIZE_OPT_HAUTE_QUALITE),
          .descFR = "dRedimensionne : utilise une interpolation bilinéaire au lieu du plus proche voisin (rendu plus lisse).",
          .descEN = "dResize: use bilinear interpolation instead of nearest neighbor (smoother output)." },
        /* Lot I.4 — options de rotation. */
        { .name = "drD\xc3\xa9" "faut", .category = "Image", .nameEN = "drDefault",
          .value = INTEGER_VAL(OWL_ROTATE_OPT_DEFAUT),
          .descFR = "dRotation : agrandit l'image pour contenir entièrement le résultat tourné (défaut).",
          .descEN = "dRotate: enlarge the image to fully contain the rotated result (default)." },
        { .name = "drAdapte", .category = "Image", .nameEN = "drAdapt",
          .value = INTEGER_VAL(OWL_ROTATE_OPT_ADAPTE),
          .descFR = "dRotation : tourne puis réduit pour revenir à la taille initiale.",
          .descEN = "dRotate: rotate then shrink back to the original size." },
        { .name = "drSansAgrandissement", .category = "Image", .nameEN = "drNoEnlarge",
          .value = INTEGER_VAL(OWL_ROTATE_OPT_SANS_AGRANDISSEMENT),
          .descFR = "dRotation : conserve la taille initiale ; l'image tournée est tronquée si elle dépasse.",
          .descEN = "dRotate: keep the original size; the rotated image is clipped if it exceeds the bounds." },
        /*  Lot 2 — valeurs ThèmeSnapshot migrent vers stdlib_ui.c. */
    };

    static const OwlParam p_charge[] = {
        {"image",   "chaîne", "chemin du fichier image (relatif au cwd ou absolu)"},
        {"options", "entier", "options (imgDéfaut | imgConvertir)", .allowedBase=&E_imgOptions},
        {"largeur", "entier", "largeur cible (image vectorielle uniquement)"},
        {"hauteur", "entier", "hauteur cible (image vectorielle uniquement)"},
        {0}
    };
    static const OwlParam p_cree[] = {
        {"largeur", "entier",  "largeur en pixels (>0)"},
        {"hauteur", "entier",  "hauteur en pixels (>0)"},
        {"couleur", "couleur", "couleur de remplissage (défaut: Blanc)"},
        {0}
    };
    static const OwlParam p_save_basic[] = {
        {"image",  "Image",  "image source"},
        {"chemin", "chaîne", "chemin du fichier de destination"},
        {0}
    };
    static const OwlParam p_save_png[] = {
        {"image",       "Image",   "image source"},
        {"chemin",      "chaîne",  "chemin du fichier PNG de destination"},
        {"transparent", "couleur", "couleur à rendre transparente dans le fichier produit (optionnel — sans cet argument, l'image est sauvée telle quelle)"},
        {0}
    };
    static const OwlParam p_save_jpeg[] = {
        {"image",   "Image",  "image source"},
        {"chemin",  "chaîne", "chemin du fichier JPEG"},
        {"qualité", "entier", "qualité 1..100 (défaut 80)"},
        {0}
    };
    static const OwlParam p_pixel_read[] = {
        {"image",    "Image",  "image source"},
        {"abscisse", "entier", "position horizontale (0 = bord gauche)"},
        {"ordonnée", "entier", "position verticale (0 = bord haut)"},
        {0}
    };
    static const OwlParam p_dpoint[] = {
        {"image",    "Image",   "image cible"},
        {"abscisse", "entier",  "position horizontale"},
        {"ordonnée", "entier",  "position verticale"},
        {"couleur",  "couleur", "couleur du point (BGR sur 24 bits)"},
        {"opacité",  "entier",  "opacité 0..255 (défaut 255 = opaque)"},
        {0}
    };
    /* Lot I.3a — état du dessin (signatures avec <Image> optionnelle). */
    static const OwlParam p_debut[] = {
        {"image",   "Image",  "variable de type Image cible"},
        {"options", "entier", "options de dessin (dEffacer par défaut)", NULL, &E_dessinOptions},
        {0}
    };
    static const OwlParam p_fin[] = {
        {"image", "Image", "image cible (optionnel — par défaut, libère la courante)"},
        {0}
    };
    static const OwlParam p_stylo[] = {
        {"image",   "Image",   "image cible (optionnel si dDébutDessin)"},
        {"couleur", "couleur", "couleur du trait (BGR sur 24 bits)"},
        {"style",   "entier",  "0 plein (défaut), 1 tiret, 2 pointillé, 3 tiret-point, 4 tiret-2-points, 5 invisible"},
        {"largeur", "entier",  "largeur du trait en pixels (défaut 1)"},
        {"opacité", "entier",  "opacité 0..255 (défaut 255)"},
        {0}
    };
    /* Forme courte : applique le stylo à la session dDébutDessin courante. */
    static const OwlParam p_stylo_session[] = {
        {"couleur", "couleur", "couleur du trait (BGR sur 24 bits)"},
        {"style",   "entier",  "0 plein (défaut), 1 tiret, 2 pointillé, 3 tiret-point, 4 tiret-2-points, 5 invisible"},
        {"largeur", "entier",  "largeur du trait en pixels (défaut 1)"},
        {"opacité", "entier",  "opacité 0..255 (défaut 255)"},
        {0}
    };
    static const OwlParam p_fond[] = {
        {"image",    "Image",   "image cible (optionnel si dDébutDessin)"},
        {"couleur",  "couleur", "couleur de fond (BGR sur 24 bits)"},
        {"style",    "entier",  "FondPlein (défaut), FondTransparent, FondHachuré", .allowedBase=&E_fondStyle},
        {"hachures", "entier",  "type de hachures 0..5 (utilisé si style=FondHachuré)"},
        {"opacité",  "entier",  "opacité 0..255 (défaut 255)"},
        {0}
    };
    static const OwlParam p_efface[] = {
        {"image",   "Image",   "image cible (optionnel si dDébutDessin)"},
        {"couleur", "couleur", "couleur d'effacement (défaut: CouleurRemplissage de l'image)"},
        {0}
    };
    /* Lot I.3b — formes droites. */
    static const OwlParam p_ligne[] = {
        {"image",   "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",      "entier",  "abscisse du point de départ"},
        {"y1",      "entier",  "ordonnée du point de départ"},
        {"x2",      "entier",  "abscisse du point d'arrivée"},
        {"y2",      "entier",  "ordonnée du point d'arrivée"},
        {"couleur", "couleur", "couleur du trait (défaut : stylo courant)"},
        {"largeur", "entier",  "largeur du trait en pixels (défaut : largeur du stylo)"},
        {0}
    };
    static const OwlParam p_rectangle[] = {
        {"image", "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",    "entier",  "abscisse du coin haut-gauche"},
        {"y1",    "entier",  "ordonnée du coin haut-gauche"},
        {"x2",    "entier",  "abscisse du coin bas-droit"},
        {"y2",    "entier",  "ordonnée du coin bas-droit"},
        {"fond",  "couleur", "couleur de remplissage (défaut : fond courant ; FondTransparent ne peint pas l'intérieur)"},
        {"trait", "couleur", "couleur du contour (défaut : stylo courant)"},
        {0}
    };
    /* Lot I.3c — formes courbes (cercle, arc, corde, portion). */
    static const OwlParam p_cercle[] = {
        {"image", "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",    "entier",  "abscisse du coin haut-gauche du rectangle englobant"},
        {"y1",    "entier",  "ordonnée du coin haut-gauche du rectangle englobant"},
        {"x2",    "entier",  "abscisse du coin bas-droit du rectangle englobant"},
        {"y2",    "entier",  "ordonnée du coin bas-droit du rectangle englobant"},
        {"fond",  "couleur", "couleur de remplissage (défaut : fond courant)"},
        {"trait", "couleur", "couleur du contour (défaut : stylo courant)"},
        {0}
    };
    static const OwlParam p_arc[] = {
        {"image",   "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",      "entier",  "abscisse du coin haut-gauche du rectangle englobant"},
        {"y1",      "entier",  "ordonnée du coin haut-gauche du rectangle englobant"},
        {"x2",      "entier",  "abscisse du coin bas-droit du rectangle englobant"},
        {"y2",      "entier",  "ordonnée du coin bas-droit du rectangle englobant"},
        {"x3",      "entier",  "abscisse du point de départ (projeté sur le contour)"},
        {"y3",      "entier",  "ordonnée du point de départ"},
        {"x4",      "entier",  "abscisse du point d'arrivée (projeté sur le contour)"},
        {"y4",      "entier",  "ordonnée du point d'arrivée"},
        {"couleur", "couleur", "couleur du trait (défaut : stylo courant)"},
        {"largeur", "entier",  "largeur du trait (défaut : largeur du stylo)"},
        {0}
    };
    static const OwlParam p_corde_portion[] = {
        {"image", "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",    "entier",  "abscisse du coin haut-gauche du rectangle englobant"},
        {"y1",    "entier",  "ordonnée du coin haut-gauche du rectangle englobant"},
        {"x2",    "entier",  "abscisse du coin bas-droit du rectangle englobant"},
        {"y2",    "entier",  "ordonnée du coin bas-droit du rectangle englobant"},
        {"x3",    "entier",  "abscisse du point de départ"},
        {"y3",    "entier",  "ordonnée du point de départ"},
        {"x4",    "entier",  "abscisse du point d'arrivée"},
        {"y4",    "entier",  "ordonnée du point d'arrivée"},
        {"fond",  "couleur", "couleur de remplissage (défaut : fond courant)"},
        {"trait", "couleur", "couleur du contour (défaut : stylo courant)"},
        {0}
    };
    /* Lot I.3d — polygones et flood fill. Les signatures variadic
     * acceptent les deux formes canon (count + paires, ou tableau). */
    static const OwlParam p_polygone[] = {
        {"image",       "Image",   "image cible (optionnel si dDébutDessin)"},
        {"points",      "varié",   "soit nombre de points puis paires x/y, soit tableau d'entiers [x1, y1, ..., xn, yn]"},
        {"fond",        "couleur", "couleur de remplissage (défaut : fond courant)"},
        {"trait",       "couleur", "couleur du contour (défaut : stylo courant)"},
        {0}
    };
    static const OwlParam p_polyligne[] = {
        {"image",       "Image",   "image cible (optionnel si dDébutDessin)"},
        {"points",      "varié",   "soit nombre de points puis paires x/y, soit tableau d'entiers [x1, y1, ..., xn, yn]"},
        {"couleur",     "couleur", "couleur du trait (défaut : stylo courant)"},
        {0}
    };
    static const OwlParam p_remplissage[] = {
        {"image",        "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x",            "entier",  "abscisse du point de départ"},
        {"y",            "entier",  "ordonnée du point de départ"},
        {"couleur",      "couleur", "couleur de remplissage (défaut : fond courant)"},
        {"bordure",      "couleur", "couleur de bordure : si fournie, le remplissage s'arrête à cette couleur (boundary fill) ; sinon, le remplissage s'étend à tous les pixels de même couleur que (x, y)"},
        {0}
    };
    /* Lot I.5 — effets et modifications de couleurs. */
    static const OwlParam p_inverse[] = {
        {"image", "Image", "image cible (transformation en place)"},
        {0}
    };
    static const OwlParam p_incruste[] = {
        {"image",   "Image",   "image cible (transformation en place)"},
        {"couleur", "couleur", "couleur à incruster sur l'image"},
        {"opacité", "réel",    "opacité 0..100 (en pourcent) — 0 sans effet, 100 remplace par la couleur (100 par défaut)"},
        {0}
    };
    static const OwlParam p_contraste[] = {
        {"image",     "Image",  "image cible (transformation en place)"},
        {"variation", "entier", "variation -100..+100 (-100 = gris uniforme, +100 = saturation max)"},
        {0}
    };
    static const OwlParam p_opacite[] = {
        {"image",   "Image", "image cible"},
        {"facteur", "réel",  "facteur en pourcent appliqué à l'alpha de chaque pixel (0 = transparent total, 100 = inchangé)"},
        {0}
    };
    static const OwlParam p_modif_tsl1[] = {
        {"image",     "Image",  "image cible"},
        {"variation", "entier", "variation à appliquer (en degrés pour la teinte, -100..+100 pour la saturation/luminosité)"},
        {0}
    };
    static const OwlParam p_modif_tsl3[] = {
        {"image",        "Image",  "image cible"},
        {"deltaTeinte",  "entier", "variation de teinte en degrés (-360..+360)"},
        {"deltaSat",     "entier", "variation de saturation -100..+100"},
        {"deltaLum",     "entier", "variation de luminosité -100..+100"},
        {0}
    };
    static const OwlParam p_flou[] = {
        {"image",  "Image",  "image cible"},
        {"rayon",  "entier", "rayon du flou en pixels (défaut 10)"},
        {0}
    };
    static const OwlParam p_couleur_dom[] = {
        {"image", "Image", "image source"},
        {0}
    };
    /* Lot I.4 — transformations. */
    static const OwlParam p_redim[] = {
        {"image",    "Image",  "image cible"},
        {"largeur",  "entier", "nouvelle largeur en pixels"},
        {"hauteur",  "entier", "nouvelle hauteur en pixels"},
        {"options",  "entier", "drEtiré (défaut), drHomothétique, drHauteQualité (combinables)", .allowedBase=&E_imgRedimOptions},
        {0}
    };
    static const OwlParam p_rotation[] = {
        {"image",    "Image",  "image cible"},
        {"angle",    "réel",   "angle en degrés (positif = sens horaire)"},
        {"options",  "entier", "drDéfaut (défaut, agrandit), drAdapte (réduit), drSansAgrandissement", .allowedBase=&E_imgRotationOptions},
        {0}
    };
    static const OwlParam p_decoupe[] = {
        {"image",    "Image",  "image cible"},
        {"x",        "entier", "abscisse du coin haut-gauche de la zone conservée"},
        {"y",        "entier", "ordonnée du coin haut-gauche de la zone conservée"},
        {"largeur",  "entier", "largeur de la zone conservée"},
        {"hauteur",  "entier", "hauteur de la zone conservée"},
        {0}
    };
    static const OwlParam p_copie[] = {
        {"source",      "Image",  "image source"},
        {"destination", "Image",  "image destination"},
        {"sx",          "entier", "abscisse dans la source (optionnel)"},
        {"sy",          "entier", "ordonnée dans la source"},
        {"sw",          "entier", "largeur de la zone source"},
        {"sh",          "entier", "hauteur de la zone source"},
        {"dx",          "entier", "abscisse dans la destination"},
        {"dy",          "entier", "ordonnée dans la destination"},
        {"dw",          "entier", "largeur de la zone destination (rééchantillonnage si différente de sw)"},
        {"dh",          "entier", "hauteur de la zone destination"},
        {0}
    };
    static const OwlParam p_fusionne[] = {
        {"destination", "Image",  "image destination (sera modifiée)"},
        {"source",      "Image",  "image source à fusionner par-dessus"},
        {"opacité",     "réel",   "opacité 0..100 (en pourcent) modulant l'alpha source (100 par défaut)"},
        {0}
    };
    /* Lot I.3e — texte. */
    static const OwlParam p_police[] = {
        {"image",        "Image",   "image cible (optionnel si dDébutDessin)"},
        {"police",        "chaîne",  "chemin vers un fichier de police TTF ou OTF"},
        {"taille",        "entier",  "taille en points (défaut 12)"},
        {"attributs",     "entier",  "iNormal, iGras, iItalique, iSouligné, iBarré (combinables)",
         NULL, &E_IStyle},
        {"inclinaison",   "entier",  "inclinaison en degrés (réservé pour usage futur)"},
        {0}
    };
    static const OwlParam p_texte[] = {
        {"image",         "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x",             "entier",  "abscisse du coin haut-gauche du texte"},
        {"y",             "entier",  "ordonnée du coin haut-gauche du texte"},
        {"texte",         "chaîne",  "chaîne UTF-8 à dessiner (les nombres sont auto-convertis)"},
        {"couleur",       "couleur", "couleur du texte (défaut : stylo courant)"},
        {"gestion_amp",   "entier",  "gestion du caractère & (réservé pour usage futur)"},
        {0}
    };
    static const OwlParam p_mesure_texte[] = {
        {"image",         "Image",   "image cible (optionnel si dDébutDessin)"},
        {"texte",         "chaîne",  "chaîne UTF-8 à mesurer (les nombres sont auto-convertis)"},
        {0}
    };
    static const OwlParam p_rect_degrade[] = {
        {"image",    "Image",   "image cible (optionnel si dDébutDessin)"},
        {"x1",       "entier",  "abscisse du coin haut-gauche"},
        {"y1",       "entier",  "ordonnée du coin haut-gauche"},
        {"x2",       "entier",  "abscisse du coin bas-droit"},
        {"y2",       "entier",  "ordonnée du coin bas-droit"},
        {"couleur1", "couleur", "couleur de début du dégradé"},
        {"couleur2", "couleur", "couleur de fin du dégradé"},
        {"angle",    "entier",  "angle du dégradé : 0 horizontal (défaut), 90 vertical, 180 et 270 inverses"},
        {0}
    };

    /*  : formes discrètes. L'init de déclaration typée
     * (`img est une Image = dChargeImage(...)`) résout désormais le retour
     * d'une native multi-formes. L'arité contrôle les params requis ; le
     * param array porte tous les params (les optionnels après le minimum). */
    static const OwlSignature sig_charge[]     = {
        {1, p_charge, "Image", "image chargée (Valide=Vrai si succès)"},
        {2, p_charge, "Image", "image chargée avec options"},
        {3, p_charge, "Image", "image chargée à la largeur cible (vectoriel)"},
        {4, p_charge, "Image", "image chargée à la taille cible (vectoriel)"},
        {0} };
    static const OwlSignature sig_cree[]       = {
        {2, p_cree, "Image", "nouvelle image (fond blanc par défaut)"},
        {3, p_cree, "Image", "nouvelle image (fond couleur fournie)"},
        {0} };
    static const OwlSignature sig_basic[]      = { { 2, p_save_basic, "booléen", "Vrai si succès"}, {0} };
    /* dSauveImagePNG(img, chemin [, couleurTransparence]) — 2 ou 3 args. */
    static const OwlSignature sig_save_png[]   = {
        { 2, p_save_png, "booléen", "Vrai si succès"},
        { 3, p_save_png, "booléen", "Vrai si succès (couleur de transparence appliquée)"},
        {0} };
    /* dSauveImageJPEG(img, chemin [, qualité]) — 2 ou 3 args. */
    static const OwlSignature sig_jpeg[]       = {
        { 2, p_save_jpeg, "booléen", "Vrai si succès (qualité par défaut 80)"},
        { 3, p_save_jpeg, "booléen", "Vrai si succès (qualité explicite)"},
        {0} };
    /* dPixelCouleur / dPixelOpacité([img,] x, y) — 2 (forme session) ou 3 (image explicite). */
    static const OwlSignature sig_pixel_read[] = {
        { 2, p_pixel_read, "entier", "valeur lue sur l'image courante (0 si hors limites)"},
        { 3, p_pixel_read, "entier", "valeur lue sur l'image explicite (0 si hors limites)"},
        {0} };
    /* dPoint([img,] x, y [, couleur [, opacité]]) — session 2..4, image explicite 3..5. */
    static const OwlSignature sig_dpoint[]     = {
        { 2, p_dpoint, "rien", "écrit le pixel sur l'image courante (couleur du stylo)"},
        { 3, p_dpoint, "rien", "écrit le pixel à (x,y)"},
        { 4, p_dpoint, "rien", "écrit le pixel à (x,y) (couleur ou image explicite)"},
        { 5, p_dpoint, "rien", "écrit le pixel à (x,y) (image, couleur et opacité explicites)"},
        {0} };
    /* dDébutDessin(img [, options]) — 1 ou 2 args. */
    static const OwlSignature sig_debut[]      = {
        { 1, p_debut, "booléen", "Vrai si dessin initialisé"},
        { 2, p_debut, "booléen", "Vrai si dessin initialisé (options explicites)"},
        {0} };
    /* dFinDessin([img]) — 0 ou 1 arg. */
    static const OwlSignature sig_fin[]        = {
        { 0, p_fin, "rien", "termine la session de dessin courante"},
        { 1, p_fin, "rien", "termine la session de dessin (image explicite)"},
        {0} };
    /* dStylo([img,] couleur [, style [, largeur [, opacité]]]) — session 1..4,
     * image explicite 2..5. Le param affiché reste la forme image explicite. */
    static const OwlSignature sig_stylo[]      = {
        { 1, p_stylo_session, "rien", "définit le stylo de la session dDébutDessin courante"},
        { 2, p_stylo, "rien", "définit le stylo (couleur ; ou image explicite + couleur)"},
        { 3, p_stylo, "rien", "définit le stylo (avec style)"},
        { 4, p_stylo, "rien", "définit le stylo (avec largeur)"},
        { 5, p_stylo, "rien", "définit le stylo de l'image cible explicite (avec opacité)"},
        {0} };
    /* dFond([img,] couleur [, style [, hachures [, opacité]]]) — session 1..4,
     * image explicite 2..5. */
    static const OwlSignature sig_fond[]       = {
        { 1, p_fond, "rien", "définit le fond de la session courante"},
        { 2, p_fond, "rien", "définit le fond (couleur ; ou image explicite + couleur)"},
        { 3, p_fond, "rien", "définit le fond (avec style)"},
        { 4, p_fond, "rien", "définit le fond (avec type de hachures)"},
        { 5, p_fond, "rien", "définit le fond courant (avec opacité)"},
        {0} };
    /* dEfface([img] [, couleur]) — session 0..1, image explicite 1..2. */
    static const OwlSignature sig_efface[]     = {
        { 0, p_efface, "rien", "efface la session courante avec sa couleur de remplissage"},
        { 1, p_efface, "rien", "efface avec une couleur unie (ou image courante)"},
        { 2, p_efface, "rien", "remplit l'image cible explicite avec une couleur unie"},
        {0} };
    /* dLigne([img,] x1, y1, x2, y2 [, couleur [, largeur]]) — session 4..6,
     * image explicite 5..7. */
    static const OwlSignature sig_ligne[]      = {
        { 4, p_ligne, "rien", "trace une ligne (couleur du stylo courant)"},
        { 5, p_ligne, "rien", "trace une ligne entre deux points"},
        { 6, p_ligne, "rien", "trace une ligne (couleur ou image explicite)"},
        { 7, p_ligne, "rien", "trace une ligne (image, couleur et largeur explicites)"},
        {0} };
    /* dRectangle([img,] x1, y1, x2, y2 [, fond [, trait]]) — session 4..6,
     * image explicite 5..7. */
    static const OwlSignature sig_rectangle[]  = {
        { 4, p_rectangle, "rien", "dessine un rectangle (fond + stylo courants)"},
        { 5, p_rectangle, "rien", "dessine un rectangle (avec fond)"},
        { 6, p_rectangle, "rien", "dessine un rectangle (fond + contour ; ou image explicite)"},
        { 7, p_rectangle, "rien", "dessine un rectangle de l'image explicite (fond + contour)"},
        {0} };
    /* dRectangleDégradé([img,] x1, y1, x2, y2, c1, c2 [, angle [, c3, d3 [, c4, d4]]]) —
     * session 6..11, image explicite 7..12. Les couleurs 3/4 (dégradés
     * multi-couleurs) sont acceptées mais non rendues par cette implémentation
     * (seul le dégradé 2 couleurs est tracé) — formes conservées pour la
     * compatibilité d'appel. */
    static const OwlSignature sig_rect_degrade[] = {
        { 6,  p_rect_degrade, "rien", "dessine un rectangle rempli avec un dégradé 2 couleurs"},
        { 7,  p_rect_degrade, "rien", "dessine un rectangle dégradé (angle ; ou image explicite)"},
        { 8,  p_rect_degrade, "rien", "dessine un rectangle dégradé (avec angle, image explicite)"},
        { 9,  p_rect_degrade, "rien", "dessine un rectangle dégradé (3e couleur)"},
        { 10, p_rect_degrade, "rien", "dessine un rectangle dégradé (3e couleur + distance)"},
        { 11, p_rect_degrade, "rien", "dessine un rectangle dégradé (4e couleur)"},
        { 12, p_rect_degrade, "rien", "dessine un rectangle dégradé (4e couleur + distance, image explicite)"},
        {0} };
    /* dCercle([img,] x1, y1, x2, y2 [, fond [, trait]]) — session 4..6,
     * image explicite 5..7. */
    static const OwlSignature sig_cercle[]         = {
        { 4, p_cercle, "rien", "dessine un cercle ou une ellipse (fond + stylo courants)"},
        { 5, p_cercle, "rien", "dessine un cercle ou une ellipse (avec fond)"},
        { 6, p_cercle, "rien", "dessine un cercle ou une ellipse (fond + contour ; ou image explicite)"},
        { 7, p_cercle, "rien", "dessine un cercle de l'image explicite (fond + contour)"},
        {0} };
    /* dArc([img,] x1, y1, x2, y2, x3, y3, x4, y4 [, couleur [, largeur]]) —
     * session 8..10, image explicite 9..11. */
    static const OwlSignature sig_arc[]            = {
        { 8,  p_arc, "rien", "trace un arc de cercle / d'ellipse (couleur du stylo)"},
        { 9,  p_arc, "rien", "trace un arc (couleur ; ou image explicite)"},
        { 10, p_arc, "rien", "trace un arc (couleur et largeur ; ou image explicite)"},
        { 11, p_arc, "rien", "trace un arc de l'image explicite (couleur et largeur)"},
        {0} };
    /* dCorde / dPortion([img,] x1, y1, x2, y2, x3, y3, x4, y4 [, fond [, trait]]) —
     * session 8..10, image explicite 9..11. */
    static const OwlSignature sig_corde_portion[]  = {
        { 8,  p_corde_portion, "rien", "dessine la zone bornée par l'arc (fond + stylo courants)"},
        { 9,  p_corde_portion, "rien", "dessine la zone bornée par l'arc (avec fond)"},
        { 10, p_corde_portion, "rien", "dessine la zone bornée par l'arc (fond + contour)"},
        { 11, p_corde_portion, "rien", "dessine la zone de l'image explicite (fond + contour)"},
        {0} };
    static const OwlSignature sig_polygone[]       = { {-1, p_polygone,    "rien", "dessine un polygone fermé (fond + contour)"}, {0} };
    static const OwlSignature sig_polyligne[]      = { {-1, p_polyligne,   "rien", "trace une ligne brisée non fermée"}, {0} };
    /* dRemplissage([img,] x, y [, couleur [, bordure]]) — session 2..4,
     * image explicite 3..5. */
    static const OwlSignature sig_remplissage[]    = {
        { 2, p_remplissage, "rien", "remplit une zone connexe à partir de (x,y) (fond courant)"},
        { 3, p_remplissage, "rien", "remplit une zone connexe (couleur ; ou image explicite)"},
        { 4, p_remplissage, "rien", "remplit une zone connexe (couleur et bordure ; ou image explicite)"},
        { 5, p_remplissage, "rien", "remplit une zone connexe de l'image explicite (couleur et bordure)"},
        {0} };
    /* dPolice([img,] police [, taille [, attributs [, inclinaison]]]) —
     * session 1..4, image explicite 2..5. La forme à enum (p_police) reste
     * en tête pour préserver la complétion sur les attributs. */
    static const OwlSignature sig_police[]         = {
        { 1, p_police, "booléen", "charge une police pour la session courante"},
        { 2, p_police, "booléen", "charge une police (taille ; ou image explicite)"},
        { 3, p_police, "booléen", "charge une police (taille et attributs)"},
        { 4, p_police, "booléen", "charge une police (taille, attributs, inclinaison)"},
        { 5, p_police, "booléen", "Vrai si la police a été chargée (image explicite)"},
        {0} };
    /* dTexte([img,] x, y, texte [, couleur [, gestion&]]) — session 3..5,
     * image explicite 4..6. */
    static const OwlSignature sig_texte[]          = {
        { 3, p_texte, "rien", "rend une chaîne avec la police courante (couleur du stylo)"},
        { 4, p_texte, "rien", "rend une chaîne (couleur ; ou image explicite)"},
        { 5, p_texte, "rien", "rend une chaîne (couleur et gestion & ; ou image explicite)"},
        { 6, p_texte, "rien", "rend une chaîne sur l'image explicite (couleur et gestion &)"},
        {0} };
    /* dLargeurTexte / dHauteurTexte([img,] texte) — 1 (session) ou 2 (image explicite). */
    static const OwlSignature sig_largeur_texte[]  = {
        { 1, p_mesure_texte, "entier", "largeur en pixels du texte dans la police courante"},
        { 2, p_mesure_texte, "entier", "largeur en pixels du texte (image explicite)"},
        {0} };
    static const OwlSignature sig_hauteur_texte[]  = {
        { 1, p_mesure_texte, "entier", "hauteur en pixels de la police courante"},
        { 2, p_mesure_texte, "entier", "hauteur en pixels de la police (image explicite)"},
        {0} };
    /* dRedimensionne(img, largeur, hauteur [, options]) — 3 ou 4 args. */
    static const OwlSignature sig_redim[]          = {
        { 3, p_redim, "booléen", "Vrai si redimensionnement effectué"},
        { 4, p_redim, "booléen", "Vrai si redimensionnement effectué (avec options)"},
        {0} };
    /* dRotation(img, angle [, options]) — 2 ou 3 args. */
    static const OwlSignature sig_rotation[]       = {
        { 2, p_rotation, "booléen", "Vrai si rotation effectuée"},
        { 3, p_rotation, "booléen", "Vrai si rotation effectuée (avec options)"},
        {0} };
    static const OwlSignature sig_decoupe[]        = { { 5, p_decoupe,    "Image",   "nouvelle image contenant la zone extraite (Valide=Faux si la zone est invalide)"}, {0} };
    /* dCopie(source, destination [, sx, sy, sw, sh [, dx, dy, dw, dh]]) —
     * 2 (toute la source), 6 (zone source), ou 10 (zone source + destination). */
    static const OwlSignature sig_copie[]          = {
        { 2,  p_copie, "booléen", "copie toute la source au coin haut-gauche de la destination"},
        { 6,  p_copie, "booléen", "copie une zone de la source"},
        { 10, p_copie, "booléen", "copie une zone source vers une zone destination (rééchantillonnage)"},
        {0} };
    /*  : formes discrètes {2,3}. L'argument manquant (dFusionne(img)
     * seul) est désormais rejeté au compile-time. Les gardes runtime
     * type-mismatch restent en place. */
    static const OwlSignature sig_fusionne[]       = {
        { 2, p_fusionne, "rien", "fusionne source dans destination"},
        { 3, p_fusionne, "rien", "fusionne source dans destination (opacité fournie)"},
        {0} };
    static const OwlSignature sig_inverse[]        = { { 1, p_inverse,    "booléen", "inversion réussie"}, {0} };
    /*  : formes discrètes {2,3}. L'argument manquant
     * (dIncrusteCouleur(img) seul) est désormais rejeté au compile-time. */
    static const OwlSignature sig_incruste[]       = {
        { 2, p_incruste, "rien", "applique la teinte (opacité 100 par défaut)"},
        { 3, p_incruste, "rien", "applique la teinte (opacité fournie)"},
        {0} };
    static const OwlSignature sig_contraste[]      = { { 2, p_contraste,  "booléen", "modification effectuée"}, {0} };
    static const OwlSignature sig_opacite[]        = { { 2, p_opacite,    "rien",    "alpha modifié"}, {0} };
    static const OwlSignature sig_modif_tsl1[]     = { { 2, p_modif_tsl1, "booléen", "modification effectuée"}, {0} };
    static const OwlSignature sig_modif_tsl3[]     = { { 4, p_modif_tsl3, "booléen", "modification effectuée"}, {0} };
    /* dFlou(img [, rayon]) — 1 ou 2 args. */
    static const OwlSignature sig_flou[]           = {
        { 1, p_flou, "rien", "flou appliqué (rayon par défaut 10)"},
        { 2, p_flou, "rien", "flou appliqué (rayon explicite)"},
        {0} };
    static const OwlSignature sig_couleur_dom[]    = { { 1, p_couleur_dom,"entier",  "couleur dominante (entier sur 24 bits, packing RVB)"}, {0} };

    /*  Lot 2 — params/sigs dCopieImage* migrent vers stdlib_ui.c. */

    static const OwlNativeDef DEFS[] = {
        {
            .canonical      = "dChargeImage",
            .nameEN         = "dLoadImage",
            .category       = "Image",
            .descFR         = "Charge une image depuis un fichier. Formats supportés : PNG, JPEG, BMP, GIF (premier frame uniquement), TGA, PSD. Le chemin peut être absolu ou relatif au répertoire d'exécution.",
            .descEN         = "Load an image from a file. Supported formats: PNG, JPEG, BMP, GIF (first frame only), TGA, PSD. Path may be absolute or relative to the executable directory.",
            .signatures     = sig_charge,
            .impl           = fn_dChargeImage,
        },
        {
            .canonical      = "dCr\xc3\xa9" "eImage",   /* dCréeImage */
            .canonicalAscii = "dCreeImage",
            .nameEN         = "dCreateImage",
            .category       = "Image",
            .descFR         = "Crée une nouvelle image aux dimensions données, remplie avec la couleur indiquée (Blanc par défaut). Le buffer pixels est alloué immédiatement et l'image est marquée Valide.",
            .descEN         = "Create a new image with the given dimensions, filled with the chosen color (white by default). The pixel buffer is allocated immediately and the image is marked Valid.",
            .signatures     = sig_cree,
            .impl           = fn_dCreeImage,
        },
        {
            .canonical      = "dSauveImagePNG",
            .nameEN         = "dSaveImagePNG",
            .category       = "Image",
            .descFR         = "Sauvegarde une image au format PNG (compression sans perte, canal alpha préservé). Une couleur de transparence optionnelle peut être passée en 3e argument : tous les pixels de cette couleur seront sauvés avec un alpha de 0 (transparent).",
            .descEN         = "Save an image as PNG (lossless compression, alpha channel preserved). An optional transparency color may be passed as 3rd argument: all pixels matching that color will be saved with an alpha of 0 (transparent).",
            .signatures     = sig_save_png,
            .impl           = fn_dSauveImagePNG,
        },
        {
            .canonical      = "dSauveImageJPEG",
            .nameEN         = "dSaveImageJPEG",
            .category       = "Image",
            .descFR         = "Sauvegarde une image au format JPEG (compression avec perte, canal alpha non préservé). Qualité 1..100 (80 par défaut).",
            .descEN         = "Save an image as JPEG (lossy compression, alpha channel not preserved). Quality 1..100 (default 80).",
            .signatures     = sig_jpeg,
            .impl           = fn_dSauveImageJPEG,
        },
        {
            .canonical      = "dSauveImageBMP",
            .nameEN         = "dSaveImageBMP",
            .category       = "Image",
            .descFR         = "Sauvegarde une image au format BMP (non compressé, canal alpha préservé).",
            .descEN         = "Save an image as BMP (uncompressed, alpha channel preserved).",
            .signatures     = sig_basic,
            .impl           = fn_dSauveImageBMP,
        },
        {
            .canonical      = "dSauveImage",
            .nameEN         = "dSaveImage",
            .category       = "Image",
            .descFR         = "Sauvegarde une image — le format est auto-détecté par l'extension du chemin (.png, .jpg/.jpeg, .bmp).",
            .descEN         = "Save an image — format auto-detected from path extension (.png, .jpg/.jpeg, .bmp).",
            .signatures     = sig_basic,
            .impl           = fn_dSauveImage,
        },
        {
            .canonical      = "dPixelCouleur",
            .nameEN         = "dPixelColor",
            .category       = "Image",
            .descFR         = "Lit la couleur d'un pixel à la position (x, y) et la renvoie comme entier BGR sur 24 bits. Les coordonnées sont en pixels, (0, 0) désignant le coin haut-gauche.",
            .descEN         = "Read a pixel's color at position (x, y) and return it as a 24-bit BGR integer. Coordinates are in pixels, with (0, 0) at the top-left corner.",
            .signatures     = sig_pixel_read,
            .impl           = fn_dPixelCouleur,
        },
        {
            .canonical      = "dPixelOpacit\xc3\xa9",   /* dPixelOpacité */
            .canonicalAscii = "dPixelOpacite",
            .nameEN         = "dPixelOpacity",
            .category       = "Image",
            .descFR         = "Lit l'opacité d'un pixel : valeur 0..255 du canal alpha (0 = transparent, 255 = opaque).",
            .descEN         = "Read a pixel's opacity: 0..255 alpha channel value (0 = transparent, 255 = opaque).",
            .signatures     = sig_pixel_read,
            .impl           = fn_dPixelOpacite,
        },
        {
            .canonical      = "dPoint",
            .nameEN         = "dDot",
            .category       = "Image",
            .descFR         = "Dessine un point coloré à la position (x, y). La couleur est requise tant qu'aucun stylo courant n'est défini via dStylo. Une opacité optionnelle 0..255 peut être passée en dernier argument.",
            .descEN         = "Draw a colored dot at position (x, y). The color is required when no current pen has been set via dPen. An optional opacity 0..255 can be passed as last argument.",
            .signatures     = sig_dpoint,
            .impl           = fn_dPoint,
        },
        {
            .canonical      = "dD\xc3\xa9" "butDessin",   /* dDébutDessin */
            .canonicalAscii = "dDebutDessin",
            .nameEN         = "dStartDrawing",
            .category       = "Image",
            .descFR         = "Initialise une session de dessin sur une image : celle-ci devient l'image courante des fonctions de dessin suivantes (dStylo, dFond, dPoint, etc.). Options : dEffacer (défaut), dSansEffacer, dAvecOpacité.",
            .descEN         = "Start a drawing session on an image: it becomes the current image for subsequent drawing calls (dPen, dBackground, dDot, etc.). Options: dErase (default), dNoErase, dWithOpacity.",
            .signatures     = sig_debut,
            .impl           = fn_dDebutDessin,
        },
        {
            .canonical      = "dFinDessin",
            .nameEN         = "dEndDrawing",
            .category       = "Image",
            .descFR         = "Termine la session de dessin courante. Les fonctions de dessin appelées ensuite sans argument image explicite renvoient une erreur.",
            .descEN         = "End the current drawing session. Drawing functions called afterwards without an explicit image argument will return an error.",
            .signatures     = sig_fin,
            .impl           = fn_dFinDessin,
        },
        {
            .canonical      = "dStylo",
            .nameEN         = "dPen",
            .category       = "Image",
            .descFR         = "Définit le stylo : couleur du trait, style (0 plein, 1 tiret, 2 pointillé, 3 tiret-point, 4 tiret-2-points, 5 invisible), largeur en pixels, opacité 0..255.",
            .descEN         = "Set the pen: line color, style (0 solid, 1 dash, 2 dot, 3 dash-dot, 4 dash-dot-dot, 5 invisible), width in pixels, opacity 0..255.",
            .signatures     = sig_stylo,
            .impl           = fn_dStylo,
        },
        {
            .canonical      = "dFond",
            .nameEN         = "dBackground",
            .category       = "Image",
            .descFR         = "Définit le fond : couleur de remplissage, style (FondPlein, FondTransparent, FondHachuré), type de hachures (0 horizontal, 1 vertical, 2 et 3 diagonal, 4 croisé, 5 diagonal croisé), opacité 0..255.",
            .descEN         = "Set the background: fill color, style (BackgroundSolid, BackgroundTransparent, BackgroundHatched), hatch type (0 horizontal, 1 vertical, 2 and 3 diagonal, 4 crossed, 5 diagonal crossed), opacity 0..255.",
            .signatures     = sig_fond,
            .impl           = fn_dFond,
        },
        {
            .canonical      = "dEfface",
            .nameEN         = "dErase",
            .category       = "Image",
            .descFR         = "Efface l'image en la remplissant d'une couleur unie. Si aucune couleur n'est passée, la propriété CouleurRemplissage de l'image est utilisée.",
            .descEN         = "Clear the image by filling it with a solid color. If no color is passed, the image's CouleurRemplissage property is used.",
            .signatures     = sig_efface,
            .impl           = fn_dEfface,
        },
        {
            .canonical      = "dLigne",
            .nameEN         = "dLine",
            .category       = "Image",
            .descFR         = "Trace une ligne entre deux points (x1, y1) et (x2, y2). Si la couleur n'est pas fournie, le stylo courant est utilisé. La largeur (en pixels) peut être passée explicitement ou héritée du stylo.",
            .descEN         = "Draw a line between two points (x1, y1) and (x2, y2). If the color is omitted, the current pen is used. Width (in pixels) may be passed explicitly or inherited from the pen.",
            .signatures     = sig_ligne,
            .impl           = fn_dLigne,
        },
        {
            .canonical      = "dRectangle",
            .nameEN         = "dRect",
            .category       = "Image",
            .descFR         = "Dessine un rectangle entre (x1, y1) et (x2, y2). Le fond est rempli avec la couleur passée ou avec le fond courant ; si le fond courant est FondTransparent, l'intérieur n'est pas peint. Le contour est tracé avec le stylo courant ou la couleur passée en 7e argument.",
            .descEN         = "Draw a rectangle between (x1, y1) and (x2, y2). The interior is filled with the given color or the current background; if the current background is FondTransparent, the interior is not painted. The outline uses the current pen, or the color passed as 7th argument.",
            .signatures     = sig_rectangle,
            .impl           = fn_dRectangle,
        },
        {
            .canonical      = "dRectangleD\xc3\xa9" "grad\xc3\xa9",   /* dRectangleDégradé */
            .canonicalAscii = "dRectangleDegrade",
            .nameEN         = "dGradientRect",
            .category       = "Image",
            .descFR         = "Remplit un rectangle avec un dégradé linéaire entre deux couleurs. L'angle (par défaut 0 = gauche vers droite) est normalisé au multiple de 90° le plus proche : 0 horizontal, 90 vertical, 180 et 270 inverses.",
            .descEN         = "Fill a rectangle with a linear gradient between two colors. The angle (default 0 = left to right) is normalized to the closest multiple of 90 degrees: 0 horizontal, 90 vertical, 180 and 270 reversed.",
            .signatures     = sig_rect_degrade,
            .impl           = fn_dRectangleDegrade,
        },
        {
            .canonical      = "dCercle",
            .nameEN         = "dCircle",
            .category       = "Image",
            .descFR         = "Dessine un cercle (ou une ellipse si la bbox n'est pas carrée) inscrit dans le rectangle (x1, y1)..(x2, y2). Fond rempli avec la couleur passée ou le fond courant ; contour tracé avec le stylo courant ou la couleur passée en 7e argument. Si le fond courant est FondTransparent, l'intérieur n'est pas peint.",
            .descEN         = "Draw a circle (or ellipse if the bbox is not square) inscribed in the rectangle (x1, y1)..(x2, y2). Interior filled with the given color or current background; outline drawn with the current pen or the color passed as 7th argument. If the current background is FondTransparent, the interior is not painted.",
            .signatures     = sig_cercle,
            .impl           = fn_dCercle,
        },
        {
            .canonical      = "dArc",
            .nameEN         = "dArc",
            .category       = "Image",
            .descFR         = "Trace un arc de cercle ou d'ellipse défini par le rectangle englobant (x1, y1)..(x2, y2) et les deux points (x3, y3) (départ) et (x4, y4) (arrivée). Si les points ne sont pas exactement sur le contour, ils sont projetés. L'arc est tracé en sens trigonométrique (anti-horaire) et n'est pas rempli.",
            .descEN         = "Draw a circular or elliptical arc defined by the bounding rectangle (x1, y1)..(x2, y2) and the two points (x3, y3) (start) and (x4, y4) (end). Points off the contour are projected onto it. The arc goes counter-clockwise (mathematical convention) and is not filled.",
            .signatures     = sig_arc,
            .impl           = fn_dArc,
        },
        {
            .canonical      = "dCorde",
            .nameEN         = "dChord",
            .category       = "Image",
            .descFR         = "Dessine la zone fermée bornée par l'arc défini par (x1, y1)..(x2, y2) entre (x3, y3) et (x4, y4), et par la corde droite reliant ces deux extrémités. Fond + contour suivent les mêmes conventions que dRectangle / dCercle.",
            .descEN         = "Draw the region bounded by the arc defined by (x1, y1)..(x2, y2) between (x3, y3) and (x4, y4), closed by the straight chord between the two endpoints. Fill + outline follow the same conventions as dRectangle / dCircle.",
            .signatures     = sig_corde_portion,
            .impl           = fn_dCorde,
        },
        {
            .canonical      = "dPortion",
            .nameEN         = "dPie",
            .category       = "Image",
            .descFR         = "Dessine une part de tarte : zone bornée par l'arc défini par (x1, y1)..(x2, y2) entre (x3, y3) et (x4, y4), et par les deux rayons reliant le centre à chacune de ces extrémités. Fond + contour suivent les mêmes conventions que dRectangle / dCercle.",
            .descEN         = "Draw a pie slice: region bounded by the arc defined by (x1, y1)..(x2, y2) between (x3, y3) and (x4, y4), and by the two radii from the center to each endpoint. Fill + outline follow the same conventions as dRectangle / dCircle.",
            .signatures     = sig_corde_portion,
            .impl           = fn_dPortion,
        },
        {
            .canonical      = "dPolygone",
            .nameEN         = "dPolygon",
            .category       = "Image",
            .descFR         = "Dessine un polygone fermé. Deux formes acceptées : (n, x1, y1, ..., xn, yn) avec le nombre de points en premier, ou un tableau d'entiers contenant directement les coordonnées entrelacées [x1, y1, x2, y2, ...]. Fond + contour suivent les conventions de dRectangle.",
            .descEN         = "Draw a closed polygon. Two accepted forms: (n, x1, y1, ..., xn, yn) with the point count first, or an integer array containing the interleaved coordinates [x1, y1, x2, y2, ...]. Fill + outline follow dRectangle conventions.",
            .signatures     = sig_polygone,
            .impl           = fn_dPolygone,
        },
        {
            .canonical      = "dPolyligne",
            .nameEN         = "dPolyline",
            .category       = "Image",
            .descFR         = "Trace une ligne brisée non fermée passant par les points fournis. Deux formes acceptées comme dPolygone (nombre de points + paires, ou tableau).",
            .descEN         = "Draw an open polyline through the given points. Two accepted forms like dPolygon (point count + pairs, or array).",
            .signatures     = sig_polyligne,
            .impl           = fn_dPolyligne,
        },
        {
            .canonical      = "dRemplissage",
            .nameEN         = "dFloodFill",
            .category       = "Image",
            .descFR         = "Remplit une zone à partir du pixel (x, y). Sans couleur de bordure, étend le remplissage à tous les pixels connexes de même couleur que (x, y) (flood fill). Avec une couleur de bordure, étend le remplissage tant que la couleur de bordure n'est pas rencontrée (boundary fill).",
            .descEN         = "Fill a region starting from the pixel (x, y). Without a border color, extends the fill to all connected pixels having the same color as (x, y) (flood fill). With a border color, extends until the border color is reached (boundary fill).",
            .signatures     = sig_remplissage,
            .impl           = fn_dRemplissage,
        },
        {
            .canonical      = "dPolice",
            .nameEN         = "dFont",
            .category       = "Image",
            .descFR         = "Charge une police de caractères TTF ou OTF pour le rendu de texte. Le 1er argument est le chemin du fichier de police. Taille en points (défaut 12), attributs iNormal/iGras/iItalique/iSouligné/iBarré combinables. La police reste active jusqu'au prochain appel à dPolice ou jusqu'à dFinDessin.",
            .descEN         = "Load a TTF or OTF font for text rendering. The 1st argument is the font file path. Size in points (default 12), attributes iNormal/iBold/iItalic/iUnderlined/iStrikeOut are combinable. The font stays active until the next call to dFont or until dEndDrawing.",
            .signatures     = sig_police,
            .impl           = fn_dPolice,
        },
        {
            .canonical      = "dTexte",
            .nameEN         = "dText",
            .category       = "Image",
            .descFR         = "Dessine une chaîne UTF-8 à la position (x, y) qui désigne le coin haut-gauche du texte. Une police doit avoir été chargée avec dPolice. Si la couleur n'est pas passée, le stylo courant est utilisé. Les nombres passés en lieu et place du texte sont auto-convertis en chaîne.",
            .descEN         = "Draw a UTF-8 string at position (x, y) which designates the top-left corner of the text. A font must have been loaded via dFont. If the color is omitted, the current pen is used. Numeric values passed instead of text are auto-converted to string.",
            .signatures     = sig_texte,
            .impl           = fn_dTexte,
        },
        {
            .canonical      = "dLargeurTexte",
            .nameEN         = "dTextWidth",
            .category       = "Image",
            .descFR         = "Retourne la largeur en pixels de la chaîne dans la police courante de l'image (kerning inclus). Une police doit avoir été chargée avec dPolice. Une chaîne vide retourne 0. Utile pour centrer ou aligner du texte sur une image.",
            .descEN         = "Return the width in pixels of the string in the image's current font (kerning included). A font must have been loaded via dFont. An empty string returns 0. Useful for centering or aligning text on an image.",
            .signatures     = sig_largeur_texte,
            .impl           = fn_dLargeurTexte,
        },
        {
            .canonical      = "dHauteurTexte",
            .nameEN         = "dTextHeight",
            .category       = "Image",
            .descFR         = "Retourne la hauteur en pixels de la police courante de l'image. La hauteur dépend de la police chargée (taille en points + métriques ascent/descent), pas du contenu de la chaîne — celle-ci est néanmoins requise pour la symétrie de signature avec dLargeurTexte.",
            .descEN         = "Return the height in pixels of the image's current font. The height depends on the loaded font (point size + ascent/descent metrics), not on the string content — which is still required for signature symmetry with dTextWidth.",
            .signatures     = sig_hauteur_texte,
            .impl           = fn_dHauteurTexte,
        },
        {
            .canonical      = "dRedimensionne",
            .nameEN         = "dResize",
            .category       = "Image",
            .descFR         = "Redimensionne l'image en place aux nouvelles dimensions. Options : drEtiré (étire les pixels, défaut), drHomothétique (préserve les proportions et remplit le reste avec CouleurRemplissage), drHauteQualité (interpolation bilinéaire au lieu du plus proche voisin).",
            .descEN         = "Resize the image in place to the given dimensions. Options: drStretched (stretch pixels, default), drHomothetic (preserve aspect ratio and fill remainder with CouleurRemplissage), drHighQuality (bilinear interpolation instead of nearest neighbor).",
            .signatures     = sig_redim,
            .impl           = fn_dRedimensionne,
        },
        {
            .canonical      = "dRotation",
            .nameEN         = "dRotate",
            .category       = "Image",
            .descFR         = "Tourne l'image autour de son centre par l'angle indiqué (positif = sens horaire). Options : drDéfaut (agrandit pour contenir, défaut), drAdapte (tourne puis réduit à la taille initiale), drSansAgrandissement (garde la taille initiale, l'image tournée est tronquée).",
            .descEN         = "Rotate the image around its center by the given angle (positive = clockwise). Options: drDefault (enlarge to contain, default), drAdapt (rotate then shrink back to original size), drNoEnlarge (keep original size, rotated image is clipped).",
            .signatures     = sig_rotation,
            .impl           = fn_dRotation,
        },
        {
            .canonical      = "dD\xc3\xa9" "coupe",   /* dDécoupe */
            .canonicalAscii = "dDecoupe",
            .nameEN         = "dCrop",
            .category       = "Image",
            .descFR         = "Extrait une zone rectangulaire d'une image source et la retourne comme nouvelle image. L'image source reste inchangée (opération non-mutative). Les pixels hors limites de la source mais à l'intérieur de la zone demandée sont remplis avec CouleurRemplissage.",
            .descEN         = "Extract a rectangular region from a source image and return it as a new image. The source image is left unchanged (non-mutating operation). Pixels outside the source bounds but inside the requested region are filled with CouleurRemplissage.",
            .signatures     = sig_decoupe,
            .impl           = fn_dDecoupe,
        },
        {
            .canonical      = "dCopie",
            .nameEN         = "dCopy",
            .category       = "Image",
            .descFR         = "Copie tout ou partie d'une image source dans une image destination. Sans paramètres optionnels, copie l'intégralité de la source au coin haut-gauche de la destination. Avec des coordonnées explicites, blit avec rééchantillonnage bilinéaire si la zone destination a des dimensions différentes de la zone source.",
            .descEN         = "Copy all or part of a source image into a destination image. Without optional parameters, copies the whole source at the top-left of the destination. With explicit coordinates, performs a blit with bilinear resampling if the destination zone has different dimensions than the source zone.",
            .signatures     = sig_copie,
            .impl           = fn_dCopie,
        },
        {
            .canonical      = "dFusionne",
            .nameEN         = "dMerge",
            .category       = "Image",
            .descFR         = "Fusionne l'image source dans l'image destination par alpha-blending source-over. L'opacité globale 0..100 (en pourcent) module l'alpha source (par défaut 100 = source opaque). La taille de la zone fusionnée est le minimum de chaque dimension entre source et destination, alignée au coin haut-gauche.",
            .descEN         = "Merge the source image into the destination using source-over alpha-blending. The global opacity 0..100 (in percent) modulates the source alpha (default 100 = source opaque). The merged area size is the minimum of each dimension between source and destination, aligned to the top-left corner.",
            .signatures     = sig_fusionne,
            .impl           = fn_dFusionne,
        },
        {
            .canonical      = "dInverseCouleur",
            .nameEN         = "dInvertColor",
            .category       = "Image",
            .descFR         = "Inverse les composantes R/G/B de chaque pixel (255 - valeur). Le canal alpha est préservé.",
            .descEN         = "Invert the R/G/B components of each pixel (255 - value). The alpha channel is preserved.",
            .signatures     = sig_inverse,
            .impl           = fn_dInverseCouleur,
        },
        {
            .canonical      = "dIncrusteCouleur",
            .nameEN         = "dColorize",
            .category       = "Image",
            .descFR         = "Applique une teinte uniforme sur l'image. L'opacité 0..100 (en pourcent, défaut 100) contrôle l'intensité du mélange (100 remplace tous les pixels par la couleur, 0 ne change rien).",
            .descEN         = "Apply a uniform tint to the image. The 0..100 opacity (in percent, default 100) controls blend intensity (100 replaces every pixel with the color, 0 leaves it unchanged).",
            .signatures     = sig_incruste,
            .impl           = fn_dIncrusteCouleur,
        },
        {
            .canonical      = "dModifieContraste",
            .nameEN         = "dModifyContrast",
            .category       = "Image",
            .descFR         = "Modifie le contraste de l'image. La variation -100..+100 décale chaque composante autour de la valeur médiane 128 : -100 produit une image grise uniforme, +100 amplifie la séparation des tons.",
            .descEN         = "Modify the image contrast. Variation -100..+100 shifts each component around the median value 128: -100 produces a uniformly gray image, +100 amplifies tone separation.",
            .signatures     = sig_contraste,
            .impl           = fn_dModifieContraste,
        },
        {
            .canonical      = "dModifieOpacit\xc3\xa9",   /* dModifieOpacité */
            .canonicalAscii = "dModifieOpacite",
            .nameEN         = "dModifyOpacity",
            .category       = "Image",
            .descFR         = "Multiplie le canal alpha de chaque pixel par le facteur fourni (0..100 en pourcent : 0 = transparent total, 100 = inchangé). Hors plage : erreur. Permet d'éclaircir progressivement une image avant fusion.",
            .descEN         = "Multiply each pixel's alpha channel by the given factor (0..100 in percent: 0 = fully transparent, 100 = unchanged). Out of range raises an error. Useful to progressively fade an image before merging.",
            .signatures     = sig_opacite,
            .impl           = fn_dModifieOpacite,
        },
        {
            .canonical      = "dModifieSaturation",
            .nameEN         = "dModifySaturation",
            .category       = "Image",
            .descFR         = "Ajuste la saturation des couleurs (-100 = gris pur, +100 = couleurs maximales). La conversion passe par l'espace HSL pixel par pixel.",
            .descEN         = "Adjust color saturation (-100 = pure gray, +100 = maximum saturation). Conversion goes through HSL space pixel by pixel.",
            .signatures     = sig_modif_tsl1,
            .impl           = fn_dModifieSaturation,
        },
        {
            .canonical      = "dModifieTeinte",
            .nameEN         = "dModifyHue",
            .category       = "Image",
            .descFR         = "Décale la teinte de chaque pixel par un nombre de degrés (modulo 360). +120 transforme un rouge en vert, +240 en bleu.",
            .descEN         = "Shift each pixel's hue by the given number of degrees (modulo 360). +120 turns red into green, +240 into blue.",
            .signatures     = sig_modif_tsl1,
            .impl           = fn_dModifieTeinte,
        },
        {
            .canonical      = "dModifieTSL",
            .nameEN         = "dModifyHSL",
            .category       = "Image",
            .descFR         = "Combine les trois variations Teinte/Saturation/Luminosité en une seule passe. Teinte en degrés, saturation et luminosité en pourcentage -100..+100.",
            .descEN         = "Combine the three Hue/Saturation/Lightness variations in a single pass. Hue in degrees, saturation and lightness in percent -100..+100.",
            .signatures     = sig_modif_tsl3,
            .impl           = fn_dModifieTSL,
        },
        {
            .canonical      = "dFlou",
            .nameEN         = "dBlur",
            .category       = "Image",
            .descFR         = "Applique un flou box-blur à l'image. Le rayon en pixels (par défaut 10) contrôle la largeur de la moyenne de voisinage : un rayon plus grand donne un flou plus prononcé. Rayon 0 = aucun effet. Rayon négatif = erreur.",
            .descEN         = "Apply a box-blur effect to the image. The radius in pixels (default 10) controls the neighborhood averaging width: a larger radius yields a stronger blur. Radius 0 = no effect. Negative radius raises an error.",
            .signatures     = sig_flou,
            .impl           = fn_dFlou,
        },
        {
            .canonical      = "CouleurDominanteImage",
            .nameEN         = "DominantColorImage",
            .category       = "Image",
            .descFR         = "Renvoie la couleur dominante de l'image (entier sur 24 bits, packing standard RVB) : la couleur RVB la plus fréquente parmi les pixels opaques. En cas d'égalité, la première rencontrée gagne.",
            .descEN         = "Return the image dominant color (24-bit integer in standard RGB packing): the most frequent RGB color among opaque pixels. On tie, the first encountered wins.",
            .signatures     = sig_couleur_dom,
            .impl           = fn_CouleurDominanteImage,
        },
        /*  Lot 2 — dCopieImageFenêtre / dCopieImageChamp /
         * dCopieImageEcran ont migre vers stdlib_ui.c (besoin Qt). */
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_consts(IMG_CONSTS, (int)(sizeof(IMG_CONSTS)/sizeof(IMG_CONSTS[0])));
    owl_registry_register_enums(IMG_ENUMS, (int)(sizeof(IMG_ENUMS)/sizeof(IMG_ENUMS[0])));

    /* Enregistre le parser de coerce dans le registre central. Le
     * compilo emettra __InstanceCoerce(v, "Image") qui dispatch ici. */
    owl_register_instance_coerce("Image", parse_to_image);

    /*  : enregistre g_current_instance au registre unique. */
    owl_register_persistent_obj_root((Obj**)&g_current_instance);
}

void stdlib_image_reset_globals(void) {
    g_current_instance = NULL;
}
