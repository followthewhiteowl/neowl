// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Platform glue: UTF-8 console setup + Ctrl-C / SIGINT handler.
 *
 * Kept in its own translation unit so the rest of OWL can avoid
 * including <windows.h>. winnt.h declares an enum constant named
 * TokenType that collides with OWL scanner's TokenType typedef — by
 * isolating the Windows-facing code here we sidestep the clash
 * entirely. Called once from main() at startup. */

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>   /* CommandLineToArgvW */
#include <dbghelp.h>    /* SetUnhandledExceptionFilter + StackWalk64 + Sym* */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#endif

#ifndef _WIN32
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#endif

void vm_request_interrupt(void);

#ifdef _WIN32
static BOOL WINAPI owl_win_ctrl_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        vm_request_interrupt();
        return TRUE; /* swallow — don't terminate the process */
    }
    return FALSE;
}

void owl_force_utf8_console(void) {
    /* Strict UTF-8 on stdio so LSP clients, Jupyter kernels and any piped
     * consumer stop seeing CP-1252 mojibake on accented output. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void owl_install_interrupt_handler(void) {
    SetConsoleCtrlHandler(owl_win_ctrl_handler, TRUE);
}

/* ====================================================================
 * Crash handler + traceur menu
 *
 * Un segfault (0xC0000005) intermittent survient chez le dev quand une
 * fenetre coiffee d'un MenuPrincipal ET d'un MenuContextuel s'affiche au
 * show() natif. Introuvable en lecture statique (heisenbug : memoire
 * non-initialisee / race / use-after-free), invisible headless. On pose
 * donc de quoi CAPTURER la localisation exacte au moment ou ca peourrit :
 *
 *  1. SetUnhandledExceptionFilter -> ecrit une stack symbolisee (autant
 *     que possible) dans un fichier qui survit au crash.
 *  2. owl_menu_trace() -> checkpoints jalons sur le chemin menu, gates sur
 *     la variable d'env OWL_MENU_TRACE. Le dernier jalon imprime avant le
 *     crash localise l'etape.
 *
 * NB symbolisation, deux situations distinctes :
 *  - build de dev : owl.pdb est produit a cote de owl.exe et CONSERVE (pas de
 *    owl_strip_pdb sur cette cible), et les sources qui atterrissent dans
 *    owl.exe sont compilees -g2 -gcodeview (owl_codeview_debug). SymFromAddr
 *    rend donc les noms, statiques comprises, et SymGetLineFromAddr64 y ajoute
 *    fichier:ligne. Le code vendore reste en -g0 : ses frames ne sont nommees
 *    que par le symbole public le plus proche, d'ou le marqueur "nom
 *    approximatif" pose plus bas.
 *  - binaire distribue : --strip-all (OWL_RELEASE_DISTRIB) et le pdb ne part
 *    dans aucune distribution. Aucun nom, donc : chaque frame sort en
 *    `module+RVA` (RVA = adresse - base module), resymbolisable a posteriori
 *    avec addr2line/llvm-symbolizer sur un binaire NON strippe compile depuis
 *    le meme commit. */

/* Chemin d'un fichier temporaire (repertoire %TEMP%, fallback repertoire
 * courant). Robuste : ne touche pas au heap OWL, buffers statiques caller. */
static void owl_temp_file_path(const char* leaf, char* out, DWORD outsz) {
    DWORD n = GetTempPathA(outsz, out);
    if (n == 0 || n >= outsz) {
        /* fallback : repertoire courant */
        out[0] = '.';
        out[1] = '\\';
        out[2] = '\0';
        n = 2;
    }
    /* GetTempPathA garantit un backslash final. */
    size_t leafLen = strlen(leaf);
    if ((size_t)n + leafLen + 1 < (size_t)outsz) {
        memcpy(out + n, leaf, leafLen + 1);
    }
}

/* ---- Traceur menu (jalons) --------------------------------------- */

static int g_menu_trace = -1;   /* -1 = pas encore evalue */

int owl_menu_trace_enabled(void) {
    if (g_menu_trace < 0) {
        const char* v = getenv("OWL_MENU_TRACE");
        g_menu_trace = (v != NULL && v[0] != '\0') ? 1 : 0;
    }
    return g_menu_trace;
}

/* Ecrit un jalon horodate sur stderr ET dans %TEMP%\owl_menu_trace.txt.
 * Le fichier survit meme si stderr est avale (lancement F9 sans console). */
void owl_menu_trace(const char* fmt, ...) {
    if (!owl_menu_trace_enabled()) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (m < 0) return;

    DWORD tid = GetCurrentThreadId();
    fprintf(stderr, "[owl-menu-trace tid=%lu] %s\n", (unsigned long)tid, line);
    fflush(stderr);

    char path[MAX_PATH];
    owl_temp_file_path("owl_menu_trace.txt", path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "[tid=%lu] %s\n", (unsigned long)tid, line);
        fclose(f);
    }
}

