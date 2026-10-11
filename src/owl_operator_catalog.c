// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_operator_catalog.c — implémentation du catalogue opérateurs.
 *
 * Pour la documentation architecturale, voir owl_operator_catalog.h.
 *
 * Tous les opérateurs user-facing du périmètre V1 sont documentés.
 * Rule 32 (couverture montante) est en strict absolu : ajouter un
 * opérateur à is_operator_user_facing sans rédiger sa fiche fait
 * immédiatement échouer le self-test registry.
 */

#include "owl_operator_catalog.h"
#include <string.h>

/* ==================================================================
 * Périmètre user-facing — liste blanche des TokenType d'opérateurs
 * qui doivent avoir une fiche pédagogique.
 *
 * Bloc 1 (à faire d'abord) : comparateurs chaîne fixes — 9 opérateurs.
 * Bloc 2 : null-safe — 4 opérateurs.
 * Bloc 3 : égalité souple — 2 opérateurs.
 * Bloc 4 : affectation par référence — 2 opérateurs (REFERENCE + COPY).
 *
 * Hors périmètre (au moins V1) :
 *  - Opérateurs arithmétiques classiques (+, -, *, /, ^, %) : déjà
 *    documentés en fondation 05_operateurs, auto-évidents.
 *  - Comparaison classique (=, <>, <, <=, >, >=).
 *  - Affectation composée (+=, -=, *=, /=, %=).
 *  - Incrément (++, --).
 *  - Bit-à-bit classique (&, |, ||, ~) : sauf cas combinés keyword.
 *  - Accès membres (., .., [], [[]]).
 * ================================================================== */
bool is_operator_user_facing(TokenType t) {
    switch (t) {
        /* --- Bloc 1 : comparateurs chaîne fixes (9) --- */
        case TOKEN_STARTS_WITH:        /* [= */
        case TOKEN_ENDS_WITH:          /* =] */
        case TOKEN_CONTAINS:           /* [=] */
        case TOKEN_STARTS_WITH_FLEX:   /* [~ */
        case TOKEN_ENDS_WITH_FLEX:     /* ~] */
        case TOKEN_CONTAINS_FLEX:      /* [~] */
        case TOKEN_STARTS_WITH_VFLEX:  /* [~~ */
        case TOKEN_ENDS_WITH_VFLEX:    /* ~~] */
        case TOKEN_CONTAINS_VFLEX:     /* [~~] */
        /* --- Bloc 2 : null-safe (4) --- */
        case TOKEN_QUESTION_DOT:       /* ?. */
        case TOKEN_QUESTION_QUESTION:  /* ?? */
        case TOKEN_QUESTION_EQUAL:     /* ??= */
        case TOKEN_QUESTION_STAR:      /* ??* */
        /* --- Bloc 3 : égalité souple (2) --- */
        case TOKEN_TILDE_EQUAL:        /* ~= */
        case TOKEN_DOUBLE_TILDE:       /* ~~ */
        /* --- Bloc 4 : référence / copie (2) --- */
        case TOKEN_REFERENCE:          /* <- */
        case TOKEN_COPY:               /* <= force copy (ambigu LESS_EQUAL) */
        /* --- Bloc 5 : arithmétique de base (6) --- */
        case TOKEN_PLUS:               /* + (addition / concaténation) */
        case TOKEN_MINUS:              /* - (soustraction / moins unaire) */
        case TOKEN_STAR:               /* * */
        case TOKEN_SLASH:              /* / */
        case TOKEN_CARET:              /* ^ puissance */
        case TOKEN_PERCENT:            /* % modulo */
        /* --- Bloc 6 : comparaison de base (4) --- */
        case TOKEN_LESS:               /* < */
        case TOKEN_GREATER:            /* > */
        case TOKEN_GREATER_EQUAL:      /* >= */
        case TOKEN_NOT_EQUAL:          /* <> */
            /* `<=` n'est PAS ici : déjà couvert par TOKEN_COPY (sens copie +
             * sens comparaison ≤). `=` n'est pas couvert : double sens
             * affectation/comparaison désambiguïsé par le contexte
             * syntaxique, hors périmètre de ce catalogue. */
            return true;
        default:
            return false;
    }
}

/* ==================================================================
 * Fiches pédagogiques
 *
 * Réutilise OwlKeywordSense. Une fiche = une OwlOperatorDef avec son
 * tableau de senses. Les exemples sont validés via le runtime au
 * moment de la rédaction.
 * ================================================================== */

