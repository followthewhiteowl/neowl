// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/*
 * OWL stdlib — client FTP (clair + FTPS/FTPES sur TLS).
 *
 * Un type reifie cote OWL :
 *   - ftpClient : descripteur d'une connexion FTP (Serveur/Utilisateur/
 *                 MotDePasse/Port/Mode/Connectee/Securisee/Chiffrement +
 *                 handle canal de controle cache dans _handle).
 *
 * Securite (RFC 4217) : un prefixe "ftps://" ouvre un canal de controle
 * chiffre d'emblee (TLS implicite) ; "ftpes://" ouvre en clair puis negocie
 * AUTH TLS avant le login (TLS explicite). Le canal de donnees suit via
 * PBSZ 0 / PROT P. Le wrap TLS reutilise net_tcp_tls (owl_tls_connect /
 * owl_tls_start_tls). Le relachement cible de la verification du certificat
 * passe par un masque OU-binaire de constantes ftpIgnore*.
 *
 * Le canal de controle FTP (RFC 959) est une conversation ligne a ligne
 * au-dessus de net_tcp : chaque reponse serveur est un code a 3 chiffres
 * suivi d'un texte, eventuellement multiligne ("211-...\r\n211 End\r\n").
 * Les commandes de repertoire simples (PWD/CWD/CDUP/MKD/RMD/DELE/RNFR-RNTO/
 * SIZE) n'utilisent QUE ce canal — pas de canal de donnees. Les transferts
 * et le listage (RETR/STOR/LIST/MLSD) ouvrent un 2e canal TCP negocie par
 * PASV. Le coeur protocole (owl_ftp_open/close/list/retr/stor, cf.
 * stdlib_ftp.h) est VM-free : il remonte octets et entrees par callbacks
 * C, testables directement contre un serveur mock. Les natives de cette
 * unite marshalent ces callbacks vers une procedure OWL par
 * vm_call_reentrant.
 *
 * Erreurs : contrat canon soft-error. On pose ErreurDetectee via
 * owl_emit_native_error() et on retourne une valeur neutre. Pas
 * d'exception runtime sauf etat impossible.
 *
 * Mode passif uniquement au runtime : le mode actif est accepte comme
 * valeur de propriete (l'utilisateur peut le lire/ecrire) mais degrade
 * proprement vers le passif au moment du transfert (avertissement stderr).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "stdlib_ftp.h"
/* net_tcp.h tire winsock2.h sous Windows -> collision TokenType (winnt.h vs
 * scanner.h). Meme garde que stdlib_sql.c. */
#define OWL_ERRORS_SKIP_COMPILE_AWARE
#include "owl_errors.h"
#include "object.h"
#include "vm.h"
#include "net_tcp.h"
#include "net_tcp_tls.h"
#include "sentinels.h"
/* SFTP (Lot D) : transport SSH-2, protocole entierement distinct du FTP-sur-TLS.
 * libssh2.h tire winsock2 sous Windows — deja inclus par net_tcp.h ci-dessus,
 * donc pas de collision d'ordre d'inclusion. */
#include "libssh2.h"
#include "libssh2_sftp.h"
#include "sftp_hostkey_keymask.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* Port FTP de controle par defaut (RFC 959). */
#define FTP_PORT_DEFAUT     21
/* Delai d'ouverture du canal de controle, en millisecondes. */
#define FTP_CONNECT_TIMEOUT_MS 10000

/* ============================================================
 * Handle du canal de controle.
 *
 * Stocke la socket TCP + un petit buffer de lecture pour reconstituer
 * les lignes de reponse (net_tcp livre des chunks arbitraires). Alloue
 * au connect, libere au QUIT / a l'echec, pointeur cache dans _handle.
 * ============================================================ */

struct OwlFtpClient {
    /* Canal de controle. `ctrl.tcp` est le transport TCP sous-jacent, en
     * clair OU chiffre : on route send/recv via TLS des que tls_active
     * (meme pattern que sql_postgres_conn : un seul socket, dispatch par
     * flag). Pour FTPES le canal demarre clair (AUTH TLS + login voit du
     * clair) puis bascule ; tls_active traduit CET etat, distinct de
     * secure/encryption qui decrivent l'intention de la connexion. */
    OwlTlsSocket ctrl;
    bool   tls_active; /* handshake TLS effectue -> router via owl_tls_* */
    bool   secure;     /* connexion FTPS ou FTPES (canal chiffre a terme) */
    int    encryption; /* OwlFtpEncryption : 0 clair / 1 FTPS / 2 FTPES */
    int    verify_level; /* OwlTlsVerifyLevel reutilise pour le canal de donnees */
    char   rbuf[4096];
    size_t rlen;       /* octets valides dans rbuf */
    size_t rpos;       /* prochain octet a consommer */
    bool   utf8;       /* le serveur a annonce UTF8 (FEAT) */
    bool   has_mlsd;   /* le serveur a annonce MLSD (FEAT, RFC 3659) */
    char   host[256];  /* hote du canal de controle : reutilise pour le
                        * canal de donnees (robuste face au NAT qui annonce
                        * une IP privee dans la reponse PASV) */

    /* ---- SFTP (transport SSH-2 via libssh2) ----
     * Actif quand is_sftp : le canal FTP clair/TLS ci-dessus est inutilise,
     * tout passe par la session SSH. SFTP n'a pas de notion de "repertoire
     * courant" cote protocole (chemins resolus depuis le home) : on la suit
     * cote client dans cwd pour offrir la semantique CWD attendue. */
    bool             is_sftp;
    bool             host_verified;  /* cle d'hote matchee dans known_hosts */
    OwlTcpSocket     ssh_sock;       /* transport TCP porteur du tunnel SSH */
    LIBSSH2_SESSION* ssh;
    LIBSSH2_SFTP*    sftp;
    char             cwd[1024];      /* repertoire courant SFTP suivi cote client */
};

/* ---- Dispatch canal de controle : TLS si le handshake a eu lieu, sinon
 * le TCP sous-jacent (clair, ou phase pre-AUTH-TLS de FTPES). ---- */
static int ftp_ctrl_recv(OwlFtpClient* c, void* buf, size_t len) {
    return c->tls_active ? owl_tls_recv(&c->ctrl, buf, len)
                         : owl_tcp_recv(&c->ctrl.tcp, buf, len);
}
static int ftp_ctrl_send(OwlFtpClient* c, const void* buf, size_t len) {
    return c->tls_active ? owl_tls_send(&c->ctrl, buf, len)
                         : owl_tcp_send(&c->ctrl.tcp, buf, len);
}

/* ---- Canal de donnees (PASV) : clair ou chiffre (PROT P). Comme le canal
 * de controle, `tls.tcp` est le transport sous-jacent dans les deux cas. ---- */
typedef struct {
    OwlTlsSocket tls;      /* .tcp = transport TCP (clair ou chiffre) */
    bool         secure;   /* handshake TLS effectue sur ce canal */
} FtpDataChan;

static int ftp_data_recv(FtpDataChan* d, void* buf, size_t len) {
    return d->secure ? owl_tls_recv(&d->tls, buf, len)
                     : owl_tcp_recv(&d->tls.tcp, buf, len);
}
static int ftp_data_send(FtpDataChan* d, const void* buf, size_t len) {
    return d->secure ? owl_tls_send(&d->tls, buf, len)
                     : owl_tcp_send(&d->tls.tcp, buf, len);
}
static void ftp_data_close(FtpDataChan* d) {
    if (d->secure) owl_tls_close(&d->tls);
    else           owl_tcp_close(&d->tls.tcp);
}

/* ============================================================
 * Template du type reifie ftpClient.
 * ============================================================ */

ObjInstance* build_ftpClient_template(void) {
    ObjString* name = obj_copy_string("ftpClient", 9);
    vm_push(OBJ_VAL(name));
    ObjInstance* tpl = obj_new_instance(name);
    vm_push(OBJ_VAL(tpl));

    inst_field_set_str (tpl, "Serveur",     "");
    inst_field_set_str (tpl, "Utilisateur", "");
    inst_field_set_str (tpl, "MotDePasse",  "");
    inst_field_set_int (tpl, "Port",        FTP_PORT_DEFAUT);
    inst_field_set_int (tpl, "Mode",        OWL_FTP_MODE_PASSIF);   /* defaut passif */
    inst_field_set_bool(tpl, "Connectee",   false);                 /* canon : Connectée */
    inst_field_set_bool(tpl, "Securisee",   false);                 /* canon : Sécurisée */
    inst_field_set_str (tpl, "Chiffrement", "");                    /* "FTPS"/"FTPES"/"SFTP"/"" */
    inst_field_set_bool(tpl, "VerifieHote", false);                 /* canon : VérifieHôte (cle d'hote SFTP) */

    /* _handle : pointeur opaque vers OwlFtpClient* (cast intptr_t). Field
     * interne, non documente cote user. */
    inst_field_set_int (tpl, "_handle",     0);

    vm_pop();
    vm_pop();
    return tpl;
}

/* ============================================================
 * Enums ftpMode (canal) + ftpModeTransfert (type de transfert).
 * ============================================================ */

static const char* const E_ftpMode_values[] = {
    "ftpModeActif", "ftpModePassif", "ftpModePassifEtendu", NULL
};

const OwlEnum E_ftpMode = {
    .name       = "ftpMode",
    .nameAscii  = "ftpMode",
    .nameEN     = "ftpMode",
    .category   = "FTP",
    .descFR     = "Mode d'ouverture du canal de données d'une connexion FTP (actif, passif, passif étendu).",
    .descEN     = "Data channel mode of an FTP connection (active, passive, extended passive).",
    .values     = E_ftpMode_values,
    .combinable = false
};

static const char* const E_ftpModeTransfert_values[] = {
    "ftpModeASCII", "ftpModeBinaire", NULL
};

const OwlEnum E_ftpModeTransfert = {
    .name       = "ftpModeTransfert",
    .nameAscii  = "ftpModeTransfert",
    .nameEN     = "ftpTransferType",
    .category   = "FTP",
    .descFR     = "Type de transfert d'un fichier FTP : ASCII (conversion des fins de ligne) ou binaire (octets bruts).",
    .descEN     = "FTP file transfer type: ASCII (line-ending conversion) or binary (raw bytes).",
    .values     = E_ftpModeTransfert_values,
    .combinable = false
};

static const char* const E_ftpTypeDate_values[] = {
    "ftpAccès", "ftpCréation", "ftpModification", NULL
};

const OwlEnum E_ftpTypeDate = {
    .name       = "ftpTypeDate",
    .nameAscii  = "ftpTypeDate",
    .nameEN     = "ftpDateType",
    .category   = "FTP",
    .descFR     = "Type de date d'un fichier distant lu par FTPDate/FTPHeure/FTPDateHeure : accès, création ou modification.",
    .descEN     = "Date type of a remote file read by FTPDate/FTPTime/FTPDateTime: access, creation or modification.",
    .values     = E_ftpTypeDate_values,
    .combinable = false
};

static const OwlEnum* const FTP_ENUMS[] = { &E_ftpMode, &E_ftpModeTransfert, &E_ftpTypeDate };

#define C_FTP(nm, val, dfr, den, en) {nm, "FTP", INTEGER_VAL(val), dfr, den, en}
static const OwlConstDef FTP_CONSTS[] = {
    C_FTP("ftpModeActif",        OWL_FTP_MODE_ACTIF,
          "Canal de données en mode actif : le serveur ouvre la connexion vers le client. Peu franchissant en présence de pare-feu/NAT côté client.",
          "Active data channel: the server opens the connection to the client. Rarely traverses client-side firewalls/NAT.", "ftpModeActive"),
    C_FTP("ftpModePassif",       OWL_FTP_MODE_PASSIF,
          "Canal de données en mode passif : le client ouvre la connexion vers le serveur. Mode par défaut, franchit les pare-feux/NAT côté client.",
          "Passive data channel: the client opens the connection to the server. Default mode, traverses client-side firewalls/NAT.", "ftpModePassive"),
    C_FTP("ftpModePassifEtendu", OWL_FTP_MODE_PASSIF_ETENDU,
          "Canal de données en mode passif étendu (EPSV) : variante à port unique, compatible IPv6.",
          "Extended passive data channel (EPSV): single-port variant, IPv6-compatible.", "ftpModeExtendedPassive"),
    C_FTP("ftpModeASCII",        OWL_FTP_TRANSFERT_ASCII,
          "Transfert en mode ASCII : les fins de ligne sont converties entre systèmes différents. Réservé au texte.",
          "ASCII transfer mode: line endings are converted between differing systems. Text only.", "ftpModeASCII"),
    C_FTP("ftpModeBinaire",      OWL_FTP_TRANSFERT_BINAIRE,
          "Transfert en mode binaire : les octets sont transmis inchangés. Mode par défaut, obligatoire pour les fichiers non texte.",
          "Binary transfer mode: bytes are transmitted unchanged. Default mode, required for non-text files.", "ftpModeBinary"),
    C_FTP("ftpAccès",            OWL_FTP_TYPEDATE_ACCES,
          "Date/heure du dernier accès au fichier distant. Selon la disponibilité du serveur.",
          "Date/time of the last access to the remote file. Subject to server availability.", "ftpAccess"),
    C_FTP("ftpCréation",         OWL_FTP_TYPEDATE_CREATION,
          "Date/heure de création du fichier distant. Selon la disponibilité du serveur.",
          "Creation date/time of the remote file. Subject to server availability.", "ftpCreation"),
    C_FTP("ftpModification",     OWL_FTP_TYPEDATE_MODIFICATION,
          "Date/heure de dernière modification du fichier distant. Valeur par défaut.",
          "Last-modification date/time of the remote file. Default value.", "ftpModification"),
    C_FTP("ftpIgnoreCertificatInvalide",   OWL_FTP_IGNORE_CERT_INVALIDE,
          "Relâche la vérification du certificat serveur d'une connexion sécurisée : accepte un certificat qui ne serait pas validé (auto-signé, autorité inconnue). Combinable en OU-binaire avec les autres motifs ftpIgnore*.",
          "Relaxes the server certificate check of a secure connection: accepts a certificate that would not otherwise validate (self-signed, unknown authority). Combinable with the other ftpIgnore* reasons using bitwise OR.",
          "ftpIgnoreInvalidCertificate"),
    C_FTP("ftpIgnoreCertificatExpiré",     OWL_FTP_IGNORE_CERT_EXPIRE,
          "Relâche le contrôle de validité temporelle du certificat serveur : accepte un certificat expiré ou pas encore valide. Combinable en OU-binaire.",
          "Relaxes the validity-period check of the server certificate: accepts an expired or not-yet-valid certificate. Combinable using bitwise OR.",
          "ftpIgnoreExpiredCertificate"),
    C_FTP("ftpIgnoreNomCertificatInvalide", OWL_FTP_IGNORE_CERT_NOM,
          "Relâche la correspondance entre le nom du serveur et le certificat : accepte un certificat dont le nom ne correspond pas à l'hôte contacté. Combinable en OU-binaire.",
          "Relaxes the host-name match of the certificate: accepts a certificate whose name does not match the contacted host. Combinable using bitwise OR.",
          "ftpIgnoreInvalidCertificateName"),
    C_FTP("ftpIgnoreRévocation",           OWL_FTP_IGNORE_REVOCATION,
          "Relâche le contrôle de révocation du certificat serveur : n'échoue pas si l'état de révocation ne peut être vérifié ou indique une révocation. Combinable en OU-binaire.",
          "Relaxes the revocation check of the server certificate: does not fail when the revocation status cannot be verified or reports a revocation. Combinable using bitwise OR.",
          "ftpIgnoreRevocation"),
    C_FTP("ftpIgnoreHôteInconnu",          OWL_FTP_IGNORE_HOTE_INCONNU,
          "Sur une connexion SFTP (préfixe \"sftp://\") : accepte un serveur dont la clé d'hôte est absente du fichier known_hosts, au lieu d'échouer. Un serveur connu présentant une clé différente reste refusé (protection contre l'interception). Combinable en OU-binaire.",
          "On an SFTP connection (\"sftp://\" prefix): accepts a server whose host key is absent from the known_hosts file, instead of failing. A known server presenting a different key is still rejected (protection against interception). Combinable using bitwise OR.",
          "ftpIgnoreUnknownHost"),
};
#undef C_FTP

/* ============================================================
 * Helpers — extraction ftpClient + handle.
 * ============================================================ */

static ObjInstance* resolve_ftp_cnx(Value v, const char* fn) {
    if (!IS_INSTANCE(v)) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "ftpClient",
                              "actual", "un autre type", NULL);
        return NULL;
    }
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL ||
        strcmp(inst->structName->chars, "ftpClient") != 0) {
        owl_emit_native_error(E_NATIVE_ARG_TYPE_MISMATCH, fn,
                              "native", fn, "arg", "1",
                              "expected", "ftpClient",
                              "actual", inst->structName ? inst->structName->chars : "<anonyme>",
                              NULL);
        return NULL;
    }
    return inst;
}

