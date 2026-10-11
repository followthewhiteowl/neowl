// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_h
#define owl_stdlib_h

/* ------------------------------------------------------------------
 * INVARIANT owl_core — rappel central.
 *
 * Ce header est inclus largement dans la stdlib. Toute source qui le
 * tire et fait partie de OWL_CORE_SRCS (cf CMakeLists.txt) doit
 * respecter :
 *
 *   - aucune reference Qt (`#include <Q...>`, QApplication, QWidget,
 *     QPainter, ...)
 *   - aucun `#ifdef OWL_HAS_GUI` (le flag n'est jamais defini cote
 *     owl_core -> branche garantie morte, pieges  + ).
 *
 * Le code GUI-dependant va dans stdlib_ui.c / stdlib_pdf.c (compiles
 * UNIQUEMENT sous OWL_HAS_GUI) ou dans les sources per-target qui ont
 * le droit au ifdef car compilees deux fois (owl_stdlib.c,
 * stdlib_runtime.c, stdlib_builtin_types.c).
 *
 * Le garde-fou `owl_core_guard` (tools/owl_core_guard/) verifie ces
 * deux regles a chaque build. Voir aussi le commentaire INVARIANT au
 * niveau de OWL_CORE_SRCS dans CMakeLists.txt.
 * ------------------------------------------------------------------ */

#include "common.h"
#include "value.h"
#include "object.h"
#include "table.h"
#include "owl_methods.h"
#include "owl_registry.h"  /* OwlEnum definition (used by extern const E_* below) */

/* Public native registration function (used by all stdlib modules) */
void owl_define_native(const char* name, NativeFn function, int arity);

/* Register native with both bare name and canon-qualified name (__wl$Name) */
void owl_define_native_wl(const char* name, NativeFn function, int arity);

/* Set firstOutParamArg/lastOutParamArg on an existing ObjNative. Use after
 * registration to enable out-params (cf VISION §5bis).
 *   first=-1 → disabled
 *   last=-1  → variadic « jusqu'à la fin »
 *   last>=first → fenêtre fixe [first..last] inclus */
void owl_native_set_out_params(const char* name, int firstOutParamArg, int lastOutParamArg);

/* Initialize all stdlib modules. Called from vm_init(). */
void stdlib_init(void);

/* Individual module init (called by stdlib_init) */
void stdlib_string_init(void);
void stdlib_math_init(void);
void stdlib_array_init(void);
void stdlib_vecteur_init(void);  /*  : type Vecteur/Vector + conversions tableau */
void stdlib_matrice_init(void);  /*  : type Matrice/Matrix + conversions tableau/Vecteur */
void stdlib_tenseur_init(void);  /*  : type Tenseur/Tensor + moteur autograd */
void stdlib_iamodele_init(void); /*  : type IAModèle/AIModel + couches + Infère */
void stdlib_mlarbre_init(void); /*  : type MLArbreDécision/MLDecisionTree (CART) */
void stdlib_mllineaire_init(void); /*  : types MLRégressionLinéaire + MLRégressionLogistique */
void stdlib_mlcluster_init(void);  /*  : type MLKMeans (clustering non-supervisé) */
void stdlib_llm_init(void); /*  : type LLM (GGUF .Charge/.Génère) + OptionsGénération */
void stdlib_dataset_init(void); /*  : ImageVersTenseur + types ImageÉtiquetée/Dataset + chargeurs */
void stdlib_detection_init(void); /*  : IoU + SuppressionNonMax/NonMaxSuppression */
void stdlib_xls_init(void);      /*  : type xlsDocument + ecriture/lecture .xlsx */
void stdlib_io_init(void);
/* Active le flush par ligne de stdout (sortie non-TTY) — cf. stdlib_io.c. */
void owl_io_set_line_flush(bool enabled);
void stdlib_file_init(void);
void stdlib_json_init(void);
void stdlib_date_init(void);
/* Getters lives exposes pour FIELDS_Chrono.computeGetter —
 * `c..Valeur` et `c..Démarré` refletent l'etat live des champs internes
 * (_running, _start_ms, _accumulated_ms, _paused). Utilises aussi par
 * value_to_json et le debugger pour eviter tout snapshot stale
 *. */
