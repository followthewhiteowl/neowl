// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_oauth.c — Authentification OAuth 2.0.
 *
 * Surface :
 *   - Types `OAuth2Paramètres` (EN OAuth2Parameters) + `AuthToken` (EN AuthToken).
 *   - Enums `TypeAutorisation` (EN GrantType), `TypeRéponse` (EN ResponseType)
 *     + `MéthodeAuthClient` (EN ClientAuthMethod).
 *   - Natives `AuthToken(params, json)` (constructeur ; invariant doubles
 *     type-vs-fonction), `AuthIdentifie` (EN AuthIdentify) et
 *     `AuthRenouvelleToken` (EN AuthRefreshToken).
 *
 * Périmètre :
 *   - Flux machine-à-machine « client credentials » (RFC 6749 §4.4) +
 *     rafraîchissement (RFC 6749 §6).
 *   - Flux par code d'autorisation avec PKCE S256 (RFC 6749 §4.1, RFC 7636)
 *     et `state` anti-CSRF : AuthIdentifie(taCodeAutorisation) ouvre le
 *     navigateur système, capte la redirection sur localhost, échange le
 *     code contre un jeton. Le flux implicite (réponse directe par jeton)
 *     n'est PAS pris en charge : jeton exposé dans l'URL, déprécié par l'état
 *     de l'art (OAuth 2.1 / RFC 9700) -> erreur nette avant tout réseau.
 *     Divergence assumée du défaut canon (cf. ADR).
 *
 * Testabilité du flux code : la partie LIVE (bind+accept de la redirection +
 * ouverture navigateur) n'est pas déterministe (VM mono-thread, accept()
 * bloquant). Elle est validée par un e2e réel. Les trois segments purs
 * (construction URL+PKCE, extraction du code, échange) sont factorisés en
 * helpers appelés À LA FOIS par la partie live ET par trois natives test-only
 * `__OAuth*` (OWL_NAT_INTERNAL, cachées du catalogue public), contre lesquelles
 * les tests déterministes sont écrits. Aucune logique dupliquée.
 *
 * Références doc canon (formes FR + EN) :
 *   - OAuth2Paramètres / OAuth2Parameters ....... doc 1000022212
 *   - AuthToken ................................. doc 1000022220
 *   - AuthIdentifie / AuthIdentify ............. doc 1000022219
 *   - AuthRenouvelleToken / AuthRefreshToken ... doc 1000026168
 * Les formes EN des propriétés et des membres d'enum (GrantType,
 * gtClientCredentials, gtAuthorizationCode, ResponseType, AdditionalParameters,
 * AuthURL / RedirectionURL / TokenURL) proviennent du miroir en-US de la même
 * documentation de reference. `ClientAuthMethod` et `AutoRefreshToken`
 * sont des extensions OWL (hors canon).
 *
 * Testabilité : les appels HTTP internes passent par la couche interceptable
 * (mock_try_intercept_fn sur l'impl de HTTPEnvoie) PUIS, à défaut de mock,
 * l'impl HTTP réelle. Ce chemin NE repasse PAS par le dispatch VM — donc pas
 * de ré-entrée dans le hook pré-envoi posé sur HTTPEnvoie (cf. vm.c).
 *
 * Injection Authorization: Bearer + rafraîchissement auto (M1) : réalisés par
 * le hook pré-envoi gaté de vm.c (vm_set_http_presend_hook), posé ci-dessous à
 * l'init. Toute la logique OAuth reste ici.
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
/* net_tcp_server.h tire winsock2.h/windows.h sous Windows -> collision
 * TokenType (winnt.h vs scanner.h). Meme garde que stdlib_stomp/nats/mqtt :
 * ce module n'utilise que l'API native d'owl_errors, pas la compile-aware. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "vm.h"
#include "mock.h"
#include "sentinels.h"
#include "net_tcp_server.h"   /* capture live de la redirection (flux code) */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>

extern void  vm_push(Value v);
extern Value vm_pop(void);
extern ObjJson* json_parse_cstr(const char* src, int len);

/* ================================================================
 * Valeurs d'enum — zone sentinelle (cf sentinels.h). combinable=false,
 * valeurs mutuellement exclusives, hors plage des entiers naturels.
 * ================================================================ */

#define OWL_OAUTH_GRANT_BASE   OWL_SENTINEL_OFFSET(109)
#define OWL_OAUTH_GRANT_CC     (OWL_OAUTH_GRANT_BASE + 0)   /* taApplicationCliente */
#define OWL_OAUTH_GRANT_CODE   (OWL_OAUTH_GRANT_BASE + 1)   /* taCodeAutorisation */
#define OWL_OAUTH_GRANT_CUSTOM (OWL_OAUTH_GRANT_BASE + 2)   /* taGrantTypePersonnalisé */

#define OWL_OAUTH_RESP_BASE    OWL_SENTINEL_OFFSET(110)
#define OWL_OAUTH_RESP_CODE    (OWL_OAUTH_RESP_BASE + 0)     /* oauth2TypeRéponseCode */
#define OWL_OAUTH_RESP_TOKEN   (OWL_OAUTH_RESP_BASE + 1)     /* oauth2TypeRéponseToken */

/* Méthode d'authentification du client (extension OWL, hors canon). Offsets 111
 * (pays), 112 (iDestination), 113 (ldap) déjà pris → 114. Post/Basic exposés ;
 * JWT (assertion client) et None (client public) réservés pour extension. */
#define OWL_OAUTH_AUTHM_BASE   OWL_SENTINEL_OFFSET(114)
#define OWL_OAUTH_AUTHM_POST   (OWL_OAUTH_AUTHM_BASE + 0)    /* oauth2AuthPost */
#define OWL_OAUTH_AUTHM_BASIC  (OWL_OAUTH_AUTHM_BASE + 1)    /* oauth2AuthBasic */

/* Algorithme de signature d'une assertion JWT (type JWTParamètres). Offset 115
 * (108 log, 109/110/114 oauth, 111 pays, 112 iDest, 113 ldap déjà pris).
 * Phase A : RS256 seul honoré au runtime ; HS256 déclaré mais rejeté (ambiguïté
 * canon clé-vs-secret à lever). */
#define OWL_JWT_CRYPT_BASE     OWL_SENTINEL_OFFSET(115)
#define OWL_JWT_CRYPT_RS256    (OWL_JWT_CRYPT_BASE + 0)   /* jwtRS256 */
#define OWL_JWT_CRYPT_HS256    (OWL_JWT_CRYPT_BASE + 1)   /* jwtHS256 (différé) */

/* Durée de vie d'une assertion JWT client (claim exp - iat). Fenêtre courte :
 * l'assertion n'est présentée qu'une fois, immédiatement, au point de jeton. */
#define OWL_JWT_ASSERTION_TTL_SECONDS  300

/* Marge anti-dérive d'horloge (M3) : un jeton passe `Valide` = Faux cette
 * durée AVANT son expiration réelle, pour éviter d'émettre un jeton qui
 * expirera pendant le trajet réseau. 60 s = compromis usuel (tolère une
 * horloge locale en avance de ~1 min sur le serveur d'autorisation). */
#define OWL_OAUTH_EXPIRY_MARGIN_SECONDS  60

/* Échéance d'attente de la redirection navigateur (flux par code). Le login
 * utilisateur peut prendre un moment (saisie, MFA), mais pas indéfiniment :
 * passé ce délai, l'appel renonce plutôt que de bloquer à vie. */
#define OWL_OAUTH_REDIRECT_TIMEOUT_MS   120000

/* Longueurs des aléas (charset base64url, unreserved) : verifier PKCE dans la
 * plage RFC 7636 §4.1 (43-128), state CSRF suffisamment large contre le devinage. */
#define OWL_OAUTH_VERIFIER_LEN  64
#define OWL_OAUTH_STATE_LEN     32

/* ================================================================
 * Enums + constantes
 * ================================================================ */

static const char* const GRANT_VALUES[] = {
    "taApplicationCliente", "taCodeAutorisation",
    "taGrantTypePersonnalis\xc3\xa9", NULL
};
static const OwlEnum E_typeAutorisation = {
    .name = "TypeAutorisation", .nameAscii = "TypeAutorisation", .nameEN = "GrantType",
    .category = "OAuth2",
    .descFR = "Flux d'obtention du jeton : machine-a-machine (identifiants client), par code d'autorisation, ou personnalise (corps de requete entierement maitrise par l'appelant).",
    .descEN = "Token acquisition flow: machine-to-machine (client credentials), authorization code, or custom (request body fully controlled by the caller).",
    .values = GRANT_VALUES, .combinable = false,
};

static const char* const RESP_VALUES[] = {
    "oauth2TypeR\xc3\xa9ponseCode", "oauth2TypeR\xc3\xa9ponseToken", NULL
};
static const OwlEnum E_typeReponse = {
    .name = "TypeR\xc3\xa9ponse", .nameAscii = "TypeReponse", .nameEN = "ResponseType",
    .category = "OAuth2",
    .descFR = "Type de reponse attendu du fournisseur d'autorisation (code ou jeton).",
    .descEN = "Response type expected from the authorization provider (code or token).",
    .values = RESP_VALUES, .combinable = false,
};

static const char* const AUTHM_VALUES[] = {
    "oauth2AuthPost", "oauth2AuthBasic", NULL
};
static const OwlEnum E_methodeAuthClient = {
    .name = "M\xc3\xa9thodeAuthClient", .nameAscii = "MethodeAuthClient", .nameEN = "ClientAuthMethod",
    .category = "OAuth2",
    .descFR = "Methode d'authentification du client aupres du fournisseur : identifiants dans le corps de la requete ou dans un en-tete Authorization.",
    .descEN = "Client authentication method with the provider: credentials in the request body or in an Authorization header.",
    .values = AUTHM_VALUES, .combinable = false,
};

/* Algorithme de signature d'une assertion JWT (RFC 7515). Liste fermée :
 * jwtRS256 honoré, jwtHS256 déclaré mais différé. jetons techniques EN=FR. */
static const char* const CRYPTAGE_VALUES[] = {
    "jwtRS256", "jwtHS256", NULL
};
static const OwlEnum E_cryptage = {
    .name = "Cryptage", .nameAscii = "Cryptage", .nameEN = "Encryption",
    .category = "OAuth2",
    .descFR = "Algorithme de signature d'une assertion JWT : RSA SHA-256 (jwtRS256) ou HMAC SHA-256 (jwtHS256).",
    .descEN = "Signing algorithm of a JWT assertion: RSA SHA-256 (jwtRS256) or HMAC SHA-256 (jwtHS256).",
    .values = CRYPTAGE_VALUES, .combinable = false,
};

static const OwlEnum* const OAUTH_ENUMS[] = {
    &E_typeAutorisation, &E_typeReponse, &E_methodeAuthClient, &E_cryptage
};

static const OwlConstDef OAUTH_CONSTS[] = {
    { .name = "taApplicationCliente", .category = "OAuth2", .nameEN = "gtClientCredentials",
      .value = INTEGER_VAL(OWL_OAUTH_GRANT_CC),
      .descFR = "Flux machine-a-machine : le client s'authentifie seul avec ses identifiants, sans fenetre de login.",
      .descEN = "Machine-to-machine flow: the client authenticates with its own credentials, no login window." },
    { .name = "taCodeAutorisation", .category = "OAuth2", .nameEN = "gtAuthorizationCode",
      .value = INTEGER_VAL(OWL_OAUTH_GRANT_CODE),
      .descFR = "Flux par code d'autorisation : passe par une fenetre de login de l'utilisateur.",
      .descEN = "Authorization code flow: goes through an interactive user login window." },
    { .name = "taGrantTypePersonnalis\xc3\xa9", .category = "OAuth2", .nameEN = "gtCustomGrantType",
      .value = INTEGER_VAL(OWL_OAUTH_GRANT_CUSTOM),
      .descFR = "Flux personnalise : le corps de la requete d'obtention du jeton est construit a partir des seuls parametres supplementaires poses par l'appelant (ex. assertion client signee).",
      .descEN = "Custom flow: the token request body is built solely from the extra parameters set by the caller (e.g. a signed client assertion)." },
    { .name = "oauth2TypeR\xc3\xa9ponseCode", .category = "OAuth2", .nameEN = "oauth2ResponseTypeCode",
      .value = INTEGER_VAL(OWL_OAUTH_RESP_CODE),
      .descFR = "Reponse attendue : un code d'autorisation a echanger ensuite contre un jeton.",
      .descEN = "Expected response: an authorization code to later exchange for a token." },
    { .name = "oauth2TypeR\xc3\xa9ponseToken", .category = "OAuth2", .nameEN = "oauth2ResponseTypeToken",
      .value = INTEGER_VAL(OWL_OAUTH_RESP_TOKEN),
      .descFR = "Reponse attendue : directement un jeton d'acces.",
      .descEN = "Expected response: an access token directly." },
    { .name = "oauth2AuthPost", .category = "OAuth2", .nameEN = "oauth2AuthPost",
      .value = INTEGER_VAL(OWL_OAUTH_AUTHM_POST),
      .descFR = "Identifiants client transmis dans le corps de la requete d'obtention du jeton (client_id / client_secret).",
      .descEN = "Client credentials sent in the token request body (client_id / client_secret)." },
    { .name = "oauth2AuthBasic", .category = "OAuth2", .nameEN = "oauth2AuthBasic",
      .value = INTEGER_VAL(OWL_OAUTH_AUTHM_BASIC),
      .descFR = "Identifiants client transmis dans un en-tete Authorization de type Basic.",
      .descEN = "Client credentials sent in a Basic Authorization header." },
    { .name = "jwtRS256", .category = "OAuth2", .nameEN = "jwtRS256",
      .value = INTEGER_VAL(OWL_JWT_CRYPT_RS256),
      .descFR = "Signature d'assertion JWT par RSA avec empreinte SHA-256 (cle privee au format PEM).",
      .descEN = "JWT assertion signing with RSA and a SHA-256 digest (PEM private key)." },
    { .name = "jwtHS256", .category = "OAuth2", .nameEN = "jwtHS256",
      .value = INTEGER_VAL(OWL_JWT_CRYPT_HS256),
      .descFR = "Signature d'assertion JWT par HMAC avec empreinte SHA-256 (secret partage).",
      .descEN = "JWT assertion signing with HMAC and a SHA-256 digest (shared secret)." },
};

/* ================================================================
 * Types OAuth2Paramètres + AuthToken
 * ================================================================ */

static Value fn_auth_token_valide(int argCount, Value* args);  /* computeGetter */

static const OwlField FIELDS_OAuth2Parametres[] = {
    {"ClientID", NULL, "cha\xc3\xae""ne", "\"\"",
     "identifiant public du client aupres du fournisseur", "public client id at the provider", .nameEN="ClientID"},
    {"ClientSecret", NULL, "cha\xc3\xae""ne", "\"\"",
     "secret du client (confidentiel)", "client secret (confidential)", .nameEN="ClientSecret"},
    {"ParametresSupplementaires", "Param\xc3\xa8tresSuppl\xc3\xa9mentaires", "tableau associatif de cha\xc3\xae""nes", "",
     "entrees additionnelles fusionnees au corps de la requete d'obtention du jeton",
     "extra entries merged into the token request body", .nameEN="AdditionalParameters"},
    {"Scope", NULL, "cha\xc3\xae""ne", "\"\"",
     "portee demandee (liste d'autorisations separees par des espaces)", "requested scope (space-separated permissions)", .nameEN="Scope"},
    {"TypeAutorisation", NULL, "entier", "taCodeAutorisation",
     "flux d'obtention du jeton (voir TypeAutorisation)", "token acquisition flow (see GrantType)",
     .allowedBase=&E_typeAutorisation, .nameEN="GrantType"},
    {"TypeReponse", "TypeR\xc3\xa9ponse", "entier", "oauth2TypeR\xc3\xa9ponseCode",
     "type de reponse attendu (voir TypeReponse)", "expected response type (see ResponseType)",
     .allowedBase=&E_typeReponse, .nameEN="ResponseType"},
    {"URLAuth", NULL, "cha\xc3\xae""ne", "\"\"",
     "URL d'autorisation du fournisseur", "provider authorization URL", .nameEN="AuthURL"},
    {"URLRedirection", NULL, "cha\xc3\xae""ne", "\"\"",
     "URL de redirection apres autorisation", "redirection URL after authorization", .nameEN="RedirectionURL"},
    {"URLToken", NULL, "cha\xc3\xae""ne", "\"\"",
     "URL d'obtention du jeton", "token endpoint URL", .nameEN="TokenURL"},
    /* Extension OWL (hors canon) : methode d'authentification du client. */
    {"MethodeAuthClient", "M\xc3\xa9thodeAuthClient", "entier", "oauth2AuthPost",
     "methode d'authentification du client (voir MethodeAuthClient). Defaut oauth2AuthPost.",
     "client authentication method (see ClientAuthMethod). Default oauth2AuthPost.",
     .allowedBase=&E_methodeAuthClient, .nameEN="ClientAuthMethod"},
    {0},
};

static const OwlField FIELDS_AuthToken[] = {
    {"Valeur", NULL, "cha\xc3\xae""ne", "\"\"",
     "jeton d'acces (access_token)", "access token value", .nameEN="Value"},
    {"Valide", NULL, "bool\xc3\xa9""en", "Faux",
     "Vrai tant que le jeton reste loin de son expiration (lecture seule)",
     "True while the token stays far from expiration (read-only)",
     .nameEN="Valid", .computeGetter=fn_auth_token_valide},
    {"DateExpiration", NULL, "DateHeure", "Null",
     "date et heure d'expiration du jeton", "token expiration date and time", .nameEN="ExpirationDate"},
    {"Actualisation", NULL, "cha\xc3\xae""ne", "\"\"",
     "jeton de rafraichissement (refresh_token), vide si absent", "refresh token, empty if absent", .nameEN="Refresh"},
    {"ReponseServeur", "R\xc3\xa9ponseServeur", "buffer", "",
     "reponse brute du serveur d'autorisation (lecture seule)", "raw authorization server response (read-only)", .nameEN="ServerResponse"},
    {0},
};

/* JWTParamètres — authentification du client OAuth 2.0 par assertion JWT signée
 * (RFC 7523). Noms de champs stockés sous forme ASCII (accent-strippée) ; forme
 * canonique accentuée + forme EN déclarées pour le formatter et le bilinguisme.
 * ParamètresSupplémentairesURL : ordre bilingue inversé (suffixe FR, préfixe EN). */
static const OwlField FIELDS_JWTParametres[] = {
    {"Certificat", NULL, "cha\xc3\xae""ne", "\"\"",
     "certificat de signature (RSA uniquement)", "signing certificate (RSA only)", .nameEN="Certificate"},
    {"ClePrivee", "Cl\xc3\xa9Priv\xc3\xa9""e", "cha\xc3\xae""ne", "\"\"",
     "cle privee de signature au format PEM", "signing private key in PEM format", .nameEN="PrivateKey"},
    {"Consommateur", NULL, "cha\xc3\xae""ne", "\"\"",
     "destinataire de l'assertion (claim aud)", "assertion audience (aud claim)", .nameEN="Consumer"},
    {"Emetteur", "\xc3\x89metteur", "cha\xc3\xae""ne", "\"\"",
     "emetteur de l'assertion (claim iss)", "assertion issuer (iss claim)", .nameEN="Issuer"},
    {"IDClePrivee", "IDCl\xc3\xa9Priv\xc3\xa9""e", "cha\xc3\xae""ne", "\"\"",
     "identifiant de la cle privee (entete kid)", "private key identifier (kid header)", .nameEN="PrivateKeyID"},
    {"ParametresSupplementaires", "Param\xc3\xa8tresSuppl\xc3\xa9mentaires", "tableau associatif de cha\xc3\xae""nes", "",
     "revendications additionnelles fusionnees a l'assertion JWT",
     "extra claims merged into the JWT assertion", .nameEN="AdditionalParameters"},
    {"ParametresSupplementairesURL", "Param\xc3\xa8tresSuppl\xc3\xa9mentairesURL", "tableau associatif de cha\xc3\xae""nes", "",
     "parametres additionnels de la requete qui precede l'obtention du jeton",
     "extra parameters for the request preceding token acquisition", .nameEN="URLAdditionalParameters"},
    {"Scope", NULL, "cha\xc3\xae""ne", "\"\"",
     "portee demandee (liste d'autorisations separees par des espaces)", "requested scope (space-separated permissions)", .nameEN="Scope"},
    {"Sujet", NULL, "cha\xc3\xae""ne", "\"\"",
     "sujet de l'assertion (claim sub)", "assertion subject (sub claim)", .nameEN="Subject"},
    {"URLToken", NULL, "cha\xc3\xae""ne", "\"\"",
     "URL d'obtention du jeton", "token endpoint URL", .nameEN="TokenURL"},
    {"Cryptage", NULL, "entier", "jwtRS256",
     "algorithme de signature de l'assertion (voir Cryptage). Defaut jwtRS256.",
     "assertion signing algorithm (see Encryption). Default jwtRS256.",
     .allowedBase=&E_cryptage, .nameEN="Encryption"},
    {0},
};

static const char* const ALIAS_OAuth2Parametres[] = { "OAuth2Param\xc3\xa8tres", "OAuth2Parameters", NULL };
static const char* const ALIAS_AuthToken[]        = { "AuthToken", NULL };
static const char* const ALIAS_JWTParametres[]    = { "JWTParam\xc3\xa8tres", "JWTParameters", NULL };

static const OwlTypeDef OAUTH_TYPES[] = {
    { .canonical = "OAuth2Param\xc3\xa8tres", .canonicalAscii = "OAuth2Parametres",
      .aliases = ALIAS_OAuth2Parametres, .category = "OAuth2",
      .descFR = "Parametres d'une authentification OAuth 2.0 : identifiants client, URLs du fournisseur, portee, flux et parametres additionnels fusionnes a la requete d'obtention du jeton.",
      .descEN = "OAuth 2.0 authentication parameters: client credentials, provider URLs, scope, flow, and extra parameters merged into the token request.",
      .fields = FIELDS_OAuth2Parametres, .nameEN = "OAuth2Parameters", .genre = OWL_GENRE_M },
    { .canonical = "AuthToken", .canonicalAscii = "AuthToken",
      .aliases = ALIAS_AuthToken, .category = "OAuth2",
      .descFR = "Jeton d'authentification OAuth 2.0 : valeur du jeton d'acces, validite, expiration, jeton de rafraichissement et reponse brute du serveur. Retient ses parametres d'origine, necessaires au rafraichissement.",
      .descEN = "OAuth 2.0 authentication token: access token value, validity, expiration, refresh token and raw server response. Retains its original parameters, needed for refresh.",
      .fields = FIELDS_AuthToken, .nameEN = "AuthToken", .genre = OWL_GENRE_M },
    { .canonical = "JWTParam\xc3\xa8tres", .canonicalAscii = "JWTParametres",
      .aliases = ALIAS_JWTParametres, .category = "OAuth2",
      .descFR = "Parametres d'une authentification OAuth 2.0 par assertion JWT signee : emetteur, sujet, consommateur, cle privee, algorithme de signature et revendications additionnelles. Rempli puis passe a AuthIdentifie pour deroule le flux par assertion.",
      .descEN = "Parameters for an OAuth 2.0 authentication by signed JWT assertion: issuer, subject, consumer, private key, signing algorithm and extra claims. Filled then passed to AuthIdentify to run the assertion flow.",
      .fields = FIELDS_JWTParametres, .nameEN = "JWTParameters", .genre = OWL_GENRE_M },
};

/* ================================================================
 * Lecture de champs d'instance
 * ================================================================ */

static bool oauth_field(ObjInstance* inst, const char* name, Value* out) {
    if (inst == NULL) return false;
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    return table_get((Table*)&inst->fields, k, out);
}

/* Pointeur vers les octets d'un champ chaine (ou "" si absent / non-chaine).
 * Le resultat vit dans l'ObjString du champ : valide tant que `inst` est roote. */
static const char* oauth_get_str(ObjInstance* inst, const char* name) {
    Value v;
    if (oauth_field(inst, name, &v) && IS_STRING(v)) return AS_STRING(v)->chars;
    return "";
}

static int64_t oauth_get_int(ObjInstance* inst, const char* name, int64_t dflt) {
    Value v;
    if (oauth_field(inst, name, &v) && IS_INTEGER(v)) return AS_INTEGER(v);
    return dflt;
}

static bool oauth_get_bool(ObjInstance* inst, const char* name, bool dflt) {
    Value v;
    if (oauth_field(inst, name, &v) && IS_BOOL(v)) return AS_BOOL(v);
    return dflt;
}

static ObjInstance* oauth_get_instance(ObjInstance* inst, const char* name) {
    Value v;
    if (oauth_field(inst, name, &v) && IS_INSTANCE(v)) return AS_INSTANCE(v);
    return NULL;
}

/* ================================================================
 * Petit tampon de chaine (construction du corps form-urlencoded)
 * ================================================================ */

typedef struct { char* buf; int len; int cap; } OAuthBuf;

static void ob_init(OAuthBuf* b) { b->cap = 128; b->len = 0; b->buf = (char*)malloc((size_t)b->cap); if (b->buf) b->buf[0] = '\0'; }
static void ob_puts(OAuthBuf* b, const char* s) {
    if (b->buf == NULL || s == NULL) return;
    int n = (int)strlen(s);
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->buf = (char*)realloc(b->buf, (size_t)b->cap);
        if (b->buf == NULL) return;
    }
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* ================================================================
 * Helpers HTTP / JSON internes
 * ================================================================ */

static char* oauth_strdup(const char* s) {
    if (s == NULL) s = "";
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

/* Clone le template httpRequête (vm.types). Même motif que la famille AWS :
 * les sous-objets mutables (assoc Entête) sont dupliqués. */
static ObjInstance* oauth_clone_httprequete(void) {
    ObjString* k = obj_copy_string("httpRequete", 11);
    Value v;
    if (!table_get(&vm.types, k, &v) || !IS_INSTANCE(v)) return NULL;
    ObjInstance* tpl = AS_INSTANCE(v);
    ObjInstance* out = obj_new_instance(tpl->structName);
    vm_push(OBJ_VAL(out));
    for (int i = 0; i < tpl->fields.capacity; i++) {
        Entry* e = &tpl->fields.entries[i];
        if (e->key == NULL) continue;
        Value fv = e->value;
        if (IS_ASSOC_ARRAY(fv)) fv = OBJ_VAL(obj_assoc_clone(AS_ASSOC_ARRAY(fv)));
        obj_inst_set_kv(out, e->key, fv);
    }
    vm_pop();
    return out;
}

/* Pose un en-tete "name: value" dans l'assoc Entête d'une requete (rootée). */
static void oauth_header_set(ObjAssocArray* entete, const char* name, const char* value) {
    ObjString* k = obj_copy_string(name, (int)strlen(name));
    vm_push(OBJ_VAL(k));
    ObjString* val = obj_copy_string(value, (int)strlen(value));
    obj_assoc_set(entete, k, OBJ_VAL(val));
    vm_pop();
}

/* "Authorization: Basic base64(client_id:client_secret)" dans out (borné). */
static void oauth_basic_header(ObjInstance* params, char* out, size_t cap) {
    const char* cid = oauth_get_str(params, "ClientID");
    const char* csec = oauth_get_str(params, "ClientSecret");
    char raw[768];
    int rn = snprintf(raw, sizeof raw, "%s:%s", cid, csec);
    if (rn < 0) rn = 0;
    if (rn >= (int)sizeof raw) rn = (int)sizeof raw - 1;
    unsigned char b64[1024];
    size_t olen = 0;
    if (mbedtls_base64_encode(b64, sizeof b64, &olen,
                              (const unsigned char*)raw, (size_t)rn) != 0) {
        olen = 0;
    }
    b64[olen] = '\0';
    snprintf(out, cap, "Basic %s", b64);
}

/* true si les paramètres portent de quoi re-jouer un client_credentials. */
static bool oauth_params_cc_exploitable(ObjInstance* params) {
    if (params == NULL) return false;
    if (oauth_get_int(params, "TypeAutorisation", OWL_OAUTH_GRANT_CODE) != OWL_OAUTH_GRANT_CC)
        return false;
    return oauth_get_str(params, "URLToken")[0] != '\0'
        && oauth_get_str(params, "ClientID")[0] != '\0'
        && oauth_get_str(params, "ClientSecret")[0] != '\0';
}

/* Construit le corps form-urlencoded de la requete d'obtention du jeton.
 * `grantLine` = "grant_type=...". `refreshToken` non-NULL/non-vide ajoute
 * &refresh_token=. Les identifiants client vont dans le corps uniquement en
 * methode "post". Scope + ParamètresSupplémentaires fusionnes. *outUseBasic
 * indique si l'en-tete Authorization Basic doit etre pose par l'appelant.
 * Retourne une chaine malloc'd (caller free). */
static char* oauth_build_body(ObjInstance* params, const char* grantLine,
                              const char* refreshToken, bool* outUseBasic) {
    OAuthBuf b;
    ob_init(&b);
    ob_puts(&b, grantLine);
    if (refreshToken != NULL && refreshToken[0] != '\0') {
        ob_puts(&b, "&refresh_token=");
        ob_puts(&b, refreshToken);
    }
    int64_t method = oauth_get_int(params, "MethodeAuthClient", OWL_OAUTH_AUTHM_POST);
    bool useBasic = (method == OWL_OAUTH_AUTHM_BASIC);
    if (!useBasic) {
        ob_puts(&b, "&client_id=");
        ob_puts(&b, oauth_get_str(params, "ClientID"));
        ob_puts(&b, "&client_secret=");
        ob_puts(&b, oauth_get_str(params, "ClientSecret"));
    }
    const char* scope = oauth_get_str(params, "Scope");
    if (scope[0] != '\0') {
        ob_puts(&b, "&scope=");
        ob_puts(&b, scope);
    }
    /* ParamètresSupplémentaires (tableau associatif de chaines) -> &cle=valeur */
    Value pv;
    if (oauth_field(params, "ParametresSupplementaires", &pv) && IS_ASSOC_ARRAY(pv)) {
        ObjAssocArray* a = AS_ASSOC_ARRAY(pv);
        for (int i = 0; i < a->count; i++) {
            if (a->keys[i] == NULL) continue;
            ob_puts(&b, "&");
            ob_puts(&b, a->keys[i]->chars);
            ob_puts(&b, "=");
            if (IS_STRING(a->values[i])) ob_puts(&b, AS_STRING(a->values[i])->chars);
        }
    }
    if (outUseBasic) *outUseBasic = useBasic;
    return b.buf;
}

/* Construit une requete POST vers URLToken, corps `body`, en-tete Basic
 * si demandé. Retourne une ObjInstance httpRequête fraiche (non rootée). */
static ObjInstance* oauth_make_token_request(ObjInstance* params, const char* body, bool useBasic) {
    ObjInstance* req = oauth_clone_httprequete();
    if (req == NULL) return NULL;
    vm_push(OBJ_VAL(req));
    inst_field_set_str(req, "URL", oauth_get_str(params, "URLToken"));
    inst_field_set_str(req, "Methode", "POST");
    inst_field_set_str(req, "ContentType", "application/x-www-form-urlencoded");
    inst_field_set_str(req, "Contenu", body ? body : "");
    if (useBasic) {
        char hdr[1200];
        oauth_basic_header(params, hdr, sizeof hdr);
        ObjString* ek = obj_copy_string("Entete", 6);
        vm_push(OBJ_VAL(ek));
        Value ev;
        if (table_get(&req->fields, ek, &ev) && IS_ASSOC_ARRAY(ev)) {
            oauth_header_set(AS_ASSOC_ARRAY(ev), "Authorization", hdr);
        }
        vm_pop();  /* ek */
    }
    vm_pop();  /* req */
    return req;
}

/* Envoie `req` via la couche HTTP interceptable (mock PUIS impl réelle).
 * Pose *outResp (httpRéponse) et retourne true si une instance est obtenue.
 * NE repasse PAS par le dispatch VM -> pas de ré-entrée dans le hook. */
static bool oauth_send(ObjInstance* req, ObjInstance** outResp) {
    const OwlNativeDef* send = owl_registry_find("HTTPEnvoie");
    if (send == NULL || send->impl == NULL) return false;
    Value reqVal = OBJ_VAL(req);
    vm_push(reqVal);  /* root pendant intercept/envoi (peut allouer -> GC) */
    Value res;
    bool intercepted = mock_try_intercept_fn(send->impl, 1, &reqVal, &res);
    if (!intercepted) res = send->impl(1, &reqVal);
    vm_pop();  /* reqVal */
    if (!IS_INSTANCE(res)) return false;
    *outResp = AS_INSTANCE(res);
    return true;
}

/* Copie le corps (chaine OU buffer) de la réponse dans une chaine malloc'd.
 * Lecture immédiate post-envoi : aucune alloc OWL entre-temps. */
static char* oauth_response_body(ObjInstance* resp, int* outLen) {
    *outLen = 0;
    Value v;
    const char* bytes = NULL; int len = 0;
    if (oauth_field(resp, "Contenu", &v)) {
        if (IS_STRING(v)) { bytes = AS_STRING(v)->chars; len = AS_STRING(v)->length; }
        else if (IS_BUFFER(v)) { bytes = (const char*)AS_BUFFER(v)->bytes; len = AS_BUFFER(v)->length; }
    }
    if (bytes == NULL || len <= 0) return oauth_strdup("");
    char* out = (char*)malloc((size_t)len + 1);
    if (out == NULL) return NULL;
    memcpy(out, bytes, (size_t)len);
    out[len] = '\0';
    *outLen = len;
    return out;
}

/* Extrait access_token / refresh_token / expires_in d'une réponse JSON plate.
 * Pose des chaines malloc'd (ou NULL) + l'entier. Extraction immédiate : aucune
 * alloc OWL entre le parse et la copie C (l'ObjJson reste valide le temps de la
 * boucle de lecture). */
static void oauth_parse_token_json(const char* json, int len,
                                   char** access, char** refresh, int64_t* expiresIn) {
    *access = NULL; *refresh = NULL; *expiresIn = 0;
    if (json == NULL || len <= 0) return;
    ObjJson* j = json_parse_cstr(json, len);
    if (j == NULL || j->jtype != JSON_T_OBJECT) return;
    for (int i = 0; i < j->as.objectVal.count; i++) {
        const char* k = j->as.objectVal.keys[i]->chars;
        ObjJson* val = j->as.objectVal.values[i];
        if (val == NULL) continue;
        if (strcmp(k, "access_token") == 0 && val->jtype == JSON_T_STRING)
            *access = oauth_strdup(val->as.strVal->chars);
        else if (strcmp(k, "refresh_token") == 0 && val->jtype == JSON_T_STRING)
            *refresh = oauth_strdup(val->as.strVal->chars);
        else if (strcmp(k, "expires_in") == 0) {
            if (val->jtype == JSON_T_INT)      *expiresIn = val->as.intVal;
            else if (val->jtype == JSON_T_REAL) *expiresIn = (int64_t)val->as.realVal;
        }
    }
}

/* Construit une instance AuthToken depuis les paramètres d'origine (retenus)
 * et le corps brut de la réponse serveur. */
static ObjInstance* oauth_build_token(ObjInstance* params, const char* rawJson, int rawLen) {
    /* Parse AVANT toute alloc OWL : les chaines extraites sont des copies C. */
    char* access = NULL; char* refresh = NULL; int64_t expiresIn = 0;
    oauth_parse_token_json(rawJson, rawLen, &access, &refresh, &expiresIn);
    int64_t expEpoch = 0;
    if (access != NULL && expiresIn > 0) expEpoch = (int64_t)time(NULL) + expiresIn;

    ObjString* nm = obj_copy_string("AuthToken", 9);
    vm_push(OBJ_VAL(nm));
    ObjInstance* tok = obj_new_instance(nm);
    vm_push(OBJ_VAL(tok));

    inst_field_set_str(tok, "Valeur", access ? access : "");
    inst_field_set_str(tok, "Actualisation", refresh ? refresh : "");

    {
        ObjBuffer* buf = obj_new_buffer(rawJson ? rawJson : "", rawLen > 0 ? rawLen : 0);
        vm_push(OBJ_VAL(buf));
        inst_field_set_value(tok, "ReponseServeur", OBJ_VAL(buf));
        vm_pop();  /* buf */
    }

    inst_field_set_int(tok, "_exp", expEpoch);
    if (expEpoch > 0) {
        time_t tt = (time_t)expEpoch;
        struct tm* lt = localtime(&tt);
        if (lt != NULL) {
            ObjDateTime* dt = obj_new_datetime(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
                                               lt->tm_hour, lt->tm_min, lt->tm_sec, 0);
            vm_push(OBJ_VAL(dt));
            inst_field_set_value(tok, "DateExpiration", OBJ_VAL(dt));
            vm_pop();  /* dt */
        }
    }
    /* Paramètres retenus (état interne) : nécessaires au rafraîchissement. */
    inst_field_set_value(tok, "_params", params ? OBJ_VAL(params) : NIL_VAL);

    vm_pop();  /* tok */
    vm_pop();  /* nm */
    free(access);
    free(refresh);
    return tok;
}

/* État de validité (marge anti-dérive). Partagé par le getter et le hook. */
static bool oauth_token_is_valid(ObjInstance* tok) {
    const char* val = oauth_get_str(tok, "Valeur");
    if (val[0] == '\0') return false;
    int64_t exp = oauth_get_int(tok, "_exp", 0);
    if (exp <= 0) return false;
    return (int64_t)time(NULL) + OWL_OAUTH_EXPIRY_MARGIN_SECONDS < exp;
}

static Value fn_auth_token_valide(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) return BOOL_VAL(false);
    return BOOL_VAL(oauth_token_is_valid(AS_INSTANCE(args[0])));
}

/* ================================================================
 * Flux par code d'autorisation (PKCE S256, state CSRF, redirection)
 *
 * Segments purs ci-dessous : partagés entre la partie LIVE de
 * AuthIdentifie(taCodeAutorisation) et les natives test-only __OAuth*.
 * ================================================================ */

/* Alphabet base64url (RFC 4648 §5), identique à celui de ChaîneVersBase64 en
 * mode encodeBASE64URL (cf. stdlib_string.c). Sert au challenge PKCE ET au
 * tirage aléatoire : tous ses caractères sont « unreserved » (RFC 3986), donc
 * sûrs dans une URL comme dans un code_verifier (RFC 7636). */
static const char OAUTH_B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* base64url SANS remplissage de `len` octets -> chaîne malloc'd (caller free).
 * Reproduit à l'octet près l'encodage de ChaîneVersBase64(.., encodeBASE64URL) :
 * c'est l'oracle du test de conformité S256 (challenge figuré dans l'URL). */
static char* oauth_base64url(const unsigned char* src, int len) {
    int outLen = 4 * ((len + 2) / 3);
    char* out = (char*)malloc((size_t)outLen + 1);
    if (out == NULL) return NULL;
    int j = 0;
    for (int i = 0; i < len; i += 3) {
        int a = src[i];
        int b = (i + 1 < len) ? src[i + 1] : 0;
        int c = (i + 2 < len) ? src[i + 2] : 0;
        out[j++] = OAUTH_B64URL[(a >> 2) & 0x3F];
        out[j++] = OAUTH_B64URL[((a & 3) << 4) | ((b >> 4) & 0xF)];
        if (i + 1 < len) out[j++] = OAUTH_B64URL[((b & 0xF) << 2) | ((c >> 6) & 3)];
        if (i + 2 < len) out[j++] = OAUTH_B64URL[c & 0x3F];
    }
    out[j] = '\0';
    return out;
}

/* code_challenge PKCE = base64url(SHA256(ascii(verifier))) — méthode S256
 * (RFC 7636 §4.2). Chaîne malloc'd (caller free), NULL si échec crypto. */
static char* oauth_pkce_challenge(const char* verifier) {
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char*)verifier, strlen(verifier), digest, 0) != 0)
        return NULL;
    return oauth_base64url(digest, 32);
}

