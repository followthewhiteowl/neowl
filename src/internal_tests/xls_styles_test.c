// SPDX-License-Identifier: AGPL-3.0-only
/* xls_styles_test.c —  : preuve de la sérialisation OOXML des styles.
 *
 * Exécuté via `owl --self-test-xls-styles` (cf. main.c). Ce que le harness de
 * scénarios .owl NE PEUT PAS voir : le contenu réel du .xlsx écrit. Les tests
 * .owl valident l'API en mémoire ; ici on construit un classeur stylé, on le
 * sérialise via xlsSauve, puis on dézippe (miniz) et on parse (libxml2) le
 * xl/styles.xml et le xl/worksheets/sheet1.xml pour asserter la structure OOXML
 * effective :
 *   - dédup fonts / fills / borders / numFmts + cellXfs,
 *   - conversion couleur BGR OWL -> ARGB Excel (bon ordre R/B : rouge=FFFF0000),
 *   - formatCode du masque monétaire,
 *   - attribut s= sur la cellule stylée pointant le bon <xf>,
 *   - <cols> (largeur) et <row ht customHeight> (hauteur).
 *
 * Retourne 0 si tous les cas passent, sinon le nombre d'échecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"
#include "miniz.h"
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "fixture_unique.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* --- Navigation libxml2 minimale ------------------------------------------- */

/* Premier descendant (en profondeur d'abord) de nom `name`, ou NULL. */
static xmlNode* find_elem(xmlNode* node, const char* name) {
    for (xmlNode* n = node ? node->children : NULL; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(n->name, (const xmlChar*)name) == 0) return n;
        xmlNode* deep = find_elem(n, name);
        if (deep) return deep;
    }
    return NULL;
}

/* Nombre d'enfants directs de nom `name`. */
static int count_children(xmlNode* parent, const char* name) {
    int n = 0;
    for (xmlNode* c = parent ? parent->children : NULL; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE && xmlStrcmp(c->name, (const xmlChar*)name) == 0) n++;
    return n;
}

/* Valeur d'attribut `attr` sur `node`, dupliquée (free au caller) ou NULL. */
static char* attr_dup(xmlNode* node, const char* attr) {
    if (!node) return NULL;
    xmlChar* v = xmlGetProp(node, (const xmlChar*)attr);
    if (!v) return NULL;
    char* out = strdup((const char*)v);
    xmlFree(v);
    return out;
}

/* Extrait une entrée du zip vers un buffer heap (mz_free au caller). */
static void* extract(mz_zip_archive* zip, const char* name, size_t* len) {
    return mz_zip_reader_extract_file_to_heap(zip, name, len, 0);
}

/* Programme OWL : classeur stylé complet. Identifiants ASCII (le scanner strip
 * les accents), seules les chaînes littérales portent des octets non-ASCII. */
static const char* SRC =
    "doc est un xlsDocument\n"
    "doc[1, 1] = \"Article\"\n"
    "doc[1, 2] = 39.90\n"
    "doc..Cellule[1, 1]..CouleurFond   = RVB(255, 0, 0)\n"       /* rouge -> FFFF0000 */
    "doc..Cellule[1, 1]..Police..Nom    = \"Arial\"\n"
    "doc..Cellule[1, 1]..Police..Gras   = Vrai\n"
    "doc..Cellule[1, 1]..Police..Taille = 12\n"
    "doc..Cellule[1, 1]..Cadre..Couleur = RVB(0, 0, 255)\n"      /* bleu -> FF0000FF */
    "doc..Cellule[1, 1]..Cadre..Trait   = TraitContinu\n"
    "doc..Cellule[1, 1]..Cadre..Bords   = BordHaut + BordBas + BordGauche + BordDroite\n"
    "doc..Cellule[1, 1]..AlignementH    = chCentre\n"
    "doc..Cellule[1, 2]..Masque         = xlsFormatMonetaire\n"  /* #,##0.00 */
    "doc..Colonne[1]..Largeur           = 200\n"
    "doc..Ligne[1]..Hauteur             = 40\n";

