# Services AWS : `AWSSession`, `AWSRequête` et clients dédiés

## 1. En bref

Deux couches permettent d'adresser les services AWS depuis OWL :

- **L'appel générique `AWSRequête`** — utilisable avec **n'importe quel**
  service (S3, IAM, DynamoDB, EC2, STS, API Gateway…). On lui passe une
  **session** d'identifiants, un **service**, une **action** et un
  **corps** ; il signe la requête (SigV4), l'envoie et rend la réponse
  décodée en `JSON`.
- **Des clients dédiés** — `AwsS3Client`, `AwsBedrockClient`,
  `AwsLambdaClient` — qui offrent une forme objet plus directe pour les
  opérations les plus courantes de leur service.

Toutes les requêtes partent d'une même feuille d'identifiants,
`AWSSession` : une région et deux clés.

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON = ChaîneVersJSON("{}")
réponse est un JSON = AWSRequête(sess, awsIAM, awsIAMListUsers, corps)

Trace(réponse.ListUsersResponse.ListUsersResult.IsTruncated)
```

La réponse est un document `JSON` que l'on parcourt avec le point simple
(`réponse.Chemin.Vers.Le.Champ`), exactement comme n'importe quel autre
`JSON`.

---

## 2. La session : `AWSSession`

`AWSSession` est un type composé qui rassemble les identifiants d'appel.
Ses propriétés se lisent et s'écrivent avec le double point `..` :

| Propriété | Rôle | Obligatoire |
|---|---|---|
| `..Région` | Région AWS ciblée (ex. `us-east-1`). | oui |
| `..CléAccès` | Identifiant de clé d'accès. | oui |
| `..CléSecrète` | Clé d'accès secrète. | oui |
| `..Jeton` | Jeton de session pour des identifiants temporaires. | non |
| `..Endpoint` | Point d'entrée personnalisé. | non |

Une session déclarée sans initialisation part avec toutes ses propriétés
vides ; on les renseigne avant le premier appel.

```owl
sess est une AWSSession
sess..Région     = "eu-west-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
```

### Identifiants temporaires : `..Jeton`

Les identifiants obtenus temporairement (rôle assumé, session à durée
limitée) s'accompagnent d'un **jeton de session** en plus des deux clés.
Il se pose sur `..Jeton` et part alors automatiquement avec chaque
requête signée.

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "ASIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
sess..Jeton      = "AQoDYXdzEJr...EXAMPLETOKEN"
```

### Point d'entrée personnalisé : `..Endpoint`

Par défaut, le point d'entrée est dérivé automatiquement du service et de
la région. Le renseigner sur `..Endpoint` force une adresse précise —
utile pour viser un service compatible auto-hébergé ou un environnement de
test local.

```owl
sess est une AWSSession
sess..Région   = "us-east-1"
sess..Endpoint = "http://localhost:4566"
```

---

## 3. L'appel générique : `AWSRequête`

`AWSRequête(session, service, action, corps)` couvre l'ensemble des
services. Ses quatre arguments :

| Argument | Type | Rôle |
|---|---|---|
| `session` | `AWSSession` | Identifiants et région. |
| `service` | `∈ awsService` | Le service ciblé, choisi dans l'énumération `awsService`. |
| `action` | symbole ou chaîne | L'opération à invoquer. |
| `corps` | `JSON` | Le corps de la requête. |

Elle rend la **réponse décodée en `JSON`**.

### Le service : contraint à `awsService`

Le deuxième argument se choisit dans l'énumération `awsService`
(`awsS3`, `awsIAM`, `awsDynamoDB`, `awsEC2`, `awsSTS`,
`awsBedrockRuntime`…). Comme il s'agit d'une valeur d'énumération et non
d'une chaîne libre, une faute de frappe sur le service est signalée dès la
compilation, pas au moment de l'appel réseau.

### L'action : symbole vérifié tôt, ou chaîne libre

L'action s'exprime de deux manières :

