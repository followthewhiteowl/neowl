// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_nats_h
#define owl_stdlib_nats_h

/* Client NATS — 4e client protocolaire sur le socle Message Queue
 * E1 (, ), apres MQTT, AMQP et STOMP
 *.
 *
 * Lot 1 (courant) : le coeur. Types reifies natsConnexion (parametres de
 * connexion) + natsMessage (message de 1re classe, symetrique publish/consume :
 * Sujet, Corps, RépondreÀ). Natives NATSConnecte / NATSDéconnecte / NATSPublie
 * (PUB) / NATSAbonne (SUB, groupeFile optionnel = queue group, livraison E1 des
 * MSG entrants) / NATSAttend (pompe le reacteur possede par la session,
 * , keepalive PING/PONG). Round-trip pub/sub en TCP clair.
 *
 * Le protocole NATS est TEXTE (lignes terminees par \r\n : INFO / CONNECT /
 * PING / PONG / PUB / SUB / MSG / +OK / -ERR) et n'a aucun equivalent canon : la
 * surface OWL (noms, signatures) est HORS-CANON, concue puis validee dev
 * 2026-09-17. Reference wire = protocole client NATS (nats.io).
 *
 * Lots ulterieurs : request-reply + inbox (Lot 2), TLS via optionSSL +
 * CertificatCA (Lot 3, mecanisme ). */

#include "value.h"

void stdlib_nats_init(void);

#endif /* owl_stdlib_nats_h */
