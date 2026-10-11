// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"
#include "memory.h"
#include "owl_errors.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
  #include <share.h>      /* _fsopen, _SH_DENY* */
  /* windows.h redéfinit TokenType, qui collide avec scanner.h OWL.
   * On déclare juste les symboles nécessaires (GetFileAttributesA +
   * la constante hidden + INVALID_FILE_ATTRIBUTES). */
  #ifndef FILE_ATTRIBUTE_HIDDEN
    #define FILE_ATTRIBUTE_HIDDEN 0x00000002
  #endif
  #ifndef INVALID_FILE_ATTRIBUTES
    #define INVALID_FILE_ATTRIBUTES ((unsigned long)-1)
  #endif
  __declspec(dllimport) unsigned long __stdcall GetFileAttributesA(const char* lpFileName);
  __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
  #define owl_mkdir_one(path)  _mkdir(path)
  #define owl_rmdir(path)      _rmdir(path)
  #define owl_getcwd(buf, sz)  _getcwd(buf, sz)
  #define owl_chdir(path)      _chdir(path)
  #define PATH_SEP_CHAR        '\\'
  #define PATH_SEP_ALT         '/'
#else
  #include <unistd.h>
  #include <sys/file.h>   /* flock */
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>  /* _NSGetExecutablePath */
  #endif
  #define owl_mkdir_one(path)  mkdir(path, 0755)
  #define owl_rmdir(path)      rmdir(path)
  #define owl_getcwd(buf, sz)  getcwd(buf, sz)
  #define owl_chdir(path)      chdir(path)
  #define PATH_SEP_CHAR        '/'
  #define PATH_SEP_ALT         '\\'
#endif

/* Détecte si une entrée de répertoire est "cachée" au sens OWL :
 *  - convention POSIX : nom commençant par '.'
 *  - Windows : OR attribut FILE_ATTRIBUTE_HIDDEN
 * Convention unifiée pour que '.foo' soit caché quel que soit l'OS. */
static bool entry_is_hidden(const char* baseName, const char* fullPath) {
    if (baseName != NULL && baseName[0] == '.') return true;
#ifdef _WIN32
    unsigned long attrs = GetFileAttributesA(fullPath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN)) {
        return true;
    }
#else
    (void)fullPath;
#endif
    return false;
}

/* Emit a structured native error for a failed I/O operation.
 * `op` is the native name (used as {native} slot), `path` may be NULL. */
static void file_error(const char* op, const char* path) {
    owl_emit_native_error(E_NATIVE_IO_FAILURE, op,
                          "native", op,
                          "path", path != NULL ? path : "?",
                          "detail", strerror(errno),
                          NULL);
}

/* ================================================================
 * FILE I/O
 * Handles are 1-based (OWL convention). 0 = invalid.
 * ================================================================ */

#define MAX_OPEN_FILES 32
static FILE* openFiles[MAX_OPEN_FILES];
static bool fileSlotUsed[MAX_OPEN_FILES];
static char openFilePaths[MAX_OPEN_FILES][260]; /* path memorisé pour .NomFichier */
static bool fileSlotDeleteOnClose[MAX_OPEN_FILES]; /* foSuppressionAutomatique */

/* Extract file handle (1-based) from a Value.
 * Accepts: integer/real handle, or FichierDisque ObjInstance (lit _handle).
 * Returns 0 if invalid. */
static int extract_file_handle(Value v) {
    if (IS_INTEGER(v)) return (int)AS_INTEGER(v);
    if (IS_REAL(v)) return (int)AS_REAL(v);
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName != NULL
            && strcmp(inst->structName->chars, "FichierDisque") == 0) {
            Value h;
            /*  lot 6 : root inst pendant obj_copy_string ; inst n'est
             * sur la VM stack qu'au moment de l'appel mais on est dans un
             * helper C, defensive push. */
            vm_push(v);
            ObjString* key = obj_copy_string("_handle", 7);
            vm_pop();
            if (table_get(&inst->fields, key, &h)) {
                if (IS_INTEGER(h)) return (int)AS_INTEGER(h);
            }
        }
    }
    return 0;
}

/* fOuvre mode constants — defined early so fn_fopen can use them */
/* fo* mode constants : centralisees dans sentinels.h, bit-flags combinables */
#define FO_LECTURE                OWL_FO_LECTURE
#define FO_ECRITURE               OWL_FO_ECRITURE
#define FO_LECTURE_ECRITURE       OWL_FO_LECTURE_ECRITURE
#define FO_AJOUT                  OWL_FO_AJOUT
#define FO_ACCESS_MASK            OWL_FO_ACCESS_MASK

#define FO_CREATION               OWL_FO_CREATION
#define FO_CREATION_SI_INEXISTANT OWL_FO_CREATION_SI_INEXISTANT
#define FO_CREATION_MASK          OWL_FO_CREATION_MASK

#define FO_BLOQUE_LECTURE         OWL_FO_BLOQUE_LECTURE
#define FO_BLOQUE_ECRITURE        OWL_FO_BLOQUE_ECRITURE
#define FO_SUPPRESSION_AUTO       OWL_FO_SUPPRESSION_AUTO
#define FO_ACCES_SEQUENTIEL       OWL_FO_ACCES_SEQUENTIEL

/* fPositionne origin constants : sentinelles centralisees dans sentinels.h */
#include "sentinels.h"
#define FP_DEBUT    OWL_FP_DEBUT
#define FP_COURANT  OWL_FP_COURANT
#define FP_FIN      OWL_FP_FIN

static int alloc_file_slot(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!fileSlotUsed[i]) return i;
    }
    return -1;
}

/* fOuvre(path, mode) / fOpen(path, mode) -> handle (1-based) or -1.
 * Le mode est une combinaison bitwise de constantes fo* (canon 3036036).
 * On décompose en familles : accès, création, encodage, blocage, divers.
 * Encodage et blocage sont acceptés syntaxiquement mais no-op en OWL
 * (UTF-8 unifié, pas de file locking inter-process). */
static Value fn_fopen(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return INTEGER_VAL(-1);
    const char* path = AS_CSTRING(args[0]);

    int mode = 0;
    if (argCount >= 2) {
        if (IS_INTEGER(args[1])) mode = (int)AS_INTEGER(args[1]);
        else if (IS_REAL(args[1])) mode = (int)AS_REAL(args[1]);
    }

    /* Famille accès : si rien n'est précisé, défaut canon = foLectureEcriture. */
    int access = mode & FO_ACCESS_MASK;
    if (access == 0) access = FO_LECTURE_ECRITURE;

    /* Famille création : 0 = ouvrir existant ; FO_CREATION = créer/écraser ;
     * FO_CREATION_SI_INEXISTANT = ne créer que si absent. */
    int creation = mode & FO_CREATION_MASK;

    /* Pour foCreationSiInexistant, vérifier l'absence avant de tenter
     * la création (sinon erreur canon). */
    if (creation == FO_CREATION_SI_INEXISTANT) {
        FILE* check = fopen(path, "rb");
        if (check != NULL) {
            fclose(check);
            file_error("fOuvre", path);
            return INTEGER_VAL(-1);
        }
    }

    /* Mapping (access, creation) → mode fopen() C. */
    const char* modeStr;
    if (access == FO_AJOUT) {
        /* "ab" crée le fichier si absent par défaut. */
        modeStr = "ab";
    } else if (creation == FO_CREATION || creation == FO_CREATION_SI_INEXISTANT) {
        /* Création/écrasement : "wb" seul (écriture seule) ou "w+b" (R+W). */
        if (access == FO_LECTURE_ECRITURE || access == FO_LECTURE) {
            modeStr = "w+b";
        } else {
            modeStr = "wb";  /* foEcriture pure */
        }
    } else {
        /* Ouvrir un fichier existant. */
        if (access == FO_LECTURE)        modeStr = "rb";
        else                              modeStr = "r+b"; /* écriture/lecture+écriture */
    }

    int slot = alloc_file_slot();
    if (slot < 0) {
        owl_emit_native_error(E_NATIVE_IO_TOO_MANY_OPEN, "fOuvre",
                              "native", "fOuvre",
                              NULL);
        return INTEGER_VAL(-1);
    }

    /* foBloqueEcriture / foBloqueLecture : verrou de partage exclusif.
     *  - foBloqueEcriture : refuse aux autres process d'OUVRIR en écriture
     *    (lecture concurrente tolérée).
     *  - foBloqueLecture  : refuse aux autres process de LIRE le fichier.
     *  Combinables (deny read+write).
     * Implémenté par :
     *  - Windows : _fsopen avec le flag de partage _SH_DENY* adéquat →
     *    l'OS refuse toute ouverture concurrente interdite jusqu'à fclose
     *    (verrou natif, distingue read/write).
     *  - POSIX : fopen + flock(LOCK_EX | LOCK_NB) advisory. flock ne
     *    distingue pas deny-read de deny-write : un verrou exclusif est
     *    posé dès qu'un des deux flags est demandé (best-effort, visible
     *    seulement des autres process qui font flock aussi). */
    bool blockWrite = (mode & FO_BLOQUE_ECRITURE) != 0;
    bool blockRead  = (mode & FO_BLOQUE_LECTURE) != 0;
    FILE* f;
#ifdef _WIN32
    if (blockRead && blockWrite) {
        f = _fsopen(path, modeStr, _SH_DENYRW);
    } else if (blockRead) {
        f = _fsopen(path, modeStr, _SH_DENYRD);
    } else if (blockWrite) {
        f = _fsopen(path, modeStr, _SH_DENYWR);
    } else {
        f = fopen(path, modeStr);
    }
#else
    f = fopen(path, modeStr);
#endif
    if (f == NULL) { file_error("fOuvre", path); return INTEGER_VAL(-1); }

#ifndef _WIN32
    if (blockWrite || blockRead) {
        if (flock(fileno(f), LOCK_EX | LOCK_NB) != 0) {
            fclose(f);
            file_error("fOuvre", path);
            return INTEGER_VAL(-1);
        }
    }
#endif

    openFiles[slot] = f;
    fileSlotUsed[slot] = true;
    fileSlotDeleteOnClose[slot] = (mode & FO_SUPPRESSION_AUTO) != 0;
    /* Memorize path for .NomFichier method on FichierDisque */
    size_t plen = strlen(path);
    if (plen < sizeof(openFilePaths[slot])) {
        memcpy(openFilePaths[slot], path, plen + 1);
    } else {
        openFilePaths[slot][0] = '\0';
    }
    owl_clear_error();
    return INTEGER_VAL(slot + 1); /* 1-based */
}

/* fFerme(handle) / fClose — doc 3036027.
 * Retour: 0 si fermé avec succès, -1 sur erreur. */
static Value fn_fclose(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        owl_emit_native_error(E_NATIVE_IO_INVALID_HANDLE, "fFerme",
                              "native", "fFerme",
                              NULL);
        return INTEGER_VAL(-1);
    }
    int rc = fclose(openFiles[slot]);
    /* Snapshot avant reset pour foSuppressionAutomatique. */
    bool autoDelete = fileSlotDeleteOnClose[slot];
    char pathToDelete[260];
    if (autoDelete) {
        memcpy(pathToDelete, openFilePaths[slot], sizeof(pathToDelete));
    }
    openFiles[slot] = NULL;
    fileSlotUsed[slot] = false;
    fileSlotDeleteOnClose[slot] = false;
    openFilePaths[slot][0] = '\0';
    if (rc != 0) { file_error("fFerme", "erreur de fermeture"); return INTEGER_VAL(-1); }
    if (autoDelete && pathToDelete[0] != '\0') {
        /* foSuppressionAutomatique : retire le fichier après fermeture
         * (remove() = portable Windows + POSIX). Échec silencieux : la
         * sémantique canon est "best-effort delete on close". */
        remove(pathToDelete);
    }
    owl_clear_error();
    return INTEGER_VAL(0);
}

/* Parser de coerce vers FichierDisque — enregistre via le registry.
 * - Integer handle (from fOuvre): wraps into ObjInstance "FichierDisque"
 * - Already a FichierDisque: pass-through
 * - Invalid (handle <= 0, fOuvre failed): returns NIL_VAL
 *
 * Le compilo emet __InstanceCoerce(v, "FichierDisque") qui dispatch ici. */
static Value parse_to_fichierdisque(Value v) {
    /* Pass-through if already FichierDisque */
    if (IS_INSTANCE(v)) {
        ObjInstance* inst = AS_INSTANCE(v);
        if (inst->structName != NULL
            && strcmp(inst->structName->chars, "FichierDisque") == 0) {
            return v;
        }
    }
    int handle = 0;
    if (IS_INTEGER(v)) handle = (int)AS_INTEGER(v);
    else if (IS_REAL(v)) handle = (int)AS_REAL(v);
    else return NIL_VAL;
    if (handle <= 0 || handle > MAX_OPEN_FILES) return NIL_VAL;
    int slot = handle - 1;
    if (!fileSlotUsed[slot]) return NIL_VAL;

    /*  lot 6 : root name pendant obj_new_instance ; populate via
     * helpers canon inst_field_set_* qui rootent inst/key/val. */
    ObjString* name = obj_copy_string("FichierDisque", 13);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_pop();
    inst_field_set_int(inst, "_handle", handle);
    inst_field_set_int(inst, "Handle", handle);
    inst_field_set_str(inst, "NomFichier", openFilePaths[slot]);
    return OBJ_VAL(inst);
}

/* fTaille(FichierDisque) / method .Taille() — size of currently open file.
 * Différent de fTaille(chaîne) qui stat(path). Ici on opère sur le handle. */
static Value fn_ftaille_open(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) return INTEGER_VAL(-1);
    FILE* f = openFiles[slot];
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return INTEGER_VAL((int64_t)end);
}

/* fNomFichier(FichierDisque) / method .NomFichier() — path of the open file. */
static Value fn_fnom_fichier(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    return OBJ_VAL(obj_copy_string(openFilePaths[slot],
                                   (int)strlen(openFilePaths[slot])));
}

/* fEcrit(handle, text|buffer [, taille]) / fWrite — doc 3036014.
 * Retour: nombre d'octets écrits (entier), -1 sur erreur. */
static Value fn_fwrite(int argCount, Value* args) {
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) return INTEGER_VAL(-1);
    const char* data = NULL; int len = 0;
    if (IS_STRING(args[1])) {
        data = AS_CSTRING(args[1]); len = AS_STRING(args[1])->length;
    } else if (IS_BUFFER(args[1])) {
        ObjBuffer* b = AS_BUFFER(args[1]); data = (const char*)b->bytes; len = b->length;
    } else {
        return INTEGER_VAL(-1);
    }
    if (argCount >= 3 && IS_INTEGER(args[2])) {
        int64_t maxLen = AS_INTEGER(args[2]);
        if (maxLen >= 0 && maxLen < len) len = (int)maxLen;
    }
    size_t n = fwrite(data, 1, (size_t)len, openFiles[slot]);
    if (n != (size_t)len) { file_error("fEcrit", "écriture partielle"); return INTEGER_VAL(-1); }
    return INTEGER_VAL((int64_t)n);
}

