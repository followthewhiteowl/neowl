# Messaging

The language ships four broker-oriented messaging clients — **MQTT**,
**AMQP**, **STOMP** and **NATS** — to publish and receive messages through
a message server (the *broker*). All four follow the **same usage model**:
a **session** describes the connection, an **explicit pump** processes
incoming messages, and a callback procedure is called for each message
received. Like the rest of the language, every symbol is **bilingual**
EN/FR: the English canonical forms (`MQTTConnect`, `Body`, `VirtualHost`…)
are equivalent to the French forms and are listed at the end of this page.

## 1. The common model: session and explicit pump

The four clients share the same mechanics. Understanding it once is enough
for all four.

**A session describes the connection.** A composite type (`mqttSession`,
`amqpConnection`, `stompSession`, `natsConnection`) carries the connection
parameters: address, port, credentials, options. Its properties are read and
written with the `..` operator:

```owl
sess is a mqttSession
sess..Address  = "broker.example.local"
sess..Port     = 1883
sess..ClientID = "sensor-livingroom-01"
```

**Opening returns a boolean.** The connection function (`MQTTConnect`,
`AMQPConnect`, `STOMPConnect`, `NATSConnect`) takes the session and reports
whether the connection succeeded. A failure does not raise a blocking error:
it returns `False`, to be tested before going further.

**Receiving goes through an explicit pump.** No message arrives "on its
own" in the background: the code calls a wait function (`MQTTWait`,
`AMQPWait`, `STOMPWait`, `NATSWait`) that, over a maximum delay expressed in
milliseconds, processes incoming messages and **calls the callback
procedure** for each one. This function returns `True` as long as the
connection stays open. A worker that consumes continuously is therefore
written as a loop around this pump:

```owl
sess is a mqttSession
sess..Address  = "broker.example.local"
sess..ClientID = "collector-01"

IF MQTTConnect(sess) THEN
	open is a boolean = True
	WHILE open
		open = MQTTWait(sess, 1000)
	END
	MQTTDisconnect(sess)
END
```

This choice makes the flow **deterministic**: callback procedures run only
during the pump call, on the program's thread, never in parallel. No hidden
thread of execution, no lock to place on the variables shared between the
callback and the rest of the code.

**Closing releases the connection.** The disconnection function
(`MQTTDisconnect`, `AMQPDisconnect`, `STOMPDisconnect`, `NATSDisconnect`)
closes the session cleanly.

## 2. MQTT

MQTT suits the lightweight flow of messages published on hierarchical
**subjects** (topics), typically sensor readings or device states. The
`mqttSession` session carries in particular the `Address`, the `Port`
(`1883` in clear, `8883` encrypted) and a `ClientID` — a client identifier,
which must not be empty.

**Publishing.** `MQTTPublish` sends data on a subject. The short form takes
the session, the subject and the content; two optional arguments complete
it: a *retain* flag (the broker keeps the subject's last message and
delivers it to future subscriptions) and a **quality of service**.

```owl
sess is a mqttSession
sess..Address  = "broker.example.local"
sess..ClientID = "sensor-livingroom-01"

IF MQTTConnect(sess) THEN
	MQTTPublish(sess, "sensors/livingroom/temperature", "21.5")
	MQTTPublish(sess, "sensors/livingroom/state", "active", True, mqttQualityAtLeastOnce)
	MQTTDisconnect(sess)
END
```

Two qualities of service are available:

| Constant | Guarantee |
|-----------|----------|
| `mqttQualityAtMostOnce` | Message sent without acknowledgement: fast, but it may be lost. |
| `mqttQualityAtLeastOnce` | Message retransmitted until the broker acknowledges: delivery guaranteed, duplicates possible. |

**Subscribing and receiving.** `MQTTSubscribe` associates a callback
procedure with a subject, with a quality of service. The procedure receives
the session, the subject involved and the message as a `buffer` (a byte
sequence, which reads as a string in a text context). Subjects accept the
wildcards `+` (one level) and `#` (everything else). The worker then pumps
in a loop:

