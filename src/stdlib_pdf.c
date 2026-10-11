// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_pdf.c - Natives canon pour Etats / Impression / PDF.
 *
 * Flow canon pour Etat libre :
 *   1. iConfigure() [optionnel]
 *   2. iDestination(iPDFGenerique, "fichier.pdf") <- ouvre le doc
 *   3. iCreePolice(id, taille_pt, style, famille_ou_nom)
 *   4. iMarge(haut, bas, gauche, droite)
 *   5. iImprime(iPolice(1) + iPosX(50) + "texte"), iLigne, iCadre, ...
 *   6. iFinImprime() <- finalise + ecrit le fichier
 *
 * iPolice/iPosX/iPosY retournent une chaine de controle "\x1F<TYPE><VAL>\x1E"
 * que iImprime/iImprimeMot parse en debut de string. Permet le pattern
 * canon : iImprime(iPolice(1) + iPosX(20) + "texte").
 *
 * iImprimeEtat (V2) execute un Etat declare via le DSL 'ETAT_X est un
 * Etat(...) ... FIN'.
 */

#include "stdlib_pdf.h"
#include "stdlib_pdf_defs.h"  /* DEFS + wire des impls (donnees de registre Qt-free) */
#include "owl_stdlib.h"
#include "owl_errors.h"
#include "owl_registry.h"
#include "owl_pdf.h"
#include "owl_image.h"     /* render_etat_image : owl_image_save_png pour ObjImage */
#include "pdf_postprocess.h"
#include "vm.h"
#include "object.h"
#ifdef _WIN32
#  include <process.h>     /* getpid */
#else
#  include <unistd.h>
#endif
#include "memory.h"
#include "sentinels.h"
#include "etat_decl.h"
#include "stdlib_ui.h"     /*  : owl_etat_prepare_props */
#include "ui_catalog.h"    /*  : lookup props bilingue via catalogue */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static OwlPdfDoc* g_doc = NULL;
/* Chemin du PDF en cours d'ecriture. Capture par iDestination, lu par
 * iFinImprime pour le hook qpdf post-traitement. */
static char g_doc_path[1024] = "";

/* Defini dans stdlib_image.c (cf. extern dans stdlib_ui.c). Wrap un
 * OwlImage* en ObjInstance "Image" prete a partir dans un ObjArray. */
extern ObjInstance* image_make_instance(OwlImage* img);

/*  — mode iImageMemoire : tableau utilisateur cible des pages
 * rasterisees. Pose par iDestination(iImageMemoire, tabPages), reset par
 * iFinImprime ou par un nouvel iDestination. Le callback C i_dest_img_
 * on_page (defini plus bas) recoit chaque OwlImage*, la wrappe via
 * image_make_instance et fait obj_array_push. */
static ObjArray* g_doc_img_target = NULL;

static void i_dest_img_on_page(OwlImage* page, void* userdata) {
    ObjArray* target = (ObjArray*)userdata;
    if (!target) { /* pas de cible : drop pour eviter le leak. */
        /* image_make_instance prendrait possession en mode normal — ici on
         * laisse owl_image_new_from_rgba avoir alloue et on ne reference pas
         * l'OwlImage* donc on ne peut pas appeler de free public ; le wrap
         * via image_make_instance + GC s'en chargera... mais sans wrap on
         * fuirait. Solution : wrapper meme si pas de target pour laisser le
         * GC l'attraper au prochain cycle. */
        if (page) {
            ObjInstance* inst = image_make_instance(page);
            (void)inst;  /* visible du GC root pile ? non. Mais sans target
                          * c'est un cas de programmation (iDestination sans
                          * 2e arg en mode img) — pas grave. */
        }
        return;
    }
    if (!page) return;
    ObjInstance* inst = image_make_instance(page);
    vm_push(OBJ_VAL(inst));  /* GC root pendant obj_array_push (peut grow) */
    obj_array_push(target, OBJ_VAL(inst));
    vm_pop();
}

/* Etat global iParametrePDF (canon). Pose AVANT iDestination,
 * applique au doc PDF lors de son ouverture. Reset par iRAZ ou reapose par
 * un nouveau iParametrePDF.
 *
 * V1 (Qt6 seul) : seul grayscale + pdfA1b ont effet runtime. Les autres
 * options sont memorisees pour V2 qpdf (chiffrement, signets, attachments). */
typedef struct {
    /* Format/encodage — effet Qt6 partiel. */
    bool grayscale;        /* iPDFNiveauGris */
    bool pdfA1b;           /* iPDFA / iPDFA1b */
    bool pdfA3b;           /* iPDFA3b — V2 qpdf */
    /* Signets — V2 qpdf. */
    bool signetPage;       /* iSignetPage */
    /* Protection — V2 qpdf. */
    int  protectionFlags;  /* bitmask OWL_PDF_PROT_* */
    char passwordOpen[128];
    char passwordOwner[128];
    bool hasParams;        /* Vrai si une config a ete posee */
} PdfParams;

static PdfParams g_pdf_params = {0};

static void pdf_params_reset(void) {
    memset(&g_pdf_params, 0, sizeof(g_pdf_params));
}

/* Applique au doc Qt les options Qt-supportees. Doit etre appele juste
 * apres owl_pdf_open, avant la 1ere primitive d'impression. Warne sur
 * stderr pour les options non-supportees Qt6. */
static void pdf_params_apply(OwlPdfDoc* d) {
    if (!d || !g_pdf_params.hasParams) return;
    /* Effets Qt6 reels. */
    if (g_pdf_params.grayscale) owl_pdf_set_grayscale(d, 1);
    if (g_pdf_params.pdfA1b)    owl_pdf_set_pdf_a1b(d, 1);
    /* Options Qt6 partiel : on warn avec mention "qpdf-only" si build sans
     * qpdf, sinon le post-traitement les prendra a iFinImprime. */
    bool hasQpdf = owl_pdf_has_qpdf();
    if (g_pdf_params.pdfA3b) {
        /* PDF/A-3b reste limite meme avec qpdf (validation profil ICC out
         * of scope V2). On marque PDF/A-1b cote Qt et qpdf laisse passer. */
        owl_pdf_set_pdf_a1b(d, 1);
    }
    if (!hasQpdf) {
        if (g_pdf_params.signetPage) {
            fprintf(stderr,
                "[iParametrePDF] iSignetPage : qpdf non disponible au build, "
                "le PDF sera genere sans signets.\n");
        }
        if (g_pdf_params.protectionFlags != 0) {
            fprintf(stderr,
                "[iParametrePDF] Options de protection : qpdf non disponible "
                "au build, le PDF sera genere sans protection.\n");
        }
        if (g_pdf_params.passwordOpen[0] || g_pdf_params.passwordOwner[0]) {
            fprintf(stderr,
                "[iParametrePDF] Mot de passe : qpdf non disponible au build, "
                "le PDF sera genere sans chiffrement.\n");
        }
    }
    /* Si hasQpdf, les options seront appliquees par owl_pdf_postprocess
     * dans iFinImprime — silencieux ici. */
}

/* iPoliceDidot : flag global. Si Vrai, iCreePolice interprete <taille>
 * comme des points DIDOT (1 didot = 0.376 mm) au lieu des points pica
 * Qt natifs (1 pica = 0.353 mm). Conversion appliquee au moment du
 * font_create. */
static bool g_use_didot_units = false;

static double arg_num(Value v, double dflt) {
    if (IS_INTEGER(v)) return (double)AS_INTEGER(v);
    if (IS_REAL(v))    return AS_REAL(v);
    return dflt;
}
static const char* arg_str(Value v, const char* dflt) {
    if (IS_STRING(v)) return AS_CSTRING(v);
    return dflt;
}

static OwlPdfDoc* require_doc(const char* fnName) {
    if (!g_doc) {
        fprintf(stderr,
            "[%s] aucun doc d'impression ouvert. Appeler iDestination(...) avant.\n", fnName);
        return NULL;
    }
    return g_doc;
}

/* ============================================================
 * Parser des chaines de controle (iPolice/iPosX/iPosY prefixes).
 *
 * Format : "\x1F<TYPE><VAL>\x1E" ou TYPE = 'P' (police), 'X' (posX),
 * 'Y' (posY). VAL est decimal ASCII. Tokens dispersables n'importe ou
 * dans la string — pas seulement en debut (canon multi-segment).
 *
 * Le caractere Caract(10) ('\n') dans le texte est interprete comme un
 * saut de ligne explicite (canon).
 * ============================================================ */

/* Applique un token de controle isole (pose le curseur ou la police). */
static void apply_control_token(OwlPdfDoc* d, char type, const char* val) {
    switch (type) {
        case 'P': owl_pdf_font_use(d, atoi(val)); break;
        case 'X': owl_pdf_set_cursor_x(d, atof(val)); break;
        case 'Y': owl_pdf_set_cursor_y(d, atof(val)); break;
        default: break;
    }
}

/* Imprime une string contenant 0+ tokens de controle intercales avec
 * du texte. Pour chaque token : applique le change. Pour chaque chunk
 * de texte (jusqu'au prochain token ou '\n') : print_word. '\n' force
 * un saut de ligne intermediaire. Si end_with_newline, ajoute un saut
 * apres tout (true=iImprime, false=iImprimeMot). */
static void print_with_controls(OwlPdfDoc* d, const char* s, bool end_with_newline) {
    if (!s) {
        if (end_with_newline) owl_pdf_newline(d);
        return;
    }
    char buf[4096];
    int len = 0;
    while (*s) {
        if (*s == '\x1F') {
            /* Flush du chunk en cours avant d'appliquer le control. */
            if (len > 0) { buf[len] = 0; owl_pdf_print_word(d, buf); len = 0; }
            char type = s[1];
            if (!type) break;
            const char* p = s + 2;
            const char* end = p;
            while (*end && *end != '\x1E') end++;
            if (*end != '\x1E') break;  /* malforme : abandon */
            char ctrl[32];
            int n = (int)(end - p);
            if (n >= (int)sizeof(ctrl)) n = sizeof(ctrl) - 1;
            memcpy(ctrl, p, n);
            ctrl[n] = 0;
            apply_control_token(d, type, ctrl);
            s = end + 1;
            continue;
        }
        if (*s == '\n') {
            /* Caract(10) -> saut de ligne explicite. */
            if (len > 0) { buf[len] = 0; owl_pdf_print_word(d, buf); len = 0; }
            owl_pdf_newline(d);
            s++;
            continue;
        }
        if (len < (int)sizeof(buf) - 1) buf[len++] = *s;
        s++;
    }
    if (len > 0) { buf[len] = 0; owl_pdf_print_word(d, buf); }
    if (end_with_newline) owl_pdf_newline(d);
}

/* Strip de tous les tokens de controle d'une string. Retourne la longueur
 * du resultat ecrit dans dst (tronque si > capacite). Utilise par
 * iLargeurTexte qui mesure la largeur du texte sans les controls. */
static int strip_controls(const char* s, char* dst, int dstSize) {
    if (!s || dstSize <= 0) { if (dstSize > 0) dst[0] = 0; return 0; }
    int len = 0;
    while (*s && len < dstSize - 1) {
        if (*s == '\x1F') {
            const char* end = s + 2;
            while (*end && *end != '\x1E') end++;
            if (*end == '\x1E') { s = end + 1; continue; }
            break;
        }
        dst[len++] = *s++;
    }
    dst[len] = 0;
    return len;
}

/* ============================================================
 * Lifecycle d'une session d'impression
 * ============================================================ */

/* iConfigure — canon.
 *   Syntaxe 1 : iConfigure() -> ouvre le dialog standard de config impression
 *               (Vrai si validé, Faux si annulé).
 *   Syntaxe 2 : iConfigure(<nom>) -> sélectionne l'imprimante par son nom
 *               système (Vrai si trouvée + activée). Modification process-local
 *               uniquement.
 *
 * Divergence canon (cf docs/VISION.md §5bis) : le 2e arg <modif_temporaire>
 * de canon n'est pas exposé — Qt::PrintSupport n'expose pas
 * SetDefaultPrinter cross-platform. Le compilateur rejette toute syntaxe
 * a 2 args (la signature catalog declare 0 ou 1 args strict). */
static Value fn_i_configure(int argCount, Value* args) {
    if (argCount == 0) {
        return BOOL_VAL(owl_printer_configure_dialog() != 0);
    }
    /* 1 arg garanti par la signature stricte. Type chaine attendu. */
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const char* name = AS_CSTRING(args[0]);
    return BOOL_VAL(owl_printer_set_by_name(name, /*persistent=*/0) != 0);
}

/* iDestination([type [, cible]]) — canon + extension OWL.
 *
 * Sans args : retourne la destination courante (entier).
 * Avec args : pose la destination ET ouvre le document selon le type :
 *   - iPDFGenerique / iPDF : cible = chaine (chemin fichier de sortie).
 *   - iImageMemoire (extension ) : cible = tableau de Image qui
 *     sera peuple page par page lors de iImprimeEtat / iFinImprime.
 * Autres types : stub (no-op + warning). */
static Value fn_i_destination(int argCount, Value* args) {
    /* Getter (sans args) : retourne le type courant. */
    if (argCount == 0) {
        if (g_doc_img_target) return INTEGER_VAL(OWL_IDEST_IMAGE_MEMOIRE);
        return INTEGER_VAL(g_doc ? OWL_IDEST_PDF_GENERIQUE : OWL_IDEST_IMPRIMANTE);
    }
    if (!IS_INTEGER(args[0])) {
        fprintf(stderr, "[iDestination] type doit etre une constante (iPDFGenerique, iImageMemoire, ...).\n");
        return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
    }
    int type = (int)AS_INTEGER(args[0]);

    if (type == OWL_IDEST_PDF_GENERIQUE || type == OWL_IDEST_PDF) {
        const char* path = (argCount >= 2 && IS_STRING(args[1])) ? AS_CSTRING(args[1]) : NULL;
        if (!path || !*path) {
            fprintf(stderr, "[iDestination] PDF : chemin de sortie attendu.\n");
            return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
        }
        if (g_doc) {
            /* Doc deja ouvert -> ferme avant d'en ouvrir un nouveau. */
            owl_pdf_close(g_doc);
        }
        g_doc_img_target = NULL;
        g_doc = owl_pdf_open(path);
        if (!g_doc) {
            fprintf(stderr, "[iDestination] echec ouverture '%s'\n", path);
            return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
        }
        /* Capture le path pour iFinImprime (hook qpdf post-traitement). */
        strncpy(g_doc_path, path, sizeof(g_doc_path) - 1);
        g_doc_path[sizeof(g_doc_path) - 1] = '\0';
        /* Applique les options posees par un eventuel iParametrePDF prealable. */
        pdf_params_apply(g_doc);
        return INTEGER_VAL(OWL_IDEST_PDF_GENERIQUE);
    }

    if (type == OWL_IDEST_IMAGE_MEMOIRE) {
        /* Extension OWL  : 2e arg = tableau de Image (sera peuple). */
        if (argCount < 2 || !IS_ARRAY(args[1])) {
            fprintf(stderr, "[iDestination] iImageMemoire : tableau de Image attendu en 2e argument.\n");
            return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
        }
        if (g_doc) {
            owl_pdf_close(g_doc);
            g_doc = NULL;
        }
        g_doc_path[0] = '\0';
        g_doc_img_target = AS_ARRAY(args[1]);
        g_doc = owl_pdf_open_image_memory(300, i_dest_img_on_page,
                                           (void*)g_doc_img_target);
        if (!g_doc) {
            fprintf(stderr, "[iDestination] iImageMemoire : echec ouverture backend.\n");
            g_doc_img_target = NULL;
            return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
        }
        return INTEGER_VAL(OWL_IDEST_IMAGE_MEMOIRE);
    }

    /* Autres destinations : non implementees en V1. */
    fprintf(stderr,
        "[iDestination] type %d non supporte (seul iPDFGenerique/iImageMemoire implem).\n", type);
    return INTEGER_VAL(OWL_IDEST_IMPRIMANTE);
}

/* iFinImprime() — finalise + ecrit le fichier + libere les ressources.
 * Ferme le doc PDF courant. Hook qpdf post-traitement si iParametrePDF
 * a pose des options qui necessitent du post-traitement (chiffrement,
 * signets, PDF/A-3b). */
