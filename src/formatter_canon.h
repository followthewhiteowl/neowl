// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_formatter_canon_h
#define owl_formatter_canon_h

/* ================================================================
 * FORMATTER CANONICAL-FORM PROVIDER
 * ================================================================
 * Le formatter canonise casse + accents des symboles builtin (natives,
 * constantes, types, champs, methodes, proprietes DSL) contre une source
 * de verite. Historiquement cette source etait le registre runtime
 * (owl_registry / owl_methods / compiler PROPS / ui_catalog), ce qui
 * forcait `owl fmt` a faire vm_init() + stdlib_init() pour peupler le
 * registre — et empechait l'hebergement du format dans owl-lsp.exe (qui
 * ne linke pas le runtime).
 *
 * Ce provider abstrait l'acces aux donnees derriere une vtable. Deux
 * backends interchangeables, MEME logique de matching cote formatter :
 *   - registry  (formatter_canon_registry.c, owl_core / owl.exe) :
 *     lit le registre vivant. Comportement historique exact.
 *   - catalogue (formatter_canon_catalog.c, owl-lsp.exe) : lit les tables
 *     statiques generees owl_catalog.h (OWL_NATIVE_CATALOG, ...). Aucune
 *     dependance runtime/VM.
 *
 * Le catalogue statique est une PROJECTION generee du registre vivant
 * (`owl --dump-lsp-catalog`), pas une liste maintenue a la main : zero
 * duplication de donnees. La parite des deux backends est garantie par
 * la suite fmt-stability tournee sur les deux (gate make).
 * ================================================================ */

