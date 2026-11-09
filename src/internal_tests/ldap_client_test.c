// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du client LDAP (ldap_client.{c,h}) contre le mock in-process
 * ldap_mock_server.
 *
 * Execute via `owl --self-test-net` (famille reseau). Hermetique : le mock
 * ecoute sur un port ephemere loopback, aucun reseau externe.
 *
 * Scenario nominal (une session) :
 *   - simple bind avec les bons identifiants -> resultCode 0 ;
 *   - recherche "(sAMAccountName=bob)" sous un baseDN (scope subtree)
 *     -> l'entree "bob" (DN attendu) ;
 *   - lecture des attributs du DN "bob" (op scope base, filtre
 *     "(objectClass=*)") -> displayName mono + memberOf multi-value
 *     (ordre serveur preserve, octets UTF-8 bruts) ;
 *   - fermeture propre (UnbindRequest + socket).
 * Scenario d'echec (autre session) :
 *   - simple bind avec un mauvais mot de passe -> resultCode 49.
 */

#include "ldap_client.h"
#include "ldap_mock_server.h"
#include "net_tcp.h"
#include "net_tcp_server.h"
#include "net_tls_server.h"

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE mtls_thread_t;
#else
  #include <pthread.h>
  #include <time.h>
  #include <sys/socket.h>   /* send()/recv() — explicite pour musl strict (masque par winsock/glibc) */
  typedef pthread_t mtls_thread_t;
#endif

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

static const char* kBindDn   = "CN=admin,DC=example,DC=com";
static const char* kBindPw    = "s3cret";
static const char* kBobDn     = "CN=bob,OU=users,DC=example,DC=com";

static const OwlLdapAttribute* find_attr(const OwlLdapSearchEntry* e,
                                         const char* name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < e->attribute_count; i++) {
        if (e->attributes[i].type_len == n
            && memcmp(e->attributes[i].type, name, n) == 0)
            return &e->attributes[i];
    }
    return NULL;
}

static bool val_eq(const OwlLdapValue* v, const char* s) {
    size_t n = strlen(s);
    return v->len == n && memcmp(v->value, s, n) == 0;
}

/* ================================================================
 * Scenario nominal : bind ok, search, read object, close.
 * ================================================================ */

static void test_session_nominale(void) {
    LdapMockConfig cfg;
    ldap_mock_config_defaut(&cfg);
    cfg.bind_dn = kBindDn;
    cfg.bind_password = kBindPw;

    LdapMockServer* srv = ldap_mock_server_start(&cfg);
    CHECK(srv != NULL, "nominal: mock demarre");
    if (!srv) return;
    CHECK(ldap_mock_server_is_ready(srv), "nominal: mock pret");
    int port = ldap_mock_server_port(srv);

    OwlLdapClient c;
    owl_ldap_client_init(&c);

    int rc = -1;
    bool ok = owl_ldap_client_connect(&c, "127.0.0.1", port,
                                      false, 0, NULL, NULL, NULL, kBindDn,
                                      (const unsigned char*)kBindPw, strlen(kBindPw),
                                      false, 3, 2000, &rc);
    CHECK(ok, "nominal: connect+bind transport ok");
    CHECK(ok && rc == 0, "nominal: bind resultCode 0 (succes)");

    /* Recherche par filtre sous le baseDN. */
    const char* attrs[] = { "sAMAccountName", "displayName", "memberOf" };
    const OwlLdapSearchResult* r = owl_ldap_client_search(
        &c, "DC=example,DC=com", OWL_LDAP_SCOPE_WHOLE_SUBTREE,
        "(sAMAccountName=bob)", attrs, 3);
    CHECK(r != NULL, "nominal: search renvoie un resultat");
    if (r) {
        CHECK(r->done && r->resultCode == 0, "nominal: search done rc=0");
        CHECK(r->entry_count == 1, "nominal: 1 entree");
        if (r->entry_count == 1) {
            const OwlLdapSearchEntry* e = &r->entries[0];
            CHECK(e->dn_len == strlen(kBobDn)
                     && memcmp(e->dn, kBobDn, e->dn_len) == 0,
                  "nominal: DN de l'entree = bob");
        }
    }

    /* Lecture des attributs du DN "bob" (op derriere LDAPValeur/NbValeur). */
    const char* rattrs[] = { "displayName", "memberOf" };
    const OwlLdapSearchResult* ro = owl_ldap_client_read_object(
        &c, kBobDn, rattrs, 2);
    CHECK(ro != NULL, "nominal: read_object renvoie un resultat");
    if (ro && ro->entry_count == 1) {
        const OwlLdapSearchEntry* e = &ro->entries[0];

        /* mono-value */
        const OwlLdapAttribute* disp = find_attr(e, "displayName");
        CHECK(disp != NULL, "nominal: displayName present");
        CHECK(disp && disp->value_count == 1, "nominal: displayName mono-value");
        CHECK(disp && disp->value_count == 1 && val_eq(&disp->values[0], "Bob Smith"),
              "nominal: displayName = Bob Smith");

        /* multi-value, ordre serveur preserve */
        const OwlLdapAttribute* mo = find_attr(e, "memberOf");
        CHECK(mo != NULL, "nominal: memberOf present");
        CHECK(mo && mo->value_count == 2, "nominal: memberOf 2 valeurs");
        CHECK(mo && mo->value_count == 2
                 && val_eq(&mo->values[0], "CN=Admins,DC=example,DC=com"),
              "nominal: memberOf[0] = Admins (ordre preserve)");
        CHECK(mo && mo->value_count == 2
                 && val_eq(&mo->values[1], "CN=Users,DC=example,DC=com"),
              "nominal: memberOf[1] = Users (ordre preserve)");
    } else {
        CHECK(false, "nominal: read_object entry_count == 1");
    }

    owl_ldap_client_close(&c);

    ldap_mock_server_join(srv);
    CHECK(ldap_mock_server_saw_bind(srv) == 1, "nominal: mock a vu 1 bind");
    CHECK(ldap_mock_server_search_count(srv) == 2, "nominal: mock a vu 2 search");
    CHECK(ldap_mock_server_saw_unbind(srv) == 1, "nominal: mock a vu l'unbind");
    CHECK(strcmp(ldap_mock_server_last_bind_dn(srv), kBindDn) == 0,
          "nominal: mock a recu le bon bind DN");
    ldap_mock_server_free(srv);
}

