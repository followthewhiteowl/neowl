// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_http_exec_h
#define owl_http_exec_h

/* ================================================================
 * EXECUTEUR HTTP — phase A0.2
 *
 * Spec : contrat de transport HTTP interne (regle d'or, marshalling,
 *        politique de saturation).
 *
 * Ce qui vit ici : la file bornee, le thread proprietaire unique de la
 * VM, le marshalling requete/reponse, la politique de saturation, et le
 * TYPE OPAQUE de reponse.
 *
 * REGLE D'OR (§2) : tout ce qui traverse la frontiere est constitue
 * d'octets POSSEDES ou COPIES. Jamais une reference vers un objet gere
 * par le GC. C'est elle, et elle seule, qui permet de garder la VM
 * isolee sans un seul mutex dans gc.c ni dans l'interning.
 *
 * Deux moities dans ce module, volontairement etanches :
 *
 *   1. le type opaque OwlHttpRep — C pur, aucune connaissance de la VM.
 *      C'est la seule partie que le transport (http_server.c) touche ;
 *   2. l'executeur — possede la VM, et lui seul.
 *
 * Ce qui n'y vit PAS : aucune native OWL, aucun type OWL, aucun symbole
 * enregistre au registry ni au catalogue LSP. La surface de langage se
 * fige en phase B avec le DSL.
 * ================================================================ */

#include "common.h"
#include "http_server.h"

#include <stddef.h>

/* ================================================================
 * 1. Reponse — type OPAQUE (contrat §5)
 *
 * Exigence explicite du §5 : « rien au-dessus de l'executeur ne suppose
 * que c'est un tampon contigu ». D'ou l'incompletude du type et le
 * puits d'octets : le mode B (handle de fichier, A0.3) devient additif
 * sans toucher un seul appelant.
 * ================================================================ */

/* OwlHttpRep est declare par http_server.h : le transport doit pouvoir
 * le nommer sans connaitre l'executeur. Il est DEFINI ici. */

/* Puits d'octets. Retour 0 = ecrit, non-nul = abandon (socket morte). */
typedef struct {
    int   (*ecrit)(void* user, const char* octets, size_t len);
    void*   user;
} OwlHttpPuits;

/* Mode A (tampon complet). COPIE les octets : la reponse en devient
 * proprietaire. La copie est le prix explicite de l'isolation (§2), et
 * elle est ce qui rend impossible de laisser filer un AS_CSTRING.
 * NULL si l'allocation echoue. */
OwlHttpRep* owl_http_rep_cree_tampon(int statut, const char* type_contenu,
                                     const char* corps, size_t corps_len);

/* Mode B (handle de fichier, §5). Le fichier est ouvert ICI et streame
 * par le transport SANS jamais rentrer dans la VM ni allouer un octet
 * GC. `chemin` doit deja avoir passe la frontiere de securite
 * (owl_http_politique_chemin) : ce constructeur ne fait aucune
 * verification de confinement, c'est le role de la politique et d'elle
 * seule. NULL si le fichier ne s'ouvre pas ou si le statut est hors
 * 100-599. */
OwlHttpRep* owl_http_rep_cree_fichier(int statut, const char* type_contenu,
                                      const char* chemin);
bool        owl_http_rep_est_fichier(const OwlHttpRep* r);

/* ---------- Mode C : source incrementale (flux chunked-out, ) ----------
 *
 * Casse le modele one-shot : le corps n'est ni un tampon ni un fichier,
 * mais une FILE BORNEE alimentee au fil de l'eau par un producteur (thread
 * VM) et drainee par le transport via owl_http_rep_ecrit. La VM POUSSE, le
 * transport DRAINE : jamais l'inverse (l'inversion preserve l'isolation de
 * la regle d'or §2 — seuls des octets COPIES traversent la file).
 *
 * Content-Length devient inconnu -> le transport bascule sur
 * Transfer-Encoding: chunked (RFC 9112 §7.1). Le cadrage est pose par le
 * TRANSPORT et jamais derive du contenu des chunks. */

/* Retour de owl_http_rep_flux_pousse. */
typedef enum {
    OWL_HTTP_FLUX_OK      = 0,
    OWL_HTTP_FLUX_ABANDON = 1   /* consommateur parti : le producteur s'arrete */
} OwlHttpFluxStatut;

