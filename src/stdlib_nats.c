// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Client NATS, Lot 1 — le coeur. Voir stdlib_nats.h pour le
 * contrat.
 *
 * ETAT : client complet round-trip pub/sub en TCP clair. NATSConnecte ouvre le
 * transport, lit l'INFO non sollicite du serveur, emet un CONNECT {options}
 * (verbose:false, pedantic:false, tls_required:false, name, user/pass si
 * fournis) puis un PING, et attend le PONG (verbose=false -> pas de +OK par
 * PUB/SUB, seul le PING/PONG confirme la poignee de main). NATSPublie emet un
 * PUB (sujet + repondre-a optionnel + #octets + corps). NATSAbonne emet un SUB
 * (sujet + groupeFile optionnel + sid genere), memorise le mapping sid -> handle
 * E1 et branche le reacteur ; un MSG entrant (frame la pompe) construit un
 * natsMessage et invoque la procedure OWL abonnee via le socle E1 (2 args :
 * session, message). NATSDéconnecte ferme le transport. La connexion vivante est
 * stockee dans l'instance natsConnexion (champ interne _handle -> OwlNatsConn*).
 *
 * La reception est pompee depuis OWL par NATSAttend : la session
 * POSSEDE son reacteur (net_reacteur), cree paresseusement au 1er abonnement /
 * a la 1re attente, et NATSAttend le ticke pendant un delai borne. La pompe
 * repond au keepalive PING du serveur par un PONG (sinon nats-server ferme la
 * connexion sur timeout) et avale les PONG/+OK/-ERR serveur.
 *
 * Le wire NATS (lignes texte \r\n) n'a pas d'equivalent canon : reference =
 * protocole client NATS (nats.io/docs). Ligne de controle : VERBE [args]\r\n.
 * MSG <sujet> <sid> [repondre-a] <#octets>\r\n<payload>\r\n : le repondre-a est
 * optionnel -> la ligne MSG a 4 OU 5 tokens.
 *
 * TLS (optionSSL) : NATS negocie le chiffrement IN-BAND (contrairement a
 * AMQP/STOMP sur port dedie). La connexion s'ouvre TOUJOURS en TCP clair, le
 * serveur annonce ses capacites (dont tls_required) dans l'INFO, puis le client
 * bascule la socket existante en TLS (STARTTLS, owl_tls_start_tls) AVANT le
 * CONNECT si optionSSL est demande. VERIFY_FULL conserve, CertificatCA fournit
 * une CA privee optionnelle. Le transport unifie route send/recv/close par
 * tls_active. */

#include "stdlib_nats.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
/* net_tcp.h tire winsock2.h sous Windows -> collision TokenType (winnt.h vs
 * scanner.h). Meme garde que stdlib_stomp.c / stdlib_amqp.c. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "vm.h"
#include "object.h"
#include "value.h"
#include "table.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"
#include "net_reacteur.h"
#include "stdlib_mq.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* Port par defaut NATS (protocole client). */
#define NATS_PORT_DEFAULT 4222
/* Valeur de la constante optionSSL (masque OU-binaire sur ..Option). La
 * constante elle-meme est deja enregistree globalement (par le client MQTT) :
 * on la reutilise, on ne la re-enregistre pas (invariant registry). */
#define OWL_OPTION_SSL 1
/* Delai d'ouverture du transport en millisecondes (defaut si TimeoutConnexion
 * n'est pas renseigne cote instance). */
#define NATS_CONNECT_TIMEOUT_MS 10000
/* Nombre maximal d'abonnements simultanes sur une session (racine GC partagee
 * bornee par le socle E1). Depassement -> abonnement refuse. */
#define NATS_MAX_SUBS 8

/* Horloge monotone en millisecondes, partagee avec le socle net (definie dans
 * net_tcp_server.c). Borne la pompe NATSAttend. Extern-declaree comme dans
 * stdlib_stomp.c pour ne pas tirer winsock via net_tcp_server.h. */
extern uint64_t owl_tcp_server_horloge_ms(void);

/* ============================================================
 * Connexion vivante — pointeur opaque cache dans l'instance natsConnexion
 * (champ _handle, cast intptr_t).
 * ============================================================ */

/* Mapping abonnement -> handle E1, par session. Un MSG entrant retrouve le
 * handler par le `sid` genere a l'abonnement. Les copies sont C-heap. */
typedef struct {
    char*       sid;       /* copie malloc du sid genere */
    char*       subject;   /* copie malloc du sujet abonne */
    OwlMqHandle handle;    /* handle E1 tenant le handler comme racine GC */
} NatsSub;

typedef struct {
    OwlTlsSocket transport;      /* .tcp = TCP sous-jacent (clair ou chiffre par
                                  * STARTTLS in-band, cf. fn_nats_connecte) */
    bool         tls_active;     /* upgrade STARTTLS effectue -> router via owl_tls_* */
    bool         connected;      /* poignee de main PING/PONG effectuee */
    int          next_sid;       /* compteur d'ids d'abonnement */

    ObjInstance* session;        /* instance natsConnexion proprietaire ; passee en
                                  * arg 1 de la livraison. Rootee par l'appelant
                                  * (portee OWL), non scannee via ce champ C. */
    NatsSub      subs[NATS_MAX_SUBS];
    int          nb_subs;

    /* --- Pompe possedee par la session (, calque STOMP) --- */
    OwlReacteur* reacteur;       /* reacteur PROPRE, cree paresseusement. */
    bool         fd_branche;     /* socket vivante deja enregistree en LECTURE */

    /* Tampon d'accumulation : un recv peut couper une ligne en deux ou en
     * agreger plusieurs. On accumule puis on decoupe ligne par ligne. */
    uint8_t*     rx;
    size_t       rx_len;
    size_t       rx_cap;

    /* --- Request-reply : capture INTERNE de la reponse d'inbox ---
     * Pendant une requete, un abonnement inbox dedie est arme avec un sid
     * marque ici. Un MSG entrant portant ce sid n'est PAS dispatche a une
     * procedure OWL (E1) : il est capture directement dans `inbox_reponse`.
     * Les MSG des AUTRES sid (abonnements user, ex le repondeur) continuent
     * d'etre livres normalement via nats_livre pendant les memes ticks. */
    bool         inbox_actif;    /* une requete est en cours -> rx capture au lieu de livrer */
    bool         inbox_recu;     /* la reponse d'inbox est arrivee */
    char         inbox_sid[32];  /* sid de l'abonnement inbox interne */
    ObjInstance* inbox_reponse;  /* natsMessage capture, tenu vivant par le champ
                                  * _inbox_reponse de la session (reachable GC). */
} OwlNatsConn;

static int nats_send(OwlNatsConn* c, const void* buf, size_t len) {
    return c->tls_active ? owl_tls_send(&c->transport, buf, len)
                         : owl_tcp_send(&c->transport.tcp, buf, len);
}

static int nats_recv(OwlNatsConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv(&c->transport, buf, len)
                         : owl_tcp_recv(&c->transport.tcp, buf, len);
}

static void nats_conn_free(OwlNatsConn* c) {
    if (c == NULL) return;
    for (int i = 0; i < c->nb_subs; i++) {
        owl_mq_desabonne(c->subs[i].handle);
        free(c->subs[i].sid);
        free(c->subs[i].subject);
    }
    if (c->reacteur != NULL) owl_reacteur_detruit(c->reacteur);
    free(c->rx);
    if (c->tls_active) owl_tls_close(&c->transport);
    else               owl_tcp_close(&c->transport.tcp);
    free(c);
}

static NatsSub* nats_sub_find(OwlNatsConn* c, const char* sid) {
    if (sid == NULL || sid[0] == '\0') return NULL;
    for (int i = 0; i < c->nb_subs; i++) {
        if (strcmp(c->subs[i].sid, sid) == 0) return &c->subs[i];
    }
    return NULL;
}

static bool nats_sub_add(OwlNatsConn* c, const char* sid, const char* subject,
                         OwlMqHandle h) {
    if (c->nb_subs >= NATS_MAX_SUBS) return false;
    char* csid = strdup(sid ? sid : "");
    char* csub = strdup(subject ? subject : "");
    if (csid == NULL || csub == NULL) { free(csid); free(csub); return false; }
    c->subs[c->nb_subs].sid     = csid;
    c->subs[c->nb_subs].subject = csub;
    c->subs[c->nb_subs].handle  = h;
    c->nb_subs++;
    return true;
}

/* ============================================================
 * Tampon octet extensible pour assembler une ligne de controle.
 * ============================================================ */

typedef struct { uint8_t* data; size_t len, cap; } NatsBuf;

static bool nbuf_ensure(NatsBuf* b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t* nb = (uint8_t*)realloc(b->data, ncap);
    if (nb == NULL) return false;
    b->data = nb;
    b->cap = ncap;
    return true;
}

static void nbuf_bytes(NatsBuf* b, const void* p, size_t n) {
    if (n == 0) return;
    if (!nbuf_ensure(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void nbuf_str(NatsBuf* b, const char* s) { nbuf_bytes(b, s, strlen(s)); }

/* Ecrit une chaine JSON echappee (guillemets + backslash + contrôles usuels)
 * entre guillemets doubles. Suffisant pour user/pass/name du CONNECT. */
static void nbuf_json_string(NatsBuf* b, const char* s) {
    nbuf_bytes(b, "\"", 1);
    for (; s && *s; s++) {
        switch (*s) {
            case '"':  nbuf_bytes(b, "\\\"", 2); break;
            case '\\': nbuf_bytes(b, "\\\\", 2); break;
            case '\n': nbuf_bytes(b, "\\n", 2);  break;
            case '\r': nbuf_bytes(b, "\\r", 2);  break;
            case '\t': nbuf_bytes(b, "\\t", 2);  break;
            default:   nbuf_bytes(b, s, 1);      break;
        }
    }
    nbuf_bytes(b, "\"", 1);
}

static void nats_rx_append(OwlNatsConn* c, const uint8_t* data, size_t len) {
    if (c->rx_len + len > c->rx_cap) {
        size_t ncap = c->rx_cap ? c->rx_cap : 512;
        while (ncap < c->rx_len + len) ncap *= 2;
        uint8_t* nb = (uint8_t*)realloc(c->rx, ncap);
        if (nb == NULL) return;                      /* best-effort : bloc ignore */
        c->rx = nb;
        c->rx_cap = ncap;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
}

/* ============================================================
 * Templates des types reifies.
 * ============================================================ */

static ObjInstance* build_natsConnexion_template(void) {
    ObjString* name = obj_copy_string("natsConnexion", 13);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    vm_push(OBJ_VAL(tpl));   /* rooté pendant les sets (grow -> GC) */
    inst_field_set_str(tpl, "Adresse",          "");
    inst_field_set_int(tpl, "Port",             NATS_PORT_DEFAULT);
    inst_field_set_str(tpl, "Utilisateur",      "");
    inst_field_set_str(tpl, "MotDePasse",       "");
    inst_field_set_int(tpl, "Option",           0);
    inst_field_set_str(tpl, "CertificatCA",     "");   /* CA privee optionnelle pour TLS */
    inst_field_set_int(tpl, "TimeoutConnexion", 0);
    /* _handle : pointeur opaque vers OwlNatsConn* (cast intptr_t). 0 = non
     * connecte. Rempli par NATSConnecte, libere par NATSDéconnecte. Field
     * interne, non documente cote user. */
    inst_field_set_int(tpl, "_handle",          0);
    vm_pop();
    return tpl;
}

static ObjInstance* build_natsMessage_template(void) {
    ObjString* name = obj_copy_string("natsMessage", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    /* Construit aussi au runtime (une instance fraiche par livraison) : tpl doit
     * rester racine pendant les sets. gc-root-obj-fresh. */
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Sujet",     "");
    inst_field_set_str(tpl, "Corps",     "");   /* chaine (texte) ou Buffer en emission */
    inst_field_set_str(tpl, "RepondreA", "");
    vm_pop();
    return tpl;
}

/* ============================================================
 * Helpers — extraction natsConnexion + lecture de champs + handle.
 * ============================================================ */

static ObjInstance* resolve_nats_session(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "natsConnexion",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "natsConnexion") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "natsConnexion",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static const char* nats_str(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_STRING(v)) return "";
    return AS_CSTRING(v);
}

static int nats_int(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return 0;
    return (int)AS_INTEGER(v);
}

static OwlNatsConn* session_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return NULL;
    return (OwlNatsConn*)(intptr_t)AS_INTEGER(v);
}

static void session_set_handle(ObjInstance* inst, OwlNatsConn* c) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)c);
}

/* Corps d'un natsMessage : chaine (octets) ou Buffer (binaire). */
static void nats_msg_body(ObjInstance* msg, const unsigned char** out,
                          size_t* out_len) {
    Value v;
    ObjString* k = obj_copy_string("Corps", 5);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&msg->fields, k, &v);
    vm_pop();
    if (ok && IS_STRING(v)) {
        *out = (const unsigned char*)AS_STRING(v)->chars;
        *out_len = (size_t)AS_STRING(v)->length;
        return;
    }
    if (ok && IS_BUFFER(v)) {
        *out = AS_BUFFER(v)->bytes;
        *out_len = (size_t)AS_BUFFER(v)->length;
        return;
    }
    *out = (const unsigned char*)"";
    *out_len = 0;
}

