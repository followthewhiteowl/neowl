// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_mock_server.h"
#include "net_tcp.h"
#include "net_tcp_server.h"   /* OwlTcpListener, OwlTcpConn, OWL_TCPS_* */
#include "ldap_ber.h"
#include "ldap_pdu.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
  typedef HANDLE lm_thread_t;
#else
  #include <pthread.h>
  #include <time.h>
  typedef pthread_t lm_thread_t;
#endif

#define LM_IO_TIMEOUT_MS 5000

struct LdapMockServer {
    int        port;
    int        ready;
    lm_thread_t thread;

    char       bind_dn[256];
    char       bind_password[256];

    /* captures (lisibles apres join) */
    char       last_bind_dn[256];
    char       last_base[256];
    int        saw_bind;
    int        saw_unbind;
    int        search_count;
};

/* ================================================================
 * Reception framee : extrait un LDAPMessage complet du flux.
 * ================================================================ */

typedef struct {
    unsigned char buf[8192];
    size_t len;
    size_t start;
} LmRx;

/* 1 = total calcule, 0 = en-tete incomplet, -1 = malforme. */
static int lm_frame_len(const unsigned char* d, size_t avail, size_t* total) {
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

/* Renvoie 1 + (*out,*out_len), 0 sur EOF/timeout/erreur, -1 sur malforme. */
static int lm_read_message(OwlTcpConn* conn, LmRx* r,
                           const unsigned char** out, size_t* out_len) {
    for (;;) {
        size_t avail = r->len - r->start;
        size_t total = 0;
        int fr = lm_frame_len(r->buf + r->start, avail, &total);
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
        if (r->len == sizeof(r->buf)) return -1;   /* message trop gros pour le mock */
        int n = owl_tcp_conn_lit(conn, r->buf + r->len,
                                 sizeof(r->buf) - r->len, LM_IO_TIMEOUT_MS);
        if (n <= 0) return 0;
        r->len += (size_t)n;
    }
}

/* ================================================================
 * TLV manuel (extraction de champs d'un protocolOp). Bornage strict.
 * ================================================================ */

static bool lm_tlv(const unsigned char** p, const unsigned char* end,
                   unsigned char* tag, const unsigned char** val, size_t* vlen) {
    if ((size_t)(end - *p) < 2) return false;
    *tag = (*p)[0];
    unsigned char l0 = (*p)[1];
    const unsigned char* q = *p + 2;
    size_t len;
    if (l0 < 0x80) {
        len = l0;
    } else {
        unsigned int nb = l0 & 0x7F;
        if (nb == 0 || nb > 4 || (size_t)(end - q) < nb) return false;
        len = 0;
        for (unsigned int i = 0; i < nb; i++) len = (len << 8) | q[i];
        q += nb;
    }
    if ((size_t)(end - q) < len) return false;
    *val = q;
    *vlen = len;
    *p = q + len;
    return true;
}

/* ================================================================
 * Emission des reponses.
 * ================================================================ */

static int lm_send(OwlTcpConn* conn, const OwlBerWriter* w) {
    return owl_tcp_conn_ecrit(conn, owl_ber_writer_data(w),
                              owl_ber_writer_len(w), LM_IO_TIMEOUT_MS);
}

/* LDAPResult { resultCode, matchedDN "", diagnosticMessage "" } enrobe par
 * `app_tag` (0x61 BindResponse, 0x65 SearchResultDone), finalise en
 * LDAPMessage de message_id. Ecriture a rebours : les champs sont ecrits
 * dans l'ordre inverse de leur ordre wire. */
static bool lm_build_result(OwlBerWriter* w, int message_id,
                            unsigned char app_tag, int result_code) {
    owl_ber_writer_init(w);
    size_t mark = owl_ber_writer_len(w);
    if (!owl_ber_write_octet_string(w, (const unsigned char*)"", 0)) return false; /* diag */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)"", 0)) return false; /* matchedDN */
    if (!owl_ber_write_enumerated(w, result_code)) return false;                   /* resultCode */
    if (!owl_ber_wrap(w, app_tag, owl_ber_writer_len(w) - mark)) return false;
    return owl_ldap_message_finish(w, message_id);
}

