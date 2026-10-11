// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_icones.c — Type Icone, enum Forme, namespace icones.<nom>.
 *
 * Resolveur catalogue branche sur le vrai catalogue Iconify embarque
 * (47728 entries, blob zstd ~10.9 MB compresse / ~46.7 MB decompresse)
 * genere au build par tools/icons_catalog_gen et expose via
 * icons_catalog.h. Le placeholder minimal (7 stubs hardcoded) a ete
 * supprime au Lot 2a-ii.
 *
 * Mapping noms DSL OWL <-> Iconify :
 *   Iconify utilise du kebab-case (`file-type-pdf2`, `account-circle`).
 *   Le scanner OWL ne tolere pas `-` dans les identifiants (operateur).
 *   Le lookup normalise donc `_` -> `-` cote OWL avant bsearch :
 *   `icones.file_type_pdf2` resout `file-type-pdf2`. Combine avec les
 *   invariants 1 (case-insensible) et 2 (accent-insensible), un seul
 *   nom canon Iconify est atteignable depuis plusieurs orthographes OWL.
 *
 * Decompression lazy :
 *   Le blob zstd est decompresse au premier acces dans un buffer global
 *   statique (~46.7 MB RAM permanent). En mode CLI pur jamais appelle
 *   icones.<nom>, la RAM reste libre. Premiere resolution paye le cout
 *   (~50-100 ms typique).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_errors.h"
#include "sentinels.h"
#include "object.h"
#include "vm.h"
#include "scanner.h"
#include "icons_catalog.h"
#include "zstd.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hook rasterizer Qt6Svg (defini par stdlib_ui.c dans owl_runtime_gui).
 * stdlib_icones.c vit dans owl_core (build commun owl-cli/owl.exe), donc
 * ne peut PAS lier directement Qt6Svg. Le mode GUI s'enregistre au boot
 * via owl_icone_register_rasterizer (declaree dans owl_stdlib.h) ;
 * en mode CLI le hook reste NULL et IconeVersImage retourne une Image
 * invalide. */
static OwlIconeRasterizeFn g_icone_rasterize_fn = NULL;

void owl_icone_register_rasterizer(OwlIconeRasterizeFn fn) {
    g_icone_rasterize_fn = fn;
}

/* Forward decl : helper de construction d'ObjInstance Image depuis un
 * OwlImage* preexistant. Defini dans stdlib_image.c (static la-bas, on
 * declare une variante exporte ici). Pour eviter une dependance circulaire
 * de symboles entre stdlib_image.c et stdlib_icones.c, on construit
 * l'instance Image a la main dans IconeVersImage (cf. fn_IconeVersImage
 * plus bas) en posant le pointeur OwlImage* dans _ptr et en synchronisant
 * Valide/Largeur/Hauteur. */

/* ================================================================
 * Enum Forme — forme de cadrage d'une icone
 * ================================================================ */

static const char* const E_Forme_values[] = {
    "formeAucune",
    "formeCarree",
    "formeCercle",
    NULL
};

const OwlEnum E_Forme = {
    .name       = "forme",
    .nameAscii  = "forme",
    .nameEN     = "shape",
    .category   = "Icone",
    .descFR     = "Cadrage applique au rendu d'une icone : aucun (rendu brut), inscrit dans un carre, ou inscrit dans un cercle.",
    .descEN     = "Framing applied to an icon's rendering: none (raw render), inscribed in a square, or inscribed in a circle.",
    .values     = E_Forme_values,
    .combinable = false,
};

/* ================================================================
 * Constantes forme*
 * ================================================================ */

#define C_FORME(nm, val, fr, en) { nm, "Icone", INTEGER_VAL(val), fr, en }
/* Variante avec nameEN explicite (invariant bilingue n°3). */
#define C_FORME_EN(nm, val, fr, en, ennm) { nm, "Icone", INTEGER_VAL(val), fr, en, ennm }

static const OwlConstDef FORME_CONSTS[] = {
    C_FORME_EN("formeAucune", OWL_FORME_AUCUNE,
        "rendu brut de l'icone sans cadre.",
        "raw icon render without framing.", "formeNone"),
    C_FORME_EN("formeCarree", OWL_FORME_CARREE,
        "icone inscrite dans un cadre carre.",
        "icon inscribed in a square frame.", "formeSquare"),
    C_FORME_EN("formeCercle", OWL_FORME_CERCLE,
        "icone inscrite dans un cadre circulaire.",
        "icon inscribed in a circular frame.", "formeCircle"),
};

#undef C_FORME

/* ================================================================
 * FIELDS_Icone — declaratif (LSP / hover / self-test). Le runtime ne
 * consulte PAS ces fields : ObjIcone porte des slots typees directs,
 * et OP_STRUCT_GET/SET dispatche sur le type Obj (cf. vm.c branche
 * IS_ICONE). Les fields servent a :
 *   - declarer les noms canon FR/EN bilingues (invariant 3)
 *   - referencer E_Forme via allowedBase (rule 18 : enum referencee)
 *   - alimenter `owl explain Icone` et le LSP autocomplete
 *
 * Read-only en pratique : ..Valide et ..CouleurModifiable refusent
 * l'ecriture cote vm.c (Lot 2e-1).
 * ================================================================ */
