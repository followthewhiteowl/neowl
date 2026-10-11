// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_hfsql_driver_h
#define owl_hfsql_driver_h

/* Nom du fichier du driver ODBC embarque (bibliotheque dynamique), PAR
 * PLATEFORME. Source UNIQUE : consomme a la fois par l'installeur
 * (owl_hfsql_install.c, qui extrait ce fichier du pack vendore) et par le
 * loader (sql_odbc_loader.c, qui le dlopen directement, bypass Driver
 * Manager). Les deux chemins DOIVENT pointer le meme fichier : sinon
 * l'installeur depose un nom et le loader en cherche un autre (connexion
 * directe cassee silencieusement). Ne jamais dupliquer ce #define ailleurs.
 *
 * La valeur reelle (version-stampee) vit dans owl_hfsql_driver_local.h, un
 * header PRIVE exclu de l'export public. Build prive : present -> inclus.
 * Build public : absent -> fallback generique ci-dessous (le driver
 * embarque est hors export, OWL_HAS_HFSQL_EMBED off, le nom n'est pas
 * requis). */
#if defined(__has_include)
#  if __has_include("owl_hfsql_driver_local.h")
#    include "owl_hfsql_driver_local.h"
#  endif
#endif

#ifndef OWL_HFSQL_DRIVER_DLL_NAME
/* Fallback public : nom resolu via config/install en mode embarque. */
#define OWL_HFSQL_DRIVER_DLL_NAME  ""
#endif

#endif
