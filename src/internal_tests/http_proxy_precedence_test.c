// SPDX-License-Identifier: AGPL-3.0-only
/* http_proxy_precedence_test.c — 
 *
 * Caracterise la PRECEDENCE entre le proxy configure explicitement
 * (HTTPParametre) et les variables de proxy de l'ENVIRONNEMENT, dans
 * resolve_proxy (src/stdlib_http.c).
 *
 * Defaut instruit : dans le meme corps de fonction, le proxy explicite
 * BAT http_proxy de l'environnement, mais se fait BATTRE par no_proxy de
 * l'environnement. Ce n'est pas un arbitrage, c'est un ordre de tests
 * incoherent avec lui-meme : une variable que le developpeur ne controle
 * pas fait ignorer en silence un proxy pose en dur dans le code.
 *
 * Canon : la doc de la fonction de parametrage HTTP canon ne connait que
 * trois options (cache, cookies, mode d'implementation) — aucune option
 * de proxy, et aucune mention de variable d'environnement nulle part
 * dans la famille proxy canon. Le canon est donc MUET sur cette
 * precedence : les options de proxy d'OWL sont une extension, et la
 * convention du secteur s'applique (explicite > environnement, modele de
 * curl dont l'option --noproxy l'emporte sur NO_PROXY).
 *
 * ========================= COMMENT ON MESURE =========================
 *
 * resolve_proxy est `static` : aucun point d'entree C. On passe donc par
 * la native, en clair (pas de HTTPS) — le chemin proxy en clair emet une
 * requete a URI ABSOLUE vers le proxy, la ou une requete directe emet un
 * chemin relatif vers l'origine. Le rig monte donc TROIS pairs qui
 * repondent chacun un corps DIFFERENT :
 *
 *   ORIGINE  -> "ORIGINE"   la cible, joignable en direct
 *   PXEXPL   -> "PXEXPL"    le proxy passe a HTTPParametre
 *   PXENV    -> "PXENV"     le proxy pose dans http_proxy
 *
 * Le corps rendu au programme OWL dit donc SANS AMBIGUITE quel chemin a
 * ete pris, et il n'existe aucun etat dans lequel deux reponses se
 * confondent. Chaque vecteur asserte en plus :
 *
 *   a. le corps EXACT attendu ;
 *   b. que le pair attendu a servi 1 connexion et que les DEUX AUTRES en
 *      ont servi 0 (un « bon corps » ne prouve pas que les autres n'ont
 *      pas ete sollicites) ;
 *   c. la LIGNE DE REQUETE recue : URI absolue cote proxy, chemin
 *      relatif cote origine. Sans elle, « joindre le proxy » serait vert
 *      pour un client qui traite le proxy comme une origine.
 *
 * ===================== ISOLATION D'ENVIRONNEMENT =====================
 *
 * Deux etats globaux traversent les self-tests du meme binaire :
 *
 *   - les variables de proxy du PROCESSUS : un voisin y pose
 *     no_proxy=127.0.0.1, un autre les vide. C'est exactement ce qui a
 *     rendu vert un test de  sans executer une ligne du code
 *     sous test ;
 *   - g_http_params, statique de stdlib_http.c, qui survit a vm_free().
 *
 * Les variables sont donc REECRITES A CHAQUE VECTEUR (valeur voulue ou
 * vide, jamais « laissee telle quelle »), assertees a l'entree, et
 * g_http_params est remis a zero par httpParamReinitialise en tete de
 * chaque programme. Cf. pose_env pour le piege de la casse sous Windows.
 *
 * ============================= FAMILLES ==============================
 *
 *   F1 isolation + controles positif et negatif du rig
 *   F2 les trois confrontations de precedence
 *   F3 coherence : symetrie proxy explicite / proxy d'environnement,
 *      et no_proxy qui n'apparie pas
 *
 * Execute via `owl --self-test-net` (cf. main.c), sous vm_init. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

#include "../vm.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  typedef HANDLE pp_thread_t;
  typedef SOCKET pp_socket_t;
  #define PP_INVALID    INVALID_SOCKET
  #define PP_CLOSE      closesocket
  #define PP_SHUT_WR    SD_SEND
  #define PP_SLEEP(ms)  Sleep((DWORD)(ms))
  #define owl_dup    _dup
  #define owl_dup2   _dup2
  #define owl_close  _close
  #define owl_fileno _fileno
  #define owl_unlink _unlink
  #define owl_open   _open
  #define owl_putenv _putenv
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
#else
  #include <pthread.h>
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <time.h>
  typedef pthread_t pp_thread_t;
  typedef int pp_socket_t;
  #define PP_INVALID    (-1)
  #define PP_CLOSE      close
  #define PP_SHUT_WR    SHUT_WR
  static void pp_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define PP_SLEEP(ms)  pp_sleep_ms((int)(ms))
  #define owl_dup    dup
  #define owl_dup2   dup2
  #define owl_close  close
  #define owl_fileno fileno
  #define owl_unlink unlink
  #define owl_open   open
  #define owl_putenv putenv
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ================================================================
 * Comptage
 * ================================================================ */