/* Retour de owl_http_rep_flux_attend_premier : le transport decide
 * l'en-tete AVANT d'ecrire le moindre octet de corps (D3 — un echec
 * survenu avant le 1er chunk laisse encore possible un 500 propre). */
typedef enum {
    OWL_HTTP_FLUX_AMORCE_DONNEE = 0,  /* au moins un chunk est pret */
    OWL_HTTP_FLUX_AMORCE_VIDE   = 1,  /* termine sans le moindre octet */
    OWL_HTTP_FLUX_AMORCE_AVORTE = 2   /* avorte avant le 1er octet */
} OwlHttpFluxAmorce;

/* Cree une reponse en flux. `borne_octets` = borne CUMULEE de la file en
 * OCTETS (D2, vraie garantie memoire anti-DoS). Statut sans corps
 * (204/304/1xx) -> NULL (D7). NULL aussi si allocation echouee. */
OwlHttpRep* owl_http_rep_cree_flux(int statut, const char* type_contenu,
                                   size_t borne_octets);
bool        owl_http_rep_est_flux(const OwlHttpRep* r);

/*  P5 — borne applicative de DUREE du flux (ms ; 0 = illimite).
 * Estampillee a l'ouverture (route->delaiMax, secondes -> ms) ; le transport la
 * lit pour armer un timer dedie qui coupe le flux a l'echeance. Sans effet hors
 * flux. */
void        owl_http_rep_flux_delai_max_set(OwlHttpRep* r, int ms);
int         owl_http_rep_flux_delai_max_ms(const OwlHttpRep* r);

/* Pousse des octets COPIES dans la file (jamais un ObjString*). BLOQUE si
 * la file est pleine (contre-pression). Rend ABANDON immediat si le
 * consommateur est parti (§8) : le producteur doit alors s'arreter.
 * Regle « file vide -> un chunk surdimensionne passe quand meme » (D2 :
 * pas d'interblocage). */
OwlHttpFluxStatut owl_http_rep_flux_pousse(OwlHttpRep* r, const char* octets,
                                           size_t len);

/* Fin nominale : chunk terminal 0CRLF CRLF emis par le transport. */
void        owl_http_rep_flux_termine(OwlHttpRep* r);
/* Erreur mi-flux : fermeture ABRUPTE, PAS de chunk terminal (le client
 * detecte une reponse chunked tronquee = erreur). */
void        owl_http_rep_flux_avorte(OwlHttpRep* r);
/* Refcount : le flux est partage (producteur + transport), le DERNIER a
 * relacher libere. */
void        owl_http_rep_flux_relache(OwlHttpRep* r);

/* Bloque jusqu'a ce que le sort du 1er chunk soit connu (cf. enum). */
OwlHttpFluxAmorce owl_http_rep_flux_attend_premier(OwlHttpRep* r);

/* ---------- Drain NON BLOQUANT (flux dans la boucle reacteur, W5) ----------
 *
 * Le drain bloquant (flux_draine via owl_http_rep_ecrit) retient son thread
 * sur la condvar `non_vide` entre deux chunks — c'est ce qui epinglait un
 * worker pour toute la duree d'un flux (design §1). La boucle reacteur (W5)
 * ne peut pas bloquer : elle tire ce qui est DISPO maintenant et rend la
 * main. Ces primitives ne verrouillent le fmx que le temps de lire/retirer,
 * jamais d'attente. Aucun objet GC ne traverse : que des octets C-heap. */

/* Retour de owl_http_rep_flux_tire : etat courant SANS attente. */
typedef enum {
    OWL_HTTP_FLUX_TIRE_DONNEE = 0,  /* donnees et len remplis (a free par l'appelant) */
    OWL_HTTP_FLUX_TIRE_VIDE   = 1,  /* rien dispo, mais producteur encore actif */
    OWL_HTTP_FLUX_TIRE_FINI   = 2,  /* file vide ET producteur termine (chunk terminal du) */
    OWL_HTTP_FLUX_TIRE_AVORTE = 3,  /* avorte : fermeture abrupte, pas de terminal */
    OWL_HTTP_FLUX_TIRE_MORT   = 4   /* consommateur_fini deja pose (drain abandonne) */
} OwlHttpFluxTire;

/* Retire le chunk de tete s'il y en a un, sans bloquer. Sur DONNEE, rend
 * *donnees (malloc, a free) et *len. Sur les autres etats, ne touche pas les
 * sorties. Reveille le producteur bloque sur la contre-pression (non_plein)
 * quand un chunk sort. */