Value fn_chrono_valeur(int argCount, Value* args);
Value fn_chrono_demarre(int argCount, Value* args);
void stdlib_crypto_init(void);
void stdlib_regex_init(void);
void stdlib_http_init(void);
void stdlib_http_service_init(void);   /*  : native FluxÉcrit (DSL service) */
void stdlib_ftp_init(void);
void stdlib_email_init(void);
void stdlib_email_msg_init(void);
void stdlib_mq_init(void);
void stdlib_mqtt_init(void);
void stdlib_amqp_init(void);
void stdlib_stomp_init(void);
void stdlib_nats_init(void);
void stdlib_aws_init(void);
/* Horloge du jar de cookies — point d'injection reserve aux self-tests C
 * internes (cf. stdlib_http.c). Decale, en secondes, le temps courant que
 * lit la logique d'expiration : c'est le seul moyen d'observer une
 * echeance atteinte PENDANT la vie du processus sans attendre l'horloge
 * murale. Aucune native ne l'expose au langage. Remis a zero par
 * httpParamReinitialise (etancheite entre self-tests voisins). */
void    owl_http_test_horloge_decale(int64_t secondes);
int64_t owl_http_test_horloge_decalage(void);
/*  — GET HTTP(S) synchrone borne (timeout dur, TLS verifie plein),
 * seam reutilise par le fetch JWKS du chemin d'auth (jwks_cache.c). Corps rendu
 * dans *out_body (malloc, NUL-termine, *out_len exclut le NUL), statut dans
 * *out_status. Retourne 0 si une reponse arrive, <0 sur echec transport.
 * L'appelant libere *out_body. */
int owl_http_get_borne(const char* url, int timeout_ms,
                       char** out_body, size_t* out_len, int* out_status);
void stdlib_bitwise_init(void);
void stdlib_similarity_init(void);  /* SimilaritéTexte — TexteDistance + famille distance/similarité */
void stdlib_reflect_init(void);
/* Exported enums from stdlib_reflect.c for use in FIELDS_Definition (stdlib_builtin_types.c) */
extern const OwlEnum E_owlTypes;
extern const OwlEnum E_familleDefinition;

/* Codec charset exposé depuis stdlib_string.c — frontière chaîne⇄octets du
 * modèle sandwich UTF-8. Réutilisé par les I/O texte de
 * stdlib_file.c (fChargeTexte/fSauveTexte/fAjouteTexte) pour décoder/encoder
 * le contenu disque selon un owlEncodage. `enc` = constante OWL_ENC_* ;
 * le buffer retourné est malloc'd (à free par l'appelant). */
extern const OwlEnum E_owlEncodage;
unsigned char* owl_codec_encode(const char* s, int len, int enc, int* outLen);
unsigned char* owl_codec_decode(const unsigned char* bytes, int len, int enc, int* outLen);

/* Exported enums from stdlib_string.c for FIELDS_FormatNumerique */
extern const OwlEnum E_fnAlignement;
extern const OwlEnum E_fnNotation;
extern const OwlEnum E_fnPrefixe;
extern const OwlEnum E_fnSigne;

/* Exported enums from ui_catalog.c for stdlib_pdf.c (Etats / PDF). */
extern const OwlEnum E_ITrait;
extern const OwlEnum E_IBrosse;
extern const OwlEnum E_IBord;
extern const OwlEnum E_IAlign;
extern const OwlEnum E_IEchelle;
extern const OwlEnum E_IInfoImpr;
extern const OwlEnum E_EtatFormat;
extern const OwlEnum E_EtatOrient;
extern const OwlEnum E_IDest;
extern const OwlEnum E_IStyle;
extern const OwlEnum E_IFamily;
/* Cadre — enums associes au type builtin (canon). */
extern const OwlEnum E_TraitCadre;
extern const OwlEnum E_BordsCadre;
/* typeDégradé — type de remplissage dégradé (aucun/lineaire/radial).
 * Defini ui_catalog.c, consomme par la feuille styleDégradé..Type
 * (stdlib_builtin_types.c). */