/* ================================================================
 * Scenario d'echec : mauvais mot de passe -> resultCode 49.
 * ================================================================ */

static void test_bind_mauvais_mdp(void) {
    LdapMockConfig cfg;
    ldap_mock_config_defaut(&cfg);
    cfg.bind_dn = kBindDn;
    cfg.bind_password = kBindPw;

    LdapMockServer* srv = ldap_mock_server_start(&cfg);
    CHECK(srv != NULL, "echec: mock demarre");
    if (!srv) return;
    int port = ldap_mock_server_port(srv);

    OwlLdapClient c;
    owl_ldap_client_init(&c);

    int rc = -1;
    bool ok = owl_ldap_client_connect(&c, "127.0.0.1", port,
                                      false, 0, NULL, NULL, NULL, kBindDn,
                                      (const unsigned char*)"mauvais", 7,
                                      false, 3, 2000, &rc);
    /* transport ok, bind refuse : connect renvoie true + rc=49. */
    CHECK(ok, "echec: connect transport ok malgre bind refuse");
    CHECK(ok && rc == 49, "echec: bind resultCode 49 (invalidCredentials)");

    owl_ldap_client_close(&c);
    ldap_mock_server_join(srv);
    CHECK(ldap_mock_server_saw_bind(srv) == 1, "echec: mock a vu 1 bind");
    ldap_mock_server_free(srv);
}

/* ================================================================
 * Scenario TLS : la confiance (verify_level + extra_ca_path) gate
 * REELLEMENT le handshake LDAPS.
 *
 * On monte un endpoint TLS auto-signe (CN=localhost, hors bundle Mozilla)
 * et on pointe le client dessus en LDAPS. Le serveur se contente du
 * handshake puis coupe — c'est le handshake qu'on observe, pas le dialogue
 * LDAP. Discriminant : last_error contient "TLS" ssi c'est owl_tls_connect
 * qui a refuse. Trois sorties pour les deux parametres :
 *   - FULL sans CA           -> handshake REFUSE  (cert non trustable)  ;
 *   - NONE                   -> handshake ACCEPTE (verif desactivee)    ;
 *   - FULL + CA = le cert    -> handshake ACCEPTE (CA custom honoree).
 * ================================================================ */

static char g_cert_pem[4096];
static char g_cle_pem[2048];

static bool ldap_tls_genere_cert(void) {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       cle;
    mbedtls_x509write_cert   crt;
    unsigned char            serie[1] = { 1 };
    bool                     ok = false;

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&cle);
    mbedtls_x509write_crt_init(&crt);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-ldap-tls", 12) != 0) goto fin;
    if (mbedtls_pk_setup(&cle, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto fin;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(cle),
                            mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    if (mbedtls_pk_write_key_pem(&cle, (unsigned char*)g_cle_pem,
                                 sizeof(g_cle_pem)) != 0) goto fin;

    mbedtls_x509write_crt_set_subject_key(&crt, &cle);
    mbedtls_x509write_crt_set_issuer_key(&crt, &cle);
    if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=localhost") != 0) goto fin;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=localhost") != 0) goto fin;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serie, sizeof(serie)) != 0) goto fin;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000",
                                           "20350101000000") != 0) goto fin;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, -1) != 0) goto fin;
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)g_cert_pem, sizeof(g_cert_pem),
                                  mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    ok = true;
fin:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&cle);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* Service minimal : le handshake est deja fait par le pool avant cet appel.
 * On coupe aussitot — seule l'issue du handshake nous interesse. */
static void ldap_tls_sert(OwlTcpConn* conn, void* user) {
    (void)user;
    owl_tcp_conn_ferme(conn);
}