/* ============================================================
 * Livraison C -> OWL : (session natsConnexion, message natsMessage).
 *
 * NATS construit un natsMessage frais (Sujet + Corps chaine + RépondreÀ) et le
 * passe au callback. E1 reste le registre + le rooting GC des handlers. Motif de
 * reentrance bornee (E1/http_exec) : repere stackTop -> push callee + args
 * (chacun roote a la creation) -> appel -> reset au repere. gc-root-obj-fresh.
 * ============================================================ */

static void nats_livre(OwlNatsConn* c, const char* sid, const char* subject,
                       const char* reply, const uint8_t* payload,
                       size_t payload_len) {
    NatsSub* sub = nats_sub_find(c, sid);
    if (sub == NULL) return;
    Value handler;
    if (!owl_mq_handler(sub->handle, &handler)) return;
    if (c->session == NULL) return;

    Value* repere = vm.stackTop;                    /* frontiere de depilage propre */
    vm_push(handler);                               /* callee au-dessus du repere */
    vm_push(OBJ_VAL(c->session));                   /* arg 1 : session */

    /* arg 2 : natsMessage frais. Instance rootee immediatement (push) : les
     * inst_field_set_* suivants allouent -> grow -> GC possible. */
    ObjInstance* msg = build_natsMessage_template();
    vm_push(OBJ_VAL(msg));

    inst_field_set_str(msg, "Sujet", subject ? subject : "");
    /* Corps = chaine (le protocole client NATS transporte des octets bruts ;
     * une chaine OWL peut contenir des \0 grace a la longueur explicite). */
    ObjString* body = obj_copy_string(payload ? (const char*)payload : "",
                                      (int)payload_len);
    inst_field_set_value(msg, "Corps", OBJ_VAL(body));
    if (reply && reply[0]) inst_field_set_str(msg, "RepondreA", reply);

    (void)vm_call_reentrant(handler, 2);

    vm.stackTop = repere;                           /* rejette tout residu de retour */
}