/* ---- Traceur cycle de vie QObject ---------------------- */
/* Meme mecanique que owl_menu_trace, gate independant OWL_QOBJ_TRACE, fichier
 * dedie owl_qobj_trace.txt. Trace CREATE/DESTROY des QObjects du chemin menu
 * (pose par owl_qobj_watch dans ui_qt.cpp) pour localiser QUEL objet meurt
 * avant/pendant le show() natif. */

static int g_qobj_trace = -1;

int owl_qobj_trace_enabled(void) {
    if (g_qobj_trace < 0) {
        const char* v = getenv("OWL_QOBJ_TRACE");
        g_qobj_trace = (v != NULL && v[0] != '\0') ? 1 : 0;
    }
    return g_qobj_trace;
}

void owl_qobj_trace(const char* fmt, ...) {
    if (!owl_qobj_trace_enabled()) return;

    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (m < 0) return;

    DWORD tid = GetCurrentThreadId();
    fprintf(stderr, "[owl-qobj-trace tid=%lu] %s\n", (unsigned long)tid, line);
    fflush(stderr);

    char path[MAX_PATH];
    owl_temp_file_path("owl_qobj_trace.txt", path, sizeof(path));
    FILE* f = fopen(path, "a");
    if (f) {
        fprintf(f, "[tid=%lu] %s\n", (unsigned long)tid, line);
        fclose(f);
    }
}

/* ---- Handler de crash --------------------------------------------- */

/* Corps lourd du dump (frame grosse : MAX_PATH x3, copie de CONTEXT, buffer
 * symbole). Separe du filtre pour que le vidage des tampons stdio ait lieu dans
 * une frame minuscule, AVANT que le prologue de celle-ci ne reserve sa pile —
 * point qui compte : le plantage typique est un debordement de pile.
 * noinline est donc porteur de sens ici, pas cosmetique. */
