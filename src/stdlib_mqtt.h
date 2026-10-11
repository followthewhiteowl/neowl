// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_mqtt_h
#define owl_stdlib_mqtt_h

/* Client MQTT 3.1.1 — premier client protocolaire concret sur
 * le socle Message Queue E1 (, ).
 *
 * Lot A : type reifie mqttSession (parametres de connexion) + natives
 * MQTTConnecte / MQTTDéconnecte (handshake CONNECT/CONNACK + DISCONNECT).
 * Lot B : MQTTPublie (PUBLISH QoS 0/1 + retain).
 * Lot C : MQTTAbonne / MQTTDésabonne / MQTTDésabonneTout (SUBSCRIBE/SUBACK ;
 *         PUBLISH entrant -> procedure OWL abonnee via E1, modele pompe).
 *  : MQTTAttend (pompe explicite pilotee depuis OWL ; reacteur
 *         possede par la session).
 *
 * Le wire MQTT (encodage des trames) suit la spec OASIS MQTT 3.1.1.
 * La surface OWL (noms, signatures, retours) suit le canon. */

#include "value.h"
#include <stdbool.h>
#include <stddef.h>

void stdlib_mqtt_init(void);

/* Matching de filtre de sujet MQTT 3.1.1 §4.7 (chemin de livraison). `filtre`
 * est NUL-termine ; `sujet` pointe sur `sujet_len` octets, pas forcement
 * NUL-termine. '+' = un niveau ; '#' = niveau parent + descendants (dernier
 * segment) ; litteral = egalite stricte. Sans wildcard = egalite exacte.
 * Expose (non-static) pour le self-test C-interne du matching pur. */
bool mqtt_topic_filter_match(const char* filtre, const char* sujet, size_t sujet_len);

/* ---------- Pompe possedee par la session ----------
 *
 * Reacteur opaque (net_reacteur.h) : forward-decl de la struct pour ne
 * pas tirer net_tcp/winsock via cet en-tete. `OwlReacteur` (typedef) et
 * `struct OwlReacteur` designent le meme type.
 *
 * La session POSSEDE son reacteur (cree paresseusement au 1er abonnement
 * ou a la 1re attente ; fd branche en interet LECTURE). La surface
 * utilisateur de pompe est la native MQTTAttend : elle ticke ce reacteur,
 * un PUBLISH entrant etant framé (spec MQTT 3.1.1) et livre a la procedure
 * abonnee via E1 avec (session, sujet, message Buffer). */
struct OwlReacteur;

/* Rend le reacteur possede par une session mqttSession CONNECTEE (le cree +
 * branche le fd si besoin). Seam de test C-interne pour un tick manuel a bas
 * niveau ; la surface utilisateur est MQTTAttend. NULL si `session` n'est pas
 * une session connectee, ou si le reacteur ne peut etre cree/branche.
 * A n'appeler que depuis le thread proprietaire de la VM. */
struct OwlReacteur* owl_mqtt_reacteur_session(Value session);

/* Ferme la connexion vivante d'une session sans emettre de DISCONNECT
 * (teardown C : referme la socket, libere le transport). No-op si la
 * session n'est pas connectee. */
void owl_mqtt_ferme(Value session);

#endif /* owl_stdlib_mqtt_h */
