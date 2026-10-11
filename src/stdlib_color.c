// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_color.c — Fonctions et constantes couleur.
 *
 * Reference :
 *   RVB(R,V,B)   → entier  : R + V*256 + B*65536  (BGR encoding)
 *   TSL(T,S,L)   → entier  : HSL → RGB → BGR encode
 *   RVBVersHTML(c) → chaine "#RRGGBB" standard HTML
 *   HTMLVersRVB(s) → entier
 *
 * Constantes : Blanc/Noir/Rouge.../Vert.../Bleu.../etc.
 *
 * Type Couleur : alias scanner d'entier (cf scanner.c). Permet
 *   `c est une Couleur = RougeClair` au lieu de `c est un entier`.
 *
 * Divergence assumee vs la doc d'origine : l'exemple "RVBVersHTML(255) // #0000FF"
 * de cette doc est manifestement faux. Coherence avec
 * le canon RVB : RougeClair=255 → "#FF0000". OWL retourne donc
 * "#FF0000" pour 255 (cohérent HTML standard #RRGGBB).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ====================================================================
 *  RVB / HTML helpers
 * ==================================================================== */

static int clamp_byte(int64_t v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (int)v;
}

/*  — clamp generique [0, max]. Utilise pour le range HSL 0-240
 * (canon : Teinte/Saturation/Luminosite expriment leurs 8 bits utiles
 * sur 0-240, pas 0-255, cf doc Couleur type). */
static int clamp_range(int64_t v, int max) {
    if (v < 0)   return 0;
    if (v > max) return max;
    return (int)v;
}

static Value fn_rvb(int argCount, Value* args) {
    if (argCount < 3) return INTEGER_VAL(0);
    int r = clamp_byte(IS_INTEGER(args[0]) ? AS_INTEGER(args[0]) : 0);
    int v = clamp_byte(IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : 0);
    int b = clamp_byte(IS_INTEGER(args[2]) ? AS_INTEGER(args[2]) : 0);
    int64_t couleur = (int64_t)r + (int64_t)v * 256 + (int64_t)b * 65536;
    return INTEGER_VAL(couleur);
}

static Value fn_rvb_vers_html(int argCount, Value* args) {
    if (argCount < 1 || !IS_INTEGER(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    int64_t c = AS_INTEGER(args[0]);
    char buf[8];
    /* Conversion partagée avec l'export --vscode-data (owl_color_bgr_to_html) :
     * une seule décomposition BGR→RGB dans la codebase. Négatif (Transparent /
     * sentinelle) → false → chaine vide. */
    if (!owl_color_bgr_to_html(c, buf, sizeof buf))
        return OBJ_VAL(obj_copy_string("", 0));
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* CSS Color Module Level 4 / W3C named colors (147 entries, stable
 * depuis CSS 2.1). Stockés sous forme RGB hex (0xRRGGBB) — la fonction
 * encode ensuite en BGR (canon : R + G*256 + B*65536). */
typedef struct { const char* name; uint32_t rgb; } HtmlNamedColor;

static const HtmlNamedColor HTML_NAMED_COLORS[] = {
    {"aliceblue",            0xF0F8FF}, {"antiquewhite",         0xFAEBD7},
    {"aqua",                 0x00FFFF}, {"aquamarine",           0x7FFFD4},
    {"azure",                0xF0FFFF}, {"beige",                0xF5F5DC},
    {"bisque",               0xFFE4C4}, {"black",                0x000000},
    {"blanchedalmond",       0xFFEBCD}, {"blue",                 0x0000FF},
    {"blueviolet",           0x8A2BE2}, {"brown",                0xA52A2A},
    {"burlywood",            0xDEB887}, {"cadetblue",            0x5F9EA0},
    {"chartreuse",           0x7FFF00}, {"chocolate",            0xD2691E},
    {"coral",                0xFF7F50}, {"cornflowerblue",       0x6495ED},
    {"cornsilk",             0xFFF8DC}, {"crimson",              0xDC143C},
    {"cyan",                 0x00FFFF}, {"darkblue",             0x00008B},
    {"darkcyan",             0x008B8B}, {"darkgoldenrod",        0xB8860B},
    {"darkgray",             0xA9A9A9}, {"darkgreen",            0x006400},
    {"darkgrey",             0xA9A9A9}, {"darkkhaki",            0xBDB76B},
    {"darkmagenta",          0x8B008B}, {"darkolivegreen",       0x556B2F},
    {"darkorange",           0xFF8C00}, {"darkorchid",           0x9932CC},
    {"darkred",              0x8B0000}, {"darksalmon",           0xE9967A},
    {"darkseagreen",         0x8FBC8F}, {"darkslateblue",        0x483D8B},
    {"darkslategray",        0x2F4F4F}, {"darkslategrey",        0x2F4F4F},
    {"darkturquoise",        0x00CED1}, {"darkviolet",           0x9400D3},
    {"deeppink",             0xFF1493}, {"deepskyblue",          0x00BFFF},
    {"dimgray",              0x696969}, {"dimgrey",              0x696969},
    {"dodgerblue",           0x1E90FF}, {"firebrick",            0xB22222},
    {"floralwhite",          0xFFFAF0}, {"forestgreen",          0x228B22},
    {"fuchsia",              0xFF00FF}, {"gainsboro",            0xDCDCDC},
    {"ghostwhite",           0xF8F8FF}, {"gold",                 0xFFD700},
    {"goldenrod",            0xDAA520}, {"gray",                 0x808080},
    {"green",                0x008000}, {"greenyellow",          0xADFF2F},
    {"grey",                 0x808080}, {"honeydew",             0xF0FFF0},
    {"hotpink",              0xFF69B4}, {"indianred",            0xCD5C5C},
    {"indigo",               0x4B0082}, {"ivory",                0xFFFFF0},
    {"khaki",                0xF0E68C}, {"lavender",             0xE6E6FA},
    {"lavenderblush",        0xFFF0F5}, {"lawngreen",            0x7CFC00},
    {"lemonchiffon",         0xFFFACD}, {"lightblue",            0xADD8E6},
    {"lightcoral",           0xF08080}, {"lightcyan",            0xE0FFFF},
    {"lightgoldenrodyellow", 0xFAFAD2}, {"lightgray",            0xD3D3D3},
    {"lightgreen",           0x90EE90}, {"lightgrey",            0xD3D3D3},
    {"lightpink",            0xFFB6C1}, {"lightsalmon",          0xFFA07A},
    {"lightseagreen",        0x20B2AA}, {"lightskyblue",         0x87CEFA},
    {"lightslategray",       0x778899}, {"lightslategrey",       0x778899},
    {"lightsteelblue",       0xB0C4DE}, {"lightyellow",          0xFFFFE0},
    {"lime",                 0x00FF00}, {"limegreen",            0x32CD32},
    {"linen",                0xFAF0E6}, {"magenta",              0xFF00FF},
    {"maroon",               0x800000}, {"mediumaquamarine",     0x66CDAA},
    {"mediumblue",           0x0000CD}, {"mediumorchid",         0xBA55D3},
    {"mediumpurple",         0x9370DB}, {"mediumseagreen",       0x3CB371},
    {"mediumslateblue",      0x7B68EE}, {"mediumspringgreen",    0x00FA9A},
    {"mediumturquoise",      0x48D1CC}, {"mediumvioletred",      0xC71585},
    {"midnightblue",         0x191970}, {"mintcream",            0xF5FFFA},
    {"mistyrose",            0xFFE4E1}, {"moccasin",             0xFFE4B5},
    {"navajowhite",          0xFFDEAD}, {"navy",                 0x000080},
    {"oldlace",              0xFDF5E6}, {"olive",                0x808000},
    {"olivedrab",            0x6B8E23}, {"orange",               0xFFA500},
    {"orangered",            0xFF4500}, {"orchid",               0xDA70D6},
    {"palegoldenrod",        0xEEE8AA}, {"palegreen",            0x98FB98},
    {"paleturquoise",        0xAFEEEE}, {"palevioletred",        0xDB7093},
    {"papayawhip",           0xFFEFD5}, {"peachpuff",            0xFFDAB9},
    {"peru",                 0xCD853F}, {"pink",                 0xFFC0CB},
    {"plum",                 0xDDA0DD}, {"powderblue",           0xB0E0E6},
    {"purple",               0x800080}, {"rebeccapurple",        0x663399},
    {"red",                  0xFF0000}, {"rosybrown",            0xBC8F8F},
    {"royalblue",            0x4169E1}, {"saddlebrown",          0x8B4513},
    {"salmon",               0xFA8072}, {"sandybrown",           0xF4A460},
    {"seagreen",             0x2E8B57}, {"seashell",             0xFFF5EE},
    {"sienna",               0xA0522D}, {"silver",               0xC0C0C0},
    {"skyblue",              0x87CEEB}, {"slateblue",            0x6A5ACD},
    {"slategray",            0x708090}, {"slategrey",            0x708090},
    {"snow",                 0xFFFAFA}, {"springgreen",          0x00FF7F},
    {"steelblue",            0x4682B4}, {"tan",                  0xD2B48C},
    {"teal",                 0x008080}, {"thistle",              0xD8BFD8},
    {"tomato",               0xFF6347}, {"transparent",          0x000000},
    {"turquoise",            0x40E0D0}, {"violet",               0xEE82EE},
    {"wheat",                0xF5DEB3}, {"white",                0xFFFFFF},
    {"whitesmoke",           0xF5F5F5}, {"yellow",               0xFFFF00},
    {"yellowgreen",          0x9ACD32},
};
#define HTML_NAMED_COLORS_COUNT (int)(sizeof(HTML_NAMED_COLORS)/sizeof(HTML_NAMED_COLORS[0]))

/* Lookup case-insensitive d'un nom CSS. Retourne -1 si non trouvé. */
static int64_t lookup_html_named_color(const char* s) {
    if (!s || !*s) return -1;
    for (int i = 0; i < HTML_NAMED_COLORS_COUNT; i++) {
        const char* a = s;
        const char* b = HTML_NAMED_COLORS[i].name;
        while (*a && *b) {
            char ca = (char)tolower((unsigned char)*a);
            if (ca != *b) break;
            a++; b++;
        }
        if (*a == '\0' && *b == '\0') {
            uint32_t rgb = HTML_NAMED_COLORS[i].rgb;
            int r = (int)((rgb >> 16) & 0xFF);
            int g = (int)((rgb >>  8) & 0xFF);
            int b8 = (int)( rgb        & 0xFF);
            return (int64_t)r + (int64_t)g * 256 + (int64_t)b8 * 65536;
        }
    }
    return -1;
}

static Value fn_html_vers_rvb(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) return INTEGER_VAL(0);
    const char* s = AS_CSTRING(args[0]);
    if (!s) return INTEGER_VAL(0);

    /* Forme nom symbolique CSS (case-insensitive) — tester avant le hex
     * car certains noms (ex: "fab", "abc") pourraient ressembler à du hex. */
    if (*s != '#') {
        int64_t named = lookup_html_named_color(s);
        if (named >= 0) return INTEGER_VAL(named);
    }

    if (*s == '#') s++;
    int len = (int)strlen(s);
    if (len != 6 && len != 3) return INTEGER_VAL(0);
    int r, g, b;
    if (len == 6) {
        int d0 = hex_digit(s[0]), d1 = hex_digit(s[1]);
        int d2 = hex_digit(s[2]), d3 = hex_digit(s[3]);
        int d4 = hex_digit(s[4]), d5 = hex_digit(s[5]);
        if (d0<0||d1<0||d2<0||d3<0||d4<0||d5<0) return INTEGER_VAL(0);
        r = d0 * 16 + d1; g = d2 * 16 + d3; b = d4 * 16 + d5;
    } else {
        int d0 = hex_digit(s[0]), d1 = hex_digit(s[1]), d2 = hex_digit(s[2]);
        if (d0<0||d1<0||d2<0) return INTEGER_VAL(0);
        r = d0 * 17; g = d1 * 17; b = d2 * 17; /* shorthand "#abc" → "#aabbcc" */
    }
    int64_t couleur = (int64_t)r + (int64_t)g * 256 + (int64_t)b * 65536;
    return INTEGER_VAL(couleur);
}

/* ====================================================================
 *  TSL — HSL→RGB→BGR
 * ==================================================================== */

static double hue2rgb(double p, double q, double t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0/6.0) return p + (q - p) * 6.0 * t;
    if (t < 1.0/2.0) return q;
    if (t < 2.0/3.0) return p + (q - p) * (2.0/3.0 - t) * 6.0;
    return p;
}

static Value fn_tsl(int argCount, Value* args) {
    if (argCount < 3) return INTEGER_VAL(0);
    int h = IS_INTEGER(args[0]) ? (int)AS_INTEGER(args[0]) : 0;
    int s = IS_INTEGER(args[1]) ? (int)AS_INTEGER(args[1]) : 0;
    int l = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;

    /* Normaliser h dans [0, 360[. */
    h = ((h % 360) + 360) % 360;
    if (s < 0) s = 0; else if (s > 100) s = 100;
    if (l < 0) l = 0; else if (l > 100) l = 100;

    double H = h / 360.0;
    double S = s / 100.0;
    double L = l / 100.0;

    double r, g, b;
    if (S == 0.0) {
        r = g = b = L;
    } else {
        double q = L < 0.5 ? L * (1.0 + S) : L + S - L * S;
        double p = 2.0 * L - q;
        r = hue2rgb(p, q, H + 1.0/3.0);
        g = hue2rgb(p, q, H);
        b = hue2rgb(p, q, H - 1.0/3.0);
    }

    int R = (int)(r * 255.0 + 0.5);
    int G = (int)(g * 255.0 + 0.5);
    int B = (int)(b * 255.0 + 0.5);
    if (R > 255) { R = 255; }
    if (G > 255) { G = 255; }
    if (B > 255) { B = 255; }
    if (R < 0)   { R = 0; }
    if (G < 0)   { G = 0; }
    if (B < 0)   { B = 0; }
    int64_t couleur = (int64_t)R + (int64_t)G * 256 + (int64_t)B * 65536;
    return INTEGER_VAL(couleur);
}

/* ====================================================================
 *  Registry : DEFS + signatures
 * ==================================================================== */

static const OwlParam p_rvb[]   = {
    {"rouge", "entier", "Composante rouge (0-255)"},
    {"vert",  "entier", "Composante verte (0-255)"},
    {"bleu",  "entier", "Composante bleue (0-255)"},
    {0}
};
static const OwlSignature sig_rvb[] = {
    { 3, p_rvb, "couleur", "Couleur encodee BGR sur 24 bits" },
    {0}
};

static const OwlParam p_tsl[]   = {
    {"teinte",      "entier", "Teinte 0-360 (rouge=0, vert=120, bleu=240)"},
    {"saturation",  "entier", "Saturation 0-100"},
    {"luminosite",  "entier", "Luminosite 0-100"},
    {0}
};
static const OwlSignature sig_tsl[] = {
    { 3, p_tsl, "couleur", "Couleur encodee BGR sur 24 bits" },
    {0}
};

static const OwlParam p_rvb2html[] = {
    {"couleur", "entier", "Couleur RVB (BGR encoded)"},
    {0}
};
static const OwlSignature sig_rvb2html[] = {
    { 1, p_rvb2html, "chaine", "Chaine HTML #RRGGBB" },
    {0}
};

static const OwlParam p_html2rvb[] = {
    {"html", "chaine", "Chaine HTML (#RRGGBB ou #RGB)"},
    {0}
};
static const OwlSignature sig_html2rvb[] = {
    { 1, p_html2rvb, "couleur", "Couleur BGR (0 si invalide)" },
    {0}
};

static const OwlNativeDef DEFS[] = {
    {
        .canonical   = "RVB",
        .nameEN      = "RGB",
        .category    = "Couleur",
        .descFR      = "Compose une couleur depuis ses composantes Rouge/Vert/Bleu (0-255). Encodage : R + V*256 + B*65536.",
        .descEN      = "Build a color from RGB components (0-255). Encoding: R + G*256 + B*65536.",
        .signatures  = sig_rvb,
        .impl        = fn_rvb,
        .flags       = OWL_NAT_PURE,
    },
    {
        .canonical   = "TSL",
        .nameEN      = "HSL",
        .category    = "Couleur",
        .descFR      = "Compose une couleur depuis Teinte (0-360), Saturation (0-100), Luminosite (0-100).",
        .descEN      = "Build a color from Hue (0-360), Saturation (0-100), Lightness (0-100).",
        .signatures  = sig_tsl,
        .impl        = fn_tsl,
        .flags       = OWL_NAT_PURE,
    },
    {
        .canonical   = "RVBVersHTML",
        .nameEN      = "RGBToHTML",
        .category    = "Couleur",
        .descFR      = "Convertit une couleur (entier BGR) en chaine HTML \"#RRGGBB\".",
        .descEN      = "Convert a BGR color integer to HTML \"#RRGGBB\" string.",
        .signatures  = sig_rvb2html,
        .impl        = fn_rvb_vers_html,
        .flags       = OWL_NAT_PURE,
    },
    {
        .canonical   = "HTMLVersRVB",
        .nameEN      = "HTMLToRGB",
        .category    = "Couleur",
        .descFR      = "Convertit une chaine HTML (#RRGGBB ou #RGB) en couleur BGR.",
        .descEN      = "Convert an HTML string (#RRGGBB or #RGB) to a BGR color.",
        .signatures  = sig_html2rvb,
        .impl        = fn_html_vers_rvb,
        .flags       = OWL_NAT_PURE,
    },
};

/* ====================================================================
 *  Constantes couleur predefinies
 * ==================================================================== */

#define C_COL(name, value, htmlhex) \
    { name, "Couleur", INTEGER_VAL(value), \
      "Couleur prédéfinie (HTML " htmlhex ").", \
      "Predefined color (HTML " htmlhex ")." }

/* Variante avec nameEN explicite (invariant bilingue n°3). Reserve aux
 * couleurs dont la forme EN est indiscutable (Rouge→Red, etc.). Le reste
 * des couleurs reste sur C_COL (nameEN=NULL) tant que la population EN
 * n'est pas faite — pas de self-test bloquant sur ce champ encore. */
#define C_COL_EN(name, value, htmlhex, en) \
    { name, "Couleur", INTEGER_VAL(value), \
      "Couleur prédéfinie (HTML " htmlhex ").", \
      "Predefined color (HTML " htmlhex ").", en }

static const OwlConstDef COLOR_CONSTS[] = {
    /* Speciales */
    C_COL_EN("Transparent", -1,       "transparent", "Transparent"),
    C_COL_EN("Blanc",      16777215,  "#FFFFFF", "White"),
    C_COL_EN("Noir",       0,         "#000000", "Black"),
    /* Primaires plein-vif : raccourci pratique pour les trois couleurs
     * RVB pures (255,0,0) / (0,255,0) / (0,0,255). */
    C_COL_EN("Rouge",      255,       "#FF0000", "Red"),
    C_COL_EN("Vert",       65280,     "#00FF00", "Green"),
    C_COL_EN("Bleu",       16711680,  "#0000FF", "Blue"),
    C_COL_EN("Argent",     12632256,  "#C0C0C0", "Silver"),
    C_COL_EN("Bronze",     3309517,   "#CD7F32", "Bronze"),
    C_COL_EN("JauneOr",    55295,     "#FFD700", "Gold"),
    /* Gris */
    C_COL_EN("GrisClair",  12632256,  "#C0C0C0", "LightGray"),
    C_COL_EN("GrisFonc\xc3\xa9", 8421504, "#808080", "DarkGray"),
    /* Rouge */
    C_COL_EN("RougePastel", 10066431, "#FF9999", "PastelRed"),
    C_COL_EN("RougeClair", 255,       "#FF0000", "LightRed"),
    C_COL_EN("RougeFonc\xc3\xa9", 128,   "#800000", "DarkRed"),
    /* Vert */
    C_COL_EN("VertPastel", 10092441,  "#99FF99", "PastelGreen"),
    C_COL_EN("VertClair",  65280,     "#00FF00", "LightGreen"),
    C_COL_EN("VertFonc\xc3\xa9", 32768,  "#008000", "DarkGreen"),
    /* Bleu */
    C_COL_EN("BleuPastel", 16751001,  "#9999FF", "PastelBlue"),
    C_COL_EN("BleuClair",  16711680,  "#0000FF", "LightBlue"),
    C_COL_EN("BleuFonc\xc3\xa9", 8388608, "#000080", "DarkBlue"),
    /* Cyan */
    C_COL_EN("CyanPastel", 16777113,  "#99FFFF", "PastelCyan"),
    C_COL_EN("CyanClair",  16776960,  "#00FFFF", "LightCyan"),
    C_COL_EN("CyanFonc\xc3\xa9", 8421376, "#008080", "DarkCyan"),
    /* Jaune */
    C_COL_EN("JaunePastel", 10092543, "#FFFF99", "PastelYellow"),
    C_COL_EN("JauneClair", 65535,     "#FFFF00", "LightYellow"),
    C_COL_EN("JauneFonc\xc3\xa9", 32896, "#808000", "DarkYellow"),
    C_COL_EN("JauneBulle", 15269887,  "#FFFFE8", "TooltipYellow"),
    /* Magenta */
    C_COL_EN("MagentaPastel", 16751103, "#FF99FF", "PastelMagenta"),
    C_COL_EN("MagentaClair", 16711935, "#FF00FF", "LightMagenta"),
    C_COL_EN("MagentaFonc\xc3\xa9", 8388736, "#800080", "DarkMagenta"),
    /* Rose */
    C_COL_EN("RosePastel", 14471679,  "#FFD1DC", "PastelPink"),
    C_COL_EN("RoseClair",  13353215,  "#FFC0CB", "LightPink"),
    C_COL_EN("RoseFonc\xc3\xa9", 10382589, "#FD6C9E", "DarkPink"),
    /* Violet */
    C_COL_EN("VioletPastel", 13212107, "#CB99C9", "PastelPurple"),
    C_COL_EN("VioletClair", 15631086, "#EE82EE", "LightPurple"),
    C_COL_EN("VioletFonc\xc3\xa9", 13828244, "#9400D3", "DarkPurple"),
    /* Marron */
    C_COL_EN("MarronPastel", 6510979, "#835963", "PastelBrown"),
    C_COL_EN("MarronClair", 2763429,  "#A52A2A", "LightBrown"),
    C_COL_EN("MarronFonc\xc3\xa9", 10584, "#582900", "DarkBrown"),
    /* Orange */
    C_COL_EN("OrangePastel", 4699135, "#FFB347", "PastelOrange"),
    C_COL_EN("OrangeClair", 42495,    "#FFA500", "LightOrange"),
    C_COL_EN("OrangeFonc\xc3\xa9", 36095, "#FF8C00", "DarkOrange"),
    /* Speciales canon : sentinelles OWL (valeurs negatives reservees). */
    C_COL_EN("CouleurD\xc3\xa9" "faut", -2, "default", "DefaultColor"),
    C_COL_EN("iSyst\xc3\xa8me",         -3, "system", "iSystem"),
};

/* ====================================================================
 *  Enum Couleur (rend les constantes classifiables comme enumMember par
 *  le LSP : coloration jaune dore italique).
 * ==================================================================== */

static const char* const E_Couleur_values[] = {
    "Transparent", "Blanc", "Noir",
    "Rouge", "Vert", "Bleu",
    "Argent", "Bronze", "JauneOr",
    "GrisClair", "Gris" "Fonc\xc3\xa9",
    "RougePastel", "RougeClair", "Rouge" "Fonc\xc3\xa9",
    "VertPastel",  "VertClair",  "Vert" "Fonc\xc3\xa9",
    "BleuPastel",  "BleuClair",  "Bleu" "Fonc\xc3\xa9",
    "CyanPastel",  "CyanClair",  "Cyan" "Fonc\xc3\xa9",
    "JaunePastel", "JauneClair", "Jaune" "Fonc\xc3\xa9", "JauneBulle",
    "MagentaPastel", "MagentaClair", "Magenta" "Fonc\xc3\xa9",
    "RosePastel",   "RoseClair",   "Rose" "Fonc\xc3\xa9",
    "VioletPastel", "VioletClair", "Violet" "Fonc\xc3\xa9",
    "MarronPastel", "MarronClair", "Marron" "Fonc\xc3\xa9",
    "OrangePastel", "OrangeClair", "Orange" "Fonc\xc3\xa9",
    "Couleur" "D\xc3\xa9" "faut", "iSyst\xc3\xa8me",
    /* Aliases ASCII : meme coloration enumMember que les versions accentuees. */
    "GrisFonce", "RougeFonce", "VertFonce", "BleuFonce", "CyanFonce",
    "JauneFonce", "MagentaFonce", "RoseFonce", "VioletFonce", "MarronFonce",
    "OrangeFonce", "CouleurDefaut", "iSysteme",
    NULL
};

const OwlEnum E_Couleur = {
    .name      = "couleur",
    .nameAscii = "couleur",
    .nameEN    = "color",
    .category  = "Couleur",
    .descFR    = "Couleurs prédéfinies.",
    .descEN    = "Predefined colors.",
    .values    = E_Couleur_values,
    .combinable= false,
    /* Opt-out sentinel : Blanc=0xFFFFFF est l'encodage RGB direct passe
     * au backend Qt (QColor::fromRgb). Sentinel-er detruirait la semantique
     * runtime et casserait `txt..Couleur = 0xFF0000` (forme litterale RGB). */
    .force_small_int = true,
};

/* Aliases ASCII runtime-only (cf CLAUDE.md : ne pas dans register_consts). */
#undef C_COL
#undef C_COL_EN

/* ====================================================================
 *   — catalogue couleurs exposé en lecture (--vscode-data).
 *
 *  Source unique : COLOR_CONSTS ci-dessus (les 44 constantes canon qui
 *  peuplent aussi l'enum couleur). Le picker du designer front consomme
 *  nom→hex au lieu de recopier la table à la main (source de dérive).
 *  Sentinelle = valeur stockée négative (Transparent/-1, CouleurDéfaut/-2,
 *  iSystème/-3) : pas de #RRGGBB fixe, le front décide de l'affichage.
 * ==================================================================== */

#define COLOR_CONSTS_COUNT ((int)(sizeof(COLOR_CONSTS) / sizeof(COLOR_CONSTS[0])))

int owl_color_catalog_count(void) { return COLOR_CONSTS_COUNT; }

const OwlColorInfo* owl_color_catalog_at(int index) {
    if (index < 0 || index >= COLOR_CONSTS_COUNT) return NULL;
    /* Vue paresseuse dérivée de COLOR_CONSTS (même pattern que le catalogue
     * langueXxx de stdlib_i18n.c). */
    static OwlColorInfo view[COLOR_CONSTS_COUNT];
    const OwlConstDef* c = &COLOR_CONSTS[index];
    int64_t v = IS_INTEGER(c->value) ? AS_INTEGER(c->value) : 0;
    view[index].nameFR   = c->name;
    view[index].nameEN   = c->nameEN;
    view[index].value    = v;
    view[index].sentinel = (v < 0);
    return &view[index];
}

bool owl_color_bgr_to_html(int64_t value, char* out, size_t outLen) {
    if (value < 0) return false;             /* sentinelle : pas de RGB fixe */
    int r = (int)( value        & 0xFF);
    int g = (int)((value >> 8 ) & 0xFF);
    int b = (int)((value >> 16) & 0xFF);
    snprintf(out, outLen, "#%02X%02X%02X", r, g, b);
    return true;
}

/* ====================================================================
 *   — Type Couleur (composite canon)
 *
 *  8 proprietes, range 0-255 pour R/V/B/Opacite, 0-240 pour T/S/L
 *  (canon "Couleur (Type de variable)"). La
 *  propriete .Couleur contient la valeur entiere packee BGR 24 bits et
 *  sert de propriete par defaut (scalar unwrap).
 *
 *  Mecanisme : a la coerce (`var est une Couleur = <entier>`), on
 *  decompose l'entier en R/V/B + on calcule T/S/L + Opacite=255. Sur
 *  mutation d'un sous-field, onFieldSet rebalance les autres
 *  composantes pour conserver la coherence (.Couleur reflete toujours
 *  R/V/B, T/S/L cohérent avec R/V/B, Opacite independant).
 * ==================================================================== */

/* Conversion RGB (0-255 chacun) -> HSL canon (0-240 chacun). Algorithme
 * standard HSL avec scaling final vers 0-240 au lieu de [0,1]. */
static void rgb_to_hsl240(int R, int G, int B, int* outH, int* outS, int* outL) {
    double r = R / 255.0, g = G / 255.0, b = B / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    double mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double l = (mx + mn) / 2.0;
    double h = 0.0, s = 0.0;
    if (mx != mn) {
        double d = mx - mn;
        s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
        if (mx == r)      h = (g - b) / d + (g < b ? 6.0 : 0.0);
        else if (mx == g) h = (b - r) / d + 2.0;
        else              h = (r - g) / d + 4.0;
        h /= 6.0;
    }
    /* Scaling 0-240 canon. */
    int H = (int)(h * 240.0 + 0.5);
    int S = (int)(s * 240.0 + 0.5);
    int L = (int)(l * 240.0 + 0.5);
    if (H >= 240) H = 0;  /* hue wraps */
    if (S > 240) S = 240;
    if (L > 240) L = 240;
    *outH = H; *outS = S; *outL = L;
}

/* Conversion HSL canon (0-240) -> RGB (0-255). */
static void hsl240_to_rgb(int H, int S, int L, int* outR, int* outG, int* outB) {
    double h = H / 240.0;
    double s = S / 240.0;
    double l = L / 240.0;
    double r, g, b;
    if (s == 0.0) {
        r = g = b = l;
    } else {
        double q = l < 0.5 ? l * (1.0 + s) : l + s - l * s;
        double p = 2.0 * l - q;
        r = hue2rgb(p, q, h + 1.0/3.0);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.0/3.0);
    }
    int R = (int)(r * 255.0 + 0.5);
    int G = (int)(g * 255.0 + 0.5);
    int B = (int)(b * 255.0 + 0.5);
    if (R > 255) R = 255; if (R < 0) R = 0;
    if (G > 255) G = 255; if (G < 0) G = 0;
    if (B > 255) B = 255; if (B < 0) B = 0;
    *outR = R; *outG = G; *outB = B;
}

/* FIELDS_Couleur — 8 proprietes canon. Toutes en "entier". Storage
 * canon ASCII FR (Rouge/Vert/Bleu/Teinte/Saturation/Luminosite/Opacite/
 * Couleur), nameEN obligatoire (rule 11). */
static const OwlField FIELDS_Couleur[] = {
    {"Rouge",      NULL,             "entier", "0",
     "Composante rouge (0-255).",
     "Red component (0-255).",
     .nameEN="Red"},
    {"Vert",       NULL,             "entier", "0",
     "Composante verte (0-255).",
     "Green component (0-255).",
     .nameEN="Green"},
    {"Bleu",       NULL,             "entier", "0",
     "Composante bleue (0-255).",
     "Blue component (0-255).",
     .nameEN="Blue"},
    {"Teinte",     NULL,             "entier", "0",
     "Teinte HSL (0-240). 0=rouge, 80=vert, 160=bleu.",
     "HSL hue (0-240). 0=red, 80=green, 160=blue.",
     .nameEN="Hue"},
    {"Saturation", NULL,             "entier", "0",
     "Saturation HSL (0-240). 0=gris, 240=couleur pure.",
     "HSL saturation (0-240). 0=gray, 240=pure color.",
     .nameEN="Saturation"},
    {"Luminosite", "Luminosit\xc3\xa9", "entier", "0",
     "Luminosite HSL (0-240). 0=noir, 120=couleur pleine, 240=blanc.",
     "HSL luminance (0-240). 0=black, 120=full color, 240=white.",
     .nameEN="Luminance"},
    {"Opacite",    "Opacit\xc3\xa9",    "entier", "255",
     "Canal alpha (0-255). 0=transparent, 255=opaque. Independant de .Couleur (24 bits BGR).",
     "Alpha channel (0-255). 0=transparent, 255=opaque. Independent from .Couleur (24-bit BGR).",
     .nameEN="Opacity"},
    {"Couleur",    NULL,             "entier", "0",
     "Valeur entiere packee BGR 24 bits (R + V*256 + B*65536). Propriete par defaut.",
     "Packed BGR 24-bit integer (R + V*256 + B*65536). Default property.",
     .allowedBase=&E_Couleur, .nameEN="Color"},
    {0},
};

/* Cle ASCII canon de chaque field, utilisee par couleur_on_field_set et
 * couleur_set_field_bypass pour ecrire sans re-declencher le hook. */
#define COULEUR_FIELD_ROUGE       "Rouge"
#define COULEUR_FIELD_VERT        "Vert"
#define COULEUR_FIELD_BLEU        "Bleu"
#define COULEUR_FIELD_TEINTE      "Teinte"
#define COULEUR_FIELD_SATURATION  "Saturation"
#define COULEUR_FIELD_LUMINOSITE  "Luminosite"
#define COULEUR_FIELD_OPACITE     "Opacite"
#define COULEUR_FIELD_COULEUR     "Couleur"

/* Helper : ecrit un field entier directement sur le storage de l'instance.
 * Bypass le dispatch OP_STRUCT_SET donc N'APPELLE PAS le hook onFieldSet —
 * c'est le point cle pour eviter la recursion infinie quand le hook
 * rebalance lui-meme les fields. */
static void couleur_set_int(ObjInstance* inst, const char* fieldName, int64_t val) {
    /*  lot 6 : delegate to canonical helper (roots inst+key avant
     * table_set qui peut grow et declencher le GC). Le bug "fieldName=NULL"
     * observe dans OWL_COMPOSED_TYPE_CATALOG sous stress-GC venait de
     * cette key non rootee lors de la construction du template Couleur. */
    inst_field_set_int(inst, fieldName, val);
}

static int64_t couleur_get_int(ObjInstance* inst, const char* fieldName) {
    /*  lot 6 : root inst pendant obj_copy_string. */
    vm_push(OBJ_VAL(inst));
    ObjString* k = obj_copy_string(fieldName, (int)strlen(fieldName));
    vm_pop();
    Value v;
    if (table_get(&inst->fields, k, &v) && IS_INTEGER(v)) {
        return AS_INTEGER(v);
    }
    return 0;
}

/* Remplit les 8 fields de `inst` a partir de l'entier packed BGR `packed`.
 * Opacite = 255 par defaut (opaque) — independante du packed 24 bits. */
static void couleur_fill_from_packed(ObjInstance* inst, int64_t packed) {
    /* Cas special Transparent (-1) : on stocke -1 dans .Couleur, R/V/B/HSL
     * a 0, Opacite 0 (transparent). Aligne avec RVBVersHTML qui rend ""
     * pour les valeurs negatives. */
    if (packed < 0) {
        couleur_set_int(inst, COULEUR_FIELD_ROUGE,      0);
        couleur_set_int(inst, COULEUR_FIELD_VERT,       0);
        couleur_set_int(inst, COULEUR_FIELD_BLEU,       0);
        couleur_set_int(inst, COULEUR_FIELD_TEINTE,     0);
        couleur_set_int(inst, COULEUR_FIELD_SATURATION, 0);
        couleur_set_int(inst, COULEUR_FIELD_LUMINOSITE, 0);
        couleur_set_int(inst, COULEUR_FIELD_OPACITE,    0);
        couleur_set_int(inst, COULEUR_FIELD_COULEUR,    packed);
        return;
    }
    int R = (int)( packed        & 0xFF);
    int V = (int)((packed >> 8 ) & 0xFF);
    int B = (int)((packed >> 16) & 0xFF);
    int H, S, L;
    rgb_to_hsl240(R, V, B, &H, &S, &L);
    couleur_set_int(inst, COULEUR_FIELD_ROUGE,      R);
    couleur_set_int(inst, COULEUR_FIELD_VERT,       V);
    couleur_set_int(inst, COULEUR_FIELD_BLEU,       B);
    couleur_set_int(inst, COULEUR_FIELD_TEINTE,     H);
    couleur_set_int(inst, COULEUR_FIELD_SATURATION, S);
    couleur_set_int(inst, COULEUR_FIELD_LUMINOSITE, L);
    couleur_set_int(inst, COULEUR_FIELD_OPACITE,    255);
    couleur_set_int(inst, COULEUR_FIELD_COULEUR,    packed);
}

/*  — onFieldSet de Couleur. Rebalance les fields derives selon
 * le field qui vient d'etre mute. Garde re-entrance via flag thread-local
 * (vu que la VM est mono-thread, un simple static bool suffit). */
static bool g_couleur_rebalancing = false;

static void couleur_on_field_set(ObjInstance* inst, const char* fieldName) {
    if (g_couleur_rebalancing) return;
    if (inst == NULL || fieldName == NULL) return;
    g_couleur_rebalancing = true;

    if (strcmp(fieldName, COULEUR_FIELD_ROUGE) == 0 ||
        strcmp(fieldName, COULEUR_FIELD_VERT)  == 0 ||
        strcmp(fieldName, COULEUR_FIELD_BLEU)  == 0) {
        /* RGB mute -> recalcul packed + HSL. */
        int R = clamp_byte(couleur_get_int(inst, COULEUR_FIELD_ROUGE));
        int V = clamp_byte(couleur_get_int(inst, COULEUR_FIELD_VERT));
        int B = clamp_byte(couleur_get_int(inst, COULEUR_FIELD_BLEU));
        couleur_set_int(inst, COULEUR_FIELD_ROUGE, R);
        couleur_set_int(inst, COULEUR_FIELD_VERT,  V);
        couleur_set_int(inst, COULEUR_FIELD_BLEU,  B);
        int H, S, L;
        rgb_to_hsl240(R, V, B, &H, &S, &L);
        couleur_set_int(inst, COULEUR_FIELD_TEINTE,     H);
        couleur_set_int(inst, COULEUR_FIELD_SATURATION, S);
        couleur_set_int(inst, COULEUR_FIELD_LUMINOSITE, L);
        int64_t packed = (int64_t)R + (int64_t)V * 256 + (int64_t)B * 65536;
        couleur_set_int(inst, COULEUR_FIELD_COULEUR, packed);
    } else if (strcmp(fieldName, COULEUR_FIELD_TEINTE)     == 0 ||
               strcmp(fieldName, COULEUR_FIELD_SATURATION) == 0 ||
               strcmp(fieldName, COULEUR_FIELD_LUMINOSITE) == 0) {
        /* HSL mute -> recalcul RGB + packed. */
        int H = clamp_range(couleur_get_int(inst, COULEUR_FIELD_TEINTE),     240);
        int S = clamp_range(couleur_get_int(inst, COULEUR_FIELD_SATURATION), 240);
        int L = clamp_range(couleur_get_int(inst, COULEUR_FIELD_LUMINOSITE), 240);
        couleur_set_int(inst, COULEUR_FIELD_TEINTE,     H);
        couleur_set_int(inst, COULEUR_FIELD_SATURATION, S);
        couleur_set_int(inst, COULEUR_FIELD_LUMINOSITE, L);
        int R, V, B;
        hsl240_to_rgb(H, S, L, &R, &V, &B);
        couleur_set_int(inst, COULEUR_FIELD_ROUGE, R);
        couleur_set_int(inst, COULEUR_FIELD_VERT,  V);
        couleur_set_int(inst, COULEUR_FIELD_BLEU,  B);
        int64_t packed = (int64_t)R + (int64_t)V * 256 + (int64_t)B * 65536;
        couleur_set_int(inst, COULEUR_FIELD_COULEUR, packed);
    } else if (strcmp(fieldName, COULEUR_FIELD_COULEUR) == 0) {
        /* .Couleur reaffectee directement -> recompose tout depuis le packed.
         * On preserve Opacite courante (ne pas la reset a 255 silencieusement). */
        int64_t packed = couleur_get_int(inst, COULEUR_FIELD_COULEUR);
        int64_t alpha = couleur_get_int(inst, COULEUR_FIELD_OPACITE);
        couleur_fill_from_packed(inst, packed);
        couleur_set_int(inst, COULEUR_FIELD_OPACITE, alpha);
    } else if (strcmp(fieldName, COULEUR_FIELD_OPACITE) == 0) {
        /* Opacite mute : clamp 0-255, pas de rebalancing RGB/HSL/packed. */
        int A = clamp_byte(couleur_get_int(inst, COULEUR_FIELD_OPACITE));
        couleur_set_int(inst, COULEUR_FIELD_OPACITE, A);
    }
    /* Field inconnu : no-op silencieux (l'utilisateur a ecrit un field
     * hors-canon, rien a rebalancer). */

    g_couleur_rebalancing = false;
}

/* Cree une nouvelle ObjInstance Couleur initialisee a partir d'un entier
 * packed. Si packed=0, instance noir Opacite=255. */
static ObjInstance* couleur_new_instance(int64_t packed) {
    ObjString* name = obj_copy_string("Couleur", 7);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_pop();  /* name retenu par inst->structName */
    vm_push(OBJ_VAL(inst));
    couleur_fill_from_packed(inst, packed);
    vm_pop();
    return inst;
}

/* Template global (exporte pour register_global_instance dans
 * register_builtin_types — `c est une Couleur` clone ce template via
 * __StructClone). */
ObjInstance* build_Couleur_template(void) {
    return couleur_new_instance(0);
}

/* Parser de coerce-in : `var est une Couleur = <entier ou Couleur>`. */
static Value parse_to_couleur(Value v) {
    /* Pass-through si deja une ObjInstance Couleur. */
    if (IS_INSTANCE(v)) {
        ObjInstance* in = AS_INSTANCE(v);
        if (in->structName != NULL
            && strcmp(in->structName->chars, "Couleur") == 0) {
            return v;
        }
        /* Autre type composite : tente de deballer sa defaultProperty si
         * elle se reduit a un entier, sinon repli vers 0. */
        Value unwrapped = owl_instance_default_unwrap(v);
        if (IS_INTEGER(unwrapped)) {
            return OBJ_VAL(couleur_new_instance(AS_INTEGER(unwrapped)));
        }
        return OBJ_VAL(couleur_new_instance(0));
    }
    int64_t packed = 0;
    if (IS_INTEGER(v))  packed = AS_INTEGER(v);
    else if (IS_REAL(v)) packed = (int64_t)AS_REAL(v);
    else if (IS_BOOL(v)) packed = AS_BOOL(v) ? 1 : 0;
    else if (IS_NIL(v))  packed = 0;
    else if (IS_STRING(v)) {
        /* "Couleur" depuis chaine non standard : tente HTMLVersRVB.
         * Repli : 0. */
        Value args[1] = { v };
        Value res = fn_html_vers_rvb(1, args);
        packed = IS_INTEGER(res) ? AS_INTEGER(res) : 0;
    }
    return OBJ_VAL(couleur_new_instance(packed));
}

/* OwlTypeDef Couleur — declare apres FIELDS_Couleur. Note : fields !=NULL
 * + hasDefault=false force le path __StructClone(template) cote compilo,
 * qui clone l'instance globale enregistree par register_builtin_types. */
static const char* const ALIAS_Couleur[] = { "Color", NULL };

static const OwlTypeDef COULEUR_TYPES[] = {
    {
        .canonical       = "Couleur",
        .canonicalAscii  = "Couleur",
        .aliases         = ALIAS_Couleur,
        .nameEN          = "Color",
        .category        = "Couleur",
        .descFR          = "Type compose representant une couleur. 8 proprietes : Rouge/Vert/Bleu (0-255), Teinte/Saturation/Luminosite (0-240), Opacite (0-255, alpha independant), Couleur (entier BGR packe 24 bits, propriete par defaut). Affectation par entier ou constante (JauneClair, BleuFonce...) decompose automatiquement les composantes.",
        .descEN          = "Composite type representing a color. 8 properties: Red/Green/Blue (0-255), Hue/Saturation/Luminance (0-240), Opacity (0-255, independent alpha channel), Color (24-bit packed BGR integer, default property). Assignment via integer or predefined constant (JauneClair, BleuFonce...) decomposes components automatically.",
        .fields          = FIELDS_Couleur,
        .coercible       = true,
        .defaultProperty = "Couleur",
        .onFieldSet      = couleur_on_field_set,
        .genre           = OWL_GENRE_F,   /* une couleur */
    },
};

/* ====================================================================
 *  Init
 * ==================================================================== */

void stdlib_color_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    /* Register l'enum Couleur (avant les consts) pour que le LSP catalog
     * gen capte les noms comme enum members → coloration enumMember. */
    static const OwlEnum* COLOR_ENUMS[] = { &E_Couleur };
    owl_registry_register_enums(COLOR_ENUMS,
                                 (int)(sizeof(COLOR_ENUMS) / sizeof(COLOR_ENUMS[0])));
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * pour les noms accentues (GrisFonce/RougeFonce/iSysteme/...). */
    owl_registry_register_consts(COLOR_CONSTS,
                                  (int)(sizeof(COLOR_CONSTS) / sizeof(COLOR_CONSTS[0])));
    /*  — Type Couleur composite + parser de coerce. */
    owl_registry_register_types(COULEUR_TYPES,
                                 (int)(sizeof(COULEUR_TYPES) / sizeof(COULEUR_TYPES[0])));
    owl_register_instance_coerce("Couleur", parse_to_couleur);
    /* Le template global `Couleur` (utilise par __StructClone pour les
     * declarations sans init) est registered depuis register_builtin_types
     * via build_Couleur_template() — uniformite avec Police/Cadre etc. */
}
