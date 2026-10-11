// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* owl_serve.c — montage `owl serve` : compile -> executeur owning-VM ->
 * listener reel binde -> boucle accept.
 *
 * Voir owl_serve.h pour le contrat. Trois modules reunis, aucun nouveau :
 *   - compilation : vm_interpret peuple le registre de routes runtime
 *     (owl_service_runtime) ET le registre compile-time (service_decl, d'ou le
 *     nom du service pour le message de demarrage) ;
 *   - executeur : http_exec.c possede la VM sur SON thread (contrainte VM
 *     mono-thread, ) et route chaque requete via le registre runtime ;
 *   - transport : http_server.c sert une connexion via handler_rep branche sur
 *     l'executeur.
 *
 * Deux compilations : une SYNCHRONE dans prepare (thread appelant) pour valider
 * la presence d'un service et lire son nom AVANT de binder ; une seconde dans
 * le thread de l'executeur (owl_http_executeur_demarre) qui possede la VM de
 * service. Entre les deux, compiler_reset_poo_state() remet le registre
 * compile-time a zero — sans quoi la seconde compilation verrait le service
 * "deja declare".
 */

#include "owl_serve.h"

#include "http_exec.h"
#include "http_server.h"
#include "http_service_runtime.h"
#include "net_tcp_server.h"
#include "service_decl.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* compiler.h tire scanner.h (enum TokenType) qui entre en collision avec
 * winnt.h via la couche reseau : on ne declare que le reset dont on a besoin. */
extern void compiler_reset_poo_state(void);
/*  — compile la frontiere projet sans teardown (project_run.c). Declare
 * ici pour ne pas tirer project.h (garde la TU compiler-free cote reseau). */
extern InterpretResult project_run_sources_no_teardown(
        const char* const* names, const char* const* paths,
        const char* const* sources, int count,
        int entryIndex, bool isProjectMode);

struct OwlServeMontage {
    OwlTcpListener    listener;
    OwlHttpExecuteur* executeur;
    char              adresse[64];
    int               port;
    char              service_nom[512];  /* liste des noms servis (union multi-service) */
    int               service_count;     /* nombre de services servis (pluriel du message) */

    int max_connexions;

    /* Config de transport : DOIT survivre au pool (le pool la reference sans
     * la copier, cf. owl_http_serveur_pool_demarre). Montee une fois dans
     * demarre(), lue par les workers pendant toute la vie du pool. */
    OwlHttpServeurConfig cfg;
    OwlTcpPool*          pool;   /* pool de transport (N workers, VM unique) */

#ifdef _WIN32
    HANDLE thread;   /* thread superviseur (arret propre du pool) */
#else
    pthread_t thread;
#endif
    int thread_lance;
};

static void err_pose(char* err_out, size_t cap, const char* msg) {
    if (!err_out || cap == 0) return;
    size_t n = strlen(msg);
    if (n >= cap) n = cap - 1;
    memcpy(err_out, msg, n);
    err_out[n] = '\0';
}

/* Compte les services declares par les COMPAGNONS seuls (frontiere privee de
 * son entree). L'entree etant compilee en dernier, cette valeur = l'index de
 * debut de ses services dans le registre de la compilation complete. VM et
 * registre ephemeres : reset avant/apres pour ne pas polluer la vraie passe.
 * 0 si le fichier est auto-suffisant (aucun compagnon). */
static int compagnons_service_count(const OwlServeProjet* projet) {
    int n = projet->nb - 1;
    if (n <= 0) return 0;

    const char** noms    = (const char**)malloc((size_t)n * sizeof(char*));
    const char** chemins = (const char**)malloc((size_t)n * sizeof(char*));
    const char** sources = (const char**)malloc((size_t)n * sizeof(char*));
    if (!noms || !chemins || !sources) {
        free(noms); free(chemins); free(sources);
        return 0;
    }
    int w = 0;
    for (int i = 0; i < projet->nb; i++) {
        if (i == projet->entree) continue;
        noms[w]    = projet->noms[i];
        chemins[w] = projet->chemins[i];
        sources[w] = projet->sources[i];
        w++;
    }

    compiler_reset_poo_state();
    vm_init();
    project_run_sources_no_teardown(noms, chemins, sources, w, 0,
                                    projet->mode_projet != 0);
    int c = owl_service_registry_count();
    vm_free();
    compiler_reset_poo_state();  /* laisse le registre propre pour la vraie passe */

    free(noms); free(chemins); free(sources);
    return c;
}

