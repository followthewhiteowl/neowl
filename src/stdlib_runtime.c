// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_runtime.c — Natives d'introspection de l'environnement OWL.
 *
 * Pour l'instant : OwlInfoLibrairies() / OwlLibraryInfo() qui retourne
 * un JSON listant les libraries embarquees dans le binaire avec leur
 * version et licence. Utile pour la conformite des licences open source
 * et pour debug.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "vm.h"
/*  : skip section compile-aware d'owl_errors.h pour eviter la
 * collision TokenType (scanner.h vs winnt.h tire par windows.h sur Win32). */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "object.h"
#include "value.h"
#include "common.h"
#include "testing.h"

#ifdef OWL_HAS_GUI
#  include "owl_ui.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32)
/* GetUserNameA (advapi32) — évite d'inclure windows.h qui collisionne avec
 * TokenType (scanner.h) sous le build GUI. */
__declspec(dllimport) int __stdcall GetUserNameA(char* lpBuffer, unsigned long* pcbBuffer);
/* ShellExecuteA (shell32) — ouvre une cible via le handler par defaut de l'OS.
 * Proto declare a la main pour la meme raison. HINSTANCE degrade en void* ;
 * la regle Win32 historique est : retour > 32 = succes. */
__declspec(dllimport) void* __stdcall ShellExecuteA(void* hwnd, const char* lpOperation,
                                                    const char* lpFile, const char* lpParameters,
                                                    const char* lpDirectory, int nShowCmd);
#else
#  include <unistd.h>
#  include <pwd.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#endif

/* Sommeil OS-level, utilisé seulement quand on tourne sans backend GUI
 * (OWL_HAS_GUI non défini). Avec GUI, owl_ui_wait_ms s'occupe de l'attente
 * et fait tourner l'event loop. */
#ifndef OWL_HAS_GUI
#  if defined(_WIN32)
#    include <windows.h>
static void owl_runtime_sleep_ms(int ms) { Sleep((DWORD)ms); }
#  else
#    include <time.h>
static void owl_runtime_sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#  endif
#endif

/* Versions hardcodees correspondant a ce qui est compile dans ce binaire :
 * qpdf 12.3.2 vient de CMakeLists::FetchContent (URL fixe), Qt6 6.8+ vient
 * du paquet MSYS2 ucrt64. Si on bumpe une version dans le build, on bumpe
 * ici. Le self-test-registry pourra etre etendu pour le verifier. */

/* Build statique liste les libs embedees au moment du build. Les flags
 * OWL_HAS_GUI / OWL_HAS_QPDF / OWL_GUI_STATIC sont definis via cmake. */
static Value fn_owl_info_librairies(int argCount, Value* args) {
    (void)argCount; (void)args;

    /* On construit le JSON en static buffer (~4 KB suffisent largement). */
    static char buf[4096];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n,
        "{\n"
        "  \"owl\": {\n"
        "    \"version\": \"%s\",\n"
        "    \"build\": \"%s\"\n"
        "  },\n",
        OWL_VERSION,
#ifdef OWL_GUI_STATIC
        "static"
#elif defined(OWL_HAS_GUI)
        "dynamic"
#else
        "cli"
#endif
    );

    /* Libs embarquees vendor/ (toutes static, dans le repo). */
    n += snprintf(buf + n, sizeof(buf) - n,
        "  \"vendor\": [\n"
        "    {\"name\": \"mbedtls\",   \"version\": \"3.6.x\",  \"license\": \"Apache-2.0\", \"role\": \"TLS pour HTTP/HTTPS\"},\n"
        "    {\"name\": \"miniz\",     \"version\": \"3.0.x\",  \"license\": \"MIT\",        \"role\": \"compression deflate / zip\"},\n"
        "    {\"name\": \"lzma_sdk\",  \"version\": \"24.x\",   \"license\": \"PublicDomain\",\"role\": \"compression LZMA\"},\n"
        "    {\"name\": \"pcre2\",     \"version\": \"10.45\",  \"license\": \"BSD-2\",      \"role\": \"regex\"},\n"
        "    {\"name\": \"md4c\",      \"version\": \"0.5.x\",  \"license\": \"MIT\",        \"role\": \"parser Markdown CommonMark+GFM\"},\n"
        "    {\"name\": \"ca-bundle\", \"version\": \"Mozilla\",\"license\": \"MPL-2.0\",    \"role\": \"certificats CA pour TLS\"}\n"
        "  ],\n");