/* Capture INTERNE de la reponse d'une requete : au lieu de
 * dispatcher a une procedure OWL, on construit le natsMessage reponse et on le
 * range dans la session. Aucune reentrance E1 : NATSRequête recuperera le
 * message a la fin de sa pompe.
 *
 * Discipline GC : le natsMessage doit survivre a plusieurs ticks de reacteur
 * (donc a des GC) avant d'etre retourne. On l'ancre dans un champ de la session
 * (`_inbox_reponse`) : la session est rootee cote OWL, le champ le rend
 * reachable. Le pointeur C `inbox_reponse` n'est qu'un raccourci de lecture ; sa
 * survie tient au champ, pas au pointeur. gc-root-obj-fresh. */
static void nats_capture_inbox(OwlNatsConn* c, const char* subject,
                               const char* reply, const uint8_t* payload,
                               size_t payload_len) {
    if (c->session == NULL) return;

    ObjInstance* msg = build_natsMessage_template();
    vm_push(OBJ_VAL(msg));                           /* rooté pendant les sets */
    inst_field_set_str(msg, "Sujet", subject ? subject : "");
    ObjString* body = obj_copy_string(payload ? (const char*)payload : "",
                                      (int)payload_len);
    inst_field_set_value(msg, "Corps", OBJ_VAL(body));
    if (reply && reply[0]) inst_field_set_str(msg, "RepondreA", reply);
    inst_field_set_value(c->session, "_inbox_reponse", OBJ_VAL(msg));
    vm_pop();

    c->inbox_reponse = msg;
    c->inbox_recu = true;
}

/* ============================================================
 * Pompe : decoupe le tampon `rx` ligne par ligne, repond au keepalive et
 * livre les MSG entrants.
 * ============================================================ */

/* Extrait jusqu'a `max` tokens separes par des espaces depuis `line` (copie
 * modifiable). Renvoie le nombre de tokens. */