int owl_serve_compile_projet(const OwlServeProjet* projet, int* entry_service_start,
                             char* err_out, size_t err_out_cap) {
    if (err_out && err_out_cap > 0) err_out[0] = '\0';
    if (!projet || projet->nb <= 0 || !projet->sources ||
        projet->entree < 0 || projet->entree >= projet->nb) {
        err_pose(err_out, err_out_cap, "owl serve : projet absent ou invalide");
        return -1;
    }

    /* Passe optionnelle (openapi entry-specific) : mesure le suffixe de l'entree
     * AVANT la vraie compilation, qui laisse le registre complet peuple. */
    if (entry_service_start) {
        *entry_service_start = compagnons_service_count(projet);
    }

    /* --- Compilation de la frontiere (thread appelant) ---
     * Compile TOUS les fichiers de la frontiere (compagnons AVANT l'entree) et
     * peuple owl_service_registry + le registre de constantes. En SUCCES, la VM
     * reste VIVANTE au retour : le generateur OpenAPI lit owl_registry_const_find
     * (g_consts, libere par vm_free) pour resoudre httpOK -> 200 etc. Le caller
     * appelle vm_free() APRES avoir tout lu, puis compiler_reset_poo_state(). En
     * ECHEC, la VM est liberee ici (le caller retourne sans nettoyage). */
    compiler_reset_poo_state();
    vm_init();
    bool compiled = (project_run_sources_no_teardown(
                         projet->noms, projet->chemins, projet->sources,
                         projet->nb, projet->entree, projet->mode_projet != 0)
                     == INTERPRET_OK);
    int svc_count = owl_service_registry_count();

    if (!compiled) {
        vm_free();
        err_pose(err_out, err_out_cap,
                 "owl serve : la compilation du fichier a echoue");
        return -1;
    }
    return svc_count;
}

/* Retrouve le service+operation d'une route runtime par son handlerGlobal
 * (la route runtime ne porte pas le nom du service). "Service.Operation" ou,
 * a defaut, le handlerGlobal brut. Le registre compile-time doit etre encore
 * vivant (avant compiler_reset_poo_state). */
static void route_qualifie(const char* handlerGlobal, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!handlerGlobal) return;
    int sc = owl_service_registry_count();
    for (int i = 0; i < sc; i++) {
        OwlServiceDecl* d = owl_service_registry_at(i);
        if (!d) continue;
        for (int k = 0; k < d->operationCount; k++) {
            OwlOperationDecl* op = d->operations[k];
            if (op && op->handlerGlobal &&
                strcmp(op->handlerGlobal, handlerGlobal) == 0) {
                snprintf(out, cap, "%s.%s", d->name ? d->name : "?",
                         op->name ? op->name : "?");
                return;
            }
        }
    }
    snprintf(out, cap, "%s", handlerGlobal);
}

/* Detecte la premiere collision de route EXACTE (meme verbe + meme chemin
 * LITTERAL) a travers TOUS les services de la frontiere. Les routes portant un
 * segment parametre/catch-all sont ignorees : le recouvrement wildcard est hors
 * scope ( parque). Retourne 1 + remplit msg si collision, 0 sinon.
 * Registres runtime ET compile-time supposes vivants a l'appel. */
static int detecte_collision_route(char* msg, size_t cap) {
    int n = owl_service_runtime_count();
    for (int i = 0; i < n; i++) {
        const OwlServiceRuntimeRoute* a = owl_service_runtime_at(i);
        if (!a || a->hasParam) continue;
        for (int j = i + 1; j < n; j++) {
            const OwlServiceRuntimeRoute* b = owl_service_runtime_at(j);
            if (!b || b->hasParam) continue;
            if (strcmp(a->verb, b->verb) != 0) continue;
            if (strcmp(a->path, b->path) != 0) continue;
            char qa[192], qb[192];
            route_qualifie(a->handlerGlobal, qa, sizeof(qa));
            route_qualifie(b->handlerGlobal, qb, sizeof(qb));
            snprintf(msg, cap,
                     "owl serve : collision de route %s %s : declaree par %s ET "
                     "%s (deux services ne peuvent exposer le meme chemin)",
                     a->verb, a->path, qa, qb);
            return 1;
        }
    }
    return 0;
}

