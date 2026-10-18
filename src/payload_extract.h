// SPDX-License-Identifier: AGPL-3.0-only
#ifndef OWL_PAYLOAD_EXTRACT_H
#define OWL_PAYLOAD_EXTRACT_H

#include <stddef.h>
#include <stdint.h>

/* Resoud le path absolu de l'executable courant (argv[0] amelioré). */
int owl_get_self_path(char* out, size_t cap);

/* Verifie si l'exe contient un payload single-file (footer magic).
 * Si oui, retourne 0 et remplit out_offset/out_size. */
int owl_payload_check(const char* self_path,
                      uint64_t* out_offset, uint64_t* out_size);

/* Extrait le zip embarque dans dest_dir. Retourne 0 si OK.
 * target : triple de la cible du build (ex "windows-x86_64"). Les composants
 *   multi-cibles (<comp>/<triple>/...) d'une AUTRE cible ne sont PAS ecrits sur
 *   disque — un build ne consomme qu'une cible. NULL = tout extraire.
 * extract_common : 1 pour ecrire aussi les fichiers host-agnostiques
 *   (zig/, include/, outils host) ; 0 pour les sauter (deja presents). */
int owl_payload_extract(const char* self_path,
                         uint64_t offset, uint64_t size,
                         const char* dest_dir,
                         const char* target, int extract_common);

/* High-level : si payload present, extract dans <TEMP>/owl-build-<hash>/ et
 * retourne le chemin dans out_root. Cache par cible via marqueur
 * .complete.<target> (+ .complete.common pour l'arbre host-agnostique) :
 * re-lancer le meme build ne re-extrait rien ; un cross-build vers une autre
 * cible n'extrait que SES composants, sans re-ecrire zig/include deja la.
 * target : triple de la cible du build (non-NULL).
 * Retourne 0 si payload OK, -1 si pas de payload (mode dev / dist non-mega). */
int owl_payload_setup(const char* target, char* out_root, size_t cap);

#endif
