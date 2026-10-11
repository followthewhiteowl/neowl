// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_compiler_h
#define owl_compiler_h

#include "object.h"
#include "scanner.h"
#include "prescan.h"
#include "ui_decl.h"  /* OwlControlKindInfo pour compiler_prescan_widget_find */

/* Compile source code into a function object containing bytecode.
 * Returns NULL on compilation error. Top-level RENVOYER is forbidden. */
ObjFunction* compile(const char* source);

/* Same as compile() but treats the source as a function body, so RENVOYER
 * at top level is accepted. Used by the runtime dynamic-compilation
 * natives (Compile, ExécuteCode, EvalueExpression). */
ObjFunction* compile_as_function(const char* source);

/* Compile source code with project context (multi-file support).
 * filename: stem name of the file (for error messages).
 * symbols: project-wide symbol table from prescan.
 * fileIndex: index of this file in the project. */
ObjFunction* compile_in_project(const char* source, const char* filename,
                                 SymbolTable* symbols, int fileIndex);

/*  — precalcule l'etat voisins (widgets/sous-widgets, globales bare,
 * membres de collection) UNE fois pour tout le projet, puis chaque
 * compile_in_project rejoue les contributions des autres fichiers au lieu de
 * re-tokeniser tous les voisins (O(N^2) -> O(N)). A appeler une fois par check,
 * AVANT les passes de compilation ; a liberer par compiler_neighbor_prebuild_
 * reset ensuite. Sans prebuild, compile_in_project retombe sur le scan live. */
void compiler_neighbor_prebuild(SymbolTable* symbols);
void compiler_neighbor_prebuild_reset(void);

/*  — variante deux phases de compile_in_project. Compile le
 * fichier exactement comme compile_in_project (meme setup, meme parse, memes
 * checks cross-fichier) mais produit DEUX ObjFunction distincts au lieu d'un
 * chunk fusionne :
 *   - *outDecls : les seules declarations statiques top-level (classe,
 *     procedure/fonction, enum, structure, combinaison, interface, bloc
 *     CONSTANTE, fenetre/etat) ;
 *   - *outBody  : le seul code imperatif top-level (appels, Ouvre, Trace,
 *     assignations, init de globales).
 * Le pool de constantes est copie ENTIER dans chaque (les index OP_CONSTANT
 * restent valides). compile_in_project reste inchangee (protege --check /
 * owl test). Retourne true en succes, false + (*outDecls=*outBody=NULL) si
 * erreur de compilation. Les deux ObjFunction sont des Obj frais NON rootes au
 * retour : le caller doit les rooter avant toute nouvelle allocation. */
bool compile_in_project_split(const char* source, const char* filename,
                              SymbolTable* symbols, int fileIndex,
                              ObjFunction** outDecls, ObjFunction** outBody);

/*  — boot i18n compile-time depuis projet.owl. Reset avant de
 * compiler projet.owl, puis lit la 1ere langue declaree dans le champ
 * Langues (metadonnee constante, ). Retourne NULL si aucune. */
void compiler_projet_boot_langue_reset(void);
const char* compiler_projet_boot_langue(void);

/*  — liste complete des tags BCP 47 declares dans le champ Langues,
 * dans l'ordre de declaration ([0] = langue source). Meme fenetre de validite
 * que compiler_projet_boot_langue. Lu par project_run pour plomber le runtime
 * i18n (ListeLangues()). _tag(i) retourne NULL hors plage. */
int         compiler_projet_boot_langue_count(void);
const char* compiler_projet_boot_langue_tag(int index);

/*  — le DERNIER fichier compile declarait-il un bloc
 * `est une CollectionDeProcédures` ? Lu par project_run juste apres le
 * compile d'un compagnon pour gater la capture de son hook Terminaison :
 * un fichier a plat qui declare PROCEDURE Terminaison() ne participe pas au
 * cycle de vie du projet. */
bool compiler_file_declares_collection(void);

/*  — nom du fichier de styles designe par Projet..Theme (capture
 * compile-time du bloc Projet). NULL si aucun projet / pas de champ Theme. */