```owl
PROCEDURE onMessage(_session is a mqttSession, subject is a string, message is a buffer)
	Trace("received on " + subject + " : " + message)
END

sess is a mqttSession
sess..Address  = "broker.example.local"
sess..ClientID = "collector-01"

IF MQTTConnect(sess) THEN
	MQTTSubscribe(sess, "sensors/#", mqttQualityAtLeastOnce, onMessage)
	open is a boolean = True
	WHILE open
		open = MQTTWait(sess, 1000)
	END
	MQTTDisconnect(sess)
END
```

The `_session` parameter carries a `_` prefix because it is not used in this
callback — a convention to signal "intentionally ignored".

**Unsubscribing.** `MQTTUnsubscribe` ends the subscription to one subject;
`MQTTUnsubscribeAll` cuts off all the subscriptions of a session at once.

## 3. AMQP

AMQP targets structured application messaging: durable queues, routing by
**exchange**, fine-grained acknowledgements, throughput control. The
`amqpConnection` connection carries the `Address`, the `Port` (`5672` in
clear, `5671` encrypted), the credentials, the `VirtualHost` (`/` by
default) and reads `MessageCount` — the number of messages pending in the
last declared queue.

**A message carries more than a body.** The `amqpMessage` type transports a
body and a set of properties:

| Property | Role |
|-----------|------|
| `Body` | Message content (string or `buffer` when sending, `buffer` when receiving). |
| `Persistent` | At `True`, the server writes the message to disk (survives a restart). |
| `ContentType` / `ContentEncoding` | MIME type and encoding of the body. |
| `CorrelationId` / `ReplyTo` | Link a reply to its request, in a request/response style. |
| `Priority` | Priority from `0` to `9`. |
| `Expiration` | Message time-to-live, in milliseconds (string). |
| `MessageId` / `Type` | Application-level identifier and type. |
| `Tag` | Delivery identifier, filled in **on reception**, to pass to the acknowledgement. |

**Declaring the topology.** Before exchanging, you declare queues and
exchanges (the operation creates them if needed). `AMQPDeclareQueue` returns
the effective queue name and accepts options: `durable` (survives a
restart), `exclusive`, `autoDelete`, and an associative array of extended
arguments. `AMQPDeclareExchange` takes an exchange type, described by the
`amqpExchangeType` enumeration:

| Value | Routing |
|--------|---------|
| `amqpDirect` | The routing key must equal the binding key. |
| `amqpFanout` | Broadcast to all bound queues, key ignored. |
| `amqpTopic` | Pattern match between routing key and binding keys (wildcards). |
| `amqpHeaders` | Routing based on the message headers. |

`AMQPBind` binds a queue to an exchange via a routing key (`AMQPUnbind`
undoes the binding).

**Routing rejects (dead-letter).** A queue's extended arguments drive
server-side behaviours. By setting `x-dead-letter-exchange` and
`x-message-ttl`, a work queue automatically diverts expired or rejected
messages to a **dead-letter exchange** — useful so no failing message is
lost:

```owl
cnx is an amqpConnection
cnx..Address  = "broker.example.local"
cnx..User     = "app"
cnx..Password = "secret"

IF AMQPConnect(cnx) THEN
	// Dead-letter exchange and its queue: discarded messages land here.
	AMQPDeclareExchange(cnx, "orders.dead", amqpFanout, True)
	AMQPDeclareQueue(cnx, "orders.dead.queue", True)
	AMQPBind(cnx, "orders.dead.queue", "orders.dead", "")

	// Durable work queue, which diverts to the dead-letter exchange
	// after a minute without acknowledgement.
	args is an associative array
	args["x-dead-letter-exchange"] = "orders.dead"
	args["x-message-ttl"] = 60000
	AMQPDeclareQueue(cnx, "orders", True, False, False, args)

	AMQPDisconnect(cnx)
END
```

