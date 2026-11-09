// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_log.c — Sous-systeme de log applicatif a niveaux.
 *
 * Divergence assumee (cf ) : le WL canon n'offre AUCUN logger
 * applicatif a niveaux. `Log` = logarithme decimal, `dbg*Log` = trace
 * d'execution IDE (fichiers binaires proprietaires), `HJournal*` = journal
 * de transactions BDD. OWL fournit ici un logger de bibliotheque moderne
 * (niveaux facon SLF4J/tracing, un point d'extension facon Go slog).
 *
 * Deux surfaces, meme comportement (invariant doubles type-vs-fonction) :
 *   - type `Logger` declarable : `monLog est un Logger`, configure par
 *     proprietes `..Niveau` (seuil), `..Formateur` (procedure record->chaine),
 *     `..Destination` (stdout par defaut / chemin fichier), `..Emetteur`
 *     (procedure d'emission custom). Methodes `.Debug/.Info/.Avert/.Erreur`
 *     (EN `.Warn/.Error`).
 *   - natives statiques `LoggerDebug/LoggerInfo/LoggerAvert/LoggerErreur`
 *     + `LoggerParametre` : operent sur un logger par defaut
 *     implicite (zero declaration), stocke dans vm.globals sous "__LoggerDefaut".
 *
 * Chaque emission qui franchit le seuil construit un record `LogEnreg`
 * (metadonnees decomposees : horodatage, niveau, message) passe au formateur
 * (procedure-valeur invoquee via vm_call_reentrant) et a l'emetteur. Format,
 * destination et emission custom sont trois leviers independants.
 *
 * V1 : horodatage + niveau + message. Differe en V2 : call-site (ligne/
 * procedure), paires cle/valeur structurees, formateur JSON de confort.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "owl_methods.h"
#include "vm.h"
#include "sentinels.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern InterpretResult vm_call_reentrant(Value callee, int argCount);
extern void  vm_push(Value v);
extern Value vm_pop(void);

/* ================================================================
 * Niveaux de log — enum logNiveau / logLevel
 *
 * Valeurs en zone sentinelle (offset 108 libre, cf sentinels.h convention) :
 * croissantes du moins au plus severe. L'ordre numerique EST l'ordre de
 * severite -> le filtrage par seuil est une simple comparaison `>=`.
 * ================================================================ */

#define OWL_LOG_BASE    OWL_SENTINEL_OFFSET(108)
#define OWL_LOG_DEBUG   (OWL_LOG_BASE + 0)
#define OWL_LOG_INFO    (OWL_LOG_BASE + 1)
#define OWL_LOG_AVERT   (OWL_LOG_BASE + 2)
#define OWL_LOG_ERREUR  (OWL_LOG_BASE + 3)

/* Index interne 0..3 -> sentinel + nom neutre (format par defaut). */
static const int64_t LOG_LEVEL_SENTINELS[4] = {
    OWL_LOG_DEBUG, OWL_LOG_INFO, OWL_LOG_AVERT, OWL_LOG_ERREUR
};
static const char* const LOG_LEVEL_NAMES[4] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

enum { LOG_LVL_DEBUG = 0, LOG_LVL_INFO, LOG_LVL_AVERT, LOG_LVL_ERREUR };

static const char* const NIV_VALUES[] = {
    "nivDebug", "nivInfo", "nivAvert", "nivErreur", NULL
};
static const OwlEnum E_logNiveau = {
    .name = "logNiveau", .nameEN = "logLevel", .category = "Log",
    .descFR = "Niveau de severite d'un message de log (seuil propriete Niveau).",
    .descEN = "Severity level of a log message (Level threshold property).",
    .values = NIV_VALUES,
    .combinable = false,
};

static const OwlEnum* const LOG_ENUMS[] = { &E_logNiveau };

static const OwlConstDef LOG_CONSTS[] = {
    { .name = "nivDebug", .category = "Log", .nameEN = "lvlDebug",
      .value = INTEGER_VAL(OWL_LOG_DEBUG),
      .descFR = "Niveau le plus fin : valeurs intermediaires et diagnostics de debogage.",
      .descEN = "Finest level: intermediate values and debugging diagnostics." },
    { .name = "nivInfo", .category = "Log", .nameEN = "lvlInfo",
      .value = INTEGER_VAL(OWL_LOG_INFO),
      .descFR = "Niveau information : etapes normales du deroulement.",
      .descEN = "Info level: normal flow milestones." },
    { .name = "nivAvert", .category = "Log", .nameEN = "lvlWarn",
      .value = INTEGER_VAL(OWL_LOG_AVERT),
      .descFR = "Niveau avertissement : situation anormale mais non bloquante.",
      .descEN = "Warning level: abnormal but non-blocking situation." },
    { .name = "nivErreur", .category = "Log", .nameEN = "lvlError",
      .value = INTEGER_VAL(OWL_LOG_ERREUR),
      .descFR = "Niveau erreur : echec d'une operation.",
      .descEN = "Error level: an operation failed." },
};

/* ================================================================
 * Type LogEnreg / LogRecord — record passe au formateur / emetteur
 * ================================================================ */

static const OwlField FIELDS_LogEnreg[] = {
    {"Horodatage", NULL, "cha\xc3\xae""ne", "\"\"",
     "horodatage de l'emission du message", "timestamp of the message emission",
     .nameEN = "Timestamp"},
    {"Niveau", NULL, "entier", "nivInfo",
     "niveau de severite du message", "severity level of the message",
     .allowedBase = &E_logNiveau, .nameEN = "Level"},
    {"NiveauTexte", NULL, "cha\xc3\xae""ne", "\"\"",
     "niveau de severite en texte (DEBUG/INFO/WARN/ERROR), pour les formats structures",
     "severity level as text (DEBUG/INFO/WARN/ERROR), for structured formats",
     .nameEN = "LevelText"},
    {"Message", NULL, "cha\xc3\xae""ne", "\"\"",
     "texte du message tel que fourni a la methode de log", "message text as passed to the log method",
     .nameEN = "Message"},
    {0},
};

static const char* const ALIAS_LogEnreg[] = { "LogEnreg", "LogRecord", NULL };

/* ================================================================
 * Type Logger — journal applicatif configurable
 * ================================================================ */

static const OwlField FIELDS_Logger[] = {
    {"Niveau", NULL, "entier", "nivInfo",
     "seuil de severite : les messages sous ce niveau ne sont pas emis",
     "severity threshold: messages below this level are not emitted",
     .allowedBase = &E_logNiveau, .nameEN = "Level"},
    {"Formateur", NULL, "proc\xc3\xa9""dure", "Null",
     "procedure qui faconne un LogEnreg en chaine (format du message)",
     "procedure that shapes a LogEnreg into a string (message format)",
     .nameEN = "Formatter", .callbackSig = "LogEnreg"},
    {"Destination", NULL, "cha\xc3\xae""ne", "\"\"",
     "cible integree : stdout par defaut, ou chemin de fichier",
     "built-in target: stdout by default, or a file path",
     .nameEN = "Destination"},
    {"Emetteur", "\xc3\x89metteur", "proc\xc3\xa9""dure", "Null",
     "procedure d'emission custom appelee avec le LogEnreg (sink externe)",
     "custom emission procedure called with the LogEnreg (external sink)",
     .nameEN = "Emitter", .callbackSig = "LogEnreg"},
    {0},
};

static const char* const ALIAS_Logger[] = { "Logger", NULL };

static const OwlTypeDef LOG_TYPES[] = {
    { .canonical      = "Logger",
      .canonicalAscii = "Logger",
      .aliases        = ALIAS_Logger,
      .category       = "Log",
      .descFR         = "Journal applicatif a niveaux : filtre par seuil (Niveau), decompose chaque entree en metadonnees (LogEnreg) et laisse le format libre via une procedure (Formateur). Destination et emission custom (Emetteur) sont des leviers independants.",
      .descEN         = "Leveled application logger: filters by threshold (Level), breaks each entry into metadata (LogEnreg) and leaves the format free via a procedure (Formatter). Destination and custom emission (Emitter) are independent levers.",
      .fields         = FIELDS_Logger,
      .nameEN         = "Logger",
      .genre          = OWL_GENRE_M },   /* le Logger */
    { .canonical      = "LogEnreg",
      .canonicalAscii = "LogEnreg",
      .aliases        = ALIAS_LogEnreg,
      .category       = "Log",
      .descFR         = "Enregistrement de log a metadonnees decomposees (horodatage, niveau, message) passe au formateur et a l'emetteur d'un Logger.",
      .descEN         = "Log record with decomposed metadata (timestamp, level, message) passed to a Logger's formatter and emitter.",
      .fields         = FIELDS_LogEnreg,
      .nameEN         = "LogRecord",
      .genre          = OWL_GENRE_M },   /* le LogEnreg */
};

/* Construit le template runtime d'un type Log* (defaults canon). */
static ObjInstance* build_logger_template(void) {
    ObjString* name = obj_copy_string("Logger", 6);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));
    inst_field_set_int  (tpl, "Niveau",      OWL_LOG_INFO);
    inst_field_set_value(tpl, "Formateur",   NIL_VAL);
    inst_field_set_str  (tpl, "Destination", "");
    inst_field_set_value(tpl, "Emetteur",    NIL_VAL);
    vm_pop();  /* tpl */
    vm_pop();  /* name */
    return tpl;
}

