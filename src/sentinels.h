// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_sentinels_h
#define owl_sentinels_h

#include <stdint.h>
#include <limits.h>

/* ================================================================
 * SENTINEL CONSTANT VALUES
 * ================================================================
 *
 * Certaines constantes OWL coexistent dans le meme espace numerique que
 * des entrees utilisateur naturelles. Exemple : `ExtraitChaine(s, rang,
 * sep)` ou `rang` peut etre soit un entier 1-based ordinaire, soit l'une
 * des constantes magiques `rangPremier/rangSuivant/...`. Si la valeur
 * numerique de la constante tombe dans la plage des rangs realistes (1, 2,
 * 1000...), un utilisateur tapant cet entier par accident declenche le
 * mode magique au lieu du mode rang ordinaire.
 *
 * Solution : les valeurs des constantes sentinelles sont choisies dans la
 * zone "edge" du domaine int32 (autour de INT32_MIN), ou aucun entier
 * naturellement tape ne peut atterrir. Convention :
 *
 *   - Si le canon specifie deja une valeur pour la constante, on
 *     s'aligne strict (zero divergence).
 *   - Sinon, chaque famille de constantes recoit une "base" propre, avec
 *     suffisamment d'espace (16 ou 256 slots) pour grandir.
 *
 * Note : OWL stocke les entiers en int64_t mais les sentinelles tiennent
 * dans int32_t pour rester compatible avec les valeurs canon. Le cast
 * vers int64_t est implicite quand poussees sur la stack VM.
 * ================================================================ */

/* ----------------------------------------------------------------
 * Allocation des bases par famille. Convention :
 *   - 256 slots par famille (offset 0x100 entre bases)
 *   - canon prime quand documente (rang*).
 *   - Les familles BITWISE COMBINABLES (cc*, fListeFichier flags,
 *     httpIgnore*, etc.) NE SONT PAS shiftees — leur invariant `OR`
 *     repose sur la valeur 0 = "aucun flag". Voir la note interne
 *     d'audit des sentinelles pour la liste.
 * ---------------------------------------------------------------- */
#define OWL_SENTINEL_BASE         INT32_MIN
#define OWL_SENTINEL_OFFSET(n)    ((int32_t)(OWL_SENTINEL_BASE + ((n) * 0x100)))

/* rang* — modes de parcours pour ExtraitChaine, ExtraitChaineEntre,
 * PositionOccurrence. Valeurs canon.
 * ATTENTION : ordre dernier/precedent swappe par le canon, on suit. */
#define OWL_RANG_PREMIER     ((int32_t)INT32_MIN)        /* -2147483648 */
#define OWL_RANG_SUIVANT     ((int32_t)(INT32_MIN + 1))  /* -2147483647 */
#define OWL_RANG_PRECEDENT   ((int32_t)(INT32_MIN + 2))  /* -2147483646 */
#define OWL_RANG_DERNIER     ((int32_t)(INT32_MIN + 3))  /* -2147483645 */

/* ssc* — positions pour SansEspace(s, position). 5 modes simples. */
#define OWL_SSC_BASE         OWL_SENTINEL_OFFSET(1)
#define OWL_SSC_DROITE       (OWL_SSC_BASE + 1)
#define OWL_SSC_GAUCHE       (OWL_SSC_BASE + 2)
#define OWL_SSC_EXTERIEUR    (OWL_SSC_BASE + 3)
#define OWL_SSC_INTERIEUR    (OWL_SSC_BASE + 4)
#define OWL_SSC_TOUT         (OWL_SSC_BASE + 5)

/* fp* — origine pour fLit/fSeek/fEcrit. 3 modes simples. */
#define OWL_FP_BASE          OWL_SENTINEL_OFFSET(2)
#define OWL_FP_DEBUT         (OWL_FP_BASE + 0)
#define OWL_FP_COURANT       (OWL_FP_BASE + 1)
#define OWL_FP_FIN           (OWL_FP_BASE + 2)

/* fo* — mode pour fOuvre. Bit-flags combinables (canon fOpen 3036036).
 * 4 familles :
 *   - Accès (mutex, bits 0-3) : foLecture, foEcriture, foLectureEcriture,
 *     foAjout. Défaut canon = foLectureEcriture (= foLecture | foEcriture).
 *   - Création (mutex, bits 4-7) : foCreation (écrase), foCreationSiInexistant.
 *     Absent = ouvrir un fichier existant.
 *   - Blocage / divers (bits 12+) : foBloqueLecture, foBloqueEcriture,
 *     foSuppressionAutomatique, foAccesSequentiel.
 *
 * NOTE : ces constantes ne sont PAS des sentinelles INT32_MIN car elles
 * doivent supporter l'addition arithmétique côté utilisateur (canon
 * `foCreation + foEcriture`). Valeurs petites scopées au mode fOuvre
 * uniquement (consommées exclusivement par fn_fopen). */
#define OWL_FO_LECTURE                0x0001
#define OWL_FO_ECRITURE               0x0002
#define OWL_FO_LECTURE_ECRITURE       0x0003   /* = foLecture | foEcriture */
#define OWL_FO_AJOUT                  0x0004
#define OWL_FO_ACCESS_MASK            0x000F

#define OWL_FO_CREATION               0x0010
#define OWL_FO_CREATION_SI_INEXISTANT 0x0020
#define OWL_FO_CREATION_MASK          0x00F0

#define OWL_FO_BLOQUE_LECTURE         0x1000
#define OWL_FO_BLOQUE_ECRITURE        0x2000
#define OWL_FO_SUPPRESSION_AUTO       0x4000

#define OWL_FO_ACCES_SEQUENTIEL       0x10000

/* ti* — type d'info pour TableauInfo. 11 modes (doc 3075005). */
#define OWL_TI_BASE                  OWL_SENTINEL_OFFSET(4)
#define OWL_TI_NOMBRE_TOTAL          (OWL_TI_BASE + 1)
#define OWL_TI_NOMBRE_DIMENSIONS     (OWL_TI_BASE + 2)
#define OWL_TI_NOMBRE_LIGNES         (OWL_TI_BASE + 3)
#define OWL_TI_NOMBRE_COLONNES       (OWL_TI_BASE + 4)
#define OWL_TI_DIMENSION             (OWL_TI_BASE + 5)
#define OWL_TI_DYNAMIQUE             (OWL_TI_BASE + 6)
#define OWL_TI_TAILLE_ELEMENT        (OWL_TI_BASE + 7)
#define OWL_TI_TAILLE_TOTALE         (OWL_TI_BASE + 8)
#define OWL_TI_TYPE_ELEMENT          (OWL_TI_BASE + 9)
#define OWL_TI_TYPE_CLE              (OWL_TI_BASE + 10)
#define OWL_TI_ASSOCIATIF_DOUBLON    (OWL_TI_BASE + 11)

/* url* — SKIPPED : bitwise combinable (URL_PROTOCOLE | URL_DOMAINE = 9
 * pour extraction multi-parties). Reste dans [1, 512] powers-of-2.
 * OWL_SENTINEL_OFFSET(5) reste libre pour reuse futur. */

/* err* — index dans le tableau ErreurInfo. ~40 entries.
 * NOTE : chaque err* est juste un index entier ; pas de bitwise. */
#define OWL_ERR_BASE         OWL_SENTINEL_OFFSET(6)
/* Allocation explicite par valeur (pour matcher les valeurs courantes). */
#define OWL_ERR_MESSAGE      (OWL_ERR_BASE + 0)
#define OWL_ERR_CODE         (OWL_ERR_BASE + 1)
#define OWL_ERR_LIGNE        (OWL_ERR_BASE + 2)
#define OWL_ERR_COMPLET      (OWL_ERR_BASE + 3)
#define OWL_ERR_TRAITEMENT   (OWL_ERR_BASE + 4)
#define OWL_ERR_PENDANT      (OWL_ERR_BASE + 5)
#define OWL_ERR_PILE         (OWL_ERR_BASE + 6)
#define OWL_ERR_RESUME       (OWL_ERR_BASE + 7)
#define OWL_ERR_DATE_HEURE   (OWL_ERR_BASE + 8)
#define OWL_ERR_ELEMENT      (OWL_ERR_BASE + 9)
#define OWL_ERR_NB_SOUS_ERR  (OWL_ERR_BASE + 10)
#define OWL_ERR_INFO         (OWL_ERR_BASE + 11)
#define OWL_ERR_CODE_SYSTEME    (OWL_ERR_BASE + 12)
#define OWL_ERR_MESSAGE_SYSTEME (OWL_ERR_BASE + 13)
#define OWL_ERR_FONCTION     (OWL_ERR_BASE + 14)  /*  : nom native C */
#define OWL_ERR_CODE_NUM     (OWL_ERR_BASE + 15)  /*  : alias numerique E0NNN */

/* httpParam* — id de param pour HTTPParametre. Plage 0..N. */
#define OWL_HTTP_PARAM_BASE                  OWL_SENTINEL_OFFSET(7)
#define OWL_HTTP_PARAM_REINITIALISE          (OWL_HTTP_PARAM_BASE + 0)
#define OWL_HTTP_PARAM_PROXY                 (OWL_HTTP_PARAM_BASE + 1)
#define OWL_HTTP_PARAM_NO_PROXY              (OWL_HTTP_PARAM_BASE + 2)
#define OWL_HTTP_PARAM_TIMEOUT               (OWL_HTTP_PARAM_BASE + 3)
#define OWL_HTTP_PARAM_USER_AGENT            (OWL_HTTP_PARAM_BASE + 4)
#define OWL_HTTP_PARAM_REDIRECTION           (OWL_HTTP_PARAM_BASE + 5)
#define OWL_HTTP_PARAM_MAX_REDIRECTION       (OWL_HTTP_PARAM_BASE + 6)
#define OWL_HTTP_PARAM_COMPRESSION           (OWL_HTTP_PARAM_BASE + 7)
#define OWL_HTTP_PARAM_VERIF_CERTIFICAT      (OWL_HTTP_PARAM_BASE + 8)
#define OWL_HTTP_PARAM_GARDE_CONNEXION       (OWL_HTTP_PARAM_BASE + 9)

/* httpResult* — id de resultat pour HTTPDonneResultat. 7 entries. */
#define OWL_HTTP_RESULT_BASE OWL_SENTINEL_OFFSET(8)
#define OWL_HTTP_RESULT_CONTENU            (OWL_HTTP_RESULT_BASE + 1)
#define OWL_HTTP_RESULT_CODE_ETAT          (OWL_HTTP_RESULT_BASE + 2)
#define OWL_HTTP_RESULT_ENTETE             (OWL_HTTP_RESULT_BASE + 3)
#define OWL_HTTP_RESULT_CONTENT_TYPE       (OWL_HTTP_RESULT_BASE + 4)
#define OWL_HTTP_RESULT_ENTETE_BRUT        (OWL_HTTP_RESULT_BASE + 5)
#define OWL_HTTP_RESULT_URL                (OWL_HTTP_RESULT_BASE + 6)
#define OWL_HTTP_RESULT_DESC_CODE_ETAT     (OWL_HTTP_RESULT_BASE + 7)

