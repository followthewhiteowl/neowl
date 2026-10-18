// SPDX-License-Identifier: AGPL-3.0-only
/* http_dsl_service_test.c — fondation runtime du DSL de service.
 *
 * Prouve, bout en bout, le cablage :
 *   compilation de surRequete  ->  registre de routes runtime  ->  invocation
 *   du handler  ->  reponse servie, dont le statut vient de MoiMême..Réponse..CodeÉtat.
 *
 * Cinq familles :
 *   0. DETERMINISTE (sans socket, thread principal) : compile un ServiceREST,
 *      verifie que le registre runtime s'est peuple, sert des requetes via
 *      depose/traite_une/attend. Contient le CONTROLE NEGATIF (route non
 *      resolue -> 404) et le discriminant de routage (deux routes -> deux
 *      corps distincts) — sans quoi un routeur qui « ne sert rien » passerait
 *      au vert (cf. ).
 *   1. E2E SOCKET : la meme requete servie en loopback via OwlMockPeer, pour
 *      prouver le cablage complet du transport (patron joue_e2e de
 *      http_exec_test.c).
 *   2. BINDING PARAMS (, chantier 3) : ROUGE ATTENDU tant que
 *      http_bind.c n'existe pas. Chemin/Requete/corps types, coercion
 *      souple, ordre de declaration libre, resolution de route parametree,
 *      redirection 308 sur casse ASCII. Deux assertions y font exception et
 *      sont annotees comme telles : les baselines heritees de 
 *      (route sans parametre, deja verte) et le garde-fou repli ASCII
 *      (assertion negative qui doit rester vraie avant ET apres impl).
 *   3. CRASH HANDLER -> 500 PROPRE (, lot 1) : un `surRequete` qui
 *      plante VRAIMENT a l'execution (division par zero, indice de tableau
 *      hors bornes, variable resolue par indirection introuvable) doit
 *      produire un 500 sans fuite du message d'erreur interne, et la VM doit
 *      rester utilisable pour la requete suivante. Patron e2e socket
 *      (joue_e2e, meme famille technique que 1) : c'est le seul moyen de
 *      prouver le bout-en-bout process-survit-au-crash.
 *
 * Chaque famille fait son propre vm_init/vm_free : `vm` est un singleton de
 * process (piege g_http_params / g_horloge_decalage_test qui survivent a
 * vm_free — chaque famille repart d'une VM propre).
 *
 *   4. RENVOYER = CORPS : la valeur RENVOYER-ee par un handler
 *      `surRequete` doit devenir le corps de reponse par defaut, serialisee
 *      par type (chaine -> texte brut/text-plain, structure -> JSON/
 *      application-json), sauf si `MoiMeme..Reponse..Corps` est pose explicitement
 *      (override) ou `MoiMeme..Reponse..TypeContenu` (override du content-type
 *      deduit). ROUGE ATTENDU aujourd'hui : execute_service_job jette la
 *      valeur de retour (`vm.stackTop = svcSavedTop`) et ne lit le corps que
 *      depuis `MoiMeme..Reponse..Corps`.
 *   6. CONTEXTE REQUETE/REPONSE IMBRIQUE : ROUGE ATTENDU tant
 *      que le compilateur ne reconnait pas `Reponse`/`Requete` au 1er
 *      niveau de MoiMeme (service_response_prop_is_known, compiler.c
 *      ~l.8761) -- `surRequete` echoue a la COMPILATION, toute la famille
 *      rougit en cascade. Couvre l'ecriture (`MoiMeme..Reponse..X`, calque
 *      du modele plat migre), la lecture (`MoiMeme..Requete..Methode/
 *      Chemin`), le slot Identite (Null par defaut, ecrit puis relu dans
 *      le meme handler --  le remplira via l'authn), le piege
 *       (deux affectations `MoiMeme..Reponse..X` consecutives
 *      doivent TOUTES DEUX prendre effet) et la forme bilingue EN
 *      (`Myself..Response..StatusCode`, `Myself..Request..Method`).
 *
 *   7. AUTHENTIFICATION (, lot 1) : ROUGE ATTENDU. `surAuthentification`
 *      au niveau service n'est pas encore reconnu par `service_rest_declaration`
 *      (aujourd'hui seul `<nom> est une OperationHTTP` est accepte a ce niveau
 *      -- toute autre forme tombe dans la branche erreur generique) ET la
 *      propriete `Authentification` d'opt-out par operation n'existe pas
 *      dans le catalogue `PROPS_OPERATION_HTTP` (ui_catalog.c) -- les DEUX
 *      absences font echouer la COMPILATION de la source de cette famille,
 *      donc `owl_http_executeur_demarre` echoue et toute la famille rougit
 *      en cascade (meme patron que la famille 6 avant ). Patron
 *      e2e socket (`joue_e2e`, en-tetes HTTP bruts) : SEUL moyen de faire
 *      porter un vrai en-tete `Authorization` jusqu'a `OwlHttpRequete`, le
 *      harnais deterministe (`fabrique_requete`) n'en pose aucun. Couvre :
 *      Vrai -> passe + Identite relisible dans surRequete ; Faux + code
 *      explicite pose AVANT le retour -> court-circuite surRequete (ici
 *      403 httpInterdit, choisi plutot que le 401 de l'enonce pour ne pas
 *      coincider avec la valeur du fail-closed ci-dessous -- meme
 *      mecanisme, code arbitraire distinct) ; Faux SANS code pose -> 401
 *      par defaut (FAIL-CLOSED, le test de securite cle) ; extraction
 *      Bearer exacte (positif : jeton sans le prefixe `Bearer ` matche ;
 *      negatif : en-tete SANS le prefixe `Bearer ` n'est jamais pris tel
 *      quel comme credential) ; en-tete absent -> credential vide, hook
 *      quand meme invoque (pas de court-circuit magique) ; opt-out
 *      `httpAuthAucune` sur une operation -> hook jamais invoque, requete
 *      servie sans aucun credential ; forme bilingue EN complete
 *      (`onAuthentication`, `Myself..Request..Identity`, `httpAuthBearer`).
 *
 *   9. HOOKS DE CYCLE DE VIE (, lot 1) : ROUGE ATTENDU. `FinInitialisation`
 *      / `Fermeture` / `surErreur` (et leurs formes EN) ne sont pas encore
 *      reconnus au niveau service -- seule `surAuthentification` passe la
 *      whitelist (compiler.c:18294) -- donc la compilation echoue en cascade,
 *      comme les familles 6/7/8 avant leur feature. Patron e2e socket
 *      exclusif (les deux hooks one-shot vivent dans hx_thread_fn, jamais
 *      atteints par le harnais deterministe). Couvre : `FinInitialisation`
 *      one-shot (compteur inchange entre 1re et 2e requete) ; `Fermeture`
 *      une fois a l'arret, observee par capture stdout dup/dup2 autour de
 *      `owl_http_executeur_arrete` (meme patron que
 *      multilingue_print_test.c::capture_interpret, calque du teardown
 *      projet `run-align-project/case_048`) ; `surErreur` sur le crash
 *      handler avec corps custom portant methode+chemin de la requete ;
 *      `surErreur` jamais invoque sur un succes ; `surErreur` qui ne pose
 *      rien -> 500 par defaut (fail-safe) ; forme bilingue EN complete.
 *
 *  10. AUTHENTIFICATION BASIQUE (, lot 2) : ROUGE ATTENDU tant que
 *      `service_extrait_credential` (http_exec.c) renvoie une chaine vide
 *      pour tout schema != Bearer -- `surAuthentification` ne recoit donc
 *      jamais le credential decode, le hook renvoie systematiquement Faux,
 *      fail-closed 401 partout ou un succes est attendu. Reutilise
 *      integralement le pipeline authn de la famille 7 (meme hook, meme
 *      fail-closed, meme opt-out) -- seule l'extraction change : strip
 *      "Basic " (insensible casse) puis base64-decode. Patron e2e socket
 *      exclusif (joue_e2e) : seul moyen de poser un vrai en-tete
 *      `Authorization: Basic <b64>` sur le fil. Couvre : credential decode
 *      valide relu dans surRequete (positif, prouve le decodage) ;
 *      mauvais credential -> 401 ; en-tete SANS prefixe "Basic " jamais
 *      pris tel quel (controle negatif d'extraction, meme piege que
 *      Bearer) ; base64 malforme (chars hors alphabet, "Basic " suivi de
 *      rien) -> credential vide -> 401, jamais de crash ; SECURITE CLE --
 *      smuggling par octet NUL : base64("admin\0:x") decode en une chaine
 *      contenant un NUL, DOIT etre traite comme malforme (credential vide)
 *      et ne JAMAIS laisser passer "admin" tronque au NUL ; opt-out
 *      `httpAuthAucune` inchange sur une operation du meme service Basique
 *      (deja vert, l'opt-out court-circuite l'extraction entierement) ;
 *      forme bilingue EN complete (`httpAuthBasic`).
 *
 *  11. AUTHENTIFICATION CLE API (, lot 3) : ROUGE ATTENDU tant que
 *      l'en-tete `X-API-Key` n'est ni capture (`owl_http_executeur_depose`,
 *      seul `Authorization` l'est aujourd'hui) ni branche dans
 *      `service_extrait_credential` (le schema `httpAuthCléAPI`/
 *      `httpAuthApiKey` tombe dans le defaut "chantier aval -> VIDE").
 *      Reutilise integralement le pipeline authn des familles 7/10 (meme
 *      hook 1 param, meme fail-closed 401, meme opt-out) -- seule la
 *      SOURCE de la valeur change : `X-API-Key` brut, sans decode, sans
 *      prefixe (contrairement a Bearer/Basique). Patron e2e socket
 *      exclusif (`joue_e2e`) : seul moyen de poser un vrai en-tete
 *      `X-API-Key` sur le fil. Couvre : cle valide relue dans
 *      surAuthentification (positif, prouve la propagation brute) ;
 *      mauvaise cle -> 401 ; en-tete absent -> credential vide -> 401
 *      (fail-closed) ; en-tete present mais VIDE -> credential vide -> 401 ;
 *      SECURITE CLE -- un `Authorization: Bearer <bonne-cle>` SANS
 *      `X-API-Key` ne doit JAMAIS servir de credential CleAPI (les sources
 *      d'authn ne se melangent pas, controle negatif) ; opt-out
 *      `httpAuthAucune` inchange sur une operation du meme service CleAPI ;
 *      forme bilingue EN complete (`httpAuthApiKey`).
 *
 *  12. AUTORISATION (, lot 4) : ROUGE ATTENDU. `surAutorisation`
 *      (procedure niveau service, SANS parametre) n'est pas encore reconnue
 *      par `service_rest_declaration` -- meme absence de whitelist que
 *      `surAuthentification` avant  (compiler.c ~l.18294) -- donc
 *      la compilation echoue en cascade, toute la famille rougit (meme
 *      patron que les familles 6/7/9 avant leur feature). Etape de pipeline
 *      DISTINCTE de l'authn : tourne APRES `surAuthentification` (l'Identite
 *      est deja posee) et AVANT `surRequete`/le binding. Patron e2e socket
 *      exclusif (`joue_e2e`) : reutilise le pipeline Bearer de la famille 7.
 *      Couvre : Vrai (identite = "admin") -> passe jusqu'a `surRequete` ;
 *      Faux + code explicite pose AVANT le retour -> court-circuite AVANT
 *      `surRequete` (403 pose par le hook) ; Faux SANS code pose -> 403
 *      `httpInterdit` par defaut (FAIL-CLOSED, le test de securite cle,
 *      distinct du 401 fail-closed de l'authn) ; authz LIT l'Identite
 *      deposee par authn (reflet dans le corps, prouve le passage
 *      authn -> authz) ; authn REJETE (401) -> authz jamais invoquee (401,
 *      pas 403 -- controle negatif d'ordonnancement du pipeline) ;
 *      branchement sur `MoiMeme..Requete..Methode` (GET autorise,
 *      DELETE refuse pour un non-admin) ; opt-out `httpAuthAucune` sur une
 *      operation -> skip authn ET authz (aucun credential, hook authz
 *      jamais invoque non plus) ; forme bilingue EN complete
 *      (`onAuthorization`, `Myself..Request..Identity`).
 *
 *  13. AUTHENTIFICATION JWT DECLARATIVE (, lot 5, dernier du
 *      cluster) : ROUGE ATTENDU. `httpAuthJWT` (enum authn) et les
 *      proprietes service `ClePubliqueJWT`/`EmetteurJWT` (formes ASCII des
 *      noms accentues du ticket -- meme convention robustesse que le reste
 *      du fichier) ne sont reconnues nulle part aujourd'hui -- la
 *      compilation de cette famille echoue en cascade (meme patron que les
 *      familles 6/7/9/12 avant leur feature). Contrairement aux familles
 *      7/10/11/12, ce schema est **declaratif, zero hook** : pas de
 *      `surAuthentification` dans la source -- le framework doit valider
 *      lui-meme signature RS256 + `exp` + `iss`, puis deposer les claims
 *      (objet JSON) dans `MoiMeme..Requete..Identite`, relus ici via
 *      `Identite["sub"]`. Les JWT sont signes EN-TEST via mbedtls
 *      (`jwt_signe_rs256`, keypair RSA-2048 de test hardcode PEM
 *      prive+public genere hors-ligne, jamais utilise ailleurs) avec un
 *      `exp` calcule dynamiquement (`time(NULL) + delta`) -- jamais un
 *      jeton fige qui expirerait dans le futur. Patron e2e socket exclusif
 *      (`joue_e2e`) : seul moyen de poser un vrai en-tete `Authorization:
 *      Bearer <jwt>`. Corpus d'attaque (chacun -> 401 fail-closed,
 *      AUJOURD'HUI comme APRES impl -- seul le cas 1 discrimine
 *      aujourd'hui, cf. note en tete de fonction) : `alg=none` (LA faille
 *      JWT historique, signature 3e segment vide) ; `alg=HS256` (pas de
 *      confusion d'algo) ; signature RS256 valide mais 1 octet flippe ;
 *      `exp` absent ; `exp` depasse (`now - 3600`) ; `iss` faux ; `iss`
 *      absent ; JWT malforme (pas 3 segments, segment non-base64url, jeton
 *      vide) -- aucun crash sur aucun de ces cas. Cas VALIDE (LE rouge
 *      discriminant) : RS256 correctement signe + `exp` futur + `iss`
 *      correct -> 200, claim `sub` relu dans `surRequete` via
 *      `Identite["sub"]`. Forme bilingue EN complete (`httpAuthJWT` EN,
 *      `JWTPublicKey`, `JWTIssuer`, `Myself..Request..Identity["sub"]`).
 *      Complements (design tranche) : (a) opt-out `Authentification =
 *      httpAuthAucune` sur une operation d'un service JWT -> passe SANS
 *      jeton (garde `!authOptOut` partage avec les familles 7/12) ; (b)
 *      composition `surAutorisation` APRES JWT valide : le hook authz lit
 *      un claim (`Identite["role"]`) depose par la validation declarative
 *      JWT -- zero hook d'authn, authz pur sur claims.
 *
 *  15. MULTIPART/FORM-DATA -- FichierTéléversé : ROUGE ATTENDU.
 *      Ni la propriete `ContentType` d'OperationHTTP, ni le type de
 *      parametre `FichierTéléversé` (ASCII `FichierTeleverse` dans les
 *      sources C ci-dessous, meme convention de robustesse que le reste du
 *      fichier) n'existent -- `ContentType` tombe dans la branche propriete
 *      inconnue et un parametre `FichierTéléversé` est aujourd'hui traite
 *      comme un scalaire ordinaire SANS source de liaison connue (ni
 *      Chemin, ni Requete), donc la compilation echoue en cascade (meme
 *      patron que les familles 6/7/9/12/13 avant leur feature). Patron
 *      e2e socket (`joue_e2e`) : seul moyen de poser un VRAI corps
 *      multipart/form-data avec `Content-Length` exact sur le fil. Couvre :
 *      un champ texte + un fichier lies par nom a `surRequete(titre, scan)`
 *      (les deux receptionnes) ; `scan..Taille`/`scan..NomFichier`/
 *      `scan..TypeMime` relus dans le corps de reponse (metadonnees,
 *      distinctes du contenu) ; `tableau de FichierTeleverse` avec deux
 *      parties de meme nom de champ. SECURITE -- nom de fichier client
 *      HOSTILE (`../../etc/passwd`, chemin Windows absolu) : GARDE-FOU DE
 *      NON-REGRESSION (deliberement VRAI des aujourd'hui ET apres impl,
 *      pas un rouge attendu) -- aucun fichier n'apparait jamais au chemin
 *      litteral fourni par le client, quel que soit l'etat de la feature.
 *      Limite de taille par-fichier AVANT le handler : HORS PERIMETRE de
 *      cette famille -- le mecanisme (seuil dedie vs `corps_max` existant,
 *      ) n'est pas encore tranche par le ticket, voir rapport
 *      test_writer (needs_input) ; `corps_max` (413 avant tout parsing,
 *      deja livre et content-type-agnostique) resterait VERT des
 *      aujourd'hui et ne prouverait donc rien de propre a ce ticket.
 *
 *  18. SECURITE METHODE VIA CONSTANTE httpMethodes : ROUGE
 *      ATTENDU. `httpDelete`/`httpGet` sont aujourd'hui des entiers alors
 *      que `MoiMeme..Requete..Methode` est une chaine ("GET"/"DELETE"/...).
 *      Un `surAutorisation` qui refuse via `Methode = httpDelete` compare
 *      donc une chaine a un entier -- TOUJOURS faux -- et ne refuse jamais
 *      rien : faux-negatif d'autorisation (un DELETE cense etre bloque
 *      passe). Patron deterministe (reutilise `joue_e2e`, service simple,
 *      pas de socket necessaire au fond mais garde le patron e2e commun du
 *      fichier). Couvre : GET passe (n'est pas httpDelete, quel que soit
 *      l'etat du bug) ; DELETE DOIT etre refuse (403) une fois httpMethodes
 *      string-valued -- LE rouge discriminant, aujourd'hui 200 a tort ;
 *      garde-fou retrocompat -- comparaison a la chaine litterale "GET"
 *      (pas une constante) reste vraie avant ET apres le fix ; non-
 *      regression routing -- `Methode = httpGet` route toujours bien un
 *      GET vers l'operation declaree.
 *
 *  16. ROUTE `/openapi.json` (, critère 8) : ROUGE ATTENDU. Le
 *      document OpenAPI 3 dérivé de l'AST (générateur `http_openapi.{c,h}`,
 *      cœur du ticket) doit être exposé par le serveur lui-même sur
 *      `GET /openapi.json`, à côté des routes métier du service — n'importe
 *      quel client Scalar/Swagger/Postman externe pointe dessus sans asset
 *      web embarqué. Aujourd'hui cette route n'existe nulle part dans
 *      `owl_http_exec_handler_rep` : elle tombe dans la résolution normale
 *      du routeur, ne matche aucune opération déclarée -> 404, jamais de
 *      corps JSON. Patron e2e socket (`joue_e2e`) : seul moyen de vérifier
 *      le VRAI transport (statut + corps servis par le process serveur),
 *      symétrique de la famille 1. Couvre les mêmes champs structurants que
 *      le critère 1 (CLI `owl openapi`, cf. tests/cli/openapi/) : version
 *      `openapi` en 3.x, blocs `info`/`paths`/`components`, la route
 *      `GET /api/items` déclarée par ce service retrouvée sous sa clé de
 *      chemin complète (`UrlBase` + `Chemin`).
 *
 * Execute via `owl --self-test-http-dsl-service`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

/* Signature des JWT en-test (famille 13, ) : RS256 via mbedtls,
 * meme bibliotheque deja vendoree/linkee (cf. stdlib_crypto.c, ftp_mock_server.c). */
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

#include "../http_exec.h"
#include "../http_server.h"
#include "../http_service_runtime.h"
#include "../net_mock_peer.h"
#include "../vm.h"
#include "../gc.h"
#include "../owl_event_catalog.h"
#include "fixture_unique.h"
/* compiler.h tire prescan.h/scanner.h dont l'enum TokenType entre en collision
 * avec winnt.h (windows.h, via la couche reseau). On declare le seul symbole
 * requis : le reset des registres service entre deux compilations. */
extern void compiler_reset_poo_state(void);

#ifdef _WIN32
  #include <io.h>
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <unistd.h>
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* Sockets bruts pour le pair JWKS de test (famille 28, ) : meme
 * patron que http_jar_redirect_test.c::jr_thread_fn -- pas de conflit avec
 * compiler.h puisque ce fichier ne l'inclut jamais (cf. commentaire ci-dessus
 * sur TokenType/winnt.h). */
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef HANDLE jwks_thread_t;
  typedef SOCKET jwks_socket_t;
  #define JWKS_INVALID   INVALID_SOCKET
  #define JWKS_CLOSE     closesocket
  #define JWKS_SHUT_WR   SD_SEND
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  typedef pthread_t jwks_thread_t;
  typedef int       jwks_socket_t;
  #define JWKS_INVALID   (-1)
  #define JWKS_CLOSE     close
  #define JWKS_SHUT_WR   SHUT_WR
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define OWL_DSL_SERVICE_NB_FAMILLES 30

static int g_pass[OWL_DSL_SERVICE_NB_FAMILLES] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int g_fail[OWL_DSL_SERVICE_NB_FAMILLES] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < OWL_DSL_SERVICE_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Source OWL : un ServiceREST a deux routes.
 *   GET  /ping      -> statut httpOK  (200), corps "pong"
 *   POST /ressource -> statut httpCree (201), corps "cree"
 * Identifiants en forme ASCII (le scanner strippe les accents) : ni accent ni
 * UTF-8 dans le literal C, un choix de robustesse, pas de langage.
 * ================================================================ */

static const char* SRC_SERVICE =
    "ServicePing est un ServiceREST\n"
    "    Pong est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ping\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"pong\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Cree est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpPost\n"
    "            Chemin   = [\"ressource\"]\n"
    "            Reponses = [httpCree]\n"
    "            CodeEtat = httpCree\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpCree\n"
    "            MoiMeme..Reponse..Corps = \"cree\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : un service dont chaque route pose un CodeEtat different,
 * pour verrouiller le REASON-PHRASE de la ligne de statut.
 * Ces codes tombaient tous sur `default: "Error"` avant le fix, sauf 200.
 * ================================================================ */

static const char* SRC_RAISONS =
    "ServiceRaisons est un ServiceREST\n"
    "    R200 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r200\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "    R201 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r201\"]\n"
    "            Reponses = [httpCree]\n"
    "            CodeEtat = httpCree\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpCree\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "    R202 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r202\"]\n"
    "            Reponses = [httpAccepte]\n"
    "            CodeEtat = httpAccepte\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpAccepte\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "    R401 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r401\"]\n"
    "            Reponses = [httpNonAutorise]\n"
    "            CodeEtat = httpNonAutorise\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpNonAutorise\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "    R403 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r403\"]\n"
    "            Reponses = [httpInterdit]\n"
    "            CodeEtat = httpInterdit\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpInterdit\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "    R409 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode = httpGet\n"
    "            Chemin = [\"r409\"]\n"
    "            Reponses = [httpConflit]\n"
    "            CodeEtat = httpConflit\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpConflit\n"
    "            MoiMeme..Reponse..Corps = \"x\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : binding runtime (, chantier 3).
 *
 * Un service a six operations qui couvrent les criteres d'acceptation :
 *   GET  /clients/actifs -> route LITTERALE (zero param, deja vivante
 *                            depuis  : sert de temoin baseline).
 *   GET  /clients/{id}   -> route PARAMETREE : id vient du CHEMIN,
 *                            CodeEtat/Corps varient selon la valeur
 *                            (200/"id-42" ou 201/"id-autre").
 *   POST /clients        -> corps JSON type (structure) -> bind + echo.
 *   PUT  /clients/{id}   -> ORDRE DE DECLARATION INVERSE (client avant
 *                            id) : la liaison est par nom, pas position.
 *   GET  /recherche      -> parametre de QUERY optionnel (defaut "nom").
 *   GET  /caf${e-accent} -> route litterale a segment ACCENTUE, pour le
 *                            garde-fou repli ASCII pur (pas de fold
 *                            hors-ASCII).
 * ================================================================ */