/* Tirage cryptographique de `n` caractères de l'alphabet base64url (sans biais :
 * 64 = 2^6, chaque octet mappé par &0x3F). Chaîne malloc'd (caller free), NULL
 * si échec d'entropie. Le DRBG est amorcé par appel (coût négligeable, le flux
 * code n'est pas un chemin chaud) — même motif que le socle TLS mock. */
static char* oauth_random_token(int n) {
    if (n <= 0 || n > 128) return NULL;
    char* out = (char*)malloc((size_t)n + 1);
    if (out == NULL) return NULL;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    unsigned char raw[128];
    bool ok = (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                     (const unsigned char*)"owl-oauth-pkce", 14) == 0)
           && (mbedtls_ctr_drbg_random(&drbg, raw, (size_t)n) == 0);
    if (ok) {
        for (int i = 0; i < n; i++) out[i] = OAUTH_B64URL[raw[i] & 0x3F];
        out[n] = '\0';
    } else {
        free(out);
        out = NULL;
    }
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return out;
}

/* Percent-encodage RFC 3986 : seuls les « unreserved » passent tels quels.
 * Les valeurs challenge/state (alphabet base64url) étant déjà unreserved, elles
 * apparaissent verbatim dans l'URL (ce que le test croise). Chaîne malloc'd. */
