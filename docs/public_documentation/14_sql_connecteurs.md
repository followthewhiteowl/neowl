# Connecteurs SQL — versions et spécificités

Huit connecteurs natifs et un connecteur générique :

| Connecteur | Pour |
|---|---|
| `sqlSqlite` | SQLite — fichier local ou `:memory:` |
| `sqlDuckDB` | DuckDB — fichier local ou `:memory:` (OLAP, Parquet) |
| `sqlPostgres` | PostgreSQL |
| `sqlMySQL` | MySQL / MariaDB |
| `sqlMSSQL` | Microsoft SQL Server / Azure SQL |
| `sqlHFSQL` | Serveur SQL client/serveur, driver embarqué |
| `sqlHFSQLClassic` | Base locale en fichiers, sans serveur |
| `sqlODBC` | Tout SGBD disposant d'un driver ODBC tiers |

Cette page liste les particularités utiles à savoir avant de coder
contre un serveur réel : versions supportées, modes d'authentification,
TLS, codes erreur. Pour la sémantique des constantes elles-mêmes :
`owl explain sqlPostgres`, `owl explain tlsVerifieComplet`, etc.

---

## Quel champ pour quel connecteur

Le descripteur `sqlConnexion` expose un jeu de champs commun, mais
chaque champ ne concerne qu'une partie des connecteurs. Cette matrice
indique, pour chaque champ, où il s'applique — de quoi lever tout doute
avant de renseigner une connexion.

| Champ | Connecteurs concernés |
|---|---|
| `Provider` | tous (choisit le connecteur) |
| `Source` | mode expert : `sqlSqlite`/`sqlDuckDB` (chemin fichier), `sqlODBC`, `sqlHFSQL`, `sqlHFSQLClassic` (connection string brute) |
| `Serveur` | réseau : `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL` (Client/Serveur), `sqlODBC` |
| `Port` | réseau : `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL` |
| `BaseDeDonnées` | réseau : `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlODBC` |
| `Utilisateur` / `MotDePasse` | authentification : `sqlPostgres`, `sqlMySQL`, `sqlMSSQL`, `sqlHFSQL`, `sqlODBC` |
| `ModeTLS` / `CertificatCA` | réseau uniquement : `sqlPostgres`, `sqlMySQL`, `sqlMSSQL` |
| `Pilote` | `sqlODBC` (nom du driver ODBC installé) |
| `Analyse` / `Repertoire` | `sqlHFSQLClassic` uniquement |

Un champ hors de sa colonne n'est pas une erreur : il est ignoré, et
`Ouvre()` se déroule exactement comme s'il n'avait pas été renseigné. Pour
qu'un champ ignoré ne coûte pas une heure de recherche, un avertissement est
tout de même émis sur la sortie d'erreur, qui nomme le champ écarté et liste
ceux que le connecteur choisi prend en compte :

```
[OWL] AVERTISSEMENT SQLConnexionOuvre : cnx..Source est renseigne mais
ignore par le connecteur sqlPostgres. Champs pris en compte par ce
connecteur : Serveur, Port, BaseDeDonnees, Utilisateur, MotDePasse,
ModeTLS, CertificatCA.
```

Erreur classique visée par cet avertissement : décrire un serveur réseau dans
`Source` (`"127.0.0.1:5432"`) là où le connecteur attend `Serveur`. Le mode
expert `Source` de `sqlODBC` / `sqlHFSQL` / `sqlHFSQLClassic` (connection
string brute, voir plus bas) est une utilisation normale de `Source` et ne
déclenche aucun avertissement. `ModeTLS` non plus, puisqu'il a toujours une
valeur ; son cas particulier côté `sqlHFSQL` est traité dans la section
dédiée.

---

## TLS — connecteurs réseau (PostgreSQL / MySQL / MSSQL)

Quatre modes exposés via `cnx..ModeTLS` : `tlsDesactive` /
`tlsAccepteTout` / `tlsVerifieCA` / `tlsVerifieComplet` (défaut prod).
Détail de chaque mode dans `owl explain`.

Pour une CA interne (entreprise, AD CS, OpenSSL local) non présente dans
le bundle de confiance par défaut :

```owl
cnx..ModeTLS      = tlsVerifieCA
cnx..CertificatCA = "C:/certs/ca-interne.pem"
```

La CA fournie **remplace** le bundle par défaut — le choix de confiance
est explicite. Champ supporté par les trois connecteurs réseau.

`sqlHFSQL` et `sqlODBC` n'exposent pas `ModeTLS` côté connection string
(voir leurs sections).

---

## PostgreSQL

| Version PG | Auth plugins | Statut |
|---|---|---|
| **PG 8.0 → 9.6** | MD5 password | ✅ |
| **PG 10 → 17** | SCRAM-SHA-256 (défaut), MD5 (compat) | ✅ |
| Pré-8.0 (≤ 2004) | trust / clear | ❌ |

Borne basse : **PG 8.0 (2005)**. SCRAM exige PG 10+ ; MD5 fortement
déconseillé à partir de PG 10.

---

## MySQL / MariaDB