/* --- Bloc 1 : comparateurs chaîne fixes -------------------------- */

static const OwlKeywordSense SENSE_STARTS_WITH[] = {
    {
        "Commence par (strict)",
        "Starts with (strict)",
        "<chaîne> [= <préfixe>",
        "Teste si <chaîne> commence par <préfixe>, comparaison "
        "byte-à-byte stricte (casse et accents distinctifs). Variantes "
        "souples : `[~` (casse insensible), `[~~` (casse + accent "
        "insensibles).",
        "Tests whether <string> starts with <prefix>, strict "
        "byte-by-byte comparison (case and accents distinguishing). "
        "Looser variants: `[~` (case-insensitive), `[~~` (case + "
        "accent insensitive).",
        "SI sCode [= \"FR-\" ALORS\n"
        "    Trace(\"code français\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_STARTS_WITH_FLEX[] = {
    {
        "Commence par (casse insensible)",
        "Starts with (case insensitive)",
        "<chaîne> [~ <préfixe>",
        "Teste si <chaîne> commence par <préfixe> en ignorant la "
        "casse. Les accents restent distinctifs : `\"Héllo\" [~ "
        "\"hel\"` est faux (le `é` ne matche pas `e`). Pour ignorer "
        "aussi les accents, utiliser `[~~`.",
        "Tests whether <string> starts with <prefix> ignoring case. "
        "Accents stay distinguishing: `\"Héllo\" [~ \"hel\"` is "
        "false. To also ignore accents, use `[~~`.",
        "SI sCmd [~ \"select\" ALORS\n"
        "    Trace(\"requête de lecture\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_STARTS_WITH_VFLEX[] = {
    {
        "Commence par (casse + accent insensibles)",
        "Starts with (case + accent insensitive)",
        "<chaîne> [~~ <préfixe>",
        "Teste si <chaîne> commence par <préfixe> en ignorant à la "
        "fois la casse ET les accents. `\"Héllo\" [~~ \"hello\"` est "
        "vrai. La version la plus tolérante pour les comparaisons "
        "d'entrée utilisateur.",
        "Tests whether <string> starts with <prefix> ignoring both "
        "case AND accents. `\"Héllo\" [~~ \"hello\"` is true. The "
        "most lenient variant for user-input comparisons.",
        "SI sNom [~~ \"Élève\" ALORS\n"
        "    Trace(\"entrée élève\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_ENDS_WITH[] = {
    {
        "Termine par (strict)",
        "Ends with (strict)",
        "<chaîne> =] <suffixe>",
        "Teste si <chaîne> finit par <suffixe>, comparaison "
        "byte-à-byte stricte. Variantes souples : `~]` (casse "
        "insensible), `~~]` (casse + accent insensibles).",
        "Tests whether <string> ends with <suffix>, strict "
        "byte-by-byte comparison. Looser variants: `~]` "
        "(case-insensitive), `~~]` (case + accent insensitive).",
        "SI sFichier =] \".owl\" ALORS\n"
        "    Trace(\"fichier OWL\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_ENDS_WITH_FLEX[] = {
    {
        "Termine par (casse insensible)",
        "Ends with (case insensitive)",
        "<chaîne> ~] <suffixe>",
        "Teste si <chaîne> finit par <suffixe> en ignorant la casse. "
        "Les accents restent distinctifs. Pour ignorer aussi les "
        "accents, utiliser `~~]`.",
        "Tests whether <string> ends with <suffix> ignoring case. "
        "Accents stay distinguishing. To also ignore accents, use "
        "`~~]`.",
        "SI sFichier ~] \".OWL\" ALORS\n"
        "    Trace(\"extension owl quelconque casse\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_ENDS_WITH_VFLEX[] = {
    {
        "Termine par (casse + accent insensibles)",
        "Ends with (case + accent insensitive)",
        "<chaîne> ~~] <suffixe>",
        "Teste si <chaîne> finit par <suffixe> en ignorant à la fois "
        "la casse ET les accents.",
        "Tests whether <string> ends with <suffix> ignoring both "
        "case AND accents.",
        "SI sTitre ~~] \"épisode\" ALORS\n"
        "    Trace(\"finit par episode\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_CONTAINS[] = {
    {
        "Contient (strict)",
        "Contains (strict)",
        "<chaîne> [=] <sous-chaîne>",
        "Teste si <chaîne> contient <sous-chaîne>, comparaison "
        "byte-à-byte stricte. Variantes souples : `[~]` (casse "
        "insensible), `[~~]` (casse + accent insensibles).",
        "Tests whether <string> contains <substring>, strict "
        "byte-by-byte comparison. Looser variants: `[~]` "
        "(case-insensitive), `[~~]` (case + accent insensitive).",
        "SI sLog [=] \"ERROR\" ALORS\n"
        "    Trace(\"erreur détectée\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_CONTAINS_FLEX[] = {
    {
        "Contient (casse insensible)",
        "Contains (case insensitive)",
        "<chaîne> [~] <sous-chaîne>",
        "Teste si <chaîne> contient <sous-chaîne> en ignorant la "
        "casse. Les accents restent distinctifs.",
        "Tests whether <string> contains <substring> ignoring case. "
        "Accents stay distinguishing.",
        "SI sMsg [~] \"warning\" ALORS\n"
        "    Trace(\"alerte\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_CONTAINS_VFLEX[] = {
    {
        "Contient (casse + accent insensibles)",
        "Contains (case + accent insensitive)",
        "<chaîne> [~~] <sous-chaîne>",
        "Teste si <chaîne> contient <sous-chaîne> en ignorant à la "
        "fois la casse ET les accents. La version la plus tolérante "
        "pour la recherche dans du texte utilisateur libre.",
        "Tests whether <string> contains <substring> ignoring both "
        "case AND accents. The most lenient variant for searching "
        "free user text.",
        "SI sTexte [~~] \"épopée\" ALORS\n"
        "    Trace(\"mentionne épopée\")\n"
        "FIN"
    }
};

/* --- Bloc 2 : opérateurs null-safe ------------------------------- */

static const OwlKeywordSense SENSE_QUESTION_QUESTION[] = {
    {
        "Valeur de repli",
        "Coalesce",
        "<valeur> ?? <repli>",
        "Retourne la valeur de gauche si elle n'est pas Null, sinon "
        "la valeur de droite. Chainable : `a ?? b ?? c` prend le "
        "premier non-Null. La valeur de droite n'est évaluée que si "
        "la gauche est Null (court-circuit).",
        "Returns the left value if it is not Null, otherwise the "
        "right value. Chainable: `a ?? b ?? c` takes the first "
        "non-Null. The right value is evaluated only if the left is "
        "Null (short-circuit).",
        "nResultat est un entier = nCompteur ?? 0"
    }
};

static const OwlKeywordSense SENSE_QUESTION_EQUAL[] = {
    {
        "Affectation conditionnée sur Null",
        "Null-coalescing assignment",
        "<variable> ?\?= <valeur>",
        "Affecte la valeur à la variable uniquement si elle est "
        "actuellement Null. Sinon, aucune modification. Équivalent à "
        "`SI EstNul(<variable>) ALORS <variable> = <valeur> FIN`.",
        "Assigns the value to the variable only if it is currently "
        "Null. Otherwise, no change. Equivalent to `SI EstNul(<var>) "
        "ALORS <var> = <value> FIN`.",
        "sLog est une chaîne? = Null\n"
        "sLog ?\?= \"init\""
    }
};

static const OwlKeywordSense SENSE_QUESTION_DOT[] = {
    {
        "Accès sécurisé à un membre",
        "Safe navigation",
        "<objet>?.<membre>",
        "Accède au membre si l'objet n'est pas Null, sinon retourne "
        "Null sans lever d'erreur. Chainable : `oA?.oB?.champ` "
        "court-circuite dès le premier Null rencontré. Évite les "
        "gardes EstNul() à chaque niveau d'accès.",
        "Accesses the member if the object is not Null, otherwise "
        "returns Null without raising. Chainable: `oA?.oB?.field` "
        "short-circuits at the first Null. Avoids EstNul() guards at "
        "each access level.",
        "p est un Point\n"
        "p.px = 10\n"
        "Trace(p?.px)"
    }
};

static const OwlKeywordSense SENSE_QUESTION_STAR[] = {
    {
        "Repli sur la valeur par défaut du type",
        "Coalesce to type default",
        "<valeur-nullable> ??*",
        "Retourne la valeur si elle n'est pas Null, sinon la valeur "
        "par défaut du type cible : 0 pour les types numériques, "
        "\"\" pour chaîne, Faux pour booléen, etc. Pratique quand le "
        "repli est exactement la zero-value canon du type.",
        "Returns the value if not Null, otherwise the type's default "
        "value: 0 for numeric types, \"\" for string, False for "
        "boolean, etc. Convenient when the fallback is exactly the "
        "type's canonical zero-value.",
        "nValeur est un entier? = Null\n"
        "Trace(nValeur ??*)"
    }
};

/* --- Bloc 3 : égalité souple ------------------------------------- */

static const OwlKeywordSense SENSE_TILDE_EQUAL[] = {
    {
        "Égalité casse insensible",
        "Case-insensitive equality",
        "<chaîne-a> ~= <chaîne-b>",
        "Compare deux chaînes pour égalité en ignorant la casse. "
        "Les accents restent distinctifs (`\"héllo\" ~= \"Hello\"` "
        "est Faux car `é` ne matche pas `e`). Pour ignorer aussi "
        "les accents, utiliser `~~`. C'est une égalité stricte sur "
        "le contenu, pas une comparaison positionnelle — voir bloc "
        "commence par / termine par / contient pour celles-ci.",
        "Compares two strings for equality ignoring case. Accents "
        "stay distinguishing (`\"héllo\" ~= \"Hello\"` is False as "
        "`é` does not match `e`). To also ignore accents, use `~~`. "
        "This is strict content equality, not positional comparison "
        "— see starts-with / ends-with / contains for those.",
        "SI sCmd ~= \"SELECT\" ALORS\n"
        "    Trace(\"SELECT quelconque casse\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_DOUBLE_TILDE[] = {
    {
        "Égalité casse + accent insensible",
        "Case + accent insensitive equality",
        "<chaîne-a> ~~ <chaîne-b>",
        "Compare deux chaînes pour égalité en ignorant à la fois la "
        "casse ET les accents. `\"héllo\" ~~ \"Hello\"` est Vrai. "
        "La version la plus tolérante pour comparer des entrées "
        "utilisateur où la saisie d'accents est inconsistante.",
        "Compares two strings for equality ignoring both case AND "
        "accents. `\"héllo\" ~~ \"Hello\"` is True. The most lenient "
        "variant for comparing user input where accent typing is "
        "inconsistent.",
        "SI sSaisie ~~ \"élève\" ALORS\n"
        "    Trace(\"reconnu\")\n"
        "FIN"
    }
};

/* --- Bloc 4 : référence / copie ---------------------------------- */

static const OwlKeywordSense SENSE_REFERENCE[] = {
    {
        "Affectation par référence (non nécessaire en OWL)",
        "Reference assignment (not needed in OWL)",
        "<variable> <- <expression>  (rejeté à la compilation)",
        "Opérateur historique d'autres langages pour forcer une "
        "affectation par référence. En OWL, `=` sur un objet livre "
        "déjà une référence — l'opérateur `<-` est explicitement "
        "rejeté à la compilation avec un message d'orientation. Pour "
        "forcer une copie profonde, utiliser `<=`.",
        "Historic operator from other languages to force a reference "
        "assignment. In OWL, `=` on an object already gives a "
        "reference — the `<-` operator is explicitly rejected at "
        "compile time with a guidance message. To force a deep copy, "
        "use `<=`.",
        "// Forme rejetee :\n"
        "// oRef <- oSource\n"
        "// → erreur compile : utiliser '=' (ref) ou '<=' (copie)\n"
        "oRef est un Point\n"
        "oRef = oSource    // assignation par reference (canon OWL)"
    }
};

static const OwlKeywordSense SENSES_COPY[] = {
    {
        "Affectation par copie profonde",
        "Deep copy assignment",
        "<variable> <= <expression>",
        "En contexte d'affectation, force une copie profonde de "
        "l'expression vers la variable. La variable devient alors "
        "indépendante de la source — modifier l'une n'affecte plus "
        "l'autre. Sans `<=`, l'affectation `=` sur un objet livre "
        "une référence.",
        "In assignment context, forces a deep copy of the expression "
        "into the variable. The variable then becomes independent of "
        "the source — modifying one no longer affects the other. "
        "Without `<=`, `=` assignment on an object gives a reference.",
        "oOriginal est un Point\n"
        "oOriginal.px = 10\n"
        "oCopie est un Point\n"
        "oCopie <= oOriginal    // copie profonde, oCopie independant"
    },
    {
        "Comparaison inférieur ou égal",
        "Less than or equal comparison",
        "<a> <= <b>",
        "En contexte d'expression (condition de SI, expression de "
        "CAS, etc.), `<=` est l'opérateur de comparaison classique : "
        "Vrai si la valeur de gauche est inférieure ou égale à celle "
        "de droite. Le contexte syntaxique (affectation vs "
        "expression) suffit à désambiguïser avec le sens copie.",
        "In expression context (SI condition, CAS expression, etc.), "
        "`<=` is the classic comparison operator: true if the left "
        "value is less than or equal to the right. The syntactic "
        "context (assignment vs expression) is enough to "
        "disambiguate from the copy meaning.",
        "SI nAge <= 17 ALORS\n"
        "    Trace(\"mineur\")\n"
        "FIN"
    }
};

/* --- Bloc 5 : arithmétique de base -------------------------------- */

/* `+` porte deux sens : addition numérique et concaténation dès qu'un
 * opérande est une chaîne (cf. 05_operateurs §1 et §4). */
static const OwlKeywordSense SENSES_PLUS[] = {
    {
        "Addition",
        "Addition",
        "<a> + <b>",
        "Additionne deux nombres. Un débordement d'entier promeut "
        "automatiquement vers numérique ; un mélange avec un réel "
        "produit un réel.",
        "Adds two numbers. An integer overflow is automatically "
        "promoted to numeric; mixing with a real produces a real.",
        "Trace(7 + 3)        // 10"
    },
    {
        "Concaténation",
        "Concatenation",
        "<chaîne> + <valeur>",
        "Dès qu'au moins un opérande est une chaîne, `+` concatène. Le "
        "résultat est toujours une chaîne, l'autre opérande étant "
        "converti en texte.",
        "As soon as at least one operand is a string, `+` concatenates. "
        "The result is always a string, the other operand being "
        "converted to text.",
        "Trace(\"score : \" + 99)   // \"score : 99\""
    }
};

/* `-` porte deux sens : soustraction binaire et moins unaire. Même
 * TokenType (TOKEN_MINUS) dans les deux cas. */
static const OwlKeywordSense SENSES_MINUS[] = {
    {
        "Soustraction",
        "Subtraction",
        "<a> - <b>",
        "Soustrait le second nombre du premier. Mêmes promotions "
        "automatiques que l'addition (entier vers numérique, mélange "
        "avec réel donne un réel).",
        "Subtracts the second number from the first. Same automatic "
        "promotions as addition (integer to numeric, mixing with real "
        "gives a real).",
        "Trace(7 - 3)        // 4"
    },
    {
        "Moins unaire",
        "Unary minus",
        "-<expression>",
        "Devant une seule expression, `-` en inverse le signe. Il se "
        "lie plus fort que la puissance : `-2 ^ 2` vaut `4` (lu "
        "`(-2)^2`). En cas de doute, parenthéser.",
        "In front of a single expression, `-` negates it. It binds "
        "tighter than power: `-2 ^ 2` is `4` (read `(-2)^2`). When in "
        "doubt, parenthesise.",
        "x est un entier = 5\n"
        "Trace(-x)           // -5"
    }
};

static const OwlKeywordSense SENSE_STAR[] = {
    {
        "Multiplication",
        "Multiplication",
        "<a> * <b>",
        "Multiplie deux nombres. Mêmes promotions automatiques que les "
        "autres opérations arithmétiques.",
        "Multiplies two numbers. Same automatic promotions as the other "
        "arithmetic operations.",
        "Trace(7 * 3)        // 21"
    }
};

static const OwlKeywordSense SENSE_SLASH[] = {
    {
        "Division",
        "Division",
        "<a> / <b>",
        "Divise le premier nombre par le second. La division est "
        "toujours réelle, jamais entière : `7 / 3` donne `2.333…`. Pour "
        "le reste, voir `%`.",
        "Divides the first number by the second. Division is always "
        "real, never integer: `7 / 3` gives `2.333…`. For the "
        "remainder, see `%`.",
        "Trace(7 / 3)        // 2.333…"
    }
};

static const OwlKeywordSense SENSE_CARET[] = {
    {
        "Puissance",
        "Power",
        "<base> ^ <exposant>",
        "Élève la base à la puissance de l'exposant. Associativité à "
        "gauche : `2 ^ 3 ^ 2` vaut `64` (lu `(2^3)^2`). Le moins unaire "
        "se lie plus fort que `^`.",
        "Raises the base to the power of the exponent. Left "
        "associativity: `2 ^ 3 ^ 2` is `64` (read `(2^3)^2`). Unary "
        "minus binds tighter than `^`.",
        "Trace(2 ^ 3 ^ 2)   // 64"
    }
};

static const OwlKeywordSense SENSE_PERCENT[] = {
    {
        "Modulo",
        "Modulo",
        "<a> % <b>",
        "Reste de la division entière. La forme nommée `modulo` est "
        "équivalente : `7 % 3` et `7 modulo 3` donnent tous deux `1`.",
        "Remainder of the integer division. The named form `modulo` is "
        "equivalent: `7 % 3` and `7 modulo 3` both give `1`.",
        "Trace(7 % 3)        // 1"
    }
};

/* --- Bloc 6 : comparaison de base --------------------------------- */

static const OwlKeywordSense SENSE_LESS[] = {
    {
        "Strictement inférieur",
        "Strictly less than",
        "<a> < <b>",
        "Renvoie Vrai si la valeur de gauche est strictement inférieure "
        "à celle de droite, Faux sinon.",
        "Returns true if the left value is strictly less than the right "
        "one, false otherwise.",
        "SI nAge < 18 ALORS\n"
        "    Trace(\"mineur\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_GREATER[] = {
    {
        "Strictement supérieur",
        "Strictly greater than",
        "<a> > <b>",
        "Renvoie Vrai si la valeur de gauche est strictement supérieure "
        "à celle de droite, Faux sinon.",
        "Returns true if the left value is strictly greater than the "
        "right one, false otherwise.",
        "SI nSolde > 0 ALORS\n"
        "    Trace(\"créditeur\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_GREATER_EQUAL[] = {
    {
        "Supérieur ou égal",
        "Greater than or equal",
        "<a> >= <b>",
        "Renvoie Vrai si la valeur de gauche est supérieure ou égale à "
        "celle de droite. Le pendant inférieur ou égal est `<=`.",
        "Returns true if the left value is greater than or equal to the "
        "right one. The less-or-equal counterpart is `<=`.",
        "SI nAge >= 18 ALORS\n"
        "    Trace(\"majeur\")\n"
        "FIN"
    }
};

static const OwlKeywordSense SENSE_NOT_EQUAL[] = {
    {
        "Différent",
        "Not equal",
        "<a> <> <b>",
        "Renvoie Vrai si les deux valeurs sont différentes. C'est le "
        "seul opérateur d'inégalité : `!=` n'est pas reconnu. Le "
        "résultat est un booléen.",
        "Returns true if the two values differ. It is the only "
        "inequality operator: `!=` is not recognised. The result is a "
        "boolean.",
        "SI sStatut <> \"actif\" ALORS\n"
        "    Trace(\"inactif\")\n"
        "FIN"
    }
};

/* ==================================================================
 * Tables — Bloc 1 (9 comparateurs chaîne fixes) + Bloc 2 (4 null-safe)
 *          + Bloc 3 (2 égalité souple) + Bloc 4 (2 référence/copie)
 *          + Bloc 5 (6 arithmétique base) + Bloc 6 (4 comparaison base)
 * ================================================================== */

/* Sentinel : senses=NULL marque la fin. */
const OwlOperatorDef OWL_OPERATOR_DEFS[] = {
    { TOKEN_STARTS_WITH,       "[=",  "Comparaison chaîne",
      SENSE_STARTS_WITH,       1 },
    { TOKEN_STARTS_WITH_FLEX,  "[~",  "Comparaison chaîne",
      SENSE_STARTS_WITH_FLEX,  1 },
    { TOKEN_STARTS_WITH_VFLEX, "[~~", "Comparaison chaîne",
      SENSE_STARTS_WITH_VFLEX, 1 },
    { TOKEN_ENDS_WITH,         "=]",  "Comparaison chaîne",
      SENSE_ENDS_WITH,         1 },
    { TOKEN_ENDS_WITH_FLEX,    "~]",  "Comparaison chaîne",
      SENSE_ENDS_WITH_FLEX,    1 },
    { TOKEN_ENDS_WITH_VFLEX,   "~~]", "Comparaison chaîne",
      SENSE_ENDS_WITH_VFLEX,   1 },
    { TOKEN_CONTAINS,          "[=]", "Comparaison chaîne",
      SENSE_CONTAINS,          1 },
    { TOKEN_CONTAINS_FLEX,     "[~]", "Comparaison chaîne",
      SENSE_CONTAINS_FLEX,     1 },
    { TOKEN_CONTAINS_VFLEX,    "[~~]","Comparaison chaîne",
      SENSE_CONTAINS_VFLEX,    1 },
    /* Bloc 2 — null-safe */
    { TOKEN_QUESTION_QUESTION, "??",  "Null-safe",
      SENSE_QUESTION_QUESTION, 1 },
    { TOKEN_QUESTION_EQUAL,    "?\?=", "Null-safe",
      SENSE_QUESTION_EQUAL,    1 },
    { TOKEN_QUESTION_DOT,      "?.",  "Null-safe",
      SENSE_QUESTION_DOT,      1 },
    { TOKEN_QUESTION_STAR,     "??*", "Null-safe",
      SENSE_QUESTION_STAR,     1 },
    /* Bloc 3 — égalité souple */
    { TOKEN_TILDE_EQUAL,       "~=",  "Égalité souple",
      SENSE_TILDE_EQUAL,       1 },
    { TOKEN_DOUBLE_TILDE,      "~~",  "Égalité souple",
      SENSE_DOUBLE_TILDE,      1 },
    /* Bloc 4 — référence / copie */
    { TOKEN_REFERENCE,         "<-",  "Affectation",
      SENSE_REFERENCE,         1 },
    { TOKEN_COPY,              "<=",  "Affectation",
      SENSES_COPY,             2 },
    /* Bloc 5 — arithmétique de base */
    { TOKEN_PLUS,              "+",   "Arithmétique",
      SENSES_PLUS,             2 },
    { TOKEN_MINUS,             "-",   "Arithmétique",
      SENSES_MINUS,            2 },
    { TOKEN_STAR,              "*",   "Arithmétique",
      SENSE_STAR,              1 },
    { TOKEN_SLASH,             "/",   "Arithmétique",
      SENSE_SLASH,             1 },
    { TOKEN_CARET,             "^",   "Arithmétique",
      SENSE_CARET,             1 },
    { TOKEN_PERCENT,           "%",   "Arithmétique",
      SENSE_PERCENT,           1 },
    /* Bloc 6 — comparaison de base */
    { TOKEN_LESS,              "<",   "Comparaison",
      SENSE_LESS,              1 },
    { TOKEN_GREATER,           ">",   "Comparaison",
      SENSE_GREATER,           1 },
    { TOKEN_GREATER_EQUAL,     ">=",  "Comparaison",
      SENSE_GREATER_EQUAL,     1 },
    { TOKEN_NOT_EQUAL,         "<>",  "Comparaison",
      SENSE_NOT_EQUAL,         1 },
    /* Sentinel */
    { TOKEN_EOF, NULL, NULL, NULL, 0 }
};

/* ==================================================================
 * Lookup
 * ================================================================== */

const OwlOperatorDef* owl_operator_def_for(TokenType t) {
    for (const OwlOperatorDef* d = OWL_OPERATOR_DEFS; d->senses; d++) {
        if (d->token == t) return d;
    }
    return NULL;
}

const OwlOperatorDef* owl_operator_find_by_symbol(const char* symbol) {
    if (!symbol || !*symbol) return NULL;
    for (const OwlOperatorDef* d = OWL_OPERATOR_DEFS; d->senses; d++) {
        if (d->display_form && strcmp(d->display_form, symbol) == 0) return d;
    }
    return NULL;
}

/* ==================================================================
 * Itération
 * ================================================================== */

int owl_operator_count(void) {
    int n = 0;
    for (const OwlOperatorDef* d = OWL_OPERATOR_DEFS; d->senses; d++) n++;
    return n;
}

const OwlOperatorDef* owl_operator_at(int i) {
    int n = 0;
    for (const OwlOperatorDef* d = OWL_OPERATOR_DEFS; d->senses; d++) {
        if (n == i) return d;
        n++;
    }
    return NULL;
}
