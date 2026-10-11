// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL stdlib — moteur SQL embarque (SQLite en V1).
 *
 * Trois types reifies cote OWL :
 *   - sqlConnexion : descripteur de connexion (Provider/Source/Serveur/...
 *                    + handle SQLite cache dans _handle).
 *   - sqlRequete   : template SQL (.Code) + parametres nommes (.Parametres)
 *                    avec placeholder syntaxe {nom} → :nom au runtime.
 *   - sqlReponse   : resultat d'execution (.Succes, .Lignes, .Occurrence,
 *                    .LignesAffectees).
 *
 * API publique (canon FR / EN) :
 *   - SQLEnvoie(cnx, req)                / SQLSend
 *   - SQLConnexionOuvre(cnx)             / SQLConnectionOpen + .Ouvre()
 *   - SQLConnexionFerme(cnx)             / SQLConnectionClose + .Ferme()
 *
 * Erreurs : on remplit sqlReponse.Succes = faux et on positionne
 * ErreurDetectee globalement via owl_set_error(). Pas d'exception
 * runtime sauf etat impossible (alloc, corruption).
 *
 * Securite : placeholders {nom} dans Code → :nom dans le SQL final →
 * sqlite3_bind_*() typed. Concatenation SQL = responsabilite user.
 *
 * SQL.2.a : squelette V1 — types declares, natives stubbees. Implementation
 * reelle dans SQL.2.b (Ouvre/Ferme) et SQL.2.c (Envoie).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
/* Guard requise : ce module tire potentiellement windows.h via les vendors
 * (ODBC sqlext, mssql) et winnt.h definit un TokenType qui colliderait avec
 * scanner.h tire par owl_errors.h via son API compile-aware. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "object.h"
#include "vm.h"
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "sql_postgres_conn.h"
#include "sql_postgres_query.h"
#include "sql_postgres_types.h"

/* Codes de backend (enum sqlBackend) — sentinels INT32_MIN+offset.
 * Definis dans sentinels.h via OWL_SQL_BACKEND_BASE. */
#include "sentinels.h"
#include "sql_cnx_string.h"

/* ============================================================
 * Helpers internes (set fields, lookup handle, etc.).
 *
 * NOTE : on duplique les helpers set_field_* parce qu'ils sont
 * static dans stdlib_builtin_types.c. Pas grave, c'est ~30 lignes.
 * Si le pattern se repete dans un 4e module on les promeut.
 * ============================================================ */

/*  : inst_field_set_str/int/bool supprimes — call-sites migres
 * vers les helpers canon inst_field_set_* (object.{c,h}). Discipline
 * GC roots concentree dans 4 helpers uniques.
 *
 * sql_set_field_assoc et sql_set_field_array conserves pour la lisibilite
 * du call-site (le builder construit un sub-objet typed), mais delegueant
 * au canon inst_field_set_value. */

static void sql_set_field_assoc(ObjInstance* inst, const char* name) {
    /*  : push inst AVANT obj_new_assoc (alloc peut grow → GC). */
    vm_push(OBJ_VAL(inst));
    ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
    vm_push(OBJ_VAL(a));
    inst_field_set_value(inst, name, OBJ_VAL(a));
    vm_pop();
    vm_pop();
}

static void sql_set_field_array(ObjInstance* inst, const char* name) {
    vm_push(OBJ_VAL(inst));
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));
    inst_field_set_value(inst, name, OBJ_VAL(arr));
    vm_pop();
    vm_pop();
}

/* ============================================================
 * Templates des 3 types reifies.
 * ============================================================ */

/* Convention OWL pour les types reifies (cf httpRequete dans
 * stdlib_builtin_types.c) :
 *  - structName + fields stockes en ASCII pur. Le compilateur OWL fait
 *    l'accent-strip au lookup, donc `req.Paramètres` et `req.Parametres`
 *    accedent au meme storage (le canonique ASCII).
 *  - Les versions accentuees pour la documentation utilisateur sont
 *    declarees dans FIELDS_sql*[] (deuxieme champ de OwlField).
 *  - Aucun doublon de field dans le template. */

ObjInstance* build_sqlConnexion_template(void) {
    ObjString* name = obj_copy_string("sqlConnexion", 12);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    /* Champs publics — alignes sur le canon (type Connexion). */
    inst_field_set_int   (tpl, "Provider",      OWL_SQL_SQLITE);
    inst_field_set_str(tpl, "Source",        "");  /* fichier SQLite, ou DSN ODBC */
    inst_field_set_str(tpl, "Serveur",       "");  /* host PG/MySQL (V2). Format "host" ou "host:port". */
    inst_field_set_int   (tpl, "Port",          0);   /* override explicite (0 = parse depuis Serveur, defaut 5432 PG). */
    inst_field_set_str(tpl, "BaseDeDonnees", ""); /* canon : BaseDeDonnées */
    inst_field_set_str(tpl, "Utilisateur",   "");
    inst_field_set_str(tpl, "MotDePasse",    "");
    inst_field_set_int   (tpl, "ModeTLS",       OWL_SQL_TLS_VERIFIE_COMPLET);  /* defaut prod */
    inst_field_set_str(tpl, "CertificatCA",  "");  /* chemin PEM CA additionnelle (CA interne / self-signed) */
    inst_field_set_int   (tpl, "TimeoutCnx",    0);   /* delai d'ouverture en secondes (0 = defaut, cf cnx_timeout_seconds). */
    inst_field_set_str(tpl, "Pilote",        "");  /* ODBC : nom du driver tel qu'enregistre (ex: "Oracle in OraClient21Home1", "PostgreSQL Unicode") */
    inst_field_set_str(tpl, "Analyse",       "");  /* Backend local en fichiers : fichier d'analyse .wdd (canon accentue Analyse). */
    inst_field_set_str(tpl, "Repertoire",    "");  /* Backend local en fichiers : dossier des fichiers de donnees (canon : Répertoire). */

    /* _handle : pointeur opaque vers sqlite3* (cast intptr_t→INTEGER_VAL).
     * Prefixe underscore = field interne, non documente cote user. */
    inst_field_set_int   (tpl, "_handle",       0);

    vm_pop();
    vm_pop();
    return tpl;
}

ObjInstance* build_sqlRequete_template(void) {
    ObjString* name = obj_copy_string("sqlRequete", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    inst_field_set_str(tpl, "Code",        "");
    sql_set_field_assoc (tpl, "Parametres"); /* canon : Paramètres */

    vm_pop();
    vm_pop();
    return tpl;
}

ObjInstance* build_sqlReponse_template(void) {
    ObjString* name = obj_copy_string("sqlReponse", 10);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    inst_field_set_bool (tpl, "Succes",            false); /* canon : Succès */
    sql_set_field_array(tpl, "Lignes");                    /* tableau de tableau associatif */
    inst_field_set_int  (tpl, "Occurrence",        0);
    inst_field_set_int  (tpl, "LignesAffectees",   0);     /* canon : LignesAffectées */

    vm_pop();
    vm_pop();
    return tpl;
}

/* ============================================================
 * Enum sqlBackend + constantes.
 * ============================================================ */

static const char* const E_sqlBackend_values[] = {
    "sqlSqlite", "sqlPostgres", "sqlMySQL", "sqlDuckDB",
    "sqlMSSQL", "sqlODBC", "sqlHFSQL", "sqlHFSQLClassic", NULL
};

const OwlEnum E_sqlBackend = {
    .name           = "sqlBackend",
    .nameAscii      = "sqlBackend",
    .nameEN         = "sqlBackend",
    .category       = "SQL",
    .descFR         = "Backend SQL utilise par une sqlConnexion (SQLite, PostgreSQL, MySQL).",
    .descEN         = "SQL backend used by a sqlConnexion (SQLite, PostgreSQL, MySQL).",
    .values         = E_sqlBackend_values,
    .combinable     = false
};

/* Niveaux TLS pour sqlConnexion.ModeTLS — aligne Postgres sslmode. */
static const char* const E_tlsModes_values[] = {
    "tlsDesactive", "tlsAccepteTout", "tlsVerifieCA", "tlsVerifieComplet", NULL
};

const OwlEnum E_tlsModes = {
    .name           = "tlsModes",
    .nameAscii      = "tlsModes",
    .nameEN         = "tlsModes",
    .category       = "SQL",
    .descFR         = "Niveau TLS pour une connexion SQL (aligne Postgres sslmode).",
    .descEN         = "TLS level for a SQL connection (aligned with Postgres sslmode).",
    .values         = E_tlsModes_values,
    .combinable     = false
};

static const OwlEnum* const SQL_ENUMS[] = { &E_sqlBackend, &E_tlsModes };

#define C_SQL(nm, val, dfr, den, en) {nm, "SQL", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef SQL_CONSTS[] = {
    C_SQL("sqlSqlite",    OWL_SQL_SQLITE,
          "Backend SQLite embarque. Source = chemin fichier .db ou \":memory:\". Mode WAL active automatiquement sur fichier disque.",
          "Embedded SQLite backend. Source = .db file path or \":memory:\". WAL mode auto-enabled on disk file.", "sqlSQLite"),
    C_SQL("sqlPostgres",  OWL_SQL_POSTGRES,
          "Backend PostgreSQL via protocole natif (port 5432 par defaut). SCRAM-SHA-256 et MD5 supportes. TLS via ModeTLS.",
          "PostgreSQL backend via native wire protocol (default port 5432). SCRAM-SHA-256 and MD5 supported. TLS via ModeTLS.", "sqlPostgres"),
    C_SQL("sqlMySQL",     OWL_SQL_MYSQL,
          "Backend MySQL / MariaDB via protocole natif (port 3306 par defaut). mysql_native_password et caching_sha2_password supportes.",
          "MySQL / MariaDB backend via native wire protocol (default port 3306). mysql_native_password and caching_sha2_password supported.", "sqlMySQL"),
    C_SQL("sqlDuckDB",    OWL_SQL_DUCKDB,
          "Backend DuckDB embarque (OLAP, colonnaire). Source = chemin fichier .duckdb ou \":memory:\". Extensions parquet/json/icu embarquees.",
          "Embedded DuckDB backend (OLAP, columnar). Source = .duckdb file path or \":memory:\". parquet/json/icu extensions bundled.", "sqlDuckDB"),
    C_SQL("sqlMSSQL",     OWL_SQL_MSSQL,
          "Backend Microsoft SQL Server via TDS 7.4 natif (port 1433 par defaut). Cible SQL Server 2012+ et Azure SQL. Authentification user/password seule.",
          "Microsoft SQL Server backend via native TDS 7.4 (default port 1433). Targets SQL Server 2012+ and Azure SQL. user/password auth only.", "sqlMSSQL"),
    C_SQL("sqlODBC",      OWL_SQL_ODBC,
          "Backend SQL generique via ODBC (drivers tiers installes sur l'OS : Oracle, Access, Firebird, DB2...). TLS et auth delegues au driver.",
          "Generic SQL backend via ODBC (third-party drivers installed on the OS: Oracle, Access, Firebird, DB2...). TLS and auth delegated to driver.", "sqlODBC"),
    C_SQL("sqlHFSQL",     OWL_SQL_HFSQL,
          "Backend SQL client/serveur a driver embarque (port 4900 par defaut, zero install). Le transport est chiffre et negocie cote serveur — ModeTLS sans effet.",
          "Client/server SQL backend with embedded driver (default port 4900, zero install). Transport encryption is negotiated server-side — ModeTLS has no effect.", "sqlHFSQL"),
    C_SQL("sqlHFSQLClassic", OWL_SQL_HFSQL_CLASSIC,
          "Base locale en fichiers (sans serveur) — pilote et moteur embarques, zero install. Requiert Analyse (fichier d'analyse) + Repertoire (dossier des fichiers de donnees).",
          "Local file-based database (serverless) — driver and engine embedded, zero install. Requires Analyse (schema file) + Repertoire (data files folder).", "sqlHFSQLClassic"),
    C_SQL("tlsDesactive",       OWL_SQL_TLS_DESACTIVE,
          "Pas de TLS — connexion en clair. Reseau prive ou dev local uniquement.",
          "No TLS — clear-text connection. Private network or local dev only.", "tlsDisabled"),
    C_SQL("tlsAccepteTout",     OWL_SQL_TLS_ACCEPTE_TOUT,
          "TLS sans verification (sslmode=require) : chiffrement seul. Dev contre serveur self-signed.",
          "TLS with no verification (sslmode=require): encryption only. Dev against self-signed server.", "tlsAcceptAll"),
    C_SQL("tlsVerifieCA",       OWL_SQL_TLS_VERIFIE_CA,
          "TLS + verification chaine CA, hostname ignore (sslmode=verify-ca). Cas reverse-proxy multi-tenant.",
          "TLS + CA chain verification, hostname ignored (sslmode=verify-ca). Reverse-proxy multi-tenant case.", "tlsVerifyCA"),
    C_SQL("tlsVerifieComplet",  OWL_SQL_TLS_VERIFIE_COMPLET,
          "TLS + chaine CA + hostname (sslmode=verify-full). Defaut en prod, recommande des qu'on traverse Internet.",
          "TLS + CA chain + hostname (sslmode=verify-full). Production default, recommended whenever traversing the Internet.", "tlsVerifyFull"),
};
#undef C_SQL