static const OwlField FIELDS_Icone[] = {
    {"Couleur", NULL, "Couleur", "0",
     "Couleur effective au rendu (RGB packe BGR). Effective uniquement si CouleurModifiable vaut Vrai.",
     "Effective render color (BGR-packed RGB). Effective only when ColorEditable is True.",
     .nameEN="Color"},
    {"Rotation", NULL, "entier", "0",
     "Rotation en degres appliquee au rendu (0..359). Valeurs canon 0/90/180/270 mais valeurs libres acceptees.",
     "Rotation in degrees applied to the render (0..359). Canonical values 0/90/180/270 but free values accepted.",
     .nameEN="Rotation"},
    {"Forme", NULL, "Forme", "formeAucune",
     "Cadrage applique : aucune, carre, ou cercle.",
     "Applied framing: none, square, or circle.",
     .allowedBase=&E_Forme, .nameEN="Shape"},
    {"CouleurFond", NULL, "Couleur", "Transparent",
     "Couleur de fond derriere l'icone. Visible uniquement si Forme est formeCarree ou formeCercle (sans Forme la zone derriere l'icone reste transparente). Defaut Transparent.",
     "Background color behind the icon. Visible only when Forme is formeCarree or formeCercle (without Forme the area behind the icon stays transparent). Default Transparent.",
     .nameEN="BackgroundColor"},
    {"Valide", NULL, "booleen", "Vrai",
     "Vrai si l'icone est valide (catalogue resolu ou SVG charge sans erreur). Lecture seule.",
     "True if the icon is valid (resolved catalog entry or successfully loaded SVG). Read-only.",
     .nameEN="Valid"},
    {"CouleurModifiable", "CouleurModifiable", "booleen", "Faux",
     "Vrai si la propriete Couleur est effective au rendu. Derive de la source SVG : Vrai pour les icones mono-color, Faux pour les multi-color. Lecture seule.",
     "True when Couleur is effective on render. Derived from the SVG source: true for mono-color icons, false for multi-color. Read-only.",
     .nameEN="ColorEditable"},
    {"Icone", NULL, "chaine", "\"\"",
     "Source SVG brute de l'icone. SET accepte une entree catalogue, un chemin de fichier SVG, un fragment SVG inline, ou une autre Icone. GET retourne le markup SVG. Propriete par defaut.",
     "Raw SVG source of the icon. SET accepts a catalog entry, an SVG file path, an inline SVG fragment, or another Icone. GET returns the SVG markup. Default property.",
     .nameEN="Icon"},
    {"Type", NULL, "chaine", "\"Icone\"",
     "Nom canon du type runtime (vaut toujours \"Icone\"). Lecture seule.",
     "Canonical runtime type name (always \"Icone\"). Read-only.",
     .nameEN="Type"},
    {0},
};

/* ================================================================
 * Type Icone
 * ================================================================ */

static const char* const ALIAS_Icone[] = { "Icon", NULL };

static const OwlTypeDef ICONE_TYPES[] = {
    {
        /*  — Canon FR aligne sur canon : Icône (avec accent).
         * Fallback ASCII Icone reste expose pour les contextes qui
         * ne gerent pas l'UTF-8. Grace a l'accent-insensible runtime
         * (regle 6 invariant 2), le code OWL existant `est un Icone`
         * continue de marcher ; le formatter restaure `Icône` au save. */
        .canonical      = "Icône",
        .canonicalAscii = "Icone",
        .aliases        = ALIAS_Icone,
        .nameEN         = "Icon",
        .category       = "Icone",
        .descFR         = "Type representant une icone vectorielle. Une valeur Icone provient soit du catalogue embarque (espace de noms icones.<nom>), soit d'un fichier SVG sur disque, soit d'un fragment SVG inline. Les proprietes Couleur, Rotation et Forme permettent de personnaliser le rendu sans dupliquer la source.",
        .descEN         = "Type representing a vector icon. An Icone value comes either from the embedded catalog (icones.<name> namespace), from an SVG file on disk, or from an inline SVG fragment. The Color, Rotation and Shape properties customize the rendering without duplicating the source.",
        .fields         = FIELDS_Icone,
        .coercible      = true,
        .hasDefault     = true,
        .defaultValue   = NIL_VAL,
        .defaultProperty = "Icone",
        .genre          = OWL_GENRE_F,   /* une icône */
    },
};

