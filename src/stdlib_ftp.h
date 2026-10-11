// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_stdlib_ftp_h
#define owl_stdlib_ftp_h

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Client FTP en clair — coeur protocole VM-free (RFC 959 / 3659).
 *
 * Le canal de controle (conversation ligne a ligne) ET le canal de
 * donnees passif (PASV : 2e socket TCP ouvert par le client) vivent ici.
 * Le coeur ne connait NI la VM NI le GC : les transferts et le listage
 * remontent leurs octets/entrees via des callbacks C. Les natives OWL
 * (stdlib_ftp.c) marshalent ces callbacks vers une procedure OWL par
 * vm_call_reentrant. Cette separation rend le protocole testable
 * directement contre un serveur mock (src/internal_tests/ftp_e2e_test.c),
 * sans passer par la VM. */

typedef struct OwlFtpClient OwlFtpClient;

/* Mode de chiffrement du canal de controle FTP. Determine par le prefixe
 * du nom de serveur passe a FTPConnecte ("ftps://" / "ftpes://"). */
typedef enum {
    OWL_FTP_ENC_CLEAR = 0,   /* clair (aucun TLS) */
    OWL_FTP_ENC_FTPS  = 1,   /* implicite : TLS des la connexion (canal chiffre d'emblee) */
    OWL_FTP_ENC_FTPES = 2,   /* explicite : connexion claire puis AUTH TLS avant login */
    OWL_FTP_ENC_SFTP  = 3,   /* SFTP : transport SSH-2 (libssh2), pas du FTP-sur-TLS */
} OwlFtpEncryption;

/* Detache un prefixe optionnel "ftps://" / "ftpes://" / "sftp://" du nom de
 * serveur. Ecrit l'hote reel (prefixe retire) dans hostout et renvoie le mode
 * de transport/chiffrement detecte. Casse-insensible sur le prefixe. VM-free. */
OwlFtpEncryption owl_ftp_parse_prefix(const char* serveur, char* hostout, size_t hostsz);

/* Libelle relisible d'un mode : "FTPS", "FTPES", "SFTP" ou "" (clair). */
const char* owl_ftp_encryption_label(OwlFtpEncryption enc);

/* Vrai si un chemin de cle privee non vide est fourni : l'auth SFTP passe
 * alors par la cle publique (fichier OpenSSH), sinon par mot de passe. VM-free,
 * pur (sert au routage ET a l'auto-test hors-ligne). */
bool owl_sftp_uses_key_auth(const char* privkey);

/* Ouvre une connexion SFTP (tunnel SSH-2 via libssh2) : connexion TCP,
 * handshake SSH, verification de la cle d'hote contre known_hosts, puis
 * authentification par mot de passe OU par cle privee OpenSSH (si privkey non
 * vide). Renvoie un client pret (marque is_sftp) ou NULL (err rempli). VM-free.
 *
 *   allow_unknown_host : true = accepte un hote absent de known_hosts
 *                        (ftpIgnoreHoteInconnu). Un CONFLIT de cle reste un
 *                        echec strict quel que soit ce flag (anti-MITM).
 *   out_host_verified  : recoit true si la cle d'hote a matche known_hosts,
 *                        false si l'hote etait inconnu mais accepte. Optionnel. */
OwlFtpClient* owl_sftp_open(const char* host, int port,
                            const char* user, const char* pass,
                            const char* privkey, const char* privkey_pass,
                            bool allow_unknown_host, bool* out_host_verified,
                            char* err, size_t errlen);

/* Callback par entree de listage. Renvoie false pour interrompre le
 * parcours (equivalent canon RENVOYER Faux). */
typedef bool (*OwlFtpEntryFn)(void* ctx, const char* name, int64_t size,
                              const char* attribut, const char* date,
                              const char* heure);

/* Callback de progression d'un transfert : (tailleTotale, tailleTransferee).
 * Renvoie false pour abandonner le transfert. */
typedef bool (*OwlFtpProgressFn)(void* ctx, int64_t total, int64_t done);

/* Puits de reception des octets telecharges (RETR). Renvoie false pour
 * abandonner. */
typedef bool (*OwlFtpSinkFn)(void* ctx, const char* buf, size_t len);