/* ============================================================
 * Helpers — extraction sqlConnexion + handle SQLite.
 *
 * Le handle sqlite3* est stocke dans le field _handle (entier
 * = (intptr_t)pointeur). Field interne, non documente cote user.
 * ============================================================ */

/* Verifie que v est un ObjInstance de structName "sqlConnexion".
 * Retourne l'instance, ou NULL et pose ErreurDetectee si pas le bon type. */
static ObjInstance* resolve_sql_cnx(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn,
                              "arg", "1",
                              "expected", "sqlConnexion",
                              "actual", "un autre type",
                              NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "sqlConnexion") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn,
                              "arg", "1",
                              "expected", "sqlConnexion",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static sqlite3* cnx_get_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (sqlite3*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle(ObjInstance* inst, sqlite3* db) {
    /*  : delegue au canon inst_field_set_int (object.c). */
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)db);
}

/* Lit un field string. Retourne NULL si absent ou pas string. */
static const char* cnx_get_string_field(ObjInstance* inst, const char* name) {
    Value v;
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_STRING(v)) return NULL;
    return AS_CSTRING(v);
}

/* Lit un field int. Retourne defaultVal si absent ou pas int. */
static int64_t cnx_get_int_field(ObjInstance* inst, const char* name, int64_t defaultVal) {
    Value v;
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    if (!table_get(&inst->fields, k, &v)) return defaultVal;
    if (IS_INTEGER(v)) return AS_INTEGER(v);
    if (IS_REAL(v))    return (int64_t)AS_REAL(v);
    return defaultVal;
}

/* Delai d'ouverture en SECONDES. Le template pose 0 pour distinguer un
 * champ jamais renseigne d'un delai voulu (cf. FIELDS_sqlConnexion) ; c'est
 * ici, et uniquement ici, que 0 redevient le delai par defaut. */
#define SQL_TIMEOUT_DEFAUT_S 5
static int cnx_timeout_seconds(ObjInstance* cnx) {
    int64_t t = cnx_get_int_field(cnx, "TimeoutCnx", 0);
    if (t <= 0) return SQL_TIMEOUT_DEFAUT_S;
    return (int)t;
}

/* ============================================================
 * Garde "propriete de connexion inapplicable au Provider".
 *
 * Un champ renseigne hors de sa colonne est ignore par le backend, sans
 * un mot : cout de diagnostic pur pour l'utilisateur (cas typique :
 * cnx..Source pose sur un provider reseau). On avertit sur stderr, meme
 * forme que le warning ModeTLS de ouvre_hfsql, sans jamais changer le
 * comportement d'ouverture.
 *
 * La table est volontairement PERMISSIVE : un faux positif crierait sur
 * une configuration valide, ce qui serait pire que le silence actuel.
 * Consequences :
 *   - Source est declaree applicable partout ou une chaine de connexion
 *     brute est acceptee (ODBC, HFSQL, HFSQL Classic — le "mode expert"
 *     qui court-circuite l'assemblage), donc jamais d'alerte dessus ;
 *   - la table suit ce que les fonctions ouvre_* LISENT reellement, pas
 *     la matrice de doc, plus restrictive sur deux points (ODBC lit
 *     Serveur et BaseDeDonnees via build_odbc_connection_string) ;
 *   - ModeTLS n'est pas alertable ici : son defaut est non vide
 *     (tlsVerifieComplet) donc tout serait signale, et son cas HFSQL est
 *     deja traite dans ouvre_hfsql.
 * ============================================================ */

/* Bit du provider dans les masques ci-dessous (sentinelles +1..+8). */
#define SQL_PB(p) (1u << (unsigned)((p) - OWL_SQL_BACKEND_BASE))

/* Reseau = host/port/base : Postgres, MySQL, MSSQL, HFSQL C/S. */
#define SQL_M_RESEAU (SQL_PB(OWL_SQL_POSTGRES) | SQL_PB(OWL_SQL_MYSQL) | \
                      SQL_PB(OWL_SQL_MSSQL)    | SQL_PB(OWL_SQL_HFSQL))
/* Reseau + ODBC (qui assemble aussi Serveur/BaseDeDonnees/UID/PWD). */
#define SQL_M_RESEAU_ODBC (SQL_M_RESEAU | SQL_PB(OWL_SQL_ODBC))
/* TLS pilotable cote client : les trois backends a protocole natif. */
#define SQL_M_TLS (SQL_PB(OWL_SQL_POSTGRES) | SQL_PB(OWL_SQL_MYSQL) | \
                   SQL_PB(OWL_SQL_MSSQL))
/* TimeoutCnx : liste etablie par les sites de lecture, pas par la doc.
 * ouvre_mssql (delai converti en ms), ouvre_odbc et ouvre_hfsql_avec_chaine,
 * qui sert HFSQL C/S ET HFSQL Classic. Postgres, MySQL, SQLite et DuckDB ne
 * lisent le champ nulle part : ils ouvrent avec le delai de leur lib. */
#define SQL_M_TIMEOUT (SQL_PB(OWL_SQL_MSSQL) | SQL_PB(OWL_SQL_ODBC) | \
                       SQL_PB(OWL_SQL_HFSQL) | SQL_PB(OWL_SQL_HFSQL_CLASSIC))

typedef enum {
    SQLP_STR,   /* chaine : renseignee = non vide */
    SQLP_INT,   /* entier : renseigne = != 0 */
    SQLP_MUET   /* liste dans le message, jamais alertee (defaut non vide) */
} SqlPropKind;

typedef struct {
    const char* field;      /* nom ASCII du field (= storage) */
    unsigned    providers;  /* masque SQL_PB des providers concernes */
    SqlPropKind kind;
} SqlPropScope;

static const SqlPropScope SQL_PROP_SCOPES[] = {
    { "Source",        SQL_PB(OWL_SQL_SQLITE) | SQL_PB(OWL_SQL_DUCKDB) |
                       SQL_PB(OWL_SQL_ODBC)   | SQL_PB(OWL_SQL_HFSQL)  |
                       SQL_PB(OWL_SQL_HFSQL_CLASSIC),          SQLP_STR },
    { "Serveur",       SQL_M_RESEAU_ODBC,                      SQLP_STR },
    { "Port",          SQL_M_RESEAU,                           SQLP_INT },
    { "BaseDeDonnees", SQL_M_RESEAU_ODBC,                      SQLP_STR },
    { "Utilisateur",   SQL_M_RESEAU_ODBC,                      SQLP_STR },
    { "MotDePasse",    SQL_M_RESEAU_ODBC,                      SQLP_STR },
    { "ModeTLS",       SQL_M_TLS,                              SQLP_MUET },
    { "CertificatCA",  SQL_M_TLS,                              SQLP_STR },
    { "TimeoutCnx",    SQL_M_TIMEOUT,                          SQLP_INT },
    { "Pilote",        SQL_PB(OWL_SQL_ODBC),                   SQLP_STR },
    { "Analyse",       SQL_PB(OWL_SQL_HFSQL_CLASSIC),          SQLP_STR },
    { "Repertoire",    SQL_PB(OWL_SQL_HFSQL_CLASSIC),          SQLP_STR },
};
#define SQL_PROP_SCOPES_N \
    (int)(sizeof(SQL_PROP_SCOPES) / sizeof(SQL_PROP_SCOPES[0]))

/* Nom de la constante provider pour le message (E_sqlBackend_values). */
static const char* sql_provider_name(int64_t provider) {
    int idx = (int)(provider - OWL_SQL_BACKEND_BASE) - 1;
    if (idx < 0 || idx > 7) return "?";
    return E_sqlBackend_values[idx];
}

static bool sql_prop_is_set(ObjInstance* cnx, const SqlPropScope* p) {
    if (p->kind == SQLP_STR) {
        const char* s = cnx_get_string_field(cnx, p->field);
        return s != NULL && s[0] != '\0';
    }
    if (p->kind == SQLP_INT) return cnx_get_int_field(cnx, p->field, 0) != 0;
    return false;
}

/* Avertit une fois par champ renseigne mais hors perimetre du provider.
 * Le message nomme le champ ignore ET la liste des champs pris en compte
 * par ce provider (l'alternative se lit dedans : Serveur pour un provider
 * reseau, Analyse/Repertoire pour HFSQL Classic, ...). */
static void sql_warn_props_inapplicables(ObjInstance* cnx, int64_t provider) {
    int idx = (int)(provider - OWL_SQL_BACKEND_BASE) - 1;
    if (idx < 0 || idx > 7) return;   /* provider inconnu : erreur emise par le dispatch */
    unsigned mask = SQL_PB(provider);

    char applicables[256];
    size_t off = 0;
    applicables[0] = '\0';
    for (int i = 0; i < SQL_PROP_SCOPES_N; i++) {
        if ((SQL_PROP_SCOPES[i].providers & mask) == 0) continue;
        off += (size_t)snprintf(applicables + off, sizeof(applicables) - off,
                                "%s%s", off ? ", " : "", SQL_PROP_SCOPES[i].field);
    }

    for (int i = 0; i < SQL_PROP_SCOPES_N; i++) {
        const SqlPropScope* p = &SQL_PROP_SCOPES[i];
        if ((p->providers & mask) != 0) continue;
        if (!sql_prop_is_set(cnx, p)) continue;
        fprintf(stderr,
            "[OWL] AVERTISSEMENT SQLConnexionOuvre : cnx..%s est renseigne mais "
            "ignore par le connecteur %s. Champs pris en compte par ce "
            "connecteur : %s.\n",
            p->field, sql_provider_name(provider),
            applicables[0] ? applicables : "(aucun)");
    }
}

/* Cible locale ? Un serveur de dev en local a le plus souvent TLS coupe :
 * l'indice evite de chercher cote reseau ce qui est un souci de mode TLS. */
static bool sql_host_is_loopback(const char* host) {
    if (host == NULL || host[0] == '\0') return false;
    if (strcmp(host, "127.0.0.1") == 0) return true;
    if (strcmp(host, "::1") == 0) return true;
    if (strncmp(host, "127.", 4) == 0) return true;
    const char* lh = "localhost";
    for (int i = 0; ; i++) {
        char c = host[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != lh[i]) return false;
        if (c == '\0') return true;
    }
}

/* Suffixe d'indice a coller au detail d'erreur d'une ouverture ratee. */
static const char* sql_tls_loopback_hint(const char* host, int tls_mode) {
    if (tls_mode == OWL_SQL_TLS_DESACTIVE) return "";
    if (!sql_host_is_loopback(host)) return "";
    return " [cible locale avec un ModeTLS actif : si ce serveur n'accepte pas "
           "TLS, poser cnx..ModeTLS = tlsDesactive]";
}

/* ============================================================
 * Helpers PostgreSQL (Phase 3 — dispatch backend).
 *
 * Le field _handle de sqlConnexion stocke un pointeur opaque (cast
 * via intptr_t) :
 *   - Provider = sqlSqlite   -> sqlite3*
 *   - Provider = sqlPostgres -> OwlPostgresConn*
 * Distingues par cnx.Provider, pas de tagging du handle.
 * ============================================================ */

static OwlPostgresConn* cnx_get_handle_pg(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlPostgresConn*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle_pg(ObjInstance* inst, OwlPostgresConn* pg) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)pg);
}

/* Decode une valeur DataRow text mode vers une Value OWL selon la
 * categorie du type Oid de la colonne. */
static Value pg_value_to_owl(const OwlPostgresColumn* col,
                              const OwlPostgresValue* val) {
    if (val->length < 0) return NIL_VAL;  /* SQL NULL */
    const char* text = (const char*)val->data;
    size_t len = (size_t)val->length;
    switch (postgres_oid_category(col->type_oid)) {
        case POSTGRES_CAT_BOOL: {
            bool b = false;
            if (postgres_parse_bool(text, len, &b)) return BOOL_VAL(b);
            return NIL_VAL;
        }
        case POSTGRES_CAT_INTEGER: {
            int64_t i = 0;
            if (postgres_parse_int64(text, len, &i)) return INTEGER_VAL(i);
            return NIL_VAL;
        }
        case POSTGRES_CAT_REAL: {
            double d = 0.0;
            if (postgres_parse_double(text, len, &d)) return REAL_VAL(d);
            return NIL_VAL;
        }
        case POSTGRES_CAT_BYTEA: {
            uint8_t* bytes = NULL; size_t blen = 0;
            if (postgres_parse_bytea_hex(text, len, &bytes, &blen)) {
                ObjBuffer* b = obj_new_buffer(bytes, (int)blen);
                free(bytes);
                return OBJ_VAL(b);
            }
            return NIL_VAL;
        }
        case POSTGRES_CAT_STRING:
        case POSTGRES_CAT_DATE:
        case POSTGRES_CAT_TIME:
        case POSTGRES_CAT_TIMESTAMP:
        case POSTGRES_CAT_UNKNOWN:
        default: {
            /* Renvoie comme chaîne — l'user fait .VersDate() etc. */
            ObjString* s = obj_copy_string(text, (int)len);
            return OBJ_VAL(s);
        }
    }
}