static Value fn_i_fin_imprime(int argCount, Value* args) {
    (void)argCount; (void)args;
    if (!g_doc) return NIL_VAL;
    owl_pdf_close(g_doc);
    g_doc = NULL;
    /* Hook qpdf : si on a un path et des options qpdf-only, post-traite. */
    if (g_doc_path[0] && g_pdf_params.hasParams) {
        bool needsQpdf = (g_pdf_params.passwordOpen[0] != 0) ||
                         (g_pdf_params.passwordOwner[0] != 0) ||
                         (g_pdf_params.protectionFlags != 0) ||
                         (g_pdf_params.signetPage) ||
                         (g_pdf_params.pdfA3b);
        if (needsQpdf) {
            OwlPdfPostOptions opt = {0};
            opt.passwordOpen    = g_pdf_params.passwordOpen[0] ? g_pdf_params.passwordOpen : NULL;
            opt.passwordOwner   = g_pdf_params.passwordOwner[0] ? g_pdf_params.passwordOwner : NULL;
            opt.protectionFlags = g_pdf_params.protectionFlags;
            opt.signetPage      = g_pdf_params.signetPage;
            opt.pdfA3b          = g_pdf_params.pdfA3b;
            int rc = owl_pdf_postprocess(g_doc_path, g_doc_path, &opt);
            if (rc != 0) {
                fprintf(stderr, "[iFinImprime] qpdf post-traitement code=%d.\n", rc);
            }
        }
    }
    g_doc_path[0] = '\0';
    /* Reset la cible image-memoire. owl_pdf_close a deja flushe
     * la derniere page via le callback C, donc le tableau utilisateur
     * contient deja toutes les Image (1 par page). */
    g_doc_img_target = NULL;
    /* Hermeticite : iParametrePDF configure LE document en cours de
     * generation. Une fois iFinImprime consomme (post-traitement qpdf inclus),
     * les options sont a usage unique — sans ce reset, protection/mot de
     * passe/signet/PDF-A3b survivent au process et le prochain iFinImprime
     * (meme sans iParametrePDF) re-chiffrerait un document qui ne le demande
     * pas (flux de contenu illisibles). Une nouvelle session doit re-declarer
     * ses parametres. */
    pdf_params_reset();
    return NIL_VAL;
}

/* iParametrePDF — canon. Pose les options du PDF a generer.
 * Doit etre appelee AVANT iDestination (sinon les options sont memorisees
 * mais s'appliquent au prochain doc).
 *
 * Trois syntaxes canon :
 *   1. iParametrePDF(<pdfParametre>)                     — variable struct (V2 qpdf)
 *   2. iParametrePDF(<MotDePasse>[, <MdpProt>][, <Qual>]) — protection par mdp
 *   3. iParametrePDF(<OptionsProt>[, <Mdp>, <MdpProt>][, <Qual>]) — protection bitwise
 *
 * V1 (Qt6 seul) : iPDFNiveauGris + iPDFA1b ont effet reel. Les autres
 * options (mdp, signets, protection, A3b) sont memorisees mais warnent
 * au runtime — V2 via qpdf.
 *
 * Detection syntaxe : args[0] est-il une CHAINE (mot de passe) ou un
 * ENTIER (constante d'options) ? */
static bool pdf_arg_is_protection_const(Value v) {
    if (!IS_INTEGER(v)) return false;
    int64_t i = AS_INTEGER(v);
    /* Sentinel zone (format/qualite/signet INT32_MIN+offset). */
    if (i >= OWL_PDF_PARAM_BASE && i <= OWL_PDF_PARAM_BASE + 30) return true;
    /* Petits flags combinables (protection 1/2/4/8, max 0xFF). */
    if (i >= 0 && i <= OWL_PDF_PROT_MASK) return true;
    return false;
}

/* Applique une constante OWL_PDF_* au state g_pdf_params. Detecte la
 * famille selon la valeur (sentinel zone vs petits flags protection). */
static void pdf_params_apply_const(int64_t v) {
    if (v == OWL_PDF_NIVEAU_GRIS)       g_pdf_params.grayscale = true;
    else if (v == OWL_PDF_A1B)          g_pdf_params.pdfA1b = true;
    else if (v == OWL_PDF_A3B)          g_pdf_params.pdfA3b = true;
    else if (v == OWL_PDF_SIGNET_AUCUN) g_pdf_params.signetPage = false;
    else if (v == OWL_PDF_SIGNET_PAGE)  g_pdf_params.signetPage = true;
    else if (v >= 0 && v <= OWL_PDF_PROT_MASK) {
        /* Petits flags combinables (canon '+' bitwise). */
        g_pdf_params.protectionFlags |= (int)v;
    }
}

static Value fn_i_parametre_pdf(int argCount, Value* args) {
    if (argCount == 0) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "iParametrePDF",
                              "native", "iParametrePDF",
                              "arg", "1",
                              NULL);
        return NIL_VAL;
    }
    pdf_params_reset();
    g_pdf_params.hasParams = true;

    /* Detection de la syntaxe selon le 1er arg. */
    if (IS_STRING(args[0])) {
        /* Syntaxe 2 : iParametrePDF(<MdpOuvre>[, <MdpProt>][, <Qual>]) */
        const char* mdp = AS_CSTRING(args[0]);
        if (mdp) strncpy(g_pdf_params.passwordOpen, mdp,
                          sizeof(g_pdf_params.passwordOpen) - 1);
        if (argCount >= 2 && IS_STRING(args[1])) {
            const char* mdpProt = AS_CSTRING(args[1]);
            strncpy(g_pdf_params.passwordOwner, mdpProt,
                    sizeof(g_pdf_params.passwordOwner) - 1);
        }
        /* 3eme arg = combinaison qualite/signet/format. */
        for (int i = 2; i < argCount; i++) {
            if (IS_INTEGER(args[i])) pdf_params_apply_const(AS_INTEGER(args[i]));
        }
    } else if (pdf_arg_is_protection_const(args[0])) {
        /* Syntaxe 3 : iParametrePDF(<OptProt>[, <Mdp>, <MdpProt>][, <Qual>]) */
        for (int i = 0; i < argCount; i++) {
            if (IS_INTEGER(args[i])) pdf_params_apply_const(AS_INTEGER(args[i]));
            else if (IS_STRING(args[i])) {
                /* 1er string = mdp ouvre, 2eme = mdp owner. */
                if (!g_pdf_params.passwordOpen[0]) {
                    strncpy(g_pdf_params.passwordOpen, AS_CSTRING(args[i]),
                            sizeof(g_pdf_params.passwordOpen) - 1);
                } else {
                    strncpy(g_pdf_params.passwordOwner, AS_CSTRING(args[i]),
                            sizeof(g_pdf_params.passwordOwner) - 1);
                }
            }
        }
    } else if (IS_INSTANCE(args[0])) {
        /* Syntaxe 1 : iParametrePDF(<pdfParametre>) — canon. */
        ObjInstance* p = AS_INSTANCE(args[0]);
        Value v;
        ObjString* k;

        /* Mots de passe. */
        k = obj_copy_string("MotDePasseOuverture", 19);
        if (table_get(&p->fields, k, &v) && IS_STRING(v)) {
            const char* s = AS_CSTRING(v);
            if (s) strncpy(g_pdf_params.passwordOpen, s,
                            sizeof(g_pdf_params.passwordOpen) - 1);
        }
        k = obj_copy_string("MotDePasseModification", 22);
        if (table_get(&p->fields, k, &v) && IS_STRING(v)) {
            const char* s = AS_CSTRING(v);
            if (s) strncpy(g_pdf_params.passwordOwner, s,
                            sizeof(g_pdf_params.passwordOwner) - 1);
        }

        /* Protection (combinaison Format + iProtection*). */
        k = obj_copy_string("Protection", 10);
        if (table_get(&p->fields, k, &v) && IS_INTEGER(v)) {
            int64_t prot = AS_INTEGER(v);
            /* Petits flags iProtection* (combinable). */
            if (prot >= 0 && prot <= OWL_PDF_PROT_MASK) {
                g_pdf_params.protectionFlags |= (int)prot;
            }
            /* Format A/A1b/A3b. */
            else if (prot == OWL_PDF_A1B) g_pdf_params.pdfA1b = true;
            else if (prot == OWL_PDF_A3B) g_pdf_params.pdfA3b = true;
        }

        /* Signet (booleen). */
        k = obj_copy_string("Signet", 6);
        if (table_get(&p->fields, k, &v) && IS_BOOL(v) && AS_BOOL(v)) {
            g_pdf_params.signetPage = true;
        }

        /* Unicode : OWL UTF-8 natif, no-op silencieux. */
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "iParametrePDF",
                              "native", "iParametrePDF",
                              "arg", "1",
                              "expected", "chaine | entier | pdfParametre",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }

    /* Si un doc est deja ouvert, on applique immediatement. Sinon les
     * options seront appliquees au prochain iDestination. */
    if (g_doc) pdf_params_apply(g_doc);
    return NIL_VAL;
}

/* Helper : appelle une ObjFunction sans args en mode re-entrant.
 * La VM gere son frame stack — on push, on call, on pop le resultat. */
static void call_etat_block_fn(ObjFunction* fn) {
    if (!fn) return;
    Value fnVal = OBJ_VAL(fn);
    vm_push(fnVal);
    if (vm_call_reentrant(fnVal, 0) == INTERPRET_OK) {
        vm_pop();  /* result */
    }
}

/* Lookup d'une propriete dans un OwlControlDecl par nom, bilingue FR/EN.
 *
 *  — le nom d'une prop est stocke verbatim dans l'IR (forme FR ou EN
 * telle que tapee, accents strippes au parse) ; le sink doit accepter les DEUX
 * formes canon. Chokepoint : on matche si le nom stocke et le nom recherche
 * designent la meme entree de catalogue (owl_control_catalog_find est deja
 * bilingue -> find("Largeur") == find("Width")). Repli sur egalite exacte pour
 * les noms hors catalogue. */
static const OwlPropertyInit* ctrl_find_prop(const OwlControlDecl* c, const char* name) {
    if (!c || !name) return NULL;
    const OwlControlPropDef* target = owl_control_catalog_find(c->kind, name);
    for (int i = 0; i < c->property_count; i++) {
        const char* stored = c->properties[i].name;
        if (strcasecmp(stored, name) == 0) return &c->properties[i];
        if (target && owl_control_catalog_find(c->kind, stored) == target)
            return &c->properties[i];
    }
    return NULL;
}

/* Resout la valeur effective d'une propriete : constante directe ou eval
 * dynamique (ObjFunction* 0-arg). Renvoie NIL_VAL si p est NULL. */
static Value prop_resolve(const OwlPropertyInit* p) {
    if (!p) return NIL_VAL;
    if (p->is_constant) return p->const_value;
    if (p->eval) {
        Value fnVal = OBJ_VAL(p->eval);
        vm_push(fnVal);
        if (vm_call_reentrant(fnVal, 0) == INTERPRET_OK) {
            return vm_pop();
        }
    }
    return NIL_VAL;
}

/* Convertit Value en double (pour X/Y/Largeur/Hauteur). dflt si type incompatible. */
static double prop_num(const OwlPropertyInit* p, double dflt) {
    if (!p) return dflt;
    Value v = prop_resolve(p);
    return arg_num(v, dflt);
}

/* Convertit Value en chaine UTF-8 (pour Texte/Libellé). Buffer alloue par le
 * caller si numerique (formatte via Numerique->Chaine). NULL si NIL. */
static const char* prop_str(const OwlPropertyInit* p, char* buf, size_t bufSize) {
    if (!p) return NULL;
    Value v = prop_resolve(p);
    if (IS_STRING(v)) return AS_CSTRING(v);
    if (IS_INTEGER(v)) {
        snprintf(buf, bufSize, "%lld", (long long)AS_INTEGER(v));
        return buf;
    }
    if (IS_REAL(v)) {
        owl_real_to_str(AS_REAL(v), buf, bufSize);
        return buf;
    }
    if (IS_BOOL(v)) {
        return AS_BOOL(v) ? "Vrai" : "Faux";
    }
    if (IS_NIL(v)) return NULL;
    return NULL;
}

/* Imprime un Libelle d'Etat : applique iPolice si Police = N, puis appelle
 * iImprimeZone(text, X, Y, X+Largeur, Y+Hauteur, alignement). Le contenu
 * vient de la prop "Libellé" (canon) ou "Texte" (alias accepte). */
static const char* family_name_from_int(int v);  /* defini plus bas */
static double pica_to_mm(double pica);            /* defini plus bas (primitives PDF) */

/* Id reserve pour le font cree dynamiquement par chaque Libelle. Reutilise
 * d'un Libelle a l'autre — chaque render reconstruit le font. */
#define ETAT_LABEL_FONT_ID 9990

/* Convertit la valeur d'une prop (chaine "Arial" ou constante OWL_IPOL_*) en
 * nom de famille pour owl_pdf_font_create. NULL si type invalide. */
static const char* prop_family_name(const OwlPropertyInit* p, char* tmp, size_t tmpSize) {
    if (!p) return NULL;
    Value v = prop_resolve(p);
    if (IS_STRING(v)) return AS_CSTRING(v);
    if (IS_INTEGER(v)) {
        const char* fam = family_name_from_int((int)AS_INTEGER(v));
        if (fam) return fam;
        snprintf(tmp, tmpSize, "%lld", (long long)AS_INTEGER(v));
        return tmp;
    }
    return NULL;
}

static bool prop_bool(const OwlPropertyInit* p) {
    if (!p) return false;
    Value v = prop_resolve(p);
    if (IS_BOOL(v)) return AS_BOOL(v);
    if (IS_INTEGER(v)) return AS_INTEGER(v) != 0;
    return false;
}

/*  — retrouve l'instance runtime materialisee (ObjUiControlInstance)
 * d'un sous-champ d'Etat a partir de son decl compile-time. L'Etat vivant est
 * au sommet de vm.etat_stack (push par fn_i_imprime_etat), ses controls[] sont
 * construits dans l'ordre blocs/controls et stockent chacun leur decl. Le match
 * se fait par pointeur de decl. NULL hors impression (Etat standalone). */
static ObjUiControlInstance* etat_inst_for_ctrl(const OwlControlDecl* ctrl) {
    ObjEtatInstance* inst = vm_etat_stack_top();
    if (!inst || !ctrl) return NULL;
    for (int i = 0; i < inst->control_count; i++) {
        ObjUiControlInstance* uici = inst->controls[i];
        if (uici && uici->decl == ctrl) return uici;
    }
    return NULL;
}

/*  — convertit une Value du casier d'instance en chaine affichable.
 * NULL si le type n'est pas rendable en texte (NIL, instance...) → le caller
 * retombe sur le decl. */
static const char* value_disp_str(Value v, char* buf, size_t bufSize) {
    if (IS_STRING(v)) return AS_CSTRING(v);
    if (IS_INTEGER(v)) { snprintf(buf, bufSize, "%lld", (long long)AS_INTEGER(v)); return buf; }
    if (IS_REAL(v))    { owl_real_to_str(AS_REAL(v), buf, bufSize); return buf; }
    if (IS_BOOL(v))    return AS_BOOL(v) ? "Vrai" : "Faux";
    return NULL;
}

/*  — lecture LIVE d'une prop textuelle : casier d'instance (ecriture
 * runtime `LIB..Libelle =`, seed declaratif, valeur variant par ligne) d'abord,
 * repli sur le decl compile-time (comportement statique historique). Lu a
 * CHAQUE rendu de bloc → jamais gele. */
static const char* prop_str_live(ObjUiControlInstance* uici,
                                 const OwlControlDecl* ctrl, const char* name,
                                 char* buf, size_t bufSize) {
    Value v;
    if (uici && obj_ui_control_instance_headless_get(uici, name, &v)) {
        const char* s = value_disp_str(v, buf, bufSize);
        if (s) return s;
    }
    return prop_str(ctrl_find_prop(ctrl, name), buf, bufSize);
}

/*  — lecture LIVE d'une prop couleur (entier BGR) : casier d'instance
 * d'abord, repli decl. dflt si absente des deux. */
static int prop_color_live(ObjUiControlInstance* uici,
                           const OwlControlDecl* ctrl, const char* name, int dflt) {
    Value v;
    if (uici && obj_ui_control_instance_headless_get(uici, name, &v)) {
        if (IS_INTEGER(v)) return (int)AS_INTEGER(v);
        return dflt;
    }
    const OwlPropertyInit* p = ctrl_find_prop(ctrl, name);
    if (!p) return dflt;
    Value rv = prop_resolve(p);
    if (IS_INTEGER(rv)) return (int)AS_INTEGER(rv);
    return dflt;
}