extern const OwlEnum E_TypeDegrade;
/* PositionImageBouton — enum position icone/texte d'un Champ Bouton (feuille
 * Style..Image..Position). Defini ui_catalog.c, consomme par la feuille de
 * style styleImageBouton (stdlib_builtin_types.c). */
extern const OwlEnum E_PositionImageBouton;
/* ActionSouris — enum simulation actions utilisateur (TestSimuleSouris).
 * Defini ui_catalog.c, consomme par stdlib_ui.c::p_test_simule_souris.allowedBase. */
extern const OwlEnum E_ActionSouris;
/* PoliceEtirement — enum etirement horizontal des caracteres. Defini
 * ui_catalog.c, consomme par la prop de champ PoliceEtirement (allowedBase). */
extern const OwlEnum E_PoliceEtirement;
/* pdfParametre — enums associes au type builtin (canon). */
extern const OwlEnum E_pdfProtection;

void stdlib_markdown_init(void);  /* MarkdownVers* via md4c (CommonMark + GFM) */
void stdlib_text_riche_init(void); /* TexteRicheVersHTML — Markdown OWL étendu (E.1) */

/* API interne du transpileur Texte Riche (utilisée par stdlib_ui.c apply
 * runtime quand FormatAffichage = texteRiche). Retourne une ObjString HTML. */
struct ObjString;
struct ObjString* owl_text_riche_to_html(const char* src, int srcLen);

void stdlib_console_init(void);
void stdlib_console_set_script_args(int argc, char** argv);
void stdlib_eval_init(void);
void stdlib_erreur_init(void);
/*  — Type Timer. */
void stdlib_timer_init(void);
/* Getter computé pour FIELDS_Timer.computeGetter — ..Actif reflète l'état
 * live _active du timer (lecture seule). */
Value fn_timer_actif(int argCount, Value* args);
/* Hook onFieldSet du type Timer : rejette l'écriture de ..Actif (lecture
 * seule) et l'affectation d'une procédure exigeant des paramètres. */
void owl_timer_on_field_set(ObjInstance* inst, const char* fieldName);
/* Warning de fin de process : timer encore actif jamais déclenché (script
 * CLI sans pompe). Appelé depuis vm.c après le top-level (hors owl test). */
void owl_timer_warn_active_on_exit(void);
/* Backend GUI : enregistre les fonctions start/stop/update du QTimer (override
 * depuis stdlib_ui.c en build GUI ; no-op par défaut en CLI). updateFn propage
 * un ..Intervalle / ..Répète modifié à chaud au QTimer vivant. */
void owl_timer_register_backend(void (*startFn)(ObjInstance*),
                                void (*stopFn)(ObjInstance*),
                                void (*updateFn)(ObjInstance*));
/* Backend GUI : un vrai QTimer parenté à une fenêtre vivante a été armé
 * (exclut le timer du warning de fin de process). */
void owl_timer_mark_gui_engaged(ObjInstance* inst);
void stdlib_runtime_init(void);
void stdlib_compression_init(void);
void stdlib_color_init(void);
void stdlib_mock_init(void);
void stdlib_xml_init(void);
void stdlib_image_init(void);
void stdlib_icones_init(void); /* Type Icone + enum Forme (catalogue SVG embarque) */
void stdlib_codebarres_init(void); /* Type CodeBarres + CodeBarreVersImage (zint) */
/*  Lot 2e-1 — resolution `icones.<nom>` runtime. Appele depuis vm.c
 * (OP_STRUCT_GET sur ObjIconesNamespace). Retourne un ObjIcone valide en cas
 * de hit, un ObjIcone vide (valide=false) + erreur native en cas de miss. */