/* fEcritLigne(handle, text) / fWriteLine — doc 3036025.
 * Retour: Booléen (Vrai si succès, Faux sinon). */
static Value fn_fwriteline(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) return BOOL_VAL(false);
    if (!IS_STRING(args[1])) return BOOL_VAL(false);
    const char* text = AS_CSTRING(args[1]);
    int len = AS_STRING(args[1])->length;
    size_t n = fwrite(text, 1, (size_t)len, openFiles[slot]);
    if (n != (size_t)len) return BOOL_VAL(false);
    if (fputc('\n', openFiles[slot]) == EOF) return BOOL_VAL(false);
    return BOOL_VAL(true);
}

/* fLitLigne(handle) / fReadLine(handle) -> string */
static Value fn_freadline(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        return OBJ_VAL(obj_copy_string("", 0));
    }

    char buf[4096];
    if (fgets(buf, (int)sizeof(buf), openFiles[slot]) == NULL) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    /* Strip trailing newline */
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        len--;
    }
    return OBJ_VAL(obj_copy_string(buf, len));
}

/* fLitTout(handle) / fReadAll(handle) -> string */
static Value fn_freadall(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        return OBJ_VAL(obj_copy_string("", 0));
    }

    FILE* f = openFiles[slot];
    long startPos = ftell(f);
    fseek(f, 0, SEEK_END);
    long endPos = ftell(f);
    fseek(f, startPos, SEEK_SET);

    long size = endPos - startPos;
    if (size <= 0) return OBJ_VAL(obj_copy_string("", 0));
    /* Guard against files too large for our int-based string length */
    if (size > INT_MAX - 1) {
        return OBJ_VAL(obj_copy_string("", 0));
    }

    char* buf = ALLOCATE(char, (int)size + 1);
    size_t readBytes = fread(buf, 1, (size_t)size, f);
    buf[readBytes] = '\0';
    ObjString* str = obj_copy_string(buf, (int)readBytes);
    FREE_ARRAY(char, buf, (int)size + 1);
    return OBJ_VAL(str);
}

/* fFinFichier(handle) / fEOF(handle) -> bool */
static Value fn_feof(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        return BOOL_VAL(true);
    }
    return BOOL_VAL(feof(openFiles[slot]) != 0);
}

/* fFichierExiste(path) / fFileExist(path) -> bool.
 * Teste un FICHIER regulier seul : Faux sur un repertoire (symetrique
 * de fn_dir_exists qui filtre S_IFDIR). Aligne sur le canon. */
static Value fn_ffichier_existe(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const char* path = AS_CSTRING(args[0]);
    struct stat st;
    if (stat(path, &st) != 0) return BOOL_VAL(false);
    return BOOL_VAL((st.st_mode & S_IFMT) == S_IFREG);
}

/* fSupprime(path) / fDelete(path) -> bool */
static Value fn_fdelete(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const char* path = AS_CSTRING(args[0]);
    if (remove(path) != 0) { file_error("fSupprime", path); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Lit l'argument encodage optionnel des I/O texte ; défaut encUTF8
 * (passthrough octets, rétro-compatible). `idx` = position de l'arg
 * encodage (1 pour fChargeTexte, 2 pour fSauveTexte/fAjouteTexte). */
static int file_arg_encoding(int argCount, Value* args, int idx) {
    if (argCount > idx && IS_INTEGER(args[idx])) return (int)AS_INTEGER(args[idx]);
    return (int)OWL_ENC_UTF8;
}

/* fChargeTexte(path [, encodage]) / fLoadText -> string.
 * Décode les octets disque depuis `encodage` vers l'UTF-8 interne. Défaut
 * encUTF8 = octets repris tels quels (aucune double conversion). */
static Value fn_floadtext(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    int enc = file_arg_encoding(argCount, args, 1);
    const char* path = AS_CSTRING(args[0]);

    FILE* f = fopen(path, "rb");
    if (f == NULL) { file_error("fChargeTexte", path); return OBJ_VAL(obj_copy_string("", 0)); }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    /* Guard against files too large for our int-based string length */
    if (size > INT_MAX - 1) {
        fclose(f);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    char* buf = ALLOCATE(char, (int)size + 1);
    size_t readBytes = fread(buf, 1, (size_t)size, f);
    buf[readBytes] = '\0';
    fclose(f);

    ObjString* str;
    if (enc == (int)OWL_ENC_UTF8) {
        str = obj_copy_string(buf, (int)readBytes);
    } else {
        int outLen = 0;
        unsigned char* out = owl_codec_decode((const unsigned char*)buf,
                                              (int)readBytes, enc, &outLen);
        str = obj_copy_string((const char*)out, outLen);
        free(out);
    }
    FREE_ARRAY(char, buf, (int)size + 1);
    owl_clear_error();
    return OBJ_VAL(str);
}

/* fSauveTexte(path, content [, encodage]) / fSaveText -> booléen.
 * Encode le contenu vers `encodage` avant écriture. Défaut encUTF8 =
 * octets UTF-8 internes tels quels. */
static Value fn_fsavetext(int argCount, Value* args) {
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    int enc = file_arg_encoding(argCount, args, 2);
    const char* path = AS_CSTRING(args[0]);
    ObjString* content = AS_STRING(args[1]);

    FILE* f = fopen(path, "wb");
    if (f == NULL) { file_error("fSauveTexte", path); return BOOL_VAL(false); }

    bool ok;
    if (enc == (int)OWL_ENC_UTF8) {
        ok = fwrite(content->chars, 1, (size_t)content->length, f)
             == (size_t)content->length;
    } else {
        int outLen = 0;
        unsigned char* out = owl_codec_encode(content->chars, content->length,
                                              enc, &outLen);
        ok = fwrite(out, 1, (size_t)outLen, f) == (size_t)outLen;
        free(out);
    }
    fclose(f);
    if (!ok) { file_error("fSauveTexte (ecriture partielle)", path); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * DIRECTORY AND PATH OPERATIONS
 * ================================================================ */

static bool is_path_sep(char c) { return c == '/' || c == '\\'; }

/* fRepExiste(path) — directory exists? */
static Value fn_dir_exists(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    struct stat st;
    if (stat(AS_CSTRING(args[0]), &st) != 0) return BOOL_VAL(false);
    return BOOL_VAL((st.st_mode & S_IFMT) == S_IFDIR);
}

/* fRepCree(path) — creates a directory; creates intermediate ones as needed. */
static Value fn_dir_create(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const char* src = AS_CSTRING(args[0]);
    int len = AS_STRING(args[0])->length;
    char* path = (char*)malloc((size_t)len + 1);
    memcpy(path, src, (size_t)len);
    path[len] = 0;

    for (int i = 0; i < len; i++) {
        if (is_path_sep(path[i]) && i > 0) {
            char saved = path[i];
            path[i] = 0;
            struct stat st;
            if (stat(path, &st) != 0) {
                if (owl_mkdir_one(path) != 0 && errno != EEXIST) {
                    file_error("fRepCree (intermediaire)", path);
                    path[i] = saved;
                    free(path);
                    return BOOL_VAL(false);
                }
            }
            path[i] = saved;
        }
    }
    struct stat st;
    bool ok = true;
    if (stat(path, &st) != 0) {
        ok = (owl_mkdir_one(path) == 0);
        if (!ok) file_error("fRepCree", path);
    }
    free(path);
    if (ok) owl_clear_error();
    return BOOL_VAL(ok);
}

/* Recursively remove a directory and its contents. Used when fRepSupprime is
 * called with a truthy second argument. */
static bool rmdir_recursive(const char* path) {
    DIR* d = opendir(path);
    if (d == NULL) return false;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
            rmdir_recursive(child);
        } else {
            remove(child);
        }
    }
    closedir(d);
    return owl_rmdir(path) == 0;
}

/* fRepSupprime(path, [recursif]) — removes directory. If recursive flag is
 * truthy, also deletes contained files/subdirs. */
static Value fn_dir_remove(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const char* path = AS_CSTRING(args[0]);
    bool recursive = false;
    if (argCount >= 2) value_to_bool(args[1], &recursive);
    bool ok = recursive ? rmdir_recursive(path) : (owl_rmdir(path) == 0);
    if (!ok) { file_error("fRepSupprime", path); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Constantes options pour fListeRépertoire/fListeFichier/fCopieFichier.
 * Doc 3036062 / 3036058 / 3036004. */
#define FR_RECURSIF             0x0001  /* défaut pour fListe* */
#define FR_NON_RECURSIF         0x0002
#define FR_INTERRUPTIBLE        0x0004
#define FR_INFORMATION_COMPLETE 0x0008
#define FR_SANS_FICHIER_CACHE   0x0010
#define FR_SANS_REPERTOIRE_CACHE 0x0020

/* Constantes <Change> du callback fListeFichier (doc 1000024673). */
#define FL_FICHIER             0
#define FL_PREMIER_FICHIER     1
#define FL_CHANGE_REPERTOIRE   2

/* glob_match: simple wildcard match for filenames.
 * Pattern is a simple "*.ext" or "prefix*" glob (v1). No pattern = all. */
static bool glob_match(const char* name, const char* pattern) {
    if (pattern == NULL || pattern[0] == 0) return true;
    /* "*" single-segment glob: allow '*' anywhere. */
    const char* n = name;
    const char* p = pattern;
    const char* star_n = NULL;
    const char* star_p = NULL;
    while (*n) {
        if (*p == '*') { star_p = p++; star_n = n; continue; }
        if (*p == *n || *p == '?') { p++; n++; continue; }
        if (star_p != NULL) { p = star_p + 1; n = ++star_n; continue; }
        return false;
    }
    while (*p == '*') p++;
    return *p == 0;
}

/* Helper: append string + CRLF to a dynamic buffer. */
typedef struct { char* data; int len; int cap; } StringBuf;
static void sbuf_init(StringBuf* sb) { sb->data = NULL; sb->len = 0; sb->cap = 0; }
static void sbuf_free(StringBuf* sb) { free(sb->data); sb->data = NULL; sb->len = sb->cap = 0; }
static void sbuf_append(StringBuf* sb, const char* s, int n) {
    if (sb->len + n + 2 >= sb->cap) {
        int newCap = sb->cap < 256 ? 256 : sb->cap * 2;
        while (newCap < sb->len + n + 2) newCap *= 2;
        char* p = (char*)realloc(sb->data, (size_t)newCap);
        if (p == NULL) return;
        sb->data = p; sb->cap = newCap;
    }
    memcpy(sb->data + sb->len, s, (size_t)n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}
static void sbuf_append_rc(StringBuf* sb) { sbuf_append(sb, "\r\n", 2); }

/* Format d'une entrée frInformationComplète :
 *   <nom complet> + TAB + <taille> + TAB + <date AAAAMMJJHHmmSS> + TAB + <attrs>
 * Attrs : R/H/S/A combinaison style fAttribut. Implémentation simplifiée. */
static void format_full_info(StringBuf* sb, const char* fullPath, const struct stat* st) {
    sbuf_append(sb, fullPath, (int)strlen(fullPath));
    char tmp[64];
    int n = snprintf(tmp, sizeof(tmp), "\t%lld\t",
                     (long long)st->st_size);
    sbuf_append(sb, tmp, n);
    struct tm* t = localtime(&st->st_mtime);
    n = snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d",
                 (t->tm_year + 1900) % 10000,
                 (t->tm_mon + 1) & 0xFF,
                 t->tm_mday & 0xFF,
                 t->tm_hour & 0xFF,
                 t->tm_min & 0xFF,
                 t->tm_sec & 0xFF);
    sbuf_append(sb, tmp, n);
    /* Attributs : H si répertoire caché (commence par .), sinon vide */
    sbuf_append(sb, "\t", 1);
    /* Pas d'attributs Windows-style pleinement supportés sur Unix — on laisse vide */
}

/* Walker récursif partagé : invoque cb(fullPath, isDir, st, userdata).
 * Callback retourne true pour continuer, false pour arrêter (propagé
 * jusqu'à la sortie complète du walk). Distingue aussi premier-fichier
 * et changement de répertoire (signalés au callback via *changeOut). */
typedef bool (*DirEntryCb)(const char* dirPath, const char* baseName,
                           const char* fullPath, bool isDir, int change,
                           const struct stat* st, void* userdata);

static bool walk_directory_ex(const char* dirPath, bool recursive,
                              DirEntryCb cb, void* userdata,
                              bool* topFirst) {
    DIR* d = opendir(dirPath);
    if (d == NULL) return true;
    struct dirent* e;
    char childPath[1024];
    bool dirFirst = true;  /* premier fichier rencontré dans ce dirPath */
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(childPath, sizeof(childPath), "%s%c%s",
                 dirPath, PATH_SEP_CHAR, e->d_name);
        struct stat st;
        if (stat(childPath, &st) != 0) continue;
        bool isDir = (st.st_mode & S_IFMT) == S_IFDIR;
        int change;
        if (topFirst != NULL && *topFirst) {
            change = FL_PREMIER_FICHIER; *topFirst = false; dirFirst = false;
        } else if (dirFirst) {
            change = FL_CHANGE_REPERTOIRE; dirFirst = false;
        } else {
            change = FL_FICHIER;
        }
        if (!cb(dirPath, e->d_name, childPath, isDir, change, &st, userdata)) {
            closedir(d);
            return false;
        }
        if (recursive && isDir) {
            if (!walk_directory_ex(childPath, true, cb, userdata, topFirst)) {
                closedir(d);
                return false;
            }
        }
    }
    closedir(d);
    return true;
}

static void walk_directory(const char* dirPath, bool recursive,
                           DirEntryCb cb, void* userdata) {
    bool topFirst = true;
    walk_directory_ex(dirPath, recursive, cb, userdata, &topFirst);
}

/* fListeRépertoire callback context */
typedef struct {
    StringBuf sb;
    bool firstEntry;
    int flags;
} ListDirCtx;

static bool list_dir_cb(const char* dirPath, const char* baseName,
                        const char* fullPath, bool isDir, int change,
                        const struct stat* st, void* userdata) {
    (void)dirPath; (void)change;
    ListDirCtx* ctx = (ListDirCtx*)userdata;
    if (!isDir) return true;  /* fListeRépertoire : sous-dossiers SEULEMENT */
    if ((ctx->flags & FR_SANS_REPERTOIRE_CACHE)
            && entry_is_hidden(baseName, fullPath)) {
        return true;  /* répertoire caché filtré */
    }
    if (!ctx->firstEntry) sbuf_append_rc(&ctx->sb);
    if (ctx->flags & FR_INFORMATION_COMPLETE) {
        format_full_info(&ctx->sb, fullPath, st);
    } else {
        sbuf_append(&ctx->sb, fullPath, (int)strlen(fullPath));
    }
    ctx->firstEntry = false;
    return true;
}

/* Contexte callback (forme 2) — invoque proc(rep, fichier, change, ptr).
 * Retour proc Vrai → continue, Faux → arrêt. */
typedef struct {
    Value cbCallable;
    Value userPtr;
    int   count;       /* nombre d'entrées listées (résultat de la forme 2) */
    bool  filesOnly;   /* true = fListeFichier, false = fListeRépertoire */
    int   flags;       /* options fr* (FR_SANS_FICHIER_CACHE / FR_SANS_REPERTOIRE_CACHE) */
    const char* pattern;
} ListCallbackCtx;

/* Invoke user procedure. Returns false to abort the walk. */
static bool invoke_user_callback(ListCallbackCtx* ctx, const char* dirPath,
                                 const char* baseName, int change) {
    /* canon : <Chemin> contient toujours un séparateur final. */
    char repWithSep[1024];
    int dl = (int)strlen(dirPath);
    if (dl >= (int)sizeof(repWithSep) - 2) dl = (int)sizeof(repWithSep) - 2;
    memcpy(repWithSep, dirPath, (size_t)dl);
    if (dl == 0 || !is_path_sep(repWithSep[dl - 1])) {
        repWithSep[dl++] = PATH_SEP_CHAR;
    }
    repWithSep[dl] = '\0';

    vm_push(ctx->cbCallable);
    vm_push(OBJ_VAL(obj_copy_string(repWithSep, dl)));
    vm_push(OBJ_VAL(obj_copy_string(baseName, (int)strlen(baseName))));
    vm_push(INTEGER_VAL(change));
    vm_push(ctx->userPtr);
    InterpretResult r = vm_call_reentrant(ctx->cbCallable, 4);
    if (r != INTERPRET_OK) return false;
    Value ret = vm_pop();
    /* Vrai (ou int != 0) = continue, Faux (ou 0) = stop. */
    if (IS_BOOL(ret)) return AS_BOOL(ret);
    if (IS_INTEGER(ret)) return AS_INTEGER(ret) != 0;
    return true;  /* défaut conservateur : on continue */
}

static bool list_callback_cb(const char* dirPath, const char* baseName,
                             const char* fullPath, bool isDir, int change,
                             const struct stat* st, void* userdata) {
    (void)fullPath; (void)st;
    ListCallbackCtx* ctx = (ListCallbackCtx*)userdata;
    /* Filter par type d'entité */
    if (ctx->filesOnly && isDir) return true;
    if (!ctx->filesOnly && !isDir) return true;
    /* Pattern (uniquement fListeFichier) */
    if (ctx->pattern && !glob_match(baseName, ctx->pattern)) return true;
    /* Filtre cachés selon l'option fr* correspondante au type d'entité */
    int hiddenMask = ctx->filesOnly ? FR_SANS_FICHIER_CACHE : FR_SANS_REPERTOIRE_CACHE;
    if ((ctx->flags & hiddenMask) && entry_is_hidden(baseName, fullPath)) {
        return true;
    }
    ctx->count++;
    return invoke_user_callback(ctx, dirPath, baseName, change);
}

/* fListeRépertoire(<chemin> [, <options>])                            — forme 1
 * fListeRépertoire(<chemin>, <callback> [, <pointeur> [, <options>]]) — forme 2
 * Doc 3036062. */
static Value fn_dir_list(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    const char* dirPath = AS_CSTRING(args[0]);

    /* Forme 2 : 2e arg est callable */
    if (argCount >= 2 && IS_CALLABLE(args[1])) {
        ListCallbackCtx ctx;
        ctx.cbCallable = args[1];
        ctx.userPtr    = (argCount >= 3) ? args[2] : INTEGER_VAL(0);
        ctx.count      = 0;
        ctx.filesOnly  = false;
        ctx.pattern    = NULL;
        int flags = 0;
        if (argCount >= 4 && IS_INTEGER(args[3])) flags = (int)AS_INTEGER(args[3]);
        ctx.flags = flags;
        bool recursive = !(flags & FR_NON_RECURSIF);
        walk_directory(dirPath, recursive, list_callback_cb, &ctx);
        return INTEGER_VAL(ctx.count);
    }

    int flags = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) flags = (int)AS_INTEGER(args[1]);
    bool recursive = !(flags & FR_NON_RECURSIF);  /* récursif sauf opt-out */

    ListDirCtx ctx; sbuf_init(&ctx.sb);
    ctx.firstEntry = true;
    ctx.flags = flags;
    walk_directory(dirPath, recursive, list_dir_cb, &ctx);
    Value v = OBJ_VAL(obj_copy_string(ctx.sb.data ? ctx.sb.data : "", ctx.sb.len));
    sbuf_free(&ctx.sb);
    return v;
}

/* fListeFichier callback context */
typedef struct {
    StringBuf sb;
    bool firstEntry;
    int flags;
    const char* pattern;  /* glob pattern from path basename, NULL = match all */
} ListFileCtx;

static bool list_file_cb(const char* dirPath, const char* baseName,
                         const char* fullPath, bool isDir, int change,
                         const struct stat* st, void* userdata) {
    (void)dirPath; (void)change;
    ListFileCtx* ctx = (ListFileCtx*)userdata;
    if (isDir) return true;  /* fListeFichier : fichiers SEULEMENT */
    if (ctx->pattern && !glob_match(baseName, ctx->pattern)) return true;
    if ((ctx->flags & FR_SANS_FICHIER_CACHE)
            && entry_is_hidden(baseName, fullPath)) {
        return true;  /* fichier caché filtré */
    }
    if (!ctx->firstEntry) sbuf_append_rc(&ctx->sb);
    if (ctx->flags & FR_INFORMATION_COMPLETE) {
        format_full_info(&ctx->sb, fullPath, st);
    } else {
        sbuf_append(&ctx->sb, fullPath, (int)strlen(fullPath));
    }
    ctx->firstEntry = false;
    return true;
}

/* Sépare un raw "<dir>/<pattern>" en dir + pattern (NULL si pas de glob).
 * `dirPath` doit avoir au moins 1024 octets. */
static void split_path_pattern(const char* raw, char* dirPath, size_t dirCap,
                               const char** outPattern) {
    *outPattern = NULL;
    int rawLen = (int)strlen(raw);
    int sep = -1;
    for (int i = 0; i < rawLen; i++) {
        if (is_path_sep(raw[i])) sep = i;
    }
    if (sep >= 0 && (strchr(raw + sep + 1, '*') || strchr(raw + sep + 1, '?'))) {
        int n = sep > 0 ? sep : 1;
        if (n >= (int)dirCap) n = (int)dirCap - 1;
        memcpy(dirPath, raw, (size_t)n);
        dirPath[n] = '\0';
        *outPattern = raw + sep + 1;
    } else if (strchr(raw, '*') || strchr(raw, '?')) {
        snprintf(dirPath, dirCap, ".");
        *outPattern = raw;
    } else {
        int n = rawLen;
        if (n >= (int)dirCap) n = (int)dirCap - 1;
        memcpy(dirPath, raw, (size_t)n);
        dirPath[n] = '\0';
    }
}

/* fListeFichier(<chemin+pattern> [, <options>])                            — forme 1
 * fListeFichier(<chemin+pattern>, <callback> [, <pointeur> [, <options>]]) — forme 2
 * Doc 3036058. */
static Value fn_file_list(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    const char* raw = AS_CSTRING(args[0]);
    char dirPath[1024];
    const char* pattern = NULL;
    split_path_pattern(raw, dirPath, sizeof(dirPath), &pattern);

    /* Forme 2 : 2e arg est callable */
    if (argCount >= 2 && IS_CALLABLE(args[1])) {
        ListCallbackCtx ctx;
        ctx.cbCallable = args[1];
        ctx.userPtr    = (argCount >= 3) ? args[2] : INTEGER_VAL(0);
        ctx.count      = 0;
        ctx.filesOnly  = true;
        ctx.pattern    = pattern;
        int flags = 0;
        if (argCount >= 4 && IS_INTEGER(args[3])) flags = (int)AS_INTEGER(args[3]);
        ctx.flags = flags;
        bool recursive = !(flags & FR_NON_RECURSIF);
        walk_directory(dirPath, recursive, list_callback_cb, &ctx);
        return INTEGER_VAL(ctx.count);
    }

    int flags = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) flags = (int)AS_INTEGER(args[1]);
    bool recursive = !(flags & FR_NON_RECURSIF);

    ListFileCtx ctx; sbuf_init(&ctx.sb);
    ctx.firstEntry = true;
    ctx.flags = flags;
    ctx.pattern = pattern;
    walk_directory(dirPath, recursive, list_file_cb, &ctx);
    Value v = OBJ_VAL(obj_copy_string(ctx.sb.data ? ctx.sb.data : "", ctx.sb.len));
    sbuf_free(&ctx.sb);
    return v;
}

/* (constantes définies plus haut, avant fn_dir_list) */

static bool glob_match(const char* name, const char* pattern);

/* Does a path contain a glob wildcard? */
static bool path_has_wildcard(const char* s) {
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

/* Single file copy helper (reused by glob path). Returns true on success. */
static bool file_copy_single_impl(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (in == NULL) return false;
    FILE* out = fopen(dst, "wb");
    if (out == NULL) { fclose(in); return false; }
    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in); fclose(out);
    return ok;
}

/* Glob copy: copies files matching `pattern` in `srcDir` into `dstDir`.
 * With `recursive`, descends into subdirs (recreating tree under dstDir). */
static bool file_copy_glob(const char* srcDir, const char* pattern,
                           const char* dstDir, bool recursive) {
    /* Ensure dstDir exists */
    struct stat dstStat;
    if (stat(dstDir, &dstStat) != 0) {
        if (owl_mkdir_one(dstDir) != 0 && errno != EEXIST) return false;
    }
    DIR* d = opendir(srcDir);
    if (d == NULL) return false;
    struct dirent* e;
    char childSrc[1024], childDst[1024];
    bool ok = true;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(childSrc, sizeof(childSrc), "%s/%s", srcDir, e->d_name);
        snprintf(childDst, sizeof(childDst), "%s/%s", dstDir, e->d_name);
        struct stat st;
        if (stat(childSrc, &st) != 0) continue;
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            if (recursive) {
                if (!file_copy_glob(childSrc, pattern, childDst, true)) ok = false;
            }
        } else if (glob_match(e->d_name, pattern)) {
            if (!file_copy_single_impl(childSrc, childDst)) ok = false;
        }
    }
    closedir(d);
    return ok;
}