/* Y de depart du bloc en cours d'impression (mm relatif a la marge haute).
 * Snapshote par run_etat_block AVANT execution du body / render des controls.
 * Permet aux widgets WYSIWYG d'avoir des coords Y locales au bloc parent
 * (Y=0 = haut du bandeau). Le runtime fait l'addition Y_widget + Y_origin
 * avant impression. */
static double g_current_block_y_origin_mm = 0.0;
static int    g_in_block_render = 0;

/*  +  : bordure Cadre (descripteur compose) en sortie PDF.
 * Symetrique du rendu Fenetre (apply_cadre_instance_to_widget, stdlib_ui.c).
 * La prop "Cadre" est une ObjInstance (cascade `Cadre..Epaisseur` fusionnee
 * par owl_etat_prepare_props). On lit Epaisseur/Couleur/Trait/Arrondi/Bords
 * et on trace un rectangle de bordure autour de la zone (x,y,x2,y2).
 *
 * Helper partage : appele par render_etat_label, render_etat_image, etc.
 * Chaque appelant calcule sa zone et passe ses 4 coins. */
static void render_etat_cadre(OwlPdfDoc* d, const OwlControlDecl* ctrl,
                              double x, double y, double x2, double y2) {
    /*  — lecture STORE-FIRST du composite Cadre (parite 
     * pour les scalaires). Le casier headless porte l'instance overridee par
     * cascade imperative (`..Cadre..Couleur =`, sticky via mark_override) ;
     * repli sur le composite materialise du decl quand aucun override. Sans
     * ce store-first, le rendu lisait const_value (reseede a chaque bloc) et
     * ignorait l'ecriture cascade sur le sous-champ. */
    ObjInstance* cadre = NULL;
    ObjUiControlInstance* uici = etat_inst_for_ctrl(ctrl);
    Value sv;
    if (uici && obj_ui_control_instance_headless_get(uici, "Cadre", &sv)
        && IS_INSTANCE(sv)) {
        cadre = AS_INSTANCE(sv);
    } else {
        const OwlPropertyInit* pCadre = ctrl_find_prop(ctrl, "Cadre");
        if (!pCadre) return;
        Value cv = prop_resolve(pCadre);
        if (!IS_INSTANCE(cv)) return;
        cadre = AS_INSTANCE(cv);
    }
    Value fv;
    #define CADRE_GET(NAME) (table_get(&cadre->fields, obj_copy_string(NAME, (int)strlen(NAME)), &fv))
    int epaisseur = 0;
    if (CADRE_GET("Epaisseur") && IS_INTEGER(fv)) epaisseur = (int)AS_INTEGER(fv);
    int couleur = -1;  /* < 0 = noir par defaut (owl_pdf_rect) */
    if (CADRE_GET("Couleur") && IS_INTEGER(fv)) couleur = (int)AS_INTEGER(fv);
    /* Trait : enum traitCadre. Defaut Continu (cf build_Cadre_template).
     * TraitAucun = pas de bordure (semantique preservee, volet 1). */
    int traitCadre = OWL_TC_CONTINU;
    if (CADRE_GET("Trait") && IS_INTEGER(fv)) traitCadre = (int)AS_INTEGER(fv);
    int bords = 0x0F;  /* tous bords par defaut */
    if (CADRE_GET("Bords") && IS_INTEGER(fv)) {
        int b = (int)AS_INTEGER(fv);
        if (b > 0) bords = b & 0x0F;
    }
    /*  : Arrondi global (rayon en pixels, comme Epaisseur).
     * Per-angle CoinHautGauche/etc portent un sous-objet Coin (Largeur/Hauteur
     * en pixels). owl_pdf_rounded_rect ne prend qu'un rayonX/rayonY global :
     * si les coins per-angle different, on approxime avec le premier coin
     * defini (limite assumee, doc). On lit aussi l'Arrondi global comme
     * fallback. */
    int arrondi = 0;
    if (CADRE_GET("Arrondi") && IS_INTEGER(fv)) arrondi = (int)AS_INTEGER(fv);
    int rayon_w_px = 0, rayon_h_px = 0;
    {
        /* Premier coin per-angle defini -> rayon X/Y (Largeur/Hauteur). */
        static const char* coins[] = { "CoinHautGauche", "CoinHautDroite",
                                       "CoinBasGauche", "CoinBasDroite" };
        for (int i = 0; i < 4 && rayon_w_px == 0 && rayon_h_px == 0; i++) {
            if (CADRE_GET(coins[i]) && IS_INSTANCE(fv)) {
                ObjInstance* coin = AS_INSTANCE(fv);
                Value lw, lh;
                ObjString* kw = obj_copy_string("Largeur", 7);
                ObjString* kh = obj_copy_string("Hauteur", 7);
                if (table_get(&coin->fields, kw, &lw) && IS_INTEGER(lw))
                    rayon_w_px = (int)AS_INTEGER(lw);
                if (table_get(&coin->fields, kh, &lh) && IS_INTEGER(lh))
                    rayon_h_px = (int)AS_INTEGER(lh);
            }
        }
    }
    #undef CADRE_GET
    /* Arrondi global s'applique aux deux axes si aucun coin per-angle. */
    if (rayon_w_px == 0 && rayon_h_px == 0 && arrondi > 0) {
        rayon_w_px = arrondi;
        rayon_h_px = arrondi;
    }

    if (traitCadre == OWL_TC_AUCUN || epaisseur <= 0) return;

    /* Map traitCadre -> trait_style PDF (Qt PenStyle) :
     * Continu/Double -> 0 (solid), Tiret -> 1, Pointille -> 2. */
    int trait_style = 0;
    if (traitCadre == OWL_TC_TIRET)          trait_style = 1;
    else if (traitCadre == OWL_TC_POINTILLE) trait_style = 2;
    double thick_mm = pica_to_mm((double)epaisseur);

    /* Arrondi : owl_pdf_rounded_rect trace les 4 cotes (pas de border_mask,
     * cf canon). Donc incompatible avec des bords partiels — DECISION :
     * les bords partiels priment, on retombe sur owl_pdf_rect (coins droits)
     * des que Bords != 0x0F. Le cadre rectangulaire coins droits du MVP
     *  (pas d'arrondi) passe donc par owl_pdf_rect a l'identique. */
    if ((rayon_w_px > 0 || rayon_h_px > 0) && bords == 0x0F) {
        double rayon_w_mm = pica_to_mm((double)rayon_w_px);
        double rayon_h_mm = pica_to_mm((double)rayon_h_px);
        owl_pdf_rounded_rect(d, x, y, x2, y2, rayon_w_mm, rayon_h_mm,
                             thick_mm, couleur, -1, trait_style, 0);
    } else {
        owl_pdf_rect(d, x, y, x2, y2, thick_mm, couleur, -1,
                     trait_style, 0, bords);
    }
}

static void render_etat_label(OwlPdfDoc* d, const OwlControlDecl* ctrl) {
    if (!d || !ctrl) return;

    /*  — instance runtime du sous-champ (casier headless). Les props
     * textuelles + CouleurFond sont lues LIVE depuis ce casier (ecriture par
     * code, valeur variant par ligne), avec repli sur le decl statique. */
    ObjUiControlInstance* uici = etat_inst_for_ctrl(ctrl);

    /* Le texte affiche : "Libellé" en canon, "Texte" en alias pratique. */
    char tbuf[256];
    const char* text = prop_str_live(uici, ctrl, "Libelle", tbuf, sizeof(tbuf));
    if (!text) text = prop_str_live(uici, ctrl, "Texte", tbuf, sizeof(tbuf));
    if (!text) text = "";  /* Libelle sans texte = OK (espace reserve) */

    double x = prop_num(ctrl_find_prop(ctrl, "X"), 0);
    double y = prop_num(ctrl_find_prop(ctrl, "Y"), 0);
    double w = prop_num(ctrl_find_prop(ctrl, "Largeur"), 0);
    double h = prop_num(ctrl_find_prop(ctrl, "Hauteur"), 0);

    /* Coord Y locale au bloc : ajouter l'origine du bloc parent.
     * X reste absolu (relatif a la marge gauche, comme tous les iLigne /
     * iCadre / iImprimeZone). Si on n'est pas dans un render bloc (cas
     * theorique : Etat libre sans DSL, label rendered standalone), on ne
     * decale pas. */
    if (g_in_block_render) {
        y += g_current_block_y_origin_mm;
    }

    /* Police : 3 modes possibles (du plus specifique au plus generique).
     *
     * Mode A — raccourcis sucre directs (canon OWL pratique) :
     *   PoliceNom / PoliceTaille / PoliceGras / PoliceItalique /
     *   PoliceSoulignée / PoliceBarrée. Le plus court à ecrire.
     *
     * Mode A' — descripteur composé Police (canon) :
     *   p est un Police; p..Nom = "Arial"; p..Taille = 14; ...
     *   Lu depuis ObjInstance fields. Equivalent runtime au mode A.
     *
     * Mode B — Police = entier (id deja cree par iCréePolice). Legacy. */
    const OwlPropertyInit* pNom    = ctrl_find_prop(ctrl, "PoliceNom");
    const OwlPropertyInit* pTaille = ctrl_find_prop(ctrl, "PoliceTaille");
    const OwlPropertyInit* pGras   = ctrl_find_prop(ctrl, "PoliceGras");
    const OwlPropertyInit* pItal   = ctrl_find_prop(ctrl, "PoliceItalique");
    const OwlPropertyInit* pSoul   = ctrl_find_prop(ctrl, "PoliceSoulignee");
    const OwlPropertyInit* pBarr   = ctrl_find_prop(ctrl, "PoliceBarree");

    bool modeA = (pNom || pTaille || pGras || pItal || pSoul || pBarr);
    bool fontApplied = false;

    if (modeA) {
        char famTmp[64];
        const char* family = prop_family_name(pNom, famTmp, sizeof(famTmp));
        if (!family) family = "";  /* default Qt */
        double size = prop_num(pTaille, 11);
        int style = 0;
        if (prop_bool(pGras))  style |= 1;
        if (prop_bool(pItal))  style |= 2;
        if (prop_bool(pSoul))  style |= 4;
        if (prop_bool(pBarr))  style |= 8;
        owl_pdf_font_create(d, ETAT_LABEL_FONT_ID, size, style, family);
        owl_pdf_font_use(d, ETAT_LABEL_FONT_ID);
        fontApplied = true;
    }

    if (!fontApplied) {
        const OwlPropertyInit* pPolice = ctrl_find_prop(ctrl, "Police");
        if (pPolice) {
            Value pv = prop_resolve(pPolice);
            /* Mode A' : descripteur composé Police (canon). */
            if (IS_INSTANCE(pv)) {
                ObjInstance* poly = AS_INSTANCE(pv);
                Value v;
                /* Helper inline : lire un sous-champ via accent-strip. */
                #define POL_GET(NAME) (table_get(&poly->fields, obj_copy_string(NAME, (int)strlen(NAME)), &v))
                const char* family = "";
                char famBuf[64] = {0};
                if (POL_GET("Nom") && IS_STRING(v)) {
                    family = AS_CSTRING(v);
                } else if (POL_GET("Nom") && IS_INTEGER(v)) {
                    /* Constante iPolice* (entier) — convertir en nom famille. */
                    int fid = (int)AS_INTEGER(v);
                    const char* mapped = family_name_from_int(fid);
                    if (mapped) {
                        strncpy(famBuf, mapped, sizeof(famBuf)-1);
                        family = famBuf;
                    }
                }
                double size = 11;
                if (POL_GET("Taille") && IS_INTEGER(v)) size = (double)AS_INTEGER(v);
                else if (POL_GET("Taille") && IS_REAL(v)) size = AS_REAL(v);
                int style = 0;
                if (POL_GET("Gras")     && IS_BOOL(v) && AS_BOOL(v)) style |= 1;
                if (POL_GET("Italique") && IS_BOOL(v) && AS_BOOL(v)) style |= 2;
                if ((POL_GET("Souligne") || POL_GET("Soulign\xc3\xa9")) && IS_BOOL(v) && AS_BOOL(v)) style |= 4;
                if ((POL_GET("Barre") || POL_GET("Barr\xc3\xa9")) && IS_BOOL(v) && AS_BOOL(v)) style |= 8;
                #undef POL_GET
                owl_pdf_font_create(d, ETAT_LABEL_FONT_ID, size, style, family);
                owl_pdf_font_use(d, ETAT_LABEL_FONT_ID);
                fontApplied = true;
            }
            /* Mode B : entier (id pre-cree). */
            else if (IS_INTEGER(pv)) {
                owl_pdf_font_use(d, (int)AS_INTEGER(pv));
                fontApplied = true;
            }
        }
    }

    /* Alignement combinable iGauche/Droite/CentreH + iHaut/Bas/CentreV.
     * V2 PoC : on lit un "Alignement" entier (bitmask). Sinon defaut 0. */
    int align = 0;
    const OwlPropertyInit* pAlign = ctrl_find_prop(ctrl, "Alignement");
    if (pAlign) {
        Value av = prop_resolve(pAlign);
        if (IS_INTEGER(av)) align = (int)AS_INTEGER(av);
    }

    /* Si Largeur/Hauteur non specifiees : zone degeneree, on imprime quand meme
     * a (x, y) en utilisant un grand rectangle pour eviter le clip a 0. */
    double x2 = (w > 0) ? x + w : x + 200;
    double y2 = (h > 0) ? y + h : y + 20;

    /*  (bug D) — CouleurFond de champ : rectangle plein derriere le
     * texte, AVANT owl_pdf_print_zone. Meme motif que le fond de bloc
     * (run_etat_block) : border < 0 = pas de bord, fill = couleur, brush plein.
     * Lu LIVE (instance puis decl) : une ecriture runtime `LIB..CouleurFond`
     * est honoree comme le libelle. < 0 (absente) = pas de fond peint → flux
     * identique a la baseline sans CouleurFond (non-regression). */
    int couleur_fond_champ = prop_color_live(uici, ctrl, "CouleurFond", -1);
    if (couleur_fond_champ >= 0) {
        owl_pdf_rect(d, x, y, x2, y2, 0, -1, couleur_fond_champ, 0, 0, 0);
    }

    owl_pdf_print_zone(d, text, x, y, x2, y2, align);

    render_etat_cadre(d, ctrl, x, y, x2, y2);
}

/* Imprime un sous-control Champ Image dans un bloc Etat.
 *
 * V1 : positionnement (X/Y/Largeur/Hauteur en mm) + source Image (chemin
 * de fichier OU instance Image en memoire) + Cadrage. Pas d'animation
 * ni d'event souris (PDF = sortie statique).
 *
 * Pour les Image en memoire (ObjImage), on sauve un PNG temporaire,
 * on le passe a owl_pdf_image, puis on supprime. C'est moins efficace
 * qu'un draw direct depuis le buffer mais reutilise toute l'infra
 * existante. Optimisation possible plus tard si besoin. */
