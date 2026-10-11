// SPDX-License-Identifier: AGPL-3.0-only
/* http_politique_test.c — familles 4 et 5 du corpus d'attaque, plus le
 * vecteur 2.18 reporte d'A0.1 (, phase A0.3).
 *
 * Spec : contrat de transport HTTP interne (corpus d'attaque).
 *
 * Deux niveaux d'assertion, volontairement :
 *
 *   1. UNITAIRE sur owl_http_politique_chemin — le code exact rendu par
 *      la frontiere de securite pour chaque vecteur. C'est ce qui dit
 *      POURQUOI un vecteur est refuse, pas seulement qu'il l'est ;
 *   2. BOUT EN BOUT a travers la vraie socket et le vrai transport, avec
 *      un fichier temoin place HORS de la racine dont le contenu est une
 *      sentinelle. Aucun octet de cette sentinelle ne doit jamais
 *      apparaitre dans une reponse. C'est la preuve, pas le raisonnement.
 *
 * Le pair emetteur est src/net_mock_peer.c en role CONNECTE, comme le
 * corpus d'A0.1.
 *
 * Execute via `owl --self-test-net` (cf. main.c). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../http_politique.h"
#include "../http_server.h"
#include "../http_exec.h"
#include "../net_mock_peer.h"
#include "../gc.h"
#include "../vm.h"
#include "fixture_unique.h"

#ifdef _WIN32
  #include <windows.h>
  /* WIN32_LEAN_AND_MEAN est pose par le build : winioctl.h ne vient pas
   * tout seul, et c'est lui qui porte FSCTL_SET_REPARSE_POINT. */
  #include <winioctl.h>
  #include <direct.h>
  #define POL_SEP '\\'
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define POL_SEP '/'
#endif

/* La sentinelle : son contenu ne doit JAMAIS traverser une reponse. */
#define SENTINELLE "TOP-SECRET-HORS-RACINE"

/* ================================================================
 * Comptage
 * ================================================================ */

static int g_pass[8];
static int g_fail[8];
static int g_famille = 4;

static void compte(int ok, const char* label, int ligne) {
    int f = (g_famille >= 0 && g_famille < 8) ? g_famille : 0;
    if (ok) g_pass[f]++;
    else { g_fail[f]++; fprintf(stderr, "  FAIL F%d [%s] (ligne %d)\n", f, label, ligne); }
}

#define CHECK(expr, label) compte((expr) ? 1 : 0, (label), __LINE__)

/* ================================================================
 * Arborescence temoin
 * ================================================================ */

static char g_racine[512];
static char g_dehors[512];
static char g_dehors_nom[128];    /* nom seul du dossier hors racine */
static int  g_lien_pret = 0;      /* 1 si la jonction / le lien existe */

static OwlHttpStatique g_stat;

static void pol_mkdir(const char* p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0700);
#endif
}

static void ecrit_fichier(const char* chemin, const char* contenu) {
    FILE* f = fopen(chemin, "wb");
    if (!f) return;
    fputs(contenu, f);
    fclose(f);
}

static void chemin_temp(char* out, size_t taille, const char* nom) {
#ifdef _WIN32
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0) snprintf(tmp, sizeof(tmp), ".\\");
    snprintf(out, taille, "%s%s", tmp, nom);
#else
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    snprintf(out, taille, "%s/%s", tmp, nom);
#endif
}

/* Lien de repertoire sortant de la racine (vecteur 4.8).
 *
 * Sous Windows on pose une JONCTION plutot qu'un lien symbolique : la
 * jonction ne demande aucun privilege particulier, la creation de lien
 * symbolique demande le mode developpeur. Mesure : GetFullPathNameA ne
 * traverse PAS la jonction (le chemin lexical reste sous la racine)
 * alors que GetFinalPathNameByHandleA la traverse et rend la cible
 * reelle. C'est exactement ce qui rend l'ORDRE des six etapes
 * non-negociable. */
#ifdef _WIN32
typedef struct {
    DWORD ReparseTag;
    WORD  ReparseDataLength;
    WORD  Reserved;
    WORD  SubstituteNameOffset;
    WORD  SubstituteNameLength;
    WORD  PrintNameOffset;
    WORD  PrintNameLength;
    WCHAR PathBuffer[1];
} PolReparse;

