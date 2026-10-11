// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ftp_mock_server.h"
#include "net_tls_server.h"
#include "net_tcp_server.h"   /* OWL_TCPS_OK */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/ecp.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/md.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE fm_thread_t;
  typedef SOCKET fm_socket_t;
  #define FM_INVALID   INVALID_SOCKET
  #define FM_CLOSE     closesocket
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef pthread_t fm_thread_t;
  typedef int fm_socket_t;
  #define FM_INVALID   (-1)
  #define FM_CLOSE     close
#endif

#define FM_STOR_MAX  65536

struct FtpMockServer {
    int          port;
    int          ready;
    fm_thread_t  thread;

    int          advertise_mlsd;
    char*        listing;
    size_t       listing_len;
    char*        retr_body;
    size_t       retr_body_len;
    char         mdtm[32];        /* horodatage MDTM ("" -> 550) */
    char         mlst_facts[256]; /* facts MLST ("" -> 550) */

    char         last_mdtm[256];  /* capture : argument du dernier MDTM */
    char         last_mlst[256];  /* capture : argument du dernier MLST */

    /* captures */
    char         last_user[128];
    char         last_pass[128];
    char         last_retr[256];
    char         last_stor[256];
    char         last_list[256];
    int          saw_pasv;
    char         stor_data[FM_STOR_MAX];
    size_t       stor_data_len;

    /* --- TLS (FTPS/FTPES) --- */
    int          tls_mode;        /* 0 clair / 1 FTPS implicite / 2 FTPES explicite */
    OwlTlsServeurMateriel* mat;   /* cert+cle auto-signes (partages) */
    OwlTlsServeurCtx*      ctx;   /* contexte mbedTLS serveur */
    /* observables : preuve du chemin securise reellement emprunte */
    int          ctrl_was_tls;    /* le canal de controle a fait un handshake TLS */
    int          data_was_tls;    /* un canal de donnees a fait un handshake TLS */
    int          saw_auth_tls;    /* le serveur a recu AUTH TLS */
    int          saw_pbsz;        /* le serveur a recu PBSZ */
    char         last_prot[8];    /* argument du dernier PROT ("P" attendu) */
};

/* ================================================================
 * Materiel TLS serveur : certificat auto-signe genere a la volee.
 * Volontairement NON approuve par le bundle Mozilla -> une verif
 * cliente FULL/CHAIN echoue (cas "certificat invalide non ignore"),
 * une verif NONE (ftpIgnoreCertificatInvalide) aboutit.
 * ================================================================ */

static bool fm_genere_cert(char* cert_pem, size_t cert_sz,
                           char* cle_pem, size_t cle_sz) {
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
                              (const unsigned char*)"owl-ftp-mock", 12) != 0) goto fin;
    if (mbedtls_pk_setup(&cle, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) goto fin;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(cle),
                            mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    if (mbedtls_pk_write_key_pem(&cle, (unsigned char*)cle_pem, cle_sz) != 0) goto fin;

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
    if (mbedtls_x509write_crt_pem(&crt, (unsigned char*)cert_pem, cert_sz,
                                  mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    ok = true;
fin:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&cle);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* ================================================================
 * Canal (controle ou donnees) : en clair (fd) ou chiffre (session TLS).
 * ================================================================ */

typedef struct {
    fm_socket_t    fd;
    OwlTlsSession* tls;    /* NULL = clair */
} FmChan;

/* ================================================================
 * Helpers socket (OS direct — le mock reste independant du client).
 * ================================================================ */

#define FM_TLS_TIMEOUT_MS 5000

static int fm_send_all(FmChan* ch, const void* buf, size_t len) {
    if (ch->tls) {
        return owl_tls_session_ecrit(ch->tls, buf, len, FM_TLS_TIMEOUT_MS) == OWL_TCPS_OK
               ? 0 : -1;
    }
    const char* p = (const char*)buf;
    size_t total = 0;
    while (total < len) {
        int n = send(ch->fd, p + total, (int)(len - total), 0);
        if (n <= 0) return -1;
        total += (size_t)n;
    }
    return 0;
}

static int fm_send_line(FmChan* ch, const char* line) {
    return fm_send_all(ch, line, strlen(line));
}

/* Lecture d'une ligne de commande (jusqu'au \n) via un petit buffer
 * per-connexion. Le \r\n final est retire. Renvoie false sur EOF/erreur. */
typedef struct {
    char   buf[2048];
    size_t len;
    size_t pos;
} FmLineReader;

static int fm_read_line(FmChan* ch, FmLineReader* r, char* out, size_t out_sz) {
    size_t n = 0;
    for (;;) {
        if (r->pos >= r->len) {
            int got;
            if (ch->tls) {
                got = owl_tls_session_lit(ch->tls, r->buf, sizeof(r->buf),
                                          FM_TLS_TIMEOUT_MS);
                if (got <= 0) return 0;   /* FERME/ERREUR/TIMEOUT */
            } else {
                got = recv(ch->fd, r->buf, (int)sizeof(r->buf), 0);
                if (got <= 0) return 0;
            }
            r->len = (size_t)got;
            r->pos = 0;
        }
        char ch = r->buf[r->pos++];
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return 1;
        }
        if (n + 1 < out_sz) out[n++] = ch;
    }
}

/* Ouvre un canal de donnees en ecoute sur loopback:0. Renvoie la socket
 * d'ecoute (a accepter puis fermer) et remplit *out_port. */
static fm_socket_t fm_open_data_listener(int* out_port) {
    fm_socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == FM_INVALID) return FM_INVALID;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(0);
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) { FM_CLOSE(s); return FM_INVALID; }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, (struct sockaddr*)&addr, &alen) != 0) { FM_CLOSE(s); return FM_INVALID; }
    *out_port = ntohs(addr.sin_port);
    if (listen(s, 1) != 0) { FM_CLOSE(s); return FM_INVALID; }
    return s;
}