__attribute__((noinline))
static LONG owl_crash_dump(EXCEPTION_POINTERS* ep) {
    char path[MAX_PATH];
    owl_temp_file_path("owl_crash.txt", path, sizeof(path));
    /*  — mode BINAIRE et non tamponne, les deux mesures a l'appui sur
     * la fixture a pile saturee (il reste 4816 octets de pile quand le dump
     * demarre) :
     *  - "a" (texte) : la traduction LF->CRLF d'UCRT reserve un tampon de pile
     *    de plusieurs Ko, donc le tout premier fputs refaute avant d'ecrire un
     *    seul octet — le fichier restait vide ;
     *  - tamponne : le dump refaute plus loin (dbghelp) et emporte tout ce qui
     *    attendait le fflush final.
     * Non tamponne est gratuit ici : ce fichier n'est ecrit que sur un
     * plantage, le facteur ~420 mesure par  sur une sortie de masse ne
     * s'y applique pas. Uniquement si le fopen a reussi : on ne retouche pas le
     * tampon de stderr. */
    FILE* f = fopen(path, "ab");
    if (f) setvbuf(f, NULL, _IONBF, 0);
    else   f = stderr;

    DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : NULL;

    time_t now = time(NULL);
    char tbuf[64];
    struct tm* lt = localtime(&now);
    if (lt) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", lt);
    else    strcpy(tbuf, "?");

    fprintf(f, "\n==== OWL CRASH %s ====\n", tbuf);
    fprintf(f, "exception_code : 0x%08lX", (unsigned long)code);
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        fprintf(f, " (ACCESS_VIOLATION");
        if (ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 2) {
            ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR at = ep->ExceptionRecord->ExceptionInformation[1];
            fprintf(f, " %s @ 0x%p",
                    rw == 0 ? "read" : (rw == 1 ? "write" : "exec"),
                    (void*)at);
        }
        fprintf(f, ")");
    }
    fprintf(f, "\nfault_address  : 0x%p\n", addr);

    /* Module principal (owl.exe) + sa base : ancre pour resymboliser. */
    {
        char exePath[MAX_PATH];
        DWORD en = GetModuleFileNameA(NULL, exePath, sizeof(exePath));
        HMODULE hExe = GetModuleHandleA(NULL);
        if (en > 0) {
            fprintf(f, "exe            : %s\n", exePath);
            fprintf(f, "exe_base       : 0x%p\n", (void*)hExe);
        }
    }

    /* Contexte de travail du deroulement de pile. Declare une seule fois et
     * reutilise par les deux passes ci-dessous : chacune le modifie, mais le
     * repartir de ep->ContextRecord suffit, et une seule copie de CONTEXT
     * (~1,2 Ko) dans la frame est ce qui permet au prologue de tenir sur une
     * pile presque epuisee. */
    CONTEXT ctx;

    /*  — les adresses BRUTES d'abord, la symbolisation ensuite.
     * Mesure sur la fixture a pile saturee : quand le dump demarre il reste
     * 4816 octets de pile, et SymInitialize refaute (marqueur juste avant
     * atteint, juste apres non). Tout ce qui etait produit apres lui — donc la
     * totalite de la backtrace — n'existait jamais. Le deroulement a la main
     * (RtlLookupFunctionEntry + RtlVirtualUnwind) part du contexte de faute
     * exact et n'appelle aucune fonction de dbghelp ; le fichier etant non
     * tamponne, chaque ligne est sur disque avant la suivante. StackWalk64
     * n'est pas une option ici : en x64 il passe par SymFunctionTableAccess64,
     * donc exige justement dbghelp initialise. */
    fprintf(f, "raw_frames (adresses brutes, sans symboles) :\n");
#if defined(_M_X64) || defined(__x86_64__)
    if (ep && ep->ContextRecord) {
        ctx = *ep->ContextRecord;
        for (int i = 0; i < 64; i++) {
            DWORD64 pc = ctx.Rip;
            if (pc == 0) break;
            /* imageBase vient du lookup lui-meme : le RVA est donc gratuit, et
             * c'est lui qui borne l'endroit (a rapprocher de exe_base ci-dessus
             * pour savoir de quel module il s'agit). */
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(pc, &imageBase, NULL);
            if (imageBase && pc >= imageBase) {
                fprintf(f, "  #%02d : 0x%016llX  base 0x%016llX+0x%llX\n",
                        i, (unsigned long long)pc,
                        (unsigned long long)imageBase,
                        (unsigned long long)(pc - imageBase));
            } else {
                fprintf(f, "  #%02d : 0x%016llX\n", i, (unsigned long long)pc);
            }
            /* Pas d'entree de deroulement : feuille ou code genere. On s'arrete
             * plutot que de deviner l'adresse de retour en lisant la pile — sur
             * un debordement, cette lecture peut refauter. */
            if (!rf) break;
            PVOID hdata = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, rf, &ctx,
                             &hdata, &establisher, NULL);
            if (ctx.Rip == pc) break;
        }
    }
