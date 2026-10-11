// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_theme.c — Namespace `theme.<nom>` (styles nommes du theme actif).
 *
 *  Lot 2b+3 — expose un singleton ObjThemeNamespace globalement sous
 * les noms canon `theme` (FR) et `Theme` (EN). Le dispatch runtime vit dans
 * vm.c (OP_STRUCT_GET sur ObjThemeNamespace) : il resout `theme.<nom>` vers
 * l'instance de style nommee declaree par l'utilisateur.
 *
 * Modele de resolution : les styles nommes sont declares au top-level du
 * fichier designe par Projet..Theme (`boutonPrimaire est un styleBouton`).
 * Ce fichier est un compagnon du projet, initialise AVANT le top-level du
 * fichier lance ; au moment ou `theme.<nom>` s'execute, le global existe
 * deja dans la table des globals. `theme.<nom>` delegue donc a la resolution
 * canon des globals (casse + accents insensibles) et valide que la cible est
 * bien une instance de style typee (structName prefixe "style"). Un nom
 * absent (ou un global du mauvais type) pose une erreur soft (ErreurDetectee)
 * sans crash, contrat calque sur le namespace `icones.<nom>`.
 *
 * Ce module ne porte que la partie independante de la VM (registration du
 * singleton + emission d'erreur + predicat de type) ; la lecture des globals
 * se fait cote vm.c ou vit vm_globals_canon_get. */

#include "owl_stdlib.h"
#include "owl_errors.h"
#include "object.h"
#include "vm.h"

#include <string.h>

/* Predicat : v est-elle une instance de style typee ? Les types de style du
 * catalogue UI ont un structName prefixe "style" (styleBouton, styleSaisie,
 * styleLibelle, ...). Seules ces instances sont resolvables via theme.<nom> :
 * cela evite qu'un global scalaire homonyme soit renvoye comme un style. */
bool owl_theme_is_style_instance(Value v) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    if (inst->structName == NULL) return false;
    return strncasecmp(inst->structName->chars, "style", 5) == 0;
}

/* Emet l'erreur soft de miss (nom de style introuvable ou du mauvais type).
 * Code dedie : le template generique argument/attendu parlait d'un « argument
 * 'nom' » incomprehensible pour qui a simplement mal orthographie un style.
 * Le message nomme le style et pointe les deux causes probables (faute de
 * frappe, ou thème non designe par le fichier projet). */
void owl_theme_namespace_emit_miss(const char* name) {
    owl_emit_native_error(E_NATIVE_THEME_STYLE_UNKNOWN, "theme",
                          "nom", name != NULL ? name : "", NULL);
}

/* structName reserve de la sentinelle de miss. Le '$' le rend inatteignable
 * par une declaration utilisateur (identifiant illegal), donc pas de collision
 * avec un type de style reel (owl_theme_is_style_instance filtre "style*"). */
#define THEME_MISS_SENTINEL_NAME "$themeMiss"

/* Vraie / faux : v est-elle la sentinelle navigable d'un miss theme ? */
bool owl_theme_is_miss_sentinel(Value v) {
    if (!IS_INSTANCE(v)) return false;
    ObjInstance* inst = AS_INSTANCE(v);
    return inst->structName != NULL
        && strcmp(inst->structName->chars, THEME_MISS_SENTINEL_NAME) == 0;
}

/* Fabrique la sentinelle : instance vide au structName reserve. Le miss ne
 * connait pas le TYPE de style attendu (styleBouton ? styleSaisie ?), donc pas
 * de template hydrate exact possible. Le null-object n'a aucun champ ; le
 * dispatch OP_STRUCT_GET reconnait le structName et se replie sur la sentinelle
 * elle-meme a chaque sous-acces inconnu, ce qui rend le chainage `..X..Y`
 * tolere sans NIL intermediaire (donc sans second crash). Aucune valeur reelle
 * n'est promise : c'est un vehicule de navigation resilient, pas un style. */
Value owl_theme_make_miss_sentinel(const char* name) {
    (void)name;  /* nom deja consomme par l'emission d'erreur du caller */
    ObjString* sn = obj_copy_string(THEME_MISS_SENTINEL_NAME,
                                    (int)strlen(THEME_MISS_SENTINEL_NAME));
    vm_push(OBJ_VAL(sn));   /* root sn pendant obj_new_instance (peut GC) */
    ObjInstance* inst = obj_new_instance(sn);
    vm_pop();
    return OBJ_VAL(inst);
}

void stdlib_theme_init(void) {
    /* Namespace `theme.<nom>` : singleton ObjThemeNamespace expose sous les
     * deux noms canon (FR/EN). Le lookup est dispatch par vm.c sur
     * OP_STRUCT_GET vers la resolution des globals de style. */
    ObjThemeNamespace* ns = obj_new_theme_namespace();
    obj_globals_set("theme", OBJ_VAL(ns));
    obj_globals_set("Theme", OBJ_VAL(ns));
}