/* ================================================================
 * Dialogue de controle
 * ================================================================ */

static int fm_chan_recv(FmChan* ch, void* buf, size_t len) {
    if (ch->tls) return owl_tls_session_lit(ch->tls, buf, len, FM_TLS_TIMEOUT_MS);
    return recv(ch->fd, buf, (int)len, 0);
}

/* Accepte le canal de donnees en attente (et ferme le listener). */
static FmChan fm_data_accept(fm_socket_t* dlisten) {
    FmChan dc; dc.tls = NULL;
    dc.fd = accept(*dlisten, NULL, NULL);
    FM_CLOSE(*dlisten); *dlisten = FM_INVALID;
    return dc;
}

/* Promeut le canal de donnees en TLS quand la connexion est securisee
 * (handshake serveur, session fraiche depuis le contexte partage). Renvoie
 * false si le handshake echoue. */
static bool fm_data_secure(FtpMockServer* m, FmChan* dc) {
    if (m->tls_mode == 0 || dc->fd == FM_INVALID) return true;
    OwlTlsSession* s = NULL;
    if (owl_tls_serveur_handshake(m->ctx, dc->fd, FM_TLS_TIMEOUT_MS, &s) != OWL_TCPS_OK)
        return false;
    dc->tls = s;
    m->data_was_tls = 1;
    return true;
}

static void fm_data_close(FmChan* dc) {
    if (dc->tls) owl_tls_session_ferme(dc->tls);
    if (dc->fd != FM_INVALID) FM_CLOSE(dc->fd);
}