| Version | Auth plugin | Statut |
|---|---|---|
| **MySQL 5.0 → 5.7** | `mysql_native_password` | ✅ |
| **MySQL 8.0 → 8.4** | `caching_sha2_password` (fast path) | ✅ |
| **MariaDB 5.x → 11.x** | `mysql_native_password` | ✅ |
| MariaDB avec `ed25519` | `client_ed25519` | ❌ |
| MariaDB avec `gssapi_client` | GSSAPI | ❌ |

Borne basse : **MySQL 5.0 (2005)**.

**Slow path `caching_sha2`** : refusé par défaut (envoie le password en
clair vers le serveur, même via TLS). Cas typique d'erreur : nouveau
user jamais authentifié → cache serveur vide. Solution : authentifier
ce user une 1ère fois via le client officiel `mysql`, le cache se
remplit, puis OWL fonctionne.

---

## MSSQL (Microsoft SQL Server)

| Version | Protocole TDS | Statut |
|---|---|---|
| **SQL Server 2012 → 2022** | TDS 7.4 | ✅ cible principale |
| **SQL Server 2008 R2 / 2008** | TDS 7.3 | ⚠ compatible, validation manuelle |
| ≤ SQL Server 2005 | TDS 7.2 / 7.1 | ❌ |
| Azure SQL Database | TDS 7.4 + Force Encryption | ✅ avec `tlsVerifieComplet` |
| Azure Synapse SQL | TDS 7.4 (subset) | ✅ requêtes standard |

**Authentification** : `user / password` uniquement. Windows Integrated
(NTLM / Kerberos / SSPI) et Azure Active Directory ne sont pas
supportés.

**TLS spécifique** : Azure SQL et tout SQL Server avec « Force
Encryption ON » exigent `tlsVerifieComplet` (ou `tlsVerifieCA +
CertificatCA` pour une CA interne). `tlsDesactive` ne fonctionne que si
Force Encryption est OFF côté serveur.

**Paramètres nommés** : routés via `sp_executesql` RPC (anti-injection
garanti, cf. `13_sql_modele.md §4`). `NVARCHAR(MAX)` / `VARBINARY(MAX)`
PARTLEN, MARS et bulk insert ne sont pas couverts.

---

## Serveur client/serveur (`sqlHFSQL`)

| OS | Statut |
|---|---|
| Windows x86_64 | ✅ |
| Linux x86_64 | ✅ |
| macOS | ❌ |

**Chiffrement transport** : propre au moteur serveur, **pas TLS
standard PKI/X.509**. Le niveau de chiffrement est configuré côté
serveur et négocié automatiquement à la connexion.

Conséquence côté code : `cnx..ModeTLS` n'a **aucun effet** sur ce
connecteur. Par sécurité (éviter une fausse impression de TLS X.509),
un warning runtime est émis tant que la valeur diffère de
`tlsDesactive`. Pour silencer, poser explicitement :

```owl
cnx..Provider = sqlHFSQL
cnx..ModeTLS  = tlsDesactive   // signal explicite : le chiffrement est côté serveur
```

**Authentification** : compte configuré côté serveur.

---

## Base locale en fichiers (`sqlHFSQLClassic`)

Accès à une base stockée en **fichiers locaux** (fichiers de données et
leurs index), **sans aucun serveur** à installer ni à contacter. Le
pilote et le moteur sont embarqués dans OWL : zéro installation côté
poste.

| OS | Statut |
|---|---|
| Windows x86_64 | ✅ |
| Linux x86_64 | ✅ |
| macOS | ❌ |

**Deux champs obligatoires** :

- `cnx..Analyse` — chemin du **fichier d'analyse** (le schéma de la
  base). Sans ce fichier, les données ne peuvent pas être requêtées :
  c'est une limite dure du connecteur (le schéma n'est pas déductible
  des seuls fichiers de données).
- `cnx..Repertoire` — chemin du **dossier contenant les fichiers de
  données**.

```owl
cnx est une sqlConnexion
cnx..Provider   = sqlHFSQLClassic
cnx..Analyse    = "C:/Bases/Facturation/Facturation.wdd"
cnx..Repertoire = "C:/Bases/Facturation/Donnees"

SI cnx.Ouvre() ALORS
    req est une sqlRequete
    req..Code = "SELECT IDClient, Nom FROM Client ORDER BY Nom"
    rep est une sqlReponse = SQLEnvoie(cnx, req)
    POUR TOUT ligne DE rep..Lignes
        Trace(ligne["Nom"])
    FIN
    cnx.Ferme()
SINON
    Trace(ErreurInfo())
FIN
```

**Chiffrement / TLS** : accès purement local, **aucun transport
réseau**. `cnx..ModeTLS` n'a pas de sens ici et **aucun warning n'est
émis** (contrairement au mode Client/Serveur).

**Erreurs de configuration** : si `Analyse` ou `Repertoire` est vide,
`cnx.Ouvre()` renvoie `Faux` et `ErreurInfo()` nomme le champ manquant.

**Mode expert** : `cnx..Source` renseigné court-circuite l'assemblage
et prend la priorité (connection string brute, comme pour `sqlODBC`).

---

