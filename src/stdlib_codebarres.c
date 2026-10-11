// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_codebarres.c — Type CodeBarres + generation d'image via libzint.
 *
 * Type compose first-class (declaration `cb est un CodeBarres`) portant le
 * contenu a encoder, la symbologie, le niveau de correction d'erreur et une
 * eventuelle image centrale (logo QR). La native CodeBarreVersImage produit
 * une Image OWL en memoire a partir de cette description.
 *
 * Pipeline de rendu (backend zint 2.16.0, vendore, sortie buffer only) :
 *   ZBarcode_Create -> symbol->symbology = BARCODE_* (mappe depuis
 *   TypeCodeBarres) -> option_1 (ECC QR) -> ZBarcode_Encode(Contenu) ->
 *   ZBarcode_Buffer -> symbol->bitmap (RGB 3 octets/pixel) -> Image OWL.
 * Un retour zint >= ZINT_ERROR (5) leve un soft-error (ErreurInfo) portant
 * le texte d'erreur du moteur.
 *
 * V1 : contenu texte brut (cbTypeTexte), image centrale pour QR uniquement.
 * Hors V1 (differe) : contenus structures (SMS/WiFi/vCard...), ValeurBrute,
 * calcul de cle de controle, decodage, options d'impression.
 */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "owl_image.h"
#include "vm.h"
#include "sentinels.h"
#include "zint.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

/* image_make_instance vit dans stdlib_image.c (non-static) : enveloppe un
 * OwlImage* dans une ObjInstance Image en gerant le root GC. */
extern ObjInstance* image_make_instance(OwlImage* img);

/* ================================================================
 * Enums / constantes
 * ================================================================ */

/* Symbologies — valeurs = noms de constantes CB_* (chaque valeur est aussi
 * une OwlConstDef, cf CB_CONSTS). */
static const char* const CB_TYPE_VALUES[] = {
    "CB_QRCODE", "CB_DATAMATRIX", "CB_PDF417", "CB_AZTEC", "CB_MAXICODE",
    "CB_UPCA", "CB_UPCE", "CB_EAN13", "CB_EAN8", "CB_EAN128", "CB_GS1_128",
    "CB_CODE128", "CB_CODE39", "CB_CODE93", "CB_CODE11", "CB_INTER2OF5",
    "CB_CODABAR", "CB_MSI",
    /* Extensions OWL hors gamme canon. */
    "CB_GS1_DATABAR", "CB_GS1_DATABAR_ETENDU", "CB_MICRO_QR", "CB_RMQR",
    "CB_DOTCODE", "CB_HAN_XIN", "CB_PZN", NULL
};
static const OwlEnum E_cbType = {
    .name = "cbType", .nameEN = "cbType", .category = "CodeBarres",
    .descFR = "Symbologie d'un code-barres (propriete TypeCodeBarres).",
    .descEN = "Barcode symbology (TypeBarCode property).",
    .values = CB_TYPE_VALUES,
    .combinable = false,
};

static const char* const QR_ECC_VALUES[] = {
    "qrcodeNiveauL", "qrcodeNiveauM", "qrcodeNiveauQ", "qrcodeNiveauH", NULL
};
static const OwlEnum E_qrcodeNiveau = {
    .name = "qrcodeNiveau", .nameEN = "qrcodeLevel", .category = "CodeBarres",
    .descFR = "Niveau de correction d'erreur d'un QR Code (propriete CorrectionErreur).",
    .descEN = "QR Code error-correction level (CorrectionError property).",
    .values = QR_ECC_VALUES,
    .combinable = false,
};

static const char* const CB_CONTENT_VALUES[] = { "cbTypeTexte", NULL };
static const OwlEnum E_cbTypeContenu = {
    .name = "cbTypeContenu", .nameEN = "cbContentType", .category = "CodeBarres",
    .descFR = "Nature du contenu a encoder dans un code-barres (propriete TypeContenu).",
    .descEN = "Nature of the content to encode in a barcode (ContentType property).",
    .values = CB_CONTENT_VALUES,
    .combinable = false,
};

/* Options de generation (propriete Options, combinables). Les valeurs sont des
 * bits internes OWL : l'utilisateur combine les constantes CB_*, la valeur
 * numerique reste opaque. Sous-ensemble GENERATION uniquement — les options de
 * placement/impression (CB_Bas, CB_CentreH, CB_NonProportionnel...) sont hors
 * perimetre (elles n'ont de sens qu'avec l'impression iImprimeCodeBarre). */
#define OWL_CB_OPT_TEXTE       0x01   /* force l'affichage du texte lisible (HRT) */
#define OWL_CB_OPT_SANSTEXTE   0x02   /* masque le texte lisible sous le code */
#define OWL_CB_OPT_ETENDU      0x04   /* Code 39 etendu (jeu ASCII complet) */
#define OWL_CB_OPT_VERIF       0x08   /* ajoute un caractere de controle */
#define OWL_CB_OPT_ROT90       0x20   /* rotation 90 degres */
#define OWL_CB_OPT_ROT180      0x40   /* rotation 180 degres */
#define OWL_CB_OPT_ROT270      0x80   /* rotation 270 degres */

static const char* const CB_OPTIONS_VALUES[] = {
    "CB_Texte", "CB_SansTexte", "CB_Etendu",
    "CB_V\xc3\xa9rification",                    /* CB_Vérification */
    "CB_90", "CB_180", "CB_270", NULL
};
static const OwlEnum E_cbOptions = {
    .name = "cbOptions", .nameEN = "cbOptions", .category = "CodeBarres",
    .descFR = "Options de generation d'un code-barres (combinables) : affichage du texte, jeu etendu, caractere de controle, rotation.",
    .descEN = "Barcode generation options (combinable): human-readable text, extended set, check character, rotation.",
    .values = CB_OPTIONS_VALUES,
    .combinable = true,
};

static const OwlEnum* const CB_ENUMS[] = {
    &E_cbType, &E_qrcodeNiveau, &E_cbTypeContenu, &E_cbOptions
};

