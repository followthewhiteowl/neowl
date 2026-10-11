// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Client STOMP 1.2, Lot 1 — le coeur. Voir stdlib_stomp.h pour le
 * contrat.
 *
 * ETAT : client complet round-trip. STOMPConnecte ouvre le transport (TCP
 * clair, ou TLS via optionSSL sur le port 61614 avec verification stricte
 * hostname+chaine ; CertificatCA fournit une CA privee optionnelle), emet un
 * CONNECT (accept-version:1.2, host,
 * login/passcode, heart-beat) et attend un CONNECTED. STOMPPublie emet un SEND
 * (destination + content-type + content-length + corps + 0x00 terminal).
 * STOMPAbonne emet un SUBSCRIBE (id genere, ack:auto), memorise le mapping
 * id/destination -> handle E1 et branche le reacteur ; un MESSAGE entrant
 * (frame la pompe) construit un stompMessage et invoque la procedure OWL
 * abonnee via le socle E1 (2 args : session, message). STOMPDéconnecte emet un
 * DISCONNECT puis ferme. La connexion vivante est stockee dans l'instance
 * stompSession (champ interne _handle -> OwlStompConn*).
 *
 * La reception est pompee depuis OWL par STOMPAttend : la session
 * POSSEDE son reacteur (net_reacteur), cree paresseusement au 1er abonnement /
 * a la 1re attente, et STOMPAttend le ticke pendant un delai borne.
 *
 * Le heartbeat negocie est place dans le CONNECT (heart-beat:H,H, defaut 0,0 =
 * desactive) ; aucun timer de battement actif au Lot 1 (les octets \n de
 * heartbeat entrants sont avales cote lecture).
 *
 * Le wire STOMP (frames texte) n'a pas d'equivalent canon : reference = spec
 * STOMP 1.2. Frame : COMMANDE\n puis lignes header:valeur\n, ligne vide \n,
 * corps, octet \0 terminal. Echappement des valeurs de header : \c->:, \n->LF,
 * \r->CR, \\->\. content-length est emis pour le corps (permet le binaire, le
 * corps n'est pas coupe sur un \0 interne). Heartbeat = octets \n seuls entre
 * frames.
 */

#include "stdlib_stomp.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
/* net_tcp.h tire winsock2.h sous Windows -> collision TokenType (winnt.h vs
 * scanner.h). Meme garde que stdlib_mqtt.c / stdlib_amqp.c. */
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

/* Ports par defaut STOMP : 61613 en clair, 61614 en TLS (canon plugin RabbitMQ
 * STOMP). */
#define STOMP_PORT_CLEAR 61613
#define STOMP_PORT_SSL   61614
/* Valeur de la constante optionSSL (masque OU-binaire sur ..Option). La
 * constante elle-meme est deja enregistree globalement (par le client MQTT) :
 * on la reutilise, on ne la re-enregistre pas (invariant registry). */
#define OWL_OPTION_SSL   1
/* Delai d'ouverture du transport en millisecondes (defaut si TimeoutConnexion
 * n'est pas renseigne cote instance). */
#define STOMP_CONNECT_TIMEOUT_MS 10000

/* Nombre maximal d'abonnements simultanes sur une session. Chaque abonnement
 * mappe id + destination -> handle E1 (borne par OWL_MQ_MAX_ABONNEMENTS, cap 8,
 * racine GC partagee). Depassement -> abonnement refuse (pas de mode degrade). */
#define STOMP_MAX_SUBS 8

/* Horloge monotone en millisecondes, partagee avec le socle net (definie dans
 * net_tcp_server.c). Borne la pompe STOMPAttend. Extern-declaree comme dans
 * stdlib_mqtt.c / stdlib_amqp.c pour ne pas tirer winsock via net_tcp_server.h. */
extern uint64_t owl_tcp_server_horloge_ms(void);

/* ============================================================
 * Connexion vivante — pointeur opaque cache dans l'instance stompSession
 * (champ _handle, cast intptr_t). Un seul transport, dispatch send/recv par
 * tls_active (meme pattern que amqpConnection / mqttSession).
 * ============================================================ */

/* Mapping abonnement -> handle E1, par session. La livraison d'un MESSAGE
 * entrant retrouve le handler par l'id d'abonnement (header `subscription`) ou,
 * a defaut, par la destination. Les copies id/dest sont C-heap (independantes
 * des Value OWL). */
typedef struct {
    char*       id;        /* copie malloc de l'id d'abonnement (genere) */
    char*       dest;      /* copie malloc de la destination abonnee */
    OwlMqHandle handle;    /* handle E1 tenant le handler comme racine GC */
    bool        auto_ack;  /* Vrai = ack:auto (le courtier n'attend pas d'ack) ;
                            * Faux = ack:client-individual -> le jeton d'ack de
                            * chaque MESSAGE est extrait et pose sur le message. */
} StompSub;

typedef struct {
    OwlTlsSocket transport;      /* .tcp = TCP sous-jacent (clair ou chiffre) */
    bool         tls_active;     /* handshake TLS effectue -> router via owl_tls_* */
    bool         connected;      /* CONNECTED recu */
    int          heartbeat;      /* H negocie (millisecondes), 0 = desactive */
    int          next_sub_id;    /* compteur d'ids d'abonnement */

    ObjInstance* session;        /* instance stompSession proprietaire ; passee en
                                  * arg 1 de la livraison. Rootee par l'appelant
                                  * (portee OWL), non scannee via ce champ C. */
    StompSub     subs[STOMP_MAX_SUBS];
    int          nb_subs;

    /* --- Pompe possedee par la session (, calque MQTT/AMQP) --- */
    OwlReacteur* reacteur;       /* reacteur PROPRE, cree paresseusement (1er
                                  * abonnement / 1re attente). STOMPAttend le ticke. */
    bool         fd_branche;     /* socket vivante deja enregistree en LECTURE */

    /* Tampon d'accumulation du reacteur : un recv peut couper une frame en deux
     * ou en agreger plusieurs. On accumule puis on frame frame par frame. */
    uint8_t*     rx;
    size_t       rx_len;
    size_t       rx_cap;
} OwlStompConn;

static int stomp_send(OwlStompConn* c, const void* buf, size_t len) {
    return c->tls_active ? owl_tls_send(&c->transport, buf, len)
                         : owl_tcp_send(&c->transport.tcp, buf, len);
}

/* Lecture jusqu'a len octets (bloque jusqu'a au moins 1 octet ou coupure).
 * Route TLS/clair comme le send. */
static int stomp_recv(OwlStompConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv(&c->transport, buf, len)
                         : owl_tcp_recv(&c->transport.tcp, buf, len);
}

static void stomp_conn_free(OwlStompConn* c) {
    if (c == NULL) return;
    for (int i = 0; i < c->nb_subs; i++) {
        owl_mq_desabonne(c->subs[i].handle);
        free(c->subs[i].id);
        free(c->subs[i].dest);
    }
    /* Reacteur possede par la session : ne ferme aucun fd (contrat reacteur),
     * la socket est refermee juste apres. */
    if (c->reacteur != NULL) owl_reacteur_detruit(c->reacteur);
    free(c->rx);
    if (c->tls_active) owl_tls_close(&c->transport);
    else               owl_tcp_close(&c->transport.tcp);
    free(c);
}

/* ============================================================
 * Mapping abonnement -> handle E1.
 * ============================================================ */

/* Retrouve l'abonnement cible d'un MESSAGE entrant par l'id d'abonnement (header
 * `subscription`) ou, a defaut, par la destination. NULL si aucun. */
static StompSub* stomp_sub_find(OwlStompConn* c, const char* id,
                                const char* dest) {
    if (id && id[0]) {
        for (int i = 0; i < c->nb_subs; i++) {
            if (strcmp(c->subs[i].id, id) == 0) return &c->subs[i];
        }
    }
    if (dest && dest[0]) {
        for (int i = 0; i < c->nb_subs; i++) {
            if (strcmp(c->subs[i].dest, dest) == 0) return &c->subs[i];
        }
    }
    return NULL;
}

static bool stomp_sub_add(OwlStompConn* c, const char* id, const char* dest,
                          OwlMqHandle h, bool auto_ack) {
    if (c->nb_subs >= STOMP_MAX_SUBS) return false;
    char* cid = strdup(id ? id : "");
    char* cde = strdup(dest ? dest : "");
    if (cid == NULL || cde == NULL) { free(cid); free(cde); return false; }
    c->subs[c->nb_subs].id       = cid;
    c->subs[c->nb_subs].dest     = cde;
    c->subs[c->nb_subs].handle   = h;
    c->subs[c->nb_subs].auto_ack = auto_ack;
    c->nb_subs++;
    return true;
}

/* ============================================================
 * Encodage / decodage des frames STOMP 1.2 (texte).
 *
 * Tampon octet extensible pour assembler une frame.
 * ============================================================ */

typedef struct { uint8_t* data; size_t len, cap; } StompBuf;

static bool sbuf_ensure(StompBuf* b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t* nb = (uint8_t*)realloc(b->data, ncap);
    if (nb == NULL) return false;
    b->data = nb;
    b->cap = ncap;
    return true;
}

static void sbuf_u8(StompBuf* b, uint8_t v) {
    if (!sbuf_ensure(b, 1)) return;
    b->data[b->len++] = v;
}

static void sbuf_bytes(StompBuf* b, const void* p, size_t n) {
    if (n == 0) return;
    if (!sbuf_ensure(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* Ecrit une valeur de header en echappant les octets speciaux (spec 1.2). */
static void sbuf_escaped(StompBuf* b, const char* s) {
    for (; s && *s; s++) {
        switch (*s) {
            case '\\': sbuf_bytes(b, "\\\\", 2); break;
            case ':':  sbuf_bytes(b, "\\c", 2);  break;
            case '\n': sbuf_bytes(b, "\\n", 2);  break;
            case '\r': sbuf_bytes(b, "\\r", 2);  break;
            default:   sbuf_u8(b, (uint8_t)*s);  break;
        }
    }
}

/* Ecrit une ligne "cle:valeur\n". `escape` faux pour le CONNECT (la spec 1.2
 * interdit l'echappement dans CONNECT/CONNECTED). */
static void stomp_add_header(StompBuf* b, const char* key, const char* val,
                             bool escape) {
    if (escape) sbuf_escaped(b, key); else sbuf_bytes(b, key, strlen(key));
    sbuf_u8(b, ':');
    if (escape) sbuf_escaped(b, val); else sbuf_bytes(b, val, strlen(val));
    sbuf_u8(b, '\n');
}

/* Frame parsee : uniquement les headers utiles au Lot 1 sont extraits. */
typedef struct {
    char           command[32];
    char           destination[512];
    char           message_id[256];
    char           ack[256];           /* jeton d'acquittement (header `ack`, 1.2) */
    char           subscription[256];
    char           content_type[256];
    long           content_length;   /* -1 si absent */
    const uint8_t* body;
    size_t         body_len;
} StompParsed;

/* Desechappe une valeur de header vers `out`. */
static void stomp_unescape(const char* s, size_t len, char* out, size_t out_sz,
                           bool do_unescape) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < out_sz; i++) {
        if (do_unescape && s[i] == '\\' && i + 1 < len) {
            char n = s[i + 1];
            char r;
            switch (n) {
                case 'c':  r = ':';  break;
                case 'n':  r = '\n'; break;
                case 'r':  r = '\r'; break;
                case '\\': r = '\\'; break;
                default:   r = n;    break;
            }
            out[o++] = r;
            i++;
        } else {
            out[o++] = (char)s[i];
        }
    }
    if (out_sz) out[o] = '\0';
}

/* Parse UNE frame complete depuis buf[0..len). Renvoie 1 + *used (octets
 * consommes, corps + 0x00 terminal compris) si une frame complete est presente,
 * 0 si la frame est incomplete (attendre plus d'octets). L'appelant a deja
 * consomme les octets \n de heartbeat en tete. fr->body pointe DANS buf. */
static int stomp_parse_frame(const uint8_t* buf, size_t len, StompParsed* fr,
                             size_t* used) {
    memset(fr, 0, sizeof(*fr));
    fr->content_length = -1;

    /* Ligne de commande. */
    const uint8_t* nl = (const uint8_t*)memchr(buf, '\n', len);
    if (nl == NULL) return 0;                       /* commande incomplete */
    size_t cmd_len = (size_t)(nl - buf);
    if (cmd_len > 0 && buf[cmd_len - 1] == '\r') cmd_len--;
    size_t cn = cmd_len < sizeof(fr->command) - 1 ? cmd_len : sizeof(fr->command) - 1;
    memcpy(fr->command, buf, cn);
    fr->command[cn] = '\0';

    bool is_connected = (strcmp(fr->command, "CONNECTED") == 0);

    /* Headers jusqu'a la ligne vide. */
    size_t pos = (size_t)(nl - buf) + 1;
    for (;;) {
        const uint8_t* eol = (const uint8_t*)memchr(buf + pos, '\n', len - pos);
        if (eol == NULL) return 0;                  /* headers incomplets */
        size_t line_len = (size_t)(eol - (buf + pos));
        size_t raw_len = line_len;
        if (line_len > 0 && buf[pos + line_len - 1] == '\r') line_len--;
        if (line_len == 0) {                        /* ligne vide -> fin headers */
            pos += raw_len + 1;
            break;
        }
        /* Separe cle:valeur au premier ':'. */
        const uint8_t* colon = (const uint8_t*)memchr(buf + pos, ':', line_len);
        if (colon != NULL) {
            size_t klen = (size_t)(colon - (buf + pos));
            const uint8_t* vstart = colon + 1;
            size_t vlen = (size_t)((buf + pos + line_len) - vstart);
            char key[64];
            stomp_unescape((const char*)(buf + pos), klen, key, sizeof(key), !is_connected);
            /* Premiere occurrence gagne (spec : les repetitions sont ignorees). */
            if (strcmp(key, "destination") == 0 && fr->destination[0] == '\0') {
                stomp_unescape((const char*)vstart, vlen, fr->destination, sizeof(fr->destination), !is_connected);
            } else if (strcmp(key, "message-id") == 0 && fr->message_id[0] == '\0') {
                stomp_unescape((const char*)vstart, vlen, fr->message_id, sizeof(fr->message_id), !is_connected);
            } else if (strcmp(key, "ack") == 0 && fr->ack[0] == '\0') {
                stomp_unescape((const char*)vstart, vlen, fr->ack, sizeof(fr->ack), !is_connected);
            } else if (strcmp(key, "subscription") == 0 && fr->subscription[0] == '\0') {
                stomp_unescape((const char*)vstart, vlen, fr->subscription, sizeof(fr->subscription), !is_connected);
            } else if (strcmp(key, "content-type") == 0 && fr->content_type[0] == '\0') {
                stomp_unescape((const char*)vstart, vlen, fr->content_type, sizeof(fr->content_type), !is_connected);
            } else if (strcmp(key, "content-length") == 0 && fr->content_length < 0) {
                char clbuf[32];
                stomp_unescape((const char*)vstart, vlen, clbuf, sizeof(clbuf), !is_connected);
                fr->content_length = strtol(clbuf, NULL, 10);
            }
        }
        pos += raw_len + 1;
    }

    /* Corps : content-length si present, sinon jusqu'au 0x00 terminal. */
    if (fr->content_length >= 0) {
        size_t need = pos + (size_t)fr->content_length + 1;  /* corps + 0x00 */
        if (len < need) return 0;                   /* corps incomplet */
        fr->body = buf + pos;
        fr->body_len = (size_t)fr->content_length;
        *used = need;
        return 1;
    }
    const uint8_t* nul = (const uint8_t*)memchr(buf + pos, '\0', len - pos);
    if (nul == NULL) return 0;                       /* pas de terminateur : attendre */
    fr->body = buf + pos;
    fr->body_len = (size_t)(nul - (buf + pos));
    *used = (size_t)(nul - buf) + 1;
    return 1;
}

static void stomp_rx_append(OwlStompConn* c, const uint8_t* data, size_t len) {
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

static ObjInstance* build_stompSession_template(void) {
    ObjString* name = obj_copy_string("stompSession", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    vm_push(OBJ_VAL(tpl));   /* rooté pendant les sets (grow -> GC) */
    inst_field_set_str(tpl, "Adresse",          "");
    inst_field_set_int(tpl, "Port",             STOMP_PORT_CLEAR);
    inst_field_set_str(tpl, "Utilisateur",      "");
    inst_field_set_str(tpl, "MotDePasse",       "");
    inst_field_set_str(tpl, "HoteVirtuel",      "/");   /* header STOMP `host` */
    inst_field_set_int(tpl, "Option",           0);
    inst_field_set_str(tpl, "CertificatCA",     "");   /* CA privee optionnelle pour TLS */
    inst_field_set_int(tpl, "TimeoutConnexion", 0);
    inst_field_set_int(tpl, "Heartbeat",        0);
    /* _handle : pointeur opaque vers OwlStompConn* (cast intptr_t). 0 = non
     * connecte. Rempli par STOMPConnecte, libere par STOMPDéconnecte. Field
     * interne, non documente cote user. */
    inst_field_set_int(tpl, "_handle",          0);
    vm_pop();
    return tpl;
}

static ObjInstance* build_stompMessage_template(void) {
    ObjString* name = obj_copy_string("stompMessage", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    /* Construit aussi au runtime (une instance fraiche par livraison) : tpl doit
     * rester racine pendant les sets (chaque set alloue key + valeur -> grow ->
     * GC possible). gc-root-obj-fresh. */
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Corps",       "");   /* chaine (texte) ou Buffer en emission */
    inst_field_set_str(tpl, "Destination", "");
    inst_field_set_str(tpl, "TypeContenu", "");
    /* Identifiant : jeton d'acquittement STOMP (header `ack` d'une frame MESSAGE
     * en mode client, a defaut `message-id`). Chaine vide tant que le message
     * n'a pas ete recu en mode ack client. */
    inst_field_set_str(tpl, "Identifiant", "");
    vm_pop();
    return tpl;
}

/* ============================================================
 * Helpers — extraction stompSession + lecture de champs + handle.
 * ============================================================ */

static ObjInstance* resolve_stomp_session(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "stompSession",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "stompSession") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "stompSession",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static const char* stomp_str(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_STRING(v)) return "";
    return AS_CSTRING(v);
}

static int stomp_int(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return 0;
    return (int)AS_INTEGER(v);
}

static OwlStompConn* session_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return NULL;
    return (OwlStompConn*)(intptr_t)AS_INTEGER(v);
}

static void session_set_handle(ObjInstance* inst, OwlStompConn* c) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)c);
}

