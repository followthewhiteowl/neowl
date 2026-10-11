// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* http_server.c — transport HTTP/1.1 serveur (, phase A0.1).
 *
 * Spec : contrat de transport HTTP interne.
 *
 * Parti pris central, et il vient du contrat §6 : les limites sont
 * appliquees PENDANT l'accumulation, jamais apres. Un plafond verifie
 * apres coup a deja fait allouer ce qu'il pretend interdire — c'est un
 * compteur, pas une limite. D'ou des controles dans les callbacks
 * llhttp eux-memes, qui rendent la main des le depassement.
 *
 * Second parti pris : aucun flag LENIENT_*. llhttp est strict par
 * defaut et chacun de ses onze flags rouvre une classe de smuggling ;
 * c'est la raison meme du choix de cette bibliotheque.
 *
 * Ce module ne connait pas la VM : pas d'allocation d'objet OWL, pas de
 * GC declenchable, aucune native. L'executeur de la phase A0.2 se
 * branchera sur OwlHttpHandler, pas a la place. */

#include "http_server.h"
/* Le type OPAQUE de reponse (, contrat §5). Le transport n'en
 * voit que les accesseurs : il ne suppose pas un tampon contigu et ne
 * connait toujours pas la VM. */
#include "http_exec.h"
/* La politique (allow-list de Host) : elle depend de la configuration,
 * donc elle vit au-dessus du transport et en UN SEUL endroit (§6). */
#include "http_politique.h"
/* Masques d'evenements OWL_REAC_* remontes au drain de flux reacteur (W5). */
#include "net_reacteur.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llhttp.h"

/* ================================================================
 * Tampon accumulateur
 *
 * Les callbacks de span de llhttp (on_url, on_header_field,
 * on_header_value, on_body) sont appeles PAR MORCEAUX des que les
 * octets arrivent fragmentes. Supposer un appel unique est le piege
 * numero un de l'integration : d'ou ces accumulateurs, et d'ou le
 * vecteur octet-par-octet du corpus.
 * ================================================================ */

typedef struct {
    char*  d;
    size_t len;
    size_t cap;
} Buf;

static int buf_ajoute(Buf* b, const char* p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < b->len + n + 1) cap *= 2;
        char* d = (char*)realloc(b->d, cap);
        if (!d) return -1;
        b->d = d; b->cap = cap;
    }
    memcpy(b->d + b->len, p, n);
    b->len += n;
    b->d[b->len] = '\0';
    return 0;
}

static void buf_vide(Buf* b)   { b->len = 0; if (b->d) b->d[0] = '\0'; }
static void buf_libere(Buf* b) { free(b->d); b->d = NULL; b->len = b->cap = 0; }

static const char* buf_txt(const Buf* b) { return b->d ? b->d : ""; }

/* ================================================================
 * Comparaisons insensibles a la casse (ASCII seulement : les tokens
 * HTTP le sont par definition)
 * ================================================================ */

