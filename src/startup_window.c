// SPDX-License-Identifier: AGPL-3.0-only
/* ================================================================
 * STARTUP WINDOW — ouverture de la fenetre de demarrage d'un projet
 * ================================================================
 * Cf. startup_window.h pour le pourquoi (partage owl.exe / bundle owl-build).
 * Le code ci-dessous a ete extrait de main.c (ex go_local_auto_open + helpers
 * de planification des parametres) : source de verite unique pour les deux
 * points d'ouverture (lancement projet owl.exe ET main() genere par owl-build).
 * ================================================================ */

#include "common.h"
#include "startup_window.h"
#include "scanner.h"
#include "vm.h"
#include "table.h"
#include "object.h"
#include "owl_errors.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#  include <io.h>
/* Pas de <windows.h> (clash TokenType winnt.h vs scanner.h, cf. platform.c).
 * Ces deux entrees kernel32 (auto-linkees) sont declarees a la main pour le
 * peek non-bloquant de stdin. */
unsigned long __stdcall GetFileType(void* hFile);
int __stdcall PeekNamedPipe(void* hNamedPipe, void* lpBuffer,
                            unsigned long nBufferSize, unsigned long* lpBytesRead,
                            unsigned long* lpTotalBytesAvail,
                            unsigned long* lpBytesLeftThisMessage);
#else
#  include <unistd.h>
#  include <sys/select.h>
#endif

/* Rendu d'un message d'erreur registry selon la langue active (FR/EN). */
static const char* sw_render(OwlErrorCode code, ...) {
    va_list ap;
    va_start(ap, code);
    const char* s = owl_error_render_active_va(code, ap);
    va_end(ap);
    return s;
}

/* Un type de parametre est-il « simple » (litteral textuel bien defini) ?
 * NULL (parametre non annote) => avance par prudence. Les types date/heure/
 * duree ne sont pas inclus (refus conservateur, cf. ticket). */
static bool sw_param_type_is_simple(const char* typeName) {
    if (typeName == NULL) return false;
    char canon[128];
    scanner_canonicalize_identifier(typeName, (int)strlen(typeName),
                                    canon, sizeof(canon));
    static const char* const simple[] = {
        "chaine", "string",
        "entier", "integer",
        "reel", "real",
        "booleen", "boolean",
        "monetaire", "currency",
        NULL
    };
    for (const char* const* s = simple; *s; s++) {
        if (strcmp(canon, *s) == 0) return true;
    }
    return false;
}

/* Parse une valeur simple depuis le texte saisi selon le type declare. */
static Value sw_parse_simple(const char* typeName, const char* text) {
    char canon[128];
    scanner_canonicalize_identifier(typeName, (int)strlen(typeName),
                                    canon, sizeof(canon));
    if (strcmp(canon, "entier") == 0 || strcmp(canon, "integer") == 0) {
        return INTEGER_VAL((int64_t)strtoll(text, NULL, 10));
    }
    if (strcmp(canon, "reel") == 0 || strcmp(canon, "real") == 0) {
        return REAL_VAL(strtod(text, NULL));
    }
    if (strcmp(canon, "monetaire") == 0 || strcmp(canon, "currency") == 0) {
        return CURRENCY_VAL((int64_t)(strtod(text, NULL) * 1000000.0));
    }
    if (strcmp(canon, "booleen") == 0 || strcmp(canon, "boolean") == 0) {
        bool b = strcasecmp(text, "vrai") == 0 || strcasecmp(text, "true") == 0 ||
                 strcasecmp(text, "oui")  == 0 || strcasecmp(text, "yes")  == 0 ||
                 strcasecmp(text, "1")    == 0;
        return BOOL_VAL(b);
    }
    /* chaine / string / fallback */
    return OBJ_VAL(obj_copy_string(text, (int)strlen(text)));
}

static bool sw_stdin_is_tty(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(STDIN_FILENO) != 0;
#endif
}

/* stdin a-t-il des donnees immediatement lisibles SANS bloquer ? Filet
 * anti-hang du piege  : en contexte non-TTY sans donnees en attente,
 * on ne doit jamais rester bloque sur un fgets. */
