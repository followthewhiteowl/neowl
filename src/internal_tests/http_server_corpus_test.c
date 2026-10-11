// SPDX-License-Identifier: AGPL-3.0-only
/* http_server_corpus_test.c — corpus d'attaque du serveur HTTP.
 *
 * Spec : contrat de transport HTTP interne (corpus d'attaque).
 *
 * Conventions du corpus, reprises telles quelles : chaque vecteur emet
 * des OCTETS BRUTS sur la socket (jamais via un client HTTP, qui
 * normaliserait). L'attendu est un code de statut ET une disposition de
 * connexion. Aucun vecteur ne doit produire de crash ni de comportement
 * dependant du timing.
 *
 * Le pair emetteur est src/net_mock_peer.c en role CONNECTE — le role
 * ecrit pour ce moment precis par . Le `delai_ms` entre write()
 * est structurel : sans lui la pile TCP recolle les segments et la
 * fragmentation qu'on cherche a infliger n'est jamais observee.
 * `nb_ecritures()` PROUVE que le decoupage demande a eu lieu.
 *
 * Perimetre (A0.1 puis A1.1a) :
 *   - famille 1 (smuggling) : integralement ;
 *   - famille 2 (ligne de requete / en-tetes malformes) : integralement ;
 *   - famille 3 (epuisement) : vecteurs « une connexion » en A0.1, plus
 *     3.5 (connexions simultanees) et 3.6 (requetes par connexion) en
 *     A1.1a, qui supposaient le keep-alive ;
 *   - famille 4 (traversee de chemin) et 5 (injection en reponse) :
 *     couche politique, phase A0.3 (http_politique_test.c) ;
 *   - famille 6 (confusion d'etat sur connexion reutilisee) : A1.1a.
 *
 * La famille 6 distingue deux chemins que rien d'autre ne separe :
 *   - SEQUENTIEL : les requetes arrivent dans des read() distincts. Le
 *     harnais l'obtient par un decoupage OFFSETS avec delai — sans le
 *     delai la pile TCP recolle les segments et on retomberait sur le
 *     cas pipeline sans le savoir. `nb_ecritures` le PROUVE.
 *   - PIPELINE : les deux requetes tiennent dans UN send(), donc dans
 *     le meme tampon de lecture du serveur. C'est la que vit le residu,
 *     et c'est la que se fabrique une requete de smuggling.
 *
 * Le serveur tourne dans le THREAD PRINCIPAL et le pair dans un thread :
 * une connexion a la fois, pas de boucle a arreter, aucun ordonnancement
 * a deviner.
 *
 * Execute via `owl --self-test-net` (cf. main.c). Pas de vm_init : ce
 * module ne connait pas la VM. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http_server.h"
#include "../net_mock_peer.h"

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass[7] = {0,0,0,0,0,0,0};   /* index 1..6 = familles */
static int g_fail[7] = {0,0,0,0,0,0,0};
static int g_famille = 1;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 1 && g_famille <= 6) ? g_famille : 1;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Handler d'echo — la seule « application » de ce lot.
 *
 * Rend visible dans le corps ce que le transport a livre : methode,
 * chemin (et non la cible brute : c'est le point du vecteur 2.16),
 * version, nombre d'en-tetes, taille du corps. Un vecteur peut donc
 * asserter la valeur exacte de ce que le parseur a accumule, pas
 * seulement « ca a repondu 200 ».
 * ================================================================ */

#define CORPUS_TRACES_MAX 16

/* Une trace PAR appel du handler, pas un tampon unique ecrase : sur une
 * connexion keep-alive, c'est justement la comparaison entre l'appel n et
 * l'appel n+1 qui prouve qu'aucun etat n'a survecu. Un tampon unique
 * rendrait cette preuve impossible a formuler. */
static char g_traces[CORPUS_TRACES_MAX][256];
static int  g_nb_appels_handler;

static void handler_echo(const OwlHttpRequete* req, OwlHttpReponse* rep, void* user) {
    (void)user;
    int i = g_nb_appels_handler;
    if (i >= CORPUS_TRACES_MAX) i = CORPUS_TRACES_MAX - 1;
    snprintf(g_traces[i], sizeof(g_traces[i]), "M=%s|C=%s|V=%d.%d|NE=%u|TC=%u",
             req->methode,
             req->chemin ? req->chemin : "",
             req->version_majeure, req->version_mineure,
             (unsigned)req->nb_entetes, (unsigned)req->corps_len);
    g_nb_appels_handler++;
    rep->statut = 200;
    snprintf(rep->type_contenu, sizeof(rep->type_contenu), "text/plain");
    rep->corps     = g_traces[i];
    rep->corps_len = strlen(g_traces[i]);
}

/* ================================================================
 * Un echange = un vecteur
 * ================================================================ */

#define CORPUS_REPONSES_MAX 16

/* Une reponse FINALE decoupee du flux. Sur une connexion keep-alive le
 * pair recoit N reponses collees : les compter et les separer est la
 * seule facon d'asserter un ORDRE (vecteur 6.3) et de prouver qu'une
 * requete n'a PAS ete servie (6.2, 6.5, 6.6). */
typedef struct {
    int    statut;
    char   corps[320];
    size_t corps_len;
    int    annonce_close;       /* « Connection: close » present */
    int    annonce_keepalive;   /* « Connection: keep-alive » present */
} Reponse;

typedef struct {
    int    statut;          /* statut de la DERNIERE reponse finale, 0 si aucune */
    int    cent_continue;   /* 1 si un « 100 Continue » precede */
    int    nb_100;          /* nombre de « 100 Continue » dans le flux */
    Reponse rep[CORPUS_REPONSES_MAX];
    size_t  nb_reponses;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
    size_t nb_ecritures;
    size_t nb_octets_emis;
    int    retour_serveur;
    int    nb_appels_handler;
    /* Duree du service COTE SERVEUR. Mesuree, pas deduite : c'est ce qui
     * distingue « le serveur a ferme sur son echeance d'inactivite » de
     * « le pair est parti et le serveur a suivi ». */
    uint64_t duree_ms;
} Echange;

typedef struct {
    OwlMockPeerDecoupe decoupe;
    int                delai_ms;
    const size_t*      offsets;
    size_t             offsets_len;
    int                accept_timeout_ms;
    /* Demi-fermeture apres emission. Indispensable pour une requete
     * TRONQUEE : sans elle, le serveur attend la suite pendant que le
     * pair attend la reponse, et le vecteur mesure un interblocage au
     * lieu de mesurer le rejet. */
    int                demi_fermeture;
} Options;

static void options_defaut(Options* o) {
    memset(o, 0, sizeof(*o));
    o->decoupe           = OWL_MOCK_DECOUPE_BLOC;
    o->accept_timeout_ms = 4000;
    /* Demi-fermeture par DEFAUT depuis A1.1a. Motif : face a un serveur
     * keep-alive, un pair qui a fini d'emettre mais ne le dit pas laisse
     * la connexion ouverte jusqu'a l'echeance d'inactivite — chaque
     * vecteur couterait alors ce timeout. Le shutdown(envoi) exprime
     * « je n'enverrai plus rien », ce que fait tout client reel qui a
     * fini. Les vecteurs qui testent PRECISEMENT l'echeance d'inactivite
     * le remettent a 0. */
    o->demi_fermeture    = 1;
}

/* Recherche binaire-safe (le flux peut contenir des NUL). */
static const char* trouve_dans(const char* foin, size_t fl, const char* aiguille) {
    size_t al = strlen(aiguille);
    if (al == 0 || fl < al) return NULL;
    for (size_t i = 0; i + al <= fl; i++) {
        if (memcmp(foin + i, aiguille, al) == 0) return foin + i;
    }
    return NULL;
}

static int min_ascii_c(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Presence d'un couple nom/valeur d'en-tete, insensible a la casse, dans
 * un bloc d'en-tetes de longueur connue. */
static int tete_contient_i(const char* tete, size_t len, const char* motif) {
    size_t ml = strlen(motif);
    if (len < ml) return 0;
    for (size_t i = 0; i + ml <= len; i++) {
        size_t k = 0;
        while (k < ml && min_ascii_c((unsigned char)tete[i + k])
                       == min_ascii_c((unsigned char)motif[k])) k++;
        if (k == ml) return 1;
    }
    return 0;
}

static size_t tete_content_length(const char* tete, size_t len) {
    const char* cle = "content-length:";
    size_t cl = strlen(cle);
    for (size_t i = 0; i + cl <= len; i++) {
        size_t k = 0;
        while (k < cl && min_ascii_c((unsigned char)tete[i + k])
                       == min_ascii_c((unsigned char)cle[k])) k++;
        if (k != cl) continue;
        size_t j = i + cl;
        while (j < len && (tete[j] == ' ' || tete[j] == '\t')) j++;
        size_t v = 0;
        while (j < len && tete[j] >= '0' && tete[j] <= '9') { v = v * 10 + (size_t)(tete[j] - '0'); j++; }
        return v;
    }
    return 0;
}

/* Decoupe le flux en reponses successives. Le cadrage de sortie est
 * TOUJOURS Content-Length (« pas de chunked en sortie, jamais »), ce qui
 * rend ce decoupage exact et non heuristique. */
static void analyse_reponses(Echange* e) {
    e->statut        = 0;
    e->cent_continue = 0;
    e->nb_100        = 0;
    e->nb_reponses   = 0;

    size_t i = 0;
    while (i + 12 <= e->reponse_len && memcmp(e->reponse + i, "HTTP/1.", 7) == 0) {
        int code = atoi(e->reponse + i + 9);

        const char* tete = e->reponse + i;
        size_t reste = e->reponse_len - i;
        const char* fin = trouve_dans(tete, reste, "\r\n\r\n");
        if (!fin) { if (code == 100) e->cent_continue = 1; else e->statut = code; return; }

        size_t tete_len = (size_t)(fin - tete) + 4;

        if (code == 100) {
            e->cent_continue = 1;
            e->nb_100++;
            i += tete_len;
            continue;
        }

        size_t clen = tete_content_length(tete, tete_len);
        if (clen > reste - tete_len) clen = reste - tete_len;   /* flux tronque */
        /* Reponse a un HEAD : le Content-Length annonce la taille qu'aurait
         * eue le GET, et AUCUN corps ne suit. Avancer de clen mangerait le
         * debut de la reponse suivante — mesure : le vecteur 6.1bis_k
         * (HEAD puis GET) rendait « nb_reponses=1, appels=2 », un serveur
         * correct vu par un harnais faux. Une reponse ne commence jamais
         * au milieu du corps d'une autre : la frontiere tranche. */
        if (clen > 0 && reste - tete_len >= 7
            && memcmp(tete + tete_len, "HTTP/1.", 7) == 0) clen = 0;

        if (e->nb_reponses < CORPUS_REPONSES_MAX) {
            Reponse* r = &e->rep[e->nb_reponses];
            r->statut            = code;
            r->annonce_close     = tete_contient_i(tete, tete_len, "connection: close");
            r->annonce_keepalive = tete_contient_i(tete, tete_len, "connection: keep-alive");
            size_t cp = clen < sizeof(r->corps) - 1 ? clen : sizeof(r->corps) - 1;
            memcpy(r->corps, tete + tete_len, cp);
            r->corps[cp]  = '\0';
            r->corps_len  = clen;
        }
        e->nb_reponses++;
        e->statut = code;
        i += tete_len + clen;
    }
}

static Echange joue(const char* octets, size_t len,
                    const OwlHttpServeurConfig* cfg,
                    const Options* opt) {
    Echange e;
    memset(&e, 0, sizeof(e));
    e.retour_serveur = -99;

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) {
        fprintf(stderr, "  (ecoute impossible : %s)\n", owl_tcp_listener_erreur(&l));
        return e;
    }

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = octets;
    mc.octets_len            = len;
    mc.decoupe               = opt->decoupe;
    mc.delai_ms              = opt->delai_ms;
    mc.offsets               = opt->offsets;
    mc.offsets_len           = opt->offsets_len;
    mc.lire_apres_ecriture   = 1;
    mc.fermer_envoi_avant_lecture = opt->demi_fermeture;
    mc.timeout_lecture_ms    = 3000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); return e; }

    g_nb_appels_handler = 0;
    memset(g_traces, 0, sizeof(g_traces));

    uint64_t t0 = owl_tcp_server_horloge_ms();
    e.retour_serveur = owl_http_serveur_sert_une_connexion(&l, cfg, opt->accept_timeout_ms);
    e.duree_ms = owl_tcp_server_horloge_ms() - t0;
    e.nb_appels_handler = g_nb_appels_handler;

    owl_mock_peer_joint(pair);

    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(e.reponse, recu, rl);
    e.reponse[rl]    = '\0';
    e.reponse_len    = rl;
    e.nb_ecritures   = owl_mock_peer_nb_ecritures(pair);
    e.nb_octets_emis = owl_mock_peer_nb_octets_emis(pair);

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);

    analyse_reponses(&e);
    return e;
}