static ObjInstance* build_logenreg_template(void) {
    ObjString* name = obj_copy_string("LogEnreg", 8);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Horodatage",  "");
    inst_field_set_int(tpl, "Niveau",      OWL_LOG_INFO);
    inst_field_set_str(tpl, "NiveauTexte", "");
    inst_field_set_str(tpl, "Message",     "");
    vm_pop();  /* tpl */
    vm_pop();  /* name */
    return tpl;
}

/* ================================================================
 * Lecture des champs de l'instance Logger
 * ================================================================ */

static bool log_get_field(const ObjInstance* inst, const char* name, Value* out) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    return table_get((Table*)&inst->fields, k, out);
}

static int64_t log_get_int(const ObjInstance* inst, const char* name, int64_t dflt) {
    Value v;
    if (log_get_field(inst, name, &v) && IS_INTEGER(v)) return AS_INTEGER(v);
    return dflt;
}

static bool log_is_logger_instance(Value v) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    return inst->structName != NULL
        && strcmp(inst->structName->chars, "Logger") == 0;
}

/* Logger par defaut implicite (natives statiques), stocke dans vm.globals.
 * Cree paresseusement ; rooted par vm.globals des sa pose. */
static const char* LOG_DEFAULT_GLOBAL = "__LoggerDefaut";

