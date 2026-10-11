// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * stdlib_xls.c — Type `xlsDocument` + ecriture/lecture de classeurs Excel
 * (.xlsx, OOXML SpreadsheetML). Premier lot du chantier bureautique.
 *
 * Modele : un xlsDocument (ObjXlsDocument, cf object.h) est une pile ordonnee
 * de feuilles ; chaque feuille porte ses cellules (valeur = chaine UTF-8) et
 * ses plages fusionnees. Une cellule s'ecrit/se lit par indexation
 * `doc[ligne, colonne]` (subscript 2D, dispatche cote VM) ou via xlsDonnée.
 *
 * Ecriture : l'archive .xlsx est un ZIP de fichiers XML. On assemble a la main
 * les parties minimales exigees ([Content_Types].xml, _rels/.rels,
 * xl/workbook.xml (+ .rels), xl/styles.xml, xl/sharedStrings.xml et une
 * xl/worksheets/sheetN.xml par feuille), puis on empaquette via miniz. Le
 * fichier produit s'ouvre sans reparation dans Excel et LibreOffice.
 *
 * Lecture : xlsOuvre depaquette le .xlsx (miniz) et reparse workbook.xml,
 * sharedStrings.xml, styles.xml et chaque worksheet (libxml2) pour reconstruire
 * les feuilles, leurs cellules texte ET leurs styles — round-trip complet du
 * writer. Les parts qu'OWL ne modelise pas (macros, graphiques, medias) sont
 * capturees verbatim a l'ouverture et reinjectees a xlsSauve. Le format est
 * reniflé par signature : ZIP -> lecteur xlsx, OLE2 (.xls binaire hérité
 * BIFF8) -> lecteur libxls (xls_read_ole2_file). Ce dernier lit les VALEURS
 * (texte + numérique) ; les styles/dates du .xls restent un lot ulterieur.
 *
 * Hors-scope : styles/format de date du .xls binaire, formules recalculees.
 */

#include "owl_stdlib.h"
#include "owl_ui.h"   /*  : prototype owl_ui_table_export_xlsx (export data->.xlsx) */
#include "owl_registry.h"
#define OWL_ERRORS_SKIP_COMPILE_AWARE  /* éviter scanner.h vs windows.h TokenType */
#include "owl_errors.h"
#include "memory.h"
#include "vm.h"
#include "object.h"
#include "table.h"
#include "sentinels.h"   /* OWL_CH_* / OWL_CV_* / OWL_TC_* pour la génération styles.xml */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "miniz.h"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "xls.h"   /* libxls 1.6.3 — lecteur .xls binaire hérité (BIFF8/OLE2) */

/* strdup portable (evite _strdup Windows / la variante POSIX non-standard). */
static char* xls_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ================================================================
 * Constantes — catégorie "Tableur"
 * ================================================================ */

/* Modes d'ouverture (xlsOuvre). Valeurs de sentinelle internes ; le moteur
 * charge le classeur completement quel que soit le mode. */
#define XLS_MODE_ECRITURE    2
#define XLS_MODE_COMPATIBLE  1

/* Type de coordonnee demande a xlsNomVersCoordonnées. */
#define XLS_NUM_LIGNE        1
#define XLS_NUM_COLONNE      2

/* Type de fusion (xlsFusionneCellule, 6e/4e argument optionnel). */
#define XLS_FUSION_CENTREE   0
#define XLS_FUSION_SIMPLE    1

#define C_XLS(nm, val, dfr, den, nen) {nm, "Tableur", INTEGER_VAL(val), dfr, den, nen}

static const OwlConstDef XLS_CONSTS[] = {
    C_XLS("xlsEcriture", XLS_MODE_ECRITURE,
          "Ouvre le classeur en lecture / écriture (modifications enregistrables).",
          "Opens the workbook for reading and writing (changes can be saved).",
          "xlsWrite"),
    C_XLS("xlsCompatible", XLS_MODE_COMPATIBLE,
          "Ouvre le classeur en mode compatible (une seule feuille, conservé pour interopérabilité).",
          "Opens the workbook in compatibility mode (single sheet, kept for interoperability).",
          "xlsCompatible"),
    C_XLS("xlsNumLigne", XLS_NUM_LIGNE,
          "Demande le numéro de ligne à xlsNomVersCoordonnées.",
          "Requests the row number from xlsNameToCoordinates.",
          "xlsNumRow"),
    C_XLS("xlsNumColonne", XLS_NUM_COLONNE,
          "Demande le numéro de colonne à xlsNomVersCoordonnées.",
          "Requests the column number from xlsNameToCoordinates.",
          "xlsNumColumn"),
    C_XLS("xlsFusionCelluleCentr\xc3\xa9""e", XLS_FUSION_CENTREE,
          "Fusionne une plage en centrant le contenu (valeur par défaut).",
          "Merges a range and centers the content (default value).",
          "xlsMergeCellCentered"),
    C_XLS("xlsFusionCelluleSimple", XLS_FUSION_SIMPLE,
          "Fusionne une plage sans centrer le contenu.",
          "Merges a range without centering the content.",
          "xlsMergeCellSimple"),
};

static const char* const E_xlsCoordType_values[] = {
    "xlsNumLigne", "xlsNumColonne", NULL
};
static const OwlEnum E_xlsCoordType = {
    .name="xlsTypeCoordonn\xc3\xa9""e", .nameAscii="xlsTypeCoordonnee",
    .nameEN="xlsCoordinateType", .category="Tableur",
    .descFR="Composante renvoyée par xlsNomVersCoordonnées : numéro de ligne ou de colonne.",
    .descEN="Component returned by xlsNameToCoordinates: row number or column number.",
    .values=E_xlsCoordType_values, .combinable=false, .force_small_int=true
};

/* Les constantes xlsFusionCelluleCentrée/Simple restent des constantes
 * autonomes (pas d'OwlEnum) : le paramètre Type de fusion optionnel n'est pas
 * exposé en signature dans ce lot, donc une enum câblée resterait sans effet
 * compile-time (self-test rule enum_unreferenced). */

static const OwlEnum* const XLS_ENUMS[] = {
    &E_xlsCoordType
};

/* ================================================================
 * Helpers coordonnées <-> nom de cellule
 * ================================================================ */

/* Numéro de colonne 1-based -> lettres ("A", "Z", "AA"...). Écrit dans buf. */
static void xls_col_to_letters(int col, char* buf, size_t bufSize) {
    char tmp[16];
    int n = 0;
    if (col < 1) col = 1;
    while (col > 0 && n < (int)sizeof(tmp)) {
        int rem = (col - 1) % 26;
        tmp[n++] = (char)('A' + rem);
        col = (col - 1) / 26;
    }
    int j = 0;
    while (n > 0 && j < (int)bufSize - 1) buf[j++] = tmp[--n];
    buf[j] = '\0';
}

/* Lettres de colonne -> numéro 1-based. Renvoie 0 si aucune lettre. */
static int xls_letters_to_col(const char* s, int len) {
    int col = 0, count = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c < 'A' || c > 'Z') break;
        col = col * 26 + (c - 'A' + 1);
        count++;
    }
    return count > 0 ? col : 0;
}

/* Parse un nom de cellule "B12" -> col=2, row=12. Renvoie true si valide. */
static bool xls_parse_cell_name(const char* s, int* outCol, int* outRow) {
    int i = 0;
    int col = 0;
    while (s[i] && isalpha((unsigned char)s[i])) {
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        col = col * 26 + (c - 'A' + 1);
        i++;
    }
    if (col == 0) return false;
    int row = 0, digits = 0;
    while (s[i] && isdigit((unsigned char)s[i])) {
        row = row * 10 + (s[i] - '0');
        i++; digits++;
    }
    if (digits == 0 || s[i] != '\0') return false;
    *outCol = col; *outRow = row;
    return true;
}

/* ================================================================
 * Manipulation du modèle (feuilles / cellules / fusions)
 * ================================================================ */

static XlsSheet* xls_current_sheet(ObjXlsDocument* d) {
    if (d->sheetCount == 0) return NULL;
    if (d->current < 0) d->current = 0;
    if (d->current >= d->sheetCount) d->current = d->sheetCount - 1;
    return &d->sheets[d->current];
}

static XlsCell* xls_find_cell(XlsSheet* s, int row, int col) {
    for (int i = 0; i < s->cellCount; i++)
        if (s->cells[i].row == row && s->cells[i].col == col)
            return &s->cells[i];
    return NULL;
}

/*  — remet le style d'une cellule fraiche a "non defini". */
static void xls_cell_init_style(XlsCell* c) {
    c->bgColor = 0;    c->hasBg = false;
    c->alignH = 0;     c->hasAlignH = false;
    c->alignV = 0;     c->hasAlignV = false;
    c->multiline = false; c->hasMultiline = false;
    c->mask = NULL;
    c->font = NIL_VAL;
    c->border = NIL_VAL;
}

/*  — cellule (row,col) existante, creee vide (style neutre) si absente.
 * Le pointeur reste valide tant qu'aucune autre insertion ne realloc `cells`. */
static XlsCell* xls_cell_ensure(XlsSheet* s, int row, int col) {
    XlsCell* c = xls_find_cell(s, row, col);
    if (c != NULL) return c;
    if (s->cellCount >= s->cellCap) {
        int nc = s->cellCap ? s->cellCap * 2 : 8;
        s->cells = (XlsCell*)realloc(s->cells, sizeof(XlsCell) * (size_t)nc);
        s->cellCap = nc;
    }
    c = &s->cells[s->cellCount++];
    c->row = row; c->col = col; c->value = NULL; c->isNumber = false;
    xls_cell_init_style(c);
    return c;
}

/*  — hauteur de ligne / largeur de colonne (creux, 1-based). */
static void xls_dim_set(XlsDim** arr, int* count, int* cap, int index, double val) {
    for (int i = 0; i < *count; i++) if ((*arr)[i].index == index) { (*arr)[i].value = val; return; }
    if (*count >= *cap) {
        int nc = *cap ? *cap * 2 : 4;
        *arr = (XlsDim*)realloc(*arr, sizeof(XlsDim) * (size_t)nc);
        *cap = nc;
    }
    (*arr)[*count].index = index; (*arr)[*count].value = val; (*count)++;
}
static bool xls_dim_get(XlsDim* arr, int count, int index, double* out) {
    for (int i = 0; i < count; i++) if (arr[i].index == index) { *out = arr[i].value; return true; }
    return false;
}

/* Pose la valeur (chaine UTF-8 dupliquee) d'une cellule d'une feuille. */
static void xls_sheet_set(XlsSheet* s, int row, int col, const char* utf8, int len) {
    XlsCell* c = xls_find_cell(s, row, col);
    if (c == NULL) {
        if (s->cellCount >= s->cellCap) {
            int nc = s->cellCap ? s->cellCap * 2 : 8;
            s->cells = (XlsCell*)realloc(s->cells, sizeof(XlsCell) * (size_t)nc);
            s->cellCap = nc;
        }
        c = &s->cells[s->cellCount++];
        c->row = row; c->col = col; c->value = NULL; c->isNumber = false;
        xls_cell_init_style(c);
    }
    free(c->value);
    c->value = (char*)malloc((size_t)len + 1);
    memcpy(c->value, utf8, (size_t)len);
    c->value[len] = '\0';
}

static void xls_sheet_add_merge(XlsSheet* s, int fr, int fc, int lr, int lc) {
    if (s->mergeCount >= s->mergeCap) {
        int nc = s->mergeCap ? s->mergeCap * 2 : 4;
        s->merges = (XlsMerge*)realloc(s->merges, sizeof(XlsMerge) * (size_t)nc);
        s->mergeCap = nc;
    }
    XlsMerge* m = &s->merges[s->mergeCount++];
    m->firstRow = fr; m->firstCol = fc; m->lastRow = lr; m->lastCol = lc;
}

/* Ajoute une feuille vide nommée `name` (dupliqué). Renvoie l'index. */
static int xls_doc_add_sheet(ObjXlsDocument* d, const char* name) {
    if (d->sheetCount >= d->sheetCap) {
        int nc = d->sheetCap ? d->sheetCap * 2 : 4;
        d->sheets = (XlsSheet*)realloc(d->sheets, sizeof(XlsSheet) * (size_t)nc);
        d->sheetCap = nc;
    }
    XlsSheet* s = &d->sheets[d->sheetCount];
    size_t nl = strlen(name);
    s->name = (char*)malloc(nl + 1);
    memcpy(s->name, name, nl + 1);
    s->cells = NULL; s->cellCount = 0; s->cellCap = 0;
    s->merges = NULL; s->mergeCount = 0; s->mergeCap = 0;
    s->rowDims = NULL; s->rowDimCount = 0; s->rowDimCap = 0;  /*  */
    s->colDims = NULL; s->colDimCount = 0; s->colDimCap = 0;  /*  */
    return d->sheetCount++;
}

/* Réinitialise le document à vide (utilisé par xlsFerme et avant relecture). */
static void xls_doc_clear(ObjXlsDocument* d) {
    for (int i = 0; i < d->sheetCount; i++) {
        XlsSheet* s = &d->sheets[i];
        for (int c = 0; c < s->cellCount; c++) {
            free(s->cells[c].value);
            free(s->cells[c].mask);   /*  — font/border = Value, GC */
        }
        free(s->cells);
        free(s->merges);
        free(s->rowDims);            /*  */
        free(s->colDims);            /*  */
        free(s->name);
    }
    free(d->sheets);
    d->sheets = NULL; d->sheetCount = 0; d->sheetCap = 0; d->current = 0;
    /*  — parts ZIP preservees (octets bruts). Liberees a xlsFerme et
     * avant une relecture (re-capture par xls_read_file). */
    for (int i = 0; i < d->preservedCount; i++) {
        free(d->preservedNames[i]);
        free(d->preservedData[i]);
    }
    free(d->preservedNames); free(d->preservedData); free(d->preservedLens);
    d->preservedNames = NULL; d->preservedData = NULL; d->preservedLens = NULL;
    d->preservedCount = 0; d->preservedCap = 0;
}

static int xls_sheet_max_row(XlsSheet* s) {
    int m = 0;
    for (int i = 0; i < s->cellCount; i++) if (s->cells[i].row > m) m = s->cells[i].row;
    return m;
}
static int xls_sheet_max_col(XlsSheet* s) {
    int m = 0;
    for (int i = 0; i < s->cellCount; i++) if (s->cells[i].col > m) m = s->cells[i].col;
    return m;
}

/* ================================================================
 * Buffer de chaîne extensible (assemblage XML)
 * ================================================================ */

typedef struct { char* data; size_t len; size_t cap; } StrBuf;

static void sb_init(StrBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void sb_free(StrBuf* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
static void sb_append(StrBuf* b, const char* s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        b->data = (char*)realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}
static void sb_puts(StrBuf* b, const char* s) { sb_append(b, s, strlen(s)); }

/* Échappe le texte pour un contenu XML (&, <, >). */
static void sb_put_xml_escaped(StrBuf* b, const char* s) {
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '&': sb_puts(b, "&amp;");  break;
            case '<': sb_puts(b, "&lt;");   break;
            case '>': sb_puts(b, "&gt;");   break;
            default:  sb_append(b, p, 1);   break;
        }
    }
}

/* Échappe une valeur d'attribut XML (ajoute " autour). */
static void sb_put_xml_attr_escaped(StrBuf* b, const char* s) {
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '&':  sb_puts(b, "&amp;");   break;
            case '<':  sb_puts(b, "&lt;");    break;
            case '>':  sb_puts(b, "&gt;");    break;
            case '"':  sb_puts(b, "&quot;");  break;
            default:   sb_append(b, p, 1);    break;
        }
    }
}

/* ================================================================
 * Table de chaînes partagées (sharedStrings)
 * ================================================================ */

typedef struct { char** items; int count; int cap; } SharedStrings;

static int shared_intern(SharedStrings* ss, const char* s) {
    for (int i = 0; i < ss->count; i++)
        if (strcmp(ss->items[i], s) == 0) return i;
    if (ss->count >= ss->cap) {
        int nc = ss->cap ? ss->cap * 2 : 16;
        ss->items = (char**)realloc(ss->items, sizeof(char*) * (size_t)nc);
        ss->cap = nc;
    }
    ss->items[ss->count] = xls_strdup(s);
    return ss->count++;
}
static void shared_free(SharedStrings* ss) {
    for (int i = 0; i < ss->count; i++) free(ss->items[i]);
    free(ss->items);
}

/* ================================================================
 * Écriture .xlsx
 * ================================================================ */

static const char* XML_HEADER =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n";