static int nats_split_tokens(char* line, char* tokens[], int max) {
    int n = 0;
    char* p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

/* Frame toutes les lignes completes accumulees dans `rx`. Renvoie quand il
 * manque des octets (ligne ou payload incomplet) pour compacter le reliquat. */
static void nats_rx_parse(OwlNatsConn* c) {
    size_t off = 0;
    for (;;) {
        if (off >= c->rx_len) break;
        const uint8_t* nl = (const uint8_t*)memchr(c->rx + off, '\n',
                                                   c->rx_len - off);
        if (nl == NULL) break;                       /* ligne de controle incomplete */
        size_t line_end = (size_t)(nl - c->rx);      /* index du \n */
        size_t line_len = line_end - off;
        if (line_len > 0 && c->rx[off + line_len - 1] == '\r') line_len--;

        char line[600];
        size_t cl = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
        memcpy(line, c->rx + off, cl);
        line[cl] = '\0';

        /* Verbe = 3 premiers caracteres significatifs. MSG porte un payload ;
         * PING exige un PONG ; le reste (PONG/+OK/-ERR/INFO/ligne vide) est avale. */
        if (strncmp(line, "MSG", 3) == 0 && (line[3] == ' ' || line[3] == '\t')) {
            char work[600];
            memcpy(work, line, cl + 1);
            char* tok[6];
            int nt = nats_split_tokens(work, tok, 6);
            /* MSG <sujet> <sid> [repondre-a] <#octets> : 4 ou 5 tokens. */
            const char* subject = NULL;
            const char* sid = NULL;
            const char* reply = NULL;
            long nbytes = -1;
            if (nt == 4) {
                subject = tok[1]; sid = tok[2]; reply = NULL;
                nbytes = strtol(tok[3], NULL, 10);
            } else if (nt == 5) {
                subject = tok[1]; sid = tok[2]; reply = tok[3];
                nbytes = strtol(tok[4], NULL, 10);
            }
            if (nbytes < 0) { off = line_end + 1; continue; }  /* ligne malformee : sauter */

            size_t payload_start = line_end + 1;               /* apres le \n de controle */
            /* payload + CRLF terminal. */
            if (c->rx_len < payload_start + (size_t)nbytes + 2) break;  /* incomplet : attendre */

            /* Un MSG portant le sid de l'inbox interne d'une requete en cours
             * est capture (case connexion) au lieu d'etre dispatche via E1. Les
             * autres sid (abonnements user, ex le repondeur) suivent la
             * livraison normale, y compris pendant la pompe d'une requete. */
            if (c->inbox_actif && sid != NULL && strcmp(sid, c->inbox_sid) == 0) {
                nats_capture_inbox(c, subject, reply, c->rx + payload_start,
                                   (size_t)nbytes);
            } else {
                nats_livre(c, sid, subject, reply, c->rx + payload_start,
                           (size_t)nbytes);
            }
            off = payload_start + (size_t)nbytes + 2;          /* saute payload + \r\n */
            continue;
        }

        if (strcmp(line, "PING") == 0) {
            nats_send(c, "PONG\r\n", 6);              /* keepalive : sinon deconnexion serveur */
        }
        /* PONG / +OK / -ERR / INFO / ligne vide : avales (rien a livrer). */
        off = line_end + 1;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* Callback de lecture du reacteur : draine la socket, accumule, decoupe. */
static void nats_reacteur_lecture_cb(OwlReacteur* r, owl_socket_fd_t fd,
                                     short evts_prets, void* user) {
    OwlNatsConn* c = (OwlNatsConn*)user;
    if (evts_prets & OWL_REAC_LECTURE) {
        uint8_t tampon[4096];
        int n = nats_recv(c, tampon, sizeof(tampon));
        if (n > 0) {
            nats_rx_append(c, tampon, (size_t)n);
            nats_rx_parse(c);
            return;
        }
        owl_reacteur_retire_fd(r, fd);              /* EOF (n==0) ou erreur (n<0) */
        c->connected = false;                       /* serveur a raccroche */
        c->fd_branche = false;
        return;
    }
    if (evts_prets & OWL_REAC_ERREUR) {
        owl_reacteur_retire_fd(r, fd);
        c->connected = false;
        c->fd_branche = false;
    }
}

static bool nats_reacteur_assure(OwlNatsConn* c) {
    if (c->reacteur == NULL) {
        c->reacteur = owl_reacteur_cree();
        if (c->reacteur == NULL) return false;
    }
    if (!c->fd_branche) {
        if (!owl_reacteur_ajoute_fd(c->reacteur, c->transport.tcp.fd,
                                    OWL_REAC_LECTURE, nats_reacteur_lecture_cb, c))
            return false;
        c->fd_branche = true;
    }
    return true;
}

/* Lit une ligne de controle complete en synchrone (poignee de main, avant que
 * le reacteur ne prenne le relais). La ligne (sans CRLF) est copiee dans `out`.
 * Les octets excedentaires restent dans `rx` pour la pompe. false + `detail`
 * sur coupure de transport. */
static bool nats_read_line_sync(OwlNatsConn* c, char* out, size_t outsz,
                                char* detail, size_t dsz) {
    for (;;) {
        const uint8_t* nl = (const uint8_t*)memchr(c->rx, '\n', c->rx_len);
        if (nl != NULL) {
            size_t line_end = (size_t)(nl - c->rx);
            size_t line_len = line_end;
            if (line_len > 0 && c->rx[line_len - 1] == '\r') line_len--;
            size_t cl = line_len < outsz - 1 ? line_len : outsz - 1;
            memcpy(out, c->rx, cl);
            out[cl] = '\0';
            size_t used = line_end + 1;
            memmove(c->rx, c->rx + used, c->rx_len - used);
            c->rx_len -= used;
            return true;
        }
        uint8_t tampon[2048];
        int n = nats_recv(c, tampon, sizeof(tampon));
        if (n <= 0) {
            snprintf(detail, dsz, "connexion coupée en attente de réponse du serveur");
            return false;
        }
        nats_rx_append(c, tampon, (size_t)n);
    }
}

/* ============================================================
 * Natives.
 * ============================================================ */

static Value fn_nats_connecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSConnecte");
    if (inst == NULL) return BOOL_VAL(false);

    /* Reconnexion : libere une connexion vivante residuelle (idempotence). */
    OwlNatsConn* old = session_handle(inst);
    if (old != NULL) { nats_conn_free(old); session_set_handle(inst, NULL); }

    const char* adresse = nats_str(inst, "Adresse");
    const char* user    = nats_str(inst, "Utilisateur");
    const char* pass    = nats_str(inst, "MotDePasse");
    int timeout = nats_int(inst, "TimeoutConnexion");
    int port = nats_int(inst, "Port");
    if (port <= 0) port = NATS_PORT_DEFAULT;
    /* optionSSL demande le chiffrement in-band (STARTTLS apres l'INFO). */
    bool ssl = (nats_int(inst, "Option") & OWL_OPTION_SSL) != 0;

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", adresse, port);

    OwlNatsConn* c = (OwlNatsConn*)calloc(1, sizeof(OwlNatsConn));
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                              "native", "NATSConnecte", "cible", cible,
                              "detail", "mémoire insuffisante", NULL);
        return BOOL_VAL(false);
    }

    owl_tcp_global_init();
    owl_tls_init(&c->transport);
    int tmo = timeout > 0 ? timeout * 1000 : NATS_CONNECT_TIMEOUT_MS;

    /* NATS negocie le TLS IN-BAND : on ouvre TOUJOURS en TCP clair (meme port),
     * on lit l'INFO en clair, puis on bascule la socket existante en TLS
     * (STARTTLS) si demande, AVANT le CONNECT. */
    if (!owl_tcp_connect(&c->transport.tcp, adresse, port, tmo)) {
        owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                              "native", "NATSConnecte", "cible", cible,
                              "detail", owl_tcp_last_error(&c->transport.tcp), NULL);
        nats_conn_free(c);
        return BOOL_VAL(false);
    }

    /* 1) INFO non sollicite : le serveur l'envoie en premier a la connexion, EN
     * CLAIR (avant tout TLS). Contient les capacites du serveur, dont
     * tls_required/tls_available. */
    char detail[300];
    char ligne[600];
    if (!nats_read_line_sync(c, ligne, sizeof(ligne), detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                              "native", "NATSConnecte", "cible", cible,
                              "detail", detail, NULL);
        nats_conn_free(c);
        return BOOL_VAL(false);
    }
    if (strncmp(ligne, "INFO", 4) != 0) {
        snprintf(detail, sizeof(detail),
                 "réponse inattendue du serveur (%.32s)", ligne);
        owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                              "native", "NATSConnecte", "cible", cible,
                              "detail", detail, NULL);
        nats_conn_free(c);
        return BOOL_VAL(false);
    }
    /* Le serveur exige-t-il TLS ? Recherche brute du champ dans le JSON de
     * l'INFO (pas de parseur complet requis ici). */
    bool server_tls_required = (strstr(ligne, "\"tls_required\":true") != NULL);

    /* 2) Bascule STARTTLS si optionSSL : la socket TCP deja connectee est
     * promue en TLS. VERIFY_FULL (hostname + chaine, ) conserve dans
     * tous les cas. CertificatCA (si renseigne) devient la racine de confiance,
     * sinon NULL -> bundle Mozilla. Le PEM du champ vit pendant le handshake. */
    if (ssl) {
        const char* ca = nats_str(inst, "CertificatCA");
        const char* extra_ca = (ca && ca[0] != '\0') ? ca : NULL;
        if (!owl_tls_start_tls(&c->transport, adresse, OWL_TLS_VERIFY_FULL,
                               extra_ca)) {
            owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                                  "native", "NATSConnecte", "cible", cible,
                                  "detail", owl_tls_last_error(&c->transport), NULL);
            nats_conn_free(c);
            return BOOL_VAL(false);
        }
        c->tls_active = true;
    } else if (server_tls_required) {
        /* Le serveur impose TLS mais optionSSL n'a pas ete demande : echec
         * propre plutot qu'un CONNECT en clair rejete de facon obscure. */
        owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                              "native", "NATSConnecte", "cible", cible,
                              "detail", "le serveur exige TLS (poser Option = optionSSL)", NULL);
        nats_conn_free(c);
        return BOOL_VAL(false);
    }

    /* 3) CONNECT {options} : verbose=false (pas de +OK par commande),
     * pedantic=false. tls_required reflete la bascule reelle. user/pass si
     * fournis. Desormais emis par-dessus TLS si tls_active. */
    {
        NatsBuf f = {0};
        nbuf_str(&f, "CONNECT {\"verbose\":false,\"pedantic\":false,\"tls_required\":");
        nbuf_str(&f, c->tls_active ? "true" : "false");
        nbuf_str(&f, ",\"name\":");
        nbuf_json_string(&f, "owl");
        nbuf_str(&f, ",\"lang\":");
        nbuf_json_string(&f, "owl");
        nbuf_str(&f, ",\"version\":");
        nbuf_json_string(&f, "1.0");
        if (user && user[0]) {
            nbuf_str(&f, ",\"user\":");
            nbuf_json_string(&f, user);
            nbuf_str(&f, ",\"pass\":");
            nbuf_json_string(&f, pass ? pass : "");
        }
        nbuf_str(&f, "}\r\n");
        nbuf_str(&f, "PING\r\n");
        bool ok = (f.data != NULL) && (nats_send(c, f.data, f.len) == (int)f.len);
        free(f.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                                  "native", "NATSConnecte", "cible", cible,
                                  "detail", "émission du CONNECT impossible", NULL);
            nats_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* 3) Attend le PONG (confirme la poignee de main). -ERR -> echec. Les
     * autres lignes (INFO de mise a jour, +OK) sont avalees. */
    for (;;) {
        if (!nats_read_line_sync(c, ligne, sizeof(ligne), detail, sizeof(detail))) {
            owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                                  "native", "NATSConnecte", "cible", cible,
                                  "detail", detail, NULL);
            nats_conn_free(c);
            return BOOL_VAL(false);
        }
        if (strcmp(ligne, "PONG") == 0) break;
        if (strncmp(ligne, "-ERR", 4) == 0) {
            snprintf(detail, sizeof(detail), "%.200s", ligne);
            owl_emit_native_error(E_NATIVE_NATS_CONNECT_FAILURE, "NATSConnecte",
                                  "native", "NATSConnecte", "cible", cible,
                                  "detail", detail, NULL);
            nats_conn_free(c);
            return BOOL_VAL(false);
        }
        /* INFO/+OK/ligne vide : ignore, on relit. */
    }

    c->connected = true;
    c->session = inst;              /* arg 1 des livraisons ; instance rootee OWL */
    session_set_handle(inst, c);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_nats_deconnecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSDéconnecte");
    if (inst == NULL) return NIL_VAL;
    OwlNatsConn* c = session_handle(inst);
    if (c != NULL) {
        nats_conn_free(c);
        session_set_handle(inst, NULL);
    }
    return NIL_VAL;
}

