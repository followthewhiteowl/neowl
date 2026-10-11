// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* ================================================================
 * FORMATTER CONTEXT — construction du contexte de casse cross-fichier
 * ================================================================
 * Decouvre le projet alentour, prescanne les fichiers eligibles
 * et construit un OwlFormatContext read-only pour le formatter (casse des
 * types/globales/membres declares dans d'AUTRES fichiers du projet).
 *
 * Extrait de main.c (/0175/0258) pour etre partage entre owl.exe
 * (handle_fmt_command) et owl-lsp.exe (handler textDocument/formatting) :
 * une seule logique de contexte -> les deux formatent a l'identique. Ne tire
 * que project.c (discover, compiler-free) + prescan.c + formatter.c.
 * ================================================================ */

#include "formatter.h"
#include "project.h"
#include "prescan.h"

#include <string.h>

const char* owl_format_path_basename(const char* path) {
    const char* base = path;
    const char* fwd = strrchr(path, '/');
    const char* bwd = strrchr(path, '\\');
    if (fwd && fwd >= base) base = fwd + 1;
    if (bwd && bwd >= base) base = bwd + 1;
    return base;
}

bool owl_format_basename_is_test(const char* basename) {
    return strncmp(basename, "test_", 5) == 0;
}

OwlFormatContext* owl_format_build_context(const char* root,
                                           const char* targetBasename,
                                           bool targetIsTest,
                                           bool recursive) {
    Project project;
    project_init(&project);
    /* skipTestDirs : on saute les sous-dossiers test/tests sauf si la cible
     * est elle-meme un test (alors tout le projet est pertinent).
     * recursive : , false en mode script/point d'entree. */
    if (!project_discover_scoped(&project, root, !targetIsTest, recursive)) {
        project_free(&project);
        return NULL;
    }

    SymbolTable* symbols = symtable_create();
    for (int i = 0; i < project.fileCount; i++) {
        /* Cible prod : on ignore les fichiers test_* comme sources. */
        if (!targetIsTest && project.files[i].isTest) continue;
        /* Ne pas reinjecter la cible elle-meme dans le contexte. */
        if (strcmp(owl_format_path_basename(project.files[i].path), targetBasename) == 0)
            continue;
        prescan_file(symbols, project.files[i].source, i);
    }

    OwlFormatContext* ctx = owl_format_context_new();
    for (int i = 0; i < symbols->count; i++) {
        SymbolKind k = symbols->symbols[i].kind;
        if (k == SYM_CLASS || k == SYM_STRUCT || k == SYM_ENUM) {
            /*  : casse divergente du meme type dans >=2 fichiers ->
             * pas de canon possible, on n'alimente pas le contexte (verbatim). */
            if (symtable_type_is_ambiguous(symbols, symbols->symbols[i].name))
                continue;
            owl_format_context_add_type(ctx, symbols->symbols[i].name);
        }
        /*  : variables/constantes globales et procedures/fonctions
         * top-level. Meme regle d'ambiguite que les types : un meme nom
         * declare avec des casses divergentes a travers le projet -> verbatim
         * (pas de canonisation arbitraire). Variante dediee parce que
         * symtable_type_is_ambiguous filtre explicitement sur CLASS/STRUCT/
         * ENUM/INTERFACE. */
        else if (k == SYM_VARIABLE || k == SYM_CONSTANT ||
                 k == SYM_PROCEDURE || k == SYM_FUNCTION ||
                 k == SYM_WINDOW /*  */) {
            if (symtable_global_is_ambiguous(symbols, symbols->symbols[i].name))
                continue;
            owl_format_context_add_global(ctx, symbols->symbols[i].name);
            /*  : les callables (procedures/fonctions) alimentent aussi
             * procGlobals -> restauration de graphie d'un appel NU cross-fichier. */
            if (k == SYM_PROCEDURE || k == SYM_FUNCTION)
                owl_format_context_add_proc(ctx, symbols->symbols[i].name);
        }
    }
    for (int i = 0; i < symbols->memberCount; i++) {
        ProjectMember* m = &symbols->members[i];
        /*  : meme regle au niveau membre. Un membre declare en casses
         * divergentes a travers le projet ne peut pas etre canonise -> verbatim.
         * (La priorite locale stricte protege deja les membres declares DANS le
         * fichier formate ; ici on couvre le cas non-local ambigu.) */
        if (symtable_member_is_ambiguous(symbols, m->ownerType, m->name))
            continue;
        /* Un membre sans type declare provenant d'un enum est reference bare.
         * On ne distingue pas ici const-de-classe vs membre-d'enum a partir
         * de la seule absence de type : on resout via le kind du proprietaire. */
        SymbolKind ownerKind = SYM_CLASS;
        bool found = false;
        for (int j = 0; j < symbols->count; j++) {
            if (strcasecmp(symbols->symbols[j].name, m->ownerType) == 0 &&
                (symbols->symbols[j].kind == SYM_CLASS ||
                 symbols->symbols[j].kind == SYM_STRUCT ||
                 symbols->symbols[j].kind == SYM_ENUM)) {
                ownerKind = symbols->symbols[j].kind; found = true; break;
            }
        }
        (void)found;
        if (ownerKind == SYM_ENUM) {
            owl_format_context_add_enum_member(ctx, m->ownerType, m->name);
        } else {
            owl_format_context_add_member(ctx, m->ownerType, m->name,
                                          m->declType[0] ? m->declType : NULL);
        }
    }
    /*  : les methodes de classe sont des membres a part entiere pour
     * le formatter — un appel `recepteur.Methode()` doit retrouver la graphie
     * de declaration de la methode, pas etre re-aligne (voire desaccentue)
     * vers une forme native homonyme. Le prescan les capture dans
     * classMethods[] (signatures), distinct de members[] (champs/constantes) ;
     * on les seede ici sans type declare (une methode n'a pas de type de
     * champ — sa graphie suffit a la canonisation). */
    for (int i = 0; i < symbols->classMethodCount; i++) {
        const char* cls = symbols->classMethods[i].className;
        const char* mname = symbols->classMethods[i].sig.name;
        if (!cls[0] || !mname[0]) continue;
        owl_format_context_add_member(ctx, cls, mname, NULL);
    }

    symtable_destroy(symbols);
    project_free(&project);
    return ctx;
}