/* Parse "host" ou "host:port" -> separe host + port.
 * host_buf doit avoir sizeof >= 256. Retourne le port (default_port si absent).
 * default_port = 5432 (Postgres) ou 3306 (MySQL) selon le backend.
 *
 * Delegue a la brique partagee de sql_cnx_string.c (assertable sans serveur).
 * Le 0 en 2e argument = pas de champ Port explicite ; les appelants ci-dessous
 * l'appliquent encore eux-memes apres coup, ils pourront le passer ici. */
static int parse_host_port_default(const char* serveur, char* host_buf,
                                    size_t host_buf_size, int default_port) {
    return owl_sql_resolve_host_port(serveur, 0, default_port,
                                     host_buf, host_buf_size);
}

/* Wrappers commodes par backend. */
static int parse_host_port(const char* serveur, char* host_buf, size_t host_buf_size) {
    return parse_host_port_default(serveur, host_buf, host_buf_size, 5432);
}
static int parse_host_port_mysql(const char* serveur, char* host_buf, size_t host_buf_size) {
    return parse_host_port_default(serveur, host_buf, host_buf_size, 3306);
}

/* Ouvre une connexion PostgreSQL. */
static Value ouvre_postgres(ObjInstance* cnx) {
    const char* serveur = cnx_get_string_field(cnx, "Serveur");
    if (serveur == NULL || serveur[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (Postgres)",
                              "native", "SQLConnexionOuvre (Postgres)",
                              "arg", "Serveur",
                              NULL);
        return BOOL_VAL(false);
    }
    /* Parse host:port. Field Port explicite override si renseigne. */
    char host_buf[256];
    int port = parse_host_port(serveur, host_buf, sizeof(host_buf));
    int64_t port_field = cnx_get_int_field(cnx, "Port", 0);
    if (port_field > 0 && port_field < 65536) port = (int)port_field;

    const char* user = cnx_get_string_field(cnx, "Utilisateur");
    const char* pwd  = cnx_get_string_field(cnx, "MotDePasse");
    const char* db   = cnx_get_string_field(cnx, "BaseDeDonnees");
    if (db == NULL || db[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (Postgres)",
                              "native", "SQLConnexionOuvre (Postgres)",
                              "arg", "BaseDeDonnees",
                              NULL);
        return BOOL_VAL(false);
    }

    /* TLS : lecture du field ModeTLS (defaut tlsVerifieComplet). */
    int tls_mode = OWL_SQL_TLS_VERIFIE_COMPLET;
    {
        Value v;
        ObjString* k = obj_copy_string("ModeTLS", 7);
        if (table_get(&cnx->fields, k, &v) && IS_INTEGER(v)) {
            tls_mode = (int)AS_INTEGER(v);
        }
    }

    OwlPostgresConn* pg = (OwlPostgresConn*)calloc(1, sizeof(OwlPostgresConn));
    if (!pg) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLConnexionOuvre (Postgres)",
                              "native", "SQLConnexionOuvre (Postgres)",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* extra_ca = cnx_get_string_field(cnx, "CertificatCA");

    owl_postgres_conn_init(pg);
    if (!owl_postgres_conn_open(pg, host_buf, port,
                                 user ? user : "",
                                 pwd  ? pwd  : "",
                                 db,
                                 tls_mode,
                                 extra_ca)) {
        char detail_buf[700];
        const char* detail = owl_postgres_conn_last_error(pg);
        snprintf(detail_buf, sizeof(detail_buf), "%s%s",
                 detail ? detail : "echec d'ouverture",
                 sql_tls_loopback_hint(host_buf, tls_mode));
        free(pg);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (Postgres)",
                              "native", "SQLConnexionOuvre (Postgres)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    cnx_set_handle_pg(cnx, pg);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Ferme une connexion PostgreSQL. */
static Value ferme_postgres(ObjInstance* cnx) {
    OwlPostgresConn* pg = cnx_get_handle_pg(cnx);
    if (pg == NULL) {
        owl_clear_error();
        return BOOL_VAL(true);
    }
    owl_postgres_conn_close(pg);
    free(pg);
    cnx_set_handle_pg(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Forward declaration : build_runtime_reponse cree un sqlReponse cote
 * OWL. Defini plus bas (SQLite path). On le reutilise pour PG. */
static ObjInstance* build_runtime_reponse(bool success, ObjArray* lignes,
                                           int occurrence, int affected);
static Value build_error_reponse(void);

/* Transforme {nom} en $N positional pour Postgres extended query.
 * Le mapping nom->N est construit au fur et a mesure (1-based).
 * Si {nom} reapparait, on reutilise le meme $N.
 *
 * Sortie : SQL transforme (malloc, free par caller), table de noms
 * dans l'ordre des $N (caller free chaque nom + le tableau), et
 * count. */
static char* transform_placeholders_postgres(const char* sql,
                                              char*** out_names,
                                              int* out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (sql == NULL) return NULL;
    size_t len = strlen(sql);
    /* Buffer suffisant : chaque {nom} -> $NNN, pire cas = +3 chars
     * (mais $999 reste rare). Allocation genereuse. */
    char* out = (char*)malloc(len * 4 + 16);
    if (!out) return NULL;

    char** names = NULL;
    int    count = 0;
    int    cap   = 0;

    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (sql[i] == '{') {
            size_t end = i + 1;
            while (end < len && sql[end] != '}') end++;
            if (end < len) {
                size_t nlen = end - (i + 1);
                /* Cherche si nom deja vu. */
                int found = -1;
                for (int k = 0; k < count; k++) {
                    if (strlen(names[k]) == nlen &&
                        memcmp(names[k], sql + i + 1, nlen) == 0) {
                        found = k; break;
                    }
                }
                int pos1;
                if (found >= 0) {
                    pos1 = found + 1;
                } else {
                    if (count >= cap) {
                        cap = cap ? cap * 2 : 8;
                        char** n = (char**)realloc(names, sizeof(char*) * cap);
                        if (!n) { free(out); for (int k=0;k<count;k++) free(names[k]); free(names); return NULL; }
                        names = n;
                    }
                    char* nm = (char*)malloc(nlen + 1);
                    memcpy(nm, sql + i + 1, nlen);
                    nm[nlen] = '\0';
                    names[count] = nm;
                    pos1 = ++count;
                }
                int w = snprintf(out + j, 16, "$%d", pos1);
                if (w < 0) w = 0;
                j += (size_t)w;
                i = end + 1;
                continue;
            }
        }
        out[j++] = sql[i++];
    }
    out[j] = '\0';
    *out_names = names;
    *out_count = count;
    return out;
}

/* Convertit une Value OWL en chaine text pour bind Postgres.
 * Retourne malloc'ed (free par caller) ou NULL pour SQL NULL.
 * is_unsupported mis a true si type non gere. */
static char* pg_value_to_text(Value v, bool* is_unsupported) {
    *is_unsupported = false;
    if (IS_NIL(v)) return NULL;  /* NULL bind */
    if (IS_BOOL(v)) {
        return strdup(AS_BOOL(v) ? "t" : "f");
    }
    if (IS_INTEGER(v)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(v));
        return strdup(buf);
    }
    if (IS_REAL(v)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", AS_REAL(v));
        return strdup(buf);
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        char* out = (char*)malloc(s->length + 1);
        memcpy(out, s->chars, s->length);
        out[s->length] = '\0';
        return out;
    }
    *is_unsupported = true;
    return NULL;
}

/* Verbe du CommandComplete designant une requete qui a modifie des lignes.
 * COPY en est exclu : son compteur ne dit pas le sens du transfert (import ou
 * export), et le sous-protocole CopyData n'est pas implemente ici. */
static bool pg_tag_is_write(const char* tag) {
    static const char* verbs[] = { "INSERT", "UPDATE", "DELETE", "MERGE" };
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++) {
        size_t n = strlen(verbs[i]);
        if (strncmp(tag, verbs[i], n) == 0 &&
            (tag[n] == ' ' || tag[n] == '\0')) return true;
    }
    return false;
}

/* Envoie une requete sur connexion PG. Si req.Parametres non vide,
 * passe en extended query (Parse/Bind/Execute) ; sinon simple query. */
static Value envoie_postgres(ObjInstance* cnx, ObjInstance* req) {
    OwlPostgresConn* pg = cnx_get_handle_pg(cnx);
    if (pg == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie (Postgres)",
                              "native", "SQLEnvoie (Postgres)",
                              NULL);
        ObjArray* empty = obj_new_array();
        vm_push(OBJ_VAL(empty));
        ObjInstance* rep = build_runtime_reponse(false, empty, 0, 0);
        vm_pop();
        return OBJ_VAL(rep);
    }

    /* Verifie req.Code. */
    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie (Postgres)",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        ObjArray* empty = obj_new_array();
        vm_push(OBJ_VAL(empty));
        ObjInstance* rep = build_runtime_reponse(false, empty, 0, 0);
        vm_pop();
        return OBJ_VAL(rep);
    }
    const char* sql = AS_CSTRING(codeVal);

    /* Detecte parametres -> extended query. */
    Value paramsVal;
    ObjString* k_params = obj_copy_string("Parametres", 10);
    ObjAssocArray* params = NULL;
    if (table_get(&req->fields, k_params, &paramsVal) &&
        IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
        ObjAssocArray* p = AS_ASSOC_ARRAY(paramsVal);
        if (p->count > 0) params = p;
    }

    OwlPostgresResult r;
    owl_postgres_result_init(&r);
    bool ok;

    if (params != NULL) {
        /* Transforme {nom} -> $N et collecte les noms dans l'ordre. */
        char** names = NULL;
        int    n_names = 0;
        char*  pgSql = transform_placeholders_postgres(sql, &names, &n_names);
        if (pgSql == NULL) {
            owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie (Postgres)",
                                  "native", "SQLEnvoie (Postgres)",
                                  NULL);
            return build_error_reponse();
        }

        /* Construit le tableau de values dans l'ordre $1..$N. */
        char** values = (char**)calloc(n_names ? n_names : 1, sizeof(char*));
        bool   bind_err = false;
        char   bind_err_msg[256];
        bind_err_msg[0] = '\0';

        for (int i = 0; i < n_names; i++) {
            ObjString* key = obj_copy_string(names[i], (int)strlen(names[i]));
            int exists = 0;
            Value v = obj_assoc_get(params, key, &exists);
            if (!exists) {
                /* Param manquant -> SQL NULL. */
                values[i] = NULL;
                continue;
            }
            bool unsupported = false;
            char* txt = pg_value_to_text(v, &unsupported);
            if (unsupported) {
                snprintf(bind_err_msg, sizeof(bind_err_msg),
                         "SQLEnvoie (Postgres) bind {%s}: type non supporte",
                         names[i]);
                bind_err = true;
                break;
            }
            values[i] = txt;  /* peut etre NULL pour NIL */
        }

        if (!bind_err) {
            ok = owl_postgres_extended_query(pg, pgSql, n_names,
                                              (const char* const*)values, &r);
        } else {
            ok = false;
            snprintf(r.error_message, sizeof(r.error_message), "%s", bind_err_msg);
        }

        /* Cleanup. */
        for (int i = 0; i < n_names; i++) { free(names[i]); free(values[i]); }
        free(names);
        free(values);
        free(pgSql);
    } else {
        ok = owl_postgres_simple_query(pg, sql, &r);
    }
    if (!ok) {
        char detail_buf[512];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 r.error_message[0] ? r.error_message : "(erreur inconnue)");
        owl_postgres_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (Postgres)",
                              "native", "SQLEnvoie (Postgres)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        ObjArray* empty = obj_new_array();
        vm_push(OBJ_VAL(empty));
        ObjInstance* rep = build_runtime_reponse(false, empty, 0, 0);
        vm_pop();
        return OBJ_VAL(rep);
    }

    /* Construit les lignes OWL : tableau de tableau associatif. */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));
    for (int i = 0; i < r.row_count; i++) {
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (int j = 0; j < r.column_count; j++) {
            Value v = pg_value_to_owl(&r.columns[j], &r.rows[i][j]);
            obj_assoc_put_cstr(row, r.columns[j].name,
                               (int)strlen(r.columns[j].name), v);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop(); /* row */
    }

    /* Extrait LignesAffectees du CommandComplete tag (ex "INSERT 0 3").
     * Le tag porte aussi un nombre sur les lectures ("SELECT 3") : seul le
     * verbe, produit par le serveur et donc insensible a la mise en forme du
     * SQL envoye, dit si la requete a ecrit. */
    int affected = 0;
    if (r.command_tag[0] && pg_tag_is_write(r.command_tag)) {
        const char* tag = r.command_tag;
        /* Cherche le dernier nombre dans le tag. */
        const char* p = tag + strlen(tag);
        while (p > tag && (p[-1] == ' ' || (p[-1] >= '0' && p[-1] <= '9'))) p--;
        affected = atoi(p);
    }

    ObjInstance* rep = build_runtime_reponse(true, lignes, r.row_count, affected);
    vm_pop(); /* lignes */
    owl_postgres_result_free(&r);
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * MySQL — ouvre / ferme / envoie. Architecture parallele a PG.
 * ============================================================ */

#include "sql_mysql_conn.h"
#include "sql_mysql_query.h"
#include "sql_mysql_types.h"

static OwlMysqlConn* cnx_get_handle_mysql(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlMysqlConn*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle_mysql(ObjInstance* inst, OwlMysqlConn* my) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)my);
}

