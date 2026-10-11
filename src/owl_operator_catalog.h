// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_operator_catalog.h — catalogue pédagogique des opérateurs OWL.
 *
 * Architecture parallèle au keyword catalog (owl_keyword_catalog.h)
 * mais pour les opérateurs symboliques (`+`, `[~~`, `??*`, etc.).
 *
 * Pourquoi un catalogue séparé : les opérateurs ne sont pas dans la
 * table KEYWORDS[] du scanner (qui mappe des MOTS vers TokenType) —
 * ils sont tokenisés à part comme séquences de caractères
 * ponctuationnels. Le lookup utilisateur se fait donc par symbole
 * (chaîne brute "[~~" ou "??*"), pas via scanner_keyword_at().
 *
 * Réutilise la struct OwlKeywordSense de owl_keyword_catalog.h pour
 * la fiche pédagogique elle-même — cohérence d'affichage entre
 * keywords et opérateurs.
 *
 * Cohérence garantie par self-tests rule 32-35 (cf.
 * registry_self_test.c) :
 *   32. Couverture montante : tout TokenType opérateur user-facing
 *       non-pending a une OwlOperatorDef
 *   33. Couverture descendante : toute fiche a TokenType + display_form
 *       valides
 *   34. Senses non vides : chaque OwlKeywordSense est complet
 *   35. Pas de doublon display_form
 */

#ifndef OWL_OPERATOR_CATALOG_H
#define OWL_OPERATOR_CATALOG_H

#include "scanner.h"
#include "owl_keyword_catalog.h"  /* OwlKeywordSense réutilisée */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TokenType               token;          /* TOKEN_STARTS_WITH_VFLEX, etc. */
    const char*             display_form;   /* "[~~", "??*", "+", ... */
    const char*             category;       /* "Comparaison chaîne", ... */
    const OwlKeywordSense*  senses;         /* tableau, terminé par {NULL,...} */
    int                     senses_count;   /* hors sentinel */
} OwlOperatorDef;

/* ----- Couverture / predicate ----- */

/* True si ce TokenType d'opérateur doit avoir une fiche utilisateur.
 * Liste blanche maintenue ligne-à-ligne avec commentaires. */
bool is_operator_user_facing(TokenType t);

/* ----- Lookup ----- */

/* Cherche la fiche attachée à un TokenType. NULL si pas couvert. */
const OwlOperatorDef* owl_operator_def_for(TokenType t);

/* Cherche la fiche correspondant à un symbole (chaîne brute, ex. "[~~"
 * ou "??*"). Match exact byte-à-byte sur display_form. */
const OwlOperatorDef* owl_operator_find_by_symbol(const char* symbol);

/* ----- Itération ----- */

int  owl_operator_count(void);
const OwlOperatorDef* owl_operator_at(int i);

#ifdef __cplusplus
}
#endif

#endif /* OWL_OPERATOR_CATALOG_H */