/* fCopieFichier(src, dst [, flags]) — doc 3036004.
 * Forme simple: src/dst = chemins fichier. Copie un fichier.
 * Forme glob  : src contient *, ? — copie tous les fichiers matchant
 *               le pattern dans dst (qui doit être un dossier).
 *               Avec flag `frRécursif`, descend dans les sous-dossiers. */
static Value fn_file_copy(int argCount, Value* args) {
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    const char* srcRaw = AS_CSTRING(args[0]);
    const char* dstRaw = AS_CSTRING(args[1]);
    int flags = 0;
    if (argCount >= 3 && IS_INTEGER(args[2])) flags = (int)AS_INTEGER(args[2]);

    if (path_has_wildcard(srcRaw)) {
        /* Split src into dir + pattern (last path component = pattern) */
        const char* lastSep = NULL;
        for (const char* p = srcRaw; *p; p++) {
            if (is_path_sep(*p)) lastSep = p;
        }
        char srcDir[1024];
        const char* pattern;
        if (lastSep == NULL) {
            snprintf(srcDir, sizeof(srcDir), ".");
            pattern = srcRaw;
        } else {
            int dl = (int)(lastSep - srcRaw);
            if (dl == 0) { srcDir[0] = PATH_SEP_CHAR; srcDir[1] = '\0'; }
            else {
                if (dl >= (int)sizeof(srcDir)) dl = (int)sizeof(srcDir) - 1;
                memcpy(srcDir, srcRaw, (size_t)dl);
                srcDir[dl] = '\0';
            }
            pattern = lastSep + 1;
        }
        /* Strip trailing separator from dst if any */
        char dst[1024];
        int dl = (int)strlen(dstRaw);
        if (dl >= (int)sizeof(dst)) dl = (int)sizeof(dst) - 1;
        memcpy(dst, dstRaw, (size_t)dl); dst[dl] = '\0';
        if (dl > 0 && is_path_sep(dst[dl - 1])) dst[dl - 1] = '\0';

        bool ok = file_copy_glob(srcDir, pattern, dst, (flags & FR_RECURSIF) != 0);
        if (!ok) { file_error("fCopieFichier (glob)", srcRaw); return BOOL_VAL(false); }
        owl_clear_error();
        return BOOL_VAL(true);
    }

    /* Single-file fallback — garder les labels "(source)" / "(destination)"
     * pour rester compatible avec les tests de message d'erreur existants. */
    FILE* in = fopen(srcRaw, "rb");
    if (in == NULL) { file_error("fCopieFichier (source)", srcRaw); return BOOL_VAL(false); }
    FILE* out = fopen(dstRaw, "wb");
    if (out == NULL) { file_error("fCopieFichier (destination)", dstRaw); fclose(in); return BOOL_VAL(false); }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            file_error("fCopieFichier (ecriture)", dstRaw);
            fclose(in); fclose(out);
            return BOOL_VAL(false);
        }
    }
    fclose(in); fclose(out);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fRenomme(src, dst) / fDeplace — renames or moves (same filesystem). */