/* NATSPublie(<session>, <sujet>, <message>) -> booleen. Validation des types
 * d'arguments AVANT le controle de connexion (parite AMQP/MQTT/STOMP). */
static Value fn_nats_publie(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSPublie");
    if (inst == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSPublie",
                              "native", "NATSPublie", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* sujet = AS_CSTRING(args[1]);

    if (!IS_INSTANCE(args[2]) ||
        AS_INSTANCE(args[2])->structName == NULL ||
        strcmp(AS_INSTANCE(args[2])->structName->chars, "natsMessage") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSPublie",
                              "native", "NATSPublie", "arg", "3",
                              "expected", "natsMessage", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* msg = AS_INSTANCE(args[2]);

    OwlNatsConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_NATS_PUBLISH_FAILURE, "NATSPublie",
                              "native", "NATSPublie",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    const char* reply = nats_str(msg, "RepondreA");
    const unsigned char* body;
    size_t body_len;
    nats_msg_body(msg, &body, &body_len);

    /* PUB <sujet> [repondre-a] <#octets>\r\n<corps>\r\n */
    NatsBuf f = {0};
    nbuf_str(&f, "PUB ");
    nbuf_str(&f, sujet);
    nbuf_bytes(&f, " ", 1);
    if (reply && reply[0]) { nbuf_str(&f, reply); nbuf_bytes(&f, " ", 1); }
    char nb[32];
    snprintf(nb, sizeof(nb), "%lu", (unsigned long)body_len);
    nbuf_str(&f, nb);
    nbuf_str(&f, "\r\n");
    nbuf_bytes(&f, body, body_len);
    nbuf_str(&f, "\r\n");
    bool ok = (f.data != NULL) && (nats_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_NATS_PUBLISH_FAILURE, "NATSPublie",
                              "native", "NATSPublie",
                              "detail", "émission du PUB impossible", NULL);
        return BOOL_VAL(false);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

/* NATSAbonne(<session>, <sujet>, <procedure> [, <groupeFile>]) -> booleen.
 * Validation des types d'arguments AVANT le controle de connexion. */
static Value fn_nats_abonne(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSAbonne");
    if (inst == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSAbonne",
                              "native", "NATSAbonne", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* sujet = AS_CSTRING(args[1]);

    /* groupeFile (4e arg optionnel = queue group). Chaine vide si absent. */
    const char* groupe = (argCount >= 4 && IS_STRING(args[3])) ? AS_CSTRING(args[3]) : "";

    /* Enregistre le handler via E1 : valide qu'il est appelable et le tient
     * comme racine GC. Refus -> ARG_TYPE_MISMATCH, avant tout controle de
     * connexion. */
    OwlMqHandle h = owl_mq_abonne(args[2]);
    if (h == OWL_MQ_HANDLE_NUL) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSAbonne",
                              "native", "NATSAbonne", "arg", "3",
                              "expected", "procédure", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }

    OwlNatsConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_NATS_SUBSCRIBE_FAILURE, "NATSAbonne",
                              "native", "NATSAbonne",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    /* sid genere (unique par session). */
    char sid[32];
    snprintf(sid, sizeof(sid), "%d", ++c->next_sid);

    /* SUB <sujet> [groupeFile] <sid>\r\n */
    NatsBuf f = {0};
    nbuf_str(&f, "SUB ");
    nbuf_str(&f, sujet);
    nbuf_bytes(&f, " ", 1);
    if (groupe && groupe[0]) { nbuf_str(&f, groupe); nbuf_bytes(&f, " ", 1); }
    nbuf_str(&f, sid);
    nbuf_str(&f, "\r\n");
    bool sent = (f.data != NULL) && (nats_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!sent) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_NATS_SUBSCRIBE_FAILURE, "NATSAbonne",
                              "native", "NATSAbonne",
                              "detail", "émission du SUB impossible", NULL);
        return BOOL_VAL(false);
    }

    if (!nats_sub_add(c, sid, sujet, h)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_NATS_SUBSCRIBE_FAILURE, "NATSAbonne",
                              "native", "NATSAbonne",
                              "detail", "nombre maximal d'abonnements atteint sur la session", NULL);
        return BOOL_VAL(false);
    }

    /* Reacteur possede par la session : cree + fd branche. NATSAttend le
     * tickera pour dispatcher les MSG entrants. */
    nats_reacteur_assure(c);

    owl_clear_error();
    return BOOL_VAL(true);
}

