// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* http_exec.c — executeur et file du serveur HTTP (, A0.2).
 *
 * Contrat : src/http_exec.h et le contrat de transport HTTP interne.
 *
 * Deux moities etanches, dans cet ordre :
 *   1. le type opaque OwlHttpRep — C pur, aucune connaissance de la VM ;
 *   2. l'executeur — proprietaire unique de la VM.
 *
 * La frontiere entre les deux est la regle d'or (§2) : seuls des octets
 * POSSEDES la traversent. Le GC d'OWL est a rooting precis
 * (gc.c::mark_roots enumere des racines explicites, aucun balayage de
 * pile C), donc deplacer la VM dans un thread dedie ne demande rien a
 * vm.c / gc.c / object.c — mesure de la phase Plan, confirmee par la
 * famille 0 du harnais.
 */

#include "http_exec.h"
/* Le point de passage unique des en-tetes et la plage de statut valides
 * vivent dans la politique : c'est elle qui doit rester identique quel
 * que soit le transport (contrat §6). */
#include "http_politique.h"

#include "gc.h"
#include "object.h"
#include "table.h"
#include "vm.h"
#include "http_service_runtime.h"  /*  : routage runtime du DSL de service */
#include "http_multipart.h"        /*  : parseur multipart/form-data */
#include "service_decl.h"          /*  : AST du service pour /openapi.json */
#include "http_openapi.h"          /*  : generateur du document OpenAPI 3 */
/* scanner.h n'est PAS inclus ici : son enum TokenType entre en collision avec
 * winnt.h (windows.h, tire par l'abstraction thread). On declare le seul
 * symbole utile (lecture accent-insensible des champs de reponse). */
extern int scanner_strip_accents(const char* src, int srcLen, char* buf, int bufSize);
/*  — compile la frontiere projet sans teardown (project_run.c). Declare
 * ici : project.h tire vm.h/common.h mais on garde la TU minimale, comme pour
 * scanner_strip_accents. InterpretResult vit dans vm.h (deja inclus). */
extern InterpretResult project_run_sources_no_teardown(
        const char* const* names, const char* const* paths,
        const char* const* sources, int count,
        int entryIndex, bool isProjectMode);
/*  — parse JSON en C (header/payload JWT). Declare ici : stdlib_json.h
 * n'est pas inclus (evite d'aspirer d'autres dependances scanner/registry). */
extern ObjJson* json_parse_cstr(const char* src, int len);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>            /*  : time() pour la validation exp du JWT */

/*  — crypto JWT declarative (RS256). mbedtls est deja linke dans
 * owl_core (cf. stdlib_crypto.c) ; aucune modif build. */
#include "mbedtls/pk.h"
#include "mbedtls/md.h"
#include "jwks_cache.h"     /*  : resolution de cle JWKS (fetch + cache) */

#ifdef _WIN32
  #include <windows.h>
  typedef CRITICAL_SECTION   hx_mutex_t;
  typedef CONDITION_VARIABLE hx_cond_t;
  typedef HANDLE             hx_thread_t;
#else
  #include <pthread.h>
  #include <errno.h>
  #include <time.h>
  typedef pthread_mutex_t hx_mutex_t;
  typedef pthread_cond_t  hx_cond_t;
  typedef pthread_t       hx_thread_t;
#endif

/* ================================================================
 * 0. Primitives de synchronisation
 *
 * Hissees au-dessus des deux moities : la moitie « type opaque » les
 * utilise aussi depuis  (le mode C flux porte son propre mutex
 * et ses condvars DANS l'OwlHttpRep — jamais dans gc.c ni l'interning,
 * c'est la regle d'or §2). L'executeur les utilise pour la file de jobs.
 * ms < 0 = attente infinie.
 * ================================================================ */

#ifdef _WIN32
static void hx_mutex_init(hx_mutex_t* m)    { InitializeCriticalSection(m); }
static void hx_mutex_detruit(hx_mutex_t* m) { DeleteCriticalSection(m); }
static void hx_verrouille(hx_mutex_t* m)    { EnterCriticalSection(m); }
static void hx_deverrouille(hx_mutex_t* m)  { LeaveCriticalSection(m); }
static void hx_cond_init(hx_cond_t* c)      { InitializeConditionVariable(c); }
static void hx_cond_detruit(hx_cond_t* c)   { (void)c; }
static void hx_cond_signale(hx_cond_t* c)   { WakeAllConditionVariable(c); }
static int  hx_cond_attend(hx_cond_t* c, hx_mutex_t* m, int ms) {
    return SleepConditionVariableCS(c, m, (DWORD)(ms < 0 ? INFINITE : ms)) ? 0 : -1;
}
#else
static void hx_mutex_init(hx_mutex_t* m)    { pthread_mutex_init(m, NULL); }
static void hx_mutex_detruit(hx_mutex_t* m) { pthread_mutex_destroy(m); }
static void hx_verrouille(hx_mutex_t* m)    { pthread_mutex_lock(m); }
static void hx_deverrouille(hx_mutex_t* m)  { pthread_mutex_unlock(m); }
static void hx_cond_init(hx_cond_t* c)      { pthread_cond_init(c, NULL); }
static void hx_cond_detruit(hx_cond_t* c)   { pthread_cond_destroy(c); }
static void hx_cond_signale(hx_cond_t* c)   { pthread_cond_broadcast(c); }
static int  hx_cond_attend(hx_cond_t* c, hx_mutex_t* m, int ms) {
    if (ms < 0) return pthread_cond_wait(c, m);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(c, m, &ts) == 0 ? 0 : -1;
}
#endif

/* ================================================================
 * 0bis. GIL VM
 *
 * Mutex VM global dedie, NON-RECURSIF, DISTINCT de la file executeur
 * (x->m) et du flux (r->fmx). Contrat  §2 : un seul flot
 * d'execution OWL logique a un instant donne. Acquis au BORD EXTERNE
 * d'un handler (owl_http_executeur_traite_une, autour de execute_job),
 * jamais par opcode, jamais par vm_call_reentrant.
 *
 * A N=1 (aujourd'hui : un unique thread executeur, hx_thread_fn) il
 * n'est JAMAIS contendu -> plomberie strictement inerte, comportement
 * identique. Le pool N-threads et le relachement au point de yield sont
 * des lots ulterieurs.
 *
 * Piege cross-OS neutralise : CRITICAL_SECTION (Windows) est RECURSIF,
 * pthread_mutex par defaut ne l'est PAS. Le GIL doit se comporter a
 * l'identique NON-RECURSIF sur les deux. On l'impose par une assertion
 * de propriete (owner thread) valable partout : une double-prise par le
 * meme thread (bug de logique, ex. un yield qui reprendrait le GIL) est
 * piegee, pas silencieusement avalee comme le ferait CRITICAL_SECTION.
 *
 * L'implementation vit ICI, pas dans vm.c : l'abstraction hx_mutex_t et
 * l'inclusion soignee de windows.h y sont deja (vm.c ne peut pas inclure
 * windows.h -- collision TokenType scanner.h/winnt.h). vm.h n'expose que
 * l'API (owl_vm_gil_acquire/release), consommable par la VM et le GC.
 *
 * Singleton process-lifetime (comme `VM vm`) : initialise une fois a la
 * demande, jamais detruit (des executeurs naissent/meurent en serie).
 * ================================================================ */

#include <assert.h>

static hx_mutex_t g_vm_gil;
static int        g_vm_gil_init = 0;
#ifdef _WIN32
static volatile DWORD g_vm_gil_owner = 0;      /* thread id detenteur, 0 = libre */
#define VM_GIL_TID()        (GetCurrentThreadId())
#define VM_GIL_HELD_BY_ME() (g_vm_gil_owner == GetCurrentThreadId())
#define VM_GIL_SET_OWNER()  (g_vm_gil_owner = GetCurrentThreadId())
#define VM_GIL_CLR_OWNER()  (g_vm_gil_owner = 0)
#else
static volatile pthread_t g_vm_gil_owner;
static volatile int       g_vm_gil_owned = 0;
#define VM_GIL_HELD_BY_ME() (g_vm_gil_owned && pthread_equal(g_vm_gil_owner, pthread_self()))
#define VM_GIL_SET_OWNER()  do { g_vm_gil_owner = pthread_self(); g_vm_gil_owned = 1; } while (0)
#define VM_GIL_CLR_OWNER()  do { g_vm_gil_owned = 0; } while (0)
#endif

static void vm_gil_lazy_init(void) {
    if (g_vm_gil_init) return;
    hx_mutex_init(&g_vm_gil);
    VM_GIL_CLR_OWNER();
    g_vm_gil_init = 1;
}

void owl_vm_gil_acquire(void) {
    vm_gil_lazy_init();
    assert(!VM_GIL_HELD_BY_ME()
           && "GIL VM non-recursif : deja tenu par ce thread");
    hx_verrouille(&g_vm_gil);
    VM_GIL_SET_OWNER();
}

void owl_vm_gil_release(void) {
    assert(VM_GIL_HELD_BY_ME()
           && "GIL VM relache par un thread qui ne le tient pas");
    VM_GIL_CLR_OWNER();
    hx_deverrouille(&g_vm_gil);
}

/* ================================================================
 * 1. Reponse — type opaque
 *
 * Cette section ne connait NI la VM NI le GC : c'est la seule que le
 * transport (http_server.c) touche.
 * ================================================================ */

typedef struct {
    char nom[64];
    char valeur[192];
} RepEntete;

/* Un morceau de la file du mode C : des octets COPIES (regle d'or §2 —
 * jamais un ObjString*), et rien d'autre. */
typedef struct FluxChunk {
    char*             donnees;
    size_t            len;
    struct FluxChunk* suivant;
} FluxChunk;

struct OwlHttpRep {
    int    statut;
    char   type[128];
    char*  corps;      /* possede ; NULL si longueur nulle ou mode fichier */
    size_t corps_len;

    /* Mode B (§5) : le corps n'est pas un tampon, c'est un fichier
     * ouvert. Le transport le streame par le meme puits — il ne suppose
     * donc toujours pas de tampon contigu. */
    FILE*  fichier;

    RepEntete entetes[OWL_HTTP_REP_ENTETES_MAX];
    size_t    nb_entetes;

    /* ---- Mode C : source incrementale (flux chunked-out, ) ----
     *
     * Le corps n'est ni tampon ni fichier : une FILE BORNEE EN OCTETS
     * (D2) alimentee par le producteur (thread VM) et drainee par le
     * transport. Le mutex et les condvars vivent ICI, dans la reponse —
     * PAS dans gc.c ni l'interning : c'est ce qui garde la VM isolee sans
     * un seul verrou cote GC (regle d'or §2). Refcompte : producteur +
     * transport, le dernier a relacher libere. */
    int        est_flux;
    hx_mutex_t fmx;
    hx_cond_t  non_vide;          /* pousse -> drain */
    hx_cond_t  non_plein;         /* drain  -> pousse (contre-pression) */
    FluxChunk* file_tete;
    FluxChunk* file_queue;
    size_t     file_octets;       /* octets cumules actuellement en file */
    size_t     cap;               /* borne cumulee EN OCTETS (D2) */
    int        producteur_fini;   /* owl_http_rep_flux_termine appele */
    int        avorte;            /* owl_http_rep_flux_avorte appele */
    int        consommateur_fini; /* drain abandonne (socket morte / HEAD) */
    int        refcount;

    /*  P5 — borne applicative de DUREE du flux, en ms (0 = illimite).
     * Estampillee a l'ouverture depuis route->delaiMax (secondes -> ms). La
     * contre-pression borne la MEMOIRE (cap), ce champ borne le TEMPS : le
     * transport (reacteur) arme un timer dedie qui, a echeance, marque le
     * consommateur fini (comme une mort de socket) -> la prochaine pousse rend
     * ABANDON. Le socle flux ne l'INTERPRETE pas ; il n'en est que le porteur. */
    int        delai_max_ms;

    /* Reveil du drain (W5, ) : appele sur le thread producteur apres
     * chaque pousse/termine/avorte pour reveiller la boucle reacteur. NULL en
     * mode bloquant historique. Appele SOUS fmx (le cb ne fait qu'un send
     * self-pipe non bloquant) : c'est ce qui garantit que le reacteur — donc
     * l'arret du pool — ne peut finaliser le flux AVANT que ce signal soit
     * emis. Sans ce verrou, un producteur pourrait signaler un pool deja
     * libere apres que le reacteur a ferme la connexion (UAF observe). */
    OwlHttpFluxNotif notif;
    void*            notif_user;
};

static void flux_detruit(OwlHttpRep* r);
static int  flux_draine(OwlHttpRep* r, const OwlHttpPuits* puits);

OwlHttpRep* owl_http_rep_cree_tampon(int statut, const char* type_contenu,
                                     const char* corps, size_t corps_len) {
    /* Statut hors 100-599 : la reponse n'est pas construite (famille 5.4).
     * Refuser plutot que rabattre sur 500 — un rabattement muet est un
     * bug qu'on ne verra jamais. */
    if (!owl_http_politique_statut_valide(statut)) return NULL;

    OwlHttpRep* r = (OwlHttpRep*)calloc(1, sizeof(OwlHttpRep));
    if (!r) return NULL;
    r->statut = statut;
    if (type_contenu && type_contenu[0]) {
        size_t n = strlen(type_contenu);
        if (n >= sizeof(r->type)) n = sizeof(r->type) - 1;
        memcpy(r->type, type_contenu, n);
        r->type[n] = '\0';
    }
    if (corps && corps_len > 0) {
        /* La COPIE est le point : c'est elle qui rend impossible de
         * laisser filer un AS_CSTRING vers le thread reseau (§2). */
        r->corps = (char*)malloc(corps_len + 1);
        if (!r->corps) { free(r); return NULL; }
        memcpy(r->corps, corps, corps_len);
        r->corps[corps_len] = '\0';
        r->corps_len = corps_len;
    }
    return r;
}

/* Mode B. Le fichier est ouvert ICI : sa longueur est donc
 * connue avant le premier octet emis, ce qui garde Content-Length
 * toujours calculable et le chunked sortant definitivement absent (§5).
 *
 * Aucune verification de confinement : elle appartient a la politique et
 * a elle seule (http_politique.c). Deux endroits qui verifient, c'est un
 * endroit qui finit par ne plus verifier. */
