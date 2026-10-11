// SPDX-License-Identifier: AGPL-3.0-only
/* llhttp_smoketest.c — smoke test du vendoring llhttp 9.4.2 (MIT).
 *
 * But : valider que la brique de parsing HTTP/1.1 vendoree compile, linke,
 * et surtout qu'elle est bien STRICTE sur la toolchain OWL (host gcc +
 * cross zig), AVANT que du code OWL la consomme. Un parseur HTTP qui linke
 * mais qui accepte du smuggling ne vaut rien : ce test prouve la strictesse,
 * pas le link.
 *
 * Ici aucun wrapping OWL : la lib tourne sur sa propre machine a etats, on
 * ne touche que l'API publique (llhttp_init / llhttp_execute / llhttp_finish
 * + les callbacks de span). Sanity check isole, sans framework (pattern
 * zint_smoketest / argon2_smoketest / mpdecimal_smoketest).
 *
 * INVARIANT VERIFIE PAR CE TEST : aucun flag LENIENT_* n'est active.
 * `llhttp_init` doit laisser `lenient_flags` a 0, et chacun des cas de rejet
 * ci-dessous correspond a une regle qu'un flag LENIENT_* aurait relachee.
 * Si un jour quelqu'un appelle un `llhttp_set_lenient_*` quelque part, les
 * cas 5 a 15 tombent.
 *
 * Vecteurs : reproduits depuis le corpus de tests upstream (les .md de
 * test/request et test/response, MIT, non vendore car absent du tag
 * release). Le corpus donne les octets bruts ET le code d'erreur attendu ;
 * on rejoue les deux.
 *
 * Verifs (exit code 0 si tout passe, != 0 + message sinon) :
 *   1. lenient_flags == 0 apres init ET apres reset (le defaut est strict).
 *   2. Parse nominal d'une REQUETE (Content-Length, puis chunked) : methode,
 *      version, URL et corps reassemble corrects.
 *   3. Parse nominal d'une REPONSE (Content-Length, puis chunked) : le client
 *      HTTP OWL est le premier consommateur vise, il parse des reponses.
 *   4. Rejets de smuggling / non-conformite, un par regle stricte :
 *      Content-Length + Transfer-Encoding (les deux ordres), Content-Length
 *      duplique, Content-Length non numerique, Content-Length vide, espace
 *      avant les deux-points (CVE-2024-27982), CR seul comme delimiteur
 *      (CVE-2023-30589), LF seul comme delimiteur, `Transfer-Encoding:
 *      chunked` non final (liste et header duplique), version HTTP hors
 *      {0.9, 1.0, 1.1, 2.0}.
 */

#include <stdio.h>
#include <string.h>
#include "llhttp.h"

/* ------------------------------------------------------------------ */
/* Collecte : on agrege les spans pour pouvoir asserter sur le nominal. */

#define CAP 256

typedef struct {
    char url[CAP];
    char status[CAP];
    char body[CAP];
    int  headers_complete;
    int  message_complete;
    /* Les flags sont releves DANS on_headers_complete : llhttp les remet a
       zero quand le message est termine (F_CHUNKED notamment disparait apres
       le dernier chunk), on ne peut pas les lire apres coup. */
    uint16_t flags_at_headers;
    uint64_t content_length_at_headers;
} collect_t;

static int append(char *dst, const char *at, size_t len) {
    size_t cur = strlen(dst);
    if (cur + len + 1 > CAP) return -1;
    memcpy(dst + cur, at, len);
    dst[cur + len] = '\0';
    return 0;
}

