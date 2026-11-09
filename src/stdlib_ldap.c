// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL stdlib — surface langage du client LDAP (lecture seule, RFC 4511).
 *
 * Handle type `ldapSession` : `sess est un ldapSession` demarre deja aux
 * defauts (port 389, version 3, verification de certificat active, reste
 * vide). Pas de remise a zero a appeler : une instance neuve EST l'etat par
 * defaut. L'etat de connexion (OwlLdapClient*) vit SUR l'instance, dans le
 * field interne `_handle` (pointeur opaque, cast intptr_t -> INTEGER_VAL), a
 * la facon de sqlConnexion. Reconnecter ferme le client precedent.
 *
 * Deux surfaces equivalentes (invariant doubles type-vs-fonction) :
 *   - methodes sur l'instance : sess.Connecte(), sess.Recherche(base, filtre),
 *     sess.Valeur(dn, attr[, i]), sess.NbValeur(dn, attr), sess.Deconnecte() ;
 *   - natives globales prenant le handle en premier argument :
 *     LDAPConnecte(sess), LDAPRecherche(sess, base, filtre),
 *     LDAPValeur(sess, dn, attr[, i]), LDAPNbValeur(sess, dn, attr),
 *     LDAPDeconnecte(sess).
 * Le binding de methode relaie le MEME NativeFn que la native globale (le
 * receveur est l'argument 0 dans les deux formes), donc une seule
 * interception de mock (MockCrée(LDAPConnecte)) couvre les deux.
 *
 * Config = proprietes `..` : Hote, Port, Utilisateur, MotDePasse, Version,
 * Option (optionSSL = transport LDAPS), confiance TLS (VerifieCertificat,
 * CertificatCA) et Authentification (enum ldapAuth, mecanisme du bind,
 * separe du transport).
 *
 * Transport sur le port configure en clair, ou LDAPS (TLS sur 636) quand
 * Option porte optionSSL.
 *
 * Erreurs : contrat canon soft-error. Un echec de connexion pose
 * ErreurDetectee via owl_emit_native_error() et retourne Faux.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
/* net_tcp.h (tire par ldap_client.h) inclut winsock2.h sous Windows, qui
 * entre en collision avec les TokenType de scanner.h via owl_errors.h. Meme
 * garde que stdlib_ftp.c / stdlib_sql.c. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "object.h"
#include "vm.h"
#include "sentinels.h"
#include "ldap_client.h"
#include "ldap_negotiate.h"
#include "ldap_pdu.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Valeurs de la propriete Option (masque combinable en OU-binaire). */
#define LDAP_OPTION_DEFAUT  0
#define LDAP_OPTION_SSL     1

#define LDAP_PORT_DEFAUT    389
#define LDAP_PORT_LDAPS     636
#define LDAP_VERSION_DEFAUT 3

/* ============================================================
 * Enum ldapAuth — mecanisme d'authentification du bind.
 *
 * Separe du transport (Option/optionSSL, VerifieCertificat, CertificatCA).
 * Valeurs en zone sentinelle (offset 113 libre). Seul authSimple est vivant ;
 * les autres sont declares mais refuses a l'usage (erreur explicite) tant que
 * non implementes.
 * ============================================================ */

#define OWL_LDAP_AUTH_BASE      OWL_SENTINEL_OFFSET(113)
#define OWL_LDAP_AUTH_SIMPLE    (OWL_LDAP_AUTH_BASE + 0)
#define OWL_LDAP_AUTH_NEGOTIATE (OWL_LDAP_AUTH_BASE + 1)
#define OWL_LDAP_AUTH_EXTERNE   (OWL_LDAP_AUTH_BASE + 2)
#define OWL_LDAP_AUTH_ANONYME   (OWL_LDAP_AUTH_BASE + 3)

static const char* const LDAP_AUTH_VALUES[] = {
    "authSimple", "authNegotiate", "authExterne", "authAnonyme", NULL
};

static const OwlEnum E_ldapAuth = {
    .name = "ldapAuth", .nameEN = "ldapAuth", .category = "LDAP",
    .descFR = "Mecanisme d'authentification du bind LDAP (propriete Authentification), independant du transport.",
    .descEN = "Authentication mechanism of the LDAP bind (Authentication property), independent from transport.",
    .values = LDAP_AUTH_VALUES,
    .combinable = false,
};

static const OwlEnum* const LDAP_ENUMS[] = { &E_ldapAuth };

/* ============================================================
 * Type ldapSession + template runtime.
 * ============================================================ */

static const OwlField FIELDS_ldapSession[] = {
    {"Hote", "H\xc3\xb4te", "cha\xc3\xae""ne", "\"\"",
     "nom d'hote ou adresse IP de l'annuaire", "directory host name or IP address",
     .nameEN = "Host"},
    {"Port", NULL, "entier", "389",
     "port TCP de l'annuaire (389 en clair, 636 pour LDAPS)", "directory TCP port (389 clear, 636 for LDAPS)",
     .nameEN = "Port"},
    {"Utilisateur", NULL, "cha\xc3\xae""ne", "\"\"",
     "DN ou UPN du compte utilise pour le bind", "DN or UPN of the account used for the bind",
     .nameEN = "User"},
    {"MotDePasse", NULL, "cha\xc3\xae""ne", "\"\"",
     "mot de passe du compte utilise pour le bind", "password of the account used for the bind",
     .nameEN = "Password"},
    {"Version", NULL, "entier", "3",
     "version du protocole LDAP (seule la v3 est prise en charge)", "LDAP protocol version (only v3 is supported)",
     .nameEN = "Version"},
    {"Option", NULL, "entier", "ldapOptionD\xc3\xa9""faut",
     "mode de transport : en clair par defaut, optionSSL pour LDAPS (TLS)", "transport mode: clear by default, optionSSL for LDAPS (TLS)",
     .nameEN = "Option"},
    {"VerifieCertificat", "V\xc3\xa9rifieCertificat", "bool\xc3\xa9""en", "Vrai",
     "en LDAPS, controle la validite du certificat serveur ; Faux relache la verification (chiffre mais non controle)",
     "in LDAPS, checks the server certificate validity; False relaxes verification (encrypted but unchecked)",
     .nameEN = "VerifyCertificate"},
    {"CertificatCA", NULL, "cha\xc3\xae""ne", "\"\"",
     "chemin d'un fichier PEM contenant une autorite de certification personnalisee (CA interne / auto-signee). Vide = bundle par defaut",
     "path to a PEM file holding a custom certificate authority (internal / self-signed CA). Empty = default bundle",
     .nameEN = "CACertificate"},
    {"CertificatClient", NULL, "cha\xc3\xae""ne", "\"\"",
     "chemin d'un fichier PEM contenant le certificat client presente au handshake (mTLS, authentification externe). Vide = aucun certificat client",
     "path to a PEM file holding the client certificate presented at the handshake (mTLS, external authentication). Empty = no client certificate",
     .nameEN = "ClientCertificate"},
    {"CleClient", "Cl\xc3\xa9""Client", "cha\xc3\xae""ne", "\"\"",
     "chemin d'un fichier contenant la cle privee du certificat client (mTLS). Vide = aucune cle client",
     "path to a file holding the client certificate private key (mTLS). Empty = no client key",
     .nameEN = "ClientKey"},
    {"Authentification", NULL, "entier", "authSimple",
     "mecanisme d'authentification du bind (independant du transport)", "authentication mechanism of the bind (independent from transport)",
     .allowedBase = &E_ldapAuth, .nameEN = "Authentication"},
    {0},
};

static const char* const ALIAS_ldapSession[] = { "ldapSession", NULL };

static const OwlTypeDef LDAP_TYPES[] = {
    { .canonical      = "ldapSession",
      .canonicalAscii = "ldapSession",
      .aliases        = ALIAS_ldapSession,
      .category       = "LDAP",
      .descFR         = "Session vers un annuaire LDAP : configuration (hote, port, identifiants, transport, confiance TLS, mecanisme d'authentification) et etat de connexion portes par l'instance. Une instance neuve demarre aux valeurs par defaut.",
      .descEN         = "Session to an LDAP directory: configuration (host, port, credentials, transport, TLS trust, authentication mechanism) and connection state carried by the instance. A fresh instance starts at default values.",
      .fields         = FIELDS_ldapSession,
      .nameEN         = "ldapSession",
      .genre          = OWL_GENRE_F },   /* la session */
};

/* Construit le template runtime de ldapSession (defauts + _handle interne). */
static ObjInstance* build_ldapSession_template(void) {
    ObjString* name = obj_copy_string("ldapSession", 11);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    inst_field_set_str (tpl, "Hote",              "");
    inst_field_set_int (tpl, "Port",              LDAP_PORT_DEFAUT);
    inst_field_set_str (tpl, "Utilisateur",       "");
    inst_field_set_str (tpl, "MotDePasse",        "");
    inst_field_set_int (tpl, "Version",           LDAP_VERSION_DEFAUT);
    inst_field_set_int (tpl, "Option",            LDAP_OPTION_DEFAUT);
    inst_field_set_bool(tpl, "VerifieCertificat", true);
    inst_field_set_str (tpl, "CertificatCA",      "");
    inst_field_set_str (tpl, "CertificatClient",  "");
    inst_field_set_str (tpl, "CleClient",         "");
    inst_field_set_int (tpl, "Authentification",  OWL_LDAP_AUTH_SIMPLE);

    /* _handle : pointeur opaque vers OwlLdapClient* (cast intptr_t ->
     * INTEGER_VAL). Prefixe underscore = field interne, hors schema public. */
    inst_field_set_int (tpl, "_handle",           0);

    vm_pop();  /* tpl */
    vm_pop();  /* name */
    return tpl;
}

/* ============================================================
 * Helpers — resolution d'instance, lecture de champs, handle.
 * ============================================================ */

/* Verifie que args[0] est un ObjInstance de structName "ldapSession".
 * Retourne l'instance, ou NULL + ErreurDetectee si mauvais type. */
static ObjInstance* resolve_ldap_session(int argCount, Value* args, const char* fn) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn,
                              "arg", "1",
                              "expected", "ldapSession",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(args[0]);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "ldapSession") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn,
                              "arg", "1",
                              "expected", "ldapSession",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static const char* ldap_session_str(ObjInstance* inst, const char* field) {
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    Value v;
    if (table_get(&inst->fields, k, &v) && IS_STRING(v)) return AS_STRING(v)->chars;
    return "";
}