static char* oauth_url_encode(const char* s) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = strlen(s);
    char* out = (char*)malloc(n * 3 + 1);
    if (out == NULL) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = HEX[(c >> 4) & 0xF];
            out[j++] = HEX[c & 0xF];
        }
    }
    out[j] = '\0';
    return out;
}

static int oauth_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Décodage percent + '+' -> espace de `len` octets -> chaîne malloc'd. Lit les
 * valeurs de la query de redirection (un IdP peut percent-encoder le code). */
static char* oauth_url_decode(const char* s, int len) {
    char* out = (char*)malloc((size_t)len + 1);
    if (out == NULL) return NULL;
    int j = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '+') {
            out[j++] = ' ';
        } else if (c == '%' && i + 2 < len) {
            int hi = oauth_hexval(s[i + 1]), lo = oauth_hexval(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out[j++] = (char)((hi << 4) | lo); i += 2; }
            else out[j++] = c;
        } else {
            out[j++] = c;
        }
    }
    out[j] = '\0';
    return out;
}

/* Pose une entrée chaîne->chaîne dans un assoc roté. Même motif éprouvé que
 * oauth_header_set : obj_assoc_set est auto-rootant pour son grow,
 * la clé est poussée le temps de créer la valeur. */
static void oauth_assoc_set_str(ObjAssocArray* a, const char* key, const char* val) {
    ObjString* k = obj_copy_string(key, (int)strlen(key));
    vm_push(OBJ_VAL(k));
    ObjString* v = obj_copy_string(val, (int)strlen(val));
    obj_assoc_set(a, k, OBJ_VAL(v));
    vm_pop();  /* k */
}

