// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* OWL — user config override pour mbedtls.
 *
 * Inclus par mbedtls/build_info.h APRES mbedtls_config.h (defaut vendor).
 * Permet de neutraliser des options sans toucher au vendor (cf doc
 * docs/internal_documentation/build_hardening.md).
 *
 * Active via target_compile_definitions(owl_mbedtls PRIVATE
 *     MBEDTLS_USER_CONFIG_FILE="\"owl_mbedtls_user_config.h\"")
 */
#ifndef OWL_MBEDTLS_USER_CONFIG_H
#define OWL_MBEDTLS_USER_CONFIG_H

/* Desactive le module debug de mbedtls — OWL ne consomme pas
 * mbedtls_debug_set_threshold ni mbedtls_ssl_conf_dbg. Cela retire :
 *   - le code de library/debug.c
 *   - les strings "assertion is too long", "assertion in UTF",
 *     "assertion is not limited" qui vivent dans ce module
 *   - les callbacks de debug TLS jamais utilises par OWL
 * Cf vendor/mbedtls/include/mbedtls/mbedtls_config.h ligne ~2683. */
#undef MBEDTLS_DEBUG_C

#endif /* OWL_MBEDTLS_USER_CONFIG_H */