/* http* (methodes) — verbe HTTP pour HTTPRequete. 7 methods. */
#define OWL_HTTP_METHOD_BASE OWL_SENTINEL_OFFSET(9)
#define OWL_HTTP_GET         (OWL_HTTP_METHOD_BASE + 1)
#define OWL_HTTP_POST        (OWL_HTTP_METHOD_BASE + 2)
#define OWL_HTTP_PUT         (OWL_HTTP_METHOD_BASE + 3)
#define OWL_HTTP_DELETE      (OWL_HTTP_METHOD_BASE + 4)
#define OWL_HTTP_HEAD        (OWL_HTTP_METHOD_BASE + 5)
#define OWL_HTTP_PATCH       (OWL_HTTP_METHOD_BASE + 6)
#define OWL_HTTP_COPY        (OWL_HTTP_METHOD_BASE + 7)

/* trt* — niveau gestionnaire HSurErreur. 2 modes. */
#define OWL_TRT_BASE         OWL_SENTINEL_OFFSET(10)
#define OWL_TRT_PROCEDURE        (OWL_TRT_BASE + 1)
#define OWL_TRT_PROCEDURE_PROJET (OWL_TRT_BASE + 2)

/* psd* — format JSONVersChaine + Sérialise/Désérialise. */
#define OWL_PSD_BASE         OWL_SENTINEL_OFFSET(11)
#define OWL_PSD_MISE_EN_FORME    (OWL_PSD_BASE + 1)
#define OWL_PSD_MINIFIE          (OWL_PSD_BASE + 2)
#define OWL_PSD_JSON             (OWL_PSD_BASE + 3)

/* ind* — type d'indirection (IndirectionType). Valeurs canon : le scope
 * dirige la resolution d'un nom calcule dynamiquement vers une fenetre (1),
 * un champ (4) ou une variable globale (5). Ce ne sont PAS des sentinelles :
 * aucune detection interne compiler/vm ne depend de ces valeurs (le scope est
 * valide au compile-time puis jete par OP_POP ; la resolution passe par la
 * table globale unifiee). L'offset 12 reste donc libre. */
#define OWL_IND_WINDOW       1
#define OWL_IND_CHAMP        4
#define OWL_IND_VARIABLE     5

/* img* — options dChargeImage (Lot I.1). */
#define OWL_IMG_BASE         OWL_SENTINEL_OFFSET(13)
#define OWL_IMG_DEFAUT       (OWL_IMG_BASE + 1)
#define OWL_IMG_CONVERTIR    (OWL_IMG_BASE + 2)

/* dEffacer / dSansEffacer / dAvecOpacité — options dDébutDessin. */
#define OWL_DESSIN_BASE          OWL_SENTINEL_OFFSET(14)
#define OWL_DESSIN_EFFACER             (OWL_DESSIN_BASE + 1)   /* défaut */
#define OWL_DESSIN_SANS_EFFACER        (OWL_DESSIN_BASE + 2)
#define OWL_DESSIN_AVEC_OPACITE        (OWL_DESSIN_BASE + 3)

/* Fond* — styles de remplissage dFond (Lot I.3a). */
#define OWL_FOND_PLEIN       0   /* canon : compatibilité valeur 0 */
#define OWL_FOND_TRANSPARENT 1   /* canon : compatibilité valeur 1 */
#define OWL_FOND_HACHURE     2   /* canon : compatibilité valeur 2 */

/* dr* — options de redimensionnement (combinables) et rotation. */
#define OWL_RESIZE_BASE          OWL_SENTINEL_OFFSET(15)
#define OWL_RESIZE_OPT_ETIRE         (OWL_RESIZE_BASE + 1)
#define OWL_RESIZE_OPT_HOMOTHETIQUE  (OWL_RESIZE_BASE + 2)
#define OWL_RESIZE_OPT_HAUTE_QUALITE (OWL_RESIZE_BASE + 4)

#define OWL_ROTATE_BASE          OWL_SENTINEL_OFFSET(16)
#define OWL_ROTATE_OPT_DEFAUT             (OWL_ROTATE_BASE + 0)
#define OWL_ROTATE_OPT_ADAPTE             (OWL_ROTATE_BASE + 1)
#define OWL_ROTATE_OPT_SANS_AGRANDISSEMENT (OWL_ROTATE_BASE + 2)

/* tt* / tc* — SKIPPED : combinables bitwise avec tcc* (low byte = tt/tc
 * direction/mode, high bytes = tcc flags). Le code split via mask 0xFF.
 * Restent dans [1..6] pour preserver l'invariant. */

/* zip* — SKIPPED : ZipArchiveMode bitwise (`ZIP_MODE_WRITE | ZIP_MODE_CREATE`)
 * et ZipArchiveFormat est un enum C interne mappe depuis l'extension du
 * path. Risque de collision pratique faible. Reste en [0..6] (formats) et
 * [1..4] (modes bitwise). */

/* compresse* — algorithme Compresse(). 4 algos. */
#define OWL_COMPRESSE_BASE           OWL_SENTINEL_OFFSET(17)
#define OWL_COMPRESSE_LZW            (OWL_COMPRESSE_BASE + 0)
#define OWL_COMPRESSE_GZIP           (OWL_COMPRESSE_BASE + 1)
#define OWL_COMPRESSE_AUCUN          (OWL_COMPRESSE_BASE + 2)
#define OWL_COMPRESSE_CHAINE_COURTE  (OWL_COMPRESSE_BASE + 3)

/* maskDate* — format date pour DateVersChaine. 6 modes (doc 3027013). */
#define OWL_MASK_DATE_BASE           OWL_SENTINEL_OFFSET(18)
#define OWL_MASK_DATE_EMAIL          (OWL_MASK_DATE_BASE + 1)
#define OWL_MASK_DATE_EMAIL_UTC      (OWL_MASK_DATE_BASE + 2)
#define OWL_MASK_DATE_INTERNET       (OWL_MASK_DATE_BASE + 3)
#define OWL_MASK_DATE_INTERNETUTC    (OWL_MASK_DATE_BASE + 4)
#define OWL_MASK_DATE_SYSTEME        (OWL_MASK_DATE_BASE + 5)
#define OWL_MASK_DATE_DUREE_RELATIVE (OWL_MASK_DATE_BASE + 6)

/* iso8601 — flag NumeroDeSemaine. Sentinel pour eviter collision avec int 1. */
#define OWL_ISO8601                  OWL_SENTINEL_OFFSET(19)

/* ÉlémentCourant / PremierÉlément / DernierÉlément — sentinels pour Supprime
 * sur tableau/assoc/liste pendant ou en dehors d'un POUR TOUT. */
#define OWL_ITER_BASE                OWL_SENTINEL_OFFSET(31)
#define OWL_ELEMENT_COURANT          (OWL_ITER_BASE + 0)
#define OWL_PREMIER_ELEMENT          (OWL_ITER_BASE + 1)
#define OWL_DERNIER_ELEMENT          (OWL_ITER_BASE + 2)

/* colType* — type d'une colonne dans un Champ Table.
 * Servent comme valeur de la propriete 'Type' d'une colonne. */
#define OWL_COL_TYPE_BASE            OWL_SENTINEL_OFFSET(20)
#define OWL_COL_TYPE_TEXTE           (OWL_COL_TYPE_BASE + 0)
#define OWL_COL_TYPE_NUMERIQUE       (OWL_COL_TYPE_BASE + 1)
#define OWL_COL_TYPE_DATE            (OWL_COL_TYPE_BASE + 2)
#define OWL_COL_TYPE_BOOLEEN         (OWL_COL_TYPE_BASE + 3)
#define OWL_COL_TYPE_HEURE           (OWL_COL_TYPE_BASE + 4)
#define OWL_COL_TYPE_DUREE           (OWL_COL_TYPE_BASE + 5)
#define OWL_COL_TYPE_MONETAIRE       (OWL_COL_TYPE_BASE + 6)
#define OWL_COL_TYPE_MOTDEPASSE      (OWL_COL_TYPE_BASE + 7)

/* to* — TableOccurrence info type (doc 3074021). */
#define OWL_TO_BASE                  OWL_SENTINEL_OFFSET(26)
#define OWL_TO_TOTAL                 (OWL_TO_BASE + 0)
#define OWL_TO_AFFICHEE              (OWL_TO_BASE + 1)
#define OWL_TO_COLONNE               (OWL_TO_BASE + 2)
#define OWL_TO_VISIBLE               (OWL_TO_BASE + 3)

/* ts* — TableSelect info type (doc 3074020). */
#define OWL_TS_BASE                  OWL_SENTINEL_OFFSET(27)
#define OWL_TS_LIGNE                 (OWL_TS_BASE + 0)
#define OWL_TS_COLONNE               (OWL_TS_BASE + 1)
#define OWL_TS_LIGNE_CELLULE         (OWL_TS_BASE + 2)
#define OWL_TS_COLONNE_CELLULE       (OWL_TS_BASE + 3)

/* tixy* — TableInfoXY info type (doc 3074007). Distinct du ti*
 * TableauInfo (OWL_TI_BASE) qui partage le prefix court. */
#define OWL_TIXY_BASE                OWL_SENTINEL_OFFSET(28)
#define OWL_TIXY_NUM_LIGNE           (OWL_TIXY_BASE + 0)
#define OWL_TIXY_NUM_COLONNE         (OWL_TIXY_BASE + 1)
#define OWL_TIXY_NOM_COLONNE         (OWL_TIXY_BASE + 2)
#define OWL_TIXY_ORIGINE_ECRAN       (OWL_TIXY_BASE + 3)

/* ta* — TablePosition option (doc 3074022). */
#define OWL_TA_BASE                  OWL_SENTINEL_OFFSET(29)
#define OWL_TA_PREMIER               (OWL_TA_BASE + 0)
#define OWL_TA_VISIBLE               (OWL_TA_BASE + 1)

/* filtre* — TableActiveFiltre type (doc 1000017149). */
#define OWL_FILTRE_BASE              OWL_SENTINEL_OFFSET(30)
#define OWL_FILTRE_CONTIENT          (OWL_FILTRE_BASE + 0)
#define OWL_FILTRE_COMMENCE_PAR      (OWL_FILTRE_BASE + 1)
#define OWL_FILTRE_TERMINE_PAR       (OWL_FILTRE_BASE + 2)
#define OWL_FILTRE_EGAL              (OWL_FILTRE_BASE + 3)
#define OWL_FILTRE_DIFFERENT         (OWL_FILTRE_BASE + 4)
#define OWL_FILTRE_INFERIEUR         (OWL_FILTRE_BASE + 5)
#define OWL_FILTRE_INFERIEUR_EGAL    (OWL_FILTRE_BASE + 6)
#define OWL_FILTRE_SUPERIEUR         (OWL_FILTRE_BASE + 7)
#define OWL_FILTRE_SUPERIEUR_EGAL    (OWL_FILTRE_BASE + 8)
#define OWL_FILTRE_NE_CONTIENT_PAS   (OWL_FILTRE_BASE + 9)
#define OWL_FILTRE_NE_COMMENCE_PAS   (OWL_FILTRE_BASE + 10)
#define OWL_FILTRE_NE_TERMINE_PAS    (OWL_FILTRE_BASE + 11)
/* Filtre multi-valeurs « IN {ensemble} » (checklist facon tableur). Sentinelle
 * INTERNE : non exposee en constante publique (pas d'affordance langage), posee
 * uniquement par le chemin UI (popup checklist) et le hook de test. Valeur =
 * ensemble des valeurs a retenir, jointes par RC dans le store $tblfilters. */