static OwlFtpClient* cnx_get_handle(ObjInstance* inst) {
    Value v;
    ObjString* k = obj_copy_string("_handle", 7);
    if (!table_get(&inst->fields, k, &v)) return NULL;
    if (!IS_INTEGER(v)) return NULL;
    return (OwlFtpClient*)(intptr_t)AS_INTEGER(v);
}

static void cnx_set_handle(ObjInstance* inst, OwlFtpClient* c) {
    inst_field_set_int(inst, "_handle", (int64_t)(intptr_t)c);
}

static const char* arg_str(Value v) {
    return IS_STRING(v) ? AS_CSTRING(v) : NULL;
}

/* ============================================================
 * Canal de controle FTP — lecture de reponses ligne a ligne.
 * ============================================================ */

/* Lit une ligne complete (jusqu'au \n) du canal de controle dans out.
 * Le \r\n final est retire. Retourne true si une ligne a ete lue. */
static bool ftp_read_line(OwlFtpClient* c, char* out, size_t out_size) {
    size_t n = 0;
    for (;;) {
        if (c->rpos >= c->rlen) {
            int got = ftp_ctrl_recv(c, c->rbuf, sizeof(c->rbuf));
            if (got <= 0) return false;    /* fermeture ou erreur */
            c->rlen = (size_t)got;
            c->rpos = 0;
        }
        char ch = c->rbuf[c->rpos++];
        if (ch == '\n') {
            if (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return true;
        }
        if (n + 1 < out_size) out[n++] = ch;
    }
}

/* Lit une reponse complete (mono ou multiligne) et renvoie son code a 3
 * chiffres. Une reponse multiligne commence par "NNN-" et se termine par
 * une ligne "NNN " (espace apres le code). Retourne -1 en cas d'echec. */
static int ftp_read_reply(OwlFtpClient* c, char* text, size_t text_size) {
    char line[1024];
    if (!ftp_read_line(c, line, sizeof(line))) return -1;
    if (strlen(line) < 3) return -1;

    char code[4] = { line[0], line[1], line[2], '\0' };
    int codeN = atoi(code);
    size_t tlen = 0;
    if (text && text_size) {
        int w = snprintf(text, text_size, "%s", line + (strlen(line) > 4 ? 4 : 3));
        tlen = (w > 0) ? (size_t)w : 0;
        if (tlen >= text_size) tlen = text_size - 1;
    }

    /* Reponse multiligne : "NNN-..." -> lire jusqu'a la ligne "NNN ". Les
     * lignes de continuation sont ACCUMULEES dans text (indispensable a
     * FEAT : les capacites — MLSD, UTF8 — vivent sur ces lignes). */
    if (line[3] == '-') {
        for (;;) {
            if (!ftp_read_line(c, line, sizeof(line))) return -1;
            bool terminal = (strlen(line) >= 4 &&
                             strncmp(line, code, 3) == 0 && line[3] == ' ');
            if (text && text_size && tlen + 1 < text_size) {
                int w = snprintf(text + tlen, text_size - tlen, "\n%s", line);
                if (w > 0) tlen += (size_t)w;
                if (tlen >= text_size) tlen = text_size - 1;
            }
            if (terminal) break;
        }
    }
    return codeN;
}

/* Envoie une commande + CRLF sur le canal de controle. */
static bool ftp_send_cmd(OwlFtpClient* c, const char* fmt, ...) {
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n > sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
    buf[n++] = '\r';
    buf[n++] = '\0';
    /* remplace le '\0' provisoire par '\n' pour l'envoi (n compte le \r). */
    buf[n - 1] = '\n';
    return ftp_ctrl_send(c, buf, (size_t)n) == n;
}

/* Envoie une commande simple et verifie que le code de reponse est dans la
 * plage 2xx (succes) ou, si expect2 != 0, egal a expect2. Retourne le code
 * lu (ou -1). text recoit le texte de la reponse si non NULL. */
static int ftp_command(OwlFtpClient* c, char* text, size_t text_size,
                       const char* fmt, ...) {
    char buf[1200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (!ftp_send_cmd(c, "%s", buf)) return -1;
    return ftp_read_reply(c, text, text_size);
}

/* Demonte la session SFTP (canal SFTP + session SSH + socket porteuse).
 * Idempotent sur les sous-handles NULL. Ne libere PAS c. */
static void sftp_session_teardown(OwlFtpClient* c) {
    if (c->sftp) { libssh2_sftp_shutdown(c->sftp); c->sftp = NULL; }
    if (c->ssh) {
        libssh2_session_disconnect(c->ssh, "owl sftp bye");
        libssh2_session_free(c->ssh);
        c->ssh = NULL;
    }
    owl_tcp_close(&c->ssh_sock);
}

/* Ferme et libere le handle (sans QUIT — usage interne sur echec). */
static void ftp_client_free(OwlFtpClient* c) {
    if (c == NULL) return;
    if (c->is_sftp) {
        sftp_session_teardown(c);
    } else if (c->tls_active) {
        owl_tls_close(&c->ctrl);
    } else {
        owl_tcp_close(&c->ctrl.tcp);
    }
    free(c);
}

/* ============================================================
 * Coeur protocole VM-free — ouverture, canal de donnees, transferts,
 * listage. Aucune dependance VM/GC : les octets et entrees remontent
 * par callbacks C. Teste directement contre un serveur mock.
 * ============================================================ */

OwlFtpClient* owl_ftp_open(const char* host, int port,
                           const char* user, const char* pass,
                           OwlFtpEncryption enc, int verify_level,
                           char* err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (host == NULL) host = "";
    if (user == NULL) user = "";
    if (pass == NULL) pass = "";

    OwlFtpClient* c = (OwlFtpClient*)calloc(1, sizeof(OwlFtpClient));
    if (c == NULL) {
        if (err) snprintf(err, errlen, "mémoire insuffisante");
        return NULL;
    }
    snprintf(c->host, sizeof(c->host), "%s", host);
    c->encryption   = enc;
    c->secure       = (enc != OWL_FTP_ENC_CLEAR);
    c->verify_level = verify_level;

    owl_tcp_global_init();
    owl_tls_init(&c->ctrl);

    if (enc == OWL_FTP_ENC_FTPS) {
        /* Implicite : TLS des la connexion, aucun echange en clair. */
        if (!owl_tls_connect(&c->ctrl, host, port, verify_level,
                             FTP_CONNECT_TIMEOUT_MS, NULL)) {
            if (err) snprintf(err, errlen, "handshake TLS échoué : %s",
                              owl_tls_last_error(&c->ctrl));
            ftp_client_free(c);
            return NULL;
        }
        c->tls_active = true;
    } else {
        /* Clair ou FTPES : on connecte le TCP sous-jacent en clair. */
        if (!owl_tcp_connect(&c->ctrl.tcp, host, port, FTP_CONNECT_TIMEOUT_MS)) {
            if (err) snprintf(err, errlen, "%s", owl_tcp_last_error(&c->ctrl.tcp));
            ftp_client_free(c);
            return NULL;
        }
    }

    /* Banniere d'accueil : 220. */
    if (ftp_read_reply(c, NULL, 0) != 220) {
        if (err) snprintf(err, errlen, "bannière d'accueil inattendue");
        ftp_client_free(c);
        return NULL;
    }

    /* FTPES : negocie AUTH TLS avant tout echange d'identifiants, puis
     * promeut le canal de controle deja ouvert en TLS. */
    if (enc == OWL_FTP_ENC_FTPES) {
        int a = ftp_command(c, NULL, 0, "AUTH TLS");
        if (a != 234 && a != 334) {
            if (err) snprintf(err, errlen, "AUTH TLS refusé (code %d)", a);
            ftp_client_free(c);
            return NULL;
        }
        if (!owl_tls_start_tls(&c->ctrl, host, verify_level, NULL)) {
            if (err) snprintf(err, errlen, "handshake TLS (AUTH TLS) échoué : %s",
                              owl_tls_last_error(&c->ctrl));
            ftp_client_free(c);
            return NULL;
        }
        c->tls_active = true;
    }

    /* USER / PASS. 331 = mot de passe requis, 230 = connecte sans. */
    int code = ftp_command(c, NULL, 0, "USER %s", user[0] ? user : "anonymous");
    if (code == 331) code = ftp_command(c, NULL, 0, "PASS %s", pass);
    if (code != 230 && code != 202) {
        if (err) snprintf(err, errlen, "authentification refusée");
        ftp_client_free(c);
        return NULL;
    }

    /* Connexion securisee : protege le canal de donnees. PBSZ 0 (taille de
     * bloc nulle, obligatoire avant PROT sur TLS) puis PROT P (donnees
     * chiffrees). Best-effort : un serveur qui refuse laisse la connexion
     * utilisable, le canal de donnees suivra l'etat du canal de controle. */
    if (c->secure) {
        ftp_command(c, NULL, 0, "PBSZ 0");
        ftp_command(c, NULL, 0, "PROT P");
    }

    /* FEAT : detecte MLSD (RFC 3659) et UTF8. */
    char feat[2048];
    if (ftp_command(c, feat, sizeof(feat), "FEAT") == 211) {
        if (strstr(feat, "MLSD") || strstr(feat, "mlsd")) c->has_mlsd = true;
        if (strstr(feat, "UTF8") || strstr(feat, "utf8")) {
            ftp_command(c, NULL, 0, "OPTS UTF8 ON");
            c->utf8 = true;
        }
    }
    return c;
}

OwlFtpEncryption owl_ftp_encryption(const OwlFtpClient* c) {
    return c ? (OwlFtpEncryption)c->encryption : OWL_FTP_ENC_CLEAR;
}

/* ---- Prefixe de securite + libelle de chiffrement ---- */

OwlFtpEncryption owl_ftp_parse_prefix(const char* serveur, char* hostout, size_t hostsz) {
    if (serveur == NULL) serveur = "";
    OwlFtpEncryption enc = OWL_FTP_ENC_CLEAR;
    const char* host = serveur;
    /* Prefixes casse-insensibles. ftpes:// AVANT ftps:// n'est pas ambigu
     * (prefixes distincts), mais on teste le plus long d'abord par surete. */
    if (strncasecmp(serveur, "ftpes://", 8) == 0) {
        enc = OWL_FTP_ENC_FTPES; host = serveur + 8;
    } else if (strncasecmp(serveur, "sftp://", 7) == 0) {
        enc = OWL_FTP_ENC_SFTP;  host = serveur + 7;
    } else if (strncasecmp(serveur, "ftps://", 7) == 0) {
        enc = OWL_FTP_ENC_FTPS;  host = serveur + 7;
    } else if (strncasecmp(serveur, "ftp://", 6) == 0) {
        enc = OWL_FTP_ENC_CLEAR; host = serveur + 6;
    }
    if (hostout && hostsz) snprintf(hostout, hostsz, "%s", host);
    return enc;
}

const char* owl_ftp_encryption_label(OwlFtpEncryption enc) {
    switch (enc) {
        case OWL_FTP_ENC_FTPS:  return "FTPS";
        case OWL_FTP_ENC_FTPES: return "FTPES";
        case OWL_FTP_ENC_SFTP:  return "SFTP";
        default:                return "";
    }
}

void owl_ftp_close(OwlFtpClient* c) {
    if (c == NULL) return;
    if (!c->is_sftp) ftp_command(c, NULL, 0, "QUIT");  /* SFTP : pas de QUIT, teardown SSH */
    ftp_client_free(c);
}

bool owl_ftp_has_mlsd(const OwlFtpClient* c) {
    return c != NULL && c->has_mlsd;
}

/* Ouvre le 2e canal TCP (donnees) via PASV. On se connecte au meme hote
 * que le canal de controle en ne reprenant que le PORT annonce : c'est le
 * comportement robuste face au NAT qui inscrit une IP privee dans la
 * reponse 227. */
static bool ftp_pasv_open(OwlFtpClient* c, FtpDataChan* d,
                          char* err, size_t errlen) {
    char text[256];
    int code = ftp_command(c, text, sizeof(text), "PASV");
    if (code != 227) {
        if (err) snprintf(err, errlen, "PASV refusé (code %d)", code);
        return false;
    }
    int h1, h2, h3, h4, p1, p2;
    const char* open = strchr(text, '(');
    if (open == NULL ||
        sscanf(open + 1, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        if (sscanf(text, "%*[^0-9]%d,%d,%d,%d,%d,%d",
                   &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
            if (err) snprintf(err, errlen, "réponse PASV illisible");
            return false;
        }
    }
    int dport = (p1 & 0xFF) * 256 + (p2 & 0xFF);
    const char* dhost = c->host[0] ? c->host : "127.0.0.1";
    memset(d, 0, sizeof(*d));
    owl_tls_init(&d->tls);
    if (!owl_tcp_connect(&d->tls.tcp, dhost, dport, FTP_CONNECT_TIMEOUT_MS)) {
        if (err) snprintf(err, errlen, "canal de données injoignable (%s:%d)",
                          dhost, dport);
        return false;
    }
    /* PROT P : canal de donnees chiffre quand le canal de controle l'est.
     * On promeut le TCP fraichement connecte en TLS (handshake frais — sans
     * reprise de la session du canal de controle, non exigee par notre pair).
     * A appeler APRES la reponse 150 de la commande de transfert (voir plus
     * bas) : le serveur ne demarre son handshake data qu'a ce moment. */
    return true;
}

/* Promeut le canal de donnees en TLS (apres 150). No-op en clair. */
static bool ftp_data_secure(OwlFtpClient* c, FtpDataChan* d,
                            char* err, size_t errlen) {
    if (!c->secure) return true;
    if (!owl_tls_start_tls(&d->tls, c->host, c->verify_level, NULL)) {
        if (err) snprintf(err, errlen, "handshake TLS du canal de données échoué : %s",
                          owl_tls_last_error(&d->tls));
        return false;
    }
    d->secure = true;
    return true;
}

/* ============================================================
 * SFTP (Lot D) — transport SSH-2 via libssh2.
 *
 * SFTP n'est PAS du FTP-sur-TLS : c'est un sous-systeme de SSH-2, un
 * protocole entierement distinct (une session SSH, un canal, des requetes
 * binaires typees). Quand le nom de serveur est prefixe "sftp://", la
 * connexion ouvre un tunnel SSH (owl_sftp_open) et les operations FTP*
 * routent vers libssh2_sftp_* au lieu du canal de controle FTP. Le meme
 * objet reifie ftpClient porte les deux mondes, discrimines par is_sftp.
 *
 * VM-free comme le coeur FTP : ces fonctions ne connaissent ni la VM ni le
 * GC ; elles remontent octets et entrees par les memes callbacks C
 * (OwlFtpSinkFn / OwlFtpEntryFn / OwlFtpProgressFn), donc les adaptateurs de
 * marshalling des natives sont reutilises tels quels.
 * ============================================================ */

bool owl_sftp_uses_key_auth(const char* privkey) {
    return privkey != NULL && privkey[0] != '\0';
}

typedef enum {
    SFTP_HOST_MATCH = 0,   /* cle d'hote presente dans known_hosts et identique */
    SFTP_HOST_UNKNOWN = 1, /* hote absent de known_hosts */
    SFTP_HOST_MISMATCH = 2,/* hote connu mais cle differente (interception possible) */
    SFTP_HOST_ERROR = 3,   /* API known-hosts / cle d'hote indisponible */
} SftpHostStatus;

/* Confronte la cle d'hote presentee par le serveur au fichier known_hosts de
 * l'utilisateur (~/.ssh/known_hosts, format OpenSSH). */
static SftpHostStatus sftp_check_host(LIBSSH2_SESSION* ssh, const char* host, int port) {
    LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(ssh);
    if (!kh) return SFTP_HOST_ERROR;

    char khpath[1024];
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (!home || !home[0]) home = getenv("USERPROFILE");
#endif
    if (home && home[0]) {
        snprintf(khpath, sizeof(khpath), "%s/.ssh/known_hosts", home);
        libssh2_knownhost_readfile(kh, khpath, LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }

    size_t klen = 0;
    int ktype = 0;
    const char* key = libssh2_session_hostkey(ssh, &klen, &ktype);
    if (!key) { libssh2_knownhost_free(kh); return SFTP_HOST_ERROR; }

    /* Scoper le check par TYPE de cle negociee : sans le bit KEY_, libssh2
     * compare la cle contre toutes les entrees de l'hote tous types confondus,
     * et un hote connu sous un autre type (rsa vs ed25519...) remonte un faux
     * MISMATCH. Avec le bon bit : entree meme-type-cle-differente -> MISMATCH
     * (vrai MITM), aucune entree du bon type -> NOTFOUND (inconnu, relachable),
     * comme OpenSSH. */
    struct libssh2_knownhost* found = NULL;
    int check = libssh2_knownhost_checkp(kh, host, port, key, klen,
        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW
            | owl_sftp_knownhost_keymask(ktype),
        &found);
    libssh2_knownhost_free(kh);

    switch (check) {
        case LIBSSH2_KNOWNHOST_CHECK_MATCH:    return SFTP_HOST_MATCH;
        case LIBSSH2_KNOWNHOST_CHECK_MISMATCH: return SFTP_HOST_MISMATCH;
        case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: return SFTP_HOST_UNKNOWN;
        default:                               return SFTP_HOST_ERROR;
    }
}

/* Init globale libssh2 une seule fois (refcount interne, teardown au process
 * exit — pas de libssh2_exit explicite, comme les autres briques vendorees). */
static bool g_libssh2_inited = false;

OwlFtpClient* owl_sftp_open(const char* host, int port,
                            const char* user, const char* pass,
                            const char* privkey, const char* privkey_pass,
                            bool allow_unknown_host, bool* out_host_verified,
                            char* err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (out_host_verified) *out_host_verified = false;
    if (host == NULL) host = "";
    if (user == NULL) user = "";
    if (pass == NULL) pass = "";

    OwlFtpClient* c = (OwlFtpClient*)calloc(1, sizeof(OwlFtpClient));
    if (c == NULL) {
        if (err) snprintf(err, errlen, "mémoire insuffisante");
        return NULL;
    }
    c->is_sftp    = true;
    c->secure     = true;
    c->encryption = OWL_FTP_ENC_SFTP;
    snprintf(c->host, sizeof(c->host), "%s", host);

    owl_tcp_global_init();
    owl_tcp_init(&c->ssh_sock);
    if (!owl_tcp_connect(&c->ssh_sock, host, port, FTP_CONNECT_TIMEOUT_MS)) {
        if (err) snprintf(err, errlen, "%s", owl_tcp_last_error(&c->ssh_sock));
        ftp_client_free(c);
        return NULL;
    }

    if (!g_libssh2_inited) {
        if (libssh2_init(0) != 0) {
            if (err) snprintf(err, errlen, "initialisation SSH échouée");
            ftp_client_free(c);
            return NULL;
        }
        g_libssh2_inited = true;
    }

    c->ssh = libssh2_session_init();
    if (!c->ssh) {
        if (err) snprintf(err, errlen, "session SSH indisponible");
        ftp_client_free(c);
        return NULL;
    }
    libssh2_session_set_blocking(c->ssh, 1);

    if (libssh2_session_handshake(c->ssh, c->ssh_sock.fd) != 0) {
        char* msg = NULL;
        libssh2_session_last_error(c->ssh, &msg, NULL, 0);
        if (err) snprintf(err, errlen, "handshake SSH échoué : %s", msg ? msg : "");
        ftp_client_free(c);
        return NULL;
    }

    /* Verification de la cle d'hote. Un CONFLIT (hote connu, cle differente)
     * echoue toujours, meme sous ftpIgnoreHoteInconnu : c'est la signature
     * d'une interception, pas d'un simple premier contact. */
    SftpHostStatus hs = sftp_check_host(c->ssh, host, port);
    if (hs == SFTP_HOST_MISMATCH) {
        if (err) snprintf(err, errlen,
            "clé d'hôte SSH en conflit avec known_hosts (interception possible)");
        ftp_client_free(c);
        return NULL;
    }
    if (hs != SFTP_HOST_MATCH && !allow_unknown_host) {
        if (err) snprintf(err, errlen,
            "hôte SSH inconnu (absent de known_hosts) — ftpIgnoreHôteInconnu pour l'accepter");
        ftp_client_free(c);
        return NULL;
    }
    c->host_verified = (hs == SFTP_HOST_MATCH);
    if (out_host_verified) *out_host_verified = c->host_verified;

    /* Authentification : cle privee OpenSSH si fournie, sinon mot de passe. */
    int rc;
    if (owl_sftp_uses_key_auth(privkey)) {
        rc = libssh2_userauth_publickey_fromfile(
                 c->ssh, user, NULL, privkey, privkey_pass ? privkey_pass : "");
    } else {
        rc = libssh2_userauth_password(c->ssh, user, pass);
    }
    if (rc != 0) {
        if (err) snprintf(err, errlen, "authentification SSH refusée");
        ftp_client_free(c);
        return NULL;
    }

    c->sftp = libssh2_sftp_init(c->ssh);
    if (!c->sftp) {
        if (err) snprintf(err, errlen, "sous-système SFTP indisponible");
        ftp_client_free(c);
        return NULL;
    }

    /* Repertoire courant initial = realpath(".") (home de l'utilisateur).
     * SFTP n'a pas de CWD serveur : on le suit cote client. */
    char rp[1024];
    int n = libssh2_sftp_realpath(c->sftp, ".", rp, sizeof(rp) - 1);
    if (n > 0) { rp[n] = '\0'; snprintf(c->cwd, sizeof(c->cwd), "%s", rp); }
    else       { snprintf(c->cwd, sizeof(c->cwd), "."); }

    return c;
}

/* Resout un chemin relatif contre le repertoire courant suivi cote client.
 * Chemin absolu (commence par '/') ou cwd trivial -> tel quel. */
static void sftp_resolve(OwlFtpClient* c, const char* path, char* out, size_t outsz) {
    if (path == NULL) path = "";
    if (path[0] == '/' || c->cwd[0] == '\0' || strcmp(c->cwd, ".") == 0) {
        snprintf(out, outsz, "%s", path);
    } else if (path[0] == '\0') {
        snprintf(out, outsz, "%s", c->cwd);
    } else {
        snprintf(out, outsz, "%s/%s", c->cwd, path);
    }
}

static bool sftp_stat(OwlFtpClient* c, const char* path, LIBSSH2_SFTP_ATTRIBUTES* attrs) {
    char full[1200];
    sftp_resolve(c, path, full, sizeof(full));
    memset(attrs, 0, sizeof(*attrs));
    return libssh2_sftp_stat(c->sftp, full, attrs) == 0;
}

/* Formate un mtime/atime SFTP (secondes epoch) en date AAAAMMJJ + heure
 * HHMMSS (UTC), memes champs que le listage MLSD cote FTP clair. */
static void sftp_format_epoch(unsigned long epoch, char* date, size_t dsz,
                              char* heure, size_t hsz) {
    time_t t = (time_t)epoch;
    struct tm g;
#ifdef _WIN32
    struct tm* gp = gmtime(&t);
    if (gp) g = *gp; else memset(&g, 0, sizeof(g));
#else
    gmtime_r(&t, &g);
#endif
    snprintf(date, dsz, "%04d%02d%02d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    snprintf(heure, hsz, "%02d%02d%02d", g.tm_hour, g.tm_min, g.tm_sec);
}

static bool sftp_retr(OwlFtpClient* c, const char* remote,
                      OwlFtpSinkFn sink, void* sctx,
                      OwlFtpProgressFn prog, void* pctx,
                      char* err, size_t errlen) {
    char full[1200];
    sftp_resolve(c, remote, full, sizeof(full));

    int64_t total = -1;
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    if (libssh2_sftp_stat(c->sftp, full, &a) == 0 && (a.flags & LIBSSH2_SFTP_ATTR_SIZE)) {
        total = (int64_t)a.filesize;
    }

    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(c->sftp, full, LIBSSH2_FXF_READ, 0);
    if (!h) {
        if (err) snprintf(err, errlen, "ouverture SFTP en lecture refusée");
        return false;
    }

    bool ok = true;
    int64_t done = 0;
    char buf[32768];
    for (;;) {
        long long n = libssh2_sftp_read(h, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;                       /* EOF */
        if (sink && !sink(sctx, buf, (size_t)n)) { ok = false; break; }
        done += n;
        if (prog && !prog(pctx, total >= 0 ? total : done, done)) { ok = false; break; }
    }
    libssh2_sftp_close(h);
    if (!ok && err && err[0] == '\0') snprintf(err, errlen, "transfert SFTP (lecture) incomplet");
    return ok;
}

static bool sftp_stor(OwlFtpClient* c, const char* remote,
                      const char* data, size_t len,
                      OwlFtpProgressFn prog, void* pctx,
                      char* err, size_t errlen) {
    char full[1200];
    sftp_resolve(c, remote, full, sizeof(full));

    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(c->sftp, full,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!h) {
        if (err) snprintf(err, errlen, "ouverture SFTP en écriture refusée");
        return false;
    }

    bool ok = true;
    size_t sent = 0;
    while (sent < len) {
        long long n = libssh2_sftp_write(h, data + sent, len - sent);
        if (n < 0) { ok = false; break; }
        if (n == 0) continue;                    /* blocage : rien ecrit, on retente */
        sent += (size_t)n;
        if (prog && !prog(pctx, (int64_t)len, (int64_t)sent)) { ok = false; break; }
    }
    libssh2_sftp_close(h);
    if (!ok && err && err[0] == '\0') snprintf(err, errlen, "transfert SFTP (écriture) incomplet");
    return ok;
}

static int sftp_list(OwlFtpClient* c, const char* path, OwlFtpEntryFn cb, void* ctx) {
    char full[1200];
    sftp_resolve(c, path, full, sizeof(full));

    LIBSSH2_SFTP_HANDLE* d = libssh2_sftp_opendir(c->sftp, full[0] ? full : ".");
    if (!d) return -1;

    int count = 0;
    char name[512];
    LIBSSH2_SFTP_ATTRIBUTES a;
    for (;;) {
        memset(&a, 0, sizeof(a));
        int n = libssh2_sftp_readdir(d, name, sizeof(name) - 1, &a);
        if (n <= 0) break;
        name[n] = '\0';
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        bool is_dir = (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                      LIBSSH2_SFTP_S_ISDIR(a.permissions);
        const char* attribut = is_dir ? "dir" : "file";
        int64_t size = (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (int64_t)a.filesize : -1;
        char date[16] = "", heure[16] = "";
        if (a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
            sftp_format_epoch(a.mtime, date, sizeof(date), heure, sizeof(heure));
        }

        count++;
        if (cb && !cb(ctx, name, size, attribut, date, heure)) break;
    }
    libssh2_sftp_closedir(d);
    return count;
}

/* ---- Operations de repertoire / metadonnees SFTP a chemin explicite ---- */

static bool sftp_unlink(OwlFtpClient* c, const char* path) {
    char full[1200]; sftp_resolve(c, path, full, sizeof(full));
    return libssh2_sftp_unlink(c->sftp, full) == 0;
}
static bool sftp_rename(OwlFtpClient* c, const char* src, const char* dst) {
    char fs[1200], fd[1200];
    sftp_resolve(c, src, fs, sizeof(fs));
    sftp_resolve(c, dst, fd, sizeof(fd));
    return libssh2_sftp_rename(c->sftp, fs, fd) == 0;
}
static bool sftp_mkdir(OwlFtpClient* c, const char* path) {
    char full[1200]; sftp_resolve(c, path, full, sizeof(full));
    return libssh2_sftp_mkdir(c->sftp, full, 0755) == 0;
}
static bool sftp_rmdir(OwlFtpClient* c, const char* path) {
    char full[1200]; sftp_resolve(c, path, full, sizeof(full));
    return libssh2_sftp_rmdir(c->sftp, full) == 0;
}
static bool sftp_exists(OwlFtpClient* c, const char* path) {
    LIBSSH2_SFTP_ATTRIBUTES a;
    return sftp_stat(c, path, &a);
}

/* Change le repertoire courant suivi cote client : resout, normalise via
 * realpath, exige un repertoire. ".." remonte depuis le cwd courant. */
static bool sftp_chdir(OwlFtpClient* c, const char* path) {
    char target[1200];
    if (path && strcmp(path, "..") == 0) {
        snprintf(target, sizeof(target), "%s/..", c->cwd);
    } else {
        sftp_resolve(c, path, target, sizeof(target));
    }
    char rp[1024];
    int n = libssh2_sftp_realpath(c->sftp, target, rp, sizeof(rp) - 1);
    if (n <= 0) return false;
    rp[n] = '\0';
    LIBSSH2_SFTP_ATTRIBUTES a;
    memset(&a, 0, sizeof(a));
    if (libssh2_sftp_stat(c->sftp, rp, &a) != 0) return false;
    if (!((a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(a.permissions)))
        return false;
    snprintf(c->cwd, sizeof(c->cwd), "%s", rp);
    return true;
}

/* Attributs portables d'un fichier distant SFTP, memes conventions que le FTP
 * clair : "R" (lecture seule : aucune permission d'ecriture), "D" (repertoire),
 * ordre R avant D. Chaine vide si aucun ne s'applique. */
static void sftp_attr(OwlFtpClient* c, const char* path, char* out, size_t outsz) {
    out[0] = '\0';
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (!sftp_stat(c, path, &a) || !(a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)) return;
    size_t k = 0;
    bool writable = (a.permissions & (LIBSSH2_SFTP_S_IWUSR |
                                      LIBSSH2_SFTP_S_IWGRP | LIBSSH2_SFTP_S_IWOTH)) != 0;
    if (!writable && k + 1 < outsz) out[k++] = 'R';
    if (LIBSSH2_SFTP_S_ISDIR(a.permissions) && k + 1 < outsz) out[k++] = 'D';
    if (outsz > 0) out[k] = '\0';
}

bool owl_ftp_retr(OwlFtpClient* c, const char* remote, bool ascii,
                  OwlFtpSinkFn sink, void* sctx,
                  OwlFtpProgressFn prog, void* pctx,
                  char* err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (c == NULL || remote == NULL) return false;
    if (c->is_sftp) {
        /* SFTP : transfert binaire pur (pas de conversion ASCII cote SSH). */
        (void)ascii;
        return sftp_retr(c, remote, sink, sctx, prog, pctx, err, errlen);
    }
    ftp_command(c, NULL, 0, "TYPE %s", ascii ? "A" : "I");

    /* Taille annoncee (best-effort) pour renseigner la progression. */
    int64_t total = -1;
    char sz[128];
    if (ftp_command(c, sz, sizeof(sz), "SIZE %s", remote) == 213) {
        total = (int64_t)strtoll(sz, NULL, 10);
    }

    FtpDataChan data;
    if (!ftp_pasv_open(c, &data, err, errlen)) return false;

    int code = ftp_command(c, NULL, 0, "RETR %s", remote);
    if (code != 150 && code != 125) {
        ftp_data_close(&data);
        if (err) snprintf(err, errlen, "RETR refusé (code %d)", code);
        return false;
    }
    if (!ftp_data_secure(c, &data, err, errlen)) { ftp_data_close(&data); return false; }

    bool ok = true;
    int64_t done = 0;
    char buf[8192];
    for (;;) {
        int n = ftp_data_recv(&data, buf, sizeof(buf));
        if (n < 0) { ok = false; break; }
        if (n == 0) break;                       /* EOF : fin du transfert */
        if (sink && !sink(sctx, buf, (size_t)n)) { ok = false; break; }
        done += n;
        if (prog && !prog(pctx, total >= 0 ? total : done, done)) { ok = false; break; }
    }
    ftp_data_close(&data);

    int fin = ftp_read_reply(c, NULL, 0);       /* 226 = transfert complet */
    if (fin != 226 && fin != 250) ok = false;
    if (!ok && err && err[0] == '\0') snprintf(err, errlen, "transfert RETR incomplet");
    return ok;
}

bool owl_ftp_stor(OwlFtpClient* c, const char* remote, bool ascii,
                  const char* data, size_t len,
                  OwlFtpProgressFn prog, void* pctx,
                  char* err, size_t errlen) {
    if (err && errlen) err[0] = '\0';
    if (c == NULL || remote == NULL) return false;
    if (c->is_sftp) {
        (void)ascii;
        return sftp_stor(c, remote, data, len, prog, pctx, err, errlen);
    }
    ftp_command(c, NULL, 0, "TYPE %s", ascii ? "A" : "I");

    FtpDataChan dsock;
    if (!ftp_pasv_open(c, &dsock, err, errlen)) return false;

    int code = ftp_command(c, NULL, 0, "STOR %s", remote);
    if (code != 150 && code != 125) {
        ftp_data_close(&dsock);
        if (err) snprintf(err, errlen, "STOR refusé (code %d)", code);
        return false;
    }
    if (!ftp_data_secure(c, &dsock, err, errlen)) { ftp_data_close(&dsock); return false; }

    bool ok = true;
    size_t sent = 0;
    const size_t CH = 8192;
    while (sent < len) {
        size_t chunk = (len - sent < CH) ? (len - sent) : CH;
        int n = ftp_data_send(&dsock, data + sent, chunk);
        if (n <= 0) { ok = false; break; }
        sent += (size_t)n;
        if (prog && !prog(pctx, (int64_t)len, (int64_t)sent)) { ok = false; break; }
    }
    ftp_data_close(&dsock);                      /* EOF cote serveur */

    int fin = ftp_read_reply(c, NULL, 0);
    if (fin != 226 && fin != 250) ok = false;
    if (!ok && err && err[0] == '\0') snprintf(err, errlen, "transfert STOR incomplet");
    return ok;
}

/* ---- Metadonnees (canal de controle seul, a fichier explicite) ---- */

int64_t owl_ftp_size(OwlFtpClient* c, const char* remote) {
    if (c == NULL || remote == NULL) return -1;
    if (c->is_sftp) {
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (!sftp_stat(c, remote, &a) || !(a.flags & LIBSSH2_SFTP_ATTR_SIZE)) return -1;
        return (int64_t)a.filesize;
    }
    /* SIZE en binaire : certains serveurs refusent SIZE en mode ASCII. */
    ftp_command(c, NULL, 0, "TYPE I");
    char text[128];
    if (ftp_command(c, text, sizeof(text), "SIZE %s", remote) != 213) return -1;
    return (int64_t)strtoll(text, NULL, 10);
}

/* Recopie jusqu'a 14 chiffres consecutifs d'un horodatage dans out
 * (AAAAMMJJHHMMSS). Renvoie false si moins de 14 chiffres. */
static bool ftp_copy_ts14(const char* src, char* out, size_t outsz) {
    if (outsz < 15) return false;
    size_t n = 0;
    while (*src && (*src < '0' || *src > '9')) src++;   /* saute jusqu'au 1er chiffre */
    while (src[n] >= '0' && src[n] <= '9' && n < 14) { out[n] = src[n]; n++; }
    if (n < 14) return false;
    out[14] = '\0';
    return true;
}

bool owl_ftp_mdtm(OwlFtpClient* c, const char* remote, char* out, size_t outsz) {
    if (c == NULL || remote == NULL || out == NULL) return false;
    char text[128];
    if (ftp_command(c, text, sizeof(text), "MDTM %s", remote) != 213) return false;
    /* Reponse : "AAAAMMJJHHMMSS" eventuellement suivi de ".fff". */
    return ftp_copy_ts14(text, out, outsz);
}

bool owl_ftp_mlst(OwlFtpClient* c, const char* remote, char* facts, size_t factssz) {
    if (c == NULL || remote == NULL || facts == NULL) return false;
    char text[1024];
    if (ftp_command(c, text, sizeof(text), "MLST %s", remote) != 250) return false;
    /* Reponse multiligne 250 : la ligne de facts est la ligne de
     * continuation (ex " modify=...;create=...;type=file; /chemin"). On
     * copie la 1re ligne de continuation, sans l'espace initial. */
    const char* nl = strchr(text, '\n');
    const char* fl = nl ? nl + 1 : text;
    while (*fl == ' ' || *fl == '\t') fl++;
    snprintf(facts, factssz, "%s", fl);
    /* Coupe a la fin de la ligne de facts. */
    char* end = strchr(facts, '\n');
    if (end) *end = '\0';
    return true;
}

/* Parse une ligne MLSD (RFC 3659) : "fact=val;fact=val; nom". Remonte nom,
 * taille, type, date (AAAAMMJJ) et heure (HHMMSS). Renvoie false si le nom
 * designe le repertoire lui-meme (cdir/pdir) ou "."/"..". */
static bool ftp_parse_mlsd_line(const char* line, char* name, size_t name_sz,
                                int64_t* size, char* attr, size_t attr_sz,
                                char* date, size_t date_sz,
                                char* heure, size_t heure_sz) {
    const char* sp = strchr(line, ' ');
    if (sp == NULL) return false;
    size_t facts_len = (size_t)(sp - line);
    const char* nm = sp + 1;
    if (nm[0] == '\0') return false;
    if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) return false;

    *size = -1;
    attr[0] = '\0'; date[0] = '\0'; heure[0] = '\0';
    snprintf(name, name_sz, "%s", nm);

    /* Parcours manuel des paires key=value; dans le prefixe facts (pas de
     * strtok : reste reentrant et portable). */
    char facts[512];
    size_t fl = facts_len < sizeof(facts) - 1 ? facts_len : sizeof(facts) - 1;
    memcpy(facts, line, fl);
    facts[fl] = '\0';

    char* tok = facts;
    while (*tok) {
        char* semi = strchr(tok, ';');
        if (semi) *semi = '\0';
        char* eq = strchr(tok, '=');
        if (eq != NULL) {
            *eq = '\0';
            const char* key = tok;
            const char* val = eq + 1;
            if (strcasecmp(key, "size") == 0) {
                *size = (int64_t)strtoll(val, NULL, 10);
            } else if (strcasecmp(key, "type") == 0) {
                snprintf(attr, attr_sz, "%s", val);
                if (strcasecmp(val, "cdir") == 0 || strcasecmp(val, "pdir") == 0)
                    return false;
            } else if (strcasecmp(key, "modify") == 0) {
                /* AAAAMMJJHHMMSS -> date AAAAMMJJ + heure HHMMSS. */
                size_t vl = strlen(val);
                if (vl >= 8)  snprintf(date, date_sz, "%.8s", val);
                if (vl >= 14) snprintf(heure, heure_sz, "%.6s", val + 8);
            }
        }
        if (semi == NULL) break;
        tok = semi + 1;
    }
    return true;
}

/* Parse une ligne LIST facon "ls -l" Unix (repli heuristique). Renvoie
 * false si la ligne n'est pas exploitable ou designe "."/"..". */
static bool ftp_parse_list_line(const char* line, char* name, size_t name_sz,
                                int64_t* size, char* attr, size_t attr_sz,
                                char* date, size_t date_sz,
                                char* heure, size_t heure_sz) {
    *size = -1;
    attr[0] = '\0'; date[0] = '\0'; heure[0] = '\0';
    name[0] = '\0';

    /* On ne gere que le format Unix (commence par un type + permissions,
     * ex "-rw-r--r--" ou "drwxr-xr-x"). Le format DOS/IIS n'est pas cible. */
    if (line[0] != '-' && line[0] != 'd' && line[0] != 'l') return false;
    snprintf(attr, attr_sz, "%s", line[0] == 'd' ? "dir" : "file");

    /* 9 champs : perms links owner group size mois jour heure/annee nom... */
    const char* p = line;
    const char* fields[9];
    int nf = 0;
    while (*p && nf < 9) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        fields[nf++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (nf < 9) return false;
    *size = (int64_t)strtoll(fields[4], NULL, 10);
    /* Date = "mois jour", heure = champ 8 (HH:MM ou annee). */
    { char mois[8], jour[8];
      sscanf(fields[5], "%7s", mois);
      sscanf(fields[6], "%7s", jour);
      snprintf(date, date_sz, "%s %s", mois, jour); }
    { size_t k = 0; const char* h = fields[7];
      while (h[k] && h[k] != ' ' && h[k] != '\t' && k + 1 < heure_sz) {
          heure[k] = h[k]; k++;
      }
      heure[k] = '\0'; }
    /* Le nom est le reste de la ligne a partir du 9e champ. */
    const char* nm = fields[8];
    snprintf(name, name_sz, "%s", nm);
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return true;
}

int owl_ftp_list(OwlFtpClient* c, const char* path,
                 OwlFtpEntryFn cb, void* ctx) {
    if (c == NULL) return -1;
    if (c->is_sftp) return sftp_list(c, path, cb, ctx);
    ftp_command(c, NULL, 0, "TYPE A");           /* listage transmis en ASCII */

    FtpDataChan dsock;
    if (!ftp_pasv_open(c, &dsock, NULL, 0)) return -1;

    bool mlsd = c->has_mlsd;
    int code;
    if (mlsd) code = ftp_command(c, NULL, 0, "MLSD %s", path ? path : "");
    else      code = ftp_command(c, NULL, 0, "LIST %s", path ? path : "");
    if (code != 150 && code != 125) {
        /* MLSD refuse : repli LIST sur un nouveau canal de donnees. */
        ftp_data_close(&dsock);
        if (mlsd) {
            if (!ftp_pasv_open(c, &dsock, NULL, 0)) return -1;
            mlsd = false;
            code = ftp_command(c, NULL, 0, "LIST %s", path ? path : "");
            if (code != 150 && code != 125) { ftp_data_close(&dsock); return -1; }
        } else {
            return -1;
        }
    }
    if (!ftp_data_secure(c, &dsock, NULL, 0)) { ftp_data_close(&dsock); return -1; }

    /* Lit tout le canal de donnees. */
    char* body = NULL;
    size_t blen = 0, bcap = 0;
    char buf[4096];
    for (;;) {
        int n = ftp_data_recv(&dsock, buf, sizeof(buf));
        if (n <= 0) break;
        if (blen + (size_t)n + 1 > bcap) {
            size_t ncap = bcap ? bcap * 2 : 8192;
            while (ncap < blen + (size_t)n + 1) ncap *= 2;
            char* nb = (char*)realloc(body, ncap);
            if (nb == NULL) { free(body); ftp_data_close(&dsock); return -1; }
            body = nb; bcap = ncap;
        }
        memcpy(body + blen, buf, (size_t)n);
        blen += (size_t)n;
    }
    ftp_data_close(&dsock);
    ftp_read_reply(c, NULL, 0);                  /* 226 = listage complet */
    if (body) body[blen] = '\0';

    int count = 0;
    char name[512], attr[64], date[32], heure[32];
    int64_t size;
    char* line = body;
    while (line != NULL && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        size_t ll = strlen(line);
        if (ll > 0 && line[ll - 1] == '\r') line[--ll] = '\0';

        bool got = ll > 0 &&
            (mlsd ? ftp_parse_mlsd_line(line, name, sizeof(name), &size,
                                        attr, sizeof(attr), date, sizeof(date),
                                        heure, sizeof(heure))
                  : ftp_parse_list_line(line, name, sizeof(name), &size,
                                        attr, sizeof(attr), date, sizeof(date),
                                        heure, sizeof(heure)));
        if (got) {
            count++;
            if (cb && !cb(ctx, name, size, attr, date, heure)) break;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(body);
    return count;
}

/* ============================================================
 * FTPConnecte — ouverture reelle du canal de controle.
 * ============================================================ */

/* Mappe le masque ftpIgnore* (OU-binaire) vers un niveau de verification TLS
 * de net_tcp_tls (NONE/CHAIN/FULL).
 *
 * GRAIN VOLONTAIREMENT GROSSIER (mais correct) : mbedtls verifie le
 * certificat PENDANT le handshake, en mode REQUIRED (la couche partagee
 * net_tcp_tls echoue le handshake sur defaut). Sans chirurgie invasive de ce
 * module partage (HTTP/SQL en dependent), les seuls leviers sont FULL / CHAIN
 * / NONE. Consequence assumee et documentee :
 *   - aucun flag                     -> FULL  (chaine + nom d'hote)
 *   - ftpIgnoreNomCertificatInvalide -> CHAIN (nom ignore, chaine verifiee) — precis
 *   - invalide / expiré / révocation -> NONE  (accepte tout certificat)
 * Le dernier seau est SUR-permissif face a un "ignore par motif" strict :
 * en handshake REQUIRED on ne peut accepter une chaine expiree-seule ou
 * revoquee-seule tout en rejetant les autres defauts. On ne pretend donc PAS
 * un grain fin inexistant — cf. ADR / rapport implementer. */
static int ftp_verify_level_from_mask(int mask) {
    /* Motifs qui obligent a accepter une chaine que mbedtls rejetterait. */
    if (mask & (OWL_FTP_IGNORE_CERT_INVALIDE |
                OWL_FTP_IGNORE_CERT_EXPIRE |
                OWL_FTP_IGNORE_REVOCATION)) {
        return OWL_TLS_VERIFY_NONE;
    }
    if (mask & OWL_FTP_IGNORE_CERT_NOM) {
        return OWL_TLS_VERIFY_CHAIN;   /* nom ignore, chaine toujours verifiee */
    }
    return OWL_TLS_VERIFY_FULL;
}

/* FTPConnecte([ftps://|ftpes://|sftp://]serveur, utilisateur, motDePasse,
 *             [port], [mode], [optionsSécurité], [cléPrivée], [motDePasseClé])
 *             -> ftpClient
 *
 * Prefixe "sftp://" -> tunnel SSH-2 (libssh2), port 22 par defaut, auth par
 * mot de passe ou par cle privee OpenSSH (params 7-8). */
static Value fn_ftp_connecte(int argCount, Value* args) {
    const char* serveur = arg_str(args[0]);
    const char* user    = argCount > 1 ? arg_str(args[1]) : "";
    const char* pass    = argCount > 2 ? arg_str(args[2]) : "";
    int port = FTP_PORT_DEFAUT;
    bool port_explicit = false;
    if (argCount > 3 && IS_INTEGER(args[3])) {
        int64_t p = AS_INTEGER(args[3]);
        if (p > 0 && p < 65536) { port = (int)p; port_explicit = true; }
    }
    int mode = OWL_FTP_MODE_PASSIF;
    if (argCount > 4 && IS_INTEGER(args[4])) mode = (int)AS_INTEGER(args[4]);
    int secOptions = 0;
    if (argCount > 5 && IS_INTEGER(args[5])) secOptions = (int)AS_INTEGER(args[5]);
    const char* privkey      = argCount > 6 ? arg_str(args[6]) : NULL;
    const char* privkey_pass = argCount > 7 ? arg_str(args[7]) : NULL;

    if (serveur == NULL) serveur = "";
    if (user == NULL) user = "";
    if (pass == NULL) pass = "";

    /* Detache le prefixe : host reel + mode de transport/chiffrement. */
    char host[256];
    OwlFtpEncryption enc = owl_ftp_parse_prefix(serveur, host, sizeof(host));
    /* SFTP : port SSH 22 par defaut, sauf port explicite. */
    if (enc == OWL_FTP_ENC_SFTP && !port_explicit) port = 22;

    char cible[300];
    snprintf(cible, sizeof(cible), "%s:%d", host, port);

    char detail[256];
    OwlFtpClient* c;
    bool host_verified = false;
    if (enc == OWL_FTP_ENC_SFTP) {
        bool allow_unknown = (secOptions & OWL_FTP_IGNORE_HOTE_INCONNU) != 0;
        c = owl_sftp_open(host, port, user, pass, privkey, privkey_pass,
                          allow_unknown, &host_verified, detail, sizeof(detail));
    } else {
        int verify_level = ftp_verify_level_from_mask(secOptions);
        c = owl_ftp_open(host, port, user, pass, enc, verify_level,
                         detail, sizeof(detail));
    }
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_FTP_CONNECT_FAILURE, "FTPConnecte",
                              "native", "FTPConnecte",
                              "cible", cible, "detail", detail, NULL);
        return NIL_VAL;
    }

    /* Peuple l'instance retournee. */
    ObjString* nm = obj_copy_string("ftpClient", 9);
    vm_push(OBJ_VAL(nm));
    ObjInstance* inst = obj_new_instance(nm);
    vm_push(OBJ_VAL(inst));
    inst_field_set_str (inst, "Serveur",     host);
    inst_field_set_str (inst, "Utilisateur", user);
    inst_field_set_str (inst, "MotDePasse",  "");     /* jamais reflechi en clair */
    inst_field_set_int (inst, "Port",        port);
    inst_field_set_int (inst, "Mode",        mode);
    inst_field_set_bool(inst, "Connectee",   true);
    inst_field_set_bool(inst, "Securisee",   enc != OWL_FTP_ENC_CLEAR);
    inst_field_set_str (inst, "Chiffrement", owl_ftp_encryption_label(enc));
    inst_field_set_bool(inst, "VerifieHote", host_verified);
    cnx_set_handle(inst, c);
    vm_pop();
    vm_pop();

    owl_clear_error();
    return OBJ_VAL(inst);
}

/* ============================================================
 * FTPDeconnecte — QUIT + liberation.
 * ============================================================ */

static Value fn_ftp_deconnecte(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPDéconnecte");
    if (inst == NULL) return BOOL_VAL(false);

    OwlFtpClient* c = cnx_get_handle(inst);
    if (c != NULL) {
        owl_ftp_close(c);
        cnx_set_handle(inst, NULL);
    }
    inst_field_set_bool(inst, "Connectee", false);
    owl_clear_error();
    return BOOL_VAL(true);
}

/* ============================================================
 * Commandes de repertoire (canal de controle seul).
 * ============================================================ */

/* Exige un canal ouvert. Pose une soft-error et retourne NULL sinon. */
static OwlFtpClient* require_open(ObjInstance* inst, const char* fn) {
    OwlFtpClient* c = cnx_get_handle(inst);
    if (c == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, fn,
                              "native", fn, "arg", "1", NULL);
        return NULL;
    }
    return c;
}

/* FTPFichierExiste(cnx, chemin) -> booléen (via SIZE, repli MLST). */
static Value fn_ftp_fichier_existe(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPFichierExiste");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPFichierExiste");
    if (c == NULL) return BOOL_VAL(false);
    const char* chemin = arg_str(args[1]);
    if (chemin == NULL) return BOOL_VAL(false);

    if (c->is_sftp) { owl_clear_error(); return BOOL_VAL(sftp_exists(c, chemin)); }

    /* SIZE en mode binaire (certains serveurs refusent SIZE en ASCII). */
    ftp_command(c, NULL, 0, "TYPE I");
    int code = ftp_command(c, NULL, 0, "SIZE %s", chemin);
    if (code == 213) { owl_clear_error(); return BOOL_VAL(true); }
    /* Repli MLST pour les serveurs sans SIZE sur repertoire. */
    code = ftp_command(c, NULL, 0, "MLST %s", chemin);
    owl_clear_error();
    return BOOL_VAL(code == 250);
}

/* FTPSupprimeFichier(cnx, chemin) -> booléen (DELE). */
static Value fn_ftp_supprime_fichier(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPSupprimeFichier");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPSupprimeFichier");
    if (c == NULL) return BOOL_VAL(false);
    const char* chemin = arg_str(args[1]);
    if (chemin == NULL) return BOOL_VAL(false);
    if (c->is_sftp) { bool ok = sftp_unlink(c, chemin); owl_clear_error(); return BOOL_VAL(ok); }
    int code = ftp_command(c, NULL, 0, "DELE %s", chemin);
    owl_clear_error();
    return BOOL_VAL(code == 250);
}

/* FTPRenommeFichier(cnx, source, destination) -> booléen (RNFR/RNTO). */
static Value fn_ftp_renomme_fichier(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRenommeFichier");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPRenommeFichier");
    if (c == NULL) return BOOL_VAL(false);
    const char* src = arg_str(args[1]);
    const char* dst = arg_str(args[2]);
    if (src == NULL || dst == NULL) return BOOL_VAL(false);
    if (c->is_sftp) { bool ok = sftp_rename(c, src, dst); owl_clear_error(); return BOOL_VAL(ok); }
    int code = ftp_command(c, NULL, 0, "RNFR %s", src);
    if (code != 350) { owl_clear_error(); return BOOL_VAL(false); }
    code = ftp_command(c, NULL, 0, "RNTO %s", dst);
    owl_clear_error();
    return BOOL_VAL(code == 250);
}

/* FTPRepCrée(cnx, chemin) -> booléen (MKD). */
static Value fn_ftp_rep_cree(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRepCrée");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPRepCrée");
    if (c == NULL) return BOOL_VAL(false);
    const char* chemin = arg_str(args[1]);
    if (chemin == NULL) return BOOL_VAL(false);
    if (c->is_sftp) { bool ok = sftp_mkdir(c, chemin); owl_clear_error(); return BOOL_VAL(ok); }
    int code = ftp_command(c, NULL, 0, "MKD %s", chemin);
    owl_clear_error();
    return BOOL_VAL(code == 257);
}

/* FTPRepSupprime(cnx, chemin) -> booléen (RMD). */
static Value fn_ftp_rep_supprime(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRepSupprime");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPRepSupprime");
    if (c == NULL) return BOOL_VAL(false);
    const char* chemin = arg_str(args[1]);
    if (chemin == NULL) return BOOL_VAL(false);
    if (c->is_sftp) { bool ok = sftp_rmdir(c, chemin); owl_clear_error(); return BOOL_VAL(ok); }
    int code = ftp_command(c, NULL, 0, "RMD %s", chemin);
    owl_clear_error();
    return BOOL_VAL(code == 250);
}

/* Extrait le chemin entre guillemets d'une reponse PWD/MKD 257
 * ("/depot" est la valeur retournee...). Copie dans out. */
static void ftp_extract_quoted(const char* text, char* out, size_t out_size) {
    const char* start = strchr(text, '"');
    out[0] = '\0';
    if (start == NULL) { snprintf(out, out_size, "%s", text); return; }
    start++;
    size_t n = 0;
    while (*start && n + 1 < out_size) {
        if (start[0] == '"') {
            if (start[1] == '"') { out[n++] = '"'; start += 2; continue; }
            break;
        }
        out[n++] = *start++;
    }
    out[n] = '\0';
}

/* FTPRepEnCours(cnx)          -> chaîne  (PWD, lecture)
 * FTPRepEnCours(cnx, chemin)  -> booléen (CWD/CDUP, modification) */
static Value fn_ftp_rep_en_cours(int argCount, Value* args) {
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRepEnCours");
    if (inst == NULL) return argCount > 1 ? BOOL_VAL(false) : NIL_VAL;
    OwlFtpClient* c = require_open(inst, "FTPRepEnCours");
    if (c == NULL) return argCount > 1 ? BOOL_VAL(false) : NIL_VAL;

    if (argCount > 1) {
        const char* chemin = arg_str(args[1]);
        if (chemin == NULL) return BOOL_VAL(false);
        if (c->is_sftp) { bool ok = sftp_chdir(c, chemin); owl_clear_error(); return BOOL_VAL(ok); }
        int code;
        if (strcmp(chemin, "..") == 0) code = ftp_command(c, NULL, 0, "CDUP");
        else                           code = ftp_command(c, NULL, 0, "CWD %s", chemin);
        owl_clear_error();
        return BOOL_VAL(code == 250);
    }

    if (c->is_sftp) {
        owl_clear_error();
        return OBJ_VAL(obj_copy_string(c->cwd, (int)strlen(c->cwd)));
    }

    char text[1024];
    int code = ftp_command(c, text, sizeof(text), "PWD");
    if (code != 257) { owl_clear_error(); return OBJ_VAL(obj_copy_string("", 0)); }
    char path[1024];
    ftp_extract_quoted(text, path, sizeof(path));
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(path, (int)strlen(path)));
}

/* ============================================================
 * Transferts et listage — canal de donnees reel (PASV).
 *
 * Le coeur protocole (owl_ftp_retr/stor/list) est VM-free : il remonte
 * ses octets/entrees par callbacks C. Les adaptateurs ci-dessous
 * marshalent ces callbacks vers une procedure OWL via vm_call_reentrant.
 * GC : chaque valeur fraiche (ObjString) est poussee sur la pile VM
 * AVANT toute allocation suivante — elle est donc rootee le temps de
 * l'appel reentrant (cf. ). Dans les scenarios owl test, ces
 * natives sont mockees : le corps reel ne s'execute pas (pas de reseau).
 * ============================================================ */

static int ftp_callable_arity(Value cb) {
    if (IS_FUNCTION(cb)) return AS_FUNCTION(cb)->arity;
    if (IS_NATIVE(cb))   return ((ObjNative*)AS_OBJ(cb))->arity;
    return -1;
}

/* Glob minimal ('*' et '?') applique au nom d'une entree de listage. */
static bool ftp_glob_match(const char* pat, const char* str) {
    if (*pat == '\0') return *str == '\0';
    if (*pat == '*') {
        while (*pat == '*') pat++;
        if (*pat == '\0') return true;
        for (; *str; str++) if (ftp_glob_match(pat, str)) return true;
        return ftp_glob_match(pat, str);
    }
    if (*str == '\0') return false;
    if (*pat == '?' || *pat == *str) return ftp_glob_match(pat + 1, str + 1);
    return false;
}

/* ---- Marshalling listage : entree C -> procedure OWL ---- */
typedef struct {
    Value       callback;   /* procedure OWL de rappel */
    const char* pattern;    /* glob applique au nom, NULL = pas de filtre */
    bool        has_cb;     /* une procedure a-t-elle ete fournie ? */
    int         index;      /* param 1-based passe au rappel */
    int         matched;    /* entrees presentees au rappel */
    bool        error;      /* le rappel a leve une exception (unwind VM) */
} FtpListMarshalCtx;

static bool ftp_list_marshal_cb(void* vctx, const char* name, int64_t size,
                                const char* attribut, const char* date,
                                const char* heure) {
    FtpListMarshalCtx* m = (FtpListMarshalCtx*)vctx;
    if (m->pattern && !ftp_glob_match(m->pattern, name)) return true;
    m->matched++;
    m->index++;
    if (!m->has_cb) return true;

    /* Signature documentee : (nom, taille, attribut, date, heure, param).
     * On adapte au nombre de parametres declares par la procedure. */
    int ar = ftp_callable_arity(m->callback);
    int n = 0;
    vm_push(m->callback);
    vm_push(OBJ_VAL(obj_copy_string(name, (int)strlen(name)))); n++;
    if (ar < 0 || ar >= 2) { vm_push(INTEGER_VAL(size)); n++; }
    if (ar < 0 || ar >= 3) { vm_push(OBJ_VAL(obj_copy_string(attribut, (int)strlen(attribut)))); n++; }
    if (ar < 0 || ar >= 4) { vm_push(OBJ_VAL(obj_copy_string(date, (int)strlen(date)))); n++; }
    if (ar < 0 || ar >= 5) { vm_push(OBJ_VAL(obj_copy_string(heure, (int)strlen(heure)))); n++; }
    if (ar < 0 || ar >= 6) { vm_push(INTEGER_VAL(m->index)); n++; }

    if (vm_call_reentrant(m->callback, n) != INTERPRET_OK) {
        m->error = true;
        return false;
    }
    Value r = vm_pop();
    return !(IS_BOOL(r) && AS_BOOL(r) == false);   /* RENVOYER Faux interrompt */
}

/* ---- Marshalling progression : (total, transfere) -> procedure OWL ---- */
typedef struct {
    Value callback;
    bool  has_cb;
    bool  error;
} FtpProgressMarshalCtx;

static bool ftp_progress_marshal_cb(void* vctx, int64_t total, int64_t done) {
    FtpProgressMarshalCtx* m = (FtpProgressMarshalCtx*)vctx;
    if (!m->has_cb) return true;
    vm_push(m->callback);
    vm_push(INTEGER_VAL(total));
    vm_push(INTEGER_VAL(done));
    if (vm_call_reentrant(m->callback, 2) != INTERPRET_OK) {
        m->error = true;
        return false;
    }
    Value r = vm_pop();
    return !(IS_BOOL(r) && AS_BOOL(r) == false);   /* RENVOYER Faux interrompt */
}

/* ---- Puits fichier local pour RETR ---- */
typedef struct { FILE* f; bool ok; } FtpFileSinkCtx;

static bool ftp_file_sink(void* vctx, const char* buf, size_t len) {
    FtpFileSinkCtx* s = (FtpFileSinkCtx*)vctx;
    if (s->f == NULL) return false;
    if (fwrite(buf, 1, len, s->f) != len) { s->ok = false; return false; }
    return true;
}

/* ---- Collecte d'entrees (FTPRécupère : recopie d'un repertoire) ---- */
#define FTP_DIR_MAX 256
typedef struct {
    char names[FTP_DIR_MAX][256];
    char types[FTP_DIR_MAX][32];
    int  count;
} FtpDirCollect;

static bool ftp_dir_collect_cb(void* vctx, const char* name, int64_t size,
                               const char* attribut, const char* date,
                               const char* heure) {
    (void)size; (void)date; (void)heure;
    FtpDirCollect* d = (FtpDirCollect*)vctx;
    if (d->count >= FTP_DIR_MAX) return false;
    snprintf(d->names[d->count], sizeof(d->names[0]), "%s", name);
    snprintf(d->types[d->count], sizeof(d->types[0]), "%s", attribut);
    d->count++;
    return true;
}

/* FTPEnvoie(cnx, local, distant, [progression], [mode]) -> booléen (STOR). */
static Value fn_ftp_envoie(int argCount, Value* args) {
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPEnvoie");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPEnvoie");
    if (c == NULL) return BOOL_VAL(false);
    const char* local   = arg_str(args[1]);
    const char* distant = arg_str(args[2]);
    if (local == NULL || distant == NULL) return BOOL_VAL(false);

    Value cbv = NIL_VAL;
    if (argCount > 3 && (IS_FUNCTION(args[3]) || IS_NATIVE(args[3]))) cbv = args[3];
    bool ascii = (argCount > 4 && IS_INTEGER(args[4]) &&
                  AS_INTEGER(args[4]) == OWL_FTP_TRANSFERT_ASCII);

    FILE* f = fopen(local, "rb");
    if (f == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "FTPEnvoie",
                              "native", "FTPEnvoie", "arg", "2", NULL);
        return BOOL_VAL(false);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return BOOL_VAL(false); }
    char* data = (char*)malloc((size_t)sz + 1);
    if (data == NULL) { fclose(f); return BOOL_VAL(false); }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);

    FtpProgressMarshalCtx pm = { cbv, !IS_NIL(cbv), false };
    char err[256];
    bool ok = owl_ftp_stor(c, distant, ascii, data, rd,
                           IS_NIL(cbv) ? NULL : ftp_progress_marshal_cb, &pm,
                           err, sizeof(err));
    free(data);
    if (pm.error) return BOOL_VAL(false);   /* exception : dispatch propage l'unwind */
    owl_clear_error();
    return BOOL_VAL(ok);
}