/* PartialAttribute { type, vals SET OF value }. Ordre des valeurs preserve. */
static bool lm_write_attr(OwlBerWriter* w, const char* type,
                          const char* const* vals, size_t n) {
    size_t mark = owl_ber_writer_len(w);
    size_t vmark = owl_ber_writer_len(w);
    for (size_t i = n; i-- > 0; ) {   /* reverse => ordre serveur v0..v(n-1) */
        if (!owl_ber_write_octet_string(w, (const unsigned char*)vals[i],
                                        strlen(vals[i]))) return false;
    }
    if (!owl_ber_wrap(w, 0x31, owl_ber_writer_len(w) - vmark)) return false;  /* SET OF */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)type, strlen(type)))
        return false;
    return owl_ber_wrap_sequence(w, owl_ber_writer_len(w) - mark);            /* PartialAttribute */
}

/* SearchResultEntry canned pour l'objet "bob". */
static bool lm_build_entry(OwlBerWriter* w, int message_id) {
    static const char* const groups[] = {
        "CN=Admins,DC=example,DC=com",
        "CN=Users,DC=example,DC=com"
    };
    static const char* const vSam[]  = { "bob" };
    static const char* const vDisp[] = { "Bob Smith" };
    const char* dn = "CN=bob,OU=users,DC=example,DC=com";

    owl_ber_writer_init(w);
    size_t emark = owl_ber_writer_len(w);
    size_t amark = owl_ber_writer_len(w);
    /* Ecrits a rebours : ordre wire final = sAMAccountName, displayName, memberOf. */
    if (!lm_write_attr(w, "memberOf", groups, 2)) return false;
    if (!lm_write_attr(w, "displayName", vDisp, 1)) return false;
    if (!lm_write_attr(w, "sAMAccountName", vSam, 1)) return false;
    if (!owl_ber_wrap_sequence(w, owl_ber_writer_len(w) - amark)) return false;  /* attributes */
    if (!owl_ber_write_octet_string(w, (const unsigned char*)dn, strlen(dn)))    /* objectName */
        return false;
    if (!owl_ber_wrap(w, OWL_LDAP_OP_SEARCH_RESULT_ENTRY,
                      owl_ber_writer_len(w) - emark)) return false;
    return owl_ldap_message_finish(w, message_id);
}

/* ================================================================
 * Dialogue
 * ================================================================ */

static void lm_handle_bind(LdapMockServer* m, OwlTcpConn* conn,
                           const OwlLdapMessageHeader* hdr) {
    m->saw_bind++;

    const unsigned char* p = hdr->opContent;
    const unsigned char* end = hdr->opContent + hdr->opLen;
    unsigned char tag;
    const unsigned char *v;
    size_t vlen;

    const unsigned char* dn = NULL; size_t dn_len = 0;
    const unsigned char* pw = NULL; size_t pw_len = 0;

    /* version INTEGER */
    if (lm_tlv(&p, end, &tag, &v, &vlen) && tag == 0x02) {
        /* name LDAPDN */
        if (lm_tlv(&p, end, &tag, &v, &vlen) && tag == 0x04) {
            dn = v; dn_len = vlen;
            /* authentication [0] simple */
            if (lm_tlv(&p, end, &tag, &v, &vlen) && tag == 0x80) {
                pw = v; pw_len = vlen;
            }
        }
    }

    if (dn) {
        size_t cp = dn_len < sizeof(m->last_bind_dn) - 1 ? dn_len
                                                         : sizeof(m->last_bind_dn) - 1;
        memcpy(m->last_bind_dn, dn, cp);
        m->last_bind_dn[cp] = '\0';
    }

    bool dn_ok = dn && dn_len == strlen(m->bind_dn)
                 && memcmp(dn, m->bind_dn, dn_len) == 0;
    bool pw_ok = (pw_len == strlen(m->bind_password))
                 && (pw_len == 0 || memcmp(pw, m->bind_password, pw_len) == 0);
    int rc = (dn_ok && pw_ok) ? OWL_LDAP_SUCCESS : OWL_LDAP_INVALID_CREDENTIALS;

    OwlBerWriter w;
    if (lm_build_result(&w, hdr->messageID, OWL_LDAP_OP_BIND_RESPONSE, rc))
        lm_send(conn, &w);
    owl_ber_writer_free(&w);
}