Value owl_icones_namespace_lookup(const char* name);
/* did-you-mean Levenshtein-base sur le catalogue. NULL si rien proche. */
const char* owl_icones_namespace_did_you_mean(const char* name);
/*  Lot 2b+3 — namespace `theme.<nom>` : styles nommes du theme actif.
 * Registration du singleton + helpers de resolution appeles depuis vm.c. */
void stdlib_theme_init(void);
bool owl_theme_is_style_instance(Value v);
void owl_theme_namespace_emit_miss(const char* name);
/* Sentinelle navigable renvoyee sur un miss `theme.<nom>` : une instance
 * "null-object" dont tout acces de champ inconnu se replie sur elle-meme
 * (cf. dispatch OP_STRUCT_GET). Permet de chainer `theme.miss..X..Y` sans
 * second crash, la ou un NIL casserait des le premier sous-acces. */
Value owl_theme_make_miss_sentinel(const char* name);
bool owl_theme_is_miss_sentinel(Value v);
/*  — Materialise un ObjIcone depuis une entry catalogue deja
 * choisie. Bypass le lookup case-insensible (qui retombe sur priorite
 * mono-first). Utilise par CLI preview colored-first. L'entry est un
 * `const OwlIconCatalogEntry*` (cf. icons_catalog.h), opaque ici pour
 * eviter cycle d'include. Caller passe const void*. */
Value owl_icones_materialize_entry(const void* entry_ptr);
/* Coerce wrapper expose pour vm.c (reassignation polymorphique
 * ..Icone = <expr>). Equivalent au coerce enregistre via
 * owl_register_instance_coerce("Icone"). */
Value owl_icones_parse_value(Value v);
/*  Lot 2e-2 — rasterise un ObjIcone vers RGBA brut.
 * Le rasterizer concret (Qt6Svg) est enregistre au boot d'owl.exe via
 * owl_icone_register_rasterizer (cf. stdlib_ui.c qui passe le hook).
 * En mode CLI sans rasterizer enregistre : retourne false.
 *
 * out_rgba : alloue par le rasterizer (caller free).
 * Retour true si succes, false sinon (icone invalide, rasterizer
 * absent, ou rasterizing KO). */
bool owl_icone_rasterize_value(Value icn, int target_size,
                               unsigned char** out_rgba,
                               int* out_w, int* out_h);

/* Type du callback rasterizer enregistre par stdlib_ui.c (owl.exe).
 * stdlib_icones.c vit dans owl_core (commun a owl-cli/owl.exe) donc ne
 * peut pas dependre directement de Qt6Svg ; un hook NULLable est pose
 * ici et resolu au boot par le runtime GUI.
 *
 *  : ajout forme (0=aucune, 1=carre, 2=cercle) + bg_color_bgr
 * + want_bg pour appliquer le fond colore derriere l'icone + clipping
 * forme avant le render SVG. */
typedef bool (*OwlIconeRasterizeFn)(const char* svg_raw, int svg_len,
                                    unsigned int color_bgr, bool want_color,
                                    int target_size, int rotation_deg,
                                    int forme,
                                    unsigned int bg_color_bgr, bool want_bg,
                                    unsigned char** out_rgba,
                                    int* out_w, int* out_h);
void owl_icone_register_rasterizer(OwlIconeRasterizeFn fn);

/* Retour void* legacy (stdlib_image.h interop) : retourne NULL en V1,
 * conserve pour shape forward-compat. */
