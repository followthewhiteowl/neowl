// SPDX-License-Identifier: AGPL-3.0-only
/* mqtt_connexion_test.c — client MQTT 3.1.1, Lot A.
 *
 * Test wire C-interne, calque sur mq_livraison_test.c : un pair mock joue
 * le broker MQTT. Contrat mesure du handshake :
 *   - le client (MQTTConnecte) se connecte au listener local, emet un
 *     CONNECT bien forme, lit le CONNACK accepte (0x20 0x02 0x00 0x00)
 *     injecte par le broker, et renvoie Vrai ;
 *   - MQTTDéconnecte emet un DISCONNECT (0xE0 0x00) puis ferme.
 *
 * ETAT ATTENDU AU LOT A RED : les natives sont des STUBS. MQTTConnecte
 * ouvre/referme la socket (ce qui debloque le broker mock) mais n'emet
 * aucune trame et renvoie Faux. Les assertions « connecte », « CONNECT
 * emis » et « DISCONNECT emis » sont donc ROUGES tant que l'impl reelle
 * (Lot A green) n'est pas ecrite.
 *
 * Wire = spec OASIS MQTT 3.1.1 (absente de la doc) :
 *   CONNECT   : 0x10 <remLen> 0x00 0x04 'M' 'Q' 'T' 'T' 0x04 <flags>
 *               <keepAliveHi> <keepAliveLo> <clientIdLen(2)> <clientId...>
 *   CONNACK   : 0x20 0x02 <flags> <returnCode> ; accepte = 0x00 0x00
 *   DISCONNECT: 0xE0 0x00
 *
 * Execute via `owl --self-test-net`. Gere son propre vm_init/vm_free.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "../net_mock_peer.h"
#include "../net_reacteur.h"
#include "../stdlib_mqtt.h"
#include "../vm.h"
#include "../object.h"
#include "../table.h"
#include "../gc.h"

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass = 0;
static int g_fail = 0;

static void compte(int ok, const char* label, int ligne) {
    if (ok) g_pass++;
    else { g_fail++; fprintf(stderr, "  FAIL MQTT [%s] (ligne %d)\n", label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Lecture de globales OWL depuis le C (assertion).
 * Cle ObjString rootee pendant le lookup (skill gc-root-obj-fresh).
 * ================================================================ */

static bool lit_globale(const char* nom, Value* out) {
    ObjString* k = obj_copy_string(nom, (int)strlen(nom));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&vm.globals, k, out);
    vm_pop();
    return ok;
}

static bool lit_globale_bool(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return false;
    if (!IS_BOOL(v)) return false;
    return AS_BOOL(v);
}

/* Recherche d'octets bruts dans le tampon recu (binary-safe, contrairement
 * a strstr qui s'arrete au premier 0x00 d'une trame CONNECT). */
static bool contient(const char* buf, size_t len, const char* motif, size_t mlen) {
    if (mlen == 0 || len < mlen) return false;
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(buf + i, motif, mlen) == 0) return true;
    }
    return false;
}

/* Position (index) de la premiere occurrence de motif, ou -1. Binary-safe. */
static long trouve(const char* buf, size_t len, const char* motif, size_t mlen) {
    if (mlen == 0 || len < mlen) return -1;
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(buf + i, motif, mlen) == 0) return (long)i;
    }
    return -1;
}

static bool lit_globale_entier(const char* nom, int64_t* out) {
    Value v;
    if (!lit_globale(nom, &v)) return false;
    if (!IS_INTEGER(v)) return false;
    *out = AS_INTEGER(v);
    return true;
}

static const char* lit_globale_chaine(const char* nom) {
    Value v;
    if (!lit_globale(nom, &v)) return NULL;
    if (!IS_STRING(v)) return NULL;
    return AS_CSTRING(v);
}

/* Presence d'un octet exact dans le tampon (binary-safe). Sert a reperer
 * un type de trame MQTT (0x82 SUBSCRIBE, 0xA2 UNSUBSCRIBE) sans dependre
 * du reste de l'encodage. */
static bool contient_octet(const char* buf, size_t len, unsigned char octet) {
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)buf[i] == octet) return true;
    }
    return false;
}

/* Nombre d'occurrences (non chevauchantes au sens strict mais ici les motifs
 * de trame sont assez longs pour que ca n'arrive pas) d'un motif d'octets dans
 * le tampon. Binary-safe. Sert a compter 2x PUBREC vs 1x PUBCOMP. */
static int compte_motif(const char* buf, size_t len, const char* motif, size_t mlen) {
    if (mlen == 0 || len < mlen) return 0;
    int n = 0;
    for (size_t i = 0; i + mlen <= len; i++) {
        if (memcmp(buf + i, motif, mlen) == 0) n++;
    }
    return n;
}

/* ================================================================
 * Handshake CONNECT/CONNACK + DISCONNECT contre un broker mock.
 * ================================================================ */

