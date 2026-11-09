// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_client.h"
#include "ldap_ber.h"
#include "ldap_filter.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Transport : dispatch clair / TLS. Les deux chemins partagent la meme
 * semantique que net_tcp (send = tout ou -1 ; recv = octets>0, 0 EOF,
 * <0 erreur).
 * ================================================================ */

static int client_send(OwlLdapClient* c, const void* buf, size_t len) {
    return c->use_tls ? owl_tls_send(&c->tls, buf, len)
                      : owl_tcp_send(&c->tcp, buf, len);
}

static int client_recv(OwlLdapClient* c, void* buf, size_t len) {
    return c->use_tls ? owl_tls_recv(&c->tls, buf, len)
                      : owl_tcp_recv(&c->tcp, buf, len);
}

static void set_error(OwlLdapClient* c, const char* msg) {
    snprintf(c->last_error, sizeof(c->last_error), "%s", msg);
}

/* Diagnostic serveur (non nul-termine, pointe dans le buffer de reception) :
 * copie bornee dans last_error pour remonter le sous-code AD (ex. "data 52e").
 * Silencieux si le message est vide. */
static void set_error_diag(OwlLdapClient* c,
                           const unsigned char* diag, size_t diag_len) {
    if (!diag || diag_len == 0) return;
    if (diag_len >= sizeof(c->last_error)) diag_len = sizeof(c->last_error) - 1;
    memcpy(c->last_error, diag, diag_len);
    c->last_error[diag_len] = '\0';
}

/* ================================================================
 * Decodage de la longueur BER d'un en-tete (framing de flux).
 *
 * data[0] = tag, data[1..] = longueur (forme courte < 0x80, sinon forme
 * longue : bits bas = nombre d'octets de longueur). La forme indefinie
 * (0x80) est rejetee (AD ne l'emet pas). Renvoie :
 *   1  : total = en-tete + contenu decode (tient dans *out_total)
 *   0  : pas encore assez d'octets pour decoder l'en-tete
 *  -1  : en-tete malforme (indefinie, debordement, longueur aberrante)
 * ================================================================ */

#define LDAP_MSG_MAX (16u * 1024u * 1024u)  /* garde-fou anti-OOM */

static int frame_total_len(const unsigned char* data, size_t avail,
                           size_t* out_total) {
    if (avail < 2) return 0;
    unsigned char l0 = data[1];
    if (l0 < 0x80) {
        *out_total = 2 + (size_t)l0;
        return 1;
    }
    unsigned int nbytes = l0 & 0x7F;
    if (nbytes == 0 || nbytes > 4) return -1;   /* indefinie / aberrant */
    if (avail < 2 + nbytes) return 0;
    size_t content = 0;
    for (unsigned int i = 0; i < nbytes; i++)
        content = (content << 8) | data[2 + i];
    if (content > LDAP_MSG_MAX) return -1;
    *out_total = 2 + nbytes + content;
    return 1;
}

/* ================================================================
 * Accumulateur de reception. Lit depuis la socket par paquets et extrait
 * les LDAPMessage complets un a un, en gerant la fragmentation (un
 * message peut arriver en plusieurs recv, et un recv peut contenir
 * plusieurs messages).
 * ================================================================ */

typedef struct {
    unsigned char* buf;
    size_t len;    /* octets presents */
    size_t cap;
    size_t start;  /* debut des octets non consommes */
} LdapRx;

static void rx_init(LdapRx* r) { memset(r, 0, sizeof(*r)); }
static void rx_free(LdapRx* r) { free(r->buf); memset(r, 0, sizeof(*r)); }

static bool rx_ensure(LdapRx* r, size_t extra) {
    if (r->len + extra <= r->cap) return true;
    size_t ncap = r->cap ? r->cap * 2 : 4096;
    while (ncap < r->len + extra) ncap *= 2;
    unsigned char* nb = (unsigned char*)realloc(r->buf, ncap);
    if (!nb) return false;
    r->buf = nb;
    r->cap = ncap;
    return true;
}

/* Extrait le prochain LDAPMessage complet. *out pointe DANS r->buf et
 * reste valide jusqu'au prochain rx_next_message (qui peut compacter ou
 * reallouer). Renvoie true + (*out,*out_len) sur succes, false sur
 * EOF/erreur de transport ou PDU malformee (c->last_error rempli). */
