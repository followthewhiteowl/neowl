# La messagerie

Le langage intègre quatre clients de messagerie orientée courtier — **MQTT**,
**AMQP**, **STOMP** et **NATS** — pour publier et recevoir des messages à
travers un serveur de messages (le *courtier*). Les quatre suivent le **même
modèle d'usage** : une **session** décrit la connexion, une **pompe
explicite** traite les messages entrants, et une procédure de rappel est
appelée pour chaque message reçu. Comme le reste du langage, chaque symbole
est **bilingue** FR/EN : les formes anglaises canoniques (`MQTTConnect`,
`Body`, `VirtualHost`…) sont équivalentes aux formes françaises et sont
listées en fin de fiche.

## 1. Le modèle commun : session et pompe explicite

Les quatre clients partagent la même mécanique. La comprendre une fois suffit
pour les quatre.

**Une session décrit la connexion.** Un type composé (`mqttSession`,
`amqpConnection`, `stompSession`, `natsConnexion`) porte les paramètres de
connexion : adresse, port, identifiants, options. Ses propriétés se lisent et s'écrivent avec
l'opérateur `..` :

```owl
sess est un mqttSession
sess..Adresse  = "broker.exemple.local"
sess..Port     = 1883
sess..IDClient = "capteur-salon-01"
```

**Ouvrir renvoie un booléen.** La fonction de connexion (`MQTTConnecte`,
`AMQPConnecte`, `STOMPConnecte`, `NATSConnecte`) prend la session et indique si
la connexion a réussi. Un échec ne lève pas d'erreur bloquante : il renvoie `Faux`, à
tester avant d'aller plus loin.

**Recevoir passe par une pompe explicite.** Aucun message n'arrive « tout
seul » en tâche de fond : le code appelle une fonction d'attente
(`MQTTAttend`, `AMQPAttend`, `STOMPAttend`, `NATSAttend`) qui, pendant un délai maximal
exprimé en millisecondes, traite les messages entrants et **appelle la
procédure de rappel** pour chacun. Cette fonction renvoie `Vrai` tant que la
connexion reste ouverte. Un travailleur qui consomme en continu s'écrit donc
comme une boucle autour de cette pompe :

```owl
sess est un mqttSession
sess..Adresse  = "broker.exemple.local"
sess..IDClient = "collecteur-01"

SI MQTTConnecte(sess) ALORS
	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = MQTTAttend(sess, 1000)
	FIN
	MQTTDéconnecte(sess)
FIN
```

Ce choix rend le flux **déterministe** : les procédures de rappel s'exécutent
uniquement pendant l'appel à la pompe, sur le fil du programme, jamais en
parallèle. Pas de fil d'exécution caché, pas de verrou à poser sur les
variables partagées entre le rappel et le reste du code.

**Fermer libère la connexion.** La fonction de déconnexion
(`MQTTDéconnecte`, `AMQPDéconnecte`, `STOMPDéconnecte`, `NATSDéconnecte`)
referme proprement la session.

## 2. MQTT

MQTT convient au flux léger de messages publiés sur des **sujets** (topics)
hiérarchiques, typiquement des relevés de capteurs ou des états d'appareils.
La session `mqttSession` porte notamment l'`Adresse`, le `Port` (`1883` en
clair, `8883` chiffré) et un `IDClient` — un identifiant de client, qui ne
doit pas être vide.

**Publier.** `MQTTPublie` envoie des données sur un sujet. La forme courte
prend la session, le sujet et le contenu ; deux arguments optionnels
complètent : `àConserver` (le courtier retient le dernier message du sujet et
le livre aux abonnements futurs) et une **qualité de service**.

```owl
sess est un mqttSession
sess..Adresse  = "broker.exemple.local"
sess..IDClient = "capteur-salon-01"

SI MQTTConnecte(sess) ALORS
	MQTTPublie(sess, "capteurs/salon/temperature", "21.5")
	MQTTPublie(sess, "capteurs/salon/etat", "actif", Vrai, mqttQualitéAuMoinsUneFois)
	MQTTDéconnecte(sess)
FIN
```

Deux qualités de service sont disponibles :

| Constante | Garantie |
|-----------|----------|
| `mqttQualitéAuPlusUneFois` | Message envoyé sans accusé de réception : rapide, mais il peut être perdu. |
| `mqttQualitéAuMoinsUneFois` | Message retransmis jusqu'à l'accusé du courtier : livraison garantie, doublons possibles. |

