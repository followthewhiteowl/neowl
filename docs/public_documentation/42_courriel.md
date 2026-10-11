# Le courriel

Le langage intègre un framework de **courrier électronique** complet :
décrire un message, l'envoyer par un serveur d'envoi **SMTP**, le sérialiser
en fichier `.eml` et le relire, ou importer un message `.msg` produit par un
client de messagerie de bureau. Tout tourne autour d'un type central,
`Email`, qui porte à la fois le message **à envoyer** et le message **relu**.
Comme le reste du langage, chaque symbole est **bilingue** FR/EN : les formes
anglaises canoniques (`EmailSendMessage`, `Sender`, `Recipient`…) sont
équivalentes aux formes françaises et sont listées en fin de fiche.

## 1. Le modèle : trois types composés

Trois types composés décrivent un message et son transport. Leurs propriétés
se lisent et s'écrivent avec l'opérateur `..`.

**`Email` décrit le message.** C'est le type pivot : il porte l'expéditeur,
les listes de destinataires, le sujet, les corps texte et HTML, les pièces
jointes et un jeu d'options (priorité, confidentialité, accusés). Les mêmes
champs servent à l'envoi et à la lecture d'un message importé.

```owl
msg est un Email
msg..Expéditeur = "alice@exemple.org"
msg..Sujet      = "Compte-rendu de réunion"
msg..Message    = "Bonjour, voici le compte-rendu en texte brut."
msg..HTML       = "<p>Bonjour, voici le compte-rendu en <b>HTML</b>.</p>"
Ajoute(msg..Destinataire, "bob@exemple.org")
Ajoute(msg..Cc, "chef.equipe@exemple.org")
```

Les destinataires se répartissent sur trois listes de chaînes :
`Destinataire` (destinataires principaux, en-tête `To`), `Cc` (copie visible)
et `Cci` (copie cachée). La priorité et la confidentialité se posent avec des
constantes dédiées :

| Propriété | Constantes | Défaut |
|-----------|------------|--------|
| `Priorité` | `emailPrioritéBasse`, `emailPrioritéNormale`, `emailPrioritéHaute` | `emailPrioritéNormale` |
| `Confidentialité` | `emailNormal`, `emailConfidentiel`, `emailPersonnel`, `emailPrivé` | `emailNormal` |

Deux booléens, `AccuséRéception` et `ConfirmationLecture` (tous deux `Faux`
par défaut), demandent respectivement un accusé de remise et une confirmation
de lecture au destinataire. La propriété `Source` porte le message **brut**
(en-têtes et corps MIME) ; elle est le support de l'aller-retour sérialisation
décrit plus bas.

**`emailSessionSMTP` décrit le serveur d'envoi.** Une session porte l'adresse
du serveur, le port, les identifiants d'authentification et les options de
chiffrement :

```owl
session est un emailSessionSMTP
session..AdresseServeur = "smtp.exemple.org"
session..Port          = 587
session..Nom           = "compte@exemple.org"
session..MotDePasse    = "secret"
```

Le port vaut `25` par défaut. `Nom` et `MotDePasse`, laissés vides, ouvrent
une session **non authentifiée** ; renseignés, ils déclenchent une
authentification à l'ouverture de la session.

**`emailAttache` décrit une pièce jointe.** Chaque pièce porte un contenu
binaire (`Contenu`, un `Buffer`), son type MIME (`ContentType`), son nom de
fichier (`Nom`) et, pour une image référencée depuis le corps HTML, une
description et un identifiant de contenu :

```owl
piece est un emailAttache
piece..Nom         = "rapport.pdf"
piece..ContentType = "application/pdf"
piece..Contenu     = ChaîneVersBuffer("contenu binaire du rapport")
```

Les pièces jointes d'un message vivent dans le tableau `Email..Attache`.