static void famille_handshake(void) {
    static const char CONNACK[] = { 0x20, 0x02, 0x00, 0x00 }; /* accepte */
    static const char* CLIENT_ID = "owl-test-client";         /* 15 octets */

    vm_init();

    /* Broker mock : accepte, lit le CONNECT (au moins la partie fixe +
     * variable = 12 octets), repond CONNACK, puis lit le DISCONNECT
     * jusqu'a EOF. */
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                = OWL_MOCK_ROLE_ECOUTE;
    mc.port                = 0;
    mc.octets              = CONNACK;
    mc.octets_len          = sizeof(CONNACK);
    mc.decoupe             = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture  = 12;
    mc.lire_apres_ecriture = 1;
    mc.timeout_lecture_ms  = 2000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "broker_mock_demarre");
    if (broker == NULL) { vm_free(); return; }

    int port = owl_mock_peer_port(broker);
    CHECK(port > 0, "broker_port_ephemere");

    /* Harnais OWL : configure la session, connecte, deconnecte si OK.
     * Proprietes via `..` (invariant 6), methode via `.` — ici on
     * exerce la forme native globale. */
    char src[600];
    snprintf(src, sizeof(src),
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gResConnect est un booleen = MQTTConnecte(gSess)\n"
        "SI gResConnect ALORS\n"
        "    MQTTDeconnecte(gSess)\n"
        "FIN\n",
        port, CLIENT_ID);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "harnais_compile_execute");

    /* Le client a couru pendant vm_interpret ; on attend la fin du thread
     * broker avant de lire ce qu'il a accumule. */
    owl_mock_peer_joint(broker);

    /* Coeur du contrat : la connexion a reussi. ROUGE au Lot A (stub). */
    CHECK(lit_globale_bool("gResConnect"), "mqtt_connecte_reussit");

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);

    /* CONNECT bien forme : type 0x1x, nom de protocole "MQTT", niveau 4,
     * client id present. ROUGE au Lot A (rien n'est emis). */
    CHECK(rlen >= 12 && ((unsigned char)recu[0] & 0xF0) == 0x10, "connect_type_paquet");
    CHECK(contient(recu, rlen, "MQTT", 4), "connect_nom_protocole");
    CHECK(rlen >= 9 && (unsigned char)recu[8] == 0x04, "connect_niveau_protocole_4");
    CHECK(contient(recu, rlen, CLIENT_ID, strlen(CLIENT_ID)), "connect_client_id_present");

    /* DISCONNECT en fin de flux : 0xE0 0x00. ROUGE au Lot A. */
    CHECK(rlen >= 2 &&
          (unsigned char)recu[rlen - 2] == 0xE0 &&
          (unsigned char)recu[rlen - 1] == 0x00,
          "disconnect_emis");

    owl_mock_peer_libere(broker);
    vm_free();
}

/* ================================================================
 * Publication PUBLISH (Lot B) contre un broker mock.
 *
 * Wire = spec OASIS MQTT 3.1.1 :
 *   PUBLISH : byte0 = 0x30 | (retain?0x01) | (QoS<<1) ; remLen ;
 *             topic (len(2)+UTF-8) ; packet-id(2) SI QoS>0 ; payload
 *   PUBACK  : 0x40 0x02 <pid_hi> <pid_lo>  (accuse QoS 1)
 *
 * On reutilise le CONNACK du Lot A pour etablir la session avant de publier.
 * ETAT ATTENDU AU LOT B RED : MQTTPublie est un stub (aucune trame PUBLISH,
 * renvoie -1) -> les assertions type/sujet/payload/packet-id/retour sont
 * rouges tant que l'impl reelle (Lot B green) n'est pas ecrite.
 *
 * Le byte0 de la trame PUBLISH est localise par rapport au sujet : la trame
 * courte a un remLen sur 1 octet, donc byte0 = recu[idx_sujet - 4]
 * (byte0, remLen, topicLen_hi, topicLen_lo, puis le sujet).
 * ================================================================ */

/* QoS 0 : fire-and-forget. Le broker n'accuse pas ; retour attendu = 0. */
static void cas_publie_qos0(void) {
    static const char CONNACK[] = { 0x20, 0x02, 0x00, 0x00 };
    static const char* CLIENT_ID = "owl-pub-q0";
    static const char* SUJET     = "capteurs/temperature";
    static const char* PAYLOAD   = "bonjour-mqtt";

    vm_init();

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = CONNACK;
    mc.octets_len            = sizeof(CONNACK);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 2000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "q0_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[700];
    snprintf(src, sizeof(src),
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gRes est un booleen = MQTTConnecte(gSess)\n"
        "gPub est un entier = -9\n"
        "SI gRes ALORS\n"
        "    gPub = MQTTPublie(gSess, \"%s\", \"%s\")\n"
        "    MQTTDeconnecte(gSess)\n"
        "FIN\n",
        port, CLIENT_ID, SUJET, PAYLOAD);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "q0_harnais_execute");
    owl_mock_peer_joint(broker);

    CHECK(lit_globale_bool("gRes"), "q0_connecte");

    int64_t ret = -99;
    CHECK(lit_globale_entier("gPub", &ret) && ret == 0, "q0_retour_zero");

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    long ti = trouve(recu, rlen, SUJET, strlen(SUJET));
    CHECK(ti >= 4, "q0_sujet_present");
    if (ti >= 4) {
        unsigned char b0 = (unsigned char)recu[ti - 4];
        CHECK((b0 & 0xF0) == 0x30, "q0_type_publish");
        CHECK(((b0 >> 1) & 0x03) == 0, "q0_qos_zero");
        CHECK((b0 & 0x01) == 0, "q0_retain_absent");
        /* QoS 0 : payload immediatement apres le sujet, aucun packet-id. */
        long pi = trouve(recu, rlen, PAYLOAD, strlen(PAYLOAD));
        CHECK(pi == ti + (long)strlen(SUJET), "q0_payload_sans_packet_id");
    }
    CHECK(trouve(recu, rlen, PAYLOAD, strlen(PAYLOAD)) >= 0, "q0_payload_present");

    owl_mock_peer_libere(broker);
    vm_free();
}