/* Helper #1 — construit l'URL d'autorisation et génère verifier/challenge/state.
 * Toutes les sorties sont des chaînes malloc'd (caller free) ; false (+ sorties
 * NULL) en cas d'échec crypto/mémoire. URL :
 *   URLAuth?response_type=code&client_id=..&redirect_uri=..[&scope=..]
 *          &state=..&code_challenge=..&code_challenge_method=S256
 * Les valeurs sont percent-encodées. Jamais response_type=token (implicit). */
static bool oauth_build_auth_url(ObjInstance* params, char** outUrl,
                                 char** outVerifier, char** outChallenge,
                                 char** outState) {
    *outUrl = *outVerifier = *outChallenge = *outState = NULL;
    char* verifier = oauth_random_token(OWL_OAUTH_VERIFIER_LEN);
    char* state    = oauth_random_token(OWL_OAUTH_STATE_LEN);
    char* challenge = (verifier != NULL) ? oauth_pkce_challenge(verifier) : NULL;
    if (verifier == NULL || state == NULL || challenge == NULL) {
        free(verifier); free(state); free(challenge);
        return false;
    }

    OAuthBuf b;
    ob_init(&b);
    ob_puts(&b, oauth_get_str(params, "URLAuth"));
    ob_puts(&b, "?response_type=code");
    ob_puts(&b, "&client_id=");
    { char* e = oauth_url_encode(oauth_get_str(params, "ClientID")); ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&redirect_uri=");
    { char* e = oauth_url_encode(oauth_get_str(params, "URLRedirection")); ob_puts(&b, e ? e : ""); free(e); }
    const char* scope = oauth_get_str(params, "Scope");
    if (scope[0] != '\0') {
        ob_puts(&b, "&scope=");
        char* e = oauth_url_encode(scope); ob_puts(&b, e ? e : ""); free(e);
    }
    ob_puts(&b, "&state=");
    { char* e = oauth_url_encode(state); ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&code_challenge=");
    { char* e = oauth_url_encode(challenge); ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&code_challenge_method=S256");

    /* ParamètresSupplémentaires : paramètres de la première requête
     * d'authentification, fusionnés sur l'URL d'autorisation (ex. router vers un
     * annuaire d'entreprise). Clé ET valeur percent-encodées comme le reste de
     * l'URL. Même itération assoc que le corps du token (oauth_build_body). */
    Value pv;
    if (oauth_field(params, "ParametresSupplementaires", &pv) && IS_ASSOC_ARRAY(pv)) {
        ObjAssocArray* a = AS_ASSOC_ARRAY(pv);
        for (int i = 0; i < a->count; i++) {
            if (a->keys[i] == NULL) continue;
            ob_puts(&b, "&");
            { char* e = oauth_url_encode(a->keys[i]->chars); ob_puts(&b, e ? e : ""); free(e); }
            ob_puts(&b, "=");
            if (IS_STRING(a->values[i])) {
                char* e = oauth_url_encode(AS_STRING(a->values[i])->chars); ob_puts(&b, e ? e : ""); free(e);
            }
        }
    }

    if (b.buf == NULL) {
        free(verifier); free(state); free(challenge);
        return false;
    }
    *outUrl = b.buf;
    *outVerifier = verifier;
    *outChallenge = challenge;
    *outState = state;
    return true;
}

/* Helper #2 — extrait le `code` de l'URL de redirection reçue, après contrôle
 * du `state` (anti-CSRF). Chaîne malloc'd (caller free) en succès ; NULL si
 * param error= présent, code absent, ou state discordant. N'émet PAS l'erreur :
 * l'appelant la pose avec son propre nom de native (contexte utilisateur). */
static char* oauth_extract_code(const char* redirectUrl, const char* expectedState) {
    const char* q = strchr(redirectUrl, '?');
    char* code = NULL; char* gotState = NULL; char* errorParam = NULL;
    if (q != NULL) {
        const char* p = q + 1;
        while (*p != '\0') {
            const char* amp = strchr(p, '&');
            const char* end = amp ? amp : p + strlen(p);
            const char* eq = memchr(p, '=', (size_t)(end - p));
            if (eq != NULL) {
                int klen = (int)(eq - p);
                const char* vstart = eq + 1;
                int vlen = (int)(end - vstart);
                char* val = oauth_url_decode(vstart, vlen);
                if (klen == 4 && strncmp(p, "code", 4) == 0)       { free(code);       code = val; }
                else if (klen == 5 && strncmp(p, "state", 5) == 0) { free(gotState);   gotState = val; }
                else if (klen == 5 && strncmp(p, "error", 5) == 0) { free(errorParam); errorParam = val; }
                else free(val);
            }
            if (amp == NULL) break;
            p = amp + 1;
        }
    }
    bool fail = (errorParam != NULL)                 /* le fournisseur a refusé */
             || (code == NULL)                        /* pas de code dans la redirection */
             || (expectedState != NULL && expectedState[0] != '\0'
                 && (gotState == NULL || strcmp(gotState, expectedState) != 0));  /* CSRF */
    free(gotState);
    free(errorParam);
    if (fail) { free(code); return NULL; }
    return code;
}

/* Helper #3 — échange authorization_code -> AuthToken. POST vers URLToken :
 *   grant_type=authorization_code&code=..&code_verifier=..&redirect_uri=..
 * + identifiants client selon MéthodeAuthClient (basic en-tête / post corps),
 * comme la phase 1. Chemin HTTP interceptable (mock PUIS impl réelle). Retourne
 * un AuthToken (marqué invalide si la réponse ne porte pas de jeton). */
static ObjInstance* oauth_exchange_code(ObjInstance* params, const char* code,
                                        const char* verifier) {
    int64_t method = oauth_get_int(params, "MethodeAuthClient", OWL_OAUTH_AUTHM_POST);
    bool useBasic = (method == OWL_OAUTH_AUTHM_BASIC);

    OAuthBuf b;
    ob_init(&b);
    ob_puts(&b, "grant_type=authorization_code");
    ob_puts(&b, "&code=");
    { char* e = oauth_url_encode(code ? code : ""); ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&code_verifier=");
    { char* e = oauth_url_encode(verifier ? verifier : ""); ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&redirect_uri=");
    { char* e = oauth_url_encode(oauth_get_str(params, "URLRedirection")); ob_puts(&b, e ? e : ""); free(e); }
    if (!useBasic) {
        ob_puts(&b, "&client_id=");
        { char* e = oauth_url_encode(oauth_get_str(params, "ClientID")); ob_puts(&b, e ? e : ""); free(e); }
        ob_puts(&b, "&client_secret=");
        { char* e = oauth_url_encode(oauth_get_str(params, "ClientSecret")); ob_puts(&b, e ? e : ""); free(e); }
    }

    ObjInstance* req = oauth_make_token_request(params, b.buf ? b.buf : "", useBasic);
    free(b.buf);
    if (req == NULL) return oauth_build_token(params, "", 0);
    vm_push(OBJ_VAL(req));
    ObjInstance* resp = NULL;
    char* respBody = NULL; int respLen = 0;
    if (oauth_send(req, &resp)) respBody = oauth_response_body(resp, &respLen);
    vm_pop();  /* req */
    ObjInstance* tok = oauth_build_token(params, respBody ? respBody : "", respLen);
    free(respBody);
    return tok;
}

/* Ouvre l'URL dans le navigateur système via la native OuvreURL, par le chemin
 * interceptable (mock PUIS impl réelle) — exactement comme oauth_send pour
 * HTTPEnvoie. C'est ce qui permet au test de mocker le navigateur (aucune
 * fenêtre réelle) et de vérifier qu'il a bien été sollicité. */
static bool oauth_open_browser(const char* url) {
    const OwlNativeDef* open = owl_registry_find("OuvreURL");
    if (open == NULL || open->impl == NULL) return false;
    ObjString* u = obj_copy_string(url, (int)strlen(url));
    vm_push(OBJ_VAL(u));
    Value uv = OBJ_VAL(u);
    Value res;
    bool intercepted = mock_try_intercept_fn(open->impl, 1, &uv, &res);
    if (!intercepted) res = open->impl(1, &uv);
    vm_pop();  /* u */
    return IS_BOOL(res) ? AS_BOOL(res) : false;
}

/* Port de l'URL de redirection ("http://localhost:18240/retour" -> 18240).
 * -1 si introuvable : sans port explicite, pas d'écoute fiable. */
static int oauth_redirect_port(const char* url) {
    const char* host = strstr(url, "://");
    host = host ? host + 3 : url;
    const char* colon = strchr(host, ':');
    if (colon == NULL) return -1;
    const char* p = colon + 1;
    if (*p < '0' || *p > '9') return -1;
    int port = 0;
    while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    if (port <= 0 || port > 65535) return -1;
    return port;
}

/* Orchestration LIVE du flux par code d'autorisation. NON couverte par les
 * tests automatiques (VM mono-thread, accept() bloquant) : validée par un e2e
 * navigateur réel. Les trois segments purs sont les helpers partagés avec les
 * natives __OAuth* (aucune logique dupliquée). Séquence : refus implicit ->
 * validation params -> URL+PKCE+state -> bind/listen -> OuvreURL -> accept
 * (timeout) -> lecture GET + page de clôture -> extraction code -> échange. */
static Value oauth_identifie_code(ObjInstance* params) {
    /* (1) Flux implicite (réponse directe par jeton) refusé AVANT tout réseau :
     * jeton exposé dans l'URL, déprécié (OAuth 2.1 / RFC 9700). Divergence
     * assumée du défaut canon. Discrimination explicite (pas un reliquat). */
    if (oauth_get_int(params, "TypeReponse", OWL_OAUTH_RESP_CODE) == OWL_OAUTH_RESP_TOKEN) {
        owl_emit_native_error(E_NATIVE_OAUTH_IMPLICIT_UNSUPPORTED, "AuthIdentifie", NULL);
        return NIL_VAL;
    }
    /* (2) Sans URL d'autorisation ni port de redirection exploitable, rien à
     * ouvrir ni à écouter : erreur nette, aucun navigateur sollicité. */
    const char* urlAuth = oauth_get_str(params, "URLAuth");
    int port = oauth_redirect_port(oauth_get_str(params, "URLRedirection"));
    if (urlAuth[0] == '\0' || port < 0) {
        owl_emit_native_error(E_NATIVE_OAUTH_PARAMS_INCOMPLETE, "AuthIdentifie",
                              "detail", "URL d'autorisation ou port de redirection manquant", NULL);
        return NIL_VAL;
    }

    /* (3) URL d'autorisation + PKCE + state (helper #1 partagé). */
    char *authUrl = NULL, *verifier = NULL, *challenge = NULL, *state = NULL;
    if (!oauth_build_auth_url(params, &authUrl, &verifier, &challenge, &state)) {
        free(authUrl); free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "AuthIdentifie",
                              "detail", "préparation de la demande d'autorisation impossible", NULL);
        return NIL_VAL;
    }

    /* (4) bind+listen sur localhost:port AVANT d'ouvrir le navigateur : la
     * redirection peut arriver dès la première milliseconde après OuvreURL. */
    OwlTcpListener listener;
    if (!owl_tcp_listener_ouvre(&listener, "127.0.0.1", port, 1, false)) {
        free(authUrl); free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "AuthIdentifie",
                              "detail", "écoute sur le port de redirection impossible", NULL);
        return NIL_VAL;
    }

    /* (5) Ouverture navigateur (chemin mockable). Échec -> erreur nette : on ne
     * reste pas bloqué sur une redirection qui n'arrivera jamais. */
    bool opened = oauth_open_browser(authUrl);
    free(authUrl);
    if (!opened) {
        owl_tcp_listener_ferme(&listener);
        free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_BROWSER_UNAVAILABLE, "AuthIdentifie", NULL);
        return NIL_VAL;
    }

    /* (6) Attente bornée de la redirection. */
    OwlTcpConn conn;
    if (owl_tcp_listener_accepte(&listener, OWL_OAUTH_REDIRECT_TIMEOUT_MS, &conn) != OWL_TCPS_OK) {
        owl_tcp_listener_ferme(&listener);
        free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "AuthIdentifie",
                              "detail", "aucune redirection reçue dans le délai imparti", NULL);
        return NIL_VAL;
    }

    /* (7) Lecture de la ligne de requête GET, page de clôture, fermeture. */
    char reqbuf[4096];
    int rn = owl_tcp_conn_lit(&conn, reqbuf, sizeof reqbuf - 1, OWL_OAUTH_REDIRECT_TIMEOUT_MS);
    if (rn < 0) rn = 0;
    reqbuf[rn] = '\0';

    static const char BODY[] =
        "<html><body>Authentification termin\xc3\xa9""e, vous pouvez fermer cette "
        "fen\xc3\xaatre.</body></html>";
    char hdr[160];
    int hn = snprintf(hdr, sizeof hdr,
                      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                      "Connection: close\r\nContent-Length: %zu\r\n\r\n",
                      sizeof BODY - 1);
    if (hn > 0) owl_tcp_conn_ecrit(&conn, hdr, (size_t)hn, OWL_OAUTH_REDIRECT_TIMEOUT_MS);
    owl_tcp_conn_ecrit(&conn, BODY, sizeof BODY - 1, OWL_OAUTH_REDIRECT_TIMEOUT_MS);
    owl_tcp_conn_ferme(&conn);
    owl_tcp_listener_ferme(&listener);

    /* Cible de la requête : "GET /retour?code=..&state=.. HTTP/1.1". */
    char* target = NULL;
    if (strncmp(reqbuf, "GET ", 4) == 0) {
        char* sp = strchr(reqbuf + 4, ' ');
        if (sp != NULL) {
            int tlen = (int)(sp - (reqbuf + 4));
            target = (char*)malloc((size_t)tlen + 1);
            if (target != NULL) { memcpy(target, reqbuf + 4, (size_t)tlen); target[tlen] = '\0'; }
        }
    }
    /* (8) Extraction du code + vérification du state (helper #2). */
    char* code = oauth_extract_code(target ? target : "", state);
    free(target);
    if (code == NULL) {
        free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "AuthIdentifie",
                              "detail", "redirection d'autorisation invalide", NULL);
        return NIL_VAL;
    }

    /* (9) Échange code -> jeton (helper #3). */
    ObjInstance* tok = oauth_exchange_code(params, code, verifier);
    free(code);
    free(verifier);
    free(challenge);
    free(state);
    return OBJ_VAL(tok);
}