static const char* SRC_SERVICE_BINDING =
    "ServiceClientsBinding est un ServiceREST\n"
    "    stClientBinding est une Structure\n"
    "        nom est une chaine\n"
    "    FIN\n"
    "    ClientsActifs est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"clients\", \"actifs\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"liste-actifs\"\n"
    "        FIN\n"
    "    FIN\n"
    "    ClientParId est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"clients\", id]\n"
    "            Reponses = [httpOK, httpCree]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(id est un entier)\n"
    "            SI id = 42 ALORS\n"
    "                MoiMeme..Reponse..CodeEtat = httpOK\n"
    "                MoiMeme..Reponse..Corps = \"id-42\"\n"
    "            SINON\n"
    "                MoiMeme..Reponse..CodeEtat = httpCree\n"
    "                MoiMeme..Reponse..Corps = \"id-autre\"\n"
    "            FIN\n"
    "        FIN\n"
    "    FIN\n"
    "    ClientCree est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpPost\n"
    "            Chemin   = [\"clients\"]\n"
    "            Reponses = [httpCree]\n"
    "            CodeEtat = httpCree\n"
    "        FIN\n"
    "        PROCEDURE surRequete(client est une stClientBinding)\n"
    "            MoiMeme..Reponse..CodeEtat = httpCree\n"
    "            MoiMeme..Reponse..Corps = client.nom\n"
    "        FIN\n"
    "    FIN\n"
    "    ClientMaj est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpPut\n"
    "            Chemin   = [\"clients\", id]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(client est une stClientBinding, id est un entier)\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = client.nom\n"
    "        FIN\n"
    "    FIN\n"
    "    ClientsRecherche est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"recherche\"]\n"
    "            Requete  = [tri]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(tri est une chaine = \"nom\")\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = tri\n"
    "        FIN\n"
    "    FIN\n"
    "    CafeMenu est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"caf\xc3\xa9\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"menu-cafe\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : Structure declaree DANS le bloc ServiceREST.
 *
 * `stDemandeInterne` vit au top-level du CORPS du service (co-localisee avec
 * l'unique operation qui la consomme), pas au top-level fichier. Elle sert de
 * type de parametre pour `surRequete` : le corps JSON POST doit etre
 * deserialise dedans et son champ relu dans la reponse.
 *   POST /creer -> corps JSON type (structure DANS le bloc) -> bind + echo.
 * ================================================================ */

static const char* SRC_SERVICE_STRUCT_DANS_BLOC =
    "ServiceStructDansBloc est un ServiceREST\n"
    "    stDemandeInterne est une Structure\n"
    "        nom est une chaine\n"
    "    FIN\n"
    "    Creer est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpPost\n"
    "            Chemin   = [\"creer\"]\n"
    "            Reponses = [httpCree]\n"
    "            CodeEtat = httpCree\n"
    "        FIN\n"
    "        PROCEDURE surRequete(demande est une stDemandeInterne)\n"
    "            MoiMeme..Reponse..CodeEtat = httpCree\n"
    "            MoiMeme..Reponse..Corps = demande.nom\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : service adversarial (, lot 1).
 *
 * Une route saine (Ok) plus trois routes dont le handler plante VRAIMENT a
 * l'execution, chacune par un mecanisme d'erreur runtime distinct :
 *   GET /ok         -> pas de crash, sert de temoin avant/apres.
 *   GET /div0       -> division entiere par zero (E_RT_DIV_BY_ZERO).
 *   GET /indexoob   -> indice de tableau hors bornes (E_RT_ARRAY_INDEX_OOB).
 *   GET /indirect   -> indirection {var} vers un nom qui ne resout aucune
 *                       variable (E_RT_INDIRECTION_NAME_UNKNOWN).
 * Chaque handler crash pose un fragment de message distinctif dans une
 * variable locale AVANT de planter — inutile pour la 500 elle-meme, mais
 * les fragments choisis (E_RT_DIV_BY_ZERO, E_RT_ARRAY_INDEX_OOB, le nom de
 * variable introuvable) sont ceux qu'on verifie explicitement ABSENTS du
 * corps de la reponse (garde-fou anti-fuite).
 * ================================================================ */

static const char* SRC_SERVICE_CRASH =
    "ServiceCrash est un ServiceREST\n"
    "    Ok est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ok\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Div0 est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"div0\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            nDividende est un entier = 1\n"
    "            nDiviseur est un entier = 0\n"
    "            nResultat est un entier = nDividende / nDiviseur\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "    IndexOOB est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"indexoob\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            t est un tableau d'entiers = [1, 2, 3]\n"
    "            v est un entier = t[100]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Indirect est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"indirect\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sNomInexistant est une chaine = \"variableIntrouvableZZ\"\n"
    "            v est un entier = {sNomInexistant}\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : service RENVOYER.
 *
 * Cinq operations qui couvrent les criteres d'acceptation :
 *   GET /texte      -> RENVOYER chaine, aucun Corps pose : le corps par
 *                        defaut doit venir de la valeur de retour.
 *   GET /structure  -> RENVOYER structure : corps JSON, content-type
 *                        application/json.
 *   GET /override   -> MoiMeme..Reponse..Corps pose ET RENVOYER une autre valeur :
 *                        l'override gagne.
 *   GET /typeforce  -> MoiMeme..Reponse..TypeContenu pose explicitement, RENVOYER une
 *                        chaine sans Corps : le corps vient du retour, le
 *                        content-type vient de l'override explicite.
 *   GET /vide       -> RENVOYER Null, CodeEtat pose explicitement : corps
 *                        vide, pas de crash, statut pilote par CodeEtat.
 * ================================================================ */

static const char* SRC_SERVICE_RENVOYER =
    "ServiceRenvoyer est un ServiceREST\n"
    "    stRenvoiPersonne est une Structure\n"
    "        nom est une chaine\n"
    "        age est un entier\n"
    "    FIN\n"
    "    Texte est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"texte\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete() : chaine\n"
    "            RENVOYER \"bonjour\"\n"
    "        FIN\n"
    "    FIN\n"
    "    RenvoieStructure est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"structure\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete() : stRenvoiPersonne\n"
    "            p est une stRenvoiPersonne\n"
    "            p.nom = \"Alice\"\n"
    "            p.age = 30\n"
    "            RENVOYER p\n"
    "        FIN\n"
    "    FIN\n"
    "    Override est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"override\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"explicite\"\n"
    "            RENVOYER \"ignore\"\n"
    "        FIN\n"
    "    FIN\n"
    "    TypeForce est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"typeforce\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..TypeContenu = \"application/xml\"\n"
    "            RENVOYER \"<a/>\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Vide est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"vide\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            RENVOYER Null\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : contexte Requete/Reponse imbrique.
 *
 * Trois operations qui couvrent les criteres d'acceptation :
 *   GET /meta     -> lit MoiMeme..Requete..Methode et ..Chemin, les
 *                     reflete dans le corps servi via MoiMeme..Reponse..Corps.
 *   GET /identite -> MoiMeme..Requete..Identite vaut Null avant toute
 *                     ecriture, puis ecrit puis relu DANS LE MEME handler.
 *   GET /double   -> piege  : CodeEtat puis TypeContenu, deux
 *                     affectations MoiMeme..Reponse..X consecutives (PAS
 *                     Corps entre les deux) doivent toutes deux tenir.
 * ================================================================ */

static const char* SRC_SERVICE_CONTEXTE =
    "ServiceContexte est un ServiceREST\n"
    "    MetaRequete est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"meta\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = MoiMeme..Requete..Methode + \"|\" + MoiMeme..Requete..Chemin\n"
    "        FIN\n"
    "    FIN\n"
    "    IdentiteSlot est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"identite\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sAvant est une chaine = \"non-null\"\n"
    "            SI MoiMeme..Requete..Identite = Null ALORS\n"
    "                sAvant = \"null\"\n"
    "            FIN\n"
    "            MoiMeme..Requete..Identite = \"user-42\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"avant:\" + sAvant + \"|apres:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    DoubleAffectation est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"double\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..TypeContenu = \"text/plain\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : contexte Requete/Reponse imbrique, forme BILINGUE EN
 * (, invariant 3). Meme geste que SRC_SERVICE_CONTEXTE (ecriture
 * statut + echo methode via lecture), entierement en formes EN canon.
 * ================================================================ */

static const char* SRC_SERVICE_CONTEXTE_EN =
    "ServiceContexteEN is a ServiceREST\n"
    "    Ping is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = Myself..Request..Method\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification (, lot 1).
 *
 * `surAuthentification` recoit le jeton Bearer extrait par le framework et
 * decide Vrai/Faux :
 *   - "bon-jeton"         -> pose l'Identite, RENVOYER Vrai.
 *   - "mauvais-explicite" -> pose CodeEtat=httpInterdit (403) puis RENVOYER
 *                             Faux (l'app choisit elle-meme son code de rejet).
 *   - tout le reste (y compris jeton vide) -> RENVOYER Faux SANS rien poser
 *     -> le framework doit appliquer le defaut fail-closed (401).
 *
 *   GET /protegee -> protegee par le schema Bearer du service.
 *   GET /publique -> opt-out `Authentification = httpAuthAucune` : le hook
 *                     ne doit jamais etre invoque pour cette route.
 *   GET /sonde-longue -> protegee par le meme schema Bearer. Le hook
 *                     n'autorise PAS sur la valeur du jeton mais sur sa
 *                     LONGUEUR (`Taille(jeton) = 1000`) : verrou anti-
 *                     regression  (buffer credential `cred[512]`
 *                     partage qui tronquait tout Bearer >= 511 octets ->
 *                     credential vide -> 401). Si le framework retronque le
 *                     jeton, `Taille(jeton)` != 1000 -> ce hook rejette ->
 *                     ce test rougit pour la bonne raison.
 * ================================================================ */

static const char* SRC_SERVICE_AUTH =
    "ServiceAuth est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthBearer\n"
    "    FIN\n"
    "    PROCEDURE surAuthentification(jeton est une chaine) : booleen\n"
    "        SI jeton = \"bon-jeton\" ALORS\n"
    "            MoiMeme..Requete..Identite = \"user-42\"\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        SI jeton = \"mauvais-explicite\" ALORS\n"
    "            MoiMeme..Reponse..CodeEtat = httpInterdit\n"
    "            RENVOYER Faux\n"
    "        FIN\n"
    "        SI Taille(jeton) = 1000 ALORS\n"
    "            MoiMeme..Requete..Identite = \"\" + Taille(jeton)\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    OpProtegee est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"protegee\"]\n"
    "            Reponses = [httpOK, httpNonAutorise, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"metier:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    OpSondeLongue est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde-longue\"]\n"
    "            Reponses = [httpOK, httpNonAutorise]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"recu_taille:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    OpPublique est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"publique\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "            Authentification = httpAuthAucune\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"public-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification, forme BILINGUE EN complete
 * (`onAuthentication`, `Myself..Request..Identity`, `httpAuthBearer`).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_EN =
    "ServiceAuthEN is a ServiceREST\n"
    "    PROPERTIES\n"
    "        Authentication = httpAuthBearer\n"
    "    FIN\n"
    "    PROCEDURE onAuthentication(token is a string) : boolean\n"
    "        IF token = \"ok-en\" THEN\n"
    "            Myself..Request..Identity = \"en-user\"\n"
    "            RETURN true\n"
    "        FIN\n"
    "        RETURN false\n"
    "    FIN\n"
    "    Ping is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = Myself..Request..Identity\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification Basique (, lot 2).
 *
 * `surAuthentification` recoit le "user:pass" DECODE (framework strip
 * "Basic ", base64-decode, credential VIDE sur tout ecart -- prefixe
 * absent, base64 invalide, decode contenant un octet NUL). Meme hook,
 * meme signature 1 param que Bearer -- seule l'extraction
 * differe cote framework.
 *
 *   GET /sonde     -> protegee par le schema Basique du service. Deux
 *                      identifiants acceptes ("alice:secret" legitime,
 *                      "admin" -- credential jamais atteignable via un
 *                      Authorization Basic normal, sert de piege pour le
 *                      test de smuggling NUL ci-dessous).
 *   GET /ressource -> opt-out `Authentification = httpAuthAucune` : le
 *                      hook ne doit jamais etre invoque pour cette route.
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_BASIQUE =
    "ServiceAuthBasique est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthBasique\n"
    "    FIN\n"
    "    PROCEDURE surAuthentification(jeton est une chaine) : booleen\n"
    "        SI jeton = \"alice:secret\" ALORS\n"
    "            MoiMeme..Requete..Identite = jeton\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        SI jeton = \"admin\" ALORS\n"
    "            MoiMeme..Requete..Identite = jeton\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde\"]\n"
    "            Reponses = [httpOK, httpNonAutorise]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"recu:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    Ressource est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ressource\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "            Authentification = httpAuthAucune\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ressource-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification Basique, forme BILINGUE EN complete
 * (`onAuthentication`, `Myself..Request..Identity`, `httpAuthBasic`).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_BASIQUE_EN =
    "ServiceAuthBasicEN is a ServiceREST\n"
    "    PROPERTIES\n"
    "        Authentication = httpAuthBasic\n"
    "    FIN\n"
    "    PROCEDURE onAuthentication(token is a string) : boolean\n"
    "        IF token = \"alice:secret\" THEN\n"
    "            Myself..Request..Identity = token\n"
    "            RETURN true\n"
    "        FIN\n"
    "        RETURN false\n"
    "    FIN\n"
    "    Ping is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK, httpNonAutorise]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"recu:\" + Myself..Request..Identity\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification Cle API (, lot 3).
 *
 * `surAuthentification` recoit la valeur BRUTE de l'en-tete `X-API-Key`
 * (pas de decode, pas de prefixe -- contrairement a Bearer/Basique). Meme
 * hook, meme signature 1 param que les familles 7/10 -- seule la SOURCE
 * de la valeur change cote framework.
 *
 *   GET /sonde     -> protegee par le schema CleAPI du service.
 *   GET /ressource -> opt-out `Authentification = httpAuthAucune` : le
 *                      hook ne doit jamais etre invoque pour cette route.
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_CLE_API =
    "ServiceAuthCleAPI est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthCléAPI\n"
    "    FIN\n"
    "    PROCEDURE surAuthentification(jeton est une chaine) : booleen\n"
    "        SI jeton = \"cle-secrete-123\" ALORS\n"
    "            MoiMeme..Requete..Identite = jeton\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde\"]\n"
    "            Reponses = [httpOK, httpNonAutorise]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"recu:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    Ressource est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ressource\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "            Authentification = httpAuthAucune\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ressource-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : authentification Cle API, forme BILINGUE EN complete
 * (`onAuthentication`, `Myself..Request..Identity`, `httpAuthApiKey`).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_CLE_API_EN =
    "ServiceAuthApiKeyEN is a ServiceREST\n"
    "    PROPERTIES\n"
    "        Authentication = httpAuthApiKey\n"
    "    FIN\n"
    "    PROCEDURE onAuthentication(token is a string) : boolean\n"
    "        IF token = \"cle-secrete-123\" THEN\n"
    "            Myself..Request..Identity = token\n"
    "            RETURN true\n"
    "        FIN\n"
    "        RETURN false\n"
    "    FIN\n"
    "    Ping is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK, httpNonAutorise]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"recu:\" + Myself..Request..Identity\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : en-tetes de reponse dynamiques (, lot 1).
 *
 * MoiMeme..Reponse..Entetes["Nom"] = "Valeur" reutilise l'ecriture indexee
 * par chaine des tableaux associatifs (zero nouvelle grammaire). Chaque
 * route exerce UN comportement a verrouiller :
 *   /un         -> pose + emission d'un seul en-tete.
 *   /plusieurs  -> 3 en-tetes poses, tous emis.
 *   /injection  -> valeur portant CRLF -> refusee au point de passage
 *                  unique (owl_http_rep_ajoute_entete), jamais d'en-tete
 *                  "Evil:" scinde sur le fil.
 *   /lecture    -> un en-tete pose plus tot est relisible dans le meme
 *                  handler (tableau associatif, lecture symetrique).
 *   /collision  -> TypeContenu (canal dedie) reste l'unique source du
 *                  Content-Type meme si Entetes["Content-Type"] est aussi
 *                  pose : jamais de doublon.
 *   /tolerant   -> forme sans accent/casse (..entetes[...]) marche au
 *                  runtime, meme canon qu'..Entetes.
 *   /overflow   -> 17 en-tetes poses, plafond OWL_HTTP_REP_ENTETES_MAX=16
 *                  respecte, pas de crash (edge case proactif test_writer).
 * ================================================================ */

static const char* SRC_SERVICE_ENTETES =
    "ServiceEntetes est un ServiceREST\n"
    "    UnEntete est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"un\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"Cache-Control\"] = \"no-store\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Plusieurs est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"plusieurs\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"X-Un\"] = \"un\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-Deux\"] = \"deux\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-Trois\"] = \"trois\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Injection est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"injection\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"X-Test\"] = \"a\" + CRLF + \"Evil: pwned\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Lecture est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"lecture\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"X-Lu\"] = \"valeur-posee\"\n"
    "            sLu est une chaine = MoiMeme..Reponse..Entetes[\"X-Lu\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"lu:\" + sLu\n"
    "        FIN\n"
    "    FIN\n"
    "    Collision est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"collision\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..TypeContenu = \"application/json\"\n"
    "            MoiMeme..Reponse..Entetes[\"Content-Type\"] = \"text/hostile\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"{}\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Tolerant est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"tolerant\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..reponse..entetes[\"X-Tol\"] = \"ok-tolerant\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Overflow est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"overflow\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"X-H01\"] = \"1\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H02\"] = \"2\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H03\"] = \"3\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H04\"] = \"4\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H05\"] = \"5\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H06\"] = \"6\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H07\"] = \"7\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H08\"] = \"8\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H09\"] = \"9\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H10\"] = \"10\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H11\"] = \"11\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H12\"] = \"12\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H13\"] = \"13\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H14\"] = \"14\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H15\"] = \"15\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H16\"] = \"16\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-H17\"] = \"17\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Forme bilingue EN complete : Myself..Response..Headers["..."] = "...". */
static const char* SRC_SERVICE_ENTETES_EN =
    "ServiceHeadersEN is a ServiceREST\n"
    "    OneHeader is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"en\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..Headers[\"X-Foo\"] = \"bar\"\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source OWL : en-tetes ENTRANTS de requete, en lecture. Miroir
 * cote lecture de SRC_SERVICE_ENTETES (, ecriture cote Reponse).
 * `Entetes`/`Headers` n'est PAS dans la liste blanche 2e niveau de
 * MoiMeme..Requete (service_ctx_request_sub_canon, compiler.c:8858-8866,
 * qui ne reconnait que Methode/Chemin/Identite) -> la compilation de
 * surRequete echoue -> ROUGE ATTENDU sur toutes les routes, meme patron que
 * la famille 8. */
static const char* SRC_SERVICE_ENTETES_REQUETE =
    "ServiceEntetesRequete est un ServiceREST\n"
    "    Echo est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"echo\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..Requete..Entetes[\"X-Forwarded-For\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"fwd:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "    CasseMin est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"casse-min\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..Requete..Entetes[\"x-forwarded-for\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"min:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "    CasseMaj est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"casse-maj\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..Requete..Entetes[\"X-FORWARDED-FOR\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"maj:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "    Absent est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"absent\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..Requete..Entetes[\"X-Inexistant\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"abs:[\" + sVal + \"]\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Repete est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"repete\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..Requete..Entetes[\"X-Test\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"rep:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "    Combo est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"combo\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sMeth est une chaine = MoiMeme..Requete..Methode\n"
    "            sChem est une chaine = MoiMeme..Requete..Chemin\n"
    "            sHdr est une chaine = MoiMeme..Requete..Entetes[\"X-Extra\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = sMeth + \"|\" + sChem + \"|\" + sHdr\n"
    "        FIN\n"
    "    FIN\n"
    "    Tolerant est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"tolerant\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sVal est une chaine = MoiMeme..requete..entetes[\"X-Tol\"]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"tol:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Forme bilingue EN complete : Myself..Request..Headers["..."] en lecture. */
static const char* SRC_SERVICE_ENTETES_REQUETE_EN =
    "ServiceRequestHeadersEN is a ServiceREST\n"
    "    OneHeader is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"en\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            sVal is a string = Myself..Request..Headers[\"X-Foo\"]\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"bar:\" + sVal\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source OWL : en-tetes de reponse a valeur SCALAIRE non-chaine.
 * Un handler pose des valeurs entiere / reelle / booleenne + une chaine
 * temoin, plus un tableau NON coercible qui doit etre SKIPPE proprement. */
static const char* SRC_SERVICE_ENTETES_SCALAIRES =
    "ServiceEntetesScalaires est un ServiceREST\n"
    "    Scalaires est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"scalaires\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Entetes[\"X-Entier\"] = 123\n"
    "            MoiMeme..Reponse..Entetes[\"X-Reel\"] = 3.5\n"
    "            MoiMeme..Reponse..Entetes[\"X-Bool\"] = Vrai\n"
    "            MoiMeme..Reponse..Entetes[\"X-Chaine\"] = \"abc\"\n"
    "            MoiMeme..Reponse..Entetes[\"X-Tableau\"] = [1, 2]\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source OWL : corps binaire brut via MoiMeme..Reponse..Corps = <buffer>
 *. Le blob (21 octets) est construit octet-par-octet via
 * BufferVide + BufferÉcritOctet : un octet NUL au milieu + 4 octets non-
 * UTF-8-valides (0xFF/0xFE/0xC0/0x80), un tel contenu ne peut PAS etre
 * feinte comme une chaine UTF-8. /pdf pose TypeContenu explicitement ;
 * /texte est le temoin non-regression (corps chaine reste text/plain). */
static const char* SRC_SERVICE_CORPS_BINAIRE =
    "ServiceCorpsBinaire est un ServiceREST\n"
    "    Pdf est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"pdf\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            buf est un buffer = BufferVide(21)\n"
    "            BufferÉcritOctet(buf, 1, 37)\n"
    "            BufferÉcritOctet(buf, 2, 80)\n"
    "            BufferÉcritOctet(buf, 3, 68)\n"
    "            BufferÉcritOctet(buf, 4, 70)\n"
    "            BufferÉcritOctet(buf, 5, 45)\n"
    "            BufferÉcritOctet(buf, 6, 49)\n"
    "            BufferÉcritOctet(buf, 7, 46)\n"
    "            BufferÉcritOctet(buf, 8, 52)\n"
    "            BufferÉcritOctet(buf, 9, 10)\n"
    "            BufferÉcritOctet(buf, 10, 0)\n"
    "            BufferÉcritOctet(buf, 11, 255)\n"
    "            BufferÉcritOctet(buf, 12, 254)\n"
    "            BufferÉcritOctet(buf, 13, 192)\n"
    "            BufferÉcritOctet(buf, 14, 128)\n"
    "            BufferÉcritOctet(buf, 15, 10)\n"
    "            BufferÉcritOctet(buf, 16, 37)\n"
    "            BufferÉcritOctet(buf, 17, 37)\n"
    "            BufferÉcritOctet(buf, 18, 69)\n"
    "            BufferÉcritOctet(buf, 19, 79)\n"
    "            BufferÉcritOctet(buf, 20, 70)\n"
    "            BufferÉcritOctet(buf, 21, 10)\n"
    "            MoiMeme..Reponse..TypeContenu = \"application/pdf\"\n"
    "            MoiMeme..Reponse..Corps = buf\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "        FIN\n"
    "    FIN\n"
    "    Texte est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"texte\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"texte simple\"\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source OWL : corps binaire via RENVOYER <buffer>. Meme blob
 * que ci-dessus. /pdf pose TypeContenu AVANT le RENVOYER (doit gagner) ;
 * /brut ne pose aucun TypeContenu (doit tomber sur un type binaire neutre
 * par defaut, PAS la serialisation JSON qui produirait "null") ; /texte
 * est le temoin non-regression RENVOYER chaine -> text/plain. */
static const char* SRC_SERVICE_RENVOYER_BUFFER =
    "ServiceRenvoyerBuffer est un ServiceREST\n"
    "    Pdf est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"pdf\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            buf est un buffer = BufferVide(21)\n"
    "            BufferÉcritOctet(buf, 1, 37)\n"
    "            BufferÉcritOctet(buf, 2, 80)\n"
    "            BufferÉcritOctet(buf, 3, 68)\n"
    "            BufferÉcritOctet(buf, 4, 70)\n"
    "            BufferÉcritOctet(buf, 5, 45)\n"
    "            BufferÉcritOctet(buf, 6, 49)\n"
    "            BufferÉcritOctet(buf, 7, 46)\n"
    "            BufferÉcritOctet(buf, 8, 52)\n"
    "            BufferÉcritOctet(buf, 9, 10)\n"
    "            BufferÉcritOctet(buf, 10, 0)\n"
    "            BufferÉcritOctet(buf, 11, 255)\n"
    "            BufferÉcritOctet(buf, 12, 254)\n"
    "            BufferÉcritOctet(buf, 13, 192)\n"
    "            BufferÉcritOctet(buf, 14, 128)\n"
    "            BufferÉcritOctet(buf, 15, 10)\n"
    "            BufferÉcritOctet(buf, 16, 37)\n"
    "            BufferÉcritOctet(buf, 17, 37)\n"
    "            BufferÉcritOctet(buf, 18, 69)\n"
    "            BufferÉcritOctet(buf, 19, 79)\n"
    "            BufferÉcritOctet(buf, 20, 70)\n"
    "            BufferÉcritOctet(buf, 21, 10)\n"
    "            MoiMeme..Reponse..TypeContenu = \"application/pdf\"\n"
    "            RENVOYER buf\n"
    "        FIN\n"
    "    FIN\n"
    "    Brut est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"brut\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            buf est un buffer = BufferVide(21)\n"
    "            BufferÉcritOctet(buf, 1, 37)\n"
    "            BufferÉcritOctet(buf, 2, 80)\n"
    "            BufferÉcritOctet(buf, 3, 68)\n"
    "            BufferÉcritOctet(buf, 4, 70)\n"
    "            BufferÉcritOctet(buf, 5, 45)\n"
    "            BufferÉcritOctet(buf, 6, 49)\n"
    "            BufferÉcritOctet(buf, 7, 46)\n"
    "            BufferÉcritOctet(buf, 8, 52)\n"
    "            BufferÉcritOctet(buf, 9, 10)\n"
    "            BufferÉcritOctet(buf, 10, 0)\n"
    "            BufferÉcritOctet(buf, 11, 255)\n"
    "            BufferÉcritOctet(buf, 12, 254)\n"
    "            BufferÉcritOctet(buf, 13, 192)\n"
    "            BufferÉcritOctet(buf, 14, 128)\n"
    "            BufferÉcritOctet(buf, 15, 10)\n"
    "            BufferÉcritOctet(buf, 16, 37)\n"
    "            BufferÉcritOctet(buf, 17, 37)\n"
    "            BufferÉcritOctet(buf, 18, 69)\n"
    "            BufferÉcritOctet(buf, 19, 79)\n"
    "            BufferÉcritOctet(buf, 20, 70)\n"
    "            BufferÉcritOctet(buf, 21, 10)\n"
    "            RENVOYER buf\n"
    "        FIN\n"
    "    FIN\n"
    "    Texte est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"texte\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            RENVOYER \"texte simple\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : hooks de cycle de vie (, lot 1).
 *
 * FinInitialisation compte ses propres executions dans une globale relue par
 * /etat -> prouve le one-shot (la 2e requete doit voir la MEME valeur, pas
 * un compteur qui grimpe). Fermeture pose un temoin Trace observable par
 * capture stdout autour de l'arret du thread executeur (meme mecanisme que
 * le teardown projet, cf. tests/compiler/internal_fixtures/run-align-project/
 * case_048, transpose ici en capture C dup/dup2 faute de process separe).
 * surErreur lit MoiMeme..Requete..Methode/Chemin et personnalise le corps du
 * 500 issu du crash de /crash (division entiere par zero).
 * ================================================================ */

static const char* SRC_SERVICE_HOOKS =
    "ServiceHooks est un ServiceREST\n"
    "    gHooksCompteur est un entier = 0\n"
    "    PROCEDURE FinInitialisation()\n"
    "        gHooksCompteur = gHooksCompteur + 1\n"
    "    FIN\n"
    "    PROCEDURE Fermeture()\n"
    "        Trace(\"fermeture_hooks_temoin\")\n"
    "    FIN\n"
    "    PROCEDURE surErreur()\n"
    "        MoiMeme..Reponse..Corps = \"erreur_geree:\" + MoiMeme..Requete..Methode + \"|\" + MoiMeme..Requete..Chemin\n"
    "    FIN\n"
    "    Statut est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"etat\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"init:\" + gHooksCompteur\n"
    "        FIN\n"
    "    FIN\n"
    "    Crash est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"crash\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            nDividende est un entier = 1\n"
    "            nDiviseur est un entier = 0\n"
    "            nResultat est un entier = nDividende / nDiviseur\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Source OWL : surErreur declare mais qui ne pose RIEN -> fail-safe 500 par
 * defaut (aucune propriete de Reponse touchee dans le hook). */
static const char* SRC_SERVICE_HOOKS_NOOP_ERROR =
    "ServiceHooksNoop est un ServiceREST\n"
    "    PROCEDURE surErreur()\n"
    "        Trace(\"surErreur_invoque_sans_rien_poser\")\n"
    "    FIN\n"
    "    Crash est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"crash\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            nDividende est un entier = 1\n"
    "            nDiviseur est un entier = 0\n"
    "            nResultat est un entier = nDividende / nDiviseur\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Forme bilingue EN complete : EndInitialization/Closing/onError,
 * Myself..Response..Body/StatusCode, Myself..Request..Method/Path. */
static const char* SRC_SERVICE_HOOKS_EN =
    "ServiceHooksEN is a ServiceREST\n"
    "    gHooksEnCompteur is an integer = 0\n"
    "    PROCEDURE EndInitialization()\n"
    "        gHooksEnCompteur = gHooksEnCompteur + 1\n"
    "    FIN\n"
    "    PROCEDURE Closing()\n"
    "        Trace(\"closing_hooks_temoin\")\n"
    "    FIN\n"
    "    PROCEDURE onError()\n"
    "        Myself..Response..Body = \"handled:\" + Myself..Request..Method + \"|\" + Myself..Request..Path\n"
    "    FIN\n"
    "    State is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"state\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"init:\" + gHooksEnCompteur\n"
    "        FIN\n"
    "    FIN\n"
    "    Crash is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"crash\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            n is an integer = 1\n"
    "            d is an integer = 0\n"
    "            r is an integer = n / d\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"never\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : FinInitialisation qui PLANTE (, garde reviewer sur
 * la race demarrage/init). Division entiere par zero -- meme mecanisme de
 * crash hard que Div0 de SRC_SERVICE_CRASH (INTERPRET_RUNTIME_ERROR), ici
 * dans le hook one-shot au demarrage plutot que dans un handler de route.
 * Nom d'operation "Sonde" (ni Etat ni Publique, pas de collision mot-cle).
 * Contrat : owl_http_executeur_demarre(x) doit rendre faux -- fail-safe,
 * pas de service sur une init ratee.
 * ================================================================ */

static const char* SRC_SERVICE_HOOKS_INIT_CRASH =
    "ServiceHooksInitCrash est un ServiceREST\n"
    "    PROCEDURE FinInitialisation()\n"
    "        nZero est un entier = 0\n"
    "        nX est un entier = 1 / nZero\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Forme bilingue EN : EndInitialization qui plante. */
static const char* SRC_SERVICE_HOOKS_INIT_CRASH_EN =
    "ServiceHooksInitCrashEN is a ServiceREST\n"
    "    PROCEDURE EndInitialization()\n"
    "        n is an integer = 0\n"
    "        x is an integer = 1 / n\n"
    "    FIN\n"
    "    Sonde is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"sonde\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"never\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Helpers (repris de http_exec_test.c, sans reseau)
 * ================================================================ */

static void fabrique_requete(OwlHttpRequete* r, const char* methode,
                             const char* chemin, const char* corps) {
    memset(r, 0, sizeof(*r));
    snprintf(r->methode, sizeof(r->methode), "%s", methode);
    r->cible  = (char*)chemin;
    r->chemin = r->cible;
    r->version_majeure = 1;
    r->version_mineure = 1;
    r->corps     = (char*)corps;
    r->corps_len = corps ? strlen(corps) : 0;
    snprintf(r->pair, sizeof(r->pair), "127.0.0.1:0");
}

typedef struct { char d[4096]; size_t len; } Sac;

static int sac_ecrit(void* user, const char* octets, size_t len) {
    Sac* s = (Sac*)user;
    if (s->len + len >= sizeof(s->d)) return -1;
    memcpy(s->d + s->len, octets, len);
    s->len += len;
    s->d[s->len] = '\0';
    return 0;
}

static void vide_reponse(const OwlHttpRep* rep, Sac* sac) {
    OwlHttpPuits p; p.ecrit = sac_ecrit; p.user = sac;
    memset(sac, 0, sizeof(*sac));
    owl_http_rep_ecrit(rep, &p);
}

/* Sert une requete de bout en bout SANS socket : depose -> traite_une ->
 * attend. Rend la reponse (propriete de l'appelant) ou NULL. */
static OwlHttpRep* sert_deterministe(OwlHttpExecuteur* x, const char* methode,
                                     const char* chemin, const char* corps) {
    OwlHttpRequete r;
    fabrique_requete(&r, methode, chemin, corps);
    OwlHttpJeton* j = NULL;
    if (owl_http_executeur_depose(x, &r, &j) != OWL_HTTP_EXEC_OK) return NULL;
    if (owl_http_executeur_traite_une(x, 0) != 1) return NULL;
    OwlHttpRep* rep = NULL;
    if (owl_http_executeur_attend(x, j, &rep) != OWL_HTTP_EXEC_OK) return NULL;
    return rep;
}

/* Variante avec un unique en-tete Content-Type (negociation du corps
 * structure, ). nom/valeur sont recopies au depot (copie_chaine),
 * donc des litteraux suffisent. contentType == NULL -> aucun en-tete. */
static OwlHttpRep* sert_deterministe_ct(OwlHttpExecuteur* x, const char* methode,
                                        const char* chemin, const char* corps,
                                        const char* contentType) {
    OwlHttpRequete r;
    fabrique_requete(&r, methode, chemin, corps);
    if (contentType != NULL) {
        r.entetes[0].nom    = (char*)"Content-Type";
        r.entetes[0].valeur = (char*)contentType;
        r.nb_entetes = 1;
    }
    OwlHttpJeton* j = NULL;
    if (owl_http_executeur_depose(x, &r, &j) != OWL_HTTP_EXEC_OK) return NULL;
    if (owl_http_executeur_traite_une(x, 0) != 1) return NULL;
    OwlHttpRep* rep = NULL;
    if (owl_http_executeur_attend(x, j, &rep) != OWL_HTTP_EXEC_OK) return NULL;
    return rep;
}

/* ================================================================
 * FAMILLE 0 — cablage deterministe (compile -> registre -> invocation)
 * ================================================================ */

static void famille_0_cablage_deterministe(void) {
    g_famille = 0;

    /* Registre service (compile-time + runtime) frais : sans reset, un service
     * compile par une famille precedente ferait "deja declare" ici. */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE;
    cfg.service_mode       = 1;   /* routage runtime, pas de procedure fixe */

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "cree");
    if (!x) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "service_compile_dans_la_vm");

    /* (A)+(B) : compiler surRequete a peuple le registre RUNTIME. Sans les deux
     * routes ici, tout le reste ne prouverait rien. */
    CHECK(owl_service_runtime_count() >= 2, "deux_routes_enregistrees");
    CHECK(owl_service_runtime_find("GET", "/ping") != NULL, "route_get_ping_resolue");
    CHECK(owl_service_runtime_find("POST", "/ressource") != NULL,
          "route_post_ressource_resolue");

    /* CONTROLE NEGATIF direct sur le routeur : une cible inconnue ne resout
     * rien, et une bonne cible sous le mauvais verbe non plus. */
    CHECK(owl_service_runtime_find("GET", "/inconnu") == NULL,
          "route_inconnue_non_resolue");
    CHECK(owl_service_runtime_find("POST", "/ping") == NULL,
          "route_bon_chemin_mauvais_verbe_non_resolue");

    /* Route 1 : GET /ping -> handler invoque, statut+corps issus de MoiMême. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/ping", NULL);
        CHECK(rep != NULL, "ping_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "ping_statut_200_via_moimeme");
            CHECK(strcmp(s.d, "pong") == 0, "ping_corps_pong");
            owl_http_rep_libere(rep);
        }
    }

    /* Route 2 : POST /ressource -> MoiMême..CodeEtat = httpCree pilote le 201.
     * Un statut distinct de 200 prouve qu'il vient de la reponse, pas d'un
     * defaut code en dur. Un corps distinct prouve que le routage discrimine. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/ressource", NULL);
        CHECK(rep != NULL, "ressource_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "ressource_statut_201_via_moimeme");
            CHECK(strcmp(s.d, "cree") == 0, "ressource_corps_cree");
            owl_http_rep_libere(rep);
        }
    }

    /* CONTROLE NEGATIF servi : route non resolue -> 404, handler jamais
     * invoque. Si owl_service_runtime_find matchait tout, ce test rougirait
     * (200/201 au lieu de 404). */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/inconnu", NULL);
        CHECK(rep != NULL, "inconnu_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 404, "route_non_resolue_404");
            owl_http_rep_libere(rep);
        }
    }

    /* Bon chemin, mauvais verbe : meme verdict negatif. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/ping", NULL);
        CHECK(rep != NULL, "mauvais_verbe_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 404, "mauvais_verbe_404");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);
}

/* Cherche un en-tete par nom (casse-insensible) dans une reponse. NULL si
 * absent. */
static const char* trouve_entete(const OwlHttpRep* rep, const char* nom) {
    size_t n = owl_http_rep_nb_entetes(rep);
    for (size_t i = 0; i < n; i++) {
        const char* k = owl_http_rep_entete_nom(rep, i);
        if (k && strcasecmp(k, nom) == 0) return owl_http_rep_entete_valeur(rep, i);
    }
    return NULL;
}

/* ================================================================
 * FAMILLE 2 — binding params (, chantier 3) : ROUGE ATTENDU
 *
 * Le harnais de service route et invoque deja un handler SANS
 * parametre. Des qu'un `surRequete` declare des parametres, l'appel actuel
 * (`vm_call_global`, toujours 0 argument) echoue par arite -> 500. Et une
 * route dont le chemin porte un parametre n'est meme pas resolue par
 * `owl_service_runtime_find` (hasParam ecarte) -> 404. C'est exactement le
 * vide que ce chantier comble : chaque assertion ci-dessous encode la
 * valeur ATTENDUE APRES implementation ; elle doit rougir aujourd'hui.
 *
 * Deux groupes d'exception, clairement annotes : les baselines heritees de
 *  (routes sans parametre, deja vertes) et le garde-fou repli
 * ASCII (assertion NEGATIVE, vraie aujourd'hui par absence totale de
 * routage parametre/casse, et qui doit RESTER vraie apres implementation).
 * ================================================================ */

static void famille_2_binding_params(void) {
    g_famille = 2;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_BINDING;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "binding_cree");
    if (!x) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "binding_service_compile_dans_la_vm");

    /* Substrat : la compilation doit avoir enregistre les 6 routes, dont
     * celles a parametre marquees hasParam=1 (deja vrai depuis  —
     * la VALIDATION de compile-time est committee ; seule l'INVOCATION
     * reste a batir). */
    CHECK(owl_service_runtime_count() >= 6, "binding_six_routes_enregistrees");

    /* --- Groupe A : route litterale vs route parametree (criteres 1 et 6) --- */
    {
        /* Baseline DEJA VERTE : route sans parametre, temoin
         * que la coexistence litteral+parametre ne casse pas l'existant. */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/clients/actifs", NULL);
        CHECK(rep != NULL, "actifs_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "actifs_statut_200_baseline");
            CHECK(strcmp(s.d, "liste-actifs") == 0, "actifs_corps_baseline");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* ROUGE : id=42 lie depuis le chemin -> 200/"id-42". Aujourd'hui la
         * route parametree n'est pas resolue (hasParam ecarte) -> 404. */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/clients/42", NULL);
        CHECK(rep != NULL, "id42_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "id42_statut_200_moimeme_dynamique");
            CHECK(strcmp(s.d, "id-42") == 0, "id42_corps_echo_chemin");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* ROUGE : id=123 (coercion souple "123"->entier depuis le segment
         * texte de l'URL) -> branche SINON -> 201/"id-autre". Prouve a la
         * fois la coercion et que CodeEtat varie vraiment selon la donnee
         * (pas code en dur). */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/clients/123", NULL);
        CHECK(rep != NULL, "id123_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "id123_statut_201_moimeme_dynamique");
            CHECK(strcmp(s.d, "id-autre") == 0, "id123_corps_echo_chemin");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe B : corps JSON type -> parametre structure (critere 2) --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/clients",
                                            "{\"nom\":\"Alice\"}");
        CHECK(rep != NULL, "post_valide_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "post_valide_statut_201");
            CHECK(strcmp(s.d, "Alice") == 0, "post_valide_corps_desserialise");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* Adversarial : champ en trop dans le JSON -> ignore, bind reussit
         * quand meme (comportement permissif deja etabli par
         * JSONDeserialise sur le reste du langage). */
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/clients",
                                            "{\"nom\":\"Alice\",\"extra\":123}");
        CHECK(rep != NULL, "post_champ_extra_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "post_champ_extra_statut_201");
            CHECK(strcmp(s.d, "Alice") == 0, "post_champ_extra_corps_ignore_extra");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe C : corps malforme -> 400 AVANT la logique metier (critere 3) --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/clients", "{oups");
        CHECK(rep != NULL, "post_malforme_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 400, "post_malforme_statut_400");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe C2 : negociation Content-Type du corps structure ---
     * Un parametre de type structure attend un corps JSON. Le Content-Type
     * entrant arbitre : absent / application/json -> on tente le parse ; type
     * non-JSON explicite -> 415 SANS parse ; JSON mais corps malforme -> 400.
     * Les deux rejets portent desormais un corps de message non vide (plus de
     * 400/415 muet) qui cite le type attendu. */
    {
        /* CT non-JSON explicite -> 415, AUCUN parse (le corps est pourtant du
         * JSON valide : c'est le Content-Type qui tranche, pas le contenu). */
        OwlHttpRep* rep = sert_deterministe_ct(x, "POST", "/clients",
                                               "{\"nom\":\"Alice\"}", "text/plain");
        CHECK(rep != NULL, "ct_texte_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 415, "ct_texte_statut_415");
            CHECK(s.len > 0, "ct_texte_corps_non_vide");
            CHECK(strstr(s.d, "stClientBinding") != NULL,
                  "ct_texte_corps_cite_le_type_attendu");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* CT formulaire -> 415 aussi (non-JSON explicite). */
        OwlHttpRep* rep = sert_deterministe_ct(x, "POST", "/clients",
                                               "nom=Alice",
                                               "application/x-www-form-urlencoded");
        CHECK(rep != NULL, "ct_form_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 415, "ct_form_statut_415");
            CHECK(owl_http_rep_longueur(rep) > 0, "ct_form_corps_non_vide");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* CT application/json AVEC parametre (; charset=utf-8) -> toujours JSON :
         * corps valide -> 201 (les parametres du media type sont ignores). */
        OwlHttpRep* rep = sert_deterministe_ct(x, "POST", "/clients",
                                               "{\"nom\":\"Carol\"}",
                                               "application/json; charset=utf-8");
        CHECK(rep != NULL, "ct_json_charset_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "ct_json_charset_statut_201");
            CHECK(strcmp(s.d, "Carol") == 0, "ct_json_charset_corps_desserialise");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* CT application/json + corps malforme -> 400 avec message non vide
         * citant le type attendu (plus de 400 muet). */
        OwlHttpRep* rep = sert_deterministe_ct(x, "POST", "/clients",
                                               "hello", "application/json");
        CHECK(rep != NULL, "ct_json_malforme_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 400, "ct_json_malforme_statut_400");
            CHECK(s.len > 0, "ct_json_malforme_corps_non_vide");
            CHECK(strstr(s.d, "stClientBinding") != NULL,
                  "ct_json_malforme_corps_cite_le_type_attendu");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* CT ABSENT + corps malforme -> 400 avec message non vide (le 400 muet
         * historique disparait meme sans Content-Type). */
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/clients", "hello");
        CHECK(rep != NULL, "ct_absent_malforme_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 400, "ct_absent_malforme_statut_400");
            CHECK(owl_http_rep_longueur(rep) > 0, "ct_absent_malforme_corps_non_vide");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe D : ordre des parametres libre (critere 5) ---
     * `client` est declare AVANT `id` dans surRequete, alors que `id` est
     * un segment de chemin et `client` le corps : la liaison doit rester
     * par nom, insensible a la position de declaration. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "PUT", "/clients/42",
                                            "{\"nom\":\"Bob\"}");
        CHECK(rep != NULL, "ordre_libre_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "ordre_libre_statut_200");
            CHECK(strcmp(s.d, "Bob") == 0, "ordre_libre_corps_malgre_ordre_inverse");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe E : query optionnelle/defaut + query en trop (adversarial) --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/recherche", NULL);
        CHECK(rep != NULL, "query_defaut_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "query_defaut_statut_200");
            CHECK(strcmp(s.d, "nom") == 0, "query_defaut_valeur_par_defaut_utilisee");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/recherche?tri=age", NULL);
        CHECK(rep != NULL, "query_fournie_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "query_fournie_statut_200");
            CHECK(strcmp(s.d, "age") == 0, "query_fournie_ecrase_le_defaut");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* Adversarial : une query NON declaree en plus de celle attendue ->
         * ignoree, ne casse pas le bind. */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/recherche?tri=age&debug=1", NULL);
        CHECK(rep != NULL, "query_en_trop_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "query_en_trop_statut_200");
            CHECK(strcmp(s.d, "age") == 0, "query_en_trop_ignoree");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe F : redirection 308 canonique sur casse ASCII (critere 7) --- */
    {
        /* Casse divergente sur le segment litteral "Clients" -> 308 vers la
         * forme canonique enregistree "/clients/actifs". */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/Clients/actifs", NULL);
        CHECK(rep != NULL, "casse_prefixe_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 308, "casse_prefixe_statut_308");
            const char* loc = trouve_entete(rep, "Location");
            CHECK(loc != NULL && strcmp(loc, "/clients/actifs") == 0,
                  "casse_prefixe_location_canonique");
            owl_http_rep_libere(rep);
        }
    }
    {
        /* Casse divergente sur le DERNIER segment litteral "ACTIFS". */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/clients/ACTIFS", NULL);
        CHECK(rep != NULL, "casse_suffixe_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 308, "casse_suffixe_statut_308");
            const char* loc = trouve_entete(rep, "Location");
            CHECK(loc != NULL && strcmp(loc, "/clients/actifs") == 0,
                  "casse_suffixe_location_canonique");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Groupe G : garde-fou repli ASCII PUR (critere 7, negatif) ---
     * "café" (segment accentue enregistre en minuscule) sert de baseline
     * (deja verte,  : un segment UTF-8 litteral exact matche deja).
     * La requete majuscule-accentuee "CAFÉ" NE DOIT JAMAIS se rapprocher du
     * segment minuscule par un fold hors-ASCII : ni 200 (deviner la route),
     * ni 308 (rediriger vers elle). Cette assertion est deliberement VRAIE
     * DES AUJOURD'HUI (aucun routage parametre/casse n'existe encore) et
     * DOIT LE RESTER apres implementation : un garde-fou de non-regression,
     * pas un rouge attendu. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/caf\xc3\xa9", NULL);
        CHECK(rep != NULL, "cafe_minuscule_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "cafe_minuscule_statut_200_baseline");
            CHECK(strcmp(s.d, "menu-cafe") == 0, "cafe_minuscule_corps_baseline");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/CAF\xc3\x89", NULL);
        CHECK(rep != NULL, "cafe_majuscule_reponse_rendue");
        if (rep) {
            CHECK(owl_http_rep_statut(rep) == 404, "cafe_majuscule_404_jamais_de_fold_hors_ascii");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 1 — bout en bout sur socket loopback (patron joue_e2e)
 * ================================================================ */

typedef struct {
    int    statut;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} EchangeE2E;

static EchangeE2E joue_e2e(const char* octets, OwlHttpExecuteur* x) {
    EchangeE2E e;
    memset(&e, 0, sizeof(e));

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) return e;

    OwlHttpServeurConfig cfg;
    owl_http_serveur_config_defaut(&cfg);
    cfg.handler_rep      = owl_http_exec_handler_rep;
    cfg.handler_rep_user = x;

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = octets;
    mc.octets_len            = strlen(octets);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture       = 1;
    mc.timeout_lecture_ms        = 5000;
    mc.fermer_apres_ecriture     = 1;
    mc.fermer_envoi_avant_lecture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); return e; }

    e.statut = owl_http_serveur_sert_une_connexion(&l, &cfg, 5000);

    owl_mock_peer_joint(pair);
    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(e.reponse, recu, rl);
    e.reponse[rl] = '\0';
    e.reponse_len = rl;

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);
    return e;
}

static void famille_1_bout_en_bout(void) {
    g_famille = 1;

    /* Reset avant que le thread executeur ne recompile la meme source. */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "thread_executeur_demarre");

    {
        EchangeE2E e = joue_e2e("GET /ping HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "e2e_statut_200");
        CHECK(strstr(e.reponse, "HTTP/1.1 200 OK\r\n") == e.reponse,
              "e2e_ligne_de_statut");
        CHECK(strstr(e.reponse, "\r\n\r\npong") != NULL,
              "e2e_corps_produit_par_le_handler");
    }

    /* Un statut non trivial (201) traverse aussi le transport. */
    {
        EchangeE2E e = joue_e2e("POST /ressource HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 201, "e2e_post_statut_201");
        CHECK(strstr(e.reponse, "\r\n\r\ncree") != NULL,
              "e2e_post_corps_cree");
    }

    /* Route inconnue servie sur socket -> 404. */
    {
        EchangeE2E e = joue_e2e("GET /inconnu HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 404, "e2e_route_inconnue_404");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 3 — crash handler -> 500 propre, VM survit (, lot 1)
 *
 * Patron e2e socket (identique a joue_e2e ci-dessus) : chaque crash est
 * suivi immediatement d'une requete /ok pour prouver que le thread executeur
 * (meme instance x, meme thread VM) sert encore correctement APRES un
 * handler qui a plante. Si la VM restait dans un etat corrompu (frameCount
 * ou stackTop pas remis a plat, event_sender desequilibre), la requete /ok
 * suivante rougirait ou le process bloquerait/crasherait.
 * ================================================================ */

static void famille_3_crash_handler(void) {
    g_famille = 3;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_CRASH;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "crash_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "crash_thread_executeur_demarre");

    /* Temoin AVANT tout crash : /ok repond normalement. */
    {
        EchangeE2E e = joue_e2e("GET /ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "avant_crash_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nok") != NULL, "avant_crash_corps_ok");
    }

    /* --- Div0 : division entiere par zero --- */
    {
        EchangeE2E e = joue_e2e("GET /div0 HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 500, "div0_statut_500");
        CHECK(strstr(e.reponse, "jamais_atteint") == NULL,
              "div0_corps_metier_absent");
        CHECK(strstr(e.reponse, "DIV_BY_ZERO") == NULL &&
              strstr(e.reponse, "zéro") == NULL && strstr(e.reponse, "zero") == NULL,
              "div0_pas_de_fuite_message_erreur");
    }
    {
        /* VM survit : /ok repond encore normalement juste apres. */
        EchangeE2E e = joue_e2e("GET /ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "apres_div0_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nok") != NULL, "apres_div0_corps_ok");
    }

    /* --- IndexOOB : indice de tableau hors bornes --- */
    {
        EchangeE2E e = joue_e2e("GET /indexoob HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 500, "indexoob_statut_500");
        CHECK(strstr(e.reponse, "jamais_atteint") == NULL,
              "indexoob_corps_metier_absent");
        CHECK(strstr(e.reponse, "ARRAY_INDEX") == NULL &&
              strstr(e.reponse, "hors bornes") == NULL &&
              strstr(e.reponse, "hors limites") == NULL,
              "indexoob_pas_de_fuite_message_erreur");
    }
    {
        EchangeE2E e = joue_e2e("GET /ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "apres_indexoob_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nok") != NULL, "apres_indexoob_corps_ok");
    }

    /* --- Indirect : {var} vers un nom introuvable --- */
    {
        EchangeE2E e = joue_e2e("GET /indirect HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 500, "indirect_statut_500");
        CHECK(strstr(e.reponse, "jamais_atteint") == NULL,
              "indirect_corps_metier_absent");
        CHECK(strstr(e.reponse, "variableIntrouvableZZ") == NULL,
              "indirect_pas_de_fuite_nom_variable");
    }
    {
        /* Dernier temoin : la VM a survecu aux TROIS crashs successifs. */
        EchangeE2E e = joue_e2e("GET /ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "apres_trois_crashs_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nok") != NULL, "apres_trois_crashs_corps_ok");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 4 — RENVOYER = corps par defaut
 * ================================================================ */

static void famille_4_renvoyer_corps(void) {
    g_famille = 4;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_RENVOYER;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "renvoyer_cree");
    if (!x) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "renvoyer_service_compile_dans_la_vm");

    /* --- Cas 1 : RENVOYER chaine, aucun Corps pose -> corps = "bonjour",
     * Content-Type text/plain. ROUGE ATTENDU sur le corps aujourd'hui (la
     * valeur de retour est jetee, Corps jamais pose -> corps vide). Le
     * Content-Type est deja text/plain aujourd'hui, mais par COINCIDENCE
     * (c'est le defaut quand rien n'est pose) : cette assertion-la reste
     * verte avant ET apres impl, elle ne prouve rien a elle seule ; seule
     * l'assertion de corps verrouille le fix. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/texte", NULL);
        CHECK(rep != NULL, "texte_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "texte_statut_200");
            CHECK(strcmp(s.d, "bonjour") == 0,
                  "texte_corps_vient_du_renvoyer");   /* ROUGE ATTENDU */
            const char* ct = owl_http_rep_type_contenu(rep);
            CHECK(ct != NULL && strncmp(ct, "text/plain", 10) == 0,
                  "texte_content_type_text_plain");   /* vert par coincidence aujourd'hui */
            owl_http_rep_libere(rep);
        }
    }

    /* --- Cas 2 : RENVOYER structure -> JSON, Content-Type application/json.
     * ROUGE ATTENDU sur les deux assertions aujourd'hui (corps vide, type
     * text/plain par defaut). La forme JSON exacte n'est pas figee ici :
     * on verifie par sous-chaines (cle + valeur), robuste a l'espacement /
     * l'indentation exacts du serialiseur (cf. JSONSérialise, stdlib_json.c
     * value_to_json). */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/structure", NULL);
        CHECK(rep != NULL, "structure_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "structure_statut_200");
            CHECK(strstr(s.d, "\"nom\"") != NULL && strstr(s.d, "Alice") != NULL,
                  "structure_corps_json_champ_nom");        /* ROUGE ATTENDU */
            CHECK(strstr(s.d, "\"age\"") != NULL && strstr(s.d, "30") != NULL,
                  "structure_corps_json_champ_age");        /* ROUGE ATTENDU */
            const char* ct = owl_http_rep_type_contenu(rep);
            CHECK(ct != NULL && strncmp(ct, "application/json", 16) == 0,
                  "structure_content_type_application_json"); /* ROUGE ATTENDU */
            owl_http_rep_libere(rep);
        }
    }

    /* --- Cas 3 : MoiMeme..Reponse..Corps pose ET RENVOYER une autre valeur ->
     * l'override gagne. DEJA VERT aujourd'hui : Corps est deja lu
     * directement, RENVOYER est jete dans les deux cas (avant/apres impl,
     * ce test doit rester vert -> garde-fou non-regression).
     *
     * NOTE : handler deliberement SANS type de retour declare (`: chaine`).
     * Avec un type declare, `MoiMeme..Reponse..Corps = ...` pose AVANT le `RENVOYER`
     * n'est PLUS honore par service_lit_champ (bug adjacent decouvert en
     * ecrivant ce cas, hors-perimetre  -- signale au coordinator,
     * pas fixe ici). Ce cas teste la semantique d'override, pas ce bug :
     * le handler reste untyped pour l'isoler. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/override", NULL);
        CHECK(rep != NULL, "override_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "override_statut_200");
            CHECK(strcmp(s.d, "explicite") == 0,
                  "override_corps_gagne_sur_renvoyer");   /* deja vert (non-regression) */
            owl_http_rep_libere(rep);
        }
    }

    /* --- Cas 4 : MoiMeme..Reponse..TypeContenu pose explicitement (override du
     * content-type deduit), RENVOYER une chaine sans Corps pose. Le corps
     * ROUGIT (valeur de retour jetee) ; le content-type est DEJA VERT
     * aujourd'hui (TypeContenu est lu et applique independamment du corps,
     * feature deja cablee, hors-perimetre du bug RENVOYER).
     *
     * NOTE : handler deliberement SANS type de retour declare, meme raison
     * que le cas 3 (bug adjacent MoiMeme..X + type de retour declare). */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/typeforce", NULL);
        CHECK(rep != NULL, "typeforce_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "typeforce_statut_200");
            CHECK(strcmp(s.d, "<a/>") == 0,
                  "typeforce_corps_vient_du_renvoyer");   /* ROUGE ATTENDU */
            const char* ct = owl_http_rep_type_contenu(rep);
            CHECK(ct != NULL && strncmp(ct, "application/xml", 15) == 0,
                  "typeforce_content_type_override_explicite"); /* deja vert */
            owl_http_rep_libere(rep);
        }
    }

    /* --- Cas 5 : RENVOYER Null, CodeEtat pose explicitement -> corps vide,
     * pas de crash, statut pilote par CodeEtat. DEJA VERT aujourd'hui (le
     * retour est de toute facon jete, Corps jamais pose -> corps vide par
     * defaut) ; doit RESTER vert apres impl (RENVOYER Null = corps vide,
     * pas une chaine "Null" ni un crash). Temoin de survie immediat apres
     * (meme executeur, meme VM) : /texte doit repondre encore normalement. */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/vide", NULL);
        CHECK(rep != NULL, "vide_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "vide_statut_200_via_codeetat");
            CHECK(s.len == 0, "vide_corps_vide_pas_de_crash");   /* deja vert */
            owl_http_rep_libere(rep);
        }
    }
    {
        /* VM toujours saine apres RENVOYER Null : /texte re-sert un corps
         * non vide (ROUGE ATTENDU pour la meme raison que le cas 1). */
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/texte", NULL);
        CHECK(rep != NULL, "apres_vide_texte_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(strcmp(s.d, "bonjour") == 0,
                  "apres_vide_texte_corps_toujours_correct");   /* ROUGE ATTENDU */
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 5 — reason-phrase de la ligne de statut
 *
 * Chaque route pose un CodeEtat different ; on assert la ligne
 * "HTTP/1.1 <code> <raison>\r\n" EXACTE, ecrite sur le socket via
 * joue_e2e -> owl_http_serveur_sert_une_connexion -> raison(). 200
 * sert de temoin non-regression ; les autres etaient "Error" avant.
 * ================================================================ */

static void assert_ligne_statut(OwlHttpExecuteur* x, const char* chemin,
                                int statut_attendu, const char* ligne_attendue,
                                const char* label) {
    char req[128];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", chemin);
    EchangeE2E e = joue_e2e(req, x);
    compte(e.statut == statut_attendu, label, __LINE__);
    compte(strstr(e.reponse, ligne_attendue) == e.reponse, label, __LINE__);
}

static void famille_5_reason_phrase(void) {
    g_famille = 5;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_RAISONS;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "cree_raisons"); return; }
    CHECK(owl_http_executeur_demarre(x), "thread_executeur_raisons_demarre");

    /* Temoin non-regression : 200 OK reste bon. */
    assert_ligne_statut(x, "/r200", 200, "HTTP/1.1 200 OK\r\n", "raison_200_ok");
    /* Rouges avant fix (servaient "Error"). */
    assert_ligne_statut(x, "/r201", 201, "HTTP/1.1 201 Created\r\n", "raison_201_created");
    assert_ligne_statut(x, "/r202", 202, "HTTP/1.1 202 Accepted\r\n", "raison_202_accepted");
    assert_ligne_statut(x, "/r401", 401, "HTTP/1.1 401 Unauthorized\r\n", "raison_401_unauthorized");
    assert_ligne_statut(x, "/r403", 403, "HTTP/1.1 403 Forbidden\r\n", "raison_403_forbidden");
    assert_ligne_statut(x, "/r409", 409, "HTTP/1.1 409 Conflict\r\n", "raison_409_conflict");

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 6 — contexte Requete/Reponse imbrique
 * ================================================================ */

static void famille_6_contexte_requete_reponse(void) {
    g_famille = 6;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_CONTEXTE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "contexte_cree");
    if (!x) return;

    vm_init();
    /* ROUGE ATTENDU global : Reponse/Requete pas encore reconnus au 1er
     * niveau de MoiMeme -> compilation de surRequete echoue -> prepare_vm
     * rend faux. Les CHECK plus bas dependent de rep != NULL et ne
     * comptent donc rien tant que ceci reste faux -- normal, c'est le
     * meme patron que les familles precedentes (0/2/4). */
    CHECK(owl_http_executeur_prepare_vm(x), "contexte_service_compile_dans_la_vm");

    /* --- Reponse (ecriture) + Requete (lecture Methode/Chemin) --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/meta", NULL);
        CHECK(rep != NULL, "meta_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "meta_statut_200_via_reponse_imbriquee");
            CHECK(strcmp(s.d, "GET|/meta") == 0,
                  "meta_corps_reflete_methode_et_chemin_via_requete_imbriquee");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Identite : Null par defaut, ecrit puis relu dans le meme handler --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/identite", NULL);
        CHECK(rep != NULL, "identite_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "identite_statut_200");
            CHECK(strcmp(s.d, "avant:null|apres:user-42") == 0,
                  "identite_slot_null_par_defaut_puis_ecrit_relu");
            owl_http_rep_libere(rep);
        }
    }

    /* --- Piege  : deux affectations MoiMeme..Reponse..X
     * consecutives (CodeEtat puis TypeContenu, sans Corps entre les deux)
     * doivent TOUTES DEUX prendre effet. Le tracker de chaine
     * g_last_var_token fuyait entre deux `MoiMeme..prop = const`
     * (decouvert sur la fondation ). --- */
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/double", NULL);
        CHECK(rep != NULL, "double_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200,
                  "double_statut_200_premiere_affectation_prise_en_compte");
            const char* ct = owl_http_rep_type_contenu(rep);
            CHECK(ct != NULL && strncmp(ct, "text/plain", 10) == 0,
                  "double_type_contenu_deuxieme_affectation_prise_en_compte");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);

    /* --- Forme bilingue EN, memes assertions (statut + echo methode) --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 4;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_CONTEXTE_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    CHECK(x_en != NULL, "contexte_en_cree");
    if (!x_en) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x_en), "contexte_en_service_compile_dans_la_vm");
    {
        OwlHttpRep* rep = sert_deterministe(x_en, "GET", "/ping", NULL);
        CHECK(rep != NULL, "contexte_en_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "contexte_en_statut_200_via_response_imbriquee");
            CHECK(strcmp(s.d, "GET") == 0,
                  "contexte_en_corps_reflete_methode_via_request_imbriquee");
            owl_http_rep_libere(rep);
        }
    }
    vm_free();
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * FAMILLE 7 — surAuthentification (, lot 1) : ROUGE ATTENDU
 * ================================================================ */

static void famille_7_authentification(void) {
    g_famille = 7;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "auth_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "auth_thread_executeur_demarre");

    /* --- Vrai -> passe, Identite posee par le hook relisible dans
     * surRequete. Prouve aussi l'extraction Bearer exacte (positif) :
     * seul le jeton nu, sans le prefixe "Bearer ", matche "bon-jeton". --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer bon-jeton\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_vrai_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmetier:user-42") != NULL,
              "auth_vrai_identite_relue_dans_surrequete");
    }

    /* --- Faux + code explicite pose AVANT le retour -> court-circuite
     * AVANT surRequete. 403 choisi (pas le 401 du fail-closed) pour que
     * cette assertion ne puisse pas coincider avec le defaut ci-dessous. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer mauvais-explicite\r\n\r\n", x);
        CHECK(e.statut == 403, "auth_faux_code_explicite_statut_403");
        CHECK(strstr(e.reponse, "metier:") == NULL,
              "auth_faux_code_explicite_surrequete_non_execute");
    }

    /* --- Faux SANS aucun code pose -> 401 par defaut. LE test de securite
     * cle (fail-closed) : un hook qui oublie de poser un code ne doit
     * jamais laisser passer la requete. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer autre-chose\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_faux_sans_code_401_fail_closed");
        CHECK(strstr(e.reponse, "metier:") == NULL,
              "auth_faux_sans_code_surrequete_non_execute");
    }

    /* --- Controle NEGATIF d'extraction : en-tete SANS le prefixe "Bearer "
     * ne doit JAMAIS etre pris tel quel comme credential (sinon la valeur
     * brute "bon-jeton" matcherait par accident et authentifierait). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: bon-jeton\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_sans_prefixe_bearer_jamais_pris_tel_quel");
    }

    /* --- En-tete Authorization absent -> credential vide, le hook est
     * QUAND MEME invoque (pas de court-circuit magique avant le hook) ;
     * l'app rejette une chaine vide -> 401. --- */
    {
        EchangeE2E e = joue_e2e("GET /protegee HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_entete_absent_credential_vide_rejete_401");
    }

    /* --- Opt-out httpAuthAucune sur l'operation -> le hook n'est jamais
     * invoque, la requete passe meme sans le moindre credential. --- */
    {
        EchangeE2E e = joue_e2e("GET /publique HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_optout_statut_200_sans_credential");
        CHECK(strstr(e.reponse, "\r\n\r\npublic-ok") != NULL,
              "auth_optout_corps_public_ok");
    }

    /* --- REGRESSION  : Bearer LONG (1000 octets, > 512) doit
     * arriver ENTIER a surAuthentification. Le hook n'autorise que sur
     * `Taille(jeton) = 1000` -- il ne peut PAS matcher par accident sur un
     * jeton tronque/vide. Aujourd'hui, `cred[512]` (http_exec.c) plafonne
     * le credential a 511 octets -> credential vide -> hook rejette -> 401.
     * ROUGE ATTENDU pour cette raison precise, PAS pour une erreur de
     * syntaxe/compile. --- */
    {
        const size_t nJeton = 1000;
        char* jetonLong = (char*)malloc(nJeton + 1);
        CHECK(jetonLong != NULL, "auth_longue_alloc_jeton");
        if (jetonLong) {
            memset(jetonLong, 'a', nJeton);
            jetonLong[nJeton] = '\0';

            const char* prefixe = "GET /sonde-longue HTTP/1.1\r\nHost: a\r\n"
                                   "Authorization: Bearer ";
            const char* suffixe = "\r\n\r\n";
            size_t nReq = strlen(prefixe) + nJeton + strlen(suffixe) + 1;
            char* req = (char*)malloc(nReq);
            CHECK(req != NULL, "auth_longue_alloc_requete");
            if (req) {
                snprintf(req, nReq, "%s%s%s", prefixe, jetonLong, suffixe);

                EchangeE2E e = joue_e2e(req, x);
                CHECK(e.statut == 200,
                      "auth_bearer_long_1000_octets_statut_200_non_tronque");
                CHECK(strstr(e.reponse, "\r\n\r\nrecu_taille:1000") != NULL,
                      "auth_bearer_long_1000_octets_taille_integrale_relue");

                free(req);
            }
            free(jetonLong);
        }
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- Forme bilingue EN complete --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_AUTH_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "auth_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "auth_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer ok-en\r\n\r\n", x_en);
        CHECK(e.statut == 200, "auth_en_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nen-user") != NULL,
              "auth_en_identity_relue_via_myself_request_identity");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * FAMILLE 8 — en-tetes de reponse dynamiques : ROUGE ATTENDU
 *
 * "Entetes"/"Headers" ne sont pas encore dans la liste blanche 2e niveau
 * de MoiMeme..Reponse (service_ctx_response_sub_canon, compiler.c:8782-
 * 8790) -> la compilation de surRequete echoue -> le montage repond en
 * echec sur toutes les routes -> chaque CHECK ci-dessous rougit. Meme
 * patron que les familles 0/2/4/6/7 : le rouge est porte par le COMPILE,
 * pas par une assertion trivialement fausse.
 *
 * joue_e2e (socket reel) est le SEUL harnais utilise : c'est le seul moyen
 * fiable de prouver a la fois l'emission ET la non-injection CRLF, sur les
 * octets qui ont VRAIMENT traverse le transport (pas un objet OwlHttpRep
 * inspecte in-process).
 * ================================================================ */

/* Marche les lignes d'en-tete de la reponse HTTP brute (entre la ligne de
 * statut et la ligne vide) et rend la valeur du premier en-tete dont le NOM
 * matche exactement `nom` (casse-insensible). 0 si absent. */
static int entete_ligne_valeur(const char* reponse, const char* nom,
                               char* dst, size_t cap) {
    dst[0] = '\0';
    const char* p = strstr(reponse, "\r\n");
    if (!p) return 0;
    p += 2;
    size_t nlen = strlen(nom);
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') return 0;
        const char* fin = strstr(p, "\r\n");
        if (!fin) return 0;
        if ((size_t)(fin - p) > nlen && p[nlen] == ':' &&
#ifdef _WIN32
            _strnicmp(p, nom, nlen) == 0
#else
            strncasecmp(p, nom, nlen) == 0
#endif
        ) {
            const char* v = p + nlen + 1;
            while (v < fin && (*v == ' ' || *v == '\t')) v++;
            size_t n = (size_t)(fin - v);
            if (n >= cap) n = cap - 1;
            memcpy(dst, v, n);
            dst[n] = '\0';
            return 1;
        }
        p = fin + 2;
    }
    return 0;
}

/* Compte les lignes d'en-tete dont le NOM matche exactement `nom` (casse-
 * insensible). Sert a verrouiller "pas de doublon" (collision Content-Type). */
static int compte_occurrences_entete(const char* reponse, const char* nom) {
    int n = 0;
    const char* p = strstr(reponse, "\r\n");
    if (!p) return 0;
    p += 2;
    size_t nlen = strlen(nom);
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') break;
        const char* fin = strstr(p, "\r\n");
        if (!fin) break;
        if ((size_t)(fin - p) > nlen && p[nlen] == ':' &&
#ifdef _WIN32
            _strnicmp(p, nom, nlen) == 0
#else
            strncasecmp(p, nom, nlen) == 0
#endif
        ) n++;
        p = fin + 2;
    }
    return n;
}

/* Compte les lignes d'en-tete dont le NOM COMMENCE par `prefixe` (casse-
 * insensible). Sert a compter un groupe d'en-tetes generiques (X-H01..17)
 * sans dependre de l'ordre d'insertion d'un tableau associatif. */
static int compte_occurrences_prefixe(const char* reponse, const char* prefixe) {
    int n = 0;
    const char* p = strstr(reponse, "\r\n");
    if (!p) return 0;
    p += 2;
    size_t plen = strlen(prefixe);
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') break;
        const char* fin = strstr(p, "\r\n");
        if (!fin) break;
        if ((size_t)(fin - p) > plen &&
#ifdef _WIN32
            _strnicmp(p, prefixe, plen) == 0
#else
            strncasecmp(p, prefixe, plen) == 0
#endif
        ) n++;
        p = fin + 2;
    }
    return n;
}

static void famille_8_entetes_reponse(void) {
    g_famille = 8;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_ENTETES;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "entetes_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "entetes_thread_executeur_demarre");

    /* --- 1. Pose + emission : un en-tete pose dans surRequete atterrit
     * ON-THE-WIRE. --- */
    {
        EchangeE2E e = joue_e2e("GET /un HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "un_statut_200");
        CHECK(strstr(e.reponse, "Cache-Control: no-store\r\n") != NULL,
              "un_entete_emis_sur_le_fil");
    }

    /* --- 2. Plusieurs en-tetes poses -> tous emis. --- */
    {
        EchangeE2E e = joue_e2e("GET /plusieurs HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "plusieurs_statut_200");
        CHECK(strstr(e.reponse, "X-Un: un\r\n") != NULL, "plusieurs_x_un_emis");
        CHECK(strstr(e.reponse, "X-Deux: deux\r\n") != NULL, "plusieurs_x_deux_emis");
        CHECK(strstr(e.reponse, "X-Trois: trois\r\n") != NULL, "plusieurs_x_trois_emis");
    }

    /* --- 3. SECURITE -- injection CRLF refusee (RFC 9110). Le point de
     * passage unique owl_http_rep_ajoute_entete refuse une valeur qui porte
     * un CR/LF : la reponse ne doit JAMAIS contenir un en-tete "Evil:"
     * fabrique par scission de ligne. C'EST LE TEST DE SECURITE CLE. --- */
    {
        EchangeE2E e = joue_e2e("GET /injection HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "injection_statut_200_pas_de_crash");
        CHECK(strstr(e.reponse, "Evil:") == NULL,
              "injection_crlf_jamais_scinde_en_entete_evil");
    }

    /* --- 5. Lecture symetrique : un en-tete pose plus tot dans le handler
     * est relisible via MoiMeme..Reponse..Entetes["X"]. --- */
    {
        EchangeE2E e = joue_e2e("GET /lecture HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "lecture_statut_200");
        CHECK(strstr(e.reponse, "X-Lu: valeur-posee\r\n") != NULL,
              "lecture_entete_emis");
        CHECK(strstr(e.reponse, "\r\n\r\nlu:valeur-posee") != NULL,
              "lecture_relue_dans_le_meme_handler");
    }

    /* --- 6. Collision Content-Type : TypeContenu est le canal canonique,
     * Entetes["Content-Type"] ne doit JAMAIS produire un doublon. --- */
    {
        EchangeE2E e = joue_e2e("GET /collision HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "collision_statut_200");
        CHECK(compte_occurrences_entete(e.reponse, "Content-Type") == 1,
              "collision_une_seule_ligne_content_type");
        char valeur[128];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", valeur, sizeof(valeur))
                  && strcmp(valeur, "application/json") == 0,
              "collision_typecontenu_canonique_gagne");
    }

    /* --- 7. Forme accent/casse toleree au runtime (..entetes[...]). --- */
    {
        EchangeE2E e = joue_e2e("GET /tolerant HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "tolerant_statut_200");
        CHECK(strstr(e.reponse, "X-Tol: ok-tolerant\r\n") != NULL,
              "tolerant_forme_sans_accent_sans_casse_marche_au_runtime");
    }

    /* --- Edge case proactif : plafond OWL_HTTP_REP_ENTETES_MAX (16).
     * 17 en-tetes poses -> exactement 16 emis, pas de crash. --- */
    {
        EchangeE2E e = joue_e2e("GET /overflow HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "overflow_statut_200_pas_de_crash");
        CHECK(compte_occurrences_prefixe(e.reponse, "X-H") == 16,
              "overflow_plafond_16_entetes_respecte");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- 4. Forme bilingue EN complete (Myself..Response..Headers). --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 4;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_ENTETES_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "entetes_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "entetes_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e("GET /en HTTP/1.1\r\nHost: a\r\n\r\n", x_en);
        CHECK(e.statut == 200, "entetes_en_statut_200");
        CHECK(strstr(e.reponse, "X-Foo: bar\r\n") != NULL,
              "entetes_en_headers_emis_sur_le_fil");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * FAMILLE 9 — hooks de cycle de vie (, lot 1) : ROUGE ATTENDU
 *
 * FinInitialisation/EndInitialization, Fermeture/Closing et surErreur/onError
 * ne sont PAS encore reconnus au niveau service : seul "surAuthentification"/
 * "onAuthentication" passe la whitelist de service_parse_auth_procedure
 * (compiler.c:18294). Chaque source ci-dessous echoue donc a la COMPILATION
 * des le premier hook rencontre (E_SYNTAX_SERVICE_EVENT_UNKNOWN) -> le
 * montage repond en echec sur toutes les routes -> chaque CHECK rougit en
 * cascade. Meme patron que les familles 6/7/8 avant leur feature respective.
 *
 * Patron e2e socket exclusivement (joue_e2e, demarre/arrete) : FinInitialisation
 * et Fermeture sont invoques dans hx_thread_fn (avant/apres la boucle de
 * service), jamais atteints par le harnais deterministe sert_deterministe
 * (prepare_vm seul, pas de thread executeur -- cf. commentaire famille 2).
 * ================================================================ */

static void famille_9_hooks_cycle_de_vie(void) {
    g_famille = 9;

    /* --- FinInitialisation (one-shot) + surErreur (crash handler) --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_HOOKS;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "hooks_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "hooks_thread_executeur_demarre");

    {
        /* 1ere requete : FinInitialisation a deja tourne une fois avant
         * d'accepter la moindre requete -> compteur=1. */
        EchangeE2E e = joue_e2e("GET /etat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "fin_init_1re_requete_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\ninit:1") != NULL,
              "fin_init_compteur_vaut_1_apres_demarrage");
    }
    {
        /* 2e requete : le compteur ne bouge pas -> FinInitialisation n'est
         * PAS reexecute a chaque requete (one-shot au demarrage, pas un
         * hook par-requete). */
        EchangeE2E e = joue_e2e("GET /etat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "fin_init_2e_requete_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\ninit:1") != NULL,
              "fin_init_pas_reexecute_par_requete");
    }

    /* --- surErreur sur crash handler : statut 500 (defaut, pas touche par
     * le hook) + corps custom portant methode et chemin de la requete. --- */
    {
        EchangeE2E e = joue_e2e("GET /crash HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 500, "surerreur_crash_statut_500");
        CHECK(strstr(e.reponse, "erreur_geree:GET|/crash") != NULL,
              "surerreur_corps_custom_avec_methode_et_chemin");
    }

    /* --- surErreur PAS appele sur succes : le corps custom d'erreur n'apparait
     * jamais sur une reponse OK. --- */
    {
        EchangeE2E e = joue_e2e("GET /etat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "surerreur_absent_sur_succes_statut_200");
        CHECK(strstr(e.reponse, "erreur_geree") == NULL,
              "surerreur_pas_invoque_sur_succes");
    }

    /* --- Fermeture : une fois a l'arret, AVANT vm_free. Observabilite PLUS
     * DURE que FinInitialisation (la VM est liberee juste apres) : capture
     * stdout dup/dup2 autour de l'arret du thread executeur (meme patron que
     * multilingue_print_test.c::capture_interpret), Trace(...) servant de
     * temoin C-observable. Calque du mecanisme de teardown projet
     * (ConsoleEcrit dans Terminaison(), tests/compiler/internal_fixtures/
     * run-align-project/case_048), transpose en capture in-process faute de
     * process separe pour un service (pas de subprocess owl serve ici). --- */
    {
        char TMP[128];
        fixture_unique(TMP, sizeof TMP, "_owl_dsl_service_hooks_fermeture_", ".tmp");

        fflush(stdout);
        int saved_fd = owl_dup(owl_fileno(stdout));
        int tmp_fd   = (saved_fd >= 0)
            ? owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644)
            : -1;

        if (saved_fd >= 0 && tmp_fd >= 0) {
            owl_dup2(tmp_fd, owl_fileno(stdout));
            owl_close(tmp_fd);

            owl_http_executeur_arrete(x);   /* Fermeture doit tourner ICI */

            fflush(stdout);
            owl_dup2(saved_fd, owl_fileno(stdout));
            owl_close(saved_fd);

            char buf[512]; buf[0] = '\0';
            FILE* r = fopen(TMP, "rb");
            if (r) {
                size_t n = fread(buf, 1, sizeof(buf) - 1, r);
                buf[n] = '\0';
                fclose(r);
            }
            owl_unlink(TMP);
            CHECK(strstr(buf, "fermeture_hooks_temoin") != NULL,
                  "fermeture_invoquee_une_fois_a_arret");
        } else {
            if (saved_fd >= 0) owl_close(saved_fd);
            CHECK(0, "fermeture_capture_stdout_setup");
            owl_http_executeur_arrete(x);
        }
    }

    owl_http_executeur_libere(x);

    /* --- surErreur qui ne pose rien -> 500 par defaut (fail-safe) --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_noop;
    owl_http_exec_config_defaut(&cfg_noop);
    cfg_noop.file_profondeur    = 8;
    cfg_noop.plafond_attente_ms = 5000;
    cfg_noop.source_owl         = SRC_SERVICE_HOOKS_NOOP_ERROR;
    cfg_noop.service_mode       = 1;

    OwlHttpExecuteur* x_noop = owl_http_executeur_cree(&cfg_noop);
    if (!x_noop) { CHECK(0, "hooks_noop_cree"); }
    else {
        CHECK(owl_http_executeur_demarre(x_noop), "hooks_noop_thread_executeur_demarre");
        {
            EchangeE2E e = joue_e2e("GET /crash HTTP/1.1\r\nHost: a\r\n\r\n", x_noop);
            CHECK(e.statut == 500, "surerreur_noop_statut_500_par_defaut");
        }
        owl_http_executeur_arrete(x_noop);
        owl_http_executeur_libere(x_noop);
    }

    /* --- Forme bilingue EN complete : EndInitialization + Closing + onError --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_HOOKS_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "hooks_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "hooks_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e("GET /state HTTP/1.1\r\nHost: a\r\n\r\n", x_en);
        CHECK(e.statut == 200, "hooks_en_end_initialization_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\ninit:1") != NULL,
              "hooks_en_end_initialization_compteur_1");
    }
    {
        EchangeE2E e = joue_e2e("GET /crash HTTP/1.1\r\nHost: a\r\n\r\n", x_en);
        CHECK(e.statut == 500, "hooks_en_onerror_statut_500");
        CHECK(strstr(e.reponse, "handled:GET|/crash") != NULL,
              "hooks_en_onerror_corps_custom");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);

    /* --- FinInitialisation QUI PLANTE -> demarrage AVORTE (fail-safe) ---
     * Contrat : owl_http_executeur_demarre(x) doit rendre faux. Aujourd'hui
     * RACY (x->vm_prete est signale par hx_thread_fn AVANT l'execution de
     * FinInitialisation, donc le thread appelant peut lire vm_echec=0 avant
     * que le hook n'ait eu la chance de le poser a 1) -- ce test peut donc
     * etre flaky tant que la race n'est pas fermee. Il devient une garde de
     * non-regression fiable une fois le fix pose. --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_init_crash;
    owl_http_exec_config_defaut(&cfg_init_crash);
    cfg_init_crash.file_profondeur    = 4;
    cfg_init_crash.plafond_attente_ms = 5000;
    cfg_init_crash.source_owl         = SRC_SERVICE_HOOKS_INIT_CRASH;
    cfg_init_crash.service_mode       = 1;

    OwlHttpExecuteur* x_init_crash = owl_http_executeur_cree(&cfg_init_crash);
    if (!x_init_crash) { CHECK(0, "hooks_init_crash_cree"); }
    else {
        CHECK(!owl_http_executeur_demarre(x_init_crash),
              "fininit_plante_demarre_rend_faux");
        owl_http_executeur_libere(x_init_crash);
    }

    /* --- Variante bilingue EN : EndInitialization qui plante --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_init_crash_en;
    owl_http_exec_config_defaut(&cfg_init_crash_en);
    cfg_init_crash_en.file_profondeur    = 4;
    cfg_init_crash_en.plafond_attente_ms = 5000;
    cfg_init_crash_en.source_owl         = SRC_SERVICE_HOOKS_INIT_CRASH_EN;
    cfg_init_crash_en.service_mode       = 1;

    OwlHttpExecuteur* x_init_crash_en = owl_http_executeur_cree(&cfg_init_crash_en);
    if (!x_init_crash_en) { CHECK(0, "hooks_init_crash_en_cree"); }
    else {
        CHECK(!owl_http_executeur_demarre(x_init_crash_en),
              "endinit_plante_demarre_rend_faux");
        owl_http_executeur_libere(x_init_crash_en);
    }
}

/* ================================================================
 * FAMILLE 10 — surAuthentification Basique (, lot 2) : ROUGE
 * ATTENDU
 *
 * Bases64 utilisees ici (calculees hors-C, valeurs figees en commentaire
 * pour eviter tout doute sur ce qu'elles encodent) :
 *   "YWxpY2U6c2VjcmV0"  = base64("alice:secret")   -- credential legitime.
 *   "YWxpY2U6d3Jvbmc="  = base64("alice:wrong")    -- mauvais mot de passe.
 *   "YWRtaW4AOng="      = base64("admin\0:x")      -- 8 octets, NUL apres
 *                          "admin" -- LE cas de smuggling : un decodeur qui
 *                          strlen() le resultat verrait "admin" tronque.
 * ================================================================ */

static void famille_10_authentification_basique(void) {
    g_famille = 10;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH_BASIQUE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "auth_basic_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "auth_basic_thread_executeur_demarre");

    /* --- Credential Basic valide -> 200, corps reflete le "user:pass"
     * DECODE tel que recu par surAuthentification (preuve du decodage,
     * pas seulement du strip du prefixe). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: Basic YWxpY2U6c2VjcmV0\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_basic_valide_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrecu:alice:secret") != NULL,
              "auth_basic_valide_credential_decode_relu");
    }

    /* --- Mauvais mot de passe -> Faux -> fail-closed 401. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: Basic YWxpY2U6d3Jvbmc=\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_basic_mauvais_credential_401");
    }

    /* --- Controle NEGATIF d'extraction : en-tete SANS le prefixe "Basic "
     * ne doit JAMAIS etre pris tel quel (la valeur brute "alice:secret"
     * matcherait par accident et authentifierait). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: alice:secret\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_basic_sans_prefixe_jamais_pris_tel_quel");
    }

    /* --- Base64 malforme : chars hors alphabet -> credential vide, JAMAIS
     * de crash (401 ou 500 propre, mais le process doit rester debout --
     * le CHECK suivant sur la meme connexion x le prouve). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: Basic !!!nonbase64$$$\r\n\r\n", x);
        CHECK(e.statut == 401 || e.statut == 500,
              "auth_basic_base64_malforme_401_ou_500_sans_crash");
    }

    /* --- "Basic " suivi de rien -> credential vide -> 401. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: Basic \r\n\r\n", x);
        CHECK(e.statut == 401, "auth_basic_vide_apres_prefixe_401");
    }

    /* --- SECURITE CLE -- smuggling par octet NUL : base64("admin\0:x")
     * decode en une chaine contenant un NUL. Le hook accepte pourtant
     * jeton = "admin" tel quel (piege pose expres) : si l'extraction
     * strlen() le decode au lieu de le traiter comme malforme, "admin"
     * matche et authentifie -> FAILLE. Le contrat exige credential VIDE
     * (rejet), jamais "admin" tronque au NUL. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nAuthorization: Basic YWRtaW4AOng=\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_basic_smuggling_nul_jamais_admin_401");
        CHECK(strstr(e.reponse, "recu:admin") == NULL,
              "auth_basic_smuggling_nul_admin_jamais_authentifie");
    }

    /* --- Opt-out httpAuthAucune sur l'operation, meme service Basique --
     * le hook n'est jamais invoque, la requete passe sans credential.
     * L'opt-out court-circuite l'extraction entierement : ce CHECK est
     * attendu VERT des maintenant, pas un rouge de ce ticket. --- */
    {
        EchangeE2E e = joue_e2e("GET /ressource HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_basic_optout_statut_200_sans_credential");
        CHECK(strstr(e.reponse, "\r\n\r\nressource-ok") != NULL,
              "auth_basic_optout_corps_ressource_ok");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- Forme bilingue EN complete --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_AUTH_BASIQUE_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "auth_basic_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "auth_basic_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Basic YWxpY2U6c2VjcmV0\r\n\r\n", x_en);
        CHECK(e.statut == 200, "auth_basic_en_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrecu:alice:secret") != NULL,
              "auth_basic_en_credential_decode_relu");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

static void famille_11_authentification_cle_api(void) {
    g_famille = 11;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH_CLE_API;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "auth_apikey_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "auth_apikey_thread_executeur_demarre");

    /* --- Cle valide dans X-API-Key -> 200, corps reflete la valeur BRUTE
     * telle que recue par surAuthentification (preuve de la propagation,
     * sans decode ni prefixe). ROUGE ATTENDU : X-API-Key n'est ni capture
     * (owl_http_executeur_depose) ni branche dans service_extrait_credential
     * -> credential vide -> hook rejette -> 401 au lieu de 200. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nX-API-Key: cle-secrete-123\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_apikey_valide_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrecu:cle-secrete-123") != NULL,
              "auth_apikey_valide_valeur_brute_relue");
    }

    /* --- Mauvaise cle -> Faux -> fail-closed 401. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nX-API-Key: mauvaise-cle\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_apikey_mauvaise_cle_401");
    }

    /* --- En-tete X-API-Key ABSENT -> credential vide -> 401 (fail-closed,
     * hook quand meme invoque -- pas de court-circuit magique). --- */
    {
        EchangeE2E e = joue_e2e("GET /sonde HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_apikey_entete_absent_credential_vide_401");
    }

    /* --- En-tete X-API-Key present mais VIDE -> credential vide -> 401. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\nX-API-Key: \r\n\r\n", x);
        CHECK(e.statut == 401, "auth_apikey_entete_vide_credential_vide_401");
    }

    /* --- SECURITE CLE -- un Authorization: Bearer portant la BONNE cle,
     * SANS X-API-Key, ne doit JAMAIS servir de credential CleAPI : les
     * sources d'authn ne se melangent pas. Reste VRAI avant ET apres
     * l'impl (aujourd'hui 401 par absence totale de branchement CleAPI ;
     * demain 401 parce que l'extraction CleAPI lit STRICTEMENT X-API-Key,
     * jamais Authorization) -- controle negatif de securite, pas une
     * mesure du rouge de ce ticket. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /sonde HTTP/1.1\r\nHost: a\r\n"
            "Authorization: Bearer cle-secrete-123\r\n\r\n", x);
        CHECK(e.statut == 401, "auth_apikey_authorization_jamais_source_401");
        CHECK(strstr(e.reponse, "recu:cle-secrete-123") == NULL,
              "auth_apikey_authorization_jamais_credential_accepte");
    }

    /* --- Opt-out httpAuthAucune sur l'operation, meme service CleAPI --
     * le hook n'est jamais invoque, la requete passe sans credential.
     * Deja VERT, pas un rouge de ce ticket. --- */
    {
        EchangeE2E e = joue_e2e("GET /ressource HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "auth_apikey_optout_statut_200_sans_credential");
        CHECK(strstr(e.reponse, "\r\n\r\nressource-ok") != NULL,
              "auth_apikey_optout_corps_ressource_ok");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- Forme bilingue EN complete --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_AUTH_CLE_API_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "auth_apikey_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "auth_apikey_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nX-API-Key: cle-secrete-123\r\n\r\n", x_en);
        CHECK(e.statut == 200, "auth_apikey_en_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrecu:cle-secrete-123") != NULL,
              "auth_apikey_en_valeur_brute_relue");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * Source OWL : autorisation (, lot 4).
 *
 * `surAuthentification` pose l'Identite ("admin" ou "invite") sur jeton
 * valide. `surAutorisation` (SANS parametre) lit ensuite cette Identite
 * ET la Methode/le Chemin de la requete pour decider :
 *   - refus explicite en premier (chemin "/refuse-explicite") -> pose
 *     CodeEtat=httpInterdit puis RENVOYER Faux (l'app choisit son code).
 *   - Identite = "admin" -> RENVOYER Vrai (admin passe partout ailleurs).
 *   - Methode = "GET" -> RENVOYER Vrai (lecture ouverte aux non-admin).
 *   - sinon -> RENVOYER Faux SANS rien poser -> fail-closed 403 par defaut.
 *
 *   GET    /ping               -> admin uniquement via Identite, reflete
 *                                  "metier:<identite>" dans le corps.
 *   GET    /statut              -> ouvert a tout jeton valide (branche GET),
 *                                  reflete "identite:<identite>" -- prouve
 *                                  la lecture Identite posee par authn.
 *   DELETE /ressource-protegee  -> refuse pour un non-admin (ni admin ni
 *                                  GET) -> fail-closed 403 SANS code pose.
 *   GET    /refuse-explicite    -> refuse pour un non-admin AVANT la regle
 *                                  GET (l'ordre des IF prouve le court-
 *                                  circuit) -> 403 avec code explicite,
 *                                  corps jamais execute.
 *   GET    /ressource            -> opt-out `Authentification = httpAuthAucune`
 *                                  -> ni authn ni authz ne tournent.
 * ================================================================ */

static const char* SRC_SERVICE_AUTORISATION =
    "ServiceAutorisation est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthBearer\n"
    "    FIN\n"
    "    PROCEDURE surAuthentification(jeton est une chaine) : booleen\n"
    "        SI jeton = \"jeton-admin\" ALORS\n"
    "            MoiMeme..Requete..Identite = \"admin\"\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        SI jeton = \"jeton-invite\" ALORS\n"
    "            MoiMeme..Requete..Identite = \"invite\"\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    PROCEDURE surAutorisation() : booleen\n"
    "        SI MoiMeme..Requete..Chemin = \"/refuse-explicite\" ALORS\n"
    "            MoiMeme..Reponse..CodeEtat = httpInterdit\n"
    "            RENVOYER Faux\n"
    "        FIN\n"
    "        SI MoiMeme..Requete..Identite = \"admin\" ALORS\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        SI MoiMeme..Requete..Methode = \"GET\" ALORS\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    Ping est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ping\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"metier:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    Statut est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"statut\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"identite:\" + MoiMeme..Requete..Identite\n"
    "        FIN\n"
    "    FIN\n"
    "    OpSuppr est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpDelete\n"
    "            Chemin   = [\"ressource-protegee\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"supprime\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"refuse-explicite\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"ne-devrait-jamais-etre-execute\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Ressource est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ressource\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "            Authentification = httpAuthAucune\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"public-sans-authz\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : autorisation, forme BILINGUE EN complete
 * (`onAuthentication`, `onAuthorization`, `Myself..Request..Identity`).
 * ================================================================ */

static const char* SRC_SERVICE_AUTORISATION_EN =
    "ServiceAuthorizationEN is a ServiceREST\n"
    "    PROPERTIES\n"
    "        Authentication = httpAuthBearer\n"
    "    FIN\n"
    "    PROCEDURE onAuthentication(token is a string) : boolean\n"
    "        IF token = \"token-admin\" THEN\n"
    "            Myself..Request..Identity = \"admin\"\n"
    "            RETURN true\n"
    "        FIN\n"
    "        IF token = \"token-guest\" THEN\n"
    "            Myself..Request..Identity = \"guest\"\n"
    "            RETURN true\n"
    "        FIN\n"
    "        RETURN false\n"
    "    FIN\n"
    "    PROCEDURE onAuthorization() : boolean\n"
    "        IF Myself..Request..Identity = \"admin\" THEN\n"
    "            RETURN true\n"
    "        FIN\n"
    "        RETURN false\n"
    "    FIN\n"
    "    Ping is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK, httpInterdit]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"metier:\" + Myself..Request..Identity\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 12 — surAutorisation (, lot 4) : ROUGE ATTENDU
 * ================================================================ */

static void famille_12_autorisation(void) {
    g_famille = 12;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTORISATION;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "authz_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "authz_thread_executeur_demarre");

    /* --- 1. Autorise : Identite = "admin" -> surAutorisation Vrai -> passe
     * jusqu'a surRequete. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer jeton-admin\r\n\r\n", x);
        CHECK(e.statut == 200, "authz_admin_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmetier:admin") != NULL,
              "authz_admin_surrequete_execute");
    }

    /* --- 2a. Interdit avec code explicite pose AVANT le retour Faux :
     * chemin "/refuse-explicite", identite non-admin -> 403, surRequete
     * jamais execute (corps metier absent). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /refuse-explicite HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer jeton-invite\r\n\r\n", x);
        CHECK(e.statut == 403, "authz_refus_explicite_statut_403");
        CHECK(strstr(e.reponse, "ne-devrait-jamais-etre-execute") == NULL,
              "authz_refus_explicite_surrequete_non_execute");
    }

    /* --- 2b. Interdit SANS code pose (Faux "sec", branche finale) -> 403
     * httpInterdit par defaut. LE test de securite cle du fail-closed
     * authz : DELETE par un non-admin ne matche ni la regle admin ni la
     * regle GET -> RENVOYER Faux sans rien poser. --- */
    {
        EchangeE2E e = joue_e2e(
            "DELETE /ressource-protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer jeton-invite\r\n\r\n", x);
        CHECK(e.statut == 403, "authz_sans_code_403_fail_closed");
        CHECK(strstr(e.reponse, "supprime") == NULL,
              "authz_sans_code_surrequete_non_execute");
    }

    /* --- 3. authz LIT l'Identite deposee par authn : un non-admin sur une
     * route ouverte par la regle GET voit son identite reflechie dans le
     * corps -- prouve le passage d'info authn -> authz -> surRequete. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /statut HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer jeton-invite\r\n\r\n", x);
        CHECK(e.statut == 200, "authz_lit_identite_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nidentite:invite") != NULL,
              "authz_lit_identite_relue_dans_le_corps");
    }

    /* --- 4. authn REJETE (401) -> authz ne tourne JAMAIS. Controle negatif
     * d'ordonnancement : un mauvais credential doit rester 401, pas 403,
     * meme sur une route que la regle GET de authz aurait autorisee. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /statut HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer jeton-invalide\r\n\r\n", x);
        CHECK(e.statut == 401, "authz_authn_rejette_401_pas_403_authz_jamais_invoquee");
    }

    /* --- 5. Branchement Methode : GET passe pour un non-admin (deja prouve
     * au 3), DELETE refuse pour un non-admin (deja prouve au 2b) sur la
     * MEME identite -- couple positif/negatif sur le seul critere Methode. --- */

    /* --- 6. Opt-out httpAuthAucune -> skip authn ET authz : la requete
     * passe SANS le moindre credential, le hook authz n'est jamais invoque
     * non plus (sinon Identite=Null tomberait dans la branche finale Faux
     * -> 403, or on attend 200). --- */
    {
        EchangeE2E e = joue_e2e("GET /ressource HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "authz_optout_statut_200_sans_credential");
        CHECK(strstr(e.reponse, "\r\n\r\npublic-sans-authz") != NULL,
              "authz_optout_corps_public_sans_authz");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- 7. Forme bilingue EN complete (onAuthorization) --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_AUTORISATION_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "authz_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "authz_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer token-admin\r\n\r\n", x_en);
        CHECK(e.statut == 200, "authz_en_admin_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmetier:admin") != NULL,
              "authz_en_admin_surrequete_execute");
    }
    {
        EchangeE2E e = joue_e2e(
            "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer token-guest\r\n\r\n", x_en);
        CHECK(e.statut == 403, "authz_en_guest_statut_403_fail_closed");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * FAMILLE 13 — AUTHENTIFICATION JWT DECLARATIVE (, lot 5)
 *
 * Keypair RSA-2048 de TEST (genere hors-ligne via openssl, jamais utilise
 * ailleurs) : la cle publique va dans la config du service OWL
 * (ClePubliqueJWT/JWTPublicKey), la cle privee sert UNIQUEMENT a signer
 * les JWT ici, en C, via mbedtls -- jamais exposee au service.
 * ================================================================ */

static const char* JWT_TEST_CLE_PRIVEE_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQC3H49EdnLkTmYJ\n"
    "jfYDR7K6dQMibGSUezT5RKRO280JLAVG92ggP6Ss1woISMYZaMApRJX0HJw/Walx\n"
    "iyWkcdUSgXr5djlSD59/bslRIoSRjcTbWf5ib/luvmxHv7L4O2cEaid15PxiehHF\n"
    "nBYftZxR1ZftR28IPNb0SPgmHlpivDIqR617K1ZEwXKC67EFbjMGn85mBIAkb+v5\n"
    "/V4u83IFhb4mcSW4eonidvyXIcnHlx+054qe6VdC3gF1g2qzAgac24CUhJwIp6he\n"
    "IsHyf38/RmkcC2OvLPy38AxIOVMjhwfw6JTUI7W0ycHMFz/JFX5yAfWUXxCVNSTi\n"
    "bp9TmYL1AgMBAAECggEAA43hQfrqj/PVlyniNH4XwfazcN+ZtCgQByaF6eHqQNtR\n"
    "3ua+ZqHVmGU0FKUU8zlNQX/LSUmk3rXmzAVfXam38PwCXwUkZ6tV9Ss5TRHWcNzH\n"
    "jVpZzZ9OwA+CGoJl/ttoTHWi/m7LVz33SlaVN1hXCF+U604PoaFMqsEfz92W84Ob\n"
    "IL9nvd865vZ2G47SvWef9Ahcy7o8f9KCY38hWPaJ1+PT2Ge/maIvkX0C9t76dx0G\n"
    "Cvnxz+wP7d0cVtj+PfTsv8wnII0SVzuvlFYCoZvMmXxM3Cv+WA3RYqgVRDko6LsS\n"
    "ciMM0lk1V7M+BCKIV7Rpnu5RGkoSRhhs44A+PspSAQKBgQDx7x4By1c6kbbgmK+h\n"
    "gFmuJT6mITTg00Dn4X//5FLB/J/Iw5QBc4dJ2NrJ+sW0sdp5iCRu3iwQdC9qmnXK\n"
    "h2SVYmjHygQBqdblC9/qE0ZuFuqbdEsssxOtM8YO9pFGYfT0oZzcG5BPBCgqftww\n"
    "w3hNgeWwQjlzdvbzEiG4rYPucQKBgQDBxR5NPbiU0VupqNb4IMfEi/uOfVum8//h\n"
    "E1qVA87QjWRm2UkDyjnZ3Td6SD7VEQJ0Q044W7uyS0CpYTmu304BLv8DhGyzQ9dx\n"
    "NCeuFkfrEAbNj22YYk4nK1ClgkZpwmEiXK4lHmf92mAJvdMdD+1tcom/kEzyavuU\n"
    "2O2OSjJmxQKBgQCsnhei4BqUx3LcPRTS/mj+P8S7c+Xuo8wjpllVkusZTc4TyLfJ\n"
    "cUyN/BPfGPYHPv7Kx8B2+j4mXz+VqC9zGbdCfryUECdbqHqTYi2ZLbeKxP8Pe6JH\n"
    "el4HrnEaLALrXOkRwl3RDjmgpy6hY3JnuJAVfSEM6bNkFvWL6Nv2kJcZgQKBgEat\n"
    "wQTJq6dUykuTU35EbrmpmcN6xFCclcwihBrwvaJiwKmnPjPMstdeEpte+NQ9FRkV\n"
    "Z9W02D0Lp/VgimrX9xsFam19Ay1MZKxdKtAcPnVmbiTS2VYJHZq5+Pu2DjsLdOTU\n"
    "y4ysj6AP5JIJFJV4NtGCQu6GExslnMjJa7VIKSRFAoGBAK854CeGT6LNOXGsAddj\n"
    "U8bHY7kocx2sNxTZ5QHPRE6BTG4B+Va+KO7EyGHxTtkVdtLIqdtQRqOaxL+xllA1\n"
    "RG4vV/8rFp4mDqw3iSAgqWwmBTYTV4VlQMu5l+VPeT7SMcVj3zZzkZAF3ammGUDM\n"
    "LSbsa4pt0bHg1Xq+YlYaRrVS\n"
    "-----END PRIVATE KEY-----\n";

/* Meme PEM public, en litteral OWL backtick (multi-ligne raw, cf. skill
 * test-writing-pitfalls #1bis) pour la config service ClePubliqueJWT --
 * SEULE forme utilisee (rien a verifier cote C, la verification vit dans
 * le service OWL une fois httpAuthJWT implemente). */
#define JWT_TEST_CLE_PUBLIQUE_PEM_OWL \
    "`-----BEGIN PUBLIC KEY-----\n" \
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAtx+PRHZy5E5mCY32A0ey\n" \
    "unUDImxklHs0+USkTtvNCSwFRvdoID+krNcKCEjGGWjAKUSV9BycP1mpcYslpHHV\n" \
    "EoF6+XY5Ug+ff27JUSKEkY3E21n+Ym/5br5sR7+y+DtnBGondeT8YnoRxZwWH7Wc\n" \
    "UdWX7UdvCDzW9Ej4Jh5aYrwyKketeytWRMFyguuxBW4zBp/OZgSAJG/r+f1eLvNy\n" \
    "BYW+JnEluHqJ4nb8lyHJx5cftOeKnulXQt4BdYNqswIGnNuAlIScCKeoXiLB8n9/\n" \
    "P0ZpHAtjryz8t/AMSDlTI4cH8OiU1CO1tMnBzBc/yRV+cgH1lF8QlTUk4m6fU5mC\n" \
    "9QIDAQAB\n" \
    "-----END PUBLIC KEY-----`\n"

static const char* SRC_SERVICE_AUTH_JWT =
    "ServiceAuthJWT est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthJWT\n"
    "        ClePubliqueJWT = " JWT_TEST_CLE_PUBLIQUE_PEM_OWL
    "        EmetteurJWT = \"owl-test-emetteur\"\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"protegee\"]\n"
    "            Reponses = [httpOK, httpNonAutorise]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"sub:\" + MoiMeme..Requete..Identite[\"sub\"]\n"
    "        FIN\n"
    "    FIN\n"
    "    OpPublique est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"publique\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "            Authentification = httpAuthAucune\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"publique-sans-jwt\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : JWT declaratif, forme BILINGUE EN complete
 * (`httpAuthJWT`, `JWTPublicKey`, `JWTIssuer`, `Myself..Request..Identity`).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_JWT_EN =
    "ServiceAuthJWTEN is a ServiceREST\n"
    "    PROPERTIES\n"
    "        Authentication = httpAuthJWT\n"
    "        JWTPublicKey = " JWT_TEST_CLE_PUBLIQUE_PEM_OWL
    "        JWTIssuer = \"owl-test-emetteur\"\n"
    "    FIN\n"
    "    Sonde is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"ping\"]\n"
    "            Responses  = [httpOK, httpNonAutorise]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"sub:\" + Myself..Request..Identity[\"sub\"]\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : composition `surAutorisation` APRES validation JWT
 * declarative (, complement). ZERO hook d'authn -- le framework
 * valide la signature/exp/iss, puis surAutorisation lit le claim "role"
 * depose dans Identite pour decider (fail-closed sur tout role != admin).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_JWT_AUTORISATION =
    "ServiceAuthJWTAutorisation est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthJWT\n"
    "        ClePubliqueJWT = " JWT_TEST_CLE_PUBLIQUE_PEM_OWL
    "        EmetteurJWT = \"owl-test-emetteur\"\n"
    "    FIN\n"
    "    PROCEDURE surAutorisation() : booleen\n"
    "        SI MoiMeme..Requete..Identite[\"role\"] = \"admin\" ALORS\n"
    "            RENVOYER Vrai\n"
    "        FIN\n"
    "        RENVOYER Faux\n"
    "    FIN\n"
    "    Ping est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ping\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"role:\" + MoiMeme..Requete..Identite[\"role\"]\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : audience JWT configuree -- `AudienceJWT`
 * miroir d'`EmetteurJWT`, opt-in : seul ce service exige un claim "aud"
 * correspondant. Les autres services de la famille (sans AudienceJWT)
 * doivent continuer a ignorer "aud" (non-regression cle).
 * ================================================================ */

static const char* SRC_SERVICE_AUTH_JWT_AUDIENCE =
    "ServiceAuthJWTAudience est un ServiceREST\n"
    "    PROPRIETES\n"
    "        Authentification = httpAuthJWT\n"
    "        ClePubliqueJWT = " JWT_TEST_CLE_PUBLIQUE_PEM_OWL
    "        EmetteurJWT = \"owl-test-emetteur\"\n"
    "        AudienceJWT = \"mon-service\"\n"
    "    FIN\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"protegee\"]\n"
    "            Reponses = [httpOK, httpNonAutorise]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"sub:\" + MoiMeme..Requete..Identite[\"sub\"]\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* base64url (RFC 7515) : mbedtls_base64_encode rend du base64 standard
 * (+, /, padding =) -- transposition minimale pour matcher l'alphabet JWT. */
static void jwt_b64url(const unsigned char* src, size_t slen, char* dst, size_t dst_cap) {
    unsigned char tmp[4096];
    size_t olen = 0;
    dst[0] = '\0';
    if (mbedtls_base64_encode(tmp, sizeof(tmp), &olen, src, slen) != 0) return;
    size_t j = 0;
    for (size_t i = 0; i < olen && j + 1 < dst_cap; i++) {
        unsigned char c = tmp[i];
        if (c == '+') dst[j++] = '-';
        else if (c == '/') dst[j++] = '_';
        else if (c == '=') continue;
        else dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

/* Construit un payload JSON de claims : iss (omis si NULL), exp (omis si
 * exp_present=0, sinon time(NULL)+exp_delta_s -- peut etre negatif pour
 * fabriquer un jeton deja expire), sub. */
static void jwt_payload(char* dst, size_t cap, const char* iss_ou_null,
                        int exp_present, long long exp_delta_s, const char* sub) {
    char champ_iss[256] = "";
    char champ_exp[64]  = "";
    if (iss_ou_null) snprintf(champ_iss, sizeof(champ_iss), "\"iss\":\"%s\",", iss_ou_null);
    if (exp_present) {
        long long exp = (long long)time(NULL) + exp_delta_s;
        snprintf(champ_exp, sizeof(champ_exp), "\"exp\":%lld,", exp);
    }
    snprintf(dst, cap, "{%s%s\"sub\":\"%s\"}", champ_iss, champ_exp, sub);
}

/* Variante avec claim "role" supplementaire (composition authz/1949 --
 * `surAutorisation` lit ce claim depose par la validation JWT declarative). */
static void jwt_payload_avec_role(char* dst, size_t cap, const char* iss_ou_null,
                                  int exp_present, long long exp_delta_s,
                                  const char* sub, const char* role) {
    char champ_iss[256] = "";
    char champ_exp[64]  = "";
    if (iss_ou_null) snprintf(champ_iss, sizeof(champ_iss), "\"iss\":\"%s\",", iss_ou_null);
    if (exp_present) {
        long long exp = (long long)time(NULL) + exp_delta_s;
        snprintf(champ_exp, sizeof(champ_exp), "\"exp\":%lld,", exp);
    }
    snprintf(dst, cap, "{%s%s\"sub\":\"%s\",\"role\":\"%s\"}", champ_iss, champ_exp, sub, role);
}

/* Variante avec claims "aud" et/ou "nbf" (, RFC 7519 4.1.3 et
 * 4.1.5). aud_json_ou_null est le FRAGMENT JSON deja forme par l'appelant :
 * soit une chaine JSON (`"\"mon-service\""`), soit un tableau JSON
 * (`"[\"x\",\"mon-service\"]"`), NULL pour omettre le claim. nbf_present=0
 * omet le claim nbf ; sinon nbf = time(NULL) + nbf_delta_s (meme patron
 * dynamique que exp, delta negatif possible pour un nbf deja passe). */
static void jwt_payload_aud_nbf(char* dst, size_t cap, const char* iss_ou_null,
                                int exp_present, long long exp_delta_s,
                                const char* sub, const char* aud_json_ou_null,
                                int nbf_present, long long nbf_delta_s) {
    char champ_iss[256] = "";
    char champ_exp[64]  = "";
    char champ_aud[512] = "";
    char champ_nbf[64]  = "";
    if (iss_ou_null) snprintf(champ_iss, sizeof(champ_iss), "\"iss\":\"%s\",", iss_ou_null);
    if (exp_present) {
        long long exp = (long long)time(NULL) + exp_delta_s;
        snprintf(champ_exp, sizeof(champ_exp), "\"exp\":%lld,", exp);
    }
    if (aud_json_ou_null) snprintf(champ_aud, sizeof(champ_aud), "\"aud\":%s,", aud_json_ou_null);
    if (nbf_present) {
        long long nbf = (long long)time(NULL) + nbf_delta_s;
        snprintf(champ_nbf, sizeof(champ_nbf), "\"nbf\":%lld,", nbf);
    }
    snprintf(dst, cap, "{%s%s%s%s\"sub\":\"%s\"}", champ_iss, champ_exp, champ_aud, champ_nbf, sub);
}

/* Signe header_json + "." + payload_json en RS256 (PKCS#1 v1.5) avec la
 * cle privee de test. `flip_premier_octet_sig` != 0 -> falsifie 1 octet de
 * la signature APRES calcul (corpus d'attaque, cas 4). Rend 1 en succes. */
static int jwt_signe_rs256_ex(const char* header_json, const char* payload_json,
                              int flip_premier_octet_sig, char* dst, size_t dst_cap) {
    char h64[1024], p64[2048];
    jwt_b64url((const unsigned char*)header_json, strlen(header_json), h64, sizeof(h64));
    jwt_b64url((const unsigned char*)payload_json, strlen(payload_json), p64, sizeof(p64));

    char signing_input[4096];
    snprintf(signing_input, sizeof(signing_input), "%s.%s", h64, p64);

    unsigned char hash[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char*)signing_input, strlen(signing_input), hash) != 0) {
        return 0;
    }

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       pk;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&pk);

    int ok = 0;
    unsigned char sig[512];
    size_t sig_len = 0;

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-jwt-test", 12) != 0) goto fin;
    if (mbedtls_pk_parse_key(&pk, (const unsigned char*)JWT_TEST_CLE_PRIVEE_PEM,
                             strlen(JWT_TEST_CLE_PRIVEE_PEM) + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                        sig, sizeof(sig), &sig_len,
                        mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;

    if (flip_premier_octet_sig) sig[0] ^= 0x01;

    {
        char sig64[1024];
        jwt_b64url(sig, sig_len, sig64, sizeof(sig64));
        snprintf(dst, dst_cap, "%s.%s", signing_input, sig64);
        ok = 1;
    }

fin:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

static int jwt_signe_rs256(const char* header_json, const char* payload_json,
                           char* dst, size_t dst_cap) {
    return jwt_signe_rs256_ex(header_json, payload_json, 0, dst, dst_cap);
}

static const char* JWT_HDR_RS256 = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
static const char* JWT_HDR_NONE  = "{\"alg\":\"none\",\"typ\":\"JWT\"}";
static const char* JWT_HDR_HS256 = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

static void famille_13_authentification_jwt(void) {
    g_famille = 13;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH_JWT;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "jwt_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "jwt_thread_executeur_demarre");

    char jwt[4096];
    char payload[512];
    char req[8192];

    /* --- 1. VALIDE : RS256 correctement signe + exp futur + iss correct ->
     * 200, claim "sub" relu dans surRequete via Identite["sub"]. LE rouge
     * DISCRIMINANT aujourd'hui : httpAuthJWT n'existe pas encore -> montage
     * du service echoue a la compilation -> jamais 200 ici. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_valide_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 200, "jwt_valide_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-42") != NULL,
              "jwt_valide_claim_sub_relu_dans_surrequete");
    }

    /* --- 1b. Opt-out httpAuthAucune sur une operation d'un service JWT :
     * AUCUN en-tete Authorization -> 200 quand meme -- prouve que le garde
     * `!authOptOut` partage avec les familles 7/12 s'applique aussi au
     * schema declaratif (la validation JWT elle-meme ne tourne pas). --- */
    {
        EchangeE2E e = joue_e2e("GET /publique HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "jwt_optout_statut_200_sans_authorization");
        CHECK(strstr(e.reponse, "\r\n\r\npublique-sans-jwt") != NULL,
              "jwt_optout_corps_public_sans_validation_jwt");
    }

    /* --- 2. alg=none : LA faille JWT historique -- signature (3e segment)
     * vide, malgre exp/iss valides -> 401, JAMAIS accepte. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        char h64[1024], p64[2048];
        jwt_b64url((const unsigned char*)JWT_HDR_NONE, strlen(JWT_HDR_NONE), h64, sizeof(h64));
        jwt_b64url((const unsigned char*)payload, strlen(payload), p64, sizeof(p64));
        snprintf(jwt, sizeof(jwt), "%s.%s.", h64, p64);
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_alg_none_401");
    }

    /* --- 3. alg=HS256 : pas de confusion d'algo -> 401 (rejet sur l'algo
     * SEUL, jamais de tentative de verif avec la cle publique en HMAC). --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        char h64[1024], p64[2048], sigfaux64[64];
        jwt_b64url((const unsigned char*)JWT_HDR_HS256, strlen(JWT_HDR_HS256), h64, sizeof(h64));
        jwt_b64url((const unsigned char*)payload, strlen(payload), p64, sizeof(p64));
        jwt_b64url((const unsigned char*)"signature-hs256-fantaisiste", 27,
                   sigfaux64, sizeof(sigfaux64));
        snprintf(jwt, sizeof(jwt), "%s.%s.%s", h64, p64, sigfaux64);
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_alg_hs256_401_pas_de_confusion_algo");
    }

    /* --- 4. Signature RS256 valide dont on flippe 1 octet -> 401. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        CHECK(jwt_signe_rs256_ex(JWT_HDR_RS256, payload, 1, jwt, sizeof(jwt)),
              "jwt_falsifie_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_signature_falsifiee_401");
    }

    /* --- 5. exp absent -> 401 (exp exige, jamais tolere absent). --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 0, 0, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_exp_absent_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_exp_absent_401");
    }

    /* --- 6. exp depasse (now - 3600) -> 401. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, -3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_exp_depasse_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_exp_depasse_401");
    }

    /* --- 7a. iss faux (!= EmetteurJWT configure) -> 401. --- */
    {
        jwt_payload(payload, sizeof(payload), "emetteur-inconnu", 1, 3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_iss_faux_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_iss_faux_401");
    }

    /* --- 7b. iss absent -> 401. --- */
    {
        jwt_payload(payload, sizeof(payload), NULL, 1, 3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_iss_absent_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "jwt_iss_absent_401");
    }

    /* --- 8a. JWT malforme : pas 3 segments -> 401, PAS de crash. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer a.b\r\n\r\n", x);
        CHECK(e.statut == 401, "jwt_malforme_pas_3_segments_401");
    }

    /* --- 8b. JWT malforme : segments hors alphabet base64url -> 401, PAS
     * de crash. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer !!!.!!!.!!!\r\n\r\n", x);
        CHECK(e.statut == 401, "jwt_malforme_segment_non_base64url_401");
    }

    /* --- 8c. Jeton vide -> 401, PAS de crash. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer \r\n\r\n", x);
        CHECK(e.statut == 401, "jwt_vide_401");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- 9. Forme bilingue EN complete : httpAuthJWT, JWTPublicKey,
     * JWTIssuer, Myself..Request..Identity["sub"]. --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 8;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_AUTH_JWT_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "jwt_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "jwt_en_thread_executeur_demarre");
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-en");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_en_valide_signe");
        snprintf(req, sizeof(req),
                 "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x_en);
        CHECK(e.statut == 200, "jwt_en_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-en") != NULL,
              "jwt_en_claim_sub_relu_via_myself_request_identity");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);

    /* --- 10. Composition surAutorisation APRES JWT valide : le hook authz
     * lit le claim "role" depose par la validation declarative (ZERO hook
     * d'authn). role=admin -> 200 ; role=invite -> 403 fail-closed. --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_authz;
    owl_http_exec_config_defaut(&cfg_authz);
    cfg_authz.file_profondeur    = 8;
    cfg_authz.plafond_attente_ms = 5000;
    cfg_authz.source_owl         = SRC_SERVICE_AUTH_JWT_AUTORISATION;
    cfg_authz.service_mode       = 1;

    OwlHttpExecuteur* x_authz = owl_http_executeur_cree(&cfg_authz);
    if (!x_authz) { CHECK(0, "jwt_authz_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_authz), "jwt_authz_thread_executeur_demarre");
    {
        jwt_payload_avec_role(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                              "user-admin", "admin");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_authz_admin_signe");
        snprintf(req, sizeof(req),
                 "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x_authz);
        CHECK(e.statut == 200, "jwt_authz_role_admin_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrole:admin") != NULL,
              "jwt_authz_role_admin_claim_relu_dans_surrequete");
    }
    {
        jwt_payload_avec_role(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                              "user-invite", "invite");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "jwt_authz_invite_signe");
        snprintf(req, sizeof(req),
                 "GET /ping HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x_authz);
        CHECK(e.statut == 403, "jwt_authz_role_invite_statut_403_fail_closed");
    }

    owl_http_executeur_arrete(x_authz);
    owl_http_executeur_libere(x_authz);
}

/* ================================================================
 * Source OWL : trois operations nommees d'apres des mots-cles de
 * visibilite POO (Publique / Prive / Etat) -- . La position
 * `<nom> est une OperationHTTP` desambiguise totalement : ces noms
 * sont des ressources REST plausibles (/publique, /prive, /etat), pas
 * des membres de classe. Formes ASCII (le scanner strippe les accents
 * ET matche les alias publique/prive/privee/protege/protegee/etat,
 * cf. scanner.c:311-322 — meme convention robustesse-litteral-C que
 * SRC_SERVICE plus haut, pas une omission de couverture accentuee).
 * ================================================================ */

static const char* SRC_SERVICE_MOTS_CLES_OP =
    "ServiceMotsClesOp est un ServiceREST\n"
    "    Publique est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"publique\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"op-publique-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Prive est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"prive\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"op-prive-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Etat est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"etat\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"op-etat-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : forme BILINGUE EN complete (Public / Private /
 * Protected -- pendants EN des memes mots-cles de visibilite POO).
 * ================================================================ */

static const char* SRC_SERVICE_MOTS_CLES_OP_EN =
    "ServiceKeywordOpsEN is a ServiceREST\n"
    "    Public is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"public\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"op-public-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Private is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"private\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"op-private-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Protected is a HTTPOperation\n"
    "        PROPERTIES\n"
    "            Method     = httpGet\n"
    "            Path       = [\"protected\"]\n"
    "            Responses  = [httpOK]\n"
    "            StatusCode = httpOK\n"
    "        FIN\n"
    "        PROCEDURE onRequest()\n"
    "            Myself..Response..StatusCode = httpOK\n"
    "            Myself..Response..Body = \"op-protected-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * Source OWL : le SERVICE lui-meme est nomme d'apres un mot-cle
 * (Etat est un ServiceREST). Contrat #2  : `<nom> est un
 * ServiceREST` doit accepter un token mot-cle au meme titre qu'un
 * nom d'operation.
 * ================================================================ */

static const char* SRC_SERVICE_NOM_MOTCLE =
    "Etat est un ServiceREST\n"
    "    Sonde est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"service-nom-motcle-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 14 — noms d'operation/service = mots-cles :
 * ROUGE ATTENDU. `service_rest_declaration` (compiler.c) ne reconnait
 * aujourd'hui QUE TOKEN_IDENTIFIER en position de nom d'operation et
 * de nom de service : un token mot-cle (Publique/Prive/Etat, formes FR
 * des visibilites POO + Etat, et leurs pendants EN Public/Private/
 * Protected) fait echouer tout le parsing du bloc ServiceREST (cascade
 * "doit etre suivi de est une OperationHTTP" / "Expression attendue").
 * Le fix elargit la reconnaissance a un token mot-cle EN POSITION
 * GATEE par le contexte `<nom> est une OperationHTTP` / `<nom> est un
 * ServiceREST`. Verrouille ici par compilation (prepare_vm) + routage
 * reel (owl_service_runtime_find + sert_deterministe), patron
 * deterministe sans socket des familles 0/6 -- pas de risque de
 * timeout/hang quand prepare_vm rend faux (thread jamais lance).
 * ================================================================ */

static void famille_14_noms_motcles(void) {
    g_famille = 14;

    /* --- FR : trois operations nommees Publique / Prive / Etat --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_MOTS_CLES_OP;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "motcles_op_cree");
    if (!x) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "motcles_op_compile_dans_la_vm");

    CHECK(owl_service_runtime_find("GET", "/publique") != NULL,
          "motcles_op_route_publique_resolue");
    CHECK(owl_service_runtime_find("GET", "/prive") != NULL,
          "motcles_op_route_prive_resolue");
    CHECK(owl_service_runtime_find("GET", "/etat") != NULL,
          "motcles_op_route_etat_resolue");

    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/publique", NULL);
        CHECK(rep != NULL, "motcles_op_publique_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_publique_statut_200");
            CHECK(strcmp(s.d, "op-publique-ok") == 0, "motcles_op_publique_corps");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/prive", NULL);
        CHECK(rep != NULL, "motcles_op_prive_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_prive_statut_200");
            CHECK(strcmp(s.d, "op-prive-ok") == 0, "motcles_op_prive_corps");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x, "GET", "/etat", NULL);
        CHECK(rep != NULL, "motcles_op_etat_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_etat_statut_200");
            CHECK(strcmp(s.d, "op-etat-ok") == 0, "motcles_op_etat_corps");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);

    /* --- EN : Public / Private / Protected --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 4;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_MOTS_CLES_OP_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    CHECK(x_en != NULL, "motcles_op_en_cree");
    if (!x_en) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x_en), "motcles_op_en_compile_dans_la_vm");

    CHECK(owl_service_runtime_find("GET", "/public") != NULL,
          "motcles_op_en_route_public_resolue");
    CHECK(owl_service_runtime_find("GET", "/private") != NULL,
          "motcles_op_en_route_private_resolue");
    CHECK(owl_service_runtime_find("GET", "/protected") != NULL,
          "motcles_op_en_route_protected_resolue");

    {
        OwlHttpRep* rep = sert_deterministe(x_en, "GET", "/public", NULL);
        CHECK(rep != NULL, "motcles_op_en_public_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_en_public_statut_200");
            CHECK(strcmp(s.d, "op-public-ok") == 0, "motcles_op_en_public_corps");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x_en, "GET", "/private", NULL);
        CHECK(rep != NULL, "motcles_op_en_private_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_en_private_statut_200");
            CHECK(strcmp(s.d, "op-private-ok") == 0, "motcles_op_en_private_corps");
            owl_http_rep_libere(rep);
        }
    }
    {
        OwlHttpRep* rep = sert_deterministe(x_en, "GET", "/protected", NULL);
        CHECK(rep != NULL, "motcles_op_en_protected_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_op_en_protected_statut_200");
            CHECK(strcmp(s.d, "op-protected-ok") == 0, "motcles_op_en_protected_corps");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x_en);

    /* --- Nom de SERVICE mot-cle : Etat est un ServiceREST --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_nom;
    owl_http_exec_config_defaut(&cfg_nom);
    cfg_nom.file_profondeur    = 4;
    cfg_nom.plafond_attente_ms = 5000;
    cfg_nom.source_owl         = SRC_SERVICE_NOM_MOTCLE;
    cfg_nom.service_mode       = 1;

    OwlHttpExecuteur* x_nom = owl_http_executeur_cree(&cfg_nom);
    CHECK(x_nom != NULL, "motcles_nom_service_cree");
    if (!x_nom) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x_nom), "motcles_nom_service_compile_dans_la_vm");

    CHECK(owl_service_runtime_find("GET", "/sonde") != NULL,
          "motcles_nom_service_route_sonde_resolue");
    {
        OwlHttpRep* rep = sert_deterministe(x_nom, "GET", "/sonde", NULL);
        CHECK(rep != NULL, "motcles_nom_service_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 200, "motcles_nom_service_statut_200");
            CHECK(strcmp(s.d, "service-nom-motcle-ok") == 0, "motcles_nom_service_corps");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x_nom);
}

/* ================================================================
 * FAMILLE 15 — multipart/form-data, FichierTéléversé
 *
 * Deux operations : Televerse (un champ texte "titre" + un fichier "scan")
 * et TeleverseListe (un `tableau de FichierTeleverse` sur le champ
 * "fichiers", plusieurs parties de meme nom). Identifiants ASCII dans le
 * literal C (le scanner strippe les accents, meme convention que le reste
 * du fichier) : `FichierTeleverse` == `FichierTéléversé`.
 * ================================================================ */

static const char* SRC_SERVICE_MULTIPART =
    "ServiceUpload est un ServiceREST\n"
    "    PROPRIETES\n"
    "        UrlBase = [\"api\"]\n"
    "    FIN\n"
    "\n"
    "    Televerse est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode     = httpPost\n"
    "            Chemin      = [\"upload\"]\n"
    "            Reponses    = [httpOK]\n"
    "            CodeEtat    = httpOK\n"
    "            ContentType = typeMimeFormulaireMultiPartie\n"
    "        FIN\n"
    "        PROCEDURE surRequete(titre est une chaine, scan est un FichierTeleverse)\n"
    "            RENVOYER titre + \"|\" + scan..NomFichier + \"|\" + scan..Taille + \"|\" + scan..TypeMime\n"
    "        FIN\n"
    "    FIN\n"
    "\n"
    "    TeleverseListe est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode     = httpPost\n"
    "            Chemin      = [\"upload-multi\"]\n"
    "            Reponses    = [httpOK]\n"
    "            CodeEtat    = httpOK\n"
    "            ContentType = typeMimeFormulaireMultiPartie\n"
    "        FIN\n"
    "        PROCEDURE surRequete(fichiers est un tableau de FichierTeleverse)\n"
    "            sNoms est une chaine = \"\"\n"
    "            POUR TOUT f DE fichiers\n"
    "                sNoms = sNoms + f..NomFichier + \",\"\n"
    "            FIN\n"
    "            RENVOYER sNoms\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Construit une requete HTTP multipart/form-data brute : un champ texte
 * "titre" + un fichier "scan" (nom + contenu + content-type fournis par
 * l'appelant). `Content-Length` calcule sur le VRAI corps assemble (jamais
 * compte a la main). Buffer statique : famille_15 est sequentielle, une
 * requete a la fois. */
static char g_mp_buf[8192];
static const char* mp_un_fichier(const char* chemin, const char* titre,
                                  const char* nomFichier, const char* typeMime,
                                  const char* contenu) {
    char corps[4096];
    int cn = snprintf(corps, sizeof corps,
        "--OWLB\r\n"
        "Content-Disposition: form-data; name=\"titre\"\r\n"
        "\r\n"
        "%s\r\n"
        "--OWLB\r\n"
        "Content-Disposition: form-data; name=\"scan\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n"
        "\r\n"
        "%s\r\n"
        "--OWLB--\r\n",
        titre, nomFichier, typeMime, contenu);
    snprintf(g_mp_buf, sizeof g_mp_buf,
        "POST %s HTTP/1.1\r\nHost: a\r\n"
        "Content-Type: multipart/form-data; boundary=OWLB\r\n"
        "Content-Length: %d\r\n\r\n%s",
        chemin, cn, corps);
    return g_mp_buf;
}

/* Deux parties de MEME nom de champ ("fichiers"), pour tableau de
 * FichierTeleverse. */
static char g_mp_buf2[8192];
static const char* mp_deux_fichiers(const char* chemin,
                                     const char* nom1, const char* c1,
                                     const char* nom2, const char* c2) {
    char corps[4096];
    int cn = snprintf(corps, sizeof corps,
        "--OWLB\r\n"
        "Content-Disposition: form-data; name=\"fichiers\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "%s\r\n"
        "--OWLB\r\n"
        "Content-Disposition: form-data; name=\"fichiers\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "%s\r\n"
        "--OWLB--\r\n",
        nom1, c1, nom2, c2);
    snprintf(g_mp_buf2, sizeof g_mp_buf2,
        "POST %s HTTP/1.1\r\nHost: a\r\n"
        "Content-Type: multipart/form-data; boundary=OWLB\r\n"
        "Content-Length: %d\r\n\r\n%s",
        chemin, cn, corps);
    return g_mp_buf2;
}

static void famille_15_multipart_upload(void) {
    g_famille = 15;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_MULTIPART;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "multipart_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "multipart_thread_executeur_demarre");

    /* --- Critere 1+2 : champ texte + fichier lies par nom, metadonnees
     * (Taille/NomFichier/TypeMime) lisibles dans le corps de reponse. --- */
    {
        const char* req = mp_un_fichier("/api/upload", "Rapport mensuel",
                                         "rapport.pdf", "application/pdf",
                                         "%PDF-1.4 contenu de test");
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 200, "multipart_un_fichier_statut_200");
        CHECK(strstr(e.reponse,
              "\r\n\r\nRapport mensuel|rapport.pdf|24|application/pdf") != NULL,
              "multipart_un_fichier_titre_et_metadonnees_fichier");
    }

    /* --- Critere 5 : tableau de FichierTeleverse, deux parties de meme
     * nom de champ ("fichiers"). --- */
    {
        const char* req = mp_deux_fichiers("/api/upload-multi",
                                            "a.txt", "contenu-a",
                                            "b.txt", "contenu-b");
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 200, "multipart_liste_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\na.txt,b.txt,") != NULL,
              "multipart_liste_deux_fichiers_meme_champ");
    }

    /* --- Critere 4 (SECURITE, garde-fou de non-regression) : un nom de
     * fichier client HOSTILE (traversee de repertoire, chemin Windows
     * absolu) ne doit JAMAIS etre reutilise tel quel comme chemin disque.
     * Assertion deliberement VRAIE des aujourd'hui (aucune ecriture disque
     * n'existe encore pour un FichierTeleverse) ET qui DOIT LE RESTER apres
     * implementation -- pas un rouge attendu, un invariant de securite. */
    {
        const char* chemins_interdits[] = {
            "../../etc/passwd",
            "etc/passwd",
            "C:\\Windows\\Temp\\owl_pwned_1839.txt",
            "owl_pwned_1839.txt",
        };
        const char* req = mp_un_fichier("/api/upload", "peu importe",
                                         "../../etc/passwd", "text/plain",
                                         "contenu hostile");
        joue_e2e(req, x);   /* statut non verifie ici : seul l'effet disque compte. */

        int aucune_fuite = 1;
        for (size_t i = 0; i < sizeof(chemins_interdits) / sizeof(chemins_interdits[0]); i++) {
            struct stat st;
            if (stat(chemins_interdits[i], &st) == 0) aucune_fuite = 0;
        }
        CHECK(aucune_fuite,
              "multipart_nom_fichier_hostile_jamais_utilise_comme_chemin_disque");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Source OWL : service minimal pour la route `/openapi.json` (,
 * famille 16). Une seule route metier (GET /api/items), volontairement
 * simple : ce qui est verrouille ici, c'est que le SERVEUR expose LUI-MEME
 * le document derive sur /openapi.json, pas la richesse du document (deja
 * couverte par tests/cli/openapi/openapi_cli_scan.c cote CLI).
 * ================================================================ */

static const char* SRC_SERVICE_OPENAPI_ROUTE =
    "ServiceOpenAPIRoute est un ServiceREST\n"
    "    PROPRIETES\n"
    "        UrlBase = [\"api\"]\n"
    "    FIN\n"
    "    ListeItems est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"items\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"liste\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 16 — route `/openapi.json` servie par le serveur
 * ================================================================ */

static void famille_16_openapi_route(void) {
    g_famille = 16;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_OPENAPI_ROUTE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "openapi_route_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "openapi_route_thread_executeur_demarre");

    /* Route metier temoin AVANT tout : /api/items sert normalement — prouve
     * que la nouvelle route ne remplace/casse pas le routage existant. */
    {
        EchangeE2E e = joue_e2e("GET /api/items HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "openapi_route_metier_temoin_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nliste") != NULL,
              "openapi_route_metier_temoin_corps");
    }

    /* GET /openapi.json -> 200 + document derive, memes champs structurants
     * que le critere 1 (CLI). ROUGE ATTENDU AUJOURD'HUI : cette route
     * n'existe pas -> 404, aucun corps JSON. */
    {
        EchangeE2E e = joue_e2e("GET /openapi.json HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "openapi_route_statut_200");
        CHECK(strstr(e.reponse, "\"openapi\":\"3.") != NULL,
              "openapi_route_version_openapi_3x");
        CHECK(strstr(e.reponse, "\"info\"") != NULL, "openapi_route_bloc_info_present");
        CHECK(strstr(e.reponse, "\"paths\"") != NULL, "openapi_route_bloc_paths_present");
        CHECK(strstr(e.reponse, "\"components\"") != NULL,
              "openapi_route_bloc_components_present");
        CHECK(strstr(e.reponse, "\"/api/items\"") != NULL,
              "openapi_route_chemin_urlbase_plus_segment");
        CHECK(strstr(e.reponse, "\"get\"") != NULL,
              "openapi_route_methode_get_presente");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Source OWL : segment wildcard catch-all `*reste`.
 *
 * Trois opérations, toutes reposant sur le marqueur `*` en dernière
 * position d'un Chemin, aujourd'hui NON reconnu par le parseur (le `*` est
 * TOKEN_STAR, la multiplication, en dehors d'une position segment) :
 *   GET /fichiers/{*reste}             -> reste JOINT (chaîne)
 *   GET /fichiers-array/{*reste}       -> reste SEGMENTÉ (tableau de chaîne)
 *   GET /clients/{id}/docs/{*reste}    -> préfixe littéral + param + wildcard
 * ROUGE ATTENDU AUJOURD'HUI : la source entière échoue à compiler (le `*`
 * fait dérailler le parse de PROPRIETES), donc `owl_http_executeur_demarre`
 * renvoie faux — cf. famille 17 ci-dessous qui n'exerce le routage réel que
 * lorsque ce démarrage réussit (donc après implémentation, sans toucher au
 * fichier de test).
 * ================================================================ */

static const char* SRC_SERVICE_WILDCARD =
    "ServiceFichiersWildcard est un ServiceREST\n"
    "    FichiersReste est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"fichiers\", *reste]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(reste est une chaine)\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = reste\n"
    "        FIN\n"
    "    FIN\n"
    "    FichiersResteArray est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"fichiers-array\", *reste]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(reste est un tableau de chaine)\n"
    "            corps est une chaine = \"\" + Taille(reste)\n"
    "            POUR i = 1 A Taille(reste)\n"
    "                corps += \"|\" + reste[i]\n"
    "            FIN\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = corps\n"
    "        FIN\n"
    "    FIN\n"
    "    ClientsDocsReste est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"clients\", id, \"docs\", *reste]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete(id est un entier, reste est une chaine)\n"
    "            idTxt est une chaine = \"\" + id\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = idTxt + \"|\" + reste\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* Baseline SANS aucun wildcard : garde-fou de non-régression (critère 8).
 * Assertion déjà VRAIE aujourd'hui (routage littéral déjà livré, )
 * ET qui doit le rester après implémentation — pas un rouge attendu. Source
 * séparée à dessein : la coexistence dans le MÊME fichier source que le
 * wildcard ferait échouer sa compilation par ricochet (panic-mode du
 * parseur), ce qui ne serait pas un rouge « pour la bonne raison » pour ce
 * critère-là. */
static const char* SRC_SERVICE_WILDCARD_BASELINE =
    "ServiceFichiersWildcardBaseline est un ServiceREST\n"
    "    ClientsActifsWildcardBaseline est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"clients\", \"actifs\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"liste-actifs\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 17 — segment wildcard catch-all `*reste`
 * ================================================================ */

static void famille_17_wildcard_catchall(void) {
    g_famille = 17;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 1000;
    cfg.source_owl         = SRC_SERVICE_WILDCARD;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "wildcard_cree"); return; }

    /* ROUGE ATTENDU AUJOURD'HUI : le `*` de Chemin n'est pas encore reconnu
     * par le parseur -> la source ne compile pas -> demarre() renvoie faux
     * (vm_echec). Le bloc ci-dessous ne s'exécute qu'une fois demarre() OK,
     * donc les scénarios 1 à 5 s'activent automatiquement après
     * implémentation, sans retoucher ce fichier. */
    int demarreOk = owl_http_executeur_demarre(x);
    CHECK(demarreOk, "wildcard_service_compile_et_demarre");

    if (demarreOk) {
        /* --- Critère 1 : binding chaîne, plusieurs segments joints --- */
        {
            EchangeE2E e = joue_e2e("GET /fichiers/a/b/c.txt HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 200, "wildcard_chaine_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\na/b/c.txt") != NULL,
                  "wildcard_chaine_corps_segments_joints");
        }

        /* --- Critère 2 : binding tableau, mêmes segments séparés --- */
        {
            EchangeE2E e = joue_e2e("GET /fichiers-array/a/b/c.txt HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 200, "wildcard_tableau_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\n3|a|b|c.txt") != NULL,
                  "wildcard_tableau_corps_taille_et_segments");
        }

        /* --- Critère 3 : un seul segment final (les deux représentations) --- */
        {
            EchangeE2E e = joue_e2e("GET /fichiers/x HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 200, "wildcard_un_segment_chaine_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\nx") != NULL,
                  "wildcard_un_segment_chaine_corps");
        }
        {
            EchangeE2E e = joue_e2e("GET /fichiers-array/x HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 200, "wildcard_un_segment_tableau_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\n1|x") != NULL,
                  "wildcard_un_segment_tableau_corps");
        }

        /* --- Critère 4 : préfixe littéral + paramètre AVANT le wildcard --- */
        {
            EchangeE2E e = joue_e2e("GET /clients/42/docs/a/b HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 200, "wildcard_prefixe_param_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\n42|a/b") != NULL,
                  "wildcard_prefixe_param_corps_id_et_reste");
        }

        /* --- Critère 5 : zéro segment final NE matche PAS (au moins un
         * segment exigé) — les deux routes catch-all, symétriquement. --- */
        {
            EchangeE2E e = joue_e2e("GET /fichiers HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 404, "wildcard_zero_segment_chaine_404");
        }
        {
            EchangeE2E e = joue_e2e("GET /fichiers-array HTTP/1.1\r\nHost: a\r\n\r\n", x);
            CHECK(e.statut == 404, "wildcard_zero_segment_tableau_404");
        }
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- Critère 8 : non-régression, service SANS wildcard, isolé --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfgBase;
    owl_http_exec_config_defaut(&cfgBase);
    cfgBase.file_profondeur    = 4;
    cfgBase.plafond_attente_ms = 5000;
    cfgBase.source_owl         = SRC_SERVICE_WILDCARD_BASELINE;
    cfgBase.service_mode       = 1;

    OwlHttpExecuteur* xBase = owl_http_executeur_cree(&cfgBase);
    if (!xBase) { CHECK(0, "wildcard_baseline_cree"); return; }

    CHECK(owl_http_executeur_demarre(xBase), "wildcard_baseline_demarre");

    {
        EchangeE2E e = joue_e2e("GET /clients/actifs HTTP/1.1\r\nHost: a\r\n\r\n", xBase);
        CHECK(e.statut == 200, "wildcard_baseline_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nliste-actifs") != NULL,
              "wildcard_baseline_corps_inchange");
    }

    owl_http_executeur_arrete(xBase);
    owl_http_executeur_libere(xBase);
}

/* ================================================================
 * Source OWL : securite methode via constante httpMethodes.
 *
 * `surAutorisation` refuse une methode DELETE en la comparant a la
 * constante `httpDelete`. Aujourd'hui `httpDelete` est un entier et
 * `MoiMeme..Requete..Methode` une chaine ("DELETE") -- la comparaison
 * chaine/entier est TOUJOURS fausse, donc le refus ne se declenche
 * jamais : un DELETE sur la route protegee passe a tort (faux-negatif
 * d'autorisation). ROUGE ATTENDU tant que httpMethodes n'est pas
 * string-valued.
 *   GET    /ressource-securisee -> autorise (n'est pas httpDelete).
 *   DELETE /ressource-securisee -> DOIT etre refuse (403) une fois le
 *                                    fix applique ; aujourd'hui : 200 a
 *                                    tort (comparaison toujours fausse).
 *   GET    /sonde-retrocompat   -> garde-fou retrocompat : comparaison
 *                                    avec la chaine litterale "GET"
 *                                    reste vraie (deja vert aujourd'hui,
 *                                    ne doit pas regresser apres le fix).
 * ================================================================ */

static const char* SRC_SERVICE_SECURITE_METHODE =
    "ServiceSecuriteMethode est un ServiceREST\n"
    "    PROCEDURE surAutorisation() : booleen\n"
    "        SI MoiMeme..Requete..Methode = httpDelete ALORS\n"
    "            MoiMeme..Reponse..CodeEtat = httpInterdit\n"
    "            RENVOYER Faux\n"
    "        FIN\n"
    "        RENVOYER Vrai\n"
    "    FIN\n"
    "    Lecture est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"ressource-securisee\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"lecture-ok\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Suppression est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpDelete\n"
    "            Chemin   = [\"ressource-securisee\"]\n"
    "            Reponses = [httpOK, httpInterdit]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"supprime-a-tort\"\n"
    "        FIN\n"
    "    FIN\n"
    "    SondeRetrocompat est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sonde-retrocompat\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            sMatch est une chaine = \"non\"\n"
    "            SI MoiMeme..Requete..Methode = \"GET\" ALORS\n"
    "                sMatch = \"oui\"\n"
    "            FIN\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"match-get:\" + sMatch\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 18 — securite methode via constante httpMethodes
 * ================================================================ */

static void famille_18_securite_methode_constante(void) {
    g_famille = 18;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_SECURITE_METHODE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "secmeth_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "secmeth_thread_executeur_demarre");

    /* --- Non-regression routing : GET route bien vers Lecture (httpGet
     * comme discriminant de route, independant du bug de comparaison). --- */
    {
        EchangeE2E e = joue_e2e("GET /ressource-securisee HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "secmeth_get_route_ok_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nlecture-ok") != NULL,
              "secmeth_get_route_ok_corps");
    }

    /* --- LE bug : DELETE doit etre refuse (403) par surAutorisation quand
     * `Methode = httpDelete` matche reellement. AUJOURD'HUI : httpDelete
     * est un entier, Methode une chaine ("DELETE") -- comparaison toujours
     * fausse -- le refus ne se declenche jamais -- le DELETE passe a tort
     * et sert "supprime-a-tort" avec un statut 200. Rouge pour la bonne
     * raison : ce CHECK echoue parce que le statut obtenu est 200 au lieu
     * de 403 (faux-negatif d'autorisation), pas un crash ni une erreur de
     * compilation. --- */
    {
        EchangeE2E e = joue_e2e("DELETE /ressource-securisee HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 403, "secmeth_delete_refuse_403_apres_fix");
        CHECK(strstr(e.reponse, "supprime-a-tort") == NULL,
              "secmeth_delete_surrequete_jamais_execute_apres_fix");
    }

    /* --- Garde-fou retrocompat : comparaison a la chaine litterale "GET"
     * (pas une constante httpMethodes) reste vraie sur un GET, avant ET
     * apres le fix -- ne doit jamais regresser. --- */
    {
        EchangeE2E e = joue_e2e("GET /sonde-retrocompat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "secmeth_retrocompat_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmatch-get:oui") != NULL,
              "secmeth_retrocompat_chaine_litterale_toujours_vraie");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Source OWL : un ServiceREST a 4 ops, chacune illustrant une facette du
 * pilotage du statut par defaut par le CodeEtat declare.
 *   AccepteSansOverride    : CodeEtat = httpAccepte (202), handler qui pose
 *                            le corps SANS statut -> LE bug/fix (coeur).
 *   SansCodeEtatDeclare    : pas de CodeEtat sur l'op, handler sans statut
 *                            -> fallback 200 (non-regression).
 *   CodeEtatOK             : CodeEtat = httpOK (200), handler sans statut
 *                            -> 200 inchange (non-regression).
 *   AccepteAvecOverride    : CodeEtat = httpAccepte (202), handler qui pose
 *                            explicitement un autre statut -> l'override du
 *                            handler doit gagner sur le defaut declare.
 * ================================================================ */

static const char* SRC_SERVICE_CODEETAT_DEFAUT =
    "ServiceCodeEtatDefaut est un ServiceREST\n"
    "    AccepteSansOverride est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"accepte-sans-override\"]\n"
    "            Reponses = [httpAccepte]\n"
    "            CodeEtat = httpAccepte\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"traite\"\n"
    "        FIN\n"
    "    FIN\n"
    "    SansCodeEtatDeclare est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"sans-codeetat-declare\"]\n"
    "            Reponses = [httpOK]\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"traite\"\n"
    "        FIN\n"
    "    FIN\n"
    "    CodeEtatOK est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"codeetat-ok\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..Corps = \"traite\"\n"
    "        FIN\n"
    "    FIN\n"
    "    AccepteAvecOverride est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"accepte-avec-override\"]\n"
    "            Reponses = [httpAccepte, httpNonAutorise]\n"
    "            CodeEtat = httpAccepte\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpNonAutorise\n"
    "            MoiMeme..Reponse..Corps = \"refuse-explicite\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 19 — CodeEtat declare pilote le statut par defaut
 * ================================================================ */

static void famille_19_codeetat_pilote_defaut(void) {
    g_famille = 19;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_CODEETAT_DEFAUT;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "codeetat_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "codeetat_thread_executeur_demarre");

    /* --- LE bug (coeur, ) : CodeEtat = httpAccepte (202) declare
     * sur l'op, handler qui pose le corps SANS poser de statut. AUJOURD'HUI :
     * le chemin succes hardcode 200 -- le CodeEtat declare de l'op est
     * ignore -- ce CHECK echoue parce que le statut obtenu est 200 au lieu
     * du 202 attendu. Rouge pour la bonne raison (pas un crash, pas une
     * erreur de compilation). --- */
    {
        EchangeE2E e = joue_e2e("GET /accepte-sans-override HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 202, "codeetat_declare_pilote_defaut_202_apres_fix");
        CHECK(strstr(e.reponse, "\r\n\r\ntraite") != NULL,
              "codeetat_declare_corps_traite");
    }

    /* --- Non-regression : aucun CodeEtat declare sur l'op -> le fallback
     * 200 reste vrai, avant ET apres le fix. --- */
    {
        EchangeE2E e = joue_e2e("GET /sans-codeetat-declare HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "codeetat_absent_fallback_200");
        CHECK(strstr(e.reponse, "\r\n\r\ntraite") != NULL,
              "codeetat_absent_corps_traite");
    }

    /* --- Non-regression : CodeEtat = httpOK (200) declare -> 200 inchange
     * (le defaut declare vaut deja 200, rien ne bouge). --- */
    {
        EchangeE2E e = joue_e2e("GET /codeetat-ok HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "codeetat_httpok_reste_200");
        CHECK(strstr(e.reponse, "\r\n\r\ntraite") != NULL,
              "codeetat_httpok_corps_traite");
    }

    /* --- Override handler preserve : CodeEtat = httpAccepte (202) declare,
     * mais le handler pose explicitement MoiMeme..Reponse..CodeEtat =
     * httpNonAutorise (401). L'override du handler doit gagner sur le
     * defaut declare, avant ET apres le fix -- le fix ne touche QUE le
     * defaut du chemin succes, jamais l'override handler. --- */
    {
        EchangeE2E e = joue_e2e("GET /accepte-avec-override HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 401, "codeetat_override_handler_gagne_401");
        CHECK(strstr(e.reponse, "\r\n\r\nrefuse-explicite") != NULL,
              "codeetat_override_corps_refuse_explicite");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * Source OWL : surErreur lit ExceptionInfo() sur le crash d'un handler
 * (, partie 2). g_exc_info doit etre peuple AVANT l'invocation de
 * surErreur (contexte de l'exception qui a fait planter /crash), et nettoye
 * ensuite -- aucune fuite vers une requete suivante qui reussit.
 *   Etat  : handler normal qui lit aussi ExceptionInfo() -- doit rester vide
 *           hors de tout crash (avant ET apres le crash -- garde anti-stale).
 *   Crash : division entiere par zero -- surErreur doit voir le message de
 *           l'exception via ExceptionInfo().
 * ================================================================ */

static const char* SRC_SERVICE_SURERREUR_EXCEPTIONINFO =
    "ServiceSurErreurExc est un ServiceREST\n"
    "    PROCEDURE surErreur()\n"
    "        MoiMeme..Reponse..Corps = \"EXC:[\" + ExceptionInfo() + \"]\"\n"
    "    FIN\n"
    "    Etat est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"etat\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"etat:[\" + ExceptionInfo() + \"]\"\n"
    "        FIN\n"
    "    FIN\n"
    "    Crash est une OperationHTTP\n"
    "        PROPRIETES\n"
    "            Methode  = httpGet\n"
    "            Chemin   = [\"crash\"]\n"
    "            Reponses = [httpOK]\n"
    "            CodeEtat = httpOK\n"
    "        FIN\n"
    "        PROCEDURE surRequete()\n"
    "            nDividende est un entier = 1\n"
    "            nDiviseur est un entier = 0\n"
    "            nResultat est un entier = nDividende / nDiviseur\n"
    "            MoiMeme..Reponse..CodeEtat = httpOK\n"
    "            MoiMeme..Reponse..Corps = \"jamais_atteint\"\n"
    "        FIN\n"
    "    FIN\n"
    "FIN\n";

/* ================================================================
 * FAMILLE 20 — ExceptionInfo() peuple dans surErreur sur un crash
 * ================================================================ */

static void famille_20_surerreur_exceptioninfo(void) {
    g_famille = 20;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_SURERREUR_EXCEPTIONINFO;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "surerreur_exc_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "surerreur_exc_thread_executeur_demarre");

    /* --- Baseline AVANT tout crash : ExceptionInfo() est vide dans un
     * handler normal -- vrai avant ET apres le fix (rien n'a encore plante
     * pour peupler g_exc_info). --- */
    {
        EchangeE2E e = joue_e2e("GET /etat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "surerreur_exc_etat_avant_crash_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\netat:[]") != NULL,
              "surerreur_exc_etat_avant_crash_vide");
    }

    /* --- LE bug/fix (coeur,  partie 2) : sur le crash de /crash
     * (division entiere par zero), surErreur lit ExceptionInfo() dans son
     * corps de reponse. AUJOURD'HUI : g_exc_info n'est jamais peuple avant
     * l'invocation reentrante de surErreur -> ExceptionInfo() renvoie vide
     * -> corps "EXC:[]" -- ce CHECK echoue car "Division" n'apparait pas.
     * Rouge pour la bonne raison (pas un crash de la suite, pas une erreur
     * de compilation -- juste un contexte d'exception vide). --- */
    {
        EchangeE2E e = joue_e2e("GET /crash HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 500, "surerreur_exc_crash_statut_500");
        CHECK(strstr(e.reponse, "Division") != NULL,
              "surerreur_exc_crash_corps_contient_message_exception");
    }

    /* --- Garde anti-stale : APRES le crash, une requete qui reussit ne doit
     * JAMAIS voir un ExceptionInfo() issu du crash precedent -- le contexte
     * doit etre nettoye des la sortie de surErreur. --- */
    {
        EchangeE2E e = joue_e2e("GET /etat HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "surerreur_exc_etat_apres_crash_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\netat:[]") != NULL,
              "surerreur_exc_etat_apres_crash_pas_de_fuite");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 21 — en-tetes de reponse a valeur SCALAIRE coercee
 *
 * Un en-tete de reponse pose a une valeur non-chaine (entier, reel,
 * booleen) etait SILENCIEUSEMENT droppe (garde IS_STRING dans l'emission).
 * Un header HTTP est textuel : on coerce le scalaire en sa chaine canonique
 * (comme la query / les params). Une valeur NON coercible (tableau, instance)
 * reste skippee proprement. Patron e2e socket exclusif (joue_e2e) : seul
 * moyen de prouver l'emission sur les octets qui ont VRAIMENT traverse le fil.
 * ================================================================ */
static void famille_21_entetes_scalaires_coerces(void) {
    g_famille = 21;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_ENTETES_SCALAIRES;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "scalaires_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "scalaires_thread_executeur_demarre");

    EchangeE2E e = joue_e2e("GET /scalaires HTTP/1.1\r\nHost: a\r\n\r\n", x);
    CHECK(e.statut == 200, "scalaires_statut_200");

    /* --- LE bug/fix : la valeur ENTIERE est coercee et emise sur le fil. --- */
    {
        char v[64];
        CHECK(entete_ligne_valeur(e.reponse, "X-Entier", v, sizeof(v))
                  && strcmp(v, "123") == 0,
              "scalaires_entier_coerce_en_123");
    }

    /* --- Reel : chaine canonique round-trippable (owl_real_to_str). --- */
    {
        char v[64];
        CHECK(entete_ligne_valeur(e.reponse, "X-Reel", v, sizeof(v))
                  && strcmp(v, "3.5") == 0,
              "scalaires_reel_coerce_en_3.5");
    }

    /* --- Booleen : "1"/"0" en contexte textuel (canon OWL). --- */
    {
        char v[64];
        CHECK(entete_ligne_valeur(e.reponse, "X-Bool", v, sizeof(v))
                  && strcmp(v, "1") == 0,
              "scalaires_booleen_coerce_en_1");
    }

    /* --- Temoin chaine : toujours emis (non-regression). --- */
    {
        char v[64];
        CHECK(entete_ligne_valeur(e.reponse, "X-Chaine", v, sizeof(v))
                  && strcmp(v, "abc") == 0,
              "scalaires_chaine_temoin_emise");
    }

    /* --- Non coercible (tableau) : SKIPPE proprement, jamais emis. --- */
    {
        char v[64];
        CHECK(entete_ligne_valeur(e.reponse, "X-Tableau", v, sizeof(v)) == 0,
              "scalaires_tableau_non_coercible_absent");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLES 22/23 — corps binaire brut
 *
 * Le gap : un service qui genere un PDF/image ne peut pas servir un
 * corps binaire — `..Corps = <buffer>` est droppe (garde IS_STRING),
 * `RENVOYER <buffer>` tombe dans la serialisation JSON (le buffer n'a
 * pas de forme JSON -> corps ne reflete jamais les octets reels).
 *
 * PDF_BLOB (21 octets) : un octet NUL au milieu (attrape toute troncature
 * type strlen) + 4 octets non-UTF-8-valides consecutifs (0xFF, 0xFE, 0xC0,
 * 0x80 — 0xFF/0xFE n'apparaissent JAMAIS dans de l'UTF-8 valide, 0xC0 0x80
 * est un encodage surlonge invalide de NUL). Un tel blob ne peut PAS etre
 * feinte comme une chaine UTF-8 : seul un VRAI chemin octets-bruts peut le
 * restituer intact. Construit cote OWL via BufferVide + BufferÉcritOctet
 * (memes 21 octets, memes positions) dans SRC_SERVICE_CORPS_BINAIRE /
 * SRC_SERVICE_RENVOYER_BUFFER ci-dessus.
 *
 * Patron e2e socket exclusif (joue_e2e) : seul moyen de prouver que les
 * octets qui ont VRAIMENT traverse le transport sont identiques au blob,
 * memcmp octet-par-octet apres localisation du corps (`\r\n\r\n`), plus
 * Content-Length numerique == taille exacte et Content-Type sur le fil.
 * ================================================================ */

static const unsigned char PDF_BLOB[] = {
    0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x34, 0x0A,
    0x00,
    0xFF, 0xFE, 0xC0, 0x80,
    0x0A,
    0x25, 0x25, 0x45, 0x4F, 0x46, 0x0A
};
#define PDF_BLOB_LEN ((size_t)(sizeof(PDF_BLOB)))

/* Localise le debut du corps ("\r\n\r\n") dans une reponse e2e brute et
 * rend la longueur restante (reponse_len - offset corps). NULL si la
 * separation en-tetes/corps est introuvable. Le corps peut contenir des
 * NUL/octets non-UTF-8 : on borne strictement par reponse_len (jamais de
 * strlen sur le corps), c'est tout l'interet du test. */
static const char* corps_e2e(const EchangeE2E* e, size_t* out_len) {
    /* strstr s'arrete au premier NUL rencontre — sans risque ici car le
     * separateur "\r\n\r\n" est toujours dans le prefixe TEXTE des
     * en-tetes, avant tout octet binaire du corps. */
    const char* sep = strstr(e->reponse, "\r\n\r\n");
    if (!sep) { *out_len = 0; return NULL; }
    const char* debut = sep + 4;
    size_t offset = (size_t)(debut - e->reponse);
    if (offset > e->reponse_len) { *out_len = 0; return NULL; }
    *out_len = e->reponse_len - offset;
    return debut;
}

static void famille_22_corps_binaire(void) {
    g_famille = 22;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_CORPS_BINAIRE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "corpsbinaire_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "corpsbinaire_thread_executeur_demarre");

    /* --- 1. MoiMeme..Reponse..Corps = <buffer> : octets bruts INTACTS,
     * Content-Length exact, Content-Type pilote par TypeContenu. --- */
    {
        EchangeE2E e = joue_e2e("GET /pdf HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "pdf_statut_200");

        char ct[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", ct, sizeof(ct))
                  && strncmp(ct, "application/pdf", 15) == 0,
              "pdf_content_type_application_pdf_sur_le_fil");

        char cl[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Length", cl, sizeof(cl))
                  && (size_t)atol(cl) == PDF_BLOB_LEN,
              "pdf_content_length_taille_exacte");

        size_t blen = 0;
        const char* b = corps_e2e(&e, &blen);
        CHECK(b != NULL && blen == PDF_BLOB_LEN
                  && memcmp(b, PDF_BLOB, PDF_BLOB_LEN) == 0,
              "pdf_corps_memcmp_octets_bruts_intacts");
    }

    /* --- 2. Temoin non-regression : corps chaine reste text/plain,
     * intact (pas touche par le nouveau chemin binaire). --- */
    {
        EchangeE2E e = joue_e2e("GET /texte HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "corpsbinaire_texte_statut_200");

        char ct[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", ct, sizeof(ct))
                  && strncmp(ct, "text/plain", 10) == 0,
              "corpsbinaire_texte_content_type_text_plain");

        size_t blen = 0;
        const char* b = corps_e2e(&e, &blen);
        CHECK(b != NULL && blen == strlen("texte simple")
                  && memcmp(b, "texte simple", blen) == 0,
              "corpsbinaire_texte_corps_intact_non_regression");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 23 — RENVOYER <buffer> = corps binaire brut
 *
 * Symetrique de la famille 22 pour le chemin RENVOYER (au lieu de
 * MoiMeme..Reponse..Corps =). /pdf pose TypeContenu explicitement (doit
 * gagner) ; /brut ne pose rien (doit tomber sur un type binaire neutre,
 * PAS la serialisation JSON "null" actuelle) ; /texte est le temoin
 * non-regression RENVOYER chaine -> text/plain deja verrouille par la
 * famille 4, reverifie ici dans le contexte du nouveau chemin
 * binaire pour s'assurer qu'il ne regresse pas au passage.
 * ================================================================ */

static void famille_23_renvoyer_buffer(void) {
    g_famille = 23;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_RENVOYER_BUFFER;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "renvoyerbuffer_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "renvoyerbuffer_thread_executeur_demarre");

    /* --- 1. RENVOYER buf + TypeContenu pose explicitement : l'override
     * gagne, octets bruts intacts. --- */
    {
        EchangeE2E e = joue_e2e("GET /pdf HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "renvoyerbuffer_pdf_statut_200");

        char ct[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", ct, sizeof(ct))
                  && strncmp(ct, "application/pdf", 15) == 0,
              "renvoyerbuffer_pdf_content_type_override_gagne");

        char cl[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Length", cl, sizeof(cl))
                  && (size_t)atol(cl) == PDF_BLOB_LEN,
              "renvoyerbuffer_pdf_content_length_taille_exacte");

        size_t blen = 0;
        const char* b = corps_e2e(&e, &blen);
        CHECK(b != NULL && blen == PDF_BLOB_LEN
                  && memcmp(b, PDF_BLOB, PDF_BLOB_LEN) == 0,
              "renvoyerbuffer_pdf_corps_memcmp_octets_bruts_intacts");
    }

    /* --- 2. RENVOYER buf SANS TypeContenu : type binaire neutre par
     * defaut (PAS la serialisation JSON actuelle -> "null"), octets bruts
     * intacts (LE bug/fix : le buffer n'est pas feinte, ni tronque). --- */
    {
        EchangeE2E e = joue_e2e("GET /brut HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "renvoyerbuffer_brut_statut_200");

        char ct[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", ct, sizeof(ct))
                  && strncmp(ct, "application/octet-stream", 24) == 0,
              "renvoyerbuffer_brut_content_type_binaire_neutre_par_defaut");

        char cl[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Length", cl, sizeof(cl))
                  && (size_t)atol(cl) == PDF_BLOB_LEN,
              "renvoyerbuffer_brut_content_length_taille_exacte");

        size_t blen = 0;
        const char* b = corps_e2e(&e, &blen);
        CHECK(b != NULL && blen == PDF_BLOB_LEN
                  && memcmp(b, PDF_BLOB, PDF_BLOB_LEN) == 0,
              "renvoyerbuffer_brut_corps_memcmp_octets_bruts_intacts");
    }

    /* --- 3. Temoin non-regression : RENVOYER chaine reste text/plain,
     * intact, dans le contexte du nouveau chemin binaire. --- */
    {
        EchangeE2E e = joue_e2e("GET /texte HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "renvoyerbuffer_texte_statut_200");

        char ct[64];
        CHECK(entete_ligne_valeur(e.reponse, "Content-Type", ct, sizeof(ct))
                  && strncmp(ct, "text/plain", 10) == 0,
              "renvoyerbuffer_texte_content_type_text_plain");

        size_t blen = 0;
        const char* b = corps_e2e(&e, &blen);
        CHECK(b != NULL && blen == strlen("texte simple")
                  && memcmp(b, "texte simple", blen) == 0,
              "renvoyerbuffer_texte_corps_intact_non_regression");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);
}

/* ================================================================
 *  — catalogue events partage ServiceREST/OpérationHTTP.
 * `service_event_classify` (liste en dur, compiler.c) doit ceder la place
 * a un lookup dans une source unique consommee aussi par le LSP
 * (coloration + completion) : `owl_operation_event_catalog_*` (niveau
 * OpérationHTTP : surRequete/onRequest) et `owl_service_event_catalog_*`
 * (niveau ServiceREST : surAuthentification/onAuthentication,
 * surAutorisation/onAuthorization, surErreur/onError, FinInitialisation/
 * EndInitialization, Fermeture/Closing). ROUGE ATTENDU : ces deux API
 * n'existent pas encore (echec de COMPILATION de ce fichier, pas
 * d'echec runtime) tant que owl_event_catalog.{c,h} n'a pas ete etendu.
 * ================================================================ */

static void famille_24_catalogue_events_partage(void) {
    g_famille = 24;

    vm_init();

    /* --- Niveau OpérationHTTP : surRequete/onRequest, source unique. --- */
    {
        const OwlEventDef* d = owl_operation_event_catalog_find("surRequete");
        CHECK(d != NULL, "catalogue_operation_surRequete_trouve");
        if (d) {
            CHECK(d->nameEN != NULL && strcasecmp(d->nameEN, "onRequest") == 0,
                  "catalogue_operation_surRequete_nameEN_onRequest");
            CHECK(d->descFR != NULL && d->descFR[0] != '\0',
                  "catalogue_operation_surRequete_descFR_non_vide");
            CHECK(d->descEN != NULL && d->descEN[0] != '\0',
                  "catalogue_operation_surRequete_descEN_non_vide");
        }
        /* Alias EN retrouve le meme event (lookup casse/accent-insensible,
         * meme contrat que les autres catalogues owl_event_catalog). */
        const OwlEventDef* dEN = owl_operation_event_catalog_find("onRequest");
        CHECK(dEN != NULL && dEN == d, "catalogue_operation_alias_EN_onRequest_meme_def");
    }

    /* --- Niveau ServiceREST : surErreur/onError. --- */
    {
        const OwlEventDef* d = owl_service_event_catalog_find("surErreur");
        CHECK(d != NULL, "catalogue_service_surErreur_trouve");
        if (d) {
            CHECK(d->nameEN != NULL && strcasecmp(d->nameEN, "onError") == 0,
                  "catalogue_service_surErreur_nameEN_onError");
            CHECK(d->descFR != NULL && d->descFR[0] != '\0',
                  "catalogue_service_surErreur_descFR_non_vide");
            CHECK(d->descEN != NULL && d->descEN[0] != '\0',
                  "catalogue_service_surErreur_descEN_non_vide");
        }
    }

    /* --- Niveau ServiceREST : les 4 autres hooks (authn/authz/init/close). --- */
    {
        CHECK(owl_service_event_catalog_find("surAuthentification") != NULL,
              "catalogue_service_surAuthentification_trouve");
        CHECK(owl_service_event_catalog_find("surAutorisation") != NULL,
              "catalogue_service_surAutorisation_trouve");
        CHECK(owl_service_event_catalog_find("FinInitialisation") != NULL,
              "catalogue_service_FinInitialisation_trouve");
        CHECK(owl_service_event_catalog_find("Fermeture") != NULL,
              "catalogue_service_Fermeture_trouve");
    }

    /* --- Nom inconnu : aucune fuite, NULL propre des deux cotes (le
     * compilateur s'appuie sur ce meme lookup pour rejeter, 
     * pt.3 — remplacement de service_event_classify). --- */
    {
        CHECK(owl_service_event_catalog_find("surTruc") == NULL,
              "catalogue_service_nom_inconnu_NULL");
        CHECK(owl_operation_event_catalog_find("surTruc") == NULL,
              "catalogue_operation_nom_inconnu_NULL");
        /* Etancheite croisee : un event operation n'est pas confondu avec
         * un event service (deux catalogues distincts, pas un seul fourre-tout). */
        CHECK(owl_service_event_catalog_find("surRequete") == NULL,
              "catalogue_service_surRequete_absent_niveau_operation_uniquement");
        CHECK(owl_operation_event_catalog_find("surErreur") == NULL,
              "catalogue_operation_surErreur_absent_niveau_service_uniquement");
    }

    /* --- Comportement fonctionnel du compilateur inchange : un service
     * declarant les 5 hooks connus + une operation avec surRequete compile
     * toujours sans erreur, desormais via lookup catalogue (pas la liste
     * en dur). Non-regression directe sur service_event_classify. --- */
    {
        compiler_reset_poo_state();
        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = SRC_SERVICE_HOOKS;
        cfg.service_mode       = 1;
        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) {
            CHECK(0, "catalogue_events_connus_compile_toujours_via_lookup");
        } else {
            CHECK(owl_http_executeur_demarre(x),
                  "catalogue_events_connus_compile_toujours_via_lookup");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }

    vm_free();
}

/* ================================================================
 * FAMILLE 25 — Structure declaree DANS le bloc ServiceREST :
 * ROUGE ATTENDU. La grammaire du corps ServiceREST n'accepte aujourd'hui
 * que `<Nom> est une OperationHTTP` au top-level du bloc -- une Structure y
 * tombe dans la branche d'erreur generique en cascade (miroir compile-time
 * des fixtures service_isolation_*.owl / service_rest_structure_dans_bloc_
 * param_valide.owl). Va au-dela du simple compile-check : prouve que le
 * TYPE de la structure est bien RESOLU pour un parametre d'operation DU
 * MEME service -- le corps JSON POST est deserialise dedans et son champ
 * relu dans la reponse servie (pas juste "ca compile", "ca route et
 * desserialise").
 * ================================================================ */

static void famille_25_structure_dans_bloc(void) {
    g_famille = 25;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 4;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_STRUCT_DANS_BLOC;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    CHECK(x != NULL, "structure_dans_bloc_compile_ok");
    if (!x) return;

    vm_init();
    CHECK(owl_http_executeur_prepare_vm(x), "structure_dans_bloc_prepare_vm_ok");

    {
        OwlHttpRep* rep = sert_deterministe(x, "POST", "/creer", "{\"nom\":\"Alice\"}");
        CHECK(rep != NULL, "structure_dans_bloc_reponse_rendue");
        if (rep) {
            Sac s; vide_reponse(rep, &s);
            CHECK(owl_http_rep_statut(rep) == 201, "structure_dans_bloc_statut_201");
            CHECK(strcmp(s.d, "Alice") == 0, "structure_dans_bloc_champ_resolu");
            owl_http_rep_libere(rep);
        }
    }

    vm_free();
    owl_http_executeur_libere(x);
}

/* ================================================================
 * FAMILLE 26 — en-tetes ENTRANTS de requete, en lecture :
 * ROUGE ATTENDU. Miroir cote lecture de la famille 8 (, cote
 * ecriture Reponse). `Entetes`/`Headers` n'est pas encore dans la liste
 * blanche 2e niveau de MoiMeme..Requete -> la compilation de surRequete
 * echoue -> le montage repond en echec sur toutes les routes -> chaque
 * CHECK ci-dessous rougit. Meme patron que les familles 0/2/6/7/8 : le
 * rouge est porte par le COMPILE, pas par une assertion trivialement
 * fausse.
 *
 * joue_e2e (socket reel) est le SEUL harnais utilise ici : c'est le seul
 * moyen fiable de poser un vrai en-tete entrant sur le fil (Authorization
 * l'a deja prouve pour les familles 7/10/11/13), OwlHttpRequete accumulant
 * deja tous les en-tetes bruts (`r->entetes[]`, http_server.c:202-204,
 * recon coordinator 2026-09-15) sans qu'aucun ne soit encore convoye
 * jusqu'a la sous-instance Requete du service.
 * ================================================================ */

static void famille_26_entetes_requete(void) {
    g_famille = 26;

    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_ENTETES_REQUETE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "req_entetes_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "req_entetes_thread_executeur_demarre");

    /* --- 1. Cas nominal : un en-tete custom (X-Forwarded-For) pose par le
     * "proxy" (mock-peer) est lu via MoiMeme..Requete..Entetes[...] et
     * refletee dans le corps -- c'est le besoin communaute d'origine
     * (IP client reelle derriere un reverse-proxy). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /echo HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 203.0.113.7\r\n\r\n", x);
        CHECK(e.statut == 200, "echo_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nfwd:203.0.113.7") != NULL,
              "echo_entete_entrant_lu_et_reflete");
    }

    /* --- 2/3. Casse-insensible en LECTURE (semantique HTTP) : le meme
     * en-tete envoye avec la graphie "X-Forwarded-For" se relit aussi bien
     * via une cle tout-minuscule que tout-majuscule. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /casse-min HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 203.0.113.7\r\n\r\n", x);
        CHECK(e.statut == 200, "casse_min_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmin:203.0.113.7") != NULL,
              "casse_min_cle_minuscule_lit_entete_envoye_mixte");
    }
    {
        EchangeE2E e = joue_e2e(
            "GET /casse-maj HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 203.0.113.7\r\n\r\n", x);
        CHECK(e.statut == 200, "casse_maj_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nmaj:203.0.113.7") != NULL,
              "casse_maj_cle_majuscule_lit_entete_envoye_mixte");
    }

    /* --- 4. En-tete absent : la cle rend la valeur par defaut du type
     * (chaine vide), comme tout tableau associatif OWL (cf.
     * docs/public_documentation/30_tableaux_associatifs.md) -- pas d'erreur,
     * pas de Null. --- */
    {
        EchangeE2E e = joue_e2e("GET /absent HTTP/1.1\r\nHost: a\r\n\r\n", x);
        CHECK(e.statut == 200, "absent_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nabs:[]") != NULL,
              "absent_cle_rend_chaine_vide_pas_erreur");
    }

    /* --- 5. En-tete repete : valeurs jointes par ", " (RFC 7230 S3.2.2),
     * decision verrouillee par le coordinator (recon 2026-09-15). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /repete HTTP/1.1\r\nHost: a\r\nX-Test: a\r\nX-Test: b\r\n\r\n", x);
        CHECK(e.statut == 200, "repete_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nrep:a, b") != NULL,
              "repete_valeurs_jointes_par_virgule_espace_rfc7230");
    }

    /* --- 6. Non-regression : Methode/Chemin restent lisibles cote Requete
     * dans le MEME handler qui lit aussi Entetes -- aucune interference
     * entre les proprietes deja whitelistees et la nouvelle. --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /combo HTTP/1.1\r\nHost: a\r\nX-Extra: valeur-extra\r\n\r\n", x);
        CHECK(e.statut == 200, "combo_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nGET|/combo|valeur-extra") != NULL,
              "combo_methode_chemin_et_entetes_coexistent");
    }

    /* --- 7. Forme accent/casse toleree au runtime (..requete..entetes[...]). --- */
    {
        EchangeE2E e = joue_e2e(
            "GET /tolerant HTTP/1.1\r\nHost: a\r\nX-Tol: ok-tolerant\r\n\r\n", x);
        CHECK(e.statut == 200, "tolerant_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\ntol:ok-tolerant") != NULL,
              "tolerant_forme_sans_accent_sans_casse_marche_au_runtime");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- Forme bilingue EN complete (Myself..Request..Headers). --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg_en;
    owl_http_exec_config_defaut(&cfg_en);
    cfg_en.file_profondeur    = 4;
    cfg_en.plafond_attente_ms = 5000;
    cfg_en.source_owl         = SRC_SERVICE_ENTETES_REQUETE_EN;
    cfg_en.service_mode       = 1;

    OwlHttpExecuteur* x_en = owl_http_executeur_cree(&cfg_en);
    if (!x_en) { CHECK(0, "req_entetes_en_cree"); return; }

    CHECK(owl_http_executeur_demarre(x_en), "req_entetes_en_thread_executeur_demarre");
    {
        EchangeE2E e = joue_e2e(
            "GET /en HTTP/1.1\r\nHost: a\r\nX-Foo: baz\r\n\r\n", x_en);
        CHECK(e.statut == 200, "req_entetes_en_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nbar:baz") != NULL,
              "req_entetes_en_headers_lu_sur_le_fil");
    }

    owl_http_executeur_arrete(x_en);
    owl_http_executeur_libere(x_en);
}

/* ================================================================
 * FAMILLE 27 — CLAIMS JWT aud/nbf (, complement famille 13)
 *
 * `aud` (audience) est OPTIONNEL : valide uniquement si le service
 * configure `AudienceJWT`. Le claim token peut etre une chaine OU un
 * tableau de chaines (RFC 7519 4.1.3) -- match si l'audience configuree
 * egale la chaine, ou est presente dans le tableau. `nbf` (not-before)
 * est valide des qu'il est present dans le token (rejet si nbf > now),
 * jamais exige (absent -> pas de rejet), meme patron dynamique
 * (time(NULL)+delta) que exp en famille 13. ROUGE ATTENDU aujourd'hui :
 * `AudienceJWT` n'est reconnue nulle part -> montage du bloc A echoue en
 * cascade (meme patron que la famille 13 avant httpAuthJWT) ; `nbf` est
 * lu par `service_jwt_valide` mais jamais valide -> cas 7 (futur) passe
 * a tort en 200 tant que le check n'existe pas.
 * ================================================================ */

static void famille_27_jwt_claims_aud_nbf(void) {
    g_famille = 27;

    char jwt[4096];
    char payload[512];
    char req[8192];

    /* --- BLOC A : service AVEC AudienceJWT="mon-service" configure. --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH_JWT_AUDIENCE;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "aud_cree"); return; }

    CHECK(owl_http_executeur_demarre(x), "aud_thread_executeur_demarre");

    /* --- 1. aud = "mon-service" (chaine) -> 200, sub relu. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-aud-1", "\"mon-service\"", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "aud_chaine_ok_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 200, "aud_chaine_correspond_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-aud-1") != NULL,
              "aud_chaine_correspond_claim_sub_relu");
    }

    /* --- 2. aud = "autre-service" (chaine, mauvaise audience) -> 401. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-aud-2", "\"autre-service\"", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "aud_chaine_mauvaise_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "aud_chaine_mauvaise_401");
    }

    /* --- 3. aud = ["x","mon-service","y"] (tableau contenant l'audience)
     * -> 200. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-aud-3", "[\"x\",\"mon-service\",\"y\"]", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "aud_tableau_contient_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 200, "aud_tableau_contient_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-aud-3") != NULL,
              "aud_tableau_contient_claim_sub_relu");
    }

    /* --- 4. aud = ["x","y"] (tableau SANS l'audience) -> 401. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-aud-4", "[\"x\",\"y\"]", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "aud_tableau_absent_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "aud_tableau_absent_401");
    }

    /* --- 5. aud absent alors que AudienceJWT configure -> 401 (exige des
     * qu'on configure une audience). --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-aud-5", NULL, 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "aud_absent_exige_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x);
        CHECK(e.statut == 401, "aud_absent_alors_qu_exige_401");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- BLOC B : service SANS AudienceJWT configure -- non-regression
     * (aud ignore) + corpus nbf. --- */
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg2;
    owl_http_exec_config_defaut(&cfg2);
    cfg2.file_profondeur    = 8;
    cfg2.plafond_attente_ms = 5000;
    cfg2.source_owl         = SRC_SERVICE_AUTH_JWT;
    cfg2.service_mode       = 1;

    OwlHttpExecuteur* x2 = owl_http_executeur_cree(&cfg2);
    if (!x2) { CHECK(0, "nbf_cree"); return; }

    CHECK(owl_http_executeur_demarre(x2), "nbf_thread_executeur_demarre");

    /* --- 6. aud present sur un service SANS AudienceJWT -> ignore, 200
     * (non-regression cle : les services JWT existants ne changent
     * rien). --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-non-config", "\"peu-importe\"", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)),
              "aud_non_configure_ignore_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x2);
        CHECK(e.statut == 200, "aud_non_configure_ignore_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-non-config") != NULL,
              "aud_non_configure_ignore_claim_sub_relu");
    }

    /* --- 7. nbf dans le futur (now + 3600) -> 401, pas encore valide. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-nbf-futur", NULL, 1, 3600);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "nbf_futur_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x2);
        CHECK(e.statut == 401, "nbf_futur_401_pas_encore_valide");
    }

    /* --- 8. nbf dans le passe (now - 60) -> 200, deja valide. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-nbf-passe", NULL, 1, -60);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "nbf_passe_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x2);
        CHECK(e.statut == 200, "nbf_passe_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-nbf-passe") != NULL,
              "nbf_passe_claim_sub_relu");
    }

    /* --- 9. nbf absent -> 200, nbf optionnel RFC. --- */
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-nbf-absent", NULL, 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "nbf_absent_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e(req, x2);
        CHECK(e.statut == 200, "nbf_absent_statut_200");
        CHECK(strstr(e.reponse, "\r\n\r\nsub:user-nbf-absent") != NULL,
              "nbf_absent_claim_sub_relu");
    }

    owl_http_executeur_arrete(x2);
    owl_http_executeur_libere(x2);
}

/* ================================================================
 * FAMILLE 28 — JWKS : rotation de cles (, PHASE 1)
 *
 * `URLJWKS`/`JWKSURL` est une propriete DSL NOUVELLE, absente du parseur de
 * service (compiler.c ~19590) : toute PROPRIETES qui la mentionne echoue a
 * la COMPILATION (E_SEM_DSL_SERVICE_PROP_UNKNOWN) -> owl_http_executeur_demarre
 * rend Faux (vm_interpret != INTERPRET_OK, cf. http_exec.c:2264). C'est le
 * ROUGE DISCRIMINANT de toute cette famille : chaque scenario positif (kid
 * connu, rotation, serve-stale) est aujourd'hui bloque a cette meme premiere
 * marche, et devra la franchir (demarre == Vrai) une fois l'impl posee,
 * AVANT que son assertion reseau propre ait un sens.
 *
 * DEUX scenarios (exclusivite de config, http non-loopback refuse) sont des
 * INVARIANTS DE SECURITE PERMANENTS : ils doivent rester en echec (refus a
 * la compilation OU 401 au runtime) AVANT ET APRES l'impl. Ce ne sont PAS
 * des rouges-qui-deviendront-verts -- marques explicitement plus bas.
 *
 * Serveur JWKS de test : pair TCP brut en thread (meme patron que
 * http_jar_redirect_test.c::jr_thread_fn), sert une reponse JSON canned a
 * chaque connexion. La cle publique exposee (n/e base64url) est celle du
 * keypair RSA-2048 de la famille 13 (JWT_TEST_CLE_PRIVEE_PEM), extraite
 * hors-ligne via openssl (modulus/exposant, jamais recalcules ici). Une
 * SECONDE cle RSA-2048, generee uniquement pour ce fichier, sert a fabriquer
 * une signature invalide (cas 3 : mauvaise cle, meme kid).
 * ================================================================ */

/* Meme modulus/exposant que JWT_TEST_CLE_PRIVEE_PEM (famille 13), en
 * base64url RFC 7517 (n/e d'un JWK RSA) -- extraits hors-ligne via
 * `openssl rsa -pubin -modulus`, jamais recalcules au runtime. */
#define JWKS_TEST_N_B64URL \
    "tx-PRHZy5E5mCY32A0eyunUDImxklHs0-USkTtvNCSwFRvdoID-krNcKCEjGGWjAKUSV9By" \
    "cP1mpcYslpHHVEoF6-XY5Ug-ff27JUSKEkY3E21n-Ym_5br5sR7-y-DtnBGondeT8YnoRxZ" \
    "wWH7WcUdWX7UdvCDzW9Ej4Jh5aYrwyKketeytWRMFyguuxBW4zBp_OZgSAJG_r-f1eLvNyB" \
    "YW-JnEluHqJ4nb8lyHJx5cftOeKnulXQt4BdYNqswIGnNuAlIScCKeoXiLB8n9_P0ZpHAtj" \
    "ryz8t_AMSDlTI4cH8OiU1CO1tMnBzBc_yRV-cgH1lF8QlTUk4m6fU5mC9Q"
#define JWKS_TEST_E_B64URL "AQAB"

/* Deuxieme keypair RSA-2048, SANS RAPPORT avec celui de la famille 13 :
 * jamais publie dans le JWKS servi ci-dessous, sa cle privee sert
 * UNIQUEMENT a signer un jeton dont le kid pretend correspondre a K1 (cas 3,
 * mauvaise cle / bon kid -> signature qui ne verifie jamais). */
static const char* JWKS_TEST_CLE_PRIVEE_AUTRE_PEM =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDG1vJ/jYwsJouN\n"
    "FeZm7XtkfpKNP+SWgr+yUilTNAgwvZDCEJnlbOVzVb81FMo89kIzCb5ORzEZ1X+u\n"
    "bAKs38BCdxOnZFmFRTol/94nsahfPFhFcs5brKUvrrWBzd7oksfrJq2FYPoWiMck\n"
    "f84+DTwftbSqS/FdTlfwzttWL3qtsuara9AFTrVUkpc19oZkvfJi3t/AZEb5yDp3\n"
    "4JACZgjQEU1D0rb2Yb4brKvxpcgUB6RlozjECLqibBmDZY5TL7kvWsJDzyQV2tHV\n"
    "izKo93phOdasevsPNoyKNQjp611Wua90DawyHeHEAlt5AYnBK/oPFddAUuCyg3jy\n"
    "u8afRcCdAgMBAAECggEAN4jRMc7W133se/sh3p11WRwA0nKF5YOfBq7r29EseHB5\n"
    "fbolQW6opw3L2Yd09JHS625NOI+ppw16G70PAjl7+RRI6PeeCRrMotBTu8JixDaW\n"
    "cM8f03rkp+yjxR06pLpaG9Mxo47ySljpRchsUehgYnrderK7NtQqu5cKq4eXO+u9\n"
    "HoZRN3FLC/2kuPlaUF7lPIyYmlKx8e+Vrg6BKftmZ1ipcUUmuvtNKtmeZU3TqBxp\n"
    "gutA3J649LROwMgolC//SqMX/ZpmQoOhWM9NzXiFd1rx5cpz888X+i+qU/QRXTFR\n"
    "hMLW0Bnw/OsHcrdZTuvGjttGqLVZXDr8fHe9loHCAQKBgQDzQGkni8Dc7y95EYsA\n"
    "XTt4QPbH/4YAO5GhU7zzm6Qrc27L5Se0guc6mZZXXJ7Xz7gw69jTgKl6mAn3Z5Br\n"
    "rt0/cReXrHG4MSUDpcztovFjBPI/QoXlDPUQezscmTHBiPPriJL9so8plJ5T9Y5K\n"
    "mOjX7shbmJNf2SPtqRZQo0QfPwKBgQDRQq6+4j8Kj1XnrA4pspHW1up7dfgQE6yl\n"
    "QhEMZQPfj6s4ujZPbaHIyvGAdRR2rF2GutfE7eU20VvRs64RPQqYQHd+RhnJeZtY\n"
    "h62eIa8AUDCjCjtwcE+kNVvfu7ZmwE71ShNb4jt1oK1d7DG8Yl956+nTUd+yuqez\n"
    "r9pa7pbFIwKBgQCoTy7zH+3IcO8mFA9bpybY5ApdA96SFqWDPM+Gt3B2aB6Hkf1R\n"
    "FyorD85IkFuP87f8kZOrdGDP5JcwykXRXGLV3Mh4N+dPjSGRTo1m99GRtABx6POA\n"
    "MuD2EG4sMYuN39b2Z79dLsmD+cM7ULKtxvOFRUD4r695KGOFs/MuxGkEPQKBgAfj\n"
    "0sDhuOzGAMAubY5G7FbEXazwAr1gcIb+mbP07xNz0EngQ4H1KT9v33EveOkMkK4u\n"
    "x0nmiljpxeN/OdXujkK1U4wQSpKdIi1RWEt5C6cgXuUW0qznnyka1e72neb74z4+\n"
    "X8J8Ii/KMh8zc6CduOnY6kUMdjm/7ejJfZsmGcsvAoGBAJhh3iuPxskqTGl5bJel\n"
    "Gs6YI9DmH0PnSVBcG6xrbnywJv+rxEKyUS7nX/icubftYmFMIngSxWw0I9DOlHlW\n"
    "/r/+scyskLEEmvUGy8TcilN3luzRk+aWQ5rCOjciK80PTiLjyBIVCjaqrwJYj1kU\n"
    "jzvfqIjFQE4cjoO1jajDrCk8\n"
    "-----END PRIVATE KEY-----\n";

/* JWKS canne : un seul JWK RSA, kid=K1, clef publique de la famille 13. */
static const char* JWKS_BODY_K1 =
    "{\"keys\":[{\"kty\":\"RSA\",\"kid\":\"K1\",\"use\":\"sig\",\"alg\":\"RS256\","
    "\"n\":\"" JWKS_TEST_N_B64URL "\",\"e\":\"" JWKS_TEST_E_B64URL "\"}]}";

/* JWKS canne : un seul JWK, kid=K3, kty=EC -- valeurs x/y d'exemple (RFC 7517
 * appendice A.1), sans importance puisque le rejet doit se faire sur `kty`
 * SEUL, avant toute tentative d'usage de la cle. */
static const char* JWKS_BODY_K3_EC =
    "{\"keys\":[{\"kty\":\"EC\",\"kid\":\"K3\",\"crv\":\"P-256\","
    "\"x\":\"MKBCTNIcKUSDii11ySs3526iDZ8AiTo7Tu6KPAqv7D4\","
    "\"y\":\"4Etl6SRW2YiLUrN5vfvVHuhp7x8PxltmWWlbbM4IFGg\"}]}";

/* Signe header_json + "." + payload_json en RS256 avec une cle privee PEM
 * ARBITRAIRE (variante parametree de jwt_signe_rs256_ex, famille 13) --
 * seul le cas 3 (mauvaise cle, bon kid) en a besoin, jamais utilisee pour
 * un jeton cense passer. */
static int jwt_signe_avec_cle_pem(const char* cle_privee_pem,
                                   const char* header_json, const char* payload_json,
                                   char* dst, size_t dst_cap) {
    char h64[1024], p64[2048];
    jwt_b64url((const unsigned char*)header_json, strlen(header_json), h64, sizeof(h64));
    jwt_b64url((const unsigned char*)payload_json, strlen(payload_json), p64, sizeof(p64));

    char signing_input[4096];
    snprintf(signing_input, sizeof(signing_input), "%s.%s", h64, p64);

    unsigned char hash[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char*)signing_input, strlen(signing_input), hash) != 0) {
        return 0;
    }

    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_pk_context       pk;
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_pk_init(&pk);

    int ok = 0;
    unsigned char sig[512];
    size_t sig_len = 0;

    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char*)"owl-jwks-test", 13) != 0) goto fin;
    if (mbedtls_pk_parse_key(&pk, (const unsigned char*)cle_privee_pem,
                             strlen(cle_privee_pem) + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                        sig, sizeof(sig), &sig_len,
                        mbedtls_ctr_drbg_random, &drbg) != 0) goto fin;

    {
        char sig64[1024];
        jwt_b64url(sig, sig_len, sig64, sizeof(sig64));
        snprintf(dst, dst_cap, "%s.%s", signing_input, sig64);
        ok = 1;
    }

fin:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    return ok;
}

/* Construit un en-tete JWT avec un `kid` explicite. */
static void jwt_header_avec_kid(char* dst, size_t cap, const char* alg, const char* kid) {
    snprintf(dst, cap, "{\"alg\":\"%s\",\"typ\":\"JWT\",\"kid\":\"%s\"}", alg, kid);
}

/* ---- Serveur JWKS loopback (pair TCP brut, meme patron que
 * http_jar_redirect_test.c::jr_thread_fn) ---- */

typedef struct {
    int  port;
    volatile int pret;
    volatile int stop;
    int  demarre;
    const char* corps;     /* JSON JWKS servi a CHAQUE connexion, statique */
    int  nb_servis;
    jwks_thread_t thread;
} ServeurJWKS;

static void jwks_init(ServeurJWKS* s, const char* corps) {
    memset(s, 0, sizeof(*s));
    s->corps = corps;
}

static int jwks_write_all(jwks_socket_t sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static void jwks_lit_requete(jwks_socket_t cli) {
    char buf[4096];
    int len = 0;
    while (len < (int)sizeof(buf) - 1) {
        int n = recv(cli, buf + len, (int)sizeof(buf) - 1 - len, 0);
        if (n <= 0) break;
        len += n;
        buf[len] = '\0';
        if (len >= 4 && strstr(buf, "\r\n\r\n") != NULL) break;
    }
}

static void jwks_ecrit_reponse(jwks_socket_t cli, const char* corps) {
    char rep[8192];
    int blen = (int)strlen(corps);
    int n = snprintf(rep, sizeof(rep),
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        blen, corps);
    if (n > 0) jwks_write_all(cli, rep, n);
}

#ifdef _WIN32
static DWORD WINAPI jwks_thread_fn(LPVOID arg) {
#else
static void* jwks_thread_fn(void* arg) {
#endif
    ServeurJWKS* s = (ServeurJWKS*)arg;

    jwks_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv != JWKS_INVALID) {
        int un = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&un, sizeof(un));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            socklen_t alen = sizeof(addr);
            if (getsockname(srv, (struct sockaddr*)&addr, &alen) == 0 &&
                listen(srv, 8) == 0) {
                s->port = ntohs(addr.sin_port);
                s->pret = 1;
                while (!s->stop) {
                    fd_set r;
                    FD_ZERO(&r);
                    FD_SET(srv, &r);
                    struct timeval att; att.tv_sec = 0; att.tv_usec = 100 * 1000;
                    if (select((int)srv + 1, &r, NULL, NULL, &att) <= 0) continue;
                    jwks_socket_t cli = accept(srv, NULL, NULL);
                    if (cli == JWKS_INVALID) continue;
                    jwks_lit_requete(cli);
                    jwks_ecrit_reponse(cli, s->corps);
                    shutdown(cli, JWKS_SHUT_WR);
                    char poubelle[256];
                    int garde = 0;
                    while (garde++ < 16) {
                        int n = recv(cli, poubelle, (int)sizeof(poubelle), 0);
                        if (n <= 0) break;
                    }
                    JWKS_CLOSE(cli);
                    s->nb_servis++;
                }
            }
        }
        JWKS_CLOSE(srv);
    }
    s->pret = 1;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int jwks_demarre(ServeurJWKS* s) {
#ifdef _WIN32
    s->thread = CreateThread(NULL, 0, jwks_thread_fn, s, 0, NULL);
    if (s->thread == NULL) return 0;
#else
    if (pthread_create(&s->thread, NULL, jwks_thread_fn, s) != 0) return 0;
#endif
    s->demarre = 1;
    int attendu = 0;
    while (!s->pret && attendu < 2000) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10 * 1000);
#endif
        attendu += 10;
    }
    return s->port > 0;
}

static void jwks_arrete(ServeurJWKS* s) {
    if (!s->demarre) return;
    s->stop = 1;
#ifdef _WIN32
    WaitForSingleObject(s->thread, 10000);
    CloseHandle(s->thread);
#else
    pthread_join(s->thread, NULL);
#endif
    s->demarre = 0;
}

/* Construit la source d'un ServiceREST auth JWKS pointant vers le port
 * donne (loopback), avec des proprietes additionnelles optionnelles
 * (ex : ClePubliqueJWT en plus, pour le cas d'exclusivite). */
static void construit_src_jwks(char* dst, size_t cap, const char* url_jwks,
                                const char* props_supplementaires) {
    snprintf(dst, cap,
        "ServiceAuthJWKS est un ServiceREST\n"
        "    PROPRIETES\n"
        "        Authentification = httpAuthJWT\n"
        "        URLJWKS = \"%s\"\n"
        "        EmetteurJWT = \"owl-test-emetteur\"\n"
        "%s"
        "    FIN\n"
        "    Sonde est une OperationHTTP\n"
        "        PROPRIETES\n"
        "            Methode  = httpGet\n"
        "            Chemin   = [\"protegee\"]\n"
        "            Reponses = [httpOK, httpNonAutorise]\n"
        "            CodeEtat = httpOK\n"
        "        FIN\n"
        "        PROCEDURE surRequete()\n"
        "            MoiMeme..Reponse..CodeEtat = httpOK\n"
        "            MoiMeme..Reponse..Corps = \"sub:\" + MoiMeme..Requete..Identite[\"sub\"]\n"
        "        FIN\n"
        "    FIN\n"
        "FIN\n",
        url_jwks, props_supplementaires ? props_supplementaires : "");
}

/* Un service dont l'unique port injoignable est obtenu en demarrant puis en
 * arretant tout de suite un serveur JWKS reel : le port a existe, plus rien
 * n'ecoute dessus -> connexion refusee immediate, sans dependre du reseau
 * externe ni d'un port au hasard. */
static int obtiens_port_injoignable(void) {
    ServeurJWKS tmp;
    jwks_init(&tmp, JWKS_BODY_K1);
    if (!jwks_demarre(&tmp)) return 0;
    int port = tmp.port;
    jwks_arrete(&tmp);
    return port;
}

static void famille_28_jwks_rotation_cles(void) {
    g_famille = 28;

    char jwt[4096];
    char hdr[256];
    char payload[512];
    char req[8192];
    char src[2048];

    /* --- 1. HAPPY PATH : JWKS servi contient kid=K1, JWT signe avec la cle
     * privee correspondante + en-tete kid=K1 -> 200, claim sub relu. --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K1);
        CHECK(jwks_demarre(&srv), "jwks_happy_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_happy_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_happy_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K1");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-1");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_happy_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 200, "jwks_happy_statut_200");
            CHECK(strstr(e.reponse, "\r\n\r\nsub:user-jwks-1") != NULL,
                  "jwks_happy_claim_sub_relu");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        jwks_arrete(&srv);
    }

    /* --- 2. KID INCONNU : JWKS ne contient que K1, JWT signe (valide) avec
     * en-tete kid=K2 -> 401 apres re-fetch qui ne le trouve pas. --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K1);
        CHECK(jwks_demarre(&srv), "jwks_kidinconnu_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_kidinconnu_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_kidinconnu_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K2");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-2");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_kidinconnu_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 401, "jwks_kidinconnu_401");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        jwks_arrete(&srv);
    }

    /* --- 3. SIGNATURE INVALIDE : meme kid=K1 dans le JWKS, mais le jeton
     * est signe avec une AUTRE cle privee (mismatch cle publique/privee)
     * -> 401. --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K1);
        CHECK(jwks_demarre(&srv), "jwks_sigmauvaise_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_sigmauvaise_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_sigmauvaise_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K1");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-3");
            CHECK(jwt_signe_avec_cle_pem(JWKS_TEST_CLE_PRIVEE_AUTRE_PEM, hdr, payload, jwt, sizeof(jwt)),
                  "jwks_sigmauvaise_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 401, "jwks_sigmauvaise_401");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        jwks_arrete(&srv);
    }

    /* --- 4. JWKS INJOIGNABLE, AUCUN CACHE : le port a existe (serveur
     * demarre puis arrete avant toute requete) -> connexion refusee au
     * tout premier fetch -> FAIL-CLOSED, 401 (jamais fail-open). --- */
    {
        compiler_reset_poo_state();
        int port_mort = obtiens_port_injoignable();
        CHECK(port_mort > 0, "jwks_injoignable_port_obtenu");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", port_mort);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_injoignable_cree"); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_injoignable_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K1");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-4");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_injoignable_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 401, "jwks_injoignable_401_fail_closed");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
    }

    /* --- 5. SERVE-STALE (partiel, cf. escalade rapport) : 1er appel peuple
     * le cache (200), le serveur JWKS est ensuite arrete, un 2e appel dans
     * la foulee (bien avant tout hard-max de staleness) doit rester servi
     * -- 200 -- sur les dernieres cles connues. Le hard-max de staleness
     * n'est PAS teste ici (fenetre temporelle non deterministe sans horloge
     * de test dediee -- cf. rapport). --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K1);
        CHECK(jwks_demarre(&srv), "jwks_stale_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_stale_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_stale_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K1");

            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-5a");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_stale_premier_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e1 = joue_e2e(req, x);
            CHECK(e1.statut == 200, "jwks_stale_premier_appel_peuple_cache_200");

            jwks_arrete(&srv);   /* le serveur JWKS devient injoignable */

            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-5b");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_stale_second_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e2 = joue_e2e(req, x);
            CHECK(e2.statut == 200, "jwks_stale_second_appel_serve_stale_200");
            CHECK(strstr(e2.reponse, "\r\n\r\nsub:user-jwks-5b") != NULL,
                  "jwks_stale_second_appel_claim_sub_relu");
        } else {
            jwks_arrete(&srv);
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
    }

    /* --- 6a. RS256 PINNING (cote JWT) CONSERVE EN MODE JWKS : alg=none,
     * kid=K1 present et valide dans le JWKS -> 401 (rejet sur l'algo SEUL,
     * meme faille historique que famille 13 cas 2, revalidee ici sous
     * JWKS). --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K1);
        CHECK(jwks_demarre(&srv), "jwks_algnone_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_algnone_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_algnone_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "none", "K1");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-6a");
            char h64[1024], p64[2048];
            jwt_b64url((const unsigned char*)hdr, strlen(hdr), h64, sizeof(h64));
            jwt_b64url((const unsigned char*)payload, strlen(payload), p64, sizeof(p64));
            snprintf(jwt, sizeof(jwt), "%s.%s.", h64, p64);
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 401, "jwks_algnone_401");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        jwks_arrete(&srv);
    }

    /* --- 6b. kty JWKS != "RSA" : le JWK dont le kid matche est de type EC
     * -- rejet sur le TYPE de cle seul, meme si le jeton porte une vraie
     * signature RS256 valide (signee avec la cle privee K1, mais K1 n'est
     * PAS dans ce JWKS -- kid=K3 la seule entree, kty=EC). -> 401. --- */
    {
        compiler_reset_poo_state();
        ServeurJWKS srv;
        jwks_init(&srv, JWKS_BODY_K3_EC);
        CHECK(jwks_demarre(&srv), "jwks_ktyec_serveur_local_demarre");

        char url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/jwks.json", srv.port);
        construit_src_jwks(src, sizeof(src), url, NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(0, "jwks_ktyec_cree"); jwks_arrete(&srv); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(demarre_ok, "jwks_ktyec_demarre");
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K3");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-6b");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_ktyec_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            CHECK(e.statut == 401, "jwks_ktyec_401_rejet_type_cle");
        }

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
        jwks_arrete(&srv);
    }

    /* --- 7. CONFIG EXCLUSIVE : un service qui pose A LA FOIS URLJWKS ET
     * ClePubliqueJWT -> ERREUR DE COMPILATION (miroir de l'exclusion
     * URLJWKS/surAuthentification, compiler.c:19904). INVARIANT DE
     * SECURITE PERMANENT : demarre() doit rendre Faux AVANT (URLJWKS
     * simplement inconnue) ET APRES l'impl (nouvelle regle d'exclusivite
     * dediee) -- ce test reste VERT dans les deux etats, volontairement. --- */
    {
        compiler_reset_poo_state();
        char props[512];
        snprintf(props, sizeof(props), "        ClePubliqueJWT = %s\n", JWT_TEST_CLE_PUBLIQUE_PEM_OWL);
        construit_src_jwks(src, sizeof(src), "http://127.0.0.1:1/jwks.json", props);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_exclusif_cree_echoue_deja_refus"); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        CHECK(!demarre_ok, "jwks_exclusif_urljwks_et_clepublique_refuse_invariant_permanent");

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
    }

    /* --- 8. HTTP NON-LOOPBACK REFUSE : URLJWKS en http:// vers un hote
     * DISTANT (pas 127.0.0.1/localhost) -> refus (compilation OU runtime
     * 401, selon le choix d'impl -- les deux sont un refus valide).
     * INVARIANT DE SECURITE PERMANENT, meme statut vert avant/apres impl.
     * Hote choisi en IP documentaire (RFC 5737 TEST-NET-3) pour ne DEPENDRE
     * d'aucune resolution DNS ni d'un hote reellement joignable. --- */
    {
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://203.0.113.1/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_httpdistant_cree_echoue_deja_refus"); return; }

        int demarre_ok = owl_http_executeur_demarre(x);
        int refuse = !demarre_ok;
        if (demarre_ok) {
            jwt_header_avec_kid(hdr, sizeof(hdr), "RS256", "K1");
            jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-jwks-8");
            CHECK(jwt_signe_rs256(hdr, payload, jwt, sizeof(jwt)), "jwks_httpdistant_signe");
            snprintf(req, sizeof(req),
                     "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
            EchangeE2E e = joue_e2e(req, x);
            refuse = (e.statut == 401);
        }
        CHECK(refuse, "jwks_httpdistant_refuse_invariant_permanent");

        owl_http_executeur_arrete(x);
        owl_http_executeur_libere(x);
    }

    /* --- 9-12. BYPASS "127." PAR PREFIXE (verdict reviewer BLOCKED,
     * ) : `service_jwks_url_forme_ok` (compiler.c) verifie
     * aujourd'hui `strncmp(host, "127.", 4) == 0` -- un simple PREFIXE, pas
     * une adresse IPv4 complete. Un nom DNS enregistrable comme
     * "127.evil.com" satisfait ce prefixe et est donc accepte comme
     * "loopback" A LA COMPILATION, alors qu'a l'execution parse_url/
     * connect_tcp (stdlib_http.c) resolvent ce nom DNS normalement et se
     * connectent en clair a l'hote reel -- exactement l'invariant que la
     * fonction pretend garantir qui est defait.
     *
     * Ces 4 cas assertent le refus STRICTEMENT AU NIVEAU COMPILATION
     * (demarre_ok doit etre Faux), sans jamais emprunter le chemin reseau :
     * un refus qui ne surviendrait qu'a l'execution (ex: DNS injoignable
     * pour un nom fictif) masquerait exactement le bypass vise -- le
     * controle compile-time doit rejeter la FORME, independamment de ce que
     * le reseau ferait ensuite. Rouge attendu tant que la garde reste un
     * simple prefixe ; passe au vert quand le fix exige un match EXACT
     * d'une adresse IPv4 valide apres "127.". */
    {
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://127.evil.com/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_bypass_127dns_cree_echoue_deja_refus"); }
        else {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(!demarre_ok, "jwks_bypass_127dns_refuse_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }
    {
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://127.0.0.1.attacker.io/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_bypass_127suffixe_cree_echoue_deja_refus"); }
        else {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(!demarre_ok, "jwks_bypass_127suffixe_refuse_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }
    {
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://127.0.0.1@evil.com/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_bypass_userinfo_cree_echoue_deja_refus"); }
        else {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(!demarre_ok, "jwks_bypass_userinfo_refuse_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }
    {
        /* La branche "localhost" fait deja un match EXACT (hlen == 9) : ce
         * cas est donc deja refuse aujourd'hui (pas un bypass actif), mais
         * merite un verrou de non-regression cote localhost, symetrique
         * au fix apporte cote "127.". */
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://localhost.evil.com/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        if (!x) { CHECK(1, "jwks_bypass_localhostsuffixe_cree_echoue_deja_refus"); }
        else {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(!demarre_ok, "jwks_bypass_localhostsuffixe_refuse_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }

    /* --- 13-15. NON-REGRESSION : le fix (match EXACT d'une IPv4 dans
     * 127.0.0.0/8) ne doit PAS sur-rejeter un loopback legitime autre que
     * 127.0.0.1, ni changer le comportement https:// distant deja permis.
     * Le cas 127.0.0.1:<port> (loopback canon) reste couvert par le
     * happy-path (cas 1) -- pas duplique ici. --- */
    {
        /* 13. Adresse valide dans 127.0.0.0/8 autre que 127.0.0.1 -> doit
         * rester acceptee a la compilation (aucun fetch reseau necessaire
         * pour verifier la FORME). */
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "http://127.1.2.3/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        CHECK(x != NULL, "jwks_nonregression_127autre_cree");
        if (x) {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(demarre_ok, "jwks_nonregression_127autre_accepte_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }
    {
        /* 14. https:// distant reste inchange, quel que soit l'hote. */
        compiler_reset_poo_state();
        construit_src_jwks(src, sizeof(src), "https://example.com/jwks.json", NULL);

        OwlHttpExecConfig cfg;
        owl_http_exec_config_defaut(&cfg);
        cfg.file_profondeur    = 8;
        cfg.plafond_attente_ms = 5000;
        cfg.source_owl         = src;
        cfg.service_mode       = 1;

        OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
        CHECK(x != NULL, "jwks_nonregression_httpsdistant_cree");
        if (x) {
            int demarre_ok = owl_http_executeur_demarre(x);
            CHECK(demarre_ok, "jwks_nonregression_httpsdistant_accepte_a_la_compilation");
            owl_http_executeur_arrete(x);
            owl_http_executeur_libere(x);
        }
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

/* ================================================================
 * FAMILLE 29 — RAISON D'ECHEC D'AUTH SUR LA SORTIE SERVEUR
 *
 * Un rejet d'auth JWT ecrit la RAISON precise sur le canal diagnostic
 * serveur (stderr, meme canal que la Trace de crash d'un handler). C'est
 * pour l'operateur/dev. SECURITE non negociable : la raison ne fuit JAMAIS
 * dans la reponse au client -- le 401 reste opaque (corps + en-tetes
 * inchanges). On verrouille les DEUX cotes a chaque cas :
 *   (a) la sortie serveur capturee CONTIENT la raison, discriminee par
 *       branche (exp != signature != audience) ;
 *   (b) la reponse client NE contient PAS la raison.
 *
 * Capture stderr par dup/dup2 autour de joue_e2e (meme patron que la
 * capture stdout de la famille 9). La raison est ecrite par l'executeur
 * AVANT l'emission du 401 : quand joue_e2e a lu la reponse complete, le
 * log est deja sur le fd.
 * ================================================================ */

static EchangeE2E joue_e2e_capture_stderr(const char* octets, OwlHttpExecuteur* x,
                                          char* log_dst, size_t log_cap) {
    if (log_dst && log_cap) log_dst[0] = '\0';

    char TMP[128];
    fixture_unique(TMP, sizeof TMP, "_owl_dsl_service_rejet_auth_", ".tmp");

    fflush(stderr);
    int saved_fd = owl_dup(owl_fileno(stderr));
    int tmp_fd = (saved_fd >= 0)
        ? owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644)
        : -1;
    if (saved_fd < 0 || tmp_fd < 0) {
        if (saved_fd >= 0) owl_close(saved_fd);
        return joue_e2e(octets, x);   /* pas de capture : le test rougira sur log vide */
    }
    owl_dup2(tmp_fd, owl_fileno(stderr));
    owl_close(tmp_fd);

    EchangeE2E e = joue_e2e(octets, x);

    fflush(stderr);
    owl_dup2(saved_fd, owl_fileno(stderr));
    owl_close(saved_fd);

    if (log_dst && log_cap) {
        FILE* r = fopen(TMP, "rb");
        if (r) {
            size_t n = fread(log_dst, 1, log_cap - 1, r);
            log_dst[n] = '\0';
            fclose(r);
        }
    }
    owl_unlink(TMP);
    return e;
}

static void famille_29_raison_echec_auth_log_serveur(void) {
    g_famille = 29;
    compiler_reset_poo_state();

    OwlHttpExecConfig cfg;
    owl_http_exec_config_defaut(&cfg);
    cfg.file_profondeur    = 8;
    cfg.plafond_attente_ms = 5000;
    cfg.source_owl         = SRC_SERVICE_AUTH_JWT;
    cfg.service_mode       = 1;

    OwlHttpExecuteur* x = owl_http_executeur_cree(&cfg);
    if (!x) { CHECK(0, "raison_auth_cree"); return; }
    CHECK(owl_http_executeur_demarre(x), "raison_auth_thread_executeur_demarre");

    char jwt[4096];
    char payload[512];
    char req[8192];
    char log[1024];

    /* --- 1. JWT expire -> 401. La sortie serveur contient la raison
     * ("expir") ET le prefixe "rejet auth" ; le corps client NE contient
     * PAS la raison (opaque, non negociable). --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, -3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "raison_exp_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e_capture_stderr(req, x, log, sizeof(log));
        CHECK(e.statut == 401, "raison_exp_statut_401");
        CHECK(strstr(log, "rejet auth") != NULL, "raison_exp_log_serveur_prefixe");
        CHECK(strstr(log, "expir") != NULL, "raison_exp_log_serveur_contient_raison");
        CHECK(strstr(e.reponse, "expir") == NULL, "raison_exp_corps_client_opaque");
    }

    /* --- 2. Signature falsifiee -> 401. Raison "signature" cote serveur,
     * absente cote client. Prouve la DISCRIMINATION par branche : le log ne
     * contient pas "expir" cette fois. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        CHECK(jwt_signe_rs256_ex(JWT_HDR_RS256, payload, 1, jwt, sizeof(jwt)),
              "raison_sig_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e_capture_stderr(req, x, log, sizeof(log));
        CHECK(e.statut == 401, "raison_sig_statut_401");
        CHECK(strstr(log, "signature") != NULL, "raison_sig_log_serveur_contient_raison");
        CHECK(strstr(log, "expir") == NULL, "raison_sig_log_discrimine_pas_exp");
        CHECK(strstr(e.reponse, "signature") == NULL, "raison_sig_corps_client_opaque");
    }

    /* --- 3. Non-regression : JWT valide -> 200, AUCUN rejet logge. --- */
    {
        jwt_payload(payload, sizeof(payload), "owl-test-emetteur", 1, 3600, "user-42");
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "raison_valide_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e_capture_stderr(req, x, log, sizeof(log));
        CHECK(e.statut == 200, "raison_valide_statut_200");
        CHECK(strstr(log, "rejet auth") == NULL, "raison_valide_aucun_rejet_logge");
    }

    owl_http_executeur_arrete(x);
    owl_http_executeur_libere(x);

    /* --- 4. Audience invalide (service AudienceJWT) -> 401. Raison
     * "audience" cote serveur, opaque cote client. 3e branche distincte. --- */
    compiler_reset_poo_state();
    OwlHttpExecConfig cfg_aud;
    owl_http_exec_config_defaut(&cfg_aud);
    cfg_aud.file_profondeur    = 8;
    cfg_aud.plafond_attente_ms = 5000;
    cfg_aud.source_owl         = SRC_SERVICE_AUTH_JWT_AUDIENCE;
    cfg_aud.service_mode       = 1;

    OwlHttpExecuteur* xa = owl_http_executeur_cree(&cfg_aud);
    if (!xa) { CHECK(0, "raison_aud_cree"); return; }
    CHECK(owl_http_executeur_demarre(xa), "raison_aud_thread_executeur_demarre");
    {
        jwt_payload_aud_nbf(payload, sizeof(payload), "owl-test-emetteur", 1, 3600,
                            "user-42", "\"autre-service\"", 0, 0);
        CHECK(jwt_signe_rs256(JWT_HDR_RS256, payload, jwt, sizeof(jwt)), "raison_aud_signe");
        snprintf(req, sizeof(req),
                 "GET /protegee HTTP/1.1\r\nHost: a\r\nAuthorization: Bearer %s\r\n\r\n", jwt);
        EchangeE2E e = joue_e2e_capture_stderr(req, xa, log, sizeof(log));
        CHECK(e.statut == 401, "raison_aud_statut_401");
        CHECK(strstr(log, "audience") != NULL, "raison_aud_log_serveur_contient_raison");
        CHECK(strstr(e.reponse, "audience") == NULL, "raison_aud_corps_client_opaque");
    }
    owl_http_executeur_arrete(xa);
    owl_http_executeur_libere(xa);
}

int http_dsl_service_self_test_run(void) {
    printf("== DSL service — fondation runtime (feat/1856) ==\n");

    famille_0_cablage_deterministe();
    famille_1_bout_en_bout();
    famille_2_binding_params();
    famille_3_crash_handler();
    famille_4_renvoyer_corps();
    famille_5_reason_phrase();
    famille_6_contexte_requete_reponse();
    famille_7_authentification();
    famille_8_entetes_reponse();
    famille_9_hooks_cycle_de_vie();
    famille_10_authentification_basique();
    famille_11_authentification_cle_api();
    famille_12_autorisation();
    famille_13_authentification_jwt();
    famille_14_noms_motcles();
    famille_15_multipart_upload();
    famille_16_openapi_route();
    famille_17_wildcard_catchall();
    famille_18_securite_methode_constante();
    famille_19_codeetat_pilote_defaut();
    famille_20_surerreur_exceptioninfo();
    famille_21_entetes_scalaires_coerces();
    famille_22_corps_binaire();
    famille_23_renvoyer_buffer();
    famille_24_catalogue_events_partage();
    famille_25_structure_dans_bloc();
    famille_26_entetes_requete();
    famille_27_jwt_claims_aud_nbf();
    famille_28_jwks_rotation_cles();
    famille_29_raison_echec_auth_log_serveur();

    int tp = 0, tf = 0;
    for (int i = 0; i < OWL_DSL_SERVICE_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f0 cablage deterministe    : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f1 bout en bout socket     : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f2 binding params (1837)   : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f3 crash handler (1838)    : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f4 renvoyer corps (1902)   : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 reason-phrase (1903)    : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 contexte requete/reponse (1907) : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 authentification (1908) : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   f8 entetes reponse (1909)  : %d/%d\n", g_pass[8], g_pass[8] + g_fail[8]);
    printf("   f9 hooks cycle de vie (1910) : %d/%d\n", g_pass[9], g_pass[9] + g_fail[9]);
    printf("   f10 authentification basique (1943) : %d/%d\n", g_pass[10], g_pass[10] + g_fail[10]);
    printf("   f11 authentification cle API (1945) : %d/%d\n", g_pass[11], g_pass[11] + g_fail[11]);
    printf("   f12 autorisation (1946)    : %d/%d\n", g_pass[12], g_pass[12] + g_fail[12]);
    printf("   f13 authentification JWT (1949) : %d/%d\n", g_pass[13], g_pass[13] + g_fail[13]);
    printf("   f14 noms motcles op/service (1940) : %d/%d\n", g_pass[14], g_pass[14] + g_fail[14]);
    printf("   f15 multipart upload (1839) : %d/%d\n", g_pass[15], g_pass[15] + g_fail[15]);
    printf("   f16 route openapi.json (1840) : %d/%d\n", g_pass[16], g_pass[16] + g_fail[16]);
    printf("   f17 wildcard catch-all (1854) : %d/%d\n", g_pass[17], g_pass[17] + g_fail[17]);
    printf("   f18 securite methode constante (1974) : %d/%d\n", g_pass[18], g_pass[18] + g_fail[18]);
    printf("   f19 codeetat pilote defaut (1975) : %d/%d\n", g_pass[19], g_pass[19] + g_fail[19]);
    printf("   f20 surerreur exceptioninfo (1976) : %d/%d\n", g_pass[20], g_pass[20] + g_fail[20]);
    printf("   f21 entetes scalaires coerces (1979) : %d/%d\n", g_pass[21], g_pass[21] + g_fail[21]);
    printf("   f22 corps binaire Corps= (1978) : %d/%d\n", g_pass[22], g_pass[22] + g_fail[22]);
    printf("   f23 corps binaire RENVOYER (1978) : %d/%d\n", g_pass[23], g_pass[23] + g_fail[23]);
    printf("   f24 catalogue events partage (2012) : %d/%d\n", g_pass[24], g_pass[24] + g_fail[24]);
    printf("   f25 structure dans bloc (2013) : %d/%d\n", g_pass[25], g_pass[25] + g_fail[25]);
    printf("   f26 entetes requete entrants (2032) : %d/%d\n", g_pass[26], g_pass[26] + g_fail[26]);
    printf("   f27 claims JWT aud/nbf (2038) : %d/%d\n", g_pass[27], g_pass[27] + g_fail[27]);
    printf("   f28 jwks rotation cles (2044) : %d/%d\n", g_pass[28], g_pass[28] + g_fail[28]);
    printf("   f29 raison echec auth log serveur (2056) : %d/%d\n", g_pass[29], g_pass[29] + g_fail[29]);
    printf("   TOTAL DSL service          : %d/%d\n", tp, tp + tf);
    return tf;
}