static char* build_content_types(int sheetCount) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">");
    sb_puts(&b, "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>");
    sb_puts(&b, "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
    sb_puts(&b, "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
    for (int i = 1; i <= sheetCount; i++) {
        char line[160];
        snprintf(line, sizeof(line),
            "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i);
        sb_puts(&b, line);
    }
    sb_puts(&b, "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    sb_puts(&b, "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>");
    sb_puts(&b, "</Types>");
    return b.data;  /* ownership transféré */
}

static char* build_root_rels(void) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    sb_puts(&b, "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>");
    sb_puts(&b, "</Relationships>");
    return b.data;
}

static char* build_workbook(ObjXlsDocument* d) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">");
    sb_puts(&b, "<sheets>");
    for (int i = 0; i < d->sheetCount; i++) {
        char head[64];
        snprintf(head, sizeof(head), "<sheet name=\"");
        sb_puts(&b, head);
        sb_put_xml_attr_escaped(&b, d->sheets[i].name);
        char tail[64];
        snprintf(tail, sizeof(tail), "\" sheetId=\"%d\" r:id=\"rId%d\"/>", i + 1, i + 1);
        sb_puts(&b, tail);
    }
    sb_puts(&b, "</sheets></workbook>");
    return b.data;
}

static char* build_workbook_rels(int sheetCount) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 1; i <= sheetCount; i++) {
        char line[192];
        snprintf(line, sizeof(line),
            "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", i, i);
        sb_puts(&b, line);
    }
    char styleLine[192];
    snprintf(styleLine, sizeof(styleLine),
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>", sheetCount + 1);
    sb_puts(&b, styleLine);
    char ssLine[192];
    snprintf(ssLine, sizeof(ssLine),
        "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>", sheetCount + 2);
    sb_puts(&b, ssLine);
    sb_puts(&b, "</Relationships>");
    return b.data;
}

/* ================================================================
 *  — génération OOXML des styles (xl/styles.xml)
 *
 * Le style d'une cellule vit en mémoire sur XlsCell (couleur de fond, police,
 * cadre, alignements, retour à la ligne, masque de format). La sérialisation
 * OOXML décompose ce style en quatre catalogues dédupliqués (fonts, fills,
 * borders, numFmts) référencés par des « formats de cellule » (cellXfs). Chaque
 * cellule stylée porte alors un attribut `s="N"` pointant son <xf>.
 *
 * Conversion couleur : les couleurs OWL sont des entiers packés BGR
 * (R + V*256 + B*65536, cf. stdlib_color.c). Excel attend AARRGGBB. On extrait
 * donc R = octet 0, V = octet 1, B = octet 2, et on émet "FF" + RRGGBB (alpha
 * opaque). Passer par le mauvais ordre inverserait R et B silencieusement.
 * ================================================================ */

/* Couleur OWL (entier BGR 24 bits) -> chaîne ARGB Excel "FFRRGGBB". out : 9 octets. */
static void xls_bgr_to_argb(int64_t bgr, char* out) {
    int r = (int)( bgr        & 0xFF);
    int g = (int)((bgr >>  8) & 0xFF);
    int b = (int)((bgr >> 16) & 0xFF);
    snprintf(out, 9, "FF%02X%02X%02X", r, g, b);
}

/* --- Lecture d'un sous-champ d'une ObjInstance (composite Police/Cadre) ------
 * Miroir local des accesseurs de stdlib_ui.c (statiques là-bas). Les clés sont
 * les formes ASCII canon (le scanner strip les accents au compile-time). */
static bool xls_inst_get(ObjInstance* in, const char* field, Value* out) {
    if (!in) return false;
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    return table_get(&in->fields, k, out);
}
static int64_t xls_inst_int(ObjInstance* in, const char* field, int64_t dflt) {
    Value v;
    if (!xls_inst_get(in, field, &v)) return dflt;
    if (IS_INTEGER(v)) return AS_INTEGER(v);
    if (IS_REAL(v))    return (int64_t)AS_REAL(v);
    return dflt;
}
static bool xls_inst_bool(ObjInstance* in, const char* field, bool dflt) {
    Value v;
    if (!xls_inst_get(in, field, &v)) return dflt;
    if (IS_BOOL(v)) return AS_BOOL(v);
    return dflt;
}
static const char* xls_inst_str(ObjInstance* in, const char* field, const char* dflt) {
    Value v;
    if (!xls_inst_get(in, field, &v)) return dflt;
    if (IS_STRING(v)) return AS_CSTRING(v);
    return dflt;
}

/* --- Catalogues de style dédupliqués ---------------------------------------- */
typedef struct {
    char*   name;                        /* nom de police (dupliqué) */
    int     size;                        /* taille en points */
    bool    bold, italic, underline, strike;
    int64_t color;   bool hasColor;      /* couleur BGR OWL */
} XlsFont;
typedef struct { int64_t color; } XlsFill;          /* remplissage solide fgColor BGR */
typedef struct { int64_t color; int trait; int sides; } XlsBorder;
typedef struct { char* code; int id; } XlsNumFmt;   /* id à partir de 164 */
typedef struct {
    int  numFmtId, fontId, fillId, borderId;
    int  alignH,  alignV;   bool hasAlignH, hasAlignV;
    bool wrapText;
} XlsXf;

typedef struct {
    XlsFont*   fonts;    int fontCount,   fontCap;
    XlsFill*   fills;    int fillCount,   fillCap;   /* [0]=none [1]=gray125 réservés */
    XlsBorder* borders;  int borderCount, borderCap; /* [0]=vide réservé */
    XlsNumFmt* numFmts;  int numFmtCount, numFmtCap;
    XlsXf*     xfs;      int xfCount,     xfCap;      /* [0]=défaut réservé */
} StyleTables;

static void style_tables_init(StyleTables* t) {
    memset(t, 0, sizeof(*t));
    /* Font 0 = police par défaut (Calibri 11). */
    t->fonts = (XlsFont*)calloc(4, sizeof(XlsFont)); t->fontCap = 4; t->fontCount = 1;
    t->fonts[0].name = xls_strdup("Calibri"); t->fonts[0].size = 11;
    /* Fills 0/1 réservés par OOXML (none + gray125) ; les fills custom démarrent à 2. */
    t->fills = (XlsFill*)calloc(4, sizeof(XlsFill)); t->fillCap = 4; t->fillCount = 2;
    t->fills[0].color = -1; t->fills[1].color = -2;   /* sentinelles réservées non-matchables */
    /* Border 0 = bordure vide. */
    t->borders = (XlsBorder*)calloc(4, sizeof(XlsBorder)); t->borderCap = 4; t->borderCount = 1;
    t->borders[0].sides = 0;
    /* Xf 0 = format par défaut. */
    t->xfs = (XlsXf*)calloc(4, sizeof(XlsXf)); t->xfCap = 4; t->xfCount = 1;
}
static void style_tables_free(StyleTables* t) {
    for (int i = 0; i < t->fontCount; i++) free(t->fonts[i].name);
    for (int i = 0; i < t->numFmtCount; i++) free(t->numFmts[i].code);
    free(t->fonts); free(t->fills); free(t->borders); free(t->numFmts); free(t->xfs);
    memset(t, 0, sizeof(*t));
}

static int font_intern(StyleTables* t, const XlsFont* f) {
    for (int i = 0; i < t->fontCount; i++) {
        XlsFont* e = &t->fonts[i];
        if (e->size == f->size && e->bold == f->bold && e->italic == f->italic &&
            e->underline == f->underline && e->strike == f->strike &&
            e->hasColor == f->hasColor && (!f->hasColor || e->color == f->color) &&
            strcmp(e->name, f->name) == 0)
            return i;
    }
    if (t->fontCount >= t->fontCap) {
        t->fontCap *= 2; t->fonts = (XlsFont*)realloc(t->fonts, sizeof(XlsFont) * (size_t)t->fontCap);
    }
    XlsFont* e = &t->fonts[t->fontCount];
    *e = *f; e->name = xls_strdup(f->name);
    return t->fontCount++;
}
static int fill_intern(StyleTables* t, int64_t color) {
    for (int i = 2; i < t->fillCount; i++) if (t->fills[i].color == color) return i;
    if (t->fillCount >= t->fillCap) {
        t->fillCap *= 2; t->fills = (XlsFill*)realloc(t->fills, sizeof(XlsFill) * (size_t)t->fillCap);
    }
    t->fills[t->fillCount].color = color;
    return t->fillCount++;
}
static int border_intern(StyleTables* t, const XlsBorder* b) {
    for (int i = 1; i < t->borderCount; i++)
        if (t->borders[i].color == b->color && t->borders[i].trait == b->trait &&
            t->borders[i].sides == b->sides) return i;
    if (t->borderCount >= t->borderCap) {
        t->borderCap *= 2; t->borders = (XlsBorder*)realloc(t->borders, sizeof(XlsBorder) * (size_t)t->borderCap);
    }
    t->borders[t->borderCount] = *b;
    return t->borderCount++;
}
static int numfmt_intern(StyleTables* t, const char* code) {
    for (int i = 0; i < t->numFmtCount; i++) if (strcmp(t->numFmts[i].code, code) == 0) return t->numFmts[i].id;
    if (t->numFmtCount >= t->numFmtCap) {
        int nc = t->numFmtCap ? t->numFmtCap * 2 : 4;
        t->numFmts = (XlsNumFmt*)realloc(t->numFmts, sizeof(XlsNumFmt) * (size_t)nc); t->numFmtCap = nc;
    }
    int id = 164 + t->numFmtCount;   /* IDs custom à partir de 164 (0-163 = intégrés Excel) */
    t->numFmts[t->numFmtCount].code = xls_strdup(code);
    t->numFmts[t->numFmtCount].id = id;
    t->numFmtCount++;
    return id;
}
static int xf_intern(StyleTables* t, const XlsXf* x) {
    for (int i = 0; i < t->xfCount; i++) {
        XlsXf* e = &t->xfs[i];
        if (e->numFmtId == x->numFmtId && e->fontId == x->fontId && e->fillId == x->fillId &&
            e->borderId == x->borderId && e->hasAlignH == x->hasAlignH && e->hasAlignV == x->hasAlignV &&
            e->wrapText == x->wrapText && (!x->hasAlignH || e->alignH == x->alignH) &&
            (!x->hasAlignV || e->alignV == x->alignV))
            return i;
    }
    if (t->xfCount >= t->xfCap) {
        t->xfCap *= 2; t->xfs = (XlsXf*)realloc(t->xfs, sizeof(XlsXf) * (size_t)t->xfCap);
    }
    t->xfs[t->xfCount] = *x;
    return t->xfCount++;
}

/* Une cellule porte-t-elle un style à sérialiser ? */
static bool xls_cell_is_styled(XlsCell* c) {
    return c->hasBg || IS_INSTANCE(c->font) || IS_INSTANCE(c->border) ||
           c->hasAlignH || c->hasAlignV || (c->hasMultiline && c->multiline) ||
           (c->mask && c->mask[0]);
}

/* Collecte (dédup) le style d'une cellule dans les catalogues et renvoie l'index
 * de son <xf>. Renvoie -1 si la cellule n'a aucun style (pas d'attribut s=).
 * Idempotent : rappelé sur des catalogues déjà peuplés, il renvoie le même
 * index sans rien ajouter (utilisé à la fois pour peupler et pour lire). */
static int style_collect_cell(StyleTables* t, XlsCell* c) {
    if (!xls_cell_is_styled(c)) return -1;
    int fontId = 0;
    if (IS_INSTANCE(c->font)) {
        ObjInstance* in = AS_INSTANCE(c->font);
        XlsFont f; memset(&f, 0, sizeof(f));
        const char* nm = xls_inst_str(in, "Nom", "");
        f.name      = (char*)(nm && nm[0] ? nm : "Calibri");  /* dup dans font_intern */
        int sz      = (int)xls_inst_int(in, "Taille", 0);
        f.size      = sz > 0 ? sz : 11;
        f.bold      = xls_inst_bool(in, "Gras", false);
        f.italic    = xls_inst_bool(in, "Italique", false);
        f.underline = xls_inst_bool(in, "Souligne", false);
        f.strike    = xls_inst_bool(in, "Barre", false);
        int64_t col = xls_inst_int(in, "Couleur", 0);
        if (col != 0) { f.color = col; f.hasColor = true; }
        fontId = font_intern(t, &f);
    }
    int fillId = 0;
    if (c->hasBg) fillId = fill_intern(t, c->bgColor);
    int borderId = 0;
    if (IS_INSTANCE(c->border)) {
        ObjInstance* in = AS_INSTANCE(c->border);
        XlsBorder b;
        b.color = xls_inst_int(in, "Couleur", 0);
        b.trait = (int)xls_inst_int(in, "Trait", OWL_TC_CONTINU);
        b.sides = (int)xls_inst_int(in, "Bords", 15);
        borderId = border_intern(t, &b);
    }
    int numFmtId = 0;
    if (c->mask && c->mask[0]) numFmtId = numfmt_intern(t, c->mask);

    XlsXf x; memset(&x, 0, sizeof(x));
    x.numFmtId = numFmtId; x.fontId = fontId; x.fillId = fillId; x.borderId = borderId;
    x.hasAlignH = c->hasAlignH; x.alignH = c->alignH;
    x.hasAlignV = c->hasAlignV; x.alignV = c->alignV;
    x.wrapText  = (c->hasMultiline && c->multiline);
    return xf_intern(t, &x);
}

/* Correspondance sentinelle d'alignement OWL -> valeur OOXML. */
static const char* xls_align_h(int v) {
    if (v == OWL_CH_GAUCHE) return "left";
    if (v == OWL_CH_CENTRE) return "center";
    if (v == OWL_CH_DROITE) return "right";
    return NULL;
}
static const char* xls_align_v(int v) {
    if (v == OWL_CV_HAUT)   return "top";
    if (v == OWL_CV_CENTRE) return "center";
    if (v == OWL_CV_BAS)    return "bottom";
    return NULL;
}
/* Style de trait OWL -> style de bordure OOXML. */
static const char* xls_border_style(int trait) {
    if (trait == OWL_TC_TIRET)     return "dashed";
    if (trait == OWL_TC_POINTILLE) return "dotted";
    if (trait == OWL_TC_DOUBLE)    return "double";
    return "thin";   /* OWL_TC_CONTINU et défaut */
}

/* Sérialise un côté de bordure. `bit` = masque du côté (BordHaut=1, Droite=2,
 * Bas=4, Gauche=8) ; `tag` = left/right/top/bottom. */
static void xls_emit_border_side(StrBuf* b, const XlsBorder* bd, const char* tag, int bit) {
    if (!(bd->sides & bit)) { sb_puts(b, "<"); sb_puts(b, tag); sb_puts(b, "/>"); return; }
    char argb[9]; xls_bgr_to_argb(bd->color, argb);
    sb_puts(b, "<"); sb_puts(b, tag);
    sb_puts(b, " style=\""); sb_puts(b, xls_border_style(bd->trait)); sb_puts(b, "\">");
    sb_puts(b, "<color rgb=\""); sb_puts(b, argb); sb_puts(b, "\"/>");
    sb_puts(b, "</"); sb_puts(b, tag); sb_puts(b, ">");
}