**S'abonner et recevoir.** `MQTTAbonne` associe une procédure de rappel à un
sujet, avec une qualité de service. La procédure reçoit la session, le sujet
concerné et le message sous forme de `buffer` (séquence d'octets, qui se lit
comme une chaîne dans un contexte texte). Les sujets acceptent les jokers
`+` (un niveau) et `#` (tout le reste). Le travailleur pompe ensuite en
boucle :

```owl
PROCEDURE surMessage(_session est un mqttSession, sujet est une chaîne, message est un buffer)
	Trace("reçu sur " + sujet + " : " + message)
FIN

sess est un mqttSession
sess..Adresse  = "broker.exemple.local"
sess..IDClient = "collecteur-01"

SI MQTTConnecte(sess) ALORS
	MQTTAbonne(sess, "capteurs/#", mqttQualitéAuMoinsUneFois, surMessage)
	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = MQTTAttend(sess, 1000)
	FIN
	MQTTDéconnecte(sess)
FIN
```

Le paramètre `_session` porte un préfixe `_` parce qu'il n'est pas utilisé
dans ce rappel — une convention pour signaler « volontairement ignoré ».

**Se désabonner.** `MQTTDésabonne` met fin à l'abonnement d'un sujet ;
`MQTTDésabonneTout` coupe tous les abonnements d'une session en une fois.

## 3. AMQP

AMQP vise la messagerie applicative structurée : queues durables, routage par
**échange**, accusés fins, contrôle de débit. La connexion `amqpConnection`
porte l'`Adresse`, le `Port` (`5672` en clair, `5671` chiffré), les
identifiants, l'`HôteVirtuel` (`/` par défaut) et lit `NbMessages` — le
nombre de messages en attente dans la dernière queue déclarée.

**Un message porte plus qu'un corps.** Le type `amqpMessage` transporte un
corps et un jeu de propriétés :

| Propriété | Rôle |
|-----------|------|
| `Corps` | Contenu du message (chaîne ou `buffer` en émission, `buffer` en réception). |
| `Persistant` | À `Vrai`, le serveur écrit le message sur disque (survit à un redémarrage). |
| `TypeContenu` / `EncodageContenu` | Type MIME et encodage du corps. |
| `IdCorrélation` / `RépondreÀ` | Relient une réponse à sa demande, façon requête/réponse. |
| `Priorité` | Priorité de `0` à `9`. |
| `Expiration` | Durée de vie du message, en millisecondes (chaîne). |
| `IdMessage` / `Type` | Identifiant et type applicatifs. |
| `Identifiant` | Identifiant de livraison, renseigné **en réception**, à passer à l'acquittement. |