#define OWL_FILTRE_IN                (OWL_FILTRE_BASE + 12)

/* iDestination — destination d'impression. OWL n'expose que la sortie PDF
 * (driver-independant) et l'imprimante systeme (valeur de retour/fallback) ;
 * les destinations HTML/RTF/XLS/email/visualisateur/fichier du panel canon ne
 * sont pas honorees au runtime (cf ). */
#define OWL_IDEST_BASE               OWL_SENTINEL_OFFSET(31)
#define OWL_IDEST_IMPRIMANTE         (OWL_IDEST_BASE + 0)
#define OWL_IDEST_PDF_GENERIQUE      (OWL_IDEST_BASE + 1)
#define OWL_IDEST_PDF                (OWL_IDEST_BASE + 2)
/*  — extension OWL : rasterise chaque page de l'Etat dans un tableau
 * de Image fourni par l'utilisateur (cf ADR a sceller). Pas de fichier disque. */
#define OWL_IDEST_IMAGE_MEMOIRE      (OWL_IDEST_BASE + 3)

/* iCreePolice — style (combinable bitwise) — canon. */
#define OWL_IPOL_BASE                OWL_SENTINEL_OFFSET(32)
#define OWL_IPOL_NORMAL              (OWL_IPOL_BASE + 0)
#define OWL_IPOL_GRAS                (OWL_IPOL_BASE + 1)
#define OWL_IPOL_ITALIQUE            (OWL_IPOL_BASE + 2)
#define OWL_IPOL_SOULIGNE            (OWL_IPOL_BASE + 4)
#define OWL_IPOL_BARRE               (OWL_IPOL_BASE + 8)

/* iCreePolice — famille de police (canon). Le 4e arg peut etre soit
 * un nom de famille en string ("Arial") soit une constante. */
#define OWL_IPOL_FAMILY_BASE         OWL_SENTINEL_OFFSET(33)
/* Familles génériques canon — iSwiss/iRoman/iCourier mappent vers les
 * fontes embarquées Liberation correspondantes. */
#define OWL_IPOL_DEFAUT              (OWL_IPOL_FAMILY_BASE + 0)
#define OWL_IPOL_ROMAN               (OWL_IPOL_FAMILY_BASE + 1)  /* serif */
#define OWL_IPOL_SWISS               (OWL_IPOL_FAMILY_BASE + 2)  /* sans-serif */
#define OWL_IPOL_COURIER             (OWL_IPOL_FAMILY_BASE + 3)  /* monospace */
/* Constantes OWL pour les fontes officiellement embarquees (extension hors
 * canon). Permet de cibler explicitement Liberation ou DejaVu sans passer
 * par le mapping legacy. */
#define OWL_IPOL_LIBERATION_SANS     (OWL_IPOL_FAMILY_BASE + 4)
#define OWL_IPOL_LIBERATION_SERIF    (OWL_IPOL_FAMILY_BASE + 5)
#define OWL_IPOL_LIBERATION_MONO     (OWL_IPOL_FAMILY_BASE + 6)
#define OWL_IPOL_DEJAVU_SANS         (OWL_IPOL_FAMILY_BASE + 7)
/* Aliases familieres Microsoft : valeurs distinctes pour preserver l'intent
 * de l'utilisateur (autocomplete + formatter), mais le rendu Qt fait un
 * fallback automatique vers la fonte Liberation/DejaVu embarquee equivalente
 * via map_legacy_family() (cf src/pdf_qt.cpp). Aucun fichier de police
 * Microsoft n'est embarque — pas de probleme de licence. */
#define OWL_IPOL_ARIAL               (OWL_IPOL_FAMILY_BASE + 10)
#define OWL_IPOL_TIMES_NEW_ROMAN     (OWL_IPOL_FAMILY_BASE + 11)
#define OWL_IPOL_COURIER_NEW         (OWL_IPOL_FAMILY_BASE + 12)
#define OWL_IPOL_VERDANA             (OWL_IPOL_FAMILY_BASE + 13)
#define OWL_IPOL_TAHOMA              (OWL_IPOL_FAMILY_BASE + 14)
#define OWL_IPOL_CALIBRI             (OWL_IPOL_FAMILY_BASE + 15)
#define OWL_IPOL_COMIC_SANS_MS       (OWL_IPOL_FAMILY_BASE + 16)
#define OWL_IPOL_GEORGIA             (OWL_IPOL_FAMILY_BASE + 17)
#define OWL_IPOL_TREBUCHET_MS        (OWL_IPOL_FAMILY_BASE + 18)
#define OWL_IPOL_HELVETICA           (OWL_IPOL_FAMILY_BASE + 19)

/* iLigne / iCadre / iCadreArrondi / iEllipse — style de trait. */
#define OWL_ITRAIT_BASE              OWL_SENTINEL_OFFSET(34)
#define OWL_ITRAIT_CONTINU           (OWL_ITRAIT_BASE + 0)
#define OWL_ITRAIT_TIRET             (OWL_ITRAIT_BASE + 1)
#define OWL_ITRAIT_POINTILLE         (OWL_ITRAIT_BASE + 2)
#define OWL_ITRAIT_MIXTE             (OWL_ITRAIT_BASE + 3)

/* iCadre / iEllipse — style de brosse (remplissage). */
#define OWL_IBROSSE_BASE             OWL_SENTINEL_OFFSET(35)
#define OWL_IBROSSE_PLEINE           (OWL_IBROSSE_BASE + 0)
#define OWL_IBROSSE_HORIZONTALE      (OWL_IBROSSE_BASE + 1)
#define OWL_IBROSSE_VERTICALE        (OWL_IBROSSE_BASE + 2)
#define OWL_IBROSSE_CROISEE          (OWL_IBROSSE_BASE + 3)
#define OWL_IBROSSE_DIAG_DESC        (OWL_IBROSSE_BASE + 4)
#define OWL_IBROSSE_DIAG_MONT        (OWL_IBROSSE_BASE + 5)
#define OWL_IBROSSE_CROISEE_DIAG     (OWL_IBROSSE_BASE + 6)

/* iCadre — bordure visible. */
#define OWL_IBORD_BASE               OWL_SENTINEL_OFFSET(36)
#define OWL_IBORD_TOUS               (OWL_IBORD_BASE + 0)
#define OWL_IBORD_HAUT               (OWL_IBORD_BASE + 1)
#define OWL_IBORD_BAS                (OWL_IBORD_BASE + 2)
#define OWL_IBORD_GAUCHE             (OWL_IBORD_BASE + 3)
#define OWL_IBORD_DROIT              (OWL_IBORD_BASE + 4)
#define OWL_IBORD_AUCUN              (OWL_IBORD_BASE + 5)

/* iImprimeZone — alignement (combinable horizontal + vertical via +).
 * Bitmasks SIMPLES (pas de sentinel base) : doivent s'additionner
 * proprement (iCentreH + iCentreV = 4 + 64 = 68). Bits H et V
 * regroupés sur des nibbles séparés. */
#define OWL_IALIGN_GAUCHE            0x01
#define OWL_IALIGN_DROITE            0x02
#define OWL_IALIGN_CENTRE_H          0x04
#define OWL_IALIGN_HAUT              0x10
#define OWL_IALIGN_BAS               0x20
#define OWL_IALIGN_CENTRE_V          0x40

/* iImprimeImage — echelle / mode d'affichage (canon). */
#define OWL_IECHELLE_BASE            OWL_SENTINEL_OFFSET(38)
#define OWL_IECHELLE_NORMAL          (OWL_IECHELLE_BASE + 0)  /* etire/deforme dans le rect (defaut) */
#define OWL_IECHELLE_HOMOTHETIQUE    (OWL_IECHELLE_BASE + 1)  /* proportionnel ancre haut-gauche */
#define OWL_IECHELLE_HOMOCENTRE      (OWL_IECHELLE_BASE + 2)  /* proportionnel centre */

/* iInfoImprimante — type d'information (canon). */
#define OWL_IINFOIMPR_BASE           OWL_SENTINEL_OFFSET(39)
#define OWL_IINFOIMPR_NOM            (OWL_IINFOIMPR_BASE + 0)  /* iNomImprimante (defaut) */
#define OWL_IINFOIMPR_DRIVER         (OWL_IINFOIMPR_BASE + 1)  /* iNomDriver */
#define OWL_IINFOIMPR_PORT           (OWL_IINFOIMPR_BASE + 2)  /* iNomPort */

/* DSL Etat V2 — formats de page (propriete Etat..Format). */
#define OWL_ETAT_FORMAT_BASE         OWL_SENTINEL_OFFSET(40)
#define OWL_ETAT_FORMAT_A3           (OWL_ETAT_FORMAT_BASE + 0)
#define OWL_ETAT_FORMAT_A4           (OWL_ETAT_FORMAT_BASE + 1)  /* defaut */
#define OWL_ETAT_FORMAT_A5           (OWL_ETAT_FORMAT_BASE + 2)
#define OWL_ETAT_FORMAT_LETTER       (OWL_ETAT_FORMAT_BASE + 3)  /* US Letter 8.5x11 */
#define OWL_ETAT_FORMAT_LEGAL        (OWL_ETAT_FORMAT_BASE + 4)  /* US Legal 8.5x14 */

/* DSL Etat V2 — orientation (propriete Etat..Orientation). */
#define OWL_ETAT_ORIENT_BASE         OWL_SENTINEL_OFFSET(41)
#define OWL_ETAT_ORIENT_PORTRAIT     (OWL_ETAT_ORIENT_BASE + 0)  /* defaut */
#define OWL_ETAT_ORIENT_PAYSAGE      (OWL_ETAT_ORIENT_BASE + 1)

/* iParametrePDF — options du fichier PDF genere (canon).
 * Trois familles : format/encodage, qualite images, signets. */
#define OWL_PDF_PARAM_BASE           OWL_SENTINEL_OFFSET(42)
/* Format/encodage (effet Qt6 partiel). */
#define OWL_PDF_NIVEAU_GRIS          (OWL_PDF_PARAM_BASE + 0)
#define OWL_PDF_A1B                  (OWL_PDF_PARAM_BASE + 2)  /* PDF/A-1b */
#define OWL_PDF_A3B                  (OWL_PDF_PARAM_BASE + 3)  /* PDF/A-3b */
/* Signets / bookmarks. Effet via qpdf (post-traitement). */
#define OWL_PDF_SIGNET_AUCUN         (OWL_PDF_PARAM_BASE + 20)
#define OWL_PDF_SIGNET_PAGE          (OWL_PDF_PARAM_BASE + 21)

