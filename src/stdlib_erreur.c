// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * Gestion des erreurs "soft" (Erreur* family).
 *
 * Mécanisme parallèle aux exceptions : les fonctions stdlib qui peuvent
 * échouer retournent un code d'échec (faux, -1, 0…) ET mettent à jour un
 * contexte d'erreur global. Le code utilisateur teste `ErreurDetectee` puis
 * interroge `ErreurInfo(errXxx)` pour les détails.
 *
 * Distinction vs exceptions :
 *   - Erreur  = non-bloquante, signalée par la valeur de retour
 *   - Exception = bloquante, dépile jusqu'à un handler QUAND EXCEPTION
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_globalvar_catalog.h"
#include "vm.h"
#include "gc.h"  /*  : gc_mark_object pour stdlib_erreur_mark_roots */
/*  : alias numerique. On ne tire que l'API d'introspection du
 * registre erreurs ; le skip evite la collision TokenType (winnt.h vs
 * scanner.h) puisque ce module touche au runtime VM. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * ETAT GLOBAL
 * ================================================================ */

static bool       g_err_detected = false;
static const char*  g_err_code   = NULL;  /* Migre int -> const char* (chaîne canonique)  */
static ObjString* g_err_message  = NULL;
static ObjString* g_err_native_name = NULL;  /* Nom de la native C qui a pose l'erreur */
static int        g_err_line     = 0;
static ObjString* g_err_procedure = NULL;
static ObjString* g_err_filename = NULL;
static ObjString* g_err_stack    = NULL;
static int64_t    g_err_timestamp_ms = 0;

/* Sentinelle "erreur fraîche dans l'instruction courante".
 *
 *  ligne 17 = contrat user : le pattern canonique
 *   c est une chaîne = fNative(...)
 *   Si ErreurDétectée Alors ...
 * DOIT détecter l'échec de fNative. Or le compilo intercale silencieusement
 * un OP_CALL de coercition (__EntierCoerce, __BufferVersChaine, ...) entre
 * l'OP_CALL de fNative et l'OP_DEFINE_GLOBAL. Ce 2e OP_CALL fait un RAZ
 * pré-call qui écrase l'erreur fraîche posée par fNative.
 *
 * Solution : owl_set_error() pose `g_err_fresh=true`. Le PROCHAIN
 * owl_clear_error() (pré-call) consomme la sentinelle SANS toucher au
 * flag — l'erreur survit un cran de plus, le temps que l'instruction
 * statement se termine. ErreurRAZ() user (owl_clear_error_full) reset
 * aussi la sentinelle.
 *
 * Effet de bord assumé (ligne 17 prime sur ligne 13) : si l'instruction
 * suivante est aussi un OP_CALL (ex. `fEchec() ; Trace(...)`), le pré-call
 * de Trace consomme la sentinelle sans RAZ, donc le flag survit jusqu'à
 * l'OP_CALL d'après. Coût acceptable pour rendre fiable le pattern de
 * capture, qui est le cas d'usage dominant. Cf. . */
static bool       g_err_fresh    = false;

/* Définition catalogue de la variable globale prédéfinie ErreurDétectée.
 * Source de vérité unique : ce OwlGlobalVarDef enregistré au boot. Le
 * runtime, le DAP et le CLI doc l'interrogent — aucun nom dupliqué. */
static const OwlGlobalVarDef ERR_DETECTED_DEF = {
    .canonical = "ErreurD\xc3\xa9tect\xc3\xa9""e",   /* ErreurDétectée */
    .nameAscii = "ErreurDetectee",
    .nameEN    = "ErrorOccurred",                    /* canon doc */
    .type      = "bool\xc3\xa9""en",                  /* booléen */
    .descFR    =
        "Variable globale prédéfinie indiquant le résultat de la dernière "
        "fonction utilisée. Vaut Vrai si la dernière fonction a "
        "provoqué une erreur non fatale, Faux sinon. Lecture seule côté "
        "utilisateur — mise à jour automatique après chaque native qui "
        "peut signaler une soft-error. Détails de l'erreur via ErreurInfo.",
    .descEN    =
        "Predefined global variable indicating the result of the last "
        "function used. True if the last function caused a "
        "non-fatal error, False otherwise. Read-only from user code — "
        "updated automatically after each native that may signal a "
        "soft-error. Error details via ErreurInfo.",
    .readOnly  = true,
};

