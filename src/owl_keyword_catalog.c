// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_keyword_catalog.c — implémentation du catalogue keyword.
 *
 * Pour la documentation architecturale, voir owl_keyword_catalog.h.
 *
 * Tous les TokenType user-facing du périmètre V1 ont une fiche.
 * Rule 28 (couverture montante) est en strict absolu : ajouter un
 * keyword au scanner sans rédiger sa fiche fait immédiatement
 * échouer le self-test registry.
 */

#include "owl_keyword_catalog.h"
#include <string.h>

/* ==================================================================
 * Periimètre user-facing — liste blanche des TokenType qui doivent
 * avoir une fiche pédagogique.
 *
 * Hors périmètre :
 *  - Tokens techniques (EOF, NUMBER, STRING, IDENTIFIER...)
 *  - Opérateurs ponctuationnels (parenthèses, virgule, point...)
 *  - Types primitifs (BOOLEEN, ENTIER, CHAINE...) — couverts par bucket
 *    PRIMITIVE dans `owl explain`
 *  - Tokens DSL UI (FENETRE, BOUTON, LIBELLE...) — couverts par bucket
 *    WIDGET dans `owl explain`
 *  - TOKEN_SANS / TOKEN_SIGNE — particules de type rejeté
 *    (sans signe), parsées avec les types
 * ================================================================== */
bool is_keyword_user_facing(TokenType t) {
    switch (t) {
        /* --- Contrôle de flux --- */
        case TOKEN_SI:
        case TOKEN_ALORS:
        case TOKEN_SINON:
        case TOKEN_FIN:
        case TOKEN_POUR:
        case TOKEN_A:
        case TOKEN_A_SC:
        case TOKEN_PAS:
        case TOKEN_PAS_SC:
        case TOKEN_TOUT:
        case TOKEN_DE:
        case TOKEN_DANS:
        case TOKEN_TANTQUE:
        case TOKEN_BOUCLE:
        case TOKEN_FAIRE:
        case TOKEN_SELON:
        case TOKEN_CAS:
        case TOKEN_AUTRE:
        case TOKEN_SORTIR:
        case TOKEN_CONTINUER:
        case TOKEN_RENVOYER:
        case TOKEN_RETOUR:
        case TOKEN_AVEC:
        case TOKEN_SUR:
        /* --- Déclaration --- */
        case TOKEN_EST:
        case TOKEN_SONT:
        case TOKEN_CONSTANTE:
        /* TOKEN_LOCAL : modificateur de paramètre, sémantique non
         * observable sur types valeur. Hors périmètre tant que la
         * fondation procédures n'est pas rédigée. */
        case TOKEN_GLOBAL:  /* bascule de portée top-level : exporte la
                             * déclaration au-delà du fichier (portée projet). */
        /* --- Composite particles --- */
        case TOKEN_PAR:
        /* --- Procedures --- */
        case TOKEN_PROCEDURE:
        case TOKEN_FONCTION:
        /* --- Déclarations de types composites user-defined --- */
        case TOKEN_STRUCTURE:
        case TOKEN_ENUMERATION:
        case TOKEN_COMBINAISON:
        /* --- OOP --- */
        case TOKEN_CLASSE:
        case TOKEN_HERITE:
        case TOKEN_INTERFACE:
        case TOKEN_IMPLEMENTE:
        /* TOKEN_PROPRIETES : bloc DSL Fenêtre (couvert par bucket
         * WIDGET / DSL_PROP). Hors périmètre keyword catalog. */
        case TOKEN_CONSTRUCTEUR:
        case TOKEN_DESTRUCTEUR:
        case TOKEN_OBJET:
        case TOKEN_MOIMEME:
        case TOKEN_MONCHAMP:
        case TOKEN_MAFENETRE:
        /* TOKEN_ANCETRE : reconnu, opérationnel via OP_INVOKE_SUPER pour
         * appel méthode classe parente (`Ancêtre.Méthode(...)`), . */
        case TOKEN_LIBERER:
        /* --- Visibilité --- */
        case TOKEN_PUBLIC:
        case TOKEN_PRIVE:
        case TOKEN_PROTEGE:
        /* TOKEN_GLOBALE (alias STATIC) : reconnu par le scanner mais sans
         * sémantique opérationnelle — toute déclaration `globale`/`static`
         * échoue au parse. Hors périmètre catalog tant qu'aucun comportement
         * réel n'est offert (ne pas exposer un mot-clé non fonctionnel). */
        /* --- Logique / arithmétique --- */
        case TOKEN_ET:
        case TOKEN_OU:
        case TOKEN_ET_SC:
        case TOKEN_OU_SC:
        case TOKEN_DANS_SC:
        case TOKEN_MODULO:
        case TOKEN_BIT_SHL:
        case TOKEN_BIT_SHR:
        /* --- Exceptions --- */
        case TOKEN_QUAND:
        case TOKEN_EXCEPTION:
        case TOKEN_ENFIN:
        /* --- Constantes littérales --- */
        case TOKEN_VRAI:
        case TOKEN_FAUX:
        case TOKEN_NUL:
        /* --- Divers --- */
        case TOKEN_ELEMENT:
        case TOKEN_SEPAREE:
            return true;
        default:
            return false;
    }
}

/* ==================================================================
 * Fiches pédagogiques
 *
 * Une fiche = un OwlKeywordDef avec son tableau de OwlKeywordSense.
 * Les exemples sont validés via `owl --check` au moment de la
 * rédaction (cf. routine handoff). Si la grammaire change, rule 30 ne
 * détecte pas la régression — c'est l'humain qui doit relancer le
 * check. Rule 32 (compile auto) reportée à V2.
 * ================================================================== */

/* --- SI ----------------------------------------------------------- */
static const OwlKeywordSense SENSES_SI[] = {
    {
        "Branchement conditionnel",
        "Conditional branching",
        "SI <condition> ALORS ... [SINON ...] FIN",
        "Exécute le bloc ALORS si la condition est vraie, sinon le bloc "
        "SINON optionnel. Cascade via SINON SI. Forme courte sur une "
        "ligne : `SI cond ALORS instr` (sans FIN).",
        "Runs the ALORS block when the condition holds, otherwise the "
        "optional SINON block. Cascade via SINON SI. Short one-line "
        "form: `SI cond ALORS instr` (no FIN).",
        "SI age >= 18 ALORS\n"
        "    Trace(\"Majeur\")\n"
        "FIN"
    }
};

/* --- SELON -------------------------------------------------------- */
static const OwlKeywordSense SENSES_SELON[] = {
    {
        "Aiguillage par filtrage",
        "Pattern-based dispatch",
        "SELON <expression> CAS <pattern> : ... AUTRE CAS : ... FIN",
        "Évalue l'expression une seule fois puis exécute le premier CAS "
        "qui matche. Pas de fall-through. AUTRE CAS optionnel, en "
        "dernier.",
        "Evaluates the expression once then runs the first matching "
        "CAS branch. No fall-through. AUTRE CAS optional, must come "
        "last.",
        "SELON statut\n"
        "    CAS \"actif\"  : Trace(\"OK\")\n"
        "    AUTRE CAS    : Trace(\"Inconnu\")\n"
        "FIN"
    }
};

