// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr_defs.c - Donnees de registre des natives OCR (OCRExtraitTexte, ...).
 *
 * Partie Tesseract-FREE du module OCR (cf. stdlib_ocr_defs.h). Contient l'array
 * DEFS + ses tables OwlParam/OwlSignature, SANS aucune implementation C : chaque
 * entree a .impl=NULL a la declaration.
 *
 * - Build OCR (owl.exe, OWL_HAS_OCR) : stdlib_ocr_impls_wire() (stdlib_ocr.c)
 *   remplit les .impl AVANT stdlib_ocr_defs_register() -> register_module voit
 *   des impls non-NULL -> chemin normal.
 * - Build sans OCR : les .impl restent NULL. Le compilateur consulte uniquement
 *   les signatures/arite (jamais .impl), donc la resolution est identique. Un
 *   appel runtime sur une native a impl=NULL remonte une erreur claire. */

#include "stdlib_ocr_defs.h"
#include "owl_stdlib.h"       /* owl_registry_register_module */
#include "owl_registry.h"
#include "owl_errors.h"

#include <string.h>

/* Stub par defaut (build sans OCR) : la native est enregistree partout, mais
 * sans moteur son .impl pointe ici -> erreur runtime claire au lieu d'un
 * dispatch sur NULL. Sous OWL_HAS_OCR, stdlib_ocr_impls_wire() remplace ce
 * pointeur par l'impl reelle (stdlib_ocr.c). */
static Value fn_ocr_unavailable(int argCount, Value* args) {
    (void)argCount; (void)args;
    owl_emit_native_error(E_NATIVE_OCR_NOT_IN_BUILD, "OCRExtraitTexte",
                          "native", "OCRExtraitTexte", NULL);
    return OBJ_VAL(obj_copy_string("", 0));
}

static const OwlParam p_extrait[] = {
    {"image", "chaîne", "Chemin du fichier image a analyser (PNG, JPEG, BMP, ...)"},
    {0}
};
static const OwlSignature sig_extrait[] = {
    {1, p_extrait, "chaîne", "Texte imprimé reconnu dans l'image (vide si aucun)"},
    {0}
};

/* DEFS non-const : les .impl sont poses par stdlib_ocr_defs_set_impl() (build
 * OCR) avant stdlib_ocr_defs_register(). Sinon .impl reste NULL. */
static OwlNativeDef DEFS[] = {
    { .canonical="OCRExtraitTexte", .nameEN="OCRExtractText",
      .category="OCR",
      .descFR="Lit le texte imprimé contenu dans une image et le renvoie sous forme de chaîne, à partir d'un chemin de fichier image. Le français et l'anglais sont reconnus sans réglage.",
      .descEN="Reads the printed text contained in an image and returns it as a string, from an image file path. French and English are recognized with no setup.",
      .signatures=sig_extrait, .impl=fn_ocr_unavailable },
};

void stdlib_ocr_defs_set_impl(const char* canonical, NativeFn fn) {
    if (canonical == NULL) return;
    int n = (int)(sizeof(DEFS) / sizeof(DEFS[0]));
    for (int i = 0; i < n; i++) {
        OwlNativeDef* d = &DEFS[i];
        if (strcmp(d->canonical, canonical) == 0 ||
            (d->canonicalAscii && strcmp(d->canonicalAscii, canonical) == 0)) {
            d->impl = fn;
            return;
        }
    }
}

void stdlib_ocr_defs_register(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
}
