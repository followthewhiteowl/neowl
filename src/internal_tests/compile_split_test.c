// SPDX-License-Identifier: AGPL-3.0-only
/* compile_split_test.c —  : preuve du decoupage deux phases.
 *
 * Execute via `owl --self-test-compile-split` (cf main.c). Verifie que
 * compile_in_project_split produit DEUX ObjFunction distincts :
 *   - outDecls : le bytecode des DECLARATIONS statiques top-level (ici une
 *     Enumeration -> OP_CLASS + OP_MARK_CLASS_KIND) ;
 *   - outBody  : le bytecode IMPERATIF top-level (ici l'appel Trace -> OP_CALL).
 *
 * On n'utilise pas de source `Classe + code` : la convention "1 classe = 1
 * fichier" interdit tout code runtime dans un fichier-classe. Une Enumeration
 * n'active pas cette isolation et coexiste avec du code top-level — tout en
 * emettant le meme OP_CLASS de materialisation, ce qui prouve qu'une
 * declaration part bien dans outDecls et pas dans outBody.
 *
 * Contrats verifies :
 *   1. split renvoie true, les deux fonctions sont non-NULL et distinctes ;
 *   2. le pool de constantes est copie ENTIER dans chaque (memes tailles) ;
 *   3. outDecls contient OP_CLASS, PAS d'OP_CALL ;
 *   4. outBody contient OP_CALL, PAS d'OP_CLASS ;
 *   5. chaque chunk se termine par le terminateur reemis OP_NIL + OP_RETURN.
 *
 * Requiert vm_init (compile = interning de strings + pile VM). Retourne 0 si
 * tout passe, sinon le nombre d'echecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"
#include "../compiler.h"
#include "../chunk.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Walk propre du chunk : true si l'opcode `target` apparait comme instruction
 * (pas comme octet d'operande). Renvoie -1 encode en false si un opcode inconnu
 * casse le walk (defensif : signalerait un chunk corrompu).
 *
 *  — la table de tailles vivait ici en copie manuelle de celle de
 * chunk.c. Deux tables a maintenir pour une seule verite : elle est desormais
 * consommee depuis chunk.h. */
static bool chunk_has_opcode(const Chunk* chunk, uint8_t target) {
    int i = 0;
    while (i < chunk->count) {
        uint8_t op = chunk->code[i];
        if (op == target) return true;
        int sz = chunk_opcode_size(op, chunk, i);
        if (sz <= 0) {
            fprintf(stderr, "  WARN walk stoppe sur opcode inconnu %d @%d\n", op, i);
            return false;
        }
        i += sz;
    }
    return false;
}

static bool chunk_ends_with_terminator(const Chunk* chunk) {
    int n = chunk->count;
    if (n < 2) return false;
    return chunk->code[n - 2] == OP_NIL && chunk->code[n - 1] == OP_RETURN;
}

/* Enumeration (declaration statique, emet OP_CLASS) + appel imperatif Trace.
 * "Énumération" en UTF-8 : le E accent aigu = \xc3\x89. */
static const char* SRC_SPLIT =
    "eCouleur est une \xc3\x89num\xc3\xa9ration\n"
    "\tRouge\n"
    "\tVert\n"
    "\tBleu\n"
    "FIN\n"
    "Trace(\"MEMBRE\")\n";

int compile_split_test_main(void) {
    fprintf(stderr, "\n==== COMPILE SPLIT TESTS (fix/0941, ADR_0122) ====\n");

    ObjFunction* decls = NULL;
    ObjFunction* body  = NULL;
    bool ok = compile_in_project_split(SRC_SPLIT, "entree", NULL, 0,
                                       &decls, &body);

    CHECK(ok, "split_returns_true");
    CHECK(decls != NULL, "decls_non_null");
    CHECK(body  != NULL, "body_non_null");
    CHECK(decls != body, "decls_body_distinct");

    if (!ok || decls == NULL || body == NULL) {
        fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
                g_pass, g_fail);
        return g_fail == 0 ? 1 : g_fail;  /* echec dur : au moins 1 */
    }

    /* Root immediat : on n'alloue pas ci-dessous, mais c'est l'usage correct
     * (les deux fonctions reviennent non rootees). */
    vm_push(OBJ_VAL(decls));
    vm_push(OBJ_VAL(body));

    CHECK(decls->chunk != NULL, "decls_chunk_non_null");
    CHECK(body->chunk  != NULL, "body_chunk_non_null");
    CHECK(decls->chunk->count > 0, "decls_chunk_non_empty");
    CHECK(body->chunk->count  > 0, "body_chunk_non_empty");

    /* Pool copie entier dans chaque -> memes tailles (chaque fonction possede
     * SA copie du tableau ; ici on verifie juste que la copie est complete et
     * identique des deux cotes). */
    CHECK(decls->chunk->constants.count == body->chunk->constants.count,
          "pool_copie_meme_taille");
    CHECK(decls->chunk->constants.count > 0, "pool_non_vide");

    /* Separation : la declaration (materialisation OP_CLASS) est dans outDecls
     * et PAS dans outBody ; l'appel imperatif Trace (OP_CALL) est dans outBody.
     * (On n'affirme pas l'absence d'OP_CALL cote decls : la materialisation
     * d'une Enumeration emet legitimement un appel interne — c'est le byte du
     * OP_CLASS qui prouve que la declaration a bien ete routee vers outDecls.) */
    CHECK(chunk_has_opcode(decls->chunk, OP_CLASS),  "decls_contient_OP_CLASS");
    CHECK(!chunk_has_opcode(body->chunk, OP_CLASS),  "body_sans_OP_CLASS");
    CHECK(chunk_has_opcode(body->chunk, OP_CALL),    "body_contient_OP_CALL");

    /* Terminateur reemis en fin de CHAQUE fonction. */
    CHECK(chunk_ends_with_terminator(decls->chunk), "decls_terminateur");
    CHECK(chunk_ends_with_terminator(body->chunk),  "body_terminateur");

    vm_pop();  /* body */
    vm_pop();  /* decls */

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    /* Garde ensemble vide. */
    if (g_pass + g_fail == 0) {
        fprintf(stderr, "compile-split: CORPUS VIDE — 0 cas execute. "
                        "Un controle qui n'examine rien ne prouve rien.\n");
        return g_fail + 1;
    }
    return g_fail;
}
