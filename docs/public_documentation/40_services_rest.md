# Les services REST

Un service REST expose une partie du code sous forme d'API HTTP : des
opérations adressées par une méthode et un chemin, servies par un
serveur intégré. Le service se **déclare** avec le type `ServiceREST`,
chaque route est une `OpérationHTTP`, et la commande `owl serve` met le
tout en écoute. Comme le reste du langage, la déclaration est **bilingue**
FR/EN : les formes anglaises canoniques (`HTTPOperation`, `Method`,
`Path`…) sont équivalentes aux formes françaises et sont listées en fin de
fiche.

## 1. Déclarer un service

Un service s'écrit dans un bloc déclaratif au format des types composés.
Le champ `UrlBase` fixe le préfixe de chemin commun à toutes les
opérations, sous forme d'un tableau de segments. Chaque route est un
bloc `OpérationHTTP` imbriqué, accompagné d'un handler `surRequête`.

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : entier
			RENVOYER id
		FIN
	FIN
FIN
```

Les propriétés d'une `OpérationHTTP` :

| Propriété  | Rôle                                                        |
|------------|-------------------------------------------------------------|
| `Méthode`  | Verbe HTTP de la route : `httpGet`, `httpPost`, `httpPut`, `httpDelete`. |
| `Chemin`   | Segments du chemin, relatifs à `UrlBase` (voir § 2).        |
| `Réponses` | Codes de réponse que l'opération peut produire.             |
| `CodeÉtat` | Code renvoyé par défaut ; il doit figurer dans `Réponses`.  |
| `ExclureDeOpenAPI` | Booléen (défaut `Faux`). À `Vrai`, l'opération reste servie mais n'apparaît pas dans le document OpenAPI (voir § 11). |
| `Flux`     | Mode de diffusion progressive : `fluxSSE` ou `fluxOctets`. Absent, la réponse est produite en une fois (voir § 7). |
| `DélaiMax` | Durée maximale d'un flux, en secondes ; `0` (défaut) = sans limite (voir § 7). |

Le handler porte toujours le nom `surRequête`. Ses paramètres sont liés
automatiquement (voir § 4) et sa valeur de retour devient le corps de la
réponse (voir § 6). Un service peut déclarer plusieurs opérations, chacune
avec un nom distinct :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	ListeClients est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients"]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête() : entier
			RENVOYER 0
		FIN
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK, httpNonTrouvé]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : entier
			RENVOYER id
		FIN
	FIN
FIN
```

## 2. Router : chemins littéraux et paramètres

`Chemin` est une liste de segments. Un segment entre guillemets est un
**littéral** (il doit être présent tel quel dans l'URL) ; un identifiant nu
est un **paramètre** (il capture la valeur à cette position et la transmet
au handler par son nom). Ici, `clients` est fixe et `id` capture ce qui
suit :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : entier
			RENVOYER id
		FIN
	FIN
FIN
```

Une requête sur `/api/v1/clients/42` résout cette route avec `id` valant
`42`.

**Le littéral l'emporte sur le paramètre.** Deux opérations peuvent
partager la même position tant que l'une y pose un littéral et l'autre un
paramètre : la résolution est déterministe, le segment le plus spécifique
gagne. Ci-dessous, `/actifs` est servi par `ListeActifs`, tandis que
`/7` (ou toute autre valeur) tombe sur `LitClient` :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1", "clients"]
	FIN

	ListeActifs est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["actifs"]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête() : entier
			RENVOYER 0
		FIN
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = [id]
			Réponses = [httpOK, httpNonTrouvé]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : entier
			RENVOYER id
		FIN
	FIN
FIN
```

**Casse d'un segment littéral.** Un segment littéral a une forme
canonique — celle écrite dans `Chemin`. Une requête dont la casse diffère
sur un segment littéral (`/api/v1/Clients/42` là où la route déclare
`clients`) n'échoue pas : elle reçoit une **redirection 308** vers la forme
canonique. Le client suit la redirection et atteint la bonne route. Les
valeurs de paramètre, elles, sont transmises telles quelles.

## 3. Capturer une fin de chemin variable

Un littéral ou un paramètre capture **un seul** segment. Pour servir un
sous-arbre entier — un chemin de fichier, un proxy vers un chemin
imbriqué — il faut capturer **tout ce qui suit un préfixe**, quel que soit
le nombre de segments. C'est le rôle du **segment glouton** : un paramètre
préfixé de `*`, en dernière position de `Chemin`.

```owl
ServiceFichiers est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitFichier est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["fichiers", *reste]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(reste est une chaîne) : chaîne
			RENVOYER reste
		FIN
	FIN
FIN
```

Une requête sur `/api/v1/fichiers/images/logo.png` résout cette route, le
segment glouton `reste` capturant `images/logo.png`.

**Le glouton capture un ou plusieurs segments.** Il exige au moins un
segment après le préfixe : `/api/v1/fichiers` nu — sans rien après
`fichiers` — **ne correspond pas** à cette route. Pour servir aussi le
chemin nu, déclarer une opération distincte sur `["fichiers"]`.

**Le type du paramètre décide de la forme reçue.** Le même segment glouton
se lit de deux façons, selon le type déclaré du paramètre dans `surRequête` :

| Type déclaré           | Valeur reçue pour `/fichiers/images/logo.png`     |
|------------------------|---------------------------------------------------|
| `chaîne`               | la queue jointe par `/` : `"images/logo.png"`     |
| `tableau de chaîne`    | les segments séparés : `["images", "logo.png"]`   |

La forme `chaîne` est naturelle pour reconstruire un chemin de fichier ; la
forme `tableau de chaîne` sert quand chaque segment doit être traité à part :

```owl
ServiceFichiers est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitFichier est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["fichiers", *reste]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(reste est un tableau de chaîne) : entier
			RENVOYER Occurrence(reste)
		FIN
	FIN
FIN
```

**Un seul glouton, en dernière position.** Une route ne peut porter qu'un
segment glouton, et il doit être le **dernier** segment de `Chemin`. Un
glouton placé ailleurs (`["a", *reste, "b"]`) ou en double
(`["a", *x, *y]`) est une **erreur de compilation**. La règle garde la
résolution des routes déterministe : le glouton, par nature vorace, ne peut
pas être suivi d'autre chose sans ambiguïté.

## 4. Lier les paramètres

Les paramètres de `surRequête` sont liés **par nom**, depuis trois sources :

- le **chemin** — un paramètre de `Chemin` (`id`) alimente le paramètre
  homonyme du handler ;
- la **query** — un nom déclaré dans `Requête` alimente le paramètre
  homonyme depuis la chaîne de requête (`?tri=ville`) ;
- le **corps** — un paramètre dont le type est une `Structure` reçoit le
  corps de la requête, désérialisé dans cette structure.

La liaison est souple sur les types : une valeur textuelle du chemin ou de
la query est **coercée** vers le type déclaré du paramètre (`"123"` vers un
entier, par exemple).

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	stClient est une Structure
		nom est une chaîne
	FIN

	MajClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpPut
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier, client est une stClient) : entier
			RENVOYER id
		FIN
	FIN