const char* compiler_projet_designated_theme(void);

/*  — nom de l'instance Fenetre designee par Projet..FenetreDemarrage
 * (bilingue StartupWindow, capture compile-time du bloc Projet). C'est la
 * fenetre ouverte au LANCEMENT PROJET (owl <dir> / owl projet.owl). NULL si
 * aucun projet / champ absent. */
const char* compiler_projet_designated_startup_window(void);

/*  — format de date par defaut du projet (champ FormatDate/DateFormat
 * du bloc `est un Projet`), repli des champs Date sans MasqueAffichage. NULL si
 * aucun projet / champ absent / valeur non-chaine. */
const char* compiler_projet_format_date(void);

/*  — format d'heure par defaut du projet (champ FormatHeure/TimeFormat
 * du bloc `est un Projet`), repli des champs Heure sans MasqueAffichage. NULL si
 * aucun projet / champ absent / valeur non-chaine. */
const char* compiler_projet_format_heure(void);

/*  — format de duree par defaut du projet (champ FormatDurée/
 * DurationFormat du bloc `est un Projet`), repli des champs Durée sans
 * MasqueAffichage. NULL si aucun projet / champ absent / valeur non-chaine. */
const char* compiler_projet_format_duree(void);

/*  — config du champ FormatNombre du bloc `est un Projet`, capture
 * compile-time (meme cycle de vie que FormatDate). Sert de repli aux champs
 * numeriques qui ne portent pas leur propre FormatNombre. `*chiffres` recoit
 * ChiffresAprèsVirgule (OWL_FNB_AUTO si non configure) ; les separateurs sont
 * copies dans les buffers fournis ("" = defaut). Retourne true si le projet a
 * declare un champ FormatNombre, false sinon (le runtime substitue le defaut
 * plat). Tout out-param peut etre NULL. */
bool compiler_projet_format_nombre(int* chiffres,
                                   char* sepDec, int sepDecCap,
                                   char* sepMil, int sepMilCap);

/*  — config du champ FormatMonétaire du bloc `est un Projet`, capture
 * compile-time (miroir de compiler_projet_format_nombre). Repli des champs
 * monetaires sans FormatMonétaire propre. Retourne true si le projet a declare
 * FormatMonétaire, false sinon (le runtime substitue le defaut plat). Tout
 * out-param peut etre NULL. */
bool compiler_projet_format_monetaire(char* symbole, int symboleCap,
                                      int* position,
                                      char* sepDec, int sepDecCap,
                                      char* sepMil, int sepMilCap,
                                      int* chiffres,
                                      int* negatif);

/*  — identifiant du bloc `<Ident> est un Projet` capture au compile
 * (meme cycle de vie que le boot langue). C'est le nom du projet cote runtime :
 * les valeurs persistantes s'en servent pour resoudre leur emplacement par
 * defaut. NULL en mode script (aucun projet.owl compile). */
const char* compiler_projet_ident(void);

/*  — fenetres DSL declarees du projet (kind SYM_WINDOW), captures au
 * compile depuis le SymbolTable cross-fichier. Support de la reflexion runtime
 * ElementsDe(enumFenetre). _collect remplit depuis la table (idempotent, meme
 * table pour tous les fichiers d'un projet) ; _reset vide (appele par vm_init
 * pour eviter tout residu entre fichiers d'un run recursif). Liste vide en mode
 * script (aucune fenetre dans la table). */
void        compiler_projet_windows_collect(const SymbolTable* symbols);
void        compiler_projet_windows_reset(void);
int         compiler_projet_window_count(void);
const char* compiler_projet_window_name(int i);

/*  — materialise le global de metadonnees Projet (identifiant du bloc
 * `<Ident> est un Projet`) depuis la capture compile-time, TOT dans
 * project_run (avant les compagnons), pour que la cascade cross-fichier
 * MonApp..Champ voie les metadonnees alors que le corps de projet.owl
 * s'execute en dernier. No-op si aucun bloc Projet capture. */
void compiler_projet_meta_materialize(void);

