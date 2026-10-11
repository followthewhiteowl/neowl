// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_odbc_loader.h"
#include "owl_hfsql_driver.h"

#if OWL_HAS_ODBC

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#if defined(OWL_HAS_HFSQL_EMBED) && OWL_HAS_HFSQL_EMBED
#include "owl_hfsql_install.h"
#endif

/* ================================================================
 * Abstraction de chargement dynamique cross-platform
 * ================================================================ */

#ifdef _WIN32
typedef HMODULE owl_dl_handle;
#define OWL_DLOPEN(path)            ((void*)LoadLibraryA(path))
#define OWL_DLSYM(handle, sym)      ((void*)GetProcAddress((HMODULE)(handle), sym))
#define OWL_DLCLOSE(handle)         FreeLibrary((HMODULE)(handle))
#define OWL_PATH_SEP                "\\"
#define OWL_DM_DEFAULT_NAME         "odbc32.dll"
#else
typedef void* owl_dl_handle;
#define OWL_DLOPEN(path)            dlopen((path), RTLD_NOW | RTLD_LOCAL)
#define OWL_DLSYM(handle, sym)      dlsym((handle), sym)
#define OWL_DLCLOSE(handle)         dlclose(handle)
#define OWL_PATH_SEP                "/"
/* Sur Linux le DM pour sqlODBC = libiodbc.so.2 (extrait du pack
 * HFSQL vendore). Voir owl_odbc_vt_load_default plus bas. */
#define OWL_DM_DEFAULT_NAME         "libiodbc.so.2"
#endif

/* ================================================================
 * Resolution generique des symboles
 *
 * vt_resolve_all : remplit toutes les entries du VT depuis un handle
 * deja ouvert. Renvoie le nom du 1er symbole manquant, ou NULL si OK.
 *
 * include_drivers_w : SQLDriversW est expose uniquement par les Driver
 * Manager (odbc32 / libodbc / libiodbc), pas par les drivers individuels.
 * ================================================================ */

#define RESOLVE(field, sym) do { \
    vt->field = OWL_DLSYM(vt->dll_handle, sym); \
    if (!vt->field) return sym; \
} while (0)

#define RESOLVE_OPT(field, sym) do { \
    vt->field = OWL_DLSYM(vt->dll_handle, sym); \
} while (0)

static const char* vt_resolve_all(OwlOdbcVT* vt, bool include_drivers_w) {
    RESOLVE(AllocHandle,     "SQLAllocHandle");
    RESOLVE(FreeHandle,      "SQLFreeHandle");
    RESOLVE(SetEnvAttr,      "SQLSetEnvAttr");
    RESOLVE(SetConnectAttrW, "SQLSetConnectAttrW");
    RESOLVE(DriverConnectW,  "SQLDriverConnectW");
    RESOLVE(Disconnect,      "SQLDisconnect");
    RESOLVE(GetDiagRecW,     "SQLGetDiagRecW");
    RESOLVE(EndTran,         "SQLEndTran");

    RESOLVE(ExecDirectW,     "SQLExecDirectW");
    RESOLVE(PrepareW,        "SQLPrepareW");
    RESOLVE(Execute,         "SQLExecute");
    RESOLVE(BindParameter,   "SQLBindParameter");
    RESOLVE(NumResultCols,   "SQLNumResultCols");
    RESOLVE(DescribeColW,    "SQLDescribeColW");
    RESOLVE(ColAttributeW,   "SQLColAttributeW");
    RESOLVE(Fetch,           "SQLFetch");
    RESOLVE(GetData,         "SQLGetData");
    RESOLVE(RowCount,        "SQLRowCount");

    if (include_drivers_w) {
        RESOLVE_OPT(DriversW, "SQLDriversW");
    } else {
        vt->DriversW = NULL;
    }
    return NULL;
}

static const char* dlopen_last_error(void) {
#ifdef _WIN32
    static char buf[64];
    snprintf(buf, sizeof(buf), "Win32=%lu", (unsigned long)GetLastError());
    return buf;
#else
    const char* e = dlerror();
    return e ? e : "(no dlerror)";
#endif
}

/* ================================================================
 * Loader 1 : Driver Manager pour sqlODBC generique
 *
 *  - Windows : LoadLibraryA("odbc32.dll") — DM natif OS.
 *  - Linux : dlopen sur libiodbc.so.2 extrait depuis le pack HFSQL
 *            vendore (via owl_hfsql_ensure_installed). On reutilise
 *            le meme pack pour les deux backends Linux : sqlHFSQL
 *            l'utilise pour libiodbc + les .so HFSQL, sqlODBC pour
 *            libiodbc seulement. Trade-off : ~10 MB extrait meme si
 *            l'user n'utilise que sqlODBC. Compense par : zero
 *            dependance unixODBC system, fonctionne meme sur des
 *            distros minimalistes.
 *
 * iODBC est compatible avec la majorite des drivers ODBC Linux
 * (Oracle, MS SQL Server, MySQL Connector/ODBC, PostgreSQL psqlODBC,
 * Firebird, etc.). Le user installe son driver tiers ailleurs (apt
 * ou tarball) et iODBC le charge via SQLDrivers / DSN.
 * ================================================================ */

