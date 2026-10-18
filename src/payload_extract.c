// SPDX-License-Identifier: AGPL-3.0-only
/* payload_extract.c — Detection + extraction du payload single-file
 * appende a la fin de owl-build (PyInstaller-style).
 *
 * Layout du megafile :
 *   [owl-build.exe normal]      bytes 0..N
 *   [zip payload]               bytes N..M
 *   [footer 24 bytes]           bytes M..end
 *      offset (uint64 LE)       8 bytes
 *      size   (uint64 LE)       8 bytes
 *      magic  "OWLBUNDL"        8 bytes
 *
 * Au runtime, owl-build :
 *   1. ouvre argv[0]
 *   2. lit footer (-24 bytes)
 *   3. si magic OK : extract le zip dans <TEMP>/owl-build-<size>/ (cache)
 *   4. owlRoot = ce dossier extrait
 */

#include "payload_extract.h"

/* owl_error_render_lang_is_en : bascule FR/EN du chrome owl-build (defini
 * dans owl_errors.c). Forward-decl plutot que l'include complet de
 * owl_errors.h : ce dernier tire scanner.h, dont le type TokenType entre en
 * collision avec celui de windows.h inclus plus bas. */
int owl_error_render_lang_is_en(void);

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define PATH_SEP '\\'
#else
#  include <unistd.h>
#  include <dirent.h>
#  define MKDIR(p) mkdir((p), 0755)
#  define PATH_SEP '/'
#endif

#include "miniz.h"

#define OWL_PAYLOAD_MAGIC "OWLBUNDL"
#define OWL_PAYLOAD_FOOTER_SIZE 24

#ifdef _WIN32
/* Convertit un chemin ANSI (ACP) en chemin large, prefixe "\\?\" pour lever
 * la limite MAX_PATH (260) des API Win32. Les chemins viennent de getenv(TEMP)
 * (ANSI/ACP) et de noms d'entrees zip ASCII : CP_ACP round-trip correctement
 * un login accentue, CP_UTF8 corromprait l'ACP. Le prefixe n'est pose que sur
 * un chemin drive-absolu deja canonique (backslash-only, pas de . / ..) ; sinon
 * le chemin est laisse tel quel (fallback sur, ex. UNC \\srv\...). Renvoie 0 et
 * remplit `out` (cap = nb d'elements wchar), -1 si conversion ou place manque. */
static int to_wide_path(const char* path, wchar_t* out, size_t cap) {
    wchar_t tmp[4096];
    int n = MultiByteToWideChar(CP_ACP, 0, path, -1, tmp,
                                (int)(sizeof(tmp) / sizeof(tmp[0])));
    if (n <= 0) return -1;   /* echec conversion ou debordement de tmp */
    /* n inclut le terminateur nul. */
    int drive_abs = (tmp[0] != L'\0' && tmp[1] == L':' && tmp[2] == L'\\');
    if (drive_abs) {
        if ((size_t)n + 4 > cap) return -1;
        out[0] = L'\\'; out[1] = L'\\'; out[2] = L'?'; out[3] = L'\\';
        memcpy(out + 4, tmp, (size_t)n * sizeof(wchar_t));
    } else {
        if ((size_t)n > cap) return -1;
        memcpy(out, tmp, (size_t)n * sizeof(wchar_t));
    }
    return 0;
}

/* Feuilles leaf : frame courte, pas de buffer wide retenu pendant la recursion
 * de rmtree_local. Non fatales : un chemin non convertible est simplement
 * ignore (au pire un residu reste sous un arbre en cours de destruction). */
static void win_delete_file(const char* path) {
    wchar_t w[4096];
    if (to_wide_path(path, w, sizeof(w) / sizeof(w[0])) != 0) return;
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);   /* leve un eventuel read-only */
    DeleteFileW(w);
}
static void win_remove_dir(const char* path) {
    wchar_t w[4096];
    if (to_wide_path(path, w, sizeof(w) / sizeof(w[0])) != 0) return;
    RemoveDirectoryW(w);
}
#endif

int owl_get_self_path(char* out, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) return -1;
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(out, &size);
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n < 0) return -1;
    out[n] = '\0';
    return 0;
#endif
}

