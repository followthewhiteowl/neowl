// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_http_service.c — surface OWL du DSL de service HTTP.
 *
 * Phase B, pièce runtime : la native FluxÉcrit / StreamWrite émet un morceau
 * dans le flux de service courant et CONTINUE (emit-and-continue, distinct de
 * RENVOYER qui sort). Elle lit le ProdCtx courant depuis le slot ambiant de la
 * VM (owl_vm_flux_courant_get), ouvre le flux en paresseux au 1er appel, frame
 * selon le mode (SSE `data:` / octets bruts) et pousse des octets COPIES via le
 * transport — aucun ObjString ne traverse la frontière (règle d'or §2 ).
 *
 * La famille future (FluxÉvénement / FluxIdentifiant / FluxCommentaire) se pose
 * sur le même helper interne flux_courant_emit : ne coder ici que FluxÉcrit. */

#include "owl_stdlib.h"
#include "owl_registry.h"
/* http_server.h (via http_exec.h) tire winnt.h dont TokenType entre en
 * collision avec scanner.h : on n'a besoin ici que de l'API runtime des
 * codes d'erreur, pas des emetteurs compile-time. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "vm.h"
#include "object.h"
#include "http_exec.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Sérialiseur JSON minifié partagé (stdlib_json.c) — même cœur que le fallback
 * JSON de RENVOYER (§6), en variante mono-ligne pour un SSE propre. */
extern char* owl_json_serialize_value_compact(Value v, size_t* outLen);

/* Framing d'un morceau déjà extrait en octets C, puis poussée (COPIE côté
 * transport). Paramétré par le mode pour que la famille Flux* future n'ait qu'à
 * fournir son préfixe / suffixe. */
static OwlHttpFluxStatut flux_courant_emit(OwlHttpRep* rep, int mode,
                                           const unsigned char* payload, int len) {
    if (mode == OWL_HTTP_FLUX_MODE_OCTETS) {
        return owl_http_rep_flux_pousse(rep, (const char*)payload, (size_t)len);
    }
    /* SSE : la spec veut CHAQUE ligne d'un événement re-préfixée `data: `. On
     * découpe le payload sur '\n' et on émet `data: <ligne>\n` par ligne (ligne
     * vide -> `data:\n`, sans espace superflu), l'événement se termine par un
     * `\n` final. Un seul tampon, taille calculée en une passe (alloc bornée). */
    size_t taille = 1; /* le '\n' de fin d'événement */
    {
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || payload[i] == '\n') {
                int seg = i - start;
                taille += (size_t)(seg > 0 ? 6 : 5) + (size_t)seg + 1;
                start = i + 1;
            }
        }
    }
    char* buf = (char*)malloc(taille);
    if (!buf) return OWL_HTTP_FLUX_ABANDON;
    size_t o = 0;
    {
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || payload[i] == '\n') {
                int seg = i - start;
                if (seg > 0) {
                    memcpy(buf + o, "data: ", 6); o += 6;
                    memcpy(buf + o, payload + start, (size_t)seg); o += (size_t)seg;
                } else {
                    memcpy(buf + o, "data:", 5); o += 5;
                }
                buf[o++] = '\n';
                start = i + 1;
            }
        }
    }
    buf[o++] = '\n';
    OwlHttpFluxStatut st = owl_http_rep_flux_pousse(rep, buf, taille);
    free(buf);
    return st;
}

/* Lève une exception rattrapable depuis FluxÉcrit avec un code canon du
 * registre ( P4). errCode = symbole "E_..." -> filtrable par
 * ExceptionInfo(errCode). Le message est rendu depuis le template (langue
 * active), {native} = FluxÉcrit. */
static void flux_raise(OwlErrorCode code, ...) {
    va_list ap;
    va_start(ap, code);
    const char* msg = owl_error_render_active_va(code, ap);
    va_end(ap);
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    owl_vm_raise_native("Flux\xc3\x89""crit", d != NULL ? d->name : "",
                        msg != NULL ? msg : "");
}

/* FluxÉcrit(donnée) — émet un morceau dans le flux de service courant.
 * Hors flux (slot vide / ouverture impossible) : erreur de programmeur, code
 * exposé par la constante httpFluxHorsContexte. Client déconnecté (ABANDON) :
 * httpFluxDéconnecté, rattrapable — le handler se déroule et le framework
 * nettoie (P4, clôt ). Les deux constantes sont filtrables via
 * ExceptionInfo(errCode) — cf. HTTP_SERVICE_CONSTS. */