/* retain = Vrai : bit0 du byte0 positionne (QoS 0 par ailleurs). */
static void cas_publie_retain(void) {
    static const char CONNACK[] = { 0x20, 0x02, 0x00, 0x00 };
    static const char* CLIENT_ID = "owl-pub-ret";
    static const char* SUJET     = "etat/porte";
    static const char* PAYLOAD   = "verrouillee";

    vm_init();

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = CONNACK;
    mc.octets_len            = sizeof(CONNACK);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 2000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "ret_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[700];
    snprintf(src, sizeof(src),
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gRes est un booleen = MQTTConnecte(gSess)\n"
        "gPub est un entier = -9\n"
        "SI gRes ALORS\n"
        "    gPub = MQTTPublie(gSess, \"%s\", \"%s\", Vrai)\n"
        "    MQTTDeconnecte(gSess)\n"
        "FIN\n",
        port, CLIENT_ID, SUJET, PAYLOAD);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "ret_harnais_execute");
    owl_mock_peer_joint(broker);

    CHECK(lit_globale_bool("gRes"), "ret_connecte");

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    long ti = trouve(recu, rlen, SUJET, strlen(SUJET));
    CHECK(ti >= 4, "ret_sujet_present");
    if (ti >= 4) {
        unsigned char b0 = (unsigned char)recu[ti - 4];
        CHECK((b0 & 0xF0) == 0x30, "ret_type_publish");
        CHECK((b0 & 0x01) == 0x01, "ret_retain_positionne");
    }

    owl_mock_peer_libere(broker);
    vm_free();
}

/* QoS 1 : packet-id dans le PUBLISH, PUBACK lu de maniere synchrone ; le
 * retour est le numero du message (packet-id du premier publish = 1). */
static void cas_publie_qos1(void) {
    /* CONNACK (accepte) puis PUBACK (pid 1) emis en un seul bloc : le client
     * lit le CONNACK a la connexion, le PUBACK reste tamponne pour le publish. */
    static const char CONNACK_PUBACK[] = {
        0x20, 0x02, 0x00, 0x00,   /* CONNACK accepte */
        0x40, 0x02, 0x00, 0x01    /* PUBACK packet-id 1 */
    };
    static const char* CLIENT_ID = "owl-pub-q1";
    static const char* SUJET     = "commandes/relais";
    static const char* PAYLOAD   = "avec-accuse";

    vm_init();

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = CONNACK_PUBACK;
    mc.octets_len            = sizeof(CONNACK_PUBACK);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 2000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "q1_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[700];
    snprintf(src, sizeof(src),
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gRes est un booleen = MQTTConnecte(gSess)\n"
        "gPub est un entier = -9\n"
        "SI gRes ALORS\n"
        "    gPub = MQTTPublie(gSess, \"%s\", \"%s\", Faux, mqttQualiteAuMoinsUneFois)\n"
        "    MQTTDeconnecte(gSess)\n"
        "FIN\n",
        port, CLIENT_ID, SUJET, PAYLOAD);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "q1_harnais_execute");
    owl_mock_peer_joint(broker);

    CHECK(lit_globale_bool("gRes"), "q1_connecte");

    int64_t ret = -99;
    CHECK(lit_globale_entier("gPub", &ret) && ret == 1, "q1_retour_numero_message");

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    long ti = trouve(recu, rlen, SUJET, strlen(SUJET));
    CHECK(ti >= 4, "q1_sujet_present");
    if (ti >= 4) {
        unsigned char b0 = (unsigned char)recu[ti - 4];
        CHECK((b0 & 0xF0) == 0x30, "q1_type_publish");
        CHECK(((b0 >> 1) & 0x03) == 1, "q1_qos_un");
        /* packet-id (2 octets, big-endian) juste apres le sujet, avant payload. */
        long pid_off = ti + (long)strlen(SUJET);
        CHECK((size_t)(pid_off + 2) <= rlen &&
              (unsigned char)recu[pid_off]     == 0x00 &&
              (unsigned char)recu[pid_off + 1] == 0x01,
              "q1_packet_id_present");
        long pi = trouve(recu, rlen, PAYLOAD, strlen(PAYLOAD));
        CHECK(pi == pid_off + 2, "q1_payload_apres_packet_id");
    }

    owl_mock_peer_libere(broker);
    vm_free();
}

static void famille_publication(void) {
    cas_publie_qos0();
    cas_publie_retain();
    cas_publie_qos1();
}

/* ================================================================
 * Abonnement SUBSCRIBE/SUBACK + livraison PUBLISH entrant (Lot C).
 *
 * Wire = spec OASIS MQTT 3.1.1 :
 *   SUBSCRIBE   : 0x82 <remLen> <pid(2)> [ <filtreLen(2)> <filtre...> <QoS> ]+
 *   SUBACK      : 0x90 <remLen> <pid(2)> <codeRetour>+   (accorde = QoS 0..2)
 *   UNSUBSCRIBE : 0xA2 <remLen> <pid(2)> [ <filtreLen(2)> <filtre...> ]+
 *   PUBLISH(in) : 0x30 <remLen> <sujetLen(2)> <sujet...> <payload...>  (QoS 0)
 *
 * Le broker mock ne fait qu'un aller-retour (lit le CONNECT, ecrit un bloc,
 * accumule la suite). On empile donc CONNACK + SUBACK + PUBLISH en un seul
 * bloc (meme ruse que le PUBACK groupe du Lot B QoS1) : MQTTConnecte lit le
 * CONNACK, MQTTAbonne lit le SUBACK, le PUBLISH reste tamponne pour la pompe.
 *
 * Le reacteur est POSSEDE par la session (branche au 1er abonnement,
 * ) ; ici on le recupere via owl_mqtt_reacteur_session pour un tick
 * manuel a bas niveau (comme mq_livraison_test.c). La surface utilisateur de
 * pompe (MQTTAttend) est couverte par famille_attend ci-dessous.
 * ================================================================ */

/* Filtre + charge du PUBLISH injecte. */
#define SUB_SUJET  "sujet/test"    /* 10 octets */
#define SUB_CHARGE "capteur-42"    /* 10 octets */

