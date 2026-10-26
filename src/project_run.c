// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ================================================================
 * PROJECT RUNTIME — execution & build (scinde de project.c)
 * ================================================================
 * Partie LOURDE du modele de projet : execution (call_project_teardown,
 * project_run) et packaging (build_project). Tire le compilateur + la VM
 * (compile_in_project, InterpretResult). Separe de project.c (decouverte +
 * IO, compiler-free) pour que owl-lsp.exe puisse linker la decouverte sans
 * embarquer le runtime.
 * ================================================================ */

#include "project.h"
#include "prescan.h"
#include "compiler.h"
#include "memory.h"
#include "owl_errors.h"  /* owl_error_render_lang_is_en : chrome owl-build bilingue */
#include "miniz.h"       /* deflate du source embarque dans le bundle genere (spike ) */
/* owl_service_registry_count : detection ServiceREST au build */
#include "service_decl.h"

/* boot i18n depuis les metadonnees de projet.owl (defini dans stdlib_i18n.c).
 * Declare ici pour eviter de tirer owl_stdlib.h complet.  : pose la
 * langue source depuis un tag BCP 47 capture au compile-time. */
extern void owl_i18n_set_project_source(const char* tag);
/*  — pose la liste complete des langues declarees (tags BCP 47) pour
 * ListeLangues(), en symetrie de la source. */
extern void owl_i18n_set_project_languages(const char* const* tags, int count);

/*  — plombe le runtime i18n depuis la capture compile-time du champ
 * Langues : source (1ere langue) + liste complete. Appele aux 2 sites de boot
 * projet, juste apres le compile de projet.owl. */
static void owl_boot_project_i18n(void) {
    const char* tag = compiler_projet_boot_langue();
    if (tag) owl_i18n_set_project_source(tag);
    int n = compiler_projet_boot_langue_count();
    if (n > 0) {
        const char* tags[256];
        if (n > 256) n = 256;
        for (int i = 0; i < n; i++) tags[i] = compiler_projet_boot_langue_tag(i);
        owl_i18n_set_project_languages(tags, n);
    } else {
        owl_i18n_set_project_languages(NULL, 0);
    }
}

#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

#ifdef _WIN32
#  include <direct.h>
#  define OWL_BUILD_MKDIR_ONE(p) _mkdir(p)
#  define OWL_BUILD_RMDIR_ONE(p) _rmdir(p)
#else
#  include <sys/types.h>
#  include <unistd.h>
#  define OWL_BUILD_MKDIR_ONE(p) mkdir((p), 0755)
#  define OWL_BUILD_RMDIR_ONE(p) rmdir((p))
#endif

/* mkdir -p : cree `path` et tous ses parents manquants. Utilise par
 * build_project pour materialiser le dossier de sortie JUSTE avant d'ecrire
 * l'exe, pas plus tot : ainsi un build qui echoue en amont (portail
 * de compile, compile du bundle) ne laisse aucun `exe/<target>/` vide derriere
 * lui. `is_directory` vient de project.h. */
static int build_ensure_dirs(const char* path) {
    if (is_directory(path)) return 0;
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\\') buf[i] = '/';
    }
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            /* Saute les segments racine non creables : "" (slash de tete POSIX)
             * et "C:" (lettre de lecteur Windows, mkdir echoue). */
            int is_root = (buf[0] == '\0') ||
                          (buf[1] == ':' && buf[2] == '\0' &&
                           ((buf[0] >= 'A' && buf[0] <= 'Z') ||
                            (buf[0] >= 'a' && buf[0] <= 'z')));
            if (!is_root && !is_directory(buf)) {
                if (OWL_BUILD_MKDIR_ONE(buf) != 0 && !is_directory(buf)) return -1;
            }
            buf[i] = '/';
        }
    }
    if (OWL_BUILD_MKDIR_ONE(buf) != 0 && !is_directory(buf)) return -1;
    return 0;
}

/* Retire le dossier de sortie qu'on venait de materialiser pour le linker
 * quand le link echoue. `rmdir` simple REFUSE un dossier non vide :
 * si le link n'a rien produit, `exe/<target>/` (puis `exe/`) est vide et
 * disparait ; s'il contient l'exe d'un build anterieur reussi, le rmdir echoue
 * silencieusement et ne detruit RIEN. On ignore volontairement le code de
 * retour. JAMAIS de suppression recursive/forcee ici : toute la surete tient
 * au refus des dossiers non vides. */
static void build_rmdir_output_if_empty(const char* outputName) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", outputName);
    char* lastSep = NULL;
    for (char* p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') lastSep = p;
    }
    if (lastSep == NULL) return;
    *lastSep = '\0';              /* buf -> .../exe/<target> */
    (void)OWL_BUILD_RMDIR_ONE(buf);
    char* parentSep = NULL;
    for (char* p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') parentSep = p;
    }
    if (parentSep == NULL) return;
    *parentSep = '\0';            /* buf -> .../exe */
    (void)OWL_BUILD_RMDIR_ONE(buf);
}

/* ================================================================
 * TEARDOWN PROJET
 *
 * Fin du dispatch cache (pilier 2) : Main() n'est PLUS jamais auto-appelee.
 * Le top-level du fichier/projet lance EST le programme.
 *
 * Seul reste un teardown : Fermeture() (FR canon) ou son alias Closing()
 * (EN), event de fin de vie du PROJET, auto-appele apres l'entree — et
 * UNIQUEMENT en mode projet. En mode script pur (aucun projet.owl), aucun
 * teardown projet ; les fenetres / Etats / classes gardent leur propre
 * cycle de vie. Coloration LSP "support.function.event".
 * ================================================================ */

InterpretResult call_project_teardown(void) {
    /* Check d'existence casse+accent insensible (invariants 1 & 2). Fermeture
     * (FR canon) prend la priorite. Sinon, alias Closing (EN). */
    if (vm_global_exists("Fermeture")) {
        return vm_call_global("Fermeture");
    }
    if (vm_global_exists("Closing")) {
        return vm_call_global("Closing");
    }
    return INTERPRET_OK;
}

/*  — un ObjFunction issu de compile_in_project_split (declarations ou
 * corps) est "vide" quand il ne contient que son terminateur OP_NIL+OP_RETURN
 * (2 octets). L'executer serait un no-op fonctionnel, MAIS son OP_NIL porte le
 * termLine (derniere ligne du fichier) : le hook debug se declencherait alors
 * sur cette ligne — voire un breakpoint pose la — a un instant faux (p.ex. la
 * declFn vide de l'entree jouee en phase 1 ferait hit un breakpoint du corps
 * avant meme que le corps ne tourne). On saute donc son execution. */
static bool split_fn_is_empty(const ObjFunction* fn) {
    return fn == NULL || fn->chunk == NULL || fn->chunk->count <= 2;
}

/*  — compare deux identifiants ASCII casse-insensible. Le nom porte
 * par ObjFunction->name est deja strippe de ses accents par le scanner, donc
 * une casse-insensibilite ASCII suffit a honorer les invariants 1 & 2 sur le
 * nom du hook (Terminaison / Termination). */