**Publishing, with acknowledgement.** `AMQPPublish` sends an `amqpMessage`
to an exchange, via a routing key (an empty exchange routes to the queue
named by the key). Enabling `AMQPConfirms` switches the connection to
*publisher-confirms* mode: from then on, `AMQPPublish` waits for the
server's acknowledgement, and its return of `True` guarantees that the
message was actually accepted — not merely sent.

```owl
cnx is an amqpConnection
cnx..Address = "broker.example.local"

IF AMQPConnect(cnx) THEN
	AMQPConfirms(cnx, True)

	msg is an amqpMessage
	msg..Body          = `{"id":42,"action":"ship"}`
	msg..Persistent    = True
	msg..ContentType   = "application/json"
	msg..CorrelationId = "cmd-42"
	msg..Priority      = 5

	IF AMQPPublish(cnx, "", "orders", msg) THEN
		Trace("order accepted by the server")
	ELSE
		Trace("publication refused")
	END

	AMQPDisconnect(cnx)
END
```

**Consuming, with acknowledgement.** `AMQPConsume` associates a consumer
procedure with a queue. Its procedure receives the connection and the
received message (body, properties, delivery identifier). A final argument,
`autoAck`, chooses the acknowledgement mode:

- `autoAck = True` (default): the server considers the message delivered as
  soon as it is sent.
- `autoAck = False`: **manual** acknowledgement. The code confirms
  processing with `AMQPAck` (the message is removed) or gives up with
  `AMQPReject` (with or without requeue). Both take the delivery identifier
  carried by the message.

`AMQPPrefetch` limits the number of unacknowledged messages the server sends
ahead: at `1`, a consumer receives the next message only after
acknowledging the previous one — a fair load share between several workers.

```owl
PROCEDURE onOrder(connection is an amqpConnection, message is an amqpMessage)
	processOrder(message..Body)
	AMQPAck(connection, message..Tag)
END

PROCEDURE processOrder(body is a buffer)
	Trace("processing : " + body)
END

cnx is an amqpConnection
cnx..Address = "broker.example.local"

IF AMQPConnect(cnx) THEN
	AMQPPrefetch(cnx, 10)
	AMQPConsume(cnx, "orders", onOrder, False)

	open is a boolean = True
	WHILE open
		open = AMQPWait(cnx, 1000)
	END

	AMQPDisconnect(cnx)
END
```

**Tearing down a topology.** `AMQPDeleteQueue` deletes a queue,
`AMQPPurgeQueue` empties it by returning the number of messages removed, and
`AMQPDeleteExchange` deletes an exchange.

## 4. STOMP

STOMP is a simple text protocol: you publish and subscribe on named
**destinations**, without declaring any topology on the client side. The
`stompSession` session carries the `Address`, the `Port` (`61613` by
default) and the `VirtualHost`. Destinations follow the broker's convention:
for example `/queue/…` for a point-to-point queue, `/topic/…` for a
broadcast.

**A STOMP message** (`stompMessage`) carries a `Body`, a `Destination`, a
`ContentType` and a `Tag` — the acknowledgement token, filled in on
reception when the subscription is in client-acknowledgement mode.

**Publishing.** `STOMPPublish` sends a `stompMessage` on a destination:

```owl
sess is a stompSession
sess..Address = "broker.example.local"

IF STOMPConnect(sess) THEN
	msg is a stompMessage
	msg..Body        = "order created"
	msg..ContentType = "text/plain"
	STOMPPublish(sess, "/queue/notifications", msg)
	STOMPDisconnect(sess)
END
```

**Subscribing and acknowledging.** `STOMPSubscribe` associates a callback
procedure with a destination. Its `autoAck` argument works as for AMQP: at
`True` (default), messages are acknowledged automatically; at `False`, the
code confirms processing with `STOMPAck` or gives up with `STOMPNack`. Both
functions take the session and **the received message** — the
acknowledgement token travels with it, nothing else to extract:

