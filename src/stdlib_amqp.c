// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Client AMQP 0-9-1 / RabbitMQ, Lot A. Voir stdlib_amqp.h pour
 * le contrat.
 *
 * ETAT : Lot A. AMQPConnecte ouvre le transport (TCP clair, ou TLS via
 * optionSSL), joue le handshake AMQP 0-9-1 complet (protocol header ->
 * Connection.Start/Start-Ok SASL PLAIN -> Tune/Tune-Ok -> Open/Open-Ok ->
 * Channel.Open/Open-Ok) et renvoie Vrai si le serveur ouvre le channel auto.
 * AMQPDéconnecte emet Connection.Close, attend Close-Ok, ferme le transport.
 * Chaque reponse serveur est lue en synchrone (recv_exact), les method-ids
 * attendus sont valides, un Connection.Close serveur (auth/vhost) est lu et
 * remonte proprement (Faux + ErreurInfo). Publication/consommation : lots
 * suivants.
 *
 * Surface HORS-CANON (aucun equivalent canon) — validee dev 2026-09-15 :
 *   type amqpConnection (parametres de connexion + channel auto cache)
 *   AMQPConnecte   -> AMQPConnect    : (amqpConnection) -> booleen
 *   AMQPDéconnecte -> AMQPDisconnect : (amqpConnection) -> rien
 *
 * Modele channels : CACHES. AMQPConnecte ouvrira (au vert) la connexion + UN
 * channel auto en interne, invisible du user (Multi-channel = phase 2).
 *
 * Wire (au vert) : recode maison, reference = spec OASIS AMQP 0-9-1.
 *   En-tete protocole : "AMQP" 0x00 0x00 0x09 0x01
 *   Frame : [type(1)][channel(2 BE)][size(4 BE)][payload][0xCE]
 *     types : method(1) / header(2) / body(3) / heartbeat(8)
 *   Handshake : Connection.Start(broker) -> Connection.Start-Ok(client, SASL
 *     PLAIN \0user\0pass + client-properties field-table) -> Connection.Tune
 *     -> Connection.Tune-Ok -> Connection.Open(vhost) -> Connection.Open-Ok
 *     -> Channel.Open(1) -> Channel.Open-Ok.
 *   Deconnexion : Connection.Close -> Close-Ok, puis fermeture du transport.
 *   La connexion+channel vivants seront stockes dans l'instance amqpConnection
 *   (champ interne cache _handle -> OwlAmqpConn*, pattern ftpClient/mqttSession),
 *   sur net_tcp / net_tcp_tls (TLS si optionSSL, port 5671).
 */

#include "stdlib_amqp.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
/* net_tcp.h tire winsock2.h sous Windows -> collision TokenType (winnt.h vs
 * scanner.h). Meme garde que stdlib_mqtt.c / stdlib_ftp.c. */
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
#include "sentinels.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

/* Ports par defaut AMQP 0-9-1 : 5672 en clair, 5671 en TLS. */
#define AMQP_PORT_CLEAR 5672
#define AMQP_PORT_SSL   5671
/* Valeur de la constante optionSSL (masque OU-binaire sur ..Option). La
 * constante elle-meme est deja enregistree globalement (par le client MQTT) :
 * on la reutilise, on ne la re-enregistre pas (invariant registry). */
#define OWL_OPTION_SSL  1

/* Horloge monotone en millisecondes, partagee avec le socle net (definie dans
 * net_tcp_server.c). Borne la pompe AMQPAttend. Extern-declaree comme dans
 * stdlib_mqtt.c pour ne pas tirer winsock via net_tcp_server.h. */
extern uint64_t owl_tcp_server_horloge_ms(void);

/* ============================================================
 * Template du type reifie amqpConnection.
 *
 * Struct de parametres de connexion (scalaires) — meme pattern que
 * mqttSession : un template clone a chaque `c est un amqpConnection`.
 * ============================================================ */

static ObjInstance* build_amqpConnection_template(void) {
    ObjString* name = obj_copy_string("amqpConnection", 14);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str (tpl, "Adresse",          "");
    inst_field_set_int (tpl, "Port",             AMQP_PORT_CLEAR);  /* 5671 en SSL */
    inst_field_set_str (tpl, "Utilisateur",      "");
    inst_field_set_str (tpl, "MotDePasse",       "");
    inst_field_set_str (tpl, "HoteVirtuel",      "/");   /* vhost par defaut RabbitMQ */
    inst_field_set_int (tpl, "Option",           0);
    inst_field_set_str (tpl, "CertificatCA",     "");   /* CA privee optionnelle pour TLS */
    inst_field_set_int (tpl, "TimeoutConnexion", 0);
    inst_field_set_int (tpl, "Heartbeat",        0);
    /* NbMessages : nombre de messages en attente dans la dernière queue
     * déclarée (renseigné par AMQPDéclareQueue depuis Declare-Ok). 0 tant
     * qu'aucune queue n'a été déclarée. Lecture seule côté user. */
    inst_field_set_int (tpl, "NbMessages",       0);
    /* _handle : pointeur opaque vers la connexion+channel vivants (cast
     * intptr_t). 0 = non connecte. Rempli/libere au vert par AMQPConnecte /
     * AMQPDéconnecte. Field interne, non documente cote user. */
    inst_field_set_int (tpl, "_handle",          0);
    return tpl;
}

/* ============================================================
 * Template du type reifie amqpMessage.
 *
 * Message AMQP de 1re classe : corps + persistance + proprietes riches
 * (content-type, correlation-id, priority…). Symetrique publish/consume :
 * AMQPPublie encode delivery-mode + property-flags depuis l'instance,
 * AMQPConsomme reconstruit une instance depuis le content-header decode.
 * Champs stockes sous leur forme ASCII accent-strippee (storage canon) ;
 * l'acces FR accentue / EN passe par la resolution du registre.
 * ============================================================ */

static ObjInstance* build_amqpMessage_template(void) {
    ObjString* name = obj_copy_string("amqpMessage", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    /* Construit aussi au runtime (une instance fraîche par livraison) : tpl doit
     * rester raciné pendant les inst_field_set (chaque set alloue key + valeur ->
     * grow -> GC possible). gc-root-obj-fresh. */
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str (tpl, "Corps",           "");   /* corps (chaine ou Buffer) */
    inst_field_set_str (tpl, "CleRoutage",      "");   /* renseignee en reception */
    inst_field_set_bool(tpl, "Persistant",      false);/* delivery-mode 2 si Vrai */
    inst_field_set_str (tpl, "TypeContenu",     "");
    inst_field_set_str (tpl, "EncodageContenu", "");
    inst_field_set_str (tpl, "IdCorrelation",   "");
    inst_field_set_str (tpl, "RepondreA",       "");
    inst_field_set_int (tpl, "Priorite",        0);
    inst_field_set_str (tpl, "Expiration",      "");
    inst_field_set_str (tpl, "IdMessage",       "");
    inst_field_set_str (tpl, "Type",            "");
    inst_field_set_int (tpl, "Identifiant",     0);    /* delivery-tag en reception */
    vm_pop();
    return tpl;
}

/* ============================================================
 * Helper — extraction amqpConnection.
 * ============================================================ */

static ObjInstance* resolve_amqp_connection(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "amqpConnection",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "amqpConnection") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "amqpConnection",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

/* ============================================================
 * Lecture des champs de l'instance amqpConnection.
 * ============================================================ */

static const char* amqp_str(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_STRING(v)) return "";
    return AS_CSTRING(v);
}

static int amqp_int(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return 0;
    return (int)AS_INTEGER(v);
}

static bool amqp_bool(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    return ok && IS_BOOL(v) && AS_BOOL(v);
}

/* Valeur brute d'un champ d'instance (out renseigne si present). */
static bool amqp_get(ObjInstance* inst, const char* champ, Value* out) {
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, out);
    vm_pop();
    return ok;
}

/* ============================================================
 * Connexion vivante — pointeur opaque cache dans l'instance (champ _handle,
 * cast intptr_t). Un seul transport, dispatch send/recv par tls_active
 * (meme pattern que mqttSession / ftpClient).
 * ============================================================ */

/* Nombre maximal de consommateurs simultanes sur une connexion. Chaque
 * consommateur mappe un consumer-tag (assigne par le broker) vers un handle E1
 * (borne par OWL_MQ_MAX_ABONNEMENTS, cap 8, racine GC partagee). Depassement ->
 * consommation refusee (pas de mode degrade). */
#define AMQP_MAX_CONSUMERS 8

/* Mapping consumer-tag -> handle E1, par connexion. Le consumer-tag est assigne
 * par le broker (Consume-Ok) : la livraison retrouve le handler abonne au tag
 * d'un Basic.Deliver entrant. La copie du tag est C-heap (independante des
 * Value OWL). auto_ack memorise le mode no-ack negocie a la consommation. */
typedef struct {
    char*       tag;       /* copie malloc (NUL-terminee) du consumer-tag */
    OwlMqHandle handle;    /* handle E1 tenant le handler comme racine GC */
    bool        auto_ack;  /* Vrai = no-ack=1 (le broker n'attend pas d'ack) */
} AmqpConsumer;

/* Etat d'assemblage d'un Basic.Deliver multi-frame : la method-frame ouvre la
 * livraison (consumer-tag + delivery-tag + routing-key), la content-header-frame
 * donne la taille du corps, puis une ou plusieurs body-frames l'assemblent. La
 * livraison OWL n'a lieu qu'une fois le corps complet. */
typedef struct {
    bool     active;            /* une method Basic.Deliver est en cours */
    char     consumer_tag[256]; /* tag du consommateur cible */
    char     routing_key[256];  /* cle de routage du message */
    uint64_t delivery_tag;      /* identifiant a passer a AMQPAcquitte */
    uint64_t body_size;         /* taille annoncee par la content-header-frame */
    uint8_t* body;              /* corps accumule (malloc, reutilise entre livraisons) */
    size_t   body_len;          /* octets deja accumules */
    size_t   body_cap;          /* capacite allouee de `body` */

    /* Proprietes decodees du content-header (basic.properties), remises a plat
     * au debut de chaque Basic.Deliver. delivery_mode 2 -> Persistant Vrai. */
    uint8_t  delivery_mode;
    uint8_t  priority;
    char     content_type[256];
    char     content_encoding[256];
    char     correlation_id[256];
    char     reply_to[256];
    char     expiration[256];
    char     message_id[256];
    char     msg_type[256];
} AmqpDelivery;

typedef struct {
    OwlTlsSocket transport;   /* .tcp = TCP sous-jacent (clair ou chiffre) */
    bool         tls_active;  /* handshake TLS effectue -> router via owl_tls_* */
    bool         connected;   /* Channel.Open-Ok recu */
    uint16_t     channel;     /* channel auto cache (1) */
    uint32_t     frame_max;   /* frame-max negocie (echo du serveur au Tune-Ok) */
    bool         confirm_mode;/* publisher confirms actif (Confirm.Select-Ok recu) :
                               * chaque AMQPPublie attend l'accuse du broker */

    /* --- Consommation (Lot C) --- */
    ObjInstance* session;     /* instance amqpConnection proprietaire ; arg 1 de
                               * la livraison. Rootee par l'appelant (portee OWL),
                               * non scannee via ce champ C. */
    AmqpConsumer consumers[AMQP_MAX_CONSUMERS];
    int          nb_consumers;
    AmqpDelivery deliv;       /* assemblage du Basic.Deliver en cours */

    /* --- Pompe possedee par la session (, calque MQTT) --- */
    OwlReacteur* reacteur;    /* reacteur PROPRE, cree paresseusement (1re
                               * consommation / attente). AMQPAttend le ticke. */
    bool         fd_branche;  /* socket vivante deja enregistree en LECTURE */

    /* Tampon d'accumulation du reacteur : un recv peut couper une trame ou en
     * agreger plusieurs. On accumule puis on frame trame par trame (7 octets
     * d'en-tete + payload + frame-end). */
    uint8_t*     rx;
    size_t       rx_len;
    size_t       rx_cap;
    /* Borne du reliquat de `rx` consommable par le chemin de lecture SYNCHRONE :
     * octets d'une trame scindee par TCP laissee en tete de `rx` par un pump du
     * reacteur. Pose par amqp_rx_parse (= rx_len apres compactage). Les trames
     * async re-injectees pendant un appel sync sont AU-DELA de cette borne : le
     * drain sync ne les touche pas, elles restent pour le prochain AMQPAttend. */
    size_t       rx_drainable;
} OwlAmqpConn;

/* Delai d'ouverture du transport en millisecondes (defaut si TimeoutConnexion
 * n'est pas renseigne cote instance). */
#define AMQP_CONNECT_TIMEOUT_MS 10000
/* Types de trame AMQP 0-9-1 (spec OASIS). */
#define AMQP_FRAME_METHOD 1
#define AMQP_FRAME_END    0xCE
/* Channel de controle (methodes niveau connexion) et channel auto (methodes
 * niveau channel). */
#define AMQP_CH_CTRL 0
#define AMQP_CH_DATA 1
/* Borne de securite sur la taille d'une trame lue (anti-alloc abusive), quelle
 * que soit la valeur frame-max negociee. */
#define AMQP_READ_CAP (16u * 1024u * 1024u)
/* Classes / methodes du handshake. */
#define AMQP_CLASS_CONNECTION 10
#define AMQP_CLASS_CHANNEL    20
#define AMQP_M_CONN_START     10
#define AMQP_M_CONN_START_OK  11
#define AMQP_M_CONN_TUNE      30
#define AMQP_M_CONN_TUNE_OK   31
#define AMQP_M_CONN_OPEN      40
#define AMQP_M_CONN_OPEN_OK   41
#define AMQP_M_CONN_CLOSE     50
#define AMQP_M_CONN_CLOSE_OK  51
#define AMQP_M_CHAN_OPEN      10
#define AMQP_M_CHAN_OPEN_OK   11
/* Types de trame supplementaires (Lot B). */
#define AMQP_FRAME_HEADER 2
#define AMQP_FRAME_BODY   3
/* Classes / methodes Lot B. */
#define AMQP_CLASS_EXCHANGE   40
#define AMQP_CLASS_QUEUE      50
#define AMQP_CLASS_BASIC      60
#define AMQP_M_EXCH_DECLARE     10
#define AMQP_M_EXCH_DECLARE_OK  11
#define AMQP_M_QUEUE_DECLARE    10
#define AMQP_M_QUEUE_DECLARE_OK 11
#define AMQP_M_QUEUE_BIND       20
#define AMQP_M_QUEUE_BIND_OK    21
/* Teardown de topologie. */
#define AMQP_M_EXCH_DELETE      20
#define AMQP_M_EXCH_DELETE_OK   21
#define AMQP_M_QUEUE_PURGE      30
#define AMQP_M_QUEUE_PURGE_OK   31
#define AMQP_M_QUEUE_DELETE     40
#define AMQP_M_QUEUE_DELETE_OK  41
#define AMQP_M_QUEUE_UNBIND     50
#define AMQP_M_QUEUE_UNBIND_OK  51
#define AMQP_M_BASIC_PUBLISH    40
/* Classes / methodes Lot C (consommation). */
#define AMQP_M_BASIC_CONSUME    20
#define AMQP_M_BASIC_CONSUME_OK 21
#define AMQP_M_BASIC_DELIVER    60
#define AMQP_M_BASIC_ACK        80
/* Contre-pression (Basic.Qos) et rejet negatif (Basic.Nack, extension RabbitMQ
 * standard). Qos est synchrone (Qos-Ok), Nack fire-and-forget comme Ack. */