/* ================================================================
 * Assertion JWT signée RS256 (RFC 7515 / 7519) — type JWTParamètres
 *
 * Cœur partagé par AuthIdentifie(JWTParamètres) (construction + signature de
 * l'assertion client, flux jwt-bearer RFC 7523) et le seam de test
 * __JWTVerifieSignature (relecture cryptographique). base64url SANS remplissage
 * partout, primitives mbedtls_pk comme code_sign.c.
 * ================================================================ */

/* Décodage base64url SANS remplissage (RFC 7515 §2) -> octets malloc'd (caller
 * free), *outLen posé. NULL si caractère hors alphabet. Inverse d'oauth_base64url. */
static unsigned char* oauth_base64url_decode(const char* s, int len, int* outLen) {
    *outLen = 0;
    static signed char rev[256];
    static int inited = 0;
    if (!inited) {
        for (int i = 0; i < 256; i++) rev[i] = -1;
        for (int i = 0; OAUTH_B64URL[i] != '\0'; i++)
            rev[(unsigned char)OAUTH_B64URL[i]] = (signed char)i;
        inited = 1;
    }
    unsigned char* out = (unsigned char*)malloc((size_t)(len / 4 + 1) * 3 + 3);
    if (out == NULL) return NULL;
    int acc = 0, nbits = 0, j = 0;
    for (int i = 0; i < len; i++) {
        signed char v = rev[(unsigned char)s[i]];
        if (v < 0) { free(out); return NULL; }
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8) { nbits -= 8; out[j++] = (unsigned char)((acc >> nbits) & 0xFF); }
    }
    *outLen = j;
    return out;
}

/* Append `s` à `b` comme contenu de chaîne JSON échappé (sans les guillemets).
 * Échappe ", \, les sauts de ligne et les caractères de contrôle ; l'UTF-8
 * >= 0x20 passe verbatim (accepté tel quel par JSON). */
static void ob_put_json_escaped(OAuthBuf* b, const char* s) {
    if (s == NULL) return;
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        unsigned char c = *p;
        if (c == '"')       ob_puts(b, "\\\"");
        else if (c == '\\') ob_puts(b, "\\\\");
        else if (c == '\n') ob_puts(b, "\\n");
        else if (c == '\r') ob_puts(b, "\\r");
        else if (c == '\t') ob_puts(b, "\\t");
        else if (c < 0x20)  { char u[8]; snprintf(u, sizeof u, "\\u%04x", c); ob_puts(b, u); }
        else                { char ch[2] = { (char)c, '\0' }; ob_puts(b, ch); }
    }
}

/* Cœur de signature RS256 : entête + payload JSON BRUTS -> JWT
 * `header.payload.signature` (chaîne malloc'd, caller free) ou NULL (clé PEM
 * invalide, crypto). base64url SANS remplissage, RSASSA-PKCS1-v1_5 SHA-256
 * (mbedtls_pk) — déterministe pour des entrées fixes. N'émet PAS d'erreur :
 * l'appelant décide du code selon son contexte. Partagé par JWTCréeAssertion
 * (JSON fournis par l'appelant) et AuthIdentifie(JWTParamètres) (JSON construits). */
static char* jwt_sign_rs256(const char* headerJson, int headerLen,
                            const char* payloadJson, int payloadLen,
                            const char* pem) {
    char* hB64 = oauth_base64url((const unsigned char*)headerJson, headerLen);
    char* pB64 = oauth_base64url((const unsigned char*)payloadJson, payloadLen);
    if (hB64 == NULL || pB64 == NULL) { free(hB64); free(pB64); return NULL; }

    OAuthBuf sb;
    ob_init(&sb);
    ob_puts(&sb, hB64);
    ob_puts(&sb, ".");
    ob_puts(&sb, pB64);
    free(hB64);
    free(pB64);
    if (sb.buf == NULL) return NULL;

    /* Signature RSASSA-PKCS1-v1_5 SHA-256 du signing input (mbedtls_pk). */
    unsigned char digest[32];
    if (mbedtls_sha256((const unsigned char*)sb.buf, (size_t)sb.len, digest, 0) != 0) {
        free(sb.buf); return NULL;
    }
    mbedtls_pk_context       pk;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_init(&pk);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    unsigned char sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sigLen = 0;
    char* sigB64 = NULL;
    bool ok = (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                     (const unsigned char*)"owl-jwt-sign", 12) == 0);
    /* PEM : la longueur passée à mbedtls DOIT inclure le '\0' terminal. */
    if (ok) ok = (mbedtls_pk_parse_key(&pk, (const unsigned char*)pem, strlen(pem) + 1,
                                       NULL, 0, mbedtls_ctr_drbg_random, &drbg) == 0);
    if (ok) ok = (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, digest, sizeof digest,
                                  sig, sizeof sig, &sigLen,
                                  mbedtls_ctr_drbg_random, &drbg) == 0);
    if (ok) sigB64 = oauth_base64url(sig, (int)sigLen);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    if (sigB64 == NULL) { free(sb.buf); return NULL; }

    ob_puts(&sb, ".");
    ob_puts(&sb, sigB64);
    free(sigB64);
    return sb.buf;  /* header.payload.signature */
}

