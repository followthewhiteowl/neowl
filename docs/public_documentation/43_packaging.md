# Packager une application

Une fois un projet au point, l'outil `owl-build` le transforme en un
**binaire natif autonome** : un exécutable unique, distribuable tel
quel, qui ne réclame rien d'installé sur la machine cible. Cette fiche
décrit ce que produit `owl-build`, ses prérequis et ses options. Le
contenu du fichier `projet.owl` lui-même — métadonnées, langues, code de
démarrage — est détaillé dans `24_projet.md` et n'est pas répété ici.

## 1. Ce que produit `owl-build`

`owl-build` prend un projet OWL en entrée et écrit un exécutable natif
qui embarque le programme et tout ce dont il a besoin pour tourner. Le
binaire obtenu se lance directement, sans interpréteur ni bibliothèque à
déployer à côté.

## 2. Aucun outil externe requis

`owl-build` porte **sa propre chaîne de compilation** : il n'y a aucun
compilateur, environnement d'exécution ou dépendance à installer au
préalable.

Au **premier lancement**, `owl-build` extrait sa charge interne dans un
dossier temporaire ; selon la machine et l'antivirus, cette étape va de
quelques secondes à plusieurs minutes, et sa progression s'affiche dans
la console. Les **lancements suivants** réutilisent cette extraction et
démarrent immédiatement.

Cette extraction peut être **payée à l'avance** avec l'option
`--prepare` : `owl-build --prepare` prépare le cache pour la plateforme
hôte, puis s'arrête sans packager de projet. Un premier build réel
démarre alors immédiatement. L'option est utile pour un déploiement qui
prépare la machine en amont. Elle respecte `--target` pour préchauffer
une autre cible, et ne fait rien (sans erreur) sur un binaire qui
n'embarque aucune charge.

## 3. Prérequis côté projet

`owl-build` attend un vrai **projet** : un dossier contenant un fichier
`projet.owl` qui déclare un bloc `est un Projet`. Ce bloc porte les
métadonnées (au minimum le champ `Langues`) et **son corps est le
programme** — le point de démarrage de l'application. Les fichiers
compagnons du dossier apportent leurs définitions (fenêtres, classes,
collections…) ; le code de `projet.owl` les orchestre.

Le modèle de projet complet — champs de métadonnées, ordre d'exécution,
accès en cascade — est décrit dans `24_projet.md`.

## 4. Ligne de commande

```
owl-build [DOSSIER_PROJET] [-o NOM] [--target TRIPLE]
```

| Argument / option | Rôle |
|-------------------|------|
| `DOSSIER_PROJET`  | Dossier du projet à packager. Requis ; indiquer `.` pour le dossier courant. |
| `-o NOM`          | Nom de l'exécutable produit. Par défaut, dérivé du projet. |
| `--target TRIPLE` | Plateforme visée. Par défaut, la plateforme hôte. |
| `--prepare`       | Prépare le cache d'extraction pour la cible, puis s'arrête sans packager de projet. Le build suivant pour cette cible démarre immédiatement. |

### Choix de la cible

L'option `--target` désigne la plateforme pour laquelle produire le
binaire. Sans elle, la cible est la **plateforme hôte** — celle sur
laquelle `owl-build` s'exécute.