#define AMQP_M_BASIC_QOS        10
#define AMQP_M_BASIC_QOS_OK     11
#define AMQP_M_BASIC_NACK       120
/* Publisher confirms : Confirm.Select bascule le channel en mode
 * confirm, le broker accuse alors chaque Basic.Publish par un Basic.Ack (60/80,
 * mais dans le sens serveur->client) ou un Basic.Nack (60/120) portant le
 * numero de sequence de publication. */
#define AMQP_CLASS_CONFIRM       85
#define AMQP_M_CONFIRM_SELECT    10
#define AMQP_M_CONFIRM_SELECT_OK 11

static int amqp_send(OwlAmqpConn* c, const void* buf, size_t len) {
    return c->tls_active ? owl_tls_send(&c->transport, buf, len)
                         : owl_tcp_send(&c->transport.tcp, buf, len);
}

static bool amqp_recv_exact(OwlAmqpConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv_exact(&c->transport, buf, len)
                         : owl_tcp_recv_exact(&c->transport.tcp, buf, len);
}

/* Lecture jusqu'a len octets (non bloquante-friendly) pour la pompe : route
 * TLS/clair comme le send. >0 octets, 0 = pair ferme, <0 = erreur. */
static int amqp_recv(OwlAmqpConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv(&c->transport, buf, len)
                         : owl_tcp_recv(&c->transport.tcp, buf, len);
}

static void amqp_conn_free(OwlAmqpConn* c) {
    if (c == NULL) return;
    /* Relache les consommateurs : handlers E1 (racines GC) + copies de tags. */
    for (int i = 0; i < c->nb_consumers; i++) {
        owl_mq_desabonne(c->consumers[i].handle);
        free(c->consumers[i].tag);
    }
    /* Reacteur possede par la session : ne ferme aucun fd (contrat reacteur),
     * la socket est refermee juste apres. */
    if (c->reacteur != NULL) owl_reacteur_detruit(c->reacteur);
    free(c->deliv.body);
    free(c->rx);
    if (c->tls_active) owl_tls_close(&c->transport);
    else               owl_tcp_close(&c->transport.tcp);
    free(c);
}

static OwlAmqpConn* amqp_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return NULL;
    return (OwlAmqpConn*)(intptr_t)AS_INTEGER(v);
}

static void amqp_set_handle(ObjInstance* inst, OwlAmqpConn* c) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)c);
}

/* ============================================================
 * Encodage wire AMQP 0-9-1 (spec OASIS ; le wire n'est pas dans la doc canon).
 *
 * Tampon octet extensible pour assembler args de methode et trames.
 * ============================================================ */

typedef struct { uint8_t* data; size_t len, cap; } AmqpBuf;

static bool abuf_ensure(AmqpBuf* b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 128;
    while (ncap < b->len + extra) ncap *= 2;
    uint8_t* nb = (uint8_t*)realloc(b->data, ncap);
    if (nb == NULL) return false;
    b->data = nb;
    b->cap = ncap;
    return true;
}

static void abuf_u8(AmqpBuf* b, uint8_t v) {
    if (!abuf_ensure(b, 1)) return;
    b->data[b->len++] = v;
}
static void abuf_u16(AmqpBuf* b, uint16_t v) {
    if (!abuf_ensure(b, 2)) return;
    b->data[b->len++] = (uint8_t)(v >> 8);
    b->data[b->len++] = (uint8_t)(v & 0xFF);
}
static void abuf_u32(AmqpBuf* b, uint32_t v) {
    if (!abuf_ensure(b, 4)) return;
    b->data[b->len++] = (uint8_t)(v >> 24);
    b->data[b->len++] = (uint8_t)((v >> 16) & 0xFF);
    b->data[b->len++] = (uint8_t)((v >> 8) & 0xFF);
    b->data[b->len++] = (uint8_t)(v & 0xFF);
}
static void abuf_u64(AmqpBuf* b, uint64_t v) {
    if (!abuf_ensure(b, 8)) return;
    for (int i = 7; i >= 0; i--) b->data[b->len++] = (uint8_t)((v >> (i * 8)) & 0xFF);
}
static void abuf_bytes(AmqpBuf* b, const void* p, size_t n) {
    if (n == 0) return;
    if (!abuf_ensure(b, n)) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
/* shortstr : longueur sur 1 octet (max 255) puis les octets UTF-8. */
static void abuf_shortstr(AmqpBuf* b, const char* s) {
    size_t l = s ? strlen(s) : 0;
    if (l > 255) l = 255;
    abuf_u8(b, (uint8_t)l);
    abuf_bytes(b, s, l);
}
/* longstr : longueur sur 4 octets (big-endian) puis les octets (binaire). */
static void abuf_longstr(AmqpBuf* b, const void* s, size_t l) {
    abuf_u32(b, (uint32_t)l);
    abuf_bytes(b, s, l);
}

/* client-properties : field-table minimal, encodee [size:4BE][entrees]. Chaque
 * entree = shortstr(cle) + tag('S') + longstr(valeur). size = le bloc des
 * entrees seul (hors ses 4 octets de taille). */
static void amqp_build_client_properties(AmqpBuf* out) {
    AmqpBuf t = {0};
    static const char* const entries[][2] = {
        { "product",     "OWL" },
        { "version",     "0.1" },
        { "platform",    "OWL runtime" },
        { "information", "OWL AMQP client" },
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        abuf_shortstr(&t, entries[i][0]);
        abuf_u8(&t, 'S');
        abuf_longstr(&t, entries[i][1], strlen(entries[i][1]));
    }
    abuf_u32(out, (uint32_t)t.len);
    abuf_bytes(out, t.data, t.len);
    free(t.data);
}

/* Emet une trame method : [type:1][ch:2BE][size:4BE][class:2][method:2][args]
 * [0xCE]. size = classe + methode + args (le payload seul). */
static bool amqp_send_method(OwlAmqpConn* c, uint16_t channel, uint16_t cls,
                             uint16_t meth, const AmqpBuf* args) {
    size_t args_len = args ? args->len : 0;
    size_t payload_len = 4 + args_len;
    AmqpBuf f = {0};
    abuf_u8(&f, AMQP_FRAME_METHOD);
    abuf_u16(&f, channel);
    abuf_u32(&f, (uint32_t)payload_len);
    abuf_u16(&f, cls);
    abuf_u16(&f, meth);
    if (args_len) abuf_bytes(&f, args->data, args_len);
    abuf_u8(&f, AMQP_FRAME_END);
    bool ok = (f.data != NULL) && (amqp_send(c, f.data, f.len) == (int)f.len);
    free(f.data);
    return ok;
}

/* ============================================================
 * Lecture des trames serveur (synchrone).
 * ============================================================ */

typedef struct {
    uint8_t   type;
    uint16_t  channel;
    uint32_t  size;      /* taille du payload (hors frame-end) */
    uint8_t*  payload;   /* malloc(size), a liberer par l'appelant */
} AmqpFrame;

/* Lit exactement `len` octets pour le chemin de lecture SYNCHRONE. Puise D'ABORD
 * dans le reliquat drainable de `rx` (octets d'une trame scindee par TCP laissee
 * en tete par un pump precedent du reacteur), puis complete depuis la socket.
 * Invariant : rx est la source prioritaire, la socket le complement. Sans ce
 * drainage, un recv direct socket lirait les octets de completion de la trame
 * partielle bufferisee comme un nouvel en-tete de 7 octets -> desynchronisation
 * des limites de trame entre chemin pompe et chemin sync. Seul `rx_drainable`
 * (reliquat pose par le pump) est consommable ici : les trames async re-injectees
 * par amqp_expect PENDANT l'appel sync en cours sont au-dela de cette borne, donc
 * jamais re-lues par la boucle courante -> dispatch differe au prochain
 * AMQPAttend (zero reentrance, zero perte, cf. ). */
static bool amqp_recv_exact_sync(OwlAmqpConn* c, void* buf, size_t len) {
    uint8_t* out = (uint8_t*)buf;
    if (c->rx_drainable > 0) {
        size_t taken = c->rx_drainable < len ? c->rx_drainable : len;
        memcpy(out, c->rx, taken);
        c->rx_len       -= taken;
        c->rx_drainable -= taken;
        if (c->rx_len > 0) memmove(c->rx, c->rx + taken, c->rx_len);
        out += taken;
        len -= taken;
    }
    if (len == 0) return true;
    return amqp_recv_exact(c, out, len);
}

/* Lit une trame complete : 7 octets d'en-tete puis size+1 octets (payload +
 * frame-end 0xCE, verifie). Renvoie false si transport coupe, taille aberrante
 * ou frame-end absent. Le payload est alloue (a liberer sur succes). Passe par
 * amqp_recv_exact_sync : draine le reliquat rx avant la socket (voir ci-dessus). */
static bool amqp_read_frame(OwlAmqpConn* c, AmqpFrame* fr) {
    uint8_t hdr[7];
    if (!amqp_recv_exact_sync(c, hdr, 7)) return false;
    fr->type    = hdr[0];
    fr->channel = (uint16_t)((hdr[1] << 8) | hdr[2]);
    fr->size    = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[4] << 16) |
                  ((uint32_t)hdr[5] << 8) | (uint32_t)hdr[6];
    if (fr->size > AMQP_READ_CAP) return false;
    uint8_t* buf = (uint8_t*)malloc((size_t)fr->size + 1);
    if (buf == NULL) return false;
    if (!amqp_recv_exact_sync(c, buf, (size_t)fr->size + 1)) { free(buf); return false; }
    if (buf[fr->size] != AMQP_FRAME_END) { free(buf); return false; }
    fr->payload = buf;
    return true;
}

/* Lit une trame method et en extrait class/method. false si la trame n'est pas
 * une method (payload libere dans ce cas). Sur succes, *fr->payload reste a
 * liberer par l'appelant. */
static bool amqp_read_method(OwlAmqpConn* c, uint16_t* cls, uint16_t* meth,
                             AmqpFrame* fr) {
    if (!amqp_read_frame(c, fr)) return false;
    if (fr->type != AMQP_FRAME_METHOD || fr->size < 4) {
        free(fr->payload);
        fr->payload = NULL;
        return false;
    }
    *cls  = (uint16_t)((fr->payload[0] << 8) | fr->payload[1]);
    *meth = (uint16_t)((fr->payload[2] << 8) | fr->payload[3]);
    return true;
}

/* Buffer FIFO du reacteur (defini plus bas) : accumule les octets bruts lus sur
 * la socket, parses au prochain pump. amqp_expect y re-injecte les trames async
 * entrelacees pour traitement differe. */
static void amqp_rx_append(OwlAmqpConn* c, const uint8_t* data, size_t len);

/* Re-serialise une trame lue et la re-injecte dans le buffer `rx` du reacteur :
 * son traitement (dispatch livraison) est differe au prochain AMQPAttend, sans
 * callback reentrant pendant l'appel synchrone en cours. Le payload lu inclut
 * deja le frame-end 0xCE (verifie par amqp_read_frame). En-tete AMQP :
 * [type u8][channel u16 BE][size u32 BE]. */
static void amqp_reinject_frame(OwlAmqpConn* c, const AmqpFrame* fr) {
    uint8_t hdr[7];
    hdr[0] = fr->type;
    hdr[1] = (uint8_t)(fr->channel >> 8);
    hdr[2] = (uint8_t)(fr->channel & 0xFF);
    hdr[3] = (uint8_t)(fr->size >> 24);
    hdr[4] = (uint8_t)(fr->size >> 16);
    hdr[5] = (uint8_t)(fr->size >> 8);
    hdr[6] = (uint8_t)(fr->size & 0xFF);
    amqp_rx_append(c, hdr, 7);
    amqp_rx_append(c, fr->payload, (size_t)fr->size + 1);   /* payload + 0xCE */
}

/* Attend la prochaine method sync et verifie qu'elle correspond a {want_cls,
 * want_meth}. En AMQP 0-9-1, le broker entrelace des trames asynchrones (triplet
 * Basic.Deliver method+header+body, heartbeats) sur le meme channel des qu'un
 * consommateur est actif : elles peuvent arriver avant une reponse sync attendue
 * (Declare-Ok, Bind-Ok…). Ces trames async sont re-injectees telles quelles dans
 * `rx` (traitement differe au prochain AMQPAttend, zero perte, zero reentrance)
 * et la boucle continue d'attendre la reponse sync. Si le serveur envoie
 * Connection.Close (auth refusee, vhost inconnu, erreur protocole), extrait code
 * + texte et renseigne `detail`. Renvoie false + `detail` renseigne sur toute
 * divergence sync ou coupure de transport (amqp_read_frame -> false). */
static bool amqp_expect(OwlAmqpConn* c, uint16_t want_cls, uint16_t want_meth,
                        char* detail, size_t detail_sz) {
    for (;;) {
        AmqpFrame fr;
        if (!amqp_read_frame(c, &fr)) {
            snprintf(detail, detail_sz,
                     "trame inattendue ou connexion coupée pendant le handshake");
            return false;
        }

        /* Trame non-method (content-header 2, content-body 3, heartbeat 8) :
         * async, appartient a une livraison en cours -> re-injecter et attendre
         * encore la reponse sync. */
        if (fr.type != AMQP_FRAME_METHOD || fr.size < 4) {
            amqp_reinject_frame(c, &fr);
            free(fr.payload);
            continue;
        }

        uint16_t cls  = (uint16_t)((fr.payload[0] << 8) | fr.payload[1]);
        uint16_t meth = (uint16_t)((fr.payload[2] << 8) | fr.payload[3]);

        if (cls == want_cls && meth == want_meth) {
            free(fr.payload);
            return true;
        }
        if (cls == AMQP_CLASS_CONNECTION && meth == AMQP_M_CONN_CLOSE) {
            /* args : reply-code(u16), reply-text(shortstr), class-id(u16),
             * method-id(u16). */
            const uint8_t* p   = fr.payload + 4;
            const uint8_t* end = fr.payload + fr.size;
            uint16_t code = 0;
            char text[256];
            text[0] = '\0';
            if (p + 2 <= end) { code = (uint16_t)((p[0] << 8) | p[1]); p += 2; }
            if (p + 1 <= end) {
                uint8_t tl = *p++;
                if (p + tl <= end) {
                    size_t n = tl < sizeof(text) - 1 ? tl : sizeof(text) - 1;
                    memcpy(text, p, n);
                    text[n] = '\0';
                }
            }
            snprintf(detail, detail_sz,
                     "serveur a fermé la connexion (code %u : %s)", code, text);
            free(fr.payload);
            return false;
        }
        /* Basic.Deliver entrelace (consommateur actif) : async -> re-injecter et
         * attendre encore la reponse sync. */
        if (cls == AMQP_CLASS_BASIC && meth == AMQP_M_BASIC_DELIVER) {
            amqp_reinject_frame(c, &fr);
            free(fr.payload);
            continue;
        }
        /* Autre method (Channel.Close 406 PRECONDITION_FAILED, reply inattendu…) :
         * peut-etre une vraie erreur -> ne PAS re-injecter, remonter. */
        snprintf(detail, detail_sz,
                 "méthode AMQP inattendue (classe %u méthode %u)", cls, meth);
        free(fr.payload);
        return false;
    }
}

