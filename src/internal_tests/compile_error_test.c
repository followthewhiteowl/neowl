// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test compile-error fixtures ( ; tasklist ).
 *
 * Porte run_compile_error_tests.sh en in-process. Pour chaque fixture de
 * tests/compile-error/, lance le meme chemin de validation compile-only que
 * le script (`owl --check` sur un fichier isole) et compare la sortie JSON au
 * snapshot .expected.json.
 *
 * Deux layouts (identiques au script) :
 *   1. FLAT     — <name>.owl + <name>.expected.json. Le .owl est checke seul.
 *   2. SUB-DIR  — <name>/ contenant >=2 .owl, l'un ayant un .expected.json
 *                 voisin = ENTRY ; les autres = companions (resolus par le
 *                 scan du dossier parent du compilateur).
 *
 * Isolation : on copie la/les fixture(s) dans un workdir temp dedie qui ne
 * contient QUE ces fichiers, puis on appelle handle_check_command_no_vm_lifecycle
 * (qui scanne dirname = ce workdir). Reproduit exactement la semantique du
 * script (qui copiait dans un mktemp), sans changer le verdict.
 *
 * SECURITE CONTEXTE : les fixtures sont du code OWL volontairement casse
 * (chaines non terminees, bytes inattendus). Le contenu source et la sortie
 * JSON restent EN MEMOIRE C — comparaison strcmp interne. On n'imprime que
 * des compteurs + des NOMS de fixtures. Copie binaire-safe (fread/fwrite par
 * longueur, jamais strlen : une fixture peut contenir des null bytes).
 *
 * Extension  (patron de non-vacuite, pose par ) : le harnais
 * doit PROUVER qu'il a examine quelque chose. Trois regles, dans cet ordre :
 *   1. Tout `<stem>.expected.json` DOIT avoir son entree `<stem>.owl` — au
 *      niveau FLAT comme dans un SUB-DIR. Un snapshot orphelin n'est jamais
 *      apparie, donc jamais compare : il ment sur la couverture. La boucle
 *      n'iterait que les `.owl`, ce cas etait totalement muet.
 *   2. Un SUB-DIR a exactement UNE entree (un seul `.owl` avec snapshot
 *      voisin). Deux entrees = la 2e etait silencieusement ignoree.
 *   3. Corpus vide ou dossier absent = ECHEC, plus un skip vert.
 * Le bilan affiche le nombre de cas reellement executes (flat + sous-dossiers)
 * et le nombre de compagnons — un fichier ecarte du corpus doit se voir.
 *
 * Branche sur `owl --self-test-compile-error` (cf src/main.c), gate make all.
 * Retour : 0 si tout passe, sinon le nombre d'echecs. */

#include "project.h"    /* is_directory */
#include "vm.h"         /* vm_init / vm_free : VM fraiche par fixture */
#include "compiler.h"   /* compiler_reset_poo_state : reset tables POO compile-time */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <time.h>

#ifdef _WIN32
  #include <io.h>
  #include <direct.h>
  #include <process.h>   /* _getpid */
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_open   _open
  #define owl_unlink _unlink
  #define owl_getpid _getpid
  #define OWL_MKDIR(p) _mkdir(p)
  #define OWL_RMDIR(p) _rmdir(p)
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_open   open
  #define owl_unlink unlink
  #define owl_getpid getpid
  #define OWL_MKDIR(p) mkdir((p), 0755)
  #define OWL_RMDIR(p) rmdir(p)
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

extern int handle_check_command_no_vm_lifecycle(const char* filepath);

#define CE_DIR "tests/compile-error"