/* FTPRécupère(cnx, distant, local) -> booléen : recopie tout un répertoire. */
static Value fn_ftp_recupere(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRécupère");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPRécupère");
    if (c == NULL) return BOOL_VAL(false);
    const char* distant = arg_str(args[1]);
    const char* local   = arg_str(args[2]);
    if (distant == NULL || local == NULL) return BOOL_VAL(false);

    FtpDirCollect* d = (FtpDirCollect*)calloc(1, sizeof(FtpDirCollect));
    if (d == NULL) return BOOL_VAL(false);
    int n = owl_ftp_list(c, distant, ftp_dir_collect_cb, d);
    if (n < 0) { free(d); return BOOL_VAL(false); }

    bool all = true;
    char remote[600], localpath[600], err[256];
    for (int i = 0; i < d->count; i++) {
        if (strcasecmp(d->types[i], "dir") == 0) continue;   /* non récursif */
        snprintf(remote, sizeof(remote), "%s/%s", distant, d->names[i]);
        snprintf(localpath, sizeof(localpath), "%s/%s", local, d->names[i]);
        FILE* f = fopen(localpath, "wb");
        if (f == NULL) { all = false; continue; }
        FtpFileSinkCtx sink = { f, true };
        bool ok = owl_ftp_retr(c, remote, false, ftp_file_sink, &sink,
                               NULL, NULL, err, sizeof(err));
        fclose(f);
        if (!ok || !sink.ok) all = false;
    }
    free(d);
    owl_clear_error();
    return BOOL_VAL(all);
}