- **Par symbole** de la forme `aws<Service><Action>` — par exemple
  `awsIAMListUsers`, `awsS3ListBuckets`, `awsDynamoDBGetItem`. Ce symbole
  est **résolu à la compilation** : une action inconnue est détectée tôt,
  et l'appel embarque au passage le cadrage exact de l'opération (méthode,
  chemin, protocole).
- **Par chaîne libre** en repli — par exemple `"ListUsers"`. Pratique pour
  une opération qui n'aurait pas encore de symbole dédié ; le nom est pris
  tel quel.

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON = ChaîneVersJSON(`{"TableName":"Clients","Key":{"id":{"S":"42"}}}`)

// Action par symbole (vérifiée à la compilation)…
parSymbole est un JSON = AWSRequête(sess, awsDynamoDB, awsDynamoDBGetItem, corps)

// … ou par chaîne libre, prise telle quelle.
parChaine est un JSON = AWSRequête(sess, awsDynamoDB, "GetItem", corps)
```

### Le corps et la réponse

Le corps est un `JSON` : la native l'adapte au protocole attendu par le
service (corps JSON, formulaire, ou document structuré selon le cas). La
réponse revient **décodée en `JSON`**, quelle que soit la forme d'origine
renvoyée par le service. Les **listes** de la réponse sont exposées comme
des **tableaux** JSON, indexés à partir de `1`.

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON = ChaîneVersJSON("{}")
réponse est un JSON = AWSRequête(sess, awsSTS, awsSTSGetCallerIdentity, corps)

compte est une chaîne = réponse.GetCallerIdentityResponse.GetCallerIdentityResult.Account
Trace(compte)
```

---

## 4. Les clients dédiés

Pour les services les plus utilisés, un **client objet** propose une forme
plus directe que l'appel générique. Le client se déclare, reçoit une
session via `..Session`, puis expose ses opérations en appel de méthode
(`.Méthode(...)`).

### Stockage d'objets : `AwsS3Client`

`AwsS3Client` couvre les opérations de fichiers sur S3 en style
chemin (`/bucket/clé`) :

| Méthode | Rôle |
|---|---|
| `.Upload(bucket, clé, buffer, contentType)` | Dépose un objet (corps binaire). |
| `.Télécharge(bucket, clé)` | Récupère un objet, rendu en `buffer`. |
| `.Liste(bucket, préfixe)` | Liste les objets d'un préfixe, rendu en `JSON`. |
| `.Supprime(bucket, clé)` | Supprime un objet. |

```owl
sess est une AWSSession
sess..Région     = "eu-west-3"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

s3 est un AwsS3Client
s3..Session = sess

contenu est un buffer = "contenu de test"
s3.Upload("mon-bucket", "dossier/fichier.txt", contenu, "text/plain")

objets est un JSON = s3.Liste("mon-bucket", "dossier/")
Trace(objets.Contents[1].Key)

octets est un buffer = s3.Télécharge("mon-bucket", "dossier/fichier.txt")
s3.Supprime("mon-bucket", "dossier/fichier.txt")
```

Le corps déposé et le corps récupéré sont des `buffer` : les octets sont
transmis et restitués sans altération, y compris pour un contenu binaire
non textuel.

### Inférence : `AwsBedrockClient`

`AwsBedrockClient` invoque un modèle d'inférence. La session se pose sur
`..Session`, puis `.Invoque(modèle, corps)` envoie le corps `JSON` fourni
et rend la réponse du modèle décodée en `JSON`.

```owl
sess est une AWSSession
sess..Région     = "eu-central-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON
corps.anthropic_version = "bedrock-2023-05-31"
corps.max_tokens = 100

bedrock est un AwsBedrockClient
bedrock..Session = sess

réponse est un JSON = bedrock.Invoque("eu.anthropic.claude-sonnet-4-6", corps)
Trace(réponse.completion)
```

### Exécution de fonctions : `AwsLambdaClient`

`AwsLambdaClient` invoque une fonction distante. `.Invoque(nomFonction,
charge)` transmet la charge `JSON` telle quelle et rend la réponse décodée
en `JSON`.

