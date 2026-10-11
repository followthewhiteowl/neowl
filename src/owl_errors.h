// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_errors_h
#define owl_errors_h

#include <stddef.h>

/* ================================================================
 * OWL ERRORS — Single-source registry des messages d'erreur.
 *
 * Tous les codes d'erreur OWL sont déclarés dans owl_errors.def via
 * la macro OWL_ERROR(name, category, templateFR, templateEN). Ce
 * fichier expose deux vues construites par double-include :
 *
 *   1. enum OwlErrorCode : code symbole compilable (E_LEX_UNTERMINATED_STRING, etc.)
 *   2. struct OwlErrorDef + tableau OWL_ERRORS[] : introspection
 *      (CLI, LSP, self-test)
 *
 * Pour émettre une erreur : `owl_emit_error(code, ...)` (args nommés
 * pairs terminés par NULL). Pour tester le rendu de templates :
 * `owl_error_render_fr(code, ...)` / `owl_error_render_en(code, ...)`.
 *
 * Cf.  pour le pattern X-macro et la politique stabilité.
 * ================================================================ */

/* Passe 1 — enum compilable depuis owl_errors.def */
#define OWL_ERROR(name, cat, tplFR, tplEN) name,
typedef enum OwlErrorCode {
    OWL_ERR_NONE = 0,
#include "owl_errors.def"
    OWL_ERROR_COUNT_SENTINEL_
} OwlErrorCode;
#undef OWL_ERROR

/* Passe 2 — tableau introspectable */
typedef struct OwlErrorDef {
    const char* name;       /* "E_LEX_UNTERMINATED_STRING" */
    const char* category;   /* "lex", "syntax", "semantic", "runtime", "native", "io" */
    const char* templateFR; /* "Chaîne non terminée" / "Caractère inattendu : '{char}'" */
    const char* templateEN; /* "Unterminated string" / "Unexpected character: '{char}'" */
} OwlErrorDef;

/* ================================================================
 * API d'introspection (implémentée dans owl_errors.c)
 * ================================================================ */

/* Compte de codes déclarés dans le .def (hors OWL_ERR_NONE) */
int owl_registry_error_count(void);

/* Accès par index (ordre = ordre du .def, 0-based) */
const OwlErrorDef* owl_registry_error_at(int index);

/* Lookup case-insensitive par nom symbole ("E_LEX_UNTERMINATED_STRING" / "e_lex_unterminated_string") */
const OwlErrorDef* owl_registry_error_find(const char* name);

/* Lookup direct par code enum */
const OwlErrorDef* owl_registry_error_by_code(OwlErrorCode code);

/* Alias numerique court. Combo non-remplacant du nom symbolique.
 *
 * Chaque code recoit un numero derive de son ordre d'apparition dans
 * owl_errors.def, par espace de prefixe : les codes E_* numerotent E0001,
 * E0002, ... et les codes W_* numerotent W0001, W0002, ... (compteurs
 * separes). Format : prefixe (E ou W) + 4 chiffres zero-paddes. */

/* Numero formate ("E0042", "W0003") d'un code enum. Buffer static reutilise
 * a chaque appel — copier si conservation necessaire. NULL si code invalide. */
const char* owl_registry_error_num(OwlErrorCode code);

/* Lookup inverse : retrouve la def depuis un numero "E0042" / "W0003"
 * (case-insensitive sur le prefixe). NULL si format invalide ou hors plage. */
const OwlErrorDef* owl_registry_error_by_num(const char* num);

/* Code enum correspondant a une def obtenue via find/at/by_num. Permet de
 * chainer find() -> num() sans connaitre l'index. OWL_ERR_NONE si def NULL
 * ou hors tableau. */
OwlErrorCode owl_registry_error_code_of(const OwlErrorDef* def);

/* Init facultative — homogène avec autres registries. Appelée depuis stdlib_init(). */
void owl_errors_init(void);

/* ================================================================
 * API d'émission et rendu
 * ================================================================ */

/* Émission d'erreur — args en pairs (name, value) terminés par NULL.
 * Exemple : owl_emit_error(E_LEX_UNEXPECTED_CHAR, "char", "@", NULL);
 * Détail d'impl (buffer, longjmp, callback LSP) : laissé à implementer. */
void owl_emit_error(OwlErrorCode code, ...);

/* Rendu d'un template avec args — pour test unitaire ET pour usage interne.
 * Renvoie un buffer thread-local (runtime OWL mono-thread, test unitaire OK).
 * Args en pairs (name, value) terminés par NULL.
 * Stub implémente retour "", tests assertent sur contenu = rouge attendu. */