/* FTPRécupèreFichier(cnx, distant, local, [progression], [mode]) -> booléen (RETR). */
static Value fn_ftp_recupere_fichier(int argCount, Value* args) {
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPRécupèreFichier");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPRécupèreFichier");
    if (c == NULL) return BOOL_VAL(false);
    const char* distant = arg_str(args[1]);
    const char* local   = arg_str(args[2]);
    if (distant == NULL || local == NULL) return BOOL_VAL(false);

    Value cbv = NIL_VAL;
    if (argCount > 3 && (IS_FUNCTION(args[3]) || IS_NATIVE(args[3]))) cbv = args[3];
    bool ascii = (argCount > 4 && IS_INTEGER(args[4]) &&
                  AS_INTEGER(args[4]) == OWL_FTP_TRANSFERT_ASCII);

    FILE* f = fopen(local, "wb");
    if (f == NULL) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "FTPRécupèreFichier",
                              "native", "FTPRécupèreFichier", "arg", "3", NULL);
        return BOOL_VAL(false);
    }
    FtpFileSinkCtx sink = { f, true };
    FtpProgressMarshalCtx pm = { cbv, !IS_NIL(cbv), false };
    char err[256];
    bool ok = owl_ftp_retr(c, distant, ascii, ftp_file_sink, &sink,
                           IS_NIL(cbv) ? NULL : ftp_progress_marshal_cb, &pm,
                           err, sizeof(err));
    fclose(f);
    if (pm.error) return BOOL_VAL(false);   /* exception : dispatch propage l'unwind */
    owl_clear_error();
    return BOOL_VAL(ok && sink.ok);
}