static const OwlConstDef CB_CONSTS[] = {
    /* Symbologies 2D. */
    { .name = "CB_QRCODE", .category = "CodeBarres", .nameEN = "CB_QRCODE",
      .value = INTEGER_VAL(OWL_CB_QRCODE),
      .descFR = "QR Code (2D, correction d'erreur, image centrale possible).",
      .descEN = "QR Code (2D, error correction, optional center image)." },
    { .name = "CB_DATAMATRIX", .category = "CodeBarres", .nameEN = "CB_DATAMATRIX",
      .value = INTEGER_VAL(OWL_CB_DATAMATRIX),
      .descFR = "Data Matrix (2D compact, ECC200).",
      .descEN = "Data Matrix (compact 2D, ECC200)." },
    { .name = "CB_PDF417", .category = "CodeBarres", .nameEN = "CB_PDF417",
      .value = INTEGER_VAL(OWL_CB_PDF417),
      .descFR = "PDF417 (2D empile, forte capacite).",
      .descEN = "PDF417 (stacked 2D, high capacity)." },
    { .name = "CB_AZTEC", .category = "CodeBarres", .nameEN = "CB_AZTEC",
      .value = INTEGER_VAL(OWL_CB_AZTEC),
      .descFR = "Aztec Code (2D, sans zone de silence).",
      .descEN = "Aztec Code (2D, no quiet zone)." },
    { .name = "CB_MAXICODE", .category = "CodeBarres", .nameEN = "CB_MAXICODE",
      .value = INTEGER_VAL(OWL_CB_MAXICODE),
      .descFR = "MaxiCode (2D a taille fixe, logistique).",
      .descEN = "MaxiCode (fixed-size 2D, logistics)." },
    /* Symbologies 1D. */
    { .name = "CB_UPCA", .category = "CodeBarres", .nameEN = "CB_UPCA",
      .value = INTEGER_VAL(OWL_CB_UPCA),
      .descFR = "UPC-A (12 chiffres, distribution Amerique du Nord).",
      .descEN = "UPC-A (12 digits, North American retail)." },
    { .name = "CB_UPCE", .category = "CodeBarres", .nameEN = "CB_UPCE",
      .value = INTEGER_VAL(OWL_CB_UPCE),
      .descFR = "UPC-E (version compressee de UPC-A).",
      .descEN = "UPC-E (compressed UPC-A)." },
    { .name = "CB_EAN13", .category = "CodeBarres", .nameEN = "CB_EAN13",
      .value = INTEGER_VAL(OWL_CB_EAN13),
      .descFR = "EAN-13 (13 chiffres, code produit international).",
      .descEN = "EAN-13 (13 digits, international article number)." },
    { .name = "CB_EAN8", .category = "CodeBarres", .nameEN = "CB_EAN8",
      .value = INTEGER_VAL(OWL_CB_EAN8),
      .descFR = "EAN-8 (8 chiffres, petits emballages).",
      .descEN = "EAN-8 (8 digits, small packaging)." },
    { .name = "CB_EAN128", .category = "CodeBarres", .nameEN = "CB_EAN128",
      .value = INTEGER_VAL(OWL_CB_EAN128),
      .descFR = "EAN-128 (ancien nom de GS1-128, identifiants applicatifs GS1).",
      .descEN = "EAN-128 (former name of GS1-128, GS1 application identifiers)." },
    { .name = "CB_GS1_128", .category = "CodeBarres", .nameEN = "CB_GS1_128",
      .value = INTEGER_VAL(OWL_CB_GS1_128),
      .descFR = "GS1-128 (Code 128 avec identifiants applicatifs GS1).",
      .descEN = "GS1-128 (Code 128 with GS1 application identifiers)." },
    { .name = "CB_CODE128", .category = "CodeBarres", .nameEN = "CB_CODE128",
      .value = INTEGER_VAL(OWL_CB_CODE128),
      .descFR = "Code 128 (alphanumerique dense, usage general).",
      .descEN = "Code 128 (dense alphanumeric, general purpose)." },
    { .name = "CB_CODE39", .category = "CodeBarres", .nameEN = "CB_CODE39",
      .value = INTEGER_VAL(OWL_CB_CODE39),
      .descFR = "Code 39 (alphanumerique, industrie).",
      .descEN = "Code 39 (alphanumeric, industrial)." },
    { .name = "CB_CODE93", .category = "CodeBarres", .nameEN = "CB_CODE93",
      .value = INTEGER_VAL(OWL_CB_CODE93),
      .descFR = "Code 93 (variante compacte du Code 39).",
      .descEN = "Code 93 (compact Code 39 variant)." },
    { .name = "CB_CODE11", .category = "CodeBarres", .nameEN = "CB_CODE11",
      .value = INTEGER_VAL(OWL_CB_CODE11),
      .descFR = "Code 11 (numerique, telecommunications).",
      .descEN = "Code 11 (numeric, telecommunications)." },
    { .name = "CB_INTER2OF5", .category = "CodeBarres", .nameEN = "CB_INTER2OF5",
      .value = INTEGER_VAL(OWL_CB_INTER2OF5),
      .descFR = "Entrelace 2 parmi 5 (numerique, longueur paire).",
      .descEN = "Interleaved 2 of 5 (numeric, even length)." },
    { .name = "CB_CODABAR", .category = "CodeBarres", .nameEN = "CB_CODABAR",
      .value = INTEGER_VAL(OWL_CB_CODABAR),
      .descFR = "Codabar (numerique, laboratoires et bibliotheques).",
      .descEN = "Codabar (numeric, labs and libraries)." },
    { .name = "CB_MSI", .category = "CodeBarres", .nameEN = "CB_MSI",
      .value = INTEGER_VAL(OWL_CB_MSI),
      .descFR = "MSI Plessey (numerique, gestion de stock).",
      .descEN = "MSI Plessey (numeric, inventory)." },
    /* Extensions OWL hors gamme canon. */
    { .name = "CB_GS1_DATABAR", .category = "CodeBarres", .nameEN = "CB_GS1_DATABAR",
      .value = INTEGER_VAL(OWL_CB_GS1_DATABAR),
      .descFR = "GS1 DataBar Omnidirectionnel (GTIN numerique, distribution et sante).",
      .descEN = "GS1 DataBar Omnidirectional (numeric GTIN, retail and healthcare)." },
    { .name = "CB_GS1_DATABAR_ETENDU", .category = "CodeBarres", .nameEN = "CB_GS1_DATABAR_EXPANDED",
      .value = INTEGER_VAL(OWL_CB_GS1_DATABAR_ETENDU),
      .descFR = "GS1 DataBar Etendu (donnees a identifiants applicatifs GS1).",
      .descEN = "GS1 DataBar Expanded (GS1 application-identifier data)." },
    { .name = "CB_MICRO_QR", .category = "CodeBarres", .nameEN = "CB_MICRO_QR",
      .value = INTEGER_VAL(OWL_CB_MICRO_QR),
      .descFR = "Micro QR Code (2D compact, faible capacite, contenu court).",
      .descEN = "Micro QR Code (compact 2D, low capacity, short content)." },
    { .name = "CB_RMQR", .category = "CodeBarres", .nameEN = "CB_RMQR",
      .value = INTEGER_VAL(OWL_CB_RMQR),
      .descFR = "rMQR (Micro QR rectangulaire, 2D pour surfaces etroites).",
      .descEN = "rMQR (rectangular Micro QR, 2D for narrow surfaces)." },
    { .name = "CB_DOTCODE", .category = "CodeBarres", .nameEN = "CB_DOTCODE",
      .value = INTEGER_VAL(OWL_CB_DOTCODE),
      .descFR = "DotCode (2D en points, marquage haute vitesse).",
      .descEN = "DotCode (dot-based 2D, high-speed marking)." },
    { .name = "CB_HAN_XIN", .category = "CodeBarres", .nameEN = "CB_HAN_XIN",
      .value = INTEGER_VAL(OWL_CB_HAN_XIN),
      .descFR = "Han Xin (2D, prise en charge du chinois).",
      .descEN = "Han Xin (2D, Chinese character support)." },
    { .name = "CB_PZN", .category = "CodeBarres", .nameEN = "CB_PZN",
      .value = INTEGER_VAL(OWL_CB_PZN),
      .descFR = "PZN (Pharmazentralnummer, numero d'article pharmaceutique allemand).",
      .descEN = "PZN (Pharmazentralnummer, German pharmaceutical article number)." },
    /* Niveaux de correction QR. */
    { .name = "qrcodeNiveauL", .category = "CodeBarres", .nameEN = "qrcodeLevelL",
      .value = INTEGER_VAL(OWL_QR_ECC_L),
      .descFR = "QR Code : correction d'erreur faible (~7%).",
      .descEN = "QR Code: low error correction (~7%)." },
    { .name = "qrcodeNiveauM", .category = "CodeBarres", .nameEN = "qrcodeLevelM",
      .value = INTEGER_VAL(OWL_QR_ECC_M),
      .descFR = "QR Code : correction d'erreur moyenne (~15%, defaut).",
      .descEN = "QR Code: medium error correction (~15%, default)." },
    { .name = "qrcodeNiveauQ", .category = "CodeBarres", .nameEN = "qrcodeLevelQ",
      .value = INTEGER_VAL(OWL_QR_ECC_Q),
      .descFR = "QR Code : correction d'erreur elevee (~25%).",
      .descEN = "QR Code: quartile error correction (~25%)." },
    { .name = "qrcodeNiveauH", .category = "CodeBarres", .nameEN = "qrcodeLevelH",
      .value = INTEGER_VAL(OWL_QR_ECC_H),
      .descFR = "QR Code : correction d'erreur maximale (~30%).",
      .descEN = "QR Code: highest error correction (~30%)." },
    /* Type de contenu. */
    { .name = "cbTypeTexte", .category = "CodeBarres", .nameEN = "cbTypeText",
      .value = INTEGER_VAL(OWL_CB_CONTENT_TEXTE),
      .descFR = "Contenu du code-barres encode comme texte brut.",
      .descEN = "Barcode content encoded as plain text." },
    /* Options de generation (propriete Options, combinables). */
    { .name = "CB_Texte", .category = "CodeBarres", .nameEN = "CB_Text",
      .value = INTEGER_VAL(OWL_CB_OPT_TEXTE),
      .descFR = "Affiche le texte lisible sous le code-barres.",
      .descEN = "Show the human-readable text under the barcode." },
    { .name = "CB_SansTexte", .category = "CodeBarres", .nameEN = "CB_NoText",
      .value = INTEGER_VAL(OWL_CB_OPT_SANSTEXTE),
      .descFR = "N'affiche pas le texte lisible sous le code-barres.",
      .descEN = "Hide the human-readable text under the barcode." },
    { .name = "CB_Etendu", .category = "CodeBarres", .nameEN = "CB_Extended",
      .value = INTEGER_VAL(OWL_CB_OPT_ETENDU),
      .descFR = "Jeu de caracteres etendu (ASCII complet) pour le Code 39 et le Code 93.",
      .descEN = "Extended character set (full ASCII) for Code 39 and Code 93." },
    { .name = "CB_V\xc3\xa9rification", .category = "CodeBarres", .nameEN = "CB_Check",
      .value = INTEGER_VAL(OWL_CB_OPT_VERIF),
      .descFR = "Ajoute la cle de verification (Code 39, Code 93, Interleaved 2/5, Codabar, MSI).",
      .descEN = "Append the check key (Code 39, Code 93, Interleaved 2 of 5, Codabar, MSI)." },
    { .name = "CB_90", .category = "CodeBarres", .nameEN = "CB_90",
      .value = INTEGER_VAL(OWL_CB_OPT_ROT90),
      .descFR = "Rotation du rendu de 90 degres.",
      .descEN = "Rotate the rendering by 90 degrees." },
    { .name = "CB_180", .category = "CodeBarres", .nameEN = "CB_180",
      .value = INTEGER_VAL(OWL_CB_OPT_ROT180),
      .descFR = "Rotation du rendu de 180 degres.",
      .descEN = "Rotate the rendering by 180 degrees." },
    { .name = "CB_270", .category = "CodeBarres", .nameEN = "CB_270",
      .value = INTEGER_VAL(OWL_CB_OPT_ROT270),
      .descFR = "Rotation du rendu de 270 degres.",
      .descEN = "Rotate the rendering by 270 degrees." },
};