static int pose_lien(const char* lien, const char* cible) {
    if (!CreateDirectoryA(lien, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;

    HANDLE h = CreateFileA(lien, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    wchar_t cible_nt[600];
    swprintf(cible_nt, 600, L"\\??\\%hs", cible);

    char buf[2048];
    memset(buf, 0, sizeof(buf));
    PolReparse* r = (PolReparse*)buf;
    size_t n = wcslen(cible_nt);
    r->ReparseTag           = IO_REPARSE_TAG_MOUNT_POINT;
    r->SubstituteNameOffset = 0;
    r->SubstituteNameLength = (WORD)(n * 2);
    r->PrintNameOffset      = (WORD)(n * 2 + 2);
    r->PrintNameLength      = 0;
    memcpy(r->PathBuffer, cible_nt, n * 2 + 2);
    r->ReparseDataLength    = (WORD)(8 + n * 2 + 4);

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf,
                              (DWORD)(r->ReparseDataLength + 8), NULL, 0, &ret, NULL);
    CloseHandle(h);
    return ok ? 1 : 0;
}
#else
static int pose_lien(const char* lien, const char* cible) {
    unlink(lien);
    return symlink(cible, lien) == 0 ? 1 : 0;
}
#endif

static void prepare_arborescence(void) {
    char p[700];

    /* Noms suffixes par le PID : deux executions simultanees ecrivaient et
     * reecrivaient le meme arbre temoin, ce qui deplacait les horodatages lus
     * par les vecteurs If-Modified-Since / ETag. */
    char nom[128];
    fixture_unique(nom, sizeof(nom), "owl_pol_racine_", "");
    chemin_temp(g_racine, sizeof(g_racine), nom);
    fixture_unique(g_dehors_nom, sizeof(g_dehors_nom), "owl_pol_dehors_", "");
    chemin_temp(g_dehors, sizeof(g_dehors), g_dehors_nom);

    pol_mkdir(g_racine);
    pol_mkdir(g_dehors);

    snprintf(p, sizeof(p), "%s%cfichier.txt", g_racine, POL_SEP);
    ecrit_fichier(p, "bonjour");
    snprintf(p, sizeof(p), "%s%cindex.html", g_racine, POL_SEP);
    ecrit_fichier(p, "<h1>ok</h1>");
    snprintf(p, sizeof(p), "%s%c.env", g_racine, POL_SEP);
    ecrit_fichier(p, "CLE=valeur");

    snprintf(p, sizeof(p), "%s%c.git", g_racine, POL_SEP);
    pol_mkdir(p);
    snprintf(p, sizeof(p), "%s%c.git%cconfig", g_racine, POL_SEP, POL_SEP);
    ecrit_fichier(p, "[core]");

    snprintf(p, sizeof(p), "%s%csous", g_racine, POL_SEP);
    pol_mkdir(p);
    snprintf(p, sizeof(p), "%s%csous%cpage.txt", g_racine, POL_SEP, POL_SEP);
    ecrit_fichier(p, "sous-page");

    snprintf(p, sizeof(p), "%s%csecret.txt", g_dehors, POL_SEP);
    ecrit_fichier(p, SENTINELLE);

    snprintf(p, sizeof(p), "%s%clien", g_racine, POL_SEP);
    g_lien_pret = pose_lien(p, g_dehors);
}

/* L'arbre porte le PID depuis  : sans effacement, chaque execution
 * laisserait une paire de dossiers de plus dans le repertoire temporaire. */
static void pol_efface_dossier(const char* chemin) {
#ifdef _WIN32
    RemoveDirectoryA(chemin);
#else
    rmdir(chemin);
#endif
}

static void nettoie_arborescence(void) {
    char p[700];

    snprintf(p, sizeof(p), "%s%clien", g_racine, POL_SEP);
    /* Jonction sous Windows, lien symbolique ailleurs : dans les deux cas on
     * retire le point de montage, jamais son contenu. */
#ifdef _WIN32
    RemoveDirectoryA(p);
#else
    unlink(p);
#endif

    snprintf(p, sizeof(p), "%s%cfichier.txt", g_racine, POL_SEP);   remove(p);
    snprintf(p, sizeof(p), "%s%cindex.html", g_racine, POL_SEP);    remove(p);
    snprintf(p, sizeof(p), "%s%c.env", g_racine, POL_SEP);          remove(p);
    snprintf(p, sizeof(p), "%s%c.git%cconfig", g_racine, POL_SEP, POL_SEP); remove(p);
    snprintf(p, sizeof(p), "%s%c.git", g_racine, POL_SEP);          pol_efface_dossier(p);
    snprintf(p, sizeof(p), "%s%csous%cpage.txt", g_racine, POL_SEP, POL_SEP); remove(p);
    snprintf(p, sizeof(p), "%s%csous", g_racine, POL_SEP);          pol_efface_dossier(p);
    pol_efface_dossier(g_racine);

    snprintf(p, sizeof(p), "%s%csecret.txt", g_dehors, POL_SEP);    remove(p);
    pol_efface_dossier(g_dehors);
}

/* ================================================================
 * Echange bout en bout (meme forme que le corpus d'A0.1)
 * ================================================================ */

typedef struct {
    int    statut;
    char   reponse[OWL_MOCK_PEER_MAX_RECU + 1];
    size_t reponse_len;
} Echange;

static Echange joue(const char* octets, const OwlHttpServeurConfig* cfg) {
    Echange e;
    memset(&e, 0, sizeof(e));

    OwlTcpListener l;
    if (!owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 8, false)) return e;

    OwlMockPeerConfig mc;
    owl_mock_peer_config_defaut(&mc);
    mc.role                  = OWL_MOCK_ROLE_CONNECTE;
    mc.hote                  = "127.0.0.1";
    mc.port                  = owl_tcp_listener_port(&l);
    mc.octets                = octets;
    mc.octets_len            = strlen(octets);
    mc.decoupe               = OWL_MOCK_DECOUPE_BLOC;
    mc.lire_apres_ecriture   = 1;
    mc.timeout_lecture_ms    = 3000;
    mc.fermer_apres_ecriture = 1;

    OwlMockPeer* pair = owl_mock_peer_demarre(&mc);
    if (!pair) { owl_tcp_listener_ferme(&l); return e; }

    owl_http_serveur_sert_une_connexion(&l, cfg, 4000);
    owl_mock_peer_joint(pair);

    size_t rl = 0;
    const char* recu = owl_mock_peer_recu(pair, &rl);
    if (rl > OWL_MOCK_PEER_MAX_RECU) rl = OWL_MOCK_PEER_MAX_RECU;
    memcpy(e.reponse, recu, rl);
    e.reponse[rl] = '\0';
    e.reponse_len = rl;

    owl_mock_peer_libere(pair);
    owl_tcp_listener_ferme(&l);

    if (strncmp(e.reponse, "HTTP/1.", 7) == 0) e.statut = atoi(e.reponse + 9);
    return e;
}

static OwlHttpServeurConfig g_cfg;

/* Puits de test : accumule ce que la reponse emet. */
typedef struct {
    char   d[512];
    size_t n;
} Sac;

static int sac_ecrit(void* user, const char* octets, size_t len) {
    Sac* s = (Sac*)user;
    if (s->n + len >= sizeof(s->d)) return -1;
    memcpy(s->d + s->n, octets, len);
    s->n += len;
    s->d[s->n] = '\0';
    return 0;
}

static Echange requete(const char* cible_et_entetes) {
    char req[70000];
    snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: a\r\n\r\n", cible_et_entetes);
    return joue(req, &g_cfg);
}

static const char* corps_de(const Echange* e) {
    const char* c = strstr(e->reponse, "\r\n\r\n");
    return c ? c + 4 : "";
}

/* ================================================================
 * FAMILLE 4 — traversee de chemin
 * ================================================================ */

/* Assertion unitaire : code EXACT rendu par la frontiere. */
static void chemin(const char* cible, int attendu, const char* label) {
    char out[1024];
    out[0] = '\0';
    int r = owl_http_politique_chemin(&g_stat, cible, out, sizeof(out));
    if (r != attendu) {
        fprintf(stderr, "  [%s] cible=<%.80s> code=%d attendu=%d resolu=<%.120s>\n",
                label, cible, r, attendu, out);
    }
    compte(r == attendu, label, 0);
    /* Un refus ne doit JAMAIS laisser un chemin exploitable derriere lui. */
    if (attendu != OWL_POL_OK) compte(out[0] == '\0', label, 0);
}

static void famille_4(void) {
    g_famille = 4;

    /* Controles positifs — sans eux, « tout refuser » passerait le test. */
    {
        char out[1024];
        CHECK(owl_http_politique_chemin(&g_stat, "/fichier.txt", out, sizeof(out)) == OWL_POL_OK,
              "4.0_fichier_legitime_servi");
        CHECK(strstr(out, "fichier.txt") != NULL, "4.0_chemin_resolu_pointe_le_fichier");
        CHECK(owl_http_politique_chemin(&g_stat, "/sous/page.txt", out, sizeof(out)) == OWL_POL_OK,
              "4.0_sous_dossier_servi");
        /* Normalisation LEGITIME : un « .. » qui reste sous la racine. */
        CHECK(owl_http_politique_chemin(&g_stat, "/sous/../fichier.txt", out, sizeof(out)) == OWL_POL_OK,
              "4.0_point_point_interne_autorise");
        /* Query string : coupee avant tout decodage. */
        CHECK(owl_http_politique_chemin(&g_stat, "/fichier.txt?x=../../etc", out, sizeof(out)) == OWL_POL_OK,
              "4.0_query_ignoree");
        /* Percent-encoding legitime, decode UNE fois. */
        CHECK(owl_http_politique_chemin(&g_stat, "/%66ichier.txt", out, sizeof(out)) == OWL_POL_OK,
              "4.0_percent_encoding_legitime_decode_une_fois");
    }

    /* 4.1 — traversee en clair. */
    chemin("/../../etc/passwd",            400, "4.1_point_point_en_clair");
    chemin("/../fichier.txt",              400, "4.1_un_seul_niveau_au_dessus");
    chemin("/sous/../../fichier.txt",      400, "4.1_remontee_nette_apres_descente");

    /* 4.2 — encode une fois. */
    chemin("/%2e%2e/%2e%2e/etc/passwd",    400, "4.2_point_point_encode");
    chemin("/%2E%2E/fichier.txt",          400, "4.2_point_point_encode_majuscules");
    chemin("/..%2ffichier.txt",            400, "4.2_slash_encode_apres_point_point");

    /* 4.3 — DOUBLE encodage. Le decodage a lieu EXACTEMENT une fois :
     * « %252e » devient le texte « %2e », qui reste un nom de fichier
     * litteral et n'existe pas. Un second passage de decodage le
     * transformerait en « .. » — c'est precisement le bug. */
    chemin("/%252e%252e/",                 404, "4.3_double_encodage_pas_redecode");
    chemin("/%252e%252e/fichier.txt",      404, "4.3_double_encodage_avec_cible");
    chemin("/%25%32%65%25%32%65/",         404, "4.3_double_encodage_eclate");

    /* 4.4 — UTF-8 sur-longues. « %c0%af » est un « / » encode sur deux
     * octets, forme interdite par la norme et acceptee par des
     * decodeurs laxistes. */
    chemin("/..%c0%af..%c0%afetc/passwd",  400, "4.4_utf8_surlongue_slash");
    chemin("/%c0%ae%c0%ae/fichier.txt",    400, "4.4_utf8_surlongue_point");
    chemin("/%e0%80%af/fichier.txt",       400, "4.4_utf8_surlongue_trois_octets");
    chemin("/%ed%a0%80/fichier.txt",       400, "4.4_utf8_surrogat_interdit");
    chemin("/%ff%fe/fichier.txt",          400, "4.4_octets_utf8_impossibles");
    /* Contre-epreuve : l'UTF-8 VALIDE n'est pas refuse par la frontiere
     * (il echoue plus loin, faute de fichier). Sans elle, « tout rejeter
     * au-dessus de 0x7f » passerait pour une parade. */
    chemin("/%c3%a9te.txt",                404, "4.4_utf8_valide_non_rejete_par_l_etape_2");

    /* 4.5 — antislash Windows. */
    chemin("\\..\\..\\windows\\win.ini",   400, "4.5_antislash_en_tete");
    chemin("/..\\..\\windows\\win.ini",    400, "4.5_antislash_melange");
    chemin("/%5c..%5c..%5cwin.ini",        400, "4.5_antislash_encode");

    /* 4.6 — particularites Windows sur le nom.
     * Mesure : CreateFileA(« fichier.txt::$DATA ») OUVRE le fichier et
     * GetFinalPathNameByHandle rend le chemin PROPRE — le confinement
     * post-canonique ne voit donc rien. Meme constat pour le point et
     * l'espace finaux, retires silencieusement par la couche Win32. Ces
     * trois formes doivent tomber AVANT toute ouverture. */
    chemin("/fichier.txt::$DATA",          400, "4.6_flux_alternatif_ads");
    chemin("/fichier.txt:zone",            400, "4.6_flux_alternatif_nomme");
    chemin("/fichier.txt.",                404, "4.6_point_final");
    chemin("/fichier.txt%20",              404, "4.6_espace_final");
    chemin("/C:/windows/win.ini",          400, "4.6_lettre_de_lecteur");

    /* 4.7 — noms reserves. Mesure : GetFullPathNameA(« racine\NUL »)
     * rend « \\.\NUL » et CreateFile REUSSIT. Le peripherique doit donc
     * etre refuse lexicalement, jamais ouvert. */
    chemin("/CON",                         404, "4.7_con");
    chemin("/NUL",                         404, "4.7_nul");
    chemin("/AUX",                         404, "4.7_aux");
    chemin("/nul.txt",                     404, "4.7_nul_avec_extension");
    chemin("/CoM1",                        404, "4.7_com1_casse_mixte");
    chemin("/LPT9",                        404, "4.7_lpt9");
    chemin("/sous/PRN",                    404, "4.7_reserve_en_sous_dossier");

    /* 4.8 — lien sortant de la racine. LE vecteur qui rend l'ordre des
     * six etapes non-negociable : le chemin lexical reste sous la
     * racine, seule la resolution canonique revele la sortie. */
    if (g_lien_pret) {
        chemin("/lien/secret.txt",         404, "4.8_lien_sortant_refuse");
        chemin("/lien/",                   404, "4.8_lien_lui_meme_refuse");
        chemin("/sous/../lien/secret.txt", 404, "4.8_lien_atteint_apres_normalisation");
    } else {
        fprintf(stderr, "  (4.8 : lien de repertoire non creable ici, vecteur non joue)\n");
        CHECK(0, "4.8_lien_creable");
    }

    /* 4.9 — chemins aberrants : jamais de crash. */
    {
        size_t n = 64 * 1024;
        char* grand = (char*)malloc(n + 2);
        grand[0] = '/';
        memset(grand + 1, 'a', n);
        grand[n + 1] = '\0';
        char out[1024];
        int r = owl_http_politique_chemin(&g_stat, grand, out, sizeof(out));
        CHECK(r == 400, "4.9_chemin_64ko_refuse_sans_crash");
        free(grand);
    }
    {
        char mille[4001];
        size_t o = 0;
        for (int i = 0; i < 1000; i++) { mille[o++] = '/'; mille[o++] = 'a'; }
        mille[o] = '\0';
        /* Le contrat laisse le choix (« 400 ou 404 ») ; la valeur 404 que
         * ce test predisait etait fausse. MESURE : c'est le plafond de
         * segments qui mord le premier, donc 400 — la requete est
         * refusee comme abusive, sans jamais toucher au systeme de
         * fichiers. La forme mesuree est la bonne : refuser avant de
         * chercher coute moins cher que chercher avant de refuser. */
        chemin(mille, 400, "4.9_mille_segments");
    }
    chemin("//////",                       404, "4.9_slashs_multiples");
    chemin("/sous//////page.txt",          OWL_POL_OK, "4.9_slashs_internes_ecrases");
    chemin("fichier.txt",                  400, "4.9_cible_sans_slash_initial");
    chemin("",                             400, "4.9_cible_vide");
    chemin("/fichier%2",                   400, "4.9_percent_tronque");
    chemin("/fichier%zz.txt",              400, "4.9_percent_non_hexa");
    chemin("/fichier%00.txt",              400, "4.9_nul_encode");

    /* 4.10 — fichiers caches. Refuses PAR DEFAUT, servis si la
     * configuration le demande : c'est un reglage, donc de la politique
     * (critere du §6). La contre-epreuve le prouve. */
    chemin("/.env",                        404, "4.10_env_cache");
    chemin("/.git/config",                 404, "4.10_git_config_cache");
    chemin("/sous/../.env",                404, "4.10_cache_atteint_par_normalisation");
    {
        OwlHttpStatique ouvert = g_stat;
        ouvert.servir_caches = true;
        char out[1024];
        CHECK(owl_http_politique_chemin(&ouvert, "/.env", out, sizeof(out)) == OWL_POL_OK,
              "4.10_contre_epreuve_reglage_servir_caches");
    }
}

/* ================================================================
 * FAMILLE 4 bis — la preuve bout en bout
 *
 * Aucune deduction : on envoie les vecteurs sur une vraie socket et on
 * cherche la sentinelle dans les octets recus.
 * ================================================================ */

static void famille_4_bout_en_bout(void) {
    g_famille = 4;

    /* Les deux vecteurs qui nomment le dossier hors racine doivent porter son
     * nom REEL (suffixe par le PID), sinon ils viseraient un chemin inexistant
     * et ne prouveraient plus le refus d'un fichier qui existe vraiment. */
    char v_backslash[256];
    char v_sous[256];
    snprintf(v_backslash, sizeof(v_backslash), "\\..\\..\\%s\\secret.txt", g_dehors_nom);
    snprintf(v_sous, sizeof(v_sous), "/sous/../../%s/secret.txt", g_dehors_nom);

    const char* vecteurs[] = {
        "/../../etc/passwd",
        "/%2e%2e/%2e%2e/etc/passwd",
        "/%252e%252e/secret.txt",
        "/..%c0%af..%c0%afsecret.txt",
        v_backslash,
        "/lien/secret.txt",
        "/.env",
        "/NUL",
        "/fichier.txt::$DATA",
        v_sous
    };

    /* Etiquettes fixes : les deux vecteurs construits portent le PID, qui n'a
     * rien a faire dans un nom d'assertion (il changerait a chaque execution). */
    static const char* etiquettes[] = {
        "/../../etc/passwd",
        "/%2e%2e/%2e%2e/etc/passwd",
        "/%252e%252e/secret.txt",
        "/..%c0%af..%c0%afsecret.txt",
        "\\..\\..\\<dehors>\\secret.txt",
        "/lien/secret.txt",
        "/.env",
        "/NUL",
        "/fichier.txt::$DATA",
        "/sous/../../<dehors>/secret.txt"
    };

    for (size_t i = 0; i < sizeof(vecteurs) / sizeof(vecteurs[0]); i++) {
        Echange e = requete(vecteurs[i]);
        char label[128];
        snprintf(label, sizeof(label), "4.e2e_statut_%s", etiquettes[i]);
        if (e.statut != 400 && e.statut != 404) {
            fprintf(stderr, "  [%s] statut=%d reponse=<%.160s>\n", label, e.statut, e.reponse);
        }
        compte(e.statut == 400 || e.statut == 404, label, 0);

        snprintf(label, sizeof(label), "4.e2e_aucune_sentinelle_%s", etiquettes[i]);
        compte(strstr(e.reponse, SENTINELLE) == NULL, label, 0);
    }

    /* Controle positif bout en bout : le serveur sert VRAIMENT. */
    {
        Echange e = requete("/fichier.txt");
        CHECK(e.statut == 200, "4.e2e_controle_positif_200");
        CHECK(strcmp(corps_de(&e), "bonjour") == 0, "4.e2e_controle_positif_corps_exact");
    }
}

/* ================================================================
 * FAMILLE 5 — injection dans la reponse
 * ================================================================ */

/* Handler de test : rend ce qu'on lui demande, y compris l'absurde.
 * C'est le seul moyen d'exercer 5.4 et 5.6. */
static int  g_forcer_statut;
static const char* g_forcer_corps;

static OwlHttpRep* handler_force(const OwlHttpRequete* req, void* user) {
    (void)req; (void)user;
    return owl_http_rep_cree_tampon(g_forcer_statut, "text/plain",
                                    g_forcer_corps,
                                    g_forcer_corps ? strlen(g_forcer_corps) : 0);
}

static void famille_5(void) {
    g_famille = 5;

    OwlHttpRep* r = owl_http_rep_cree_tampon(200, "text/plain", "x", 1);
    if (!r) { CHECK(0, "5.0_reponse_creee"); return; }

    /* 5.1 — CRLF dans une valeur. Refus, jamais d'assainissement muet. */
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\r\nX-Injecte: 1") == -1,
          "5.1_crlf_refuse");
    CHECK(owl_http_rep_nb_entetes(r) == 0, "5.1_rien_ajoute_apres_refus");

    /* 5.2 — les variantes. */
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\nX-Injecte: 1") == -1, "5.2_lf_seul_refuse");
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\rX-Injecte: 1") == -1, "5.2_cr_seul_refuse");
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\x01" "b") == -1,       "5.2_controle_refuse");
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\x7f" "b") == -1,       "5.2_del_refuse");
    CHECK(owl_http_rep_ajoute_entete(r, "X-Test", "a\tb") == 0,             "5.2_tabulation_acceptee");
    CHECK(owl_http_rep_nb_entetes(r) == 1, "5.2_une_seule_entete_ajoutee");
    CHECK(strcmp(owl_http_rep_entete_valeur(r, 0), "a\tb") == 0, "5.2_valeur_conservee_telle_quelle");

    /* 5.3 — nom de champ. */
    CHECK(owl_http_rep_ajoute_entete(r, "X:Y", "1") == -1,   "5.3_deux_points_dans_le_nom");
    CHECK(owl_http_rep_ajoute_entete(r, "X Y", "1") == -1,   "5.3_espace_dans_le_nom");
    CHECK(owl_http_rep_ajoute_entete(r, "", "1") == -1,      "5.3_nom_vide");
    CHECK(owl_http_rep_ajoute_entete(r, "X\r\nY", "1") == -1,"5.3_crlf_dans_le_nom");
    CHECK(owl_http_rep_ajoute_entete(r, "X-Ok", "1") == 0,   "5.3_nom_token_accepte");
    CHECK(owl_http_rep_nb_entetes(r) == 2, "5.3_seul_le_nom_valide_est_entre");
    CHECK(strcmp(owl_http_rep_entete_nom(r, 1), "X-Ok") == 0, "5.3_nom_conserve");
    owl_http_rep_libere(r);

    /* 5.4 — statut hors plage : la reponse n'est pas construite. */
    CHECK(owl_http_politique_statut_valide(200) == true,  "5.4_200_valide");
    CHECK(owl_http_politique_statut_valide(100) == true,  "5.4_100_valide");
    CHECK(owl_http_politique_statut_valide(599) == true,  "5.4_599_valide");
    CHECK(owl_http_politique_statut_valide(99)  == false, "5.4_99_invalide");
    CHECK(owl_http_politique_statut_valide(600) == false, "5.4_600_invalide");
    CHECK(owl_http_politique_statut_valide(0)   == false, "5.4_zero_invalide");
    CHECK(owl_http_politique_statut_valide(-1)  == false, "5.4_negatif_invalide");
    CHECK(owl_http_rep_cree_tampon(99,  "text/plain", "x", 1) == NULL, "5.4_cree_refuse_99");
    CHECK(owl_http_rep_cree_tampon(600, "text/plain", "x", 1) == NULL, "5.4_cree_refuse_600");
    {
        OwlHttpRep* ok = owl_http_rep_cree_tampon(503, "text/plain", "x", 1);
        CHECK(ok != NULL, "5.4_cree_accepte_503");
        owl_http_rep_libere(ok);
    }

    /* 5.5 — HEAD : memes en-tetes qu'un GET, corps VIDE. Sur un fichier
     * statique, donc en mode B : c'est la que le cadrage peut deraper. */
    {
        char req[256];
        snprintf(req, sizeof(req), "GET /fichier.txt HTTP/1.1\r\nHost: a\r\n\r\n");
        Echange g = joue(req, &g_cfg);
        snprintf(req, sizeof(req), "HEAD /fichier.txt HTTP/1.1\r\nHost: a\r\n\r\n");
        Echange h = joue(req, &g_cfg);

        CHECK(g.statut == 200 && h.statut == 200, "5.5_head_et_get_200");
        CHECK(strstr(h.reponse, "Content-Length: 7") != NULL, "5.5_head_content_length_du_get");
        CHECK(strstr(h.reponse, "Content-Type: text/plain") != NULL, "5.5_head_content_type_du_get");
        CHECK(strcmp(corps_de(&h), "") == 0,        "5.5_head_corps_vide");
        CHECK(strcmp(corps_de(&g), "bonjour") == 0, "5.5_get_corps_present");
    }

    /* 5.6 — 204 / 304 avec un corps : corps supprime, et surtout AUCUN
     * Content-Length incoherent. */
    {
        OwlHttpServeurConfig c = g_cfg;
        c.handler_rep      = handler_force;
        c.handler_rep_user = NULL;

        g_forcer_statut = 204;
        g_forcer_corps  = "ceci-ne-doit-pas-sortir";
        Echange e = joue("GET /x HTTP/1.1\r\nHost: a\r\n\r\n", &c);
        CHECK(e.statut == 204, "5.6_204_statut");
        CHECK(strstr(e.reponse, "ceci-ne-doit-pas-sortir") == NULL, "5.6_204_corps_supprime");
        CHECK(strstr(e.reponse, "Content-Length") == NULL, "5.6_204_sans_content_length");

        g_forcer_statut = 304;
        e = joue("GET /x HTTP/1.1\r\nHost: a\r\n\r\n", &c);
        CHECK(e.statut == 304, "5.6_304_statut");
        CHECK(strstr(e.reponse, "ceci-ne-doit-pas-sortir") == NULL, "5.6_304_corps_supprime");
        CHECK(strstr(e.reponse, "Content-Length") == NULL, "5.6_304_sans_content_length");

        /* Contre-epreuve : un 200 garde bien son cadrage. */
        g_forcer_statut = 200;
        e = joue("GET /x HTTP/1.1\r\nHost: a\r\n\r\n", &c);
        CHECK(strstr(e.reponse, "Content-Length: 23") != NULL, "5.6_200_garde_son_cadrage");
    }

    /* 5.7 — erreur en mode production : aucune trace d'execution OWL.
     * Le handler leve une erreur runtime ; la reponse doit etre un 500
     * NU. Ni message, ni nom de procedure, ni numero de ligne. */
    {
        static const char* SRC =
            "PROCEDURE PUBLIQUE harnaisHttpQuiExplose()\n"
            "    t est un tableau de 2 entiers\n"
            "    x est un entier = t[99]\n"
            "FIN\n";

        OwlHttpExecConfig xc;
        owl_http_exec_config_defaut(&xc);
        xc.file_profondeur    = 2;
        xc.plafond_attente_ms = 5000;
        xc.source_owl         = SRC;
        xc.procedure          = "harnaisHttpQuiExplose";

        OwlHttpExecuteur* x = owl_http_executeur_cree(&xc);
        if (x && owl_http_executeur_demarre(x)) {
            OwlHttpServeurConfig c = g_cfg;
            c.handler_rep      = owl_http_exec_handler_rep;
            c.handler_rep_user = x;
            c.handler          = NULL;
            c.handler_user     = NULL;

            Echange e = joue("GET /explose HTTP/1.1\r\nHost: a\r\n\r\n", &c);
            CHECK(e.statut == 500, "5.7_erreur_runtime_donne_500");
            CHECK(strcmp(corps_de(&e), "") == 0, "5.7_corps_vide");
            CHECK(strstr(e.reponse, "harnaisHttpQuiExplose") == NULL, "5.7_pas_de_nom_de_procedure");
            CHECK(strstr(e.reponse, "ligne") == NULL, "5.7_pas_de_numero_de_ligne");
            CHECK(strstr(e.reponse, "Erreur") == NULL, "5.7_pas_de_message_owl");
            CHECK(strstr(e.reponse, "tableau") == NULL, "5.7_pas_de_detail_de_type");

            owl_http_executeur_arrete(x);
        } else {
            CHECK(0, "5.7_executeur_demarre");
        }
        if (x) owl_http_executeur_libere(x);
    }
}