#ifdef OWL_HAS_GUI
    /* Qt6 + qpdf : lies seulement dans owl.exe (pas la variante owl-cli). */
    n += snprintf(buf + n, sizeof(buf) - n,
        "  \"gui\": [\n"
        "    {\"name\": \"Qt6\",       \"version\": \"6.8+\",   \"license\": \"LGPL-3.0\",   \"role\": \"backend graphique + PDF rendering\"},\n"
        "    {\"name\": \"Liberation Fonts\", \"version\": \"2.x\", \"license\": \"OFL-1.1\", \"role\": \"polices embarquees Sans/Serif/Mono\"},\n"
        "    {\"name\": \"DejaVu Sans\", \"version\": \"2.37\",  \"license\": \"DejaVu/Bitstream\", \"role\": \"police Unicode large\"}");
#  ifdef OWL_HAS_QPDF
    n += snprintf(buf + n, sizeof(buf) - n,
        ",\n"
        "    {\"name\": \"qpdf\",      \"version\": \"12.3.2\", \"license\": \"Apache-2.0\", \"role\": \"post-traitement PDF (chiffrement, signets, metadata, attachments)\"},\n"
        "    {\"name\": \"zlib\",      \"version\": \"1.3.x\",  \"license\": \"zlib\",       \"role\": \"compression flate (dep qpdf)\"},\n"
        "    {\"name\": \"libjpeg\",   \"version\": \"3.x\",    \"license\": \"IJG\",        \"role\": \"compression JPEG (dep qpdf)\"}");
#  endif
    n += snprintf(buf + n, sizeof(buf) - n, "\n  ],\n");
#endif

    n += snprintf(buf + n, sizeof(buf) - n,
        "  \"crypto_provider\": \"%s\"\n"
        "}\n",
#ifdef OWL_HAS_QPDF
        "qpdf-native (sha2 + AES rijndael, cross-platform pur C++)"
#else
        "mbedtls (TLS uniquement, pas de chiffrement PDF)"
#endif
    );

    return OBJ_VAL(obj_copy_string(buf, n));
}

/* Identification de la plateforme d'execution. Doc : EnModeWindows /
 * EnModeLinux / EnModeAndroid / EnModeIOS retournent un booleen. Detection
 * compile-time via macros standard. */
static Value fn_en_mode_windows(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(_WIN32)
    return BOOL_VAL(true);
#else
    return BOOL_VAL(false);
#endif
}

static Value fn_en_mode_linux(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(__linux__) && !defined(__ANDROID__)
    return BOOL_VAL(true);
#else
    return BOOL_VAL(false);
#endif
}

static Value fn_en_mode_android(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(__ANDROID__)
    return BOOL_VAL(true);
#else
    return BOOL_VAL(false);
#endif
}

