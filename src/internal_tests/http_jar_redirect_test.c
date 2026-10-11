// SPDX-License-Identifier: AGPL-3.0-only
/* http_jar_redirect_test.c — 
 *
 * Caracterise le JAR DE COOKIES et le SUIVI DES REDIRECTIONS du client
 * HTTP contre un vrai pair TCP local, SANS mocker HTTPRequete ni
 * HTTPEnvoie. Les seuls tests qui touchaient ces chemins jusqu'ici
 * (tests/runtime/test_http_mocked.owl, test_http_gaps_mocked.owl)
 * mockent la native elle-meme : ils mockent le sujet sous test. La
 * couverture C existante (http_llhttp_integration_test.c:367) verifie
 * seulement que le PARSEUR accepte des Set-Cookie repetes, pas que le
 * jar les stocke, les apparie et les renvoie.
 *
 * Pourquoi via la VM et pas un appel C direct : cookie_jar_set,
 * cookie_jar_build, cookie_domain_matches, cookie_path_matches et
 * resolve_redirect sont tous `static` dans stdlib_http.c. Le seul
 * chemin d'entree reel est la native (meme raison que ).
 *
 * ============================= METHODE =============================
 *
 * 1. CONTROLE POSITIF D'ABORD. Un test « le cookie n'a pas fuite » est
 *    vert sur un client qui n'envoie jamais aucun cookie. F1 prouve
 *    donc d'abord, sur les octets recus par le pair, que le client
 *    RENVOIE le cookie pose, a l'hote et au chemin attendus. Sans
 *    cette preuve, toutes les assertions de non-fuite de F2..F6
 *    seraient vraies pour la mauvaise raison.
 *
 * 2. ON ASSERTE SUR CE QUE LE PAIR A RECU, pas sur ce que le client a
 *    rendu. Un 200 rendu par le client ne prouve pas quel hote a ete
 *    joint ni quels octets ont ete emis. C'est cette assertion-la qui
 *    a attrape le faux vert de  (no_proxy laisse dans
 *    l'environnement du processus par un self-test voisin faisait
 *    contourner le proxy : 200 parfait, zero ligne du code sous test).
 *
 * 3. ENVIRONNEMENT NEUTRALISE ET MESURE. Ce test tourne dans le meme
 *    processus, apres les memes voisins que . On vide les six
 *    variables de proxy a l'entree ET on asserte qu'elles sont vides au
 *    moment ou les familles tournent (f1_environnement_neutre), pour
 *    qu'aucun vert futur ne puisse s'expliquer par un bypass
 *    d'environnement.
 *
 * 4. LE JAR EST GLOBAL AU PROCESSUS et survit d'un test a l'autre :
 *    http_llhttp_integration_test.c y laisse a=1 et b=2 sur 127.0.0.1.
 *    Chaque scenario ouvre donc par HTTPVideCookies().
 *
 * ============================= FAMILLES ============================
 *
 *   F1 environnement + controle positif du round-trip cookie
 *   F2 appariement de domaine (fuite hote A -> hote B)
 *   F3 portee de chemin
 *   F4 attributs (Secure, HttpOnly, SameSite, suppression)
 *   F5 redirections (Location relative / absolue, port, plafond, boucle)
 *   F6 cookies a travers une redirection (meme hote et cross-host)
 *   F7 expiration (Max-Age, Expires, precedence, formats de date)
 *   F8 expiration DIFFEREE (echeance atteinte pendant la vie du
 *      processus, via l'horloge derivable du jar)
 *
 * Les cas ou le comportement mesure s'ecarte de la reference (RFC 6265
 * pour les cookies, RFC 7231 pour les redirections) sont assertes sur
 * le comportement REEL et marques « ECART MESURE » dans le commentaire
 * qui les precede : c'est une caracterisation, elle doit etre verte
 * contre le code actuel pour valoir quelque chose.
 *
 *  — deux de ces ecarts ont ete corriges, leurs assertions de
 * caracterisation sont donc INVERSEES (f2 §4, f3 frontiere, f6 §3) et
 * completees : application de la portee declaree, cote domaine (RFC
 * 6265 §5.3 etape 6 + §5.1.3) et cote chemin (§5.1.4). Les autres
 * ECART restent des caracterisations (Host sans port, Location
 * relative nue) et sont traites ailleurs.
 *
 *  — l'ECART « Max-Age/Expires ignores » est corrige a son
 * tour : son assertion de caracterisation (f4) est INVERSEE et la
 * famille F7 couvre l'expiration en propre.
 *
 *  — les deux derniers ECART tombent : le port dans l'entete
 * Host (f1, f5 §1 et §2, assertions INVERSEES) et la Location relative
 * sans '/' initial (f5 §3, INVERSEE). Les deux sens sont couverts : le
 * port ecrit doit apparaitre, un port par defaut absent de l'URL ne
 * doit pas etre invente (f1 §5, piege symetrique de ).
 *
 *  — F7 ne pouvait couvrir que des valeurs limites interceptees
 * a la POSE : le balayage des cookies expires ne portait donc aucun
 * test (mesure : le neutraliser laissait 137/137). F8 couvre l'echeance
 * atteinte pendant la vie du processus, horloge derivee, sans sleep.
 *
 * Execute via `owl --self-test-net` (cf. main.c), sous vm_init. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../vm.h"
#include "../owl_stdlib.h"   /* owl_http_test_horloge_decale — cf. F8 */
#include "fixture_unique.h"

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <io.h>
  typedef HANDLE jr_thread_t;
  typedef SOCKET jr_socket_t;
  #define JR_INVALID    INVALID_SOCKET
  #define JR_CLOSE      closesocket
  #define JR_SHUT_WR    SD_SEND
  #define JR_SLEEP(ms)  Sleep((DWORD)(ms))
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
  typedef pthread_t jr_thread_t;
  typedef int jr_socket_t;
  #define JR_INVALID    (-1)
  #define JR_CLOSE      close
  #define JR_SHUT_WR    SHUT_WR
  static void jr_sleep_ms(int ms) {
      struct timespec ts;
      ts.tv_sec  = ms / 1000;
      ts.tv_nsec = (long)(ms % 1000) * 1000L * 1000L;
      nanosleep(&ts, NULL);
  }
  #define JR_SLEEP(ms)  jr_sleep_ms((int)(ms))
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

#define JR_NB_FAMILLES 8

static int g_pass[JR_NB_FAMILLES];
static int g_fail[JR_NB_FAMILLES];
static int g_famille = 0;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < JR_NB_FAMILLES) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f + 1, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Serveur de test multi-tours
 *
 * net_mock_peer.c n'accepte qu'UNE connexion et emet UN payload : il
 * ne peut pas servir un enchainement (poser un cookie puis observer
 * la requete suivante, ou repondre a chaque maillon d'une chaine de
 * redirections). D'ou ce pair local : N connexions successives, une
 * reponse scriptee par tour, et les octets recus conserves TOUR PAR
 * TOUR — c'est le seul instrument qui dise ce que le client a
 * reellement mis sur le fil.
 * ================================================================ */

#define JR_MAX_TOURS   24
#define JR_MAX_RECU    4096
#define JR_MAX_REP     1024
#define JR_PLAFOND_CNX 64      /* garde-fou : un client emballe ne boucle pas */

typedef struct {
    int  port;
    int  pret;
    int  stop;
    int  demarre;

    char reponses[JR_MAX_TOURS][JR_MAX_REP];
    int  reponses_len[JR_MAX_TOURS];
    int  nb_reponses;
    int  rejoue_derniere;   /* 1 = resert la derniere reponse indefiniment */

    int  nb_servis;         /* connexions effectivement servies */
    char recu[JR_MAX_TOURS][JR_MAX_RECU];
    int  recu_len[JR_MAX_TOURS];

    jr_thread_t thread;
} Serveur;

static void jr_init(Serveur* s) {
    memset(s, 0, sizeof(*s));
}

/* Ajoute une reponse scriptee (tour suivant). */
static void jr_reponse(Serveur* s, const char* fmt, ...) {
    if (s->nb_reponses >= JR_MAX_TOURS) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->reponses[s->nb_reponses], JR_MAX_REP, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= JR_MAX_REP) n = JR_MAX_REP - 1;
    s->reponses_len[s->nb_reponses] = n;
    s->nb_reponses++;
}

static int jr_write_all(jr_socket_t sock, const char* buf, int len) {
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

/* Lit la requete jusqu'a la fin des entetes. Les requetes emises par ce
 * test sont sans corps (GET) : la fin des entetes est la fin de la
 * requete. */
static void jr_lit_requete(Serveur* s, jr_socket_t cli, int idx) {
    char* dst = (idx < JR_MAX_TOURS) ? s->recu[idx] : NULL;
    int   cap = JR_MAX_RECU - 1;
    int   len = 0;
    char  tmp[1024];
    while (1) {
        if (dst && len >= 4 && memcmp(dst + len - 4, "\r\n\r\n", 4) == 0) break;
        int n = recv(cli, tmp, (int)sizeof(tmp), 0);
        if (n <= 0) break;
        if (dst && len < cap) {
            int cp = n < cap - len ? n : cap - len;
            memcpy(dst + len, tmp, (size_t)cp);
            len += cp;
            dst[len] = '\0';
        }
    }
    if (idx < JR_MAX_TOURS) s->recu_len[idx] = len;
}

static void jr_sert(Serveur* s, jr_socket_t cli) {
    int un = 1;
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&un, sizeof(un));
#ifdef _WIN32
    DWORD tv = 3000;
#else
    struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
#endif
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    int idx = s->nb_servis;
    jr_lit_requete(s, cli, idx);

    int rep = -1;
    if (idx < s->nb_reponses)                        rep = idx;
    else if (s->rejoue_derniere && s->nb_reponses)   rep = s->nb_reponses - 1;

    if (rep >= 0) jr_write_all(cli, s->reponses[rep], s->reponses_len[rep]);
    else {
        static const char R404[] =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        jr_write_all(cli, R404, (int)sizeof(R404) - 1);
    }

    /* nb_servis n'est incremente qu'apres l'ecriture : le thread
     * principal ne doit jamais lire un tour a moitie enregistre. */
    s->nb_servis = idx + 1;

    shutdown(cli, JR_SHUT_WR);
    char poubelle[512];
    int garde = 0;
    while (garde++ < 32) {
        int n = recv(cli, poubelle, (int)sizeof(poubelle), 0);
        if (n <= 0) break;
    }
    JR_CLOSE(cli);
}

