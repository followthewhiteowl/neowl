// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Client MQTT 3.1.1, Lot A. Voir stdlib_mqtt.h pour le contrat.
 *
 * ETAT : client complet (connexion, publication, abonnement). MQTTConnecte
 * ouvre le transport (TCP clair, ou TLS via optionSSL), emet un CONNECT bien
 * forme, lit le CONNACK et renvoie Vrai si le broker accepte. MQTTPublie gere
 * QoS 0/1 + retain ; MQTTAbonne/Désabonne emettent SUBSCRIBE/UNSUBSCRIBE et un
 * PUBLISH entrant invoque la procedure OWL abonnee (3 args dont Buffer) via le
 * socle E1. MQTTDéconnecte emet un DISCONNECT (0xE0 0x00) et ferme. La connexion
 * vivante est stockee dans l'instance mqttSession (champ interne _handle ->
 * OwlMqttConn*).
 *
 * La reception est pompee depuis OWL par MQTTAttend : la session
 * POSSEDE son reacteur (net_reacteur), cree paresseusement au 1er abonnement /
 * a la 1re attente, et MQTTAttend le ticke pendant un delai borne.
 *
 * Le keepalive negocie est place dans le CONNECT ; le heartbeat PINGREQ actif
 * (timer de la roue du reacteur) reste a cabler avec l'impl reelle de MQTTAttend.
 *
 * Surface canon :
 *   type mqttSession (Communication/Autres/1000023023)
 *   MQTTConnecte  -> MQTTConnect   : (mqttSession) -> booleen
 *   MQTTDéconnecte-> MQTTDisconnect: (mqttSession) -> rien
 *
 * Le wire (encodage des trames) n'est PAS dans la doc : reference =
 * spec OASIS MQTT 3.1.1.
 *   CONNECT   : 0x10 <remLen> 0x00 0x04 'M' 'Q' 'T' 'T' 0x04 <flags>
 *               <keepAliveHi> <keepAliveLo> <clientIdLen(2)> <clientId...>
 *               [<userLen(2)> <user...>] [<passLen(2)> <pass...>]
 *   CONNACK   : 0x20 0x02 <flags> <returnCode> ; accepte = returnCode 0x00
 *   DISCONNECT: 0xE0 0x00
 */

#include "stdlib_mqtt.h"

#include "owl_registry.h"
#include "owl_stdlib.h"
/* net_tcp.h tire winsock2.h sous Windows -> collision TokenType (winnt.h vs
 * scanner.h). Meme garde que stdlib_ftp.c / stdlib_sql.c. */
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

/* Ports par defaut MQTT (spec OASIS) : 1883 en clair, 8883 en TLS. */
#define MQTT_PORT_CLEAR          1883
#define MQTT_PORT_SSL            8883
/* Delai d'ouverture du transport, en millisecondes (defaut si TimeoutConnexion
 * n'est pas renseigne). */
#define MQTT_CONNECT_TIMEOUT_MS  10000
/* Delai d'attente d'un accuse (PUBACK/SUBACK) pompe via le reacteur, en
 * millisecondes. Constante dediee, alignee sur le connect (NI derivee de
 * TimeoutConnexion NI du keepalive). */
#define MQTT_ACK_TIMEOUT_MS      10000
/* Keepalive par defaut, en secondes, quand TimeoutConnexion vaut 0. */
#define MQTT_KEEPALIVE_DEFAUT    60
/* Valeur de la constante optionSSL (masque OU-binaire sur ..Option). */
#define OWL_OPTION_SSL           1

/* Horloge monotone en millisecondes, partagee avec le socle net (definie
 * dans net_tcp_server.c). Sert a borner la pompe MQTTAttend et a mesurer
 * l'oisivete keepalive. Extern-declaree comme dans net_reacteur.c pour ne
 * pas tirer winsock via net_tcp_server.h. */
extern uint64_t owl_tcp_server_horloge_ms(void);

/* ============================================================
 * Connexion vivante — pointeur opaque cache dans l'instance mqttSession
 * (champ _handle, cast intptr_t). Meme pattern que ftpClient : un seul
 * transport, dispatch send/recv par le flag tls_active.
 * ============================================================ */

/* Nombre maximal de sujets abonnes simultanement sur une session. Chaque
 * sujet mappe vers un handle E1 (lui-meme borne par OWL_MQ_MAX_ABONNEMENTS,
 * cap 8, partage GC). Depassement -> abonnement refuse (pas de mode degrade). */
#define MQTT_MAX_SUBS 8

/* Nombre maximal d'accuses (PUBACK/SUBACK/UNSUBACK) retenus en attente de
 * correlation. Une seule pompe (MQTTPublie/MQTTAbonne) attend a la fois, mais
 * le framing d'un bloc groupe peut deposer plusieurs accuses avant qu'ils ne
 * soient reclames. Table bornee : le plus ancien est evince si pleine. */
#define MQTT_ACK_MAX 16

/* Mapping sujet -> handle E1, stocke sur l'instance (via la connexion). Le
 * sujet est une copie C-heap (independante des Value OWL) : la livraison
 * retrouve le handle abonne au sujet d'un PUBLISH entrant. */
typedef struct {
    char*       sujet;   /* copie malloc (NUL-terminee) du filtre d'abonnement */
    OwlMqHandle handle;  /* handle E1 tenant le handler comme racine GC */
} MqttSub;

/* Accuse resolu par le framing : type de paquet (0x40 PUBACK, 0x90 SUBACK,
 * 0xB0 UNSUBACK) + identifiant de paquet. La pompe (mqtt_pump_until_ack)
 * scanne cette table apres chaque tick et consomme l'entree correspondante. */
typedef struct {
    uint8_t  type;
    uint16_t pid;
} MqttAck;

typedef struct {
    OwlTlsSocket transport;      /* .tcp = TCP sous-jacent (clair ou chiffre) */
    bool         tls_active;     /* handshake TLS effectue -> router via owl_tls_* */
    bool         connected;      /* CONNACK accepte */
    uint16_t     keepalive;      /* secondes ; heartbeat PINGREQ actif = differe */
    uint16_t     next_packet_id; /* compteur d'identifiants de paquet (QoS>0) */

    /* --- Abonnement (Lot C) --- */
    ObjInstance* session;        /* instance mqttSession proprietaire ; passee en
                                  * arg 1 de la livraison. Rootee par l'appelant
                                  * (portee OWL), non scannee via ce champ C. */
    MqttSub      subs[MQTT_MAX_SUBS];
    int          nb_subs;

    /* --- Correlation des accuses --- */
    /* Un seul chemin de framing (mqtt_rx_parse) nourrit MQTTAttend ET les
     * pompes d'accuse : les PUBLISH sont livres inline, les accuses (PUBACK/
     * SUBACK/UNSUBACK) sont enregistres ici au lieu d'etre avales. */
    MqttAck      acks[MQTT_ACK_MAX];
    int          nb_acks;

    /* --- Pompe possedee par la session --- */
    OwlReacteur* reacteur;       /* reacteur PROPRE a la session, cree
                                  * paresseusement (1er abonnement ou 1re
                                  * attente). MQTTAttend le ticke. */
    bool         fd_branche;     /* socket vivante deja enregistree sur
                                  * `reacteur` en interet LECTURE (une fois). */

    /* --- Keepalive PINGREQ --- */
    uint64_t     last_activity_ms; /* horloge monotone du dernier octet emis
                                    * ou recu ; 0 tant qu'aucune activite. */
    OwlTimerId   ka_timer;         /* timer one-shot de la roue du reacteur qui
                                    * emet un PINGREQ si oisif au-dela du
                                    * keepalive, puis se re-arme. N'avance que
                                    * pendant les ticks de MQTTAttend. */

    /* Tampon d'accumulation du reacteur : un recv peut couper une trame en
     * deux, ou en agreger plusieurs. On accumule puis on frame paquet par
     * paquet. Seul le CONNACK est lu de maniere synchrone (recv_exact, avant
     * tout abonnement / trafic) : il lit exactement son compte, un eventuel
     * bloc groupe (PUBACK/SUBACK a la suite) restant en socket est draine par
     * la pompe et route via la table d'accuses. */
    uint8_t*     rx;
    size_t       rx_len;
    size_t       rx_cap;
} OwlMqttConn;