**Déclarer la topologie.** Avant d'échanger, on déclare queues et échanges
(l'opération les crée si besoin). `AMQPDéclareQueue` renvoie le nom effectif
de la queue et accepte des options : `durable` (survit au redémarrage),
`exclusive`, `autoSuppression`, et un tableau associatif d'arguments étendus.
`AMQPDéclareÉchange` prend un type d'échange, décrit par l'énumération
`amqpTypeÉchange` :

| Valeur | Routage |
|--------|---------|
| `amqpDirect` | La clé de routage doit égaler la clé de liaison. |
| `amqpFanout` | Diffusion à toutes les queues liées, clé ignorée. |
| `amqpTopic` | Comparaison à motif entre clé de routage et clés de liaison (jokers). |
| `amqpHeaders` | Routage fondé sur les en-têtes du message. |

`AMQPLie` relie une queue à un échange via une clé de routage (`AMQPDélie`
défait la liaison).

**Router les rebuts (dead-letter).** Les arguments étendus d'une queue
pilotent des comportements de serveur. En posant `x-dead-letter-exchange` et
`x-message-ttl`, une queue de travail déroute automatiquement vers un
**échange de rebut** les messages expirés ou rejetés — utile pour ne perdre
aucun message défaillant :

```owl
cnx est un amqpConnection
cnx..Adresse     = "broker.exemple.local"
cnx..Utilisateur = "app"
cnx..MotDePasse  = "secret"

SI AMQPConnecte(cnx) ALORS
	// Échange de rebut et sa queue : les messages écartés y atterrissent.
	AMQPDéclareÉchange(cnx, "commandes.rebut", amqpFanout, Vrai)
	AMQPDéclareQueue(cnx, "commandes.rebut.file", Vrai)
	AMQPLie(cnx, "commandes.rebut.file", "commandes.rebut", "")

	// Queue de travail durable, qui déroute vers l'échange de rebut
	// après une minute sans acquittement.
	args est un tableau associatif
	args["x-dead-letter-exchange"] = "commandes.rebut"
	args["x-message-ttl"] = 60000
	AMQPDéclareQueue(cnx, "commandes", Vrai, Faux, Faux, args)

	AMQPDéconnecte(cnx)
FIN
```

**Publier, avec accusé.** `AMQPPublie` envoie un `amqpMessage` vers un
échange, via une clé de routage (un échange vide route vers la queue nommée
par la clé). Activer `AMQPConfirmations` bascule la connexion en mode
*publisher-confirms* : dès lors, `AMQPPublie` attend l'accusé du serveur, et
son retour à `Vrai` garantit que le message a réellement été accepté — pas
seulement émis.

```owl
cnx est un amqpConnection
cnx..Adresse = "broker.exemple.local"

SI AMQPConnecte(cnx) ALORS
	AMQPConfirmations(cnx, Vrai)

	msg est un amqpMessage
	msg..Corps         = `{"id":42,"action":"expedier"}`
	msg..Persistant    = Vrai
	msg..TypeContenu   = "application/json"
	msg..IdCorrélation = "cmd-42"
	msg..Priorité      = 5

	SI AMQPPublie(cnx, "", "commandes", msg) ALORS
		Trace("commande acceptée par le serveur")
	SINON
		Trace("publication refusée")
	FIN

	AMQPDéconnecte(cnx)
FIN
```

**Consommer, avec acquittement.** `AMQPConsomme` associe une procédure de
consommation à une queue. Sa procédure reçoit la connexion et le message reçu
(corps, propriétés, identifiant de livraison). Un dernier argument, `autoAck`,
choisit le mode d'acquittement :

- `autoAck = Vrai` (défaut) : le serveur considère le message livré dès
  l'envoi.
- `autoAck = Faux` : acquittement **manuel**. Le code confirme le traitement
  avec `AMQPAcquitte` (le message est retiré) ou renonce avec `AMQPRejette`
  (avec ou sans remise en file). Les deux prennent l'identifiant de livraison
  porté par le message.

`AMQPPrefetch` limite le nombre de messages non acquittés que le serveur
envoie d'avance : à `1`, un consommateur ne reçoit le message suivant
qu'après avoir acquitté le précédent — un partage de charge équitable entre
plusieurs travailleurs.

```owl
PROCEDURE surCommande(connexion est un amqpConnection, message est un amqpMessage)
	traiteCommande(message..Corps)
	AMQPAcquitte(connexion, message..Identifiant)
FIN

PROCEDURE traiteCommande(corps est un buffer)
	Trace("traitement : " + corps)
FIN

cnx est un amqpConnection
cnx..Adresse = "broker.exemple.local"

SI AMQPConnecte(cnx) ALORS
	AMQPPrefetch(cnx, 10)
	AMQPConsomme(cnx, "commandes", surCommande, Faux)

	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = AMQPAttend(cnx, 1000)
	FIN

	AMQPDéconnecte(cnx)
FIN
```

**Défaire une topologie.** `AMQPSupprimeQueue` supprime une queue,
`AMQPPurgeQueue` la vide en renvoyant le nombre de messages retirés, et
`AMQPSupprimeÉchange` supprime un échange.

## 4. STOMP

STOMP est un protocole texte simple : on publie et on s'abonne sur des
**destinations** nommées, sans déclarer de topologie côté client. La session
`stompSession` porte l'`Adresse`, le `Port` (`61613` par défaut) et
l'`HôteVirtuel`. Les destinations suivent la convention du courtier : par
exemple `/queue/…` pour une file point à point, `/topic/…` pour une diffusion.

**Un message STOMP** (`stompMessage`) porte un `Corps`, une `Destination`, un
`TypeContenu` et un `Identifiant` — le jeton d'acquittement, renseigné en
réception quand l'abonnement est en mode d'acquittement client.

**Publier.** `STOMPPublie` envoie un `stompMessage` sur une destination :

```owl
sess est un stompSession
sess..Adresse = "broker.exemple.local"

SI STOMPConnecte(sess) ALORS
	msg est un stompMessage
	msg..Corps       = "commande créée"
	msg..TypeContenu = "text/plain"
	STOMPPublie(sess, "/queue/notifications", msg)
	STOMPDéconnecte(sess)
FIN
```

**S'abonner et acquitter.** `STOMPAbonne` associe une procédure de rappel à
une destination. Son argument `autoAck` fonctionne comme pour AMQP : à `Vrai`
(défaut), les messages sont acquittés automatiquement ; à `Faux`, le code
confirme le traitement avec `STOMPAcquitte` ou renonce avec `STOMPRejette`.
Ces deux fonctions prennent la session et **le message reçu** — le jeton
d'acquittement voyage avec lui, rien d'autre à extraire :

```owl
PROCEDURE surNotification(session est un stompSession, message est un stompMessage)
	Trace("notification : " + message..Corps)
	STOMPAcquitte(session, message)
FIN

sess est un stompSession
sess..Adresse     = "broker.exemple.local"
sess..HôteVirtuel = "/"

SI STOMPConnecte(sess) ALORS
	STOMPAbonne(sess, "/queue/notifications", surNotification, Faux)
	connectée est un booléen = Vrai
	TANTQUE connectée
		connectée = STOMPAttend(sess, 1000)
	FIN
	STOMPDéconnecte(sess)
FIN
```

## 5. NATS

NATS est une messagerie texte légère orientée **sujets** : un client publie
sur un sujet, les clients abonnés au même sujet reçoivent le message. Ce
modèle publication/abonnement simple, sans topologie à déclarer, en fait un
choix courant pour la communication entre microservices — diffusion
d'événements, appels de service, répartition de tâches entre travailleurs. La
connexion `natsConnexion` porte l'`Adresse`, le `Port` (`4222` par défaut),
les identifiants (`Utilisateur`, `MotDePasse`) et un `TimeoutConnexion` en
secondes.

**Un message NATS** (`natsMessage`) porte un `Sujet`, un `Corps` et une
adresse de réponse `RépondreÀ` — cette dernière n'est utile qu'en
requête/réponse (voir plus bas). Contrairement à MQTT, la publication prend un
`natsMessage` construit, pas une chaîne brute.

**Publier.** `NATSPublie` envoie un `natsMessage` sur un sujet et renvoie un
booléen de réussite :

```owl
sess est un natsConnexion
sess..Adresse = "nats.exemple.local"

SI NATSConnecte(sess) ALORS
	msg est un natsMessage
	msg..Corps = "temperature=21.5"
	NATSPublie(sess, "capteurs.salon.temperature", msg)
	NATSDéconnecte(sess)
FIN
```

**S'abonner et recevoir.** `NATSAbonne` associe une procédure de rappel à un
sujet. La procédure reçoit la session et le message reçu (`Sujet` et `Corps`
renseignés). Comme pour les autres clients, le travailleur pompe ensuite en
boucle avec `NATSAttend` :

```owl
PROCEDURE surMessage(_session est un natsConnexion, message est un natsMessage)
	Trace("reçu sur " + message..Sujet + " : " + message..Corps)
FIN

sess est un natsConnexion
sess..Adresse = "nats.exemple.local"

SI NATSConnecte(sess) ALORS
	NATSAbonne(sess, "capteurs.salon.temperature", surMessage)
	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = NATSAttend(sess, 1000)
	FIN
	NATSDéconnecte(sess)
FIN
```

Le paramètre `_session` porte un préfixe `_` parce qu'il n'est pas utilisé
dans ce rappel — une convention pour signaler « volontairement ignoré ».

**Répartir la charge : les groupes de file (queue groups).** `NATSAbonne`
accepte un quatrième argument, `groupeFile`. Plusieurs abonnés d'un **même
groupe** sur un **même sujet** se **partagent** les messages : chaque message
publié est livré à **un seul** des abonnés du groupe, pas à tous. C'est le
mécanisme de mise à l'échelle des travailleurs : on lance plusieurs instances
du même service, toutes dans le même groupe, et le serveur répartit la charge
entre elles.

```owl
PROCEDURE surTâche(_session est un natsConnexion, tâche est un natsMessage)
	traite(tâche..Corps)
FIN

PROCEDURE traite(charge est une chaîne)
	Trace("traitement de : " + charge)
FIN

sess est un natsConnexion
sess..Adresse = "nats.exemple.local"

SI NATSConnecte(sess) ALORS
	// Cet abonné rejoint le groupe « travailleurs » : il ne reçoit qu'une
	// part des messages du sujet, les autres instances du groupe se
	// partagent le reste.
	NATSAbonne(sess, "tâches.encodage", surTâche, "travailleurs")
	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = NATSAttend(sess, 1000)
	FIN
	NATSDéconnecte(sess)
FIN
```

Sans `groupeFile`, chaque abonné reçoit **tous** les messages du sujet — la
diffusion classique. Avec un `groupeFile` partagé, les abonnés se répartissent
le flux.

### Requête/réponse

NATS ne se limite pas au sens unique : `NATSRequête` envoie une requête sur un
sujet et **attend la réponse**, comme un appel de service. La fonction génère
un sujet de réponse temporaire, publie la requête avec ce sujet en `RépondreÀ`,
puis renvoie le `natsMessage` de réponse reçu. Un dernier argument borne
l'attente en millisecondes ; à l'expiration du délai, elle renvoie un message
vide et signale une erreur (`ErreurDétectée`).

**Côté demandeur :**

```owl
sess est un natsConnexion
sess..Adresse = "nats.exemple.local"

SI NATSConnecte(sess) ALORS
	requête est un natsMessage
	requête..Corps = "42"

	réponse est un natsMessage = NATSRequête(sess, "service.doublement", requête, 3000)
	SI ErreurDétectée ALORS
		Trace("le service n'a pas répondu à temps")
	SINON
		Trace("réponse : " + réponse..Corps)
	FIN

	NATSDéconnecte(sess)
FIN
```

**Côté service (le répondeur).** Un répondeur est un abonné ordinaire qui, pour
chaque requête reçue, publie sa réponse **sur le sujet porté par `RépondreÀ`**
du message reçu. C'est ce sujet, généré par `NATSRequête`, qui ramène la
réponse au bon demandeur :

```owl
PROCEDURE surRequête(session est un natsConnexion, requête est un natsMessage)
	réponse est un natsMessage
	réponse..Corps = calcule(requête..Corps)
	// Publie vers le sujet de réponse fourni par le demandeur.
	NATSPublie(session, requête..RépondreÀ, réponse)
FIN

PROCEDURE calcule(entrée est une chaîne) : chaîne
	RENVOYER "" + (ChaîneVersEntier(entrée) * 2)
FIN

sess est un natsConnexion
sess..Adresse = "nats.exemple.local"

SI NATSConnecte(sess) ALORS
	NATSAbonne(sess, "service.doublement", surRequête)
	ouverte est un booléen = Vrai
	TANTQUE ouverte
		ouverte = NATSAttend(sess, 1000)
	FIN
	NATSDéconnecte(sess)
FIN
```

Le demandeur et le service tournent en général dans deux programmes distincts ;
le serveur de messages les relie sans qu'ils se connaissent.

## 6. Sécuriser le transport (TLS)

Les quatre clients chiffrent leur connexion de la même façon. Deux propriétés
suffisent, communes à toutes les sessions :

- `Option = optionSSL` demande une connexion chiffrée (TLS). Le port bascule
  alors sur sa variante sécurisée (`8883` en MQTT, `5671` en AMQP, `61614` en
  STOMP) ; NATS, lui, négocie le chiffrement sur le **même** port (`4222`),
  sans port dédié — côté code, rien ne change, on pose les deux mêmes
  propriétés.
- `CertificatCA` reçoit le chemin d'un fichier PEM d'autorité de
  certification. Laissée vide, la validation s'appuie sur les autorités
  publiques standards ; renseignée, elle ajoute une autorité **privée** de
  confiance — le cas d'un courtier interne dont le certificat est signé par
  une CA maison.

```owl
cnx est un amqpConnection
cnx..Adresse      = "broker.interne.local"
cnx..Port         = 5671
cnx..Option       = optionSSL
cnx..CertificatCA = "/etc/owl/ca-interne.pem"

SI AMQPConnecte(cnx) ALORS
	Trace("connexion chiffrée établie")
	AMQPDéconnecte(cnx)
FIN
```

La vérification du certificat serveur est **stricte** : une chaîne de
certification qui ne remonte pas à une autorité de confiance (publique ou
celle fournie par `CertificatCA`) fait échouer la connexion. Pour un courtier
interne, désigner sa CA privée via `CertificatCA` est donc la marche à
suivre — jamais désactiver la vérification.

## Aliases anglais

**Types**

| FR canon | EN canon |
|----------|----------|
| `mqttSession` | `mqttSession` |
| `amqpConnection` | `amqpConnection` |
| `amqpMessage` | `amqpMessage` |
| `amqpTypeÉchange` | `amqpExchangeType` |
| `stompSession` | `stompSession` |
| `stompMessage` | `stompMessage` |
| `natsConnexion` | `natsConnection` |
| `natsMessage` | `natsMessage` |

**Propriétés de session**

| FR canon | EN canon |
|----------|----------|
| `Adresse` | `Address` |
| `Port` | `Port` |
| `IDClient` | `ClientID` |
| `Utilisateur` | `User` |
| `MotDePasseUtilisateur` | `UserPassword` |
| `MotDePasse` | `Password` |
| `HôteVirtuel` | `VirtualHost` |
| `Option` | `Option` |
| `CertificatCA` | `CACertificate` |
| `SessionVide` | `EmptySession` |
| `TimeoutConnexion` | `ConnectionTimeout` |
| `Heartbeat` | `Heartbeat` |
| `NbMessages` | `MessageCount` |

**Propriétés de message**

| FR canon | EN canon |
|----------|----------|
| `Corps` | `Body` |
| `Sujet` | `Subject` |
| `CléRoutage` | `RoutingKey` |
| `Destination` | `Destination` |
| `Persistant` | `Persistent` |
| `TypeContenu` | `ContentType` |
| `EncodageContenu` | `ContentEncoding` |
| `IdCorrélation` | `CorrelationId` |
| `RépondreÀ` | `ReplyTo` |
| `Priorité` | `Priority` |
| `Expiration` | `Expiration` |
| `IdMessage` | `MessageId` |
| `Type` | `Type` |
| `Identifiant` | `Tag` |

**Fonctions et constantes MQTT**

| FR canon | EN canon |
|----------|----------|
| `MQTTConnecte` | `MQTTConnect` |
| `MQTTDéconnecte` | `MQTTDisconnect` |
| `MQTTPublie` | `MQTTPublish` |
| `MQTTAbonne` | `MQTTSubscribe` |
| `MQTTDésabonne` | `MQTTUnsubscribe` |
| `MQTTDésabonneTout` | `MQTTUnsubscribeAll` |
| `MQTTAttend` | `MQTTWait` |
| `mqttQualitéAuPlusUneFois` | `mqttQualityAtMostOnce` |
| `mqttQualitéAuMoinsUneFois` | `mqttQualityAtLeastOnce` |
| `optionSSL` | `optionSSL` |

**Fonctions et constantes AMQP**

| FR canon | EN canon |
|----------|----------|
| `AMQPConnecte` | `AMQPConnect` |
| `AMQPDéconnecte` | `AMQPDisconnect` |
| `AMQPDéclareQueue` | `AMQPDeclareQueue` |
| `AMQPDéclareÉchange` | `AMQPDeclareExchange` |
| `AMQPLie` | `AMQPBind` |
| `AMQPDélie` | `AMQPUnbind` |
| `AMQPPublie` | `AMQPPublish` |
| `AMQPConsomme` | `AMQPConsume` |
| `AMQPAttend` | `AMQPWait` |
| `AMQPAcquitte` | `AMQPAck` |
| `AMQPRejette` | `AMQPReject` |
| `AMQPPrefetch` | `AMQPPrefetch` |
| `AMQPConfirmations` | `AMQPConfirms` |
| `AMQPSupprimeQueue` | `AMQPDeleteQueue` |
| `AMQPPurgeQueue` | `AMQPPurgeQueue` |
| `AMQPSupprimeÉchange` | `AMQPDeleteExchange` |
| `amqpDirect` | `amqpDirect` |
| `amqpFanout` | `amqpFanout` |
| `amqpTopic` | `amqpTopic` |
| `amqpHeaders` | `amqpHeaders` |

**Fonctions STOMP**

| FR canon | EN canon |
|----------|----------|
| `STOMPConnecte` | `STOMPConnect` |
| `STOMPDéconnecte` | `STOMPDisconnect` |
| `STOMPPublie` | `STOMPPublish` |
| `STOMPAbonne` | `STOMPSubscribe` |
| `STOMPAttend` | `STOMPWait` |
| `STOMPAcquitte` | `STOMPAck` |
| `STOMPRejette` | `STOMPNack` |

**Fonctions NATS**

| FR canon | EN canon |
|----------|----------|
| `NATSConnecte` | `NATSConnect` |
| `NATSDéconnecte` | `NATSDisconnect` |
| `NATSPublie` | `NATSPublish` |
| `NATSAbonne` | `NATSSubscribe` |
| `NATSAttend` | `NATSWait` |
| `NATSRequête` | `NATSRequest` |

Voir aussi : [Les services REST](40_services_rest.md), [Le fichier
projet.owl](24_projet.md).