static void fm_serve(FtpMockServer* m, fm_socket_t cli) {
    FmLineReader rd;
    rd.len = 0; rd.pos = 0;

    FmChan cc; cc.fd = cli; cc.tls = NULL;   /* canal de controle */

    /* FTPS implicite : handshake TLS AVANT toute donnee (banniere comprise). */
    if (m->tls_mode == 1) {
        OwlTlsSession* s = NULL;
        if (owl_tls_serveur_handshake(m->ctx, cli, FM_TLS_TIMEOUT_MS, &s) != OWL_TCPS_OK)
            return;
        cc.tls = s;
        m->ctrl_was_tls = 1;
    }

    fm_send_line(&cc, "220 owl mock FTP ready\r\n");

    fm_socket_t dlisten = FM_INVALID;   /* canal de donnees en attente (post-PASV) */
    char line[2048];

    while (fm_read_line(&cc, &rd, line, sizeof(line))) {
        /* Verbe = 4 premiers caracteres majuscules ; argument = apres l'espace. */
        char verb[8];
        int i = 0;
        while (line[i] && line[i] != ' ' && i < 7) { verb[i] = line[i]; i++; }
        verb[i] = '\0';
        const char* arg = (line[i] == ' ') ? line + i + 1 : line + i;

        if (strcmp(verb, "AUTH") == 0) {
            /* FTPES explicite : AUTH TLS -> 234, puis handshake sur le canal
             * de controle deja ouvert (le login qui suit passe chiffre). */
            m->saw_auth_tls = 1;
            fm_send_line(&cc, "234 AUTH TLS OK\r\n");
            OwlTlsSession* s = NULL;
            if (owl_tls_serveur_handshake(m->ctx, cli, FM_TLS_TIMEOUT_MS, &s) != OWL_TCPS_OK)
                break;
            cc.tls = s;
            m->ctrl_was_tls = 1;
            rd.len = 0; rd.pos = 0;   /* le buffer clair est consomme */
        } else if (strcmp(verb, "PBSZ") == 0) {
            m->saw_pbsz = 1;
            fm_send_line(&cc, "200 PBSZ ok\r\n");
        } else if (strcmp(verb, "PROT") == 0) {
            snprintf(m->last_prot, sizeof(m->last_prot), "%s", arg);
            fm_send_line(&cc, "200 PROT ok\r\n");
        } else if (strcmp(verb, "USER") == 0) {
            snprintf(m->last_user, sizeof(m->last_user), "%s", arg);
            fm_send_line(&cc, "331 need password\r\n");
        } else if (strcmp(verb, "PASS") == 0) {
            snprintf(m->last_pass, sizeof(m->last_pass), "%s", arg);
            fm_send_line(&cc, "230 logged in\r\n");
        } else if (strcmp(verb, "FEAT") == 0) {
            if (m->advertise_mlsd)
                fm_send_line(&cc, "211-Features:\r\n MLSD\r\n UTF8\r\n211 End\r\n");
            else
                fm_send_line(&cc, "211-Features:\r\n UTF8\r\n211 End\r\n");
        } else if (strcmp(verb, "OPTS") == 0) {
            fm_send_line(&cc, "200 ok\r\n");
        } else if (strcmp(verb, "TYPE") == 0) {
            fm_send_line(&cc, "200 type set\r\n");
        } else if (strcmp(verb, "SIZE") == 0) {
            char rep[64];
            snprintf(rep, sizeof(rep), "213 %lu\r\n", (unsigned long)m->retr_body_len);
            fm_send_line(&cc, rep);
        } else if (strcmp(verb, "MDTM") == 0) {
            snprintf(m->last_mdtm, sizeof(m->last_mdtm), "%s", arg);
            if (m->mdtm[0]) {
                char rep[64];
                snprintf(rep, sizeof(rep), "213 %s\r\n", m->mdtm);
                fm_send_line(&cc, rep);
            } else {
                fm_send_line(&cc, "550 no such file\r\n");
            }
        } else if (strcmp(verb, "MLST") == 0) {
            snprintf(m->last_mlst, sizeof(m->last_mlst), "%s", arg);
            if (m->mlst_facts[0]) {
                char rep[512];
                snprintf(rep, sizeof(rep),
                         "250-Listing %s\r\n %s\r\n250 End\r\n", arg, m->mlst_facts);
                fm_send_line(&cc, rep);
            } else {
                fm_send_line(&cc, "550 no such file\r\n");
            }
        } else if (strcmp(verb, "PASV") == 0) {
            int dport = 0;
            if (dlisten != FM_INVALID) { FM_CLOSE(dlisten); dlisten = FM_INVALID; }
            dlisten = fm_open_data_listener(&dport);
            if (dlisten == FM_INVALID) { fm_send_line(&cc, "425 can't open data\r\n"); continue; }
            m->saw_pasv++;
            char rep[80];
            snprintf(rep, sizeof(rep), "227 Entering Passive Mode (127,0,0,1,%d,%d)\r\n",
                     (dport >> 8) & 0xFF, dport & 0xFF);
            fm_send_line(&cc, rep);
        } else if (strcmp(verb, "LIST") == 0 || strcmp(verb, "MLSD") == 0) {
            snprintf(m->last_list, sizeof(m->last_list), "%s", arg);
            if (dlisten == FM_INVALID) { fm_send_line(&cc, "425 use PASV first\r\n"); continue; }
            FmChan dc = fm_data_accept(&dlisten);
            fm_send_line(&cc, "150 opening data connection\r\n");
            if (dc.fd != FM_INVALID && fm_data_secure(m, &dc)) {
                if (m->listing && m->listing_len > 0)
                    fm_send_all(&dc, m->listing, m->listing_len);
            }
            fm_data_close(&dc);
            fm_send_line(&cc, "226 transfer complete\r\n");
        } else if (strcmp(verb, "RETR") == 0) {
            snprintf(m->last_retr, sizeof(m->last_retr), "%s", arg);
            if (dlisten == FM_INVALID) { fm_send_line(&cc, "425 use PASV first\r\n"); continue; }
            FmChan dc = fm_data_accept(&dlisten);
            fm_send_line(&cc, "150 opening data connection\r\n");
            if (dc.fd != FM_INVALID && fm_data_secure(m, &dc)) {
                if (m->retr_body && m->retr_body_len > 0)
                    fm_send_all(&dc, m->retr_body, m->retr_body_len);
            }
            fm_data_close(&dc);
            fm_send_line(&cc, "226 transfer complete\r\n");
        } else if (strcmp(verb, "STOR") == 0) {
            snprintf(m->last_stor, sizeof(m->last_stor), "%s", arg);
            if (dlisten == FM_INVALID) { fm_send_line(&cc, "425 use PASV first\r\n"); continue; }
            FmChan dc = fm_data_accept(&dlisten);
            fm_send_line(&cc, "150 opening data connection\r\n");
            if (dc.fd != FM_INVALID && fm_data_secure(m, &dc)) {
                m->stor_data_len = 0;
                char tmp[4096];
                for (;;) {
                    int n = fm_chan_recv(&dc, tmp, sizeof(tmp));
                    if (n <= 0) break;
                    size_t place = FM_STOR_MAX - m->stor_data_len;
                    size_t cp = (size_t)n < place ? (size_t)n : place;
                    if (cp > 0) {
                        memcpy(m->stor_data + m->stor_data_len, tmp, cp);
                        m->stor_data_len += cp;
                    }
                }
            }
            fm_data_close(&dc);
            fm_send_line(&cc, "226 transfer complete\r\n");
        } else if (strcmp(verb, "QUIT") == 0) {
            fm_send_line(&cc, "221 bye\r\n");
            break;
        } else if (strcmp(verb, "PWD") == 0) {
            fm_send_line(&cc, "257 \"/\" is current directory\r\n");
        } else if (strcmp(verb, "CWD") == 0) {
            fm_send_line(&cc, "250 ok\r\n");
        } else {
            fm_send_line(&cc, "500 unknown command\r\n");
        }
    }

    if (dlisten != FM_INVALID) FM_CLOSE(dlisten);
    if (cc.tls) owl_tls_session_ferme(cc.tls);
}