static int mqtt_send(OwlMqttConn* c, const void* buf, size_t len) {
    int n = c->tls_active ? owl_tls_send(&c->transport, buf, len)
                          : owl_tcp_send(&c->transport.tcp, buf, len);
    if (n > 0) c->last_activity_ms = owl_tcp_server_horloge_ms();
    return n;
}

static bool mqtt_recv_exact(OwlMqttConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv_exact(&c->transport, buf, len)
                         : owl_tcp_recv_exact(&c->transport.tcp, buf, len);
}

/* Lecture non bloquante-friendly (jusqu'a len octets) pour la pompe :
 * route TLS/clair comme le send. >0 octets, 0 = pair ferme, <0 = erreur. */
static int mqtt_recv(OwlMqttConn* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv(&c->transport, buf, len)
                         : owl_tcp_recv(&c->transport.tcp, buf, len);
}

static void mqtt_conn_free(OwlMqttConn* c) {
    if (c == NULL) return;
    /* Relache les abonnements : handlers E1 (racines GC) + copies de sujets. */
    for (int i = 0; i < c->nb_subs; i++) {
        owl_mq_desabonne(c->subs[i].handle);
        free(c->subs[i].sujet);
    }
    /* Reacteur possede par la session : ne ferme aucun fd (contrat reacteur),
     * la socket est refermee juste apres. */
    if (c->reacteur != NULL) owl_reacteur_detruit(c->reacteur);
    free(c->rx);
    if (c->tls_active) owl_tls_close(&c->transport);
    else               owl_tcp_close(&c->transport.tcp);
    free(c);
}

/* Prochain identifiant de paquet (jamais 0, interdit comme identifiant). */
static uint16_t mqtt_next_pid(OwlMqttConn* c) {
    uint16_t pid = ++c->next_packet_id;
    if (pid == 0) pid = ++c->next_packet_id;
    return pid;
}

/* ============================================================
 * Encodage des trames MQTT 3.1.1.
 * ============================================================ */

/* Longueur restante : entier variable 1-4 octets, 7 bits utiles + bit de
 * continuation. Ecrit dans out, renvoie le nombre d'octets ecrits. */
static int mqtt_encode_remlen(uint32_t len, uint8_t* out) {
    int n = 0;
    do {
        uint8_t byte = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) byte |= 0x80;
        out[n++] = byte;
    } while (len > 0 && n < 4);
    return n;
}

/* Ecrit une chaine prefixee de sa longueur sur 2 octets (big-endian). */
static void mqtt_put_str(uint8_t* buf, int* n, const char* s) {
    size_t l = s ? strlen(s) : 0;
    buf[(*n)++] = (uint8_t)(l >> 8);
    buf[(*n)++] = (uint8_t)(l & 0xFF);
    if (l > 0) { memcpy(buf + *n, s, l); *n += (int)l; }
}

/* Emet un CONNECT. flags derives de clean_session + presence user/pass. */
static bool mqtt_send_connect(OwlMqttConn* c, const char* clientId,
                              const char* user, const char* pass,
                              bool clean_session, uint16_t keepalive) {
    if (clientId == NULL) clientId = "";
    bool has_user = user && user[0];
    bool has_pass = pass && pass[0];

    /* En-tete variable : nom de protocole "MQTT", niveau 4, flags, keepalive. */
    uint8_t vh[10];
    int vhn = 0;
    vh[vhn++] = 0x00; vh[vhn++] = 0x04;
    vh[vhn++] = 'M'; vh[vhn++] = 'Q'; vh[vhn++] = 'T'; vh[vhn++] = 'T';
    vh[vhn++] = 0x04;                        /* niveau protocole = MQTT 3.1.1 */
    uint8_t flags = 0;
    if (has_user)      flags |= 0x80;
    if (has_pass)      flags |= 0x40;
    if (clean_session) flags |= 0x02;
    vh[vhn++] = flags;
    vh[vhn++] = (uint8_t)(keepalive >> 8);
    vh[vhn++] = (uint8_t)(keepalive & 0xFF);

    int idlen = (int)strlen(clientId);
    int ulen  = has_user ? (int)strlen(user) : 0;
    int plen  = has_pass ? (int)strlen(pass) : 0;
    int payload_len = 2 + idlen + (has_user ? 2 + ulen : 0) + (has_pass ? 2 + plen : 0);
    int remlen = vhn + payload_len;

    /* 1 octet type + jusqu'a 4 octets remLen + en-tete variable + payload. */
    size_t total = 1 + 4 + (size_t)remlen;
    uint8_t* pkt = (uint8_t*)malloc(total);
    if (pkt == NULL) return false;

    int n = 0;
    pkt[n++] = 0x10;                         /* type CONNECT */
    n += mqtt_encode_remlen((uint32_t)remlen, pkt + n);
    memcpy(pkt + n, vh, (size_t)vhn); n += vhn;
    mqtt_put_str(pkt, &n, clientId);
    if (has_user) mqtt_put_str(pkt, &n, user);
    if (has_pass) mqtt_put_str(pkt, &n, pass);

    bool ok = (mqtt_send(c, pkt, (size_t)n) == n);
    free(pkt);
    return ok;
}

/* Lit un CONNACK et renvoie son return code. La longueur restante d'un
 * CONNACK vaut toujours 2 (spec 3.1.1) : on la lit sur un octet. */
static bool mqtt_read_connack(OwlMqttConn* c, uint8_t* return_code) {
    uint8_t hdr[2];
    if (!mqtt_recv_exact(c, hdr, 2)) return false;
    if ((hdr[0] & 0xF0) != 0x20) return false;   /* type CONNACK attendu */
    uint32_t rem = hdr[1];
    if (rem < 2) return false;
    uint8_t body[4];
    if (rem > sizeof(body)) rem = sizeof(body);
    if (!mqtt_recv_exact(c, body, rem)) return false;
    *return_code = body[1];                       /* body[0] = flags session, body[1] = code */
    return true;
}

static void mqtt_send_disconnect(OwlMqttConn* c) {
    uint8_t d[2] = { 0xE0, 0x00 };
    mqtt_send(c, d, 2);
}

/* Emet un PUBLISH. byte0 = 0x30 | (retain?0x01) | (QoS<<1) ; en-tete variable
 * = sujet (len+UTF-8) puis packet-id (2 octets) SI QoS>0 ; payload = donnees.
 * Renvoie le packet-id utilise (QoS>0), 0 (QoS 0), ou -1 si l'emission echoue. */
static int mqtt_send_publish(OwlMqttConn* c, const char* topic,
                             const unsigned char* payload, size_t payload_len,
                             int qos, bool retain) {
    if (topic == NULL) topic = "";
    size_t tlen = strlen(topic);

    uint16_t pid = 0;
    if (qos > 0) {
        pid = ++c->next_packet_id;
        if (pid == 0) pid = ++c->next_packet_id;  /* 0 interdit comme identifiant */
    }

    size_t vh_len = 2 + tlen + (qos > 0 ? 2 : 0);   /* sujet + packet-id eventuel */
    size_t remlen = vh_len + payload_len;

    size_t total = 1 + 4 + remlen;   /* type + remLen(<=4) + en-tete variable + payload */
    uint8_t* pkt = (uint8_t*)malloc(total);
    if (pkt == NULL) return -1;

    int n = 0;
    uint8_t b0 = 0x30;                              /* type PUBLISH, DUP=0 */
    if (retain) b0 |= 0x01;
    b0 |= (uint8_t)((qos & 0x03) << 1);
    pkt[n++] = b0;
    n += mqtt_encode_remlen((uint32_t)remlen, pkt + n);
    mqtt_put_str(pkt, &n, topic);
    if (qos > 0) {
        pkt[n++] = (uint8_t)(pid >> 8);
        pkt[n++] = (uint8_t)(pid & 0xFF);
    }
    if (payload_len > 0) { memcpy(pkt + n, payload, payload_len); n += (int)payload_len; }

    bool ok = (mqtt_send(c, pkt, (size_t)n) == n);
    free(pkt);
    if (!ok) return -1;
    return qos > 0 ? (int)pid : 0;
}