#ifdef _WIN32
static DWORD WINAPI jr_thread_fn(LPVOID arg) {
#else
static void* jr_thread_fn(void* arg) {
#endif
    Serveur* s = (Serveur*)arg;

    jr_socket_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv != JR_INVALID) {
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
                /* select() avant accept() : le thread doit pouvoir
                 * constater l'ordre d'arret entre deux connexions,
                 * sinon il resterait bloque dans accept() apres le
                 * dernier tour du scenario. */
                while (!s->stop && s->nb_servis < JR_PLAFOND_CNX) {
                    fd_set r;
                    FD_ZERO(&r);
                    FD_SET(srv, &r);
                    struct timeval att; att.tv_sec = 0; att.tv_usec = 100 * 1000;
                    if (select((int)srv + 1, &r, NULL, NULL, &att) <= 0) continue;
                    jr_socket_t cli = accept(srv, NULL, NULL);
                    if (cli == JR_INVALID) continue;
                    jr_sert(s, cli);
                }
            }
        }
        JR_CLOSE(srv);
    }
    s->pret = 1;   /* debloque l'attente meme en cas d'echec */
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static int jr_demarre(Serveur* s) {
#ifdef _WIN32
    s->thread = CreateThread(NULL, 0, jr_thread_fn, s, 0, NULL);
    if (s->thread == NULL) return 0;
#else
    if (pthread_create(&s->thread, NULL, jr_thread_fn, s) != 0) return 0;
#endif
    s->demarre = 1;
    int attendu = 0;
    while (!s->pret && attendu < 2000) { JR_SLEEP(10); attendu += 10; }
    return s->port > 0;
}

static void jr_arrete(Serveur* s) {
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

/* ================================================================
 * Lecture des octets recus (ce que le client a REELLEMENT emis)
 * ================================================================ */

static const char* jr_recu(const Serveur* s, int tour) {
    if (tour < 0 || tour >= JR_MAX_TOURS || tour >= s->nb_servis) return "";
    return s->recu[tour];
}

/* Ligne de requete du tour (ex "GET /x HTTP/1.1"). "" si absente. */
static void jr_ligne_requete(const Serveur* s, int tour, char* dst, size_t cap) {
    dst[0] = '\0';
    const char* r = jr_recu(s, tour);
    const char* fin = strstr(r, "\r\n");
    if (!fin) return;
    size_t n = (size_t)(fin - r);
    if (n >= cap) n = cap - 1;
    memcpy(dst, r, n);
    dst[n] = '\0';
}

/* Valeur de l'entete `nom` du tour, "" si l'entete est absent. La
 * distinction absent / present-vide compte ici : « pas d'entete Cookie
 * du tout » est le resultat attendu d'une non-fuite. */
static void jr_entete(const Serveur* s, int tour, const char* nom,
                      char* dst, size_t cap) {
    dst[0] = '\0';
    const char* r = jr_recu(s, tour);
    size_t nlen = strlen(nom);
    const char* p = strstr(r, "\r\n");
    if (!p) return;
    p += 2;
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n') return;       /* fin des entetes */
        const char* fin = strstr(p, "\r\n");
        if (!fin) return;
        if ((size_t)(fin - p) > nlen &&
#ifdef _WIN32
            _strnicmp(p, nom, nlen) == 0 &&
#else
            strncasecmp(p, nom, nlen) == 0 &&
#endif
            p[nlen] == ':') {
            const char* v = p + nlen + 1;
            while (v < fin && (*v == ' ' || *v == '\t')) v++;
            size_t n = (size_t)(fin - v);
            if (n >= cap) n = cap - 1;
            memcpy(dst, v, n);
            dst[n] = '\0';
            return;
        }
        p = fin + 2;
    }
}

/* 1 si un entete `nom` est present au tour donne. */
static int jr_a_entete(const Serveur* s, int tour, const char* nom) {
    char buf[512];
    jr_entete(s, tour, nom, buf, sizeof(buf));
    if (buf[0] != '\0') return 1;
    /* Entete present mais vide : le distinguer d'un entete absent. */
    char recherche[72];
    snprintf(recherche, sizeof(recherche), "\r\n%s:", nom);
    return strstr(jr_recu(s, tour), recherche) != NULL;
}

/* ================================================================
 * Capture stdout autour d'un vm_interpret
 * ================================================================ */

static char g_out[64 * 1024];
static int  g_seq = 0;