FIN
```

Ici `id` vient du chemin et `client` — de type structuré — reçoit le corps.
Le type `stClient` se déclare **dans** le bloc `ServiceREST`, aux côtés des
opérations : un fichier-service ne contient que le bloc du service, et les
types qu'il utilise vivent à l'intérieur.

Un paramètre structuré attend un corps **JSON**. Le `Content-Type` de la
requête arbitre : absent ou `application/json` (ses paramètres comme
`; charset=utf-8` sont ignorés), le corps est désérialisé ; un corps JSON mal
formé est rejeté en **`400`** ; un `Content-Type` explicite autre que JSON est
rejeté en **`415`** sans tentative de lecture. Les deux rejets renvoient un
message indiquant le type attendu, et `surRequête` n'est pas appelé.

## 5. Recevoir un fichier téléversé

Un formulaire d'envoi de fichier arrive sous forme `multipart/form-data` :
un corps découpé en plusieurs parties, chacune portant un nom, certaines
contenant un fichier avec ses métadonnées (nom d'origine, type, taille),
d'autres un simple champ texte. Une opération reçoit ces fichiers via le
type `FichierTéléversé`, lié comme les autres paramètres — **par nom**.

**Deux déclarations, indissociables.** Recevoir un upload demande deux
choses ensemble :

- la propriété `TypeContenu = typeMimeFormulaireMultiPartie` sur
  l'`OpérationHTTP`, qui annonce que l'opération consomme un formulaire
  multipart ;
- au moins un paramètre de `surRequête` de type `FichierTéléversé`, qui
  reçoit une des parties fichier.

Les deux vont de pair. Déclarer `TypeContenu = typeMimeFormulaireMultiPartie`
sans aucun `FichierTéléversé`, ou l'inverse — un paramètre `FichierTéléversé`
sans la propriété — est **refusé à la compilation**. De même, un
`FichierTéléversé` ne peut pas cohabiter avec un paramètre structuré recevant
un corps (voir § 4) : un formulaire multipart et un corps `application/json`
sont deux contenus de requête mutuellement exclusifs, et leur coexistence est
une **erreur de compilation**.

```owl
ServiceDocuments est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	Téléverse est une OpérationHTTP
		PROPRIETES
			Méthode     = httpPost
			Chemin      = ["documents"]
			TypeContenu = typeMimeFormulaireMultiPartie
			Réponses    = [httpCréé]
			CodeÉtat    = httpCréé
		FIN
		PROCEDURE surRequête(titre est une chaîne, scan est un FichierTéléversé) : chaîne
			RENVOYER titre + " : " + scan..NomFichier + " (" + scan..TypeMime + ")"
		FIN
	FIN
FIN
```

Ici la partie texte `titre` et la partie fichier `scan` sont toutes deux
liées par leur nom : chaque paramètre de `surRequête` correspond à la partie
du formulaire qui porte le même nom. Les champs texte sont des paramètres
frères ordinaires (chaîne, entier…), coercés comme ceux de la query.

**Métadonnées avant contenu.** Un `FichierTéléversé` expose trois propriétés,
lisibles avec l'opérateur `..`, **sans avoir à lire le contenu du fichier** :

| Propriété     | Rôle                                                    |
|---------------|---------------------------------------------------------|
| `..NomFichier`| Nom d'origine du fichier, tel qu'envoyé par le client.  |
| `..Taille`    | Taille du fichier en octets.                            |
| `..TypeMime`  | Type de contenu déclaré pour cette partie.              |

Le nom d'origine est une donnée fournie par le client : il sert à
l'affichage ou au journal, jamais comme chemin de destination. Le contenu
reçu est conservé à part et n'emprunte pas ce nom.

**Plusieurs fichiers sous le même nom.** Quand un formulaire envoie
plusieurs fichiers sur le même nom de champ, le paramètre se déclare
`tableau de FichierTéléversé` : il reçoit toutes les parties correspondantes,
dans l'ordre d'arrivée.

```owl
ServiceDocuments est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	TéléverseLot est une OpérationHTTP
		PROPRIETES
			Méthode     = httpPost
			Chemin      = ["documents", "lot"]
			TypeContenu = typeMimeFormulaireMultiPartie
			Réponses    = [httpCréé]
			CodeÉtat    = httpCréé
		FIN
		PROCEDURE surRequête(fichiers est un tableau de FichierTéléversé) : entier
			total est un entier = 0
			POUR TOUT f DE fichiers
				total = total + f..Taille
			FIN
			RENVOYER total
		FIN
	FIN
