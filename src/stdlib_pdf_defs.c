// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_pdf_defs.c - Donnees de registre des natives Etats / PDF (i*).
 *
 * Partie Qt-FREE du module PDF (cf. stdlib_pdf_defs.h). Contient l'array DEFS
 * (42 natives) + ses tables OwlParam/OwlSignature, SANS aucune implementation
 * C : chaque entree a .impl=NULL a la declaration.
 *
 * - Cote GUI (owl.exe, owl_runtime_gui) : stdlib_pdf_impls_wire() (stdlib_pdf.c)
 *   remplit les .impl AVANT stdlib_pdf_defs_register() -> register_module voit
 *   des impls non-NULL -> chemin normal -> registre byte-identique a l'historique.
 * - Cote console (owl-cli, owl-build) : les .impl restent NULL. Le compilateur
 *   consulte uniquement les signatures/enums/arite (jamais .impl), donc la
 *   resolution des natives PDF est identique au GUI. Le portail de compile
 *   d'owl-build et `owl-cli --check` sont des passes compile-only : .impl=NULL
 *   n'est jamais dereferences.
 *
 * Les enums (E_IDest, E_IAlign, ...) ne sont PAS ici : ils vivent dans
 * ui_catalog.c (OWL_CORE_SRCS, enregistres inconditionnellement), on s'y
 * refere seulement via &E_Ixxx dans les OwlParam. */

#include "stdlib_pdf_defs.h"
#include "owl_stdlib.h"       /* extern E_IDest/E_IAlign/... + owl_registry_register_module */
#include "owl_registry.h"

#include <string.h>

/* ============================================================
 * Registry
 * ============================================================ */

static const OwlParam p_dest[]    = {{"type","entier","Destination iPDFGenerique/iImprimante/...", &E_IDest, NULL},
                                      {"chemin","chaine","Chemin de sortie (PDF)"},{0}};
/*  — Signature alternative iDestination(iImageMemoire, <tabImages>).
 * Le 2e arg est un tableau de Image qui sera peuple par iImprimeEtat
 * (1 element par page rasterisee). */
static const OwlParam p_dest_img[] = {{"type","entier","Doit etre iImageMemoire", &E_IDest, NULL},
                                       {"images","tableau","tableau de Image cible (peuple a iFinImprime)"},{0}};
static const OwlParam p_marge[]   = {{"haut","reel","mm"},{"bas","reel","mm"},
                                      {"gauche","reel","mm"},{"droite","reel","mm"},{0}};
static const OwlParam p_imprime[] = {{"texte","chaîne|entier","Texte (peut contenir iPolice/iPosX prefix)"},{0}};
static const OwlParam p_zone[]    = {{"texte","chaine","Texte a imprimer dans la zone"},
                                      {"X1","reel","Coin haut-gauche X (mm)"},{"Y1","reel","Coin haut-gauche Y (mm)"},
                                      {"X2","reel","Coin bas-droit X (mm)"},{"Y2","reel","Coin bas-droit Y (mm)"},
                                      {"alignement","entier","Combinaison iHaut/iBas/iCentreH/iCentreV/iGauche/iDroite", &E_IAlign, &E_IAlign},
                                      {"debut","entier","Optionnel : indice du premier caractere a imprimer (1-based)"},
                                      {"fin","entier","Optionnel : indice du dernier caractere a imprimer"},{0}};
static const OwlParam p_ligne[]   = {{"X1","reel","Debut X (mm)"},{"Y1","reel","Debut Y (mm)"},
                                      {"X2","reel","Fin X (mm)"},{"Y2","reel","Fin Y (mm)"},
                                      {"style","entier","iContinu/iTiret/iPointille/iMixte", &E_ITrait, NULL},
                                      {"epaisseur","reel","Points pica (defaut 1 = 0.353 mm)"},
                                      {"couleur","entier","Couleur RVB pack (defaut noir)"},{0}};
static const OwlParam p_rect[]    = {{"X1","reel","Coin haut-gauche X (mm)"},{"Y1","reel","Coin haut-gauche Y (mm)"},
                                      {"X2","reel","Coin bas-droit X (mm)"},{"Y2","reel","Coin bas-droit Y (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1 = 0.353 mm)"},
                                      {"remplissage","entier","Taux 1-200 (1-99 clarifie, 100 pur, 101-200 assombri, 0=aucun)"},
                                      {"couleur","entier","Couleur RVB pack (defaut noir)"},
                                      {"trait","entier","iContinu/iTiret/...", &E_ITrait, NULL},
                                      {"brosse","entier","iPleine/iHorizontale/iCroisee/...", &E_IBrosse, NULL},
                                      {"bord","entier","iBordTous/iBordHaut/...", &E_IBord, NULL},{0}};
static const OwlParam p_rrect[]   = {{"X1","reel","Coin haut-gauche X (mm)"},{"Y1","reel","Coin haut-gauche Y (mm)"},
                                      {"X2","reel","Coin bas-droit X (mm)"},{"Y2","reel","Coin bas-droit Y (mm)"},
                                      {"rayonH","reel","Largeur des bords arrondis (mm)"},{"rayonV","reel","Hauteur des bords arrondis (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1 = 0.353 mm)"},
                                      {"remplissage","entier","Taux 1-200 (idem iCadre)"},
                                      {"couleur","entier","Couleur RVB pack (defaut noir)"},
                                      {"trait","entier","iContinu/iTiret/...", &E_ITrait, NULL},
                                      {"brosse","entier","iPleine/iHorizontale/...", &E_IBrosse, NULL},{0}};
