// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_amqp_h
#define owl_stdlib_amqp_h

/* Client AMQP 0-9-1 / RabbitMQ — 2e client protocolaire sur le
 * socle Message Queue E1 (, ), apres MQTT.
 *
 * Lot A (courant) : type reifie amqpConnection (parametres de connexion) +
 * natives AMQPConnecte / AMQPDéconnecte. La connexion ouvre UN channel auto
 * en interne (channels CACHES cote utilisateur, decision surface hors-canon
 * validee dev 2026-09-15). Ce Lot A est livre en STUB : la surface + le type
 * sont enregistres au registry, mais le handshake wire n'est pas encore code
 * (AMQPConnecte renvoie Faux). La lane conformance RabbitMQ est donc ROUGE
 * tant que le Lot A n'est pas passe au vert (gate dev sur l'encodage wire).
 *
 * Lots ulterieurs : DeclareQueue/DeclareEchange/Lie, Publie (basic.publish),
 * Consomme (basic.consume) + livraison E1, Attend (pompe), Acquitte (ack).
 *
 * Le wire AMQP 0-9-1 (frames method/header/body, field-tables, SASL PLAIN)
 * n'est PAS dans la doc : reference = spec OASIS AMQP 0-9-1. La surface
 * OWL (noms, signatures) est HORS-CANON (aucun equivalent canon) : concue puis
 * validee dev, record ADR de cloture. */

#include "value.h"

void stdlib_amqp_init(void);

#endif /* owl_stdlib_amqp_h */
