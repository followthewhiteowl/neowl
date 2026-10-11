# Le modèle SQL

Une couche SQL minimale et sûre par construction est disponible, avec
**huit backends** accessibles via la même API :

- `sqlSqlite` — moteur SQLite embarqué (fichier ou `:memory:`)
- `sqlDuckDB` — moteur analytique embarqué (OLAP, Parquet, JSON)
- `sqlPostgres` — serveur PostgreSQL via protocole natif
- `sqlMySQL` — serveur MySQL / MariaDB via protocole natif
- `sqlMSSQL` — serveur Microsoft SQL Server via TDS natif
- `sqlHFSQL` — base SQL client/serveur, driver embarqué (zéro install)
- `sqlHFSQLClassic` — base locale en fichiers, sans serveur (pilote et moteur embarqués)
- `sqlODBC` — n'importe quel SGBD via un driver ODBC tiers

L'API se réduit à trois types et trois fonctions. Pas d'ORM, pas de
générateur de schéma, pas de DSL de requête : on écrit du SQL,
l'exécution et la sécurité du binding sont prises en charge.

Les détails de version, d'authentification et de TLS par backend sont
dans `14_sql_connecteurs.md`.

---

## 1. Les trois types

| Type | Rôle |
|---|---|
| `sqlConnexion` | Descripteur de connexion (provider, source, identifiants…) + handle interne |
| `sqlRequête` | Texte SQL avec placeholders `{nom}` + tableau associatif `Paramètres` |
| `sqlRéponse` | Résultat d'exécution : succès, lignes retournées, lignes affectées |

Chaque propriété est disponible en français et en anglais, les deux
formes sont interchangeables ligne par ligne.

### `sqlConnexion`

| Field FR | Field EN | Type | Note |
|---|---|---|---|
| `Provider` | `Provider` | `entier` | `sqlSqlite`, `sqlDuckDB`, `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlHFSQLClassic`, `sqlODBC` |
| `Source` | `Source` | `chaîne` | Fichier SQLite/DuckDB, ou connection string ODBC complète |
| `Serveur` | `Server` | `chaîne` | Host des backends réseau (`host` ou `host:port`) |
| `Port` | `Port` | `entier` | Override explicite (0 = port par défaut du backend) |
| `BaseDeDonnées` | `Database` | `chaîne` | Nom de la base |
| `Utilisateur` | `User` | `chaîne` | Identifiant |
| `MotDePasse` | `Password` | `chaîne` | Mot de passe |
| `Pilote` | `Driver` | `chaîne` | Nom du driver ODBC enregistré (pour `sqlODBC` uniquement) |
| `Analyse` | `Analysis` | `chaîne` | Fichier d'analyse (schéma) d'une base locale en fichiers (pour `sqlHFSQLClassic` uniquement) |
| `Répertoire` | `Directory` | `chaîne` | Dossier des fichiers de données d'une base locale (pour `sqlHFSQLClassic` uniquement) |
| `ModeTLS` | `TLSMode` | `entier` | `tlsDesactive`, `tlsAccepteTout`, `tlsVerifieCA`, `tlsVerifieComplet` (défaut) |
| `CertificatCA` | `CACertificate` | `chaîne` | Chemin d'un fichier PEM, CA interne à utiliser comme racine |
| `TimeoutCnx` | `ConnectTimeout` | `entier` | Délai d'ouverture **en secondes** (0 = 5 s par défaut), pris en compte par `sqlODBC`, `sqlHFSQL`, `sqlHFSQLClassic` et `sqlMSSQL` |

### `sqlRequête`

| Field FR | Field EN | Type |
|---|---|---|
| `Code` | `Code` | `chaîne` — SQL avec placeholders `{nom}` |
| `Paramètres` | `Parameters` | `tableau associatif` |

### `sqlRéponse`