/* ================================================================
 * Heuristique CouleurModifiable
 *
 * Lot 2e-1, version simple : retourne true si le SVG contient au moins
 * une occurrence de "currentColor" ET ne contient aucune valeur de
 * couleur hardcodee dans fill="" / stroke="" / fill='' / stroke=''
 * autre que "currentColor", "none", "transparent".
 *
 * Faux negatifs acceptes (SVG qui mixe currentColor + 1 fill hex sera
 * classe non modifiable). Le vrai parser arrive en Lot 2e-2 quand on
 * cable Qt6Svg pour le rendu reel. Pour le placeholder catalogue
 * minimal cette heuristique suffit (les 6 mono-color utilisent
 * exclusivement currentColor + le PDF utilise des hex).
 * ================================================================ */
static bool svg_seems_color_editable(const char* svg, int len) {
    if (svg == NULL || len <= 0) return false;
    /* Recherche "currentColor" (case-sensitive : SVG standard) */
    bool has_current = false;
    for (int i = 0; i + 12 <= len; i++) {
        if (memcmp(svg + i, "currentColor", 12) == 0) {
            has_current = true;
            break;
        }
    }
    if (!has_current) return false;
    /* Scan : si un fill= ou stroke= contient une valeur autre que
     * currentColor / none / transparent / "" -> multi-color. */
    const char* attrs[] = {"fill=", "stroke="};
    for (int a = 0; a < 2; a++) {
        const char* needle = attrs[a];
        int nlen = (int)strlen(needle);
        for (int i = 0; i + nlen < len; i++) {
            if (memcmp(svg + i, needle, (size_t)nlen) != 0) continue;
            int p = i + nlen;
            if (p >= len) break;
            char quote = svg[p];
            if (quote != '"' && quote != '\'') continue;
            p++;
            int start = p;
            while (p < len && svg[p] != quote) p++;
            int vlen = p - start;
            if (vlen <= 0) continue;
            /* Compare strict aux 3 valeurs tolerees */
            if (vlen == 12 && memcmp(svg + start, "currentColor", 12) == 0) continue;
            if (vlen == 4  && memcmp(svg + start, "none", 4) == 0) continue;
            if (vlen == 11 && memcmp(svg + start, "transparent", 11) == 0) continue;
            /* Tout le reste = couleur explicite -> multi-color */
            return false;
        }
    }
    return true;
}

/* ================================================================
 * Normalisation nom DSL OWL -> nom Iconify
 *
 * Etapes :
 *   1. strip accents (invariant 2)
 *   2. lower-case ASCII (invariant 1)
 *   3. `_` -> `-` (pont DSL OWL ne tolerant pas `-` vs Iconify kebab-case)
 *
 * Ecrit dans `out` (cap inclut '\0'). Retourne la longueur ecrite
 * (sans NUL), ou -1 si le buffer est insuffisant.
 * ================================================================ */
static int icones_normalize_name(const char* name, int nlen,
                                 char* out, size_t cap) {
    if (cap < 2) return -1;
    char stripped[128];
    int slen = scanner_strip_accents(name, nlen, stripped, sizeof(stripped));
    if (slen <= 0 || (size_t)slen >= cap) return -1;
    for (int i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)stripped[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == '_') c = '-';
        out[i] = (char)c;
    }
    out[slen] = '\0';
    return slen;
}

/* ================================================================
 * Catalogue decompresse (lazy)
 *
 * Le blob zstd embarque (OWL_ICONS_BLOB_ZSTD) est decompresse une seule
 * fois, au premier appel, dans un buffer global statique. Cout one-shot
 * (~50-100 ms) + RAM permanent ~46.7 MB. Le buffer n'est jamais libere :
 * une fois alloue, sa duree de vie est celle du process.
 *
 * En cas d'echec de decompression (zstd corrompu, OOM), `g_icons_blob_raw`
 * reste NULL et tous les lookups suivants degradent vers Icone invalide.
 * ================================================================ */
static unsigned char* g_icons_blob_raw = NULL;
static bool           g_icons_blob_init_done = false;
static bool           g_icons_blob_ok = false;

static bool icons_blob_ensure(void) {
    if (g_icons_blob_init_done) return g_icons_blob_ok;
    g_icons_blob_init_done = true;
    unsigned char* buf = (unsigned char*)malloc(OWL_ICONS_BLOB_RAW_SIZE);
    if (buf == NULL) return false;
    size_t got = ZSTD_decompress(buf, OWL_ICONS_BLOB_RAW_SIZE,
                                 OWL_ICONS_BLOB_ZSTD,
                                 OWL_ICONS_BLOB_ZSTD_SIZE);
    if (ZSTD_isError(got) || got != OWL_ICONS_BLOB_RAW_SIZE) {
        free(buf);
        return false;
    }
    g_icons_blob_raw = buf;
    g_icons_blob_ok  = true;
    return true;
}

/*  — Accesseur public au blob decompresse pour les consommateurs
 * qui iterent OWL_ICONS_CATALOG eux-memes (ex: cli_gen_assets.c qui
 * dump les SVG raw pour le plugin VSCode). Retourne NULL si la
 * decompression a echoue. */