/* ================================================================
 * FAMILLE 6 (locale) — service statique : mime, ETag, 304
 * ================================================================ */

static void famille_statique(void) {
    g_famille = 6;

    /* Types de contenu par extension. */
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.html"), "text/html; charset=utf-8") == 0,
          "mime_html");
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.txt"), "text/plain; charset=utf-8") == 0,
          "mime_txt");
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.JS"), "text/javascript; charset=utf-8") == 0,
          "mime_js_casse_mixte");
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.json"), "application/json") == 0, "mime_json");
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.png"), "image/png") == 0, "mime_png");
    CHECK(strcmp(owl_http_politique_type_mime("/a/b.inconnu"), "application/octet-stream") == 0,
          "mime_inconnu");
    CHECK(strcmp(owl_http_politique_type_mime("/a/sans_extension"), "application/octet-stream") == 0,
          "mime_sans_extension");

    /* Le service reel : en-tetes attendus sur un 200. */
    Echange e = requete("/index.html");
    CHECK(e.statut == 200, "statique_200");
    CHECK(strcmp(corps_de(&e), "<h1>ok</h1>") == 0, "statique_corps_octet_pour_octet");
    CHECK(strstr(e.reponse, "Content-Type: text/html; charset=utf-8") != NULL,
          "statique_content_type");
    CHECK(strstr(e.reponse, "Content-Length: 11") != NULL, "statique_content_length");
    CHECK(strstr(e.reponse, "ETag: \"") != NULL,           "statique_etag_present");
    CHECK(strstr(e.reponse, "Last-Modified: ") != NULL,    "statique_last_modified_present");

    /* Extraction de l'ETag et du Last-Modified reellement emis. */
    char etag[128]; etag[0] = '\0';
    {
        const char* p = strstr(e.reponse, "ETag: ");
        if (p) {
            p += 6;
            const char* fin = strstr(p, "\r\n");
            size_t n = fin ? (size_t)(fin - p) : 0;
            if (n && n < sizeof(etag)) { memcpy(etag, p, n); etag[n] = '\0'; }
        }
    }
    char lm[128]; lm[0] = '\0';
    {
        const char* p = strstr(e.reponse, "Last-Modified: ");
        if (p) {
            p += 15;
            const char* fin = strstr(p, "\r\n");
            size_t n = fin ? (size_t)(fin - p) : 0;
            if (n && n < sizeof(lm)) { memcpy(lm, p, n); lm[n] = '\0'; }
        }
    }
    CHECK(etag[0] == '"', "statique_etag_bien_forme");
    CHECK(strstr(lm, "GMT") != NULL, "statique_last_modified_imf_fixdate");

    /* 304 par If-None-Match. */
    if (etag[0]) {
        char req[512];
        snprintf(req, sizeof(req),
                 "GET /index.html HTTP/1.1\r\nHost: a\r\nIf-None-Match: %s\r\n\r\n", etag);
        Echange c = joue(req, &g_cfg);
        CHECK(c.statut == 304, "304_par_if_none_match");
        CHECK(strcmp(corps_de(&c), "") == 0, "304_sans_corps");
        CHECK(strstr(c.reponse, "Content-Length") == NULL, "304_sans_content_length");
        CHECK(strstr(c.reponse, "ETag: ") != NULL, "304_repete_l_etag");

        /* ETag qui ne correspond pas : 200 avec le corps. */
        snprintf(req, sizeof(req),
                 "GET /index.html HTTP/1.1\r\nHost: a\r\nIf-None-Match: \"autre\"\r\n\r\n");
        Echange d = joue(req, &g_cfg);
        CHECK(d.statut == 200, "etag_different_donne_200");
        CHECK(strcmp(corps_de(&d), "<h1>ok</h1>") == 0, "etag_different_corps_complet");
    }

    /* 304 par If-Modified-Since. */
    if (lm[0]) {
        char req[512];
        snprintf(req, sizeof(req),
                 "GET /index.html HTTP/1.1\r\nHost: a\r\nIf-Modified-Since: %s\r\n\r\n", lm);
        Echange c = joue(req, &g_cfg);
        CHECK(c.statut == 304, "304_par_if_modified_since");
        CHECK(strcmp(corps_de(&c), "") == 0, "304_ims_sans_corps");

        /* Date anterieure : le fichier a change depuis -> 200. */
        snprintf(req, sizeof(req),
                 "GET /index.html HTTP/1.1\r\nHost: a\r\n"
                 "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n\r\n");
        Echange d = joue(req, &g_cfg);
        CHECK(d.statut == 200, "ims_anterieure_donne_200");

        /* Date illisible : ignoree, on sert. Jamais un 304 par accident. */
        snprintf(req, sizeof(req),
                 "GET /index.html HTTP/1.1\r\nHost: a\r\nIf-Modified-Since: n-importe-quoi\r\n\r\n");
        Echange f = joue(req, &g_cfg);
        CHECK(f.statut == 200, "ims_illisible_ignoree");
    }

    /* Un repertoire n'est pas un fichier. Pas d'index automatique en A0. */
    CHECK(requete("/sous").statut == 404,  "repertoire_non_servi");
    CHECK(requete("/sous/").statut == 404, "repertoire_avec_slash_non_servi");
    CHECK(requete("/absent.txt").statut == 404, "fichier_absent_404");
}

