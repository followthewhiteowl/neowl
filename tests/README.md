# OWL Test Suite

Document interne (devs + agents). Décrit le layout réel de `tests/` et les
conventions verrouillées par les self-tests registry. Le **bilan de couverture
vivant** n'est pas figé ici — il est produit par `owl test` (nombre de suites,
d'assertions, PASS/FAIL à jour).

## Lancer les tests

```bash
./build/owl.exe test                      # toute la suite (walk récursif)
./build/owl.exe test -v                    # verbeux (résultat par assertion)
./build/owl.exe test tests/runtime/foo.owl # un seul fichier
./build/owl.exe test tests/scenarios/natives/ -vv  # un sous-dossier
```

`owl test` walk récursivement pour **trouver** les tests, mais chaque fichier
s'exécute dans le contexte de sa propre frontière de projet (pas un méga-projet
global). Cf. « Modèle de projet » dans `CLAUDE.md`.

## Layout

```
tests/
  scenarios/          scénarios PUB pédagogiques (alimentent `owl examples`)
    natives/          1 native par fichier + métadonnées @sujet/@résumé
    types/            types, enums, coercions
    language/         constructions du langage (SI, POUR, SELON, exceptions…)
    ui/               fenêtres, champs, états, événements (souvent multifile)
    poo/              classes, héritage, interfaces, destructeurs
  runtime/            verrouillages techniques (comportement runtime exact)
  regression/         anti-bug : reproduit un bug corrigé, empêche la rechute
  compile-error/      fixtures d'erreurs compilo + snapshots `.expected.json`
  conformance/        conformité au comportement de référence (valeurs vérifiées)
  lsp/                tests du Language Server
  dap/                tests du Debug Adapter Protocol
  compiler/           tests bas-niveau du compilo (bytecode, parse)
  internal_fixtures/  fixtures consommées par les self-tests C (registre)
```

Les 5 sous-dossiers de `scenarios/` sont une **liste blanche fermée**
(`natives`, `types`, `language`, `ui`, `poo`) — verrouillée par le self-test
`scenarios_subdir_whitelist`. Créer un 6e sous-dossier hors-liste = violation.

### scenarios/ vs runtime/ — la distinction qui compte

- **`scenarios/`** = **pédagogie**. Un scénario montre comment se sert d'une
  native / d'un type / d'une construction, façon exemple lisible. Il est
  moissonné par `owl examples <NomNative>`. Ce n'est **pas** l'endroit pour
  empiler des cas de vérification technique.
- **`runtime/`** = **vérification**. Comportement exact, cas limites, valeurs
  de bord, coercions tordues, edge cases. C'est là que vivent les batteries
  d'assertions serrées.

Un scénario qui n'est qu'une pile d'assertions nues (sans valeur pédagogique)
n'a rien à faire dans `scenarios/` : il appartient à `runtime/`.

## Convention scénario (doctrine : skill `write-scenario`)

La doctrine complète (structure, pièges, mock UI, mode GUI réel) vit dans le
skill `write-scenario` et la capsule `test-writing-pitfalls`. Points clés
verrouillés par les self-tests :

- **Préfixe `test_` impératif** : tout fichier de `scenarios/` (récursif) doit
  être un `test_*.owl` (règle `test_scenario_filename_convention`). Les
  compagnons multifile — `FEN_*`, `ETAT_*`, `cXxx`, `iXxx`, type PascalCase —
  sont tolérés à côté d'un `test_*.owl` pilote.
- **1 native = 1 fichier** dans `scenarios/natives/`, nommé d'après la native.
- **Procédures `scenario_*`** : chaque cas est une procédure `scenario_<...>`
  (pas de top-level fourre-tout).
- **Métadonnées `@sujet` / `@résumé`** (dans `natives/` uniquement) : lignes 1
  et 2 obligatoires, `// @sujet : <NomCanon>` (résolvable dans le registre) +
  `// @résumé : <phrase>`. Elles alimentent `owl examples`. Règles
  `scenario_pub_metadata_required` + `test_scenario_sujet_canonical`.
- **Au moins une assertion réelle** : un scénario doit vérifier quelque chose
  de concret. Une entrée dont toutes les assertions sont tautologiques
  (`dbgVérifieVrai(Vrai, …)` / `dbgCheckTrue(True, …)`), ou sans aucune
  assertion, est refusée par la règle `scenario_no_smoke_only`. Pour prouver
  qu'un appel « ne plante pas », préférer une vérification de valeur réelle
  (retour, propriété, état observable) plutôt qu'une assertion vide.

## Ajouter un test

1. Choisir le bon dossier : pédagogie → `scenarios/<catégorie>/` ;
   vérification technique → `runtime/` ; anti-régression → `regression/`.
2. Nommer `test_<sujet>.owl` (préfixe `test_` obligatoire).
3. Écrire des procédures `scenario_*` (scénario) ou un script de vérif direct
   (runtime/regression).
4. Utiliser `dbgVérifieEgalité(obtenu, attendu, libellé)` pour l'égalité,
   `dbgVérifieVrai/Faux(condition, libellé)` pour les booléens, avec **au
   moins une assertion sur valeur exacte**.
5. Terminer par `dbgBilanTest()`.
6. `./build/owl.exe test` pour vérifier la découverte et le vert.

## Fixtures compile-error

Les fixtures `tests/compile-error/*.owl` sont accompagnées d'un snapshot
`*.expected.json` généré **après impl** (le message d'erreur exact du compilo
n'est pas connu avant) :

```bash
./build/owl.exe --check tests/compile-error/foo.owl > tests/compile-error/foo.expected.json
```

Ne jamais committer un `.expected.json` dans le même lot que « tests posés
(rouges) » — il donnerait une fausse impression de couverture.

## Checklist pré-commit

- [ ] `mingw32-make all` réussit sans warning
- [ ] `./build/owl.exe --self-test-registry` : `violation_count: 0`
- [ ] `./build/owl.exe test` : suite verte
- [ ] Toute feature a un test dédié (scénario PUB + verrou runtime si besoin)
- [ ] Cas nominal **et** cas limites (vide, valeurs max, entrée invalide)

## Limitations connues

- Les tests réseau (HTTP) nécessitent un accès sortant ; ils échouent hors-ligne
  ou derrière un proxy bloquant.
- Les tests UI qui exercent les vrais widgets Qt utilisent
  `TestModeHeadless(Faux)` (montage offscreen) ; le reste tourne en headless.
</content>
</invoke>