/* ============================================================
 * Abonnement — wire SUBSCRIBE / SUBACK / UNSUBSCRIBE (spec 3.1.1).
 *
 *   SUBSCRIBE   : 0x82 <remLen> <pid(2)> <filtreLen(2)> <filtre...> <QoS>
 *   SUBACK      : 0x90 <remLen> <pid(2)> <codeRetour>+
 *   UNSUBSCRIBE : 0xA2 <remLen> <pid(2)> <filtreLen(2)> <filtre...>
 * Les flags reserves du type SUBSCRIBE/UNSUBSCRIBE valent 0x02 (bit1) :
 * d'ou les octets fixes 0x82 et 0xA2.
 * ============================================================ */

static bool mqtt_send_subscribe(OwlMqttConn* c, const char* topic, int qos,
                                uint16_t pid) {
    if (topic == NULL) topic = "";
    size_t tlen = strlen(topic);
    size_t remlen = 2 + 2 + tlen + 1;              /* pid + filtreLen + filtre + QoS */
    size_t total = 1 + 4 + remlen;
    uint8_t* pkt = (uint8_t*)malloc(total);
    if (pkt == NULL) return false;

    int n = 0;
    pkt[n++] = 0x82;                               /* SUBSCRIBE, flags reserves 0x02 */
    n += mqtt_encode_remlen((uint32_t)remlen, pkt + n);
    pkt[n++] = (uint8_t)(pid >> 8);
    pkt[n++] = (uint8_t)(pid & 0xFF);
    mqtt_put_str(pkt, &n, topic);
    pkt[n++] = (uint8_t)(qos & 0x03);

    bool ok = (mqtt_send(c, pkt, (size_t)n) == n);
    free(pkt);
    return ok;
}

static bool mqtt_send_unsubscribe(OwlMqttConn* c, const char* topic,
                                  uint16_t pid) {
    if (topic == NULL) topic = "";
    size_t tlen = strlen(topic);
    size_t remlen = 2 + 2 + tlen;                  /* pid + filtreLen + filtre */
    size_t total = 1 + 4 + remlen;
    uint8_t* pkt = (uint8_t*)malloc(total);
    if (pkt == NULL) return false;

    int n = 0;
    pkt[n++] = 0xA2;                               /* UNSUBSCRIBE, flags reserves 0x02 */
    n += mqtt_encode_remlen((uint32_t)remlen, pkt + n);
    pkt[n++] = (uint8_t)(pid >> 8);
    pkt[n++] = (uint8_t)(pid & 0xFF);
    mqtt_put_str(pkt, &n, topic);

    bool ok = (mqtt_send(c, pkt, (size_t)n) == n);
    free(pkt);
    return ok;
}

/* ============================================================
 * Mapping sujet -> handle E1 (par connexion).
 * ============================================================ */

/* Egalite d'octets exacte filtre <-> sujet. Sert au DEDUP d'abonnement et au
 * desabonnement, qui operent sur la CHAINE DE FILTRE (jamais un wildcard
 * developpe) : `owl/#` s'y compare a `owl/#`, pas a `owl/x`. */
static int mqtt_sub_index(OwlMqttConn* c, const char* sujet, size_t len) {
    for (int i = 0; i < c->nb_subs; i++) {
        if (strlen(c->subs[i].sujet) == len &&
            memcmp(c->subs[i].sujet, sujet, len) == 0) return i;
    }
    return -1;
}

/* Matching de filtre de sujet MQTT 3.1.1 §4.7 (chemin de LIVRAISON).
 *
 * `filtre` est NUL-termine (chaine d'abonnement) ; `sujet` pointe dans le
 * tampon rx sur `sujet_len` octets et n'est PAS forcement NUL-termine.
 * Decoupage par '/' en niveaux :
 *   - '+'  : matche exactement UN niveau (quelconque, vide compris) ;
 *   - '#'  : dernier segment du filtre ; matche le niveau parent ET tous ses
 *            descendants (`sport/#` matche `sport/x`, `sport/x/y` ET `sport`) ;
 *            `#` seul matche tout ;
 *   - litteral : egalite stricte de la sous-chaine de niveau.
 * Sans wildcard, degenere en egalite exacte. */
bool mqtt_topic_filter_match(const char* filtre, const char* sujet, size_t sujet_len) {
    const char* f     = filtre;
    const char* s     = sujet;
    const char* s_end = sujet + sujet_len;
    bool f_reste = true;   /* un niveau de filtre reste a consommer */
    bool s_reste = true;   /* un niveau de sujet reste a consommer */

    while (f_reste) {
        const char* fseg = f;
        while (*f != '\0' && *f != '/') f++;
        size_t flen = (size_t)(f - fseg);

        /* '#' : le parent et tout le sous-arbre matchent, quel que soit le
         * reste du sujet (y compris vide) -> match immediat. */
        if (flen == 1 && fseg[0] == '#') return true;

        /* Le filtre attend encore un niveau litteral ou '+' mais le sujet est
         * epuise -> le sujet est moins profond que le filtre. */
        if (!s_reste) return false;

        const char* sseg = s;
        while (s < s_end && *s != '/') s++;
        size_t slen = (size_t)(s - sseg);

        /* '+' accepte n'importe quel niveau unique ; sinon egalite stricte. */
        if (!(flen == 1 && fseg[0] == '+')) {
            if (flen != slen || memcmp(fseg, sseg, flen) != 0) return false;
        }

        if (*f == '/') { f++; f_reste = true; } else { f_reste = false; }
        if (s < s_end) { s++;  s_reste = true; } else { s_reste = false; }
    }

    /* Filtre epuise : match ssi le sujet l'est aussi (sinon sujet plus profond). */
    return !s_reste;
}

static OwlMqHandle mqtt_sub_lookup(OwlMqttConn* c, const char* sujet, size_t len) {
    /* Livraison : router le sujet concret du PUBLISH vers le PREMIER filtre
     * abonne qui le matche (wildcards compris). L'egalite exacte reste un
     * sous-cas (filtre sans wildcard). */
    for (int i = 0; i < c->nb_subs; i++) {
        if (mqtt_topic_filter_match(c->subs[i].sujet, sujet, len))
            return c->subs[i].handle;
    }
    return OWL_MQ_HANDLE_NUL;
}

static bool mqtt_sub_add(OwlMqttConn* c, const char* sujet, OwlMqHandle h) {
    if (c->nb_subs >= MQTT_MAX_SUBS) return false;
    char* copie = strdup(sujet ? sujet : "");
    if (copie == NULL) return false;
    c->subs[c->nb_subs].sujet  = copie;
    c->subs[c->nb_subs].handle = h;
    c->nb_subs++;
    return true;
}

static void mqtt_sub_remove(OwlMqttConn* c, int idx) {
    if (idx < 0 || idx >= c->nb_subs) return;
    free(c->subs[idx].sujet);
    c->subs[idx] = c->subs[c->nb_subs - 1];        /* comble le trou (ordre libre) */
    c->nb_subs--;
}

/* ============================================================
 * Table de correlation des accuses.
 * ============================================================ */

/* Enregistre un accuse resolu par le framing. Ordre FIFO : si la table est
 * pleine, on evince le plus ancien (decalage vers le bas). */
static void mqtt_ack_push(OwlMqttConn* c, uint8_t type, uint16_t pid) {
    if (c->nb_acks >= MQTT_ACK_MAX) {
        memmove(&c->acks[0], &c->acks[1], (MQTT_ACK_MAX - 1) * sizeof(MqttAck));
        c->nb_acks = MQTT_ACK_MAX - 1;
    }
    c->acks[c->nb_acks].type = type;
    c->acks[c->nb_acks].pid  = pid;
    c->nb_acks++;
}

/* Consomme (retire) l'accuse {type, pid} s'il est present. Renvoie vrai sur
 * match. Preserve l'ordre relatif des accuses restants. */
