// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "project.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>

/* ================================================================
 * FILE UTILITIES
 * ================================================================ */

/* strdup local : la partie discover (compiler-free) ne doit pas dependre de
 * memory.c qui tire le GC (gc.h). Helper trivial, decouplage volontaire pour
 * que owl-lsp.exe puisse linker project.c sans la VM. */
static char* proj_strdup(const char* s) {
    if (s == NULL) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)malloc(len);
    if (copy == NULL) {
        fprintf(stderr, "owl: out of memory in project strdup\n");
        exit(74);
    }
    memcpy(copy, s, len);
    return copy;
}

/* Convert a UTF-16 LE or BE buffer to a newly allocated UTF-8 string.
 * `src` starts AFTER the BOM. `srcLen` is byte length (must be even).
 * Handles only the BMP + surrogate pairs — enough for all source
 * in practice. Returns NULL on allocation failure. */
static char* utf16_to_utf8(const unsigned char* src, size_t srcLen, int bigEndian) {
    /* UTF-8 worst case: 1 code point (2 bytes UTF-16 BMP) → 3 bytes UTF-8.
     * Allow 4 bytes per code point for surrogate safety. */
    char* out = (char*)malloc(srcLen * 2 + 1);
    if (out == NULL) return NULL;
    size_t oi = 0;
    size_t i = 0;
    while (i + 1 < srcLen) {
        unsigned int u = bigEndian
            ? ((unsigned int)src[i] << 8) | src[i+1]
            : ((unsigned int)src[i+1] << 8) | src[i];
        i += 2;
        /* Surrogate pair */
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < srcLen) {
            unsigned int low = bigEndian
                ? ((unsigned int)src[i] << 8) | src[i+1]
                : ((unsigned int)src[i+1] << 8) | src[i];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                i += 2;
                unsigned int cp = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
                out[oi++] = (char)(0xF0 | (cp >> 18));
                out[oi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                out[oi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[oi++] = (char)(0x80 | (cp & 0x3F));
                continue;
            }
        }
        if (u < 0x80) {
            out[oi++] = (char)u;
        } else if (u < 0x800) {
            out[oi++] = (char)(0xC0 | (u >> 6));
            out[oi++] = (char)(0x80 | (u & 0x3F));
        } else {
            out[oi++] = (char)(0xE0 | (u >> 12));
            out[oi++] = (char)(0x80 | ((u >> 6) & 0x3F));
            out[oi++] = (char)(0x80 | (u & 0x3F));
        }
    }
    out[oi] = '\0';
    return out;
}

char* read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "owl: cannot open file '%s': %s\n", path, strerror(errno));
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    unsigned char* raw = (unsigned char*)malloc(fileSize + 1);
    if (raw == NULL) {
        fprintf(stderr, "owl: not enough memory to read '%s'\n", path);
        fclose(file);
        return NULL;
    }

    size_t bytesRead = fread(raw, sizeof(char), fileSize, file);
    fclose(file);
    if (bytesRead < fileSize) {
        fprintf(stderr, "owl: could not read file '%s'\n", path);
        free(raw);
        return NULL;
    }
    raw[bytesRead] = '\0';

    /* BOM detection — some source editors write UTF-16 LE. We transparently
     * convert to UTF-8 so the scanner doesn't choke. */
    if (bytesRead >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        /* UTF-16 LE BOM */
        char* utf8 = utf16_to_utf8(raw + 2, bytesRead - 2, 0);
        free(raw);
        return utf8;
    }
    if (bytesRead >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        /* UTF-16 BE BOM */
        char* utf8 = utf16_to_utf8(raw + 2, bytesRead - 2, 1);
        free(raw);
        return utf8;
    }
    /* UTF-8 BOM — strip it. Helper partage avec main.c:repl(). */
    owl_strip_utf8_bom((char*)raw);
    return (char*)raw;
}

bool is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* ================================================================
 * FRONTIERE DE PROJET
 * ================================================================ */

/* Vrai si `name` designe le marqueur de frontiere de projet, sous ses deux
 * graphies bilingues (FR `projet`, EN `project`), casse-insensible, avec ou
 * sans l'extension `.owl`. Invariant 6.3 : le nom reserve vaut dans les deux
 * langues comme tout symbole OWL. Accepte aussi bien un basename complet
 * ("projet.owl") qu'un stem nu ("projet") selon le site appelant. */
