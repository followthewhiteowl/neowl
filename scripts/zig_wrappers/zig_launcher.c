/* zig_launcher.c — wrapper Windows minimal pour zig.
 *
 * Compile en zig-cc.exe / zig-c++.exe / zig-ar.exe / zig-ranlib.exe
 * (le launcher detecte la sous-commande zig via son propre nom d'exe).
 *
 * Pourquoi pas un script .cmd ou .sh ?
 *  - cmd.exe + bash + zig.exe = 3 niveaux d'escape shell qui corrompent
 *    les arguments contenant des quotes (ex -DOWL_VERSION=\\"0.1.0\\").
 *  - Un wrapper natif passe les argv directement a CreateProcess sans
 *    re-parsing shell, ce qui preserve les quotes/espaces.
 *
 * Transformations appliquees :
 *  1. -Wl,-subsystem,X  ->  -Wl,--subsystem,X  (lld vs GNU ld syntax)
 *  2. Pour cc/c++ : ajout auto -L<repo>/vendor/zig/winapi_stubs si dispo
 *     (libsynchronization, libruntimeobject absentes du sysroot zig).
 *
 * Build : cc zig_launcher.c -o zig-cc.exe (ou zig-c++.exe, etc.)
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CMDLINE 65536

/* Append str a cmdline en respectant le quoting Windows :
 * si str contient espace, tab, ou ", entoure de "..." et escape \" en \\". */
static int append_quoted(char* cmdline, int pos, int max, const char* str) {
    if (pos >= max - 1) return pos;
    int needs_quote = 0;
    for (const char* p = str; *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') { needs_quote = 1; break; }
    }
    if (pos > 0 && cmdline[pos-1] != '\0') {
        cmdline[pos++] = ' ';
    }
    if (!needs_quote) {
        size_t len = strlen(str);
        if (pos + (int)len >= max) len = max - pos - 1;
        memcpy(cmdline + pos, str, len);
        pos += (int)len;
    } else {
        cmdline[pos++] = '"';
        for (const char* p = str; *p && pos < max - 4; p++) {
            if (*p == '"') { cmdline[pos++] = '\\'; cmdline[pos++] = '"'; }
            else if (*p == '\\') {
                /* Backslash : echappe seulement s'il precede un " ou en fin. */
                int run = 0;
                while (*p == '\\') { run++; p++; }
                int doubled = (*p == '"' || *p == '\0') ? 2 : 1;
                for (int i = 0; i < run * doubled && pos < max - 2; i++) cmdline[pos++] = '\\';
                if (*p == '"' && pos < max - 2) {
                    cmdline[pos++] = '\\'; cmdline[pos++] = '"';
                }
                if (*p == '\0') break;
                p--;  /* re-entre dans la boucle pour le prochain caractere */
            }
            else cmdline[pos++] = *p;
        }
        if (pos < max - 1) cmdline[pos++] = '"';
    }
    cmdline[pos] = '\0';
    return pos;
}

/* Detecte la sous-commande zig depuis argv[0] basename */
static const char* detect_subcommand(const char* argv0) {
    const char* base = argv0;
    for (const char* p = argv0; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    if (strstr(base, "c++"))    return "c++";
    if (strstr(base, "ranlib")) return "ranlib";
    if (strstr(base, "ar"))     return "ar";
    return "cc";  /* default */
}

/* Resoud le path absolu vers vendor/zig/x86_64-windows/zig.exe a partir
 * du path du launcher : <repo>/scripts/zig_wrappers/zig-cc.exe -->
 * <repo>/vendor/zig/x86_64-windows/zig.exe */
static int resolve_zig_path(char* zig_path, int max) {
    char self[MAX_PATH];
    if (!GetModuleFileNameA(NULL, self, MAX_PATH)) return 0;
    /* self = <repo>\\scripts\\zig_wrappers\\zig-cc.exe
     * zig  = <repo>\\vendor\\zig\\x86_64-windows\\zig.exe */
    char* slash = strrchr(self, '\\');
    if (!slash) return 0;
    *slash = '\0';  /* zig_wrappers */
    slash = strrchr(self, '\\');
    if (!slash) return 0;
    *slash = '\0';  /* scripts */
    slash = strrchr(self, '\\');
    if (!slash) return 0;
    *slash = '\0';  /* repo */
    snprintf(zig_path, max, "%s\\vendor\\zig\\x86_64-windows\\zig.exe", self);
    DWORD attr = GetFileAttributesA(zig_path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

static int resolve_stubs_dir(char* stubs, int max) {
    char self[MAX_PATH];
    if (!GetModuleFileNameA(NULL, self, MAX_PATH)) return 0;
    char* slash = strrchr(self, '\\');
    if (!slash) return 0;
    *slash = '\0'; slash = strrchr(self, '\\'); if (!slash) return 0;
    *slash = '\0'; slash = strrchr(self, '\\'); if (!slash) return 0;
    *slash = '\0';
    snprintf(stubs, max, "%s\\vendor\\zig\\winapi_stubs", self);
    DWORD attr = GetFileAttributesA(stubs);
    return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
}

int main(int argc, char* argv[]) {
    char zig_path[MAX_PATH];
    if (!resolve_zig_path(zig_path, sizeof(zig_path))) {
        fprintf(stderr, "zig-launcher: zig.exe not found at expected path\n");
        return 1;
    }
    const char* subcmd = detect_subcommand(argv[0]);

    /* Construit cmdline : "zig.exe" subcmd [transformed args] */
    char* cmdline = (char*)malloc(MAX_CMDLINE);
    if (!cmdline) return 1;
    int pos = 0;
    pos = append_quoted(cmdline, pos, MAX_CMDLINE, zig_path);
    pos = append_quoted(cmdline, pos, MAX_CMDLINE, subcmd);

    /* Pour cc/c++ : ajoute -L winapi_stubs si dispo */
    if (strcmp(subcmd, "cc") == 0 || strcmp(subcmd, "c++") == 0) {
        char stubs[MAX_PATH];
        if (resolve_stubs_dir(stubs, sizeof(stubs))) {
            char L_arg[MAX_PATH + 4];
            snprintf(L_arg, sizeof(L_arg), "-L%s", stubs);
            pos = append_quoted(cmdline, pos, MAX_CMDLINE, L_arg);
        }
    }

    /* Forward args avec transformation -Wl,-subsystem,X -> -Wl,--subsystem,X */
    const char* SS_PREFIX = "-Wl,-subsystem,";
    size_t ss_len = strlen(SS_PREFIX);
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], SS_PREFIX, ss_len) == 0) {
            char fixed[1024];
            snprintf(fixed, sizeof(fixed), "-Wl,--subsystem,%s", argv[i] + ss_len);
            pos = append_quoted(cmdline, pos, MAX_CMDLINE, fixed);
        } else {
            pos = append_quoted(cmdline, pos, MAX_CMDLINE, argv[i]);
        }
    }

    /* Lance zig */
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(zig_path, cmdline, NULL, NULL, TRUE,
                        0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "zig-launcher: CreateProcess failed (err %lu)\n",
                GetLastError());
        free(cmdline);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cmdline);
    return (int)exit_code;
}
