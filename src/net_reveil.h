// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_reveil_h
#define owl_net_reveil_h

/* ================================================================
 * RÉVEIL INTER-THREAD — self-pipe portable (chantier W4)
 *
 * Problème : la boucle réacteur (W1) dort dans poll/WSAPoll. Un thread
 * TIERS (l'exécuteur) veut la réveiller pour lui dire « chunk dispo »
 * / « job fini ». Il faut un descripteur qu'un autre thread peut
 * « toucher » et qui rende le poll de la boucle immédiatement prêt.
 *
 * Pas d'eventfd : absent de Windows. Pas de _pipe/CreatePipe : les
 * handles pipe Windows ne sont PAS pollables par WSAPoll. Seul dénomi-
 * nateur commun portable et pollable : une PAIRE DE SOCKETS loopback
 * (127.0.0.1). C'est le « self-pipe trick » classique, décliné socket.
 *
 * Deux bouts :
 *   - fd_lecture  : enregistré dans le réacteur (côté boucle), drainé
 *                   sans bloquer quand il devient lisible.
 *   - fd_ecriture : le thread tiers y écrit 1 octet pour réveiller.
 *
 * Coalescing : plusieurs signals avant un drain → un seul réveil
 * suffit (le contenu de l'octet n'a aucun sens ; c'est le fait qu'il
 * y ait DES octets qui compte). Si le tampon d'envoi est saturé (des
 * réveils déjà en attente non drainés), l'écriture non-bloquante
 * échoue silencieusement : un octet est déjà pending, la boucle sera
 * réveillée de toute façon. Pas de perte de réveil, pas de blocage.
 *
 * Isolation GC (design §8 Q1) : AUCUN objet GC (Obj, ObjString, Value)
 * ne traverse la frontière de thread. SEULS des octets C-heap. Ce module
 * n'inclut ni object.h ni vm.h et ne connaît pas la VM : le thread de
 * boucle, réveillé par ce self-pipe, n'entre JAMAIS dans threads_vm.
 * L'octet est un pur signal « quelque chose t'attend, regarde tes
 * files » — la charge utile réelle voyage par les structures C-heap
 * de l'exécuteur (condvars internes conservées, pont côté exécuteur
 * = W5). Zéro nouveau point GC.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"       /* owl_socket_fd_t, OWL_INVALID_SOCKET */
#include "net_reacteur.h"  /* OwlReacteur */

/* Callback réveil : appelé sur le THREAD DE BOUCLE quand le self-pipe
 * a été signalé et drainé. Pas de fd, pas d'octet remis : c'est un pur
 * « on t'a réveillé, va regarder tes files C-heap ». `user` est remis
 * tel quel. Peut être NULL (réveil nu : sortir du poll suffit). */
typedef void (*OwlReveilCb)(void* user);

typedef struct {
    owl_socket_fd_t fd_lecture;    /* côté boucle : enregistré + drainé */
    owl_socket_fd_t fd_ecriture;   /* côté thread tiers : signalé */
    OwlReveilCb     cb;            /* invoqué côté boucle après drain */
    void*           user;         /* opaque, remis au callback */
} OwlReveil;

/* Crée la paire loopback (deux sockets 127.0.0.1 connectées entre
 * elles), les met en non-bloquant, remplit `rv`. `cb`/`user` sont le
 * callback appelé côté boucle après chaque drain (cb peut être NULL).
 * false si la paire n'a pas pu être créée (fds laissés invalides). */
bool owl_reveil_cree(OwlReveil* rv, OwlReveilCb cb, void* user);

/* Enregistre le côté LECTURE dans le réacteur : à chaque tick où le
 * self-pipe est lisible, le réacteur draine tous les octets pending
 * puis appelle `cb`. À appeler depuis le thread de boucle (le réacteur
 * n'est pas thread-safe). false si l'enregistrement échoue. */
bool owl_reveil_enregistre(OwlReveil* rv, OwlReacteur* r);

/* Signale le réveil : écrit 1 octet dans fd_ecriture. THREAD-SAFE —
 * seule opération de ce module appelable depuis un AUTRE thread que
 * celui de la boucle (c'est tout le point du self-pipe). Non-bloquant,
 * idempotent/coalescé : si un réveil est déjà pending, l'écriture qui
 * échoue (tampon plein) est ignorée sans conséquence. */
void owl_reveil_signale(OwlReveil* rv);

/* Désenregistre du réacteur (si `r` non NULL) puis ferme les deux
 * fds. À appeler depuis le thread de boucle. Après cet appel plus
 * aucun signale n'est valide. Idempotent sur fds déjà fermés. */
void owl_reveil_detruit(OwlReveil* rv, OwlReacteur* r);

#endif /* owl_net_reveil_h */
