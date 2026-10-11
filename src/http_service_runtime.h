// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_HTTP_SERVICE_RUNTIME_H
#define OWL_HTTP_SERVICE_RUNTIME_H

/* Registre runtime des routes d'un ServiceREST (fondation , binding
 * ).
 *
 * Distinct du registre de compilation (service_decl.h) : celui-la est
 * COMPILE-TIME et libere par owl_service_registry_reset() apres compilation.
 * Ce registre-ci est peuple au fil de la compilation (une route par
 * OperationHTTP dont le handler surRequete a ete compile en fonction globale)
 * et GARDE VIVANT au-dela du reset : l'executeur HTTP le consulte a l'execution
 * pour resoudre une requete vers son handler ET lier les parametres.
 *
 * Il est reset au DEBUT de chaque compilation (a cote de
 * owl_service_registry_reset), donc une compile fraiche reconstruit ses routes.
 *
 *  — le registre porte desormais TOUTE la metadonnee serve-time :
 * segments (litteral vs parametre) pour le routage par specificite + la
 * redirection 308 sur casse ASCII, et le contrat de liaison des parametres du
 * handler (nom / source chemin|query|corps / type / optionnel / index de
 * segment). Le serve-time ne lit QUE ce registre durable, jamais le registre
 * compile-time (libere apres compilation). */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Source d'ou provient la valeur d'un parametre du handler. */
typedef enum {
    OWL_SVC_SRC_PATH  = 0, /* segment de l'URL (index dans les segments de route) */
    OWL_SVC_SRC_QUERY = 1, /* parametre de query string */
    OWL_SVC_SRC_BODY  = 2  /* corps de la requete, deserialise vers un type structure */
} OwlServiceParamSource;

/* Un parametre du handler surRequete, capture DANS L'ORDRE DE DECLARATION
 * (l'arite et l'ordre des arguments passes a la VM en dependent). */
/*  — nature d'un parametre FichierTeleverse (source multipart). */
typedef enum {
    OWL_SVC_UPLOAD_NONE   = 0, /* pas un fichier televerse */
    OWL_SVC_UPLOAD_SCALAR = 1, /* `X est un FichierTeleverse` : une instance */
    OWL_SVC_UPLOAD_ARRAY  = 2  /* `X est un tableau de FichierTeleverse` */
} OwlServiceUploadKind;

typedef struct {
    char* name;         /* nom du parametre (forme ASCII canon) */
    int   source;       /* OwlServiceParamSource */
    char* typeName;     /* type declare, ou NULL (coercion scalaire / corps) */
    int   isStructured; /* 1 = type structure (candidat corps) */
    int   hasDefault;   /* 1 = valeur par defaut (query optionnel) */
    int   pathIndex;    /* si source==PATH : index du segment ; sinon -1 */
    /*  — OwlServiceUploadKind. Non nul => la valeur du parametre est
     * construite au serve-time depuis les parties d'un corps multipart lie par
     * nom (scalaire = 1 instance, tableau = toutes les parties de meme nom). */
    int   fileUpload;
} OwlServiceRuntimeParam;