/* FTPListeFichier(cnx, masque, rappel) -> entier : nombre d'entrées listées. */
static Value fn_ftp_liste_fichier(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPListeFichier");
    if (inst == NULL) return INTEGER_VAL(0);
    OwlFtpClient* c = require_open(inst, "FTPListeFichier");
    if (c == NULL) return INTEGER_VAL(0);
    const char* masque = arg_str(args[1]);
    if (masque == NULL) masque = "";
    Value cbv = NIL_VAL;
    if (argCount > 2 && (IS_FUNCTION(args[2]) || IS_NATIVE(args[2]))) cbv = args[2];

    /* Sépare répertoire et motif au dernier '/'. Le motif (glob) filtre les
     * noms d'entrées ; le répertoire est listé via MLSD/LIST. */
    char dir[512], pat[256];
    const char* slash = strrchr(masque, '/');
    if (slash) {
        size_t dl = (size_t)(slash - masque);
        if (dl >= sizeof(dir)) dl = sizeof(dir) - 1;
        memcpy(dir, masque, dl); dir[dl] = '\0';
        snprintf(pat, sizeof(pat), "%s", slash + 1);
    } else {
        dir[0] = '\0';
        snprintf(pat, sizeof(pat), "%s", masque);
    }

    FtpListMarshalCtx m;
    m.callback = cbv;
    m.pattern  = pat[0] ? pat : NULL;
    m.has_cb   = !IS_NIL(cbv);
    m.index    = 0;
    m.matched  = 0;
    m.error    = false;

    int n = owl_ftp_list(c, dir[0] ? dir : masque, ftp_list_marshal_cb, &m);
    if (m.error) return INTEGER_VAL(0);   /* exception : dispatch propage l'unwind */
    if (n < 0) { owl_clear_error(); return INTEGER_VAL(0); }
    owl_clear_error();
    return INTEGER_VAL(m.matched);
}