bool is_projet_marker_name(const char* name) {
    return strcasecmp(name, "projet") == 0
        || strcasecmp(name, "project") == 0
        || strcasecmp(name, "projet.owl") == 0
        || strcasecmp(name, "project.owl") == 0;
}

/* Vrai si le dossier `dir` contient un fichier `marker` (ex: "projet.owl"). */
static bool dir_has_file(const char* dir, const char* marker) {
    size_t len = strlen(dir) + 1 + strlen(marker) + 1;
    char* path = (char*)malloc(len);
    if (path == NULL) return false;
    snprintf(path, len, "%s/%s", dir, marker);
    bool ok = file_exists(path);
    free(path);
    return ok;
}

/* Extrait le dossier parent de `filepath` dans `dirOut` (separateurs `/`).
 * Si `filepath` n'a pas de separateur, renvoie ".". */
static void parent_dir_of(const char* filepath, char* dirOut, size_t dirSize) {
    snprintf(dirOut, dirSize, "%s", filepath);
    /* Uniformise les separateurs en `/`. */
    for (char* p = dirOut; *p; p++) if (*p == '\\') *p = '/';
    char* lastSep = strrchr(dirOut, '/');
    if (lastSep) {
        if (lastSep == dirOut) {
            /* "/foo" -> racine "/" */
            dirOut[1] = '\0';
        } else {
            *lastSep = '\0';
        }
    } else {
        snprintf(dirOut, dirSize, ".");
    }
}

/* Vrai si le dossier contenant `filepath` heberge SIMULTANEMENT les deux
 * graphies bilingues du marqueur de projet (projet.owl FR + project.owl EN).
 * Ambiguite de frontiere : on ne sait pas laquelle fait foi. Reutilise
 * dir_has_file / parent_dir_of (memes primitives que la remontee de
 * frontiere), pour que le controle --check soit strictement aligne sur la
 * detection deja faite par le chemin d'execution (project_discover). */
bool project_dir_has_ambiguous_markers(const char* filepath) {
    char dir[1024];
    parent_dir_of(filepath, dir, sizeof(dir));
    return dir_has_file(dir, "projet.owl") && dir_has_file(dir, "project.owl");
}

ProjectMode resolve_project_root(const char* filepath,
                                 char* rootOut, size_t rootSize) {
    /* On resout d'abord la cible en chemin absolu. Sans ca, une cible
     * relative multi-composants (`models/config.owl`) bloque la remontee :
     * `parent_dir_of` donne `models`, puis strrchr ne trouve plus de `/` et
     * la boucle s'arrete avant d'atteindre la racine du projet (`.`). Le
     * `projet.owl` ancetre n'est alors jamais vu -> mode SCRIPT a tort, et le
     * contexte cross-sous-dossier (check ET fmt) est perdu. */
    char absPath[1024];
#ifdef _WIN32
    if (_fullpath(absPath, filepath, sizeof(absPath)) == NULL) {
        snprintf(absPath, sizeof(absPath), "%s", filepath);
    }
#else
    if (realpath(filepath, absPath) == NULL) {
        snprintf(absPath, sizeof(absPath), "%s", filepath);
    }
#endif

    /* Dossier du fichier (point de depart de la remontee). */
    char fileDir[1024];
    parent_dir_of(absPath, fileDir, sizeof(fileDir));

    /* Remonte les parents a la recherche d'un `projet.owl`. */
    char cur[1024];
    snprintf(cur, sizeof(cur), "%s", fileDir);
    for (;;) {
        if (dir_has_file(cur, "projet.owl") || dir_has_file(cur, "project.owl")) {
            snprintf(rootOut, rootSize, "%s", cur);
            return PROJECT_MODE_PROJECT;
        }
        /* Calcule le parent de `cur`. Borne a la racine du FS : si plus de
         * separateur exploitable, on s'arrete. */
        char* lastSep = strrchr(cur, '/');
        if (lastSep == NULL) break;          /* chemin relatif sans `/` */
        if (lastSep == cur) break;           /* "/" racine unix */
        /* "C:/" racine Windows (lecteur). */
        if (lastSep == cur + 2 && cur[1] == ':') break;
        *lastSep = '\0';
    }

    /* Pas de projet.owl ancetre : mode script (dossier du fichier seul,
     * voisins non-recursifs). Depuis , main.owl n'a plus de statut
     * particulier — c'est un nom de fichier ordinaire. */
    snprintf(rootOut, rootSize, "%s", fileDir);
    return PROJECT_MODE_SCRIPT;
}