typedef struct {
    char* verb;          /* verbe HTTP majuscule ("GET", "POST"...) */
    char* path;          /* chemin canonique complet, ex "/clients/actifs" */
    char* handlerGlobal; /* nom global synthetise a passer a vm_call_reentrant */
    int   hasParam;      /* 1 = le chemin porte au moins un parametre */

    /* Segments du chemin (paralleles), pour le routage parametre + 308. Un
     * segment parametre porte le nom du parametre comme libelle. */
    char**         segments;
    /* Nature de chaque segment : 0 = litteral, 1 = parametre nomme, 2 =
     * parametre catch-all glouton ( : dernier segment, capture 1+
     * segments finaux). Toute lecture "parametre ?" teste != 0 ; seul le
     * matching longueur-variable et le binding distinguent la valeur 2. */
    unsigned char* segIsParam;
    int            segCount;

    /* Contrat de liaison du handler, dans l'ordre de declaration. */
    OwlServiceRuntimeParam* params;
    int                     paramCount;

    /*  — authentification. Le schema et le hook sont portes au NIVEAU
     * SERVICE (identiques pour toutes les routes d'un meme service), recopies
     * sur chaque route pour que l'executeur les resolve depuis la seule route
     * appariee. authScheme / authHandlerGlobal NULL = pas d'authentification.
     * authOptOut = 1 si l'operation opte hors auth (`httpAuthAucune`). */
    char* authScheme;          /* valeur httpAuthentification, ou NULL */
    char* authHandlerGlobal;   /* nom global synthetise du hook, ou NULL */
    int   authOptOut;          /* 1 = operation exemptee d'authentification */

    /*  — hook surErreur (niveau service, recopie sur chaque route) :
     * nom global synthetise, invoque sur le 500 de CRASH DU HANDLER. NULL =
     * aucun hook -> 500 nu. */
    char* errorHandlerGlobal;

    /*  — hook surAutorisation (niveau service, recopie sur chaque
     * route) : nom global synthetise, invoque APRES l'authn et AVANT le
     * binding. NULL = aucune verification d'autorisation. */
    char* authzHandlerGlobal;

    /*  — schema JWT declaratif (niveau service, recopie sur chaque
     * route). authScheme == "httpAuthJWT" ET jwtPublicKeyPem non NULL ->
     * l'executeur valide lui-meme le jeton Bearer (signature RS256, exp, iss)
     * et depose les claims dans MoiMême..Requête..Identité, sans hook. NULL =
     * pas de validation JWT declarative. */
    char* jwtPublicKeyPem;   /* cle publique RS256 (PEM), ou NULL */
    /*  — URL JWKS (source de cle dynamique). Exclusive de
     * jwtPublicKeyPem. authScheme == "httpAuthJWT" ET (jwtPublicKeyPem OU
     * jwtJwksUrl) non NULL -> l'executeur valide le jeton. NULL = pas de JWKS. */
    char* jwtJwksUrl;
    char* jwtIssuer;         /* emetteur attendu (claim iss), ou NULL */
    /*  — audience attendue (claim aud, RFC 7519 4.1.3). NULL = aud
     * ignore ; non-NULL = le claim aud doit correspondre (chaine == audience,
     * ou audience presente dans le tableau aud), sinon 401. */
    char* jwtAudience;       /* audience attendue (claim aud), ou NULL */

    /*  — statut de reponse par defaut du chemin succes, resolu du
     * CodeÉtat declare de l'operation en son entier HTTP ("httpAccepté" -> 202).
     * 0 = aucun CodeÉtat declare -> l'executeur retombe sur 200. L'override par
     * le handler (MoiMême..Réponse..CodeÉtat) reste prioritaire. */
    int   defaultStatus;

    /*  — surface DSL statique du streaming (P1). flux NULL = reponse
     * normale bufferisee ; sinon valeur canon de httpFlux ("fluxSSE" = framing
     * SSE `data:`, "fluxOctets" = octets bruts). delaiMax = borne applicative en
     * secondes (0 = illimite). La route PORTE ces champs mais ne les EXECUTE pas
     * encore : le 3e mode d'execution streaming et le timer de duree sont P3/P5. */
    char* flux;
    int   delaiMax;
} OwlServiceRuntimeRoute;

/* Vide le registre. Appele au debut de chaque compilation. */
void owl_service_runtime_reset(void);

/* Ajoute une route (chaines copiees). Retourne l'index de la route (>= 0) ou
 * -1 en cas d'echec. L'index reste stable pour cette compilation ; utiliser
 * les setters ci-dessous pour completer segments et parametres. */
int owl_service_runtime_add(const char* verb, const char* path,
                            const char* handlerGlobal, int hasParam);

/* Ajoute un segment a une route (dans l'ordre du chemin). */
void owl_service_runtime_add_segment(int routeIdx, const char* segment,
                                     int isParam);

/* Ajoute un parametre de liaison a une route (ordre de declaration).
 *  — fileUpload = OwlServiceUploadKind (0 hors multipart). */
