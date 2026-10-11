// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_tls_server_h
#define owl_net_tls_server_h

/* ================================================================
 * TLS SERVEUR (mbedTLS) — phase A1.2
 *
 * Pourquoi un fichier separe de net_tcp_tls.c : celui-ci est le chemin
 * CLIENT, et il est BLOCANT (« Handshake loop (blocking : pas de
 * WANT_READ/WRITE attendu) »). Un serveur ne peut pas se le permettre :
 * une poignee de main qui n'aboutit pas est un vecteur d'epuisement,
 * exactement comme un slowloris. Ici le handshake est non bloquant,
 * entrelace avec poll(), sur echeance ABSOLUE — donc insensible a un
 * pair qui distille.
 *
 * PROPRIETE PAR WORKER, ET RIEN DE PARTAGE
 * ----------------------------------------
 * MBEDTLS_THREADING_C n'est PAS active, et n'a pas a l'etre : ce module
 * ne partage AUCUN objet mbedTLS mutable entre threads. Mesure faite
 * dans le vendor (3.6.2) avant d'ecrire la premiere ligne — pendant un
 * handshake serveur, ce qui est mute n'est pas seulement le DRBG :
 *
 *   - ctr_drbg : compteur + reseed        (ctr_drbg.h:220-229)
 *   - entropy  : etat de collecte         (entropy.h:112-113)
 *   - rsa      : cache d'aveuglement Vi/Vf reecrit A CHAQUE signature
 *                                          (rsa.c:1296-1337)
 *   - ecp_group: table de precalcul grp->T posee paresseusement
 *                                          (ecp.c:527-528)
 *
 * Autrement dit, partager une CLE PRIVEE entre workers est une course
 * quel que soit l'algorithme — choisir EC pour esquiver le mutex RSA
 * n'y change rien. D'ou : chaque worker POSSEDE ses entropy + ctr_drbg
 * + pk + x509_crt + ssl_config, construits AU DEMARRAGE DU POOL depuis
 * les memes tampons PEM immuables. N workers = N parsings au demarrage,
 * jamais dans le chemin de la connexion.
 *
 * Consequence de build : aucun define global, CMakeLists intouche cote
 * mbedTLS, MBEDTLS_USER_CONFIG_FILE reste PRIVATE, zero changement
 * d'ABI. (MBEDTLS_THREADING_C exige de surcroit THREADING_ALT ou
 * THREADING_PTHREAD — mbedtls_config.h:3625-3626 — alors que le pool
 * utilise CreateThread sur Windows.)
 *
 * Et le chemin serveur n'appelle deliberement PAS owl_tls_get_drbg() :
 * ce DRBG global de net_tcp_tls.c est precisement le seul objet qui
 * redeviendrait partage.
 * ================================================================ */

#include "common.h"
#include "net_tcp.h"   /* owl_socket_fd_t */

#include <stddef.h>

/* Tampons PEM immuables (certificat + cle privee), partages en LECTURE
 * SEULE par tous les workers. Rien de mbedTLS ne vit ici : c'est le
 * point qui rend la propriete par worker possible. */
typedef struct OwlTlsServeurMateriel OwlTlsServeurMateriel;

/* Contexte mbedTLS POSSEDE par un worker. Jamais touche par deux
 * threads. */
typedef struct OwlTlsServeurCtx OwlTlsServeurCtx;

/* Session TLS d'UNE connexion. Vit sous le service de cette connexion,
 * donc sur le seul worker qui la mene. */
typedef struct OwlTlsSession OwlTlsSession;

/* Valide le PEM tout de suite (un materiel qui ne parse pas n'est pas
 * un materiel) et copie les tampons. NULL + `err` renseigne sur echec. */
OwlTlsServeurMateriel* owl_tls_serveur_materiel_cree(const char* cert_pem,
                                                     const char* cle_pem,
                                                     char* err, size_t err_len);
void owl_tls_serveur_materiel_libere(OwlTlsServeurMateriel* m);

OwlTlsServeurCtx* owl_tls_serveur_ctx_cree(const OwlTlsServeurMateriel* m);
void              owl_tls_serveur_ctx_libere(OwlTlsServeurCtx* c);

/* Handshake serveur non bloquant sur `fd`, echeance ABSOLUE.
 * OWL_TCPS_OK (session posee dans *out) / OWL_TCPS_TIMEOUT /
 * OWL_TCPS_ERREUR (*out = NULL, rien a liberer pour l'appelant). */
int owl_tls_serveur_handshake(OwlTlsServeurCtx* ctx, owl_socket_fd_t fd,
                              int timeout_ms, OwlTlsSession** out);

/* Memes conventions de retour que owl_tcp_conn_lit / _ecrit. */
int  owl_tls_session_lit(OwlTlsSession* s, void* buf, size_t len, int timeout_ms);
int  owl_tls_session_ecrit(OwlTlsSession* s, const void* buf, size_t len, int timeout_ms);
/* close_notify (au plus deux essais) puis liberation. Le descripteur
 * reste a l'appelant : c'est lui qui l'a ouvert. */