| Field FR | Field EN | Type |
|---|---|---|
| `Succès` | `Success` | `booléen` |
| `Lignes` | `Rows` | `tableau` de `tableau associatif` |
| `Occurrence` | `Count` | `entier` — nombre de lignes retournées |
| `LignesAffectées` | `AffectedRows` | `entier` — nombre de lignes créées, modifiées ou supprimées |

Chaque champ décrit **la requête qui vient d'être envoyée**, jamais l'historique de la
connexion. Une lecture rapporte donc `LignesAffectées` à 0, et une écriture rapporte
`Lignes` vide et `Occurrence` à 0. Ces valeurs sont identiques quel que soit le connecteur :
un même programme rend les mêmes chiffres sur SQLite comme sur PostgreSQL.

Les deux mesures restent **indépendantes** : `Lignes` et `Occurrence` décrivent ce que la
requête a *rendu*, `LignesAffectées` ce qu'elle a *écrit*. Une requête qui fait les deux —
une écriture assortie d'une clause de retour, `RETURNING` ou `OUTPUT` — renseigne les deux
sur `sqlSqlite`, `sqlDuckDB` et `sqlPostgres` : aucune des deux mesures n'y retombe à 0 au
motif que l'autre est renseignée.

> **La garantie s'arrête à ces trois connecteurs.** Sur `sqlMySQL` (MariaDB), `sqlMSSQL` et
> `sqlODBC`, une écriture assortie d'une clause de retour rend bien ses lignes dans `Lignes`
> et `Occurrence`, mais `LignesAffectées` y vaut 0 : le protocole de ces connecteurs ne
> permet pas de distinguer cette forme d'une lecture. Compter les lignes rendues y est le
> contournement fiable, puisque la clause rend exactement une ligne par ligne écrite.

---

## 2. Les trois fonctions

| Fonction FR | Fonction EN | Méthode prefix |
|---|---|---|
| `SQLConnexionOuvre(cnx)` | `SQLConnectionOpen(cnx)` | `cnx.Ouvre()` / `cnx.Open()` |
| `SQLConnexionFerme(cnx)` | `SQLConnectionClose(cnx)` | `cnx.Ferme()` / `cnx.Close()` |
| `SQLEnvoie(cnx, req)` | `SQLSend(cnx, req)` | — |

Les méthodes préfixe et les fonctions globales sont équivalentes —
choisir l'une ou l'autre est une question de style.

---

## 3. Squelette idiomatique

Sur SQLite (fichier local) :

```owl
cnx est une sqlConnexion
cnx..Provider = sqlSqlite
cnx..Source   = "./donnees.db"
cnx.Ouvre()

req est une sqlRequête
req..Code = "SELECT * FROM clients WHERE pays = {p}"
req..Paramètres["p"] = "FR"

rep est une sqlRéponse = SQLEnvoie(cnx, req)
POUR TOUT ligne DE rep..Lignes
    Trace(ligne["nom"])
FIN

cnx.Ferme()
```

Sur PostgreSQL (réseau, TLS par défaut) :

```owl
cnx est une sqlConnexion
cnx..Provider      = sqlPostgres
cnx..Serveur       = "db.example.com:5432"
cnx..BaseDeDonnées = "production"
cnx..Utilisateur   = "app"
cnx..MotDePasse    = "..."
// cnx..ModeTLS = tlsVerifieComplet  // défaut, redondant
cnx.Ouvre()
```

Sur un serveur client/serveur (`sqlHFSQL`) :

```owl
cnx est une sqlConnexion
cnx..Provider      = sqlHFSQL
cnx..Serveur       = "base.local:4900"
cnx..BaseDeDonnées = "facture"
cnx..Utilisateur   = "admin"
cnx..MotDePasse    = "..."
cnx..ModeTLS       = tlsDesactive  // le serveur gère son propre chiffrement
cnx.Ouvre()
```

Les exemples utilisent SQLite pour la compacité ; la mécanique est
identique sur les autres connecteurs, seul le champ `Provider` change.

---

## 4. Injection SQL — neutralisée par construction