const unsigned char* owl_icones_blob_raw_ensure(void) {
    if (!icons_blob_ensure()) return NULL;
    return g_icons_blob_raw;
}

/* ================================================================
 * Index secondaire `name ASC` pour lookup binaire
 *
 * Le tableau natif OWL_ICONS_CATALOG est trie par (set_id ASC, name ASC)
 * — choix du generateur tools/icons_catalog_gen pour grouper les entries
 * par set. Pour un lookup `icones.<nom>` indifferent au set, on construit
 * a la premiere recherche un index secondaire trie par (name ASC, set_id ASC) :
 * `g_icons_name_index[k]` est un indice dans OWL_ICONS_CATALOG. Memoire
 * one-shot ~191 KB (47728 * uint32_t), construction O(N log N).
 *
 * En cas d'allocation KO, le lookup degrade vers scan lineaire O(N).
 * ================================================================ */
static uint32_t* g_icons_name_index = NULL;
static bool      g_icons_name_index_done = false;

static int icons_name_index_qsort_cmp(const void* a, const void* b) {
    uint32_t ia = *(const uint32_t*)a;
    uint32_t ib = *(const uint32_t*)b;
    const OwlIconCatalogEntry* ea = &OWL_ICONS_CATALOG[ia];
    const OwlIconCatalogEntry* eb = &OWL_ICONS_CATALOG[ib];
    int c = strcmp(ea->name, eb->name);
    if (c != 0) return c;
    return (int)ea->set_id - (int)eb->set_id;
}

static bool icons_name_index_ensure(void) {
    if (g_icons_name_index_done) return g_icons_name_index != NULL;
    g_icons_name_index_done = true;
    size_t n = OWL_ICONS_CATALOG_COUNT;
    uint32_t* idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (idx == NULL) return false;
    for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;
    qsort(idx, n, sizeof(uint32_t), icons_name_index_qsort_cmp);
    g_icons_name_index = idx;
    return true;
}

/* ================================================================
 * Catalogue lookup case+accent-insensible (binary search sur l'index name)
 *
 * Plusieurs entries peuvent partager un nom (ex `save` present dans 3 sets).
 * Apres binary search, on backtrack vers la premiere entry du meme nom
 * pour respecter la priorite set_id (0 = material-symbols, prioritaire).
 *
 * Si l'index n'a pas pu etre alloue, fallback scan lineaire (O(N), 47k
 * iterations, quelques ms — acceptable comme degrade).
 * ================================================================ */
const OwlIconCatalogEntry* owl_icones_catalog_find(const char* name) {
    if (name == NULL) return NULL;
    char norm[128];
    if (icones_normalize_name(name, (int)strlen(name), norm, sizeof(norm)) < 0) {
        return NULL;
    }

    if (icons_name_index_ensure()) {
        size_t lo = 0, hi = OWL_ICONS_CATALOG_COUNT;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            const OwlIconCatalogEntry* e = &OWL_ICONS_CATALOG[g_icons_name_index[mid]];
            int c = strcmp(norm, e->name);
            if (c == 0) {
                /* Backtrack : index trie (name, set_id), donc la premiere
                 * occurrence du nom est la plus prioritaire. */
                while (mid > 0) {
                    const OwlIconCatalogEntry* pe =
                        &OWL_ICONS_CATALOG[g_icons_name_index[mid - 1]];
                    if (strcmp(pe->name, norm) != 0) break;
                    mid--;
                }
                return &OWL_ICONS_CATALOG[g_icons_name_index[mid]];
            }
            if (c < 0) hi = mid;
            else       lo = mid + 1;
        }
        return NULL;
    }

    /* Fallback scan lineaire si OOM sur l'index. Conserve la priorite
     * set_id le plus bas (le tableau natif est ordonne par set_id ASC,
     * donc le premier match croise est forcement le prioritaire). */
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        if (strcmp(OWL_ICONS_CATALOG[i].name, norm) == 0) {
            return &OWL_ICONS_CATALOG[i];
        }
    }
    return NULL;
}

/* ================================================================
 * did-you-mean Levenshtein sur 47728 entries
 *
 * Budget distance <= 3. Early-out par difference de longueur (si l'ecart
 * |slen - tlen| > budget, distance impossible). Sur 47k entries le scan
 * lineaire prend quelques ms grace au filtre longueur (la plupart des
 * noms ont 5-15 chars, la fenetre eligible est tres etroite).
 * ================================================================ */
static int min3(int a, int b, int c) {
    if (a < b) return (a < c) ? a : c;
    return (b < c) ? b : c;
}

