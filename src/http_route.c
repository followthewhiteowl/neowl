// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "http_route.h"

#include "owl_errors.h"  /* owl_emit_error_at, Token (via scanner.h) */
#include "scanner.h"     /* scanner_strip_accents */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Cle canon ASCII (accents strippes + minuscule) pour comparer segments,
 * methodes et noms de parametres entre eux. Meme discipline que le reste du
 * DSL de service (comparaison casse/accent-insensible). */
static void route_canon(const char* s, int len, char* out, int cap) {
    char stripped[256];
    int n = scanner_strip_accents(s, len, stripped, sizeof(stripped));
    if (n <= 0) {
        n = len < (int)sizeof(stripped) - 1 ? len : (int)sizeof(stripped) - 1;
        memcpy(stripped, s, (size_t)n);
    }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) {
        char c = stripped[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[i] = c;
    }
    out[n] = '\0';
}

static int seg_is_param(const OwlOperationDecl* op, int i) {
    return op->pathSegmentIsParam != NULL && op->pathSegmentIsParam[i] != 0;
}

/*  — vrai si le segment i est un catch-all glouton (`*<nom>`). */
static int seg_is_catchall(const OwlOperationDecl* op, int i) {
    return op->pathSegmentIsParam != NULL && op->pathSegmentIsParam[i] == 2;
}

/* Token synthetique pointant sur la declaration d'une operation (position de
 * l'erreur : ligne du nom d'operation). */
static Token op_token(const OwlOperationDecl* op) {
    Token t;
    t.start = op->name;
    t.length = op->name != NULL ? (int)strlen(op->name) : 0;
    t.line = op->line;
    t.column = 0;
    t.type = TOKEN_IDENTIFIER;
    return t;
}

/* Label lisible de methode pour le message d'exemple : "httpGet" -> "GET".
 * Repli sur la valeur brute majuscule si le prefixe attendu manque. */