static bool mqtt_ack_take(OwlMqttConn* c, uint8_t type, uint16_t pid) {
    for (int i = 0; i < c->nb_acks; i++) {
        if (c->acks[i].type == type && c->acks[i].pid == pid) {
            memmove(&c->acks[i], &c->acks[i + 1],
                    (size_t)(c->nb_acks - i - 1) * sizeof(MqttAck));
            c->nb_acks--;
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Livraison C -> OWL : (session, sujet chaine, message Buffer).
 *
 * MQTT fait sa PROPRE livraison (3 args, message binaire = Buffer), et
 * ne reutilise E1 que pour le registre + le rooting GC des handlers. On
 * respecte la frontiere de reentrance bornee (motif E1/http_exec) : repere
 * stackTop -> push callee + args (chacun roote a la creation) -> appel ->
 * reset au repere.
 * ============================================================ */

static void mqtt_livre(OwlMqttConn* c, const char* sujet, size_t sujet_len,
                       const unsigned char* charge, size_t charge_len) {
    OwlMqHandle h = mqtt_sub_lookup(c, sujet, sujet_len);
    if (h == OWL_MQ_HANDLE_NUL) return;
    Value handler;
    if (!owl_mq_handler(h, &handler)) return;
    if (c->session == NULL) return;

    Value* repere = vm.stackTop;                   /* frontiere de depilage propre */
    vm_push(handler);                              /* callee au-dessus du repere */
    vm_push(OBJ_VAL(c->session));                  /* arg 1 : session */
    /* ObjString/ObjBuffer frais : push immediat en racine GC avant toute autre
     * allocation (gc-root-obj-fresh). Le message est un Buffer (binaire). */
    ObjString* suj = obj_copy_string(sujet, (int)sujet_len);
    vm_push(OBJ_VAL(suj));                          /* arg 2 : sujet chaine */
    ObjBuffer* buf = obj_new_buffer(charge, (int)charge_len);
    vm_push(OBJ_VAL(buf));                          /* arg 3 : message Buffer */

    (void)vm_call_reentrant(handler, 3);

    vm.stackTop = repere;                          /* rejette tout residu de retour */
}

/* Traite une trame PUBLISH entrante (pointe le paquet complet en memoire :
 * `pkt`, en-tete fixe de `header` octets, `remlen` octets restants). */
static void mqtt_traite_publish(OwlMqttConn* c, const uint8_t* pkt,
                                size_t header, uint32_t remlen, uint8_t byte0) {
    int qos = (byte0 >> 1) & 0x03;
    const uint8_t* p   = pkt + header;
    const uint8_t* end = p + remlen;
    if (p + 2 > end) return;
    size_t tlen = ((size_t)p[0] << 8) | p[1];
    p += 2;
    if (p + tlen > end) return;
    const char* sujet = (const char*)p;
    p += tlen;

    uint16_t pid = 0;
    if (qos > 0) {
        if (p + 2 > end) return;
        pid = (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
    }
    size_t charge_len = (size_t)(end - p);

    mqtt_livre(c, sujet, tlen, p, charge_len);

    if (qos == 1) {                                /* accuse le PUBLISH entrant */
        uint8_t puback[4] = { 0x40, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
        mqtt_send(c, puback, 4);
    }
}

static void mqtt_rx_append(OwlMqttConn* c, const uint8_t* data, size_t len) {
    if (c->rx_len + len > c->rx_cap) {
        size_t ncap = c->rx_cap ? c->rx_cap : 256;
        while (ncap < c->rx_len + len) ncap *= 2;
        uint8_t* nb = (uint8_t*)realloc(c->rx, ncap);
        if (nb == NULL) return;                    /* best-effort : on ignore le bloc */
        c->rx = nb;
        c->rx_cap = ncap;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
}

/* Frame tous les paquets complets accumules dans `rx`, dispatche les PUBLISH
 * inline, ENREGISTRE les accuses (PUBACK/SUBACK/UNSUBACK) dans la table de
 * correlation pour les pompes, avale PINGRESP, puis compacte le reliquat. Un
 * seul chemin de framing nourrit MQTTAttend ET les pompes d'accuse. */
static void mqtt_rx_parse(OwlMqttConn* c) {
    size_t off = 0;
    while (c->rx_len - off >= 2) {
        /* Decode la longueur restante (varint 1-4 octets) a partir de off+1. */
        uint32_t remlen = 0;
        uint32_t mult = 1;
        size_t   i = off + 1;
        int      nbytes = 0;
        bool     complete = false;
        while (i < c->rx_len && nbytes < 4) {
            uint8_t b = c->rx[i++];
            remlen += (uint32_t)(b & 0x7F) * mult;
            mult *= 128;
            nbytes++;
            if ((b & 0x80) == 0) { complete = true; break; }
        }
        if (!complete) break;                      /* remLen incomplet : attendre */

        size_t hdr   = 1 + (size_t)nbytes;
        size_t total = hdr + remlen;
        if (c->rx_len - off < total) break;        /* paquet incomplet : attendre */

        uint8_t byte0 = c->rx[off];
        uint8_t type  = byte0 & 0xF0;
        if (type == 0x30) {                        /* PUBLISH : livre inline */
            mqtt_traite_publish(c, c->rx + off, hdr, remlen, byte0);
        } else if (type == 0x40 || type == 0x90 || type == 0xB0) {
            /* PUBACK / SUBACK / UNSUBACK : le packet-id est les 2 premiers
             * octets de l'en-tete variable. Enregistre pour la pompe. */
            if (remlen >= 2) {
                uint16_t pid = (uint16_t)((c->rx[off + hdr] << 8) |
                                          c->rx[off + hdr + 1]);
                mqtt_ack_push(c, type, pid);
            }
        }
        /* PINGRESP (0xD0) et autres types non attendus : avales. */
        off += total;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/* Callback de lecture du reacteur : draine la socket, accumule, frame. */
static void mqtt_reacteur_lecture_cb(OwlReacteur* r, owl_socket_fd_t fd,
                                     short evts_prets, void* user) {
    OwlMqttConn* c = (OwlMqttConn*)user;

    if (evts_prets & OWL_REAC_LECTURE) {
        uint8_t tampon[4096];
        int n = mqtt_recv(c, tampon, sizeof(tampon));
        if (n > 0) {
            c->last_activity_ms = owl_tcp_server_horloge_ms();
            mqtt_rx_append(c, tampon, (size_t)n);
            mqtt_rx_parse(c);
            return;
        }
        owl_reacteur_retire_fd(r, fd);             /* EOF (n==0) ou erreur (n<0) */
        c->connected = false;                      /* broker a raccroche */
        c->fd_branche = false;
        return;
    }
    if (evts_prets & OWL_REAC_ERREUR) {
        owl_reacteur_retire_fd(r, fd);
        c->connected = false;
        c->fd_branche = false;
    }
}

/* Cree paresseusement le reacteur possede par la session et y branche la
 * socket vivante en interet LECTURE (une seule fois). La pompe (MQTTAttend)
 * tickera ce reacteur : le callback frame les PUBLISH entrants et livre au
 * handler abonne. false si allocation impossible ou fd invalide. */
static bool mqtt_reacteur_assure(OwlMqttConn* c) {
    if (c->reacteur == NULL) {
        c->reacteur = owl_reacteur_cree();
        if (c->reacteur == NULL) return false;
    }
    if (!c->fd_branche) {
        if (!owl_reacteur_ajoute_fd(c->reacteur, c->transport.tcp.fd,
                                    OWL_REAC_LECTURE, mqtt_reacteur_lecture_cb, c))
            return false;
        c->fd_branche = true;
    }
    return true;
}

/* Pompe le reacteur possede par la session jusqu'a ce que l'accuse {type, pid}
 * soit resolu, ou que la session se deconnecte, ou que `timeout_ms` s'ecoule.
 * Le framing (mqtt_rx_parse) livre les PUBLISH inline pendant l'attente et
 * depose les accuses dans la table de correlation. Renvoie vrai sur match. */
static bool mqtt_pump_until_ack(OwlMqttConn* c, uint8_t type, uint16_t pid,
                                int timeout_ms) {
    if (!mqtt_reacteur_assure(c)) return false;
    /* L'accuse peut deja etre tamponne (bloc groupe frame par un tick anterieur,
     * ex. SUBACK + PUBACK lus ensemble). */
    if (mqtt_ack_take(c, type, pid)) return true;

    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) return false;                   /* broker a raccroche */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)timeout_ms) return false;  /* timeout */
        int reste = (int)((uint64_t)timeout_ms - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) return false;  /* poll casse */
        if (mqtt_ack_take(c, type, pid)) return true;
    }
}

/* Timer keepalive one-shot de la roue du reacteur : re-arme sur l'intervalle
 * keepalive et emet un PINGREQ (0xC0 0x00) si la connexion est restee oisive
 * au-dela de cet intervalle. Comme tout timer de la roue, il n'avance que
 * pendant les ticks de MQTTAttend (coherent worker) : pas de heartbeat en
 * dehors d'une pompe active. PINGRESP (0xD0) est avale par mqtt_rx_parse. */
static void mqtt_keepalive_timer_cb(OwlReacteur* r, OwlTimerId id, void* user) {
    (void)id;
    OwlMqttConn* c = (OwlMqttConn*)user;
    if (!c->connected) { c->ka_timer = OWL_TIMER_NUL; return; }

    uint64_t ka_ms = (uint64_t)c->keepalive * 1000u;
    if (ka_ms == 0) { c->ka_timer = OWL_TIMER_NUL; return; }  /* keepalive desactive */

    uint64_t oisif = owl_tcp_server_horloge_ms() - c->last_activity_ms;
    if (oisif >= ka_ms) {
        static const uint8_t PINGREQ[2] = { 0xC0, 0x00 };
        mqtt_send(c, PINGREQ, sizeof(PINGREQ));  /* met a jour last_activity_ms */
    }
    /* Ré-arme un timer frais : la prochaine echeance est un intervalle plein
     * apres l'activite courante (l'oisivete se remesure a chaque expiration). */
    c->ka_timer = owl_reacteur_arme_timer(r, ka_ms, mqtt_keepalive_timer_cb, c);
}

/* Arme le timer keepalive s'il ne l'est pas deja et que le keepalive est actif.
 * Appelee au demarrage d'une pompe : le heartbeat ne vit que sous MQTTAttend. */
static void mqtt_keepalive_assure(OwlMqttConn* c) {
    if (c->ka_timer != OWL_TIMER_NUL) return;
    if (c->keepalive == 0 || c->reacteur == NULL) return;
    if (c->last_activity_ms == 0) c->last_activity_ms = owl_tcp_server_horloge_ms();
    c->ka_timer = owl_reacteur_arme_timer(c->reacteur, (uint64_t)c->keepalive * 1000u,
                                          mqtt_keepalive_timer_cb, c);
}

/* ============================================================
 * Template du type reifie mqttSession.
 *
 * Struct de parametres de connexion (scalaires) — meme pattern que
 * AWSSession / httpRequête : un template clone a chaque `s est un
 * mqttSession`. Les cles de storage sont les formes ASCII des champs.
 * ============================================================ */

static ObjInstance* build_mqttSession_template(void) {
    ObjString* name = obj_copy_string("mqttSession", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_pop();

    inst_field_set_str (tpl, "Adresse",               "");
    inst_field_set_int (tpl, "Port",                  MQTT_PORT_CLEAR);  /* 8883 en SSL */
    inst_field_set_str (tpl, "IDClient",              "");
    inst_field_set_str (tpl, "Utilisateur",           "");
    inst_field_set_str (tpl, "MotDePasseUtilisateur", "");
    inst_field_set_str (tpl, "ClePrivee",             "");
    inst_field_set_str (tpl, "MotDePasseClePrivee",   "");
    inst_field_set_int (tpl, "Option",                0);
    inst_field_set_str (tpl, "CertificatCA",          "");   /* CA privee optionnelle pour TLS */
    inst_field_set_bool(tpl, "SessionVide",           false);
    inst_field_set_int (tpl, "TimeoutConnexion",      0);
    /* _handle : pointeur opaque vers OwlMqttConn* (cast intptr_t). 0 = non
     * connecte. Rempli par MQTTConnecte, libere par MQTTDéconnecte. Field
     * interne, non documente cote user. */
    inst_field_set_int (tpl, "_handle",               0);
    return tpl;
}

/* ============================================================
 * Helpers — extraction mqttSession + lecture de champs + handle.
 * ============================================================ */

static ObjInstance* resolve_mqtt_session(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "mqttSession",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "mqttSession") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "mqttSession",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static const char* session_str(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_STRING(v)) return "";
    return AS_CSTRING(v);
}

static int session_int(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return 0;
    return (int)AS_INTEGER(v);
}

static bool session_bool(ObjInstance* inst, const char* champ) {
    Value v;
    ObjString* k = obj_copy_string(champ, (int)strlen(champ));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_BOOL(v)) return false;
    return AS_BOOL(v);
}

static OwlMqttConn* session_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&inst->fields, k, &v);
    vm_pop();
    if (!ok || !IS_INTEGER(v)) return NULL;
    return (OwlMqttConn*)(intptr_t)AS_INTEGER(v);
}

static void session_set_handle(ObjInstance* inst, OwlMqttConn* c) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)c);
}