/* Update the public `ErreurDétectée` global so `SI ErreurDétectée ALORS ...`
 * works naturally. Itère les 3 noms enregistrés dans le catalogue. */
static void sync_err_detected_global(void) {
    const char* names[3] = {
        ERR_DETECTED_DEF.canonical,
        ERR_DETECTED_DEF.nameAscii,
        ERR_DETECTED_DEF.nameEN,
    };
    for (int i = 0; i < 3; i++) {
        if (!names[i]) continue;
        /*  : delegue au canon obj_globals_set. */
        obj_globals_set(names[i], BOOL_VAL(g_err_detected));
    }
}

/* Transport soft-error pour natives ( Lot 4).
 * Appele depuis owl_emit_native_error() via owl_errors_native.c.
 * Stocke le code symbole canonique ("E_NATIVE_ARG_INVALID") au lieu d'un int,
 * + le nom de la native C qui a pose l'erreur (ex: "HashChaine"). */
void owl_set_error_native(const char* code_canonical,
                           const char* native_name,
                           const char* message) {
    g_err_detected = true;
    g_err_fresh    = true;
    g_err_code = code_canonical;  /* Chaîne statique (pointe dans OWL_ERRORS[]) */
    g_err_message = obj_copy_string(message, (int)strlen(message));
    g_err_native_name = obj_copy_string(native_name, (int)strlen(native_name));
    g_err_timestamp_ms = (int64_t)time(NULL) * 1000;
    /* Capture caller context from the top VM frame. */
    if (vm.frameCount > 0) {
        CallFrame* top = &vm.frames[vm.frameCount - 1];
        ObjFunction* fn = top->function;
        size_t instr = top->ip - fn->chunk->code - 1;
        g_err_line = fn->chunk->lines[instr];
        g_err_filename = fn->filename;
        g_err_procedure = fn->name;
    } else {
        g_err_line = 0;
        g_err_filename = NULL;
        g_err_procedure = NULL;
    }
    /* Stack trace. */
    char buf[2048];
    int off = 0;
    for (int i = vm.frameCount - 1; i >= 0 && off < (int)sizeof(buf) - 80; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* fn = frame->function;
        size_t instr = frame->ip - fn->chunk->code - 1;
        int line = fn->chunk->lines[instr];
        const char* name = fn->name == NULL ? "<script>" : fn->name->chars;
        const char* file = fn->filename == NULL ? "?" : fn->filename->chars;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "  %s() [%s:%d]\n", name, file, line);
    }
    g_err_stack = obj_copy_string(buf, off);
    sync_err_detected_global();
}

/* ================================================================
 * Accesseurs internes ( jalon B) — exposes pour tests
 * internes owl_errors_native_test.c qui doivent assert sur l'etat
 * apres owl_emit_native_error(). Pas d'usage user-facing.
 * ================================================================ */
const char* owl_err_get_code(void) {
    return g_err_code;
}

const char* owl_err_get_message(void) {
    return g_err_message != NULL ? g_err_message->chars : NULL;
}

const char* owl_err_get_native_name(void) {
    return g_err_native_name != NULL ? g_err_native_name->chars : NULL;
}

bool owl_err_get_fresh(void) {
    return g_err_fresh;
}

/* Etat courant du flag soft-error. Utilise par les coercions de declaration
 * (parse_to_tenseur/vecteur/matrice) pour distinguer « l'expression source a
 * deja leve une erreur » (a propager) d'un « vrai type non-coercible » (ou le
 * message generique de la coercion est legitime). Cf. . */
bool owl_err_is_detected(void) {
    return g_err_detected;
}

/* Clear the soft-error FLAG only. The error details (message, code, line,
 * procedure, filename, stack, timestamp) survive intentionally so that
 * `ErreurInfo()` and `ErreurPropage()` keep working after the dispatch
 * auto-RAZ kicks in. Canon alignement : reading ErreurInfo() RAZ le flag
 * mais conserve l'info ; `ErreurPropage()` peut re-promouvoir l'erreur tant
 * que le message reste en mémoire. Les détails ne sont écrasés que par le
 * prochain `owl_set_error()`. Cf. ticket  + .
 *
 * Sentinelle `g_err_fresh` : si une erreur vient d'être posée,
 * on consomme la sentinelle au lieu de RAZ le flag. Protège contre les
 * OP_CALL intercalés (notamment les coercitions __XxxCoerce générées par
 * le compilo pour `c est une chaîne = fNative(...)`). */