/* Attend l'accuse du broker apres un Basic.Publish en mode publisher-confirms :
 * une method entrante Basic.Ack (60/80, ici emise par le SERVEUR) -> *out_ack
 * Vrai (message confirme) ; Basic.Nack (60/120) -> *out_ack Faux (rejete).
 * Comme AMQPPublie publie 1 message par 1 en synchrone, le prochain accuse porte
 * forcement notre message : pas besoin de suivre le numero de sequence (le bit
 * `multiple` d'un Ack groupe couvre au plus notre unique message en vol).
 * Reutilise la robustesse anti-entrelacement d'amqp_expect : les trames async
 * (triplet Basic.Deliver, content-header/body, heartbeats) sont re-injectees
 * dans `rx` pour traitement differe au prochain AMQPAttend. Un Connection.Close
 * serveur -> false + `detail`. Renvoie false + `detail` sur coupure transport ou
 * method inattendue. */
static bool amqp_await_confirm(OwlAmqpConn* c, bool* out_ack,
                              char* detail, size_t detail_sz) {
    for (;;) {
        AmqpFrame fr;
        if (!amqp_read_frame(c, &fr)) {
            snprintf(detail, detail_sz,
                     "connexion coupée en attente de l'accusé du broker");
            return false;
        }

        /* Trame non-method (content-header, body, heartbeat) : async -> re-injecter. */
        if (fr.type != AMQP_FRAME_METHOD || fr.size < 4) {
            amqp_reinject_frame(c, &fr);
            free(fr.payload);
            continue;
        }

        uint16_t cls  = (uint16_t)((fr.payload[0] << 8) | fr.payload[1]);
        uint16_t meth = (uint16_t)((fr.payload[2] << 8) | fr.payload[3]);

        if (cls == AMQP_CLASS_BASIC && meth == AMQP_M_BASIC_ACK) {
            *out_ack = true;
            free(fr.payload);
            return true;
        }
        if (cls == AMQP_CLASS_BASIC && meth == AMQP_M_BASIC_NACK) {
            *out_ack = false;
            free(fr.payload);
            return true;
        }
        if (cls == AMQP_CLASS_CONNECTION && meth == AMQP_M_CONN_CLOSE) {
            const uint8_t* p   = fr.payload + 4;
            const uint8_t* end = fr.payload + fr.size;
            uint16_t code = 0;
            char text[256];
            text[0] = '\0';
            if (p + 2 <= end) { code = (uint16_t)((p[0] << 8) | p[1]); p += 2; }
            if (p + 1 <= end) {
                uint8_t tl = *p++;
                if (p + tl <= end) {
                    size_t n = tl < sizeof(text) - 1 ? tl : sizeof(text) - 1;
                    memcpy(text, p, n);
                    text[n] = '\0';
                }
            }
            snprintf(detail, detail_sz,
                     "serveur a fermé la connexion (code %u : %s)", code, text);
            free(fr.payload);
            return false;
        }
        /* Basic.Deliver entrelace (consommateur actif) : async -> re-injecter. */
        if (cls == AMQP_CLASS_BASIC && meth == AMQP_M_BASIC_DELIVER) {
            amqp_reinject_frame(c, &fr);
            free(fr.payload);
            continue;
        }
        snprintf(detail, detail_sz,
                 "méthode AMQP inattendue en attente de l'accusé (classe %u méthode %u)",
                 cls, meth);
        free(fr.payload);
        return false;
    }
}

/* ============================================================
 * Natives — Lot A (handshake connexion + channel auto cache).
 * ============================================================ */

static Value fn_amqp_connecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_amqp_connection(args[0], "AMQPConnecte");
    if (inst == NULL) return BOOL_VAL(false);

    /* Reconnexion : libere une connexion vivante residuelle (idempotence). */
    OwlAmqpConn* old = amqp_handle(inst);
    if (old != NULL) { amqp_conn_free(old); amqp_set_handle(inst, NULL); }

    const char* adresse = amqp_str(inst, "Adresse");
    const char* user    = amqp_str(inst, "Utilisateur");
    const char* pass    = amqp_str(inst, "MotDePasse");
    const char* vhost   = amqp_str(inst, "HoteVirtuel");
    int option  = amqp_int(inst, "Option");
    int timeout = amqp_int(inst, "TimeoutConnexion");
    bool ssl    = (option & OWL_OPTION_SSL) != 0;

    /* Port : explicite si renseigne, sinon defaut selon le chiffrement. SSL
     * avec le port clair par defaut (5672) -> bascule 5671. */
    int port = amqp_int(inst, "Port");
    if (port <= 0) {
        port = ssl ? AMQP_PORT_SSL : AMQP_PORT_CLEAR;
    } else if (ssl && port == AMQP_PORT_CLEAR) {
        port = AMQP_PORT_SSL;
    }

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", adresse, port);

    OwlAmqpConn* c = (OwlAmqpConn*)calloc(1, sizeof(OwlAmqpConn));
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                              "native", "AMQPConnecte", "cible", cible,
                              "detail", "mémoire insuffisante", NULL);
        return BOOL_VAL(false);
    }
    c->channel = AMQP_CH_DATA;

    owl_tcp_global_init();
    owl_tls_init(&c->transport);
    int tmo = timeout > 0 ? timeout * 1000 : AMQP_CONNECT_TIMEOUT_MS;

    if (ssl) {
        /* CA privee optionnelle : si CertificatCA est renseigne, il devient la
         * racine de confiance ; vide -> NULL (bundle Mozilla). Verification
         * hostname+chaine conservee (VERIFY_FULL) dans les deux cas. La chaine
         * du champ d'instance vit pendant le connect (PEM parse a la volee). */
        const char* ca = amqp_str(inst, "CertificatCA");
        const char* extra_ca = (ca && ca[0] != '\0') ? ca : NULL;
        if (!owl_tls_connect(&c->transport, adresse, port, OWL_TLS_VERIFY_FULL,
                             tmo, extra_ca)) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", owl_tls_last_error(&c->transport), NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
        c->tls_active = true;
    } else {
        if (!owl_tcp_connect(&c->transport.tcp, adresse, port, tmo)) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", owl_tcp_last_error(&c->transport.tcp), NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    char detail[300];

    /* 1. Protocol header (hors trame) : "AMQP" 0x00 0x00 0x09 0x01. */
    static const uint8_t proto_hdr[8] = { 'A','M','Q','P', 0x00, 0x00, 0x09, 0x01 };
    if (amqp_send(c, proto_hdr, sizeof(proto_hdr)) != (int)sizeof(proto_hdr)) {
        owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                              "native", "AMQPConnecte", "cible", cible,
                              "detail", "émission de l'en-tête protocole impossible", NULL);
        amqp_conn_free(c);
        return BOOL_VAL(false);
    }

    /* 2. S -> Connection.Start (10/10). */
    if (!amqp_expect(c, AMQP_CLASS_CONNECTION, AMQP_M_CONN_START, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                              "native", "AMQPConnecte", "cible", cible,
                              "detail", detail, NULL);
        amqp_conn_free(c);
        return BOOL_VAL(false);
    }

    /* 3. C -> Connection.Start-Ok (10/11) : client-properties, PLAIN,
     * response \0user\0pass, locale en_US. */
    {
        AmqpBuf resp = {0};
        abuf_u8(&resp, 0);  abuf_bytes(&resp, user, strlen(user));
        abuf_u8(&resp, 0);  abuf_bytes(&resp, pass, strlen(pass));

        AmqpBuf a = {0};
        amqp_build_client_properties(&a);
        abuf_shortstr(&a, "PLAIN");
        abuf_longstr(&a, resp.data ? resp.data : (const uint8_t*)"", resp.len);
        abuf_shortstr(&a, "en_US");

        bool ok = amqp_send_method(c, AMQP_CH_CTRL, AMQP_CLASS_CONNECTION,
                                   AMQP_M_CONN_START_OK, &a);
        free(a.data);
        free(resp.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "émission de Start-Ok impossible", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* 4. S -> Connection.Tune (10/30) : channel-max(u16), frame-max(u32),
     * heartbeat(u16). On lit la trame et on extrait frame-max serveur. */
    uint32_t server_frame_max = 0;
    {
        AmqpFrame fr;
        uint16_t cls = 0, meth = 0;
        if (!amqp_read_method(c, &cls, &meth, &fr)) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "Tune attendu, trame inattendue ou connexion coupée", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
        if (cls == AMQP_CLASS_CONNECTION && meth == AMQP_M_CONN_CLOSE) {
            const uint8_t* p   = fr.payload + 4;
            const uint8_t* end = fr.payload + fr.size;
            uint16_t code = 0;
            if (p + 2 <= end) code = (uint16_t)((p[0] << 8) | p[1]);
            snprintf(detail, sizeof(detail),
                     "serveur a fermé la connexion pendant la négociation (code %u)", code);
            free(fr.payload);
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", detail, NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
        if (cls != AMQP_CLASS_CONNECTION || meth != AMQP_M_CONN_TUNE || fr.size < 4 + 8) {
            free(fr.payload);
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "Connection.Tune malformé ou inattendu", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
        const uint8_t* p = fr.payload + 4;  /* apres class+method */
        /* channel-max(u16) ignore ici, frame-max(u32), heartbeat(u16). */
        server_frame_max = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) |
                           ((uint32_t)p[4] << 8) | (uint32_t)p[5];
        free(fr.payload);
    }
    c->frame_max = server_frame_max;

    /* 5. C -> Connection.Tune-Ok (10/31) : channel-max cappe a 1, frame-max =
     * serveur, heartbeat = 0 (desactive au Lot A). */
    {
        AmqpBuf a = {0};
        abuf_u16(&a, 1);                 /* channel-max cappe a 1 */
        abuf_u32(&a, server_frame_max);  /* frame-max negocie = serveur */
        abuf_u16(&a, 0);                 /* heartbeat desactive */
        bool ok = amqp_send_method(c, AMQP_CH_CTRL, AMQP_CLASS_CONNECTION,
                                   AMQP_M_CONN_TUNE_OK, &a);
        free(a.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "émission de Tune-Ok impossible", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* 6. C -> Connection.Open (10/40) : vhost, reserved shortstr "", reserved
     * bit 0. */
    {
        AmqpBuf a = {0};
        abuf_shortstr(&a, vhost && vhost[0] ? vhost : "/");
        abuf_shortstr(&a, "");   /* reserved-1 (capabilities, deprecated) */
        abuf_u8(&a, 0);          /* reserved-2 (bit unique, LSB) */
        bool ok = amqp_send_method(c, AMQP_CH_CTRL, AMQP_CLASS_CONNECTION,
                                   AMQP_M_CONN_OPEN, &a);
        free(a.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "émission de Connection.Open impossible", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* 7. S -> Connection.Open-Ok (10/41). */
    if (!amqp_expect(c, AMQP_CLASS_CONNECTION, AMQP_M_CONN_OPEN_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                              "native", "AMQPConnecte", "cible", cible,
                              "detail", detail, NULL);
        amqp_conn_free(c);
        return BOOL_VAL(false);
    }

    /* 8. C -> Channel.Open (20/10) sur le channel auto : reserved shortstr "". */
    {
        AmqpBuf a = {0};
        abuf_shortstr(&a, "");   /* reserved-1 */
        bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_CHANNEL,
                                   AMQP_M_CHAN_OPEN, &a);
        free(a.data);
        if (!ok) {
            owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                                  "native", "AMQPConnecte", "cible", cible,
                                  "detail", "émission de Channel.Open impossible", NULL);
            amqp_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* 9. S -> Channel.Open-Ok (20/11). */
    if (!amqp_expect(c, AMQP_CLASS_CHANNEL, AMQP_M_CHAN_OPEN_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_CONNECT_FAILURE, "AMQPConnecte",
                              "native", "AMQPConnecte", "cible", cible,
                              "detail", detail, NULL);
        amqp_conn_free(c);
        return BOOL_VAL(false);
    }

    c->connected = true;
    c->session = inst;          /* arg 1 des livraisons ; instance rootee OWL */
    amqp_set_handle(inst, c);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_amqp_deconnecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_amqp_connection(args[0], "AMQPDéconnecte");
    if (inst == NULL) return NIL_VAL;

    OwlAmqpConn* c = amqp_handle(inst);
    if (c != NULL) {
        if (c->connected) {
            /* Connection.Close (10/50) : reply-code 200, reply-text "",
             * class-id 0, method-id 0. Puis on attend Close-Ok (best-effort). */
            AmqpBuf a = {0};
            abuf_u16(&a, 200);
            abuf_shortstr(&a, "");
            abuf_u16(&a, 0);
            abuf_u16(&a, 0);
            if (amqp_send_method(c, AMQP_CH_CTRL, AMQP_CLASS_CONNECTION,
                                 AMQP_M_CONN_CLOSE, &a)) {
                char detail[128];
                (void)amqp_expect(c, AMQP_CLASS_CONNECTION, AMQP_M_CONN_CLOSE_OK,
                                  detail, sizeof(detail));
            }
            free(a.data);
        }
        amqp_conn_free(c);
        amqp_set_handle(inst, NULL);
    }
    return NIL_VAL;
}

/* ============================================================
 * Natives — Lot B (déclaration queue/échange, binding, publication).
 * ============================================================ */

/* Résout l'instance + récupère une connexion vivante. Émet une erreur et
 * renvoie NULL si l'arg n'est pas une amqpConnection ouverte. */
static OwlAmqpConn* amqp_live(Value v, const char* fn, ObjInstance** out_inst) {
    ObjInstance* inst = resolve_amqp_connection(v, fn);
    if (inst == NULL) return NULL;
    OwlAmqpConn* c = amqp_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, fn,
                              "native", fn, "detail", "connexion AMQP non ouverte", NULL);
        return NULL;
    }
    if (out_inst) *out_inst = inst;
    return c;
}

/* Booléen optionnel (faux si absent ou d'un autre type). */
static bool amqp_opt_bool(int argCount, Value* args, int idx) {
    if (idx >= argCount || !IS_BOOL(args[idx])) return false;
    return AS_BOOL(args[idx]);
}

/* Chaîne d'un argument (vide si absent ou d'un autre type). */
static const char* amqp_arg_str(int argCount, Value* args, int idx) {
    if (idx >= argCount || !IS_STRING(args[idx])) return "";
    return AS_CSTRING(args[idx]);
}

/* Nom wire d'un type d'échange depuis la valeur d'enum amqpTypeÉchange. */
static const char* amqp_exchange_wire(int64_t v) {
    switch (v) {
        case OWL_AMQP_EXCH_FANOUT:  return "fanout";
        case OWL_AMQP_EXCH_TOPIC:   return "topic";
        case OWL_AMQP_EXCH_HEADERS: return "headers";
        default:                    return "direct";
    }
}

static Value fn_amqp_declare_queue(int argCount, Value* args) {
    /* Verrou de rupture : si le 6e arg (arguments étendus) est présent, il DOIT
     * être un tableau associatif dont chaque valeur est chaîne, entier ou
     * booléen. Validé AVANT tout accès réseau (amqp_live) — sinon une connexion
     * morte masquerait le vrai code d'erreur (type d'argument) derrière un
     * « connexion non ouverte ». */
    bool has_args = (argCount > 5 && !IS_NIL(args[5]));
    if (has_args) {
        if (!IS_ASSOC_ARRAY(args[5])) {
            owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPDéclareQueue",
                                  "native", "AMQPDéclareQueue", "arg", "6",
                                  "expected", "tableau associatif",
                                  "actual", "un autre type", NULL);
            return OBJ_VAL(obj_copy_string("", 0));
        }
        ObjAssocArray* aa = AS_ASSOC_ARRAY(args[5]);
        for (int i = 0; i < aa->count; i++) {
            Value v = aa->values[i];
            if (!IS_STRING(v) && !IS_INTEGER(v) && !IS_BOOL(v)) {
                owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPDéclareQueue",
                                      "native", "AMQPDéclareQueue", "arg", "6",
                                      "expected", "chaîne, entier ou booléen",
                                      "actual", "un autre type", NULL);
                return OBJ_VAL(obj_copy_string("", 0));
            }
        }
    }

    ObjInstance* inst = NULL;
    OwlAmqpConn* c = amqp_live(args[0], "AMQPDéclareQueue", &inst);
    if (c == NULL) return OBJ_VAL(obj_copy_string("", 0));

    const char* nom  = amqp_arg_str(argCount, args, 1);
    bool durable     = amqp_opt_bool(argCount, args, 2);
    bool exclusive   = amqp_opt_bool(argCount, args, 3);
    bool autodel     = amqp_opt_bool(argCount, args, 4);

    /* Queue.Declare (50/10) : reserved-1(u16 0), queue(shortstr),
     * bits[passive,durable,exclusive,auto-delete,no-wait] (LSB-first),
     * arguments(field-table). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, nom);
    uint8_t bits = 0;
    if (durable)   bits |= 0x02;
    if (exclusive) bits |= 0x04;
    if (autodel)   bits |= 0x08;
    abuf_u8(&a, bits);
    /* Field-table des x-arguments : corps encodé dans un tampon temporaire, puis
     * émis en [u32 taille du corps][corps]. Chaque entrée = shortstr(clé) +
     * octet type + valeur : 'S' long-string, 'l' long-long int64, 't' booléen. */
    if (has_args) {
        ObjAssocArray* aa = AS_ASSOC_ARRAY(args[5]);
        AmqpBuf t = {0};
        for (int i = 0; i < aa->count; i++) {
            ObjString* k = aa->keys[i];
            Value      v = aa->values[i];
            abuf_shortstr(&t, k ? k->chars : "");
            if (IS_STRING(v)) {
                ObjString* s = AS_STRING(v);
                abuf_u8(&t, 'S');
                abuf_longstr(&t, s->chars, (size_t)s->length);
            } else if (IS_BOOL(v)) {
                abuf_u8(&t, 't');
                abuf_u8(&t, AS_BOOL(v) ? 1 : 0);
            } else { /* IS_INTEGER (garanti par la validation en amont) */
                abuf_u8(&t, 'l');
                abuf_u64(&t, (uint64_t)AS_INTEGER(v));
            }
        }
        abuf_u32(&a, (uint32_t)t.len);
        abuf_bytes(&a, t.data, t.len);
        free(t.data);
    } else {
        abuf_u32(&a, 0);
    }
    bool sent = amqp_send_method(c, c->channel, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_DECLARE, &a);
    free(a.data);
    if (!sent) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDéclareQueue",
                              "native", "AMQPDéclareQueue",
                              "detail", "émission de Queue.Declare impossible", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* Declare-Ok (50/11) : queue(shortstr), message-count(u32), consumer-count(u32). */
    AmqpFrame fr;
    uint16_t cls = 0, meth = 0;
    if (!amqp_read_method(c, &cls, &meth, &fr)) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDéclareQueue",
                              "native", "AMQPDéclareQueue",
                              "detail", "Queue.Declare-Ok attendu, trame inattendue ou connexion coupée", NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }
    if (cls != AMQP_CLASS_QUEUE || meth != AMQP_M_QUEUE_DECLARE_OK) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "réponse inattendue à Queue.Declare (classe %u méthode %u)", cls, meth);
        free(fr.payload);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDéclareQueue",
                              "native", "AMQPDéclareQueue", "detail", detail, NULL);
        return OBJ_VAL(obj_copy_string("", 0));
    }

    const uint8_t* p   = fr.payload + 4;
    const uint8_t* end = fr.payload + fr.size;
    char qname[256]; qname[0] = '\0';
    uint32_t msgcount = 0;
    if (p < end) {
        uint8_t ql = *p++;
        if (p + ql <= end) {
            size_t n = ql < sizeof(qname) - 1 ? ql : sizeof(qname) - 1;
            memcpy(qname, p, n);
            qname[n] = '\0';
            p += ql;
        }
    }
    if (p + 4 <= end) {
        msgcount = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                   ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    free(fr.payload);
    inst_field_set_int(inst, "NbMessages", (int64_t)msgcount);
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(qname, (int)strlen(qname)));
}

