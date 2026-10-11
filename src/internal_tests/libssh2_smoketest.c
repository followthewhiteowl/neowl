// SPDX-License-Identifier: AGPL-3.0-only
/* libssh2_smoketest.c — smoke test du vendoring libssh2 (BSD-3, backend mbedTLS).
 *
 * But : valider que la brique SSH-2 / SFTP vendoree compile, linke et s'INITIE
 * au runtime sur la toolchain OWL (host gcc UCRT64 + cross zig), AVANT que les
 * natives FTP* (Lot D : routage sftp://) la consomment. Aucun wrapping OWL ici,
 * aucune connexion reseau : on prouve le socle crypto, pas le protocole en
 * ligne (ca, c'est le lot feature + le harness sshd conteneurise).
 *
 * Ce qui est REELLEMENT prouve (au-dela du simple link) :
 *   1. libssh2_init(0) : init globale (le backend mbedTLS amorce son RNG).
 *   2. libssh2_session_init() : allocation de session sur le backend crypto.
 *   3. Version >= 1.11.1 : garantit qu'on n'est PAS sur un tag anterieur. Le pin
 *      reel est un commit master post-1.11.1 (1.11.2_DEV) qui embarque les fixes
 *      CVE-2025-15661 / CVE-2026-55199 / CVE-2026-55200 (cf. bloc owl_libssh2 de
 *      CMakeLists.txt) — non testable au runtime, garanti par le commit epingle.
 *   4. libssh2_session_supported_algs(KEX) et (HOSTKEY) NON VIDES : c'est la
 *      preuve que le backend mbedTLS a bien ENREGISTRE des algorithmes (un link
 *      reussi mais un backend muet donnerait des listes vides et casserait tout
 *      handshake). On verifie en plus la presence d'au moins un algo moderne
 *      (curve25519 en KEX, ecdsa/ssh-ed25519 ou rsa-sha2 en HOSTKEY).
 *   5. libssh2_knownhost_init() : amorce l'API known-hosts, socle de la future
 *      propriete ..VerifieHote (verification de la cle d'hote SSH).
 *
 * Exit code 0 si tout passe, != 0 + message sinon (pattern zint/zstd/libxls).
 */

#include <stdio.h>
#include <string.h>
#include "libssh2.h"
#include "../sftp_hostkey_keymask.h"

/* Cherche `needle` dans la liste d'algos renvoyee par libssh2 (tableau de
   char* termine par la taille `n`). Retourne 1 si trouve. */
static int algs_contains(const char **algs, int n, const char *needle) {
    for (int i = 0; i < n; i++) {
        if (algs[i] && strstr(algs[i], needle)) return 1;
    }
    return 0;
}

/* Verrou hors-ligne du fix "conflit type vs MITM".
 *
 * Construit un known_hosts EN MEMOIRE avec une seule entree pour un hote, sous
 * le type de cle ssh-rsa, puis confronte via libssh2_knownhost_checkp des cles
 * de differents types/valeurs — en construisant le typemask EXACTEMENT comme
 * sftp_check_host (PLAIN | KEYENC_RAW | owl_sftp_knownhost_keymask(type)).
 *
 * Ce que ca prouve, sans serveur ni socket :
 *   A) cle NEGOCIEE d'un AUTRE type (ed25519) que la seule entree (rsa) ->
 *      NOTFOUND (hote inconnu pour ce type, relachable), PAS MISMATCH.
 *      C'est le bug : sans le bit KEY_ (keymask==0) libssh2 rend MISMATCH.
 *   B) cle du MEME type (rsa) mais octets differents -> MISMATCH (vrai MITM,
 *      protection PRESERVEE, non negociable).
 *   C) cle du MEME type et memes octets -> MATCH.
 *   D) hote absent -> NOTFOUND.
 *
 * Retour 0 si les 4 cas tiennent, code 2x sinon. */
static int test_hostkey_type_scoping(LIBSSH2_SESSION *session) {
    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(session);
    if (!kh) return 20;

    const char *host = "testhost.invalid";
    /* Cles synthetiques : la comparaison known_hosts est octet-a-octet, le
       contenu importe peu, seule la DISTINCTION compte. */
    const char keyStored[] = "RAW-RSA-KEY-STORED-0001";
    const char keyOther[]  = "RAW-KEY-DIFFERENT-9999";

    /* Une seule entree : host sous ssh-rsa. */
    int addmask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                  LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    if (libssh2_knownhost_addc(kh, host, NULL, keyStored, sizeof(keyStored) - 1,
                               NULL, 0, addmask, NULL) != 0) {
        libssh2_knownhost_free(kh);
        return 21;
    }

    struct libssh2_knownhost *found = NULL;
    int base = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    int rc;

    /* A) cle d'un autre type (ed25519) : doit etre NOTFOUND (pas MISMATCH). */
    rc = libssh2_knownhost_checkp(kh, host, 22, keyOther, sizeof(keyOther) - 1,
             base | owl_sftp_knownhost_keymask(LIBSSH2_HOSTKEY_TYPE_ED25519),
             &found);
    if (rc != LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        fprintf(stderr, "libssh2_smoketest: cross-type attendu NOTFOUND, obtenu "
                "%d (0=MATCH 1=MISMATCH 2=NOTFOUND 3=FAILURE)\n", rc);
        libssh2_knownhost_free(kh);
        return 22;
    }

    /* B) meme type (rsa), octets differents : MISMATCH (MITM preserve). */
    rc = libssh2_knownhost_checkp(kh, host, 22, keyOther, sizeof(keyOther) - 1,
             base | owl_sftp_knownhost_keymask(LIBSSH2_HOSTKEY_TYPE_RSA),
             &found);
    if (rc != LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        fprintf(stderr, "libssh2_smoketest: meme-type-cle-differente attendu "
                "MISMATCH, obtenu %d (protection MITM !)\n", rc);
        libssh2_knownhost_free(kh);
        return 23;
    }

    /* C) meme type, memes octets : MATCH. */
    rc = libssh2_knownhost_checkp(kh, host, 22, keyStored, sizeof(keyStored) - 1,
             base | owl_sftp_knownhost_keymask(LIBSSH2_HOSTKEY_TYPE_RSA),
             &found);
    if (rc != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        fprintf(stderr, "libssh2_smoketest: meme-cle attendu MATCH, obtenu %d\n", rc);
        libssh2_knownhost_free(kh);
        return 24;
    }

    /* D) hote absent : NOTFOUND. */
    rc = libssh2_knownhost_checkp(kh, "autre.invalid", 22,
             keyStored, sizeof(keyStored) - 1,
             base | owl_sftp_knownhost_keymask(LIBSSH2_HOSTKEY_TYPE_RSA),
             &found);
    if (rc != LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        fprintf(stderr, "libssh2_smoketest: hote absent attendu NOTFOUND, obtenu "
                "%d\n", rc);
        libssh2_knownhost_free(kh);
        return 25;
    }

    libssh2_knownhost_free(kh);
    return 0;
}