OwlHttpRep* owl_http_rep_cree_fichier(int statut, const char* type_contenu,
                                      const char* chemin) {
    if (!owl_http_politique_statut_valide(statut) || !chemin || !chemin[0]) return NULL;

    FILE* f = fopen(chemin, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long taille = ftell(f);
    if (taille < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }

    OwlHttpRep* r = (OwlHttpRep*)calloc(1, sizeof(OwlHttpRep));
    if (!r) { fclose(f); return NULL; }

    r->statut    = statut;
    r->fichier   = f;
    r->corps_len = (size_t)taille;
    if (type_contenu && type_contenu[0]) {
        size_t n = strlen(type_contenu);
        if (n >= sizeof(r->type)) n = sizeof(r->type) - 1;
        memcpy(r->type, type_contenu, n);
        r->type[n] = '\0';
    }
    return r;
}

bool owl_http_rep_est_fichier(const OwlHttpRep* r) { return r && r->fichier != NULL; }

/* Le point de passage UNIQUE du §6. Un nom ou une valeur hors norme est
 * REFUSE, jamais assaini : un assainissement muet transforme une
 * tentative d'injection en en-tete valide et personne ne le saura. */
int owl_http_rep_ajoute_entete(OwlHttpRep* r, const char* nom, const char* valeur) {
    if (!r || r->nb_entetes >= OWL_HTTP_REP_ENTETES_MAX) return -1;
    if (!owl_http_politique_nom_entete_valide(nom))       return -1;
    if (!owl_http_politique_valeur_entete_valide(valeur)) return -1;

    RepEntete* e = &r->entetes[r->nb_entetes];
    if (strlen(nom) >= sizeof(e->nom) || strlen(valeur) >= sizeof(e->valeur)) return -1;

    snprintf(e->nom,    sizeof(e->nom),    "%s", nom);
    snprintf(e->valeur, sizeof(e->valeur), "%s", valeur);
    r->nb_entetes++;
    return 0;
}

size_t owl_http_rep_nb_entetes(const OwlHttpRep* r) { return r ? r->nb_entetes : 0; }

const char* owl_http_rep_entete_nom(const OwlHttpRep* r, size_t i) {
    return (r && i < r->nb_entetes) ? r->entetes[i].nom : "";
}

const char* owl_http_rep_entete_valeur(const OwlHttpRep* r, size_t i) {
    return (r && i < r->nb_entetes) ? r->entetes[i].valeur : "";
}

void owl_http_rep_libere(OwlHttpRep* r) {
    if (!r) return;
    /* Un flux est refcompte : liberer = relacher une reference (le dernier
     * detruit reellement). Le transport appelle owl_http_rep_libere sans
     * savoir si c'est un tampon, un fichier ou un flux (§5). */
    if (r->est_flux) { owl_http_rep_flux_relache(r); return; }
    if (r->fichier) fclose(r->fichier);
    free(r->corps);
    free(r);
}

int         owl_http_rep_statut(const OwlHttpRep* r)       { return r ? r->statut : 0; }
const char* owl_http_rep_type_contenu(const OwlHttpRep* r) { return r ? r->type : ""; }
size_t      owl_http_rep_longueur(const OwlHttpRep* r)     { return r ? r->corps_len : 0; }

int owl_http_rep_ecrit(const OwlHttpRep* r, const OwlHttpPuits* puits) {
    if (!r || !puits || !puits->ecrit) return -1;

    /* Mode C : le corps n'existe pas encore en entier — il arrive au fil
     * de l'eau. Le drain enfile-desenfile et pose le cadrage chunked. Le
     * const du contrat est un mensonge assume ici : le flux mute (il draine
     * une file), mais le transport n'a pas a le savoir. */
    if (r->est_flux) return flux_draine((OwlHttpRep*)r, puits);

    if (r->corps_len == 0) return 0;

    /* Mode B : le fichier est streame par tranches, sans jamais etre
     * charge en entier et sans jamais rentrer dans la VM. Le tampon est
     * sur la pile du thread reseau — donc invisible du GC, ce que la
     * mesure sur gc_bytesAllocated verrouille. */
    if (r->fichier) {
        char tranche[16 * 1024];
        size_t reste = r->corps_len;
        if (fseek(r->fichier, 0, SEEK_SET) != 0) return -1;
        while (reste > 0) {
            size_t vise = reste < sizeof(tranche) ? reste : sizeof(tranche);
            size_t lu = fread(tranche, 1, vise, r->fichier);
            if (lu == 0) return -1;
            if (puits->ecrit(puits->user, tranche, lu) != 0) return -1;
            reste -= lu;
        }
        return 0;
    }

    return puits->ecrit(puits->user, r->corps, r->corps_len);
}

/* ================================================================
 * Mode C — source incrementale (flux chunked-out, )
 *
 * C pur : cette section ne connait toujours NI la VM NI le GC. La VM
 * POUSSE (owl_http_rep_flux_pousse), le transport DRAINE (flux_draine via
 * owl_http_rep_ecrit) — jamais l'inverse. Seuls des octets COPIES
 * traversent la file (regle d'or §2).
 * ================================================================ */

/* Un flux implique un corps : un cadrage chunked sur un statut sans corps
 * (204/304/1xx) est incoherent (D7). */
static int statut_sans_corps(int s) {
    return s == 204 || s == 304 || (s >= 100 && s < 200);
}

OwlHttpRep* owl_http_rep_cree_flux(int statut, const char* type_contenu,
                                   size_t borne_octets) {
    if (!owl_http_politique_statut_valide(statut)) return NULL;
    if (statut_sans_corps(statut)) return NULL;   /* D7 */

    OwlHttpRep* r = (OwlHttpRep*)calloc(1, sizeof(OwlHttpRep));
    if (!r) return NULL;
    r->statut = statut;
    if (type_contenu && type_contenu[0]) {
        size_t n = strlen(type_contenu);
        if (n >= sizeof(r->type)) n = sizeof(r->type) - 1;
        memcpy(r->type, type_contenu, n);
        r->type[n] = '\0';
    }
    r->est_flux  = 1;
    r->cap       = borne_octets;
    r->refcount  = 1;
    hx_mutex_init(&r->fmx);
    hx_cond_init(&r->non_vide);
    hx_cond_init(&r->non_plein);
    return r;
}

bool owl_http_rep_est_flux(const OwlHttpRep* r) { return r && r->est_flux; }

/*  P5 — borne applicative de duree (ms, 0 = illimite). Estampillee a
 * l'ouverture du flux ; lue par le transport pour armer son timer DelaiMax. */
void owl_http_rep_flux_delai_max_set(OwlHttpRep* r, int ms) {
    if (r && r->est_flux) r->delai_max_ms = (ms > 0 ? ms : 0);
}

int owl_http_rep_flux_delai_max_ms(const OwlHttpRep* r) {
    return (r && r->est_flux) ? r->delai_max_ms : 0;
}

OwlHttpFluxStatut owl_http_rep_flux_pousse(OwlHttpRep* r, const char* octets,
                                           size_t len) {
    if (!r || !r->est_flux) return OWL_HTTP_FLUX_ABANDON;

    hx_verrouille(&r->fmx);
    /* Contre-pression bornee EN OCTETS (D2). Regle « file vide -> un chunk
     * surdimensionne passe quand meme » : sinon un chunk plus gros que la
     * borne interbloquerait a jamais. */
    for (;;) {
        if (r->consommateur_fini) { hx_deverrouille(&r->fmx); return OWL_HTTP_FLUX_ABANDON; }
        if (r->file_octets == 0 || r->file_octets + len <= r->cap) break;

        /* File pleine : le handler doit attendre qu'un consommateur draine.
         * C'est LE point de yield cooperatif ( §3,  lot 4).
         * Si ce thread tient le GIL VM (cas nominal : le producteur tourne DANS
         * traite_une), on park son etat d'execution, on relache le GIL AUTOUR
         * de toute l'attente, on restore a la reprise. Ainsi un autre handler
         * progresse pendant qu'on est bloque, au lieu de monopoliser la VM.
         *
         * Ordre des verrous STRICT : on ne demande JAMAIS le GIL en tenant
         * r->fmx (deadlock potentiel). fmx est donc relache AVANT chaque
         * transition GIL, et l'attente sur non_plein se fait hors GIL. */
        if (VM_GIL_HELD_BY_ME()) {
            void* park = owl_vm_exec_park();      /* copie l'etat vif (heap) */
            hx_deverrouille(&r->fmx);             /* fmx AVANT de toucher le GIL */
            owl_vm_gil_release();

            /* Attente HORS GIL : un autre thread executeur peut prendre le GIL
             * et faire progresser son handler. */
            hx_verrouille(&r->fmx);
            while (!r->consommateur_fini &&
                   !(r->file_octets == 0 || r->file_octets + len <= r->cap)) {
                hx_cond_attend(&r->non_plein, &r->fmx, -1);
            }
            hx_deverrouille(&r->fmx);             /* fmx AVANT de redemander le GIL */

            owl_vm_gil_acquire();
            owl_vm_exec_restore(park);            /* recopie dans les MEMES arrays */
            hx_verrouille(&r->fmx);               /* re-verrouille pour re-boucler */
            continue;
        }

        /* Hors GIL (appel direct hors executeur, ex. fast-path draine inline
         * ou harnais bas niveau) : attente historique simple sous fmx. */
        hx_cond_attend(&r->non_plein, &r->fmx, -1);
    }
    /* Un push vide n'enfile RIEN : il ne doit pas emettre de chunk 0
     * (= terminal) au milieu du flux. */
    if (len == 0) { hx_deverrouille(&r->fmx); return OWL_HTTP_FLUX_OK; }

    FluxChunk* c = (FluxChunk*)malloc(sizeof(FluxChunk));
    if (!c) { hx_deverrouille(&r->fmx); return OWL_HTTP_FLUX_ABANDON; }
    c->donnees = (char*)malloc(len);
    if (!c->donnees) { free(c); hx_deverrouille(&r->fmx); return OWL_HTTP_FLUX_ABANDON; }
    memcpy(c->donnees, octets, len);   /* LA copie : aucun ObjString* ne traverse (§2) */
    c->len     = len;
    c->suivant = NULL;

    if (r->file_queue) r->file_queue->suivant = c; else r->file_tete = c;
    r->file_queue    = c;
    r->file_octets  += len;
    hx_cond_signale(&r->non_vide);
    if (r->notif) r->notif(r->notif_user);   /* reveil boucle : chunk dispo (W5) */
    hx_deverrouille(&r->fmx);
    return OWL_HTTP_FLUX_OK;
}

void owl_http_rep_flux_termine(OwlHttpRep* r) {
    if (!r || !r->est_flux) return;
    hx_verrouille(&r->fmx);
    r->producteur_fini = 1;
    hx_cond_signale(&r->non_vide);
    if (r->notif) r->notif(r->notif_user);   /* reveil : le drain emet le terminal */
    hx_deverrouille(&r->fmx);
}

void owl_http_rep_flux_avorte(OwlHttpRep* r) {
    if (!r || !r->est_flux) return;
    hx_verrouille(&r->fmx);
    r->avorte = 1;
    hx_cond_signale(&r->non_vide);
    if (r->notif) r->notif(r->notif_user);   /* reveil : fermeture abrupte a traiter */
    hx_deverrouille(&r->fmx);
}

/* Drain non bloquant (W5) : voir http_exec.h. Le mutex fmx n'est tenu que le
 * temps de retirer/consulter — jamais d'attente sur condvar. */
OwlHttpFluxTire owl_http_rep_flux_tire(OwlHttpRep* r, char** donnees, size_t* len) {
    if (!r || !r->est_flux) return OWL_HTTP_FLUX_TIRE_MORT;

    hx_verrouille(&r->fmx);
    if (r->consommateur_fini) { hx_deverrouille(&r->fmx); return OWL_HTTP_FLUX_TIRE_MORT; }

    FluxChunk* c = r->file_tete;
    if (c) {
        r->file_tete = c->suivant;
        if (!r->file_tete) r->file_queue = NULL;
        r->file_octets -= c->len;
        hx_cond_signale(&r->non_plein);   /* de la place : reveille le producteur */
        hx_deverrouille(&r->fmx);
        *donnees = c->donnees;   /* propriete transferee a l'appelant */
        *len     = c->len;
        free(c);                 /* le noeud, pas les donnees */
        return OWL_HTTP_FLUX_TIRE_DONNEE;
    }

    int avorte = r->avorte;
    int fini   = r->producteur_fini;
    hx_deverrouille(&r->fmx);

    if (avorte) return OWL_HTTP_FLUX_TIRE_AVORTE;
    if (fini)   return OWL_HTTP_FLUX_TIRE_FINI;
    return OWL_HTTP_FLUX_TIRE_VIDE;
}

void owl_http_rep_flux_marque_consommateur_fini(OwlHttpRep* r) {
    if (!r || !r->est_flux) return;
    hx_verrouille(&r->fmx);
    r->consommateur_fini = 1;
    hx_cond_signale(&r->non_plein);
    hx_deverrouille(&r->fmx);
}

void owl_http_rep_flux_definit_notif(OwlHttpRep* r, OwlHttpFluxNotif cb, void* user) {
    if (!r || !r->est_flux) return;
    hx_verrouille(&r->fmx);
    r->notif      = cb;
    r->notif_user = user;
    hx_deverrouille(&r->fmx);
}

/* Detruit reellement le flux (dernier relache). Vide la file : des octets
 * possedes, jamais un objet GC. */
static void flux_detruit(OwlHttpRep* r) {
    FluxChunk* c = r->file_tete;
    while (c) { FluxChunk* n = c->suivant; free(c->donnees); free(c); c = n; }
    hx_cond_detruit(&r->non_plein);
    hx_cond_detruit(&r->non_vide);
    hx_mutex_detruit(&r->fmx);
    free(r->corps);
    free(r);
}

void owl_http_rep_flux_relache(OwlHttpRep* r) {
    if (!r) return;
    hx_verrouille(&r->fmx);
    int reste = --r->refcount;
    hx_deverrouille(&r->fmx);
    if (reste == 0) flux_detruit(r);
}

OwlHttpFluxAmorce owl_http_rep_flux_attend_premier(OwlHttpRep* r) {
    if (!r || !r->est_flux) return OWL_HTTP_FLUX_AMORCE_AVORTE;
    hx_verrouille(&r->fmx);
    while (r->file_tete == NULL && !r->producteur_fini && !r->avorte) {
        hx_cond_attend(&r->non_vide, &r->fmx, -1);
    }
    OwlHttpFluxAmorce a;
    if      (r->file_tete != NULL) a = OWL_HTTP_FLUX_AMORCE_DONNEE;
    else if (r->avorte)            a = OWL_HTTP_FLUX_AMORCE_AVORTE;
    else                           a = OWL_HTTP_FLUX_AMORCE_VIDE;  /* fini, aucun octet */
    hx_deverrouille(&r->fmx);
    return a;
}

/* Draine la file vers le puits en posant le cadrage chunked (RFC 9112
 * §7.1). Le cadrage est pose par le TRANSPORT et jamais derive du
 * contenu : la taille = longueur REELLE des octets, les CRLF sont ajoutes
 * ici. Trois ecritures par chunk (D6 — prefixe / donnees / CRLF), aucune
 * copie du corps. 0 = flux termine proprement (chunk terminal emis),
 * non-nul = fermeture abrupte (avorte, ou puits mort). */
static int flux_draine(OwlHttpRep* r, const OwlHttpPuits* puits) {
    for (;;) {
        hx_verrouille(&r->fmx);
        while (r->file_tete == NULL && !r->producteur_fini && !r->avorte) {
            hx_cond_attend(&r->non_vide, &r->fmx, -1);
        }
        FluxChunk* c = r->file_tete;
        if (c) {
            r->file_tete = c->suivant;
            if (!r->file_tete) r->file_queue = NULL;
            r->file_octets -= c->len;
            hx_cond_signale(&r->non_plein);   /* de la place : reveille le producteur */
        }
        int avorte = r->avorte;
        int fini   = r->producteur_fini;
        hx_deverrouille(&r->fmx);

        if (c) {
            char prefixe[32];
            int  n = snprintf(prefixe, sizeof(prefixe), "%lx\r\n", (unsigned long)c->len);
            int  echec = 0;
            if (             puits->ecrit(puits->user, prefixe, (size_t)n) != 0) echec = 1;
            if (!echec &&    puits->ecrit(puits->user, c->donnees, c->len) != 0) echec = 1;
            if (!echec &&    puits->ecrit(puits->user, "\r\n", 2)          != 0) echec = 1;
            free(c->donnees);
            free(c);
            if (echec) {
                /* Consommateur mort (§8) : le producteur doit s'arreter.
                 * La prochaine pousse rendra ABANDON. */
                hx_verrouille(&r->fmx);
                r->consommateur_fini = 1;
                hx_cond_signale(&r->non_plein);
                hx_deverrouille(&r->fmx);
                return -1;
            }
            continue;
        }

        /* Plus de chunk en file. */
        if (avorte) return -1;                 /* fermeture abrupte, PAS de terminal */
        if (fini) {                            /* fin nominale : chunk terminal */
            if (puits->ecrit(puits->user, "0\r\n\r\n", 5) != 0) return -1;
            return 0;
        }
    }
}

/* ================================================================
 * 3. Jeton — la copie possedee d'une requete (§2, §3)
 * ================================================================ */

struct OwlHttpJeton {
    char   methode[32];
    char*  chemin;
    char*  corps;
    size_t corps_len;

    /*  — valeur brute de l'en-tete Authorization de la requete
     * entrante (NULL si absent). Copie possedee (comme chemin/corps) : le jeton
     * vit dans la file bornee, aucun ObjString GC ne traverse la frontiere. Le
     * credential (jeton Bearer...) en est extrait au thread VM selon le schema. */
    char*  autorisation;

    /*  — valeur brute de l'en-tete X-API-Key de la requete entrante
     * (NULL si absent). Copie possedee comme `autorisation`. Source STRICTE et
     * exclusive du credential pour le schema httpAuthCléAPI : jamais melangee
     * avec `autorisation` (un Authorization: Bearer ne sert pas de cle API). */
    char*  cleApi;

    /*  — valeur brute de l'en-tete Content-Type de la requete entrante
     * (NULL si absent). Copie possedee comme `autorisation`. Le serve-time en
     * extrait le boundary pour parser un corps multipart/form-data. */
    char*  contentType;

    /*  — copie POSSEDEE de TOUS les en-tetes entrants bruts (nom+valeur),
     * miroir de OwlHttpRequete.entetes[]. Convoyee pour exposer
     * MoiMême..Requête..Entêtes (lecture) au thread VM. Copie profonde (regle
     * d'or §2 : le jeton vit dans la file bornee, aucun pointeur GC ni pointeur
     * dans OwlHttpRequete ne traverse la frontiere transport/VM). Bornee par
     * nb_entetes de la requete (deja plafonne par limites.entetes_nb_max). */
    OwlHttpEntete* entetes;
    size_t         nb_entetes;

    /* Echeance ABSOLUE posee AU DEPOT (§7) : un job qui a trop attendu
     * est abandonne, pas execute — le client a probablement renonce. */
    uint64_t echeance;
    int      sans_echeance;

    int fait;
    int expire;
    int abandonne;          /* §8 : le transport n'attend plus */

    /* Mode C. `demarre` : le job a ete depile et lance — a
     * partir de la le plafond d'attente ne s'applique plus (D1). Un flux
     * rend la main au transport des `flux_demarre` (1er chunk pret), pas a
     * `fait`. `transport_detache` : le transport a pris le flux ; le thread
     * VM peut alors liberer le JETON (pas le flux, refcompte). */
    int demarre;
    int flux_demarre;
    int transport_detache;

    /* Reveil du reacteur async (W7) : appele SOUS x->m a chaque resolution
     * (flux_demarre, fait, expiry). Le reacteur ne bloque jamais dessus — il
     * consulte l'etat via owl_http_executeur_essaie quand ce hook le tire. */
    OwlHttpJetonNotif notif;
    void*             notif_user;

    OwlHttpRep* rep;

    struct OwlHttpJeton* suivant;
};

static char* copie_octets(const char* src, size_t len) {
    char* d = (char*)malloc(len + 1);
    if (!d) return NULL;
    if (len > 0 && src) memcpy(d, src, len);
    d[len] = '\0';
    return d;
}

static void jeton_libere(OwlHttpJeton* j) {
    if (!j) return;
    free(j->chemin);
    free(j->corps);
    free(j->autorisation);
    free(j->cleApi);
    free(j->contentType);
    /*  — libere la liste possedee des en-tetes entrants (miroir des
     * copies profondes nom/valeur faites au depot). */
    for (size_t hi = 0; hi < j->nb_entetes; hi++) {
        free(j->entetes[hi].nom);
        free(j->entetes[hi].valeur);
    }
    free(j->entetes);
    owl_http_rep_libere(j->rep);
    free(j);
}

/* Reveil du reacteur async (W7) : appele SOUS x->m aux points de resolution.
 * Le hook ne fait qu'un signal self-pipe non bloquant (comme le notif de
 * flux) — l'appeler sous verrou garantit que le reacteur ne peut finaliser la
 * connexion avant que le signal soit emis (meme discipline que le notif de
 * flux, cf. commentaire du champ notif d'OwlHttpRep). */
static void jeton_reveille(OwlHttpJeton* j) {
    if (j->notif) j->notif(j->notif_user);
}

/* ================================================================
 * 4. Executeur
 * ================================================================ */

#define HX_PROFONDEUR_DEFAUT 64
#define HX_PLAFOND_DEFAUT_MS 5000

/* Plafond du pool de drain VM (, LOT 3). Borne le tableau de handles
 * de threads dans la struct ; OWL_VM_THREADS est clampé à cette valeur. Calqué
 * OWL_PAR_MAX_THREADS (owl_parallel.c). */
#define OWL_VM_MAX_THREADS 32

struct OwlHttpExecuteur {
    OwlHttpExecConfig cfg;
    char* source_owl;
    char* procedure;

    /* Voie PROJET : copie profonde possedee des tableaux paralleles
     * de la frontiere, recompilee sur CE thread par prepare_vm. NULL/0 hors
     * mode projet (fallback source_owl). */
    char** projet_noms;
    char** projet_chemins;
    char** projet_sources;
    int    projet_nb;
    int    projet_entree;
    int    projet_mode;

    hx_mutex_t m;
    hx_cond_t  file_non_vide;   /* depot -> executeur */
    hx_cond_t  job_termine;     /* executeur -> transport */

    OwlHttpJeton* tete;
    OwlHttpJeton* queue;
    size_t        profondeur;

    /* Mode C : le job en cours d'execution, accede SOUS x->m.
     * Le producteur y publie son flux (owl_http_prod_ouvre_flux) — c'est
     * le seul pont entre le thread VM qui pousse et le transport qui
     * attend le 1er chunk. */
    OwlHttpJeton* jeton_courant;

    size_t jobs_pris;
    size_t rejets;

    int arret;
    int thread_lance;
    int vm_prete;
    int vm_echec;
    hx_thread_t thread;                 /* thread MAITRE : bootstrap + drain + teardown */

    /* Pool de drain (, LOT 3) : threads exécuteurs ADDITIONNELS qui
     * drainent le MEME FIFO sous x->m, l'exécution VM étant sérialisée par le
     * GIL (owl_vm_gil_acquire/release). Le maître (x->thread) est le M-ième
     * drain ; ce tableau porte les M-1 autres. Spawné après le bootstrap
     * one-shot, joint par le maître avant vm_free. OWL_VM_THREADS=1 -> vide
     * (cas dégénéré = comportement mono-thread du LOT 1). */
    hx_thread_t pool[OWL_VM_MAX_THREADS - 1];
    size_t      pool_nb;

    /* Identifiants des threads ayant execute un job. Le contrat §1 (mono-thread)
     * ayant cédé au pool GIL-sérialisé (LOT 3), ceci MESURE désormais combien de
     * threads distincts ont drainé — attendu jusqu'à OWL_VM_THREADS. Huit places
     * suffisent : le GIL garantit qu'un seul exécute à un instant donné, seul le
     * COMPTE de threads ayant participé importe. */
    unsigned long threads_vm[8];
    size_t        nb_threads_vm;
};

/* Contexte producteur (mode C, ). Le seam de test C tient juste
 * l'executeur : owl_http_prod_ouvre_flux publie le flux via x->jeton_courant
 * sous x->m. Le pont OWL de la phase B remplacera ce seam. */
struct OwlHttpProdCtx {
    OwlHttpExecuteur* x;

    /* Fast-path plan de controle (, W2) : quand `fast_path` vaut 1,
     * le producteur ne tourne PAS dans le thread VM derriere la FIFO — il est
     * appele INLINE sur le thread appelant (reacteur ou worker), qui contourne
     * ainsi la file mono-thread. owl_http_prod_ouvre_flux publie alors le flux
     * dans `fast_flux` au lieu de x->jeton_courant, et le drain est fait par
     * l'appelant APRES le retour du producteur (pas de concurrence). */
    int         fast_path;
    OwlHttpRep* fast_flux;

    /*  — surface OWL FluxÉcrit. `flux_mode` porte le framing
     * (OWL_HTTP_FLUX_MODE_*), pose par la voie service (P3) ou le harnais de
     * test (P2). `flux_courant` cache le flux ouvert en paresseux par
     * owl_http_prod_flux_assure : borrowed (une des 2 refs prod+transport), le
     * proprietaire le termine + relache. */
    int         flux_mode;
    OwlHttpRep* flux_courant;

    /*  P5 — borne applicative de duree (ms, 0 = illimite), posee par la
     * voie service depuis route->delaiMax AVANT l'appel du handler. Estampillee
     * sur le flux DANS owl_http_prod_ouvre_flux (avant publication au transport,
     * donc sans course avec le drain qui resout puis interroge le delai). */
    int         delai_max_ms;
};

void owl_http_prod_flux_mode_set(OwlHttpProdCtx* ctx, int mode) {
    if (ctx) ctx->flux_mode = mode;
}

void owl_http_prod_flux_delai_max_set(OwlHttpProdCtx* ctx, int ms) {
    if (ctx) ctx->delai_max_ms = (ms > 0 ? ms : 0);
}

int owl_http_prod_flux_mode(const OwlHttpProdCtx* ctx) {
    return ctx ? ctx->flux_mode : OWL_HTTP_FLUX_MODE_SSE;
}

OwlHttpRep* owl_http_prod_flux_courant(const OwlHttpProdCtx* ctx) {
    return ctx ? ctx->flux_courant : NULL;
}

/* Publie le flux du job courant : bump le refcount a 2 (producteur +
 * transport) et reveille owl_http_executeur_attend, qui prend le flux. */
OwlHttpRep* owl_http_prod_ouvre_flux(OwlHttpProdCtx* ctx, int statut,
                                     const char* type_contenu,
                                     size_t borne_octets) {
    if (!ctx || !ctx->x) return NULL;
    OwlHttpExecuteur* x = ctx->x;

    /* Fast-path : flux autonome, draine par l'appelant apres retour du
     * producteur. La borne de contre-pression est EFFACEE — il n'y a aucun
     * drain concurrent (thread unique), donc une file bornee interbloquerait
     * le producteur des qu'il pousse au-dela de la borne. Les routes de
     * controle sont petites par contrat (design §8 Q1), le tampon reste
     * modeste malgre la borne effacee. */
    if (ctx->fast_path) {
        (void)borne_octets;
        OwlHttpRep* f = owl_http_rep_cree_flux(statut, type_contenu, (size_t)-1);
        if (!f) return NULL;
        f->refcount     = 2;   /* producteur + appelant fast-path */
        f->delai_max_ms = ctx->delai_max_ms;   /* P5 : borne de duree (0 = illimite) */
        ctx->fast_flux  = f;
        return f;
    }

    OwlHttpRep* f = owl_http_rep_cree_flux(statut, type_contenu, borne_octets);
    if (!f) return NULL;   /* statut sans corps (D7) ou allocation echouee */

    /* P5 : estampiller la borne de duree AVANT toute publication (j->rep = f
     * ci-dessous rend le flux visible au drain, qui la lit une fois resolu). */
    f->delai_max_ms = ctx->delai_max_ms;

    hx_verrouille(&x->m);
    OwlHttpJeton* j = x->jeton_courant;
    if (!j || j->abandonne) {
        /* Le transport a renonce avant le 1er octet : le flux est marque
         * mort pour que la 1re pousse rende ABANDON et que le producteur
         * s'arrete (pas d'interblocage sur une file que personne ne draine).
         * Le producteur reste seul proprietaire (refcount 1) et relachera.
         * Acces direct sans fmx : le flux n'est pas encore partage. */
        hx_deverrouille(&x->m);
        f->consommateur_fini = 1;
        return f;
    }
    f->refcount     = 2;           /* producteur + transport */
    j->rep          = f;
    j->flux_demarre = 1;
    hx_cond_signale(&x->job_termine);
    jeton_reveille(j);             /* reacteur async : le 1er chunk est pret (W7) */
    hx_deverrouille(&x->m);
    return f;
}

void owl_http_exec_config_defaut(OwlHttpExecConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->file_profondeur    = HX_PROFONDEUR_DEFAUT;
    cfg->plafond_attente_ms = HX_PLAFOND_DEFAUT_MS;
}

static char* copie_chaine(const char* s) {
    if (!s) return NULL;
    return copie_octets(s, strlen(s));
}

/* Copie profonde d'un tableau de n chaines. Renvoie NULL si n<=0 ou src NULL.
 * En cas d'echec partiel, libere ce qui a ete copie et renvoie NULL. */
static char** copie_tableau_chaines(const char* const* src, int n) {
    if (!src || n <= 0) return NULL;
    char** dst = (char**)calloc((size_t)n, sizeof(char*));
    if (!dst) return NULL;
    for (int i = 0; i < n; i++) {
        dst[i] = copie_chaine(src[i]);
        if (src[i] && !dst[i]) {
            for (int k = 0; k < i; k++) free(dst[k]);
            free(dst);
            return NULL;
        }
    }
    return dst;
}

static void libere_tableau_chaines(char** t, int n) {
    if (!t) return;
    for (int i = 0; i < n; i++) free(t[i]);
    free(t);
}

OwlHttpExecuteur* owl_http_executeur_cree(const OwlHttpExecConfig* cfg) {
    OwlHttpExecuteur* x = (OwlHttpExecuteur*)calloc(1, sizeof(OwlHttpExecuteur));
    if (!x) return NULL;
    x->cfg = *cfg;
    if (x->cfg.file_profondeur == 0) x->cfg.file_profondeur = HX_PROFONDEUR_DEFAUT;
    x->source_owl = copie_chaine(cfg->source_owl);
    x->procedure  = copie_chaine(cfg->procedure);
    x->cfg.source_owl = x->source_owl;
    x->cfg.procedure  = x->procedure;

    /* Voie PROJET : copie profonde des tableaux paralleles. Les
     * pointeurs de cfg sont empruntes (l'appelant les libere apres cree) ; on
     * repointe cfg vers nos copies pour ne jamais lire de pendante. */
    if (cfg->projet_nb > 0 && cfg->projet_sources) {
        x->projet_nb      = cfg->projet_nb;
        x->projet_entree  = cfg->projet_entree;
        x->projet_mode    = cfg->projet_mode;
        x->projet_noms    = copie_tableau_chaines(cfg->projet_noms,    cfg->projet_nb);
        x->projet_chemins = copie_tableau_chaines(cfg->projet_chemins, cfg->projet_nb);
        x->projet_sources = copie_tableau_chaines(cfg->projet_sources, cfg->projet_nb);
        x->cfg.projet_noms    = (const char* const*)x->projet_noms;
        x->cfg.projet_chemins = (const char* const*)x->projet_chemins;
        x->cfg.projet_sources = (const char* const*)x->projet_sources;
    }

    hx_mutex_init(&x->m);
    hx_cond_init(&x->file_non_vide);
    hx_cond_init(&x->job_termine);
    return x;
}

void owl_http_executeur_libere(OwlHttpExecuteur* x) {
    if (!x) return;
    if (x->thread_lance) owl_http_executeur_arrete(x);

    OwlHttpJeton* j = x->tete;
    while (j) { OwlHttpJeton* s = j->suivant; jeton_libere(j); j = s; }

    hx_cond_detruit(&x->job_termine);
    hx_cond_detruit(&x->file_non_vide);
    hx_mutex_detruit(&x->m);
    free(x->source_owl);
    free(x->procedure);
    libere_tableau_chaines(x->projet_noms,    x->projet_nb);
    libere_tableau_chaines(x->projet_chemins, x->projet_nb);
    libere_tableau_chaines(x->projet_sources, x->projet_nb);
    free(x);
}

size_t owl_http_executeur_jobs_pris(const OwlHttpExecuteur* x)  { return x ? x->jobs_pris : 0; }
size_t owl_http_executeur_rejets(const OwlHttpExecuteur* x)     { return x ? x->rejets : 0; }
size_t owl_http_executeur_profondeur(const OwlHttpExecuteur* x) { return x ? x->profondeur : 0; }

size_t owl_http_executeur_nb_threads_vm(const OwlHttpExecuteur* x) {
    return x ? x->nb_threads_vm : 0;
}

/* Releve l'identifiant du thread courant a chaque entree dans la VM.
 * Appele sous le verrou de l'executeur. */
static void note_thread_vm(OwlHttpExecuteur* x) {
#ifdef _WIN32
    unsigned long id = (unsigned long)GetCurrentThreadId();
#else
    unsigned long id = (unsigned long)pthread_self();
#endif
    for (size_t i = 0; i < x->nb_threads_vm; i++) {
        if (x->threads_vm[i] == id) return;
    }
    if (x->nb_threads_vm < sizeof(x->threads_vm) / sizeof(x->threads_vm[0])) {
        x->threads_vm[x->nb_threads_vm++] = id;
    }
}

/* ---------- Depot : le seul endroit qui produit un SATURE ---------- */

OwlHttpExecStatut owl_http_executeur_depose(OwlHttpExecuteur* x,
                                            const OwlHttpRequete* req,
                                            OwlHttpJeton** out_jeton) {
    if (out_jeton) *out_jeton = NULL;
    if (!x || !req) return OWL_HTTP_EXEC_SATURE;

    hx_verrouille(&x->m);

    /* File pleine -> SATURE IMMEDIAT, AVANT tout signal de condvar.
     * L'executeur n'est pas reveille : c'est tout l'interet du 503
     * produit par le transport seul (§7), et jobs_pris — incremente
     * uniquement dans traite_une — le prouve. */
    if (x->profondeur >= x->cfg.file_profondeur || x->arret) {
        x->rejets++;
        hx_deverrouille(&x->m);
        return OWL_HTTP_EXEC_SATURE;
    }
    hx_deverrouille(&x->m);

    OwlHttpJeton* j = (OwlHttpJeton*)calloc(1, sizeof(OwlHttpJeton));
    if (!j) return OWL_HTTP_EXEC_SATURE;

    snprintf(j->methode, sizeof(j->methode), "%s", req->methode);
    j->chemin    = copie_chaine(req->chemin ? req->chemin : "");
    j->corps     = copie_octets(req->corps, req->corps_len);
    j->corps_len = req->corps_len;
    if (!j->chemin || !j->corps) { jeton_libere(j); return OWL_HTTP_EXEC_SATURE; }

    /*  — capture l'en-tete Authorization (casse-insensible) pour que
     * le thread VM en extraie le credential selon le schema d'auth du service.
     * Les autres en-tetes sont jetes (non requis en lot 1). */
    /*  — capture aussi X-API-Key (casse-insensible), source stricte du
     * credential pour httpAuthCléAPI. Les deux en-tetes sont captures dans la
     * meme passe ; la selection selon le schema se fait au thread VM. */
    /*  — convoie AUSSI l'ensemble des en-tetes bruts (nom+valeur) vers
     * le job, pour exposer MoiMême..Requête..Entêtes (lecture) au thread VM.
     * Copie profonde possedee, bornee par req->nb_entetes (deja plafonne). */
    if (req->nb_entetes > 0) {
        j->entetes = (OwlHttpEntete*)calloc(req->nb_entetes, sizeof(OwlHttpEntete));
        if (!j->entetes) { jeton_libere(j); return OWL_HTTP_EXEC_SATURE; }
    }
    for (size_t hi = 0; hi < req->nb_entetes; hi++) {
        if (!req->entetes[hi].nom) continue;
        const char* nom = req->entetes[hi].nom;
        const char* val = req->entetes[hi].valeur ? req->entetes[hi].valeur : "";
        if (j->autorisation == NULL && strcasecmp(nom, "Authorization") == 0) {
            j->autorisation = copie_chaine(val);
        } else if (j->cleApi == NULL && strcasecmp(nom, "X-API-Key") == 0) {
            j->cleApi = copie_chaine(val);
        } else if (j->contentType == NULL && strcasecmp(nom, "Content-Type") == 0) {
            /*  — capture pour extraire le boundary multipart au thread VM. */
            j->contentType = copie_chaine(val);
        }
        /* Convoyage integral : chaque en-tete (y compris les 3 ci-dessus) est
         * aussi recopie dans la liste possedee. La sous-instance Requête expose
         * la vue brute complete, les extractions ciblees restent independantes. */
        j->entetes[j->nb_entetes].nom    = copie_chaine(nom);
        j->entetes[j->nb_entetes].valeur = copie_chaine(val);
        j->nb_entetes++;
    }

    if (x->cfg.plafond_attente_ms > 0) {
        j->echeance = owl_tcp_server_horloge_ms() +
                      (uint64_t)x->cfg.plafond_attente_ms;
    } else {
        j->sans_echeance = 1;
    }

    hx_verrouille(&x->m);
    /* Re-check APRES le re-lock : entre le premier controle et ici, le verrou
     * a ete relache pour la copie des octets. Des depots concurrents ont pu
     * remplir la file dans cet intervalle — sans ce second controle, le cap
     * n'est pas strictement tenu sous rafale (le 503 fuit au-dela de la
     * profondeur). Le jeton deja alloue est libere, pas enfile. */
    if (x->profondeur >= x->cfg.file_profondeur || x->arret) {
        x->rejets++;
        hx_deverrouille(&x->m);
        jeton_libere(j);
        return OWL_HTTP_EXEC_SATURE;
    }
    if (x->queue) x->queue->suivant = j; else x->tete = j;
    x->queue = j;
    x->profondeur++;
    hx_cond_signale(&x->file_non_vide);
    hx_deverrouille(&x->m);

    if (out_jeton) *out_jeton = j;
    return OWL_HTTP_EXEC_OK;
}

/* ---------- Marshalling + appel de la procedure OWL ----------
 *
 * Tourne DANS le thread proprietaire de la VM, et nulle part ailleurs.
 * Les globales de harnais sont une convention jetable (cf. http_exec.h),
 * pas une surface de langage. */

static void pose_globale_chaine(const char* nom, const char* octets, size_t len) {
    ObjString* s = obj_copy_string(octets ? octets : "", (int)len);
    vm_push(OBJ_VAL(s));            /* root explicite : obj_globals_set alloue */
    obj_globals_set(nom, OBJ_VAL(s));
    vm_pop();
}

static bool lit_globale(const char* nom, Value* out) {
    ObjString* k = obj_copy_string(nom, (int)strlen(nom));
    vm_push(OBJ_VAL(k));
    bool ok = table_get(&vm.globals, k, out);
    vm_pop();
    return ok;
}

/* ---------- Voie SERVICE : routage + invocation du handler ----------
 *
 * Fondation runtime du DSL de service. Tourne DANS le thread proprietaire de la
 * VM, comme la voie harnais. `MoiMême` est une ObjInstance implicite portant deux
 * sous-objets FRAIS : `Réponse` (ecriture : `MoiMême..Réponse..CodeÉtat/..Corps/
 * ..TypeContenu`) et `Requête` (lecture : `MoiMême..Requête..Méthode/..Chemin/
 * ..Identité`), semes plus bas. OP_MOIMEME resout vers le sommet de
 * event_sender_stack. */

/* Lit un champ de la reponse par nom accent+casse-insensible (le handler peut
 * ecrire CodeÉtat/CodeEtat/StatusCode). Retourne true si trouve. */
static bool service_lit_champ(ObjInstance* inst, const char* const* noms,
                              Value* out) {
    for (int i = 0; i < inst->fields.capacity; i++) {
        Entry* e = &inst->fields.entries[i];
        if (e->key == NULL) continue;
        char stripped[128];
        int n = scanner_strip_accents(e->key->chars, e->key->length,
                                      stripped, sizeof(stripped));
        if (n <= 0) continue;
        for (int j = 0; noms[j] != NULL; j++) {
            if ((int)strlen(noms[j]) == n
                && strncasecmp(stripped, noms[j], (size_t)n) == 0) {
                *out = e->value;
                return true;
            }
        }
    }
    return false;
}

/*  — recupere une sous-instance du contexte d'echange (Réponse /
 * Requête) semee sur `rep`. Lookup accent+casse-insensible via service_lit_champ.
 * NULL si absente ou non-instance. */
static ObjInstance* service_lit_sous_instance(ObjInstance* inst,
                                              const char* const* noms) {
    Value v;
    if (service_lit_champ(inst, noms, &v) && IS_INSTANCE(v)) {
        return AS_INSTANCE(v);
    }
    return NULL;
}

/*  — ouverture paresseuse du flux courant pour la native FluxÉcrit.
 * 1er appel : statut/type lus depuis MoiMême..Réponse si un contexte d'echange
 * service est present (voie P3), defauts sinon (test / usage direct) ; ouvre le
 * flux et le met en cache. Appels suivants : renvoie le meme flux. */
OwlHttpRep* owl_http_prod_flux_assure(OwlHttpProdCtx* ctx) {
    if (!ctx) return NULL;
    if (ctx->flux_courant) return ctx->flux_courant;

    int statut = 200;
    char type[128];
    snprintf(type, sizeof(type), "%s",
             (ctx->flux_mode == OWL_HTTP_FLUX_MODE_OCTETS)
                 ? "application/octet-stream" : "text/event-stream");

    /* MoiMême..Réponse..CodeÉtat / ..TypeContenu si un contexte d'echange est
     * present (event_sender = ObjInstance). Hors service, MoiMême est absent
     * -> defauts ci-dessus. */
    Value moi = vm_event_sender_top();
    if (IS_INSTANCE(moi)) {
        static const char* const nomsReponse[] = { "Reponse", "Response", NULL };
        static const char* const nomsStatut[]  = { "CodeEtat", "StatusCode", NULL };
        static const char* const nomsType[]    = { "TypeContenu", "ContentType", NULL };
        ObjInstance* subRep = service_lit_sous_instance(AS_INSTANCE(moi), nomsReponse);
        Value v;
        if (subRep && service_lit_champ(subRep, nomsStatut, &v) && IS_NUMBER(v)) {
            int s = (int)AS_NUMBER(v);
            if (s >= 100 && s <= 599) statut = s;
        }
        if (subRep && service_lit_champ(subRep, nomsType, &v) && IS_STRING(v)) {
            snprintf(type, sizeof(type), "%s", AS_CSTRING(v));
        }
    }

    /* borne_octets : la contre-pression borne la MEMOIRE, pas la duree
     * ( §6). Cache la ref (borrowed : une des 2 refs prod+transport). */
    ctx->flux_courant = owl_http_prod_ouvre_flux(ctx, statut, type, (size_t)1 << 20);
    return ctx->flux_courant;
}

/*  — deserialisation guidee du corps (impl dans stdlib_json.c) :
 * 0 = ok (*out rempli), 1 = JSON malforme, 2 = type structure introuvable. */
extern int owl_json_bind_body(const char* typeName, const char* body,
                              size_t len, Value* out);

/*  — serialise une Value OWL en JSON (buffer malloc'd, a liberer). */
extern char* owl_json_serialize_value(Value v, size_t* outLen);

/* Coercion souple d'un segment/valeur texte vers le type declare du parametre.
 * Type inconnu ou chaine -> ObjString brut. La valeur retournee doit etre
 * pushee IMMEDIATEMENT (racine GC) : aucune alloc entre ici et le push. */
static Value service_coerce_scalar(const char* text, const char* typeName) {
    if (text == NULL) text = "";
    char t[64];
    int n = 0;
    if (typeName != NULL) {
        for (const char* s = typeName; *s && n < 63; s++) {
            t[n++] = (char)tolower((unsigned char)*s);
        }
    }
    t[n] = '\0';
    if (strstr(t, "entier") || strstr(t, "integer")) {
        return INTEGER_VAL((int64_t)strtoll(text, NULL, 10));
    }
    if (strstr(t, "reel") || strstr(t, "real") || strstr(t, "numerique")
        || strstr(t, "numeric")) {
        return REAL_VAL(strtod(text, NULL));
    }
    if (strstr(t, "booleen") || strstr(t, "boolean") || strcmp(t, "bool") == 0) {
        int v = (strcasecmp(text, "vrai") == 0 || strcasecmp(text, "true") == 0
                 || strcmp(text, "1") == 0);
        return BOOL_VAL(v);
    }
    return OBJ_VAL(obj_copy_string(text, (int)strlen(text)));
}

/*  — vrai si le type declare d'un parametre est un conteneur tableau
 * (`tableau de chaine` / `array of ...`). Le 1er token du type suffit : le
 * compilateur ne capture que lui (`tableau`), les modificateurs sont avales. */
static bool service_type_is_array(const char* typeName) {
    if (typeName == NULL) return false;
    char t[64];
    int n = 0;
    for (const char* s = typeName; *s && n < 63; s++) {
        t[n++] = (char)tolower((unsigned char)*s);
    }
    t[n] = '\0';
    return strstr(t, "tableau") != NULL || strstr(t, "array") != NULL;
}

/* Un parametre de type structure attend un corps JSON. Le Content-Type entrant
 * arbitre : absent / vide = statu quo (on tente le parse). Sinon, seul le media
 * type `application/json` (ses parametres `; charset=...` ignores) ou un sous-type
 * a suffixe `+json` (ex `application/vnd.api+json`) compte comme JSON ; tout autre
 * type explicite est refuse en 415 sans tenter le parse. */
static bool service_ct_est_json(const char* ct) {
    if (ct == NULL) return true;
    while (*ct == ' ' || *ct == '\t') ct++;
    if (*ct == '\0') return true;
    /* Media type = jusqu'au ';' ou a un espace ; bornes de fin rognees. */
    const char* end = ct;
    while (*end != '\0' && *end != ';' && *end != ' ' && *end != '\t') end++;
    size_t n = (size_t)(end - ct);
    if (n == 16 && strncasecmp(ct, "application/json", 16) == 0) return true;
    if (n > 5 && strncasecmp(end - 5, "+json", 5) == 0) return true;
    return false;
}

/* Extrait la valeur d'un parametre de query dans une cible `/x?k=v&k2=v2`.
 * Aucun percent-decodage (les valeurs simples suffisent au contrat courant).
 * Retourne true si trouve, remplit out (borne). */
static bool service_query_get(const char* target, const char* key,
                              char* out, size_t outCap) {
    const char* q = strchr(target, '?');
    if (q == NULL) return false;
    q++;
    size_t klen = strlen(key);
    while (*q != '\0' && *q != '#') {
        const char* amp = q;
        while (*amp != '\0' && *amp != '&' && *amp != '#') amp++;
        /* Paire [q, amp) de la forme k=v. */
        if ((size_t)(amp - q) > klen && strncmp(q, key, klen) == 0
            && q[klen] == '=') {
            const char* v = q + klen + 1;
            size_t vlen = (size_t)(amp - v);
            if (vlen >= outCap) vlen = outCap - 1;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return true;
        }
        q = (*amp == '&') ? amp + 1 : amp;
    }
    return false;
}

/*  — decode base64 standard (RFC 4648, alphabet A-Z a-z 0-9 + /) dans
 * un tampon BORNE. Ce decodeur voit une valeur d'en-tete Authorization venue du
 * reseau : aucune tolerance silencieuse, aucune ecriture hors `out`. Retourne
 * true + `*outlen` si l'entree est un base64 bien forme ET tient dans `outcap`.
 * Toute anomalie (char hors alphabet — espaces et CR/LF inclus, char apres
 * remplissage '=', longueur invalide, padding surnumeraire, debordement de
 * `out`) -> false. La detection d'un octet NUL dans le decode est laissee a
 * l'appelant (via `*outlen`, jamais un strlen du decode hostile). */
static bool b64_decode_borne_ex(const char* in, size_t inlen,
                                unsigned char* out, size_t outcap, size_t* outlen,
                                bool url) {
    unsigned int acc = 0;
    int nbits = 0, ngroups = 0, npad = 0;
    size_t j = 0;
    for (size_t i = 0; i < inlen; i++) {
        char c = in[i];
        if (c == '=') { npad++; continue; }
        if (npad > 0) return false;          /* char apres remplissage '=' */
        int v;
        if      (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (!url && c == '+')     v = 62;
        else if (!url && c == '/')     v = 63;
        else if (url && c == '-')      v = 62;  /* base64url (RFC 4648 §5) */
        else if (url && c == '_')      v = 63;
        else return false;                   /* hors alphabet base64 */
        acc = (acc << 6) | (unsigned int)v;
        nbits += 6;
        ngroups++;
        if (nbits >= 8) {
            nbits -= 8;
            if (j >= outcap) return false;   /* decode trop long pour `out` */
            out[j++] = (unsigned char)((acc >> nbits) & 0xFF);
        }
    }
    if (npad > 2)                                    return false;
    if (ngroups % 4 == 1)                            return false;  /* longueur invalide */
    if (npad > 0 && (ngroups + npad) % 4 != 0)       return false;
    /* Padding '=' OPTIONNEL toleré : un base64 sans remplissage est accepté tant
     * que la longueur reste valide (ngroups % 4 != 1). Divergence assumée du RFC
     * 4648 strict (qui exige le padding) ; le décodage reste déterministe et sans
     * ambiguïté — les octets sont reconstruits identiquement avec ou sans '='. */
    *outlen = j;
    return true;
}

/* Décodeur base64 standard (RFC 4648, alphabet +/) — conserve la signature
 * historique pour ses appelants (Basique). */
static bool b64_decode_borne(const char* in, size_t inlen,
                             unsigned char* out, size_t outcap, size_t* outlen) {
    return b64_decode_borne_ex(in, inlen, out, outcap, outlen, false);
}

/*  /  — extrait le credential d'un en-tete Authorization
 * selon le schema, dans le tampon `out` (capacite `outcap`, toujours laisse
 * NUL-termine). Contrat « ecrit dans un tampon fourni » : Bearer y COPIE le
 * jeton, Basique y ecrit le "user:pass" DECODE (buffer distinct de l'entree).
 * Le credential par defaut est VIDE ; toute forme non reconnue le laisse tel.
 *
 * Bearer (RFC 6750) : mot-cle "Bearer" insensible a la casse, un espace, le
 *   reste EST le jeton. Jamais la valeur brute sans prefixe.
 * Basique (RFC 7617) : mot-cle "Basic " insensible a la casse (FR httpAuthBasique
 *   / EN httpAuthBasic — `authentication` porte la graphie source, non canon),
 *   le reste est base64-decode. Toute anomalie (prefixe absent, base64 malforme,
 *   decode debordant, chaine vide) -> credential VIDE (fail-closed, l'app rejette).
 *   SECURITE : un decode contenant un octet NUL est traite comme MALFORME
 *   (smuggling `admin\0:x`) -> VIDE. On ne strlen JAMAIS le decode hostile ;
 *   la longueur vient de `b64_decode_borne`. */
static void service_extrait_credential(const char* scheme,
                                       const char* autorisation,
                                       const char* cleApi,
                                       char* out, size_t outcap) {
    if (out == NULL || outcap == 0) return;
    out[0] = '\0';
    if (scheme == NULL) return;

    /*  — CléAPI : le credential est la valeur BRUTE de l'en-tete
     * X-API-Key (aucun decode, aucun prefixe). Source STRICTE et exclusive :
     * jamais `autorisation` (un Authorization: Bearer ne sert jamais de cle
     * API -- les sources d'authn ne se melangent pas). Absent/vide -> VIDE. */
    if (strcasecmp(scheme, "httpAuthCl\xc3\xa9""API") == 0 ||
        strcasecmp(scheme, "httpAuthCleAPI") == 0 ||
        strcasecmp(scheme, "httpAuthApiKey") == 0) {
        if (cleApi != NULL) {
            size_t n = strlen(cleApi);
            if (n < outcap) memcpy(out, cleApi, n + 1);   /* trop long -> reste vide */
        }
        return;
    }

    if (autorisation == NULL) return;

    if (strcasecmp(scheme, "httpAuthBearer") == 0) {
        if (strncasecmp(autorisation, "Bearer ", 7) == 0) {
            const char* tok = autorisation + 7;
            size_t n = strlen(tok);
            if (n < outcap) memcpy(out, tok, n + 1);   /* trop long -> reste vide */
        }
        return;
    }

    if (strcasecmp(scheme, "httpAuthBasique") == 0 ||
        strcasecmp(scheme, "httpAuthBasic")   == 0) {
        if (strncasecmp(autorisation, "Basic ", 6) != 0) return;
        const char* enc = autorisation + 6;
        size_t declen = 0;
        if (!b64_decode_borne(enc, strlen(enc),
                              (unsigned char*)out, outcap - 1, &declen)) {
            out[0] = '\0';
            return;
        }
        /* Garde NUL : un octet NUL avant `declen` = decode binaire hostile ->
         * malforme -> credential VIDE (jamais tronque au NUL par un strlen). */
        for (size_t i = 0; i < declen; i++) {
            if (out[i] == '\0') { out[0] = '\0'; return; }
        }
        out[declen] = '\0';
        return;
    }

    /* Schemas non reconnus : chantier aval -> credential VIDE. */
}

/*  — construit une reponse a partir de MoiMême..Réponse (statut / corps
 * / type poses par un hook). `statutDefaut` s'applique si aucun CodeÉtat n'a ete
 * pose : 401 fail-closed pour un rejet d'authentification (un hook qui oublie le
 * code ne laisse jamais passer), 500 pour le hook surErreur. `rep`
 * doit rester racine (cree_tampon COPIE avant tout relachement). */
static OwlHttpRep* service_rep_depuis_moimeme(ObjInstance* rep, int statutDefaut) {
    static const char* const nomsReponse[] = { "Reponse", "Response", NULL };
    static const char* const nomsStatut[]  = { "CodeEtat", "StatusCode", NULL };
    static const char* const nomsCorps[]   = { "Corps", "Body", NULL };
    static const char* const nomsType[]    = { "TypeContenu", "ContentType", NULL };

    int    statut = statutDefaut;
    char   type[128]; type[0] = '\0';
    const char* corps = "";
    size_t corps_len = 0;

    ObjInstance* subRep = service_lit_sous_instance(rep, nomsReponse);
    Value v;
    if (subRep != NULL && service_lit_champ(subRep, nomsStatut, &v) && IS_NUMBER(v)) {
        statut = (int)AS_NUMBER(v);
    }
    if (subRep != NULL && service_lit_champ(subRep, nomsType, &v) && IS_STRING(v)) {
        snprintf(type, sizeof(type), "%s", AS_CSTRING(v));
    }
    if (subRep != NULL && service_lit_champ(subRep, nomsCorps, &v)) {
        /* value_as_bytes unifie chaine ET Buffer : un hook surErreur/
         * surAuthentification peut poser un corps binaire brut sans troncature. */
        const unsigned char* b; int bl;
        if (value_as_bytes(v, &b, &bl)) {
            corps     = (const char*)b;
            corps_len = (size_t)bl;
        }
    }
    if (statut < 100 || statut > 599) statut = 500;
    if (type[0] == '\0') snprintf(type, sizeof(type), "%s", "text/plain");
    return owl_http_rep_cree_tampon(statut, type, corps, corps_len);
}

/*  — journalise la RAISON d'un rejet d'auth sur la sortie serveur
 * diagnostic (stderr, meme canal que la Trace d'un crash de handler), pour
 * l'operateur/dev. SECURITE : cette raison ne quitte JAMAIS le serveur -- elle
 * ne touche pas la reponse client (401 opaque). Format court prefixe, une ligne,
 * fflush pour lecture live en console. */
static void service_log_rejet_auth(const char* methode, const char* chemin,
                                   const char* raison) {
    fprintf(stderr, "owl serve: rejet auth %s %s : %s\n",
            methode ? methode : "?", chemin ? chemin : "?",
            raison ? raison : "raison inconnue");
    fflush(stderr);
}

/*  — validation JWT DECLARATIVE (RS256, fail-closed). Retourne true si
 * le jeton Bearer de `autorisation` est integralement valide :
 *   - en-tete `alg` EXACTEMENT "RS256" (rejette none/HS256/absent/autre : la
 *     confusion d'algo est LA faille JWT historique, verifiee EN PREMIER) ;
 *   - signature RS256 (PKCS#1 v1.5) verifiee par la CLE DE VERIFICATION sur
 *     SHA256 du signing input (header64 "." payload64, segments base64url) ;
 *   - `exp` present, entier, >= maintenant (jamais tolere absent) ;
 *   - `iss` present et egal a `issuer`.
 *  — la cle de verification provient de DEUX sources mutuellement
 * exclusives : `pubKeyPem` (cle statique PEM pinnee) OU `jwksUrl` (jeu de cles
 * JWKS recupere + cache, selectionne par le `kid` de l'en-tete). En mode JWKS,
 * un `kid` est EXIGE (absent -> rejet, le plus sur). Le pinning RS256 et tous
 * les controles de claims sont IDENTIQUES quelle que soit la source.
 * En succes, depose les claims (objet JSON) tels quels dans
 * MoiMême..Requête..Identité (subReq deja lie a `rep`, racine). Toute anomalie
 * (Bearer absent, pas 3 segments non vides, base64url hostile, JSON malforme,
 * kid/cle introuvable, signature/exp/iss invalides) -> false SANS rien modifier
 * d'observable : l'appelant repond 401. `rep` racine (event_sender) tout l'appel.
 *
 *  — `raison` (out-param, jamais NULL cote appelant) recoit une chaine
 * statique FR decrivant QUELLE branche a echoue. Destinee a la sortie serveur
 * diagnostic UNIQUEMENT (l'appelant la logge) ; elle ne touche JAMAIS la reponse
 * client (401 opaque). */
static bool service_jwt_valide(ObjInstance* rep, const char* autorisation,
                               const char* pubKeyPem, const char* jwksUrl,
                               const char* issuer, const char* audience,
                               const char** raison) {
    if (raison) *raison = "jeton invalide";
    if (autorisation == NULL || (pubKeyPem == NULL && jwksUrl == NULL)) {
        if (raison) *raison = (autorisation == NULL)
            ? "en-tête d'autorisation absent" : "configuration de clé absente";
        return false;
    }
    if (strncasecmp(autorisation, "Bearer ", 7) != 0) {
        if (raison) *raison = "schéma Bearer attendu";
        return false;
    }
    const char* tok = autorisation + 7;

    if (raison) *raison = "jeton malformé";

    /* Split header.payload.signature : exactement 3 segments NON VIDES. */
    const char* dot1 = strchr(tok, '.');
    if (dot1 == NULL || dot1 == tok) return false;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (dot2 == NULL || dot2 == dot1 + 1) return false;
    const char* sig64 = dot2 + 1;
    if (*sig64 == '\0') return false;              /* 3e segment vide (alg=none) */
    if (strchr(sig64, '.') != NULL) return false;  /* plus de 3 segments */

    size_t h64len = (size_t)(dot1 - tok);
    size_t p64len = (size_t)(dot2 - (dot1 + 1));
    size_t s64len = strlen(sig64);

    /* --- CHECK CRITIQUE (EN PREMIER) : alg == "RS256" exact. --- */
    unsigned char hdr[1024];
    size_t hdrlen = 0;
    if (!b64_decode_borne_ex(tok, h64len, hdr, sizeof(hdr) - 1, &hdrlen, true))
        return false;
    hdr[hdrlen] = '\0';
    ObjJson* hjson = json_parse_cstr((const char*)hdr, (int)hdrlen);
    if (hjson == NULL) return false;
    vm_push(OBJ_VAL(hjson));                         /* root avant alloc de la cle */
    bool algOk = false;
    char kid[256]; kid[0] = '\0';
    bool haveKid = false;
    {
        ObjString* algKey = obj_copy_string("alg", 3);
        vm_push(OBJ_VAL(algKey));
        ObjJson* algNode = obj_json_object_get(hjson, algKey);
        vm_pop();                                     /* algKey */
        if (algNode != NULL && algNode->jtype == JSON_T_STRING &&
            algNode->as.strVal != NULL &&
            algNode->as.strVal->length == 5 &&
            memcmp(algNode->as.strVal->chars, "RS256", 5) == 0) {
            algOk = true;
        }
        /*  — kid de l'en-tete (selecteur de cle en mode JWKS). Extrait
         * pendant que hjson est racine ; recopie dans un tampon C borne. */
        ObjString* kidKey = obj_copy_string("kid", 3);
        vm_push(OBJ_VAL(kidKey));
        ObjJson* kidNode = obj_json_object_get(hjson, kidKey);
        vm_pop();                                     /* kidKey */
        if (kidNode != NULL && kidNode->jtype == JSON_T_STRING &&
            kidNode->as.strVal != NULL) {
            size_t kl = (size_t)kidNode->as.strVal->length;
            if (kl > 0 && kl < sizeof(kid)) {
                memcpy(kid, kidNode->as.strVal->chars, kl);
                kid[kl] = '\0';
                haveKid = true;
            }
        }
    }
    vm_pop();                                         /* hjson */
    if (!algOk) {
        if (raison) *raison = "algorithme non supporté";
        return false;
    }

    /* --- Signature : SHA256(header64 "." payload64) verifie via la cle PEM. --- */
    unsigned char hash[32];
    if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                   (const unsigned char*)tok, (size_t)(dot2 - tok), hash) != 0) {
        if (raison) *raison = "erreur interne de vérification";
        return false;
    }

    unsigned char sig[512];
    size_t siglen = 0;
    if (!b64_decode_borne_ex(sig64, s64len, sig, sizeof(sig), &siglen, true)) {
        if (raison) *raison = "signature invalide";
        return false;
    }

    /* --- Cle de verification : PEM statique pinnee OU lookup JWKS par kid. --- */
    mbedtls_pk_context  pkLocal;            /* possede si mode PEM */
    mbedtls_pk_init(&pkLocal);
    mbedtls_pk_context* pkVerif = NULL;     /* cle a utiliser (locale OU cache) */
    const char* raisonCle = NULL;           /* raison specifique de l'etape cle */
    if (pubKeyPem != NULL) {
        if (mbedtls_pk_parse_public_key(&pkLocal, (const unsigned char*)pubKeyPem,
                                        strlen(pubKeyPem) + 1) == 0) {
            pkVerif = &pkLocal;
        } else {
            raisonCle = "clé publique invalide";
        }
    } else if (haveKid) {
        /* Mode JWKS : kid EXIGE. La cle rendue est POSSEDEE par le cache
         * (fail-closed sur injoignable / kid inconnu / kty non-RSA). */
        mbedtls_pk_context* cached = NULL;
        JwksResolution jr = owl_jwks_resolve(jwksUrl, kid, &cached);
        if (jr == JWKS_OK && cached != NULL) {
            pkVerif = cached;
        } else if (jr == JWKS_INJOIGNABLE) {
            raisonCle = "jwks injoignable";
        } else {
            raisonCle = "kid inconnu";
        }
    } else {
        /* Mode JWKS mais l'en-tete n'a pas de kid : rejet fail-closed. */
        raisonCle = "kid absent";
    }
    bool verifyOk = false;
    if (pkVerif != NULL) {
        if (mbedtls_pk_verify(pkVerif, MBEDTLS_MD_SHA256, hash, sizeof(hash),
                              sig, siglen) == 0) {
            verifyOk = true;
        } else {
            raisonCle = "signature invalide";
        }
    }
    mbedtls_pk_free(&pkLocal);              /* init'd meme si inutilise : free sur */
    if (!verifyOk) {
        if (raison) *raison = raisonCle ? raisonCle : "signature invalide";
        return false;
    }

    /* --- Claims : exp EXIGE + futur, iss EXIGE == issuer. --- */
    if (raison) *raison = "jeton malformé";
    unsigned char pl[8192];
    size_t pllen = 0;
    if (!b64_decode_borne_ex(dot1 + 1, p64len, pl, sizeof(pl) - 1, &pllen, true))
        return false;
    pl[pllen] = '\0';
    ObjJson* pjson = json_parse_cstr((const char*)pl, (int)pllen);
    if (pjson == NULL) return false;
    vm_push(OBJ_VAL(pjson));                          /* root avant toute alloc */

    static const char* const nomsRequete[] = { "Requete", "Request", NULL };
    bool claimsOk = false;
    {
        /* exp : present, entier, >= maintenant. */
        ObjString* expKey = obj_copy_string("exp", 3);
        vm_push(OBJ_VAL(expKey));
        ObjJson* expNode = obj_json_object_get(pjson, expKey);
        vm_pop();                                     /* expKey */
        bool expFutur = false;
        if (expNode != NULL) {
            long long expVal = 0;
            bool got = false;
            if (expNode->jtype == JSON_T_INT)  { expVal = (long long)expNode->as.intVal;  got = true; }
            else if (expNode->jtype == JSON_T_REAL) { expVal = (long long)expNode->as.realVal; got = true; }
            if (got && expVal >= (long long)time(NULL)) expFutur = true;
        }

        /* iss : present, chaine, == issuer configure. */
        bool issOk = false;
        if (issuer != NULL) {
            ObjString* issKey = obj_copy_string("iss", 3);
            vm_push(OBJ_VAL(issKey));
            ObjJson* issNode = obj_json_object_get(pjson, issKey);
            vm_pop();                                 /* issKey */
            if (issNode != NULL && issNode->jtype == JSON_T_STRING &&
                issNode->as.strVal != NULL) {
                size_t ilen = strlen(issuer);
                if ((size_t)issNode->as.strVal->length == ilen &&
                    memcmp(issNode->as.strVal->chars, issuer, ilen) == 0) {
                    issOk = true;
                }
            }
        }

        /*  — nbf (not-before, RFC 7519 4.1.5) : miroir d'exp, sans
         * config. Absent -> pas de rejet. Present -> doit etre un nombre <=
         * maintenant (present mais type inattendu -> fail-closed, comme exp). */
        bool nbfOk = true;
        {
            ObjString* nbfKey = obj_copy_string("nbf", 3);
            vm_push(OBJ_VAL(nbfKey));
            ObjJson* nbfNode = obj_json_object_get(pjson, nbfKey);
            vm_pop();                                 /* nbfKey */
            if (nbfNode != NULL) {
                long long nbfVal = 0;
                bool got = false;
                if (nbfNode->jtype == JSON_T_INT)  { nbfVal = (long long)nbfNode->as.intVal;  got = true; }
                else if (nbfNode->jtype == JSON_T_REAL) { nbfVal = (long long)nbfNode->as.realVal; got = true; }
                if (!got || nbfVal > (long long)time(NULL)) nbfOk = false;
            }
        }

        /*  — aud (audience, RFC 7519 4.1.3) : validee UNIQUEMENT si une
         * audience est configuree sur le service (opt-in). Le claim peut etre une
         * chaine (match si == audience) ou un tableau de chaines (match si
         * audience ∈ tableau). Absent-quand-exige, non-matching ou type inattendu
         * -> fail-closed (401). audience non configuree -> aud ignore. */
        bool audOk = true;
        if (audience != NULL) {
            audOk = false;                            /* exige : rejet par defaut */
            size_t alen = strlen(audience);
            ObjString* audKey = obj_copy_string("aud", 3);
            vm_push(OBJ_VAL(audKey));
            ObjJson* audNode = obj_json_object_get(pjson, audKey);
            vm_pop();                                 /* audKey */
            if (audNode != NULL && audNode->jtype == JSON_T_STRING &&
                audNode->as.strVal != NULL) {
                if ((size_t)audNode->as.strVal->length == alen &&
                    memcmp(audNode->as.strVal->chars, audience, alen) == 0) {
                    audOk = true;
                }
            } else if (audNode != NULL && audNode->jtype == JSON_T_ARRAY) {
                for (int ai = 0; ai < audNode->as.arrayVal.count; ai++) {
                    ObjJson* it = audNode->as.arrayVal.items[ai];
                    if (it != NULL && it->jtype == JSON_T_STRING &&
                        it->as.strVal != NULL &&
                        (size_t)it->as.strVal->length == alen &&
                        memcmp(it->as.strVal->chars, audience, alen) == 0) {
                        audOk = true;
                        break;
                    }
                }
            }
            /* audNode NULL (absent) ou autre type -> audOk reste false. */
        }

        claimsOk = expFutur && issOk && nbfOk && audOk;

        /* Raison discriminee, premier claim fautif dans l'ordre exp/iss/nbf/aud. */
        if (raison && !claimsOk) {
            if (!expFutur)      *raison = (expNode == NULL)
                                    ? "date d'expiration absente" : "jeton expiré";
            else if (!issOk)    *raison = "émetteur invalide";
            else if (!nbfOk)    *raison = "jeton pas encore valide";
            else                *raison = "audience invalide";
        }
    }

    if (claimsOk) {
        /* Depot de l'identite : claims (objet JSON) tels quels. subReq est deja
         * lie a `rep` (racine), pjson toujours sur la pile -> lien le sauve. */
        ObjInstance* subReq = service_lit_sous_instance(rep, nomsRequete);
        if (subReq != NULL) {
            inst_field_set_value(subReq, "Identite", OBJ_VAL(pjson));
        } else {
            claimsOk = false;   /* contexte absent : fail-closed */
            if (raison) *raison = "contexte de requête absent";
        }
    }
    vm_pop();                                         /* pjson */
    return claimsOk;
}

