// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test du garde de coherence pre-scan / parse des membres de
 * CollectionDeProcedures.
 *
 * CE QUE LE GARDE PROTEGE
 * -----------------------
 * Un membre de collection est indexe DEUX fois : par un scan lexical prealable
 * de tous les fichiers du projet (hydrate_collection_members), et par le parse
 * reel du fichier courant. La resolution d'un nom nu depuis un AUTRE fichier
 * n'utilise que la premiere. Quand le scan lexical rate un membre que le parse
 * definit correctement, la compilation reste verte et l'appel echoue au
 * runtime sur « Fonction ou procedure inconnue » — panne muette, sans la
 * moindre indication de la cause. C'est exactement le defaut qu'a corrige
 *  puis , deux fois, pour deux causes distinctes.
 * collection_prescan_cross_check transforme cette classe de panne en
 * diagnostic de compilation.
 *
 * POURQUOI CE FICHIER EXISTE
 * --------------------------
 * Un garde dont personne ne verifie qu'il mord encore devient du code mort
 * silencieux : il cesse de proteger sans qu'aucun signal ne soit emis, et le
 * defaut qu'il combattait revient par la porte de derriere. Le garde n'avait
 * jusqu'ici ete prouve que par une injection manuelle de desynchronisation,
 * defaite juste apres. Ce self-test rend la preuve permanente.
 *
 * LA MECANIQUE VIT DANS compiler.c
 * --------------------------------
 * Le garde, le registre des membres et le drapeau de provenance sont des
 * statics de compiler.c. Les exposer pour les besoins d'un test elargirait la
 * surface interne du compilateur bien au-dela de ce que le test justifie. Le
 * point d'entree collection_prescan_guard_self_test() vit donc dans
 * compiler.c ; ce fichier porte l'intention et le contrat, et sert de point
 * d'accroche a la convention des self-tests internes.
 *
 * LES CINQ CAS
 * ------------
 *  1. Parse-seul                -> diagnostic (la panne visee).
 *  2. Parse + pre-scan          -> muet (cas nominal).
 *  3. Pre-scan seul             -> muet (le garde est a SENS UNIQUE : un nom
 *                                  indexe que le parse ne definit pas arrive
 *                                  legitimement et ne prouve aucune panne).
 *  4. Membre confine par LOCAL  -> muet. LE CAS DECISIF : un garde qui accuse
 *                                  du code legitime coute plus cher que pas de
 *                                  garde du tout.
 *  5. Source hors perimetre du
 *     scan                      -> muet (comparer un parse a l'index d'un
 *                                  AUTRE texte ne prouve rien).
 *
 * VALIDATION PAR MUTATION (mesuree, pas deduite)
 * ----------------------------------------------
 *  - Neutraliser le garde (return immediat en tete de
 *    collection_prescan_cross_check)                    -> cas 1 rouge.
 *  - Retirer la condition de sens unique
 *    (`|| m->seenPrescan`)                              -> cas 1 et 3 rouges.
 *  - Compter LOCAL/GLOBAL comme ouvrants de bloc dans le scan lexical, ce qui
 *    fait sortir de l'index tous les membres declares APRES la section
 *    (drift plausible du suivi de profondeur)           -> cas 4 rouge.
 *  - Retirer la condition `g_collection_prescan_saw_source` -> cas 5 rouge.
 *
 * Note : simplement SUPPRIMER la bascule LOCAL/GLOBAL du scan ne fait rien
 * rougir, et c'est normal — le membre confine devient alors indexe sans etre
 * defini par le parse, ce que le garde ignore deliberement (sens unique).
 *
 * Pas de vm_init : le garde ne touche ni la VM ni le GC, seulement le scanner
 * et des tables statiques du compilateur. */

#include "../compiler.h"

int collection_prescan_guard_self_test_run(void) {
    return collection_prescan_guard_self_test();
}