static void render_etat_image(OwlPdfDoc* d, const OwlControlDecl* ctrl) {
    if (!d || !ctrl) return;

    double x = prop_num(ctrl_find_prop(ctrl, "X"), 0);
    double y = prop_num(ctrl_find_prop(ctrl, "Y"), 0);
    double w = prop_num(ctrl_find_prop(ctrl, "Largeur"), 0);
    double h = prop_num(ctrl_find_prop(ctrl, "Hauteur"), 0);

    if (g_in_block_render) y += g_current_block_y_origin_mm;

    /*  : le Cadre est une decoration de zone, independante de
     * l'image source. Il doit s'afficher dans TOUS les cas (image presente
     * ou non) et, quand une image est rendue, passer PAR-DESSUS elle (sinon
     * l'image recouvre la bordure). Zone deduite de X/Y/Largeur/Hauteur :
     * fallback grand rectangle si dimensions absentes (comme le libelle).
     * On trace le cadre :
     *   - juste avant chaque early-return (pas d'image source / save echoue),
     *   - apres owl_pdf_image dans le cas nominal. */
    double cx2 = (w > 0) ? x + w : x + 200;
    double cy2 = (h > 0) ? y + h : y + 20;

    /* Cadrage : 0=cadCentre (taille native, position fixe — owl_pdf_image
     * mode 0 + zoom 100), 1=cadEtire (mode 0 etire dans la zone),
     * 2=cadHomothetique (mode 2 = homothetique centre). */
    int cadrage = 0;
    const OwlPropertyInit* pCad = ctrl_find_prop(ctrl, "Cadrage");
    if (pCad) {
        Value cv = prop_resolve(pCad);
        if (IS_INTEGER(cv)) cadrage = (int)AS_INTEGER(cv);
    }
    int pdfMode = (cadrage == 2) ? 2 : 0;  /* 2 = homothetique centre */

    /* Lookup source. Le cadre est tracé sur chaque sortie anticipée (pas
     * d'image source / save echoue) pour rester une decoration de zone. */
    const OwlPropertyInit* pImg = ctrl_find_prop(ctrl, "Image");
    if (!pImg) { render_etat_cadre(d, ctrl, x, y, cx2, cy2); return; }
    Value iv = prop_resolve(pImg);

    char tmpPath[512] = {0};
    const char* path = NULL;

    if (IS_STRING(iv)) {
        path = AS_CSTRING(iv);
    } else if (stdlib_image_is_instance(iv)) {
        OwlImage* handle = stdlib_image_get_handle(iv);
        if (!handle) { render_etat_cadre(d, ctrl, x, y, cx2, cy2); return; }
        /* Tempfile PNG : nom unique base sur le pointeur instance + pid. */
        const char* tmpDir = getenv("TEMP");
        if (!tmpDir) tmpDir = getenv("TMP");
        if (!tmpDir) tmpDir = "/tmp";
        snprintf(tmpPath, sizeof(tmpPath), "%s/owl_etat_img_%p_%d.png",
                 tmpDir, (void*)AS_INSTANCE(iv), (int)getpid());
        if (!owl_image_save_png(handle, tmpPath)) {
            render_etat_cadre(d, ctrl, x, y, cx2, cy2);
            return;  /* Sauvegarde echouee — on skip silencieusement. */
        }
        path = tmpPath;
    }

    if (!path) { render_etat_cadre(d, ctrl, x, y, cx2, cy2); return; }

    /* cadCentre = mode 0 sans zone (taille native) si w/h=0, sinon etire.
     * Pour vrai "centré" non-deformé en V1 on utilise cadHomothetique (mode 2)
     * qui est l'option canonique canon pour ce cas. */
    double x2 = (w > 0) ? x + w : 0;
    double y2 = (h > 0) ? y + h : 0;
    owl_pdf_image(d, path, x, y, x2, y2, pdfMode, 100);

    /* Cadre PAR-DESSUS l'image (sinon l'image recouvre la bordure). */
    render_etat_cadre(d, ctrl, x, y, cx2, cy2);

    /* Cleanup tempfile. */
    if (tmpPath[0]) remove(tmpPath);
}

/*  — Champ CodeBarres dans un Etat : parite avec le Champ Image.
 * Le pipeline code-barres (zint, pur C, headless) produit deja une ObjImage ;
 * on la draine vers le PDF exactement comme render_etat_image (temp PNG ->
 * owl_pdf_image). Source : descripteur instance (prop par defaut CodeBarres)
 * ou props plates (TypeCodeBarres/Contenu/CorrectionErreur/Option). Homothetique
 * force (mode 2) : un code-barres ne se deforme pas. */
extern Value owl_codebarre_image_value_from_params(int64_t cbType, int64_t ecc,
                                                   int64_t options,
                                                   const char* contenu, int contenuLen,
                                                   int largeurMax, int hauteurMax);
extern Value owl_codebarre_image_value_from_instance(ObjInstance* cb);

static void render_etat_codebarre(OwlPdfDoc* d, const OwlControlDecl* ctrl) {
    if (!d || !ctrl) return;

    double x = prop_num(ctrl_find_prop(ctrl, "X"), 0);
    double y = prop_num(ctrl_find_prop(ctrl, "Y"), 0);
    double w = prop_num(ctrl_find_prop(ctrl, "Largeur"), 0);
    double h = prop_num(ctrl_find_prop(ctrl, "Hauteur"), 0);
    if (g_in_block_render) y += g_current_block_y_origin_mm;

    double cx2 = (w > 0) ? x + w : x + 200;
    double cy2 = (h > 0) ? y + h : y + 20;

    /* Source 1 : descripteur instance (prop par defaut CodeBarres/BarCodes). */
    Value imgV = NIL_VAL;
    const OwlPropertyInit* pDesc = ctrl_find_prop(ctrl, "CodeBarres");
    if (!pDesc) pDesc = ctrl_find_prop(ctrl, "BarCodes");
    if (pDesc) {
        Value dv = prop_resolve(pDesc);
        if (IS_INSTANCE(dv))
            imgV = owl_codebarre_image_value_from_instance(AS_INSTANCE(dv));
    }
    /* Source 2 : props plates (TypeCodeBarres/Contenu/CorrectionErreur/Option). */
    if (!stdlib_image_is_instance(imgV)) {
        int64_t cbType = OWL_CB_QRCODE, ecc = OWL_QR_ECC_M, opts = 0;
        Value tv = prop_resolve(ctrl_find_prop(ctrl, "TypeCodeBarres"));
        if (!IS_INTEGER(tv)) tv = prop_resolve(ctrl_find_prop(ctrl, "TypeBarCode"));
        if (IS_INTEGER(tv)) cbType = AS_INTEGER(tv);
        Value ev = prop_resolve(ctrl_find_prop(ctrl, "CorrectionErreur"));
        if (!IS_INTEGER(ev)) ev = prop_resolve(ctrl_find_prop(ctrl, "CorrectionError"));
        if (IS_INTEGER(ev)) ecc = AS_INTEGER(ev);
        Value ov = prop_resolve(ctrl_find_prop(ctrl, "Option"));
        if (IS_INTEGER(ov)) opts = AS_INTEGER(ov);
        char cbuf[512];
        const char* contenu = prop_str(ctrl_find_prop(ctrl, "Contenu"), cbuf, sizeof(cbuf));
        if (!contenu) contenu = prop_str(ctrl_find_prop(ctrl, "Content"), cbuf, sizeof(cbuf));
        if (contenu && *contenu)
            imgV = owl_codebarre_image_value_from_params(
                cbType, ecc, opts, contenu, (int)strlen(contenu), 0, 0);
    }

    /* Pas d'image produite (contenu vide/invalide) : juste le cadre de zone. */
    if (!stdlib_image_is_instance(imgV)) {
        render_etat_cadre(d, ctrl, x, y, cx2, cy2);
        return;
    }

    /* ObjImage fraiche non referencee ailleurs -> root pendant save + draw. */
    vm_push(imgV);
    OwlImage* handle = stdlib_image_get_handle(imgV);
    char tmpPath[512] = {0};
    if (handle) {
        const char* tmpDir = getenv("TEMP");
        if (!tmpDir) tmpDir = getenv("TMP");
        if (!tmpDir) tmpDir = "/tmp";
        snprintf(tmpPath, sizeof(tmpPath), "%s/owl_etat_cb_%p_%d.png",
                 tmpDir, (void*)AS_INSTANCE(imgV), (int)getpid());
        if (owl_image_save_png(handle, tmpPath)) {
            double x2 = (w > 0) ? x + w : 0;
            double y2 = (h > 0) ? y + h : 0;
            owl_pdf_image(d, tmpPath, x, y, x2, y2, 2, 100);
        } else {
            tmpPath[0] = '\0';
        }
    }
    vm_pop();

    /* Cadre PAR-DESSUS l'image (decoration de zone, comme le Champ Image). */
    render_etat_cadre(d, ctrl, x, y, cx2, cy2);
    if (tmpPath[0]) remove(tmpPath);
}

/* Trigger l'event 'AvantImpression' (canon) sur un sous-control Etat
 * avant son rendu PDF. Permet a l'utilisateur de modifier les props du
 * control au dernier moment (ex: changer le Libellé selon une variable
 * d'iteration). */
static void trigger_avant_impression(const OwlControlDecl* c) {
    if (!c) return;
    for (int i = 0; i < c->event_count; i++) {
        const OwlEventBlock* e = &c->events[i];
        if (strcasecmp(e->name, "AvantImpression") == 0 ||
            strcasecmp(e->name, "BeforePrint") == 0) {
            if (e->impl) {
                /* Even-style call : MaFenetre + MonChamp en NIL_VAL.
                 * Le user peut acceder aux globales/variables Etat. */
                vm_push(OBJ_VAL(e->impl));
                vm_push(NIL_VAL);   /* MaFenetre */
                vm_push(NIL_VAL);   /* MonChamp */
                vm_call_reentrant(OBJ_VAL(e->impl), 2);
                vm_pop();           /* return value */
            }
            return;
        }
    }
}

/* Imprime tous les controls d'un sub-bloc apres l'execution de son body
 * code canon. AvantImpression event de chaque control est invoque juste
 * avant son rendu (canon). */
static void render_etat_block_controls(OwlPdfDoc* d, const OwlEtatBlockDecl* b) {
    if (!d || !b) return;
    for (int i = 0; i < b->control_count; i++) {
        OwlControlDecl* c = b->controls[i];
        if (!c) continue;
        trigger_avant_impression(c);
        if (c->kind == OWL_CONTROL_LABEL) {
            render_etat_label(d, c);
        } else if (c->kind == OWL_CONTROL_IMAGE) {
            render_etat_image(d, c);
        } else if (c->kind == OWL_CONTROL_CODEBARRE) {
            render_etat_codebarre(d, c);
        }
        /* V2.x : Calcule, Predefini, etc. */
    }
}

/* Execute le top-level (code d'init de niveau 0) de chaque sous-champ de
 * tous les blocs d'un Etat : 1x a l'ouverture de l'Etat, apres l'Ouverture
 * de l'Etat et avant le 1er rendu de bloc, dans l'ordre d'ecriture (blocs
 * puis controls dans chaque bloc). Le top-level manipule l'UI (props par
 * nom) avant tout rendu.
 *
 *  — push event_sender_stack avec le sous-champ uici autour de
 * chaque top-level pour que MoiMême resolve au sous-champ courant. L'ordre
 * d'iteration matche exactement la construction de etat_inst->controls[]
 * dans fn_i_imprime_etat (boucle bi, ci identique) : on peut donc indexer
 * en parallele. */
static void run_etat_controls_top_level(const OwlEtatDecl* etat,
                                        ObjEtatInstance* etat_inst) {
    (void)etat_inst;  /*  — actuellement inutilise : on push le
                       * ObjUiControlRef global plutot que l'instance, par
                       * symetrie stricte avec open_window_instance. Garde
                       * le param pour evolution future (push instance si
                       * OP_STRUCT_GET/SET gere IS_UI_CONTROL_INSTANCE). */
    if (!etat) return;
    for (int b = 0; b < etat->block_count; b++) {
        const OwlEtatBlockDecl* blk = &etat->blocks[b];
        for (int i = 0; i < blk->control_count; i++) {
            const OwlControlDecl* c = blk->controls[i];
            if (!c) continue;
            if (c->top_level) {
                /*  — sender = ObjUiControlRef pose par le compilo
                 * dans vm.globals[<name>] (symetrie open_window_instance).
                 * resolve_ui_ref consultera vm.etat_stack (deja push par
                 * fn_i_imprime_etat) pour retomber sur l'ObjUiControlInstance
                 * du sous-champ. */
                Value sender = owl_moimeme_widget_value_for_name(c->name);
                vm_event_sender_push(sender);
                call_etat_block_fn(c->top_level);
                vm_event_sender_pop();
            }
        }
    }
}

/* Forward decl : etat_page_break_with_hook utilise plus bas par run_etat_block.
 * Definition complete apres g_current_etat / g_in_page_hook. */
static void etat_page_break_with_hook(OwlPdfDoc* d);

/* Lookup d'une prop boolean dans les props d'un bloc. dflt si absente. */
/* Lookup d'une prop de bloc par nom, bilingue FR/EN via le catalogue (meme
 * chokepoint que ctrl_find_prop, cf. ). owl_etat_block_catalog_find
 * matche nameFR ET nameEN -> "Height" et "Hauteur" resolvent la meme def. */
static const OwlPropertyInit* block_find_prop(const OwlEtatBlockDecl* b, const char* name) {
    if (!b || !name) return NULL;
    const OwlControlPropDef* target = owl_etat_block_catalog_find(name);
    for (int i = 0; i < b->property_count; i++) {
        const char* stored = b->properties[i].name;
        if (strcasecmp(stored, name) == 0) return &b->properties[i];
        if (target && owl_etat_block_catalog_find(stored) == target)
            return &b->properties[i];
    }
    return NULL;
}

static bool block_prop_bool(const OwlEtatBlockDecl* b, const char* name, bool dflt) {
    const OwlPropertyInit* p = block_find_prop(b, name);
    if (!p) return dflt;
    if (p->is_constant && IS_BOOL(p->const_value)) return AS_BOOL(p->const_value);
    return dflt;
}

/* Lookup d'une prop numerique dans les props d'un bloc. dflt si absente. */
static double block_prop_num(const OwlEtatBlockDecl* b, const char* name, double dflt) {
    const OwlPropertyInit* p = block_find_prop(b, name);
    if (!p || !p->is_constant) return dflt;
    if (IS_INTEGER(p->const_value)) return (double)AS_INTEGER(p->const_value);
    if (IS_REAL(p->const_value))    return AS_REAL(p->const_value);
    return dflt;
}

/* Wrapper : appelle le body code canon puis rend les controls WYSIWYG. Respecte
 * les props canon :
 *  - Visible = Faux : bloc skip integralement
 *  - SautDePageApresBloc = Vrai : page break apres l'impression
 *
 * Sécable : prop acceptee au compile, effet runtime V2.x (necessite gestion
 * d'espace vertical, pas dispo en V2 PoC). Autres props pagination
 * (repetition/conservation/reset) seront introduites avec iImprimeEtat. */