**`emailEntête` décrit un en-tête personnalisé**, avec un `Nom` et une `Valeur`. Les
en-têtes non standard (par exemple `X-Origine`) vivent dans le tableau `Email..Entête` :
ils sont ajoutés au message à l'envoi, et à la lecture les en-têtes non reconnus y sont
déposés (les en-têtes standard restent lus dans leurs propriétés dédiées).

## 2. Envoyer un message

L'envoi suit trois temps : **ouvrir** une session vers le serveur, **envoyer**
un ou plusieurs messages, **fermer** la session. Chaque fonction renvoie un
booléen (sauf la fermeture) : un échec ne lève pas d'erreur bloquante, il
renvoie `Faux` et positionne `ErreurDétectée`, à tester avant d'aller plus
loin.

- `EmailOuvreSession(session)` établit la connexion, négocie le chiffrement
  éventuel et s'authentifie si des identifiants sont renseignés. Renvoie
  `Vrai` si la session est ouverte.
- `EmailEnvoieMessage(session, email)` envoie un `Email` au travers d'une
  session déjà ouverte. Le message brut est construit à la volée à partir des
  champs de l'`Email`.
- `EmailFermeSession(session)` referme la connexion et libère les ressources.

`EmailChangeTimeOut(session, délai)` fixe, avant l'ouverture, le délai maximal
d'attente du serveur en millisecondes : passé ce délai sans réponse,
l'ouverture échoue proprement plutôt que de bloquer indéfiniment.

L'exemple complet ci-dessous compose un message à deux corps (texte et HTML),
lui joint un fichier local avec `EmailChargeFichierAttaché`, puis l'envoie :

```owl
session est un emailSessionSMTP
session..AdresseServeur = "smtp.exemple.org"
session..Port          = 587
session..Nom           = "compte@exemple.org"
session..MotDePasse    = "secret"
EmailChangeTimeOut(session, 5000)

msg est un Email
msg..Expéditeur = "compte@exemple.org"
msg..Sujet      = "Rapport mensuel"
msg..Message    = "Bonjour, le rapport du mois est en pièce jointe."
msg..HTML       = "<p>Bonjour,</p><p>Le rapport du mois est en pièce jointe.</p>"
msg..Priorité   = emailPrioritéHaute
Ajoute(msg..Destinataire, "bob@exemple.org")
Ajoute(msg..Cc, "archive@exemple.org")
EmailChargeFichierAttaché(msg, "rapports/mensuel.pdf")

SI EmailOuvreSession(session) ALORS
	SI EmailEnvoieMessage(session, msg) ALORS
		Trace("message accepté par le serveur d'envoi")
	SINON
		Trace("l'envoi a échoué")
	FIN
	EmailFermeSession(session)
SINON
	Trace("connexion au serveur d'envoi impossible")
FIN
```

`EmailChargeFichierAttaché(email, chemin)` lit le fichier désigné, déduit son
type MIME de l'extension et ajoute une `emailAttache` au message ; il renvoie
`Faux` si le fichier est introuvable, sans rien ajouter.

`EmailEnvoieMessage` accepte un troisième argument optionnel : un jeu
d'**options de construction** du message, combinables avec l'opérateur `+`
(voir la section suivante et la sérialisation).

## 3. Chiffrer le transport (TLS)

La propriété `Option` de la session choisit le mode de connexion. Trois
valeurs sont disponibles :

| Constante | Connexion | Port standard |
|-----------|-----------|---------------|
| `emailOptionDéfaut` | En clair, sans chiffrement. | `25` |
| `emailOptionSécuriséTLS` | Démarre en clair puis bascule en TLS (STARTTLS). | `587` |
| `optionSSL` | Chiffrée dès l'ouverture (TLS direct). | `465` |

La propriété `CertificatCA` reçoit le chemin d'un fichier PEM d'autorité de
certification. Laissée vide, la validation du certificat serveur s'appuie sur
les autorités publiques standards ; renseignée, elle ajoute une autorité
**privée** de confiance — le cas d'un serveur d'envoi interne dont le
certificat est signé par une autorité maison.