OwlHttpFluxTire owl_http_rep_flux_tire(OwlHttpRep* r, char** donnees, size_t* len);

/* Pose consommateur_fini (le drain abandonne : socket morte, HEAD, arret) :
 * la prochaine pousse rend ABANDON et le producteur s'arrete, sans
 * interblocage sur une file que plus personne ne draine. Reveille un
 * producteur bloque sur non_plein. Idempotent. */
void        owl_http_rep_flux_marque_consommateur_fini(OwlHttpRep* r);

/* Hook de reveil du drain (W5) : `cb(user)` est appele sur le THREAD
 * PRODUCTEUR apres chaque pousse / termine / avorte. Il doit etre
 * thread-safe et non bloquant (typiquement : signaler un self-pipe W4). Sert
 * a la boucle reacteur pour se reveiller quand un chunk devient dispo. `cb`
 * NULL = aucun reveil (chemin bloquant historique). A poser AVANT que le
 * producteur ne commence a pousser. */
typedef void (*OwlHttpFluxNotif)(void* user);
void        owl_http_rep_flux_definit_notif(OwlHttpRep* r, OwlHttpFluxNotif cb,
                                            void* user);

void        owl_http_rep_libere(OwlHttpRep* r);

/* ---------- En-tetes supplementaires : point de passage UNIQUE ----------
 *
 * Contrat §6 : « interdire CR/LF dans les valeurs fournies par
 * l'utilisateur, en un seul point de passage ». C'est celui-ci. Un nom
 * ou une valeur invalide est REFUSE (retour non nul), jamais assaini en
 * silence — un assainissement muet est une injection qu'on n'a pas vue.
 * 0 = ajoute, -1 = refuse (invalide ou table pleine). */
#define OWL_HTTP_REP_ENTETES_MAX 16

int         owl_http_rep_ajoute_entete(OwlHttpRep* r, const char* nom,
                                       const char* valeur);
size_t      owl_http_rep_nb_entetes(const OwlHttpRep* r);
const char* owl_http_rep_entete_nom(const OwlHttpRep* r, size_t i);
const char* owl_http_rep_entete_valeur(const OwlHttpRep* r, size_t i);

int         owl_http_rep_statut(const OwlHttpRep* r);
const char* owl_http_rep_type_contenu(const OwlHttpRep* r);

/* Toujours connue : sans streaming, Content-Length est toujours
 * calculable et il n'y a pas de chunked en sortie (§5). */
size_t      owl_http_rep_longueur(const OwlHttpRep* r);

/* Emet le corps vers le puits. 0 = OK, non-nul = le puits a abandonne. */
int         owl_http_rep_ecrit(const OwlHttpRep* r, const OwlHttpPuits* puits);

/* ================================================================
 * 2. Executeur
 * ================================================================ */

typedef enum {
    OWL_HTTP_EXEC_OK     = 0,
    OWL_HTTP_EXEC_SATURE = 1   /* file pleine, ou plafond d'attente depasse */
} OwlHttpExecStatut;

/* ---------- Seam producteur (mode C, ) ----------
 *
 * Le pont OWL viendra en phase B ; ce seam est le point de test C. Le
 * producteur est appele DANS le thread proprietaire de la VM : il ouvre
 * le flux (owl_http_prod_ouvre_flux) puis pousse des octets copies. */
typedef struct OwlHttpProdCtx OwlHttpProdCtx;

/* Ouvre le flux sortant du job courant. Rend le flux (propriete partagee
 * avec le transport, a relacher via owl_http_rep_flux_relache) ou NULL en
 * cas de refus (statut sans corps, allocation). */
OwlHttpRep* owl_http_prod_ouvre_flux(OwlHttpProdCtx* ctx, int statut,
                                     const char* type_contenu,
                                     size_t borne_octets);

/* ---------- Seam « flux courant » (surface OWL FluxÉcrit, , phase B) --
 *
 * La native FluxÉcrit lit le ProdCtx courant depuis le slot ambiant de la VM
 * (owl_vm_flux_courant_get) et emet via ces seams. Le framing (SSE / octets)
 * reste dans la native ; ici on ne fait que porter le mode et ouvrir le flux
 * de maniere paresseuse. */
#define OWL_HTTP_FLUX_MODE_SSE    0   /* framing SSE : data: <payload>\n\n */
#define OWL_HTTP_FLUX_MODE_OCTETS 1   /* octets bruts (chunked) */