## ODBC (Oracle, Access, Firebird, …)

`sqlODBC` ouvre l'accès à tout SGBD disposant d'un driver ODBC
installé. Voie de sortie pour les SGBD non couverts par un connecteur
natif.

> Pour le serveur client/serveur embarqué, utiliser `sqlHFSQL` plutôt
> que de passer par `sqlODBC` (qui demanderait au user d'installer le
> driver séparément).

| OS | Statut |
|---|---|
| Windows | ✅ (driver manager natif depuis Win 2000) |
| Linux | ✅ |
| macOS | ❌ |

**Drivers validés (Windows)** : ODBC Driver 17/18 for SQL Server,
Oracle Instant Client 12c+, IBM DB2, Microsoft Access (`.mdb`/`.accdb`),
Sybase ASE, Firebird. Tout driver ODBC 3.x est accepté.

**Deux modes API** :

```owl
// Mode 1 — connection string complète
cnx..Provider = sqlODBC
cnx..Source   = "DRIVER={Oracle in OraClient21Home1};DBQ=prod_db;UID=app;PWD=..."

// Mode 2 — champs structurés (OWL assemble en interne)
cnx..Provider    = sqlODBC
cnx..Pilote      = "Oracle in OraClient21Home1"
cnx..Serveur     = "prod_db"
cnx..Utilisateur = "app"
cnx..MotDePasse  = "..."
```

Si `Source` est renseigné, il prend la priorité (mode expert).

**Authentification & TLS** : entièrement délégués au driver. `ModeTLS`
et `CertificatCA` sont sans effet — chaque driver tiers expose ses
propres paramètres dans sa connection string (`Encrypt=yes`,
`SslMode=verify-full`, etc.). Consulter la doc du driver concerné.

**Limitations** : BLOB/CLOB matérialisés en mémoire (pas de streaming),
seul le premier result set d'une procédure stockée multi-curseurs est
lu.

### Diagnostic ODBC

En cas d'échec, `ErreurInfo()` retourne un message au format :

```
SQLConnexionOuvre (ODBC): SQLDriverConnect: [SQLSTATE] native_code <message>
```

Le `SQLSTATE` (5 caractères, standard ISO) permet de diagnostiquer.
Codes utiles :

| Code | Cause |
|---|---|
| `IM002` | Data source name not found — driver mal écrit ou pas installé |
| `IM003` | Specified driver could not be loaded — bitness 32/64 incohérent |
| `28000` | Invalid authorization specification — credentials |
| `08001` | Client unable to establish connection — réseau / firewall |

---

## Oracle

Aucun connecteur natif. Deux options :

- **`Provider = sqlODBC`** + Oracle Instant Client ODBC driver installé
  côté machine.
- Migrer le périmètre concerné vers PostgreSQL ou MSSQL.

---

## Diagnostic d'une connexion qui échoue

`Ouvre()` renvoie `Faux` et **rien ne s'affiche** : c'est la convention de
toutes les opérations qui peuvent échouer sans être des fautes de
programmation (accès fichier, appel réseau, connexion). Le détail complet
n'est pas perdu pour autant — il est déposé dans `ErreurInfo()`, et c'est la
seule chose à lire quand une connexion refuse de s'ouvrir :

```owl
SI PAS cnx.Ouvre() ALORS
    Trace(ErreurInfo())
FIN
```

Le message nomme l'opération, la cible contactée, et le code natif renvoyé
par le serveur ou le driver, ce qui suffit en général à identifier la cause :

```
SQLConnexionOuvre (Postgres): tcp connect: connect: 127.0.0.1:5432
injoignable (timeout 5000ms) (code 1).
```

> **Piège à éviter** : substituer un message maison au diagnostic, par
> exemple `Erreur("connexion impossible")` dans la branche d'échec. Le
> message générique remplace la seule information exploitable. Afficher
> `ErreurInfo()`, ou l'inclure dans le message maison.

Quand la cible est locale (`127.0.0.1`, `::1`, `localhost`) et qu'un mode TLS
est exigé, le détail ajoute la piste la plus fréquente : un serveur de
développement local est très souvent configuré sans transport chiffré, alors
que le défaut `tlsVerifieComplet` l'impose. Dans ce cas précis, poser
`cnx..ModeTLS = tlsDesactive` — et uniquement pour ce serveur local.

Le champ à interroger séparément est disponible via les constantes
d'information d'erreur (`ErreurInfo(errMessage)` pour le seul message), utile
pour tester la cause dans du code de validation.

Des exemples concrets de connexion sont disponibles pour les
principaux SGBD (PostgreSQL, MySQL, `sqlHFSQL`) et servent de point de
départ pour reproduire un problème — éditer les credentials avant
exécution.

---

## Pour aller plus loin

| Sujet | Fiche |
|---|---|
| Modèle SQL (connexion, requête, jeu de résultats) | `13_sql_modele.md` |
| Aperçu général du langage | `00_apercu_du_langage.md` |
| Conventions de code | `10_conventions_de_code.md` |

Pour la sémantique d'une constante donnée :
`owl explain <constante>` (ex : `sqlPostgres`, `tlsVerifieComplet`).