/* ============================================================
 * Natives — Lot A green.
 * ============================================================ */

static Value fn_mqtt_connecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTConnecte");
    if (inst == NULL) return BOOL_VAL(false);

    /* Reconnexion : libere une connexion vivante residuelle avant d'en
     * rouvrir une (idempotence cote utilisateur). */
    OwlMqttConn* old = session_handle(inst);
    if (old != NULL) { mqtt_conn_free(old); session_set_handle(inst, NULL); }

    const char* adresse  = session_str(inst, "Adresse");
    const char* clientId = session_str(inst, "IDClient");
    const char* user     = session_str(inst, "Utilisateur");
    const char* pass     = session_str(inst, "MotDePasseUtilisateur");
    int option  = session_int(inst, "Option");
    int timeout = session_int(inst, "TimeoutConnexion");
    bool clean  = session_bool(inst, "SessionVide");
    bool ssl    = (option & OWL_OPTION_SSL) != 0;

    /* Port : explicite si renseigne, sinon defaut selon le chiffrement.
     * SSL avec le port clair par defaut (1883) -> bascule 8883. */
    int port = session_int(inst, "Port");
    if (port <= 0) {
        port = ssl ? MQTT_PORT_SSL : MQTT_PORT_CLEAR;
    } else if (ssl && port == MQTT_PORT_CLEAR) {
        port = MQTT_PORT_SSL;
    }

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", adresse, port);

    OwlMqttConn* c = (OwlMqttConn*)calloc(1, sizeof(OwlMqttConn));
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                              "native", "MQTTConnecte", "cible", cible,
                              "detail", "mémoire insuffisante", NULL);
        return BOOL_VAL(false);
    }

    owl_tcp_global_init();
    owl_tls_init(&c->transport);
    int tmo = timeout > 0 ? timeout * 1000 : MQTT_CONNECT_TIMEOUT_MS;

    if (ssl) {
        /* CA privee optionnelle : si CertificatCA est renseigne, il devient la
         * racine de confiance ; vide -> NULL (bundle Mozilla). Verification
         * hostname+chaine conservee (VERIFY_FULL) dans les deux cas. La chaine
         * du champ d'instance vit pendant le connect (PEM parse a la volee). */
        const char* ca = session_str(inst, "CertificatCA");
        const char* extra_ca = (ca && ca[0] != '\0') ? ca : NULL;
        if (!owl_tls_connect(&c->transport, adresse, port, OWL_TLS_VERIFY_FULL,
                             tmo, extra_ca)) {
            owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                                  "native", "MQTTConnecte", "cible", cible,
                                  "detail", owl_tls_last_error(&c->transport), NULL);
            mqtt_conn_free(c);
            return BOOL_VAL(false);
        }
        c->tls_active = true;
    } else {
        if (!owl_tcp_connect(&c->transport.tcp, adresse, port, tmo)) {
            owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                                  "native", "MQTTConnecte", "cible", cible,
                                  "detail", owl_tcp_last_error(&c->transport.tcp), NULL);
            mqtt_conn_free(c);
            return BOOL_VAL(false);
        }
    }

    /* Keepalive negocie : TimeoutConnexion (secondes) ou defaut. */
    int ka = timeout > 0 ? timeout : MQTT_KEEPALIVE_DEFAUT;
    if (ka > 65535) ka = 65535;
    c->keepalive = (uint16_t)ka;

    if (!mqtt_send_connect(c, clientId, user, pass, clean, c->keepalive)) {
        owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                              "native", "MQTTConnecte", "cible", cible,
                              "detail", "émission du CONNECT impossible", NULL);
        mqtt_conn_free(c);
        return BOOL_VAL(false);
    }

    uint8_t rc = 0xFF;
    if (!mqtt_read_connack(c, &rc)) {
        owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                              "native", "MQTTConnecte", "cible", cible,
                              "detail", "aucun CONNACK reçu du broker", NULL);
        mqtt_conn_free(c);
        return BOOL_VAL(false);
    }
    if (rc != 0x00) {
        char detail[64];
        snprintf(detail, sizeof(detail), "connexion refusée (code CONNACK %d)", rc);
        owl_emit_native_error(E_NATIVE_MQTT_CONNECT_FAILURE, "MQTTConnecte",
                              "native", "MQTTConnecte", "cible", cible,
                              "detail", detail, NULL);
        mqtt_conn_free(c);
        return BOOL_VAL(false);
    }

    c->connected = true;
    c->session = inst;              /* arg 1 des livraisons ; instance rootee OWL */
    session_set_handle(inst, c);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_mqtt_deconnecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTDéconnecte");
    if (inst == NULL) return NIL_VAL;
    OwlMqttConn* c = session_handle(inst);
    if (c != NULL) {
        if (c->connected) mqtt_send_disconnect(c);
        mqtt_conn_free(c);
        session_set_handle(inst, NULL);
    }
    return NIL_VAL;
}