/* Mode de framing porte par le ProdCtx (pose par la voie service en P3, ou par
 * le harnais de test en P2). */
void owl_http_prod_flux_mode_set(OwlHttpProdCtx* ctx, int mode);
int  owl_http_prod_flux_mode(const OwlHttpProdCtx* ctx);

/*  P5 — borne applicative de duree (ms, 0 = illimite) portee par le
 * ProdCtx, posee AVANT l'appel du handler (voie service : route->delaiMax en s,
 * converti en ms). Estampillee sur le flux a son ouverture. */
void owl_http_prod_flux_delai_max_set(OwlHttpProdCtx* ctx, int ms);

/* Ouverture PARESSEUSE du flux courant : au 1er appel, lit le statut / type
 * depuis MoiMême..Réponse..CodeÉtat / ..TypeContenu si un contexte d'echange
 * service est present (defauts : text/event-stream en SSE, application/octet-
 * stream en octets, statut 200), ouvre le flux et le met en cache dans le
 * ProdCtx. Les appels suivants renvoient le MEME flux. NULL si ctx invalide ou
 * statut sans corps. */
OwlHttpRep* owl_http_prod_flux_assure(OwlHttpProdCtx* ctx);

/* Flux deja ouvert par owl_http_prod_flux_assure (cache du ProdCtx), ou NULL.
 * Le proprietaire (producteur / voie service) le termine + relache. */
OwlHttpRep* owl_http_prod_flux_courant(const OwlHttpProdCtx* ctx);

/* Callback producteur, execute dans le thread VM de l'executeur. */
typedef void (*OwlHttpProducteur)(const OwlHttpRequete* req,
                                  OwlHttpProdCtx* ctx, void* user);

typedef struct {
    /* Borne de la file. 0 -> defaut. */
    size_t file_profondeur;

    /* Plafond d'attente (§7) : echeance ABSOLUE posee AU DEPOT. Une
     * requete qui a trop attendu est retiree de la file sans etre
     * executee — le client a probablement deja renonce. */
    int    plafond_attente_ms;

    /* Source OWL du harnais, compilee une fois au demarrage du thread.
     * Provisoire : remplacee par le DSL de la phase B. */
    const char* source_owl;

    /* Voie PROJET : quand `projet_nb > 0`, le thread executeur ne
     * compile pas `source_owl` mais TOUTE la frontiere projet (compagnons
     * defs+corps avant l'entree, meme moteur que `owl <fichier>`), pour qu'un
     * ServiceREST resolve les symboles des fichiers voisins (ETAT_X,
     * procedures). Tableaux paralleles copies en profondeur par cree(). */
    const char* const* projet_noms;
    const char* const* projet_chemins;
    const char* const* projet_sources;
    int                projet_nb;
    int                projet_entree;   /* index du fichier d'entree */
    int                projet_mode;     /* isProjectMode (bool) */

    /* Procedure OWL appelee pour chaque requete (0 argument). */
    const char* procedure;

    /* Instrument de verrouillage de la regle d'or (§2), pas un reglage
     * d'exploitation : force un cycle GC complet AUX DEUX frontieres —
     * apres la copie entrante et avant de rendre la reponse. Toute
     * reference qui aurait fuite serait alors pendante, et l'assertion
     * sur l'octet exact du corps rendu la revelerait. Cout prohibitif
     * en production, d'ou le defaut a 0. */
    int gc_frontiere;

    /* Mode C : quand `producteur` est pose, l'executeur
     * l'appelle au lieu de la procedure OWL du harnais et
     * owl_http_executeur_attend rend la main au 1er chunk (flux_demarre),
     * pas a la completion du job. Le `plafond_attente_ms` est neutralise
     * des qu'un tel job a DEMARRE (D1/§2.d). NULL -> mode one-shot. */
    OwlHttpProducteur producteur;
    void*             producteur_user;

    /* Mode SERVICE : fondation runtime du DSL de service. Quand
     * pose (et sans producteur), `procedure` est ignore : chaque requete est
     * ROUTEE via le registre runtime (owl_service_runtime_find, peuple a la
     * compilation de `source_owl`), et le handler surRequete resolu est invoque
     * avec une reponse implicite (MoiMême). Route non resolue -> 404. */
    int service_mode;
} OwlHttpExecConfig;