static int ldap_session_int(ObjInstance* inst, const char* field, int fallback) {
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    Value v;
    if (table_get(&inst->fields, k, &v) && IS_INTEGER(v)) return (int)AS_INTEGER(v);
    return fallback;
}

static bool ldap_session_bool(ObjInstance* inst, const char* field, bool fallback) {
    ObjString* k = obj_copy_string(field, (int)strlen(field));
    Value v;
    if (table_get(&inst->fields, k, &v) && IS_BOOL(v)) return AS_BOOL(v);
    return fallback;
}

static OwlLdapClient* ldap_get_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v) || !IS_INTEGER(v)) return NULL;
    return (OwlLdapClient*)(intptr_t)AS_INTEGER(v);
}

static void ldap_set_handle(ObjInstance* inst, OwlLdapClient* cli) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)cli);
}

/* Ferme + libere le client porte par l'instance (idempotent). */
static void ldap_close_handle(ObjInstance* inst) {
    OwlLdapClient* cli = ldap_get_handle(inst);
    if (cli != NULL) {
        owl_ldap_client_close(cli);
        free(cli);
        ldap_set_handle(inst, NULL);
    }
}

/* Bind SASL GSS-SPNEGO (authNegotiate) sur un client neuf deja initialise.
 * Ouvre le transport LDAPS (sans bind), derive le channel binding
 * tls-server-end-point du certificat serveur, cree le backend Negotiate
 * (SSPI Windows / GSSAPI POSIX) et deroule l'echange de jetons.
 *
 * Renvoie true quand l'echange a abouti a une reponse serveur definitive
 * (out_result_code porte le resultCode final, 0 = succes). Renvoie false sur
 * echec transport / fournisseur de securite / PDU ; cli->last_error est
 * alors renseigne pour l'emission d'erreur en amont. */