/* iEllipse : pas de Bord, pas de rayons. 9 params max. */
static const OwlParam p_ellipse[] = {{"X1","reel","Coin haut-gauche X (mm)"},{"Y1","reel","Coin haut-gauche Y (mm)"},
                                      {"X2","reel","Coin bas-droit X (mm)"},{"Y2","reel","Coin bas-droit Y (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1 = 0.353 mm)"},
                                      {"luminosite","entier","Taux 1-200 (idem iCadre)"},
                                      {"couleur","entier","Couleur RVB pack (defaut noir)"},
                                      {"trait","entier","iContinu/iTiret/...", &E_ITrait, NULL},
                                      {"brosse","entier","iPleine/iHorizontale/...", &E_IBrosse, NULL},{0}};
/* iImprimeImage : 3 syntaxes canon distinctes -> 3 jeux de params dedies. */
static const OwlParam p_image_pos[]  = {{"chemin","chaine","Fichier image (PNG/JPEG/BMP)"},
                                         {"X1","reel","Position X (mm)"},{"Y1","reel","Position Y (mm)"},
                                         {"echelle","entier","Optionnel : zoom 0-999 ou iAffichageNormal/Homothetique/HomoCentre", &E_IEchelle, NULL},{0}};
static const OwlParam p_image_zone[] = {{"chemin","chaine","Fichier image (PNG/JPEG/BMP)"},
                                         {"X1","reel","Coin haut-gauche X (mm)"},{"Y1","reel","Coin haut-gauche Y (mm)"},
                                         {"X2","reel","Coin bas-droit X (mm)"},{"Y2","reel","Coin bas-droit Y (mm)"},
                                         {"echelle","entier","Optionnel : iAffichageNormal/Homothetique/HomoCentre", &E_IEchelle, NULL},{0}};
static const OwlParam p_image_part[] = {{"chemin","chaine","Fichier image"},
                                         {"X1","reel","Cible coin haut-gauche X (mm)"},{"Y1","reel","Cible coin haut-gauche Y (mm)"},
                                         {"X2","reel","Cible coin bas-droit X (mm)"},{"Y2","reel","Cible coin bas-droit Y (mm)"},
                                         {"X3","reel","Source coin haut-gauche X (mm)"},{"Y3","reel","Source coin haut-gauche Y (mm)"},
                                         {"X4","reel","Source coin bas-droit X (mm)"},{"Y4","reel","Source coin bas-droit Y (mm)"},
                                         {"echelle","entier","Optionnel : iAffichageNormal/Homothetique/HomoCentre", &E_IEchelle, NULL},{0}};
static const OwlParam p_cree_pol[]= {{"id","entier","Identifiant police"},
                                      {"taille","reel","Points typographiques"},
                                      {"style","entier","iNormal+iGras+iItalique+...", NULL, &E_IStyle},
                                      {"nom","chaîne|entier","Nom (chaîne) ou famille (iSwiss/iRoman/...)", &E_IFamily, NULL},{0}};
static const OwlParam p_pol[]     = {{"id","entier","Identifiant police créée par iCreePolice"},{0}};
static const OwlParam p_pos[]     = {{"valeur","reel","mm"},{0}};
static const OwlParam p_simple[]  = {{0}};
static const OwlParam p_saute[]   = {{"nbLignes","entier","Defaut 1"},{0}};
static const OwlParam p_imprime_etat[] = {
    {"etat","chaine","Nom de l'État declare via 'ETAT_X est un État(...)'"},
    {0}
};
static const OwlSignature sig_imprime_etat[] = {
    {-1, p_imprime_etat, "booleen", "Vrai si l'impression a reussi"},
    {0}
};
static const OwlParam p_imprime_bloc[] = {
    {"bloc","chaine","Nom du bloc a imprimer (sous-bloc de l'État courant)"},
    {0}
};
static const OwlSignature sig_imprime_bloc[] = {
    {1, p_imprime_bloc, "booleen", "Vrai si le bloc a ete trouve et imprime"},
    {0}
};

/* iDestination : 0 = getter, 1 = type seul, 2 = type + chemin (PDF) ou
 * type + tableau (iImageMemoire). p_dest_img documente la 2e syntaxe arite 2. */
static const OwlSignature sig_dest[]    = {
    { 0, p_simple,   "entier", "Destination courante (getter)"},
    { 1, p_dest,     "entier", "Type de destination seul"},
    { 2, p_dest,     "entier", "Type + chemin de sortie (PDF)"},
    { 2, p_dest_img, "entier", "Destination courante (iImageMemoire)"},
    {0}
};
static const OwlSignature sig_simple[]  = {{0, p_simple, "", ""}, {0}};
static const OwlParam p_iconfigure[] = {
    {"nomImprimante","chaine","Nom systeme de l'imprimante a selectionner"},
    {0}
};
/* iConfigure : 0 args = popup dialog, 1 arg = nom imprimante. 2+ args
 * (modifTemporaire canon) non supporte V1 -> rejet compile-time. */