static Value fn_amqp_declare_echange(int argCount, Value* args) {
    OwlAmqpConn* c = amqp_live(args[0], "AMQPDéclareÉchange", NULL);
    if (c == NULL) return BOOL_VAL(false);

    const char* nom  = amqp_arg_str(argCount, args, 1);
    int64_t typeVal  = (argCount >= 3 && IS_INTEGER(args[2]))
                       ? AS_INTEGER(args[2]) : OWL_AMQP_EXCH_DIRECT;
    const char* type = amqp_exchange_wire(typeVal);
    bool durable     = amqp_opt_bool(argCount, args, 3);

    /* Exchange.Declare (40/10) : reserved-1, exchange(shortstr), type(shortstr),
     * bits[passive,durable,auto-delete,internal,no-wait], arguments(table). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, nom);
    abuf_shortstr(&a, type);
    uint8_t bits = 0;
    if (durable) bits |= 0x02;
    abuf_u8(&a, bits);
    abuf_u32(&a, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_EXCHANGE, AMQP_M_EXCH_DECLARE, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDéclareÉchange",
                              "native", "AMQPDéclareÉchange",
                              "detail", "émission de Exchange.Declare impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_EXCHANGE, AMQP_M_EXCH_DECLARE_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDéclareÉchange",
                              "native", "AMQPDéclareÉchange", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_amqp_lie(int argCount, Value* args) {
    OwlAmqpConn* c = amqp_live(args[0], "AMQPLie", NULL);
    if (c == NULL) return BOOL_VAL(false);

    const char* queue   = amqp_arg_str(argCount, args, 1);
    const char* echange = amqp_arg_str(argCount, args, 2);
    const char* cle     = amqp_arg_str(argCount, args, 3);

    /* Queue.Bind (50/20) : reserved-1, queue, exchange, routing-key, no-wait(bit),
     * arguments(field-table vide). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, queue);
    abuf_shortstr(&a, echange);
    abuf_shortstr(&a, cle);
    abuf_u8(&a, 0);
    abuf_u32(&a, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_BIND, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPLie",
                              "native", "AMQPLie",
                              "detail", "émission de Queue.Bind impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_BIND_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPLie",
                              "native", "AMQPLie", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Natives de teardown de topologie (supprime/vide queue, délie,
 * supprime échange). Toutes synchrones : émission de la méthode + attente de
 * l'Ok. Le nom/queue/échange/clé doit être une chaîne, validé AVANT tout accès
 * réseau (amqp_live) — sinon une connexion morte masquerait le vrai code
 * d'erreur (type d'argument) derrière un « connexion non ouverte ».
 * ============================================================ */

/* Valide qu'un argument attendu chaîne en est bien un. Sinon émet
 * E_NATIVE_ARG_TYPE_MISMATCH (numéro d'arg 1-based) et renvoie faux. */
static bool amqp_require_str(Value* args, int idx, const char* fn) {
    if (IS_STRING(args[idx])) return true;
    char num[8];
    snprintf(num, sizeof(num), "%d", idx + 1);
    owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                          "native", fn, "arg", num,
                          "expected", "chaîne", "actual", "un autre type", NULL);
    return false;
}

static Value fn_amqp_supprime_queue(int argCount, Value* args) {
    (void)argCount;
    if (!amqp_require_str(args, 1, "AMQPSupprimeQueue")) return BOOL_VAL(false);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPSupprimeQueue", NULL);
    if (c == NULL) return BOOL_VAL(false);

    const char* nom = AS_CSTRING(args[1]);

    /* Queue.Delete (50/40) : reserved-1(u16 0), queue(shortstr),
     * bits[if-unused, if-empty, no-wait] = 0 (suppression inconditionnelle). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, nom);
    abuf_u8(&a, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_DELETE, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPSupprimeQueue",
                              "native", "AMQPSupprimeQueue",
                              "detail", "émission de Queue.Delete impossible", NULL);
        return BOOL_VAL(false);
    }
    /* Delete-Ok (50/41) : porte un message-count(u32) ignoré (retour booléen). */
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_DELETE_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPSupprimeQueue",
                              "native", "AMQPSupprimeQueue", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_amqp_purge_queue(int argCount, Value* args) {
    (void)argCount;
    if (!amqp_require_str(args, 1, "AMQPPurgeQueue")) return INTEGER_VAL(-1);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPPurgeQueue", NULL);
    if (c == NULL) return INTEGER_VAL(-1);

    const char* nom = AS_CSTRING(args[1]);

    /* Queue.Purge (50/30) : reserved-1(u16 0), queue(shortstr), no-wait(bit 0). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, nom);
    abuf_u8(&a, 0);
    bool sent = amqp_send_method(c, c->channel, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_PURGE, &a);
    free(a.data);
    if (!sent) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPPurgeQueue",
                              "native", "AMQPPurgeQueue",
                              "detail", "émission de Queue.Purge impossible", NULL);
        return INTEGER_VAL(-1);
    }

    /* Purge-Ok (50/31) : message-count(u32 BE) = nombre de messages retirés. */
    AmqpFrame fr;
    uint16_t cls = 0, meth = 0;
    if (!amqp_read_method(c, &cls, &meth, &fr)) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPPurgeQueue",
                              "native", "AMQPPurgeQueue",
                              "detail", "Queue.Purge-Ok attendu, trame inattendue ou connexion coupée", NULL);
        return INTEGER_VAL(-1);
    }
    if (cls != AMQP_CLASS_QUEUE || meth != AMQP_M_QUEUE_PURGE_OK) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "réponse inattendue à Queue.Purge (classe %u méthode %u)", cls, meth);
        free(fr.payload);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPPurgeQueue",
                              "native", "AMQPPurgeQueue", "detail", detail, NULL);
        return INTEGER_VAL(-1);
    }
    const uint8_t* p   = fr.payload + 4;
    const uint8_t* end = fr.payload + fr.size;
    uint32_t purged = 0;
    if (p + 4 <= end) {
        purged = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }
    free(fr.payload);
    owl_clear_error();
    return INTEGER_VAL((int64_t)purged);
}

