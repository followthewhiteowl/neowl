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

/* Extrait le zip embarque dans dest_dir. Retourne 0 si OK. */
int owl_payload_extract(const char* self_path,
                         uint64_t offset, uint64_t size,
                         const char* dest_dir);

/* High-level : si payload present, extract dans <TEMP>/owl-build-<hash>/
 * (cache via marker .complete) et retourne le chemin dans out_root.
 * Retourne 0 si payload OK, -1 si pas de payload (mode dev / dist non-mega). */
int owl_payload_setup(char* out_root, size_t cap);

#endif