L'**injection SQL** est la faille qui consiste, pour un attaquant, à
glisser du code SQL dans une valeur que le serveur va exécuter au lieu
de la traiter comme une donnée. Classique sur les formulaires :

```owl
// ❌ Faille — la chaîne saisie devient du code
req..Code = "SELECT * FROM clients WHERE nom = '" + entreeUser + "'"
```

Si `entreeUser` vaut `"'; DROP TABLE clients; --"`, la requête envoyée
exécute le `DROP`. La table disparaît.

Le pattern **paramètres nommés** est imposé : on n'écrit jamais la
valeur dans le texte SQL, on la passe via `req..Paramètres["nom"]`. Le
moteur prépare la requête (le SQL est figé), puis lie la valeur comme
**donnée** — jamais comme code.

```owl
// ✅ Sécurisé — entreeUser ne peut pas s'évader vers du SQL
req..Code = "SELECT * FROM clients WHERE nom = {nom}"
req..Paramètres["nom"] = entreeUser
```

Même si `entreeUser` contient toute la malice du monde, le serveur la
traite comme un littéral texte. La table reste intacte.

Le placeholder `{nom}` est transformé en placeholder positionnel canon
du backend (`:nom` SQLite/DuckDB, `$N` Postgres, `?` MySQL/MSSQL/ODBC)
au pré-traitement. Les noms sont libres tant qu'ils sont des
identifiants valides.

---

## 5. Itération sur les lignes

Le résultat d'un `SELECT` est un `tableau` standard OWL ; on itère avec
`POUR TOUT … DE …` :

```owl
POUR TOUT ligne DE rep..Lignes
    Trace(ligne["id"], ligne["nom"])
FIN
```

Chaque `ligne` est un `tableau associatif` indexé par le nom de
colonne. Les types sont préservés à la lecture (cf §7).

Pour un accès indexé direct, les lignes sont 1-based comme tout
tableau OWL :

```owl
premiere est un tableau associatif = rep..Lignes[1]
Trace(premiere["nom"])
```

---

## 6. Réutilisation d'une `sqlRequête`

Une requête est réutilisable. On change les paramètres et on rappelle
`SQLEnvoie` :

```owl
ins est une sqlRequête
ins..Code = "INSERT INTO clients (nom, email) VALUES ({n}, {e})"

POUR TOUT c DE clientsAImporter
    ins..Paramètres["n"] = c.Nom
    ins..Paramètres["e"] = c.Email
    SQLEnvoie(cnx, ins)
FIN
```

Le statement préparé est interne à `SQLEnvoie` (prepare + bind + step
+ finalize en une fois). Pas d'objet curseur à gérer.

---

## 7. Mapping de types OWL ↔ SQL

Le mapping de référence ci-dessous concerne SQLite. Les autres backends
ont leurs spécificités (BIT vs INTEGER, DATETIME natif, BUFFER vs
VARBINARY, NVARCHAR Unicode…) — voir `14_sql_connecteurs.md`
pour chaque backend.

### Bind (OWL → SQLite)

| Type OWL | SQLite | Note |
|---|---|---|
| `entier` | `INTEGER` | int64 |
| `réel` | `REAL` | double |
| `chaîne` | `TEXT` | UTF-8 |
| `booléen` | `INTEGER` | 0 ou 1 |
| `Buffer` | `BLOB` | bytes binaires |
| `Null` | `NULL` | direct |
| `Date`/`Heure`/`DateHeure` | ❌ | convertir manuellement via `.VersChaîne()` |
| `tableau`, autres | ❌ | erreur explicite |

### Read (SQLite → OWL)

| SQLite | Type OWL retourné |
|---|---|
| `INTEGER` | `entier` |
| `REAL` | `réel` |
| `TEXT` | `chaîne` (UTF-8) |
| `BLOB` | `Buffer` |
| `NULL` | `Null` |

---

## 8. Au-delà du `SELECT`