static Value fn_en_mode_ios(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(__APPLE__) && defined(__MACH__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return BOOL_VAL(true);
#else
    return BOOL_VAL(false);
#endif
}

static Value fn_en_mode_mac(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(__APPLE__) && defined(__MACH__) && (!defined(TARGET_OS_IPHONE) || !TARGET_OS_IPHONE)
    return BOOL_VAL(true);
#else
    return BOOL_VAL(false);
#endif
}

/* Temporise([durée]) — met en pause l'exécution.
 *
 * Trois formes acceptées :
 *   entier  : nombre de centièmes de seconde (canon historique). Négatif
 *             accepté → ABS.
 *   Durée   : valeur native, utilise totalMs (toujours en absolu).
 *   chaîne  : forme courte "<n><unité>" (ms/cs/s/mn). Cf
 *             owl_duree_parse_short_form.
 *
 * Court-circuit total sous le runner de tests unitaires : la pause est
 * complètement ignorée (retour immédiat), ce qui garantit que les
 * scénarios mettant Temporise en jeu ne ralentissent pas la suite.
 *
 * Hors mode test, si OWL_HAS_GUI est défini, on délègue à owl_ui_wait_ms
 * avec pump_events=1 → l'interface reste fluide. Sinon, sommeil OS direct.
 *
 * Erreur soft (ErreurDétectée=Vrai) si le type d'argument est invalide
 * ou la chaîne non reconnue. */
static Value fn_temporise(int argCount, Value* args) {
    /* Sans argument : no-op silencieux. */
    if (argCount == 0) return NIL_VAL;
    if (argCount > 1) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "Temporise",
                              "native", "Temporise", NULL);
        return NIL_VAL;
    }

    /* Validation du type AVANT le court-circuit test unitaire : un argument
     * invalide doit positionner ErreurDétectée même sous owl test, sinon les
     * tests ciblant la robustesse ne peuvent pas vérifier le contrat. */
    Value v = args[0];
    int64_t ms = 0;

    if (IS_INTEGER(v)) {
        int64_t cs = AS_INTEGER(v);
        if (cs < 0) cs = -cs;
        ms = cs * 10;
    } else if (IS_DURATION(v)) {
        int64_t t = AS_DURATION(v)->totalMs;
        if (t < 0) t = -t;
        ms = t;
    } else if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        int64_t parsed = owl_duree_parse_short_form(s->chars, s->length);
        if (parsed < 0) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "Temporise",
                                  "native", "Temporise", NULL);
            return NIL_VAL;
        }
        ms = parsed;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "Temporise",
                              "native", "Temporise",
                              "arg", "duree",
                              "expected", "entier, Duree ou chaine",
                              "actual", "autre",
                              NULL);
        return NIL_VAL;
    }

    /*  : sous le mode headless (owl test par defaut), on
     * court-circuite la pause effective pour ne pas ralentir la suite et
     * eviter qu'un test bloque sur une attente. */
    if (vm.headless_mode) return NIL_VAL;

    if (ms <= 0) return NIL_VAL;
    /* Clamp défensif : INT_MAX ms = ~24 jours, largement suffisant. */
    if (ms > 0x7FFFFFFFLL) ms = 0x7FFFFFFFLL;

#ifdef OWL_HAS_GUI
    owl_ui_wait_ms((int)ms, 1);
#else
    owl_runtime_sleep_ms((int)ms);
#endif
    return NIL_VAL;
}

/* RéseauUtilisateur() -> nom de l'utilisateur du système en cours de session.
 * Renvoie le seul login (pas le domaine). Chaîne vide si indéterminable. */
static Value fn_reseau_utilisateur(int argCount, Value* args) {
    (void)argCount; (void)args;
#if defined(_WIN32)
    char buf[256];
    unsigned long n = (unsigned long)sizeof(buf);
    if (GetUserNameA(buf, &n) && n > 0) {
        /* n inclut le terminateur nul : la longueur utile est n-1. */
        int len = (int)n - 1;
        if (len < 0) len = 0;
        return OBJ_VAL(obj_copy_string(buf, len));
    }
    const char* env = getenv("USERNAME");
    if (env != NULL && env[0] != '\0') return OBJ_VAL(obj_copy_string(env, (int)strlen(env)));
    return OBJ_VAL(obj_copy_string("", 0));
#else
    struct passwd* pw = getpwuid(getuid());
    if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
        return OBJ_VAL(obj_copy_string(pw->pw_name, (int)strlen(pw->pw_name)));
    }
    const char* env = getenv("USER");
    if (env == NULL || env[0] == '\0') env = getenv("LOGNAME");
    if (env != NULL && env[0] != '\0') return OBJ_VAL(obj_copy_string(env, (int)strlen(env)));
    return OBJ_VAL(obj_copy_string("", 0));