void* owl_icone_rasterize_to_image(Value icn);
void stdlib_sql_init(void);   /* SQL embarque (SQLite V1, PG/MySQL V2) */
void stdlib_uuid_init(void);  /* Type UUID + native DonneUUID + méthode .VersChaîne */
void stdlib_i18n_init(void);  /* Type langue + enum langueXxx + coerce chaîne→langue ( lot A) */
void stdlib_parametre_init(void);  /* Valeurs persistantes : ParamètreInitialise / ParamètreEnregistre / ParamètreCharge / ParamètreSupprime */
void stdlib_ini_init(void);        /* Fichiers .ini section-conscients : INILit / INIEcrit */

/*  : les `_mark_roots()` ad-hoc  sont remplaces par un
 * registre unique dans gc.c (cf. owl_register_persistent_obj_root). Chaque
 * module enregistre ses globals dans son `_init()`. Seuls les `_reset_globals()`
 * subsistent (utiles pour vm_init reset NULL). */
void stdlib_erreur_reset_globals(void);
void stdlib_image_reset_globals(void);
void stdlib_string_reset_globals(void);
#ifdef OWL_HAS_GUI
void stdlib_ui_reset_globals(void);
#endif

/* Accesseurs i18n runtime ( lot C).
 *
 * V1 : valeurs hardcodees "fr-FR" pour les deux globales. Au futur ticket 2
 * du chantier multilangue, ces accesseurs deviendront mutables via les
 * natives `Langue()` / `LangueSource()` user-facing. La signature reste
 * stable : tout consommateur (vm.c, object.c::multilingue_resolve,
 * stdlib_string.c::fn_taille...) appelle ces helpers, pas les const inline. */
const char* owl_i18n_langue_source(void);    /* langue source projet (defaut "fr-FR") */
const char* owl_i18n_langue_courante(void);  /* langue active runtime */

/* boot i18n depuis le champ Langues du Projet (, ).
 *   owl_i18n_set_project_source : pose la langue source (+ courante initiale)
 *     depuis un tag BCP 47. Appele au boot projet par project_run, depuis la
 *     langue capturee au compile-time de projet.owl.
 *   owl_i18n_lookup_tag_by_constant_name : resout un nom de constante langueXxx
 *     (canon accentue ou ASCII) vers son tag BCP 47 ; NULL si le nom n'est pas
 *     une langue officielle. Consomme par le compilo (validation champ Langues). */
void        owl_i18n_set_project_source(const char* tag);
/*   owl_i18n_set_project_languages : pose la liste COMPLETE des langues
 *     declarees (tags BCP 47) au boot projet, en symetrie de set_project_source
 *     (qui ne fige que la 1ere). Alimente ListeLangues(). tags==NULL/count<=0
 *     vide la liste (retour au fallback ["fr-FR"] du mode script). */
void        owl_i18n_set_project_languages(const char* const* tags, int count);
const char* owl_i18n_lookup_tag_by_constant_name(const char* constName);

/* owl_bcp47_canon vit dans owl_name_normalize.h (source UNIQUE dep-free
 * partagee avec le formatter, backend owl-lsp compris). */

/*  — hot-switch de la langue sur l'UI vivante ( §4).
 * owl_core reste sans Qt : le setter Langue() appelle owl_i18n_on_language_changed()
 * apres avoir change la langue courante. La couche GUI (stdlib_ui, OWL_HAS_GUI)
 * enregistre au boot un callback via owl_i18n_register_on_language_changed qui
 * parcourt les fenetres/controles vivants, re-resout leurs proprietes typees
 * chaineMultilingue sur la nouvelle langue et re-pousse la chaine resolue vers
 * le backend graphique. default no-op (CLI / headless : callback NULL). */
typedef void (*OwlI18nLanguageChangedFn)(void);
void owl_i18n_register_on_language_changed(OwlI18nLanguageChangedFn fn);
void owl_i18n_on_language_changed(void);

/* Catalogue des constantes langueXxx ( lot A) exposé en lecture
 * pour les consommateurs externes (--vscode-data → éditeur multilingue
 * designer). Source unique : table statique de stdlib_i18n.c.
 *   name      : canon accentué ("langueFrançais")
 *   nameAscii : alias ASCII sans accent, NULL si identique au canon
 *   bcp47     : tag RFC 5646 ("fr-FR")
 *   nameEN    : alias EN canon ("languageFrench"), NULL si absent
 *   descFR/EN : descriptions registry */