static char* build_styles(StyleTables* t) {
    StrBuf b; sb_init(&b);
    char num[128];
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");

    /* numFmts (formats custom, IDs à partir de 164). */
    if (t->numFmtCount > 0) {
        snprintf(num, sizeof(num), "<numFmts count=\"%d\">", t->numFmtCount);
        sb_puts(&b, num);
        for (int i = 0; i < t->numFmtCount; i++) {
            snprintf(num, sizeof(num), "<numFmt numFmtId=\"%d\" formatCode=\"", t->numFmts[i].id);
            sb_puts(&b, num);
            sb_put_xml_attr_escaped(&b, t->numFmts[i].code);
            sb_puts(&b, "\"/>");
        }
        sb_puts(&b, "</numFmts>");
    }

    /* fonts. */
    snprintf(num, sizeof(num), "<fonts count=\"%d\">", t->fontCount);
    sb_puts(&b, num);
    for (int i = 0; i < t->fontCount; i++) {
        XlsFont* f = &t->fonts[i];
        sb_puts(&b, "<font>");
        if (f->bold)      sb_puts(&b, "<b/>");
        if (f->italic)    sb_puts(&b, "<i/>");
        if (f->strike)    sb_puts(&b, "<strike/>");
        if (f->underline) sb_puts(&b, "<u/>");
        snprintf(num, sizeof(num), "<sz val=\"%d\"/>", f->size > 0 ? f->size : 11);
        sb_puts(&b, num);
        if (f->hasColor) {
            char argb[9]; xls_bgr_to_argb(f->color, argb);
            sb_puts(&b, "<color rgb=\""); sb_puts(&b, argb); sb_puts(&b, "\"/>");
        }
        sb_puts(&b, "<name val=\"");
        sb_put_xml_attr_escaped(&b, f->name && f->name[0] ? f->name : "Calibri");
        sb_puts(&b, "\"/>");
        sb_puts(&b, "</font>");
    }
    sb_puts(&b, "</fonts>");

    /* fills : 0=none, 1=gray125, puis les fills solides custom. */
    snprintf(num, sizeof(num), "<fills count=\"%d\">", t->fillCount);
    sb_puts(&b, num);
    sb_puts(&b, "<fill><patternFill patternType=\"none\"/></fill>");
    sb_puts(&b, "<fill><patternFill patternType=\"gray125\"/></fill>");
    for (int i = 2; i < t->fillCount; i++) {
        char argb[9]; xls_bgr_to_argb(t->fills[i].color, argb);
        sb_puts(&b, "<fill><patternFill patternType=\"solid\"><fgColor rgb=\"");
        sb_puts(&b, argb);
        sb_puts(&b, "\"/><bgColor indexed=\"64\"/></patternFill></fill>");
    }
    sb_puts(&b, "</fills>");

    /* borders. */
    snprintf(num, sizeof(num), "<borders count=\"%d\">", t->borderCount);
    sb_puts(&b, num);
    sb_puts(&b, "<border><left/><right/><top/><bottom/><diagonal/></border>");
    for (int i = 1; i < t->borderCount; i++) {
        XlsBorder* bd = &t->borders[i];
        sb_puts(&b, "<border>");
        xls_emit_border_side(&b, bd, "left",   8);   /* BordGauche */
        xls_emit_border_side(&b, bd, "right",  2);   /* BordDroite */
        xls_emit_border_side(&b, bd, "top",    1);   /* BordHaut */
        xls_emit_border_side(&b, bd, "bottom", 4);   /* BordBas */
        sb_puts(&b, "<diagonal/>");
        sb_puts(&b, "</border>");
    }
    sb_puts(&b, "</borders>");

    sb_puts(&b, "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>");

    /* cellXfs : un <xf> par combinaison distincte. */
    snprintf(num, sizeof(num), "<cellXfs count=\"%d\">", t->xfCount);
    sb_puts(&b, num);
    for (int i = 0; i < t->xfCount; i++) {
        XlsXf* x = &t->xfs[i];
        const char* ah = x->hasAlignH ? xls_align_h(x->alignH) : NULL;
        const char* av = x->hasAlignV ? xls_align_v(x->alignV) : NULL;
        bool hasAlign = (ah || av || x->wrapText);
        snprintf(num, sizeof(num), "<xf numFmtId=\"%d\" fontId=\"%d\" fillId=\"%d\" borderId=\"%d\" xfId=\"0\"",
                 x->numFmtId, x->fontId, x->fillId, x->borderId);
        sb_puts(&b, num);
        if (x->numFmtId) sb_puts(&b, " applyNumberFormat=\"1\"");
        if (x->fontId)   sb_puts(&b, " applyFont=\"1\"");
        if (x->fillId)   sb_puts(&b, " applyFill=\"1\"");
        if (x->borderId) sb_puts(&b, " applyBorder=\"1\"");
        if (hasAlign)    sb_puts(&b, " applyAlignment=\"1\"");
        if (hasAlign) {
            sb_puts(&b, "><alignment");
            if (ah) { sb_puts(&b, " horizontal=\""); sb_puts(&b, ah); sb_puts(&b, "\""); }
            if (av) { sb_puts(&b, " vertical=\"");   sb_puts(&b, av); sb_puts(&b, "\""); }
            if (x->wrapText) sb_puts(&b, " wrapText=\"1\"");
            sb_puts(&b, "/></xf>");
        } else {
            sb_puts(&b, "/>");
        }
    }
    sb_puts(&b, "</cellXfs>");

    sb_puts(&b, "</styleSheet>");
    return b.data;
}

static char* build_shared_strings(SharedStrings* ss) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    char open[160];
    snprintf(open, sizeof(open),
        "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" count=\"%d\" uniqueCount=\"%d\">",
        ss->count, ss->count);
    sb_puts(&b, open);
    for (int i = 0; i < ss->count; i++) {
        sb_puts(&b, "<si><t xml:space=\"preserve\">");
        sb_put_xml_escaped(&b, ss->items[i]);
        sb_puts(&b, "</t></si>");
    }
    sb_puts(&b, "</sst>");
    return b.data;
}

/* Largeur de colonne : la valeur OWL est en pixels ; Excel exprime la largeur
 * de colonne en « caractères » (largeur du chiffre le plus large de la police
 * par défaut, MDW=7 px pour Calibri 11). Formule standard : width=(px-5)/MDW. */
static double xls_px_to_col_width(double px) {
    double w = (px - 5.0) / 7.0;
    return w > 0.0 ? w : 0.0;
}
/* Hauteur de ligne : la valeur OWL est en pixels ; Excel exprime la hauteur de
 * ligne en points typographiques. 96 px/pouce -> 72 pt/pouce : pt = px * 0.75. */
static double xls_px_to_row_height(double px) {
    return px * 0.75;
}

/* Une worksheet. Les cellules sont émises ligne par ligne (tri implicite via
 * regroupement par row). sharedStrings + StyleTables alimentés au passage :
 * chaque cellule stylée reçoit son attribut `s="N"` (index dans cellXfs). */
static char* build_worksheet(XlsSheet* s, SharedStrings* ss, StyleTables* st) {
    StrBuf b; sb_init(&b);
    sb_puts(&b, XML_HEADER);
    sb_puts(&b, "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");

    /* <cols> : largeurs de colonne (avant <sheetData>, exigence OOXML). */
    if (s->colDimCount > 0) {
        sb_puts(&b, "<cols>");
        for (int i = 0; i < s->colDimCount; i++) {
            if (s->colDims[i].value <= 0) continue;
            char col[96];
            snprintf(col, sizeof(col),
                "<col min=\"%d\" max=\"%d\" width=\"%.6f\" customWidth=\"1\"/>",
                s->colDims[i].index, s->colDims[i].index,
                xls_px_to_col_width(s->colDims[i].value));
            sb_puts(&b, col);
        }
        sb_puts(&b, "</cols>");
    }

    sb_puts(&b, "<sheetData>");

    /* maxRow inclut les lignes qui n'ont qu'une hauteur (pas de cellule). */
    int maxRow = xls_sheet_max_row(s);
    for (int i = 0; i < s->rowDimCount; i++)
        if (s->rowDims[i].index > maxRow) maxRow = s->rowDims[i].index;

    for (int r = 1; r <= maxRow; r++) {
        bool hasAny = false;
        for (int i = 0; i < s->cellCount; i++)
            if (s->cells[i].row == r) { hasAny = true; break; }
        double ht; bool hasHeight = xls_dim_get(s->rowDims, s->rowDimCount, r, &ht);
        if (!hasAny && !hasHeight) continue;

        char rowOpen[80];
        if (hasHeight && ht > 0)
            snprintf(rowOpen, sizeof(rowOpen), "<row r=\"%d\" ht=\"%.4f\" customHeight=\"1\">",
                     r, xls_px_to_row_height(ht));
        else
            snprintf(rowOpen, sizeof(rowOpen), "<row r=\"%d\">", r);
        sb_puts(&b, rowOpen);

        /* Émet les cellules de la ligne, ordre de colonne croissant. */
        int maxCol = xls_sheet_max_col(s);
        for (int c = 1; c <= maxCol; c++) {
            XlsCell* cell = xls_find_cell(s, r, c);
            if (cell == NULL) continue;
            int xf = style_collect_cell(st, cell);
            bool styled = (xf >= 0);
            if (cell->value == NULL && !styled) continue;  /* cellule vide non stylée */

            char ref[24], letters[16];
            xls_col_to_letters(c, letters, sizeof(letters));
            snprintf(ref, sizeof(ref), "%s%d", letters, r);
            sb_puts(&b, "<c r=\""); sb_puts(&b, ref); sb_puts(&b, "\"");
            if (styled) { char sAttr[24]; snprintf(sAttr, sizeof(sAttr), " s=\"%d\"", xf); sb_puts(&b, sAttr); }

            if (cell->value == NULL) {
                sb_puts(&b, "/>");                       /* cellule stylée sans contenu */
            } else if (cell->isNumber) {
                /* Cellule numerique OOXML : pas d'attribut t, valeur litterale
                 * dans <v> (le read path la relit via la branche type==NULL). */
                sb_puts(&b, "><v>"); sb_puts(&b, cell->value); sb_puts(&b, "</v></c>");
            } else {
                int idx = shared_intern(ss, cell->value);
                char v[40]; snprintf(v, sizeof(v), " t=\"s\"><v>%d</v></c>", idx);
                sb_puts(&b, v);
            }
        }
        sb_puts(&b, "</row>");
    }
    sb_puts(&b, "</sheetData>");

    if (s->mergeCount > 0) {
        char mOpen[40];
        snprintf(mOpen, sizeof(mOpen), "<mergeCells count=\"%d\">", s->mergeCount);
        sb_puts(&b, mOpen);
        for (int i = 0; i < s->mergeCount; i++) {
            XlsMerge* m = &s->merges[i];
            char l1[16], l2[16], ref[48];
            xls_col_to_letters(m->firstCol, l1, sizeof(l1));
            xls_col_to_letters(m->lastCol,  l2, sizeof(l2));
            snprintf(ref, sizeof(ref), "<mergeCell ref=\"%s%d:%s%d\"/>",
                     l1, m->firstRow, l2, m->lastRow);
            sb_puts(&b, ref);
        }
        sb_puts(&b, "</mergeCells>");
    }

    sb_puts(&b, "</worksheet>");
    return b.data;
}

/* Sérialise le document vers un .xlsx sur disque. Renvoie true si OK. */
static bool xls_write_file(ObjXlsDocument* d, const char* path) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path, 0)) return false;

    bool ok = true;
    SharedStrings ss; ss.items = NULL; ss.count = 0; ss.cap = 0;
    StyleTables st; style_tables_init(&st);

    /* Les worksheets d'abord (alimentent sharedStrings ET les catalogues de
     * style) ; styles.xml est sérialisé ensuite depuis les catalogues peuplés. */
    char** wsXml = (char**)calloc((size_t)d->sheetCount, sizeof(char*));
    for (int i = 0; i < d->sheetCount && ok; i++) {
        wsXml[i] = build_worksheet(&d->sheets[i], &ss, &st);
    }

    #define ADD_PART(name, buf) do {                                       \
        char* _b = (buf);                                                  \
        if (!_b || !mz_zip_writer_add_mem(&zip, (name), _b, strlen(_b),    \
                                          MZ_DEFAULT_COMPRESSION)) ok = false; \
        free(_b);                                                          \
    } while (0)

    if (ok) ADD_PART("[Content_Types].xml", build_content_types(d->sheetCount));
    if (ok) ADD_PART("_rels/.rels",         build_root_rels());
    if (ok) ADD_PART("xl/workbook.xml",     build_workbook(d));
    if (ok) ADD_PART("xl/_rels/workbook.xml.rels", build_workbook_rels(d->sheetCount));
    if (ok) ADD_PART("xl/styles.xml",       build_styles(&st));
    if (ok) ADD_PART("xl/sharedStrings.xml", build_shared_strings(&ss));
    for (int i = 0; i < d->sheetCount; i++) {
        char name[48];
        snprintf(name, sizeof(name), "xl/worksheets/sheet%d.xml", i + 1);
        char* b = wsXml[i];
        if (ok && (!b || !mz_zip_writer_add_mem(&zip, name, b, strlen(b),
                                                MZ_DEFAULT_COMPRESSION))) ok = false;
        free(b);
    }
    #undef ADD_PART

    /*  — reinjection verbatim des parts non modelisees d'un classeur
     * OUVERT (macros/graphiques/medias...). Vide pour un classeur cree de zero :
     * la boucle ne fait rien et on regenere tout (comportement d'origine). */
    for (int i = 0; i < d->preservedCount && ok; i++) {
        if (!mz_zip_writer_add_mem(&zip, d->preservedNames[i], d->preservedData[i],
                                   d->preservedLens[i], MZ_DEFAULT_COMPRESSION))
            ok = false;
    }

    free(wsXml);
    shared_free(&ss);
    style_tables_free(&st);

    if (ok) ok = mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    return ok;
}

/* ================================================================
 * Lecture .xlsx
 * ================================================================ */

/* Concatène le texte de tous les descendants <t> d'un nœud dans un StrBuf. */
static void collect_text_nodes(xmlNode* node, StrBuf* out) {
    for (xmlNode* n = node->children; n; n = n->next) {
        if (n->type == XML_ELEMENT_NODE && xmlStrcmp(n->name, (const xmlChar*)"t") == 0) {
            xmlChar* txt = xmlNodeGetContent(n);
            if (txt) { sb_puts(out, (const char*)txt); xmlFree(txt); }
        } else if (n->type == XML_ELEMENT_NODE) {
            collect_text_nodes(n, out);  /* rich text runs <r><t> */
        }
    }
}

/*  — premier enfant direct de nom `name`, ou NULL. */
static xmlNode* xls_first_child(xmlNode* parent, const char* name) {
    for (xmlNode* c = parent ? parent->children : NULL; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, (const xmlChar*)name) == 0)
            return c;
    return NULL;
}
/*  — nombre d'enfants directs de nom `name`. */
static int xls_count_child(xmlNode* parent, const char* name) {
    int n = 0;
    for (xmlNode* c = parent ? parent->children : NULL; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, (const xmlChar*)name) == 0) n++;
    return n;
}