static Value ouvre_mysql(ObjInstance* cnx) {
    const char* serveur = cnx_get_string_field(cnx, "Serveur");
    if (serveur == NULL || serveur[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (MySQL)",
                              "native", "SQLConnexionOuvre (MySQL)",
                              "arg", "Serveur",
                              NULL);
        return BOOL_VAL(false);
    }
    char host_buf[256];
    int port = parse_host_port_mysql(serveur, host_buf, sizeof(host_buf));
    int64_t port_field = cnx_get_int_field(cnx, "Port", 0);
    if (port_field > 0 && port_field < 65536) port = (int)port_field;

    const char* user = cnx_get_string_field(cnx, "Utilisateur");
    const char* pwd  = cnx_get_string_field(cnx, "MotDePasse");
    const char* db   = cnx_get_string_field(cnx, "BaseDeDonnees");
    /* db facultative pour MySQL (USE db; possible plus tard). */

    int tls_mode = OWL_SQL_TLS_VERIFIE_COMPLET;
    {
        Value v;
        ObjString* k = obj_copy_string("ModeTLS", 7);
        if (table_get(&cnx->fields, k, &v) && IS_INTEGER(v)) {
            tls_mode = (int)AS_INTEGER(v);
        }
    }

    OwlMysqlConn* my = (OwlMysqlConn*)calloc(1, sizeof(OwlMysqlConn));
    if (!my) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLConnexionOuvre (MySQL)",
                              "native", "SQLConnexionOuvre (MySQL)",
                              NULL);
        return BOOL_VAL(false);
    }
    const char* extra_ca = cnx_get_string_field(cnx, "CertificatCA");

    owl_mysql_conn_init(my);
    if (!owl_mysql_conn_open(my, host_buf, port,
                              user ? user : "",
                              pwd  ? pwd  : "",
                              (db && db[0]) ? db : NULL,
                              tls_mode,
                              extra_ca)) {
        char detail_buf[700];
        const char* detail = owl_mysql_conn_last_error(my);
        snprintf(detail_buf, sizeof(detail_buf), "%s%s",
                 detail ? detail : "echec d'ouverture",
                 sql_tls_loopback_hint(host_buf, tls_mode));
        free(my);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (MySQL)",
                              "native", "SQLConnexionOuvre (MySQL)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    cnx_set_handle_mysql(cnx, my);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value ferme_mysql(ObjInstance* cnx) {
    OwlMysqlConn* my = cnx_get_handle_mysql(cnx);
    if (my == NULL) { owl_clear_error(); return BOOL_VAL(true); }
    owl_mysql_conn_close(my);
    free(my);
    cnx_set_handle_mysql(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value envoie_mysql(ObjInstance* cnx, ObjInstance* req) {
    OwlMysqlConn* my = cnx_get_handle_mysql(cnx);
    if (my == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie (MySQL)",
                              "native", "SQLEnvoie (MySQL)",
                              NULL);
        return build_error_reponse();
    }

    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie (MySQL)",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        return build_error_reponse();
    }
    const char* sql = AS_CSTRING(codeVal);

    /* Parametres bind : non supportes en V2.2 (prepared statements à venir). */
    Value paramsVal;
    ObjString* k_params = obj_copy_string("Parametres", 10);
    if (table_get(&req->fields, k_params, &paramsVal) &&
        IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
        ObjAssocArray* params = AS_ASSOC_ARRAY(paramsVal);
        if (params->count > 0) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "SQLEnvoie (MySQL)",
                                  "native", "SQLEnvoie (MySQL)",
                                  "arg", "Parametres",
                                  NULL);
            return build_error_reponse();
        }
    }

    OwlMysqlResult r;
    owl_mysql_result_init(&r);
    bool ok = owl_mysql_simple_query(my, sql, &r);
    if (!ok) {
        char detail_buf[512];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 r.error_message[0] ? r.error_message : "(erreur inconnue)");
        owl_mysql_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (MySQL)",
                              "native", "SQLEnvoie (MySQL)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return build_error_reponse();
    }

    /* Construit les lignes OWL : tableau de tableau associatif. */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));
    for (int i = 0; i < r.row_count; i++) {
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (int j = 0; j < r.column_count; j++) {
            Value v = owl_mysql_value_to_owl(r.columns[j].type,
                                              r.columns[j].flags,
                                              r.rows[i][j].data,
                                              r.rows[i][j].length);
            obj_assoc_put_cstr(row, r.columns[j].name,
                               (int)strlen(r.columns[j].name), v);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop(); /* row */
    }

    /* Le paquet OK final d'un jeu de resultats porte lui aussi un champ
     * affected_rows ; le protocole y met 0 sur une lecture, mais c'est une
     * convention du serveur, pas une garantie du contrat. La presence de
     * colonnes decrites est le signal qui separe lecture et ecriture, comme
     * pour les autres connecteurs. */
    int affected = (r.column_count > 0) ? 0 : (int)r.affected_rows;
    ObjInstance* rep = build_runtime_reponse(true, lignes, r.row_count, affected);
    vm_pop(); /* lignes */
    owl_mysql_result_free(&r);
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * DuckDB — ouvre / ferme / envoie. Embedded (pas reseau).
 * ============================================================ */

#include "sql_duckdb_conn.h"
#include "sql_duckdb_query.h"

static OwlDuckdbConn* cnx_get_handle_duckdb(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlDuckdbConn*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle_duckdb(ObjInstance* inst, OwlDuckdbConn* d) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)d);
}

static Value ouvre_duckdb(ObjInstance* cnx) {
    /* Source ou Fichier (alias) — DuckDB embedded, pas de host/user/pwd. */
    const char* source = cnx_get_string_field(cnx, "Source");
    if (source == NULL || source[0] == '\0') {
        Value v;
        ObjString* k = obj_copy_string("Fichier", 7);
        if (table_get(&cnx->fields, k, &v) && IS_STRING(v)) {
            const char* f = AS_CSTRING(v);
            if (f && f[0]) source = f;
        }
    }
    if (source == NULL || source[0] == '\0') source = ":memory:";

    OwlDuckdbConn* d = (OwlDuckdbConn*)calloc(1, sizeof(OwlDuckdbConn));
    if (!d) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLConnexionOuvre (DuckDB)",
                              "native", "SQLConnexionOuvre (DuckDB)",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_duckdb_conn_init(d);
    if (!owl_duckdb_conn_open(d, source)) {
        char detail_buf[512];
        const char* detail = owl_duckdb_conn_last_error(d);
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 detail ? detail : "echec d'ouverture");
        free(d);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (DuckDB)",
                              "native", "SQLConnexionOuvre (DuckDB)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    cnx_set_handle_duckdb(cnx, d);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value ferme_duckdb(ObjInstance* cnx) {
    OwlDuckdbConn* d = cnx_get_handle_duckdb(cnx);
    if (d == NULL) { owl_clear_error(); return BOOL_VAL(true); }
    owl_duckdb_conn_close(d);
    free(d);
    cnx_set_handle_duckdb(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value envoie_duckdb(ObjInstance* cnx, ObjInstance* req) {
    OwlDuckdbConn* d = cnx_get_handle_duckdb(cnx);
    if (d == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie (DuckDB)",
                              "native", "SQLEnvoie (DuckDB)",
                              NULL);
        return build_error_reponse();
    }

    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie (DuckDB)",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        return build_error_reponse();
    }
    const char* sql = AS_CSTRING(codeVal);

    /* Parametres bind : non supportes V2.3 (prepared statements à venir). */
    Value paramsVal;
    ObjString* k_params = obj_copy_string("Parametres", 10);
    if (table_get(&req->fields, k_params, &paramsVal) &&
        IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
        ObjAssocArray* params = AS_ASSOC_ARRAY(paramsVal);
        if (params->count > 0) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "SQLEnvoie (DuckDB)",
                                  "native", "SQLEnvoie (DuckDB)",
                                  "arg", "Parametres",
                                  NULL);
            return build_error_reponse();
        }
    }

    OwlDuckdbResult r;
    owl_duckdb_result_init(&r);
    bool ok = owl_duckdb_simple_query(d, sql, &r);
    if (!ok) {
        char detail_buf[512];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 r.error_message[0] ? r.error_message : "(erreur inconnue)");
        owl_duckdb_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (DuckDB)",
                              "native", "SQLEnvoie (DuckDB)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return build_error_reponse();
    }

    /* Sur une ecriture, le moteur rend une ligne de service {Count: N} : c'est
     * le compteur, pas une donnee utilisateur. Il alimente LignesAffectees et
     * n'entre pas dans Lignes. */
    int occurrence = r.changed_rows ? 0 : r.row_count;

    /* Construit les lignes OWL : tableau de tableau associatif. */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));
    for (int i = 0; i < occurrence; i++) {
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (int j = 0; j < r.column_count; j++) {
            obj_assoc_put_cstr(row, r.columns[j].name,
                               (int)strlen(r.columns[j].name), r.rows[i][j]);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop(); /* row */
    }

    /* duckdb_rows_changed() ne renseigne le compteur que sur un resultat de
     * type CHANGED_ROWS ; une clause RETURNING produit un jeu de resultats et
     * le laisse muet. Le type d'instruction annonce par le moteur tranche
     * alors — jamais le texte du SQL — et RETURNING rendant exactement une
     * ligne par ligne touchee, les lignes rendues sont le compteur d'ecriture. */
    int affected = (int)r.affected_rows;
    if (affected == 0 && !r.changed_rows && r.write_statement) {
        affected = occurrence;
    }
    ObjInstance* rep = build_runtime_reponse(true, lignes, occurrence, affected);
    vm_pop(); /* lignes */
    /* owl_duckdb_result_free depile l'ancrage GC du jeu de resultats : le rendu
     * OWL est fini, les valeurs vivent desormais dans `rep`. L'ancrage etant
     * sous `lignes` (pousse en premier par la query), il est au sommet de la
     * pile VM ici, apres le pop de `lignes` (contrat LIFO respecte). */
    owl_duckdb_result_free(&r);
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * MSSQL — ouvre / ferme / envoie. TDS 7.4 natif, SQL Server Auth.
 * ============================================================ */

#include "sql_mssql_conn.h"
#include "sql_mssql_types.h"

static OwlMssqlConn* cnx_get_handle_mssql(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlMssqlConn*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle_mssql(ObjInstance* inst, OwlMssqlConn* m) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)m);
}

static int parse_host_port_mssql(const char* serveur, char* host_buf, size_t host_buf_size) {
    return parse_host_port_default(serveur, host_buf, host_buf_size, 1433);
}