`owl-build` sait produire pour `windows-x86_64` comme pour `linux-x86_64`.
Lorsque les deux cibles sont embarquées, un même `owl-build` produit aussi bien
un exécutable Windows qu'un binaire Linux statique autonome, quelle que soit la
plateforme sur laquelle il tourne : produire un binaire Linux depuis Windows
(ou l'inverse) ne demande alors aucun outil supplémentaire.

```
owl-build mon_projet --target linux-x86_64
```

Les cibles réellement disponibles restent celles embarquées dans le binaire
`owl-build` utilisé ; une distribution donnée peut en proposer davantage ou
se limiter à sa plateforme.

## 5. Emplacement du résultat

Le binaire est écrit sous le dossier du projet, dans une arborescence
`exe` :

```
<projet>/exe/<target>/<nom>[.exe]
```

Le segment `<target>` reprend la valeur de `--target`, c'est-à-dire la
cible visée. Le suffixe `.exe` n'apparaît que pour les cibles qui
l'exigent.

## 6. Paramètres de build

À ce jour, les paramètres de packaging se résument aux **options de
ligne de commande** décrites ci-dessus : dossier d'entrée, nom de sortie,
cible.

Il n'existe pas encore de manifeste de build déclaré dans le projet
lui-même. Cette capacité — décrire les paramètres de packaging au sein de
`projet.owl` — est prévue et sera documentée ici lorsqu'elle sera
ouverte.

## 7. Exemple minimal

Un projet réduit à l'essentiel : un `projet.owl` qui déclare ses
métadonnées et affiche un message au démarrage.

```owl
// projet.owl
MonApplication est un Projet
	PROPRIETES
		Langues     = [langueFrançais]
		Description = "Carnet d'adresses"
	FIN

	Trace("Démarrage de " + MonApplication..Description)
FIN
```

Depuis le dossier qui contient ce fichier, le packaging se lance en
désignant explicitement le dossier courant par `.` :

```bash
owl-build .
```

Lancé sans dossier, `owl-build` affiche son aide et s'arrête sans rien
packager : le dossier à traiter est toujours indiqué, `.` compris.

Ou en nommant explicitement le dossier et l'exécutable produit :

```bash
owl-build mon_projet -o carnet
```

Le binaire autonome apparaît alors sous `mon_projet/exe/<target>/`,
prêt à être distribué et exécuté sans rien installer d'autre.

## 8. Packager un serveur (ServiceREST)

Lorsqu'un projet déclare un bloc `est un ServiceREST`, `owl-build` le
reconnaît et produit un **exécutable serveur** : au lancement, le binaire
met en place le service, ouvre un port et répond aux requêtes jusqu'à son
arrêt. Il ne s'agit pas d'un programme qui s'exécute une fois puis se
termine, mais d'un processus qui reste en écoute.

Aucune option supplémentaire n'est requise au packaging : la présence d'un
`ServiceREST` dans la frontière du projet suffit à produire ce type de
binaire. Un projet sans service continue d'être packagé comme un programme
ordinaire.

La déclaration du bloc `ServiceREST`, de ses opérations `OpérationHTTP` et du
handler `surRequête` est décrite dans `40_services_rest.md`.

### Lancer le serveur packagé

L'exécutable produit se configure **au lancement**, par ligne de commande :

```
<exe> [--port N] [--bind ADR]
```

| Option      | Rôle |
|-------------|------|
| `--port N`  | Port d'écoute. Par défaut `8080`. |
| `--bind ADR`| Adresse d'écoute. Par défaut `127.0.0.1`. |
| `-h`, `--help` | Affiche l'aide et s'arrête. |

Par défaut, le serveur écoute uniquement sur `127.0.0.1` : il n'est
joignable que depuis la machine locale. Pour l'ouvrir au réseau, indiquer
explicitement `--bind 0.0.0.0`.

```bash
# Écoute locale sur le port 9000
monserveur --port 9000

# Ouverture au réseau sur le port par défaut
monserveur --bind 0.0.0.0
```

Un port invalide (non numérique ou hors de l'intervalle `1`–`65535`)
provoque un message d'erreur et un arrêt immédiat. Le serveur s'arrête par
`Ctrl-C`.

Comme un même binaire se déploie tel quel sur plusieurs machines, l'adresse
et le port se choisissent à l'exécution et non au packaging.

## 9. Limitations actuelles (alpha)

Deux limites à connaître avant de distribuer un binaire produit par
`owl-build` :

- **Binaire non signé.** Les exécutables produits ne portent pas de
  signature d'éditeur ; Windows SmartScreen peut afficher un
  avertissement « éditeur inconnu » au premier lancement. La signature
  est prévue.
- **Code source récupérable.** Le code `.owl` du projet est embarqué
  dans le binaire sous forme **compressée** : il n'apparaît pas en clair
  dans l'exécutable, mais reste récupérable par une analyse déterminée.
  Éviter, pour l'instant, de packager du code dont la confidentialité est
  critique. Un renforcement de la protection est à l'étude.
