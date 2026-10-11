// SPDX-License-Identifier: AGPL-3.0-only
#include "lsp_catalog.h"
#include "../src/scanner.h"
#include <string.h>
#include <ctype.h>

/* ================================================================
 * NATIVE FUNCTION CATALOG
 * ================================================================
 * OwlNativeInfo type is declared in src/owl_catalog.h (just the types,
 * `extern` decls for the array).
 *
 * OWL_NATIVE_CATALOG[] itself is defined in lsp_catalog_gen.c — an
 * AUTO-GENERATED file produced by:
 *     owl --dump-lsp-catalog > lsp/lsp_catalog_gen.c
 * or equivalently:
 *     make lsp-catalog
 *
 * Regenerate after any stdlib_*.c DEFS change.
 * ================================================================ */

/* ================================================================
 * KEYWORD CATALOG
 * ================================================================ */

const KeywordInfo KEYWORD_CATALOG[] = {
    {"SI",        "SI condition ALORS ... [SINON SI ...] [SINON ...] FIN",
     "Structure conditionnelle. Execute le bloc si la condition est vraie."},
    {"POUR",      "POUR i = debut A fin [PAS increment] ... FIN",
     "Boucle avec compteur. Le compteur est incremente a chaque iteration."},
    {"POUR TOUT", "POUR TOUT element [, indice] DE collection ... FIN",
     "Iteration sur chaque element d'un tableau, file, pile ou liste."},
    {"TANTQUE",   "TANTQUE condition ... FIN",
     "Boucle tant que la condition est vraie. La condition est evaluee avant chaque iteration."},
    {"BOUCLE",    "BOUCLE [(n)] ... FIN | BOUCLE ... A FAIRE TANTQUE condition",
     "Boucle infinie (avec SORTIR), comptee, ou do-while."},
    {"SELON",     "SELON expression CAS pattern [, pattern]* [:] body [AUTRE CAS [:] body] FIN",
     "Aiguillage. Pattern : valeur (=), plage (1 A 10 / A 8 / 11 A), comparateur prefixe (> 5, [= \"abc\"), sandwich (10 < * < 20). SORTIR SELON pour sortir explicitement. ':' optionnel apres pattern."},
    {"AVEC",      "AVEC objet ... FIN",
     "Raccourci pour acceder aux membres d'un objet sans repeter son nom (.Membre)."},
    {"QUAND EXCEPTION", "QUAND EXCEPTION DANS ... FAIRE ... FIN",
     "Bloc try/catch. Le code risque est dans DANS, le handler dans FAIRE."},
    {"PROCEDURE", "PROCEDURE Nom(params) [:type_retour] ... FIN",
     "Declaration d'une procedure (sous-programme). FIN optionnel en fin de fichier."},
    {"FONCTION",  "FONCTION Nom(params) : type_retour ... RENVOYER valeur ... FIN",
     "Declaration d'une fonction (retourne une valeur)."},
    {"CLASSE",    "NomClasse est une Classe [Herite de Parent] ... FIN",
     "Declaration d'une classe avec membres, methodes, constructeur, destructeur."},
    {"STRUCTURE", "NomStruct est une Structure ... FIN",
     "Declaration d'un type structure (record) avec des champs nommes."},
    {"ENUMERATION","NomEnum est une Enumeration ... FIN",
     "Declaration d'un type enumere. Chaque valeur a un .Nom et un .Valeur."},
    {"COMBINAISON","NomCombi est une Combinaison ... FIN",
     "Declaration de flags bitmask. Valeurs auto en puissances de 2. Test avec []."},
    {"RENVOYER",  "RENVOYER valeur",
     "Retourne une valeur depuis une fonction et quitte la procedure."},
    {"SORTIR",    "SORTIR [(n)]",
     "Sort de la boucle courante. Avec (n), sort de n boucles imbriquees."},
    {"SORTIR SELON", "SORTIR SELON",
     "Sort du SELON englobant. Distinct de SORTIR qui reste reserve aux boucles."},
    {"CONTINUER", "CONTINUER [(n)]",
     "Passe a l'iteration suivante de la boucle courante."},
    {"EST",       "variable EST [UN/UNE] type [= valeur]",
     "Declaration de variable avec typage explicite."},
};