/* iParametrePDF — options de protection. Volontairement de PETITS flags
 * (1/2/4/8) plutot que des sentinels INT32_MIN+offset, pour respecter le
 * canon : ces options sont combinables via '+' bitwise dans le code
 * utilisateur ('iProtectionImpression + iProtectionSelection'). Le risque
 * de collision avec un entier littéral 1/2/4/8 user est accepte — c'est
 * canon et on detecte la syntaxe au site d'appel via le contexte
 * (1er arg de iParametrePDF). Aucune option Qt6 native — tout est V2 via
 * qpdf, le runtime warne et continue. */
#define OWL_PDF_PROT_AUCUNE          0
#define OWL_PDF_PROT_IMPRESSION      1
#define OWL_PDF_PROT_MODIFICATION    2
#define OWL_PDF_PROT_SELECTION       4
#define OWL_PDF_PROT_COMMENTAIRE     8
#define OWL_PDF_PROT_MASK            0xFF  /* 8 bits reserves pour les options */

/* HA_* — algorithmes hash pour HashChaine/HashFichier/HashVerifieChaine
 * (doc 1000007111). 13 standard + 17 HMAC. Allocation explicite par
 * famille pour rester lisible. */
#define OWL_HA_BASE                  OWL_SENTINEL_OFFSET(21)

/* Hash standard 0x00..0x3F */
#define OWL_HA_MD5_128               (OWL_HA_BASE + 0x01)
#define OWL_HA_RIPEMD_128            (OWL_HA_BASE + 0x02)
#define OWL_HA_RIPEMD_160            (OWL_HA_BASE + 0x03)
#define OWL_HA_SHA_160               (OWL_HA_BASE + 0x04)  /* SHA-1 */
#define OWL_HA_SHA_256               (OWL_HA_BASE + 0x05)
#define OWL_HA_SHA_256_DOUBLE        (OWL_HA_BASE + 0x06)
#define OWL_HA_SHA_384               (OWL_HA_BASE + 0x07)
#define OWL_HA_SHA_512               (OWL_HA_BASE + 0x08)
#define OWL_HA_SHA3_224              (OWL_HA_BASE + 0x09)
#define OWL_HA_SHA3_256              (OWL_HA_BASE + 0x0A)
#define OWL_HA_SHA3_384              (OWL_HA_BASE + 0x0B)
#define OWL_HA_SHA3_512              (OWL_HA_BASE + 0x0C)

/* HMAC variantes 0x40..0x7F */
#define OWL_HA_HMAC_MD5_128          (OWL_HA_BASE + 0x41)
#define OWL_HA_HMAC_RIPEMD_128       (OWL_HA_BASE + 0x42)
#define OWL_HA_HMAC_RIPEMD_160       (OWL_HA_BASE + 0x43)
#define OWL_HA_HMAC_SHA_160          (OWL_HA_BASE + 0x44)
#define OWL_HA_HMAC_SHA_256          (OWL_HA_BASE + 0x45)
#define OWL_HA_HMAC_SHA_256_DOUBLE   (OWL_HA_BASE + 0x46)
#define OWL_HA_HMAC_SHA_384          (OWL_HA_BASE + 0x47)
#define OWL_HA_HMAC_SHA_512          (OWL_HA_BASE + 0x48)
#define OWL_HA_HMAC_SHA3_224         (OWL_HA_BASE + 0x49)
#define OWL_HA_HMAC_SHA3_256         (OWL_HA_BASE + 0x4A)
#define OWL_HA_HMAC_SHA3_384         (OWL_HA_BASE + 0x4B)
#define OWL_HA_HMAC_SHA3_512         (OWL_HA_BASE + 0x4C)

/* BLAKE2 family (extension OWL, hors doc canon).
 * BLAKE2b est plus rapide que SHA-2/SHA-3 sur CPU 64-bit.
 * RFC 7693. */
#define OWL_HA_BLAKE2B_256           (OWL_HA_BASE + 0x60)
#define OWL_HA_BLAKE2B_384           (OWL_HA_BASE + 0x61)
#define OWL_HA_BLAKE2B_512           (OWL_HA_BASE + 0x62)
#define OWL_HA_HMAC_BLAKE2B_256      (OWL_HA_BASE + 0x63)
#define OWL_HA_HMAC_BLAKE2B_384      (OWL_HA_BASE + 0x64)
#define OWL_HA_HMAC_BLAKE2B_512      (OWL_HA_BASE + 0x65)

/* mask* — masques système pour NumériqueVersChaîne (doc 3024024).
 * canon traite ces constantes comme opaques (Trace les affiche en chaînes
 * magic "NumSystemMask"/"MoneySystemMask$"). OWL les encode en sentinels
 * entiers détectables dans le formatter. */
#define OWL_MASK_NUM_BASE            OWL_SENTINEL_OFFSET(22)
#define OWL_MASK_NOMBRE_SYSTEME      (OWL_MASK_NUM_BASE + 0)
#define OWL_MASK_MONETAIRE_SYSTEME   (OWL_MASK_NUM_BASE + 1)

/* fnc* / fnn* / fnp* / fns* — propriétés du type FormatNumérique
 * (canon). Constantes opaques utilisées comme
 * valeurs des champs Alignement / Notation / Préfixe / Signe. */
#define OWL_FN_BASE                  OWL_SENTINEL_OFFSET(23)
/* Alignement (fnc*) */
#define OWL_FNC_DROITE               (OWL_FN_BASE + 0x10)
#define OWL_FNC_GAUCHE               (OWL_FN_BASE + 0x11)
/* Notation (fnn*) */
#define OWL_FNN_BINAIRE              (OWL_FN_BASE + 0x20)
#define OWL_FNN_DECIMAL              (OWL_FN_BASE + 0x21)
#define OWL_FNN_EXPONENTIEL          (OWL_FN_BASE + 0x22)
#define OWL_FNN_HEXADECIMAL          (OWL_FN_BASE + 0x23)
#define OWL_FNN_OCTAL                (OWL_FN_BASE + 0x24)
/* Préfixe (fnp*) */
#define OWL_FNP_AUTO                 (OWL_FN_BASE + 0x30)
/* Signe (fns*) */
#define OWL_FNS_MOINS_SI_NEGATIF     (OWL_FN_BASE + 0x40)
#define OWL_FNS_PARENTHESES_SI_NEGATIF (OWL_FN_BASE + 0x41)
#define OWL_FNS_TOUJOURS             (OWL_FN_BASE + 0x42)

/* disp* — modes de Disposition (Lot F). Sentinel-zone pour empêcher
 * 'Disposition = 1' (collision int). 4 valeurs canon. */
#define OWL_DISP_BASE                OWL_SENTINEL_OFFSET(43)
#define OWL_DISP_VERTICALE           (OWL_DISP_BASE + 0)
#define OWL_DISP_HORIZONTALE         (OWL_DISP_BASE + 1)
#define OWL_DISP_GRILLE              (OWL_DISP_BASE + 2)
#define OWL_DISP_LIBRE               (OWL_DISP_BASE + 3)

/* ong* — position de la barre d'onglets d'un Champ Onglet (prop Position).
 * Valeurs plates 0..3, alignees sur l'ordre attendu par le backend d'onglets
 * (haut/bas/gauche/droite). ongHaut = 0 = defaut. */
#define OWL_POS_HAUT                 0
#define OWL_POS_BAS                  1
#define OWL_POS_GAUCHE               2
#define OWL_POS_DROITE               3

/* align* — alignement transverse (Lot F). Sentinel-zone. 4 valeurs.
 * Sert pour Alignement enfant ET AlignH/AlignV conteneur. */
#define OWL_ALIGN_BASE               OWL_SENTINEL_OFFSET(44)
#define OWL_ALIGN_DEBUT              (OWL_ALIGN_BASE + 0)
#define OWL_ALIGN_CENTRE             (OWL_ALIGN_BASE + 1)
#define OWL_ALIGN_FIN                (OWL_ALIGN_BASE + 2)
#define OWL_ALIGN_ETIRE              (OWL_ALIGN_BASE + 3)

/* forme* — forme de cadrage d'une icone (Icone..Forme). 3 modes mutuellement
 * exclusifs : aucune (rendu brut), carree (cadre carre), cercle (cadre rond). */
#define OWL_FORME_BASE               OWL_SENTINEL_OFFSET(45)
#define OWL_FORME_AUCUNE             (OWL_FORME_BASE + 0)
#define OWL_FORME_CARREE             (OWL_FORME_BASE + 1)
#define OWL_FORME_CERCLE             (OWL_FORME_BASE + 2)

/* td* — ListeDeplaceLigne / TableDeplaceLigne (canon).
 * Operations : petites valeurs bitmask combinables (tdDeplace + tdEchange + ...).
 * Cibles relatives : sentinels pour ne pas collisioner avec un indice reel. */
/* Operations (parametre <Operation a effectuer>, bitmask combinable) */
#define OWL_TD_DEPLACE               0  /* defaut, pas de bit */
#define OWL_TD_ECHANGE               1
#define OWL_TD_AFFICHE_DESTINATION   2
/* Cibles relatives (parametre <Indice destination>, mutuellement exclusives,
 * dans la zone sentinelle) */
#define OWL_TD_BASE                  OWL_SENTINEL_OFFSET(46)
#define OWL_TD_PREMIER               (OWL_TD_BASE + 10)
#define OWL_TD_DERNIER               (OWL_TD_BASE + 11)
#define OWL_TD_PRECEDENT             (OWL_TD_BASE + 12)
#define OWL_TD_SUIVANT               (OWL_TD_BASE + 13)
#define OWL_TD_PAGE_PRECEDENTE       (OWL_TD_BASE + 14)
#define OWL_TD_PAGE_SUIVANTE         (OWL_TD_BASE + 15)
/* Ligne actuellement selectionnee (utilisable pour <Indice source>). */
#define OWL_LIGNE_SELECTIONNEE       (OWL_TD_BASE + 99)

/* sel* — mode d'affichage du Champ Selecteur (canon).
 * 4 modes : standard, avec sous-libelles, look Vista (image), Vista+sous-lib. */
#define OWL_SELMODE_BASE             OWL_SENTINEL_OFFSET(47)
#define OWL_SELMODE_STANDARD         (OWL_SELMODE_BASE + 0)
#define OWL_SELMODE_SOUSLIB          (OWL_SELMODE_BASE + 1)
#define OWL_SELMODE_VISTA            (OWL_SELMODE_BASE + 2)
#define OWL_SELMODE_VISTA_SOUSLIB    (OWL_SELMODE_BASE + 3)

/* zr* — sens de repetition Champ ZoneRepetee (Vertical seul). */
#define OWL_ZRSENS_BASE              OWL_SENTINEL_OFFSET(48)
#define OWL_ZRSENS_VERTICAL          (OWL_ZRSENS_BASE + 0)