static bool rx_next_message(OwlLdapClient* c, LdapRx* r,
                            const unsigned char** out, size_t* out_len) {
    for (;;) {
        size_t avail = r->len - r->start;
        size_t total = 0;
        int fr = frame_total_len(r->buf + r->start, avail, &total);
        if (fr < 0) { set_error(c, "reponse LDAP malformee (longueur BER)"); return false; }
        if (fr == 1 && avail >= total) {
            *out = r->buf + r->start;
            *out_len = total;
            r->start += total;
            return true;
        }
        /* Compacte (ramene les octets non consommes au debut) pour borner
         * la croissance, puis lit un paquet de plus. */
        if (r->start > 0) {
            memmove(r->buf, r->buf + r->start, avail);
            r->len = avail;
            r->start = 0;
        }
        if (!rx_ensure(r, 4096)) { set_error(c, "memoire epuisee (reception)"); return false; }
        int n = client_recv(c, r->buf + r->len, r->cap - r->len);
        if (n <= 0) {
            set_error(c, n == 0 ? "connexion fermee par le serveur"
                                : "erreur de lecture reseau");
            return false;
        }
        r->len += (size_t)n;
    }
}

/* ================================================================
 * Buffer de reponse possede : copie stable sur laquelle les entrees
 * zero-copie restent valides.
 * ================================================================ */

static bool resp_append(OwlLdapClient* c, const unsigned char* data, size_t len,
                        size_t* out_off) {
    if (c->resp_len + len > c->resp_cap) {
        size_t ncap = c->resp_cap ? c->resp_cap * 2 : 4096;
        while (ncap < c->resp_len + len) ncap *= 2;
        unsigned char* nb = (unsigned char*)realloc(c->resp_buf, ncap);
        if (!nb) return false;
        c->resp_buf = nb;
        c->resp_cap = ncap;
    }
    *out_off = c->resp_len;
    memcpy(c->resp_buf + c->resp_len, data, len);
    c->resp_len += len;
    return true;
}

/* ================================================================
 * API
 * ================================================================ */

void owl_ldap_client_init(OwlLdapClient* c) {
    memset(c, 0, sizeof(*c));
    c->next_message_id = 1;
    c->result.resultCode = -1;
}

const char* owl_ldap_client_last_error(const OwlLdapClient* c) {
    return c->last_error;
}

void owl_ldap_client_reset(OwlLdapClient* c) {
    if (c->has_result) {
        owl_ldap_search_result_free(&c->result);
        c->has_result = false;
    }
    free(c->resp_buf);
    c->resp_buf = NULL;
    c->resp_len = 0;
    c->resp_cap = 0;
    c->result.resultCode = -1;
}

static void client_transport_close(OwlLdapClient* c) {
    if (!c->connected) return;
    if (c->use_tls) owl_tls_close(&c->tls);
    else            owl_tcp_close(&c->tcp);
    c->connected = false;
}

bool owl_ldap_client_open(OwlLdapClient* c, const char* host, int port,
                          bool use_tls, int verify_level,
                          const char* extra_ca_path,
                          const char* client_cert_path,
                          const char* client_key_path,
                          int timeout_ms) {
    c->use_tls = use_tls;
    c->timeout_ms = timeout_ms;
    c->last_error[0] = '\0';

    if (use_tls) {
        owl_tls_init(&c->tls);
        if (!owl_tls_connect(&c->tls, host, port, verify_level, timeout_ms,
                             extra_ca_path, client_cert_path, client_key_path)) {
            snprintf(c->last_error, sizeof(c->last_error),
                     "connexion TLS echouee: %s", owl_tls_last_error(&c->tls));
            return false;
        }
    } else {
        owl_tcp_init(&c->tcp);
        if (!owl_tcp_connect(&c->tcp, host, port, timeout_ms)) {
            snprintf(c->last_error, sizeof(c->last_error),
                     "connexion TCP echouee: %s", owl_tcp_last_error(&c->tcp));
            return false;
        }
    }
    c->connected = true;
    return true;
}