static Value fn_amqp_delie(int argCount, Value* args) {
    (void)argCount;
    if (!amqp_require_str(args, 1, "AMQPDélie")) return BOOL_VAL(false);
    if (!amqp_require_str(args, 2, "AMQPDélie")) return BOOL_VAL(false);
    if (!amqp_require_str(args, 3, "AMQPDélie")) return BOOL_VAL(false);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPDélie", NULL);
    if (c == NULL) return BOOL_VAL(false);

    const char* queue   = AS_CSTRING(args[1]);
    const char* echange = AS_CSTRING(args[2]);
    const char* cle     = AS_CSTRING(args[3]);

    /* Queue.Unbind (50/50) : reserved-1, queue, exchange, routing-key,
     * arguments(field-table vide). Pas de bit no-wait (contrairement à Bind). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, queue);
    abuf_shortstr(&a, echange);
    abuf_shortstr(&a, cle);
    abuf_u32(&a, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_UNBIND, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDélie",
                              "native", "AMQPDélie",
                              "detail", "émission de Queue.Unbind impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_QUEUE, AMQP_M_QUEUE_UNBIND_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPDélie",
                              "native", "AMQPDélie", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_amqp_supprime_echange(int argCount, Value* args) {
    (void)argCount;
    if (!amqp_require_str(args, 1, "AMQPSupprimeÉchange")) return BOOL_VAL(false);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPSupprimeÉchange", NULL);
    if (c == NULL) return BOOL_VAL(false);

    const char* nom = AS_CSTRING(args[1]);

    /* Exchange.Delete (40/20) : reserved-1(u16 0), exchange(shortstr),
     * bits[if-unused, no-wait] = 0 (suppression inconditionnelle). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, nom);
    abuf_u8(&a, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_EXCHANGE, AMQP_M_EXCH_DELETE, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPSupprimeÉchange",
                              "native", "AMQPSupprimeÉchange",
                              "detail", "émission de Exchange.Delete impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_EXCHANGE, AMQP_M_EXCH_DELETE_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPSupprimeÉchange",
                              "native", "AMQPSupprimeÉchange", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_amqp_publie(int argCount, Value* args) {
    (void)argCount;

    /* Verrou de rupture : le 4e argument DOIT être un amqpMessage. Validé AVANT
     * tout accès réseau (amqp_live) — sinon une connexion morte masquerait le
     * vrai code d'erreur (type d'argument) derrière un « connexion non ouverte ».
     * L'ancienne forme (corps brut en chaîne/Buffer) n'est plus acceptée. */
    if (!IS_INSTANCE(args[3]) ||
        AS_INSTANCE(args[3])->structName == NULL ||
        strcmp(AS_INSTANCE(args[3])->structName->chars, "amqpMessage") != 0) {
        const char* actual = IS_INSTANCE(args[3]) && AS_INSTANCE(args[3])->structName
                             ? AS_INSTANCE(args[3])->structName->chars : "un autre type";
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPPublie",
                              "native", "AMQPPublie", "arg", "4",
                              "expected", "amqpMessage", "actual", actual, NULL);
        return BOOL_VAL(false);
    }
    ObjInstance* msg = AS_INSTANCE(args[3]);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPPublie", NULL);
    if (c == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPPublie",
                              "native", "AMQPPublie", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    if (!IS_STRING(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPPublie",
                              "native", "AMQPPublie", "arg", "3",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* echange = AS_CSTRING(args[1]);
    const char* cle     = AS_CSTRING(args[2]);

    /* Corps depuis msg..Corps : chaîne (octets bruts, longueur exacte — pas
     * d'arrêt à un \0 interne) ou Buffer (binaire). Tout autre type -> corps
     * vide (le champ par défaut est une chaîne vide). */
    const unsigned char* corps = (const unsigned char*)"";
    size_t corps_len = 0;
    Value vCorps;
    if (amqp_get(msg, "Corps", &vCorps)) {
        if (IS_STRING(vCorps)) {
            corps     = (const unsigned char*)AS_STRING(vCorps)->chars;
            corps_len = (size_t)AS_STRING(vCorps)->length;
        } else if (IS_BUFFER(vCorps)) {
            corps     = AS_BUFFER(vCorps)->bytes;
            corps_len = (size_t)AS_BUFFER(vCorps)->length;
        }
    }

    /* Propriétés du message. */
    const char* content_type     = amqp_str(msg, "TypeContenu");
    const char* content_encoding = amqp_str(msg, "EncodageContenu");
    const char* correlation_id   = amqp_str(msg, "IdCorrelation");
    const char* reply_to         = amqp_str(msg, "RepondreA");
    const char* expiration       = amqp_str(msg, "Expiration");
    const char* message_id       = amqp_str(msg, "IdMessage");
    const char* app_type         = amqp_str(msg, "Type");
    bool persistant              = amqp_bool(msg, "Persistant");
    int  priorite                = amqp_int(msg, "Priorite");

    /* 1. Basic.Publish (60/40) method : reserved-1(u16 0), exchange(shortstr),
     * routing-key(shortstr), bits[mandatory,immediate] (0 pour ce lot). */
    AmqpBuf m = {0};
    abuf_u16(&m, 0);
    abuf_shortstr(&m, echange);
    abuf_shortstr(&m, cle);
    abuf_u8(&m, 0);
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_BASIC, AMQP_M_BASIC_PUBLISH, &m);
    free(m.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_PUBLISH_FAILURE, "AMQPPublie",
                              "native", "AMQPPublie",
                              "detail", "émission de Basic.Publish impossible", NULL);
        return BOOL_VAL(false);
    }

    /* 2. Content header frame (type 2) : class-id(60), weight(0), body-size(u64 BE),
     * property-flags(u16) + valeurs des propriétés présentes, sérialisées dans
     * l'ordre canonique des bits de basic.properties (MSB->LSB). */
    {
        bool has_ct    = content_type[0]     != '\0';
        bool has_ce    = content_encoding[0] != '\0';
        bool has_corr  = correlation_id[0]   != '\0';
        bool has_reply = reply_to[0]         != '\0';
        bool has_exp   = expiration[0]       != '\0';
        bool has_mid   = message_id[0]       != '\0';
        bool has_type  = app_type[0]         != '\0';
        bool has_prio  = priorite > 0;

        uint16_t flags = 0;
        AmqpBuf props = {0};   /* valeurs, dans l'ordre des bits */
        if (has_ct)   { flags |= 0x8000; abuf_shortstr(&props, content_type); }
        if (has_ce)   { flags |= 0x4000; abuf_shortstr(&props, content_encoding); }
        /* headers (bit 0x2000) : non encore émis. */
        flags |= 0x1000; abuf_u8(&props, persistant ? 2 : 1);   /* delivery-mode */
        if (has_prio) { flags |= 0x0800; abuf_u8(&props, (uint8_t)priorite); }
        if (has_corr) { flags |= 0x0400; abuf_shortstr(&props, correlation_id); }
        if (has_reply){ flags |= 0x0200; abuf_shortstr(&props, reply_to); }
        if (has_exp)  { flags |= 0x0100; abuf_shortstr(&props, expiration); }
        if (has_mid)  { flags |= 0x0080; abuf_shortstr(&props, message_id); }
        /* timestamp (bit 0x0040) : non émis. */
        if (has_type) { flags |= 0x0020; abuf_shortstr(&props, app_type); }

        AmqpBuf hp = {0};
        abuf_u16(&hp, AMQP_CLASS_BASIC);
        abuf_u16(&hp, 0);                        /* weight (toujours 0) */
        abuf_u64(&hp, (uint64_t)corps_len);
        abuf_u16(&hp, flags);
        if (props.len) abuf_bytes(&hp, props.data, props.len);
        free(props.data);

        AmqpBuf h = {0};
        abuf_u8(&h, AMQP_FRAME_HEADER);
        abuf_u16(&h, c->channel);
        abuf_u32(&h, (uint32_t)hp.len);
        abuf_bytes(&h, hp.data, hp.len);
        abuf_u8(&h, AMQP_FRAME_END);
        free(hp.data);
        bool hok = (h.data != NULL) && (amqp_send(c, h.data, h.len) == (int)h.len);
        free(h.data);
        if (!hok) {
            owl_emit_native_error(E_NATIVE_AMQP_PUBLISH_FAILURE, "AMQPPublie",
                                  "native", "AMQPPublie",
                                  "detail", "émission de l'en-tête de contenu impossible", NULL);
            return BOOL_VAL(false);
        }
    }

    /* 3. Body frame(s) (type 3). Découpe selon frame-max (overhead 8 octets :
     * 7 d'en-tête + 1 de fin). frame-max 0 = pas de limite -> un seul frame. */
    {
        size_t max_body = (c->frame_max > 8) ? (size_t)c->frame_max - 8 : 0;
        size_t off = 0;
        while (off < corps_len) {
            size_t chunk = corps_len - off;
            if (max_body > 0 && chunk > max_body) chunk = max_body;
            AmqpBuf b = {0};
            abuf_u8(&b, AMQP_FRAME_BODY);
            abuf_u16(&b, c->channel);
            abuf_u32(&b, (uint32_t)chunk);
            abuf_bytes(&b, corps + off, chunk);
            abuf_u8(&b, AMQP_FRAME_END);
            bool bok = (b.data != NULL) && (amqp_send(c, b.data, b.len) == (int)b.len);
            free(b.data);
            if (!bok) {
                owl_emit_native_error(E_NATIVE_AMQP_PUBLISH_FAILURE, "AMQPPublie",
                                      "native", "AMQPPublie",
                                      "detail", "émission du corps du message impossible", NULL);
                return BOOL_VAL(false);
            }
            off += chunk;
        }
    }

    /* 4. Mode publisher-confirms actif : attendre l'accusé du broker. Le retour
     * de AMQPPublie devient une vraie garantie (Vrai = message accepté). Sans
     * mode confirm, on ne bloque pas (fire-and-forget, comportement inchangé). */
    if (c->confirm_mode) {
        bool acked = false;
        char detail[256];
        if (!amqp_await_confirm(c, &acked, detail, sizeof(detail))) {
            owl_emit_native_error(E_NATIVE_AMQP_PUBLISH_FAILURE, "AMQPPublie",
                                  "native", "AMQPPublie", "detail", detail, NULL);
            return BOOL_VAL(false);
        }
        if (!acked) {
            owl_emit_native_error(E_NATIVE_AMQP_PUBLISH_FAILURE, "AMQPPublie",
                                  "native", "AMQPPublie",
                                  "detail", "le broker a rejeté le message (Basic.Nack)", NULL);
            return BOOL_VAL(false);
        }
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

/* AMQPConfirmations(<connexion>, <actif>) -> booléen. Active (ou refuse de
 * désactiver) le mode publisher-confirms sur le channel. Vrai : émet
 * Confirm.Select (85/10), attend Confirm.Select-Ok (85/11) et pose confirm_mode
 * (idempotent si déjà actif). Faux : no-op si le mode est déjà inactif ; une fois
 * activé le mode ne peut plus être désactivé (contrainte AMQP) -> erreur claire.
 * Le drapeau `actif` est validé AVANT tout accès réseau (amqp_live) : une
 * connexion morte ne doit pas masquer un type d'argument invalide. */
static Value fn_amqp_confirmations(int argCount, Value* args) {
    (void)argCount;
    if (!IS_BOOL(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPConfirmations",
                              "native", "AMQPConfirmations", "arg", "2",
                              "expected", "booléen", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    bool actif = AS_BOOL(args[1]);

    OwlAmqpConn* c = amqp_live(args[0], "AMQPConfirmations", NULL);
    if (c == NULL) return BOOL_VAL(false);

    if (!actif) {
        if (!c->confirm_mode) { owl_clear_error(); return BOOL_VAL(true); }
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConfirmations",
                              "native", "AMQPConfirmations",
                              "detail", "le mode confirm ne peut pas être désactivé une fois activé sur le channel", NULL);
        return BOOL_VAL(false);
    }

    if (c->confirm_mode) { owl_clear_error(); return BOOL_VAL(true); }  /* idempotent */

    /* Confirm.Select (85/10) : nowait(bit) = 0 (on veut Confirm.Select-Ok). */
    AmqpBuf a = {0};
    abuf_u8(&a, 0);
    bool sent = amqp_send_method(c, c->channel, AMQP_CLASS_CONFIRM, AMQP_M_CONFIRM_SELECT, &a);
    free(a.data);
    if (!sent) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConfirmations",
                              "native", "AMQPConfirmations",
                              "detail", "émission de Confirm.Select impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_CONFIRM, AMQP_M_CONFIRM_SELECT_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConfirmations",
                              "native", "AMQPConfirmations", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    c->confirm_mode = true;
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Natives — Lot C (consommation, acquittement, pompe).
 *
 * Modele calque sur le client MQTT : le reacteur est POSSEDE par
 * la connexion (cree paresseusement, ticke par AMQPAttend). E1 sert au registre
 * + rooting GC des handlers ; la livraison est maison (4 args dont Buffer).
 * Le heartbeat AMQP est negocie a 0 (Lot A) : pas de timer de battement (le
 * broker ne droppe pas, un TCP mort est detecte au recv).
 * ============================================================ */

/* --- Registre consumer-tag -> handle E1 (par connexion) --- */

static int amqp_consumer_index(OwlAmqpConn* c, const char* tag) {
    for (int i = 0; i < c->nb_consumers; i++) {
        if (strcmp(c->consumers[i].tag, tag) == 0) return i;
    }
    return -1;
}

static OwlMqHandle amqp_consumer_lookup(OwlAmqpConn* c, const char* tag,
                                        bool* out_auto_ack) {
    int i = amqp_consumer_index(c, tag);
    if (i < 0) return OWL_MQ_HANDLE_NUL;
    if (out_auto_ack) *out_auto_ack = c->consumers[i].auto_ack;
    return c->consumers[i].handle;
}

static bool amqp_consumer_add(OwlAmqpConn* c, const char* tag, OwlMqHandle h,
                              bool auto_ack) {
    if (c->nb_consumers >= AMQP_MAX_CONSUMERS) return false;
    char* copie = strdup(tag ? tag : "");
    if (copie == NULL) return false;
    c->consumers[c->nb_consumers].tag      = copie;
    c->consumers[c->nb_consumers].handle   = h;
    c->consumers[c->nb_consumers].auto_ack = auto_ack;
    c->nb_consumers++;
    return true;
}

/* --- Livraison C -> OWL : (connexion amqpConnection, message amqpMessage).
 * AMQP construit un amqpMessage frais (corps Buffer + propriétés décodées du
 * content-header) et le passe au callback ; le delivery-tag vit dans
 * msg..Identifiant. E1 reste le registre + le rooting GC des handlers. Motif de
 * réentrance bornée (E1/http_exec) : repère stackTop -> push callee + args
 * (chacun roote à la création) -> appel -> reset au repère. --- */

static void amqp_livre(OwlAmqpConn* c) {
    OwlMqHandle h = amqp_consumer_lookup(c, c->deliv.consumer_tag, NULL);
    if (h == OWL_MQ_HANDLE_NUL) return;
    Value handler;
    if (!owl_mq_handler(h, &handler)) return;
    if (c->session == NULL) return;

    Value* repere = vm.stackTop;                    /* frontiere de depilage propre */
    vm_push(handler);                               /* callee au-dessus du repere */
    vm_push(OBJ_VAL(c->session));                   /* arg 1 : connexion */

    /* arg 2 : amqpMessage frais. Instance rootee immediatement (push) : les
     * inst_field_set_* suivants allouent (key + valeurs) -> grow -> GC possible.
     * gc-root-obj-fresh. */
    ObjInstance* msg = build_amqpMessage_template();
    vm_push(OBJ_VAL(msg));

    /* Corps = Buffer du corps assemble (longueur exacte, \0 internes compris).
     * inst_field_set_value pousse la valeur avant d'allouer la key : le Buffer
     * frais survit au grow. */
    ObjBuffer* buf = obj_new_buffer(c->deliv.body ? (const void*)c->deliv.body
                                                  : (const void*)"",
                                    (int)c->deliv.body_len);
    inst_field_set_value(msg, "Corps", OBJ_VAL(buf));

    inst_field_set_str (msg, "CleRoutage",  c->deliv.routing_key);
    inst_field_set_int (msg, "Identifiant", (int64_t)c->deliv.delivery_tag);
    inst_field_set_bool(msg, "Persistant",  c->deliv.delivery_mode == 2);
    inst_field_set_int (msg, "Priorite",    (int64_t)c->deliv.priority);
    if (c->deliv.content_type[0])     inst_field_set_str(msg, "TypeContenu",     c->deliv.content_type);
    if (c->deliv.content_encoding[0]) inst_field_set_str(msg, "EncodageContenu", c->deliv.content_encoding);
    if (c->deliv.correlation_id[0])   inst_field_set_str(msg, "IdCorrelation",   c->deliv.correlation_id);
    if (c->deliv.reply_to[0])         inst_field_set_str(msg, "RepondreA",       c->deliv.reply_to);
    if (c->deliv.expiration[0])       inst_field_set_str(msg, "Expiration",      c->deliv.expiration);
    if (c->deliv.message_id[0])       inst_field_set_str(msg, "IdMessage",       c->deliv.message_id);
    if (c->deliv.msg_type[0])         inst_field_set_str(msg, "Type",            c->deliv.msg_type);

    (void)vm_call_reentrant(handler, 2);

    vm.stackTop = repere;                           /* rejette tout residu de retour */
}

/* --- Assemblage Basic.Deliver multi-frame --- */

static const uint8_t* amqp_read_shortstr(const uint8_t* p, const uint8_t* end,
                                         char* out, size_t out_sz) {
    if (out_sz) out[0] = '\0';
    if (p >= end) return p;
    uint8_t l = *p++;
    if (p + l > end) return end;                    /* tronque : on abandonne */
    size_t n = (size_t)l < out_sz - 1 ? (size_t)l : out_sz - 1;
    if (out_sz) { memcpy(out, p, n); out[n] = '\0'; }
    return p + l;
}

/* Livre la livraison assemblee (corps complet) puis remet l'etat a plat. */
static void amqp_deliv_flush(OwlAmqpConn* c) {
    amqp_livre(c);
    c->deliv.active    = false;
    c->deliv.body_len  = 0;
    c->deliv.body_size = 0;
}

/* Traite une method-frame Basic.Deliver : ouvre une livraison en cours. */
static void amqp_deliv_method(OwlAmqpConn* c, const uint8_t* payload, uint32_t size) {
    const uint8_t* p   = payload + 4;               /* apres class + method */
    const uint8_t* end = payload + size;
    p = amqp_read_shortstr(p, end, c->deliv.consumer_tag, sizeof(c->deliv.consumer_tag));
    if (p + 8 > end) { c->deliv.active = false; return; }
    uint64_t dtag = 0;
    for (int i = 0; i < 8; i++) dtag = (dtag << 8) | p[i];
    p += 8;
    if (p + 1 > end) { c->deliv.active = false; return; }
    p += 1;                                         /* redelivered (bit), ignore */
    char exchange[256];
    p = amqp_read_shortstr(p, end, exchange, sizeof(exchange));  /* echange, ignore */
    (void)amqp_read_shortstr(p, end, c->deliv.routing_key, sizeof(c->deliv.routing_key));
    c->deliv.delivery_tag = dtag;
    c->deliv.active       = true;
    c->deliv.body_len     = 0;
    c->deliv.body_size    = 0;
    /* Remise a plat des proprietes decodees (evite le report d'une livraison
     * precedente si le content-header ne pose pas un bit). */
    c->deliv.delivery_mode      = 0;
    c->deliv.priority           = 0;
    c->deliv.content_type[0]     = '\0';
    c->deliv.content_encoding[0] = '\0';
    c->deliv.correlation_id[0]   = '\0';
    c->deliv.reply_to[0]         = '\0';
    c->deliv.expiration[0]       = '\0';
    c->deliv.message_id[0]       = '\0';
    c->deliv.msg_type[0]         = '\0';
}

/* Traite une content-header-frame : taille du corps attendue + décodage des
 * property-flags (basic.properties). Livre immédiatement si le corps est vide.
 * Décodage symétrique de l'encodage AMQPPublie : mêmes bits, même ordre
 * (MSB->LSB). Les 14 propriétés tiennent dans les bits 15..2 d'un seul mot de
 * flags (le bit 0 de continuation n'est jamais posé pour basic.properties). */
static void amqp_deliv_header(OwlAmqpConn* c, const uint8_t* payload, uint32_t size) {
    if (!c->deliv.active) return;
    if (size < 12) { c->deliv.active = false; return; } /* class(2)+weight(2)+body(8) */
    uint64_t bs = 0;
    for (int i = 0; i < 8; i++) bs = (bs << 8) | payload[4 + i];
    c->deliv.body_size = bs;
    c->deliv.body_len  = 0;

    if (size >= 14) {
        const uint8_t* p   = payload + 12;
        const uint8_t* end = payload + size;
        uint16_t flags = (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        if (flags & 0x8000) p = amqp_read_shortstr(p, end, c->deliv.content_type,     sizeof(c->deliv.content_type));
        if (flags & 0x4000) p = amqp_read_shortstr(p, end, c->deliv.content_encoding, sizeof(c->deliv.content_encoding));
        if (flags & 0x2000) {                           /* headers field-table : sauté */
            if (p + 4 <= end) {
                uint32_t ftl = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
                p += 4;
                p = (p + ftl <= end) ? p + ftl : end;
            } else { p = end; }
        }
        if (flags & 0x1000) { if (p < end) c->deliv.delivery_mode = *p++; }  /* delivery-mode u8 */
        if (flags & 0x0800) { if (p < end) c->deliv.priority      = *p++; }  /* priority u8 */
        if (flags & 0x0400) p = amqp_read_shortstr(p, end, c->deliv.correlation_id, sizeof(c->deliv.correlation_id));
        if (flags & 0x0200) p = amqp_read_shortstr(p, end, c->deliv.reply_to,       sizeof(c->deliv.reply_to));
        if (flags & 0x0100) p = amqp_read_shortstr(p, end, c->deliv.expiration,     sizeof(c->deliv.expiration));
        if (flags & 0x0080) p = amqp_read_shortstr(p, end, c->deliv.message_id,     sizeof(c->deliv.message_id));
        if (flags & 0x0040) { p = (p + 8 <= end) ? p + 8 : end; }            /* timestamp u64 : sauté */
        if (flags & 0x0020) (void)amqp_read_shortstr(p, end, c->deliv.msg_type, sizeof(c->deliv.msg_type)); /* type */
    }

    if (bs == 0) amqp_deliv_flush(c);
}

/* Accumule une body-frame ; livre quand le corps est complet. */
static void amqp_deliv_body(OwlAmqpConn* c, const uint8_t* payload, uint32_t size) {
    if (!c->deliv.active) return;
    if (c->deliv.body_len + size > c->deliv.body_cap) {
        size_t ncap = c->deliv.body_cap ? c->deliv.body_cap : 256;
        while (ncap < c->deliv.body_len + size) ncap *= 2;
        uint8_t* nb = (uint8_t*)realloc(c->deliv.body, ncap);
        if (nb == NULL) { c->deliv.active = false; return; }
        c->deliv.body     = nb;
        c->deliv.body_cap = ncap;
    }
    memcpy(c->deliv.body + c->deliv.body_len, payload, size);
    c->deliv.body_len += size;
    if (c->deliv.body_len >= c->deliv.body_size) amqp_deliv_flush(c);
}

/* Dispatch d'une trame complete par type. Seul le triplet (Basic.Deliver method
 * + content-header + body) nous interesse ; le reste (Channel.Close, etc.) est
 * ignore. */
static void amqp_handle_frame(OwlAmqpConn* c, uint8_t type,
                              const uint8_t* payload, uint32_t size) {
    if (type == AMQP_FRAME_METHOD) {
        if (size < 4) return;
        uint16_t cls  = (uint16_t)((payload[0] << 8) | payload[1]);
        uint16_t meth = (uint16_t)((payload[2] << 8) | payload[3]);
        if (cls == AMQP_CLASS_BASIC && meth == AMQP_M_BASIC_DELIVER) {
            amqp_deliv_method(c, payload, size);
        }
        /* Autres methods entrantes (Basic.Cancel, Channel.Close…) : ignorees. */
    } else if (type == AMQP_FRAME_HEADER) {
        amqp_deliv_header(c, payload, size);
    } else if (type == AMQP_FRAME_BODY) {
        amqp_deliv_body(c, payload, size);
    }
    /* heartbeat (type 8) : avale. */
}

static void amqp_rx_append(OwlAmqpConn* c, const uint8_t* data, size_t len) {
    if (c->rx_len + len > c->rx_cap) {
        size_t ncap = c->rx_cap ? c->rx_cap : 256;
        while (ncap < c->rx_len + len) ncap *= 2;
        uint8_t* nb = (uint8_t*)realloc(c->rx, ncap);
        if (nb == NULL) return;                     /* best-effort : bloc ignore */
        c->rx = nb;
        c->rx_cap = ncap;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
}

/* Frame toutes les trames completes accumulees dans `rx` et les dispatche, puis
 * compacte le reliquat. Une trame = [type:1][ch:2][size:4][payload][0xCE]. */
static void amqp_rx_parse(OwlAmqpConn* c) {
    size_t off = 0;
    while (c->rx_len - off >= 7) {
        uint8_t  type = c->rx[off];
        uint32_t size = ((uint32_t)c->rx[off + 3] << 24) |
                        ((uint32_t)c->rx[off + 4] << 16) |
                        ((uint32_t)c->rx[off + 5] << 8) |
                        (uint32_t)c->rx[off + 6];
        if (size > AMQP_READ_CAP) { c->connected = false; break; }  /* trame aberrante */
        size_t total = 7 + (size_t)size + 1;        /* + frame-end */
        if (c->rx_len - off < total) break;         /* trame incomplete : attendre */
        if (c->rx[off + 7 + size] != AMQP_FRAME_END) { c->connected = false; break; }
        amqp_handle_frame(c, type, c->rx + off + 7, size);
        off += total;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
    /* Le reliquat apres compactage est une trame incomplete (scindee par TCP) :
     * c'est la continuation du flux que le chemin sync doit consommer en priorite
     * avant de relire la socket. Les trames async re-injectees plus tard pendant
     * un appel sync s'ajouteront au-dela de cette borne (non drainables). */
    c->rx_drainable = c->rx_len;
}

/* Callback de lecture du reacteur : draine la socket, accumule, frame. */
static void amqp_reacteur_lecture_cb(OwlReacteur* r, owl_socket_fd_t fd,
                                     short evts_prets, void* user) {
    OwlAmqpConn* c = (OwlAmqpConn*)user;
    if (evts_prets & OWL_REAC_LECTURE) {
        uint8_t tampon[4096];
        int n = amqp_recv(c, tampon, sizeof(tampon));
        if (n > 0) {
            amqp_rx_append(c, tampon, (size_t)n);
            amqp_rx_parse(c);
            return;
        }
        owl_reacteur_retire_fd(r, fd);              /* EOF (n==0) ou erreur (n<0) */
        c->connected = false;                       /* broker a raccroche */
        c->fd_branche = false;
        return;
    }
    if (evts_prets & OWL_REAC_ERREUR) {
        owl_reacteur_retire_fd(r, fd);
        c->connected = false;
        c->fd_branche = false;
    }
}

/* Cree paresseusement le reacteur possede par la connexion et y branche la
 * socket vivante en interet LECTURE (une seule fois). false si allocation
 * impossible ou fd invalide. */
static bool amqp_reacteur_assure(OwlAmqpConn* c) {
    if (c->reacteur == NULL) {
        c->reacteur = owl_reacteur_cree();
        if (c->reacteur == NULL) return false;
    }
    if (!c->fd_branche) {
        if (!owl_reacteur_ajoute_fd(c->reacteur, c->transport.tcp.fd,
                                    OWL_REAC_LECTURE, amqp_reacteur_lecture_cb, c))
            return false;
        c->fd_branche = true;
    }
    return true;
}

/* AMQPConsomme(<connexion>, <queue>, <procédure> [, <autoAck>]) -> booléen.
 * Emet Basic.Consume, lit Consume-Ok en SYNCHRONE (il precede toute livraison
 * sur le channel : pas d'entrelacement), memorise le mapping consumer-tag ->
 * handler et branche le reacteur. Les Basic.Deliver entrants sont dispatches
 * par AMQPAttend. autoAck defaut Vrai (no-ack=1). */
static Value fn_amqp_consomme(int argCount, Value* args) {
    ObjInstance* inst = NULL;
    OwlAmqpConn* c = amqp_live(args[0], "AMQPConsomme", &inst);
    if (c == NULL) return BOOL_VAL(false);

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPConsomme",
                              "native", "AMQPConsomme", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* queue = AS_CSTRING(args[1]);

    /* autoAck : defaut Vrai (no-ack=1). Faux -> ack manuel requis. */
    bool auto_ack = (argCount >= 4 && IS_BOOL(args[3])) ? AS_BOOL(args[3]) : true;

    /* Enregistre le handler via E1 : valide qu'il est appelable et le tient
     * comme racine GC. Refus (non appelable / registre plein) -> Faux, avant
     * toute emission sur le fil. */
    OwlMqHandle h = owl_mq_abonne(args[2]);
    if (h == OWL_MQ_HANDLE_NUL) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPConsomme",
                              "native", "AMQPConsomme", "arg", "3",
                              "expected", "procédure", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }

    /* Basic.Consume (60/20) : reserved-1(u16 0), queue(shortstr), consumer-tag
     * (shortstr vide = broker assigne), bits[no-local,no-ack,exclusive,no-wait],
     * arguments(field-table vide). */
    AmqpBuf a = {0};
    abuf_u16(&a, 0);
    abuf_shortstr(&a, queue);
    abuf_shortstr(&a, "");
    uint8_t bits = 0;
    if (auto_ack) bits |= 0x02;                     /* no-ack */
    abuf_u8(&a, bits);
    abuf_u32(&a, 0);
    bool sent = amqp_send_method(c, c->channel, AMQP_CLASS_BASIC, AMQP_M_BASIC_CONSUME, &a);
    free(a.data);
    if (!sent) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConsomme",
                              "native", "AMQPConsomme",
                              "detail", "émission de Basic.Consume impossible", NULL);
        return BOOL_VAL(false);
    }

    /* Consume-Ok (60/21) : consumer-tag(shortstr). Lu en synchrone. */
    AmqpFrame fr;
    uint16_t cls = 0, meth = 0;
    if (!amqp_read_method(c, &cls, &meth, &fr)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConsomme",
                              "native", "AMQPConsomme",
                              "detail", "Basic.Consume-Ok attendu, trame inattendue ou connexion coupée", NULL);
        return BOOL_VAL(false);
    }
    if (cls != AMQP_CLASS_BASIC || meth != AMQP_M_BASIC_CONSUME_OK) {
        char detail[128];
        snprintf(detail, sizeof(detail),
                 "réponse inattendue à Basic.Consume (classe %u méthode %u)", cls, meth);
        free(fr.payload);
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConsomme",
                              "native", "AMQPConsomme", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    char tag[256];
    (void)amqp_read_shortstr(fr.payload + 4, fr.payload + fr.size, tag, sizeof(tag));
    free(fr.payload);

    if (!amqp_consumer_add(c, tag, h, auto_ack)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPConsomme",
                              "native", "AMQPConsomme",
                              "detail", "nombre maximal de consommateurs atteint sur la connexion", NULL);
        return BOOL_VAL(false);
    }

    /* Reacteur possede par la connexion : cree + fd branche. AMQPAttend le
     * tickera pour dispatcher les Basic.Deliver entrants. */
    amqp_reacteur_assure(c);

    owl_clear_error();
    return BOOL_VAL(true);
}

/* AMQPAcquitte(<connexion>, <tag>) -> booléen. Emet Basic.Ack (60/80) :
 * delivery-tag(u64 BE), multiple(bit 0). Fire-and-forget (pas de réponse). */
static Value fn_amqp_acquitte(int argCount, Value* args) {
    (void)argCount;
    OwlAmqpConn* c = amqp_live(args[0], "AMQPAcquitte", NULL);
    if (c == NULL) return BOOL_VAL(false);

    if (!IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPAcquitte",
                              "native", "AMQPAcquitte", "arg", "2",
                              "expected", "entier", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    uint64_t dtag = (uint64_t)AS_INTEGER(args[1]);

    AmqpBuf a = {0};
    abuf_u64(&a, dtag);
    abuf_u8(&a, 0);                                 /* multiple = 0 */
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_BASIC, AMQP_M_BASIC_ACK, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPAcquitte",
                              "native", "AMQPAcquitte",
                              "detail", "émission de Basic.Ack impossible", NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* AMQPRejette(<connexion>, <tag>, <remetEnFile>) -> booléen. Emet Basic.Nack
 * (60/120) : delivery-tag(u64 BE), bits[multiple(0), requeue]. Fire-and-forget
 * (pas de réponse), comme Basic.Ack. remetEnFile Vrai -> le message repart en
 * file (redélivré) ; Faux -> écarté (ou dead-letter si la queue en a un). */
static Value fn_amqp_rejette(int argCount, Value* args) {
    (void)argCount;
    /* Verrou de rupture : le tag DOIT être un entier, validé AVANT tout accès
     * réseau (amqp_live) — sinon une connexion morte masquerait le vrai code
     * d'erreur (type d'argument) derrière un « connexion non ouverte ». */
    if (!IS_INTEGER(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPRejette",
                              "native", "AMQPRejette", "arg", "2",
                              "expected", "entier", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    OwlAmqpConn* c = amqp_live(args[0], "AMQPRejette", NULL);
    if (c == NULL) return BOOL_VAL(false);

    uint64_t dtag   = (uint64_t)AS_INTEGER(args[1]);
    bool     requeue = amqp_opt_bool(argCount, args, 2);

    AmqpBuf a = {0};
    abuf_u64(&a, dtag);
    abuf_u8(&a, requeue ? 0x02 : 0x00);             /* bit0 multiple=0, bit1 requeue */
    bool ok = amqp_send_method(c, c->channel, AMQP_CLASS_BASIC, AMQP_M_BASIC_NACK, &a);
    free(a.data);
    if (!ok) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPRejette",
                              "native", "AMQPRejette",
                              "detail", "émission de Basic.Nack impossible", NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* AMQPPrefetch(<connexion>, <nombre>) -> booléen. Emet Basic.Qos (60/10) :
 * prefetch-size(u32 0), prefetch-count(u16), global(bit 0 -> par consommateur),
 * puis attend Qos-Ok (60/11). Limite le nombre de messages non acquittés que le
 * serveur envoie d'avance. A appeler AVANT AMQPConsomme. */
static Value fn_amqp_prefetch(int argCount, Value* args) {
    (void)argCount;
    /* Verrou de rupture : le nombre DOIT être un entier >= 0, validé AVANT tout
     * accès réseau (amqp_live). */
    if (!IS_INTEGER(args[1]) || AS_INTEGER(args[1]) < 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "AMQPPrefetch",
                              "native", "AMQPPrefetch", "arg", "2",
                              "expected", "entier", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    OwlAmqpConn* c = amqp_live(args[0], "AMQPPrefetch", NULL);
    if (c == NULL) return BOOL_VAL(false);

    uint16_t count = (uint16_t)AS_INTEGER(args[1]);

    AmqpBuf a = {0};
    abuf_u32(&a, 0);                                /* prefetch-size (octets) : illimité */
    abuf_u16(&a, count);                            /* prefetch-count */
    abuf_u8(&a, 0);                                 /* global=0 : par consommateur */
    bool sent = amqp_send_method(c, c->channel, AMQP_CLASS_BASIC, AMQP_M_BASIC_QOS, &a);
    free(a.data);
    if (!sent) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPPrefetch",
                              "native", "AMQPPrefetch",
                              "detail", "émission de Basic.Qos impossible", NULL);
        return BOOL_VAL(false);
    }
    char detail[256];
    if (!amqp_expect(c, AMQP_CLASS_BASIC, AMQP_M_BASIC_QOS_OK, detail, sizeof(detail))) {
        owl_emit_native_error(E_NATIVE_AMQP_OP_FAILURE, "AMQPPrefetch",
                              "native", "AMQPPrefetch", "detail", detail, NULL);
        return BOOL_VAL(false);
    }
    owl_clear_error();
    return BOOL_VAL(true);
}

/* AMQPAttend(<connexion>, <délai>) -> booléen. Pompe le reacteur possede par la
 * connexion pendant `délai` ms au plus : dispatche les Basic.Deliver entrants
 * aux procedures consumer, puis rend la main. Renvoie Vrai tant que la connexion
 * reste ouverte, Faux si le broker a raccroche. Idiome worker :
 * TANTQUE AMQPAttend(cnx, 1000) ... FIN. */
static Value fn_amqp_attend(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_amqp_connection(args[0], "AMQPAttend");
    if (inst == NULL) return BOOL_VAL(false);

    OwlAmqpConn* c = amqp_handle(inst);
    if (c == NULL || !c->connected) return BOOL_VAL(false);

    int64_t delai = IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : 0;
    if (delai < 0) delai = 0;

    if (!amqp_reacteur_assure(c)) return BOOL_VAL(c->connected);

    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) break;                   /* broker a raccroche */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)delai) break;       /* fenetre epuisee */
        int reste = (int)((uint64_t)delai - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) break;  /* poll casse */
    }
    return BOOL_VAL(c->connected);
}

/* ============================================================
 * Enum amqpTypeÉchange — type d'un échange AMQP. Les valeurs sont les noms
 * des constantes membres (chacune une OwlConstDef à valeur sentinel). Le nom
 * wire ("direct"/"fanout"/…) est résolu à l'émission.
 * ============================================================ */

static const char* const AMQP_EXCH_VALUES[] = {
    "amqpDirect", "amqpFanout", "amqpTopic", "amqpHeaders", NULL
};
static const OwlEnum E_amqpTypeEchange = {
    .name      = "amqpTypeÉchange",
    .nameAscii = "amqpTypeEchange",
    .nameEN    = "amqpExchangeType",
    .category  = "AMQP",
    .descFR    = "Type d'un échange AMQP : routage direct, diffusion, par motif de sujet, ou par en-têtes.",
    .descEN    = "Type of an AMQP exchange: direct routing, broadcast, topic pattern, or headers.",
    .values    = AMQP_EXCH_VALUES,
    .combinable = false,
};

static const OwlEnum* const AMQP_ENUMS[] = { &E_amqpTypeEchange };

static const OwlConstDef AMQP_CONSTS[] = {
    { "amqpDirect", "AMQP", INTEGER_VAL(OWL_AMQP_EXCH_DIRECT),
      "Échange direct : le message va vers les queues dont la clé de liaison égale la clé de routage.",
      "Direct exchange: the message goes to queues whose binding key equals the routing key.", "amqpDirect" },
    { "amqpFanout", "AMQP", INTEGER_VAL(OWL_AMQP_EXCH_FANOUT),
      "Échange par diffusion : le message est copié vers toutes les queues liées, sans tenir compte de la clé de routage.",
      "Fanout exchange: the message is copied to every bound queue, ignoring the routing key.", "amqpFanout" },
    { "amqpTopic", "AMQP", INTEGER_VAL(OWL_AMQP_EXCH_TOPIC),
      "Échange par sujet : la clé de routage est comparée aux clés de liaison à motif (jokers).",
      "Topic exchange: the routing key is matched against pattern binding keys (wildcards).", "amqpTopic" },
    { "amqpHeaders", "AMQP", INTEGER_VAL(OWL_AMQP_EXCH_HEADERS),
      "Échange par en-têtes : le routage se fonde sur les en-têtes du message plutôt que sur la clé de routage.",
      "Headers exchange: routing is based on the message headers rather than the routing key.", "amqpHeaders" },
    { NULL, NULL, INTEGER_VAL(0), NULL, NULL, NULL }
};

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_connexion[] = {
    {"connexion", "amqpConnection", "connexion AMQP décrivant les paramètres de connexion au serveur"}, {0}
};
static const OwlSignature sig_connecte[] = {
    {1, p_connexion, "booléen", "vrai si la connexion au serveur AMQP a réussi, faux sinon"}, {0}
};
static const OwlSignature sig_deconnecte[] = {
    {1, p_connexion, "rien", NULL}, {0}
};

static const OwlParam p_declare_queue[] = {
    {"connexion",       "amqpConnection", "connexion AMQP ouverte"},
    {"nom",             "chaîne",         "nom de la queue à déclarer (vide pour un nom généré par le serveur)"},
    {"durable",         "booléen",        "vrai pour une queue qui survit au redémarrage du serveur (optionnel, faux par défaut)"},
    {"exclusive",       "booléen",        "vrai pour une queue réservée à cette connexion (optionnel, faux par défaut)"},
    {"autoSuppression", "booléen",        "vrai pour supprimer la queue quand le dernier consommateur se déconnecte (optionnel, faux par défaut)"},
    {"arguments",       "tableau associatif", "arguments étendus de la queue (x-arguments AMQP), clé chaîne → valeur (optionnel)"},
    {0}
};
#define AMQP_DECLARE_QUEUE_RET "nom effectif de la queue déclarée (utile quand le serveur en génère un)"
static const OwlSignature sig_declare_queue[] = {
    {2, p_declare_queue, "chaîne", AMQP_DECLARE_QUEUE_RET},
    {3, p_declare_queue, "chaîne", AMQP_DECLARE_QUEUE_RET},
    {4, p_declare_queue, "chaîne", AMQP_DECLARE_QUEUE_RET},
    {5, p_declare_queue, "chaîne", AMQP_DECLARE_QUEUE_RET},
    {6, p_declare_queue, "chaîne", AMQP_DECLARE_QUEUE_RET},
    {0}
};

static const OwlParam p_declare_echange[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"nom",       "chaîne",         "nom de l'échange à déclarer"},
    {"type",      "entier",         "type d'échange", .allowedBase=&E_amqpTypeEchange},
    {"durable",   "booléen",        "vrai pour un échange qui survit au redémarrage du serveur (optionnel, faux par défaut)"},
    {0}
};
#define AMQP_DECLARE_ECHANGE_RET "vrai si l'échange a été déclaré, faux sinon"
static const OwlSignature sig_declare_echange[] = {
    {3, p_declare_echange, "booléen", AMQP_DECLARE_ECHANGE_RET},
    {4, p_declare_echange, "booléen", AMQP_DECLARE_ECHANGE_RET},
    {0}
};

static const OwlParam p_lie[] = {
    {"connexion",  "amqpConnection", "connexion AMQP ouverte"},
    {"queue",      "chaîne",         "nom de la queue à lier"},
    {"échange",    "chaîne",         "nom de l'échange source"},
    {"cléRoutage", "chaîne",         "clé de routage (ou motif de liaison) reliant l'échange à la queue"},
    {0}
};
static const OwlSignature sig_lie[] = {
    {4, p_lie, "booléen", "vrai si la liaison a été créée, faux sinon"}, {0}
};

static const OwlParam p_supprime_queue[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"nom",       "chaîne",         "nom de la queue à supprimer"},
    {0}
};
static const OwlSignature sig_supprime_queue[] = {
    {2, p_supprime_queue, "booléen", "vrai si la queue a été supprimée, faux sinon"}, {0}
};

static const OwlParam p_purge_queue[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"nom",       "chaîne",         "nom de la queue à vider"},
    {0}
};
static const OwlSignature sig_purge_queue[] = {
    {2, p_purge_queue, "entier", "nombre de messages retirés de la queue, ou -1 en cas d'échec"}, {0}
};