/* ================================================================
 *  — binding multipart/form-data serve-time
 * ================================================================ */

/* Liste des fichiers temporaires crees pendant le binding d'une requete, a
 * supprimer sur TOUS les chemins de sortie. Petite liste dynamique : le nombre
 * de parties fichier est borne par corps_max, pas par une constante
 * locale, d'ou l'absence de tableau a taille fixe. */
typedef struct {
    char** items;
    int    count;
    int    cap;
} MpTempList;

static void mp_temps_push(MpTempList* l, const char* path) {
    if (l->count >= l->cap) {
        int nc = l->cap == 0 ? 8 : l->cap * 2;
        char** ni = (char**)realloc(l->items, (size_t)nc * sizeof(char*));
        if (ni == NULL) return;   /* OOM : temp non trace (best-effort cleanup) */
        l->items = ni;
        l->cap   = nc;
    }
    size_t n = strlen(path);
    char* d = (char*)malloc(n + 1);
    if (d == NULL) return;
    memcpy(d, path, n + 1);
    l->items[l->count++] = d;
}

static void mp_temps_cleanup(MpTempList* l) {
    for (int i = 0; i < l->count; i++) {
        owl_multipart_remove_temp(l->items[i]);
        free(l->items[i]);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;
}

/* Cherche une partie par nom de champ. wantFile=1 exige une partie fichier
 * (filename present), wantFile=0 exige un champ texte. `skip` saute les N
 * premieres correspondances (iteration d'un tableau de fichiers). Comparaison
 * casse-insensible ASCII (noms de champ de formulaire). */
static const OwlMultipartPart* mp_find_part(const OwlMultipartForm* form,
                                            const char* name, int wantFile,
                                            int skip) {
    int seen = 0;
    for (int i = 0; i < form->count; i++) {
        const OwlMultipartPart* p = &form->parts[i];
        if (p->name == NULL) continue;
        if (strcasecmp(p->name, name) != 0) continue;
        int isFile = (p->filename != NULL);
        if (wantFile && !isFile) continue;
        if (!wantFile && isFile) continue;
        if (seen++ < skip) continue;
        return p;
    }
    return NULL;
}

/* Construit une instance FichierTéléversé depuis une partie (NULL => instance
 * vide) et la laisse AU SOMMET DE LA PILE (racine GC). L'appelant la consomme
 * comme argument scalaire ou l'insere dans un tableau deja racine, puis depile.
 * Le contenu est ecrit dans un fichier temp au nom GENERE (jamais derive du nom
 * client) ; son chemin va dans _cheminTemp et la liste de nettoyage. Discipline
 *  : l'instance est rootee AVANT les inst_field_set_* (alloc ObjString). */
static void mp_push_instance(const OwlMultipartPart* part, MpTempList* temps) {
    ObjString* nm = obj_copy_string("FichierTeleverse", 16);
    vm_push(OBJ_VAL(nm));                 /* root pendant obj_new_instance */
    ObjInstance* inst = obj_new_instance(nm);
    vm_pop();                             /* nm (l'instance en tient la reference) */
    vm_push(OBJ_VAL(inst));               /* root inst (aucune alloc entre pop/push) */

    const char* fname = (part && part->filename)    ? part->filename    : "";
    const char* ctype = (part && part->contentType) ? part->contentType : "";
    size_t dlen       = part ? part->dataLen : 0;

    inst_field_set_str(inst, "NomFichier", fname);
    inst_field_set_int(inst, "Taille",     (int64_t)dlen);
    inst_field_set_str(inst, "TypeMime",   ctype);

    char tmpPath[1024];
    if (part != NULL &&
        owl_multipart_write_temp(part->data, part->dataLen, tmpPath, sizeof tmpPath) == 0) {
        inst_field_set_str(inst, "_cheminTemp", tmpPath);
        mp_temps_push(temps, tmpPath);
    } else {
        inst_field_set_str(inst, "_cheminTemp", "");
    }
    /* inst reste au sommet de pile pour l'appelant. */
}

/*  — route de documentation servie par le serveur lui-meme :
 * GET /openapi.json rend le document OpenAPI 3 derive de l'AST du service
 * (meme generateur que la CLI `owl openapi`). Le registre compile-time est
 * peuple par la compilation faite sur CE thread executeur (prepare_vm). */
static int chemin_est_openapi(const char* chemin) {
    if (!chemin) return 0;
    const char* p = chemin;
    const char* cible = "/openapi.json";
    size_t n = strlen(cible);
    if (strncmp(p, cible, n) != 0) return 0;
    /* fin de chemin ou debut de query-string uniquement */
    return p[n] == '\0' || p[n] == '?';
}

static OwlHttpRep* execute_service_job(OwlHttpExecuteur* x, OwlHttpJeton* j) {
    /* GET /openapi.json : document OpenAPI, avant le routage metier. */
    if (chemin_est_openapi(j->chemin) && strcasecmp(j->methode, "GET") == 0) {
        if (owl_service_registry_count() >= 1) {
            char* doc = owl_openapi_generate(owl_service_registry_at(0));
            if (doc != NULL) {
                OwlHttpRep* rep = owl_http_rep_cree_tampon(
                    200, "application/json", doc, strlen(doc));
                free(doc);
                if (rep != NULL) return rep;
            }
        }
        return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
    }

    OwlServiceResolveResult res;
    owl_service_runtime_resolve(j->methode, j->chemin, &res);

    /* Route non resolue -> 404. C'est le CONTROLE NEGATIF : sans handler a
     * invoquer, rien ne prouve le cablage — un test doit rougir ici si la
     * resolution casse. */
    if (res.kind == OWL_SVC_RESOLVE_NONE) {
        return owl_http_rep_cree_tampon(404, "text/plain", NULL, 0);
    }
    /* Casse divergente (ASCII pur) -> 308 vers la forme canonique. Un desaccord
     * de casse ne peut plus contourner une regle en aval. */
    if (res.kind == OWL_SVC_RESOLVE_REDIRECT) {
        OwlHttpRep* red = owl_http_rep_cree_tampon(308, "text/plain", NULL, 0);
        owl_http_rep_ajoute_entete(red, "Location", res.canonical);
        return red;
    }

    const OwlServiceRuntimeRoute* route = res.route;

    if (x->cfg.gc_frontiere) gc_collect();

    /* Resout la fonction handler (nom synthetise exact) via la table globale. */
    ObjString* hkey = obj_copy_string(route->handlerGlobal,
                                      (int)strlen(route->handlerGlobal));
    vm_push(OBJ_VAL(hkey));
    Value fnVal;
    bool haveFn = table_get(&vm.globals, hkey, &fnVal);
    vm_pop();                                /* hkey */
    if (!haveFn) return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);

    /* Reponse implicite : ObjInstance mutee par le handler via MoiMême. Le nom
     * de structure est un label interne (aucun type registry), les champs sont
     * ecrits/lus par nom. */
    ObjString* sname = obj_copy_string("httpReponseService", 18);
    vm_push(OBJ_VAL(sname));                 /* root pendant obj_new_instance */
    ObjInstance* rep = obj_new_instance(sname);
    vm_event_sender_push(OBJ_VAL(rep));      /* root rep (aucune alloc depuis) + MoiMême */
    vm_pop();                                /* sname */

    /*  — contexte d'echange imbrique : MoiMême porte deux sous-objets
     * FRAIS et DISTINCTS, Réponse (ecriture : CodeÉtat/Corps/TypeContenu) et
     * Requête (lecture : Méthode/Chemin/Identité). Chacun est lie a `rep`
     * IMMEDIATEMENT apres allocation (aucune alloc intercalee), `rep` etant deja
     * racine (event_sender) : le lien le sauve d'un GC. Les sous-instances ne
     * referencent JAMAIS `rep` — aucun cycle possible (cf. ). Les cles
     * ASCII canon sont exactement celles que le compilateur emet en STRUCT_GET/
     * SET (service_ctx_*_canon), donc le round-trip runtime tape la bonne cle
     * quelle que soit la graphie source (FR/EN, casse, accent). */
    {
        ObjString* rpn = obj_copy_string("httpContexteReponse", 19);
        vm_push(OBJ_VAL(rpn));
        ObjInstance* subRep = obj_new_instance(rpn);
        vm_pop();                            /* rpn */
        inst_field_set_value(rep, "Reponse", OBJ_VAL(subRep));  /* lie + root */

        /*  — sous-champ Entetes de la Réponse : tableau associatif
         * FRAIS (cle chaine sans-casse, noms d'en-tete HTTP insensibles a la
         * casse). MoiMême..Réponse..Entêtes["Nom"] = "Valeur" tape un
         * OP_ARRAY_SET sur cet assoc (STRUCT_GET Entetes -> assoc, subscript
         * store). Racine via inst_field_set_value (subRep deja racine). */
        ObjAssocArray* repHdr = obj_new_assoc(ASSOC_KEY_STRING,
                                              ASSOC_OPT_SANS_CASSE,
                                              ASSOC_VALUE_CHAINE);
        inst_field_set_value(subRep, "Entetes", OBJ_VAL(repHdr));  /* lie + root */

        ObjString* rqn = obj_copy_string("httpContexteRequete", 19);
        vm_push(OBJ_VAL(rqn));
        ObjInstance* subReq = obj_new_instance(rqn);
        vm_pop();                            /* rqn */
        inst_field_set_value(rep, "Requete", OBJ_VAL(subReq));  /* lie + root */
        inst_field_set_str  (subReq, "Methode",  j->methode);
        inst_field_set_str  (subReq, "Chemin",   j->chemin);
        inst_field_set_value(subReq, "Identite", NIL_VAL);

        /*  — sous-champ Entetes de la Requête : tableau associatif
         * FRAIS (cle chaine sans-casse — noms d'en-tete HTTP insensibles a la
         * casse, RFC 7230), peuple depuis la copie possedee des en-tetes
         * entrants du job. MoiMême..Requête..Entêtes["Nom"] lit cet assoc
         * (STRUCT_GET Entetes -> assoc, subscript load). En-tete repete ->
         * valeurs jointes par ", " (RFC 7230 §3.2.2). LECTURE seule cote app ;
         * en-tete absent -> chaine vide (defaut du type assoc CHAINE). Racine
         * via inst_field_set_value (subReq deja racine). */
        ObjAssocArray* reqHdr = obj_new_assoc(ASSOC_KEY_STRING,
                                              ASSOC_OPT_SANS_CASSE,
                                              ASSOC_VALUE_CHAINE);
        inst_field_set_value(subReq, "Entetes", OBJ_VAL(reqHdr));  /* lie + root */
        for (size_t hi = 0; hi < j->nb_entetes; hi++) {
            if (!j->entetes[hi].nom) continue;
            const char* hnom = j->entetes[hi].nom;
            const char* hval = j->entetes[hi].valeur ? j->entetes[hi].valeur : "";
            ObjString* k = obj_copy_string(hnom, (int)strlen(hnom));
            vm_push(OBJ_VAL(k));                 /* root : allocs suivent */
            int exists = 0;
            Value prev = obj_assoc_get(reqHdr, k, &exists);
            ObjString* v;
            if (exists && IS_STRING(prev)) {
                /* En-tete deja present -> concatene "ancien, nouveau". `prev`
                 * reste valide (l'assoc est racine) ; on copie ses octets dans
                 * un tampon C AVANT toute alloc GC. */
                ObjString* p = AS_STRING(prev);
                size_t vl = strlen(hval);
                size_t nl = (size_t)p->length + 2 + vl;
                char* buf = (char*)malloc(nl + 1);
                if (!buf) { vm_pop(); continue; }
                memcpy(buf, p->chars, (size_t)p->length);
                memcpy(buf + p->length, ", ", 2);
                memcpy(buf + p->length + 2, hval, vl);
                buf[nl] = '\0';
                v = obj_copy_string(buf, (int)nl);
                free(buf);
            } else {
                v = obj_copy_string(hval, (int)strlen(hval));
            }
            vm_push(OBJ_VAL(v));                 /* root : obj_assoc_set alloue */
            obj_assoc_set(reqHdr, k, OBJ_VAL(v));
            vm_pop();                            /* v */
            vm_pop();                            /* k */
        }
    }

    /* ---  : validation JWT DECLARATIVE --- Inseree APRES le pre-seed
     * Requête/Réponse et AVANT le bloc hook. Gardee par le meme !authOptOut que
     * les familles 7/12 (httpAuthAucune saute aussi la validation JWT) + schema
     * httpAuthJWT + cle publique configuree. Le framework valide seul (signature
     * RS256 / exp / iss) et depose l'identite -- ZERO code app. Fail-closed 401
     * sur TOUT echec, jamais de pass-through. En succes, le bloc hook ci-dessous
     * est naturellement saute (un service JWT n'a pas de surAuthentification, la
     * combinaison etant une erreur compile) ; surAutorisation, lui, tourne apres
     * et lit l'identite JWT (composition declarative). */
    if (!route->authOptOut && route->authScheme != NULL &&
        (route->jwtPublicKeyPem != NULL || route->jwtJwksUrl != NULL) &&
        strcasecmp(route->authScheme, "httpAuthJWT") == 0) {
        const char* raisonJwt = NULL;
        if (!service_jwt_valide(rep, j->autorisation,
                                route->jwtPublicKeyPem, route->jwtJwksUrl,
                                route->jwtIssuer, route->jwtAudience, &raisonJwt)) {
            service_log_rejet_auth(j->methode, j->chemin, raisonJwt);
            OwlHttpRep* out = service_rep_depuis_moimeme(rep, 401);
            vm_event_sender_pop();           /* unroot rep */
            if (x->cfg.gc_frontiere) gc_collect();
            return out;
        }
    }

    /* ---  : authentification (lot 1) --- Insere APRES le pre-seed
     * Requête/Réponse et AVANT le binding : le hook lit/pose son identite et son
     * code sur le MEME MoiMême que surRequête (rep deja racine). Saute si
     * l'operation opte hors auth (httpAuthAucune) OU si le service n'a ni schema
     * ni hook. Sinon : extrait le credential, invoque surAuthentification, et
     * court-circuite sur rejet (fail-closed 401 par defaut). */
    if (!route->authOptOut && route->authScheme != NULL &&
        route->authHandlerGlobal != NULL) {
        /* Resout le hook (nom global synthetise). Absent -> 500 fail-closed.
         * Resolu AVANT l'extraction du credential : ainsi aucune early-return
         * ne traverse la duree de vie du tampon `cred` alloue plus bas. */
        ObjString* akey = obj_copy_string(route->authHandlerGlobal,
                                          (int)strlen(route->authHandlerGlobal));
        vm_push(OBJ_VAL(akey));
        Value authFn;
        bool haveAuth = table_get(&vm.globals, akey, &authFn);
        vm_pop();                            /* akey */
        if (!haveAuth) {
            vm_event_sender_pop();           /* unroot rep */
            return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        }

        /* Extrait le credential dans un tampon dimensionne a la borne haute
         * stricte de l'en-tete PERTINENT selon le schema : Authorization pour
         * Bearer/Basique (Bearer = en-tete moins "Bearer " ; Basique decode
         * <= 3/4 du base64 <= en-tete), X-API-Key pour CléAPI (valeur brute).
         * On dimensionne au MAX des deux longueurs (null-safe) : sans cela une
         * cle API longue avec Authorization absent deborderait (regression
         *  = cap Bearer trop etroit pour CléAPI). AUCUN cap arbitraire
         * sous la limite serveur (en-tetes jusqu'a 16 Ko) : une cle / un jeton
         * de plusieurs Ko passe ENTIER. `cred` ne vit que jusqu'a la creation
         * de son ObjString, libere aussitot — aucune early-return (rejet 401,
         * crash-hook 500) ne traverse sa duree de vie. */
        size_t lenAuth = j->autorisation ? strlen(j->autorisation) : 0;
        size_t lenCle  = j->cleApi ? strlen(j->cleApi) : 0;
        size_t credCap = (lenAuth > lenCle ? lenAuth : lenCle) + 1;
        char* cred = (char*)malloc(credCap);
        if (cred == NULL) {
            vm_event_sender_pop();           /* unroot rep */
            return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        }
        service_extrait_credential(route->authScheme, j->autorisation,
                                   j->cleApi, cred, credCap);

        Value* authSavedTop = vm.stackTop;   /* frontiere de depilage propre */
        vm_push(authFn);
        ObjString* credStr = obj_copy_string(cred, (int)strlen(cred));
        free(cred);                          /* duree de vie close : ObjString en main */
        vm_push(OBJ_VAL(credStr));           /* push immediat (racine GC) */

        InterpretResult ar = vm_call_reentrant(authFn, 1);
        /* Capture le booleen AVANT de rejeter le residu de pile. */
        Value authRet = NIL_VAL;
        if (ar == INTERPRET_OK && vm.stackTop > authSavedTop) {
            authRet = vm.stackTop[-1];
        }
        vm.stackTop = authSavedTop;

        if (ar != INTERPRET_OK) {
            /* Crash du hook -> 500, JAMAIS pass-through (fail-closed sur erreur). */
            vm_event_sender_pop();           /* unroot rep */
            return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        }

        bool authOk = IS_BOOL(authRet) && AS_BOOL(authRet);
        if (!authOk) {
            /* Rejet : court-circuit AVANT binding + surRequête. Reponse = ce que
             * le hook a pose sur MoiMême..Réponse, defaut fail-closed 401. Le hook
             * a deja tranche : on logge la raison generique cote serveur. */
            service_log_rejet_auth(j->methode, j->chemin,
                                   "authentification refusée par le hook");
            OwlHttpRep* out = service_rep_depuis_moimeme(rep, 401);
            vm_event_sender_pop();           /* unroot rep */
            if (x->cfg.gc_frontiere) gc_collect();
            return out;
        }
        /* authOk -> l'identite posee par le hook (MoiMême..Requête..Identité)
         * survit pour surRequête : meme rep, toujours racine. */
    }

    /* ---  : autorisation (lot 4) --- Insere APRES l'authn (l'identite
     * est deja deposee sur MoiMême..Requête, ou Null si aucun schema) et AVANT
     * le binding. Le hook (0 argument) lit MoiMême..Requête..Identité/Méthode/
     * Chemin et RENVOYER Vrai (autorise) / Faux (interdit). Le garde
     * !authOptOut couvre l'opt-out httpAuthAucune (skip authn ET authz : une
     * operation publique n'est ni authentifiee ni autorisee). Si l'authn a
     * rejete, elle a deja return 401 plus haut -> authz jamais atteinte. */
    if (!route->authOptOut && route->authzHandlerGlobal != NULL) {
        /* Resout le hook (nom global synthetise). Absent -> 500 fail-closed. */
        ObjString* zkey = obj_copy_string(route->authzHandlerGlobal,
                                          (int)strlen(route->authzHandlerGlobal));
        vm_push(OBJ_VAL(zkey));
        Value authzFn;
        bool haveAuthz = table_get(&vm.globals, zkey, &authzFn);
        vm_pop();                            /* zkey */
        if (!haveAuthz) {
            vm_event_sender_pop();           /* unroot rep */
            return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        }

        Value* authzSavedTop = vm.stackTop;  /* frontiere de depilage propre */
        vm_push(authzFn);
        InterpretResult zr = vm_call_reentrant(authzFn, 0);
        /* Capture le booleen AVANT de rejeter le residu de pile. */
        Value authzRet = NIL_VAL;
        if (zr == INTERPRET_OK && vm.stackTop > authzSavedTop) {
            authzRet = vm.stackTop[-1];
        }
        vm.stackTop = authzSavedTop;

        if (zr != INTERPRET_OK) {
            /* Crash du hook -> 500, JAMAIS pass-through (fail-closed sur erreur). */
            vm_event_sender_pop();           /* unroot rep */
            return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        }

        bool authzOk = IS_BOOL(authzRet) && AS_BOOL(authzRet);
        if (!authzOk) {
            /* Rejet : court-circuit AVANT binding + surRequête. Reponse = ce que
             * le hook a pose sur MoiMême..Réponse, defaut fail-closed 403
             * httpInterdit. */
            OwlHttpRep* out = service_rep_depuis_moimeme(rep, 403);
            vm_event_sender_pop();           /* unroot rep */
            if (x->cfg.gc_frontiere) gc_collect();
            return out;
        }
        /* authzOk -> continue vers le binding + surRequête, meme rep racine. */
    }

    /* --- Liaison des parametres : chemin / query / corps -> arguments --- */
    int pc = route->paramCount;
    if (pc > OWL_SVC_MAX_SEG) pc = OWL_SVC_MAX_SEG;

    /*  — mode multipart : au moins un parametre FichierTéléversé. Le
     * corps est parse UNE fois ; les champs texte frères sont lies par nom aux
     * parties non-fichier, les parametres fichier deviennent des instances
     * FichierTéléversé (contenu streame vers un fichier temp au nom genere). Un
     * corps malforme -> 400 avant toute invocation. Les fichiers temp crees sont
     * nettoyes sur TOUS les chemins de sortie (mp_temps). */
    int mpMode = 0;
    for (int i = 0; i < pc; i++) {
        if (route->params[i].fileUpload != OWL_SVC_UPLOAD_NONE) { mpMode = 1; break; }
    }
    OwlMultipartForm mpForm; mpForm.parts = NULL; mpForm.count = 0;
    MpTempList mpTemps; mpTemps.items = NULL; mpTemps.count = 0; mpTemps.cap = 0;
    if (mpMode) {
        if (owl_multipart_parse(j->contentType, j->corps, j->corps_len, &mpForm) != 0) {
            owl_multipart_free(&mpForm);
            vm_event_sender_pop();           /* unroot rep */
            if (x->cfg.gc_frontiere) gc_collect();
            return owl_http_rep_cree_tampon(400, "text/plain", NULL, 0);
        }
    }

    /* Passe A (sans allocation) : presence de chaque parametre, puis arite
     * effective en retirant les optionnels-query manquants EN QUEUE (la VM
     * comble le defaut d'un optionnel trailing via function->defaults). */
    int present[OWL_SVC_MAX_SEG];
    for (int i = 0; i < pc; i++) {
        const OwlServiceRuntimeParam* p = &route->params[i];
        if (p->source == OWL_SVC_SRC_QUERY) {
            char tmp[256];
            bool found = service_query_get(j->chemin, p->name, tmp, sizeof(tmp));
            present[i] = (found || !p->hasDefault) ? 1 : 0;
        } else {
            present[i] = 1;   /* chemin / corps : toujours fournis */
        }
    }
    int argCount = pc;
    while (argCount > 0 && !present[argCount - 1]) argCount--;

    /* Passe B (allocation + push) : fn puis chaque argument, chacun push
     * immediatement (racine GC). Le corps malforme -> 400 avant invocation. */
    Value* svcSavedTop = vm.stackTop;        /* frontiere de depilage propre */
    vm_push(fnVal);
    for (int i = 0; i < argCount; i++) {
        const OwlServiceRuntimeParam* p = &route->params[i];
        if (!present[i]) { vm_push(NIL_VAL); continue; }

        if (p->fileUpload == OWL_SVC_UPLOAD_SCALAR) {
            /* Une instance FichierTéléversé, la 1re partie fichier de ce nom
             * (instance vide si aucune). mp_push_instance la laisse en argument. */
            mp_push_instance(mp_find_part(&mpForm, p->name, 1, 0), &mpTemps);
        } else if (p->fileUpload == OWL_SVC_UPLOAD_ARRAY) {
            /* Un tableau de FichierTéléversé : toutes les parties fichier de ce
             * nom. Le tableau frais est racine (vm_push) AVANT de construire ses
             * elements ; il reste comme argument. */
            ObjArray* arr = obj_new_array();
            vm_push(OBJ_VAL(arr));
            int skip = 0;
            const OwlMultipartPart* part;
            while ((part = mp_find_part(&mpForm, p->name, 1, skip)) != NULL) {
                mp_push_instance(part, &mpTemps);      /* instance au sommet */
                obj_array_push(arr, vm.stackTop[-1]);  /* arr racine tient l'element */
                vm_pop();                              /* instance (detenue par arr) */
                skip++;
            }
            /* arr reste au sommet comme argument. */
        } else if (p->source == OWL_SVC_SRC_PATH) {
            const char* text = (p->pathIndex >= 0 && p->pathIndex < OWL_SVC_MAX_SEG)
                               ? res.pathValues[p->pathIndex] : "";
            int isCatchAll = (p->pathIndex >= 0 && p->pathIndex < route->segCount
                              && route->segIsParam != NULL
                              && route->segIsParam[p->pathIndex] == 2);
            if (isCatchAll && service_type_is_array(p->typeName)) {
                /*  — catch-all lie a un `tableau de chaine` : la queue
                 * jointe (`a/b/c.txt`) est RE-SEGMENTEE en un tableau de chaines
                 * (["a","b","c.txt"]). Le tableau frais est racine (vm_push)
                 * AVANT toute alloc d'element ; il reste l'argument. */
                ObjArray* arr = obj_new_array();
                vm_push(OBJ_VAL(arr));
                const char* s = text;
                while (*s != '\0') {
                    const char* start = s;
                    while (*s != '\0' && *s != '/') s++;
                    ObjString* seg = obj_copy_string(start, (int)(s - start));
                    vm_push(OBJ_VAL(seg));              /* racine avant push array */
                    obj_array_push(arr, vm.stackTop[-1]);
                    vm_pop();                            /* seg (detenu par arr) */
                    if (*s == '/') s++;
                }
                /* arr reste au sommet comme argument. */
            } else {
                /* Catch-all `chaine` ou parametre de chemin ordinaire : la
                 * valeur (queue jointe ou segment unique) coercee au type. */
                vm_push(service_coerce_scalar(text, p->typeName));
            }
        } else if (p->source == OWL_SVC_SRC_QUERY) {
            /* En multipart, un scalaire est un champ texte du formulaire (lie par
             * nom a une partie non-fichier) ; sinon, un parametre de query. */
            if (mpMode) {
                const OwlMultipartPart* tp = mp_find_part(&mpForm, p->name, 0, 0);
                if (tp != NULL) {
                    char buf[1024];
                    size_t n = tp->dataLen < sizeof(buf) - 1 ? tp->dataLen : sizeof(buf) - 1;
                    if (n > 0) memcpy(buf, tp->data, n);
                    buf[n] = '\0';
                    vm_push(service_coerce_scalar(buf, p->typeName));
                    continue;
                }
            }
            char tmp[256];
            if (service_query_get(j->chemin, p->name, tmp, sizeof(tmp))) {
                vm_push(service_coerce_scalar(tmp, p->typeName));
            } else {
                vm_push(service_coerce_scalar("", p->typeName));
            }
        } else { /* OWL_SVC_SRC_BODY */
            const char* tn = p->typeName ? p->typeName : "?";
            /* Negociation Content-Type : un corps structure attend du JSON. Un
             * type explicite non-JSON -> 415 SANS parse, avec un message clair
             * (plus de rejet muet). Content-Type absent/application/json ->
             * statu quo (parse ci-dessous). */
            if (!service_ct_est_json(j->contentType)) {
                vm.stackTop = svcSavedTop;
                vm_event_sender_pop();       /* unroot rep */
                owl_multipart_free(&mpForm);
                mp_temps_cleanup(&mpTemps);
                char msg[320];
                int mlen = snprintf(msg, sizeof(msg),
                    "Unsupported Content-Type. This endpoint expects a JSON "
                    "request body (type %s).", tn);
                if (mlen < 0) mlen = 0;
                if ((size_t)mlen >= sizeof(msg)) mlen = (int)sizeof(msg) - 1;
                return owl_http_rep_cree_tampon(415, "text/plain", msg,
                                                (size_t)mlen);
            }
            Value bodyVal;
            int rc = owl_json_bind_body(p->typeName, j->corps, j->corps_len,
                                        &bodyVal);
            if (rc == 1 || rc == 2) {
                /* rc==1 corps malforme -> 400 (handler jamais invoque) ;
                 * rc==2 type introuvable -> 500. Depile fn + args pousses. */
                vm.stackTop = svcSavedTop;
                vm_event_sender_pop();       /* unroot rep */
                owl_multipart_free(&mpForm);
                mp_temps_cleanup(&mpTemps);
                if (rc == 1) {
                    char msg[320];
                    int mlen = snprintf(msg, sizeof(msg),
                        "Request body is not valid JSON (type %s expected).", tn);
                    if (mlen < 0) mlen = 0;
                    if ((size_t)mlen >= sizeof(msg)) mlen = (int)sizeof(msg) - 1;
                    return owl_http_rep_cree_tampon(400, "text/plain", msg,
                                                    (size_t)mlen);
                }
                return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
            }
            vm_push(bodyVal);
        }
    }

    /*  P3 — 3e mode d'execution : mode FLUX. Quand la route porte Flux
     * (fluxSSE/fluxOctets), on arme le ProdCtx ambiant AVANT l'appel du handler.
     * L'ouverture du flux est PARESSEUSE (1er FluxÉcrit, owl_http_prod_flux_assure) :
     * ici on ne pose que le mode de framing + le slot ambiant (owl_vm_flux_courant_
     * set). Le routing/binding/auth/JWT/MoiMême ci-dessus ont tourne IDENTIQUES au
     * mode bufferise. ctx est une variable de PILE C : elle survit au park/restore
     * du handler (le thread reste bloque dans owl_http_rep_flux_pousse, sa frame C
     * reste vive) ; le slot ambiant, lui, est sauve/restore par la VM (P2) -> pas
     * de croisement entre deux flux concurrents. jeton_courant vaut encore j a
     * l'ouverture (avant le 1er push donc avant tout yield). */
    OwlHttpProdCtx fluxCtx;
    const bool routeFlux = (route->flux != NULL);
    if (routeFlux) {
        memset(&fluxCtx, 0, sizeof(fluxCtx));
        fluxCtx.x = x;
        int fmode = (strcasecmp(route->flux, "fluxOctets") == 0 ||
                     strcasecmp(route->flux, "streamBytes") == 0)
                        ? OWL_HTTP_FLUX_MODE_OCTETS : OWL_HTTP_FLUX_MODE_SSE;
        owl_http_prod_flux_mode_set(&fluxCtx, fmode);
        /* P5 — borne applicative de duree : route->delaiMax est en SECONDES
         * (0 = illimite), le socle raisonne en ms. Estampillee sur le flux a son
         * ouverture (owl_http_prod_ouvre_flux), le transport arme alors son timer. */
        owl_http_prod_flux_delai_max_set(&fluxCtx,
            route->delaiMax > 0 ? route->delaiMax * 1000 : 0);
        owl_vm_flux_courant_set(&fluxCtx);
    }

    /*  : arme la capture du contexte d'exception pour un crash NON
     * rattrapé du handler. Sans ça, le message/ligne/pile du crash partent sur
     * stderr puis l'unwind les efface, et ExceptionInfo() dans surErreur renvoie
     * vide. Désarmé juste après (le drapeau ne franchit pas cette frontière). */
    vm_set_capture_uncaught(true);
    InterpretResult r = vm_call_reentrant(fnVal, argCount);
    vm_set_capture_uncaught(false);
    /*  P3 — bord symetrique du slot ambiant (comme jeton_courant) : des
     * le retour du handler, plus aucun FluxÉcrit ne doit viser ce ctx de pile. */
    if (routeFlux) owl_vm_flux_courant_set(NULL);
    /*  : la valeur RENVOYER-ee est le corps par defaut. On la capture
     * AVANT de rejeter le residu de pile ; un appel OK laisse exactement une
     * valeur (le retour) au sommet, au-dessus de svcSavedTop. */
    Value retVal = NIL_VAL;
    if (r == INTERPRET_OK && vm.stackTop > svcSavedTop) {
        retVal = vm.stackTop[-1];
    }
    vm.stackTop = svcSavedTop;

    /*  P3 — le handler a-t-il OUVERT le flux (>= 1 FluxÉcrit) ? Si oui,
     * le transport a deja pris le relais (j->flux_demarre = 1, corps incremental,
     * statut FIGE au 1er chunk,  §5) : on NE synthetise JAMAIS de 500 ni
     * de surErreur -- on termine (retour OK) ou on avorte (crash handler / client
     * parti), on relache la ref producteur (borrowed, l'autre est au transport),
     * et on rend NULL -> l'epilogue flux de traite_une prend le relais. Flux
     * JAMAIS ouvert -> chemin bufferise ci-dessous INCHANGE (surErreur -> 500
     * compris). C'est ici que surErreur/capture_uncaught sont NEUTRALISES en
     * flux. */
    if (routeFlux) {
        OwlHttpRep* flux = owl_http_prod_flux_courant(&fluxCtx);
        if (flux != NULL) {
            /* Un crash a pu armer le contexte d'exception (capture_uncaught) : on
             * le vide pour ne pas le fuiter (stale) vers la requete suivante. */
            vm_exception_context_reset();
            if (r == INTERPRET_OK) owl_http_rep_flux_termine(flux);
            else                   owl_http_rep_flux_avorte(flux);
            owl_http_rep_flux_relache(flux);   /* ref producteur */
            vm_event_sender_pop();             /* unroot rep */
            owl_multipart_free(&mpForm);
            mp_temps_cleanup(&mpTemps);
            if (x->cfg.gc_frontiere) gc_collect();
            return NULL;
        }
        /* Flux jamais ouvert : poursuite du chemin bufferise ci-dessous. */
    }

    if (r != INTERPRET_OK) {
        /*  — surErreur sur CRASH DU HANDLER : si le service declare un
         * hook, on l'invoque sur le MEME MoiMême (rep encore event_sender). Il lit
         * MoiMême..Requête (semee au pre-seed) et personnalise MoiMême..Réponse.
         * La reponse finale se batit depuis MoiMême..Réponse, defaut 500. La VM
         * est reutilisable : vm_call_reentrant reset l'etat fatal a la frontiere
         * (comme pour la requete suivante). surErreur qui plante lui-meme -> 500
         * nu (pas de recursion, pas de re-invocation). */
        OwlHttpRep* out = NULL;
        if (route->errorHandlerGlobal != NULL) {
            ObjString* ekey = obj_copy_string(route->errorHandlerGlobal,
                                              (int)strlen(route->errorHandlerGlobal));
            vm_push(OBJ_VAL(ekey));
            Value errFn;
            bool haveErr = table_get(&vm.globals, ekey, &errFn);
            vm_pop();                        /* ekey */
            if (haveErr) {
                Value* errSavedTop = vm.stackTop;
                vm_push(errFn);
                /* Le contexte d'exception du crash (capturé ci-dessus via
                 * g_capture_uncaught) est vivant : surErreur le lit par
                 * ExceptionInfo(). */
                InterpretResult er = vm_call_reentrant(errFn, 0);
                vm.stackTop = errSavedTop;
                if (er == INTERPRET_OK) {
                    out = service_rep_depuis_moimeme(rep, 500);
                }
            }
        }
        /*  : nettoie le contexte du crash pour qu'il ne fuite pas
         * (stale) vers la requête suivante, que surErreur ait tourné ou non. */
        vm_exception_context_reset();
        if (out == NULL) out = owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        vm_event_sender_pop();
        owl_multipart_free(&mpForm);
        mp_temps_cleanup(&mpTemps);
        if (x->cfg.gc_frontiere) gc_collect();
        return out;
    }

    /* retVal a quitte la pile : on la re-enracine explicitement, la
     * serialisation JSON (et le GC de frontiere) pouvant survenir avant que
     * cree_tampon n'en COPIE le contenu. */
    vm_push(retVal);

    static const char* const nomsReponse[] = { "Reponse", "Response", NULL };
    static const char* const nomsStatut[] = { "CodeEtat", "StatusCode", NULL };
    static const char* const nomsCorps[]  = { "Corps", "Body", NULL };
    static const char* const nomsType[]   = { "TypeContenu", "ContentType", NULL };

    /*  — le statut par defaut du chemin succes est le CodeÉtat declare
     * de l'operation (resolu en entier a la compilation), 200 a defaut. Un
     * MoiMême..Réponse..CodeÉtat pose par le handler l'override juste apres. */
    int    statut = route->defaultStatus ? route->defaultStatus : 200;
    char   type[128]; type[0] = '\0';
    const char* corps = "";
    size_t corps_len = 0;
    char*  json_buf = NULL;                  /* possede : libere en fin */
    const char* type_deduit = "text/plain";  /* defaut si rien n'est deduit */

    /*  — statut/corps/type se lisent desormais dans la sous-instance
     * Réponse (semee plus haut), plus sur `rep` directement. */
    ObjInstance* subRep = service_lit_sous_instance(rep, nomsReponse);

    Value v;
    bool corpsPose = false;
    if (subRep != NULL && service_lit_champ(subRep, nomsStatut, &v) && IS_NUMBER(v)) {
        statut = (int)AS_NUMBER(v);
    }
    if (subRep != NULL && service_lit_champ(subRep, nomsType, &v) && IS_STRING(v)) {
        snprintf(type, sizeof(type), "%s", AS_CSTRING(v));
    }
    if (subRep != NULL && service_lit_champ(subRep, nomsCorps, &v)) {
        /* MoiMême..Réponse..Corps pose -> override explicite, gagne sur le retour.
         * value_as_bytes unifie chaine ET Buffer en octets bruts : un corps binaire
         * (octets non-UTF-8) traverse intact, la ou AS_CSTRING l'aurait tronque au
         * premier octet nul / mange par validation UTF-8. Un Buffer explicitement
         * pose (meme vide, bl==0) reste un corps voulu -> corpsPose. */
        const unsigned char* b; int bl;
        if (value_as_bytes(v, &b, &bl)) {
            corps     = (const char*)b;
            corps_len = (size_t)bl;
            corpsPose = true;
        }
    }
    if (!corpsPose && IS_STRING(retVal)) {
        /* RENVOYER chaine -> corps brut, text/plain par defaut. */
        corps     = AS_CSTRING(retVal);
        corps_len = (size_t)AS_STRING(retVal)->length;
    } else if (!corpsPose && IS_BUFFER(retVal)) {
        /* RENVOYER Buffer -> corps binaire brut, type neutre par defaut. Doit
         * passer AVANT le fallback JSON (un Buffer serait sinon serialise en null).
         * MoiMême..Réponse..TypeContenu pose par le handler gagne en aval (type[0]). */
        const unsigned char* b; int bl;
        if (value_as_bytes(retVal, &b, &bl)) {
            corps       = (const char*)b;
            corps_len   = (size_t)bl;
            type_deduit = "application/octet-stream";
        }
    } else if (!corpsPose && !IS_NIL(retVal)) {
        /* RENVOYER structure / autre valeur -> JSON, application/json. */
        size_t jlen = 0;
        json_buf = owl_json_serialize_value(retVal, &jlen);
        if (json_buf != NULL) {
            corps       = json_buf;
            corps_len   = jlen;
            type_deduit = "application/json";
        }
    }
    /* RENVOYER Null et aucun Corps -> corps vide (defauts ci-dessus). */

    if (statut < 100 || statut > 599) statut = 500;
    if (type[0] == '\0') snprintf(type, sizeof(type), "%s", type_deduit);

    /* cree_tampon COPIE : plus rien ne pointe dans le tas OWL apres. On peut
     * alors depiler rep + retVal (unroot) et forcer le GC de frontiere. */
    OwlHttpRep* out = owl_http_rep_cree_tampon(statut, type, corps, corps_len);
    free(json_buf);

    /*  — emission des en-tetes de reponse custom poses par le handler
     * via MoiMême..Réponse..Entêtes["Nom"] = "Valeur". On itere l'assoc frais
     * semee au pre-seed et on passe CHAQUE paire par owl_http_rep_ajoute_entete,
     * point de passage UNIQUE qui valide RFC 9110 (refus CR/LF -> pas d'injection
     * de scission de ligne) et cape a OWL_HTTP_REP_ENTETES_MAX. Une valeur
     * invalide renvoie un code d'echec IGNORE : l'en-tete est SKIPPE, la reponse
     * reste 200. Content-Type est SKIPPE (canal dedie MoiMême..Réponse..TypeContenu
     * = source unique ; Content-Length est calcule par le framework). Voie succes
     * seule (le rejet auth ne passe pas ici). subRep reste racine (via rep). */
    static const char* const nomsEntetes[] = { "Entetes", "Headers", NULL };
    Value hv;
    if (subRep != NULL && service_lit_champ(subRep, nomsEntetes, &hv)
        && IS_ASSOC_ARRAY(hv)) {
        ObjAssocArray* hdr = AS_ASSOC_ARRAY(hv);
        for (int i = 0; i < hdr->count; i++) {
            ObjString* k = hdr->keys[i];
            if (k == NULL) continue;
            if (strcasecmp(k->chars, "Content-Type") == 0) continue;  /* canal dedie */
            Value vv = hdr->values[i];
            /* Un header HTTP est textuel : une valeur SCALAIRE non-chaine
             * (entier, reel, booleen, monetaire, numeric bignum) est coercee
             * en sa chaine canonique — comme la query / les params.
             * value_to_string alloue une ObjString fraiche ; owl_http_rep_
             * ajoute_entete la COPIE immediatement (aucune alloc OWL entre
             * les deux -> pas de fenetre GC). Une valeur NON coercible (Null,
             * tableau, instance) est SKIPPEE proprement, jamais emise. */
            const char* sval;
            if (IS_STRING(vv)) {
                sval = AS_CSTRING(vv);
            } else if (IS_INTEGER(vv) || IS_REAL(vv) || IS_BOOL(vv)
                       || IS_CURRENCY(vv) || IS_NUMERIC(vv)) {
                sval = value_to_string(vv)->chars;
            } else {
                continue;  /* Null / tableau / instance : non coercible */
            }
            owl_http_rep_ajoute_entete(out, k->chars, sval);  /* code ignore */
        }
    }

    vm_pop();                                /* retVal */
    vm_event_sender_pop();                   /* unroot rep */
    owl_multipart_free(&mpForm);
    mp_temps_cleanup(&mpTemps);
    if (x->cfg.gc_frontiere) gc_collect();

    return out;
}