/* NATSAttend(<session>, <delai>) -> booleen. Pompe le reacteur possede par la
 * session pendant `delai` ms au plus : dispatche les MSG entrants aux procedures
 * abonnees et repond au keepalive PING, puis rend la main. Renvoie Vrai tant que
 * la session reste connectee. */
static Value fn_nats_attend(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSAttend");
    if (inst == NULL) return BOOL_VAL(false);

    OwlNatsConn* c = session_handle(inst);
    if (c == NULL || !c->connected) return BOOL_VAL(false);

    int64_t delai = IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : 0;
    if (delai < 0) delai = 0;

    if (!nats_reacteur_assure(c)) return BOOL_VAL(c->connected);

    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) break;                    /* serveur a raccroche */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)delai) break;        /* fenetre epuisee */
        int reste = (int)((uint64_t)delai - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) break;  /* poll casse */
    }
    return BOOL_VAL(c->connected);
}

/* NATSRequête(<session>, <sujet>, <message>, <délai>) -> natsMessage. Publie
 * `message` vers `sujet` avec un sujet d'inbox de reponse unique, puis pompe le
 * reacteur jusqu'a recevoir la reponse sur cet inbox OU expiration de `délai`
 * (ms). La reponse est capturee EN INTERNE (case connexion), pas via un callback
 * OWL : les MSG des autres abonnements continuent d'etre livres pendant la
 * pompe, ce qui permet a un repondeur sur la meme session de recevoir la requete
 * et de publier sa reponse. Succes -> le natsMessage reponse (ErreurDétectée
 * Faux). Timeout/echec -> natsMessage vide + erreur. Se desabonne de l'inbox
 * dans tous les cas. Validation des types d'arguments AVANT tout reseau. */
static Value fn_nats_requete(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_nats_session(args[0], "NATSRequête");
    if (inst == NULL) return OBJ_VAL(build_natsMessage_template());

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSRequête",
                              "native", "NATSRequête", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return OBJ_VAL(build_natsMessage_template());
    }
    const char* sujet = AS_CSTRING(args[1]);

    if (!IS_INSTANCE(args[2]) ||
        AS_INSTANCE(args[2])->structName == NULL ||
        strcmp(AS_INSTANCE(args[2])->structName->chars, "natsMessage") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSRequête",
                              "native", "NATSRequête", "arg", "3",
                              "expected", "natsMessage", "actual", "un autre type", NULL);
        return OBJ_VAL(build_natsMessage_template());
    }
    ObjInstance* reqMsg = AS_INSTANCE(args[2]);

    if (!IS_INTEGER(args[3])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "NATSRequête",
                              "native", "NATSRequête", "arg", "4",
                              "expected", "entier", "actual", "un autre type", NULL);
        return OBJ_VAL(build_natsMessage_template());
    }
    int64_t delai = AS_INTEGER(args[3]);
    if (delai < 0) delai = 0;

    OwlNatsConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_NATS_PUBLISH_FAILURE, "NATSRequête",
                              "native", "NATSRequête",
                              "detail", "session non connectée", NULL);
        return OBJ_VAL(build_natsMessage_template());
    }

    if (!nats_reacteur_assure(c)) {
        owl_emit_native_error(E_NATIVE_NATS_SUBSCRIBE_FAILURE, "NATSRequête",
                              "native", "NATSRequête",
                              "detail", "réacteur d'écoute indisponible", NULL);
        return OBJ_VAL(build_natsMessage_template());
    }

    /* Inbox unique + sid dedie. Le sid (compteur session) suffit a l'unicite
     * cote pompe ; l'horloge et un alea evitent la collision de sujet inbox
     * entre requetes successives / instances. */
    char sid[32];
    snprintf(sid, sizeof(sid), "%d", ++c->next_sid);
    char inbox[96];
    snprintf(inbox, sizeof(inbox), "_INBOX.%s.%llu.%d", sid,
             (unsigned long long)owl_tcp_server_horloge_ms(), rand());

    /* Arme la case de capture AVANT le SUB : tout MSG de ce sid sera capture. */
    c->inbox_actif = true;
    c->inbox_recu = false;
    c->inbox_reponse = NULL;
    snprintf(c->inbox_sid, sizeof(c->inbox_sid), "%s", sid);

    /* SUB inbox AVANT le PUB : garantit l'enregistrement du recepteur avant que
     * le repondeur ne puisse publier sa reponse. */
    {
        NatsBuf f = {0};
        nbuf_str(&f, "SUB ");
        nbuf_str(&f, inbox);
        nbuf_bytes(&f, " ", 1);
        nbuf_str(&f, sid);
        nbuf_str(&f, "\r\n");
        bool ok = (f.data != NULL) && (nats_send(c, f.data, f.len) == (int)f.len);
        free(f.data);
        if (!ok) {
            c->inbox_actif = false;
            owl_emit_native_error(E_NATIVE_NATS_SUBSCRIBE_FAILURE, "NATSRequête",
                                  "native", "NATSRequête",
                                  "detail", "émission du SUB d'inbox impossible", NULL);
            return OBJ_VAL(build_natsMessage_template());
        }
    }

    /* PUB sujet avec RépondreÀ = inbox. */
    {
        const unsigned char* body;
        size_t body_len;
        nats_msg_body(reqMsg, &body, &body_len);
        NatsBuf f = {0};
        nbuf_str(&f, "PUB ");
        nbuf_str(&f, sujet);
        nbuf_bytes(&f, " ", 1);
        nbuf_str(&f, inbox);
        nbuf_bytes(&f, " ", 1);
        char nb[32];
        snprintf(nb, sizeof(nb), "%lu", (unsigned long)body_len);
        nbuf_str(&f, nb);
        nbuf_str(&f, "\r\n");
        nbuf_bytes(&f, body, body_len);
        nbuf_str(&f, "\r\n");
        bool ok = (f.data != NULL) && (nats_send(c, f.data, f.len) == (int)f.len);
        free(f.data);
        if (!ok) {
            /* Desabonne l'inbox avant de rendre : la case reste propre. */
            char unsub[48];
            int un = snprintf(unsub, sizeof(unsub), "UNSUB %s\r\n", sid);
            nats_send(c, unsub, (size_t)un);
            c->inbox_actif = false;
            owl_emit_native_error(E_NATIVE_NATS_PUBLISH_FAILURE, "NATSRequête",
                                  "native", "NATSRequête",
                                  "detail", "émission du PUB impossible", NULL);
            return OBJ_VAL(build_natsMessage_template());
        }
    }

    /* Pompe bornee : il faut >=2 evenements reseau (recevoir la requete cote
     * repondeur -> il publie -> recevoir la reponse), d'ou une vraie boucle de
     * ticks jusqu'a inbox_recu OU expiration. */
    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) break;                    /* serveur a raccroche */
        if (c->inbox_recu) break;                    /* reponse capturee */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)delai) break;        /* fenetre epuisee */
        int reste = (int)((uint64_t)delai - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) break;  /* poll casse */
    }

    /* Desabonnement de l'inbox dans TOUS les cas. */
    {
        char unsub[48];
        int un = snprintf(unsub, sizeof(unsub), "UNSUB %s\r\n", sid);
        nats_send(c, unsub, (size_t)un);
    }
    c->inbox_actif = false;

    if (c->inbox_recu && c->inbox_reponse != NULL) {
        Value rep = OBJ_VAL(c->inbox_reponse);
        vm_push(rep);                                /* roote avant de liberer le champ ancre */
        inst_field_set_int(inst, "_inbox_reponse", 0);   /* relache l'ancrage session */
        c->inbox_reponse = NULL;
        c->inbox_recu = false;
        owl_clear_error();
        return vm_pop();                             /* aucune alloc avant retour natif */
    }

    /* Timeout / pas de reponse : nettoie l'ancrage eventuel et signale. */
    if (c->inbox_reponse != NULL) {
        inst_field_set_int(inst, "_inbox_reponse", 0);
        c->inbox_reponse = NULL;
    }
    c->inbox_recu = false;
    owl_emit_native_error(E_NATIVE_NATS_REQUEST_TIMEOUT, "NATSRequête",
                          "native", "NATSRequête",
                          "detail", "délai dépassé sans réponse", NULL);
    return OBJ_VAL(build_natsMessage_template());
}

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_session[] = {
    {"session", "natsConnexion", "session NATS décrivant les paramètres de connexion"}, {0}
};
static const OwlSignature sig_connecte[] = {
    {1, p_session, "booléen", "vrai si la connexion au serveur a réussi, faux sinon"}, {0}
};
static const OwlSignature sig_deconnecte[] = {
    {1, p_session, "rien", NULL}, {0}
};