/* Ouvre en LDAPS vers l'endpoint `port`, renvoie last_error (le client est
 * local, ferme a la sortie). *out_ok = valeur de retour du connect. */
static void ldap_tls_essai(int port, int verify_level, const char* extra_ca,
                           bool* out_ok, char* err, size_t err_len) {
    OwlLdapClient c;
    owl_ldap_client_init(&c);
    int rc = -1;
    bool ok = owl_ldap_client_connect(&c, "localhost", port,
                                      true, verify_level, extra_ca,
                                      NULL, NULL,
                                      "", (const unsigned char*)"", 0,
                                      false, 3, 2000, &rc);
    snprintf(err, err_len, "%s", owl_ldap_client_last_error(&c));
    *out_ok = ok;
    owl_ldap_client_close(&c);
}

static void test_tls_confiance(void) {
    if (!ldap_tls_genere_cert()) { CHECK(false, "tls: cert auto-signe genere"); return; }

    char merr[256] = {0};
    OwlTlsServeurMateriel* mat =
        owl_tls_serveur_materiel_cree(g_cert_pem, g_cle_pem, merr, sizeof(merr));
    CHECK(mat != NULL, "tls: materiel serveur construit");
    if (!mat) return;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre_tls(&l, "127.0.0.1", 0, 8, mat)) {
        CHECK(false, "tls: listener ouvert");
        owl_tls_serveur_materiel_libere(mat);
        return;
    }
    int port = owl_tcp_listener_port(&l);

    OwlTcpPoolConfig pc;
    owl_tcp_pool_config_defaut(&pc);
    pc.nb_workers               = 2;
    pc.connexions_max           = 2;
    pc.tls                      = mat;
    pc.tls_handshake_timeout_ms = 3000;
    OwlTcpPool* pool = owl_tcp_pool_demarre(&l, &pc, ldap_tls_sert, NULL);
    CHECK(pool != NULL, "tls: pool demarre");
    if (!pool) { owl_tcp_listener_ferme(&l); owl_tls_serveur_materiel_libere(mat); return; }

    /* Cert personnalise ecrit sur disque : owl_tls_connect prend un CHEMIN. */
    const char* ca_file = "owl_ldap_tls_test_ca.pem";
    FILE* f = fopen(ca_file, "wb");
    bool ca_ecrit = false;
    if (f) {
        ca_ecrit = fwrite(g_cert_pem, 1, strlen(g_cert_pem), f) == strlen(g_cert_pem);
        fclose(f);
    }
    CHECK(ca_ecrit, "tls: CA personnalisee ecrite sur disque");

    bool ok; char err[256];

    /* (a) FULL sans CA : cert auto-signe absent du bundle -> handshake refuse. */
    ldap_tls_essai(port, OWL_TLS_VERIFY_FULL, NULL, &ok, err, sizeof(err));
    CHECK(!ok, "tls: FULL sans CA echoue");
    CHECK(strstr(err, "TLS") != NULL, "tls: FULL sans CA echoue AU handshake TLS");

    /* (b) NONE : verification desactivee -> handshake accepte (echec ulterieur
     *     au bind, hors transport TLS). */
    ldap_tls_essai(port, OWL_TLS_VERIFY_NONE, NULL, &ok, err, sizeof(err));
    CHECK(strstr(err, "TLS") == NULL,
          "tls: NONE franchit le handshake (echec hors TLS)");

    /* (c) FULL + CA = le cert lui-meme -> handshake accepte (CA custom honoree,
     *     CN=localhost matche l'hote). Preuve que extra_ca_path est propage. */
    if (ca_ecrit) {
        ldap_tls_essai(port, OWL_TLS_VERIFY_FULL, ca_file, &ok, err, sizeof(err));
        CHECK(strstr(err, "TLS") == NULL,
              "tls: FULL + CA custom franchit le handshake");
    }

    owl_tcp_pool_arrete(pool);
    owl_tcp_listener_ferme(&l);
    owl_tls_serveur_materiel_libere(mat);
    if (ca_ecrit) remove(ca_file);
}

/* ================================================================
 * Scenario mTLS : un endpoint TLS qui EXIGE un certificat client
 * (authmode REQUIRED cote serveur, CA qui valide le cert client).
 *
 * Prouve que l'enabler cert client de net_tcp_tls est reellement cable
 * jusqu'au client LDAP (chemin authExterne) :
 *   - AVEC cert client valide  -> handshake ACCEPTE (le refus ulterieur
 *     vient du bind, pas du transport : last_error ne mentionne pas "TLS") ;
 *   - SANS cert client          -> handshake REFUSE au transport
 *     (last_error mentionne "TLS").
 * Le serveur se contente du handshake puis coupe — c'est le handshake qu'on
 * observe. Meme discriminant que test_tls_confiance.
 *
 * On genere une petite PKI : une CA auto-signee, un certificat client signe
 * par cette CA (presente par le client), et on reutilise le cert serveur
 * auto-signe (CN=localhost) comme identite serveur (le client est en
 * VERIFY_NONE cote serveur, il ne valide donc pas ce cert).
 * ================================================================ */

