// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_ocr_engine.cpp - Backend OCR Tesseract (build OWL_HAS_OCR uniquement).
 *
 * Enveloppe l'API C++ de Tesseract derriere la surface C de stdlib_ocr_engine.h.
 * Decodage image via stb_image (implementation fournie par owl_core -> on inclut
 * seulement les declarations, pas STB_IMAGE_IMPLEMENTATION, pour eviter un
 * double symbole au link). Modeles eng+fra bakes en .rodata (stdlib_ocr_embed.c),
 * servis par un FileReader in-memory -> zero fichier externe. Codecs Leptonica
 * OFF : on passe des octets gris bruts a SetImage. */

#include "stdlib_ocr_engine.h"

/* La build OWL definit HAVE_CONFIG_H globalement (en-tetes d'autres vendors).
 * Les headers publics Tesseract installes sont deja configures ; sous
 * HAVE_CONFIG_H ils tirent config_auto.h (interne au build Tesseract, non
 * livre). On le neutralise pour cette unite seulement. */
#ifdef HAVE_CONFIG_H
#  undef HAVE_CONFIG_H
#endif

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#include "stb_image.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* Blobs bakes (stdlib_ocr_embed.c, .incbin pilote par macros CMake). */
extern "C" const unsigned char owl_ocr_eng_traineddata[];
extern "C" const unsigned char owl_ocr_eng_traineddata_end[];
extern "C" const unsigned char owl_ocr_fra_traineddata[];
extern "C" const unsigned char owl_ocr_fra_traineddata_end[];

/* FileReader : sert les blobs bakes depuis la .rodata selon le basename demande. */
static bool owl_ocr_baked_reader(const char* path, std::vector<char>* out) {
    std::string p(path);
    const unsigned char* b = nullptr;
    const unsigned char* e = nullptr;
    if (p.find("eng.traineddata") != std::string::npos) {
        b = owl_ocr_eng_traineddata; e = owl_ocr_eng_traineddata_end;
    } else if (p.find("fra.traineddata") != std::string::npos) {
        b = owl_ocr_fra_traineddata; e = owl_ocr_fra_traineddata_end;
    }
    if (b == nullptr) return false;
    out->assign(reinterpret_cast<const char*>(b), reinterpret_cast<const char*>(e));
    return true;
}

extern "C" int owl_ocr_extract_text_from_file(const char* path, char** out_text) {
    *out_text = nullptr;

    int w = 0, h = 0, chan = 0;
    unsigned char* gray = stbi_load(path, &w, &h, &chan, 1 /* force gris 8bpp */);
    if (gray == nullptr) return -1;

    /* Codecs Leptonica OFF -> diagnostics cosmetiques possibles ; on coupe sa
     * console, l'echec reel remonte via le texte vide / l'assertion appelante. */
    setMsgSeverity(L_SEVERITY_NONE);

    tesseract::TessBaseAPI api;
    /* eng+fra charges ensemble -> FR ou EN reconnu sans parametre de langue. */
    int rc = api.Init(nullptr, 0, "eng+fra", tesseract::OEM_LSTM_ONLY,
                      nullptr, 0, nullptr, nullptr, false, owl_ocr_baked_reader);
    if (rc != 0) {
        stbi_image_free(gray);
        return -2;
    }

    api.SetImage(gray, w, h, 1 /* bpp */, w /* bytes per line */);
    api.SetSourceResolution(150);
    char* txt = api.GetUTF8Text();
    std::string s = txt ? txt : "";
    delete[] txt;
    api.End();
    stbi_image_free(gray);

    char* buf = static_cast<char*>(malloc(s.size() + 1));
    if (buf == nullptr) return -3;
    memcpy(buf, s.c_str(), s.size() + 1);
    *out_text = buf;
    return 0;
}