/* ================================================================
 * Type CodeBarres / BarCodes
 * ================================================================ */

static const OwlField FIELDS_CodeBarres[] = {
    {"Contenu", NULL, "chaîne", "\"\"",
     "donnee a encoder (texte brut en v1)", "data to encode (plain text in v1)",
     .nameEN = "Content"},
    {"TypeCodeBarres", NULL, "entier", "CB_QRCODE",
     "symbologie du code-barres", "barcode symbology",
     .allowedBase = &E_cbType, .nameEN = "TypeBarCode"},
    {"CorrectionErreur", NULL, "entier", "qrcodeNiveauM",
     "niveau de correction d'erreur (QR : qrcodeNiveau* ; PDF417 : 0-8 ; Aztec : 1-4)",
     "error-correction level (QR: qrcodeLevel*; PDF417: 0-8; Aztec: 1-4)",
     .allowedBase = &E_qrcodeNiveau, .nameEN = "CorrectionError"},
    {"ImageCentrale", NULL, "Image", "Null",
     "image centrale a incruster (QR Code)", "center image to overlay (QR Code)",
     .nameEN = "CenterImage"},
    {"ProportionImage", NULL, "réel", "0",
     "pourcentage de surface occupee par l'image centrale", "percentage of area covered by the center image",
     .nameEN = "ImageProportion"},
    {"TypeContenu", NULL, "entier", "cbTypeTexte",
     "nature du contenu a encoder", "nature of the content to encode",
     .allowedBase = &E_cbTypeContenu, .nameEN = "ContentType"},
    {"Options", NULL, "entier", "0",
     "options de generation (combinaison de constantes CB_*)", "generation options (combination of CB_* constants)",
     .allowedOptions = &E_cbOptions, .nameEN = "Options"},
    {0},
};