static char g_ca_cert_pem[4096];
static char g_ca_key_pem[2048];
static char g_client_cert_pem[4096];
static char g_client_key_pem[2048];

static bool mtls_gen_keypair(mbedtls_pk_context* key,
                             mbedtls_ctr_drbg_context* drbg,
                             char* key_pem, size_t key_pem_len) {
    if (mbedtls_pk_setup(key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) return false;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*key),
                            mbedtls_ctr_drbg_random, drbg) != 0) return false;
    if (mbedtls_pk_write_key_pem(key, (unsigned char*)key_pem, key_pem_len) != 0) return false;
    return true;
}

static bool mtls_write_cert(const char* subject, const char* issuer,
                            mbedtls_pk_context* subject_key,
                            mbedtls_pk_context* issuer_key,
                            bool is_ca, unsigned char serial,
                            mbedtls_ctr_drbg_context* drbg,
                            char* cert_pem, size_t cert_pem_len) {
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    bool ok = false;
    unsigned char serie[1] = { serial };
    mbedtls_x509write_crt_set_subject_key(&crt, subject_key);
    mbedtls_x509write_crt_set_issuer_key(&crt, issuer_key);
    if (mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0) goto fin;
    if (mbedtls_x509write_crt_set_issuer_name(&crt, issuer) != 0) goto fin;
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    if (mbedtls_x509write_crt_set_serial_raw(&crt, serie, sizeof serie) != 0) goto fin;
    if (mbedtls_x509write_crt_set_validity(&crt, "20250101000000",
                                           "20350101000000") != 0) goto fin;
    if (mbedtls_x509write_crt_set_basic_constraints(&crt, is_ca ? 1 : 0, -1) != 0) goto fin;
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)cert_pem, cert_pem_len,
                                  mbedtls_ctr_drbg_random, drbg) != 0) goto fin;
    ok = true;
fin:
    mbedtls_x509write_crt_free(&crt);
    return ok;
}

/* Genere CA auto-signee + certificat client signe par la CA. Le cert serveur
 * reutilise g_cert_pem/g_cle_pem (CN=localhost) deja produits par
 * ldap_tls_genere_cert. */
static bool ldap_mtls_genere_materiel(void) {
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       ca_key, client_key;
    bool                     ok = false;

    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&ca_key);
    mbedtls_pk_init(&client_key);

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-ldap-mtls", 13) != 0) goto fin;
    if (!mtls_gen_keypair(&ca_key, &drbg, g_ca_key_pem, sizeof g_ca_key_pem)) goto fin;
    if (!mtls_gen_keypair(&client_key, &drbg, g_client_key_pem, sizeof g_client_key_pem)) goto fin;
    if (!mtls_write_cert("CN=owl-test-ca", "CN=owl-test-ca", &ca_key, &ca_key,
                         true, 1, &drbg, g_ca_cert_pem, sizeof g_ca_cert_pem)) goto fin;
    if (!mtls_write_cert("CN=owl-client", "CN=owl-test-ca", &client_key, &ca_key,
                         false, 2, &drbg, g_client_cert_pem, sizeof g_client_cert_pem)) goto fin;
    ok = true;
fin:
    mbedtls_pk_free(&ca_key);
    mbedtls_pk_free(&client_key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* BIO non blocants sur un fd brut (le socket accepte est non bloquant). */
static int mtls_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    owl_socket_fd_t fd = *(owl_socket_fd_t*)ctx;
    int n = (int)send(fd, (const char*)buf, (int)len, 0);
    if (n >= 0) return n;
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
#endif
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int mtls_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    owl_socket_fd_t fd = *(owl_socket_fd_t*)ctx;
    int n = (int)recv(fd, (char*)buf, (int)len, 0);
    if (n > 0) return n;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
#ifdef _WIN32
    if (WSAGetLastError() == WSAEWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
#endif
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

typedef struct {
    int          port;
    volatile int ready;
    volatile int stop;
    int          handshake_ok;    /* handshakes serveur reussis (cert client valide) */
    int          handshake_fail;  /* handshakes refuses (cert client absent) */
    mtls_thread_t thread;
} MtlsSrv;

/* Un handshake serveur non bloquant sur le fd accepte. Renvoie 0 si ok. */
static int mtls_srv_handshake(mbedtls_ssl_config* conf, owl_socket_fd_t fd) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_init(&ssl);
    int rc = -1;
    if (mbedtls_ssl_setup(&ssl, conf) != 0) goto fin;
    mbedtls_ssl_set_bio(&ssl, &fd, mtls_bio_send, mtls_bio_recv, NULL);
    for (;;) {
        rc = mbedtls_ssl_handshake(&ssl);
        if (rc == 0) break;
        if (rc == MBEDTLS_ERR_SSL_WANT_READ)  { if (owl_tcp_attend(fd, 1, 2000) <= 0) break; continue; }
        if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) { if (owl_tcp_attend(fd, 0, 2000) <= 0) break; continue; }
        break;
    }
fin:
    mbedtls_ssl_free(&ssl);
    return rc;
}

