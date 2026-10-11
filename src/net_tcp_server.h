// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_tcp_server_h
#define owl_net_tcp_server_h

/* ================================================================
 * SOCKET SERVEUR TCP (cross-platform Win32 / POSIX)
 *
 * Pourquoi un fichier separe de net_tcp.{c,h} et pas une greffe :
 * l'en-tete de net_tcp.h annonce noir sur blanc « API blocante,
 * synchrone, simple. Pas d'async ni de select — on n'en a pas besoin
 * pour le client SQL ». C'est exact et c'est la bonne API pour un
 * client (un appel = une requete = un round-trip). Un serveur a
 * besoin de l'inverse : socket d'ecoute, accept, et surtout des
 * lectures/ecritures a echeance (poll / WSAPoll) — sans quoi il n'y a
 * aucune parade slowloris. Greffer ca dans net_tcp reviendrait a
 * mentir dans son en-tete et a trainer poll() dans le chemin du
 * client SQL, qui n'en veut pas.
 *
 * Volontairement AGNOSTIQUE du protocole applicatif : rien de HTTP
 * ici. Le cadrage HTTP/1.1 vit dans http_server.{c,h}.
 *
 * Depuis A1.1b : pool de threads RESEAU (niveau 1 de concurrence du
 * contrat §1). Plusieurs connexions menees en parallele, sans que la VM
 * devienne concurrente — ce fichier ne la connait toujours pas.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"   /* owl_socket_fd_t, OWL_INVALID_SOCKET */
#include "net_tls_server.h"

#include <stddef.h>

/* Codes de retour des operations a echeance. Les valeurs positives
 * d'owl_tcp_conn_lit sont un nombre d'octets. */
#define OWL_TCPS_OK       0
#define OWL_TCPS_TIMEOUT (-1)
#define OWL_TCPS_FERME   (-2)   /* EOF propre du pair */
#define OWL_TCPS_ERREUR  (-3)

typedef struct {
    owl_socket_fd_t fd;
    int             port;       /* port effectif (utile si port 0 demande) */
    char            erreur[256];
} OwlTcpListener;

typedef struct {
    owl_socket_fd_t fd;
    char            pair[80];   /* "127.0.0.1:54321" */
    /* Session TLS quand la connexion est chiffree, NULL sinon (A1.2).
     * owl_tcp_conn_lit / _ecrit / _ferme y routent quand il est pose :
     * c'est ce qui rend le transport REMPLACABLE sans que les couches
     * du dessus le sachent. http_server.c n'utilise que ces trois
     * fonctions et n'a donc pas ete modifie d'une ligne — corpus,
     * politique et concurrence restent rejouables tels quels. */
    OwlTlsSession*  tls;
} OwlTcpConn;

/* ---------- Ecoute ---------- */

/* bind + listen sur hote:port. port == 0 -> port ephemere, relu dans
 * l->port.
 *
 * autoriser_non_loopback : garde-fou de bind (contrat de transport
 * §10bis b). TLS serveur n'existe pas en A0 ; s'attacher a 0.0.0.0
 * exposerait du clair sur le reseau. Refuse par defaut, deverrouillable
 * explicitement par l'appelant qui assume d'etre derriere un proxy. */
bool owl_tcp_listener_ouvre(OwlTcpListener* l, const char* hote, int port,
                            int backlog, bool autoriser_non_loopback);

/* Meme chose, mais le garde-fou est lie a la presence REELLE d'un
 * materiel TLS : c'est le SEUL endroit ou autoriser_non_loopback
 * devient vrai. La signature d'owl_tcp_listener_ouvre ne bouge pas d'un
 * octet — un materiel qui ne parse pas n'existe pas (cf.
 * owl_tls_serveur_materiel_cree), donc « TLS configure » ne peut pas se
 * reduire a « un pointeur non nul ». */
bool owl_tcp_listener_ouvre_tls(OwlTcpListener* l, const char* hote, int port,
                                int backlog, const OwlTlsServeurMateriel* tls);

int  owl_tcp_listener_port(const OwlTcpListener* l);
const char* owl_tcp_listener_erreur(const OwlTcpListener* l);
void owl_tcp_listener_ferme(OwlTcpListener* l);

/* Attend une connexion au plus timeout_ms.
 * OWL_TCPS_OK / OWL_TCPS_TIMEOUT / OWL_TCPS_ERREUR. */
int owl_tcp_listener_accepte(OwlTcpListener* l, int timeout_ms, OwlTcpConn* out);

/* ---------- Connexion acceptee ---------- */

/* > 0 : octets lus. Sinon OWL_TCPS_TIMEOUT / _FERME / _ERREUR. */
int  owl_tcp_conn_lit(OwlTcpConn* c, void* buf, size_t len, int timeout_ms);

