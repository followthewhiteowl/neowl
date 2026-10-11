# tests/scenarios/ — doctrine

Les scénarios sont la **vitrine pédagogique** d'OWL : ils alimentent
`owl examples <symbole>`, la documentation CLI et le futur MCP. Un scénario
doit se lire en 30 secondes et montrer un usage idiomatique. Le détail de
rédaction vit dans le skill `.claude/skills/write-scenario/SKILL.md` — ce
fichier fixe seulement les invariants au point d'usage.

## 1. Cinq catégories (liste blanche, règle 53)

Tout est à plat dans cinq dossiers thématiques :

| Dossier | Contenu | Nommage fichier |
|---|---|---|
| `natives/` | une fonction native | `test_<NomNative>.owl` (nom canon, accents strippés) |
| `types/` | un type nommé natif | `test_<NomType>.owl` |
| `language/` | concept langage (invariant, contrôle de flux, opérateurs, déclarations, enums) | `test_<concept>.owl` (snake_case) |
| `ui/` | surface UI (slots, DSL PROPRIETES, champs, fenêtres, events) | `test_<feature>.owl` (snake_case) |
| `poo/` | classe + scénario (isolation par sous-dossier) | `test_<scenario>/{C<Classe>.owl, test_<scenario>.owl}` |

Aucun autre sous-dossier n'est autorisé. Par défaut **1 native = 1 fichier**
dédié (découvrabilité `owl examples`). Groupage transversal seulement si le
sujet ne se réduit pas à une native unique (à documenter dans le ticket).

Le préfixe `test_` est **impératif** : le runner filtre sur `^test_`. Un
fichier sans ce préfixe compile mais n'est jamais exécuté (perte de
couverture silencieuse).

## 2. Nomenclature des procédures — zéro assertion baladée (règle 54)

Toute assertion (`dbgVérifie*` / `dbgCheck*`) DOIT vivre à l'intérieur d'une
procédure-scénario. **Aucune assertion au top-level du fichier.**

- `PROCEDURE PUBLIQUE scenario_<sujet>()` — vitrine, visible dans
  `owl examples`. Cas nominal + 1-2 cas limites, narration claire.
- `PROCEDURE PRIVÉE scenario_edge_<sujet>()` — edge cases / cas internes,
  exécutés mais **invisibles** dans `owl examples`.

Le préfixe `scenario_` est ce qui déclenche la détection par le runner
(les préfixes `test_` / `should_` sont aussi reconnus, mais `scenario_` est
la forme recommandée). Une procédure mal préfixée (`edge_case_*`,
`nonregression_*`, `repro_*`, …) n'est PAS exécutée : ses assertions sont
mortes.

Pourquoi zéro assertion au top-level : le runner exécute **le top-level PUIS
les procédures scénario**. Une assertion posée au top-level compterait donc
deux fois (double exécution, double `Ouvre` pour l'UI). Le top-level ne
contient que : déclarations de globals, directives (`TestModeHeadless(...)`)
et procédures compagnons appelées par nom nu (callbacks, helpers sans
assertion).

Les deux critères de la règle 54 coexistent : au moins une assertion réelle
(non tautologique) ET chaque assertion dans une procédure scénario.

## 3. Métadonnées `@sujet` / `@résumé` — seul lien vers `owl examples`

Les deux premières lignes d'un scénario sont un bloc metadata parsé pour
`owl examples` :

```
// @sujet : <NomCanon>[, <NomCanon>…]
// @résumé : <description ≥ 10 caractères>
```

**C'est l'unique mécanisme de résolution** : `owl examples <symbole>` fait
correspondre le symbole aux `@sujet` déclarés (`sujet_matches` dans
`src/cli_doc.c`), quel que soit le dossier. Il n'existe aucun repli par nom
de fichier. Un scénario sans ce bloc est exécuté par le runner mais reste
invisible dans `owl examples` — le symbole qu'il illustre paraît orphelin.

Un `@sujet` peut lister plusieurs symboles séparés par des virgules, quand
un même scénario en illustre plusieurs indissociables (une paire
ouvrir/fermer, une famille de natives).

`natives/` est le seul dossier **contrôlé** sur ce point : les self-tests
(règles 47/49) n'y scannent que lui, donc un oubli n'y est pas silencieux.
Ailleurs le bloc est tout aussi fonctionnel et tout aussi nécessaire, mais
rien ne rappelle à l'ordre — d'où l'importance de vérifier à la main avec
`owl examples <symbole>` après avoir écrit un scénario hors `natives/`.

Ce bloc ne doit **jamais** apparaître dans la documentation utilisateur.

## 4. Scénario (pédagogie) vs runtime (vérification technique)

`tests/scenarios/` = vitrine pédagogique, vocabulaire utilisateur, code
idiomatique, règle 7 stricte (aucune référence au langage propriétaire
d'origine, à son éditeur ou à ses outils ; aucun jargon implémenteur ;
aucune copie verbatim de documentation externe — cf. CLAUDE.md règle 7).

Un verrouillage purement technique (cas limites exotiques, transitions
internes, valeurs extrêmes) n'a pas sa place ici : il va dans
`tests/runtime/` (ou `tests/regression/` pour un anti-bug historique), où la
narration n'est pas requise.

## 5. Verrous automatiques

Ces invariants sont vérifiés par `owl --self-test-registry` (lancé par
`make`) :

- règle 39 — nom de fichier `test_*.owl` et catégorie cohérente ;
- règle 47 — `@sujet` résout vers une native du registry (`natives/`) ;
- règle 49 — bloc metadata présent et valide (`natives/`) ;
- règle 53 — sous-dossier dans la liste blanche des cinq catégories ;
- règle 54 — au moins une assertion réelle ET aucune assertion baladée au
  top-level du fichier (toute assertion dans une `PROCEDURE`). La
  nomenclature `scenario_*` (section 2) reste la convention recommandée,
  non durcie par le self-test.