#ifdef _WIN32
static DWORD WINAPI fm_thread_fn(LPVOID arg) {
#else
static void* fm_thread_fn(void* arg) {
#endif
    FtpMockServer* m = (FtpMockServer*)arg;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int one = 1;
    fm_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == FM_INVALID) {
#ifdef _WIN32
        return 0;
#else
        return NULL;
#endif
    }
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(0);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
        getsockname(srv, (struct sockaddr*)&addr, &alen) == 0 &&
        (m->port = ntohs(addr.sin_port), listen(srv, 1) == 0)) {
        m->ready = 1;
        fm_socket_t cli = accept(srv, NULL, NULL);
        if (cli != FM_INVALID) {
            fm_serve(m, cli);
#ifdef _WIN32
            shutdown(cli, SD_BOTH);
#else
            shutdown(cli, SHUT_RDWR);
#endif
            FM_CLOSE(cli);
        }
    }

    FM_CLOSE(srv);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================
 * API publique
 * ================================================================ */

void ftp_mock_config_defaut(FtpMockConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->advertise_mlsd = 1;
}

FtpMockServer* ftp_mock_server_start(const FtpMockConfig* cfg) {
    FtpMockServer* m = (FtpMockServer*)calloc(1, sizeof(FtpMockServer));
    if (!m) return NULL;
    m->advertise_mlsd = cfg->advertise_mlsd;
    if (cfg->listing) {
        m->listing_len = strlen(cfg->listing);
        m->listing = (char*)malloc(m->listing_len + 1);
        if (m->listing) memcpy(m->listing, cfg->listing, m->listing_len + 1);
    }
    if (cfg->retr_body) {
        m->retr_body_len = cfg->retr_body_len ? cfg->retr_body_len
                                              : strlen(cfg->retr_body);
        m->retr_body = (char*)malloc(m->retr_body_len ? m->retr_body_len : 1);
        if (m->retr_body) memcpy(m->retr_body, cfg->retr_body, m->retr_body_len);
    }
    if (cfg->mdtm)       snprintf(m->mdtm, sizeof(m->mdtm), "%s", cfg->mdtm);
    if (cfg->mlst_facts) snprintf(m->mlst_facts, sizeof(m->mlst_facts), "%s", cfg->mlst_facts);

    /* Materiel TLS serveur : requis pour FTPS/FTPES. Genere un certificat
     * auto-signe (non approuve par les racines Mozilla) au demarrage. */
    m->tls_mode = cfg->tls_mode;
    if (m->tls_mode != 0) {
        static char cert_pem[4096];
        static char cle_pem[2048];
        char terr[256];
        if (!fm_genere_cert(cert_pem, sizeof(cert_pem), cle_pem, sizeof(cle_pem))) {
            free(m->listing); free(m->retr_body); free(m); return NULL;
        }
        m->mat = owl_tls_serveur_materiel_cree(cert_pem, cle_pem, terr, sizeof(terr));
        if (!m->mat) { free(m->listing); free(m->retr_body); free(m); return NULL; }
        m->ctx = owl_tls_serveur_ctx_cree(m->mat);
        if (!m->ctx) {
            owl_tls_serveur_materiel_libere(m->mat);
            free(m->listing); free(m->retr_body); free(m); return NULL;
        }
    }
#ifdef _WIN32
    m->thread = CreateThread(NULL, 0, fm_thread_fn, m, 0, NULL);
    if (m->thread == NULL) { free(m->listing); free(m->retr_body); free(m); return NULL; }
#else
    if (pthread_create(&m->thread, NULL, fm_thread_fn, m) != 0) {
        free(m->listing); free(m->retr_body); free(m); return NULL;
    }
#endif
    int waited = 0;
    while (!m->ready && waited < 2000) {
#ifdef _WIN32
        Sleep(10);
#else
        struct timespec ts = { 0, 10 * 1000 * 1000 }; nanosleep(&ts, NULL);
#endif
        waited += 10;
    }
    return m;
}

