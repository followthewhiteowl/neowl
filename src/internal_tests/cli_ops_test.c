// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test des commandes CLI OPERATIONNELLES.
 *
 * Execute via `owl --self-test-cli-ops` (cf main.c). Complement de
 * `--self-test-cli` (cli_doc_test.c) qui ne couvre que les commandes
 * DOCUMENTAIRES (help/primer/learn/list/explain/...). Ici on verrouille
 * le dispatch argv des commandes qui AGISSENT : fmt, check, list keywords.
 *
 * Trou systemique remonte par les reviewers de  /  :
 *   -  : `owl fmt` sans cible doit sortir non-zero (64) et NE RIEN
 *     formater (formater l'arbre entier par defaut est destructeur).
 *   -  : `owl list keywords` doit exposer la section « Reserves »
 *     (local / global).
 *
 * Approche : in-process, identique a cli_doc_test.c. On redirige stdout
 * vers un fichier temporaire, on appelle directement le handler avec un
 * argv synthetique, on verifie rc + substrings, on restaure stdout.
 *
 * Retourne 0 si tous les tests passent, sinon le nombre d'echecs. */

#include "vm.h"
#include "fixture_unique.h"

/* Reset des tables compile-time (POO + registre ServiceREST + user_procs)
 * entre deux compilations independantes dans un meme process — cf. testing.c. */
extern void compiler_reset_poo_state(void);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #define owl_mkdir(p)  _mkdir(p)
  #define owl_chdir     _chdir
  #define owl_getcwd    _getcwd
  #define owl_rmdir     _rmdir
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #define owl_mkdir(p)  mkdir((p), 0755)
  #define owl_chdir     chdir
  #define owl_getcwd    getcwd
  #define owl_rmdir     rmdir
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* Handlers drives (declares ailleurs). */
extern int handle_fmt_command(int argc, char* argv[]);
extern int handle_check_command_no_vm_lifecycle(const char* filepath);
/*  — check en mode DOSSIER (deux passes contexte + capture). Gere son
 * propre vm_init/vm_free ; le harnais re-init apres appel (cf. check_projet_shim). */
extern int handle_check_project_command(const char* dir, const char* activeFile,
                                        bool overlayStdin);
extern int cli_doc_list(int argc, char** argv);
extern int handle_new_command(int argc, char* argv[]);
extern int cli_doc_template(int argc, char** argv);
/*  — bascule la langue de rendu (equivalent `--lang en`) pour tester
 * le template EN genere par `owl new`. */
extern void owl_error_render_set_lang_en(int enabled);
extern int  owl_error_render_lang_is_en(void);

/* Helper teste par  : strip d'un BOM UTF-8 en tete de chaine.
 * Defini inline dans common.h, partage par project.c (file-side) et
 * main.c:repl() (stdin-side). */
#include "common.h"

/* ================================================================
 * Infra (mirroir cli_doc_test.c)
 * ================================================================ */

static int  g_pass = 0;
static int  g_fail = 0;
static char g_buf[128 * 1024];

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

#define CHECK_HAS(needle, label) \
    CHECK(strstr(g_buf, (needle)) != NULL, label)

#define CHECK_RC(actual, expected, label) \
    CHECK((actual) == (expected), label)

/* ================================================================
 * Filet de sortie
 *
 * Ce fichier depose deux choses dans le repertoire courant : la capture
 * stdout (`_owl_cli_ops_test_<pid>.tmp`) et la racine de bac a sable
 * (`.owl_cli_ops_sandbox_<pid>/`). Les deux n'etaient effacees que sur le
 * chemin nominal. Un arret entre la creation et l'effacement laisse donc un
 * residu de plus a la racine du depot — c'est l'interruption, pas le chemin
 * nominal, qui produit l'accumulation.
 *
 * Le menage ne porte QUE sur ce que ce processus a cree et refuse tout chemin
 * qui ne porte pas nos prefixes : le depot est partage avec d'autres agents,
 * un retrait recursif mal pointe emporterait leur travail.
 * ================================================================ */

/* Point initial : ce bac a sable nait dans le repertoire courant —
 * la racine du depot quand `make` lance ce self-test — et contient des
 * `projet.owl` / `theme.owl`. Tous les walkers du depot sautent les entrees
 * pointees, donc une campagne `owl test` concurrente ne l'enumere plus ; sans
 * ca elle chargeait ces fichiers comme compagnons, et un `projet.owl` qui
 * apparait puis disparait deplace en plus une frontiere de projet
 * sous ses pieds. La fenetre de course est ouverte pendant que le self-test
 * TOURNE : le filet de sortie ci-dessus nettoie bien, il ne la ferme pas. */
#define SANDBOX_PREFIXE ".owl_cli_ops_sandbox_"
#define CAPTURE_PREFIXE "_owl_cli_ops_test_"

static char g_capture_tmp[1152];   /* capture en cours (absolu), "" sinon */
static char g_sandbox_root[128];   /* racine de bac a sable (relative), "" sinon */
static char g_filet_cwd[1024];     /* cwd au moment de poser le filet */

/* Vide `dir` recursivement puis le supprime. N'est appele que sur un chemin
 * deja valide par le garde de prefixe de son appelant. */
static void retirer_arbre(const char* dir) {
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char p[1024];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && (st.st_mode & S_IFDIR)) retirer_arbre(p);
            else owl_unlink(p);
        }
        closedir(d);
    }
    owl_rmdir(dir);
}

/* Retire la racine de bac a sable et tout ce qu'elle contient. Recursif :
 * un test interrompu laisse son dossier de travail dedans, et un rmdir simple
 * echoue alors en silence sur un parent non vide. */
static void sandbox_wipe(void) {
    if (g_sandbox_root[0] == '\0') return;
    if (strncmp(g_sandbox_root, SANDBOX_PREFIXE, strlen(SANDBOX_PREFIXE)) != 0) return;
    retirer_arbre(g_sandbox_root);
    g_sandbox_root[0] = '\0';
}

static void filet_balayer(void) {
    /* g_sandbox_root est relatif : un arret survenu depuis un dossier de
     * travail le rendrait introuvable sans ce retour. */
    if (g_filet_cwd[0] != '\0') owl_chdir(g_filet_cwd);

    if (g_capture_tmp[0] != '\0') {
        if (strstr(g_capture_tmp, CAPTURE_PREFIXE) != NULL) owl_unlink(g_capture_tmp);
        g_capture_tmp[0] = '\0';
    }
    sandbox_wipe();
}

static void filet_signal(int sig) {
    filet_balayer();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void filet_poser(void) {
    static int pose = 0;
    if (pose) return;
    pose = 1;
    if (owl_getcwd(g_filet_cwd, sizeof(g_filet_cwd)) == NULL) g_filet_cwd[0] = '\0';
    atexit(filet_balayer);
    /* atexit ne couvre pas Ctrl-C ni un kill : c'est justement ce chemin-la
     * qui a produit les residus observes a la racine du depot. */
    signal(SIGINT,  filet_signal);
    signal(SIGTERM, filet_signal);
}

/* Capture stdout dans g_buf le temps d'appeler fn(argc, argv). Pattern
 * dup/dup2/open portable, identique a cli_doc_test.c (cf. son commentaire
 * detaille sur le pourquoi du dup vs freopen). */
static int capture_call(int (*fn)(int, char**), int argc, char** argv) {
    /* Nom propre au processus : a nom constant, deux executions
     * lancees du meme dossier se tronquaient mutuellement cette capture, et
     * g_buf arrivait vide -> assertions de contenu rouges sur un CLI intact. */
    char TMP[128];
    fixture_unique(TMP, sizeof TMP, CAPTURE_PREFIXE, ".tmp");
    g_buf[0] = '\0';

    /* Enregistre la capture en absolu : le filet de sortie revient au cwd de
     * depart, et certains tests capturent depuis un dossier de bac a sable. */
    filet_poser();
    char cwd[1024];
    if (owl_getcwd(cwd, sizeof(cwd)) != NULL)
        snprintf(g_capture_tmp, sizeof(g_capture_tmp), "%s/%s", cwd, TMP);
    else
        snprintf(g_capture_tmp, sizeof(g_capture_tmp), "%s", TMP);

    fflush(stdout);

    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return -1;

    int tmp_fd = owl_open(TMP,
                          O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
                          0644);
    if (tmp_fd < 0) {
        owl_close(saved_fd);
        return -1;
    }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd);
        owl_close(saved_fd);
        return -1;
    }
    owl_close(tmp_fd);

    int rc = fn(argc, argv);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_buf, 1, sizeof(g_buf) - 1, r);
        g_buf[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    g_capture_tmp[0] = '\0';
    return rc;
}

/* Lit un fichier entier dans un buffer alloue (NULL si absent). */
static char* slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Vrai si `path` existe et est lisible (marqueur de fichier cree). */
static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Ecrit `content` dans `path` (ecrase). Sert a poser une sentinelle avant
 * un test de non-ecrasement. */
static void write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

/* ================================================================
 * Sandbox cwd —  (owl new projet/project)
 *
 * `owl new projet` ecrit dans le DOSSIER COURANT (pas un chemin argv), donc
 * driver le handler in-process sans bouger le cwd polluerait la racine du
 * repo (fichier projet.owl/project.owl orphelin, HARD RULE 7). On bascule
 * le process entier dans un dossier sandbox temporaire, on restaure le cwd
 * d'origine a la fin de CHAQUE test (les tests suivants du meme process
 * — fmt/check/list plus haut dans ce fichier — supposent cwd = racine repo).
 * ================================================================ */

static char g_prev_cwd[1024];

/* Racine de sandbox propre au processus.
 *
 * Le PID vit sur le dossier PARENT, jamais sur le dossier de travail lui-meme :
 * deux tests lisent le NOM du dossier courant (nom de projet derive du dossier,
 * et « le nom du dossier sandbox n'apparait pas quand un nom explicite est
 * fourni »). Suffixer ce nom-la aurait laisse les assertions vertes tout en
 * changeant ce qu'elles observent. Le parent, lui, n'est jamais lu par
 * `owl new` (qui ne prend que le dernier segment du cwd). */
static const char* sandbox_root(void) {
    if (g_sandbox_root[0] == '\0') {
        filet_poser();
        fixture_unique(g_sandbox_root, sizeof g_sandbox_root, SANDBOX_PREFIXE, "");
        owl_mkdir(g_sandbox_root);
    }
    return g_sandbox_root;
}

/* Cree `<racine>/<dir>` (au besoin) et y bascule le process. A appairer avec
 * leave_sandbox_dir() en fin de test, sans exception. */
static void enter_sandbox_dir(const char* dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", sandbox_root(), dir);
    if (owl_getcwd(g_prev_cwd, sizeof(g_prev_cwd)) == NULL) g_prev_cwd[0] = '\0';
    owl_mkdir(path);
    owl_chdir(path);
}

/* Restaure le cwd sauvegarde par enter_sandbox_dir(). */
static void leave_sandbox_dir(void) {
    if (g_prev_cwd[0] != '\0') owl_chdir(g_prev_cwd);
}

/* Retire `<racine>/<dir>` apres leave_sandbox_dir() (dossier suppose vide). */
static void remove_sandbox_dir(const char* dir) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", sandbox_root(), dir);
    owl_rmdir(path);
}

/* ================================================================
 * Tests : owl fmt
 * ================================================================ */

/* `owl fmt` sans cible -> exit 64 + AUCUN fichier touche. On capture
 * l'etat d'un fichier fixture avant/apres pour prouver le no-op. */
static void test_fmt_no_arg_is_noop_exit64(void) {
    static const char* FIXTURE = "tests/internal_fixtures/cli_ops/well_formatted.owl";

    char* before = slurp(FIXTURE);
    CHECK(before != NULL, "fixture well_formatted.owl lisible");

    char* argv[] = { "owl", "fmt" };
    int rc = capture_call(handle_fmt_command, 2, argv);

    CHECK_RC(rc, 64, "owl fmt sans cible -> exit 64");

    char* after = slurp(FIXTURE);
    CHECK(after != NULL, "fixture toujours lisible apres fmt no-arg");
    if (before && after) {
        CHECK(strcmp(before, after) == 0,
              "owl fmt sans cible ne touche AUCUN fichier (no-op)");
    }
    free(before);
    free(after);
}

/* `owl fmt <fichier deja formate>` -> exit 0 et le fichier reste
 * identique (deja canonique = idempotent). */