/* CONNACK accepte + SUBACK (pid 1, QoS accorde 1) + PUBLISH QoS0 sur le sujet.
 * PUBLISH remLen = 2 (prefixe sujet) + 10 (sujet) + 10 (charge) = 22 = 0x16. */
static const char SUB_BUNDLE[] = {
    0x20, 0x02, 0x00, 0x00,                         /* CONNACK accepte */
    (char)0x90, 0x03, 0x00, 0x01, 0x01,             /* SUBACK pid=1 accorde=1 */
    0x30, 0x16, 0x00, 0x0A,                          /* PUBLISH byte0/remLen/sujetLen */
    's','u','j','e','t','/','t','e','s','t',         /* sujet */
    'c','a','p','t','e','u','r','-','4','2'          /* charge */
};

/* Broker mock commun aux deux cas : ECOUTE, lit le CONNECT (>=12), ecrit le
 * bundle, accumule ce que le client envoie ensuite, referme sur timeout. */
static OwlMockPeer* demarre_broker_abonnement(void) {
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = SUB_BUNDLE;
    mc.octets_len            = sizeof(SUB_BUNDLE);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 2000;
    mc.fermer_apres_ecriture = 1;
    return owl_mock_peer_demarre(&mc);
}

/* Cas 1 — livraison : abonnement puis PUBLISH entrant livre au handler OWL
 * pendant un tick de pompe. Le handler survit a des cycles GC forces entre
 * l'abonnement et la pompe (racine tenue par le socle E1). */
static void cas_abonne_livraison(void) {
    static const char* CLIENT_ID = "owl-sub-01";

    vm_init();

    OwlMockPeer* broker = demarre_broker_abonnement();
    CHECK(broker != NULL, "sub_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    /* Le handler enregistre le sujet et la charge (Buffer -> chaine par
     * coercion a l'affectation) dans des globales observables. */
    char src[900];
    snprintf(src, sizeof(src),
        "gAbAppels est un entier = 0\n"
        "gAbSujet est une chaine = \"\"\n"
        "gAbCharge est une chaine = \"\"\n"
        "PROCEDURE gAbRecoit(sess est un mqttSession, sujet est une chaine, msg est un Buffer)\n"
        "    gAbAppels = gAbAppels + 1\n"
        "    gAbSujet = sujet\n"
        "    gAbCharge = msg\n"
        "FIN\n"
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gConn est un booleen = MQTTConnecte(gSess)\n"
        "gAb est un booleen = Faux\n"
        "SI gConn ALORS\n"
        "    gAb = MQTTAbonne(gSess, \"%s\", mqttQualiteAuMoinsUneFois, gAbRecoit)\n"
        "FIN\n",
        port, CLIENT_ID, SUB_SUJET);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "sub_harnais_execute");
    CHECK(lit_globale_bool("gConn"), "sub_connecte");

    /* Coeur du contrat : MQTTAbonne a reussi. ROUGE au Lot C (stub -> Faux). */
    CHECK(lit_globale_bool("gAb"), "sub_abonne_reussit");

    /* Cycles GC forces : le handler abonne ne doit pas etre collecte. */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 32; j++) (void)obj_copy_string("ballast", 7);
        gc_collect();
    }

    /* Pompe C-level : lit le PUBLISH tamponne, frame, livre au handler. Le
     * reacteur est possede par la session (branche au 1er abonnement) ; on le
     * recupere pour un tick manuel a bas niveau. */
    Value session;
    CHECK(lit_globale("gSess", &session) && IS_INSTANCE(session), "sub_session_lue");

    OwlReacteur* r = owl_mqtt_reacteur_session(session);
    CHECK(r != NULL, "sub_reacteur_session");

    int n = r ? owl_reacteur_tick(r, 2000) : 0;
    (void)n;

    /* Le handler a recu le message : appele une fois, sujet + charge exacts.
     * ROUGE au Lot C (aucune livraison). Vaut aussi assertion GC : la
     * livraison apres cycles GC prouve que le handler est reste racine. */
    int64_t appels = -1;
    CHECK(lit_globale_entier("gAbAppels", &appels) && appels == 1,
          "sub_handler_appele_une_fois");
    {
        const char* sujet  = lit_globale_chaine("gAbSujet");
        const char* charge = lit_globale_chaine("gAbCharge");
        CHECK(sujet != NULL && strcmp(sujet, SUB_SUJET) == 0,
              "sub_handler_recoit_le_sujet");
        CHECK(charge != NULL && strcmp(charge, SUB_CHARGE) == 0,
              "sub_handler_recoit_la_charge");
    }

    owl_mqtt_ferme(session);       /* referme la socket + detruit le reacteur possede */
    owl_mock_peer_joint(broker);

    /* Sur le fil : un SUBSCRIBE bien forme est parvenu au broker.
     * ROUGE au Lot C (stub -> rien emis). */
    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    long ti = trouve(recu, rlen, SUB_SUJET, strlen(SUB_SUJET));
    CHECK(ti >= 6, "sub_subscribe_sujet_present");
    if (ti >= 6) {
        /* byte0, remLen(1), pid(2), filtreLen(2) precedent le sujet. */
        unsigned char b0 = (unsigned char)recu[ti - 6];
        CHECK((b0 & 0xF0) == 0x80, "sub_subscribe_type_paquet");
        CHECK((b0 & 0x0F) == 0x02, "sub_subscribe_flags_reserves");
    }

    owl_mock_peer_libere(broker);
    vm_free();
}