static bool sw_stdin_data_ready(void) {
#ifdef _WIN32
    intptr_t h = _get_osfhandle(_fileno(stdin));
    if (h < 0) return false;
    void* handle = (void*)h;
    unsigned long ft = GetFileType(handle);
    if (ft == 0x0003 /* FILE_TYPE_PIPE */) {
        unsigned long avail = 0;
        if (PeekNamedPipe(handle, NULL, 0, NULL, &avail, NULL)) return avail > 0;
        return false;  /* pipe ferme/erreur -> pas de donnees */
    }
    if (ft == 0x0001 /* FILE_TYPE_DISK */) {
        int c = fgetc(stdin);
        if (c == EOF) return false;
        ungetc(c, stdin);
        return true;
    }
    return false;  /* console idle / inconnu -> pas de donnees */
#else
    fd_set rfds;
    struct timeval tv = {0, 0};
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    return select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) > 0;
#endif
}

#define SW_MAX_PARAMS 64
#define SW_PARAM_LINE 256

/*  — canal `--arg <nom>=<valeur>` du lancement direct. Capture des
 * arguments CLI additionnels (ceux qui suivent le nom de fichier) faite dans
 * main() ; consommee par sw_plan_params. NULL/0 hors lancement direct d'un
 * fichier-fenetre avec arguments. */
static int    g_sw_argc = 0;
static char** g_sw_argv = NULL;

void owl_startup_set_cli_args(int argc, char** argv) {
    g_sw_argc = argc;
    g_sw_argv = argv;
}

/* Plan de lancement produit par l'inspection des params (phase SANS alloc
 * OWL). Le parse->Value (qui peut allouer) est differe a owl_run_startup_window,
 * apres avoir roote fn + nom, pour eviter tout Obj fraiche non rootee entre
 * deux allocations (discipline GC roots, ). */
typedef struct {
    int  refuseCode;   /* != 0 : refus/erreur deja imprime, sortir avec ce code */
    int  count;        /* nb de valeurs positionnelles a passer a Ouvre */
    char raw[SW_MAX_PARAMS][SW_PARAM_LINE];
    bool useDefault[SW_MAX_PARAMS];
} SwParamPlan;

/* Inspecte les params de `decl` et remplit `plan` (aucune alloc OWL ici).
 * Imprime le message de refus/erreur le cas echeant (aucun Qt monte). */