static int min_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int egal_i(const char* a, const char* b) {
    while (*a && *b) {
        if (min_ascii((unsigned char)*a) != min_ascii((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int commence_par_i(const char* s, const char* prefixe) {
    while (*prefixe) {
        if (min_ascii((unsigned char)*s) != min_ascii((unsigned char)*prefixe)) return 0;
        s++; prefixe++;
    }
    return 1;
}

/* ================================================================
 * Contexte de parsing
 * ================================================================ */

/* TOUT l'etat par-requete vit dans ce bloc, et rien d'autre n'y vit.
 *
 * Ce n'est pas une coquetterie de rangement : le contrat §6 designe « une
 * remise a zero imparfaite de l'etat du parseur entre deux requetes »
 * comme un vecteur de smuggling A ELLE SEULE. Un etat eparpille en
 * champs de Ctx se remet a zero par une liste, et une liste s'oublie. Un
 * bloc contigu se remet a zero par un memset, et un champ ajoute plus
 * tard y tombe d'office. La verification `etat_vierge` relit ce bloc
 * octet par octet apres chaque remise a zero. */
typedef struct {
    size_t entetes_octets;
    size_t extension_octets;

    int entetes_finis;   /* on_headers_complete passe : la suite est trailer */
    int message_fini;
    int rejet;           /* code HTTP de rejet, 0 = aucun */
    int envoyer_100;
    /* Releve DANS on_headers_complete. llhttp remet ses drapeaux internes
     * a zero en fin de message (mesure ) : le lire apres coup
     * rendrait « ferme » sur une connexion parfaitement reutilisable. */
    int garder_connexion;

    size_t nb_content_length;
    size_t nb_host;
    const char* host;    /* pointe dans la table d'en-tetes de la requete */
} EtatRequete;

typedef struct {
    const OwlHttpServeurConfig* cfg;
    OwlHttpRequete*             req;

    /* Accumulateurs : l'ALLOCATION survit d'une requete a l'autre, le
     * CONTENU non. Un tampon de cible non vide fabriquerait « /un/deux »
     * a partir de deux requetes distinctes. */
    Buf cible;
    Buf champ;
    Buf valeur;
    Buf corps;

    EtatRequete r;
} Ctx;

#define CTX(p) ((Ctx*)((p)->data))

/* Un rejet arme le code puis coupe le parseur. Rendre -1 depuis un
 * callback fait remonter une erreur a llhttp_execute ; c'est notre code
 * qui porte le statut, pas le sien. */
static int rejette(Ctx* c, int statut) {
    if (!c->r.rejet) c->r.rejet = statut;
    return -1;
}

/* ================================================================
 * Callbacks llhttp
 * ================================================================ */

static int cb_url(llhttp_t* p, const char* at, size_t len) {
    Ctx* c = CTX(p);
    /* La ligne de requete, c'est methode + SP + cible + SP + version +
     * CRLF. On reserve forfaitairement de quoi loger le reste : le
     * plafond doit mordre pendant l'accumulation, la precision au
     * caractere pres n'a aucune valeur de securite. */
    const size_t reserve = 32;
    size_t plafond = c->cfg->limites.ligne_requete_max;
    plafond = (plafond > reserve) ? plafond - reserve : 0;

    if (c->cible.len + len > plafond) return rejette(c, 414);
    if (buf_ajoute(&c->cible, at, len) != 0) return rejette(c, 500);
    return 0;
}

static int cb_header_field(llhttp_t* p, const char* at, size_t len) {
    Ctx* c = CTX(p);
    if (c->r.entetes_finis) return 0;   /* trailer : jamais livre, jamais cadrant */

    c->r.entetes_octets += len;
    if (c->r.entetes_octets > c->cfg->limites.entetes_octets_max) return rejette(c, 431);
    if (buf_ajoute(&c->champ, at, len) != 0) return rejette(c, 500);
    return 0;
}

static int cb_header_value(llhttp_t* p, const char* at, size_t len) {
    Ctx* c = CTX(p);
    if (c->r.entetes_finis) return 0;

    c->r.entetes_octets += len;
    if (c->r.entetes_octets > c->cfg->limites.entetes_octets_max) return rejette(c, 431);
    if (buf_ajoute(&c->valeur, at, len) != 0) return rejette(c, 500);
    return 0;
}

static int cb_header_value_complete(llhttp_t* p) {
    Ctx* c = CTX(p);
    if (c->r.entetes_finis) { buf_vide(&c->champ); buf_vide(&c->valeur); return 0; }

    OwlHttpRequete* r = c->req;
    if (r->nb_entetes >= c->cfg->limites.entetes_nb_max) return rejette(c, 431);

    char* nom = (char*)malloc(c->champ.len + 1);
    char* val = (char*)malloc(c->valeur.len + 1);
    if (!nom || !val) { free(nom); free(val); return rejette(c, 500); }
    memcpy(nom, buf_txt(&c->champ), c->champ.len + 1);
    memcpy(val, buf_txt(&c->valeur), c->valeur.len + 1);

    r->entetes[r->nb_entetes].nom    = nom;
    r->entetes[r->nb_entetes].valeur = val;
    r->nb_entetes++;

    if (egal_i(nom, "content-length")) c->r.nb_content_length++;
    if (egal_i(nom, "host")) { c->r.nb_host++; if (c->r.nb_host == 1) c->r.host = val; }

    buf_vide(&c->champ);
    buf_vide(&c->valeur);
    return 0;
}

static int cb_chunk_extension(llhttp_t* p, const char* at, size_t len) {
    (void)at;
    Ctx* c = CTX(p);
    /* llhttp ne borne pas les extensions de chunk : une extension d'un
     * mega-octet passerait sans bruit. Plafond a notre charge. */
    c->r.extension_octets += len;
    if (c->r.extension_octets > c->cfg->limites.chunk_extension_max) return rejette(c, 400);
    return 0;
}

static int cb_chunk_header(llhttp_t* p) {
    CTX(p)->r.extension_octets = 0;
    return 0;
}

static int cb_body(llhttp_t* p, const char* at, size_t len) {
    Ctx* c = CTX(p);
    if (c->corps.len + len > c->cfg->limites.corps_max) return rejette(c, 413);
    if (buf_ajoute(&c->corps, at, len) != 0) return rejette(c, 500);
    return 0;
}

/* Fin de message : on ARRETE llhttp net.
 *
 * Sans cette pause, llhttp_execute continuerait a devider le tampon et
 * declencherait les callbacks de la requete SUIVANTE alors que le
 * contexte porte encore la precedente — l'etat de la premiere entrerait
 * dans la seconde par construction, ce qui est exactement le vecteur 6.1.
 * La pause rend la main avec la position du premier octet non consomme :
 * on sert, on remet a zero, on reprend. */
static int cb_message_complete(llhttp_t* p) {
    CTX(p)->r.message_fini = 1;
    return HPE_PAUSED;
}

/* ---------- Politique appliquee a la fin des en-tetes ----------
 *
 * NOTE DE FRONTIERE. Le contrat §6 classe Host et la forme absolue
 * cote « politique », donc au-dessus du transport (phase A0.3), tandis
 * que le present ticket exige la famille 2 du corpus integralement
 * verte des maintenant. Les deux ne peuvent pas etre vrais en meme
 * temps ; ces controles vivent donc ici, ISOLES dans cette fonction,
 * pour qu'A0.3 les deplace sans toucher au reste. Point remonte au
 * coordinator. */
static int politique_entetes(Ctx* c, llhttp_t* p);

static int cb_headers_complete(llhttp_t* p) {
    Ctx* c = CTX(p);
    c->r.entetes_finis = 1;
    int st = politique_entetes(c, p);
    /* Releve ICI et pas plus tard : llhttp_should_keep_alive lit des
     * drapeaux que le parseur remet a zero en fin de message. */
    if (st == 0) c->r.garder_connexion = llhttp_should_keep_alive(p);
    return st;
}

/* ================================================================
 * Forme absolue : extraction du chemin et de l'autorite
 *
 * RFC 9112 §3.2.2 : un serveur DOIT accepter la forme absolue. La
 * parade n'est donc pas le refus, c'est que l'autorite n'entre JAMAIS
 * dans le routage.
 * ================================================================ */

static const char* separe_cible(const char* cible, char* autorite, size_t taille) {
    autorite[0] = '\0';
    const char* apres = NULL;

    if (commence_par_i(cible, "http://"))       apres = cible + 7;
    else if (commence_par_i(cible, "https://")) apres = cible + 8;
    else return cible;   /* origin-form, asterisk-form : rien a separer */

    const char* fin = apres;
    while (*fin && *fin != '/' && *fin != '?' && *fin != '#') fin++;

    /* userinfo@hote : seul l'hote compte pour la comparaison a Host. */
    const char* debut = apres;
    for (const char* q = apres; q < fin; q++) if (*q == '@') debut = q + 1;

    size_t n = (size_t)(fin - debut);
    if (n >= taille) n = taille - 1;
    memcpy(autorite, debut, n);
    autorite[n] = '\0';

    return (*fin == '/') ? fin : "/";
}

static int politique_entetes(Ctx* c, llhttp_t* p) {
    OwlHttpRequete* r = c->req;
    const OwlHttpLimites* lim = &c->cfg->limites;

    /* Versions servies : 1.0 et 1.1. Tout le reste (0.9 sans version,
     * 1.2, 2.0 en clair, 01.1) tombe ici si llhttp l'a laisse passer. */
    r->version_majeure = llhttp_get_http_major(p);
    r->version_mineure = llhttp_get_http_minor(p);
    if (r->version_majeure != 1 || (r->version_mineure != 0 && r->version_mineure != 1)) {
        return rejette(c, 400);
    }

    const char* m = llhttp_method_name((llhttp_method_t)llhttp_get_method(p));
    snprintf(r->methode, sizeof(r->methode), "%s", m ? m : "");
    r->est_head = (llhttp_get_method(p) == HTTP_HEAD);

    /* Content-Length duplique, meme a valeurs identiques : llhttp durcit
     * le cas divergent, celui-ci nous revient (contrat 1.3). Replier
     * deux en-tetes de cadrage en un seul est precisement ce sur quoi
     * deux intermediaires peuvent differer. */
    if (c->r.nb_content_length > 1) return rejette(c, 400);

    /* Espace residuel autour d'une valeur de CADRAGE.
     *
     * Mesure (probe llhttp 9.4.2, ) : « Content-Length: 5 »
     * ressort avec la valeur brute « 5 » suivie d'un espace ET un
     * content_length interne de 5 — llhttp l'ACCEPTE. Le contrat range
     * pourtant ce vecteur (1.4) cote « L », en pretendant que llhttp le
     * rejette : la prediction est fausse, et RFC 9112 §5 donne raison a
     * llhttp (l'OWS de fin fait partie de la grammaire et se retire).
     *
     * On durcit quand meme, et seulement sur les deux en-tetes qui
     * DECIDENT DU CADRAGE. Motif : la valeur de ces deux champs est
     * exactement ce sur quoi deux intermediaires peuvent diverger, et
     * aucun client legitime n'emet d'espace la. Divergence assumee et
     * bornee — elle ne touche aucun autre en-tete. */
    for (size_t i = 0; i < r->nb_entetes; i++) {
        const char* nom = r->entetes[i].nom;
        if (!egal_i(nom, "content-length") && !egal_i(nom, "transfer-encoding")) continue;
        const char* v = r->entetes[i].valeur;
        size_t vl = strlen(v);
        if (vl == 0) return rejette(c, 400);
        if (v[0] == ' ' || v[0] == '\t' || v[vl - 1] == ' ' || v[vl - 1] == '\t') {
            return rejette(c, 400);
        }
    }

    /* Cible : octets bruts, ni decodes ni normalises (contrat §3). */
    size_t n = c->cible.len;
    r->cible = (char*)malloc(n + 2);
    if (!r->cible) return rejette(c, 500);
    if (n == 0) { r->cible[0] = '/'; r->cible[1] = '\0'; }
    else        { memcpy(r->cible, buf_txt(&c->cible), n + 1); }

    char autorite[256];
    r->chemin = separe_cible(r->cible, autorite, sizeof(autorite));

    /* Host — cf. la note de frontiere ci-dessus. */
    if (c->r.nb_host > 1) return rejette(c, 400);
    if (c->r.nb_host == 0 && r->version_mineure == 1) return rejette(c, 400);

    if (autorite[0] && c->r.host && !egal_i(autorite, c->r.host)) return rejette(c, 400);

    /* Allow-list (vecteur 2.18). Le transport applique, il ne decide pas :
     * le predicat vit dans http_politique.c pour rester identique quel que
     * soit le transport. Desactivee par defaut. */
    if (!owl_http_politique_hote_permis(c->cfg->hotes_autorises,
                                        c->cfg->nb_hotes_autorises,
                                        c->r.host ? c->r.host : autorite)) {
        return rejette(c, 400);
    }

    /* Corps annonce au-dela du plafond : 413 tout de suite, sans lire
     * la suite. C'est la difference entre une limite et un piege. */
    if ((p->flags & F_CONTENT_LENGTH) && p->content_length > (uint64_t)lim->corps_max) {
        return rejette(c, 413);
    }

    /* Expect. L'ordre compte : le 413 ci-dessus passe AVANT, pour ne
     * jamais inviter un client a envoyer ce qu'on refusera. */
    for (size_t i = 0; i < r->nb_entetes; i++) {
        if (!egal_i(r->entetes[i].nom, "expect")) continue;
        if (egal_i(r->entetes[i].valeur, "100-continue")) c->r.envoyer_100 = 1;
        else return rejette(c, 417);
    }
    return 0;
}

/* ================================================================
 * Reponse
 * ================================================================ */

/* Reason-phrases canon du registre IANA (RFC 9110 sec.15). Couvre au moins
 * tous les codes exposes par l'enum httpCodeEtat (ce qu'un service peut poser
 * via MoiMeme..CodeEtat) + les 3xx/4xx/5xx courants. Le reason-phrase est
 * purement informatif (le client se fie au code) : pour un code inconnu, on
 * renvoie une chaine vide plutot qu'un libelle trompeur — HTTP/1.1 201 \r\n
 * est valide (RFC 9110 sec.15 : le reason-phrase est optionnel). */
static const char* raison(int code) {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 421: return "Misdirected Request";
        case 422: return "Unprocessable Content";
        case 426: return "Upgrade Required";
        case 428: return "Precondition Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "";
    }
}

/* Pas de chunked en sortie, jamais : la decision « pas de streaming »
 * garantit que la longueur est toujours connue. C'est un benefice
 * acquis (toute une classe de bugs de cadrage disparait), pas un oubli. */
/* Puits vers la socket. Le corps ne transite QUE par ce chemin : le
 * transport ne suppose donc jamais un tampon contigu (contrat §5), ce
 * qui rendra le mode B (handle de fichier, A0.3) purement additif. */
typedef struct {
    OwlTcpConn* conn;
    int         timeout_ms;
} PuitsSocket;

static int puits_socket_ecrit(void* user, const char* octets, size_t len) {
    PuitsSocket* p = (PuitsSocket*)user;
    return owl_tcp_conn_ecrit(p->conn, octets, len, p->timeout_ms) == OWL_TCPS_OK
           ? 0 : -1;
}

/* 1xx, 204 et 304 n'ont PAS de contenu (RFC 9110 §6.4.1) : ni corps, ni
 * Content-Length. Emettre un Content-Length non nul sur un 304 est un
 * cadrage incoherent — exactement ce que le vecteur 5.6 cherche. */
static bool sans_contenu(int statut) {
    return statut == 204 || statut == 304 || (statut >= 100 && statut < 200);
}

static bool ecrit_reponse_flux(OwlTcpConn* conn, const OwlHttpRep* r,
                               bool head, int timeout_ms);

static bool ecrit_reponse(OwlTcpConn* conn, const OwlHttpRep* r,
                          bool head, int timeout_ms, bool garder_ouvert) {
    /* Mode C : reponse en flux -> Transfer-Encoding: chunked,
     * jamais Content-Length, et la connexion se ferme toujours (la
     * longueur reste inconnue jusqu'au dernier chunk). */
    if (owl_http_rep_est_flux(r)) {
        (void)garder_ouvert;
        return ecrit_reponse_flux(conn, r, head, timeout_ms);
    }

    int         statut    = owl_http_rep_statut(r);
    const char* type      = owl_http_rep_type_contenu(r);
    size_t      corps_len = owl_http_rep_longueur(r);
    bool        nu        = sans_contenu(statut);

    char tete[2048];
    int t = snprintf(tete, sizeof(tete), "HTTP/1.1 %d %s\r\n", statut, raison(statut));
    if (!nu && type && type[0]) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Content-Type: %s\r\n", type);
    }

    /* En-tetes de la reponse. Ils sont deja passes par le point de
     * controle unique a l'insertion (owl_http_rep_ajoute_entete) : aucun
     * ne peut porter de CR/LF, donc aucune scission n'est possible ici. */
    for (size_t i = 0; i < owl_http_rep_nb_entetes(r); i++) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "%s: %s\r\n",
                      owl_http_rep_entete_nom(r, i),
                      owl_http_rep_entete_valeur(r, i));
        if ((size_t)t >= sizeof(tete)) return false;
    }

    if (!nu) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t,
                      "Content-Length: %lu\r\n", (unsigned long)corps_len);
    }
    /* La disposition est ANNONCEE, dans les deux sens. Un client HTTP/1.0
     * ne suppose la reutilisation que si on l'ecrit ; un client 1.1 doit
     * savoir laquelle est la derniere. Le silence se lit differemment
     * selon la version — soit une divergence d'interpretation de plus
     * entre intermediaires, et on en a assez comme ca. */
    t += snprintf(tete + t, sizeof(tete) - (size_t)t,
                  garder_ouvert ? "Connection: keep-alive\r\n\r\n"
                                : "Connection: close\r\n\r\n");
    if ((size_t)t >= sizeof(tete)) return false;

    if (owl_tcp_conn_ecrit(conn, tete, (size_t)t, timeout_ms) != OWL_TCPS_OK) return false;

    /* HEAD : memes en-tetes qu'un GET, y compris le Content-Length, mais
     * corps vide. Le cadrage reste coherent. */
    if (!head && !nu && corps_len > 0) {
        PuitsSocket p;
        p.conn = conn;
        p.timeout_ms = timeout_ms;
        OwlHttpPuits puits;
        puits.ecrit = puits_socket_ecrit;
        puits.user  = &p;
        if (owl_http_rep_ecrit(r, &puits) != 0) return false;
    }
    return true;
}

