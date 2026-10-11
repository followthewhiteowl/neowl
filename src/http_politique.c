// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* http_politique.c — politique HTTP (, phase A0.3).
 *
 * Contrat : src/http_politique.h et le contrat de transport HTTP interne
 *           (repartition des responsabilites).
 *
 * ================================================================
 * L'ORDRE DES SIX ETAPES N'EST PAS DECORATIF
 * ================================================================
 *
 * Le piege classique est de verifier le confinement AVANT la resolution
 * canonique. MESURE faite avant d'ecrire ce fichier, avec une jonction
 * de repertoire pointant hors de la racine :
 *
 *   GetFullPathNameA("racine\lien\secret.txt")
 *       -> "racine\lien\secret.txt"        (ne traverse PAS la jonction)
 *   GetFinalPathNameByHandleA(meme chemin)
 *       -> "\\?\C:\...\dehors\secret.txt"  (traverse, et sort de la racine)
 *
 * Un confinement pose sur le premier resultat conclut « sous la racine »
 * et sert le fichier. Le meme confinement pose sur le second refuse.
 * D'ou l'ordre : resoudre (4), PUIS confiner (5).
 *
 * Deux particularites Windows ne peuvent PAS attendre l'etape 6, et
 * c'est une mesure qui le dit, pas une intuition :
 *
 *   GetFullPathNameA("racine\NUL") -> "\\.\NUL", et CreateFile REUSSIT.
 *       Un controle apres l'etape 4 arriverait apres l'ouverture du
 *       peripherique.
 *   CreateFileA("racine\fichier.txt::$DATA") REUSSIT, et le chemin final
 *       rendu par handle est PROPRE, sous la racine. Le confinement ne
 *       voit donc rien du flux alternatif.
 *
 * Ces deux familles-la sont donc traitees au niveau du NOM, a l'etape 3,
 * avant toute ouverture. L'etape 6 garde ce qui ne peut se decider
 * qu'apres resolution (nature reelle de l'objet vise).
 *
 * Ce module ne connait pas la VM : pas d'allocation d'objet OWL, pas de
 * GC declenchable, aucune native. */

#include "http_politique.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
  #include <windows.h>
  #define POL_SEP     '\\'
  #define POL_SEP_TXT "\\"
#else
  #include <limits.h>
  #include <unistd.h>
  #define POL_SEP     '/'
  #define POL_SEP_TXT "/"
#endif

#define POL_SEGMENTS_MAX 64
#define POL_SEGMENT_MAX  255

/* ================================================================
 * Petits utilitaires ASCII (les tokens HTTP et les noms de fichiers
 * qu'on accepte le sont par construction)
 * ================================================================ */