```owl
sess est une AWSSession
sess..Région     = "eu-central-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

charge est un JSON
charge.action = "ping"
charge.valeur = 42

lambda est un AwsLambdaClient
lambda..Session = sess

réponse est un JSON = lambda.Invoque("ma-fonction", charge)
Trace(réponse.statusCode)
```

Les clients partagent le même mécanisme de signature que l'appel
générique : chaque opération part signée, avec l'empreinte de son corps.

---

## 5. Réponses et erreurs

Quel que soit le chemin (générique ou client dédié), la réponse est un
`JSON` que l'on parcourt au point simple. Deux formes d'échec sont à
distinguer.

### Erreur renvoyée par le service

Quand le service rejette la requête (identifiants refusés, ressource
absente, paramètre invalide…), il répond par un **document d'erreur**, lui
aussi décodé en `JSON`. Cette réponse porte alors un nœud `Error`
contenant `Code` (l'identifiant court de l'erreur) et `Message` (le
libellé). Il suffit d'inspecter la réponse pour le détecter :

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON = ChaîneVersJSON("{}")
réponse est un JSON = AWSRequête(sess, awsIAM, awsIAMListUsers, corps)

SI réponse.Error.Code <> "" ALORS
	Trace("échec AWS : " + réponse.Error.Code + " — " + réponse.Error.Message)
FIN
```

### Échec local avant l'envoi

Quand l'appel ne peut pas être construit (une valeur de chemin requise
manque dans le corps, par exemple), l'échec est **local** : aucune requête
ne part, et `ErreurDétectée` est posé. On le teste immédiatement après
l'appel, avant toute autre instruction.

```owl
sess est une AWSSession
sess..Région     = "us-east-1"
sess..CléAccès   = "AKIAIOSFODNN7EXAMPLE"
sess..CléSecrète = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"

corps est un JSON = ChaîneVersJSON("{}")

ErreurRAZ()
réponse est un JSON = AWSRequête(sess, awsAPIGateway, awsAPIGatewayGetRestApi, corps)
SI ErreurDétectée ALORS
	Trace("requête non construite : " + ErreurInfo())
FIN
```

---

## 6. Hors-périmètre actuel

- **Pas de pagination automatique.** Lorsqu'une liste dépasse une page, la
  réponse porte un **jeton de continuation** ; poursuivre le parcours
  consiste à repasser ce jeton dans le corps de l'appel suivant. Il n'y a
  pas de paginateur qui déroule les pages tout seul.
- **Pas de conversion implicite des dates.** Une valeur de date ou d'heure
  attendue par un service se passe **en chaîne, telle quelle** ; elle n'est
  pas convertie automatiquement depuis un type date.
- **Empreinte de corps : rien à faire.** Certaines écritures exigent un
  checksum du corps. Il est **calculé et posé automatiquement** là où le
  service le réclame : aucune manipulation manuelle n'est nécessaire.

---

## 7. Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Manipuler les `buffer` (corps binaires S3) | [`27_buffers.md`](27_buffers.md) |
| Encodage et entrées/sorties | [`26_encodage_et_io.md`](26_encodage_et_io.md) |
| Gestion des exceptions et `ErreurDétectée` | [`08_exceptions.md`](08_exceptions.md) |
| Interroger un modèle de langage local (`LLM`) | [`35_llm.md`](35_llm.md) |

---

## Aliases anglais

| FR canon | EN canon |
|---|---|
| `AWSRequête` | `AWSRequest` |
| `AWSSession` | `AWSSession` |
| `..Région` | `..Region` |
| `..CléAccès` | `..AccessKey` |
| `..CléSecrète` | `..SecretKey` |
| `..Jeton` | `..Token` |
| `..Endpoint` | `..Endpoint` |
| `.Télécharge` | `.Download` |
| `.Liste` | `.List` |
| `.Supprime` | `.Delete` |
| `.Upload` | `.Upload` |
| `.Invoque` | `.Invoke` |