typedef struct {
    const char* name;
    const char* nameAscii;
    const char* bcp47;
    const char* nameEN;
    const char* descFR;
    const char* descEN;
} OwlLangueInfo;

int                  owl_i18n_langue_count(void);
const OwlLangueInfo* owl_i18n_langue_at(int index);  /* NULL si hors borne */

/* Catalogue des couleurs prédéfinies (membres de l'enum couleur) exposé en
 * lecture pour --vscode-data → le picker du designer consomme la table
 * nom→hex sans la recopier à la main. Source unique : table statique de
 * stdlib_color.c (les mêmes entrées qui peuplent constantes + enum couleur).
 *   nameFR   : nom canon FR accentué ("Rouge", "RougeFoncé")
 *   nameEN   : nom canon EN ("Red"), NULL si absent
 *   value    : entier stocké — couleur packée BGR (R + V*256 + B*65536) si
 *              >= 0, ou valeur négative réservée = sentinelle sans RGB fixe
 *   sentinel : true quand la couleur n'a pas de #RRGGBB fixe (Transparent,
 *              CouleurDéfaut, iSystème) — le front décide de l'affichage */
typedef struct {
    const char* nameFR;
    const char* nameEN;
    int64_t     value;
    bool        sentinel;
} OwlColorInfo;

int                 owl_color_catalog_count(void);
const OwlColorInfo* owl_color_catalog_at(int index);  /* NULL si hors borne */

/* Conversion couleur packée BGR (R + V*256 + B*65536) → "#RRGGBB".
 * Écrit dans out (>= 8 octets) et retourne true si la valeur porte un RGB
 * fixe. Retourne false sans toucher out si value < 0 (sentinelle : pas de
 * #RRGGBB). Même décomposition que RVBVersHTML (canon HTML #RRGGBB). */
bool owl_color_bgr_to_html(int64_t value, char* out, size_t outLen);

/* CSPRNG OS-backed (BCryptGenRandom Windows, /dev/urandom Linux/macOS).
 * Source unique de randomness cryptographique pour la stdlib : AES IV/salt
 * (stdlib_crypto.c), UUID v4 (stdlib_uuid.c). Retourne 0 sur succès. */
int owl_random_bytes(unsigned char* buf, size_t len);

/* UUID — hooks consommés par vm.c::value_to_string pour la coercion
 * implicite UUID → chaîne (format canonique RFC : 36 chars hex à tirets).
 * Implémentés dans stdlib_uuid.c pour ne pas répandre la connaissance du
 * field privé `_bytes` hors du module métier. owl_uuid_format_canonical
 * retourne NULL si v n'est pas une instance UUID valide. */
bool      owl_value_is_uuid(Value v);
ObjString* owl_uuid_format_canonical(Value v);

/* Templates des types reifies SQL — exposes pour register_builtin_types(). */
ObjInstance* build_sqlConnexion_template(void);
ObjInstance* build_sqlRequete_template(void);
ObjInstance* build_sqlReponse_template(void);

/* Template du type reifie FTP — expose pour register_builtin_types(). */
ObjInstance* build_ftpClient_template(void);

/* Image runtime pixel access — utilisé par la VM pour OP_ARRAY_GET_2D /
 * OP_ARRAY_SET_2D (syntaxe `MaImage[x,y]` doc Multimedia/Images/
 * 1000019650 §"Accès aux pixels").
 *
 * stdlib_image_is_instance(v) : true si v est un ObjInstance de type Image.
 * stdlib_image_get_pixel(v, x, y) : retourne entier canon Couleur (BGR 24-bit)
 *   ou 0 si v n'est pas Image valide ou (x,y) hors limites.
 * stdlib_image_set_pixel(v, x, y, color) : écrit le pixel ; no-op
 *   silencieux si v n'est pas Image valide ou hors limites. La couleur
 *   est masquée à 24 bits BGR ; alpha conservé à 255 (opaque). */