/* Construit la liste des noms de service servis (union multi-service), separes
 * par ", ". Registre compile-time suppose vivant. Retourne le nombre ecrit. */
static int liste_services(char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    int sc = owl_service_registry_count();
    size_t used = 0;
    int written = 0;
    for (int i = 0; i < sc; i++) {
        OwlServiceDecl* d = owl_service_registry_at(i);
        const char* nm = (d && d->name) ? d->name : NULL;
        if (!nm) continue;
        const char* sep = (written > 0) ? ", " : "";
        int r = snprintf(out + used, cap - used, "%s%s", sep, nm);
        if (r < 0) break;
        if ((size_t)r >= cap - used) { used = cap - 1; break; }
        used += (size_t)r;
        written++;
    }
    return written;
}

OwlServeMontage* owl_serve_prepare_projet(const OwlServeProjet* projet,
                                          const OwlServeOptions* opts,
                                          char* err_out, size_t err_out_cap) {
    if (err_out && err_out_cap > 0) err_out[0] = '\0';
    if (!projet || projet->nb <= 0 || !projet->sources ||
        projet->entree < 0 || projet->entree >= projet->nb) {
        err_pose(err_out, err_out_cap, "owl serve : projet absent ou invalide");
        return NULL;
    }

    const char* adr = (opts && opts->bind_adr) ? opts->bind_adr : "127.0.0.1";
    int port = opts ? opts->port : 0;

    /* --- 1. Compilation de validation (compile la frontiere, capture le nom du
     * service AVANT de binder). Partage avec `owl openapi` via
     * owl_serve_compile_projet ; entry_service_start ignore (serve sert le 1er
     * service de la frontiere, comportement inchange). --- */
    int svc_count = owl_serve_compile_projet(projet, NULL, err_out, err_out_cap);
    if (svc_count < 0) {
        return NULL;  /* err_out deja rempli, VM deja liberee */
    }
    /* --- Detection de collision de route (fail-loud AVANT le bind) : deux
     * OperationHTTP de services distincts ne peuvent exposer le meme couple
     * (methode, chemin litteral). Registres encore vivants ici. --- */
    if (svc_count >= 1) {
        char coll[256];
        if (detecte_collision_route(coll, sizeof(coll))) {
            err_pose(err_out, err_out_cap, coll);
            vm_free();
            compiler_reset_poo_state();
            return NULL;  /* aucun listener ouvert : echec avant bind */
        }
    }

    /* Liste des services servis (message de demarrage honnete, union). */
    char nom[512];
    nom[0] = '\0';
    int nb_services = 0;
    if (svc_count >= 1) {
        nb_services = liste_services(nom, sizeof(nom));
    }
    vm_free();  /* noms copies ; serve n'a plus besoin des registres ici */

    if (svc_count < 1) {
        err_pose(err_out, err_out_cap,
                 "owl serve : aucun service a servir (aucun ServiceREST declare)");
        return NULL;
    }

    /* Remet le registre compile-time a zero pour la recompilation qui aura lieu
     * dans le thread de l'executeur (sinon "service deja declare"). */
    compiler_reset_poo_state();

    /* --- 2. Bind du listener reel (detecte le port deja pris) ---
     * autoriser_non_loopback est laisse a false : l'appelant (main) valide en
     * amont qu'une adresse non-loopback est explicite ; ici, defaut sur. */
    OwlServeMontage* m = calloc(1, sizeof(*m));
    if (!m) {
        err_pose(err_out, err_out_cap, "owl serve : allocation impossible");
        return NULL;
    }

    bool non_loopback = (strcmp(adr, "127.0.0.1") != 0 && strcmp(adr, "::1") != 0);
    if (!owl_tcp_listener_ouvre(&m->listener, adr, port, 64, non_loopback)) {
        char buf[256];
        const char* raison = owl_tcp_listener_erreur(&m->listener);
        snprintf(buf, sizeof(buf), "owl serve : ecoute impossible sur %s:%d (%s)",
                 adr, port, (raison && raison[0]) ? raison : "port deja pris ?");
        err_pose(err_out, err_out_cap, buf);
        free(m);
        return NULL;
    }

    strncpy(m->adresse, adr, sizeof(m->adresse) - 1);
    m->adresse[sizeof(m->adresse) - 1] = '\0';
    m->port = owl_tcp_listener_port(&m->listener);
    strncpy(m->service_nom, nom, sizeof(m->service_nom) - 1);
    m->service_nom[sizeof(m->service_nom) - 1] = '\0';
    m->service_count = nb_services;

    /* --- 3. Executeur owning-VM (pas encore demarre : demarre() lance le
     * thread qui recompile la source et prend possession de la VM). --- */
    OwlHttpExecConfig xc;
    owl_http_exec_config_defaut(&xc);
    xc.file_profondeur    = 32;
    xc.plafond_attente_ms = 30000;
    /*  — le thread executeur recompile la frontiere projet (copie
     * profonde par cree()). source_owl reste NULL : la voie projet prime. */
    xc.projet_noms        = projet->noms;
    xc.projet_chemins     = projet->chemins;
    xc.projet_sources     = projet->sources;
    xc.projet_nb          = projet->nb;
    xc.projet_entree      = projet->entree;
    xc.projet_mode        = projet->mode_projet;
    xc.service_mode       = 1;

    m->executeur = owl_http_executeur_cree(&xc);
    if (!m->executeur) {
        err_pose(err_out, err_out_cap, "owl serve : creation de l'executeur impossible");
        owl_tcp_listener_ferme(&m->listener);
        free(m);
        return NULL;
    }

    return m;
}