static const char* const ALIAS_CodeBarres[] = { "CodeBarres", "BarCodes", NULL };

static const OwlTypeDef CB_TYPES[] = {
    { .canonical      = "CodeBarres",
      .canonicalAscii = "CodeBarres",
      .aliases        = ALIAS_CodeBarres,
      .category       = "CodeBarres",
      .descFR         = "Description d'un code-barres a generer : contenu, symbologie, niveau de correction d'erreur et image centrale optionnelle. Genere une Image via CodeBarreVersImage.",
      .descEN         = "Description of a barcode to generate: content, symbology, error-correction level and optional center image. Generates an Image via BarCodeToImage.",
      .fields         = FIELDS_CodeBarres,
      .nameEN         = "BarCodes",
      .genre          = OWL_GENRE_M },   /* le code-barres */
};

/* Construit le template runtime (defaults canon) enregistre dans vm.types.
 * __StructClone(<CodeBarres>) part de ce template pour toute declaration. */
static ObjInstance* build_codebarres_template(void) {
    ObjString* name = obj_copy_string("CodeBarres", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();
    inst_field_set_str  (tpl, "Contenu",          "");
    inst_field_set_int  (tpl, "TypeCodeBarres",   OWL_CB_QRCODE);
    inst_field_set_int  (tpl, "CorrectionErreur", OWL_QR_ECC_M);
    inst_field_set_value(tpl, "ImageCentrale",    NIL_VAL);
    inst_field_set_value(tpl, "ProportionImage",  REAL_VAL(0.0));
    inst_field_set_int  (tpl, "TypeContenu",      OWL_CB_CONTENT_TEXTE);
    inst_field_set_int  (tpl, "Options",          0);
    return tpl;
}

/* ================================================================
 * Helpers de lecture des champs de l'instance CodeBarres
 * ================================================================ */

static bool cb_get_field(const ObjInstance* inst, const char* name, Value* out) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    return table_get((Table*)&inst->fields, k, out);
}

static int64_t cb_get_int(const ObjInstance* inst, const char* name, int64_t dflt) {
    Value v;
    if (cb_get_field(inst, name, &v) && IS_INTEGER(v)) return AS_INTEGER(v);
    return dflt;
}

static double cb_get_real(const ObjInstance* inst, const char* name, double dflt) {
    Value v;
    if (!cb_get_field(inst, name, &v)) return dflt;
    if (IS_REAL(v)) return AS_REAL(v);
    if (IS_INTEGER(v)) return (double)AS_INTEGER(v);
    return dflt;
}

/* OwlImage* d'une Value censee etre une ObjInstance Image, NULL sinon. */
static OwlImage* cb_value_to_image(Value v) {
    if (!IS_INSTANCE(v)) return NULL;
    ObjInstance* inst = AS_INSTANCE(v);
    if (!inst->structName || strcmp(inst->structName->chars, "Image") != 0)
        return NULL;
    Value ptr;
    ObjString* k = obj_copy_string("_ptr", 4);
    if (!table_get(&inst->fields, k, &ptr) || !IS_INTEGER(ptr)) return NULL;
    return (OwlImage*)(intptr_t)AS_INTEGER(ptr);
}

/* Mappe une symbologie CB_* vers le code du moteur zint. -1 si inconnue. */
static int cb_symbology_to_zint(int64_t cbType) {
    switch (cbType) {
        case OWL_CB_QRCODE:     return BARCODE_QRCODE;
        case OWL_CB_DATAMATRIX: return BARCODE_DATAMATRIX;
        case OWL_CB_PDF417:     return BARCODE_PDF417;
        case OWL_CB_AZTEC:      return BARCODE_AZTEC;
        case OWL_CB_MAXICODE:   return BARCODE_MAXICODE;
        case OWL_CB_UPCA:       return BARCODE_UPCA;
        case OWL_CB_UPCE:       return BARCODE_UPCE;
        /* Symbologies dediees (strictes sur la longueur/le jeu de caracteres)
         * plutot que BARCODE_EANX legacy, qui tolere silencieusement les
         * add-ons EAN-2/EAN-5 et masque les contenus invalides. */
        case OWL_CB_EAN13:      return BARCODE_EAN13;
        case OWL_CB_EAN8:       return BARCODE_EAN8;
        case OWL_CB_EAN128:     return BARCODE_GS1_128;
        case OWL_CB_GS1_128:    return BARCODE_GS1_128;
        case OWL_CB_CODE128:    return BARCODE_CODE128;
        case OWL_CB_CODE39:     return BARCODE_CODE39;
        case OWL_CB_CODE93:     return BARCODE_CODE93;
        case OWL_CB_CODE11:     return BARCODE_CODE11;
        case OWL_CB_INTER2OF5:  return BARCODE_C25INTER;
        case OWL_CB_CODABAR:    return BARCODE_CODABAR;
        case OWL_CB_MSI:        return BARCODE_MSI_PLESSEY;
        /* Extensions OWL hors gamme canon. */
        case OWL_CB_GS1_DATABAR:        return BARCODE_DBAR_OMN;
        case OWL_CB_GS1_DATABAR_ETENDU: return BARCODE_DBAR_EXP;
        case OWL_CB_MICRO_QR:           return BARCODE_MICROQR;
        case OWL_CB_RMQR:               return BARCODE_RMQR;
        case OWL_CB_DOTCODE:            return BARCODE_DOTCODE;
        case OWL_CB_HAN_XIN:            return BARCODE_HANXIN;
        case OWL_CB_PZN:                return BARCODE_PZN;
        default:                return -1;
    }
}