void owl_clear_error(void) {
    if (g_err_fresh) {
        g_err_fresh = false;
        return;
    }
    g_err_detected = false;
    sync_err_detected_global();
}

/* Clear FLAG + INFO. Reserved for the user-explicit `ErreurRAZ()` call —
 * canon strict ("Le résultat de la fonction ErreurInfo est
 * réinitialisé"). Le dispatch
 * auto continue d'utiliser `owl_clear_error()` (flag-only) pour préserver
 * l'info entre l'appel qui échoue et le `Si ErreurDetectee` qui suit.
 * Cf. ticket  arbitrage doctrinal. */
void owl_clear_error_full(void) {
    g_err_detected     = false;
    g_err_fresh        = false;   /* user reset = wipe la sentinelle aussi */
    g_err_code         = NULL;    /*  : migré int -> const char* */
    g_err_message      = NULL;
    g_err_native_name  = NULL;    /*  : nouveau champ */
    g_err_line         = 0;
    g_err_procedure    = NULL;
    g_err_filename     = NULL;
    g_err_stack        = NULL;
    g_err_timestamp_ms = 0;
    sync_err_detected_global();
}

/* ================================================================
 * CONSTANTES errXxx (même IDs que pour ExceptionInfo)
 * Sentinelles centralisees dans sentinels.h. Sources externes (vm.c
 * pour ExceptionInfo) doivent aussi inclure sentinels.h.
 * ================================================================ */
#include "sentinels.h"

static const char* const E_errInfo_values[] = {
    "errMessage", "errCode", "errLigne",   "errComplet",
    "errTraitement", "errPendant", "errPile", "errR\xc3\xa9sum\xc3\xa9",
    "errDateHeure", "errElement", "errNombreSousErreur",
    "errInfo", "errCodeSyst\xc3\xa8me",
    "errMessageSyst\xc3\xa8me", "errFonction",
    "errCodeNum",
    NULL
};
const OwlEnum E_errInfo = {
    .name="errInfo", .nameEN="errInfo", .category="Erreur",
    .descFR="Composants d'information retournés par ErreurInfo / ExceptionInfo.",
    .descEN="Information components returned by ErreurInfo / ExceptionInfo.",
    .values=E_errInfo_values, .combinable=false
};

static const OwlConstDef ERREUR_CONSTS[] = {
    {"errMessage",          "Erreur", INTEGER_VAL(OWL_ERR_MESSAGE),     "message complet de l'exception (défaut)",        "full exception message (default)", "errMessage"},
    {"errCode",             "Erreur", INTEGER_VAL(OWL_ERR_CODE),        "code de l'erreur ou de l'exception, sous forme de chaîne : symbole canonique (style E_NATIVE_...) quand une native est en cause, valeur brute (ex. \"42\") quand elle a été fournie explicitement", "error or exception code, as a string: canonical symbol (like E_NATIVE_...) when a native is involved, raw value (e.g. \"42\") when supplied explicitly", "errCode"},
    {"errLigne",            "Erreur", INTEGER_VAL(OWL_ERR_LIGNE),       "ligne source de la levée",                       "source line of the throw", "errLine"},
    {"errComplet",          "Erreur", INTEGER_VAL(OWL_ERR_COMPLET),     "rapport complet (message, code, fichier, ligne, traitement, pile)", "full report (message, code, file, line, handler, stack)", "errFullDetails"},
    {"errTraitement",       "Erreur", INTEGER_VAL(OWL_ERR_TRAITEMENT),  "nom de la procédure courante au moment de la levée", "name of the current procedure at throw time", "errProcess"},
    {"errPendant",          "Erreur", INTEGER_VAL(OWL_ERR_PENDANT),     "nom de la fonction qui a réellement déclenché l'exception", "name of the function that actually raised the exception", "errDuring"},
    {"errPile",             "Erreur", INTEGER_VAL(OWL_ERR_PILE),        "pile d'appels formatée (multi-ligne)",           "formatted call stack (multi-line)", "errStack"},
    {"errR\xc3\xa9sum\xc3\xa9", "Erreur", INTEGER_VAL(OWL_ERR_RESUME),  "résumé court (première ligne du message, max 80 caractères)", "short summary (first line of message, 80 char max)", "errSummary"},
    {"errDateHeure",        "Erreur", INTEGER_VAL(OWL_ERR_DATE_HEURE),  "horodatage Unix milliseconde de la levée",       "Unix milliseconds timestamp at throw", "errDateTime"},
    {"errElement",          "Erreur", INTEGER_VAL(OWL_ERR_ELEMENT),     "nom du fichier source où la levée a eu lieu",    "source file name where the throw occurred", "errElement"},
    {"errNombreSousErreur", "Erreur", INTEGER_VAL(OWL_ERR_NB_SOUS_ERR), "nombre de sous-erreurs (toujours 0 en l'absence de mécanisme de sous-erreur)", "sub-error count (always 0 with no sub-error mechanism)", "errNumberSubError"},
    {"errInfo",             "Erreur", INTEGER_VAL(OWL_ERR_INFO),        "informations complémentaires fournies à la levée (3e argument d'ExceptionDéclenche, vide sinon)", "additional information passed at throw (3rd argument of ExceptionThrow, empty otherwise)", "errInfo"},
    {"errCodeSyst\xc3\xa8me",   "Erreur", INTEGER_VAL(OWL_ERR_CODE_SYSTEME),    "code d'erreur système si renseigné par la native qui a levé, 0 sinon", "system error code if set by the raising native, 0 otherwise", "errSystemCode"},
    {"errMessageSyst\xc3\xa8me","Erreur", INTEGER_VAL(OWL_ERR_MESSAGE_SYSTEME), "message d'erreur système si renseigné, chaîne vide sinon",             "system error message if set, empty string otherwise", "errSystemMessage"},
    {"errFonction",         "Erreur", INTEGER_VAL(OWL_ERR_FONCTION),    "nom de la fonction native qui a posé l'erreur", "name of the native function that raised the error", "errFunction"},
    {"errCodeNum",          "Erreur", INTEGER_VAL(OWL_ERR_CODE_NUM),    "alias numérique court du code d'erreur (ex. \"E0042\"), vide si le code n'est pas un symbole du registre", "short numeric alias of the error code (e.g. \"E0042\"), empty if the code is not a registry symbol", "errNumericCode"},
};