static int levenshtein_bounded(const char* s, int slen,
                               const char* t, int tlen, int budget) {
    if (slen == 0) return tlen;
    if (tlen == 0) return slen;
    enum { CAP = 64 };
    if (slen > CAP || tlen > CAP) return budget + 1;
    int prev[CAP + 1], curr[CAP + 1];
    for (int j = 0; j <= tlen; j++) prev[j] = j;
    for (int i = 1; i <= slen; i++) {
        curr[0] = i;
        int row_min = curr[0];
        for (int j = 1; j <= tlen; j++) {
            int cost = (tolower((unsigned char)s[i-1])
                        == tolower((unsigned char)t[j-1])) ? 0 : 1;
            curr[j] = min3(prev[j] + 1, curr[j-1] + 1, prev[j-1] + cost);
            if (curr[j] < row_min) row_min = curr[j];
        }
        /* Early-out : si toute la ligne depasse le budget, distance
         * finale > budget garantie. */
        if (row_min > budget) return budget + 1;
        for (int j = 0; j <= tlen; j++) prev[j] = curr[j];
    }
    return prev[tlen];
}

const char* owl_icones_namespace_did_you_mean(const char* name) {
    if (name == NULL) return NULL;
    char norm[128];
    int nlen = icones_normalize_name(name, (int)strlen(name), norm, sizeof(norm));
    if (nlen < 0) return NULL;
    const char* best = NULL;
    int best_d = 4; /* seuil : distance <= 3 pour suggerer */
    for (size_t i = 0; i < OWL_ICONS_CATALOG_COUNT; i++) {
        const char* cn = OWL_ICONS_CATALOG[i].name;
        int clen = (int)strlen(cn);
        /* Filtre longueur (early-out) : la distance d'edition est
         * au moins egale a |clen - nlen|. */
        int dlen = clen > nlen ? clen - nlen : nlen - clen;
        if (dlen >= best_d) continue;
        int d = levenshtein_bounded(norm, nlen, cn, clen, best_d - 1);
        if (d < best_d) {
            best_d = d;
            best = cn;
            if (best_d == 1) break; /* presque pas mieux possible */
        }
    }
    return best;
}

/* ================================================================
 * Materialise un ObjIcone catalogue depuis une entree du vrai catalogue.
 *
 * Le body Iconify est un fragment SVG sans enveloppe (cf. doc
 * icons_catalog.h) : on l'enveloppe a la volee dans
 *   <svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'>...</svg>
 * Le viewBox `0 0 24 24` est la grille standard Iconify ; un affinage
 * par set (32x32 pour fluent-emoji, etc.) reste a faire si necessaire.
 *
 * catalog_hash = entry->svg_hash (XXH64 content-addressable, cf. note
 * Lot 2a-i sur XXH3 desactive par xxhash patche dans zstd vendor).
 * ================================================================ */
static Value icones_materialize_catalog(const OwlIconCatalogEntry* entry) {
    if (!icons_blob_ensure()) {
        ObjIcone* ic = obj_new_icone();
        ic->valide = false;
        return OBJ_VAL(ic);
    }
    const char* body = (const char*)(g_icons_blob_raw + entry->offset);
    int body_len = (int)entry->length;

    /* b — Composition svg_raw avec viewBox NATIF.
     * Priorite : (1) width/height per-entry si > 0 (logos, devicon ont
     * des dimensions variables par icone), (2) default per-set via
     * OWL_ICONS_SET_VIEWBOX, (3) fallback 24x24. */
    int vw = (entry->width  > 0) ? entry->width  : OWL_ICONS_SET_VIEWBOX[entry->set_id];
    int vh = (entry->height > 0) ? entry->height : OWL_ICONS_SET_VIEWBOX[entry->set_id];
    if (vw <= 0) vw = 24;
    if (vh <= 0) vh = 24;
    char hdr_buf[96];
    int hdr_len = snprintf(hdr_buf, sizeof(hdr_buf),
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 %d %d'>",
        vw, vh);
    const char* hdr = hdr_buf;
    static const char ftr[] = "</svg>";
    int ftr_len = (int)(sizeof(ftr) - 1);
    int total = hdr_len + body_len + ftr_len;

    char stack_buf[8192];
    char* svg = (total + 1 <= (int)sizeof(stack_buf))
                ? stack_buf
                : (char*)malloc((size_t)(total + 1));
    if (svg == NULL) {
        ObjIcone* ic = obj_new_icone();
        ic->valide = false;
        return OBJ_VAL(ic);
    }
    memcpy(svg, hdr, (size_t)hdr_len);
    memcpy(svg + hdr_len, body, (size_t)body_len);
    memcpy(svg + hdr_len + body_len, ftr, (size_t)ftr_len);
    svg[total] = '\0';

    ObjIcone* icn = obj_new_icone();
    vm_push(OBJ_VAL(icn));
    icn->catalog_hash       = entry->svg_hash;
    icn->svg_raw            = obj_copy_string(svg, total);
    icn->couleur_modifiable = entry->couleur_modifiable;
    icn->valide             = true;
    vm_pop();

    if (svg != stack_buf) free(svg);
    return OBJ_VAL(icn);
}