#define PP_NB_FAMILLES 3

static int g_pass[PP_NB_FAMILLES];
static int g_fail[PP_NB_FAMILLES];
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < PP_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Pair minimal : repond toujours le MEME corps, enregistre la
 * premiere requete recue et compte les connexions servies.
 * ================================================================ */

#define PP_MAX_RECU 2048

typedef struct {
    const char* corps;      /* corps rendu, sert de signature du pair */
    int         port;
    int         pret;
    int         stop;
    int         demarre;
    int         nb_servis;
    char        recu[PP_MAX_RECU];
    pp_thread_t thread;
} Pair;

static int pp_write_all(pp_socket_t sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static void pp_lit_requete(Pair* p, pp_socket_t cli) {
    /* Seule la premiere requete est conservee : chaque vecteur n'en emet
     * qu'une, et un second tour signalerait une anomalie que nb_servis
     * suffit a rendre visible. */
    char  jetable[1024];
    char* dst = (p->nb_servis == 0) ? p->recu : jetable;
    int   cap = (p->nb_servis == 0) ? PP_MAX_RECU - 1 : (int)sizeof(jetable) - 1;
    int   len = 0;
    char  tmp[1024];
    dst[0] = '\0';
    while (1) {
        if (len >= 4 && memcmp(dst + len - 4, "\r\n\r\n", 4) == 0) break;
        int n = recv(cli, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) break;
        if (len < cap) {
            int cp = n < cap - len ? n : cap - len;
            memcpy(dst + len, tmp, (size_t)cp);
            len += cp;
            dst[len] = '\0';
        }
    }
}

static void pp_sert(Pair* p, pp_socket_t cli) {
    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
#ifdef _WIN32
    DWORD tv = 3000;
#else
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
#endif
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    pp_lit_requete(p, cli);

    char rep[512];
    int n = snprintf(rep, sizeof(rep),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n%s",
                     (int)strlen(p->corps), p->corps);
    if (n > 0) pp_write_all(cli, rep, n);

    /* Incremente APRES l'ecriture : le thread principal ne doit jamais
     * lire un tour a moitie enregistre. */
    p->nb_servis++;

    shutdown(cli, PP_SHUT_WR);
    char poubelle[512];
    int garde = 0;
    while (garde++ < 32) {
        int k = recv(cli, poubelle, (int)sizeof(poubelle), 0);
        if (k <= 0) break;
    }
    PP_CLOSE(cli);
}

#ifdef _WIN32
static DWORD WINAPI pp_thread_fn(LPVOID arg) {
#else
static void* pp_thread_fn(void* arg) {
#endif
    Pair* p = (Pair*)arg;

    pp_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv != PP_INVALID) {
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
                p->port = ntohs(addr.sin_port);
                p->pret = 1;
                /* select() avant accept() : le thread doit pouvoir
                 * constater l'ordre d'arret entre deux connexions. */
                while (!p->stop && p->nb_servis < 16) {
                    fd_set r;
                    FD_ZERO(&r);
                    FD_SET(srv, &r);
                    struct timeval att; att.tv_sec = 0; att.tv_usec = 100 * 1000;
                    if (select((int)srv + 1, &r, NULL, NULL, &att) <= 0) continue;
                    pp_socket_t cli = accept(srv, NULL, NULL);
                    if (cli == PP_INVALID) continue;
                    pp_sert(p, cli);
                }
            }
        }
        PP_CLOSE(srv);
    }
    p->pret = 1;   /* debloque l'attente meme en cas d'echec */
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int pp_demarre(Pair* p, const char* corps) {
    memset(p, 0, sizeof(*p));
    p->corps = corps;
#ifdef _WIN32
    p->thread = CreateThread(NULL, 0, pp_thread_fn, p, 0, NULL);
    if (p->thread == NULL) return 0;
#else
    if (pthread_create(&p->thread, NULL, pp_thread_fn, p) != 0) return 0;
#endif
    p->demarre = 1;
    int attendu = 0;
    while (!p->pret && attendu < 2000) { PP_SLEEP(10); attendu += 10; }
    return p->port > 0;
}