/* Corps d'un stompMessage : chaine (octets) ou Buffer (binaire). */
static void stomp_msg_body(ObjInstance* msg, const unsigned char** out,
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
 * Livraison C -> OWL : (session stompSession, message stompMessage).
 *
 * STOMP construit un stompMessage frais (Corps chaine texte + Destination +
 * TypeContenu) et le passe au callback. E1 reste le registre + le rooting GC
 * des handlers. Motif de reentrance bornee (E1/http_exec) : repere stackTop ->
 * push callee + args (chacun roote a la creation) -> appel -> reset au repere.
 * ============================================================ */

static void stomp_livre(OwlStompConn* c, const StompParsed* fr) {
    StompSub* sub = stomp_sub_find(c, fr->subscription, fr->destination);
    if (sub == NULL) return;
    Value handler;
    if (!owl_mq_handler(sub->handle, &handler)) return;
    if (c->session == NULL) return;

    Value* repere = vm.stackTop;                    /* frontiere de depilage propre */
    vm_push(handler);                               /* callee au-dessus du repere */
    vm_push(OBJ_VAL(c->session));                   /* arg 1 : session */

    /* arg 2 : stompMessage frais. Instance rootee immediatement (push) : les
     * inst_field_set_* suivants allouent (key + valeurs) -> grow -> GC possible.
     * gc-root-obj-fresh. */
    ObjInstance* msg = build_stompMessage_template();
    vm_push(OBJ_VAL(msg));

    /* Corps = chaine (STOMP est texte-oriente ; le corps peut contenir des \0
     * internes grace a content-length). inst_field_set_value pousse la valeur
     * avant d'allouer la key : la chaine fraiche survit au grow. */
    ObjString* body = obj_copy_string(fr->body ? (const char*)fr->body : "",
                                      (int)fr->body_len);
    inst_field_set_value(msg, "Corps", OBJ_VAL(body));
    inst_field_set_str(msg, "Destination", fr->destination);
    if (fr->content_type[0]) inst_field_set_str(msg, "TypeContenu", fr->content_type);

    /* Mode ack client : pose le jeton d'acquittement (header `ack` de la frame
     * MESSAGE en 1.2, a defaut `message-id`) directement sur l'instance livree,
     * qui survivra si le user la stocke pour l'acquitter plus tard. En mode auto,
     * Identifiant reste "" (le courtier n'attend aucun ack). */
    if (!sub->auto_ack) {
        const char* token = fr->ack[0] ? fr->ack : fr->message_id;
        if (token[0]) inst_field_set_str(msg, "Identifiant", token);
    }

    (void)vm_call_reentrant(handler, 2);

    vm.stackTop = repere;                           /* rejette tout residu de retour */
}

/* Dispatche une frame parsee entrante. Au Lot 1, seuls les MESSAGE declenchent
 * une livraison ; ERROR/RECEIPT sont avales (RECEIPT non demande, ERROR sera
 * exploite aux lots suivants). */
static void stomp_dispatch(OwlStompConn* c, const StompParsed* fr) {
    if (strcmp(fr->command, "MESSAGE") == 0) stomp_livre(c, fr);
}

/* Frame toutes les frames completes accumulees dans `rx`, avale les octets \n
 * de heartbeat en tete, dispatche, puis compacte le reliquat. */
static void stomp_rx_parse(OwlStompConn* c) {
    size_t off = 0;
    for (;;) {
        while (off < c->rx_len && (c->rx[off] == '\n' || c->rx[off] == '\r')) off++;
        if (off >= c->rx_len) break;
        StompParsed fr;
        size_t used = 0;
        if (stomp_parse_frame(c->rx + off, c->rx_len - off, &fr, &used) == 0) break;
        stomp_dispatch(c, &fr);
        off += used;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* Callback de lecture du reacteur : draine la socket, accumule, frame. */
static void stomp_reacteur_lecture_cb(OwlReacteur* r, owl_socket_fd_t fd,
                                      short evts_prets, void* user) {
    OwlStompConn* c = (OwlStompConn*)user;
    if (evts_prets & OWL_REAC_LECTURE) {
        uint8_t tampon[4096];
        int n = stomp_recv(c, tampon, sizeof(tampon));
        if (n > 0) {
            stomp_rx_append(c, tampon, (size_t)n);
            stomp_rx_parse(c);
            return;
        }
        owl_reacteur_retire_fd(r, fd);              /* EOF (n==0) ou erreur (n<0) */
        c->connected = false;                       /* courtier a raccroche */
        c->fd_branche = false;
        return;
    }
    if (evts_prets & OWL_REAC_ERREUR) {
        owl_reacteur_retire_fd(r, fd);
        c->connected = false;
        c->fd_branche = false;
    }
}

/* Cree paresseusement le reacteur possede par la session et y branche la socket
 * vivante en interet LECTURE (une seule fois). false si allocation impossible
 * ou fd invalide. */
static bool stomp_reacteur_assure(OwlStompConn* c) {
    if (c->reacteur == NULL) {
        c->reacteur = owl_reacteur_cree();
        if (c->reacteur == NULL) return false;
    }
    if (!c->fd_branche) {
        if (!owl_reacteur_ajoute_fd(c->reacteur, c->transport.tcp.fd,
                                    OWL_REAC_LECTURE, stomp_reacteur_lecture_cb, c))
            return false;
        c->fd_branche = true;
    }
    return true;
}

/* Lit une frame complete en synchrone (avant que le reacteur ne prenne le
 * relais) : accumule les octets recus dans `rx`, avale les heartbeats de tete,
 * et rend la premiere frame complete. La frame rendue est copiee dans *fr
 * (headers scalaires) ; fr->body N'EST PAS valide apres retour (rx est compacte).
 * false + `detail` renseigne sur coupure de transport. */
static bool stomp_read_frame_sync(OwlStompConn* c, StompParsed* fr,
                                  char* detail, size_t dsz) {
    for (;;) {
        size_t off = 0;
        while (off < c->rx_len && (c->rx[off] == '\n' || c->rx[off] == '\r')) off++;
        if (off > 0) {
            memmove(c->rx, c->rx + off, c->rx_len - off);
            c->rx_len -= off;
        }
        size_t used = 0;
        if (c->rx_len > 0 && stomp_parse_frame(c->rx, c->rx_len, fr, &used) == 1) {
            memmove(c->rx, c->rx + used, c->rx_len - used);
            c->rx_len -= used;
            return true;
        }
        uint8_t tampon[2048];
        int n = stomp_recv(c, tampon, sizeof(tampon));
        if (n <= 0) {
            snprintf(detail, dsz, "connexion coupée en attente de réponse du courtier");
            return false;
        }
        stomp_rx_append(c, tampon, (size_t)n);
    }
}

/* ============================================================
 * Natives.
 * ============================================================ */

static Value fn_stomp_connecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPConnecte");
    if (inst == NULL) return BOOL_VAL(false);

    /* Reconnexion : libere une connexion vivante residuelle (idempotence). */
    OwlStompConn* old = session_handle(inst);
    if (old != NULL) { stomp_conn_free(old); session_set_handle(inst, NULL); }

    const char* adresse = stomp_str(inst, "Adresse");
    const char* user    = stomp_str(inst, "Utilisateur");
    const char* pass    = stomp_str(inst, "MotDePasse");
    const char* vhost   = stomp_str(inst, "HoteVirtuel");
    int timeout   = stomp_int(inst, "TimeoutConnexion");
    int heartbeat = stomp_int(inst, "Heartbeat");
    if (heartbeat < 0) heartbeat = 0;
    int option = stomp_int(inst, "Option");
    bool ssl   = (option & OWL_OPTION_SSL) != 0;

    /* Port : explicite si renseigne, sinon defaut selon le chiffrement. SSL
     * avec le port clair par defaut (61613) -> bascule 61614. */
    int port = stomp_int(inst, "Port");
    if (port <= 0) {
        port = ssl ? STOMP_PORT_SSL : STOMP_PORT_CLEAR;
    } else if (ssl && port == STOMP_PORT_CLEAR) {
        port = STOMP_PORT_SSL;
    }

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", adresse, port);

    OwlStompConn* c = (OwlStompConn*)calloc(1, sizeof(OwlStompConn));
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                              "native", "STOMPConnecte", "cible", cible,
                              "detail", "mémoire insuffisante", NULL);
        return BOOL_VAL(false);
    }
    c->heartbeat = heartbeat;

    owl_tcp_global_init();
    owl_tls_init(&c->transport);
    int tmo = timeout > 0 ? timeout * 1000 : STOMP_CONNECT_TIMEOUT_MS;

    if (ssl) {
        /* CA privee optionnelle : si CertificatCA est renseigne, il devient la
         * racine de confiance ; vide -> NULL (bundle Mozilla). Verification
         * hostname+chaine conservee (VERIFY_FULL) dans les deux cas. La chaine
         * du champ d'instance vit pendant le connect (PEM parse a la volee). */
        const char* ca = stomp_str(inst, "CertificatCA");
        const char* extra_ca = (ca && ca[0] != '\0') ? ca : NULL;
        if (!owl_tls_connect(&c->transport, adresse, port, OWL_TLS_VERIFY_FULL,
                             tmo, extra_ca)) {
            owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                                  "native", "STOMPConnecte", "cible", cible,
                                  "detail", owl_tls_last_error(&c->transport), NULL);
            stomp_conn_free(c);
            return BOOL_VAL(false);
        }
        c->tls_active = true;
    } else {
        if (!owl_tcp_connect(&c->transport.tcp, adresse, port, tmo)) {
            owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                                  "native", "STOMPConnecte", "cible", cible,
                                  "detail", owl_tcp_last_error(&c->transport.tcp), NULL);
            stomp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* CONNECT : pas d'echappement (spec 1.2). heart-beat:H,H symetrique. */
    {
        StompBuf f = {0};
        sbuf_bytes(&f, "CONNECT\n", 8);
        stomp_add_header(&f, "accept-version", "1.2", false);
        stomp_add_header(&f, "host", vhost && vhost[0] ? vhost : "/", false);
        if (user && user[0]) {
            stomp_add_header(&f, "login", user, false);
            stomp_add_header(&f, "passcode", pass ? pass : "", false);
        }
        char hb[48];
        snprintf(hb, sizeof(hb), "%d,%d", heartbeat, heartbeat);
        stomp_add_header(&f, "heart-beat", hb, false);
        sbuf_u8(&f, '\n');                           /* ligne vide : fin des headers */
        sbuf_u8(&f, '\0');                           /* terminateur de frame */
        bool ok = (f.data != NULL) && (stomp_send(c, f.data, f.len) == (int)f.len);
        free(f.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                                  "native", "STOMPConnecte", "cible", cible,
                                  "detail", "émission du CONNECT impossible", NULL);
            stomp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    char detail[300];
    StompParsed fr;
    if (!stomp_read_frame_sync(c, &fr, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                              "native", "STOMPConnecte", "cible", cible,
                              "detail", detail, NULL);
        stomp_conn_free(c);
        return BOOL_VAL(false);
    }
    if (strcmp(fr.command, "CONNECTED") != 0) {
        /* ERROR (auth/vhost refuse) ou frame inattendue. */
        snprintf(detail, sizeof(detail),
                 "réponse inattendue du courtier (%s)", fr.command);
        owl_emit_native_error(E_NATIVE_STOMP_CONNECT_FAILURE, "STOMPConnecte",
                              "native", "STOMPConnecte", "cible", cible,
                              "detail", detail, NULL);
        stomp_conn_free(c);
        return BOOL_VAL(false);
    }

    c->connected = true;
    c->session = inst;              /* arg 1 des livraisons ; instance rootee OWL */
    session_set_handle(inst, c);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_stomp_deconnecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPDéconnecte");
    if (inst == NULL) return NIL_VAL;
    OwlStompConn* c = session_handle(inst);
    if (c != NULL) {
        if (c->connected) {
            /* DISCONNECT (RECEIPT best-effort non demande) puis fermeture. */
            StompBuf f = {0};
            sbuf_bytes(&f, "DISCONNECT\n\n", 12);
            sbuf_u8(&f, '\0');
            if (f.data != NULL) stomp_send(c, f.data, f.len);
            free(f.data);
        }
        stomp_conn_free(c);
        session_set_handle(inst, NULL);
    }
    return NIL_VAL;
}

/* STOMPPublie(<session>, <destination>, <message>) -> booleen. Validation des
 * types d'arguments AVANT le controle de connexion (parite AMQP/MQTT). */
static Value fn_stomp_publie(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPPublie");
    if (inst == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "STOMPPublie",
                              "native", "STOMPPublie", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* destination = AS_CSTRING(args[1]);

    if (!IS_INSTANCE(args[2]) ||
        AS_INSTANCE(args[2])->structName == NULL ||
        strcmp(AS_INSTANCE(args[2])->structName->chars, "stompMessage") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "STOMPPublie",
                              "native", "STOMPPublie", "arg", "3",
                              "expected", "stompMessage", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* msg = AS_INSTANCE(args[2]);

    OwlStompConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_STOMP_PUBLISH_FAILURE, "STOMPPublie",
                              "native", "STOMPPublie",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    const char* content_type = stomp_str(msg, "TypeContenu");
    const unsigned char* body;
    size_t body_len;
    stomp_msg_body(msg, &body, &body_len);

    StompBuf f = {0};
    sbuf_bytes(&f, "SEND\n", 5);
    stomp_add_header(&f, "destination", destination, true);
    if (content_type && content_type[0]) stomp_add_header(&f, "content-type", content_type, true);
    char cl[32];
    snprintf(cl, sizeof(cl), "%lu", (unsigned long)body_len);
    stomp_add_header(&f, "content-length", cl, true);
    sbuf_u8(&f, '\n');                               /* fin des headers */
    sbuf_bytes(&f, body, body_len);
    sbuf_u8(&f, '\0');                               /* terminateur de frame */
    bool ok = (f.data != NULL) && (stomp_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_STOMP_PUBLISH_FAILURE, "STOMPPublie",
                              "native", "STOMPPublie",
                              "detail", "émission du SEND impossible", NULL);
        return BOOL_VAL(false);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

/* STOMPAbonne(<session>, <destination>, <procedure>) -> booleen. Validation des
 * types d'arguments AVANT le controle de connexion (parite AMQP/MQTT). */
static Value fn_stomp_abonne(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPAbonne");
    if (inst == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "STOMPAbonne",
                              "native", "STOMPAbonne", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* destination = AS_CSTRING(args[1]);

    /* autoAck (4e arg optionnel) : Vrai par defaut (= ack:auto, le courtier
     * n'attend aucun acquittement) ; Faux -> ack:client-individual (chaque
     * message doit etre acquitte via STOMPAcquitte / rejete via STOMPRejette). */
    bool auto_ack = (argCount >= 4 && IS_BOOL(args[3])) ? AS_BOOL(args[3]) : true;

    /* Enregistre le handler via E1 : valide qu'il est appelable et le tient
     * comme racine GC. Refus (non appelable / registre plein) -> ARG_TYPE_MISMATCH,
     * avant tout controle de connexion. */
    OwlMqHandle h = owl_mq_abonne(args[2]);
    if (h == OWL_MQ_HANDLE_NUL) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "STOMPAbonne",
                              "native", "STOMPAbonne", "arg", "3",
                              "expected", "procédure", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }

    OwlStompConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_STOMP_SUBSCRIBE_FAILURE, "STOMPAbonne",
                              "native", "STOMPAbonne",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    /* Id d'abonnement genere (unique par session). */
    char id[32];
    snprintf(id, sizeof(id), "%d", ++c->next_sub_id);

    StompBuf f = {0};
    sbuf_bytes(&f, "SUBSCRIBE\n", 10);
    stomp_add_header(&f, "id", id, true);
    stomp_add_header(&f, "destination", destination, true);
    stomp_add_header(&f, "ack", auto_ack ? "auto" : "client-individual", true);
    sbuf_u8(&f, '\n');
    sbuf_u8(&f, '\0');
    bool sent = (f.data != NULL) && (stomp_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!sent) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_STOMP_SUBSCRIBE_FAILURE, "STOMPAbonne",
                              "native", "STOMPAbonne",
                              "detail", "émission du SUBSCRIBE impossible", NULL);
        return BOOL_VAL(false);
    }

    if (!stomp_sub_add(c, id, destination, h, auto_ack)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_STOMP_SUBSCRIBE_FAILURE, "STOMPAbonne",
                              "native", "STOMPAbonne",
                              "detail", "nombre maximal d'abonnements atteint sur la session", NULL);
        return BOOL_VAL(false);
    }

    /* Reacteur possede par la session : cree + fd branche. STOMPAttend le
     * tickera pour dispatcher les MESSAGE entrants. */
    stomp_reacteur_assure(c);

    owl_clear_error();
    return BOOL_VAL(true);
}

/* STOMPAttend(<session>, <delai>) -> booleen. Pompe le reacteur possede par la
 * session pendant `delai` ms au plus : dispatche les MESSAGE entrants aux
 * procedures abonnees, puis rend la main. Renvoie Vrai tant que la session
 * reste connectee, Faux si le courtier a raccroche pendant l'attente. Idiome
 * worker : TANTQUE STOMPAttend(s, 1000) ... FIN. */
static Value fn_stomp_attend(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPAttend");
    if (inst == NULL) return BOOL_VAL(false);

    OwlStompConn* c = session_handle(inst);
    if (c == NULL || !c->connected) return BOOL_VAL(false);

    int64_t delai = IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : 0;
    if (delai < 0) delai = 0;

    if (!stomp_reacteur_assure(c)) return BOOL_VAL(c->connected);

    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) break;                    /* courtier a raccroche */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)delai) break;        /* fenetre epuisee */
        int reste = (int)((uint64_t)delai - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) break;  /* poll casse */
    }
    return BOOL_VAL(c->connected);
}

