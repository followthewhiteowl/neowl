// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_SERVE_H
#define OWL_SERVE_H

/* `owl serve <fichier.owl>` — montage minimal qui compile un fichier
 * declarant un ServiceREST et le sert sur un vrai port TCP.
 *
 * De la GLUE, pas un nouveau sous-systeme : reunit le compilateur (qui peuple
 * le registre de routes runtime), l'executeur owning-VM (http_exec.c) et le
 * transport (http_server.c) derriere un listener reel binde. Le premier jet
 * est mono-executeur / boucle mono-thread (VM mono-thread, concurrence-par-
 * processus ).
 *
 * Cycle de vie : prepare (compile + bind, SYNCHRONE) -> demarre (boucle sur un
 * thread dedie) -> attend (join) -> libere.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* bind_adr;   /* NULL -> "127.0.0.1" (defaut sur, pas d'exposition) */
    int         port;       /* 0 -> port ephemere (comme owl_tcp_listener_ouvre) */
} OwlServeOptions;

typedef struct OwlServeMontage OwlServeMontage;

/* Projet a servir : la frontiere resolue par l'appelant (`owl serve`
 * suit le meme moteur que `owl <fichier>` / `owl .`). Tableaux paralleles
 * empruntes le temps de owl_serve_prepare_projet ; l'executeur en prend une
 * copie profonde. `entree` indexe le fichier lance ; `mode_projet` != 0 quand
 * un projet.owl gouverne la frontiere (scan recursif). */
typedef struct {
    const char* const* noms;      /* stems (name) par fichier */
    const char* const* chemins;   /* chemins complets (messages d'erreur) */
    const char* const* sources;   /* contenus */
    int                nb;        /* nombre de fichiers */
    int                entree;    /* index du fichier d'entree */
    int                mode_projet;/* isProjectMode (bool) */
} OwlServeProjet;

/* Compile TOUS les fichiers de la frontiere projet (compagnons defs+corps AVANT
 * l'entree) et peuple owl_service_registry, SANS binder de socket. Retourne le
 * nombre TOTAL de services enregistres, ou -1 si la compilation echoue (err_out
 * rempli).
 *
 * En SUCCES, la VM reste VIVANTE au retour et les registres (services +
 * constantes) sont peuples : l'appelant lit owl_service_registry_at(...) / genere
 * le document OpenAPI (qui resout httpOK -> 200 via le registre de constantes),
 * PUIS appelle vm_free() suivi de compiler_reset_poo_state(). En ECHEC (-1), la
 * VM est deja liberee (l'appelant retourne sans nettoyer).
 *
 * Si `entry_service_start` != NULL (voie `owl openapi`, documentation
 * entry-specific), y depose le nombre de services declares par les COMPAGNONS :
 * l'entree etant compilee EN DERNIER (project_run phase 1.c), ses services
 * occupent le suffixe [*entry_service_start, total) du registre. serve passe
 * NULL -> aucune passe supplementaire, comportement inchange. */
int owl_serve_compile_projet(const OwlServeProjet* projet, int* entry_service_start,
                             char* err_out, size_t err_out_cap);

/* Compile TOUS les fichiers de la frontiere projet (compagnons defs+corps AVANT
 * l'entree), exige AU MOINS un ServiceREST declare (0 -> erreur claire dans
 * err_out, retour NULL). Bind un listener REEL, SYNCHRONE. Meme contrat de
 * retour que owl_serve_prepare. */
OwlServeMontage* owl_serve_prepare_projet(const OwlServeProjet* projet,
                                          const OwlServeOptions* opts,
                                          char* err_out, size_t err_out_cap);

/* Compile `source_owl` (fichier unique — sucre au-dessus de
 * owl_serve_prepare_projet a un seul fichier, mode script). Exige AU MOINS un
 * ServiceREST declare (0 -> erreur claire dans err_out, retour NULL). Bind un
 * listener REEL, SYNCHRONE : le port et l'adresse sont connus des le retour.
 * NULL sur erreur (fichier sans service, port deja pris, ...) avec err_out
 * rempli d'un message non vide (au moins 1 octet + NUL si err_out_cap > 0). */
OwlServeMontage* owl_serve_prepare(const char* source_owl,
                                   const OwlServeOptions* opts,
                                   char* err_out, size_t err_out_cap);

int         owl_serve_montage_port(const OwlServeMontage* m);
const char* owl_serve_montage_adresse(const OwlServeMontage* m);
const char* owl_serve_montage_service(const OwlServeMontage* m);

/* Demarre la boucle de service sur un thread dedie : accepte, sert via
 * l'executeur, repond, jusqu'a owl_tcp_server_arret_demande() (Ctrl-C, mode
 * production) OU jusqu'a ce que `max_connexions` aient ete servies (mode test ;
 * 0 = illimite). `out` recoit le message de demarrage (NULL = silencieux). */
bool owl_serve_demarre(OwlServeMontage* m, int max_connexions, FILE* out);

/* Bloque jusqu'a la fin du thread de boucle (max atteint / arret demande). */
void owl_serve_attend(OwlServeMontage* m);

void owl_serve_libere(OwlServeMontage* m);

#ifdef __cplusplus
}
#endif

#endif /* OWL_SERVE_H */
