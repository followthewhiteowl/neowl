// SPDX-License-Identifier: AGPL-3.0-only
/* http_llhttp_integration_test.c —  lot 1
 *
 * Caracterise le parsing de REPONSE du client HTTP en pilotant le VRAI
 * chemin (HTTPRequete -> do_http_request_with_redirects -> do_http_request
 * -> read_headers / decode_chunked / read_body_*) contre un pair TCP local
 * qui controle les octets emis ET le decoupage de ses write()
 * (src/net_mock_peer.c).
 *
 * Pourquoi via la VM et pas un appel C direct : tout le parsing de
 * stdlib_http.c est `static`, il n'existe aucun point d'entree
 * `parse_response(octets) -> resultat`. Le seul moyen d'exercer le vrai
 * chemin sans le modifier est de passer par la native, donc par
 * vm_interpret() (meme pattern que multilingue_print_test.c).
 *
 * Execute via `owl --self-test-net` (cf. main.c). La VM est initialisee
 * par le bloc appelant.
 *
 * ============================ DEUX FAMILLES ============================
 *
 * FAMILLE 1 — non-regression. VERTE contre le parseur maison actuel, et
 *   elle doit rester verte apres la bascule. Content-Length, chunked
 *   (dont chunks de taille 1 et dernier chunk vide), corps delimite par
 *   fermeture, entetes repetes legitimes, 204/304 sans corps, gzip via
 *   miniz, statuts varies, casses/espaces varies. Plus les tests de
 *   fragmentation : reponse bien formee injectee octet par octet
 *   (entetes ET corps) == meme resultat qu'en un bloc, et decoupages
 *   adversariaux (au milieu d'un CRLF, entre le nom d'entete et les
 *   deux-points, au milieu d'une taille de chunk).
 *
 * FAMILLE 2 — durcissement. ROUGE contre le parseur maison actuel, par
 *   construction : ce sont les 6 classes que le parseur maison accepte en
 *   silence et qu'une machine a etats stricte rejette. Chaque cas asserte
 *   qu'une erreur runtime est levee (ErreurDetectee) ET qu'elle porte le
 *   code dedie aux reponses non conformes, pas un echec reseau generique.
 *   Ecrite rouge contre le parseur maison : si cette famille etait sortie
 *   verte avant la bascule, les tests n'auraient rien prouve.
 * ====================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "../vm.h"
#include "../net_mock_peer.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <io.h>
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
  #include <unistd.h>
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

static int g_pass_f1 = 0, g_fail_f1 = 0;
static int g_pass_f2 = 0, g_fail_f2 = 0;
static int g_pass_f3 = 0, g_fail_f3 = 0;
static int g_famille  = 1;   /* famille courante, pour le comptage */
static int g_seq      = 0;   /* suffixe des globales OWL (une par requete) */