/* Un puits qui refuse tout octet : sert a stopper le producteur d'un flux
 * (il pose consommateur_fini via l'echec du drain) sans emettre de corps —
 * exactement ce que demande un HEAD sur une reponse en flux. */
static int puits_mort_ecrit(void* user, const char* octets, size_t len) {
    (void)user; (void)octets; (void)len;
    return -1;
}

/* Mode C. L'en-tete est decide APRES que le sort du 1er chunk
 * est connu (D3) : un echec AVANT le moindre octet de corps laisse encore
 * la place a un 500 propre ; passe ce point, le statut est fige et une
 * erreur mi-flux devient une fermeture abrupte (pas de chunk terminal). */
static bool ecrit_reponse_flux(OwlTcpConn* conn, const OwlHttpRep* r,
                               bool head, int timeout_ms) {
    OwlHttpFluxAmorce amorce =
        owl_http_rep_flux_attend_premier((OwlHttpRep*)r);

    /* Avorte avant le 1er octet : le corps n'a jamais commence, on peut
     * encore repondre proprement. 500, connexion fermee. */
    if (amorce == OWL_HTTP_FLUX_AMORCE_AVORTE) {
        OwlHttpRep* err = owl_http_rep_cree_tampon(500, "text/plain", NULL, 0);
        bool ok = err ? ecrit_reponse(conn, err, head, timeout_ms, false) : false;
        owl_http_rep_libere(err);
        return ok;
    }

    int         statut = owl_http_rep_statut(r);
    const char* type   = owl_http_rep_type_contenu(r);

    char tete[2048];
    int t = snprintf(tete, sizeof(tete), "HTTP/1.1 %d %s\r\n", statut, raison(statut));
    if (type && type[0]) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Content-Type: %s\r\n", type);
    }
    for (size_t i = 0; i < owl_http_rep_nb_entetes(r); i++) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "%s: %s\r\n",
                      owl_http_rep_entete_nom(r, i),
                      owl_http_rep_entete_valeur(r, i));
        if ((size_t)t >= sizeof(tete)) return false;
    }
    /* Corps de longueur inconnue -> chunked, jamais Content-Length. Un
     * flux ne se garde pas ouvert : la fin du corps n'est connue qu'au
     * dernier chunk, on annonce donc toujours la fermeture. */
    t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Transfer-Encoding: chunked\r\n");
    t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Connection: close\r\n\r\n");
    if ((size_t)t >= sizeof(tete)) return false;

    if (owl_tcp_conn_ecrit(conn, tete, (size_t)t, timeout_ms) != OWL_TCPS_OK) {
        /* En-tete non parti : stopper le producteur pour ne pas le laisser
         * pousser dans une file que plus personne ne draine. */
        OwlHttpPuits mort; mort.ecrit = puits_mort_ecrit; mort.user = NULL;
        owl_http_rep_ecrit(r, &mort);
        return false;
    }

    /* HEAD : memes en-tetes qu'un GET, corps vide. On ne draine pas — mais
     * il faut stopper le producteur, sinon il bloque sur la contre-pression.
     * Le puits mort pose consommateur_fini via l'echec du drain. */
    if (head) {
        OwlHttpPuits mort; mort.ecrit = puits_mort_ecrit; mort.user = NULL;
        owl_http_rep_ecrit(r, &mort);
        return true;
    }

    PuitsSocket p;
    p.conn = conn;
    p.timeout_ms = timeout_ms;
    OwlHttpPuits puits;
    puits.ecrit = puits_socket_ecrit;
    puits.user  = &p;
    /* Drain : 0 = terminal emis (flux complet), non-nul = fermeture abrupte
     * (avorte mi-flux ou socket morte). Dans les deux cas la connexion se
     * ferme (garder_ouvert etait force a false). */
    return owl_http_rep_ecrit(r, &puits) == 0;
}

/* ================================================================
 * Drain de flux DANS LA BOUCLE REACTEUR (, W5)
 *
 * Meme cadrage chunked qu'ecrit_reponse_flux, mais NON BLOQUANT et pilote
 * par POLLOUT : le worker n'est plus retenu le temps du flux (design §1). Le
 * worker fait le setup (lecture requete + handler), puis remet la connexion
 * au reacteur (owl_tcp_pool_reac_prise) ; la boucle draine ensuite les chunks
 * au fil de leur disponibilite, reveillee par le producteur (self-pipe W4).
 *
 * Cycle de vie de OwlHttpRep a travers l'async : le worker transfere la
 * SEULE reference transport (celle rendue par le handler) dans HttpFluxDrain.
 * Le reacteur la relache une fois — au FINI (http_flux_detruit). Aucun objet
 * GC ne traverse : le flux est du C-heap refcompte (design §8 Q1).
 * ================================================================ */