/* ================================================================
 * PROJECT INIT / FREE
 * ================================================================ */

void project_init(Project* project) {
    project->files = NULL;
    project->fileCount = 0;
    project->fileCapacity = 0;
    project->entryIndex = -1;
    project->isProjectMode = false;
    project->rootDir = NULL;
    project->overlays = NULL;
    project->overlayCount = 0;
}

void project_free(Project* project) {
    for (int i = 0; i < project->fileCount; i++) {
        free(project->files[i].name);
        free(project->files[i].path);
        free(project->files[i].source);
    }
    free(project->files);
    for (int i = 0; i < project->overlayCount; i++) {
        free(project->overlays[i].path);
        free(project->overlays[i].content);
    }
    free(project->overlays);
    free(project->rootDir);
    project_init(project);
}

void project_set_overlay(Project* project, const char* path,
                         const char* content) {
    if (path == NULL || content == NULL) return;
    project->overlays = realloc(project->overlays,
                                sizeof(ProjectOverlay) * (project->overlayCount + 1));
    if (project->overlays == NULL) {
        fprintf(stderr, "owl: out of memory in project overlay\n");
        exit(74);
    }
    ProjectOverlay* ov = &project->overlays[project->overlayCount++];
    ov->path = proj_strdup(path);
    ov->content = proj_strdup(content);
}

/* Canonise un chemin pour matcher overlay.path contre project.files[i].path :
 * resolution absolue (_fullpath/realpath), separateurs uniformises en `/`, et
 * minuscule sous Windows (FS insensible a la casse). Miroir de la
 * canonicalisation cote main.c. `out` >= 1024 octets. */
static void proj_path_canon(const char* in, char* out, size_t outSize) {
    const char* src = in;
#ifdef _WIN32
    /* Pre-normalisation MSYS/Cygwin : un chemin POSIX `/c/rest` designe le
     * lecteur Windows `c:/rest`. Les argv passes a owl.exe subissent cette
     * conversion automatiquement (d'ou `project.files[i].path` en forme
     * `C:/...`), mais le JSON overlay lu sur stdin ne la subit PAS et arrive
     * en forme `/c/...`. Sans cette normalisation, `_fullpath("/c/projet")`
     * produirait `C:/c/projet` (le `/` initial = racine du lecteur courant) et
     * le matching echouerait. */
    char pre[1024];
    if (in[0] == '/' && isalpha((unsigned char)in[1]) &&
        (in[2] == '/' || in[2] == '\0')) {
        snprintf(pre, sizeof(pre), "%c:%s", in[1], in + 2);
        src = pre;
    }
#endif
    char resolved[1024];
#ifdef _WIN32
    if (_fullpath(resolved, src, sizeof(resolved)) == NULL) {
        snprintf(resolved, sizeof(resolved), "%s", src);
    }
#else
    if (realpath(src, resolved) == NULL) {
        snprintf(resolved, sizeof(resolved), "%s", src);
    }
#endif
    size_t n = 0;
    for (const char* p = resolved; *p && n + 1 < outSize; p++, n++) {
        char c = *p;
        if (c == '\\') c = '/';
#ifdef _WIN32
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
#endif
        out[n] = c;
    }
    out[n] = '\0';
}

/* Renvoie le contenu overlaye pour `filePath` si un overlay matche (chemin
 * canonique), sinon NULL. */
static const char* project_overlay_lookup(Project* project, const char* filePath) {
    if (project->overlayCount == 0) return NULL;
    char fileCanon[1024];
    proj_path_canon(filePath, fileCanon, sizeof(fileCanon));
    for (int i = 0; i < project->overlayCount; i++) {
        char ovCanon[1024];
        proj_path_canon(project->overlays[i].path, ovCanon, sizeof(ovCanon));
        if (strcmp(ovCanon, fileCanon) == 0) return project->overlays[i].content;
    }
    return NULL;
}

const char* project_get_overlay(Project* project, const char* path) {
    return project_overlay_lookup(project, path);
}