```owl
PROCEDURE onNotification(session is a stompSession, message is a stompMessage)
	Trace("notification : " + message..Body)
	STOMPAck(session, message)
END

sess is a stompSession
sess..Address     = "broker.example.local"
sess..VirtualHost = "/"

IF STOMPConnect(sess) THEN
	STOMPSubscribe(sess, "/queue/notifications", onNotification, False)
	connected is a boolean = True
	WHILE connected
		connected = STOMPWait(sess, 1000)
	END
	STOMPDisconnect(sess)
END
```

## 5. NATS

NATS is a lightweight text messaging system oriented around **subjects**: a
client publishes on a subject, and the clients subscribed to the same
subject receive the message. This simple publish/subscribe model, with no
topology to declare, makes it a common choice for communication between
microservices — event broadcasting, service calls, task distribution
between workers. The `natsConnection` connection carries the `Address`, the
`Port` (`4222` by default), the credentials (`User`, `Password`) and a
`ConnectionTimeout` in seconds.

**A NATS message** (`natsMessage`) carries a `Subject`, a `Body` and a reply
address `ReplyTo` — the latter is only useful in request/response (see
below). Unlike MQTT, publishing takes a built `natsMessage`, not a raw
string.

**Publishing.** `NATSPublish` sends a `natsMessage` on a subject and returns
a success boolean:

```owl
sess is a natsConnection
sess..Address = "nats.example.local"

IF NATSConnect(sess) THEN
	msg is a natsMessage
	msg..Body = "temperature=21.5"
	NATSPublish(sess, "sensors.livingroom.temperature", msg)
	NATSDisconnect(sess)
END
```

**Subscribing and receiving.** `NATSSubscribe` associates a callback
procedure with a subject. The procedure receives the session and the
received message (`Subject` and `Body` filled in). As with the other
clients, the worker then pumps in a loop with `NATSWait`:

```owl
PROCEDURE onMessage(_session is a natsConnection, message is a natsMessage)
	Trace("received on " + message..Subject + " : " + message..Body)
END

sess is a natsConnection
sess..Address = "nats.example.local"

IF NATSConnect(sess) THEN
	NATSSubscribe(sess, "sensors.livingroom.temperature", onMessage)
	open is a boolean = True
	WHILE open
		open = NATSWait(sess, 1000)
	END
	NATSDisconnect(sess)
END
```

The `_session` parameter carries a `_` prefix because it is not used in this
callback — a convention to signal "intentionally ignored".

**Sharing the load: queue groups.** `NATSSubscribe` accepts a fourth
argument, `queueGroup`. Several subscribers of a **same group** on a **same
subject** **share** the messages: each published message is delivered to
**a single** subscriber of the group, not to all of them. This is the worker
scaling mechanism: you launch several instances of the same service, all in
the same group, and the server spreads the load between them.

```owl
PROCEDURE onTask(_session is a natsConnection, task is a natsMessage)
	process(task..Body)
END

PROCEDURE process(load is a string)
	Trace("processing : " + load)
END

sess is a natsConnection
sess..Address = "nats.example.local"

IF NATSConnect(sess) THEN
	// This subscriber joins the "workers" group: it receives only a
	// share of the subject's messages, the other instances of the group
	// share the rest.
	NATSSubscribe(sess, "tasks.encoding", onTask, "workers")
	open is a boolean = True
	WHILE open
		open = NATSWait(sess, 1000)
	END
	NATSDisconnect(sess)
END
```

Without a `queueGroup`, each subscriber receives **all** the subject's
messages — the classic broadcast. With a shared `queueGroup`, subscribers
split the flow.

### Request/response