/* ================================================================
 * FAMILLE 7 (locale) — allow-list de Host, garde-fou de bind, GC
 * ================================================================ */

static void famille_politique_diverse(void) {
    g_famille = 7;

    /* --- Vecteur 2.18 : allow-list de Host, DESACTIVEE par defaut. --- */
    CHECK(owl_http_politique_hote_permis(NULL, 0, "n-importe.quoi") == true,
          "2.18_liste_nulle_tout_permis");
    {
        static const char* liste[] = { "exemple.test", "autre.test" };
        CHECK(owl_http_politique_hote_permis(liste, 0, "exemple.test") == true,
              "2.18_liste_vide_tout_permis");
        CHECK(owl_http_politique_hote_permis(liste, 2, "exemple.test") == true,
              "2.18_hote_liste");
        CHECK(owl_http_politique_hote_permis(liste, 2, "EXEMPLE.TEST") == true,
              "2.18_comparaison_insensible_a_la_casse");
        CHECK(owl_http_politique_hote_permis(liste, 2, "exemple.test:8080") == true,
              "2.18_port_ignore");
        CHECK(owl_http_politique_hote_permis(liste, 2, "mechant.test") == false,
              "2.18_hote_hors_liste");
        CHECK(owl_http_politique_hote_permis(liste, 2, "") == false, "2.18_hote_vide");
        CHECK(owl_http_politique_hote_permis(liste, 2, NULL) == false, "2.18_hote_nul");

        /* Bout en bout : le meme reglage, applique par le transport. */
        OwlHttpServeurConfig c = g_cfg;
        c.hotes_autorises    = liste;
        c.nb_hotes_autorises = 2;
        Echange ok = joue("GET /fichier.txt HTTP/1.1\r\nHost: exemple.test\r\n\r\n", &c);
        CHECK(ok.statut == 200, "2.18_e2e_hote_liste_servi");
        Echange ko = joue("GET /fichier.txt HTTP/1.1\r\nHost: mechant.test\r\n\r\n", &c);
        CHECK(ko.statut == 400, "2.18_e2e_hote_hors_liste_400");
        CHECK(strstr(ko.reponse, "bonjour") == NULL, "2.18_e2e_aucun_contenu_servi");

        Echange defaut = joue("GET /fichier.txt HTTP/1.1\r\nHost: mechant.test\r\n\r\n", &g_cfg);
        CHECK(defaut.statut == 200, "2.18_desactivee_par_defaut");
    }

    /* --- Garde-fou de bind (livre par A0.1, verrouille ici). --- */
    {
        OwlTcpListener l;
        bool ok = owl_tcp_listener_ouvre(&l, "203.0.113.7", 0, 4, false);
        CHECK(ok == false, "bind_non_loopback_refuse_sans_drapeau");
        CHECK(strstr(owl_tcp_listener_erreur(&l), "non-loopback") != NULL,
              "bind_refus_nomme_la_raison");
        if (ok) owl_tcp_listener_ferme(&l);

        /* Avec le drapeau explicite, le garde-fou laisse passer : le bind
         * echoue alors pour une raison de PILE (adresse inexistante sur
         * cette machine), pas de politique. C'est ce que prouve l'absence
         * du mot « non-loopback » dans le message. */
        bool ok2 = owl_tcp_listener_ouvre(&l, "203.0.113.7", 0, 4, true);
        CHECK(strstr(owl_tcp_listener_erreur(&l), "non-loopback") == NULL,
              "bind_drapeau_explicite_franchit_le_garde_fou");
        if (ok2) owl_tcp_listener_ferme(&l);

        /* Loopback : jamais refuse. */
        bool ok3 = owl_tcp_listener_ouvre(&l, "127.0.0.1", 0, 4, false);
        CHECK(ok3 == true, "bind_loopback_autorise");
        if (ok3) owl_tcp_listener_ferme(&l);
    }

    /* --- Mode reponse-fichier : AUCUNE allocation GC (mesure A0.2). ---
     *
     * Le transport streame le fichier sans jamais rentrer dans la VM.
     * gc_bytesAllocated doit rester strictement inchange, a l'octet
     * pres, sur tout le service. Si quelqu'un fait passer le corps par
     * une chaine OWL, le compteur bouge et cette assertion casse. */
    {
        vm_init();
        gc_collect();

        char resolu[1024];
        CHECK(owl_http_politique_chemin(&g_stat, "/fichier.txt", resolu, sizeof(resolu)) == OWL_POL_OK,
              "gc_chemin_resolu");

        size_t avant = gc_bytesAllocated;
        OwlHttpRep* r = owl_http_rep_cree_fichier(200, "text/plain", resolu);
        CHECK(r != NULL, "gc_reponse_fichier_creee");
        if (r) {
            CHECK(owl_http_rep_est_fichier(r) == true, "gc_mode_fichier_actif");
            CHECK(owl_http_rep_longueur(r) == 7, "gc_longueur_connue_sans_lire");
        }
        size_t apres_creation = gc_bytesAllocated;
        CHECK(apres_creation == avant, "gc_creation_n_alloue_aucun_octet_gc");

        /* Ecriture reelle : le corps sort par le puits, octet pour octet. */
        if (r) {
            Sac sac;
            memset(&sac, 0, sizeof(sac));
            OwlHttpPuits puits;
            puits.ecrit = sac_ecrit;
            puits.user  = &sac;
            CHECK(owl_http_rep_ecrit(r, &puits) == 0, "gc_ecriture_vers_le_puits");
            CHECK(sac.n == 7, "gc_sept_octets_emis");
            CHECK(strcmp(sac.d, "bonjour") == 0, "gc_octets_exacts_streames");
        }

        size_t apres = gc_bytesAllocated;
        CHECK(apres == avant, "gc_service_fichier_n_alloue_aucun_octet_gc");

        if (r) owl_http_rep_libere(r);
        vm_free();
    }

    /* --- Le service statique complet, sous mesure GC. --- */
    {
        vm_init();
        gc_collect();
        size_t avant = gc_bytesAllocated;
        Echange e = requete("/index.html");
        size_t apres = gc_bytesAllocated;
        CHECK(e.statut == 200, "gc_e2e_statut");
        CHECK(strcmp(corps_de(&e), "<h1>ok</h1>") == 0, "gc_e2e_corps_exact");
        CHECK(apres == avant, "gc_e2e_transport_n_alloue_aucun_octet_gc");
        vm_free();
    }
}