int main(void) {
    /* 1) Init globale. */
    if (libssh2_init(0) != 0) {
        fprintf(stderr, "libssh2_smoketest: libssh2_init a echoue\n");
        return 1;
    }

    /* 3) Garde-fou de version : jamais < 1.11.1 (le dernier tag vulnerable).
       LIBSSH2_VERSION_NUM = 0xMMmmpp. 1.11.1 = 0x010b01. */
    if (LIBSSH2_VERSION_NUM < 0x010b01) {
        fprintf(stderr, "libssh2_smoketest: version %s < 1.11.1 attendue "
                "(pin doit etre un commit post-1.11.1 patche CVE)\n",
                LIBSSH2_VERSION);
        return 2;
    }
    if (libssh2_version(0) == NULL) {
        fprintf(stderr, "libssh2_smoketest: libssh2_version(0) NULL\n");
        return 3;
    }

    /* 2) Session sur le backend crypto. */
    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        fprintf(stderr, "libssh2_smoketest: libssh2_session_init a echoue "
                "(backend crypto ?)\n");
        libssh2_exit();
        return 4;
    }

    /* 4) Le backend mbedTLS a-t-il enregistre des algos ? Listes NON vides. */
    const char **kex = NULL;
    int nkex = libssh2_session_supported_algs(session, LIBSSH2_METHOD_KEX, &kex);
    if (nkex <= 0 || kex == NULL) {
        fprintf(stderr, "libssh2_smoketest: aucun algo KEX (backend mbedTLS "
                "muet ?) n=%d\n", nkex);
        libssh2_session_free(session);
        libssh2_exit();
        return 5;
    }
    int has_modern_kex = algs_contains(kex, nkex, "curve25519") ||
                         algs_contains(kex, nkex, "ecdh-sha2");
    libssh2_free(session, (void *)kex);
    if (!has_modern_kex) {
        fprintf(stderr, "libssh2_smoketest: aucun KEX moderne (curve25519/ecdh) "
                "dans les %d algos\n", nkex);
        libssh2_session_free(session);
        libssh2_exit();
        return 6;
    }

    const char **hk = NULL;
    int nhk = libssh2_session_supported_algs(session, LIBSSH2_METHOD_HOSTKEY, &hk);
    if (nhk <= 0 || hk == NULL) {
        fprintf(stderr, "libssh2_smoketest: aucun algo HOSTKEY n=%d\n", nhk);
        libssh2_session_free(session);
        libssh2_exit();
        return 7;
    }
    int has_hostkey = algs_contains(hk, nhk, "ecdsa") ||
                      algs_contains(hk, nhk, "ssh-ed25519") ||
                      algs_contains(hk, nhk, "rsa-sha2") ||
                      algs_contains(hk, nhk, "ssh-rsa");
    libssh2_free(session, (void *)hk);
    if (!has_hostkey) {
        fprintf(stderr, "libssh2_smoketest: aucun HOSTKEY exploitable dans les "
                "%d algos\n", nhk);
        libssh2_session_free(session);
        libssh2_exit();
        return 8;
    }

    /* 5) Amorce known-hosts (socle de la future propriete ..VerifieHote). */
    LIBSSH2_KNOWNHOSTS *kh = libssh2_knownhost_init(session);
    if (!kh) {
        fprintf(stderr, "libssh2_smoketest: libssh2_knownhost_init a echoue\n");
        libssh2_session_free(session);
        libssh2_exit();
        return 9;
    }
    libssh2_knownhost_free(kh);

    /* 6) Scoping par type de cle du check known_hosts. */
    int scope_rc = test_hostkey_type_scoping(session);
    if (scope_rc != 0) {
        libssh2_session_free(session);
        libssh2_exit();
        return scope_rc;
    }

    libssh2_session_free(session);
    libssh2_exit();

    printf("libssh2_smoketest OK (libssh2 %s, backend mbedTLS : %d KEX / %d "
           "HOSTKEY, known-hosts amorce)\n", LIBSSH2_VERSION, nkex, nhk);
    return 0;
}