const char* owl_error_render_fr(OwlErrorCode code, ...);
const char* owl_error_render_en(OwlErrorCode code, ...);

/* Variantes va_list pour permettre le forwarding depuis un autre helper
 * (cf. owl_errors_compile.c). Mêmes buffers que les versions ... */
#include <stdarg.h>
const char* owl_error_render_fr_va(OwlErrorCode code, va_list args);
const char* owl_error_render_en_va(OwlErrorCode code, va_list args);

/* Rendu selon la langue active. Defaut FR ; bascule EN via
 * owl_error_render_set_lang_en(1). Tous les helpers d'emission
 * (compile/runtime/native) passent par cette variante pour respecter le
 * flag `--lang en` / le mode LSP `en` sans dupliquer la logique de format. */
const char* owl_error_render_active_va(OwlErrorCode code, va_list args);
void owl_error_render_set_lang_en(int enabled);
int  owl_error_render_lang_is_en(void);

/* Lie le nom de native servant de repli au marqueur {native} du gabarit,
 * quand le site d'emission n'a pas passe de paire "native", <nom> explicite.
 * Posee puis relachee (NULL) autour de chaque rendu par owl_emit_native_error.
 * Le nom de la native est deja un parametre positionnel de ce helper :
 * exiger en plus une paire "native", <nom> etait un cablage manuel qu'une
 * quarantaine de sites avaient oublie, faisant fuiter "{native}" litteral
 * dans le message lu par l'utilisateur. */
void owl_error_render_bind_native(const char* native_name);

/* ================================================================
 * API compile-aware (Lot 2 : migration compiler)
 *
 * Section conditionnelle. Les modules stdlib_*.c qui ont besoin de
 * l'API native-aware mais incluent windows.h (collision TokenType
 * winnt.h vs scanner.h) peuvent definir OWL_ERRORS_SKIP_COMPILE_AWARE
 * AVANT d'inclure owl_errors.h pour eviter de tirer scanner.h. Refacto
 * fondation possible plus tard pour decoupler proprement (extraction
 * d'un sous-header owl_errors_compile.h). Cf.  jalon B.
 * ================================================================ */
#ifndef OWL_ERRORS_SKIP_COMPILE_AWARE

/* Token est un typedef sur struct anonyme dans scanner.h (`typedef struct
 * { ... } Token;`) donc forward decl impossible — on include directement.
 * Couplage assume : owl_errors est par nature lie a la notion de Token
 * pour cette API compile-aware. */
#include "scanner.h"

/* Emission compile-time avec contexte Token (file/line/column).
 * Produit stderr texte ou JSON selon mode global compiler (g_json_compile_errors).
 * Marque parser.hadError = true.
 * Args en pairs (name, value) termines par NULL. */
void owl_emit_error_at(const Token* tok, OwlErrorCode code, ...);
void owl_emit_warning_at(const Token* tok, OwlErrorCode code, ...);

/* Emission compile-time sans Token disponible : utilise parser.current en interne. */
void owl_emit_error_current(OwlErrorCode code, ...);

#endif /* OWL_ERRORS_SKIP_COMPILE_AWARE */

/* ================================================================
 * API runtime-aware (Lot 3 : migration runtime VM)
 * ================================================================ */

/* Emission runtime avec contexte VM (file/line depuis stack frame).
 * Produit stderr texte ou JSON selon mode global vm (g_json_error_mode).
 * Compatibilite longjmp : si exception handler VM set, longjmp apres emission.
 * Args en pairs (name, value) termines par NULL.
 *
 * Impl dans owl_errors_runtime.c (isole du LSP qui ne linke pas vm.c). */
void owl_emit_runtime_error(OwlErrorCode code, ...);

/* ================================================================
 * API native-aware (Lot 4 : migration stdlib)
 * ================================================================ */

/* Emission soft-error par une native stdlib (HashChaine, Modulo, etc.).
 * Stocke le code symbole canonique dans g_err_code (chaîne "E_NATIVE_xxx")
 * + le nom de la native dans g_err_native_name (accessible via
 * ErreurInfo(errFonction)) + le rendu FR dans g_err_message + pose
 * g_err_fresh = true (sentinelle soft-error ). Pas de longjmp :
 * les natives posent un soft-error, elles ne jettent pas.
 *
 * Args en pairs (name, value) termines par NULL.
 * Exemple : owl_emit_native_error(E_NATIVE_ARG_INVALID, "HashChaine",
 *                                  "arg", "chaine a hasher", NULL);
 *
 * Impl dans owl_errors_native.c (nouveau, isole du LSP). */
void owl_emit_native_error(OwlErrorCode code, const char* native_name, ...);

#endif
