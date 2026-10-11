// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* sftp_hostkey_keymask.h — mapping type de cle d'hote SSH -> bit KEY_ known_hosts.
 *
 * libssh2_session_hostkey() rend le type negocie sous forme LIBSSH2_HOSTKEY_TYPE_*
 * (RSA=1, ECDSA_256=3, ED25519=6...). libssh2_knownhost_checkp() attend, dans son
 * typemask, le type sous forme LIBSSH2_KNOWNHOST_KEY_* (SSHRSA=2<<18, ED25519=7<<18...).
 * Ce sont deux enumerations DISTINCTES ; il faut convertir.
 *
 * Pourquoi c'est critique (cf. vendor/libssh2/src/knownhost.c::knownhost_check) :
 * quand le typemask passe a checkp n'a AUCUN bit KEY_ (host_key_type == 0), libssh2
 * compare la cle negociee contre TOUTES les entrees de l'hote, quel que soit leur
 * type. Un hote connu seulement sous rsa, negociant ed25519, donne alors un
 * strcmp qui echoue -> MISMATCH (faux conflit MITM). En passant le bon bit KEY_,
 * libssh2 scope la comparaison au meme type : entree meme-type-cle-differente ->
 * MISMATCH (vrai MITM, preserve) ; aucune entree du bon type -> NOTFOUND
 * (relachable comme un hote inconnu, aligne sur OpenSSH).
 *
 * Header pur (inline, sans dependance owl) : consomme par stdlib_ftp.c ET par le
 * smoke test libssh2 qui verrouille la semantique de scoping hors-ligne.
 */
#ifndef OWL_SFTP_HOSTKEY_KEYMASK_H
#define OWL_SFTP_HOSTKEY_KEYMASK_H

#include "libssh2.h"

/* Convertit un LIBSSH2_HOSTKEY_TYPE_* (retour de libssh2_session_hostkey) vers le
 * bit LIBSSH2_KNOWNHOST_KEY_* correspondant, a OU-er dans le typemask de checkp.
 *
 * Retourne 0 pour un type non mappe (exotique) : checkp retombe alors sur son
 * comportement historique (comparaison tous types). C'est le cote SUR du
 * fallback — un type inconnu confronte a une entree d'un autre type donnera
 * MISMATCH (refus), jamais un faux MATCH. */
static inline int owl_sftp_knownhost_keymask(int hostkey_type) {
    switch (hostkey_type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
#if defined(LIBSSH2_HOSTKEY_TYPE_DSS) && !defined(LIBSSH2_NO_DEPRECATED)
        case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
#endif
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default:                             return 0;
    }
}

#endif /* OWL_SFTP_HOSTKEY_KEYMASK_H */
