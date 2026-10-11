Spec de calibration — lint garde-fou anti-natives-fantômes (,
volet B). Ce fichier n'est pas de la doc utilisateur : c'est le contrat
que l'implémentation du lint doit satisfaire avant clôture du ticket.
Il vit sous `tests/` (pas `docs/public_documentation/`, pas
`tests/scenarios/`) donc il est hors du champ de la règle 7 de style
utilisateur — mais reste sobre par cohérence.

## Objectif

Un contrôle build/test-time qui scanne la doc embarquée
(`docs/public_documentation/*.md`) et FLAG tout symbole cité **comme
native/fonction** dans la prose qui est absent du registre. Doit
tourner sous `make all` (cf. décision ticket — leçon ).

## Deux surfaces à construire

1. **Production** : une nouvelle règle dans `src/registry_self_test.c`
   (prochain numéro libre au moment de l'écriture de cette spec :
   **Règle 63** — vérifier le numéro encore libre au moment de
   l'implémentation, d'autres tickets peuvent avoir pris des numéros
   entre-temps). Calquer la structure sur la Règle 22
   (`check_rule22_public_files_mentions`, ligne ~2044) et la Règle 41
   (`check_rule41_public_files_utf8`, ligne ~3480) : un
   `scan_dir_recursive` sur `docs/public_documentation` (racine unique,
   PAS `examples/` ni `tests/scenarios/` — ces derniers ne sont pas la
   doc embarquée), lisant chaque `.md`, appliquant le détecteur
   (section Heuristique ci-dessous), et pour tout symbole flaggé posant
   `rep_violation(r, "public_doc_phantom_native", path, detail)` avec
   le nom du symbole et le numéro de ligne (même pattern que
   `scan_file_for_mentions`). Zéro tolérance : violation = échec de
   `--self-test-registry`, qui tourne déjà sous `make all` (confirmé :
   `Makefile:287` cible `self-test-registry: all`, appelée par
   `scripts/run_all_checks.sh:157`). Pas de nouveau flag CLI requis,
   juste une fonction de plus appelée dans l'orchestrateur (même
   séquence que les appels `check_rule22_public_files_mentions(&r)` /
   `check_rule41_public_files_utf8(&r)` visibles vers la fin du
   fichier).
   - Exclure `docs/public_documentation/candidats/` (chantier, même
     exemption que la Règle 22) et les blocs de code fencés (```…```)
     — un nom inventé dans un exemple de code n'est pas une citation
     de native dans la prose (cf. fixture `ignore_09`).
   - Résolution registre : le lookup doit couvrir les formes FR **et**
     EN canoniques (invariant 3 — bilinguisme), pas seulement un
     `strcmp` exact sur le nom FR. Sinon un alias EN légitime
     (`Contains` pour `Contient`) serait flaggé à tort.

2. **Calibration** : un harnais `--self-test-doc-phantom-lint` (ou nom
   équivalent), modélisé sur `src/internal_tests/compile_error_test.c`
   (fixtures `tests/compile-error/<name>.owl` + `.expected.json`). Ici :
   - Corpus : `tests/doc-lint-fixtures/catch/*.md` et
     `tests/doc-lint-fixtures/ignore/*.md`.
   - Oracle : `tests/doc-lint-fixtures/EXPECTED.json`, un objet
     `{"catch": {"<fichier>": [<noms attendus flaggés>, ...]}, "ignore":
     {"<fichier>": []}}`.
   - Pour chaque fichier de `catch/`, le détecteur DOIT produire
     exactement l'ensemble de noms listé (ni plus, ni moins — un nom en
     trop sur un fichier `catch/` est aussi un échec, cf. non-régression
     ci-dessous).
   - Pour chaque fichier de `ignore/`, le détecteur DOIT produire un
     ensemble vide.
   - Même patron de non-vacuité que `compile_error_test.c` (section 3
     de son en-tête) : le harnais doit PROUVER qu'il a bien itéré les
     16 fixtures (6 catch + 10 ignore), pas juste retourner vert par
     absence de corpus trouvé.
   - Gate sous `make all` / `scripts/run_all_checks.sh`, même famille
     que `--self-test-compile-error`.

Si le détecteur unique (fonction partagée) sert aux deux surfaces
(production ET calibration), c'est l'idéal — la calibration devient
alors une preuve directe que le détecteur de production est correct
avant de le lâcher sur le vrai corpus.

## État actuel (avant implémentation)

Le lint n'existe pas encore : aucune des deux surfaces n'est câblée.
`--self-test-doc-phantom-lint` n'existe pas comme flag CLI. C'est
attendu — voir `status: needs_input` dans le rapport du test_writer
pour la décision de routage (implémenter ce flag est un ajout `src/`,
hors périmètre test_writer).