static const OwlParam p_delie[] = {
    {"connexion",  "amqpConnection", "connexion AMQP ouverte"},
    {"queue",      "chaîne",         "nom de la queue à délier"},
    {"échange",    "chaîne",         "nom de l'échange source"},
    {"cléRoutage", "chaîne",         "clé de routage de la liaison à défaire"},
    {0}
};
static const OwlSignature sig_delie[] = {
    {4, p_delie, "booléen", "vrai si la liaison a été défaite, faux sinon"}, {0}
};

static const OwlParam p_supprime_echange[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"nom",       "chaîne",         "nom de l'échange à supprimer"},
    {0}
};
static const OwlSignature sig_supprime_echange[] = {
    {2, p_supprime_echange, "booléen", "vrai si l'échange a été supprimé, faux sinon"}, {0}
};

static const OwlParam p_publie[] = {
    {"connexion",  "amqpConnection", "connexion AMQP ouverte"},
    {"échange",    "chaîne",         "nom de l'échange cible (vide pour l'échange par défaut)"},
    {"cléRoutage", "chaîne",         "clé de routage du message (nom de queue pour l'échange par défaut)"},
    {"message",    "amqpMessage",    "message à publier : corps, persistance et propriétés (type de contenu, corrélation, priorité…)"},
    {0}
};
#define AMQP_PUBLIE_RET "vrai si le message a été émis, faux sinon"
static const OwlSignature sig_publie[] = {
    {4, p_publie, "booléen", AMQP_PUBLIE_RET},
    {0}
};

