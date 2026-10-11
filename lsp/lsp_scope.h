// SPDX-License-Identifier: AGPL-3.0-only
#ifndef owl_lsp_scope_h
#define owl_lsp_scope_h

#include <stdbool.h>

/* ================================================================
 * MOTEUR DE PORTEE / LIAISON du LSP ( Lot 0, ).
 *
 * Resolveur lexical DEDIE (pas d'instrumentation du compilateur,
 *  §1) : pour les documents ouverts, construit un arbre de
 * portees (ScopeNode) + une table de liaisons (Binding) permettant de
 * resoudre une occurrence de nom vers SA declaration precise, en tenant
 * compte de la portee lexicale (deux procedures sœurs declarant chacune
 * un `x` ne sont plus confondues).
 *
 * Liaison lexicale seulement : ni inference de type, ni resolution de
 * surcharge. L'axe MEMBER (`recv.champ`) reste resolu par le
 * chemin recepteur->type existant de handle_definition ; ce moteur ne
 * traite que l'axe VALUE (locales / parametres / globales) pour le Lot 0.
 * ================================================================ */

/* Trois axes de namespace independants qu'un rename ne doit JAMAIS
 * fusionner ( §2, , invariant 5). L'axe est infere au
 * site d'usage. */
typedef enum {
    SCOPE_AXIS_VALUE = 0,  /* valeur : vm.globals, locales, parametres */
    SCOPE_AXIS_TYPE,       /* nom de type : `est un/une <Type>` */
    SCOPE_AXIS_MEMBER      /* membre : precede de `.` / `:` / `..` */
} ScopeAxis;

/* Resultat d'une resolution : localisation de la declaration liee. */
typedef struct {
    int  docIndex;   /* doc ouvert (>=0) ; -1 si non applicable */
    int  line;       /* 0-based */
    int  character;  /* 0-based */
    int  nameLen;    /* longueur (octets) du nom declare, forme verbatim */
    bool found;
} ScopeResolveResult;

/* Une occurrence liee (declaration ou usage) renvoyee par les references. */
typedef struct {
    int docIndex;   /* doc ouvert (>=0) */
    int line;       /* 0-based */
    int character;  /* 0-based */
    int nameLen;    /* octets du nom (forme du declarant) */
} ScopeReference;

/* Reconstruit l'arbre de portees + la table de bindings pour TOUS les
 * documents ouverts. Appele depuis scan_user_symbols_force : gratuit vis
 * a vis du cache fingerprint existant (rebuild seulement si un buffer
 * change). Idempotent. */
void scope_build(void);

/* Resout l'occurrence VALUE du mot `word` (forme verbatim tapee) a la
 * position (uri, line, char) vers sa declaration lexicale. Renvoie true
 * et remplit `out` si un binding VALUE visible est trouve dans la chaine
 * de portees englobantes ; false sinon (l'appelant retombe alors sur ses
 * fallbacks : userSymbols plat, index disque cross-fichier...). */
bool scope_resolve_at(const char* uri, int line, int character,
                      const char* word, ScopeResolveResult* out);

/* Resout le binding VALUE dont une occurrence couvre la position (uri, line,
 * character) et ecrit ses occurrences liees dans `out` (au plus `maxOut`).
 * Si `includeDeclaration` est faux, la declaration (occurrence[0]) est omise.
 * Renvoie le nombre d'occurrences ecrites ; 0 si aucun binding a la position
 * (l'appelant renvoie alors un tableau vide, pas une erreur). */
int scope_references_of(const char* uri, int line, int character,
                        bool includeDeclaration,
                        ScopeReference* out, int maxOut);

/* Resout le binding VALUE couvrant la position (uri, line, character) et
 * indique s'il est renommable — locale, parametre OU globale user-declared
 * (Lot 2b : la portee top-level est desormais renommable,  garantit
 * l'unicite projet-wide d'une globale VALUE). *outIsGlobal signale une globale
 * (portee de declaration KIND_TOPLEVEL) : l'appelant etend alors le rename a
 * TOUT le projet via scope_collect_value_occurrences. *outCanonKey (buffer
 * `canonKeySize`) recoit la cle canonique du binding, pour la collecte cross-
 * fichier. outIsGlobal / outCanonKey peuvent etre NULL (prepareRename n'en a
 * pas besoin). Ecrit les occurrences DU DOC COURANT dans `out` (au plus
 * `maxOut`) et la longueur du nom dans *outNameLen. Renvoie le nombre
 * d'occurrences ecrites ; 0 si aucun binding a la position (native, mot-cle,
 * position non liee). Partage la resolution de scope_references_of. */
int scope_rename_query(const char* uri, int line, int character,
                       bool* outRenamable, int* outNameLen,
                       bool* outIsGlobal, char* outCanonKey, int canonKeySize,
                       ScopeReference* out, int maxOut);

/* Collecte les occurrences VALUE non-membres de `canonKey` dans `source`
 * (rename cross-fichier d'une globale, Lot 2b). Tokenise la source et retient
 * chaque TOKEN_IDENTIFIER dont la cle canonique egale `canonKey` et qui n'est
 * PAS un acces membre (non precede de `.` / `:` / `..` / `::`, namespace
 * disjoint  §2).  : une globale VALUE etant unique projet-wide,
 * toutes ces occurrences sont le meme symbole. Ecrit {line, character, nameLen}
 * (docIndex=-1, a la charge de l'appelant) dans `out` (au plus `maxOut`).
 * Renvoie le nombre d'occurrences ecrites. */
int scope_collect_value_occurrences(const char* source, const char* canonKey,
                                    ScopeReference* out, int maxOut);

#endif
