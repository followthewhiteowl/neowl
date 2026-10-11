// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_net_be_buffer_h
#define owl_net_be_buffer_h

/* ================================================================
 * BIG-ENDIAN BINARY BUFFER (net byte order)
 *
 * Read/write primitives pour parser les protocoles réseau binaires
 * (PostgreSQL wire, MySQL wire, redis RESP3, etc.). Tous les entiers
 * sont en network byte order (big-endian) — standard des protocoles
 * réseau.
 *
 * Le buffer est en mode write (append) OU read (consume), pas mixte
 * dans une même phase. Ré-initialisable via *_reset.
 *
 * Convention erreur :
 *   - Les fonctions write ne plantent pas (grow auto sauf OOM)
 *   - Les fonctions read retournent false si le buffer est consommé
 *     plus loin que disponible (truncated). L'appelant doit vérifier.
 * ================================================================ */

#include "common.h"

typedef struct {
    uint8_t* data;       /* contenu */
    size_t   length;     /* octets utilisés (write) ou total à lire (read) */
    size_t   capacity;   /* allocation interne, >= length */
    size_t   pos;        /* position de lecture courante (mode read) */
} OwlBeBuffer;

/* ---------- Construction / teardown ---------- */

/* Initialise un buffer vide. Capacité initiale optionnelle (0 = défaut 64). */
void owl_be_buffer_init(OwlBeBuffer* buf, size_t initial_capacity);

/* Libère les octets internes. Le buffer redevient utilisable via _init. */
void owl_be_buffer_free(OwlBeBuffer* buf);

/* Vide le buffer (length=0, pos=0). Garde la capacité allouée. */
void owl_be_buffer_reset(OwlBeBuffer* buf);

/* Attache un buffer existant pour lecture seule. Aucune copie.
 * Le caller garantit que `data` reste vivant pendant l'utilisation. */
void owl_be_buffer_attach_read(OwlBeBuffer* buf, const uint8_t* data, size_t length);

/* ---------- Write (mode append) ---------- */

void owl_be_buffer_write_u8 (OwlBeBuffer* buf, uint8_t  v);
void owl_be_buffer_write_u16(OwlBeBuffer* buf, uint16_t v);  /* BE */
void owl_be_buffer_write_u32(OwlBeBuffer* buf, uint32_t v);  /* BE */
void owl_be_buffer_write_u64(OwlBeBuffer* buf, uint64_t v);  /* BE */
void owl_be_buffer_write_i16(OwlBeBuffer* buf, int16_t  v);
void owl_be_buffer_write_i32(OwlBeBuffer* buf, int32_t  v);
void owl_be_buffer_write_i64(OwlBeBuffer* buf, int64_t  v);

/* Bytes bruts, longueur connue. */
void owl_be_buffer_write_bytes(OwlBeBuffer* buf, const void* src, size_t n);

/* String C avec '\0' terminal (utile pour protocoles type PG). */
void owl_be_buffer_write_cstring(OwlBeBuffer* buf, const char* s);

/* Patch ultérieur : écrire un u32 BE à un offset arbitraire (pour
 * back-patcher la length d'un message une fois sa taille connue). */
void owl_be_buffer_patch_u32(OwlBeBuffer* buf, size_t offset, uint32_t v);

/* ---------- Read (mode consume, mute pos) ---------- */

/* Toutes les read_* retournent false si pas assez d'octets disponibles.
 * Sur false, pos est inchangé et la valeur out est laissée intacte. */

bool owl_be_buffer_read_u8 (OwlBeBuffer* buf, uint8_t*  out);
bool owl_be_buffer_read_u16(OwlBeBuffer* buf, uint16_t* out);
bool owl_be_buffer_read_u32(OwlBeBuffer* buf, uint32_t* out);
bool owl_be_buffer_read_u64(OwlBeBuffer* buf, uint64_t* out);
bool owl_be_buffer_read_i16(OwlBeBuffer* buf, int16_t*  out);
bool owl_be_buffer_read_i32(OwlBeBuffer* buf, int32_t*  out);
bool owl_be_buffer_read_i64(OwlBeBuffer* buf, int64_t*  out);

/* Bytes : copy n octets dans dst. */
bool owl_be_buffer_read_bytes(OwlBeBuffer* buf, void* dst, size_t n);

/* C-string : retourne un pointeur INTERNE vers le buffer (zero-copy).
 * Le caller ne doit PAS free le retour. Le pointeur reste valide
 * jusqu'à la prochaine modification du buffer (write/reset/free).
 * Avance pos jusqu'après le '\0'. Retourne NULL si pas de '\0' trouvé
 * dans les octets restants. */
const char* owl_be_buffer_read_cstring(OwlBeBuffer* buf);

/* ---------- Introspection ---------- */

/* Nombre d'octets restant à lire (length - pos). */
size_t owl_be_buffer_remaining(const OwlBeBuffer* buf);

/* True si pos == length. */
bool owl_be_buffer_eof(const OwlBeBuffer* buf);

#endif