/* --- PAS (multi-sens) --------------------------------------------- */
static const OwlKeywordSense SENSES_PAS[] = {
    {
        "Négation booléenne",
        "Boolean negation",
        "PAS <expression-booléenne>",
        "Inverse une valeur booléenne. Précédence unaire — se lie plus "
        "fort que ET/OU.",
        "Inverts a boolean value. Unary precedence — binds tighter "
        "than AND/OR.",
        "SI PAS estPresent ALORS\n"
        "    Trace(\"absent\")\n"
        "FIN"
    },
    {
        "Pas d'itération de boucle",
        "Loop step",
        "POUR <var> = <start> A <end> PAS <step>",
        "Incrément appliqué entre chaque tour d'une boucle POUR. Peut "
        "être négatif pour décrémenter.",
        "Step added between iterations of a POUR loop. May be "
        "negative to count down.",
        "POUR i = 1 A 10 PAS 2\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- SORTIR ------------------------------------------------------- */
static const OwlKeywordSense SENSES_SORTIR[] = {
    {
        "Sortie de boucle, optionnellement multi-niveau",
        "Loop break, optionally multi-level",
        "SORTIR  |  SORTIR(<N>)",
        "Quitte la boucle englobante (POUR, POUR TOUT, TANTQUE, "
        "BOUCLE). Avec un entier littéral N, sort de N boucles "
        "imbriquées d'un coup (1 ≤ N ≤ profondeur courante). Ne sort "
        "pas d'un SELON — utiliser SORTIR SELON.",
        "Exits the enclosing loop (POUR, POUR TOUT, TANTQUE, BOUCLE). "
        "With a literal integer N, exits N nested loops at once "
        "(1 ≤ N ≤ current depth). Does not exit a SELON — use SORTIR "
        "SELON for that.",
        "POUR x = 1 À 5\n"
        "    POUR y = 1 À 5\n"
        "        SI x * y > 10 ALORS SORTIR(2)\n"
        "    FIN\n"
        "FIN"
    }
};

/* --- ALORS -------------------------------------------------------- */
static const OwlKeywordSense SENSES_ALORS[] = {
    {
        "Marqueur du bloc THEN",
        "THEN block marker",
        "SI <condition> ALORS ... [SINON ...] FIN",
        "Sépare la condition du corps d'un SI. Obligatoire.",
        "Separates the condition from the body of a SI. Required.",
        "SI estPret ALORS\n"
        "    Trace(\"demarrage\")\n"
        "FIN"
    }
};

/* --- SINON -------------------------------------------------------- */
static const OwlKeywordSense SENSES_SINON[] = {
    {
        "Branche par défaut",
        "Else branch",
        "SI ... ALORS ... SINON ... FIN  |  SINON SI <condition> ALORS ...",
        "Branche exécutée quand la condition du SI est fausse. Cascade "
        "via SINON SI.",
        "Branch run when the SI condition is false. Cascade via "
        "SINON SI.",
        "SI n > 0 ALORS\n"
        "    Trace(\"positif\")\n"
        "SINON\n"
        "    Trace(\"nul ou negatif\")\n"
        "FIN"
    }
};

/* --- FIN ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_FIN[] = {
    {
        "Fin de bloc structuré",
        "End of structured block",
        "FIN",
        "Termine tout bloc structuré : SI, POUR, POUR TOUT, TANTQUE, "
        "BOUCLE, SELON, PROCEDURE, CLASSE, STRUCTURE, ENUMERATION. "
        "Apparié avec l'ouverture la plus récente.",
        "Closes any structured block: SI, POUR, POUR TOUT, TANTQUE, "
        "BOUCLE, SELON, PROCEDURE, CLASSE, STRUCTURE, ENUMERATION. "
        "Matches the most recent opener.",
        "POUR i = 1 A 3\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- POUR --------------------------------------------------------- */
static const OwlKeywordSense SENSES_POUR[] = {
    {
        "Boucle compteur",
        "Indexed loop",
        "POUR <var> = <start> À <end> [PAS <step>] [DepuisFin] ... FIN",
        "Boucle indicée. Le compteur est local au bloc, typé entier, "
        "automatiquement déclaré. Bornes inclusives.",
        "Indexed loop. The counter is local to the block, integer-typed, "
        "auto-declared. Inclusive bounds.",
        "POUR i = 1 À 5\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- À (séparateur de bornes) ------------------------------------- */
static const OwlKeywordSense SENSES_A[] = {
    {
        "Séparateur de bornes inclusives",
        "Inclusive bounds separator",
        "<start> À <end>",
        "Sépare les bornes inclusives d'une plage : boucle POUR ou "
        "pattern CAS. Borne ré-évaluée à chaque tour dans une boucle. "
        "Variante évaluation unique : _À_.",
        "Separates the inclusive bounds of a range: POUR loop or CAS "
        "pattern. Bound re-evaluated each iteration in a loop. "
        "Single-evaluation variant: _À_.",
        "POUR i = 1 À 10\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- _À_ (variante eval unique) ----------------------------------- */
static const OwlKeywordSense SENSES_A_SC[] = {
    {
        "Borne évaluée une seule fois",
        "Bound evaluated once",
        "POUR <var> = <start> _À_ <end>",
        "Variante de À où la borne finale est évaluée une seule fois au "
        "démarrage. À utiliser quand l'expression d'arrivée est "
        "coûteuse ou doit rester stable pendant la boucle.",
        "Variant of À where the upper bound is evaluated once at start. "
        "Use when the bound expression is costly or must stay stable "
        "throughout the loop.",
        "POUR i = 1 _À_ Taille(tValeurs)\n"
        "    Trace(tValeurs[i])\n"
        "FIN"
    }
};

/* --- _PAS_ (variante step eval unique) ---------------------------- */
static const OwlKeywordSense SENSES_PAS_SC[] = {
    {
        "Step évalué une seule fois",
        "Step evaluated once",
        "POUR <var> = <start> À <end> _PAS_ <step>",
        "Variante de PAS où le step est évalué une seule fois au "
        "démarrage. À utiliser quand l'expression du pas est coûteuse "
        "ou doit rester stable.",
        "Variant of PAS where the step is evaluated once at start. "
        "Use when the step expression is costly or must stay stable.",
        "POUR i = 0 À 20 _PAS_ 5\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- TOUT --------------------------------------------------------- */
static const OwlKeywordSense SENSES_TOUT[] = {
    {
        "Marqueur de boucle foreach",
        "Foreach loop marker",
        "POUR TOUT <var> [, <var2>] DE <itérable> ... FIN",
        "Marqueur de parcours foreach. Utilisé immédiatement après "
        "POUR. Les formes TOUT, TOUS, TOUTE, CHAQUE sont strictement "
        "équivalentes — le choix relève du style.",
        "Foreach traversal marker. Used right after POUR. The forms "
        "TOUT, TOUS, TOUTE, CHAQUE are strictly equivalent — choice "
        "is stylistic.",
        "POUR TOUT s DE [\"a\", \"b\", \"c\"]\n"
        "    Trace(s)\n"
        "FIN"
    }
};

/* --- DE ----------------------------------------------------------- */
static const OwlKeywordSense SENSES_DE[] = {
    {
        "Préposition de lien",
        "Linking preposition",
        "<chose> de <cible>",
        "Préposition utilisée dans trois contextes : type composite "
        "(`tableau de chaînes`) et héritage (`Hérite de Parent`), où "
        "la forme canon est en minuscules ; parcours foreach (`POUR "
        "TOUT x DE tab`), où elle passe en majuscules.",
        "Preposition used in three contexts: composite type (`tableau "
        "de chaînes`) and inheritance (`Hérite de Parent`), where the "
        "canonical form is lowercase; foreach traversal (`POUR TOUT x "
        "DE tab`), where it is uppercase.",
        "tFruits est un tableau de chaînes = [\"pomme\", \"poire\"]\n"
        "POUR TOUT s DE tFruits\n"
        "    Trace(s)\n"
        "FIN"
    }
};

/* --- DANS (multi-sens) -------------------------------------------- */
static const OwlKeywordSense SENSES_DANS[] = {
    {
        "Test d'appartenance",
        "Membership test",
        "<valeur> DANS (<elem1>, <elem2>, ...)",
        "Opérateur booléen : Vrai si la valeur figure dans la liste "
        "énumérée. Liste non vide obligatoire. Négation : PAS DANS. "
        "Variante évaluation paresseuse : _DANS_.",
        "Boolean operator: true if the value appears in the "
        "enumerated list. List must be non-empty. Negation: PAS DANS. "
        "Lazy-evaluation variant: _DANS_.",
        "SI x DANS (1, 2, 3) ALORS\n"
        "    Trace(\"dans la plage\")\n"
        "FIN"
    },
    {
        "Marqueur de chaîne source",
        "Source string marker",
        "POUR TOUTE Position <p> DE <motif> DANS <chaîne>",
        "Dans la variante `POUR TOUTE Position`, introduit la chaîne "
        "source où chercher les occurrences du motif.",
        "In the `POUR TOUTE Position` variant, introduces the source "
        "string where pattern occurrences are searched.",
        "POUR TOUTE Position p DE \";\" DANS \"a;b;c\"\n"
        "    Trace(p)\n"
        "FIN"
    }
};

/* --- _DANS_ (variante short-circuit) ------------------------------ */
static const OwlKeywordSense SENSES_DANS_SC[] = {
    {
        "Appartenance évaluée paresseusement",
        "Lazy-evaluated membership",
        "<valeur> _DANS_ (<elem1>, <elem2>, ...)",
        "Variante de DANS qui s'arrête au premier match. À utiliser "
        "quand les éléments de la liste sont coûteux à calculer.",
        "Variant of DANS that stops at the first match. Use when "
        "the list elements are costly to compute.",
        "SI x _DANS_ (1, 2, 3) ALORS\n"
        "    Trace(\"trouve\")\n"
        "FIN"
    }
};

/* --- POUR TOUT (composé) ------------------------------------------ */
static const OwlKeywordSense SENSE_POUR_TOUT = {
    "Parcours foreach",
    "Foreach traversal",
    "POUR TOUT <var>[, <var2>] DE <itérable> [AVEC <cond>] [DepuisFin] ... FIN",
    "Parcours d'un tableau ordinal, tableau associatif ou chaîne. "
    "Variable secondaire optionnelle : index sur ordinal, clé sur "
    "associatif. Filtre inline via AVEC, parcours inverse via "
    "DepuisFin. Variantes chaîne : `POUR TOUTE chaîne x DE s SEPAREE "
    "par <sép>` (découpage par séparateur), `POUR TOUTE Position p "
    "DE <motif> DANS s` (occurrences d'un motif).",
    "Traversal of an ordinal array, associative array, or string. "
    "Optional secondary variable: index on ordinal, key on "
    "associative. Inline filter via AVEC, reverse traversal via "
    "DepuisFin. String variants: `POUR TOUTE chaîne x DE s SEPAREE "
    "par <sep>` (split), `POUR TOUTE Position p DE <pattern> DANS s` "
    "(occurrences).",
    "POUR TOUT s DE [\"a\", \"b\", \"c\"]\n"
    "    Trace(s)\n"
    "FIN"
};

/* --- TANTQUE ------------------------------------------------------ */
static const OwlKeywordSense SENSES_TANTQUE[] = {
    {
        "Boucle conditionnelle pré-testée",
        "Pre-tested conditional loop",
        "TANTQUE <condition> ... FIN",
        "Boucle qui répète son corps tant que la condition est vraie. "
        "La condition est évaluée avant chaque itération — si fausse "
        "au départ, le corps n'est jamais exécuté.",
        "Loop that repeats its body while the condition holds. The "
        "condition is checked before each iteration — if false at "
        "start, the body never runs.",
        "n est un entier = 3\n"
        "TANTQUE n > 0\n"
        "    Trace(n)\n"
        "    n--\n"
        "FIN"
    }
};

/* --- BOUCLE ------------------------------------------------------- */
static const OwlKeywordSense SENSES_BOUCLE[] = {
    {
        "Boucle inconditionnelle ou comptée",
        "Unconditional or counted loop",
        "BOUCLE ... FIN  |  BOUCLE (<N>) ... FIN",
        "Sans argument : boucle infinie, sortie via SORTIR. Avec un "
        "compteur entre parenthèses : exécute le corps exactement N "
        "fois. N est évalué une seule fois au démarrage ; N ≤ 0 = "
        "zéro itération sans erreur.",
        "Without argument: infinite loop, exit via SORTIR. With a "
        "counter in parentheses: runs the body exactly N times. N is "
        "evaluated once at start; N ≤ 0 yields zero iterations with "
        "no error.",
        "BOUCLE (3)\n"
        "    Trace(\"ping\")\n"
        "FIN"
    }
};

/* --- FAIRE -------------------------------------------------------- */
static const OwlKeywordSense SENSES_FAIRE[] = {
    {
        "Séparateur try / handler",
        "Try / handler separator",
        "QUAND EXCEPTION DANS <try> FAIRE <handler> [SINON <else>] FIN",
        "Marqueur qui sépare le bloc surveillé du bloc gestionnaire "
        "dans la forme spécifique de gestion d'exception.",
        "Marker that separates the protected block from the handler "
        "block in the specific exception-handling form.",
        "QUAND EXCEPTION DANS\n"
        "    Trace(\"essai\")\n"
        "FAIRE\n"
        "    Trace(\"handler\")\n"
        "FIN"
    }
};

/* --- CAS ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_CAS[] = {
    {
        "Pattern d'aiguillage",
        "Dispatch pattern",
        "CAS <pattern>[, <pattern2>, ...] [:] ...",
        "Branche d'un SELON. Plusieurs patterns par CAS séparés par "
        "virgule. Formes acceptées : valeur simple, plage `1 A 10`, "
        "borne ouverte `A 8` ou `11 A`, comparateur préfixe `> 100`, "
        "sandwich `10 <= * <= 20`, comparateurs souples chaîne "
        "(`[=`, `[~`, `[=]`, etc.). Le `:` après le pattern est "
        "facultatif.",
        "Branch of a SELON. Multiple patterns per CAS, comma-separated. "
        "Accepted forms: plain value, range `1 A 10`, open bound `A 8` "
        "or `11 A`, prefix comparator `> 100`, sandwich `10 <= * <= "
        "20`, soft string comparators (`[=`, `[~`, `[=]`, etc.). The "
        "colon after the pattern is optional.",
        "SELON n\n"
        "    CAS 1, 2     : Trace(\"petit\")\n"
        "    CAS 3 A 10   : Trace(\"moyen\")\n"
        "    AUTRE CAS    : Trace(\"autre\")\n"
        "FIN"
    }
};

/* --- AUTRE -------------------------------------------------------- */
static const OwlKeywordSense SENSES_AUTRE[] = {
    {
        "Branche par défaut",
        "Default branch",
        "AUTRE CAS [:] ...  |  AUTRE [:] ...",
        "Branche par défaut d'un SELON, exécutée si aucun CAS ne "
        "matche. Doit venir en dernier. Forme canon : `AUTRE CAS`, "
        "le `CAS` est syntaxiquement optionnel.",
        "Default branch of a SELON, run when no CAS matches. Must "
        "come last. Canonical form: `AUTRE CAS`, the `CAS` is "
        "syntactically optional.",
        "SELON s\n"
        "    CAS \"a\"   : Trace(\"A\")\n"
        "    AUTRE CAS : Trace(\"hors A\")\n"
        "FIN"
    }
};

/* --- SORTIR SELON (composé) --------------------------------------- */
static const OwlKeywordSense SENSE_SORTIR_SELON = {
    "Sortie de SELON",
    "Switch break",
    "SORTIR SELON",
    "Quitte le SELON englobant sans toucher à la boucle qui "
    "l'entoure éventuellement. Utile pour court-circuiter un CAS en "
    "milieu de bloc. Hors d'un SELON : erreur de compilation.",
    "Exits the enclosing SELON without touching a surrounding loop. "
    "Useful to short-circuit a CAS body. Outside a SELON: compile "
    "error.",
    "SELON qte\n"
    "    CAS 1\n"
    "        SI estAdmin ALORS SORTIR SELON\n"
    "        Trace(\"promo\")\n"
    "    AUTRE CAS : Trace(\"standard\")\n"
    "FIN"
};

/* --- CONTINUER ---------------------------------------------------- */
static const OwlKeywordSense SENSES_CONTINUER[] = {
    {
        "Itération suivante",
        "Continue to next iteration",
        "CONTINUER",
        "Saut à l'itération suivante de la boucle englobante. POUR : "
        "incrémente le compteur. TANTQUE : ré-évalue la condition. "
        "BOUCLE : retour en tête. Hors d'une boucle : erreur de "
        "compilation.",
        "Skip to the next iteration of the enclosing loop. POUR: "
        "increment the counter. TANTQUE: re-check the condition. "
        "BOUCLE: jump back to head. Outside a loop: compile error.",
        "POUR i = 1 A 5\n"
        "    SI i modulo 2 = 0 ALORS CONTINUER\n"
        "    Trace(i)\n"
        "FIN"
    }
};

/* --- RENVOYER ----------------------------------------------------- */
static const OwlKeywordSense SENSES_RENVOYER[] = {
    {
        "Retour de procédure (forme préférée pour fonction)",
        "Procedure return (preferred form for functions)",
        "RENVOYER [<valeur>]  |  RENVOYER (<v1>, <v2>, ...)",
        "Termine la procédure courante, optionnellement avec une ou "
        "plusieurs valeurs de retour (forme tuple). Forme préférée "
        "pour les fonctions qui calculent un résultat. Mélange "
        "interdit avec RETOUR dans la même procédure (erreur compile).",
        "Exits the current procedure, optionally with one or more "
        "return values (tuple form). Preferred form for functions "
        "that produce a result. Mixing with RETOUR in the same "
        "procedure is forbidden (compile error).",
        "PROCEDURE doubler(n est un entier) : entier\n"
        "    RENVOYER n * 2\n"
        "FIN"
    }
};

/* --- RETOUR ------------------------------------------------------- */
static const OwlKeywordSense SENSES_RETOUR[] = {
    {
        "Retour de procédure (forme préférée pour action)",
        "Procedure return (preferred form for action)",
        "RETOUR [<valeur>]  |  RETOUR (<v1>, <v2>, ...)",
        "Termine la procédure courante, optionnellement avec valeur. "
        "Forme préférée pour les procédures-actions qui sortent sans "
        "résultat. Mélange interdit avec RENVOYER dans la même "
        "procédure (erreur compile). Le mode `RETOUR = X` "
        "(assignation différée) n'est pas supporté.",
        "Exits the current procedure, optionally with a value. "
        "Preferred form for action procedures that exit without a "
        "result. Mixing with RENVOYER in the same procedure is "
        "forbidden (compile error). The `RETOUR = X` form "
        "(deferred assignment) is not supported.",
        "PROCEDURE journaliser(msg est une chaîne)\n"
        "    SI msg = \"\" ALORS RETOUR\n"
        "    Trace(msg)\n"
        "FIN"
    }
};

/* --- QUAND -------------------------------------------------------- */
static const OwlKeywordSense SENSES_QUAND[] = {
    {
        "Introduction d'un handler d'exception",
        "Exception handler introduction",
        "QUAND EXCEPTION ... FIN  |  QUAND EXCEPTION DANS ... [FAIRE ...] [SINON ...] [ENFIN ...] FIN",
        "Introduit un handler d'exception. Toujours suivi de EXCEPTION. "
        "Forme générale : couvre toute la suite de la procédure, et la "
        "termine après l'exécution du handler (use-once). Forme "
        "spécifique avec DANS : protège un bloc précis, reprise du "
        "flux après FIN. Clauses optionnelles dans l'ordre strict FAIRE "
        "→ SINON → ENFIN. La forme générale est refusée à l'intérieur "
        "d'une boucle.",
        "Introduces an exception handler. Always followed by EXCEPTION. "
        "General form: covers the rest of the procedure and ends it "
        "after the handler runs (use-once). Specific form with DANS: "
        "protects a single block, flow resumes after FIN. Optional "
        "clauses in strict order FAIRE → SINON → ENFIN. The general "
        "form is refused inside a loop.",
        "PROCEDURE traiter() : chaîne\n"
        "    QUAND EXCEPTION\n"
        "        RENVOYER \"[erreur]\"\n"
        "    FIN\n"
        "    RENVOYER \"ok\"\n"
        "FIN"
    }
};

/* --- ENFIN -------------------------------------------------------- */
static const OwlKeywordSense SENSES_ENFIN[] = {
    {
        "Nettoyage garanti",
        "Guaranteed cleanup",
        "QUAND EXCEPTION DANS <try> [FAIRE ...] [SINON ...] ENFIN <cleanup> FIN",
        "Clause optionnelle de la forme spécifique. Son corps "
        "s'exécute toujours, sur les quatre chemins (succès, exception "
        "attrapée, exception traversante, re-levée). Ordre des clauses "
        "strict : après FAIRE et SINON, juste avant FIN. Un seul ENFIN "
        "par bloc. Refusé dans la forme générale. Interdits dans le "
        "corps : RENVOYER, RETOUR, SORTIR, CONTINUER, EXCEPTION "
        "(re-lance), ExceptionDéclenche — le cleanup ne doit pas "
        "interrompre le flux.",
        "Optional clause of the specific form. Its body always runs, "
        "on all four paths (success, caught exception, traversing "
        "exception, re-raised). Strict clause order: after FAIRE and "
        "SINON, just before FIN. One ENFIN per block. Refused in the "
        "general form. Forbidden in its body: RENVOYER, RETOUR, "
        "SORTIR, CONTINUER, EXCEPTION (re-raise), ExceptionDéclenche "
        "— cleanup must not interrupt the flow.",
        "QUAND EXCEPTION DANS\n"
        "    f = fOuvre(\"data.txt\", foLecture)\n"
        "    Trace(fLisLigne(f))\n"
        "ENFIN\n"
        "    fFerme(f)\n"
        "FIN"
    }
};

/* --- EXCEPTION (multi-sens) --------------------------------------- */
static const OwlKeywordSense SENSES_EXCEPTION[] = {
    {
        "Marqueur du handler",
        "Handler marker",
        "QUAND EXCEPTION ...  |  QUAND EXCEPTION DANS ...",
        "Suit immédiatement QUAND pour déclarer un handler. Désigne "
        "l'événement traité — toute exception levée dans la zone "
        "couverte est attrapée.",
        "Follows QUAND directly to declare a handler. Designates the "
        "event handled — any exception raised in the covered area is "
        "caught.",
        "QUAND EXCEPTION DANS\n"
        "    Traiter()\n"
        "FAIRE\n"
        "    Trace(\"echec\")\n"
        "FIN"
    },
    {
        "Re-levée de l'exception courante",
        "Re-raise of current exception",
        "EXCEPTION",
        "Seul, dans le corps d'un handler, relance l'exception "
        "courante vers le handler englobant avec son message, son "
        "code et son contexte intacts. Équivalent à ExceptionPropage(). "
        "Hors d'un handler : erreur de compilation.",
        "Alone, inside a handler body, re-raises the current "
        "exception to the enclosing handler with message, code and "
        "context intact. Equivalent to ExceptionPropage(). Outside a "
        "handler: compile error.",
        "QUAND EXCEPTION DANS\n"
        "    Action()\n"
        "FAIRE\n"
        "    SI ExceptionInfo(errCode) <> 99 ALORS EXCEPTION\n"
        "    Trace(\"traité localement\")\n"
        "FIN"
    }
};

/* --- AVEC (multi-sens) -------------------------------------------- */
static const OwlKeywordSense SENSES_AVEC[] = {
    {
        "Bloc d'accès aux membres",
        "Member-access block",
        "AVEC <objet> ... FIN",
        "Préfixe implicite pour accéder aux membres d'un objet sans "
        "les répéter. Dans le bloc, `.membre` désigne `<objet>.membre`. "
        "Imbrications possibles.",
        "Implicit prefix to access an object's members without "
        "repeating them. Inside the block, `.member` means "
        "`<object>.member`. Nesting supported.",
        "AVEC oPersonne\n"
        "    Trace(.sNom)\n"
        "    Trace(.nAge)\n"
        "FIN"
    },
    {
        "Filtre inline de foreach",
        "Inline foreach filter",
        "POUR TOUT <var> DE <itérable> AVEC <condition> ... FIN",
        "Filtre inline d'une boucle POUR TOUT. Équivaut à `SI NON "
        "condition ALORS CONTINUER` injecté en tête du corps — seuls "
        "les éléments validant la condition sont traités.",
        "Inline filter for a POUR TOUT loop. Equivalent to `SI NON "
        "condition ALORS CONTINUER` injected at the start of the "
        "body — only elements satisfying the condition are processed.",
        "POUR TOUT v DE tValeurs AVEC v > 10\n"
        "    Trace(v)\n"
        "FIN"
    }
};

/* --- EST ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_EST[] = {
    {
        "Déclarateur de type",
        "Type declarator",
        "<nom> est un[e] <type> [= <init>]",
        "Déclare une variable ou un membre avec un type explicite. "
        "Peut être suivi d'une initialisation. Forme plurielle : "
        "`sont`.",
        "Declares a variable or member with an explicit type. May be "
        "followed by an initializer. Plural form: `sont`.",
        "age est un entier = 30\n"
        "nom est une chaîne = \"Alice\""
    }
};

/* --- SONT --------------------------------------------------------- */
static const OwlKeywordSense SENSES_SONT[] = {
    {
        "Déclarateur de type, forme plurielle",
        "Type declarator, plural form",
        "<nom1>, <nom2>, ... sont des <type>",
        "Déclare plusieurs variables d'un même type sur une ligne. "
        "Forme plurielle de `est`.",
        "Declares several same-typed variables on one line. Plural "
        "form of `est`.",
        "x, y, z sont des entiers"
    }
};

/* --- CONSTANTE ---------------------------------------------------- */
static const OwlKeywordSense SENSES_CONSTANTE[] = {
    {
        "Bloc de constantes",
        "Constant block",
        "CONSTANTE <nom1> = <litéral1>  <nom2> = <litéral2>  ... FIN",
        "Déclare un bloc de constantes globales immuables. Valeurs "
        "littérales uniquement : entier, réel, chaîne, Vrai/Faux, "
        "Null, ou leur négation unaire. Toute tentative de "
        "réassignation est rejetée à la compilation.",
        "Declares a block of immutable global constants. Literal "
        "values only: integer, real, string, Vrai/Faux, Null, or "
        "their unary negation. Any reassignment is rejected at "
        "compile time.",
        "CONSTANTE\n"
        "    PI_APPROX = 3.14159\n"
        "    NOM_APP   = \"MonApp\"\n"
        "FIN"
    }
};

/* --- PUBLIC ------------------------------------------------------- */
static const OwlKeywordSense SENSES_PUBLIC[] = {
    {
        "Visibilité publique",
        "Public visibility",
        "PUBLIC <membre1> ... <membreN>  |  PROCEDURE PUBLIC <nom>(...)",
        "Marque les membres ou méthodes accessibles depuis n'importe "
        "où, y compris en dehors de la classe. Posé en tête de bloc, "
        "s'applique à tous les membres jusqu'au prochain modificateur "
        "de visibilité.",
        "Marks members or methods accessible from anywhere, including "
        "outside the class. Placed at the top of a block, applies to "
        "all following members until the next visibility modifier.",
        "cCompte est une Classe\n"
        "PUBLIC\n"
        "    sIBAN est une chaîne\n"
        "FIN"
    }
};

/* --- PRIVE -------------------------------------------------------- */
static const OwlKeywordSense SENSES_PRIVE[] = {
    {
        "Visibilité privée",
        "Private visibility",
        "PRIVE <membre1> ... <membreN>  |  PROCEDURE PRIVE <nom>(...)",
        "Marque les membres ou méthodes accessibles uniquement depuis "
        "l'intérieur de la classe. Ni les classes filles ni le code "
        "externe ne peuvent y accéder.",
        "Marks members or methods accessible only from inside the "
        "class. Neither subclasses nor external code can reach them.",
        "cCompte est une Classe\n"
        "PRIVE\n"
        "    nSolde est un entier\n"
        "FIN"
    }
};

/* --- PROTEGE ------------------------------------------------------ */
static const OwlKeywordSense SENSES_PROTEGE[] = {
    {
        "Visibilité protégée",
        "Protected visibility",
        "PROTEGE <membre1> ... <membreN>  |  PROCEDURE PROTEGE <nom>(...)",
        "Marque les membres ou méthodes accessibles depuis la classe "
        "elle-même et ses classes filles. Le code externe est exclu.",
        "Marks members or methods accessible from the class itself "
        "and its subclasses. External code is excluded.",
        "cBase est une Classe\n"
        "PROTEGE\n"
        "    nId est un entier\n"
        "FIN"
    }
};

/* --- GLOBAL ------------------------------------------------------- */
static const OwlKeywordSense SENSES_GLOBAL[] = {
    {
        "Portée projet",
        "Project scope",
        "GLOBAL <déclaration>  |  GLOBAL (seul sur une ligne)",
        "Promeut une déclaration de niveau fichier à la portée du "
        "projet : la variable reste visible au-delà du fichier où elle "
        "est écrite. Placé seul sur une ligne, bascule le mode des "
        "déclarations suivantes jusqu'au prochain changement ; c'est "
        "le mode par défaut au niveau fichier. À l'opposé, une "
        "déclaration confinée reste privée au fichier courant.",
        "Promotes a file-level declaration to project scope: the "
        "variable stays visible beyond the file where it is written. "
        "Placed alone on a line, it switches the mode of the following "
        "declarations until the next change; this is the default mode "
        "at file level. Conversely, a confined declaration stays "
        "private to the current file.",
        "GLOBAL gCompteur est un entier = 0\n"
        "Trace(gCompteur)"
    }
};

/* --- CLASSE ------------------------------------------------------- */
static const OwlKeywordSense SENSES_CLASSE[] = {
    {
        "Déclaration de classe",
        "Class declaration",
        "<nom> est une Classe  [PUBLIC|PRIVE|PROTEGE] <membres>  PROCEDURE Constructeur(...)  PROCEDURE <méthode>(...)  FIN",
        "Déclare une classe avec ses membres et leurs blocs de "
        "visibilité. Le constructeur et les méthodes sont déclarés "
        "inline dans le corps de la classe, entre le `est une Classe` "
        "et le `FIN` final. Le membre courant s'accède via `:` "
        "(ex. `:sNom`). Instanciation : `o est un <nom>(args)`.",
        "Declares a class with its members and visibility blocks. "
        "Constructor and methods are declared inline in the class "
        "body, between `est une Classe` and the final `FIN`. The "
        "current member is accessed via `:` (e.g. `:sNom`). "
        "Instantiation: `o est un <name>(args)`.",
        "cAnimal est une Classe\n"
        "PUBLIC\n"
        "    sNom est une chaîne\n"
        "    PROCEDURE Constructeur(sNom est une chaîne)\n"
        "        :sNom = sNom\n"
        "    FIN\n"
        "FIN"
    }
};

/* --- HERITE ------------------------------------------------------- */
static const OwlKeywordSense SENSES_HERITE[] = {
    {
        "Héritage de classe",
        "Class inheritance",
        "<classe-fille> est une Classe  Hérite de <classe-parente>  ... FIN",
        "Déclare que la classe hérite des membres et méthodes d'une "
        "classe parente. Posé en tête de corps de classe. Une seule "
        "classe parente (pas d'héritage multiple). Pour les "
        "interfaces, voir `implemente`.",
        "Declares that the class inherits members and methods from a "
        "parent class. Placed at the top of the class body. Single "
        "parent (no multiple inheritance). For interfaces, see "
        "`implemente`.",
        "cChat est une Classe\n"
        "    Hérite de cAnimal\n"
        "FIN"
    }
};

/* --- INTERFACE ---------------------------------------------------- */
static const OwlKeywordSense SENSES_INTERFACE[] = {
    {
        "Déclaration d'interface",
        "Interface declaration",
        "<nom> est une Interface  PROCEDURE <méthode>(args) ... FIN",
        "Déclare une interface — contrat de méthodes que les classes "
        "implémentatrices doivent fournir. Pas de corps de méthode "
        "dans l'interface, seulement leur signature. Une classe "
        "annonce qu'elle implémente une interface via `implemente`.",
        "Declares an interface — a contract of methods that "
        "implementing classes must provide. No method bodies in the "
        "interface, just their signatures. A class announces it "
        "implements an interface via `implemente`.",
        "iAffichable est une Interface\n"
        "    PROCEDURE Afficher()\n"
        "FIN"
    }
};

/* --- IMPLEMENTE --------------------------------------------------- */
static const OwlKeywordSense SENSES_IMPLEMENTE[] = {
    {
        "Implémentation d'interface",
        "Interface implementation",
        "<classe> est une Classe  implemente <interface>  ... FIN",
        "Annonce qu'une classe respecte le contrat d'une interface. "
        "Posé en tête de corps de classe. La classe doit fournir une "
        "méthode pour chaque procédure déclarée dans l'interface.",
        "Declares that a class fulfills an interface contract. Placed "
        "at the top of the class body. The class must provide a "
        "method for each procedure declared in the interface.",
        "cPersonne est une Classe\n"
        "    implemente iAffichable\n"
        "FIN"
    }
};

/* --- CONSTRUCTEUR ------------------------------------------------- */
static const OwlKeywordSense SENSES_CONSTRUCTEUR[] = {
    {
        "Appel du constructeur parent",
        "Parent constructor call",
        "Constructeur <classe-parente>(args)",
        "Dans le constructeur d'une classe fille, appelle le "
        "constructeur de la classe parente avec les arguments fournis. "
        "Doit être la première instruction du corps avant toute autre "
        "initialisation.",
        "Inside a subclass constructor, calls the parent class "
        "constructor with the given arguments. Must be the first "
        "statement of the body, before any other initialization.",
        "cChild est une Classe\n"
        "    Hérite de cBase\n"
        "PUBLIC\n"
        "    sExtra est une chaîne\n"
        "    PROCEDURE Constructeur(id est un entier, sExt est une chaîne)\n"
        "        Constructeur cBase(id)\n"
        "        :sExtra = sExt\n"
        "    FIN\n"
        "FIN"
    }
};

/* --- DESTRUCTEUR -------------------------------------------------- */
static const OwlKeywordSense SENSES_DESTRUCTEUR[] = {
    {
        "Déclaration de destructeur",
        "Destructor declaration",
        "PROCEDURE Destructeur()",
        "Déclare le destructeur d'une classe — appelé automatiquement "
        "quand une instance est libérée par `Libérer` ou par le "
        "ramasse-miettes. Le destructeur est déclaré inline dans le "
        "corps de la classe.",
        "Declares a class destructor — called automatically when an "
        "instance is released by `Libérer` or by the garbage collector. "
        "The destructor is declared inline in the class body.",
        "cRessource est une Classe\n"
        "PUBLIC\n"
        "    sNom est une chaîne\n"
        "    PROCEDURE Destructeur()\n"
        "        Trace(\"libere : \" + :sNom)\n"
        "    FIN\n"
        "FIN"
    }
};

/* --- OBJET -------------------------------------------------------- */
static const OwlKeywordSense SENSES_OBJET[] = {
    {
        "Instance courante dans une méthode",
        "Current instance inside a method",
        "objet",
        "Désigne l'instance sur laquelle la méthode courante est "
        "appelée. Utile pour retourner soi-même (chaînage fluide de "
        "méthodes) ou passer l'instance courante en argument. "
        "Utilisable uniquement dans le corps d'une méthode (pas dans "
        "une PROCEDURE GLOBALE de classe — pas d'instance courante).",
        "Refers to the instance the current method is being called on. "
        "Useful to return self (fluent method chaining) or to pass the "
        "current instance as an argument. Only valid inside a method "
        "body (not inside a class GLOBALE method — no current instance).",
        "cBuilder est une Classe\n"
        "PUBLIC\n"
        "    sNom est une chaîne\n"
        "    PROCEDURE FixeNom(s est une chaîne) : cBuilder\n"
        "        :sNom = s\n"
        "        RENVOYER objet\n"
        "    FIN\n"
        "FIN"
    }
};

/* --- MOIMEME ------------------------------------------------------ */
static const OwlKeywordSense SENSES_MOIMEME[] = {
    {
        "Champ emetteur du handler courant",
        "Control that triggered the current handler",
        "MoiMême",
        "Désigne le champ qui exécute le code en cours. Dans un handler "
        "d'événement (surClic, surBoutonGauche…), MoiMême est le champ "
        "déclencheur — pratique pour partager un handler entre N cellules "
        "d'une grille. Dans le bloc top-level d'une Fenêtre ou d'un État, "
        "MoiMême désigne la fenêtre ou l'état lui-même. Hérité dans la "
        "chaîne d'appel : une procédure appelée depuis un handler voit "
        "MoiMême résoudre au champ émetteur initial.",
        "Refers to the control currently running the code. Inside an "
        "event handler (surClic, surBoutonGauche…), MySelf is the "
        "triggering control — handy to share one handler across N grid "
        "cells. In a Window or Report top-level block, MySelf refers to "
        "the window or report itself. Inherited along the call chain: "
        "a procedure called from a handler still sees MySelf resolve to "
        "the original triggering control.",
        "// Une même procédure surClic partagée par plusieurs cellules\n"
        "PROCEDURE surClic()\n"
        "    Trace(MoiMême..Libellé)\n"
        "FIN"
    }
};

/* --- MONCHAMP ----------------------------------------------------- */
static const OwlKeywordSense SENSES_MONCHAMP[] = {
    {
        "Champ courant, sans repli",
        "Current control, no fallback",
        "MonChamp",
        "Désigne toujours le champ qui exécute le code en cours. Version "
        "stricte de MoiMême : là où MoiMême se rabat sur la fenêtre ou l'état "
        "quand aucun champ n'est actif, MonChamp exige un champ courant et "
        "signale une erreur sinon. Utilisable dans un handler d'événement de "
        "champ ou dans une procédure appelée depuis ce handler (héritage de "
        "la chaîne d'appel). MonChamp..Propriété lit ou écrit une propriété "
        "du champ.",
        "Always refers to the control currently running the code. Strict "
        "form of MySelf: where MySelf falls back to the window or report when "
        "no control is active, MyControl requires a current control and "
        "signals an error otherwise. Usable inside a control's event handler "
        "or in a procedure called from that handler (call-chain inheritance). "
        "MyControl..Property reads or writes a property of the control.",
        "PROCEDURE surClic()\n"
        "    MonChamp..Libellé = \"Cliqué\"\n"
        "FIN"
    }
};

/* --- MAFENETRE ---------------------------------------------------- */
static const OwlKeywordSense SENSES_MAFENETRE[] = {
    {
        "Fenêtre courante",
        "Current window",
        "MaFenêtre",
        "Désigne la fenêtre en cours d'exécution. Depuis le corps d'une "
        "fenêtre, un champ ou un handler d'événement, MaFenêtre pointe la "
        "fenêtre parente — pratique pour lire ou modifier ses propriétés sans "
        "répéter son nom. Hors de toute fenêtre ouverte, MaFenêtre signale "
        "une erreur. MaFenêtre..Propriété lit ou écrit une propriété de la "
        "fenêtre.",
        "Refers to the window currently running. From a window body, a "
        "control or an event handler, MyWindow points to the parent window — "
        "handy to read or change its properties without repeating its name. "
        "Outside any open window, MyWindow signals an error. "
        "MyWindow..Property reads or writes a property of the window.",
        "PROCEDURE surClic()\n"
        "    MaFenêtre..Titre = \"Modifié\"\n"
        "FIN"
    }
};

/* --- LIBERER ------------------------------------------------------ */
static const OwlKeywordSense SENSES_LIBERER[] = {
    {
        "Libération explicite d'instance",
        "Explicit instance release",
        "Libérer <variable>",
        "Libère explicitement une instance et invoque son destructeur. "
        "Après l'appel, la variable est invalide. Utile pour relâcher "
        "des ressources sans attendre le ramasse-miettes.",
        "Explicitly releases an instance and invokes its destructor. "
        "After the call, the variable becomes invalid. Useful to "
        "release resources without waiting for the garbage collector.",
        "oRes est un cRessource()\n"
        "Libérer oRes"
    }
};

/* --- ET ----------------------------------------------------------- */
static const OwlKeywordSense SENSES_ET[] = {
    {
        "Conjonction logique",
        "Logical AND",
        "<a> ET <b>",
        "Conjonction booléenne. Les deux opérandes sont évalués (pas "
        "de court-circuit). Variante évaluation paresseuse : _ET_. "
        "ET est plus prioritaire que OU.",
        "Boolean conjunction. Both operands are evaluated (no "
        "short-circuit). Lazy-evaluation variant: _ET_. ET binds "
        "tighter than OU.",
        "SI bActif ET PAS bSuspendu ALORS\n"
        "    Trace(\"actif\")\n"
        "FIN"
    }
};

/* --- OU ----------------------------------------------------------- */
static const OwlKeywordSense SENSES_OU[] = {
    {
        "Disjonction logique",
        "Logical OR",
        "<a> OU <b>",
        "Disjonction booléenne. Les deux opérandes sont évalués (pas "
        "de court-circuit). Variante évaluation paresseuse : _OU_. "
        "OU est moins prioritaire que ET.",
        "Boolean disjunction. Both operands are evaluated (no "
        "short-circuit). Lazy-evaluation variant: _OU_. OU binds "
        "less tightly than ET.",
        "SI bActif OU bSuspendu ALORS\n"
        "    Trace(\"au moins un\")\n"
        "FIN"
    }
};

/* --- _ET_ --------------------------------------------------------- */
static const OwlKeywordSense SENSES_ET_SC[] = {
    {
        "Conjonction court-circuit",
        "Short-circuit AND",
        "<a> _ET_ <b>",
        "Variante de ET qui évalue le second opérande uniquement si "
        "le premier est vrai. Indispensable quand l'opérande de "
        "droite peut lever une erreur ou coûter cher.",
        "Variant of ET that evaluates the right operand only if the "
        "left one is true. Required when the right operand may raise "
        "or be costly to compute.",
        "SI s <> \"\" _ET_ Taille(s) > 0 ALORS\n"
        "    Trace(\"non vide\")\n"
        "FIN"
    }
};

/* --- _OU_ --------------------------------------------------------- */
static const OwlKeywordSense SENSES_OU_SC[] = {
    {
        "Disjonction court-circuit",
        "Short-circuit OR",
        "<a> _OU_ <b>",
        "Variante de OU qui évalue le second opérande uniquement si "
        "le premier est faux. Indispensable quand l'opérande de "
        "droite peut lever une erreur ou coûter cher.",
        "Variant of OU that evaluates the right operand only if the "
        "left one is false. Required when the right operand may "
        "raise or be costly to compute.",
        "SI s = \"\" _OU_ Taille(s) > 100 ALORS\n"
        "    Trace(\"hors plage\")\n"
        "FIN"
    }
};

/* --- MODULO ------------------------------------------------------- */
static const OwlKeywordSense SENSES_MODULO[] = {
    {
        "Reste de la division entière (opérateur)",
        "Integer division remainder (operator)",
        "<a> modulo <b>  |  <a> % <b>",
        "Opérateur infix qui retourne le reste de la division entière. "
        "L'opérateur `%` est strictement équivalent.",
        "Infix operator returning the remainder of integer division. "
        "The `%` operator is strictly equivalent.",
        "n1 est un entier = 7 modulo 3"
    }
};

/* --- VRAI --------------------------------------------------------- */
static const OwlKeywordSense SENSES_VRAI[] = {
    {
        "Littéral booléen vrai",
        "True boolean literal",
        "Vrai",
        "Valeur booléenne vraie. Alias EN : True. Couramment utilisé "
        "comme valeur d'initialisation ou dans l'idiome `SELON Vrai` "
        "pour les conditions complexes.",
        "True boolean value. EN alias: True. Common as an init value "
        "or in the `SELON Vrai` idiom for complex conditions.",
        "bActif est un booléen = Vrai"
    }
};

/* --- FAUX --------------------------------------------------------- */
static const OwlKeywordSense SENSES_FAUX[] = {
    {
        "Littéral booléen faux",
        "False boolean literal",
        "Faux",
        "Valeur booléenne fausse. Alias EN : False.",
        "False boolean value. EN alias: False.",
        "bTermine est un booléen = Faux"
    }
};

/* --- NUL ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_NUL[] = {
    {
        "Littéral d'absence de valeur",
        "Null literal",
        "Null",
        "Marqueur d'absence de valeur, distinct de la chaîne vide ou "
        "du zéro. Utilisé pour les types nullables (cf. fondation "
        "02_null_et_nullable).",
        "Marker for an absent value, distinct from the empty string "
        "or zero. Used with nullable types (see foundation "
        "02_null_et_nullable).",
        "valeur = Null"
    }
};

/* --- bitDécaleGauche ---------------------------------------------- */
static const OwlKeywordSense SENSES_BIT_SHL[] = {
    {
        "Décalage binaire à gauche",
        "Bitwise shift left",
        "<a> bitDécaleGauche <n>  |  bitDécaleGauche(<a>, <n>)",
        "Décale les bits de <a> de <n> positions vers la gauche. "
        "Équivaut à multiplier par 2^n pour les entiers positifs. "
        "Les deux formes (opérateur infix et appel de fonction) sont "
        "strictement équivalentes.",
        "Shifts the bits of <a> by <n> positions to the left. "
        "Equivalent to multiplying by 2^n for positive integers. The "
        "two forms (infix operator and function call) are strictly "
        "equivalent.",
        "n1 est un entier = 1 bitDécaleGauche 4"
    }
};

/* --- bitDécaleDroite ---------------------------------------------- */
static const OwlKeywordSense SENSES_BIT_SHR[] = {
    {
        "Décalage binaire à droite",
        "Bitwise shift right",
        "<a> bitDécaleDroite <n>  |  bitDécaleDroite(<a>, <n>)",
        "Décale les bits de <a> de <n> positions vers la droite. "
        "Équivaut à diviser par 2^n pour les entiers positifs. Les "
        "deux formes (opérateur infix et appel de fonction) sont "
        "strictement équivalentes.",
        "Shifts the bits of <a> by <n> positions to the right. "
        "Equivalent to dividing by 2^n for positive integers. The "
        "two forms (infix operator and function call) are strictly "
        "equivalent.",
        "n1 est un entier = 256 bitDécaleDroite 4"
    }
};

/* --- SUR ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_SUR[] = {
    {
        "Spécification de taille octet",
        "Byte-size specification",
        "<type-entier> sur <N> octets",
        "Précise le nombre d'octets de stockage d'un entier. "
        "Utilisé dans les déclarations de structures à layout fixé "
        "(binaire, interopérabilité). N typiquement 1, 2, 4 ou 8.",
        "Specifies the storage size of an integer in bytes. Used in "
        "fixed-layout struct declarations (binary, interop). N "
        "typically 1, 2, 4 or 8.",
        "n est un entier sur 4 octets"
    }
};

/* --- PAR ---------------------------------------------------------- */
static const OwlKeywordSense SENSES_PAR[] = {
    {
        "Particule de liaison",
        "Linking particle",
        "... SEPAREE par <sep>  |  <n> par <m> (dimensions tableau)",
        "Lie une spécification à son contexte. Principal usage : "
        "introduit le séparateur dans `POUR TOUTE chaîne x DE s "
        "SEPAREE par <sep>`. Sert aussi pour les dimensions de "
        "tableau multi-dimensionnel.",
        "Links a specification to its context. Primary use: "
        "introduces the separator in `POUR TOUTE chaîne x DE s "
        "SEPAREE par <sep>`. Also used for multi-dimensional array "
        "dimensions.",
        "POUR TOUTE chaîne s DE \"a,b,c\" SEPAREE par \",\"\n"
        "    Trace(s)\n"
        "FIN"
    }
};

/* --- SEPAREE ------------------------------------------------------ */
static const OwlKeywordSense SENSES_SEPAREE[] = {
    {
        "Marqueur de séparation de chaîne",
        "String separation marker",
        "POUR TOUTE chaîne <var> DE <source> SEPAREE par <séparateur>",
        "Introduit la variante de découpage par séparateur dans une "
        "boucle foreach sur chaîne. Toujours suivi de `par` <séparateur>.",
        "Introduces the separator-split variant in a foreach loop "
        "over a string. Always followed by `par` <separator>.",
        "POUR TOUTE chaîne sCouleur DE \"rouge,vert,bleu\" SEPAREE par \",\"\n"
        "    Trace(sCouleur)\n"
        "FIN"
    }
};

/* --- ELEMENT ------------------------------------------------------ */
static const OwlKeywordSense SENSES_ELEMENT[] = {
    {
        "Particule optionnelle de foreach",
        "Optional foreach particle",
        "POUR TOUT [ELEMENT] <var> DE <itérable> ... FIN",
        "Particule optionnelle de lisibilité dans une boucle "
        "foreach. Sans effet sémantique — `POUR TOUT ELEMENT x DE t` "
        "et `POUR TOUT x DE t` sont strictement équivalents.",
        "Optional readability particle in a foreach loop. No "
        "semantic effect — `POUR TOUT ELEMENT x DE t` and `POUR TOUT "
        "x DE t` are strictly equivalent.",
        "POUR TOUT ELEMENT s DE [\"a\", \"b\"]\n"
        "    Trace(s)\n"
        "FIN"
    }
};

/* --- PROCEDURE ---------------------------------------------------- */
static const OwlKeywordSense SENSES_PROCEDURE[] = {
    {
        "Déclaration de procédure ou méthode",
        "Procedure or method declaration",
        "PROCEDURE <nom>(<params>) [: <retour>] ... FIN",
        "Déclare une procédure ou une méthode de classe. Paramètres "
        "typés, type de retour optionnel via `: <type>`. Une méthode "
        "de classe se déclare directement dans le corps de la classe, "
        "sous la forme `PROCEDURE <nom>(...)` sans préfixe. Synonyme "
        "strict : FONCTION.",
        "Declares a procedure or class method. Typed parameters, "
        "optional return type via `: <type>`. A class method is "
        "declared directly inside the class body, as "
        "`PROCEDURE <name>(...)` without any prefix. Strict synonym: "
        "FONCTION.",
        "PROCEDURE doubler(n est un entier) : entier\n"
        "    RENVOYER n * 2\n"
        "FIN"
    }
};

/* --- FONCTION ----------------------------------------------------- */
static const OwlKeywordSense SENSES_FONCTION[] = {
    {
        "Déclaration de procédure ou méthode (synonyme)",
        "Procedure or method declaration (synonym)",
        "FONCTION <nom>(<params>) [: <retour>] ... FIN",
        "Synonyme strict de PROCEDURE. Convention de style libre : "
        "certains préfèrent FONCTION quand un retour est livré, "
        "PROCEDURE quand le traitement sort sans valeur, mais le "
        "compilateur ne distingue pas.",
        "Strict synonym of PROCEDURE. Style convention is free: "
        "some prefer FONCTION when a return is produced, PROCEDURE "
        "when the action exits without a value, but the compiler "
        "makes no distinction.",
        "FONCTION tripler(n est un entier) : entier\n"
        "    RENVOYER n * 3\n"
        "FIN"
    }
};

/* --- STRUCTURE ---------------------------------------------------- */
static const OwlKeywordSense SENSES_STRUCTURE[] = {
    {
        "Déclaration de structure",
        "Struct declaration",
        "<nom> est une Structure  <membre1> est un <type>  ... FIN",
        "Déclare un type composite agrégeant plusieurs champs typés. "
        "Pas de méthode, juste des données. Accès aux champs via "
        "`<instance>.<champ>`. Pour un type avec comportement, "
        "utiliser une Classe.",
        "Declares a composite type aggregating several typed fields. "
        "No methods, just data. Field access via "
        "`<instance>.<field>`. For a type with behavior, use a "
        "Classe.",
        "stPoint est une Structure\n"
        "    x est un entier\n"
        "    y est un entier\n"
        "FIN"
    }
};

/* --- ENUMERATION -------------------------------------------------- */
static const OwlKeywordSense SENSES_ENUMERATION[] = {
    {
        "Déclaration d'énumération",
        "Enum declaration",
        "<nom> est une Enumeration  <valeur1> = <litéral1>  ... FIN",
        "Déclare un type énuméré : ensemble fini de valeurs nommées. "
        "Chaque valeur est associée à un littéral (souvent entier). "
        "Accès par `<nom>.<valeur>`. Pour des combinaisons de "
        "drapeaux, voir Combinaison.",
        "Declares an enum type: a finite set of named values. Each "
        "value is bound to a literal (often integer). Access via "
        "`<name>.<value>`. For flag combinations, see Combinaison.",
        "eStatut est une Enumeration\n"
        "    actif = 1\n"
        "    suspendu = 2\n"
        "FIN"
    }
};

/* --- COMBINAISON -------------------------------------------------- */
static const OwlKeywordSense SENSES_COMBINAISON[] = {
    {
        "Déclaration d'ensemble de flags combinables",
        "Combinable flag set declaration",
        "<nom> est une Combinaison  <flag1>  <flag2>  ... FIN",
        "Déclare un ensemble de flags qui peuvent coexister dans une "
        "même variable. Les valeurs sont attribuées automatiquement "
        "(1, 2, 4, 8, ...) — inutile de les spécifier. Combiner deux "
        "flags : `+`. Tester la présence : `<combi>[<flag>]` retourne "
        "un booléen. Activer/retirer : `<combi>[<flag>] = Vrai|Faux`. "
        "Valeur entière sous-jacente accessible via `..Valeur`. À la "
        "différence d'une Énumération où la variable porte une seule "
        "valeur, une Combinaison porte un sous-ensemble.",
        "Declares a set of flags that can coexist within a single "
        "variable. Values are auto-assigned (1, 2, 4, 8, ...) — no "
        "need to spell them out. Combine two flags: `+`. Test "
        "presence: `<combi>[<flag>]` returns a boolean. Set/clear: "
        "`<combi>[<flag>] = Vrai|Faux`. Underlying integer value via "
        "`..Valeur`. Unlike an Énumération where the variable holds "
        "one value, a Combinaison holds a subset.",
        "cDroits est une Combinaison\n"
        "    Lecture\n"
        "    Ecriture\n"
        "    Suppression\n"
        "FIN\n"
        "\n"
        "maDroit est un cDroits = cDroits.Lecture + cDroits.Ecriture\n"
        "SI maDroit[cDroits.Lecture] ALORS\n"
        "    Trace(\"a lecture\")\n"
        "FIN"
    }
};

/* --- PAS DANS (composé) ------------------------------------------- */
static const OwlKeywordSense SENSE_PAS_DANS = {
    "Négation d'appartenance",
    "Membership negation",
    "<valeur> PAS DANS (<elem1>, <elem2>, ...)",
    "Teste l'absence d'une valeur dans une liste. Négation de "
    "l'opérateur DANS. La liste doit être non vide.",
    "Tests absence of a value in a list. Negation of the DANS "
    "operator. The list must be non-empty.",
    "SI sCmd PAS DANS (\"SELECT\", \"INSERT\") ALORS\n"
    "    Trace(\"interdit\")\n"
    "FIN"
};

/* ==================================================================
 * Tables — Phase 1 (5 pilotes : SI, SELON, PAS, SORTIR, PAS DANS)
 * ================================================================== */

const OwlKeywordDef OWL_KEYWORD_DEFS[] = {
    { TOKEN_SI,      "Contrôle", SENSES_SI,      1, NULL },
    { TOKEN_ALORS,   "Contrôle", SENSES_ALORS,   1, NULL },
    { TOKEN_SINON,   "Contrôle", SENSES_SINON,   1, NULL },
    { TOKEN_FIN,     "Contrôle", SENSES_FIN,     1, NULL },
    { TOKEN_SELON,   "Contrôle", SENSES_SELON,   1, NULL },
    { TOKEN_CAS,     "Contrôle", SENSES_CAS,     1, NULL },
    { TOKEN_AUTRE,   "Contrôle", SENSES_AUTRE,   1, NULL },
    { TOKEN_PAS,     "Logique",  SENSES_PAS,     2, NULL },
    { TOKEN_PAS_SC,  "Contrôle", SENSES_PAS_SC,  1, NULL },
    { TOKEN_POUR,    "Contrôle", SENSES_POUR,    1, NULL },
    { TOKEN_A,       "Contrôle", SENSES_A,       1, "À"   },
    { TOKEN_A_SC,    "Contrôle", SENSES_A_SC,    1, "_À_" },
    { TOKEN_TOUT,    "Contrôle", SENSES_TOUT,    1, NULL },
    { TOKEN_DE,      "Contrôle", SENSES_DE,      1, NULL },
    { TOKEN_DANS,    "Contrôle", SENSES_DANS,    2, NULL },
    { TOKEN_DANS_SC, "Contrôle", SENSES_DANS_SC, 1, NULL },
    { TOKEN_TANTQUE, "Contrôle", SENSES_TANTQUE, 1, NULL },
    { TOKEN_BOUCLE,  "Contrôle", SENSES_BOUCLE,  1, NULL },
    { TOKEN_FAIRE,   "Contrôle", SENSES_FAIRE,   1, NULL },
    { TOKEN_SORTIR,   "Contrôle", SENSES_SORTIR,   1, NULL },
    { TOKEN_CONTINUER,"Contrôle", SENSES_CONTINUER,1, NULL },
    { TOKEN_RENVOYER, "Contrôle", SENSES_RENVOYER, 1, NULL },
    { TOKEN_RETOUR,    "Contrôle", SENSES_RETOUR,    1, NULL },
    { TOKEN_QUAND,     "Exceptions", SENSES_QUAND,     1, NULL },
    { TOKEN_EXCEPTION, "Exceptions", SENSES_EXCEPTION, 2, NULL },
    { TOKEN_ENFIN,     "Exceptions", SENSES_ENFIN,     1, NULL },
    { TOKEN_EST,       "Déclaration", SENSES_EST,       1, NULL },
    { TOKEN_SONT,      "Déclaration", SENSES_SONT,      1, NULL },
    { TOKEN_CONSTANTE, "Déclaration", SENSES_CONSTANTE, 1, NULL },
    { TOKEN_PUBLIC,    "Visibilité",  SENSES_PUBLIC,    1, NULL },
    { TOKEN_PRIVE,     "Visibilité",  SENSES_PRIVE,     1, NULL },
    { TOKEN_PROTEGE,   "Visibilité",  SENSES_PROTEGE,   1, NULL },
    { TOKEN_GLOBAL,    "Déclaration", SENSES_GLOBAL,    1, NULL },
    { TOKEN_CLASSE,    "OOP",         SENSES_CLASSE,     1, NULL },
    { TOKEN_HERITE,    "OOP",         SENSES_HERITE,     1, NULL },
    { TOKEN_INTERFACE, "OOP",         SENSES_INTERFACE,  1, NULL },
    { TOKEN_IMPLEMENTE,   "OOP", SENSES_IMPLEMENTE,    1, NULL },
    { TOKEN_CONSTRUCTEUR, "OOP", SENSES_CONSTRUCTEUR,  1, NULL },
    { TOKEN_DESTRUCTEUR,  "OOP", SENSES_DESTRUCTEUR,   1, NULL },
    { TOKEN_OBJET,        "OOP", SENSES_OBJET,         1, NULL },
    /* Pas de canon_display ici : token_type_name rend deja la forme exacte
     * ("MoiMême", "MonChamp", "MaFenêtre"). Un override identique serait une
     * seconde source de verite libre de deriver en silence. */
    { TOKEN_MOIMEME,      "UI",  SENSES_MOIMEME,       1, NULL },
    { TOKEN_MONCHAMP,     "UI",  SENSES_MONCHAMP,      1, NULL },
    { TOKEN_MAFENETRE,    "UI",  SENSES_MAFENETRE,     1, NULL },
    { TOKEN_LIBERER,      "OOP", SENSES_LIBERER,       1, NULL },
    { TOKEN_ET,           "Logique", SENSES_ET,        1, NULL },
    { TOKEN_OU,           "Logique", SENSES_OU,        1, NULL },
    { TOKEN_ET_SC,        "Logique", SENSES_ET_SC,     1, NULL },
    { TOKEN_OU_SC,        "Logique", SENSES_OU_SC,     1, NULL },
    { TOKEN_MODULO,       "Logique", SENSES_MODULO,    1, NULL },
    { TOKEN_BIT_SHL,      "Logique", SENSES_BIT_SHL,   1, NULL },
    { TOKEN_BIT_SHR,      "Logique", SENSES_BIT_SHR,   1, NULL },
    { TOKEN_VRAI,         "Constantes", SENSES_VRAI,   1, NULL },
    { TOKEN_FAUX,         "Constantes", SENSES_FAUX,   1, NULL },
    { TOKEN_NUL,          "Constantes", SENSES_NUL,    1, NULL },
    { TOKEN_SUR,          "Déclaration", SENSES_SUR,   1, NULL },
    { TOKEN_PAR,          "Déclaration", SENSES_PAR,   1, NULL },
    { TOKEN_SEPAREE,      "Contrôle", SENSES_SEPAREE,  1, NULL },
    { TOKEN_ELEMENT,      "Contrôle", SENSES_ELEMENT,  1, NULL },
    { TOKEN_PROCEDURE,    "Déclaration", SENSES_PROCEDURE,   1, NULL },
    { TOKEN_FONCTION,     "Déclaration", SENSES_FONCTION,    1, NULL },
    { TOKEN_STRUCTURE,    "Déclaration", SENSES_STRUCTURE,   1, NULL },
    { TOKEN_ENUMERATION,  "Déclaration", SENSES_ENUMERATION, 1, NULL },
    { TOKEN_COMBINAISON,  "Déclaration", SENSES_COMBINAISON, 1, NULL },
    { TOKEN_AVEC,      "Contrôle", SENSES_AVEC,      2, NULL },
    /* Sentinel */
    { TOKEN_EOF, NULL, NULL, 0, NULL }
};

const OwlKeywordComposite OWL_KEYWORD_COMPOSITES[] = {
    { TOKEN_PAS,    TOKEN_DANS,  "Contrôle", &SENSE_PAS_DANS     },
    { TOKEN_POUR,   TOKEN_TOUT,  "Contrôle", &SENSE_POUR_TOUT    },
    { TOKEN_SORTIR, TOKEN_SELON, "Contrôle", &SENSE_SORTIR_SELON },
    /* Sentinel */
    { TOKEN_EOF, TOKEN_EOF, NULL, NULL }
};

/* ==================================================================
 * Lookup
 * ================================================================== */

const OwlKeywordDef* owl_keyword_def_for(TokenType t) {
    for (const OwlKeywordDef* d = OWL_KEYWORD_DEFS; d->senses; d++) {
        if (d->token == t) return d;
    }
    return NULL;
}

const OwlKeywordDef* owl_keyword_find_by_word(const char* word) {
    if (!word || !*word) return NULL;
    /* Itère la table du scanner et compare alias par alias. */
    int wlen = (int)strlen(word);
    char wbuf[64];
    int wn = scanner_strip_accents(word, wlen, wbuf, sizeof wbuf);
    /* lowercase */
    for (int i = 0; i < wn; i++)
        if (wbuf[i] >= 'A' && wbuf[i] <= 'Z') wbuf[i] = (char)(wbuf[i] + 32);

    for (int i = 0; ; i++) {
        TokenType type;
        const char* alias = scanner_keyword_at(i, &type);
        if (!alias) break;
        int alen = (int)strlen(alias);
        char abuf[64];
        int an = scanner_strip_accents(alias, alen, abuf, sizeof abuf);
        for (int j = 0; j < an; j++)
            if (abuf[j] >= 'A' && abuf[j] <= 'Z') abuf[j] = (char)(abuf[j] + 32);
        if (an == wn && memcmp(wbuf, abuf, (size_t)an) == 0) {
            return owl_keyword_def_for(type);
        }
    }
    return NULL;
}

const OwlKeywordComposite* owl_keyword_composite_find(TokenType t1, TokenType t2) {
    for (const OwlKeywordComposite* c = OWL_KEYWORD_COMPOSITES; c->sense; c++) {
        if (c->t1 == t1 && c->t2 == t2) return c;
    }
    return NULL;
}

/* Résout un mot vers son TokenType via le scanner, en passant par la
 * normalisation case+accent. Retourne TOKEN_EOF si pas trouvé. */
static TokenType resolve_word_to_token(const char* word) {
    if (!word || !*word) return TOKEN_EOF;
    int wlen = (int)strlen(word);
    char wbuf[64];
    int wn = scanner_strip_accents(word, wlen, wbuf, sizeof wbuf);
    for (int i = 0; i < wn; i++)
        if (wbuf[i] >= 'A' && wbuf[i] <= 'Z') wbuf[i] = (char)(wbuf[i] + 32);

    for (int i = 0; ; i++) {
        TokenType type;
        const char* alias = scanner_keyword_at(i, &type);
        if (!alias) break;
        int alen = (int)strlen(alias);
        char abuf[64];
        int an = scanner_strip_accents(alias, alen, abuf, sizeof abuf);
        for (int j = 0; j < an; j++)
            if (abuf[j] >= 'A' && abuf[j] <= 'Z') abuf[j] = (char)(abuf[j] + 32);
        if (an == wn && memcmp(wbuf, abuf, (size_t)an) == 0) {
            return type;
        }
    }
    return TOKEN_EOF;
}

const OwlKeywordComposite* owl_keyword_composite_find_by_words(
    const char* w1, const char* w2) {
    TokenType t1 = resolve_word_to_token(w1);
    TokenType t2 = resolve_word_to_token(w2);
    if (t1 == TOKEN_EOF || t2 == TOKEN_EOF) return NULL;
    return owl_keyword_composite_find(t1, t2);
}

/* ==================================================================
 * Itération
 * ================================================================== */

int owl_keyword_count(void) {
    int n = 0;
    for (const OwlKeywordDef* d = OWL_KEYWORD_DEFS; d->senses; d++) n++;
    return n;
}

const OwlKeywordDef* owl_keyword_at(int i) {
    int n = 0;
    for (const OwlKeywordDef* d = OWL_KEYWORD_DEFS; d->senses; d++) {
        if (n == i) return d;
        n++;
    }
    return NULL;
}

int owl_keyword_composite_count(void) {
    int n = 0;
    for (const OwlKeywordComposite* c = OWL_KEYWORD_COMPOSITES; c->sense; c++) n++;
    return n;
}

const OwlKeywordComposite* owl_keyword_composite_at(int i) {
    int n = 0;
    for (const OwlKeywordComposite* c = OWL_KEYWORD_COMPOSITES; c->sense; c++) {
        if (n == i) return c;
        n++;
    }
    return NULL;
}