static void test_fmt_target_exit0(void) {
    static const char* FIXTURE = "tests/internal_fixtures/cli_ops/well_formatted.owl";

    char* before = slurp(FIXTURE);
    CHECK(before != NULL, "fixture well_formatted.owl lisible (target)");

    /*  : scope le contexte de format au dossier de la fixture via
     * --project. Sans ca, fmt_build_context prend racine=cwd et scanne tout le
     * repo (~1500 .owl, ~4s) pour un fichier self-contained. Verdict inchange
     * (exit 0 + idempotent) : well_formatted.owl n'a aucune dep cross-fichier. */
    char* argv[] = { "owl", "fmt", (char*)FIXTURE,
                     "--project", "tests/internal_fixtures/cli_ops" };
    int rc = capture_call(handle_fmt_command, 5, argv);

    CHECK_RC(rc, 0, "owl fmt <fichier> -> exit 0");

    char* after = slurp(FIXTURE);
    if (before && after) {
        CHECK(strcmp(before, after) == 0,
              "fichier deja formate reste identique (idempotent)");
    }
    free(before);
    free(after);
}

/* `owl fmt <fichier.md>` (extension non .owl) -> exit 64 et le fichier
 * reste byte-identique. Le formatter ne sait traiter que du
 * OWL ; l'appliquer a un markdown / texte casse le contenu silencieusement
 * (cas reel : CLAUDE.md massacre). Le rejet doit etre base sur l'extension
 * uniquement (case-insensitive), pas sur une heuristique de contenu. */
static void test_fmt_rejects_non_owl_extension(void) {
    static const char* FIXTURE = "tests/internal_fixtures/cli_ops/not_owl.md";

    char* before = slurp(FIXTURE);
    CHECK(before != NULL, "fixture not_owl.md lisible");

    char* argv[] = { "owl", "fmt", (char*)FIXTURE };
    int rc = capture_call(handle_fmt_command, 3, argv);

    CHECK_RC(rc, 64, "owl fmt <fichier.md> -> exit 64");

    char* after = slurp(FIXTURE);
    CHECK(after != NULL, "fixture toujours lisible apres rejet");
    if (before && after) {
        CHECK(strcmp(before, after) == 0,
              "owl fmt sur extension non-.owl ne touche pas le fichier");
    }
    free(before);
    free(after);
}

/* `owl fmt <a.owl> <b.owl>` -> refus explicite (exit 64), et AUCUN des deux
 * fichiers touche.
 *
 * Mesure de  sur la commande d'a cote : le dernier chemin gagnait,
 * rc=0, et rien n'etait dit du premier — un fichier nomme par l'utilisateur
 * repartait inchange sans un mot. La convention du depot est etablie par la
 * meme mesure : aucune commande qui prend un chemin ne cumule de cibles, et
 * le refus sort en 64 (`owl --check a b`, `owl test <d1> <d2>`). */
/* Le refus part sur stderr (convention CLI du depot) alors que capture_call ne
 * detourne que stdout : on replie stderr sur stdout le temps de l'appel pour
 * que CHECK_HAS voie le message. Meme montage que test_cmd_shim plus bas. */
static int fmt_cmd_shim(int argc, char** argv) {
    fflush(stderr);
    int saved_err = owl_dup(owl_fileno(stderr));
    owl_dup2(owl_fileno(stdout), owl_fileno(stderr));
    int rc = handle_fmt_command(argc, argv);
    fflush(stderr);
    if (saved_err >= 0) {
        owl_dup2(saved_err, owl_fileno(stderr));
        owl_close(saved_err);
    }
    return rc;
}

static void test_fmt_two_targets_refused_exit64(void) {
    static const char* A = "tests/internal_fixtures/cli_ops/well_formatted.owl";
    static const char* B = "tests/internal_fixtures/cli_ops_voisin/entree.owl";

    char* beforeA = slurp(A);
    char* beforeB = slurp(B);
    CHECK(beforeA != NULL && beforeB != NULL,
          "les deux fixtures .owl sont lisibles avant le refus");

    char* argv[] = { "owl", "fmt", (char*)A, (char*)B };
    int rc = capture_call(fmt_cmd_shim, 4, argv);

    CHECK_RC(rc, 64, "owl fmt <a.owl> <b.owl> -> exit 64");
    CHECK_HAS("owl fmt", "le refus nomme la commande");
    CHECK_HAS("entree.owl", "le refus cite le chemin en trop");

    char* afterA = slurp(A);
    char* afterB = slurp(B);
    if (beforeA && afterA) {
        CHECK(strcmp(beforeA, afterA) == 0,
              "owl fmt deux cibles ne touche pas la premiere");
    }
    if (beforeB && afterB) {
        CHECK(strcmp(beforeB, afterB) == 0,
              "owl fmt deux cibles ne touche pas la seconde");
    }
    free(beforeA); free(beforeB); free(afterA); free(afterB);
}

/* ================================================================
 * Tests : owl list keywords
 * ================================================================ */

/* `owl list keywords` doit afficher la section « Reserves » avec
 * local et global. cli_doc_list attend argv[0] == "list". */
static void test_list_keywords_has_reserved(void) {
    char* argv[] = { "list", "keywords" };
    int rc = capture_call(cli_doc_list, 2, argv);

    CHECK_RC(rc, 0, "owl list keywords -> exit 0");
    CHECK_HAS("Réservés", "list keywords contient la section Réservés");
    CHECK_HAS("local", "list keywords expose 'local'");
    CHECK_HAS("global", "list keywords expose 'global'");
}

/* ================================================================
 * Tests : owl --check (bonus)
 * ================================================================ */

/* Wrapper pour driver handle_check_command_no_vm_lifecycle via
 * capture_call (qui attend une signature int(int,char**)). On passe le
 * chemin dans argv[1]. */
static int check_shim(int argc, char** argv) {
    (void)argc;
    return handle_check_command_no_vm_lifecycle(argv[1]);
}

static void test_check_valid_file_exit0(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/cli_ops/well_formatted.owl" };
    int rc = capture_call(check_shim, 2, argv);
    CHECK_RC(rc, 0, "owl --check fichier valide -> exit 0");
}

static void test_check_invalid_file_nonzero(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/cli_ops_broken/broken.owl" };
    int rc = capture_call(check_shim, 2, argv);
    CHECK(rc != 0, "owl --check fichier invalide -> exit non-zero");
}

/*  — `--check <fichier>` doit controler le MEME perimetre que celui
 * que l'execution chargerait ( : script = fichier + voisins du meme
 * dossier). Un voisin qui ne compile pas rend le fichier inexecutable : le
 * controle sort donc 65 et nomme le fichier fautif avec sa ligne, ancre sur la
 * premiere ligne du fichier controle (aucun squiggle place dans un fichier que
 * l'editeur n'a pas ouvert). Sans ca, le controle rendait CLEAN sur un dossier
 * qui ne peut pas tourner — faux vert sur la commande que l'utilisateur lance
 * naturellement, et que l'editeur relance en continu. */
static void test_check_reports_broken_neighbor(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/cli_ops_voisin/entree.owl" };
    int rc = capture_call(check_shim, 2, argv);
    CHECK_RC(rc, 65, "owl --check <entree saine, voisin casse> -> exit 65");
    CHECK_HAS("FEN_Cassee.owl:31", "le diagnostic nomme le voisin fautif et sa ligne");
    CHECK_HAS("\"line\":1", "le diagnostic est ancre sur la 1re ligne du fichier controle");
    /* La construction fautive du compagnon est un type inconnu : durablement
     * invalide. Elle etait auparavant une syntaxe « pas encore supportee »
     * (mise en forme de cellule de Table) que  a rendue valide — le
     * compagnon compilait, ce test verdissait, et le faux vert que 
     * venait de fermer se rouvrait en silence. */
    CHECK_HAS("Type inconnu", "le message du compagnon est repris tel quel");
}

/*  — `owl --check` ne voyait pas les fichiers de TEST du perimetre :
 * ils sont ecartes de la decouverte, donc leur source n'entrait jamais dans la
 * liste que le scan d'usage cross-fichier parcourt. Une variable dont le seul
 * utilisateur est un test etait annoncee "jamais utilisee" par `--check` alors
 * que `owl test` sur le meme dossier ne disait rien. C'est le chemin qui
 * alimente l'editeur : le soulignement conseillait de supprimer du code en
 * service, en continu pendant la frappe.
 *
 * Les DEUX familles sont couvertes, elles n'empruntent pas la meme cle : une
 * globale top-level est tracee sous son nom nu, une globale d'element sous sa
 * cle manglee `<ELEM>$<var>` (cf. ). */
static void test_check_global_used_by_test_neighbor_not_flagged(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/check_voisin_test/A_Lib.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <lib lue par un test voisin> -> exit 0");
    CHECK(strstr(g_buf, "'tabTop'") == NULL,
          "globale top-level lue par un test voisin : plus d'avertissement");
    CHECK_HAS("Variable 'tabOrphelin' est déclarée mais jamais utilisée.",
              "temoin : la globale reellement inutilisee reste signalee");
}

static void test_check_element_global_used_by_test_neighbor_not_flagged(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/check_voisin_test/FEN_CheckVoisin.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <fenetre lue par un test voisin> -> exit 0");
    CHECK(strstr(g_buf, "'nLu'") == NULL,
          "globale d'element jointe par un test voisin : plus d'avertissement");
    CHECK_HAS("Variable 'tabJamais' est déclarée mais jamais utilisée.",
              "temoin : le nom nu homonyme du voisin ne tait pas la globale d'element");
}

/* Le mode script ISOLE ne doit rien perdre : un fichier vraiment seul dans son
 * dossier n'a aucun usage possible ailleurs, l'avertissement reste du. Sans ce
 * temoin, un correctif qui eteindrait l'analyse passerait pour bon. */
static void test_check_lone_file_still_reports_unused(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/check_seul/seul.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <fichier seul dans son dossier> -> exit 0");
    CHECK_HAS("Variable 'tabSeulInutilise' est déclarée mais jamais utilisée.",
              "fichier sans voisin : l'avertissement d'inutilisation subsiste");
}

/* ================================================================
 * Tests : owl --check <dossier> — faux doublon ServiceREST
 *
 * Le check en mode DOSSIER compile en deux passes (contexte puis capture).
 * La passe de contexte enregistre le bloc ServiceREST dans le registre de
 * declaration ; sans reset de ce registre avant la passe de capture, la
 * seconde compilation retombe sur l'entree deja presente -> faux
 * "Service 'X' deja declare". Symetrique du reset Fenetre/Etat.
 * Le check MONO (--check <fichier>) ne compile la cible qu'une fois : pas
 * concerne. Ce vecteur passe donc par handle_check_project_command.
 * ================================================================ */

/* deja declare, en UTF-8 (message E_SEM_DSL_SERVICE_BLOCK_DUPLICATE). */
/* Litteral coupe apres \xa9 : sinon \xa9 avalerait le 'c' suivant (hex). */
#define SERVICE_DUP_NEEDLE "d\xc3\xa9j\xc3\xa0 d\xc3\xa9" "clar\xc3\xa9"

static const char* PROJET_SERVICE_MARKER =
    "MonServiceCheck est un Projet\n"
    "\tPROPRIETES\n"
    "\t\tLangues     = [langueFrancais]\n"
    "\t\tDescription = \"\"\n"
    "\tFIN\n"
    "FIN\n";

static const char* SERVICE_REST_SANTE =
    "ServiceFacture est un ServiceREST\n"
    "\tPROPRIETES\n"
    "\t\tUrlBase = [\"api\", \"v1\"]\n"
    "\tFIN\n"
    "\n"
    "\tSante est une OperationHTTP\n"
    "\t\tPROPRIETES\n"
    "\t\t\tMethode  = httpGet\n"
    "\t\t\tChemin   = [\"sante\"]\n"
    "\t\t\tReponses = [httpOK]\n"
    "\t\t\tCodeEtat = httpOK\n"
    "\t\tFIN\n"
    "\t\tPROCEDURE surRequete() : chaine\n"
    "\t\t\tRENVOYER \"ok\"\n"
    "\t\tFIN\n"
    "\tFIN\n"
    "FIN\n";

/* Compte les occurrences (non chevauchantes) de needle dans hay. */
static int count_occ(const char* hay, const char* needle) {
    int n = 0;
    size_t len = strlen(needle);
    const char* p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += len; }
    return n;
}

/* Wrapper pour driver handle_check_project_command via capture_call.
 * Le handler gere son propre vm_init/vm_free ; on re-init derriere pour que
 * les tests suivants du meme process retrouvent un registre peuple
 * (meme discipline que test_new_service_*). */
static int check_projet_shim(int argc, char** argv) {
    (void)argc;
    int rc = handle_check_project_command(argv[1], NULL, false);
    vm_init();
    compiler_reset_poo_state();
    return rc;
}

/* Un projet.owl + un fichier ServiceREST-only (service declare UNE fois),
 * checke en mode dossier : AUCUN faux "deja declare". ROUGE avant 
 * (la passe de contexte laisse le service dans le registre). */