/*  — Wrapper public : materialise un ObjIcone depuis une
 * entry deja choisie (pas de lookup case-insensible, pas de chute sur
 * la priorite catalogue). Utilise par cli_doc.c preview colored-first
 * pour forcer la version visuelle plus representative quand l'icone
 * existe dans plusieurs sets. Signature void* pour eviter cycle
 * d'include cote header (cf. owl_stdlib.h). */
Value owl_icones_materialize_entry(const void* entry_ptr) {
    const OwlIconCatalogEntry* entry = (const OwlIconCatalogEntry*)entry_ptr;
    if (entry == NULL) {
        ObjIcone* ic = obj_new_icone();
        ic->valide = false;
        return OBJ_VAL(ic);
    }
    return icones_materialize_catalog(entry);
}

/* ================================================================
 * Resolution `icones.<nom>` runtime
 *
 * Appele depuis vm.c (OP_STRUCT_GET sur ObjIconesNamespace). Retourne
 * un Value valide quoi qu'il arrive :
 *   - hit : ObjIcone catalogue avec props par defaut
 *   - miss : emet une erreur native E_NATIVE_ARG_TYPE_MISMATCH avec
 *     did-you-mean et renvoie un ObjIcone invalide pour preserver
 *     l'invariant pile.
 * ================================================================ */
Value owl_icones_namespace_lookup(const char* name) {
    if (name == NULL) {
        ObjIcone* ic = obj_new_icone();
        ic->valide = false;
        return OBJ_VAL(ic);
    }
    const OwlIconCatalogEntry* entry = owl_icones_catalog_find(name);
    if (entry == NULL) {
        const char* suggestion = owl_icones_namespace_did_you_mean(name);
        if (suggestion != NULL) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "icones",
                                  "native", "icones", "arg", "nom",
                                  "expected", suggestion,
                                  "actual", name, NULL);
        } else {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "icones",
                                  "native", "icones", "arg", "nom",
                                  "expected", "nom du catalogue",
                                  "actual", name, NULL);
        }
        ObjIcone* ic = obj_new_icone();
        ic->valide = false;
        return OBJ_VAL(ic);
    }
    return icones_materialize_catalog(entry);
}

/* ================================================================
 * parse_to_icone — coerce (Lot 2e-1 version "vraie")
 *
 * Distingue :
 *   - ObjIcone  -> clone (copie shallow des slots, partage svg_raw).
 *                  Permet `ICN_Backup est une Icone = ICN_Source` avec
 *                  preservation des customisations (Cas E).
 *   - NIL       -> ObjIcone vierge.
 *   - chaine "<...>" -> SVG inline. Heuristique CouleurModifiable
 *                       appliquee sur le contenu.
 *   - chaine "<truc>.svg" ou path -> tentative de lecture disque.
 *                                    Si fopen echoue : valide = false.
 *                                    Sinon : svg_raw = contenu fichier.
 *   - autre     -> erreur runtime.
 *
 * Le path est detecte par : ne commence pas par '<' ET (.svg en
 * suffixe OU contient '/' / '\\').
 * ================================================================ */