Preuve d'état rouge côté Volet A (contenu doc, PAS le lint lui-même) :
`tests/regression/test_doc_phantom_natives_absent.owl`, rouge
aujourd'hui sur les 5 assertions "ne cite plus la native fantôme
X" — confirmé par exécution (`./build/owl.exe test
tests/regression/test_doc_phantom_natives_absent.owl -vv`, 5 FAIL sur
14, tous "got 1 expected 0" = présence confirmée du fantôme).

## Heuristique (3 règles, dérivées du calibrage empirique des fixtures)

Les 2 pistes du ticket ne suffisent PAS seules à catcher les 5 cas
réels sans réintroduire les faux positifs. Calibrage réel :

1. **Appel avec parenthèses** : token `Nom(...)` (majuscule initiale,
   style TitleCase/camelCase native) absent du registre (FR et EN) →
   haute confiance, flag direct. Couvre `catch_02` (EstUneProcedure a
   des parens), `catch_04` (Toast(...)), `catch_05` (Bidon(x)).
   Exclusion : token à l'intérieur d'un bloc fencé ```…``` (couvre
   `ignore_09`, où `Bidon(x)` apparaît en code, pas en prose).

2. **Contexte lexical "native"/"fonction"** dans la phrase (même
   ligne ou phrase courante) : tout token backtick TitleCase sans
   parenthèses, absent du registre, dont la phrase contient "native"
   ou "fonction" (insensible casse/accent) → flag. Couvre
   `catch_01` ("les natives `ChaineCommence`, `ChaineCherche`"),
   `catch_02` (InfoProcedure, phrase "Introspection native
   disponible…"), `catch_06` ("les natives (`Info`, `Bidon`,
   `Erreur`)").

3. **Énumération parenthésée de tokens backtick** (`(`A`, `B`, `C`)`,
   séparés par virgules, entre parenthèses) : chaque token de la liste
   est vérifié individuellement contre le registre (FR/EN), les
   inconnus sont flaggés, **indépendamment** du contexte lexical.
   Nécessaire pour `catch_03` (« Les popups (`Info`, `Erreur`,
   `Avertissement`, `Toast`) ne s'affichent pas » — AUCUN mot
   "native"/"fonction" dans la phrase, seule la structure
   d'énumération parenthésée signale une liste de callables). C'est
   la règle la MOINS couverte par les pistes initiales du ticket —
   **point d'attention explicite pour l'implémenter** : si cette
   3e règle produit des faux positifs sur le vrai corpus
   `docs/public_documentation/` (ex. une énumération parenthésée de
   types ou de champs de config), c'est un signal pour affiner
   (peut-être restreindre aux énumérations où AU MOINS un membre est
   un native réel du registre — cas de `catch_03` où Info/Erreur/
   Avertissement sont réels — pour distinguer "liste de fonctions"
   d'une "liste de types/champs"). Remonter au coordinator si le
   calibrage sur corpus réel révèle ce cas.

**Exclusions transverses** (toutes les 3 règles) :
- Token précédé de `..` (propriété type composé, invariant 6) → jamais
  flaggé, même absent du registre (couvre `ignore_04`).
- Bloc de code fencé ```…``` → jamais scanné pour la détection prose
  (couvre `ignore_09`).
- `docs/public_documentation/candidats/` → hors scope (chantier).

## Matrice résumée

| Fixture | Règle qui catche | Flaggé |
|---|---|---|
| `catch_01_chainecommence_chainecherche.md` | 2 (contexte "natives") | ChaineCommence, ChaineCherche |
| `catch_02_estuneprocedure_infoprocedure.md` | 1 (parens) + 2 (contexte "native") | EstUneProcedure, InfoProcedure |
| `catch_03_toast_popup_list.md` | 3 (énumération parenthésée) | Toast |
| `catch_04_toast_call.md` | 1 (parens) | Toast |
| `catch_05_generic_native_call.md` | 1 (parens) | Bidon |
| `catch_06_generic_popup_list_no_parens.md` | 2 (contexte "natives") + 3 (énumération) | Bidon |
| `ignore_01_real_native_no_parens.md` | — (résolu registre) | — |
| `ignore_02_real_native_call_and_property.md` | — (résolus registre + `..` property) | — |
| `ignore_03_real_file_natives.md` | — (résolus registre) | — |
| `ignore_04_property_backtick.md` | — (préfixe `..`) | — |
| `ignore_05_type_backtick.md` | — (pas de parens, pas de contexte native, pas d'énumération parenthésée) | — |
| `ignore_06_sql_keyword.md` | — (idem, contexte "SQL" pas "native") | — |
| `ignore_07_config_field.md` | — (idem, contexte "configuration") | — |
| `ignore_08_demo_identifier.md` | — (idem, contexte "contrôles se nomment") | — |
| `ignore_09_fenced_code_block.md` | — (exclusion bloc fencé) | — |
| `ignore_10_known_real_natives_list.md` | — (tous résolus registre) | — |

## Critères de sortie (repris du ticket, section Critères d'acceptation)

- Les 6 fixtures `catch/` produisent exactement les noms de
  `EXPECTED.json`.
- Les 10 fixtures `ignore/` produisent un ensemble vide.
- Après le fix Volet A (les 4 phrases corrigées dans les 3 fiches
  réelles), la Règle production sur `docs/public_documentation/`
  réel ne remonte AUCUNE violation.
- `make all` reste vert ; `--self-test-registry` à 0 violation.