static ObjInstance* logger_default_instance(void) {
    ObjString* k = obj_copy_string(LOG_DEFAULT_GLOBAL, (int)strlen(LOG_DEFAULT_GLOBAL));
    vm_push(OBJ_VAL(k));
    Value existing;
    bool have = table_get(&vm.globals, k, &existing);
    vm_pop();  /* k */
    if (have && IS_INSTANCE(existing) && log_is_logger_instance(existing))
        return AS_INSTANCE(existing);

    ObjString* name = obj_copy_string("Logger", 6);
    vm_push(OBJ_VAL(name));
    ObjInstance* inst = obj_new_instance(name);
    vm_push(OBJ_VAL(inst));
    inst_field_set_int  (inst, "Niveau",      OWL_LOG_INFO);
    inst_field_set_value(inst, "Formateur",   NIL_VAL);
    inst_field_set_str  (inst, "Destination", "");
    inst_field_set_value(inst, "Emetteur",    NIL_VAL);
    obj_globals_set(LOG_DEFAULT_GLOBAL, OBJ_VAL(inst));
    vm_pop();  /* inst */
    vm_pop();  /* name */
    return inst;
}

/* ================================================================
 * Emission
 * ================================================================ */

/* Ecrit le texte formate vers la destination integree (fichier en append si
 * un chemin non vide est pose, stdout sinon). N'alloue rien cote GC. */