/* Ecrit la totalite de buf, en gerant les send() partiels.
 * OWL_TCPS_OK / OWL_TCPS_TIMEOUT / OWL_TCPS_ERREUR. */
int  owl_tcp_conn_ecrit(OwlTcpConn* c, const void* buf, size_t len, int timeout_ms);

void owl_tcp_conn_ferme(OwlTcpConn* c);

/* Attente d'un evenement sur un descripteur. lecture != 0 -> lecture,
 * sinon ecriture. 1 = pret, 0 = echeance atteinte, -1 = erreur.
 * Expose pour que le handshake TLS re-polle sur le BON SENS sans que
 * poll/WSAPoll vive a deux endroits. */
int owl_tcp_attend(owl_socket_fd_t fd, int lecture, int timeout_ms);

/* ================================================================
 * POOL DE WORKERS RESEAU (A1.1b)
 *
 * N threads, un `accept` serialise, une connexion menee de bout en bout
 * par le worker qui l'a acceptee. Volontairement agnostique du protocole :
 * le pool ne sait pas ce que `sert` fait de la connexion.
 *
 * Le plafond de connexions SIMULTANEES est distinct du nombre de
 * workers : c'est lui qui rend le vecteur 3.5 mesurable. Les connexions
 * excedentaires ne sont pas acceptees — elles restent dans le backlog
 * d'ecoute, donc AUCUN descripteur supplementaire n'est ouvert. Refuser
 * d'accepter est ce qui distingue une contre-pression d'un epuisement.
 * ================================================================ */

/* Le worker POSSEDE `conn` pendant tout l'appel, et la ferme. */
typedef void (*OwlTcpPoolSert)(OwlTcpConn* conn, void* user);

typedef struct {
    int nb_workers;         /* 0 -> 4 */
    int connexions_max;     /* connexions acceptees simultanement. 0 -> nb_workers */
    /* Periode de reveil de l'accept. Ce n'est pas un reglage de
     * performance : c'est la latence maximale entre une demande d'arret
     * et la sortie d'un worker. 0 -> 50 ms. */
    int accept_timeout_ms;

    /* Front reacteur uniquement : echeance ABSOLUE de lecture de la ligne de
     * requete AVANT classification (parade slowloris de la phase pending). Le
     * pool ne connait pas les limites HTTP ; l'appelant y cable son propre
     * timeout d'en-tetes (OwlHttpLimites.timeout_entetes_ms). 0 -> defaut
     * interne. Le service reapplique ensuite ses timeouts fins. */
    int reac_classe_timeout_ms;

    /* TLS serveur (A1.2). NULL -> le pool sert en clair, a l'octet
     * pres comme avant ce lot. Non NULL -> chaque worker se construit
     * SON contexte mbedTLS depuis ce materiel au demarrage du pool, et
     * fait le handshake juste apres l'accept, avant `sert`. Le materiel
     * doit survivre au pool. */
    const OwlTlsServeurMateriel* tls;
    /* Echeance ABSOLUE du handshake. 0 -> 10 s. Une poignee de main qui
     * n'aboutit pas est un vecteur d'epuisement, exactement comme un
     * slowloris : elle est bornee comme le reste. */
    int tls_handshake_timeout_ms;
} OwlTcpPoolConfig;

typedef struct OwlTcpPool OwlTcpPool;

void owl_tcp_pool_config_defaut(OwlTcpPoolConfig* cfg);

/* Le listener et la config de service doivent survivre au pool. NULL si
 * aucun thread n'a pu demarrer. */
OwlTcpPool* owl_tcp_pool_demarre(OwlTcpListener* l, const OwlTcpPoolConfig* cfg,
                                 OwlTcpPoolSert sert, void* user);

/* ================================================================
 * FRONT REACTEUR (, W2) — coexistence reacteur / pool
 *
 * Un thread reacteur (net_reacteur.c) possede l'accept de TOUTES les
 * connexions — NON borne par connexions_max : il accepte tout et met en
 * file, ce qui supprime la famine d'accept du pool worker-par-connexion.
 * Apres lecture des premiers octets (peek, non consommes), il CLASSE la
 * connexion :
 *   - route de CONTROLE -> servie INLINE sur le thread reacteur (rapide,
 *     ne touche NI VM NI modele, design §8 Q1). Ne consomme pas de worker ;
 *   - route DATA-PLANE  -> dispatchee FIFO a un pool de workers borne
 *     (concurrence = min(nb_workers, connexions_max)), qui servent en
 *     bloquant comme le pool historique (drain de flux compris).
 *
 * `sert` est le MEME point d'entree que le pool historique (agnostique du
 * protocole) : la seule difference entre controle et data-plane est QUI
 * l'appelle, jamais COMMENT — meme fonction de service, donc aucune
 * divergence de frontiere anti-smuggling.
 * ================================================================ */

