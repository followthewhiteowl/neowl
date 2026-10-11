// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_SERVICE_DECL_H
#define OWL_SERVICE_DECL_H

/* Structures collectees a la compilation pour les blocs declaratifs
 * `<Nom> est un ServiceREST` et `<Op> est une OperationHTTP` (socle du DSL
 * de service HTTP). Chantier 1 : parse + validation statique uniquement —
 * ces structures sont peuplees par le compilateur et exposees pour les
 * chantiers aval (routage, invocation, OpenAPI). Aucune implication runtime
 * ni GC : allocation C simple, duree de vie = la compilation courante,
 * liberee par owl_service_registry_reset() (appele par
 * compiler_reset_poo_state entre deux compilations). */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* name;        /* nom du parametre */
    char* typeName;    /* type declare, ou NULL si absent */
    int   isStructured;/* 1 = type structure (candidat corps), 0 = scalaire */
    int   isFileUpload;/* 1 = type FichierTeleverse/UploadedFile (source multipart) */
    int   hasDefault;  /* 1 = valeur par defaut (query optionnel) */
} OwlServiceProcParam;

typedef struct {
    char* name;              /* nom de l'operation */
    int   line;
    /* Proprietes (valeurs canon telles qu'ecrites, NULL si absente). */
    char* method;            /* valeur httpMethodes (ex "httpGet") */
    char* codeEtat;          /* valeur httpCodeEtat (ex "httpOK") */
    char** pathSegments;     /* segments litteraux + params de Chemin */
    /* Nature de chaque segment (tableau parallele a pathSegments) :
     *   0 = litteral, 1 = parametre nomme, 2 = parametre catch-all glouton
     * ( : `*<nom>`, capture 1+ segments finaux). Un catch-all reste
     * un parametre (toute lecture "parametre ?" teste != 0) ; seul l'analyseur
     * de routes et le peuplement runtime distinguent la valeur 2. */
    unsigned char* pathSegmentIsParam;
    int    pathSegmentCount;
    char** queryParams;      /* noms de params declares dans Requete (query) */
    int    queryParamCount;
    int    pathHadError;     /* 1 = un segment de Chemin invalide => pas de
                              * cascade de liaison (chemin non fiable) */
    char** responses;        /* valeurs httpCodeEtat de Reponses */
    int    responseCount;
    /* Signature de l'evenement surRequete (capturee, pas invoquee). */
    int    hasProc;
    char*  procName;
    OwlServiceProcParam* procParams;
    int    procParamCount;
    char*  procReturnType;   /* type de retour, ou NULL */
    /* Nom global synthetise sous lequel le corps de surRequete a ete compile
     * (fondation runtime, ). NULL si le handler n'a pas ete compile
     * (bloc jetable / doublon). */
    char*  handlerGlobal;
    /*  — opt-out d'authentification par operation : 1 si l'operation
     * declare `Authentification = httpAuthAucune` (le hook surAuthentification
     * du service n'est pas invoque pour ses requetes). */
    int    authOptOut;
    /*  — 1 si l'operation declare `ContentType =
     * typeMimeFormulaireMultiPartie` (contrat multipart form-data). Doit
     * coexister avec au moins un parametre FichierTeleverse, jamais avec un
     * corps de requete structure. */
    int    contentTypeMultipart;
    /*  — 1 si l'operation declare `ExclureDeOpenAPI = Vrai` : elle est
     * servie normalement par le routeur mais OMISE du document OpenAPI genere
     * (page/bundle de doc, health-check interne, webhook prive...). Defaut 0
     * (l'operation est documentee). Ne touche jamais le routage runtime. */
    int    excludeOpenAPI;
    /*  — surface DSL statique du streaming (P1). NULL = reponse normale
     * bufferisee ; sinon valeur canon de httpFlux ("fluxSSE" = framing SSE
     * `data:`, "fluxOctets" = octets bruts). En P1 la propriete est parsee,
     * validee et portee jusqu'a la route runtime, mais n'active AUCUN
     * comportement de flux a l'execution (3e mode + FluxEcrit = P2/P3). */
    char*  flux;
    /*  — borne applicative de duree d'un flux, en secondes. 0 =
     * illimite (defaut). Portee jusqu'a la route runtime ; le timer reacteur
     * qui l'applique est P5. */
    int    delaiMax;
} OwlOperationDecl;