/* MQTTPublie(<session>, <sujet>, <données> [, <à conserver> [, <qualité>
 * [, <procédure>]]]) -> entier. Retour canon : numéro du message (QoS 1),
 * 0 si réussite (QoS 0), -1 en cas d'erreur.
 *
 * QoS 1 est synchrone (modèle pompe) : la fonction rend la main une fois le
 * PUBACK reçu. Le paramètre <procédure> (callback de diffusion) est de ce fait
 * redondant avec la valeur de retour en publication synchrone ; son invocation
 * native->OWL (E1) est différée. */
static Value fn_mqtt_publie(int argCount, Value* args) {
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTPublie");
    if (inst == NULL) return INTEGER_VAL(-1);

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_MQTT_PUBLISH_FAILURE, "MQTTPublie",
                              "native", "MQTTPublie",
                              "detail", "session non connectée", NULL);
        return INTEGER_VAL(-1);
    }

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "MQTTPublie",
                              "native", "MQTTPublie", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return INTEGER_VAL(-1);
    }
    const char* topic = AS_CSTRING(args[1]);

    /* Données : chaîne (octets UTF-8) ou Buffer (binaire). */
    const unsigned char* payload;
    size_t payload_len;
    if (IS_STRING(args[2])) {
        payload     = (const unsigned char*)AS_STRING(args[2])->chars;
        payload_len = (size_t)AS_STRING(args[2])->length;
    } else if (IS_BUFFER(args[2])) {
        payload     = AS_BUFFER(args[2])->bytes;
        payload_len = (size_t)AS_BUFFER(args[2])->length;
    } else {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "MQTTPublie",
                              "native", "MQTTPublie", "arg", "3",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return INTEGER_VAL(-1);
    }

    bool retain = (argCount >= 4 && IS_BOOL(args[3])) ? AS_BOOL(args[3]) : false;

    int qos = (argCount >= 5 && IS_INTEGER(args[4])) ? (int)AS_INTEGER(args[4]) : 0;
    if (qos < 0) qos = 0;
    if (qos > 1) {
        /* Qualité « exactement une fois » (QoS 2) : mécanique dédiée
         * (PUBREC/PUBREL/PUBCOMP) pas encore implémentée. */
        owl_emit_native_error(E_NATIVE_MQTT_PUBLISH_FAILURE, "MQTTPublie",
                              "native", "MQTTPublie",
                              "detail", "qualité de service « exactement une fois » non prise en charge", NULL);
        return INTEGER_VAL(-1);
    }

    int num = mqtt_send_publish(c, topic, payload, payload_len, qos, retain);
    if (num < 0) {
        owl_emit_native_error(E_NATIVE_MQTT_PUBLISH_FAILURE, "MQTTPublie",
                              "native", "MQTTPublie",
                              "detail", "émission du PUBLISH impossible", NULL);
        return INTEGER_VAL(-1);
    }

    /* QoS 1 : le PUBACK est route par le framing (pas de lecture synchrone,
     * incompatible avec une connexion abonnee dont les PUBLISH entrants
     * s'entrelacent). On assure le reacteur puis on pompe jusqu'a l'accuse. */
    if (qos == 1) {
        if (!mqtt_reacteur_assure(c) ||
            !mqtt_pump_until_ack(c, 0x40, (uint16_t)num, MQTT_ACK_TIMEOUT_MS)) {
            owl_emit_native_error(E_NATIVE_MQTT_PUBLISH_FAILURE, "MQTTPublie",
                                  "native", "MQTTPublie",
                                  "detail", "PUBACK absent ou incohérent", NULL);
            return INTEGER_VAL(-1);
        }
    }

    owl_clear_error();
    return INTEGER_VAL(num);
}

/* ============================================================
 * Abonnement — Lot C.
 *
 * MQTTAbonne enregistre la procedure OWL via E1 (handle rooté GC), emet un
 * SUBSCRIBE, memorise le mapping sujet->handle et branche le reacteur, PUIS
 * pompe jusqu'au SUBACK (route par le framing, pas de lecture synchrone : une
 * connexion abonnee recoit des PUBLISH entrants entrelaces). Le mapping etant
 * pose avant la pompe, un PUBLISH retenu entrelace au SUBACK est livre au
 * handler. La livraison des PUBLISH entrants passe par la pompe : le reacteur
 * possede par la session est ticke par MQTTAttend. MQTT fait sa propre
 * livraison 3 args (session, sujet, message Buffer) ; E1 ne sert qu'au
 * registre + GC.
 * ============================================================ */

static Value fn_mqtt_abonne(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTAbonne");
    if (inst == NULL) return BOOL_VAL(false);

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTAbonne",
                              "native", "MQTTAbonne",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "MQTTAbonne",
                              "native", "MQTTAbonne", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* sujet = AS_CSTRING(args[1]);

    int qos = IS_INTEGER(args[2]) ? (int)AS_INTEGER(args[2]) : 0;
    if (qos < 0) qos = 0;
    if (qos > 2) qos = 2;

    /* Enregistre le handler via E1 : valide qu'il est appelable et le tient
     * comme racine GC. Refus (non appelable / registre plein) -> Faux, avant
     * toute emission sur le fil. */
    OwlMqHandle h = owl_mq_abonne(args[3]);
    if (h == OWL_MQ_HANDLE_NUL) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "MQTTAbonne",
                              "native", "MQTTAbonne", "arg", "4",
                              "expected", "procédure", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }

    uint16_t pid = mqtt_next_pid(c);
    if (!mqtt_send_subscribe(c, sujet, qos, pid)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTAbonne",
                              "native", "MQTTAbonne",
                              "detail", "émission du SUBSCRIBE impossible", NULL);
        return BOOL_VAL(false);
    }

    if (!mqtt_sub_add(c, sujet, h)) {
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTAbonne",
                              "native", "MQTTAbonne",
                              "detail", "nombre maximal d'abonnements atteint sur la session", NULL);
        return BOOL_VAL(false);
    }

    /* Reacteur possede par la session : cree + fd branche AVANT la pompe. Le
     * mapping sujet->handle est deja pose (mqtt_sub_add) : un PUBLISH retenu
     * entrelace au SUBACK est ainsi livre au handler, pas droppe. Le SUBACK
     * est route par le framing (pas de lecture synchrone). */
    mqtt_reacteur_assure(c);

    if (!mqtt_pump_until_ack(c, 0x90, pid, MQTT_ACK_TIMEOUT_MS)) {
        /* SUBACK absent : rollback complet (mapping local + handle E1). */
        int idx = mqtt_sub_index(c, sujet, strlen(sujet));
        if (idx >= 0) mqtt_sub_remove(c, idx);
        owl_mq_desabonne(h);
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTAbonne",
                              "native", "MQTTAbonne",
                              "detail", "aucun SUBACK reçu du broker", NULL);
        return BOOL_VAL(false);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_mqtt_desabonne(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTDésabonne");
    if (inst == NULL) return BOOL_VAL(false);

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTDésabonne",
                              "native", "MQTTDésabonne",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    if (!IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "MQTTDésabonne",
                              "native", "MQTTDésabonne", "arg", "2",
                              "expected", "chaîne", "actual", "un autre type", NULL);
        return BOOL_VAL(false);
    }
    const char* sujet = AS_CSTRING(args[1]);

    /* Fire-and-forget : on emet l'UNSUBSCRIBE et on rend Vrai sans attendre
     * l'UNSUBACK (avale par la pompe). Le mapping local est retire. */
    mqtt_send_unsubscribe(c, sujet, mqtt_next_pid(c));

    int idx = mqtt_sub_index(c, sujet, strlen(sujet));
    if (idx >= 0) {
        owl_mq_desabonne(c->subs[idx].handle);
        mqtt_sub_remove(c, idx);
    }

    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_mqtt_desabonne_tout(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTDésabonneTout");
    if (inst == NULL) return BOOL_VAL(false);

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) {
        owl_emit_native_error(E_NATIVE_MQTT_SUBSCRIBE_FAILURE, "MQTTDésabonneTout",
                              "native", "MQTTDésabonneTout",
                              "detail", "session non connectée", NULL);
        return BOOL_VAL(false);
    }

    for (int i = 0; i < c->nb_subs; i++) {
        mqtt_send_unsubscribe(c, c->subs[i].sujet, mqtt_next_pid(c));
        owl_mq_desabonne(c->subs[i].handle);
        free(c->subs[i].sujet);
    }
    c->nb_subs = 0;

    owl_clear_error();
    return BOOL_VAL(true);
}