static void logger_write_destination(ObjInstance* self, const char* text, int len) {
    Value dest;
    const char* path = NULL;
    if (log_get_field(self, "Destination", &dest) && IS_STRING(dest)
        && AS_STRING(dest)->length > 0) {
        path = AS_STRING(dest)->chars;   /* dest rooted via self->fields */
    }
    if (path) {
        FILE* f = fopen(path, "ab");
        if (f) {
            if (len > 0) fwrite(text, 1, (size_t)len, f);
            fputc('\n', f);
            fclose(f);
        }
    } else {
        if (len > 0) fwrite(text, 1, (size_t)len, stdout);
        fputc('\n', stdout);
    }
}

/* Invoque une procedure-valeur avec le record en unique argument.
 * Retourne true (+ *out si non-NULL) quand l'appel reussit. Retourne false si
 * le callback du dev leve une erreur : la VM a alors DEJA deroule la pile de la
 * frontiere reentrante — l'appelant ne doit plus la remanier (ni vm_pop ni
 * forcer stackTop), sous peine de corrompre la pile en cours de propagation
 * d'erreur. L'etat d'erreur reste arme et remonte a l'appelant du logger. */
static bool logger_invoke(Value proc, ObjInstance* rec, Value* out) {
    vm_push(proc);
    vm_push(OBJ_VAL(rec));
    InterpretResult r = vm_call_reentrant(proc, 1);
    if (r != INTERPRET_OK) {
        if (out) *out = NIL_VAL;
        return false;
    }
    Value ret = vm_pop();   /* OK : le reentrant pousse toujours un resultat */
    if (out) *out = ret;
    return true;
}

static void logger_emit(ObjInstance* self, int levelIdx, Value msgVal) {
    int64_t seuil  = log_get_int(self, "Niveau", OWL_LOG_INFO);
    int64_t lvlVal = LOG_LEVEL_SENTINELS[levelIdx];
    if (lvlVal < seuil) return;   /* sous le seuil : aucune sortie, aucun emetteur */

    /* Horodatage (toujours renseigne -> Taille > 0). */
    char ts[32];
    time_t tt = time(NULL);
    struct tm* lt = localtime(&tt);
    if (lt) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", lt);
    else    snprintf(ts, sizeof(ts), "0000-00-00 00:00:00");

    /* Record LogEnreg (rooted pendant toute l'emission). */
    ObjString* recName = obj_copy_string("LogEnreg", 8);
    vm_push(OBJ_VAL(recName));
    ObjInstance* rec = obj_new_instance(recName);
    vm_push(OBJ_VAL(rec));
    inst_field_set_str  (rec, "Horodatage",  ts);
    inst_field_set_int  (rec, "Niveau",      lvlVal);
    inst_field_set_str  (rec, "NiveauTexte", LOG_LEVEL_NAMES[levelIdx]);
    inst_field_set_value(rec, "Message",     msgVal);

    /* Formateur -> texte de sortie (defaut neutre si non pose). Si le formateur
     * du dev leve une erreur, la VM a deroule la pile : on sort immediatement
     * SANS remanier la pile ni depiler nos roots (deja reclames par l'unwind),
     * l'erreur remonte a l'appelant du logger. */
    Value fmt;
    bool hasFmt = log_get_field(self, "Formateur", &fmt) && !IS_NIL(fmt);
    Value formatted = NIL_VAL;
    if (hasFmt && !logger_invoke(fmt, rec, &formatted)) return;

    char* heapText = NULL;
    const char* outText;
    int outLen;
    char defbuf[600];
    if (IS_STRING(formatted)) {
        /* Copie immediate (malloc, pas de GC) : `formatted` a ete depile,
         * le lire apres une alloc OWL ulterieure serait un UAF. */
        outLen = AS_STRING(formatted)->length;
        heapText = (char*)malloc((size_t)outLen + 1);
        if (heapText) {
            memcpy(heapText, AS_STRING(formatted)->chars, (size_t)outLen);
            heapText[outLen] = '\0';
            outText = heapText;
        } else {
            outText = "";
            outLen = 0;
        }
    } else {
        const char* msgc = IS_STRING(msgVal) ? AS_STRING(msgVal)->chars : "";
        int n = snprintf(defbuf, sizeof(defbuf), "[%s] %s %s",
                         ts, LOG_LEVEL_NAMES[levelIdx], msgc);
        if (n < 0) n = 0;
        if (n >= (int)sizeof(defbuf)) n = (int)sizeof(defbuf) - 1;
        outText = defbuf;
        outLen = n;
    }

    logger_write_destination(self, outText, outLen);
    free(heapText);

    /* Emetteur custom (sink externe) — independant de la destination. Meme
     * contrat d'erreur que le formateur : sortie immediate sans remanier la
     * pile si le callback du dev leve. */
    Value em;
    if (log_get_field(self, "Emetteur", &em) && !IS_NIL(em)
        && !logger_invoke(em, rec, NULL))
        return;

    vm_pop();  /* rec */
    vm_pop();  /* recName */
}