static const OwlParam p_publie[] = {
    {"session", "natsConnexion", "session NATS connectée au serveur"},
    {"sujet",   "chaîne",        "sujet sur lequel publier le message"},
    {"message", "natsMessage",   "message à publier"},
    {0}
};
static const OwlSignature sig_publie[] = {
    {3, p_publie, "booléen", "vrai si le message a été publié, faux sinon"}, {0}
};

static const OwlParam p_abonne[] = {
    {"session",    "natsConnexion", "session NATS connectée au serveur"},
    {"sujet",      "chaîne",        "sujet auquel s'abonner"},
    {"procédure",  "procédure",     "procédure appelée à chaque message reçu (session, message)"},
    {"groupeFile", "chaîne",        "groupe de file (queue group) : les abonnés d'un même groupe se partagent les messages (optionnel)"},
    {0}
};
#define NATS_ABONNE_RET "vrai si l'abonnement a été mis en place, faux sinon"
static const OwlSignature sig_abonne[] = {
    {3, p_abonne, "booléen", NATS_ABONNE_RET},
    {4, p_abonne, "booléen", NATS_ABONNE_RET},
    {0}
};

static const OwlParam p_attend[] = {
    {"session", "natsConnexion", "session NATS connectée au serveur"},
    {"délai",   "entier",        "durée maximale d'attente, en millisecondes"},
    {0}
};
static const OwlSignature sig_attend[] = {
    {2, p_attend, "booléen", "vrai si la session est toujours connectée, faux si le serveur a fermé la connexion pendant l'attente"}, {0}
};

static const OwlParam p_requete[] = {
    {"session", "natsConnexion", "session NATS connectée au serveur"},
    {"sujet",   "chaîne",        "sujet du service auquel envoyer la requête"},
    {"message", "natsMessage",   "message de requête à envoyer"},
    {"délai",   "entier",        "durée maximale d'attente de la réponse, en millisecondes"},
    {0}
};
static const OwlSignature sig_requete[] = {
    {4, p_requete, "natsMessage", "le message de réponse reçu ; en cas d'expiration du délai, un message vide (ErreurDétectée est alors vrai)"}, {0}
};

/* ============================================================
 * Method bindings — formes prefixe session.Xxx().
 * ============================================================ */