void owl_service_runtime_add_param(int routeIdx, const char* name, int source,
                                   const char* typeName, int isStructured,
                                   int hasDefault, int pathIndex, int fileUpload);

/*  — fixe le contexte d'authentification d'une route : schema du
 * service (NULL = aucun), nom global du hook surAuthentification (NULL =
 * aucun), et opt-out de l'operation (1 = exemptee). Chaines copiees. */
void owl_service_runtime_set_auth(int routeIdx, const char* scheme,
                                  const char* handlerGlobal, int optOut);

/*  — fixe le hook surErreur (niveau service) d'une route : nom global
 * synthetise (NULL = aucun). Chaine copiee. */
void owl_service_runtime_set_error_hook(int routeIdx, const char* handlerGlobal);

/*  — fixe le hook surAutorisation (niveau service) d'une route : nom
 * global synthetise (NULL = aucun). Chaine copiee. */
void owl_service_runtime_set_authz_hook(int routeIdx, const char* handlerGlobal);

/*  — fixe le contexte JWT declaratif (niveau service) d'une route :
 * cle publique RS256 (PEM) et emetteur attendu (NULL = aucun). Chaines copiees.
 * L'executeur ne valide le jeton que si le schema est httpAuthJWT ET que la
 * cle publique est presente. */
void owl_service_runtime_set_jwt(int routeIdx, const char* publicKeyPem,
                                 const char* jwksUrl,
                                 const char* issuer, const char* audience);

/*  — fixe le statut de reponse par defaut d'une route (CodeÉtat
 * declare resolu en entier HTTP). 0 = aucun -> l'executeur retombe sur 200. */
void owl_service_runtime_set_default_status(int routeIdx, int status);

/*  — fixe le mode de flux (P1) d'une route : valeur canon de httpFlux
 * (NULL = reponse normale bufferisee) et borne DelaiMax en secondes (0 =
 * illimite). Chaine copiee. La route porte l'information ; l'executeur ne
 * streame pas encore (P3). */
void owl_service_runtime_set_flux(int routeIdx, const char* flux, int delaiMax);

int                            owl_service_runtime_count(void);
const OwlServiceRuntimeRoute*  owl_service_runtime_at(int index);

/* Apparie une requete LITTERALE exacte (verbe insensible a la casse, chemin
 * exact, query ignoree). Les routes a parametre sont ecartees. NULL si aucune.
 * Conserve pour le socle deterministe : la resolution riche est ci-dessous. */
const OwlServiceRuntimeRoute* owl_service_runtime_find(const char* verb,
                                                       const char* path);

/* Resultat d'une resolution riche (litteral + parametre + specificite + 308). */
typedef enum {
    OWL_SVC_RESOLVE_NONE     = 0, /* aucune route -> 404 */
    OWL_SVC_RESOLVE_MATCH    = 1, /* route trouvee (route + valeurs de chemin) */
    OWL_SVC_RESOLVE_REDIRECT = 2  /* casse divergente ASCII -> 308 vers canonical */
} OwlServiceResolveKind;

#define OWL_SVC_MAX_SEG 32

typedef struct {
    OwlServiceResolveKind         kind;
    const OwlServiceRuntimeRoute* route;              /* MATCH */
    char canonical[512];                              /* REDIRECT : chemin canon */
    char pathValues[OWL_SVC_MAX_SEG][256];            /* valeur capturee par segment */
} OwlServiceResolveResult;

/* Resout une requete (verbe + cible brute, query toleree). Regles :
 *  - appariement EXACT casse-sensible d'abord, specificite litteral > parametre ;
 *  - a defaut, appariement insensible a la casse ASCII PURE (aucun fold
 *    hors-ASCII) -> 308 vers la forme canonique enregistree ;
 *  - sinon NONE. Les valeurs des segments parametres sont extraites dans
 *    out->pathValues[indexSegment]. */
void owl_service_runtime_resolve(const char* verb, const char* target,
                                 OwlServiceResolveResult* out);

#ifdef __cplusplus
}
#endif

#endif /* OWL_HTTP_SERVICE_RUNTIME_H */