/* sqlBackend — backend SQL d'une sqlConnexion (canon Pilote). */
#define OWL_SQL_BACKEND_BASE         OWL_SENTINEL_OFFSET(49)
#define OWL_SQL_SQLITE               (OWL_SQL_BACKEND_BASE + 1)
#define OWL_SQL_POSTGRES             (OWL_SQL_BACKEND_BASE + 2)
#define OWL_SQL_MYSQL                (OWL_SQL_BACKEND_BASE + 3)
#define OWL_SQL_DUCKDB               (OWL_SQL_BACKEND_BASE + 4)
#define OWL_SQL_MSSQL                (OWL_SQL_BACKEND_BASE + 5)
#define OWL_SQL_ODBC                 (OWL_SQL_BACKEND_BASE + 6)
#define OWL_SQL_HFSQL                (OWL_SQL_BACKEND_BASE + 7)
#define OWL_SQL_HFSQL_CLASSIC        (OWL_SQL_BACKEND_BASE + 8)

/* tlsModes — niveau TLS d'une sqlConnexion (aligne Postgres sslmode). */
#define OWL_SQL_TLS_BASE             OWL_SENTINEL_OFFSET(50)
#define OWL_SQL_TLS_DESACTIVE        (OWL_SQL_TLS_BASE + 0)
#define OWL_SQL_TLS_ACCEPTE_TOUT     (OWL_SQL_TLS_BASE + 1)
#define OWL_SQL_TLS_VERIFIE_CA       (OWL_SQL_TLS_BASE + 2)
#define OWL_SQL_TLS_VERIFIE_COMPLET  (OWL_SQL_TLS_BASE + 3)

/* tronqueType — direction de troncature pour Tronque (canon). */
#define OWL_TRONQUE_BASE             OWL_SENTINEL_OFFSET(51)
#define OWL_TRONQUE_CONSERVER        (OWL_TRONQUE_BASE + 0)
#define OWL_TRONQUE_SUPPRIMER        (OWL_TRONQUE_BASE + 1)

/* TypeBouton — type Bouton (canon btn*). */
#define OWL_BTN_BASE                 OWL_SENTINEL_OFFSET(52)
#define OWL_BTN_NORMAL               (OWL_BTN_BASE + 0)
#define OWL_BTN_VALIDER              (OWL_BTN_BASE + 1)
#define OWL_BTN_INTERRUPTION         (OWL_BTN_BASE + 2)
#define OWL_BTN_ABANDON              (OWL_BTN_BASE + 3)
#define OWL_BTN_AIDE                 (OWL_BTN_BASE + 4)

/* TypeLibellé — wrap mono/multi-ligne (TL*). */
#define OWL_TL_BASE                  OWL_SENTINEL_OFFSET(53)
#define OWL_TL_NORMAL                (OWL_TL_BASE + 0)
#define OWL_TL_MULTILIGNE            (OWL_TL_BASE + 1)

/* TypeSaisie — type de champ saisie (S*). */
#define OWL_TYPESAISIE_BASE          OWL_SENTINEL_OFFSET(54)
#define OWL_TYPESAISIE_TEXTE         (OWL_TYPESAISIE_BASE + 0)
#define OWL_TYPESAISIE_MOTDEPASSE    (OWL_TYPESAISIE_BASE + 1)
#define OWL_TYPESAISIE_MULTILIGNE    (OWL_TYPESAISIE_BASE + 2)
#define OWL_TYPESAISIE_NUM           (OWL_TYPESAISIE_BASE + 3)
#define OWL_TYPESAISIE_MONETAIRE     (OWL_TYPESAISIE_BASE + 4)
#define OWL_TYPESAISIE_DATE          (OWL_TYPESAISIE_BASE + 5)
#define OWL_TYPESAISIE_HEURE         (OWL_TYPESAISIE_BASE + 6)
#define OWL_TYPESAISIE_DUREE         (OWL_TYPESAISIE_BASE + 7)
#define OWL_TYPESAISIE_NUM_PRECIS    (OWL_TYPESAISIE_BASE + 8)

/* CadrageHorizontal — alignement horizontal (ch*). */
#define OWL_CH_BASE                  OWL_SENTINEL_OFFSET(55)
#define OWL_CH_GAUCHE                (OWL_CH_BASE + 0)
#define OWL_CH_CENTRE                (OWL_CH_BASE + 1)
#define OWL_CH_DROITE                (OWL_CH_BASE + 2)

/* CadrageVertical — alignement vertical (cv*). */
#define OWL_CV_BASE                  OWL_SENTINEL_OFFSET(56)
#define OWL_CV_HAUT                  (OWL_CV_BASE + 0)
#define OWL_CV_CENTRE                (OWL_CV_BASE + 1)
#define OWL_CV_BAS                   (OWL_CV_BASE + 2)

/* Cadrage image (cad*) — mode de remplissage Image dans Libelle. */
#define OWL_CADIMG_BASE              OWL_SENTINEL_OFFSET(57)
#define OWL_CADIMG_CENTRE            (OWL_CADIMG_BASE + 0)
#define OWL_CADIMG_ETIRE             (OWL_CADIMG_BASE + 1)
#define OWL_CADIMG_HOMOTHETIQUE      (OWL_CADIMG_BASE + 2)

/* Etat — etat d'interactivite d'un champ. */
#define OWL_ETAT_ACTIF_BASE          OWL_SENTINEL_OFFSET(58)
#define OWL_ETAT_ACTIF               (OWL_ETAT_ACTIF_BASE + 0)
#define OWL_ETAT_AFFICHAGE_SEUL      (OWL_ETAT_ACTIF_BASE + 1)  /* alias: Inactif */
#define OWL_ETAT_GRISE               (OWL_ETAT_ACTIF_BASE + 2)
#define OWL_ETAT_INVISIBLE           (OWL_ETAT_ACTIF_BASE + 3)
/* AffichageSansSelection — Table uniquement : affichee, aucune selection
 * utilisateur (souris/fleches) ; la selection par programmation reste
 * possible (TableSelectPlus). No-op sur les autres kinds. */
#define OWL_ETAT_AFFICHAGE_SANS_SELECTION (OWL_ETAT_ACTIF_BASE + 4)

/* CurseurSouris (cur*) — mappe sur Qt::CursorShape (ui_qt.cpp). */
#define OWL_CUR_BASE                 OWL_SENTINEL_OFFSET(59)
#define OWL_CUR_FLECHE               (OWL_CUR_BASE + 0)
#define OWL_CUR_MAIN                 (OWL_CUR_BASE + 1)
#define OWL_CUR_MAIN_SYSTEME         (OWL_CUR_BASE + 2)
#define OWL_CUR_CARET                (OWL_CUR_BASE + 3)
#define OWL_CUR_CROIX                (OWL_CUR_BASE + 4)
#define OWL_CUR_SABLIER              (OWL_CUR_BASE + 5)
#define OWL_CUR_AIDE                 (OWL_CUR_BASE + 6)
#define OWL_CUR_INTERDIT             (OWL_CUR_BASE + 7)
#define OWL_CUR_HFLECHE              (OWL_CUR_BASE + 8)
#define OWL_CUR_TAILLE               (OWL_CUR_BASE + 9)
#define OWL_CUR_TAILLE_NS            (OWL_CUR_BASE + 10)
#define OWL_CUR_TAILLE_OE            (OWL_CUR_BASE + 11)
#define OWL_CUR_TAILLE_NESO          (OWL_CUR_BASE + 12)
#define OWL_CUR_TAILLE_NOSE          (OWL_CUR_BASE + 13)

/* ellipse — mode d'ellipsis sur texte tronque. */
#define OWL_ELL_BASE                 OWL_SENTINEL_OFFSET(60)
#define OWL_ELL_INACTIVE             (OWL_ELL_BASE + 0)
#define OWL_ELL_FIN                  (OWL_ELL_BASE + 1)
#define OWL_ELL_CHEMIN               (OWL_ELL_BASE + 2)

/* sensEcriture (se*) — direction LTR/RTL d'un champ. */
#define OWL_SE_BASE                  OWL_SENTINEL_OFFSET(61)
#define OWL_SE_DEFAUT                (OWL_SE_BASE + 0)
#define OWL_SE_GAUCHE_DROITE         (OWL_SE_BASE + 1)
#define OWL_SE_DROITE_GAUCHE         (OWL_SE_BASE + 2)

/* traitCadre — style de trait d'un Cadre. */
#define OWL_TC_BASE                  OWL_SENTINEL_OFFSET(62)
#define OWL_TC_CONTINU               (OWL_TC_BASE + 0)
#define OWL_TC_AUCUN                 (OWL_TC_BASE + 1)
#define OWL_TC_TIRET                 (OWL_TC_BASE + 2)
#define OWL_TC_POINTILLE             (OWL_TC_BASE + 3)
#define OWL_TC_DOUBLE                (OWL_TC_BASE + 4)

/* formatAffichage — interpretation texte (literal vs markdown OWL). */
#define OWL_FAFF_BASE                OWL_SENTINEL_OFFSET(63)
#define OWL_FAFF_TEXTE               (OWL_FAFF_BASE + 0)
#define OWL_FAFF_TEXTE_RICHE         (OWL_FAFF_BASE + 1)

/* toast* — durée d'affichage pour ToastAffiche. 2 modes (court/long).
 * Sentinel-zone : empêche `ToastAffiche("x", 1)` de tomber par accident
 * dans la zone enum. Durées en ms : divergence assumée vs canon
 * « dépend de l'appareil » (cf docs/VISION.md §5bis). */
#define OWL_TOAST_BASE               OWL_SENTINEL_OFFSET(64)
#define OWL_TOAST_COURT              (OWL_TOAST_BASE + 0)
#define OWL_TOAST_LONG               (OWL_TOAST_BASE + 1)

/* Durées effectives en millisecondes (alignement Android Toast SHORT/LONG). */
#define OWL_TOAST_COURT_MS           2000
#define OWL_TOAST_LONG_MS            5000

/* uuidV* — version pour DonneUUID(version). 2 modes (v4 random, v7 timestamp).
 * Sentinel-zone pour empêcher `DonneUUID(4)` ou `DonneUUID(7)` de tomber par
 * accident dans la zone enum. v4 = défaut (cohérent avec DonneUUID() sans arg). */
#define OWL_UUID_VERSION_BASE        OWL_SENTINEL_OFFSET(65)
#define OWL_UUID_V4                  (OWL_UUID_VERSION_BASE + 0)
#define OWL_UUID_V7                  (OWL_UUID_VERSION_BASE + 1)

/* uuidFormat* — format de sérialisation pour UUID..VersChaîne(format).
 * 3 modes : standard (36 chars à tirets), compact (32 hex pur),
 * accolades (38 chars legacy Microsoft). Sentinel-zone. */