#ifndef OWL_HTTP_EXECUTEUR_DEFINI
#define OWL_HTTP_EXECUTEUR_DEFINI
typedef struct OwlHttpExecuteur OwlHttpExecuteur;
#endif
typedef struct OwlHttpJeton     OwlHttpJeton;

/* ---------------------------------------------------------------
 * Convention de marshalling — PROVISOIRE, JETABLE.
 *
 * vm_call_global() est 0-argument : l'entree et la sortie du handler
 * passent donc par des globales OWL. Ces noms sont une convention de
 * HARNAIS, pas une API :
 *   - jamais enregistres au registry, jamais dans le catalogue LSP,
 *     jamais dans une desc, jamais dans --vscode-data ;
 *   - declares uniquement dans la source OWL passee en config ;
 *   - prefixe « harnais » pour que personne ne puisse les prendre pour
 *     une surface de langage.
 *
 * Ils sont REMPLACES par le DSL de la phase B, pas etendus. L'alternative
 * (ObjInstance + vm_call_reentrant) a ete ecartee : elle figerait une
 * forme de type alors que c'est precisement ce que la phase B doit
 * decider, et le dev a annonce la refonte de httpRequete/httpReponse.
 * --------------------------------------------------------------- */
#define OWL_HARNAIS_HTTP_METHODE "harnaisHttpEntreeMethode"
#define OWL_HARNAIS_HTTP_CHEMIN  "harnaisHttpEntreeChemin"
#define OWL_HARNAIS_HTTP_CORPS   "harnaisHttpEntreeCorps"
#define OWL_HARNAIS_HTTP_STATUT  "harnaisHttpSortieStatut"
#define OWL_HARNAIS_HTTP_TYPE    "harnaisHttpSortieType"
#define OWL_HARNAIS_HTTP_SORTIE  "harnaisHttpSortieCorps"

void owl_http_exec_config_defaut(OwlHttpExecConfig* cfg);

/* Ni thread ni VM : cree() n'alloue que la file. */
OwlHttpExecuteur* owl_http_executeur_cree(const OwlHttpExecConfig* cfg);
void              owl_http_executeur_libere(OwlHttpExecuteur* x);

/* Chemin DETERMINISTE (tests) : le thread appelant possede deja une VM
 * initialisee, prepare_vm() y compile la source du harnais, et
 * traite_une() execute UN job dans ce meme thread. Aucun thread, aucun
 * ordonnancement a deviner.
 *
 * traite_une() : 1 si un job a ete traite, 0 si la file est restee vide
 * pendant attente_ms, -1 si le job a ete abandonne au plafond. */
bool owl_http_executeur_prepare_vm(OwlHttpExecuteur* x);
int  owl_http_executeur_traite_une(OwlHttpExecuteur* x, int attente_ms);

/* Chemin THREAD : vm_init + prepare_vm + boucle + vm_free, dans le
 * thread cree ici et lui seul. UN SEUL THREAD POSSEDE LA VM. */
bool owl_http_executeur_demarre(OwlHttpExecuteur* x);
void owl_http_executeur_arrete(OwlHttpExecuteur* x);

/* Depot sans attente. File pleine -> SATURE IMMEDIAT, avant tout signal
 * de condvar : le 503 est produit par le transport seul (§7), et
 * jobs_pris() le prouve puisqu'il n'est incremente que par traite_une. */
OwlHttpExecStatut owl_http_executeur_depose(OwlHttpExecuteur* x,
                                            const OwlHttpRequete* req,
                                            OwlHttpJeton** out_jeton);

/* Attend la reponse d'un jeton depose et en rend la PROPRIETE (a liberer
 * via owl_http_rep_libere). SATURE si le plafond d'attente a expire. */
OwlHttpExecStatut owl_http_executeur_attend(OwlHttpExecuteur* x,
                                            OwlHttpJeton* jeton,
                                            OwlHttpRep** out_rep);

/* depose + attend. C'est ce que branche le transport. */
OwlHttpExecStatut owl_http_executeur_soumet(OwlHttpExecuteur* x,
                                            const OwlHttpRequete* req,
                                            OwlHttpRep** out_rep);

/* Jeton abandonne par le transport (§8, client deconnecte) : l'executeur
 * termine le handler quand meme et la reponse est jetee. */
void owl_http_executeur_abandonne(OwlHttpExecuteur* x, OwlHttpJeton* jeton);

