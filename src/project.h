// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_project_h
#define owl_project_h

#include "common.h"
#include "vm.h"

/* ================================================================
 * PROJECT FILE
 * ================================================================ */

typedef struct {
    char* name;       /* stem without extension, e.g. "utils" */
    char* path;       /* full path to the file */
    char* source;     /* file contents (malloc'd) */
    bool isTest;      /* true if filename starts with "test_" */
} ProjectFile;

/* Overlay = contenu non-sauve d'un fichier injecte a la place du disque au
 * moment de la lecture des sources ( Lot 1). Le LSP passe les
 * buffers ouverts non-sauves ; le check compile ce contenu-la, les autres
 * fichiers restant lus du disque. `path` = forme brute telle que fournie par
 * l'appelant ; le matching contre les fichiers projet se fait par chemin
 * canonique des deux cotes. */
typedef struct {
    char* path;     /* chemin du fichier overlaye (brut, canonise au matching) */
    char* content;  /* contenu a substituer au disque */
} ProjectOverlay;

/* ================================================================
 * PROJECT
 * ================================================================ */

typedef struct {
    ProjectFile* files;
    int fileCount;
    int fileCapacity;
    int entryIndex;      /* effective entry point (index dans files) */
    bool isProjectMode;  /* true si un projet.owl gouverne l'execution :
                          * boot i18n depuis ses metadonnees + teardown
                          * Fermeture()/Closing() en fin de run. false en
                          * mode script (fichier nomme, aucun projet.owl). */
    char* rootDir;
    ProjectOverlay* overlays;  /* buffers non-sauves injectes */
    int overlayCount;
} Project;

/* Enregistre un overlay {path -> content} sur le projet AVANT discover : a la
 * lecture des sources, tout fichier dont le chemin canonique matche `path`
 * est compile depuis `content` au lieu du disque ( Lot 1). Les deux
 * chaines sont copiees. A appeler apres project_init, avant
 * project_discover_scoped. */
void project_set_overlay(Project* project, const char* path,
                         const char* content);

/* Renvoie le contenu overlaye pour `path` (matching chemin canonique, meme
 * regle que le discover), sinon NULL. Permet au chemin de check MONO d'injecter
 * l'overlay du fichier cible sans passer par le scan projet. */
const char* project_get_overlay(Project* project, const char* path);

/* Initialize a project struct (zeroes everything). */
void project_init(Project* project);

/* Discover all .owl files in dir recursively. Returns false on error.
 * project_discover skips test/tests subdirs (for run mode).
 * project_discover_ex lets you control that behavior. */
bool project_discover(Project* project, const char* dir);
bool project_discover_ex(Project* project, const char* dir, bool skipTestDirs);

/* Variante avec controle de la recursion. recursive=false => seul le
 * dossier `dir` est scanne, les sous-dossiers sont ignores (modes
 * script / point d'entree d'). recursive=true equivaut a
 * project_discover_ex. */
bool project_discover_scoped(Project* project, const char* dir,
                             bool skipTestDirs, bool recursive);

/* ================================================================
 * FRONTIERE DE PROJET
 * ================================================================ */

typedef enum {
    PROJECT_MODE_SCRIPT,     /* aucun projet.owl ancetre : dossier du fichier seul */
    PROJECT_MODE_PROJECT     /* projet.owl ancetre : arbre raciné (recursif) */
} ProjectMode;

/* Resout la frontiere du fichier `filepath` par remontee des dossiers
 * parents (, modele a 2 niveaux) :
 *  - premier `projet.owl` rencontre en remontant -> PROJECT, racine = son
 *    dossier (scan recursif) ;
 *  - sinon -> SCRIPT, racine = dossier du fichier (voisins non-recursifs).
 * Ecrit la racine resolue dans `rootOut` (`rootSize` >= 1024 recommande,
 * separateurs `/`). Retourne le mode. La remontee est bornee a la racine
 * du systeme de fichiers. */
ProjectMode resolve_project_root(const char* filepath,
                                 char* rootOut, size_t rootSize);

/* Find a file by its basename (e.g. "utils.owl"). Returns index or -1. */
int project_find_file(Project* project, const char* basename);

/* Le fichier `index` participe-t-il au perimetre de COMPILATION, en tant que
 * compagnon (hors fichier d'entree) ? Suppose une decouverte deja bornee a la
 * frontiere du fichier (project_discover_scoped + resolve_project_root,
 * ).
 *
 * Site de verite unique, partage par l'execution (project_run, qui compile puis
 * execute ces fichiers) et par le controle (`--check`, qui doit couvrir
 * EXACTEMENT le meme perimetre : un controle plus etroit rend vert un fichier
 * que l'execution refuse). Defini dans project_run.c. */
bool project_companion_in_compile_scope(const Project* project, int index,
                                        int entryIndex);

/* Run the project: prescan, compile, execute. */
InterpretResult project_run(Project* project);

/* `owl serve` : compile la frontiere projet (compagnons puis entree,
 * meme moteur que project_run) sans jouer le teardown de fin de vie — le
 * serveur possede son propre cycle (FinInitialisation / Fermeture, http_exec.c).
 * Les tableaux sont empruntes (l'appelant garde la propriete des chaines). */
InterpretResult project_run_sources_no_teardown(
        const char* const* names, const char* const* paths,
        const char* const* sources, int count,
        int entryIndex, bool isProjectMode);

/* Teardown de fin de run PROJET : appelle Fermeture() (FR canon) ou son
 * alias Closing() (EN) si definie. N'auto-appelle PLUS Main() ( :
 * fin du dispatch cache, pilier 2 — le top-level EST le programme). A
 * n'appeler qu'en mode PROJET ; en mode script pur, aucun teardown projet.
 * Convention partagee entre project_run et le test runner. */
InterpretResult call_project_teardown(void);

/* Free all memory associated with the project. */
void project_free(Project* project);

/* Build a standalone executable from the project.
 * dir: project directory to scan.
 * outputName: output exe name (without .exe).
 * owlInstallRoot: path to OWL install (lib/<target>/ + include/owl/).
 * target: target triple (windows-x86_64, linux-x86_64, etc.) ou NULL = host.
 *         Selectionne lib/<target>/ et passe -target a zig cc. */
int build_project(const char* dir, const char* outputName,
                  const char* owlInstallRoot, const char* target);

/* Shared file reading utility (moved from main.c). */
char* read_file(const char* path);

/* Check if a path is a directory. */
bool is_directory(const char* path);

/* Check if a file exists. */
bool file_exists(const char* path);

/* Vrai si `name` est le marqueur de frontiere de projet, graphie FR
 * (`projet`) ou EN (`project`), casse-insensible, avec ou sans `.owl`. */
bool is_projet_marker_name(const char* name);

/* Vrai si le dossier `dir` contient, a son TOP-LEVEL (pas en recursif), le
 * marqueur de projet de graphie `spelling` ("projet.owl" FR ou "project.owl"
 * EN). Permet a owl-build de verifier la frontiere du dossier donne avant de
 * lancer le scan recursif. */
bool project_dir_has_marker(const char* dir, const char* spelling);

/* Vrai si le dossier contenant `filepath` heberge les DEUX graphies du
 * marqueur de projet (projet.owl + project.owl) : coexistence ambigue de la
 * frontiere. Utilise par le controle compile-time (--check / LSP), en miroir
 * de la detection deja faite par le chemin d'execution (project_discover). */
bool project_dir_has_ambiguous_markers(const char* filepath);

#endif