#define OWL_UUID_FORMAT_BASE         OWL_SENTINEL_OFFSET(66)
#define OWL_UUID_FORMAT_STANDARD     (OWL_UUID_FORMAT_BASE + 0)
#define OWL_UUID_FORMAT_COMPACT      (OWL_UUID_FORMAT_BASE + 1)
#define OWL_UUID_FORMAT_ACCOLADES    (OWL_UUID_FORMAT_BASE + 2)

/* ActionSouris — action utilisateur souris simulee par TestSimuleSouris.
 * 9 valeurs sentinel-zone : 4 actions sans coords (clic/dclic G/D)
 * + 5 actions avec coords (bouton G/D, survol, molette haut/bas), ces
 * dernieres utilisees avec la surcharge 4-args (champ, action, x, y).
 * Empeche `TestSimuleSouris(BTN, 1)` de tomber par accident dans la zone. */
#define OWL_ACTION_SOURIS_BASE       OWL_SENTINEL_OFFSET(67)
#define OWL_ACTION_SOURIS_CLIC_G     (OWL_ACTION_SOURIS_BASE + 0)
#define OWL_ACTION_SOURIS_CLIC_D     (OWL_ACTION_SOURIS_BASE + 1)
#define OWL_ACTION_SOURIS_DCLIC_G    (OWL_ACTION_SOURIS_BASE + 2)
#define OWL_ACTION_SOURIS_DCLIC_D    (OWL_ACTION_SOURIS_BASE + 3)
#define OWL_ACTION_SOURIS_BTN_G      (OWL_ACTION_SOURIS_BASE + 4)
#define OWL_ACTION_SOURIS_BTN_D      (OWL_ACTION_SOURIS_BASE + 5)
#define OWL_ACTION_SOURIS_SURVOL     (OWL_ACTION_SOURIS_BASE + 6)
#define OWL_ACTION_SOURIS_MOLETTE_H  (OWL_ACTION_SOURIS_BASE + 7)
#define OWL_ACTION_SOURIS_MOLETTE_B  (OWL_ACTION_SOURIS_BASE + 8)

/* PoliceEtirement — etirement horizontal des caracteres d'un champ. 4 valeurs
 * sentinel-zone, mutuellement exclusives (le reglage est un etat unique, pas
 * un cumul). etirNormal = etat par defaut. Les commentaires donnent la valeur
 * de stretch appliquee au moteur de rendu (100 = nominal). */
#define OWL_ETIR_BASE                OWL_SENTINEL_OFFSET(68)
#define OWL_ETIR_NORMAL              (OWL_ETIR_BASE + 0)  /* stretch 100 */
#define OWL_ETIR_CONDENSE            (OWL_ETIR_BASE + 1)  /* stretch 75  */
#define OWL_ETIR_ETENDU             (OWL_ETIR_BASE + 2)  /* stretch 125 */
#define OWL_ETIR_LARGE               (OWL_ETIR_BASE + 3)  /* stretch 150 */

/* theme* — thème de rendu appliqué lors d'une capture d'image
 * (dCopieImageFenêtre / dCopieImageChamp, ). En Lot 1 ces
 * valeurs sont acceptées mais no-op : la palette OS courante est
 * toujours utilisée. L'activation effective vient au Lot 3. */
#define OWL_THEME_BASE               OWL_SENTINEL_OFFSET(69)
#define OWL_THEME_DEFAUT             (OWL_THEME_BASE + 0)
#define OWL_THEME_CLAIR              (OWL_THEME_BASE + 1)
#define OWL_THEME_SOMBRE             (OWL_THEME_BASE + 2)

/* débordement* — mode de débordement pour EntierDansPlage / EntierVersOctet /
 * BufferÉcritOctet. 4 constantes partagées par 2 enums (entierDébordement,
 * octetDébordement). Sentinel-zone : empêche `EntierDansPlage(v,0,255, 0)`
 * de tomber par accident dans la zone enum. octetFaible n'a de sens que
 * sur une plage 8 bits, d'où son absence de l'enum plage. */
#define OWL_OVERFLOW_BASE            OWL_SENTINEL_OFFSET(70)
#define OWL_OVERFLOW_ERREUR          (OWL_OVERFLOW_BASE + 0)  /* défaut : lève une exception */
#define OWL_OVERFLOW_CYCLIQUE        (OWL_OVERFLOW_BASE + 1)  /* repli modulo positif dans la plage */
#define OWL_OVERFLOW_BORNE           (OWL_OVERFLOW_BASE + 2)  /* clamp vers la borne violée */
#define OWL_OVERFLOW_OCTET_FAIBLE    (OWL_OVERFLOW_BASE + 3)  /* & 0xFF (octet only) */

/* crypte* — selection algo / mode / remplissage pour CrypteStandard et
 * DecrypteStandard. Trois familles mutuellement exclusives, regroupees dans
 * un meme offset (256 slots) avec un sous-decoupage par tranche de 16 :
 *   - algorithme   : tranche +0x00
 *   - mode         : tranche +0x10
 *   - remplissage  : tranche +0x20
 * Sentinel-zone (force_small_int=false) : empeche `CrypteStandard(m, c, 1)`
 * de tomber par accident dans la zone enum. */
#define OWL_CRYPTE_BASE              OWL_SENTINEL_OFFSET(71)
/* Algorithme */
#define OWL_CRYPTE_AES128           (OWL_CRYPTE_BASE + 0x00)
#define OWL_CRYPTE_AES256           (OWL_CRYPTE_BASE + 0x01)
/* Mode d'operation */
#define OWL_CRYPTE_MODE_CBC         (OWL_CRYPTE_BASE + 0x10)
#define OWL_CRYPTE_MODE_CFB         (OWL_CRYPTE_BASE + 0x11)
#define OWL_CRYPTE_MODE_CTR         (OWL_CRYPTE_BASE + 0x12)
#define OWL_CRYPTE_MODE_GCM         (OWL_CRYPTE_BASE + 0x13)
/* Remplissage (pertinent CBC ; modes flux et GCM = sans padding) */
#define OWL_CRYPTE_PAD_PKCS         (OWL_CRYPTE_BASE + 0x20)
#define OWL_CRYPTE_PAD_ZERO         (OWL_CRYPTE_BASE + 0x21)

/* fsel* — mode d'ouverture du selecteur de fichiers (fSelecteur). Bitmask
 * combinable (canon : defaut = fselOuvre + fselExiste). Famille bitwise :
 * petites valeurs positives, PAS shiftees en zone sentinelle (l'invariant OR
 * repose sur des bits propres ; cf convention combinables en tete de fichier).
 * Sous-ensemble assume : les modes Windows-legacy (fselChange / fCheminUNICODE)
 * ne sont pas exposes. */
#define OWL_FSEL_OUVRE               1  /* bouton "Ouvrir" */
#define OWL_FSEL_CREE                2  /* bouton "Enregistrer" */
#define OWL_FSEL_EXISTE              4  /* impose un fichier existant */
#define OWL_FSEL_MULTI               8  /* multiselection (chemins separes par RC) */

/* arrondi* — tie-rule (regle de depart) pour Arrondi(valeur, [decimales], [mode]).
 * Tie-rules uniquement (pas de floor/ceil : la direction est portee par la
 * famille PartieEntiere / ArrondiSuperieur). demi-pair = defaut moderne OWL
 * (banker's, sans biais cumule), divergence assumee vs canon historique.
 * demi-eloigne = ties-away-from-zero (echappatoire pour code porte).
 * Sentinel-zone : empeche `Arrondi(v, 0, 1)` de tomber dans la zone enum. */
#define OWL_ROUND_BASE               OWL_SENTINEL_OFFSET(72)
#define OWL_ROUND_HALF_EVEN          (OWL_ROUND_BASE + 0)  /* demi-pair (defaut) */
#define OWL_ROUND_HALF_AWAY          (OWL_ROUND_BASE + 1)  /* demi-eloigne (away from zero) */

/* enc* — encodage cible/source pour ChaineVersBuffer / BufferVersChaine.
 * Frontiere charset du modele sandwich UTF-8 : le texte (chaine) reste
 * abstrait, l'encodage est une propriete des octets (buffer). 5 codepages
 * mutuellement exclusifs. Sentinel-zone : empeche `ChaineVersBuffer(s, 1)`
 * de tomber par accident dans la zone enum. encUTF8 = defaut (passthrough,
 * OWL est UTF-8 en interne). CP850/CP437 = codepages OEM/DOS (console
 * historique, vieux exports comptables). */
#define OWL_ENC_BASE                 OWL_SENTINEL_OFFSET(73)
#define OWL_ENC_UTF8                 (OWL_ENC_BASE + 0)  /* defaut, passthrough */
#define OWL_ENC_UTF16LE              (OWL_ENC_BASE + 1)
#define OWL_ENC_UTF16BE              (OWL_ENC_BASE + 2)
#define OWL_ENC_LATIN1               (OWL_ENC_BASE + 3)  /* ISO-8859-1 */
#define OWL_ENC_WINDOWS1252          (OWL_ENC_BASE + 4)
#define OWL_ENC_CP850                (OWL_ENC_BASE + 5)  /* OEM DOS Europe Ouest */
#define OWL_ENC_CP437                (OWL_ENC_BASE + 6)  /* OEM DOS US */

/* posBtnImage* — position de l'icone par rapport au texte d'un Champ Bouton
 * (feuille de style Style..Image..Position, enum E_PositionImageBouton).
 * 6 valeurs mutuellement exclusives, sentinel-zone : empeche
 * `Style..Image..Position = 1` de tomber par accident dans la zone enum.
 * posBtnImageGauche = defaut (natif Qt : icone a gauche du texte). Les deux
 * dernieres masquent un cote (icone seule / texte seul). */
#define OWL_POSBTNIMG_BASE           OWL_SENTINEL_OFFSET(75)
#define OWL_POSBTNIMG_GAUCHE         (OWL_POSBTNIMG_BASE + 0)  /* defaut */
#define OWL_POSBTNIMG_DROITE         (OWL_POSBTNIMG_BASE + 1)
#define OWL_POSBTNIMG_HAUT           (OWL_POSBTNIMG_BASE + 2)
#define OWL_POSBTNIMG_BAS            (OWL_POSBTNIMG_BASE + 3)
#define OWL_POSBTNIMG_SEULE          (OWL_POSBTNIMG_BASE + 4)  /* icone seule */
#define OWL_POSBTNIMG_TEXTE_SEUL     (OWL_POSBTNIMG_BASE + 5)  /* texte seul */

/* boutisme* — ordre des octets en sortie pour EntierVersBuffer /
 * RéelVersBuffer. La taille/le boutisme sont des proprietes des octets
 * produits, jamais des attributs du type entier/reel. Petit-boutiste =
 * defaut (cible Windows/x64, deterministe — jamais "natif"). Sentinel-zone :
 * empeche `EntierVersBuffer(n, 4, 1)` de tomber par accident dans la zone enum. */
#define OWL_ENDIAN_BASE              OWL_SENTINEL_OFFSET(74)
#define OWL_ENDIAN_LITTLE            (OWL_ENDIAN_BASE + 0)  /* poids faible en premier (defaut) */
#define OWL_ENDIAN_BIG               (OWL_ENDIAN_BASE + 1)  /* poids fort en premier */