/* ================================================================
 * Point d'entree
 * ================================================================ */

int http_politique_self_test_run(void) {
    printf("== Politique HTTP et service statique (feat/1369, phase A0.3) ==\n");

    prepare_arborescence();

    if (!owl_http_politique_racine(&g_stat, g_racine)) {
        fprintf(stderr, "  (racine du statique non resolue : %s)\n", g_racine);
        g_famille = 4;
        CHECK(0, "racine_resolue");
    }

    owl_http_serveur_config_defaut(&g_cfg);
    g_cfg.handler_rep      = owl_http_statique_handler;
    g_cfg.handler_rep_user = &g_stat;

    famille_4();
    famille_4_bout_en_bout();
    famille_5();
    famille_statique();
    famille_politique_diverse();

    int tp = 0, tf = 0;
    for (int i = 0; i < 8; i++) { tp += g_pass[i]; tf += g_fail[i]; }

    printf("   f4 traversee de chemin      : %d/%d\n", g_pass[4], g_pass[4] + g_fail[4]);
    printf("   f5 injection en reponse     : %d/%d\n", g_pass[5], g_pass[5] + g_fail[5]);
    printf("   f6 statique (mime/ETag/304) : %d/%d\n", g_pass[6], g_pass[6] + g_fail[6]);
    printf("   f7 hote, bind, anti-GC      : %d/%d\n", g_pass[7], g_pass[7] + g_fail[7]);
    printf("   TOTAL politique HTTP        : %d/%d\n", tp, tp + tf);

    nettoie_arborescence();
    return tf;
}