static const OwlEnum* const ERREUR_ENUMS[] = { &E_errInfo };

/* ================================================================
 * NATIVES
 * ================================================================ */

/* ErreurInfo([typeInfo]) */
static Value fn_erreur_info(int argCount, Value* args) {
    int kind = OWL_ERR_MESSAGE;
    if (argCount >= 1 && IS_INTEGER(args[0])) kind = (int)AS_INTEGER(args[0]);

    switch (kind) {
        case OWL_ERR_MESSAGE:
            return g_err_message != NULL ? OBJ_VAL(g_err_message)
                                         : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_CODE:
            /*  : migré int -> string canonique ("E_NATIVE_ARG_INVALID", etc.) */
            return g_err_code != NULL ? OBJ_VAL(obj_copy_string(g_err_code, (int)strlen(g_err_code)))
                                      : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_LIGNE:
            return INTEGER_VAL(g_err_line);
        case OWL_ERR_TRAITEMENT:
        case OWL_ERR_PENDANT:
        case OWL_ERR_ELEMENT:
            return g_err_procedure != NULL ? OBJ_VAL(g_err_procedure)
                                           : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_PILE:
            return g_err_stack != NULL ? OBJ_VAL(g_err_stack)
                                       : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_DATE_HEURE:
            return INTEGER_VAL(g_err_timestamp_ms);
        case OWL_ERR_NB_SOUS_ERR:
            return INTEGER_VAL(0);
        case OWL_ERR_INFO:
            /* errInfo = informations complémentaires. En OWL = même que errMessage. */
            return g_err_message != NULL ? OBJ_VAL(g_err_message)
                                         : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_CODE_SYSTEME:
            /* En OWL pas d'erreur système séparée → 0. */
            return INTEGER_VAL(0);
        case OWL_ERR_MESSAGE_SYSTEME:
            /* En OWL pas d'erreur système séparée → "". */
            return OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_FONCTION:
            /*  : nom de la native C qui a posé l'erreur */
            return g_err_native_name != NULL ? OBJ_VAL(g_err_native_name)
                                             : OBJ_VAL(obj_copy_string("", 0));
        case OWL_ERR_CODE_NUM: {
            /*  : alias numerique du code. g_err_code stocke le nom
             * symbolique (ou une valeur user brute "42" via ErreurDeclenche) ;
             * on resout vers la def du registre puis son numero E0NNN/W0NNN.
             * Code non resoluble (valeur user libre) -> chaine vide. */
            const char* num = "";
            if (g_err_code != NULL) {
                const OwlErrorDef* d = owl_registry_error_find(g_err_code);
                if (d) {
                    const char* n = owl_registry_error_num(
                        owl_registry_error_code_of(d));
                    if (n) num = n;
                }
            }
            return OBJ_VAL(obj_copy_string(num, (int)strlen(num)));
        }
        case OWL_ERR_RESUME: {
            const char* m = g_err_message != NULL ? g_err_message->chars : "";
            return OBJ_VAL(obj_copy_string(m, (int)strlen(m)));
        }
        case OWL_ERR_COMPLET: {
            char buf[3072];
            const char* msg = g_err_message != NULL ? g_err_message->chars : "";
            const char* code = g_err_code != NULL ? g_err_code : "";
            const char* file = g_err_filename != NULL ? g_err_filename->chars : "?";
            const char* proc = g_err_procedure != NULL ? g_err_procedure->chars : "<script>";
            const char* stack = g_err_stack != NULL ? g_err_stack->chars : "";
            const char* native = g_err_native_name != NULL ? g_err_native_name->chars : "";
            int n = snprintf(buf, sizeof(buf),
                "Message: %s\nCode: %s\nNative: %s\nFichier: %s\nLigne: %d\nTraitement: %s\n\nPile:\n%s",
                msg, code, native, file, g_err_line, proc, stack);
            return OBJ_VAL(obj_copy_string(buf, n));
        }
    }
    return OBJ_VAL(obj_copy_string("", 0));
}

