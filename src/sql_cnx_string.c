// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "sql_cnx_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

int owl_sql_resolve_host_port(const char* serveur, int64_t port_champ,
                              int port_defaut,
                              char* hote_buf, size_t hote_buf_taille) {
    int port = port_defaut;
    if (hote_buf_taille > 0) hote_buf[0] = '\0';
    if (serveur == NULL) serveur = "";

    bool hote_pose = false;

    /* Cas IPv6 entre crochets : "[::1]:4900". */
    if (serveur[0] == '[') {
        const char* fin = strchr(serveur, ']');
        if (fin != NULL) {
            size_t hlen = (size_t)(fin - serveur - 1);
            if (hlen < hote_buf_taille) {
                memcpy(hote_buf, serveur + 1, hlen);
                hote_buf[hlen] = '\0';
                hote_pose = true;
                if (fin[1] == ':') {
                    int p = atoi(fin + 2);
                    if (p > 0 && p < 65536) port = p;
                }
            }
        }
    }

    /* Cas "hote:port" (dernier ':'). */
    if (!hote_pose) {
        const char* colon = strrchr(serveur, ':');
        if (colon != NULL) {
            /* Ce qui suit doit etre purement numerique, sinon ce n'est pas un
             * port (IPv6 non crochete, par exemple) et le champ Port explicite
             * reste la seule voie. */
            bool est_port = (colon[1] != '\0');
            for (const char* p = colon + 1; *p; p++) {
                if (*p < '0' || *p > '9') { est_port = false; break; }
            }
            if (est_port) {
                int p = atoi(colon + 1);
                if (p > 0 && p < 65536) {
                    size_t hlen = (size_t)(colon - serveur);
                    if (hlen < hote_buf_taille) {
                        memcpy(hote_buf, serveur, hlen);
                        hote_buf[hlen] = '\0';
                        hote_pose = true;
                        port = p;
                    }
                }
            }
        }
    }

    if (!hote_pose) snprintf(hote_buf, hote_buf_taille, "%s", serveur);

    /* Port explicite prioritaire sur celui deduit de `serveur`. */
    if (port_champ > 0 && port_champ < 65536) port = (int)port_champ;

    return port;
}

char* owl_sql_build_hfsql_cnx_string(const char* serveur, int64_t port_champ,
                                     const char* base, const char* utilisateur,
                                     const char* mot_de_passe) {
    size_t cap = 128
        + (serveur      ? strlen(serveur)      : 0)
        + (base         ? strlen(base)         : 0)
        + (utilisateur  ? strlen(utilisateur)  : 0)
        + (mot_de_passe ? strlen(mot_de_passe) : 0);
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t off = (size_t)snprintf(out, cap, "DRIVER={HFSQL};");

    if (serveur && serveur[0]) {
        char hote[256];
        int port = owl_sql_resolve_host_port(serveur, port_champ,
                                             OWL_SQL_HFSQL_PORT_DEFAUT,
                                             hote, sizeof(hote));
        off += (size_t)snprintf(out + off, cap - off,
                                "Server Name=%s;Server Port=%d;", hote, port);
    }
    if (base && base[0]) {
        off += (size_t)snprintf(out + off, cap - off, "Database=%s;", base);
    }
    if (utilisateur && utilisateur[0]) {
        off += (size_t)snprintf(out + off, cap - off, "UID=%s;", utilisateur);
    }
    if (mot_de_passe && mot_de_passe[0]) {
        off += (size_t)snprintf(out + off, cap - off, "PWD=%s;", mot_de_passe);
    }
    return out;
}