/* MQTTAttend(<session>, <délai>) -> booléen. Pompe le reacteur possede par la
 * session pendant `délai` ms au plus : dispatche les PUBLISH entrants aux
 * procedures abonnees, honore le keepalive, puis rend la main. Renvoie Vrai
 * tant que la session reste connectee, Faux si le broker a raccroche pendant
 * l'attente. Idiome worker : TANTQUE MQTTAttend(s, 1000) ... FIN.
 *
 * Boucle de pompe : ticke le reacteur de la session en bornant chaque tick au
 * temps restant (horloge monotone owl_tcp_server_horloge_ms), jusqu'a ce que
 * `délai` soit ecoule OU que la session se deconnecte (le callback de lecture
 * pose connected=false sur EOF/erreur du recv). Le keepalive PINGREQ vit sous
 * cette pompe via un timer de la roue. Retour = c->connected apres pompage. */
static Value fn_mqtt_attend(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_mqtt_session(args[0], "MQTTAttend");
    if (inst == NULL) return BOOL_VAL(false);

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) return BOOL_VAL(false);

    int64_t delai = IS_INTEGER(args[1]) ? AS_INTEGER(args[1]) : 0;
    if (delai < 0) delai = 0;

    /* Assure le reacteur (fd branche en LECTURE) et le heartbeat keepalive. */
    if (!mqtt_reacteur_assure(c)) return BOOL_VAL(c->connected);
    mqtt_keepalive_assure(c);

    uint64_t debut = owl_tcp_server_horloge_ms();
    for (;;) {
        if (!c->connected) break;                       /* broker a raccroche */
        uint64_t ecoule = owl_tcp_server_horloge_ms() - debut;
        if (ecoule >= (uint64_t)delai) break;           /* fenetre epuisee */
        int reste = (int)((uint64_t)delai - ecoule);
        if (owl_reacteur_tick(c->reacteur, reste) < 0) break;  /* poll casse */
    }
    return BOOL_VAL(c->connected);
}

/* Reacteur possede par la session (cree paresseusement, fd branche). Seam de
 * test C-interne : rend le reacteur pour un tick manuel a bas niveau. La
 * surface utilisateur de pompe est la native MQTTAttend. NULL si la session
 * n'est pas connectee ou si le reacteur ne peut etre cree/branche. */
struct OwlReacteur* owl_mqtt_reacteur_session(Value session) {
    if (!IS_INSTANCE(session)) return NULL;
    ObjInstance* inst = AS_INSTANCE(session);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "mqttSession") != 0) return NULL;

    OwlMqttConn* c = session_handle(inst);
    if (c == NULL || !c->connected) return NULL;
    if (!mqtt_reacteur_assure(c)) return NULL;
    return c->reacteur;
}

/* Teardown C : referme la socket vivante sans emettre de DISCONNECT.
 * Reutilise le chemin de liberation du Lot A. Reel des maintenant (la
 * connexion est etablie par MQTTConnecte, deja green). */
void owl_mqtt_ferme(Value session) {
    if (!IS_INSTANCE(session)) return;
    ObjInstance* inst = AS_INSTANCE(session);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "mqttSession") != 0) return;
    OwlMqttConn* c = session_handle(inst);
    if (c != NULL) { mqtt_conn_free(c); session_set_handle(inst, NULL); }
}

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_session[] = {
    {"session", "mqttSession", "session MQTT décrivant les paramètres de connexion"}, {0}
};
static const OwlSignature sig_connecte[] = {
    {1, p_session, "booléen", "vrai si la connexion au broker a réussi, faux sinon"}, {0}
};
static const OwlSignature sig_deconnecte[] = {
    {1, p_session, "rien", NULL}, {0}
};

static const OwlParam p_publie[] = {
    {"session",    "mqttSession", "session MQTT connectée au broker"},
    {"sujet",      "chaîne",      "sujet (topic) sur lequel publier le message"},
    {"données",    "chaîne",      "données du message à publier"},
    {"àConserver", "booléen",     "vrai pour que le broker conserve le message (optionnel, faux par défaut)"},
    {"qualité",    "entier",      "qualité de service souhaitée (optionnel : mqttQualitéAuPlusUneFois par défaut)"},
    {0}
};
/* Retour canon : entier. Numéro du message publié en QoS 1, 0 en QoS 0, -1 si erreur. */
#define MQTT_PUBLIE_RET "numéro du message publié (QoS 1), 0 si réussite (QoS 0), -1 en cas d'erreur"
static const OwlSignature sig_publie[] = {
    {3, p_publie, "entier", MQTT_PUBLIE_RET},
    {4, p_publie, "entier", MQTT_PUBLIE_RET},
    {5, p_publie, "entier", MQTT_PUBLIE_RET},
    {0}
};

static const OwlParam p_abonne[] = {
    {"session",   "mqttSession", "session MQTT connectée au broker"},
    {"sujet",     "chaîne",      "sujet (topic) auquel s'abonner"},
    {"qualité",   "entier",      "qualité de service souhaitée pour l'abonnement"},
    {"procédure", "procédure",   "procédure appelée à chaque message reçu (session, sujet, message)"},
    {0}
};
static const OwlSignature sig_abonne[] = {
    {4, p_abonne, "booléen", "vrai si l'abonnement a été mis en place, faux sinon"}, {0}
};

static const OwlParam p_desabonne[] = {
    {"session", "mqttSession", "session MQTT connectée au broker"},
    {"sujet",   "chaîne",      "sujet (topic) dont on se désabonne"},
    {0}
};
static const OwlSignature sig_desabonne[] = {
    {2, p_desabonne, "booléen", "vrai si le désabonnement a été effectué, faux sinon"}, {0}
};

static const OwlSignature sig_desabonne_tout[] = {
    {1, p_session, "booléen", "vrai si tous les désabonnements ont été effectués, faux sinon"}, {0}
};

static const OwlParam p_attend[] = {
    {"session", "mqttSession", "session MQTT connectée au broker"},
    {"délai",   "entier",      "durée maximale d'attente, en millisecondes"},
    {0}
};
static const OwlSignature sig_attend[] = {
    {2, p_attend, "booléen", "vrai si la session est toujours connectée, faux si le broker a fermé la connexion pendant l'attente"}, {0}
};

/* ============================================================
 * Method bindings — formes prefixe session.Xxx().
 * ============================================================ */