/* Classification depuis les premiers octets PEEK (non consommes). */
#define OWL_TCPS_CLASSE_INCOMPLET (-1)   /* pas encore assez d'octets */
#define OWL_TCPS_CLASSE_DATAPLANE   0    /* dispatch a un worker */
#define OWL_TCPS_CLASSE_CONTROLE    1    /* servi inline sur le reacteur */

/* Rendue par le classifieur sur les octets peekes. Ne consomme rien : le
 * service (worker ou inline) relira les octets depuis le noyau. */
typedef int (*OwlTcpClassifie)(const char* octets, size_t len, void* user);

/* ================================================================
 * DRAIN DE FLUX PILOTE PAR LE REACTEUR (, W5)
 *
 * Un flux (SSE, corps chunked long) ne doit PLUS epingler un worker le
 * temps de sa duree (design §1). Le worker fait le SETUP (lecture requete +
 * handler), puis, si la reponse est un flux, REND la connexion au reacteur
 * via owl_tcp_pool_reac_prise : le reacteur draine les chunks au fil de
 * POLLOUT, reveille par le producteur (self-pipe W4). Le worker est libere
 * immediatement — c'est ce qui supprime la retention (le coeur du hang).
 *
 * Le pool reste AGNOSTIQUE du protocole : le cadrage chunked et le cycle de
 * vie de la reponse vivent dans les callbacks fournis par la couche appli
 * (http_server.c). Ici on ne connait qu'un `appctx` opaque a faire avancer.
 * Aucun objet GC ne traverse : que des octets C-heap (design §8 Q1).
 * ================================================================ */

/* Verdict d'un pas de drain (callback OwlTcpFluxDrain), sur le thread boucle. */
typedef enum {
    OWL_TCPS_FLUX_ATTEND = 0,  /* rien a ecrire maintenant : couper POLLOUT,
                                * attendre un reveil producteur (anti-spin) */
    OWL_TCPS_FLUX_ENCORE = 1,  /* des octets restent a ecrire : garder POLLOUT */
    OWL_TCPS_FLUX_FINI   = 2   /* termine : le reacteur ferme la conn + detruit */
} OwlTcpFluxProgres;

/* Avance le drain d'un flux pris en charge. Appele sur le thread boucle
 * quand le fd est pret en ecriture (ou apres un reveil producteur). `evts`
 * porte OWL_REAC_* prets (ERREUR = pair raccroche mi-flux -> FINI attendu).
 * Ne bloque jamais : ecrit ce qui passe, tire ce qui est dispo, rend son
 * verdict. */
typedef OwlTcpFluxProgres (*OwlTcpFluxDrain)(OwlTcpConn* conn, void* appctx,
                                             short evts, void* user);
/* Libere le contexte appli d'un flux (thread boucle), au FINI ou a l'arret. */
typedef void (*OwlTcpFluxDetruit)(void* appctx, void* user);

/*  P5 — interroge la borne de DUREE applicative d'un flux (DelaiMax).
 * Appele apres chaque pas de drain TANT QUE le flux n'est pas resolu. Retour :
 *   < 0  : flux pas encore resolu (exec async pas encore FLUX) -> re-interroger
 *   == 0 : resolu, PAS de borne (DelaiMax=0 illimite, ou reponse bufferisee)
 *   > 0  : resolu, armer un timer de `retour` ms qui coupera le flux a l'echeance
 * Ce timer est DISTINCT de l'echeance de synthese 503 (W7) : il borne la duree
 * d'un flux VIVANT, la ou W7 borne l'attente d'un flux qui n'a jamais demarre. */
typedef int64_t (*OwlTcpFluxDelaiMax)(void* appctx, void* user);

/* Service data-plane cote reacteur. Sert la connexion (lecture + handler).
 * Rend 1 si un flux a ete PRIS EN CHARGE (l'appli a appele
 * owl_tcp_pool_reac_prise, la conn est passee au reacteur), 0 si la
 * connexion a ete servie en entier et fermee. `p` permet l'appel a
 * owl_tcp_pool_reac_prise / _signale. NULL -> repli sur `sert` (pas de
 * flux reacteur, drain inline comme avant). */
typedef int (*OwlTcpPoolSertDp)(OwlTcpPool* p, OwlTcpConn* conn, void* user);

/* Le listener, la config de service et le classifieur doivent survivre au
 * pool. NULL si aucun thread n'a pu demarrer.
 *
 * `sert` : service inline du plan de controle (inchange). `sert_dp` :
 * service data-plane capable de prise en charge de flux (peut etre NULL ->
 * repli sur `sert`). `drain`/`detruit`/`flux_user` : callbacks de drain des
 * flux pris en charge (requis si `sert_dp` peut prendre en charge). `delaimax`
 * ( P5) : borne de duree applicative par flux, NULL = aucune borne. */