```owl
session est un emailSessionSMTP
session..AdresseServeur = "smtp.interne.local"
session..Port          = 465
session..Option        = optionSSL
session..CertificatCA  = "/etc/owl/ca-interne.pem"
session..Nom           = "service@interne.local"
session..MotDePasse    = "secret"

SI EmailOuvreSession(session) ALORS
	Trace("session chiffrée ouverte")
	EmailFermeSession(session)
FIN
```

La vérification du certificat serveur est **stricte** : une chaîne de
certification qui ne remonte pas à une autorité de confiance fait échouer
l'ouverture. Pour un serveur interne, désigner son autorité privée via
`CertificatCA` est la marche à suivre — jamais désactiver la vérification.

## 4. Enregistrer et relire un message (`.eml`)

Un `Email` se sérialise en message brut standard, celui d'un fichier `.eml`.
Deux fonctions font l'aller-retour, via la propriété `Source` :

- `EmailConstruitSource(email)` fabrique le message brut (en-têtes et corps
  MIME) à partir des champs de l'`Email` et le dépose dans `email..Source`.
- `EmailImporteSource(email)` fait l'inverse : il analyse le message brut
  présent dans `email..Source` et en remplit les champs (expéditeur,
  destinataires, sujet, corps texte et HTML, pièces jointes).

Combinées aux fonctions de fichier `fSauveTexte` et `fChargeTexte`, elles
écrivent et relisent un fichier `.eml` :

```owl
msg est un Email
msg..Expéditeur = "alice@exemple.org"
msg..Sujet      = "Compte-rendu"
msg..Message    = "Voici le compte-rendu de la réunion."
Ajoute(msg..Destinataire, "bob@exemple.org")

// Construire le message brut, puis l'écrire sur disque.
SI EmailConstruitSource(msg) ALORS
	fSauveTexte("archive/compte-rendu.eml", msg..Source)
FIN

// Plus tard : relire le fichier et repeupler un Email.
relu est un Email
relu..Source = fChargeTexte("archive/compte-rendu.eml")
SI EmailImporteSource(relu) ALORS
	Trace("sujet relu : " + relu..Sujet)
FIN
```

Le corps produit dépend du message : texte seul (`text/plain`), texte et HTML
(`multipart/alternative`) ou message avec pièces jointes (`multipart/mixed`,
parties encodées en base64). L'analyse au retour restitue chaque partie, y
compris le contenu binaire des pièces jointes.

`EmailConstruitSource` accepte un second argument optionnel d'options de
construction, combinables avec l'opérateur `+` :

| Constante | Effet |
|-----------|-------|
| `emailOptionEncodeEntête` | Encode les en-têtes contenant des caractères non ASCII (mots-encodés). Sans l'option, un sujet accentué reste en clair. |
| `emailOptionEntêteCCi` | Fait apparaître les destinataires en copie cachée dans un en-tête dédié du message (masqués par défaut). |

```owl
msg est un Email
msg..Expéditeur = "alice@exemple.org"
msg..Sujet      = "Réunion budgétaire"
msg..Message    = "Merci de confirmer votre présence."
Ajoute(msg..Destinataire, "bob@exemple.org")

EmailConstruitSource(msg, emailOptionEncodeEntête)
```

`EmailImporteSource` décode ces mots-encodés au retour : le sujet accentué est
restitué à l'identique.

## 5. Importer un message Outlook (`.msg`)

`EmailImporteMSG(email, chemin)` lit un fichier `.msg` — le format de message
enregistré par un client de messagerie de bureau tel qu'Outlook — et en
remplit un `Email` : expéditeur, destinataires principaux et en copie, sujet,
corps texte et HTML, pièces jointes. La lecture est **en lecture seule** :
l'import ne modifie pas le fichier d'origine.