#endif

    HANDLE proc = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    /*  — symbolisation live des frames OWL. Le montage historique
     * (SYMOPT_DEFERRED_LOADS + SymInitialize(NULL, invade=TRUE)) ne resolvait
     * PAS les noms du module principal meme avec owl.pdb a cote de l'exe : le
     * chargement differe + search path NULL ne matchaient pas le pdb CodeView.
     * Recette verifiee : search path = dossier de l'exe (ou vit le pdb),
     * chargement EAGER (pas de DEFERRED), et SymLoadModuleEx explicite du module
     * principal a sa base reelle pour forcer l'appariement du pdb. */
    char exeDir[MAX_PATH] = {0};
    {
        char exeFull[MAX_PATH];
        if (GetModuleFileNameA(NULL, exeFull, sizeof(exeFull)) > 0) {
            const char* slash = strrchr(exeFull, '\\');
            size_t dl = slash ? (size_t)(slash - exeFull) : 0;
            if (dl && dl < sizeof(exeDir)) memcpy(exeDir, exeFull, dl);
        }
    }
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    BOOL symOk = SymInitialize(proc, exeDir[0] ? exeDir : NULL, TRUE);
    if (symOk) {
        /* Force l'appariement du pdb du module principal (invade seul suffit
         * rarement pour un pdb CodeView deplace a cote de l'exe). */
        char exeFull[MAX_PATH];
        HMODULE hExe = GetModuleHandleA(NULL);
        if (GetModuleFileNameA(NULL, exeFull, sizeof(exeFull)) > 0) {
            SymLoadModuleEx(proc, NULL, exeFull, NULL,
                            (DWORD64)(uintptr_t)hExe, 0, NULL, 0);
        }
    }

    fprintf(f, "backtrace (frame : addr  module+rva  symbol) :\n");

    if (ep && ep->ContextRecord) {
        /* Repart du contexte de faute : la passe brute ci-dessus a deroule le
         * meme ctx, et StackWalk64 le modifie a son tour. */
        ctx = *ep->ContextRecord;
        STACKFRAME64 frame;
        memset(&frame, 0, sizeof(frame));
        DWORD machine;
#if defined(_M_X64) || defined(__x86_64__)
        machine = IMAGE_FILE_MACHINE_AMD64;
        frame.AddrPC.Offset    = ctx.Rip;
        frame.AddrFrame.Offset = ctx.Rbp;
        frame.AddrStack.Offset = ctx.Rsp;
#elif defined(_M_IX86) || defined(__i386__)
        machine = IMAGE_FILE_MACHINE_I386;
        frame.AddrPC.Offset    = ctx.Eip;
        frame.AddrFrame.Offset = ctx.Ebp;
        frame.AddrStack.Offset = ctx.Esp;
#else
        machine = IMAGE_FILE_MACHINE_UNKNOWN;
#endif
        frame.AddrPC.Mode    = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;

        /* Buffer symbole (nom variable => alloc inline via union). */
        union {
            SYMBOL_INFO si;
            char raw[sizeof(SYMBOL_INFO) + 512];
        } symbuf;

        for (int i = 0; i < 64; i++) {
            if (!StackWalk64(machine, proc, thread, &frame, &ctx,
                             NULL, SymFunctionTableAccess64,
                             SymGetModuleBase64, NULL)) {
                break;
            }
            DWORD64 pc = frame.AddrPC.Offset;
            if (pc == 0) break;

            /* module + RVA */
            const char* modName = "?";
            DWORD64 modBase = 0;
            char modFile[MAX_PATH];
            HMODULE hMod = NULL;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    (LPCSTR)(uintptr_t)pc, &hMod) && hMod) {
                modBase = (DWORD64)(uintptr_t)hMod;
                if (GetModuleFileNameA(hMod, modFile, sizeof(modFile)) > 0) {
                    const char* slash = strrchr(modFile, '\\');
                    modName = slash ? slash + 1 : modFile;
                }
            }
            DWORD64 rva = (modBase && pc >= modBase) ? (pc - modBase) : 0;

            fprintf(f, "  #%02d : 0x%016llX  %s+0x%llX",
                    i, (unsigned long long)pc, modName,
                    (unsigned long long)rva);

            /* Symbole : nomme en dev (owl.pdb conserve a cote de l'exe),
             * absent sur un binaire distribue (strippe, sans pdb). */
            if (symOk) {
                memset(&symbuf, 0, sizeof(symbuf));
                symbuf.si.SizeOfStruct = sizeof(SYMBOL_INFO);
                symbuf.si.MaxNameLen = 512;
                DWORD64 disp = 0;
                if (SymFromAddr(proc, pc, &disp, &symbuf.si)) {
                    /*  — un nom faux ment plus qu'une adresse nue.
                     * Quand un module n'a que ses symboles PUBLICS (pdb sans
                     * info complete, code vendore compile -g0), dbghelp resout
                     * l'adresse sur le symbole qui la precede : le dump affiche
                     * alors le nom d'une AUTRE fonction, sans rien signaler, et
                     * le lecteur part fouiller au mauvais endroit.
                     *
                     * Deux marqueurs mesures separent ce cas d'un nom exact,
                     * aucun seuil arbitraire :
                     *  - Size == 0 : le pdb ne connait pas l'etendue du symbole.
                     *    Mesure dos a dos sur le meme plantage : 0 pour toutes
                     *    les frames owl.exe tant que le module etait compile en
                     *    -g0, non nul (426 / 43 / 4447) des que -gcodeview leur
                     *    donne une vraie entree. Un symbole sans etendue n'est
                     *    qu'un jalon : le nom n'est pas fiable, quel que soit le
                     *    deplacement (mesure : 0x69, assez petit pour paraitre
                     *    credible, sur une fonction qui n'etait pas la bonne).
                     *  - disp >= Size : l'adresse tombe apres la fin de la
                     *    fonction nommee, donc dans une autre. */
                    DWORD64 extent = symbuf.si.Size;
                    const char* approx = "";
                    if (extent == 0) {
                        approx = "  (nom approximatif : symbole le plus proche,"
                                 " etendue inconnue)";
                    } else if (disp >= extent) {
                        approx = "  (nom approximatif : adresse hors de cette"
                                 " fonction)";
                    }
                    fprintf(f, "  %s+0x%llX%s", symbuf.si.Name,
                            (unsigned long long)disp, approx);
                }
                IMAGEHLP_LINE64 lineInfo;
                memset(&lineInfo, 0, sizeof(lineInfo));
                lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                DWORD lineDisp = 0;
                if (SymGetLineFromAddr64(proc, pc, &lineDisp, &lineInfo)) {
                    fprintf(f, "  (%s:%lu)", lineInfo.FileName,
                            (unsigned long)lineInfo.LineNumber);
                }
            }
            fprintf(f, "\n");
        }
    } else {
        /* Pas de contexte : capture la pile du handler lui-meme (moins utile
         * mais mieux que rien). */
        void* frames[64];
        USHORT n = RtlCaptureStackBackTrace(0, 64, frames, NULL);
        for (USHORT i = 0; i < n; i++) {
            fprintf(f, "  #%02u : 0x%p\n", i, frames[i]);
        }
    }

    fprintf(f, "==== fin dump ====\n");
    fflush(f);
    if (f != stderr) fclose(f);

    if (symOk) SymCleanup(proc);

    /* Laisse le comportement par defaut faire son oeuvre (popup WER / exit)
     * pour que le dev voie le meme code=3221225477 qu'avant. */
    return EXCEPTION_CONTINUE_SEARCH;
}