/* orientationsTable — orientation de disposition d'un Champ Table.
 * V1 : une seule valeur (Vertical, lignes empilees). Sentinel-zone : empeche
 * `Orientation = 0` de tomber par accident dans la zone enum. */
#define OWL_ORIENT_TABLE_BASE        OWL_SENTINEL_OFFSET(76)
#define OWL_ORIENT_TABLE_VERTICAL    (OWL_ORIENT_TABLE_BASE + 0)

/* etatsTableInitial — etat initial d'interactivite d'un Champ Table
 * (saisie / grise / selection sans saisie / ni saisie ni selection). Distinct
 * de l'enum `etat` (par champ generique). Sentinel-zone. */
#define OWL_ETAT_TABLE_BASE          OWL_SENTINEL_OFFSET(77)
#define OWL_ETAT_TABLE_EN_SAISIE     (OWL_ETAT_TABLE_BASE + 0)
#define OWL_ETAT_TABLE_SELECTION     (OWL_ETAT_TABLE_BASE + 1)
#define OWL_ETAT_TABLE_NI_NI         (OWL_ETAT_TABLE_BASE + 2)
/* La valeur Grisé reutilise le sentinel de l'enum etat (OWL_ETAT_GRISE) : un
 * seul const Grisé pour tout le registry (rule 5 enum_value_orphan). */

/* typeDégradé — type de remplissage dégradé d'un CadreExtérieur
 * (aucun / linéaire / radial). dégradéAucun = défaut : pas de dégradé, le fond
 * uni (CouleurFond) reste. Gate d'activation par enum (évite tout sentinel sur
 * les couleurs, 0 = noir étant une couleur valide). Sentinel-zone : empêche
 * `Dégradé..Type = 0` de tomber par accident dans la zone enum. */
#define OWL_GRAD_BASE                OWL_SENTINEL_OFFSET(78)
#define OWL_GRAD_AUCUN               (OWL_GRAD_BASE + 0)  /* défaut : pas de dégradé */
#define OWL_GRAD_LINEAIRE            (OWL_GRAD_BASE + 1)
#define OWL_GRAD_RADIAL              (OWL_GRAD_BASE + 2)

/* CB_* — symbologie d'un CodeBarres (propriete TypeCodeBarres, ).
 * 25 constantes mutuellement exclusives (EAN128 et GS1_128 = meme symbologie,
 * deux noms conserves ; 7 extensions OWL hors gamme canon en fin de bloc).
 * Sentinel-zone : empeche `TypeCodeBarres = 1` de tomber
 * par accident dans la zone enum. CB_QRCODE = defaut. Le mapping vers le code
 * symbologie du moteur de rendu se fait dans stdlib_codebarres.c. */
#define OWL_CB_BASE                  OWL_SENTINEL_OFFSET(79)
#define OWL_CB_QRCODE                (OWL_CB_BASE + 0)   /* defaut */
#define OWL_CB_DATAMATRIX            (OWL_CB_BASE + 1)
#define OWL_CB_PDF417                (OWL_CB_BASE + 2)
#define OWL_CB_AZTEC                 (OWL_CB_BASE + 3)
#define OWL_CB_MAXICODE              (OWL_CB_BASE + 4)
#define OWL_CB_UPCA                  (OWL_CB_BASE + 5)
#define OWL_CB_UPCE                  (OWL_CB_BASE + 6)
#define OWL_CB_EAN13                 (OWL_CB_BASE + 7)
#define OWL_CB_EAN8                  (OWL_CB_BASE + 8)
#define OWL_CB_EAN128                (OWL_CB_BASE + 9)   /* = GS1_128 (ancien nom) */
#define OWL_CB_GS1_128               (OWL_CB_BASE + 10)
#define OWL_CB_CODE128               (OWL_CB_BASE + 11)
#define OWL_CB_CODE39                (OWL_CB_BASE + 12)
#define OWL_CB_CODE93                (OWL_CB_BASE + 13)
#define OWL_CB_CODE11                (OWL_CB_BASE + 14)
#define OWL_CB_INTER2OF5             (OWL_CB_BASE + 15)
#define OWL_CB_CODABAR               (OWL_CB_BASE + 16)
#define OWL_CB_MSI                   (OWL_CB_BASE + 17)
/* Extensions OWL hors gamme canon (symbologies populaires de libzint) :
 * GS1 DataBar, Micro QR, rMQR, DotCode, Han Xin, PZN. Pas de constante
 * canon correspondante — naming OWL. */
#define OWL_CB_GS1_DATABAR           (OWL_CB_BASE + 18)
#define OWL_CB_GS1_DATABAR_ETENDU    (OWL_CB_BASE + 19)
#define OWL_CB_MICRO_QR              (OWL_CB_BASE + 20)
#define OWL_CB_RMQR                  (OWL_CB_BASE + 21)
#define OWL_CB_DOTCODE               (OWL_CB_BASE + 22)
#define OWL_CB_HAN_XIN               (OWL_CB_BASE + 23)
#define OWL_CB_PZN                   (OWL_CB_BASE + 24)

/* qrcodeNiveau* — niveau de correction d'erreur d'un QR Code (propriete
 * CorrectionErreur, ). 4 niveaux croissants L<M<Q<H. Sentinel-zone.
 * Le mapping vers option_1 (1..4) du moteur QR se fait au runtime. */
#define OWL_QR_ECC_BASE              OWL_SENTINEL_OFFSET(80)
#define OWL_QR_ECC_L                 (OWL_QR_ECC_BASE + 0)   /* ~7%  */
#define OWL_QR_ECC_M                 (OWL_QR_ECC_BASE + 1)   /* ~15% (defaut QR) */
#define OWL_QR_ECC_Q                 (OWL_QR_ECC_BASE + 2)   /* ~25% */
#define OWL_QR_ECC_H                 (OWL_QR_ECC_BASE + 3)   /* ~30% */

/* cbType* — nature du contenu d'un CodeBarres (propriete TypeContenu,
 * ). V1 : texte brut uniquement. Sentinel-zone. */
#define OWL_CB_CONTENT_BASE          OWL_SENTINEL_OFFSET(81)
#define OWL_CB_CONTENT_TEXTE         (OWL_CB_CONTENT_BASE + 0)  /* defaut */

/* largeurVolets* — mode de largeur des languettes d'un Champ Onglet (prop
 * LargeurVolets). Sentinel-zone pour empecher 'LargeurVolets = 1'. 2 valeurs :
 * proportionnelle (defaut = QTabBar natif, chaque languette selon son libelle)
 * et egale (toutes calees sur la plus large). */
#define OWL_LARGVOL_BASE             OWL_SENTINEL_OFFSET(83)
#define OWL_LARGVOL_PROPORTIONNELLE  (OWL_LARGVOL_BASE + 0)
#define OWL_LARGVOL_EGALE            (OWL_LARGVOL_BASE + 1)

/* evt* — constantes d'evenement widget pour DeclencheEvenement / TriggerEvent
 *. Derivees 1:1 du catalogue d'events widget (strip du prefixe
 * sur/on -> prefixe evt). La valeur = OWL_EVT_BASE + index dedup stable : le
 * runtime decode value -> index -> OwlEventDef (canonical + params). Sentinel
 * zone (256 slots), une base unique pour toute la famille genere a l'init. */
#define OWL_EVT_BASE                 OWL_SENTINEL_OFFSET(82)

/* param* — support de stockage des valeurs persistantes (ParametreInitialise).
 * Sentinel-zone : sans ca `ParametreInitialise(1, ...)` matcherait un support au
 * hasard. paramJSON est le defaut (extension OWL, cf ADR parametres). */
#define OWL_PARAM_BASE               OWL_SENTINEL_OFFSET(84)
#define OWL_PARAM_JSON               (OWL_PARAM_BASE + 0)  /* defaut */
#define OWL_PARAM_XML                (OWL_PARAM_BASE + 1)
#define OWL_PARAM_INI                (OWL_PARAM_BASE + 2)
#define OWL_PARAM_CHAINE             (OWL_PARAM_BASE + 3)

/* init* — strategie d'initialisation des poids d'une couche dense
 * (IAModèle.AjouteCoucheDense, ). initHe (defaut, adapte a ReLU :
 * variance 2/entrees) ou initXavier (adapte a tanh/sigmoide : bornes
 * uniformes ±racine(6/(entrees+sorties))). Sentinel-zone : empeche
 * `AjouteCoucheDense(3, 4, 0)` de tomber par accident sur une strategie. */
#define OWL_INIT_BASE                OWL_SENTINEL_OFFSET(85)
#define OWL_INIT_HE                  (OWL_INIT_BASE + 0)  /* defaut */
#define OWL_INIT_XAVIER              (OWL_INIT_BASE + 1)

/* optim* — algorithme d'optimisation d'un entrainement (propriete
 * OptionsEntrainement..Optimiseur, ). Sentinel-zone : empeche
 * `opts..Optimiseur = 0` de tomber par accident sur un optimiseur. SGD est
 * le seul optimiseur de la phase entrainement de base ; d'autres (moments,
 * correction de biais) etendront cette famille. */
#define OWL_OPTIM_BASE               OWL_SENTINEL_OFFSET(86)
#define OWL_OPTIM_SGD                (OWL_OPTIM_BASE + 0)  /* defaut */
#define OWL_OPTIM_ADAM               (OWL_OPTIM_BASE + 1)  /* moments m/v + correction de biais */

/* perte* — fonction de perte d'un entrainement (propriete
 * OptionsEntrainement..Perte, ). perteEntropieCroisee (defaut,
 * classification multi-classe : mange des logits, softmax fusionnee) ou
 * perteErreurQuadratique (regression : moindres carres). Sentinel-zone. */
#define OWL_PERTE_BASE               OWL_SENTINEL_OFFSET(87)
#define OWL_PERTE_ENTROPIE_CROISEE   (OWL_PERTE_BASE + 0)  /* defaut */
#define OWL_PERTE_ERREUR_QUADRATIQUE (OWL_PERTE_BASE + 1)

/* stop* — raison d'arret d'une generation Chat (propriete RéponseLLM..Raison,
 * ). Enum stricte (l'utilisateur ne fait que lire) : stopNaturel (le
 * modele a emis son jeton de fin) ou stopMaxTokens (plafond ..MaxTokens atteint,
 * reponse coupee). Sentinel-zone : empeche `rep..Raison = 0` de tomber par
 * accident sur une raison ; force la comparaison via les constantes nommees. */
#define OWL_STOP_BASE                OWL_SENTINEL_OFFSET(88)
#define OWL_STOP_NATUREL             (OWL_STOP_BASE + 0)
#define OWL_STOP_MAX_TOKENS          (OWL_STOP_BASE + 1)

/* pi* — type d'information demande a ProjetInfo. Enum extensible (une seule
 * valeur aujourd'hui : le nom du projet). Sentinel-zone : empeche
 * `ProjetInfo(0)` de tomber par accident sur un type d'info. */