static void compte(int ok, const char* label, int ligne) {
    if (g_famille == 1) {
        if (ok) g_pass_f1++;
        else { g_fail_f1++; fprintf(stderr, "  FAIL F1 [%s] (ligne %d)\n", label, ligne); }
    } else if (g_famille == 2) {
        if (ok) g_pass_f2++;
        else { g_fail_f2++; fprintf(stderr, "  FAIL F2 [%s] (ligne %d)\n", label, ligne); }
    } else {
        if (ok) g_pass_f3++;
        else { g_fail_f3++; fprintf(stderr, "  FAIL F3 [%s] (ligne %d)\n", label, ligne); }
    }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Capture stdout autour d'un vm_interpret
 * ================================================================ */

static char g_out[64 * 1024];

static InterpretResult capture_interpret(const char* source) {
    char TMP[128];
    fixture_unique(TMP, sizeof(TMP), "_owl_http_llhttp_test_", ".tmp");
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

/* ================================================================
 * Pilotage d'une requete contre le pair mock
 * ================================================================ */

typedef struct {
    int    interprete_ok;
    int    erreur;        /* ErreurDetectee : 0 / 1 */
    char   code[96];      /* ErreurInfo(errCode) : symbole canonique, "" si aucun */
    char   message[512];  /* ErreurInfo(errMessage) : message rendu, "" si aucun */
    int    statut;        /* HTTPDernierStatut() */
    int    taille;        /* Taille(corps) */
    char   corps[8192];
    size_t nb_ecritures;  /* write() reellement emis par le mock */
} Resultat;

/* Extrait la valeur entiere d'une ligne "CLE=<n>" de la sortie capturee.
 * Retourne -1 si la cle est absente. */
static int lit_entier(const char* cle) {
    char motif[32];
    snprintf(motif, sizeof(motif), "%s=", cle);
    const char* p = strstr(g_out, motif);
    if (!p) return -1;
    return atoi(p + strlen(motif));
}

/* Extrait la valeur d'une ligne "CLE=[...]" de la sortie capturee (ni les
 * payloads de test ni les symboles d'erreur ne contiennent de saut de ligne
 * ou de crochet fermant, donc la ligne est non ambigue). */
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

static Resultat joue(const char* octets, size_t len,
                     OwlMockPeerDecoupe decoupe,
                     const size_t* offsets, size_t noff,
                     int delai_ms) {
    Resultat r;
    memset(&r, 0, sizeof(r));

    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role               = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets             = octets;
    cfg.octets_len         = len;
    cfg.decoupe            = decoupe;
    cfg.offsets            = offsets;
    cfg.offsets_len        = noff;
    cfg.delai_ms           = delai_ms;
    cfg.sentinelle_lecture = "\r\n\r\n";   /* fin des entetes de la requete */
    cfg.fermer_apres_ecriture = 1;

    OwlMockPeer* p = owl_mock_peer_demarre(&cfg);
    if (!p) return r;
    if (!owl_mock_peer_pret(p)) { owl_mock_peer_libere(p); return r; }

    int id = ++g_seq;
    char src[896];
    snprintf(src, sizeof(src),
        "r%d est une chaine = HTTPRequete(\"http://127.0.0.1:%d/x\")\n"
        /* ErreurDetectee doit etre lue AVANT tout autre appel : la
         * sentinelle d'erreur ne survit qu'a un seul OP_CALL. Le code et le
         * message, eux, survivent au RAZ du drapeau (owl_clear_error ne
         * touche pas l'info). */
        "e%d est un booleen = ErreurDetectee\n"
        "c%d est une chaine = ErreurInfo(errCode)\n"
        "m%d est une chaine = ErreurInfo(errMessage)\n"
        "Trace(\"ERREUR=\" + e%d)\n"
        "Trace(\"CODE=[\" + c%d + \"]\")\n"
        "Trace(\"MSG=[\" + m%d + \"]\")\n"
        "Trace(\"STATUT=\" + HTTPDernierStatut())\n"
        "Trace(\"TAILLE=\" + Taille(r%d))\n"
        "Trace(\"CORPS=[\" + r%d + \"]\")\n",
        id, owl_mock_peer_port(p), id, id, id, id, id, id, id, id);

    r.interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    r.erreur = lit_entier("ERREUR");
    r.statut = lit_entier("STATUT");
    r.taille = lit_entier("TAILLE");
    lit_entre_crochets("CODE", r.code, sizeof(r.code));
    lit_entre_crochets("MSG", r.message, sizeof(r.message));
    lit_entre_crochets("CORPS", r.corps, sizeof(r.corps));

    owl_mock_peer_joint(p);
    r.nb_ecritures = owl_mock_peer_nb_ecritures(p);
    owl_mock_peer_libere(p);
    return r;
}

static Resultat joue_bloc(const char* octets, size_t len) {
    return joue(octets, len, OWL_MOCK_DECOUPE_BLOC, NULL, 0, 0);
}

/* Second pilote, pour le bloc d'entetes rendu tel qu'il est arrive sur le fil.
 * `ctype` sort en meme temps mais vient, lui, du bloc normalise : les deux
 * representations sont ainsi verifiees sur une seule et meme reponse.
 *
 * Le champ qui porte ce contrat cote langage est httpReponse..EnteteBrut. Il
 * est hors d'atteinte pour l'instant : HTTPEnvoie cherche le modele de type a
 * cloner dans la table des globales alors qu'il vit dans celle des types, et
 * rend donc Null sur toute reponse reelle (defaut anterieur a ce chantier,
 * mesure et signale). En attendant sa correction, la lecture se fait par la
 * chaine de reponse, alimentee par la meme capture. */
static void joue_entete_brut(const char* octets, size_t len,
                              OwlMockPeerDecoupe decoupe, int delai_ms,
                              char* brut, size_t brut_cap,
                              char* ctype, size_t ctype_cap) {
    brut[0] = '\0';
    ctype[0] = '\0';

    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets                = octets;
    cfg.octets_len            = len;
    cfg.decoupe               = decoupe;
    cfg.delai_ms              = delai_ms;
    cfg.sentinelle_lecture    = "\r\n\r\n";
    cfg.fermer_apres_ecriture = 1;

    OwlMockPeer* p = owl_mock_peer_demarre(&cfg);
    if (!p) return;
    if (!owl_mock_peer_pret(p)) { owl_mock_peer_libere(p); return; }

    int id = ++g_seq;
    char src[512];
    snprintf(src, sizeof(src),
        "r%d est une chaine = HTTPRequete(\"http://127.0.0.1:%d/x\")\n"
        "Trace(\"BRUT=[\" + HTTPDonneChaineReponse() + \"]\")\n"
        "Trace(\"CTYPE=[\" + HTTPDernierEntete(\"Content-Type\") + \"]\")\n",
        id, owl_mock_peer_port(p));

    capture_interpret(src);
    lit_entre_crochets("BRUT", brut, brut_cap);
    lit_entre_crochets("CTYPE", ctype, ctype_cap);

    owl_mock_peer_joint(p);
    owl_mock_peer_libere(p);
}

/* Vrai si `s` se termine par `suffixe`. */
static int finit_par(const char* s, const char* suffixe) {
    size_t ls = strlen(s), lp = strlen(suffixe);
    return ls >= lp && memcmp(s + ls - lp, suffixe, lp) == 0;
}

/* Offset juste apres la 1re occurrence de `motif` (pour couper a une
 * frontiere signifiante sans coder des indices en dur). */
static size_t off_apres(const char* payload, const char* motif) {
    const char* p = strstr(payload, motif);
    if (!p) return 0;
    return (size_t)(p - payload) + strlen(motif);
}

/* ================================================================
 * Payloads
 * ================================================================ */

#define REP_CL_SIMPLE \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type: text/plain\r\n" \
    "Content-Length: 7\r\n" \
    "\r\n" \
    "Bonjour"

/* ================================================================
 * FAMILLE 1 — non-regression (verte contre le parseur actuel)
 * ================================================================ */

static void f1_content_length(void) {
    Resultat r = joue_bloc(REP_CL_SIMPLE, strlen(REP_CL_SIMPLE));
    CHECK(r.interprete_ok, "cl_interprete_ok");
    CHECK(r.erreur == 0,   "cl_pas_erreur");
    CHECK(r.statut == 200, "cl_statut_200");
    CHECK(r.taille == 7,   "cl_taille_7");
    CHECK(strcmp(r.corps, "Bonjour") == 0, "cl_corps_exact");
}

static void f1_chunked(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\nBonjour\r\n"
        "4\r\n OWL\r\n"
        "0\r\n\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,   "chunked_pas_erreur");
    CHECK(r.statut == 200, "chunked_statut_200");
    CHECK(r.taille == 11,  "chunked_taille_11");
    CHECK(strcmp(r.corps, "Bonjour OWL") == 0, "chunked_corps_exact");
}

/* Chunks de taille 1 + dernier chunk vide. */
static void f1_chunked_taille_1(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "1\r\nO\r\n"
        "1\r\nW\r\n"
        "1\r\nL\r\n"
        "0\r\n\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,  "chunked1_pas_erreur");
    CHECK(r.taille == 3,  "chunked1_taille_3");
    CHECK(strcmp(r.corps, "OWL") == 0, "chunked1_corps_exact");
}

/* Ni Content-Length ni Transfer-Encoding : le corps s'arrete a la
 * fermeture de connexion. */
static void f1_delimite_par_fermeture(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "FinParFermeture";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,   "fermeture_pas_erreur");
    CHECK(r.statut == 200, "fermeture_statut_200");
    CHECK(r.taille == 15,  "fermeture_taille_15");
    CHECK(strcmp(r.corps, "FinParFermeture") == 0, "fermeture_corps_exact");
}

/* Entetes repetes legitimes (Set-Cookie, X-Trace) : pas un motif de rejet. */
static void f1_entetes_repetes(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: a=1; Path=/\r\n"
        "Set-Cookie: b=2; Path=/\r\n"
        "X-Trace: un\r\n"
        "X-Trace: deux\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "ok";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,   "repetes_pas_erreur");
    CHECK(r.statut == 200, "repetes_statut_200");
    CHECK(r.taille == 2,   "repetes_taille_2");
    CHECK(strcmp(r.corps, "ok") == 0, "repetes_corps_exact");
}

static void f1_204_sans_corps(void) {
    static const char REP[] =
        "HTTP/1.1 204 No Content\r\n"
        "X-Vide: 1\r\n"
        "\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,   "204_pas_erreur");
    CHECK(r.statut == 204, "204_statut");
    CHECK(r.taille == 0,   "204_taille_0");
}

static void f1_304_sans_corps(void) {
    static const char REP[] =
        "HTTP/1.1 304 Not Modified\r\n"
        "ETag: \"abc\"\r\n"
        "\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 0,   "304_pas_erreur");
    CHECK(r.statut == 304, "304_statut");
    CHECK(r.taille == 0,   "304_taille_0");
}

/* Construit un flux gzip (RFC 1952) valide dont la partie deflate est un
 * bloc "stored" (BTYPE=00, BFINAL=1) : pas besoin de compresseur, et
 * miniz le decode par le meme chemin qu'un vrai gzip. */
static size_t gzip_stored(char* out, size_t cap, const char* data, size_t len) {
    if (cap < 10 + 5 + len + 8) return 0;
    size_t p = 0;
    out[p++] = (char)0x1f; out[p++] = (char)0x8b;   /* magic */
    out[p++] = 8;                                    /* deflate */
    out[p++] = 0;                                    /* flags */
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 0;  /* mtime */
    out[p++] = 0;                                    /* xflags */
    out[p++] = (char)0xff;                           /* OS inconnu */
    out[p++] = 1;                                    /* BFINAL=1 BTYPE=00 */
    out[p++] = (char)(len & 0xff);
    out[p++] = (char)((len >> 8) & 0xff);
    out[p++] = (char)(~len & 0xff);
    out[p++] = (char)((~len >> 8) & 0xff);
    memcpy(out + p, data, len); p += len;
    /* CRC32 non verifie par le client, ISIZE informatif. */
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 0;
    out[p++] = (char)(len & 0xff);
    out[p++] = (char)((len >> 8) & 0xff);
    out[p++] = 0; out[p++] = 0;
    return p;
}

static void f1_gzip(void) {
    static const char CLAIR[] = "Bonjour OWL";
    char gz[256];
    size_t gzl = gzip_stored(gz, sizeof(gz), CLAIR, sizeof(CLAIR) - 1);
    CHECK(gzl > 0, "gzip_payload_construit");
    if (gzl == 0) return;

    char rep[512];
    int hl = snprintf(rep, sizeof(rep),
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Length: %d\r\n"
        "\r\n", (int)gzl);
    memcpy(rep + hl, gz, gzl);

    Resultat r = joue_bloc(rep, (size_t)hl + gzl);
    CHECK(r.erreur == 0,   "gzip_pas_erreur");
    CHECK(r.statut == 200, "gzip_statut_200");
    CHECK(r.taille == 11,  "gzip_taille_decompressee_11");
    CHECK(strcmp(r.corps, "Bonjour OWL") == 0, "gzip_corps_decompresse");
}

static void f1_statuts_varies(void) {
    static const char R201[] =
        "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\ncree";
    static const char R404[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 8\r\n\r\nintrouve";
    static const char R500[] =
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 3\r\n\r\nbam";
    /* Ligne de statut sans raison : toleree par HTTP/1.1. */
    static const char R200[] =
        "HTTP/1.1 200\r\nContent-Length: 2\r\n\r\nok";

    Resultat a = joue_bloc(R201, sizeof(R201) - 1);
    CHECK(a.statut == 201, "statut_201");
    CHECK(strcmp(a.corps, "cree") == 0, "statut_201_corps");

    Resultat b = joue_bloc(R404, sizeof(R404) - 1);
    CHECK(b.statut == 404, "statut_404");
    CHECK(strcmp(b.corps, "introuve") == 0, "statut_404_corps");

    Resultat c = joue_bloc(R500, sizeof(R500) - 1);
    CHECK(c.statut == 500, "statut_500");
    CHECK(strcmp(c.corps, "bam") == 0, "statut_500_corps");

    Resultat d = joue_bloc(R200, sizeof(R200) - 1);
    CHECK(d.statut == 200, "statut_sans_raison_200");
    CHECK(strcmp(d.corps, "ok") == 0, "statut_sans_raison_corps");
}

/* Casses et espaces varies : noms en minuscules / majuscules, zero, un ou
 * plusieurs espaces apres les deux-points, tabulation. */
static void f1_casses_et_espaces(void) {
    static const char A[] =
        "HTTP/1.1 200 OK\r\ncontent-length:7\r\n\r\nBonjour";
    static const char B[] =
        "HTTP/1.1 200 OK\r\nCONTENT-LENGTH:   7\r\n\r\nBonjour";
    /*  lot 2 — le second en-tete etait initialement
     * "tRaNsFeR-eNcOdInG: identity", ce qui rendait ce cas INSATISFIABLE :
     * Content-Length + Transfer-Encoding est precisement le vecteur que
     * f2_cl_et_te exige de rejeter (llhttp refuse la coexistence quelle que
     * soit la valeur de TE, RFC 9112). Les deux tests s'excluaient donc
     * mutuellement. L'intention ici est la CASSE MIXTE des noms d'en-tete,
     * pas le cadrage : on garde un en-tete en casse mixte sans role de
     * cadrage. Assertions inchangees.
     * Piege inverse mesure : retirer le Content-Length en gardant le TE ne
     * marche pas non plus (garde "TE sans chunked final"). */
    static const char C[] =
        "HTTP/1.1 200 OK\r\nCoNtEnT-LeNgTh: 7\r\nX-CaSsE-MiXtE: oui\r\n\r\nBonjour";

    Resultat a = joue_bloc(A, sizeof(A) - 1);
    CHECK(a.erreur == 0, "casse_min_pas_erreur");
    CHECK(a.taille == 7, "casse_min_taille_7");
    CHECK(strcmp(a.corps, "Bonjour") == 0, "casse_min_corps");

    Resultat b = joue_bloc(B, sizeof(B) - 1);
    CHECK(b.erreur == 0, "casse_maj_pas_erreur");
    CHECK(b.taille == 7, "casse_maj_taille_7");
    CHECK(strcmp(b.corps, "Bonjour") == 0, "casse_maj_corps");

    Resultat c = joue_bloc(C, sizeof(C) - 1);
    CHECK(c.erreur == 0, "casse_mixte_pas_erreur");
    CHECK(c.taille == 7, "casse_mixte_taille_7");
    CHECK(strcmp(c.corps, "Bonjour") == 0, "casse_mixte_corps");
}

/* ---- Fragmentation : le test qui compte le plus -------------------
 * Une reponse bien formee injectee octet par octet (entetes ET corps)
 * doit donner exactement le meme resultat qu'envoyee d'un bloc. C'est
 * ce test qui attrape le piege des callbacks de span appeles par
 * morceaux : une implementation naive qui suppose un appel unique par
 * valeur d'entete ou par fragment de corps casse ici, et nulle part
 * ailleurs jusqu'a la production sur reseau lent.
 *
 * Le delai de 1 ms entre les write() est structurel : sans lui la pile
 * TCP recolle les segments et le client ne voit jamais la
 * fragmentation. nb_ecritures verifie que le decoupage a bien eu lieu. */
static void f1_fragmentation_octet_par_octet(void) {
    const char* rep = REP_CL_SIMPLE;
    size_t len = strlen(rep);

    Resultat bloc = joue_bloc(rep, len);
    CHECK(bloc.nb_ecritures == 1, "frag_ref_un_seul_write");

    Resultat oct = joue(rep, len, OWL_MOCK_DECOUPE_OCTET, NULL, 0, 1);
    CHECK(oct.nb_ecritures == len, "frag_octet_un_write_par_octet");
    CHECK(oct.erreur == bloc.erreur, "frag_octet_meme_erreur");
    CHECK(oct.statut == bloc.statut, "frag_octet_meme_statut");
    CHECK(oct.taille == bloc.taille, "frag_octet_meme_taille");
    CHECK(strcmp(oct.corps, bloc.corps) == 0, "frag_octet_meme_corps");
    /* Valeurs exactes, pas seulement l'egalite avec la reference. */
    CHECK(oct.statut == 200, "frag_octet_statut_200");
    CHECK(strcmp(oct.corps, "Bonjour") == 0, "frag_octet_corps_exact");
}

static void f1_fragmentation_chunked_octet_par_octet(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "1\r\nO\r\n"
        "2\r\nWL\r\n"
        "0\r\n\r\n";
    size_t len = sizeof(REP) - 1;

    Resultat bloc = joue_bloc(REP, len);
    Resultat oct  = joue(REP, len, OWL_MOCK_DECOUPE_OCTET, NULL, 0, 1);
    /* Pas d'egalite stricte ici, contrairement au cas Content-Length : le
     * client rend la main des qu'il a lu la ligne "0\r\n" du chunk
     * terminal et ne consomme jamais le CRLF final. Les 2 derniers write()
     * tombent donc sur une connexion deja fermee. Mesure, pas hypothese :
     * c'est ce qui distingue len-2 de len. */
    CHECK(oct.nb_ecritures >= len - 2, "frag_chunked_decoupage_par_octet");
    CHECK(oct.nb_ecritures > 1,        "frag_chunked_bien_fragmente");
    CHECK(oct.statut == bloc.statut, "frag_chunked_meme_statut");
    CHECK(oct.taille == bloc.taille, "frag_chunked_meme_taille");
    CHECK(strcmp(oct.corps, bloc.corps) == 0, "frag_chunked_meme_corps");
    CHECK(oct.taille == 3, "frag_chunked_taille_3");
    CHECK(strcmp(oct.corps, "OWL") == 0, "frag_chunked_corps_exact");
}

/* Decoupage adversarial : au milieu du CRLF de la ligne de statut, au
 * milieu du CRLFCRLF de fin d'entetes, et au milieu d'une valeur. */
static void f1_decoupage_milieu_crlf(void) {
    const char* rep = REP_CL_SIMPLE;
    size_t len = strlen(rep);

    size_t offs[3];
    offs[0] = off_apres(rep, "HTTP/1.1 200 OK\r");        /* entre CR et LF */
    offs[1] = off_apres(rep, "Content-Length: 7\r\n\r");  /* dans le CRLFCRLF */
    offs[2] = off_apres(rep, "Bonjo");                    /* dans le corps */

    Resultat r = joue(rep, len, OWL_MOCK_DECOUPE_OFFSETS, offs, 3, 1);
    CHECK(r.nb_ecritures == 4, "milieu_crlf_4_writes");
    CHECK(r.erreur == 0,   "milieu_crlf_pas_erreur");
    CHECK(r.statut == 200, "milieu_crlf_statut_200");
    CHECK(strcmp(r.corps, "Bonjour") == 0, "milieu_crlf_corps_exact");
}

/* Decoupage entre le nom d'entete et les deux-points. */
static void f1_decoupage_avant_deux_points(void) {
    const char* rep = REP_CL_SIMPLE;
    size_t len = strlen(rep);

    size_t offs[2];
    offs[0] = off_apres(rep, "Content-Type");    /* juste avant le ':' */
    offs[1] = off_apres(rep, "Content-Length");  /* idem */

    Resultat r = joue(rep, len, OWL_MOCK_DECOUPE_OFFSETS, offs, 2, 1);
    CHECK(r.nb_ecritures == 3, "avant_deux_points_3_writes");
    CHECK(r.erreur == 0,   "avant_deux_points_pas_erreur");
    CHECK(r.statut == 200, "avant_deux_points_statut_200");
    CHECK(r.taille == 7,   "avant_deux_points_taille_7");
    CHECK(strcmp(r.corps, "Bonjour") == 0, "avant_deux_points_corps_exact");
}

/* Decoupage au milieu d'une taille de chunk (hexa a deux chiffres). */
static void f1_decoupage_milieu_taille_chunk(void) {
    /* 0x0b = 11 octets : "Bonjour OWL". */
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0b\r\nBonjour OWL\r\n"
        "0\r\n\r\n";
    size_t len = sizeof(REP) - 1;

    size_t offs[2];
    offs[0] = off_apres(REP, "\r\n\r\n0");   /* entre le '0' et le 'b' */
    offs[1] = off_apres(REP, "0b\r");        /* entre CR et LF de la taille */

    Resultat r = joue(REP, len, OWL_MOCK_DECOUPE_OFFSETS, offs, 2, 1);
    CHECK(r.nb_ecritures == 3, "milieu_taille_chunk_3_writes");
    CHECK(r.erreur == 0,  "milieu_taille_chunk_pas_erreur");
    CHECK(r.taille == 11, "milieu_taille_chunk_taille_11");
    CHECK(strcmp(r.corps, "Bonjour OWL") == 0, "milieu_taille_chunk_corps_exact");
}

/* Espacement legal mais varie apres les deux-points : plusieurs espaces, une
 * tabulation, aucun espace. RFC 9110 : OWS = SP / HTAB, donc reponse bien
 * formee, famille 1. C'est le payload qui distingue un bloc rendu tel quel
 * d'un bloc renormalise. */
#define REP_OWS_VARIE \
    "HTTP/1.1 200 OK\r\n" \
    "Content-Type:    text/plain\r\n" \
    "X-Tab:\tavec-tabulation\r\n" \
    "X-Zero:sans-espace\r\n" \
    "Content-Length: 7\r\n" \
    "\r\n" \
    "Bonjour"

/* Le bloc d'entetes doit sortir avec les octets recus, pas sous une forme
 * reconstruite : un utilisateur peut calculer une empreinte ou verifier une
 * signature dessus, et la moindre renormalisation la casserait en silence. */
static void f1_entete_brut_fidele(void) {
    char brut[2048], ctype[128];
    joue_entete_brut(REP_OWS_VARIE, sizeof(REP_OWS_VARIE) - 1,
                     OWL_MOCK_DECOUPE_BLOC, 0,
                     brut, sizeof(brut), ctype, sizeof(ctype));

    CHECK(strncmp(brut, "HTTP/1.1 200 OK\r\n", 17) == 0, "brut_ligne_statut");
    CHECK(strstr(brut, "Content-Type:    text/plain\r\n") != NULL,
          "brut_espaces_multiples_conserves");
    CHECK(strstr(brut, "X-Tab:\tavec-tabulation\r\n") != NULL,
          "brut_tabulation_conservee");
    CHECK(strstr(brut, "X-Zero:sans-espace\r\n") != NULL,
          "brut_absence_espace_conservee");
    /* Le corps suit immediatement la ligne vide : la coupe du bloc tombe donc
     * pile a la fin des entetes, ni avant ni apres. */
    CHECK(finit_par(brut, "\r\n\r\nBonjour"), "brut_coupe_a_la_ligne_vide");

    /* Le bloc normalise reste la source des champs derives : la fidelite du
     * brut ne doit pas se payer d'une regression sur ce chemin. */
    CHECK(strcmp(ctype, "text/plain") == 0, "brut_content_type_derive");
}

/* Meme payload, un write() par octet : l'offset de fin d'entetes doit se
 * cumuler correctement d'une lecture a l'autre. C'est le cas ou une capture
 * naive rendrait un bloc tronque ou decale. */
static void f1_entete_brut_fragmente(void) {
    char brut[2048], ctype[128];
    joue_entete_brut(REP_OWS_VARIE, sizeof(REP_OWS_VARIE) - 1,
                     OWL_MOCK_DECOUPE_OCTET, 1,
                     brut, sizeof(brut), ctype, sizeof(ctype));

    CHECK(strncmp(brut, "HTTP/1.1 200 OK\r\n", 17) == 0, "brut_frag_ligne_statut");
    CHECK(strstr(brut, "Content-Type:    text/plain\r\n") != NULL,
          "brut_frag_espaces_multiples_conserves");
    CHECK(strstr(brut, "X-Tab:\tavec-tabulation\r\n") != NULL,
          "brut_frag_tabulation_conservee");
    CHECK(finit_par(brut, "\r\n\r\nBonjour"), "brut_frag_coupe_a_la_ligne_vide");
    CHECK(strcmp(ctype, "text/plain") == 0, "brut_frag_content_type_derive");
}

/* ================================================================
 * FAMILLE 2 — durcissement (ROUGE contre le parseur actuel)
 *
 * Deux assertions discriminantes par cas : une erreur runtime est levee, et
 * elle porte le code dedie aux reponses non conformes. Le code compte autant
 * que l'erreur : un echec reseau generique rendrait le rejet indiscernable
 * d'une coupure de connexion pour qui lit ErreurInfo.
 *
 * Le corps n'est pas asserte : quand HTTPRequete echoue elle rend Null,
 * donc une chaine vide — l'assertion "corps vide" serait verte par
 * coincidence sur plusieurs de ces cas des aujourd'hui et ne prouverait
 * rien. Ce qui distingue le rejet du silence, c'est ErreurDetectee.
 * ================================================================ */

#define CODE_NON_CONFORME "E_NATIVE_HTTP_MALFORMED_RESPONSE"

/* Classe 1 : Content-Length ET Transfer-Encoding simultanes.
 * Parseur maison : Transfer-Encoding gagne, decode chunked, rc 0. */
static void f2_cl_et_te(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 7\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\nBonjour\r\n0\r\n\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_cl_et_te_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_cl_et_te_code_dedie");
}

/* Classe 2a : Content-Length duplique avec deux valeurs divergentes.
 * Parseur maison : find_header rend la premiere, rc 0. */
static void f2_cl_duplique(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 7\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "Bonjour";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_cl_duplique_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_cl_duplique_code_dedie");
}

/* Classe 2b : Content-Length non numerique.
 * Parseur maison : la boucle ne retient que les chiffres — "1x2" vaut 12,
 * "abc" vaut 0 et rend une chaine vide sans le dire. */
static void f2_cl_non_numerique(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: abc\r\n"
        "\r\n"
        "Bonjour";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_cl_non_numerique_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_cl_non_numerique_code_dedie");

    /* "1x2" vaut 12 pour le parseur maison. Le corps fait exactement 12
     * octets : sinon le corps tronque ferait echouer la lecture et
     * l'assertion passerait au vert par accident, sans que la moindre
     * validation d'entete soit intervenue. */
    static const char REP2[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 1x2\r\n"
        "\r\n"
        "BonjourOWL!!";
    Resultat r2 = joue_bloc(REP2, sizeof(REP2) - 1);
    CHECK(r2.erreur == 1, "f2_cl_chiffres_melanges_rejete");
    CHECK(strcmp(r2.code, CODE_NON_CONFORME) == 0, "f2_cl_chiffres_melanges_code_dedie");
}

/* Classe 3 : espace avant les deux-points.
 * Parseur maison : l'entete n'est simplement pas reconnu — on retombe sur
 * un corps delimite par fermeture et personne ne signale rien. */
static void f2_espace_avant_deux_points(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length : 7\r\n"
        "\r\n"
        "Bonjour";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_espace_avant_deux_points_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_espace_avant_deux_points_code_dedie");
}

/* Classe 4 : CR seul comme delimiteur (CR non suivi de LF).
 * Parseur maison : la valeur s'arrete au CR, la fin de ligne est
 * "trouvee" quand meme, rc 0. */
static void f2_cr_seul(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 7\rX-Injecte: 1\r\n"
        "\r\n"
        "Bonjour";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_cr_seul_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_cr_seul_code_dedie");
}

/* Classe 5 : chunked non final dans Transfer-Encoding.
 * Parseur maison : le prefixe "chunked" suffit a declencher le decodage. */
static void f2_chunked_non_final(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked, gzip\r\n"
        "\r\n"
        "7\r\nBonjour\r\n0\r\n\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_chunked_non_final_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_chunked_non_final_code_dedie");
}

/* Classe 6 : caractere de controle dans une valeur d'entete.
 * Parseur maison : aucune validation des octets de valeur. */
static void f2_controle_dans_valeur(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "X-Mauvais: a\x01" "b\r\n"
        "Content-Length: 7\r\n"
        "\r\n"
        "Bonjour";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_controle_dans_valeur_rejete");
    CHECK(strcmp(r.code, CODE_NON_CONFORME) == 0, "f2_controle_dans_valeur_code_dedie");
}

/* Le detail du message doit etre dans la langue du message. Assertion portee
 * sur la forme francaise, qui appartient a OWL : le libelle d'origine du
 * parseur, lui, changerait a une montee de version sans que quoi que ce soit
 * soit casse — l'asserter transformerait une mise a jour en faux echec. */
static void f2_detail_en_francais(void) {
    static const char REP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 7\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\nBonjour\r\n0\r\n\r\n";
    Resultat r = joue_bloc(REP, sizeof(REP) - 1);
    CHECK(r.erreur == 1, "f2_detail_fr_rejete");
    CHECK(strstr(r.message, "ne peuvent pas coexister") != NULL,
          "f2_detail_fr_traduit");
    /* Les noms d'entetes, eux, ne se traduisent pas. */
    CHECK(strstr(r.message, "Transfer-Encoding") != NULL,
          "f2_detail_fr_garde_noms_entetes");

    static const char REP2[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 7\r\n"
        "Content-Length: 3\r\n"
        "\r\n"
        "Bonjour";
    Resultat r2 = joue_bloc(REP2, sizeof(REP2) - 1);
    CHECK(r2.erreur == 1, "f2_detail_fr_dup_rejete");
    CHECK(strstr(r2.message, "présent en double") != NULL,
          "f2_detail_fr_dup_traduit");
}

/* ================================================================
 * FAMILLE 3 — HTTPEnvoie contre un vrai pair, SANS mock de la native
 *
 * Toute la couverture existante de HTTPEnvoie (tests/runtime/) mocke
 * HTTPEnvoie elle-meme : la native reelle n'est jamais exercee. D'ou un
 * retour Null systematique reste invisible. Ici on pilote la vraie native
 * contre le pair TCP local et on lit l'instance qu'elle rend.
 *
 * ROUGE par construction avant correctif : le modele httpReponse est
 * cherche dans la table des globales alors qu'il vit dans celle des types,
 * donc la native rend Null sur toute reponse — reelle ou non.
 * ================================================================ */

/* Espacements varies volontaires (plusieurs espaces, tabulation, aucun
 * espace) : c'est ce qui distingue un bloc d'entetes rendu tel quel d'un
 * bloc renormalise, et donc ce qui verifie reellement EnteteBrut. */
#define REP_ENVOIE \
    "HTTP/1.1 201 Created\r\n" \
    "Content-Type:    text/plain\r\n" \
    "X-Test:\t42\r\n" \
    "X-Zero:sans-espace\r\n" \
    "Content-Length: 7\r\n" \
    "\r\n" \
    "Bonjour"

typedef struct {
    int  interprete_ok;
    int  typevar;         /* TypeVar(reponse) : 0 = Null */
    int  code;            /* CodeEtat, -1 si non lu */
    char desc[64];
    char corps[512];
    char ctype[128];
    char urlf[256];
    char hdr[128];        /* Entete["X-Test"] */
    char brut[2048];
    int  port;            /* port ephemere du pair, pour asserter ..URL */
} ResultatEnvoie;

/* Chaque lecture de champ est gardee par TypeVar : un acces sur Null
 * avorterait le script a la premiere ligne et on ne saurait pas distinguer
 * "rendu Null" de "champ absent". Ici, l'etat courant sort toujours. */
static ResultatEnvoie joue_envoie(const char* octets, size_t len) {
    ResultatEnvoie r;
    memset(&r, 0, sizeof(r));
    r.code = -1;

    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets                = octets;
    cfg.octets_len            = len;
    cfg.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg.sentinelle_lecture    = "\r\n\r\n";
    cfg.fermer_apres_ecriture = 1;

    OwlMockPeer* p = owl_mock_peer_demarre(&cfg);
    if (!p) return r;
    if (!owl_mock_peer_pret(p)) { owl_mock_peer_libere(p); return r; }

    r.port = owl_mock_peer_port(p);

    int id = ++g_seq;
    char src[2048];
    snprintf(src, sizeof(src),
        "q%d est un httpRequete\n"
        "q%d..URL = \"http://127.0.0.1:%d/x\"\n"
        "p%d est un httpReponse = HTTPEnvoie(q%d)\n"
        "Trace(\"TVAR=\" + TypeVar(p%d))\n"
        "SI TypeVar(p%d) <> 0 ALORS\n"
        "    Trace(\"STAT=\" + p%d..CodeEtat)\n"
        "    Trace(\"DESC=[\" + p%d..DescriptionCodeEtat + \"]\")\n"
        "    Trace(\"BODY=[\" + p%d..Contenu + \"]\")\n"
        "    Trace(\"CTYP=[\" + p%d..ContentType + \"]\")\n"
        "    Trace(\"URLF=[\" + p%d..URL + \"]\")\n"
        "    Trace(\"HDRX=[\" + p%d..Entete[\"X-Test\"] + \"]\")\n"
        "    Trace(\"BRUT=[\" + p%d..EnteteBrut + \"]\")\n"
        "FIN\n",
        id, id, owl_mock_peer_port(p), id, id, id, id,
        id, id, id, id, id, id, id);

    r.interprete_ok = (capture_interpret(src) == INTERPRET_OK);
    r.typevar = lit_entier("TVAR");
    r.code    = lit_entier("STAT");
    lit_entre_crochets("DESC", r.desc,  sizeof(r.desc));
    lit_entre_crochets("BODY", r.corps, sizeof(r.corps));
    lit_entre_crochets("CTYP", r.ctype, sizeof(r.ctype));
    lit_entre_crochets("URLF", r.urlf,  sizeof(r.urlf));
    lit_entre_crochets("HDRX", r.hdr,   sizeof(r.hdr));
    lit_entre_crochets("BRUT", r.brut,  sizeof(r.brut));

    owl_mock_peer_joint(p);
    owl_mock_peer_libere(p);
    return r;
}

/* Le contrat public de la native : elle rend une INSTANCE, pas Null. */
static void f3_envoie_rend_une_instance(void) {
    ResultatEnvoie r = joue_envoie(REP_ENVOIE, sizeof(REP_ENVOIE) - 1);
    CHECK(r.interprete_ok, "envoie_interprete_ok");
    CHECK(r.typevar != 0,  "envoie_pas_null");
}

/* Chaque champ documente de httpReponse doit etre renseigne depuis la
 * reponse reellement recue, pas laisse a sa valeur de modele. */
static void f3_envoie_champs_renseignes(void) {
    ResultatEnvoie r = joue_envoie(REP_ENVOIE, sizeof(REP_ENVOIE) - 1);
    CHECK(r.code == 201, "envoie_code_etat_201");
    CHECK(strcmp(r.desc,  "Created")    == 0, "envoie_description_code_etat");
    CHECK(strcmp(r.corps, "Bonjour")    == 0, "envoie_contenu_exact");
    CHECK(strcmp(r.ctype, "text/plain") == 0, "envoie_content_type");
    CHECK(strcmp(r.hdr,   "42")         == 0, "envoie_entete_associatif");
}

/* URL finale : reconstruite apres redirections, donc lisible meme quand il
 * n'y en a pas eu.
 *
 * Le port doit y figurer des lors que l'URL d'origine en portait un
 * : sans lui, l'URL rendue designe une autre ressource que celle
 * qui a repondu. Le pair ecoute sur un port ephemere, donc l'attendu se
 * construit a partir du port reellement obtenu. */
static void f3_envoie_url_finale(void) {
    ResultatEnvoie r = joue_envoie(REP_ENVOIE, sizeof(REP_ENVOIE) - 1);
    char attendu[128];
    snprintf(attendu, sizeof(attendu), "http://127.0.0.1:%d/x", r.port);
    CHECK(strcmp(r.urlf, attendu) == 0, "envoie_url_finale_avec_port");
    CHECK(finit_par(r.urlf, "/x"), "envoie_url_chemin");
}

/* Lit le seul ..URL d'une requete vers une cible sans listener : la requete
 * echoue, mais l'instance de reponse est rendue et son URL renseignee. Seul
 * moyen d'observer une URL sans port — un pair de test ecoute toujours sur
 * un port ephemere, donc explicite. */
static void lit_url_finale_seule(const char* url, char* dst, size_t cap) {
    int id = ++g_seq;
    char src[512];
    snprintf(src, sizeof(src),
        "u%d est un httpReponse = HTTPEnvoie(\"%s\")\n"
        "SI TypeVar(u%d) <> 0 ALORS\n"
        "    Trace(\"URLF=[\" + u%d..URL + \"]\")\n"
        "FIN\n",
        id, url, id, id);
    dst[0] = '\0';
    capture_interpret(src);
    lit_entre_crochets("URLF", dst, cap);
}

/* Piege symetrique du correctif : preserver le port ne doit pas revenir a en
 * inventer un. Une URL sans port reste sans port, quel que soit le schema —
 * sinon http://hote/x deviendrait http://hote:80/x, tout aussi faux que la
 * perte du port. */
static void f3_envoie_url_finale_port_par_defaut(void) {
    char u[256];
    lit_url_finale_seule("http://127.0.0.1/x", u, sizeof(u));
    CHECK(strcmp(u, "http://127.0.0.1/x") == 0, "envoie_url_finale_http_sans_port_ajoute");
    lit_url_finale_seule("https://127.0.0.1/x", u, sizeof(u));
    CHECK(strcmp(u, "https://127.0.0.1/x") == 0, "envoie_url_finale_https_sans_port_ajoute");
    /* Port explicite egal au port par defaut : rendu tel qu'il a ete ecrit. */
    lit_url_finale_seule("http://127.0.0.1:80/x", u, sizeof(u));
    CHECK(strcmp(u, "http://127.0.0.1:80/x") == 0, "envoie_url_finale_port_defaut_explicite_conserve");
}

/* Apres redirection, l'URL rendue est celle de la ressource qui a finalement
 * repondu — donc le port de la cible de redirection, pas celui de depart.
 * Deux pairs : le premier renvoie une 302 absolue vers le second. */
static void f3_envoie_url_finale_apres_redirection(void) {
    OwlMockPeerConfig cfg_b;
    owl_mock_peer_config_defaut(&cfg_b);
    cfg_b.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg_b.octets                = REP_ENVOIE;
    cfg_b.octets_len            = sizeof(REP_ENVOIE) - 1;
    cfg_b.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg_b.sentinelle_lecture    = "\r\n\r\n";
    cfg_b.fermer_apres_ecriture = 1;

    OwlMockPeer* b = owl_mock_peer_demarre(&cfg_b);
    if (!b || !owl_mock_peer_pret(b)) {
        if (b) owl_mock_peer_libere(b);
        CHECK(0, "redirection_pair_cible_pret");
        return;
    }
    int port_b = owl_mock_peer_port(b);

    char rep302[256];
    int n302 = snprintf(rep302, sizeof(rep302),
        "HTTP/1.1 302 Found\r\n"
        "Location: http://127.0.0.1:%d/y\r\n"
        "Content-Length: 0\r\n"
        "\r\n", port_b);

    OwlMockPeerConfig cfg_a;
    owl_mock_peer_config_defaut(&cfg_a);
    cfg_a.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg_a.octets                = rep302;
    cfg_a.octets_len            = (size_t)n302;
    cfg_a.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg_a.sentinelle_lecture    = "\r\n\r\n";
    cfg_a.fermer_apres_ecriture = 1;

    OwlMockPeer* a = owl_mock_peer_demarre(&cfg_a);
    if (!a || !owl_mock_peer_pret(a)) {
        if (a) owl_mock_peer_libere(a);
        owl_mock_peer_libere(b);
        CHECK(0, "redirection_pair_depart_pret");
        return;
    }

    int id = ++g_seq;
    char src[512];
    snprintf(src, sizeof(src),
        "d%d est un httpReponse = HTTPEnvoie(\"http://127.0.0.1:%d/x\")\n"
        "SI TypeVar(d%d) <> 0 ALORS\n"
        "    Trace(\"STAT=\" + d%d..CodeEtat)\n"
        "    Trace(\"URLF=[\" + d%d..URL + \"]\")\n"
        "FIN\n",
        id, owl_mock_peer_port(a), id, id, id);
    capture_interpret(src);
    int stat = lit_entier("STAT");
    char urlf[256];
    lit_entre_crochets("URLF", urlf, sizeof(urlf));

    owl_mock_peer_joint(a);
    owl_mock_peer_joint(b);
    owl_mock_peer_libere(a);
    owl_mock_peer_libere(b);

    char attendu[128];
    snprintf(attendu, sizeof(attendu), "http://127.0.0.1:%d/y", port_b);
    CHECK(stat == 201, "redirection_suivie_jusqu_a_la_cible");
    CHECK(strcmp(urlf, attendu) == 0, "redirection_url_finale_porte_le_port_cible");
}

/* EnteteBrut porte le bloc tel qu'il est arrive sur le fil. C'est la
 * verification que  n'a pas pu faire : elle passait par la chaine de
 * reponse faute d'instance exploitable. */
static void f3_envoie_entete_brut(void) {
    ResultatEnvoie r = joue_envoie(REP_ENVOIE, sizeof(REP_ENVOIE) - 1);
    CHECK(strncmp(r.brut, "HTTP/1.1 201 Created\r\n", 22) == 0,
          "envoie_brut_ligne_statut");
    CHECK(strstr(r.brut, "Content-Type:    text/plain\r\n") != NULL,
          "envoie_brut_espaces_multiples_conserves");
    CHECK(strstr(r.brut, "X-Test:\t42\r\n") != NULL,
          "envoie_brut_tabulation_conservee");
    CHECK(strstr(r.brut, "X-Zero:sans-espace\r\n") != NULL,
          "envoie_brut_absence_espace_conservee");
    CHECK(finit_par(r.brut, "\r\n\r\n"), "envoie_brut_finit_a_la_ligne_vide");
    /* Le corps n'appartient pas au bloc d'entetes. */
    CHECK(strstr(r.brut, "Bonjour") == NULL, "envoie_brut_sans_corps");
}

/* HTTPTransfereFichier clone le meme modele : meme defaut, meme correctif.
 * Le corps part dans un fichier, donc on asserte l'instance et son statut. */
static void f3_transfere_fichier_rend_une_instance(void) {
    char DEST[128];
    fixture_unique(DEST, sizeof(DEST), "_owl_http_envoie_test_", ".bin");
    owl_unlink(DEST);

    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets                = REP_ENVOIE;
    cfg.octets_len            = sizeof(REP_ENVOIE) - 1;
    cfg.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg.sentinelle_lecture    = "\r\n\r\n";
    cfg.fermer_apres_ecriture = 1;

    OwlMockPeer* p = owl_mock_peer_demarre(&cfg);
    if (!p) { CHECK(0, "transfere_pair_demarre"); return; }
    if (!owl_mock_peer_pret(p)) {
        owl_mock_peer_libere(p);
        CHECK(0, "transfere_pair_pret");
        return;
    }

    int id = ++g_seq;
    char src[1024];
    snprintf(src, sizeof(src),
        "t%d est un httpReponse = HTTPTransfereFichier("
            "\"http://127.0.0.1:%d/x\", \"%s\")\n"
        "Trace(\"TVAR=\" + TypeVar(t%d))\n"
        "SI TypeVar(t%d) <> 0 ALORS\n"
        "    Trace(\"STAT=\" + t%d..CodeEtat)\n"
        "    Trace(\"CTYP=[\" + t%d..ContentType + \"]\")\n"
        "FIN\n",
        id, owl_mock_peer_port(p), DEST, id, id, id, id);

    int ok = (capture_interpret(src) == INTERPRET_OK);
    int tvar = lit_entier("TVAR");
    int stat = lit_entier("STAT");
    char ctype[128];
    lit_entre_crochets("CTYP", ctype, sizeof(ctype));

    owl_mock_peer_joint(p);
    owl_mock_peer_libere(p);
    owl_unlink(DEST);

    CHECK(ok,          "transfere_interprete_ok");
    CHECK(tvar != 0,   "transfere_pas_null");
    CHECK(stat == 201, "transfere_code_etat_201");
    CHECK(strcmp(ctype, "text/plain") == 0, "transfere_content_type");
}

/* Le silence est un defaut distinct du mauvais namespace : une native qui ne
 * peut pas construire sa valeur de retour doit le dire. On retire le modele
 * de la table des types le temps d'une requete — seul moyen d'atteindre cette
 * branche — puis on le remet. */
static void f3_modele_absent_leve_une_erreur(void) {
    ObjString* cle = obj_copy_string("httpReponse", 11);
    vm_push(OBJ_VAL(cle));
    Value sauvegarde;
    if (!table_get(&vm.types, cle, &sauvegarde)) {
        vm_pop();
        CHECK(0, "modele_absent_preparation");
        return;
    }
    table_delete(&vm.types, cle);

    OwlMockPeerConfig cfg;
    owl_mock_peer_config_defaut(&cfg);
    cfg.role                  = OWL_MOCK_ROLE_ECOUTE;
    cfg.octets                = REP_ENVOIE;
    cfg.octets_len            = sizeof(REP_ENVOIE) - 1;
    cfg.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    cfg.sentinelle_lecture    = "\r\n\r\n";
    cfg.fermer_apres_ecriture = 1;

    OwlMockPeer* p = owl_mock_peer_demarre(&cfg);
    int erreur = -1;
    char code[96]; code[0] = '\0';
    if (p && owl_mock_peer_pret(p)) {
        int id = ++g_seq;
        char src[768];
        snprintf(src, sizeof(src),
            "z%d est un httpReponse = HTTPEnvoie(\"http://127.0.0.1:%d/x\")\n"
            "e%d est un booleen = ErreurDetectee\n"
            "c%d est une chaine = ErreurInfo(errCode)\n"
            "Trace(\"ERREUR=\" + e%d)\n"
            "Trace(\"CODE=[\" + c%d + \"]\")\n",
            id, owl_mock_peer_port(p), id, id, id, id);
        capture_interpret(src);
        erreur = lit_entier("ERREUR");
        lit_entre_crochets("CODE", code, sizeof(code));
        owl_mock_peer_joint(p);
    }
    if (p) owl_mock_peer_libere(p);

    vm_type_register("httpReponse", sauvegarde);
    vm_pop(); /* cle */

    CHECK(erreur == 1, "modele_absent_rejete");
    CHECK(strcmp(code, "E_NATIVE_HTTP_RESPONSE_TEMPLATE_MISSING") == 0,
          "modele_absent_code_dedie");
}

/* ================================================================
 * Entree
 * ================================================================ */

int http_llhttp_integration_self_test_run(void) {
    /* Un proxy herite de l'environnement detournerait toutes les requetes
     * et rendrait ce self-test dependant de la machine. */
    static char e1[] = "http_proxy=";
    static char e2[] = "HTTP_PROXY=";
    static char e3[] = "https_proxy=";
    static char e4[] = "HTTPS_PROXY=";
    static char e5[] = "no_proxy=127.0.0.1";
    owl_putenv(e1); owl_putenv(e2); owl_putenv(e3); owl_putenv(e4); owl_putenv(e5);

    g_pass_f1 = g_fail_f1 = g_pass_f2 = g_fail_f2 = 0;

    fprintf(stderr, "\n==== HTTP PARSING REPONSE — FAMILLE 1 (non-regression) ====\n");
    g_famille = 1;
    f1_content_length();
    f1_chunked();
    f1_chunked_taille_1();
    f1_delimite_par_fermeture();
    f1_entetes_repetes();
    f1_204_sans_corps();
    f1_304_sans_corps();
    f1_gzip();
    f1_statuts_varies();
    f1_casses_et_espaces();
    f1_fragmentation_octet_par_octet();
    f1_fragmentation_chunked_octet_par_octet();
    f1_decoupage_milieu_crlf();
    f1_decoupage_avant_deux_points();
    f1_decoupage_milieu_taille_chunk();
    f1_entete_brut_fidele();
    f1_entete_brut_fragmente();

    fprintf(stderr, "\n==== HTTP PARSING REPONSE — FAMILLE 2 (durcissement) ====\n");
    fprintf(stderr, "  (rejet + code dedie attendus sur chaque cas)\n");
    g_famille = 2;
    f2_cl_et_te();
    f2_cl_duplique();
    f2_cl_non_numerique();
    f2_espace_avant_deux_points();
    f2_cr_seul();
    f2_chunked_non_final();
    f2_controle_dans_valeur();
    f2_detail_en_francais();

    fprintf(stderr, "\n==== HTTPEnvoie NON MOCKEE — FAMILLE 3 (valeur de retour) ====\n");
    g_famille = 3;
    f3_envoie_rend_une_instance();
    f3_envoie_champs_renseignes();
    f3_envoie_url_finale();
    f3_envoie_url_finale_port_par_defaut();
    f3_envoie_url_finale_apres_redirection();
    f3_envoie_entete_brut();
    f3_transfere_fichier_rend_une_instance();
    f3_modele_absent_leve_une_erreur();

    fprintf(stderr, "\nhttp_llhttp_integration self-test:\n");
    fprintf(stderr, "  famille 1 (non-regression) : %d pass, %d fail\n",
            g_pass_f1, g_fail_f1);
    fprintf(stderr, "  famille 2 (durcissement)   : %d pass, %d fail\n",
            g_pass_f2, g_fail_f2);
    fprintf(stderr, "  famille 3 (valeur retour)  : %d pass, %d fail\n",
            g_pass_f3, g_fail_f3);

    return g_fail_f1 + g_fail_f2 + g_fail_f3;
}