/* ErreurDeclenche(num [, msg]) — soft throw. Doesn't unwind; caller must
 * test ErreurDetectee.
 *
 * Cas particulier dans le chantier registry erreurs : la native
 * stocke le `code` int passe par l'utilisateur sous forme de chaine
 * numerique brute ("42") dans g_err_code, pour preserver le contrat
 * `ErreurInfo(errCode)` qui retourne la valeur user-explicit, pas un code
 * symbole canonique E_NATIVE_*. Le buffer static survit jusqu'au prochain
 * appel a ErreurDeclenche, ce qui suffit puisque g_err_code est lu de
 * maniere synchrone via ErreurInfo. */
static Value fn_erreur_declenche(int argCount, Value* args) {
    const char* msg = "Erreur";
    int code = 0;
    if (argCount >= 1) {
        if (IS_STRING(args[0])) {
            msg = AS_CSTRING(args[0]);
        } else if (IS_INTEGER(args[0])) {
            code = (int)AS_INTEGER(args[0]);
            if (argCount >= 2 && IS_STRING(args[1])) msg = AS_CSTRING(args[1]);
        }
    }
    static char code_buf[32];
    snprintf(code_buf, sizeof(code_buf), "%d", code);
    owl_set_error_native(code_buf, "ErreurD\xc3\xa9" "clenche", msg);
    return NIL_VAL;
}

/* ErreurRAZ() — user-explicit full reset.
 * Canon strict : remet à zéro le flag ET l'info. Diffère
 * du RAZ auto dispatch qui, lui, ne clear que le flag pour permettre à
 * ErreurInfo()/ErreurPropage() de fonctionner après-coup. Cf. . */
static Value fn_erreur_raz(int argCount, Value* args) {
    (void)argCount; (void)args;
    owl_clear_error_full();
    return NIL_VAL;
}

/* ErreurPropage() — promote the soft error to an exception.
 * Avec RAZ-auto dispatch : le flag `g_err_detected` est souvent
 * Faux au moment où ErreurPropage est appelé (RAZ par OP_CALL avant), mais
 * l'info reste mémorisée tant qu'aucune nouvelle erreur ne l'a écrasée.
 * Critère : `g_err_message != NULL` (info encore présente). */
static Value fn_erreur_propage(int argCount, Value* args) {
    (void)argCount; (void)args;
    if (g_err_message != NULL) {
        /* Promotion soft-error -> exception : on transporte le code canonique
         * (chaine, "42" via ErreurDeclenche ou "E_NATIVE_*" via une native) ET
         * le nom de la native d'origine (errFonction). ExceptionInfo converge
         * ainsi avec ErreurInfo, plus de perte de code (, item reporte
         * d'). */
        vm_throw_from_erreur_coded(
            g_err_message->chars,
            g_err_code,
            g_err_native_name != NULL ? g_err_native_name->chars : NULL);
    }
    return NIL_VAL;
}