static void lm_handle_search(LdapMockServer* m, OwlTcpConn* conn,
                             const OwlLdapMessageHeader* hdr) {
    m->search_count++;

    /* baseObject = premier OCTET STRING du SearchRequest. */
    const unsigned char* p = hdr->opContent;
    const unsigned char* end = hdr->opContent + hdr->opLen;
    unsigned char tag;
    const unsigned char* v;
    size_t vlen;
    bool have_base = false;
    if (lm_tlv(&p, end, &tag, &v, &vlen) && tag == 0x04) {
        have_base = true;
        size_t cp = vlen < sizeof(m->last_base) - 1 ? vlen : sizeof(m->last_base) - 1;
        memcpy(m->last_base, v, cp);
        m->last_base[cp] = '\0';
    }

    /* Matching grossier : une entree canned des que le baseObject est
     * non vide (le but est de tester le client, pas l'annuaire). */
    if (have_base && vlen > 0) {
        OwlBerWriter we;
        if (lm_build_entry(&we, hdr->messageID))
            lm_send(conn, &we);
        owl_ber_writer_free(&we);
    }

    OwlBerWriter wd;
    if (lm_build_result(&wd, hdr->messageID, OWL_LDAP_OP_SEARCH_RESULT_DONE,
                        OWL_LDAP_SUCCESS))
        lm_send(conn, &wd);
    owl_ber_writer_free(&wd);
}

static void lm_serve(LdapMockServer* m, OwlTcpConn* conn) {
    LmRx rx;
    rx.len = 0;
    rx.start = 0;

    for (;;) {
        const unsigned char* msg;
        size_t msglen;
        int r = lm_read_message(conn, &rx, &msg, &msglen);
        if (r != 1) break;   /* EOF / timeout / malforme */

        OwlLdapMessageHeader hdr;
        if (!owl_ldap_message_parse(msg, msglen, &hdr)) break;

        switch (hdr.protocolOpTag) {
            case OWL_LDAP_OP_BIND_REQUEST:
                lm_handle_bind(m, conn, &hdr);
                break;
            case OWL_LDAP_OP_SEARCH_REQUEST:
                lm_handle_search(m, conn, &hdr);
                break;
            case OWL_LDAP_OP_UNBIND_REQUEST:
                m->saw_unbind++;
                return;   /* unbind = fin de session */
            default:
                /* protocolOp non gere par le MVP : on ignore. */
                break;
        }
    }
}

/* ================================================================
 * Thread
 * ================================================================ */

#ifdef _WIN32
static DWORD WINAPI lm_thread_fn(LPVOID arg) {
#else
static void* lm_thread_fn(void* arg) {
#endif
    LdapMockServer* m = (LdapMockServer*)arg;

    owl_tcp_global_init();   /* idempotent */

    OwlTcpListener l;
    if (owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 1, false)) {
        m->port = owl_tcp_listener_port(&l);
        m->ready = 1;
        OwlTcpConn conn;
        int rc = owl_tcp_listener_accepte(&l, 5000, &conn);
        if (rc == OWL_TCPS_OK) {
            lm_serve(m, &conn);
            owl_tcp_conn_ferme(&conn);
        }
        owl_tcp_listener_ferme(&l);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================
 * API publique
 * ================================================================ */

void ldap_mock_config_defaut(LdapMockConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
}

LdapMockServer* ldap_mock_server_start(const LdapMockConfig* cfg) {
    LdapMockServer* m = (LdapMockServer*)calloc(1, sizeof(LdapMockServer));
    if (!m) return NULL;
    snprintf(m->bind_dn, sizeof(m->bind_dn), "%s", cfg->bind_dn ? cfg->bind_dn : "");
    snprintf(m->bind_password, sizeof(m->bind_password), "%s",
             cfg->bind_password ? cfg->bind_password : "");

#ifdef _WIN32
    m->thread = CreateThread(NULL, 0, lm_thread_fn, m, 0, NULL);
    if (m->thread == NULL) { free(m); return NULL; }
#else
    if (pthread_create(&m->thread, NULL, lm_thread_fn, m) != 0) { free(m); return NULL; }
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

int ldap_mock_server_port(const LdapMockServer* m)     { return m->port; }
int ldap_mock_server_is_ready(const LdapMockServer* m) { return m->ready; }

void ldap_mock_server_join(LdapMockServer* m) {
#ifdef _WIN32
    WaitForSingleObject(m->thread, 10000);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread, NULL);
#endif
}

void ldap_mock_server_free(LdapMockServer* m) {
    if (!m) return;
    free(m);
}

const char* ldap_mock_server_last_bind_dn(const LdapMockServer* m) { return m->last_bind_dn; }
int         ldap_mock_server_saw_bind(const LdapMockServer* m)     { return m->saw_bind; }
int         ldap_mock_server_saw_unbind(const LdapMockServer* m)   { return m->saw_unbind; }
int         ldap_mock_server_search_count(const LdapMockServer* m) { return m->search_count; }
const char* ldap_mock_server_last_base(const LdapMockServer* m)    { return m->last_base; }