/* Cas 2 — desabonnement : MQTTDésabonne emet un UNSUBSCRIBE bien forme. */
static void cas_desabonne_wire(void) {
    static const char* CLIENT_ID = "owl-unsub-1";

    vm_init();

    OwlMockPeer* broker = demarre_broker_abonnement();
    CHECK(broker != NULL, "unsub_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[900];
    snprintf(src, sizeof(src),
        "PROCEDURE gUnsubRecoit(sess est un mqttSession, sujet est une chaine, msg est un Buffer)\n"
        "FIN\n"
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gConn est un booleen = MQTTConnecte(gSess)\n"
        "gDes est un booleen = Faux\n"
        "SI gConn ALORS\n"
        "    MQTTAbonne(gSess, \"%s\", mqttQualiteAuMoinsUneFois, gUnsubRecoit)\n"
        "    gDes = MQTTDesabonne(gSess, \"%s\")\n"
        "    MQTTDeconnecte(gSess)\n"
        "FIN\n",
        port, CLIENT_ID, SUB_SUJET, SUB_SUJET);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "unsub_harnais_execute");
    CHECK(lit_globale_bool("gConn"), "unsub_connecte");

    /* MQTTDésabonne a rendu Vrai. ROUGE au Lot C (stub -> Faux). */
    CHECK(lit_globale_bool("gDes"), "unsub_desabonne_reussit");

    owl_mock_peer_joint(broker);

    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    /* SUBSCRIBE (0x82) puis UNSUBSCRIBE (0xA2) parvenus au broker.
     * ROUGE au Lot C (stubs -> aucune des deux trames). */
    CHECK(contient_octet(recu, rlen, 0x82), "unsub_subscribe_emis");
    CHECK(contient_octet(recu, rlen, 0xA2), "unsub_unsubscribe_emis");

    owl_mock_peer_libere(broker);
    vm_free();
}

static void famille_abonnement(void) {
    cas_abonne_livraison();
    cas_desabonne_wire();
}

/* ================================================================
 * QoS 1 sur un sujet ABONNÉ — accusé PUBACK entrelacé avec l'écho
 * PUBLISH entrant.
 *
 * Reproduit AU NIVEAU UNITAIRE le trou que seul un vrai broker
 * exhibait : sur une connexion abonnée, publier en QoS 1 sur le sujet
 * abonné fait revenir un PUBLISH (écho du message) qui S'ENTRELACE avec
 * le PUBACK attendu. La lecture SYNCHRONE du PUBACK (recv_exact) tombe
 * alors sur l'octet 0x30 du PUBLISH au lieu de 0x40 -> échec (-1) et
 * flux désynchronisé.
 *
 * Le broker mock empile en un seul bloc, DANS L'ORDRE DU FIL :
 *   CONNACK + SUBACK(pid1) + ECHO PUBLISH(QoS0, sujet abonné) + PUBACK(pid2)
 * Le PUBACK arrive donc APRÈS l'écho : impl synchrone -> lit l'écho à la
 * place du PUBACK.
 *
 * ETAT ATTENDU AU RED (impl synchrone) :
 *   - MQTTPublie QoS 1 renvoie -1 (lit l'écho, pas le PUBACK)  -> ROUGE ;
 *   - l'écho n'est jamais livré au handler (MQTTPublie ne pompe pas)
 *     -> gAppels == 0                                          -> ROUGE.
 * ETAT ATTENDU AU GREEN (accusés routés par la pompe / corrélation) :
 *   - MQTTPublie renvoie le n° de message (2) ;
 *   - l'écho PUBLISH est framé et livré au handler (une fois, sujet +
 *     charge intègres).
 * ================================================================ */

/* Écho publié en retour sur le sujet abonné (charge distincte pour
 * lever toute ambiguïté sur l'origine de la livraison). */
#define ECHO_CHARGE "temp-22"      /* 7 octets */

/* CONNACK + SUBACK(pid1,accordé1) + ECHO PUBLISH QoS0 (sujet abonné) +
 * PUBACK(pid2). ECHO remLen = 2 (préfixe sujet) + 10 (sujet) + 7 (charge)
 * = 19 = 0x13. Le PUBACK (pid 2 = 1er publish après l'abonnement pid 1)
 * est placé APRÈS l'écho : c'est l'entrelacement qui casse la lecture
 * synchrone. */
static const char PUB_ABONNE_BUNDLE[] = {
    0x20, 0x02, 0x00, 0x00,                         /* CONNACK accepte */
    (char)0x90, 0x03, 0x00, 0x01, 0x01,             /* SUBACK pid=1 accorde=1 */
    0x30, 0x13, 0x00, 0x0A,                          /* ECHO PUBLISH byte0/remLen/sujetLen=10 */
    's','u','j','e','t','/','t','e','s','t',         /* sujet (SUB_SUJET) */
    't','e','m','p','-','2','2',                     /* charge écho (ECHO_CHARGE) */
    0x40, 0x02, 0x00, 0x02                          /* PUBACK pid=2 (le publish QoS 1) */
};