static void test_check_projet_service_no_false_duplicate(void) {
    enter_sandbox_dir("_owl_check_projet_service_ok");
    write_file("projet.owl", PROJET_SERVICE_MARKER);
    write_file("api.owl", SERVICE_REST_SANTE);

    char* argv[] = { "owl", "." };
    int rc = capture_call(check_projet_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <projet ServiceREST unique> -> exit 0");
    CHECK(strstr(g_buf, SERVICE_DUP_NEEDLE) == NULL,
          "service declare une seule fois : aucun faux 'deja declare'");

    owl_unlink("projet.owl");
    owl_unlink("api.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_check_projet_service_ok");
}

/* Vrai doublon : deux fichiers declarent le MEME service. Le check dossier
 * DOIT toujours le detecter, et une seule fois (le fix ne masque pas le vrai
 * positif). Temoin que le reset n'eteint pas la detection intra-capture. */
static void test_check_projet_service_real_duplicate_detected(void) {
    enter_sandbox_dir("_owl_check_projet_service_dup");
    write_file("projet.owl", PROJET_SERVICE_MARKER);
    write_file("svc_a.owl", SERVICE_REST_SANTE);
    write_file("svc_b.owl", SERVICE_REST_SANTE);

    char* argv[] = { "owl", "." };
    int rc = capture_call(check_projet_shim, 2, argv);

    CHECK_RC(rc, 65, "owl --check <deux fois le meme service> -> exit 65");
    CHECK(count_occ(g_buf, SERVICE_DUP_NEEDLE) == 1,
          "vrai doublon detecte exactement une fois (pas de double comptage)");

    owl_unlink("projet.owl");
    owl_unlink("svc_a.owl");
    owl_unlink("svc_b.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_check_projet_service_dup");
}

/* ================================================================
 * Tests : owl --check <fichier de test> — callback injecte cross-fichier
 *
 *
 * Une procedure top-level declaree dans un fichier de TEST, appelee par le
 * FinInitialisation d'une fenetre voisine (pattern callback injecte). A
 * l'execution, le fichier de test EST l'entree : sa procedure est une globale
 * visible du compagnon, donc `owl test` passe. Le controle mono ecartait les
 * fichiers de test de l'indexation meme quand la CIBLE est ce test :
 * la passe perimetre recompilait alors le compagnon sans voir le
 * callback -> faux positif "fonction/procedure inconnue". Meme famille que
 *  /  /  : aligner le controle sur l'execution.
 * ================================================================ */

/* Le fichier de test cible DECLARE le callback appele par le compagnon : le
 * controle doit le resoudre comme l'execution, aucun faux positif. ROUGE avant
 *  (le test n'etant pas indexe, le compagnon ne voit pas le callback). */
static void test_check_callback_injecte_par_test_resolu(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/check_callback_cross_fichier/test_cb.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <test qui injecte un callback au compagnon> -> exit 0");
    CHECK(strstr(g_buf, "cb_injecte_par_test") == NULL,
          "le callback declare dans le test cible est resolu (pas de faux positif inconnue)");
}

/* TEMOIN discriminant : le compagnon appelle un callback declare NULLE PART.
 * Le controle DOIT toujours le remonter (65 + message inconnue). Sans ce
 * temoin, un correctif qui indexerait aveuglement tout ce que nomme un
 * compagnon passerait pour bon en aveuglant le vrai positif. */
static void test_check_callback_vraiment_inconnu_toujours_remonte(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/check_callback_inconnu/test_cb_bad.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 65, "owl --check <compagnon appelant un callback inexistant> -> exit 65");
    CHECK_HAS("cb_vraiment_inexistant", "le callback reellement inconnu est nomme");
    CHECK_HAS("inconnue", "le vrai positif 'fonction/procedure inconnue' reste remonte");
}

/* ================================================================
 * Tests : attribut de declaration <utile> / <useful>
 *
 * Nouvel attribut place apres le type (`n est un entier <utile>`, forme avec
 * valeur libre `<utile="commentaire">`) qui doit eteindre W_SEM_VAR_UNUSED
 * pour la variable concernee, sans toucher aux autres declarations du meme
 * fichier. Coexiste avec le prefixe `_` deja fonctionnel (opt-out historique,
 * verifie ici comme garde de non-regression). Grammaire pas encore posee au
 * moment ou ces cas sont ecrits : tous les cas d'extinction sont ROUGES
 * attendus (l'attribut ne suspend rien aujourd'hui, le warning reste emis).
 * ================================================================ */

/* Forme FR canon, bare. Le fichier porte deux temoins dans la MEME
 * compilation : 'm' (sans aucun attribut) doit rester signalee -- garde de
 * non-regression sur le diagnostic lui-meme -- et '_p' (prefixe historique)
 * doit deja etre eteinte independamment de ce ticket -- garde du mecanisme
 * existant, cense rester vert avant/apres l'impl. Seule l'assertion sur 'n'
 * est rouge attendue. */
static void test_check_util_attr_fr_bare_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_fr/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut <utile> FR bare> -> exit 0");
    CHECK(strstr(g_buf, "'n'") == NULL,
          "variable attribuee <utile> : plus d'avertissement");
    CHECK_HAS("Variable 'm' est déclarée mais jamais utilisée.",
              "temoin : la variable sans attribut reste signalee dans le meme fichier");
    CHECK(strstr(g_buf, "'_p'") == NULL,
          "temoin : le prefixe _ existant reste une extinction valide (mecanisme non touche)");
}

/* Forme FR avec valeur libre : la chaine documente le pourquoi et ne doit ni
 * casser le parse ni empecher l'extinction du warning. */
static void test_check_util_attr_fr_valeur_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_fr_valeur/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut <utile=\"...\"> FR> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur <utile=\"...\">");
    CHECK(strstr(g_buf, "'n'") == NULL,
          "variable attribuee <utile=\"pas nécessaire\"> : plus d'avertissement");
}

/* Forme EN canon bare (invariant 3 : bilingue). */
static void test_check_util_attr_en_bare_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_en/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut <useful> EN bare> -> exit 0");
    CHECK(strstr(g_buf, "'n'") == NULL,
          "variable attribuee <useful> : plus d'avertissement");
}

/* Forme EN avec valeur libre (invariant 3 : bilingue, capacite valeur). */
static void test_check_util_attr_en_valeur_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_en_valeur/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut <useful=\"...\"> EN> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur <useful=\"...\">");
    CHECK(strstr(g_buf, "'n'") == NULL,
          "variable attribuee <useful=\"not needed\"> : plus d'avertissement");
}

/* Casse-insensible au parse (invariant 1), bilingue compris (invariant 3) :
 * <UTILE> (FR tout-majuscule) et <Useful> (EN casse mixte) doivent tous deux
 * eteindre le warning. */
static void test_check_util_attr_case_insensitive(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_casse/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut casse variee> -> exit 0");
    CHECK(strstr(g_buf, "'nMaj'") == NULL, "<UTILE> tout-majuscule accepte");
    CHECK(strstr(g_buf, "'nCasseMixte'") == NULL, "<Useful> casse mixte accepte");
}

/* Compatibilite avec une initialisation inline. NOTE (a lire avant de
 * modifier ce test) : `warn_unused_globals`/`warn_unused_locals`
 * (src/compiler.c) exemptent DEJA aujourd'hui toute variable initialisee
 * (`hasInitializer`) du warning, attribut ou pas — l'absence de warning ici
 * n'est donc PAS une preuve que l'attribut fonctionne, seulement que la
 * grammaire combinee <attribut> + `= <init>` compile sans erreur. Ce cas est
 * VERT des le depot (pas un rouge attendu) : seule la ligne CHECK_RC compte
 * comme assertion utile pour ce ticket ; signale au coordinator (cf. rapport
 *  test_writer) car il ne discrimine pas la feature. */
static void test_check_util_attr_with_init_compiles(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_init/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <attribut + init inline> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur la combinaison attribut + init");
}

/* ================================================================
 * Tests : attribut <utile> / <useful> sur un PARAMETRE de procedure
 *
 * Meme attribut que  mais pose apres le TYPE d'un parametre :
 * `PROCEDURE Foo(x est un entier <utile>)`. Avant  cette forme
 * produisait une erreur de parse (« ')' attendu apres les parametres. »)
 * ET laissait le warning « declaree mais jamais utilisee » actif. Apres
 *  : le parse passe et le warning est eteint POUR CE parametre.
 * Rouges attendus au depot (le parseur de params ne connait pas encore le
 * marqueur) : assertion CHECK_RC exit 0 + absence du warning sur le param
 * marque. Le temoin non marque ('y') reste signale (non-regression).
 * ================================================================ */

/* Forme FR canon <utile> sur un parametre, plus un temoin non marque ('y')
 * qui doit rester signale dans la meme procedure. */
static void test_check_util_attr_param_fr_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_param_fr/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <param <utile> FR> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur un param <utile> (plus de parse error)");
    CHECK(strstr(g_buf, "'x'") == NULL,
          "parametre attribue <utile> : plus d'avertissement");
    CHECK_HAS("Variable 'y' est déclarée mais jamais utilisée.",
              "temoin : le parametre sans attribut reste signale (non-regression)");
}

/* Forme EN canon <useful> sur un parametre (invariant 3 : bilingue). */
static void test_check_util_attr_param_en_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_param_en/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <param <useful> EN> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur un param <useful>");
    CHECK(strstr(g_buf, "'x'") == NULL,
          "parametre attribue <useful> : plus d'avertissement");
}

/* ================================================================
 * Tests : attribut <utile> / <useful> sur un PARAMETRE de METHODE de
 * classe (, extension de )
 *
 * Meme attribut, deuxieme des trois copies du parseur de params. Avant
 *  le marqueur posé apres le type d'un parametre de methode
 * produisait « ')' attendu apres les parametres. » ET laissait le warning
 * actif. Apres  : parse OK et warning eteint pour ce parametre.
 * Temoin non marque ('y', FR) reste signale (non-regression). Fixture =
 * fichier classe pur (pas de code runtime top-level, sinon rejet convention).
 * ================================================================ */

/* Forme FR canon <utile> sur un parametre de methode, plus un temoin non
 * marque ('y') qui doit rester signale dans la meme methode. */
static void test_check_util_attr_param_method_fr_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_param_method_fr/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <param methode <utile> FR> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur un param methode <utile> (plus de parse error)");
    CHECK(strstr(g_buf, "'x'") == NULL,
          "parametre methode attribue <utile> : plus d'avertissement");
    CHECK_HAS("Variable 'y' est déclarée mais jamais utilisée.",
              "temoin : le parametre methode sans attribut reste signale (non-regression)");
}

/* Forme EN canon <useful> sur un parametre de methode (invariant 3 : bilingue). */
static void test_check_util_attr_param_method_en_suppresses_warning(void) {
    char* argv[] = { "owl", "tests/internal_fixtures/util_attr_param_method_en/decl.owl" };
    int rc = capture_call(check_shim, 2, argv);

    CHECK_RC(rc, 0, "owl --check <param methode <useful> EN> -> exit 0, pas d'erreur de parse");
    CHECK(strstr(g_buf, "\"errors\":[]") != NULL,
          "aucune erreur de compilation sur un param methode <useful>");
    CHECK(strstr(g_buf, "'x'") == NULL,
          "parametre methode attribue <useful> : plus d'avertissement");
}

/* ================================================================
 * Tests : owl test — plusieurs chemins
 *
 * `owl test <dir1> <dir2>` ne traitait qu'UN des deux (mesure : le
 * DERNIER gagne, target etant ecrase a chaque positionnel) et n'en disait
 * rien : le rapport final annoncait « 1 suites / ALL CLEAR » sans mentionner
 * le chemin ignore. Convention des commandes voisines qui prennent un
 * chemin : `owl --check a b` refuse (usage + exit 64). `owl test` refuse
 * donc aussi, plutot que d'inventer un accumulateur multi-cibles.
 * ================================================================ */

extern int handle_test_command(int argc, char* argv[]);

/* Le refus part sur stderr (convention CLI du depot) alors que
 * capture_call ne detourne que stdout : on replie stderr sur stdout le
 * temps de l'appel pour que CHECK_HAS voie le message. */
static int test_cmd_shim(int argc, char** argv) {
    fflush(stderr);
    int saved_err = owl_dup(owl_fileno(stderr));
    owl_dup2(owl_fileno(stdout), owl_fileno(stderr));
    int rc = handle_test_command(argc, argv);
    fflush(stderr);
    if (saved_err >= 0) {
        owl_dup2(saved_err, owl_fileno(stderr));
        owl_close(saved_err);
    }
    return rc;
}

/* Deux chemins -> refus explicite (exit 64), et AUCUNE suite lancee : la
 * banniere du runner ne doit pas apparaitre. */