static Value parse_to_icone(Value v) {
    if (IS_ICONE(v)) {
        /* Clone : preserve catalog_hash + svg_raw + props customisees.
         *  — root v (source) pendant obj_new_icone() : alloc
         * peut declencher GC ; v est passe par parametre et n'est plus
         * sur la stack VM cote caller (cf. vm.c OP_STRUCT_SET branche
         * ..Icone = qui vm_pop la valeur RHS avant). Sans push, src
         * peut etre collecte avant qu'on lise ses champs. */
        vm_push(v);
        ObjIcone* src = AS_ICONE(v);
        ObjIcone* dst = obj_new_icone();
        dst->catalog_hash       = src->catalog_hash;
        dst->svg_raw            = src->svg_raw;
        dst->couleur            = src->couleur;
        dst->couleur_fond       = src->couleur_fond;
        dst->rotation           = src->rotation;
        dst->forme              = src->forme;
        dst->couleur_modifiable = src->couleur_modifiable;
        dst->valide             = src->valide;
        vm_pop();
        return OBJ_VAL(dst);
    }
    if (IS_NIL(v)) {
        ObjIcone* ic = obj_new_icone();
        return OBJ_VAL(ic);
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        const char* str = s->chars;
        int len = s->length;
        /* Heuristique de classification :
         *   - commence par '<' : SVG inline.
         *   - sinon : path disque -> tentative lecture. */
        bool is_inline = (len > 0 && str[0] == '<');
        ObjIcone* ic = obj_new_icone();
        vm_push(OBJ_VAL(ic)); /* root */
        if (is_inline) {
            /* Heuristique de validite : doit contenir "<svg" et une
             * fermeture "</svg>" OU se fermer correctement (au moins
             * un '>'). Test grossier mais suffit pour distinguer Cas D
             * (valide) de Cas malformed. */
            bool has_open = false;
            bool has_close_marker = false;
            for (int i = 0; i + 4 <= len; i++) {
                if (memcmp(str + i, "<svg", 4) == 0) { has_open = true; break; }
            }
            if (has_open) {
                /* Cherche </svg> OU se contente d'un '/>' final pour
                 * fermeture auto. Cas Bad "<svg><circle..." sans '>'
                 * final ni </svg> -> invalide. */
                for (int i = 0; i + 6 <= len; i++) {
                    if (memcmp(str + i, "</svg>", 6) == 0) {
                        has_close_marker = true;
                        break;
                    }
                }
                if (!has_close_marker
                    && len >= 2
                    && str[len-1] == '>'
                    && str[len-2] == '/') {
                    has_close_marker = true;
                }
            }
            ic->svg_raw            = s;
            ic->valide             = has_open && has_close_marker;
            ic->couleur_modifiable = svg_seems_color_editable(str, len);
        } else {
            /* Path disque. Tentative de lecture. */
            FILE* f = fopen(str, "rb");
            if (f == NULL) {
                ic->svg_raw = s;
                ic->valide  = false;
            } else {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz < 0 || sz > 4 * 1024 * 1024) {
                    /* Limite defensive 4MB : SVG > 4MB c'est suspect.
                     * En Lot 2a la limite sera reevaluee. */
                    fclose(f);
                    ic->svg_raw = s;
                    ic->valide  = false;
                } else {
                    char* buf = (char*)malloc((size_t)sz + 1);
                    if (buf == NULL) {
                        fclose(f);
                        ic->svg_raw = s;
                        ic->valide  = false;
                    } else {
                        size_t got = fread(buf, 1, (size_t)sz, f);
                        fclose(f);
                        buf[got] = '\0';
                        ObjString* loaded = obj_copy_string(buf, (int)got);
                        free(buf);
                        ic->svg_raw            = loaded;
                        ic->valide             = true;
                        ic->couleur_modifiable = svg_seems_color_editable(
                            loaded->chars, loaded->length);
                    }
                }
            }
        }
        vm_pop();
        return OBJ_VAL(ic);
    }
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName &&
            strcmp(inst->structName->chars, "Image") == 0) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Icone",
                                  "native", "Icone", "arg", "source",
                                  "expected", "Icone|chaine",
                                  "actual", "Image", NULL);
            ObjIcone* ic = obj_new_icone();
            ic->valide = false;
            return OBJ_VAL(ic);
        }
    }
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Icone",
                          "native", "Icone", "arg", "source",
                          "expected", "Icone|chaine",
                          "actual", "autre", NULL);
    ObjIcone* ic = obj_new_icone();
    ic->valide = false;
    return OBJ_VAL(ic);
}

/* Wrapper public exporte pour vm.c (reassignation polymorphique
 * ..Icone = <expr>). parse_to_icone reste static : seul ce wrapper
 * traverse la frontiere translation-unit. */
Value owl_icones_parse_value(Value v) {
    return parse_to_icone(v);
}

/* ================================================================
 * IconeVersImage(ICN) — Lot 2e-2.
 *
 * Rasterise l'icone via Qt6Svg (hook stdlib_ui.c -> icone_render_qt.cpp,
 * enregistre au boot owl.exe via owl_icone_register_rasterizer) et
 * construit une ObjInstance Image bitmap.
 *
 * En cas d'erreur (icone invalide, SVG malformed, rasterizer absent en
 * mode CLI, ou rasterizer KO) : retourne une Image vide non-valide
 * (Valide=Faux). Le caller peut tester ..Valide pour distinguer.
 *
 * Taille cible fixe a 64x64 px en V1 (taille usuelle des icones UI).
 *
 * Construction ObjInstance Image inline : pas besoin d'allouer un
 * OwlImage* sous-jacent — on stocke les pixels RGBA brut dans un champ
 * dedie pour permettre une utilisation ulterieure (sauvegarde PNG via
 * dSauveImage). En V1 on pose juste Valide/Largeur/Hauteur, le caller
 * peut composer avec le champ _rgba (non-public).
 * ================================================================ */
#define OWL_ICONE_RENDER_SIZE 64

static ObjInstance* icone_make_image_instance(int width, int height) {
    ObjString* name = obj_copy_string("Image", 5);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_push(OBJ_VAL(inst));

    bool valid = (width > 0 && height > 0);
    inst_field_set_bool(inst, "Valide",  valid);
    inst_field_set_int (inst, "Largeur", width);
    inst_field_set_int (inst, "Hauteur", height);
    inst_field_set_bool(inst, "AvecAlpha",   valid);
    inst_field_set_int (inst, "BitParPixel", valid ? 32 : 0);

    vm_pop();
    vm_pop();
    return inst;
}

/* Helper exporte : rasterise un ObjIcone et retourne true + buffer RGBA
 * via out_*. Reutilise par stdlib_ui.c. Caller free(*out_rgba). */