static const OwlMethodBinding MB_CONNECTE[] = {
    { "natsConnexion", "Connecte", 1 }, { "natsConnexion", "Connect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECONNECTE[] = {
    { "natsConnexion", "Deconnecte", 1 }, { "natsConnexion", "Disconnect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PUBLIE[] = {
    { "natsConnexion", "Publie", 3 }, { "natsConnexion", "Publish", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ABONNE[] = {
    { "natsConnexion", "Abonne", -1 }, { "natsConnexion", "Subscribe", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ATTEND[] = {
    { "natsConnexion", "Attend", 2 }, { "natsConnexion", "Wait", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REQUETE[] = {
    { "natsConnexion", "Requete", 4 }, { "natsConnexion", "Request", 4 }, { NULL, NULL, 0 }
};

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define M_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "NATS", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_NATS[] = {
    M_DEF("NATSConnecte", NULL, "NATSConnect",
        "Ouvre une connexion vers un serveur NATS à partir des paramètres d'une session, et indique si la connexion a réussi.",
        "Opens a connection to a NATS server from a session's parameters, and reports whether the connection succeeded.",
        sig_connecte, fn_nats_connecte, MB_CONNECTE),
    M_DEF("NATSDéconnecte", "NATSDeconnecte", "NATSDisconnect",
        "Ferme une connexion NATS ouverte.",
        "Closes an open NATS connection.",
        sig_deconnecte, fn_nats_deconnecte, MB_DECONNECTE),
    M_DEF("NATSPublie", NULL, "NATSPublish",
        "Publie un message sur un sujet du serveur NATS et indique si la publication a réussi.",
        "Publishes a message to a subject on the NATS server and reports whether it succeeded.",
        sig_publie, fn_nats_publie, MB_PUBLIE),
    M_DEF("NATSAbonne", NULL, "NATSSubscribe",
        "Abonne une procédure aux messages publiés sur un sujet du serveur NATS ; la procédure est appelée à chaque message reçu. Un groupe de file optionnel partage les messages entre les abonnés du même groupe.",
        "Subscribes a procedure to messages published on a NATS subject; the procedure is called for each incoming message. An optional queue group shares messages between subscribers of the same group.",
        sig_abonne, fn_nats_abonne, MB_ABONNE),
    M_DEF("NATSAttend", NULL, "NATSWait",
        "Attend et traite les messages entrants d'une session NATS pendant un délai maximal exprimé en millisecondes, en appelant les procédures abonnées à mesure que les messages arrivent ; renvoie vrai tant que la session reste connectée.",
        "Waits for and processes the incoming messages of a NATS session for a maximum delay in milliseconds, calling the subscribed procedures as messages arrive; returns true as long as the session stays connected.",
        sig_attend, fn_nats_attend, MB_ATTEND),
    M_DEF("NATSRequête", "NATSRequete", "NATSRequest",
        "Envoie une requête sur un sujet du serveur NATS et attend la réponse pendant un délai maximal exprimé en millisecondes : génère un sujet de réponse dédié, publie la requête, puis renvoie le message de réponse reçu. En cas d'expiration du délai, renvoie un message vide et signale une erreur.",
        "Sends a request to a NATS subject and waits for the reply for a maximum delay in milliseconds: generates a dedicated reply subject, publishes the request, then returns the received reply message. On delay expiry, returns an empty message and reports an error.",
        sig_requete, fn_nats_requete, MB_REQUETE),
};

#undef M_DEF

/* ============================================================
 * Types reifies natsConnexion + natsMessage.
 * ============================================================ */

static const OwlField FIELDS_natsConnexion[] = {
    {"Adresse",          NULL, "chaîne", "\"\"",   "adresse du serveur NATS (IP ou nom d'hôte)", "NATS server address (IP or host name)", .nameEN="Address"},
    {"Port",             NULL, "entier", "4222",   "port du serveur (4222 par défaut)", "server port (4222 by default)", .nameEN="Port"},
    {"Utilisateur",      NULL, "chaîne", "\"\"",   "nom d'utilisateur pour l'authentification", "user name for authentication", .nameEN="User"},
    {"MotDePasse",       NULL, "chaîne", "\"\"",   "mot de passe (ou jeton) pour l'authentification", "password (or token) for authentication", .nameEN="Password"},
    {"Option",           NULL, "entier", "0",      "options de connexion (optionSSL pour une connexion chiffrée)", "connection options (optionSSL for an encrypted connection)", .nameEN="Option"},
    {"CertificatCA",     NULL, "chaîne", "\"\"",   "chemin d'un fichier PEM de certificat d'autorité (CA) à utiliser pour valider le serveur en TLS ; vide = autorités publiques par défaut", "path to a PEM certificate-authority (CA) file used to validate the server over TLS; empty = default public authorities", .nameEN="CACertificate"},
    {"TimeoutConnexion", NULL, "entier", "0",      "délai de connexion en secondes (0 = défaut)", "connection timeout in seconds (0 = default)", .nameEN="ConnectionTimeout"},
    {0}
};

static const OwlField FIELDS_natsMessage[] = {
    {"Sujet",     NULL,        "chaîne", "\"\"",  "sujet du message", "subject of the message", .nameEN="Subject"},
    {"Corps",     NULL,        "chaîne", "\"\"",  "corps du message : chaîne ou Buffer en émission, chaîne en réception", "message body: string or Buffer when sending, string when receiving", .nameEN="Body"},
    {"RepondreA", "RépondreÀ", "chaîne", "\"\"",  "sujet de réponse pour le request-reply (facultatif)", "reply subject for request-reply (optional)", .nameEN="ReplyTo"},
    {0}
};

static const OwlTypeDef TYPES_NATS[] = {
    { .canonical      = "natsConnexion",
      .canonicalAscii = "natsConnexion",
      .aliases        = NULL,
      .category       = "NATS",
      .descFR         = "Paramètres de connexion à un serveur NATS (adresse, port, identifiants, options). Passé à NATSConnecte.",
      .descEN         = "Connection parameters for a NATS server (address, port, credentials, options). Passed to NATSConnect.",
      .fields         = FIELDS_natsConnexion,
      .nameEN         = "natsConnection",
      .genre          = OWL_GENRE_F },   /* la connexion */
    { .canonical      = "natsMessage",
      .canonicalAscii = "natsMessage",
      .aliases        = NULL,
      .category       = "NATS",
      .descFR         = "Message NATS échangé sur un sujet (corps, sujet, adresse de réponse). Publié par NATSPublie, reçu par NATSAbonne.",
      .descEN         = "NATS message exchanged on a subject (body, subject, reply address). Published by NATSPublish, received by NATSSubscribe.",
      .fields         = FIELDS_natsMessage,
      .nameEN         = "natsMessage",
      .genre          = OWL_GENRE_M },   /* le message */
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_nats_init(void) {
    /* Le socle Message Queue E1 est arme par stdlib_mq_init (point central
     * neutre, appele par owl_stdlib_init AVANT tout client MQ). Ce client le
     * partage avec MQTT/AMQP/STOMP. La constante optionSSL est deja enregistree
     * globalement (client MQTT) : on la reutilise (transport chiffre effectif via
     * owl_tls_start_tls). */
    owl_registry_register_module(DEFS_NATS,
        (int)(sizeof(DEFS_NATS) / sizeof(DEFS_NATS[0])));
    owl_registry_register_types(TYPES_NATS,
        (int)(sizeof(TYPES_NATS) / sizeof(TYPES_NATS[0])));

    ObjInstance* tpl = build_natsConnexion_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("natsConnexion", OBJ_VAL(tpl));
    vm_pop();

    ObjInstance* tplMsg = build_natsMessage_template();
    vm_push(OBJ_VAL(tplMsg));
    vm_type_register("natsMessage", OBJ_VAL(tplMsg));
    vm_pop();
}