/* ============================================================
 * Metadonnees fichier — a fichier explicite (canal de controle seul).
 *
 * Aucun etat cache "dernier fichier consulte" (divergence assumee du canon,
 * ) : chaque native prend le chemin en argument. Dans les scenarios
 * owl test ces natives sont mockees ; le corps reel ci-dessous ne s'execute
 * qu'en production (SIZE/MDTM/MLST sur le canal de controle).
 * ============================================================ */

/* FTPTaille(cnx, fichier) -> entier : taille en octets, -1 si erreur (SIZE). */
static Value fn_ftp_taille(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPTaille");
    if (inst == NULL) return INTEGER_VAL(-1);
    OwlFtpClient* c = require_open(inst, "FTPTaille");
    if (c == NULL) return INTEGER_VAL(-1);
    const char* fichier = arg_str(args[1]);
    if (fichier == NULL) return INTEGER_VAL(-1);
    int64_t sz = owl_ftp_size(c, fichier);
    owl_clear_error();
    return INTEGER_VAL(sz);
}

/* Valeur d'un fact MLST ("modify=", "create=", "type="), ou NULL. */
static const char* ftp_fact_value(const char* facts, const char* key) {
    const char* p = strstr(facts, key);
    return p ? p + strlen(key) : NULL;
}