static OwlOdbcVT g_vt_default;
static bool      g_vt_default_loaded = false;
static bool      g_vt_default_failed = false;

const OwlOdbcVT* owl_odbc_vt_load_default(char* err_out, size_t err_cap) {
    if (g_vt_default_loaded) return &g_vt_default;
    if (g_vt_default_failed) {
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap, "ODBC: Driver Manager indisponible");
        }
        return NULL;
    }

    /* Resolution du path vers le DM :
     *  - Windows : LoadLibraryA cherche odbc32.dll dans System32 (auto).
     *  - Linux : on extrait libiodbc depuis le pack HFSQL vendore puis
     *    dlopen avec le path absolu, sinon le loader Linux echoue car
     *    libiodbc.so.2 n'est probablement pas dans LD_LIBRARY_PATH. */
#ifdef _WIN32
    const char* dm_path = OWL_DM_DEFAULT_NAME;
#else
    char dm_path_buf[1100];
#  if defined(OWL_HAS_HFSQL_EMBED) && OWL_HAS_HFSQL_EMBED
    char hfsql_err[512];
    if (!owl_hfsql_ensure_installed(hfsql_err, sizeof(hfsql_err))) {
        g_vt_default_failed = true;
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "ODBC: iODBC extract echec : %s", hfsql_err);
        }
        return NULL;
    }
    char install_dir[1024];
    if (!owl_hfsql_get_install_dir(install_dir, sizeof(install_dir))) {
        g_vt_default_failed = true;
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap, "ODBC: install_dir introuvable");
        }
        return NULL;
    }
    snprintf(dm_path_buf, sizeof(dm_path_buf), "%s/%s",
             install_dir, OWL_DM_DEFAULT_NAME);
    /* Pre-load libiodbcinst aussi : certains drivers attendent ses
     * symboles disponibles via RTLD_GLOBAL au moment du SQLDriverConnect. */
    char iodbcinst_path[1100];
    snprintf(iodbcinst_path, sizeof(iodbcinst_path), "%s/libiodbcinst.so.2",
             install_dir);
    dlopen(iodbcinst_path, RTLD_NOW | RTLD_GLOBAL);
    const char* dm_path = dm_path_buf;
#  else
    /* Fallback sans pack embed : on tente le DM systeme dans
     * LD_LIBRARY_PATH. L'user doit avoir libiodbc OU libodbc installe. */
    const char* dm_path = OWL_DM_DEFAULT_NAME;
#  endif
#endif

    void* h = OWL_DLOPEN(dm_path);
    if (!h) {
        g_vt_default_failed = true;
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "ODBC: dlopen(%s) echec : %s",
                     dm_path, dlopen_last_error());
        }
        return NULL;
    }

    memset(&g_vt_default, 0, sizeof(g_vt_default));
    g_vt_default.dll_handle = h;
    g_vt_default.source_label = OWL_DM_DEFAULT_NAME;
    const char* missing = vt_resolve_all(&g_vt_default, /*include_drivers_w=*/true);
    if (missing) {
        g_vt_default_failed = true;
        OWL_DLCLOSE(h);
        memset(&g_vt_default, 0, sizeof(g_vt_default));
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "ODBC: symbole '%s' introuvable dans %s",
                     missing, OWL_DM_DEFAULT_NAME);
        }
        return NULL;
    }
    g_vt_default_loaded = true;
    return &g_vt_default;
}

/* ================================================================
 * Loader 2 : driver HFSQL direct (bypass Driver Manager)
 *
 *  - Windows : LoadLibraryA sur le driver ODBC embarque + SetDllDirectoryA
 *              pour que les DLLs cousines soient resolues automatiquement.
 *  - Linux   : dlopen sur le driver ODBC embarque. Pre-requis : LD_LIBRARY_PATH
 *              positionne sur le dossier d'install AVANT le process,
 *              ou bien le caller s'en charge (cf owl_hfsql_install qui
 *              extrait aussi libiodbc.so.2 + libiodbcinst.so.2 dans le
 *              meme dossier — le driver les charge via dlopen
 *              dynamic-link).
 *
 * Le driver expose les memes symboles SQLxxxW directement, prouve via
 * objdump (Windows) / nm -D (Linux). Cf. notes internes sur les backends SQL.
 * ================================================================ */