static void sw_plan_params(const OwlWindowDecl* decl, const char* fenName,
                           SwParamPlan* plan) {
    plan->refuseCode = 0;
    plan->count = 0;
    if (decl == NULL || decl->param_count <= 0) return;

    /* 1. Type avance (ou trop de params) ? refus immediat. */
    bool advanced = decl->param_count > SW_MAX_PARAMS;
    for (int i = 0; !advanced && i < decl->param_count; i++) {
        if (!sw_param_type_is_simple(decl->params[i].type_name)) advanced = true;
    }
    if (advanced) {
        fprintf(stderr, "%s\n",
                sw_render(E_RT_WINDOW_LAUNCH_ADVANCED_PARAM,
                          "window", fenName, NULL));
        plan->refuseCode = 65;
        return;
    }

    /* 2. Canal `--arg <nom>=<valeur>` (repetable), evalue APRES le refus type
     * avance ci-dessus (ordre  : `--arg` ne contourne jamais le refus).
     * Prioritaire sur stdin : des qu'au moins un `--arg` est fourni, on bind
     * depuis ces valeurs (+ defauts) sans jamais lire stdin (hang-safe). */
    bool argCovered[SW_MAX_PARAMS];
    char argValue[SW_MAX_PARAMS][SW_PARAM_LINE];
    for (int i = 0; i < decl->param_count; i++) argCovered[i] = false;
    bool anyArg = false;
    for (int a = 0; a + 1 < g_sw_argc; a++) {
        if (strcmp(g_sw_argv[a], "--arg") != 0) continue;
        anyArg = true;
        const char* pair = g_sw_argv[a + 1];
        const char* eq = strchr(pair, '=');
        if (eq == NULL) {
            fprintf(stderr, "%s\n",
                    sw_render(E_RT_WINDOW_LAUNCH_ARG_MALFORMED,
                              "window", fenName, "arg", pair, NULL));
            plan->refuseCode = 65;
            return;
        }
        char argCanon[128];
        scanner_canonicalize_identifier(pair, (int)(eq - pair),
                                        argCanon, sizeof(argCanon));
        int match = -1;
        for (int i = 0; i < decl->param_count; i++) {
            char pCanon[128];
            scanner_canonicalize_identifier(decl->params[i].name,
                                            (int)strlen(decl->params[i].name),
                                            pCanon, sizeof(pCanon));
            if (strcmp(argCanon, pCanon) == 0) { match = i; break; }
        }
        if (match < 0) {
            /* Nom fautif rendu tel que saisi (segment avant '='). */
            char badName[128];
            int nlen = (int)(eq - pair);
            if (nlen >= (int)sizeof(badName)) nlen = (int)sizeof(badName) - 1;
            memcpy(badName, pair, (size_t)nlen);
            badName[nlen] = '\0';
            char declared[512];
            int dpos = 0;
            for (int i = 0; i < decl->param_count; i++) {
                int n = snprintf(declared + dpos, sizeof(declared) - (size_t)dpos,
                                 "%s%s", dpos == 0 ? "" : ", ", decl->params[i].name);
                if (n < 0 || dpos + n >= (int)sizeof(declared)) break;
                dpos += n;
            }
            fprintf(stderr, "%s\n",
                    sw_render(E_RT_WINDOW_LAUNCH_ARG_UNKNOWN_PARAM,
                              "window", fenName, "name", badName,
                              "params", declared, NULL));
            plan->refuseCode = 65;
            return;
        }
        snprintf(argValue[match], sizeof(argValue[match]), "%s", eq + 1);
        argCovered[match] = true;
    }

    if (anyArg) {
        /* Requis restant apres binding --arg -> erreur ne nommant QUE les
         * params encore manquants. */
        char missing[512];
        int mo = 0;
        for (int i = 0; i < decl->param_count; i++) {
            if (argCovered[i] || decl->params[i].has_default) continue;
            int n = snprintf(missing + mo, sizeof(missing) - (size_t)mo, "%s%s",
                             mo == 0 ? "" : ", ", decl->params[i].name);
            if (n > 0 && mo + n < (int)sizeof(missing)) mo += n;
        }
        if (mo > 0) {
            fprintf(stderr, "%s\n",
                    sw_render(E_RT_WINDOW_LAUNCH_REQUIRED_PARAM_MISSING,
                              "window", fenName, "params", missing, NULL));
            plan->refuseCode = 65;
            return;
        }
        for (int i = 0; i < decl->param_count; i++) {
            plan->raw[i][0] = '\0';
            plan->useDefault[i] = false;
            if (argCovered[i]) {
                snprintf(plan->raw[i], SW_PARAM_LINE, "%s", argValue[i]);
            } else {
                plan->useDefault[i] = true;  /* garanti has_default (check ci-dessus) */
            }
        }
        plan->count = decl->param_count;
        return;
    }

    /* 3. Aucun `--arg` : source de valeurs stdin — TTY (invite) ou pipe (consomme). */
    bool interactive = sw_stdin_is_tty();
    bool dataReady   = !interactive && sw_stdin_data_ready();

    int requiredCount = 0;
    for (int i = 0; i < decl->param_count; i++) {
        if (!decl->params[i].has_default) requiredCount++;
    }

    if (!interactive && !dataReady) {
        /* Aucune saisie possible sans bloquer. Requis manquant -> erreur. */
        if (requiredCount == 0) return;  /* tous defaultes : ouverture OK */
        char list[512];
        int o = 0;
        for (int i = 0; i < decl->param_count; i++) {
            if (decl->params[i].has_default) continue;
            int n = snprintf(list + o, sizeof(list) - (size_t)o, "%s%s",
                             o == 0 ? "" : ", ", decl->params[i].name);
            if (n < 0 || o + n >= (int)sizeof(list)) break;
            o += n;
        }
        fprintf(stderr, "%s\n",
                sw_render(E_RT_WINDOW_LAUNCH_REQUIRED_PARAM_MISSING,
                          "window", fenName, "params", list, NULL));
        plan->refuseCode = 65;
        return;
    }

    /* Phase lecture (aucune alloc OWL) : une ligne par param, positionnel. */
    char missing[512];
    int  mo = 0;
    for (int i = 0; i < decl->param_count; i++) {
        if (interactive) {
            const char* tn = decl->params[i].type_name;
            fprintf(stderr, "Paramètre '%s'%s%s%s : ", decl->params[i].name,
                    tn ? " (" : "", tn ? tn : "", tn ? ")" : "");
        }
        plan->raw[i][0] = '\0';
        plan->useDefault[i] = false;
        if (fgets(plan->raw[i], SW_PARAM_LINE, stdin) != NULL) {
            size_t len = strlen(plan->raw[i]);
            while (len > 0 && (plan->raw[i][len - 1] == '\n' ||
                               plan->raw[i][len - 1] == '\r')) {
                plan->raw[i][--len] = '\0';
            }
        }
        if (plan->raw[i][0] == '\0') {
            if (decl->params[i].has_default) {
                plan->useDefault[i] = true;
            } else {
                int n = snprintf(missing + mo, sizeof(missing) - (size_t)mo, "%s%s",
                                 mo == 0 ? "" : ", ", decl->params[i].name);
                if (n > 0 && mo + n < (int)sizeof(missing)) mo += n;
            }
        }
    }
    if (mo > 0) {
        fprintf(stderr, "%s\n",
                sw_render(E_RT_WINDOW_LAUNCH_REQUIRED_PARAM_MISSING,
                          "window", fenName, "params", missing, NULL));
        plan->refuseCode = 65;
        return;
    }
    plan->count = decl->param_count;
}