OwlServeMontage* owl_serve_prepare(const char* source_owl,
                                   const OwlServeOptions* opts,
                                   char* err_out, size_t err_out_cap) {
    if (err_out && err_out_cap > 0) err_out[0] = '\0';
    if (!source_owl) {
        err_pose(err_out, err_out_cap, "owl serve : source absente");
        return NULL;
    }
    /* Fichier unique, mode script (aucun projet.owl) : une frontiere a un seul
     * fichier. Nom/chemin synthetiques (jamais le marqueur projet) — ils ne
     * servent qu'aux messages d'erreur et a la resolution de nom qualifie. */
    const char* noms[1]    = { "serve" };
    const char* chemins[1] = { "serve.owl" };
    const char* sources[1] = { source_owl };
    OwlServeProjet projet = {
        .noms = noms, .chemins = chemins, .sources = sources,
        .nb = 1, .entree = 0, .mode_projet = 0,
    };
    return owl_serve_prepare_projet(&projet, opts, err_out, err_out_cap);
}

int owl_serve_montage_port(const OwlServeMontage* m) {
    return m ? m->port : 0;
}

const char* owl_serve_montage_adresse(const OwlServeMontage* m) {
    return m ? m->adresse : NULL;
}

const char* owl_serve_montage_service(const OwlServeMontage* m) {
    return m ? m->service_nom : NULL;
}

/* ---------- Superviseur d'arret (thread dedie) ----------
 *
 * Le service reel est fait par le POOL DE TRANSPORT (N workers reseau
 * derriere le meme executeur mono-VM, cf. owl_serve_demarre). Ce thread ne
 * sert aucune connexion : il veille la condition d'arret et joint le pool.
 * Deux sorties :
 *   - demande d'arret globale (owl_tcp_server_demande_arret, SIGINT/filet) ;
 *   - mode test : `max_connexions` connexions servies puis arret propre.
 *     owl_tcp_pool_connexions_servies compte une CONNEXION close (keep-alive
 *     compris : N requetes sur une conn persistante = 1 servie), exactement
 *     comme l'ancienne boucle mono-connexion — d'ou la parite de contrat
 *     pour les familles F0-F7 qui passent un max fini et attendent l'arret. */
static void boucle_sert(OwlServeMontage* m) {
    while (!owl_tcp_server_arret_demande()) {
        if (m->max_connexions > 0 &&
            owl_tcp_pool_connexions_servies(m->pool) >= (size_t)m->max_connexions) {
            break;
        }
        /* Reveil periodique : latence maximale entre la condition d'arret et
         * la sortie. Ce n'est pas un chemin chaud (aucune connexion servie ici). */
#ifdef _WIN32
        Sleep(20);
#else
        struct timespec ts = { 0, 20L * 1000L * 1000L };
        nanosleep(&ts, NULL);
#endif
    }
    /* Arret propre : demande l'arret, joint les workers + le reacteur, libere
     * le pool. Les workers en cours drainent leur connexion avant de sortir. */
    owl_tcp_pool_arrete(m->pool);
    m->pool = NULL;
}