`SQLEnvoie` accepte tout SQL que le backend cible comprend : `INSERT`,
`UPDATE`, `DELETE`, `CREATE TABLE`, `CREATE VIEW`, `BEGIN` / `COMMIT` /
`ROLLBACK`, déclarations de triggers, appels de procédures stockées…
Aucun wrapper transactionnel n'est fourni : la gestion des transactions
reste à la charge du SQL applicatif. Si une erreur survient entre un
`BEGIN` et son `COMMIT`/`ROLLBACK`, la transaction reste pendante
jusqu'à fermeture de la connexion.

---

## 9. Erreurs

`SQLEnvoie` retourne toujours une `sqlRéponse` — jamais d'exception
(sauf erreur fatale type OOM). En cas d'échec :

- `rep..Succès = Faux`
- `ErreurDetectee = Vrai` (variable globale)
- `ErreurInfo()` retourne un message explicatif (incluant code natif du
  backend)

```owl
rep est une sqlRéponse = SQLEnvoie(cnx, req)
SI PAS rep..Succès ALORS
    Trace("Echec : ", ErreurInfo())
FIN
```

`cnx.Ouvre()` et `cnx.Ferme()` retournent un `booléen` : `Vrai` au
succès, `Faux` sinon (avec `ErreurInfo()` rempli).

`cnx.Ferme()` est **idempotent** : refermer une connexion déjà fermée
est un no-op silencieux.

---

## 10. Points d'attention

Communs à tous les connecteurs :

- **Types datetime.** `Date` / `Heure` / `DateHeure` se convertissent
  manuellement en `chaîne` ISO (`"YYYY-MM-DD HH:MM:SS"`) avant binding.
  Les colonnes datetime SQL sont lues en `chaîne` ISO.
- **Lecture eager.** `rep..Lignes` charge l'intégralité du résultat en
  mémoire. Pour les très grands résultats, paginer avec `LIMIT` côté
  SQL.
- **Libération mémoire d'une `sqlRéponse`.** Une réponse volumineuse
  (conséquence directe de la lecture eager) occupe la mémoire tant
  qu'une variable la référence. Sur une portée courte, la mémoire est
  récupérée automatiquement à la sortie de portée, sans rien faire de
  particulier. En revanche, quand une grosse réponse est gardée dans une
  portée longue — variable globale, boucle qui enchaîne les requêtes —
  et qu'elle n'est plus utile avant la fin naturelle de cette portée,
  réaffecter la variable relâche la référence : le ramasse-miettes
  récupère alors l'ancien contenu. Deux formes équivalentes : écraser la
  variable avec la réponse suivante, ou l'affecter à `Null`.

  ```owl
  rep est une sqlRéponse = SQLEnvoie(cnx, req)
  POUR TOUT ligne DE rep..Lignes
      Trace(ligne["nom"])
  FIN
  rep = Null   // relâche la référence : l'ancien résultat devient collectable
  ```

Particularités par connecteur : voir `14_sql_connecteurs.md`.

---

## 11. Référence rapide

```owl
// Setup
cnx est une sqlConnexion
cnx..Provider = sqlSqlite
cnx..Source   = "./db.sqlite"
cnx.Ouvre()

// Une requête, plusieurs exécutions
req est une sqlRequête
req..Code = "SELECT * FROM t WHERE x = {x}"
req..Paramètres["x"] = 42

rep est une sqlRéponse = SQLEnvoie(cnx, req)

// Lecture
POUR TOUT ligne DE rep..Lignes
    Trace(ligne["col"])
FIN

// Statut
SI rep..Succès ALORS
    Trace(rep..Occurrence, " lignes")
SINON
    Trace(ErreurInfo())
FIN

cnx.Ferme()
```

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Versions, auth, TLS par connecteur | `14_sql_connecteurs.md` |
| Conventions générales | `10_conventions_de_code.md` |
| Charte de test | `11_charte_de_test.md` |
| Préfixes de nommage | `12_prefixes_de_nommage.md` |

Des exemples de connexion, requête et itération sont disponibles
pour chacun des connecteurs supportés.