/*  — materialise les variables GLOBAL declarees au corps du bloc Projet
 * avec la valeur zero de leur type, TOT dans project_run (Phase 1, avant les
 * compagnons), pour qu'un compagnon lance avant le corps de projet.owl (Phase
 * 2.c) resolve la globale au runtime. L'init explicite reste jouee au corps.
 * No-op si aucune globale de projet capturee. */
void compiler_projet_globals_materialize(void);

/*  : hydrate les tables persistantes POO compile-time
 * (g_class_constructor_arity, g_class_super) a partir des donnees prescan
 * AVANT le parse principal. Necessaire en mode --check : on ne compile que
 * le fichier cible, donc les classes des autres fichiers du projet n'ont
 * pas encore peuple g_class_constructor_arity[]. Sans cette hydration,
 * `oFoo est un cFoo` cross-file rate l'erreur E_SEM_INSTANCE_NEEDS_ARGS
 * quand cFoo a un cstr a args requis (faux negatif).
 *
 * Idempotent : ecrase les entrees existantes (re-hydration safe). */
void compiler_hydrate_constructor_arity_from_symbols(SymbolTable* t);

/*  : hydrate g_class_field_vis[] (visibilite des champs de classe) a
 * partir des membres prescan (ProjectMember.visibility >= 0). Remplace, dans la
 * passe de contexte de `owl --check <file> --project`, le full-compile de chaque
 * voisin qui ne servait qu'a peupler g_class_field_vis de facon
 * order-independante. N'hydrate QUE la visibilite (pas le typeToken : il n'est
 * lu que pour les champs de la classe en cours de compile, jamais cross-fichier).
 *
 * Idempotent : ecrase les entrees existantes (re-hydration safe). */
void compiler_hydrate_field_vis_from_symbols(SymbolTable* t);

/*  : hydrate g_method_visibility[] (visibilite des methodes de classe)
 * depuis classMethodVis[] du prescan. Necessaire pour que `owl --check <file>
 * --project` resolve un colon-ref :M() vers une methode heritee d'une base
 * declaree dans un autre fichier (sinon faux "membre inconnu"), sans full-
 * compiler les voisins. Idempotent (re-hydration safe). */
void compiler_hydrate_method_vis_from_symbols(SymbolTable* t);

/*  — sources du perimetre reservees au SEUL scan d'usage cross-fichier
 * ("variable declaree mais jamais utilisee").
 *
 * `owl --check` ecarte les fichiers de test de la decouverte : leur source
 * n'entre donc pas dans SymbolTable.sources[], que ce scan parcourt. Une
 * variable dont le seul utilisateur est un test etait annoncee inutilisee,
 * alors que `owl test` sur le meme dossier ne disait rien. On les enregistre
 * ici au lieu de les indexer : un usage ne peut qu'ETEINDRE un avertissement,
 * jamais faire resoudre un symbole — le perimetre de resolution du controle
 * reste celui de l'execution.
 *
 * Pointeurs non-owning : a vider avant la liberation du Project source. */
void compiler_usage_scan_sources_clear(void);
void compiler_usage_scan_source_add(const char* source, int fileIndex);

/* GC: mark compiler roots (open functions being compiled) */
void gc_mark_compiler_roots(void);

/* Reset des tables persistantes POO compile-time (g_class_super,
 * g_class_field_vis, g_class_abstract, g_class_abstract_methods,
 * g_class_interfaces, g_interface_methods, g_class_constants,
 * g_declared_method_sigs). Ces tables sont enregistrees a la compilation
 * des classes/interfaces et restent volontairement persistantes au sein
 * d'un meme programme (multi-fichier ). Mais pour `owl test`, qui
 * compile/execute chaque test_*.owl dans une VM "fraiche", il faut aussi
 * remettre a zero ces compteurs entre fichiers — sinon la pollution croisee
 * fait croire au compilo qu'une classe est abstraite, herite, redeclare un
 * champ herite, etc., d'apres le test precedent (cf. ).
 *
 * Appelle uniquement par le test runner. Le compile/run normal en
 * production ne doit PAS appeler ce reset : un projet multi-fichier
 * a besoin de la persistance pour que B.owl puisse referencer A.owl. */