static void method_label(const char* method, char* out, int cap) {
    char canon[64];
    route_canon(method, (int)strlen(method), canon, sizeof(canon));
    const char* rest = canon;
    if (strncmp(canon, "http", 4) == 0) rest += 4;
    int n = (int)strlen(rest);
    if (n <= 0) { rest = canon; n = (int)strlen(canon); }
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) {
        char c = rest[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    out[n] = '\0';
}

/* Construit un chemin d'exemple concret qui matche les deux motifs a et b.
 * A chaque position : le segment litteral s'il existe d'un cote (pour matcher
 * les deux), sinon le nom du parametre du motif a (les deux etant parametres,
 * n'importe quelle valeur matche). Precondition : a et b se recouvrent. */
static void build_example_path(const OwlOperationDecl* a,
                               const OwlOperationDecl* b,
                               char* out, int cap) {
    int pos = 0;
    out[0] = '\0';
    for (int i = 0; i < a->pathSegmentCount && i < b->pathSegmentCount; i++) {
        const char* seg;
        if (!seg_is_param(a, i)) seg = a->pathSegments[i];
        else if (!seg_is_param(b, i)) seg = b->pathSegments[i];
        else seg = a->pathSegments[i];
        int add = (int)strlen(seg) + 1; /* '/' + segment */
        if (pos + add >= cap - 1) break;
        out[pos++] = '/';
        memcpy(out + pos, seg, strlen(seg));
        pos += (int)strlen(seg);
    }
    if (pos == 0 && cap > 1) { out[pos++] = '/'; } /* chemin racine */
    out[pos] = '\0';
}

/* Vrai si a et b se recouvrent : meme longueur et, a chaque position, soit
 * les deux litteraux egaux, soit au moins un parametre. */
static int routes_overlap(const OwlOperationDecl* a, const OwlOperationDecl* b) {
    if (a->pathSegmentCount != b->pathSegmentCount) return 0;
    for (int i = 0; i < a->pathSegmentCount; i++) {
        int ap = seg_is_param(a, i), bp = seg_is_param(b, i);
        if (!ap && !bp) {
            char ka[256], kb[256];
            route_canon(a->pathSegments[i], (int)strlen(a->pathSegments[i]),
                        ka, sizeof(ka));
            route_canon(b->pathSegments[i], (int)strlen(b->pathSegments[i]),
                        kb, sizeof(kb));
            if (strcmp(ka, kb) != 0) return 0; /* litteraux differents */
        }
        /* au moins un parametre : compatible (le parametre matche tout) */
    }
    return 1;
}

/* Vrai si a est STRICTEMENT plus specifique que b : a au moins aussi
 * specifique partout (jamais parametre la ou b est litteral) et strictement
 * plus specifique quelque part (litteral la ou b est parametre).
 * Precondition : meme longueur (routes se recouvrant). */
static int strictly_more_specific(const OwlOperationDecl* a,
                                  const OwlOperationDecl* b) {
    int strictSomewhere = 0;
    for (int i = 0; i < a->pathSegmentCount; i++) {
        int ap = seg_is_param(a, i), bp = seg_is_param(b, i);
        if (ap && !bp) return 0;      /* a moins specifique ici : ne domine pas */
        if (!ap && bp) strictSomewhere = 1;
    }
    return strictSomewhere;
}

/* Controle 1 : chaque segment-parametre de Chemin doit correspondre a un
 * parametre de la signature de surRequete. */
static void check_param_binding(const OwlOperationDecl* op) {
    for (int i = 0; i < op->pathSegmentCount; i++) {
        if (!seg_is_param(op, i)) continue;
        char want[256];
        route_canon(op->pathSegments[i], (int)strlen(op->pathSegments[i]),
                    want, sizeof(want));
        int found = 0;
        for (int p = 0; p < op->procParamCount; p++) {
            char have[256];
            route_canon(op->procParams[p].name,
                        (int)strlen(op->procParams[p].name),
                        have, sizeof(have));
            if (strcmp(have, want) == 0) { found = 1; break; }
        }
        if (!found) {
            Token t = op_token(op);
            owl_emit_error_at(&t, E_SEM_DSL_SERVICE_PATH_PARAM_UNBOUND,
                              "param", op->pathSegments[i],
                              "op", op->name, NULL);
        }
    }
}

/* Vrai si `name` (deja canon) est un segment-parametre du chemin de op. */
static int param_is_path(const OwlOperationDecl* op, const char* wantCanon) {
    for (int i = 0; i < op->pathSegmentCount; i++) {
        if (!seg_is_param(op, i)) continue;
        char have[256];
        route_canon(op->pathSegments[i], (int)strlen(op->pathSegments[i]),
                    have, sizeof(have));
        if (strcmp(have, wantCanon) == 0) return 1;
    }
    return 0;
}

/* Vrai si `name` (deja canon) figure dans la liste Requete (query) de op. */
static int param_is_query(const OwlOperationDecl* op, const char* wantCanon) {
    for (int i = 0; i < op->queryParamCount; i++) {
        char have[256];
        route_canon(op->queryParams[i], (int)strlen(op->queryParams[i]),
                    have, sizeof(have));
        if (strcmp(have, wantCanon) == 0) return 1;
    }
    return 0;
}

/* Controle 3 : chaque param de surRequete se lie a EXACTEMENT une source.
 * Nomme dans Chemin -> chemin ; nomme dans Requete -> query ; le reste doit
 * etre structure (le corps), et il n'y a qu'un seul corps. */
static void check_binding_source(const OwlOperationDecl* op) {
    /* Chemin non fiable (segment invalide deja signale) : ne pas cascader
     * de faux "source inconnue" par-dessus la vraie erreur de chemin. */
    if (op->pathHadError) return;
    int bodyIdx = -1;   /* index du 1er param corps rencontre */
    for (int p = 0; p < op->procParamCount; p++) {
        char want[256];
        route_canon(op->procParams[p].name, (int)strlen(op->procParams[p].name),
                    want, sizeof(want));
        int inPath  = param_is_path(op, want);
        int inQuery = param_is_query(op, want);
        if (inPath && inQuery) {
            /* Deux sources pour un meme param : incoherent (quelle valeur
             * gagne a l'execution ?). Chaque param doit avoir une source. */
            Token t = op_token(op);
            owl_emit_error_at(&t, E_SEM_DSL_SERVICE_BINDING_MULTIPLE_SOURCES,
                              "param", op->procParams[p].name,
                              "op", op->name, NULL);
            continue;
        }
        if (inPath) continue;   /* source = chemin */
        if (inQuery) continue;  /* source = query */

        /*  — un FichierTéléversé est lie a une partie du formulaire
         * multipart (source dediee), jamais un scalaire orphelin. */
        if (op->procParams[p].isFileUpload) continue;
        /* En contexte multipart, un scalaire est un champ texte du formulaire
         * (lie par nom a une partie au serve-time), pas un corps ni un orphelin. */
        if (op->contentTypeMultipart && !op->procParams[p].isStructured) continue;

        /* Reste : candidat corps. Un scalaire n'a pas de source connue. */
        if (!op->procParams[p].isStructured) {
            Token t = op_token(op);
            owl_emit_error_at(&t, E_SEM_DSL_SERVICE_BINDING_SCALAR_NO_SOURCE,
                              "param", op->procParams[p].name,
                              "op", op->name, NULL);
            continue;
        }
        if (bodyIdx < 0) {
            bodyIdx = p;   /* premier corps : OK */
        } else {
            Token t = op_token(op);
            owl_emit_error_at(&t, E_SEM_DSL_SERVICE_BINDING_MULTIPLE_BODIES,
                              "a", op->procParams[bodyIdx].name,
                              "b", op->procParams[p].name,
                              "op", op->name, NULL);
        }
    }
}

/*  — contrat multipart bidirectionnel. Trois refus dedies :
 *  (1) ContentType = multipart declare mais aucun param FichierTéléversé ;
 *  (2) param FichierTéléversé present mais ContentType != multipart ;
 *  (3) param FichierTéléversé coexistant avec un corps de requete structure
 *      (formulaire multipart et corps JSON mutuellement exclusifs).
 * Les trois cas sont disjoints par construction : (3) court-circuite (1)/(2)
 * quand un fichier ET un corps sont presents. */
static void check_multipart_contract(const OwlOperationDecl* op) {
    if (op->pathHadError) return;
    int hasFile = 0;
    int structuredBody = 0;
    for (int p = 0; p < op->procParamCount; p++) {
        if (op->procParams[p].isFileUpload) hasFile = 1;
        else if (op->procParams[p].isStructured) structuredBody = 1;
    }

    if (hasFile && structuredBody) {
        Token t = op_token(op);
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_FILE_WITH_BODY,
                          "op", op->name, NULL);
        return;
    }
    if (op->contentTypeMultipart && !hasFile) {
        Token t = op_token(op);
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_MULTIPART_WITHOUT_FILE,
                          "op", op->name, NULL);
    }
    if (hasFile && !op->contentTypeMultipart) {
        Token t = op_token(op);
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_FILE_WITHOUT_MULTIPART,
                          "op", op->name, NULL);
    }
}