/* Parse sharedStrings.xml -> tableau de chaînes (ownership au caller). */
static void parse_shared_strings(const char* xml, size_t len, SharedStrings* ss) {
    xmlDoc* doc = xmlReadMemory(xml, (int)len, "sharedStrings.xml", NULL,
                                XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
    if (!doc) return;
    xmlNode* root = xmlDocGetRootElement(doc);  /* <sst> */
    if (root) {
        for (xmlNode* si = root->children; si; si = si->next) {
            if (si->type != XML_ELEMENT_NODE) continue;
            if (xmlStrcmp(si->name, (const xmlChar*)"si") != 0) continue;
            StrBuf t; sb_init(&t);
            collect_text_nodes(si, &t);
            shared_intern(ss, t.data ? t.data : "");
            sb_free(&t);
        }
    }
    xmlFreeDoc(doc);
}

/* ================================================================
 *  — lecture des styles (reverse de xl/styles.xml)
 *
 * Miroir de build_styles/style_collect_cell : on reconstruit les quatre
 * catalogues OOXML (fonts, fills, borders, numFmts) et la table cellXfs, puis
 * pour chaque cellule `<c s="N">` on resout l'index N -> combinaison de style
 * -> champs de XlsCell (CouleurFond, Police, Cadre, Alignements, Multiligne,
 * Masque). La conversion couleur est l'inverse exact de xls_bgr_to_argb :
 * "FFRRGGBB" -> entier BGR OWL (R octet 0, V octet 1, B octet 2). Passer par
 * le mauvais ordre inverserait R et B silencieusement.
 * ================================================================ */

typedef struct {
    char*   name; int size;
    bool    bold, italic, underline, strike;
    int64_t color; bool hasColor;
} RFont;
typedef struct { int64_t color; bool solid; } RFill;
typedef struct { int64_t color; bool hasColor; int trait; int sides; bool any; } RBorder;
typedef struct { int id; char* code; } RNumFmt;
typedef struct {
    int  numFmtId, fontId, fillId, borderId;
    int  alignH; bool hasAlignH;
    int  alignV; bool hasAlignV;
    bool wrap;
} RXf;
typedef struct {
    RFont*   fonts;   int fontCount;
    RFill*   fills;   int fillCount;
    RBorder* borders; int borderCount;
    RNumFmt* numFmts; int numFmtCount;
    RXf*     xfs;     int xfCount;
} ReadStyles;

static void read_styles_free(ReadStyles* rs) {
    for (int i = 0; i < rs->fontCount; i++) free(rs->fonts[i].name);
    for (int i = 0; i < rs->numFmtCount; i++) free(rs->numFmts[i].code);
    free(rs->fonts); free(rs->fills); free(rs->borders); free(rs->numFmts); free(rs->xfs);
    memset(rs, 0, sizeof(*rs));
}

/* "FFRRGGBB" (ou "RRGGBB") -> entier BGR OWL. Inverse de xls_bgr_to_argb. */
static bool xls_argb_to_bgr(const char* rgb, int64_t* out) {
    if (!rgb) return false;
    size_t n = strlen(rgb);
    const char* h = rgb;
    if (n == 8) h = rgb + 2;         /* saute l'octet alpha "FF" */
    else if (n != 6) return false;
    unsigned int r, g, b;
    if (sscanf(h, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    *out = (int64_t)((r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16));
    return true;
}

/* "left"/"center"/"right" -> sentinelle CadrageH ; renvoie false si inconnu. */
static bool xls_align_h_rev(const char* v, int* out) {
    if (!v) return false;
    if (strcmp(v, "left") == 0)   { *out = OWL_CH_GAUCHE; return true; }
    if (strcmp(v, "center") == 0) { *out = OWL_CH_CENTRE; return true; }
    if (strcmp(v, "right") == 0)  { *out = OWL_CH_DROITE; return true; }
    return false;
}
static bool xls_align_v_rev(const char* v, int* out) {
    if (!v) return false;
    if (strcmp(v, "top") == 0)    { *out = OWL_CV_HAUT;   return true; }
    if (strcmp(v, "center") == 0) { *out = OWL_CV_CENTRE; return true; }
    if (strcmp(v, "bottom") == 0) { *out = OWL_CV_BAS;    return true; }
    return false;
}
/* Style de bordure OOXML -> trait OWL (inverse de xls_border_style). */
static int xls_border_style_rev(const char* s) {
    if (!s) return OWL_TC_CONTINU;
    if (strcmp(s, "dashed") == 0) return OWL_TC_TIRET;
    if (strcmp(s, "dotted") == 0) return OWL_TC_POINTILLE;
    if (strcmp(s, "double") == 0) return OWL_TC_DOUBLE;
    return OWL_TC_CONTINU;   /* "thin" et defaut */
}

/* Parse xl/styles.xml -> catalogues + cellXfs (ownership au caller). */
static void parse_styles(const char* xml, size_t len, ReadStyles* rs) {
    memset(rs, 0, sizeof(*rs));
    xmlDoc* doc = xmlReadMemory(xml, (int)len, "styles.xml", NULL,
                                XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
    if (!doc) return;
    xmlNode* root = xmlDocGetRootElement(doc);  /* <styleSheet> */
    if (!root) { xmlFreeDoc(doc); return; }

    /* Comptage prealable pour dimensionner les tableaux. */
    int nFonts = 0, nFills = 0, nBorders = 0, nNumFmts = 0, nXfs = 0;
    for (xmlNode* sec = root->children; sec; sec = sec->next) {
        if (sec->type != XML_ELEMENT_NODE) continue;
        if      (xmlStrcmp(sec->name, (const xmlChar*)"fonts") == 0)   nFonts   = xls_count_child(sec, "font");
        else if (xmlStrcmp(sec->name, (const xmlChar*)"fills") == 0)   nFills   = xls_count_child(sec, "fill");
        else if (xmlStrcmp(sec->name, (const xmlChar*)"borders") == 0) nBorders = xls_count_child(sec, "border");
        else if (xmlStrcmp(sec->name, (const xmlChar*)"numFmts") == 0) nNumFmts = xls_count_child(sec, "numFmt");
        else if (xmlStrcmp(sec->name, (const xmlChar*)"cellXfs") == 0) nXfs     = xls_count_child(sec, "xf");
    }
    if (nFonts)   rs->fonts   = (RFont*)calloc((size_t)nFonts, sizeof(RFont));
    if (nFills)   rs->fills   = (RFill*)calloc((size_t)nFills, sizeof(RFill));
    if (nBorders) rs->borders = (RBorder*)calloc((size_t)nBorders, sizeof(RBorder));
    if (nNumFmts) rs->numFmts = (RNumFmt*)calloc((size_t)nNumFmts, sizeof(RNumFmt));
    if (nXfs)     rs->xfs     = (RXf*)calloc((size_t)nXfs, sizeof(RXf));

    for (xmlNode* sec = root->children; sec; sec = sec->next) {
        if (sec->type != XML_ELEMENT_NODE) continue;

        if (xmlStrcmp(sec->name, (const xmlChar*)"numFmts") == 0) {
            for (xmlNode* nf = sec->children; nf; nf = nf->next) {
                if (nf->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(nf->name, (const xmlChar*)"numFmt") != 0) continue;
                xmlChar* id   = xmlGetProp(nf, (const xmlChar*)"numFmtId");
                xmlChar* code = xmlGetProp(nf, (const xmlChar*)"formatCode");
                RNumFmt* e = &rs->numFmts[rs->numFmtCount++];
                e->id   = id ? atoi((const char*)id) : 0;
                e->code = xls_strdup(code ? (const char*)code : "");
                if (id) xmlFree(id);
                if (code) xmlFree(code);
            }
        } else if (xmlStrcmp(sec->name, (const xmlChar*)"fonts") == 0) {
            for (xmlNode* fn = sec->children; fn; fn = fn->next) {
                if (fn->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(fn->name, (const xmlChar*)"font") != 0) continue;
                RFont* f = &rs->fonts[rs->fontCount++];
                f->name = xls_strdup("Calibri"); f->size = 11;
                for (xmlNode* a = fn->children; a; a = a->next) {
                    if (a->type != XML_ELEMENT_NODE) continue;
                    if      (xmlStrcmp(a->name, (const xmlChar*)"b") == 0) f->bold = true;
                    else if (xmlStrcmp(a->name, (const xmlChar*)"i") == 0) f->italic = true;
                    else if (xmlStrcmp(a->name, (const xmlChar*)"u") == 0) f->underline = true;
                    else if (xmlStrcmp(a->name, (const xmlChar*)"strike") == 0) f->strike = true;
                    else if (xmlStrcmp(a->name, (const xmlChar*)"sz") == 0) {
                        xmlChar* v = xmlGetProp(a, (const xmlChar*)"val");
                        if (v) { f->size = atoi((const char*)v); xmlFree(v); }
                    } else if (xmlStrcmp(a->name, (const xmlChar*)"name") == 0) {
                        xmlChar* v = xmlGetProp(a, (const xmlChar*)"val");
                        if (v) { free(f->name); f->name = xls_strdup((const char*)v); xmlFree(v); }
                    } else if (xmlStrcmp(a->name, (const xmlChar*)"color") == 0) {
                        xmlChar* v = xmlGetProp(a, (const xmlChar*)"rgb");
                        int64_t col;
                        if (v && xls_argb_to_bgr((const char*)v, &col)) { f->color = col; f->hasColor = true; }
                        if (v) xmlFree(v);
                    }
                }
            }
        } else if (xmlStrcmp(sec->name, (const xmlChar*)"fills") == 0) {
            for (xmlNode* fl = sec->children; fl; fl = fl->next) {
                if (fl->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(fl->name, (const xmlChar*)"fill") != 0) continue;
                RFill* e = &rs->fills[rs->fillCount++];
                xmlNode* pf = xls_first_child(fl, "patternFill");
                xmlChar* pt = pf ? xmlGetProp(pf, (const xmlChar*)"patternType") : NULL;
                if (pt && xmlStrcmp(pt, (const xmlChar*)"solid") == 0) {
                    xmlNode* fg = xls_first_child(pf, "fgColor");
                    xmlChar* rgb = fg ? xmlGetProp(fg, (const xmlChar*)"rgb") : NULL;
                    int64_t col;
                    if (rgb && xls_argb_to_bgr((const char*)rgb, &col)) { e->color = col; e->solid = true; }
                    if (rgb) xmlFree(rgb);
                }
                if (pt) xmlFree(pt);
            }
        } else if (xmlStrcmp(sec->name, (const xmlChar*)"borders") == 0) {
            for (xmlNode* bd = sec->children; bd; bd = bd->next) {
                if (bd->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(bd->name, (const xmlChar*)"border") != 0) continue;
                RBorder* e = &rs->borders[rs->borderCount++];
                /* Cotes : bit BordHaut=1, Droite=2, Bas=4, Gauche=8 (cf. write). */
                struct { const char* tag; int bit; } sides[4] =
                    { {"left",8}, {"right",2}, {"top",1}, {"bottom",4} };
                for (int k = 0; k < 4; k++) {
                    xmlNode* sd = xls_first_child(bd, sides[k].tag);
                    if (!sd) continue;
                    xmlChar* st = xmlGetProp(sd, (const xmlChar*)"style");
                    if (!st) continue;   /* cote sans style => absent */
                    e->sides |= sides[k].bit; e->any = true;
                    e->trait = xls_border_style_rev((const char*)st);
                    xmlFree(st);
                    xmlNode* col = xls_first_child(sd, "color");
                    xmlChar* cv = col ? xmlGetProp(col, (const xmlChar*)"rgb") : NULL;
                    int64_t c;
                    if (cv && xls_argb_to_bgr((const char*)cv, &c)) { e->color = c; e->hasColor = true; }
                    if (cv) xmlFree(cv);
                }
            }
        } else if (xmlStrcmp(sec->name, (const xmlChar*)"cellXfs") == 0) {
            for (xmlNode* xf = sec->children; xf; xf = xf->next) {
                if (xf->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(xf->name, (const xmlChar*)"xf") != 0) continue;
                RXf* e = &rs->xfs[rs->xfCount++];
                xmlChar* nf = xmlGetProp(xf, (const xmlChar*)"numFmtId");
                xmlChar* fo = xmlGetProp(xf, (const xmlChar*)"fontId");
                xmlChar* fi = xmlGetProp(xf, (const xmlChar*)"fillId");
                xmlChar* bo = xmlGetProp(xf, (const xmlChar*)"borderId");
                e->numFmtId = nf ? atoi((const char*)nf) : 0;
                e->fontId   = fo ? atoi((const char*)fo) : 0;
                e->fillId   = fi ? atoi((const char*)fi) : 0;
                e->borderId = bo ? atoi((const char*)bo) : 0;
                if (nf) xmlFree(nf); if (fo) xmlFree(fo);
                if (fi) xmlFree(fi); if (bo) xmlFree(bo);
                xmlNode* al = xls_first_child(xf, "alignment");
                if (al) {
                    xmlChar* h = xmlGetProp(al, (const xmlChar*)"horizontal");
                    if (h) { e->hasAlignH = xls_align_h_rev((const char*)h, &e->alignH); xmlFree(h); }
                    xmlChar* v = xmlGetProp(al, (const xmlChar*)"vertical");
                    if (v) { e->hasAlignV = xls_align_v_rev((const char*)v, &e->alignV); xmlFree(v); }
                    xmlChar* w = xmlGetProp(al, (const xmlChar*)"wrapText");
                    if (w) { e->wrap = (xmlStrcmp(w, (const xmlChar*)"1") == 0); xmlFree(w); }
                }
            }
        }
    }
    xmlFreeDoc(doc);
}

/* Materialise un composite Police depuis un RFont et le pose sur la cellule
 * (row,col) de `sheet`. L'appelant DOIT garder le classeur roote (clone +
 * pose de champs = allocations OWL). Refetch la cellule apres l'alloc car les
 * inst_field_set n'ajoutent pas de cellule -> le pointeur reste stable, mais
 * on repasse par (row,col) pour rester defensif. */
static void xls_apply_font(XlsSheet* sheet, int row, int col, const RFont* f) {
    Value fontVal = owl_clone_global_template("Police");
    if (!IS_INSTANCE(fontVal)) return;
    vm_push(fontVal);                                  /* root pendant les set */
    ObjInstance* in = AS_INSTANCE(fontVal);
    if (f->name && f->name[0]) inst_field_set_str(in, "Nom", f->name);
    if (f->size > 0)           inst_field_set_int(in, "Taille", f->size);
    inst_field_set_bool(in, "Gras", f->bold);
    inst_field_set_bool(in, "Italique", f->italic);
    inst_field_set_bool(in, "Souligne", f->underline);
    inst_field_set_bool(in, "Barre", f->strike);
    if (f->hasColor)           inst_field_set_int(in, "Couleur", f->color);
    XlsCell* c = xls_find_cell(sheet, row, col);
    if (c) c->font = fontVal;
    vm_pop();
}

/* Materialise un composite Cadre depuis un RBorder et le pose sur la cellule. */
static void xls_apply_border(XlsSheet* sheet, int row, int col, const RBorder* b) {
    Value borderVal = owl_clone_global_template("Cadre");
    if (!IS_INSTANCE(borderVal)) return;
    vm_push(borderVal);
    ObjInstance* in = AS_INSTANCE(borderVal);
    if (b->hasColor) inst_field_set_int(in, "Couleur", b->color);
    inst_field_set_int(in, "Trait", b->trait);
    inst_field_set_int(in, "Bords", b->sides);
    XlsCell* c = xls_find_cell(sheet, row, col);
    if (c) c->border = borderVal;
    vm_pop();
}

/* Applique la combinaison de style d'index `xfIdx` (cellXfs) a la cellule
 * (row,col), qui doit deja exister. Reconstruit CouleurFond/Police/Cadre/
 * Alignements/Multiligne/Masque depuis les catalogues. */
static void xls_apply_style(XlsSheet* sheet, int row, int col, ReadStyles* rs, int xfIdx) {
    if (xfIdx < 0 || xfIdx >= rs->xfCount) return;
    RXf* x = &rs->xfs[xfIdx];

    if (x->fillId >= 0 && x->fillId < rs->fillCount && rs->fills[x->fillId].solid) {
        XlsCell* c = xls_find_cell(sheet, row, col);
        if (c) { c->bgColor = rs->fills[x->fillId].color; c->hasBg = true; }
    }
    if (x->hasAlignH) { XlsCell* c = xls_find_cell(sheet, row, col); if (c) { c->alignH = x->alignH; c->hasAlignH = true; } }
    if (x->hasAlignV) { XlsCell* c = xls_find_cell(sheet, row, col); if (c) { c->alignV = x->alignV; c->hasAlignV = true; } }
    if (x->wrap)      { XlsCell* c = xls_find_cell(sheet, row, col); if (c) { c->multiline = true; c->hasMultiline = true; } }
    if (x->numFmtId != 0) {
        for (int i = 0; i < rs->numFmtCount; i++) {
            if (rs->numFmts[i].id == x->numFmtId) {
                XlsCell* c = xls_find_cell(sheet, row, col);
                if (c) { free(c->mask); c->mask = xls_strdup(rs->numFmts[i].code); }
                break;
            }
        }
    }
    /* Composites en dernier : allocations OWL (clone + champs). */
    if (x->fontId > 0 && x->fontId < rs->fontCount)
        xls_apply_font(sheet, row, col, &rs->fonts[x->fontId]);
    if (x->borderId >= 1 && x->borderId < rs->borderCount && rs->borders[x->borderId].any)
        xls_apply_border(sheet, row, col, &rs->borders[x->borderId]);
}

/* Parse une worksheet et remplit la feuille (cellules texte + styles + fusions).
 * `rs` (peut etre NULL) fournit les styles reconstruits depuis xl/styles.xml. */
static void parse_worksheet(const char* xml, size_t len, XlsSheet* sheet,
                            SharedStrings* ss, ReadStyles* rs) {
    xmlDoc* doc = xmlReadMemory(xml, (int)len, "sheet.xml", NULL,
                                XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
    if (!doc) return;
    xmlNode* root = xmlDocGetRootElement(doc);  /* <worksheet> */
    if (!root) { xmlFreeDoc(doc); return; }

    for (xmlNode* sect = root->children; sect; sect = sect->next) {
        if (sect->type != XML_ELEMENT_NODE) continue;

        if (xmlStrcmp(sect->name, (const xmlChar*)"sheetData") == 0) {
            for (xmlNode* row = sect->children; row; row = row->next) {
                if (row->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(row->name, (const xmlChar*)"row") != 0) continue;
                for (xmlNode* c = row->children; c; c = c->next) {
                    if (c->type != XML_ELEMENT_NODE) continue;
                    if (xmlStrcmp(c->name, (const xmlChar*)"c") != 0) continue;
                    xmlChar* ref = xmlGetProp(c, (const xmlChar*)"r");
                    xmlChar* type = xmlGetProp(c, (const xmlChar*)"t");
                    xmlChar* sAttr = xmlGetProp(c, (const xmlChar*)"s");
                    int col = 0, rw = 0;
                    if (ref) xls_parse_cell_name((const char*)ref, &col, &rw);
                    int xfIdx = (sAttr && rs) ? atoi((const char*)sAttr) : -1;
                    /* Valeur : <v> (index shared si t="s", sinon littéral) ou
                     * <is><t> pour t="inlineStr". */
                    StrBuf val; sb_init(&val);
                    bool isShared = (type && xmlStrcmp(type, (const xmlChar*)"s") == 0);
                    bool isInline = (type && xmlStrcmp(type, (const xmlChar*)"inlineStr") == 0);
                    for (xmlNode* v = c->children; v; v = v->next) {
                        if (v->type != XML_ELEMENT_NODE) continue;
                        if (xmlStrcmp(v->name, (const xmlChar*)"v") == 0) {
                            xmlChar* txt = xmlNodeGetContent(v);
                            if (txt) { sb_puts(&val, (const char*)txt); xmlFree(txt); }
                        } else if (xmlStrcmp(v->name, (const xmlChar*)"is") == 0) {
                            collect_text_nodes(v, &val);
                        }
                    }
                    if (col > 0 && rw > 0) {
                        bool hasValue = false;
                        if (isShared && val.data) {
                            int idx = atoi(val.data);
                            if (idx >= 0 && idx < ss->count) {
                                xls_sheet_set(sheet, rw, col, ss->items[idx],
                                              (int)strlen(ss->items[idx]));
                                hasValue = true;
                            }
                        } else if (isInline || (type == NULL) || val.data) {
                            const char* v = val.data ? val.data : "";
                            xls_sheet_set(sheet, rw, col, v, (int)strlen(v));
                            hasValue = true;
                        }
                        /* Cellule stylee sans contenu : la materialiser quand meme
                         * pour porter le style relu (round-trip preservation). */
                        if (xfIdx >= 0) {
                            if (!hasValue) xls_cell_ensure(sheet, rw, col);
                            xls_apply_style(sheet, rw, col, rs, xfIdx);
                        }
                    }
                    sb_free(&val);
                    if (ref) xmlFree(ref);
                    if (type) xmlFree(type);
                    if (sAttr) xmlFree(sAttr);
                }
            }
        } else if (xmlStrcmp(sect->name, (const xmlChar*)"mergeCells") == 0) {
            for (xmlNode* m = sect->children; m; m = m->next) {
                if (m->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(m->name, (const xmlChar*)"mergeCell") != 0) continue;
                xmlChar* mref = xmlGetProp(m, (const xmlChar*)"ref");
                if (mref) {
                    char* colon = strchr((char*)mref, ':');
                    if (colon) {
                        *colon = '\0';
                        int fc, fr, lc, lr;
                        if (xls_parse_cell_name((char*)mref, &fc, &fr) &&
                            xls_parse_cell_name(colon + 1, &lc, &lr))
                            xls_sheet_add_merge(sheet, fr, fc, lr, lc);
                    }
                    xmlFree(mref);
                }
            }
        }
    }
    xmlFreeDoc(doc);
}

/* Extrait une entrée de l'archive vers un buffer heap (mz_free au caller).
 * Renvoie NULL si absente. */
static void* zip_extract(mz_zip_archive* zip, const char* name, size_t* outLen) {
    return mz_zip_reader_extract_file_to_heap(zip, name, outLen, 0);
}

/*  — une entree zip est-elle une part modelisee par OWL (regeneree a
 * chaque xlsSauve) ? Tout le reste (macros, medias, graphiques, drawings,
 * themes...) est preserve verbatim. Les worksheets `xl/worksheets/sheet<N>.xml`
 * sont regenerees, donc modelisees ; leurs rels et draws eventuels restent
 * preserves (byte-identiques). */
static bool xls_is_modeled_part(const char* name) {
    if (strcmp(name, "[Content_Types].xml") == 0) return true;
    if (strcmp(name, "_rels/.rels") == 0) return true;
    if (strcmp(name, "xl/workbook.xml") == 0) return true;
    if (strcmp(name, "xl/_rels/workbook.xml.rels") == 0) return true;
    if (strcmp(name, "xl/styles.xml") == 0) return true;
    if (strcmp(name, "xl/sharedStrings.xml") == 0) return true;
    /* xl/worksheets/sheet<digits>.xml (pas de sous-dossier _rels). */
    const char* pre = "xl/worksheets/sheet";
    size_t pl = strlen(pre);
    if (strncmp(name, pre, pl) == 0) {
        const char* p = name + pl;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { p++; digits++; }
        if (digits > 0 && strcmp(p, ".xml") == 0) return true;
    }
    return false;
}

/*  — capture les entrees zip non modelisees dans le classeur, pour les
 * reinjecter telles quelles a xlsSauve (preservation macros/graphiques/medias). */
static void xls_capture_preserved(ObjXlsDocument* d, mz_zip_archive* zip) {
    mz_uint n = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        if (xls_is_modeled_part(st.m_filename)) continue;
        size_t len = 0;
        void* data = mz_zip_reader_extract_to_heap(zip, i, &len, 0);
        if (!data) continue;
        if (d->preservedCount >= d->preservedCap) {
            int nc = d->preservedCap ? d->preservedCap * 2 : 4;
            d->preservedNames = (char**)realloc(d->preservedNames, sizeof(char*) * (size_t)nc);
            d->preservedData  = (void**)realloc(d->preservedData,  sizeof(void*) * (size_t)nc);
            d->preservedLens  = (size_t*)realloc(d->preservedLens, sizeof(size_t) * (size_t)nc);
            d->preservedCap = nc;
        }
        /* mz_free != free : recopie dans un buffer C-heap standard. */
        void* copy = malloc(len ? len : 1);
        if (copy && len) memcpy(copy, data, len);
        mz_free(data);
        d->preservedNames[d->preservedCount] = xls_strdup(st.m_filename);
        d->preservedData[d->preservedCount]  = copy;
        d->preservedLens[d->preservedCount]  = len;
        d->preservedCount++;
    }
}

/* Charge un .xlsx dans le document (déjà vidé). Renvoie true si OK. */
static bool xls_read_file(ObjXlsDocument* d, const char* path) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) return false;

    bool ok = true;
    SharedStrings ss; ss.items = NULL; ss.count = 0; ss.cap = 0;
    ReadStyles rs; memset(&rs, 0, sizeof(rs));

    /* Parts non modelisees (macros/graphiques/medias) -> memoire pour reinjection. */
    xls_capture_preserved(d, &zip);

    /* styles.xml (optionnel) -> catalogues + cellXfs pour reconstruire les styles. */
    size_t stLen = 0;
    void* stXml = zip_extract(&zip, "xl/styles.xml", &stLen);
    if (stXml) { parse_styles((const char*)stXml, stLen, &rs); mz_free(stXml); }

    /* sharedStrings (optionnel — un classeur sans chaîne n'en a pas). */
    size_t ssLen = 0;
    void* ssXml = zip_extract(&zip, "xl/sharedStrings.xml", &ssLen);
    if (ssXml) { parse_shared_strings((const char*)ssXml, ssLen, &ss); mz_free(ssXml); }

    /* workbook.xml -> noms + ordre des feuilles. */
    size_t wbLen = 0;
    void* wbXml = zip_extract(&zip, "xl/workbook.xml", &wbLen);
    if (!wbXml) { ok = false; }

    if (ok) {
        xmlDoc* wb = xmlReadMemory((const char*)wbXml, (int)wbLen, "workbook.xml",
                                   NULL, XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
        if (wb) {
            xmlNode* root = xmlDocGetRootElement(wb);  /* <workbook> */
            int sheetIdx = 0;
            for (xmlNode* sheets = root ? root->children : NULL; sheets; sheets = sheets->next) {
                if (sheets->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(sheets->name, (const xmlChar*)"sheets") != 0) continue;
                for (xmlNode* sh = sheets->children; sh; sh = sh->next) {
                    if (sh->type != XML_ELEMENT_NODE) continue;
                    if (xmlStrcmp(sh->name, (const xmlChar*)"sheet") != 0) continue;
                    xmlChar* nm = xmlGetProp(sh, (const xmlChar*)"name");
                    int idx = xls_doc_add_sheet(d, nm ? (const char*)nm : "Feuil");
                    if (nm) xmlFree(nm);
                    /* Charge la worksheet correspondante (nommage positionnel du writer). */
                    char wsName[48];
                    snprintf(wsName, sizeof(wsName), "xl/worksheets/sheet%d.xml", sheetIdx + 1);
                    size_t wsLen = 0;
                    void* wsXml = zip_extract(&zip, wsName, &wsLen);
                    if (wsXml) {
                        parse_worksheet((const char*)wsXml, wsLen, &d->sheets[idx], &ss, &rs);
                        mz_free(wsXml);
                    }
                    sheetIdx++;
                }
            }
            xmlFreeDoc(wb);
        } else ok = false;
    }

    if (wbXml) mz_free(wbXml);
    shared_free(&ss);
    read_styles_free(&rs);
    mz_zip_reader_end(&zip);

    if (d->sheetCount == 0) {
        /* Toujours au moins une feuille (invariant du type). */
        xls_doc_add_sheet(d, "Feuil1");
    }
    d->current = 0;
    return ok;
}

/*  — charge un .xls binaire hérité (BIFF8 dans conteneur OLE2) via
 * libxls, dans le document déjà vidé. Lot « valeurs » : texte relu tel quel,
 * numérique formaté en repr canonique OWL (owl_real_to_str, comme la voie
 * xlsx). Les dates restent des sérials bruts (mapping de format = lot ultérieur).
 *
 * Sur échec (fichier corrompu/tronqué -> wb NULL), renvoie false ; le caller
 * émet une erreur runtime propre. libxls libère cell->str à xls_close_WB : la
 * valeur est COPIÉE dans XlsCell (xls_sheet_set) avant fermeture. */
static bool xls_read_ole2_file(ObjXlsDocument* d, const char* path) {
    xls_error_t err = LIBXLS_OK;
    xlsWorkBook* wb = xls_open_file(path, "UTF-8", &err);
    if (!wb) return false;

    for (DWORD si = 0; si < wb->sheets.count; si++) {
        const char* nm = wb->sheets.sheet[si].name;  /* déjà UTF-8 */
        int sIdx = xls_doc_add_sheet(d, nm ? nm : "Feuil");
        XlsSheet* sheet = &d->sheets[sIdx];

        xlsWorkSheet* ws = xls_getWorkSheet(wb, (int)si);
        if (!ws) continue;
        if (xls_parseWorkSheet(ws) != LIBXLS_OK) { xls_close_WS(ws); continue; }

        for (WORD r = 0; r <= ws->rows.lastrow; r++) {
            struct st_row_data* row = &ws->rows.row[r];
            for (WORD c = 0; c <= ws->rows.lastcol; c++) {
                xlsCell* cell = &row->cells.cell[c];
                int rr = cell->row + 1, cc = cell->col + 1;  /* 0-based -> 1-based */

                bool isNum = false, isStr = false;
                switch (cell->id) {
                    case XLS_RECORD_LABELSST:
                    case XLS_RECORD_LABEL:
                    case XLS_RECORD_RSTRING:
                        isStr = true; break;
                    case XLS_RECORD_NUMBER:
                    case XLS_RECORD_RK:      /* MULRK est éclaté en RK par libxls */
                        isNum = true; break;
                    case XLS_RECORD_FORMULA:
                        /* résultat chaîne : l == 0xFFFF (str renseigné par le
                         * record STRING) ; résultat numérique : l == 0, d posé. */
                        if (cell->l == 0xFFFF) isStr = true; else isNum = true;
                        break;
                    default: break;  /* BLANK / MULBLANK / BOOLERR : cellule vide */
                }

                if (isStr) {
                    /* cell->str PEUT être NULL (formule chaîne vide, cellule
                     * dégénérée) : ne pas déréférencer aveuglément. */
                    if (cell->str)
                        xls_sheet_set(sheet, rr, cc, cell->str, (int)strlen(cell->str));
                } else if (isNum) {
                    /* Même formatage que la voie xlsx : 21.0 -> "21", 12.5 -> "12.5". */
                    char buf[40];
                    owl_real_to_str(cell->d, buf, (int)sizeof(buf));
                    xls_sheet_set(sheet, rr, cc, buf, (int)strlen(buf));
                    XlsCell* xc = xls_find_cell(sheet, rr, cc);
                    if (xc) xc->isNumber = true;
                }

                /* Fusion : colspan/rowspan comptent les cellules couvertes. */
                if (cell->colspan > 1 || cell->rowspan > 1) {
                    int lr = cell->row + (cell->rowspan > 1 ? cell->rowspan : 1);
                    int lc = cell->col + (cell->colspan > 1 ? cell->colspan : 1);
                    xls_sheet_add_merge(sheet, rr, cc, lr, lc);
                }
            }
        }
        xls_close_WS(ws);
    }
    xls_close_WB(wb);   /* libère cell->str : valeurs déjà copiées ci-dessus */

    if (d->sheetCount == 0) xls_doc_add_sheet(d, "Feuil1");
    d->current = 0;
    return true;
}

/* ================================================================
 * Conversion d'une Value en texte de cellule (UTF-8)
 * ================================================================ */

/* Jour julien (JDN) d'une date grégorienne — sert de base au sérial Excel.
 * Formule valide pour toute date grégorienne. */
static int64_t xls_date_to_jdn(int y, int m, int d) {
    int a = (14 - m) / 12;
    int yy = y + 4800 - a;
    int mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}

/* Numéro de série Excel (système 1900) : nombre de jours depuis 1899-12-30.
 * Cette base absorbe le bug historique « 1900 année bissextile » d'Excel pour
 * toute date ≥ 1900-03-01. Référence : 2020-01-01 → 43831. */
static int64_t xls_date_serial(int y, int m, int d) {
    /* JDN(1899-12-30) = 2415019. */
    return xls_date_to_jdn(y, m, d) - 2415019LL;
}

/* Écrit dans buf la forme texte d'une Value scalaire. Pour une chaîne, renvoie
 * directement le pointeur interne (pas de copie). *needFree indique si le
 * caller doit free le retour. */
static const char* value_to_cell_text(Value v, char* buf, size_t bufSize) {
    if (IS_STRING(v)) return AS_CSTRING(v);
    if (IS_INTEGER(v)) { snprintf(buf, bufSize, "%lld", (long long)AS_INTEGER(v)); return buf; }
    if (IS_REAL(v))    { owl_real_to_str(AS_REAL(v), buf, (int)bufSize); return buf; }
    if (IS_CURRENCY(v)) {
        /* Forme decimale canonique (12.50 -> "12.5") ; ecrit en cellule
         * numerique (cf. stdlib_xls_set_cell) pour rester sommable dans Excel. */
        currency_to_str(AS_CURRENCY(v), buf, (int)bufSize);
        return buf;
    }
    if (IS_DATE(v)) {
        ObjDate* dt = AS_DATE(v);
        snprintf(buf, bufSize, "%lld", (long long)xls_date_serial(dt->year, dt->month, dt->day));
        return buf;
    }
    if (IS_DATETIME(v)) {
        ObjDateTime* dt = AS_DATETIME(v);
        /* Sérial fractionnaire : partie entière = jours, partie fractionnaire =
         * fraction du jour, pour ne pas perdre l'heure. */
        double serial = (double)xls_date_serial(dt->year, dt->month, dt->day)
                      + ((dt->hour * 3600 + dt->minute * 60 + dt->second) + dt->ms / 1000.0)
                        / 86400.0;
        owl_real_to_str(serial, buf, (int)bufSize);
        return buf;
    }
    if (IS_BOOL(v))    { return AS_BOOL(v) ? "Vrai" : "Faux"; }
    if (IS_NIL(v))     { return ""; }
    /* Defaut sur : repr canonique via value_to_string (celle de Trace). Couvre
     * duree, heure (OBJ_TIME), numerique bignum (OBJ_NUMERIC) et tout type
     * futur — aucun scalaire ne peut plus disparaitre silencieusement en "".
     * L'ObjString retourne est frais mais non collectable ici : xls_sheet_set
     * (appelant) ne fait que du malloc/memcpy C, jamais d'allocation OWL, donc
     * pas de GC entre ce retour et la copie du buffer cellule. */
    return value_to_string(v)->chars;
}

/* ================================================================
 * API exposée à la VM (subscript 2D + propriétés)
 * ================================================================ */

bool stdlib_xls_is_document(Value v) { return IS_XLS_DOCUMENT(v); }

/* Résout un indice de colonne (entier 1-based ou lettres "A"). Renvoie true. */
bool stdlib_xls_resolve_col(Value v, int* outCol) {
    if (IS_INTEGER(v)) { *outCol = (int)AS_INTEGER(v); return *outCol >= 1; }
    if (IS_REAL(v))    { *outCol = (int)AS_REAL(v); return *outCol >= 1; }
    if (IS_STRING(v)) {
        const char* s = AS_CSTRING(v);
        int col = xls_letters_to_col(s, (int)strlen(s));
        if (col >= 1) { *outCol = col; return true; }
    }
    return false;
}

/* doc[row, col] = value. Renvoie false si row/col invalides. */
bool stdlib_xls_set_cell(Value docVal, int row, int col, Value v) {
    if (row < 1 || col < 1) return false;
    ObjXlsDocument* d = AS_XLS_DOCUMENT(docVal);
    XlsSheet* s = xls_current_sheet(d);
    if (!s) return false;
    char buf[64];
    const char* txt = value_to_cell_text(v, buf, sizeof(buf));
    xls_sheet_set(s, row, col, txt, (int)strlen(txt));
    XlsCell* cell = xls_find_cell(s, row, col);
    /* Tout type a valeur numerique s'ecrit en vraie cellule numerique OOXML
     * (<v> nu, sommable), pas en sharedString texte : entier, reel, montant,
     * date et dateheure (serial 1900). La forme chaine dans <v> reste celle
     * produite par value_to_cell_text ; seul le type de cellule change. Le
     * serial numerique est aussi le prerequis pour qu'un format de date
     * puisse s'appliquer cote styles. */
    if (cell) cell->isNumber = IS_INTEGER(v) || IS_REAL(v) || IS_CURRENCY(v) ||
                               IS_DATE(v) || IS_DATETIME(v);
    return true;
}

/*  — export « table -> .xlsx » du menu contextuel de table (corps).
 * Pure-données (aucun widget Qt) donc testable headless ; vit ici, dans owl_core,
 * pour etre linkable par owl-cli comme par owl.exe (le self-test round-trip est
 * un self-test core). Le menu Qt (ui_qt.cpp) se contente d'extraire entêtes +
 * lignes visibles et d'appeler ceci avec le chemin choisi. Entêtes en ligne 1,
 * données à partir de la ligne 2. headers[ncols], cells row-major nrows*ncols
 * (entrée NULL = cellule vide). Retourne 1 si écrit, 0 sinon. */
int owl_ui_table_export_xlsx(const char* path_utf8,
                             const char* const* headers, int ncols,
                             const char* const* cells, int nrows) {
    if (path_utf8 == NULL || ncols <= 0) return 0;
    Value doc = OBJ_VAL(obj_new_xls_document());
    /* Racine GC : le doc n'est référencé que par cette locale C ; obj_copy_string
     * dans la boucle peut déclencher un GC qui le collecterait sinon. */
    vm_push(doc);
    for (int c = 0; c < ncols; c++) {
        const char* h = headers ? headers[c] : NULL;
        stdlib_xls_set_cell(doc, 1, c + 1,
            OBJ_VAL(obj_copy_string(h ? h : "", h ? (int)strlen(h) : 0)));
    }
    for (int r = 0; r < nrows; r++) {
        for (int c = 0; c < ncols; c++) {
            const char* v = cells ? cells[r * ncols + c] : NULL;
            stdlib_xls_set_cell(doc, r + 2, c + 1,
                OBJ_VAL(obj_copy_string(v ? v : "", v ? (int)strlen(v) : 0)));
        }
    }
    bool ok = xls_write_file(AS_XLS_DOCUMENT(doc), path_utf8);
    vm_pop();
    return ok ? 1 : 0;
}

/* doc[row, col] -> chaîne (vide si absente). */
Value stdlib_xls_get_cell(Value docVal, int row, int col) {
    ObjXlsDocument* d = AS_XLS_DOCUMENT(docVal);
    XlsSheet* s = xls_current_sheet(d);
    if (s && row >= 1 && col >= 1) {
        XlsCell* c = xls_find_cell(s, row, col);
        if (c && c->value)
            return OBJ_VAL(obj_copy_string(c->value, (int)strlen(c->value)));
    }
    return OBJ_VAL(obj_copy_string("", 0));
}

/* Propriété ..NombreFeuille / ..NombreLigne / ..NombreColonne (lecture seule).
 * Renvoie true + pose *out si `prop` matche, false sinon. */
bool stdlib_xls_get_property(Value docVal, const char* prop, Value* out) {
    ObjXlsDocument* d = AS_XLS_DOCUMENT(docVal);
    XlsSheet* s = xls_current_sheet(d);
    if (strcasecmp(prop, "NombreFeuille") == 0 || strcasecmp(prop, "SheetCount") == 0 ||
        strcasecmp(prop, "NombreDeFeuille") == 0) {
        *out = INTEGER_VAL(d->sheetCount); return true;
    }
    if (strcasecmp(prop, "NombreLigne") == 0 || strcasecmp(prop, "RowCount") == 0) {
        *out = INTEGER_VAL(s ? xls_sheet_max_row(s) : 0); return true;
    }
    if (strcasecmp(prop, "NombreColonne") == 0 || strcasecmp(prop, "ColumnCount") == 0) {
        *out = INTEGER_VAL(s ? xls_sheet_max_col(s) : 0); return true;
    }
    return false;
}

/* ================================================================
 *  — références de style (cellule / ligne / colonne)
 *
 * `doc..Cellule[l,c]..CouleurFond = X` se compile en une chaîne d'accès :
 *   doc  --OP_STRUCT_GET "Cellule"-->  ref(CELL, non liée)
 *        --OP_ARRAY_GET_2D [l,c]---->  ref(CELL, liée l,c)
 *        --OP_STRUCT_SET "CouleurFond"-> écrit le style de la cellule
 * Le style vit sur XlsCell (scalaires + composites Police/Cadre). Aucune
 * génération OOXML dans ce lot : le style est purement en mémoire.
 * ================================================================ */

bool stdlib_xls_is_ref(Value v) { return IS_XLS_REF(v); }

/* doc..Cellule / ..Colonne / ..Ligne : renvoie une référence NON liée. */
bool stdlib_xls_document_subref(Value docVal, const char* prop, Value* out) {
    ObjXlsDocument* d = AS_XLS_DOCUMENT(docVal);
    XlsRefKind kind;
    if (strcasecmp(prop, "Cellule") == 0 || strcasecmp(prop, "Cell") == 0)
        kind = XLS_REF_CELL;
    else if (strcasecmp(prop, "Colonne") == 0 || strcasecmp(prop, "Column") == 0)
        kind = XLS_REF_COLUMN;
    else if (strcasecmp(prop, "Ligne") == 0 || strcasecmp(prop, "Row") == 0)
        kind = XLS_REF_ROW;
    else
        return false;
    *out = OBJ_VAL(obj_new_xls_ref(d, kind));
    return true;
}

/* Lie une référence CELL via un subscript 2D [ligne, colonne]. */
Value stdlib_xls_ref_bind_cell(Value refVal, int row, int col) {
    ObjXlsRef* r = AS_XLS_REF(refVal);
    ObjXlsRef* b = obj_new_xls_ref(r->doc, XLS_REF_CELL);
    b->row = row; b->col = col; b->bound = true;
    return OBJ_VAL(b);
}

/* Lie une référence ROW/COLUMN via un subscript 1D [index]. */
Value stdlib_xls_ref_bind_index(Value refVal, int idx) {
    ObjXlsRef* r = AS_XLS_REF(refVal);
    ObjXlsRef* b = obj_new_xls_ref(r->doc, r->kind);
    if (r->kind == XLS_REF_ROW) { b->row = idx; b->col = 0; }
    else                        { b->col = idx; b->row = 0; }
    b->bound = true;
    return OBJ_VAL(b);
}

/* Prop composite (Police/Cadre) : matérialise le composite sur la cellule si
 * absent, renvoie l'instance stockée (round-trip `..Police..Nom`). L'appelant
 * DOIT avoir la ref rootée (clone alloue → GC possible). */
static Value xls_cell_composite(XlsCell* c, bool wantFont) {
    Value* slot = wantFont ? &c->font : &c->border;
    if (IS_NIL(*slot)) {
        Value tpl = owl_clone_global_template(wantFont ? "Police" : "Cadre");
        *slot = tpl;  /* clone ne réalloue pas `cells` : pointeur c toujours valide */
    }
    return *slot;
}

/* Lecture d'une propriété de style sur une référence liée. */
bool stdlib_xls_ref_get_property(Value refVal, const char* prop, Value* out) {
    ObjXlsRef* r = AS_XLS_REF(refVal);
    if (!r->bound) return false;
    XlsSheet* s = xls_current_sheet(r->doc);
    if (!s) return false;

    if (r->kind == XLS_REF_CELL) {
        XlsCell* c = xls_cell_ensure(s, r->row, r->col);
        if (strcasecmp(prop, "CouleurFond") == 0 || strcasecmp(prop, "BackgroundColor") == 0) {
            *out = c->hasBg ? INTEGER_VAL(c->bgColor) : INTEGER_VAL(-1); return true;
        }
        if (strcasecmp(prop, "Police") == 0 || strcasecmp(prop, "Font") == 0) {
            *out = xls_cell_composite(c, true); return true;
        }
        if (strcasecmp(prop, "Cadre") == 0 || strcasecmp(prop, "Border") == 0) {
            *out = xls_cell_composite(c, false); return true;
        }
        if (strcasecmp(prop, "AlignementH") == 0 || strcasecmp(prop, "AlignmentH") == 0) {
            *out = INTEGER_VAL(c->alignH); return true;
        }
        if (strcasecmp(prop, "AlignementV") == 0 || strcasecmp(prop, "AlignmentV") == 0) {
            *out = INTEGER_VAL(c->alignV); return true;
        }
        if (strcasecmp(prop, "Multiligne") == 0 || strcasecmp(prop, "Multiline") == 0) {
            *out = BOOL_VAL(c->multiline); return true;
        }
        if (strcasecmp(prop, "Masque") == 0 || strcasecmp(prop, "Mask") == 0) {
            *out = OBJ_VAL(obj_copy_string(c->mask ? c->mask : "",
                                           c->mask ? (int)strlen(c->mask) : 0));
            return true;
        }
        return false;
    }
    if (r->kind == XLS_REF_ROW) {
        if (strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0) {
            double h; *out = REAL_VAL(xls_dim_get(s->rowDims, s->rowDimCount, r->row, &h) ? h : -1.0);
            return true;
        }
        return false;
    }
    if (r->kind == XLS_REF_COLUMN) {
        if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0) {
            double w; *out = REAL_VAL(xls_dim_get(s->colDims, s->colDimCount, r->col, &w) ? w : -1.0);
            return true;
        }
        return false;
    }
    return false;
}

/* Écriture d'une propriété de style sur une référence liée. Chaque branche de
 * propriété connue valide le type de la valeur : un type incompatible renvoie
 * XLS_SET_BAD_TYPE (avec le type attendu dans *outExpected) plutôt que d'avaler
 * l'affectation en silence. */
XlsSetResult stdlib_xls_ref_set_property(Value refVal, const char* prop, Value val,
                                         const char** outExpected) {
    ObjXlsRef* r = AS_XLS_REF(refVal);
    if (!r->bound) return XLS_SET_UNKNOWN_PROP;
    XlsSheet* s = xls_current_sheet(r->doc);
    if (!s) return XLS_SET_UNKNOWN_PROP;
    bool en = owl_error_render_lang_is_en();

    if (r->kind == XLS_REF_CELL) {
        XlsCell* c = xls_cell_ensure(s, r->row, r->col);
        if (strcasecmp(prop, "CouleurFond") == 0 || strcasecmp(prop, "BackgroundColor") == 0) {
            if (IS_INTEGER(val)) { c->bgColor = AS_INTEGER(val); c->hasBg = true; return XLS_SET_OK; }
            if (IS_REAL(val)) { c->bgColor = (int64_t)AS_REAL(val); c->hasBg = true; return XLS_SET_OK; }
            *outExpected = en ? "a color integer (e.g. Blue, RGB(...))"
                              : "un entier de couleur (ex : Bleu, RVB(...))";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "Police") == 0 || strcasecmp(prop, "Font") == 0) {
            /* Affectation d'un composite Police entier (variable / littéral). */
            if (IS_INSTANCE(val)) { c->font = val; return XLS_SET_OK; }
            *outExpected = en ? "a Font composite" : "un composite Police";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "Cadre") == 0 || strcasecmp(prop, "Border") == 0) {
            if (IS_INSTANCE(val)) { c->border = val; return XLS_SET_OK; }
            *outExpected = en ? "a Border composite" : "un composite Cadre";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "AlignementH") == 0 || strcasecmp(prop, "AlignmentH") == 0) {
            if (IS_INTEGER(val)) { c->alignH = (int)AS_INTEGER(val); c->hasAlignH = true; return XLS_SET_OK; }
            *outExpected = en ? "an integer (alignment constant)" : "un entier (constante d'alignement)";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "AlignementV") == 0 || strcasecmp(prop, "AlignmentV") == 0) {
            if (IS_INTEGER(val)) { c->alignV = (int)AS_INTEGER(val); c->hasAlignV = true; return XLS_SET_OK; }
            *outExpected = en ? "an integer (alignment constant)" : "un entier (constante d'alignement)";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "Multiligne") == 0 || strcasecmp(prop, "Multiline") == 0) {
            if (IS_BOOL(val)) { c->multiline = AS_BOOL(val); c->hasMultiline = true; return XLS_SET_OK; }
            *outExpected = en ? "a boolean (True/False)" : "un booléen (Vrai/Faux)";
            return XLS_SET_BAD_TYPE;
        }
        if (strcasecmp(prop, "Masque") == 0 || strcasecmp(prop, "Mask") == 0) {
            if (IS_STRING(val)) {
                free(c->mask);
                const char* m = AS_CSTRING(val);
                size_t n = strlen(m);
                c->mask = (char*)malloc(n + 1);
                memcpy(c->mask, m, n + 1);
                return XLS_SET_OK;
            }
            *outExpected = en ? "a text string" : "un texte";
            return XLS_SET_BAD_TYPE;
        }
        return XLS_SET_UNKNOWN_PROP;
    }
    if (r->kind == XLS_REF_ROW) {
        if (strcasecmp(prop, "Hauteur") == 0 || strcasecmp(prop, "Height") == 0) {
            if (!IS_INTEGER(val) && !IS_REAL(val)) {
                *outExpected = en ? "a number" : "un nombre";
                return XLS_SET_BAD_TYPE;
            }
            double h = IS_INTEGER(val) ? (double)AS_INTEGER(val) : AS_REAL(val);
            xls_dim_set(&s->rowDims, &s->rowDimCount, &s->rowDimCap, r->row, h);
            return XLS_SET_OK;
        }
        return XLS_SET_UNKNOWN_PROP;
    }
    if (r->kind == XLS_REF_COLUMN) {
        if (strcasecmp(prop, "Largeur") == 0 || strcasecmp(prop, "Width") == 0) {
            if (!IS_INTEGER(val) && !IS_REAL(val)) {
                *outExpected = en ? "a number" : "un nombre";
                return XLS_SET_BAD_TYPE;
            }
            double w = IS_INTEGER(val) ? (double)AS_INTEGER(val) : AS_REAL(val);
            xls_dim_set(&s->colDims, &s->colDimCount, &s->colDimCap, r->col, w);
            return XLS_SET_OK;
        }
        return XLS_SET_UNKNOWN_PROP;
    }
    return XLS_SET_UNKNOWN_PROP;
}