/* ErreurConstruit(<format> [, <p1>, <p2>, ...]) — substitue les marqueurs
 * %1, %2, …, %n dans <format> par les paramètres correspondants.
 * Doc 1000018878 :
 *   - %% pour % littéral
 *   - paramètres manquants → chaînes vides
 *   - les paramètres en surplus sont ignorés */
static Value fn_erreur_construit(int argCount, Value* args) {
    if (argCount == 0) return OBJ_VAL(obj_copy_string("", 0));
    if (!IS_STRING(args[0])) return args[0];
    ObjString* fmt = AS_STRING(args[0]);
    char buf[4096];
    int n = 0;
    int i = 0;
    while (i < fmt->length && n < (int)sizeof(buf) - 1) {
        if (fmt->chars[i] == '%' && i + 1 < fmt->length) {
            if (fmt->chars[i+1] == '%') {
                buf[n++] = '%';
                i += 2;
                continue;
            }
            int j = i + 1;
            int num = 0;
            while (j < fmt->length && fmt->chars[j] >= '0' && fmt->chars[j] <= '9') {
                num = num * 10 + (fmt->chars[j] - '0');
                j++;
            }
            if (num >= 1 && j > i + 1) {
                /* Substitue par args[num] (ou chaîne vide si manquant) */
                if (num < argCount) {
                    ObjString* sub = value_to_string(args[num]);
                    if (sub != NULL) {
                        int take = sub->length;
                        if (take > (int)sizeof(buf) - n - 1) take = (int)sizeof(buf) - n - 1;
                        memcpy(buf + n, sub->chars, (size_t)take);
                        n += take;
                    }
                }
                i = j;
                continue;
            }
        }
        buf[n++] = fmt->chars[i++];
    }
    return OBJ_VAL(obj_copy_string(buf, n));
}

/* ================================================================
 * REGISTRATION
 * ================================================================ */

/* ================================================================
 * Registry declarations — Erreur family (canonical names).
 * ================================================================ */

static const OwlParam p_msg[]    = { {"message", "tout", "format avec marqueurs %1, %2, …"}, {0} };

/* ErreurInfo — formes discrètes. 0 arg (errMessage par défaut),
 * 1 arg (typeInfo), 2 args (typeInfo + indice de sous-erreur). L'impl ne lit
 * jamais au-delà de args[0] : l'indice de sous-erreur est accepté pour la
 * conformité de signature mais ignoré (pas de mécanisme de sous-erreur,
 * errNombreSousErreur vaut toujours 0). */
static const OwlParam p_info_type[]   = { {"typeInfo", "entier", "composant d'info demandé", .allowedBase=&E_errInfo}, {0} };
static const OwlParam p_info_type_ix[] = { {"typeInfo", "entier", "composant d'info demandé", .allowedBase=&E_errInfo},
                                           {"indice",   "entier", "indice de la sous-erreur"}, {0} };
static const OwlSignature sig_info[]   = {
    {0, NULL,           "chaîne|entier", "info demandée (errMessage par défaut)"},
    {1, p_info_type,    "chaîne|entier", "info demandée (chaîne pour Message/Pile/Code, entier pour Ligne/DateHeure...)"},
    {2, p_info_type_ix, "chaîne|entier", "info demandée pour la sous-erreur d'indice donné"},
    {0}
};
/* ErreurDéclenche — formes discrètes. 0 arg (code 0 + message
 * "Erreur" par défaut), 1 arg (code entier OU message seul), 2 args (code +
 * message). L'impl lit args[0] et args[1] (si présent), jamais au-delà.
 * La forme 0 arg est une tolérance OWL (la doc exige le numéro) : code
 * vaut 0 et le message vaut "Erreur" — contrat verrouillé côté runtime. */
static const OwlParam p_trig_1[]   = { {"code", "entier|chaîne", "code d'erreur (entier) ou message (chaîne)"}, {0} };
static const OwlParam p_trig_2[]   = { {"code", "entier", "code d'erreur"},
                                       {"message", "chaîne", "message associé"}, {0} };