static void test_test_two_paths_refused_exit64(void) {
    char* argv[] = { "owl", "test",
                     "tests/regression/isolation_check_a",
                     "tests/regression/isolation_check_b" };
    int rc = capture_call(test_cmd_shim, 4, argv);

    CHECK_RC(rc, 64, "owl test <dir1> <dir2> -> exit 64");
    CHECK_HAS("owl test", "le refus nomme la commande");
    CHECK_HAS("isolation_check_b", "le refus cite le chemin en trop");
    CHECK(strstr(g_buf, "OWL Test Suite") == NULL,
          "owl test <dir1> <dir2> ne lance AUCUNE suite");
}

/* Trois chemins -> meme refus (le garde n'est pas un cas special a deux).
 * Cibles volontairement minuscules : tant que le garde manque, le dernier
 * chemin est REELLEMENT execute par ce self-test. */
static void test_test_three_paths_refused_exit64(void) {
    char* argv[] = { "owl", "test",
                     "tests/regression/isolation_check_a",
                     "tests/regression/isolation_check_b",
                     "tests/internal_fixtures/cli_ops" };
    int rc = capture_call(test_cmd_shim, 5, argv);

    CHECK_RC(rc, 64, "owl test <d1> <d2> <d3> -> exit 64");
    CHECK(strstr(g_buf, "OWL Test Suite") == NULL,
          "owl test trois chemins ne lance AUCUNE suite");
}

/* Une faute de frappe sur la verbosite (`-vvv`) partait en CIBLE : le runner
 * repondait « cannot open directory '-vvv' », rc=1. `owl fmt`
 * possede deja la garde du tiret et sort 64 avec la liste des options ; la
 * commande d'a cote la reprend telle quelle, plutot que de laisser un flag
 * mal tape se faire passer pour un dossier. */
static void test_test_unknown_option_refused_exit64(void) {
    char* argv[] = { "owl", "test", "-vvv" };
    int rc = capture_call(test_cmd_shim, 3, argv);

    CHECK_RC(rc, 64, "owl test -vvv -> exit 64");
    CHECK_HAS("-vvv", "le refus cite l'option inconnue");
    CHECK(strstr(g_buf, "cannot open directory") == NULL,
          "owl test -vvv n'est plus traite comme une cible");
    CHECK(strstr(g_buf, "OWL Test Suite") == NULL,
          "owl test -vvv ne lance AUCUNE suite");
}

/*  — un avertissement compile-time ne de la compilation d'un VOISIN
 * etait affiche sous le nom du fichier de test, avec le numero de ligne du
 * voisin : ligne juste, fichier faux, donc un diagnostic qui ressemble a une
 * information valide et conduit au mauvais endroit.
 *
 * La fixture porte DEUX avertissements de ligne differente, un par fichier :
 * sans le temoin du fichier de test, un correctif qui se contenterait
 * d'inverser l'attribution passerait pour bon. */
static void test_test_warning_names_its_own_file(void) {
    char* argv[] = { "owl", "test", "tests/internal_fixtures/warn_attrib_voisin" };
    int rc = capture_call(test_cmd_shim, 3, argv);

    CHECK_RC(rc, 0, "owl test <dossier a voisin> -> exit 0 (warnings non bloquants)");
    CHECK_HAS("A_Lib.owl:3 — Variable 'tabVoisinInutile'",
              "l'avertissement du voisin nomme le voisin");
    CHECK_HAS("test_warn_attrib.owl:2 — Variable 'tabTestInutile'",
              "temoin : l'avertissement du test nomme toujours le test");
    CHECK(strstr(g_buf, "test_warn_attrib.owl:3") == NULL,
          "la ligne du voisin n'est plus portee au compte du fichier de test");
    CHECK_HAS("2 WARNINGS",
              "l'attribution ne change pas le nombre d'avertissements");
    /* Le libelle de la SUITE reste le fichier de test : c'est lui qu'on relance.
     * Seule la ligne de diagnostic nomme le fichier fautif. */
    CHECK_HAS("✓ test_warn_attrib.owl",
              "la suite reste libellee par le fichier de test");
}

/*  — l'analyse d'usage etait cross-fichier pour les globales top-level
 * et strictement intra-fichier pour les globales d'element : une globale de
 * fenetre lue par un voisin dans sa forme qualifiee (la seule forme qui la
 * joigne de l'exterieur) etait annoncee "jamais utilisee". Un diagnostic qui
 * conseille de supprimer du code en service coute plus cher que pas de
 * diagnostic.
 *
 * La fixture porte les DEUX temoins, sans lesquels un baillon passerait pour un
 * correctif :
 *   - trois globales joignies depuis le voisin (lecture, mutation par native,
 *     lecture en cascade) : plus aucun avertissement ;
 *   - une globale jamais qualifiee nulle part : son avertissement subsiste,
 *     et le nom nu homonyme declare en locale dans le voisin ne le tait pas. */
static void test_element_global_used_by_neighbor_not_flagged(void) {
    char* argv[] = { "owl", "test", "tests/internal_fixtures/globale_element_voisin" };
    int rc = capture_call(test_cmd_shim, 3, argv);

    CHECK_RC(rc, 0, "owl test <fenetre + voisin> -> exit 0");
    CHECK(strstr(g_buf, "'nLu'") == NULL,
          "lecture qualifiee depuis un voisin : plus d'avertissement");
    CHECK(strstr(g_buf, "'tabMute'") == NULL,
          "mutation qualifiee depuis un voisin : plus d'avertissement");
    CHECK(strstr(g_buf, "'tabCascade'") == NULL,
          "lecture en cascade depuis un voisin : plus d'avertissement");
    CHECK_HAS("FEN_UsageVoisin.owl:22 — Variable 'tabJamais'",
              "temoin : la globale d'element reellement inutilisee reste signalee");
    CHECK_HAS("1 WARNINGS",
              "exactement un avertissement — ni baillon, ni faux positif residuel");
    CHECK_HAS("4 assertions",
              "les quatre assertions du voisin tournent bien");
}

/* ================================================================
 * Tests : owl_strip_utf8_bom
 *
 * REPL stdin oublie de strip le BOM UTF-8 (PowerShell 5.1 pipe avec BOM
 * en tete). On factorise le strip dans common.h, on couvre les 3 cas
 * cles : BOM en tete (strippe), pas de BOM (no-op), BOM au milieu
 * (non-strippe, coherent file-side).
 * ================================================================ */

static void test_strip_bom_at_head_removes_three_bytes(void) {
    char buf[32];
    memcpy(buf, "\xEF\xBB\xBF" "Trace(2+2)", 14); /* 3 BOM + 10 chars + \0 */
    buf[13] = '\0';
    bool stripped = owl_strip_utf8_bom(buf);
    CHECK(stripped == true, "owl_strip_utf8_bom retourne true quand BOM present");
    CHECK(strcmp(buf, "Trace(2+2)") == 0,
          "BOM en tete -> les 3 octets sont retires");
}

static void test_strip_bom_absent_is_noop(void) {
    char buf[32];
    strcpy(buf, "Trace(2+2)");
    bool stripped = owl_strip_utf8_bom(buf);
    CHECK(stripped == false, "owl_strip_utf8_bom retourne false quand pas de BOM");
    CHECK(strcmp(buf, "Trace(2+2)") == 0,
          "pas de BOM -> chaine inchangee");
}

static void test_strip_bom_in_middle_not_stripped(void) {
    /* BOM apparaissant au milieu : doit rester intact (coherent avec
     * le strip file-side qui ne touche qu'au debut du flux). */
    char buf[32];
    memcpy(buf, "ab\xEF\xBB\xBF" "cd", 8);
    buf[7] = '\0';
    bool stripped = owl_strip_utf8_bom(buf);
    CHECK(stripped == false, "BOM au milieu -> retourne false (non strippe)");
    CHECK(memcmp(buf, "ab\xEF\xBB\xBF" "cd", 8) == 0,
          "BOM au milieu -> chaine inchangee");
}

static void test_strip_bom_short_string(void) {
    /* Garde-fou : chaine plus courte que 3 octets ne doit pas crasher. */
    char buf[4] = { 'h', 'i', '\0', 0 };
    bool stripped = owl_strip_utf8_bom(buf);
    CHECK(stripped == false, "chaine < 3 octets -> no-op safe");
    CHECK(strcmp(buf, "hi") == 0, "chaine courte inchangee");

    char empty[4] = { '\0', 0, 0, 0 };
    bool s2 = owl_strip_utf8_bom(empty);
    CHECK(s2 == false, "chaine vide -> no-op safe");
}

/* ================================================================
 * Tests : owl new — 'main' est un nom de fichier ordinaire
 *
 * Depuis la suppression de main.owl comme point d'entree special, 'main'
 * n'est plus un nom reserve. `owl new fenetre main` (etat/classe/collection)
 * genere le squelette normalement, comme n'importe quel autre nom. Plus
 * aucune garde ne rejette 'main'. (Ex-, garde retiree par le Lot B.)
 * ================================================================ */

/* `owl new fenetre main` -> exit 0 + squelette de fenetre sur stdout portant
 * le nom 'main' (traite exactement comme un nom ordinaire). */
static void test_new_fenetre_main_ok(void) {
    char* argv[] = { "owl", "new", "fenetre", "main" };

    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new fenetre main -> exit 0");
    CHECK(strstr(g_buf, "Fenêtre") != NULL || strstr(g_buf, "Fenetre") != NULL,
          "owl new fenetre main -> squelette de fenetre emis");
    CHECK_HAS("main", "le squelette emis porte le nom 'main'");
}

/* Casse : 'Main' et 'MAIN' generent normalement (aucun statut special). */
static void test_new_fenetre_main_case_variants(void) {
    char* argv_cap[] = { "owl", "new", "fenetre", "Main" };
    int rc = capture_call(handle_new_command, 4, argv_cap);
    CHECK_RC(rc, 0, "owl new fenetre Main (capitale) -> exit 0");
    CHECK_HAS("Main", "le squelette emis porte le nom 'Main'");

    char* argv_up[] = { "owl", "new", "fenetre", "MAIN" };
    rc = capture_call(handle_new_command, 4, argv_up);
    CHECK_RC(rc, 0, "owl new fenetre MAIN (majuscules) -> exit 0");
    CHECK_HAS("MAIN", "le squelette emis porte le nom 'MAIN'");
}

/* Les autres types FICHIER acceptent aussi le nom 'main' (meme chemin `name`).
 * (collection n'incorpore pas `name` dans son squelette -> on verifie exit 0.) */
static void test_new_main_ok_for_all_file_types(void) {
    char* etat[]  = { "owl", "new", "etat", "main" };
    int rc = capture_call(handle_new_command, 4, etat);
    CHECK_RC(rc, 0, "owl new etat main -> exit 0");
    CHECK_HAS("main", "owl new etat main -> squelette porte le nom 'main'");

    char* cls[]   = { "owl", "new", "classe", "main" };
    rc = capture_call(handle_new_command, 4, cls);
    CHECK_RC(rc, 0, "owl new classe main -> exit 0");
    CHECK_HAS("main", "owl new classe main -> squelette porte le nom 'main'");

    char* coll[]  = { "owl", "new", "collection", "main" };
    rc = capture_call(handle_new_command, 4, coll);
    CHECK_RC(rc, 0, "owl new collection main -> exit 0");
}