int owl_payload_check(const char* self_path,
                      uint64_t* out_offset, uint64_t* out_size) {
    FILE* f = fopen(self_path, "rb");
    if (!f) return -1;
    if (fseek(f, -OWL_PAYLOAD_FOOTER_SIZE, SEEK_END) != 0) {
        fclose(f); return -1;
    }
    uint8_t buf[OWL_PAYLOAD_FOOTER_SIZE];
    if (fread(buf, 1, OWL_PAYLOAD_FOOTER_SIZE, f) != OWL_PAYLOAD_FOOTER_SIZE) {
        fclose(f); return -1;
    }
    fclose(f);
    if (memcmp(buf + 16, OWL_PAYLOAD_MAGIC, 8) != 0) return -1;
    uint64_t off = 0, sz = 0;
    for (int i = 0; i < 8; i++) {
        off |= ((uint64_t)buf[i]) << (i * 8);
        sz  |= ((uint64_t)buf[8 + i]) << (i * 8);
    }
    *out_offset = off;
    *out_size = sz;
    return 0;
}

static int mkdirs(const char* path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
#ifdef _WIN32
    /* Cree chaque parent via CreateDirectoryW sur chemin large "\\?\" (leve
     * MAX_PATH). ERROR_ALREADY_EXISTS = succes. i == len traite le chemin
     * complet (pas de separateur final requis). */
    for (size_t i = 1; i <= len; i++) {
        if (i == len || buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i]; buf[i] = '\0';
            wchar_t wbuf[4096];
            if (to_wide_path(buf, wbuf, sizeof(wbuf) / sizeof(wbuf[0])) == 0) {
                CreateDirectoryW(wbuf, NULL);  /* ALREADY_EXISTS ignore */
            }
            buf[i] = c;
        }
    }
    return 0;
#else
    /* Iterate / create chaque parent */
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char c = buf[i]; buf[i] = '\0';
            MKDIR(buf);
            buf[i] = c;
        }
    }
    MKDIR(buf);
    return 0;
#endif
}

static int file_exists_local(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Suppression recursive d'un dossier, portable, non fatale : une entree
 * verrouillee ou non supprimable est ignoree (au pire un residu reste). Ne
 * suit PAS les liens : cote Windows FindFirstFile ne traverse pas les reparse
 * points (on retire la jonction telle quelle) ; cote POSIX lstat distingue le
 * lien de sa cible (on unlink le lien, on ne recurse pas dedans). Buffers
 * larges pour encaisser les arbres profonds du payload (Qt6) ; une troncature
 * eventuelle de snprintf produit un chemin invalide qui echoue proprement, et
 * resterait de toute facon sous l'arbre en cours de destruction. */
static void rmtree_local(const char* path) {
#ifdef _WIN32
    /* API larges + prefixe "\\?\" : indispensable pour purger un arbre profond
     * que l'extraction vient de creer au-dela de MAX_PATH. Les noms renvoyes
     * par FindFirstFileW (wide) sont reconvertis en ACP pour reconstruire le
     * chemin enfant (ASCII dans l'arbre payload, round-trip sur). */
    char patbuf[4096];
    snprintf(patbuf, sizeof(patbuf), "%s\\*", path);
    wchar_t wpattern[4096];
    if (to_wide_path(patbuf, wpattern, sizeof(wpattern) / sizeof(wpattern[0])) != 0) {
        win_remove_dir(path); return;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { win_remove_dir(path); return; }
    do {
        char name[1024];
        if (WideCharToMultiByte(CP_ACP, 0, fd.cFileName, -1, name,
                                (int)sizeof(name), NULL, NULL) == 0) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s\\%s", path, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                win_remove_dir(child);        /* jonction : ne pas traverser */
            } else {
                rmtree_local(child);
            }
        } else {
            win_delete_file(child);           /* leve read-only + supprime */
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    win_remove_dir(path);
#else
    DIR* d = opendir(path);
    if (!d) { rmdir(path); return; }
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, name);
        struct stat st;
        if (lstat(child, &st) != 0) continue;       /* lien casse / inaccessible */
        if (S_ISDIR(st.st_mode)) {
            rmtree_local(child);                     /* vrai dossier uniquement */
        } else {
            unlink(child);                           /* fichier ou lien symbolique */
        }
    }
    closedir(d);
    rmdir(path);
#endif
}