/* ================================================================
 * INTERNAL: ADD FILE TO PROJECT
 * ================================================================ */

static void project_add_file(Project* project, const char* path,
                              const char* stem, bool isTest) {
    if (project->fileCount >= project->fileCapacity) {
        int newCap = project->fileCapacity < 8 ? 8 : project->fileCapacity * 2;
        project->files = realloc(project->files, sizeof(ProjectFile) * newCap);
        project->fileCapacity = newCap;
    }

    ProjectFile* f = &project->files[project->fileCount];
    f->name = proj_strdup(stem);
    f->path = proj_strdup(path);
    f->source = NULL;  /* read later */
    f->isTest = isTest;

    project->fileCount++;
}

/* ================================================================
 * INTERNAL: SCAN DIRECTORY RECURSIVELY
 * ================================================================ */

static bool scan_directory(Project* project, const char* dirPath,
                           bool skipTestDirs, bool recursive) {
    DIR* dir = opendir(dirPath);
    if (dir == NULL) {
        fprintf(stderr, "owl: cannot open directory '%s': %s\n",
                dirPath, strerror(errno));
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;

        /* Skip . and .. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        /* Skip hidden files/dirs (starting with .) */
        if (name[0] == '.') continue;

        /* Build full path */
        size_t dirLen = strlen(dirPath);
        size_t nameLen = strlen(name);
        char* fullPath = malloc(dirLen + 1 + nameLen + 1);
        sprintf(fullPath, "%s/%s", dirPath, name);

        /* Check if directory -> recurse (sauf mode non recursif) */
        if (is_directory(fullPath)) {
            if (!recursive) {
                /* Mode script : sous-dossiers ignores ( : seul un
                 * projet.owl etend le perimetre au scan recursif). */
                free(fullPath);
                continue;
            }
            /* Skip inconditionnel des fixtures internes : un dossier
             * `internal_fixtures` contient des cas délibérément cassants
             * (crash volontaire, etc.) qui ne doivent jamais être découverts
             * par le walk d'`owl test`. Une cible explicite qui démarre SOUS
             * `internal_fixtures` reste valide : on ne croise jamais le nom
             * ici dans ce cas. */
            if (strcmp(name, "internal_fixtures") == 0) {
                free(fullPath);
                continue;
            }
            if (skipTestDirs &&
                (strcmp(name, "test") == 0 || strcmp(name, "tests") == 0)) {
                free(fullPath);
                continue;
            }
            if (!scan_directory(project, fullPath, skipTestDirs, recursive)) {
                free(fullPath);
                closedir(dir);
                return false;
            }
            free(fullPath);
            continue;
        }

        /* Check if .owl file (casse-insensible : MAIN.OWL / file.Owl
         * restent reconnus, coherent avec l'invariant casse du langage). */
        if (nameLen < 5 || strcasecmp(name + nameLen - 4, ".owl") != 0) {
            free(fullPath);
            continue;
        }

        /* `projet.owl` est le marqueur de frontiere ET, depuis la
         * brique métadonnées (/0118), le manifeste declaratif : son
         * bloc `<Ident> est un Projet PROPRIETES … FIN FIN` est prescanné (le
         * global projet devient référençable en cascade cross-fichier). Depuis
         * , son top-level EST le programme du projet : `owl <projet>`
         * l'execute (en dernier, apres les compagnons). Casse-insensible :
         * Projet.owl / PROJET.OWL valent autant. Le stem "projet" n'est pas
         * `test_*`, donc jamais un test. */

        /* Extract stem (filename without .owl extension) */
        size_t stemLen = nameLen - 4;
        char* stem = malloc(stemLen + 1);
        memcpy(stem, name, stemLen);
        stem[stemLen] = '\0';

        /* Convention test runner : un fichier est un test ssi son nom
         * commence par `test_`. Pas de filtrage par dossier — la règle est
         * uniforme : `test_<name>.owl` est un test, point.
         * Le mode multi-fichier (poo_multifile / cf. ) est dérivé :
         * un `test_*.owl` qui a des `.owl` voisins non-test dans son
         * dossier active automatiquement le chargement multi-fichier. */
        bool isTest = (stemLen > 5 && strncmp(name, "test_", 5) == 0);

        project_add_file(project, fullPath, stem, isTest);

        free(stem);
        free(fullPath);
    }

    closedir(dir);
    return true;
}