static Value ouvre_mssql(ObjInstance* cnx) {
    const char* serveur = cnx_get_string_field(cnx, "Serveur");
    if (serveur == NULL || serveur[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (MSSQL)",
                              "native", "SQLConnexionOuvre (MSSQL)",
                              "arg", "Serveur",
                              NULL);
        return BOOL_VAL(false);
    }
    char host_buf[256];
    int port = parse_host_port_mssql(serveur, host_buf, sizeof(host_buf));
    int64_t port_field = cnx_get_int_field(cnx, "Port", 0);
    if (port_field > 0 && port_field < 65536) port = (int)port_field;

    const char* user = cnx_get_string_field(cnx, "Utilisateur");
    const char* pwd  = cnx_get_string_field(cnx, "MotDePasse");
    const char* db   = cnx_get_string_field(cnx, "BaseDeDonnees");

    int tls_mode = OWL_SQL_TLS_DESACTIVE;
    {
        Value v;
        ObjString* k = obj_copy_string("ModeTLS", 7);
        if (table_get(&cnx->fields, k, &v) && IS_INTEGER(v)) {
            tls_mode = (int)AS_INTEGER(v);
        }
    }

    int timeout_ms = cnx_timeout_seconds(cnx) * 1000;

    const char* extra_ca = cnx_get_string_field(cnx, "CertificatCA");

    OwlMssqlConn* m = (OwlMssqlConn*)calloc(1, sizeof(OwlMssqlConn));
    if (!m) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLConnexionOuvre (MSSQL)",
                              "native", "SQLConnexionOuvre (MSSQL)",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_mssql_conn_init(m);
    if (!owl_mssql_conn_open(m, host_buf, port,
                              user ? user : "",
                              pwd  ? pwd  : "",
                              db   ? db   : "",
                              tls_mode,
                              extra_ca,
                              timeout_ms)) {
        char detail_buf[760];
        const char* detail = owl_mssql_conn_last_error(m);
        snprintf(detail_buf, sizeof(detail_buf), "%s%s",
                 detail ? detail : "echec d'ouverture",
                 sql_tls_loopback_hint(host_buf, tls_mode));
        free(m);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (MSSQL)",
                              "native", "SQLConnexionOuvre (MSSQL)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    cnx_set_handle_mssql(cnx, m);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value ferme_mssql(ObjInstance* cnx) {
    OwlMssqlConn* m = cnx_get_handle_mssql(cnx);
    if (m == NULL) { owl_clear_error(); return BOOL_VAL(true); }
    owl_mssql_conn_close(m);
    free(m);
    cnx_set_handle_mssql(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Transforme {nom} -> @P1, @P2, ... pour sp_executesql.
 * Memorise noms dans l'ordre d'apparition (1ere occurrence). Si {nom}
 * reapparait, on reutilise le meme @PN. */
static char* transform_placeholders_mssql(const char* sql,
                                            char*** out_names,
                                            int* out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!sql) return NULL;
    size_t len = strlen(sql);
    char* out = (char*)malloc(len * 6 + 16);   /* {x} -> @P999 = +5 chars */
    if (!out) return NULL;

    char** names = NULL;
    int    count = 0, cap = 0;
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (sql[i] == '{') {
            size_t end = i + 1;
            while (end < len && sql[end] != '}') end++;
            if (end < len) {
                size_t nlen = end - (i + 1);
                int found = -1;
                for (int k = 0; k < count; k++) {
                    if (strlen(names[k]) == nlen
                     && memcmp(names[k], sql + i + 1, nlen) == 0) {
                        found = k; break;
                    }
                }
                int pos1;
                if (found >= 0) {
                    pos1 = found + 1;
                } else {
                    if (count >= cap) {
                        cap = cap ? cap * 2 : 8;
                        char** n = (char**)realloc(names, sizeof(char*) * cap);
                        if (!n) {
                            free(out);
                            for (int k = 0; k < count; k++) free(names[k]);
                            free(names);
                            return NULL;
                        }
                        names = n;
                    }
                    char* nm = (char*)malloc(nlen + 1);
                    memcpy(nm, sql + i + 1, nlen);
                    nm[nlen] = '\0';
                    names[count] = nm;
                    pos1 = ++count;
                }
                int w = snprintf(out + j, 16, "@P%d", pos1);
                if (w < 0) w = 0;
                j += (size_t)w;
                i = end + 1;
                continue;
            }
        }
        out[j++] = sql[i++];
    }
    out[j] = '\0';
    *out_names = names;
    *out_count = count;
    return out;
}

/* Convertit une Value OWL en OwlMssqlRpcParam (pas de copie, pointe dans
 * la string source qu'il faut garder vivante le temps du send). */
static bool owl_value_to_rpc_param(Value v, const char* pname,
                                     OwlMssqlRpcParam* out, const char** err) {
    memset(out, 0, sizeof(*out));
    out->name = pname;
    if (IS_NIL(v)) { out->type = OWL_MSSQL_PARAM_NULL; return true; }
    if (IS_BOOL(v)) {
        out->type = OWL_MSSQL_PARAM_BOOL;
        out->b_value = AS_BOOL(v);
        return true;
    }
    if (IS_INTEGER(v)) {
        out->type = OWL_MSSQL_PARAM_INT;
        out->i_value = AS_INTEGER(v);
        return true;
    }
    if (IS_REAL(v)) {
        out->type = OWL_MSSQL_PARAM_REAL;
        out->r_value = AS_REAL(v);
        return true;
    }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        out->type = OWL_MSSQL_PARAM_STRING_UTF8;
        out->bytes = (const uint8_t*)s->chars;
        out->bytes_len = (size_t)s->length;
        return true;
    }
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        out->type = OWL_MSSQL_PARAM_BUFFER;
        out->bytes = b->bytes;
        out->bytes_len = (size_t)b->length;
        return true;
    }
    *err = "type de valeur non supporte (attendu : chaine, entier, reel, booleen, buffer, NULL)";
    return false;
}

static Value envoie_mssql(ObjInstance* cnx, ObjInstance* req) {
    OwlMssqlConn* m = cnx_get_handle_mssql(cnx);
    if (m == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie (MSSQL)",
                              "native", "SQLEnvoie (MSSQL)",
                              NULL);
        return build_error_reponse();
    }
    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie (MSSQL)",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        return build_error_reponse();
    }
    const char* sql = AS_CSTRING(codeVal);

    /* Parametres : si presents, utilise sp_executesql RPC. */
    bool has_params = false;
    ObjAssocArray* params_assoc = NULL;
    Value paramsVal;
    ObjString* k_params = obj_copy_string("Parametres", 10);
    if (table_get(&req->fields, k_params, &paramsVal) &&
        IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
        params_assoc = AS_ASSOC_ARRAY(paramsVal);
        if (params_assoc->count > 0) has_params = true;
    }

    OwlMssqlResult r;
    owl_mssql_result_init(&r);
    bool ok;
    if (has_params) {
        /* Transforme {nom} -> @P1, @P2, ..., collecte noms dans l'ordre. */
        char** names = NULL;
        int    n_names = 0;
        char* sql2 = transform_placeholders_mssql(sql, &names, &n_names);
        if (!sql2) {
            owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie (MSSQL)",
                                  "native", "SQLEnvoie (MSSQL)",
                                  NULL);
            owl_mssql_result_free(&r);
            return build_error_reponse();
        }
        /* Pour chaque nom collecte, lookup la valeur dans params_assoc et
         * convertit en OwlMssqlRpcParam. Genere "P1", "P2"... comme nom
         * RPC (independant du nom utilisateur). */
        OwlMssqlRpcParam* rpc_params =
            (OwlMssqlRpcParam*)calloc(n_names, sizeof(OwlMssqlRpcParam));
        char* pnames_storage = (char*)malloc(n_names * 8);  /* "P999\0" max 5 char */
        if (!rpc_params || !pnames_storage) {
            free(rpc_params); free(pnames_storage);
            free(sql2);
            for (int k = 0; k < n_names; k++) free(names[k]);
            free(names);
            owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie (MSSQL)",
                                  "native", "SQLEnvoie (MSSQL)",
                                  NULL);
            owl_mssql_result_free(&r);
            return build_error_reponse();
        }
        const char* err_msg = NULL;
        bool conv_ok = true;
        for (int i = 0; i < n_names; ++i) {
            /* Nom RPC = "P{i+1}" */
            char* pname = pnames_storage + i * 8;
            snprintf(pname, 8, "P%d", i + 1);

            /* Cherche names[i] (sans @, sans {}) dans params_assoc. */
            ObjString* key = obj_copy_string(names[i], (int)strlen(names[i]));
            int exists = 0;
            Value v = obj_assoc_get(params_assoc, key, &exists);
            if (!exists) {
                err_msg = "parametre manquant dans Parametres";
                conv_ok = false;
                break;
            }
            if (!owl_value_to_rpc_param(v, pname, &rpc_params[i], &err_msg)) {
                conv_ok = false;
                break;
            }
        }
        if (!conv_ok) {
            char detail_buf[256];
            snprintf(detail_buf, sizeof(detail_buf), "%s", err_msg ? err_msg : "param error");
            free(rpc_params); free(pnames_storage);
            free(sql2);
            for (int k = 0; k < n_names; k++) free(names[k]);
            free(names);
            owl_mssql_result_free(&r);
            owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (MSSQL)",
                                  "native", "SQLEnvoie (MSSQL)",
                                  "detail", detail_buf,
                                  "code", "1",
                                  NULL);
            return build_error_reponse();
        }

        ok = owl_mssql_conn_query_rpc(m, sql2, rpc_params,
                                        (size_t)n_names, &r);

        free(rpc_params); free(pnames_storage);
        free(sql2);
        for (int k = 0; k < n_names; k++) free(names[k]);
        free(names);
    } else {
        ok = owl_mssql_conn_query(m, sql, &r);
    }
    if (!ok) {
        char detail_buf[600];
        const char* detail = owl_mssql_conn_last_error(m);
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 detail ? detail : "(erreur inconnue)");
        owl_mssql_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (MSSQL)",
                              "native", "SQLEnvoie (MSSQL)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return build_error_reponse();
    }
    if (r.kind == TDS_RESULT_ERROR) {
        char detail_buf[600];
        char code_buf[16];
        snprintf(detail_buf, sizeof(detail_buf), "%s", r.error_message);
        snprintf(code_buf, sizeof(code_buf), "%u", (unsigned)r.error_number);
        owl_mssql_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (MSSQL)",
                              "native", "SQLEnvoie (MSSQL)",
                              "detail", detail_buf,
                              "code", code_buf,
                              NULL);
        return build_error_reponse();
    }

    /* Construit Lignes : tableau de tableaux associatifs col_name -> Value. */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));
    for (size_t i = 0; i < r.row_count; ++i) {
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (size_t j = 0; j < r.column_count; ++j) {
            OwlMssqlCell* cell = &r.cells[i * r.column_count + j];
            Value v = owl_mssql_cell_to_owl(&r.columns[j], cell);
            obj_assoc_put_cstr(row, r.columns[j].name,
                               (int)strlen(r.columns[j].name), v);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop(); /* row */
    }

    /* Le compteur du jeton DONE vaut aussi bien "lignes rendues" que "lignes
     * modifiees" : sur une lecture il compte les premieres. La presence de
     * metadonnees de colonnes est le seul signal du protocole qui separe les
     * deux cas. */
    int occurrence = (int)r.row_count;
    int affected   = (r.column_count > 0) ? 0 : (int)r.rows_affected;
    ObjInstance* rep = build_runtime_reponse(true, lignes, occurrence, affected);
    vm_pop(); /* lignes */
    owl_mssql_result_free(&r);
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * ODBC — ouvre / ferme / envoie. Backend generique pour HFSQL,
 * Oracle, Access, et tout SGBD avec un driver ODBC installe.
 * ============================================================ */

#include "sql_odbc_conn.h"
#include "sql_odbc_loader.h"
#include "sql_odbc_query.h"
#include "sql_odbc_types.h"

static OwlOdbcConn* cnx_get_handle_odbc(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlOdbcConn*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle_odbc(ObjInstance* inst, OwlOdbcConn* o) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)o);
}

/* Construit la connection string ODBC generique (sqlODBC) :
 *   1. Si cnx..Source non vide -> utilisee directement.
 *   2. Sinon -> assemblage Pilote / Serveur / BaseDeDonnees /
 *      Utilisateur / MotDePasse. Format DRIVER={...};Server=...;...
 *
 * Pour HFSQL (sqlHFSQL), cf build_hfsql_connection_string ci-dessous —
 * format proprietaire separe.
 *
 * Renvoie chaine malloc, free par caller. NULL sur OOM. */
static char* build_odbc_connection_string(ObjInstance* cnx) {
    const char* source = cnx_get_string_field(cnx, "Source");
    if (source && source[0]) {
        char* dup = (char*)malloc(strlen(source) + 1);
        if (dup) strcpy(dup, source);
        return dup;
    }

    const char* pilote = cnx_get_string_field(cnx, "Pilote");
    const char* serveur = cnx_get_string_field(cnx, "Serveur");
    const char* db = cnx_get_string_field(cnx, "BaseDeDonnees");
    const char* user = cnx_get_string_field(cnx, "Utilisateur");
    const char* pwd = cnx_get_string_field(cnx, "MotDePasse");

    size_t cap = 128
        + (pilote  ? strlen(pilote)  : 0)
        + (serveur ? strlen(serveur) : 0)
        + (db      ? strlen(db)      : 0)
        + (user    ? strlen(user)    : 0)
        + (pwd     ? strlen(pwd)     : 0);
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t off = 0;
    out[0] = '\0';
    if (pilote  && pilote[0])  off += (size_t)snprintf(out + off, cap - off,
                                            "DRIVER={%s};", pilote);
    if (serveur && serveur[0]) off += (size_t)snprintf(out + off, cap - off,
                                            "Server=%s;", serveur);
    if (db      && db[0])      off += (size_t)snprintf(out + off, cap - off,
                                            "Database=%s;", db);
    if (user    && user[0])    off += (size_t)snprintf(out + off, cap - off,
                                            "UID=%s;", user);
    if (pwd     && pwd[0])     off += (size_t)snprintf(out + off, cap - off,
                                            "PWD=%s;", pwd);
    return out;
}

/* Construit la connection string HFSQL Client/Serveur (sqlHFSQL).
 *
 * Ne fait que lire les champs : l'assemblage vit dans sql_cnx_string.c, ou il
 * est pur et donc assertable par --self-test-net (aucune instance HFSQL n'est
 * disponible en integration continue).
 *
 * cnx..Source court-circuite l'assemblage si fournie (user expert). */
static char* build_hfsql_connection_string(ObjInstance* cnx) {
    const char* source = cnx_get_string_field(cnx, "Source");
    if (source && source[0]) {
        char* dup = (char*)malloc(strlen(source) + 1);
        if (dup) strcpy(dup, source);
        return dup;
    }

    return owl_sql_build_hfsql_cnx_string(
        cnx_get_string_field(cnx, "Serveur"),
        cnx_get_int_field(cnx, "Port", 0),
        cnx_get_string_field(cnx, "BaseDeDonnees"),
        cnx_get_string_field(cnx, "Utilisateur"),
        cnx_get_string_field(cnx, "MotDePasse"));
}

/* Construit la connection string HFSQL Classic (sqlHFSQLClassic) — fichiers
 * .fic locaux, sans serveur. Format du driver :
 *   DRIVER={HFSQL};ANA=<analyse .wdd>;REP=<dossier .fic>;IntegrityCheck=1
 *
 * ANA = fichier d'analyse (schema, cnx..Analyse), obligatoire.
 * REP = dossier des fichiers de donnees (cnx..Repertoire), obligatoire.
 * IntegrityCheck=1 : fige en dur (pas de champ expose).
 * cnx..Source court-circuite l'assemblage si fournie (user expert). */