NATS is not limited to one-way: `NATSRequest` sends a request on a subject
and **waits for the reply**, like a service call. The function generates a
temporary reply subject, publishes the request with that subject as
`ReplyTo`, then returns the received reply `natsMessage`. A final argument
bounds the wait in milliseconds; when the delay expires, it returns an empty
message and signals an error (`ErrorOccurred`).

**On the requester side:**

```owl
sess is a natsConnection
sess..Address = "nats.example.local"

IF NATSConnect(sess) THEN
	request is a natsMessage
	request..Body = "42"

	response is a natsMessage = NATSRequest(sess, "service.double", request, 3000)
	IF ErrorOccurred THEN
		Trace("the service did not reply in time")
	ELSE
		Trace("reply : " + response..Body)
	END

	NATSDisconnect(sess)
END
```

**On the service side (the responder).** A responder is an ordinary
subscriber that, for each request received, publishes its reply **on the
subject carried by `ReplyTo`** of the received message. It is this subject,
generated by `NATSRequest`, that brings the reply back to the right
requester:

```owl
PROCEDURE onRequest(session is a natsConnection, request is a natsMessage)
	response is a natsMessage
	response..Body = compute(request..Body)
	// Publish to the reply subject provided by the requester.
	NATSPublish(session, request..ReplyTo, response)
END

PROCEDURE compute(input is a string) : string
	RESULT "" + (StringToInteger(input) * 2)
END

sess is a natsConnection
sess..Address = "nats.example.local"

IF NATSConnect(sess) THEN
	NATSSubscribe(sess, "service.double", onRequest)
	open is a boolean = True
	WHILE open
		open = NATSWait(sess, 1000)
	END
	NATSDisconnect(sess)
END
```

The requester and the service generally run in two distinct programs; the
message server links them without their knowing each other.

## 6. Securing the transport (TLS)

The four clients encrypt their connection the same way. Two properties
suffice, common to all sessions:

- `Option = optionSSL` requests an encrypted connection (TLS). The port then
  switches to its secure variant (`8883` for MQTT, `5671` for AMQP, `61614`
  for STOMP); NATS, for its part, negotiates encryption on the **same** port
  (`4222`), with no dedicated port — on the code side, nothing changes, you
  set the same two properties.
- `CACertificate` takes the path of a certificate-authority PEM file. Left
  empty, validation relies on the standard public authorities; filled in, it
  adds a **private** trusted authority — the case of an internal broker
  whose certificate is signed by an in-house CA.

```owl
cnx is an amqpConnection
cnx..Address       = "broker.internal.local"
cnx..Port          = 5671
cnx..Option        = optionSSL
cnx..CACertificate = "/etc/owl/ca-internal.pem"

IF AMQPConnect(cnx) THEN
	Trace("encrypted connection established")
	AMQPDisconnect(cnx)
END
```

Server certificate verification is **strict**: a certification chain that
does not trace back to a trusted authority (public or the one provided by
`CACertificate`) makes the connection fail. For an internal broker, pointing
to its private CA via `CACertificate` is therefore the way to go — never
disable verification.

## French aliases

**Types**

| EN canon | FR canon |
|----------|----------|
| `mqttSession` | `mqttSession` |
| `amqpConnection` | `amqpConnection` |
| `amqpMessage` | `amqpMessage` |
| `amqpExchangeType` | `amqpTypeÉchange` |
| `stompSession` | `stompSession` |
| `stompMessage` | `stompMessage` |
| `natsConnection` | `natsConnexion` |
| `natsMessage` | `natsMessage` |

**Session properties**

| EN canon | FR canon |
|----------|----------|
| `Address` | `Adresse` |
| `Port` | `Port` |
| `ClientID` | `IDClient` |
| `User` | `Utilisateur` |
| `UserPassword` | `MotDePasseUtilisateur` |
| `Password` | `MotDePasse` |
| `VirtualHost` | `HôteVirtuel` |
| `Option` | `Option` |
| `CACertificate` | `CertificatCA` |
| `EmptySession` | `SessionVide` |
| `ConnectionTimeout` | `TimeoutConnexion` |
| `Heartbeat` | `Heartbeat` |
| `MessageCount` | `NbMessages` |