static const OwlSignature sig_trig[]   = {
    {0, NULL,     "rien", "déclenche l'état d'erreur soft (code 0, message par défaut)"},
    {1, p_trig_1, "rien", "déclenche l'état d'erreur soft"},
    {2, p_trig_2, "rien", "déclenche l'état d'erreur soft avec message"},
    {0}
};
static const OwlSignature sig_raz[]    = { {0,  NULL,   "rien",    "remet le contexte d'erreur à zéro"}, {0} };
static const OwlSignature sig_prop[]   = { {0,  NULL,   "rien",    "promeut l'erreur soft en exception"}, {0} };
static const OwlSignature sig_build[]  = { {-1, p_msg,  "chaîne",  "message avec marqueurs substitués"}, {0} };

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "ErreurInfo",
        .nameEN     = "ErrorInfo",
        .category   = "Erreur",
        .descFR     = "Retourne le détail de la dernière erreur détectée.",
        .descEN     = "Return details of the last detected error.",
        .signatures = sig_info,
        .impl       = fn_erreur_info,
    },
    {
        .canonical      = "ErreurD\xc3\xa9" "clenche",   /* ErreurDéclenche */
        .canonicalAscii = "ErreurDeclenche",
        .nameEN         = "ErrorThrow",
        .category       = "Erreur",
        .descFR         = "Déclenche une erreur soft (ErreurDetectee devient Vrai).",
        .descEN         = "Trigger a soft error (ErreurDetectee becomes True).",
        .signatures     = sig_trig,
        .impl           = fn_erreur_declenche,
    },
    {
        .canonical  = "ErreurRAZ",
        .nameEN     = "ErrorReset",
        .category   = "Erreur",
        .descFR     = "Remet à zéro le contexte d'erreur (ErreurDetectee = Faux).",
        .descEN     = "Reset the error context (ErreurDetectee = False).",
        .signatures = sig_raz,
        .impl       = fn_erreur_raz,
    },
    {
        .canonical  = "ErreurPropage",
        .nameEN     = "ErrorPropagate",
        .category   = "Erreur",
        .descFR     = "Promeut l'erreur soft courante en exception.",
        .descEN     = "Promote the current soft error to an exception.",
        .signatures = sig_prop,
        .impl       = fn_erreur_propage,
    },
    {
        .canonical  = "ErreurConstruit",
        .nameEN     = "ErrorBuild",
        .category   = "Erreur",
        .descFR     = "Construit un message en substituant les marqueurs %1, %2, … par les arguments fournis.",
        .descEN     = "Build a message by substituting the %1, %2, … markers with the given arguments.",
        .signatures = sig_build,
        .impl       = fn_erreur_construit,
    },
};

void stdlib_erreur_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums (ERREUR_ENUMS,
        (int)(sizeof(ERREUR_ENUMS)/sizeof(ERREUR_ENUMS[0])));
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * pour les noms accentues (errRésumé -> errResume, errCodeSystème ->
     * errCodeSysteme, etc.). */
    owl_registry_register_consts(ERREUR_CONSTS,
        (int)(sizeof(ERREUR_CONSTS)/sizeof(ERREUR_CONSTS[0])));

    /* Les formes EN des composants errXxx sont les nameEN portés par les
     * OwlConstDef (errLine, errFullDetails, errProcess, errStack, errSummary,
     * errDateTime, errNumberSubError, errSystemCode, errSystemMessage,
     * errFunction) : register_consts les enregistre déjà comme alias du
     * sentinel FR. Aucun register_ascii_alias supplémentaire nécessaire. */

    /* Enregistre la variable globale prédéfinie ErreurDétectée dans le
     * catalogue (consommé par DAP / CLI doc / self-test) et initialise
     * ses 3 noms à Faux dans vm.globals. */
    owl_globalvar_catalog_register(&ERR_DETECTED_DEF);
    sync_err_detected_global();

    /*  : enregistre les 5 globals contexte d'erreur au registre
     * unique des racines persistantes. Remplace l'ancienne fonction
     * stdlib_erreur_mark_roots() supprimee. */
    owl_register_persistent_obj_root((Obj**)&g_err_message);
    owl_register_persistent_obj_root((Obj**)&g_err_native_name);
    owl_register_persistent_obj_root((Obj**)&g_err_procedure);
    owl_register_persistent_obj_root((Obj**)&g_err_filename);
    owl_register_persistent_obj_root((Obj**)&g_err_stack);
}

void stdlib_erreur_reset_globals(void) {
    g_err_message     = NULL;
    g_err_native_name = NULL;
    g_err_procedure   = NULL;
    g_err_filename    = NULL;
    g_err_stack       = NULL;
}