typedef struct {
    OwlHttpRep* rep;           /* reference transport unique, transferee ici */
    int         head;          /* requete HEAD : en-tetes seuls, pas de corps */
    int         entete_faite;  /* ligne de statut + en-tetes emis */
    int         terminal_fait; /* dernier octet a emettre est dans le tampon */
    char*       out;           /* tampon de sortie (au plus un cadre a la fois) */
    size_t      out_len;
    size_t      out_cap;
    size_t      out_off;       /* deja envoye du tampon (send partiel) */

    /* Phase ATTENTE exec async (W7) : le rep n'est pas encore connu — un jeton
     * est en cours d'execution. Le drain le resout via essaie a chaque pas,
     * SANS bloquer. `jeton` NULL une fois resolu (ne plus jamais y toucher :
     * l'executeur peut le liberer). `bufferise` = reponse resolue en mode
     * tampon (Content-Length PRESERVE, pas de chunked). */
    OwlHttpExecuteur* x_async;
    OwlHttpJeton*     jeton;
    int               bufferise;

    /* Pool possedant le drain : necessaire pour reveiller la boucle quand le
     * producteur pousse un chunk (definit_notif sur le flux resolu, W7). Sur le
     * chemin async le flux n'est connu qu'apres essaie -> le reveil du flux doit
     * y etre pose la, pas au handoff (ou seul le jeton existe). */
    OwlTcpPool*       pool;
} HttpFluxDrain;

/* Reveil de la boucle depuis le producteur (defini plus bas). */
static void http_flux_notif(void* user);

static int hfd_append(HttpFluxDrain* hd, const char* data, size_t len) {
    if (len == 0) return 0;
    if (hd->out_len + len > hd->out_cap) {
        size_t nc = hd->out_cap ? hd->out_cap : 512;
        while (nc < hd->out_len + len) nc *= 2;
        char* nb = (char*)realloc(hd->out, nc);
        if (!nb) return -1;
        hd->out = nb;
        hd->out_cap = nc;
    }
    memcpy(hd->out + hd->out_len, data, len);
    hd->out_len += len;
    return 0;
}

/* En-tete de flux : byte-identique a ecrit_reponse_flux (chunked + close). */
static int hfd_frame_entete(HttpFluxDrain* hd) {
    const OwlHttpRep* r = hd->rep;
    int statut = owl_http_rep_statut(r);
    const char* type = owl_http_rep_type_contenu(r);

    char tete[2048];
    int t = snprintf(tete, sizeof(tete), "HTTP/1.1 %d %s\r\n", statut, raison(statut));
    if (type && type[0]) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Content-Type: %s\r\n", type);
    }
    for (size_t i = 0; i < owl_http_rep_nb_entetes(r); i++) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "%s: %s\r\n",
                      owl_http_rep_entete_nom(r, i), owl_http_rep_entete_valeur(r, i));
        if ((size_t)t >= sizeof(tete)) return -1;
    }
    t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Transfer-Encoding: chunked\r\n");
    t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Connection: close\r\n\r\n");
    if ((size_t)t >= sizeof(tete)) return -1;
    return hfd_append(hd, tete, (size_t)t);
}

/* Avorte avant le 1er octet : 500 propre, identique a un cree_tampon(500). */
static int hfd_frame_500(HttpFluxDrain* hd) {
    char tete[256];
    int t = snprintf(tete, sizeof(tete),
                     "HTTP/1.1 500 %s\r\nContent-Type: text/plain\r\n"
                     "Content-Length: 0\r\nConnection: close\r\n\r\n", raison(500));
    if (t <= 0 || (size_t)t >= sizeof(tete)) return -1;
    return hfd_append(hd, tete, (size_t)t);
}

/* Un chunk : prefixe hex CRLF / donnees / CRLF (identique a flux_draine). */
static int hfd_frame_chunk(HttpFluxDrain* hd, const char* d, size_t l) {
    char pre[32];
    int n = snprintf(pre, sizeof(pre), "%lx\r\n", (unsigned long)l);
    if (n <= 0 || hfd_append(hd, pre, (size_t)n) != 0) return -1;
    if (hfd_append(hd, d, l) != 0) return -1;
    return hfd_append(hd, "\r\n", 2);
}

/* Puits qui accumule le corps d'une reponse bufferisee dans `out` (W7). */
static int hfd_puits_append(void* user, const char* octets, size_t len) {
    return hfd_append((HttpFluxDrain*)user, octets, len);
}

/* Reponse BUFFERISEE (exec async resolu en tampon) : Content-Length PRESERVE,
 * PAS de conversion chunked (le cadrage de C3 et de la politique reste intact).
 * Connection: close comme le reste du chemin reacteur (la boucle de service
 * keep-alive n'est pas re-entree apres un handoff). Tout est frame dans `out`
 * en une passe, puis draine par POLLOUT ; terminal_fait pose a la fin. */
static int hfd_frame_bufferise(HttpFluxDrain* hd) {
    const OwlHttpRep* r = hd->rep;
    int    statut = owl_http_rep_statut(r);
    const char* type = owl_http_rep_type_contenu(r);
    size_t len    = owl_http_rep_longueur(r);

    char tete[2560];
    int t = snprintf(tete, sizeof(tete), "HTTP/1.1 %d %s\r\n", statut, raison(statut));
    if (type && type[0]) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Content-Type: %s\r\n", type);
    }
    for (size_t i = 0; i < owl_http_rep_nb_entetes(r); i++) {
        t += snprintf(tete + t, sizeof(tete) - (size_t)t, "%s: %s\r\n",
                      owl_http_rep_entete_nom(r, i), owl_http_rep_entete_valeur(r, i));
        if ((size_t)t >= sizeof(tete)) return -1;
    }
    t += snprintf(tete + t, sizeof(tete) - (size_t)t,
                  "Content-Length: %lu\r\n", (unsigned long)len);
    t += snprintf(tete + t, sizeof(tete) - (size_t)t, "Connection: close\r\n\r\n");
    if ((size_t)t >= sizeof(tete)) return -1;
    if (hfd_append(hd, tete, (size_t)t) != 0) return -1;

    /* HEAD : en-tetes seuls (Content-Length reflete quand meme la taille). */
    if (!hd->head && len > 0) {
        OwlHttpPuits puits;
        puits.ecrit = hfd_puits_append;
        puits.user  = hd;
        if (owl_http_rep_ecrit(r, &puits) != 0) return -1;
    }
    hd->terminal_fait = 1;
    return 0;
}