typedef struct OwlFmtCanon {
    /* --- natives : ITERATION (matching cote formatter, source unique) ------
     * Le formatter parcourt canonical/ascii/EN/aliases de chaque native, dans
     * cet ordre, via owl_name_matches — exactement comme owl_registry_native_
     * canonical_for. Les deux backends exposent le MEME ensemble (natives
     * internes filtrees a l'identique du catalogue) -> parite par construction. */
    int         (*native_count)(void);
    const char* (*native_canonical)(int i); /* forme FR accentuee (a retourner) */
    const char* (*native_ascii)(int i);     /* FR accent-strip */
    const char* (*native_en)(int i);        /* EN (peut etre NULL ou == canonical) */
    int         (*native_alias_count)(int i);
    const char* (*native_alias)(int i, int aliasIndex);

    /* --- constantes (httpGet, RC, ValPI, iPDFGenerique...) : iteration ------ */
    int         (*const_count)(void);
    const char* (*const_name)(int i);

    /* --- proprietes des types primitifs builtin (date/heure/tableau/file/pile/
     * liste/JSON/duree) : iteration. `fr` = canon FR (forme retournee au match),
     * `en` = alias EN (NULL possible). Meme ensemble que OWL_BUILTIN_PROP_
     * CATALOG cote LSP. --- */
    int         (*builtin_prop_count)(void);
    const char* (*builtin_prop_fr)(int i);
    const char* (*builtin_prop_en)(int i);

    /* --- types builtin struct-like (httpRequete, Chrono, Definition...) --
     * Iteration. `type_canonical` = forme d'affichage accentuee (jamais
     * NULL). `type_ascii` = cle de match ASCII (== canonical si deja ASCII). */
    int         (*type_count)(void);
    const char* (*type_canonical)(int i);
    const char* (*type_ascii)(int i);
    /* genre grammatical du nom FR, valeurs OwlGenre encodees en
     * int (0=NONE, 1=M, 2=F). Consomme UNIQUEMENT par `owl translate` pour
     * l'article EN->FR. Backend catalogue : renvoie 0 (NONE) -- le catalogue
     * ne porte pas le genre, et `owl translate` ne tourne que dans owl.exe
     * (backend registry). Peut etre NULL si un backend ne l'expose pas. */
    int         (*type_genre)(int i);

    /* --- champs d'un type builtin -------------------------------------
     * `type_field_key` = cle de match (ASCII canon). `type_field_display`
     * = forme d'affichage (accentuee si dispo, sinon == key). */
    int         (*type_field_count)(int typeIndex);
    const char* (*type_field_key)(int typeIndex, int fieldIndex);
    const char* (*type_field_display)(int typeIndex, int fieldIndex);

    /* --- resolution TYPE-AWARE pour `owl translate` ---------
     * Consomme UNIQUEMENT par owl_translate sur la DSL des types composes
     * (themes/styles) : nom EN du type, nom EN d'un champ, et nom du type
     * du sous-bloc d'un champ (pour la descente recursive). Toutes peuvent
     * etre NULL si un backend ne les expose pas (backend catalogue :
     * exposees ; translate ne tourne de toute facon que dans owl.exe). */
    const char* (*type_en)(int typeIndex);                    /* nameEN du type, NULL si absent */
    const char* (*type_field_en)(int typeIndex, int fieldIndex);   /* nameEN du champ */
    const char* (*type_field_type)(int typeIndex, int fieldIndex); /* nom FR du type du champ (sous-bloc) */
    /* nameEN d'une constante d'index i (enum member DSL : dégradéRadial ->
     * gradientRadial). NULL si la constante n'a pas d'alias EN. */
    const char* (*const_en)(int i);

    /* --- methodes d'extension (.Majuscule, .VersEntier, .Ajoute...) ---- */
    int         (*method_count)(void);
    const char* (*method_name)(int i);
    /*  — partenaire bilingue d'une methode fluente pour `owl translate`.
     * Deux methodes enregistrees avec le MEME receveur et le MEME pointeur
     * d'implementation sont les deux graphies (FR/EN) d'une meme operation
     * (`Rétropropage`/`Backpropagate`). Retourne la graphie SOEUR de `name`
     * (accent/casse-insensible) quand la paire est NON AMBIGUE : exactement
     * deux entrees partagent (type, impl). Trois entrees ou plus (alias ASCII,
     * formes prefixees) -> NULL, la resolution retombe sur le catalogue natives.
     * NULL aussi hors registre (backend catalogue : translate n'y tourne pas). */
    const char* (*method_partner)(const char* name, int len);

    /* --- proprietes de blocs DSL (Fenetre, Libelle, Table, Etat...) ----
     * Trois formes par entree : FR canon (accentue), ASCII (FR sans accent),
     * EN canon. NULL si l'entree n'a pas la forme demandee. */
    int         (*dsl_prop_count)(void);
    const char* (*dsl_prop_fr)(int i);
    const char* (*dsl_prop_ascii)(int i);
    const char* (*dsl_prop_en)(int i);
    /*  — pont kind de widget -> type de style racine, pour resoudre
     * une cascade `Style..X..Y` ecrite en ligne dans un bloc PROPRIETES.
     * `dsl_prop_kind` = sorte proprietaire de l'entree ("Onglet", "Saisie"...,
     * forme ASCII canon) ; `dsl_prop_object_type` = nom du type compose cible
     * quand la prop est de kind objet ("styleOnglet"), NULL sinon. Le couple
     * (kind, prop `Style`) donne la racine de la cascade sans dupliquer aucune
     * table cote formatter. Peuvent etre NULL si un backend ne les expose pas. */
    const char* (*dsl_prop_kind)(int i);
    const char* (*dsl_prop_object_type)(int i);
} OwlFmtCanon;

/* Provider actif (config niveau process, pas etat mutable par appel).
 * owl.exe le pose sur le backend registry apres vm_init ; owl-lsp.exe sur
 * le backend catalogue au demarrage. NULL avant init -> le formatter ne
 * canonise pas les symboles builtin (mais reste fonctionnel : casse user,
 * indentation, whitespace inchanges). */
extern const OwlFmtCanon* owl_fmt_canon;

/* Pose le provider actif. */
void owl_format_set_canon(const OwlFmtCanon* canon);

/* Backend registry (lit le registre vivant). Defini dans owl_core, donc
 * uniquement disponible dans les binaires qui linkent le runtime. */
const OwlFmtCanon* owl_fmt_canon_registry(void);

/* Backend catalogue (lit les tables statiques generees owl_catalog.h).
 * Defini dans formatter_canon_catalog.c (linke dans owl-lsp.exe). Aucune
 * dependance runtime/VM. */
const OwlFmtCanon* owl_fmt_canon_catalog(void);

#endif