/* ================================================================
 * Natives
 * ================================================================ */

static Value fn_xls_document_new(int argCount, Value* args) {
    (void)argCount; (void)args;
    return OBJ_VAL(obj_new_xls_document());
}

/*  — reniflage de format par signature. Un .xlsx est un ZIP
 * ("PK\x03\x04") ; un .xls binaire hérité est un container OLE2
 * ("\xD0\xCF\x11\xE0"). On distingue les deux pour ne PAS laisser un .xls
 * échouer en silence (classeur vide) — même classe de bug que celle chassée
 * sur la lecture des valeurs. */
typedef enum { XLS_FMT_ZIP, XLS_FMT_OLE2, XLS_FMT_ABSENT, XLS_FMT_UNKNOWN } XlsFormat;

static XlsFormat xls_sniff_format(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return XLS_FMT_ABSENT;
    unsigned char sig[8]; size_t n = fread(sig, 1, sizeof(sig), f);
    fclose(f);
    if (n >= 4 && sig[0] == 0x50 && sig[1] == 0x4B && sig[2] == 0x03 && sig[3] == 0x04)
        return XLS_FMT_ZIP;
    if (n >= 4 && sig[0] == 0xD0 && sig[1] == 0xCF && sig[2] == 0x11 && sig[3] == 0xE0)
        return XLS_FMT_OLE2;
    return XLS_FMT_UNKNOWN;
}