/* Un pas de drain (thread boucle). Voir OwlTcpFluxDrain. */
static OwlTcpFluxProgres http_flux_drain(OwlTcpConn* conn, void* appctx,
                                         short evts, void* user) {
    (void)user;
    HttpFluxDrain* hd = (HttpFluxDrain*)appctx;

    /* Deconnexion mi-flux. Le reacteur remonte OWL_REAC_ERREUR
     * (POLLHUP/POLLERR) meme sans etre demande. Sur un pair PERDU (RST/erreur de
     * connexion) on coupe la generation en amont, sans courir jusqu'a
     * ..MaxTokens (le zombie qui plombe l'executeur mono-thread). Un demi-close
     * (le pair a fini d'emettre mais LIT encore) N'est PAS coupe. pair_perdu ne
     * rend true que sur preuve fiable de depart, cf. owl_tcp_conn_pair_perdu. */
    int pair_parti = (evts & OWL_REAC_ERREUR) && owl_tcp_conn_pair_perdu(conn);

    /* Phase ATTENTE exec async (W7) : resoudre le jeton SANS bloquer. Tant
     * qu'il n'a ni demarre en flux ni fini, rendre ATTEND (le fd est retire,
     * le reveil producteur / le timer d'echeance le re-poussera). */
    if (hd->jeton) {
        OwlHttpRep*   rep = NULL;
        OwlHttpEssaie e   = owl_http_executeur_essaie(hd->x_async, hd->jeton, &rep);
        if (e == OWL_HTTP_ESSAIE_ATTEND) {
            /* Job pas encore demarre. Si le client est deja parti, l'ABANDONNER
             * maintenant (abort race-safe cote http_exec) plutot que le laisser
             * tourner jusqu'a ..MaxTokens : c'est la coupe du zombie. essaie a
             * rendu ATTEND -> le job n'a NI demarre en flux NI fini, donc le
             * producteur verra `abandonne` a l'ouverture du flux et s'arretera
             * sans pousser dans une file que plus personne ne draine. */
            if (pair_parti) {
                owl_http_executeur_abandonne(hd->x_async, hd->jeton);
                hd->jeton = NULL;
                return OWL_TCPS_FLUX_FINI;
            }
            return OWL_TCPS_FLUX_ATTEND;
        }
        hd->jeton = NULL;   /* resolu : plus jamais y toucher (l'exec peut le liberer) */
        if (e == OWL_HTTP_ESSAIE_FLUX) {
            hd->rep = rep;                     /* flux : drain chunked (chemin W5) */
            /* Reveil du flux : sans ca, une pousse du producteur ne reveille PAS
             * la boucle (le jeton, seul reveil pose au handoff, ne signale qu'au
             * 1er chunk puis disparait). Le drain retombait alors sur la seule
             * re-inscription opportuniste par trafic croise (reac_reveil_cb), un
             * spin qui degenere sous teardown concurrent. definit_notif ici pose
             * le meme reveil que le chemin W5 direct (mesure). */
            owl_http_rep_flux_definit_notif(rep, http_flux_notif, hd->pool);
        } else if (e == OWL_HTTP_ESSAIE_BUFFERISE) {
            hd->rep = rep; hd->bufferise = 1;  /* tampon : Content-Length preserve */
        } else {
            /* SATURE : file pleine expiree / echec -> 503 bufferise synthetise. */
            hd->rep = owl_http_rep_cree_tampon(503, NULL, NULL, 0);
            hd->bufferise = 1;
            if (!hd->rep) return OWL_TCPS_FLUX_FINI;
        }
    }

    /* Jeton resolu (ou flux W5 direct) et consommateur parti : stopper le
     * producteur (la prochaine pousse rend ABANDON -> arret gracieux de la
     * generation) et fermer, sans attendre l'echec d'un send inoperant. */
    if (pair_parti && hd->rep) {
        if (!hd->bufferise) owl_http_rep_flux_marque_consommateur_fini(hd->rep);
        return OWL_TCPS_FLUX_FINI;
    }

    OwlHttpRep* r = hd->rep;

    for (;;) {
        /* 1. Vider le tampon en attente (send partiels). */
        if (hd->out_off < hd->out_len) {
            int w = owl_tcp_conn_ecrit_dispo(conn, hd->out + hd->out_off,
                                             hd->out_len - hd->out_off);
            if (w < 0) {
                if (!hd->bufferise) owl_http_rep_flux_marque_consommateur_fini(r);
                return OWL_TCPS_FLUX_FINI;
            }
            hd->out_off += (size_t)w;
            if (hd->out_off < hd->out_len) return OWL_TCPS_FLUX_ENCORE;  /* garder POLLOUT */
            hd->out_len = hd->out_off = 0;   /* tout parti */
        }
        if (hd->terminal_fait) return OWL_TCPS_FLUX_FINI;   /* dernier octet parti */

        /* 2. Mode bufferise (W7) : framer une fois (en-tetes + corps), puis la
         * boucle vide `out` et FINI (terminal_fait pose par le framing). */
        if (hd->bufferise) {
            if (!hd->entete_faite) {
                if (hfd_frame_bufferise(hd) != 0) return OWL_TCPS_FLUX_FINI;
                hd->entete_faite = 1;
            }
            continue;
        }

        /* 2. Tirer le prochain chunk, sans bloquer. */
        char*  d = NULL;
        size_t l = 0;
        OwlHttpFluxTire t = owl_http_rep_flux_tire(r, &d, &l);
        if (t == OWL_HTTP_FLUX_TIRE_VIDE) {
            /* Rien dispo (en-tete pas encore ecrit -> on attend le 1er chunk ;
             * deja ecrit -> le suivant). Couper POLLOUT, attendre le reveil. */
            return OWL_TCPS_FLUX_ATTEND;
        }

        /* 3. En-tete d'abord (decision D3 : figee au sort du 1er chunk). */
        if (!hd->entete_faite) {
            if (t == OWL_HTTP_FLUX_TIRE_AVORTE || t == OWL_HTTP_FLUX_TIRE_MORT) {
                if (hfd_frame_500(hd) != 0) return OWL_TCPS_FLUX_FINI;
                hd->entete_faite = hd->terminal_fait = 1;
                continue;   /* remonter vider puis FINI */
            }
            if (hfd_frame_entete(hd) != 0) {
                owl_http_rep_flux_marque_consommateur_fini(r);
                if (t == OWL_HTTP_FLUX_TIRE_DONNEE) free(d);
                return OWL_TCPS_FLUX_FINI;
            }
            hd->entete_faite = 1;
            if (hd->head) {
                /* HEAD : en-tetes seuls. Stopper le producteur, pas de corps. */
                owl_http_rep_flux_marque_consommateur_fini(r);
                if (t == OWL_HTTP_FLUX_TIRE_DONNEE) free(d);
                hd->terminal_fait = 1;
                continue;
            }
            /* En-tete pose ; le chunk tire est traite ci-dessous. */
        }

        /* 4. Corps. */
        switch (t) {
            case OWL_HTTP_FLUX_TIRE_DONNEE:
                if (hfd_frame_chunk(hd, d, l) != 0) {
                    free(d);
                    owl_http_rep_flux_marque_consommateur_fini(r);
                    return OWL_TCPS_FLUX_FINI;
                }
                free(d);
                continue;
            case OWL_HTTP_FLUX_TIRE_FINI:
                if (hfd_append(hd, "0\r\n\r\n", 5) != 0) return OWL_TCPS_FLUX_FINI;
                hd->terminal_fait = 1;
                continue;
            case OWL_HTTP_FLUX_TIRE_AVORTE:
            case OWL_HTTP_FLUX_TIRE_MORT:
                /* Avorte APRES l'en-tete : fermeture abrupte, pas de terminal. */
                return OWL_TCPS_FLUX_FINI;
            case OWL_HTTP_FLUX_TIRE_VIDE:
                return OWL_TCPS_FLUX_ATTEND;   /* deja traite, par surete */
        }
    }
}

/*  P5 — borne de duree applicative d'un flux (voir OwlTcpFluxDelaiMax).
 * < 0 tant que l'exec async n'a pas resolu le jeton ; 0 pour une reponse
 * bufferisee (503 de synthese, tampon Content-Length) ou un flux sans borne ;
 * DelaiMax en ms pour un flux vivant. */
static int64_t http_flux_delaimax(void* appctx, void* user) {
    (void)user;
    HttpFluxDrain* hd = (HttpFluxDrain*)appctx;
    if (!hd) return 0;
    if (hd->jeton) return -1;                 /* exec async pas encore resolu */
    if (hd->bufferise || !hd->rep) return 0;  /* pas un flux vivant : aucune borne */
    if (!owl_http_rep_est_flux(hd->rep)) return 0;
    return (int64_t)owl_http_rep_flux_delai_max_ms(hd->rep);
}

static void http_flux_detruit(void* appctx, void* user) {
    (void)user;
    HttpFluxDrain* hd = (HttpFluxDrain*)appctx;
    if (!hd) return;
    if (hd->jeton) {
        /* Pending exec non resolu (reacteur en arret avant flux_demarre/fait) :
         * rendre le jeton a l'executeur (§8) — il termine le handler et jette
         * la reponse. Ne PAS liberer le jeton ici (l'executeur en devient
         * proprietaire des lors qu'il n'est pas encore fait). */
        owl_http_executeur_abandonne(hd->x_async, hd->jeton);
        hd->jeton = NULL;
    }
    if (hd->rep) {
        /* Couper le reveil (plus de boucle a signaler) et stopper un
         * producteur eventuel AVANT de relacher la reference transport : la
         * prochaine pousse rend ABANDON, le producteur relache SA reference,
         * le dernier relache libere reellement (pas de fuite, pas d'UAF). */
        owl_http_rep_flux_definit_notif(hd->rep, NULL, NULL);
        owl_http_rep_flux_marque_consommateur_fini(hd->rep);
        owl_http_rep_libere(hd->rep);
    }
    free(hd->out);
    free(hd);
}

/* Reveil : le producteur (thread VM ou thread de test) signale la boucle. */
static void http_flux_notif(void* user) {
    owl_tcp_pool_reac_signale((OwlTcpPool*)user);
}

/* ================================================================
 * Configuration
 * ================================================================ */

void owl_http_limites_defaut(OwlHttpLimites* lim) {
    memset(lim, 0, sizeof(*lim));
    lim->ligne_requete_max   = 8 * 1024;
    lim->entetes_nb_max      = OWL_HTTP_ENTETES_MAX;
    lim->entetes_octets_max  = 16 * 1024;
    lim->corps_max           = 1024 * 1024;
    lim->chunk_extension_max = 4 * 1024;
    lim->requetes_max_par_connexion = 100;

    lim->timeout_entetes_ms    = 5000;
    lim->timeout_corps_ms      = 10000;
    lim->timeout_ecriture_ms   = 5000;
    lim->timeout_inactivite_ms = 5000;
}