void compiler_reset_poo_state(void);

/* ================================================================
 * PRESCAN WIDGETS — forward-ref des widgets DSL au sein d'une window/etat
 * ================================================================ */

/* Cherche un widget DSL pre-enregistre par le prescan source. Permet de
 * resoudre `LIB_R` en widget meme si sa declaration `LIB_R est un Libelle`
 * vient APRES le site d'usage (typique : PROCEDURE surClic() qui modifie
 * un Libelle declare plus bas dans la fenetre).
 *
 * Retourne le OwlControlKindInfo* du kind detecte, NULL si absent.
 * Consomme par vm_find_widget_info_by_name en amont de vm.windows. */
const OwlControlKindInfo* compiler_prescan_widget_find(const char* name);

/* ================================================================
 * DIAGNOSTIC CAPTURE (for --check mode and LSP)
 * ================================================================ */

typedef struct {
    int line;
    int column;
    /* Taille genereuse : certains messages enumerent toutes les proprietes
     * d'un template DSL (httpRequete = 17 props, ~340 octets UTF-8) et
     * doivent rester complets pour ne pas tronquer un nom au milieu. */
    char message[1024];
    /*  : fichier source du diagnostic quand il provient d'un fichier
     * different du fichier-cible (ex: champ classe cross-file qui shadow une
     * globale main.owl). Chaine vide = same-file, omis du JSON output pour
     * preserver le format historique. Taille = basename + marge. */
    char file[256];
    /*  : basename du fichier EN COURS DE COMPILATION au moment de
     * l'emission, toujours renseigne (chaine vide seulement si le compilateur
     * n'a pas de nom de fichier, ex. source anonyme du REPL). Distinct de
     * `file` ci-dessus, qui est une annotation posee APRES coup par un
     * consommateur pour signaler un diagnostic cross-fichier.
     *
     * Un consommateur qui affiche les diagnostics en differe (bilan de
     * `owl test`) n'a plus de contexte de compilation : sans ce champ il
     * attribuait tous les avertissements au dernier fichier compile, y compris
     * ceux nes dans un compagnon. Le chemin des ERREURS non capturees, lui,
     * ecrit sur stderr au moment de l'emission et etait donc deja juste. */
    char origin[128];
} CompileDiagnostic;

/* Redirect compiler errors to a buffer instead of stderr.
 * Pass NULL to restore normal stderr output. */
void compile_set_error_capture(CompileDiagnostic* buffer, int capacity, int* count);

/* Redirect compiler warnings to a buffer (parallel to error capture).
 * Pass NULL to restore normal stderr output. */
void compile_set_warning_capture(CompileDiagnostic* buffer, int capacity, int* count);

/* REPL auto-print: when enabled, a bare expression statement emits OP_PRINT
 * (which pops and displays the value) instead of the usual OP_POP. Used by
 * `owl --repl` for interactive / notebook-kernel sessions where evaluating
 * `2 + 2` should produce `4` on stdout without an explicit Trace() call.
 * Assignments, calls returning nothing, and control statements are NOT
 * printed — only the value that would have been silently discarded. */
void compile_set_auto_print(bool enabled);

/* Emit compile-time errors as JSON lines on stderr (one line per error,
 * `{"kind":"compile","message":"...","line":N,"column":N,"file":"..."}`)
 * instead of the human-readable `[file:line] Error at 'x': ...` format.
 * Used by notebook kernels / CI linters along with vm_set_json_errors
 * so BOTH compile and runtime failures look the same to the consumer. */
void compile_set_json_errors(bool enabled);

/* ================================================================
 * REGISTRY ERREURS — bridge owl_errors.c -> compiler internals
 * ================================================================
 * Exposes minimaux pour que les helpers compile-aware (owl_emit_error_at,
 * owl_emit_warning_at, owl_emit_error_current dans owl_errors.c) puissent
 * reutiliser la machinerie d'emission legacy (diagBuffer/warnBuffer, mode
 * JSON, currentFilename) sans la dupliquer. Lot 2 . */