/* Pre-validation du contenu pour les symbologies EAN a longueur fixe. zint
 * zero-pad silencieusement un EAN trop court (leading zeroes) et genererait un
 * code-barres trompeur ; le canon, lui, refuse. On rejette donc en amont pour
 * rester fidele au canon : longueur exacte (check digit optionnel) + numerique.
 * Retourne un message d'erreur ou NULL si le contenu est acceptable. */
static const char* cb_validate_content(int64_t cbType, const char* s, int len) {
    /* Longueur canon (donnees seules — le caractere de controle est calcule
     * par la lib). Aligne sur le canon :
     * EAN13 = 12 chiffres, EAN8 = 7 chiffres, exactement. */
    /* Divergence assumee UPC-E : le canon prend le numero UPC-A complet (11
     * chiffres) et le compresse lui-meme en UPC-E ; OWL/zint prend directement
     * les 6-8 chiffres UPC-E deja compresses. Modeles d'entree incompatibles —
     * on conserve le modele zint (documente), donc PAS de validation de
     * longueur ici pour CB_UPCE (choix dev). */
    int wantLen;
    const char* msg;
    switch (cbType) {
        case OWL_CB_EAN13:
            wantLen = 12;
            msg = "EAN13 : 12 chiffres attendus (caractere de controle calcule)";
            break;
        case OWL_CB_EAN8:
            wantLen = 7;
            msg = "EAN8 : 7 chiffres attendus (caractere de controle calcule)";
            break;
        case OWL_CB_UPCA:
            wantLen = 11;
            msg = "UPC-A : 11 chiffres attendus (caractere de controle calcule)";
            break;
        default:
            return NULL;   /* pas de contrainte de longueur fixe */
    }
    if (len != wantLen)
        return msg;
    for (int i = 0; i < len; i++)
        if (s[i] < '0' || s[i] > '9')
            return "symbologie EAN : chiffres uniquement";
    return NULL;
}

/* Niveau de correction QR -> option_1 zint (1=L,2=M,3=Q,4=H). 0 si non-QR. */
static int cb_ecc_to_option1(int64_t ecc) {
    switch (ecc) {
        case OWL_QR_ECC_L: return 1;
        case OWL_QR_ECC_M: return 2;
        case OWL_QR_ECC_Q: return 3;
        case OWL_QR_ECC_H: return 4;
        default:           return 0;   /* laisse zint choisir */
    }
}

/* La prop CorrectionErreur porte une valeur de nature differente selon la
 * symbologie (QR : enum qrcodeNiveau* ; PDF417 : entier 0-8 ; Aztec : entier
 * 1-4). Vrai si la valeur est une constante qrcodeNiveau* (defaut QR non
 * touche) : pour PDF417/Aztec on laisse alors le moteur choisir plutot que
 * d'interpreter un sentinel QR comme un niveau entier. */
static bool cb_ecc_is_qr_sentinel(int64_t ecc) {
    return ecc == OWL_QR_ECC_L || ecc == OWL_QR_ECC_M
        || ecc == OWL_QR_ECC_Q || ecc == OWL_QR_ECC_H;
}

/* Plafond de proportion (% de surface) de l'image centrale selon le niveau
 * ECC : plus la correction est forte, plus on peut masquer. L->1, M->3,
 * Q->5, H->5 (au-dela le decodage n'est plus fiable). */
static double cb_max_proportion_for_ecc(int64_t ecc) {
    switch (ecc) {
        case OWL_QR_ECC_H: return 5.0;
        case OWL_QR_ECC_Q: return 5.0;
        case OWL_QR_ECC_M: return 3.0;
        case OWL_QR_ECC_L: return 1.0;
        default:           return 3.0;
    }
}

/* Convertit une couleur OWL (BGR sur 24 bits : R + V*256 + B*65536) vers la
 * representation hexadecimale "RRGGBB" attendue par zint. */
static void cb_color_to_hex(int64_t couleur, char out[7]) {
    unsigned r = (unsigned)(couleur & 0xFF);
    unsigned g = (unsigned)((couleur >> 8) & 0xFF);
    unsigned b = (unsigned)((couleur >> 16) & 0xFF);
    snprintf(out, 7, "%02X%02X%02X", r, g, b);
}

/* Caractere de garde START/STOP Codabar accepte par zint (majuscules A-D). */
static bool cb_is_codabar_guard(char c) {
    return c == 'A' || c == 'B' || c == 'C' || c == 'D';
}

/* ================================================================
 * CodeBarreVersImage(<CodeBarres> [, largeurMax [, hauteurMax
 *                     [, couleurFond [, couleurCodeBarres]]]]) -> Image
 * ================================================================ */

#define CB_DEFAULT_TARGET_PX 512

static Value cb_error_image(const char* detail) {
    owl_emit_native_error(E_NATIVE_BARCODE_ENCODE_FAILURE, "CodeBarreVersImage",
                          "detail", detail, NULL);
    return OBJ_VAL(image_make_instance(owl_image_new_empty()));
}

/* Pipeline zint partage (native CodeBarreVersImage + widget Champ CodeBarres) :
 * produit une OwlImage* rasterisee depuis les parametres bruts d'un code-barres
 * (type + contenu + correction d'erreur + options + bornes + couleurs).
 * Retourne NULL et pose *errOut (message statique) en cas d'echec. N'emet PAS
 * d'erreur runtime — l'appelant decide (la native leve un ErreurInfo, le widget
 * laisse simplement le champ vide). L'image centrale (logo QR) est appliquee
 * par l'appelant (native) car elle depend de l'instance CodeBarres. */
