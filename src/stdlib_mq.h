// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_mq_h
#define owl_stdlib_mq_h

/* ================================================================
 * SOCLE MESSAGE QUEUE — E1 « pont de livraison »
 *
 * Mecanique COMMUNE et AGNOSTIQUE DU PROTOCOLE : quand un message
 * entrant est pret sur le reacteur, une procedure OWL enregistree est
 * invoquee avec le payload. E1 ne parse AUCUN protocole (MQTT/AMQP/
 * STOMP/NATS = tickets par lot ensuite) : il recoit un payload deja
 * delimite et appelle le handler. C'est la brique sous le modele
 * canon MQTT (consommation en callback-procedure).
 *
 * Modele = POMPE EXPLICITE. Le programme OWL est un worker qui boucle
 * et pilote le reacteur (owl_reacteur_tick / owl_reacteur_boucle).
 * AUCUN thread de fond, aucun callback « magique » : la livraison ne
 * se declenche QUE pendant un tick de pompe.
 *
 * Isolation GC (design §8 Q1) : ce module est la SEULE moitie du socle
 * MQ qui connait la VM et les Value. net_reacteur reste 100 % C-heap.
 * Les handlers OWL enregistres sont tenus comme RACINES GC via le
 * registre de racines persistantes (gc.h) : un handler abonne ne peut
 * pas etre collecte tant que son abonnement vit.
 *
 * E1 N'EXPOSE AUCUNE NATIVE OWL : pas de registry, pas de bilingue,
 * pas de surface user. Pur enabler C, pilote par les clients
 * protocolaires a venir.
 * ================================================================ */

#include "value.h"
#include "net_reacteur.h"
#include "net_tcp.h"   /* owl_socket_fd_t */

#include <stddef.h>

/* Handle d'abonnement opaque. 0 = invalide (aucun abonnement). */
typedef int OwlMqHandle;
#define OWL_MQ_HANDLE_NUL 0

/* Nombre maximal d'abonnements simultanes du socle. Borne volontaire :
 * chaque slot est enregistre comme racine GC persistante (gc.h, cap 64
 * partagee). Augmentable ; un depassement d'abonnement est refuse
 * (retour OWL_MQ_HANDLE_NUL), pas un mode degrade silencieux. */
#define OWL_MQ_MAX_ABONNEMENTS 8

/* ---------- Cycle de vie du registre d'abonnements ---------- */

/* Prepare le registre : vide les slots et enregistre chacun comme racine
 * GC persistante. Idempotent — un 2e appel est un no-op qui PRESERVE les
 * abonnements actifs (contrat multi-protocole : plusieurs clients MQ
 * partagent le socle sans se clobberer). A appeler avant tout owl_mq_abonne. */
void owl_mq_registre_init(void);

/* Init du socle MQ, point central neutre appele par owl_stdlib_init (avant
 * les clients protocolaires). Delegue a owl_mq_registre_init. */
void stdlib_mq_init(void);

/* Vide tous les abonnements (les handlers ne sont plus tenus). Les
 * slots restent enregistres comme racines (NIL_VAL). */
void owl_mq_registre_libere(void);

/* ---------- Abonnements ---------- */

/* Enregistre un handler OWL (procedure/closure). Retourne un handle
 * > 0, ou OWL_MQ_HANDLE_NUL si `handler` n'est pas appelable ou si le
 * registre est plein. Le handler est tenu comme racine GC tant que
 * l'abonnement vit. */
OwlMqHandle owl_mq_abonne(Value handler);

/* Retire l'abonnement `h` : son handler n'est plus tenu comme racine.
 * false si `h` est inconnu / deja retire. */
bool owl_mq_desabonne(OwlMqHandle h);

/* Lit le handler tenu par `h`. false (et *out inchange) si inconnu. */
bool owl_mq_handler(OwlMqHandle h, Value* out);

/* ---------- Livraison C -> OWL ---------- */

/* Livre `payload` (len octets, deja delimite par le client
 * protocolaire) au handler de l'abonnement `h` : marshalle le payload
 * en Value OWL et invoque le handler via vm_call_reentrant, dans le
 * respect de la frontiere de reentrance bornee.
 *
 * A n'appeler QUE pendant un tick de pompe (thread proprietaire de la
 * VM). `h` inconnu -> no-op. */
void owl_mq_deliver(OwlMqHandle h, const char* payload, size_t len);

/* ---------- Ancrage reacteur (pompe) ---------- */

/* Branche `fd` sur `r` en interet LECTURE, en l'associant a
 * l'abonnement `h`. Quand des octets arrivent pendant un tick, ils
 * sont lus et livres au handler via owl_mq_deliver. Framing trivial :
 * un bloc lu = un payload (le decoupage protocolaire viendra du client
 * qui frame en amont). false si fd/handle invalide ou fd deja branche. */
bool owl_mq_reacteur_branche(OwlReacteur* r, owl_socket_fd_t fd, OwlMqHandle h);

#endif /* owl_stdlib_mq_h */
