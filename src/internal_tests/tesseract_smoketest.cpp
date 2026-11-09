/* Smoke test OCR (gate OWL_HAS_OCR) : prouve que la chaine Leptonica+Tesseract
 * vendoree statique OCR-ise reellement sous notre toolchain (zig clang+libc++),
 * dans les DEUX modes canon :
 *
 *   - "bake"     : modeles tessdata_fast embarques en .rodata (incbin, cf
 *                  tesseract_embed.S), servis via le FileReader in-memory de
 *                  Tesseract -> zero fichier externe (pilier 1 all-include).
 *   - "override" : modeles ecrits sur disque dans un repertoire user, lus par
 *                  Init(dir, lang) (canon OCRRepertoireModele).
 *
 * Decodage image : stb_image (deja vendore) -> gris 8bpp -> SetImage octets
 * bruts. Aucune lib de codec image cote Leptonica (tous codecs OFF au build).
 *
 * Standalone : ne linke pas owl_core (pas de double STB_IMAGE_IMPLEMENTATION).
 * Cible la surface publique extern "C" de Tesseract ; le C++ reste interne aux .a.
 */
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <direct.h>
#  define OWL_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define OWL_MKDIR(p) mkdir((p), 0777)
#endif

/* Blobs bakes (tesseract_embed.S, .incbin pilotes par macros CMake). */
extern "C" const unsigned char owl_ocr_eng_traineddata[];
extern "C" const unsigned char owl_ocr_eng_traineddata_end[];
extern "C" const unsigned char owl_ocr_fra_traineddata[];
extern "C" const unsigned char owl_ocr_fra_traineddata_end[];
extern "C" const unsigned char owl_ocr_img_en[];
extern "C" const unsigned char owl_ocr_img_en_end[];
extern "C" const unsigned char owl_ocr_img_fr[];
extern "C" const unsigned char owl_ocr_img_fr_end[];

/* FileReader : sert les blobs bakes depuis la .rodata selon le basename demande. */
static bool baked_reader(const char *path, std::vector<char> *out) {
    std::string p(path);
    const unsigned char *b = nullptr, *e = nullptr;
    if (p.find("eng.traineddata") != std::string::npos) {
        b = owl_ocr_eng_traineddata; e = owl_ocr_eng_traineddata_end;
    } else if (p.find("fra.traineddata") != std::string::npos) {
        b = owl_ocr_fra_traineddata; e = owl_ocr_fra_traineddata_end;
    }
    if (!b) { fprintf(stderr, "[reader] modele non bake: %s\n", path); return false; }
    out->assign(reinterpret_cast<const char *>(b), reinterpret_cast<const char *>(e));
    return true;
}

static int ocr_mem(tesseract::TessBaseAPI &api, const unsigned char *png,
                   size_t n, std::string &out) {
    int w, h, chan;
    unsigned char *gray = stbi_load_from_memory(png, (int)n, &w, &h, &chan, 1);
    if (!gray) { fprintf(stderr, "[stb] image illisible\n"); return -1; }
    api.SetImage(gray, w, h, 1 /*bpp*/, w /*bpl*/);
    api.SetSourceResolution(150);
    char *txt = api.GetUTF8Text();
    out = txt ? txt : "";
    delete[] txt;
    stbi_image_free(gray);
    return 0;
}

static bool has(const std::string &s, const char *sub) {
    return s.find(sub) != std::string::npos;
}

static int fail(const char *msg, const std::string &got) {
    fprintf(stderr, "FAIL: %s\n----recu----\n%s\n------------\n", msg, got.c_str());
    return 1;
}

static int write_blob(const char *path, const unsigned char *b, const unsigned char *e) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "ecriture impossible: %s\n", path); return -1; }
    size_t n = (size_t)(e - b);
    size_t w = fwrite(b, 1, n, f);
    fclose(f);
    return (w == n) ? 0 : -1;
}

int main(void) {
    /* Codecs Leptonica OFF -> sa police bitmap interne (stockee en image
     * compressee) n'est pas decodable, ce qui crache des diagnostics cosmetiques
     * "pixReadMemTiff / png library missing" sans incidence sur l'OCR. On coupe
     * la console leptonica : l'echec reel eventuel est deja capte par les
     * assertions sur le texte OCR ci-dessous. */
    setMsgSeverity(L_SEVERITY_NONE);

    const size_t en_png = (size_t)(owl_ocr_img_en_end - owl_ocr_img_en);
    const size_t fr_png = (size_t)(owl_ocr_img_fr_end - owl_ocr_img_fr);
    std::string got;

    /* 1) Mode bake, anglais : zero fichier disque. */
    {
        tesseract::TessBaseAPI api;
        int rc = api.Init(nullptr, 0, "eng", tesseract::OEM_LSTM_ONLY,
                          nullptr, 0, nullptr, nullptr, false, baked_reader);
        if (rc != 0) return fail("Init bake/eng echoue", "");
        if (ocr_mem(api, owl_ocr_img_en, en_png, got) != 0) return fail("decode EN", "");
        api.End();
        if (!has(got, "quick brown fox") || !has(got, "lazy dog"))
            return fail("OCR bake/eng ne contient pas le texte attendu", got);
        printf("[ok] bake/eng  : %s", got.c_str());
    }

    /* 2) Mode bake, francais (accents). */
    {
        tesseract::TessBaseAPI api;
        int rc = api.Init(nullptr, 0, "fra", tesseract::OEM_LSTM_ONLY,
                          nullptr, 0, nullptr, nullptr, false, baked_reader);
        if (rc != 0) return fail("Init bake/fra echoue", "");
        if (ocr_mem(api, owl_ocr_img_fr, fr_png, got) != 0) return fail("decode FR", "");
        api.End();
        if (!has(got, "renard brun") || !has(got, "chien paresseux"))
            return fail("OCR bake/fra ne contient pas le texte attendu", got);
        printf("[ok] bake/fra  : %s", got.c_str());
    }

    /* 3) Mode override : modeles ecrits sur disque, lus par Init(dir, lang). */
    {
        const char *dir = getenv("OWL_OCR_OVERRIDE_DIR");
        std::string d = dir ? dir : "ocr_override_tmp";
        OWL_MKDIR(d.c_str());
        std::string eng = d + "/eng.traineddata";
        std::string fra = d + "/fra.traineddata";
        if (write_blob(eng.c_str(), owl_ocr_eng_traineddata, owl_ocr_eng_traineddata_end) != 0)
            return fail("ecriture modele override eng", "");
        if (write_blob(fra.c_str(), owl_ocr_fra_traineddata, owl_ocr_fra_traineddata_end) != 0)
            return fail("ecriture modele override fra", "");

        tesseract::TessBaseAPI api;
        int rc = api.Init(d.c_str(), "eng", tesseract::OEM_LSTM_ONLY);
        if (rc != 0) return fail("Init override/eng echoue", "");
        if (ocr_mem(api, owl_ocr_img_en, en_png, got) != 0) return fail("decode EN override", "");
        api.End();
        if (!has(got, "quick brown fox"))
            return fail("OCR override/eng ne contient pas le texte attendu", got);
        printf("[ok] override  : %s", got.c_str());

        remove(eng.c_str());
        remove(fra.c_str());
    }

    printf("tesseract_smoketest: OK (bake eng+fra, override eng)\n");
    return 0;
}