#ifdef _WIN32
static DWORD WINAPI mtls_srv_fn(LPVOID arg) {
#else
static void* mtls_srv_fn(void* arg) {
#endif
    MtlsSrv* s = (MtlsSrv*)arg;
    owl_tcp_global_init();

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         srv_cert, client_ca;
    mbedtls_pk_context       srv_key;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&srv_cert);
    mbedtls_x509_crt_init(&client_ca);
    mbedtls_pk_init(&srv_key);
    mbedtls_ssl_config_init(&conf);

    bool conf_ok =
        mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-mtls-srv", 12) == 0
        && mbedtls_x509_crt_parse(&srv_cert, (const unsigned char*)g_cert_pem,
                                  strlen(g_cert_pem) + 1) == 0
        && mbedtls_pk_parse_key(&srv_key, (const unsigned char*)g_cle_pem,
                                strlen(g_cle_pem) + 1, NULL, 0,
                                mbedtls_ctr_drbg_random, &drbg) == 0
        && mbedtls_x509_crt_parse(&client_ca, (const unsigned char*)g_ca_cert_pem,
                                  strlen(g_ca_cert_pem) + 1) == 0
        && mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT) == 0;
    if (conf_ok) {
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
        /* CA qui valide le cert client + EXIGE un cert client. */
        mbedtls_ssl_conf_ca_chain(&conf, &client_ca, NULL);
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        conf_ok = mbedtls_ssl_conf_own_cert(&conf, &srv_cert, &srv_key) == 0;
    }

    OwlTcpListener l;
    if (conf_ok && owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 4, false)) {
        s->port = owl_tcp_listener_port(&l);
        s->ready = 1;
        while (!s->stop) {
            OwlTcpConn conn;
            int rc = owl_tcp_listener_accepte(&l, 200, &conn);
            if (rc != OWL_TCPS_OK) continue;
            int hrc = mtls_srv_handshake(&conf, conn.fd);
            if (hrc == 0) s->handshake_ok++;
            else          s->handshake_fail++;
            owl_tcp_conn_ferme(&conn);
        }
        owl_tcp_listener_ferme(&l);
    } else {
        s->ready = 1;  /* debloque l'attente cote test meme sur echec de setup */
    }

    mbedtls_ssl_config_free(&conf);
    mbedtls_pk_free(&srv_key);
    mbedtls_x509_crt_free(&srv_cert);
    mbedtls_x509_crt_free(&client_ca);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void mtls_dort_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000 * 1000 };
    nanosleep(&ts, NULL);
#endif
}

static void test_mtls_cert_client(void) {
    /* Reutilise le cert serveur auto-signe (CN=localhost) de test_tls_confiance. */
    if (!ldap_tls_genere_cert())   { CHECK(false, "mtls: cert serveur genere"); return; }
    if (!ldap_mtls_genere_materiel()) { CHECK(false, "mtls: PKI client generee"); return; }

    /* Cert + cle client ecrits sur disque : owl_tls_connect prend des CHEMINS. */
    const char* cc_file = "owl_ldap_mtls_client_cert.pem";
    const char* ck_file = "owl_ldap_mtls_client_key.pem";
    bool files_ok = false;
    FILE* f = fopen(cc_file, "wb");
    if (f) {
        bool a = fwrite(g_client_cert_pem, 1, strlen(g_client_cert_pem), f)
                 == strlen(g_client_cert_pem);
        fclose(f);
        FILE* g = fopen(ck_file, "wb");
        if (g) {
            bool b = fwrite(g_client_key_pem, 1, strlen(g_client_key_pem), g)
                     == strlen(g_client_key_pem);
            fclose(g);
            files_ok = a && b;
        }
    }
    CHECK(files_ok, "mtls: cert+cle client ecrits sur disque");
    if (!files_ok) return;

    MtlsSrv s;
    memset(&s, 0, sizeof s);
#ifdef _WIN32
    s.thread = CreateThread(NULL, 0, mtls_srv_fn, &s, 0, NULL);
    bool spawned = s.thread != NULL;
#else
    bool spawned = pthread_create(&s.thread, NULL, mtls_srv_fn, &s) == 0;
#endif
    CHECK(spawned, "mtls: serveur demarre");
    if (!spawned) { remove(cc_file); remove(ck_file); return; }

    int waited = 0;
    while (!s.ready && waited < 2000) { mtls_dort_ms(10); waited += 10; }
    CHECK(s.port > 0, "mtls: endpoint pret");

    if (s.port > 0) {
        /* (a) AVEC cert client : handshake accepte (le refus vient du bind,
         *     pas du transport -> last_error ne mentionne pas "TLS"). */
        {
            OwlLdapClient c;
            owl_ldap_client_init(&c);
            int rc = -1;
            bool ok = owl_ldap_client_connect(&c, "localhost", s.port,
                                              true, OWL_TLS_VERIFY_NONE, NULL,
                                              cc_file, ck_file,
                                              "", (const unsigned char*)"", 0,
                                              true, 3, 2000, &rc);
            (void)ok;
            const char* err = owl_ldap_client_last_error(&c);
            CHECK(strstr(err, "TLS") == NULL,
                  "mtls: AVEC cert client franchit le handshake TLS");
            owl_ldap_client_close(&c);
        }

        /* (b) SANS cert client : handshake refuse au transport (le serveur
         *     exige un cert -> last_error mentionne "TLS"). */
        {
            OwlLdapClient c;
            owl_ldap_client_init(&c);
            int rc = -1;
            bool ok = owl_ldap_client_connect(&c, "localhost", s.port,
                                              true, OWL_TLS_VERIFY_NONE, NULL,
                                              NULL, NULL,
                                              "", (const unsigned char*)"", 0,
                                              true, 3, 2000, &rc);
            CHECK(!ok, "mtls: SANS cert client echoue");
            const char* err = owl_ldap_client_last_error(&c);
            CHECK(strstr(err, "TLS") != NULL,
                  "mtls: SANS cert client echoue AU handshake TLS");
            owl_ldap_client_close(&c);
        }
    }

    s.stop = 1;
#ifdef _WIN32
    WaitForSingleObject(s.thread, 10000);
    CloseHandle(s.thread);
#else
    pthread_join(s.thread, NULL);
#endif

    CHECK(s.handshake_ok >= 1, "mtls: serveur a accepte un handshake (cert client valide)");
    CHECK(s.handshake_fail >= 1, "mtls: serveur a refuse un handshake (cert client absent)");

    remove(cc_file);
    remove(ck_file);
}