#if defined(OWL_HAS_HFSQL_EMBED) && OWL_HAS_HFSQL_EMBED
/* Etat du driver HFSQL embarque. Uniquement quand le pack est vendore : en
 * EMBED=0 (ex. build public, pack absent) la fonction retourne tot sur la
 * branche d'erreur et ces statiques seraient inutilisees (-Werror). */
static OwlOdbcVT g_vt_hfsql;
static bool      g_vt_hfsql_loaded = false;
#endif

const OwlOdbcVT* owl_odbc_vt_load_hfsql_direct(char* err_out, size_t err_cap) {
#if !defined(OWL_HAS_HFSQL_EMBED) || !OWL_HAS_HFSQL_EMBED
    if (err_out && err_cap > 0) {
        snprintf(err_out, err_cap,
                 "HFSQL embed indisponible sur cette plateforme "
                 "(OWL_HAS_HFSQL_EMBED=0 — pas de pack vendore)");
    }
    return NULL;
#else
    if (g_vt_hfsql_loaded) return &g_vt_hfsql;

    /* 1. Assure l'extraction du pack HFSQL (idempotent). */
    char hfsql_err[512];
    if (!owl_hfsql_ensure_installed(hfsql_err, sizeof(hfsql_err))) {
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap, "HFSQL extract echec: %s", hfsql_err);
        }
        return NULL;
    }

    /* 2. Resoudre le dossier d'install. */
    char install_dir[1024];
    if (!owl_hfsql_get_install_dir(install_dir, sizeof(install_dir))) {
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap, "HFSQL: install dir introuvable");
        }
        return NULL;
    }

    /* 3. Plateforme-dependant : pre-load search path pour les
     *    dependances cousines (libiodbc Linux, DLLs cousines Windows). */
#ifdef _WIN32
    SetDllDirectoryA(install_dir);
#else
    /* Sur Linux on doit set LD_LIBRARY_PATH AVANT le process pour
     * que le loader le voit. setenv apres le start n'a aucun effet
     * sur ld.so. Solution : on dlopen explicitement libiodbc d'abord
     * (avec son path complet), ce qui le charge en RTLD_GLOBAL puis
     * le driver le retrouvera via cache du loader. */
    char iodbc_path[1100];
    snprintf(iodbc_path, sizeof(iodbc_path), "%s%slibiodbc.so.2",
             install_dir, OWL_PATH_SEP);
    void* h_iodbc = dlopen(iodbc_path, RTLD_NOW | RTLD_GLOBAL);
    if (!h_iodbc) {
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "HFSQL: dlopen(%s) echec : %s",
                     iodbc_path, dlopen_last_error());
        }
        return NULL;
    }
    /* libiodbcinst peut etre demandee par le driver — on la pre-load
     * idem (RTLD_GLOBAL pour exposer ses symboles aux libs suivantes). */
    char iodbcinst_path[1100];
    snprintf(iodbcinst_path, sizeof(iodbcinst_path), "%s%slibiodbcinst.so.2",
             install_dir, OWL_PATH_SEP);
    void* h_iodbcinst = dlopen(iodbcinst_path, RTLD_NOW | RTLD_GLOBAL);
    (void)h_iodbcinst;  /* Pas critique si absent — certaines distros bundle */
#endif

    /* 4. dlopen sur le driver lui-meme. */
    char drv_path[1100];
    snprintf(drv_path, sizeof(drv_path), "%s%s%s",
             install_dir, OWL_PATH_SEP, OWL_HFSQL_DRIVER_DLL_NAME);
    void* h = OWL_DLOPEN(drv_path);
    if (!h) {
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "HFSQL: dlopen(%s) echec : %s",
                     drv_path, dlopen_last_error());
        }
#ifdef _WIN32
        SetDllDirectoryA(NULL);
#endif
        return NULL;
    }

    memset(&g_vt_hfsql, 0, sizeof(g_vt_hfsql));
    g_vt_hfsql.dll_handle = h;
    g_vt_hfsql.source_label = OWL_HFSQL_DRIVER_DLL_NAME;
    const char* missing = vt_resolve_all(&g_vt_hfsql, /*include_drivers_w=*/false);
    if (missing) {
        OWL_DLCLOSE(h);
#ifdef _WIN32
        SetDllDirectoryA(NULL);
#endif
        memset(&g_vt_hfsql, 0, sizeof(g_vt_hfsql));
        if (err_out && err_cap > 0) {
            snprintf(err_out, err_cap,
                     "HFSQL: symbole '%s' introuvable dans %s",
                     missing, OWL_HFSQL_DRIVER_DLL_NAME);
        }
        return NULL;
    }

    /* Search path reste pose volontairement : si le driver fait des
     * LoadLibrary/dlopen lazy pendant SQLDriverConnectW, ses cousines
     * doivent rester accessibles. */
    g_vt_hfsql_loaded = true;
    return &g_vt_hfsql;
#endif
}

#endif  /* OWL_HAS_ODBC */