static OwlHttpRep* execute_job(OwlHttpExecuteur* x, OwlHttpJeton* j) {
    /* Mode C : le producteur est appele DANS ce thread VM. Il
     * ouvre le flux (owl_http_prod_ouvre_flux, qui le publie et reveille le
     * transport) puis pousse des octets copies. Le flux est livre via
     * j->rep — on ne rend donc rien ici (NULL). */
    if (x->cfg.producteur) {
        if (x->cfg.gc_frontiere) gc_collect();

        OwlHttpRequete req;
        memset(&req, 0, sizeof(req));
        snprintf(req.methode, sizeof(req.methode), "%s", j->methode);
        req.cible     = j->chemin;   /* emprunte, non possede : jamais libere ici */
        req.chemin    = j->chemin;
        req.corps     = j->corps;
        req.corps_len = j->corps_len;

        OwlHttpProdCtx ctx;
        memset(&ctx, 0, sizeof(ctx));   /* fast_path = 0 : chemin FIFO normal */
        ctx.x = x;
        x->cfg.producteur(&req, &ctx, x->cfg.producteur_user);
        return NULL;
    }

    /* Voie SERVICE : routage runtime + handler surRequete. */
    if (x->cfg.service_mode) {
        return execute_service_job(x, j);
    }

    /* Entree : les octets possedes du jeton deviennent des ObjString
     * FRAICHES. Rien du thread reseau n'est reference. */
    pose_globale_chaine(OWL_HARNAIS_HTTP_METHODE, j->methode, strlen(j->methode));
    pose_globale_chaine(OWL_HARNAIS_HTTP_CHEMIN,  j->chemin,  strlen(j->chemin));
    pose_globale_chaine(OWL_HARNAIS_HTTP_CORPS,   j->corps,   j->corps_len);

    /* Sortie remise a zero : aucune valeur de la requete precedente ne
     * doit pouvoir survivre a un handler qui echoue. */
    obj_globals_set(OWL_HARNAIS_HTTP_STATUT, INTEGER_VAL(0));
    pose_globale_chaine(OWL_HARNAIS_HTTP_TYPE,   "", 0);
    pose_globale_chaine(OWL_HARNAIS_HTTP_SORTIE, "", 0);

    /* Frontiere entrante : tout ce qui aurait fuite est desormais
     * pendant. Les globales, elles, sont des racines explicites. */
    if (x->cfg.gc_frontiere) gc_collect();

    InterpretResult r = vm_call_global(x->procedure);
    if (r != INTERPRET_OK) {
        /* §8 et critere « un handler fautif produit un 500, pas un crash ».
         * La VM est reutilisable telle quelle : runtime_error() a remis
         * frameCount et stackTop a plat (mesure, famille 0 du harnais). */
        return owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
    }

    int    statut = 0;
    char   type[128];
    type[0] = '\0';
    const char* corps = "";
    size_t      corps_len = 0;

    Value v;
    if (lit_globale(OWL_HARNAIS_HTTP_STATUT, &v) && IS_NUMBER(v)) {
        statut = (int)AS_NUMBER(v);
    }
    if (lit_globale(OWL_HARNAIS_HTTP_TYPE, &v) && IS_STRING(v)) {
        snprintf(type, sizeof(type), "%s", AS_CSTRING(v));
    }
    if (lit_globale(OWL_HARNAIS_HTTP_SORTIE, &v) && IS_STRING(v)) {
        corps     = AS_CSTRING(v);
        corps_len = (size_t)AS_STRING(v)->length;
    }
    if (statut < 100 || statut > 599) statut = 500;

    /* cree_tampon COPIE : a partir d'ici plus rien ne pointe dans le
     * tas OWL. C'est la seule raison pour laquelle le GC force juste
     * apres est sans danger. */
    OwlHttpRep* rep = owl_http_rep_cree_tampon(statut, type, corps, corps_len);

    /* Frontiere sortante, AVANT de rendre la reponse au transport. */
    if (x->cfg.gc_frontiere) gc_collect();

    return rep;
}