/* GC des versions obsoletes : balaye le dossier parent `parent` et supprime
 * recursivement chaque frere nomme EXACTEMENT "owl-build-<...>" qui n'est pas
 * la version courante `current_dir`. Tout le reste de `parent` est intouche.
 * Edge concurrentiel accepte (extreme) : un vieux owl-build.exe encore en
 * cours d'execution pourrait se voir retirer son dossier sous les pieds ;
 * c'est non fatal pour le build courant, et une suppression bloquee par un
 * handle ouvert est simplement ignoree. */
static void owl_payload_gc(const char* parent, const char* current_dir) {
    int en = owl_error_render_lang_is_en();
    const char* prefix = "owl-build-";
    size_t plen = strlen(prefix);
#ifdef _WIN32
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\owl-build-*", parent);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const char* name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        /* Le motif "owl-build-*" peut matcher via un nom court 8.3 ;
         * re-verifie le prefixe exact sur le nom long. */
        if (strncmp(name, prefix, plen) != 0) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s\\%s", parent, name);
        if (strcmp(full, current_dir) == 0) continue;
        printf(en ? "owl-build: pruning a previous cache version: %s\n"
                  : "owl-build: purge d'une version précédente du cache : %s\n",
               full);
        fflush(stdout);
        rmtree_local(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(parent);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        const char* name = ent->d_name;
        if (strncmp(name, prefix, plen) != 0) continue;
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", parent, name);
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;         /* dossiers uniquement */
        if (strcmp(full, current_dir) == 0) continue;
        printf(en ? "owl-build: pruning a previous cache version: %s\n"
                  : "owl-build: purge d'une version précédente du cache : %s\n",
               full);
        fflush(stdout);
        rmtree_local(full);
    }
    closedir(d);
#endif
}

/* Composants du payload indexes par cible de build : leur arbre est
 * <comp>/<triple>/... (un sous-dossier par cible supportee). Le reste du
 * payload (zig/, include/, outils host) est host-agnostique. */
static const char* const MULTI_TARGET_COMPS[] = {
    "duckdb", "qt6", "llama", "lib"
};

/* Classe une entree du zip par rapport a `target` :
 *   0 = host-agnostique (toujours candidate, selon extract_common)
 *   1 = composant de la cible demandee (a extraire)
 *  -1 = composant d'une AUTRE cible (a sauter)
 * En cas de doute (prefixe multi-cible sans triple lisible), on renvoie 0
 * (host-agnostique) : extraire en trop est sur, amputer casserait le build.
 * target == NULL => tout est traite comme host-agnostique (extraction totale). */
static int classify_entry(const char* name, const char* target) {
    if (!target) return 0;
    size_t ncomp = sizeof(MULTI_TARGET_COMPS) / sizeof(MULTI_TARGET_COMPS[0]);
    for (size_t k = 0; k < ncomp; k++) {
        const char* comp = MULTI_TARGET_COMPS[k];
        size_t clen = strlen(comp);
        if (strncmp(name, comp, clen) != 0 || name[clen] != '/') continue;
        /* name = "<comp>/<triple>/..." : isole le segment triple. */
        const char* triple = name + clen + 1;
        const char* end = strchr(triple, '/');
        if (!end) return 0;  /* pas de triple complet -> garder par prudence */
        size_t tlen = (size_t)(end - triple);
        if (tlen == strlen(target) && strncmp(triple, target, tlen) == 0) return 1;
        return -1;
    }
    return 0;
}