static const OwlParam p_consomme[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"queue",     "chaîne",         "nom de la queue à consommer"},
    {"procédure", "procédure",      "procédure appelée à chaque message reçu (connexion, message) ; le message reçu porte le corps et ses propriétés, et son identifiant de livraison"},
    {"autoAck",   "booléen",        "vrai pour acquitter automatiquement les messages, faux pour un acquittement manuel via AMQPAcquitte (optionnel, vrai par défaut)"},
    {0}
};
#define AMQP_CONSOMME_RET "vrai si la consommation a été mise en place, faux sinon"
static const OwlSignature sig_consomme[] = {
    {3, p_consomme, "booléen", AMQP_CONSOMME_RET},
    {4, p_consomme, "booléen", AMQP_CONSOMME_RET},
    {0}
};

static const OwlParam p_acquitte[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"tag",       "entier",         "identifiant de livraison du message à acquitter (reçu par la procédure consumer)"},
    {0}
};
static const OwlSignature sig_acquitte[] = {
    {2, p_acquitte, "booléen", "vrai si l'acquittement a été émis, faux sinon"}, {0}
};

static const OwlParam p_rejette[] = {
    {"connexion",   "amqpConnection", "connexion AMQP ouverte"},
    {"tag",         "entier",         "identifiant de livraison du message à rejeter (reçu par la procédure consumer)"},
    {"remetEnFile", "booléen",        "vrai pour remettre le message en file (il sera redélivré), faux pour l'écarter"},
    {0}
};
static const OwlSignature sig_rejette[] = {
    {3, p_rejette, "booléen", "vrai si le rejet a été émis, faux sinon"}, {0}
};

static const OwlParam p_prefetch[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"nombre",    "entier",         "nombre maximal de messages non acquittés que le serveur envoie d'avance (0 pour aucune limite)"},
    {0}
};
static const OwlSignature sig_prefetch[] = {
    {2, p_prefetch, "booléen", "vrai si la limite a été acceptée par le serveur, faux sinon"}, {0}
};

static const OwlParam p_attend[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"délai",     "entier",         "durée maximale d'attente, en millisecondes"},
    {0}
};
static const OwlSignature sig_attend[] = {
    {2, p_attend, "booléen", "vrai si la connexion est toujours ouverte, faux si le serveur a fermé la connexion pendant l'attente"}, {0}
};

static const OwlParam p_confirmations[] = {
    {"connexion", "amqpConnection", "connexion AMQP ouverte"},
    {"actif",     "booléen",        "vrai pour activer le mode publisher-confirms (irréversible pour la durée de vie de la connexion) ; faux n'est accepté que si le mode n'a jamais été activé"},
    {0}
};
static const OwlSignature sig_confirmations[] = {
    {2, p_confirmations, "booléen", "vrai si le mode publisher-confirms est actif, faux sinon"}, {0}
};

/* ============================================================
 * Method bindings — formes prefixe connexion.Xxx().
 * ============================================================ */