static Value fn_flux_ecrit(int argCount, Value* args) {
    (void)argCount;
    void* slot = owl_vm_flux_courant_get();
    if (slot == NULL) {
        flux_raise(E_NATIVE_HTTP_FLUX_HORS_CONTEXTE, "native", "FluxÉ""crit", NULL);
        return NIL_VAL;
    }
    OwlHttpProdCtx* ctx = (OwlHttpProdCtx*)slot;

    OwlHttpRep* rep = owl_http_prod_flux_assure(ctx);
    if (rep == NULL) {
        flux_raise(E_NATIVE_HTTP_FLUX_HORS_CONTEXTE, "native", "FluxÉ""crit", NULL);
        return NIL_VAL;
    }

    /* La signature annonce `donnée : tout`. Chaîne / Buffer -> octets bruts.
     * Toute autre valeur (entier, structure, tableau…) est auto-sérialisée en
     * JSON minifié sur le même chemin que le fallback JSON de RENVOYER (§6),
     * afin d'honorer `tout` (miroir de RENVOYER : structure -> application/json).
     * Le tampon JSON est possédé ici et libéré après l'émission. */
    const unsigned char* payload = NULL;
    int len = 0;
    char* json_buf = NULL;
    if (!value_as_bytes(args[0], &payload, &len)) {
        size_t jlen = 0;
        json_buf = owl_json_serialize_value_compact(args[0], &jlen);
        if (json_buf == NULL) {
            /* Non sérialisable (rare : échec d'allocation) : erreur claire. */
            flux_raise(E_NATIVE_ARG_INVALID, "native", "FluxÉ""crit", NULL);
            return NIL_VAL;
        }
        payload = (const unsigned char*)json_buf;
        len     = (int)jlen;
    }

    OwlHttpFluxStatut st = flux_courant_emit(rep, owl_http_prod_flux_mode(ctx),
                                             payload, len);
    free(json_buf);
    if (st == OWL_HTTP_FLUX_ABANDON) {
        /* Client parti / flux clos : exception rattrapable dédiée. Le handler
         * la filtre par la constante publique httpFluxDéconnecté (dont la valeur
         * EST le symbole du code, cf. HTTP_SERVICE_CONSTS) via
         * `SI ExceptionInfo(errCode) = httpFluxDéconnecté` pour libérer ses
         * ressources ; sans handler, elle remonte et déroule le flux —
         * fermeture abrupte, statut figé. */
        flux_raise(E_NATIVE_HTTP_FLUX_DECONNECTE, "native", "FluxÉ""crit", NULL);
        return NIL_VAL;
    }
    /* emit-and-continue : la native ne rend AUCUNE valeur (≠ RENVOYER). */
    return NIL_VAL;
}

/* ================================================================
 * Registry
 * ================================================================ */

static const OwlParam p_flux_ecrit[] = {
    {"donn\xc3\xa9""e", "tout", "morceau \xc3\xa0 \xc3\xa9mettre dans le flux"},
    {0}
};
static const OwlSignature sig_flux_ecrit[] = {
    {1, p_flux_ecrit, "rien", "n'a pas de valeur de retour : \xc3\xa9met et continue"},
    {0}
};

static const OwlNativeDef HTTP_SERVICE_DEFS[] = {
    {
        .canonical      = "Flux\xc3\x89""crit",         /* FluxÉcrit */
        .canonicalAscii = "FluxEcrit",
        .nameEN         = "StreamWrite",
        .category       = "HTTP",
        .family         = "Service",
        .descFR         = "\xc3\x89met un morceau dans le flux de service en cours et poursuit l'ex\xc3\xa9""cution (\xc3\xa9met-et-continue, distinct de RENVOYER). Hors d'un flux, l'appel l\xc3\xa8ve une exception.",
        .descEN         = "Emit a chunk into the current service stream and keep running (emit-and-continue, distinct from RETURN). Outside a stream, the call raises an exception.",
        .signatures     = sig_flux_ecrit,
        .impl           = fn_flux_ecrit,
    },
};

/* Constantes publiques exposant le code des exceptions de flux.
 * Leur VALEUR est le symbole canonique du code d'erreur — celui que
 * ExceptionInfo(errCode) retourne après une levée depuis FluxÉcrit. Filtrer
 * `SI ExceptionInfo(errCode) = httpFluxDéconnecté` devient donc réel. La valeur
 * est dérivée du registre d'erreurs (pas d'une string en dur -> pas de dérive
 * si le symbole change). Value = OBJ_VAL(string) impossible en init statique :
 * on construit l'OwlConstDef à l'exécution, comme les constantes chaîne du
 * module builtin_types. */
static void register_flux_exception_consts(void) {
    struct { OwlErrorCode code; const char* nameFR; const char* nameEN; const char* descFR; const char* descEN; } specs[] = {
        { E_NATIVE_HTTP_FLUX_DECONNECTE,
          "httpFluxD\xc3\xa9""connect\xc3\xa9", "httpStreamDisconnected",
          "Code de l'exception levée par FluxÉcrit quand le client s'est déconnecté pendant le flux (ou que le délai maximal est atteint) — à comparer à ExceptionInfo(errCode).",
          "Code of the exception raised by StreamWrite when the client disconnected during the stream (or the maximum delay elapsed) — compare to ExceptionInfo(errCode)." },
        { E_NATIVE_HTTP_FLUX_HORS_CONTEXTE,
          "httpFluxHorsContexte", "httpStreamOutOfContext",
          "Code de l'exception levée par FluxÉcrit appelé hors d'un flux de service ouvert (erreur de programmeur) — à comparer à ExceptionInfo(errCode).",
          "Code of the exception raised when StreamWrite is called outside of an open service stream (programmer error) — compare to ExceptionInfo(errCode)." },
    };
    int nb = (int)(sizeof(specs) / sizeof(specs[0]));
    static OwlConstDef consts[2];

    for (int i = 0; i < nb; i++) {
        const OwlErrorDef* d = owl_registry_error_by_code(specs[i].code);
        const char* sym = (d != NULL) ? d->name : "";
        /* Push pour protéger l'ObjString d'un GC déclenché par les allocations
         * internes de register_consts (copie des clés). */
        ObjString* v = obj_copy_string(sym, (int)strlen(sym));
        vm_push(OBJ_VAL(v));
        consts[i].name     = specs[i].nameFR;
        consts[i].category = "HTTP";
        consts[i].value    = OBJ_VAL(v);
        consts[i].descFR   = specs[i].descFR;
        consts[i].descEN   = specs[i].descEN;
        consts[i].nameEN   = specs[i].nameEN;
    }
    owl_registry_register_consts(consts, nb);
    for (int i = 0; i < nb; i++) vm_pop();
}

void stdlib_http_service_init(void) {
    owl_registry_register_module(HTTP_SERVICE_DEFS,
        (int)(sizeof(HTTP_SERVICE_DEFS) / sizeof(HTTP_SERVICE_DEFS[0])));
    register_flux_exception_consts();
}