static void run_etat_block(OwlPdfDoc* d, const OwlEtatBlockDecl* b) {
    if (!b) return;
    /* Respect Visible : Faux = skip total. */
    if (!block_prop_bool(b, "Visible", true)) return;

    /* Snapshot Y au debut du bloc pour render les widgets WYSIWYG en local.
     * Le body code canon peut ensuite faire avancer le curseur (iImprime,
     * iSauteLigne) — les widgets WYSIWYG eux restent en local du bloc. */
    double y_start = owl_pdf_get_cursor_y(d);
    double declared_h = block_prop_num(b, "Hauteur", 0);

    /* Dessine d'abord le fond CouleurFond du bloc en absolu (canon).
     * Rectangle plein sur toute la largeur utilisable, hauteur
     * du bloc. Ne dessine rien si CouleurFond absente ou < 0. */
    int couleur_fond = -1;
    {
        const OwlPropertyInit* p = block_find_prop(b, "CouleurFond");
        if (p && p->is_constant && IS_INTEGER(p->const_value))
            couleur_fond = (int)AS_INTEGER(p->const_value);
    }
    if (couleur_fond >= 0 && declared_h > 0) {
        double page_w = owl_pdf_page_width_mm(d);
        /* fill_color_bgr=couleur_fond, border<0=pas de bord, brush=plein. */
        owl_pdf_rect(d, 0, y_start, page_w, y_start + declared_h,
                     0, -1, couleur_fond, 0, 0, 0);
    }

    double saved_origin = g_current_block_y_origin_mm;
    int    saved_flag   = g_in_block_render;
    g_current_block_y_origin_mm = y_start;
    g_in_block_render = 1;

    if (b->body) call_etat_block_fn(b->body);

    /*  — degel des expressions PROPRIETES : a chaque rendu du bloc, une
     * prop de sous-champ declaree comme expression (`Libelle = sVal`) se
     * re-evalue avec la valeur COURANTE des variables, au lieu d'etre gelee a
     * la 1re resolution (owl_etat_resolve_control_props, 1x par iImprimeEtat).
     *
     * Le render lit le casier headless store-first : ce casier
     * contient AUSSI la valeur seedee de l'expression (, necessaire aux
     * lectures `LIB..Libelle` par code). Il faut donc re-seeder le casier apres
     * chaque re-evaluation — SANS ecraser un override imperatif `..prop =` pose
     * par l'utilisateur ( : l'imperatif PRIME toujours).
     *
     * Override distingue par le FAIT d'ecriture, jamais par la valeur : une prop
     * ecrite imperativement est marquee override dans l'instance (,
     * mark_override cote vm.c). is_override(prop) => on ne touche pas (sticky,
     * meme si la valeur coincide avec l'expression). Sinon on re-seede la valeur
     * fraiche via le setter NON marquant. Litteraux purs (eval == NULL) :
     * ignores, rendu statique inchange. */
    for (int ci = 0; ci < b->control_count; ci++) {
        const OwlControlDecl* c = b->controls[ci];
        if (!c) continue;
        ObjUiControlInstance* uici = etat_inst_for_ctrl(c);
        for (int pi = 0; pi < c->property_count; pi++) {
            OwlPropertyInit* p = &c->properties[pi];
            if (!p->eval) continue;  /* literal pur : jamais gele, jamais re-seede */
            owl_etat_prop_reeval(p);  /* const_value <- valeur courante */
            if (uici && !obj_ui_control_instance_is_override(uici, p->name)) {
                obj_ui_control_instance_headless_set(uici, p->name, p->const_value);
            }
        }
    }
    render_etat_block_controls(d, b);

    g_current_block_y_origin_mm = saved_origin;
    g_in_block_render = saved_flag;

    /* Avance le curseur de la hauteur du bloc (canon : chaque bloc
     * occupe une bande verticale de Hauteur mm sur la page, et le bloc
     * suivant commence en-dessous). Sans ca, tous les blocs s'imprimeraient
     * a la meme position absolue Y et se chevaucheraient.
     *
     * Note : le body canon peut deja avoir avance le curseur (iImprime,
     * iSauteLigne...). On respecte la position max entre :
     *   - y_start + Hauteur (espace reserve par le bloc)
     *   - cursor_y courant   (position posee par le body)
     * Ainsi un Corps avec body iImprime() qui depasse Hauteur ne se fait
     * pas tronquer artificiellement. */
    double y_end_block = y_start + declared_h;
    double y_now = owl_pdf_get_cursor_y(d);
    double y_after = y_now > y_end_block ? y_now : y_end_block;
    owl_pdf_set_cursor_y(d, y_after);

    /* Respect SautDePageApresBloc : trigger un saut apres impression
     * (avec hook multi-page si dans un Etat). */
    if (block_prop_bool(b, "SautDePageApresBloc", false)) {
        etat_page_break_with_hook(d);
    }
}

/* Etat courant en cours d'impression. Pose par fn_i_imprime_etat au debut,
 * reset a NULL a la fin. Utilise par fn_i_imprime_bloc pour resoudre le nom
 * du bloc demande sans repasser par vm_etat_lookup. */
static const OwlEtatDecl* g_current_etat = NULL;

/* Garde-fou anti-recursion pour le hook iSautePage : quand le hook s'execute
 * (BasDePage -> saut -> HautDePage), un appel iSautePage fait depuis le
 * code de BasDePage/HautDePage doit faire un saut "brut" sans relancer le
 * hook (sinon recursion infinie). */
static bool g_in_page_hook = false;

/* Hook multi-page : si on est dans un Etat actif, un saut de page replay
 * automatiquement BasDePage (page courante) -> page_break -> HautDePage
 * (nouvelle page). Sinon saut simple. */
static void etat_page_break_with_hook(OwlPdfDoc* d) {
    if (!d) return;
    if (!g_current_etat || g_in_page_hook) {
        owl_pdf_new_page(d);
        return;
    }
    g_in_page_hook = true;
    const OwlEtatBlockDecl* bp = owl_etat_decl_find_canon_block(g_current_etat, OWL_ETAT_BLOCK_BASPAGE);
    if (bp) run_etat_block(d, bp);
    owl_pdf_new_page(d);
    const OwlEtatBlockDecl* hp = owl_etat_decl_find_canon_block(g_current_etat, OWL_ETAT_BLOCK_HAUTPAGE);
    if (hp) run_etat_block(d, hp);
    g_in_page_hook = false;
}

/* Convertit une constante de format de page en dimensions mm (largeur, hauteur)
 * en orientation portrait. Pour paysage, le caller echange w<->h. */
static void etat_format_dimensions(int format_const, double* w_mm, double* h_mm) {
    switch (format_const) {
        case OWL_ETAT_FORMAT_A3:     *w_mm = 297;  *h_mm = 420;   break;
        case OWL_ETAT_FORMAT_A5:     *w_mm = 148;  *h_mm = 210;   break;
        case OWL_ETAT_FORMAT_LETTER: *w_mm = 215.9; *h_mm = 279.4; break;
        case OWL_ETAT_FORMAT_LEGAL:  *w_mm = 215.9; *h_mm = 355.6; break;
        case OWL_ETAT_FORMAT_A4:
        default:                     *w_mm = 210;  *h_mm = 297;   break;
    }
}

/* Applique les PROPRIETES Format/Orientation/MargeXxx du decl Etat sur le
 * document PDF courant. */
/* Lookup d'une prop de conteneur Etat par nom, bilingue FR/EN via le catalogue
 * (meme chokepoint que ctrl_find_prop/block_find_prop, cf. ).
 * owl_etat_catalog_find matche nameFR ET nameEN -> "TopMargin" et "MargeHaut"
 * resolvent la meme def. */
static const OwlPropertyInit* etat_find_prop(const OwlEtatDecl* etat, const char* name) {
    if (!etat || !name) return NULL;
    const OwlControlPropDef* target = owl_etat_catalog_find(name);
    for (int i = 0; i < etat->property_count; i++) {
        const char* stored = etat->properties[i].name;
        if (strcasecmp(stored, name) == 0) return &etat->properties[i];
        if (target && owl_etat_catalog_find(stored) == target)
            return &etat->properties[i];
    }
    return NULL;
}

static void apply_etat_properties(OwlPdfDoc* d, const OwlEtatDecl* etat) {
    if (!d || !etat) return;
    int format = OWL_ETAT_FORMAT_A4;
    int orient = OWL_ETAT_ORIENT_PORTRAIT;
    const OwlPropertyInit* p;
    if ((p = etat_find_prop(etat, "Format")) && p->is_constant && IS_INTEGER(p->const_value))
        format = (int)AS_INTEGER(p->const_value);
    if ((p = etat_find_prop(etat, "Orientation")) && p->is_constant && IS_INTEGER(p->const_value))
        orient = (int)AS_INTEGER(p->const_value);
    double mh = 0, mb = 0, mg = 0, md = 0;
    bool hasMarges = false;
    if ((p = etat_find_prop(etat, "MargeHaut"))   && p->is_constant) { mh = arg_num(p->const_value, 0); hasMarges = true; }
    if ((p = etat_find_prop(etat, "MargeBas"))    && p->is_constant) { mb = arg_num(p->const_value, 0); hasMarges = true; }
    if ((p = etat_find_prop(etat, "MargeGauche")) && p->is_constant) { mg = arg_num(p->const_value, 0); hasMarges = true; }
    if ((p = etat_find_prop(etat, "MargeDroite")) && p->is_constant) { md = arg_num(p->const_value, 0); hasMarges = true; }
    double w = 210, h = 297;
    etat_format_dimensions(format, &w, &h);
    if (orient == OWL_ETAT_ORIENT_PAYSAGE) {
        double tmp = w; w = h; h = tmp;
    }
    owl_pdf_set_page_size_mm(d, w, h);
    if (hasMarges) owl_pdf_set_margins(d, mh, mb, mg, md);
}

/* iImprimeEtat(<Etat> [, <param1>, <param2>, ...]) — canon.
 *
 * Execute un Etat declare via 'ETAT_X est un Etat(<params>) ... FIN'.
 * Necessite qu'iDestination ait ete appele avant (le doc PDF doit exister).
 * L'utilisateur appelle iFinImprime() apres pour finaliser le PDF.
 *
 * Ordre d'execution :
 *   1. Inject params dans globales
 *   2. Apply PROPRIETES (Format/Orientation/Marges)
 *   3. Ouverture (top-level code de l'Etat)
 *   4. EnTeteDocument
 *   5. HautDePage
 *   6. PROCEDURE Lecture (slot pour code de chargement de donnees, 1 fois)
 *   7. PROCEDURE Filtre  (slot pour pre-filtrage, 1 fois)
 *   8. Corps
 *   9. BasDePage
 *  10. FinDeDocument
 *  11. PROCEDURE Fermeture
 *
 * Note OWL : pas de databinding magique automatique. Lecture/Filtre sont
 * juste des slots dans la sequence. Si le dev veut repeter le Corps, il
 * appelle iImprimeBloc("nomCorps") manuellement depuis Lecture. */