static int on_url(llhttp_t *p, const char *at, size_t len) {
    return append(((collect_t *) p->data)->url, at, len);
}
static int on_status(llhttp_t *p, const char *at, size_t len) {
    return append(((collect_t *) p->data)->status, at, len);
}
static int on_body(llhttp_t *p, const char *at, size_t len) {
    return append(((collect_t *) p->data)->body, at, len);
}
static int on_headers_complete(llhttp_t *p) {
    collect_t *c = (collect_t *) p->data;
    c->headers_complete = 1;
    c->flags_at_headers = p->flags;
    c->content_length_at_headers = p->content_length;
    return 0;
}
static int on_message_complete(llhttp_t *p) {
    ((collect_t *) p->data)->message_complete = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Harness. `expected` == HPE_OK signifie "doit parser jusqu'au bout". */

static llhttp_settings_t settings;
static int failures;

#define RUN(type, raw, expected) \
    run_case(__LINE__, #expected, (type), (raw), sizeof(raw) - 1, (expected), NULL)
#define RUN_C(type, raw, expected, out) \
    run_case(__LINE__, #expected, (type), (raw), sizeof(raw) - 1, (expected), (out))

static llhttp_t *run_case(int line, const char *expected_name,
                          llhttp_type_t type, const char *raw, size_t len,
                          llhttp_errno_t expected, collect_t *out) {
    static llhttp_t parser;
    static collect_t collect;

    memset(&collect, 0, sizeof collect);
    llhttp_init(&parser, type, &settings);
    parser.data = &collect;

    /* Le defaut strict ne doit jamais etre relache, y compris apres reset
       (llhttp_reset preserve volontairement lenient_flags : c'est justement
       pour ca qu'il faut verifier qu'ils valent bien 0 des l'init). */
    if (parser.lenient_flags != 0) {
        fprintf(stderr, "L%d: lenient_flags=0x%x apres init, attendu 0 "
                        "(un llhttp_set_lenient_* traine quelque part)\n",
                line, parser.lenient_flags);
        failures++;
        return &parser;
    }
    llhttp_reset(&parser);
    if (parser.lenient_flags != 0) {
        fprintf(stderr, "L%d: lenient_flags=0x%x apres reset, attendu 0\n",
                line, parser.lenient_flags);
        failures++;
        return &parser;
    }

    llhttp_errno_t rc = llhttp_execute(&parser, raw, len);
    if (rc == HPE_OK && expected == HPE_OK) {
        /* Message sans longueur explicite : on ferme proprement sur EOF. */
        if (llhttp_message_needs_eof(&parser)) rc = llhttp_finish(&parser);
    }

    if (rc != expected) {
        fprintf(stderr, "L%d: rc=%s (%s) \"%s\", attendu %s\n",
                line, llhttp_errno_name(rc),
                llhttp_errno_name(llhttp_get_errno(&parser)),
                llhttp_get_error_reason(&parser) ? llhttp_get_error_reason(&parser) : "",
                expected_name);
        failures++;
    } else if (expected == HPE_OK && !collect.message_complete) {
        fprintf(stderr, "L%d: parse OK mais message jamais complete\n", line);
        failures++;
    } else if (expected != HPE_OK && collect.message_complete) {
        fprintf(stderr, "L%d: message complete alors qu'un rejet etait attendu\n", line);
        failures++;
    }

    if (out) *out = collect;
    return &parser;
}

static void expect_str(int line, const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "L%d: %s = \"%s\", attendu \"%s\"\n", line, what, got, want);
        failures++;
    }
}
static void expect_int(int line, const char *what, long got, long want) {
    if (got != want) {
        fprintf(stderr, "L%d: %s = %ld, attendu %ld\n", line, what, got, want);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* 1. Nominal — requetes. */

static void nominal_requests(void) {
    collect_t c;
    llhttp_t *p;

    p = RUN_C(HTTP_REQUEST,
              "POST /api/items?q=1 HTTP/1.1\r\n"
              "Host: localhost:8080\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: 13\r\n"
              "\r\n"
              "{\"ok\":true}\r\n",
              HPE_OK, &c);
    expect_str(__LINE__, "url", c.url, "/api/items?q=1");
    expect_str(__LINE__, "body", c.body, "{\"ok\":true}\r\n");
    expect_int(__LINE__, "method", llhttp_get_method(p), HTTP_POST);
    expect_int(__LINE__, "http_major", llhttp_get_http_major(p), 1);
    expect_int(__LINE__, "http_minor", llhttp_get_http_minor(p), 1);
    expect_int(__LINE__, "content_length annonce",
               (long) c.content_length_at_headers, 13);
    expect_int(__LINE__, "flag F_CONTENT_LENGTH",
               (c.flags_at_headers & F_CONTENT_LENGTH) != 0, 1);
    expect_int(__LINE__, "keep_alive", llhttp_should_keep_alive(p), 1);
    printf("  requete Content-Length OK (POST /api/items?q=1, corps 13 o)\n");

    /* Chunked en entree : deux chunks + terminaison. */
    p = RUN_C(HTTP_REQUEST,
              "PUT /upload HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "Transfer-Encoding: chunked\r\n"
              "\r\n"
              "5\r\nHello\r\n"
              "6\r\n world\r\n"
              "0\r\n"
              "\r\n",
              HPE_OK, &c);
    expect_str(__LINE__, "body chunked", c.body, "Hello world");
    expect_int(__LINE__, "flag F_CHUNKED",
               (c.flags_at_headers & F_CHUNKED) != 0, 1);
    printf("  requete chunked OK (2 chunks reassembles)\n");

    /* GET sans corps : ni CL ni TE, ne doit pas attendre d'EOF. */
    p = RUN_C(HTTP_REQUEST,
              "GET /health HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "\r\n",
              HPE_OK, &c);
    expect_str(__LINE__, "url health", c.url, "/health");
    expect_str(__LINE__, "corps vide", c.body, "");
    expect_int(__LINE__, "needs_eof", llhttp_message_needs_eof(p), 0);
    printf("  requete GET sans corps OK\n");
}

/* ------------------------------------------------------------------ */
/* 2. Nominal — reponses (le client HTTP OWL est le 1er consommateur vise). */

static void nominal_responses(void) {
    collect_t c;
    llhttp_t *p;

    p = RUN_C(HTTP_RESPONSE,
              "HTTP/1.1 201 Created\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 2\r\n"
              "\r\n"
              "hi",
              HPE_OK, &c);
    expect_int(__LINE__, "status_code", llhttp_get_status_code(p), 201);
    expect_str(__LINE__, "status", c.status, "Created");
    expect_str(__LINE__, "body", c.body, "hi");
    printf("  reponse Content-Length OK (201 Created, corps \"hi\")\n");

    p = RUN_C(HTTP_RESPONSE,
              "HTTP/1.1 200 OK\r\n"
              "Transfer-Encoding: chunked\r\n"
              "\r\n"
              "4\r\nWiki\r\n"
              "5\r\npedia\r\n"
              "0\r\n"
              "\r\n",
              HPE_OK, &c);
    expect_int(__LINE__, "status_code", llhttp_get_status_code(p), 200);
    expect_str(__LINE__, "body chunked", c.body, "Wikipedia");
    printf("  reponse chunked OK (2 chunks reassembles)\n");

    /* HTTP/1.0 sans Content-Length : le corps va jusqu'a EOF. llhttp_finish()
       est ce qui distingue "connexion coupee proprement" de "tronquee". */
    p = RUN_C(HTTP_RESPONSE,
              "HTTP/1.0 200 OK\r\n"
              "\r\n"
              "corps jusqu'a la fermeture",
              HPE_OK, &c);
    expect_str(__LINE__, "body EOF-delimite", c.body, "corps jusqu'a la fermeture");
    expect_int(__LINE__, "keep_alive", llhttp_should_keep_alive(p), 0);
    printf("  reponse HTTP/1.0 corps delimite par EOF OK\n");

    /* 204 : pas de corps, meme sans Content-Length. Piege classique des
       parseurs maison, que llhttp gere par la semantique de statut. */
    p = RUN_C(HTTP_RESPONSE,
              "HTTP/1.1 204 No Content\r\n"
              "\r\n",
              HPE_OK, &c);
    expect_int(__LINE__, "status_code", llhttp_get_status_code(p), 204);
    expect_int(__LINE__, "needs_eof sur 204", llhttp_message_needs_eof(p), 0);
    printf("  reponse 204 sans corps OK\n");
}

/* ------------------------------------------------------------------ */
/* 3. Rejets. Une regle stricte par cas ; chacune est relachee par un
   LENIENT_* qu'on n'active pas. */

static void strict_rejections(void) {
    /* Content-Length + Transfer-Encoding simultanes (LENIENT_CHUNKED_LENGTH).
       Le code d'erreur depend de l'ordre d'apparition : c'est le second
       header qui declenche. Les deux ordres sont testes. */
    RUN(HTTP_REQUEST,
        "PUT /url HTTP/1.1\r\n"
        "Content-Length: 1\r\n"
        "Transfer-Encoding: identity\r\n"
        "\r\n\r\n",
        HPE_INVALID_TRANSFER_ENCODING);
    RUN(HTTP_REQUEST,
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "0\r\n\r\n",
        HPE_INVALID_CONTENT_LENGTH);
    printf("  Content-Length + Transfer-Encoding rejete (les 2 ordres)\n");

    /* Content-Length duplique — vecteur de smuggling front/back. */
    RUN(HTTP_REQUEST,
        "PUT /url HTTP/1.1\r\n"
        "Content-Length: 1\r\n"
        "Content-Length: 2\r\n"
        "\r\n",
        HPE_UNEXPECTED_CONTENT_LENGTH);
    printf("  Content-Length duplique rejete\n");

    /* Content-Length non numerique / vide / en overflow. */
    RUN(HTTP_REQUEST,
        "POST / HTTP/1.1\r\n"
        "Content-Length: 4 2\r\n"
        "\r\n\r\n",
        HPE_INVALID_CONTENT_LENGTH);
    RUN(HTTP_REQUEST,
        "POST / HTTP/1.1\r\n"
        "Content-Length: 0x10\r\n"
        "\r\n\r\n",
        HPE_INVALID_CONTENT_LENGTH);
    RUN(HTTP_REQUEST,
        "POST / HTTP/1.1\r\n"
        "Content-Length:\r\n"
        "\r\n\r\n",
        HPE_INVALID_CONTENT_LENGTH);
    RUN(HTTP_REQUEST,
        "PUT /url HTTP/1.1\r\n"
        "Content-Length: 100000000000000000000\r\n"
        "\r\n\r\n",
        HPE_INVALID_CONTENT_LENGTH);
    printf("  Content-Length non numerique / vide / overflow rejetes\n");

    /* Espace avant les deux-points (CVE-2024-27982). Relache par
       LENIENT_HEADERS — et par LUI SEUL, ce n'est PAS durci en dur. */
    RUN(HTTP_REQUEST,
        "PUT /url HTTP/1.1\r\n"
        "Connection: upgrade\r\n"
        "Content-Length : 4\r\n"
        "Upgrade: ws\r\n"
        "\r\n"
        "abcdefgh",
        HPE_INVALID_HEADER_TOKEN);
    printf("  espace avant les deux-points rejete (CVE-2024-27982)\n");

    /* CR seul comme delimiteur (CVE-2023-30589) — LENIENT_OPTIONAL_LF_AFTER_CR. */
    RUN(HTTP_REQUEST,
        "GET / HTTP/1.1\r\n"
        "Foo: 1\rBar: 2\r\n"
        "\r\n",
        HPE_LF_EXPECTED);
    printf("  CR seul comme delimiteur rejete (CVE-2023-30589)\n");

    /* LF seul comme delimiteur — LENIENT_OPTIONAL_CR_BEFORE_LF. Le vecteur
       upstream cache un Transfer-Encoding derriere le LF : smuggling type. */
    RUN(HTTP_REQUEST,
        "POST / HTTP/1.1\r\n"
        "Host: localhost:5000\r\n"
        "x:x\nTransfer-Encoding: chunked\r\n"
        "\r\n"
        "1\r\nA\r\n0\r\n\r\n",
        HPE_CR_EXPECTED);
    printf("  LF seul comme delimiteur rejete\n");

    /* `chunked` non final — LENIENT_TRANSFER_ENCODING. Liste dans un seul
       header, puis reparti sur deux headers (joints en interne par ", "). */
    RUN(HTTP_REQUEST,
        "POST /body HTTP/1.1\r\n"
        "Accept: */*\r\n"
        "Transfer-Encoding: chunked, deflate\r\n"
        "\r\n"
        "World",
        HPE_INVALID_TRANSFER_ENCODING);
    RUN(HTTP_REQUEST,
        "POST /body HTTP/1.1\r\n"
        "Accept: */*\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Transfer-Encoding: deflate\r\n"
        "\r\n"
        "World",
        HPE_INVALID_TRANSFER_ENCODING);
    printf("  Transfer-Encoding chunked non final rejete (liste + duplique)\n");

    /* Version hors {0.9, 1.0, 1.1, 2.0} — LENIENT_VERSION. */
    RUN(HTTP_REQUEST, "GET / HTTP/5.6\r\n\r\n", HPE_INVALID_VERSION);
    RUN(HTTP_RESPONSE, "HTTP/5.6 200 OK\r\n\r\n", HPE_INVALID_VERSION);
    printf("  version HTTP hors {0.9,1.0,1.1,2.0} rejetee\n");

    /* Token de nom de header invalide + obs-fold en position de nom. */
    RUN(HTTP_REQUEST,
        "GET / HTTP/1.1\r\n"
        "Fo@: Failure\r\n"
        "\r\n",
        HPE_INVALID_HEADER_TOKEN);
    RUN(HTTP_REQUEST,
        "GET / HTTP/1.1\r\n"
        "name\r\n : value\r\n"
        "\r\n",
        HPE_INVALID_HEADER_TOKEN);
    printf("  nom de header invalide + repli de ligne rejetes\n");

    /* Caractere de controle dans une valeur de header
       (LENIENT_HEADER_VALUE_RELAXED : \f = 0x0c). */
    RUN(HTTP_REQUEST,
        "GET /url HTTP/1.1\r\n"
        "Header1: \f\r\n"
        "\r\n",
        HPE_INVALID_HEADER_TOKEN);
    printf("  caractere de controle en valeur de header rejete\n");
}

int main(void) {
    llhttp_settings_init(&settings);
    settings.on_url = on_url;
    settings.on_status = on_status;
    settings.on_body = on_body;
    settings.on_headers_complete = on_headers_complete;
    settings.on_message_complete = on_message_complete;

    printf("llhttp %d.%d.%d — parse nominal\n",
           LLHTTP_VERSION_MAJOR, LLHTTP_VERSION_MINOR, LLHTTP_VERSION_PATCH);
    nominal_requests();
    nominal_responses();
    printf("llhttp — rejets strictes (aucun flag LENIENT_* active)\n");
    strict_rejections();

    if (failures) {
        fprintf(stderr, "llhttp_smoketest: %d echec(s)\n", failures);
        return 1;
    }
    printf("llhttp_smoketest OK (parse requete + reponse, 17 rejets stricts)\n");
    return 0;
}