OwlImage* owl_codebarre_generate_owlimage(int64_t cbType, int64_t ecc,
                                          int64_t options,
                                          const char* contenu, int contenuLen,
                                          int largeurMax, int hauteurMax,
                                          bool hasBg, int64_t bgColor,
                                          bool hasFg, int64_t fgColor,
                                          const char** errOut) {
    #define CB_FAIL(msg) do { if (errOut) *errOut = (msg); return NULL; } while (0)

    if (contenu == NULL || contenuLen == 0)
        CB_FAIL("contenu vide");

    int symbology = cb_symbology_to_zint(cbType);
    if (symbology < 0)
        CB_FAIL("symbologie non prise en charge");

    /* CB_Etendu : le Code 39 etendu (ASCII complet) est une symbologie zint
     * distincte, pas un simple flag. Le Code 93 est deja full-ASCII en zint. */
    if ((options & OWL_CB_OPT_ETENDU) && symbology == BARCODE_CODE39)
        symbology = BARCODE_EXCODE39;

    /* Validation stricte amont (EAN a longueur fixe) — zint zero-pad sinon. */
    const char* vErr = cb_validate_content(cbType, contenu, contenuLen);
    if (vErr)
        CB_FAIL(vErr);

    struct zint_symbol* symbol = ZBarcode_Create();
    if (!symbol)
        CB_FAIL("allocation du moteur impossible");

    symbol->symbology = symbology;
    symbol->input_mode = UNICODE_MODE;   /* contenu texte UTF-8 */

    /* Niveau de correction d'erreur (option_1) selon la symbologie :
     *  - QR     : enum qrcodeNiveau* -> 1..4
     *  - PDF417 : entier 0..8 (nombre de codewords de correction)
     *  - Aztec  : entier 1..4 (couches de correction ; zint n'expose PAS le
     *             modele pourcentage 5-95% du canon — divergence documentee)
     * Si la valeur est restee sur le defaut QR (sentinel qrcodeNiveau*) pour un
     * PDF417/Aztec, on laisse le moteur choisir. */
    if (symbology == BARCODE_QRCODE) {
        int opt1 = cb_ecc_to_option1(ecc);
        if (opt1 > 0) symbol->option_1 = opt1;
    } else if (symbology == BARCODE_PDF417) {
        if (!cb_ecc_is_qr_sentinel(ecc) && ecc >= 0 && ecc <= 8)
            symbol->option_1 = (int)ecc;
    } else if (symbology == BARCODE_AZTEC) {
        if (!cb_ecc_is_qr_sentinel(ecc) && ecc >= 1 && ecc <= 4)
            symbol->option_1 = (int)ecc;
    } else if (symbology == BARCODE_DATAMATRIX) {
        /* Carre par defaut (canon + attente usuelle) : sans DM_SQUARE,
         * zint peut choisir une version rectangulaire (ex. 8x32) sur du
         * contenu court, ce qui surprend l'utilisateur. */
        symbol->option_3 = DM_SQUARE;
    }

    /* Options de generation (propriete Options, bitmask). */
    if (options & OWL_CB_OPT_SANSTEXTE)   symbol->show_hrt = 0;
    else if (options & OWL_CB_OPT_TEXTE)  symbol->show_hrt = 1;
    /* CB_Verification : cle de controle 1D optionnelle. option_2 n'a ce sens
     * (ajouter le check digit) que pour Code39/Code93/Interleaved 2 of 5/Codabar.
     * Pour DataMatrix (taille imposee) ou PDF417 (nb colonnes), option_2=1 force
     * la plus petite matrice / une seule colonne -> encodage casse. On ne touche
     * donc option_2 QUE sur les 1D valides (MSI : option_2=1 = cle mod10).
     * NB : le markup <...> des contenus 128/2D (FNC1, bascule de jeu, jeux
     * DataMatrix/PDF417) n'est PAS supporte — zint auto-selectionne l'encodage
     * optimal (comme le canon par defaut) et n'expose pas de levier pour forcer
     * les jeux DataMatrix/PDF417 (cf  + DIVERGENCES codebarre_markup_001). */
    if (options & OWL_CB_OPT_VERIF) {
        if (symbology == BARCODE_CODE39 || symbology == BARCODE_EXCODE39 ||
            symbology == BARCODE_CODE93 || symbology == BARCODE_C25INTER ||
            symbology == BARCODE_CODABAR || symbology == BARCODE_MSI_PLESSEY)
            symbol->option_2 = 1;
    }

    /* Couleurs optionnelles (defaut zint : noir sur blanc). */
    if (hasBg) {
        char hex[7];
        cb_color_to_hex(bgColor, hex);
        memcpy(symbol->bgcolour, hex, 7);
    }
    if (hasFg) {
        char hex[7];
        cb_color_to_hex(fgColor, hex);
        memcpy(symbol->fgcolour, hex, 7);
    }

    /* Codabar : le canon prefixe/suffixe automatiquement un caractere de garde
     * START/STOP 'A' quand le contenu n'en a pas ; zint, lui, refuse. On aligne
     * sur le canon en encadrant le contenu avant l'encodage. */
    const char* encContenu = contenu;
    int encLen = contenuLen;
    char* codabarBuf = NULL;
    if (symbology == BARCODE_CODABAR) {
        bool needStart = !cb_is_codabar_guard(contenu[0]);
        bool needStop  = !cb_is_codabar_guard(contenu[contenuLen - 1]);
        if (needStart || needStop) {
            int extra = (needStart ? 1 : 0) + (needStop ? 1 : 0);
            codabarBuf = (char*)malloc((size_t)contenuLen + (size_t)extra + 1);
            if (codabarBuf) {
                int p = 0;
                if (needStart) codabarBuf[p++] = 'A';
                memcpy(codabarBuf + p, contenu, (size_t)contenuLen);
                p += contenuLen;
                if (needStop) codabarBuf[p++] = 'A';
                codabarBuf[p] = '\0';
                encContenu = codabarBuf;
                encLen = p;
            }
        }
    }

    int err = ZBarcode_Encode(symbol, (const unsigned char*)encContenu, encLen);
    free(codabarBuf);   /* zint a copie le contenu en interne */
    if (err >= ZINT_ERROR) {
        static char detail[192];
        snprintf(detail, sizeof(detail), "%s", symbol->errtxt);
        ZBarcode_Delete(symbol);
        CB_FAIL(detail);
    }

    /* Rotation du rendu (Options CB_90/180/270 -> angle passe au rasteriseur). */
    int rotate = 0;
    if      (options & OWL_CB_OPT_ROT90)  rotate = 90;
    else if (options & OWL_CB_OPT_ROT180) rotate = 180;
    else if (options & OWL_CB_OPT_ROT270) rotate = 270;

    err = ZBarcode_Buffer(symbol, rotate);
    if (err >= ZINT_ERROR) {
        static char detail[192];
        snprintf(detail, sizeof(detail), "%s", symbol->errtxt);
        ZBarcode_Delete(symbol);
        CB_FAIL(detail);
    }

    if (symbol->bitmap == NULL || symbol->bitmap_width <= 0 || symbol->bitmap_height <= 0) {
        ZBarcode_Delete(symbol);
        CB_FAIL("bitmap vide");
    }

    /* zint : bitmap RGB (3 octets/pixel), top-down. Conversion vers RGBA8. */
    int w = symbol->bitmap_width;
    int h = symbol->bitmap_height;
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * (size_t)h * 4);
    if (!rgba) {
        ZBarcode_Delete(symbol);
        CB_FAIL("memoire insuffisante");
    }
    const unsigned char* src = symbol->bitmap;
    for (int i = 0; i < w * h; i++) {
        rgba[i * 4 + 0] = src[i * 3 + 0];
        rgba[i * 4 + 1] = src[i * 3 + 1];
        rgba[i * 4 + 2] = src[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
    ZBarcode_Delete(symbol);

    OwlImage* img = owl_image_new_from_rgba(w, h, rgba);
    free(rgba);
    if (!img)
        CB_FAIL("construction de l'image impossible");

    /* Mise a l'echelle : les bornes largeurMax/hauteurMax plafonnent la
     * sortie (canon). Sans bornes, on vise une taille lisible par defaut.
     * - source <= boite : agrandissement entier (rendu net des modules) ;
     * - source > boite  : reduction homothetique bilineaire. */
    int boxW = largeurMax > 0 ? largeurMax : CB_DEFAULT_TARGET_PX;
    int boxH = hauteurMax > 0 ? hauteurMax : CB_DEFAULT_TARGET_PX;
    if (w <= boxW && h <= boxH) {
        int k = boxW / w;
        int kh = boxH / h;
        if (kh < k) k = kh;
        if (k > 1)
            owl_image_resize_to(img, w * k, h * k, OWL_RESIZE_ETIRE);
    } else {
        owl_image_resize_to(img, boxW, boxH,
                            OWL_RESIZE_HOMOTHETIQUE | OWL_RESIZE_HAUTE_QUALITE);
    }

    return img;
    #undef CB_FAIL
}

/*  — superpose une image centrale (logo) centree sur un code-barres QR
 * selon une proportion de surface (%) plafonnee par le niveau ECC. QR
 * uniquement (les autres symbologies ne tolerent pas ce masquage). 100% C-level
 * (aucune VM/GC) : reutilisable par le chemin instance ET la regen au resize
 * (timer Qt hors contexte VM). Sans effet si barcode/centre invalides ou si la
 * symbologie n'est pas QR. */
void owl_codebarre_composite_center_image(OwlImage* barcode, int64_t cbType,
                                          int64_t ecc, OwlImage* centre,
                                          double proportion) {
    if (!barcode || !centre) return;
    if (cb_symbology_to_zint(cbType) != BARCODE_QRCODE) return;
    if (!owl_image_valid(centre)) return;
    double capMax = cb_max_proportion_for_ecc(ecc);
    if (proportion <= 0.0) proportion = capMax;
    if (proportion > capMax) proportion = capMax;
    int fw = owl_image_width(barcode);
    int fh = owl_image_height(barcode);
    /* Fraction lineaire = racine de la fraction de surface. */
    double sideFrac = sqrt(proportion / 100.0);
    int cw = (int)(fw * sideFrac);
    int ch = (int)(fh * sideFrac);
    if (cw > 0 && ch > 0) {
        int dx = (fw - cw) / 2;
        int dy = (fh - ch) / 2;
        owl_image_copy_rect(centre, barcode, 0, 0, -1, -1, dx, dy, cw, ch);
    }
}

/* Genere l'Image (ObjInstance GC-geree) d'un code-barres decrit par une
 * instance CodeBarres. Applique l'image centrale (logo QR). Retourne NIL_VAL
 * et pose *errOut en cas d'echec (l'appelant decide de l'emission d'erreur). */
static Value cb_image_value_from_instance(ObjInstance* cb, int largeurMax,
                                          int hauteurMax, bool hasBg,
                                          int64_t bgC, bool hasFg, int64_t fgC,
                                          const char** errOut) {
    int64_t cbType  = cb_get_int(cb, "TypeCodeBarres", OWL_CB_QRCODE);
    int64_t ecc     = cb_get_int(cb, "CorrectionErreur", OWL_QR_ECC_M);
    int64_t options = cb_get_int(cb, "Options", 0);
    Value contenuV;
    const char* contenu = "";
    int contenuLen = 0;
    if (cb_get_field(cb, "Contenu", &contenuV) && IS_STRING(contenuV)) {
        contenu = AS_STRING(contenuV)->chars;
        contenuLen = AS_STRING(contenuV)->length;
    }

    const char* err = NULL;
    OwlImage* img = owl_codebarre_generate_owlimage(cbType, ecc, options,
                                                    contenu, contenuLen,
                                                    largeurMax, hauteurMax,
                                                    hasBg, bgC, hasFg, fgC, &err);
    if (!img) {
        if (errOut) *errOut = err;
        return NIL_VAL;
    }

    /* Image centrale (logo) — QR Code uniquement. Depend de l'instance. */
    if (cb_symbology_to_zint(cbType) == BARCODE_QRCODE) {
        Value imgCentreV;
        if (cb_get_field(cb, "ImageCentrale", &imgCentreV) && !IS_NIL(imgCentreV)) {
            OwlImage* centre = cb_value_to_image(imgCentreV);
            double prop = cb_get_real(cb, "ProportionImage", 0.0);
            owl_codebarre_composite_center_image(img, cbType, ecc, centre, prop);
        }
    }

    return OBJ_VAL(image_make_instance(img));
}

/* Widget Champ CodeBarres — adoption d'un descripteur complet
 * (`CBA = maVariableCodeBarres`) : produit l'Image depuis l'instance, sans
 * bornes ni couleurs, silencieux (NIL_VAL si echec, le champ reste vide). */
Value owl_codebarre_image_value_from_instance(ObjInstance* cb) {
    if (cb == NULL) return NIL_VAL;
    return cb_image_value_from_instance(cb, 0, 0, false, 0, false, 0, NULL);
}

/*  — extrait les params bruts d'une instance CodeBarres pour que le
 * Champ CodeBarres puisse stocker sa config de regen (chemin C pur au resize).
 * *contenuOut pointe dans l'ObjString de l'instance : valide tant que cb reste
 * reference (l'appelant copie immediatement). Retourne NULL contenu si absent. */
void owl_codebarre_params_from_instance(ObjInstance* cb, int64_t* typeOut,
                                        int64_t* eccOut, int64_t* optionsOut,
                                        const char** contenuOut, int* contenuLenOut) {
    if (typeOut)    *typeOut    = OWL_CB_QRCODE;
    if (eccOut)     *eccOut     = OWL_QR_ECC_M;
    if (optionsOut) *optionsOut = 0;
    if (contenuOut) *contenuOut = NULL;
    if (contenuLenOut) *contenuLenOut = 0;
    if (cb == NULL) return;
    if (typeOut)    *typeOut    = cb_get_int(cb, "TypeCodeBarres", OWL_CB_QRCODE);
    if (eccOut)     *eccOut     = cb_get_int(cb, "CorrectionErreur", OWL_QR_ECC_M);
    if (optionsOut) *optionsOut = cb_get_int(cb, "Options", 0);
    Value contenuV;
    if (cb_get_field(cb, "Contenu", &contenuV) && IS_STRING(contenuV)) {
        if (contenuOut)    *contenuOut    = AS_STRING(contenuV)->chars;
        if (contenuLenOut) *contenuLenOut = AS_STRING(contenuV)->length;
    }
}

/* Widget Champ CodeBarres — rendu depuis les props plates
 * (TypeCodeBarres/Contenu/CorrectionErreur). Silencieux (NIL_VAL si echec). */
Value owl_codebarre_image_value_from_params(int64_t cbType, int64_t ecc,
                                            int64_t options,
                                            const char* contenu, int contenuLen,
                                            int largeurMax, int hauteurMax) {
    const char* err = NULL;
    OwlImage* img = owl_codebarre_generate_owlimage(cbType, ecc, options,
                                                    contenu, contenuLen,
                                                    largeurMax, hauteurMax,
                                                    false, 0, false, 0, &err);
    if (!img) return NIL_VAL;
    return OBJ_VAL(image_make_instance(img));
}

static Value fn_codebarre_vers_image(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "CodeBarreVersImage",
                              "arg", "1", "expected", "CodeBarres",
                              "actual", "autre", NULL);
        return OBJ_VAL(image_make_instance(owl_image_new_empty()));
    }
    ObjInstance* cb = AS_INSTANCE(args[0]);

    /* Dimensions maximales optionnelles (bornes hautes, canon). */
    int largeurMax = (argCount >= 2 && IS_INTEGER(args[1])) ? (int)AS_INTEGER(args[1]) : 0;
    int hauteurMax = (argCount >= 3 && IS_INTEGER(args[2])) ? (int)AS_INTEGER(args[2]) : 0;
    bool hasBg   = (argCount >= 4 && IS_INTEGER(args[3]));
    int64_t bgC  = hasBg ? AS_INTEGER(args[3]) : 0;
    bool hasFg   = (argCount >= 5 && IS_INTEGER(args[4]));
    int64_t fgC  = hasFg ? AS_INTEGER(args[4]) : 0;

    const char* err = NULL;
    Value res = cb_image_value_from_instance(cb, largeurMax, hauteurMax,
                                             hasBg, bgC, hasFg, fgC, &err);
    if (IS_NIL(res))
        return cb_error_image(err ? err : "generation impossible");
    return res;
}