bool owl_http_executeur_prepare_vm(OwlHttpExecuteur* x) {
    if (!x) return false;
    /* Mode C : le producteur pousse des octets bruts, il ne
     * s'appuie pas sur une procedure OWL compilee. Rien a interpreter. */
    if (x->cfg.producteur) return true;
    /* Voie SERVICE : compiler la source suffit — elle definit les
     * handlers globaux et peuple le routeur runtime. Pas de procedure fixe.
     *  : en mode projet, compile TOUTE la frontiere (compagnons avant
     * l'entree) pour que le service resolve les symboles voisins. */
    if (x->cfg.service_mode) {
        if (x->projet_nb > 0 && x->projet_sources) {
            return project_run_sources_no_teardown(
                       (const char* const*)x->projet_noms,
                       (const char* const*)x->projet_chemins,
                       (const char* const*)x->projet_sources,
                       x->projet_nb, x->projet_entree,
                       x->projet_mode != 0) == INTERPRET_OK;
        }
        if (!x->source_owl) return false;
        return vm_interpret(x->source_owl) == INTERPRET_OK;
    }
    if (!x->source_owl || !x->procedure) return false;
    return vm_interpret(x->source_owl) == INTERPRET_OK;
}

int owl_http_executeur_traite_une(OwlHttpExecuteur* x, int attente_ms) {
    if (!x) return 0;

    hx_verrouille(&x->m);
    while (x->tete == NULL && !x->arret) {
        if (attente_ms == 0) { hx_deverrouille(&x->m); return 0; }
        if (hx_cond_attend(&x->file_non_vide, &x->m, attente_ms) != 0) {
            if (x->tete == NULL) { hx_deverrouille(&x->m); return 0; }
        }
    }
    OwlHttpJeton* j = x->tete;
    if (!j) { hx_deverrouille(&x->m); return 0; }
    x->tete = j->suivant;
    if (!x->tete) x->queue = NULL;
    j->suivant = NULL;
    x->profondeur--;

    /* Plafond d'attente : le job est retire de la file SANS etre
     * execute. Il n'entre donc pas dans jobs_pris. */
    if (!j->sans_echeance && owl_tcp_server_horloge_ms() > j->echeance) {
        j->expire = 1;
        j->fait   = 1;
        int orphelin = j->abandonne;
        hx_cond_signale(&x->job_termine);
        jeton_reveille(j);         /* reacteur async : expiry a signaler (W7) */
        hx_deverrouille(&x->m);
        if (orphelin) jeton_libere(j);
        return -1;
    }

    x->jobs_pris++;
    note_thread_vm(x);
    /* Le job DEMARRE : a partir d'ici le plafond d'attente est neutralise
     * (D1). `demarre` est pose au depilage — un job depile ne renvoie plus de
     * 503 quel que soit le drain qui le prend. `jeton_courant`, LUI, n'est PAS
     * pose ici : avec M drains (LOT 3), un 2e drain l'ecraserait pendant que ce
     * thread execute son handler -> owl_http_prod_ouvre_flux lirait le mauvais
     * jeton. Il n'est publie qu'au thread qui tient le GIL (ci-dessous). */
    j->demarre = 1;
    hx_deverrouille(&x->m);

    /* Handler execute SANS le verrou file : la file reste ouverte aux depots
     * concurrents. Le GIL VM enrobe l'execution du handler : bord
     * externe, acquis AVANT execute_job, relache APRES. Regle d'ordre stricte :
     * GIL = verrou EXTERNE, x->m = verrou INTERNE ; on ne demande JAMAIS le GIL
     * en tenant x->m (deja deverrouille ci-dessus), et on relache x->m avant le
     * GIL. Avec M drains le GIL serialise l'execution : un seul thread execute
     * du bytecode a un instant donne. JAMAIS relache/repris par opcode (le
     * yield en cours de handler, c'est un lot ulterieur). */
    owl_vm_gil_acquire();

    /* `jeton_courant` (pont vers owl_http_prod_ouvre_flux, lu sous x->m) n'est
     * visible qu'au detenteur du GIL : les autres drains attendent le GIL sans
     * pouvoir l'ecraser. Pose sous x->m pour le lecteur, sous le GIL pour
     * l'unicite. */
    hx_verrouille(&x->m);
    x->jeton_courant = j;
    hx_deverrouille(&x->m);

    OwlHttpRep* rep = execute_job(x, j);

    /* Efface jeton_courant AVANT de relacher le GIL : sinon un autre drain
     * prendrait le GIL, poserait SON jeton, et notre reset a NULL l'ecraserait.
     * Verrou interne (x->m) relache avant le verrou externe (GIL). */
    hx_verrouille(&x->m);
    x->jeton_courant = NULL;
    hx_deverrouille(&x->m);
    owl_vm_gil_release();

    hx_verrouille(&x->m);

    if (j->flux_demarre) {
        /* Epilogue flux (mode C) : le transport a pris (ou prend) le flux
         * via j->rep — on n'y touche pas, on ne le libere pas (refcompte).
         * Le thread VM possede le JETON (transport detache, comme le chemin
         * orphelin) et le libere une fois le transport detache. */
        j->fait = 1;
        hx_cond_signale(&x->job_termine);
        /*  lot 4 — le yield cooperatif (relache du GIL au blocage flux)
         * permet a un handler de flux jamais draine (aucun transport pour poser
         * transport_detache) de rendre la main a l'executeur. Sans la garde
         * x->arret ci-dessous, ce thread resterait bloque ici a l'arret et
         * owl_http_executeur_arrete ne pourrait jamais le joindre -> teardown
         * bloque, puis UAF quand libere() detruit x sous le thread encore vif.
         * A l'arret on abandonne le flux en vol : jeton_libere relache la ref
         * refcomptee (owl_http_rep_libere), le transport eventuel garde la sienne. */
        while (!j->transport_detache && !j->abandonne && !x->arret) {
            hx_cond_attend(&x->job_termine, &x->m, 50);
        }
        hx_deverrouille(&x->m);
        jeton_libere(j);   /* j->rep pris par le transport (== NULL ici) */
        return 1;
    }

    j->rep  = rep;
    j->fait = 1;
    int orphelin = j->abandonne;
    hx_cond_signale(&x->job_termine);
    jeton_reveille(j);             /* reacteur async : reponse bufferisee prete (W7) */
    hx_deverrouille(&x->m);

    /* §8 : le client est parti, le handler a tourne quand meme, la
     * reponse est jetee. Ce n'est pas un bug a corriger. */
    if (orphelin) jeton_libere(j);
    return 1;
}