#endif
}

/* OuvreURL(url) — ouvre l'URL dans le gestionnaire par defaut du systeme
 * (navigateur pour http/https, application associee pour mailto:, etc.).
 * Retourne Vrai si le lancement a reussi, Faux sinon.
 *
 * Aucune injection shell possible : sous Windows via ShellExecuteA (l'URL
 * n'transite jamais par cmd.exe) ; sous Unix via fork + execlp avec l'URL
 * passee en argv brut (jamais interpretee par un shell). */
static Value fn_ouvre_url(int argCount, Value* args) {
    if (argCount < 1 || !IS_STRING(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "OuvreURL",
                              "native", "OuvreURL",
                              "arg", "url",
                              "expected", "chaine",
                              "actual", "autre",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* url = AS_CSTRING(args[0]);
#if defined(_WIN32)
    /* SW_SHOWNORMAL = 1. Regle Win32 : valeur de retour > 32 = succes. */
    void* r = ShellExecuteA(NULL, "open", url, NULL, NULL, 1);
    return BOOL_VAL((intptr_t)r > 32);
#else
#  if defined(__APPLE__)
    const char* launcher = "open";
#  else
    const char* launcher = "xdg-open";
#  endif
    /* Double-fork : le petit-enfant qui exec le lanceur est reparente a init,
     * ce qui evite tout zombie cote OWL. Le fork intermediaire est reape
     * immediatement par waitpid. On ne peut pas savoir si execlp a reussi
     * (petit-enfant detache) : le succes reflete l'aboutissement du fork. */
    pid_t pid = fork();
    if (pid < 0) return BOOL_VAL(false);
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            execlp(launcher, launcher, url, (char*)NULL);
            _exit(127);  /* execlp a echoue */
        }
        _exit(grandchild < 0 ? 127 : 0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return BOOL_VAL(false);
    return BOOL_VAL(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

/* ============================================================
 * Registry
 * ============================================================ */

static const OwlParam p_simple[]  = {{0}};
static const OwlSignature sig_owl_info[] = {
    {0, p_simple, "cha\xc3\xaene", "JSON listant les libs embarquees"},
    {0}
};
static const OwlSignature sig_en_mode[] = {
    {0, p_simple, "bool\xc3\xa9" "en", "Vrai/Faux"},
    {0}
};
static const OwlSignature sig_str_no_arg[] = {
    {0, p_simple, "cha\xc3\xaene", "cha\xc3\xaene r\xc3\xa9sultat"},
    {0}
};

static const OwlParam p_ouvre_url[] = {
    {"url", "cha\xc3\xaene", "URL ou cible \xc3\xa0 ouvrir (http, https, mailto, ...)"},
    {0}
};
static const OwlSignature sig_ouvre_url[] = {
    {1, p_ouvre_url, "bool\xc3\xa9" "en", "Vrai si le lancement a r\xc3\xa9ussi"},
    {0}
};

/* Param de Temporise : accepte entier (centièmes), Durée, ou chaîne courte. */
static const OwlParam p_temporise[] = {
    {"dur\xc3\xa9" "e", "entier|dur\xc3\xa9" "e|cha\xc3\xaene",
     "entier (centi\xc3\xa8mes), Dur\xc3\xa9" "e, ou cha\xc3\xaene courte (\"200ms\", \"1s\", \"10cs\", \"5mn\")"},
    {0}
};
static const OwlSignature sig_temporise[] = {
    {0, NULL,        "rien", "no-op imm\xc3\xa9" "diat"},
    {1, p_temporise, "rien", "met en pause pendant la dur\xc3\xa9" "e indiqu\xc3\xa9" "e"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    { .canonical="OwlInfoLibrairies",  .nameEN="OwlLibraryInfo",
      .category="Runtime",
      .descFR="Retourne un JSON listant les libraries embarquees dans ce binaire OWL avec leur version et licence (utile pour conformite legale et debug). Inclut OWL lui-même, les libs vendor (mbedtls, miniz, pcre2...), et selon le build : backend graphique, fontes, generateur PDF.",
      .descEN="Returns a JSON listing the libraries embedded in this OWL binary with their version and license (useful for legal compliance and debug). Includes OWL itself, vendor libs (mbedtls, miniz, pcre2...), and per build: graphics backend, fonts, PDF generator.",
      .signatures=sig_owl_info, .impl=fn_owl_info_librairies },
    { .canonical="R\xc3\xa9seauUtilisateur", .canonicalAscii="ReseauUtilisateur",
      .nameEN="NetworkUser", .category="Runtime",
      .descFR="Renvoie le nom de l'utilisateur du système en cours de session (login seul, sans domaine). Chaîne vide si l'identité ne peut être déterminée.",
      .descEN="Returns the name of the operating-system user of the current session (login only, no domain). Empty string if the identity cannot be determined.",
      .signatures=sig_str_no_arg, .impl=fn_reseau_utilisateur },
    { .canonical="OuvreURL", .nameEN="OpenURL", .category="Syst\xc3\xa8me",
      .descFR="Ouvre une URL dans le gestionnaire par defaut du systeme : le navigateur pour http et https, l'application associee pour les autres schemas (mailto, tel, ...). Renvoie Vrai si le lancement a reussi, Faux sinon.",
      .descEN="Opens a URL in the operating-system default handler: the browser for http and https, the associated application for other schemes (mailto, tel, ...). Returns True if the launch succeeded, False otherwise.",
      .signatures=sig_ouvre_url, .impl=fn_ouvre_url },
    { .canonical="EnModeWindows", .nameEN="InWindowsMode", .category="Runtime",
      .descFR="Renvoie Vrai si le code s'execute sur Windows.",
      .descEN="Returns True if the code is running on Windows.",
      .signatures=sig_en_mode, .impl=fn_en_mode_windows },
    { .canonical="EnModeLinux", .nameEN="InLinuxMode", .category="Runtime",
      .descFR="Renvoie Vrai si le code s'execute sur Linux (hors Android).",
      .descEN="Returns True if the code is running on Linux (excluding Android).",
      .signatures=sig_en_mode, .impl=fn_en_mode_linux },
    { .canonical="EnModeAndroid", .nameEN="InAndroidMode", .category="Runtime",
      .descFR="Renvoie Vrai si le code s'execute sur Android.",
      .descEN="Returns True if the code is running on Android.",
      .signatures=sig_en_mode, .impl=fn_en_mode_android },
    { .canonical="EnModeIOS", .nameEN="InIOSMode", .category="Runtime",
      .descFR="Renvoie Vrai si le code s'execute sur iOS.",
      .descEN="Returns True if the code is running on iOS.",
      .signatures=sig_en_mode, .impl=fn_en_mode_ios },
    { .canonical="EnModeMac", .nameEN="InMacMode", .category="Runtime",
      .descFR="Renvoie Vrai si le code s'execute sur macOS.",
      .descEN="Returns True if the code is running on macOS.",
      .signatures=sig_en_mode, .impl=fn_en_mode_mac },
    { .canonical="Temporise", .nameEN="Wait", .category="Runtime",
      .descFR="Met en pause l'execution pendant la duree indiquee. Accepte un entier (centiemes de seconde), une Duree, ou une chaine courte (\"200ms\", \"1s\", \"10cs\", \"5mn\"). Sans argument ou avec 0, ne fait rien. Sous le runner de tests unitaires, la pause est court-circuitee. L'interface utilisateur reste fluide pendant l'attente quand un backend graphique est actif.",
      .descEN="Pauses execution for the given duration. Accepts an integer (hundredths of a second), a Duration, or a short string (\"200ms\", \"1s\", \"10cs\", \"5mn\"). With no argument or 0, does nothing. Under the unit test runner, the pause is short-circuited. The user interface remains responsive while waiting when a graphical backend is active.",
      .signatures=sig_temporise, .impl=fn_temporise },
};

void stdlib_runtime_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
}
