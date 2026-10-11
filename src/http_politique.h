// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_http_politique_h
#define owl_http_politique_h

/* ================================================================
 * POLITIQUE HTTP — phase A0.3
 *
 * Spec : contrat de transport HTTP interne (repartition des
 *        responsabilites, mode B, familles 4 et 5 du corpus d'attaque).
 *
 * L'UN SEUL ENDROIT IDENTIFIE. Le §6 l'exige noir sur blanc : la
 * politique doit etre « identique quel que soit le transport ». Tout ce
 * qui depend de la CONFIGURATION vit donc ici et nulle part ailleurs —
 * c'est le critere qui departage transport et politique (§6, precise le
 * 2026-07-31) :
 *
 *   - frontiere de securite des chemins (racine du statique configuree) ;
 *   - allow-list de Host (liste configuree, desactivee par defaut) ;
 *   - service de fichiers statiques : type de contenu, ETag,
 *     If-Modified-Since, 304 ;
 *   - point de passage unique pour l'ecriture des en-tetes de reponse
 *     (interdiction de CR/LF, cf. famille 5).
 *
 * Ce module ne connait PAS la VM : aucune allocation d'objet OWL, aucun
 * GC declenchable, aucune native, aucun symbole visible depuis un .owl.
 * ================================================================ */

#include "common.h"
#include "http_server.h"
#include "http_exec.h"

#include <stddef.h>

/* ================================================================
 * 1. Frontiere de securite des chemins
 *
 * Six etapes, dans l'ordre du contrat §6. L'ordre n'est pas decoratif :
 * verifier le confinement AVANT la resolution canonique laisse passer
 * une jonction ou un lien symbolique (mesure consignee dans le .c).
 * ================================================================ */

#define OWL_POL_OK 0   /* sinon : code HTTP a emettre (400 / 404) */

/* Longueur maximale d'une cible acceptee par la politique. Le transport
 * a deja son propre plafond de ligne de requete ; celui-ci est ce qui
 * rend la politique INDEPENDANTE du transport (contrat §9bis). */
#define OWL_POL_CIBLE_MAX 4096

typedef struct {
    /* Racine canonique, resolue une fois par owl_http_politique_racine.
     * Forme native de la plateforme (prefixe \\?\ conserve sous Windows :
     * c'est la forme que rend la resolution canonique, donc la seule
     * comparable octet a octet). */
    char racine[1024];

    /* Faux par defaut : /.env et /.git/config repondent 404 (vecteur
     * 4.10). Reglage, donc politique. */
    bool servir_caches;
} OwlHttpStatique;

/* Resout et memorise la racine. Faux si le dossier n'existe pas. */
bool owl_http_politique_racine(OwlHttpStatique* s, const char* dossier);

/* Applique les six etapes a une cible BRUTE (telle que livree par le
 * transport : percent-encodee, non normalisee).
 *
 * OWL_POL_OK -> `out` recoit un chemin de fichier utilisable, garanti
 * confine sous la racine. Sinon `out` est vide et le retour est le code
 * HTTP a emettre. */
int owl_http_politique_chemin(const OwlHttpStatique* s, const char* cible,
                              char* out, size_t out_taille);

/* ================================================================
 * 2. Ecriture des en-tetes : le point de passage unique (§6, famille 5)
 * ================================================================ */

/* Nom de champ : token RFC 9110 §5.6.2. Ni « : », ni espace, ni vide. */
bool owl_http_politique_nom_entete_valide(const char* nom);

/* Valeur de champ : ni CR, ni LF, ni NUL, ni octet de controle. C'est
 * la seule parade a la scission de reponse. */
bool owl_http_politique_valeur_entete_valide(const char* valeur);

/* 100-599. Hors de cette plage, la reponse n'est pas construite. */
bool owl_http_politique_statut_valide(int statut);

/* ================================================================
 * 3. Allow-list de Host (vecteur 2.18, reporte d'A0.1)
 *
 * Depend d'une liste configuree -> politique. Liste NULL ou vide =
 * desactivee, ce qui est le defaut : d'ordinaire un proxy s'en charge.
 * ================================================================ */

bool owl_http_politique_hote_permis(const char* const* liste, size_t nb,
                                    const char* hote);

/* ================================================================
 * 4. Service de fichiers statiques
 *
 * Consommateur reel du mode B (§5) : la reponse rendue en cas de succes
 * est en MODE FICHIER — le transport la streame sans jamais rentrer
 * dans la VM ni allouer un octet GC.
 *
 * Pas de Range : decision dev actee (§10bis b).
 * ================================================================ */

/* Rend la PROPRIETE d'une reponse (a liberer via owl_http_rep_libere).
 * 200 + mode fichier, 304 sans corps, ou 400/404 sans corps. NULL sur
 * echec d'allocation seulement. */
OwlHttpRep* owl_http_statique_sert(const OwlHttpStatique* s,
                                   const OwlHttpRequete* req);

/* Handler pret a brancher sur OwlHttpServeurConfig.handler_rep ;
 * `user` = l'OwlHttpStatique*. */
OwlHttpRep* owl_http_statique_handler(const OwlHttpRequete* req, void* user);

/* Type de contenu deduit de l'extension. Jamais NULL. */
const char* owl_http_politique_type_mime(const char* chemin);

#endif /* owl_http_politique_h */