bool owl_icone_rasterize_value(Value icn_value, int target_size,
                               unsigned char** out_rgba,
                               int* out_w, int* out_h) {
    if (out_rgba) *out_rgba = NULL;
    if (out_w)    *out_w = 0;
    if (out_h)    *out_h = 0;
    if (!IS_ICONE(icn_value)) return false;
    ObjIcone* icn = AS_ICONE(icn_value);
    if (icn->svg_raw == NULL || icn->svg_raw->length <= 0) return false;
    if (!icn->valide) return false;
    if (g_icone_rasterize_fn == NULL) return false;
    /*  : passe forme + couleur_fond au rasterizer.
     *  : si couleur unset (-1), passe 0 (noir) au lieu de
     * 0xFFFFFF (blanc) qui serait invisible sur fond clair. Pattern
     * coherent vm.c L4901-4903 "couleur == -1 -> retourne 0 (noir)". */
    bool want_bg = (icn->couleur_fond != -1);
    unsigned int color_eff = (icn->couleur == -1)
                             ? 0u
                             : (unsigned int)(icn->couleur & 0xFFFFFF);
    return g_icone_rasterize_fn(
        icn->svg_raw->chars, icn->svg_raw->length,
        color_eff,
        icn->couleur_modifiable,
        target_size,
        icn->rotation,
        icn->forme,
        (unsigned int)((want_bg ? icn->couleur_fond : 0) & 0xFFFFFF),
        want_bg,
        out_rgba, out_w, out_h);
}

/* Compat ABI : owl_stdlib.h declare void* owl_icone_rasterize_to_image
 * pour permettre une eventuelle integration backend Qt. En V1 on
 * renvoie NULL (les consommateurs UI passent par owl_icone_rasterize_value
 * directement pour eviter le detour OwlImage). */
void* owl_icone_rasterize_to_image(Value icn_value) {
    (void)icn_value;
    return NULL;
}

static Value fn_IconeVersImage(int argCount, Value* args) {
    if (argCount < 1) {
        return OBJ_VAL(icone_make_image_instance(0, 0));
    }
    unsigned char* rgba = NULL;
    int w = 0, h = 0;
    if (owl_icone_rasterize_value(args[0], OWL_ICONE_RENDER_SIZE,
                                  &rgba, &w, &h)) {
        /*  : conserve les pixels dans l'Image retournee
         * (avant : free(rgba) -> Image scalaire sans pixels lisibles).
         * Debloque dPixelCouleur sur l'Image rasterisee, utile pour
         * tests headless + composition graphique (dDessine*). */
        Value result = stdlib_image_value_from_rgba(rgba, w, h);
        free(rgba);
        return result;
    }
    return OBJ_VAL(icone_make_image_instance(0, 0));
}

static const OwlParam p_IconeVersImage[] = {
    { .name = "icone", .type = "Icone", .desc = "icone source" },
    {0},
};

static const OwlSignature sig_IconeVersImage[] = {
    { 1, p_IconeVersImage, "Image",
      "image rasterisee a partir de l'icone" },
    {0},
};

static const OwlNativeDef ICONE_NATIVES[] = {
    {
        .canonical  = "IconeVersImage",
        .nameEN     = "IconToImage",
        .category   = "Icone",
        .descFR     = "Rasterise une icone vectorielle vers une Image bitmap, utile pour la sauvegarder en PNG/JPEG ou la composer avec dDessine*.",
        .descEN     = "Rasterizes a vector icon into a bitmap Image, useful for saving as PNG/JPEG or composing with dDessine*.",
        .signatures = sig_IconeVersImage,
        .impl       = fn_IconeVersImage,
    },
};

/* ================================================================
 * Init
 * ================================================================ */

void stdlib_icones_init(void) {
    static const OwlEnum* ICONE_ENUMS[] = { &E_Forme };
    owl_registry_register_enums(ICONE_ENUMS,
                                (int)(sizeof(ICONE_ENUMS) / sizeof(ICONE_ENUMS[0])));
    owl_registry_register_consts(FORME_CONSTS,
                                 (int)(sizeof(FORME_CONSTS) / sizeof(FORME_CONSTS[0])));
    owl_registry_register_types(ICONE_TYPES,
                                (int)(sizeof(ICONE_TYPES) / sizeof(ICONE_TYPES[0])));
    owl_registry_register_module(ICONE_NATIVES,
                                 (int)(sizeof(ICONE_NATIVES) / sizeof(ICONE_NATIVES[0])));

    /* Coerce parser : permet `var est une Icone = <expr>` cote compilo. */
    owl_register_instance_coerce("Icone", parse_to_icone);

    /* Namespace `icones.<nom>` : singleton ObjIconesNamespace expose
     * sous les deux noms canon (FR/EN). Le lookup est dispatch par
     * vm.c sur OP_STRUCT_GET vers owl_icones_namespace_lookup. */
    ObjIconesNamespace* ns = obj_new_icones_namespace();
    obj_globals_set("icones", OBJ_VAL(ns));
    obj_globals_set("icons",  OBJ_VAL(ns));
}