/* Nom normal : generation inchangee, exit 0 + squelette de fenetre sur stdout. */
static void test_new_fenetre_normal_name_ok(void) {
    char* argv[] = { "owl", "new", "fenetre", "Accueil" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new fenetre Accueil -> exit 0");
    CHECK(strstr(g_buf, "Fenêtre") != NULL || strstr(g_buf, "Fenetre") != NULL,
          "owl new fenetre Accueil -> squelette de fenetre emis");
    CHECK_HAS("Accueil", "le squelette emis porte le nom 'Accueil'");
}

/* `champ` est un widget, pas un fichier : un champ nomme 'main' n'est PAS
 * concerne par la garde (kind 'bouton' + name 'main' -> generation normale). */
static void test_new_champ_main_not_guarded(void) {
    char* argv[] = { "owl", "new", "champ", "bouton", "main" };
    int rc = capture_call(handle_new_command, 5, argv);
    CHECK_RC(rc, 0, "owl new champ bouton main -> exit 0 (champ non garde)");
}

/* ================================================================
 * Tests : validation d'identifiant du nom `owl new` — 
 *
 * `owl new <type> <nom>` doit REFUSER un nom qui n'est pas un identifiant
 * OWL legal (regles scanner is_ident_start/is_ident_char : lettres,
 * chiffres non-initiaux, '_', accents). Le tiret est l'operateur
 * soustraction, pas un caractere d'identifiant : un squelette portant
 * `cl-testCompil` est du code casse. La garde s'applique AVANT toute
 * emission de squelette (rien sur stdout) et sort non-zero.
 * ================================================================ */

/* Tiret dans le nom -> rejet : exit non-zero, AUCUN squelette sur stdout. */
static void test_new_rejects_hyphen_name(void) {
    char* argv[] = { "owl", "new", "classe", "cl-testCompil" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK(rc != 0, "owl new classe cl-testCompil -> exit non-zero");
    CHECK(strstr(g_buf, "cl-testCompil") == NULL,
          "nom invalide -> aucun squelette (nom absent de stdout)");
    CHECK(strstr(g_buf, "Classe") == NULL,
          "nom invalide -> aucun squelette de classe sur stdout");
}

/* Chiffre en premiere position -> rejet (is_ident_start exclut les chiffres). */
static void test_new_rejects_digit_initial(void) {
    char* argv[] = { "owl", "new", "classe", "2mauvais" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK(rc != 0, "owl new classe 2mauvais -> exit non-zero");
    CHECK(strstr(g_buf, "Classe") == NULL,
          "chiffre initial -> aucun squelette sur stdout");
}

/* Espace / ponctuation -> rejet (l'espace n'est pas un caractere d'ident). */
static void test_new_rejects_space_name(void) {
    char* argv[] = { "owl", "new", "fenetre", "ma fenetre" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK(rc != 0, "owl new fenetre 'ma fenetre' -> exit non-zero");
}

/* La garde couvre TOUS les types file (fenetre/etat/collection), pas que classe. */
static void test_new_rejects_hyphen_all_file_types(void) {
    char* fen[]  = { "owl", "new", "fenetre", "ma-fen" };
    int rc = capture_call(handle_new_command, 4, fen);
    CHECK(rc != 0, "owl new fenetre ma-fen -> exit non-zero");

    char* etat[] = { "owl", "new", "etat", "mon-etat" };
    rc = capture_call(handle_new_command, 4, etat);
    CHECK(rc != 0, "owl new etat mon-etat -> exit non-zero");

    char* coll[] = { "owl", "new", "collection", "ma-coll" };
    rc = capture_call(handle_new_command, 4, coll);
    CHECK(rc != 0, "owl new collection ma-coll -> exit non-zero");
}

/* Le sous-nom `champ` (type de widget) N'EST PAS un identifiant user : il
 * garde sa propre resolution. Seul le `name` du champ est valide comme
 * identifiant. Un nom de champ avec tiret -> rejet ; un widget legal + nom
 * legal -> exit 0 (non-regression). */
static void test_new_champ_name_validated_sub_not(void) {
    /* name du champ avec tiret -> rejet */
    char* bad[] = { "owl", "new", "champ", "bouton", "BTN-OK" };
    int rc = capture_call(handle_new_command, 5, bad);
    CHECK(rc != 0, "owl new champ bouton BTN-OK -> exit non-zero (name invalide)");

    /* widget legal + nom legal -> generation normale */
    char* ok[] = { "owl", "new", "champ", "bouton", "BTN_OK" };
    rc = capture_call(handle_new_command, 5, ok);
    CHECK_RC(rc, 0, "owl new champ bouton BTN_OK -> exit 0 (nom legal)");
}

/* Nom legal accentue -> accepte (accents = caracteres d'identifiant OWL). */
static void test_new_accepts_accented_name(void) {
    char* argv[] = { "owl", "new", "classe", "Mod\xc3\xa8le" };  /* "Modèle" UTF-8 */
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new classe Modèle -> exit 0 (accents legaux)");
    CHECK(strstr(g_buf, "Mod\xc3\xa8le") != NULL,
          "le squelette emis porte le nom accentue 'Modèle'");
    CHECK(strstr(g_buf, "Classe") != NULL,
          "squelette de classe emis pour un nom accentue");
}

/* Nom legal avec chiffre non-initial + underscore -> accepte. */
static void test_new_accepts_legal_name_with_digit_and_underscore(void) {
    char* argv[] = { "owl", "new", "classe", "cMoteur_v2" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new classe cMoteur_v2 -> exit 0");
    CHECK_HAS("cMoteur_v2", "le squelette porte le nom 'cMoteur_v2'");
}

/* ================================================================
 * Tests : owl new service — 
 *
 * `owl new service <Nom>` emet sur stdout un squelette `ServiceREST` public
 * (sans Authentification -> aucun warning), avec UrlBase + une operation de
 * lecture parametree (Chemin = [..., id]) bindee dans surRequête(id) et une
 * reponse via MoiMême..Réponse..CodeÉtat. La sortie doit compiler proprement
 * ({"errors":[],"warnings":[]}) et etre un point fixe du formateur. Le DSL
 * de service est bilingue : --lang en emet les formes EN canon.
 * ================================================================ */

/* FR : scaffold ServiceREST, compile clean (0 erreur, 0 warning) + canonique. */
static void test_new_service_fr_compiles_clean(void) {
    enter_sandbox_dir("_owl_new_test_service_fr");

    char* argv[] = { "owl", "new", "service", "Articles" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new service Articles -> exit 0");

    char* scaffold = strdup(g_buf);
    CHECK(scaffold != NULL, "owl new service Articles -> squelette capture");
    if (scaffold) {
        CHECK(strstr(scaffold, "Articles est un ServiceREST") != NULL,
              "le squelette declare 'Articles est un ServiceREST'");
        CHECK(strstr(scaffold, "est une Op\xc3\xa9rationHTTP") != NULL,
              "le squelette contient une OpérationHTTP");
        CHECK(strstr(scaffold, "UrlBase") != NULL,
              "le squelette contient la propriete UrlBase");
        CHECK(strstr(scaffold, "surRequ\xc3\xaate(id est un entier)") != NULL,
              "le handler est surRequête(id est un entier)");
        CHECK(strstr(scaffold, "MoiM\xc3\xaame..R\xc3\xa9ponse..Code\xc3\x89tat = httpOK") != NULL,
              "la reponse passe par MoiMême..Réponse..CodeÉtat = httpOK");
        CHECK(strstr(scaffold, "Authentification") == NULL,
              "service public : PAS de bloc Authentification (pas de warning)");

        write_file("Articles.owl", scaffold);
    }

    char* checkArgv[] = { "owl", "Articles.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "Articles.owl genere compile (owl --check)");
    CHECK_HAS("\"errors\":[]", "aucune erreur de compilation");
    CHECK_HAS("\"warnings\":[]", "aucun avertissement (service public)");

    char* fmtArgv[] = { "owl", "fmt", "--check", "Articles.owl" };
    int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
    CHECK_RC(rcFmt, 0, "Articles.owl genere est deja canonique (owl fmt --check)");

    free(scaffold);
    owl_unlink("Articles.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_service_fr");
}

/* EN : `--lang en` emet les formes EN canon (HTTPOperation, Method, Path,
 * StatusCode, BaseUrl, onRequest, MySelf..Response..StatusCode) ; compile clean +
 * canonique, aucune forme FR ne subsiste. */
static void test_new_service_en_compiles_clean(void) {
    enter_sandbox_dir("_owl_new_test_service_en");

    int prevEn = owl_error_render_lang_is_en();
    owl_error_render_set_lang_en(1);

    char* argv[] = { "owl", "new", "service", "Articles" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new service Articles --lang en -> exit 0");

    /* La generation EN passe par vm_init()/vm_free() (canon EN lu dans le
     * registre) : ca libere le vm partage du harnais. On le re-initialise
     * pour que le check_shim (no_vm_lifecycle) retrouve un registre peuple.
     * compiler_reset_poo_state() vide le registre de DECLARATION ServiceREST
     * (compile-unit) pollue par le check FR precedent dans ce meme process :
     * meme isolation que testing.c entre deux compilations independantes. Un
     * `owl --check` reel est one-shot, donc jamais concerne. */
    vm_init();
    compiler_reset_poo_state();

    char* scaffold = strdup(g_buf);
    CHECK(scaffold != NULL, "owl new service (lang en) -> squelette capture");
    if (scaffold) {
        CHECK(strstr(scaffold, "Articles is a ServiceREST") != NULL,
              "le squelette declare 'Articles is a ServiceREST'");
        CHECK(strstr(scaffold, "HTTPOperation") != NULL,
              "forme EN canon 'HTTPOperation'");
        CHECK(strstr(scaffold, "BaseUrl") != NULL,
              "forme EN canon 'BaseUrl'");
        CHECK(strstr(scaffold, "onRequest(id is an integer)") != NULL,
              "handler EN 'onRequest(id is an integer)'");
        CHECK(strstr(scaffold, "MySelf..Response..StatusCode = httpOK") != NULL,
              "reponse EN 'MySelf..Response..StatusCode = httpOK'");
        CHECK(strstr(scaffold, "Op\xc3\xa9rationHTTP") == NULL,
              "aucune forme FR 'OpérationHTTP' ne subsiste");
        CHECK(strstr(scaffold, "MoiM\xc3\xaame") == NULL,
              "aucune forme FR 'MoiMême' ne subsiste");

        write_file("Articles.owl", scaffold);
    }

    char* checkArgv[] = { "owl", "Articles.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "Articles.owl (lang en) genere compile (owl --check)");
    CHECK_HAS("\"errors\":[]", "aucune erreur de compilation (en)");
    CHECK_HAS("\"warnings\":[]", "aucun avertissement (service public, en)");

    char* fmtArgv[] = { "owl", "fmt", "--check", "Articles.owl" };
    int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
    CHECK_RC(rcFmt, 0, "Articles.owl (lang en) est deja canonique (owl fmt --check)");

    owl_error_render_set_lang_en(prevEn);
    free(scaffold);
    owl_unlink("Articles.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_service_en");
}

/* ================================================================
 * Tests : owl new menuloupe — 
 *
 * `owl new menuloupe <Nom>` emet sur stdout un bloc MenuContextuel pret a
 * attacher a un Champ Table via la propriete MenuLoupe. Les options sont
 * deja cablees sur MoiMême (la colonne cliquee dans le surSélection) :
 * TableSaisieLoupe / TableActiveFiltre / TableDésactiveFiltre. Un commentaire
 * montre l'attache. Le bloc n'etant pas compilable au top-level (il vit dans
 * une Fenetre), on l'entoure d'une Fenetre minimale pour la compile-check.
 * ================================================================ */

/* FR : scaffold MenuContextuel de loupe, options cablees sur MoiMême,
 * commentaire d'attache, compile clean une fois place dans une Fenetre. */
static void test_new_menuloupe_fr_compiles_clean(void) {
    enter_sandbox_dir("_owl_new_test_menuloupe_fr");

    char* argv[] = { "owl", "new", "menuloupe", "MNU_Test" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new menuloupe MNU_Test -> exit 0");

    char* scaffold = strdup(g_buf);
    CHECK(scaffold != NULL, "owl new menuloupe -> squelette capture");
    if (scaffold) {
        CHECK(strstr(scaffold, "MNU_Test est un MenuContextuel") != NULL,
              "le squelette declare 'MNU_Test est un MenuContextuel'");
        CHECK(strstr(scaffold, "MoiM\xc3\xaame") != NULL,
              "les options sont cablees sur MoiMême");
        CHECK(strstr(scaffold, "TableSaisieLoupe(MoiM\xc3\xaame)") != NULL,
              "option Rechercher -> TableSaisieLoupe(MoiMême)");
        CHECK(strstr(scaffold,
                     "TableActiveFiltre(MoiM\xc3\xaame, filtreCommencePar,") != NULL,
              "option Filtrer -> TableActiveFiltre(MoiMême, filtreCommencePar, ...)");
        CHECK(strstr(scaffold, "TableD\xc3\xa9sactiveFiltre(MoiM\xc3\xaame)") != NULL,
              "option Effacer -> TableDésactiveFiltre(MoiMême)");
        CHECK(strstr(scaffold, "MenuLoupe = MNU_Test") != NULL,
              "commentaire d'attache 'MenuLoupe = MNU_Test'");
        CHECK(count_occ(scaffold, "est une OptionMenu") == 3,
              "le squelette contient 3 options");
        CHECK(strstr(scaffold, "WLan" "gage") == NULL
              && strstr(scaffold, "Win" "Dev") == NULL
              && strstr(scaffold, "PC" "SOFT") == NULL,
              "aucune mention interne (regle 7)");

        /* Le squelette seul est deja canonique (owl_emit_canonical). */
        write_file("MNU_Test.owl", scaffold);
        char* fmtArgv[] = { "owl", "fmt", "--check", "MNU_Test.owl" };
        int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
        CHECK_RC(rcFmt, 0, "le squelette menuloupe est deja canonique (owl fmt --check)");
        owl_unlink("MNU_Test.owl");

        /* Bloc non top-level : entoure d'une Fenetre pour la compile-check. */
        char wrapped[8192];
        snprintf(wrapped, sizeof wrapped,
            "FEN_Loupe est une Fen\xc3\xaatre\n"
            "\tPROPRIETES\n"
            "\t\tTitre       = \"T\"\n"
            "\t\tDisposition = dispLibre\n"
            "\tFIN\n\n"
            "%s\n"
            "FIN\n", scaffold);
        write_file("FEN_Loupe.owl", wrapped);
    }

    char* checkArgv[] = { "owl", "FEN_Loupe.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "FEN_Loupe.owl (menuloupe dans une Fenetre) compile (owl --check)");
    CHECK_HAS("\"errors\":[]", "aucune erreur de compilation");

    free(scaffold);
    owl_unlink("FEN_Loupe.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_menuloupe_fr");
}

/* ================================================================
 * Tests : owl new menucorps — 
 *
 * `owl new menucorps <Nom>` emet sur stdout un bloc MenuContextuel pret a
 * attacher au corps d'un Champ Table via la propriete canon MenuContextuel.
 * Les options sont deja cablees sur MoiMême (= la Table) dans surSélection :
 * Tout copier -> TableVersPressePapier(MoiMême), Exporter vers Excel ->
 * fSélecteur (mode sauvegarde fselCrée) puis TableVersExcel(MoiMême, chemin).
 * Un commentaire montre l'attache. Non compilable au top-level (vit dans une
 * Fenetre) : on l'entoure d'une Fenetre minimale pour la compile-check.
 * ================================================================ */

/* FR : scaffold MenuContextuel de corps, options cablees sur MoiMême,
 * commentaire d'attache, compile clean une fois place dans une Fenetre. */
static void test_new_menucorps_fr_compiles_clean(void) {
    enter_sandbox_dir("_owl_new_test_menucorps_fr");

    char* argv[] = { "owl", "new", "menucorps", "MNU_Test" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new menucorps MNU_Test -> exit 0");

    char* scaffold = strdup(g_buf);
    CHECK(scaffold != NULL, "owl new menucorps -> squelette capture");
    if (scaffold) {
        CHECK(strstr(scaffold, "MNU_Test est un MenuContextuel") != NULL,
              "le squelette declare 'MNU_Test est un MenuContextuel'");
        CHECK(strstr(scaffold, "TableVersPressePapier(MoiM\xc3\xaame)") != NULL,
              "option Tout copier -> TableVersPressePapier(MoiMême)");
        CHECK(strstr(scaffold, "fS\xc3\xa9lecteur(") != NULL,
              "option Excel -> chemin via fSélecteur");
        CHECK(strstr(scaffold, "fselCr\xc3\xa9" "e") != NULL,
              "fSélecteur en mode sauvegarde (fselCrée)");
        CHECK(strstr(scaffold, "TableVersExcel(MoiM\xc3\xaame, chemin)") != NULL,
              "option Excel -> TableVersExcel(MoiMême, chemin)");
        CHECK(strstr(scaffold, "MenuContextuel = MNU_Test") != NULL,
              "commentaire d'attache 'MenuContextuel = MNU_Test'");
        CHECK(count_occ(scaffold, "est une OptionMenu") == 2,
              "le squelette contient 2 options");
        CHECK(strstr(scaffold, "WLan" "gage") == NULL
              && strstr(scaffold, "Win" "Dev") == NULL
              && strstr(scaffold, "PC" "SOFT") == NULL,
              "aucune mention interne (regle 7)");

        /* Le squelette seul est deja canonique (owl_emit_canonical). */
        write_file("MNU_Test.owl", scaffold);
        char* fmtArgv[] = { "owl", "fmt", "--check", "MNU_Test.owl" };
        int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
        CHECK_RC(rcFmt, 0, "le squelette menucorps est deja canonique (owl fmt --check)");
        owl_unlink("MNU_Test.owl");

        /* Bloc non top-level : entoure d'une Fenetre avec une Table pour la
         * compile-check (les options referencent MoiMême = la Table). */
        char wrapped[8192];
        snprintf(wrapped, sizeof wrapped,
            "FEN_Corps est une Fen\xc3\xaatre\n"
            "\tPROPRIETES\n"
            "\t\tTitre       = \"T\"\n"
            "\t\tDisposition = dispLibre\n"
            "\tFIN\n\n"
            "\tTABLE_Donnees est un Champ Table\n"
            "\t\tPROPRIETES\n"
            "\t\t\tMenuContextuel = MNU_Test\n"
            "\t\tFIN\n\n"
            "\t\tCOL_Nom est une Colonne\n"
            "\t\t\tPROPRIETES\n"
            "\t\t\t\tTitre = \"Nom\"\n"
            "\t\t\tFIN\n"
            "\t\tFIN\n"
            "\tFIN\n\n"
            "%s\n"
            "FIN\n", scaffold);
        write_file("FEN_Corps.owl", wrapped);
    }

    char* checkArgv[] = { "owl", "FEN_Corps.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "FEN_Corps.owl (menucorps dans une Fenetre) compile (owl --check)");
    CHECK_HAS("\"errors\":[]", "aucune erreur de compilation");

    free(scaffold);
    owl_unlink("FEN_Corps.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_menucorps_fr");
}

/* ================================================================
 * Tests : owl new projet / project — 
 *
 * `owl new projet` (FR) / `owl new project` (EN) pose un fichier marqueur
 * de projet dans le dossier COURANT s'il n'existe pas encore, avec un
 * prefill minimal VALIDE (Langues obligatoire, cf. ). L'argument
 * kind est bilingue et casse-insensible ; il choisit la GRAPHIE du fichier
 * genere (projet.owl vs project.owl), independamment de la langue du
 * contenu (qui suit --lang, FR par defaut — cf. , marqueur bilingue
 * pur fichier). ROUGE AUJOURD'HUI : "projet"/"project" n'est pas un kind
 * reconnu par owl_new_generate_ex -> owl_new_generate_ex retourne -1 ->
 * handle_new_command imprime "type '...' inconnu" et sort en erreur, sans
 * jamais ecrire de fichier.
 * ================================================================ */

/* Creation FR : `owl new projet MonApp` dans un dossier vide -> exit 0,
 * `projet.owl` cree, contenu compile ET passe fmt --check (canonique). */
static void test_new_projet_fr_creates_and_compiles(void) {
    enter_sandbox_dir("_owl_new_test_projet_fr");

    char* argv[] = { "owl", "new", "projet", "MonApp" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new projet MonApp -> exit 0");
    CHECK(file_exists("projet.owl"), "owl new projet MonApp -> projet.owl cree");

    char* content = slurp("projet.owl");
    CHECK(content != NULL, "projet.owl lisible");
    if (content) {
        CHECK(strstr(content, "MonApp") != NULL,
              "projet.owl contient l'identifiant 'MonApp'");
        CHECK(strstr(content, "Langues") != NULL,
              "projet.owl contient la propriete 'Langues' (obligatoire, fix/0882)");
        CHECK(strstr(content, "langueFran\xc3\xa7" "ais") != NULL,
              "projet.owl declare 'langueFrançais' comme langue par defaut");
        /*  — template complet : toutes les props du catalogue, meme
         * vides. */
        CHECK(strstr(content, "Description = \"\"") != NULL,
              "projet.owl contient 'Description = \"\"' (props vides du catalogue)");
        CHECK(strstr(content, "Auteur      = \"\"") != NULL,
              "projet.owl contient 'Auteur = \"\"' (aligne canonique)");
        CHECK(strstr(content, "Licence     = \"\"") != NULL,
              "projet.owl contient 'Licence = \"\"' (aligne canonique)");
        free(content);
    }

    char* checkArgv[] = { "owl", "projet.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "projet.owl genere compile (owl --check)");

    char* fmtArgv[] = { "owl", "fmt", "--check", "projet.owl" };
    int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
    CHECK_RC(rcFmt, 0, "projet.owl genere est deja canonique (owl fmt --check)");

    owl_unlink("projet.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_fr");
}

/* Creation EN : `owl new project MonApp` -> genere `project.owl` (graphie
 * EN), PAS `projet.owl`. Le contenu reste FR par defaut (pas de --lang en) :
 * seule la graphie du marqueur suit l'argument ( est un contrat
 * purement fichier). */
static void test_new_project_en_creates_marker(void) {
    enter_sandbox_dir("_owl_new_test_project_en");

    char* argv[] = { "owl", "new", "project", "MonApp" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new project MonApp -> exit 0");
    CHECK(file_exists("project.owl"), "owl new project MonApp -> project.owl cree (graphie EN)");
    CHECK(!file_exists("projet.owl"), "owl new project MonApp -> pas de projet.owl (graphie FR)");

    char* content = slurp("project.owl");
    CHECK(content != NULL, "project.owl lisible");
    if (content) {
        CHECK(strstr(content, "MonApp") != NULL,
              "project.owl contient l'identifiant 'MonApp'");
        CHECK(strstr(content, "Langues") != NULL,
              "project.owl contient la propriete 'Langues' (obligatoire)");
        free(content);
    }

    char* checkArgv[] = { "owl", "project.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "project.owl genere compile (owl --check)");

    owl_unlink("project.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_project_en");
}

/*  — Contenu EN : quand la langue de rendu est EN (`--lang en`), le
 * template suit la forme EN : structure `is a Project`/`PROPERTIES`/`END`,
 * champs EN (Author/License), et langue par defaut `langueAnglais` (forme EN
 * `languageEnglish`, tag "en-US") au lieu de langueFrançais. Le fichier genere
 * doit compiler ET etre canonique. On bascule le flag de rendu global (ce que
 * fait `parse_session_flags` sur `--lang en`) puis on restaure. */
static void test_new_projet_lang_en_uses_langueAnglais(void) {
    enter_sandbox_dir("_owl_new_test_projet_lang_en");

    int prevEn = owl_error_render_lang_is_en();
    owl_error_render_set_lang_en(1);

    char* argv[] = { "owl", "new", "project", "MonApp" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new project MonApp --lang en -> exit 0");
    CHECK(file_exists("project.owl"), "owl new project (lang en) -> project.owl cree");

    char* content = slurp("project.owl");
    CHECK(content != NULL, "project.owl (lang en) lisible");
    if (content) {
        CHECK(strstr(content, "is a Project") != NULL,
              "project.owl (lang en) utilise la structure EN 'is a Project'");
        CHECK(strstr(content, "PROPERTIES") != NULL,
              "project.owl (lang en) utilise 'PROPERTIES'");
        CHECK(strstr(content, "Languages   = [languageEnglish]") != NULL,
              "project.owl (lang en) declare 'languageEnglish' comme langue par defaut");
        CHECK(strstr(content, "Author      = \"\"") != NULL,
              "project.owl (lang en) contient le champ EN 'Author'");
        CHECK(strstr(content, "License     = \"\"") != NULL,
              "project.owl (lang en) contient le champ EN 'License'");
        CHECK(strstr(content, "langueFran") == NULL,
              "project.owl (lang en) n'utilise PAS langueFrançais");
        free(content);
    }

    char* checkArgv[] = { "owl", "project.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "project.owl (lang en) genere compile (owl --check)");

    char* fmtArgv[] = { "owl", "fmt", "--check", "project.owl" };
    int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
    CHECK_RC(rcFmt, 0, "project.owl (lang en) est deja canonique (owl fmt --check)");

    owl_error_render_set_lang_en(prevEn);
    owl_unlink("project.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_lang_en");
}

/* Nom omis -> derive du nom du dossier courant, sanitize en identifiant
 * valide. Exemple canon documente par le ticket : dossier "mon projet" ->
 * identifiant "mon_projet" (espace -> underscore). */
static void test_new_projet_default_name_sanitized_from_dirname(void) {
    enter_sandbox_dir("mon projet");

    char* argv[] = { "owl", "new", "projet" };
    int rc = capture_call(handle_new_command, 3, argv);
    CHECK_RC(rc, 0, "owl new projet (sans nom) -> exit 0");
    CHECK(file_exists("projet.owl"),
          "owl new projet (sans nom) -> projet.owl cree malgre l'absence de nom explicite");

    char* content = slurp("projet.owl");
    CHECK(content != NULL, "projet.owl lisible (nom derive du dossier)");
    if (content) {
        CHECK(strstr(content, "mon_projet") != NULL,
              "identifiant derive du dossier 'mon projet' -> 'mon_projet' (espace sanitize)");
        free(content);
    }

    owl_unlink("projet.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("mon projet");
}

/* Argument explicite : `owl new projet <nom>` utilise <nom> tel que fourni
 * par l'utilisateur comme identifiant (pas de re-derivation depuis le
 * dossier). */
static void test_new_projet_explicit_name_overrides_dirname(void) {
    enter_sandbox_dir("_owl_new_test_projet_override");

    char* argv[] = { "owl", "new", "projet", "ProjetPersonnalise" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new projet ProjetPersonnalise -> exit 0");

    char* content = slurp("projet.owl");
    CHECK(content != NULL, "projet.owl lisible");
    if (content) {
        CHECK(strstr(content, "ProjetPersonnalise") != NULL,
              "l'identifiant explicite 'ProjetPersonnalise' prime sur le nom du dossier");
        CHECK(strstr(content, "_owl_new_test_projet_override") == NULL,
              "le nom du dossier sandbox n'apparait pas quand un nom explicite est fourni");
        free(content);
    }

    owl_unlink("projet.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_override");
}

/* Argument explicite NON VALIDE comme identifiant : `owl new projet
 * "mon nom invalide"` (espaces). Le nom explicite doit etre sanitize
 * EXACTEMENT comme le nom derive du dossier -> identifiant 'mon_nom_invalide'.
 * Le fichier genere DOIT compiler (owl --check) et etre canonique (fmt
 * --check). Regression du bug  : le chemin nom-explicite utilisait
 * l'argument brut, produisant un projet.owl non compilable des qu'un nom
 * contient un espace/accent (cas d'usage banal). */
static void test_new_projet_explicit_name_sanitized(void) {
    enter_sandbox_dir("_owl_new_test_projet_sanitize");

    char* argv[] = { "owl", "new", "projet", "mon nom invalide" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new projet \"mon nom invalide\" -> exit 0");
    CHECK(file_exists("projet.owl"), "owl new projet (nom a espaces) -> projet.owl cree");

    char* content = slurp("projet.owl");
    CHECK(content != NULL, "projet.owl lisible");
    if (content) {
        CHECK(strstr(content, "mon_nom_invalide") != NULL,
              "identifiant explicite 'mon nom invalide' sanitize en 'mon_nom_invalide'");
        CHECK(strstr(content, "mon nom invalide") == NULL,
              "le nom brut (avec espaces) n'apparait jamais dans le fichier genere");
        free(content);
    }

    char* checkArgv[] = { "owl", "projet.owl" };
    int rcCheck = capture_call(check_shim, 2, checkArgv);
    CHECK_RC(rcCheck, 0, "projet.owl (nom sanitize) compile (owl --check)");

    char* fmtArgv[] = { "owl", "fmt", "--check", "projet.owl" };
    int rcFmt = capture_call(handle_fmt_command, 4, fmtArgv);
    CHECK_RC(rcFmt, 0, "projet.owl (nom sanitize) est deja canonique (owl fmt --check)");

    owl_unlink("projet.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_sanitize");
}

/* Marqueur deja present (projet.owl OU project.owl, cf. ) -> refus
 * propre, exit non-zero, AUCUNE ecriture (fichier existant inchange).
 *
 * IMPORTANT (piege test-writing-pitfalls #3 cas B) : sans le controle
 * "baseline" ci-dessous, ce test serait VERT AUJOURD'HUI pour la MAUVAISE
 * raison — 'projet' n'etant pas encore un kind reconnu, handle_new_command
 * retourne systematiquement rc!=0 et n'ecrit jamais, refus ou pas. Le
 * controle baseline (aucun marqueur pre-existant -> doit reussir) est ce
 * qui fait rougir ce test pour la BONNE raison (feature absente) tant que
 * l'impl n'existe pas ; les assertions de refus proprement dites resteront
 * vertes par coincidence jusqu'a l'impl, a re-verifier ensuite. */
static void test_new_projet_refuses_when_marker_exists(void) {
    enter_sandbox_dir("_owl_new_test_refus_marqueur");

    static const char* SENTINEL = "// SENTINELLE — ne doit jamais etre ecrasee\n";

    /* Controle baseline : SANS marqueur pre-existant, la commande doit
     * reussir. Sans ce controle, le reste du test est trivialement vert
     * (feature absente = "refus" systematique par accident). */
    char* argvBaseline[] = { "owl", "new", "projet", "Baseline" };
    int rcBaseline = capture_call(handle_new_command, 4, argvBaseline);
    CHECK_RC(rcBaseline, 0,
             "[baseline] owl new projet Baseline sans marqueur pre-existant -> exit 0");
    CHECK(file_exists("projet.owl"),
          "[baseline] projet.owl cree en l'absence de marqueur (precondition du test de refus)");
    owl_unlink("projet.owl");

    /* Cas 1 : projet.owl (meme graphie que la commande) deja present. */
    write_file("projet.owl", SENTINEL);
    char* argv1[] = { "owl", "new", "projet", "Autre" };
    int rc1 = capture_call(handle_new_command, 4, argv1);
    CHECK(rc1 != 0, "owl new projet refuse si projet.owl existe deja -> exit non-zero");
    char* after1 = slurp("projet.owl");
    CHECK(after1 != NULL && strcmp(after1, SENTINEL) == 0,
          "projet.owl existant reste byte-identique apres refus (pas d'ecrasement)");
    free(after1);
    owl_unlink("projet.owl");

    /* Cas 2 : project.owl (graphie EN, cf.  coexistence ambigue)
     * deja present -> `owl new projet` (FR) refuse aussi (meme frontiere
     * de projet, deux graphies interdites simultanement). */
    write_file("project.owl", SENTINEL);
    char* argv2[] = { "owl", "new", "projet", "Autre" };
    int rc2 = capture_call(handle_new_command, 4, argv2);
    CHECK(rc2 != 0, "owl new projet refuse si project.owl (graphie EN) existe deja -> exit non-zero");
    CHECK(!file_exists("projet.owl"),
          "owl new projet ne cree pas projet.owl quand project.owl existe deja");
    char* after2 = slurp("project.owl");
    CHECK(after2 != NULL && strcmp(after2, SENTINEL) == 0,
          "project.owl existant reste byte-identique apres refus croise");
    free(after2);
    owl_unlink("project.owl");

    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_refus_marqueur");
}

/* Argument kind casse-insensible : 'PROJET' / 'Project' reconnus comme
 * 'projet' / 'project'. */
static void test_new_projet_case_insensitive_argument(void) {
    enter_sandbox_dir("_owl_new_test_case_insensitive");

    char* argvFr[] = { "owl", "new", "PROJET", "Foo" };
    int rcFr = capture_call(handle_new_command, 4, argvFr);
    CHECK_RC(rcFr, 0, "owl new PROJET (majuscules) -> exit 0");
    CHECK(file_exists("projet.owl"), "owl new PROJET -> projet.owl cree");
    owl_unlink("projet.owl");

    char* argvEn[] = { "owl", "new", "Project", "Bar" };
    int rcEn = capture_call(handle_new_command, 4, argvEn);
    CHECK_RC(rcEn, 0, "owl new Project (casse mixte) -> exit 0");
    CHECK(file_exists("project.owl"), "owl new Project -> project.owl cree");
    owl_unlink("project.owl");

    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_case_insensitive");
}

/*  — EOL NATIF PLATEFORME pour tout fichier cree de zero.
 *
 * `owl new projet` est le seul type de `owl new` qui ecrit un fichier lui-meme
 * (les 10 autres impriment sur stdout, donc le CRT Windows y traduit deja
 * chaque '\n' en "\r\n"). L'ecriture directe partait en binaire -> LF meme sous
 * Windows : deux chemins, deux EOL, et un projet.owl affiche en une seule ligne
 * dans un editeur ancien.
 *
 * Ce test lit les OCTETS BRUTS du fichier produit (slurp = fopen "rb"), donc
 * l'assertion ne depend d'aucun `.gitattributes` : le fichier est genere a
 * l'instant dans un sandbox, il n'est jamais commite. Attendu : CRLF partout
 * sous Windows, LF nu partout ailleurs, et jamais de CR isole (ni de "\r\r\n",
 * qui trahirait une double traduction). */
static void test_new_projet_eol_natif_plateforme(void) {
    enter_sandbox_dir("_owl_new_test_projet_eol");

    char* argv[] = { "owl", "new", "projet", "MonApp" };
    int rc = capture_call(handle_new_command, 4, argv);
    CHECK_RC(rc, 0, "owl new projet MonApp -> exit 0");

    char* content = slurp("projet.owl");
    CHECK(content != NULL, "projet.owl lisible (octets bruts)");
    if (content) {
        int lf = 0, crlf = 0, cr_isole = 0;
        for (char* p = content; *p; p++) {
            if (*p == '\n') {
                lf++;
                if (p != content && p[-1] == '\r') crlf++;
            } else if (*p == '\r' && p[1] != '\n') {
                cr_isole++;
            }
        }
        CHECK(lf == 8, "projet.owl genere fait 8 fins de ligne");
        CHECK(cr_isole == 0, "projet.owl ne contient aucun CR isole");
#ifdef _WIN32
        CHECK(crlf == lf, "sous Windows, chaque fin de ligne de projet.owl est un CRLF");
        CHECK(crlf == 8, "sous Windows, projet.owl contient exactement 8 CRLF");
#else
        CHECK(crlf == 0, "hors Windows, chaque fin de ligne de projet.owl est un LF nu");
#endif
        free(content);
    }

    /* L'EOL natif ne casse ni la compilation ni la canonicite. */
    char* checkArgv[] = { "owl", "projet.owl" };
    CHECK_RC(capture_call(check_shim, 2, checkArgv), 0,
             "projet.owl en EOL natif compile (owl --check)");
    char* fmtArgv[] = { "owl", "fmt", "--check", "projet.owl" };
    CHECK_RC(capture_call(handle_fmt_command, 4, fmtArgv), 0,
             "projet.owl en EOL natif est deja canonique (owl fmt --check)");

    owl_unlink("projet.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_eol");
}

/*  — meme exigence pour `owl template <nom> -o <fichier>`, le second
 * chemin ou OWL cree un fichier source de zero (le premier etant
 * `owl new projet`). Sans le helper commun, l'export partait en binaire (LF)
 * alors que `owl template <nom> > f.owl` sortait en CRLF sous Windows : deux
 * commandes documentees comme equivalentes, deux resultats differents.
 *
 * Lecture en octets bruts dans un sandbox jamais commite : aucun
 * `.gitattributes` ne peut fausser la mesure. `cr_isole == 0` garde des deux
 * cotes contre un "\r\r\n" de double traduction (le corps du template contient
 * des CR quand la source embarquee est en CRLF). */
static void test_template_export_eol_natif_plateforme(void) {
    enter_sandbox_dir("_owl_template_export_eol");

    static const char* OUT = "export_eol.owl";
    char* argv[] = { "template", "hello", "-o", (char*)OUT };
    int rc = capture_call(cli_doc_template, 4, argv);
    CHECK_RC(rc, 0, "owl template hello -o <fichier> -> exit 0");

    char* content = slurp(OUT);
    CHECK(content != NULL, "template exporte lisible (octets bruts)");
    if (content) {
        int lf = 0, crlf = 0, cr_isole = 0;
        for (char* p = content; *p; p++) {
            if (*p == '\n') {
                lf++;
                if (p != content && p[-1] == '\r') crlf++;
            } else if (*p == '\r' && p[1] != '\n') {
                cr_isole++;
            }
        }
        CHECK(lf == 16, "template hello exporte fait 16 fins de ligne");
        CHECK(cr_isole == 0, "template exporte ne contient aucun CR isole");
#ifdef _WIN32
        CHECK(crlf == lf, "sous Windows, chaque fin de ligne du template exporte est un CRLF");
        CHECK(crlf == 16, "sous Windows, le template exporte contient exactement 16 CRLF");
#else
        CHECK(crlf == 0, "hors Windows, chaque fin de ligne du template exporte est un LF nu");
#endif
        free(content);
    }

    /* L'EOL natif ne casse pas la compilation. Pas d'assertion `fmt --check`
     * ici : le corps du template aligne ses declarations sur des espaces de
     * remplissage que le formateur recompacte, independamment de l'EOL. */
    char* checkArgv[] = { "owl", (char*)OUT };
    CHECK_RC(capture_call(check_shim, 2, checkArgv), 0,
             "template exporte en EOL natif compile (owl --check)");

    owl_unlink(OUT);
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_template_export_eol");
}

/* ================================================================
 *  — `owl new projet --theme <X>` genere un voisin theme.owl
 * compilable (avant : reference de theme pendante -> projet.owl ne
 * compilait pas). Verrouille : les 2 fichiers sont crees, le projet
 * complet compile, refus si theme.owl existe deja.
 * ================================================================ */

/* FR : projet.owl (reference) + theme.owl (instance) crees, projet compile,
 * les deux fichiers restent canoniques (acquis ). */
static void test_new_projet_theme_generates_theme_and_compiles(void) {
    enter_sandbox_dir("_owl_new_test_projet_theme_fr");

    char* argv[] = { "owl", "new", "projet", "MonApp", "--theme", "MonTheme" };
    int rc = capture_call(handle_new_command, 6, argv);
    CHECK_RC(rc, 0, "owl new projet --theme MonTheme -> exit 0");
    CHECK(file_exists("projet.owl"), "--theme -> projet.owl cree");
    CHECK(file_exists("theme.owl"),  "--theme -> voisin theme.owl cree (fix/1328)");

    char* proj = slurp("projet.owl");
    if (proj) {
        CHECK(strstr(proj, "Th\xc3\xa8me") != NULL && strstr(proj, "MonTheme") != NULL,
              "projet.owl reference le theme (Thème = MonTheme)");
        free(proj);
    }
    char* thm = slurp("theme.owl");
    if (thm) {
        CHECK(strstr(thm, "MonTheme") != NULL && strstr(thm, "Th\xc3\xa8me") != NULL,
              "theme.owl declare l'instance 'MonTheme est un Thème'");
        free(thm);
    }

    /* Le coeur du fix : le projet complet (projet.owl + voisin theme.owl)
     * compile. La resolution cross-file du champ Theme voit theme.owl. */
    char* checkArgv[] = { "owl", "projet.owl" };
    CHECK_RC(capture_call(check_shim, 2, checkArgv), 0,
             "projet --theme compile : reference theme resolue via theme.owl");

    char* fmtP[] = { "owl", "fmt", "--check", "projet.owl" };
    CHECK_RC(capture_call(handle_fmt_command, 4, fmtP), 0,
             "projet.owl (--theme) reste canonique");
    char* fmtT[] = { "owl", "fmt", "--check", "theme.owl" };
    CHECK_RC(capture_call(handle_fmt_command, 4, fmtT), 0,
             "theme.owl genere est canonique");

    owl_unlink("projet.owl");
    owl_unlink("theme.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_theme_fr");
}

/* EN (--lang en) : project.owl + theme.owl en graphie EN, projet compile. */
static void test_new_project_theme_en_generates_theme_and_compiles(void) {
    enter_sandbox_dir("_owl_new_test_project_theme_en");
    int prevEn = owl_error_render_lang_is_en();
    owl_error_render_set_lang_en(1);

    char* argv[] = { "owl", "new", "project", "MyApp", "--theme", "MyTheme" };
    int rc = capture_call(handle_new_command, 6, argv);
    CHECK_RC(rc, 0, "owl new project --theme MyTheme (lang en) -> exit 0");
    CHECK(file_exists("project.owl"), "--theme (en) -> project.owl cree");
    CHECK(file_exists("theme.owl"),   "--theme (en) -> voisin theme.owl cree");

    char* thm = slurp("theme.owl");
    if (thm) {
        CHECK(strstr(thm, "MyTheme") != NULL && strstr(thm, "is a Theme") != NULL,
              "theme.owl (en) declare 'MyTheme is a Theme'");
        free(thm);
    }
    char* checkArgv[] = { "owl", "project.owl" };
    CHECK_RC(capture_call(check_shim, 2, checkArgv), 0,
             "project --theme (en) compile : reference resolue via theme.owl");

    owl_error_render_set_lang_en(prevEn);
    owl_unlink("project.owl");
    owl_unlink("theme.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_project_theme_en");
}

/* Refus atomique : si theme.owl existe deja, `--theme` refuse (rc != 0) et ne
 * clobbere NI theme.owl NI ne laisse un projet.owl orphelin. */
static void test_new_projet_theme_refuses_when_theme_exists(void) {
    enter_sandbox_dir("_owl_new_test_projet_theme_refuse");

    write_file("theme.owl", "// theme utilisateur pre-existant\n");
    char* argv[] = { "owl", "new", "projet", "MonApp", "--theme", "MonTheme" };
    int rc = capture_call(handle_new_command, 6, argv);
    CHECK(rc != 0, "--theme refuse si theme.owl existe deja (exit non-zero)");
    CHECK(!file_exists("projet.owl"),
          "refus atomique : pas de projet.owl orphelin ecrit");
    char* thm = slurp("theme.owl");
    if (thm) {
        CHECK(strstr(thm, "pre-existant") != NULL,
              "theme.owl utilisateur non clobbere");
        free(thm);
    }

    owl_unlink("theme.owl");
    leave_sandbox_dir();
    remove_sandbox_dir("_owl_new_test_projet_theme_refuse");
}

/* ================================================================
 * Sandbox : survie du menage a une interruption
 * ================================================================ */

static bool dir_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return (st.st_mode & S_IFDIR) != 0;
}

/* Le bac a sable doit disparaitre MEME s'il n'est pas vide.
 *
 * Mesure a l'origine du verrou : `.owl_cli_ops_sandbox_26188/` trainait a la
 * racine du depot, contenant `_owl_new_test_projet_theme_fr/`. Un arret entre
 * enter_sandbox_dir() et remove_sandbox_dir() saute le retrait du dossier de
 * travail ; le menage de fin, non recursif, echoue alors en silence sur un
 * parent non vide. Chaque interruption laisse un dossier de plus derriere
 * elle — meme classe de defaut que les orphelins du dossier temporaire.
 *
 * On seme le residu qu'une interruption aurait laisse, puis on exige que le
 * menage emporte quand meme la racine. */
static void test_sandbox_root_removed_even_when_not_empty(void) {
    char root[512];
    snprintf(root, sizeof(root), "%s", sandbox_root());

    char residu[640];
    snprintf(residu, sizeof(residu), "%s/_owl_interrompu", root);
    owl_mkdir(residu);
    char fichier[768];
    snprintf(fichier, sizeof(fichier), "%s/projet.owl", residu);
    write_file(fichier, "// residu d'un test interrompu\n");
    CHECK(file_exists(fichier), "residu d'interruption bien seme dans la sandbox");

    sandbox_wipe();

    CHECK(!dir_exists(root),
          "la racine de sandbox disparait malgre le residu d'une interruption");
}

/* ================================================================
 * Entry point
 * ================================================================ */

/* Ordre d'execution — verrou d'independance
 *
 * Le corpus a heberge une dependance cachee a l'ordre : les deux cas qui
 * lancent une VRAIE campagne `owl test` in-process laissaient le process sans
 * registre, et le premier `--check` place APRES eux rougissait. Le symptome
 * accusait un test sain — un faux rouge attribue a un innocent coute le temps
 * de le chercher, puis celui de comprendre qu'on cherchait au mauvais endroit.
 *
 * Le defaut est ferme cote source (`run_tests_ex` rend la main avec un
 * registre peuple). Reste a EMPECHER LA RECHUTE : la liste passe par une table
 * et l'ordre est melangeable a la demande.
 *
 *     OWL_CLI_OPS_MELANGE=<graine> owl --self-test-cli-ops
 *
 * Sans la variable, l'ordre est celui de la table (reproductible, verdict
 * stable). Avec, la permutation est deterministe pour une graine donnee : un
 * rouge se rejoue a l'identique. Le verdict doit etre le meme pour toute
 * graine — c'est le critere, pas le vert dans une configuration particuliere.
 */
typedef struct {
    const char* nom;
    void (*fn)(void);
} CasCliOps;

/* Permutation de Fisher-Yates sur un generateur congruentiel local : pas de
 * rand() global, dont l'etat serait partage avec le code sous test. */
static void melanger(int* idx, int n, unsigned graine) {
    unsigned etat = graine ? graine : 1u;
    for (int i = n - 1; i > 0; i--) {
        etat = etat * 1103515245u + 12345u;
        int j = (int)((etat >> 16) % (unsigned)(i + 1));
        int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
    }
}

#define CAS(f) { #f, f }
static const CasCliOps g_cas[] = {
    CAS(test_fmt_no_arg_is_noop_exit64),
    CAS(test_fmt_target_exit0),
    CAS(test_fmt_rejects_non_owl_extension),
    CAS(test_fmt_two_targets_refused_exit64),
    CAS(test_list_keywords_has_reserved),
    CAS(test_check_valid_file_exit0),
    CAS(test_check_invalid_file_nonzero),
    CAS(test_check_reports_broken_neighbor),
    CAS(test_check_global_used_by_test_neighbor_not_flagged),
    CAS(test_check_element_global_used_by_test_neighbor_not_flagged),
    CAS(test_check_lone_file_still_reports_unused),
    CAS(test_check_projet_service_no_false_duplicate),
    CAS(test_check_projet_service_real_duplicate_detected),
    CAS(test_check_callback_injecte_par_test_resolu),
    CAS(test_check_callback_vraiment_inconnu_toujours_remonte),
    CAS(test_check_util_attr_fr_bare_suppresses_warning),
    CAS(test_check_util_attr_fr_valeur_suppresses_warning),
    CAS(test_check_util_attr_en_bare_suppresses_warning),
    CAS(test_check_util_attr_en_valeur_suppresses_warning),
    CAS(test_check_util_attr_case_insensitive),
    CAS(test_check_util_attr_with_init_compiles),
    CAS(test_check_util_attr_param_fr_suppresses_warning),
    CAS(test_check_util_attr_param_en_suppresses_warning),
    CAS(test_check_util_attr_param_method_fr_suppresses_warning),
    CAS(test_check_util_attr_param_method_en_suppresses_warning),
    CAS(test_test_two_paths_refused_exit64),
    CAS(test_test_three_paths_refused_exit64),
    CAS(test_test_unknown_option_refused_exit64),
    /* Les deux seuls cas du corpus qui executent reellement une suite `owl
     * test` in-process. Leur place ici est celle de leur SUJET (les commandes
     * `owl test`), plus une consigne d'ordre : cf. le commentaire au-dessus. */
    CAS(test_test_warning_names_its_own_file),
    CAS(test_element_global_used_by_neighbor_not_flagged),
    CAS(test_strip_bom_at_head_removes_three_bytes),
    CAS(test_strip_bom_absent_is_noop),
    CAS(test_strip_bom_in_middle_not_stripped),
    CAS(test_strip_bom_short_string),
    CAS(test_new_fenetre_main_ok),
    CAS(test_new_fenetre_main_case_variants),
    CAS(test_new_main_ok_for_all_file_types),
    CAS(test_new_fenetre_normal_name_ok),
    CAS(test_new_champ_main_not_guarded),
    CAS(test_new_rejects_hyphen_name),
    CAS(test_new_rejects_digit_initial),
    CAS(test_new_rejects_space_name),
    CAS(test_new_rejects_hyphen_all_file_types),
    CAS(test_new_champ_name_validated_sub_not),
    CAS(test_new_accepts_accented_name),
    CAS(test_new_accepts_legal_name_with_digit_and_underscore),
    CAS(test_new_service_fr_compiles_clean),
    CAS(test_new_service_en_compiles_clean),
    CAS(test_new_menuloupe_fr_compiles_clean),
    CAS(test_new_menucorps_fr_compiles_clean),
    CAS(test_new_projet_fr_creates_and_compiles),
    CAS(test_new_project_en_creates_marker),
    CAS(test_new_projet_lang_en_uses_langueAnglais),
    CAS(test_new_projet_default_name_sanitized_from_dirname),
    CAS(test_new_projet_explicit_name_overrides_dirname),
    CAS(test_new_projet_explicit_name_sanitized),
    CAS(test_new_projet_refuses_when_marker_exists),
    CAS(test_new_projet_case_insensitive_argument),
    CAS(test_new_projet_eol_natif_plateforme),
    CAS(test_template_export_eol_natif_plateforme),
    /*  — variante --theme (genere le voisin theme.owl). */
    CAS(test_new_projet_theme_generates_theme_and_compiles),
    CAS(test_new_project_theme_en_generates_theme_and_compiles),
    CAS(test_new_projet_theme_refuses_when_theme_exists),
    CAS(test_sandbox_root_removed_even_when_not_empty),
};
#undef CAS

int cli_ops_self_test_run(void) {
    fprintf(stderr, "Running CLI ops self-tests...\n");

    int n = (int)(sizeof(g_cas) / sizeof(g_cas[0]));
    int idx[sizeof(g_cas) / sizeof(g_cas[0])];
    for (int i = 0; i < n; i++) idx[i] = i;

    const char* graineTxt = getenv("OWL_CLI_OPS_MELANGE");
    if (graineTxt != NULL && graineTxt[0] != '\0') {
        unsigned graine = (unsigned)strtoul(graineTxt, NULL, 10);
        melanger(idx, n, graine);
        fprintf(stderr, "  ordre melange (graine %u) — le verdict doit etre "
                        "identique a l'ordre nominal\n", graine);
    }

    for (int i = 0; i < n; i++) {
        int avant = g_fail;
        g_cas[idx[i]].fn();
        if (g_fail > avant) {
            fprintf(stderr, "  ^ dans %s (position %d)\n", g_cas[idx[i]].nom, i);
        }
    }

    if (g_fail > 0) {
        fprintf(stderr, "  %d/%d tests FAILED\n", g_fail, g_pass + g_fail);
    } else {
        fprintf(stderr, "  All %d tests PASSED\n", g_pass);
    }
    /* Garde ensemble vide : "All 0 tests PASSED" est un vert qui ne
     * prouve rien, et il ne gene personne — donc personne ne le cherche. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "  CORPUS VIDE : 0 cas execute. Un controle qui "
                        "n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