static bool ldap_bind_negotiate(OwlLdapClient* cli, const char* host, int port,
                                int verify_level, const char* extra_ca,
                                int version, int* out_result_code) {
    *out_result_code = 0;

    if (!owl_ldap_client_open(cli, host, port, true, verify_level, extra_ca,
                              NULL, NULL, 0)) {
        return false;  /* owl_ldap_client_open a deja rempli last_error */
    }

    /* Nom de service Kerberos de l'annuaire. */
    char spn[300];
    snprintf(spn, sizeof(spn), "ldap/%s", host);

    char be_err[256];
    be_err[0] = '\0';
    OwlLdapNegotiate* neg = owl_ldap_negotiate_create(spn, be_err, sizeof(be_err));
    if (neg == NULL) {
        snprintf(cli->last_error, sizeof(cli->last_error), "%s",
                 be_err[0] ? be_err : "fournisseur de sécurité Negotiate indisponible");
        return false;
    }

    /* Channel binding tls-server-end-point (RFC 5929) : hash du certificat
     * serveur. Omis si le serveur n'a presente aucun cert (DC non durci). */
    unsigned char cb[128];
    size_t cb_len = 0;
    const mbedtls_x509_crt* peer = owl_tls_peer_cert(&cli->tls);
    owl_ldap_channel_binding_endpoint(peer, cb, sizeof(cb), &cb_len);

    OwlLdapTokenProvider prov = owl_ldap_negotiate_provider(neg);
    bool ok = owl_ldap_client_sasl_bind(cli, "GSS-SPNEGO", &prov,
                                        cb_len ? cb : NULL, cb_len,
                                        version, out_result_code);
    if (!ok) {
        const char* be = owl_ldap_negotiate_last_error(neg);
        if (be && be[0]) {
            snprintf(cli->last_error, sizeof(cli->last_error), "%s", be);
        }
        /* sinon last_error vient deja de owl_ldap_client_sasl_bind */
    }

    owl_ldap_negotiate_destroy(neg);
    return ok;
}