typedef struct {
    char* name;
    int   line;
    char** urlBase;          /* segments litteraux de UrlBase */
    int    urlBaseCount;
    char*  authentication;   /* valeur httpAuthentification, ou NULL */
    /*  — hook surAuthentification (niveau service) : nom global
     * synthetise sous lequel son corps est compile, NULL si absent. */
    char*  authHandlerGlobal;
    /*  — hook surErreur (niveau service) : nom global synthetise du
     * corps compile (avec contexte MoiMême), NULL si absent. Les hooks de cycle
     * de vie FinInitialisation/Fermeture sont compiles sous leur nom LITTERAL
     * (resolus par vm_call_global depuis l'executeur, sans contexte) ; on ne
     * memorise ici qu'un temoin de presence pour eviter une double compilation. */
    char*  errorHandlerGlobal;
    /*  — hook surAutorisation (niveau service) : nom global synthetise
     * du corps compile (avec contexte MoiMême, 0 argument, RENVOYER booleen),
     * NULL si absent. Tourne APRES l'authn, AVANT le binding. */
    char*  authzHandlerGlobal;
    /*  — schema JWT declaratif : cle publique RS256 (PEM) et emetteur
     * attendu, configures en proprietes (chaines libres). Le framework valide
     * la signature/exp/iss et depose l'identite -- zero code app. NULL si le
     * service n'utilise pas le schema JWT. */
    char*  jwtPublicKeyPem;
    char*  jwtIssuer;
    /*  — URL d'un jeu de cles JWKS (source de cle dynamique). Exclusive
     * de jwtPublicKeyPem (verifie a la compilation). NULL = pas de mode JWKS. Le
     * framework recupere les cles a la demande, les met en cache et selectionne
     * par kid. */
    char*  jwtJwksUrl;
    /*  — audience attendue (claim aud, RFC 7519 4.1.3). Opt-in : si non
     * NULL, le framework exige un claim aud correspondant (chaine == audience,
     * ou audience presente dans le tableau aud). NULL = aud ignore. */
    char*  jwtAudience;
    int    initHookSeen;    /* 1 = FinInitialisation/EndInitialization deja compile */
    int    closeHookSeen;   /* 1 = Fermeture/Closing deja compile */
    /* Position + nature du schema d'auth, pour le warning DIFFERE : emis en fin
     * de parse du service uniquement si le schema est protecteur (Bearer /
     * Basique / CleAPI) ET qu'aucun surAuthentification n'est fourni. Le pointeur
     * vise la source, vivante pendant toute la compilation. */
    const char* authSchemeStart;
    int    authSchemeLen;
    int    authSchemeLine;
    int    authSchemeCol;
    int    authSchemeProtective; /* 1 = Bearer/Basique/CleAPI */
    OwlOperationDecl** operations;
    int    operationCount;
    int    operationCapacity;
} OwlServiceDecl;

/* Cree une decl service (nom copie). A liberer via le reset global. */
OwlServiceDecl* owl_service_decl_new(const char* name, int line);
/* Ajoute une operation (nom copie) a un service et la retourne. */
OwlOperationDecl* owl_service_decl_add_operation(OwlServiceDecl* svc,
                                                 const char* name, int line);

/* Registre a l'echelle de la compilation courante. */
void            owl_service_registry_reset(void);
int             owl_service_registry_count(void);
OwlServiceDecl* owl_service_registry_at(int index);
/* Enregistre un service. NULL si le nom est deja pris (doublon). */
OwlServiceDecl* owl_service_registry_add(const char* name, int line);
/* Recherche casse/accent-insensible par nom canon deja strippe (ASCII). */
OwlServiceDecl* owl_service_registry_find_ascii(const char* asciiName);

#ifdef __cplusplus
}
#endif

#endif /* OWL_SERVICE_DECL_H */
