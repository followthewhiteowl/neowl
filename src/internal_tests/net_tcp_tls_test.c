// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du wrapper TLS sur OwlTcpSocket.
 *
 * Tests minimaux (CI-friendly, pas de dependance reseau externe) :
 *   - owl_tls_global_init() charge CA bundle + RNG sans erreur
 *   - init/cleanup de OwlTlsSocket idempotents
 *   - connexion impossible (port refused) -> echec propre avec
 *     last_error rempli (chemin d'erreur exerce)
 *
 * Le handshake reel sera teste en Phase 2 contre :
 *   - Soit un mini-serveur TLS local avec cert self-signed
 *   - Soit un vrai endpoint PG en STARTTLS
 * Ces tests-la necessitent infrastructure (cert+key) ou Internet, donc
 * pas dans le self-test interne pour rester deterministe. */

#include "net_tcp_tls.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define TLS_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static void test_global_init(void) {
    TLS_CHECK(owl_tls_global_init(), "global_init OK (CA bundle + RNG)");
    /* Idempotent. */
    TLS_CHECK(owl_tls_global_init(), "global_init idempotent");
}

static void test_init_cleanup(void) {
    OwlTlsSocket s;
    owl_tls_init(&s);
    TLS_CHECK(s.ssl_active == false, "init: ssl_active = false");
    TLS_CHECK(s.tcp.connected == false, "init: tcp.connected = false");
    TLS_CHECK(s.last_error[0] == '\0', "init: last_error vide");
    /* close sur instance non-connectee doit etre idempotent. */
    owl_tls_close(&s);
    /* Re-close : pas de crash. */
    owl_tls_close(&s);
    TLS_CHECK(s.ssl_active == false, "close idempotent");
}

static void test_connect_refused(void) {
    /* Init globale TCP requise. */
    TLS_CHECK(owl_tcp_global_init(), "tcp_global_init");

    OwlTlsSocket s;
    owl_tls_init(&s);
    /* 127.0.0.1:1 : aucun service standard. Connect doit echouer
     * au stade TCP (avant meme le handshake TLS). last_error refletera
     * l'erreur TCP propagee. */
    bool ok = owl_tls_connect(&s, "127.0.0.1", 1, OWL_TLS_VERIFY_FULL, 1000, NULL);
    TLS_CHECK(!ok, "connect port ferme echoue");
    TLS_CHECK(s.last_error[0] != '\0', "last_error rempli sur echec");
    /* Verifie que le message contient la mention TCP (pas TLS). */
    TLS_CHECK(strstr(s.last_error, "tls_connect/tcp") != NULL,
              "erreur identifiee comme TCP-stage");
    owl_tls_close(&s);
    owl_tcp_global_shutdown();
}

int net_tcp_tls_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_global_init();
    test_init_cleanup();
    test_connect_refused();

    printf("net_tcp_tls self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