/* Emet un diagnostic deja rendu sur stderr (ou capture buffer), exactement
 * comme le ferait error_at()/warning_at() legacy. Si isWarning == false,
 * marque parser.hadError = true et panicMode comme avant. Si isWarning
 * == true, route vers warnBuffer / "Warning" sans toucher hadError.
 * `message` est la chaine deja rendue (template + substitution), pas le
 * code. La fonction gere le formatage [file:line] / JSON / at end / at 'x'
 * en fonction du Token. */
void compile_emit_diagnostic_at(const Token* token, const char* message,
                                bool isWarning);

/* Renvoie le Token "current" du parser. Utilise par owl_emit_error_current
 * quand l'appelant n'a pas de Token sous la main. Pointeur stable jusqu'au
 * prochain advance_token. */
const Token* compile_current_token(void);

/* Introspection parser.hadError pour les self-tests internes. Renvoie 0/1. */
int compile_peek_had_error(void);

/* Reset parser.hadError + panicMode a false. Pour les self-tests internes
 * qui veulent enchainer plusieurs emit sans rester en panicMode. */
void compile_reset_parser_error_state(void);

/* ================================================================
 * USER TYPE FIELD METADATA (pour stdlib_json::Sérialise/Désérialise)
 * ================================================================
 * Les attributs <Sérialise=Faux> / <Sérialise="autre_nom"> sur les
 * champs de structure sont enregistrés au compile-time. Le runtime
 * de Sérialise/Désérialise les consulte via ces deux helpers. */

/* Renvoie true si le champ est trouvé. Pose les out-params :
 *   *outSkip   = true si <Sérialise=Faux>
 *   *outRename = nom alternatif (ou NULL si pas de rename) */
bool compiler_user_type_field_serialise(const char* typeName, int typeLen,
                                        const char* fieldName, int fieldLen,
                                        bool* outSkip, const char** outRename);

/* Recherche inverse : trouve le champ qui a <Sérialise="jsonKey">.
 * Renvoie le nom canonique du champ (interne au .name de UserTypeField) ou
 * NULL si aucun champ n'a ce rename. La comparaison est case+accent
 * insensible (mapping souple, conforme à l'esprit OWL/canon). */
const char* compiler_user_type_field_by_rename(const char* typeName, int typeLen,
                                               const char* jsonKey, int jsonKeyLen,
                                               int* outFieldLen);

/* ================================================================
 * BUILTIN PROPERTY CANONICAL LOOKUP (formatter / LSP / self-test)
 * ================================================================
 * Source unique de vérité des propriétés des types primitifs builtin
 * (tableau, file/pile/liste, JSON, date, heure, dateheure, durée). Le
 * formatter et le LSP consomment cette source au lieu de tables dupliquées.
 *
 * Convention : si `descFR == NULL` l'entrée est un alias ASCII de tolerance
 * (ex: Annee pour Année) — invisible aux iterators publics. */
typedef struct {
    const char* nameFR;      /* canon FR (accentué ou non) */
    const char* nameEN;      /* alias EN, NULL si identique au FR */
    const char* descFR;      /* description user-facing FR ; NULL = alias non exposé */
    const char* descEN;      /* description user-facing EN */
    const char* valueType;   /* "entier" | "réel" | "chaine" | "Date" | "Heure" | "booléen" */
} BuiltinKindProp;

/* Iteration index-based des proprietes builtin canoniques (date/heure/tableau/
 * JSON/duree). Saute les alias de tolerance (descFR==NULL). Consomme par le
 * backend registry du formatter, qui matche cote formatter via owl_name_matches
 * (meme ensemble que OWL_BUILTIN_PROP_CATALOG -> parite registre/catalogue). */
int  compiler_builtin_prop_count(void);
bool compiler_builtin_prop_at(int idx, const char** outFR, const char** outEN);