/* ================================================================
 * Enregistrement
 * ================================================================ */

void stdlib_codebarres_init(void) {
    static const OwlParam p_vers_image[] = {
        {"codeBarres",       "CodeBarres", "description du code-barres a generer"},
        {"largeurMax",       "entier",     "largeur maximale de l'image produite en pixels (optionnel)"},
        {"hauteurMax",       "entier",     "hauteur maximale de l'image produite en pixels (optionnel)"},
        {"couleurFond",      "couleur",    "couleur de fond (defaut : Blanc)"},
        {"couleurCodeBarres","couleur",    "couleur des barres/modules (defaut : Noir)"},
        {0}
    };
    static const OwlSignature sig_vers_image[] = {
        {1, p_vers_image, "Image", "image du code-barres genere"},
        {2, p_vers_image, "Image", "image bornee en largeur"},
        {3, p_vers_image, "Image", "image bornee en largeur et hauteur"},
        {4, p_vers_image, "Image", "image avec couleur de fond"},
        {5, p_vers_image, "Image", "image avec couleurs de fond et de barres"},
        {0}
    };

    /* Forme fluente : cb.VersImage() / cb.ToImage() (point simple = methode
     * prefixe sur un receiver CodeBarres). La fonction globale
     * CodeBarreVersImage(cb) coexiste (pattern OWL standard). */
    static const OwlMethodBinding CB_VERSIMAGE_METHODS[] = {
        {"CodeBarres", "VersImage", -1},
        {"CodeBarres", "ToImage",   -1},
        {NULL, NULL, 0}
    };

    static const OwlNativeDef DEFS[] = {
        {
            .canonical  = "CodeBarreVersImage",
            .nameEN     = "BarCodeToImage",
            .category   = "CodeBarres",
            .descFR     = "Genere une Image en memoire a partir d'un CodeBarres (contenu + symbologie). Les bornes largeurMax/hauteurMax plafonnent la taille du rendu ; les couleurs de fond et de barres sont optionnelles. Un QR Code peut incruster une image centrale (propriete ImageCentrale). Forme fluente disponible : cb.VersImage().",
            .descEN     = "Generate an in-memory Image from a BarCodes value (content + symbology). The largeurMax/hauteurMax bounds cap the rendered size; background and bar colors are optional. A QR Code can overlay a center image (CenterImage property). Fluent form available: cb.ToImage().",
            .signatures = sig_vers_image,
            .impl       = fn_codebarre_vers_image,
            .methodBindings = CB_VERSIMAGE_METHODS,
        },
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums(CB_ENUMS, (int)(sizeof(CB_ENUMS)/sizeof(CB_ENUMS[0])));
    owl_registry_register_consts(CB_CONSTS, (int)(sizeof(CB_CONSTS)/sizeof(CB_CONSTS[0])));
    owl_registry_register_types(CB_TYPES, (int)(sizeof(CB_TYPES)/sizeof(CB_TYPES[0])));

    /* Template runtime pour __StructClone : sous le nom canon ASCII et le
     * nom EN (le lookup vm.types est deja insensible casse+accents). */
    ObjInstance* tpl = build_codebarres_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("CodeBarres", OBJ_VAL(tpl));
    vm_type_register("BarCodes",   OBJ_VAL(tpl));
    vm_pop();
}
