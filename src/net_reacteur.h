// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_reacteur_h
#define owl_net_reacteur_h

/* ================================================================
 * CŒUR RÉACTEUR — boucle poll-many + roue de timers (chantier W1)
 *
 * Mécanique événementielle PURE, cross-platform (poll / WSAPoll).
 * AUCUNE logique HTTP ici : ce module multiplexe des descripteurs et
 * des échéances, rien d'autre. La machine à états HTTP (W2), le TLS
 * non-bloquant (W3), le réveil inter-thread (W4) et le flux SSE (W5)
 * se BRANCHENT dessus via cette API — ils ne vivent pas dedans.
 *
 * Isolation GC (design §8 Q1) : ce module est 100 % C-heap. Il ne
 * connaît ni la VM, ni les Obj*, ni les Value. Aucun include de
 * object.h / vm.h. C'est du transport pur — un descripteur, un
 * intérêt, une échéance, un pointeur utilisateur opaque. Le GC ne
 * peut par construction voir aucune racine ici : il n'y en a pas.
 *
 * Modèle de threads : 1 boucle unique au départ (design §7.2). Le
 * réacteur n'est PAS thread-safe : toutes les opérations
 * (enregistrement, armement, tick) s'appellent depuis le thread qui
 * possède la boucle. Le réveil depuis un autre thread (self-pipe) est
 * le sujet de W4 — il enregistrera un fd de réveil comme un autre.
 *
 * Réutilise la couche socket existante (owl_socket_fd_t de net_tcp.h,
 * l'horloge monotone de net_tcp_server.c). Le shim poll/WSAPoll est le
 * même que net_tcp_server.c — cinq lignes de plateforme, pas une
 * réinvention de la couche socket.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"   /* owl_socket_fd_t, OWL_INVALID_SOCKET */

#include <stddef.h>
#include <stdint.h>

/* ---------- Intérêts et événements ----------
 *
 * Masque d'intérêt passé à l'enregistrement, et masque d'événements
 * prêts remis au callback. Volontairement découplé de POLLIN/POLLOUT :
 * le vocabulaire poll ne fuit pas hors de ce module. */
#define OWL_REAC_LECTURE   0x01   /* données lisibles (POLLIN) */
#define OWL_REAC_ECRITURE  0x02   /* écriture possible (POLLOUT) */
#define OWL_REAC_ERREUR    0x04   /* fd en erreur / raccroché (POLLERR/HUP/NVAL).
                                   * Toujours remonté même sans être demandé :
                                   * un fd mort doit pouvoir être nettoyé. */

typedef struct OwlReacteur OwlReacteur;

/* Handle opaque de timer. 0 = handle invalide (aucun timer). Un id
 * n'est jamais réutilisé tant que le réacteur vit : un id qui a expiré
 * ou été annulé reste invalide pour toujours (pas d'ABA). */
typedef uint64_t OwlTimerId;
#define OWL_TIMER_NUL ((OwlTimerId)0)

/* Callback fd : appelé quand `fd` est prêt. `evts_prets` est une
 * combinaison de OWL_REAC_LECTURE / _ECRITURE / _ERREUR effectivement
 * survenus. Le callback PEUT muter le réacteur (modifier son intérêt,
 * se désenregistrer, armer/annuler des timers, enregistrer d'autres
 * fds) : c'est sûr pendant le tick courant. */
typedef void (*OwlReacteurFdCb)(OwlReacteur* r, owl_socket_fd_t fd,
                                short evts_prets, void* user);

/* Callback timer : appelé une fois quand l'échéance est atteinte. Le
 * timer est déjà désarmé (one-shot) quand le callback est invoqué :
 * son `id` est mort. Pour un timer récurrent, le callback ré-arme un
 * NOUVEAU timer. Comme le callback fd, il peut muter le réacteur. */
typedef void (*OwlReacteurTimerCb)(OwlReacteur* r, OwlTimerId id, void* user);

/* ---------- Cycle de vie ---------- */

/* Crée un réacteur vide. NULL si allocation impossible. */
OwlReacteur* owl_reacteur_cree(void);

/* Détruit le réacteur et libère ses tables internes. Ne ferme AUCUN
 * fd et n'appelle aucun callback : la propriété des descripteurs reste
 * à l'appelant (le réacteur ne fait que les observer). */
void owl_reacteur_detruit(OwlReacteur* r);

/* Remplace la source d'horloge monotone du réacteur. RÉSERVÉ AUX TESTS.
 *
 * Par défaut, un réacteur lit l'horloge monotone réelle
 * (owl_tcp_server_horloge_ms) : c'est le comportement de TOUTE la
 * production (HTTP / MQTT / AMQP / STOMP / pumps), et aucun de ces
 * appelants n'appelle cette fonction. Elle n'existe que pour piloter
 * une horloge virtuelle dans les self-tests, rendant la roue de timers
 * déterministe sans dépendre du wall-clock (donc insensible à la charge
 * CPU). `horloge` == NULL restaure le défaut réel. */
void owl_reacteur_horloge_test(OwlReacteur* r, uint64_t (*horloge)(void));

/* ---------- Descripteurs ---------- */