static Value fn_i_imprime_etat(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprimeEtat");
    if (!d) return BOOL_VAL(false);
    if (argCount < 1) {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "iImprimeEtat",
                              "native", "iImprimeEtat",
                              "arg", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "iImprimeEtat",
                              "native", "iImprimeEtat",
                              "arg", "1",
                              "expected", "chaine",
                              "actual", "autre",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* etatName = AS_CSTRING(args[0]);
    const OwlEtatDecl* etat = vm_etat_lookup(etatName);
    if (!etat) {
        owl_emit_native_error(E_NATIVE_PDF_ETAT_NOT_FOUND, "iImprimeEtat",
                              "native", "iImprimeEtat",
                              "name", etatName,
                              NULL);
        return BOOL_VAL(false);
    }

    /* Inject les valeurs des params utilisateur dans les globales declarees
     * lors du parse. Itere sur TOUS les params declares : arg fourni -> valeur
     * fournie ; sinon si le param a une valeur par defaut -> le
     * defaut ; sinon -> NIL_VAL (reset explicite pour ne pas heriter d'une
     * valeur laissee par une impression precedente). */
    int provided = argCount - 1;
    for (int i = 0; i < etat->param_count; i++) {
        Value v;
        if (i < provided)                    v = args[i + 1];
        else if (etat->params[i].has_default) v = etat->params[i].default_value;
        else                                 v = NIL_VAL;
        /*  : delegue au canon obj_globals_set. */
        obj_globals_set(etat->params[i].name, v);
    }

    /*  - Resout les eval dynamiques + fusionne les cascades
     * homonymes (Cadre..Epaisseur + Cadre..Couleur sur 2 lignes) sur les
     * 3 niveaux de l'Etat AVANT tout acces aux props. Symetrique du fix
     * Fenetre  dans build_window(). Sans ca, ctrl_find_prop
     * (lookup first-match) ne voyait que la 1ere cascade et le rendu PDF
     * perdait les autres sous-champs. Cast const-off : l'IR Etat est
     * stocke const dans vm.etats mais owl_etat_prepare_props mute les
     * const_value des props in-place (memes regles que build_window). */
    owl_etat_prepare_props((OwlEtatDecl*)etat);

    /* Applique format/orientation/marges sur le doc PDF. */
    apply_etat_properties(d, etat);

    /* Pose l'Etat courant pour iImprimeBloc(). Restoré a la fin (idiome
     * scope-guard manuel — la VM est mono-thread donc OK). */
    const OwlEtatDecl* prev_current = g_current_etat;
    g_current_etat = etat;

    /*  lot D — materialisation de l'Etat vivant. Cree une
     * ObjEtatInstance + une ObjUiControlInstance par sous-champ de chaque
     * bloc, seede les props declaratives dans le headless_store de chaque
     * sous-champ (modele exact  cote champ de fenetre). Push sur
     * vm.etat_stack : resolve_ui_ref consultera ce stack en repli quand
     * un proxy ObjUiControlRef d'un sous-champ d'Etat est dispatche.
     *
     * Ordre obligatoire : push AVANT Ouverture + top-levels. Pop AVANT
     * return (succes ou erreur). owl_etat_prepare_props ci-dessus a deja
     * resolu les eval dynamiques + fusionne les cascades homonymes : on
     * peut seeder direct depuis decl->properties[]. */
    ObjEtatInstance* etat_inst = obj_new_etat_instance((void*)etat);
    vm_push(OBJ_VAL(etat_inst));  /* GC root pendant la construction */
    for (int bi = 0; bi < etat->block_count; bi++) {
        const OwlEtatBlockDecl* blk = &etat->blocks[bi];
        for (int ci = 0; ci < blk->control_count; ci++) {
            OwlControlDecl* c = blk->controls[ci];
            if (!c) continue;
            ObjUiControlInstance* uici = obj_new_ui_control_instance(c, NULL);
            obj_etat_instance_add_control(etat_inst, uici);
            /* Seed des props declaratives dans le store. Les cascades
             * homonymes sont regroupees au compile-time ; le
             * filtre "premiere occurrence" reste defensif pour les
             * doublons litteraux (Titre = "A" ; Titre = "B"). */
            for (int pi = 0; pi < c->property_count; pi++) {
                OwlPropertyInit* p = &c->properties[pi];
                if (!p->is_constant) continue;
                bool seen = false;
                for (int k = 0; k < pi; k++) {
                    if (strcasecmp(c->properties[k].name, p->name) == 0) {
                        seen = true; break;
                    }
                }
                if (seen) continue;
                vm_push(p->const_value);  /* GC root pendant le set */
                obj_ui_control_instance_headless_set(uici, p->name,
                                                      p->const_value);
                vm_pop();
            }
        }
    }
    vm_etat_stack_push(etat_inst);

    /*  — sender de l'Etat pour MoiMême dans le top-level Etat,
     * Lecture/Filtre/Fermeture, et toute proc Etat sans contexte champ.
     * Symetrique a `winSender` dans open_window_instance. La Value pushee
     * est l'ObjEtatInstance lui-meme : OP_MOIMEME push cette Value telle
     * quelle, le programme peut donc constater MoiMême != Nul (le test
     * d'ErreurDetectee passe). Les acces MoiMême..Prop sur un Etat seront
     * traites par le code generique de struct_get (V2.x : surface props
     * Etat exposees). Pour les top-levels sous-champs, run_etat_controls_
     * top_level remplace ce sender par le sous-champ courant. */
    Value etatSender = OBJ_VAL(etat_inst);

    /*  — liaison des PROCEDURE de l'Etat (user_procs) AVANT toute
     * execution. Modele plat : toute PROCEDURE declaree dans l'Etat (top-level
     * OU dans un bloc/widget) est une globale de l'Etat, appelable depuis
     * n'importe ou DANS l'Etat. La resolution compile-time d'un appel bare
     * `Calcule(...)` dans une section/widget/proc passe par l'alias DSL
     * `<etat>_Calcule` (scope a l'Etat) ; il faut donc lier cette globale
     * qualifiee a la vraie fn ici, avant le top-level et les sections.
     * Parite stricte avec build_window (stdlib_ui.c). La fermeture externe
     * tient : l'alias est scope a l'Etat, le nom qualifie n'est jamais
     * reformable depuis un script/fenetre. */
    for (int i = 0; i < etat->user_proc_count; i++) {
        char qname[256];
        snprintf(qname, sizeof(qname), "%s_%s", etat->name,
                 etat->user_procs[i].name);
        obj_globals_set(qname, OBJ_VAL(etat->user_procs[i].impl));
    }

    /* 1. Ouverture (top-level code de l'Etat = init implicite, ).
     *  — l'Etat est endormi jusqu'a la 1re impression : son top-level
     * (stocke sur le decl par etat_declaration au lieu d'un auto-call eager au
     * boot) tourne ICI, mais UNE seule fois pour toute la vie du process. Le
     * flag top_level_done garantit l'init UNIQUE meme sur impressions multiples
     * (contrat dev : un Etat reimprime ne se reinitialise pas — pas de re-fire
     * par iImprimeEtat). Cast const-off comme owl_etat_prepare_props plus haut :
     * l'IR Etat est stocke const dans vm.etats mais le cycle de vie le mute.
     *
     * NB : aucun event "Ouverture" n'est enregistre cote etat_declaration (le
     * code top-level vit dans decl->top_level, pas dans events[]). Le lookup ci
     * dessous reste defensif pour une eventuelle source future. */
    vm_event_sender_push(etatSender);
    if (etat->top_level && !etat->top_level_done) {
        ((OwlEtatDecl*)etat)->top_level_done = true;
        call_etat_block_fn(etat->top_level);
    }
    const OwlEventBlock* ev = owl_etat_decl_find_event(etat, "Ouverture");
    if (ev && ev->impl) call_etat_block_fn(ev->impl);
    vm_event_sender_pop();

    /*  — resout les props dynamiques des sous-champs MAINTENANT, apres
     * le top-level Etat (owl_etat_prepare_props plus haut n'a resolu que l'Etat
     * + les blocs). Une prop `Image = imgLogo` referencant une globale top-level
     * Etat (ETAT_X$imgLogo) n'est resoluble qu'a ce point ; la resolution eager
     * avant le top-level echouait sur "Variable inconnue" (parite ). */
    owl_etat_resolve_control_props((OwlEtatDecl*)etat);

    /*  — re-seede dans le casier des instances les props CONTROLE a
     * evaluateur (chaine multilingue `{langueXxx: "..."}`, couleur inline,
     * expression dynamique) qui viennent seulement d'etre resolues.
     * La materialisation ci-dessus tourne AVANT le top-level (le casier doit
     * exister pour resolve_ui_ref), or  a differe la resolution des
     * props de sous-champs a APRES le top-level : la boucle de seed ne voyait
     * donc que les litteraux purs (is_constant des le compile), laissant le
     * slot multilingue vide et une lecture `LIB..Libellé` sur le default
     * (chaine vide). Une fois const_value resolu, on l'injecte dans le store
     * (parite stricte avec le seed post-resolution du chemin Fenetre). */
    for (int i = 0; i < etat_inst->control_count; i++) {
        ObjUiControlInstance* uici = etat_inst->controls[i];
        if (!uici || !uici->decl) continue;
        OwlControlDecl* c = uici->decl;
        for (int pi = 0; pi < c->property_count; pi++) {
            OwlPropertyInit* p = &c->properties[pi];
            if (!p->eval) continue;        /* litteraux purs deja seedes */
            if (!p->is_constant) continue; /* non resolu (erreur eval) : skip */
            bool seen = false;
            for (int k = 0; k < pi; k++) {
                if (strcasecmp(c->properties[k].name, p->name) == 0) {
                    seen = true; break;
                }
            }
            if (seen) continue;  /* premiere occurrence gagne (parite seed) */
            vm_push(p->const_value);  /* GC root pendant le set */
            obj_ui_control_instance_headless_set(uici, p->name,
                                                 p->const_value);
            vm_pop();
        }
    }

    /* 1.b Top-level de chaque sous-champ : 1x a l'ouverture, apres
     * l'Ouverture de l'Etat, avant le 1er rendu de bloc, ordre d'ecriture. */
    run_etat_controls_top_level(etat, etat_inst);

    /* 2. EnTeteDocument + HautDePage (1ere page). */
    const OwlEtatBlockDecl* b;
    b = owl_etat_decl_find_canon_block(etat, OWL_ETAT_BLOCK_ENTETEDOC);
    if (b) run_etat_block(d, b);
    b = owl_etat_decl_find_canon_block(etat, OWL_ETAT_BLOCK_HAUTPAGE);
    if (b) run_etat_block(d, b);

    /* 3. PROCEDURE Lecture (slot canon, 1x). Si le dev veut repeter
     *    le Corps, il appelle iImprimeBloc("nomCorps") depuis Lecture.
     *     — push etatSender pour que MoiMême resolve a l'Etat. */
    ev = owl_etat_decl_find_event(etat, "Lecture");
    if (!ev) ev = owl_etat_decl_find_event(etat, "Read");
    if (ev && ev->impl) {
        vm_event_sender_push(etatSender);
        call_etat_block_fn(ev->impl);
        vm_event_sender_pop();
    }

    /* 4. PROCEDURE Filtre (slot, 1x). Pas d'effet builtin sur le Corps —
     *    si le dev veut filtrer, il fait son SI dans Lecture/Filtre et
     *    appelle iImprimeBloc conditionnellement.
     *     — push etatSender (idem Lecture). */
    ev = owl_etat_decl_find_event(etat, "Filtre");
    if (!ev) ev = owl_etat_decl_find_event(etat, "Filter");
    if (ev && ev->impl) {
        vm_event_sender_push(etatSender);
        call_etat_block_fn(ev->impl);
        vm_event_sender_pop();
    }

    /* 5. Corps (auto, 1x). Saute si Lecture/Filtre l'a deja imprime
     *    manuellement via iImprimeBloc — V2 MVP : pas de tracking, le
     *    Corps auto s'imprime quand meme. Le dev qui veut full control
     *    peut declarer un bloc Iteration nomme et l'appeler explicitement
     *    depuis Lecture, sans declarer de bloc Corps. */
    b = owl_etat_decl_find_canon_block(etat, OWL_ETAT_BLOCK_CORPS);
    if (b) run_etat_block(d, b);

    /* 6. FinDeDocument (canon : a la fin du flux Corps, position courante). */
    b = owl_etat_decl_find_canon_block(etat, OWL_ETAT_BLOCK_FINDOC);
    if (b) run_etat_block(d, b);

    /* 7. BasDePage : colle au bas de la page (canon :
     *    "Par défaut, le bloc Pied de page est imprimé au bas de chaque
     *    page"). On positionne le curseur juste avant la marge basse,
     *    moins la hauteur du bloc, pour qu'il s'imprime contre le pied.
     *    Si le curseur est deja au-dela (cas pathologique : Corps tres
     *    long), on ne remonte pas — le BasDePage s'imprime juste apres. */
    b = owl_etat_decl_find_canon_block(etat, OWL_ETAT_BLOCK_BASPAGE);
    if (b) {
        double bloc_h = block_prop_num(b, "Hauteur", 25);
        double page_h = owl_pdf_page_height_mm(d);
        double y_target = page_h - bloc_h;
        double y_now = owl_pdf_get_cursor_y(d);
        if (y_target > y_now) owl_pdf_set_cursor_y(d, y_target);
        run_etat_block(d, b);
    }

    /* 7. PROCEDURE Fermeture.  — push etatSender (idem Lecture). */
    ev = owl_etat_decl_find_event(etat, "Fermeture");
    if (!ev) ev = owl_etat_decl_find_event(etat, "Closing");
    if (ev && ev->impl) {
        vm_event_sender_push(etatSender);
        call_etat_block_fn(ev->impl);
        vm_event_sender_pop();
    }

    /*  lot D — pop de l'instance d'Etat. APRES Fermeture (le code
     * utilisateur peut encore acceder aux sous-champs par nom). vm_pop
     * supprime le root sur la pile VM ; le GC collecte l'instance et ses
     * sous-champs au prochain cycle, sauf si referencees ailleurs. */
    if (vm_etat_stack_top() == etat_inst) vm_etat_stack_pop();
    vm_pop();

    g_current_etat = prev_current;
    return BOOL_VAL(true);
}

/* iImprimeBloc(<NomBloc>) — canon.
 *
 * Imprime manuellement un bloc nomme de l'Etat courant. Utilise typiquement
 * dans PROCEDURE Lecture pour repeter le Corps sur N enregistrements, ou
 * dans Ouverture pour declencher un bloc Iteration libre.
 *
 * Retourne Vrai si le bloc a ete trouve et imprime, Faux sinon. */
static Value fn_i_imprime_bloc(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprimeBloc");
    if (!d) return BOOL_VAL(false);
    OWL_REQUIRE_ARG("iImprimeBloc", "1", 0, IS_STRING(args[0]),
                    "chaîne", BOOL_VAL(false));
    if (!g_current_etat) {
        owl_emit_native_error(E_NATIVE_PDF_NOT_IN_ETAT, "iImprimeBloc",
                              "native", "iImprimeBloc",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* blocName = AS_CSTRING(args[0]);
    const OwlEtatBlockDecl* b = owl_etat_decl_find_block_by_name(g_current_etat, blocName);
    if (!b) {
        owl_emit_native_error(E_NATIVE_PDF_BLOC_NOT_FOUND, "iImprimeBloc",
                              "native", "iImprimeBloc",
                              "bloc", blocName,
                              "etat", g_current_etat->name,
                              NULL);
        return BOOL_VAL(false);
    }
    run_etat_block(d, b);
    return BOOL_VAL(true);
}

/* ============================================================
 * Mise en page
 * ============================================================ */

static Value fn_i_marge(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iMarge");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iMarge] (haut, bas, gauche, droite) en mm attendu.\n");
        return NIL_VAL;
    }
    owl_pdf_set_margins(d,
        arg_num(args[0], 0), arg_num(args[1], 0),
        arg_num(args[2], 0), arg_num(args[3], 0));
    return NIL_VAL;
}

static Value fn_i_saute_page(int argCount, Value* args) {
    (void)argCount; (void)args;
    OwlPdfDoc* d = require_doc("iSautePage");
    if (d) etat_page_break_with_hook(d);
    return NIL_VAL;
}

static Value fn_i_saute_ligne(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iSauteLigne");
    if (!d) return NIL_VAL;
    int n = 1;
    if (argCount >= 1 && IS_INTEGER(args[0])) n = (int)AS_INTEGER(args[0]);
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) owl_pdf_newline(d);
    return NIL_VAL;
}

/* iNumPage([<n>]) — canon.
 *   Sans args : renvoie le numero de page logique courant.
 *   Avec arg  : initialise le compteur logique. N'affecte pas le PDF
 *               physique — seulement le compteur. Le prochain iSautePage
 *               fera n+1. */
static Value fn_i_num_page(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iNumPage");
    if (!d) return INTEGER_VAL(0);
    if (argCount >= 1 && IS_INTEGER(args[0])) {
        int n = (int)AS_INTEGER(args[0]);
        if (n < 0) {
            /* Erreur metier (pas syntaxique) : valeur semantiquement invalide. */
            owl_emit_native_error(E_NATIVE_ARG_OUT_OF_RANGE, "iNumPage",
                                  "native", "iNumPage",
                                  "arg", "1",
                                  NULL);
            return INTEGER_VAL(owl_pdf_page_number(d));
        }
        owl_pdf_set_page_number(d, n);
    }
    return INTEGER_VAL(owl_pdf_page_number(d));
}
static Value fn_i_largeur_page(int argCount, Value* args) {
    (void)argCount; (void)args;
    OwlPdfDoc* d = require_doc("iLargeurPage");
    return REAL_VAL(d ? owl_pdf_page_width_mm(d) : 0);
}
static Value fn_i_hauteur_page(int argCount, Value* args) {
    (void)argCount; (void)args;
    OwlPdfDoc* d = require_doc("iHauteurPage");
    return REAL_VAL(d ? owl_pdf_page_height_mm(d) : 0);
}
static Value fn_i_hauteur_restante_page(int argCount, Value* args) {
    (void)argCount; (void)args;
    OwlPdfDoc* d = require_doc("iHauteurRestantePage");
    return REAL_VAL(d ? owl_pdf_remaining_height_mm(d) : 0);
}

/* ============================================================
 * Polices : iCreePolice / iPolice / iPosX / iPosY / iLargeurTexte
 * ============================================================ */

/* Mappe une famille constante vers un nom de famille Qt-friendly.
 * - Familles canon (iSwiss/iRoman/iCourier) -> noms classiques que le
 *   backend Qt remappera vers Liberation via map_legacy_family.
 * - Constantes OWL extension (iLiberationSans/Serif/Mono/iDejaVuSans)
 *   pointent directement vers les noms canoniques des fontes embarquees. */
static const char* family_name_from_int(int v) {
    switch (v) {
        case OWL_IPOL_ROMAN:            return "Times New Roman";
        case OWL_IPOL_SWISS:            return "Arial";
        case OWL_IPOL_COURIER:          return "Courier New";
        case OWL_IPOL_DEFAUT:           return "";  /* default Qt */
        case OWL_IPOL_LIBERATION_SANS:  return "Liberation Sans";
        case OWL_IPOL_LIBERATION_SERIF: return "Liberation Serif";
        case OWL_IPOL_LIBERATION_MONO:  return "Liberation Mono";
        case OWL_IPOL_DEJAVU_SANS:      return "DejaVu Sans";
        /* Aliases Microsoft : on retourne le nom Microsoft. map_legacy_family
         * cote Qt redirige vers la fonte Liberation/DejaVu embarquee si la
         * fonte Microsoft n'est pas installee (Linux, build static). */
        case OWL_IPOL_ARIAL:            return "Arial";
        case OWL_IPOL_TIMES_NEW_ROMAN:  return "Times New Roman";
        case OWL_IPOL_COURIER_NEW:      return "Courier New";
        case OWL_IPOL_VERDANA:          return "Verdana";
        case OWL_IPOL_TAHOMA:           return "Tahoma";
        case OWL_IPOL_CALIBRI:          return "Calibri";
        case OWL_IPOL_COMIC_SANS_MS:    return "Comic Sans MS";
        case OWL_IPOL_GEORGIA:          return "Georgia";
        case OWL_IPOL_TREBUCHET_MS:     return "Trebuchet MS";
        case OWL_IPOL_HELVETICA:        return "Helvetica";
        default: return NULL;
    }
}

/* iCreePolice(<id>, <taille_pt>, <style>, <nom_ou_famille>). */
static Value fn_i_cree_police(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iCreePolice");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iCreePolice] (id, taille, style, nom/famille) attendu.\n");
        return NIL_VAL;
    }
    int id = IS_INTEGER(args[0]) ? (int)AS_INTEGER(args[0]) : 0;
    double sz = arg_num(args[1], 10);
    /* Si iPoliceDidot a active l'unite didot : conversion didot -> pica.
     * 1 didot = 0.376 mm, 1 pica = 0.353 mm -> facteur = 0.376/0.353 ≈ 1.066.
     * Qt::QFont accepte une taille en points pica de toute facon. */
    if (g_use_didot_units) sz *= (0.376 / 0.353);
    /* Style : combinable bitwise. iNormal = OWL_IPOL_BASE + 0, iGras = +1, etc.
     * On extrait les flags bas (low byte) en soustrayant la base. Si l'utilisateur
     * combine (iGras + iItalique) -> base + 3. */
    int style_in = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;
    int style_flags = 0;
    /* iNormal seul : style_in == OWL_IPOL_BASE. Sinon decode le low byte. */
    if (style_in >= OWL_IPOL_BASE && style_in < OWL_IPOL_BASE + 16) {
        style_flags = style_in - OWL_IPOL_BASE;
    }

    /* Famille : soit string ("Arial"), soit constante (iSwiss/iRoman/...). */
    const char* family = NULL;
    if (IS_STRING(args[3])) {
        family = AS_CSTRING(args[3]);
    } else if (IS_INTEGER(args[3])) {
        family = family_name_from_int((int)AS_INTEGER(args[3]));
        if (!family) family = "";
    }
    if (!family) family = "";
    owl_pdf_font_create(d, id, sz, style_flags, family);
    return NIL_VAL;
}

/* iPolice(<id>) — retourne une chaine de controle "\x1FP<id>\x1E"
 * concatenable a un texte iImprime. Le change est applique au moment de
 * l'impression (mode toujours différé equivalent au canon `iPolice(<id>, Faux)`).
 *
 * Divergence canon (cf VISION §5bis) : l'arg <Calcul Immediat> de canon
 * n'est pas exposé — OWL est toujours en mode différé via control string.
 * Le compilateur rejette tout appel a 2 args. */
static Value fn_i_police(int argCount, Value* args) {
    if (argCount < 1 || !IS_INTEGER(args[0])) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    int id = (int)AS_INTEGER(args[0]);
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1FP%d\x1E", id);
    return OBJ_VAL(obj_copy_string(buf, n));
}

/* iPosX([<x_mm>]) — canon.
 *   0 args : retourne la position X courante du curseur (réel, mm).
 *   1 arg  : retourne une chaîne de contrôle "\x1FX<x>\x1E" concatenable
 *            a iImprime/iImprimeMot. */
static Value fn_i_pos_x(int argCount, Value* args) {
    OwlPdfDoc* d = owl_pdf_get_current();
    if (argCount == 0) {
        return REAL_VAL(d ? owl_pdf_get_cursor_x(d) : 0);
    }
    double x = arg_num(args[0], 0);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1FX%g\x1E", x);
    return OBJ_VAL(obj_copy_string(buf, n));
}
/* iPosY([<y_mm>]) — canon. Idem pour Y. */
static Value fn_i_pos_y(int argCount, Value* args) {
    OwlPdfDoc* d = owl_pdf_get_current();
    if (argCount == 0) {
        return REAL_VAL(d ? owl_pdf_get_cursor_y(d) : 0);
    }
    double y = arg_num(args[0], 0);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\x1FY%g\x1E", y);
    return OBJ_VAL(obj_copy_string(buf, n));
}