static Value fn_xls_ouvre(int argCount, Value* args) {
    OWL_REQUIRE_ARG("xlsOuvre", "chemin", 0, IS_STRING(args[0]), "cha\xc3\xaene", NIL_VAL);
    (void)argCount;
    const char* path = AS_CSTRING(args[0]);
    ObjXlsDocument* d = obj_new_xls_document();
    vm_push(OBJ_VAL(d));            /* root pendant la lecture (allocations) */
    xls_doc_clear(d);              /* repart d'un classeur vide avant relecture */

    XlsFormat fmt = xls_sniff_format(path);
    if (fmt == XLS_FMT_OLE2) {
        /* .xls binaire hérité (BIFF8/OLE2) : lecteur libxls. Un fichier
         * malformé/tronqué (wb NULL) donne une erreur runtime explicite plutôt
         * qu'un classeur vide muet — libxls parse du binaire non fiable. */
        if (!xls_read_ole2_file(d, path)) {
            owl_emit_native_error(E_NATIVE_IO_OPEN_FAILURE, "xlsOuvre",
                                  "path", path,
                                  "detail", "fichier .xls illisible ou corrompu",
                                  NULL);
            if (d->sheetCount == 0) xls_doc_add_sheet(d, "Feuil1");
        }
        free(d->path);
        d->path = xls_strdup(path);
        vm_pop();
        return OBJ_VAL(d);
    }

    if (!xls_read_file(d, path)) {
        /* Fichier absent/illisible : garde un classeur vide utilisable. */
        if (d->sheetCount == 0) xls_doc_add_sheet(d, "Feuil1");
    }
    free(d->path);
    d->path = xls_strdup(path);
    vm_pop();
    return OBJ_VAL(d);
}