static int pol_min(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int pol_egal_i(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (pol_min((unsigned char)*a) != pol_min((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int pol_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ================================================================
 * ETAPE 1 — decodage du percent-encoding, EXACTEMENT une fois
 *
 * Le point entier du vecteur 4.3 : « %252e » doit devenir le TEXTE
 * « %2e », pas « . ». Le « % » produit reste donc un caractere
 * ordinaire du nom, et rien ne le relit. Une seconde passe — meme
 * involontaire, meme deleguee a une bibliotheque — rouvre la traversee.
 * ================================================================ */

static int etape1_decode_une_fois(const char* src, size_t src_len,
                                  char* out, size_t out_taille,
                                  size_t* out_len) {
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        if (o + 1 >= out_taille) return 400;

        if (src[i] != '%') { out[o++] = src[i]; continue; }

        if (i + 2 >= src_len) return 400;             /* « %2 » tronque */
        int h = pol_hex((unsigned char)src[i + 1]);
        int l = pol_hex((unsigned char)src[i + 2]);
        if (h < 0 || l < 0) return 400;               /* « %zz » */

        out[o++] = (char)((h << 4) | l);
        i += 2;
    }
    out[o] = '\0';
    /* La LONGUEUR est rendue a part : un « %00 » decode pose un octet nul
     * au milieu du tampon, et se fier a strlen reviendrait a tronquer en
     * silence — c'est-a-dire a valider un chemin puis en servir un autre. */
    *out_len = o;
    return OWL_POL_OK;
}

/* ================================================================
 * ETAPE 2 — rejet du NUL, des separateurs interdits et des octets
 *           impossibles
 *
 * Le NUL est rejete par la LONGUEUR, pas par le terminateur : un
 * « %00 » decode produit un octet nul au milieu du tampon, et se fier a
 * strlen reviendrait a tronquer silencieusement — c'est-a-dire a servir
 * un autre fichier que celui qu'on a valide.
 *
 * L'UTF-8 est valide strictement : les formes sur-longues (« %c0%af »
 * pour « / »), les demi-substituts et les points de code hors plage
 * sont des encodages ILLEGAUX d'un caractere legal. Les accepter, c'est
 * offrir un second alphabet pour ecrire « ../ ».
 * ================================================================ */

static int etape2_octets_interdits(const char* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)p[i];

        if (c == 0)    return 400;   /* NUL, y compris issu de %00 */
        if (c < 0x20)  return 400;   /* controles : CR, LF, TAB... */
        if (c == 0x7f) return 400;
        if (c == '\\') return 400;   /* separateur Windows */
        if (c == ':')  return 400;   /* flux alternatif, lettre de lecteur */

        if (c < 0x80) continue;

        /* UTF-8 strict. */
        int suite;
        unsigned int cp;
        if      ((c & 0xE0) == 0xC0) { suite = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { suite = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { suite = 3; cp = c & 0x07u; }
        else return 400;             /* 0x80-0xBF isole, 0xF8+, 0xFE, 0xFF */

        if (i + (size_t)suite >= len) return 400;
        for (int k = 0; k < suite; k++) {
            unsigned char s = (unsigned char)p[i + 1 + (size_t)k];
            if ((s & 0xC0) != 0x80) return 400;
            cp = (cp << 6) | (unsigned int)(s & 0x3Fu);
        }
        i += (size_t)suite;

        if (suite == 1 && cp < 0x80)    return 400;   /* sur-longue 2 octets */
        if (suite == 2 && cp < 0x800)   return 400;   /* sur-longue 3 octets */
        if (suite == 3 && cp < 0x10000) return 400;   /* sur-longue 4 octets */
        if (cp >= 0xD800 && cp <= 0xDFFF) return 400; /* demi-substitut */
        if (cp > 0x10FFFF)                return 400;
    }
    return OWL_POL_OK;
}

/* ================================================================
 * ETAPE 3 — normalisation des segments, et controles de NOM
 *
 * « . » disparait, « .. » depile, et une pile vide qui recoit « .. »
 * est une tentative de sortie : 400, pas un ecretage silencieux. Un
 * ecretage laisserait croire au reste du code que la requete etait
 * legitime.
 * ================================================================ */

static const char* const POL_RESERVES[] = {
    "con", "prn", "aux", "nul",
    "com0", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
    "lpt0", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    "conin$", "conout$"
};

/* Un nom reserve l'est aussi avec une extension : « nul.txt » vise le
 * meme peripherique. On compare donc la partie avant le premier point. */
static int pol_nom_reserve(const char* seg) {
    char base[POL_SEGMENT_MAX + 1];
    size_t n = 0;
    while (seg[n] && seg[n] != '.' && n < POL_SEGMENT_MAX) { base[n] = seg[n]; n++; }
    base[n] = '\0';
    if (n == 0) return 0;

    for (size_t i = 0; i < sizeof(POL_RESERVES) / sizeof(POL_RESERVES[0]); i++) {
        if (pol_egal_i(base, POL_RESERVES[i])) return 1;
    }
    return 0;
}

typedef struct {
    char   seg[POL_SEGMENTS_MAX][POL_SEGMENT_MAX + 1];
    size_t nb;
} PolSegments;

static int etape3_normalise(const OwlHttpStatique* s, const char* chemin,
                            PolSegments* out) {
    out->nb = 0;

    const char* p = chemin;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char* fin = p;
        while (*fin && *fin != '/') fin++;
        size_t n = (size_t)(fin - p);

        if (n > POL_SEGMENT_MAX) return 400;

        char seg[POL_SEGMENT_MAX + 1];
        memcpy(seg, p, n);
        seg[n] = '\0';
        p = fin;

        if (strcmp(seg, ".") == 0) continue;

        if (strcmp(seg, "..") == 0) {
            /* Sortie par le haut : refus net, jamais un ecretage. */
            if (out->nb == 0) return 400;
            out->nb--;
            continue;
        }

        /* --- Controles de NOM, obligatoirement avant toute ouverture ---
         * (cf. l'en-tete du fichier : mesure sur NUL et sur ::$DATA). */
        if (seg[0] == '.' && !s->servir_caches) return 404;
        if (seg[n - 1] == '.' || seg[n - 1] == ' ') return 404;
        if (pol_nom_reserve(seg)) return 404;

        if (out->nb >= POL_SEGMENTS_MAX) return 400;
        memcpy(out->seg[out->nb], seg, n + 1);
        out->nb++;
    }
    return OWL_POL_OK;
}

/* ================================================================
 * ETAPE 4 — resolution canonique
 *
 * Windows : ouverture d'un handle puis GetFinalPathNameByHandleW. C'est
 * la SEULE forme qui traverse jonctions et liens symboliques ; la
 * mesure consignee en tete du fichier montre que GetFullPathName ne les
 * traverse pas. POSIX : realpath, qui les traverse aussi.
 *
 * La racine passe par la meme fonction : les deux formes doivent etre
 * comparables octet a octet (la forme finale developpe les noms courts
 * 8.3 et corrige la casse — comparer une racine non canonicalisee a un
 * candidat canonique echouerait sur un chemin contenant un nom court « EXAMP~1 »).
 * ================================================================ */

static int etape4_canonique(const char* brut, char* out, size_t out_taille) {
#ifdef _WIN32
    HANDLE h = CreateFileA(brut, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return 404;

    DWORD n = GetFinalPathNameByHandleA(h, out, (DWORD)out_taille,
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= out_taille) { out[0] = '\0'; return 404; }
    return OWL_POL_OK;
#else
    char* r = realpath(brut, NULL);
    if (!r) return 404;
    size_t n = strlen(r);
    if (n >= out_taille) { free(r); out[0] = '\0'; return 404; }
    memcpy(out, r, n + 1);
    free(r);
    return OWL_POL_OK;
#endif
}

/* ================================================================
 * ETAPE 5 — confinement
 *
 * La comparaison porte sur les formes CANONIQUES des deux cotes, et
 * exige une frontiere de separateur : sans elle, « /var/www-prive »
 * passerait pour etant sous « /var/www ».
 * ================================================================ */

static int etape5_confine(const char* racine, const char* candidat) {
    size_t rn = strlen(racine);
    if (rn == 0) return 404;

    /* Un separateur final sur la racine fausserait la longueur. */
    while (rn > 1 && (racine[rn - 1] == '/' || racine[rn - 1] == '\\')) rn--;

    for (size_t i = 0; i < rn; i++) {
#ifdef _WIN32
        /* NTFS est insensible a la casse : comparer sensiblement
         * refuserait des chemins legitimes. */
        if (pol_min((unsigned char)racine[i]) != pol_min((unsigned char)candidat[i])) return 404;
#else
        if (racine[i] != candidat[i]) return 404;
#endif
        if (candidat[i] == '\0') return 404;
    }

    char suite = candidat[rn];
    if (suite == '\0') return 404;                 /* la racine elle-meme */
    if (suite != '/' && suite != '\\') return 404; /* « www-prive » vs « www » */
    return OWL_POL_OK;
}

/* ================================================================
 * ETAPE 6 — ce qui ne peut se decider qu'apres resolution
 *
 * Les particularites Windows au niveau du NOM (antislash, noms
 * reserves, flux alternatifs, point/espace finaux) sont traitees aux
 * etapes 2 et 3 : les mesures en tete de fichier montrent qu'attendre
 * ici reviendrait a decider APRES l'ouverture. Reste ce qui depend de
 * l'objet reel : ce doit etre un fichier ordinaire.
 * ================================================================ */

static int etape6_fichier_ordinaire(const char* canonique) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(canonique);
    if (a == INVALID_FILE_ATTRIBUTES) return 404;
    if (a & FILE_ATTRIBUTE_DIRECTORY) return 404;
    if (a & FILE_ATTRIBUTE_DEVICE)    return 404;
    return OWL_POL_OK;
#else
    struct stat st;
    if (stat(canonique, &st) != 0) return 404;
    if (!S_ISREG(st.st_mode)) return 404;
    return OWL_POL_OK;
#endif
}

/* ================================================================
 * La frontiere, assemblee
 * ================================================================ */

bool owl_http_politique_racine(OwlHttpStatique* s, const char* dossier) {
    if (!s) return false;
    memset(s, 0, sizeof(*s));
    if (!dossier || !dossier[0]) return false;
    return etape4_canonique(dossier, s->racine, sizeof(s->racine)) == OWL_POL_OK;
}

int owl_http_politique_chemin(const OwlHttpStatique* s, const char* cible,
                              char* out, size_t out_taille) {
    if (out && out_taille) out[0] = '\0';
    if (!s || !s->racine[0] || !cible || !out || out_taille == 0) return 400;

    /* Prealable : borne de longueur PROPRE a la politique. Le transport
     * a la sienne ; celle-ci est ce qui rend la politique identique quel
     * que soit le transport (contrat §9bis). */
    size_t len = strlen(cible);
    if (len == 0 || len > OWL_POL_CIBLE_MAX) return 400;
    if (cible[0] != '/') return 400;

    /* Query et fragment : coupes AVANT le decodage, sinon un « %3f »
     * decode fabriquerait une frontiere de query qui n'existait pas. */
    const char* q = cible;
    while (*q && *q != '?' && *q != '#') q++;
    len = (size_t)(q - cible);

    char   decode[OWL_POL_CIBLE_MAX + 1];
    size_t dlen = 0;
    int r = etape1_decode_une_fois(cible, len, decode, sizeof(decode), &dlen);
    if (r != OWL_POL_OK) return r;

    r = etape2_octets_interdits(decode, dlen);
    if (r != OWL_POL_OK) return r;

    PolSegments seg;
    r = etape3_normalise(s, decode, &seg);
    if (r != OWL_POL_OK) return r;
    if (seg.nb == 0) return 404;   /* « / » : pas d'index automatique en A0 */

    char brut[1400];
    size_t o = 0;
    int e = snprintf(brut, sizeof(brut), "%s", s->racine);
    if (e < 0 || (size_t)e >= sizeof(brut)) return 400;
    o = (size_t)e;
    for (size_t i = 0; i < seg.nb; i++) {
        e = snprintf(brut + o, sizeof(brut) - o, "%c%s", POL_SEP, seg.seg[i]);
        if (e < 0 || (size_t)e >= sizeof(brut) - o) return 400;
        o += (size_t)e;
    }

    char canonique[1400];
    r = etape4_canonique(brut, canonique, sizeof(canonique));
    if (r != OWL_POL_OK) return r;

    r = etape5_confine(s->racine, canonique);
    if (r != OWL_POL_OK) return r;

    r = etape6_fichier_ordinaire(canonique);
    if (r != OWL_POL_OK) return r;

    if (strlen(canonique) >= out_taille) return 400;
    memcpy(out, canonique, strlen(canonique) + 1);
    return OWL_POL_OK;
}

/* ================================================================
 * Ecriture des en-tetes : le point de passage unique
 * ================================================================ */

bool owl_http_politique_nom_entete_valide(const char* nom) {
    if (!nom || !nom[0]) return false;
    for (const char* p = nom; *p; p++) {
        unsigned char c = (unsigned char)*p;
        /* token, RFC 9110 §5.6.2 : ni separateur, ni controle, ni 8 bits. */
        if (c <= 0x20 || c >= 0x7f) return false;
        if (strchr("\"(),/:;<=>?@[\\]{}", (int)c)) return false;
    }
    return true;
}

bool owl_http_politique_valeur_entete_valide(const char* valeur) {
    if (!valeur) return false;
    for (const char* p = valeur; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\r' || c == '\n') return false;   /* scission de reponse */
        if (c == 0x7f) return false;
        if (c < 0x20 && c != '\t') return false;    /* HTAB reste legal */
    }
    return true;
}

bool owl_http_politique_statut_valide(int statut) {
    return statut >= 100 && statut <= 599;
}

/* ================================================================
 * Allow-list de Host (vecteur 2.18)
 * ================================================================ */

bool owl_http_politique_hote_permis(const char* const* liste, size_t nb,
                                    const char* hote) {
    if (!liste || nb == 0) return true;   /* desactivee : le defaut */
    if (!hote || !hote[0]) return false;

    /* Le port ne fait pas partie de l'identite comparee : une liste
     * s'ecrit en noms d'hote, pas en couples hote:port. */
    char sans_port[256];
    size_t n = 0;
    while (hote[n] && hote[n] != ':' && n < sizeof(sans_port) - 1) {
        sans_port[n] = hote[n];
        n++;
    }
    sans_port[n] = '\0';
    if (n == 0) return false;

    for (size_t i = 0; i < nb; i++) {
        if (pol_egal_i(sans_port, liste[i])) return true;
    }
    return false;
}

/* ================================================================
 * Service de fichiers statiques
 * ================================================================ */

typedef struct { const char* ext; const char* type; } PolMime;

static const PolMime POL_MIMES[] = {
    { "html", "text/html; charset=utf-8" },
    { "htm",  "text/html; charset=utf-8" },
    { "css",  "text/css; charset=utf-8" },
    { "js",   "text/javascript; charset=utf-8" },
    { "mjs",  "text/javascript; charset=utf-8" },
    { "txt",  "text/plain; charset=utf-8" },
    { "md",   "text/markdown; charset=utf-8" },
    { "csv",  "text/csv; charset=utf-8" },
    { "json", "application/json" },
    { "xml",  "application/xml" },
    { "pdf",  "application/pdf" },
    { "wasm", "application/wasm" },
    { "zip",  "application/zip" },
    { "png",  "image/png" },
    { "jpg",  "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "gif",  "image/gif" },
    { "webp", "image/webp" },
    { "svg",  "image/svg+xml" },
    { "ico",  "image/x-icon" },
    { "woff", "font/woff" },
    { "woff2","font/woff2" }
};

const char* owl_http_politique_type_mime(const char* chemin) {
    if (!chemin) return "application/octet-stream";

    const char* point = NULL;
    for (const char* p = chemin; *p; p++) {
        if (*p == '.') point = p;
        else if (*p == '/' || *p == '\\') point = NULL;
    }
    if (!point || !point[1]) return "application/octet-stream";

    for (size_t i = 0; i < sizeof(POL_MIMES) / sizeof(POL_MIMES[0]); i++) {
        if (pol_egal_i(point + 1, POL_MIMES[i].ext)) return POL_MIMES[i].type;
    }
    return "application/octet-stream";
}

/* IMF-fixdate (RFC 9110 §5.6.7). En anglais et en GMT par obligation
 * protocolaire : ce n'est pas une chaine destinee a un lecteur humain,
 * c'est un format de champ. D'ou les tables en dur plutot que strftime,
 * qui dependrait de la locale du processus. */
static const char* const POL_JOURS[]  = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
static const char* const POL_MOIS[]   = { "Jan","Feb","Mar","Apr","May","Jun",
                                          "Jul","Aug","Sep","Oct","Nov","Dec" };

static void pol_date_http(time_t t, char* out, size_t taille) {
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    snprintf(out, taille, "%s, %02d %s %04d %02d:%02d:%02d GMT",
             POL_JOURS[g.tm_wday % 7], g.tm_mday, POL_MOIS[g.tm_mon % 12],
             g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
}

/* Analyse d'un IMF-fixdate. -1 si illisible : l'appelant SERT alors le
 * fichier. Une date qu'on ne comprend pas ne doit jamais produire un 304
 * — ce serait servir un cache perime sur une erreur de syntaxe. */
static time_t pol_lit_date_http(const char* v) {
    if (!v) return (time_t)-1;
    while (*v == ' ') v++;
    /* « Ddd, DD Mmm YYYY HH:MM:SS GMT » */
    if (strlen(v) < 29) return (time_t)-1;
    if (v[3] != ',' || v[4] != ' ') return (time_t)-1;

    char mois[4];
    struct tm t;
    memset(&t, 0, sizeof(t));
    int jour, annee, h, m, s;
    if (sscanf(v + 5, "%2d %3s %4d %2d:%2d:%2d", &jour, mois, &annee, &h, &m, &s) != 6) {
        return (time_t)-1;
    }
    int im = -1;
    for (int i = 0; i < 12; i++) if (pol_egal_i(mois, POL_MOIS[i])) { im = i; break; }
    if (im < 0) return (time_t)-1;

    t.tm_mday = jour;
    t.tm_mon  = im;
    t.tm_year = annee - 1900;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;

    /* Conversion UTC sans timegm (absent de MSVCRT) : jours depuis
     * l'epoque, calcul civil direct. */
    long a = annee;
    long mo = im + 1;
    long y = a - (mo <= 2 ? 1 : 0);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + jour - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long jours = era * 146097 + doe - 719468;

    return (time_t)(jours * 86400L + h * 3600L + m * 60L + s);
}

static const char* pol_entete(const OwlHttpRequete* req, const char* nom) {
    for (size_t i = 0; i < req->nb_entetes; i++) {
        if (pol_egal_i(req->entetes[i].nom, nom)) return req->entetes[i].valeur;
    }
    return NULL;
}

OwlHttpRep* owl_http_statique_sert(const OwlHttpStatique* s,
                                   const OwlHttpRequete* req) {
    if (!s || !req) return owl_http_rep_cree_tampon(500, NULL, NULL, 0);

    /* Seules les methodes de lecture servent un fichier. */
    if (!pol_egal_i(req->methode, "GET") && !pol_egal_i(req->methode, "HEAD")) {
        return owl_http_rep_cree_tampon(405, NULL, NULL, 0);
    }

    char resolu[1400];
    int r = owl_http_politique_chemin(s, req->chemin ? req->chemin : "", resolu, sizeof(resolu));
    if (r != OWL_POL_OK) return owl_http_rep_cree_tampon(r, NULL, NULL, 0);

    struct stat st;
    if (stat(resolu, &st) != 0) return owl_http_rep_cree_tampon(404, NULL, NULL, 0);

    /* Validateurs. ETag fort : taille et date suffisent a distinguer
     * deux versions d'un fichier servi depuis le disque. */
    char etag[80];
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
             (unsigned long long)st.st_size, (unsigned long long)st.st_mtime);

    char date[64];
    pol_date_http((time_t)st.st_mtime, date, sizeof(date));

    /* If-None-Match prime sur If-Modified-Since (RFC 9110 §13.1.3) : le
     * validateur fort l'emporte sur le validateur faible. */
    int non_modifie = 0;
    const char* inm = pol_entete(req, "if-none-match");
    if (inm) {
        non_modifie = (strcmp(inm, etag) == 0);
    } else {
        const char* ims = pol_entete(req, "if-modified-since");
        if (ims) {
            time_t borne = pol_lit_date_http(ims);
            if (borne != (time_t)-1 && (time_t)st.st_mtime <= borne) non_modifie = 1;
        }
    }

    const char* type = owl_http_politique_type_mime(resolu);

    OwlHttpRep* rep = non_modifie
        ? owl_http_rep_cree_tampon(304, NULL, NULL, 0)
        : owl_http_rep_cree_fichier(200, type, resolu);

    /* Le fichier a disparu entre stat et l'ouverture : 404 plutot qu'un
     * 200 sans corps. */
    if (!rep) return owl_http_rep_cree_tampon(404, NULL, NULL, 0);

    /* Passage par le point unique : un ETag mal forme serait refuse ici
     * plutot que concatene dans la reponse. */
    owl_http_rep_ajoute_entete(rep, "ETag", etag);
    owl_http_rep_ajoute_entete(rep, "Last-Modified", date);
    /* Pas de Range en A0 : on le dit, plutot que de laisser un client
     * en demander un et recevoir 200 sans comprendre. */
    owl_http_rep_ajoute_entete(rep, "Accept-Ranges", "none");

    return rep;
}

OwlHttpRep* owl_http_statique_handler(const OwlHttpRequete* req, void* user) {
    return owl_http_statique_sert((const OwlHttpStatique*)user, req);
}