/*  — un plantage emportait TOUTE la sortie deja produite quand elle
 * etait redirigee (fichier ou pipe) : stdout est pleinement tamponne des que le
 * handle n'est pas un terminal, et personne ne vidait le tampon avant que le
 * process meure. Mesure : `owl test` sur la fixture a pile saturee, stdout
 * redirige -> exit 139, 0 octet, alors que deux tests s'etaient acheves.
 *
 * La reponse n'est PAS le non tamponne generalise (mesure  : ~3 s pour
 * 20 000 lignes contre 7 ms, facteur ~420) mais un fflush(NULL) ici : cout nul
 * en fonctionnement normal, effet uniquement sur un process deja condamne.
 *
 * fflush n'est pas async-signal-safe au sens POSIX strict (verrous stdio
 * potentiellement pris par le thread qui plante). Assume : sur un process qui
 * meurt de toute facon, une sortie partielle vaut mieux qu'aucune, et le pire
 * cas (blocage) est borne par le drapeau de reentrance ci-dessous — un plantage
 * pendant le vidage repart en CONTINUE_SEARCH et laisse le systeme tuer le
 * process, jamais de boucle ni de gel.
 *
 * Ordre : vidage AVANT le dump de pile. Ils ne se disputent rien (le dump part
 * dans son propre fichier), et le vidage est l'operation courte et sure des
 * deux — la faire d'abord la met hors d'atteinte d'un re-plantage dans dbghelp.
 * `segfault-unwind` n'est pas concerne : il verifie un demontage propre sans
 * plantage, ce filtre n'y est jamais appele. */