static const OwlSignature sig_iconfigure[] = {
    { 0, p_simple,     "booleen", "Ouvre le dialog standard de configuration" },
    { 1, p_iconfigure, "booleen", "Selectionne l'imprimante par nom" },
    {0}
};
/* iParametrePDF : 3 syntaxes canon distinctes mais variadic (1..N args). */
static const OwlParam p_parametre_pdf[] = {
    {"option","pdfParametre|chaîne|entier","pdfParametre, mot de passe (chaine), ou option de protection/format (entier)"},
    {0}
};
static const OwlSignature sig_parametre_pdf[] = {
    {-1, p_parametre_pdf, "", ""},
    {0}
};
static const OwlSignature sig_marge[]   = {{4, p_marge, "", ""}, {0}};
static const OwlSignature sig_saute[]   = {
    { 0, p_simple, "", "Saut d'une ligne" },
    { 1, p_saute,  "", "Saut de N lignes" },
    {0}
};
static const OwlParam p_inumpage[] = {{"numéro","entier","Nouvelle valeur du compteur de page"},{0}};
/* iNumPage : 0 args = getter, 1 arg = setter. */
static const OwlSignature sig_inumpage[] = {
    { 0, p_simple,   "entier", "Numéro de page courant" },
    { 1, p_inumpage, "entier", "Initialise le compteur logique" },
    {0}
};
static const OwlSignature sig_real[]    = {{0, p_simple, "reel",   "mm"}, {0}};
static const OwlSignature sig_imprime[] = {{1, p_imprime, "booleen", "Vrai si succès"}, {0}};
static const OwlSignature sig_zone[]    = {
    { 5, p_zone, "entier", "Texte + zone (X1,Y1,X2,Y2)" },
    { 6, p_zone, "entier", "Zone + alignement" },
    { 7, p_zone, "entier", "Zone + alignement + debut" },
    { 8, p_zone, "entier", "Zone + alignement + debut + fin" },
    {0}
};
static const OwlSignature sig_largeur[] = {{1, p_imprime, "reel", "Largeur en mm"}, {0}};
static const OwlSignature sig_ligne[]   = {
    { 4, p_ligne, "booleen", "X1,Y1,X2,Y2" },
    { 5, p_ligne, "booleen", "+ Style" },
    { 6, p_ligne, "booleen", "+ Epaisseur" },
    { 7, p_ligne, "booleen", "+ Couleur" },
    {0}
};
static const OwlSignature sig_rect[]    = {
    { 4,  p_rect, "", "X1,Y1,X2,Y2" },
    { 5,  p_rect, "", "+ Epaisseur" },
    { 6,  p_rect, "", "+ Remplissage" },
    { 7,  p_rect, "", "+ Couleur" },
    { 8,  p_rect, "", "+ Trait" },
    { 9,  p_rect, "", "+ Brosse" },
    { 10, p_rect, "", "+ Bord" },
    {0}
};
static const OwlSignature sig_rrect[]   = {
    { 6,  p_rrect, "", "X1,Y1,X2,Y2,LargBords,HautBords" },
    { 7,  p_rrect, "", "+ Epaisseur" },
    { 8,  p_rrect, "", "+ Remplissage" },
    { 9,  p_rrect, "", "+ Couleur" },
    { 10, p_rrect, "", "+ Trait" },
    { 11, p_rrect, "", "+ Brosse" },
    {0}
};
static const OwlSignature sig_ellipse[] = {
    { 4, p_ellipse, "", "X1,Y1,X2,Y2" },
    { 5, p_ellipse, "", "+ Epaisseur" },
    { 6, p_ellipse, "", "+ Luminosite" },
    { 7, p_ellipse, "", "+ Couleur" },
    { 8, p_ellipse, "", "+ Trait" },
    { 9, p_ellipse, "", "+ Brosse" },
    {0}
};
/* iImprimeImage : 6 entries pour rejet compile-time strict des arities
 * non couvertes par les 3 syntaxes canon. */
static const OwlSignature sig_image[]   = {
    { 3,  p_image_pos,  "booleen", "Position seule, taille native" },
    { 4,  p_image_pos,  "booleen", "Position + zoom 0-999 ou iAffichage*" },
    { 5,  p_image_zone, "booleen", "Zone delimitee" },
    { 6,  p_image_zone, "booleen", "Zone + echelle iAffichage*" },
    { 9,  p_image_part, "booleen", "Partie d'image" },
    { 10, p_image_part, "booleen", "Partie d'image + echelle" },
    {0}
};
static const OwlSignature sig_cpol[]    = {{4, p_cree_pol, "", ""}, {0}};
static const OwlSignature sig_pol[]     = {{1, p_pol, "chaine", "Chaine de controle concatenable a iImprime"}, {0}};
/* iPosX/iPosY/iPosH/iPosV : 0 args = getter (reel mm), 1 arg = setter (chaine). */
static const OwlSignature sig_pos[]     = {
    { 0, p_simple, "reel",   "Position courante du curseur (mm)" },
    { 1, p_pos,    "chaine", "Chaine de controle concatenable" },
    {0}
};

