// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_errors.c — Implementation du registry des messages d'erreur.
 *
 * Le tableau OWL_ERRORS[] est construit en passe 2 du double-include
 * de owl_errors.def. Toute la logique d'introspection lit ce tableau.
 * Cf. owl_errors.h pour la documentation de l'interface.
 *
 * L'entree d'index 0 est reservee a OWL_ERR_NONE (sentinel "pas d'erreur").
 * Les codes utilisateur commencent a l'index 1. L'ordre du tableau suit
 * l'ordre du .def par construction X-macro, donc OWL_ERRORS[code] retourne
 * directement la def correspondant a OwlErrorCode == code.
 */

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#include "owl_errors.h"

/* Passe 2 — construit OWL_ERRORS[] depuis le .def. L'index 0 est OWL_ERR_NONE. */
#define OWL_ERROR(name, cat, tplFR, tplEN) \
    { #name, (cat), (tplFR), (tplEN) },
static const OwlErrorDef OWL_ERRORS[] = {
    { "OWL_ERR_NONE", "internal", "(aucune erreur)", "(no error)" },
#include "owl_errors.def"
};
#undef OWL_ERROR

static const int OWL_ERRORS_N =
    (int)(sizeof(OWL_ERRORS) / sizeof(OWL_ERRORS[0]));

/* ================================================================
 * Introspection
 * ================================================================ */

void owl_errors_init(void) {
    /* Tableau static const, pas d'init dynamique necessaire. Conserve pour
     * homogeneite avec owl_limits_init / autres registries. */
}

int owl_registry_error_count(void) {
    /* Hors OWL_ERR_NONE (index 0) : nombre de codes utilisables. */
    return OWL_ERRORS_N - 1;
}

const OwlErrorDef* owl_registry_error_at(int index) {
    /* Index utilisateur 0-based qui saute OWL_ERR_NONE : on decale de +1. */
    if (index < 0 || index >= OWL_ERRORS_N - 1) return NULL;
    return &OWL_ERRORS[index + 1];
}

/* Comparaison case-insensitive ASCII pure (noms de codes sont ASCII E_FOO_BAR). */
static int eq_ci(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

const OwlErrorDef* owl_registry_error_find(const char* name) {
    if (!name) return NULL;
    /* On scanne tout le tableau sentinel inclus pour pouvoir retrouver
     * "OWL_ERR_NONE" via find si besoin. */
    for (int i = 0; i < OWL_ERRORS_N; i++) {
        if (eq_ci(OWL_ERRORS[i].name, name)) return &OWL_ERRORS[i];
    }
    return NULL;
}

const OwlErrorDef* owl_registry_error_by_code(OwlErrorCode code) {
    int idx = (int)code;
    if (idx < 0 || idx >= OWL_ERRORS_N) return NULL;
    return &OWL_ERRORS[idx];
}

/* ================================================================
 * Alias numerique
 *
 * Le numero d'un code est son rang d'apparition dans le .def, compte par
 * espace de prefixe (E_ -> E000N, W_ -> W000N). On ne stocke pas ces rangs :
 * on les recalcule par scan lineaire, le tableau est petit (< 1000 codes)
 * et les acces sont rares (CLI explain / ErreurInfo, jamais en boucle chaude).
 * ================================================================ */

/* Prefixe numerique d'un code selon la 1ere lettre de son nom symbolique :
 * 'E' pour E_*, 'W' pour W_*, 0 si aucun (ne devrait pas arriver). */
static char num_prefix_of(const char* name) {
    if (!name) return 0;
    if (name[0] == 'E' || name[0] == 'e') return 'E';
    if (name[0] == 'W' || name[0] == 'w') return 'W';
    return 0;
}

const char* owl_registry_error_num(OwlErrorCode code) {
    static char buf[8];
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    if (!d) return NULL;
    char prefix = num_prefix_of(d->name);
    if (!prefix) return NULL;

    int idx = (int)code;
    /* Compte les codes du meme espace jusqu'a `code` inclus (rang 1-based). */
    int rank = 0;
    for (int i = 1; i <= idx && i < OWL_ERRORS_N; i++) {
        if (num_prefix_of(OWL_ERRORS[i].name) == prefix) rank++;
    }
    if (rank == 0) return NULL;
    snprintf(buf, sizeof buf, "%c%04d", prefix, rank);
    return buf;
}

OwlErrorCode owl_registry_error_code_of(const OwlErrorDef* def) {
    if (!def) return OWL_ERR_NONE;
    if (def < &OWL_ERRORS[0] || def >= &OWL_ERRORS[OWL_ERRORS_N])
        return OWL_ERR_NONE;
    return (OwlErrorCode)(int)(def - &OWL_ERRORS[0]);
}

const OwlErrorDef* owl_registry_error_by_num(const char* num) {
    if (!num) return NULL;
    char prefix = num[0];
    if (prefix == 'e') prefix = 'E';
    if (prefix == 'w') prefix = 'W';
    if (prefix != 'E' && prefix != 'W') return NULL;

    /* Le reste doit etre exclusivement des chiffres (au moins un). */
    const char* p = num + 1;
    if (!*p) return NULL;
    long target = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return NULL;
        target = target * 10 + (*p - '0');
    }
    if (target <= 0) return NULL;

    /* Retrouve le Nème code de l'espace `prefix`. */
    int rank = 0;
    for (int i = 1; i < OWL_ERRORS_N; i++) {
        if (num_prefix_of(OWL_ERRORS[i].name) == prefix) {
            rank++;
            if (rank == target) return &OWL_ERRORS[i];
        }
    }
    return NULL;
}

/* ================================================================
 * Format engine : substitution {placeholder} -> valeur.
 *
 * Approche : scan lineaire du template, copie byte-a-byte. A chaque '{'
 * on cherche le '}' fermant et on regarde si la clef entre les deux est
 * dans la liste va_args (paires name/value terminees par NULL). Match =
 * insertion de la valeur. Pas de match = on garde le placeholder litteral
 * (utile pour debug : visible immediatement qu'un arg manque).
 *
 * Buffer thread-local statique (runtime OWL mono-thread). Deux buffers
 * separes FR/EN pour permettre des rendus FR et EN concurrents dans le
 * meme test unitaire sans collision.
 * ================================================================ */

#define OWL_ERROR_BUF_SIZE 1024

/* Liaison de repli pour la clef "native" (cf. owl_error_render_bind_native).
 * Le nom de la native est deja un parametre POSITIONNEL de
 * owl_emit_native_error ; l'exiger une seconde fois sous forme de paire
 * "native", <nom> etait un cablage manuel que ~40 sites d'emission avaient
 * oublie, faisant sortir "{native}" litteral dans le message utilisateur.
 * La paire explicite reste prioritaire (aucun site existant ne change de
 * rendu), ce repli ne sert que quand elle est absente. */
static const char* g_render_native_fallback = NULL;

void owl_error_render_bind_native(const char* native_name) {
    g_render_native_fallback = native_name;
}

/* Lookup d'un placeholder dans va_args. Recommence va_copy a chaque appel
 * pour preserver les arguments cote caller. Retourne la valeur ou NULL. */
static const char* lookup_arg(va_list args, const char* key, int keyLen) {
    va_list copy;
    va_copy(copy, args);
    const char* val = NULL;
    while (1) {
        const char* name = va_arg(copy, const char*);
        if (name == NULL) break;
        const char* v = va_arg(copy, const char*);
        if ((int)strlen(name) == keyLen && memcmp(name, key, keyLen) == 0) {
            val = v;
            /* On ne break pas — derniere occurrence gagne, mais peu importe
             * en pratique car les caller ne dupliquent pas les keys. */
            break;
        }
    }
    va_end(copy);
    if (val == NULL && g_render_native_fallback != NULL &&
        keyLen == 6 && memcmp(key, "native", 6) == 0) {
        val = g_render_native_fallback;
    }
    return val;
}

static const char* render_template(const char* tpl, char* buf, int bufSize,
                                   va_list args) {
    if (!tpl) { if (bufSize > 0) buf[0] = '\0'; return buf; }
    int o = 0;
    const char* p = tpl;
    while (*p && o < bufSize - 1) {
        if (*p == '{') {
            /* Cherche le '}' fermant. */
            const char* end = strchr(p, '}');
            if (!end) {
                /* '{' non ferme : copie litteralement le reste. */
                buf[o++] = *p++;
                continue;
            }
            int keyLen = (int)(end - p - 1);
            if (keyLen <= 0) {
                /* "{}" vide : copie litteralement. */
                buf[o++] = *p++;
                continue;
            }
            /*  fix : valider que le contenu entre {...} est un
             * identifiant valide (alphanumerique + underscore). Sinon c'est
             * une notation OWL brace literal `{ ... }` ou `{...}` litterale
             * dans le message — on la copie telle quelle SANS scanner la
             * va_list (sinon lookup_arg lit la pile sans NULL terminator
             * quand le caller n'a pas passe d'args = SEGV deterministe).
             *
             * Cas qui a explose : template E_SYNTAX_BRACE_LITERAL_NO_TYPE
             * contient `{ ... }` et `{...}` litteraux, mais
             * owl_emit_error_current(...) etait appele sans args
             * supplementaires ni NULL terminator. */
            bool isValidKey = true;
            for (int ki = 0; ki < keyLen; ki++) {
                char c = p[1 + ki];
                bool ok = (c >= 'a' && c <= 'z') ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') ||
                          c == '_';
                if (!ok) { isValidKey = false; break; }
            }
            if (!isValidKey) {
                /* Litteral OWL brace `{ ... }` : copie verbatim jusqu'au '}'. */
                int span = (int)(end - p + 1);
                int room = bufSize - 1 - o;
                if (span > room) span = room;
                memcpy(buf + o, p, span);
                o += span;
                p = end + 1;
                continue;
            }
            const char* val = lookup_arg(args, p + 1, keyLen);
            if (val) {
                int vlen = (int)strlen(val);
                int room = bufSize - 1 - o;
                if (vlen > room) vlen = room;
                memcpy(buf + o, val, vlen);
                o += vlen;
            } else {
                /* Clef inconnue : garde le placeholder litteral. */
                int span = (int)(end - p + 1);
                int room = bufSize - 1 - o;
                if (span > room) span = room;
                memcpy(buf + o, p, span);
                o += span;
            }
            p = end + 1;
        } else {
            buf[o++] = *p++;
        }
    }
    buf[o] = '\0';
    return buf;
}

const char* owl_error_render_fr_va(OwlErrorCode code, va_list args) {
    static char buf[OWL_ERROR_BUF_SIZE];
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    const char* tpl = d ? d->templateFR : "";
    render_template(tpl, buf, sizeof buf, args);
    return buf;
}

const char* owl_error_render_en_va(OwlErrorCode code, va_list args) {
    static char buf[OWL_ERROR_BUF_SIZE];
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    const char* tpl = d ? d->templateEN : "";
    render_template(tpl, buf, sizeof buf, args);
    return buf;
}

const char* owl_error_render_fr(OwlErrorCode code, ...) {
    va_list args;
    va_start(args, code);
    const char* r = owl_error_render_fr_va(code, args);
    va_end(args);
    return r;
}

const char* owl_error_render_en(OwlErrorCode code, ...) {
    va_list args;
    va_start(args, code);
    const char* r = owl_error_render_en_va(code, args);
    va_end(args);
    return r;
}

/* Langue de rendu active. Defaut FR (non-regression : tous les
 * sites d'emission existants — compiler, runtime, native — passaient par le
 * rendu FR en dur). Le flag CLI `--lang en` (main.c) et le LSP en mode `en`
 * basculent en EN. Etat global mono-thread (runtime OWL mono-thread). */
static int g_owl_error_lang_en = 0;

void owl_error_render_set_lang_en(int enabled) {
    g_owl_error_lang_en = enabled ? 1 : 0;
}

int owl_error_render_lang_is_en(void) {
    return g_owl_error_lang_en;
}

const char* owl_error_render_active_va(OwlErrorCode code, va_list args) {
    return g_owl_error_lang_en ? owl_error_render_en_va(code, args)
                               : owl_error_render_fr_va(code, args);
}

/* ================================================================
 * Emission : sortie stderr (homogene avec error_at / runtime_error legacy).
 *
 * Lot 1 minimal viable. L'integration LSP / buffer / longjmp viendra dans
 * un ticket ulterieur (cf  quand redige).
 * ================================================================ */

void owl_emit_error(OwlErrorCode code, ...) {
    static char buf[OWL_ERROR_BUF_SIZE];
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    const char* tpl = d ? d->templateFR : "";
    va_list args;
    va_start(args, code);
    render_template(tpl, buf, sizeof buf, args);
    va_end(args);
    fprintf(stderr, "%s\n", buf);
}

/* Note : les helpers compile-aware owl_emit_error_at / owl_emit_warning_at /
 * owl_emit_error_current sont implementes dans owl_errors_compile.c (linke
 * seulement avec le binaire owl, pas le LSP qui ne connait pas compiler.c). */