/* ================================================================
 * INTERNAL: SORT FILES LEXICOGRAPHICALLY (entry first, then alpha)
 * ================================================================ */

static int compare_files(const void* a, const void* b) {
    const ProjectFile* fa = (const ProjectFile*)a;
    const ProjectFile* fb = (const ProjectFile*)b;

    /* L'entree est repositionnee en dernier par le runtime (project_run),
     * pas ici. Tri lexicographique pur des noms. */
    return strcmp(fa->name, fb->name);
}

static void project_sort(Project* project) {
    if (project->fileCount <= 1) return;
    qsort(project->files, project->fileCount, sizeof(ProjectFile), compare_files);
}

/* ================================================================
 * PUBLIC: DISCOVER
 * ================================================================ */

bool project_discover(Project* project, const char* dir) {
    return project_discover_ex(project, dir, true);
}

bool project_discover_ex(Project* project, const char* dir, bool skipTestDirs) {
    return project_discover_scoped(project, dir, skipTestDirs, true);
}

bool project_discover_scoped(Project* project, const char* dir,
                             bool skipTestDirs, bool recursive) {
    project->rootDir = proj_strdup(dir);

    if (!scan_directory(project, dir, skipTestDirs, recursive)) {
        return false;
    }

    if (project->fileCount == 0) {
        fprintf(stderr, "owl: no .owl files found in '%s'\n", dir);
        return false;
    }

    /* Coexistence des deux graphies bilingues du marqueur de projet
     * (projet.owl FR + project.owl EN) dans le meme projet = ambiguite :
     * on ne sait pas laquelle fait foi. L'utilisateur garde une seule
     * graphie. */
    if (skipTestDirs) {
        const char* projetFrPath = NULL;
        const char* projetEnPath = NULL;
        for (int i = 0; i < project->fileCount; i++) {
            if (strcasecmp(project->files[i].name, "projet") == 0)
                projetFrPath = project->files[i].path;
            else if (strcasecmp(project->files[i].name, "project") == 0)
                projetEnPath = project->files[i].path;
        }
        if (projetFrPath != NULL && projetEnPath != NULL) {
            fprintf(stderr,
                    "owl: deux marqueurs de projet ambigus (garder une seule "
                    "graphie) :\n");
            fprintf(stderr, "  %s\n", projetFrPath);
            fprintf(stderr, "  %s\n", projetEnPath);
            return false;
        }
    }

    /* Sort files lexicographically */
    project_sort(project);

    /* Read all file sources. Chokepoint unique : un fichier present dans
     * l'overlay est compile depuis le buffer injecte ( Lot 1) au lieu
     * du disque ; les autres restent lus du disque. */
    for (int i = 0; i < project->fileCount; i++) {
        const char* overlaySrc = project_overlay_lookup(project, project->files[i].path);
        if (overlaySrc != NULL) {
            project->files[i].source = proj_strdup(overlaySrc);
        } else {
            project->files[i].source = read_file(project->files[i].path);
        }
        if (project->files[i].source == NULL) {
            return false;
        }
    }

    return true;
}

/* ================================================================
 * PUBLIC: FIND FILE BY BASENAME
 * ================================================================ */

int project_find_file(Project* project, const char* basename) {
    /* basename can be "utils.owl" or just "utils" */
    size_t len = strlen(basename);
    const char* stem = basename;
    char stemBuf[256];

    /* Strip `.owl` casse-insensible : `MAIN.OWL`, `Utils.Owl`, `foo.owl`
     * doivent tous donner le stem nu pour le lookup. */
    if (len > 4 && strcasecmp(basename + len - 4, ".owl") == 0) {
        size_t stemLen = len - 4;
        if (stemLen >= sizeof(stemBuf)) stemLen = sizeof(stemBuf) - 1;
        memcpy(stemBuf, basename, stemLen);
        stemBuf[stemLen] = '\0';
        stem = stemBuf;
    }

    /* Lookup casse-insensible : coherence avec la reconnaissance de
     * `MAIN.OWL` comme entry au scan. Sinon `owl MAIN.OWL` echouerait
     * meme quand le scan a bien indexe le fichier. */
    for (int i = 0; i < project->fileCount; i++) {
        if (strcasecmp(project->files[i].name, stem) == 0) {
            return i;
        }
    }
    return -1;
}
