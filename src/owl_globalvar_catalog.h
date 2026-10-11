// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_globalvar_catalog.h — catalogue des variables globales prédéfinies
 * du langage.
 *
 * Une variable globale prédéfinie est posée par le runtime dans vm.globals
 * sous plusieurs noms (canon FR accentué, alias ASCII, alias EN canon)
 * et reflète un état que l'utilisateur peut consulter — typiquement en
 * lecture seule. Exemple : ErreurDétectée (booléen mis à Vrai après une
 * native qui a signalé une soft-error).
 *
 * Source de vérité unique consommée par :
 *  - le runtime (stdlib_erreur::sync_err_detected_global) pour itérer
 *    les 3 noms à poser dans vm.globals ;
 *  - le DAP (debug_api) pour reconnaître ces globals système sans
 *    dupliquer la liste ;
 *  - le CLI doc (owl explain, owl list globalvars) ;
 *  - le self-test registry (rule 36) pour valider la cohérence.
 *
 * Architecture parallèle au catalogue keyword/operator : tables statiques,
 * register au boot via stdlib_*_init, lookup loose (casse+accent insensible).
 */

#ifndef OWL_GLOBALVAR_CATALOG_H
#define OWL_GLOBALVAR_CATALOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* canonical;     /* "ErreurDétectée" — canon FR accentué */
    const char* nameAscii;     /* "ErreurDetectee" — alias sans accent */
    const char* nameEN;        /* "ErrorOccurred"  — canon EN doc */
    const char* type;          /* "booléen", "entier", "chaîne", … */
    const char* descFR;        /* obligatoire, non vide */
    const char* descEN;        /* obligatoire, non vide */
    bool        readOnly;      /* true = lecture seule côté utilisateur */
} OwlGlobalVarDef;

/* Enregistrement (appelé au boot par stdlib_*_init). La table doit être
 * statique — pointeurs conservés tels quels. */
void owl_globalvar_catalog_register(const OwlGlobalVarDef* def);

/* Lookup loose : case + accent insensible, matche canonical / nameAscii /
 * nameEN. Retourne NULL si pas trouvé. */
const OwlGlobalVarDef* owl_globalvar_catalog_find(const char* name);

/* Predicate optimisé pour le DAP : true si `name` est l'une des 3 formes
 * (canonical / nameAscii / nameEN) d'une globalvar enregistrée. Match
 * strict (les 3 formes sont posées dans vm.globals donc on les voit
 * littéralement). */
bool owl_globalvar_catalog_is_global_name(const char* name);

/* Itération (pour CLI list + self-test). */
int                       owl_globalvar_catalog_count(void);
const OwlGlobalVarDef*    owl_globalvar_catalog_at(int index);

#ifdef __cplusplus
}
#endif

#endif /* OWL_GLOBALVAR_CATALOG_H */