/* ================================================================
 * Scenario SASL multi-round : prouve la boucle de bind GSS-SPNEGO
 * (ldap_client::owl_ldap_client_sasl_bind) de bout en bout, sans aucun
 * code plateforme ni vrai AD.
 *
 * Un mock serveur fait un echange SASL en 2 tours (jetons canned) :
 *   - tour 1 : BindRequest(GSS-SPNEGO, CLIENT_TOK_0)
 *              -> BindResponse saslBindInProgress(14) + serverSaslCreds[7] ;
 *   - tour 2 : BindRequest(GSS-SPNEGO, CLIENT_TOK_1)
 *              -> BindResponse success(0).
 * Un mock token-provider fournit les jetons canned et verifie qu'il recoit
 * bien le serverSaslCreds du tour precedent + que le channel binding opaque
 * lui est transmis.
 * ================================================================ */

#define SASL_CLIENT_TOK_0 "CLIENT-SPNEGO-0"
#define SASL_SERVER_TOK_0 "SERVER-CHALLENGE-0"
#define SASL_CLIENT_TOK_1 "CLIENT-SPNEGO-1"

/* ---- mock token-provider ---- */

typedef struct {
    int                  step_calls;
    bool                 saw_server_token;  /* step a recu SASL_SERVER_TOK_0 */
    const unsigned char* cb_seen;           /* channel binding recu par init */
    size_t               cb_seen_len;
} SaslMockProvider;

static OwlLdapSaslToken sasl_prov_init(void* self,
                                       const unsigned char* cb, size_t cb_len) {
    SaslMockProvider* p = (SaslMockProvider*)self;
    p->cb_seen = cb;
    p->cb_seen_len = cb_len;
    OwlLdapSaslToken t;
    t.ok = true;
    t.done = false;
    t.token = (const unsigned char*)SASL_CLIENT_TOK_0;
    t.token_len = strlen(SASL_CLIENT_TOK_0);
    return t;
}

static OwlLdapSaslToken sasl_prov_step(void* self,
                                       const unsigned char* srv, size_t srv_len) {
    SaslMockProvider* p = (SaslMockProvider*)self;
    p->step_calls++;
    p->saw_server_token = (srv != NULL && srv_len == strlen(SASL_SERVER_TOK_0)
                           && memcmp(srv, SASL_SERVER_TOK_0, srv_len) == 0);
    OwlLdapSaslToken t;
    t.ok = true;
    t.done = true;
    t.token = (const unsigned char*)SASL_CLIENT_TOK_1;
    t.token_len = strlen(SASL_CLIENT_TOK_1);
    return t;
}

/* ---- mock serveur SASL (plain TCP, in-process) ---- */

typedef struct {
    unsigned char buf[8192];
    size_t len;
    size_t start;
} SaslRx;

static int sasl_frame_len(const unsigned char* d, size_t avail, size_t* total) {
    if (avail < 2) return 0;
    unsigned char l0 = d[1];
    if (l0 < 0x80) { *total = 2 + (size_t)l0; return 1; }
    unsigned int nb = l0 & 0x7F;
    if (nb == 0 || nb > 4) return -1;
    if (avail < 2 + nb) return 0;
    size_t c = 0;
    for (unsigned int i = 0; i < nb; i++) c = (c << 8) | d[2 + i];
    *total = 2 + nb + c;
    return 1;
}

