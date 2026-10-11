// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_http_server_h
#define owl_http_server_h

/* ================================================================
 * TRANSPORT HTTP/1.1 SERVEUR — phase A0.1
 *
 * Spec : conception du serveur HTTP interne et son contrat de transport.
 *
 * Ce qui vit ici : accept d'UNE connexion a la fois, parsing de
 * requete via llhttp en mode HTTP_REQUEST (aucun flag LENIENT_*),
 * limites appliquees PENDANT l'accumulation, timeouts, strictesse
 * protocolaire, reponse minimale avec Content-Length, arret propre.
 *
 * Depuis A1.1a : keep-alive, pipelining, remise a zero complete de
 * l'etat du parseur entre deux requetes d'une meme connexion, plafond
 * de requetes par connexion.
 *
 * Depuis A1.1b : le service d'une connexion est reentrant (aucun etat
 * global mutable) et se branche sur le pool de workers de
 * net_tcp_server.h. N connexions en parallele, VM toujours possedee par
 * un seul thread — le transport ne la connait toujours pas.
 *
 * Ce qui n'y vit PAS : TLS, fichiers statiques,
 * routage, executeur, file. Aucune native OWL, aucune surface de langage :
 * ce module ne connait pas la VM et n'alloue aucun objet OWL.
 * ================================================================ */

#include "common.h"
#include "net_tcp_server.h"

#include <stddef.h>

#define OWL_HTTP_ENTETES_MAX 100

/* ---------- Limites (contrat §6 : appliquees par le transport) ---------- */

typedef struct {
    size_t ligne_requete_max;     /* 8 Ko   -> 414 */
    size_t entetes_nb_max;        /* 100    -> 431 */
    size_t entetes_octets_max;    /* 16 Ko  -> 431 */
    size_t corps_max;             /* config -> 413 */
    size_t chunk_extension_max;   /* 4 Ko   -> 400 */
    /* Plafond de requetes servies sur UNE connexion keep-alive
     * (corpus 3.6). Une connexion sans plafond est un vecteur de deni de
     * service : le client garde un descripteur et un contexte de parseur
     * aussi longtemps qu'il veut. 0 = pas de keep-alive du tout. */
    size_t requetes_max_par_connexion;

    int timeout_entetes_ms;       /* lecture des en-tetes */
    int timeout_corps_ms;         /* lecture du corps */
    int timeout_ecriture_ms;      /* ecriture de la reponse */
    int timeout_inactivite_ms;    /* connexion ouverte, aucun octet */
} OwlHttpLimites;

/* ---------- Requete livree ---------- */

typedef struct {
    char* nom;
    char* valeur;
} OwlHttpEntete;

typedef struct {
    char  methode[32];

    /* Contrat §3 : octets bruts, NON decodes, NON normalises. La
     * frontiere de securite des chemins est de la politique et vit
     * au-dessus (A0.3). */
    char* cible;
    /* Forme absolue acceptee (RFC 9112 §3.2.2) : seul le chemin
     * routera. Pointe dans `cible` quand la forme est d'origine. */
    const char* chemin;

    int   version_majeure;
    int   version_mineure;

    OwlHttpEntete entetes[OWL_HTTP_ENTETES_MAX];
    size_t        nb_entetes;

    char*  corps;
    size_t corps_len;

    bool  est_head;
    char  pair[80];
} OwlHttpRequete;

/* ---------- Reponse rendue ---------- */

typedef struct {
    int    statut;
    char   type_contenu[128];
    /* Possede par l'appelant du handler : le handler ecrit corps_fixe
     * (borne) ou pose corps/corps_len sur une zone qui lui survit. */
    const char* corps;
    size_t      corps_len;
} OwlHttpReponse;

/* Handler C. Pas de VM ici : l'executeur de la phase A0.2 se
 * branchera derriere ce point d'entree, pas a la place. */
typedef void (*OwlHttpHandler)(const OwlHttpRequete* req,
                               OwlHttpReponse* rep,
                               void* user);

/* Le type OPAQUE de reponse (§5). Declare ici, defini par http_exec.c :
 * le transport doit pouvoir le nommer sans rien supposer de sa forme —
 * ni tampon contigu, ni fichier. */
typedef struct OwlHttpRep OwlHttpRep;

/* Handler « opaque » (phase A0.3) : rend la PROPRIETE d'une reponse, ce
 * qui lui permet de choisir le mode A (tampon) ou le mode B (fichier).
 * C'est par ce point d'entree que le mode fichier se declenche, et c'est
 * le service statique qui en est le consommateur reel.
 * Prioritaire sur `handler` quand les deux sont poses. */