OwlHttpExecStatut owl_http_executeur_attend(OwlHttpExecuteur* x,
                                            OwlHttpJeton* jeton,
                                            OwlHttpRep** out_rep) {
    if (out_rep) *out_rep = NULL;
    if (!x || !jeton) return OWL_HTTP_EXEC_SATURE;

    hx_verrouille(&x->m);
    /* Un flux rend la main des `flux_demarre` (1er chunk pret), pas a
     * `fait` : c'est ce qui transforme le one-shot en corps incremental. */
    while (!jeton->fait && !jeton->flux_demarre) {
        /* Plafond d'attente : UNIQUEMENT tant que le job n'a pas demarre
         * (D1/§2.d). Une fois lance, une generation lente n'est pas un
         * client parti — plus de 503 apres demarrage. */
        if (!jeton->demarre && !jeton->sans_echeance &&
            owl_tcp_server_horloge_ms() > jeton->echeance) {
            /* Plus personne ne l'attend : la propriete passe a
             * l'executeur, qui le liberera quand il le depilera. */
            jeton->abandonne = 1;
            hx_deverrouille(&x->m);
            return OWL_HTTP_EXEC_SATURE;
        }
        hx_cond_attend(&x->job_termine, &x->m, 50);
    }

    if (jeton->flux_demarre) {
        /* Flux demarre : on prend le flux, on signale au thread VM qu'il
         * peut liberer le jeton, et on rend la main SANS liberer le jeton
         * (le thread VM en est proprietaire). */
        OwlHttpRep* rep = jeton->rep;
        jeton->rep               = NULL;
        jeton->transport_detache = 1;
        hx_cond_signale(&x->job_termine);
        hx_deverrouille(&x->m);
        if (out_rep) *out_rep = rep;
        return OWL_HTTP_EXEC_OK;
    }

    int expire = jeton->expire;
    OwlHttpRep* rep = jeton->rep;
    jeton->rep = NULL;
    hx_deverrouille(&x->m);

    jeton_libere(jeton);

    if (expire || !rep) {
        owl_http_rep_libere(rep);
        return OWL_HTTP_EXEC_SATURE;
    }
    if (out_rep) *out_rep = rep;
    return OWL_HTTP_EXEC_OK;
}

