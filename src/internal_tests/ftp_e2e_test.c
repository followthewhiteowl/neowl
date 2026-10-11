// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test end-to-end FTP : coeur protocole VM-free (owl_ftp_*) contre un
 * serveur mock STATEFUL multi-tours. Le scenario owl test mocke chaque
 * native individuellement — il ne touche jamais au fil. C'est ICI que le
 * protocole reel est prouve : auth, PASV (2e canal TCP), MLSD listing avec
 * rappel par entree, RETR octet-exact, STOR, callbacks de progression.
 *
 * Aucun vm_init : le coeur ne connait ni la VM ni le GC (il remonte octets
 * et entrees par callbacks C). */

#include "stdlib_ftp.h"
#include "ftp_mock_server.h"
#include "net_tcp_tls.h"   /* OwlTlsVerifyLevel (NONE/CHAIN/FULL) */
#include "sentinels.h"     /* OWL_FTP_IGNORE_* (masque optionsSécurité) */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Ouverture claire (vérif complète) — la forme historique des tests non
 * sécurisés, factorisée depuis l'extension de la signature owl_ftp_open. */
static OwlFtpClient* e2e_open_clear(FtpMockServer* m, const char* user,
                                    const char* pass, char* err, size_t errlen) {
    return owl_ftp_open("127.0.0.1", ftp_mock_server_port(m), user, pass,
                        OWL_FTP_ENC_CLEAR, OWL_TLS_VERIFY_FULL, err, errlen);
}

static int g_pass = 0;
static int g_fail = 0;

#define FTP_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

/* ---- Collecteur d'entrees de listage ---- */
typedef struct {
    char    names[16][256];
    int64_t sizes[16];
    char    attrs[16][32];
    int     count;
    int     stop_after;   /* 0 = ne jamais interrompre ; N = false a la Nieme */
} EntrySink;

static bool entry_cb(void* ctx, const char* name, int64_t size,
                     const char* attribut, const char* date, const char* heure) {
    (void)date; (void)heure;
    EntrySink* s = (EntrySink*)ctx;
    if (s->count < 16) {
        snprintf(s->names[s->count], sizeof(s->names[0]), "%s", name);
        s->sizes[s->count] = size;
        snprintf(s->attrs[s->count], sizeof(s->attrs[0]), "%s", attribut);
    }
    s->count++;
    if (s->stop_after > 0 && s->count >= s->stop_after) return false;
    return true;
}

/* ---- Puits d'octets pour RETR ---- */
typedef struct { char buf[65536]; size_t len; } ByteSink;
static bool byte_sink(void* ctx, const char* buf, size_t len) {
    ByteSink* s = (ByteSink*)ctx;
    if (s->len + len > sizeof(s->buf)) return false;
    memcpy(s->buf + s->len, buf, len);
    s->len += len;
    return true;
}

/* ---- Progression ---- */
typedef struct { int calls; bool abort_now; int64_t last_total; } ProgCtx;
static bool prog_cb(void* ctx, int64_t total, int64_t done) {
    (void)done;
    ProgCtx* p = (ProgCtx*)ctx;
    p->calls++;
    p->last_total = total;
    if (p->abort_now) return false;
    return true;
}

/* ================================================================ */

static void test_connect_auth_pasv(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.listing = "type=file;size=1; a.txt\r\n";
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    FTP_CHECK(m && ftp_mock_server_is_ready(m), "mock FTP ready");
    if (!m || !ftp_mock_server_is_ready(m)) { if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "alice", "secret42", err, sizeof(err));
    FTP_CHECK(c != NULL, "owl_ftp_open reussit");
    FTP_CHECK(c && owl_ftp_has_mlsd(c), "FEAT annonce MLSD");

    EntrySink s; memset(&s, 0, sizeof(s));
    int n = owl_ftp_list(c, "/depot", entry_cb, &s);
    FTP_CHECK(n >= 1, "listage renvoie >= 1 entree (PASV emprunte)");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    FTP_CHECK(strcmp(ftp_mock_server_last_user(m), "alice") == 0, "USER recu par le serveur");
    FTP_CHECK(strcmp(ftp_mock_server_last_pass(m), "secret42") == 0, "PASS recu par le serveur");
    FTP_CHECK(ftp_mock_server_saw_pasv(m) >= 1, "le serveur a vu PASV (2e canal negocie)");
    ftp_mock_server_free(m);
}

