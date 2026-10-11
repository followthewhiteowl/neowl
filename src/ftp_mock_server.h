// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_ftp_mock_server_h
#define owl_ftp_mock_server_h

#include <stddef.h>

/* ================================================================
 * Mini-serveur FTP mock STATEFUL — pour tests CI deterministes.
 *
 * Le pair net_mock_peer existant est mono-payload / mono-accept : il ne
 * peut pas mener la conversation multi-tours d'un dialogue FTP (banniere,
 * USER/PASS, FEAT, PASV qui ANNONCE un 2e port, puis un transfert sur ce
 * canal de donnees). Ce serveur, modele sur sql_postgres_mock_server.c,
 * implemente juste assez du protocole cote serveur pour valider le client
 * OwlFtpClient :
 *   - banniere 220
 *   - USER (331) / PASS (230)
 *   - FEAT (211, annonce MLSD/UTF8 selon config)
 *   - TYPE (200), SIZE (213), MDTM (213), MLST (250 multiligne)
 *   - PASV (227, ouvre un canal de donnees ephemere loopback)
 *   - LIST / MLSD (150 -> corpus sur le canal de donnees -> 226)
 *   - RETR (150 -> contenu sur le canal de donnees -> 226)
 *   - STOR (150 -> lit le canal de donnees jusqu'a EOF -> 226)
 *   - QUIT (221)
 *
 * Lance en thread, port assigne dynamiquement (bind 0). Single-shot :
 * une connexion de controle, puis fin.
 * ================================================================ */

typedef struct FtpMockServer FtpMockServer;

typedef struct {
    int         advertise_mlsd;   /* 1 = FEAT annonce MLSD */
    const char* listing;          /* corps MLSD/LIST emis sur LIST/MLSD */
    const char* retr_body;        /* contenu emis sur RETR */
    size_t      retr_body_len;    /* longueur du contenu RETR (0 -> strlen) */
    const char* mdtm;             /* horodatage AAAAMMJJHHMMSS renvoye par MDTM (NULL -> 550) */
    const char* mlst_facts;       /* facts renvoyes par MLST (ex "modify=...;type=file;", NULL -> 550) */
    int         tls_mode;         /* 0 clair / 1 FTPS implicite / 2 FTPES explicite (AUTH TLS) */
} FtpMockConfig;

/* Config neutre : MLSD annonce, pas de listing ni de contenu. */
void ftp_mock_config_defaut(FtpMockConfig* cfg);

/* Demarre le thread serveur. Retourne NULL sur echec. Au retour, le port
 * est lisible des que ready == 1 (poller jusqu'a 2s). */
FtpMockServer* ftp_mock_server_start(const FtpMockConfig* cfg);

int  ftp_mock_server_port(const FtpMockServer* m);
int  ftp_mock_server_is_ready(const FtpMockServer* m);

/* Attend la fin du thread (le serveur traite UNE connexion puis sort). */
void ftp_mock_server_join(FtpMockServer* m);
void ftp_mock_server_free(FtpMockServer* m);

/* Inspection apres join. */
const char* ftp_mock_server_last_user(const FtpMockServer* m);
const char* ftp_mock_server_last_pass(const FtpMockServer* m);
const char* ftp_mock_server_last_retr(const FtpMockServer* m);
const char* ftp_mock_server_last_stor(const FtpMockServer* m);
const char* ftp_mock_server_last_list(const FtpMockServer* m);
const char* ftp_mock_server_last_mdtm(const FtpMockServer* m);
const char* ftp_mock_server_last_mlst(const FtpMockServer* m);
int         ftp_mock_server_saw_pasv(const FtpMockServer* m);
/* Octets recus lors d'un STOR (upload client -> serveur). */
const char* ftp_mock_server_stor_data(const FtpMockServer* m, size_t* out_len);

/* Preuve du chemin securise reellement emprunte (FTPS/FTPES). */
int         ftp_mock_server_ctrl_was_tls(const FtpMockServer* m);  /* handshake TLS sur le canal de controle */
int         ftp_mock_server_data_was_tls(const FtpMockServer* m);  /* handshake TLS sur un canal de donnees */
int         ftp_mock_server_saw_auth_tls(const FtpMockServer* m);  /* AUTH TLS recu (FTPES) */
int         ftp_mock_server_saw_pbsz(const FtpMockServer* m);      /* PBSZ recu */
const char* ftp_mock_server_last_prot(const FtpMockServer* m);     /* argument du dernier PROT ("P") */

#endif /* owl_ftp_mock_server_h */