/* Valeur du champ `alg` d'un entête JWT (JSON plat) -> chaîne malloc'd (caller
 * free) ou NULL si absent / non-chaîne / JSON invalide. Lecture immédiate :
 * aucune alloc OWL entre le parse et la copie C. */
static char* jwt_header_alg(const char* headerJson, int len) {
    if (headerJson == NULL || len <= 0) return NULL;
    ObjJson* j = json_parse_cstr(headerJson, len);
    if (j == NULL || j->jtype != JSON_T_OBJECT) return NULL;
    char* alg = NULL;
    for (int i = 0; i < j->as.objectVal.count; i++) {
        const char* k = j->as.objectVal.keys[i]->chars;
        ObjJson* v = j->as.objectVal.values[i];
        if (v != NULL && strcmp(k, "alg") == 0 && v->jtype == JSON_T_STRING) {
            alg = oauth_strdup(v->as.strVal->chars);
            break;
        }
    }
    return alg;
}

/* Construit et signe l'assertion JWT RS256 depuis les JWTParamètres. Rend le
 * JWT `header.payload.signature` (chaîne malloc'd, caller free) ou NULL en cas
 * d'échec (clé PEM invalide, crypto). N'émet PAS d'erreur : l'appelant décide
 * du code selon son contexte. `params` doit rester rooté par l'appelant. */
static char* jwt_build_assertion_rs256(ObjInstance* params) {
    static const char HEADER_JSON[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";

    /* Payload : claims standard (RFC 7519) + AdditionalParameters fusionnés. */
    int64_t iat = (int64_t)time(NULL);
    int64_t exp = iat + OWL_JWT_ASSERTION_TTL_SECONDS;
    OAuthBuf pb;
    ob_init(&pb);
    ob_puts(&pb, "{\"iss\":\"");  ob_put_json_escaped(&pb, oauth_get_str(params, "Emetteur"));
    ob_puts(&pb, "\",\"sub\":\""); ob_put_json_escaped(&pb, oauth_get_str(params, "Sujet"));
    ob_puts(&pb, "\",\"aud\":\""); ob_put_json_escaped(&pb, oauth_get_str(params, "Consommateur"));
    ob_puts(&pb, "\",\"iat\":");
    { char n[32]; snprintf(n, sizeof n, "%lld", (long long)iat); ob_puts(&pb, n); }
    ob_puts(&pb, ",\"exp\":");
    { char n[32]; snprintf(n, sizeof n, "%lld", (long long)exp); ob_puts(&pb, n); }
    Value pv;
    if (oauth_field(params, "ParametresSupplementaires", &pv) && IS_ASSOC_ARRAY(pv)) {
        ObjAssocArray* a = AS_ASSOC_ARRAY(pv);
        for (int i = 0; i < a->count; i++) {
            if (a->keys[i] == NULL) continue;
            ob_puts(&pb, ",\"");
            ob_put_json_escaped(&pb, a->keys[i]->chars);
            ob_puts(&pb, "\":\"");
            if (IS_STRING(a->values[i])) ob_put_json_escaped(&pb, AS_STRING(a->values[i])->chars);
            ob_puts(&pb, "\"");
        }
    }
    ob_puts(&pb, "}");
    if (pb.buf == NULL) return NULL;

    char* jwt = jwt_sign_rs256(HEADER_JSON, (int)(sizeof HEADER_JSON - 1),
                               pb.buf, pb.len, oauth_get_str(params, "ClePrivee"));
    free(pb.buf);
    return jwt;
}

/* AuthIdentifie(JWTParamètres) : construit + signe l'assertion, l'échange
 * contre un jeton par le flux jwt-bearer (RFC 7523, authentification client).
 * Chemin HTTP interceptable (mock PUIS impl réelle). jwtHS256 (ou tout autre
 * algorithme) -> refus net AVANT tout réseau. */
static Value jwt_identifie(ObjInstance* params) {
    int64_t crypt = oauth_get_int(params, "Cryptage", OWL_JWT_CRYPT_RS256);
    if (crypt != OWL_JWT_CRYPT_RS256) {
        owl_emit_native_error(E_NATIVE_JWT_ALGORITHM_UNSUPPORTED, "AuthIdentifie", NULL);
        return NIL_VAL;
    }
    char* jwt = jwt_build_assertion_rs256(params);
    if (jwt == NULL) {
        owl_emit_native_error(E_NATIVE_OAUTH_PARAMS_INCOMPLETE, "AuthIdentifie",
                              "detail", "signature de l'assertion JWT impossible (cle privee invalide)", NULL);
        return NIL_VAL;
    }

    /* Corps form-urlencoded du flux jwt-bearer (client_assertion + type RFC 7523). */
    OAuthBuf b;
    ob_init(&b);
    ob_puts(&b, "grant_type=client_credentials");
    ob_puts(&b, "&client_assertion_type=");
    { char* e = oauth_url_encode("urn:ietf:params:oauth:client-assertion-type:jwt-bearer");
      ob_puts(&b, e ? e : ""); free(e); }
    ob_puts(&b, "&client_assertion=");
    { char* e = oauth_url_encode(jwt); ob_puts(&b, e ? e : ""); free(e); }
    const char* scope = oauth_get_str(params, "Scope");
    if (scope[0] != '\0') {
        ob_puts(&b, "&scope=");
        char* e = oauth_url_encode(scope); ob_puts(&b, e ? e : ""); free(e);
    }
    free(jwt);

    ObjInstance* req = oauth_make_token_request(params, b.buf ? b.buf : "", false);
    free(b.buf);
    if (req == NULL) {
        owl_emit_native_error(E_NATIVE_OAUTH_PARAMS_INCOMPLETE, "AuthIdentifie",
                              "detail", "requete d'obtention du jeton impossible", NULL);
        return NIL_VAL;
    }
    vm_push(OBJ_VAL(req));
    ObjInstance* resp = NULL;
    char* respBody = NULL; int respLen = 0;
    if (oauth_send(req, &resp)) respBody = oauth_response_body(resp, &respLen);
    vm_pop();  /* req */
    ObjInstance* tok = oauth_build_token(params, respBody ? respBody : "", respLen);
    free(respBody);
    return OBJ_VAL(tok);
}

/* ================================================================
 * Natives
 * ================================================================ */

/* Constructeur AuthToken(oParams, sJson). */
static Value fn_auth_token(int argCount, Value* args) {
    ObjInstance* params = (argCount >= 1 && IS_INSTANCE(args[0])) ? AS_INSTANCE(args[0]) : NULL;
    const char* json = "";
    int jlen = 0;
    if (argCount >= 2 && IS_STRING(args[1])) {
        json = AS_STRING(args[1])->chars;
        jlen = AS_STRING(args[1])->length;
    }
    /* params + args[1] restent rootés via la pile d'arguments de la native. */
    return OBJ_VAL(oauth_build_token(params, json, jlen));
}

/* Corps form-urlencoded du flux personnalisé (gtCustomGrantType) : fusionne tels
 * quels les ParamètresSupplémentaires posés par l'appelant (clé verbatim, valeur
 * percent-encodée RFC 3986). Le framework n'ajoute aucun paramètre de son cru —
 * l'appelant maîtrise entièrement le corps (ex. client_assertion signée + son
 * client_assertion_type, RFC 7523). Chaîne malloc'd (caller free). */
static char* oauth_build_custom_body(ObjInstance* params) {
    OAuthBuf b;
    ob_init(&b);
    bool first = true;
    Value pv;
    if (oauth_field(params, "ParametresSupplementaires", &pv) && IS_ASSOC_ARRAY(pv)) {
        ObjAssocArray* a = AS_ASSOC_ARRAY(pv);
        for (int i = 0; i < a->count; i++) {
            if (a->keys[i] == NULL) continue;
            if (!first) ob_puts(&b, "&");
            first = false;
            ob_puts(&b, a->keys[i]->chars);
            ob_puts(&b, "=");
            if (IS_STRING(a->values[i])) {
                char* e = oauth_url_encode(AS_STRING(a->values[i])->chars);
                ob_puts(&b, e ? e : ""); free(e);
            }
        }
    }
    return b.buf;
}

/* AuthIdentifie(oParams) -> AuthToken. Dispatch : code d'autorisation
 * (navigateur), client_credentials (machine-à-machine) ou personnalisé. */
static Value fn_auth_identifie(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthIdentifie", NULL);
        return NIL_VAL;
    }
    ObjInstance* params = AS_INSTANCE(args[0]);
    /* Dispatch polymorphe sur le TYPE de l'argument : une assertion JWT signée
     * (JWTParamètres) déroule le flux jwt-bearer ; sinon flux OAuth2 classique. */
    if (params->structName != NULL &&
        strcasecmp(params->structName->chars, "JWTParametres") == 0) {
        return jwt_identifie(params);
    }
    int64_t grant = oauth_get_int(params, "TypeAutorisation", OWL_OAUTH_GRANT_CODE);
    if (grant == OWL_OAUTH_GRANT_CODE) {
        /* Flux par code d'autorisation (navigateur + redirection + PKCE). */
        return oauth_identifie_code(params);
    }
    if (grant == OWL_OAUTH_GRANT_CUSTOM) {
        /* Flux personnalisé : corps bâti depuis les seuls ParamètresSupplémentaires
         * (l'appelant y a posé son assertion et son type). Chemin HTTP mockable. */
        char* body = oauth_build_custom_body(params);
        ObjInstance* req = oauth_make_token_request(params, body ? body : "", false);
        free(body);
        if (req == NULL) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthIdentifie", NULL);
            return NIL_VAL;
        }
        vm_push(OBJ_VAL(req));
        ObjInstance* resp = NULL;
        char* respBody = NULL; int respLen = 0;
        if (oauth_send(req, &resp)) respBody = oauth_response_body(resp, &respLen);
        vm_pop();  /* req */
        ObjInstance* tok = oauth_build_token(params, respBody ? respBody : "", respLen);
        free(respBody);
        return OBJ_VAL(tok);
    }
    if (grant != OWL_OAUTH_GRANT_CC) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthIdentifie", NULL);
        return NIL_VAL;
    }

    bool useBasic = true;
    char* body = oauth_build_body(params, "grant_type=client_credentials", NULL, &useBasic);
    ObjInstance* req = oauth_make_token_request(params, body, useBasic);
    free(body);
    if (req == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthIdentifie", NULL);
        return NIL_VAL;
    }
    vm_push(OBJ_VAL(req));
    ObjInstance* resp = NULL;
    char* respBody = NULL; int respLen = 0;
    if (oauth_send(req, &resp)) respBody = oauth_response_body(resp, &respLen);
    vm_pop();  /* req */

    ObjInstance* tok = oauth_build_token(params, respBody ? respBody : "", respLen);
    free(respBody);
    return OBJ_VAL(tok);
}

/* ----------------------------------------------------------------
 * Natives test-only __OAuth* (OWL_NAT_INTERNAL, cachées du catalogue).
 * Exposent les trois segments purs du flux code+PKCE pour des tests
 * déterministes (la partie live de AuthIdentifie n'est pas testable en
 * unit). Même rôle que la famille __Debug* côté UI.
 * ---------------------------------------------------------------- */

/* __OAuthConstruitURLEtPKCE(params) -> assoc {url, verifier, challenge, state}. */
static Value fn_oauth_construit_url_pkce(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "__OAuthConstruitURLEtPKCE", NULL);
        return NIL_VAL;
    }
    ObjInstance* params = AS_INSTANCE(args[0]);
    char *url = NULL, *verifier = NULL, *challenge = NULL, *state = NULL;
    if (!oauth_build_auth_url(params, &url, &verifier, &challenge, &state)) {
        free(url); free(verifier); free(challenge); free(state);
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "__OAuthConstruitURLEtPKCE",
                              "detail", "préparation de la demande d'autorisation impossible", NULL);
        return NIL_VAL;
    }
    ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
    vm_push(OBJ_VAL(a));
    oauth_assoc_set_str(a, "url", url);
    oauth_assoc_set_str(a, "verifier", verifier);
    oauth_assoc_set_str(a, "challenge", challenge);
    oauth_assoc_set_str(a, "state", state);
    vm_pop();  /* a */
    free(url); free(verifier); free(challenge); free(state);
    return OBJ_VAL(a);
}