static LONG WINAPI owl_crash_filter(EXCEPTION_POINTERS* ep) {
    /* Anti-recursion : un crash pendant le vidage ou le dump ne doit pas
     * boucler. Pose AVANT toute action, donc couvre aussi le fflush. */
    static volatile LONG in_handler = 0;
    if (InterlockedExchange(&in_handler, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    fflush(NULL);
    return owl_crash_dump(ep);
}

void owl_install_crash_handler(void) {
    SetUnhandledExceptionFilter(owl_crash_filter);
}

/* Sur Windows, argv arrive en ANSI (CP-1252 / locale) — pas UTF-8.
 * Une commande type `owl explain httpRequête` voit donc `httpRequ\xeate`
 * en CP-1252, qui ne matche aucun registry (registre = UTF-8 strict).
 *
 * On récupère donc la commandline brute via GetCommandLineW(), on la
 * découpe en wargv (UTF-16), et on ré-encode chaque arg en UTF-8 avant
 * de pointer *pArgv vers le nouveau tableau. Les allocations restent
 * vivantes jusqu'à la fin du process (≈argc petits malloc) — pas de
 * leak qui importe pour un binaire CLI. */
void owl_normalize_argv_utf8(int* pArgc, char*** pArgv) {
    if (!pArgc || !pArgv) return;
    LPWSTR cmdLine = GetCommandLineW();
    if (!cmdLine) return;
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(cmdLine, &wargc);
    if (!wargv || wargc <= 0) return;

    char** narg = (char**)malloc(sizeof(char*) * (size_t)(wargc + 1));
    if (!narg) { LocalFree(wargv); return; }

    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                    NULL, 0, NULL, NULL);
        if (n <= 0) { narg[i] = _strdup(""); continue; }
        char* buf = (char*)malloc((size_t)n);
        if (!buf) { narg[i] = _strdup(""); continue; }
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, buf, n, NULL, NULL);
        narg[i] = buf;
    }
    narg[wargc] = NULL;

    LocalFree(wargv);
    *pArgc = wargc;
    *pArgv = narg;
}

/*  — Detection locale OS pour LangueSysteme().
 *
 * Win32 : GetUserDefaultLocaleName renvoie deja un tag BCP 47 ("fr-FR",
 * "en-US"). Buffer min LOCALE_NAME_MAX_LENGTH (85 wchar) ; on convertit
 * en UTF-8 dans buf. Fallback "en-US" si appel echoue ou buf trop petit. */
void owl_platform_detect_locale(char* buf, int bufsize) {
    if (!buf || bufsize < 6) return;
    WCHAR wlocale[LOCALE_NAME_MAX_LENGTH];
    int n = GetUserDefaultLocaleName(wlocale, LOCALE_NAME_MAX_LENGTH);
    if (n > 0) {
        int u = WideCharToMultiByte(CP_UTF8, 0, wlocale, -1,
                                    buf, bufsize, NULL, NULL);
        if (u > 0) return;
    }
    /* fallback */
    strncpy(buf, "en-US", (size_t)bufsize - 1);
    buf[bufsize - 1] = '\0';
}
#else
static void owl_sigint_handler(int sig) { (void)sig; vm_request_interrupt(); }

void owl_force_utf8_console(void) { /* Unix: stdio is UTF-8 already */ }

void owl_install_interrupt_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = owl_sigint_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART keeps the REPL readline from being aborted by the
     * signal — the interrupt is delivered to the VM dispatch loop via
     * the flag set by vm_request_interrupt(). */
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
}

void owl_normalize_argv_utf8(int* pArgc, char*** pArgv) {
    (void)pArgc; (void)pArgv;  /* Unix: argv déjà en UTF-8 */
}

/*  — crash handler + traceur menu. Le heisenbug cible etait
 * Windows-only (show() natif windows11), d'ou l'absence historique de tout
 * traitement ici, la plateforme laissant faire le core dump systeme.
 *
 *  — le blackout de sortie, lui, n'a rien de specifique a Windows :
 * stdout est pleinement tamponne des que le handle n'est pas un terminal, donc
 * un plantage emporte tout ce qui n'a pas encore ete ecrit. Meme reponse
 * qu'en Win32 : vider les tampons, ne rien changer d'autre, et redonner la
 * main a la disposition par defaut pour que le core dump reste produit.
 *
 * Points delicats assumes (identiques a la branche Win32) :
 *  - fflush n'est pas async-signal-safe ; sur un process deja condamne, une
 *    sortie partielle vaut mieux qu'aucune. Le drapeau de reentrance borne le
 *    pire cas : un plantage pendant le vidage retombe sur SIG_DFL sans boucler.
 *  - la pile alternative (sigaltstack) est indispensable : sans elle, le
 *    handler ne tourne PAS sur le cas qui nous occupe le plus, le debordement
 *    de pile — le noyau ne peut pas y empiler la frame du handler. */