/*  — garde de coherence P1 : un flux (Flux = fluxSSE / fluxOctets) est
 * un mode de REPONSE progressive ; un corps multipart (ContentType = multipart)
 * est un mode de RECEPTION d'upload. Les deux combines sur une meme operation
 * sont incoherents et refuses a la compilation. Les autres combos (Reponses,
 * CodeEtat) restent valides : le statut est fige au 1er chunk, mais un CodeEtat
 * declare reste le statut par defaut avant tout FluxEcrit. */
static void check_flux_contract(const OwlOperationDecl* op) {
    if (op->flux == NULL) return;
    if (op->contentTypeMultipart) {
        Token t = op_token(op);
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_FLUX_WITH_MULTIPART,
                          "op", op->name, NULL);
    }
}

/*  — regle de position du catch-all glouton (Smithy) : AU PLUS un
 * segment catch-all par Chemin, et SEULEMENT en derniere position. Un catch-all
 * capture tous les segments finaux, donc rien ne peut le suivre et deux
 * gloutons sont ambigus. Le message MULTIPLE prime (diagnostic le plus
 * structurant) sur NOT_LAST quand les deux s'appliquent. */
static void check_wildcard_position(const OwlOperationDecl* op) {
    int total = 0;
    int nonFinal = 0;
    for (int i = 0; i < op->pathSegmentCount; i++) {
        if (!seg_is_catchall(op, i)) continue;
        total++;
        if (i != op->pathSegmentCount - 1) nonFinal = 1;
    }
    if (total == 0) return;
    Token t = op_token(op);
    if (total >= 2) {
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_WILDCARD_MULTIPLE,
                          "op", op->name, NULL);
        return;
    }
    if (nonFinal) {
        owl_emit_error_at(&t, E_SEM_DSL_SERVICE_WILDCARD_NOT_LAST,
                          "op", op->name, NULL);
    }
}