static void cas_publie_qos1_sujet_abonne(void) {
    static const char* CLIENT_ID = "owl-pub-echo";
    static const char* PAYLOAD   = "temp=22.7";

    vm_init();

    /* Connexion gardée vivante après l'écriture du bloc : la pompe (green)
     * a besoin que la socket reste ouverte le temps de framer l'écho + le
     * PUBACK. Le mock accumule ensuite les envois du client puis referme
     * sur timeout. */
    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = PUB_ABONNE_BUNDLE;
    mc.octets_len            = sizeof(PUB_ABONNE_BUNDLE);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 1500;
    mc.fermer_apres_ecriture = 0;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "pubab_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[1000];
    snprintf(src, sizeof(src),
        "gAppels est un entier = 0\n"
        "gSujet est une chaine = \"\"\n"
        "gCharge est une chaine = \"\"\n"
        "PROCEDURE gRecoit(sess est un mqttSession, sujet est une chaine, msg est un Buffer)\n"
        "    gAppels = gAppels + 1\n"
        "    gSujet = sujet\n"
        "    gCharge = msg\n"
        "FIN\n"
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gConn est un booleen = MQTTConnecte(gSess)\n"
        "gAb est un booleen = Faux\n"
        "gPub est un entier = -9\n"
        "SI gConn ALORS\n"
        "    gAb = MQTTAbonne(gSess, \"%s\", mqttQualiteAuMoinsUneFois, gRecoit)\n"
        "    gPub = MQTTPublie(gSess, \"%s\", \"%s\", Faux, mqttQualiteAuMoinsUneFois)\n"
        "FIN\n",
        port, CLIENT_ID, SUB_SUJET, SUB_SUJET, PAYLOAD);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "pubab_harnais_execute");
    CHECK(lit_globale_bool("gConn"), "pubab_connecte");
    CHECK(lit_globale_bool("gAb"), "pubab_abonne");

    /* Coeur du contrat  : le PUBACK est retrouvé malgré l'écho
     * entrelacé -> numéro de message positif. ROUGE contre l'impl
     * synchrone (lit l'écho au lieu du PUBACK -> -1). */
    int64_t ret = -99;
    CHECK(lit_globale_entier("gPub", &ret) && ret > 0,
          "pubab_qos1_numero_message_positif");

    /* L'écho PUBLISH entrant est livré au handler (une seule fois), sujet
     * et charge intègres. ROUGE contre l'impl synchrone (l'écho est
     * consommé par la lecture du PUBACK, jamais livré). */
    int64_t appels = -1;
    CHECK(lit_globale_entier("gAppels", &appels) && appels == 1,
          "pubab_echo_livre_une_fois");
    {
        const char* sujet  = lit_globale_chaine("gSujet");
        const char* charge = lit_globale_chaine("gCharge");
        CHECK(sujet != NULL && strcmp(sujet, SUB_SUJET) == 0,
              "pubab_echo_sujet_integre");
        CHECK(charge != NULL && strcmp(charge, ECHO_CHARGE) == 0,
              "pubab_echo_charge_integre");
    }

    Value session;
    if (lit_globale("gSess", &session) && IS_INSTANCE(session))
        owl_mqtt_ferme(session);
    owl_mock_peer_joint(broker);
    owl_mock_peer_libere(broker);
    vm_free();
}

static void famille_publie_abonne(void) {
    cas_publie_qos1_sujet_abonne();
}

/* ================================================================
 * Pompe de reception pilotee depuis OWL — MQTTAttend.
 *
 * MQTTAttend(session, délai) EST la pompe cote utilisateur : bloquant jusqu'a
 * `délai` ms, elle ticke le reacteur POSSEDE par la session, dispatche les
 * PUBLISH entrants aux procedures abonnees, et renvoie un booleen — Vrai tant
 * que la session reste connectee, Faux si le broker a raccroche pendant
 * l'attente. Aucun tick C manuel ici : MQTTAttend est la pompe.
 *
 * Le broker mock emet CONNACK + SUBACK + PUBLISH en un bloc puis garde la
 * connexion ouverte le temps d'une fenetre de lecture avant de raccrocher :
 *   - la 1re attente livre le PUBLISH tamponne au handler, connexion vivante
 *     -> Vrai ;
 *   - la 2e attente rencontre le raccrochage -> Faux.
 *
 * ETAT ATTENDU AU RED : MQTTAttend est un STUB (ne pompe pas,
 * renvoie Faux). La 1re attente ne livre donc rien (handler jamais appele) et
 * renvoie Faux au lieu de Vrai. La 2e attente renvoie Faux (valeur attendue au
 * green : le stub la satisfait par coincidence).
 * ================================================================ */

static void cas_attend_pompe(void) {
    static const char* CLIENT_ID = "owl-attend";

    vm_init();

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = SUB_BUNDLE;
    mc.octets_len            = sizeof(SUB_BUNDLE);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 800;   /* fenetre ouverte avant raccrochage */
    mc.fermer_apres_ecriture = 0;     /* garde la connexion vivante d'abord */

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "attend_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    /* Handler observable : compte les appels, memorise sujet + charge. Les
     * deux attentes sont pilotees depuis OWL (MQTTAttend), pas de tick C. */
    char src[1000];
    snprintf(src, sizeof(src),
        "gWSujet est une chaine = \"\"\n"
        "gWCharge est une chaine = \"\"\n"
        "gWAppels est un entier = 0\n"
        "PROCEDURE gWRecoit(sess est un mqttSession, sujet est une chaine, msg est un Buffer)\n"
        "    gWAppels = gWAppels + 1\n"
        "    gWSujet = sujet\n"
        "    gWCharge = msg\n"
        "FIN\n"
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gWait1 est un booleen = Faux\n"
        "gWait2 est un booleen = Faux\n"
        "gConn est un booleen = MQTTConnecte(gSess)\n"
        "gAb est un booleen = Faux\n"
        "SI gConn ALORS\n"
        "    gAb = MQTTAbonne(gSess, \"%s\", mqttQualiteAuMoinsUneFois, gWRecoit)\n"
        "    gWait1 = MQTTAttend(gSess, 100)\n"
        "    gWait2 = MQTTAttend(gSess, 3000)\n"
        "FIN\n",
        port, CLIENT_ID, SUB_SUJET);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "attend_harnais_execute");
    CHECK(lit_globale_bool("gConn"), "attend_connecte");
    CHECK(lit_globale_bool("gAb"), "attend_abonne");

    /* Coeur du contrat . ROUGE au RED (stub ne pompe pas). */
    CHECK(lit_globale_bool("gWait1"), "attend_retour_vrai_connecte");

    int64_t appels = -1;
    CHECK(lit_globale_entier("gWAppels", &appels) && appels == 1,
          "attend_handler_appele_une_fois_pendant_attente");
    {
        const char* sujet  = lit_globale_chaine("gWSujet");
        const char* charge = lit_globale_chaine("gWCharge");
        CHECK(sujet != NULL && strcmp(sujet, SUB_SUJET) == 0,
              "attend_handler_recoit_le_sujet");
        CHECK(charge != NULL && strcmp(charge, SUB_CHARGE) == 0,
              "attend_handler_recoit_la_charge");
    }

    /* Raccrochage du broker pendant la 2e attente -> Faux. */
    CHECK(!lit_globale_bool("gWait2"), "attend_retour_faux_apres_raccrochage");

    Value session;
    if (lit_globale("gSess", &session) && IS_INSTANCE(session))
        owl_mqtt_ferme(session);
    owl_mock_peer_joint(broker);
    owl_mock_peer_libere(broker);
    vm_free();
}