void owl_http_serveur_config_defaut(OwlHttpServeurConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    owl_http_limites_defaut(&cfg->limites);
}

/* Plan de controle vs plan de donnees (, W2). Comparaison exacte,
 * casse comprise (les chemins HTTP sont sensibles a la casse), sur le chemin
 * ampute de sa chaine de requete. Le choix des routes est FIGE ici et nulle
 * part ailleurs. */
bool owl_http_route_est_controle(const char* chemin) {
    if (!chemin) return false;
    size_t n = 0;
    while (chemin[n] && chemin[n] != '?') n++;

    static const char* const routes[] = { "/v1/models", "/health", "/sante" };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (strlen(routes[i]) == n && strncmp(chemin, routes[i], n) == 0) return true;
    }
    return false;
}

/* Les reglages doivent survivre au parseur (exigence llhttp), pas au
 * service de la connexion : ils vivent donc sur la pile du thread qui
 * sert, comme tout le reste de l'etat.
 *
 * Ils etaient statiques et initialises paresseusement en A0.1, ce qui
 * etait exact tant qu'une seule connexion tournait a la fois. Avec N
 * workers, cette initialisation paresseuse est une course a l'ecriture
 * — et un etat global partage entre connexions concurrentes, ce que le
 * lot A1.1b elimine par principe. Cent octets recopies par connexion
 * contre un partage a prouver : le choix est vite fait. */
static void prepare_reglages(llhttp_settings_t* r) {
    llhttp_settings_init(r);
    r->on_url                    = cb_url;
    r->on_header_field           = cb_header_field;
    r->on_header_value           = cb_header_value;
    r->on_header_value_complete  = cb_header_value_complete;
    r->on_headers_complete       = cb_headers_complete;
    r->on_chunk_extension_name   = cb_chunk_extension;
    r->on_chunk_extension_value  = cb_chunk_extension;
    r->on_chunk_header           = cb_chunk_header;
    r->on_body                   = cb_body;
    r->on_message_complete       = cb_message_complete;
}

static void libere_requete(OwlHttpRequete* r) {
    for (size_t i = 0; i < r->nb_entetes; i++) {
        free(r->entetes[i].nom);
        free(r->entetes[i].valeur);
    }
    r->nb_entetes = 0;
    free(r->cible);
    r->cible  = NULL;
    r->chemin = NULL;
}

/* ================================================================
 * Remise a zero entre deux requetes d'une meme connexion
 *
 * Le point que le contrat §6 designe comme un vecteur de smuggling a lui
 * seul. Deux precautions, pas une :
 *   - la remise a zero passe par un MEMSET d'un bloc contigu, pour qu'un
 *     champ ajoute plus tard y tombe sans qu'on ait a y penser ;
 *   - elle est RELUE. `etat_vierge` n'est pas une assertion de confort :
 *     c'est la seule chose qui distingue « on a remis a zero » de « on
 *     croit avoir remis a zero ». Le cout est une centaine d'octets
 *     compares par requete ; le prix de l'oubli est une requete
 *     fabriquee.
 * ================================================================ */

static void requete_suivante(Ctx* c) {
    buf_vide(&c->cible);
    buf_vide(&c->champ);
    buf_vide(&c->valeur);
    buf_vide(&c->corps);
    memset(&c->r, 0, sizeof(c->r));

    OwlHttpRequete* q = c->req;
    char pair[sizeof(q->pair)];
    memcpy(pair, q->pair, sizeof(pair));
    libere_requete(q);
    memset(q, 0, sizeof(*q));
    memcpy(q->pair, pair, sizeof(pair));
}

static bool etat_vierge(const Ctx* c) {
    const unsigned char* p = (const unsigned char*)&c->r;
    for (size_t i = 0; i < sizeof(c->r); i++) if (p[i]) return false;

    if (c->cible.len || c->champ.len || c->valeur.len || c->corps.len) return false;

    const OwlHttpRequete* q = c->req;
    return q->nb_entetes == 0 && q->cible == NULL && q->chemin == NULL
        && q->corps == NULL && q->corps_len == 0 && q->methode[0] == '\0'
        && q->version_majeure == 0 && q->version_mineure == 0 && !q->est_head;
}

/* ================================================================
 * Service d'une connexion
 * ================================================================ */

/* Prise en charge d'un flux par le reacteur (, W5). Remplie par
 * sert_requete quand la reponse est un flux ET qu'un drain reacteur est
 * disponible : la reponse (reference transport transferee) et la conn sont a
 * remettre au reacteur au lieu d'etre drainees en bloquant sur le worker. */
typedef struct {
    int         actif;
    OwlHttpRep* rep;    /* reference transport transferee (NON liberee ici) */
    int         head;

    /* Pending exec async (W7) : le worker a DEPOSE un job sans attendre. Le
     * reacteur resout via essaie. `jeton` == NULL -> handoff de flux W5 (rep). */
    OwlHttpExecuteur* x_async;
    OwlHttpJeton*     jeton;
    uint64_t          echeance;
} FluxHandoff;

/* Livre la requete accumulee au handler et ecrit la reponse.
 * Rend le statut emis, ou 0 si l'ecriture a echoue. Pose *out_flux si la
 * reponse etait un flux (mode C) : la connexion se ferme alors toujours,
 * quelle que soit l'intention keep-alive.
 *
 * Si `handoff` est fourni ET que la reponse est un flux, la reponse n'est PAS
 * drainee ici : elle est confiee au reacteur (worker libere, W5). Le drain
 * inline (handoff NULL) reste le chemin des connexions non-reacteur. */
static int sert_requete(OwlTcpConn* conn, const OwlHttpServeurConfig* cfg,
                        Ctx* ctx, bool garder_ouvert, bool* out_flux,
                        FluxHandoff* handoff) {
    OwlHttpRequete* req = ctx->req;
    req->corps     = ctx->corps.d;
    req->corps_len = ctx->corps.len;

    /* Data-plane executeur ASYNC (W7) : deposer le job SANS attendre, puis
     * remettre le jeton au reacteur — le worker se libere aussitot (fin de la
     * retention sur l'executeur mono-thread). Uniquement sur le chemin reacteur
     * (handoff dispo) et hors routes de controle (servies inline). */
    if (cfg->executeur_async && handoff &&
        !owl_http_route_est_controle(req->chemin)) {
        OwlHttpJeton* j = NULL;
        if (owl_http_executeur_depose(cfg->executeur_async, req, &j)
                != OWL_HTTP_EXEC_OK) {
            /* File pleine : 503 produit par le transport SEUL (jobs_pris
             * inchange le prouve, §7). Ecrit inline, connexion fermee. */
            OwlHttpRep* r503 = owl_http_rep_cree_tampon(503, NULL, NULL, 0);
            bool ok = ecrit_reponse(conn, r503, req->est_head,
                                    cfg->limites.timeout_ecriture_ms, false);
            owl_http_rep_libere(r503);
            if (out_flux) *out_flux = true;   /* ferme toujours apres un 503 */
            return ok ? 503 : 0;
        }
        handoff->actif    = 1;
        handoff->rep      = NULL;
        handoff->head     = req->est_head;
        handoff->x_async  = cfg->executeur_async;
        handoff->jeton    = j;
        handoff->echeance = owl_http_executeur_jeton_echeance(j);
        if (out_flux) *out_flux = true;   /* handoff -> conn migree, close */
        return 200;   /* provisoire : le reacteur emettra la vraie reponse */
    }

    OwlHttpRep* opaque = NULL;

    if (cfg->handler_rep) {
        /* Declenchement du mode reponse-fichier : c'est le handler
         * opaque qui choisit son mode, le transport ne sait pas
         * lequel — et n'a pas a le savoir (§5). */
        opaque = cfg->handler_rep(req, cfg->handler_rep_user);
    } else {
        OwlHttpReponse rep;
        memset(&rep, 0, sizeof(rep));
        rep.statut = 404;
        if (cfg->handler) cfg->handler(req, &rep, cfg->handler_user);
        opaque = owl_http_rep_cree_tampon(rep.statut, rep.type_contenu,
                                          rep.corps, rep.corps_len);
    }

    /* Un handler qui ne rend rien n'est pas une raison de laisser la
     * connexion muette. */
    if (!opaque) opaque = owl_http_rep_cree_tampon(500, NULL, NULL, 0);

    bool est_flux = owl_http_rep_est_flux(opaque);
    if (out_flux) *out_flux = est_flux;

    int statut = owl_http_rep_statut(opaque);

    /* Prise en charge reacteur : transferer la reference transport au
     * reacteur au lieu de drainer en bloquant. Le worker est libere aussitot
     * — c'est ce qui supprime la retention du worker sur le flux (W5). */
    if (est_flux && handoff) {
        handoff->actif = 1;
        handoff->rep   = opaque;   /* propriete transferee, PAS de libere ici */
        handoff->head  = req->est_head;
        return statut;
    }

    bool ok = ecrit_reponse(conn, opaque, req->est_head,
                            cfg->limites.timeout_ecriture_ms, garder_ouvert);
    owl_http_rep_libere(opaque);
    return ok ? statut : 0;
}