bool owl_ldap_client_connect(OwlLdapClient* c, const char* host, int port,
                             bool use_tls, int verify_level,
                             const char* extra_ca_path,
                             const char* client_cert_path,
                             const char* client_key_path,
                             const char* dn,
                             const unsigned char* password, size_t password_len,
                             bool sasl_external,
                             int version,
                             int timeout_ms, int* out_result_code) {
    if (!owl_ldap_client_open(c, host, port, use_tls, verify_level,
                              extra_ca_path, client_cert_path, client_key_path,
                              timeout_ms)) {
        return false;
    }

    /* Bind : simple (DN + password) ou SASL EXTERNAL (name vide, identite
     * portee par le cert client presente au handshake). */
    OwlBerWriter w;
    owl_ber_writer_init(&w);
    const char* bind_dn = sasl_external ? "" : dn;
    const char* mechanism = sasl_external ? "EXTERNAL" : NULL;
    if (!owl_ldap_encode_bind_request(&w, c->next_message_id++, version, bind_dn,
                                      password, password_len, mechanism)) {
        owl_ber_writer_free(&w);
        set_error(c, "encodage BindRequest echoue");
        client_transport_close(c);
        return false;
    }
    int rc = client_send(c, owl_ber_writer_data(&w), owl_ber_writer_len(&w));
    owl_ber_writer_free(&w);
    if (rc < 0) {
        set_error(c, "envoi BindRequest echoue");
        client_transport_close(c);
        return false;
    }

    /* Lit le BindResponse. */
    LdapRx rx;
    rx_init(&rx);
    const unsigned char* msg;
    size_t msglen;
    if (!rx_next_message(c, &rx, &msg, &msglen)) {
        rx_free(&rx);
        client_transport_close(c);
        return false;
    }
    OwlLdapMessageHeader hdr;
    if (!owl_ldap_message_parse(msg, msglen, &hdr)
        || hdr.protocolOpTag != OWL_LDAP_OP_BIND_RESPONSE) {
        rx_free(&rx);
        set_error(c, "BindResponse attendu (tag inattendu)");
        client_transport_close(c);
        return false;
    }
    OwlLdapResult res;
    bool ok = owl_ldap_parse_result(hdr.opContent, hdr.opLen, &res);
    rx_free(&rx);
    if (!ok) {
        set_error(c, "BindResponse malforme (resultCode illisible)");
        client_transport_close(c);
        return false;
    }
    *out_result_code = res.resultCode;
    if (res.resultCode != OWL_LDAP_SUCCESS)
        set_error_diag(c, res.diagnosticMessage, res.diagnosticMessageLen);
    return true;
}

/* Borne anti-boucle du ping-pong SASL. GSS-SPNEGO (Kerberos, repli NTLM)
 * converge en 2-4 tours ; une marge genereuse couvre les variantes sans
 * risquer une boucle infinie face a un serveur qui repondrait toujours
 * saslBindInProgress. */
#define LDAP_SASL_MAX_ROUNDS 12