static bool ident_equals_ci(const char* a, const char* b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/*  — extrait le hook de fin de vie d'un compagnon : sa PROCEDURE
 * Terminaison() (FR canon) ou Termination() (alias EN).  : cet event
 * n'existe QUE dans un bloc `est une CollectionDeProcédures` — un fichier a
 * plat qui declare une procedure de ce nom ne participe a aucun cycle de vie
 * de projet (la garde vit chez l'appelant). On la capture
 * comme ObjFunction directement dans le pool de constantes de son chunk de
 * declarations — c'est le meme objet que OP_DEFINE_GLOBAL lie au global. La
 * capture est PAR FICHIER parce que vm.globals est une table plate : deux
 * compagnons declarant chacun Terminaison() s'y ecraseraient et un
 * vm_call_global naif n'en rejouerait qu'UN. FR prioritaire sur l'alias EN
 * (meme regle que call_project_teardown pour Fermeture/Closing). */
static ObjFunction* companion_termination_hook(const ObjFunction* declFn) {
    if (declFn == NULL || declFn->chunk == NULL) return NULL;
    ObjFunction* frHook = NULL;
    ObjFunction* enHook = NULL;
    for (int i = 0; i < declFn->chunk->constants.count; i++) {
        Value v = declFn->chunk->constants.values[i];
        if (!IS_FUNCTION(v)) continue;
        ObjFunction* fn = AS_FUNCTION(v);
        if (fn->name == NULL) continue;
        if (ident_equals_ci(fn->name->chars, "Terminaison")) frHook = fn;
        else if (ident_equals_ci(fn->name->chars, "Termination")) enHook = fn;
    }
    return frHook != NULL ? frHook : enHook;
}

/* Perimetre de compilation d'un compagnon (contrat documente dans project.h).
 *
 *  — en mode projet, un compagnon PLAT (aucun bloc DSL au top-level)
 * est un script prive : invisible au projet, ni indexe, ni hydrate, ni compile.
 * En mode script, tous les voisins du dossier participent. */
bool project_companion_in_compile_scope(const Project* project, int index,
                                        int entryIndex) {
    if (index < 0 || index >= project->fileCount) return false;
    if (index == entryIndex) return false;
    if (project->files[index].isTest) return false;
    if (project->isProjectMode
        && !prescan_source_is_dsl_block_file(project->files[index].source)) {
        return false;
    }
    return true;
}

/* ================================================================
 * PUBLIC: PROJECT RUN
 * ================================================================ */

/* compileOnly : execute la PHASE 1 complete (prescan + compile de
 * tous les fichiers + exec des declarations, necessaire a la resolution de
 * type cross-fichier au compile -> parite stricte avec le demarrage de l'exe),
 * mais SAUTE la phase 2 (corps imperatifs) et le teardown. Tout compile se fait
 * en phase 1, donc ce mode detecte toutes les erreurs de compilation sans
 * executer le programme de l'utilisateur. Sert au portail de compile d'owl
 * build (build_project). */
static InterpretResult project_run_ex(Project* project, bool runTeardown,
                                      bool compileOnly) {
    int entry = project->entryIndex;
    if (entry < 0) {
        fprintf(stderr, "owl: aucun point d'entree defini\n");
        return INTERPRET_RUNTIME_ERROR;
    }

    /* Count non-test files (excluding entry) to decide mode */
    int collectionCount = 0;
    for (int i = 0; i < project->fileCount; i++) {
        if (i == entry || project->files[i].isTest) continue;
        collectionCount++;
    }

    /* Single file mode: pas de collections cross-file, mais on fait quand
     * même un prescan du fichier d'entrée pour tracker ses globals top-level
     * et permettre la forward reference (PROCEDURE Foo qui réfère à gVar
     * déclaré plus bas dans le même fichier). Coût ~µs, gain : zéro
     * contrainte d'ordre pour l'utilisateur.
     *
     *  : le top-level EST le programme (plus d'auto-appel Main()).
     * En mode projet uniquement, on boote l'i18n depuis les metadonnees
     * compile-time de projet.owl AVANT execution, puis on appelle le teardown
     * Fermeture()/Closing() apres. */
    if (collectionCount == 0) {
        SymbolTable* symbols = symtable_create();
        prescan_file(symbols, project->files[entry].source, entry);

        bool entryIsProjet = is_projet_marker_name(project->files[entry].name);
        if (project->isProjectMode && entryIsProjet) {
            compiler_projet_boot_langue_reset();
        }
        ObjFunction* fn = compile_in_project(
            project->files[entry].source,
            project->files[entry].path,
            symbols,
            entry);
        if (fn == NULL) {
            symtable_destroy(symbols);
            return INTERPRET_COMPILE_ERROR;
        }
        if (compileOnly) {
            /* Compile reussi (fn != NULL) : c'est tout ce que le portail
             * verifie. Pas d'exec, pas de boot i18n, pas de teardown. */
            symtable_destroy(symbols);
            return INTERPRET_OK;
        }
        if (project->isProjectMode && entryIsProjet) {
            owl_boot_project_i18n();
        }
        InterpretResult r = vm_execute(fn);
        symtable_destroy(symbols);
        if (r != INTERPRET_OK) return r;
        if (project->isProjectMode && runTeardown) return call_project_teardown();
        return r;
    }

    /* ---- Multi-file mode ---- */

    /* 1. Pre-scan all non-test files for top-level declarations */
    SymbolTable* symbols = symtable_create();

    /* Register file names for qualified name resolution.
     * Le fichier d'entree n'est JAMAIS skippe meme s'il est test_*.owl :
     * `owl <test_x.owl>` doit hydrater ses globals top-level pour que les
     * voisins resolvent ses procedures (parite avec --check / owl test).
     * Les AUTRES test_*.owl voisins restent exclus (companions). */
    for (int i = 0; i < project->fileCount; i++) {
        if (project->files[i].isTest && i != entry) continue;
        if (i < MAX_PROJECT_FILES) {
            size_t len = strlen(project->files[i].name);
            if (len >= 128) len = 127;
            memcpy(symbols->fileNames[i], project->files[i].name, len);
            symbols->fileNames[i][len] = '\0';
            if (i >= symbols->fileNameCount) {
                symbols->fileNameCount = i + 1;
            }
        }
    }

    /*  — en mode projet, restreint la decouverte aux fichiers a bloc
     * DSL + l'entree. Les compagnons PLATS (scripts prives) sont exclus de
     * l'indexation ET de l'hydratation (sources[]). Mode script : pas de
     * filtre. Reset immediat apres la boucle (le gate n'est lu que par
     * prescan_file). */
    prescan_set_project_scope(project->isProjectMode, entry);
    for (int i = 0; i < project->fileCount; i++) {
        if (project->files[i].isTest && i != entry) continue;
        if (!prescan_file(symbols, project->files[i].source, i)) {
            prescan_set_project_scope(false, -1);
            symtable_destroy(symbols);
            return INTERPRET_COMPILE_ERROR;
        }
    }
    prescan_set_project_scope(false, -1);

    /* 1b. Verify interface contracts */
    if (!symtable_verify_interfaces(symbols)) {
        symtable_destroy(symbols);
        return INTERPRET_COMPILE_ERROR;
    }

    InterpretResult result = INTERPRET_OK;

    /* projet.owl (marqueur bilingue FR/EN, casse-insensible) : son bloc
     * `<Ident> est un Projet` pose le global metadonnees (APP..Version)
     * consomme en cascade par les compagnons. Il doit donc etre COMPILE tot
     * (pour capturer la langue source compile-time, ). Ses metadonnees
     * (langue, theme, fenetre de demarrage...) sont materialisees en phase 1.a,
     * disponibles pour tous les compagnons.
     *
     *  : le CORPS de son bloc = le PROGRAMME d'initialisation du projet
     * (globales, deps). Il s'execute TOUJOURS (mode fichier ET projet) : quand
     * projet.owl est compagnon, son corps joue en phase 2.a (avant les autres
     * compagnons) ; quand il EST l'entree, son corps = le programme et joue en
     * DERNIER (phase 2.c). La fenetre affichee au demarrage n'est plus un
     * Ouvre() code en dur ici mais la metadonnee typee FenetreDemarrage, ouverte
     * par le lancement projet (main.c) — pas en lancement fichier-isole (F9). */
    int projetIdx = -1;
    for (int i = 0; i < project->fileCount; i++) {
        if (project->files[i].isTest) continue;
        if (is_projet_marker_name(project->files[i].name)) {
            projetIdx = i;
            break;
        }
    }

    /* ================================================================
     * Modele deux phases (, ) : materialiser TOUTES les
     * declarations statiques (entree incluse) AVANT tout code imperatif.
     * Un voisin peut ainsi ouvrir une fenetre / appeler une procedure /
     * instancier une classe declaree dans le fichier lance sans "inconnue".
     *
     * Phase 1 (interleavee compile->exec-decl) : le compilateur lit
     * vm.globals au compile pour resoudre des types cross-fichier (R1),
     * donc la declFn d'un fichier DOIT etre executee avant que le fichier
     * suivant compile. Ordre lexicographique, entree en dernier (ses
     * definitions gagnent). Phase 2 : corps imperatifs, ordre 
     * (collections, puis compagnons, puis fichier lance, puis projet.owl
     * EN DERNIER — "le programme").
     *
     * Racines GC : bodyFns garde vivants les corps des compagnons
     * (obj_array_push canon). projetBodyFn / entryBodyFn sont rootes
     * separement (ordre d'execution specifique en phase 2). rootCount =
     * nombre de vm_push persistants a depiler en sortie / sur erreur.
     * Les declFn transitoires sont push/exec/pop en LIFO local. */
    ObjArray* bodyFns = obj_new_array();
    vm_push(OBJ_VAL(bodyFns));
    /*  — hooks Terminaison()/Termination() des compagnons (un slot par
     * fichier qui en declare un), collectes dans l'ordre d'init (= ordre de
     * remplissage de bodyFns) pour un teardown en ordre INVERSE. Rootes via
     * rootCount : ils doivent survivre jusqu'au teardown (phase 3), la ou
     * vm.globals ne conserve qu'un seul homonyme. */
    ObjArray* termFns = obj_new_array();
    vm_push(OBJ_VAL(termFns));
    /*  — corps des compagnons-COLLECTION isoles de bodyFns : ils
     * initialisent les globales manglees COL$xxx + les procedures de la
     * collection (le bloc entier est classe "statement" par le split, donc son
     * init vit dans le corps, pas les declarations). Le corps de projet.owl
     * peut consommer ces symboles en nom nu ET tourne quelle que soit l'entree
     * (comportement A). On execute donc ces corps AVANT lui (phase 2.pre),
     * cohérent "dependances d'abord" (/0120), dans TOUS les chemins. */
    ObjArray* collectionBodyFns = obj_new_array();
    vm_push(OBJ_VAL(collectionBodyFns));
    int rootCount = 3;

    ObjFunction* projetBodyFn = NULL;
    ObjFunction* entryBodyFn  = NULL;

    /* ---- Phase 1.a : declarations de projet.owl EN PREMIER ----
     * (cascade metadonnees MonApp..Champ avant les compagnons, ).
     * Son corps est differe en phase 2 ; si projet.owl EST l'entree, son
     * corps = le programme et joue en dernier (phase 2.c). */
    if (projetIdx >= 0) {
        compiler_projet_boot_langue_reset();
        ObjFunction* projetDeclFn = NULL;
        if (!compile_in_project_split(
                project->files[projetIdx].source,
                project->files[projetIdx].path, symbols, projetIdx,
                &projetDeclFn, &projetBodyFn)) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return INTERPRET_COMPILE_ERROR;
        }
        /* Root immediat des 2 Obj frais avant toute nouvelle alloc.
         * projetBodyFn persistant (joue en phase 2) ; projetDeclFn transitoire
         * (jusqu'apres son exec). Aucune alloc entre le split et ces push. */
        vm_push(OBJ_VAL(projetBodyFn));
        rootCount++;
        vm_push(OBJ_VAL(projetDeclFn));

        /*  — boot i18n depuis la langue source capturee au compile,
         * AVANT tout compagnon (metadonnee introspectable, ). */
        if (project->isProjectMode) {
            owl_boot_project_i18n();
        }
        /*  — materialise le global de metadonnees (MonApp) juste
         * apres le compile : la cascade cross-fichier (MonApp..Licence lue
         * depuis un compagnon) le voit des la phase 1. projetBodyFn rooté ->
         * les Value capturees (constantes du chunk) sont vivantes. */
        if (project->isProjectMode) {
            compiler_projet_meta_materialize();
        }
        /*  — hisse les variables GLOBAL du corps du bloc Projet (nom +
         * valeur zero du type) en Phase 1 : un compagnon lance avant le corps de
         * projet.owl (Phase 2.c) resout la globale, meme si son init n'a pas
         * encore tourne. Vaut mode projet ET mode fichier (le corps projet joue
         * toujours en dernier). */
        compiler_projet_globals_materialize();

        /* Execute les declarations de projet.owl (classes, procedures,
         * enums...) pour qu'un fichier ulterieur les resolve au compile. */
        if (!split_fn_is_empty(projetDeclFn)) result = vm_execute(projetDeclFn);
        vm_pop();  /* projetDeclFn : plus utile */
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* ---- Phase 1.b : declarations des compagnons ----
     * hors entree, hors projet.owl (deja traite). Chaque declFn est executee
     * immediatement (R1) ; le corps est accumule dans bodyFns pour la phase 2.
     *
     *  : l'ordre lexicographique seul plante quand un compagnon enfant
     * (cAnimal Herite de cMammifere) est nomme AVANT son parent — a la
     * materialisation, `Herite de cMammifere` ne resout pas encore le parent.
     * On collecte d'abord les compagnons dans l'ordre de decouverte (stable),
     * puis on les reordonne par profondeur d'heritage (parent avant enfant),
     * comme owl test le fait deja. Le tri est stable : les fichiers sans
     * heritage gardent leur ordre lexicographique. La phase 1.c (entree en
     * dernier, ses definitions gagnent — ) reste inchangee : l'entree
     * n'entre jamais dans cette liste. */
    int companionOrder[MAX_PROJECT_FILES];
    int companionOrderCount = 0;
    for (int i = 0; i < project->fileCount; i++) {
        if (!project_companion_in_compile_scope(project, i, entry)) continue;
        if (i == projetIdx) continue;  /* deja traite en 1.a */
        if (companionOrderCount < MAX_PROJECT_FILES) {
            companionOrder[companionOrderCount++] = i;
        }
    }
    topo_sort_companions_by_inheritance(symbols, companionOrder,
                                        companionOrderCount);

    for (int oc = 0; oc < companionOrderCount; oc++) {
        int i = companionOrder[oc];
        /*  : .path (pas .name) — messages d'erreur vers le chemin reel. */
        ObjFunction* declFn = NULL;
        ObjFunction* bodyFn = NULL;
        if (!compile_in_project_split(
                project->files[i].source,
                project->files[i].path, symbols, i, &declFn, &bodyFn)) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return INTERPRET_COMPILE_ERROR;
        }
        /* declFn transitoire rooté sur la pile (aucune alloc avant ce push) ;
         * bodyFn rooté en permanence via bodyFns (obj_array_push canon le
         * protege pendant son propre grow). */
        vm_push(OBJ_VAL(declFn));
        /*  — le flag reflete le compile qui vient de tourner (aucun
         * recompile d'ici la fin de l'iteration). Un fichier-collection voit son
         * corps route vers collectionBodyFns (execute en phase 2.pre) ; les
         * autres compagnons restent dans bodyFns (phase 2.b). bodyFn est rooté
         * par l'un ou l'autre tableau via obj_array_push canon. */
        bool fileIsCollection = compiler_file_declares_collection();
        obj_array_push(fileIsCollection ? collectionBodyFns : bodyFns,
                       OBJ_VAL(bodyFn));
        /*  — capture le hook de fin de vie eventuel de CE compagnon.
         * Le push suit companionOrder, donc termFns reste aligne sur l'ordre
         * d'init des collections, condition du teardown en ordre inverse. Le
         * hook est une constante de declFn (stack-rooted ici) et de bodyFn
         * (rooté par son tableau), donc vivant pendant l'obj_array_push (GC).
         *
         *  — capture GATEE sur la declaration d'un bloc collection
         * dans CE fichier (meme flag) : l'event de fin de vie est une surface du
         * bloc, pas une magie de nom disponible sur n'importe quel fichier. */
        ObjFunction* termHook = fileIsCollection
                              ? companion_termination_hook(declFn) : NULL;
        if (termHook != NULL) obj_array_push(termFns, OBJ_VAL(termHook));
        if (!split_fn_is_empty(declFn)) result = vm_execute(declFn);
        vm_pop();  /* declFn */
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* ---- Phase 1.c : declarations du fichier d'entree EN DERNIER ----
     * (ses definitions gagnent en cas de conflit). Si l'entree EST projet.owl,
     * ses declarations ont deja ete jouees en 1.a et son corps = projetBodyFn. */
    if (projetIdx == entry) {
        entryBodyFn = projetBodyFn;
    } else {
        /*  : .path (pas .name). */
        ObjFunction* entryDeclFn = NULL;
        if (!compile_in_project_split(
                project->files[entry].source,
                project->files[entry].path, symbols, entry,
                &entryDeclFn, &entryBodyFn)) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return INTERPRET_COMPILE_ERROR;
        }
        vm_push(OBJ_VAL(entryBodyFn));   /* persistant : joue en phase 2.b */
        rootCount++;
        vm_push(OBJ_VAL(entryDeclFn));   /* transitoire */
        if (!split_fn_is_empty(entryDeclFn)) result = vm_execute(entryDeclFn);
        vm_pop();  /* entryDeclFn */
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* ================================================================
     * Phase 2 — code imperatif (ordre ). Toutes les declarations
     * statiques du perimetre sont deja vivantes.
     * ================================================================ */

    /* Portail de compile : tout compile de phase 1 est fait. On
     * saute la phase 2 (corps imperatifs) et le teardown — on ne veut PAS
     * executer le programme, juste prouver qu'il compile. */
    if (compileOnly) {
        while (rootCount-- > 0) vm_pop();
        symtable_destroy(symbols);
        return result;
    }

    /* 2.pre corps des collections EN PREMIER. Elles definissent les
     * globales/procs consommees en nom nu par les compagnons (2.a), le fichier
     * lance (2.b) et le corps de projet.owl (2.c). Ordre companionOrder (= ordre
     * d'init capture par termFns, pour un teardown en ordre inverse). */
    for (int i = 0; i < collectionBodyFns->elements.count; i++) {
        ObjFunction* bodyFn = AS_FUNCTION(collectionBodyFns->elements.values[i]);
        if (split_fn_is_empty(bodyFn)) continue;
        result = vm_execute(bodyFn);
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* 2.a corps des compagnons NON-collection, ordre lexicographique (bodyFns).
     * Les collections ont deja tourne en 2.pre. */
    for (int i = 0; i < bodyFns->elements.count; i++) {
        ObjFunction* bodyFn = AS_FUNCTION(bodyFns->elements.values[i]);
        if (split_fn_is_empty(bodyFn)) continue;
        result = vm_execute(bodyFn);
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* 2.b corps du fichier explicitement lance (F9), SAUF s'il EST projet.owl
     * (son corps = le programme, joue en 2.c).  : "le fichier lance
     * execute toujours son propre code de niveau superieur". */
    if (projetIdx != entry && !split_fn_is_empty(entryBodyFn)) {
        result = vm_execute(entryBodyFn);
        if (result != INTERPRET_OK) {
            while (rootCount-- > 0) vm_pop();
            symtable_destroy(symbols);
            return result;
        }
    }

    /* 2.c corps de projet.owl EN DERNIER ( : "le corps de son bloc est
     * le programme, execute en dernier, quel que soit le fichier lance"). Vaut
     * qu'il soit l'entree (owl <dossier>) OU compagnon (F9 sur un voisin) : dans
     * les deux cas il orchestre apres l'init des dependances (collections 2.pre,
     * compagnons 2.a) et apres le code propre du fichier lance (2.b).  :
     * avant, le corps projet-compagnon jouait AVANT l'entree — il mutait les
     * globales de collection que l'entree lisait ensuite (ordre incoherent). */
    if (projetIdx >= 0 && !split_fn_is_empty(projetBodyFn)) {
        result = vm_execute(projetBodyFn);
    }

    if (result != INTERPRET_OK) {
        while (rootCount-- > 0) vm_pop();
        symtable_destroy(symbols);
        return result;
    }

    /* 3. Teardown projet (mode projet uniquement). Ordre canon, LIFO :
     *   a. Fermeture()/Closing() du projet D'ABORD.
     *   b. PUIS les Terminaison()/Termination() des compagnons, en ordre
     *      INVERSE de leur init. bodyFns/termFns sont en ordre d'init, on
     *      parcourt donc termFns a rebours. Chaque hook est un ObjFunction
     *      distinct capturé par-fichier (vm.globals aplati ne garderait qu'un
     *      homonyme) : on le rejoue via vm_execute. termFns reste rooté
     *      (rootCount) pendant toute cette phase — le depilement vient apres. */
    if (project->isProjectMode && runTeardown) {
        result = call_project_teardown();
        for (int i = termFns->elements.count - 1;
             i >= 0 && result == INTERPRET_OK; i--) {
            result = vm_execute(AS_FUNCTION(termFns->elements.values[i]));
        }
    }

    /* Racines persistantes (bodyFns, termFns, corps différés) plus necessaires. */
    while (rootCount-- > 0) vm_pop();

    symtable_destroy(symbols);
    return result;
}

InterpretResult project_run(Project* project) {
    return project_run_ex(project, true, false);
}

/* `owl serve` : compile TOUS les fichiers de la frontiere projet
 * (compagnons defs+corps AVANT l'entree, meme moteur que `owl <fichier>`) pour
 * qu'un ServiceREST resolve les symboles voisins (ETAT_X, procedures), puis
 * laisse la VM peuplee du routeur runtime. Le teardown de fin de vie
 * (Fermeture/Closing) est VOLONTAIREMENT saute : le serveur possede son propre
 * cycle (FinInitialisation au demarrage, Fermeture a l'arret, http_exec.c
 * ). Construit un Project ephemere depuis des tableaux paralleles —
 * les pointeurs sont empruntes (aucune copie), l'appelant garde la propriete. */
InterpretResult project_run_sources_no_teardown(
        const char* const* names, const char* const* paths,
        const char* const* sources, int count,
        int entryIndex, bool isProjectMode) {
    if (count <= 0 || entryIndex < 0 || entryIndex >= count) {
        return INTERPRET_COMPILE_ERROR;
    }
    Project project;
    project_init(&project);
    project.files = (ProjectFile*)calloc((size_t)count, sizeof(ProjectFile));
    if (project.files == NULL) return INTERPRET_COMPILE_ERROR;
    project.fileCapacity = count;
    project.fileCount    = count;
    project.entryIndex   = entryIndex;
    project.isProjectMode = isProjectMode;
    for (int i = 0; i < count; i++) {
        /* Emprunts : project_free ne libere que ce qu'il possede, or ces
         * chaines appartiennent a l'appelant. On les caste hors const et on
         * remet les pointeurs a NULL avant project_free (voir plus bas). */
        project.files[i].name   = (char*)names[i];
        project.files[i].path   = (char*)paths[i];
        project.files[i].source = (char*)sources[i];
        project.files[i].isTest = false;
    }
    InterpretResult r = project_run_ex(&project, false, false);
    /* Detache les emprunts pour que project_free ne les libere pas. */
    for (int i = 0; i < count; i++) {
        project.files[i].name = NULL;
        project.files[i].path = NULL;
        project.files[i].source = NULL;
    }
    project_free(&project);
    return r;
}

/* ================================================================
 * BUILD: GENERATE STANDALONE EXECUTABLE
 * ================================================================ */

/* `owl build` link contre les libs pre-compilees dans <owl_root>/lib/
 * (8 archives : libowl_core + 6 vendor + libxml2). La liste de sources
 * vit dans CMakeLists.txt (OWL_CORE_SRCS) — single source of truth.
 * Cf docs/internal_documentation/owl_build_architecture.md. */



/* Message "pas de projet ici" : owl-build package un projet, il faut un
 * projet.owl dans le dossier. Factorise entre le garde de frontiere (0
 * marqueur top-level) et le garde d'entree (projet.owl absent du perimetre
 * decouvert). Bilingue, forme minimale actionnable. */
static void owl_build_print_no_project(const char* dir) {
    fprintf(stderr, owl_error_render_lang_is_en()
        ? "owl build: no projet.owl in '%s'.\n"
          "  owl-build packages a project: a projet.owl file is required in the\n"
          "  directory. Minimal form:\n"
          "    MyApp is a Project\n"
          "      PROPERTIES\n"
          "        Languages = [languageEnglish]\n"
          "      END\n"
          "    END\n"
        : "owl build: aucun projet.owl dans '%s'.\n"
          "  owl-build package un projet : il faut un fichier projet.owl dans le\n"
          "  dossier. Forme minimale :\n"
          "    MonApp est un Projet\n"
          "      PROPRIETES\n"
          "        Langues = [langueFrancais]\n"
          "      FIN\n"
          "    FIN\n", dir);
}

int build_project(const char* dir, const char* outputName,
                  const char* owlInstallRoot, const char* target) {
    /* 0. Garde de frontiere. Avant tout scan, verifier qu'il y a un
     * marqueur de projet AU TOP-LEVEL du dossier donne (pas en recursif). Sans
     * ca, un dossier qui n'est pas lui-meme une frontiere lancerait
     * le scan recursif et ramasserait a plat des marqueurs de projets etrangers
     * enfouis dans des sous-dossiers sans rapport -> faux diagnostic "deux
     * graphies ambigues". */
    bool hasFr = project_dir_has_marker(dir, "projet.owl");
    bool hasEn = project_dir_has_marker(dir, "project.owl");
    if (hasFr && hasEn) {
        /* Ambiguite LEGITIME : les deux graphies au MEME top-level. Meme sens
         * que le garde-fou de project_discover, mais correctement scope a la
         * frontiere du dossier donne (pas a un sous-arbre entier). */
        fprintf(stderr, owl_error_render_lang_is_en()
            ? "owl build: two ambiguous project markers (keep a single spelling):\n"
            : "owl build: deux marqueurs de projet ambigus (garder une seule "
              "graphie) :\n");
        fprintf(stderr, "  %s/projet.owl\n", dir);
        fprintf(stderr, "  %s/project.owl\n", dir);
        return 65;
    }
    if (!hasFr && !hasEn) {
        owl_build_print_no_project(dir);
        return 65;
    }

    /* 1. Discover project files */
    Project project;
    project_init(&project);

    if (!project_discover(&project, dir)) {
        project_free(&project);
        return 65;
    }

    /*  — l'entree d'un projet est projet.owl (son top-level = le
     * programme). `owl build` exige donc un projet.owl. */
    int buildEntry = -1;
    for (int i = 0; i < project.fileCount; i++) {
        if (project.files[i].isTest) continue;
        if (is_projet_marker_name(project.files[i].name)) {
            buildEntry = i;
            break;
        }
    }
    if (buildEntry < 0) {
        owl_build_print_no_project(dir);
        project_free(&project);
        return 65;
    }

    /* 1bis. Portail de compile (, etendu UI ). owl-build ne
     * package AUCUN bytecode : la source OWL n'est compilee qu'au demarrage de
     * l'exe produit. Sans ce garde, un projet qui ne compile pas donne quand
     * meme un .exe mort-ne (exit 0, erreur visible seulement au lancement). On
     * compile-check ici, dans le registre console d'owl-build : meme chemin de
     * compile que le demarrage de l'exe (project_run phase 1), sans executer le
     * programme. Le registre console annonce desormais les natives UI + Etats/PDF
     * (stdlib_ui_defs.c / stdlib_pdf_defs.c, Qt-free) -> parite stricte du verdict
     * avec l'exe GUI produit. Erreurs -> diagnostics compilateur sur stderr (deja
     * emis par le compile) + exit 65, aucun bundle genere, aucun exe, aucune ligne
     * `-> path` (build_main n'imprime `-> path` que si build_project rend 0). */
    /*  — un projet qui declare un ServiceREST doit etre package en
     * SERVEUR (main de montage serve : bind + boucle accept), pas en programme
     * run-once (qui sortirait aussitot le top-level fini). La detection reutilise
     * le registre compile-time de services peuple par le portail de compile
     * ci-dessous (le meme que lit `owl serve`). Capte AVANT compiler_reset_poo_state. */
    bool declaresService = false;
    /*  — projet fenetre : le bloc `est un Projet` designe une
     * FenetreDemarrage. Meme signal que l'ouverture de fenetre. Non
     * vide => appli GUI -> exe lie en sous-systeme windows (pas de console au
     * double-clic). Capture dans le portail de compile, AVANT vm_free (la
     * designation est un const char* du chunk, invalide apres liberation du
     * tas). Un projet serveur (ServiceREST) reste console meme s'il designe une
     * FenetreDemarrage : son main de montage garde sa sortie (logs + erreurs de
     * demarrage sur stdout/stderr) -> exclusion `&& !declaresService`. */
    bool projetIsGui = false;
    {
        /* project_discover laisse entryIndex=-1 ; le programme = le top-level de
         * projet.owl (meme choix que le bundle genere, _entryIndex). */
        project.entryIndex = buildEntry;
        vm_init();  /* peuple le registre console (natives UI+PDF incluses) */
        InterpretResult chk = project_run_ex(&project, false, /*compileOnly=*/true);
        /* Le portail compile toute la frontiere en phase 1 : les blocs
         * `<Nom> est un ServiceREST` ont peuple owl_service_registry. Non vide =>
         * projet serveur. Registre encore vivant (reset plus bas). */
        declaresService = (owl_service_registry_count() > 0);
        const char* startupWin = compiler_projet_designated_startup_window();
        projetIsGui = (startupWin != NULL && startupWin[0] != '\0') && !declaresService;
        vm_free();
        compiler_reset_poo_state();  /* libere le registre compile-time du portail */
        /* Portail limite aux erreurs de COMPILATION (scope du ticket : "projet
         * qui ne compile pas"). Les diagnostics sont deja emis sur stderr par le
         * compilateur. Une erreur runtime de phase 1 (init de declaration) n'est
         * PAS bloquee ici : elle peut dependre de l'environnement d'execution
         * (absent au build) et sort du scope de ce garde. */
        if (chk == INTERPRET_COMPILE_ERROR) {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl build: the project does not compile, no executable generated.\n"
                : "owl build: le projet ne compile pas, aucun executable genere.\n");
            project_free(&project);
            return 65;
        }
    }

    /* 2. Generate _owl_bundle.c */
    const char* bundlePath = "_owl_bundle.c";
    FILE* out = fopen(bundlePath, "w");
    if (out == NULL) {
        fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl build: cannot create '%s': %s\n"
                : "owl build: creation de '%s' impossible : %s\n",
                bundlePath, strerror(errno));
        project_free(&project);
        return 65;
    }

    /* Count non-test files */
    int buildCount = 0;
    for (int i = 0; i < project.fileCount; i++) {
        if (!project.files[i].isTest) buildCount++;
    }

    fprintf(out, "/* Auto-generated by owl build -- do not edit. */\n");
    fprintf(out, "#include \"common.h\"\n");
    fprintf(out, "#include \"vm.h\"\n");
    fprintf(out, "#include \"project.h\"\n");
    fprintf(out, "#include \"object.h\"\n");
    fprintf(out, "#include \"table.h\"\n");
    /* owl_set_warnings_suppressed : le main genere silence totalement les
     * warnings du recompile de demarrage (source deja validee au packaging). */
    fprintf(out, "#include \"compiler.h\"\n");
    fprintf(out, "#include <stdlib.h>\n");
    fprintf(out, "#include <string.h>\n");
    /*  — projet serveur : le main de montage appelle l'API serve
     * (prepare/demarre/attend/libere). Le header est copié dans include/owl/
     * par owl_distrib (self-contained : stdbool/stddef/stdio). */
    if (declaresService) {
        fprintf(out, "#include \"owl_serve.h\"\n");
    }
    fprintf(out, "\n");

    /* File names (kept clear so error messages stay useful) */
    fprintf(out, "static const char* _names[] = {\n");
    for (int i = 0; i < project.fileCount; i++) {
        if (project.files[i].isTest) continue;
        fprintf(out, "    \"%s\",\n", project.files[i].name);
    }
    fprintf(out, "    NULL\n};\n\n");

    /* Index de l'entree (projet.owl) dans le tableau _names filtre (hors
     * tests).  : le top-level de projet.owl est le programme. */
    int bundleEntryFiltered = 0;
    for (int i = 0; i < buildEntry; i++) {
        if (!project.files[i].isTest) bundleEntryFiltered++;
    }
    fprintf(out, "static const int _entryIndex = %d;\n\n", bundleEntryFiltered);

    /* Source files emitted as raw-deflate blobs (no zlib header), NOT plain C
     * strings. Purpose : keep the .owl text out of `strings` output. The
     * compression alone defeats the trivial `strings` scrape while staying
     * heuristic-clean — the generated main just reuses tinfl (already linked in
     * every produced exe for the compression natives) to inflate each blob back
     * to source at startup. There is NO decryption step to reuse.
     *
     * DELIBERATELY plain deflate : NO XOR, NO cipher, NO key. A static XOR pack
     * was tried once (see git history) and tripped Windows Defender heuristics
     * because a decrypt-at-runtime pattern reads as a malware signature. Without
     * a code-signing certificate nothing covers that false positive, so do NOT
     * re-introduce XOR or any cipher here : it buys obscurity we cannot afford.
     * Deflate is a mundane operation that already powers legitimate features. */
    {
        /* Accumule les arrays _blob_lens / _orig_lens en texte pendant qu'on
         * emet les blobs, puis on les ecrit apres. */
        char* lensBuf = NULL; size_t lensCap = 0, lensLen = 0;
        char* origsBuf = NULL; size_t origsCap = 0, origsLen = 0;
        int fileIdx = 0;
        for (int i = 0; i < project.fileCount; i++) {
            if (project.files[i].isTest) continue;
            const char* src = project.files[i].source;
            size_t srcLen = strlen(src);
            mz_uint flags = tdefl_create_comp_flags_from_zip_params(9, -15, 0);
            size_t compLen = 0;
            void* comp = tdefl_compress_mem_to_heap(src, srcLen, &compLen, flags);
            if (comp == NULL) {
                fprintf(stderr, owl_error_render_lang_is_en()
                        ? "owl build: deflate failed for '%s'\n"
                        : "owl build: echec deflate pour '%s'\n",
                        project.files[i].name);
                free(lensBuf); free(origsBuf);
                fclose(out);
                project_free(&project);
                return 65;
            }
            fprintf(out, "static const unsigned char _blob_%d[] = {", fileIdx);
            const unsigned char* cp = (const unsigned char*)comp;
            for (size_t b = 0; b < compLen; b++) {
                if (b % 20 == 0) fprintf(out, "\n    ");
                fprintf(out, "%u,", (unsigned)cp[b]);
            }
            fprintf(out, "\n};\n");
            free(comp);

            /* append len entries (grow buffers as needed) */
            char tmp[64];
            int n1 = snprintf(tmp, sizeof(tmp), "%s%zu", fileIdx ? ", " : "", compLen);
            if (lensLen + (size_t)n1 + 1 > lensCap) {
                lensCap = (lensCap ? lensCap * 2 : 256) + (size_t)n1;
                lensBuf = realloc(lensBuf, lensCap);
            }
            memcpy(lensBuf + lensLen, tmp, (size_t)n1 + 1); lensLen += (size_t)n1;

            int n2 = snprintf(tmp, sizeof(tmp), "%s%zu", fileIdx ? ", " : "", srcLen);
            if (origsLen + (size_t)n2 + 1 > origsCap) {
                origsCap = (origsCap ? origsCap * 2 : 256) + (size_t)n2;
                origsBuf = realloc(origsBuf, origsCap);
            }
            memcpy(origsBuf + origsLen, tmp, (size_t)n2 + 1); origsLen += (size_t)n2;

            fileIdx++;
        }
        fprintf(out, "\nstatic const unsigned char* const _blobs[] = {\n    ");
        for (int k = 0; k < fileIdx; k++) fprintf(out, "_blob_%d, ", k);
        fprintf(out, "NULL\n};\n");
        fprintf(out, "static const unsigned int _blob_lens[] = { %s };\n",
                lensBuf ? lensBuf : "");
        fprintf(out, "static const unsigned int _orig_lens[] = { %s };\n\n",
                origsBuf ? origsBuf : "");
        free(lensBuf);
        free(origsBuf);
    }

    /* tinfl est fourni par miniz, deja linke dans tout exe produit (natives de
     * compression). On forward-declare le seul symbole requis plutot que
     * d'inclure miniz.h, absent de include/owl (ou compile le bundle). */
    fprintf(out,
        "extern void *tinfl_decompress_mem_to_heap(const void *pSrc_buf,\n"
        "    size_t src_buf_len, size_t *pOut_len, int flags);\n\n");

    /* Chrome console de l'exe produit :
     *  - owl_force_utf8_console (platform.c, lie dans tout exe produit) : la
     *    console affiche correctement les accents des messages de demarrage.
     *  - owl_set_warnings_suppressed : les avertissements du compilateur emis
     *    pendant le recompile de demarrage (la source est deja compile-checkee
     *    au packaging) sont totalement silencieux (garantie dure, independante
     *    de toute capacite de buffer), pas de pollution de la console du
     *    client. Les ERREURS, elles, continuent de sortir sur stderr et
     *    stoppent le demarrage. */
    fprintf(out,
        "void owl_force_utf8_console(void);\n\n");

    /*  — ouverture de la fenetre de demarrage (projet GUI). Vit dans
     * owl_core (Qt-free, resout Ouvre via vm.globals a l'execution) ; forward-
     * declaree ici comme owl_force_utf8_console plutot que via #include pour ne
     * pas dependre de la distribution du header. Non emise en mode serveur (le
     * montage serve bloque deja, pas de fenetre de demarrage). */
    if (!declaresService) {
        fprintf(out,
            "int owl_run_startup_window(const char* fenName);\n\n");
    }

    /*  — projet fenetre lie en sous-systeme windows : rattache la
     * console parente si l'exe est lance depuis un terminal, pour que Trace
     * (printf) reste visible. Lancement par double-clic : no-op (pas de console
     * parente), Trace tombe dans le vide (comportement GUI voulu). Vit dans
     * owl_core (Win32-garde, no-op ailleurs) ; forward-declaree comme ses
     * voisines. Emise uniquement en build GUI : un exe console a deja sa
     * console. */
    if (projetIsGui) {
        fprintf(out,
            "void owl_attach_parent_console_if_any(void);\n\n");
    }

    if (declaresService) {
        /*  — main de montage SERVEUR. Le projet declare un ServiceREST :
         * au lieu d'un project_run() one-shot (qui finirait le top-level et
         * sortirait sans servir), on nourrit les sources embarquees (inflatees) au
         * montage serve — exactement comme `owl serve` : owl_serve_prepare_projet
         * (compile + bind SYNCHRONE) -> owl_serve_demarre (executeur owning-VM sur
         * SON thread + pool de transport) -> owl_serve_attend (bloque jusqu'a
         * Ctrl-C). Le main NE fait AUCUN vm_init : la VM est possedee par le thread
         * executeur du montage (, VM mono-thread). Config a l'EXECUTION
         * de l'exe produit : `<exe> [--port N] [--bind ADR]`, build
         * une fois / deploie partout. Defauts surs 127.0.0.1:8080 (localhost) ;
         * --bind 0.0.0.0 ouvre explicitement au reseau. */
        fprintf(out,
            "extern void owl_tcp_server_installe_arret(void);\n\n"
            "static void _serve_usage(const char* exe, FILE* o) {\n"
            "    fprintf(o, \"Usage: %%s [--port N] [--bind ADR]\\n\", exe);\n"
            "    fprintf(o, \"\\n\");\n"
            "    fprintf(o, \"Starts the REST service. All options are optional.\\n\");\n"
            "    fprintf(o, \"\\n\");\n"
            "    fprintf(o, \"  --port N      Port to listen on (default: 8080).\\n\");\n"
            "    fprintf(o, \"  --bind ADR    Address to bind (default: 127.0.0.1).\\n\");\n"
            "    fprintf(o, \"                Use --bind 0.0.0.0 to accept connections from the network.\\n\");\n"
            "    fprintf(o, \"  -h, --help    Show this help and exit.\\n\");\n"
            "    fprintf(o, \"\\n\");\n"
            "    fprintf(o, \"Press Ctrl-C to stop the server.\\n\");\n"
            "}\n"
            "static int _serve_parse_port(const char* s, int* out) {\n"
            "    if (s == NULL || *s == '\\0') return 0;\n"
            "    char* end = NULL;\n"
            "    long v = strtol(s, &end, 10);\n"
            "    if (end == s || *end != '\\0') return 0;\n"
            "    if (v < 1 || v > 65535) return 0;\n"
            "    *out = (int)v;\n"
            "    return 1;\n"
            "}\n\n"
            "int main(int argc, char* argv[]) {\n"
            "    owl_force_utf8_console();\n"
            "    const char* bind_adr = \"127.0.0.1\";\n"
            "    int port = 8080;\n"
            "    for (int ai = 1; ai < argc; ai++) {\n"
            "        const char* a = argv[ai];\n"
            "        if (strcmp(a, \"-h\") == 0 || strcmp(a, \"--help\") == 0) {\n"
            "            _serve_usage(argv[0], stdout);\n"
            "            return 0;\n"
            "        } else if (strcmp(a, \"--port\") == 0) {\n"
            "            if (ai + 1 >= argc) {\n"
            "                fprintf(stderr, \"%%s: --port requires a value\\n\", argv[0]);\n"
            "                _serve_usage(argv[0], stderr);\n"
            "                return 2;\n"
            "            }\n"
            "            if (!_serve_parse_port(argv[++ai], &port)) {\n"
            "                fprintf(stderr, \"%%s: invalid port '%%s' (expected 1..65535)\\n\", argv[0], argv[ai]);\n"
            "                return 2;\n"
            "            }\n"
            "        } else if (strncmp(a, \"--port=\", 7) == 0) {\n"
            "            if (!_serve_parse_port(a + 7, &port)) {\n"
            "                fprintf(stderr, \"%%s: invalid port '%%s' (expected 1..65535)\\n\", argv[0], a + 7);\n"
            "                return 2;\n"
            "            }\n"
            "        } else if (strcmp(a, \"--bind\") == 0) {\n"
            "            if (ai + 1 >= argc) {\n"
            "                fprintf(stderr, \"%%s: --bind requires a value\\n\", argv[0]);\n"
            "                _serve_usage(argv[0], stderr);\n"
            "                return 2;\n"
            "            }\n"
            "            bind_adr = argv[++ai];\n"
            "        } else if (strncmp(a, \"--bind=\", 7) == 0) {\n"
            "            bind_adr = a + 7;\n"
            "        } else {\n"
            "            fprintf(stderr, \"%%s: unknown argument '%%s'\\n\", argv[0], a);\n"
            "            _serve_usage(argv[0], stderr);\n"
            "            return 2;\n"
            "        }\n"
            "    }\n"
            "    int count = 0;\n"
            "    while (_names[count]) count++;\n"
            "    const char** sources = (const char**)malloc(sizeof(char*) * (size_t)count);\n"
            "    if (sources == NULL) {\n"
            "        fprintf(stderr, \"owl: out of memory\\n\");\n"
            "        return 70;\n"
            "    }\n"
            "    for (int i = 0; i < count; i++) {\n"
            "        size_t outLen = 0;\n"
            "        void* plain = tinfl_decompress_mem_to_heap(_blobs[i],\n"
            "            (size_t)_blob_lens[i], &outLen, 0 /* raw deflate */);\n"
            "        if (plain == NULL || outLen != (size_t)_orig_lens[i]) {\n"
            "            fprintf(stderr, \"owl: corrupted bundle (inflate failed)\\n\");\n"
            "            free(plain);\n"
            "            return 70;\n"
            "        }\n"
            "        char* s = (char*)malloc(outLen + 1);\n"
            "        memcpy(s, plain, outLen);\n"
            "        s[outLen] = '\\0';\n"
            "        free(plain);\n"
            "        sources[i] = s;\n"
            "    }\n"
            "    OwlServeProjet projet;\n"
            "    projet.noms = _names;\n"
            "    projet.chemins = _names;\n"
            "    projet.sources = sources;\n"
            "    projet.nb = count;\n"
            "    projet.entree = _entryIndex;\n"
            "    projet.mode_projet = 1;\n"
            "    OwlServeOptions opts;\n"
            "    opts.bind_adr = bind_adr;\n"
            "    opts.port = port;\n"
            "    char err[256];\n"
            "    err[0] = '\\0';\n"
            "    owl_set_warnings_suppressed(1);\n"
            "    OwlServeMontage* m = owl_serve_prepare_projet(&projet, &opts, err, sizeof(err));\n"
            "    if (m == NULL) {\n"
            "        fprintf(stderr, \"%%s\\n\", err[0] ? err : \"owl: server mount failed\");\n"
            "        return 1;\n"
            "    }\n"
            "    owl_tcp_server_installe_arret();\n"
            "    if (!owl_serve_demarre(m, 0, stdout)) {\n"
            "        fprintf(stderr, \"owl: server start failed\\n\");\n"
            "        owl_serve_libere(m);\n"
            "        return 1;\n"
            "    }\n"
            "    owl_serve_attend(m);\n"
            "    owl_serve_libere(m);\n"
            "    return 0;\n"
            "}\n");
    } else {
        fprintf(out,
            "int main(void) {\n");
        /*  — build GUI (sous-systeme windows) : rattache la console
         * parente au tout debut, avant tout output, pour router Trace vers le
         * terminal lanceur le cas echeant. Non emise en build console. */
        if (projetIsGui) {
            fprintf(out,
            "    owl_attach_parent_console_if_any();\n");
        }
        fprintf(out,
            "    owl_force_utf8_console();\n"
            "    vm_init();\n"
            "    Project project;\n"
            "    project_init(&project);\n"
            "    project.rootDir = strdup(\".\");\n"
            "    int count = 0;\n"
            "    while (_names[count]) count++;\n"
            "    project.files = (ProjectFile*)malloc(sizeof(ProjectFile) * count);\n"
            "    project.fileCapacity = count;\n"
            "    project.fileCount = count;\n"
            "    for (int i = 0; i < count; i++) {\n"
            "        project.files[i].name = strdup(_names[i]);\n"
            "        project.files[i].path = strdup(_names[i]);\n"
            "        size_t outLen = 0;\n"
            "        void* plain = tinfl_decompress_mem_to_heap(_blobs[i],\n"
            "            (size_t)_blob_lens[i], &outLen, 0 /* raw deflate */);\n"
            "        if (plain == NULL || outLen != (size_t)_orig_lens[i]) {\n"
            "            fprintf(stderr, \"owl: corrupted bundle (inflate failed)\\n\");\n"
            "            free(plain);\n"
            "            return 70;\n"
            "        }\n"
            "        char* s = (char*)malloc(outLen + 1);\n"
            "        memcpy(s, plain, outLen);\n"
            "        s[outLen] = '\\0';\n"
            "        free(plain);\n"
            "        project.files[i].source = s;\n"
            "        project.files[i].isTest = 0;\n"
            "    }\n"
            "    project.entryIndex = _entryIndex;\n"
            "    project.isProjectMode = true;\n"
            "    owl_set_warnings_suppressed(1);\n"
            "    InterpretResult result = project_run(&project);\n"
            /*  — symetrie avec run_project_scoped (main.c) : apres le
             * body projet, ouvre la FenetreDemarrage (metadonnee typee peuplee
             * par le compile de projet.owl) et entre dans l'event loop. La
             * designation est un const char* du chunk -> copie AVANT project_free
             * et avant toute alloc (owl_run_startup_window alloue). L'ouverture
             * utilise la VM -> AVANT vm_free. Projet sans FenetreDemarrage :
             * _startupWin vide -> run-once puis sortie, pas d'event loop fantome. */
            "    char _startupWin[128];\n"
            "    _startupWin[0] = '\\0';\n"
            "    if (result == INTERPRET_OK) {\n"
            "        const char* sw = compiler_projet_designated_startup_window();\n"
            "        if (sw != NULL && sw[0] != '\\0') {\n"
            "            strncpy(_startupWin, sw, sizeof(_startupWin) - 1);\n"
            "            _startupWin[sizeof(_startupWin) - 1] = '\\0';\n"
            "        }\n"
            "    }\n"
            "    project_free(&project);\n"
            "    int rc = 0;\n"
            "    if (result == INTERPRET_COMPILE_ERROR) rc = 65;\n"
            "    else if (result == INTERPRET_RUNTIME_ERROR) rc = 70;\n"
            "    else if (_startupWin[0] != '\\0') rc = owl_run_startup_window(_startupWin);\n"
            "    vm_free();\n"
            "    return rc;\n"
            "}\n");
    }

    fclose(out);

    /* Le source embarque est toujours compresse (deflate, cf. generation du
     * bundle ci-dessus) — message unique et honnete. */
    printf(owl_error_render_lang_is_en()
        ? "owl build: %d file(s) bundled (compressed source)\n"
        : "owl build: %d fichier(s) empaquete(s) (source compressee)\n", buildCount);

    /* 3. Build link command — full GUI (Qt6 statique + plugins).
     * Lit lib/owl_link_gui.cfg genere au build OWL par
     * scripts/extract_link_spec.py. Decision Phase B : tout binaire produit
     * par owl-build est GUI par defaut (mode console-only = owl-cli, dev). */
    char* owlRoot = safe_strdup(owlInstallRoot);
    size_t rL = strlen(owlRoot);
    /* Target par defaut = host. */
    const char* effTarget = target;
    if (effTarget == NULL || *effTarget == '\0') {
#ifdef _WIN32
        effTarget = "windows-x86_64";
#elif defined(__APPLE__)
        effTarget = "macos-x86_64";
#else
        effTarget = "linux-x86_64";
#endif
    }
    size_t tL = strlen(effTarget);
    char* incDir = malloc(rL + 32);
    char* libDir = malloc(rL + tL + 32);
    char* cfgPath = malloc(rL + tL + 64);
    sprintf(incDir, "%s/include/owl", owlRoot);
    sprintf(libDir, "%s/lib/%s",         owlRoot, effTarget);
    sprintf(cfgPath, "%s/lib/%s/owl_link_gui.cfg", owlRoot, effTarget);

    /* Localise zig vendore (Phase B.4). Cherche vendor/zig/<host>/zig
     * a partir de owlRoot (dev = <repo>/build/, release = <install>/).
     * Fallback : gcc/c++ via PATH (compatible avec le mode pre-Phase-B). */
#ifdef _WIN32
    const char* zigHost = "x86_64-windows";
    const char* zigExeName = "zig.exe";
#elif defined(__APPLE__)
    const char* zigHost = (sizeof(void*) == 8) ? "aarch64-macos" : "x86_64-macos";
    const char* zigExeName = "zig";
#else
    const char* zigHost = "x86_64-linux";
    const char* zigExeName = "zig";
#endif
    char zigBin[1024]; zigBin[0] = '\0';
    {
        char probe[1024];
        const char* candidates[] = {
            "%s/zig/%s/%s",               /* dist install : <owl-build>/zig/<host>/ */
            "%s/../vendor/zig/%s/%s",     /* dev : <repo>/build/, vendor at ../ */
            "%s/vendor/zig/%s/%s",        /* layout alternatif : <install>/vendor/zig/ */
            NULL
        };
        for (int i = 0; candidates[i]; i++) {
            snprintf(probe, sizeof(probe), candidates[i], owlRoot, zigHost, zigExeName);
            if (file_exists(probe)) {
                snprintf(zigBin, sizeof(zigBin), "%s", probe);
                break;
            }
        }
    }
    /* useZig ON par defaut si le binaire zig est trouve. Permet a la
     * distribution dist/owl-build-<host>/ de fonctionner sans env var.
     * Forcer OFF via OWL_USE_ZIG=0 pour fallback gcc/c++ via PATH (utile
     * pour debug). */
    const char* useZigEnv = getenv("OWL_USE_ZIG");
    bool useZig = (zigBin[0] != '\0');
    if (useZigEnv && strcmp(useZigEnv, "0") == 0) useZig = false;
#ifdef _WIN32
    /* cmd.exe ne tolere pas les chemins mixtes / + \, ni les paths cites
     * commencant par '"' (system() les corrompt). Resolu en path absolu
     * canonique via _fullpath (resoud les '..'), avec backslashes. */
    if (useZig) {
        char fullpath[1024];
        if (_fullpath(fullpath, zigBin, sizeof(fullpath)) != NULL) {
            snprintf(zigBin, sizeof(zigBin), "%s", fullpath);
        }
        for (char* p = zigBin; *p; p++) if (*p == '/') *p = '\\';
    }
#endif
    if (useZig) {
        printf("owl build: toolchain = zig cc (%s)\n", zigBin);
    } else if (zigBin[0] != '\0') {
        printf(owl_error_render_lang_is_en()
               ? "owl build: toolchain = gcc/c++ via PATH (zig available "
                 "but OWL_USE_ZIG!=1, default until Qt6 is rebuilt under "
                 "zig cc)\n"
               : "owl build: toolchain = gcc/c++ via PATH (zig dispo "
                 "mais OWL_USE_ZIG!=1, par defaut tant que Qt6 n'est pas "
                 "rebuilde sous zig cc)\n");
    } else {
        printf(owl_error_render_lang_is_en()
               ? "owl build: toolchain = gcc/c++ via PATH (zig not vendored)\n"
               : "owl build: toolchain = gcc/c++ via PATH (zig non vendore)\n");
    }

    /* Lit owl_link_gui.cfg : un fichier key=value. */
    char* link_flags = NULL;
    char* objects = NULL;
    char* libs_rel = NULL;
    char* libs_abs = NULL;
    char* libs_names = NULL;
    char* qt6_root_default = NULL;
    char* duckdb_root_default = NULL;
    char* llama_root_default = NULL;
    {
        FILE* cf = fopen(cfgPath, "r");
        if (cf == NULL) {
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "owl build: cannot read %s : %s\n"
                : "owl build: lecture de %s impossible : %s\n",
                cfgPath, strerror(errno));
            fprintf(stderr, owl_error_render_lang_is_en()
                ? "  This file is generated when OWL is built.\n"
                : "  Le fichier est genere au build OWL.\n");
            free(owlRoot); free(incDir); free(libDir); free(cfgPath);
            return 65;
        }
        char buf[16384];
        while (fgets(buf, sizeof(buf), cf)) {
            if (buf[0] == '#' || buf[0] == '\n') continue;
            char* eq = strchr(buf, '=');
            if (!eq) continue;
            *eq = '\0';
            char* val = eq + 1;
            size_t vlen = strlen(val);
            /* Strip trailing \r\n et \n (cfg generes en CRLF cote Windows). */
            while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) {
                val[--vlen] = '\0';
            }
            char** field = NULL;
            if      (strcmp(buf, "LINK_FLAGS") == 0)       field = &link_flags;
            else if (strcmp(buf, "OBJECTS")    == 0)       field = &objects;
            else if (strcmp(buf, "LIBS_REL")   == 0)       field = &libs_rel;
            else if (strcmp(buf, "LIBS_ABS")   == 0)       field = &libs_abs;
            else if (strcmp(buf, "LIBS_NAMES") == 0)       field = &libs_names;
            else if (strcmp(buf, "QT6_ROOT_DEFAULT") == 0) field = &qt6_root_default;
            else if (strcmp(buf, "DUCKDB_ROOT_DEFAULT") == 0) field = &duckdb_root_default;
            else if (strcmp(buf, "LLAMA_ROOT_DEFAULT") == 0) field = &llama_root_default;
            if (field) *field = safe_strdup(val);
        }
        fclose(cf);
    }

    /* B.5 : resout @QT6_ROOT@ / @DUCKDB_ROOT@ / @LLAMA_ROOT@ dans OBJECTS/LIBS_ABS.
     * Chaque root, par priorite :
     *   1. OWL_<NAME>_ROOT_<TARGET> env (ex OWL_QT6_ROOT_WINDOWS_X86_64)
     *   2. OWL_<NAME>_ROOT env (toutes plateformes)
     *   3. <owlRoot>/<subdir>/<target>/ (layout d'extraction du mega)
     *   4. <NAME>_ROOT_DEFAULT du cfg (mode dev = path absolu d'origine)
     * DuckDB + llama sont stages par package_dist et relocalises ici : sans ca
     * leurs .a seraient references par le chemin d'arbre de build (owl-build
     * casse hors machine de build). */
    {
        struct { const char* ph; const char* name; const char* subdir; const char* def; char root[1024]; } roots[] = {
            { "@QT6_ROOT@",    "QT6",    "qt6",    qt6_root_default,    { 0 } },
            { "@DUCKDB_ROOT@", "DUCKDB", "duckdb", duckdb_root_default, { 0 } },
            { "@LLAMA_ROOT@",  "LLAMA",  "llama",  llama_root_default,  { 0 } },
        };
        int nroots = (int)(sizeof(roots) / sizeof(roots[0]));
        for (int ri = 0; ri < nroots; ri++) {
            char envVar[96];
            snprintf(envVar, sizeof(envVar), "OWL_%s_ROOT_%s", roots[ri].name, effTarget);
            for (char* p = envVar; *p; p++) { if (*p == '-') *p = '_'; *p = (char)toupper((unsigned char)*p); }
            char envGenName[64];
            snprintf(envGenName, sizeof(envGenName), "OWL_%s_ROOT", roots[ri].name);
            const char* envSpec = getenv(envVar);
            const char* envGen  = getenv(envGenName);
            if (envSpec && *envSpec) {
                snprintf(roots[ri].root, sizeof(roots[ri].root), "%s", envSpec);
            } else if (envGen && *envGen) {
                snprintf(roots[ri].root, sizeof(roots[ri].root), "%s", envGen);
            } else {
                char probe[1024];
                snprintf(probe, sizeof(probe), "%s/%s/%s", owlRoot, roots[ri].subdir, effTarget);
                if (is_directory(probe)) {
                    snprintf(roots[ri].root, sizeof(roots[ri].root), "%s", probe);
                } else if (roots[ri].def && *roots[ri].def) {
                    snprintf(roots[ri].root, sizeof(roots[ri].root), "%s", roots[ri].def);
                }
            }
            if (roots[ri].root[0]) printf("owl build: %s_root = %s\n", roots[ri].subdir, roots[ri].root);
        }
        /* Substitue les 3 placeholders dans objects + libs_abs (un seul scan). */
        char** ptrs[] = { &objects, &libs_abs };
        for (int fi = 0; fi < 2; fi++) {
            const char* src = *ptrs[fi];
            if (!src) continue;
            size_t outCap = strlen(src) * 6 + 1024;
            char* out = malloc(outCap);
            if (!out) continue;
            size_t oi = 0;
            const char* p = src;
            while (*p && oi + 1 < outCap) {
                int matched = 0;
                for (int ri = 0; ri < nroots; ri++) {
                    size_t phl = strlen(roots[ri].ph);
                    if (strncmp(p, roots[ri].ph, phl) == 0) {
                        size_t rl = strlen(roots[ri].root);
                        if (oi + rl < outCap) { memcpy(out + oi, roots[ri].root, rl); oi += rl; }
                        p += phl;
                        matched = 1;
                        break;
                    }
                }
                if (!matched) out[oi++] = *p++;
            }
            out[oi] = '\0';
            free(*ptrs[fi]);
            *ptrs[fi] = out;
        }
    }

    /* Convertit le target triple OWL en target zig.
     * Linux : musl-static. Les libs OWL + Qt6 + deps sont bâties en
     * x86_64-linux-musl (binaire autonome, zéro dépendance glibc) ; le target
     * du bundle DOIT matcher cet ABI, sinon link cassé (mélange glibc/musl).
     * Le cfg linux porte lui aussi -target x86_64-linux-musl dans LINK_FLAGS. */
    char zigTarget[64];
    if (strstr(effTarget, "windows")) {
        snprintf(zigTarget, sizeof(zigTarget), "x86_64-windows-gnu");
    } else if (strstr(effTarget, "macos")) {
        if (strstr(effTarget, "aarch64")) {
            snprintf(zigTarget, sizeof(zigTarget), "aarch64-macos-none");
        } else {
            snprintf(zigTarget, sizeof(zigTarget), "x86_64-macos-none");
        }
    } else {
        snprintf(zigTarget, sizeof(zigTarget), "x86_64-linux-musl");
    }

    /* Etape A : compile le bundle.c en .obj. zig cc en mode C ou gcc
     * via PATH selon useZig. Le link ulterieur via c++ se fera sur le
     * .obj C qui a des symboles non-mangled, compatibles avec les .a
     * OWL en C. */
    const char* bundleObj = "_owl_bundle.c.obj";
    char compileCmd[2048];
    if (useZig) {
#ifdef _WIN32
        snprintf(compileCmd, sizeof(compileCmd),
            "\"\"%s\" cc -target %s -std=gnu99 -O2 -fno-strict-aliasing -DLIBXML_STATIC "
            "-I\"%s\" -c \"%s\" -o \"%s\"\"",
            zigBin, zigTarget, incDir, bundlePath, bundleObj);
#else
        snprintf(compileCmd, sizeof(compileCmd),
            "\"%s\" cc -target %s -std=gnu99 -O2 -fno-strict-aliasing -DLIBXML_STATIC "
            "-I\"%s\" -c \"%s\" -o \"%s\"",
            zigBin, zigTarget, incDir, bundlePath, bundleObj);
#endif
    } else {
        snprintf(compileCmd, sizeof(compileCmd),
            "gcc -std=gnu99 -O2 -fno-strict-aliasing -DLIBXML_STATIC "
            "-I\"%s\" -c \"%s\" -o \"%s\"",
            incDir, bundlePath, bundleObj);
    }
    printf(owl_error_render_lang_is_en()
        ? "owl build: compiling bundle...\n"
        : "owl build: compilation du bundle...\n");
    int rcCompile = system(compileCmd);
    if (rcCompile != 0) {
        fprintf(stderr, owl_error_render_lang_is_en()
            ? "owl build: bundle compile failed (exit %d)\n"
            : "owl build: echec de compilation du bundle (code %d)\n", rcCompile);
        free(owlRoot); free(incDir); free(libDir); free(cfgPath);
        free(link_flags); free(objects); free(libs_rel); free(libs_abs); free(libs_names); free(qt6_root_default); free(duckdb_root_default); free(llama_root_default);
        return 65;
    }

    /* Etape B : link le bundle.obj + tout le reste via c++ (Qt6 statique
     * exige libstdc++). Response file (cmd.exe 8KB limit). */
    const char* rspPath = "_owl_build.rsp";
    FILE* rsp = fopen(rspPath, "w");
    if (rsp == NULL) {
        fprintf(stderr, owl_error_render_lang_is_en()
            ? "owl build: cannot create response file: %s\n"
            : "owl build: creation du fichier reponse impossible : %s\n", strerror(errno));
        free(owlRoot); free(incDir); free(libDir); free(cfgPath);
        return 65;
    }

    /* Le .obj du bundle vient en premier. */
    fprintf(rsp, "\"%s\"\n", bundleObj);

    /* Si zig : ajoute -L vendor/zig/winapi_stubs pour les libs Windows
     * (synchronization, runtimeobject) absentes du sysroot zig. */
    if (useZig) {
        char stubsDir[1024];
        const char* candidates2[] = {
            "%s/zig/winapi_stubs",         /* dist install */
            "%s/../vendor/zig/winapi_stubs", /* dev */
            "%s/vendor/zig/winapi_stubs",
            NULL
        };
        for (int i = 0; candidates2[i]; i++) {
            snprintf(stubsDir, sizeof(stubsDir), candidates2[i], owlRoot);
            if (is_directory(stubsDir)) {
                fprintf(rsp, "-L\"%s\"\n", stubsDir);
                break;
            }
        }
    }

    /* Sur Linux target, lld zig est strict sur les .so undefined symbols
     * (libxcb-cursor.so refere des symboles non resolus). Tolere via
     * --allow-shlib-undefined. */
    if (strstr(effTarget, "linux")) {
        fprintf(rsp, "-Wl,--allow-shlib-undefined\n");
    }

    /* LINK_FLAGS du .cfg (-static -static-libgcc -static-libstdc++ ...).
     * Si zig (lld), traduit -Wl,-subsystem,X -> -Wl,--subsystem,X (lld
     * syntax vs GNU ld). */
    if (link_flags) {
        /*  — projet fenetre : bascule le sous-systeme console (herite de
         * owl.exe, l'outil CLI) en windows pour que le double-clic n'ouvre pas de
         * console residuelle. "console" et "windows" font 7 caracteres : remplace
         * en place. La traduction lld plus bas (-Wl,-subsystem,X ->
         * -Wl,--subsystem,X) s'applique ensuite sans changement. Projet non
         * fenetre (CLI / ServiceREST) : rien ne change, reste console. */
        if (projetIsGui) {
            char* p = strstr(link_flags, "-Wl,-subsystem,console");
            if (p != NULL) memcpy(p + strlen("-Wl,-subsystem,"), "windows", 7);
        }
        if (useZig) {
            char* p = link_flags;
            while ((p = strstr(p, "-Wl,-subsystem,")) != NULL) {
                /* Insere un '-' apres le 'l,' pour transformer en
                 * '-Wl,--subsystem,'. Methode in-place : on shift d'un
                 * caractere. */
                char buf[8192];
                size_t prefix_len = (size_t)(p - link_flags) + 4; /* "-Wl," */
                size_t link_flags_len = strlen(link_flags);
                if (prefix_len + (link_flags_len - prefix_len) + 1 < sizeof(buf)) {
                    memcpy(buf, link_flags, prefix_len);
                    buf[prefix_len] = '-';
                    strcpy(buf + prefix_len + 1, link_flags + prefix_len);
                    free(link_flags);
                    link_flags = safe_strdup(buf);
                    p = link_flags + prefix_len + 1;
                } else {
                    break;
                }
            }
        }
        fprintf(rsp, "%s\n", link_flags);
    }

    /* Plugin .obj precompiles (Qt6 statique : QWindowsIntegrationPlugin
     * etc., qrc resources). */
    if (objects) fprintf(rsp, "%s\n", objects);

    /* Libs OWL (relatives a libDir) : libowl_runtime_gui.a en tete +
     * owl_core + vendor. Préfixe par -L<libDir> et utilise les noms tels
     * quels (ld trouve via -L). */
    fprintf(rsp, "-L\"%s\"\n", libDir);
    if (libs_rel) {
        /* Wrap dans --start-group/--end-group pour resoudre les deps
         * croisees (owl_runtime_gui <-> owl_core, libxml2 internes). */
        fprintf(rsp, "-Wl,--start-group ");
        char* tok = strtok(libs_rel, " \t");
        while (tok) {
            /* Convertit libowl_X.a en -lowl_X (le -L<libDir> resout). */
            const char* base = tok;
            const char* slash = strrchr(tok, '/');
            if (slash) base = slash + 1;
            const char* bslash = strrchr(base, '\\');
            if (bslash) base = bslash + 1;
            if (strncmp(base, "lib", 3) == 0) {
                /* libNAME.a -> -lNAME */
                char nm[256];
                size_t baselen = strlen(base);
                if (baselen > 5 && strcmp(base + baselen - 2, ".a") == 0) {
                    size_t nmlen = baselen - 5;
                    if (nmlen >= sizeof(nm)) nmlen = sizeof(nm) - 1;
                    memcpy(nm, base + 3, nmlen);
                    nm[nmlen] = '\0';
                    fprintf(rsp, "-l%s ", nm);
                } else {
                    fprintf(rsp, "\"%s\" ", tok);
                }
            } else {
                fprintf(rsp, "\"%s\" ", tok);
            }
            tok = strtok(NULL, " \t");
        }
        fprintf(rsp, "-Wl,--end-group\n");
    }

    /* Libs absolues (Qt6, etc.). */
    if (libs_abs) {
        char* tok = strtok(libs_abs, " \t");
        while (tok) {
            fprintf(rsp, "\"%s\" ", tok);
            tok = strtok(NULL, " \t");
        }
        fprintf(rsp, "\n");
    }

    /* Libs system (-lm, -lbcrypt, Win API). */
    if (libs_names) fprintf(rsp, "%s\n", libs_names);

    fprintf(rsp, "-o \"%s\"\n", outputName);
    fclose(rsp);

    /*  — materialise le dossier de sortie MAINTENANT, juste avant que le
     * linker n'y ecrive l'exe. On a passe le portail de compile et la compile du
     * bundle : le dossier n'est cree que pour un build qui va reellement
     * produire un binaire. Un echec en amont n'aura laisse aucun `exe/<target>/`
     * vide. Le link ecrit directement dans `outputName` (-o), donc son dossier
     * parent doit exister a cet instant. */
    {
        char outDirBuf[1024];
        snprintf(outDirBuf, sizeof(outDirBuf), "%s", outputName);
        char* lastSep = NULL;
        for (char* p = outDirBuf; *p; p++) {
            if (*p == '/' || *p == '\\') lastSep = p;
        }
        if (lastSep != NULL) {
            *lastSep = '\0';
            if (build_ensure_dirs(outDirBuf) != 0) {
                fprintf(stderr, owl_error_render_lang_is_en()
                    ? "owl build: cannot create output directory '%s'\n"
                    : "owl build: creation du dossier de sortie impossible : '%s'\n",
                    outDirBuf);
                remove(rspPath);
                free(owlRoot); free(incDir); free(libDir); free(cfgPath);
                free(link_flags); free(objects); free(libs_rel); free(libs_abs); free(libs_names); free(qt6_root_default); free(duckdb_root_default); free(llama_root_default);
                project_free(&project);
                return 65;
            }
        }
    }

    /* 4. Lance c++ (link). Qt6 = C++, donc on lie via c++ pour avoir
     * libstdc++ statique. zig c++ -target <X> ou c++ via PATH.
     * -Wno-nullability-completeness : les en-tetes libc++ embarques par zig
     * (clang) crachent des centaines de warnings "pointer is missing a
     * nullability type specifier" quand les headers C++ (string, thread...)
     * sont tires. Benins, headers systeme, ils noient la console utilisateur.
     * On muselle ce seul diagnostic (pas -w global : les vrais warnings du
     * code restent visibles). Branche zig uniquement (le c++ systeme du
     * fallback n'est pas concerne). */
    char gccCmd[2048];
    if (useZig) {
#ifdef _WIN32
        snprintf(gccCmd, sizeof(gccCmd), "\"\"%s\" c++ -Wno-nullability-completeness -target %s @%s\"",
                 zigBin, zigTarget, rspPath);
#else
        snprintf(gccCmd, sizeof(gccCmd), "\"%s\" c++ -Wno-nullability-completeness -target %s @%s",
                 zigBin, zigTarget, rspPath);
#endif
    } else {
        snprintf(gccCmd, sizeof(gccCmd), "c++ @%s", rspPath);
    }
    printf(owl_error_render_lang_is_en()
        ? "owl build: compiling...\n"
        : "owl build: compilation...\n");
    int rc = system(gccCmd);

    /* 5. Cleanup (skip si OWL_KEEP_BUILD pour debug) */
    if (!getenv("OWL_KEEP_BUILD")) {
        remove(bundlePath);
        remove(bundleObj);
        remove(rspPath);
    }
    free(owlRoot); free(incDir); free(libDir); free(cfgPath);
    free(link_flags); free(objects); free(libs_rel); free(libs_abs); free(libs_names); free(qt6_root_default); free(duckdb_root_default); free(llama_root_default);
    project_free(&project);

    if (rc != 0) {
        fprintf(stderr, owl_error_render_lang_is_en()
            ? "owl build: c++ failed (exit code %d)\n"
            : "owl build: echec de c++ (code de sortie %d)\n", rc);
        /* Le link n'a produit aucun exe : retire le dossier de sortie vide
         * qu'on avait materialise pour le linker, sans jamais toucher un
         * dossier non vide (build anterieur). */
        build_rmdir_output_if_empty(outputName);
        return 65;
    }

    printf(owl_error_render_lang_is_en()
        ? "owl build: %s created\n"
        : "owl build: %s cree\n", outputName);
    return 0;
}