static Value fn_file_rename(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    if (rename(AS_CSTRING(args[0]), AS_CSTRING(args[1])) != 0) {
        file_error("fRenomme", AS_CSTRING(args[0]));
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fRepEnCours([newPath]) — doc 3036006.
 *  - 0 arg : retourne le dossier courant (chaîne).
 *  - 1 arg (chaîne) : change le dossier courant (canon accepte les
 *    espaces de fin, qui sont strippés). Retourne le NOUVEAU dossier
 *    courant (la chaîne effectivement settée).
 * Note : la doc canon mélange getter/setter sous le même nom. */
static Value fn_dir_cwd(int argCount, Value* args) {
    if (argCount >= 1 && IS_STRING(args[0])) {
        ObjString* req = AS_STRING(args[0]);
        /* canon : strip espaces de fin sur le chemin (ex2 doc avec
         * "C:\MonRépertoire\    "). */
        int len = req->length;
        while (len > 0 && (req->chars[len - 1] == ' ' || req->chars[len - 1] == '\t')) len--;
        char path[1024];
        int copy = len < (int)sizeof(path) - 1 ? len : (int)sizeof(path) - 1;
        memcpy(path, req->chars, copy);
        path[copy] = '\0';
        if (owl_chdir(path) != 0) {
            file_error("fRepEnCours", path);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        owl_clear_error();
        /* Fall through pour retourner le cwd actuel (le canon retourne
         * la nouvelle valeur). */
    }
    char buf[1024];
    if (owl_getcwd(buf, sizeof(buf)) == NULL) return OBJ_VAL(obj_copy_string("", 0));
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* fChangeRepertoire(path) -> bool */
static Value fn_dir_chdir(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    if (owl_chdir(AS_CSTRING(args[0])) != 0) {
        file_error("fChangeRepertoire", AS_CSTRING(args[0]));
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fExtraitChemin(path, mode) — slice a path. Mode values are declared as
 * an OwlEnum below (E_fExtraitPartie) so LSP completion and the compile
 * checker know exactly which constants are accepted. */
static const char* const E_fExtraitPartie_values[] = {
    "fDisque", "fRepertoire", "fFichier", "fExtension",
    NULL
};
const OwlEnum E_fExtraitPartie = {
    .name="fExtraitPartie", .nameEN="fExtractPart", .category="Fichiers",
    .descFR="Composants extractibles d'un chemin par fExtraitChemin. Combinable: fDisque + fRepertoire.",
    .descEN="Path components extractible by fExtraitChemin. Combinable: fDisque + fRepertoire.",
    .values=E_fExtraitPartie_values, .combinable=true
};

/* fExtraitChemin mode bits — combinable (fDisque + fRepertoire, fFichier + fExtension, etc.) */
/* Divergence canon assumee : le canon declare fDisque indisponible hors
 * Windows. OWL le conserve sur toutes les plateformes ; sur un chemin sans
 * lettre de lecteur (cas POSIX) fExtraitChemin(..., fDisque) renvoie "". */
#define OWL_EXT_DISQUE     0x01   /* fDisque    — drive letter (Windows), "" si absent */
#define OWL_EXT_REPERTOIRE 0x02   /* fRepertoire — directory path */
#define OWL_EXT_FICHIER    0x04   /* fFichier   — filename without extension */
#define OWL_EXT_EXTENSION  0x08   /* fExtension — extension with dot */

static const OwlConstDef FEXTRAIT_CONSTS[] = {
    /* canon official names */
    {"fDisque",              "Fichiers", INTEGER_VAL(OWL_EXT_DISQUE),     "lettre de lecteur (Windows)", "drive letter (Windows)", "fDrive"},
    {"fRepertoire",          "Fichiers", INTEGER_VAL(OWL_EXT_REPERTOIRE), "chemin du répertoire",        "directory path", "fDirectory"},
    {"fFichier",             "Fichiers", INTEGER_VAL(OWL_EXT_FICHIER),    "nom de fichier sans extension","filename without extension", "fFile"},
    {"fExtension",           "Fichiers", INTEGER_VAL(OWL_EXT_EXTENSION),  "extension avec point",        "extension with dot", "fExtension"},
};

static const OwlEnum* const FILE_ENUMS[] = { &E_fExtraitPartie };

static Value fn_path_extract(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    const char* p = AS_CSTRING(args[0]);
    int len = AS_STRING(args[0])->length;
    int mode = 0;
    if (IS_INTEGER(args[1])) mode = (int)AS_INTEGER(args[1]);

    /* Find: last separator index, drive end, last dot after last sep.
     * drive_end = index of last char of drive prefix (C: or \\server\share). */
    int sep = -1, dot = -1, drive_end = -1;
    if (len >= 2 && p[1] == ':') drive_end = 1; /* "C:" → drive_end=1 */
    else if (len >= 2 && is_path_sep(p[0]) && is_path_sep(p[1])) {
        /* UNC: \\server\share — drive_end points to the last char of share */
        int i = 2;
        while (i < len && !is_path_sep(p[i])) i++; /* skip server */
        if (i < len) {
            i++; /* skip separator */
            while (i < len && !is_path_sep(p[i])) i++; /* skip share */
            drive_end = i - 1; /* index of last char of share */
        }
    }
    for (int i = 0; i < len; i++) {
        if (is_path_sep(p[i])) { sep = i; dot = -1; }
        else if (p[i] == '.') { dot = i; }
    }

    /* Build result by OR-ing requested components */
    char out[1024]; int oi = 0;

    if (mode & OWL_EXT_DISQUE) {
        /* drive_end already located: "C:" (1) or "\\server\share" (>1).
         * Emit p[0..drive_end]. */
        if (drive_end >= 0) {
            int n = drive_end + 1;
            if (n + oi < 1020) { memcpy(out + oi, p, (size_t)n); oi += n; }
        }
    }

    if (mode & OWL_EXT_REPERTOIRE) {
        /* portion between drive and last separator (inclusive of last sep) */
        int start = (drive_end >= 0) ? drive_end + 1 : 0;
        int end   = (sep >= 0) ? sep + 1 : 0;
        if (end > start) {
            int n = end - start;
            if (oi + n < 1020) { memcpy(out + oi, p + start, (size_t)n); oi += n; }
        }
    }

    if (mode & OWL_EXT_FICHIER) {
        int start = sep + 1;
        int end   = (dot > sep) ? dot : len;
        int n = end - start;
        if (n > 0 && oi + n < 1020) { memcpy(out + oi, p + start, (size_t)n); oi += n; }
    }

    if (mode & OWL_EXT_EXTENSION) {
        if (dot > sep) {
            int n = len - dot;
            if (oi + n < 1020) { memcpy(out + oi, p + dot, (size_t)n); oi += n; }
        }
    }

    out[oi] = '\0';
    return OBJ_VAL(obj_copy_string(out, oi));
}

/* Remplit `buf` avec le chemin absolu de l'exécutable en cours.
 * Retourne la longueur (>0) en cas de succès, 0 sinon. Portable :
 * Windows GetModuleFileNameA, Linux /proc/self/exe, macOS _NSGetExecutablePath. */
static int owl_exe_path(char* buf, int size) {
#if defined(_WIN32)
    unsigned long n = GetModuleFileNameA(NULL, buf, (unsigned long)size);
    if (n == 0 || (int)n >= size) return 0;
    buf[n] = '\0';
    return (int)n;
#elif defined(__APPLE__)
    uint32_t bufsize = (uint32_t)size;
    if (_NSGetExecutablePath(buf, &bufsize) != 0) return 0;
    buf[size - 1] = '\0';
    return (int)strlen(buf);
#else
    ssize_t n = readlink("/proc/self/exe", buf, (size_t)size - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return (int)n;
#endif
}

/* fRepExe() -> répertoire contenant l'exécutable en cours.
 * Chemin absolu, SANS séparateur final (aligné canon). */
static Value fn_dir_exe(int argCount, Value* args) {
    (void)argCount; (void)args;
    char buf[1024];
    int n = owl_exe_path(buf, (int)sizeof(buf));
    if (n <= 0) return OBJ_VAL(obj_copy_string("", 0));
    /* Retire le nom de fichier : tronque au dernier séparateur. */
    int cut = -1;
    for (int i = 0; i < n; i++) {
        if (buf[i] == PATH_SEP_CHAR || buf[i] == PATH_SEP_ALT) cut = i;
    }
    if (cut < 0) return OBJ_VAL(obj_copy_string(buf, n));  /* pas de séparateur */
    /* cut = index du dernier séparateur : le répertoire s'arrête juste avant. */
    return OBJ_VAL(obj_copy_string(buf, cut));
}

/* fRepTemp() -> system temp directory */
static Value fn_dir_temp(int argCount, Value* args) {
    (void)argCount; (void)args;
    const char* t = getenv("TEMP");
    if (t == NULL) t = getenv("TMP");
    if (t == NULL) t = getenv("TMPDIR");
    if (t == NULL) t = "/tmp";
    return OBJ_VAL(obj_copy_string(t, (int)strlen(t)));
}

/* ================================================================
 * RÉPERTOIRES DE DONNÉES SYSTÈME (fDisqueEnCours, fRepGlobal*, fRepDonnées*)
 * Aucun effet de bord disque : ces natives CALCULENT un chemin, elles ne
 * créent aucun dossier. Chemins absolus, sans séparateur final.
 * ================================================================ */

/* Nom d'application (segment <app> de fRepDonnées*) — cf. stdlib_parametre.c :
 * identifiant du bloc Projet vivant, sinon nom du fichier lancé, sinon "owl". */
extern void owl_program_name(char* out, int outSize);

/* Racine des données applicatives propres à l'utilisateur courant. */
static void data_root_user(char* out, int outSize) {
#if defined(_WIN32)
    const char* p = getenv("APPDATA");
    snprintf(out, (size_t)outSize, "%s", (p != NULL && p[0] != '\0') ? p : "");
#elif defined(__APPLE__)
    const char* home = getenv("HOME");
    snprintf(out, (size_t)outSize, "%s/Library/Application Support",
             (home != NULL) ? home : "");
#else
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(out, (size_t)outSize, "%s", xdg);
    } else {
        const char* home = getenv("HOME");
        snprintf(out, (size_t)outSize, "%s/.local/share", (home != NULL) ? home : "");
    }
#endif
}

/* Racine des données applicatives partagées entre tous les utilisateurs. */
static void data_root_common(char* out, int outSize) {
#if defined(_WIN32)
    const char* p = getenv("PROGRAMDATA");
    snprintf(out, (size_t)outSize, "%s",
             (p != NULL && p[0] != '\0') ? p : "C:\\ProgramData");
#elif defined(__APPLE__)
    snprintf(out, (size_t)outSize, "/Library/Application Support");
#else
    snprintf(out, (size_t)outSize, "/usr/local/share");
#endif
}

/* fDisqueEnCours() -> identifiant du disque / de la racine du répertoire
 * courant. Windows : lettre de lecteur suivie de ':' (ex "C:"). POSIX : "/". */
static Value fn_current_drive(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(_WIN32)
    char buf[1024];
    if (owl_getcwd(buf, sizeof(buf)) == NULL || buf[0] == '\0')
        return OBJ_VAL(obj_copy_string("", 0));
    if (buf[1] == ':') return OBJ_VAL(obj_copy_string(buf, 2));
    return OBJ_VAL(obj_copy_string("/", 1));  /* chemin UNC sans lettre */
#else
    return OBJ_VAL(obj_copy_string("/", 1));
#endif
}

/* fRepGlobalUtilisateur() -> racine des données applicatives de l'utilisateur. */
static Value fn_global_dir_user(int argCount, Value* args) {
    (void)argCount; (void)args;
    char buf[1024];
    data_root_user(buf, (int)sizeof(buf));
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* fRepGlobalCommun() -> racine des données applicatives tous-utilisateurs. */
static Value fn_global_dir_common(int argCount, Value* args) {
    (void)argCount; (void)args;
    char buf[1024];
    data_root_common(buf, (int)sizeof(buf));
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* fRepDonnéesUtilisateur() -> <fRepGlobalUtilisateur>/<app>. */
static Value fn_data_dir_user(int argCount, Value* args) {
    (void)argCount; (void)args;
    char root[1024];
    data_root_user(root, (int)sizeof(root));
    char app[256];
    owl_program_name(app, (int)sizeof(app));
    char out[1300];
    snprintf(out, sizeof(out), "%s%c%s", root, PATH_SEP_CHAR, app);
    return OBJ_VAL(obj_copy_string(out, (int)strlen(out)));
}

/* fRepDonnéesCommun() -> <fRepGlobalCommun>/<app>. */
static Value fn_data_dir_common(int argCount, Value* args) {
    (void)argCount; (void)args;
    char root[1024];
    data_root_common(root, (int)sizeof(root));
    char app[256];
    owl_program_name(app, (int)sizeof(app));
    char out[1300];
    snprintf(out, sizeof(out), "%s%c%s", root, PATH_SEP_CHAR, app);
    return OBJ_VAL(obj_copy_string(out, (int)strlen(out)));
}

/* ================================================================
 * BINARY I/O & POSITIONING (fLit, fPositionne, fChargeBuffer, fSauveBuffer,
 * fAjouteTexte, fAjouteBuffer, fCree, fChangeTaille)
 * ================================================================ */

/* fLit(handle, taille) — read exactly `taille` bytes, return them as a
 * binary-safe string. Returns "" at EOF. */
static Value fn_fread_bytes(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int taille = 0;
    if (IS_INTEGER(args[1])) taille = (int)AS_INTEGER(args[1]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot] || taille <= 0) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    char* buf = (char*)malloc((size_t)taille);
    if (buf == NULL) return OBJ_VAL(obj_copy_string("", 0));
    size_t n = fread(buf, 1, (size_t)taille, openFiles[slot]);
    Value v = OBJ_VAL(obj_copy_string(buf, (int)n));
    free(buf);
    return v;
}

/* fPositionne(handle, position [, origine]) — seek.
 * origine: 0 = start (default), 1 = current, 2 = end. Returns the new
 * absolute position, -1 on error. */
static Value fn_fseek(int argCount, Value* args) {
    int handle = extract_file_handle(args[0]);
    int64_t pos = 0; int origin = 0;
    if (IS_INTEGER(args[1])) pos    = AS_INTEGER(args[1]);
    if (argCount >= 3 && IS_INTEGER(args[2])) origin = (int)AS_INTEGER(args[2]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        owl_emit_native_error(E_NATIVE_IO_INVALID_HANDLE, "fPositionne",
                              "native", "fPositionne",
                              NULL);
        return INTEGER_VAL(-1);
    }
    int whence = SEEK_SET;
    if (origin == FP_COURANT) whence = SEEK_CUR;
    if (origin == FP_FIN)     whence = SEEK_END;
    if (fseek(openFiles[slot], (long)pos, whence) != 0) {
        file_error("fPositionne", NULL);
        return INTEGER_VAL(-1);
    }
    owl_clear_error();
    return INTEGER_VAL((int64_t)ftell(openFiles[slot]));
}

/* fCree(path) — create an empty file (truncates if exists). Returns bool. */
/* fCrée(path) — doc 3036042. Crée un fichier externe et l'ouvre en
 * lecture/écriture. Retourne l'identifiant du fichier (-1 si échec).
 * Équivalent à fOuvre(path, foCreation). */
static Value fn_fcreate(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return INTEGER_VAL(-1);
    const char* path = AS_CSTRING(args[0]);
    int slot = alloc_file_slot();
    if (slot < 0) {
        owl_emit_native_error(E_NATIVE_IO_TOO_MANY_OPEN, "fCrée",
                              "native", "fCrée",
                              NULL);
        return INTEGER_VAL(-1);
    }
    FILE* f = fopen(path, "w+b");  /* create/truncate + read-write */
    if (f == NULL) { file_error("fCree", path); return INTEGER_VAL(-1); }
    openFiles[slot] = f;
    fileSlotUsed[slot] = true;
    size_t plen = strlen(path);
    if (plen < sizeof(openFilePaths[slot])) memcpy(openFilePaths[slot], path, plen + 1);
    else openFilePaths[slot][0] = '\0';
    owl_clear_error();
    return INTEGER_VAL(slot + 1);
}

/* fChangeTaille(handle, newSize) — truncate/extend. */
static Value fn_ftruncate(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int64_t newSize = 0;
    if (IS_INTEGER(args[1])) newSize = AS_INTEGER(args[1]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) {
        owl_emit_native_error(E_NATIVE_IO_INVALID_HANDLE, "fChangeTaille",
                              "native", "fChangeTaille",
                              NULL);
        return BOOL_VAL(false);
    }
    FILE* f = openFiles[slot];
    fflush(f);
    int rc;
#ifdef _WIN32
    rc = _chsize_s(_fileno(f), newSize);
#else
    rc = ftruncate(fileno(f), (off_t)newSize);
#endif
    if (rc != 0) { file_error("fChangeTaille", NULL); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fAjouteTexte(path, texte [, encodage]) — append text to a file.
 * Encode le texte vers `encodage` avant ajout. Défaut encUTF8 = octets
 * UTF-8 internes tels quels. */
static Value fn_fappend_text(int argCount, Value* args) {
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    int enc = file_arg_encoding(argCount, args, 2);
    FILE* f = fopen(AS_CSTRING(args[0]), "ab");
    if (f == NULL) { file_error("fAjouteTexte", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    ObjString* s = AS_STRING(args[1]);
    bool ok;
    if (enc == (int)OWL_ENC_UTF8) {
        ok = fwrite(s->chars, 1, (size_t)s->length, f) == (size_t)s->length;
    } else {
        int outLen = 0;
        unsigned char* out = owl_codec_encode(s->chars, s->length, enc, &outLen);
        ok = fwrite(out, 1, (size_t)outLen, f) == (size_t)outLen;
        free(out);
    }
    fclose(f);
    if (!ok) { file_error("fAjouteTexte (ecriture partielle)", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fAjouteBuffer(path, buffer) — append buffer bytes to a file. */
static Value fn_fappend_buffer(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const void* data = NULL; int len = 0;
    if (IS_BUFFER(args[1])) {
        ObjBuffer* b = AS_BUFFER(args[1]);
        data = b->bytes; len = b->length;
    } else if (IS_STRING(args[1])) {
        ObjString* s = AS_STRING(args[1]);
        data = s->chars; len = s->length;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "fAjouteBuffer",
                              "native", "fAjouteBuffer",
                              "arg", "2",
                              "expected", "buffer ou chaîne",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    FILE* f = fopen(AS_CSTRING(args[0]), "ab");
    if (f == NULL) { file_error("fAjouteBuffer", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    size_t n = fwrite(data, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) { file_error("fAjouteBuffer (ecriture partielle)", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fChargeBuffer(path [, offset [, length]]) -> buffer with file contents
 * offset: position de départ (octets depuis le début, défaut 0)
 * length: nombre d'octets max à lire (défaut: jusqu'à la fin) */
static Value fn_fload_buffer(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_buffer_empty(0));
    long offset = 0, length = -1;
    if (argCount >= 2 && IS_INTEGER(args[1])) offset = (long)AS_INTEGER(args[1]);
    if (argCount >= 3 && IS_INTEGER(args[2])) length = (long)AS_INTEGER(args[2]);
    FILE* f = fopen(AS_CSTRING(args[0]), "rb");
    if (f == NULL) { file_error("fChargeBuffer", AS_CSTRING(args[0])); return OBJ_VAL(obj_buffer_empty(0)); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (offset < 0 || offset > sz) { fclose(f); return OBJ_VAL(obj_buffer_empty(0)); }
    long remaining = sz - offset;
    long toRead = (length < 0 || length > remaining) ? remaining : length;
    fseek(f, offset, SEEK_SET);
    if (toRead <= 0 || toRead > INT_MAX) { fclose(f); owl_clear_error(); return OBJ_VAL(obj_buffer_empty(0)); }
    unsigned char* buf = (unsigned char*)malloc((size_t)toRead);
    size_t n = fread(buf, 1, (size_t)toRead, f);
    fclose(f);
    ObjBuffer* out = obj_new_buffer(buf, (int)n);
    free(buf);
    owl_clear_error();
    return OBJ_VAL(out);
}

/* fSauveBuffer(path, buffer) — overwrite file with buffer. */
static Value fn_fsave_buffer(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return BOOL_VAL(false);
    const void* data = NULL; int len = 0;
    if (IS_BUFFER(args[1])) {
        ObjBuffer* b = AS_BUFFER(args[1]); data = b->bytes; len = b->length;
    } else if (IS_STRING(args[1])) {
        ObjString* s = AS_STRING(args[1]); data = s->chars; len = s->length;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "fSauveBuffer",
                              "native", "fSauveBuffer",
                              "arg", "2",
                              "expected", "buffer ou chaîne",
                              "actual", "autre type",
                              NULL);
        return BOOL_VAL(false);
    }
    FILE* f = fopen(AS_CSTRING(args[0]), "wb");
    if (f == NULL) { file_error("fSauveBuffer", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    size_t n = fwrite(data, 1, (size_t)len, f);
    fclose(f);
    if (n != (size_t)len) { file_error("fSauveBuffer (ecriture partielle)", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* fCherche(handle, motif) — find a string motif in the file starting from
 * the current position. Returns the position of the first occurrence or -1.
 * Leaves the file cursor AT the match (or at EOF if not found). */
static Value fn_fsearch(int argCount, Value* args) {
    (void)argCount;
    int handle = extract_file_handle(args[0]);
    int slot = handle - 1;
    if (slot < 0 || slot >= MAX_OPEN_FILES || !fileSlotUsed[slot]) return INTEGER_VAL(-1);
    const char* patData = NULL;
    int patLen = 0;
    if (IS_STRING(args[1])) {
        ObjString* s = AS_STRING(args[1]);
        patData = s->chars; patLen = s->length;
    } else if (IS_BUFFER(args[1])) {
        ObjBuffer* b = AS_BUFFER(args[1]);
        patData = (const char*)b->bytes; patLen = b->length;
    } else {
        return INTEGER_VAL(-1);
    }
    if (patLen == 0) return INTEGER_VAL(-1);
    FILE* f = openFiles[slot];
    long start = ftell(f);
    char buf[8192];
    long pos = start;
    /* Naive: sliding window with overlap of patLen - 1. */
    int overlap = patLen - 1;
    int retained = 0;
    char windowBuf[16384];
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;
        if (retained + (int)n > (int)sizeof(windowBuf)) break; /* too big for this naive impl */
        memcpy(windowBuf + retained, buf, n);
        int total = retained + (int)n;
        for (int i = 0; i <= total - patLen; i++) {
            if (memcmp(windowBuf + i, patData, (size_t)patLen) == 0) {
                long absPos = pos + (i - retained);
                fseek(f, absPos, SEEK_SET);
                return INTEGER_VAL((int64_t)absPos);
            }
        }
        pos += n;
        /* Keep last overlap bytes. */
        if (total > overlap) {
            memmove(windowBuf, windowBuf + total - overlap, (size_t)overlap);
            retained = overlap;
        } else {
            retained = total;
        }
    }
    return INTEGER_VAL(-1);
}

/* ================================================================
 * METADATA (fTaille, fDate, fHeure, fDateHeure)
 * ================================================================ */

/* fTaille(path | handle | FichierDisque) — doc 3036053.
 * Forme chaîne : stat(path) → taille en octets.
 * Forme handle/FichierDisque : taille du fichier ouvert (via ftell+fseek). */
static Value fn_fsize_path(int argCount, Value* args) {
    (void)argCount;
    /* Délégation forme handle/FichierDisque. */
    if (!IS_STRING(args[0])) {
        return fn_ftaille_open(argCount, args);
    }
    struct stat st;
    if (stat(AS_CSTRING(args[0]), &st) != 0) {
        file_error("fTaille", AS_CSTRING(args[0]));
        return INTEGER_VAL(-1);
    }
    owl_clear_error();
    return INTEGER_VAL((int64_t)st.st_size);
}

/* Return a "YYYYMMDD" / "HHMMSS" / "YYYYMMDDHHMMSS" string from st.st_mtime. */
/* fDate/fHeure/fDateHeure selector constants (doc 3036019).
 * Valeur arbitraire OWL — identité du mode seule compte. */
#define FD_MODIFICATION 0
#define FD_CREATION     1
#define FD_ACCES        2

/* Extract timestamp from stat per selector. Defaults to mtime on Windows
 * where birthtime isn't always reliable via stat(). */
static time_t fdate_pick_time(const struct stat* st, int which) {
    switch (which) {
        case FD_CREATION: return st->st_ctime;
        case FD_ACCES:    return st->st_atime;
        case FD_MODIFICATION:
        default:          return st->st_mtime;
    }
}

/* Reads the optional 3rd selector arg (fModification/fCréation/fAccès). */
static int fdate_which_arg(int argCount, Value* args) {
    if (argCount >= 3 && IS_INTEGER(args[2])) return (int)AS_INTEGER(args[2]);
    return FD_MODIFICATION;
}

/* fDate(path [, nouvelleDate [, which]]) — doc 3036019.
 * Forme getter: args[1] == "" ou absent → retourne la date (AAAAMMJJ).
 * Forme setter: args[1] non vide → positionne la date (non implémenté: wip). */
static Value fn_fdate(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    struct stat st;
    if (stat(AS_CSTRING(args[0]), &st) != 0) return OBJ_VAL(obj_copy_string("", 0));
    time_t ts = fdate_pick_time(&st, fdate_which_arg(argCount, args));
    struct tm* t = localtime(&ts);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d",
             (t->tm_year + 1900) % 10000,
             (t->tm_mon + 1) & 0xFF,
             t->tm_mday & 0xFF);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

static Value fn_fheure(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    struct stat st;
    if (stat(AS_CSTRING(args[0]), &st) != 0) return OBJ_VAL(obj_copy_string("", 0));
    time_t ts = fdate_pick_time(&st, fdate_which_arg(argCount, args));
    struct tm* t = localtime(&ts);
    char buf[32];
    snprintf(buf, sizeof(buf), "%02d%02d%02d",
             t->tm_hour & 0xFF, t->tm_min & 0xFF, t->tm_sec & 0xFF);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

static Value fn_fdateheure(int argCount, Value* args) {
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    struct stat st;
    if (stat(AS_CSTRING(args[0]), &st) != 0) return OBJ_VAL(obj_copy_string("", 0));
    time_t ts = fdate_pick_time(&st, fdate_which_arg(argCount, args));
    struct tm* t = localtime(&ts);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d",
             (t->tm_year + 1900) % 10000,
             (t->tm_mon + 1) & 0xFF,
             t->tm_mday & 0xFF,
             t->tm_hour & 0xFF,
             t->tm_min & 0xFF,
             t->tm_sec & 0xFF);
    return OBJ_VAL(obj_copy_string(buf, (int)strlen(buf)));
}

/* ================================================================
 * PATH HELPERS (fConstruitChemin, fRelativise, fChangeExtension, fSep,
 * fNomFichier, fFichierTemp, fRepParent)
 * ================================================================ */

/* fSep() -> platform path separator as a 1-char string. */
static Value fn_fsep(int argCount, Value* args) {
    (void)argCount; (void)args;
    char s[2] = { PATH_SEP_CHAR, 0 };
    return OBJ_VAL(obj_copy_string(s, 1));
}

/* fConstruitChemin(<dir>, <file> [, <extension>]) — doc 1000019880.
 * Algorithm: CompléteRep(<dir>) + <file> + ["." if ext lacks leading dot] + <ext>.
 * Arity 2 = dir + file. Arity 3 = dir + file + ext (smart dot). */
/* CompléteRep(chemin) — doc 3024018.
 * Ajoute le séparateur de répertoire (`\\` Windows, `/` Unix) à la fin du
 * chemin si absent. Trim les espaces avant/après. Chaîne vide → "". */
Value owl_complete_rep(Value v) {
    if (!IS_STRING(v)) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* s = AS_STRING(v);
    int start = 0, end = s->length;
    /* Trim spaces (SansEspace canon) */
    while (start < end && (s->chars[start] == ' ' || s->chars[start] == '\t')) start++;
    while (end > start && (s->chars[end-1] == ' ' || s->chars[end-1] == '\t')) end--;
    int len = end - start;
    if (len == 0) return OBJ_VAL(obj_copy_string("", 0));
    /* Already terminated by a separator? */
    char last = s->chars[end - 1];
    if (last == '/' || last == '\\') {
        return OBJ_VAL(obj_copy_string(s->chars + start, len));
    }
    /* Append platform separator */
    char buf[1024];
    if (len + 1 >= (int)sizeof(buf)) len = (int)sizeof(buf) - 2;
    memcpy(buf, s->chars + start, (size_t)len);
    buf[len] = PATH_SEP_CHAR;
    return OBJ_VAL(obj_copy_string(buf, len + 1));
}

static Value fn_complete_rep(int argCount, Value* args) {
    (void)argCount;
    return owl_complete_rep(args[0]);
}

static Value fn_fbuild_path(int argCount, Value* args) {
    char out[1024];
    int n = 0;
    /* <dir> */
    if (argCount >= 1 && IS_STRING(args[0])) {
        ObjString* dir = AS_STRING(args[0]);
        int dl = dir->length;
        if (dl > (int)sizeof(out) - 1) dl = (int)sizeof(out) - 1;
        memcpy(out, dir->chars, (size_t)dl);
        n = dl;
        /* Add separator if dir non-empty and doesn't end with one */
        if (n > 0 && !is_path_sep(out[n - 1]) && n < (int)sizeof(out) - 1) {
            out[n++] = PATH_SEP_CHAR;
        }
    }
    /* <file> */
    if (argCount >= 2 && IS_STRING(args[1])) {
        ObjString* file = AS_STRING(args[1]);
        const char* fp = file->chars;
        int fl = file->length;
        /* Skip leading separator on file part */
        if (fl > 0 && is_path_sep(fp[0])) { fp++; fl--; }
        if (n + fl > (int)sizeof(out) - 1) fl = (int)sizeof(out) - 1 - n;
        memcpy(out + n, fp, (size_t)fl);
        n += fl;
    }
    /* <extension> (optionnel): préfixe "." si absent */
    if (argCount >= 3 && IS_STRING(args[2])) {
        ObjString* ext = AS_STRING(args[2]);
        if (ext->length > 0) {
            if (ext->chars[0] != '.' && n < (int)sizeof(out) - 1) {
                out[n++] = '.';
            }
            int el = ext->length;
            if (n + el > (int)sizeof(out) - 1) el = (int)sizeof(out) - 1 - n;
            memcpy(out + n, ext->chars, (size_t)el);
            n += el;
        }
    }
    return OBJ_VAL(obj_copy_string(out, n));
}

/* fChangeExtension(path, newExt) — swap the extension (newExt with or
 * without leading dot). */
static Value fn_fchange_ext(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* src = AS_STRING(args[0]);
    ObjString* ext = AS_STRING(args[1]);
    int dot = -1;
    for (int i = src->length - 1; i >= 0; i--) {
        if (src->chars[i] == '.') { dot = i; break; }
        if (is_path_sep(src->chars[i])) break;
    }
    int base = dot >= 0 ? dot : src->length;
    char buf[1024];
    int n = base < (int)sizeof(buf) - 1 ? base : (int)sizeof(buf) - 1;
    memcpy(buf, src->chars, (size_t)n);
    const char* e = ext->chars;
    int elen = ext->length;
    if (elen > 0 && e[0] != '.' && n < (int)sizeof(buf) - 1) {
        buf[n++] = '.';
    }
    if (n + elen > (int)sizeof(buf) - 1) elen = (int)sizeof(buf) - 1 - n;
    memcpy(buf + n, e, (size_t)elen);
    n += elen;
    return OBJ_VAL(obj_copy_string(buf, n));
}

/* fNomFichier(path) — basename (alias for fExtraitChemin+NomEtExtension). */
/* fNomFichier(arg) — doc 1000017290.
 * - arg int/FichierDisque : retourne le chemin complet du fichier ouvert
 * - arg string            : retourne le basename (extension OWL pratique) */
static Value fn_fbasename(int argCount, Value* args) {
    (void)argCount;
    /* Si c'est un handle de fichier ouvert (entier ou FichierDisque),
     * déléguer à fn_fnom_fichier qui retourne le path complet. */
    if (IS_INTEGER(args[0]) || IS_REAL(args[0]) || IS_INSTANCE(args[0])) {
        return fn_fnom_fichier(argCount, args);
    }
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* src = AS_STRING(args[0]);
    int sep = -1;
    for (int i = 0; i < src->length; i++) {
        if (is_path_sep(src->chars[i])) sep = i;
    }
    int start = sep + 1;
    return OBJ_VAL(obj_copy_string(src->chars + start, src->length - start));
}

/* fRepParent(path) — parent directory. */
static Value fn_fparent(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return OBJ_VAL(obj_copy_string("", 0));
    ObjString* src = AS_STRING(args[0]);
    int sep = -1;
    for (int i = src->length - 1; i >= 0; i--) {
        if (is_path_sep(src->chars[i])) { sep = i; break; }
    }
    if (sep < 0) return OBJ_VAL(obj_copy_string("", 0));
    /* Keep the drive letter + separator on Windows (e.g. "C:/"). */
    int keepLen = sep;
    if (keepLen == 0 || (keepLen == 2 && src->chars[1] == ':')) keepLen = sep + 1;
    return OBJ_VAL(obj_copy_string(src->chars, keepLen));
}

/* fFichierTemp([prefix [, dir]]) — unique temp filename. Doc 3036044.
 * Produit un chemin mais ne crée pas le fichier.
 *   prefix : préfixe du nom (par défaut "owl")
 *   dir    : répertoire (par défaut: TEMP / TMPDIR / /tmp) */
static Value fn_ftempname(int argCount, Value* args) {
    const char* prefix = "owl";
    const char* dir = NULL;
    if (argCount >= 1 && IS_STRING(args[0])) prefix = AS_CSTRING(args[0]);
    if (argCount >= 2 && IS_STRING(args[1])) dir = AS_CSTRING(args[1]);
    if (dir == NULL || *dir == '\0') {
        dir = getenv("TEMP");
        if (dir == NULL) dir = getenv("TMPDIR");
        if (dir == NULL) dir = "/tmp";
    }
    char out[1024];
    static int counter = 0;
    counter++;
    /* Strip trailing separator from dir to avoid duplicates */
    int dirLen = (int)strlen(dir);
    while (dirLen > 0 && (dir[dirLen-1] == '/' || dir[dirLen-1] == '\\')) dirLen--;
    snprintf(out, sizeof(out), "%.*s%c%s_%llx_%d.tmp",
             dirLen, dir, PATH_SEP_CHAR, prefix,
             (unsigned long long)time(NULL), counter);
    return OBJ_VAL(obj_copy_string(out, (int)strlen(out)));
}

/* ================================================================
 * DIRECTORY UTILITIES (fCompare, fRepTaille, fRepCopie, fListeFichier
 * recursive)
 * ================================================================ */

/* fCompare(path1, path2) — byte-for-byte compare. Returns 0 if identical,
 * -1 if different, -2 on error (couldn't open). */
/* fCompare(path1, path2) — doc 3036002. Retour:
 *   0  = fichiers identiques (ou chemins identiques)
 *   1  = fichiers différents
 *   -1 = <path1> introuvable
 *   -2 = <path2> introuvable */
static Value fn_fcompare(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return INTEGER_VAL(-1);
    FILE* a = fopen(AS_CSTRING(args[0]), "rb");
    if (a == NULL) { file_error("fCompare (source 1)", AS_CSTRING(args[0])); return INTEGER_VAL(-1); }
    FILE* b = fopen(AS_CSTRING(args[1]), "rb");
    if (b == NULL) { file_error("fCompare (source 2)", AS_CSTRING(args[1])); fclose(a); return INTEGER_VAL(-2); }
    char buf1[4096], buf2[4096];
    int diff = 0;
    while (1) {
        size_t n1 = fread(buf1, 1, sizeof(buf1), a);
        size_t n2 = fread(buf2, 1, sizeof(buf2), b);
        if (n1 != n2) { diff = 1; break; }
        if (n1 == 0) break;
        if (memcmp(buf1, buf2, n1) != 0) { diff = 1; break; }
    }
    fclose(a); fclose(b);
    owl_clear_error();
    return INTEGER_VAL(diff);
}

/* fRepTaille(path) — total size of a directory tree in bytes. */
static int64_t dir_size_recursive(const char* path) {
    DIR* d = opendir(path);
    if (d == NULL) return 0;
    int64_t total = 0;
    struct dirent* e;
    char child[1024];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0) {
            if ((st.st_mode & S_IFMT) == S_IFDIR) total += dir_size_recursive(child);
            else total += (int64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

static Value fn_frepsize(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return INTEGER_VAL(0);
    return INTEGER_VAL(dir_size_recursive(AS_CSTRING(args[0])));
}

/* fRepCopie(src, dst) — recursive directory copy. Creates dst if needed. */
static bool copy_file_single(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (in == NULL) return false;
    FILE* out = fopen(dst, "wb");
    if (out == NULL) { fclose(in); return false; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return false; }
    }
    fclose(in); fclose(out);
    return true;
}

static bool copy_dir_recursive(const char* src, const char* dst) {
    struct stat st;
    if (stat(dst, &st) != 0) {
        if (owl_mkdir_one(dst) != 0 && errno != EEXIST) return false;
    }
    DIR* d = opendir(src);
    if (d == NULL) return false;
    struct dirent* e;
    char childSrc[1024], childDst[1024];
    bool ok = true;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(childSrc, sizeof(childSrc), "%s/%s", src, e->d_name);
        snprintf(childDst, sizeof(childDst), "%s/%s", dst, e->d_name);
        struct stat cst;
        if (stat(childSrc, &cst) == 0) {
            if ((cst.st_mode & S_IFMT) == S_IFDIR) {
                if (!copy_dir_recursive(childSrc, childDst)) ok = false;
            } else {
                if (!copy_file_single(childSrc, childDst)) ok = false;
            }
        }
    }
    closedir(d);
    return ok;
}

static Value fn_frepcopy(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0]) || !IS_STRING(args[1])) return BOOL_VAL(false);
    bool ok = copy_dir_recursive(AS_CSTRING(args[0]), AS_CSTRING(args[1]));
    if (!ok) { file_error("fRepCopie", AS_CSTRING(args[0])); return BOOL_VAL(false); }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* ================================================================
 * Registry declarations — Fichiers family (fXxx).
 * File function names don't use accents (fEcrit, fLit, fCree…).
 * ================================================================ */

static const OwlParam p_path[]     = { {"chemin", "chaîne", "chemin de fichier"}, {0} };
static const OwlParam p_path_mode[]= { {"chemin", "chaîne", "chemin"}, {"mode", "entier", "foLecture/foEcriture/…"}, {0} };
static const OwlParam p_fid[]      = { {"id", "entier", "identifiant fichier"}, {0} };
static const OwlParam p_fid_n[]    = { {"id", "entier", "identifiant"}, {"n", "entier", "nb octets à lire"}, {0} };
static const OwlParam p_fid_c[]    = { {"id", "entier", "identifiant"}, {"contenu", "chaîne|buffer", "à écrire"}, {0} };
static const OwlParam p_pc[]       = { {"chemin", "chaîne", "fichier"}, {"contenu", "chaîne", "texte"}, {0} };
static const OwlParam p_pb[]       = { {"chemin", "chaîne", "fichier"}, {"buffer", "buffer", "données"}, {0} };
static const OwlParam p_pp[]       = { {"source", "chaîne", "source"}, {"dest", "chaîne", "destination"}, {0} };
static const OwlParam p_path_mode2[] = {
    {"chemin",    "chaîne", "chemin"},
    {"composant", "entier", "composant à extraire", &E_fExtraitPartie},
    {0}
};

static const OwlSignature sig_path_bool[]  = { {1, p_path, "booléen", "Vrai/Faux"}, {0} };
static const OwlSignature sig_path_int[]   = { {1, p_path, "entier", "taille en octets"}, {0} };
static const OwlParam p_load_text[]        = { {"chemin", "chaîne", "fichier à charger"},
                                                {"encodage", "owlEncodage", "encodage source du fichier (défaut encUTF8)", &E_owlEncodage},
                                                {0} };
static const OwlSignature sig_load_text[]  = { {1, p_path, "chaîne", "contenu (décodé en UTF-8 interne)"},
                                                {2, p_load_text, "chaîne", "contenu décodé depuis l'encodage indiqué"}, {0} };
static const OwlParam p_load_buf[]         = { {"chemin", "chaîne", "fichier à charger"},
                                                {"offset", "entier?", "position de départ (défaut 0)"},
                                                {"longueur", "entier?", "taille max à lire (défaut: tout)"},
                                                {0} };
static const OwlSignature sig_load_buf[]   = { {1, p_path, "buffer", "contenu entier"},
                                                {2, p_load_buf, "buffer", "contenu depuis offset"},
                                                {3, p_load_buf, "buffer", "contenu depuis offset (max length)"},
                                                {0} };
static const OwlSignature sig_open[]       = { {1, p_path, "entier", "identifiant fichier (mode par défaut foLectureEcriture)"},
                                                {2, p_path_mode, "entier", "identifiant fichier (mode combinable bit-à-bit)"}, {0} };
static const OwlSignature sig_fid_void[]   = { {1, p_fid,  "rien", ""}, {0} };
static const OwlSignature sig_fid_bool[]   = { {1, p_fid,  "booléen", "Vrai si fin de fichier"}, {0} };
static const OwlSignature sig_fid_str[]    = { {1, p_fid,  "chaîne", "ligne ou contenu"}, {0} };
static const OwlSignature sig_read[]       = { {2, p_fid_n, "chaîne", "octets lus"}, {0} };
static const OwlSignature sig_write[]      = { {2, p_fid_c, "rien",   "écriture"}, {0} };
static const OwlParam p_save_text_enc[]    = { {"chemin", "chaîne", "fichier"},
                                                {"contenu", "chaîne", "texte"},
                                                {"encodage", "owlEncodage", "encodage cible avant écriture (défaut encUTF8)", &E_owlEncodage},
                                                {0} };
static const OwlSignature sig_save_text[]  = { {2, p_pc,   "booléen", "succès"},
                                                {3, p_save_text_enc, "booléen", "succès (avec encodage)"}, {0} };
static const OwlSignature sig_save_buf[]   = { {2, p_pb,   "booléen", "succès"}, {0} };
static const OwlSignature sig_copy[]       = { {2, p_pp,   "booléen", "succès"}, {0} };
static const OwlParam p_copy_flags[]       = { {"source",      "chaîne", "fichier ou pattern avec * / ?"},
                                                {"destination", "chaîne", "fichier ou dossier"},
                                                {"flags",       "entier?", "combinaison fr* (frRécursif, …)"}, {0} };
static const OwlSignature sig_copy_flags[] = { {2, p_pp,         "booléen", "succès"},
                                                {3, p_copy_flags, "booléen", "succès"}, {0} };
static const OwlSignature sig_extract[]    = { {2, p_path_mode2, "chaîne", "composant extrait"}, {0} };
static const OwlSignature sig_dir_bool[]   = { {1, p_path, "booléen", "Vrai/Faux"}, {0} };
static const OwlSignature sig_dir_void[]   = { {1, p_path, "rien", "opération effectuée"}, {0} };
static const OwlParam p_dir_rm[]           = { {"chemin", "chaîne", "dossier à supprimer"},
                                                {"indicateur", "entier?", "frRécursif pour supprimer le contenu"}, {0} };
static const OwlSignature sig_dir_rm[]      = { {1, p_path, "booléen", "succès"},
                                                {2, p_dir_rm, "booléen", "succès (avec indicateur)"}, {0} };
static const OwlParam p_dir_list[]         = { {"chemin", "chaîne", "dossier de départ"},
                                                {"options", "entier?", "combinaison fr* (frRécursif…)"}, {0} };
static const OwlParam p_dir_list_cb[]      = { {"chemin", "chaîne", "dossier de départ"},
                                                {"callback", "procédure", "appelée pour chaque sous-dossier"},
                                                {"pointeur", "entier?", "passé tel quel à la procédure"},
                                                {"options", "entier?", "combinaison fr*"}, {0} };
static const OwlSignature sig_dir_list[]   = { {1, p_path, "chaîne", "chemins CRLF-séparés"},
                                                {2, p_dir_list, "chaîne", "avec options"},
                                                {3, p_dir_list_cb, "entier", "forme callback"},
                                                {4, p_dir_list_cb, "entier", "forme callback complète"}, {0} };
static const OwlParam p_file_list[]        = { {"pattern", "chaîne", "chemin avec wildcard (*.bmp, dir/*.txt)"},
                                                {"options", "entier?", "combinaison fr*"}, {0} };
static const OwlParam p_file_list_cb[]     = { {"pattern", "chaîne", "chemin avec wildcard"},
                                                {"callback", "procédure", "appelée pour chaque fichier"},
                                                {"pointeur", "entier?", "passé tel quel à la procédure"},
                                                {"options", "entier?", "combinaison fr*"}, {0} };
static const OwlSignature sig_file_list[]  = { {1, p_file_list, "chaîne", "chemins CRLF-séparés"},
                                                {2, p_file_list, "chaîne", "avec options"},
                                                {3, p_file_list_cb, "entier", "forme callback"},
                                                {4, p_file_list_cb, "entier", "forme callback complète"}, {0} };
static const OwlSignature sig_no_arg[]     = { {0, NULL, "chaîne", "chemin"}, {0} };
static const OwlParam p_dir_set[]          = { {"chemin", "chaîne?", "nouveau dossier courant (optionnel)"}, {0} };
static const OwlSignature sig_cwd[]        = { {0, NULL,        "chaîne", "dossier courant"},
                                                {1, p_dir_set,   "chaîne", "change et retourne le nouveau"}, {0} };
static const OwlParam p_ftempname[]        = { {"prefixe",   "chaîne?", "préfixe du nom (défaut 'owl')"},
                                                {"repertoire","chaîne?", "dossier (défaut: dossier temp système)"},
                                                {0} };
static const OwlSignature sig_ftempname[]  = { {0, NULL, "chaîne", "chemin temporaire unique"},
                                                {1, p_ftempname, "chaîne", "avec préfixe"},
                                                {2, p_ftempname, "chaîne", "avec préfixe et dossier"}, {0} };
static const OwlParam p_seek[]             = { {"id", "entier", "identifiant fichier"},
                                                {"deplacement", "entier", "déplacement en octets"},
                                                {"origine", "entier?", "fpDébut/fpCourant/fpFin (défaut fpDébut)"}, {0} };
static const OwlSignature sig_seek[]        = { {2, p_seek, "entier", "position résultante"},
                                                {3, p_seek, "entier", "position résultante (avec origine)"}, {0} };
static const OwlParam p_trunc[]            = { {"id",     "entier", "identifiant fichier"},
                                                {"taille", "entier", "nouvelle taille en octets (tronque ou étend le fichier)"},
                                                {0} };
static const OwlSignature sig_trunc[]      = { {2, p_trunc, "rien", ""}, {0} };
static const OwlSignature sig_fsearch[]    = { {2, p_fid_c, "entier", "position base 1 ou 0"}, {0} };
static const OwlParam p_fdate[]            = { {"chemin",       "chaîne", "fichier à inspecter"},
                                                {"nouvelleDate", "chaîne?", "date à positionner (\"\" pour getter)"},
                                                {"which",        "entier?", "fModification/fCréation/fAccès"},
                                                {0} };
static const OwlSignature sig_fdate[]      = { {1, p_path,  "date",  "date de modification"},
                                                {2, p_fdate, "date",  "avec positionnement (wip)"},
                                                {3, p_fdate, "date",  "avec sélecteur de date"}, {0} };
static const OwlSignature sig_fheure[]     = { {1, p_path,  "heure", "heure de modification"},
                                                {2, p_fdate, "heure", "avec positionnement (wip)"},
                                                {3, p_fdate, "heure", "avec sélecteur de date"}, {0} };
static const OwlSignature sig_fdh[]        = { {1, p_path,  "dateheure", "date+heure"},
                                                {2, p_fdate, "dateheure", "avec positionnement (wip)"},
                                                {3, p_fdate, "dateheure", "avec sélecteur de date"}, {0} };
static const OwlParam p_build_path[]       = { {"chemin", "chaîne", "dossier de base"},
                                                {"nom", "chaîne", "nom de fichier"},
                                                {"extension", "chaîne?", "extension (point ajouté si absent)"}, {0} };
static const OwlSignature sig_path_multi[] = { {2, p_build_path, "chaîne", "chemin construit"},
                                                {3, p_build_path, "chaîne", "chemin construit (avec extension)"}, {0} };
static const OwlSignature sig_change_ext[] = { {2, p_pp,   "chaîne", "nouveau chemin"}, {0} };
static const OwlSignature sig_basename[]   = { {1, p_path, "chaîne", "nom de fichier"}, {0} };
static const OwlSignature sig_complete_rep[] = { {1, p_path, "chaîne", "chemin avec séparateur final"}, {0} };

#define F_DEF(fr, en, cat, desc_fr, desc_en, sig, fn) \
    { .canonical = (fr), .nameEN = (en), .category = (cat), \
      .descFR = (desc_fr), .descEN = (desc_en), .signatures = (sig), .impl = (fn) }

/* Aliases legacy : meme native, noms historiques heritages. Resolus
 * par owl_registry_find (parcourt le champ aliases). N'apparaissent
 * pas comme entrees distinctes dans owl list natives. */
static const char* const ALIASES_fTaille[] = {
    "fTailleFichier", "fFileSize", NULL
};

static const OwlNativeDef DEFS_FILE[] = {
    F_DEF("fOuvre", "fOpen", "Fichiers",
        "Ouvre un fichier, retourne un identifiant.",
        "Open a file, return an identifier.", sig_open, fn_fopen),
    F_DEF("fFerme", "fClose", "Fichiers",
        "Ferme un fichier ouvert.", "Close an opened file.", sig_fid_void, fn_fclose),
    F_DEF("fEcrit", "fWrite", "Fichiers",
        "Écrit du texte/buffer dans un fichier ouvert.",
        "Write text/buffer to an open file.", sig_write, fn_fwrite),
    F_DEF("fEcritLigne", "fWriteLine", "Fichiers",
        "Écrit une ligne (avec retour à la ligne).",
        "Write a line (with trailing newline).", sig_write, fn_fwriteline),
    F_DEF("fLitLigne", "fReadLine", "Fichiers",
        "Lit une ligne d'un fichier ouvert.",
        "Read a line from an open file.", sig_fid_str, fn_freadline),
    F_DEF("fLitTout", "fReadAll", "Fichiers",
        "Lit tout le contenu restant d'un fichier.",
        "Read all remaining content from a file.", sig_fid_str, fn_freadall),
    F_DEF("fFinFichier", "fEOF", "Fichiers",
        "Vrai si la fin de fichier est atteinte.",
        "True if end of file is reached.", sig_fid_bool, fn_feof),
    F_DEF("fFichierExiste", "fFileExist", "Fichiers",
        "Vrai si un fichier existe.", "True if a file exists.",
        sig_path_bool, fn_ffichier_existe),
    F_DEF("fSupprime", "fDelete", "Fichiers",
        "Supprime un fichier.", "Delete a file.",
        sig_path_bool, fn_fdelete),
    F_DEF("fChargeTexte", "fLoadText", "Fichiers",
        "Charge le contenu complet d'un fichier texte. L'argument encodage "
        "optionnel (encUTF8 par défaut, encLatin1, encWindows1252, encUTF16LE, "
        "encUTF16BE) décode les octets du fichier vers la chaîne résultante. "
        "En cas d'erreur "
        "(fichier absent, permission refusée…), retourne une chaîne vide "
        "et positionne ErreurDétectée à Vrai ; ErreurInfo() donne le "
        "détail. Aucune exception levée — utiliser le pattern "
        "`Si ErreurDétectée Alors …` pour détecter l'échec.",
        "Load a text file's entire content. The optional encoding argument "
        "(encUTF8 by default, encLatin1, encWindows1252, encUTF16LE, "
        "encUTF16BE) decodes the file bytes into the resulting string. "
        "On failure (missing file, "
        "permission denied…), returns an empty string and sets "
        "ErrorOccurred to True ; ErrorInfo() yields details. No "
        "exception is raised — use the `If ErrorOccurred Then …` "
        "pattern to detect failure.",
        sig_load_text, fn_floadtext),
    F_DEF("fSauveTexte", "fSaveText", "Fichiers",
        "Enregistre une chaîne dans un fichier texte. L'argument encodage "
        "optionnel (encUTF8 par défaut, encLatin1, encWindows1252, encUTF16LE, "
        "encUTF16BE) transcode le contenu avant écriture. Retourne Vrai en cas de succès.",
        "Save a string to a text file. The optional encoding argument "
        "(encUTF8 by default, encLatin1, encWindows1252, encUTF16LE, "
        "encUTF16BE) transcodes the content before writing. Returns True on success.",
        sig_save_text, fn_fsavetext),

    /* Dossiers */
    F_DEF("fRepExiste", "fDirExist", "Fichiers",
        "Vrai si un dossier existe.", "True if a directory exists.",
        sig_dir_bool, fn_dir_exists),
    F_DEF("fRepCree", "fMakeDir", "Fichiers",
        "Crée un dossier (et parents si nécessaire).", "Create a directory (with parents).",
        sig_dir_void, fn_dir_create),
    F_DEF("fRepSupprime", "fRemoveDir", "Fichiers",
        "Supprime un dossier.", "Delete a directory.", sig_dir_rm, fn_dir_remove),
    { .canonical = "fListeR\xc3\xa9" "pertoire",
      .canonicalAscii = "fListeRepertoire",
      .nameEN = "fListDirectory",
      .category = "Fichiers",
      .descFR = "Liste les sous-répertoires d'un dossier (chemins complets, CRLF-séparés). Récursif par défaut.",
      .descEN = "List subdirectories (full paths, CRLF-separated). Recursive by default.",
      .signatures = sig_dir_list, .impl = fn_dir_list },
    F_DEF("fListeFichier", "fListFile", "Fichiers",
        "Liste les fichiers d'un dossier (chemins complets, CRLF-séparés). Récursif par défaut. Pattern avec * et ? supportés.",
        "List files in a directory (full paths, CRLF-separated). Recursive by default. Glob with * and ? supported.",
        sig_file_list, fn_file_list),
    F_DEF("fRepEnCours", "fCurrentDir", "Fichiers",
        "Sans arg : retourne le dossier courant. Avec un chemin : change le dossier courant et retourne le nouveau.",
        "No arg: returns current directory. With a path: changes current directory and returns new one.",
        sig_cwd, fn_dir_cwd),
    F_DEF("fChangeRepertoire", "fChangeDir", "Fichiers",
        "Change le dossier courant.", "Change the current directory.",
        sig_dir_void, fn_dir_chdir),
    F_DEF("fRepTemp", "fTempDir", "Fichiers",
        "Chemin du dossier temporaire système.", "System temp directory path.",
        sig_no_arg, fn_dir_temp),
    F_DEF("fRepExe", "fExeDir", "Fichiers",
        "Répertoire contenant l'exécutable en cours. Chemin absolu, sans séparateur final.",
        "Directory containing the running executable. Absolute path, no trailing separator.",
        sig_no_arg, fn_dir_exe),
    F_DEF("fDisqueEnCours", "fCurrentDrive", "Fichiers",
        "Identifiant du disque ou de la racine du système de fichiers du répertoire en cours "
        "(lettre de lecteur sous Windows, racine \"/\" ailleurs).",
        "Identifier of the drive or filesystem root of the current directory "
        "(drive letter on Windows, root \"/\" elsewhere).",
        sig_no_arg, fn_current_drive),
    F_DEF("fRepGlobalUtilisateur", "fGlobalDirUser", "Fichiers",
        "Répertoire racine des données applicatives propres à l'utilisateur courant, "
        "partagé par toutes les applications. Chemin absolu, sans séparateur final.",
        "Root directory of application data specific to the current user, shared by all "
        "applications. Absolute path, no trailing separator.",
        sig_no_arg, fn_global_dir_user),
    F_DEF("fRepGlobalCommun", "fGlobalDirCommon", "Fichiers",
        "Répertoire racine des données applicatives partagées entre tous les utilisateurs de la "
        "machine. Chemin absolu, sans séparateur final.",
        "Root directory of application data shared by all users of the machine. "
        "Absolute path, no trailing separator.",
        sig_no_arg, fn_global_dir_common),
    { .canonical = "fRepDonn\xc3\xa9" "esUtilisateur",
      .canonicalAscii = "fRepDonneesUtilisateur",
      .nameEN = "fDataDirUser",
      .category = "Fichiers",
      .descFR = "Répertoire de données de l'application courante, propre à l'utilisateur en cours "
                "(sous-dossier applicatif de fRepGlobalUtilisateur). Chemin absolu, sans séparateur final.",
      .descEN = "Data directory of the current application, specific to the current user "
                "(application subfolder of fGlobalDirUser). Absolute path, no trailing separator.",
      .signatures = sig_no_arg, .impl = fn_data_dir_user },
    { .canonical = "fRepDonn\xc3\xa9" "esCommun",
      .canonicalAscii = "fRepDonneesCommun",
      .nameEN = "fDataDirCommon",
      .category = "Fichiers",
      .descFR = "Répertoire de données de l'application courante, partagé entre tous les utilisateurs "
                "(sous-dossier applicatif de fRepGlobalCommun). Chemin absolu, sans séparateur final.",
      .descEN = "Data directory of the current application, shared by all users "
                "(application subfolder of fGlobalDirCommon). Absolute path, no trailing separator.",
      .signatures = sig_no_arg, .impl = fn_data_dir_common },

    /* Fichier manipulation */
    F_DEF("fCopieFichier", "fCopyFile", "Fichiers",
        "Copie un fichier (ou un pattern glob avec frRécursif).",
        "Copy a file (or a glob pattern with frRecursive).", sig_copy_flags, fn_file_copy),
    F_DEF("fRenomme", "fRename", "Fichiers",
        "Renomme ou déplace un fichier.", "Rename or move a file.",
        sig_copy, fn_file_rename),
    F_DEF("fDeplace", "fMove", "Fichiers",
        "Alias de fRenomme.", "Alias of fRename.", sig_copy, fn_file_rename),

    /* Chemin */
    F_DEF("fExtraitChemin", "fExtractPath", "Fichiers",
        "Extrait un composant d'un chemin (fDisque, fRepertoire, fFichier, fExtension).",
        "Extract a component of a path (fDisque, fRepertoire, fFichier, fExtension).",
        sig_extract, fn_path_extract),

    /* I/O binaire + position */
    F_DEF("fLit", "fRead", "Fichiers",
        "Lit un nombre d'octets.", "Read bytes from file.", sig_read, fn_fread_bytes),
    F_DEF("fPositionne", "fSeek", "Fichiers",
        "Positionne le pointeur dans un fichier.", "Seek to a position in a file.",
        sig_seek, fn_fseek),
    F_DEF("fCree", "fCreate", "Fichiers",
        "Crée un fichier externe et l'ouvre en lecture/écriture. Retourne l'ID (-1 si échec).",
        "Create an external file and open in read/write. Returns file ID (-1 on failure).",
        sig_path_int, fn_fcreate),
    F_DEF("fChangeTaille", "fChangeSize", "Fichiers",
        "Tronque ou étend un fichier.", "Truncate or extend a file.",
        sig_trunc, fn_ftruncate),
    F_DEF("fAjouteTexte", "fAddText", "Fichiers",
        "Ajoute du texte à la fin d'un fichier. L'argument encodage optionnel "
        "(encUTF8 par défaut, encLatin1, encWindows1252, encUTF16LE, encUTF16BE) "
        "transcode le contenu avant ajout.",
        "Append text to a file. The optional encoding argument (encUTF8 by "
        "default, encLatin1, encWindows1252, encUTF16LE, encUTF16BE) transcodes "
        "the content before appending.",
        sig_save_text, fn_fappend_text),
    F_DEF("fAjouteBuffer", "fAddBuffer", "Fichiers",
        "Ajoute un buffer à la fin d'un fichier.", "Append a buffer to a file.",
        sig_save_buf, fn_fappend_buffer),
    F_DEF("fChargeBuffer", "fLoadBuffer", "Fichiers",
        "Charge un fichier en buffer (offset/length optionnels).",
        "Load a file as buffer (optional offset/length).",
        sig_load_buf, fn_fload_buffer),
    F_DEF("fSauveBuffer", "fSaveBuffer", "Fichiers",
        "Enregistre un buffer dans un fichier.", "Save a buffer to a file.",
        sig_save_buf, fn_fsave_buffer),
    F_DEF("fCherche", "fFind", "Fichiers",
        "Cherche une chaîne dans un fichier.", "Search a string in a file.",
        sig_fsearch, fn_fsearch),

    /* Métadonnées */
    /* fTailleFichier / fFileSize sont des aliases legacy : meme
     * semantique que fTaille forme chemin. Resolution via aliases. */
    { .canonical = "fTaille", .nameEN = "fSize",
      .aliases   = ALIASES_fTaille,
      .category  = "Fichiers",
      .descFR    = "Taille d'un fichier en octets (chemin).",
      .descEN    = "File size in bytes (path).",
      .signatures = sig_path_int, .impl = fn_fsize_path },
    F_DEF("fDate", "fDate", "Fichiers",
        "Date de dernière modification.", "Last modification date.",
        sig_fdate, fn_fdate),
    F_DEF("fHeure", "fTime", "Fichiers",
        "Heure de dernière modification.", "Last modification time.",
        sig_fheure, fn_fheure),
    F_DEF("fDateHeure", "fDateTime", "Fichiers",
        "Date et heure de dernière modification.", "Last modification date+time.",
        sig_fdh, fn_fdateheure),

    /* Chemin helpers */
    F_DEF("fSep", "fSep", "Fichiers",
        "Séparateur de chemin du système.", "System path separator.",
        sig_no_arg, fn_fsep),
    F_DEF("fConstruitChemin", "fBuildPath", "Fichiers",
        "Construit un chemin à partir de plusieurs segments.",
        "Build a path from multiple segments.", sig_path_multi, fn_fbuild_path),
    F_DEF("fChangeExtension", "fChangeExtension", "Fichiers",
        "Change l'extension d'un nom de fichier.", "Change a filename's extension.",
        sig_change_ext, fn_fchange_ext),
    F_DEF("fNomFichier", "fFileName", "Fichiers",
        "Nom de fichier (sans dossier).", "Basename (without directory).",
        sig_basename, fn_fbasename),
    { .canonical      = "Compl\xc3\xa8" "teRep",
      .canonicalAscii = "CompleteRep",
      .nameEN         = "CompleteDir",
      .category       = "Fichiers",
      .descFR         = "Ajoute le séparateur de répertoire (\\\\ ou / selon plateforme) si absent.",
      .descEN         = "Append the platform's directory separator to the path if missing.",
      .signatures     = sig_complete_rep, .impl = fn_complete_rep },
    F_DEF("fRepParent", "fParentDir", "Fichiers",
        "Dossier parent d'un chemin.", "Parent directory of a path.",
        sig_basename, fn_fparent),
    F_DEF("fFichierTemp", "fTempFile", "Fichiers",
        "Chemin unique pour un fichier temporaire (préfixe et dossier optionnels).",
        "Unique temp file path (optional prefix and directory).",
        sig_ftempname, fn_ftempname),

    F_DEF("fCompare", "fCompare", "Fichiers",
        "Compare le contenu de deux fichiers.", "Compare the content of two files.",
        sig_copy, fn_fcompare),
    F_DEF("fRepTaille", "fDirSize", "Fichiers",
        "Taille totale d'un dossier (récursive).", "Total size of a directory (recursive).",
        sig_path_int, fn_frepsize),
    F_DEF("fRepCopie", "fCopyDir", "Fichiers",
        "Copie récursive d'un dossier.", "Recursive directory copy.",
        sig_copy, fn_frepcopy),
};

#undef F_DEF

static const OwlConstDef FO_CONSTS[] = {
    /* Famille accès (mutex) */
    {"foLecture",              "Fichiers", INTEGER_VAL(FO_LECTURE),              "Ouverture en lecture seule.",                          "Open read-only.", "foRead"},
    {"foEcriture",             "Fichiers", INTEGER_VAL(FO_ECRITURE),             "Ouverture en écriture seule.",                         "Open write-only.", "foWrite"},
    {"foLectureEcriture",      "Fichiers", INTEGER_VAL(FO_LECTURE_ECRITURE),     "Ouverture en lecture/écriture (défaut).",              "Open read/write (default).", "foReadWrite"},
    {"foAjout",                "Fichiers", INTEGER_VAL(FO_AJOUT),                "Ouverture en ajout (position en fin de fichier).",     "Open for append.", "foAdd"},
    /* Famille création (mutex, combinable avec accès) */
    {"foCr\xc3\xa9" "ation",   "Fichiers", INTEGER_VAL(FO_CREATION),             "Crée ou écrase le fichier (combinable).",              "Create or overwrite (combinable).", "foCreate"},
    {"foCr\xc3\xa9" "ationSiInexistant", "Fichiers", INTEGER_VAL(FO_CREATION_SI_INEXISTANT), "Crée uniquement si le fichier n'existe pas.", "Create only if not exists.", "foCreateIfNotFound"},
    /* Famille blocage */
    {"foBloqueLecture",        "Fichiers", INTEGER_VAL(FO_BLOQUE_LECTURE),       "Empêche les autres processus de lire le fichier jusqu'à fFerme.", "Prevent other processes from reading the file until fClose.", "foReadLock"},
    {"foBloqueEcriture",       "Fichiers", INTEGER_VAL(FO_BLOQUE_ECRITURE),      "Empêche les autres processus d'ouvrir le fichier en écriture jusqu'à fFerme.", "Prevent other processes from opening the file for write until fClose.", "foWriteLock"},
    {"foSuppressionAutomatique","Fichiers",INTEGER_VAL(FO_SUPPRESSION_AUTO),     "Supprime le fichier automatiquement à fFerme.",       "Automatically delete the file at fClose.", "foAutomaticDeletion"},
    /* Optim */
    {"foAcc\xc3\xa8" "sS\xc3\xa9" "quentiel", "Fichiers", INTEGER_VAL(FO_ACCES_SEQUENTIEL), "Optimise pour lecture séquentielle (no-op OWL).", "Optimize for sequential read (no-op in OWL).", "foSequentialAccess"},
};

static const OwlConstDef FP_CONSTS[] = {
    {"fpDebut",   "Fichiers", INTEGER_VAL(FP_DEBUT),   "Origine = premier octet du fichier (SEEK_SET).", "Origin = start of file (SEEK_SET).", "fpBeginning"},
    {"fpCourant", "Fichiers", INTEGER_VAL(FP_COURANT), "Origine = position en cours (SEEK_CUR).",        "Origin = current position (SEEK_CUR).", "fpCurrent"},
    {"fpFin",     "Fichiers", INTEGER_VAL(FP_FIN),     "Origine = dernier octet du fichier (SEEK_END).", "Origin = end of file (SEEK_END).", "fpEnd"},
};

static const OwlConstDef FR_CONSTS[] = {
    {"frR\xc3\xa9" "cursif", "Fichiers", INTEGER_VAL(FR_RECURSIF),
        "Parcours récursif (défaut pour fListe*, opt-in pour fCopieFichier).",
        "Recursive walk (default for fListe*, opt-in for fCopieFichier).", "frRecursive"},
    {"frNonR\xc3\xa9" "cursif", "Fichiers", INTEGER_VAL(FR_NON_RECURSIF),
        "fListe*: limite le parcours au niveau 1.",
        "fListe*: limit walk to level 1.", "frNotRecursive"},
    {"frInterruptible", "Fichiers", INTEGER_VAL(FR_INTERRUPTIBLE),
        "fListe*: ESC interrompt le parcours (no-op en OWL CLI).",
        "fListe*: ESC interrupts walk (no-op in OWL CLI).", "fdInterruptible"},
    {"frInformationCompl\xc3\xa8" "te", "Fichiers", INTEGER_VAL(FR_INFORMATION_COMPLETE),
        "fListeFichier: chaque entrée = chemin+TAB+taille+TAB+date+TAB+attrs.",
        "fListFile: each entry = path+TAB+size+TAB+date+TAB+attrs.", "fdFullInformation"},
    {"frSansFichierCach\xc3\xa9", "Fichiers", INTEGER_VAL(FR_SANS_FICHIER_CACHE),
        "fListeFichier+frInformationComplète: ignore les fichiers cachés.",
        "fListFile+fdFullInformation: ignore hidden files.", "frNoHiddenFile"},
    {"frSansR\xc3\xa9" "pertoireCach\xc3\xa9", "Fichiers", INTEGER_VAL(FR_SANS_REPERTOIRE_CACHE),
        "fListeFichier+frInformationComplète: ignore les répertoires cachés.",
        "fListFile+fdFullInformation: ignore hidden directories.", "frNoHiddenDirectory"},
};

/* Constantes <Change> du callback de fListeFichier (doc 1000024673). */
static const OwlConstDef FL_CONSTS[] = {
    {"flFichier", "Fichiers", INTEGER_VAL(FL_FICHIER),
        "Callback fListeFichier: cas standard (suite du parcours).",
        "fListeFichier callback: standard case (continuing walk).", "flFile"},
    {"flPremierFichier", "Fichiers", INTEGER_VAL(FL_PREMIER_FICHIER),
        "Callback fListeFichier: première entrée listée dans le chemin.",
        "fListeFichier callback: first entry listed in the path.", "flFirstFile"},
    {"flChangeR\xc3\xa9pertoire", "Fichiers", INTEGER_VAL(FL_CHANGE_REPERTOIRE),
        "Callback fListeFichier: première entrée d'un nouveau sous-répertoire.",
        "fListeFichier callback: first entry of a new subdirectory.", "flChangeDirectory"},
};

static const OwlConstDef FDATE_CONSTS[] = {
    {"fModification", "Fichiers", INTEGER_VAL(FD_MODIFICATION),
        "fDate/fHeure/fDateHeure: date de dernière modification (défaut).",
        "fDate/fTime/fDateTime: last modification date (default).", "fModify"},
    /* fCréation : nameEN canon = "fCreation" == alias ASCII auto-genere de la
     * forme FR -> re-set runtime de la MEME cle a la MEME valeur (bénin),
     * posé pour satisfaire la digue nameEN. */
    {"fCr\xc3\xa9" "ation",  "Fichiers", INTEGER_VAL(FD_CREATION),
        "fDate/fHeure/fDateHeure: date de création du fichier.",
        "fDate/fTime/fDateTime: file creation date.", "fCreation"},
    {"fAcc\xc3\xa8s",     "Fichiers", INTEGER_VAL(FD_ACCES),
        "fDate/fHeure/fDateHeure: date du dernier accès.",
        "fDate/fTime/fDateTime: last access date.", "fAccess"},
};

void stdlib_file_init(void) {
    owl_registry_register_module(DEFS_FILE,  (int)(sizeof(DEFS_FILE) /sizeof(DEFS_FILE[0])));
    owl_registry_register_enums (FILE_ENUMS, (int)(sizeof(FILE_ENUMS)/sizeof(FILE_ENUMS[0])));

    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * (frRecursif/frNonRecursif/frInformationComplete/frSansFichierCache/
     * frSansRepertoireCache, flChangeRepertoire, etc.). */
    owl_registry_register_consts(FEXTRAIT_CONSTS,
        (int)(sizeof(FEXTRAIT_CONSTS)/sizeof(FEXTRAIT_CONSTS[0])));
    owl_registry_register_consts(FO_CONSTS, (int)(sizeof(FO_CONSTS)/sizeof(FO_CONSTS[0])));
    owl_registry_register_consts(FP_CONSTS, (int)(sizeof(FP_CONSTS)/sizeof(FP_CONSTS[0])));
    owl_registry_register_consts(FR_CONSTS, (int)(sizeof(FR_CONSTS)/sizeof(FR_CONSTS[0])));
    owl_registry_register_consts(FL_CONSTS, (int)(sizeof(FL_CONSTS)/sizeof(FL_CONSTS[0])));

    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * (fCreation pour fCréation, fAcces pour fAccès, etc.). */
    owl_registry_register_consts(FDATE_CONSTS, (int)(sizeof(FDATE_CONSTS)/sizeof(FDATE_CONSTS[0])));

    /* Enregistre le parser de coerce dans le registre central. Le compilo
     * emettra __InstanceCoerce(v, "FichierDisque") qui dispatch ici. */
    owl_register_instance_coerce("FichierDisque", parse_to_fichierdisque);

    /* Prefix methods on FichierDisque (doc 1410088883).
     * Délèguent aux fn_* existantes, qui acceptent maintenant un handle
     * entier OU une instance FichierDisque via extract_file_handle. */
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Ferme",       fn_fclose,       1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Close",       fn_fclose,       1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Ecrit",       fn_fwrite,       2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Write",       fn_fwrite,       2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "EcritLigne",  fn_fwriteline,   2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "WriteLine",   fn_fwriteline,   2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "LitLigne",    fn_freadline,    1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "ReadLine",    fn_freadline,    1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Lit",         fn_fread_bytes,  2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Read",        fn_fread_bytes,  2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Positionne",  fn_fseek,       -1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Seek",        fn_fseek,       -1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "ChangeTaille",fn_ftruncate,    2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "ChangeSize",  fn_ftruncate,    2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Cherche",     fn_fsearch,      2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Search",      fn_fsearch,      2);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Taille",      fn_ftaille_open, 1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "Size",        fn_ftaille_open, 1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "NomFichier",  fn_fnom_fichier, 1);
    owl_register_method(METHOD_TYPE_FICHIERDISQUE, "FileName",    fn_fnom_fichier, 1);
}