FIN
```

**Plafond de taille.** Le serveur applique une limite globale sur la taille
du corps de requête. Un envoi qui dépasse cette limite est rejeté **avant**
d'atteindre l'opération : `surRequête` n'est pas appelé pour un corps trop
gros, et le service reste protégé sans code supplémentaire.

Un paramètre de query devient **optionnel** en lui donnant une valeur par
défaut. Comme pour toute procédure du langage, un paramètre optionnel se
déclare **en dernier** : aucun paramètre requis ne suit un paramètre à
défaut. Ci-dessous, `tri` est déclaré dans `Requête` et porte la valeur
`"nom"` par défaut :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	stClient est une Structure
		nom est une chaîne
	FIN

	MajClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpPut
			Chemin   = ["clients", id]
			Requête  = [tri]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier, client est une stClient, tri est une chaîne = "nom") : entier
			RENVOYER id
		FIN
	FIN
FIN
```

## 6. Répondre

La valeur passée à `RENVOYER` **est le corps** de la réponse. Son type
détermine le type de contenu :

- une **chaîne** est renvoyée telle quelle, en `text/plain` ;
- une **structure** (ou toute autre valeur non textuelle) est sérialisée
  automatiquement en **JSON**, en `application/json`.

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	stClient est une Structure
		nom est une chaîne
		ville est une chaîne
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : stClient
			client est une stClient
			client.nom = "Ada"
			client.ville = "Lyon"
			RENVOYER client
		FIN
	FIN
FIN
```

Ce handler répond `{"nom":"Ada","ville":"Lyon"}` en `application/json`.

**Le contexte d'échange.** Dans le corps du handler, `MoiMême` porte deux
sous-objets, adressés par l'opérateur de propriété `..` : `MoiMême..Réponse`
regroupe ce que le handler pose en sortie (`CodeÉtat`, `Corps`, `TypeContenu`)
et `MoiMême..Requête` ce qu'il consulte de la requête entrante (`Méthode`,
`Chemin`, `Identité`).

**Fixer le code de statut.** `MoiMême..Réponse..CodeÉtat` fixe le statut
renvoyé — par exemple `httpCréé` pour une création. Le code choisi doit
figurer dans `Réponses` :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	stNouveauClient est une Structure
		nom est une chaîne
	FIN

	CréeClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpPost
			Chemin   = ["clients"]
			Réponses = [httpCréé]
			CodeÉtat = httpCréé
		FIN
		PROCEDURE surRequête(client est une stNouveauClient) : chaîne
			MoiMême..Réponse..CodeÉtat = httpCréé
			RENVOYER "créé : " + client.nom
		FIN
	FIN
FIN
```

**Contrôle explicite du corps.** `MoiMême..Réponse..Corps` et
`MoiMême..Réponse..TypeContenu` prennent la main sur la déduction
automatique : `Corps` remplace la valeur renvoyée, `TypeContenu` impose
l'en-tête de type. Utile pour produire un contenu déjà formaté :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	Ping est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["ping"]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête() : chaîne
			MoiMême..Réponse..TypeContenu = "application/json"
			MoiMême..Réponse..Corps       = `{"etat":"ok"}`
			RENVOYER ""
		FIN
	FIN