bool owl_ldap_client_sasl_bind(OwlLdapClient* c, const char* mechanism,
                               OwlLdapTokenProvider* provider,
                               const unsigned char* channel_binding,
                               size_t cb_len,
                               int version, int* out_result_code) {
    if (!c->connected) { set_error(c, "session fermee"); return false; }
    c->last_error[0] = '\0';

    OwlLdapSaslToken st = provider->init(provider->self, channel_binding, cb_len);
    if (!st.ok) { set_error(c, "fournisseur de jetons SASL: echec init"); return false; }

    LdapRx rx;
    rx_init(&rx);

    for (int round = 0; round < LDAP_SASL_MAX_ROUNDS; round++) {
        /* Emet BindRequest(mechanism, jeton courant). name vide : l'identite
         * vient du contexte de securite negocie, pas du DN. */
        OwlBerWriter w;
        owl_ber_writer_init(&w);
        if (!owl_ldap_encode_bind_request_sasl(&w, c->next_message_id++, version,
                                               "", mechanism,
                                               st.token, st.token_len)) {
            owl_ber_writer_free(&w);
            rx_free(&rx);
            set_error(c, "encodage BindRequest SASL echoue");
            return false;
        }
        int rc = client_send(c, owl_ber_writer_data(&w), owl_ber_writer_len(&w));
        owl_ber_writer_free(&w);
        if (rc < 0) {
            rx_free(&rx);
            set_error(c, "envoi BindRequest SASL echoue");
            return false;
        }

        /* Lit le BindResponse. */
        const unsigned char* msg;
        size_t msglen;
        if (!rx_next_message(c, &rx, &msg, &msglen)) {
            rx_free(&rx);
            return false;  /* last_error deja rempli par rx_next_message */
        }
        OwlLdapMessageHeader hdr;
        if (!owl_ldap_message_parse(msg, msglen, &hdr)
            || hdr.protocolOpTag != OWL_LDAP_OP_BIND_RESPONSE) {
            rx_free(&rx);
            set_error(c, "BindResponse attendu (tag inattendu)");
            return false;
        }
        OwlLdapResult res;
        if (!owl_ldap_parse_result(hdr.opContent, hdr.opLen, &res)) {
            rx_free(&rx);
            set_error(c, "BindResponse malforme (resultCode illisible)");
            return false;
        }
        if (res.resultCode != OWL_LDAP_SASL_BIND_IN_PROGRESS) {
            /* Reponse definitive : succes(0) ou erreur de bind (ex. 49). Un
             * bind refuse n'est pas une erreur de transport. */
            *out_result_code = res.resultCode;
            if (res.resultCode != OWL_LDAP_SUCCESS)
                set_error_diag(c, res.diagnosticMessage, res.diagnosticMessageLen);
            rx_free(&rx);
            return true;
        }

        /* saslBindInProgress : le serveur attend un jeton de plus. */
        if (st.done) {
            rx_free(&rx);
            set_error(c, "serveur demande un jeton SASL mais le fournisseur a termine");
            return false;
        }
        /* res.serverSaslCreds pointe dans rx->buf : il est consomme par step()
         * AVANT le prochain rx_next_message (qui peut compacter/realloc rx). */
        st = provider->step(provider->self,
                            res.serverSaslCreds, res.serverSaslCredsLen);
        if (!st.ok) {
            rx_free(&rx);
            set_error(c, "fournisseur de jetons SASL: echec step");
            return false;
        }
    }

    rx_free(&rx);
    set_error(c, "bind SASL: nombre de tours depasse (anti-boucle)");
    return false;
}