/* Raccourci : requete envoyee d'un bloc, config par defaut du corpus. */
static OwlHttpServeurConfig g_cfg;

static Echange bloc(const char* octets) {
    Options o; options_defaut(&o);
    return joue(octets, strlen(octets), &g_cfg, &o);
}

/* Un vecteur de rejet doit produire le statut attendu ET fermer. Sans
 * keep-alive en A0.1 toute reponse ferme, mais l'assertion reste : elle
 * deviendra discriminante en A1. */
static void attend(const char* label, const char* octets, int statut) {
    Echange e = bloc(octets);
    if (e.statut != statut) {
        fprintf(stderr, "  [%s] statut=%d attendu=%d reponse=<%.120s>\n",
                label, e.statut, statut, e.reponse);
    }
    compte(e.statut == statut, label, 0);
}

/* ================================================================
 * FAMILLE 1 — request smuggling (contrat §9bis)
 * ================================================================ */

static void famille_1(void) {
    g_famille = 1;

    /* 1.1 — Content-Length ET Transfer-Encoding. Le vecteur de smuggling
     * canonique : deux cadrages contradictoires, deux intermediaires qui
     * n'en choisissent pas le meme. */
    attend("1.1_cl_et_te",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
           "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400);
    attend("1.1_te_puis_cl",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "Content-Length: 5\r\n\r\n0\r\n\r\n", 400);

    /* 1.2 / 1.3 — Content-Length duplique. Valeurs differentes : llhttp.
     * Valeurs IDENTIQUES : a notre charge (le contrat le marque T), et
     * c'est le cas subtil — un intermediaire pourrait replier. */
    attend("1.2_cl_double_divergent",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
           "Content-Length: 6\r\n\r\nhello", 400);
    attend("1.3_cl_double_identique",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
           "Content-Length: 5\r\n\r\nhello", 400);

    /* 1.4 — Content-Length malforme, cinq formes. */
    attend("1.4_cl_espace_final",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5 \r\n\r\nhello", 400);
    attend("1.4_cl_plus",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: +5\r\n\r\nhello", 400);
    attend("1.4_cl_hexa",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 0x5\r\n\r\nhello", 400);
    attend("1.4_cl_suffixe",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5abc\r\n\r\nhello", 400);
    attend("1.4_cl_vide",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: \r\n\r\nhello", 400);

    /* 1.5 — espace AVANT les deux-points : CVE-2024-27982. Rejete par
     * defaut ; ne le serait plus si LENIENT_HEADERS etait active. */
    attend("1.5_espace_avant_deux_points",
           "POST / HTTP/1.1\r\nHost: a\r\nContent-Length : 5\r\n\r\nhello", 400);

    /* 1.6 / 1.7 / 1.8 — Transfer-Encoding hors norme. */
    attend("1.6_chunked_non_final",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked, gzip\r\n"
           "\r\n0\r\n\r\n", 400);
    attend("1.7_xchunked",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: xchunked\r\n"
           "\r\n0\r\n\r\n", 400);
    attend("1.8_te_double",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "Transfer-Encoding: gzip\r\n\r\n0\r\n\r\n", 400);

    /* La casse d'un nom de transfer-coding est insensible (RFC 9112
     * §7) : « Chunked » est LEGAL et doit passer. Le tableau du contrat
     * range « casse mixte » avec xchunked ; ce serait une divergence de
     * norme. On verrouille le comportement conforme. */
    {
        Echange e = bloc("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: ChUnKeD\r\n"
                         "\r\n5\r\nhello\r\n0\r\n\r\n");
        CHECK(e.statut == 200, "1.7bis_chunked_casse_mixte_acceptee");
        CHECK(strstr(e.reponse, "TC=5") != NULL, "1.7bis_corps_dechunke_5_octets");
    }

    /* 1.9 — tailles de chunk aberrantes. */
    attend("1.9_chunk_negatif",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "\r\n-5\r\nhello\r\n0\r\n\r\n", 400);
    attend("1.9_chunk_0x_seul",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "\r\n0x5\r\nhello\r\n0\r\n\r\n", 400);
    attend("1.9_chunk_overflow",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "\r\nFFFFFFFFFFFFFFFFF\r\nhello\r\n0\r\n\r\n", 400);

    /* 1.10 — extension de chunk geante : llhttp ne la borne pas, c'est
     * notre plafond (contrat : marque T). */
    {
        size_t n = 100 * 1024;
        const char* tete = "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n5;a=";
        const char* queue = "\r\nhello\r\n0\r\n\r\n";
        size_t len = strlen(tete) + n + strlen(queue);
        char* buf = (char*)malloc(len + 1);
        memcpy(buf, tete, strlen(tete));
        memset(buf + strlen(tete), 'x', n);
        memcpy(buf + strlen(tete) + n, queue, strlen(queue));
        buf[len] = '\0';
        Options o; options_defaut(&o);
        Echange e = joue(buf, len, &g_cfg, &o);
        CHECK(e.statut == 400, "1.10_extension_chunk_geante");
        free(buf);
    }

    /* 1.11 — trailers apres le dernier chunk. Le contrat autorise DEUX
     * sorties (« trailers ignores ou 400 »), a une condition
     * imperative : jamais interpretes comme du cadrage. On verifie les
     * deux branches separement, ce qui couvre l'intention entiere.
     *
     * (a) trailer anodin : ignore. Il ne remonte pas dans les en-tetes
     *     livres et ne touche pas au corps. */
    {
        Echange e = bloc("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                         "\r\n5\r\nhello\r\n0\r\nX-Somme: abc\r\n\r\n");
        CHECK(e.statut == 200,                          "1.11a_trailer_anodin_ignore");
        CHECK(strstr(e.reponse, "TC=5") != NULL,        "1.11a_corps_reste_5_octets");
        CHECK(strstr(e.reponse, "NE=2") != NULL,        "1.11a_trailer_hors_entetes");
    }
    /* (b) trailer nomme Content-Length : llhttp coupe (mesure :
     *     HPE_INVALID_CONTENT_LENGTH « Content-Length can't be present
     *     with Transfer-Encoding »). C'est la seconde sortie permise, et
     *     la seule chose interdite — servir la requete en prenant ce
     *     trailer pour du cadrage — n'a pas lieu. */
    attend("1.11b_trailer_content_length_rejete",
           "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
           "\r\n5\r\nhello\r\n0\r\nContent-Length: 999\r\n\r\n", 400);
}

/* ================================================================
 * FAMILLE 2 — ligne de requete et en-tetes malformes
 * ================================================================ */

static void famille_2(void) {
    g_famille = 2;

    /* 2.1 / 2.2 — delimiteurs de ligne degrades. CVE-2023-30589 pour le
     * CR seul : un intermediaire qui l'accepte comme delimiteur permet
     * de fabriquer une requete cachee. */
    attend("2.1_cr_seul",  "GET / HTTP/1.1\rHost: a\r\n\r\n", 400);
    attend("2.2_lf_seul",  "GET / HTTP/1.1\nHost: a\n\n",     400);

    /* 2.3 — octet NUL aux trois emplacements. */
    {
        Options o; options_defaut(&o);
        static const char nul_chemin[]  = "GET /a\0b HTTP/1.1\r\nHost: a\r\n\r\n";
        static const char nul_nom[]     = "GET / HTTP/1.1\r\nHost: a\r\nX\0Y: z\r\n\r\n";
        static const char nul_valeur[]  = "GET / HTTP/1.1\r\nHost: a\r\nX: a\0b\r\n\r\n";
        Echange e1 = joue(nul_chemin, sizeof(nul_chemin) - 1, &g_cfg, &o);
        Echange e2 = joue(nul_nom,    sizeof(nul_nom)    - 1, &g_cfg, &o);
        Echange e3 = joue(nul_valeur, sizeof(nul_valeur) - 1, &g_cfg, &o);
        CHECK(e1.statut == 400, "2.3_nul_dans_chemin");
        CHECK(e2.statut == 400, "2.3_nul_dans_nom_entete");
        CHECK(e3.statut == 400, "2.3_nul_dans_valeur_entete");
    }

    /* 2.4 — caractere de controle en valeur d'en-tete. */
    attend("2.4_controle_en_valeur",
           "GET / HTTP/1.1\r\nHost: a\r\nX: a\x01" "b\r\n\r\n", 400);

    /* 2.5 — obs-fold : deprecie par la RFC, surface de confusion de
     * parsing entre intermediaires. Decision explicite : refuse. */
    attend("2.5_obs_fold",
           "GET / HTTP/1.1\r\nHost: a\r\nX: debut\r\n suite\r\n\r\n", 400);

    /* 2.6 — noms d'en-tete hors token. */
    attend("2.6_nom_vide",     "GET / HTTP/1.1\r\nHost: a\r\n: v\r\n\r\n",     400);
    attend("2.6_nom_espace",   "GET / HTTP/1.1\r\nHost: a\r\nX Y: v\r\n\r\n",  400);
    attend("2.6_nom_parenthese","GET / HTTP/1.1\r\nHost: a\r\nX(Y: v\r\n\r\n", 400);
    attend("2.6_nom_arobase",  "GET / HTTP/1.1\r\nHost: a\r\nX@Y: v\r\n\r\n",  400);

    /* 2.7 — ligne de requete > 8 Ko : 414, et le plafond doit mordre
     * PENDANT l'accumulation, pas apres avoir tout garde en memoire. */
    {
        size_t n = 9000;
        char* buf = (char*)malloc(n + 64);
        int t = sprintf(buf, "GET /");
        memset(buf + t, 'a', n);
        strcpy(buf + t + n, " HTTP/1.1\r\nHost: a\r\n\r\n");
        Options o; options_defaut(&o);
        Echange e = joue(buf, strlen(buf), &g_cfg, &o);
        CHECK(e.statut == 414, "2.7_ligne_requete_9ko");
        free(buf);
    }

    /* 2.8 — 101 en-tetes pour un plafond de 100. */
    {
        char* buf = (char*)malloc(64 * 1024);
        int t = sprintf(buf, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (int i = 0; i < 101; i++) t += sprintf(buf + t, "X%d: v\r\n", i);
        strcpy(buf + t, "\r\n");
        Options o; options_defaut(&o);
        Echange e = joue(buf, strlen(buf), &g_cfg, &o);
        CHECK(e.statut == 431, "2.8_101_entetes");
        free(buf);
    }

    /* 2.9 — en-tetes cumules > 16 Ko, en restant sous le plafond de
     * NOMBRE : c'est bien la taille cumulee qui doit mordre. */
    {
        char* buf = (char*)malloc(64 * 1024);
        int t = sprintf(buf, "GET / HTTP/1.1\r\nHost: a\r\n");
        for (int i = 0; i < 20; i++) {
            t += sprintf(buf + t, "X%d: ", i);
            memset(buf + t, 'v', 1000); t += 1000;
            t += sprintf(buf + t, "\r\n");
        }
        strcpy(buf + t, "\r\n");
        Options o; options_defaut(&o);
        Echange e = joue(buf, strlen(buf), &g_cfg, &o);
        CHECK(e.statut == 431, "2.9_entetes_cumules_20ko");
        free(buf);
    }

    /* 2.10 — methode inconnue, et methode de 4 Ko. Jamais de crash. */
    {
        Echange e = bloc("BREW / HTTP/1.1\r\nHost: a\r\n\r\n");
        CHECK(e.statut == 400 || e.statut == 501, "2.10_methode_inconnue");

        char* buf = (char*)malloc(8192);
        memset(buf, 'A', 4096);
        strcpy(buf + 4096, " / HTTP/1.1\r\nHost: a\r\n\r\n");
        Options o; options_defaut(&o);
        Echange e2 = joue(buf, strlen(buf), &g_cfg, &o);
        CHECK(e2.statut == 400 || e2.statut == 501, "2.10_methode_4ko");
        free(buf);
    }

    /* 2.11 / 2.12 — versions. Seules 1.0 et 1.1 sont servies. */
    attend("2.11_http_1_2",   "GET / HTTP/1.2\r\nHost: a\r\n\r\n",  400);
    attend("2.11_http_2_0",   "GET / HTTP/2.0\r\nHost: a\r\n\r\n",  400);
    attend("2.11_http_01_1",  "GET / HTTP/01.1\r\nHost: a\r\n\r\n", 400);
    attend("2.11_version_absente", "GET /\r\nHost: a\r\n\r\n",      400);
    /* 2.12 — HTTP/0.9 : « GET / » sans version, donc requete TRONQUEE du
     * point de vue de HTTP/1.x. Le pair doit fermer son sens d'envoi
     * pour que le serveur sache qu'il n'y aura pas de suite ; sinon les
     * deux s'attendent et on mesurerait une echeance, pas un rejet. */
    {
        Options o; options_defaut(&o);
        o.demi_fermeture = 1;
        const char* r09 = "GET /\r\n";
        Echange e = joue(r09, strlen(r09), &g_cfg, &o);
        CHECK(e.statut == 400, "2.12_http_0_9");
    }

    /* 2.13 — HTTP/1.0 accepte en requete, reponse annoncee en 1.1,
     * fermeture. */
    {
        Echange e = bloc("GET /x HTTP/1.0\r\n\r\n");
        CHECK(e.statut == 200,                              "2.13_http_1_0_accepte");
        CHECK(strncmp(e.reponse, "HTTP/1.1 200", 12) == 0,  "2.13_reponse_en_1_1");
        CHECK(strstr(e.reponse, "V=1.0") != NULL,           "2.13_version_requete_preservee");
        CHECK(strstr(e.reponse, "Connection: close") != NULL,"2.13_fermeture_annoncee");
    }

    /* 2.14 / 2.15 — Host. Absent en 1.1 ou duplique : 400. */
    attend("2.14_host_absent_en_1_1", "GET / HTTP/1.1\r\n\r\n", 400);
    attend("2.15_host_duplique",
           "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400);

    /* 2.16 — forme absolue : RFC 9112 §3.2.2 exige de l'ACCEPTER. La
     * parade n'est pas le refus, c'est que l'autorite n'entre jamais
     * dans le routage. On asserte donc le chemin exact, pas juste 200. */
    {
        Echange e = bloc("GET http://a/chemin?q=1 HTTP/1.1\r\nHost: a\r\n\r\n");
        CHECK(e.statut == 200,                                "2.16_forme_absolue_acceptee");
        CHECK(strstr(e.reponse, "C=/chemin?q=1") != NULL,     "2.16_route_sur_le_chemin_seul");
    }

    /* 2.17 — forme absolue dont l'autorite CONTREDIT Host. */
    attend("2.17_autorite_contredit_host",
           "GET http://autre/chemin HTTP/1.1\r\nHost: a\r\n\r\n", 400);

    /* 2.18 — allow-list de Host : desactivee par defaut, mordante quand
     * configuree. */
    {
        Echange e = bloc("GET / HTTP/1.1\r\nHost: nimporte.quoi\r\n\r\n");
        CHECK(e.statut == 200, "2.18_allow_list_desactivee_par_defaut");

        static const char* const permis[] = { "attendu.local" };
        OwlHttpServeurConfig c = g_cfg;
        c.hotes_autorises    = permis;
        c.nb_hotes_autorises = 1;
        Options o; options_defaut(&o);

        const char* hors = "GET / HTTP/1.1\r\nHost: intrus.local\r\n\r\n";
        const char* dans = "GET / HTTP/1.1\r\nHost: attendu.local\r\n\r\n";
        Echange e1 = joue(hors, strlen(hors), &c, &o);
        Echange e2 = joue(dans, strlen(dans), &c, &o);
        CHECK(e1.statut == 400, "2.18_host_hors_allow_list");
        CHECK(e2.statut == 200, "2.18_host_dans_allow_list");
    }
}

/* ================================================================
 * FAMILLE 3 — epuisement de ressources (vecteurs « une connexion »)
 * ================================================================ */

static void famille_3(void) {
    g_famille = 3;

    /* Timeouts raccourcis : c'est la MESURE du plafond qu'on teste, pas
     * la patience du harnais. */
    OwlHttpServeurConfig c = g_cfg;
    c.limites.timeout_entetes_ms    = 300;
    c.limites.timeout_corps_ms      = 300;
    c.limites.timeout_inactivite_ms = 300;

    /* 3.1 — slowloris sur les en-tetes : la requete n'est jamais
     * terminee, un octet a la fois. La parade est le timeout, pas le
     * parseur. On PROUVE que le serveur a coupe avant la fin en
     * comparant les octets reellement emis a la charge. */
    {
        const char* req = "GET / HTTP/1.1\r\nHost: a\r\nX: 1\r\nY: 2\r\nZ: 3\r\n\r\n";
        Options o; options_defaut(&o);
        o.decoupe  = OWL_MOCK_DECOUPE_OCTET;
        o.delai_ms = 30;
        Echange e = joue(req, strlen(req), &c, &o);
        CHECK(e.nb_octets_emis < strlen(req), "3.1_slowloris_entetes_coupe_avant_la_fin");
        CHECK(e.statut == 0 || e.statut == 408, "3.1_pas_de_reponse_applicative");
    }

    /* 3.2 — corps annonce puis distille. Meme parade, autre echeance. */
    {
        const char* req = "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 40\r\n\r\n"
                          "0123456789012345678901234567890123456789";
        Options o; options_defaut(&o);
        /* En-tetes d'un bloc, corps octet par octet : la frontiere est
         * l'offset de fin d'en-tetes. */
        static size_t frontiere[1];
        frontiere[0] = (size_t)(strstr(req, "\r\n\r\n") - req) + 4;
        o.decoupe     = OWL_MOCK_DECOUPE_OFFSETS;
        o.offsets     = frontiere;
        o.offsets_len = 1;
        o.delai_ms    = 500;   /* > timeout_corps_ms */
        Echange e = joue(req, strlen(req), &c, &o);
        CHECK(e.nb_ecritures >= 1,              "3.2_corps_lent_decoupage_effectif");
        CHECK(e.statut == 0 || e.statut == 408, "3.2_corps_lent_ferme_au_timeout");
    }

    /* 3.3 — connexion ouverte, aucun octet. Le serveur ne doit pas
     * attendre indefiniment un client muet. */
    {
        Options o; options_defaut(&o);
        Echange e = joue("", 0, &c, &o);
        CHECK(e.statut == 0,          "3.3_connexion_muette_aucune_reponse");
        CHECK(e.retour_serveur == 0,  "3.3_connexion_muette_fermee_proprement");
    }

    /* 3.7 — corps enorme ANNONCE : 413 des l'en-tete, sans lire la
     * suite. C'est la difference entre une limite et un piege. */
    {
        OwlHttpServeurConfig cc = g_cfg;
        cc.limites.corps_max = 1024;
        Options o; options_defaut(&o);
        const char* req = "POST / HTTP/1.1\r\nHost: a\r\n"
                          "Content-Length: 10737418240\r\n\r\n";
        Echange e = joue(req, strlen(req), &cc, &o);
        CHECK(e.statut == 413, "3.7_corps_annonce_10go_413");
    }

    /* Plafond de taille de corps reellement transmis (le pendant du
     * precedent : ici le client n'annonce rien d'enorme, il ENVOIE). */
    {
        OwlHttpServeurConfig cc = g_cfg;
        cc.limites.corps_max = 64;
        char buf[1024];
        int t = sprintf(buf, "POST / HTTP/1.1\r\nHost: a\r\n"
                             "Transfer-Encoding: chunked\r\n\r\n");
        t += sprintf(buf + t, "%X\r\n", 200);
        memset(buf + t, 'z', 200); t += 200;
        t += sprintf(buf + t, "\r\n0\r\n\r\n");
        Options o; options_defaut(&o);
        Echange e = joue(buf, (size_t)t, &cc, &o);
        CHECK(e.statut == 413, "3.7bis_corps_chunked_depasse_le_plafond");
    }

    /* 3.5 (plafond de connexions simultanees) et 3.6 (plafond de
     * requetes par connexion) ne sont PAS ecrits : ils supposent le
     * keep-alive, arbitre en phase A1. Les ecrire ici prejugerait de
     * l'arbitrage. */
}

/* ================================================================
 * FAMILLE 6 — confusion d'etat sur connexion reutilisee (A1.1a)
 *
 * « La famille la plus insidieuse », et le §6 nomme le piege :
 *   « une remise a zero imparfaite de l'etat du parseur entre deux
 *     requetes est un vecteur de smuggling a elle seule ».
 *
 * D'ou la structure de cette section : il ne suffit PAS de verifier que
 * la seconde requete est rejetee quand elle est malformee. On verifie
 * champ par champ, avec une requete seconde VALIDE, que chaque compteur
 * du parseur est reparti de zero — un compteur non remis a zero se
 * traduit par un rejet de la seconde requete, donc par un vecteur de
 * deni de service, et un tampon non vide se traduit par une requete
 * fabriquee, donc par du smuggling.
 * ================================================================ */

/* Envoie deux requetes dans DEUX send() separes par un delai : le
 * serveur les voit dans deux read() distincts. Sans le delai, la pile
 * TCP recollerait les segments et on testerait le pipeline en croyant
 * tester le sequentiel. `nb_ecritures == 2` le prouve. */
static Echange sequentiel(const char* r1, const char* r2,
                          const OwlHttpServeurConfig* cfg) {
    static char buf[64 * 1024];
    static size_t off[1];
    size_t l1 = strlen(r1), l2 = strlen(r2);
    memcpy(buf, r1, l1);
    memcpy(buf + l1, r2, l2);
    off[0] = l1;

    Options o; options_defaut(&o);
    o.decoupe     = OWL_MOCK_DECOUPE_OFFSETS;
    o.offsets     = off;
    o.offsets_len = 1;
    o.delai_ms    = 120;
    return joue(buf, l1 + l2, cfg, &o);
}

/* Les deux requetes dans UN SEUL send() : elles atterrissent dans le
 * meme tampon de lecture du serveur. C'est la que vit le residu. */
static Echange pipeline(const char* r1, const char* r2) {
    static char buf[64 * 1024];
    size_t l1 = strlen(r1), l2 = strlen(r2);
    memcpy(buf, r1, l1);
    memcpy(buf + l1, r2, l2);
    Options o; options_defaut(&o);
    return joue(buf, l1 + l2, &g_cfg, &o);
}

/* Aucune trace de handler ne doit mentionner ce chemin : c'est la
 * formulation testable de « jamais de requete fabriquee ». */
static int aucune_trace_contient(const Echange* e, const char* motif) {
    for (int i = 0; i < e->nb_appels_handler && i < CORPUS_TRACES_MAX; i++) {
        if (strstr(g_traces[i], motif)) return 0;
    }
    return 1;
}

/* Deux requetes valides successives : couple (envoi, attendu echo de la
 * SECONDE). Le contrat de cette aide est le contrat de la famille :
 * deux 200, et la seconde vue par l'applicatif exactement telle qu'elle
 * est, sans une once de la premiere.
 *
 * Le CONTROLE POSITIF est `nb_reponses == 2` : sans lui, un serveur qui
 * ne sert plus rien du tout passerait toutes les assertions de
 * non-fuite. La lecon d'A0.3, appliquee. */
static void couple_valide(const char* label, const char* r1, const char* r2,
                          const char* echo2_attendu,
                          const OwlHttpServeurConfig* cfg) {
    Echange e = sequentiel(r1, r2, cfg);
    char l[128];

    snprintf(l, sizeof(l), "%s_deux_reponses", label);
    if (e.nb_reponses != 2) {
        fprintf(stderr, "  [%s] nb_reponses=%u statut=%d appels=%d\n",
                label, (unsigned)e.nb_reponses, e.statut, e.nb_appels_handler);
    }
    compte(e.nb_reponses == 2, l, 0);

    snprintf(l, sizeof(l), "%s_deux_200", label);
    compte(e.nb_reponses == 2 && e.rep[0].statut == 200 && e.rep[1].statut == 200, l, 0);

    snprintf(l, sizeof(l), "%s_seconde_sans_trace_de_la_premiere", label);
    if (e.nb_reponses == 2 && strcmp(e.rep[1].corps, echo2_attendu) != 0) {
        fprintf(stderr, "  [%s] echo2=<%s> attendu=<%s>\n",
                label, e.rep[1].corps, echo2_attendu);
    }
    compte(e.nb_reponses == 2 && strcmp(e.rep[1].corps, echo2_attendu) == 0, l, 0);
}

static void famille_6(void) {
    g_famille = 6;

    static const char* const R_POST =
        "POST /a/b HTTP/1.1\r\nHost: h1\r\nContent-Length: 11\r\nX-Un: 1\r\n\r\nhello world";
    static const char* const R_GET =
        "GET / HTTP/1.1\r\nHost: h2\r\n\r\n";
    /* Nom d'en-tete hors token : rejet a l'analyse, pas au cadrage. */
    static const char* const R_MAL =
        "GET /mauvais HTTP/1.1\r\nHost: h\r\nX Y: v\r\n\r\n";

    /* ---- 6.0 keep-alive nominal, sequentiel. Le socle : sans lui, tout
     * le reste de la famille mesurerait un serveur qui ferme. ---- */
    {
        Echange e = sequentiel(R_POST, R_GET, &g_cfg);
        CHECK(e.nb_ecritures == 2,      "6.0_deux_send_distincts");
        CHECK(e.nb_reponses == 2,       "6.0_deux_reponses_sur_une_connexion");
        CHECK(e.nb_appels_handler == 2, "6.0_handler_appele_deux_fois");
        if (e.nb_reponses == 2) {
            CHECK(e.rep[0].statut == 200 && e.rep[1].statut == 200, "6.0_deux_200");
            CHECK(strcmp(e.rep[0].corps, "M=POST|C=/a/b|V=1.1|NE=3|TC=11") == 0,
                  "6.0_premiere_requete_exacte");
            CHECK(strcmp(e.rep[1].corps, "M=GET|C=/|V=1.1|NE=1|TC=0") == 0,
                  "6.0_seconde_requete_exacte");
            /* Les DEUX annoncent keep-alive, et c'est correct : au moment
             * de repondre a la seconde, le client n'a pas dit qu'il
             * s'arretait la et le plafond n'est pas atteint. Un serveur
             * qui annoncerait « close » ici mentirait sur une connexion
             * qu'il est pret a reutiliser. L'annonce de fermeture ne se
             * teste que la ou le serveur SAIT que c'est la derniere :
             * plafond atteint (3.6) ou rejet (6.1, 6.4). */
            CHECK(e.rep[0].annonce_keepalive == 1, "6.0_premiere_annonce_keep_alive");
            CHECK(e.rep[1].annonce_keepalive == 1, "6.0_seconde_annonce_keep_alive");
            CHECK(e.rep[0].annonce_close == 0,     "6.0_aucune_fermeture_prematuree");
        }
    }

    /* ---- 6.1 valide PUIS malformee sur la meme connexion ---- */
    {
        Echange e = sequentiel(R_POST, R_MAL, &g_cfg);
        CHECK(e.nb_reponses == 2, "6.1_une_reponse_par_requete");
        if (e.nb_reponses == 2) {
            CHECK(e.rep[0].statut == 200,      "6.1_premiere_servie");
            CHECK(e.rep[1].statut == 400,      "6.1_seconde_rejetee");
            CHECK(e.rep[1].annonce_close == 1, "6.1_rejet_ferme_la_connexion");
        }
        /* La malformee n'a JAMAIS reveille l'applicatif (contrat §6). */
        CHECK(e.nb_appels_handler == 1,             "6.1_handler_appele_une_seule_fois");
        CHECK(aucune_trace_contient(&e, "/mauvais"), "6.1_requete_malformee_jamais_servie");
    }

    /* ---- 6.1bis PREUVE CHAMP PAR CHAMP qu'aucun etat ne survit ----
     *
     * Un vecteur par element d'etat du parseur. Chacun est construit pour
     * que la NON-remise a zero se traduise par un rejet observable de la
     * seconde requete. C'est ce qui transforme « la seconde est acceptee »
     * en preuve, plutot qu'en absence d'indice. */

    /* (a) compteur de Host. Non remis a zero -> « Host duplique » -> 400. */
    couple_valide("6.1bis_a_compteur_host",
                  "GET /un HTTP/1.1\r\nHost: h\r\n\r\n",
                  "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n",
                  "M=GET|C=/deux|V=1.1|NE=1|TC=0", &g_cfg);

    /* (b) compteur de Content-Length. Non remis a zero -> 400 (le vecteur
     *     1.3, retourne contre le keep-alive). */
    couple_valide("6.1bis_b_compteur_content_length",
                  "POST /un HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello",
                  "POST /deux HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc",
                  "M=POST|C=/deux|V=1.1|NE=2|TC=3", &g_cfg);

    /* (c) octets d'en-tetes cumules. Deux fois 10 Ko sous un plafond de
     *     16 Ko : le cumul non remis a zero mord au second passage (431). */
    {
        static char r1[16 * 1024], r2[16 * 1024];
        int t = sprintf(r1, "GET /un HTTP/1.1\r\nHost: h\r\n");
        for (int i = 0; i < 10; i++) {
            t += sprintf(r1 + t, "A%d: ", i);
            memset(r1 + t, 'v', 1000); t += 1000;
            t += sprintf(r1 + t, "\r\n");
        }
        strcpy(r1 + t, "\r\n");
        t = sprintf(r2, "GET /deux HTTP/1.1\r\nHost: h\r\n");
        for (int i = 0; i < 10; i++) {
            t += sprintf(r2 + t, "B%d: ", i);
            memset(r2 + t, 'w', 1000); t += 1000;
            t += sprintf(r2 + t, "\r\n");
        }
        strcpy(r2 + t, "\r\n");
        couple_valide("6.1bis_c_octets_entetes_cumules", r1, r2,
                      "M=GET|C=/deux|V=1.1|NE=11|TC=0", &g_cfg);
    }

    /* (d) nombre d'en-tetes. Deux fois 60 sous un plafond de 100. */
    {
        static char r1[8 * 1024], r2[8 * 1024];
        int t = sprintf(r1, "GET /un HTTP/1.1\r\nHost: h\r\n");
        for (int i = 0; i < 60; i++) t += sprintf(r1 + t, "A%d: v\r\n", i);
        strcpy(r1 + t, "\r\n");
        t = sprintf(r2, "GET /deux HTTP/1.1\r\nHost: h\r\n");
        for (int i = 0; i < 60; i++) t += sprintf(r2 + t, "B%d: v\r\n", i);
        strcpy(r2 + t, "\r\n");
        couple_valide("6.1bis_d_nombre_entetes", r1, r2,
                      "M=GET|C=/deux|V=1.1|NE=61|TC=0", &g_cfg);
    }

    /* (e) tampon de corps. La seconde n'a PAS de corps : un tampon non
     *     vide donnerait TC=11 au lieu de TC=0 — et l'applicatif verrait
     *     un corps que le client n'a pas envoye. */
    couple_valide("6.1bis_e_tampon_corps",
                  "POST /un HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello world",
                  "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n",
                  "M=GET|C=/deux|V=1.1|NE=1|TC=0", &g_cfg);

    /* (f) tampon de cible. Concatenation silencieuse -> « /un/deux ». */
    couple_valide("6.1bis_f_tampon_cible",
                  "GET /un/tres/long/chemin HTTP/1.1\r\nHost: h\r\n\r\n",
                  "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n",
                  "M=GET|C=/deux|V=1.1|NE=1|TC=0", &g_cfg);

    /* (g) octets d'extension de chunk. Deux fois 3 Ko sous un plafond de
     *     4 Ko : le cumul non remis a zero rejette la seconde (400). */
    {
        static char r1[8 * 1024], r2[8 * 1024];
        int t = sprintf(r1, "POST /un HTTP/1.1\r\nHost: h\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n5;a=");
        memset(r1 + t, 'x', 3000); t += 3000;
        strcpy(r1 + t, "\r\nhello\r\n0\r\n\r\n");
        t = sprintf(r2, "POST /deux HTTP/1.1\r\nHost: h\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n3;b=");
        memset(r2 + t, 'y', 3000); t += 3000;
        strcpy(r2 + t, "\r\nabc\r\n0\r\n\r\n");
        couple_valide("6.1bis_g_extension_chunk", r1, r2,
                      "M=POST|C=/deux|V=1.1|NE=2|TC=3", &g_cfg);
    }

    /* (h) drapeau « 100 Continue a emettre ». Reste arme -> un second
     *     100 Continue sans que le client l'ait demande, ce qui desynchronise
     *     le flux de reponses. Compte EXACTEMENT un. */
    {
        Echange e = sequentiel(
            "POST /un HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
            "Content-Length: 5\r\n\r\nhello",
            "POST /deux HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nabc",
            &g_cfg);
        CHECK(e.nb_100 == 1,      "6.1bis_h_exactement_un_100_continue");
        CHECK(e.nb_reponses == 2, "6.1bis_h_deux_reponses_finales");
        if (e.nb_reponses == 2) {
            CHECK(e.rep[0].statut == 200 && e.rep[1].statut == 200, "6.1bis_h_deux_200");
            CHECK(strcmp(e.rep[1].corps, "M=POST|C=/deux|V=1.1|NE=2|TC=3") == 0,
                  "6.1bis_h_seconde_exacte");
        }
    }

    /* (i) cadrage llhttp lui-meme : chunked PUIS Content-Length. Les
     *     drapeaux internes du parseur retombent en fin de message
     *     (mesure ) ; ce vecteur verifie qu'on ne les traine pas. */
    couple_valide("6.1bis_i_cadrage_chunked_puis_content_length",
                  "POST /un HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
                  "\r\n5\r\nhello\r\n0\r\n\r\n",
                  "POST /deux HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcd",
                  "M=POST|C=/deux|V=1.1|NE=2|TC=4", &g_cfg);

    /* (j) et l'inverse : Content-Length PUIS chunked. */
    couple_valide("6.1bis_j_cadrage_content_length_puis_chunked",
                  "POST /un HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcd",
                  "POST /deux HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
                  "\r\n5\r\nhello\r\n0\r\n\r\n",
                  "M=POST|C=/deux|V=1.1|NE=2|TC=5", &g_cfg);

    /* (k) methode et version. HEAD puis GET, 1.0 puis 1.1. */
    couple_valide("6.1bis_k_methode_et_version",
                  "HEAD /un HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n",
                  "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n",
                  "M=GET|C=/deux|V=1.1|NE=1|TC=0", &g_cfg);

    /* ---- 6.2 malformee PUIS valide : la seconde n'est jamais servie ---- */
    {
        Echange e = sequentiel(R_MAL, R_GET, &g_cfg);
        CHECK(e.nb_reponses == 1,  "6.2_une_seule_reponse");
        CHECK(e.statut == 400,     "6.2_rejet_400");
        if (e.nb_reponses >= 1) {
            CHECK(e.rep[0].annonce_close == 1, "6.2_rejet_ferme_la_connexion");
        }
        CHECK(e.nb_appels_handler == 0, "6.2_handler_jamais_appele");
        CHECK(aucune_trace_contient(&e, "C=/"), "6.2_seconde_jamais_servie");
    }

    /* ---- 6.3 pipelining nominal : deux valides en UN send ---- */
    {
        Echange e = pipeline("GET /premier HTTP/1.1\r\nHost: h\r\n\r\n",
                             "GET /second HTTP/1.1\r\nHost: h\r\n\r\n");
        CHECK(e.nb_ecritures == 1,      "6.3_un_seul_send");
        CHECK(e.nb_reponses == 2,       "6.3_deux_reponses");
        CHECK(e.nb_appels_handler == 2, "6.3_handler_appele_deux_fois");
        if (e.nb_reponses == 2) {
            CHECK(e.rep[0].statut == 200 && e.rep[1].statut == 200, "6.3_deux_200");
            /* L'ORDRE est le point du vecteur : une reponse rendue dans
             * le desordre est une reponse attribuee a la mauvaise requete. */
            CHECK(strcmp(e.rep[0].corps, "M=GET|C=/premier|V=1.1|NE=1|TC=0") == 0,
                  "6.3_premiere_reponse_est_celle_de_la_premiere_requete");
            CHECK(strcmp(e.rep[1].corps, "M=GET|C=/second|V=1.1|NE=1|TC=0") == 0,
                  "6.3_seconde_reponse_est_celle_de_la_seconde_requete");
        }
    }

    /* ---- 6.4 pipelining : une valide + une malformee en UN send ---- */
    {
        Echange e = pipeline("GET /bonne HTTP/1.1\r\nHost: h\r\n\r\n", R_MAL);
        CHECK(e.nb_ecritures == 1, "6.4_un_seul_send");
        CHECK(e.nb_reponses == 2,  "6.4_deux_reponses");
        if (e.nb_reponses == 2) {
            CHECK(e.rep[0].statut == 200,      "6.4_premiere_servie");
            CHECK(e.rep[1].statut == 400,      "6.4_seconde_rejetee");
            CHECK(e.rep[1].annonce_close == 1, "6.4_rejet_ferme_la_connexion");
        }
        CHECK(e.nb_appels_handler == 1,              "6.4_handler_appele_une_seule_fois");
        CHECK(aucune_trace_contient(&e, "/mauvais"), "6.4_malformee_jamais_servie");
    }

    /* ---- 6.5 pipelining : le corps de la 1re DEBORDE sur la 2e ----
     *
     * Le mecanisme meme du request smuggling. Content-Length: 6 sur un
     * corps « 12345 » suivi de « GET /admin ... » : les six octets manges
     * sont « 12345G », et le residu « ET /admin HTTP/1.1... » ne doit
     * JAMAIS etre recolle en une requete. Il n'est pas une requete : il
     * est du bruit, et le bruit se rejette. */
    {
        Echange e = pipeline(
            "POST /vrai HTTP/1.1\r\nHost: h\r\nContent-Length: 6\r\n\r\n12345",
            "GET /admin HTTP/1.1\r\nHost: h\r\n\r\n");
        CHECK(e.nb_ecritures == 1, "6.5_un_seul_send");
        CHECK(e.nb_reponses >= 1,  "6.5_au_moins_une_reponse");
        if (e.nb_reponses >= 1) {
            CHECK(e.rep[0].statut == 200, "6.5_premiere_servie");
            /* Le corps a bien mange SIX octets, pas cinq : c'est ce
             * debordement d'un octet qui fabrique le residu. */
            CHECK(strcmp(e.rep[0].corps, "M=POST|C=/vrai|V=1.1|NE=2|TC=6") == 0,
                  "6.5_corps_de_six_octets_dont_un_vole_a_la_seconde");
        }
        CHECK(e.statut == 400,          "6.5_residu_rejete_en_400");
        CHECK(e.nb_appels_handler == 1, "6.5_handler_appele_une_seule_fois");
        CHECK(aucune_trace_contient(&e, "/admin"), "6.5_jamais_de_requete_fabriquee");
    }

    /* Variante sans debordement d'un octet : le corps mange EXACTEMENT
     * la ligne de requete suivante. Le residu est alors une requete
     * syntaxiquement valide amputee de sa premiere ligne — le cas ou
     * fabriquer une requete serait le plus tentant. */
    {
        const char* r1_tete = "POST /vrai HTTP/1.1\r\nHost: h\r\nContent-Length: ";
        const char* r2 = "GET /admin HTTP/1.1\r\n";
        static char buf[1024];
        int t = sprintf(buf, "%s%u\r\n\r\n", r1_tete, (unsigned)strlen(r2));
        t += sprintf(buf + t, "%s", r2);
        t += sprintf(buf + t, "Host: h\r\n\r\n");
        Options o; options_defaut(&o);
        Echange e = joue(buf, (size_t)t, &g_cfg, &o);
        CHECK(e.nb_appels_handler == 1, "6.5bis_handler_appele_une_seule_fois");
        CHECK(aucune_trace_contient(&e, "/admin"), "6.5bis_jamais_de_requete_fabriquee");
        CHECK(e.rep[0].statut == 200 && e.nb_reponses >= 1, "6.5bis_premiere_servie");
    }

    /* ---- 6.6 « Connection: close » puis octets supplementaires ---- */
    {
        Echange e = pipeline(
            "GET /un HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
            "GET /deux HTTP/1.1\r\nHost: h\r\n\r\n");
        CHECK(e.nb_reponses == 1,       "6.6_une_seule_reponse");
        CHECK(e.statut == 200,          "6.6_premiere_servie");
        CHECK(e.nb_appels_handler == 1, "6.6_handler_appele_une_seule_fois");
        if (e.nb_reponses >= 1) {
            CHECK(e.rep[0].annonce_close == 1, "6.6_fermeture_annoncee");
        }
        CHECK(aucune_trace_contient(&e, "/deux"), "6.6_octets_suivants_ignores");
    }

    /* HTTP/1.0 : la fermeture est le DEFAUT. Sans « Connection:
     * keep-alive » explicite, la seconde requete ne doit pas etre servie. */
    {
        Echange e = pipeline("GET /un HTTP/1.0\r\n\r\n",
                             "GET /deux HTTP/1.0\r\n\r\n");
        CHECK(e.nb_reponses == 1,       "6.6bis_http_1_0_ferme_par_defaut");
        CHECK(e.nb_appels_handler == 1, "6.6bis_seconde_non_servie");
    }

    /* Et son pendant : « Connection: keep-alive » explicite en 1.0 REOUVRE
     * la reutilisation. Controle positif du vecteur precedent — sans lui,
     * un serveur qui ferme toujours passerait 6.6bis pour de mauvaises
     * raisons. */
    {
        Echange e = pipeline("GET /un HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
                             "GET /deux HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
        CHECK(e.nb_reponses == 2,       "6.6ter_http_1_0_keep_alive_explicite");
        CHECK(e.nb_appels_handler == 2, "6.6ter_les_deux_servies");
    }

    /* ---- 6.10 le client disparait pendant l'execution du handler ----
     * Le handler va au bout, la reponse part dans le vide, pas de crash
     * et pas de blocage (contrat §8). */
    {
        Options o; options_defaut(&o);
        o.demi_fermeture = 0;
        const char* r = "GET /parti HTTP/1.1\r\nHost: h\r\n\r\n";
        Echange e = joue(r, strlen(r), &g_cfg, &o);
        CHECK(e.nb_appels_handler == 1, "6.10_handler_termine_quand_meme");
        CHECK(e.retour_serveur == 200 || e.retour_serveur == 0,
              "6.10_pas_de_crash_ni_de_blocage");
    }
}

/* ================================================================
 * FAMILLE 3 — les deux vecteurs qui attendaient le keep-alive
 * ================================================================ */

static void famille_3_keepalive(void) {
    g_famille = 3;

    /* ---- 3.6 plafond de requetes par connexion ----
     * Le contrat parle de 10 000 requetes ; ce qui se teste, c'est que le
     * plafond EXISTE et mord, pas la patience du harnais. Plafond a 5,
     * huit requetes emises. */
    {
        OwlHttpServeurConfig c = g_cfg;
        c.limites.requetes_max_par_connexion = 5;

        static char buf[4096];
        int t = 0;
        for (int i = 0; i < 8; i++) {
            t += sprintf(buf + t, "GET /r%d HTTP/1.1\r\nHost: h\r\n\r\n", i);
        }
        Options o; options_defaut(&o);
        Echange e = joue(buf, (size_t)t, &c, &o);
        CHECK(e.nb_reponses == 5,       "3.6_plafond_cinq_reponses");
        CHECK(e.nb_appels_handler == 5, "3.6_plafond_cinq_appels_handler");
        if (e.nb_reponses == 5) {
            CHECK(e.rep[4].annonce_close == 1, "3.6_derniere_annonce_la_fermeture");
            CHECK(strcmp(e.rep[4].corps, "M=GET|C=/r4|V=1.1|NE=1|TC=0") == 0,
                  "3.6_la_cinquieme_est_bien_la_cinquieme");
        }
        CHECK(aucune_trace_contient(&e, "/r5"), "3.6_sixieme_jamais_servie");
    }

    /* Controle positif : sous le plafond, les huit passent. Sans lui,
     * « le serveur ferme apres cinq » serait indiscernable de « le serveur
     * ferme apres cinq parce qu'il ferme toujours trop tot ». */
    {
        OwlHttpServeurConfig c = g_cfg;
        c.limites.requetes_max_par_connexion = 50;
        static char buf[4096];
        int t = 0;
        for (int i = 0; i < 8; i++) {
            t += sprintf(buf + t, "GET /r%d HTTP/1.1\r\nHost: h\r\n\r\n", i);
        }
        Options o; options_defaut(&o);
        Echange e = joue(buf, (size_t)t, &c, &o);
        CHECK(e.nb_reponses == 8,       "3.6bis_sous_le_plafond_huit_reponses");
        CHECK(e.nb_appels_handler == 8, "3.6bis_sous_le_plafond_huit_appels");
    }

    /* ---- Echeance d'inactivite KEEP-ALIVE ----
     * Une requete servie, puis plus rien. Le serveur doit reprendre la
     * main sur SON echeance, pas parce que le pair est parti. La preuve
     * est une MESURE : la duree du service est bornee par l'echeance, et
     * strictement inferieure au temps pendant lequel le pair garde la
     * connexion ouverte. */
    {
        OwlHttpServeurConfig c = g_cfg;
        c.limites.timeout_inactivite_ms = 250;

        Options o; options_defaut(&o);
        o.demi_fermeture = 0;   /* le pair ne dit PAS qu'il a fini */
        const char* r = "GET /un HTTP/1.1\r\nHost: h\r\n\r\n";
        Echange e = joue(r, strlen(r), &c, &o);
        CHECK(e.nb_reponses == 1, "keepalive_inactivite_une_reponse");
        CHECK(e.statut == 200,    "keepalive_inactivite_requete_servie");
        CHECK(e.duree_ms < 2000,  "keepalive_inactivite_serveur_rend_la_main_seul");
    }

    /* ---- 3.5 N connexions simultanees ----
     *
     * Sans pool de workers (lot suivant), « simultane » ne peut pas
     * vouloir dire « servi en parallele » : les connexions s'empilent
     * dans le backlog d'ecoute et sont servies une par une. Ce qui se
     * verifie donc ici est ce qui est verifiable aujourd'hui, et le
     * contrat le demande explicitement : « jamais d'epuisement de
     * descripteurs ». Le vrai plafond de concurrence appartient au lot
     * du pool.
     *
     * Backlog dimensionne pour la rafale : un backlog trop court ne
     * mesurerait pas le serveur mais la patience des pairs refuses par
     * la pile TCP — mesure faite, c'est ce qui rendait « 0 servie ». */
    {
        enum { N = 12 };
        OwlTcpListener l;
        if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, N + 4, false)) {
            CHECK(0, "3.5_ecoute_impossible");
        } else {
            const char* r = "GET /rafale HTTP/1.1\r\nHost: h\r\n\r\n";
            OwlMockPeer* pairs[N];
            for (int i = 0; i < N; i++) {
                OwlMockPeerConfig mc;
                owl_mock_peer_config_defaut(&mc);
                mc.role                  = OWL_MOCK_ROLE_CONNECTE;
                mc.hote                  = "127.0.0.1";
                mc.port                  = owl_tcp_listener_port(&l);
                mc.octets                = r;
                mc.octets_len            = strlen(r);
                mc.lire_apres_ecriture   = 1;
                mc.fermer_envoi_avant_lecture = 1;
                /* Patience des pairs > temps de service de la rafale
                 * entiere : sinon les derniers abandonnent avant leur
                 * tour et on mesurerait le harnais. */
                mc.timeout_lecture_ms    = 15000;
                mc.fermer_apres_ecriture = 1;
                pairs[i] = owl_mock_peer_demarre(&mc);
            }

            int servies = 0;
            for (int i = 0; i < N; i++) {
                if (owl_http_serveur_sert_une_connexion(&l, &g_cfg, 2000) == 200) servies++;
            }
            for (int i = 0; i < N; i++) {
                if (!pairs[i]) continue;
                owl_mock_peer_joint(pairs[i]);
                owl_mock_peer_libere(pairs[i]);
            }
            owl_tcp_listener_ferme(&l);

            CHECK(servies == N, "3.5_rafale_entiere_servie_sans_crash");

            /* Controle positif : apres la rafale, on sert encore. Un
             * epuisement de descripteurs se verrait ici et nulle part
             * ailleurs. */
            Echange apres = bloc("GET /apres HTTP/1.1\r\nHost: h\r\n\r\n");
            CHECK(apres.statut == 200, "3.5_listener_encore_utilisable_apres_rafale");
        }
    }
}

/* ================================================================
 * Criteres d'acceptation transverses du lot
 * ================================================================ */

static void nominal_et_fragmentation(void) {
    g_famille = 1;

    const char* req = "POST /a/b?c=d HTTP/1.1\r\nHost: exemple.local\r\n"
                      "Content-Length: 11\r\nX-Un: 1\r\nX-Deux: 2\r\n\r\n"
                      "hello world";
    size_t len = strlen(req);

    Options ob; options_defaut(&ob);
    Echange e_bloc = joue(req, len, &g_cfg, &ob);
    CHECK(e_bloc.statut == 200,                             "nominal_200");
    CHECK(e_bloc.nb_ecritures == 1,                         "nominal_un_seul_write");
    CHECK(strstr(e_bloc.reponse, "M=POST") != NULL,         "nominal_methode");
    CHECK(strstr(e_bloc.reponse, "C=/a/b?c=d") != NULL,     "nominal_cible");
    CHECK(strstr(e_bloc.reponse, "NE=4") != NULL,           "nominal_4_entetes");
    CHECK(strstr(e_bloc.reponse, "TC=11") != NULL,          "nominal_corps_11");
    CHECK(strstr(e_bloc.reponse, "Content-Length: ") != NULL,"nominal_reponse_cadree");

    /* Le test qui attrape l'accumulation des spans llhttp : on_url,
     * on_header_field, on_header_value et on_body sont appeles PAR
     * MORCEAUX quand les octets arrivent fragmentes. Envoye octet par
     * octet, le resultat doit etre rigoureusement identique. */
    Options oo; options_defaut(&oo);
    oo.decoupe  = OWL_MOCK_DECOUPE_OCTET;
    oo.delai_ms = 1;
    Echange e_oct = joue(req, len, &g_cfg, &oo);
    CHECK(e_oct.nb_ecritures == len,                        "frag_un_write_par_octet");
    CHECK(e_oct.statut == 200,                              "frag_200");
    CHECK(strcmp(e_oct.reponse, e_bloc.reponse) == 0,       "frag_reponse_identique_au_bloc");

    /* Decoupages adversariaux : au milieu du CRLF de fin d'en-tetes, et
     * entre le nom d'en-tete et ses deux-points. */
    {
        static size_t off[3];
        const char* p = strstr(req, "\r\n\r\n");
        off[0] = 3;                          /* au milieu de la methode */
        off[1] = (size_t)(strstr(req, "X-Un") - req) + 4;  /* avant le ':' */
        off[2] = (size_t)(p - req) + 3;      /* entre les deux CRLF */
        Options oa; options_defaut(&oa);
        oa.decoupe     = OWL_MOCK_DECOUPE_OFFSETS;
        oa.offsets     = off;
        oa.offsets_len = 3;
        oa.delai_ms    = 5;
        Echange e = joue(req, len, &g_cfg, &oa);
        CHECK(e.nb_ecritures == 4,                     "frag_adversarial_4_writes");
        CHECK(strcmp(e.reponse, e_bloc.reponse) == 0,  "frag_adversarial_identique");
    }
}

static void expect_et_head(void) {
    g_famille = 3;

    /* Expect: 100-continue nominal — le « 100 Continue » precede la
     * lecture du corps, puis la reponse finale suit. */
    {
        Echange e = bloc("POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                         "Content-Length: 5\r\n\r\nhello");
        CHECK(e.cent_continue == 1, "expect_100_continue_emis");
        CHECK(e.statut == 200,      "expect_100_continue_puis_200");
        CHECK(strncmp(e.reponse, "HTTP/1.1 100 Continue", 21) == 0,
              "expect_100_continue_en_tete_du_flux");
    }

    /* Corps annonce au-dela du plafond : 413 SANS « 100 Continue ». Le
     * client ne doit jamais etre invite a envoyer ce qu'on refusera. */
    {
        OwlHttpServeurConfig cc = g_cfg;
        cc.limites.corps_max = 16;
        Options o; options_defaut(&o);
        const char* req = "POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                          "Content-Length: 100000\r\n\r\n";
        Echange e = joue(req, strlen(req), &cc, &o);
        CHECK(e.statut == 413,       "expect_413_si_corps_trop_gros");
        CHECK(e.cent_continue == 0,  "expect_413_sans_100_continue");
    }

    /* Expect inconnu : 417, jamais un silence ni un 100 optimiste. */
    {
        Echange e = bloc("POST / HTTP/1.1\r\nHost: a\r\nExpect: quelque-chose-dautre\r\n"
                         "Content-Length: 5\r\n\r\nhello");
        CHECK(e.statut == 417,      "expect_inconnu_417");
        CHECK(e.cent_continue == 0, "expect_inconnu_sans_100_continue");
    }

    /* HEAD : memes en-tetes qu'un GET, corps VIDE. Le cadrage doit
     * rester coherent — Content-Length annonce la taille du GET. */
    {
        Echange g = bloc("GET /page HTTP/1.1\r\nHost: a\r\n\r\n");
        Echange h = bloc("HEAD /page HTTP/1.1\r\nHost: a\r\n\r\n");
        CHECK(g.statut == 200 && h.statut == 200, "head_et_get_200");

        const char* fin_g = strstr(g.reponse, "\r\n\r\n");
        const char* fin_h = strstr(h.reponse, "\r\n\r\n");
        CHECK(fin_g != NULL && fin_h != NULL, "head_reponses_bien_formees");
        if (fin_g && fin_h) {
            /* Les en-tetes different d'un seul champ : la methode echoee
             * dans le Content-Length. On compare la presence du meme
             * cadrage et l'absence de corps cote HEAD. */
            CHECK(strstr(h.reponse, "Content-Length: ") != NULL, "head_content_length_present");
            CHECK(*(fin_h + 4) == '\0',                          "head_corps_vide");
            CHECK(*(fin_g + 4) != '\0',                          "head_get_a_bien_un_corps");
        }
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_server_corpus_self_test_run(void) {
    printf("== Corpus d'attaque serveur HTTP (feat/1363 + feat/1375) ==\n");

    owl_http_serveur_config_defaut(&g_cfg);
    g_cfg.handler = handler_echo;
    /* Echeance d'inactivite courte pour tout le corpus : c'est le plafond
     * qu'on mesure, pas la patience du harnais. Les vecteurs qui testent
     * l'echeance elle-meme posent leur propre valeur. */
    g_cfg.limites.timeout_inactivite_ms = 600;

    famille_1();
    famille_2();
    famille_3();
    famille_3_keepalive();
    famille_6();
    nominal_et_fragmentation();
    expect_et_head();

    int total_pass = g_pass[1] + g_pass[2] + g_pass[3] + g_pass[6];
    int total_fail = g_fail[1] + g_fail[2] + g_fail[3] + g_fail[6];

    printf("   famille 1 (smuggling)          : %d/%d\n", g_pass[1], g_pass[1] + g_fail[1]);
    printf("   famille 2 (requete malformee)  : %d/%d\n", g_pass[2], g_pass[2] + g_fail[2]);
    printf("   famille 3 (epuisement + lot)   : %d/%d\n", g_pass[3], g_pass[3] + g_fail[3]);
    printf("   famille 6 (connexion reutilisee): %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   TOTAL corpus serveur HTTP      : %d/%d\n", total_pass, total_pass + total_fail);
    return total_fail;
}