void owl_tls_session_ferme(OwlTlsSession* s);

/* ================================================================
 * API EVENT-DRIVEN (chantier W3) — inversion de controle
 *
 * Les trois fonctions blocantes ci-dessus (handshake / lit / ecrit)
 * pollent en INTERNE : elles immobilisent le thread appelant jusqu'a
 * l'echeance. C'est le bon modele pour un worker-par-connexion (le
 * pool A1.1b), mais pas pour une boucle reacteur unique qui multiplexe
 * N connexions sur un seul thread : la, un pas qui bloque gele TOUTES
 * les autres connexions.
 *
 * Les fonctions _pas ci-dessous font UN pas mbedTLS et RENDENT a
 * l'appelant le sens a re-attendre (WANT_LECTURE / WANT_ECRITURE) au
 * lieu de poller. La boucle reacteur mappe ce retour sur
 * owl_reacteur_modifie_interet (LECTURE <-> ECRITURE) et re-pompe le
 * meme pas a l'evenement suivant. Les trois fonctions blocantes sont
 * desormais des BOUCLES par-dessus ces _pas + tlss_attend : les deux
 * chemins partagent EXACTEMENT la meme mecanique mbedTLS, et le chemin
 * blocant (rejoue par les six familles d'http_tls_test.c) valide de
 * fait les fonctions _pas.
 *
 * Les BIO non blocants ne changent pas : c'est toujours eux qui mappent
 * EAGAIN -> MBEDTLS_ERR_SSL_WANT_READ/WRITE. W3 ne fait que remonter ce
 * WANT jusqu'a l'appelant au lieu de le consommer dans une boucle poll.
 * ================================================================ */

typedef enum {
    OWL_TLS_PAS_OK = 0,          /* pas termine : handshake fini, ou octets transferes */
    OWL_TLS_PAS_WANT_LECTURE,    /* re-pomper quand le fd est LISIBLE (OWL_REAC_LECTURE) */
    OWL_TLS_PAS_WANT_ECRITURE,   /* re-pomper quand le fd est ECRIVABLE (OWL_REAC_ECRITURE) */
    OWL_TLS_PAS_FERME,           /* pair ferme proprement (close_notify / EOF) — lecture */
    OWL_TLS_PAS_ERREUR           /* echec irrecuperable */
} OwlTlsPas;

/* Cree la session (mbedtls_ssl_setup + BIO non blocants) SANS lancer le
 * handshake : c'est la boucle appelante qui le pompera pas a pas. NULL
 * si echec. Le descripteur reste a l'appelant ; il est passe en non
 * blocant ici. Liberer avec owl_tls_session_ferme. */
OwlTlsSession* owl_tls_session_cree(OwlTlsServeurCtx* ctx, owl_socket_fd_t fd);

/* Descripteur porte par la session — ce que la boucle reacteur
 * enregistre et dont elle bascule l'interet. */
owl_socket_fd_t owl_tls_session_fd(const OwlTlsSession* s);

/* Un pas de handshake. OWL_TLS_PAS_OK = handshake termine ; sinon
 * WANT_* (re-pomper) ou ERREUR (alerte, pair en clair, protocole
 * invalide). Incremente le compteur de tours (un par appel mbedTLS). */
OwlTlsPas owl_tls_serveur_handshake_pas(OwlTlsSession* s);

/* Un pas de lecture. OWL_TLS_PAS_OK -> *out_lu octets (> 0) deposes
 * dans buf ; FERME -> le pair a coupe proprement ; sinon WANT_* / ERREUR. */
OwlTlsPas owl_tls_session_lit_pas(OwlTlsSession* s, void* buf, size_t len,
                                  size_t* out_lu);

/* Un pas d'ecriture. OWL_TLS_PAS_OK -> *out_ecrit octets (> 0) consommes
 * de buf ; l'appelant boucle si *out_ecrit < len (write TLS partiel).
 * Sinon WANT_* / ERREUR. */
OwlTlsPas owl_tls_session_ecrit_pas(OwlTlsSession* s, const void* buf, size_t len,
                                    size_t* out_ecrit);

/* Nombre cumule de TOURS des boucles TLS serveur : une unite par appel a
 * mbedtls_ssl_handshake / _read / _write. Instrument de mesure — c'est
 * ce qui rend l'absence de boucle occupee OBSERVABLE au lieu
 * d'affirmee.
 *
 * Compter les reveils poll() ne suffirait PAS : une implementation qui
 * boucle sur WANT_READ sans jamais attendre n'appelle pas poll, donc
 * afficherait zero reveil tout en brulant un coeur. Le tour de boucle,
 * lui, est incontournable — aucune boucle occupee ne peut exister sans
 * l'incrementer.
 *
 * Volontairement non atomique : la mesure vise une connexion qui stagne,
 * pas une charge concurrente. */
size_t owl_tls_serveur_tours_boucle(void);
void   owl_tls_serveur_tours_boucle_raz(void);

#endif /* owl_net_tls_server_h */