/* Rejet : aucun corps. Rien de ce que le client a envoye n'est renvoye —
 * c'est la porte d'entree des injections de reponse.
 *
 * Et la connexion se ferme, toujours. Ce n'est pas de la prudence de
 * principe : apres un rejet, le cadrage du flux entrant n'est plus connu,
 * donc la suite de ce flux n'est plus interpretable. Continuer a la lire
 * reviendrait a laisser l'attaquant choisir ou commence la requete
 * suivante — la definition meme du request smuggling. */
static void rejette_et_ferme(OwlTcpConn* conn, const OwlHttpServeurConfig* cfg,
                             int statut) {
    OwlHttpRep* opaque = owl_http_rep_cree_tampon(statut, NULL, NULL, 0);
    ecrit_reponse(conn, opaque, false, cfg->limites.timeout_ecriture_ms, false);
    owl_http_rep_libere(opaque);
}

/* Coeur du service. `handoff` non-NULL active la prise en charge des flux par
 * le reacteur (W5) : au lieu de drainer un flux en bloquant, la conn (ownership
 * transferee vers *out_conn) et la reponse sont remises via *handoff, et la
 * fonction rend sans fermer la conn. Rend le statut de la derniere reponse. */
static int sert_connexion_impl(OwlTcpConn* conn_in,
                               const OwlHttpServeurConfig* cfg,
                               FluxHandoff* handoff, OwlTcpConn* out_conn) {
    OwlTcpConn conn = *conn_in;
    conn_in->fd = OWL_INVALID_SOCKET;   /* la propriete passe ici */

    const OwlHttpLimites* lim = &cfg->limites;

    OwlHttpRequete req;
    memset(&req, 0, sizeof(req));
    snprintf(req.pair, sizeof(req.pair), "%s", conn.pair);

    Ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.req = &req;

    llhttp_settings_t reglages;
    prepare_reglages(&reglages);
    llhttp_t parseur;
    llhttp_init(&parseur, HTTP_REQUEST, &reglages);
    parseur.data = &ctx;

    /* Meme preuve qu'entre deux requetes d'une meme connexion (A1.1a),
     * appliquee a l'ENTREE : un etat non vierge ici signifierait qu'un
     * residu a survecu d'une connexion a une autre — sur un worker
     * reutilise, c'est la version concurrente du vecteur 6.1. Preuve,
     * pas promesse : on relit le bloc plutot que de faire confiance au
     * memset qui precede. */
    if (!etat_vierge(&ctx)) {
        rejette_et_ferme(&conn, cfg, 500);
        owl_tcp_conn_ferme(&conn);
        return 500;
    }

    int    statut      = 0;
    int    rien_recu   = 1;
    int    phase_corps = 0;
    size_t nb_servies  = 0;
    char   tampon[8192];

    /* Le RESIDU du pipelining. Deux requetes peuvent tenir dans un seul
     * read() ; les octets qui suivent la premiere ne sont ni jetes, ni
     * recopies ailleurs — ils restent exactement ou ils sont, et
     * `consomme` dit jusqu'ou llhttp est alle. Toute autre gestion
     * (recopie, concatenation, tampon de reprise) serait une occasion de
     * fabriquer une requete a partir de morceaux, et c'est precisement
     * ce que cherche le vecteur 6.5. */
    size_t dispo    = 0;
    size_t consomme = 0;

    /* Echeances ABSOLUES, pas un timeout par lecture : un client qui
     * envoie un octet toutes les 30 ms ne doit pas pouvoir repousser
     * indefiniment la limite en restant sous le timeout unitaire. C'est
     * toute la difference entre une parade slowloris et une illusion. */
    uint64_t echeance = owl_tcp_server_horloge_ms() +
                        (uint64_t)lim->timeout_inactivite_ms;

    for (;;) {
        if (consomme >= dispo) {
            uint64_t maintenant = owl_tcp_server_horloge_ms();
            if (maintenant >= echeance) break;
            int restant = (int)(echeance - maintenant);

            int n = owl_tcp_conn_lit(&conn, tampon, sizeof(tampon), restant);

            if (n == OWL_TCPS_TIMEOUT || n == OWL_TCPS_ERREUR) break;

            if (n == OWL_TCPS_FERME) {
                llhttp_finish(&parseur);
                /* Une requete EN COURS au moment du EOF est une requete
                 * tronquee, donc un rejet. Entre deux requetes keep-alive,
                 * le meme EOF est la fin normale de la conversation : c'est
                 * `rien_recu`, remis a 1 apres chaque reponse, qui distingue
                 * les deux. */
                if (!rien_recu) {
                    statut = ctx.r.rejet ? ctx.r.rejet : 400;
                    rejette_et_ferme(&conn, cfg, statut);
                }
                break;
            }

            if (rien_recu) {
                rien_recu = 0;
                echeance = owl_tcp_server_horloge_ms() + (uint64_t)lim->timeout_entetes_ms;
            }
            dispo    = (size_t)n;
            consomme = 0;
        }

        llhttp_errno_t e = llhttp_execute(&parseur, tampon + consomme, dispo - consomme);

        if (e == HPE_PAUSED) {
            /* Fin de message. `llhttp_get_error_pos` pointe le premier
             * octet NON consomme : la frontiere exacte entre cette
             * requete et le residu, calculee par le parseur et pas par
             * nous. */
            const char* arret = llhttp_get_error_pos(&parseur);
            consomme = (size_t)(arret - tampon);
            llhttp_resume(&parseur);
        } else if (e != HPE_OK) {
            statut = ctx.r.rejet ? ctx.r.rejet : 400;
            rejette_et_ferme(&conn, cfg, statut);
            break;
        } else {
            consomme = dispo;
        }

        if (ctx.r.envoyer_100) {
            ctx.r.envoyer_100 = 0;
            static const char cent[] = "HTTP/1.1 100 Continue\r\n\r\n";
            if (owl_tcp_conn_ecrit(&conn, cent, sizeof(cent) - 1,
                                   lim->timeout_ecriture_ms) != OWL_TCPS_OK) break;
        }

        if (!ctx.r.message_fini) {
            if (ctx.r.entetes_finis && !phase_corps) {
                phase_corps = 1;
                echeance = owl_tcp_server_horloge_ms() + (uint64_t)lim->timeout_corps_ms;
            }
            continue;
        }

        /* Requete complete. Decider AVANT de repondre si cette reponse
         * est la derniere : le client doit l'apprendre dans les en-tetes
         * de celle-ci, pas par une fermeture inexpliquee. */
        bool encore = ctx.r.garder_connexion != 0
                   && (nb_servies + 1) < lim->requetes_max_par_connexion;

        bool servi_flux = false;
        statut = sert_requete(&conn, cfg, &ctx, encore, &servi_flux, handoff);
        nb_servies++;
        /* Flux pris en charge par le reacteur (W5) : transferer la conn au
         * reacteur et rendre la main SANS drainer ni fermer (le worker est
         * libere). conn.fd invalide -> le owl_tcp_conn_ferme final est un
         * no-op, la propriete a bien migre. */
        if (servi_flux && handoff && handoff->actif) {
            if (out_conn) { *out_conn = conn; conn.fd = OWL_INVALID_SOCKET; }
            break;
        }
        /* Un flux ferme toujours la connexion (Connection: close annonce,
         * longueur inconnue) : on ne recycle pas la connexion apres. */
        if (statut == 0 || !encore || servi_flux) break;

        requete_suivante(&ctx);
        /* Preuve, pas promesse. Si le bloc n'est pas vierge, la requete
         * suivante heriterait de la precedente : on ferme plutot que de
         * servir un melange des deux. */
        if (!etat_vierge(&ctx)) {
            statut = 500;
            rejette_et_ferme(&conn, cfg, 500);
            break;
        }

        rien_recu   = 1;
        phase_corps = 0;
        echeance = owl_tcp_server_horloge_ms() + (uint64_t)lim->timeout_inactivite_ms;
    }

    owl_tcp_conn_ferme(&conn);

    libere_requete(&req);
    buf_libere(&ctx.cible);
    buf_libere(&ctx.champ);
    buf_libere(&ctx.valeur);
    buf_libere(&ctx.corps);

    return statut;
}

