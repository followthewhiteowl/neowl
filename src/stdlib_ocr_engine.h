// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr_engine.h - Frontiere C/C++ du moteur OCR.
 *
 * L'API Tesseract est C++ : stdlib_ocr_engine.cpp l'enveloppe derriere cette
 * surface C stable, appelee par la glue native stdlib_ocr.c (C). Compile et
 * link uniquement sous OWL_HAS_OCR. */

#ifndef OWL_STDLIB_OCR_ENGINE_H
#define OWL_STDLIB_OCR_ENGINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode l'image `path` (via stb_image), lance l'OCR (modeles eng+fra bakes) et
 * ecrit dans *out_text un buffer UTF-8 alloue par malloc() (a liberer par
 * l'appelant). Retour :
 *    0  succes (*out_text non-NULL, meme pour un texte vide)
 *   -1  image illisible / format non decode
 *   -2  initialisation du moteur OCR echouee
 *   -3  allocation memoire echouee
 * En cas d'echec, *out_text vaut NULL. */
int owl_ocr_extract_text_from_file(const char* path, char** out_text);

#ifdef __cplusplus
}
#endif

#endif