static char* build_hfsql_classic_connection_string(ObjInstance* cnx) {
    const char* source = cnx_get_string_field(cnx, "Source");
    if (source && source[0]) {
        char* dup = (char*)malloc(strlen(source) + 1);
        if (dup) strcpy(dup, source);
        return dup;
    }

    const char* ana = cnx_get_string_field(cnx, "Analyse");
    const char* rep = cnx_get_string_field(cnx, "Repertoire");
    if (!ana || !ana[0] || !rep || !rep[0]) {
        /* Champ obligatoire manquant : le dispatch a deja emis l'erreur nommee.
         * On renvoie NULL pour signaler l'echec (jamais atteint en pratique). */
        return NULL;
    }

    size_t cap = 64 + strlen(ana) + strlen(rep);
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap, "DRIVER={HFSQL};ANA=%s;REP=%s;IntegrityCheck=1", ana, rep);
    return out;
}

/* Ouvre une connexion ODBC en passant par le Driver Manager systeme
 * (odbc32 Windows, unixODBC Linux a venir). Pour HFSQL specifiquement,
 * cf ouvre_hfsql() qui bypass le DM et charge le driver embarque. */
static Value ouvre_odbc(ObjInstance* cnx) {
    char* conn_str = build_odbc_connection_string(cnx);
    if (!conn_str || !conn_str[0]) {
        free(conn_str);
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (ODBC)",
                              "native", "SQLConnexionOuvre (ODBC)",
                              "arg", "Source/Pilote/Serveur",
                              NULL);
        return BOOL_VAL(false);
    }

    char vt_err[512] = {0};
    const struct OwlOdbcVT* vt = owl_odbc_vt_load_default(vt_err, sizeof(vt_err));
    if (!vt) {
        char detail_buf[700];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 vt_err[0] ? vt_err : "VT load_default echec");
        free(conn_str);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (ODBC)",
                              "native", "SQLConnexionOuvre (ODBC)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }

    int timeout_s = cnx_timeout_seconds(cnx);

    OwlOdbcConn* o = (OwlOdbcConn*)calloc(1, sizeof(OwlOdbcConn));
    if (!o) {
        free(conn_str);
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLConnexionOuvre (ODBC)",
                              "native", "SQLConnexionOuvre (ODBC)",
                              NULL);
        return BOOL_VAL(false);
    }
    owl_odbc_conn_init(o);
    if (!owl_odbc_conn_open(o, vt, conn_str, timeout_s)) {
        char detail_buf[600];
        const char* detail = owl_odbc_conn_last_error(o);
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 detail ? detail : "echec d'ouverture");
        free(o);
        free(conn_str);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre (ODBC)",
                              "native", "SQLConnexionOuvre (ODBC)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    free(conn_str);
    cnx_set_handle_odbc(cnx, o);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Partie commune C/S et Classic : charge le driver HFSQL embarque (bypass
 * Driver Manager) et ouvre la connexion a partir d'une chaine deja construite.
 * Prend possession de conn_str (free en interne, y compris sur echec). */
static Value ouvre_hfsql_avec_chaine(ObjInstance* cnx, char* conn_str, const char* fn) {
    char vt_err[512] = {0};
    const struct OwlOdbcVT* vt = owl_odbc_vt_load_hfsql_direct(vt_err, sizeof(vt_err));
    if (!vt) {
        char detail_buf[700];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 vt_err[0] ? vt_err : "load_hfsql_direct echec");
        free(conn_str);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, fn,
                              "native", fn,
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }

    int timeout_s = cnx_timeout_seconds(cnx);

    OwlOdbcConn* o = (OwlOdbcConn*)calloc(1, sizeof(OwlOdbcConn));
    if (!o) {
        free(conn_str);
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, fn,
                              "native", fn,
                              NULL);
        return BOOL_VAL(false);
    }
    owl_odbc_conn_init(o);
    if (!owl_odbc_conn_open(o, vt, conn_str, timeout_s)) {
        char detail_buf[600];
        const char* detail = owl_odbc_conn_last_error(o);
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 detail ? detail : "echec d'ouverture");
        free(o);
        free(conn_str);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, fn,
                              "native", fn,
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return BOOL_VAL(false);
    }
    free(conn_str);
    cnx_set_handle_odbc(cnx, o);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Ouvre une connexion HFSQL Client/Serveur via le driver HFSQL
 * embarque + bypass complet du Driver Manager. Au 1er usage, extrait
 * le pack vers %LOCALAPPDATA%/owl/hfsql_driver/<ver>/ + LoadLibrary
 * direct sur le driver ODBC embarque. Zero install user, zero registry.
 *
 * Note sur cnx..ModeTLS et le chiffrement HFSQL :
 *
 * HFSQL chiffre BIEN le transport client/serveur, mais avec son
 * propre systeme (PAS TLS standard / X.509 / PKI) :
 *   - Algos : RC5_16, AES128/192/256 (propriete canon "Cryptage" cote
 *     API native).
 *   - Configure cote SERVEUR via le Control Center HFSQL — l'admin
 *     peut "interdire les connexions sans cryptage".
 *   - Negocie automatiquement par le driver a la connexion : si le
 *     serveur impose, le client accepte ; sinon, c'est en clair.
 *
 * Cote CONNECTION STRING ODBC HFSQL : la propriete Cryptage est
 * documentee comme "Not available with these connection types" pour
 * ODBC. Aucun mot-cle Encrypt/SSL/TLS cote ODBC.
 * Le controle est PUREMENT cote serveur — l'app OWL ne peut ni
 * forcer, ni refuser, ni inspecter le mode de chiffrement via le
 * driver ODBC. Pour avoir ce controle, il faudrait passer par l'API
 * canon native (HOuvreConnexion + .Cryptage), pas disponible ici.
 *
 * Consequence : cnx..ModeTLS est ignore pour HFSQL, mais on emet un
 * warning runtime sur stderr si != tlsDesactive pour eviter la fausse
 * impression de securite. Le user doit set explicitement tlsDesactive
 * pour silencer (signal explicite "je sais que je delegue au serveur"). */
static Value ouvre_hfsql(ObjInstance* cnx) {
    /* Warning ModeTLS non-applicable. Le defaut sqlConnexion = tlsVerifieComplet
     * (pour Postgres/MySQL/MSSQL ou ca marche reellement). Sur HFSQL, ce
     * defaut est trompeur : le user peut croire qu'il a un TLS verify-full
     * alors que c'est le serveur HFSQL qui decide tout. On previent au 1er
     * usage de ouvre — l'user qui assume met cn..ModeTLS = tlsDesactive
     * pour silencer. */
    int64_t tls_mode = cnx_get_int_field(cnx, "ModeTLS", OWL_SQL_TLS_VERIFIE_COMPLET);
    if (tls_mode != OWL_SQL_TLS_DESACTIVE) {
        fprintf(stderr,
            "[OWL] AVERTISSEMENT sqlHFSQL : cnx..ModeTLS est ignore (le driver "
            "ODBC HFSQL ne supporte pas le controle TLS cote client). "
            "Le chiffrement transport est negocie cote serveur HFSQL. "
            "Pour silencer ce warning : "
            "cnx..ModeTLS = tlsDesactive (signale que tu en es conscient).\n");
    }

    char* conn_str = build_hfsql_connection_string(cnx);
    if (!conn_str || !conn_str[0]) {
        free(conn_str);
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (HFSQL)",
                              "native", "SQLConnexionOuvre (HFSQL)",
                              "arg", "Serveur/Source",
                              NULL);
        return BOOL_VAL(false);
    }
    return ouvre_hfsql_avec_chaine(cnx, conn_str, "SQLConnexionOuvre (HFSQL)");
}

/* Ouvre une connexion HFSQL Classic (sqlHFSQLClassic) — fichiers .fic locaux,
 * sans serveur. Meme driver embarque + bypass DM que le mode Client/Serveur,
 * mais chaine de connexion locale (ANA/REP). AUCUN warning ModeTLS : acces
 * local, zero transport reseau — le warning C/S n'a pas de sens ici. */
static Value ouvre_hfsql_classic(ObjInstance* cnx) {
    /* Mode expert : Source renseignee court-circuite (chaine ODBC brute). */
    const char* source = cnx_get_string_field(cnx, "Source");
    if (!(source && source[0])) {
        /* Sinon Analyse ET Repertoire obligatoires. Message nomme le champ. */
        const char* ana = cnx_get_string_field(cnx, "Analyse");
        if (!ana || !ana[0]) {
            owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (HFSQL Classic)",
                                  "native", "SQLConnexionOuvre (HFSQL Classic)",
                                  "arg", "Analyse",
                                  NULL);
            return BOOL_VAL(false);
        }
        const char* rep = cnx_get_string_field(cnx, "Repertoire");
        if (!rep || !rep[0]) {
            owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (HFSQL Classic)",
                                  "native", "SQLConnexionOuvre (HFSQL Classic)",
                                  "arg", "Repertoire",
                                  NULL);
            return BOOL_VAL(false);
        }
    }

    char* conn_str = build_hfsql_classic_connection_string(cnx);
    if (!conn_str || !conn_str[0]) {
        free(conn_str);
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre (HFSQL Classic)",
                              "native", "SQLConnexionOuvre (HFSQL Classic)",
                              "arg", "Analyse/Repertoire",
                              NULL);
        return BOOL_VAL(false);
    }
    return ouvre_hfsql_avec_chaine(cnx, conn_str, "SQLConnexionOuvre (HFSQL Classic)");
}

static Value ferme_odbc(ObjInstance* cnx) {
    OwlOdbcConn* o = cnx_get_handle_odbc(cnx);
    if (o == NULL) { owl_clear_error(); return BOOL_VAL(true); }
    owl_odbc_conn_close(o);
    free(o);
    cnx_set_handle_odbc(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* Transforme {nom} -> ? positionnel. Memorise l'ordre des noms dans
 * out_names (1-based, doublons gardent meme position). Renvoie SQL
 * transforme malloc, free par caller. */
static char* transform_placeholders_odbc(const char* sql,
                                           char*** out_names, int* out_count) {
    *out_names = NULL;
    *out_count = 0;
    if (!sql) return NULL;
    size_t len = strlen(sql);
    char* out = (char*)malloc(len + 16);   /* {nom} -> ? le SQL retrecit toujours */
    if (!out) return NULL;
    char** names = NULL;
    int    count = 0, cap = 0;
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (sql[i] == '{') {
            size_t end = i + 1;
            while (end < len && sql[end] != '}') end++;
            if (end < len) {
                size_t nlen = end - (i + 1);
                int found = -1;
                for (int k = 0; k < count; k++) {
                    if (strlen(names[k]) == nlen
                     && memcmp(names[k], sql + i + 1, nlen) == 0) {
                        found = k; break;
                    }
                }
                if (found < 0) {
                    if (count >= cap) {
                        cap = cap ? cap * 2 : 8;
                        char** n = (char**)realloc(names, sizeof(char*) * cap);
                        if (!n) {
                            free(out);
                            for (int k = 0; k < count; k++) free(names[k]);
                            free(names);
                            return NULL;
                        }
                        names = n;
                    }
                    char* nm = (char*)malloc(nlen + 1);
                    memcpy(nm, sql + i + 1, nlen);
                    nm[nlen] = '\0';
                    names[count++] = nm;
                }
                /* Note : ODBC '?' est positionnel, donc si {nom} apparait
                 * plusieurs fois, on doit emettre plusieurs '?' avec des
                 * params dupliques cote bind. On insere a chaque fois. */
                out[j++] = '?';
                i = end + 1;
                continue;
            }
        }
        out[j++] = sql[i++];
    }
    out[j] = '\0';
    *out_names = names;
    *out_count = count;
    return out;
}

/* Convertit Value -> OwlOdbcParam (non-owning sur bytes). */
static bool owl_value_to_odbc_param(Value v, OwlOdbcParam* out,
                                      const char** err) {
    memset(out, 0, sizeof(*out));
    if (IS_NIL(v))     { out->kind = OWL_ODBC_PARAM_NULL; return true; }
    if (IS_BOOL(v))    { out->kind = OWL_ODBC_PARAM_BOOL; out->b_val = AS_BOOL(v); return true; }
    if (IS_INTEGER(v)) { out->kind = OWL_ODBC_PARAM_INTEGER; out->i_val = AS_INTEGER(v); return true; }
    if (IS_REAL(v))    { out->kind = OWL_ODBC_PARAM_REAL; out->r_val = AS_REAL(v); return true; }
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        out->kind = OWL_ODBC_PARAM_STRING_UTF8;
        out->bytes = (const uint8_t*)s->chars;
        out->bytes_len = (size_t)s->length;
        return true;
    }
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        out->kind = OWL_ODBC_PARAM_BUFFER;
        out->bytes = b->bytes;
        out->bytes_len = (size_t)b->length;
        return true;
    }
    *err = "type non supporte (chaine/entier/reel/booleen/buffer/NULL)";
    return false;
}