/* Resout (logger, message) depuis les args : forme methode (args[0] = instance
 * Logger, args[1] = message) OU forme statique (args[0] = message, logger par
 * defaut implicite). */
static void logger_level_dispatch(int levelIdx, int argCount, Value* args) {
    if (argCount >= 1 && log_is_logger_instance(args[0])) {
        ObjInstance* self = AS_INSTANCE(args[0]);
        Value msg = (argCount >= 2) ? args[1] : NIL_VAL;
        logger_emit(self, levelIdx, msg);
    } else {
        ObjInstance* self = logger_default_instance();
        Value msg = (argCount >= 1) ? args[0] : NIL_VAL;
        logger_emit(self, levelIdx, msg);
    }
}

static Value fn_logger_debug (int c, Value* a) { logger_level_dispatch(LOG_LVL_DEBUG,  c, a); return NIL_VAL; }
static Value fn_logger_info  (int c, Value* a) { logger_level_dispatch(LOG_LVL_INFO,   c, a); return NIL_VAL; }
static Value fn_logger_avert (int c, Value* a) { logger_level_dispatch(LOG_LVL_AVERT,  c, a); return NIL_VAL; }
static Value fn_logger_erreur(int c, Value* a) { logger_level_dispatch(LOG_LVL_ERREUR, c, a); return NIL_VAL; }

/* LoggerParametre(niveau [, formateur [, destination]]) — regle les proprietes
 * du logger par defaut implicite. */
static Value fn_logger_parametre(int argCount, Value* args) {
    ObjInstance* self = logger_default_instance();
    vm_push(OBJ_VAL(self));   /* root pendant les ecritures de champs */
    if (argCount >= 1 && IS_INTEGER(args[0]))
        inst_field_set_int(self, "Niveau", AS_INTEGER(args[0]));
    if (argCount >= 2)
        inst_field_set_value(self, "Formateur", args[1]);
    if (argCount >= 3)
        inst_field_set_value(self, "Destination", args[2]);
    vm_pop();   /* self */
    return NIL_VAL;
}

/* ================================================================
 * Enregistrement
 * ================================================================ */