int ftp_mock_server_port(const FtpMockServer* m)      { return m->port; }
int ftp_mock_server_is_ready(const FtpMockServer* m)  { return m->ready; }

void ftp_mock_server_join(FtpMockServer* m) {
#ifdef _WIN32
    WaitForSingleObject(m->thread, 10000);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread, NULL);
#endif
}

void ftp_mock_server_free(FtpMockServer* m) {
    if (!m) return;
    if (m->ctx) owl_tls_serveur_ctx_libere(m->ctx);
    if (m->mat) owl_tls_serveur_materiel_libere(m->mat);
    free(m->listing);
    free(m->retr_body);
    free(m);
}

int ftp_mock_server_ctrl_was_tls(const FtpMockServer* m) { return m->ctrl_was_tls; }
int ftp_mock_server_data_was_tls(const FtpMockServer* m) { return m->data_was_tls; }
int ftp_mock_server_saw_auth_tls(const FtpMockServer* m) { return m->saw_auth_tls; }
int ftp_mock_server_saw_pbsz(const FtpMockServer* m)     { return m->saw_pbsz; }
const char* ftp_mock_server_last_prot(const FtpMockServer* m) { return m->last_prot; }

const char* ftp_mock_server_last_user(const FtpMockServer* m) { return m->last_user; }
const char* ftp_mock_server_last_pass(const FtpMockServer* m) { return m->last_pass; }
const char* ftp_mock_server_last_retr(const FtpMockServer* m) { return m->last_retr; }
const char* ftp_mock_server_last_stor(const FtpMockServer* m) { return m->last_stor; }
const char* ftp_mock_server_last_list(const FtpMockServer* m) { return m->last_list; }
const char* ftp_mock_server_last_mdtm(const FtpMockServer* m) { return m->last_mdtm; }
const char* ftp_mock_server_last_mlst(const FtpMockServer* m) { return m->last_mlst; }
int         ftp_mock_server_saw_pasv(const FtpMockServer* m)  { return m->saw_pasv; }

const char* ftp_mock_server_stor_data(const FtpMockServer* m, size_t* out_len) {
    if (out_len) *out_len = m->stor_data_len;
    return m->stor_data;
}