static Value fn_xls_sauve(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsSauve", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", BOOL_VAL(false));
    OWL_REQUIRE_ARG("xlsSauve", "chemin", 1, IS_STRING(args[1]), "cha\xc3\xaene", BOOL_VAL(false));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    const char* path = AS_CSTRING(args[1]);
    bool ok = xls_write_file(d, path);
    if (ok) { free(d->path); d->path = xls_strdup(path); }
    return BOOL_VAL(ok);
}

static Value fn_xls_ferme(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsFerme", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", NIL_VAL);
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    xls_doc_clear(d);
    free(d->path); d->path = NULL;
    /* Un classeur reste toujours porteur d'au moins une feuille. */
    xls_doc_add_sheet(d, "Feuil1");
    return NIL_VAL;
}

static Value fn_xls_ajoute_feuille(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsAjouteFeuille", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", BOOL_VAL(false));
    OWL_REQUIRE_ARG("xlsAjouteFeuille", "nom", 1, IS_STRING(args[1]), "cha\xc3\xaene", BOOL_VAL(false));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    xls_doc_add_sheet(d, AS_CSTRING(args[1]));
    return BOOL_VAL(true);
}

static Value fn_xls_feuille_en_cours(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsFeuilleEnCours", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", BOOL_VAL(false));
    OWL_REQUIRE_ARG("xlsFeuilleEnCours", "num\xc3\xa9ro", 1, IS_INTEGER(args[1]), "entier", BOOL_VAL(false));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    int num = (int)AS_INTEGER(args[1]);  /* 1-based */
    if (num < 1 || num > d->sheetCount) return BOOL_VAL(false);
    d->current = num - 1;
    return BOOL_VAL(true);
}

static Value fn_xls_donnee(int argCount, Value* args) {
    OWL_REQUIRE_ARG("xlsDonn\xc3\xa9""e", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", NIL_VAL);
    OWL_REQUIRE_ARG("xlsDonn\xc3\xa9""e", "ligne", 1, IS_INTEGER(args[1]), "entier", NIL_VAL);
    int row = (int)AS_INTEGER(args[1]);
    int col = 1;
    if (argCount >= 3) {
        if (!stdlib_xls_resolve_col(args[2], &col)) return OBJ_VAL(obj_copy_string("", 0));
    }
    return stdlib_xls_get_cell(args[0], row, col);
}

static Value fn_xls_fusionne_cellule(int argCount, Value* args) {
    OWL_REQUIRE_ARG("xlsFusionneCellule", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", BOOL_VAL(false));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    XlsSheet* s = xls_current_sheet(d);
    if (!s) return BOOL_VAL(false);

    int fr, fc, lr, lc;
    if (argCount >= 3 && IS_STRING(args[1]) && IS_STRING(args[2])) {
        /* Forme par nom de cellule : xlsFusionneCellule(doc, "B2", "D4"). */
        if (!xls_parse_cell_name(AS_CSTRING(args[1]), &fc, &fr)) return BOOL_VAL(false);
        if (!xls_parse_cell_name(AS_CSTRING(args[2]), &lc, &lr)) return BOOL_VAL(false);
    } else if (argCount >= 5 && IS_INTEGER(args[1]) && IS_INTEGER(args[2]) &&
               IS_INTEGER(args[3]) && IS_INTEGER(args[4])) {
        /* Forme numérique : xlsFusionneCellule(doc, ligneDéb, colDéb, ligneFin, colFin). */
        fr = (int)AS_INTEGER(args[1]); fc = (int)AS_INTEGER(args[2]);
        lr = (int)AS_INTEGER(args[3]); lc = (int)AS_INTEGER(args[4]);
    } else {
        return BOOL_VAL(false);
    }
    xls_sheet_add_merge(s, fr, fc, lr, lc);
    return BOOL_VAL(true);
}

static Value fn_xls_defusionne_cellule(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsD\xc3\xa9""fusionneCellule", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", BOOL_VAL(false));
    OWL_REQUIRE_ARG("xlsD\xc3\xa9""fusionneCellule", "cellule", 1, IS_STRING(args[1]), "cha\xc3\xaene", BOOL_VAL(false));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    XlsSheet* s = xls_current_sheet(d);
    if (!s) return BOOL_VAL(false);
    int col, row;
    if (!xls_parse_cell_name(AS_CSTRING(args[1]), &col, &row)) return BOOL_VAL(false);
    /* Retire toute fusion dont le coin haut-gauche est cette cellule. */
    bool removed = false;
    for (int i = 0; i < s->mergeCount; i++) {
        if (s->merges[i].firstRow == row && s->merges[i].firstCol == col) {
            for (int j = i; j < s->mergeCount - 1; j++) s->merges[j] = s->merges[j + 1];
            s->mergeCount--; i--; removed = true;
        }
    }
    return BOOL_VAL(removed);
}

static Value fn_xls_nb_ligne(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsNbLigne", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", INTEGER_VAL(0));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    XlsSheet* s = xls_current_sheet(d);
    return INTEGER_VAL(s ? xls_sheet_max_row(s) : 0);
}

static Value fn_xls_nb_colonne(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsNbColonne", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", INTEGER_VAL(0));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    XlsSheet* s = xls_current_sheet(d);
    return INTEGER_VAL(s ? xls_sheet_max_col(s) : 0);
}

static Value fn_xls_nb_feuille(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsNbFeuille", "document", 0, IS_XLS_DOCUMENT(args[0]), "xlsDocument", INTEGER_VAL(0));
    ObjXlsDocument* d = AS_XLS_DOCUMENT(args[0]);
    return INTEGER_VAL(d->sheetCount);
}

static Value fn_xls_coord_vers_nom(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsCoordonn\xc3\xa9""esVersNom", "colonne", 0, IS_INTEGER(args[0]), "entier", NIL_VAL);
    OWL_REQUIRE_ARG("xlsCoordonn\xc3\xa9""esVersNom", "ligne", 1, IS_INTEGER(args[1]), "entier", NIL_VAL);
    int col = (int)AS_INTEGER(args[0]);
    int row = (int)AS_INTEGER(args[1]);
    char letters[16], out[32];
    xls_col_to_letters(col, letters, sizeof(letters));
    snprintf(out, sizeof(out), "%s%d", letters, row);
    return OBJ_VAL(obj_copy_string(out, (int)strlen(out)));
}

static Value fn_xls_nom_vers_coord(int argCount, Value* args) {
    (void)argCount;
    OWL_REQUIRE_ARG("xlsNomVersCoordonn\xc3\xa9""es", "nom", 0, IS_STRING(args[0]), "cha\xc3\xaene", NIL_VAL);
    OWL_REQUIRE_ARG("xlsNomVersCoordonn\xc3\xa9""es", "type", 1, IS_INTEGER(args[1]), "entier", NIL_VAL);
    int col, row;
    if (!xls_parse_cell_name(AS_CSTRING(args[0]), &col, &row)) return INTEGER_VAL(0);
    int type = (int)AS_INTEGER(args[1]);
    if (type == XLS_NUM_COLONNE) return INTEGER_VAL(col);
    return INTEGER_VAL(row);  /* xlsNumLigne (défaut) */
}

/* ================================================================
 * Registry
 * ================================================================ */

static const OwlField FIELDS_xlsDocument[] = {
    { .name="NombreFeuille", .type="entier", .defaultValue="0",
      .descFR="Nombre de feuilles du classeur (lecture seule).",
      .descEN="Number of sheets in the workbook (read-only).", .nameEN="SheetCount" },
    { .name="NombreLigne", .type="entier", .defaultValue="0",
      .descFR="Dernière ligne occupée de la feuille en cours (lecture seule).",
      .descEN="Last used row of the current sheet (read-only).", .nameEN="RowCount" },
    { .name="NombreColonne", .type="entier", .defaultValue="0",
      .descFR="Dernière colonne occupée de la feuille en cours (lecture seule).",
      .descEN="Last used column of the current sheet (read-only).", .nameEN="ColumnCount" },
    /*  — accès indexé au style. `doc..Cellule[l, c]..CouleurFond = X`,
     * `doc..Ligne[l]..Hauteur = H`, `doc..Colonne[c]..Largeur = W`. */
    { .name="Cellule", .type="xlsCellule", .defaultValue="",
      .descFR="Accès au style d'une cellule de la feuille en cours : `..Cellule[ligne, colonne]`.",
      .descEN="Access to a cell's style on the current sheet: `..Cell[row, column]`.",
      .nameEN="Cell" },
    { .name="Ligne", .type="xlsLigne", .defaultValue="",
      .descFR="Accès au style d'une ligne de la feuille en cours : `..Ligne[ligne]`.",
      .descEN="Access to a row's style on the current sheet: `..Row[row]`.",
      .nameEN="Row" },
    { .name="Colonne", .type="xlsColonne", .defaultValue="",
      .descFR="Accès au style d'une colonne de la feuille en cours : `..Colonne[colonne]`.",
      .descEN="Access to a column's style on the current sheet: `..Column[column]`.",
      .nameEN="Column" },
    { NULL }
};

/*  — propriétés de style d'une cellule. Le Cadre réutilise le type
 * composite existant, mais SEULS Couleur, Trait et Bords s'appliquent en cellule
 * Excel (épaisseur, arrondis et traits par-côté non gérés à ce niveau). */
static const OwlField FIELDS_xlsCellule[] = {
    { .name="CouleurFond", .type="entier", .defaultValue="-1",
      .descFR="Couleur de remplissage de la cellule (entier couleur RVB ou constante couleur).",
      .descEN="Cell fill color (RGB color integer or a color constant).",
      .nameEN="BackgroundColor" },
    { .name="Police", .type="Police", .defaultValue="",
      .descFR="Police du texte de la cellule (composite : Nom, Taille, Gras, Italique, Couleur, Souligné…).",
      .descEN="Cell text font (composite: Name, Size, Bold, Italic, Color, Underlined…).",
      .nameEN="Font" },
    { .name="Cadre", .type="Cadre", .defaultValue="",
      .descFR="Bordure de la cellule. En cellule Excel, seuls Couleur, Trait et Bords sont pris en compte.",
      .descEN="Cell border. In an Excel cell, only Color, Line and Sides are taken into account.",
      .nameEN="Border" },
    { .name="AlignementH", .type="entier", .defaultValue="0",
      .descFR="Alignement horizontal du contenu (chGauche, chCentre, chDroite).",
      .descEN="Horizontal content alignment (chLeft, chCenter, chRight).",
      .nameEN="AlignmentH" },
    { .name="AlignementV", .type="entier", .defaultValue="0",
      .descFR="Alignement vertical du contenu (cvHaut, cvMilieu, cvBas).",
      .descEN="Vertical content alignment (cvTop, cvMiddle, cvBottom).",
      .nameEN="AlignmentV" },
    { .name="Multiligne", .type="bool\xc3\xa9""en", .defaultValue="Faux",
      .descFR="Renvoie le texte à la ligne dans la cellule (retour automatique).",
      .descEN="Wraps text within the cell (automatic line break).",
      .nameEN="Multiline" },
    { .name="Masque", .type="cha\xc3\xaene", .defaultValue="",
      .descFR="Format d'affichage : une constante xlsFormat* ou une chaîne de format Excel brute.",
      .descEN="Display format: an xlsFormat* constant or a raw Excel format string.",
      .nameEN="Mask" },
    { NULL }
};

static const OwlField FIELDS_xlsLigne[] = {
    { .name="Hauteur", .type="r\xc3\xa9""el", .defaultValue="-1",
      .descFR="Hauteur de la ligne en pixels (-1 = hauteur par défaut).",
      .descEN="Row height in pixels (-1 = default height).",
      .nameEN="Height" },
    { NULL }
};

static const OwlField FIELDS_xlsColonne[] = {
    { .name="Largeur", .type="r\xc3\xa9""el", .defaultValue="-1",
      .descFR="Largeur de la colonne en pixels (-1 = largeur par défaut).",
      .descEN="Column width in pixels (-1 = default width).",
      .nameEN="Width" },
    { NULL }
};

static const char* const ALIAS_xls_document[] = { "xlsDocument", NULL };

static const OwlTypeDef XLS_TYPES[] = {
    { .canonical      = "xlsDocument",
      .canonicalAscii = "xlsDocument",
      .aliases        = ALIAS_xls_document,
      .category       = "Tableur",
      .descFR         = "Classeur Excel (.xlsx) manipulé en mémoire. Se déclare "
                        "directement (`doc est un xlsDocument`), sans ouvrir de "
                        "fichier. Une cellule s'écrit et se lit par indexation "
                        "`doc[ligne, colonne]` ; les propriétés en lecture seule "
                        "`..NombreFeuille`, `..NombreLigne` et `..NombreColonne` "
                        "reflètent l'état du classeur. Enregistrement via "
                        "xlsSauve, relecture via xlsOuvre.",
      .descEN         = "Excel workbook (.xlsx) handled in memory. Declared "
                        "directly (`doc is an xlsDocument`), without opening a "
                        "file. A cell is written and read through `doc[row, "
                        "column]` indexing; the read-only properties "
                        "`..SheetCount`, `..RowCount` and `..ColumnCount` reflect "
                        "the workbook state. Saved with xlsSave, reloaded with "
                        "xlsOpen.",
      .fields         = FIELDS_xlsDocument,
      .coercible      = false,
      .hasDefault     = true,
      .defaultValue   = NIL_VAL,
      .nameEN         = "xlsDocument",
      .genre          = OWL_GENRE_M,
      .defaultFactoryNative = "__XlsDocumentNew" },

    /*  — sous-objets de style. Non instanciables directement : ils ne
     * s'obtiennent que via `doc..Cellule[...]` / `..Ligne[...]` / `..Colonne[...]`. */
    { .canonical      = "xlsCellule",
      .canonicalAscii = "xlsCellule",
      .category       = "Tableur",
      .descFR         = "Style d'une cellule d'un classeur, obtenu par "
                        "`doc..Cellule[ligne, colonne]`. Porte couleur de fond, "
                        "police, cadre, alignements, retour à la ligne et masque "
                        "de format. Les valeurs sont conservées en mémoire.",
      .descEN         = "Style of a workbook cell, obtained through "
                        "`doc..Cell[row, column]`. Carries fill color, font, "
                        "border, alignments, text wrapping and format mask. Values "
                        "are kept in memory.",
      .fields         = FIELDS_xlsCellule,
      .coercible      = false, .hasDefault = false, .defaultValue = NIL_VAL,
      .nameEN         = "xlsCell", .genre = OWL_GENRE_F },

    { .canonical      = "xlsLigne",
      .canonicalAscii = "xlsLigne",
      .category       = "Tableur",
      .descFR         = "Style d'une ligne d'un classeur, obtenu par "
                        "`doc..Ligne[ligne]`. Expose la hauteur de la ligne.",
      .descEN         = "Style of a workbook row, obtained through "
                        "`doc..Row[row]`. Exposes the row height.",
      .fields         = FIELDS_xlsLigne,
      .coercible      = false, .hasDefault = false, .defaultValue = NIL_VAL,
      .nameEN         = "xlsRow", .genre = OWL_GENRE_F },

    { .canonical      = "xlsColonne",
      .canonicalAscii = "xlsColonne",
      .category       = "Tableur",
      .descFR         = "Style d'une colonne d'un classeur, obtenu par "
                        "`doc..Colonne[colonne]`. Expose la largeur de la colonne.",
      .descEN         = "Style of a workbook column, obtained through "
                        "`doc..Column[column]`. Exposes the column width.",
      .fields         = FIELDS_xlsColonne,
      .coercible      = false, .hasDefault = false, .defaultValue = NIL_VAL,
      .nameEN         = "xlsColumn", .genre = OWL_GENRE_F },
};