int owl_payload_extract(const char* self_path,
                         uint64_t offset, uint64_t size,
                         const char* dest_dir,
                         const char* target, int extract_common) {
    FILE* f = fopen(self_path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return -1; }
    void* buf = malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) { free(buf); return -1; }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, buf, (size_t)size, 0)) {
        fprintf(stderr, "owl-build: invalid zip payload\n");
        free(buf);
        return -1;
    }

    int n = (int)mz_zip_reader_get_num_files(&zip);
    int errors = 0;
    int en = owl_error_render_lang_is_en();

    /* Pre-passe : total decompresse pour le denominateur de progression et
     * nombre d'entrees reellement extraites (apres filtre par cible). Lit
     * seulement l'annuaire central deja en memoire, cout negligeable. */
    uint64_t total_bytes = 0;
    int n_extract = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        int cls = classify_entry(st.m_filename, target);
        if (cls < 0) continue;                 /* autre cible : jamais ecrite */
        if (cls == 0 && !extract_common) continue; /* host-agnostique deja la */
        total_bytes += st.m_uncomp_size;
        n_extract++;
    }
    const double MB = 1024.0 * 1024.0;

    /* Cadence : une ligne tous les ~10 % des entrees extraites (jamais
     * par-fichier sur des milliers d'entrees), plus la derniere. Progression
     * en newlines (pas de \r) pour rester lisible une fois la sortie redirigee. */
    int step = n_extract / 10;
    if (step < 1) step = 1;
    /* Wall-clock (time/difftime), pas clock() : l'extraction est dominee par
     * l'attente I/O + antivirus ; clock() compterait du temps CPU et sous-
     * estimerait la duree reelle sur les hotes ou clock() == temps processeur. */
    time_t t0 = time(NULL);
    uint64_t written = 0;
    int done = 0;

    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) { errors++; continue; }
        /* Filtre par cible : on n'ecrit sur disque que les composants de la
         * cible du build + le host-agnostique (si pas deja extrait). L'autre
         * cible represente ~315 Mo ecrits pour rien, scannes par l'antivirus. */
        int cls = classify_entry(st.m_filename, target);
        if (cls < 0) continue;
        if (cls == 0 && !extract_common) continue;
        char outpath[4096];
        snprintf(outpath, sizeof(outpath), "%s/%s", dest_dir, st.m_filename);
        /* Normalise separateurs */
#ifdef _WIN32
        for (char* p = outpath; *p; p++) if (*p == '/') *p = '\\';
#endif
        if (st.m_is_directory) {
            mkdirs(outpath);
        } else {
            /* mkdir du parent */
            char parent[4096];
            snprintf(parent, sizeof(parent), "%s", outpath);
            char* slash = strrchr(parent, '/');
            char* bslash = strrchr(parent, '\\');
            if (bslash && (!slash || bslash > slash)) slash = bslash;
            if (slash) { *slash = '\0'; mkdirs(parent); }
#ifdef _WIN32
            /* mz_zip_reader_extract_to_file ouvre via fopen (borne MAX_PATH) :
             * on extrait en memoire puis on ecrit via CreateFileW sur chemin
             * large "\\?\" pour franchir MAX_PATH. m_uncomp_size vaut 0 pour une
             * entree legitimement vide (data peut alors etre NULL sans erreur) ;
             * un echec reel laisse data == NULL avec m_uncomp_size > 0. */
            size_t ucs = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, &ucs, 0);
            int ok = 0;
            if (data != NULL || st.m_uncomp_size == 0) {
                wchar_t wpath[4096];
                if (to_wide_path(outpath, wpath,
                                 sizeof(wpath) / sizeof(wpath[0])) == 0) {
                    HANDLE hf = CreateFileW(wpath, GENERIC_WRITE, 0, NULL,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                            NULL);
                    if (hf != INVALID_HANDLE_VALUE) {
                        ok = 1;
                        if (data != NULL && ucs > 0) {
                            const unsigned char* p = (const unsigned char*)data;
                            size_t remaining = ucs;
                            while (remaining > 0) {
                                DWORD chunk = (remaining > 0x40000000UL)
                                              ? 0x40000000UL : (DWORD)remaining;
                                DWORD wr = 0;
                                if (!WriteFile(hf, p, chunk, &wr, NULL)
                                    || wr != chunk) { ok = 0; break; }
                                p += wr;
                                remaining -= wr;
                            }
                        }
                        CloseHandle(hf);
                    }
                }
            }
            if (data != NULL) mz_free(data);
            if (!ok) {
                fprintf(stderr, "owl-build: extract failed for %s\n", st.m_filename);
                errors++;
            }
#else
            if (!mz_zip_reader_extract_to_file(&zip, (mz_uint)i, outpath, 0)) {
                fprintf(stderr, "owl-build: extract failed for %s\n", st.m_filename);
                errors++;
            }
            /* Restore exec bit pour tout fichier extrait sur POSIX.
             * zip ne preserve pas les modes par defaut, et les binaires
             * (zig, owl-build interne, etc.) doivent etre executables.
             * Pas grave que les .a / .h soient aussi 0755, c'est sans
             * impact runtime. */
            chmod(outpath, 0755);