/* __OAuthExtraitCode(urlRedirectionRecue, stateAttendu) -> code (chaîne). */
static Value fn_oauth_extrait_code(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "__OAuthExtraitCode", NULL);
        return NIL_VAL;
    }
    char* code = oauth_extract_code(AS_STRING(args[0])->chars, AS_STRING(args[1])->chars);
    if (code == NULL) {
        owl_emit_native_error(E_NATIVE_OAUTH_AUTH_CODE_FAILED, "__OAuthExtraitCode",
                              "detail", "redirection d'autorisation invalide", NULL);
        return NIL_VAL;
    }
    ObjString* s = obj_copy_string(code, (int)strlen(code));
    free(code);
    return OBJ_VAL(s);
}

/* __OAuthEchangeCode(params, code, verifier) -> AuthToken. */
static Value fn_oauth_echange_code(int argCount, Value* args) {
    if (argCount < 3 || !IS_INSTANCE(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "__OAuthEchangeCode", NULL);
        return NIL_VAL;
    }
    ObjInstance* params = AS_INSTANCE(args[0]);
    /* params + args restent rootés via la pile d'arguments de la native. */
    ObjInstance* tok = oauth_exchange_code(params, AS_STRING(args[1])->chars,
                                           AS_STRING(args[2])->chars);
    return OBJ_VAL(tok);
}

/* JWTCréeAssertion(entête, payload, cléPrivéePEM) -> chaîne. Signe en RS256 une
 * assertion depuis un entête et un payload JSON fournis BRUTS par l'appelant
 * (aucun claim implicite : résultat entièrement déterministe). L'algorithme est
 * lu dans le champ `alg` de l'entête : RS256 seul est honoré ; tout autre (dont
 * HS256) -> E_NATIVE_JWT_ALGORITHM_UNSUPPORTED. Clé PEM non exploitable -> erreur
 * nette (pas de crash). */
static Value fn_jwt_cree_assertion(int argCount, Value* args) {
    if (argCount < 3 || !IS_STRING(args[0]) || !IS_STRING(args[1]) || !IS_STRING(args[2])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "JWTCr\xc3\xa9""eAssertion", NULL);
        return NIL_VAL;
    }
    const char* header  = AS_STRING(args[0])->chars;
    int         headerLen = AS_STRING(args[0])->length;
    const char* payload = AS_STRING(args[1])->chars;
    int         payloadLen = AS_STRING(args[1])->length;
    const char* pem     = AS_STRING(args[2])->chars;

    char* alg = jwt_header_alg(header, headerLen);
    bool isRs256 = (alg != NULL && strcmp(alg, "RS256") == 0);
    free(alg);
    if (!isRs256) {
        owl_emit_native_error(E_NATIVE_JWT_ALGORITHM_UNSUPPORTED, "JWTCr\xc3\xa9""eAssertion", NULL);
        return NIL_VAL;
    }

    char* jwt = jwt_sign_rs256(header, headerLen, payload, payloadLen, pem);
    if (jwt == NULL) {
        owl_emit_native_error(E_NATIVE_OAUTH_PARAMS_INCOMPLETE, "JWTCr\xc3\xa9""eAssertion",
                              "detail", "signature de l'assertion impossible (cle privee PEM invalide)", NULL);
        return NIL_VAL;
    }
    ObjString* s = obj_copy_string(jwt, (int)strlen(jwt));
    free(jwt);
    return OBJ_VAL(s);
}

/* __JWTVerifieSignature(jwt, clePubliquePEM) -> booléen. Seam de test : preuve
 * cryptographique réelle que la signature RS256 du JWT valide contre la clé
 * publique PEM (mbedtls_pk_verify). Vrai/Faux uniquement (jamais d'erreur dure
 * sur un JWT malformé : Faux). */
static Value fn_jwt_verifie_signature(int argCount, Value* args) {
    if (argCount < 2 || !IS_STRING(args[0]) || !IS_STRING(args[1])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "__JWTVerifieSignature", NULL);
        return NIL_VAL;
    }
    const char* jwt = AS_STRING(args[0])->chars;
    int jwtLen = AS_STRING(args[0])->length;
    const char* pubPem = AS_STRING(args[1])->chars;

    /* Découpe `header.payload.signature` : signing input = les 2 premiers
     * segments (points inclus), signature = le 3e. */
    const char* dot1 = memchr(jwt, '.', (size_t)jwtLen);
    if (dot1 == NULL) return BOOL_VAL(false);
    const char* dot2 = memchr(dot1 + 1, '.', (size_t)(jwt + jwtLen - (dot1 + 1)));
    if (dot2 == NULL) return BOOL_VAL(false);
    int signingLen = (int)(dot2 - jwt);
    const char* sigB64 = dot2 + 1;
    int sigB64Len = (int)(jwt + jwtLen - sigB64);

    int sigLen = 0;
    unsigned char* sig = oauth_base64url_decode(sigB64, sigB64Len, &sigLen);
    if (sig == NULL) return BOOL_VAL(false);

    unsigned char digest[32];
    bool ok = (mbedtls_sha256((const unsigned char*)jwt, (size_t)signingLen, digest, 0) == 0);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    /* PEM : longueur incluant le '\0' terminal (exigence mbedtls). */
    if (ok) ok = (mbedtls_pk_parse_public_key(&pk, (const unsigned char*)pubPem,
                                              strlen(pubPem) + 1) == 0);
    if (ok) ok = (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, sizeof digest,
                                    sig, (size_t)sigLen) == 0);
    mbedtls_pk_free(&pk);
    free(sig);
    return BOOL_VAL(ok);
}

/* AuthRenouvelleToken(oToken) -> AuthToken. */
static Value fn_auth_renouvelle(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthRenouvelleToken", NULL);
        return NIL_VAL;
    }
    ObjInstance* old = AS_INSTANCE(args[0]);
    ObjInstance* params = oauth_get_instance(old, "_params");
    const char* rt = oauth_get_str(old, "Actualisation");

    if (rt[0] == '\0') {
        /* Sans jeton de rafraichissement : non renouvelable, aucune requete
         * reseau. On rend un jeton invalide (reponse vide). */
        return OBJ_VAL(oauth_build_token(params, "", 0));
    }

    bool useBasic = true;
    char* body = oauth_build_body(params, "grant_type=refresh_token", rt, &useBasic);
    ObjInstance* req = oauth_make_token_request(params, body, useBasic);
    free(body);
    if (req == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "AuthRenouvelleToken", NULL);
        return NIL_VAL;
    }
    vm_push(OBJ_VAL(req));
    ObjInstance* resp = NULL;
    char* respBody = NULL; int respLen = 0;
    if (oauth_send(req, &resp)) respBody = oauth_response_body(resp, &respLen);
    vm_pop();  /* req */

    ObjInstance* tok = oauth_build_token(params, respBody ? respBody : "", respLen);
    free(respBody);
    return OBJ_VAL(tok);
}

/* ================================================================
 * Rafraîchissement interne (appelé par le hook pré-envoi vm.c)
 * ================================================================ */

/* Rafraichit `tok` et retourne le NOUVEAU jeton d'acces (chaine malloc'd), ou
 * NULL si non rafraichissable / echec. NE mute PAS `tok` : la valeur produite
 * est injectee par requete a l'envoi, l'objet jeton reste inchange. Stratégie :
 * refresh_token present -> grant_type=refresh_token ; sinon params
 * client_credentials exploitables -> rejeu grant_type=client_credentials. */
static char* oauth_refresh_access(ObjInstance* tok) {
    ObjInstance* params = oauth_get_instance(tok, "_params");
    const char* rt = oauth_get_str(tok, "Actualisation");

    char* body = NULL;
    bool useBasic = true;
    if (rt[0] != '\0') {
        body = oauth_build_body(params, "grant_type=refresh_token", rt, &useBasic);
    } else if (oauth_params_cc_exploitable(params)) {
        body = oauth_build_body(params, "grant_type=client_credentials", NULL, &useBasic);
    } else {
        return NULL;  /* non rafraichissable */
    }

    ObjInstance* req = oauth_make_token_request(params, body, useBasic);
    free(body);
    if (req == NULL) return NULL;
    vm_push(OBJ_VAL(req));
    ObjInstance* resp = NULL;
    char* respBody = NULL; int respLen = 0;
    bool sent = oauth_send(req, &resp);
    if (sent) respBody = oauth_response_body(resp, &respLen);
    vm_pop();  /* req */
    if (!sent || respBody == NULL) { free(respBody); return NULL; }

    char* access = NULL; char* refresh = NULL; int64_t expiresIn = 0;
    oauth_parse_token_json(respBody, respLen, &access, &refresh, &expiresIn);
    free(respBody);
    free(refresh);
    return access;  /* NULL si pas d'access_token dans la reponse */
}

/* Hook pré-envoi posé sur HTTPEnvoie (appelé par vm.c AVANT le mock).
 * Retour : 0 = continuer l'envoi (en-tete injecté si un jeton est présent) ;
 * <0 = empecher l'envoi (jeton mort non rafraichissable, erreur soft émise). */
static int oauth_http_presend(int argCount, Value* args) {
    if (argCount < 1 || !IS_INSTANCE(args[0])) return 0;
    ObjInstance* req = AS_INSTANCE(args[0]);

    Value tv;
    if (!oauth_field(req, "AuthToken", &tv) || !IS_INSTANCE(tv)) return 0;
    ObjInstance* tok = AS_INSTANCE(tv);

    bool autoRefresh = oauth_get_bool(req, "RafraichitTokenAuto", true);
    bool valid = oauth_token_is_valid(tok);

    char* effective = NULL;  /* malloc'd */
    if (valid || !autoRefresh) {
        effective = oauth_strdup(oauth_get_str(tok, "Valeur"));
    } else {
        effective = oauth_refresh_access(tok);
        if (effective == NULL) {
            /* Jeton expire non rafraichissable : pas d'envoi silencieux. */
            owl_emit_native_error(E_NATIVE_OAUTH_TOKEN_NOT_REFRESHABLE, "HTTPEnvoie", NULL);
            return -1;
        }
    }

    /* Injecte Authorization: Bearer <valeur effective> dans l'Entête. L'objet
     * jeton n'est pas muté : la valeur est recalculée par envoi. */
    ObjString* ek = obj_copy_string("Entete", 6);
    vm_push(OBJ_VAL(ek));
    Value ev;
    if (table_get(&req->fields, ek, &ev) && IS_ASSOC_ARRAY(ev)) {
        size_t n = strlen(effective) + 8;
        char* hdr = (char*)malloc(n);
        if (hdr != NULL) {
            snprintf(hdr, n, "Bearer %s", effective);
            oauth_header_set(AS_ASSOC_ARRAY(ev), "Authorization", hdr);
            free(hdr);
        }
    }
    vm_pop();  /* ek */
    free(effective);
    return 0;
}

/* ================================================================
 * Templates runtime + enregistrement
 * ================================================================ */

static ObjInstance* build_oauth2_parametres_template(void) {
    ObjString* nm = obj_copy_string("OAuth2Parametres", 16);
    vm_push(OBJ_VAL(nm));
    ObjInstance* tpl = obj_new_instance(nm);
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "ClientID", "");
    inst_field_set_str(tpl, "ClientSecret", "");
    /* ParamètresSupplémentaires : assoc vide (chaine->chaine). */
    {
        ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
        vm_push(OBJ_VAL(a));
        inst_field_set_value(tpl, "ParametresSupplementaires", OBJ_VAL(a));
        vm_pop();
    }
    inst_field_set_str(tpl, "Scope", "");
    inst_field_set_int(tpl, "TypeAutorisation", OWL_OAUTH_GRANT_CODE);  /* defaut canon */
    inst_field_set_int(tpl, "TypeReponse", OWL_OAUTH_RESP_CODE);
    inst_field_set_str(tpl, "URLAuth", "");
    inst_field_set_str(tpl, "URLRedirection", "");
    inst_field_set_str(tpl, "URLToken", "");
    inst_field_set_int(tpl, "MethodeAuthClient", OWL_OAUTH_AUTHM_POST);
    vm_pop();  /* tpl */
    vm_pop();  /* nm */
    return tpl;
}