/* Workdir : build/<PREFIX><pid>_<ts>/.
 *
 *  : le prefixe commence par un POINT. `owl test` (project.c /
 * scan_directory) walke le depot depuis la racine et descend dans build/ : les
 * fixtures copiees ici etaient DECOUVERTES ET COMPILEES comme compagnons d'une
 * campagne de tests (mesure : `owl test build --list-files` sortait
 * `[loaded] build/_ce_selftest_<pid>_<ts>/<fixture>.owl`). D'ou les aborts
 * transitoires quand un self-test concurrent supprimait ces .owl sous les pieds
 * du runner. Tous les walkers du depot sautent les entrees commencant par '.',
 * donc le workdir devient invisible pour eux : la race disparait a la racine
 * plutot qu'a coup d'exclusions a maintenir walker par walker. */
#define CE_WORKDIR_PREFIX ".ce_selftest_"

/* Retention des workdirs `_KEPT` (conserves volontairement sur echec pour
 * debug post-mortem, ) : on garde les N plus recents, on purge au-dela.
 * Un `_KEPT` est TERMINAL (aucun process ne l'ecrit plus) : sa suppression est
 * sans risque quel que soit son age. */
#define CE_KEPT_MAX 3

/* Un workdir SANS `_KEPT` appartient peut-etre a un run en cours (autre pid) :
 * on ne le purge qu'au-dela de cet age. Un run complet dure ~30s ; 1h de marge
 * garantit qu'on ne detruit jamais le workdir d'un voisin vivant. */
#define CE_ORPHAN_MIN_AGE_SEC 3600

static int g_pass = 0;
static int g_fail = 0;

/* Ventilation du corpus, affichee au bilan : un chiffre visible permet de
 * reperer une chute anormale, la ou la garde ne protege que du zero. */
static int g_flatCases   = 0;   /* fixtures FLAT appariees */
static int g_dirCases    = 0;   /* sous-dossiers avec une entree */
static int g_companions  = 0;   /* .owl de contexte dans les sous-dossiers */

/* `name` se termine-t-il par `suffix` ? */
static bool has_suffix(const char* name, const char* suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    return n > s && strcmp(name + n - s, suffix) == 0;
}

/* Un `<stem>.expected.json` sans `<stem>.owl` ne sera jamais apparie, donc
 * jamais compare : il simule une couverture qui n'existe pas. */
static void check_orphan_expected(const char* dir, const char* name) {
    size_t stem = strlen(name) - strlen(".expected.json");
    char owlPath[1024];
    snprintf(owlPath, sizeof owlPath, "%s/%.*s.owl", dir, (int)stem, name);
    FILE* probe = fopen(owlPath, "rb");
    if (probe) { fclose(probe); return; }
    g_fail++;
    fprintf(stderr, "  ORPHELIN %s/%s : snapshot sans entree — aucun %.*s.owl "
                    "a checker, cette fixture ne teste rien (ecrire l'entree "
                    "ou supprimer le snapshot)\n",
            dir, name, (int)stem, name);
}

static char  g_workdir[1024];
static char  g_actual[64 * 1024];
static char  g_expected[64 * 1024];

/* Noms des .owl copies dans le workdir pour cette fixture (a nettoyer). */
static char  g_copied[64][256];
static int   g_copiedCount = 0;

/* Lit un fichier entier (binaire-safe). *outLen = octets lus. NULL si absent. */
static char* slurp_bytes(const char* path, size_t* outLen) {
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
    if (outLen) *outLen = n;
    return buf;
}

/* Ecrit des octets bruts (binaire-safe). */
static bool write_bytes(const char* path, const char* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    if (n) fwrite(data, 1, n, f);
    fclose(f);
    return true;
}

/* Copie src -> g_workdir/basename, enregistre basename pour le cleanup. */
static bool copy_into_workdir(const char* srcPath, const char* basename) {
    size_t len = 0;
    char* data = slurp_bytes(srcPath, &len);
    if (!data) return false;
    char dst[1024];
    snprintf(dst, sizeof dst, "%s/%s", g_workdir, basename);
    bool ok = write_bytes(dst, data, len);
    free(data);
    if (ok && g_copiedCount < 64) {
        snprintf(g_copied[g_copiedCount], sizeof g_copied[0], "%s", basename);
        g_copiedCount++;
    }
    return ok;
}