/* ---------- Resolution non bloquante (W7, exec async) ---------- */

void owl_http_executeur_jeton_definit_notif(OwlHttpExecuteur* x,
                                            OwlHttpJeton* jeton,
                                            OwlHttpJetonNotif cb, void* user) {
    if (!x || !jeton) return;
    hx_verrouille(&x->m);
    jeton->notif      = cb;
    jeton->notif_user = user;
    /* Course depot/resolution : si le job a ete resolu ENTRE le depot et cet
     * enregistrement, l'executeur a deja tire un notif NUL. On rattrape en
     * declenchant le cb sur-le-champ (le reacteur appellera essaie et verra
     * l'etat resolu). Reveil coalesce -> un doublon eventuel est benin. */
    int deja = jeton->fait || jeton->flux_demarre;
    hx_deverrouille(&x->m);
    if (deja && cb) cb(user);
}

uint64_t owl_http_executeur_jeton_echeance(const OwlHttpJeton* jeton) {
    if (!jeton || jeton->sans_echeance) return 0;
    return jeton->echeance;
}

OwlHttpEssaie owl_http_executeur_essaie(OwlHttpExecuteur* x, OwlHttpJeton* jeton,
                                        OwlHttpRep** out_rep) {
    if (out_rep) *out_rep = NULL;
    if (!x || !jeton) return OWL_HTTP_ESSAIE_SATURE;

    hx_verrouille(&x->m);

    if (!jeton->fait && !jeton->flux_demarre) {
        /* Plafond d'attente : UNIQUEMENT tant que le job n'a pas demarre
         * (D1/§2.d), identique a owl_http_executeur_attend. Le jeton passe a
         * l'executeur (abandonne) : il le liberera au depilage. */
        if (!jeton->demarre && !jeton->sans_echeance &&
            owl_tcp_server_horloge_ms() > jeton->echeance) {
            jeton->abandonne = 1;
            hx_deverrouille(&x->m);
            return OWL_HTTP_ESSAIE_SATURE;
        }
        hx_deverrouille(&x->m);
        return OWL_HTTP_ESSAIE_ATTEND;
    }

    if (jeton->flux_demarre) {
        /* Le flux a demarre : prendre le flux, signaler au thread VM qu'il
         * peut liberer le jeton, ne PAS le liberer ici (le thread VM en est
         * proprietaire — meme handshake que owl_http_executeur_attend). */
        OwlHttpRep* rep = jeton->rep;
        jeton->rep               = NULL;
        jeton->transport_detache = 1;
        hx_cond_signale(&x->job_termine);
        hx_deverrouille(&x->m);
        if (out_rep) *out_rep = rep;
        return OWL_HTTP_ESSAIE_FLUX;
    }

    /* fait (one-shot) : la propriete du jeton revient a l'appelant. */
    int expire = jeton->expire;
    OwlHttpRep* rep = jeton->rep;
    jeton->rep = NULL;
    hx_deverrouille(&x->m);

    jeton_libere(jeton);

    if (expire || !rep) {
        owl_http_rep_libere(rep);
        return OWL_HTTP_ESSAIE_SATURE;
    }
    if (out_rep) *out_rep = rep;
    return OWL_HTTP_ESSAIE_BUFFERISE;
}