/* Valide qu'un argument est un stompMessage porteur d'un jeton d'acquittement
 * non vide (champ Identifiant), AVANT tout acces reseau. Renvoie le jeton (chaine
 * vivante tant que args reste sur la pile OWL) ou NULL avec une erreur
 * ARG_TYPE_MISMATCH posee. */
static const char* stomp_ack_token(Value v, const char* fn) {
    if (!IS_INSTANCE(v) ||
        AS_INSTANCE(v)->structName == NULL ||
        strcmp(AS_INSTANCE(v)->structName->chars, "stompMessage") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "2",
                              "expected", "stompMessage", "actual", "un autre type", NULL);
        return NULL;
    }
    const char* tok = stomp_str(AS_INSTANCE(v), "Identifiant");
    if (tok == NULL || tok[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "2",
                              "expected", "stompMessage acquittable (jeton non vide)",
                              "actual", "message sans jeton d'acquittement", NULL);
        return NULL;
    }
    return tok;
}

/* STOMPAcquitte(<session>, <message>) -> booleen. Emet une frame ACK
 * (header id:<jeton>) pour un message recu en mode ack client. Validation du
 * type d'argument AVANT le controle de connexion (parite AMQP/MQTT). */
static Value fn_stomp_acquitte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPAcquitte");
    if (inst == NULL) return BOOL_VAL(false);

    const char* tok = stomp_ack_token(args[1], "STOMPAcquitte");
    if (tok == NULL) return BOOL_VAL(false);

    OwlStompConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_STOMP_ACK_FAILURE, "STOMPAcquitte",
                              "native", "STOMPAcquitte",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    StompBuf f = {0};
    sbuf_bytes(&f, "ACK\n", 4);
    stomp_add_header(&f, "id", tok, true);
    sbuf_u8(&f, '\n');                                /* fin des headers */
    sbuf_u8(&f, '\0');                               /* terminateur de frame */
    bool ok = (f.data != NULL) && (stomp_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_STOMP_ACK_FAILURE, "STOMPAcquitte",
                              "native", "STOMPAcquitte",
                              "detail", "émission de l'ACK impossible", NULL);
        return BOOL_VAL(false);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