/* ============================================================
 * Natives.
 * ============================================================ */

/* LDAPConnecte(sess) -> booleen.
 * Lit la configuration posee sur l'instance, ouvre la connexion (LDAPS:636 si
 * Option porte optionSSL, sinon clair sur Port) et effectue le bind selon le
 * mecanisme Authentification. Le client est memorise sur l'instance (_handle).
 * Vrai si connexion + bind acceptes, Faux + ErreurDetectee sur echec. */
static Value fn_ldap_connecte(int argCount, Value* args) {
    ObjInstance* sess = resolve_ldap_session(argCount, args, "LDAPConnecte");
    if (sess == NULL) return BOOL_VAL(false);

    const char* host = ldap_session_str(sess, "Hote");
    int  port    = ldap_session_int(sess, "Port", LDAP_PORT_DEFAUT);
    int  option  = ldap_session_int(sess, "Option", LDAP_OPTION_DEFAUT);
    int  version = ldap_session_int(sess, "Version", LDAP_VERSION_DEFAUT);
    int  auth    = ldap_session_int(sess, "Authentification", OWL_LDAP_AUTH_SIMPLE);
    bool use_tls = (option & LDAP_OPTION_SSL) != 0;

    /* Seule la v3 est prise en charge (la v2 est depreciee, RFC 3494). Toute
     * autre valeur est refusee en amont, sans aucune tentative reseau. */
    if (version != LDAP_VERSION_DEFAUT) {
        char detail[80];
        snprintf(detail, sizeof(detail),
                 "LDAP v3 requis, version %d non supportée", version);
        owl_emit_native_error(E_NATIVE_LDAP_CONNECT_FAILURE, "LDAPConnecte",
                              "native", "LDAPConnecte",
                              "cible", host,
                              "detail", detail, NULL);
        return BOOL_VAL(false);
    }

    /* Negotiate (SASL GSS-SPNEGO, SSPI/GSSAPI) : le channel binding
     * tls-server-end-point derive du certificat serveur, donc le transport
     * chiffre (optionSSL) est requis. Refus explicite sinon, aucune tentative
     * reseau — comme authExterne exige mTLS. */
    bool negotiate = (auth == OWL_LDAP_AUTH_NEGOTIATE);
    if (negotiate && !use_tls) {
        owl_emit_native_error(E_NATIVE_LDAP_CONNECT_FAILURE, "LDAPConnecte",
                              "native", "LDAPConnecte",
                              "cible", host,
                              "detail",
                              "authentification négociée : le transport sécurisé (optionSSL) est requis",
                              NULL);
        return BOOL_VAL(false);
    }

    /* Authentification externe (SASL EXTERNAL) : l'identite vient du certificat
     * client presente au handshake TLS. Elle exige donc le transport chiffre
     * (optionSSL) ET un certificat client + sa cle. Refus explicite si l'une de
     * ces conditions manque, aucune tentative reseau. */
    bool externe = (auth == OWL_LDAP_AUTH_EXTERNE);
    const char* client_cert = ldap_session_str(sess, "CertificatClient");
    const char* client_key  = ldap_session_str(sess, "CleClient");
    if (externe) {
        const char* manque = NULL;
        if (!use_tls)                  manque = "le transport sécurisé (optionSSL) est requis";
        else if (client_cert[0] == '\0') manque = "un certificat client (CertificatClient) est requis";
        else if (client_key[0] == '\0')  manque = "une clé client (CléClient) est requise";
        if (manque != NULL) {
            char detail[160];
            snprintf(detail, sizeof(detail),
                     "authentification externe : %s", manque);
            owl_emit_native_error(E_NATIVE_LDAP_CONNECT_FAILURE, "LDAPConnecte",
                                  "native", "LDAPConnecte",
                                  "cible", host,
                                  "detail", detail, NULL);
            return BOOL_VAL(false);
        }
    }

    /* Credentials : vides pour un bind anonyme ou externe, sinon les champs
     * poses (le bind externe ne porte aucun credential). */
    bool anonymous = (auth == OWL_LDAP_AUTH_ANONYME);
    bool sans_cred = anonymous || externe;
    const char* user = sans_cred ? "" : ldap_session_str(sess, "Utilisateur");
    const char* pass = sans_cred ? "" : ldap_session_str(sess, "MotDePasse");

    /* Confiance TLS du LDAPS. VerifieCertificat=Faux releche la verification
     * (TLS chiffre, cert non controle) ; sinon verification pleine, contre la
     * CA personnalisee CertificatCA si fournie, sinon le bundle par defaut. */
    bool verifie_cert = ldap_session_bool(sess, "VerifieCertificat", true);
    const char* ca_path = ldap_session_str(sess, "CertificatCA");
    int verify_level = verifie_cert ? OWL_TLS_VERIFY_FULL : OWL_TLS_VERIFY_NONE;
    const char* extra_ca = (verifie_cert && ca_path[0] != '\0') ? ca_path : NULL;

    /* En SSL le port canon est 636 ; un port explicite non-defaut est honore. */
    int eff_port = port;
    if (use_tls && port == LDAP_PORT_DEFAUT) eff_port = LDAP_PORT_LDAPS;

    /* Reconnexion : ferme le client precedent porte par l'instance. */
    ldap_close_handle(sess);

    OwlLdapClient* cli = (OwlLdapClient*)malloc(sizeof(OwlLdapClient));
    if (cli == NULL) {
        owl_emit_native_error(E_NATIVE_LDAP_CONNECT_FAILURE, "LDAPConnecte",
                              "native", "LDAPConnecte",
                              "cible", host,
                              "detail", "mémoire insuffisante", NULL);
        return BOOL_VAL(false);
    }
    owl_ldap_client_init(cli);

    /* Cert client (mTLS) : uniquement pour l'authentification externe. */
    const char* cc_path = externe ? client_cert : NULL;
    const char* ck_path = externe ? client_key  : NULL;

    int result_code = 0;
    bool ok;
    if (negotiate) {
        /* GSS-SPNEGO : transport LDAPS ouvert sans bind, puis echange de
         * jetons adosse au fournisseur de securite de l'OS. */
        ok = ldap_bind_negotiate(cli, host, eff_port, verify_level, extra_ca,
                                 version, &result_code);
    } else {
        ok = owl_ldap_client_connect(
            cli, host, eff_port, use_tls, verify_level, extra_ca,
            cc_path, ck_path,
            user, (const unsigned char*)pass, strlen(pass), externe,
            version, 0, &result_code);
    }

    if (!ok || result_code != OWL_LDAP_SUCCESS) {
        char cible[300];
        snprintf(cible, sizeof(cible), "%s:%d", host, eff_port);
        /* Bind refuse (ok transport, result_code != 0) : le serveur a souvent
         * renseigne un diagnosticMessage (sous-code AD), capte dans last_error
         * — plus parlant que le generique "identifiants refusés". */
        const char* srv_diag = owl_ldap_client_last_error(cli);
        const char* detail = ok
            ? (srv_diag && srv_diag[0] ? srv_diag : "identifiants refusés")
            : srv_diag;
        owl_emit_native_error(E_NATIVE_LDAP_CONNECT_FAILURE, "LDAPConnecte",
                              "native", "LDAPConnecte",
                              "cible", cible,
                              "detail", detail && detail[0] ? detail : "échec",
                              NULL);
        owl_ldap_client_close(cli);
        free(cli);
        return BOOL_VAL(false);
    }

    ldap_set_handle(sess, cli);
    return BOOL_VAL(true);
}