static InterpretResult capture_interpret(const char* source) {
    char TMP[128];
    fixture_unique(TMP, sizeof(TMP), "_owl_http_jar_test_", ".tmp");
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
 * Pilotage d'une requete reelle
 * ================================================================ */

/* Les parametres HTTP globaux (g_http_params) sont statiques dans
 * stdlib_http.c : ils survivent a vm_free(), donc au self-test VOISIN.
 * Mesure faite : sans cette remise a zero, TOUTES les
 * requetes de ce fichier echouaient avec « echec reseau (code status
 * 0) » et le pair local ne voyait AUCUNE connexion — un proxy laisse
 * par un self-test precedent detournait la connexion vers un port
 * mort. Le bloc f1_isolation_* verrouille ce constat par une mesure
 * differentielle. */
#define RESET_PARAMS "HTTPParametre(httpParamReinitialise, 0)\n"

typedef struct {
    int  interprete_ok;
    int  erreur;
    int  statut;
    char corps[2048];
    char url[512];
} Resultat;

/* `prelude` : instructions OWL jouees avant la requete (vidage du jar,
 * reglage des parametres). Ne doit declarer aucune globale — les
 * fichiers de test partagent la table des globales de la VM.
 *
 * `avec_reinit` a 0 : le verrouillage d'isolation N'EST PAS emis par ce
 * tour. Reserve a F8, qui doit l'emettre lui-meme AVANT d'armer
 * l'horloge de test (que httpParamReinitialise remet a zero). Partout
 * ailleurs il vaut 1. */
static Resultat requete_ex(const char* prelude, const char* url, int avec_reinit) {
    Resultat r;
    memset(&r, 0, sizeof(r));

    int id = ++g_seq;
    char src[1400];
    snprintf(src, sizeof(src),
        "%s%s"
        "r%d est une chaine = HTTPRequete(\"%s\")\n"
        /* ErreurDetectee se lit AVANT tout autre appel : la sentinelle
         * d'erreur ne survit qu'a un seul OP_CALL. */
        "e%d est un booleen = ErreurDetectee\n"
        "Trace(\"ERREUR=\" + e%d)\n"
        "Trace(\"STATUT=\" + HTTPDernierStatut())\n"
        "Trace(\"CORPS=[\" + r%d + \"]\")\n",
        avec_reinit ? RESET_PARAMS : "", prelude ? prelude : "",
        id, url, id, id, id);

    r.interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    r.erreur = lit_entier("ERREUR");
    r.statut = lit_entier("STATUT");
    lit_entre_crochets("CORPS", r.corps, sizeof(r.corps));
    return r;
}

static Resultat requete(const char* prelude, const char* url) {
    return requete_ex(prelude, url, 1);
}

/* Second pilote : passe par HTTPEnvoie / httpReponse, seule surface qui
 * porte l'URL FINALE apres redirections (httpReponse..URL) — le champ
 * exact ou  a vu le port se perdre. La native est appelee pour
 * de vrai, jamais mockee. */
static Resultat envoie(const char* prelude, const char* url) {
    Resultat r;
    memset(&r, 0, sizeof(r));

    int id = ++g_seq;
    char src[1400];
    snprintf(src, sizeof(src),
        RESET_PARAMS
        "%s"
        "q%d est un httpRequete\n"
        "q%d..URL = \"%s\"\n"
        "rep%d est un httpReponse = HTTPEnvoie(q%d)\n"
        "e%d est un booleen = ErreurDetectee\n"
        "Trace(\"ERREUR=\" + e%d)\n"
        "Trace(\"STATUT=\" + HTTPDernierStatut())\n"
        "Trace(\"URL=[\" + rep%d..URL + \"]\")\n"
        "Trace(\"CORPS=[\" + rep%d..Contenu + \"]\")\n",
        prelude ? prelude : "", id, id, url, id, id, id, id, id, id);

    r.interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    r.erreur = lit_entier("ERREUR");
    r.statut = lit_entier("STATUT");
    lit_entre_crochets("URL",   r.url,   sizeof(r.url));
    lit_entre_crochets("CORPS", r.corps, sizeof(r.corps));
    return r;
}

/* Reponse 200 minimale portant un corps identifiable. */
static void rep_200(Serveur* s, const char* corps) {
    jr_reponse(s,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n%s", (int)strlen(corps), corps);
}

static void rep_200_cookie(Serveur* s, const char* set_cookie, const char* corps) {
    jr_reponse(s,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Set-Cookie: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n%s", set_cookie, (int)strlen(corps), corps);
}

static void rep_302(Serveur* s, const char* location) {
    jr_reponse(s,
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n", location);
}

static void rep_302_cookie(Serveur* s, const char* set_cookie, const char* location) {
    jr_reponse(s,
        "HTTP/1.1 302 Found\r\n"
        "Set-Cookie: %s\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n", set_cookie, location);
}

#define VIDE_JAR "HTTPVideCookies()\n"

/* ================================================================
 * F1 — environnement + CONTROLE POSITIF
 * ================================================================ */

/* Sans cette famille, tout le reste est vrai pour la mauvaise raison. */
static void famille_f1(void) {
    g_famille = 0;

    /* --- 1. L'environnement du processus ne peut pas expliquer un vert.
     * Mesure directe, pas deduction :  a montre qu'un no_proxy
     * pose par un self-test voisin rendait un 200 sans executer une
     * ligne du code sous test. */
    {
        const char* v;
        v = getenv("http_proxy");  CHECK(v == NULL || v[0] == '\0', "f1_env_http_proxy_vide");
        v = getenv("HTTP_PROXY");  CHECK(v == NULL || v[0] == '\0', "f1_env_HTTP_PROXY_vide");
        v = getenv("no_proxy");    CHECK(v == NULL || v[0] == '\0', "f1_env_no_proxy_vide");
        v = getenv("NO_PROXY");    CHECK(v == NULL || v[0] == '\0', "f1_env_NO_PROXY_vide");
    }

    /* --- 1bis. L'environnement n'est pas seulement fait de variables :
     * g_http_params est un statique de stdlib_http.c qui survit a
     * vm_free(), donc au self-test voisin. Mesure differentielle, une
     * seule variable changee : MEME url, MEME pair.
     *   - avec le proxy laisse en place -> echec reseau, le pair ne voit
     *     RIEN (c'est l'etat dans lequel ce fichier a demarre) ;
     *   - apres httpParamReinitialise -> 200 et le pair voit la
     *     connexion.
     * Sans cette preuve, le vert de tout le fichier reposerait sur une
     * remise a zero dont personne ne saurait dire si elle sert. */
    {
        Serveur iso; jr_init(&iso);
        rep_200(&iso, "isole");
        rep_200(&iso, "isole2");
        if (!jr_demarre(&iso)) { CHECK(0, "f1_iso_serveur"); return; }

        char u[128];
        snprintf(u, sizeof(u), "http://127.0.0.1:%d/iso", iso.port);

        /* Proxy pointe sur un port ou personne n'ecoute. */
        Resultat avec_proxy = requete(
            "HTTPParametre(httpParamProxy, \"http://127.0.0.1:1\")\n", u);
        CHECK(avec_proxy.erreur == 1, "f1_isolation_proxy_actif_echec_reseau");
        CHECK(iso.nb_servis == 0, "f1_isolation_proxy_actif_pair_jamais_joint");

        /* requete() emet httpParamReinitialise en tete : le proxy pose
         * juste au-dessus ne survit pas au tour suivant. */
        Resultat sans_proxy = requete("", u);
        CHECK(sans_proxy.statut == 200, "f1_isolation_apres_reinit_statut_200");
        CHECK(iso.nb_servis == 1, "f1_isolation_apres_reinit_pair_joint");
        jr_arrete(&iso);
    }

    /* --- 2. Le pair recoit bien une requete directe (forme origine).
     * Via un proxy, la ligne de requete porterait l'URL absolue et
     * partirait sur un autre port : le pair ne verrait rien. */
    Serveur s; jr_init(&s);
    rep_200_cookie(&s, "sid=ABC123; Path=/", "un");
    rep_200(&s, "deux");
    if (!jr_demarre(&s)) { CHECK(0, "f1_serveur_demarre"); return; }
    CHECK(1, "f1_serveur_demarre");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", s.port);
    Resultat r1 = requete(VIDE_JAR, url);

    CHECK(r1.interprete_ok, "f1_interprete_ok");
    CHECK(r1.erreur == 0,   "f1_pas_erreur");
    CHECK(r1.statut == 200, "f1_statut_200");
    CHECK(strcmp(r1.corps, "un") == 0, "f1_corps_exact");
    CHECK(s.nb_servis == 1, "f1_une_connexion_servie");

    char ligne[256], hote[256], cookie[512];
    jr_ligne_requete(&s, 0, ligne, sizeof(ligne));
    CHECK(strcmp(ligne, "GET /x HTTP/1.1") == 0, "f1_ligne_requete_forme_origine");

    jr_entete(&s, 0, "Host", hote, sizeof(hote));
    /*  — l'entete Host porte le port des lors qu'il figurait
     * dans l'URL (RFC 7230 §5.4 : « host [ ":" port ] »). C'etait la
     * moitie que  n'avait pas traitee : la reconstruction
     * d'URL conservait le port, l'entete emis non. Un pair qui route
     * sur Host (hote virtuel par port) recevait une valeur fausse.
     * Le port ephemere du pair n'est jamais 80 : l'attendu se construit
     * a partir du port reellement obtenu. */
    {
        char attendu_hote[280];
        snprintf(attendu_hote, sizeof(attendu_hote), "127.0.0.1:%d", s.port);
        CHECK(strcmp(hote, attendu_hote) == 0, "f1_host_porte_le_port_explicite");
    }

    /* Jar vide au depart : aucun Cookie sur la 1re requete. */
    CHECK(!jr_a_entete(&s, 0, "Cookie"), "f1_pas_de_cookie_sur_la_1re_requete");

    /* --- 3. CONTROLE POSITIF : le cookie pose au tour 1 revient au
     * tour 2, valeur exacte, sur le meme hote et un chemin couvert. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x/suite", s.port);
    Resultat r2 = requete("", url);
    CHECK(r2.statut == 200, "f1_2e_requete_statut_200");
    CHECK(strcmp(r2.corps, "deux") == 0, "f1_2e_corps_exact");
    CHECK(s.nb_servis == 2, "f1_deux_connexions_servies");

    jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "sid=ABC123") == 0, "f1_CONTROLE_POSITIF_cookie_renvoye_exact");

    jr_arrete(&s);

    /* --- 4. HTTPVideCookies() coupe effectivement le renvoi : sans ca,
     * le vidage entre scenarios ne prouverait rien. */
    Serveur s2; jr_init(&s2);
    rep_200(&s2, "trois");
    if (!jr_demarre(&s2)) { CHECK(0, "f1_serveur2_demarre"); return; }
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/x", s2.port);
    Resultat r3 = requete(VIDE_JAR, url);
    CHECK(r3.statut == 200, "f1_apres_vidage_statut_200");
    CHECK(!jr_a_entete(&s2, 0, "Cookie"), "f1_apres_vidage_plus_de_cookie");
    jr_arrete(&s2);

    /* --- 5. PIEGE SYMETRIQUE (, repris a l'identique) :
     * conserver le port ne doit pas revenir a en inventer un. Une URL
     * sans port doit emettre un Host nu, jamais « hote:80 ».
     *
     * Un pair de test ecoute toujours sur un port ephemere : une URL
     * SANS port ne peut donc pas etre observee en direct (la connexion
     * partirait sur le port 80, ou personne n'ecoute, et aucun octet ne
     * serait emis). Le proxy est le seul instrument qui rende ces
     * octets-la observables : le client se connecte au pair (qui joue
     * le proxy) et y emet la requete destinee a un hote quelconque. Le
     * pair recoit donc pour de vrai la ligne de requete et l'entete
     * Host d'une requete vers une URL sans port.
     *
     * CONTROLE POSITIF de l'instrument : sans les deux assertions sur
     * la forme absolue et sur le nombre de tours servis, un « Host sans
     * :80 » serait vrai pour un client qui n'a rien emis du tout. */
    {
        Serveur px; jr_init(&px);
        rep_200(&px, "px1");
        rep_200(&px, "px2");
        rep_200(&px, "px3");
        if (!jr_demarre(&px)) { CHECK(0, "f1_host_proxy_serveur"); return; }

        char prelude[192];
        snprintf(prelude, sizeof(prelude),
                 VIDE_JAR "HTTPParametre(httpParamProxy, \"http://127.0.0.1:%d\")\n",
                 px.port);

        /* 5a. URL sans port -> Host nu. */
        Resultat a = requete(prelude, "http://exemple.test/nu");
        CHECK(a.statut == 200, "f1_host_proxy_statut_200");
        CHECK(px.nb_servis == 1, "f1_host_proxy_pair_joint");
        jr_ligne_requete(&px, 0, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET http://exemple.test/nu HTTP/1.1") == 0,
              "f1_host_proxy_forme_absolue");
        jr_entete(&px, 0, "Host", hote, sizeof(hote));
        CHECK(strcmp(hote, "exemple.test") == 0,
              "f1_host_url_sans_port_reste_nu");

        /* 5b. Port explicite non standard -> present dans Host. */
        Resultat b = requete(prelude, "http://exemple.test:8081/p");
        CHECK(b.statut == 200, "f1_host_proxy_statut_200_port_explicite");
        CHECK(px.nb_servis == 2, "f1_host_proxy_deuxieme_tour_servi");
        jr_entete(&px, 1, "Host", hote, sizeof(hote));
        CHECK(strcmp(hote, "exemple.test:8081") == 0,
              "f1_host_url_avec_port_le_porte");

        /* 5c. Port par defaut ECRIT dans l'URL : restitue tel quel —
         * on ne normalise pas, exactement comme build_url_finale
         * ( : http://hote:80/x rend http://hote:80/x). */
        Resultat c = requete(prelude, "http://exemple.test:80/p");
        CHECK(c.statut == 200, "f1_host_proxy_statut_200_port_defaut");
        CHECK(px.nb_servis == 3, "f1_host_proxy_troisieme_tour_servi");
        jr_entete(&px, 2, "Host", hote, sizeof(hote));
        CHECK(strcmp(hote, "exemple.test:80") == 0,
              "f1_host_port_defaut_ecrit_restitue");

        jr_arrete(&px);
    }
}

/* ================================================================
 * F2 — appariement de domaine
 * ================================================================ */

static void famille_f2(void) {
    g_famille = 1;

    /* --- 1. Cookie pose par 127.0.0.1 : jamais envoye a localhost.
     * Meme machine, meme boucle locale, deux noms d'hote distincts —
     * c'est exactement le cas « hote A / hote B » sans dependre d'un
     * DNS externe. */
    Serveur a; jr_init(&a);
    rep_200_cookie(&a, "sessA=SECRET_A; Path=/", "A");
    if (!jr_demarre(&a)) { CHECK(0, "f2_serveur_a_demarre"); return; }

    Serveur b; jr_init(&b);
    rep_200(&b, "B");
    if (!jr_demarre(&b)) { CHECK(0, "f2_serveur_b_demarre"); jr_arrete(&a); return; }

    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", a.port);
    Resultat ra = requete(VIDE_JAR, url);
    CHECK(ra.statut == 200, "f2_pose_sur_A_statut_200");

    snprintf(url, sizeof(url), "http://localhost:%d/", b.port);
    Resultat rb = requete("", url);
    CHECK(rb.statut == 200, "f2_appel_B_statut_200");
    CHECK(b.nb_servis == 1, "f2_B_a_bien_ete_joint");
    CHECK(!jr_a_entete(&b, 0, "Cookie"), "f2_cookie_de_A_absent_chez_B");
    CHECK(strstr(jr_recu(&b, 0), "SECRET_A") == NULL, "f2_aucun_octet_de_A_chez_B");

    jr_arrete(&a);
    jr_arrete(&b);

    /* --- 2. Sens inverse (localhost pose, 127.0.0.1 appelle). Un
     * appariement asymetrique passerait au travers du seul sens 1. */
    Serveur c; jr_init(&c);
    rep_200_cookie(&c, "sessB=SECRET_B; Path=/", "C");
    if (!jr_demarre(&c)) { CHECK(0, "f2_serveur_c_demarre"); return; }
    Serveur d; jr_init(&d);
    rep_200(&d, "D");
    if (!jr_demarre(&d)) { CHECK(0, "f2_serveur_d_demarre"); jr_arrete(&c); return; }

    snprintf(url, sizeof(url), "http://localhost:%d/", c.port);
    requete(VIDE_JAR, url);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", d.port);
    Resultat rd = requete("", url);
    CHECK(rd.statut == 200, "f2_inverse_statut_200");
    CHECK(!jr_a_entete(&d, 0, "Cookie"), "f2_inverse_pas_de_fuite");

    jr_arrete(&c);
    jr_arrete(&d);

    /* --- 3. Le port ne separe PAS deux origines pour le jar (conforme
     * RFC 6265 §8.5 : les cookies ignorent le port). Meme hote, autre
     * port -> le cookie suit. Mesure, pas supposition. */
    Serveur e; jr_init(&e);
    rep_200_cookie(&e, "porte=P; Path=/", "E");
    if (!jr_demarre(&e)) { CHECK(0, "f2_serveur_e_demarre"); return; }
    Serveur f; jr_init(&f);
    rep_200(&f, "F");
    if (!jr_demarre(&f)) { CHECK(0, "f2_serveur_f_demarre"); jr_arrete(&e); return; }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", e.port);
    requete(VIDE_JAR, url);
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", f.port);
    requete("", url);
    char cookie[512];
    jr_entete(&f, 0, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "porte=P") == 0, "f2_cookie_partage_entre_ports_du_meme_hote");

    jr_arrete(&e);
    jr_arrete(&f);

    /* --- 4. Domain qui ne couvre pas l'hote emetteur : cookie REJETE.
     * RFC 6265 §5.3 etape 6 : si l'attribut Domain est non vide et que
     * l'hote emetteur ne domain-match pas cette valeur, l'agent
     * utilisateur « ignore le cookie ENTIEREMENT et abandonne ». Il ne
     * le degrade PAS en cookie host-only — nuance qui se mesure : on
     * verifie l'absence chez l'autre hote ET chez l'emetteur lui-meme,
     * sinon un stockage silencieux sous 127.0.0.1 passerait pour un
     * rejet. Ici 127.0.0.1 pose Domain=localhost : injection de cookie
     * cross-hote, exploitable par redirection (cf. f6 §3). */
    Serveur g; jr_init(&g);
    rep_200_cookie(&g, "injecte=X; Domain=localhost; Path=/", "G");
    rep_200(&g, "G2");
    if (!jr_demarre(&g)) { CHECK(0, "f2_serveur_g_demarre"); return; }
    Serveur h; jr_init(&h);
    rep_200(&h, "H");
    if (!jr_demarre(&h)) { CHECK(0, "f2_serveur_h_demarre"); jr_arrete(&g); return; }

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g.port);
    requete(VIDE_JAR, url);
    snprintf(url, sizeof(url), "http://localhost:%d/", h.port);
    requete("", url);
    CHECK(!jr_a_entete(&h, 0, "Cookie"),
          "f2_domain_non_couvrant_rien_chez_l_hote_vise");
    CHECK(strstr(jr_recu(&h, 0), "injecte") == NULL,
          "f2_domain_non_couvrant_aucun_octet_chez_l_hote_vise");

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", g.port);
    requete("", url);
    CHECK(!jr_a_entete(&g, 1, "Cookie"),
          "f2_domain_non_couvrant_rejet_total_meme_chez_l_emetteur");

    jr_arrete(&g);
    jr_arrete(&h);

    /* --- 5. CONTROLE POSITIF du chemin d'acceptation : un Domain egal a
     * l'hote emetteur (RFC 6265 §5.1.3, 1er cas — chaines identiques)
     * reste accepte et renvoye. Sans lui, le vert de §4 serait aussi
     * celui d'un client qui jette TOUT Set-Cookie portant un Domain. */
    {
        Serveur i; jr_init(&i);
        rep_200_cookie(&i, "legit=L; Domain=localhost; Path=/", "I");
        rep_200(&i, "I2");
        if (!jr_demarre(&i)) { CHECK(0, "f2_serveur_i_demarre"); return; }

        snprintf(url, sizeof(url), "http://localhost:%d/", i.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&i, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "legit=L") == 0,
              "f2_domain_identique_a_l_hote_accepte_et_renvoye");
        jr_arrete(&i);
    }

    /* --- 6. Meme controle positif sur un litteral IP : Domain=127.0.0.1
     * pose par 127.0.0.1 est le cas « chaines identiques », valide meme
     * pour une adresse. */
    {
        Serveur j; jr_init(&j);
        rep_200_cookie(&j, "ip=IP1; Domain=127.0.0.1; Path=/", "J");
        rep_200(&j, "J2");
        if (!jr_demarre(&j)) { CHECK(0, "f2_serveur_j_demarre"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", j.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&j, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "ip=IP1") == 0,
              "f2_domain_egal_au_litteral_ip_accepte");
        jr_arrete(&j);
    }

    /* --- 7. Suffixe d'un litteral IP : REJETE. La regle de suffixe de
     * RFC 6265 §5.1.3 ne s'applique qu'a un NOM d'hote (« the string is
     * a host name (i.e., not an IP address) »). 127.0.0.1 se termine
     * bien par « .0.0.1 », mais 0.0.1 ne designe rien : un appariement
     * purement textuel ferait d'un octet de l'adresse un « domaine
     * parent ». */
    {
        Serveur k; jr_init(&k);
        rep_200_cookie(&k, "susp=S; Domain=0.0.1; Path=/", "K");
        rep_200(&k, "K2");
        if (!jr_demarre(&k)) { CHECK(0, "f2_serveur_k_demarre"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", k.port);
        requete(VIDE_JAR, url);
        requete("", url);
        CHECK(!jr_a_entete(&k, 1, "Cookie"),
              "f2_domain_suffixe_d_un_litteral_ip_rejete");
        jr_arrete(&k);
    }

    /* --- 8. Suffixe d'un nom d'hote sans frontiere de point : REJETE.
     * « ocalhost » est bien un suffixe textuel de « localhost », mais
     * RFC 6265 §5.1.3 exige que le caractere qui precede le suffixe
     * dans l'hote soit un '.'. C'est la frontiere du cote domaine,
     * jumelle de celle du cote chemin (f3). */
    {
        Serveur l; jr_init(&l);
        rep_200_cookie(&l, "part=P2; Domain=ocalhost; Path=/", "L");
        rep_200(&l, "L2");
        if (!jr_demarre(&l)) { CHECK(0, "f2_serveur_l_demarre"); return; }

        snprintf(url, sizeof(url), "http://localhost:%d/", l.port);
        requete(VIDE_JAR, url);
        requete("", url);
        CHECK(!jr_a_entete(&l, 1, "Cookie"),
              "f2_domain_suffixe_sans_frontiere_de_point_rejete");
        jr_arrete(&l);
    }
}

/* ================================================================
 * F3 — portee de chemin
 * ================================================================ */

static void famille_f3(void) {
    g_famille = 2;

    Serveur s; jr_init(&s);
    rep_200_cookie(&s, "scope=S; Path=/admin", "pose");
    rep_200(&s, "t1");   /* /admin        -> couvert           */
    rep_200(&s, "t2");   /* /admin/sous   -> couvert           */
    rep_200(&s, "t3");   /* /autre        -> hors portee       */
    rep_200(&s, "t4");   /* /             -> hors portee       */
    rep_200(&s, "t5");   /* /administrateur -> frontiere       */
    if (!jr_demarre(&s)) { CHECK(0, "f3_serveur_demarre"); return; }

    char url[160], cookie[512];

    snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin", s.port);
    Resultat r0 = requete(VIDE_JAR, url);
    CHECK(r0.statut == 200, "f3_pose_statut_200");

    /* Chemin exact. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin", s.port);
    requete("", url);
    jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "scope=S") == 0, "f3_chemin_exact_cookie_envoye");

    /* Sous-chemin. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin/sous", s.port);
    requete("", url);
    jr_entete(&s, 2, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "scope=S") == 0, "f3_sous_chemin_cookie_envoye");

    /* Chemin frere : hors portee. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/autre", s.port);
    requete("", url);
    CHECK(!jr_a_entete(&s, 3, "Cookie"), "f3_chemin_frere_pas_de_cookie");

    /* Racine : hors portee d'un cookie Path=/admin. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
    requete("", url);
    CHECK(!jr_a_entete(&s, 4, "Cookie"), "f3_racine_pas_de_cookie");

    /* Frontiere de chemin. RFC 6265 §5.1.4 n'accepte le prefixe que si
     * le chemin est identique, si le cookie-path finit par '/', ou si
     * le premier caractere du chemin de requete au-dela du prefixe est
     * un '/'. Sans cette derniere condition, /admin couvrirait
     * /administrateur, qui est une AUTRE ressource. */
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/administrateur", s.port);
    requete("", url);
    CHECK(!jr_a_entete(&s, 5, "Cookie"),
          "f3_frontiere_admin_ne_couvre_pas_administrateur");
    CHECK(strstr(jr_recu(&s, 5), "scope") == NULL,
          "f3_frontiere_aucun_octet_du_cookie_sur_administrateur");

    jr_arrete(&s);

    /* --- La frontiere se juge sur le CHEMIN, pas sur l'URL brute : la
     * chaine de requete n'en fait pas partie (RFC 6265 §5.4 apparie le
     * « request-path », composant chemin de l'URI). Un appariement
     * naif qui compterait le '?' comme un caractere de chemin
     * refuserait /admin?x=1 — regression facile a introduire en posant
     * la frontiere. */
    {
        Serveur q; jr_init(&q);
        rep_200_cookie(&q, "sc=Q; Path=/admin", "pose");
        rep_200(&q, "u1");   /* /admin?x=1          -> couvert     */
        rep_200(&q, "u2");   /* /admin/x?y=2        -> couvert     */
        rep_200(&q, "u3");   /* /administrateur?x=1 -> hors portee */
        if (!jr_demarre(&q)) { CHECK(0, "f3_query_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin", q.port);
        requete(VIDE_JAR, url);

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin?x=1", q.port);
        requete("", url);
        jr_entete(&q, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "sc=Q") == 0, "f3_chemin_exact_avec_chaine_de_requete");

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin/x?y=2", q.port);
        requete("", url);
        jr_entete(&q, 2, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "sc=Q") == 0, "f3_sous_chemin_avec_chaine_de_requete");

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/administrateur?x=1", q.port);
        requete("", url);
        CHECK(!jr_a_entete(&q, 3, "Cookie"),
              "f3_frontiere_tient_malgre_la_chaine_de_requete");
        jr_arrete(&q);
    }

    /* --- Cookie-path terminant par '/' : le 2e cas de RFC 6265 §5.1.4.
     * /admin/ couvre /admin/ et /admin/x, mais pas /adminx ni /admin
     * (plus court que le cookie-path). */
    {
        Serveur t; jr_init(&t);
        rep_200_cookie(&t, "sl=T; Path=/admin/", "pose");
        rep_200(&t, "v1");   /* /admin/x -> couvert     */
        rep_200(&t, "v2");   /* /adminx  -> hors portee */
        rep_200(&t, "v3");   /* /admin   -> hors portee */
        if (!jr_demarre(&t)) { CHECK(0, "f3_slash_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin/", t.port);
        requete(VIDE_JAR, url);

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin/x", t.port);
        requete("", url);
        jr_entete(&t, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "sl=T") == 0, "f3_cookie_path_slash_couvre_le_sous_chemin");

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/adminx", t.port);
        requete("", url);
        CHECK(!jr_a_entete(&t, 2, "Cookie"), "f3_cookie_path_slash_ne_couvre_pas_adminx");

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin", t.port);
        requete("", url);
        CHECK(!jr_a_entete(&t, 3, "Cookie"), "f3_cookie_path_slash_ne_couvre_pas_admin_nu");
        jr_arrete(&t);
    }
}

/* ================================================================
 * F4 — attributs
 * ================================================================ */

static void famille_f4(void) {
    g_famille = 3;

    char url[160], cookie[512];

    /* --- Secure : pose sur http, jamais renvoye sur http. Conforme :
     * cookie_jar_build filtre sur is_https. Le cookie EST stocke (il
     * partirait sur une origine https) mais ne fuit pas en clair. */
    {
        Serveur s; jr_init(&s);
        rep_200_cookie(&s, "sec=S1; Path=/; Secure", "pose");
        rep_200(&s, "t1");
        if (!jr_demarre(&s)) { CHECK(0, "f4_secure_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        CHECK(!jr_a_entete(&s, 1, "Cookie"), "f4_secure_pas_renvoye_en_clair");
        jr_arrete(&s);
    }

    /* --- HttpOnly : attribut ignore par le jar. Sans surface DOM cote
     * OWL, l'attribut n'a pas de sens observable : le cookie reste
     * stocke et renvoye. Caracterisation, pas defaut. */
    {
        Serveur s; jr_init(&s);
        rep_200_cookie(&s, "ho=H1; Path=/; HttpOnly", "pose");
        rep_200(&s, "t1");
        if (!jr_demarre(&s)) { CHECK(0, "f4_httponly_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "ho=H1") == 0, "f4_httponly_ignore_cookie_renvoye");
        jr_arrete(&s);
    }

    /* --- SameSite : attribut ignore. Sans notion de requete « tierce »
     * cote client HTTP programmatique, l'attribut n'a pas de porteur.
     * Caracterise pour que le jour ou il en aura un, le changement soit
     * visible. */
    {
        Serveur s; jr_init(&s);
        rep_200_cookie(&s, "ss=S2; Path=/; SameSite=Strict", "pose");
        rep_200(&s, "t1");
        if (!jr_demarre(&s)) { CHECK(0, "f4_samesite_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "ss=S2") == 0, "f4_samesite_ignore_cookie_renvoye");
        jr_arrete(&s);
    }

    /* --- Suppression par valeur vide : Set-Cookie: nom=; ... retire le
     * cookie du jar. C'est le seul mecanisme d'effacement (le jar est
     * en memoire, sans expiration). */
    {
        Serveur s; jr_init(&s);
        rep_200_cookie(&s, "tmp=T1; Path=/", "pose");
        rep_200_cookie(&s, "tmp=; Path=/", "efface");
        rep_200(&s, "t2");
        if (!jr_demarre(&s)) { CHECK(0, "f4_suppr_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "tmp=T1") == 0, "f4_suppr_present_avant_effacement");
        requete("", url);
        CHECK(!jr_a_entete(&s, 2, "Cookie"), "f4_suppr_absent_apres_effacement");
        jr_arrete(&s);
    }

    /* --- Max-Age=0 est la forme d'effacement la plus repandue cote
     * serveurs. L'assertion qui vivait ici caracterisait l'ECART (les
     * deux attributs ignores, le cookie continuait de partir) ; 
     * l'INVERSE. Le detail de l'expiration est en F7. */
    {
        Serveur s; jr_init(&s);
        rep_200_cookie(&s, "ma=M1; Path=/", "pose");
        rep_200_cookie(&s, "ma=M1; Path=/; Max-Age=0", "expire");
        rep_200(&s, "t2");
        if (!jr_demarre(&s)) { CHECK(0, "f4_maxage_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        requete("", url);
        CHECK(!jr_a_entete(&s, 2, "Cookie"),
              "f4_max_age_0_efface_le_cookie");
        jr_arrete(&s);
    }

    /* --- Plusieurs cookies applicables : un seul entete Cookie,
     * valeurs separees par "; " (RFC 6265 §5.4). */
    {
        Serveur s; jr_init(&s);
        jr_reponse(&s,
            "HTTP/1.1 200 OK\r\n"
            "Set-Cookie: c1=1; Path=/\r\n"
            "Set-Cookie: c2=2; Path=/\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\npose");
        rep_200(&s, "t1");
        if (!jr_demarre(&s)) { CHECK(0, "f4_multi_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "c1=1; c2=2") == 0, "f4_deux_cookies_un_seul_entete");
        jr_arrete(&s);
    }
}

/* ================================================================
 * F5 — redirections
 * ================================================================ */

static void famille_f5(void) {
    g_famille = 4;

    char url[160], ligne[256], hote[256];

    /* --- 1. Location relative absolue-path : meme hote, meme port,
     * chemin remplace. Le pair voit les DEUX requetes. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/suite");
        rep_200(&s, "arrivee");
        if (!jr_demarre(&s)) { CHECK(0, "f5_rel_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/depart", s.port);
        Resultat r = envoie(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_rel_statut_final_200");
        CHECK(strcmp(r.corps, "arrivee") == 0, "f5_rel_corps_de_l_arrivee");
        CHECK(s.nb_servis == 2, "f5_rel_deux_connexions");

        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /suite HTTP/1.1") == 0, "f5_rel_2e_chemin_resolu");

        /* Le Host emis APRES le saut porte lui aussi le port :
         * c'est la ou l'URL est reconstruite, donc la ou une perte
         * passerait le plus facilement inapercue. */
        jr_entete(&s, 1, "Host", hote, sizeof(hote));
        {
            char attendu_hote[280];
            snprintf(attendu_hote, sizeof(attendu_hote), "127.0.0.1:%d", s.port);
            CHECK(strcmp(hote, attendu_hote) == 0, "f5_rel_2e_host_porte_le_port");
        }

        char attendu_url[160];
        snprintf(attendu_url, sizeof(attendu_url), "http://127.0.0.1:%d/suite", s.port);
        CHECK(strcmp(r.url, attendu_url) == 0, "f5_rel_url_finale_avec_port");
        jr_arrete(&s);
    }

    /* --- 2. Location absolue vers un AUTRE port. Le port est la partie
     * que  a vue se perdre ailleurs dans la reconstruction
     * d'URL : on l'asserte sur le Host recu par le second pair, donc
     * sur le fil, et pas seulement sur la chaine rendue. */
    {
        Serveur s1; jr_init(&s1);
        Serveur s2; jr_init(&s2);
        rep_200(&s2, "absolue");
        if (!jr_demarre(&s2)) { CHECK(0, "f5_abs_serveur2"); return; }

        char loc[160];
        snprintf(loc, sizeof(loc), "http://127.0.0.1:%d/cible", s2.port);
        rep_302(&s1, loc);
        if (!jr_demarre(&s1)) { CHECK(0, "f5_abs_serveur1"); jr_arrete(&s2); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/depart", s1.port);
        Resultat r = envoie(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_abs_statut_final_200");
        CHECK(strcmp(r.corps, "absolue") == 0, "f5_abs_corps_du_2e_pair");
        CHECK(s1.nb_servis == 1 && s2.nb_servis == 1, "f5_abs_un_tour_par_pair");

        jr_ligne_requete(&s2, 0, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /cible HTTP/1.1") == 0, "f5_abs_chemin_resolu");

        /* Le saut a bien atterri sur le port de la cible — c'est le
         * pair s2 qui a recu la connexion. L'entete Host doit porter le
         * port de la CIBLE (celui de la Location), pas celui du depart :
         * un Host recopie du depart serait vert sur un simple « contient
         * un port », d'ou l'attendu exact. */
        jr_entete(&s2, 0, "Host", hote, sizeof(hote));
        {
            char attendu_hote[280];
            snprintf(attendu_hote, sizeof(attendu_hote), "127.0.0.1:%d", s2.port);
            CHECK(strcmp(hote, attendu_hote) == 0, "f5_abs_host_porte_le_port_de_la_cible");
        }

        char attendu_url[160];
        snprintf(attendu_url, sizeof(attendu_url), "http://127.0.0.1:%d/cible", s2.port);
        CHECK(strcmp(r.url, attendu_url) == 0, "f5_abs_url_finale_avec_port_cible");

        jr_arrete(&s1);
        jr_arrete(&s2);
    }

    /* --- 3. Location relative SANS '/' initial : resolue par rapport au
     * REPERTOIRE du chemin courant (RFC 3986 §5.2.2), pas par rapport a
     * la racine. /dossier/page + "voisin" -> /dossier/voisin ( ;
     * l'ancien comportement rendait /voisin, silencieusement faux). */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "voisin");
        rep_200(&s, "relatif");
        if (!jr_demarre(&s)) { CHECK(0, "f5_relnu_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/dossier/page", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_relnu_statut_200");
        CHECK(strcmp(r.corps, "relatif") == 0, "f5_relnu_corps_de_l_arrivee");
        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /dossier/voisin HTTP/1.1") == 0,
              "f5_relnu_resolue_dans_le_repertoire_courant");
        jr_arrete(&s);
    }

    /* --- 3bis. Le cas enracine ne bouge pas : depuis le MEME chemin de
     * depart, "/voisin" reste /voisin. Sans cette contre-mesure, une
     * implementation qui prefixerait tout par le repertoire courant
     * serait verte sur 3. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/voisin");
        rep_200(&s, "enracine");
        if (!jr_demarre(&s)) { CHECK(0, "f5_relracine_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/dossier/page", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_relracine_statut_200");
        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /voisin HTTP/1.1") == 0,
              "f5_relracine_reste_a_la_racine");
        jr_arrete(&s);
    }

    /* --- 3ter. Repertoire imbrique + segment « .. » : /a/b/page +
     * "../c" -> /a/c (RFC 3986 §5.2.4). Une resolution qui se
     * contenterait de concatener emettrait /a/b/../c sur le fil. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "../c");
        rep_200(&s, "remonte");
        if (!jr_demarre(&s)) { CHECK(0, "f5_relpoint_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/a/b/page", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_relpoint_statut_200");
        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /a/c HTTP/1.1") == 0,
              "f5_relpoint_segment_point_point_resolu");
        jr_arrete(&s);
    }

    /* --- 3quater. La chaine de requete du chemin de depart n'entre pas
     * dans la base de resolution (RFC 3986 §5.2.2 : la base est le
     * chemin, pas la requete). /dossier/page?a=1 + "voisin" ->
     * /dossier/voisin, sans ?a=1 recolle. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "voisin");
        rep_200(&s, "sansquery");
        if (!jr_demarre(&s)) { CHECK(0, "f5_relquery_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/dossier/page?a=1", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f5_relquery_statut_200");
        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strcmp(ligne, "GET /dossier/voisin HTTP/1.1") == 0,
              "f5_relquery_base_amputee_de_la_query");
        jr_arrete(&s);
    }

    /* --- 4. 302 sur une requete GET : methode conservee GET (RFC 7231
     * §6.4.3). Verifie sur la ligne de requete recue. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/apres");
        rep_200(&s, "ok");
        if (!jr_demarre(&s)) { CHECK(0, "f5_methode_serveur"); return; }
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/avant", s.port);
        requete(VIDE_JAR, url);
        jr_ligne_requete(&s, 1, ligne, sizeof(ligne));
        CHECK(strncmp(ligne, "GET ", 4) == 0, "f5_302_methode_get_conservee");
        jr_arrete(&s);
    }

    /* --- 5. Plafond de redirections : boucle coupee. Le pair resert
     * indefiniment le meme 302 vers lui-meme. Le nombre de connexions
     * servies EST la mesure du plafond : 1 requete initiale + 10 sauts
     * (DEFAULT_MAX_REDIRECTS). Sans plafond, le garde-fou du serveur
     * (64 connexions) serait atteint. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/boucle");
        s.rejoue_derniere = 1;
        if (!jr_demarre(&s)) { CHECK(0, "f5_boucle_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/boucle", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.erreur == 0, "f5_boucle_pas_d_erreur_runtime");
        CHECK(r.statut == 302, "f5_boucle_rend_le_dernier_302");
        CHECK(s.nb_servis == 11, "f5_boucle_11_connexions_1_plus_10_sauts");
        CHECK(s.nb_servis < JR_PLAFOND_CNX, "f5_boucle_effectivement_coupee");
        jr_arrete(&s);
    }

    /* --- 6. Plafond reglable : httpParamMaxRedirection = 2 -> 3
     * connexions. Preuve que le compteur est bien celui du parametre et
     * pas une constante. Le parametre est global au processus : on le
     * remet a 10 tout de suite apres. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/boucle");
        s.rejoue_derniere = 1;
        if (!jr_demarre(&s)) { CHECK(0, "f5_plafond2_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/boucle", s.port);
        Resultat r = requete(VIDE_JAR
                             "HTTPParametre(httpParamMaxRedirection, 2)\n", url);
        CHECK(r.statut == 302, "f5_plafond2_rend_302");
        CHECK(s.nb_servis == 3, "f5_plafond2_trois_connexions");
        jr_arrete(&s);

        /* Restauration : sans ca, tout ce qui suit hériterait d'un
         * plafond de 2 et serait vert pour la mauvaise raison. */
        Serveur t; jr_init(&t);
        rep_302(&t, "/boucle");
        t.rejoue_derniere = 1;
        if (!jr_demarre(&t)) { CHECK(0, "f5_restaure_serveur"); return; }
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/boucle", t.port);
        requete(VIDE_JAR "HTTPParametre(httpParamMaxRedirection, 10)\n", url);
        CHECK(t.nb_servis == 11, "f5_plafond_restaure_a_10");
        jr_arrete(&t);
    }

    /* --- 7. httpIgnoreRedirection : la redirection n'est pas suivie,
     * le 302 est rendu tel quel et le pair ne voit qu'une connexion.
     * Passe par HTTPEnvoie (la native reelle, pas un mock) : c'est la
     * seule surface qui porte ce drapeau. */
    {
        Serveur s; jr_init(&s);
        rep_302(&s, "/jamais");
        rep_200(&s, "jamais_atteint");
        if (!jr_demarre(&s)) { CHECK(0, "f5_ignore_serveur"); return; }

        int id = ++g_seq;
        char src[900];
        snprintf(src, sizeof(src),
            VIDE_JAR
            "q%d est un httpRequete\n"
            "q%d..URL = \"http://127.0.0.1:%d/depart\"\n"
            "q%d..IgnoreErreur = httpIgnoreRedirection\n"
            "rep%d est un httpReponse = HTTPEnvoie(q%d)\n"
            "Trace(\"STATUT=\" + HTTPDernierStatut())\n",
            id, id, s.port, id, id, id);
        capture_interpret(src);
        int statut = lit_entier("STATUT");
        CHECK(statut == 302, "f5_ignore_redirection_302_rendu_tel_quel");
        CHECK(s.nb_servis == 1, "f5_ignore_redirection_une_seule_connexion");
        jr_arrete(&s);
    }
}

/* ================================================================
 * F6 — cookies a travers une redirection
 * ================================================================ */

static void famille_f6(void) {
    g_famille = 5;

    char url[160], cookie[512];

    /* --- 1. Redirection MEME hote : le cookie pose par le 302 doit
     * partir sur le maillon suivant. C'est le motif « login puis
     * redirection vers l'espace connecte ». */
    {
        Serveur s; jr_init(&s);
        rep_302_cookie(&s, "sess=LOGIN1; Path=/", "/espace");
        rep_200(&s, "connecte");
        if (!jr_demarre(&s)) { CHECK(0, "f6_meme_hote_serveur"); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/login", s.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f6_meme_hote_statut_200");
        CHECK(s.nb_servis == 2, "f6_meme_hote_deux_connexions");
        CHECK(!jr_a_entete(&s, 0, "Cookie"), "f6_meme_hote_pas_de_cookie_au_1er_saut");
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "sess=LOGIN1") == 0,
              "f6_meme_hote_cookie_du_302_porte_au_saut_suivant");
        jr_arrete(&s);
    }

    /* --- 2. Redirection CROSS-HOST — le cas le plus a risque. L'hote A
     * pose un cookie et redirige vers l'hote B. Comportement mesure :
     * le cookie ne suit PAS (l'appariement de domaine est evalue a
     * chaque maillon, sur l'hote du maillon). Pas de fuite. */
    {
        Serveur b; jr_init(&b);
        rep_200(&b, "hoteB");
        if (!jr_demarre(&b)) { CHECK(0, "f6_cross_serveurB"); return; }

        Serveur a; jr_init(&a);
        char loc[160];
        snprintf(loc, sizeof(loc), "http://localhost:%d/cible", b.port);
        rep_302_cookie(&a, "sess=SECRET_CROSS; Path=/", loc);
        if (!jr_demarre(&a)) { CHECK(0, "f6_cross_serveurA"); jr_arrete(&b); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/login", a.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f6_cross_statut_final_200");
        CHECK(strcmp(r.corps, "hoteB") == 0, "f6_cross_corps_de_l_hote_B");
        CHECK(b.nb_servis == 1, "f6_cross_hote_B_joint");
        CHECK(!jr_a_entete(&b, 0, "Cookie"), "f6_cross_cookie_de_A_absent_chez_B");
        CHECK(strstr(jr_recu(&b, 0), "SECRET_CROSS") == NULL,
              "f6_cross_aucun_octet_du_secret_chez_B");
        jr_arrete(&a);
        jr_arrete(&b);
    }

    /* --- 3. Cross-host avec Domain force (forme exploitable de f2 §4).
     * L'hote A pose Domain=localhost et redirige vers localhost. C'est
     * le scenario d'attaque complet : sans validation du Domain a
     * l'ingestion, A depose chez le client un cookie que le client
     * presente ensuite a B, dans la meme requete enchainee. Le rejet
     * doit tenir sur le fil, pas seulement dans le jar. */
    {
        Serveur b; jr_init(&b);
        rep_200(&b, "hoteB2");
        if (!jr_demarre(&b)) { CHECK(0, "f6_cross_dom_serveurB"); return; }

        Serveur a; jr_init(&a);
        char loc[160], sc[160];
        snprintf(loc, sizeof(loc), "http://localhost:%d/cible", b.port);
        snprintf(sc, sizeof(sc), "sess=FORCE; Domain=localhost; Path=/");
        rep_302_cookie(&a, sc, loc);
        if (!jr_demarre(&a)) { CHECK(0, "f6_cross_dom_serveurA"); jr_arrete(&b); return; }

        snprintf(url, sizeof(url), "http://127.0.0.1:%d/login", a.port);
        Resultat r = requete(VIDE_JAR, url);
        CHECK(r.statut == 200, "f6_cross_dom_statut_200");
        CHECK(b.nb_servis == 1, "f6_cross_dom_hote_B_joint");
        CHECK(!jr_a_entete(&b, 0, "Cookie"),
              "f6_cookie_a_domain_force_ne_suit_pas_la_redirection_cross_host");
        CHECK(strstr(jr_recu(&b, 0), "FORCE") == NULL,
              "f6_cross_dom_aucun_octet_du_cookie_force_chez_B");
        jr_arrete(&a);
        jr_arrete(&b);
    }
}

/* ================================================================
 * F7 — expiration (Max-Age / Expires)
 *
 * Pourquoi ca compte : Max-Age=0 (ou une Expires passee) est la facon
 * standard dont un serveur REVOQUE une session. Ignoree, la revocation
 * ne se propage pas : le client continue de presenter un jeton mort.
 *
 * Deux precautions de methode :
 *
 * 1. AUCUNE ASSERTION NE DEPEND DE L'HORLOGE MURALE. On ne teste que
 *    des valeurs limites — Max-Age=0, Max-Age negatif, date largement
 *    passee, date largement future. Pas de sleep, donc pas de
 *    clignotement. Contrepartie assumee : l'expiration d'un cookie
 *    encore valide AU MOMENT ou il est stocke n'est pas observable
 *    ici (elle demanderait d'attendre).
 *
 * 2. CHAQUE CAS NEGATIF EST APPAREILLE A UN CAS POSITIF. « Le cookie
 *    n'est plus envoye » est vrai sur un jar qui n'envoie plus rien du
 *    tout : §1 (un cookie non expire continue de partir) et §14 (un
 *    cookie voisin non expire survit a l'effacement de l'autre) sont
 *    ce qui empeche cette lecture.
 * ================================================================ */

/* Dates limites. Assez loin dans le passe / le futur pour qu'aucune
 * derive d'horloge ne les rapproche du present. La borne future tient
 * sous un time_t 32 bits (< 2038) : le test ne doit pas dependre de la
 * largeur du time_t de la plateforme. */
#define DATE_PASSEE_IMF  "Wed, 21 Oct 2015 07:28:00 GMT"
#define DATE_PASSEE_850  "Sunday, 06-Nov-94 08:49:37 GMT"
#define DATE_PASSEE_ASC  "Sun Nov  6 08:49:37 1994"
#define DATE_FUTURE_IMF  "Thu, 01 Jan 2037 00:00:00 GMT"

/* Joue sur un pair neuf : jar vide + Set-Cookie `pose`, puis (si non
 * NULL) un second Set-Cookie `suite`, puis une requete TEMOIN dont
 * l'entete Cookie recu est copie dans `cookie_recu`. Rend le nombre de
 * connexions servies (0 = le pair n'a pas demarre).
 *
 * C'est sur les octets recus par le pair au tour temoin que tout se
 * joue : ce que le client a rendu ne dit pas ce qu'il a emis. */
static int expiration_temoin(const char* pose, const char* suite,
                             char* cookie_recu, size_t cap,
                             const char** recu_brut) {
    static Serveur s;
    cookie_recu[0] = '\0';
    if (recu_brut) *recu_brut = "";

    jr_init(&s);
    rep_200_cookie(&s, pose, "pose");
    if (suite) rep_200_cookie(&s, suite, "suite");
    rep_200(&s, "temoin");
    if (!jr_demarre(&s)) return 0;

    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
    requete(VIDE_JAR, url);
    if (suite) requete("", url);
    requete("", url);

    int tour_temoin = suite ? 2 : 1;
    jr_entete(&s, tour_temoin, "Cookie", cookie_recu, cap);
    if (recu_brut) *recu_brut = jr_recu(&s, tour_temoin);
    int servis = s.nb_servis;
    jr_arrete(&s);
    return servis;
}

/* Lit le jar par HTTPCookie(nom) — l'autre porte du jar, celle du code
 * appelant. Tout le reste de la famille se mesure sur les octets recus
 * par le pair : ce qui part sur le fil ne dit rien de ce que le
 * programme peut relire. */
static void jr_lit_cookie(const char* nom, char* dst, size_t cap) {
    char src[320];
    snprintf(src, sizeof(src),
        RESET_PARAMS
        "Trace(\"LU=[\" + HTTPCookie(\"%s\") + \"]\")\n", nom);
    capture_interpret(src);
    lit_entre_crochets("LU", dst, cap);
}

static void famille_f7(void) {
    g_famille = 6;

    char cookie[512];
    const char* brut;

    /* --- 1. CONTROLE POSITIF. Sans lui, tout le reste de la famille
     * serait vrai sur un jar devenu incapable d'envoyer quoi que ce
     * soit. Deux formes : sans attribut d'expiration (cookie de
     * session), et avec un Max-Age largement positif. */
    {
        int n = expiration_temoin("cp=V1; Path=/", NULL, cookie, sizeof(cookie), NULL);
        CHECK(n == 2, "f7_positif_session_deux_tours");
        CHECK(strcmp(cookie, "cp=V1") == 0,
              "f7_CONTROLE_POSITIF_cookie_sans_expiration_renvoye");
    }
    {
        int n = expiration_temoin("cp=V2; Path=/", "cp=V2; Path=/; Max-Age=3600",
                                  cookie, sizeof(cookie), NULL);
        CHECK(n == 3, "f7_positif_maxage_trois_tours");
        CHECK(strcmp(cookie, "cp=V2") == 0,
              "f7_CONTROLE_POSITIF_max_age_positif_cookie_conserve");
    }

    /* --- 2. LE CAS DU DEFAUT : un serveur revoque la session par
     * Max-Age=0 sur un cookie deja pose. Le jeton ne doit plus partir,
     * et aucun de ses octets ne doit rester sur le fil. */
    {
        int n = expiration_temoin("session=abc; Path=/",
                                  "session=abc; Path=/; Max-Age=0",
                                  cookie, sizeof(cookie), &brut);
        CHECK(n == 3, "f7_revocation_trois_tours");
        CHECK(cookie[0] == '\0', "f7_max_age_0_revoque_la_session");
        CHECK(strstr(brut, "abc") == NULL,
              "f7_max_age_0_aucun_octet_du_jeton_revoque");
    }

    /* --- 3. Max-Age negatif : meme effet (RFC 6265 §5.2.2 — « if
     * delta-seconds is less than or equal to zero, let expiry-time be
     * the earliest representable date »). */
    {
        expiration_temoin("neg=N1; Path=/", "neg=N1; Path=/; Max-Age=-1",
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_max_age_negatif_efface");
    }

    /* --- 4. Cookie deja expire a la POSE : jamais stocke, donc jamais
     * presente. Chemin distinct du precedent (pas de slot existant a
     * retirer). */
    {
        expiration_temoin("mort=M1; Path=/; Max-Age=0", NULL,
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_max_age_0_des_la_pose_jamais_stocke");
    }

    /* --- 5. Nom d'attribut insensible a la casse (§5.2 : « case-
     * insensitively matches »). */
    {
        expiration_temoin("casse=C1; Path=/", "casse=C1; Path=/; MAX-AGE=0",
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_max_age_casse_indifferente");
    }

    /* --- 6. Expires dans le passe, format IMF-fixdate (le format
     * courant). */
    {
        expiration_temoin("exp=E1; Path=/",
                          "exp=E1; Path=/; Expires=" DATE_PASSEE_IMF,
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_expires_passee_imf_efface");
    }

    /* --- 7. Expires dans le futur : conserve. Pris SEUL, ce cas ne
     * discrimine pas (un client qui ignore Expires est vert aussi) —
     * ce sont §6, §8 et §9 qui le rendent parlant. Il verrouille le
     * sens inverse : une logique « tout est expire » le fait rougir. */
    {
        expiration_temoin("fut=F1; Path=/",
                          "fut=F1; Path=/; Expires=" DATE_FUTURE_IMF,
                          cookie, sizeof(cookie), NULL);
        CHECK(strcmp(cookie, "fut=F1") == 0, "f7_expires_future_conserve");
    }

    /* --- 8. Formats de date herites, tous deux DANS LE PASSE, donc
     * discriminants : si le format n'est pas compris, l'attribut est
     * ignore (§5.2.1) et le cookie SURVIT. Ces deux cas mesurent donc
     * la tolerance de format exigee par §5.1.1, pas seulement
     * l'expiration.
     *   - RFC 850 : annee sur 2 chiffres, separateurs '-'
     *   - asctime : pas de fuseau, annee en dernier, double espace */
    {
        expiration_temoin("v850=A; Path=/",
                          "v850=A; Path=/; Expires=" DATE_PASSEE_850,
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_expires_format_rfc850_compris_et_efface");
    }
    {
        expiration_temoin("vasc=B; Path=/",
                          "vasc=B; Path=/; Expires=" DATE_PASSEE_ASC,
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_expires_format_asctime_compris_et_efface");
    }

    /* --- 9. Expires illisible : l'attribut est IGNORE (§5.2.1 « if the
     * attribute-value failed to parse as a cookie date, ignore the
     * cookie-av »), le cookie reste. Discriminant contre un parseur qui
     * rendrait l'epoque en cas d'echec — ce qui effacerait le cookie
     * sur une simple faute de frappe du serveur. */
    {
        expiration_temoin("ill=I1; Path=/",
                          "ill=I1; Path=/; Expires=n-importe-quoi",
                          cookie, sizeof(cookie), NULL);
        CHECK(strcmp(cookie, "ill=I1") == 0, "f7_expires_illisible_attribut_ignore");
    }

    /* --- 10. PRECEDENCE. RFC 6265 §5.3 etape 3 : « If the cookie-
     * attribute-list contains an attribute with an attribute-name of
     * Max-Age [...] Otherwise, if [...] Expires ». Max-Age l'emporte,
     * dans les DEUX sens, et l'ordre d'ecriture dans l'entete ne
     * change rien (l'etape 3 parle de presence, pas de position). */
    {
        expiration_temoin("p1=X; Path=/",
                          "p1=X; Path=/; Expires=" DATE_FUTURE_IMF "; Max-Age=0",
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_precedence_max_age_0_bat_expires_future");
    }
    {
        expiration_temoin("p2=Y; Path=/",
                          "p2=Y; Path=/; Expires=" DATE_PASSEE_IMF "; Max-Age=3600",
                          cookie, sizeof(cookie), NULL);
        CHECK(strcmp(cookie, "p2=Y") == 0, "f7_precedence_max_age_positif_bat_expires_passee");
    }
    {
        expiration_temoin("p3=Z; Path=/",
                          "p3=Z; Path=/; Max-Age=3600; Expires=" DATE_PASSEE_IMF,
                          cookie, sizeof(cookie), NULL);
        CHECK(strcmp(cookie, "p3=Z") == 0, "f7_precedence_independante_de_l_ordre_ecrit");
    }

    /* --- 11. Max-Age malforme : §5.2.2 impose d'IGNORER l'attribut
     * (« if the remainder of attribute-value contains a non-DIGIT
     * character, ignore the cookie-av »). Il ne prend donc pas la
     * precedence : l'Expires passee qui l'accompagne s'applique et le
     * cookie part. Un « Max-Age present => Max-Age gagne » naif rend
     * ce cas faux. */
    {
        expiration_temoin("mal=W; Path=/",
                          "mal=W; Path=/; Max-Age=abc; Expires=" DATE_PASSEE_IMF,
                          cookie, sizeof(cookie), NULL);
        CHECK(cookie[0] == '\0', "f7_max_age_malforme_ignore_expires_s_applique");
    }

    /* --- 12. L'expiration est CIBLEE. Deux cookies poses ensemble, un
     * seul revoque : l'autre doit continuer de partir, avec sa valeur
     * exacte. C'est l'assertion qui distingue « ce cookie-ci a expire »
     * de « le jar ne sert plus rien ». */
    {
        Serveur s; jr_init(&s);
        jr_reponse(&s,
            "HTTP/1.1 200 OK\r\n"
            "Set-Cookie: part=P; Path=/\r\n"
            "Set-Cookie: garde=G; Path=/\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\npose");
        rep_200_cookie(&s, "part=P; Path=/; Max-Age=0", "revoque");
        rep_200(&s, "temoin");
        if (!jr_demarre(&s)) { CHECK(0, "f7_cible_serveur"); return; }

        char url[160];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);
        requete(VIDE_JAR, url);
        requete("", url);
        jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "part=P; garde=G") == 0, "f7_cible_les_deux_avant_revocation");

        requete("", url);
        jr_entete(&s, 2, "Cookie", cookie, sizeof(cookie));
        CHECK(strcmp(cookie, "garde=G") == 0, "f7_cible_seul_le_cookie_revoque_disparait");
        CHECK(strstr(jr_recu(&s, 2), "part=") == NULL,
              "f7_cible_aucun_octet_du_cookie_revoque");
        jr_arrete(&s);
    }

    /* --- 13. CHEMIN DE LECTURE. Les douze cas precedents se mesurent
     * sur le fil ; aucun ne dit ce que HTTPCookie(nom) rend au code
     * appelant. Une revocation qui ne vaudrait que sur le fil laisserait
     * le jeton mort relisible par le programme.
     *
     * Les trois lectures sont appareillees DANS LE MEME JAR, ce qui les
     * rend discriminantes chacune dans un sens :
     *   - une lecture qui rendrait toujours la chaine vide fait rougir
     *     les deux premieres (valeur exacte attendue) ;
     *   - une lecture aveugle au nom, ou a l'expiration, fait rougir la
     *     troisieme (le cookie revoque ressortirait). */
    {
        Serveur s; jr_init(&s);
        jr_reponse(&s,
            "HTTP/1.1 200 OK\r\n"
            "Set-Cookie: part=P13; Path=/\r\n"
            "Set-Cookie: garde=G13; Path=/\r\n"
            "Content-Length: 4\r\n"
            "Connection: close\r\n"
            "\r\npose");
        rep_200_cookie(&s, "part=P13; Path=/; Max-Age=0", "revoque");
        if (!jr_demarre(&s)) { CHECK(0, "f7_lecture_serveur"); return; }

        char url[160], lu[512];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);

        requete(VIDE_JAR, url);
        jr_lit_cookie("part", lu, sizeof(lu));
        CHECK(strcmp(lu, "P13") == 0,
              "f7_lecture_HTTPCookie_rend_la_valeur_exacte_avant_revocation");

        requete("", url);
        jr_lit_cookie("garde", lu, sizeof(lu));
        CHECK(strcmp(lu, "G13") == 0,
              "f7_lecture_HTTPCookie_rend_la_valeur_exacte_du_cookie_vivant");
        jr_lit_cookie("part", lu, sizeof(lu));
        CHECK(lu[0] == '\0',
              "f7_lecture_HTTPCookie_ne_rend_plus_le_cookie_revoque");
        jr_arrete(&s);
    }
}

/* ================================================================
 * F8 — expiration DIFFEREE : l'echeance tombe PENDANT la vie du
 *      processus
 *
 * Ce que F7 ne peut pas voir. F7 ne joue que des valeurs limites
 * (Max-Age nul ou negatif, Expires passee) : toutes sont interceptees
 * A LA POSE par cookie_jar_set, qui n'enregistre jamais un cookie deja
 * expire. L'etat « slot occupe, echeance passee » — le seul que le
 * balayage cookie_jar_evince_expires sache traiter — reste donc hors
 * d'atteinte. Mesure : neutraliser ce balayage laissait F1..F7 a
 * 137/137, zero rouge.
 *
 * C'est pourtant l'etat NORMAL d'un processus de longue duree : un
 * cookie pose avec Max-Age=3600 dans un serveur qui tourne quatre
 * heures. Sans balayage, le jeton mort continue d'etre presente a
 * chaque requete — le defaut meme que  a corrige, par la porte
 * de derriere.
 *
 * Comment on l'observe sans attendre. La logique d'expiration lit son
 * temps courant a travers un decalage reserve aux self-tests C
 * (owl_http_test_horloge_decale, cf. stdlib_http.c). Aucun sleep,
 * aucune horloge murale, donc aucun clignotement. Le decalage n'est
 * PAS visible depuis le langage : le manipuler depuis OWL supposerait
 * une native de manipulation du temps, c'est-a-dire une surface
 * publique permanente pour un besoin de test.
 *
 * Ordre impose : httpParamReinitialise remet le decalage a zero (c'est
 * ce qui l'empeche de fuir vers un self-test voisin). Il faut donc
 * emettre le verrouillage d'isolation D'ABORD, armer l'horloge
 * ENSUITE, et lancer la requete sans nouveau verrouillage. §1 mesure
 * les deux bouts de cette propriete au lieu de les supposer.
 *
 * Chaque cas negatif est appareille : un cookie de session pose au
 * meme tour doit continuer de partir apres l'echeance de son voisin.
 * Sans lui, « le jeton n'est plus presente » serait vrai sur un jar
 * devenu muet.
 * ================================================================ */

#define F8_MAX_AGE       3600   /* echeance du cookie sous test */
#define F8_AVANT_ECHEANCE  600  /* +10 min : encore valide */
#define F8_APRES_ECHEANCE 7200  /* +2 h  : largement perime */

/* Reponse 200 portant DEUX Set-Cookie : le cookie sous test et son
 * temoin de session. Les deux arrivent au meme tour, donc dans le meme
 * etat de jar — le temoin ne peut pas etre suspecte d'avoir ete pose
 * dans des conditions differentes. */
static void rep_200_deux_cookies(Serveur* s, const char* c1, const char* c2,
                                 const char* corps) {
    jr_reponse(s,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Set-Cookie: %s\r\n"
        "Set-Cookie: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n%s", c1, c2, (int)strlen(corps), corps);
}

/* Emet le verrouillage d'isolation SEUL, puis arme l'horloge. Les tours
 * qui suivent doivent passer par requete_ex(..., 0) : un
 * httpParamReinitialise desarmerait l'horloge. */
static void arme_horloge(int64_t secondes) {
    capture_interpret(RESET_PARAMS);
    owl_http_test_horloge_decale(secondes);
}

/* HTTPCookie(nom) sans reinitialiser les parametres : l'horloge armee
 * doit rester en place pour ce tour. */
static void lit_cookie_horloge_armee(const char* nom, char* dst, size_t cap) {
    char src[320];
    snprintf(src, sizeof(src),
        "Trace(\"LU=[\" + HTTPCookie(\"%s\") + \"]\")\n", nom);
    capture_interpret(src);
    lit_entre_crochets("LU", dst, cap);
}

static void famille_f8(void) {
    g_famille = 7;

    /* --- 1. ETANCHEITE DE LA COUTURE, mesuree aux deux bouts.
     * a) aucun self-test voisin ne nous a laisse une horloge armee ;
     * b) le verrouillage d'isolation la desarme — c'est cette propriete
     *    qui garantit qu'une horloge oubliee ici ne peut pas produire un
     *    vert trompeur chez le voisin (classe ). */
    CHECK(owl_http_test_horloge_decalage() == 0,
          "f8_horloge_au_repos_a_l_entree");
    owl_http_test_horloge_decale(F8_APRES_ECHEANCE);
    CHECK(owl_http_test_horloge_decalage() == F8_APRES_ECHEANCE,
          "f8_horloge_armable");
    capture_interpret(RESET_PARAMS);
    CHECK(owl_http_test_horloge_decalage() == 0,
          "f8_horloge_desarmee_par_le_verrouillage_d_isolation");

    /* --- 2. Le cas complet, sur un seul pair et un seul jar :
     *   tour 0 : pose « dur » (Max-Age=3600) et « sess » (session) ;
     *   tour 1 : AVANT echeance (+600 s)  -> les deux partent ;
     *   tour 2 : APRES echeance (+7200 s) -> « dur » a disparu, « sess »
     *            part toujours.
     * On asserte sur les octets recus par le pair : ce que le client a
     * rendu ne dit pas ce qu'il a mis sur le fil. */
    Serveur s; jr_init(&s);
    rep_200_deux_cookies(&s, "dur=D1; Path=/; Max-Age=3600", "sess=S1; Path=/", "pose");
    rep_200(&s, "avant");
    rep_200(&s, "apres");
    if (!jr_demarre(&s)) { CHECK(0, "f8_serveur_demarre"); return; }
    CHECK(1, "f8_serveur_demarre");

    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", s.port);

    Resultat pose = requete(VIDE_JAR, url);
    CHECK(pose.statut == 200, "f8_pose_statut_200");

    /* CONTROLE POSITIF — avant echeance, le cookie sous test EST
     * presente. Sans cette assertion, tout le §3 serait vrai sur un jar
     * qui n'envoie plus rien. */
    char cookie[512];
    arme_horloge(F8_AVANT_ECHEANCE);
    Resultat avant = requete_ex("", url, 0);
    CHECK(avant.statut == 200, "f8_avant_statut_200");
    jr_entete(&s, 1, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "dur=D1; sess=S1") == 0,
          "f8_CONTROLE_POSITIF_avant_echeance_les_deux_cookies_partent");

    /* --- 3. LE CAS DU DEFAUT. Meme cookie, meme jar, meme pair : seule
     * l'heure a change. Le jeton perime ne doit plus partir, et aucun de
     * ses octets ne doit rester sur le fil. */
    arme_horloge(F8_APRES_ECHEANCE);
    Resultat apres = requete_ex("", url, 0);
    CHECK(apres.statut == 200, "f8_apres_statut_200");
    jr_entete(&s, 2, "Cookie", cookie, sizeof(cookie));
    CHECK(strcmp(cookie, "sess=S1") == 0,
          "f8_apres_echeance_le_jeton_perime_ne_part_plus");
    CHECK(strstr(jr_recu(&s, 2), "D1") == NULL,
          "f8_apres_echeance_aucun_octet_du_jeton_perime");
    /* Appareillage du negatif : le voisin de session est intact. */
    CHECK(strstr(jr_recu(&s, 2), "sess=S1") != NULL,
          "f8_CONTROLE_POSITIF_le_cookie_de_session_survit_au_balayage");

    /* --- 4. L'autre porte du jar : HTTPCookie(nom), celle du code
     * appelant. Elle balaye aussi — un programme ne doit pas pouvoir
     * relire un jeton que le client a cesse d'emettre. L'horloge est
     * encore armee a +7200 (aucun verrouillage entre-temps). */
    char lu[256];
    CHECK(owl_http_test_horloge_decalage() == F8_APRES_ECHEANCE,
          "f8_horloge_encore_armee_pour_la_lecture");
    lit_cookie_horloge_armee("dur", lu, sizeof(lu));
    CHECK(lu[0] == '\0', "f8_lecture_HTTPCookie_ne_rend_plus_le_jeton_perime");
    lit_cookie_horloge_armee("sess", lu, sizeof(lu));
    CHECK(strcmp(lu, "S1") == 0,
          "f8_CONTROLE_POSITIF_lecture_HTTPCookie_rend_le_cookie_de_session");

    /* --- 5. On ne laisse pas l'horloge armee derriere nous : la
     * verifier ici plutot que s'en remettre au verrouillage du voisin. */
    owl_http_test_horloge_decale(0);
    CHECK(owl_http_test_horloge_decalage() == 0, "f8_horloge_desarmee_a_la_sortie");
    jr_arrete(&s);
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_jar_redirect_self_test_run(void) {
    printf("== jar cookies + redirections (fix/1391) ==\n");

    memset(g_pass, 0, sizeof(g_pass));
    memset(g_fail, 0, sizeof(g_fail));

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    /* L'environnement du PROCESSUS decide : un self-test voisin du meme
     * binaire y pose no_proxy=127.0.0.1, un autre le vide. On repart
     * d'un environnement neutre, et f1 asserte qu'il l'est encore au
     * moment ou les familles tournent. */
    {
        static char e1[] = "http_proxy=";
        static char e2[] = "HTTP_PROXY=";
        static char e3[] = "https_proxy=";
        static char e4[] = "HTTPS_PROXY=";
        static char e5[] = "no_proxy=";
        static char e6[] = "NO_PROXY=";
        owl_putenv(e1); owl_putenv(e2); owl_putenv(e3);
        owl_putenv(e4); owl_putenv(e5); owl_putenv(e6);
    }

    famille_f1();
    famille_f2();
    famille_f3();
    famille_f4();
    famille_f5();
    famille_f6();
    famille_f7();
    famille_f8();

    /* Le jar est global au processus : on ne le laisse pas peuple
     * derriere nous, par egard pour les self-tests suivants — c'est
     * exactement l'etat dans lequel celui-ci a ete trouve. */
    capture_interpret(RESET_PARAMS VIDE_JAR);

    int tp = 0, tf = 0;
    for (int i = 0; i < JR_NB_FAMILLES; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f1 controle positif        : %d/%d\n", g_pass[0], g_pass[0] + g_fail[0]);
    printf("   f2 appariement de domaine  : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   f3 portee de chemin        : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   f4 attributs               : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   f5 redirections            : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f6 cookies + redirection   : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f7 expiration              : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f8 expiration differee     : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   TOTAL jar + redirections   : %d/%d\n", tp, tp + tf);
    return tf;
}