/* iPosH([<x_mm>]) — canon. Synonyme historique d'iPosX
 * sans le 2e arg <Calcul Immédiat> (qui n'existe pas dans la doc iPosH). */
static Value fn_i_pos_h(int argCount, Value* args) {
    return fn_i_pos_x(argCount, args);
}

/* iPosV([<y_mm>]) — canon. Synonyme historique d'iPosY. */
static Value fn_i_pos_v(int argCount, Value* args) {
    return fn_i_pos_y(argCount, args);
}

/* iLargeurTexte(<texte>) — largeur du texte en mm avec la police courante.
 * Les tokens de controle iPolice/iPosX/iPosY embarques dans la string sont
 * appliques avant la mesure (font_use, set cursor) puis stripes. La mesure
 * porte uniquement sur le texte resultant, en police courante apres
 * application des changes. */
/* Helper interne : applique les tokens de contrôle d'une string (font_use,
 * set cursor) et écrit le texte stripé dans `dst`. Utilisé par
 * iLargeurTexte et iHauteurTexte. */
static int apply_controls_and_strip(OwlPdfDoc* d, const char* raw, char* dst, int dstSize) {
    int len = 0;
    while (*raw && len < dstSize - 1) {
        if (*raw == '\x1F') {
            char type = raw[1];
            if (!type) break;
            const char* p = raw + 2;
            const char* end = p;
            while (*end && *end != '\x1E') end++;
            if (*end != '\x1E') break;
            char ctrl[32];
            int n = (int)(end - p);
            if (n >= (int)sizeof(ctrl)) n = sizeof(ctrl) - 1;
            memcpy(ctrl, p, n);
            ctrl[n] = 0;
            apply_control_token(d, type, ctrl);
            raw = end + 1;
            continue;
        }
        dst[len++] = *raw++;
    }
    dst[len] = 0;
    return len;
}

static Value fn_i_largeur_texte(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iLargeurTexte");
    if (!d || argCount < 1) return REAL_VAL(0);
    char buf[4096];
    apply_controls_and_strip(d, arg_str(args[0], ""), buf, sizeof(buf));
    return REAL_VAL(owl_pdf_text_width_mm(d, buf));
}

/* iHauteurTexte([<id_police> +] <texte>) — canon.
 * Retourne la hauteur de la police courante (en mm). Le texte est ignoré
 * pour le calcul (la hauteur dépend de la police, pas du contenu) — mais
 * les tokens de contrôle iPolice embarqués sont appliqués. */
static Value fn_i_hauteur_texte(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iHauteurTexte");
    if (!d || argCount < 1) return REAL_VAL(0);
    char buf[4096];
    apply_controls_and_strip(d, arg_str(args[0], ""), buf, sizeof(buf));
    return REAL_VAL(owl_pdf_text_height_mm(d));
}

/* iLargeurImage(<chemin>) — canon.
 * Retourne la largeur en mm a la resolution courante du document. */
static Value fn_i_largeur_image(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iLargeurImage");
    if (!d || argCount < 1) return REAL_VAL(0);
    const char* path = arg_str(args[0], "");
    double w = 0, h = 0;
    owl_pdf_image_size_mm(d, path, &w, &h);
    return REAL_VAL(w);
}

/* iHauteurImage(<chemin>) — canon. */
static Value fn_i_hauteur_image(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iHauteurImage");
    if (!d || argCount < 1) return REAL_VAL(0);
    const char* path = arg_str(args[0], "");
    double w = 0, h = 0;
    owl_pdf_image_size_mm(d, path, &w, &h);
    return REAL_VAL(h);
}

/* iMMVersPica(<MM> [, <Sens>]) — canon.
 * 1 pica = 0.353 mm. Le param <Sens> (Vrai=horizontal, Faux=vertical) est
 * ignoré V1 — le PDF a un dpi unique, pas de différence H/V. */
static Value fn_i_mm_vers_pica(int argCount, Value* args) {
    if (argCount < 1) return REAL_VAL(0);
    double mm = arg_num(args[0], 0);
    /* sens en args[1] ignoré V1. */
    return REAL_VAL(mm / 0.353);
}

/* iPicaVersMM(<Pica> [, <Sens>]) — canon. */
static Value fn_i_pica_vers_mm(int argCount, Value* args) {
    if (argCount < 1) return REAL_VAL(0);
    double pica = arg_num(args[0], 0);
    return REAL_VAL(pica * 0.353);
}

/* iPoliceDidot([<Actif>]) — canon.
 *   Sans args : toggle l'unite didot/pica.
 *   Avec arg booleen : pose Vrai = didot, Faux = pica.
 * Affecte les iCreePolice futurs (le facteur taille). */
static Value fn_i_police_didot(int argCount, Value* args) {
    if (argCount == 0) {
        g_use_didot_units = !g_use_didot_units;
    } else if (IS_BOOL(args[0])) {
        g_use_didot_units = AS_BOOL(args[0]);
    }
    return BOOL_VAL(g_use_didot_units);
}

/* iMagentaTransparent([<Actif>]) — canon.
 *   Sans args : toggle.
 *   Avec arg booleen : pose. */
static Value fn_i_magenta_transparent(int argCount, Value* args) {
    int current = owl_pdf_get_magenta_transparent();
    if (argCount == 0) {
        owl_pdf_set_magenta_transparent(!current);
    } else if (IS_BOOL(args[0])) {
        owl_pdf_set_magenta_transparent(AS_BOOL(args[0]) ? 1 : 0);
    }
    return BOOL_VAL(owl_pdf_get_magenta_transparent() != 0);
}

/* iListeImprimante() — canon.
 * Retourne la liste des imprimantes systeme separees par LF ('\n'). */
static Value fn_i_liste_imprimante(int argCount, Value* args) {
    (void)argCount; (void)args;
    const char* list = owl_printer_list_names();
    return OBJ_VAL(obj_copy_string(list, (int)strlen(list)));
}

/* iInfoImprimante([<ImprimanteEnCours> [, <TypeInfo>]]) — canon.
 * 1er arg booleen : Vrai (defaut) = imprimante courante set via iConfigure,
 *                   Faux = imprimante par defaut OS.
 * 2e arg constante : iNomImprimante (defaut), iNomDriver, iNomPort. */
static Value fn_i_info_imprimante(int argCount, Value* args) {
    bool use_current = true;
    if (argCount >= 1 && IS_BOOL(args[0])) use_current = AS_BOOL(args[0]);
    int info_type = 0;  /* iNomImprimante par defaut */
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        int s = (int)AS_INTEGER(args[1]);
        int idx = s - OWL_IINFOIMPR_BASE;
        if (idx >= 0 && idx <= 2) info_type = idx;
    }
    const char* info = owl_printer_info(use_current ? 1 : 0, info_type);
    return OBJ_VAL(obj_copy_string(info, (int)strlen(info)));
}

/* iNationImpression(<Nation> [, ...]) — canon.
 *
 * Modifie la langue de la prochaine impression (mois/jours formates en
 * langue locale dans les Etats graphiques). Pour V1 PDF/Etat libre, cette
 * fonction est sans effet : il n'y a pas de format natif locale-aware
 * dans les natives iImprime (le user formate les strings lui-meme via
 * DateVersChaine etc.).
 *
 * Divergence OWL V1 (cf VISION §5bis) : stub no-op accepte les args pour
 * conformite mais ne fait rien. A reimplementer V2 quand on aura les
 * Etats graphiques avec formats date/nombre locale-aware. */
static Value fn_i_nation_impression(int argCount, Value* args) {
    (void)argCount; (void)args;
    return NIL_VAL;
}

/* iSelectPolice(<num> [, options [, police_pre [, hauteur_pre]]] [, style] [, couleur])
 * — canon. Ouvre le QFontDialog systeme.
 *
 * Divergence OWL V1 (cf VISION §5bis) : on ne supporte que la signature de
 * base `iSelectPolice(<num>)`. Les params <Options>, <Police pré-sélectionnée>,
 * <Hauteur pré-sélectionnée>, <Style>, <Couleur> sont acceptes pour conformité
 * mais ignorés — le dialog Qt utilise ses defauts. */
static Value fn_i_select_police(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iSelectPolice");
    if (!d || argCount < 1 || !IS_INTEGER(args[0])) return BOOL_VAL(false);
    int font_id = (int)AS_INTEGER(args[0]);
    return BOOL_VAL(owl_pdf_select_font_dialog(d, font_id) != 0);
}

/* ============================================================
 * Texte
 * ============================================================ */

/* iImprime(<texte>) — canon.
 *
 * Le texte peut contenir des tokens de controle multiples produits par
 * iPolice/iPosX/iPosY, intercales avec les chunks a imprimer. Caract(10)
 * agit comme saut de ligne explicite. Saut de ligne final automatique.
 *
 * Divergence canon (cf VISION §5bis) : le 2e arg <Chaîne sur page suivante>
 * de la doc canon n'est PAS exposé — la pagination auto intra-string
 * n'est pas implémentée V1. Le compilateur rejette tout appel à 2 args.
 * L'utilisateur gère les sauts de page via iSautePage. */
static Value fn_i_imprime(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprime");
    if (!d) return BOOL_VAL(false);
    if (argCount == 0) {
        owl_pdf_newline(d);
        return BOOL_VAL(true);
    }
    const char* raw = NULL;
    ObjString* tmp = NULL;
    if (IS_STRING(args[0])) {
        raw = AS_CSTRING(args[0]);
    } else {
        tmp = value_to_string(args[0]);
        raw = tmp ? tmp->chars : "";
    }
    print_with_controls(d, raw, /*end_with_newline=*/true);
    return BOOL_VAL(true);
}

/* iImprimeMot(<texte>) — comme iImprime mais sans saut de ligne final.
 * Le curseur reste apres le dernier caractere imprime. */
static Value fn_i_imprime_mot(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprimeMot");
    if (!d || argCount < 1) return BOOL_VAL(false);
    const char* raw = NULL;
    ObjString* tmp = NULL;
    if (IS_STRING(args[0])) {
        raw = AS_CSTRING(args[0]);
    } else {
        tmp = value_to_string(args[0]);
        raw = tmp ? tmp->chars : "";
    }
    print_with_controls(d, raw, /*end_with_newline=*/false);
    return BOOL_VAL(true);
}

/* iImprimeZone — canon.
 * Retourne le nombre de caracteres imprimes (Length du sous-texte). */
static Value fn_i_imprime_zone(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprimeZone");
    if (!d || argCount < 5) {
        if (d) fprintf(stderr, "[iImprimeZone] (texte, X1, Y1, X2, Y2 [, alignement [, debut [, fin]]]) attendu.\n");
        return INTEGER_VAL(0);
    }
    const char* raw = arg_str(args[0], "");
    /* Strip les controls (Qt fait le wrap dans le rect). */
    char stripped[4096];
    strip_controls(raw, stripped, sizeof(stripped));
    int total = (int)strlen(stripped);

    /* Substring optionnel <Debut> et <Fin> (1-based). */
    int start = 1;
    int end   = total;
    if (argCount >= 7 && IS_INTEGER(args[6])) start = (int)AS_INTEGER(args[6]);
    if (argCount >= 8 && IS_INTEGER(args[7])) end   = (int)AS_INTEGER(args[7]);
    if (start < 1) start = 1;
    if (end > total) end = total;
    if (end < start) return INTEGER_VAL(0);
    int subLen = end - start + 1;

    char sub[4096];
    if (subLen >= (int)sizeof(sub)) subLen = sizeof(sub) - 1;
    memcpy(sub, stripped + (start - 1), subLen);
    sub[subLen] = 0;

    double x1 = arg_num(args[1], 0), y1 = arg_num(args[2], 0);
    double x2 = arg_num(args[3], 0), y2 = arg_num(args[4], 0);
    /* Alignement : bitmask iAlign combinable. Defaut canon = iHaut + iGauche = 0x11. */
    int align = (argCount >= 6 && IS_INTEGER(args[5])) ? (int)AS_INTEGER(args[5]) : 0x11;
    owl_pdf_print_zone(d, sub, x1, y1, x2, y2, align);
    return INTEGER_VAL(subLen);
}

/* ============================================================
 * Geometrie
 *
 * Conversions canon :
 *   - Epaisseur en points pica : 1 pica = 0.353 mm. Defaut canon = 1 pica.
 *     Epaisseur 0 = 1 pixel a la resolution courante (300 dpi -> ~0.085 mm).
 *   - Style (iContinu/iTiret/iPointille/iMixte) : sentinel - OWL_ITRAIT_BASE.
 *   - Brosse (iPleine/iHorizontale/...) : sentinel - OWL_IBROSSE_BASE.
 *   - Bord (iBordTous/iBordHaut/...) : mapping vers bitmask 4 bits.
 *   - Remplissage 1-200 (canon) : gradient luminosite par mix lineaire
 *     de la couleur avec blanc (1-99) ou noir (101-200). 100 = pur.
 * ============================================================ */

/* Pica → mm. Defaut canon = 1 pica. Cas special "0" = 1 pixel a 300 dpi. */
static double pica_to_mm(double pica) {
    if (pica < 0) return 0.353;          /* defaut canon */
    if (pica == 0) return 25.4 / 300.0;  /* 1 pixel a 300 dpi */
    return pica * 0.353;
}

/* Decode constante iTrait (sentinel) en index 0..3 (defaut iContinu). */
static int trait_from_sentinel(Value v) {
    if (!IS_INTEGER(v)) return 0;
    int s = (int)AS_INTEGER(v);
    int idx = s - OWL_ITRAIT_BASE;
    return (idx >= 0 && idx <= 3) ? idx : 0;
}

/* Decode constante iBrosse en index 0..6 (defaut iPleine). */
static int brosse_from_sentinel(Value v) {
    if (!IS_INTEGER(v)) return 0;
    int s = (int)AS_INTEGER(v);
    int idx = s - OWL_IBROSSE_BASE;
    return (idx >= 0 && idx <= 6) ? idx : 0;
}

/* Decode constante iBord en bitmask haut(1) bas(2) gauche(4) droit(8).
 * iBordTous -> 0x0F, iBordAucun -> 0, defaut = 0x0F (tous bords). */
static int bord_mask_from_sentinel(Value v) {
    if (!IS_INTEGER(v)) return 0x0F;
    int s = (int)AS_INTEGER(v);
    if (s == OWL_IBORD_TOUS)   return 0x0F;
    if (s == OWL_IBORD_AUCUN)  return 0x00;
    if (s == OWL_IBORD_HAUT)   return 0x01;
    if (s == OWL_IBORD_BAS)    return 0x02;
    if (s == OWL_IBORD_GAUCHE) return 0x04;
    if (s == OWL_IBORD_DROIT)  return 0x08;
    return 0x0F;
}

/* Applique le gradient luminosite sur une couleur BGR (canon Remplissage
 * 1-200). Retourne la couleur ajustee. fill_pct = 0 -> caller doit traiter
 * comme "pas de remplissage" et passer -1 au backend. */
static int apply_luminosity_gradient(int color_bgr, int fill_pct) {
    if (fill_pct <= 0) return -1;
    if (fill_pct > 200) fill_pct = 200;
    if (color_bgr < 0) color_bgr = 0;  /* noir par defaut */
    int r = color_bgr & 0xFF;
    int g = (color_bgr >> 8) & 0xFF;
    int b = (color_bgr >> 16) & 0xFF;
    if (fill_pct < 100) {
        /* Mix avec blanc : couleur clarifiee. t=0 -> blanc, t=1 -> couleur. */
        double t = (double)fill_pct / 100.0;
        r = (int)(255 + (r - 255) * t);
        g = (int)(255 + (g - 255) * t);
        b = (int)(255 + (b - 255) * t);
    } else if (fill_pct > 100) {
        /* Mix avec noir : couleur assombrie. t=0 -> couleur, t=1 -> noir. */
        double t = (double)(fill_pct - 100) / 100.0;
        r = (int)(r * (1.0 - t));
        g = (int)(g * (1.0 - t));
        b = (int)(b * (1.0 - t));
    }
    return (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16);
}

/* iLigne — canon. Bout arrondi (RoundCap).
 * (X1, Y1, X2, Y2 [, Style [, Epaisseur [, Couleur]]]) */