typedef OwlHttpRep* (*OwlHttpHandlerRep)(const OwlHttpRequete* req, void* user);

/* Executeur HTTP (defini par http_exec.c). Declare opaque ici pour que le
 * transport puisse le nommer sans dependance circulaire (http_exec.h inclut
 * http_server.h). Guard partage avec http_exec.h : le typedef n'apparait
 * qu'une fois quels que soient l'ordre et la combinaison d'includes. */
#ifndef OWL_HTTP_EXECUTEUR_DEFINI
#define OWL_HTTP_EXECUTEUR_DEFINI
typedef struct OwlHttpExecuteur OwlHttpExecuteur;
#endif

typedef struct {
    OwlHttpLimites limites;

    /* Allow-list de Host. NULL = desactivee (defaut : le proxy s'en
     * charge d'ordinaire, cf. corpus 2.18). */
    const char* const* hotes_autorises;
    size_t             nb_hotes_autorises;

    OwlHttpHandler handler;
    void*          handler_user;

    OwlHttpHandlerRep handler_rep;
    void*             handler_rep_user;

    /* Executeur async (W7) : quand pose (non-NULL) ET que la connexion est
     * servie par le front reacteur, le data-plane executeur-based est DEPOSE
     * puis remis au reacteur (le worker se libere aussitot, plus d'attente
     * bloquante sur l'executeur mono-thread — c'est le residu que W7 supprime).
     * Les routes de controle restent servies inline via handler_rep ; les
     * handlers synchrones (statique/mock) aussi. Ignore hors mode reacteur
     * (le chemin bloquant retombe alors sur handler_rep). */
    OwlHttpExecuteur* executeur_async;
} OwlHttpServeurConfig;

void owl_http_limites_defaut(OwlHttpLimites* lim);
void owl_http_serveur_config_defaut(OwlHttpServeurConfig* cfg);

/* Classification plan de controle / plan de donnees (, W2).
 * true = route de CONTROLE (servie INLINE, ne touche NI VM NI modele —
 * design §8 Q1) : `/v1/models`, `/health`, `/sante` (bilingue FR/EN,
 * invariant 3). Le predicat est partage entre le front reacteur (qui
 * classe apres lecture des en-tetes) et le bypass de la FIFO executeur
 * (http_exec.c) — un seul endroit definit ce qu'est une route de controle,
 * pour que les deux ne divergent jamais. Compare le chemin brut jusqu'a la
 * chaine de requete (`?`). */
bool owl_http_route_est_controle(const char* chemin);

/* Sert une connexion DEJA ACCEPTEE, de bout en bout, et la ferme. En
 * prend la propriete (le descripteur de l'appelant est invalide au
 * retour). Tout l'etat de service vit sur la pile de l'appelant : c'est
 * ce qui rend le pool de workers sans verrou, et c'est le point d'entree
 * que le pool branche.
 *
 * Retour : identique a owl_http_serveur_sert_une_connexion. */
int owl_http_serveur_sert_connexion_acceptee(OwlTcpConn* conn,
                                             const OwlHttpServeurConfig* cfg);

/* Sert exactement UNE connexion, de son accept a sa fermeture, et
 * autant de requetes que le keep-alive en autorise : lecture+parsing,
 * handler ou rejet, reponse, remise a zero, recommence. Une connexion a
 * la fois reste volontaire (le pool de workers est un lot distinct).
 *
 * Le pipelining est transparent pour l'appelant : les octets qui suivent
 * une requete complete dans le meme tampon de lecture sont la requete
 * suivante, jamais une requete recomposee.
 *
 * Retour : le code de statut de la DERNIERE reponse emise, 0 si aucune
 * connexion n'est arrivee dans accept_timeout_ms ou si la connexion est
 * morte sans qu'aucune reponse ne soit due (timeout de lecture, EOF
 * muet), -1 sur erreur d'accept. */
int owl_http_serveur_sert_une_connexion(OwlTcpListener* l,
                                        const OwlHttpServeurConfig* cfg,
                                        int accept_timeout_ms);

/* Boucle jusqu'a owl_tcp_server_arret_demande(). */
void owl_http_serveur_boucle(OwlTcpListener* l, const OwlHttpServeurConfig* cfg);

/* Pool de workers reseau (A1.1b) : N connexions menees en parallele
 * derriere la MEME file bornee et le MEME executeur. `l` et `cfg` doivent
 * survivre au pool. Arret via owl_tcp_pool_arrete. */
OwlTcpPool* owl_http_serveur_pool_demarre(OwlTcpListener* l,
                                          const OwlHttpServeurConfig* cfg,
                                          const OwlTcpPoolConfig* pcfg);

#endif /* owl_http_server_h */
