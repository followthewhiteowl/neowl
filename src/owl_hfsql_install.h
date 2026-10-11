// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_hfsql_install_h
#define owl_hfsql_install_h

/* ================================================================
 * HFSQL ODBC driver — extraction au premier usage
 *
 * Le pack de bibliotheques du provider sqlHFSQL est embarque dans
 * owl(.exe) via objcopy au build.
 * Au premier appel d'une connexion sqlHFSQL, le loader (cf
 * sql_odbc_loader.load_hfsql_direct) declenche ensure_installed() qui :
 *
 *   1. Verifie l'existence physique du driver dans le dossier d'install
 *      pour la version courante. Si present : no-op.
 *   2. Sinon : decompresse le pack LZMA1 embarque + parser tar +
 *      ecrit les fichiers dans le dossier d'install (persistant).
 *
 * Path d'install :
 *  - Windows : %LOCALAPPDATA%/owl/hfsql_driver/v<VERSION>/
 *  - Linux   : $XDG_DATA_HOME/owl/hfsql_driver/v<VERSION>/
 *              (fallback : $HOME/.local/share/owl/hfsql_driver/v<VERSION>/)
 *
 * Chargement direct de la bibliotheque, pas d'enregistrement systeme
 * requis.
 *
 * Pas de cleanup au shutdown — install persistante, idempotente.
 * Bumper OWL_HFSQL_PACK_VERSION dans .c quand on change le pack.
 * ================================================================ */

#include "common.h"

#if defined(OWL_HAS_HFSQL_EMBED) && OWL_HAS_HFSQL_EMBED

/* Assure que le driver HFSQL est extrait et pret a etre dlopen().
 * Idempotent : appel multiple sans cout (apres le 1er succes).
 *
 * Renvoie true si pret a l'usage. Sur false, err_out (>= 512) explique
 * pourquoi (extract echec, ecriture echec, OOM, etc.). */
bool owl_hfsql_ensure_installed(char* err_out, size_t err_cap);

/* Copie dans out_buf le path du dossier d'install pour la version
 * courante. Renvoie true si tout tient dans le buffer. N'extrait rien —
 * utilise ensure_installed avant si tu veux que le dossier existe. */
bool owl_hfsql_get_install_dir(char* out_buf, size_t out_cap);

#else

/* Stub : sur les plateformes sans pack embarque, renvoie false avec
 * message clair. */
static inline bool owl_hfsql_ensure_installed(char* err_out, size_t err_cap) {
    if (err_out && err_cap > 0) {
        snprintf(err_out, err_cap,
                 "HFSQL embedded driver non disponible sur cet OS "
                 "(OWL_HAS_HFSQL_EMBED=0 — pas de pack vendore).");
    }
    return false;
}

#endif

#endif
