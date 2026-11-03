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
owl-build [DOSSIER_PROJET] [-o NOM] [--target TRIPLE] [--sign CERT.pfx [--sign-pass MDP]]
```

| Argument / option | Rôle |
|-------------------|------|
| `DOSSIER_PROJET`  | Dossier du projet à packager. Requis ; indiquer `.` pour le dossier courant. |
| `-o NOM`          | Nom de l'exécutable produit. Par défaut, dérivé du projet. |
| `--target TRIPLE` | Plateforme visée. Par défaut, la plateforme hôte. |
| `--sign CERT.pfx` | Signe le binaire produit avec un certificat de signature de code (PKCS#12). Cible Windows : signature Authenticode incrustée. Autres cibles : signature détachée `.p7s` + checksum `.sha256` à côté du binaire. |
| `--sign-pass MDP` | Mot de passe protégeant le certificat `.pfx`. Optionnel si le certificat n'en a pas. |
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

### Signer le binaire

L'option `--sign` signe le binaire produit avec un certificat **PKCS#12**
(`.pfx`/`.p12`) apporté par l'éditeur : il contient la clé privée et la chaîne
de certificats, et la signature porte **l'identité de l'éditeur** inscrite dans
le certificat. La même option s'applique à **toutes les cibles** ; le mécanisme
s'adapte à la plateforme visée.

```
owl-build mon_projet --sign mon_certificat.pfx --sign-pass motdepasse
```

Dispositions communes à toutes les cibles :

- Format attendu : certificat de **signature de code** (OV), algorithme
  **RSA**, empreinte **SHA-256**. Un certificat EV, qui réside sur un
  support matériel, n'est pas pris en charge par cette option fichier.
- Le certificat et le mot de passe sont validés **avant** le build : un mot
  de passe erroné échoue immédiatement, sans attendre la fin du packaging.
- Le mot de passe passé en ligne de commande peut apparaître dans la liste
  des processus du système. Pour un usage sensible, en tenir compte.

**Cible Windows — signature incrustée.** Une **signature de code Authenticode**
est apposée dans l'exécutable (format PE). Un binaire signé évite
l'avertissement « éditeur inconnu » de Windows SmartScreen et porte une
identité d'éditeur vérifiable.

**Cibles non-Windows (Linux, macOS) — signature détachée.** Les formats
exécutables de ces plateformes n'ont pas de signature incrustée vérifiée à
l'exécution. `--sign` produit à la place une **provenance vérifiable hors
ligne** : deux fichiers écrits à côté du binaire.

| Fichier | Contenu |
|---------|---------|
| `<binaire>.p7s`    | Signature **PKCS#7 détachée** (DER) sur les octets du binaire. Vérifiable avec `openssl cms -verify`. |
| `<binaire>.sha256` | Empreinte **SHA-256** au format `SHA256SUMS` (`<hex>␠␠<nom>`). Vérifiable avec `sha256sum -c`. |

Cette signature détachée ne retire aucun avertissement du système — Linux et
macOS n'en affichent pas pour un binaire téléchargé de cette façon. Elle sert
à **attester la provenance** : qui reçoit le binaire peut vérifier, avec la clé
publique de l'éditeur, qu'il n'a pas été altéré.

### Vérifier une signature

**Cible Windows** (signature Authenticode incrustée) : clic droit sur
l'exécutable → *Propriétés* → onglet *Signatures numériques*, ou en ligne de
commande PowerShell :

```
Get-AuthenticodeSignature mon_app.exe
```

Un résultat `Valid` indique une signature valide reconnue par le système. Un
certificat non reconnu (auto-signé, ou autorité absente du magasin) donne
`UnknownError` : la signature est bien formée, seule la confiance accordée au
certificat fait défaut.

**Cibles non-Windows** (signature détachée), avec le certificat public de
l'éditeur (`editeur.pem`), à côté du binaire et de ses deux fichiers :

```
openssl cms -verify -binary -content mon_app -in mon_app.p7s -inform DER -CAfile editeur.pem
sha256sum -c mon_app.sha256
```

`CMS Verification successful` + `mon_app: OK` confirment que le binaire provient
bien de l'éditeur attendu et n'a pas été modifié.

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

Limites à connaître avant de distribuer un binaire produit par
`owl-build` :

- **Signature facultative.** Par défaut, les binaires ne portent pas de
  signature d'éditeur et Windows SmartScreen peut afficher un avertissement
  « éditeur inconnu » au premier lancement. L'option `--sign` (section 4)
  appose une signature Authenticode sur les cibles Windows (et lève cet
  avertissement), ou une signature détachée `.p7s` + checksum `.sha256` sur
  les cibles non-Windows (provenance vérifiable hors ligne). L'horodatage de
  la signature (qui prolonge sa validité au-delà de l'expiration du
  certificat) est prévu.
- **Code source récupérable.** Le code `.owl` du projet est embarqué
  dans le binaire sous forme **compressée** : il n'apparaît pas en clair
  dans l'exécutable, mais reste récupérable par une analyse déterminée.
  Éviter, pour l'instant, de packager du code dont la confidentialité est
  critique. Un renforcement de la protection est à l'étude.