void owl_http_route_analyze_service(const OwlServiceDecl* svc) {
    if (svc == NULL) return;

    /*  — Controle 0 : position/unicite du catch-all glouton. */
    for (int i = 0; i < svc->operationCount; i++) {
        check_wildcard_position(svc->operations[i]);
    }

    /* Controle 1 : liaison des parametres de chemin. */
    for (int i = 0; i < svc->operationCount; i++) {
        check_param_binding(svc->operations[i]);
    }

    /* Controle 3 : resolution de source (chemin / query / corps) par param. */
    for (int i = 0; i < svc->operationCount; i++) {
        check_binding_source(svc->operations[i]);
    }

    /*  — contrat multipart <-> FichierTéléversé, bidirectionnel. */
    for (int i = 0; i < svc->operationCount; i++) {
        check_multipart_contract(svc->operations[i]);
    }

    /*  — garde de coherence P1 : Flux incompatible avec multipart. */
    for (int i = 0; i < svc->operationCount; i++) {
        check_flux_contract(svc->operations[i]);
    }

    /* Controle 2 : conflits de routes, comparaison exhaustive par paires.
     * Meme methode requise (la methode fait partie de la cle). */
    for (int i = 0; i < svc->operationCount; i++) {
        OwlOperationDecl* a = svc->operations[i];
        if (a->method == NULL) continue;
        char ma[64];
        route_canon(a->method, (int)strlen(a->method), ma, sizeof(ma));
        for (int j = i + 1; j < svc->operationCount; j++) {
            OwlOperationDecl* b = svc->operations[j];
            if (b->method == NULL) continue;
            char mb[64];
            route_canon(b->method, (int)strlen(b->method), mb, sizeof(mb));
            if (strcmp(ma, mb) != 0) continue; /* methodes distinctes */

            if (!routes_overlap(a, b)) continue;
            if (strictly_more_specific(a, b)) continue;
            if (strictly_more_specific(b, a)) continue;

            /* Ni l'une ni l'autre n'est strictement plus specifique et elles
             * se recouvrent (doublon exact inclus) : conflit. */
            char example[512];
            build_example_path(a, b, example, sizeof(example));
            char label[64];
            method_label(a->method, label, sizeof(label));
            char req[600];
            snprintf(req, sizeof(req), "%s %s", label, example);

            Token t = op_token(b); /* pointe l'operation declaree en second */
            owl_emit_error_at(&t, E_SEM_DSL_SERVICE_ROUTE_CONFLICT,
                              "a", a->name, "b", b->name,
                              "example", req, NULL);
        }
    }
}
