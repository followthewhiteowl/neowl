// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* jwks_cache.h — cache global de cles JWKS pour l'auth JWT declarative du DSL
 * de service.
 *
 * Cache process-global, cle = URL JWKS (dedup inter-routes/services). SANS
 * VERROU : touche uniquement par le thread VM (l'auth ne tourne jamais en
 * concurrence, ), ce qui rend le single-flight automatique. Chaque
 * entree tient les cles RSA DEJA PARSEES (hot path = 0 reseau + 0 parse).
 *
 * Politique fail-closed : toute anomalie -> code != JWKS_OK -> l'appelant
 * repond 401. Jamais fail-open.
 */
#ifndef OWL_JWKS_CACHE_H
#define OWL_JWKS_CACHE_H

#include "mbedtls/pk.h"

typedef enum {
    JWKS_OK = 0,        /* cle trouvee et prete : *out_pk valide (possede par le cache) */
    JWKS_KID_INCONNU,   /* fetch(s) reussi(s) mais aucune cle RSA pour ce kid -> 401 */
    JWKS_INJOIGNABLE    /* aucune cle disponible (ni cache exploitable ni fetch) -> 401 */
} JwksResolution;

/* Resout la cle publique RSA pour (url, kid). Cache-first : un hit frais ne
 * touche pas le reseau. Cold / kid-miss / cache perime -> 1 GET HTTPS borne
 * (rate-limite ~1/60s/URL). Sur echec de fetch, sert le cache existant jusqu'a
 * un hard-max de peremption puis bascule fail-closed. `*out_pk` pointe une cle
 * POSSEDEE par le cache : ne pas la liberer, ne l'utiliser que pour verifier
 * immediatement (thread VM, aucune reentrance). `kid` peut etre NULL (mode
 * JWKS exige un kid -> alors JWKS_KID_INCONNU). */
JwksResolution owl_jwks_resolve(const char* url, const char* kid,
                                mbedtls_pk_context** out_pk);

/* Vide le cache et libere toutes les cles parsees. Appele au reset de
 * compilation (owl_service_runtime_reset) : etancheite entre services /
 * self-tests, et coherence quand les routes changent. */
void owl_jwks_cache_reset(void);

#endif /* OWL_JWKS_CACHE_H */