/* ---------- Resolution NON BLOQUANTE d'un jeton (W7, exec async) ----------
 *
 * owl_http_executeur_attend BLOQUE le thread appelant jusqu'a flux_demarre /
 * fait / expiry — c'est ce blocage qui retenait un worker sur l'executeur
 * mono-thread (design §1, residu apres W5). Le reacteur ne peut pas bloquer :
 * il consulte l'etat du jeton SANS attendre, reveille par le hook de notif
 * (self-pipe W4) que l'executeur declenche a chaque resolution, et par un
 * timer d'echeance cote reacteur pour l'expiry (le job non demarre dont
 * personne ne signale l'echeance). Aucun objet GC ne traverse : la reponse
 * rendue est du C-heap possede (regle d'or §2). */

/* Hook de reveil du jeton : `cb(user)` est appele par l'executeur (SOUS son
 * verrou, comme le notif de flux) apres chaque resolution — flux_demarre,
 * fait, ou expiry au depilage. Il doit etre thread-safe et non bloquant
 * (typiquement : signaler un self-pipe W4). A poser via
 * owl_http_executeur_jeton_definit_notif, qui declenche le cb SUR-LE-CHAMP si
 * le jeton est deja resolu (course depot/resolution refermee). */
typedef void (*OwlHttpJetonNotif)(void* user);
void owl_http_executeur_jeton_definit_notif(OwlHttpExecuteur* x,
                                            OwlHttpJeton* jeton,
                                            OwlHttpJetonNotif cb, void* user);

/* Echeance ABSOLUE (owl_tcp_server_horloge_ms) posee au depot, ou 0 si le
 * jeton est sans echeance (plafond d'attente <= 0). Le reacteur async arme
 * son timer de synthese du 503 dessus. */
uint64_t owl_http_executeur_jeton_echeance(const OwlHttpJeton* jeton);

typedef enum {
    OWL_HTTP_ESSAIE_ATTEND    = 0,  /* ni flux ni fait : rappeler plus tard */
    OWL_HTTP_ESSAIE_FLUX      = 1,  /* flux_demarre : *out_rep = flux (detache) */
    OWL_HTTP_ESSAIE_BUFFERISE = 2,  /* fait : *out_rep = reponse complete (detachee) */
    OWL_HTTP_ESSAIE_SATURE    = 3   /* expiry / echec : le reacteur produit un 503 */
} OwlHttpEssaie;

/* Consulte l'etat d'un jeton depose SANS bloquer (miroir non bloquant
 * d'owl_http_executeur_attend). ATTEND tant que le job n'a ni demarre en flux
 * ni fini ; a l'echeance d'un job non demarre, rend SATURE (le jeton est
 * marque abandonne, l'executeur le liberera au depilage). Sur FLUX/BUFFERISE,
 * rend la PROPRIETE de *out_rep et detache le jeton du reacteur (le thread VM
 * en reprend la charge et le libere). */
OwlHttpEssaie owl_http_executeur_essaie(OwlHttpExecuteur* x, OwlHttpJeton* jeton,
                                        OwlHttpRep** out_rep);

size_t owl_http_executeur_jobs_pris(const OwlHttpExecuteur* x);
size_t owl_http_executeur_rejets(const OwlHttpExecuteur* x);
size_t owl_http_executeur_profondeur(const OwlHttpExecuteur* x);

/* Nombre d'identifiants de thread DISTINCTS ayant execute un job, donc
 * touche la VM. La frontiere du contrat §1 se lit ici : quel que soit le
 * nombre de threads reseau, ce compteur doit valoir 1. Instrument de
 * preuve, pas metrique d'exploitation — d'ou le plafond bas de la table
 * interne. */
size_t owl_http_executeur_nb_threads_vm(const OwlHttpExecuteur* x);

/* Handler pret a brancher sur OwlHttpServeurConfig.handler_rep ;
 * `user` = l'OwlHttpExecuteur*. Rend la PROPRIETE de la reponse au
 * transport, qui la libere apres ecriture.
 *
 * C'est la forme exigee par le pool (A1.1b) : la variante OwlHttpHandler
 * devait garder la reponse vivante dans l'executeur jusqu'a l'appel
 * suivant, donc dans un emplacement partage par toutes les connexions.
 * Avec N workers, cet emplacement est une course ET une fuite d'etat
 * entre connexions concurrentes. Rendre la propriete supprime le
 * probleme au lieu de le verrouiller. */
OwlHttpRep* owl_http_exec_handler_rep(const OwlHttpRequete* req, void* user);

#endif /* owl_http_exec_h */
