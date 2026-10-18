// SPDX-License-Identifier: AGPL-3.0-only
#ifndef OWL_STARTUP_WINDOW_H
#define OWL_STARTUP_WINDOW_H

/* ================================================================
 * STARTUP WINDOW — ouverture de la fenetre de demarrage d'un projet
 * ================================================================
 * Logique partagee entre le lancement projet de owl.exe (main.c) et le
 * main() genere par owl-build (project_run.c, bundle autonome). Vit dans
 * owl_core (Qt-free) : la native Ouvre est resolue a l'execution via
 * vm.globals, donc cette unite n'a aucune dependance Qt directe. Le bundle
 * produit ne linke PAS main.c mais linke owl_core -> l'ouverture de la
 * fenetre de demarrage doit vivre ici pour etre disponible des deux cotes.
 *
 * Capture les arguments CLI additionnels (`--arg nom=valeur`) consommes par
 * le planificateur de parametres de la fenetre de demarrage. */
void owl_startup_set_cli_args(int argc, char** argv);

/* Simule `Ouvre(FEN[, v1, ...])` sur la fenetre nommee puis entre dans
 * l'event loop GUI (via la native Ouvre). Lookup global de Ouvre + plan des
 * parametres + vm_call_reentrant. Utilise la VM : DOIT etre appelee AVANT
 * vm_free() et apres que la metadonnee FenetreDemarrage soit peuplee par le
 * compile de projet.owl.
 *
 * Retour : 0 si OK (ou si Ouvre absent — build console minimal), 65 si
 * refus/erreur de parametrage, 70 si runtime error. */
int owl_run_startup_window(const char* fenName);

#endif /* OWL_STARTUP_WINDOW_H */
