// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* Assemblage des chaines de connexion SQL — briques PURES.
 *
 * Ces fonctions ne prennent que des arguments simples (chaines C, entiers) :
 * ni ObjInstance, ni VM, ni allocation GC. C'est ce qui les rend assertables
 * telles quelles par un self-test C, alors qu'aucun serveur HFSQL n'est
 * disponible en integration continue (cf. --self-test-net).
 *
 * Le module demarre avec HFSQL mais la resolution hote/port est
 * commune a tous les connecteurs reseau : Postgres, MySQL et MSSQL peuvent
 * la rejoindre en passant leur champ Port a owl_sql_resolve_host_port au
 * lieu de l'appliquer eux-memes apres coup.
 */

#ifndef OWL_SQL_CNX_STRING_H
#define OWL_SQL_CNX_STRING_H

#include <stdint.h>
#include <stddef.h>

/* Port par defaut du moteur HFSQL Client/Serveur. */
#define OWL_SQL_HFSQL_PORT_DEFAUT 4900

/* Resout le couple (hote, port) d'un connecteur reseau.
 *
 * `serveur`     : "hote", "hote:port", ou "[ipv6]:port".
 * `port_champ`  : valeur de la propriete Port du connecteur ; 0 = non
 *                 renseignee. Un port dans les bornes ]0, 65536[ l'emporte
 *                 sur celui contenu dans `serveur` ; hors bornes il est
 *                 ignore sans erreur.
 * `port_defaut` : port du backend quand aucune des deux sources n'en donne.
 * `hote_buf`    : recoit l'hote debarrasse de son port (taille >= 256
 *                 conseillee).
 *
 * Retourne le port retenu. */
int owl_sql_resolve_host_port(const char* serveur, int64_t port_champ,
                              int port_defaut,
                              char* hote_buf, size_t hote_buf_taille);

/* Assemble la chaine de connexion HFSQL Client/Serveur.
 *
 * Format de la chaine de connexion du driver HFSQL :
 *   DRIVER={HFSQL};Server Name=<hote>;Server Port=<port>;Database=...;UID=...;PWD=...
 *
 * Les segments dont la valeur est vide ou NULL sont omis ; Server Name /
 * Server Port ne sont emis que si `serveur` est non vide. `port_champ` suit
 * la regle de preseance d'owl_sql_resolve_host_port.
 *
 * Retourne un buffer malloc a liberer par l'appelant, ou NULL si
 * l'allocation echoue. */
char* owl_sql_build_hfsql_cnx_string(const char* serveur, int64_t port_champ,
                                     const char* base, const char* utilisateur,
                                     const char* mot_de_passe);

#endif /* OWL_SQL_CNX_STRING_H */