/* Ouvre le canal de controle : connect (TCP en clair, ou TLS d'emblee pour
 * FTPS) + banniere 220 + [AUTH TLS + handshake pour FTPES] + USER/PASS +
 * [PBSZ 0 / PROT P si securise] + FEAT (detection MLSD/UTF8). Renvoie un
 * client pret ou NULL (err rempli). Le coeur n'emet aucune erreur VM : il
 * ne fait que remplir err.
 *
 *   enc          : mode de chiffrement (clair / FTPS implicite / FTPES explicite).
 *   verify_level : OwlTlsVerifyLevel (NONE/CHAIN/FULL) applique au handshake
 *                  du canal de controle ET reutilise pour le canal de donnees.
 *                  Ignore quand enc == OWL_FTP_ENC_CLEAR. */
OwlFtpClient* owl_ftp_open(const char* host, int port,
                           const char* user, const char* pass,
                           OwlFtpEncryption enc, int verify_level,
                           char* err, size_t errlen);

/* Mode de chiffrement effectif du canal de controle (clair/FTPS/FTPES). */
OwlFtpEncryption owl_ftp_encryption(const OwlFtpClient* c);

/* QUIT + fermeture du canal de controle + liberation. c peut etre NULL. */
void owl_ftp_close(OwlFtpClient* c);

/* Le serveur a-t-il annonce MLSD dans FEAT ? */
bool owl_ftp_has_mlsd(const OwlFtpClient* c);

/* Listage d'un repertoire distant (MLSD si annonce, sinon LIST heuristique).
 * Ouvre un canal de donnees PASV, rappelle cb par entree. Renvoie le nombre
 * d'entrees parcourues, ou -1 sur erreur protocolaire. Un cb qui renvoie
 * false interrompt le parcours (le compte s'arrete a cette entree incluse). */
int owl_ftp_list(OwlFtpClient* c, const char* path,
                 OwlFtpEntryFn cb, void* ctx);

/* Telechargement (RETR) via canal de donnees PASV. Les octets recus sont
 * livres a sink. prog (NULL si absent) est rappele apres chaque bloc.
 * ascii = TYPE A (conversion fins de ligne), sinon TYPE I (binaire brut).
 * Renvoie false sur echec ou abandon (sink/prog ont renvoye false). */
bool owl_ftp_retr(OwlFtpClient* c, const char* remote, bool ascii,
                  OwlFtpSinkFn sink, void* sctx,
                  OwlFtpProgressFn prog, void* pctx,
                  char* err, size_t errlen);

/* Envoi (STOR) de data[0..len[ via canal de donnees PASV. prog (NULL si
 * absent) est rappele apres chaque bloc emis. Renvoie false sur echec ou
 * abandon. */
bool owl_ftp_stor(OwlFtpClient* c, const char* remote, bool ascii,
                  const char* data, size_t len,
                  OwlFtpProgressFn prog, void* pctx,
                  char* err, size_t errlen);

/* ---- Metadonnees (canal de controle seul) ----
 *
 * A fichier explicite : aucun etat cache "dernier fichier consulte". */

/* SIZE remote -> taille en octets, ou -1 sur erreur (fichier absent,
 * commande refusee). Bascule en TYPE I (certains serveurs refusent SIZE en
 * ASCII). */
int64_t owl_ftp_size(OwlFtpClient* c, const char* remote);

/* MDTM remote -> horodatage de derniere modification au format AAAAMMJJHHMMSS
 * (14 caracteres + '\0') dans out. Renvoie false sur erreur. */
bool owl_ftp_mdtm(OwlFtpClient* c, const char* remote, char* out, size_t outsz);

/* MLST remote -> ligne de facts (ex "modify=...;create=...;type=file;") dans
 * facts. Renvoie false sur erreur. */
bool owl_ftp_mlst(OwlFtpClient* c, const char* remote, char* facts, size_t factssz);

/* Derive la chaine d'attributs PORTABLE d'un fichier distant depuis une ligne
 * de facts MLST : "R" (lecture seule, depuis perm=), "D" (repertoire, depuis
 * type=dir), en ordre canon R avant D ; chaine vide si aucun attribut portable.
 * Les attributs specifiques DOS/Windows-FAT (cache/systeme/archive/compresse)
 * ne sont volontairement PAS exposes : non portables via MLST (cf. stdlib_ftp.c
 * et ). VM-free, testable directement. */
void owl_ftp_attr_from_facts(const char* facts, char* out, size_t outsz);

#endif /* owl_stdlib_ftp_h */