static Value envoie_odbc(ObjInstance* cnx, ObjInstance* req) {
    OwlOdbcConn* o = cnx_get_handle_odbc(cnx);
    if (o == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie (ODBC)",
                              "native", "SQLEnvoie (ODBC)",
                              NULL);
        return build_error_reponse();
    }
    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie (ODBC)",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        return build_error_reponse();
    }
    const char* sql = AS_CSTRING(codeVal);

    /* Params ? */
    bool has_params = false;
    ObjAssocArray* params_assoc = NULL;
    Value paramsVal;
    ObjString* k_params = obj_copy_string("Parametres", 10);
    if (table_get(&req->fields, k_params, &paramsVal) &&
        IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
        params_assoc = AS_ASSOC_ARRAY(paramsVal);
        if (params_assoc->count > 0) has_params = true;
    }

    OwlOdbcResult r;
    owl_odbc_result_init(&r);
    bool ok;
    if (has_params) {
        char** names = NULL;
        int    n_names = 0;
        char* sql2 = transform_placeholders_odbc(sql, &names, &n_names);
        if (!sql2) {
            owl_odbc_result_free(&r);
            owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie (ODBC)",
                                  "native", "SQLEnvoie (ODBC)",
                                  NULL);
            return build_error_reponse();
        }
        /* Note : ODBC '?' est positionnel, et chaque occurrence de {nom}
         * dans le SQL devient un '?' separe. Si {nom} apparait 2 fois,
         * il faut 2 binds avec la meme valeur. On compte les '?' dans sql2. */
        int n_qmark = 0;
        for (const char* p = sql2; *p; p++) if (*p == '?') n_qmark++;

        OwlOdbcParam* odbc_params =
            (OwlOdbcParam*)calloc((size_t)n_qmark, sizeof(OwlOdbcParam));
        if (!odbc_params) {
            free(sql2);
            for (int k = 0; k < n_names; k++) free(names[k]);
            free(names);
            owl_odbc_result_free(&r);
            owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie (ODBC)",
                                  "native", "SQLEnvoie (ODBC)",
                                  NULL);
            return build_error_reponse();
        }
        /* On itere sur sql2 / sql pour matcher chaque ? avec son nom
         * d'origine dans l'ordre. Plus simple : on retransforme et on
         * tracke. Mais on a deja l'ordre via names[] : chaque '?'
         * correspond au nom de la 1ere occurrence... hmm non, si {nom}
         * apparait 2x on a 2 '?' mais 1 entry dans names. Faut recompter.
         *
         * Approche : re-itere sur le SQL original, et pour chaque {nom}
         * trouve, lookup son index dans names[] -> param value -> push
         * dans odbc_params[i++]. */
        const char* err_msg = NULL;
        bool conv_ok = true;
        int qi = 0;
        for (size_t i = 0; sql[i] && conv_ok; ) {
            if (sql[i] == '{') {
                size_t end = i + 1;
                while (sql[end] && sql[end] != '}') end++;
                if (sql[end] == '}') {
                    size_t nlen = end - (i + 1);
                    ObjString* key = obj_copy_string(sql + i + 1, (int)nlen);
                    int exists = 0;
                    Value v = obj_assoc_get(params_assoc, key, &exists);
                    if (!exists) {
                        err_msg = "parametre manquant dans Parametres";
                        conv_ok = false;
                        break;
                    }
                    if (!owl_value_to_odbc_param(v, &odbc_params[qi++], &err_msg)) {
                        conv_ok = false;
                        break;
                    }
                    i = end + 1;
                    continue;
                }
            }
            i++;
        }

        if (conv_ok) {
            ok = owl_odbc_simple_query_params(o, sql2, odbc_params,
                                                 (size_t)n_qmark, &r);
        } else {
            char detail_buf[256];
            snprintf(detail_buf, sizeof(detail_buf), "%s",
                     err_msg ? err_msg : "param error");
            owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (ODBC)",
                                  "native", "SQLEnvoie (ODBC)",
                                  "detail", detail_buf,
                                  "code", "1",
                                  NULL);
            ok = false;
        }

        free(odbc_params);
        free(sql2);
        for (int k = 0; k < n_names; k++) free(names[k]);
        free(names);
    } else {
        ok = owl_odbc_simple_query(o, sql, &r);
    }

    if (!ok || !r.success) {
        char detail_buf[600];
        const char* em = r.error_message[0] ? r.error_message
                                            : owl_odbc_conn_last_error(o);
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 em ? em : "(erreur inconnue)");
        owl_odbc_result_free(&r);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie (ODBC)",
                              "native", "SQLEnvoie (ODBC)",
                              "detail", detail_buf,
                              "code", "1",
                              NULL);
        return build_error_reponse();
    }

    /* Materialise les Lignes */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));
    for (size_t i = 0; i < r.row_count; ++i) {
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (size_t j = 0; j < r.column_count; ++j) {
            OwlOdbcCell* cell = &r.cells[i * r.column_count + j];
            Value v = owl_odbc_cell_to_owl(cell);
            obj_assoc_put_cstr(row, r.columns[j].name,
                               (int)strlen(r.columns[j].name), v);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop();
    }
    /* SQLRowCount n'est defini que pour le DML ; sur un jeu de resultats sa
     * valeur est laissee au pilote (souvent le nombre de lignes rendues, ou
     * -1 pour "indisponible"). Le nombre de colonnes decrit tranche, et le
     * negatif ne franchit pas la frontiere. */
    int occurrence = (int)r.row_count;
    int affected   = (r.column_count > 0 || r.rows_affected < 0)
                         ? 0 : (int)r.rows_affected;
    ObjInstance* rep = build_runtime_reponse(true, lignes, occurrence, affected);
    vm_pop();
    owl_odbc_result_free(&r);
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * Natives — SQLConnexionOuvre / SQLConnexionFerme (SQL.2.b).
 * SQLEnvoie reste stubbe (SQL.2.c).
 * ============================================================ */

static Value fn_sql_connexion_ouvre(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* cnx = resolve_sql_cnx(args[0], "SQLConnexionOuvre");
    if (cnx == NULL) return BOOL_VAL(false);

    /* Deja ouvert ? */
    if (cnx_get_handle(cnx) != NULL) {
        owl_emit_native_error(E_NATIVE_SQL_ALREADY_OPEN, "SQLConnexionOuvre",
                              "native", "SQLConnexionOuvre",
                              NULL);
        return BOOL_VAL(false);
    }

    /* Dispatch selon Provider. Point unique : la garde des proprietes
     * inapplicables vit ici, pas dupliquee dans les ouvre_*. */
    int64_t provider = cnx_get_int_field(cnx, "Provider", OWL_SQL_SQLITE);
    sql_warn_props_inapplicables(cnx, provider);
    if (provider == OWL_SQL_POSTGRES) return ouvre_postgres(cnx);
    if (provider == OWL_SQL_MYSQL)    return ouvre_mysql   (cnx);
    if (provider == OWL_SQL_DUCKDB)   return ouvre_duckdb  (cnx);
    if (provider == OWL_SQL_MSSQL)    return ouvre_mssql   (cnx);
    if (provider == OWL_SQL_ODBC)     return ouvre_odbc    (cnx);
    if (provider == OWL_SQL_HFSQL)    return ouvre_hfsql   (cnx);
    if (provider == OWL_SQL_HFSQL_CLASSIC) return ouvre_hfsql_classic(cnx);
    if (provider != OWL_SQL_SQLITE) {
        owl_emit_native_error(E_NATIVE_SQL_PROVIDER_UNKNOWN, "SQLConnexionOuvre",
                              "native", "SQLConnexionOuvre",
                              NULL);
        return BOOL_VAL(false);
    }

    /* Source = chemin du fichier SQLite. ":memory:" autorise. */
    const char* source = cnx_get_string_field(cnx, "Source");
    if (source == NULL || source[0] == '\0') {
        owl_emit_native_error(E_NATIVE_ARG_MISSING, "SQLConnexionOuvre",
                              "native", "SQLConnexionOuvre",
                              "arg", "Source",
                              NULL);
        return BOOL_VAL(false);
    }

    /* Ouvre la base. READWRITE | CREATE = comportement SQLite par defaut. */
    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(source, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        char detail_buf[256];
        char code_buf[16];
        snprintf(detail_buf, sizeof(detail_buf), "%s",
                 db ? sqlite3_errmsg(db) : "open failed");
        snprintf(code_buf, sizeof(code_buf), "%d", rc);
        if (db) sqlite3_close(db);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionOuvre",
                              "native", "SQLConnexionOuvre",
                              "detail", detail_buf,
                              "code", code_buf,
                              NULL);
        return BOOL_VAL(false);
    }

    /* Active WAL mode : meilleure concurrence (lecteurs + 1 ecrivain).
     * Ignore si fichier ":memory:" ou systeme qui ne supporte pas le WAL. */
    if (strcmp(source, ":memory:") != 0) {
        sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    }

    /* Stocke le handle. */
    cnx_set_handle(cnx, db);
    owl_clear_error();
    return BOOL_VAL(true);
}

static Value fn_sql_connexion_ferme(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* cnx = resolve_sql_cnx(args[0], "SQLConnexionFerme");
    if (cnx == NULL) return BOOL_VAL(false);

    /* Dispatch selon Provider. */
    int64_t provider = cnx_get_int_field(cnx, "Provider", OWL_SQL_SQLITE);
    if (provider == OWL_SQL_POSTGRES) return ferme_postgres(cnx);
    if (provider == OWL_SQL_MYSQL)    return ferme_mysql   (cnx);
    if (provider == OWL_SQL_DUCKDB)   return ferme_duckdb  (cnx);
    if (provider == OWL_SQL_MSSQL)    return ferme_mssql   (cnx);
    if (provider == OWL_SQL_ODBC
     || provider == OWL_SQL_HFSQL
     || provider == OWL_SQL_HFSQL_CLASSIC) return ferme_odbc    (cnx);

    sqlite3* db = cnx_get_handle(cnx);
    if (db == NULL) {
        /* Deja fermee : no-op silencieux (idempotent). */
        owl_clear_error();
        return BOOL_VAL(true);
    }

    /* sqlite3_close echoue si des statements pending sont attaches.
     * Pour SQL.2.b on n'a pas encore de statements. SQL.2.c devra
     * tracker et finaliser les enfants avant close. */
    int rc = sqlite3_close(db);
    if (rc != SQLITE_OK) {
        char detail_buf[256];
        char code_buf[16];
        snprintf(detail_buf, sizeof(detail_buf),
                 "%s (statements pending ?)", sqlite3_errmsg(db));
        snprintf(code_buf, sizeof(code_buf), "%d", rc);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLConnexionFerme",
                              "native", "SQLConnexionFerme",
                              "detail", detail_buf,
                              "code", code_buf,
                              NULL);
        return BOOL_VAL(false);
    }

    cnx_set_handle(cnx, NULL);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * SQL.2.c : SQLEnvoie — prepare + bind + step + sqlReponse.
 *
 * Pipeline :
 *  1. Verifie cnx ouverte + req est sqlRequete.
 *  2. Lit req.Code, transforme {nom} -> :nom.
 *  3. sqlite3_prepare_v2.
 *  4. Pour chaque entree de req.Parametres : sqlite3_bind_* type.
 *  5. Boucle sqlite3_step : recupere les colonnes via column_to_value
 *     et construit un assoc array col_name -> valeur, append dans
 *     rep.Lignes.
 *  6. sqlite3_finalize, construit sqlReponse, retourne.
 *
 * Securite : les valeurs des parametres sont passees via sqlite3_bind_*
 * et JAMAIS injectees textuellement dans le SQL. Le SQL final ne
 * contient que des placeholders ":nom" — les valeurs vivent dans le
 * tableau de bindings du moteur, jamais re-parsees.
 * ============================================================ */

/* Transforme {nom} en :nom dans le SQL.
 * Buffer alloue via malloc, a free par le caller. */
static char* transform_placeholders(const char* sql) {
    if (sql == NULL) return NULL;
    size_t len = strlen(sql);
    /* Worst case = meme taille (chaque {x} -> :x, taille identique). */
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (sql[i] == '{') {
            size_t end = i + 1;
            while (end < len && sql[end] != '}') end++;
            if (end < len) {
                /* Replace {nom} par :nom */
                out[j++] = ':';
                for (size_t k = i + 1; k < end; k++) out[j++] = sql[k];
                i = end + 1;
                continue;
            }
        }
        out[j++] = sql[i++];
    }
    out[j] = '\0';
    return out;
}

/* Bind un Value OWL dans un slot SQLite. Retourne SQLITE_OK ou code
 * d'erreur. SQLITE_MISMATCH si type non supporte. */
