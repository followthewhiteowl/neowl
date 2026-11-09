// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr_embed.c - Bake des modeles tessdata (FR+EN) dans le binaire OCR,
 * via .incbin en asm de haut niveau (compile comme du C ordinaire par zig cc).
 *
 * Les chemins arrivent en macros definies au build (CMake,
 * target_compile_definitions) puis stringifiees ici -> zero chemin absolu en dur
 * dans le repo. Les .traineddata deviennent de la .rodata servie par le
 * FileReader Tesseract (stdlib_ocr_engine.cpp), sans acces disque (pilier 1
 * all-include). Compile uniquement sous OWL_HAS_OCR. */

#define OWL_STR2(x) #x
#define OWL_STR(x) OWL_STR2(x)

__asm__(
    ".section .rodata\n"
    ".global owl_ocr_eng_traineddata\n"
    ".global owl_ocr_eng_traineddata_end\n"
    ".balign 8\n"
    "owl_ocr_eng_traineddata:\n"
    ".incbin \"" OWL_STR(OWL_OCR_ENG_TRAINEDDATA) "\"\n"
    "owl_ocr_eng_traineddata_end:\n"

    ".global owl_ocr_fra_traineddata\n"
    ".global owl_ocr_fra_traineddata_end\n"
    ".balign 8\n"
    "owl_ocr_fra_traineddata:\n"
    ".incbin \"" OWL_STR(OWL_OCR_FRA_TRAINEDDATA) "\"\n"
    "owl_ocr_fra_traineddata_end:\n"
);