const int KEYWORD_CATALOG_COUNT = sizeof(KEYWORD_CATALOG) / sizeof(KEYWORD_CATALOG[0]);

/* ================================================================
 * LOOKUP
 * ================================================================ */

static int strcasecmp_portable(const char* a, const char* b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

const NativeInfo* catalog_find_native(const char* name) {
    /* Strip accents from lookup and compare against pre-stripped nameAscii + nameEN */
    char stripped[256];
    scanner_strip_accents(name, (int)strlen(name), stripped, sizeof(stripped));

    for (int i = 0; i < NATIVE_CATALOG_COUNT; i++) {
        if (strcasecmp_portable(stripped, NATIVE_CATALOG[i].nameAscii) == 0 ||
            strcasecmp_portable(stripped, NATIVE_CATALOG[i].nameEN) == 0)
            return &NATIVE_CATALOG[i];
    }
    return NULL;
}

const KeywordInfo* catalog_find_keyword(const char* name) {
    for (int i = 0; i < KEYWORD_CATALOG_COUNT; i++) {
        if (strcasecmp_portable(name, KEYWORD_CATALOG[i].keyword) == 0)
            return &KEYWORD_CATALOG[i];
    }
    return NULL;
}

const OwlTypeInfo* owl_type_info_find(const char* name) {
    char stripped[128];
    scanner_strip_accents(name, (int)strlen(name), stripped, sizeof(stripped));
    for (int i = 0; i < OWL_TYPE_CATALOG_COUNT; i++) {
        const OwlTypeInfo* t = &OWL_TYPE_CATALOG[i];
        if (strcasecmp_portable(stripped, t->nameAscii) == 0 ||
            strcasecmp_portable(stripped, t->name) == 0 ||
            (t->nameEN && strcasecmp_portable(stripped, t->nameEN) == 0))
            return t;
    }
    return NULL;
}

const OwlEnumInfo* owl_enum_info_find(const char* name) {
    if (name == NULL) return NULL;
    char stripped[128];
    scanner_strip_accents(name, (int)strlen(name), stripped, sizeof(stripped));
    for (int i = 0; i < OWL_ENUM_CATALOG_COUNT; i++) {
        const OwlEnumInfo* e = &OWL_ENUM_CATALOG[i];
        if (e->nameAscii && strcasecmp_portable(stripped, e->nameAscii) == 0) return e;
        if (e->name      && strcasecmp_portable(stripped, e->name)      == 0) return e;
        if (e->nameEN    && strcasecmp_portable(stripped, e->nameEN)    == 0) return e;
    }
    return NULL;
}

int owl_enum_info_contains(const OwlEnumInfo* e, const char* constName) {
    if (e == NULL || e->values == NULL || constName == NULL) return 0;
    char stripped[128];
    scanner_strip_accents(constName, (int)strlen(constName), stripped, sizeof(stripped));
    for (const char* const* v = e->values; *v; v++) {
        char vStripped[128];
        scanner_strip_accents(*v, (int)strlen(*v), vStripped, sizeof(vStripped));
        if (strcasecmp_portable(stripped, vStripped) == 0) return 1;
    }
    return 0;
}



/* V1.6 — OWL_COMPOSED_TYPE_CATALOG est maintenant auto-genere via
 * `owl --dump-lsp-catalog` dans lsp_catalog_gen.c. La table iterativement
 * dumpe les templates ObjInstance via vm.globals. Voir src/main.c.
 *
 * Ajouter un nouveau type composé : modifier le WHITELIST dans
 * src/main.c::dump_lsp_catalog. */
