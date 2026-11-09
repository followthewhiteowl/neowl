// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr.c - Glue native des fonctions OCR (build OWL_HAS_OCR uniquement).
 *
 * Arg parsing / erreurs OWL / valeur de retour cote C ; le decodage image +
 * l'inference Tesseract vivent dans stdlib_ocr_engine.cpp (frontiere C via
 * stdlib_ocr_engine.h). Les impls sont injectees dans l'array DEFS Qt/Tesseract-
 * free (stdlib_ocr_defs.c) par stdlib_ocr_impls_wire(), appele sous OWL_HAS_OCR
 * avant stdlib_ocr_defs_register() (owl_stdlib.c). */

#include "stdlib_ocr.h"
#include "stdlib_ocr_defs.h"
#include "stdlib_ocr_engine.h"
#include "object.h"
#include "owl_errors.h"
#include "owl_stdlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Value ocr_empty_string(void) {
    return OBJ_VAL(obj_copy_string("", 0));
}

/* OCRExtraitTexte(<chemin image : chaîne>) -> chaîne.
 *
 * Reconnait le texte imprimé d'une image (modeles eng+fra bakes). Chemin absent
 * ou image illisible : pose un soft-error (ErreurDétectée) et renvoie "" (pas de
 * resultat silencieux, pas de plantage). */
static Value fn_ocr_extrait_texte(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "OCRExtraitTexte",
                              "native", "OCRExtraitTexte", "arg", "image",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return ocr_empty_string();
    }

    const char* chemin = AS_CSTRING(args[0]);

    /* Existence verifiee cote C pour un message explicite (le moteur ne
     * distinguerait qu'un decode rate generique). */
    FILE* f = fopen(chemin, "rb");
    if (f == NULL) {
        owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, "OCRExtraitTexte",
                              "native", "OCRExtraitTexte",
                              "path", chemin, "detail", "fichier introuvable", NULL);
        return ocr_empty_string();
    }
    fclose(f);

    char* texte = NULL;
    int rc = owl_ocr_extract_text_from_file(chemin, &texte);
    if (rc != 0 || texte == NULL) {
        free(texte);
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "OCRExtraitTexte",
                              "native", "OCRExtraitTexte", "path", chemin,
                              "detail", "image illisible ou non décodable", NULL);
        return ocr_empty_string();
    }

    Value result = OBJ_VAL(obj_copy_string(texte, (int)strlen(texte)));
    free(texte);
    owl_clear_error();
    return result;
}

void stdlib_ocr_impls_wire(void) {
    stdlib_ocr_defs_set_impl("OCRExtraitTexte", fn_ocr_extrait_texte);
}