/* Supprime TOUS les fichiers reguliers du workdir (pas seulement g_copied[]).
 *  : le tracking via g_copied[] laissait des reliquats si une copie
 * partielle echouait (ex. ENAMETOOLONG) ou si un run precedent du runner
 * avait crashe au milieu. On scanne le workdir et on supprime tout ce qui
 * n'est pas "." / "..". */
static void cleanup_workdir(void) {
    g_copiedCount = 0;
    DIR* d = opendir(g_workdir);
    if (!d) return;
    char p[1024];
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;
        snprintf(p, sizeof p, "%s/%s", g_workdir, n);
        owl_unlink(p);
    }
    closedir(d);
}

/* Supprime workdir + tout son contenu (fin de run). */
static void wipe_and_remove_workdir(void) {
    cleanup_workdir();
    OWL_RMDIR(g_workdir);
}

/* Supprime un dossier PLAT quelconque (fichiers + le dossier). Les workdirs du
 * harnais ne contiennent jamais de sous-dossier : une fixture SUB-DIR est
 * copiee a plat. */
static void wipe_dir_flat(const char* path) {
    DIR* d = opendir(path);
    if (d) {
        char p[1024];
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            const char* n = e->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) continue;
            snprintf(p, sizeof p, "%s/%s", path, n);
            owl_unlink(p);
        }
        closedir(d);
    }
    OWL_RMDIR(path);
}

/* Timestamp encode dans `<PREFIX><pid>_<ts>[_KEPT]`. -1 si illisible. */
static long ce_ts_from_name(const char* name, int* isKept) {
    size_t len = strlen(name);
    *isKept = 0;
    const char* end = name + len;
    if (len > 5 && strcmp(end - 5, "_KEPT") == 0) { *isKept = 1; end -= 5; }
    const char* p = end;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == end || p == name || p[-1] != '_') return -1;
    return atol(p);
}

typedef struct { long ts; char name[256]; } CeKept;

static int ce_kept_cmp_desc(const void* a, const void* b) {
    long ta = ((const CeKept*)a)->ts, tb = ((const CeKept*)b)->ts;
    return (tb > ta) - (tb < ta);
}

/*  : borne l'accumulation dans build/ sans supprimer le mecanisme de
 * diagnostic. Garde les CE_KEPT_MAX workdirs `_KEPT` les plus recents, purge
 * les plus anciens, et ramasse les workdirs sans `_KEPT` laisses par un run
 * tue en vol (le hook de build tue owl.exe) au-dela de CE_ORPHAN_MIN_AGE_SEC.
 * Collecte AVANT suppression : on ne mute pas un DIR en cours d'iteration. */
static void prune_old_workdirs(void) {
    CeKept kept[64];
    int keptN = 0;
    char orphan[64][256];
    int orphanN = 0;
    const size_t prefixLen = strlen(CE_WORKDIR_PREFIX);
    time_t now = time(NULL);

    DIR* d = opendir("build");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (strncmp(n, CE_WORKDIR_PREFIX, prefixLen) != 0) continue;
        char full[1024];
        snprintf(full, sizeof full, "build/%s", n);
        if (!is_directory(full)) continue;
        if (strcmp(full, g_workdir) == 0) continue;   /* le notre */
        int isKept = 0;
        long ts = ce_ts_from_name(n, &isKept);
        if (isKept) {
            if (keptN < 64) {
                kept[keptN].ts = ts;
                snprintf(kept[keptN].name, sizeof kept[0].name, "%s", n);
                keptN++;
            }
        } else if (ts > 0 && (long)now - ts > CE_ORPHAN_MIN_AGE_SEC && orphanN < 64) {
            snprintf(orphan[orphanN], sizeof orphan[0], "%s", n);
            orphanN++;
        }
    }
    closedir(d);

    for (int i = 0; i < orphanN; i++) {
        char full[1024];
        snprintf(full, sizeof full, "build/%s", orphan[i]);
        wipe_dir_flat(full);
    }
    if (keptN > CE_KEPT_MAX) {
        qsort(kept, (size_t)keptN, sizeof kept[0], ce_kept_cmp_desc);
        for (int i = CE_KEPT_MAX; i < keptN; i++) {
            char full[1024];
            snprintf(full, sizeof full, "build/%s", kept[i].name);
            wipe_dir_flat(full);
        }
    }
}