static void famille_attend(void) {
    cas_attend_pompe();
}

/* ================================================================
 * Réception QoS 2 exactly-once — dédup d'un PUBLISH DUP.
 *
 * Verrouille en test permanent déterministe la dédup côté réception :
 * sur une connexion abonnée, le broker livre un PUBLISH QoS 2 (pid 5)
 * PUIS le retransmet avec DUP=1 (notre PUBREC supposé perdu — cas réseau
 * MQTT standard), PUIS relance par un PUBREL. Le contrat  :
 *   - le handler abonné est appelé EXACTEMENT 1 fois (le DUP ne re-livre
 *     pas : exactly-once applicatif) ;
 *   - côté fil, le client émet 2× PUBREC (un par PUBLISH reçu, DUP compris)
 *     puis 1× PUBCOMP (clôture du handshake sur réception du PUBREL).
 *
 * Tout est piloté par des trames broker pré-empilées (modèle bundle
 * one-shot) : un seul tick du réacteur frame les 2 PUBLISH + le PUBREL et
 * déclenche les réponses du client -> parfaitement déterministe.
 *
 * Le comportement EXISTE DÉJÀ (mqtt_traite_publish + fenêtre qos2_rx_pids) :
 * cette famille passe VERT immédiatement. Un rouge = dédup cassée.
 *
 * PUBLISH QoS 2 : byte0 = 0x30 | (2<<1) = 0x34 ; +DUP(0x08) = 0x3C.
 * remLen = 2 (préfixe sujet) + 10 (sujet) + 2 (packet-id) + 5 (charge) = 19 = 0x13.
 * ================================================================ */

#define QOS2_SUJET  "sujet/test"    /* 10 octets (= SUB_SUJET) */
#define QOS2_CHARGE "hello"         /* 5 octets */

/* CONNACK + SUBACK(pid1, accordé QoS2) + PUBLISH QoS2(pid5) +
 * PUBLISH QoS2 DUP(pid5) + PUBREL(pid5). */
static const char QOS2_BUNDLE[] = {
    0x20, 0x02, 0x00, 0x00,                          /* CONNACK accepte */
    (char)0x90, 0x03, 0x00, 0x01, 0x02,              /* SUBACK pid=1 accorde=QoS2 */
    0x34, 0x13, 0x00, 0x0A,                           /* PUBLISH QoS2 pid=5 */
    's','u','j','e','t','/','t','e','s','t',          /* sujet */
    0x00, 0x05,                                       /* packet-id = 5 */
    'h','e','l','l','o',                              /* charge */
    0x3C, 0x13, 0x00, 0x0A,                           /* PUBLISH QoS2 DUP=1 pid=5 */
    's','u','j','e','t','/','t','e','s','t',          /* sujet (identique) */
    0x00, 0x05,                                       /* packet-id = 5 (même) */
    'h','e','l','l','o',                              /* charge (identique) */
    0x62, 0x02, 0x00, 0x05                            /* PUBREL pid=5 */
};