static int sasl_read_msg(OwlTcpConn* conn, SaslRx* r,
                         const unsigned char** out, size_t* out_len) {
    for (;;) {
        size_t avail = r->len - r->start;
        size_t total = 0;
        int fr = sasl_frame_len(r->buf + r->start, avail, &total);
        if (fr < 0) return -1;
        if (fr == 1 && avail >= total) {
            *out = r->buf + r->start;
            *out_len = total;
            r->start += total;
            return 1;
        }
        if (r->start > 0) {
            memmove(r->buf, r->buf + r->start, avail);
            r->len = avail;
            r->start = 0;
        }
        if (r->len == sizeof(r->buf)) return -1;
        int n = owl_tcp_conn_lit(conn, r->buf + r->len,
                                 sizeof(r->buf) - r->len, 5000);
        if (n <= 0) return 0;
        r->len += (size_t)n;
    }
}

static bool sasl_tlv(const unsigned char** p, const unsigned char* end,
                     unsigned char* tag, const unsigned char** val, size_t* vlen) {
    if ((size_t)(end - *p) < 2) return false;
    *tag = (*p)[0];
    unsigned char l0 = (*p)[1];
    const unsigned char* q = *p + 2;
    size_t l;
    if (l0 < 0x80) {
        l = l0;
    } else {
        unsigned int nb = l0 & 0x7F;
        if (nb == 0 || nb > 4 || (size_t)(end - q) < nb) return false;
        l = 0;
        for (unsigned int i = 0; i < nb; i++) l = (l << 8) | q[i];
        q += nb;
    }
    if ((size_t)(end - q) < l) return false;
    *val = q;
    *vlen = l;
    *p = q + l;
    return true;
}

/* BindResponse { resultCode, matchedDN "", diag "", [7] serverSaslCreds? }.
 * server_creds NULL => champ [7] omis. Ecriture a rebours. */
static bool sasl_build_bind_response(OwlBerWriter* w, int message_id,
                                     int result_code, const char* server_creds) {
    owl_ber_writer_init(w);
    size_t mark = owl_ber_writer_len(w);
    if (server_creds != NULL) {
        if (!owl_ber_write_octet_string(w, (const unsigned char*)server_creds,
                                        strlen(server_creds))) return false;
        /* retag OCTET STRING (0x04) -> [7] primitif (0x87) */
        *w->p = owl_ber_tag(OWL_BER_CLASS_CONTEXT, false, 7);
    }
    if (!owl_ber_write_octet_string(w, (const unsigned char*)"", 0)) return false; /* diag */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)"", 0)) return false; /* matchedDN */
    if (!owl_ber_write_enumerated(w, result_code)) return false;                   /* resultCode */
    if (!owl_ber_wrap(w, OWL_LDAP_OP_BIND_RESPONSE,
                      owl_ber_writer_len(w) - mark)) return false;
    return owl_ldap_message_finish(w, message_id);
}

typedef struct {
    int          port;
    volatile int ready;
    mtls_thread_t thread;
    int          bind_rounds;
    char         mech0[64];
    char         cred0[128];
    char         cred1[128];
    int          saw_unbind;
} SaslSrv;

static void sasl_capture(char* dst, size_t dst_sz,
                         const char* src, size_t src_len) {
    size_t cp = src_len < dst_sz - 1 ? src_len : dst_sz - 1;
    memcpy(dst, src, cp);
    dst[cp] = '\0';
}

static void sasl_srv_handle_bind(SaslSrv* s, OwlTcpConn* conn,
                                 const OwlLdapMessageHeader* hdr) {
    /* version INTEGER, name OCTET STRING, authentication [3] sasl. */
    const unsigned char* p = hdr->opContent;
    const unsigned char* end = hdr->opContent + hdr->opLen;
    unsigned char tag;
    const unsigned char* v;
    size_t vlen;
    const char* mech = NULL; size_t mech_len = 0;
    const char* cred = NULL; size_t cred_len = 0;

    if (sasl_tlv(&p, end, &tag, &v, &vlen) && tag == 0x02        /* version */
        && sasl_tlv(&p, end, &tag, &v, &vlen) && tag == 0x04     /* name */
        && sasl_tlv(&p, end, &tag, &v, &vlen) && tag == 0xA3) {  /* [3] sasl */
        const unsigned char* ap = v;
        const unsigned char* aend = v + vlen;
        if (sasl_tlv(&ap, aend, &tag, &v, &vlen) && tag == 0x04) {
            mech = (const char*)v; mech_len = vlen;
        }
        if (sasl_tlv(&ap, aend, &tag, &v, &vlen) && tag == 0x04) {
            cred = (const char*)v; cred_len = vlen;
        }
    }

    int idx = s->bind_rounds;
    if (idx == 0) {
        if (mech) sasl_capture(s->mech0, sizeof s->mech0, mech, mech_len);
        if (cred) sasl_capture(s->cred0, sizeof s->cred0, cred, cred_len);
    } else if (idx == 1) {
        if (cred) sasl_capture(s->cred1, sizeof s->cred1, cred, cred_len);
    }
    s->bind_rounds++;

    OwlBerWriter w;
    bool built = (idx == 0)
        ? sasl_build_bind_response(&w, hdr->messageID,
                                   OWL_LDAP_SASL_BIND_IN_PROGRESS, SASL_SERVER_TOK_0)
        : sasl_build_bind_response(&w, hdr->messageID, OWL_LDAP_SUCCESS, NULL);
    if (built)
        owl_tcp_conn_ecrit(conn, owl_ber_writer_data(&w),
                           owl_ber_writer_len(&w), 5000);
    owl_ber_writer_free(&w);
}