/* Iterator pour consommateurs externes (LSP autocomplete, doc gen).
 * Callback invoqué une fois par propriété UNIQUE (dedup par nameFR), en
 * sautant les alias (descFR == NULL). Ordre de traversée : PROPS_ARRAY,
 * COLLECTION, JSON, DATE, TIME, DATETIME, DURATION. */
void compiler_iter_builtin_props(void (*cb)(const BuiltinKindProp* prop, void* ctx),
                                  void* ctx);

/* Variante "full" : itère TOUTES les entrées avec le kind d'appartenance
 * — pas de dédup. Le callback reçoit (kindName, prop). Consommé par le
 * CLI doc (owl explain) pour collecter les owners d'une propriété. */
void compiler_iter_builtin_props_full(
    void (*cb)(const char* kindName, const BuiltinKindProp* prop, void* ctx),
    void* ctx);

/* Variante "kinds" : itère TOUTES les entrées, ALIAS COMPRIS (descFR ==
 * NULL), avec le kind d'appartenance. Sert à reconstituer, pour un nom de
 * propriété donné, la liste exacte des kinds qui l'acceptent — un alias sans
 * description reste un nom que le compilateur accepte (`d..Jour` sur une
 * durée). Consommé par le dump du catalogue LSP, qui a besoin de l'ensemble
 * des kinds accepteurs et pas seulement de ceux qui documentent le nom. */
void compiler_iter_builtin_props_kinds(
    void (*cb)(const char* kindName, const char* propNameFR, void* ctx),
    void* ctx);

/* ================================================================
 * STYLE STATE PALETTES (pour --vscode-data / designer)
 * ================================================================
 * Expose la palette d'etats de style (Style[survol]/[focus]/...) par type
 * de style porteur. Source unique : STYLE_PALETTES (compiler.c). Le designer
 * front lit ces etats au lieu de les coder en dur. Seuls fr/en sont exposes
 * (surface syntaxe Style[fr]) ; le slot template interne reste prive. */

/* Nombre de types de style porteurs d'une palette d'etats. */
int compiler_style_palette_count(void);

/* Nom du type de style a l'index donne (ex "styleSaisie"), NULL si hors borne. */
const char* compiler_style_palette_type_at(int paletteIdx);

/* Nombre d'etats de la palette a l'index donne. */
int compiler_style_palette_state_count(int paletteIdx);

/* Etat (fr/en) a (paletteIdx, stateIdx). Renvoie false si hors borne. Le slot
 * template interne n'est volontairement pas expose. */
bool compiler_style_palette_state_at(int paletteIdx, int stateIdx,
                                     const char** outFR, const char** outEN);

/* Index de palette du type COLLECTION porteur d'etats donne (ex
 * "styleZoneSaisieEtats"), ou -1 si le type ne porte pas d'etats. Casse/accent
 * insensible. Relie un sous-element a sa palette sans dupliquer les etats. */
int compiler_style_palette_index_for_type(const char* collTypeName);

/*  — etats valides indexables via ..Etats[etat] d'un type collection
 * styleXxxEtats donne, unifiant widgets (STYLE_PALETTES) ET menus (survol seul).
 * Source unique cote compiler.c ; le catalogue LSP les derive sans recopie.
 * count = 0 si le type ne porte pas d'etats ; state_at renvoie false hors borne. */
int compiler_etats_type_state_count(const char* etatsType);
bool compiler_etats_type_state_at(const char* etatsType, int stateIdx,
                                  const char** outFR, const char** outEN);

/* Types de style RACINE exposes dans l'accordeon du designer (styleSaisie, futur
 * styleBouton...). Le --vscode-data itere cette liste puis derive les sous-
 * elements et leurs etats depuis le registre de types. */
int compiler_style_model_count(void);
const char* compiler_style_model_type_at(int idx);

/*  — self-test du garde de coherence pre-scan / parse des membres de
 * CollectionDeProcedures. Implante dans compiler.c : le garde et les deux
 * indexations qu'il confronte sont des statics de ce fichier, les exposer pour
 * les besoins d'un test elargirait la surface interne du compilateur. Retourne
 * le nombre de cas en echec (0 = garde conforme). */
int collection_prescan_guard_self_test(void);

#endif