#endif
        }
        written += st.m_uncomp_size;
        done++;
        if (done % step == 0 || done == n_extract) {
            printf(en
                ? "owl-build:   %d/%d files (%.1f/%.1f MB)\n"
                : "owl-build:   %d/%d fichiers (%.1f/%.1f Mo)\n",
                done, n_extract, (double)written / MB, (double)total_bytes / MB);
            fflush(stdout);
        }
    }
    mz_zip_reader_end(&zip);
    free(buf);

    double secs = difftime(time(NULL), t0);
    printf(en
        ? "owl-build: extraction complete: %d files (%.1f MB) in %.1fs\n"
        : "owl-build: extraction terminée : %d fichiers (%.1f Mo) en %.1fs\n",
        done, (double)written / MB, secs);
    fflush(stdout);

    if (errors > 0) {
        fprintf(stderr, en
            ? "owl-build: %d file(s) could not be written. Likely cause: "
              "not enough free space on the temporary folder, or missing write "
              "permission on it.\n"
            : "owl-build: %d fichier(s) n'ont pas pu être écrits. Cause probable : "
              "espace disque insuffisant sur le dossier temporaire, ou droits "
              "d'écriture manquants dessus.\n",
            errors);
        fflush(stderr);
    }

    return errors == 0 ? 0 : -1;
}

int owl_payload_setup(const char* target, char* out_root, size_t cap) {
    char self_path[1024];
    if (owl_get_self_path(self_path, sizeof(self_path)) != 0) return -1;
    uint64_t offset, size;
    if (owl_payload_check(self_path, &offset, &size) != 0) return -1;

    /* Compute temp dir avec hash simple = size en hex (suffisant comme
     * cache key : un payload regenere change de taille au moindre commit).
     * Toutes les cibles partagent ce dossier : l'arbre host-agnostique
     * (zig/, include/) n'y est extrait qu'une fois, chaque cible y ajoute
     * seulement ses propres composants. */
    const char* tempVar = NULL;
#ifdef _WIN32
    tempVar = getenv("TEMP");
    if (!tempVar) tempVar = getenv("TMP");
    if (!tempVar) tempVar = "C:\\Windows\\Temp";
#else
    tempVar = getenv("TMPDIR");
    if (!tempVar) tempVar = "/tmp";
#endif
    char temp_dir[1024];
    snprintf(temp_dir, sizeof(temp_dir), "%s%cowl-build-%llx",
             tempVar, PATH_SEP, (unsigned long long)size);

    /* Deux marqueurs : .complete.<target> garde l'extraction des composants
     * de cette cible, .complete.common garde l'arbre host-agnostique partage.
     * Les marqueurs sont poses APRES une extraction complete : un crash en
     * cours laisse le marqueur absent et force une re-extraction propre.
     * Note : un ancien marqueur global .complete (format pre-cache-par-cible)
     * eventuellement present dans un temp_dir de meme taille est simplement
     * ignore — au pire une extraction filtree de plus, jamais de build casse. */
    char tbuf[128];
    snprintf(tbuf, sizeof(tbuf), "%s", target ? target : "all");
    char marker_target[1280];
    snprintf(marker_target, sizeof(marker_target), "%s%c.complete.%s",
             temp_dir, PATH_SEP, tbuf);
    char marker_common[1280];
    snprintf(marker_common, sizeof(marker_common), "%s%c.complete.common",
             temp_dir, PATH_SEP);

    if (!file_exists_local(marker_target)) {
        int need_common = !file_exists_local(marker_common);
        printf(owl_error_render_lang_is_en()
               ? "owl-build: extracting payload (target=%s) to %s ...\n"
               : "owl-build: extraction du contenu (cible=%s) vers %s ...\n",
               target ? target : "all", temp_dir);
        fflush(stdout);
        mkdirs(temp_dir);
        if (owl_payload_extract(self_path, offset, size, temp_dir,
                                target, need_common) != 0) {
            return -1;
        }
        if (need_common) {
            FILE* mc = fopen(marker_common, "w");
            if (mc) { fputs("ok\n", mc); fclose(mc); }
        }
        FILE* mt = fopen(marker_target, "w");
        if (mt) { fputs("ok\n", mt); fclose(mt); }

        /* GC des versions precedentes : UNIQUEMENT quand une nouvelle version
         * vient d'etre posee (need_common vrai = .complete.common etait absent).
         * Ajouter seulement une cible a une version deja connue (need_common
         * faux) ne declenche rien : la version courante n'est pas obsolete.
         * Non fatal par construction (owl_payload_gc n'echoue jamais). */
        if (need_common) {
            owl_payload_gc(tempVar, temp_dir);
        }
    }
    snprintf(out_root, cap, "%s", temp_dir);
    return 0;
}