int owl_http_serveur_sert_connexion_acceptee(OwlTcpConn* conn_in,
                                             const OwlHttpServeurConfig* cfg) {
    /* Chemin bloquant historique : pas de prise en charge reacteur, le drain
     * de flux (le cas echeant) reste inline. */
    return sert_connexion_impl(conn_in, cfg, NULL, NULL);
}

int owl_http_serveur_sert_une_connexion(OwlTcpListener* l,
                                        const OwlHttpServeurConfig* cfg,
                                        int accept_timeout_ms) {
    OwlTcpConn conn;
    int acc = owl_tcp_listener_accepte(l, accept_timeout_ms, &conn);
    if (acc == OWL_TCPS_TIMEOUT) return 0;
    if (acc != OWL_TCPS_OK)      return -1;
    return owl_http_serveur_sert_connexion_acceptee(&conn, cfg);
}

void owl_http_serveur_boucle(OwlTcpListener* l, const OwlHttpServeurConfig* cfg) {
    owl_tcp_server_installe_arret();
    while (!owl_tcp_server_arret_demande()) {
        owl_http_serveur_sert_une_connexion(l, cfg, 200);
    }
}

/* ================================================================
 * Pool de workers (A1.1b)
 *
 * Le pool est generique (net_tcp_server.c) et ne sait rien de HTTP :
 * cet adaptateur est tout ce qui les relie. Chaque worker sert sa
 * connexion de bout en bout — accumulateurs, parseur, etat de requete
 * vivent sur SA pile. Aucun etat n'est partage entre deux connexions
 * concurrentes, et c'est la seule raison pour laquelle ce module reste
 * sans verrou.
 * ================================================================ */

static void pool_sert_http(OwlTcpConn* conn, void* user) {
    owl_http_serveur_sert_connexion_acceptee(conn, (const OwlHttpServeurConfig*)user);
}

/* Service data-plane du front reacteur (, W5) : setup (lecture +
 * handler) sur le worker, puis, si la reponse est un flux, remise au reacteur
 * (drain POLLOUT) au lieu du drain bloquant. Rend 1 si le flux a ete pris en
 * charge (conn migree vers le reacteur), 0 si servi + ferme sur place. */
static int pool_sert_http_reac(OwlTcpPool* p, OwlTcpConn* conn, void* user) {
    const OwlHttpServeurConfig* cfg = (const OwlHttpServeurConfig*)user;

    FluxHandoff ho;
    memset(&ho, 0, sizeof(ho));
    OwlTcpConn flux_conn;
    memset(&flux_conn, 0, sizeof(flux_conn));
    flux_conn.fd = OWL_INVALID_SOCKET;

    sert_connexion_impl(conn, cfg, &ho, &flux_conn);
    if (!ho.actif) return 0;   /* servi en entier + ferme par l'impl */

    HttpFluxDrain* hd = (HttpFluxDrain*)calloc(1, sizeof(HttpFluxDrain));
    if (!hd) {
        /* Pas de contexte : nettoyer selon le type de handoff. */
        if (ho.jeton) {
            owl_http_executeur_abandonne(ho.x_async, ho.jeton);
        } else {
            owl_http_rep_flux_marque_consommateur_fini(ho.rep);
            owl_http_rep_libere(ho.rep);
        }
        owl_tcp_conn_ferme(&flux_conn);
        return 0;
    }
    hd->head = ho.head;
    hd->pool = p;   /* pour reveiller la boucle depuis le producteur (W7, cf. drain) */

    if (ho.jeton) {
        /* Pending exec async (W7) : le reacteur resoudra le jeton via essaie.
         * Poser le reveil du jeton AVANT la remise (definit_notif declenche le
         * cb sur-le-champ si le job est deja resolu -> pas de reveil perdu).
         * L'echeance arme le timer de synthese 503 cote reacteur. */
        hd->x_async = ho.x_async;
        hd->jeton   = ho.jeton;
        owl_http_executeur_jeton_definit_notif(ho.x_async, ho.jeton,
                                               http_flux_notif, p);
        if (!owl_tcp_pool_reac_prise(p, &flux_conn, hd, ho.echeance)) {
            http_flux_detruit(hd, NULL);   /* abandonne le jeton + free */
            owl_tcp_conn_ferme(&flux_conn);
            return 0;
        }
        return 1;
    }

    /* Flux W5 : le producteur pousse des chunks, le reacteur draine POLLOUT. */
    hd->rep = ho.rep;
    owl_http_rep_flux_definit_notif(ho.rep, http_flux_notif, p);
    if (!owl_tcp_pool_reac_prise(p, &flux_conn, hd, 0)) {
        /* Pool en arret : nettoyer nous-memes (http_flux_detruit relache la
         * reference et stoppe le producteur), fermer la conn restee a nous. */
        http_flux_detruit(hd, NULL);
        owl_tcp_conn_ferme(&flux_conn);
        return 0;
    }
    return 1;
}

/* Classification pour le front reacteur (, W2). Ne lit QUE la ligne
 * de requete (methode SP cible SP version CRLF) dans les octets peekes — le
 * chemin y suffit. La decision ne fait que router (inline vs worker) : le
 * SERVICE reste owl_http_serveur_sert_connexion_acceptee dans les deux cas,
 * donc la frontiere anti-smuggling est identique quelle que soit la classe. */
static int reac_classe_http(const char* octets, size_t len, void* user) {
    (void)user;

    const char* fin_ligne = NULL;
    for (size_t i = 0; i + 1 < len; i++) {
        if (octets[i] == '\r' && octets[i + 1] == '\n') { fin_ligne = octets + i; break; }
    }
    if (!fin_ligne) return OWL_TCPS_CLASSE_INCOMPLET;   /* ligne pas encore complete */

    /* Cible = 2e token de la ligne. */
    const char* p = octets;
    while (p < fin_ligne && *p != ' ') p++;   /* saute la methode */
    while (p < fin_ligne && *p == ' ') p++;
    const char* cible = p;
    while (p < fin_ligne && *p != ' ') p++;
    size_t cible_len = (size_t)(p - cible);
    if (cible_len == 0 || cible_len >= 512) return OWL_TCPS_CLASSE_DATAPLANE;

    char tmp[512];
    memcpy(tmp, cible, cible_len);
    tmp[cible_len] = '\0';

    /* Forme absolue -> chemin seul (l'autorite ne route jamais, cf.
     * separe_cible / RFC 9112 §3.2.2). */
    char autorite[256];
    const char* chemin = separe_cible(tmp, autorite, sizeof(autorite));

    return owl_http_route_est_controle(chemin) ? OWL_TCPS_CLASSE_CONTROLE
                                               : OWL_TCPS_CLASSE_DATAPLANE;
}

OwlTcpPool* owl_http_serveur_pool_demarre(OwlTcpListener* l,
                                          const OwlHttpServeurConfig* cfg,
                                          const OwlTcpPoolConfig* pcfg) {
    /* TLS : le front reacteur ne pompe pas encore le handshake TLS (W3 non
     * integre a la boucle). Les connexions chiffrees restent servies par le
     * pool worker-par-connexion historique, inchange — corpus TLS rejoue tel
     * quel. */
    if (pcfg && pcfg->tls) {
        return owl_tcp_pool_demarre(l, pcfg, pool_sert_http, (void*)cfg);
    }

    /* Cabler l'echeance de pre-classification du reacteur sur le timeout
     * d'en-tetes HTTP : le pool ne connait pas les limites HTTP, c'est ici
     * qu'elles se rejoignent (une seule notion de « delai pour envoyer la
     * ligne de requete »). Copie locale : pcfg est const. */
    OwlTcpPoolConfig pc_local;
    if (pcfg) pc_local = *pcfg; else owl_tcp_pool_config_defaut(&pc_local);
    if (pc_local.reac_classe_timeout_ms <= 0 && cfg->limites.timeout_entetes_ms > 0) {
        pc_local.reac_classe_timeout_ms = cfg->limites.timeout_entetes_ms;
    }

    /* Clair : front reacteur (, W2 ; flux draines dans la boucle,
     * , W5). Accept non borne, classification apres la ligne de
     * requete, controle servi inline, data-plane aux workers — et un flux
     * data-plane est remis au reacteur (drain POLLOUT) au lieu de retenir un
     * worker. `pool_sert_http` reste le service inline du controle. */
    return owl_tcp_pool_reacteur_demarre(l, &pc_local, pool_sert_http, (void*)cfg,
                                         reac_classe_http, NULL,
                                         pool_sert_http_reac,
                                         http_flux_drain, http_flux_detruit,
                                         http_flux_delaimax,
                                         NULL);
}