**Message properties**

| EN canon | FR canon |
|----------|----------|
| `Body` | `Corps` |
| `Subject` | `Sujet` |
| `RoutingKey` | `CléRoutage` |
| `Destination` | `Destination` |
| `Persistent` | `Persistant` |
| `ContentType` | `TypeContenu` |
| `ContentEncoding` | `EncodageContenu` |
| `CorrelationId` | `IdCorrélation` |
| `ReplyTo` | `RépondreÀ` |
| `Priority` | `Priorité` |
| `Expiration` | `Expiration` |
| `MessageId` | `IdMessage` |
| `Type` | `Type` |
| `Tag` | `Identifiant` |

**MQTT functions and constants**

| EN canon | FR canon |
|----------|----------|
| `MQTTConnect` | `MQTTConnecte` |
| `MQTTDisconnect` | `MQTTDéconnecte` |
| `MQTTPublish` | `MQTTPublie` |
| `MQTTSubscribe` | `MQTTAbonne` |
| `MQTTUnsubscribe` | `MQTTDésabonne` |
| `MQTTUnsubscribeAll` | `MQTTDésabonneTout` |
| `MQTTWait` | `MQTTAttend` |
| `mqttQualityAtMostOnce` | `mqttQualitéAuPlusUneFois` |
| `mqttQualityAtLeastOnce` | `mqttQualitéAuMoinsUneFois` |
| `optionSSL` | `optionSSL` |

**AMQP functions and constants**

| EN canon | FR canon |
|----------|----------|
| `AMQPConnect` | `AMQPConnecte` |
| `AMQPDisconnect` | `AMQPDéconnecte` |
| `AMQPDeclareQueue` | `AMQPDéclareQueue` |
| `AMQPDeclareExchange` | `AMQPDéclareÉchange` |
| `AMQPBind` | `AMQPLie` |
| `AMQPUnbind` | `AMQPDélie` |
| `AMQPPublish` | `AMQPPublie` |
| `AMQPConsume` | `AMQPConsomme` |
| `AMQPWait` | `AMQPAttend` |
| `AMQPAck` | `AMQPAcquitte` |
| `AMQPReject` | `AMQPRejette` |
| `AMQPPrefetch` | `AMQPPrefetch` |
| `AMQPConfirms` | `AMQPConfirmations` |
| `AMQPDeleteQueue` | `AMQPSupprimeQueue` |
| `AMQPPurgeQueue` | `AMQPPurgeQueue` |
| `AMQPDeleteExchange` | `AMQPSupprimeÉchange` |
| `amqpDirect` | `amqpDirect` |
| `amqpFanout` | `amqpFanout` |
| `amqpTopic` | `amqpTopic` |
| `amqpHeaders` | `amqpHeaders` |

**STOMP functions**

| EN canon | FR canon |
|----------|----------|
| `STOMPConnect` | `STOMPConnecte` |
| `STOMPDisconnect` | `STOMPDéconnecte` |
| `STOMPPublish` | `STOMPPublie` |
| `STOMPSubscribe` | `STOMPAbonne` |
| `STOMPWait` | `STOMPAttend` |
| `STOMPAck` | `STOMPAcquitte` |
| `STOMPNack` | `STOMPRejette` |

**NATS functions**

| EN canon | FR canon |
|----------|----------|
| `NATSConnect` | `NATSConnecte` |
| `NATSDisconnect` | `NATSDéconnecte` |
| `NATSPublish` | `NATSPublie` |
| `NATSSubscribe` | `NATSAbonne` |
| `NATSWait` | `NATSAttend` |
| `NATSRequest` | `NATSRequête` |

See also: [REST services](40_services_rest.md), [The project.owl
file](24_projet.md).