/* LDAPDeconnecte(sess) -> booleen : unbind + fermeture. Vrai si une session
 * etait ouverte sur l'instance. */
static Value fn_ldap_deconnecte(int argCount, Value* args) {
    ObjInstance* sess = resolve_ldap_session(argCount, args, "LDAPDeconnecte");
    if (sess == NULL) return BOOL_VAL(false);
    if (ldap_get_handle(sess) == NULL) return BOOL_VAL(false);
    ldap_close_handle(sess);
    return BOOL_VAL(true);
}

/* LDAPRecherche(sess, dnBase, filtre) -> chaine : DN trouves joints par CRLF,
 * "" si rien / erreur. Scope subtree (recursif). */
static Value fn_ldap_recherche(int argCount, Value* args) {
    ObjInstance* sess = resolve_ldap_session(argCount, args, "LDAPRecherche");
    if (sess == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* base_dn = (argCount > 1 && IS_STRING(args[1])) ? AS_CSTRING(args[1]) : "";
    const char* filtre  = (argCount > 2 && IS_STRING(args[2])) ? AS_CSTRING(args[2]) : "";

    OwlLdapClient* cli = ldap_get_handle(sess);
    if (cli == NULL) return OBJ_VAL(obj_copy_string("", 0));

    const OwlLdapSearchResult* r = owl_ldap_client_search(
        cli, base_dn, OWL_LDAP_SCOPE_WHOLE_SUBTREE, filtre, NULL, 0);
    if (r == NULL || r->entry_count == 0) {
        return OBJ_VAL(obj_copy_string("", 0));
    }

    /* Concatene les DN separes par CRLF dans un buffer heap temporaire. */
    size_t total = 0;
    for (size_t i = 0; i < r->entry_count; i++) {
        total += r->entries[i].dn_len + 2;  /* + CRLF */
    }
    char* buf = (char*)malloc(total + 1);
    if (buf == NULL) return OBJ_VAL(obj_copy_string("", 0));
    size_t off = 0;
    for (size_t i = 0; i < r->entry_count; i++) {
        if (i > 0) { buf[off++] = '\r'; buf[off++] = '\n'; }
        memcpy(buf + off, r->entries[i].dn, r->entries[i].dn_len);
        off += r->entries[i].dn_len;
    }
    buf[off] = '\0';
    Value out = OBJ_VAL(obj_copy_string(buf, (int)off));
    free(buf);
    return out;
}

/* Retrouve l'attribut `attr` (casse-insensible) de la 1ere entree d'un
 * read_object. NULL si absent. */
static const OwlLdapAttribute* ldap_find_attribute(const OwlLdapSearchResult* r,
                                                   const char* attr) {
    if (r == NULL || r->entry_count == 0) return NULL;
    const OwlLdapSearchEntry* e = &r->entries[0];
    size_t attr_len = strlen(attr);
    for (size_t i = 0; i < e->attribute_count; i++) {
        const OwlLdapAttribute* a = &e->attributes[i];
        if (a->type_len == attr_len &&
            strncasecmp((const char*)a->type, attr, attr_len) == 0) {
            return a;
        }
    }
    return NULL;
}

/* LDAPValeur(sess, dn, attribut [, indice]) -> valeur (chaine).
 * Lecture scope-base sur dn ; renvoie la indice-ieme valeur (1-based, defaut
 * 1), "" si absente. */
static Value fn_ldap_valeur(int argCount, Value* args) {
    ObjInstance* sess = resolve_ldap_session(argCount, args, "LDAPValeur");
    if (sess == NULL) return OBJ_VAL(obj_copy_string("", 0));
    const char* dn       = (argCount > 1 && IS_STRING(args[1])) ? AS_CSTRING(args[1]) : "";
    const char* attribut = (argCount > 2 && IS_STRING(args[2])) ? AS_CSTRING(args[2]) : "";
    int indice = (argCount > 3 && IS_INTEGER(args[3])) ? (int)AS_INTEGER(args[3]) : 1;
    if (indice < 1) indice = 1;

    OwlLdapClient* cli = ldap_get_handle(sess);
    if (cli == NULL) return OBJ_VAL(obj_copy_string("", 0));

    const char* attrs[1] = { attribut };
    const OwlLdapSearchResult* r =
        owl_ldap_client_read_object(cli, dn, attrs, 1);
    const OwlLdapAttribute* a = ldap_find_attribute(r, attribut);
    if (a == NULL || (size_t)indice > a->value_count) {
        return OBJ_VAL(obj_copy_string("", 0));
    }
    const OwlLdapValue* val = &a->values[indice - 1];
    return OBJ_VAL(obj_copy_string((const char*)val->value, (int)val->len));
}

/* LDAPNbValeur(sess, dn, attribut) -> entier : nombre de valeurs. */
static Value fn_ldap_nbvaleur(int argCount, Value* args) {
    ObjInstance* sess = resolve_ldap_session(argCount, args, "LDAPNbValeur");
    if (sess == NULL) return INTEGER_VAL(0);
    const char* dn       = (argCount > 1 && IS_STRING(args[1])) ? AS_CSTRING(args[1]) : "";
    const char* attribut = (argCount > 2 && IS_STRING(args[2])) ? AS_CSTRING(args[2]) : "";

    OwlLdapClient* cli = ldap_get_handle(sess);
    if (cli == NULL) return INTEGER_VAL(0);

    const char* attrs[1] = { attribut };
    const OwlLdapSearchResult* r =
        owl_ldap_client_read_object(cli, dn, attrs, 1);
    const OwlLdapAttribute* a = ldap_find_attribute(r, attribut);
    return INTEGER_VAL(a == NULL ? 0 : (int64_t)a->value_count);
}

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_sess[] = {
    {"sess", "ldapSession", "session LDAP"}, {0}
};
static const OwlSignature sig_connecte[] = {
    {1, p_sess, "bool\xc3\xa9""en", "vrai si la connexion et le bind ont abouti"}, {0}
};
static const OwlSignature sig_deconnecte[] = {
    {1, p_sess, "bool\xc3\xa9""en", "vrai si la session était ouverte et a été fermée"}, {0}
};

static const OwlParam p_recherche[] = {
    {"sess",   "ldapSession", "session LDAP ouverte"},
    {"dnBase", "cha\xc3\xae""ne", "DN de base de la recherche"},
    {"filtre", "cha\xc3\xae""ne", "filtre LDAP (RFC 4515), ex \"(cn=a*)\""}, {0}
};
static const OwlSignature sig_recherche[] = {
    {3, p_recherche, "cha\xc3\xae""ne", "DN trouvés séparés par RC, \"\" si aucun / erreur"}, {0}
};

static const OwlParam p_valeur[] = {
    {"sess",     "ldapSession", "session LDAP ouverte"},
    {"dn",       "cha\xc3\xae""ne", "DN de l'objet à lire"},
    {"attribut", "cha\xc3\xae""ne", "nom de l'attribut"},
    {"indice",   "entier", "rang 1-based de la valeur (multi-valués ; défaut 1)"}, {0}
};
static const OwlSignature sig_valeur[] = {
    {3, p_valeur, "cha\xc3\xae""ne", "première valeur de l'attribut, \"\" si absent"},
    {4, p_valeur, "cha\xc3\xae""ne", "indice-ième valeur de l'attribut, \"\" si absent"},
    {0}
};

static const OwlParam p_nbvaleur[] = {
    {"sess",     "ldapSession", "session LDAP ouverte"},
    {"dn",       "cha\xc3\xae""ne", "DN de l'objet à lire"},
    {"attribut", "cha\xc3\xae""ne", "nom de l'attribut"}, {0}
};
static const OwlSignature sig_nbvaleur[] = {
    {3, p_nbvaleur, "entier", "nombre de valeurs de l'attribut (0 si absent)"}, {0}
};

/* ============================================================
 * methodBindings — re-exposition des natives LDAPxxx en methodes sur le
 * receveur ldapSession. Meme impl que la native globale (le receveur est
 * l'argument 0), donc une seule interception de mock couvre les deux formes.
 * ============================================================ */

static const OwlMethodBinding MB_CONNECTE[]   = { {"ldapSession","Connecte",1},  {"ldapSession","Connect",1},     {NULL,NULL,0} };
static const OwlMethodBinding MB_DECONNECTE[] = { {"ldapSession","D\xc3\xa9""connecte",1}, {"ldapSession","Disconnect",1}, {NULL,NULL,0} };
static const OwlMethodBinding MB_RECHERCHE[]  = { {"ldapSession","Recherche",3}, {"ldapSession","Find",3},        {NULL,NULL,0} };
static const OwlMethodBinding MB_VALEUR[]     = { {"ldapSession","Valeur",-1},   {"ldapSession","Value",-1},      {NULL,NULL,0} };
static const OwlMethodBinding MB_NBVALEUR[]   = { {"ldapSession","NbValeur",3},  {"ldapSession","NbValue",3},     {NULL,NULL,0} };

/* ============================================================
 * Definitions natives + constantes.
 * ============================================================ */

#define L_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "LDAP", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_LDAP[] = {
    L_DEF("LDAPConnecte", NULL, "LDAPConnect",
        "Ouvre une session vers un annuaire LDAP à partir de la configuration posée sur le handle ldapSession (hôte, port, identifiants, transport, authentification) et effectue le bind. Retourne un booléen de succès. Forme méthode : sess.Connecte().",
        "Opens a session to an LDAP directory using the configuration set on the ldapSession handle (host, port, credentials, transport, authentication) and performs the bind. Returns a success boolean. Method form: sess.Connect().",
        sig_connecte, fn_ldap_connecte, MB_CONNECTE),
    L_DEF("LDAPD\xc3\xa9""connecte", "LDAPDeconnecte", "LDAPDisconnect",
        "Ferme une session LDAP ouverte (unbind puis fermeture du transport). Forme méthode : sess.Déconnecte().",
        "Closes an open LDAP session (unbind then transport shutdown). Method form: sess.Disconnect().",
        sig_deconnecte, fn_ldap_deconnecte, MB_DECONNECTE),
    L_DEF("LDAPRecherche", NULL, "LDAPFind",
        "Recherche récursive (sous-arbre) dans l'annuaire à partir d'un DN de base et d'un filtre LDAP. Retourne les DN trouvés séparés par un retour chariot, ou une chaîne vide si aucun résultat. Forme méthode : sess.Recherche(dnBase, filtre).",
        "Recursive (subtree) search in the directory from a base DN and an LDAP filter. Returns the matching DNs separated by a carriage return, or an empty string when there is no result. Method form: sess.Find(baseDN, filter).",
        sig_recherche, fn_ldap_recherche, MB_RECHERCHE),
    L_DEF("LDAPValeur", NULL, "LDAPValue",
        "Lit la valeur d'un attribut d'un objet de l'annuaire désigné par son DN. L'indice (1-based, optionnel) cible une valeur précise d'un attribut multi-valué ; omis, la première valeur est retournée. Chaîne vide si l'attribut est absent. Forme méthode : sess.Valeur(dn, attribut [, indice]).",
        "Reads an attribute value of a directory object designated by its DN. The (optional, 1-based) index targets a specific value of a multi-valued attribute; when omitted, the first value is returned. Empty string when the attribute is absent. Method form: sess.Value(dn, attribute [, index]).",
        sig_valeur, fn_ldap_valeur, MB_VALEUR),
    L_DEF("LDAPNbValeur", NULL, "LDAPNbValue",
        "Retourne le nombre de valeurs d'un attribut d'un objet de l'annuaire désigné par son DN (0 si l'attribut est absent). Forme méthode : sess.NbValeur(dn, attribut).",
        "Returns the number of values of an attribute of a directory object designated by its DN (0 when the attribute is absent). Method form: sess.NbValue(dn, attribute).",
        sig_nbvaleur, fn_ldap_nbvaleur, MB_NBVALEUR),
};