OwlTcpPool* owl_tcp_pool_reacteur_demarre(OwlTcpListener* l,
                                          const OwlTcpPoolConfig* cfg,
                                          OwlTcpPoolSert sert, void* user,
                                          OwlTcpClassifie classe, void* classe_user,
                                          OwlTcpPoolSertDp sert_dp,
                                          OwlTcpFluxDrain drain,
                                          OwlTcpFluxDetruit detruit,
                                          OwlTcpFluxDelaiMax delaimax,
                                          void* flux_user);

/* Prend en charge un flux : transfere la conn (par valeur) et `appctx` au
 * reacteur, qui les pilotera via drain/detruit. THREAD-SAFE (appelable
 * depuis un worker). `conn->fd` est invalide au retour (propriete transferee)
 * pour eviter toute double-fermeture. false si le pool n'est pas en mode
 * reacteur ou est en arret (l'appelant garde alors la conn et doit la
 * fermer).
 *
 * `echeance_ms` (W7) : echeance ABSOLUE (owl_tcp_server_horloge_ms) apres
 * laquelle le reacteur re-pousse un pas de drain — le temps qu'un drain en
 * attente (exec async pas encore resolu) voie l'expiry et synthetise sa
 * reponse. 0 = pas d'echeance (flux W5 classique : le reveil producteur
 * suffit). */
bool owl_tcp_pool_reac_prise(OwlTcpPool* p, OwlTcpConn* conn, void* appctx,
                             uint64_t echeance_ms);

/* Reveille la boucle reacteur (self-pipe W4). THREAD-SAFE, non bloquant,
 * coalesce : c'est ce que le producteur d'un flux signale quand un chunk
 * devient dispo. Sans effet si le pool n'a pas de reveil (mode non
 * reacteur). */
void owl_tcp_pool_reac_signale(OwlTcpPool* p);

/* Ecriture NON BLOQUANTE : envoie autant que le tampon socket accepte,
 * maintenant. Rend le nombre d'octets ecrits (0 = would-block, rien ecrit),
 * ou -1 sur erreur fatale (pair mort). Socket non bloquante requise (l'accept
 * la pose). Reserve au drain reacteur en clair — TLS non gere par ce chemin
 * (le front reacteur ne pompe pas encore le handshake TLS). */
int owl_tcp_conn_ecrit_dispo(OwlTcpConn* c, const void* buf, size_t len);

/* Sonde non bloquante : le pair est-il DEFINITIVEMENT perdu (RST/erreur de
 * connexion) ? A n'appeler que sur signal POLLHUP/POLLERR du reacteur. Un EOF
 * propre (recv==0) est un demi-close (le pair lit encore) et rend false : le
 * couper serait une regression. Seule une erreur de connexion (ECONNRESET)
 * rend true. Clair uniquement (reacteur = clair). Cf. le corps pour la limite
 * physique (un FIN propre sans octet en vol reste indetectable). */
bool owl_tcp_conn_pair_perdu(OwlTcpConn* c);

/* Demande l'arret, joint tous les workers, libere le pool. */
void owl_tcp_pool_arrete(OwlTcpPool* p);

size_t owl_tcp_pool_connexions_servies(const OwlTcpPool* p);
/* Maximum de connexions REELLEMENT en service en meme temps. C'est la
 * mesure du parallelisme atteint, et la preuve que le plafond tient. */
size_t owl_tcp_pool_pic_simultanees(const OwlTcpPool* p);
/* Maximum de flux/jetons TENUS PAR LE REACTEUR en meme temps (W7). Une fois
 * le worker libere apres le handoff, c'est ici que se lit la concurrence
 * reelle du data-plane async (l'ex-`pic` des workers ne mesure plus que la
 * fenetre de SETUP). */
size_t owl_tcp_pool_pic_reac_tenue(const OwlTcpPool* p);
size_t owl_tcp_pool_nb_workers(const OwlTcpPool* p);
/* Connexions dont le handshake TLS n'a pas abouti (echeance depassee,
 * pair en clair, protocole invalide). Elles comptent aussi dans
 * `connexions_servies` : elles ont pris puis RENDU un emplacement, et
 * c'est cette restitution qu'on veut pouvoir observer. */
size_t owl_tcp_pool_handshakes_echoues(const OwlTcpPool* p);

/* ---------- Arret propre ---------- */

/* Installe SIGTERM/SIGINT (POSIX) ou le console control handler
 * (Windows). Idempotent. N'est jamais appele implicitement : un
 * interpreteur ne detourne pas les signaux du processus sans qu'on le
 * lui demande. */
void owl_tcp_server_installe_arret(void);
bool owl_tcp_server_arret_demande(void);
void owl_tcp_server_demande_arret(void);

/* Horloge monotone en millisecondes, pour les echeances. */
uint64_t owl_tcp_server_horloge_ms(void);

#endif /* owl_net_tcp_server_h */