/* Decoupe un horodatage AAAAMMJJHHMMSS en composantes. */
static bool ftp_split_ts(const char* ts, int* Y, int* Mo, int* D,
                         int* h, int* mi, int* s) {
    if (strlen(ts) < 14) return false;
    char b[5];
    b[0]=ts[0]; b[1]=ts[1]; b[2]=ts[2]; b[3]=ts[3]; b[4]='\0'; *Y  = atoi(b);
    b[0]=ts[4]; b[1]=ts[5]; b[2]='\0'; *Mo = atoi(b);
    b[0]=ts[6]; b[1]=ts[7]; b[2]='\0'; *D  = atoi(b);
    b[0]=ts[8]; b[1]=ts[9]; b[2]='\0'; *h  = atoi(b);
    b[0]=ts[10]; b[1]=ts[11]; b[2]='\0'; *mi = atoi(b);
    b[0]=ts[12]; b[1]=ts[13]; b[2]='\0'; *s  = atoi(b);
    return true;
}

/* Recupere l'horodatage AAAAMMJJHHMMSS d'un fichier selon typeDate.
 * Modification : MDTM (repli MLST modify) ; creation : MLST create ;
 * acces : MLST (repli modify, faute de fact standard). */
static bool cnx_fetch_ts(OwlFtpClient* c, const char* fichier, int typeDate,
                         char* ts, size_t tssz) {
    if (typeDate == OWL_FTP_TYPEDATE_MODIFICATION &&
        owl_ftp_mdtm(c, fichier, ts, tssz)) {
        return true;
    }
    char facts[512];
    if (!owl_ftp_mlst(c, fichier, facts, sizeof(facts))) return false;
    const char* v = NULL;
    if (typeDate == OWL_FTP_TYPEDATE_CREATION) v = ftp_fact_value(facts, "create=");
    if (v == NULL)                             v = ftp_fact_value(facts, "modify=");
    if (v == NULL) return false;
    return ftp_copy_ts14(v, ts, tssz);
}

typedef enum { FTP_KIND_DATE, FTP_KIND_HEURE, FTP_KIND_DATEHEURE } FtpDateKind;

static Value ftp_meta_datetime(int argCount, Value* args, const char* fn,
                               FtpDateKind kind) {
    ObjInstance* inst = resolve_ftp_cnx(args[0], fn);
    if (inst == NULL) return NIL_VAL;
    OwlFtpClient* c = require_open(inst, fn);
    if (c == NULL) return NIL_VAL;
    const char* fichier = arg_str(args[1]);
    if (fichier == NULL) return NIL_VAL;
    int typeDate = OWL_FTP_TYPEDATE_MODIFICATION;
    if (argCount > 2 && IS_INTEGER(args[2])) typeDate = (int)AS_INTEGER(args[2]);

    char ts[16];
    int Y, Mo, D, h, mi, s;
    if (c->is_sftp) {
        /* SFTP : horodatages atime/mtime des attributs (secondes epoch, UTC).
         * SFTP v3 n'expose pas de date de creation -> repli sur modification. */
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (!sftp_stat(c, fichier, &a) || !(a.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, fn,
                                  "native", fn, "arg", "2", NULL);
            return NIL_VAL;
        }
        unsigned long epoch = (typeDate == OWL_FTP_TYPEDATE_ACCES) ? a.atime : a.mtime;
        char date[16], heure[16];
        sftp_format_epoch(epoch, date, sizeof(date), heure, sizeof(heure));
        snprintf(ts, sizeof(ts), "%s%s", date, heure);
        if (!ftp_split_ts(ts, &Y, &Mo, &D, &h, &mi, &s)) {
            owl_emit_native_error(E_NATIVE_ARG_INVALID, fn,
                                  "native", fn, "arg", "2", NULL);
            return NIL_VAL;
        }
    } else if (!cnx_fetch_ts(c, fichier, typeDate, ts, sizeof(ts)) ||
               !ftp_split_ts(ts, &Y, &Mo, &D, &h, &mi, &s)) {
        owl_emit_native_error(E_NATIVE_ARG_INVALID, fn,
                              "native", fn, "arg", "2", NULL);
        return NIL_VAL;
    }
    owl_clear_error();
    switch (kind) {
        case FTP_KIND_DATE:      return OBJ_VAL(obj_new_date(Y, Mo, D));
        case FTP_KIND_HEURE:     return OBJ_VAL(obj_new_time(h, mi, s, 0));
        case FTP_KIND_DATEHEURE: return OBJ_VAL(obj_new_datetime(Y, Mo, D, h, mi, s, 0));
    }
    return NIL_VAL;
}

/* FTPDate(cnx, fichier, [typeDate])      -> date      (MDTM/MLST). */
static Value fn_ftp_date(int argCount, Value* args) {
    return ftp_meta_datetime(argCount, args, "FTPDate", FTP_KIND_DATE);
}
/* FTPHeure(cnx, fichier, [typeDate])     -> heure. */
static Value fn_ftp_heure(int argCount, Value* args) {
    return ftp_meta_datetime(argCount, args, "FTPHeure", FTP_KIND_HEURE);
}
/* FTPDateHeure(cnx, fichier, [typeDate]) -> dateheure. */
static Value fn_ftp_dateheure(int argCount, Value* args) {
    return ftp_meta_datetime(argCount, args, "FTPDateHeure", FTP_KIND_DATEHEURE);
}

/* Lettre de perm= (RFC 3659) autorisant une modification cote client :
 * ecriture (w), ajout (a), suppression de fichier (d), creation dans un
 * repertoire (c), creation de sous-repertoire (m), suppression du contenu
 * d'un repertoire (p), renommage (f). Presence d'au moins une -> inscriptible. */
static bool ftp_perm_is_write_letter(char ch) {
    switch (ch) {
        case 'w': case 'W': case 'a': case 'A': case 'd': case 'D':
        case 'c': case 'C': case 'm': case 'M': case 'p': case 'P':
        case 'f': case 'F': return true;
        default: return false;
    }
}

/* Construit la chaine d'attributs PORTABLE d'un fichier distant depuis les
 * facts MLST (RFC 3659). Sous-ensemble volontairement restreint aux attributs
 * derivables du protocole, sans invention :
 *   R  lecture seule  <- fact perm= present et depourvu de toute lettre de
 *                        modification (cf. ftp_perm_is_write_letter)
 *   D  repertoire     <- fact type=dir
 * Ordre canon : R avant D (un repertoire en lecture seule -> "RD"). out recoit
 * une chaine vide si aucun attribut portable ne s'applique (ex. fichier
 * ordinaire accessible en ecriture, ou serveur ne publiant pas perm=).
 *
 * Divergence assumee (trace de la revue) : les attributs H (cache), S
 * (systeme), A (archive) et C (compresse) NE SONT PAS exposes. Ce sont des
 * attributs specifiques aux systemes de fichiers DOS/Windows-FAT ; ils n'ont
 * aucune representation portable dans MLST — un serveur Unix ne les publie
 * pas. Les fabriquer reviendrait a inventer une valeur non fondee sur le
 * protocole, en contradiction avec le coeur passif cross-platform.
 * On assume le manque cote serveurs legacy/Windows. */
void owl_ftp_attr_from_facts(const char* facts, char* out, size_t outsz) {
    size_t n = 0;

    /* R — lecture seule : perm= present, sans aucune lettre de modification. */
    const char* perm = ftp_fact_value(facts, "perm=");
    if (perm != NULL) {
        bool writable = false;
        for (const char* p = perm; *p != '\0' && *p != ';'; p++) {
            if (ftp_perm_is_write_letter(*p)) { writable = true; break; }
        }
        if (!writable && n + 1 < outsz) out[n++] = 'R';
    }

    /* D — repertoire. */
    const char* t = ftp_fact_value(facts, "type=");
    if (t != NULL && strncasecmp(t, "dir", 3) == 0 && n + 1 < outsz) out[n++] = 'D';

    if (outsz > 0) out[n] = '\0';
}

/* FTPAttribut(cnx, fichier) -> chaîne : attributs portables du fichier distant
 * dérivés des facts MLST ("R" lecture seule, "D" répertoire, "" si aucun),
 * "ERR" en cas d'erreur. */
static Value fn_ftp_attribut(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPAttribut");
    if (inst == NULL) return OBJ_VAL(obj_copy_string("ERR", 3));
    OwlFtpClient* c = require_open(inst, "FTPAttribut");
    if (c == NULL) return OBJ_VAL(obj_copy_string("ERR", 3));
    const char* fichier = arg_str(args[1]);
    if (fichier == NULL) return OBJ_VAL(obj_copy_string("ERR", 3));

    if (c->is_sftp) {
        LIBSSH2_SFTP_ATTRIBUTES probe;
        if (!sftp_stat(c, fichier, &probe)) {
            owl_clear_error();
            return OBJ_VAL(obj_copy_string("ERR", 3));
        }
        char attr[8];
        sftp_attr(c, fichier, attr, sizeof(attr));
        owl_clear_error();
        return OBJ_VAL(obj_copy_string(attr, strlen(attr)));
    }

    char facts[512];
    if (!owl_ftp_mlst(c, fichier, facts, sizeof(facts))) {
        owl_clear_error();
        return OBJ_VAL(obj_copy_string("ERR", 3));
    }
    char attr[8];
    owl_ftp_attr_from_facts(facts, attr, sizeof(attr));
    owl_clear_error();
    return OBJ_VAL(obj_copy_string(attr, strlen(attr)));
}

/* FTPCommande(cnx, commande) -> booléen : envoie une commande FTP brute sur le
 * canal de contrôle. Vrai si le serveur répond par un code de succès (2xx). */
static Value fn_ftp_commande(int argCount, Value* args) {
    (void)argCount;
    ObjInstance* inst = resolve_ftp_cnx(args[0], "FTPCommande");
    if (inst == NULL) return BOOL_VAL(false);
    OwlFtpClient* c = require_open(inst, "FTPCommande");
    if (c == NULL) return BOOL_VAL(false);
    const char* commande = arg_str(args[1]);
    if (commande == NULL) return BOOL_VAL(false);
    if (c->is_sftp) {
        /* SFTP n'a pas de canal de commande texte (RFC 959) : envoyer une
         * commande FTP brute n'a pas de sens sur un tunnel SSH. Soft-error. */
        owl_emit_native_error(E_NATIVE_ARG_INVALID, "FTPCommande",
                              "native", "FTPCommande", "arg", "2", NULL);
        return BOOL_VAL(false);
    }
    int code = ftp_command(c, NULL, 0, "%s", commande);
    owl_clear_error();
    return BOOL_VAL(code >= 200 && code < 300);
}

/* ============================================================
 * methodBindings — formes methode cnx.Xxx().
 * ============================================================ */