/* Coeur partage search / read_object : filtre deja choisi, scope donne. */
static const OwlLdapSearchResult* client_do_search(
    OwlLdapClient* c, const char* base_dn, int scope,
    const char* filter_str,
    const char* const* attributes, size_t attr_count) {

    if (!c->connected) { set_error(c, "session fermee"); return NULL; }
    c->last_error[0] = '\0';

    /* Repart propre : libere le resultat precedent. */
    owl_ldap_client_reset(c);

    /* Compile le filtre en bloc BER opaque. */
    OwlBerWriter fw;
    owl_ber_writer_init(&fw);
    if (!owl_ldap_filter_encode(&fw, filter_str)) {
        owl_ber_writer_free(&fw);
        set_error(c, "filtre LDAP invalide");
        return NULL;
    }

    /* Encode le SearchRequest (filtre injecte opaque). */
    OwlBerWriter sw;
    owl_ber_writer_init(&sw);
    bool enc = owl_ldap_encode_search_request(
        &sw, c->next_message_id++, base_dn, scope,
        owl_ber_writer_data(&fw), owl_ber_writer_len(&fw),
        attributes, attr_count);
    owl_ber_writer_free(&fw);
    if (!enc) {
        owl_ber_writer_free(&sw);
        set_error(c, "encodage SearchRequest echoue");
        return NULL;
    }
    int rc = client_send(c, owl_ber_writer_data(&sw), owl_ber_writer_len(&sw));
    owl_ber_writer_free(&sw);
    if (rc < 0) { set_error(c, "envoi SearchRequest echoue"); return NULL; }

    /* Lit la suite de reponses jusqu'au SearchResultDone. On copie chaque
     * message dans le buffer possede (resp_buf) AVANT d'en parser le
     * contenu : la copie est stable (resp_buf ne reallouera plus une fois
     * la boucle finie), donc les entrees zero-copie y pointent sans risque
     * d'invalidation. On memorise les offsets, et on parse en 2e passe. */
    typedef struct { size_t off; size_t len; } MsgRef;
    MsgRef* refs = NULL;
    size_t refs_n = 0, refs_cap = 0;

    LdapRx rx;
    rx_init(&rx);
    bool done = false, failed = false;

    while (!done) {
        const unsigned char* msg;
        size_t msglen;
        if (!rx_next_message(c, &rx, &msg, &msglen)) { failed = true; break; }

        /* Tag lu sur rx (transitoire) pour decider quand s'arreter. */
        OwlLdapMessageHeader hdr;
        if (!owl_ldap_message_parse(msg, msglen, &hdr)) {
            set_error(c, "LDAPMessage de reponse malforme");
            failed = true; break;
        }
        unsigned char tag = hdr.protocolOpTag;

        /* On ne garde que les entrees et le Done ; les autres protocolOp
         * (ex. SearchResultReference) sont ignores dans le MVP. */
        if (tag == OWL_LDAP_OP_SEARCH_RESULT_ENTRY
            || tag == OWL_LDAP_OP_SEARCH_RESULT_DONE) {
            size_t off;
            if (!resp_append(c, msg, msglen, &off)) {
                set_error(c, "memoire epuisee (buffer reponse)");
                failed = true; break;
            }
            if (refs_n == refs_cap) {
                size_t nc = refs_cap ? refs_cap * 2 : 8;
                MsgRef* nr = (MsgRef*)realloc(refs, nc * sizeof(*nr));
                if (!nr) { set_error(c, "memoire epuisee (index messages)"); failed = true; break; }
                refs = nr; refs_cap = nc;
            }
            refs[refs_n].off = off;
            refs[refs_n].len = msglen;
            refs_n++;
        }
        if (tag == OWL_LDAP_OP_SEARCH_RESULT_DONE) done = true;
    }
    rx_free(&rx);

    if (failed) { free(refs); owl_ldap_client_reset(c); return NULL; }

    /* 2e passe : resp_buf est desormais stable. On parse chaque message
     * depuis sa copie et on agrege. */
    owl_ldap_search_result_init(&c->result);
    c->has_result = true;
    for (size_t i = 0; i < refs_n; i++) {
        const unsigned char* m = c->resp_buf + refs[i].off;
        OwlLdapMessageHeader hdr;
        if (!owl_ldap_message_parse(m, refs[i].len, &hdr)) continue;
        if (hdr.protocolOpTag == OWL_LDAP_OP_SEARCH_RESULT_ENTRY) {
            owl_ldap_search_result_add_entry(&c->result, hdr.opContent, hdr.opLen);
        } else if (hdr.protocolOpTag == OWL_LDAP_OP_SEARCH_RESULT_DONE) {
            owl_ldap_search_result_set_done(&c->result, hdr.opContent, hdr.opLen);
        }
    }
    free(refs);
    return &c->result;
}

const OwlLdapSearchResult* owl_ldap_client_search(
    OwlLdapClient* c, const char* base_dn, int scope,
    const char* filter_str,
    const char* const* attributes, size_t attr_count) {
    return client_do_search(c, base_dn, scope, filter_str, attributes, attr_count);
}

const OwlLdapSearchResult* owl_ldap_client_read_object(
    OwlLdapClient* c, const char* dn,
    const char* const* attributes, size_t attr_count) {
    /* LDAPValeur/LDAPNbValeur = lecture des attributs d'un DN : recherche
     * scope baseObject sur dn, filtre present "(objectClass=*)". */
    return client_do_search(c, dn, OWL_LDAP_SCOPE_BASE_OBJECT,
                            "(objectClass=*)", attributes, attr_count);
}

void owl_ldap_client_close(OwlLdapClient* c) {
    if (c->connected) {
        /* UnbindRequest best effort : on ignore une erreur d'envoi, la
         * fermeture socket suit de toute facon. */
        OwlBerWriter w;
        owl_ber_writer_init(&w);
        if (owl_ldap_encode_unbind_request(&w, c->next_message_id++)) {
            client_send(c, owl_ber_writer_data(&w), owl_ber_writer_len(&w));
        }
        owl_ber_writer_free(&w);
        client_transport_close(c);
    }
    owl_ldap_client_reset(c);
}
