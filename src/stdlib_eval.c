// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * Dynamic compilation + evaluation (Tier 2):
 *   Compile(source)            -> Procedure (compiled function)
 *   Compile(name, source)      -> Procedure, also registers as global named
 *   ExécuteCode(source)        -> runs source as a script, returns RENVOYER value
 *   EvalueExpression(expr)     -> evaluates an expression, returns result
 *
 * All three paths build a throw-away script function via the normal compiler,
 * then either return it (Compile) or execute it re-entrantly (ExécuteCode /
 * EvalueExpression) using vm_call_reentrant.
 *
 * The compiler captures any diagnostics into a static buffer so we can
 * surface a runtime error with the real compile message when dynamic code
 * fails to compile.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "sentinels.h"
#include "vm.h"
#include "compiler.h"
#include "owl_errors.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Wrap a raw source string so it behaves like a self-contained script.
 * For Compile(name, code) we keep the code as-is (it's a procedure body).
 * For ExécuteCode we also keep it as-is.
 * For EvalueExpression we wrap: "RENVOYER (<expr>)" to get a result.
 */

static InterpretResult dynamic_run(ObjFunction* fn, Value* outResult) {
    /* Push the function and invoke via vm_call_reentrant (0 args). */
    vm_push(OBJ_VAL(fn));
    InterpretResult r = vm_call_reentrant(OBJ_VAL(fn), 0);
    if (r == INTERPRET_OK) {
        *outResult = vm_pop();
    } else {
        *outResult = NIL_VAL;
    }
    return r;
}

/* Compile(source) — returns the compiled function as a callable value.
 * Compile(name, source) — also registers it as a global under `name`. */
/* Compile/eval helpers silently capture parser errors so they don't leak
 * to stderr when user code is probing for syntactic validity. The buffer
 * is discarded; the error-set API (owl_set_error) is what user code
 * inspects via ErreurDetectee / ErreurInfo. */
static void eval_silence_errors(CompileDiagnostic* buf, int* count) {
    compile_set_error_capture(buf, 16, count);
}
static void eval_restore_errors(void) {
    compile_set_error_capture(NULL, 0, NULL);
}

/* Compile — doc 3013015.
 *
 * OWL DIVERGENCE ASSUMÉE (docs/VISION.md §5bis):
 * La doc spécifie pour la forme `Compile(nom, source)` un retour CHAÎNE
 * mixte ("" OK / "ERR" grave / message d'erreur). C'est archaïque (mélange
 * type de retour + canal d'erreur) et incohérent avec le reste de l'API canon
 * moderne qui utilise `ErreurDétectée` + `ErreurInfo()`.
 *
 * OWL retourne toujours la Procédure compilée (ObjFunction), ou NIL en cas
 * d'erreur. Le canal d'erreur est `ErreurDétectée`/`ErreurInfo()`, uniforme
 * avec le reste des natives OWL.
 *
 * Signatures supportées:
 *   Compile(nom, source)  → Procédure (enregistrée aussi comme global nommé)
 *   Compile(source)       → Procédure (anonyme)
 */
/* Compile([nom, ] source) — doc 3013015.
 *
 * Forme 2-args (nom, source) : retourne "" sur succès, le message d'erreur
 * sur échec. Procédure enregistrée comme globale callable par nom (via
 * ExécuteTraitement ou appel direct).
 *
 * Forme 1-arg (source) : retourne directement l'ObjFunction (procédure
 * anonyme) — pour assignation à une variable de type Procédure. */
static Value fn_compile(int argCount, Value* args) {
    const char* source = NULL;
    ObjString*  nameStr = NULL;

    if (argCount == 1 && IS_STRING(args[0])) {
        source = AS_CSTRING(args[0]);
    } else if (argCount >= 2 && IS_STRING(args[0]) && IS_STRING(args[1])) {
        nameStr = AS_STRING(args[0]);
        source  = AS_CSTRING(args[1]);
    } else {
        return NIL_VAL;
    }

    CompileDiagnostic buf[16]; int count = 0;
    eval_silence_errors(buf, &count);
    ObjFunction* fn = compile_as_function(source);
    eval_restore_errors();

    if (fn == NULL) {
        const char* m = count > 0 ? buf[0].message : "erreur de compilation";
        owl_emit_native_error(E_NATIVE_COMPILE_ERROR, "Compile",
                              "native", "Compile",
                              "detail", m,
                              NULL);
        /* Forme 2-args : retourner le message d'erreur en string (canon).
         * Forme 1-arg : conserve la sémantique anonyme → NIL_VAL. */
        if (nameStr != NULL) {
            return OBJ_VAL(obj_copy_string(m, (int)strlen(m)));
        }
        return NIL_VAL;
    }

    owl_clear_error();
    if (nameStr != NULL) {
        /* Enregistre comme globale callable, retourne "" canon.
         *  : delegue au canon obj_globals_set_kv. */
        obj_globals_set_kv(nameStr, OBJ_VAL(fn));
        return OBJ_VAL(obj_copy_string("", 0));
    }
    /* Forme anonyme : retourne la fonction directement. */
    return OBJ_VAL(fn);
}

/* ExécuteCode(code) — runs the code string. Returns the
 * RENVOYER value if present, else Null. */
static Value fn_execute_code(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;

    CompileDiagnostic buf[16]; int count = 0;
    eval_silence_errors(buf, &count);
    ObjFunction* fn = compile_as_function(AS_CSTRING(args[0]));
    eval_restore_errors();

    if (fn == NULL) {
        const char* m = count > 0 ? buf[0].message : "erreur de compilation";
        owl_emit_native_error(E_NATIVE_COMPILE_ERROR, "ExecuteCode",
                              "native", "ExecuteCode",
                              "detail", m,
                              NULL);
        return NIL_VAL;
    }

    Value result = NIL_VAL;
    dynamic_run(fn, &result);
    owl_clear_error();
    return result;
}

/* ExécuteTraitement(nomProcédure, trtProcédure [, args...]).
 * Scope OWL: on implémente uniquement la forme "Exécuter une procédure"
 * (SYNTAXE 5 de la doc). Les autres formes (champs UI, fenêtres, menus, états)
 * sont hors scope car OWL n'a pas ces objets IDE.
 *
 * Constantes supportées: trtProcédure (local/courant) et trtProcédureProjet.
 * Sémantique OWL: les deux constantes résolvent vers un simple appel par nom
 * (pas de distinction local vs projet).
 *
 * Retourne: la valeur de retour de la procédure (RENVOYER), ou Null. */
static Value fn_execute_traitement(int argCount, Value* args) {
    if (argCount < 2) return NIL_VAL;
    if (!IS_STRING(args[0])) return NIL_VAL;
    if (!IS_INTEGER(args[1])) return NIL_VAL;

    ObjString* procName = AS_STRING(args[0]);
    /* On ignore la valeur exacte de la constante — pour OWL toutes les
     * constantes trt* procédure mènent au même dispatch. */

    /* Look up the global. Must be a callable (ObjFunction/ObjClosure/Native). */
    Value callee;
    if (!table_get(&vm.globals, procName, &callee)) {
        owl_emit_native_error(E_NATIVE_NOT_FOUND, "ExecuteTraitement",
                              "native", "ExecuteTraitement",
                              "name", procName->chars,
                              NULL);
        return NIL_VAL;
    }

    /* Push callee + forwarded args on the stack */
    vm_push(callee);
    int fwdCount = argCount - 2;
    for (int i = 0; i < fwdCount; i++) {
        vm_push(args[2 + i]);
    }

    InterpretResult r = vm_call_reentrant(callee, fwdCount);
    Value result = (r == INTERPRET_OK) ? vm_pop() : NIL_VAL;
    /* Pop the callee that was pushed (vm_call_reentrant leaves stack balanced
     * with just the result on top if OK; it's already popped above) */
    if (r == INTERPRET_OK) {
        owl_clear_error();
    }
    return result;
}

/* EvalueExpression(expr) — evaluate an expression, return the result.
 * Implemented by wrapping the expression in "RENVOYER (<expr>)\n". */
static Value fn_evalue_expression(int argCount, Value* args) {
    (void)argCount;
    if (!IS_STRING(args[0])) return NIL_VAL;

    ObjString* exprStr = AS_STRING(args[0]);
    /* Build "RENVOYER (<expr>)\n" in a temporary buffer. */
    int wrapLen = exprStr->length + 32;
    char* wrapped = (char*)malloc((size_t)wrapLen);
    if (wrapped == NULL) return NIL_VAL;
    /* No outer parens: "RENVOYER (expr)" would be parsed as multi-return
     * syntax by the compiler. Keep the expression bare. */
    snprintf(wrapped, (size_t)wrapLen, "RENVOYER %.*s\n",
             exprStr->length, exprStr->chars);

    CompileDiagnostic buf[16]; int count = 0;
    eval_silence_errors(buf, &count);
    ObjFunction* fn = compile_as_function(wrapped);
    eval_restore_errors();
    free(wrapped);
    if (fn == NULL) {
        const char* m = count > 0 ? buf[0].message : "expression invalide";
        owl_emit_native_error(E_NATIVE_COMPILE_ERROR, "EvalueExpression",
                              "native", "EvalueExpression",
                              "detail", m,
                              NULL);
        return NIL_VAL;
    }

    Value result = NIL_VAL;
    dynamic_run(fn, &result);
    owl_clear_error();
    return result;
}

/* ================================================================
 * Registry declarations — Eval family (Tier 2).
 * ================================================================ */

/* Enum traitementCible : les cibles trt* acceptees par le parametre `trt`
 * de ExecuteTraitement. Valeurs en zone sentinel OWL_TRT_BASE (negatives),
 * donc force_small_int reste faux (rule 20 satisfaite par defaut). Cibles
 * mutuellement exclusives -> combinable=false. Les noms doivent matcher
 * exactement les canon de TRT_CONSTS (rule 5). */
static const char* const E_traitementCible_values[] = {
    "trtProc\xc3\xa9""dure", "trtProc\xc3\xa9""dureProjet", NULL
};
static const OwlEnum E_traitementCible = {
    .name="traitementCible", .nameAscii="traitementCible", .nameEN="processTarget",
    .category="Eval",
    .descFR="Cible d'exécution de ExécuteTraitement : la procédure désignée, ou la procédure désignée au niveau du projet.",
    .descEN="Execution target of ExecuteProcess: the designated procedure, or the designated project-level procedure.",
    .values=E_traitementCible_values, .combinable=false
};
static const OwlEnum* const EVAL_ENUMS[] = { &E_traitementCible };

static const OwlParam p_src[]     = { {"source", "chaîne", "code source"}, {0} };
static const OwlParam p_name_src[]= { {"nom",    "chaîne", "nom global sous lequel enregistrer la fonction"},
                                      {"source", "chaîne", "code source"}, {0} };
static const OwlParam p_expr[]    = { {"expression", "chaîne", "expression à évaluer"}, {0} };

static const OwlSignature sig_compile[] = {
    {1, p_src,      "Procédure", "fonction compilée"},
    {2, p_name_src, "Procédure", "fonction compilée, enregistrée comme globale"},
    {0}
};
static const OwlSignature sig_exec[]    = { {1, p_src,  "tout", "valeur renvoyée via RENVOYER, ou Null"}, {0} };
static const OwlSignature sig_evalue[]  = { {1, p_expr, "tout", "résultat de l'expression (type selon expression)"}, {0} };
static const OwlParam p_et[] = { {"procédure", "chaîne", "nom de la procédure"},
                                 {"trt", "entier", "constante trtProcédure ou trtProcédureProjet", .allowedBase=&E_traitementCible},
                                 {0} };
static const OwlSignature sig_et[] = { {-1, p_et, "tout", "valeur de retour de la procédure (type selon procédure)"}, {0} };

static const OwlNativeDef DEFS[] = {
    {
        .canonical  = "Compile",
        .nameEN     = "Compile",
        .category   = "Eval",
        .descFR     = "Compile une chaîne de code source en une procédure appelable.",
        .descEN     = "Compile a source string into a callable procedure.",
        .signatures = sig_compile,
        .impl       = fn_compile,
    },
    {
        .canonical      = "Ex\xc3\xa9""cuteCode",  /* ExécuteCode */
        .canonicalAscii = "ExecuteCode",
        .nameEN         = "ExecuteCode",
        .category       = "Eval",
        .descFR         = "Compile et exécute une chaîne de code source.",
        .descEN         = "Compile and run a source string.",
        .signatures     = sig_exec,
        .impl           = fn_execute_code,
    },
    {
        .canonical      = "Ex\xc3\xa9""cuteTraitement",
        .canonicalAscii = "ExecuteTraitement",
        .nameEN         = "ExecuteProcess",
        .category       = "Eval",
        .descFR         = "Exécute une procédure par son nom (OWL: seule la forme procédure est supportée).",
        .descEN         = "Execute a procedure by name (OWL: only the procedure form is supported).",
        .signatures     = sig_et,
        .impl           = fn_execute_traitement,
    },
    {
        .canonical  = "EvalueExpression",
        .nameEN     = "EvaluateExpression",
        .category   = "Eval",
        .descFR     = "Évalue une expression et retourne le résultat.",
        .descEN     = "Evaluate an expression and return its value.",
        .signatures = sig_evalue,
        .impl       = fn_evalue_expression,
    },
};

void stdlib_eval_init(void) {
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums(EVAL_ENUMS,
        (int)(sizeof(EVAL_ENUMS)/sizeof(EVAL_ENUMS[0])));

    /* Constantes trt* pour ExécuteTraitement.
     * OWL ne supporte que la forme "Exécuter une procédure", donc
     * seules trtProcédure et trtProcédureProjet sont exposées. */
    #define C_TRT(nm, val, dfr, den, en) {nm, "Eval", INTEGER_VAL(val), dfr, den, en}
    static const OwlConstDef TRT_CONSTS[] = {
        C_TRT("trtProc\xc3\xa9""dure",      OWL_TRT_PROCEDURE,
              "Exécute la procédure désignée.",
              "Execute the designated procedure.", "trtProcedure"),
        C_TRT("trtProc\xc3\xa9""dureProjet", OWL_TRT_PROCEDURE_PROJET,
              "Exécute la procédure désignée du projet.",
              "Execute the designated project procedure.", "trtProjectProcedure"),
    };
    #undef C_TRT
    /* register_consts auto-enregistre dans vm.globals + auto-alias ASCII
     * (trtProcedure pour trtProcédure, trtProcedureProjet pour trtProcédureProjet). */
    owl_registry_register_consts(TRT_CONSTS, (int)(sizeof(TRT_CONSTS)/sizeof(TRT_CONSTS[0])));
}