/* iTrait : (X1,Y1,X2,Y2) requis + 3 optionnels. */
static const OwlParam p_trait[]   = {{"X1","reel","Debut X (mm)"},{"Y1","reel","Debut Y (mm)"},
                                      {"X2","reel","Fin X (mm)"},{"Y2","reel","Fin Y (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1)"},
                                      {"couleur","entier","Couleur RVB (defaut noir)"},
                                      {"style","entier","iContinu/iTiret/...", &E_ITrait, NULL},{0}};
static const OwlSignature sig_trait[] = {
    { 4, p_trait, "booleen", "X1,Y1,X2,Y2" },
    { 5, p_trait, "booleen", "+ Epaisseur" },
    { 6, p_trait, "booleen", "+ Couleur" },
    { 7, p_trait, "booleen", "+ Style" },
    {0}
};

/* iTraitH : (X1, X2) requis + 3 optionnels (a la position Y du curseur). */
static const OwlParam p_traith[]  = {{"X1","reel","Debut X (mm)"},
                                      {"X2","reel","Fin X (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1)"},
                                      {"couleur","entier","Couleur RVB (defaut noir)"},
                                      {"style","entier","iContinu/iTiret/...", &E_ITrait, NULL},{0}};
static const OwlSignature sig_traith[] = {
    { 2, p_traith, "booleen", "X1,X2" },
    { 3, p_traith, "booleen", "+ Epaisseur" },
    { 4, p_traith, "booleen", "+ Couleur" },
    { 5, p_traith, "booleen", "+ Style" },
    {0}
};

/* iTraitV : (Y1, Y2) requis + 3 optionnels (a la position X du curseur). */
static const OwlParam p_traitv[]  = {{"Y1","reel","Debut Y (mm)"},
                                      {"Y2","reel","Fin Y (mm)"},
                                      {"epaisseur","reel","Points pica (defaut 1)"},
                                      {"couleur","entier","Couleur RVB (defaut noir)"},
                                      {"style","entier","iContinu/iTiret/...", &E_ITrait, NULL},{0}};
static const OwlSignature sig_traitv[] = {
    { 2, p_traitv, "booleen", "Y1,Y2" },
    { 3, p_traitv, "booleen", "+ Epaisseur" },
    { 4, p_traitv, "booleen", "+ Couleur" },
    { 5, p_traitv, "booleen", "+ Style" },
    {0}
};

/* Mesure texte / image */
static const OwlParam p_image_path[] = {{"chemin","chaine","Fichier image"},{0}};
static const OwlSignature sig_largeur_image[] = {{1, p_image_path, "reel", "Largeur en mm a la resolution du doc"}, {0}};
static const OwlSignature sig_hauteur_image[] = {{1, p_image_path, "reel", "Hauteur en mm a la resolution du doc"}, {0}};

/* Conversion pica <-> mm. <Sens> : Vrai=horizontal (defaut), Faux=vertical. */
static const OwlParam p_mm_pica[] = {
    {"valeur","reel","Position en millimetres (resp. pica)"},
    {"sens","booleen","Optionnel : Vrai=horizontal (defaut), Faux=vertical"},
    {0}
};
static const OwlSignature sig_mm_pica[] = {
    { 1, p_mm_pica, "reel", "Conversion (sens horizontal par defaut)" },
    { 2, p_mm_pica, "reel", "Conversion avec sens explicite" },
    {0}
};

/* iPoliceDidot / iMagentaTransparent : 0 args = toggle, 1 arg = pose. */
static const OwlParam p_toggle[] = {{"actif","booleen","Optionnel : Vrai/Faux. Sans arg = toggle"},{0}};
static const OwlSignature sig_toggle[] = {
    { 0, p_simple, "booleen", "Toggle, retourne le nouvel etat" },
    { 1, p_toggle, "booleen", "Pose la valeur, retourne l'etat" },
    {0}
};

/* iSelectPolice : V1 simple, juste 1 arg num pour V1. */
static const OwlParam p_select_pol[] = {
    {"numero","entier","Numero de la police a creer"},
    {0}
};
static const OwlSignature sig_select_pol[] = {{1, p_select_pol, "booleen", "Vrai si validee, Faux si annulee"}, {0}};

/* iListeImprimante : 0 args. */
static const OwlSignature sig_liste_impr[] = {{0, p_simple, "chaine", "Liste imprimantes separees par LF"}, {0}};

/* iInfoImprimante : 0, 1 ou 2 args. */
static const OwlParam p_info_impr[] = {
    {"imprimanteEnCours","booleen","Vrai (defaut) = courante, Faux = imprimante par defaut OS"},
    {"typeInfo","entier","iNomImprimante (defaut), iNomDriver, iNomPort", &E_IInfoImpr, NULL},
    {0}
};
static const OwlSignature sig_info_impr[] = {
    { 0, p_simple,    "chaine", "Nom de l'imprimante courante" },
    { 1, p_info_impr, "chaine", "Imprimante courante ou par defaut" },
    { 2, p_info_impr, "chaine", "Avec type d'info specifique" },
    {0}
};

