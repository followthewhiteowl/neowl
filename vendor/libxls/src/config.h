/* config.h — genere a la main pour le vendoring OWL (remplace le config.h
 * autotools amont). libxls inclut ce fichier inconditionnellement dans chaque
 * .c ; il ne sert qu'a poser (ou NON) trois macros de detection.
 *
 * Les TROIS macros HAVE_* sont volontairement LAISSEES NON DEFINIES :
 *
 *   HAVE_ICONV       -> NON. Aucune dependance iconv/libiconv (pas vendoree,
 *                       cascade refusee). Le chemin sans-iconv de xlstool.c
 *                       convertit deja proprement : compresse BIFF8 ->
 *                       transcode_latin1_to_utf8() ; UTF-16 -> unicode_decode_
 *                       wcstombs() avec une locale UTF-8 FORCEE par plateforme
 *                       (_create_locale(".65001") sur Windows, newlocale
 *                       "C.UTF-8" sur Linux, "UTF-8" sur macOS, cf. locale.c).
 *                       Valide a l'execution sur du vietnamien (diacritiques)
 *                       et des noms de feuille multi-octets.
 *   HAVE_XLOCALE_H   -> NON. <xlocale.h> n'existe pas hors BSD/macOS anciens ;
 *                       locale.h suffit (UCRT / glibc / musl).
 *   HAVE_WCSTOMBS_L  -> NON. La variante POSIX `wcstombs_l` n'est pas garantie ;
 *                       le fallback uselocale()+wcstombs() de locale.c est
 *                       portable (Windows utilise sa propre branche _wcstombs_l).
 *
 * _GNU_SOURCE (requis par newlocale/uselocale/strdup cote Linux) est pose par
 * la cible CMake owl_libxls, pas ici, pour rester purement declaratif.
 */
#ifndef OWL_LIBXLS_CONFIG_H
#define OWL_LIBXLS_CONFIG_H

#define PACKAGE_VERSION "1.6.3"

#endif /* OWL_LIBXLS_CONFIG_H */
