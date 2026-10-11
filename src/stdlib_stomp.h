// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_stomp_h
#define owl_stdlib_stomp_h

/* Client STOMP 1.2 — 3e client protocolaire sur le socle Message
 * Queue E1 (, ), apres MQTT et AMQP.
 *
 * Lot 1 (courant) : le coeur. Types reifies stompSession (parametres de
 * connexion) + stompMessage (message de 1re classe, symetrique publish/
 * consume). Natives STOMPConnecte / STOMPDéconnecte / STOMPPublie (SEND) /
 * STOMPAbonne (SUBSCRIBE ack:auto, livraison E1 des MESSAGE entrants) /
 * STOMPAttend (pompe le reacteur possede par la session, ).
 * Round-trip send/receive en auto-ack.
 *
 * Le protocole STOMP 1.2 est TEXTE (frames "COMMANDE\nheader:valeur\n\ncorps\0")
 * et n'a aucun equivalent canon : la surface OWL (noms, signatures) est HORS-CANON,
 * concue puis validee dev 2026-09-17 (record ADR de cloture). Reference wire =
 * spec STOMP 1.2 (stomp.github.io).
 *
 * Lots ulterieurs : ack client + STOMPAcquitte (Lot 2), TLS via optionSSL +
 * CertificatCA (Lot 3, mecanisme ). */

#include "value.h"

void stdlib_stomp_init(void);

#endif /* owl_stdlib_stomp_h */