/* iNationImpression : variadique (1 a 5 args). V1 stub, args ignores. */
static const OwlParam p_nation[] = {
    {"nation","entier","Code nation"},
    {"alphabet","entier","Alphabet (optionnel)"},
    {"langue","entier","Langue (optionnel)"},
    {"sousLangue","entier","Sous-langue (optionnel)"},
    {"modeTri","entier","Mode de tri (optionnel)"},
    {0}
};
static const OwlSignature sig_nation[] = {
    { 1, p_nation, "", "" },
    { 2, p_nation, "", "" },
    { 3, p_nation, "", "" },
    { 4, p_nation, "", "" },
    { 5, p_nation, "", "" },
    {0}
};

/* DEFS non-const : les .impl sont poses par stdlib_pdf_defs_set_impl() (GUI)
 * avant stdlib_pdf_defs_register(). Console laisse .impl=NULL. */
static OwlNativeDef DEFS[] = {
    /* Lifecycle */
    { .canonical="iConfigure",    .nameEN="iConfigure",
      .category="Etats",
      .descFR="Configure l'imprimante pour une impression.",
      .descEN="Configures the printer for a print job.",
      .signatures=sig_iconfigure },
    { .canonical="iDestination",  .nameEN="iDestination",
      .category="Etats",
      .descFR="Lit ou fixe la cible de sortie d'impression (écran, PDF, imprimante).",
      .descEN="Reads or sets the print output target (screen, PDF, printer).",
      .signatures=sig_dest },
    { .canonical="iFinImprime",   .nameEN="iEndPrinting",
      .category="Etats",
      .descFR="Clôt le document courant et vide la file d'attente : les données accumulées partent réellement vers la sortie.",
      .descEN="Closes the current document and flushes the queue: the buffered data is actually sent to the output.",
      .signatures=sig_simple },
    { .canonical="iParam\xc3\xa8trePDF", .canonicalAscii="iParametrePDF",
      .nameEN="iParameterPDF",
      .category="Etats",
      .descFR="Fixe les options du PDF généré : niveau de gris, PDF/A, signets, mots de passe et options de protection. À appeler AVANT iDestination. iPDFNiveauGris et iPDFA1b sont effectifs. Les autres options (signets, chiffrement, PDF/A-3b, attachments) sont actuellement acceptées syntaxiquement mais avec un warning runtime sur stderr.",
      .descEN="Sets PDF generation options: grayscale, PDF/A, bookmarks, passwords and protection options. Must be called BEFORE iDestination. iPDFNiveauGris and iPDFA1b are effective. Other options (bookmarks, encryption, PDF/A-3b, attachments) are currently accepted syntactically but with a runtime warning on stderr.",
      .signatures=sig_parametre_pdf },
    { .canonical="iImprimeEtat",  .nameEN="iPrintReport",
      .category="Etats",
      .descFR="Lance l'impression d'un État déclaré via 'ETAT_X est un État(<params>) ... FIN'. Les paramètres de l'État suivent le nom. Applique les PROPRIETES (Format/Orientation/Marges) sur le document PDF courant puis exécute le code d'initialisation de l'État (son top-level) + EnTêteDocument + HautDePage + Lecture + Filtre + Corps + BasDePage + FinDeDocument + Fermeture. Nécessite iDestination préalable ; le PDF est finalisé par iFinImprime.",
      .descEN="Prints a Report declared via 'ETAT_X is a Report(<params>) ... END'. The Report parameters follow the name. Applies the PROPERTIES (Format/Orientation/Margins) onto the current PDF document, then runs the Report initialization code (its top level) + DocumentHeader + PageHeader + Read + Filter + Body + PageFooter + DocumentFooter + Closing. Requires a prior iDestination; the PDF is finalized by iEndPrinting.",
      .signatures=sig_imprime_etat },
    { .canonical="iImprimeBloc",  .nameEN="iPrintBlock",
      .category="Etats",
      .descFR="Imprime manuellement un bloc nommé de l'État en cours d'impression. Utilisé typiquement depuis PROCEDURE Lecture pour répéter le Corps sur N enregistrements, ou pour déclencher un bloc Itération libre. Ne peut être appelé que pendant l'exécution d'un État (code d'initialisation, Lecture, Filtre, sous-bloc, Fermeture).",
      .descEN="Manually prints a named block of the Report currently printing. Typically used from PROCEDURE Read to repeat the Body over N records, or to trigger a free Iteration block. Can only be called during Report execution (initialization code, Read, Filter, sub-block, Closing).",
      .signatures=sig_imprime_bloc },

    /* Mise en page */
    { .canonical="iMarge", .nameEN="iMargin",
      .category="Etats",
      .descFR="Fixe les marges logiques, cumulées par-dessus la bande physique non imprimable de l'imprimante.",
      .descEN="Sets the logical margins, stacked on top of the printer's non-printable physical band.",
      .signatures=sig_marge },
    { .canonical="iSautePage", .nameEN="iSkipPage",
      .category="Etats",
      .descFR="Insère un saut de page dans le flux et replace le curseur en (0,0) sur la page suivante.",
      .descEN="Pushes a page break into the stream and moves the cursor to (0,0) on the next page.",
      .signatures=sig_simple },
    { .canonical="iSauteLigne", .nameEN="iSkipLine",
      .category="Etats",
      .descFR="Avance le curseur d'un ou plusieurs interlignes, en le replaçant en colonne 0.",
      .descEN="Advances the cursor by one or more line breaks, resetting it to column 0.",
      .signatures=sig_saute },
    { .canonical="iNumPage", .nameEN="iPageNum",
      .category="Etats",
      .descFR="Lit ou impose le numéro de la page courante.",
      .descEN="Reads or forces the current page number.",
      .signatures=sig_inumpage },
    { .canonical="iLargeurPage", .nameEN="iPageWidth",
      .category="Etats",
      .descFR="Largeur utile de la page en mm, marges gauche et droite déduites.",
      .descEN="Usable page width in mm, with left and right margins subtracted.",
      .signatures=sig_real },
    { .canonical="iHauteurPage", .nameEN="iPageHeight",
      .category="Etats",
      .descFR="Hauteur utile de la page en mm, marges haute et basse déduites.",
      .descEN="Usable page height in mm, with top and bottom margins subtracted.",
      .signatures=sig_real },
    { .canonical="iHauteurRestantePage", .nameEN="iPageHeightLeft",
      .category="Etats",
      .descFR="Hauteur encore libre sous le curseur sur la page courante, en mm (marges haute et basse déduites).",
      .descEN="Height still free below the cursor on the current page, in mm (top and bottom margins subtracted).",
      .signatures=sig_real },

    /* Polices + curseur */
    { .canonical="iCr\xc3\xa9""ePolice", .canonicalAscii="iCreePolice", .nameEN="iCreateFont",
      .category="Etats",
      .descFR="Déclare une police d'impression, repérée par un numéro que iPolice réutilise ensuite.",
      .descEN="Declares a printing font, keyed by a number that iPolice (iFont) reuses afterwards.",
      .signatures=sig_cpol },
    { .canonical="iPolice", .nameEN="iFont",
      .category="Etats",
      .descFR="Sélectionne la police par défaut utilisée par iImprime, iImprimeMot, iLargeurTexte. Retourne une chaîne de contrôle concaténable à insérer dans le flux d'impression.",
      .descEN="Selects the default font used by iPrint, iPrintWord, iTextWidth. Returns a concatenable control string to insert into the print stream.",
      .signatures=sig_pol },
    { .canonical="iPosX", .nameEN="iXPos",
      .category="Etats",
      .descFR="Gère la position horizontale (abscisse) du curseur d'impression dans la page. Retourne une chaîne de contrôle concaténable à insérer dans le flux d'impression.",
      .descEN="Manages the horizontal position (X-axis) of the print cursor on the page. Returns a concatenable control string to insert into the print stream.",
      .signatures=sig_pos },
    { .canonical="iPosY", .nameEN="iYPos",
      .category="Etats",
      .descFR="Gère la position verticale (ordonnée) du curseur d'impression dans la page. Retourne une chaîne de contrôle concaténable à insérer dans le flux d'impression.",
      .descEN="Manages the vertical position (Y-axis) of the print cursor on the page. Returns a concatenable control string to insert into the print stream.",
      .signatures=sig_pos },
    { .canonical="iPosH", .nameEN="iHPos",
      .category="Etats",
      .descFR="Obtient ou initialise la position horizontale (colonne) du curseur dans la page. Synonyme historique d'iPosX (sans le 2e arg <Calcul Immédiat>).",
      .descEN="Gets or sets the horizontal position (column) of the cursor on the page. Historical synonym of iPosX (without the 2nd <Immediate Calculation> arg).",
      .signatures=sig_pos },
    { .canonical="iPosV", .nameEN="iVPos",
      .category="Etats",
      .descFR="Obtient ou initialise la position verticale (ligne) du curseur dans la page. Synonyme historique d'iPosY.",
      .descEN="Gets or sets the vertical position (line) of the cursor on the page. Historical synonym of iPosY.",
      .signatures=sig_pos },
    { .canonical="iLargeurTexte", .nameEN="iTextWidth",
      .category="Etats",
      .descFR="Mesure en mm la largeur d'un texte dans la police donnée. Les tokens iPolice/iPosX/iPosY inclus sont d'abord appliqués, la mesure portant ensuite sur le texte nettoyé en police courante.",
      .descEN="Measures in mm the width of a text in the given font. Any embedded iPolice/iPosX/iPosY tokens are applied first, then the measurement runs on the cleaned text in the current font.",
      .signatures=sig_largeur },
    { .canonical="iHauteurTexte", .nameEN="iTextHeight",
      .category="Etats",
      .descFR="Mesure en mm la hauteur de la police d'un texte. Les tokens iPolice inclus sont appliqués avant la mesure, qui ignore les retours chariot.",
      .descEN="Measures in mm the font height of a text. Embedded iPolice tokens are applied before measuring; carriage returns are ignored.",
      .signatures=sig_largeur },

    /* Mesure / conversion */
    { .canonical="iLargeurImage", .nameEN="iImageWidth",
      .category="Etats",
      .descFR="Largeur en mm d'une image à la résolution du document (300 dpi par défaut), sans zoom appliqué.",
      .descEN="Image width in mm at the document resolution (300 dpi default), with no zoom applied.",
      .signatures=sig_largeur_image },
    { .canonical="iHauteurImage", .nameEN="iImageHeight",
      .category="Etats",
      .descFR="Hauteur en mm d'une image à la résolution du document (300 dpi par défaut), sans zoom appliqué.",
      .descEN="Image height in mm at the document resolution (300 dpi default), with no zoom applied.",
      .signatures=sig_hauteur_image },
    { .canonical="iMMVersPica", .nameEN="iMMToPica",
      .category="Etats",
      .descFR="Convertit une coordonnée en millimètres vers les coordonnées système (points pica). 1 pica = 0.353 mm.",
      .descEN="Converts a millimeter coordinate to system coordinates (pica points). 1 pica = 0.353 mm.",
      .signatures=sig_mm_pica },
    { .canonical="iPicaVersMM", .nameEN="iPicaToMM",
      .category="Etats",
      .descFR="Convertit une coordonnée système (points pica) en millimètres. 1 pica = 0.353 mm.",
      .descEN="Converts a system coordinate (pica points) to millimeters. 1 pica = 0.353 mm.",
      .signatures=sig_mm_pica },
    { .canonical="iPoliceDidot", .nameEN="iDidotFont",
      .category="Etats",
      .descFR="Choisit l'unité de hauteur des polices créées via iCreePolice : point DIDOT (Vrai) ou PICA (Faux, défaut). 1 didot = 0.376 mm.",
      .descEN="Picks the font-height unit for fonts built via iCreateFont: DIDOT point (True) or PICA point (False, default). 1 didot = 0.376 mm.",
      .signatures=sig_toggle },
    { .canonical="iMagentaTransparent", .nameEN="iTransparentMagenta",
      .category="Etats",
      .descFR="Traite le magenta pur (#FF00FF) comme couleur transparente pour toutes les impressions d'image suivantes.",
      .descEN="Treats pure magenta (#FF00FF) as a transparent color for every following image print.",
      .signatures=sig_toggle },
    { .canonical="iSelectPolice", .nameEN="iSelectFont",
      .category="Etats",
      /* NB impl (Règle 7 — hors desc user-facing) : seule la forme de base
       * iSelectPolice(<num>) est câblée ; les params optionnels Options/
       * Police/Hauteur/Style/Couleur ne sont pas encore honorés (le dialog
       * utilise ses défauts). */
      .descFR="Affiche le sélecteur de police natif (cross-platform) et construit une police d'impression à partir du choix fait.",
      .descEN="Shows the native cross-platform font picker and builds a printing font from the chosen settings.",
      .signatures=sig_select_pol },

    /* Périphériques */
    { .canonical="iListeImprimante", .nameEN="iListPrinter",
      .category="Etats",
      .descFR="Énumère les imprimantes installées sur le poste, une par ligne (séparateur LF).",
      .descEN="Enumerates the printers installed on the host, one per line (LF separator).",
      .signatures=sig_liste_impr },
    { .canonical="iInfoImprimante", .nameEN="iInfoPrinter",
      .category="Etats",
      .descFR="Renseigne sur l'imprimante courante (Vrai, défaut) ou l'imprimante par défaut (Faux). Info voulue : iNomImprimante (défaut), iNomDriver, iNomPort.",
      .descEN="Reports on the active printer (True, default) or the default printer (False). Requested info: iNomImprimante (default), iNomDriver, iNomPort.",
      .signatures=sig_info_impr },
    { .canonical="iNationImpression", .nameEN="iPrintingNation",
      .category="Etats",
      /* NB impl (hors desc user-facing) : no-op pour l'instant — les params
       * sont acceptés mais sans effet ; à câbler avec les Etats graphiques.
       * En attendant, formater les chaînes via DateVersChaine etc. */
      .descFR="Fixe la locale appliquée au prochain travail d'impression (formats de date et de nombre).",
      .descEN="Sets the locale applied to the next print job (date and number formats).",
      .signatures=sig_nation },

    /* Texte */
    { .canonical="iImprime", .nameEN="iPrint",
      .category="Etats",
      .descFR="Empile la chaîne dans le buffer d'impression, suivie d'un retour à la ligne final. Gère les tokens iPolice/iPosX/iPosY multi-segments et Caract(10) en saut interne ; les sauts de page passent par iSautePage.",
      .descEN="Pushes the string into the print buffer, followed by a trailing line break. Handles multi-segment iFont/iXPos/iYPos tokens and Char(10) as an inline break; page breaks go through iSkipPage.",
      .signatures=sig_imprime },
    { .canonical="iImprimeMot", .nameEN="iPrintWord",
      .category="Etats",
      .descFR="Empile la chaîne dans le buffer d'impression sans retour à la ligne : le curseur reste calé en fin de chaîne. Gère les tokens iPolice/iPosX/iPosY multi-segments.",
      .descEN="Pushes the string into the print buffer with no line break: the cursor stays at the string's end. Handles multi-segment iFont/iXPos/iYPos tokens.",
      .signatures=sig_imprime },
    { .canonical="iImprimeZone", .nameEN="iPrintArea",
      .category="Etats",
      .descFR="Imprime un texte dans une zone rectangulaire avec wrap automatique. Retourne le nombre de caractères imprimés. Alignement combinable iHaut/iBas/iCentreH/iCentreV/iGauche/iDroite. Params optionnels Début/Fin pour imprimer un sous-texte.",
      .descEN="Prints text in a rectangular zone with automatic word wrap. Returns the number of characters printed. Alignment combines iTop/iBottom/iHCenter/iVCenter/iLeft/iRight. Optional Start/End for substring.",
      .signatures=sig_zone },

    /* Geometrie */
    { .canonical="iLigne", .nameEN="iNewLine",
      .category="Etats",
      .descFR="Imprime une ligne aux coordonnées spécifiées (bouts arrondis). Épaisseur en points pica (1 pica = 0.353 mm). Style configurable iContinu/iTiret/iPointillé/iMixte.",
      .descEN="Prints a line at the specified coordinates (rounded ends). Thickness in pica points (1 pica = 0.353 mm). Style configurable iContinu/iTiret/iPointille/iMixte.",
      .signatures=sig_ligne },
    { .canonical="iTrait", .nameEN="iLine",
      .category="Etats",
      .descFR="Imprime un trait aux coordonnées spécifiées avec bouts carrés (vs iLigne arrondis). Ordre des arguments optionnels : Épaisseur, Couleur, Style (différent d'iLigne).",
      .descEN="Prints a line at the specified coordinates with flat caps (vs iLigne rounded). Optional argument order: Thickness, Color, Style (differs from iLigne).",
      .signatures=sig_trait },
    { .canonical="iTraitH", .nameEN="iHLine",
      .category="Etats",
      .descFR="Imprime un trait horizontal entre X1 et X2 à la position Y du curseur courant. Bout carré.",
      .descEN="Prints a horizontal line between X1 and X2 at the current cursor Y position. Flat caps.",
      .signatures=sig_traith },
    { .canonical="iTraitV", .nameEN="iVLine",
      .category="Etats",
      .descFR="Imprime un trait vertical entre Y1 et Y2 à la position X du curseur courant. Bout carré.",
      .descEN="Prints a vertical line between Y1 and Y2 at the current cursor X position. Flat caps.",
      .signatures=sig_traitv },
    { .canonical="iCadre", .nameEN="iBorder",
      .category="Etats",
      .descFR="Imprime un cadre aux coordonnées spécifiées. Épaisseur en points pica. Remplissage 1-200 = gradient luminosité (1-99 clarifié, 100 pur, 101-200 assombri). Trait/Brosse/Bord configurables.",
      .descEN="Prints a frame at the specified coordinates. Thickness in pica points. Fill 1-200 = luminosity gradient (1-99 lighter, 100 pure, 101-200 darker). Configurable Trait/Brush/Border.",
      .signatures=sig_rect },
    { .canonical="iCadreArrondi", .nameEN="iRoundBorder",
      .category="Etats",
      .descFR="Trace un cadre à coins arrondis dans le rectangle donné ; largeur et hauteur des arrondis en mm. Options Trait/Brosse/Remplissage identiques à iCadre (pas de paramètre Bord).",
      .descEN="Draws a rounded-corner frame in the given rectangle; corner width and height in mm. Same Trait/Brush/Fill options as iBorder (no Border parameter).",
      .signatures=sig_rrect },
    { .canonical="iEllipse", .nameEN="iEllipse",
      .category="Etats",
      .descFR="Imprime une ellipse à l'intérieur du rectangle spécifié. Mêmes options de Trait/Brosse/Remplissage que iCadre (sans paramètre Bord).",
      .descEN="Prints an ellipse inscribed in the specified rectangle. Same Trait/Brush/Fill options as iBorder (no Border parameter).",
      .signatures=sig_ellipse },

    /* Image */
    { .canonical="iImprimeImage", .nameEN="iPrintImage",
      .category="Etats",
      .descFR="Empile une image dans le buffer d'impression. 3 formes : position + zoom (3-4 args), zone délimitée (5-6 args), découpe partielle (9-10 args). Mode iAffichageNormal/Homothétique/HomoCentré.",
      .descEN="Pushes an image into the print buffer. 3 forms: position + zoom (3-4 args), bounded zone (5-6 args), partial crop (9-10 args). Mode iDisplayNormal/Homothetic/HomoCentered.",
      .signatures=sig_image },
};

void stdlib_pdf_defs_set_impl(const char* canonical, NativeFn fn) {
    if (canonical == NULL) return;
    int n = (int)(sizeof(DEFS) / sizeof(DEFS[0]));
    for (int i = 0; i < n; i++) {
        OwlNativeDef* d = &DEFS[i];
        if (strcmp(d->canonical, canonical) == 0 ||
            (d->canonicalAscii && strcmp(d->canonicalAscii, canonical) == 0)) {
            d->impl = fn;
            return;
        }
    }
}

void stdlib_pdf_defs_register(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
}