#define OWL_PROJETINFO_BASE          OWL_SENTINEL_OFFSET(89)
#define OWL_PROJETINFO_NOM           (OWL_PROJETINFO_BASE + 0)  /* defaut */

/* enum* — cible de la reflexion ElementsDe. Enum extensible (une seule cible
 * aujourd'hui : les fenetres declarees du projet). Sentinel-zone : empeche
 * `ElementsDe(0)` de tomber par accident sur une cible. */
#define OWL_ELEMENTPROJET_BASE       OWL_SENTINEL_OFFSET(90)
#define OWL_ELEMENTPROJET_FENETRE    (OWL_ELEMENTPROJET_BASE + 0)

/* plan* — plan de decroissance du taux d'apprentissage (propriete
 * OptionsEntrainement..PlanTaux, ). planConstant (defaut, taux fixe =
 * comportement historique) ou planCosinus (recuit cosinus : le taux decroit de
 * sa valeur initiale vers ~0 en suivant une demi-cosinus sur le total des
 * epoques). Sentinel-zone : empeche `opts..PlanTaux = 0` de tomber par accident
 * sur un plan. */
#define OWL_PLAN_BASE                OWL_SENTINEL_OFFSET(91)
#define OWL_PLAN_CONSTANT            (OWL_PLAN_BASE + 0)  /* defaut */
#define OWL_PLAN_COSINUS             (OWL_PLAN_BASE + 1)

/* ftpMode* — mode d'ouverture du canal de donnees d'une connexion FTP
 * (propriete ftpClient..Mode). Actif = le serveur se connecte au client ;
 * passif (defaut) = le client se connecte au serveur (traverse les
 * pare-feux/NAT) ; passif etendu = variante EPSV (IPv6 / port unique).
 * Sentinel-zone : empeche `cnx..Mode = 0` de tomber par accident sur un
 * mode. Mutuellement exclusifs. */
#define OWL_FTP_MODE_BASE            OWL_SENTINEL_OFFSET(92)
#define OWL_FTP_MODE_ACTIF           (OWL_FTP_MODE_BASE + 0)
#define OWL_FTP_MODE_PASSIF          (OWL_FTP_MODE_BASE + 1)  /* defaut */
#define OWL_FTP_MODE_PASSIF_ETENDU   (OWL_FTP_MODE_BASE + 2)

/* ftpMode(ASCII|Binaire) — type de transfert FTP passe en argument a
 * FTPEnvoie / FTPRecupere / FTPRecupereFichier. ASCII = conversion des
 * fins de ligne entre systemes (texte) ; binaire (defaut) = octets bruts
 * inchanges. Sentinel-zone, mutuellement exclusifs. */
#define OWL_FTP_TRANSFERT_BASE       OWL_SENTINEL_OFFSET(93)
#define OWL_FTP_TRANSFERT_ASCII      (OWL_FTP_TRANSFERT_BASE + 0)
#define OWL_FTP_TRANSFERT_BINAIRE    (OWL_FTP_TRANSFERT_BASE + 1)  /* defaut */

/* ftp(Accès|Création|Modification) — type de date lu par FTPDate/FTPHeure/
 * FTPDateHeure sur un fichier distant. Modification (defaut) = date de
 * derniere modification (MDTM) ; creation et acces selon la disponibilite
 * serveur (facts MLST). Sentinel-zone : empeche un typeDate tape au pif de
 * tomber sur une valeur. Mutuellement exclusifs. */
#define OWL_FTP_TYPEDATE_BASE        OWL_SENTINEL_OFFSET(94)
#define OWL_FTP_TYPEDATE_ACCES       (OWL_FTP_TYPEDATE_BASE + 0)
#define OWL_FTP_TYPEDATE_CREATION    (OWL_FTP_TYPEDATE_BASE + 1)
#define OWL_FTP_TYPEDATE_MODIFICATION (OWL_FTP_TYPEDATE_BASE + 2)  /* defaut */

/* ftpIgnore* — motifs de relachement de la verification du certificat TLS
 * d'une connexion FTPS/FTPES, passes en masque OU-binaire au dernier
 * parametre de FTPConnecte. COMBINABLES : petites puissances de deux, 0 =
 * "aucun relachement" (verif complete, defaut prod-safe). Volontairement
 * PAS sentinel-shiftes (comme fo*, iProtection*, httpIgnore*) : l'invariant
 * OR repose sur la valeur 0 pour "aucun flag", et le masque est consomme
 * exclusivement par fn_ftp_connecte. */
#define OWL_FTP_IGNORE_CERT_INVALIDE   0x01
#define OWL_FTP_IGNORE_CERT_EXPIRE     0x02
#define OWL_FTP_IGNORE_CERT_NOM        0x04
#define OWL_FTP_IGNORE_REVOCATION      0x08
/* Relachement de la verification de la CLE D'HOTE d'une connexion SFTP
 * (sftp://) : accepte un hote absent du fichier known_hosts. Ne relache
 * PAS un conflit de cle (hote connu avec une cle differente = refus strict,
 * anti-MITM). Meme masque OU-binaire, meme famille bitwise. */
#define OWL_FTP_IGNORE_HOTE_INCONNU    0x10

/* httpAuth* — schema d'authentification d'un service HTTP declaratif
 * (bloc ServiceREST..Authentification). Aucune semantique entiere propre :
 * sentinel-zone pour empecher un schema tape au pif de tomber sur une
 * valeur. Mutuellement exclusifs. */
#define OWL_HTTP_AUTH_BASE           OWL_SENTINEL_OFFSET(95)
#define OWL_HTTP_AUTH_AUCUNE         (OWL_HTTP_AUTH_BASE + 0)
#define OWL_HTTP_AUTH_BEARER         (OWL_HTTP_AUTH_BASE + 1)
#define OWL_HTTP_AUTH_BASIQUE        (OWL_HTTP_AUTH_BASE + 2)
#define OWL_HTTP_AUTH_CLE_API        (OWL_HTTP_AUTH_BASE + 3)
#define OWL_HTTP_AUTH_JWT            (OWL_HTTP_AUTH_BASE + 4)

/* obj* — objectif d'un MLArbreDécision (propriété ..Objectif, ).
 * objClassification (feuille = classe majoritaire, critère Gini) ou
 * objRégression (feuille = moyenne, critère SSE). Aucun défaut : l'objectif
 * DOIT être réglé avant .Entraîne (pas d'inférence implicite du type de
 * tâche). Sentinel-zone : empêche `arbre..Objectif = 0` de tomber par
 * accident sur un objectif — la valeur interne 0 est réservée à "non réglé".
 * Mutuellement exclusifs. */
#define OWL_ML_OBJECTIF_BASE         OWL_SENTINEL_OFFSET(96)
#define OWL_ML_OBJ_CLASSIFICATION    (OWL_ML_OBJECTIF_BASE + 0)
#define OWL_ML_OBJ_REGRESSION        (OWL_ML_OBJECTIF_BASE + 1)

/* amqpTypeÉchange — type d'un échange AMQP (AMQPDéclareÉchange). Mutuellement
 * exclusifs. Sentinel-zone : la valeur interne est mappée vers le nom wire
 * ("direct"/"fanout"/"topic"/"headers") au moment de l'émission. */
#define OWL_AMQP_EXCH_BASE           OWL_SENTINEL_OFFSET(97)
#define OWL_AMQP_EXCH_DIRECT         (OWL_AMQP_EXCH_BASE + 0)
#define OWL_AMQP_EXCH_FANOUT         (OWL_AMQP_EXCH_BASE + 1)
#define OWL_AMQP_EXCH_TOPIC          (OWL_AMQP_EXCH_BASE + 2)
#define OWL_AMQP_EXCH_HEADERS        (OWL_AMQP_EXCH_BASE + 3)

/* fnbAuto — valeur sentinelle du champ ChiffresAprèsVirgule du type
 * FormatNombre : "auto/court", pas un nombre fixe de décimales. Signifie que
 * l'affichage suit la forme plate courte round-trippable (value_to_string),
 * pas un arrondi à N décimales. Distincte de 6 (le défaut du type
 * FormatNumérique, orienté conversion) : une instance FormatNombre par défaut
 * reproduit l'affichage plat actuel, pas "1234.500000". Sentinel-zone pour
 * empêcher qu'un ChiffresAprèsVirgule = <cette valeur> tapé au pif tombe sur
 * le mode auto. */
#define OWL_FNB_AUTO                 OWL_SENTINEL_OFFSET(98)

/* recherche* — type de recherche pour TableCherche. rechercheDéfaut ≡
 * rechercheIdentique (comparaison exacte), distinct pour l'expressivite du
 * code appelant. */
#define OWL_RCHT_BASE                OWL_SENTINEL_OFFSET(99)
#define OWL_RCHT_IDENTIQUE           (OWL_RCHT_BASE + 0)
#define OWL_RCHT_COMMENCE_PAR        (OWL_RCHT_BASE + 1)
#define OWL_RCHT_CONTIENT            (OWL_RCHT_BASE + 2)
#define OWL_RCHT_DEFAUT              (OWL_RCHT_BASE + 3)

/* posAvant / posAprès — position du symbole monétaire dans le type
 * FormatMonétaire (PositionSymbole). Défaut posAprès (canon FR : "1 234,50 €").
 * Sentinel-zone pour empêcher qu'un PositionSymbole = 0/1 tapé au pif tombe sur
 * une valeur magique. */
#define OWL_POSSYM_BASE              OWL_SENTINEL_OFFSET(100)
#define OWL_POSSYM_AVANT             (OWL_POSSYM_BASE + 0)
#define OWL_POSSYM_APRES             (OWL_POSSYM_BASE + 1)

/* fmnMoins / fmnParenthèses — motif d'affichage du négatif dans le type
 * FormatMonétaire (FormatNégatif). Défaut fmnMoins ("-1 234,50 €") ;
 * fmnParenthèses = style comptable ("(1 234,50 €)"). Sentinel-zone. */
#define OWL_FMN_BASE                 OWL_SENTINEL_OFFSET(101)
#define OWL_FMN_MOINS                (OWL_FMN_BASE + 0)
#define OWL_FMN_PARENTHESES          (OWL_FMN_BASE + 1)

/* httpFlux — mode de framing d'un flux de service HTTP declaratif (propriete
 * OperationHTTP..Flux, ). fluxSSE = framing SSE `data:`, fluxOctets =
 * octets bruts (chunked). Aucune semantique entiere propre : sentinel-zone pour
 * empecher une valeur tapee au pif de tomber sur un membre. Mutuellement
 * exclusifs (un seul mode par operation). */
#define OWL_HTTP_FLUX_BASE           OWL_SENTINEL_OFFSET(102)
#define OWL_HTTP_FLUX_SSE            (OWL_HTTP_FLUX_BASE + 0)
#define OWL_HTTP_FLUX_OCTETS         (OWL_HTTP_FLUX_BASE + 1)

#endif