/* Strip tous les '\r' in-place. */
static void strip_cr(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) if (*r != '\r') *w++ = *r;
    *w = '\0';
}

/* Strip les '\n'/'\r' de fin (le script normalise un newline final). */
static void strip_trailing_nl(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

/* Lance handle_check sur entryRelPath (relatif au workdir), capture stdout
 * dans g_actual. Le JSON ne sort jamais vers le terminal parent. */
static void run_check_capture(const char* entryBasename) {
    g_actual[0] = '\0';
    char entryPath[1024], captPath[1024];
    snprintf(entryPath, sizeof entryPath, "%s/%s", g_workdir, entryBasename);
    snprintf(captPath,  sizeof captPath,  "%s/_capture.out", g_workdir);

    /* Isolation par fixture : VM fraiche + reset des tables POO compile-time,
     * comme le test runner (testing.c). Le script obtenait cette isolation via
     * un process frais par fixture ; in-process, sans ce reset, les globals /
     * classes d'une fixture polluent la suivante (faux echecs sur window_* /
     * poo_* / enum_*). vm_init/vm_free hors de la zone de capture (silencieux,
     * mais on garde le stdout capture strictement autour du --check). */
    vm_init();
    compiler_reset_poo_state();

    fflush(stdout);
    int saved = owl_dup(owl_fileno(stdout));
    int fd = owl_open(captPath, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (saved < 0 || fd < 0) {
        if (saved >= 0) owl_close(saved);
        if (fd >= 0) owl_close(fd);
        vm_free();
        return;
    }
    owl_dup2(fd, owl_fileno(stdout));
    owl_close(fd);

    handle_check_command_no_vm_lifecycle(entryPath);
    fflush(stdout);

    owl_dup2(saved, owl_fileno(stdout));
    owl_close(saved);

    vm_free();

    size_t len = 0;
    char* cap = slurp_bytes(captPath, &len);
    if (cap) {
        size_t copy = len < sizeof(g_actual) - 1 ? len : sizeof(g_actual) - 1;
        memcpy(g_actual, cap, copy);
        g_actual[copy] = '\0';
        free(cap);
    }
}

/* Compare g_actual a expectPath (apres normalisation). N'imprime QUE le nom
 * en cas d'echec — jamais le contenu (securite contexte). */
static void compare_expected(const char* label, const char* expectPath) {
    size_t len = 0;
    char* exp = slurp_bytes(expectPath, &len);
    if (!exp) {
        g_fail++;
        fprintf(stderr, "  MISS %s (pas de .expected.json)\n", label);
        return;
    }
    size_t copy = len < sizeof(g_expected) - 1 ? len : sizeof(g_expected) - 1;
    memcpy(g_expected, exp, copy);
    g_expected[copy] = '\0';
    free(exp);

    strip_cr(g_actual);    strip_trailing_nl(g_actual);
    strip_cr(g_expected);  strip_trailing_nl(g_expected);

    if (strcmp(g_actual, g_expected) == 0) {
        g_pass++;
    } else {
        g_fail++;
        fprintf(stderr, "  FAIL %s (sortie --check != snapshot)\n", label);
    }
}

/* Une fixture FLAT : <name>.owl + <name>.expected.json. */
static void process_flat(const char* owlName) {
    size_t len = strlen(owlName);  /* owlName finit par ".owl" */
    char stem[256];
    snprintf(stem, sizeof stem, "%.*s", (int)(len - 4), owlName);

    char srcPath[1024], expPath[1024];
    snprintf(srcPath, sizeof srcPath, "%s/%s", CE_DIR, owlName);
    snprintf(expPath, sizeof expPath, "%s/%s.expected.json", CE_DIR, stem);

    g_copiedCount = 0;
    if (!copy_into_workdir(srcPath, owlName)) {
        /* Echec de copie = cas NON examine : un echec, jamais un silence. */
        g_fail++;
        fprintf(stderr, "  MISS %s (copie dans le workdir impossible — cas non examine)\n",
                owlName);
        cleanup_workdir();
        return;
    }
    run_check_capture(owlName);
    compare_expected(owlName, expPath);
    cleanup_workdir();
}

/* Une fixture SUB-DIR : copie tous les .owl du sous-dossier, l'entry est
 * celui qui a un <entry>.expected.json voisin. */
static void process_subdir(const char* subName) {
    char subPath[1024];
    snprintf(subPath, sizeof subPath, "%s/%s", CE_DIR, subName);

    DIR* d = opendir(subPath);
    if (!d) return;

    char entryBasename[256]; entryBasename[0] = '\0';
    char entryExpected[1024]; entryExpected[0] = '\0';
    int  entryCount = 0;

    g_copiedCount = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        size_t len = strlen(n);
        /* Snapshot : pas une entree, mais il doit en avoir une. */
        if (has_suffix(n, ".expected.json")) { check_orphan_expected(subPath, n); continue; }
        if (len < 5 || strcmp(n + len - 4, ".owl") != 0) continue;
        char srcPath[1024];
        snprintf(srcPath, sizeof srcPath, "%s/%s", subPath, n);
        copy_into_workdir(srcPath, n);

        /* entry = .owl avec un .expected.json voisin. Deux entrees dans le
         * meme sous-dossier = la 2e etait silencieusement ignoree. */
        char cand[1024];
        snprintf(cand, sizeof cand, "%s/%.*s.expected.json",
                 subPath, (int)(len - 4), n);
        FILE* probe = fopen(cand, "rb");
        if (probe) {
            fclose(probe);
            entryCount++;
            if (!entryBasename[0]) {
                snprintf(entryBasename, sizeof entryBasename, "%s", n);
                snprintf(entryExpected, sizeof entryExpected, "%s", cand);
            }
        } else {
            g_companions++;
        }
    }
    closedir(d);

    if (entryCount > 1) {
        g_fail++;
        fprintf(stderr, "  AMBIGU %s/ : %d entrees (.owl avec snapshot voisin) — "
                        "une seule est checkee, les autres ne testent rien. "
                        "Scinder en un sous-dossier par cas.\n", subName, entryCount);
        cleanup_workdir();
        return;
    }

    if (!entryBasename[0]) {
        fprintf(stderr, "  MISS %s/ (aucun .owl avec .expected.json voisin)\n", subName);
        g_fail++;
        cleanup_workdir();
        return;
    }

    g_dirCases++;
    run_check_capture(entryBasename);
    char label[512];
    snprintf(label, sizeof label, "%s/%s", subName, entryBasename);
    compare_expected(label, entryExpected);
    cleanup_workdir();
}

int compile_error_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;
    g_flatCases = 0;
    g_dirCases = 0;
    g_companions = 0;

    /* workdir DANS build/ (pas /tmp). Sur Windows, %TMP% =
     * C:\Users\<user>\AppData\Local\Temp est scanne en real-time par
     * Windows Defender : facteur ~20x sur les I/O fixture-par-fixture
     * (444s -> ~30s mesure 2026-05-30, ). build/ est exclu (ou
     * scanne nettement moins agressivement) car interne au repo.
     *
     *  : workdir UNIQUE par instance (pid + timestamp). Le path fixe
     * partage entre tous les owl du systeme provoquait :
     *   - blocages (Windows verrouille _capture.out tant qu'un autre process
     *     du runner — y compris zombie — l'a ouvert) ;
     *   - fixtures "perdues" en cours de scan (un 2e process supprimait les
     *     .owl entre la copie et le handle_check) ;
     *   - faux FAILs de fuite d'etat (le compiler scan du workdir voyait des
     *     fichiers parasites copies par l'autre instance).
     * Avec un workdir par-pid, aucune collision inter-process possible.
     *
     * Le path relatif suppose CWD = racine repo (cas standard make all qui
     * invoque `./build/owl.exe --self-test-compile-error`). */
    snprintf(g_workdir, sizeof g_workdir, "build/%s%d_%ld",
             CE_WORKDIR_PREFIX, (int)owl_getpid(), (long)time(NULL));
    /* Si un run precedent du MEME pid (rare : meme TS) a laisse un workdir,
     * on le nettoie avant de (re)creer. */
    cleanup_workdir();
    OWL_RMDIR(g_workdir);
    if (OWL_MKDIR(g_workdir) != 0) {
        fprintf(stderr, "compile-error: cannot create workdir %s\n", g_workdir);
        return 1;
    }
    prune_old_workdirs();

    DIR* d = opendir(CE_DIR);
    if (!d) {
        /* Dossier absent = ECHEC, plus un skip vert. Le harnais est branche sur
         * `make all` depuis la racine du depot : s'il ne trouve pas son corpus,
         * c'est qu'on l'execute sans corpus, pas qu'il n'y a rien a verifier. */
        fprintf(stderr, "compile-error: CORPUS INTROUVABLE — %s absent "
                        "(cwd = racine du depot attendue). 0 cas examine.\n", CE_DIR);
        wipe_and_remove_workdir();
        return 1;
    }

    /* Passe 1 : fixtures FLAT. Passe 2 : sous-dossiers. */
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
        size_t len = strlen(n);
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", CE_DIR, n);
        if (is_directory(full)) {
            process_subdir(n);
        } else if (has_suffix(n, ".expected.json")) {
            check_orphan_expected(CE_DIR, n);
        } else if (len > 4 && strcmp(n + len - 4, ".owl") == 0) {
            g_flatCases++;
            process_flat(n);
        }
    }
    closedir(d);

    /* Garde ensemble vide : sans ca, "0 fail" signifie soit "tout passe" soit
     * "rien n'a ete examine", et les deux se ressemblent trop. */
    if (g_flatCases + g_dirCases == 0) {
        g_fail++;
        fprintf(stderr, "compile-error: CORPUS VIDE — aucune fixture appariee "
                        "dans %s. Un controle qui n'examine rien ne prouve "
                        "rien.\n", CE_DIR);
    }

    /*  : detruire le workdir UNIQUE en fin de run sur succes, pour ne
     * pas accumuler des dossiers tmp/<pid>/ a chaque invocation.
     *
     *  : sur echec, on CONSERVE le workdir pour debug post-mortem
     * (snapshot du JSON capture, fixtures effectivement copiees, etc.). On le
     * renomme avec un suffixe `_KEPT` pour le rendre visible et signaler que
     * la retention est intentionnelle (pas un leak). Si le rename echoue
     * (collision tres improbable vu le pid+ts), on garde le path d'origine. */
    if (g_fail == 0) {
        wipe_and_remove_workdir();
    } else {
        char keptdir[1024];
        snprintf(keptdir, sizeof keptdir, "%s_KEPT", g_workdir);
        if (rename(g_workdir, keptdir) == 0) {
            fprintf(stderr, "compile-error: workdir conserve pour debug -> %s/\n", keptdir);
        } else {
            fprintf(stderr, "compile-error: workdir conserve pour debug -> %s/ (rename echoue)\n", g_workdir);
        }
    }

    fprintf(stderr, "compile-error: %d cas executes (%d flat, %d dossiers) — "
                    "%d pass, %d fail, %d compagnons\n",
            g_flatCases + g_dirCases, g_flatCases, g_dirCases,
            g_pass, g_fail, g_companions);
    return g_fail;
}
