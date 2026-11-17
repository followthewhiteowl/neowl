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
 * La famille d'événements SSE nommés (FluxÉvénement / FluxIdentifiant /
 * FluxCommentaire) se pose sur le même helper interne flux_courant_emit, via un
 * paramètre de genre : event: <nom>, id: <val>, : <commentaire>. */

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

/* Genre de morceau SSE à émettre. FluxÉcrit produit un bloc `data:` auto-terminé
 * (ligne vide de fin) ; FluxÉvénement le préfixe d'une ligne `event: <nom>` ;
 * FluxIdentifiant et FluxCommentaire émettent une ligne SSE unique (`id: <val>`,
 * `: <val>`) SANS ligne vide de terminaison. */
typedef enum {
    FLUX_GENRE_DATA = 0,   /* FluxÉcrit */
    FLUX_GENRE_EVENT,      /* FluxÉvénement */
    FLUX_GENRE_ID,         /* FluxIdentifiant */
    FLUX_GENRE_COMMENT     /* FluxCommentaire */
} FluxGenre;

/* Framing d'un morceau déjà extrait en octets C, puis poussée (COPIE côté
 * transport). `nom`/`nomLen` ne servent qu'au genre EVENT (ligne `event:`). */
static OwlHttpFluxStatut flux_courant_emit(OwlHttpRep* rep, int mode, FluxGenre genre,
                                           const unsigned char* nom, int nomLen,
                                           const unsigned char* payload, int len) {
    /* Octets bruts : seul FluxÉcrit a un sens hors encadrement SSE, il pousse le
     * payload tel quel. Les lignes nommées restent du SSE en toute circonstance
     * (elles n'existent que pour ce format). */
    if (mode == OWL_HTTP_FLUX_MODE_OCTETS && genre == FLUX_GENRE_DATA) {
        return owl_http_rep_flux_pousse(rep, (const char*)payload, (size_t)len);
    }

    /* id: / : (commentaire) — une ligne SSE unique, émise verbatim, close par un
     * seul '\n' (pas de ligne vide de terminaison). Payload vide -> préfixe sans
     * espace superflu (`id:\n` / `:\n`). */
    if (genre == FLUX_GENRE_ID || genre == FLUX_GENRE_COMMENT) {
        const char* pref;
        size_t preflen;
        if (genre == FLUX_GENRE_ID) { pref = len > 0 ? "id: " : "id:"; preflen = len > 0 ? 4 : 3; }
        else                        { pref = len > 0 ? ": "   : ":";   preflen = len > 0 ? 2 : 1; }
        size_t taille = preflen + (size_t)len + 1;
        char* buf = (char*)malloc(taille);
        if (!buf) return OWL_HTTP_FLUX_ABANDON;
        size_t o = 0;
        memcpy(buf + o, pref, preflen); o += preflen;
        memcpy(buf + o, payload, (size_t)len); o += (size_t)len;
        buf[o++] = '\n';
        OwlHttpFluxStatut st = owl_http_rep_flux_pousse(rep, buf, taille);
        free(buf);
        return st;
    }

    /* DATA / EVENT : la spec veut CHAQUE ligne d'un événement re-préfixée
     * `data: `. On découpe le payload sur '\n' et on émet `data: <ligne>\n` par
     * ligne (ligne vide -> `data:\n`, sans espace superflu), l'événement se
     * termine par un `\n` final (ligne vide SSE). Le genre EVENT ajoute en tête
     * une ligne `event: <nom>\n`. Un seul tampon, taille calculée en une passe. */
    size_t taille = 1; /* le '\n' de fin d'événement */
    if (genre == FLUX_GENRE_EVENT) {
        taille += (size_t)(nomLen > 0 ? 7 : 6) + (size_t)nomLen + 1; /* "event: "/"event:" + nom + \n */
    }
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
    if (genre == FLUX_GENRE_EVENT) {
        if (nomLen > 0) {
            memcpy(buf + o, "event: ", 7); o += 7;
            memcpy(buf + o, nom, (size_t)nomLen); o += (size_t)nomLen;
        } else {
            memcpy(buf + o, "event:", 6); o += 6;
        }
        buf[o++] = '\n';
    }
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

/* Lève une exception rattrapable depuis une native de flux avec un code canon du
 * registre ( P4). errCode = symbole "E_..." -> filtrable par
 * ExceptionInfo(errCode). `native` = nom canonique affiché (origine + {native}
 * du template, langue active). */
static void flux_raise(const char* native, OwlErrorCode code, ...) {
    va_list ap;
    va_start(ap, code);
    const char* msg = owl_error_render_active_va(code, ap);
    va_end(ap);
    const OwlErrorDef* d = owl_registry_error_by_code(code);
    owl_vm_raise_native(native, d != NULL ? d->name : "",
                        msg != NULL ? msg : "");
}

/* Extrait les octets d'une valeur `tout` : chaîne / Buffer -> octets bruts ;
 * toute autre valeur -> JSON minifié (même chemin que le fallback JSON de
 * RENVOYER, §6). En cas de JSON, *owned pointe le tampon possédé à libérer après
 * l'émission. Renvoie 0 si la valeur n'est pas sérialisable (rare). */
static int flux_payload_bytes(Value v, const unsigned char** payload, int* len, char** owned) {
    *owned = NULL;
    if (value_as_bytes(v, payload, len)) return 1;
    size_t jlen = 0;
    char* json_buf = owl_json_serialize_value_compact(v, &jlen);
    if (json_buf == NULL) return 0;
    *owned   = json_buf;
    *payload = (const unsigned char*)json_buf;
    *len     = (int)jlen;
    return 1;
}

/* Cœur partagé des natives de flux : résout le flux de service courant, extrait
 * les octets du payload puis émet selon le genre. Hors flux (slot vide /
 * ouverture impossible) : erreur de programmeur, code exposé par la constante
 * httpFluxHorsContexte. Client déconnecté (ABANDON) : httpFluxDéconnecté,
 * rattrapable — le handler se déroule et le framework nettoie. Les deux
 * constantes sont filtrables via ExceptionInfo(errCode) — cf. HTTP_SERVICE_CONSTS.
 * `nom`/`nomLen` ne servent qu'au genre EVENT. emit-and-continue : AUCUNE valeur
 * de retour (≠ RENVOYER). */
static Value flux_emit_commun(const char* native, FluxGenre genre,
                              const unsigned char* nom, int nomLen, Value dataVal) {
    void* slot = owl_vm_flux_courant_get();
    if (slot == NULL) {
        flux_raise(native, E_NATIVE_HTTP_FLUX_HORS_CONTEXTE, "native", native, NULL);
        return NIL_VAL;
    }
    OwlHttpProdCtx* ctx = (OwlHttpProdCtx*)slot;

    OwlHttpRep* rep = owl_http_prod_flux_assure(ctx);
    if (rep == NULL) {
        flux_raise(native, E_NATIVE_HTTP_FLUX_HORS_CONTEXTE, "native", native, NULL);
        return NIL_VAL;
    }

    const unsigned char* payload = NULL;
    int len = 0;
    char* owned = NULL;
    if (!flux_payload_bytes(dataVal, &payload, &len, &owned)) {
        flux_raise(native, E_NATIVE_ARG_INVALID, "native", native, NULL);
        return NIL_VAL;
    }

    OwlHttpFluxStatut st = flux_courant_emit(rep, owl_http_prod_flux_mode(ctx), genre,
                                             nom, nomLen, payload, len);
    free(owned);
    if (st == OWL_HTTP_FLUX_ABANDON) {
        /* Client parti / flux clos : exception rattrapable dédiée (filtrée par
         * httpFluxDéconnecté) ; sans handler, elle remonte et déroule le flux —
         * fermeture abrupte, statut figé. */
        flux_raise(native, E_NATIVE_HTTP_FLUX_DECONNECTE, "native", native, NULL);
        return NIL_VAL;
    }
    return NIL_VAL;
}

/* FluxÉcrit(donnée) — émet un morceau `data:` dans le flux de service courant. */
static Value fn_flux_ecrit(int argCount, Value* args) {
    (void)argCount;
    return flux_emit_commun("Flux\xc3\x89""crit", FLUX_GENRE_DATA, NULL, 0, args[0]);
}

/* FluxÉvénement(nom, donnée) — émet un événement SSE nommé : ligne
 * `event: <nom>` suivie du (des) `data:` du payload. */
static Value fn_flux_evenement(int argCount, Value* args) {
    (void)argCount;
    const unsigned char* nom = NULL;
    int nomLen = 0;
    if (!value_as_bytes(args[0], &nom, &nomLen)) {
        flux_raise("Flux\xc3\x89v\xc3\xa9nement", E_NATIVE_ARG_INVALID,
                   "native", "Flux\xc3\x89v\xc3\xa9nement", NULL);
        return NIL_VAL;
    }
    return flux_emit_commun("Flux\xc3\x89v\xc3\xa9nement", FLUX_GENRE_EVENT, nom, nomLen, args[1]);
}

/* FluxIdentifiant(id) — émet une ligne SSE `id: <val>` (reprise de flux côté
 * client via Last-Event-ID). */
static Value fn_flux_identifiant(int argCount, Value* args) {
    (void)argCount;
    return flux_emit_commun("FluxIdentifiant", FLUX_GENRE_ID, NULL, 0, args[0]);
}

/* FluxCommentaire(texte) — émet une ligne de commentaire SSE `: <texte>`
 * (ignorée par le client, utile en battement de cœur anti-timeout). */
static Value fn_flux_commentaire(int argCount, Value* args) {
    (void)argCount;
    return flux_emit_commun("FluxCommentaire", FLUX_GENRE_COMMENT, NULL, 0, args[0]);
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

static const OwlParam p_flux_evenement[] = {
    {"nom", "cha\xc3\xae""ne", "nom de l'\xc3\xa9v\xc3\xa9nement (ligne event:)"},
    {"donn\xc3\xa9""e", "tout", "charge utile \xc3\xa9mise en data: sous l'\xc3\xa9v\xc3\xa9nement"},
    {0}
};
static const OwlSignature sig_flux_evenement[] = {
    {2, p_flux_evenement, "rien", "n'a pas de valeur de retour : \xc3\xa9met et continue"},
    {0}
};

static const OwlParam p_flux_identifiant[] = {
    {"id", "tout", "identifiant de l'\xc3\xa9v\xc3\xa9nement (ligne id:)"},
    {0}
};
static const OwlSignature sig_flux_identifiant[] = {
    {1, p_flux_identifiant, "rien", "n'a pas de valeur de retour : \xc3\xa9met et continue"},
    {0}
};

static const OwlParam p_flux_commentaire[] = {
    {"texte", "cha\xc3\xae""ne", "texte du commentaire (ligne :)"},
    {0}
};
static const OwlSignature sig_flux_commentaire[] = {
    {1, p_flux_commentaire, "rien", "n'a pas de valeur de retour : \xc3\xa9met et continue"},
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
    {
        .canonical      = "Flux\xc3\x89v\xc3\xa9nement",   /* FluxÉvénement */
        .canonicalAscii = "FluxEvenement",
        .nameEN         = "StreamEvent",
        .category       = "HTTP",
        .family         = "Service",
        .descFR         = "\xc3\x89met un \xc3\xa9v\xc3\xa9nement nomm\xc3\xa9 dans le flux de service en cours : une ligne event: <nom> suivie de la charge utile en data:, puis poursuit l'ex\xc3\xa9""cution. Hors d'un flux, l'appel l\xc3\xa8ve une exception.",
        .descEN         = "Emit a named event into the current service stream: an event: <name> line followed by the payload as data:, then keep running. Outside a stream, the call raises an exception.",
        .signatures     = sig_flux_evenement,
        .impl           = fn_flux_evenement,
    },
    {
        .canonical      = "FluxIdentifiant",
        .canonicalAscii = "FluxIdentifiant",
        .nameEN         = "StreamId",
        .category       = "HTTP",
        .family         = "Service",
        .descFR         = "\xc3\x89met une ligne id: dans le flux de service en cours, qui identifie l'\xc3\xa9v\xc3\xa9nement courant pour permettre au client de reprendre apr\xc3\xa8s coupure, puis poursuit l'ex\xc3\xa9""cution. Hors d'un flux, l'appel l\xc3\xa8ve une exception.",
        .descEN         = "Emit an id: line into the current service stream, identifying the current event so the client can resume after a drop, then keep running. Outside a stream, the call raises an exception.",
        .signatures     = sig_flux_identifiant,
        .impl           = fn_flux_identifiant,
    },
    {
        .canonical      = "FluxCommentaire",
        .canonicalAscii = "FluxCommentaire",
        .nameEN         = "StreamComment",
        .category       = "HTTP",
        .family         = "Service",
        .descFR         = "\xc3\x89met une ligne de commentaire (:) dans le flux de service en cours, ignor\xc3\xa9""e par le client et utile comme battement de c\xc5\x93ur pour maintenir la connexion, puis poursuit l'ex\xc3\xa9""cution. Hors d'un flux, l'appel l\xc3\xa8ve une exception.",
        .descEN         = "Emit a comment line (:) into the current service stream, ignored by the client and useful as a heartbeat to keep the connection alive, then keep running. Outside a stream, the call raises an exception.",
        .signatures     = sig_flux_commentaire,
        .impl           = fn_flux_commentaire,
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