#undef L_DEF

/* optionSSL (= 1) est une constante multi-protocole enregistree UNE fois par
 * le socle MQ (categorie MQTT) et reutilisee par AMQP/STOMP/NATS/email : on la
 * reutilise telle quelle (LDAP_OPTION_SSL == OWL_OPTION_SSL), on ne la
 * re-declare pas (un doublon de nom de constante casse le self-test registry).
 * Seul le defaut propre a LDAP et les mecanismes d'auth sont enregistres ici. */
static const OwlConstDef LDAP_CONSTS[] = {
    { "ldapOptionD\xc3\xa9""faut", "LDAP", INTEGER_VAL(LDAP_OPTION_DEFAUT),
      "Mode de connexion LDAP par défaut : transport en clair sur le port configuré.",
      "Default LDAP connection mode: clear transport on the configured port.",
      "ldapOptionDefault" },
    { "authSimple", "LDAP", INTEGER_VAL(OWL_LDAP_AUTH_SIMPLE),
      "Bind simple : authentification par DN (ou UPN) et mot de passe. Mécanisme par défaut.",
      "Simple bind: authentication by DN (or UPN) and password. Default mechanism.",
      "authSimple" },
    { "authNegotiate", "LDAP", INTEGER_VAL(OWL_LDAP_AUTH_NEGOTIATE),
      "Authentification négociée (SASL GSS-SPNEGO) adossée au fournisseur de sécurité de l'OS (SSPI sous Windows, GSSAPI sous POSIX) : les identifiants sont ceux de la session courante. Requiert optionSSL (channel binding dérivé du certificat serveur).",
      "Negotiated authentication (SASL GSS-SPNEGO) backed by the OS security provider (SSPI on Windows, GSSAPI on POSIX): credentials are those of the current session. Requires optionSSL (channel binding derived from the server certificate).",
      "authNegotiate" },
    { "authExterne", "LDAP", INTEGER_VAL(OWL_LDAP_AUTH_EXTERNE),
      "Authentification externe (SASL EXTERNAL / mTLS) : l'identité est portée par le certificat client présenté au handshake. Requiert optionSSL, CertificatClient et CléClient.",
      "External authentication (SASL EXTERNAL / mTLS): the identity is carried by the client certificate presented at the handshake. Requires optionSSL, ClientCertificate and ClientKey.",
      "authExternal" },
    { "authAnonyme", "LDAP", INTEGER_VAL(OWL_LDAP_AUTH_ANONYME),
      "Bind anonyme : aucune identité présentée (ni DN ni mot de passe).",
      "Anonymous bind: no identity presented (no DN, no password).",
      "authAnonymous" },
};

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_ldap_init(void) {
    owl_registry_register_module(DEFS_LDAP,
        (int)(sizeof(DEFS_LDAP) / sizeof(DEFS_LDAP[0])));
    owl_registry_register_enums(LDAP_ENUMS,
        (int)(sizeof(LDAP_ENUMS) / sizeof(LDAP_ENUMS[0])));
    owl_registry_register_consts(LDAP_CONSTS,
        (int)(sizeof(LDAP_CONSTS) / sizeof(LDAP_CONSTS[0])));
    owl_registry_register_types(LDAP_TYPES,
        (int)(sizeof(LDAP_TYPES) / sizeof(LDAP_TYPES[0])));

    /* Template runtime pour la declaration `sess est un ldapSession`. */
    ObjInstance* tpl = build_ldapSession_template();
    vm_push(OBJ_VAL(tpl));
    vm_type_register("ldapSession", OBJ_VAL(tpl));
    vm_pop();
}