/* Enregistre `fd` avec `interet` (OWL_REAC_LECTURE / _ECRITURE, ou les
 * deux). `cb` est invoqué à chaque tick où le fd est prêt. `user` est
 * opaque, remis tel quel au callback.
 * false si fd invalide, déjà enregistré, ou allocation impossible. */
bool owl_reacteur_ajoute_fd(OwlReacteur* r, owl_socket_fd_t fd,
                            short interet, OwlReacteurFdCb cb, void* user);

/* Remplace l'intérêt d'un fd déjà enregistré. Sert au cœur de la
 * gestion POLLOUT : on n'enregistre l'intérêt d'écriture QUE quand on
 * a des octets en attente, et on le retire dès que le tampon est vidé
 * (sinon busy-loop sur un POLLOUT toujours prêt). false si inconnu. */
bool owl_reacteur_modifie_interet(OwlReacteur* r, owl_socket_fd_t fd,
                                  short interet);

/* Désenregistre `fd`. Il ne sera plus jamais notifié. Ne ferme pas le
 * fd (propriété appelant). false si inconnu. Sûr depuis un callback,
 * y compris le désenregistrement du fd en cours de dispatch. */
bool owl_reacteur_retire_fd(OwlReacteur* r, owl_socket_fd_t fd);

/* Nombre de fds actuellement enregistrés. */
size_t owl_reacteur_nb_fds(const OwlReacteur* r);

/* ---------- Roue de timers ----------
 *
 * Timers one-shot : chaque timer expire une fois puis disparaît. Les
 * 4 timeouts métier (inactivité / en-têtes / corps / écriture, W2) se
 * construisent dessus — une échéance d'inactivité se ré-arme à chaque
 * octet reçu ; si elle expire, la connexion a dépassé son délai. La
 * boucle calcule le timeout `poll` minimal à partir de la plus proche
 * échéance : au repos, elle bloque exactement jusqu'à la prochaine
 * échéance, pas plus (pas de spin, pas de dérive de réveil). */

/* Arme un timer à `delai_ms` dans le futur (depuis maintenant).
 * Retourne son id (> 0), ou OWL_TIMER_NUL si allocation impossible.
 * delai_ms == 0 => échéance immédiate (fire au prochain tick). */
OwlTimerId owl_reacteur_arme_timer(OwlReacteur* r, uint64_t delai_ms,
                                   OwlReacteurTimerCb cb, void* user);

/* Ré-arme un timer PENDANT (pas encore expiré) à `delai_ms` dans le
 * futur, en conservant son id et son callback. C'est le geste du
 * timeout d'inactivité : « repousse l'échéance ». false si l'id est
 * inconnu / déjà expiré / annulé. */
bool owl_reacteur_rearme_timer(OwlReacteur* r, OwlTimerId id, uint64_t delai_ms);

/* Annule un timer pendant. Son callback ne sera jamais appelé. false
 * si l'id est inconnu / déjà expiré / annulé. */
bool owl_reacteur_annule_timer(OwlReacteur* r, OwlTimerId id);

/* Nombre de timers actuellement pendants. */
size_t owl_reacteur_nb_timers(const OwlReacteur* r);

/* ---------- Boucle ---------- */

/* Un tour de boucle : calcule le timeout `poll` effectif = min(plus
 * proche échéance de timer, `timeout_plafond_ms`), attend, puis
 * dispatche les fds prêts (callbacks fd) et les timers échus
 * (callbacks timer). Les fds sont dispatchés AVANT les timers : une
 * connexion qui a reçu des données peut ré-armer son timeout
 * d'inactivité et échapper à l'expiration dans le même tick.
 *
 * `timeout_plafond_ms` : borne supérieure de l'attente en l'absence de
 * timer plus proche. < 0 = pas de plafond (attente jusqu'au prochain
 * événement / timer). 0 = ne bloque pas (poll non bloquant).
 *
 * Retourne le nombre de dispatchs effectués (fds prêts + timers échus)
 * pour ce tick, 0 sur simple expiration du timeout, -1 sur erreur poll
 * irrécupérable. Un tick sans fd ni timer avec plafond < 0 retourne 0
 * immédiatement (rien à attendre). */
int owl_reacteur_tick(OwlReacteur* r, int timeout_plafond_ms);

/* Boucle jusqu'à demande d'arrêt : enchaîne les ticks. Rend la main
 * quand owl_reacteur_demande_arret a été appelé (depuis un callback,
 * ou — plus tard, W4 — depuis le self-pipe d'un autre thread). Le
 * plafond interne borne la latence d'arrêt tant que W4 n'a pas posé de
 * fd de réveil. */
void owl_reacteur_boucle(OwlReacteur* r);

/* Demande l'arrêt de owl_reacteur_boucle. Idempotent. Peut être appelé
 * depuis un callback (thread de boucle). L'appel depuis un autre
 * thread est visible au plus tard au plafond de latence d'arrêt ; le
 * réveil immédiat inter-thread est le sujet de W4. */
void owl_reacteur_demande_arret(OwlReacteur* r);

/* true si l'arrêt a été demandé. */
bool owl_reacteur_arret_demande(const OwlReacteur* r);

#endif /* owl_net_reacteur_h */