static int bind_value(sqlite3_stmt* stmt, int idx, Value v) {
    if (IS_NIL(v))     return sqlite3_bind_null(stmt, idx);
    if (IS_BOOL(v))    return sqlite3_bind_int(stmt, idx, AS_BOOL(v) ? 1 : 0);
    if (IS_INTEGER(v)) return sqlite3_bind_int64(stmt, idx, AS_INTEGER(v));
    if (IS_REAL(v))    return sqlite3_bind_double(stmt, idx, AS_REAL(v));
    if (IS_STRING(v)) {
        ObjString* s = AS_STRING(v);
        return sqlite3_bind_text(stmt, idx, s->chars, s->length, SQLITE_TRANSIENT);
    }
    if (IS_BUFFER(v)) {
        ObjBuffer* b = AS_BUFFER(v);
        /* SQLite interprete un pointeur NULL comme NULL. Pour un buffer
         * vide (b->bytes peut etre NULL), on bind un blob de longueur 0
         * explicite via un pointeur statique non-null. */
        if (b->length == 0) return sqlite3_bind_zeroblob(stmt, idx, 0);
        return sqlite3_bind_blob(stmt, idx, b->bytes, b->length, SQLITE_TRANSIENT);
    }
    /* Date/Heure/DateHeure et autres types complexes : non supportes en V1.
     * Pour binder une date, l'user fait `req.Parametres["d"] = MaDate.VersChaine()`
     * ou stocke directement en TEXT canon OWL "AAAAMMJJ" / "AAAAMMJJHHMMSSFFF". */
    return SQLITE_MISMATCH;
}

/* Convertit une colonne SQLite en Value OWL. */
static Value column_to_value(sqlite3_stmt* stmt, int col) {
    int type = sqlite3_column_type(stmt, col);
    switch (type) {
        case SQLITE_INTEGER:
            return INTEGER_VAL(sqlite3_column_int64(stmt, col));
        case SQLITE_FLOAT:
            return REAL_VAL(sqlite3_column_double(stmt, col));
        case SQLITE_TEXT: {
            const char* text = (const char*)sqlite3_column_text(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            ObjString* s = obj_copy_string(text, len);
            return OBJ_VAL(s);
        }
        case SQLITE_BLOB: {
            const void* data = sqlite3_column_blob(stmt, col);
            int len = sqlite3_column_bytes(stmt, col);
            ObjBuffer* b = obj_new_buffer(data, len);
            return OBJ_VAL(b);
        }
        case SQLITE_NULL:
        default:
            return NIL_VAL;
    }
}

/* Construit un sqlReponse au runtime. Lignes doit etre push sur la
 * VM stack par le caller (pour proteger du GC pendant l'allocation
 * des fields de rep). */
static ObjInstance* build_runtime_reponse(bool success, ObjArray* lignes,
                                           int occurrence, int affected) {
    ObjString* nm = obj_copy_string("sqlReponse", 10);
    vm_push(OBJ_VAL(nm));
    ObjInstance* rep = obj_new_instance(nm);
    vm_push(OBJ_VAL(rep));

    inst_field_set_bool(rep, "Succes",          success);
    /*  : delegue au canon inst_field_set_value. */
    inst_field_set_value(rep, "Lignes", OBJ_VAL(lignes));
    inst_field_set_int(rep, "Occurrence",      occurrence);
    inst_field_set_int(rep, "LignesAffectees", affected);

    vm_pop(); /* rep */
    vm_pop(); /* nm */
    return rep;
}

/* Build a fresh empty sqlReponse with Succes=false, no rows, no affected. */
static Value build_error_reponse(void) {
    ObjArray* empty = obj_new_array();
    vm_push(OBJ_VAL(empty));
    ObjInstance* rep = build_runtime_reponse(false, empty, 0, 0);
    vm_pop();
    return OBJ_VAL(rep);
}

static Value fn_sql_envoie(int argCount, Value* args) {
    (void)argCount;

    /* Validation cnx + handle ouvert. */
    ObjInstance* cnx = resolve_sql_cnx(args[0], "SQLEnvoie");
    if (cnx == NULL) return build_error_reponse();

    /* Validation requete (commune SQLite + Postgres). */
    if (!IS_INSTANCE(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SQLEnvoie",
                              "native", "SQLEnvoie",
                              "arg", "2",
                              "expected", "sqlRequete",
                              "actual", "un autre type",
                              NULL);
        return build_error_reponse();
    }
    ObjInstance* req = AS_INSTANCE(args[1]);
    if (req->structName == NULL ||
        strcmp(req->structName->chars, "sqlRequete") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, "SQLEnvoie",
                              "native", "SQLEnvoie",
                              "arg", "2",
                              "expected", "sqlRequete",
                              "actual", req->structName ? req->structName->chars : "<anonyme>",
                              NULL);
        return build_error_reponse();
    }

    /* Dispatch selon Provider. */
    int64_t provider = cnx_get_int_field(cnx, "Provider", OWL_SQL_SQLITE);
    if (provider == OWL_SQL_POSTGRES) return envoie_postgres(cnx, req);
    if (provider == OWL_SQL_MYSQL)    return envoie_mysql   (cnx, req);
    if (provider == OWL_SQL_DUCKDB)   return envoie_duckdb  (cnx, req);
    if (provider == OWL_SQL_MSSQL)    return envoie_mssql   (cnx, req);
    if (provider == OWL_SQL_ODBC
     || provider == OWL_SQL_HFSQL
     || provider == OWL_SQL_HFSQL_CLASSIC) return envoie_odbc    (cnx, req);

    /* Path SQLite (defaut). */
    sqlite3* db = cnx_get_handle(cnx);
    if (db == NULL) {
        owl_emit_native_error(E_NATIVE_SQL_NOT_OPEN, "SQLEnvoie",
                              "native", "SQLEnvoie",
                              NULL);
        return build_error_reponse();
    }

    /* Lire req.Code. */
    Value codeVal;
    ObjString* k_code = obj_copy_string("Code", 4);
    bool codePresent = table_get(&req->fields, k_code, &codeVal);
    if (!codePresent || !IS_STRING(codeVal)) {
        owl_emit_native_error(codePresent ? E_NATIVE_ARG_TYPE_MISMATCH
                                          : E_NATIVE_ARG_MISSING,
                              "SQLEnvoie",
                              "arg", "requete.Code",
                              "expected", "chaîne",
                              "actual", "autre type",
                              NULL);
        return build_error_reponse();
    }
    const char* originalSql = AS_CSTRING(codeVal);

    /* Transforme {nom} -> :nom. */
    char* finalSql = transform_placeholders(originalSql);
    if (finalSql == NULL) {
        owl_emit_native_error(E_NATIVE_OUT_OF_MEMORY, "SQLEnvoie",
                              "native", "SQLEnvoie",
                              NULL);
        return build_error_reponse();
    }

    /* Prepare. */
    sqlite3_stmt* stmt = NULL;
    int rc = sqlite3_prepare_v2(db, finalSql, -1, &stmt, NULL);
    free(finalSql);
    if (rc != SQLITE_OK) {
        char detail_buf[512];
        char code_buf[16];
        snprintf(detail_buf, sizeof(detail_buf), "prepare: %s",
                 sqlite3_errmsg(db));
        snprintf(code_buf, sizeof(code_buf), "%d", rc);
        if (stmt) sqlite3_finalize(stmt);
        owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie",
                              "native", "SQLEnvoie",
                              "detail", detail_buf,
                              "code", code_buf,
                              NULL);
        return build_error_reponse();
    }

    /* Bind parametres depuis req.Parametres (assoc array, optionnel).
     * Stockage ASCII canon — req.Paramètres et req.Parametres pointent
     * vers le meme field grace a l'accent-strip du compilateur OWL. */
    {
        Value paramsVal;
        ObjString* k_params = obj_copy_string("Parametres", 10);
        if (table_get(&req->fields, k_params, &paramsVal) &&
            IS_OBJ(paramsVal) && OBJ_TYPE(paramsVal) == OBJ_ASSOC_ARRAY) {
            ObjAssocArray* params = AS_ASSOC_ARRAY(paramsVal);
            for (int i = 0; i < params->count; i++) {
                ObjString* key = params->keys[i];
                Value val      = params->values[i];
                char paramName[256];
                snprintf(paramName, sizeof(paramName), ":%s", key->chars);
                int idx = sqlite3_bind_parameter_index(stmt, paramName);
                if (idx == 0) continue; /* extra param non utilise — ignore */
                int br = bind_value(stmt, idx, val);
                if (br != SQLITE_OK) {
                    char detail_buf[256];
                    char code_buf[16];
                    snprintf(detail_buf, sizeof(detail_buf),
                             "bind {%s}: type non supporte", key->chars);
                    snprintf(code_buf, sizeof(code_buf), "%d", br);
                    sqlite3_finalize(stmt);
                    owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie",
                                          "native", "SQLEnvoie",
                                          "detail", detail_buf,
                                          "code", code_buf,
                                          NULL);
                    return build_error_reponse();
                }
            }
        }
    }

    /* Crée le tableau de lignes. Push pour proteger du GC pendant le step
     * (qui alloue strings/buffers/assoc). */
    ObjArray* lignes = obj_new_array();
    vm_push(OBJ_VAL(lignes));

    int occurrence = 0;
    int colCount   = sqlite3_column_count(stmt);

    /* sqlite3_changes() est un compteur DE CONNEXION : il conserve la valeur
     * de la derniere ecriture tant qu'aucune autre n'a eu lieu. Le delta de
     * total_changes autour du step dit si CETTE requete a ecrit ; lui seul
     * autorise a publier le compteur. sqlite3_stmt_readonly() ne convient pas :
     * il classe le DDL (CREATE TABLE...) comme ecriture. */
    sqlite3_int64 changesAvant = sqlite3_total_changes64(db);

    while (true) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            char detail_buf[512];
            char code_buf[16];
            snprintf(detail_buf, sizeof(detail_buf), "step: %s",
                     sqlite3_errmsg(db));
            snprintf(code_buf, sizeof(code_buf), "%d", rc);
            sqlite3_finalize(stmt);
            owl_emit_native_error(E_NATIVE_SQL_BACKEND_ERROR, "SQLEnvoie",
                                  "native", "SQLEnvoie",
                                  "detail", detail_buf,
                                  "code", code_buf,
                                  NULL);
            ObjInstance* rep = build_runtime_reponse(false, lignes, occurrence, 0);
            vm_pop(); /* lignes */
            return OBJ_VAL(rep);
        }

        /* Construit un assoc array col_name -> value. */
        ObjAssocArray* row = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_VARIANT);
        vm_push(OBJ_VAL(row));
        for (int c = 0; c < colCount; c++) {
            const char* colName = sqlite3_column_name(stmt, c);
            Value cellVal = column_to_value(stmt, c);
            obj_assoc_put_cstr(row, colName, (int)strlen(colName), cellVal);
        }
        obj_array_push(lignes, OBJ_VAL(row));
        vm_pop(); /* row */
        occurrence++;
    }

    int affected = (sqlite3_total_changes64(db) != changesAvant)
                       ? sqlite3_changes(db) : 0;
    sqlite3_finalize(stmt);

    ObjInstance* rep = build_runtime_reponse(true, lignes, occurrence, affected);
    vm_pop(); /* lignes */
    owl_clear_error();
    return OBJ_VAL(rep);
}

/* ============================================================
 * methodBindings — .Ouvre() / .Ferme() sur sqlConnexion.
 * ============================================================ */

static const OwlMethodBinding MB_OUVRE[] = {
    { "sqlConnexion", "Ouvre", 1 },
    { "sqlConnexion", "Open",  1 },
    { NULL, NULL, 0 }
};

static const OwlMethodBinding MB_FERME[] = {
    { "sqlConnexion", "Ferme", 1 },
    { "sqlConnexion", "Close", 1 },
    { NULL, NULL, 0 }
};

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_cnx[]    = { {"cnx", "sqlConnexion", "connexion"}, {0} };
static const OwlParam p_envoi[]  = {
    {"cnx", "sqlConnexion", "connexion ouverte"},
    {"req", "sqlRequ\xc3\xaa" "te", "requete a executer"},
    {0}
};

static const OwlSignature sig_cnx[]    = { {1, p_cnx,   "bool\xc3\xa9" "en", "succes"}, {0} };
static const OwlSignature sig_envoie[] = { {2, p_envoi, "sqlR\xc3\xa9" "ponse", "reponse structuree"}, {0} };

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define S_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "SQL", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_SQL[] = {
    S_DEF("SQLConnexionOuvre", NULL, "SQLConnectionOpen",
        "Ouvre une connexion vers la base decrite par cnx.",
        "Open a connection to the database described by cnx.",
        sig_cnx, fn_sql_connexion_ouvre, MB_OUVRE),
    S_DEF("SQLConnexionFerme", NULL, "SQLConnectionClose",
        "Ferme une connexion ouverte.",
        "Close an open connection.",
        sig_cnx, fn_sql_connexion_ferme, MB_FERME),
    S_DEF("SQLEnvoie", NULL, "SQLSend",
        "Envoie une requete vers la connexion et retourne la reponse.",
        "Send a request to the connection and return the response.",
        sig_envoie, fn_sql_envoie, NULL),
};

#undef S_DEF

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_sql_init(void) {
    owl_registry_register_module(DEFS_SQL,
        (int)(sizeof(DEFS_SQL) / sizeof(DEFS_SQL[0])));
    owl_registry_register_enums(SQL_ENUMS,
        (int)(sizeof(SQL_ENUMS) / sizeof(SQL_ENUMS[0])));
    owl_registry_register_consts(SQL_CONSTS,
        (int)(sizeof(SQL_CONSTS) / sizeof(SQL_CONSTS[0])));
}