/* -- Signatures -- */
static const OwlParam p_ouvre[] = {
    {"chemin", "cha\xc3\xaene", "Chemin du fichier .xlsx"},
    {"mode",   "entier", "Mode d'ouverture (xlsEcriture, xlsCompatible)", .allowedBase=NULL},
    {NULL,NULL,NULL}
};
static const OwlParam p_ouvre1[] = {
    {"chemin", "cha\xc3\xaene", "Chemin du fichier .xlsx"},
    {NULL,NULL,NULL}
};
static const OwlParam p_sauve[] = {
    {"document", "xlsDocument", "Classeur à enregistrer"},
    {"chemin",   "cha\xc3\xaene", "Chemin du fichier .xlsx à écrire"},
    {NULL,NULL,NULL}
};
static const OwlParam p_ferme[] = {
    {"document", "xlsDocument", "Classeur à fermer"},
    {NULL,NULL,NULL}
};
static const OwlParam p_ajoute[] = {
    {"document", "xlsDocument", "Classeur cible"},
    {"nom",      "cha\xc3\xaene", "Nom de la nouvelle feuille"},
    {NULL,NULL,NULL}
};
static const OwlParam p_feuille[] = {
    {"document", "xlsDocument", "Classeur cible"},
    {"num\xc3\xa9ro", "entier", "Numéro de feuille (1-based)"},
    {NULL,NULL,NULL}
};
static const OwlParam p_donnee[] = {
    {"document", "xlsDocument", "Classeur source"},
    {"ligne",    "entier", "Numéro de ligne (1-based)"},
    {"colonne",  "entier", "Numéro de colonne (1-based)"},
    {NULL,NULL,NULL}
};
static const OwlParam p_fusionne_nom[] = {
    {"document", "xlsDocument", "Classeur cible"},
    {"celluleD\xc3\xa9""but", "cha\xc3\xaene", "Cellule haut-gauche (ex. \"B2\")"},
    {"celluleFin",  "cha\xc3\xaene", "Cellule bas-droite (ex. \"D4\")"},
    {NULL,NULL,NULL}
};
static const OwlParam p_defusionne[] = {
    {"document", "xlsDocument", "Classeur cible"},
    {"cellule",  "cha\xc3\xaene", "Cellule haut-gauche de la fusion (ex. \"B2\")"},
    {NULL,NULL,NULL}
};
static const OwlParam p_doc_only[] = {
    {"document", "xlsDocument", "Classeur interrogé"},
    {NULL,NULL,NULL}
};
static const OwlParam p_coord_vers_nom[] = {
    {"colonne", "entier", "Numéro de colonne (1-based)"},
    {"ligne",   "entier", "Numéro de ligne (1-based)"},
    {NULL,NULL,NULL}
};
static const OwlParam p_nom_vers_coord[] = {
    {"nom",  "cha\xc3\xaene", "Nom de cellule (ex. \"C1\")"},
    {"type", "entier", "xlsNumLigne ou xlsNumColonne", .allowedBase=&E_xlsCoordType},
    {NULL,NULL,NULL}
};

static const OwlSignature sig_ouvre[] = {
    {1, p_ouvre1, "xlsDocument", "Classeur chargé"},
    {2, p_ouvre,  "xlsDocument", "Classeur chargé"},
    {0}
};
static const OwlSignature sig_sauve[]     = {{2, p_sauve, "bool\xc3\xa9""en", "Vrai si le fichier a été écrit"}, {0}};
static const OwlSignature sig_ferme[]     = {{1, p_ferme, "", "Aucun retour"}, {0}};
static const OwlSignature sig_ajoute[]    = {{2, p_ajoute, "bool\xc3\xa9""en", "Vrai si la feuille a été ajoutée"}, {0}};
static const OwlSignature sig_feuille[]   = {{2, p_feuille, "bool\xc3\xa9""en", "Vrai si la feuille courante a changé"}, {0}};
static const OwlSignature sig_donnee[]    = {{3, p_donnee, "cha\xc3\xaene", "Contenu de la cellule (vide si absente)"}, {0}};
static const OwlSignature sig_fusionne[]  = {{3, p_fusionne_nom, "bool\xc3\xa9""en", "Vrai si la plage a été fusionnée"}, {0}};
static const OwlSignature sig_defusionne[]= {{2, p_defusionne, "bool\xc3\xa9""en", "Vrai si une fusion a été annulée"}, {0}};
static const OwlSignature sig_nb_ligne[]  = {{1, p_doc_only, "entier", "Dernière ligne occupée"}, {0}};
static const OwlSignature sig_nb_colonne[]= {{1, p_doc_only, "entier", "Dernière colonne occupée"}, {0}};
static const OwlSignature sig_nb_feuille[]= {{1, p_doc_only, "entier", "Nombre de feuilles"}, {0}};
static const OwlSignature sig_coord_nom[] = {{2, p_coord_vers_nom, "cha\xc3\xaene", "Nom de cellule (ex. \"A1\")"}, {0}};
static const OwlSignature sig_nom_coord[] = {{2, p_nom_vers_coord, "entier", "Numéro de ligne ou de colonne"}, {0}};

/* Formes methode sur xlsDocument : le receveur devient le 1er argument.
 * Arite = args explicites + receveur (voir signatures des natives libres). */
static const OwlMethodBinding MB_SAUVE[] = {
    { "xlsDocument", "Sauve", 2 }, { "xlsDocument", "Save", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_FERME[] = {
    { "xlsDocument", "Ferme", 1 }, { "xlsDocument", "Close", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_AJOUTE[] = {
    { "xlsDocument", "AjouteFeuille", 2 }, { "xlsDocument", "AddWorksheet", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_FEUILLE[] = {
    { "xlsDocument", "FeuilleEnCours", 2 }, { "xlsDocument", "CurrentWorksheet", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_FUSIONNE[] = {
    { "xlsDocument", "FusionneCellule", 3 }, { "xlsDocument", "MergeCell", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DEFUSIONNE[] = {
    { "xlsDocument", "DefusionneCellule", 2 }, { "xlsDocument", "UnmergeCell", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_NB_LIGNE[] = {
    { "xlsDocument", "NbLigne", 1 }, { "xlsDocument", "NbRow", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_NB_COLONNE[] = {
    { "xlsDocument", "NbColonne", 1 }, { "xlsDocument", "NbColumn", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_NB_FEUILLE[] = {
    { "xlsDocument", "NbFeuille", 1 }, { "xlsDocument", "NbWorksheet", 1 }, { NULL, NULL, 0 }
};

static const OwlNativeDef DEFS[] = {
    { .canonical="xlsOuvre", .nameEN="xlsOpen", .category="Tableur",
      .descFR="Charge un classeur Excel (.xlsx) depuis un fichier.",
      .descEN="Loads an Excel workbook (.xlsx) from a file.",
      .signatures=sig_ouvre, .impl=fn_xls_ouvre },
    { .canonical="xlsSauve", .nameEN="xlsSave", .category="Tableur",
      .descFR="Enregistre le classeur au format .xlsx sur le disque.",
      .descEN="Saves the workbook as an .xlsx file on disk.",
      .signatures=sig_sauve, .impl=fn_xls_sauve, .methodBindings=MB_SAUVE },
    { .canonical="xlsFerme", .nameEN="xlsClose", .category="Tableur",
      .descFR="Ferme le classeur et libère ses données.",
      .descEN="Closes the workbook and releases its data.",
      .signatures=sig_ferme, .impl=fn_xls_ferme, .methodBindings=MB_FERME },
    { .canonical="xlsAjouteFeuille", .nameEN="xlsAddWorksheet", .category="Tableur",
      .descFR="Ajoute une nouvelle feuille au classeur.",
      .descEN="Adds a new sheet to the workbook.",
      .signatures=sig_ajoute, .impl=fn_xls_ajoute_feuille, .methodBindings=MB_AJOUTE },
    { .canonical="xlsFeuilleEnCours", .nameEN="xlsCurrentWorksheet", .category="Tableur",
      .descFR="Sélectionne la feuille courante (les cellules suivantes la ciblent).",
      .descEN="Selects the current sheet (following cells target it).",
      .signatures=sig_feuille, .impl=fn_xls_feuille_en_cours, .methodBindings=MB_FEUILLE },
    { .canonical="xlsDonn\xc3\xa9""e", .canonicalAscii="xlsDonnee", .nameEN="xlsData", .category="Tableur",
      .descFR="Lit le contenu texte d'une cellule de la feuille courante.",
      .descEN="Reads the text content of a cell of the current sheet.",
      .signatures=sig_donnee, .impl=fn_xls_donnee },
    { .canonical="xlsFusionneCellule", .nameEN="xlsMergeCell", .category="Tableur",
      .descFR="Fusionne une plage de cellules (par noms de cellule ou par coordonnées).",
      .descEN="Merges a range of cells (by cell names or by coordinates).",
      .signatures=sig_fusionne, .impl=fn_xls_fusionne_cellule, .methodBindings=MB_FUSIONNE },
    { .canonical="xlsD\xc3\xa9""fusionneCellule", .canonicalAscii="xlsDefusionneCellule",
      .nameEN="xlsUnmergeCell", .category="Tableur",
      .descFR="Annule la fusion dont la cellule indiquée est le coin haut-gauche.",
      .descEN="Cancels the merge whose top-left corner is the given cell.",
      .signatures=sig_defusionne, .impl=fn_xls_defusionne_cellule, .methodBindings=MB_DEFUSIONNE },
    { .canonical="xlsNbLigne", .nameEN="xlsNbRow", .category="Tableur",
      .descFR="Renvoie la dernière ligne occupée de la feuille courante.",
      .descEN="Returns the last used row of the current sheet.",
      .signatures=sig_nb_ligne, .impl=fn_xls_nb_ligne, .methodBindings=MB_NB_LIGNE },
    { .canonical="xlsNbColonne", .nameEN="xlsNbColumn", .category="Tableur",
      .descFR="Renvoie la dernière colonne occupée de la feuille courante.",
      .descEN="Returns the last used column of the current sheet.",
      .signatures=sig_nb_colonne, .impl=fn_xls_nb_colonne, .methodBindings=MB_NB_COLONNE },
    { .canonical="xlsNbFeuille", .nameEN="xlsNbWorksheet", .category="Tableur",
      .descFR="Renvoie le nombre de feuilles du classeur.",
      .descEN="Returns the number of sheets in the workbook.",
      .signatures=sig_nb_feuille, .impl=fn_xls_nb_feuille, .methodBindings=MB_NB_FEUILLE },
    { .canonical="xlsCoordonn\xc3\xa9""esVersNom", .canonicalAscii="xlsCoordonneesVersNom",
      .nameEN="xlsCoordinatesToName", .category="Tableur",
      .descFR="Convertit des coordonnées (colonne, ligne) en nom de cellule (ex. \"A1\").",
      .descEN="Converts coordinates (column, row) to a cell name (e.g. \"A1\").",
      .signatures=sig_coord_nom, .impl=fn_xls_coord_vers_nom },
    { .canonical="xlsNomVersCoordonn\xc3\xa9""es", .canonicalAscii="xlsNomVersCoordonnees",
      .nameEN="xlsNameToCoordinates", .category="Tableur",
      .descFR="Renvoie le numéro de ligne ou de colonne d'une cellule à partir de son nom.",
      .descEN="Returns the row or column number of a cell from its name.",
      .signatures=sig_nom_coord, .impl=fn_xls_nom_vers_coord },
};

/*  — constantes de masque de format (extension OWL). Chaque constante
 * porte un code de format Excel natif ; `..Masque` accepte soit une de ces
 * constantes, soit une chaîne de format Excel brute (pas de conversion cachée
 * canon→Excel : divergence assumée, le brut est toujours disponible). Valeurs
 * OBJ_VAL construites au runtime (pas d'ObjString en initialiseur statique). */
typedef struct { const char* nameFR; const char* nameEN; const char* code;
                 const char* descFR; const char* descEN; } XlsFmtSpec;

static const XlsFmtSpec XLS_FORMATS[] = {
    {"xlsFormatNombre", "xlsFormatNumber", "0",
     "Format Excel : entier sans décimale (\"0\").",
     "Excel format: integer with no decimals (\"0\")."},
    {"xlsFormatNombre2D\xc3\xa9""cimales", "xlsFormatNumber2Decimals", "0.00",
     "Format Excel : nombre à deux décimales (\"0.00\").",
     "Excel format: number with two decimals (\"0.00\")."},
    {"xlsFormatMon\xc3\xa9""taire", "xlsFormatCurrency", "#,##0.00",
     "Format Excel : montant avec séparateur de milliers et deux décimales.",
     "Excel format: amount with thousands separator and two decimals."},
    {"xlsFormatPourcent", "xlsFormatPercent", "0.00%",
     "Format Excel : pourcentage à deux décimales (\"0.00%\").",
     "Excel format: percentage with two decimals (\"0.00%\")."},
    {"xlsFormatDate", "xlsFormatDate", "dd/mm/yyyy",
     "Format Excel : date jour/mois/année.",
     "Excel format: day/month/year date."},
    {"xlsFormatDateISO", "xlsFormatDateISO", "yyyy-mm-dd",
     "Format Excel : date ISO année-mois-jour.",
     "Excel format: ISO year-month-day date."},
    {"xlsFormatDateHeure", "xlsFormatDateTime", "dd/mm/yyyy hh:mm:ss",
     "Format Excel : date et heure.",
     "Excel format: date and time."},
    {"xlsFormatHeure", "xlsFormatTime", "hh:mm:ss",
     "Format Excel : heure heures:minutes:secondes.",
     "Excel format: hours:minutes:seconds time."},
    {"xlsFormatScientifique", "xlsFormatScientific", "0.00E+00",
     "Format Excel : notation scientifique (\"0.00E+00\").",
     "Excel format: scientific notation (\"0.00E+00\")."},
};

void stdlib_xls_init(void) {
    owl_registry_register_types(XLS_TYPES,
        (int)(sizeof(XLS_TYPES) / sizeof(XLS_TYPES[0])));
    owl_registry_register_enums(XLS_ENUMS,
        (int)(sizeof(XLS_ENUMS) / sizeof(XLS_ENUMS[0])));
    owl_registry_register_consts(XLS_CONSTS,
        (int)(sizeof(XLS_CONSTS) / sizeof(XLS_CONSTS[0])));

    /* Constantes de masque : valeurs chaîne construites ici. On roote chaque
     * ObjString sur la pile VM le temps que register_consts la fixe dans
     * vm.globals (root permanent), puis on dépile. */
    {
        enum { NFMT = (int)(sizeof(XLS_FORMATS) / sizeof(XLS_FORMATS[0])) };
        static OwlConstDef fmtConsts[NFMT];
        for (int i = 0; i < NFMT; i++) {
            ObjString* s = obj_copy_string(XLS_FORMATS[i].code,
                                           (int)strlen(XLS_FORMATS[i].code));
            vm_push(OBJ_VAL(s));
            fmtConsts[i].name     = XLS_FORMATS[i].nameFR;
            fmtConsts[i].category = "Tableur";
            fmtConsts[i].value    = OBJ_VAL(s);
            fmtConsts[i].descFR   = XLS_FORMATS[i].descFR;
            fmtConsts[i].descEN   = XLS_FORMATS[i].descEN;
            fmtConsts[i].nameEN   = XLS_FORMATS[i].nameEN;
        }
        owl_registry_register_consts(fmtConsts, NFMT);
        for (int i = 0; i < NFMT; i++) vm_pop();
    }

    owl_registry_register_module(DEFS,
        (int)(sizeof(DEFS) / sizeof(DEFS[0])));

    /* Factory arité 0 pour `doc est un xlsDocument` (defaultFactoryNative). */
    owl_define_native("__XlsDocumentNew", fn_xls_document_new, 0);
}
