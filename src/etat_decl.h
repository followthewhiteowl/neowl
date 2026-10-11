// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* etat_decl.h - IR des declarations Etat (DSL V2).
 *
 * Apres parsing par compiler.c::etat_declaration, chaque
 * "ETA_X est un Etat ... FIN" produit un OwlEtatDecl stocke dans
 * vm.etats. La native iImprimeEtat (V2 runtime) consomme cet IR.
 *
 * Reuses : OwlPropertyInit + OwlEventBlock de ui_decl.h.
 */

#ifndef OWL_ETAT_DECL_H
#define OWL_ETAT_DECL_H

#include "ui_decl.h"   /* OwlPropertyInit, OwlEventBlock, OwlControlDecl */
#include "value.h"
#include "object.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types de blocs Etat (canon) ---- */

typedef enum {
    OWL_ETAT_BLOCK_ENTETEDOC,   /* EnTeteDocument — 1x au debut */
    OWL_ETAT_BLOCK_HAUTPAGE,    /* HautDePage     — chaque page */
    OWL_ETAT_BLOCK_CORPS,       /* Corps          — 1x par enregistrement */
    OWL_ETAT_BLOCK_BASPAGE,     /* BasDePage      — chaque page */
    OWL_ETAT_BLOCK_FINDOC,      /* FinDeDocument  — 1x a la fin */
    OWL_ETAT_BLOCK_ITERATION,   /* Iteration      — bloc libre, appele via iImprimeBloc */
} OwlEtatBlockKind;

/* Resolve "EnTeteDocument" / "Corps" / etc. -> kind. -1 si inconnu. */
int         owl_etat_block_kind_from_name(const char* name);

/* ---- Declaration d'un sous-bloc d'Etat ----
 *
 * "INTRO est un EnTeteDocument PROPRIETES Hauteur=40 FIN <code> FIN"
 *
 * - kind  : un des 5 canon ou ITERATION
 * - name  : nom utilisateur (ex. "INTRO", "LIGNE_ARTICLE")
 * - properties : Hauteur (V2.0), autres props futures
 * - body  : bytecode du code canon inline du bloc (sera execute par iImprimeEtat)
 */
typedef struct {
    OwlEtatBlockKind   kind;
    char*              name;             /* "INTRO" (alloue) */
    OwlPropertyInit*   properties;
    int                property_count;
    int                property_capacity;
    ObjFunction*       body;             /* Code du bloc (peut etre NULL si vide) */
    /* Champs WYSIWYG (Libelle/Image/...) places dans le bloc. Imprimes au
     * runtime apres l'execution du body via iImprimeZone + iPolice. */
    OwlControlDecl**   controls;
    int                control_count;
    int                control_capacity;
    int                line;
} OwlEtatBlockDecl;

/* ---- Parametre d'un Etat (canon "Etat parametre") ---- */
typedef struct {
    char*  name;          /* "client", "maSource" (alloue) */
    char*  type_name;     /* "chaine", "Personne", NULL si non type (alloue ou NULL) */
    Value  default_value; /* Litteral par defaut si has_default, sinon NIL_VAL */
    bool   has_default;   /* true si `= <litteral>` fourni a la declaration */
    int    line;
} OwlEtatParam;

/* ---- Declaration d'Etat top-level ----
 *
 * Stockage : table globale vm.etats indexee par nom (ETA_X).
 * Cree au moment du parsing. Consommee par iImprimeEtat.
 *
 * Syntaxe canon OWL V2 :
 *   ETAT_X est un Etat(client est une Personne, lignes est un tableau de Article)
 *       PROPRIETES ... FIN
 *       <body code = equivalent Ouverture canon>
 *       <NOM> est un EnTeteDocument ... FIN
 *       PROCEDURE Filtre/Lecture/Fermeture
 *   FIN
 *
 * Les params declares dans 'est un Etat(...)' sont accessibles comme
 * globales dans tout le scope de l'Etat (top-level + sub-blocs +
 * PROCEDUREs). Au runtime iImprimeEtat injecte leurs valeurs avant
 * d'executer le code.
 */
typedef struct {
    Obj                obj;              /* Heritage Obj pour GC */
    char*              name;             /* "ETA_Facture" (alloue) */
    OwlPropertyInit*   properties;       /* Format, Orientation, Marges */
    int                property_count;
    int                property_capacity;
    OwlEtatBlockDecl*  blocks;           /* EnTeteDoc, HautPage, Corps, ... */
    int                block_count;
    int                block_capacity;
    OwlEventBlock*     events;           /* Filtre, Lecture, Fermeture */
    int                event_count;
    int                event_capacity;
    OwlEventBlock*     user_procs;       /* PROCEDURE custom */
    int                user_proc_count;
    int                user_proc_capacity;
    /* Liste des params Etat. Vide si Etat sans params. */
    OwlEtatParam*      params;
    int                param_count;
    int                param_capacity;
    /* Reserve : ObjFunction de capture (pas utilise actuellement, vient
     * de window_decl par symetrie. A retirer plus tard si confirme). */
    ObjFunction*       param_capture;
    /* Code top-level de l'Etat (decls + statements orphelins = Ouverture
     * implicite).  : ENDORMI jusqu'a la 1re impression. Execute
     * UNE seule fois (top_level_done), pas au boot, pas par impression. */
    ObjFunction*       top_level;
    bool               top_level_done;
    int                line;
} OwlEtatDecl;

/* ---- API ---- */

OwlEtatDecl*       owl_etat_decl_new(const char* name, int line);
void               owl_etat_decl_free(OwlEtatDecl* e);

void               owl_etat_decl_add_property(OwlEtatDecl* e, const char* name,
                                                Value const_value, ObjFunction* eval,
                                                bool is_constant, int line);
void               owl_etat_decl_add_event(OwlEtatDecl* e, const char* name,
                                             ObjFunction* impl, int line);
void               owl_etat_decl_add_user_proc(OwlEtatDecl* e, const char* name,
                                                  ObjFunction* impl, int line);

OwlEtatBlockDecl*  owl_etat_decl_add_block(OwlEtatDecl* e, OwlEtatBlockKind kind,
                                              const char* name, int line);
void               owl_etat_block_decl_add_property(OwlEtatBlockDecl* b, const char* name,
                                                       Value const_value, ObjFunction* eval,
                                                       bool is_constant, int line);
void               owl_etat_block_decl_add_control(OwlEtatBlockDecl* b, OwlControlDecl* c);

/* Declare un parametre d'Etat (`est un Etat(<name> [est un <type>] [= <defaut>])`).
 * has_default=false + default_value ignore quand le param n'a pas de defaut. */
void               owl_etat_decl_add_param(OwlEtatDecl* e, const char* name,
                                              const char* type_name,
                                              Value default_value, bool has_default,
                                              int line);

/* Lookup */
const OwlEventBlock*   owl_etat_decl_find_event(const OwlEtatDecl* e, const char* name);
const OwlEtatBlockDecl* owl_etat_decl_find_block_by_name(const OwlEtatDecl* e, const char* name);
const OwlEtatBlockDecl* owl_etat_decl_find_canon_block(const OwlEtatDecl* e, OwlEtatBlockKind kind);

#ifdef __cplusplus
}
#endif

#endif /* OWL_ETAT_DECL_H */
