// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_keyword_catalog.h — catalogue pédagogique des mots-clés OWL.
 *
 * Couche pédagogique attachée par TokenType. Source de vérité unique :
 * la table KEYWORDS[] du scanner (cf. src/scanner.c) — ce catalogue
 * n'invente jamais de mots, il ajoute juste des fiches descriptives
 * (descFR/descEN, syntaxe, exemple) attachées à un TokenType existant.
 *
 * Les aliases FR/EN ne sont PAS stockés ici. Au moment d'afficher la
 * fiche d'un TokenType, on itère scanner_keyword_at() pour récupérer
 * dynamiquement tous ses aliases. Conséquence : ajouter un alias dans
 * le scanner se reflète instantanément dans `owl explain` sans toucher
 * au catalogue.
 *
 * Les composés (PAS DANS, SORTIR SELON, POUR TOUT...) n'ont pas de
 * TokenType propre — le scanner émet 2 tokens consécutifs. Ils vivent
 * dans une table séparée OWL_KEYWORD_COMPOSITES qui référence 2
 * TokenType existants.
 *
 * Cohérence garantie par les self-tests rule28-32 (cf.
 * registry_self_test.c) :
 *   28. Couverture montante : tout TokenType user-facing a 1 fiche
 *   29. Couverture descendante : toute fiche réfère un TokenType valide
 *   30. Sens non vides : chaque KeywordSense a tous ses champs remplis
 *   31. Composés valides : t1 et t2 existent dans le scanner
 *   32. (optionnel) Exemples compilables — pas activé en V1
 */

#ifndef OWL_KEYWORD_CATALOG_H
#define OWL_KEYWORD_CATALOG_H

#include "scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Une fiche sémantique. Un keyword multi-sens en a plusieurs (1 par
 * sens) ; un keyword mono-sens en a une seule. Tous les champs doivent
 * être renseignés non-vides (vérifié par rule 30). */
typedef struct {
    const char* label_fr;     /* Titre court du sens, ex. "Négation booléenne" */
    const char* label_en;
    const char* syntax;       /* Forme canonique, ex. "PAS <expression>" */
    const char* descFR;       /* Description pédagogique, reformulée canon OWL */
    const char* descEN;
    const char* example;      /* Court extrait OWL, doit être canonique */
} OwlKeywordSense;

typedef struct {
    TokenType               token;        /* identité primaire */
    const char*             category;     /* "Contrôle"/"Logique"/"OOP"/... */
    const OwlKeywordSense*  senses;       /* tableau, terminé par {NULL,...} */
    int                     senses_count; /* hors sentinel */
    /* Forme d'affichage canonique override. NULL = utilise
     * token_type_name(token).
     *
     * Utilisé (et load-bearing) par les seuls mots dont la forme normative
     * utilisateur diffère de celle rendue par token_type_name, parce que le
     * scanner en stocke une variante ASCII pour la normalisation :
     *   TOKEN_A    -> token_type_name "A"   / canon utilisateur "À"
     *   TOKEN_A_SC -> token_type_name "_A_" / canon utilisateur "_À_"
     * Sans l'override, `owl explain À` et `owl list keywords` titreraient
     * "A" (mesure ). Les deux mots sont aussi les seuls que
     * `--self-test-fmt-canon` signale en R2 avec `ÉLÉMENT` / `SÉPARÉE` :
     * accentués côté fiche, absents de fmt_keywords[].
     *
     * N'y mettre un override QUE dans ce cas : dupliquer une valeur que
     * token_type_name rend déjà crée une seconde source de vérité. */
    const char*             canon_display;
} OwlKeywordDef;

typedef struct {
    TokenType               t1;
    TokenType               t2;
    const char*             category;     /* idem */
    const OwlKeywordSense*  sense;        /* une seule fiche */
} OwlKeywordComposite;

/* ----- Couverture / predicate ----- */

/* True si ce TokenType doit avoir une fiche utilisateur. Filtre les
 * tokens techniques (EOF, NUMBER, IDENTIFIER, opérateurs ponctuationnels)
 * et les tokens déjà couverts par d'autres buckets (types primitifs,
 * widgets DSL). C'est la liste blanche que les self-tests respectent. */
bool is_keyword_user_facing(TokenType t);

/* ----- Lookup ----- */

/* Cherche la fiche attachée à un TokenType. NULL si pas couvert. */
const OwlKeywordDef* owl_keyword_def_for(TokenType t);

/* Cherche la fiche correspondant à un mot (un alias FR ou EN). Délègue
 * au scanner pour mot → TokenType, puis owl_keyword_def_for. Case+accent
 * insensible. NULL si mot inconnu OU TokenType pas couvert. */
const OwlKeywordDef* owl_keyword_find_by_word(const char* word);

/* Cherche une fiche composée (PAS DANS, etc.) par paire de TokenType. */
const OwlKeywordComposite* owl_keyword_composite_find(TokenType t1, TokenType t2);

/* Cherche une fiche composée par paire de mots (chaînes brutes). */
const OwlKeywordComposite* owl_keyword_composite_find_by_words(
    const char* w1, const char* w2);

/* ----- Itération ----- */

int  owl_keyword_count(void);
const OwlKeywordDef* owl_keyword_at(int i);

int  owl_keyword_composite_count(void);
const OwlKeywordComposite* owl_keyword_composite_at(int i);

#ifdef __cplusplus
}
#endif

#endif /* OWL_KEYWORD_CATALOG_H */
