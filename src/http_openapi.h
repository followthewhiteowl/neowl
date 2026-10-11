// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_HTTP_OPENAPI_H
#define OWL_HTTP_OPENAPI_H

/* http_openapi — generateur de document OpenAPI 3 derive de l'AST d'un
 * ServiceREST/OperationHTTP. Source de verite unique : la
 * declaration collectee a la compilation (service_decl.h). Aucune seconde
 * declaration a maintenir — modifier le DSL et regenerer suffit.
 *
 * Module neutre : lecture seule de l'OwlServiceDecl + resolution des codes
 * HTTP via le registre de constantes. Aucune implication VM/GC ni Qt (fait
 * partie d'OWL_CORE_SRCS). Consomme par la commande CLI `owl openapi` et par
 * la route `GET /openapi.json` de `owl serve` (meme generateur, cf. ticket). */

#include "service_decl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Genere le document OpenAPI 3 (JSON, indente 2 espaces, sans espace apres le
 * deux-points) derive de `svc`. Retourne une chaine malloc'ee terminee par NUL
 * (a liberer par l'appelant), ou NULL si svc est NULL ou en cas d'echec
 * d'allocation. */
char* owl_openapi_generate(const OwlServiceDecl* svc);

#ifdef __cplusplus
}
#endif

#endif /* OWL_HTTP_OPENAPI_H */