FIN
```

Attention à ne pas confondre `..` et `.` : `MoiMême..Réponse..CodeÉtat`
utilise le double point (propriété du contexte d'échange), tandis qu'un
accès à un champ de structure utilise le point simple (`client.nom`).

**Servir un fichier binaire en téléchargement.** Un corps peut être un
`buffer` — une séquence d'octets bruts. Un corps de type `buffer` est
transmis **tel quel, octet pour octet, sans ré-encodage** ; un corps de
type `chaîne`, lui, est du texte. C'est cette différence qui permet de
servir un PDF, une image ou tout autre binaire **sans le corrompre** : le
contenu envoyé au client est exactement celui du buffer, sans traduction.
Deux formes produisent ce résultat. La première pose explicitement le corps
et son type ; `Content-Length` vaut alors la taille du buffer :

```owl
ServiceDocuments est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitRapport est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["rapports", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			contenu est un buffer = fChargeBuffer("rapports/" + id + ".pdf")
			MoiMême..Réponse..TypeContenu = "application/pdf"
			MoiMême..Réponse..Corps       = contenu
			RENVOYER ""
		FIN
	FIN
FIN
```

La seconde renvoie directement le buffer avec `RENVOYER` : le résultat est
le même corps binaire intact. À défaut de `..TypeContenu` posé, un corps
`buffer` renvoyé de cette façon prend le type `application/octet-stream` —
le type générique du binaire. Pour annoncer un PDF (ou tout autre format
précis), poser `..TypeContenu` avant le `RENVOYER` :

```owl
ServiceDocuments est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	TéléchargeRapport est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["rapports", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : buffer
			contenu est un buffer = fChargeBuffer("rapports/" + id + ".pdf")
			MoiMême..Réponse..TypeContenu = "application/pdf"
			RENVOYER contenu
		FIN
	FIN
FIN
```

`fChargeBuffer` lit ici le fichier en buffer ; toute source d'octets
convient de la même façon (contenu généré en mémoire, données reçues d'un
autre service).

**Poser des en-têtes de réponse.** `MoiMême..Réponse..Entêtes` est un
tableau associatif indexé par le **nom** de l'en-tête. Une affectation y
ajoute — ou remplace — un en-tête de la réponse : `Cache-Control`, un
en-tête CORS, un en-tête métier :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			MoiMême..Réponse..Entêtes["Cache-Control"] = "no-store"
			MoiMême..Réponse..Entêtes["X-Version"]     = "1.0"
			RENVOYER "ok"
		FIN
	FIN
FIN
```

Points à retenir :

- **Le nom est au pluriel : `..Entêtes`.** Le singulier `..Entête`
  n'existe pas et provoque une erreur de compilation.
- **Les noms d'en-tête sont insensibles à la casse**, conformément à
  HTTP : `Cache-Control` et `cache-control` désignent le même en-tête.
- **`Content-Type` reste piloté par `..TypeContenu`.** Poser un
  `Content-Type` via `..Entêtes` est ignoré au profit du canal dédié,
  qui évite tout doublon.
- **Les valeurs sont validées** : un nom ou une valeur d'en-tête
  malformé (injection de retour à la ligne, par exemple) est rejeté et
  l'en-tête n'est pas émis. Le service reste protégé.

**Lire les en-têtes de la requête entrante.** Symétrique du côté réponse,
`MoiMême..Requête..Entêtes` est un tableau associatif indexé par le **nom**
de l'en-tête, en **lecture seule** : il donne accès aux en-têtes envoyés par
le client (ou injectés par un intermédiaire). Le cas le plus courant est le
déploiement derrière un **reverse-proxy** (nginx, Apache, HAProxy…), qui pose
`X-Forwarded-For` (adresse réelle du client), `X-Forwarded-Proto` (schéma
d'origine), ou tout en-tête de traçage propre à la passerelle :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			ipClient est une chaîne = MoiMême..Requête..Entêtes["X-Forwarded-For"]
			SI ipClient = "" ALORS
				ipClient = "inconnue"
			FIN
			RENVOYER ipClient
		FIN
	FIN
FIN
```

Points à retenir :

- **Lecture seule.** `..Requête..Entêtes` sert à consulter la requête ; les
  en-têtes de la réponse se posent, eux, via `..Réponse..Entêtes`.
- **Les noms sont insensibles à la casse**, conformément à HTTP :
  `..Entêtes["x-forwarded-for"]` et `..Entêtes["X-Forwarded-For"]` désignent
  le même en-tête.
- **Un en-tête absent renvoie la chaîne vide** — d'où le repli sur une
  valeur par défaut dans l'exemple.
- **Un en-tête répété** dans la requête est présenté une seule fois, ses
  valeurs **jointes par `, `**.

**Un handler qui échoue produit un 500 propre.** Si le code du handler
lève une erreur, la requête reçoit une réponse `500` et le serveur
**continue** de tourner : une route fautive ne fait pas tomber le
service. Le handler d'erreur `surErreur` (voir § 9) permet de
personnaliser cette réponse.

## 7. Diffuser une réponse en flux (streaming)

Jusqu'ici, `surRequête` calcule une valeur et la renvoie : le corps de la
réponse est produit **en une fois**, à la fin du handler. Certaines
opérations ont besoin de l'inverse — garder la connexion ouverte et
**émettre le contenu par morceaux, au fil du temps** : un flux
d'événements en direct, l'export progressif d'un gros jeu de données, une
sortie générée au fur et à mesure. C'est le rôle du **streaming**.

Deux éléments l'activent, toujours ensemble :

- la propriété `Flux` sur l'`OpérationHTTP`, qui déclare le mode de
  diffusion ;
- la native `FluxÉcrit`, appelée dans `surRequête` pour émettre chaque
  morceau.

**Choisir le mode avec `Flux`.** Absente, l'opération répond
normalement (§ 6). Présente, elle prend l'une de deux valeurs :

| Valeur       | Diffusion                                                      |
|--------------|---------------------------------------------------------------|
| `fluxSSE`    | Événements au format *Server-Sent Events* (chaque morceau est encadré par le préfixe `data: `, chaque ligne d'un morceau multi-ligne étant re-préfixée), pour un flux d'événements lu par un client web. |
| `fluxOctets` | Octets bruts transmis en continu, sans encadrement, pour un contenu binaire ou texte produit au fil de l'eau. |

**Émettre avec `FluxÉcrit`.** `FluxÉcrit(morceau)` envoie un morceau au
client **et poursuit l'exécution** — c'est la différence essentielle avec
`RENVOYER`, qui termine le handler. Le handler `surRequête` reste **vivant
toute la durée du flux** : il est appelé **une seule fois**, boucle et
émet autant de morceaux que nécessaire ; la **fin de la procédure marque
la fin du flux**. Une opération de streaming ne pose donc pas de
`RENVOYER` : elle écrit, puis se termine.

**Le morceau accepte tout type.** Comme la valeur d'un `RENVOYER` (§ 6),
l'argument de `FluxÉcrit` peut être de n'importe quel type. Une **chaîne**
(ou un **buffer**) est émise **telle quelle**, octet pour octet ; toute
**autre valeur** — structure, tableau, entier — est **sérialisée
automatiquement en JSON minifié** (une seule ligne, adaptée à un événement
SSE). C'est le cas d'usage le plus courant du SSE : émettre un événement
structuré. Ici, chaque tour envoie une structure, transmise au client sous
forme d'un objet JSON :

```owl
ServiceNotifications est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	stÉvénement est une Structure
		numero est un entier
		message est une chaîne
	FIN

	Diffuse est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["notifications", depuis]
			Flux     = fluxSSE
			DélaiMax = 300
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(depuis est un entier)
			POUR i = depuis À depuis + 3
				évt est une stÉvénement
				évt.numero  = i
				évt.message = "à jour"
				FluxÉcrit(évt)
			FIN
		FIN
	FIN
FIN
```

Une requête sur `/api/v1/notifications/5` reçoit alors un événement par
tour de boucle, chaque structure sérialisée en JSON :

```
data: {"numero":5,"message":"à jour"}

data: {"numero":6,"message":"à jour"}
```

Chaque `FluxÉcrit` part immédiatement sur la connexion ouverte ; le client
reçoit les événements au rythme où ils sont émis, sans attendre la fin du
handler. Pour émettre du texte brut plutôt qu'un objet, passer directement
une chaîne : `FluxÉcrit("événement " + i)` produit `data: événement 5`. Un
morceau **multi-ligne** est encadré ligne par ligne — chaque ligne interne
reçoit son propre préfixe `data: `, conformément au format SSE.

**Le statut est figé dès le premier morceau.** Envoyer le premier
`FluxÉcrit` met les en-têtes et le code de statut **sur le fil** : ils ne
peuvent plus changer ensuite. Le motif de contrôle du statut décrit au
§ 6 — poser `MoiMême..Réponse..CodeÉtat` puis `RENVOYER` — ne vaut donc
qu'**avant** le premier morceau. Une erreur qui survient **avant** le
premier `FluxÉcrit` produit un `500` propre, comme une opération normale ;
une erreur **en cours de flux** ne peut plus réécrire le statut : elle
ferme la connexion de façon abrupte, le contenu déjà reçu restant tronqué.

**Borner la durée avec `DélaiMax`.** `DélaiMax` fixe une durée maximale de
vie du flux, **en secondes**. Passé ce délai, le serveur ferme le
transport, même si le handler n'a pas terminé. La valeur `0` — le défaut —
signifie **aucune limite** : le flux vit tant que le handler émet et que
le client écoute. Poser une borne protège le service des connexions qui
traînent.

**Une déconnexion du client lève une exception.** Si le client ferme la
connexion pendant le flux, l'appel suivant à `FluxÉcrit` **lève une
exception**. Sans traitement, elle déroule le handler et le framework
libère le flux — rien à écrire pour le cas courant. Le seul cas à gérer
est celui d'une **ressource à libérer** (un fichier ouvert, une connexion
externe) : entourer l'émission d'un `QUAND EXCEPTION DANS … FAIRE` suffit
à garantir le nettoyage, que le flux se termine normalement ou soit coupé
par le départ du client.

Deux constantes exposent le code de ces exceptions, à comparer à
`ExceptionInfo(errCode)` pour distinguer les cas :

| Constante            | Cause                                                              |
|----------------------|--------------------------------------------------------------------|
| `httpFluxDéconnecté` | Le client s'est déconnecté pendant le flux (ou `DélaiMax` atteint). |
| `httpFluxHorsContexte` | `FluxÉcrit` appelé hors d'un flux ouvert (erreur de programmation). |

Le handler peut ainsi ne rattraper que la déconnexion — le cas normal à
gérer — et laisser tout autre incident se propager :

```owl
ServiceExport est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	ExporteLignes est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["export"]
			Flux     = fluxOctets
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête()
			QUAND EXCEPTION DANS
				POUR i = 1 À 1000
					FluxÉcrit("ligne " + i + RC)
				FIN
			FAIRE
				SI ExceptionInfo(errCode) = httpFluxDéconnecté ALORS
					Trace("client parti : libération des ressources")
				SINON
					// Autre incident : on ne l'avale pas.
					ExceptionPropage()
				FIN
			FIN
		FIN
	FIN
FIN
```

Comparer `ExceptionInfo(errCode)` à `httpFluxDéconnecté` cible précisément
le départ du client ; toute autre exception (y compris un `FluxÉcrit` hors
contexte, signalé par `httpFluxHorsContexte`) remonte intacte plutôt
que d'être masquée par le bloc de nettoyage.

## 8. Authentifier les requêtes

Un service peut exiger que chaque requête soit **authentifiée** avant
d'atteindre ses opérations. Le service déclare un **schéma**
d'authentification dans ses `PROPRIETES`, et fournit un handler chargé de
valider l'identité. Le schéma `httpAuthBearer` attend un jeton porteur dans
l'en-tête `Authorization` de la requête.

**Déclarer le schéma et le handler.** La propriété `Authentification`
choisit le schéma ; le handler `surAuthentification`, déclaré **au niveau du
service** (à côté des opérations, hors d'une `OpérationHTTP`), reçoit le
credential déjà extrait selon le schéma. Pour `httpAuthBearer`, c'est le
jeton qui suit `Bearer ` dans l'en-tête `Authorization`. Le handler renvoie
`Vrai` pour accepter la requête, `Faux` pour la rejeter :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase          = ["api", "v1"]
		Authentification = httpAuthBearer
	FIN

	stIdentité est une Structure
		nom est une chaîne
	FIN

	PROCEDURE surAuthentification(jeton est une chaîne) : booléen
		SI jeton = "secret" ALORS
			id est une stIdentité
			id.nom = "Ada"
			MoiMême..Requête..Identité = id
			RENVOYER Vrai
		FIN
		RENVOYER Faux
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			ident est une stIdentité = MoiMême..Requête..Identité
			RENVOYER ident.nom
		FIN
	FIN
FIN
```

**Déposer et relire l'identité.** Sur acceptation, le handler pose
l'identité applicative dans `MoiMême..Requête..Identité` — une valeur au
choix de l'application (ici une structure). Chaque opération la relit
ensuite dans `surRequête` via `MoiMême..Requête..Identité`, sans refaire la
validation.

**Rejeter proprement.** Quand `surAuthentification` renvoie `Faux`, la
requête est rejetée **avant** `surRequête` : aucune opération n'est
exécutée. Le handler peut fixer le statut du refus via
`MoiMême..Réponse..CodeÉtat`, par exemple `httpNonAutorisé` :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase          = ["api", "v1"]
		Authentification = httpAuthBearer
	FIN

	PROCEDURE surAuthentification(jeton est une chaîne) : booléen
		SI jeton = "" ALORS
			MoiMême..Réponse..CodeÉtat = httpNonAutorisé
			RENVOYER Faux
		FIN
		RENVOYER jeton = "secret"
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients"]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête() : chaîne
			RENVOYER "ok"
		FIN
	FIN
FIN
```

Si le handler ne fixe aucun statut, le rejet reçoit un **`401` par défaut** :
un refus sans code explicite reste sûr.

**Dispenser une opération.** Une opération peut se soustraire au schéma en
déclarant `Authentification = httpAuthAucune` dans ses `PROPRIETES`. Elle
n'exige alors aucune authentification — utile pour une route de connexion ou
un contrôle de santé ouvert :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase          = ["api", "v1"]
		Authentification = httpAuthBearer
	FIN

	PROCEDURE surAuthentification(jeton est une chaîne) : booléen
		RENVOYER jeton = "secret"
	FIN

	Santé est une OpérationHTTP
		PROPRIETES
			Méthode          = httpGet
			Chemin           = ["sante"]
			Authentification = httpAuthAucune
			Réponses         = [httpOK]
			CodeÉtat         = httpOK
		FIN
		PROCEDURE surRequête() : chaîne
			RENVOYER "ok"
		FIN
	FIN
FIN
```

**Le schéma doit avoir son handler.** Déclarer `Authentification` sur un
service sans fournir `surAuthentification` produit un **avertissement à la
compilation** : sans handler, le schéma n'est pas appliqué et le service
répond à toutes les requêtes. Il faut le handler pour que la protection
prenne effet.

**Autoriser au-delà d'authentifier.** L'authentification répond à
« *qui* fait la requête ? » ; l'autorisation répond à « ce demandeur
a-t-il le **droit** de faire cette action ? ». Le handler
`surAutorisation`, lui aussi déclaré **au niveau du service**, s'exécute
**après** `surAuthentification` et **avant** l'opération. Il ne reçoit
aucun argument : il **lit** l'identité déjà déposée
(`MoiMême..Requête..Identité`), au besoin la méthode et le chemin
(`MoiMême..Requête..Méthode`, `MoiMême..Requête..Chemin`), puis renvoie
`Vrai` (autorisé) ou `Faux` (interdit) :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase          = ["api", "v1"]
		Authentification = httpAuthBearer
	FIN

	stIdentité est une Structure
		rôle est une chaîne
	FIN

	PROCEDURE surAuthentification(jeton est une chaîne) : booléen
		SI jeton = "" ALORS
			RENVOYER Faux
		FIN
		id est une stIdentité
		SI jeton = "admin" ALORS
			id.rôle = "admin"
		SINON
			id.rôle = "invité"
		FIN
		MoiMême..Requête..Identité = id
		RENVOYER Vrai
	FIN

	PROCEDURE surAutorisation() : booléen
		ident est une stIdentité = MoiMême..Requête..Identité
		SI ident.rôle = "admin" ALORS
			RENVOYER Vrai
		FIN
		MoiMême..Réponse..CodeÉtat = httpInterdit
		RENVOYER Faux
	FIN

	SupprimeClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpDelete
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			RENVOYER "supprimé"
		FIN
	FIN
FIN
```

Ce que garantit `surAutorisation` :

- **Rejet sûr par défaut.** Quand il renvoie `Faux`, la requête est
  rejetée avant l'opération. Sans code posé, le refus reçoit un **`403`
  par défaut** ; le handler peut fixer un autre statut via
  `MoiMême..Réponse..CodeÉtat`.
- **Il ne tourne que s'il est déclaré**, et **après** une
  authentification réussie : une requête rejetée par
  `surAuthentification` (401) n'atteint jamais l'autorisation.
- **Une opération dispensée d'authentification** (`Authentification =
  httpAuthAucune`) est également dispensée d'autorisation : une route
  publique ne passe ni par l'un ni par l'autre.

## 9. Cycle de vie et gestion des erreurs

Au-delà des requêtes individuelles, un service peut brancher du code sur
son **cycle de vie** — un démarrage, un arrêt — et sur le **chemin
d'erreur** quand une opération échoue. Ces handlers se déclarent au
niveau du service, à côté des opérations.

**Initialiser et libérer une fois.** Deux handlers encadrent la vie du
service :

- `FinInitialisation` s'exécute **une seule fois**, au démarrage, avant
  que le service n'accepte la première requête. C'est l'endroit pour
  ouvrir une connexion, préparer un cache, charger une configuration.
- `Fermeture` s'exécute **une seule fois**, à l'arrêt, pour libérer ce
  qui a été mis en place (fermer une connexion, écrire un journal final).

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	PROCEDURE FinInitialisation()
		Trace("service prêt")
	FIN

	PROCEDURE Fermeture()
		Trace("service arrêté")
	FIN

	Santé est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["sante"]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête() : chaîne
			RENVOYER "ok"
		FIN
	FIN
FIN
```

**Attention au nommage.** Ces deux handlers s'appellent
`FinInitialisation` et `Fermeture` — les mêmes noms que les événements de
cycle de vie ailleurs dans le langage, réutilisés ici tels quels. Ce ne
sont **pas** `surInitialisation` ni `surFermeture` : seuls les handlers
liés à une requête portent le préfixe `sur` (`surRequête`,
`surAuthentification`, `surAutorisation`, `surErreur`).

Si `FinInitialisation` échoue, le démarrage est **avorté** : un service
qui ne peut pas s'initialiser ne se met pas en écoute, plutôt que de
servir dans un état incomplet.

**Personnaliser la réponse d'erreur.** Quand une opération lève une
erreur, la réponse par défaut est un `500` au corps vide. Le handler
`surErreur` permet de reprendre la main sur cette réponse. Il **ne reçoit
aucun argument** aujourd'hui ; il consulte la requête en cours
(`MoiMême..Requête`) et **écrit** la réponse à renvoyer
(`MoiMême..Réponse..Corps`, `..CodeÉtat`, `..TypeContenu`). Le détail de
l'erreur qui l'a déclenché se lit avec `ExceptionInfo()` — le message sans
argument, ou un champ précis avec une constante `errXxx` — comme dans tout
traitement d'erreur :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	PROCEDURE surErreur()
		MoiMême..Réponse..TypeContenu = "application/json"
		MoiMême..Réponse..Corps       = `{"erreur":"interne"}`
	FIN

	LitClient est une OpérationHTTP
		PROPRIETES
			Méthode  = httpGet
			Chemin   = ["clients", id]
			Réponses = [httpOK]
			CodeÉtat = httpOK
		FIN
		PROCEDURE surRequête(id est un entier) : chaîne
			RENVOYER "ok"
		FIN
	FIN
FIN
```

`surErreur` est **optionnel** : sans lui, une opération fautive produit
le `500` propre décrit au § 6. S'il est présent, la réponse finale
reprend ce qu'il a posé, et à défaut de code explicite reste un `500`. Le
service continue de tourner dans tous les cas.

## 10. Lancer le serveur

La commande `owl serve` compile un fichier, monte les services qu'il
déclare et met le serveur en écoute :

```bash
owl serve MonAPI.owl
```

Par défaut, le serveur écoute sur `127.0.0.1:8080` (accessible depuis la
machine locale uniquement). Deux options ajustent l'écoute :

| Option        | Effet                                              | Défaut       |
|---------------|----------------------------------------------------|--------------|
| `--port N`    | Port d'écoute.                                     | `8080`       |
| `--bind adr`  | Adresse d'écoute. `0.0.0.0` expose au réseau.      | `127.0.0.1`  |

```bash
owl serve MonAPI.owl --port 9000 --bind 0.0.0.0
```

Le serveur tourne jusqu'à interruption au clavier (`Ctrl-C`), qui l'arrête
proprement.

Au-delà de `owl serve`, un service se **package aussi en exécutable serveur
autonome** : un binaire unique qui met le service en écoute sans interpréteur
à déployer. Voir [Packager une application](43_packaging.md), section
« Packager un serveur (ServiceREST) ».

## 11. Décrire le service en OpenAPI

Un service porte déjà toute sa description : méthodes, chemins, paramètres,
codes de réponse, corps. La commande `owl openapi` en **dérive** un document
[OpenAPI 3](https://www.openapis.org/) — le format standard de description
d'API HTTP, lisible par les outils clients, les générateurs de code et les
explorateurs interactifs :

```bash
owl openapi MonAPI.owl
```

Le document est écrit en JSON sur la sortie standard, prêt à rediriger vers
un fichier ou à passer à un autre outil :

```bash
owl openapi MonAPI.owl > openapi.json
```

Pour un service exposant une seule opération de lecture, la sortie ressemble
à ceci (extrait fidèle) :

```json
{
  "openapi":"3.0.3",
  "info":{
    "title":"ServiceClients",
    "version":"1.0.0"
  },
  "paths":{
    "/api/v1/clients/{id}":{
      "get":{
        "operationId":"LitClient",
        "parameters":[
          {
            "name":"id",
            "in":"path",
            "required":true,
            "schema":{"type":"integer"}
          }
        ],
        "responses":{
          "200":{
            "description":"200 OK",
            "content":{
              "application/json":{
                "schema":{"type":"string"}
              }
            }
          },
          "404":{
            "description":"404 Not Found",
            "content":{
              "application/problem+json":{
                "schema":{"type":"object"}
              }
            }
          },
          "500":{
            "description":"500 Internal Server Error",
            "content":{
              "application/problem+json":{
                "schema":{"type":"object"}
              }
            }
          }
        }
      }
    }
  },
  "components":{}
}
```

Chaque opération devient une entrée de `paths` : le nom de l'opération donne
l'`operationId`, les paramètres de chemin et de query deviennent des
`parameters`, et chaque code de `Réponses` produit une réponse décrite. Les
titres reprennent le nom du service.

**Un `500` est toujours décrit.** Toute opération peut échouer et produire
un `500` (voir § 6) : le document ajoute donc systématiquement une réponse
`500` — en `application/problem+json` — à chaque opération qui n'en déclare
pas déjà une dans `Réponses`. Le contrat reflète ainsi le comportement réel
du service, où n'importe quelle route peut renvoyer une erreur interne.

**Le content-type suit le mode de diffusion.** Pour une opération sans
`Flux`, la réponse de succès est décrite en `application/json`. Pour une
opération en streaming (§ 7), le document reflète le média réellement
servi : `text/event-stream` pour `fluxSSE`, `application/octet-stream`
(schéma binaire) pour `fluxOctets`. Les réponses d'erreur restent, elles,
en `application/problem+json` quel que soit le mode.

**Exclure une opération du document.** Toute route sert par défaut de source à
sa propre description. La propriété `ExclureDeOpenAPI` sur une `OpérationHTTP`
retire l'opération du document généré **sans changer son routage** : à `Vrai`,
la route reste servie normalement, mais elle n'apparaît ni dans `paths` ni
ailleurs dans le contrat. Utile pour masquer une route interne — un contrôle de
santé, une route d'administration, la plomberie de documentation — d'un contrat
public. Le défaut est `Faux` (l'opération est documentée) :

```owl
ServiceClients est un ServiceREST
	PROPRIETES
		UrlBase = ["api", "v1"]
	FIN

	Santé est une OpérationHTTP
		PROPRIETES
			Méthode          = httpGet
			Chemin           = ["sante"]
			ExclureDeOpenAPI = Vrai
			Réponses         = [httpOK]
			CodeÉtat         = httpOK
		FIN
		PROCEDURE surRequête() : chaîne
			RENVOYER "ok"
		FIN
	FIN
FIN
```

Ici la route `/api/v1/sante` répond normalement, mais reste absente de
l'`openapi.json`.

**Le document est dérivé, jamais une seconde source.** Il n'y a rien à tenir
à jour en parallèle : la description vient du service lui-même. Modifier une
opération puis régénérer suffit à obtenir un document à jour ; éditer le
JSON à la main n'a pas de sens, la génération suivante l'écraserait.

**Servi en ligne.** Un serveur lancé par `owl serve` expose le même document
sur la route `GET /openapi.json`. Un client peut donc récupérer la
description directement auprès du service en fonctionnement, sans fichier
intermédiaire :

```bash
curl http://localhost:8080/openapi.json
```

## 12. Cycle complet

La commande `owl new` **affiche** un squelette de service sur la sortie
standard ; le rediriger vers un fichier pour l'éditer :

```bash
owl new service MonAPI > MonAPI.owl
```

Le fichier `MonAPI.owl` contient alors un `ServiceREST` minimal avec une
opération de lecture. Il suffit d'ajouter ou d'adapter les opérations,
puis de lancer le serveur :

```bash
owl serve MonAPI.owl
```

Le service est alors interrogeable :

```bash
curl http://localhost:8080/api/v1/clients/42
```

## Aliases anglais

| FR canon         | EN canon        |
|------------------|-----------------|
| `ServiceREST`    | `ServiceREST`   |
| `OpérationHTTP`  | `HTTPOperation` |
| `surRequête`     | `onRequest`     |
| `MoiMême`        | `MySelf`        |
| `Authentification` | `Authentication` |
| `surAuthentification` | `onAuthentication` |
| `surAutorisation` | `onAuthorization` |
| `surErreur`      | `onError`       |
| `FinInitialisation` | `EndInitialization` |
| `Fermeture`      | `Closing`       |
| `httpAuthBearer` | `httpAuthBearer` |
| `httpAuthAucune` | `httpAuthNone`  |
| `Réponse` (contexte) | `Response`  |
| `Requête` (contexte) | `Request`   |
| `UrlBase`        | `BaseUrl`       |
| `Méthode`        | `Method`        |
| `Chemin`         | `Path`          |
| `Identité`       | `Identity`      |
| `Requête` (liste de liaison) | `Query` |
| `Réponses`       | `Responses`     |
| `CodeÉtat`       | `StatusCode`    |
| `Corps`          | `Body`          |
| `TypeContenu`    | `ContentType`   |
| `Entêtes`        | `Headers`       |
| `ExclureDeOpenAPI` | `ExcludeFromOpenAPI` |
| `Flux`           | `Stream`        |
| `DélaiMax`       | `MaxDelay`      |
| `FluxÉcrit`      | `StreamWrite`   |
| `httpFlux`       | `httpStream`    |
| `httpFluxDéconnecté` | `httpStreamDisconnected` |
| `httpFluxHorsContexte` | `httpStreamOutOfContext` |
| `fluxSSE`        | `streamSSE`     |
| `fluxOctets`     | `streamBytes`   |
| `FichierTéléversé` | `UploadedFile` |
| `NomFichier`     | `FileName`      |
| `Taille`         | `Size`          |
| `TypeMime`       | `MimeType`      |
| `typeMimeFormulaireMultiPartie` | `mimeTypeMultipartForm` |

Voir aussi : [Le fichier projet.owl](24_projet.md), [Encodage et
entrées/sorties](26_encodage_et_io.md).