int xls_styles_test_main(void) {
    fprintf(stderr, "\n==== XLS STYLES TESTS (feat/1795) ====\n");

    char path[160];
    fixture_unique(path, sizeof path, "_owl_xls_styles_", ".xlsx");

    /* Le save est piloté depuis le script pour rester dans le chemin réel de la
     * VM (dispatch subscript + composites). On injecte le chemin via un littéral. */
    char src[4096];
    /* Échappe les backslashes Windows du chemin pour le littéral OWL. */
    char esc[320]; int ei = 0;
    for (const char* p = path; *p && ei < (int)sizeof(esc) - 2; p++) {
        if (*p == '\\') esc[ei++] = '\\';
        esc[ei++] = *p;
    }
    esc[ei] = '\0';
    snprintf(src, sizeof(src), "%sxlsSauve(doc, \"%s\")\n", SRC, esc);

    InterpretResult res = vm_interpret(src);
    CHECK(res == INTERPRET_OK, "interpret_ok");
    if (res != INTERPRET_OK) goto done;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        g_fail++; fprintf(stderr, "  FAIL [zip_open] impossible d'ouvrir %s\n", path);
        goto done;
    }

    /* ---- styles.xml ---- */
    size_t slen = 0;
    char* sxml = (char*)extract(&zip, "xl/styles.xml", &slen);
    CHECK(sxml != NULL, "styles_present");
    if (sxml) {
        xmlDoc* sd = xmlReadMemory(sxml, (int)slen, "styles.xml", NULL,
                                   XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
        CHECK(sd != NULL, "styles_parse");
        if (sd) {
            xmlNode* root = xmlDocGetRootElement(sd);

            /* fonts : défaut (Calibri) + Arial gras 12. */
            xmlNode* fonts = find_elem(root, "fonts");
            CHECK(count_children(fonts, "font") == 2, "fonts_count_2");
            bool arialBold12 = false;
            for (xmlNode* f = fonts ? fonts->children : NULL; f; f = f->next) {
                if (f->type != XML_ELEMENT_NODE) continue;
                xmlNode* nm = find_elem(f, "name");
                char* nmv = attr_dup(nm, "val");
                xmlNode* sz = find_elem(f, "sz");
                char* szv = attr_dup(sz, "val");
                bool bold = find_elem(f, "b") != NULL;
                if (nmv && strcmp(nmv, "Arial") == 0 && szv && strcmp(szv, "12") == 0 && bold)
                    arialBold12 = true;
                free(nmv); free(szv);
            }
            CHECK(arialBold12, "font_arial_bold_12");

            /* fills : none + gray125 + rouge solide. Ordre RGB : rouge = FFFF0000. */
            xmlNode* fills = find_elem(root, "fills");
            CHECK(count_children(fills, "fill") == 3, "fills_count_3");
            bool redFill = false;
            for (xmlNode* fl = fills ? fills->children : NULL; fl; fl = fl->next) {
                if (fl->type != XML_ELEMENT_NODE) continue;
                xmlNode* fg = find_elem(fl, "fgColor");
                char* rgb = attr_dup(fg, "rgb");
                if (rgb && strcmp(rgb, "FFFF0000") == 0) redFill = true;
                free(rgb);
            }
            CHECK(redFill, "fill_argb_red_order");   /* preuve du bon ordre R/B */

            /* borders : vide + cadre 4 côtés. */
            xmlNode* borders = find_elem(root, "borders");
            CHECK(count_children(borders, "border") == 2, "borders_count_2");
            /* Le 2e border porte les 4 côtés stylés + couleur bleue FF0000FF. */
            bool border4sides = false, borderBlue = false;
            for (xmlNode* bd = borders ? borders->children : NULL; bd; bd = bd->next) {
                if (bd->type != XML_ELEMENT_NODE) continue;
                xmlNode* left = find_elem(bd, "left");
                char* ls = attr_dup(left, "style");
                if (ls) {   /* un côté stylé => c'est le border custom */
                    int styled = 0;
                    const char* sides[4] = {"left", "right", "top", "bottom"};
                    for (int k = 0; k < 4; k++) {
                        xmlNode* sd2 = find_elem(bd, sides[k]);
                        char* st = attr_dup(sd2, "style");
                        if (st) styled++;
                        free(st);
                        xmlNode* col = sd2 ? find_elem(sd2, "color") : NULL;
                        char* cv = attr_dup(col, "rgb");
                        if (cv && strcmp(cv, "FF0000FF") == 0) borderBlue = true;
                        free(cv);
                    }
                    if (styled == 4) border4sides = true;
                }
                free(ls);
            }
            CHECK(border4sides, "border_4_sides_styled");
            CHECK(borderBlue, "border_argb_blue_order");

            /* numFmts : le masque monétaire. */
            xmlNode* numFmts = find_elem(root, "numFmts");
            CHECK(numFmts != NULL, "numfmts_present");
            char* fmtCode = NULL; int fmtId = -1;
            for (xmlNode* nf = numFmts ? numFmts->children : NULL; nf; nf = nf->next) {
                if (nf->type != XML_ELEMENT_NODE) continue;
                char* code = attr_dup(nf, "formatCode");
                char* id = attr_dup(nf, "numFmtId");
                if (code && strcmp(code, "#,##0.00") == 0) { fmtCode = code; if (id) fmtId = atoi(id); }
                else free(code);
                free(id);
            }
            CHECK(fmtCode != NULL, "numfmt_currency_code");
            CHECK(fmtId >= 164, "numfmt_id_from_164");
            free(fmtCode);

            /* cellXfs : au moins l'xf par défaut + un xf combiné pour A1. */
            xmlNode* cellXfs = find_elem(root, "cellXfs");
            int xfCount = count_children(cellXfs, "xf");
            CHECK(xfCount >= 2, "cellxfs_at_least_2");
            /* L'xf de A1 combine font/fill/border != 0 + alignment center. */
            bool xfA1 = false;
            for (xmlNode* xf = cellXfs ? cellXfs->children : NULL; xf; xf = xf->next) {
                if (xf->type != XML_ELEMENT_NODE) continue;
                char* fo = attr_dup(xf, "fontId");
                char* fi = attr_dup(xf, "fillId");
                char* bo = attr_dup(xf, "borderId");
                xmlNode* al = find_elem(xf, "alignment");
                char* h = attr_dup(al, "horizontal");
                if (fo && atoi(fo) > 0 && fi && atoi(fi) >= 2 && bo && atoi(bo) >= 1 &&
                    h && strcmp(h, "center") == 0)
                    xfA1 = true;
                free(fo); free(fi); free(bo); free(h);
            }
            CHECK(xfA1, "xf_a1_combined");

            xmlFreeDoc(sd);
        }
        mz_free(sxml);
    }

    /* ---- sheet1.xml ---- */
    size_t wlen = 0;
    char* wxml = (char*)extract(&zip, "xl/worksheets/sheet1.xml", &wlen);
    CHECK(wxml != NULL, "sheet_present");
    if (wxml) {
        xmlDoc* wd = xmlReadMemory(wxml, (int)wlen, "sheet1.xml", NULL,
                                   XML_PARSE_NOWARNING | XML_PARSE_NOERROR);
        CHECK(wd != NULL, "sheet_parse");
        if (wd) {
            xmlNode* root = xmlDocGetRootElement(wd);

            /* <cols> : colonne 1 avec largeur > 0. */
            xmlNode* cols = find_elem(root, "cols");
            CHECK(cols != NULL, "cols_present");
            xmlNode* col = cols ? find_elem(cols, "col") : NULL;
            char* cmin = attr_dup(col, "min");
            char* cw = attr_dup(col, "width");
            CHECK(cmin && strcmp(cmin, "1") == 0, "col_min_1");
            CHECK(cw && atof(cw) > 20.0 && atof(cw) < 35.0, "col_width_px_to_chars");  /* (200-5)/7≈27.86 */
            free(cmin); free(cw);

            /* <row r="1"> avec ht + customHeight. */
            bool rowHeight = false;
            xmlNode* sheetData = find_elem(root, "sheetData");
            for (xmlNode* row = sheetData ? sheetData->children : NULL; row; row = row->next) {
                if (row->type != XML_ELEMENT_NODE) continue;
                if (xmlStrcmp(row->name, (const xmlChar*)"row") != 0) continue;
                char* rr = attr_dup(row, "r");
                char* ht = attr_dup(row, "ht");
                char* ch = attr_dup(row, "customHeight");
                if (rr && strcmp(rr, "1") == 0 && ht && atof(ht) > 25.0 && atof(ht) < 35.0 &&
                    ch && strcmp(ch, "1") == 0)
                    rowHeight = true;   /* 40 px -> 30 pt */
                free(rr); free(ht); free(ch);
            }
            CHECK(rowHeight, "row_height_px_to_pt");

            /* Cellule A1 : attribut s= présent et cohérent (>=1, dans cellXfs). */
            bool a1Styled = false;
            for (xmlNode* row = sheetData ? sheetData->children : NULL; row; row = row->next) {
                if (row->type != XML_ELEMENT_NODE) continue;
                for (xmlNode* c = row->children; c; c = c->next) {
                    if (c->type != XML_ELEMENT_NODE) continue;
                    if (xmlStrcmp(c->name, (const xmlChar*)"c") != 0) continue;
                    char* ref = attr_dup(c, "r");
                    char* s = attr_dup(c, "s");
                    if (ref && strcmp(ref, "A1") == 0 && s && atoi(s) >= 1) a1Styled = true;
                    free(ref); free(s);
                }
            }
            CHECK(a1Styled, "cell_a1_has_s_attr");

            xmlFreeDoc(wd);
        }
        mz_free(wxml);
    }

    mz_zip_reader_end(&zip);
    remove(path);

done:
    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "xls-styles: CORPUS VIDE — 0 cas execute.\n");
        return 1;
    }
    return g_fail;
}