static volatile sig_atomic_t g_in_crash_handler = 0;

static void owl_crash_signal_handler(int sig) {
    if (g_in_crash_handler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_crash_handler = 1;
    fflush(NULL);
    /* Rend la main au comportement par defaut : core dump + code de sortie
     * habituel, comme si rien n'avait ete installe. */
    signal(sig, SIG_DFL);
    raise(sig);
}

void owl_install_crash_handler(void) {
    /* Taille en dur : SIGSTKSZ n'est plus une constante de compilation sur
     * glibc recente. 64 Ko couvrent largement un fflush. */
    static char altstack[65536];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = owl_crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;

    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        sigaction(sigs[i], &sa, NULL);
    }
}

static int g_menu_trace = -1;

int owl_menu_trace_enabled(void) {
    if (g_menu_trace < 0) {
        const char* v = getenv("OWL_MENU_TRACE");
        g_menu_trace = (v != NULL && v[0] != '\0') ? 1 : 0;
    }
    return g_menu_trace;
}

void owl_menu_trace(const char* fmt, ...) {
    if (!owl_menu_trace_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[owl-menu-trace] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

static int g_qobj_trace = -1;

int owl_qobj_trace_enabled(void) {
    if (g_qobj_trace < 0) {
        const char* v = getenv("OWL_QOBJ_TRACE");
        g_qobj_trace = (v != NULL && v[0] != '\0') ? 1 : 0;
    }
    return g_qobj_trace;
}

void owl_qobj_trace(const char* fmt, ...) {
    if (!owl_qobj_trace_enabled()) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[owl-qobj-trace] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    fflush(stderr);
}

/*  — Detection locale OS pour LangueSysteme().
 *
 * macOS : CFLocaleCopyCurrent() renvoie un identifier style "fr_FR" qui
 * doit etre normalise BCP 47 ("fr-FR" — underscore -> tiret).
 *
 * POSIX : getenv("LANG") renvoie typiquement "fr_FR.UTF-8" — on coupe au
 * point puis on remplace '_' par '-'. Fallback "en-US" si rien detecte
 * ou variable absente (typique de certains containers minimal). */
void owl_platform_detect_locale(char* buf, int bufsize) {
    if (!buf || bufsize < 6) return;
    buf[0] = '\0';

#ifdef __APPLE__
    CFLocaleRef loc = CFLocaleCopyCurrent();
    if (loc) {
        CFStringRef id = CFLocaleGetIdentifier(loc);  /* not retained */
        if (id) {
            CFIndex len = CFStringGetLength(id);
            CFIndex maxBytes = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
            if (maxBytes < bufsize) maxBytes = bufsize;
            char tmp[64];
            if (CFStringGetCString(id, tmp, sizeof(tmp), kCFStringEncodingUTF8)) {
                /* Coupe au '.' eventuel (rare sur macOS, present sur POSIX) */
                char* dot = strchr(tmp, '.');
                if (dot) *dot = '\0';
                /* Normalise '_' -> '-' (RFC 5646 BCP 47) */
                for (char* p = tmp; *p; p++) if (*p == '_') *p = '-';
                strncpy(buf, tmp, (size_t)bufsize - 1);
                buf[bufsize - 1] = '\0';
            }
        }
        CFRelease(loc);
    }
    if (buf[0]) return;
#endif

    const char* lang = getenv("LANG");
    if (!lang || !lang[0]) lang = getenv("LC_ALL");
    if (!lang || !lang[0]) lang = getenv("LC_MESSAGES");
    if (lang && lang[0] && strncmp(lang, "C", 2) != 0 && strncmp(lang, "POSIX", 6) != 0) {
        char tmp[64];
        strncpy(tmp, lang, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        char* dot = strchr(tmp, '.');
        if (dot) *dot = '\0';
        char* at = strchr(tmp, '@');
        if (at) *at = '\0';
        for (char* p = tmp; *p; p++) if (*p == '_') *p = '-';
        if (tmp[0]) {
            strncpy(buf, tmp, (size_t)bufsize - 1);
            buf[bufsize - 1] = '\0';
            return;
        }
    }
    /* fallback */
    strncpy(buf, "en-US", (size_t)bufsize - 1);
    buf[bufsize - 1] = '\0';
}
#endif