```owl
recu est un Email
SI EmailImporteMSG(recu, "boite/message.msg") ALORS
	Trace("de : " + recu..Expéditeur)
	Trace("sujet : " + recu..Sujet)
	POUR TOUT piece DE recu..Attache
		Trace("pièce jointe : " + piece..Nom)
	FIN
SINON
	Trace("import impossible : fichier absent ou format non reconnu")
FIN
```

La fonction renvoie `Faux` et positionne `ErreurDétectée` si le fichier est
introuvable ou n'est pas un conteneur de message valide, sans planter.

## Aliases anglais

**Types**

| FR canon | EN canon |
|----------|----------|
| `Email` | `Email` |
| `emailSessionSMTP` | `emailSMTPSession` |
| `emailAttache` | `emailAttach` |
| `emailEntête` | `emailHeader` |

**Propriétés de `Email`**

| FR canon | EN canon |
|----------|----------|
| `Expéditeur` | `Sender` |
| `AdresseExpéditeur` | `SenderAddress` |
| `AdresseRetourErreur` | `ErrorReturnAddress` |
| `Destinataire` | `Recipient` |
| `Cc` | `Cc` |
| `Cci` | `Bcc` |
| `Sujet` | `Subject` |
| `Message` | `Message` |
| `HTML` | `HTML` |
| `Attache` | `Attachment` |
| `Catégorie` | `Category` |
| `Priorité` | `Priority` |
| `Confidentialité` | `Confidentiality` |
| `AccuséRéception` | `AcknowledgeReceipt` |
| `ConfirmationLecture` | `ReadReceipt` |
| `MessageID` | `MessageID` |
| `Référence` | `Reference` |
| `Source` | `Source` |
| `Entête` | `Header` |

**Propriétés de `emailSessionSMTP`**

| FR canon | EN canon |
|----------|----------|
| `AdresseServeur` | `ServerAddress` |
| `Nom` | `Name` |
| `MotDePasse` | `Password` |
| `Port` | `Port` |
| `Option` | `Option` |
| `CertificatCA` | `CACertificate` |

**Propriétés de `emailAttache`**

| FR canon | EN canon |
|----------|----------|
| `Contenu` | `Content` |
| `ContentType` | `ContentType` |
| `Nom` | `Name` |
| `ContentDescription` | `ContentDescription` |
| `Identifiant` | `Identifier` |

**Propriétés de `emailEntête`**

| FR canon | EN canon |
|----------|----------|
| `Nom` | `Name` |
| `Valeur` | `Value` |

**Fonctions**

| FR canon | EN canon |
|----------|----------|
| `EmailOuvreSession` | `EmailStartSession` |
| `EmailEnvoieMessage` | `EmailSendMessage` |
| `EmailFermeSession` | `EmailCloseSession` |
| `EmailChangeTimeOut` | `EmailSetTimeOut` |
| `EmailChargeFichierAttaché` | `EmailLoadAttachment` |
| `EmailConstruitSource` | `EmailBuildSource` |
| `EmailImporteSource` | `EmailImportSource` |
| `EmailImporteMSG` | `EmailImportMSG` |

**Constantes**

| FR canon | EN canon |
|----------|----------|
| `emailOptionDéfaut` | `emailOptionDefault` |
| `emailOptionSécuriséTLS` | `emailOptionSecureTLS` |
| `optionSSL` | `optionSSL` |
| `emailOptionEncodeEntête` | `emailOptionEncodeHeader` |
| `emailOptionEntêteCCi` | `emailOptionHeaderBcc` |
| `emailPrioritéBasse` | `emailPriorityLow` |
| `emailPrioritéNormale` | `emailPriorityNormal` |
| `emailPrioritéHaute` | `emailPriorityHigh` |
| `emailNormal` | `emailNormal` |
| `emailConfidentiel` | `emailConfidential` |
| `emailPersonnel` | `emailPersonal` |
| `emailPrivé` | `emailPrivate` |

Voir aussi : [La messagerie](41_messagerie.md), [Les services
REST](40_services_rest.md).
</content>
</invoke>