#ifdef _WIN32
static DWORD WINAPI boucle_thread_fn(LPVOID arg) {
    boucle_sert((OwlServeMontage*)arg);
    return 0;
}
#else
static void* boucle_thread_fn(void* arg) {
    boucle_sert((OwlServeMontage*)arg);
    return NULL;
}
#endif

bool owl_serve_demarre(OwlServeMontage* m, int max_connexions, FILE* out) {
    if (!m || m->thread_lance) return false;
    m->max_connexions = max_connexions;

    /* Le thread de l'executeur recompile la source et prend possession de la
     * VM ; demarre() bloque jusqu'a ce que la VM soit prete (pas de connexion
     * servie avant que le handler soit compile). */
    if (!owl_http_executeur_demarre(m->executeur)) return false;

    /* --- Transport : pool de workers reseau derriere l'executeur mono-VM ---
     * Mode SYNCHRONE (handler_rep, PAS executeur_async) : le worker sert la
     * reponse bufferisee inline -> keep-alive preserve (une conn persistante
     * occupe un worker, les autres restent libres). La VM reste unique et
     * intouchee : le pool ne fait que du transport, tout depose dans la file
     * de jobs existante de l'executeur. Un client lent au transport occupe le
     * reacteur (phase pending, event-driven) sans bloquer un client sain, qui
     * obtient immediatement un autre worker. `cfg` vit dans m (elle doit
     * survivre au pool). */
    owl_http_serveur_config_defaut(&m->cfg);       /* limites bornees A0, keep-alive */
    m->cfg.handler_rep      = owl_http_exec_handler_rep;
    m->cfg.handler_rep_user = m->executeur;

    OwlTcpPoolConfig pcfg;
    owl_tcp_pool_config_defaut(&pcfg);
    pcfg.nb_workers     = 4;
    pcfg.connexions_max = 16;

    m->pool = owl_http_serveur_pool_demarre(&m->listener, &m->cfg, &pcfg);
    if (m->pool == NULL) {
        owl_http_executeur_arrete(m->executeur);
        return false;
    }

    if (out) {
        const char* label = (m->service_count > 1) ? "Services" : "Service";
        fprintf(out, "%s %s en ecoute sur http://%s:%d\n",
                label, m->service_nom, m->adresse, m->port);
        fflush(out);
    }

    /* Thread superviseur : veille la condition d'arret et joint le pool. Le
     * service reel tourne dans les workers du pool, pas ici. */
#ifdef _WIN32
    m->thread = CreateThread(NULL, 0, boucle_thread_fn, m, 0, NULL);
    if (m->thread == NULL) {
        owl_tcp_pool_arrete(m->pool); m->pool = NULL;
        owl_http_executeur_arrete(m->executeur);
        return false;
    }
#else
    if (pthread_create(&m->thread, NULL, boucle_thread_fn, m) != 0) {
        owl_tcp_pool_arrete(m->pool); m->pool = NULL;
        owl_http_executeur_arrete(m->executeur);
        return false;
    }
#endif
    m->thread_lance = 1;
    return true;
}

void owl_serve_attend(OwlServeMontage* m) {
    if (!m || !m->thread_lance) return;
#ifdef _WIN32
    WaitForSingleObject(m->thread, INFINITE);
    CloseHandle(m->thread);
#else
    pthread_join(m->thread, NULL);
#endif
    m->thread_lance = 0;
}

void owl_serve_libere(OwlServeMontage* m) {
    if (!m) return;

    if (m->thread_lance) {
        /* Filet : si le superviseur tourne encore, on demande l'arret et on
         * joint (attend() n'a pas ete appele). Le superviseur joint le pool. */
        owl_tcp_server_demande_arret();
        owl_serve_attend(m);
    }

    /* Defensif : pool encore vivant sans superviseur (demarre partiel). Il doit
     * etre joint AVANT l'executeur (ses workers y routent les requetes). */
    if (m->pool) {
        owl_tcp_pool_arrete(m->pool);
        m->pool = NULL;
    }

    if (m->executeur) {
        owl_http_executeur_arrete(m->executeur);   /* no-op si jamais demarre */
        owl_http_executeur_libere(m->executeur);
    }
    owl_tcp_listener_ferme(&m->listener);
    free(m);
}