static const OwlMethodBinding MB_CONNECTE[] = {
    { "mqttSession", "Connecte", 1 }, { "mqttSession", "Connect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DECONNECTE[] = {
    { "mqttSession", "Deconnecte", 1 }, { "mqttSession", "Disconnect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_PUBLIE[] = {
    { "mqttSession", "Publie", -1 }, { "mqttSession", "Publish", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ABONNE[] = {
    { "mqttSession", "Abonne", 4 }, { "mqttSession", "Subscribe", 4 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DESABONNE[] = {
    { "mqttSession", "Desabonne", 2 }, { "mqttSession", "Unsubscribe", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DESABONNE_TOUT[] = {
    { "mqttSession", "DesabonneTout", 1 }, { "mqttSession", "UnsubscribeAll", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ATTEND[] = {
    { "mqttSession", "Attend", 2 }, { "mqttSession", "Wait", 2 }, { NULL, NULL, 0 }
};

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define M_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "MQTT", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_MQTT[] = {
    M_DEF("MQTTConnecte", NULL, "MQTTConnect",
        "Ouvre une connexion vers un broker MQTT à partir des paramètres d'une session, et indique si la connexion a réussi.",
        "Opens a connection to an MQTT broker from a session's parameters, and reports whether the connection succeeded.",
        sig_connecte, fn_mqtt_connecte, MB_CONNECTE),
    M_DEF("MQTTDéconnecte", "MQTTDeconnecte", "MQTTDisconnect",
        "Ferme une connexion MQTT ouverte.",
        "Closes an open MQTT connection.",
        sig_deconnecte, fn_mqtt_deconnecte, MB_DECONNECTE),
    M_DEF("MQTTPublie", NULL, "MQTTPublish",
        "Publie un message sur un sujet du broker MQTT et renvoie le numéro du message (0 en qualité de service au plus une fois, -1 en cas d'erreur).",
        "Publishes a message to a topic on the MQTT broker and returns the message number (0 for at-most-once quality of service, -1 on error).",
        sig_publie, fn_mqtt_publie, MB_PUBLIE),
    M_DEF("MQTTAbonne", NULL, "MQTTSubscribe",
        "Abonne une procédure aux messages publiés sur un sujet du broker MQTT ; la procédure est appelée à chaque message reçu.",
        "Subscribes a procedure to messages published on an MQTT broker topic; the procedure is called for each incoming message.",
        sig_abonne, fn_mqtt_abonne, MB_ABONNE),
    M_DEF("MQTTDésabonne", "MQTTDesabonne", "MQTTUnsubscribe",
        "Met fin à l'abonnement à un sujet du broker MQTT.",
        "Ends the subscription to an MQTT broker topic.",
        sig_desabonne, fn_mqtt_desabonne, MB_DESABONNE),
    M_DEF("MQTTDésabonneTout", "MQTTDesabonneTout", "MQTTUnsubscribeAll",
        "Met fin à tous les abonnements en cours sur une session MQTT.",
        "Ends all active subscriptions on an MQTT session.",
        sig_desabonne_tout, fn_mqtt_desabonne_tout, MB_DESABONNE_TOUT),
    M_DEF("MQTTAttend", NULL, "MQTTWait",
        "Attend et traite les messages entrants d'une session MQTT pendant un délai maximal exprimé en millisecondes, en appelant les procédures abonnées à mesure que les messages arrivent ; renvoie vrai tant que la session reste connectée.",
        "Waits for and processes the incoming messages of an MQTT session for a maximum delay in milliseconds, calling the subscribed procedures as messages arrive; returns true as long as the session stays connected.",
        sig_attend, fn_mqtt_attend, MB_ATTEND),
};

#undef M_DEF

/* ============================================================
 * Constantes.
 * ============================================================ */

static const OwlConstDef CONSTS_MQTT[] = {
    { "optionSSL", "MQTT", INTEGER_VAL(OWL_OPTION_SSL),
      "Ouvre une connexion chiffrée (TLS) vers le broker. À affecter à la propriété Option d'une session avant l'appel à MQTTConnecte.",
      "Opens an encrypted (TLS) connection to the broker. Assign to a session's Option property before calling MQTTConnect.",
      "optionSSL" },
    /* Qualité de service passée à MQTTPublie. Valeurs alignées sur le niveau
     * QoS du protocole (0/1). La qualité « exactement une fois » (QoS 2)
     * arrivera avec sa mécanique dédiée (PUBREC/PUBREL/PUBCOMP). */
    { "mqttQualitéAuPlusUneFois", "MQTT", INTEGER_VAL(0),
      "Qualité de service « au plus une fois » : le message est envoyé sans accusé de réception, il peut être perdu.",
      "\"At most once\" quality of service: the message is sent without acknowledgement and may be lost.",
      "mqttQualityAtMostOnce" },
    { "mqttQualitéAuMoinsUneFois", "MQTT", INTEGER_VAL(1),
      "Qualité de service « au moins une fois » : le message est retransmis jusqu'à l'accusé de réception du broker.",
      "\"At least once\" quality of service: the message is retransmitted until the broker acknowledges it.",
      "mqttQualityAtLeastOnce" },
};

/* ============================================================
 * Type reifie mqttSession.
 *
 * Noms EN des proprietes : NON presents dans le canon JSON (la doc
 * n'expose que la forme FR). Formes derivees par convention alignee sur
 * les types voisins (httpRequête : User / Password / ConnectionTimeout).
 * A confronter au doc anglais si divergence — remontee coordinator.
 * ============================================================ */

static const OwlField FIELDS_mqttSession[] = {
    {"Adresse",               NULL,                  "chaîne",  "\"\"",   "adresse du broker MQTT (IP ou nom d'hôte)", "MQTT broker address (IP or host name)", .nameEN="Address"},
    {"Port",                  NULL,                  "entier",  "1883",   "port du broker (1883 en clair, 8883 en SSL)", "broker port (1883 clear, 8883 SSL)", .nameEN="Port"},
    {"IDClient",              NULL,                  "chaîne",  "\"\"",   "identifiant du client (ne doit pas être vide)", "client identifier (must not be empty)", .nameEN="ClientID"},
    {"Utilisateur",           NULL,                  "chaîne",  "\"\"",   "nom d'utilisateur pour l'authentification", "user name for authentication", .nameEN="User"},
    {"MotDePasseUtilisateur", NULL,                  "chaîne",  "\"\"",   "mot de passe de l'utilisateur", "user password", .nameEN="UserPassword"},
    {"ClePrivee",             "CléPrivée",           "chaîne",  "\"\"",   "chemin du fichier de clé privée .pem (connexion par clé)", "path to the .pem private-key file (key-based connection)", .nameEN="PrivateKey"},
    {"MotDePasseClePrivee",   "MotDePasseCléPrivée", "chaîne",  "\"\"",   "mot de passe du fichier de clé privée", "private-key file password", .nameEN="PrivateKeyPassword"},
    {"Option",                NULL,                  "entier",  "0",      "options de connexion (optionSSL pour une connexion chiffrée)", "connection options (optionSSL for an encrypted connection)", .nameEN="Option"},
    {"CertificatCA",          NULL,                  "chaîne",  "\"\"",   "chemin d'un fichier PEM de certificat d'autorité (CA) à utiliser pour valider le serveur en TLS ; vide = autorités publiques par défaut", "path to a PEM certificate-authority (CA) file used to validate the server over TLS; empty = default public authorities", .nameEN="CACertificate"},
    {"SessionVide",           NULL,                  "booléen", "Faux",   "Vrai pour repartir d'une session propre sur le broker", "True to start from a clean session on the broker", .nameEN="EmptySession"},
    {"TimeoutConnexion",      NULL,                  "entier",  "0",      "délai de connexion en secondes (0 = défaut)", "connection timeout in seconds (0 = default)", .nameEN="ConnectionTimeout"},
    {0}
};

static const OwlTypeDef TYPES_MQTT[] = {
    { .canonical      = "mqttSession",
      .canonicalAscii = "mqttSession",
      .aliases        = NULL,
      .category       = "MQTT",
      .descFR         = "Paramètres de connexion à un broker MQTT (adresse, port, identifiant client, authentification, options). Passé à MQTTConnecte.",
      .descEN         = "Connection parameters for an MQTT broker (address, port, client id, authentication, options). Passed to MQTTConnect.",
      .fields         = FIELDS_mqttSession,
      .nameEN         = "mqttSession",
      .genre          = OWL_GENRE_F },   /* la session */
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_mqtt_init(void) {
    /* Le socle Message Queue E1 (registre d'abonnements) est arme par
     * stdlib_mq_init, point central neutre appele par owl_stdlib_init AVANT ce
     * client. MQTT n'initialise donc plus le socle lui-meme : il le partage
     * avec les futurs clients MQ (contrat multi-protocole). */
    owl_registry_register_module(DEFS_MQTT,
        (int)(sizeof(DEFS_MQTT) / sizeof(DEFS_MQTT[0])));
    owl_registry_register_types(TYPES_MQTT,
        (int)(sizeof(TYPES_MQTT) / sizeof(TYPES_MQTT[0])));
    owl_registry_register_consts(CONSTS_MQTT,
        (int)(sizeof(CONSTS_MQTT) / sizeof(CONSTS_MQTT[0])));

    ObjInstance* tpl = build_mqttSession_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("mqttSession", OBJ_VAL(tpl));
    vm_pop();
}