bool     stdlib_image_is_instance(Value v);
int64_t  stdlib_image_get_pixel(Value v, int x, int y);
void     stdlib_image_set_pixel(Value v, int x, int y, int64_t couleurBGR);

/*  — accès cellule xlsDocument depuis la VM (subscript 2D
 * doc[ligne, colonne] + propriétés ..NombreXxx). */
bool     stdlib_xls_is_document(Value v);
bool     stdlib_xls_resolve_col(Value v, int* outCol);   /* entier 1-based ou lettres "A" */
bool     stdlib_xls_set_cell(Value doc, int row, int col, Value v);
Value    stdlib_xls_get_cell(Value doc, int row, int col);
bool     stdlib_xls_get_property(Value doc, const char* prop, Value* out);
/*  — références de style (cellule / ligne / colonne). */
bool     stdlib_xls_is_ref(Value v);
bool     stdlib_xls_document_subref(Value doc, const char* prop, Value* out);
Value    stdlib_xls_ref_bind_cell(Value ref, int row, int col);
Value    stdlib_xls_ref_bind_index(Value ref, int idx);
bool     stdlib_xls_ref_get_property(Value ref, const char* prop, Value* out);
/* Résultat d'une écriture de propriété de style : distingue « propriété
 * inconnue » (le caller émet la liste des propriétés valides) de « propriété
 * connue mais type de valeur invalide » (le caller émet une erreur dédiée avec
 * le type attendu, rempli dans outExpected). */
typedef enum {
    XLS_SET_OK,
    XLS_SET_UNKNOWN_PROP,
    XLS_SET_BAD_TYPE
} XlsSetResult;
XlsSetResult stdlib_xls_ref_set_property(Value ref, const char* prop, Value val,
                                         const char** outExpected);

/* Buffer access exposé pour le backend GUI (ui_qt.cpp) : convertit
 * Image OWL → QImage Format_RGBA8888 sans copie pixel-à-pixel. Retourne
 * NULL si v n'est pas une Image valide. *out_w / *out_h écrits seulement
 * si le retour est non-NULL. */
const uint8_t* stdlib_image_get_rgba(Value v, int* out_w, int* out_h);

/* Retourne le handle OwlImage* opaque depuis une Value Image, ou NULL si
 * v n'est pas une Image valide. Permet aux consumers de stdlib_image
 * (stdlib_pdf.c::render_etat_image, ...) d'appeler les API owl_image_*
 * directement (save_png, etc.) sans réimplementer la logique. */
struct OwlImage;
struct OwlImage* stdlib_image_get_handle(Value v);

/*  — Cree une Value Image OWL portant un OwlImage* alloue avec
 * une copie du buffer rgba (RGBA8 top-down). Utilise par
 * fn_IconeVersImage pour conserver les pixels rasterises au lieu de
 * les jeter. Le buffer source n'est pas pris en ownership, le caller
 * peut free apres l'appel. Retourne une Image invalide si rgba=NULL
 * ou dimensions <= 0. */
Value stdlib_image_value_from_rgba(const uint8_t* rgba, int w, int h);
/* Soft-error signalling API. Natives signal errors via
 * owl_emit_native_error() (cf. owl_errors.h) qui forward au transport
 * owl_set_error_native(). Le legacy transport `owl_set_error(int, const char*)`
 * a ete retire en  (100% migration registry erreurs).
 * Les natives lisent l'etat global via les accesseurs owl_err_get_*. */