static void pp_arrete(Pair* p) {
    if (!p->demarre) return;
    p->stop = 1;
#ifdef _WIN32
    WaitForSingleObject(p->thread, 10000);
    CloseHandle(p->thread);
#else
    pthread_join(p->thread, NULL);
#endif
    p->demarre = 0;
}

/* Ligne de requete du premier tour ("" si aucun tour servi). */
static void pp_ligne_requete(const Pair* p, char* dst, size_t cap) {
    dst[0] = '\0';
    if (p->nb_servis == 0) return;
    const char* fin = strstr(p->recu, "\r\n");
    if (!fin) return;
    size_t n = (size_t)(fin - p->recu);
    if (n >= cap) n = cap - 1;
    memcpy(dst, p->recu, n);
    dst[n] = '\0';
}

/* ================================================================
 * Capture stdout autour d'un vm_interpret
 * ================================================================ */

static char g_out[32 * 1024];

static InterpretResult capture_interpret(const char* source) {
    char TMP[128];
    fixture_unique(TMP, sizeof(TMP), "_owl_http_pxprec_test_", ".tmp");
    g_out[0] = '\0';

    fflush(stdout);
    int saved_fd = owl_dup(owl_fileno(stdout));
    if (saved_fd < 0) return INTERPRET_RUNTIME_ERROR;

    int tmp_fd = owl_open(TMP, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (tmp_fd < 0) { owl_close(saved_fd); return INTERPRET_RUNTIME_ERROR; }
    if (owl_dup2(tmp_fd, owl_fileno(stdout)) < 0) {
        owl_close(tmp_fd); owl_close(saved_fd);
        return INTERPRET_RUNTIME_ERROR;
    }
    owl_close(tmp_fd);

    InterpretResult res = vm_interpret(source);
    fflush(stdout);

    owl_dup2(saved_fd, owl_fileno(stdout));
    owl_close(saved_fd);

    FILE* r = fopen(TMP, "rb");
    if (r) {
        size_t n = fread(g_out, 1, sizeof(g_out) - 1, r);
        g_out[n] = '\0';
        fclose(r);
    }
    owl_unlink(TMP);
    return res;
}

static int lit_entier(const char* cle) {
    char motif[32];
    snprintf(motif, sizeof(motif), "%s=", cle);
    const char* p = strstr(g_out, motif);
    if (!p) return -1;
    return atoi(p + strlen(motif));
}

static void lit_entre_crochets(const char* cle, char* dst, size_t cap) {
    dst[0] = '\0';
    char motif[32];
    snprintf(motif, sizeof(motif), "%s=[", cle);
    const char* p = strstr(g_out, motif);
    if (!p) return;
    p += strlen(motif);
    const char* fin = strchr(p, ']');
    if (!fin) return;
    size_t n = (size_t)(fin - p);
    if (n >= cap) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = '\0';
}

/* ================================================================
 * Pilotage d'un vecteur
 * ================================================================ */

/* Les variables sont TOUJOURS reecrites : `NULL` vaut « vide », pas
 * « laisse telle quelle ». C'est ce qui rend chaque vecteur independant
 * de l'ordre des self-tests.
 *
 * UNE SEULE GRAPHIE par variable, mesure faite : sous Windows les noms
 * de variables d'environnement sont INSENSIBLES A LA CASSE — poser
 * `http_proxy=X` puis `HTTP_PROXY=` efface X. Une premiere version de ce
 * rig ecrivait les six graphies, et neutralisait donc silencieusement
 * tout ce qu'elle croyait poser (mesure : le vecteur « http_proxy seul »
 * partait en direct). La distinction minuscules / MAJUSCULES que lit
 * resolve_proxy n'est de ce fait pas observable ici. */
static void pose_env(const char* http_proxy, const char* no_proxy) {
    static char b1[512], b2[512], b3[512];
    snprintf(b1, sizeof(b1), "http_proxy=%s",  http_proxy ? http_proxy : "");
    snprintf(b2, sizeof(b2), "https_proxy=");
    snprintf(b3, sizeof(b3), "no_proxy=%s",    no_proxy   ? no_proxy   : "");
    owl_putenv(b1); owl_putenv(b2); owl_putenv(b3);
}

typedef struct {
    /* --- entree --- */
    const char* opt_proxy;      /* NULL = pas de httpParamProxy */
    const char* opt_noproxy;    /* NULL = pas de httpParamNoProxy */
    const char* env_proxy;      /* NULL = http_proxy vide */
    const char* env_noproxy;    /* NULL = no_proxy vide */
    /* --- sortie --- */
    int  interprete_ok;
    int  erreur;
    int  statut;
    char corps[512];
} Vecteur;

static int g_seq = 0;

/* Joue une requete en clair vers l'ORIGINE et rend qui a repondu. */
static void joue(Vecteur* v, const Pair* origine, const char* chemin) {
    pose_env(v->env_proxy, v->env_noproxy);

    int id = ++g_seq;
    char src[1536];
    int n = snprintf(src, sizeof(src),
        /* g_http_params est un statique qui survit a vm_free() : on
         * repart de zero, sinon un vecteur herite du precedent. */
        "HTTPParametre(httpParamReinitialise, 0)\n"
        "HTTPParametre(httpParamTimeoutConnexion, 5000)\n"
        "HTTPParametre(httpParamGardeConnexion, Faux)\n");
    if (v->opt_proxy)
        n += snprintf(src + n, sizeof(src) - (size_t)n,
                      "HTTPParametre(httpParamProxy, \"%s\")\n", v->opt_proxy);
    if (v->opt_noproxy)
        n += snprintf(src + n, sizeof(src) - (size_t)n,
                      "HTTPParametre(httpParamNoProxy, \"%s\")\n", v->opt_noproxy);
    snprintf(src + n, sizeof(src) - (size_t)n,
        "r%d est une chaine = HTTPRequete(\"http://127.0.0.1:%d%s\")\n"
        /* ErreurDetectee se lit AVANT tout autre appel : la sentinelle
         * d'erreur ne survit qu'a un seul OP_CALL. */
        "e%d est un booleen = ErreurDetectee\n"
        "Trace(\"ERREUR=\" + e%d)\n"
        "Trace(\"STATUT=\" + HTTPDernierStatut())\n"
        "Trace(\"CORPS=[\" + r%d + \"]\")\n",
        id, origine->port, chemin, id, id, id);

    v->interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    v->erreur = lit_entier("ERREUR");
    v->statut = lit_entier("STATUT");
    lit_entre_crochets("CORPS", v->corps, sizeof(v->corps));
}

/* ================================================================
 * Verdicts : QUI a ete joint, prouve sur trois faits independants
 * ================================================================ */

/* Le proxy `px` a servi la requete, l'origine et l'autre proxy non, et
 * la requete recue par le proxy portait bien une URI ABSOLUE. */
static void atteste_via_proxy(const Vecteur* v, const Pair* px, const char* corps_px,
                              const Pair* origine, const Pair* autre_px,
                              const char* chemin, const char* prefixe) {
    char label[128], ligne[512], attendu[256];

    snprintf(label, sizeof(label), "%s_corps_du_proxy", prefixe);
    compte(strcmp(v->corps, corps_px) == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_statut_200", prefixe);
    compte(v->statut == 200, label, __LINE__);

    snprintf(label, sizeof(label), "%s_aucune_erreur", prefixe);
    compte(v->erreur == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_proxy_a_servi_1", prefixe);
    compte(px->nb_servis == 1, label, __LINE__);

    snprintf(label, sizeof(label), "%s_origine_intouchee", prefixe);
    compte(origine->nb_servis == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_autre_proxy_intouche", prefixe);
    compte(autre_px == NULL || autre_px->nb_servis == 0, label, __LINE__);

    /* URI absolue : sans cette assertion, « le proxy a repondu » serait
     * vert pour un client qui traite le proxy comme une origine. */
    pp_ligne_requete(px, ligne, sizeof(ligne));
    snprintf(attendu, sizeof(attendu), "GET http://127.0.0.1:%d%s HTTP/1.1",
             origine->port, chemin);
    snprintf(label, sizeof(label), "%s_uri_absolue_vers_l_origine", prefixe);
    compte(strcmp(ligne, attendu) == 0, label, __LINE__);
    if (strcmp(ligne, attendu) != 0)
        fprintf(stderr, "  [%s] ligne=[%s] attendu=[%s] corps=[%s]\n",
                prefixe, ligne, attendu, v->corps);
}

/* L'origine a servi la requete EN DIRECT, aucun proxy sollicite. */
static void atteste_direct(const Vecteur* v, const Pair* origine,
                           const Pair* px_a, const Pair* px_b,
                           const char* chemin, const char* prefixe) {
    char label[128], ligne[512], attendu[256];

    snprintf(label, sizeof(label), "%s_corps_de_l_origine", prefixe);
    compte(strcmp(v->corps, "ORIGINE") == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_statut_200", prefixe);
    compte(v->statut == 200, label, __LINE__);

    snprintf(label, sizeof(label), "%s_aucune_erreur", prefixe);
    compte(v->erreur == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_origine_a_servi_1", prefixe);
    compte(origine->nb_servis == 1, label, __LINE__);

    snprintf(label, sizeof(label), "%s_proxy_explicite_intouche", prefixe);
    compte(px_a == NULL || px_a->nb_servis == 0, label, __LINE__);

    snprintf(label, sizeof(label), "%s_proxy_environnement_intouche", prefixe);
    compte(px_b == NULL || px_b->nb_servis == 0, label, __LINE__);

    /* Chemin RELATIF : la contrepartie de l'URI absolue ci-dessus. */
    pp_ligne_requete(origine, ligne, sizeof(ligne));
    snprintf(attendu, sizeof(attendu), "GET %s HTTP/1.1", chemin);
    snprintf(label, sizeof(label), "%s_chemin_relatif", prefixe);
    compte(strcmp(ligne, attendu) == 0, label, __LINE__);
    if (strcmp(ligne, attendu) != 0)
        fprintf(stderr, "  [%s] ligne=[%s] attendu=[%s] corps=[%s]\n",
                prefixe, ligne, attendu, v->corps);
}

/* Un vecteur = trois pairs neufs (compteurs a zero) + une requete. */
typedef struct { Pair origine, px_expl, px_env; int ok; } Scene;

static int scene_ouvre(Scene* s) {
    s->ok = pp_demarre(&s->origine, "ORIGINE")
         && pp_demarre(&s->px_expl, "PXEXPL")
         && pp_demarre(&s->px_env,  "PXENV");
    return s->ok;
}

static void scene_ferme(Scene* s) {
    pp_arrete(&s->origine);
    pp_arrete(&s->px_expl);
    pp_arrete(&s->px_env);
}

static void url_proxy(char* dst, size_t cap, const Pair* p) {
    snprintf(dst, cap, "http://127.0.0.1:%d", p->port);
}

/* ================================================================
 * F1 — isolation + controles positif et negatif du rig
 *
 * Sans cette famille, tout le reste peut etre vrai pour la mauvaise
 * raison : un rig incapable de router vers un proxy rendrait « direct »
 * partout, et un rig qui route toujours rendrait « proxy » partout.
 * ================================================================ */

static void famille_f1(void) {
    g_famille = 0;

    /* --- 1.1 l'environnement du processus ne peut pas expliquer un vert.
     * Mesure directe, pas deduction : un self-test voisin y laisse
     * no_proxy=127.0.0.1, ce qui contournerait tout proxy. --- */
    {
        pose_env(NULL, NULL);
        const char* x;
        x = getenv("http_proxy");  CHECK(x == NULL || x[0] == '\0', "f1_1_env_http_proxy_vide");
        x = getenv("HTTP_PROXY");  CHECK(x == NULL || x[0] == '\0', "f1_1_env_HTTP_PROXY_vide");
        x = getenv("https_proxy"); CHECK(x == NULL || x[0] == '\0', "f1_1_env_https_proxy_vide");
        x = getenv("HTTPS_PROXY"); CHECK(x == NULL || x[0] == '\0', "f1_1_env_HTTPS_PROXY_vide");
        x = getenv("no_proxy");    CHECK(x == NULL || x[0] == '\0', "f1_1_env_no_proxy_vide");
        x = getenv("NO_PROXY");    CHECK(x == NULL || x[0] == '\0', "f1_1_env_NO_PROXY_vide");
    }

    /* --- 1.2 CONTROLE NEGATIF : aucun proxy nulle part -> direct --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f1_2_scene_ouverte"); scene_ferme(&s); return; }
        Vecteur v; memset(&v, 0, sizeof(v));
        joue(&v, &s.origine, "/direct");
        CHECK(v.interprete_ok, "f1_2_programme_interprete");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/direct", "f1_2");
        scene_ferme(&s);
    }

    /* --- 1.3 CONTROLE POSITIF : proxy explicite seul -> via le proxy --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f1_3_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy = u;
        joue(&v, &s.origine, "/expl");
        CHECK(v.interprete_ok, "f1_3_programme_interprete");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/expl", "f1_3");
        scene_ferme(&s);
    }

    /* --- 1.4 http_proxy de l'environnement seul -> via ce proxy.
     * Prouve que le rig sait AUSSI faire agir l'environnement : sans ca,
     * les vecteurs de F2 ou l'environnement doit perdre seraient verts
     * simplement parce qu'il n'est jamais lu. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f1_4_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.env_proxy = u;
        joue(&v, &s.origine, "/env");
        CHECK(v.interprete_ok, "f1_4_programme_interprete");
        atteste_via_proxy(&v, &s.px_env, "PXENV", &s.origine, &s.px_expl, "/env", "f1_4");
        scene_ferme(&s);
    }

    /* --- 1.5 no_proxy de l'environnement seul, sans aucun proxy : rien a
     * contourner, la requete part en direct. Vecteur de forme, il borne
     * l'interpretation de 2.3. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f1_5_scene_ouverte"); scene_ferme(&s); return; }
        Vecteur v; memset(&v, 0, sizeof(v));
        v.env_noproxy = "127.0.0.1";
        joue(&v, &s.origine, "/nopx");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/nopx", "f1_5");
        scene_ferme(&s);
    }
}

/* ================================================================
 * F2 — LES TROIS CONFRONTATIONS
 *
 * « Environnement » n'est pas un bloc : la confrontation depend de
 * l'origine de CHACUN des deux reglages. Les trois lignes sont
 * couvertes, y compris les deux qui sont deja correctes — sans elles,
 * rien n'empeche de reparer la deuxieme en cassant les autres.
 * ================================================================ */

static void famille_f2(void) {
    g_famille = 1;

    /* --- 2.1 no_proxy EXPLICITE vs proxy EXPLICITE -> no_proxy gagne.
     * L'utilisateur a pose les deux sciemment, l'exclusion est sa raison
     * d'etre. Deja correct aujourd'hui. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f2_1_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = u;
        v.opt_noproxy = "127.0.0.1";
        joue(&v, &s.origine, "/c1");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/c1", "f2_1");
        scene_ferme(&s);
    }

    /* --- 2.2 no_proxy de l'ENVIRONNEMENT vs proxy EXPLICITE -> le PROXY
     * EXPLICITE gagne. C'est la seule ligne qui bouge : une variable que
     * le developpeur ne controle pas ne doit pas faire ignorer en silence
     * un proxy pose dans le code. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f2_2_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = u;
        v.env_noproxy = "127.0.0.1";
        joue(&v, &s.origine, "/c2");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/c2", "f2_2");
        scene_ferme(&s);
    }

    /* --- 2.2bis meme confrontation avec le joker : no_proxy=* de
     * l'environnement ne doit pas davantage desactiver un proxy pose
     * dans le code. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f2_2b_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = u;
        v.env_noproxy = "*";
        joue(&v, &s.origine, "/c2b");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/c2b", "f2_2b");
        scene_ferme(&s);
    }

    /* --- 2.3 no_proxy de l'ENVIRONNEMENT vs proxy de l'ENVIRONNEMENT ->
     * no_proxy gagne. Meme origine pour les deux reglages : c'est la
     * semantique usuelle des variables de proxy. Deja correct. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f2_3_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.env_proxy   = u;
        v.env_noproxy = "127.0.0.1";
        joue(&v, &s.origine, "/c3");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/c3", "f2_3");
        scene_ferme(&s);
    }
}

/* ================================================================
 * F3 — coherence de la fonction avec elle-meme
 * ================================================================ */

static void famille_f3(void) {
    g_famille = 2;

    /* --- 3.1 proxy EXPLICITE vs proxy de l'ENVIRONNEMENT -> l'explicite
     * gagne. C'est la precedence qui rendait l'ordre des tests
     * incoherent : elle etait deja correcte et doit le rester. Les deux
     * proxys sont VIVANTS, le corps rendu designe donc le gagnant sans
     * qu'un echec de connexion puisse tenir lieu de preuve. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_1_scene_ouverte"); scene_ferme(&s); return; }
        char ue[64], uv[64];
        url_proxy(ue, sizeof(ue), &s.px_expl);
        url_proxy(uv, sizeof(uv), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy = ue;
        v.env_proxy = uv;
        joue(&v, &s.origine, "/d1");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/d1", "f3_1");
        scene_ferme(&s);
    }

    /* --- 3.2 no_proxy EXPLICITE vs proxy de l'ENVIRONNEMENT -> le
     * no_proxy explicite gagne (une exclusion posee dans le code doit
     * pouvoir desactiver un proxy herite de l'environnement). --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_2_scene_ouverte"); scene_ferme(&s); return; }
        char uv[64]; url_proxy(uv, sizeof(uv), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_noproxy = "127.0.0.1";
        v.env_proxy   = uv;
        joue(&v, &s.origine, "/d2");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/d2", "f3_2");
        scene_ferme(&s);
    }

    /* --- 3.3 no_proxy de l'ENVIRONNEMENT QUI N'APPARIE PAS + proxy
     * explicite -> le proxy sert. Controle de non-vacuite de 2.2 : sans
     * lui, 2.2 serait vert pour un no_proxy qui n'apparie jamais rien.
     * --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_3_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = u;
        v.env_noproxy = "exemple.invalide";
        joue(&v, &s.origine, "/d3");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/d3", "f3_3");
        scene_ferme(&s);
    }

    /* --- 3.4 no_proxy de l'ENVIRONNEMENT QUI N'APPARIE PAS + proxy de
     * l'ENVIRONNEMENT -> le proxy sert. Meme controle pour 2.3. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_4_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.env_proxy   = u;
        v.env_noproxy = "exemple.invalide";
        joue(&v, &s.origine, "/d4");
        atteste_via_proxy(&v, &s.px_env, "PXENV", &s.origine, &s.px_expl, "/d4", "f3_4");
        scene_ferme(&s);
    }

    /* --- 3.5 no_proxy EXPLICITE QUI N'APPARIE PAS + proxy explicite ->
     * le proxy sert. Controle de non-vacuite de 2.1. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_5_scene_ouverte"); scene_ferme(&s); return; }
        char u[64]; url_proxy(u, sizeof(u), &s.px_expl);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = u;
        v.opt_noproxy = "exemple.invalide";
        joue(&v, &s.origine, "/d5");
        atteste_via_proxy(&v, &s.px_expl, "PXEXPL", &s.origine, &s.px_env, "/d5", "f3_5");
        scene_ferme(&s);
    }

    /* --- 3.6 les quatre reglages a la fois : no_proxy explicite qui
     * apparie l'emporte sur tout le reste. Verifie que la reparation de
     * 2.2 n'a pas fait remonter un proxy au-dessus de l'exclusion
     * explicite. --- */
    {
        Scene s;
        if (!scene_ouvre(&s)) { CHECK(0, "f3_6_scene_ouverte"); scene_ferme(&s); return; }
        char ue[64], uv[64];
        url_proxy(ue, sizeof(ue), &s.px_expl);
        url_proxy(uv, sizeof(uv), &s.px_env);
        Vecteur v; memset(&v, 0, sizeof(v));
        v.opt_proxy   = ue;
        v.opt_noproxy = "127.0.0.1";
        v.env_proxy   = uv;
        v.env_noproxy = "exemple.invalide";
        joue(&v, &s.origine, "/d6");
        atteste_direct(&v, &s.origine, &s.px_expl, &s.px_env, "/d6", "f3_6");
        scene_ferme(&s);
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_proxy_precedence_self_test_run(void) {
    printf("== precedence proxy explicite / environnement (fix/1396) ==\n");

    memset(g_pass, 0, sizeof(g_pass));
    memset(g_fail, 0, sizeof(g_fail));

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    famille_f1();
    famille_f2();
    famille_f3();

    /* Ne pas laisser derriere nous l'environnement qu'on a manipule :
     * c'est exactement le piege qui a produit le faux vert de 
     * chez le voisin. */
    pose_env(NULL, NULL);
    vm_interpret("HTTPParametre(httpParamReinitialise, 0)\n");

    int tp = 0, tf = 0;
    for (int i = 0; i < PP_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f1 isolation + controles    : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f2 trois confrontations     : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f3 coherence de precedence  : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   TOTAL precedence proxy      : %d/%d\n", tp, tp + tf);
    return tf;
}