static void cas_qos2_reception_dedup(void) {
    static const char* CLIENT_ID = "owl-qos2-rx";

    vm_init();

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_ECOUTE;
    mc.port                  = 0;
    mc.octets                = QOS2_BUNDLE;
    mc.octets_len            = sizeof(QOS2_BUNDLE);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.octets_min_lecture    = 12;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 2000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* broker = owl_mock_peer_demarre(&mc);
    CHECK(broker != NULL, "qos2_broker_demarre");
    if (broker == NULL) { vm_free(); return; }
    int port = owl_mock_peer_port(broker);

    char src[1000];
    snprintf(src, sizeof(src),
        "gQAppels est un entier = 0\n"
        "gQSujet est une chaine = \"\"\n"
        "gQCharge est une chaine = \"\"\n"
        "PROCEDURE gQRecoit(sess est un mqttSession, sujet est une chaine, msg est un Buffer)\n"
        "    gQAppels = gQAppels + 1\n"
        "    gQSujet = sujet\n"
        "    gQCharge = msg\n"
        "FIN\n"
        "gSess est un mqttSession\n"
        "gSess..Adresse = \"127.0.0.1\"\n"
        "gSess..Port = %d\n"
        "gSess..IDClient = \"%s\"\n"
        "gConn est un booleen = MQTTConnecte(gSess)\n"
        "gAb est un booleen = Faux\n"
        "SI gConn ALORS\n"
        "    gAb = MQTTAbonne(gSess, \"%s\", mqttQualiteExactementUneFois, gQRecoit)\n"
        "FIN\n",
        port, CLIENT_ID, QOS2_SUJET);

    InterpretResult ir = vm_interpret(src);
    CHECK(ir == INTERPRET_OK, "qos2_harnais_execute");
    CHECK(lit_globale_bool("gConn"), "qos2_connecte");
    CHECK(lit_globale_bool("gAb"), "qos2_abonne");

    Value session;
    CHECK(lit_globale("gSess", &session) && IS_INSTANCE(session), "qos2_session_lue");

    OwlReacteur* r = owl_mqtt_reacteur_session(session);
    CHECK(r != NULL, "qos2_reacteur_session");

    /* Un tick frame tout le bundle (2 PUBLISH pid 5 + PUBREL) et déclenche les
     * réponses du client. La dédup ne doit livrer qu'au PREMIER PUBLISH. */
    if (r) owl_reacteur_tick(r, 2000);

    /* Coeur  : le PUBLISH DUP ne re-livre pas -> handler appelé 1 fois. */
    int64_t appels = -1;
    CHECK(lit_globale_entier("gQAppels", &appels) && appels == 1,
          "qos2_handler_appele_une_seule_fois");
    {
        const char* sujet  = lit_globale_chaine("gQSujet");
        const char* charge = lit_globale_chaine("gQCharge");
        CHECK(sujet != NULL && strcmp(sujet, QOS2_SUJET) == 0,
              "qos2_handler_recoit_le_sujet");
        CHECK(charge != NULL && strcmp(charge, QOS2_CHARGE) == 0,
              "qos2_handler_recoit_la_charge");
    }

    owl_mqtt_ferme(session);
    owl_mock_peer_joint(broker);

    /* Côté fil : 2× PUBREC (0x50 pid5, un par PUBLISH reçu DUP compris) et
     * 1× PUBCOMP (0x70 pid5, émis sur réception du PUBREL). */
    size_t rlen = 0;
    const char* recu = owl_mock_peer_recu(broker, &rlen);
    static const char PUBREC[]  = { 0x50, 0x02, 0x00, 0x05 };
    static const char PUBCOMP[] = { 0x70, 0x02, 0x00, 0x05 };
    CHECK(compte_motif(recu, rlen, PUBREC, sizeof(PUBREC)) == 2,
          "qos2_deux_pubrec_emis");
    CHECK(compte_motif(recu, rlen, PUBCOMP, sizeof(PUBCOMP)) == 1,
          "qos2_un_pubcomp_emis");

    owl_mock_peer_libere(broker);
    vm_free();
}

static void famille_qos2_reception(void) {
    cas_qos2_reception_dedup();
}

/* ================================================================
 * Matching de filtre de sujet MQTT 3.1.1 §4.7 — fonction
 * PURE (aucun reseau). Garde rapide du routage de livraison : sans elle,
 * un filtre wildcard (`owl/#`) ne matche jamais un sujet concret
 * (`owl/x`), et le PUBLISH entrant n'atteint pas le handler abonne.
 * Le `sujet` est passe en (ptr,len) — non NUL-termine, comme dans le
 * tampon rx.
 * ================================================================ */

static void famille_topic_match(void) {
    /* Le sujet est volontairement non NUL-termine : sous-chaine d'un buffer
     * plus large, coupee par la longueur. */
    static const char BUF[] = "a/b/c!!!GARBAGE";  /* "a/b/c" = 5 premiers octets */

    CHECK(mqtt_topic_filter_match("a/b/#", "a/b/c", 5),   "topic_hash_descendant");
    CHECK(mqtt_topic_filter_match("a/b/#", "a/b",   3),   "topic_hash_parent");
    CHECK(!mqtt_topic_filter_match("a/b/#", "a/x",  3),   "topic_hash_rejette_autre_branche");
    CHECK(mqtt_topic_filter_match("a/+/c", "a/x/c", 5),   "topic_plus_un_niveau");
    CHECK(!mqtt_topic_filter_match("a/+/c", "a/x/y/c", 7),"topic_plus_rejette_deux_niveaux");
    CHECK(mqtt_topic_filter_match("#", "a/b/c", 5),       "topic_hash_seul_matche_tout");
    CHECK(mqtt_topic_filter_match("a/b/c", "a/b/c", 5),   "topic_exact_matche");
    CHECK(!mqtt_topic_filter_match("a/b", "a/c", 3),      "topic_litteral_non_match");
    /* Non NUL-termine : seuls les 5 premiers octets de BUF ("a/b/c") comptent,
     * la suite "!!!GARBAGE" est ignoree. */
    CHECK(mqtt_topic_filter_match("a/+/c", BUF, 5),       "topic_ptr_len_ignore_suite");
    CHECK(mqtt_topic_filter_match("a/b/c", BUF, 5),       "topic_ptr_len_exact");
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int mqtt_connexion_self_test_run(void) {
    printf("== Client MQTT 3.1.1 — handshake + publication (feat/1998) ==\n");

    famille_handshake();
    int apres_handshake = g_pass + g_fail;
    printf("   handshake CONNECT/CONNACK/DISCONNECT : %d/%d\n",
           g_pass, apres_handshake);

    famille_publication();
    printf("   publication PUBLISH QoS 0/1 + retain : %d/%d\n",
           g_pass, g_pass + g_fail);

    famille_abonnement();
    printf("   abonnement SUBSCRIBE/SUBACK + livraison + UNSUBSCRIBE : %d/%d\n",
           g_pass, g_pass + g_fail);

    famille_publie_abonne();
    printf("   QoS 1 sur sujet abonné (PUBACK vs écho entrelacé) : %d/%d\n",
           g_pass, g_pass + g_fail);

    famille_attend();
    printf("   pompe MQTTAttend (livraison + connexion/raccrochage) : %d/%d\n",
           g_pass, g_pass + g_fail);

    famille_qos2_reception();
    printf("   réception QoS 2 exactly-once (dédup PUBLISH DUP) : %d/%d\n",
           g_pass, g_pass + g_fail);

    famille_topic_match();
    printf("   matching filtre de sujet §4.7 (fonction pure) : %d/%d\n",
           g_pass, g_pass + g_fail);
    return g_fail;
}