#ifdef _WIN32
static DWORD WINAPI sasl_srv_fn(LPVOID arg) {
#else
static void* sasl_srv_fn(void* arg) {
#endif
    SaslSrv* s = (SaslSrv*)arg;
    owl_tcp_global_init();

    OwlTcpListener l;
    if (owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 1, false)) {
        s->port = owl_tcp_listener_port(&l);
        s->ready = 1;
        OwlTcpConn conn;
        if (owl_tcp_listener_accepte(&l, 5000, &conn) == OWL_TCPS_OK) {
            SaslRx rx; rx.len = 0; rx.start = 0;
            for (;;) {
                const unsigned char* msg; size_t msglen;
                if (sasl_read_msg(&conn, &rx, &msg, &msglen) != 1) break;
                OwlLdapMessageHeader hdr;
                if (!owl_ldap_message_parse(msg, msglen, &hdr)) break;
                if (hdr.protocolOpTag == OWL_LDAP_OP_BIND_REQUEST) {
                    sasl_srv_handle_bind(s, &conn, &hdr);
                } else if (hdr.protocolOpTag == OWL_LDAP_OP_UNBIND_REQUEST) {
                    s->saw_unbind++;
                    break;
                }
            }
            owl_tcp_conn_ferme(&conn);
        }
        owl_tcp_listener_ferme(&l);
    } else {
        s->ready = 1;  /* debloque l'attente cote test meme sur echec de setup */
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void test_sasl_multiround(void) {
    SaslSrv s;
    memset(&s, 0, sizeof s);
#ifdef _WIN32
    s.thread = CreateThread(NULL, 0, sasl_srv_fn, &s, 0, NULL);
    bool spawned = s.thread != NULL;
#else
    bool spawned = pthread_create(&s.thread, NULL, sasl_srv_fn, &s) == 0;
#endif
    CHECK(spawned, "sasl: serveur demarre");
    if (!spawned) return;

    int waited = 0;
    while (!s.ready && waited < 2000) { mtls_dort_ms(10); waited += 10; }
    CHECK(s.port > 0, "sasl: endpoint pret");

    if (s.port > 0) {
        OwlLdapClient c;
        owl_ldap_client_init(&c);
        bool opened = owl_ldap_client_open(&c, "127.0.0.1", s.port,
                                           false, 0, NULL, NULL, NULL, 2000);
        CHECK(opened, "sasl: transport ouvert (sans bind)");
        if (opened) {
            SaslMockProvider prov;
            memset(&prov, 0, sizeof prov);
            OwlLdapTokenProvider tp;
            tp.init = sasl_prov_init;
            tp.step = sasl_prov_step;
            tp.self = &prov;

            const char* cb = "CB-tls-server-end-point";
            int rc = -1;
            bool ok = owl_ldap_client_sasl_bind(&c, "GSS-SPNEGO", &tp,
                          (const unsigned char*)cb, strlen(cb), 3, &rc);
            CHECK(ok, "sasl: boucle multi-round aboutit");
            CHECK(ok && rc == 0, "sasl: resultCode final success (0)");
            CHECK(prov.cb_seen != NULL && prov.cb_seen_len == strlen(cb)
                     && memcmp(prov.cb_seen, cb, prov.cb_seen_len) == 0,
                  "sasl: channel binding opaque transmis a init");
            CHECK(prov.step_calls == 1, "sasl: provider.step appele 1 fois");
            CHECK(prov.saw_server_token,
                  "sasl: provider.step a recu le serverSaslCreds du tour 1");
        }
        owl_ldap_client_close(&c);
    }

#ifdef _WIN32
    WaitForSingleObject(s.thread, 10000);
    CloseHandle(s.thread);
#else
    pthread_join(s.thread, NULL);
#endif

    CHECK(s.bind_rounds == 2, "sasl: serveur a vu 2 BindRequest");
    CHECK(strcmp(s.mech0, "GSS-SPNEGO") == 0, "sasl: mecanisme annonce GSS-SPNEGO");
    CHECK(strcmp(s.cred0, SASL_CLIENT_TOK_0) == 0, "sasl: jeton client tour 1 recu");
    CHECK(strcmp(s.cred1, SASL_CLIENT_TOK_1) == 0, "sasl: jeton client tour 2 recu");
    CHECK(s.saw_unbind == 1, "sasl: serveur a vu l'unbind");
}

int ldap_client_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    owl_tcp_global_init();

    test_session_nominale();
    test_bind_mauvais_mdp();
    test_tls_confiance();
    test_mtls_cert_client();
    test_sasl_multiround();

    owl_tcp_global_shutdown();

    printf("ldap_client self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