static const OwlMethodBinding MB_DECONNECTE[] = {
    { "ftpClient", "Deconnecte", 1 }, { "ftpClient", "Disconnect", 1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_FICHIER_EXISTE[] = {
    { "ftpClient", "FichierExiste", 2 }, { "ftpClient", "FileExists", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_SUPPRIME_FICHIER[] = {
    { "ftpClient", "SupprimeFichier", 2 }, { "ftpClient", "DeleteFile", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_RENOMME_FICHIER[] = {
    { "ftpClient", "RenommeFichier", 3 }, { "ftpClient", "RenameFile", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REP_CREE[] = {
    { "ftpClient", "RepCree", 2 }, { "ftpClient", "MakeDir", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REP_SUPPRIME[] = {
    { "ftpClient", "RepSupprime", 2 }, { "ftpClient", "RemoveDir", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_REP_EN_COURS[] = {
    { "ftpClient", "RepEnCours", -1 }, { "ftpClient", "CurrentDir", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ENVOIE[] = {
    { "ftpClient", "Envoie", -1 }, { "ftpClient", "Send", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_RECUPERE[] = {
    { "ftpClient", "Recupere", 3 }, { "ftpClient", "Retrieve", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_RECUPERE_FICHIER[] = {
    { "ftpClient", "RecupereFichier", -1 }, { "ftpClient", "RetrieveFile", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_LISTE_FICHIER[] = {
    { "ftpClient", "ListeFichier", 3 }, { "ftpClient", "ListFile", 3 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_TAILLE[] = {
    { "ftpClient", "Taille", 2 }, { "ftpClient", "Size", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DATE[] = {
    { "ftpClient", "Date", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_HEURE[] = {
    { "ftpClient", "Heure", -1 }, { "ftpClient", "Time", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_DATEHEURE[] = {
    { "ftpClient", "DateHeure", -1 }, { "ftpClient", "DateTime", -1 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_ATTRIBUT[] = {
    { "ftpClient", "Attribut", 2 }, { "ftpClient", "Attribute", 2 }, { NULL, NULL, 0 }
};
static const OwlMethodBinding MB_COMMANDE[] = {
    { "ftpClient", "Commande", 2 }, { "ftpClient", "Command", 2 }, { NULL, NULL, 0 }
};

/* ============================================================
 * Signatures.
 * ============================================================ */

static const OwlParam p_connecte[] = {
    {"serveur", "chaîne", "nom d'hôte ou IP du serveur, éventuellement préfixé \"ftps://\" (chiffré d'emblée), \"ftpes://\" (chiffré après négociation) ou \"sftp://\" (tunnel SSH, port 22 par défaut)"},
    {"utilisateur", "chaîne", "nom d'utilisateur (vide = anonyme)"},
    {"motDePasse", "chaîne", "mot de passe"},
    {"port", "entier", "port du canal de contrôle (optionnel, défaut 21 en FTP, 22 en SFTP)"},
    {"mode", "entier", "mode du canal de données (constante ftpMode*, optionnel, défaut ftpModePassif)", &E_ftpMode},
    {"optionsSécurité", "entier", "masque OU-binaire de constantes ftpIgnore* relâchant la vérification du certificat (FTPS/FTPES) ou de la clé d'hôte (SFTP, ftpIgnoreHôteInconnu) (optionnel, défaut vérification complète)"},
    {"cléPrivée", "chaîne", "chemin d'un fichier de clé privée OpenSSH pour l'authentification SFTP par clé, au lieu du mot de passe (optionnel)"},
    {"motDePasseClé", "chaîne", "phrase de passe protégeant la clé privée (optionnel, vide si la clé n'est pas chiffrée)"},
    {0}
};
static const OwlSignature sig_connecte[] = {
    {3, p_connecte, "ftpClient", "connexion ouverte (propriétés relisibles), ou Null en cas d'échec (voir ErreurInfo)"},
    {4, p_connecte, "ftpClient", "connexion ouverte sur le port indiqué"},
    {5, p_connecte, "ftpClient", "connexion ouverte sur le port et le mode indiqués"},
    {6, p_connecte, "ftpClient", "connexion (sécurisée si préfixe ftps://ftpes://sftp://) avec relâchement ciblé de la vérification"},
    {7, p_connecte, "ftpClient", "connexion SFTP (préfixe sftp://) authentifiée par clé privée OpenSSH"},
    {8, p_connecte, "ftpClient", "connexion SFTP par clé privée protégée par une phrase de passe"},
    {0}
};

static const OwlParam p_cnx[] = { {"cnx", "ftpClient", "connexion FTP"}, {0} };
static const OwlSignature sig_deconnecte[] = { {1, p_cnx, "booléen", "vrai si la déconnexion a réussi"}, {0} };

static const OwlParam p_cnx_chemin[] = {
    {"cnx", "ftpClient", "connexion FTP"}, {"chemin", "chaîne", "chemin distant"}, {0}
};
static const OwlSignature sig_existe[]   = { {2, p_cnx_chemin, "booléen", "vrai si le fichier distant existe"}, {0} };
static const OwlSignature sig_supprime[] = { {2, p_cnx_chemin, "booléen", "vrai si le fichier a été supprimé"}, {0} };
static const OwlSignature sig_rep_cree[] = { {2, p_cnx_chemin, "booléen", "vrai si le répertoire a été créé"}, {0} };
static const OwlSignature sig_rep_supprime[] = { {2, p_cnx_chemin, "booléen", "vrai si le répertoire a été supprimé"}, {0} };

static const OwlParam p_renomme[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"source", "chaîne", "chemin distant actuel"},
    {"destination", "chaîne", "nouveau chemin distant"}, {0}
};
static const OwlSignature sig_renomme[] = { {3, p_renomme, "booléen", "vrai si le renommage a réussi"}, {0} };

static const OwlParam p_rep_lecture[] = { {"cnx", "ftpClient", "connexion FTP"}, {0} };
static const OwlParam p_rep_modif[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"chemin", "chaîne", "répertoire distant cible (\"..\" pour remonter)"}, {0}
};
static const OwlSignature sig_rep_en_cours[] = {
    {1, p_rep_lecture, "chaîne",  "répertoire courant distant"},
    {2, p_rep_modif,   "booléen", "vrai si le changement de répertoire a réussi"},
    {0}
};

static const OwlParam p_envoie[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"local", "chaîne", "chemin du fichier local à envoyer"},
    {"distant", "chaîne", "chemin distant de destination"},
    {"progression", "procédure", "cb(tailleTotale, tailleTransférée) -> booléen (optionnel)"},
    {"mode", "entier", "type de transfert (constante ftpMode ASCII/Binaire, optionnel, défaut ftpModeBinaire)", &E_ftpModeTransfert},
    {0}
};
static const OwlSignature sig_envoie[] = {
    {3, p_envoie, "booléen", "vrai si l'envoi a réussi"},
    {4, p_envoie, "booléen", "vrai si l'envoi a réussi (avec callback de progression)"},
    {5, p_envoie, "booléen", "vrai si l'envoi a réussi (mode de transfert imposé)"},
    {0}
};

static const OwlParam p_recupere[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"distant", "chaîne", "chemin distant à récupérer"},
    {"local", "chaîne", "chemin local de destination"}, {0}
};
static const OwlSignature sig_recupere[] = { {3, p_recupere, "booléen", "vrai si la récupération a réussi"}, {0} };

static const OwlParam p_recupere_fichier[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"distant", "chaîne", "chemin distant du fichier"},
    {"local", "chaîne", "chemin local de destination"},
    {"progression", "procédure", "cb(tailleTotale, tailleTransférée) -> booléen (optionnel)"},
    {"mode", "entier", "type de transfert (constante ftpMode ASCII/Binaire, optionnel, défaut ftpModeBinaire)", &E_ftpModeTransfert},
    {0}
};
static const OwlSignature sig_recupere_fichier[] = {
    {3, p_recupere_fichier, "booléen", "vrai si la récupération a réussi"},
    {4, p_recupere_fichier, "booléen", "vrai si la récupération a réussi (avec callback)"},
    {5, p_recupere_fichier, "booléen", "vrai si la récupération a réussi (mode imposé)"},
    {0}
};

static const OwlParam p_liste[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"masque", "chaîne", "chemin/masque distant (ex \"/depot/*.csv\")"},
    {"rappel", "procédure", "cb(nom, taille, attribut, date, heure, param) -> booléen"}, {0}
};
static const OwlSignature sig_liste[] = { {3, p_liste, "entier", "nombre d'entrées listées"}, {0} };

static const OwlParam p_cnx_fichier[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"fichier", "chaîne", "chemin du fichier distant"}, {0}
};
static const OwlSignature sig_taille[] = {
    {2, p_cnx_fichier, "entier", "taille du fichier distant en octets, -1 en cas d'erreur"}, {0}
};
static const OwlSignature sig_attribut[] = {
    {2, p_cnx_fichier, "chaîne", "attributs portables (\"R\" lecture seule, \"D\" répertoire), \"ERR\" en cas d'erreur"}, {0}
};

static const OwlParam p_meta_date[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"fichier", "chaîne", "chemin du fichier distant"},
    {"typeDate", "entier", "type de date lu (constante ftpTypeDate, optionnel, défaut ftpModification)", &E_ftpTypeDate},
    {0}
};
static const OwlSignature sig_date[] = {
    {2, p_meta_date, "date", "date de dernière modification du fichier distant"},
    {3, p_meta_date, "date", "date du fichier distant pour le type demandé"},
    {0}
};
static const OwlSignature sig_heure[] = {
    {2, p_meta_date, "heure", "heure de dernière modification du fichier distant"},
    {3, p_meta_date, "heure", "heure du fichier distant pour le type demandé"},
    {0}
};
static const OwlSignature sig_dateheure[] = {
    {2, p_meta_date, "dateheure", "date et heure de dernière modification du fichier distant"},
    {3, p_meta_date, "dateheure", "date et heure du fichier distant pour le type demandé"},
    {0}
};

static const OwlParam p_commande[] = {
    {"cnx", "ftpClient", "connexion FTP"},
    {"commande", "chaîne", "commande FTP brute à envoyer sur le canal de contrôle"}, {0}
};
static const OwlSignature sig_commande[] = {
    {2, p_commande, "booléen", "vrai si le serveur a exécuté la commande (code de succès)"}, {0}
};

/* ============================================================
 * Definitions natives.
 * ============================================================ */

#define F_DEF(fr, fr_ascii, en, df, de, sig, fn, mb) \
    { .canonical = (fr), .canonicalAscii = (fr_ascii), .nameEN = (en), \
      .category = "FTP", .descFR = (df), .descEN = (de), \
      .signatures = (sig), .impl = (fn), .methodBindings = (mb) }

static const OwlNativeDef DEFS_FTP[] = {
    F_DEF("FTPConnecte", NULL, "FTPConnect",
        "Ouvre une connexion vers un serveur de fichiers — FTP en clair, sécurisée par TLS via un préfixe \"ftps://\" (implicite) ou \"ftpes://\" (explicite), ou SFTP via un tunnel SSH avec le préfixe \"sftp://\" (port 22 par défaut, authentification par mot de passe ou par clé privée OpenSSH) — et retourne un objet ftpClient dont les propriétés se relisent après coup.",
        "Opens a connection to a file server — clear FTP, TLS-secured through a \"ftps://\" (implicit) or \"ftpes://\" (explicit) prefix, or SFTP over an SSH tunnel with the \"sftp://\" prefix (port 22 by default, password or OpenSSH private-key authentication) — and returns a ftpClient object whose properties can be read back.",
        sig_connecte, fn_ftp_connecte, NULL),
    F_DEF("FTPDéconnecte", "FTPDeconnecte", "FTPDisconnect",
        "Ferme une connexion FTP ouverte.",
        "Closes an open FTP connection.",
        sig_deconnecte, fn_ftp_deconnecte, MB_DECONNECTE),
    F_DEF("FTPFichierExiste", NULL, "FTPFileExists",
        "Teste l'existence d'un fichier sur le serveur distant.",
        "Tests whether a file exists on the remote server.",
        sig_existe, fn_ftp_fichier_existe, MB_FICHIER_EXISTE),
    F_DEF("FTPSupprimeFichier", NULL, "FTPDeleteFile",
        "Supprime un fichier sur le serveur distant.",
        "Deletes a file on the remote server.",
        sig_supprime, fn_ftp_supprime_fichier, MB_SUPPRIME_FICHIER),
    F_DEF("FTPRenommeFichier", NULL, "FTPRenameFile",
        "Renomme ou déplace un fichier sur le serveur distant.",
        "Renames or moves a file on the remote server.",
        sig_renomme, fn_ftp_renomme_fichier, MB_RENOMME_FICHIER),
    F_DEF("FTPRepCrée", "FTPRepCree", "FTPMakeDir",
        "Crée un répertoire sur le serveur distant.",
        "Creates a directory on the remote server.",
        sig_rep_cree, fn_ftp_rep_cree, MB_REP_CREE),
    F_DEF("FTPRepSupprime", NULL, "FTPRemoveDir",
        "Supprime un répertoire sur le serveur distant.",
        "Removes a directory on the remote server.",
        sig_rep_supprime, fn_ftp_rep_supprime, MB_REP_SUPPRIME),
    F_DEF("FTPRepEnCours", NULL, "FTPCurrentDir",
        "Lit le répertoire courant distant (un paramètre), ou le modifie (deux paramètres).",
        "Reads the remote current directory (one argument), or changes it (two arguments).",
        sig_rep_en_cours, fn_ftp_rep_en_cours, MB_REP_EN_COURS),
    F_DEF("FTPEnvoie", NULL, "FTPSend",
        "Envoie un fichier local vers le serveur distant.",
        "Sends a local file to the remote server.",
        sig_envoie, fn_ftp_envoie, MB_ENVOIE),
    F_DEF("FTPRécupère", "FTPRecupere", "FTPRetrieve",
        "Récupère un répertoire distant complet vers le poste local.",
        "Retrieves a whole remote directory to the local machine.",
        sig_recupere, fn_ftp_recupere, MB_RECUPERE),
    F_DEF("FTPRécupèreFichier", "FTPRecupereFichier", "FTPRetrieveFile",
        "Récupère un fichier distant vers le poste local.",
        "Retrieves a remote file to the local machine.",
        sig_recupere_fichier, fn_ftp_recupere_fichier, MB_RECUPERE_FICHIER),
    F_DEF("FTPListeFichier", NULL, "FTPListFile",
        "Liste les fichiers distants correspondant à un masque, en appelant un rappel pour chaque entrée.",
        "Lists remote files matching a mask, calling a callback for each entry.",
        sig_liste, fn_ftp_liste_fichier, MB_LISTE_FICHIER),
    F_DEF("FTPTaille", NULL, "FTPSize",
        "Retourne la taille en octets d'un fichier distant, ou -1 en cas d'erreur.",
        "Returns the size in bytes of a remote file, or -1 on error.",
        sig_taille, fn_ftp_taille, MB_TAILLE),
    F_DEF("FTPDate", NULL, "FTPDate",
        "Retourne la date (création, modification ou accès) d'un fichier distant, exprimée en UTC.",
        "Returns the date (creation, modification or access) of a remote file, expressed in UTC.",
        sig_date, fn_ftp_date, MB_DATE),
    F_DEF("FTPHeure", NULL, "FTPTime",
        "Retourne l'heure (création, modification ou accès) d'un fichier distant, exprimée en UTC.",
        "Returns the time (creation, modification or access) of a remote file, expressed in UTC.",
        sig_heure, fn_ftp_heure, MB_HEURE),
    F_DEF("FTPDateHeure", NULL, "FTPDateTime",
        "Retourne la date et l'heure (création, modification ou accès) d'un fichier distant, exprimées en UTC.",
        "Returns the date and time (creation, modification or access) of a remote file, expressed in UTC.",
        sig_dateheure, fn_ftp_dateheure, MB_DATEHEURE),
    F_DEF("FTPAttribut", NULL, "FTPAttribute",
        "Retourne les attributs portables d'un fichier distant sous forme de chaîne (\"R\" lecture seule, \"D\" répertoire, \"\" si aucun), \"ERR\" en cas d'erreur.",
        "Returns the portable attributes of a remote file as a string (\"R\" read-only, \"D\" directory, \"\" if none), \"ERR\" on error.",
        sig_attribut, fn_ftp_attribut, MB_ATTRIBUT),
    F_DEF("FTPCommande", NULL, "FTPCommand",
        "Envoie une commande FTP brute sur le canal de contrôle et indique si le serveur l'a exécutée.",
        "Sends a raw FTP command on the control channel and reports whether the server ran it.",
        sig_commande, fn_ftp_commande, MB_COMMANDE),
};

#undef F_DEF

/* ============================================================
 * Init.
 * ============================================================ */

void stdlib_ftp_init(void) {
    owl_registry_register_module(DEFS_FTP,
        (int)(sizeof(DEFS_FTP) / sizeof(DEFS_FTP[0])));
    owl_registry_register_enums(FTP_ENUMS,
        (int)(sizeof(FTP_ENUMS) / sizeof(FTP_ENUMS[0])));
    owl_registry_register_consts(FTP_CONSTS,
        (int)(sizeof(FTP_CONSTS) / sizeof(FTP_CONSTS[0])));
}
