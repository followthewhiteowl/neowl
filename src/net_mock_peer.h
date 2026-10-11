// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_mock_peer_h
#define owl_net_mock_peer_h

#include <stddef.h>

/* Pair TCP de test : emet des octets bruts arbitraires en controlant
 * explicitement le decoupage des write().
 *
 * Volontairement AGNOSTIQUE du protocole applicatif : le contrat est
 * "quels octets, decoupes comment", rien de plus. Aucune connaissance
 * de HTTP ici (les sentinelles de lecture sont fournies par l'appelant).
 * Deux consommateurs prevus :
 *   - role ECOUTE   : sert de serveur face a notre client HTTP
 *                     ( — caracterisation du parsing de reponse) ;
 *   - role CONNECTE : sert de client face a notre futur serveur HTTP
 *                     (phase A0 + corpus d'attaque, cf. contrat de
 *                      transport HTTP interne).
 *
 * Le pair tourne dans un thread (CreateThread / pthread), meme pattern
 * que src/sql_postgres_mock_server.c. Sockets OS directs, pas notre
 * couche net_tcp : le mock doit rester independant du code sous test. */

typedef enum {
    OWL_MOCK_ROLE_ECOUTE = 0,   /* bind/listen/accept : on est le serveur */
    OWL_MOCK_ROLE_CONNECTE      /* connect : on est le client */
} OwlMockPeerRole;

typedef enum {
    OWL_MOCK_DECOUPE_BLOC = 0,  /* tout le payload en un seul write() */
    OWL_MOCK_DECOUPE_OCTET,     /* un write() par octet */
    OWL_MOCK_DECOUPE_OFFSETS    /* write() aux frontieres fournies */
} OwlMockPeerDecoupe;

#define OWL_MOCK_PEER_MAX_OFFSETS 32
#define OWL_MOCK_PEER_MAX_RECU    8192

typedef struct {
    OwlMockPeerRole role;

    /* Role CONNECTE : cible a joindre. NULL/0 -> 127.0.0.1 refuse. */
    const char* hote;
    int         port;

    /* Payload a emettre. Copie a l'interieur du pair : l'appelant peut
     * liberer son buffer des le retour de owl_mock_peer_demarre. */
    const char* octets;
    size_t      octets_len;

    OwlMockPeerDecoupe decoupe;
    /* Mode OFFSETS : frontieres strictement croissantes dans
     * ]0, octets_len[. Le dernier fragment va jusqu'a la fin. */
    const size_t* offsets;
    size_t        offsets_len;

    /* Pause entre deux write() consecutifs. Indispensable en mode OCTET :
     * sans elle, la pile TCP recolle les segments et le pair sous test ne
     * voit jamais la fragmentation qu'on cherche a lui infliger. */
    int delai_ms;

    /* Lecture prealable (0 = ne rien lire avant d'emettre).
     * Les deux criteres sont OU-es : on s'arrete au premier satisfait,
     * ou sur EOF / erreur socket.
     *   sentinelle : suite d'octets a rencontrer (ex "\r\n\r\n" pour la
     *                fin des entetes d'une requete HTTP) ;
     *   min        : nombre minimal d'octets a avoir lus. */
    const char* sentinelle_lecture;
    size_t      octets_min_lecture;

    /* 1 = apres l'emission, lit ce que le pair repond et l'accumule
     * dans `recu`, jusqu'a EOF ou timeout_lecture_ms.
     * Indispensable au role CONNECTE face a notre serveur : sans ca la
     * reponse part dans le drain de fermeture et n'est jamais observee. */
    int lire_apres_ecriture;

    /* 1 = shutdown(envoi) juste apres l'emission, AVANT de lire la
     * reponse. Sans ca, une requete tronquee interbloque : le pair sous
     * test attend la suite des octets pendant qu'on attend sa reponse.
     * Le demi-fermeture lui signale « je n'enverrai plus rien ». */
    int fermer_envoi_avant_lecture;

    /* Timeout de reception (SO_RCVTIMEO) applique aux lectures.
     * 0 = bloquant jusqu'a EOF. Borne la duree d'un vecteur dont le
     * pair ne repond jamais. */
    int timeout_lecture_ms;

    /* 1 = shutdown(envoi) + drain + close des le dernier write.
     * Necessaire pour tester un corps delimite par fermeture. */
    int fermer_apres_ecriture;

    /* fermer_apres_ecriture == 0 : duree pendant laquelle on garde la
     * connexion ouverte (laisse le pair conclure lui-meme). */
    int garder_ouvert_ms;
} OwlMockPeerConfig;

typedef struct OwlMockPeer OwlMockPeer;

/* Config neutre : role ECOUTE, port ephemere, decoupe BLOC, pas de
 * lecture prealable, fermeture apres ecriture. */
void owl_mock_peer_config_defaut(OwlMockPeerConfig* cfg);

/* Demarre le thread du pair. Pour le role ECOUTE, l'appel ne rend la main
 * qu'apres bind+listen (ou timeout 2s) : le port est donc lisible tout de
 * suite. Retourne NULL si l'allocation ou le thread echoue. */
OwlMockPeer* owl_mock_peer_demarre(const OwlMockPeerConfig* cfg);

/* Port effectivement ecoute (role ECOUTE, port ephemere). */
int owl_mock_peer_port(const OwlMockPeer* p);
/* 1 si le listen (role ECOUTE) est en place. */
int owl_mock_peer_pret(const OwlMockPeer* p);

void owl_mock_peer_joint(OwlMockPeer* p);
void owl_mock_peer_libere(OwlMockPeer* p);

/* Octets recus du pair (tronques a OWL_MOCK_PEER_MAX_RECU). Toujours
 * termine par un NUL, pour permettre strstr sur du texte. */
const char* owl_mock_peer_recu(const OwlMockPeer* p, size_t* out_len);

/* Nombre de write() reellement emis. Permet d'asserter que le decoupage
 * demande a bien eu lieu (ex : == octets_len en mode OCTET). */
size_t owl_mock_peer_nb_ecritures(const OwlMockPeer* p);

/* Nombre d'octets effectivement emis (utile si le pair a coupe tot). */
size_t owl_mock_peer_nb_octets_emis(const OwlMockPeer* p);

#endif /* owl_net_mock_peer_h */
