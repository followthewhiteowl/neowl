// SPDX-License-Identifier: AGPL-3.0-only
/* Noms de fixture uniques par processus.
 *
 * Mesure  : deux `--self-test-net` simultanes lances depuis le meme
 * repertoire partagent leurs fichiers de capture a nom constant
 * (`_owl_http_llhttp_test.tmp`, `%TEMP%/owl_pol_racine`, ...). L'un tronque ou
 * efface ce que l'autre est en train de lire, la sortie capturee se melange, et
 * des dizaines d'assertions tombent sans qu'une seule ligne du code sous test
 * n'ait bouge. A/B mesure : 21 executions solo -> 0 echec ; 6 paires
 * simultanees -> 5 paires rouges, 13 a 20 assertions chacune.
 *
 * Le correctif appartient au test, pas a l'appelant : suffixer par le PID rend
 * l'etat de fixture propre au processus, donc le resultat independant de la
 * presence d'un voisin. Un verrou de suite aurait fait payer ~200 s d'attente a
 * chaque agent concurrent sans traiter le partage cache lui-meme. */
#ifndef OWL_FIXTURE_UNIQUE_H
#define OWL_FIXTURE_UNIQUE_H

#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32
  #include <process.h>
  #define owl_fixture_pid() ((long)_getpid())
#else
  #include <unistd.h>
  #define owl_fixture_pid() ((long)getpid())
#endif

/* `prefixe` + PID + `suffixe`. Un seul processus peut donc creer, tronquer et
 * effacer le chemin rendu. */
static void fixture_unique(char* out, size_t taille,
                           const char* prefixe, const char* suffixe) {
    snprintf(out, taille, "%s%ld%s", prefixe, owl_fixture_pid(), suffixe);
}

#endif /* OWL_FIXTURE_UNIQUE_H */