int owl_run_startup_window(const char* fenName) {
    /*  — inspection des params AVANT toute ouverture (donc avant tout
     * montage Qt). Le plan ne fait aucune alloc OWL ; le parse->Value (qui
     * peut allouer une ObjString) est differe plus bas, apres avoir roote
     * fn + nom, avec push immediat par valeur. */
    const OwlWindowDecl* decl = vm_window_lookup(fenName);
    SwParamPlan plan;
    sw_plan_params(decl, fenName, &plan);
    if (plan.refuseCode != 0) return plan.refuseCode;

    /* Lookup global Ouvre. Casse exacte — la native est enregistree
     * sous "Ouvre" et "Open" (alias EN), le canon get gere les deux
     * mais on prend la forme FR canonique. */
    ObjString* fnNameStr = obj_copy_string("Ouvre", 5);
    vm_push(OBJ_VAL(fnNameStr));  /* GC root pendant lookup */
    Value fn;
    bool ok = table_get(&vm.globals, fnNameStr, &fn);
    vm_pop();
    if (!ok) {
        /* Pas de Ouvre globalement — probablement un build minimal.
         * On rend le code de run normal (succes du run du script). */
        return 0;
    }

    /* Push callee + arg (nom du FEN comme chaine), puis les valeurs de params.
     * fn et le nom sont rootes AVANT tout parse pouvant allouer ; chaque
     * valeur est parsee puis poussee immediatement (aucune alloc entre la
     * creation d'une ObjString et son root). */
    ObjString* arg = obj_copy_string(fenName, (int)strlen(fenName));
    vm_push(fn);
    vm_push(OBJ_VAL(arg));
    for (int i = 0; i < plan.count; i++) {
        Value v = plan.useDefault[i]
                    ? decl->params[i].default_value
                    : sw_parse_simple(decl->params[i].type_name, plan.raw[i]);
        vm_push(v);
    }
    InterpretResult r = vm_call_reentrant(fn, 1 + plan.count);
    if (r == INTERPRET_RUNTIME_ERROR) return 70;
    return 0;
}