static void test_mlsd_listing_count(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    /* cdir/pdir doivent etre ignores ; 3 entrees reelles. */
    cfg.listing =
        "type=cdir;modify=20260101000000; .\r\n"
        "type=pdir;modify=20260101000000; ..\r\n"
        "type=file;size=100;modify=20260102120000; rapport.csv\r\n"
        "type=file;size=200;modify=20260103130000; notes.txt\r\n"
        "type=dir;modify=20260104140000; archives\r\n";
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    EntrySink s; memset(&s, 0, sizeof(s));
    int n = owl_ftp_list(c, "/depot", entry_cb, &s);
    FTP_CHECK(n == 3, "MLSD : 3 entrees reelles (cdir/pdir ignores)");
    FTP_CHECK(s.count == 3, "le rappel est invoque 3 fois");
    FTP_CHECK(strcmp(s.names[0], "rapport.csv") == 0, "1re entree = rapport.csv");
    FTP_CHECK(s.sizes[0] == 100, "taille de rapport.csv = 100");
    FTP_CHECK(strcmp(s.attrs[2], "dir") == 0, "3e entree = repertoire");
    FTP_CHECK(strcmp(ftp_mock_server_last_list(m), "/depot") == 0, "chemin de listage recu");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

static void test_mlsd_listing_early_stop(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.listing =
        "type=file;size=100; rapport.csv\r\n"
        "type=file;size=200; notes.txt\r\n"
        "type=file;size=300; data.bin\r\n";
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    EntrySink s; memset(&s, 0, sizeof(s));
    s.stop_after = 1;   /* le rappel renvoie false des la 1re entree */
    int n = owl_ftp_list(c, "/depot", entry_cb, &s);
    FTP_CHECK(n == 1, "le parcours s'arrete a la 1re entree (RENVOYER Faux)");
    FTP_CHECK(s.count == 1, "le rappel n'est invoque qu'une fois");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

static void test_retr_octet_exact(void) {
    /* Contenu binaire avec octet NUL et octets non-texte. */
    static const char body[] = { 'O','W','L', 0x00, (char)0xFF, 'z','\n','Z', 0x01 };
    const size_t body_len = sizeof(body);

    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.retr_body = body;
    cfg.retr_body_len = body_len;
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    ByteSink sink; sink.len = 0;
    ProgCtx pg; memset(&pg, 0, sizeof(pg));
    bool ok = owl_ftp_retr(c, "/depot/data.bin", false, byte_sink, &sink,
                           prog_cb, &pg, err, sizeof(err));
    FTP_CHECK(ok, "RETR reussit");
    FTP_CHECK(sink.len == body_len, "RETR : longueur exacte");
    FTP_CHECK(sink.len == body_len && memcmp(sink.buf, body, body_len) == 0,
              "RETR : contenu octet-exact (binaire, NUL inclus)");
    FTP_CHECK(pg.calls >= 1, "progression RETR appelee");
    FTP_CHECK(pg.last_total == (int64_t)body_len, "progression : taille totale de SIZE");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    FTP_CHECK(strcmp(ftp_mock_server_last_retr(m), "/depot/data.bin") == 0, "chemin RETR recu");
    ftp_mock_server_free(m);
}

static void test_stor_octet_exact(void) {
    static const char payload[] = { 'u','p', 0x00, (char)0xAB, 'l','o','a','d', 0x02 };
    const size_t payload_len = sizeof(payload);

    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    ProgCtx pg; memset(&pg, 0, sizeof(pg));
    bool ok = owl_ftp_stor(c, "/depot/upload.bin", false, payload, payload_len,
                           prog_cb, &pg, err, sizeof(err));
    FTP_CHECK(ok, "STOR reussit");
    FTP_CHECK(pg.calls >= 1, "progression STOR appelee");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    size_t got_len = 0;
    const char* got = ftp_mock_server_stor_data(m, &got_len);
    FTP_CHECK(got_len == payload_len, "STOR : longueur recue par le serveur exacte");
    FTP_CHECK(got_len == payload_len && memcmp(got, payload, payload_len) == 0,
              "STOR : contenu recu octet-exact (binaire, NUL inclus)");
    FTP_CHECK(strcmp(ftp_mock_server_last_stor(m), "/depot/upload.bin") == 0, "chemin STOR recu");
    ftp_mock_server_free(m);
}

static void test_progress_interruption(void) {
    static const char body[] = "quelques octets a telecharger";
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.retr_body = body;
    cfg.retr_body_len = strlen(body);
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    ByteSink sink; sink.len = 0;
    ProgCtx pg; memset(&pg, 0, sizeof(pg));
    pg.abort_now = true;   /* la progression renvoie false -> abandon */
    bool ok = owl_ftp_retr(c, "/depot/f.txt", false, byte_sink, &sink,
                           prog_cb, &pg, err, sizeof(err));
    FTP_CHECK(!ok, "RETR signale l'abandon quand la progression renvoie false");
    FTP_CHECK(pg.calls == 1, "la progression n'est appelee qu'une fois avant l'abandon");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

static void test_metadonnees(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.retr_body   = "0123456789";                 /* SIZE -> 10 */
    cfg.mdtm        = "20260315143045";
    cfg.mlst_facts  = "modify=20260315143045;create=20250101090000;type=file;";
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    int64_t sz = owl_ftp_size(c, "/depot/rapport.zip");
    FTP_CHECK(sz == 10, "SIZE renvoie la taille du fichier distant");

    char ts[16];
    bool okm = owl_ftp_mdtm(c, "/depot/rapport.zip", ts, sizeof(ts));
    FTP_CHECK(okm && strcmp(ts, "20260315143045") == 0, "MDTM renvoie l'horodatage de modification");

    char facts[512];
    bool okl = owl_ftp_mlst(c, "/depot/rapport.zip", facts, sizeof(facts));
    FTP_CHECK(okl, "MLST reussit");
    FTP_CHECK(okl && strstr(facts, "create=20250101090000") != NULL, "MLST renvoie le fact create");
    FTP_CHECK(okl && strstr(facts, "type=file") != NULL, "MLST renvoie le fact type");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    FTP_CHECK(strcmp(ftp_mock_server_last_mdtm(m), "/depot/rapport.zip") == 0, "chemin MDTM recu par le serveur");
    FTP_CHECK(strcmp(ftp_mock_server_last_mlst(m), "/depot/rapport.zip") == 0, "chemin MLST recu par le serveur");
    ftp_mock_server_free(m);
}

static void test_metadonnees_absent(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    /* mdtm/mlst_facts non configures -> le serveur repond 550. */
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
    if (!c) { FTP_CHECK(false, "open"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

    char ts[16];
    FTP_CHECK(!owl_ftp_mdtm(c, "/depot/absent.zip", ts, sizeof(ts)), "MDTM d'un fichier absent echoue (550)");
    char facts[512];
    FTP_CHECK(!owl_ftp_mlst(c, "/depot/absent.zip", facts, sizeof(facts)), "MLST d'un fichier absent echoue (550)");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

/* Prouve la construction de la chaine d'attributs PORTABLE depuis les facts
 * MLST reels (le scenario owl test mocke FTPAttribut et ne teste donc pas ce
 * parsing) : un fichier en lecture seule contient "R", un repertoire "D". */
static void test_attribut_portable(void) {
    /* Fichier en lecture seule : perm=r (aucune lettre d'ecriture) -> "R". */
    {
        FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
        cfg.mlst_facts = "type=file;size=10;perm=r;modify=20260315143045;";
        FtpMockServer* m = ftp_mock_server_start(&cfg);
        if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup ro"); if (m) ftp_mock_server_free(m); return; }
        char err[256];
        OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
        if (!c) { FTP_CHECK(false, "open ro"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

        char facts[512];
        bool okl = owl_ftp_mlst(c, "/depot/rapport.zip", facts, sizeof(facts));
        FTP_CHECK(okl, "MLST fichier lecture seule");
        char attr[8]; owl_ftp_attr_from_facts(facts, attr, sizeof(attr));
        FTP_CHECK(strchr(attr, 'R') != NULL, "fichier lecture seule -> attribut contient R");
        FTP_CHECK(strchr(attr, 'D') == NULL, "fichier -> pas de D");

        owl_ftp_close(c);
        ftp_mock_server_join(m);
        ftp_mock_server_free(m);
    }
    /* Repertoire inscriptible : type=dir + perm avec lettres d'ecriture -> "D". */
    {
        FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
        cfg.mlst_facts = "type=dir;perm=elcmw;modify=20260104140000;";
        FtpMockServer* m = ftp_mock_server_start(&cfg);
        if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock setup dir"); if (m) ftp_mock_server_free(m); return; }
        char err[256];
        OwlFtpClient* c = e2e_open_clear(m, "u", "p", err, sizeof(err));
        if (!c) { FTP_CHECK(false, "open dir"); ftp_mock_server_join(m); ftp_mock_server_free(m); return; }

        char facts[512];
        bool okl = owl_ftp_mlst(c, "/depot/archives", facts, sizeof(facts));
        FTP_CHECK(okl, "MLST repertoire");
        char attr[8]; owl_ftp_attr_from_facts(facts, attr, sizeof(attr));
        FTP_CHECK(strchr(attr, 'D') != NULL, "repertoire -> attribut contient D");
        FTP_CHECK(strchr(attr, 'R') == NULL, "repertoire inscriptible -> pas de R");

        owl_ftp_close(c);
        ftp_mock_server_join(m);
        ftp_mock_server_free(m);
    }
}

/* ================================================================
 * FTPS / FTPES sur TLS. Le scenario owl test mocke FTPConnecte au niveau
 * native : il prouve la SURFACE (props/constantes/signature), pas le fil.
 * La preuve protocole vit ICI, contre un mock TLS-capable.
 * ================================================================ */

/* Parsing du prefixe de securite : pur, sans serveur. */
static void test_prefixe_securite(void) {
    char host[256];
    FTP_CHECK(owl_ftp_parse_prefix("ftps://ftp.exemple.test", host, sizeof(host)) == OWL_FTP_ENC_FTPS,
              "ftps:// -> chiffrement implicite");
    FTP_CHECK(strcmp(host, "ftp.exemple.test") == 0, "ftps:// -> host sans prefixe");
    FTP_CHECK(owl_ftp_parse_prefix("ftpes://srv", host, sizeof(host)) == OWL_FTP_ENC_FTPES,
              "ftpes:// -> chiffrement explicite");
    FTP_CHECK(strcmp(host, "srv") == 0, "ftpes:// -> host sans prefixe");
    FTP_CHECK(owl_ftp_parse_prefix("FTPS://Casse", host, sizeof(host)) == OWL_FTP_ENC_FTPS,
              "prefixe casse-insensible");
    FTP_CHECK(owl_ftp_parse_prefix("ftp.clair.test", host, sizeof(host)) == OWL_FTP_ENC_CLEAR,
              "sans prefixe -> clair");
    FTP_CHECK(strcmp(host, "ftp.clair.test") == 0, "sans prefixe -> host inchange");
    FTP_CHECK(strcmp(owl_ftp_encryption_label(OWL_FTP_ENC_FTPS),  "FTPS")  == 0, "libelle FTPS");
    FTP_CHECK(strcmp(owl_ftp_encryption_label(OWL_FTP_ENC_FTPES), "FTPES") == 0, "libelle FTPES");
    FTP_CHECK(strcmp(owl_ftp_encryption_label(OWL_FTP_ENC_CLEAR), "")      == 0, "libelle clair vide");
}

/* FTPS implicite : TLS d'emblee, PBSZ/PROT P, listing chiffre. Le certificat
 * du mock est auto-signe -> on ouvre en VERIFY_NONE (= ftpIgnoreCertificatInvalide). */
static void test_ftps_implicite(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.tls_mode = 1;   /* FTPS implicite */
    cfg.listing  = "type=file;size=42; secret.txt\r\n";
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock TLS setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = owl_ftp_open("127.0.0.1", ftp_mock_server_port(m), "u", "p",
                                   OWL_FTP_ENC_FTPS, OWL_TLS_VERIFY_NONE, err, sizeof(err));
    FTP_CHECK(c != NULL, "FTPS : connexion TLS d'emblee reussit");
    if (!c) { ftp_mock_server_join(m); ftp_mock_server_free(m); return; }
    FTP_CHECK(owl_ftp_encryption(c) == OWL_FTP_ENC_FTPS, "FTPS : chiffrement = FTPS");

    EntrySink s; memset(&s, 0, sizeof(s));
    int n = owl_ftp_list(c, "/depot", entry_cb, &s);
    FTP_CHECK(n == 1, "FTPS : listing chiffre remonte 1 entree");
    FTP_CHECK(n == 1 && strcmp(s.names[0], "secret.txt") == 0, "FTPS : entree lue sur canal chiffre");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    FTP_CHECK(ftp_mock_server_ctrl_was_tls(m), "FTPS : handshake TLS sur le canal de controle");
    FTP_CHECK(ftp_mock_server_data_was_tls(m), "FTPS : handshake TLS sur le canal de donnees");
    FTP_CHECK(ftp_mock_server_saw_pbsz(m), "FTPS : PBSZ recu");
    FTP_CHECK(strcmp(ftp_mock_server_last_prot(m), "P") == 0, "FTPS : PROT P recu");
    FTP_CHECK(!ftp_mock_server_saw_auth_tls(m), "FTPS : pas d'AUTH TLS (implicite)");
    ftp_mock_server_free(m);
}

/* FTPES explicite : connexion claire, AUTH TLS AVANT login, puis chiffre. */
static void test_ftpes_explicite(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.tls_mode = 2;   /* FTPES explicite */
    cfg.retr_body = "chiffre";
    cfg.retr_body_len = 7;
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock TLS setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = owl_ftp_open("127.0.0.1", ftp_mock_server_port(m), "alice", "secret42",
                                   OWL_FTP_ENC_FTPES, OWL_TLS_VERIFY_NONE, err, sizeof(err));
    FTP_CHECK(c != NULL, "FTPES : connexion + AUTH TLS reussit");
    if (!c) { ftp_mock_server_join(m); ftp_mock_server_free(m); return; }
    FTP_CHECK(owl_ftp_encryption(c) == OWL_FTP_ENC_FTPES, "FTPES : chiffrement = FTPES");

    ByteSink sink; sink.len = 0;
    bool ok = owl_ftp_retr(c, "/depot/f.txt", false, byte_sink, &sink, NULL, NULL, err, sizeof(err));
    FTP_CHECK(ok && sink.len == 7, "FTPES : transfert chiffre octet-exact");

    owl_ftp_close(c);
    ftp_mock_server_join(m);
    FTP_CHECK(ftp_mock_server_saw_auth_tls(m), "FTPES : AUTH TLS negocie");
    FTP_CHECK(ftp_mock_server_ctrl_was_tls(m), "FTPES : handshake TLS sur le canal de controle");
    FTP_CHECK(ftp_mock_server_data_was_tls(m), "FTPES : handshake TLS sur le canal de donnees");
    /* Le login n'est envoye qu'apres AUTH TLS : le serveur l'a bien recu chiffre. */
    FTP_CHECK(strcmp(ftp_mock_server_last_user(m), "alice") == 0, "FTPES : USER recu apres AUTH TLS");
    ftp_mock_server_free(m);
}

/* Certificat invalide (auto-signe) NON ignore -> connexion echoue en
 * soft-error (owl_ftp_open renvoie NULL, err renseigne, pas d'abort). */
static void test_ftps_cert_invalide_rejete(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.tls_mode = 1;
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock TLS setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256]; err[0] = '\0';
    OwlFtpClient* c = owl_ftp_open("127.0.0.1", ftp_mock_server_port(m), "u", "p",
                                   OWL_FTP_ENC_FTPS, OWL_TLS_VERIFY_FULL, err, sizeof(err));
    FTP_CHECK(c == NULL, "cert auto-signe + verif complete -> connexion refusee");
    FTP_CHECK(err[0] != '\0', "l'echec renseigne un detail (soft-error)");
    if (c) owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

/* Meme cert invalide, mais VERIFY_NONE (= ftpIgnoreCertificatInvalide cote
 * native) -> la connexion aboutit. */
static void test_ftps_cert_invalide_ignore(void) {
    FtpMockConfig cfg; ftp_mock_config_defaut(&cfg);
    cfg.tls_mode = 1;
    FtpMockServer* m = ftp_mock_server_start(&cfg);
    if (!m || !ftp_mock_server_is_ready(m)) { FTP_CHECK(false, "mock TLS setup"); if (m) ftp_mock_server_free(m); return; }

    char err[256];
    OwlFtpClient* c = owl_ftp_open("127.0.0.1", ftp_mock_server_port(m), "u", "p",
                                   OWL_FTP_ENC_FTPS, OWL_TLS_VERIFY_NONE, err, sizeof(err));
    FTP_CHECK(c != NULL, "cert auto-signe ignore -> connexion aboutit");
    if (c) owl_ftp_close(c);
    ftp_mock_server_join(m);
    ftp_mock_server_free(m);
}

/* SFTP (Lot D) hors-ligne : le SSH ne se simule pas en memoire (pas de mock
 * in-process). On prouve ici ce qui est prouvable SANS serveur vivant — parsing
 * du prefixe, libelle, selection du mode d'auth, mapping du masque known-hosts,
 * et surtout le CONTRAT soft-error : une connexion sftp:// vers un port mort
 * doit remonter NULL proprement (pas de crash, pas de blocage), err renseigne.
 * Le vrai handshake + get/put est valide manuellement contre un serveur reel. */
static void test_sftp_offline(void) {
    char host[256];

    /* Parsing du prefixe sftp:// : mode SFTP + host detache, casse-insensible. */
    FTP_CHECK(owl_ftp_parse_prefix("sftp://serveur.test", host, sizeof(host)) == OWL_FTP_ENC_SFTP,
              "sftp:// -> mode SFTP");
    FTP_CHECK(strcmp(host, "serveur.test") == 0, "sftp:// -> host sans prefixe");
    FTP_CHECK(owl_ftp_parse_prefix("SFTP://Casse", host, sizeof(host)) == OWL_FTP_ENC_SFTP,
              "SFTP:// casse-insensible");
    FTP_CHECK(strcmp(owl_ftp_encryption_label(OWL_FTP_ENC_SFTP), "SFTP") == 0, "libelle SFTP");

    /* Selection du mode d'authentification selon le chemin de cle privee. */
    FTP_CHECK(owl_sftp_uses_key_auth(NULL) == false, "cle absente (NULL) -> auth mot de passe");
    FTP_CHECK(owl_sftp_uses_key_auth("")   == false, "cle vide -> auth mot de passe");
    FTP_CHECK(owl_sftp_uses_key_auth("/home/u/.ssh/id_ed25519") == true,
              "cle fournie -> auth par cle");

    /* Mapping du masque optionsSécurité vers la politique known-hosts : le bit
     * ftpIgnoreHôteInconnu est distinct des motifs de certificat TLS. */
    FTP_CHECK((OWL_FTP_IGNORE_HOTE_INCONNU & OWL_FTP_IGNORE_CERT_INVALIDE) == 0 &&
              (OWL_FTP_IGNORE_HOTE_INCONNU & OWL_FTP_IGNORE_CERT_NOM) == 0,
              "ftpIgnoreHôteInconnu : bit disjoint des motifs certificat");
    FTP_CHECK(((OWL_FTP_IGNORE_HOTE_INCONNU | OWL_FTP_IGNORE_CERT_INVALIDE)
                  & OWL_FTP_IGNORE_HOTE_INCONNU) != 0,
              "ftpIgnoreHôteInconnu : combinable en OU-binaire");

    /* Contrat soft-error : connexion vers un port mort (loopback, port haut
     * inutilise -> refus TCP immediat). NULL propre, err renseigne, aucun
     * crash/blocage. allow_unknown=true : on ne bloque pas sur la cle d'hote,
     * la connexion TCP echoue AVANT tout handshake. */
    char err[256];
    bool verified = true;   /* doit etre remis a false par l'ouverture ratee */
    OwlFtpClient* c = owl_sftp_open("127.0.0.1", 53179, "u", "p", NULL, NULL,
                                    true, &verified, err, sizeof(err));
    FTP_CHECK(c == NULL, "sftp port mort -> NULL (soft-error)");
    FTP_CHECK(err[0] != '\0', "sftp port mort -> err renseigne");
    FTP_CHECK(verified == false, "sftp echec -> host_verified faux");

    /* Idem via le mode auth par cle (port mort : echoue avant l'auth, mais
     * exerce la branche de selection). */
    OwlFtpClient* c2 = owl_sftp_open("127.0.0.1", 53179, "u", "", "/tmp/absent_key",
                                     "phrase", true, NULL, err, sizeof(err));
    FTP_CHECK(c2 == NULL, "sftp cle privee + port mort -> NULL");
}

int ftp_e2e_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_connect_auth_pasv();
    test_mlsd_listing_count();
    test_mlsd_listing_early_stop();
    test_retr_octet_exact();
    test_stor_octet_exact();
    test_progress_interruption();
    test_metadonnees();
    test_metadonnees_absent();
    test_attribut_portable();

    /* FTPS / FTPES sur TLS. */
    test_prefixe_securite();
    test_ftps_implicite();
    test_ftpes_explicite();
    test_ftps_cert_invalide_rejete();
    test_ftps_cert_invalide_ignore();

    /* SFTP (Lot D) — surface hors-ligne + contrat soft-error. */
    test_sftp_offline();

    printf("ftp_e2e self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
