// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_HTTP_ROUTE_H
#define OWL_HTTP_ROUTE_H

/* Routeur HTTP declaratif : analyse statique des routes d'un ServiceREST.
 *
 * Deux controles a la compilation, une fois le bloc service collecte
 * (service_decl.h) :
 *   1. Liaison des parametres de chemin : chaque segment-parametre de Chemin
 *      doit avoir un parametre correspondant dans la signature de surRequete.
 *   2. Detection des conflits de routes : comparaison exhaustive par paires
 *      (N^2/2, exacte). Deux routes de meme methode qui se recouvrent sans
 *      qu'aucune ne soit strictement plus specifique => conflit (erreur), y
 *      compris le doublon exact. La methode fait partie de la cle (GET et POST
 *      sur le meme chemin ne conflictent pas). Portee par service : UrlBase
 *      prefixe, deux services distincts n'entrent jamais en conflit.
 *
 * Specificite : un segment litteral est plus specifique qu'un segment
 * parametre a la meme position. Pas de wildcard a ce stade (reporte). */

#include "service_decl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Analyse les routes d'un service et emet les erreurs de compilation
 * (conflits + parametres non lies) via le registre d'erreurs. */
void owl_http_route_analyze_service(const OwlServiceDecl* svc);

#ifdef __cplusplus
}
#endif

#endif /* OWL_HTTP_ROUTE_H */