void owl_http_executeur_abandonne(OwlHttpExecuteur* x, OwlHttpJeton* jeton) {
    if (!x || !jeton) return;
    hx_verrouille(&x->m);

    /* Race-safety. Le proprietaire du jeton depend de son etat :
     *
     *  - one-shot TERMINE (fait && !flux_demarre) : la reponse bufferisee
     *    attend d'etre prise par le transport via essaie. Le transport renonce
     *    ici et ne rappellera plus essaie -> c'est a NOUS de liberer.
     *
     *  - FLUX (flux_demarre) : le thread executeur POSSEDE le jeton (epilogue
     *    de traite_une, bloque sur `while (!transport_detache && !abandonne)`),
     *    et le liberera lui-meme. Le liberer ICI serait un DOUBLE FREE avec cet
     *    epilogue (crash concurrent observe sous N flux + deconnexion : la
     *    revue mono-flux ne couvrait pas le fait+flux_demarre concurrent). On
     *    se contente de poser `abandonne` et de reveiller l'epilogue.
     *
     *  - pas encore fait : le job est en file / en cours. Poser `abandonne` ;
     *    l'executeur le liberera au depilage (chemin orphelin) ou a l'epilogue.
     *
     * Reveiller job_termine dans TOUS les cas non-liberateurs : l'epilogue flux
     * attend ce signal pour liberer sans laisser trainer un stall de 50 ms. */
    if (jeton->fait && !jeton->flux_demarre) {
        hx_deverrouille(&x->m);
        jeton_libere(jeton);
        return;
    }
    jeton->abandonne = 1;
    hx_cond_signale(&x->job_termine);
    hx_deverrouille(&x->m);
}

OwlHttpExecStatut owl_http_executeur_soumet(OwlHttpExecuteur* x,
                                            const OwlHttpRequete* req,
                                            OwlHttpRep** out_rep) {
    OwlHttpJeton* j = NULL;
    if (owl_http_executeur_depose(x, req, &j) != OWL_HTTP_EXEC_OK) {
        return OWL_HTTP_EXEC_SATURE;
    }
    return owl_http_executeur_attend(x, j, out_rep);
}

/* ---------- Pool de drain VM (, LOT 3) ---------- */

/* Resout OWL_VM_THREADS : entier, defaut 4 (arbitrage A4), clampe
 * [1, OWL_VM_MAX_THREADS]. =1 = un seul drain (le maitre) -> comportement
 * mono-thread du LOT 1 (cas degenere). Controle interne, non-langage, non
 * documente publiquement (calque OWL_THREADS /  §4). */
static int vm_threads_resolve(void) {
    const char* e = getenv("OWL_VM_THREADS");
    int nt = 4;
    if (e && *e) {
        nt = atoi(e);
        if (nt < 1) nt = 1;   /* OWL_VM_THREADS<=0 -> serie stricte */
    }
    if (nt > OWL_VM_MAX_THREADS) nt = OWL_VM_MAX_THREADS;
    if (nt < 1) nt = 1;
    return nt;
}

/* Boucle de drain partagee par le maitre et les workers du pool : depile le
 * FIFO (sous x->m) et execute chaque job (sous le GIL, dans traite_une)
 * jusqu'a l'arret file videe. Aucun etat VM ne survit a un tour ; le GIL
 * serialise l'execution entre tous les drains. */
static void hx_drain_loop(OwlHttpExecuteur* x) {
    for (;;) {
        hx_verrouille(&x->m);
        int stop = x->arret && x->tete == NULL;
        hx_deverrouille(&x->m);
        if (stop) break;
        owl_http_executeur_traite_une(x, 50);
    }
}

/* Worker du pool : PAS de bootstrap, PAS de vm_free. La VM est deja nee sur le
 * maitre (vm_init/prepare_vm/FinInitialisation one-shot) ; ce thread ne fait
 * que drainer, l'execution etant serialisee par le GIL. */
#ifdef _WIN32
static DWORD WINAPI hx_drain_fn(LPVOID arg) {
    hx_drain_loop((OwlHttpExecuteur*)arg);
    return 0;
}
#else
static void* hx_drain_fn(void* arg) {
    hx_drain_loop((OwlHttpExecuteur*)arg);
    return NULL;
}
#endif

/* ---------- Thread MAITRE de la VM ---------- */

#ifdef _WIN32
static DWORD WINAPI hx_thread_fn(LPVOID arg)
#else
static void* hx_thread_fn(void* arg)
#endif
{
    OwlHttpExecuteur* x = (OwlHttpExecuteur*)arg;

    /* LE MAITRE FAIT NAITRE ET MOURIR LA VM (bootstrap + teardown one-shot).
     * Les workers du pool (LOT 3) ne font que drainer, jamais vm_init/vm_free. */
    vm_init();
    bool ok = owl_http_executeur_prepare_vm(x);

    /*  — FinInitialisation : one-shot au demarrage, AVANT d'accepter
     * la moindre requete (init d'une connexion base, d'un cache...). SANS
     * contexte MoiMême (aucune requete). Fail-safe : si l'init plante, on
     * avorte le demarrage et on n'entre pas dans la boucle — pas de service
     * sur une init ratee. Nom LITTERAL resolu canon (FR/EN).
     *
     * L'init s'execute AVANT de signaler vm_prete : le fail-safe doit etre
     * atomique. Sinon demarre() debloque sur vm_prete et relit un vm_echec
     * intermediaire (prepare_vm seul), rendant true alors qu'une init va
     * planter juste apres — course. Le signal est pose UNE fois, apres l'init,
     * vm_echec definitif reflete prepare_vm ET l'init. */
    bool initOk = true;
    if (ok) {
        if (vm_global_exists("FinInitialisation")) {
            initOk = (vm_call_global("FinInitialisation") == INTERPRET_OK);
        } else if (vm_global_exists("EndInitialization")) {
            initOk = (vm_call_global("EndInitialization") == INTERPRET_OK);
        }
    }

    hx_verrouille(&x->m);
    x->vm_prete = 1;
    x->vm_echec = (ok && initOk) ? 0 : 1;   /* definitif : prepare_vm ET init */
    hx_cond_signale(&x->job_termine);
    hx_deverrouille(&x->m);

    if (ok && initOk) {
        /* Pool de drain (LOT 3) : APRES le bootstrap one-shot, lancer M-1
         * threads exécuteurs supplémentaires. Le maitre est le M-ieme drain.
         * Tous drainent le MEME FIFO (sous x->m) ; le GIL serialise l'execution
         * VM (un seul execute du bytecode a un instant donne). Un echec de spawn
         * reduit le pool sans casser la correction (moins de threads = meme
         * resultat deterministe, juste moins de concurrence). */
        int M = vm_threads_resolve();
        x->pool_nb = 0;
        for (int i = 0; i < M - 1; i++) {
#ifdef _WIN32
            x->pool[i] = CreateThread(NULL, 0, hx_drain_fn, x, 0, NULL);
            if (x->pool[i] == NULL) break;
#else
            if (pthread_create(&x->pool[i], NULL, hx_drain_fn, x) != 0) break;
#endif
            x->pool_nb++;
        }

        hx_drain_loop(x);

        /* Teardown : joindre les M-1 workers AVANT Fermeture/vm_free (une seule
         * fois, sur le maitre). Le maitre n'atteint ce point qu'apres que SA
         * boucle a vu (x->arret && FIFO vide) ; les workers voient le meme etat
         * (x->arret pose dans arrete + poll 50 ms sur file_non_vide) et rendent
         * la main. Le join bloque jusqu'a ce que CHAQUE worker ait fini son job
         * courant : aucun bytecode ne tourne quand vm_free() s'execute.
         * Ainsi aucun bytecode ne tourne quand vm_free() s'execute. */
        for (size_t i = 0; i < x->pool_nb; i++) {
#ifdef _WIN32
            WaitForSingleObject(x->pool[i], 10000);
            CloseHandle(x->pool[i]);
#else
            pthread_join(x->pool[i], NULL);
#endif
        }
        x->pool_nb = 0;

        /*  — Fermeture : one-shot a l'arret, AVANT vm_free (teardown
         * d'une connexion base, flush d'un cache...). SANS contexte MoiMême.
         * Teardown best-effort : un plantage est ignore (on poursuit vers
         * vm_free). Garde ok && initOk : pas de teardown d'une init ratee.
         * Nom LITTERAL resolu canon (FR/EN). */
        if (vm_global_exists("Fermeture")) {
            (void)vm_call_global("Fermeture");
        } else if (vm_global_exists("Closing")) {
            (void)vm_call_global("Closing");
        }
    }

    vm_free();
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

bool owl_http_executeur_demarre(OwlHttpExecuteur* x) {
    if (!x || x->thread_lance) return false;
    /* Mode C : pas de procedure OWL a compiler ; le producteur
     * fournit le corps. Voie SERVICE : la source suffit (routage
     * runtime). Le harnais one-shot exige toujours sa source ET sa procedure. */
    if (x->cfg.service_mode) {
        if (!x->source_owl && x->projet_nb <= 0) return false;
    } else if (!x->cfg.producteur && (!x->source_owl || !x->procedure)) {
        return false;
    }

#ifdef _WIN32
    x->thread = CreateThread(NULL, 0, hx_thread_fn, x, 0, NULL);
    if (x->thread == NULL) return false;
#else
    if (pthread_create(&x->thread, NULL, hx_thread_fn, x) != 0) return false;
#endif
    x->thread_lance = 1;

    /* Rendre la main avant que la VM soit prete exposerait un serveur
     * qui accepte des requetes sans handler compile. */
    hx_verrouille(&x->m);
    while (!x->vm_prete) hx_cond_attend(&x->job_termine, &x->m, 50);
    int echec = x->vm_echec;
    hx_deverrouille(&x->m);

    if (echec) { owl_http_executeur_arrete(x); return false; }
    return true;
}

void owl_http_executeur_arrete(OwlHttpExecuteur* x) {
    if (!x || !x->thread_lance) return;

    hx_verrouille(&x->m);
    x->arret = 1;
    hx_cond_signale(&x->file_non_vide);
    hx_deverrouille(&x->m);

#ifdef _WIN32
    WaitForSingleObject(x->thread, 10000);
    CloseHandle(x->thread);
#else
    pthread_join(x->thread, NULL);
#endif
    x->thread_lance = 0;
}

/* ---------- Point de branchement sur le transport ---------- */

/* Fast-path plan de controle (, W2, design §7.5 + §8 Q1) : une route
 * de controle ne touche NI VM NI modele, on execute donc le producteur INLINE
 * sur le thread appelant en contournant la FIFO mono-thread de la VM. Sans ce
 * bypass, /v1/models ferait la queue derriere une generation lente (repro-A,
 * §1). Le producteur ouvre le flux (owl_http_prod_ouvre_flux, mode fast-path),
 * pousse, termine, relache — le flux (refcount 1 apres relache du producteur)
 * est rendu au transport, qui le draine puis le libere. NULL si le producteur
 * n'a rien ouvert (route de controle non geree par ce producteur). */
static OwlHttpRep* controle_inline(OwlHttpExecuteur* x, const OwlHttpRequete* req) {
    OwlHttpProdCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.x         = x;
    ctx.fast_path = 1;
    x->cfg.producteur(req, &ctx, x->cfg.producteur_user);
    return ctx.fast_flux;
}

OwlHttpRep* owl_http_exec_handler_rep(const OwlHttpRequete* req, void* user) {
    OwlHttpExecuteur* x = (OwlHttpExecuteur*)user;
    if (!x) return owl_http_rep_cree_tampon(503, NULL, NULL, 0);

    /* Bypass VM-safe de la FIFO pour le plan de controle. */
    if (x->cfg.producteur && owl_http_route_est_controle(req->chemin)) {
        OwlHttpRep* fp = controle_inline(x, req);
        if (fp) return fp;
        return owl_http_rep_cree_tampon(404, NULL, NULL, 0);
    }

    OwlHttpRep* r = NULL;
    if (owl_http_executeur_soumet(x, req, &r) != OWL_HTTP_EXEC_OK || !r) {
        /* File pleine ou attente expiree : 503, et l'executeur n'a rien
         * eu a en connaitre — jobs_pris le prouve (§7). */
        owl_http_rep_libere(r);
        return owl_http_rep_cree_tampon(503, NULL, NULL, 0);
    }

    /* La propriete part avec la reponse : le worker qui a servi cette
     * connexion la liberera apres l'avoir ecrite, sans rien partager
     * avec les autres. */
    return r;
}
