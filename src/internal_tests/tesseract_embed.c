/* Bake des modeles tessdata_fast (FR+EN) et des fixtures image dans le binaire
 * de smoke test OCR, via .incbin en asm de haut niveau (compile comme du C
 * ordinaire par zig cc -> pas d'activation du langage ASM cote CMake).
 *
 * Les chemins arrivent en macros definies au build (CMake, target_compile_
 * definitions) puis stringifiees ici -> zero chemin absolu en dur dans le repo
 * (contrairement au POC jetable). Preuve du mecanisme all-include (pilier 1) :
 * les .traineddata deviennent de la .rodata servie par un FileReader Tesseract,
 * sans acces disque. Reutilisable tel quel par le chantier natif OCR.
 */
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

    ".global owl_ocr_img_en\n"
    ".global owl_ocr_img_en_end\n"
    ".balign 8\n"
    "owl_ocr_img_en:\n"
    ".incbin \"" OWL_STR(OWL_OCR_IMG_EN) "\"\n"
    "owl_ocr_img_en_end:\n"

    ".global owl_ocr_img_fr\n"
    ".global owl_ocr_img_fr_end\n"
    ".balign 8\n"
    "owl_ocr_img_fr:\n"
    ".incbin \"" OWL_STR(OWL_OCR_IMG_FR) "\"\n"
    "owl_ocr_img_fr_end:\n"
);
