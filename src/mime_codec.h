// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_MIME_CODEC_H
#define OWL_MIME_CODEC_H

/* Encodeurs MIME internes (sans surface OWL — aucune native, aucun registre).
 * Servent la fabrique MIME et le parseur des lots email suivants.
 *
 * Contrat commun :
 *   - Entrees (ptr,len)-safe : `src` peut contenir des \0, `len` en octets.
 *     `src` NULL n'est autorise que si `len == 0`.
 *   - Sorties allouees sur le tas (malloc), toujours terminees par un \0
 *     (le \0 n'est PAS compte dans *out_len). L'appelant free().
 *   - `out_len` peut etre NULL si la longueur ne sert pas.
 *   - Retour NULL uniquement en cas d'echec d'allocation.
 *   - UTF-8 : OWL stocke en UTF-8, ces codecs raisonnent en octets et
 *     preservent les frontieres de caracteres UTF-8 (jamais de coupe au
 *     milieu d'un caractere multi-octets lors du folding RFC 2047).
 */

#include <stddef.h>

/* Quoted-Printable, RFC 2045 §6.7.
 * encode : octets >0x7E, de controle et '=' -> "=XX" (hexa MAJUSCULE) ;
 *   espaces/tabs en fin de ligne encodes ; retours de ligne souples "=\r\n"
 *   pour ne pas depasser 76 colonnes ; \n et \r\n de l'entree -> CRLF dur.
 * decode : inverse, tolerant ("=XX", soft break "=\r\n" et "=\n"). */
char* qp_encode(const char* src, size_t len, size_t* out_len);
char* qp_decode(const char* src, size_t len, size_t* out_len);

/* Mot-encode d'en-tete, RFC 2047.
 * encode : une valeur pur-ASCII est renvoyee telle quelle ; sinon elle est
 *   decoupee en un ou plusieurs mots-encodes UTF-8 (Q ou B selon la densite
 *   de non-ASCII), chacun <= 75 caracteres, separes par un repli "\r\n ".
 * decode : gere Q et B, plusieurs mots-encodes successifs, retire l'espace
 *   de repli entre deux mots adjacents, charset UTF-8 (et tolere ISO-8859-1
 *   en entree). */
char* rfc2047_encode(const char* src, size_t len, size_t* out_len);
char* rfc2047_decode(const char* src, size_t len, size_t* out_len);

#endif /* OWL_MIME_CODEC_H */