static Value fn_i_ligne(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iLigne");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iLigne] (X1,Y1,X2,Y2 [,Style,Epaisseur,Couleur]) attendu.\n");
        return BOOL_VAL(false);
    }
    int trait = (argCount >= 5) ? trait_from_sentinel(args[4]) : 0;
    double thick_pica = (argCount >= 6) ? arg_num(args[5], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int color = (argCount >= 7 && IS_INTEGER(args[6])) ? (int)AS_INTEGER(args[6]) : -1;
    owl_pdf_line(d, arg_num(args[0],0), arg_num(args[1],0),
                     arg_num(args[2],0), arg_num(args[3],0),
                     thick_mm, color, trait, /*cap_style=*/0);
    return BOOL_VAL(true);
}

/* iTrait — canon. Bout carré (FlatCap), différent d'iLigne.
 * Attention : ordre des params optionnels diverge d'iLigne :
 *   iLigne : Style, Épaisseur, Couleur
 *   iTrait : Épaisseur, Couleur, Style
 * (X1, Y1, X2, Y2 [, Épaisseur [, Couleur [, Style]]]) */
static Value fn_i_trait(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iTrait");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iTrait] (X1,Y1,X2,Y2 [,Epaisseur,Couleur,Style]) attendu.\n");
        return BOOL_VAL(false);
    }
    double thick_pica = (argCount >= 5) ? arg_num(args[4], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int color = (argCount >= 6 && IS_INTEGER(args[5])) ? (int)AS_INTEGER(args[5]) : -1;
    int trait = (argCount >= 7) ? trait_from_sentinel(args[6]) : 0;
    owl_pdf_line(d, arg_num(args[0],0), arg_num(args[1],0),
                     arg_num(args[2],0), arg_num(args[3],0),
                     thick_mm, color, trait, /*cap_style=*/1);
    return BOOL_VAL(true);
}

/* iTraitH — canon. Trait horizontal entre X1 et X2 a la
 * position Y du curseur courant. Bout carré.
 * (X1, X2 [, Épaisseur [, Couleur [, Style]]]) */
static Value fn_i_trait_h(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iTraitH");
    if (!d || argCount < 2) {
        if (d) fprintf(stderr, "[iTraitH] (X1, X2 [,Epaisseur,Couleur,Style]) attendu.\n");
        return BOOL_VAL(false);
    }
    double y = owl_pdf_get_cursor_y(d);
    double thick_pica = (argCount >= 3) ? arg_num(args[2], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int color = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : -1;
    int trait = (argCount >= 5) ? trait_from_sentinel(args[4]) : 0;
    owl_pdf_line(d, arg_num(args[0],0), y, arg_num(args[1],0), y,
                     thick_mm, color, trait, /*cap_style=*/1);
    return BOOL_VAL(true);
}

/* iTraitV — canon. Trait vertical entre Y1 et Y2 a la
 * position X du curseur courant. Bout carré.
 * (Y1, Y2 [, Épaisseur [, Couleur [, Style]]]) */
static Value fn_i_trait_v(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iTraitV");
    if (!d || argCount < 2) {
        if (d) fprintf(stderr, "[iTraitV] (Y1, Y2 [,Epaisseur,Couleur,Style]) attendu.\n");
        return BOOL_VAL(false);
    }
    double x = owl_pdf_get_cursor_x(d);
    double thick_pica = (argCount >= 3) ? arg_num(args[2], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int color = (argCount >= 4 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : -1;
    int trait = (argCount >= 5) ? trait_from_sentinel(args[4]) : 0;
    owl_pdf_line(d, x, arg_num(args[0],0), x, arg_num(args[1],0),
                     thick_mm, color, trait, /*cap_style=*/1);
    return BOOL_VAL(true);
}

/* iCadre — canon.
 * (X1, Y1, X2, Y2 [, Epaisseur [, Remplissage [, Couleur [, Trait [, Brosse [, Bord]]]]]]) */
static Value fn_i_cadre(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iCadre");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iCadre] (X1,Y1,X2,Y2 [,Epaisseur,Remplissage,Couleur,Trait,Brosse,Bord]) attendu.\n");
        return NIL_VAL;
    }
    double thick_pica = (argCount >= 5) ? arg_num(args[4], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int fill_pct = (argCount >= 6 && IS_INTEGER(args[5])) ? (int)AS_INTEGER(args[5]) : 0;
    int color    = (argCount >= 7 && IS_INTEGER(args[6])) ? (int)AS_INTEGER(args[6]) : -1;
    int fill_col = apply_luminosity_gradient(color, fill_pct);
    int trait    = (argCount >= 8) ? trait_from_sentinel(args[7]) : 0;
    int brosse   = (argCount >= 9) ? brosse_from_sentinel(args[8]) : 0;
    int bord     = (argCount >= 10) ? bord_mask_from_sentinel(args[9]) : 0x0F;
    owl_pdf_rect(d, arg_num(args[0],0), arg_num(args[1],0),
                     arg_num(args[2],0), arg_num(args[3],0),
                     thick_mm, color, fill_col, trait, brosse, bord);
    return NIL_VAL;
}

/* iCadreArrondi — canon.
 * (X1, Y1, X2, Y2, LargBords, HautBords [, Epaisseur [, Remplissage [, Couleur [, Trait [, Brosse]]]]]) */
static Value fn_i_cadre_arrondi(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iCadreArrondi");
    if (!d || argCount < 6) {
        if (d) fprintf(stderr, "[iCadreArrondi] (X1,Y1,X2,Y2,LargBords,HautBords [,Epaisseur,Remplissage,Couleur,Trait,Brosse]) attendu.\n");
        return NIL_VAL;
    }
    double thick_pica = (argCount >= 7) ? arg_num(args[6], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int fill_pct = (argCount >= 8 && IS_INTEGER(args[7])) ? (int)AS_INTEGER(args[7]) : 0;
    int color    = (argCount >= 9 && IS_INTEGER(args[8])) ? (int)AS_INTEGER(args[8]) : -1;
    int fill_col = apply_luminosity_gradient(color, fill_pct);
    int trait    = (argCount >= 10) ? trait_from_sentinel(args[9]) : 0;
    int brosse   = (argCount >= 11) ? brosse_from_sentinel(args[10]) : 0;
    owl_pdf_rounded_rect(d, arg_num(args[0],0), arg_num(args[1],0),
                              arg_num(args[2],0), arg_num(args[3],0),
                              arg_num(args[4],0), arg_num(args[5],0),
                              thick_mm, color, fill_col, trait, brosse);
    return NIL_VAL;
}

/* iEllipse — canon.
 * (X1, Y1, X2, Y2 [, Epaisseur [, Luminosite [, Couleur [, Trait [, Brosse]]]]]) */
static Value fn_i_ellipse(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iEllipse");
    if (!d || argCount < 4) {
        if (d) fprintf(stderr, "[iEllipse] (X1,Y1,X2,Y2 [,Epaisseur,Luminosite,Couleur,Trait,Brosse]) attendu.\n");
        return NIL_VAL;
    }
    double thick_pica = (argCount >= 5) ? arg_num(args[4], 1.0) : 1.0;
    double thick_mm   = pica_to_mm(thick_pica);
    int fill_pct = (argCount >= 6 && IS_INTEGER(args[5])) ? (int)AS_INTEGER(args[5]) : 0;
    int color    = (argCount >= 7 && IS_INTEGER(args[6])) ? (int)AS_INTEGER(args[6]) : -1;
    int fill_col = apply_luminosity_gradient(color, fill_pct);
    int trait    = (argCount >= 8) ? trait_from_sentinel(args[7]) : 0;
    int brosse   = (argCount >= 9) ? brosse_from_sentinel(args[8]) : 0;
    owl_pdf_ellipse(d, arg_num(args[0],0), arg_num(args[1],0),
                         arg_num(args[2],0), arg_num(args[3],0),
                         thick_mm, color, fill_col, trait, brosse);
    return NIL_VAL;
}

/* ============================================================
 * Image
 * ============================================================ */

/* Decode une constante iEchelle (sentinel) en mode 0/1/2.
 * Si la valeur n'est pas une constante iAffichage* mais un entier en
 * dehors de la plage sentinel, on traite comme zoom% (canon syntaxe 2). */
static int echelle_mode_from_sentinel(Value v, double* out_zoom_pct) {
    *out_zoom_pct = 100.0;
    if (!IS_INTEGER(v)) return 0;
    int s = (int)AS_INTEGER(v);
    int idx = s - OWL_IECHELLE_BASE;
    if (idx >= 0 && idx <= 2) return idx;
    /* Pas une constante iEchelle -> traite comme zoom% (canon syntaxe 2). */
    if (s >= 0 && s <= 999) {
        *out_zoom_pct = (double)s;
        return 0;  /* mode normal pour le zoom% */
    }
    return 0;
}

/* iImprimeImage — canon. 3 syntaxes :
 *   S1 : (image, X1, Y1, X2, Y2 [, Echelle])           -> 5 ou 6 args
 *   S2 : (image, X1, Y1 [, Echelle])                   -> 3 ou 4 args
 *   S3 : (image, X1,Y1,X2,Y2, X3,Y3,X4,Y4 [, Echelle]) -> 9 ou 10 args
 * Le compilateur via signatures strictes rejette les autres arities. */
static Value fn_i_imprime_image(int argCount, Value* args) {
    OwlPdfDoc* d = require_doc("iImprimeImage");
    if (!d) return BOOL_VAL(false);
    const char* path = arg_str(args[0], "");

    if (argCount == 3 || argCount == 4) {
        /* S2 : position seule + zoom optionnel */
        double x1 = arg_num(args[1], 0), y1 = arg_num(args[2], 0);
        double zoom_pct = 100.0;
        int mode = 0;
        if (argCount == 4) {
            mode = echelle_mode_from_sentinel(args[3], &zoom_pct);
        }
        /* On signale via x2<=x1 que c'est S2 (taille native + zoom). */
        owl_pdf_image(d, path, x1, y1, x1, y1, mode, zoom_pct);
        return BOOL_VAL(true);
    }
    if (argCount == 5 || argCount == 6) {
        /* S1 : zone delimitee + echelle optionnelle */
        double x1 = arg_num(args[1], 0), y1 = arg_num(args[2], 0);
        double x2 = arg_num(args[3], 0), y2 = arg_num(args[4], 0);
        double zoom_pct = 100.0;
        int mode = 0;
        if (argCount == 6) {
            mode = echelle_mode_from_sentinel(args[5], &zoom_pct);
        }
        owl_pdf_image(d, path, x1, y1, x2, y2, mode, zoom_pct);
        return BOOL_VAL(true);
    }
    if (argCount == 9 || argCount == 10) {
        /* S3 : partie d'image */
        double x1 = arg_num(args[1], 0), y1 = arg_num(args[2], 0);
        double x2 = arg_num(args[3], 0), y2 = arg_num(args[4], 0);
        double sx1 = arg_num(args[5], 0), sy1 = arg_num(args[6], 0);
        double sx2 = arg_num(args[7], 0), sy2 = arg_num(args[8], 0);
        double zoom_pct = 100.0;
        int mode = 0;
        if (argCount == 10) {
            mode = echelle_mode_from_sentinel(args[9], &zoom_pct);
        }
        owl_pdf_image_part(d, path, x1, y1, x2, y2, sx1, sy1, sx2, sy2, mode);
        return BOOL_VAL(true);
    }
    /* Arity non couverte par la signature catalog : ne devrait pas arriver. */
    fprintf(stderr, "[iImprimeImage] arity %d non supportee.\n", argCount);
    return BOOL_VAL(false);
}

/* ============================================================
 * Registry — wiring des implementations
 *
 * Les donnees de registre (array DEFS, signatures, enums refs) vivent dans
 * stdlib_pdf_defs.c (Qt-free, compile aussi en console). Ici (build GUI
 * uniquement) on injecte les implementations C reelles dans DEFS, AVANT que
 * owl_stdlib.c n'appelle stdlib_pdf_defs_register() : register_module voit
 * alors des .impl non-NULL et prend le chemin d'enregistrement normal
 * (registre byte-identique a l'historique pre-split).
 * ============================================================ */
void stdlib_pdf_impls_wire(void) {
    /* Lifecycle */
    stdlib_pdf_defs_set_impl("iConfigure",           fn_i_configure);
    stdlib_pdf_defs_set_impl("iDestination",         fn_i_destination);
    stdlib_pdf_defs_set_impl("iFinImprime",          fn_i_fin_imprime);
    stdlib_pdf_defs_set_impl("iParametrePDF",        fn_i_parametre_pdf);
    stdlib_pdf_defs_set_impl("iImprimeEtat",         fn_i_imprime_etat);
    stdlib_pdf_defs_set_impl("iImprimeBloc",         fn_i_imprime_bloc);
    /* Mise en page */
    stdlib_pdf_defs_set_impl("iMarge",               fn_i_marge);
    stdlib_pdf_defs_set_impl("iSautePage",           fn_i_saute_page);
    stdlib_pdf_defs_set_impl("iSauteLigne",          fn_i_saute_ligne);
    stdlib_pdf_defs_set_impl("iNumPage",             fn_i_num_page);
    stdlib_pdf_defs_set_impl("iLargeurPage",         fn_i_largeur_page);
    stdlib_pdf_defs_set_impl("iHauteurPage",         fn_i_hauteur_page);
    stdlib_pdf_defs_set_impl("iHauteurRestantePage", fn_i_hauteur_restante_page);
    /* Polices + curseur */
    stdlib_pdf_defs_set_impl("iCreePolice",          fn_i_cree_police);
    stdlib_pdf_defs_set_impl("iPolice",              fn_i_police);
    stdlib_pdf_defs_set_impl("iPosX",                fn_i_pos_x);
    stdlib_pdf_defs_set_impl("iPosY",                fn_i_pos_y);
    stdlib_pdf_defs_set_impl("iPosH",                fn_i_pos_h);
    stdlib_pdf_defs_set_impl("iPosV",                fn_i_pos_v);
    stdlib_pdf_defs_set_impl("iLargeurTexte",        fn_i_largeur_texte);
    stdlib_pdf_defs_set_impl("iHauteurTexte",        fn_i_hauteur_texte);
    /* Mesure / conversion */
    stdlib_pdf_defs_set_impl("iLargeurImage",        fn_i_largeur_image);
    stdlib_pdf_defs_set_impl("iHauteurImage",        fn_i_hauteur_image);
    stdlib_pdf_defs_set_impl("iMMVersPica",          fn_i_mm_vers_pica);
    stdlib_pdf_defs_set_impl("iPicaVersMM",          fn_i_pica_vers_mm);
    stdlib_pdf_defs_set_impl("iPoliceDidot",         fn_i_police_didot);
    stdlib_pdf_defs_set_impl("iMagentaTransparent",  fn_i_magenta_transparent);
    stdlib_pdf_defs_set_impl("iSelectPolice",        fn_i_select_police);
    /* Peripheriques */
    stdlib_pdf_defs_set_impl("iListeImprimante",     fn_i_liste_imprimante);
    stdlib_pdf_defs_set_impl("iInfoImprimante",      fn_i_info_imprimante);
    stdlib_pdf_defs_set_impl("iNationImpression",    fn_i_nation_impression);
    /* Texte */
    stdlib_pdf_defs_set_impl("iImprime",             fn_i_imprime);
    stdlib_pdf_defs_set_impl("iImprimeMot",          fn_i_imprime_mot);
    stdlib_pdf_defs_set_impl("iImprimeZone",         fn_i_imprime_zone);
    /* Geometrie */
    stdlib_pdf_defs_set_impl("iLigne",               fn_i_ligne);
    stdlib_pdf_defs_set_impl("iTrait",               fn_i_trait);
    stdlib_pdf_defs_set_impl("iTraitH",              fn_i_trait_h);
    stdlib_pdf_defs_set_impl("iTraitV",              fn_i_trait_v);
    stdlib_pdf_defs_set_impl("iCadre",               fn_i_cadre);
    stdlib_pdf_defs_set_impl("iCadreArrondi",        fn_i_cadre_arrondi);
    stdlib_pdf_defs_set_impl("iEllipse",             fn_i_ellipse);
    /* Image */
    stdlib_pdf_defs_set_impl("iImprimeImage",        fn_i_imprime_image);
}