void stdlib_log_init(void) {
    static const OwlParam p_log[] = {
        {"message", "cha\xc3\xae""ne", "message a journaliser"},
        {0}
    };
    static const OwlSignature sig_log[] = {
        {1, p_log, "rien", "aucune valeur (emission de log)"},
        {0}
    };

    static const OwlParam p_param[] = {
        {"niveau",      "entier",      "seuil de severite", .allowedBase = &E_logNiveau},
        {"formateur",   "proc\xc3\xa9""dure", "procedure de formatage (LogEnreg -> chaine, optionnel)"},
        {"destination", "cha\xc3\xae""ne",    "cible : stdout par defaut, ou chemin fichier (optionnel)"},
        {0}
    };
    static const OwlSignature sig_param[] = {
        {1, p_param, "rien", "regle le seuil du logger par defaut"},
        {2, p_param, "rien", "regle le seuil et le formateur"},
        {3, p_param, "rien", "regle le seuil, le formateur et la destination"},
        {0}
    };

    static const OwlMethodBinding M_DEBUG[]  = { {"Logger", "Debug", -1}, {NULL, NULL, 0} };
    static const OwlMethodBinding M_INFO[]   = { {"Logger", "Info",  -1}, {NULL, NULL, 0} };
    static const OwlMethodBinding M_AVERT[]  = { {"Logger", "Avert", -1}, {"Logger", "Warn",  -1}, {NULL, NULL, 0} };
    static const OwlMethodBinding M_ERREUR[] = { {"Logger", "Erreur", -1}, {"Logger", "Error", -1}, {NULL, NULL, 0} };

    static const OwlNativeDef DEFS[] = {
        { .canonical = "LoggerDebug", .nameEN = "LoggerDebug", .category = "Log",
          .descFR = "Emet un message au niveau le plus fin (Debug) via le logger par defaut implicite, ou sur une instance Logger (methode .Debug).",
          .descEN = "Emits a message at the finest level (Debug) through the implicit default logger, or on a Logger instance (.Debug method).",
          .signatures = sig_log, .impl = fn_logger_debug, .methodBindings = M_DEBUG },
        { .canonical = "LoggerInfo", .nameEN = "LoggerInfo", .category = "Log",
          .descFR = "Emet un message au niveau Info via le logger par defaut implicite, ou sur une instance Logger (methode .Info).",
          .descEN = "Emits a message at Info level through the implicit default logger, or on a Logger instance (.Info method).",
          .signatures = sig_log, .impl = fn_logger_info, .methodBindings = M_INFO },
        { .canonical = "LoggerAvert", .nameEN = "LoggerWarn", .category = "Log",
          .descFR = "Emet un avertissement (niveau Avert) via le logger par defaut implicite, ou sur une instance Logger (methode .Avert).",
          .descEN = "Emits a warning (Warn level) through the implicit default logger, or on a Logger instance (.Warn method).",
          .signatures = sig_log, .impl = fn_logger_avert, .methodBindings = M_AVERT },
        { .canonical = "LoggerErreur", .nameEN = "LoggerError", .category = "Log",
          .descFR = "Emet un message d'erreur (niveau Erreur) via le logger par defaut implicite, ou sur une instance Logger (methode .Erreur). N'ouvre aucun dialogue et ne declenche pas ErreurDetectee : c'est une emission de log, pas une exception.",
          .descEN = "Emits an error message (Error level) through the implicit default logger, or on a Logger instance (.Error method). Opens no dialog and does not raise ErreurDetectee: it is a log emission, not an exception.",
          .signatures = sig_log, .impl = fn_logger_erreur, .methodBindings = M_ERREUR },
        { .canonical = "LoggerParam\xc3\xa8tre", .canonicalAscii = "LoggerParametre", .nameEN = "LoggerSetting",
          .category = "Log",
          .descFR = "Regle les proprietes du logger par defaut implicite : seuil de niveau, formateur et destination, en un seul appel (arites 1 a 3).",
          .descEN = "Sets the implicit default logger's properties: level threshold, formatter and destination, in a single call (arities 1 to 3).",
          .signatures = sig_param, .impl = fn_logger_parametre },
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums(LOG_ENUMS, (int)(sizeof(LOG_ENUMS)/sizeof(LOG_ENUMS[0])));
    owl_registry_register_consts(LOG_CONSTS, (int)(sizeof(LOG_CONSTS)/sizeof(LOG_CONSTS[0])));
    owl_registry_register_types(LOG_TYPES, (int)(sizeof(LOG_TYPES)/sizeof(LOG_TYPES[0])));

    /* Templates runtime pour __StructClone (declaration `x est un Logger`). */
    ObjInstance* tplLog = build_logger_template();
    vm_push(OBJ_VAL(tplLog));
    vm_type_register("Logger", OBJ_VAL(tplLog));
    vm_pop();

    ObjInstance* tplRec = build_logenreg_template();
    vm_push(OBJ_VAL(tplRec));
    vm_type_register("LogEnreg",  OBJ_VAL(tplRec));
    vm_type_register("LogRecord", OBJ_VAL(tplRec));
    vm_pop();
}