static ObjInstance* build_jwt_parametres_template(void) {
    ObjString* nm = obj_copy_string("JWTParametres", 13);
    vm_push(OBJ_VAL(nm));
    ObjInstance* tpl = obj_new_instance(nm);
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Certificat", "");
    inst_field_set_str(tpl, "ClePrivee", "");
    inst_field_set_str(tpl, "Consommateur", "");
    inst_field_set_str(tpl, "Emetteur", "");
    inst_field_set_str(tpl, "IDClePrivee", "");
    {
        ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
        vm_push(OBJ_VAL(a));
        inst_field_set_value(tpl, "ParametresSupplementaires", OBJ_VAL(a));
        vm_pop();
    }
    {
        ObjAssocArray* a = obj_new_assoc(ASSOC_KEY_STRING, 0, ASSOC_VALUE_CHAINE);
        vm_push(OBJ_VAL(a));
        inst_field_set_value(tpl, "ParametresSupplementairesURL", OBJ_VAL(a));
        vm_pop();
    }
    inst_field_set_str(tpl, "Scope", "");
    inst_field_set_str(tpl, "Sujet", "");
    inst_field_set_str(tpl, "URLToken", "");
    inst_field_set_int(tpl, "Cryptage", OWL_JWT_CRYPT_RS256);  /* defaut : seul algo honoré */
    vm_pop();  /* tpl */
    vm_pop();  /* nm */
    return tpl;
}

static ObjInstance* build_authtoken_template(void) {
    ObjString* nm = obj_copy_string("AuthToken", 9);
    vm_push(OBJ_VAL(nm));
    ObjInstance* tpl = obj_new_instance(nm);
    vm_push(OBJ_VAL(tpl));
    inst_field_set_str(tpl, "Valeur", "");
    inst_field_set_str(tpl, "Actualisation", "");
    {
        ObjBuffer* buf = obj_buffer_empty(0);
        vm_push(OBJ_VAL(buf));
        inst_field_set_value(tpl, "ReponseServeur", OBJ_VAL(buf));
        vm_pop();
    }
    inst_field_set_int(tpl, "_exp", 0);
    inst_field_set_value(tpl, "DateExpiration", NIL_VAL);
    inst_field_set_value(tpl, "_params", NIL_VAL);
    vm_pop();  /* tpl */
    vm_pop();  /* nm */
    return tpl;
}

void stdlib_oauth_init(void) {
    static const OwlParam p_authtoken[] = {
        {"parametres", "OAuth2Param\xc3\xa8tres", "parametres d'authentification d'origine"},
        {"reponse",    "cha\xc3\xae""ne",          "reponse JSON du serveur d'autorisation"},
        {0}
    };
    static const OwlSignature sig_authtoken[] = {
        {2, p_authtoken, "AuthToken", "jeton construit depuis la reponse"},
        {0}
    };

    static const OwlParam p_identifie[] = {
        {"parametres", "OAuth2Param\xc3\xa8tres", "parametres d'authentification OAuth 2.0"},
        {0}
    };
    static const OwlSignature sig_identifie[] = {
        {1, p_identifie, "AuthToken", "jeton d'authentification obtenu"},
        {0}
    };

    static const OwlParam p_renouvelle[] = {
        {"jeton", "AuthToken", "jeton a renouveler"},
        {0}
    };
    static const OwlSignature sig_renouvelle[] = {
        {1, p_renouvelle, "AuthToken", "jeton renouvele"},
        {0}
    };

    /* Natives test-only (OWL_NAT_INTERNAL) : segments purs du flux code+PKCE. */
    static const OwlParam p_oauth_url_pkce[] = {
        {"parametres", "OAuth2Param\xc3\xa8tres", "parametres d'authentification OAuth 2.0"},
        {0}
    };
    static const OwlSignature sig_oauth_url_pkce[] = {
        {1, p_oauth_url_pkce, "tableau associatif de cha\xc3\xae""nes",
         "cles url / verifier / challenge / state"},
        {0}
    };
    static const OwlParam p_oauth_extrait[] = {
        {"urlRedirectionRecue", "cha\xc3\xae""ne", "URL de redirection recue du fournisseur"},
        {"stateAttendu",        "cha\xc3\xae""ne", "valeur de state attendue (anti-CSRF)"},
        {0}
    };
    static const OwlSignature sig_oauth_extrait[] = {
        {2, p_oauth_extrait, "cha\xc3\xae""ne", "code d'autorisation extrait"},
        {0}
    };
    static const OwlParam p_oauth_echange[] = {
        {"parametres", "OAuth2Param\xc3\xa8tres", "parametres d'authentification OAuth 2.0"},
        {"code",       "cha\xc3\xae""ne",          "code d'autorisation a echanger"},
        {"verifier",   "cha\xc3\xae""ne",          "code_verifier PKCE correspondant"},
        {0}
    };
    static const OwlSignature sig_oauth_echange[] = {
        {3, p_oauth_echange, "AuthToken", "jeton obtenu par echange du code"},
        {0}
    };
    static const OwlParam p_jwt_cree[] = {
        {"entete",    "cha\xc3\xae""ne", "entete JSON de l'assertion (doit declarer alg RS256)"},
        {"payload",   "cha\xc3\xae""ne", "payload JSON (revendications) de l'assertion"},
        {"clePrivee", "cha\xc3\xae""ne", "cle privee de signature au format PEM"},
        {0}
    };
    static const OwlSignature sig_jwt_cree[] = {
        {3, p_jwt_cree, "cha\xc3\xae""ne", "assertion JWT signee (entete.payload.signature)"},
        {0}
    };

    static const OwlParam p_jwt_verif[] = {
        {"jwt",            "cha\xc3\xae""ne", "assertion JWT a verifier"},
        {"clePubliquePEM", "cha\xc3\xae""ne", "cle publique PEM de verification"},
        {0}
    };
    static const OwlSignature sig_jwt_verif[] = {
        {2, p_jwt_verif, "bool\xc3\xa9""en", "Vrai si la signature RS256 est valide"},
        {0}
    };

    static const OwlNativeDef DEFS[] = {
        { .canonical = "AuthToken", .nameEN = "AuthToken", .category = "OAuth2",
          .descFR = "Construit un jeton d'authentification a partir des parametres OAuth 2.0 et de la reponse JSON du serveur (jeton d'acces, expiration, jeton de rafraichissement). Les parametres sont retenus pour un rafraichissement ulterieur.",
          .descEN = "Builds an authentication token from the OAuth 2.0 parameters and the server JSON response (access token, expiration, refresh token). Parameters are retained for later refresh.",
          .signatures = sig_authtoken, .impl = fn_auth_token },
        { .canonical = "AuthIdentifie", .nameEN = "AuthIdentify", .category = "OAuth2",
          .descFR = "Obtient un jeton d'authentification aupres d'un fournisseur OAuth 2.0. Selon le flux des parametres : par code d'autorisation (ouvre le navigateur, l'utilisateur se connecte, capture de la redirection, protection PKCE et anti-CSRF) ou machine-a-machine (identifiants client, sans fenetre de login). La reponse directe par jeton n'est pas prise en charge.",
          .descEN = "Obtains an authentication token from an OAuth 2.0 provider. Depending on the parameters' flow: authorization code (opens the browser, the user logs in, redirection is captured, PKCE and anti-CSRF protection) or machine-to-machine (client credentials, no login window). The direct token response is not supported.",
          .signatures = sig_identifie, .impl = fn_auth_identifie },
        { .canonical = "AuthRenouvelleToken", .nameEN = "AuthRefreshToken", .category = "OAuth2",
          .descFR = "Renouvelle un jeton d'authentification via son jeton de rafraichissement, aupres du fournisseur ayant emis le jeton d'origine. Sans jeton de rafraichissement, le resultat est marque invalide.",
          .descEN = "Refreshes an authentication token using its refresh token, from the provider that issued the original token. Without a refresh token, the result is marked invalid.",
          .signatures = sig_renouvelle, .impl = fn_auth_renouvelle },
        { .canonical = "JWTCr\xc3\xa9""eAssertion", .canonicalAscii = "JWTCreeAssertion",
          .nameEN = "JWTCreateAssertion", .category = "OAuth2",
          .descFR = "Signe une assertion JWT a partir d'un entete et d'un payload fournis bruts (JSON), avec une cle privee RSA au format PEM (algorithme RS256). Aucune revendication n'est ajoutee automatiquement : le resultat est entierement determine par les entrees.",
          .descEN = "Signs a JWT assertion from a header and payload provided as raw JSON, using an RSA private key in PEM format (RS256 algorithm). No claim is added automatically: the result is fully determined by the inputs.",
          .signatures = sig_jwt_cree, .impl = fn_jwt_cree_assertion },
        { .canonical = "__OAuthConstruitURLEtPKCE", .canonicalAscii = "__OAuthConstruitURLEtPKCE",
          .nameEN = "__OAuthBuildURLAndPKCE", .category = "Test", .flags = OWL_NAT_INTERNAL,
          .descFR = "Construit l'URL d'autorisation et les parametres PKCE/state (API interne de test).",
          .descEN = "Builds the authorization URL and the PKCE/state parameters (internal test API).",
          .signatures = sig_oauth_url_pkce, .impl = fn_oauth_construit_url_pkce },
        { .canonical = "__OAuthExtraitCode", .canonicalAscii = "__OAuthExtraitCode",
          .nameEN = "__OAuthExtractCode", .category = "Test", .flags = OWL_NAT_INTERNAL,
          .descFR = "Extrait le code d'autorisation d'une URL de redirection apres controle du state (API interne de test).",
          .descEN = "Extracts the authorization code from a redirection URL after state verification (internal test API).",
          .signatures = sig_oauth_extrait, .impl = fn_oauth_extrait_code },
        { .canonical = "__OAuthEchangeCode", .canonicalAscii = "__OAuthEchangeCode",
          .nameEN = "__OAuthExchangeCode", .category = "Test", .flags = OWL_NAT_INTERNAL,
          .descFR = "Echange un code d'autorisation contre un jeton via le point de jeton (API interne de test).",
          .descEN = "Exchanges an authorization code for a token via the token endpoint (internal test API).",
          .signatures = sig_oauth_echange, .impl = fn_oauth_echange_code },
        { .canonical = "__JWTVerifieSignature", .canonicalAscii = "__JWTVerifieSignature",
          .nameEN = "__JWTVerifySignature", .category = "Test", .flags = OWL_NAT_INTERNAL,
          .descFR = "Verifie la signature RS256 d'une assertion JWT contre une cle publique PEM (API interne de test).",
          .descEN = "Verifies the RS256 signature of a JWT assertion against a PEM public key (internal test API).",
          .signatures = sig_jwt_verif, .impl = fn_jwt_verifie_signature },
    };

    owl_registry_register_module(DEFS, (int)(sizeof(DEFS) / sizeof(DEFS[0])));
    owl_registry_register_enums(OAUTH_ENUMS, (int)(sizeof(OAUTH_ENUMS) / sizeof(OAUTH_ENUMS[0])));
    owl_registry_register_consts(OAUTH_CONSTS, (int)(sizeof(OAUTH_CONSTS) / sizeof(OAUTH_CONSTS[0])));
    owl_registry_register_types(OAUTH_TYPES, (int)(sizeof(OAUTH_TYPES) / sizeof(OAUTH_TYPES[0])));

    /* Templates runtime (declaration `x est un T`). Dans vm.types : la native
     * homonyme AuthToken reste seule dans vm.globals (constructeur). */
    ObjInstance* tplP = build_oauth2_parametres_template();
    vm_push(OBJ_VAL(tplP));
    vm_type_register("OAuth2Parametres", OBJ_VAL(tplP));
    vm_type_register("OAuth2Param\xc3\xa8tres", OBJ_VAL(tplP));
    vm_type_register("OAuth2Parameters", OBJ_VAL(tplP));
    vm_pop();

    ObjInstance* tplT = build_authtoken_template();
    vm_push(OBJ_VAL(tplT));
    vm_type_register("AuthToken", OBJ_VAL(tplT));
    vm_pop();

    ObjInstance* tplJ = build_jwt_parametres_template();
    vm_push(OBJ_VAL(tplJ));
    vm_type_register("JWTParametres", OBJ_VAL(tplJ));
    vm_type_register("JWTParam\xc3\xa8tres", OBJ_VAL(tplJ));
    vm_type_register("JWTParameters", OBJ_VAL(tplJ));
    vm_pop();

    /* Pose le hook pré-envoi HTTP (injection Bearer + refresh auto M1). */
    const OwlNativeDef* send = owl_registry_find("HTTPEnvoie");
    if (send != NULL && send->impl != NULL) {
        vm_set_http_presend_hook(send->impl, oauth_http_presend);
    }
}