/* STOMPRejette(<session>, <message> [, <remetEnFile>]) -> booleen. Emet une frame
 * NACK (header id:<jeton>) pour un message recu en mode ack client. `remetEnFile`
 * (defaut Vrai) est porte par le header `requeue` (extension RabbitMQ) : Vrai ->
 * le message repart en file (redelivrable), Faux -> ecarte (ou dead-letter).
 * Validation du type d'argument AVANT le controle de connexion (parite AMQP/MQTT). */
static Value fn_stomp_rejette(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_stomp_session(args[0], "STOMPRejette");
    if (inst == NULL) return BOOL_VAL(false);

    const char* tok = stomp_ack_token(args[1], "STOMPRejette");
    if (tok == NULL) return BOOL_VAL(false);

    OwlStompConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_STOMP_ACK_FAILURE, "STOMPRejette",
                              "native", "STOMPRejette",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    bool requeue = (argCount >= 3 && IS_BOOL(args[2])) ? AS_BOOL(args[2]) : true;

    StompBuf f = {0};
    sbuf_bytes(&f, "NACK\n", 5);
    stomp_add_header(&f, "id", tok, true);
    stomp_add_header(&f, "requeue", requeue ? "true" : "false", true);
    sbuf_u8(&f, '\n');                                /* fin des headers */
    sbuf_u8(&f, '\0');                               /* terminateur de frame */
    bool ok = (f.data != NULL) && (stomp_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_STOMP_ACK_FAILURE, "STOMPRejette",
                              "native", "STOMPRejette",
                              "detail", "émission du NACK impossible", NULL);
        return BOOL_VAL(false);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_session[] = {
    {"session", "stompSession", "session STOMP décrivant les paramètres de connexion"}, {0}
};
static const OwlSignature sig_connecte[] = {
    {1, p_session, "booléen", "vrai si la connexion au courtier a réussi, faux sinon"}, {0}
};
static const OwlSignature sig_deconnecte[] = {
    {1, p_session, "rien", NULL}, {0}
};

static const OwlParam p_publie[] = {
    {"session",     "stompSession", "session STOMP connectée au courtier"},
    {"destination", "chaîne",       "destination sur laquelle publier le message"},
    {"message",     "stompMessage", "message à publier"},
    {0}
};
static const OwlSignature sig_publie[] = {
    {3, p_publie, "booléen", "vrai si le message a été publié, faux sinon"}, {0}
};

static const OwlParam p_abonne[] = {
    {"session",     "stompSession", "session STOMP connectée au courtier"},
    {"destination", "chaîne",       "destination à laquelle s'abonner"},
    {"procédure",   "procédure",    "procédure appelée à chaque message reçu (session, message)"},
    {"autoAck",     "booléen",      "vrai pour acquitter automatiquement les messages, faux pour un acquittement manuel via STOMPAcquitte ou STOMPRejette (optionnel, vrai par défaut)"},
    {0}
};
#define STOMP_ABONNE_RET "vrai si l'abonnement a été mis en place, faux sinon"
static const OwlSignature sig_abonne[] = {
    {3, p_abonne, "booléen", STOMP_ABONNE_RET},
    {4, p_abonne, "booléen", STOMP_ABONNE_RET},
    {0}
};

static const OwlParam p_acquitte[] = {
    {"session", "stompSession", "session STOMP connectée au courtier"},
    {"message", "stompMessage", "message reçu à acquitter (le jeton d'acquittement est porté par le message)"},
    {0}
};
static const OwlSignature sig_acquitte[] = {
    {2, p_acquitte, "booléen", "vrai si l'acquittement a été émis, faux sinon"}, {0}
};

static const OwlParam p_rejette[] = {
    {"session",     "stompSession", "session STOMP connectée au courtier"},
    {"message",     "stompMessage", "message reçu à rejeter (le jeton d'acquittement est porté par le message)"},
    {"remetEnFile", "booléen",      "vrai pour remettre le message en file (il pourra être redélivré), faux pour l'écarter (optionnel, vrai par défaut)"},
    {0}
};
#define STOMP_REJETTE_RET "vrai si le rejet a été émis, faux sinon"
static const OwlSignature sig_rejette[] = {
    {2, p_rejette, "booléen", STOMP_REJETTE_RET},
    {3, p_rejette, "booléen", STOMP_REJETTE_RET},
    {0}
};

static const OwlParam p_attend[] = {
    {"session", "stompSession", "session STOMP connectée au courtier"},
    {"délai",   "entier",       "durée maximale d'attente, en millisecondes"},
    {0}
};
static const OwlSignature sig_attend[] = {
    {2, p_attend, "booléen", "vrai si la session est toujours connectée, faux si le courtier a fermé la connexion pendant l'attente"}, {0}
};

/* ============================================================
 * Method bindings — formes prefixe session.Xxx().
 * ============================================================ */

static const OwlMethodBinding MB_CONNECTE[] = {
    { "stompSession", "Connecte", 1 }, { "stompSession", "Connect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECONNECTE[] = {
    { "stompSession", "Deconnecte", 1 }, { "stompSession", "Disconnect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PUBLIE[] = {
    { "stompSession", "Publie", 3 }, { "stompSession", "Publish", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ABONNE[] = {
    { "stompSession", "Abonne", -1 }, { "stompSession", "Subscribe", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ATTEND[] = {
    { "stompSession", "Attend", 2 }, { "stompSession", "Wait", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ACQUITTE[] = {
    { "stompSession", "Acquitte", 2 }, { "stompSession", "Ack", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REJETTE[] = {
    { "stompSession", "Rejette", -1 }, { "stompSession", "Reject", -1 }, { NULL, NULL, 0 }
};

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define M_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "STOMP", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_STOMP[] = {
    M_DEF("STOMPConnecte", NULL, "STOMPConnect",
        "Ouvre une connexion vers un courtier STOMP à partir des paramètres d'une session, et indique si la connexion a réussi.",
        "Opens a connection to a STOMP broker from a session's parameters, and reports whether the connection succeeded.",
        sig_connecte, fn_stomp_connecte, MB_CONNECTE),
    M_DEF("STOMPDéconnecte", "STOMPDeconnecte", "STOMPDisconnect",
        "Ferme une connexion STOMP ouverte.",
        "Closes an open STOMP connection.",
        sig_deconnecte, fn_stomp_deconnecte, MB_DECONNECTE),
    M_DEF("STOMPPublie", NULL, "STOMPPublish",
        "Publie un message sur une destination du courtier STOMP et indique si la publication a réussi.",
        "Publishes a message to a destination on the STOMP broker and reports whether it succeeded.",
        sig_publie, fn_stomp_publie, MB_PUBLIE),
    M_DEF("STOMPAbonne", NULL, "STOMPSubscribe",
        "Abonne une procédure aux messages publiés sur une destination du courtier STOMP ; la procédure est appelée à chaque message reçu.",
        "Subscribes a procedure to messages published on a STOMP broker destination; the procedure is called for each incoming message.",
        sig_abonne, fn_stomp_abonne, MB_ABONNE),
    M_DEF("STOMPAttend", NULL, "STOMPWait",
        "Attend et traite les messages entrants d'une session STOMP pendant un délai maximal exprimé en millisecondes, en appelant les procédures abonnées à mesure que les messages arrivent ; renvoie vrai tant que la session reste connectée.",
        "Waits for and processes the incoming messages of a STOMP session for a maximum delay in milliseconds, calling the subscribed procedures as messages arrive; returns true as long as the session stays connected.",
        sig_attend, fn_stomp_attend, MB_ATTEND),
    M_DEF("STOMPAcquitte", NULL, "STOMPAck",
        "Acquitte un message STOMP reçu en mode d'acquittement client, à partir de son jeton d'acquittement, et indique si l'acquittement a été émis.",
        "Acknowledges a STOMP message received in client acknowledgement mode, from its acknowledgement token, and reports whether the acknowledgement was sent.",
        sig_acquitte, fn_stomp_acquitte, MB_ACQUITTE),
    M_DEF("STOMPRejette", NULL, "STOMPNack",
        "Rejette un message STOMP reçu en mode d'acquittement client, avec ou sans remise en file, et indique si le rejet a été émis.",
        "Rejects a STOMP message received in client acknowledgement mode, with or without requeue, and reports whether the rejection was sent.",
        sig_rejette, fn_stomp_rejette, MB_REJETTE),
};

#undef M_DEF

/* ============================================================
 * Types reifies stompSession + stompMessage.
 * ============================================================ */

static const OwlField FIELDS_stompSession[] = {
    {"Adresse",          NULL,          "chaîne",  "\"\"",    "adresse du courtier STOMP (IP ou nom d'hôte)", "STOMP broker address (IP or host name)", .nameEN="Address"},
    {"Port",             NULL,          "entier",  "61613",   "port du courtier (61613 par défaut)", "broker port (61613 by default)", .nameEN="Port"},
    {"Utilisateur",      NULL,          "chaîne",  "\"\"",    "nom d'utilisateur pour l'authentification", "user name for authentication", .nameEN="User"},
    {"MotDePasse",       NULL,          "chaîne",  "\"\"",    "mot de passe pour l'authentification", "password for authentication", .nameEN="Password"},
    {"HoteVirtuel",      "HôteVirtuel", "chaîne",  "\"/\"",   "hôte virtuel du courtier (header STOMP « host », « / » par défaut)", "virtual host of the broker (STOMP \"host\" header, \"/\" by default)", .nameEN="VirtualHost"},
    {"Option",           NULL,          "entier",  "0",       "options de connexion (optionSSL pour une connexion chiffrée)", "connection options (optionSSL for an encrypted connection)", .nameEN="Option"},
    {"CertificatCA",     NULL,          "chaîne",  "\"\"",    "chemin d'un fichier PEM de certificat d'autorité (CA) à utiliser pour valider le serveur en TLS ; vide = autorités publiques par défaut", "path to a PEM certificate-authority (CA) file used to validate the server over TLS; empty = default public authorities", .nameEN="CACertificate"},
    {"TimeoutConnexion", NULL,          "entier",  "0",       "délai de connexion en secondes (0 = défaut)", "connection timeout in seconds (0 = default)", .nameEN="ConnectionTimeout"},
    {"Heartbeat",        NULL,          "entier",  "0",       "intervalle de battement de cœur souhaité, en millisecondes (0 = désactivé)", "requested heartbeat interval, in milliseconds (0 = disabled)", .nameEN="Heartbeat"},
    {0}
};

static const OwlField FIELDS_stompMessage[] = {
    {"Corps",       NULL, "chaîne", "\"\"",  "corps du message : chaîne ou Buffer en émission, chaîne en réception", "message body: string or Buffer when sending, string when receiving", .nameEN="Body"},
    {"Destination", NULL, "chaîne", "\"\"",  "destination du message", "destination of the message", .nameEN="Destination"},
    {"TypeContenu", NULL, "chaîne", "\"\"",  "type de contenu MIME du corps (ex. \"text/plain\")", "MIME content type of the body (e.g. \"text/plain\")", .nameEN="ContentType"},
    {"Identifiant", NULL, "chaîne", "\"\"",  "jeton d'acquittement, renseigné en réception en mode d'acquittement client (à passer à STOMPAcquitte ou STOMPRejette)", "acknowledgement token, set on receipt in client acknowledgement mode (to pass to STOMPAck or STOMPNack)", .nameEN="Tag"},
    {0}
};

static const OwlTypeDef TYPES_STOMP[] = {
    { .canonical      = "stompSession",
      .canonicalAscii = "stompSession",
      .aliases        = NULL,
      .category       = "STOMP",
      .descFR         = "Paramètres de connexion à un courtier STOMP (adresse, port, identifiants, hôte virtuel, options). Passé à STOMPConnecte.",
      .descEN         = "Connection parameters for a STOMP broker (address, port, credentials, virtual host, options). Passed to STOMPConnect.",
      .fields         = FIELDS_stompSession,
      .nameEN         = "stompSession",
      .genre          = OWL_GENRE_F },   /* la session */
    { .canonical      = "stompMessage",
      .canonicalAscii = "stompMessage",
      .aliases        = NULL,
      .category       = "STOMP",
      .descFR         = "Message STOMP échangé avec un courtier (corps, destination, type de contenu). Publié par STOMPPublie, reçu par STOMPAbonne.",
      .descEN         = "STOMP message exchanged with a broker (body, destination, content type). Published by STOMPPublish, received by STOMPSubscribe.",
      .fields         = FIELDS_stompMessage,
      .nameEN         = "stompMessage",
      .genre          = OWL_GENRE_M },   /* le message */
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_stomp_init(void) {
    /* Le socle Message Queue E1 est arme par stdlib_mq_init (point central
     * neutre, appele par owl_stdlib_init AVANT tout client MQ). Ce client le
     * partage avec MQTT et AMQP (contrat multi-protocole). La constante
     * optionSSL est deja enregistree globalement (client MQTT) : on la reutilise
     * (transport chiffre effectif via owl_tls_connect). */
    owl_registry_register_module(DEFS_STOMP,
        (int)(sizeof(DEFS_STOMP) / sizeof(DEFS_STOMP[0])));
    owl_registry_register_types(TYPES_STOMP,
        (int)(sizeof(TYPES_STOMP) / sizeof(TYPES_STOMP[0])));

    ObjInstance* tpl = build_stompSession_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("stompSession", OBJ_VAL(tpl));
    vm_pop();

    ObjInstance* tplMsg = build_stompMessage_template();
    vm_push(OBJ_VAL(tplMsg));
    vm_type_register("stompMessage", OBJ_VAL(tplMsg));
    vm_pop();
}