static const OwlMethodBinding MB_CONNECTE[] = {
    { "amqpConnection", "Connecte", 1 }, { "amqpConnection", "Connect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECONNECTE[] = {
    { "amqpConnection", "Deconnecte", 1 }, { "amqpConnection", "Disconnect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECLARE_QUEUE[] = {
    { "amqpConnection", "DeclareQueue", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECLARE_ECHANGE[] = {
    { "amqpConnection", "DeclareEchange", -1 }, { "amqpConnection", "DeclareExchange", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_LIE[] = {
    { "amqpConnection", "Lie", 4 }, { "amqpConnection", "Bind", 4 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_SUPPRIME_QUEUE[] = {
    { "amqpConnection", "SupprimeQueue", 2 }, { "amqpConnection", "DeleteQueue", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PURGE_QUEUE[] = {
    { "amqpConnection", "PurgeQueue", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DELIE[] = {
    { "amqpConnection", "Delie", 4 }, { "amqpConnection", "Unbind", 4 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_SUPPRIME_ECHANGE[] = {
    { "amqpConnection", "SupprimeEchange", 2 }, { "amqpConnection", "DeleteExchange", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PUBLIE[] = {
    { "amqpConnection", "Publie", 4 }, { "amqpConnection", "Publish", 4 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_CONSOMME[] = {
    { "amqpConnection", "Consomme", -1 }, { "amqpConnection", "Consume", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ACQUITTE[] = {
    { "amqpConnection", "Acquitte", 2 }, { "amqpConnection", "Ack", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REJETTE[] = {
    { "amqpConnection", "Rejette", 3 }, { "amqpConnection", "Reject", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PREFETCH[] = {
    { "amqpConnection", "Prefetch", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ATTEND[] = {
    { "amqpConnection", "Attend", 2 }, { "amqpConnection", "Wait", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_CONFIRMATIONS[] = {
    { "amqpConnection", "Confirmations", 2 }, { "amqpConnection", "Confirms", 2 }, { NULL, NULL, 0 }
};

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define A_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "AMQP", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_AMQP[] = {
    A_DEF("AMQPConnecte", NULL, "AMQPConnect",
        "Ouvre une connexion vers un serveur AMQP à partir des paramètres d'une connexion, et indique si la connexion a réussi.",
        "Opens a connection to an AMQP server from a connection's parameters, and reports whether the connection succeeded.",
        sig_connecte, fn_amqp_connecte, MB_CONNECTE),
    A_DEF("AMQPDéconnecte", "AMQPDeconnecte", "AMQPDisconnect",
        "Ferme une connexion AMQP ouverte.",
        "Closes an open AMQP connection.",
        sig_deconnecte, fn_amqp_deconnecte, MB_DECONNECTE),
    A_DEF("AMQPDéclareQueue", "AMQPDeclareQueue", "AMQPDeclareQueue",
        "Déclare une queue sur le serveur AMQP (la crée si besoin) et renvoie son nom effectif.",
        "Declares a queue on the AMQP server (creating it if needed) and returns its effective name.",
        sig_declare_queue, fn_amqp_declare_queue, MB_DECLARE_QUEUE),
    A_DEF("AMQPDéclareÉchange", "AMQPDeclareEchange", "AMQPDeclareExchange",
        "Déclare un échange sur le serveur AMQP (le crée si besoin) selon le type demandé.",
        "Declares an exchange on the AMQP server (creating it if needed) with the requested type.",
        sig_declare_echange, fn_amqp_declare_echange, MB_DECLARE_ECHANGE),
    A_DEF("AMQPLie", NULL, "AMQPBind",
        "Lie une queue à un échange avec une clé de routage sur le serveur AMQP.",
        "Binds a queue to an exchange with a routing key on the AMQP server.",
        sig_lie, fn_amqp_lie, MB_LIE),
    A_DEF("AMQPSupprimeQueue", NULL, "AMQPDeleteQueue",
        "Supprime une queue du serveur AMQP.",
        "Deletes a queue on the AMQP server.",
        sig_supprime_queue, fn_amqp_supprime_queue, MB_SUPPRIME_QUEUE),
    A_DEF("AMQPPurgeQueue", NULL, "AMQPPurgeQueue",
        "Vide une queue du serveur AMQP de tous ses messages en attente et renvoie le nombre de messages retirés.",
        "Empties an AMQP server queue of all its pending messages and returns the number of messages removed.",
        sig_purge_queue, fn_amqp_purge_queue, MB_PURGE_QUEUE),
    A_DEF("AMQPDélie", "AMQPDelie", "AMQPUnbind",
        "Défait la liaison entre une queue et un échange sur le serveur AMQP.",
        "Removes the binding between a queue and an exchange on the AMQP server.",
        sig_delie, fn_amqp_delie, MB_DELIE),
    A_DEF("AMQPSupprimeÉchange", "AMQPSupprimeEchange", "AMQPDeleteExchange",
        "Supprime un échange du serveur AMQP.",
        "Deletes an exchange on the AMQP server.",
        sig_supprime_echange, fn_amqp_supprime_echange, MB_SUPPRIME_ECHANGE),
    A_DEF("AMQPPublie", NULL, "AMQPPublish",
        "Publie un message vers un échange du serveur AMQP avec une clé de routage.",
        "Publishes a message to an exchange on the AMQP server with a routing key.",
        sig_publie, fn_amqp_publie, MB_PUBLIE),
    A_DEF("AMQPConsomme", NULL, "AMQPConsume",
        "Consomme une queue du serveur AMQP : la procédure fournie est appelée pour chaque message reçu pendant l'attente.",
        "Consumes a queue on the AMQP server: the supplied procedure is called for each message received while waiting.",
        sig_consomme, fn_amqp_consomme, MB_CONSOMME),
    A_DEF("AMQPAcquitte", NULL, "AMQPAck",
        "Acquitte un message reçu sur le serveur AMQP, à partir de son identifiant de livraison.",
        "Acknowledges a message received on the AMQP server, from its delivery identifier.",
        sig_acquitte, fn_amqp_acquitte, MB_ACQUITTE),
    A_DEF("AMQPRejette", NULL, "AMQPReject",
        "Rejette un message reçu sur le serveur AMQP, à partir de son identifiant de livraison, avec ou sans remise en file.",
        "Rejects a message received on the AMQP server, from its delivery identifier, with or without requeue.",
        sig_rejette, fn_amqp_rejette, MB_REJETTE),
    A_DEF("AMQPPrefetch", NULL, "AMQPPrefetch",
        "Limite le nombre de messages non acquittés que le serveur envoie d'avance à un consommateur, pour maîtriser la charge.",
        "Limits the number of unacknowledged messages the server sends ahead to a consumer, to control the load.",
        sig_prefetch, fn_amqp_prefetch, MB_PREFETCH),
    A_DEF("AMQPAttend", NULL, "AMQPWait",
        "Attend et traite les messages entrants d'une connexion AMQP pendant un délai maximal exprimé en millisecondes, en appelant les procédures de consommation à mesure que les messages arrivent ; renvoie vrai tant que la connexion reste ouverte.",
        "Waits for and processes the incoming messages of an AMQP connection for a maximum delay in milliseconds, calling the consumer procedures as messages arrive; returns true as long as the connection stays open.",
        sig_attend, fn_amqp_attend, MB_ATTEND),
    A_DEF("AMQPConfirmations", NULL, "AMQPConfirms",
        "Active le mode publisher-confirms sur une connexion AMQP : une fois activé, AMQPPublie attend l'accusé du serveur et son retour garantit que le message a bien été accepté (au lieu du simple succès d'émission).",
        "Enables publisher-confirms mode on an AMQP connection: once active, AMQPPublish waits for the server's acknowledgement and its return value guarantees the message was accepted (instead of mere send success).",
        sig_confirmations, fn_amqp_confirmations, MB_CONFIRMATIONS),
};

#undef A_DEF

/* ============================================================
 * Type reifie amqpConnection.
 *
 * Type + proprietes HORS-CANON (aucune forme canon) : noms FR/EN concus pour
 * OWL et valides dev. Bilingue par convention alignee sur mqttSession.
 * ============================================================ */

static const OwlField FIELDS_amqpConnection[] = {
    {"Adresse",          NULL,          "chaîne",  "\"\"",   "adresse du serveur AMQP (IP ou nom d'hôte)", "AMQP server address (IP or host name)", .nameEN="Address"},
    {"Port",             NULL,          "entier",  "5672",   "port du serveur (5672 en clair, 5671 en SSL)", "server port (5672 clear, 5671 SSL)", .nameEN="Port"},
    {"Utilisateur",      NULL,          "chaîne",  "\"\"",   "nom d'utilisateur pour l'authentification", "user name for authentication", .nameEN="User"},
    {"MotDePasse",       NULL,          "chaîne",  "\"\"",   "mot de passe pour l'authentification", "password for authentication", .nameEN="Password"},
    {"HoteVirtuel",      "HôteVirtuel", "chaîne",  "\"/\"",  "hôte virtuel (vhost) du serveur (\"/\" par défaut)", "virtual host (vhost) of the server (\"/\" by default)", .nameEN="VirtualHost"},
    {"Option",           NULL,          "entier",  "0",      "options de connexion (optionSSL pour une connexion chiffrée)", "connection options (optionSSL for an encrypted connection)", .nameEN="Option"},
    {"CertificatCA",     NULL,          "chaîne",  "\"\"",   "chemin d'un fichier PEM de certificat d'autorité (CA) à utiliser pour valider le serveur en TLS ; vide = autorités publiques par défaut", "path to a PEM certificate-authority (CA) file used to validate the server over TLS; empty = default public authorities", .nameEN="CACertificate"},
    {"TimeoutConnexion", NULL,          "entier",  "0",      "délai de connexion en secondes (0 = défaut)", "connection timeout in seconds (0 = default)", .nameEN="ConnectionTimeout"},
    {"Heartbeat",        NULL,          "entier",  "0",      "intervalle de battement de cœur souhaité, en secondes (0 = négocié avec le serveur)", "requested heartbeat interval, in seconds (0 = negotiated with the server)", .nameEN="Heartbeat"},
    {"NbMessages",       NULL,          "entier",  "0",      "nombre de messages en attente dans la dernière queue déclarée (mis à jour par AMQPDéclareQueue)", "number of pending messages in the last declared queue (updated by AMQPDeclareQueue)", .nameEN="MessageCount"},
    {0}
};

static const OwlField FIELDS_amqpMessage[] = {
    {"Corps",           NULL,            "chaîne",  "\"\"",  "corps du message : chaîne ou Buffer en émission, Buffer en réception", "message body: string or Buffer when sending, Buffer when receiving", .nameEN="Body"},
    {"CleRoutage",      "CléRoutage",    "chaîne",  "\"\"",  "clé de routage (renseignée en réception ; en émission, la clé est un argument de la publication)", "routing key (set on receipt; when sending, the key is a publish argument)", .nameEN="RoutingKey"},
    {"Persistant",      NULL,            "booléen", "Faux",  "vrai pour un message persistant (écrit sur disque par le serveur)", "true for a persistent message (written to disk by the server)", .nameEN="Persistent"},
    {"TypeContenu",     NULL,            "chaîne",  "\"\"",  "type de contenu MIME du corps (ex. \"application/json\")", "MIME content type of the body (e.g. \"application/json\")", .nameEN="ContentType"},
    {"EncodageContenu", NULL,            "chaîne",  "\"\"",  "encodage de contenu du corps (ex. \"utf-8\", \"gzip\")", "content encoding of the body (e.g. \"utf-8\", \"gzip\")", .nameEN="ContentEncoding"},
    {"IdCorrelation",   "IdCorrélation", "chaîne",  "\"\"",  "identifiant de corrélation, pour relier une réponse à sa demande", "correlation identifier, to link a reply to its request", .nameEN="CorrelationId"},
    {"RepondreA",       "RépondreÀ",     "chaîne",  "\"\"",  "nom de la queue où adresser une réponse", "name of the queue to which a reply should be sent", .nameEN="ReplyTo"},
    {"Priorite",        "Priorité",      "entier",  "0",     "priorité du message (0 à 9)", "message priority (0 to 9)", .nameEN="Priority"},
    {"Expiration",      NULL,            "chaîne",  "\"\"",  "durée de vie du message en millisecondes, exprimée en chaîne", "time-to-live of the message in milliseconds, expressed as a string", .nameEN="Expiration"},
    {"IdMessage",       NULL,            "chaîne",  "\"\"",  "identifiant applicatif du message", "application-level identifier of the message", .nameEN="MessageId"},
    {"Type",            NULL,            "chaîne",  "\"\"",  "type applicatif du message", "application-level type of the message", .nameEN="Type"},
    {"Identifiant",     NULL,            "entier",  "0",     "identifiant de livraison, renseigné en réception (à passer à AMQPAcquitte)", "delivery identifier, set on receipt (to pass to AMQPAck)", .nameEN="Tag"},
    {0}
};

static const OwlTypeDef TYPES_AMQP[] = {
    { .canonical      = "amqpConnection",
      .canonicalAscii = "amqpConnection",
      .aliases        = NULL,
      .category       = "AMQP",
      .descFR         = "Paramètres de connexion à un serveur AMQP (adresse, port, authentification, hôte virtuel, options). Passé à AMQPConnecte.",
      .descEN         = "Connection parameters for an AMQP server (address, port, authentication, virtual host, options). Passed to AMQPConnect.",
      .fields         = FIELDS_amqpConnection,
      .nameEN         = "amqpConnection",
      .genre          = OWL_GENRE_F },   /* la connexion */
    { .canonical      = "amqpMessage",
      .canonicalAscii = "amqpMessage",
      .aliases        = NULL,
      .category       = "AMQP",
      .descFR         = "Message AMQP : corps, persistance et propriétés (type de contenu, corrélation, priorité…). Passé à AMQPPublie et reçu depuis AMQPConsomme.",
      .descEN         = "AMQP message: body, persistence and properties (content type, correlation, priority…). Passed to AMQPPublish and received from AMQPConsume.",
      .fields         = FIELDS_amqpMessage,
      .nameEN         = "amqpMessage",
      .genre          = OWL_GENRE_M },   /* le message */
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_amqp_init(void) {
    /* Le socle Message Queue E1 est arme par stdlib_mq_init (point central
     * neutre, appele par owl_stdlib_init AVANT tout client MQ). Ce client le
     * partage avec MQTT (contrat multi-protocole). La constante optionSSL est
     * deja enregistree globalement (client MQTT) : on la reutilise. */
    owl_registry_register_module(DEFS_AMQP,
        (int)(sizeof(DEFS_AMQP) / sizeof(DEFS_AMQP[0])));
    owl_registry_register_types(TYPES_AMQP,
        (int)(sizeof(TYPES_AMQP) / sizeof(TYPES_AMQP[0])));
    owl_registry_register_enums(AMQP_ENUMS,
        (int)(sizeof(AMQP_ENUMS) / sizeof(AMQP_ENUMS[0])));
    owl_registry_register_consts(AMQP_CONSTS,
        (int)(sizeof(AMQP_CONSTS) / sizeof(AMQP_CONSTS[0])));

    ObjInstance* tpl = build_amqpConnection_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("amqpConnection", OBJ_VAL(tpl));
    vm_pop();

    ObjInstance* tplMsg = build_amqpMessage_template();
    vm_push(OBJ_VAL(tplMsg));
    vm_type_register("amqpMessage", OBJ_VAL(tplMsg));
    vm_pop();
}