/* Exige un argument present ET du bon type, en distinguant les deux
 * diagnostics. Le motif `if (argCount < N || !IS_X(args[i]))` suivi d'un
 * E_NATIVE_ARG_MISSING annoncait un argument manquant alors qu'il etait
 * present et seulement du mauvais type — l'utilisateur cherchait alors un
 * oubli inexistant.
 *
 *   OWL_REQUIRE_ARG("zipOuvre", "chemin", 0, IS_STRING(args[0]),
 *                   "chaîne", NIL_VAL);
 *
 * `typeTest` n'est evalue qu'apres controle d'arite : pas de lecture de
 * args[idx] hors bornes. */
#define OWL_REQUIRE_ARG(nativeName, argName, idx, typeTest, expectedDesc, retval) \
    do {                                                                          \
        if (argCount <= (idx)) {                                                  \
            owl_emit_native_error(E_NATIVE_ARG_MISSING, (nativeName),              \
                                  "arg", (argName), NULL);                        \
            return (retval);                                                      \
        }                                                                         \
        if (!(typeTest)) {                                                        \
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, (nativeName),        \
                                  "arg", (argName),                               \
                                  "expected", (expectedDesc),                     \
                                  "actual", "autre type", NULL);                  \
            return (retval);                                                      \
        }                                                                         \
    } while (0)

/* Variante presence seule, quand le type de l'argument n'est pas contraint
 * (ou l'est plus loin par un chemin dedie). */
#define OWL_REQUIRE_ARG_PRESENT(nativeName, argName, idx, retval)   \
    do {                                                            \
        if (argCount <= (idx)) {                                    \
            owl_emit_native_error(E_NATIVE_ARG_MISSING, (nativeName),\
                                  "arg", (argName), NULL);          \
            return (retval);                                        \
        }                                                           \
    } while (0)

void owl_clear_error(void);       /* flag only — utilisé par le dispatch auto */
void owl_clear_error_full(void);  /* flag + info — réservé à ErreurRAZ() user */
bool owl_err_is_detected(void);   /* état du flag soft-error (g_err_detected) */
void register_builtin_types(void);

/* Global registration helpers (shared across stdlib modules that expose
 * their constants as string globals directly in vm.globals). */
void owl_register_global_string(const char* name, const char* val, int len);

/* Date arithmetic and normalization helpers (used by vm.c) */
Value owl_date_add_days(Value dateVal, int64_t days);
Value owl_date_subtract(Value a, Value b);

/* Parse une forme courte "<chiffres><unité>" (ms/cs/s/mn) vers ms. Retourne
 * -1 si la chaîne n'est pas reconnue. Utilisé par Temporise() pour accepter
 * une chaîne lisible ("200ms") en lieu et place d'un entier en centièmes. */
int64_t owl_duree_parse_short_form(const char* s, int slen);
Value owl_complete_rep(Value v);  /* doc 3024018, utilisé par OP_DIVIDE pour string / string */
bool owl_value_to_absolute_ms(Value v, int64_t* outMs);
void owl_normalize_date(ObjDate* d);
void owl_normalize_time(ObjTime* t);
void owl_normalize_datetime(ObjDateTime* dt);

/* /1830 — briques de detection partagees (stdlib_detection.c), reprises
 * par SuppressionNonMax ET par IAModèle.Détecte (NMS par classe). Une boite est
 * un quadruplet [x, y, largeur, hauteur] (coin haut-gauche puis taille). */
double owl_det_iou4(const double a[4], const double b[4]);
/* Suppression non-maximale gloutonne sur un sous-ensemble de candidats.
 *   boxbuf : boites a plat (4 reels par boite, indexees globalement) ;
 *   scbuf  : scores (memes index globaux) ;
 *   cand   : ncand index globaux a considerer ;
 * Remplit kept[0..N) avec les index conserves (sous-ensemble de cand), tries
 * par score decroissant (tie-break index croissant), et rend N. kept doit
 * pouvoir accueillir ncand entrees. */
int owl_det_nms_greedy(const double* boxbuf, const double* scbuf,
                       const int* cand, int ncand, double seuil, int* kept);

#endif
