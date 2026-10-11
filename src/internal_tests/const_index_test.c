// SPDX-License-Identifier: AGPL-3.0-only
/* const_index_test.c —  : l'index de constante tient sur 16 bits.
 *
 * Execute via `owl --self-test-const-index` (cf main.c).
 *
 * Ce que le harnais .owl ne peut PAS verifier, et qui vit donc ici :
 *
 *   1. La table canonique des formes d'opcodes couvre TOUT l'enum OpCode.
 *      Un opcode ajoute sans entree dans chunk_opcode_shape() rendrait -1 a
 *      la premiere marche de chunk et casserait silencieusement les
 *      peepholes, le desassembleur et le decoupage deux phases.
 *
 *   2. La largeur emise et la table de tailles s'accordent : on marche le
 *      bytecode reellement produit et on doit retomber exactement sur
 *      chunk->count. C'est le seul controle qui attrape « un site d'emission
 *      a ete oublie » — la desynchronisation ne provoque aucune erreur de
 *      compilation, juste un crash ailleurs, plus tard, au hasard.
 *
 *   3. Un fichier depassant l'ancien mur (256 constantes) compile ET
 *      s'execute juste — la valeur lue apres coup est la bonne, pas celle
 *      d'un voisin de pool.
 *
 * Le plafond haut (65535) n'est pas exerce ici : la deduplication du pool est
 * lineaire, donc quadratique a la construction, et une source de 65536
 * constantes distinctes ferait durer ce self-test plusieurs minutes pour
 * verifier une comparaison d'entiers.
 *
 * Retourne 0 si tout passe, sinon le nombre d'echecs. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../vm.h"
#include "../compiler.h"
#include "../chunk.h"
#include "../object.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL [%s] (line %d)\n", label, __LINE__); } \
} while (0)

/* Nombre de globales generees. Chaque declaration consomme deux entrees de
 * pool (le nom de la variable, le nom du type) : 200 declarations depassent
 * donc largement l'ancien mur de 256 sans allonger le test. */
#define GEN_GLOBALS 200

/* Population attendue de la famille « opcodes portant un index de constante ».
 * Fige volontairement : un ajout legitime doit passer ici, apres
 * verification des quatre chemins. Le mode d'emploi est dans le message
 * d'echec, pas dans ce commentaire — c'est le lecteur du rouge qui en a
 * besoin, pas le lecteur du fichier. */
#define EXPECTED_CONST_IDX_OPCODES 25

/* Construit une source qui declare GEN_GLOBALS globales entieres puis en
 * relit trois : la premiere, une du milieu, la derniere. Les relectures sont
 * ce qui compte — une compilation qui passe mais qui lit le mauvais slot de
 * pool est exactement le mode de defaillance que ce chantier evite. */
static char* build_source(void) {
    size_t cap = (size_t)GEN_GLOBALS * 48 + 512;
    char* src = malloc(cap);
    if (src == NULL) return NULL;
    size_t n = 0;
    for (int i = 1; i <= GEN_GLOBALS; i++) {
        n += (size_t)snprintf(src + n, cap - n,
                              "g%04d est un entier = %d\n", i, i);
    }
    n += (size_t)snprintf(src + n, cap - n,
                          "total est un entier = g0001 + g%04d + g%04d\n",
                          GEN_GLOBALS / 2, GEN_GLOBALS);
    (void)n;
    return src;
}

/* Verifie qu'une marche complete du chunk retombe sur count, et qu'aucun
 * opcode rencontre n'est inconnu de la table. */
static bool walk_ok(const Chunk* chunk, const char* label) {
    int i = 0;
    while (i < chunk->count) {
        int sz = chunk_opcode_size(chunk->code[i], chunk, i);
        if (sz <= 0) {
            fprintf(stderr, "  FAIL [%s] opcode %d sans forme canonique @%d\n",
                    label, chunk->code[i], i);
            return false;
        }
        i += sz;
    }
    if (i != chunk->count) {
        fprintf(stderr, "  FAIL [%s] marche desynchronisee : %d != %d\n",
                label, i, chunk->count);
        return false;
    }
    return true;
}

/* Assignation scalaire nue sur une chaineMultilingue GLOBALE.
 *
 * Le compilateur pousse ici la variable de gauche avec un opcode resolu a
 * l'execution : OP_GET_LOCAL (slot, un octet) pour une locale, OP_GET_GLOBAL
 * (index de constante, largeur canonique) pour une globale. C'est le seul site
 * du chantier qui avait survecu a la migration, et il ecrivait un octet la ou
 * la VM en lit deux — bytecode desynchronise des la troisieme globale, bien
 * en-deca de l'ancien mur de 256.
 *
 * Ce qui l'attrape est la VALEUR relue apres execution, pas la marche du
 * chunk : la marche se resynchronise spontanement sur ce motif (l'octet de
 * poids fort d'un index bas vaut 0, soit OP_CONSTANT, qui reabsorbe le
 * decalage). Les deux controles sont donc joues, mais c'est le second qui
 * prouve quelque chose. */
static char* build_multilingue_source(const char* varName, int filler) {
    size_t cap = (size_t)filler * 48 + 1024;
    char* src = malloc(cap);
    if (src == NULL) return NULL;
    size_t n = 0;
    for (int i = 1; i <= filler; i++) {
        n += (size_t)snprintf(src + n, cap - n,
                              "f%04d est un entier = %d\n", i, i);
    }
    n += (size_t)snprintf(src + n, cap - n,
        "%s est une chaineMultilingue = "
        "{langueFrancais: \"Bonjour\", langueAnglais: \"Hello\"}\n"
        "%s = \"Salut\"\n"
        "lu%s est une chaine = %s\n",
        varName, varName, varName, varName);
    (void)n;
    return src;
}

static void check_multilingue_globale(const char* varName, int filler,
                                      const char* label) {
    char* src = build_multilingue_source(varName, filler);
    CHECK(src != NULL, label);
    if (src == NULL) return;

    ObjFunction* decls = NULL;
    ObjFunction* body  = NULL;
    bool compiled = compile_in_project_split(src, "entree", NULL, 0,
                                             &decls, &body);
    CHECK(compiled, label);
    if (compiled && decls != NULL && body != NULL) {
        vm_push(OBJ_VAL(decls));
        vm_push(OBJ_VAL(body));
        CHECK(walk_ok(decls->chunk, label), label);
        CHECK(walk_ok(body->chunk,  label), label);
        vm_pop();
        vm_pop();
    }

    InterpretResult r = vm_interpret(src);
    CHECK(r == INTERPRET_OK, label);

    char keyName[64];
    snprintf(keyName, sizeof(keyName), "lu%s", varName);
    Value lu;
    ObjString* key = obj_copy_string(keyName, (int)strlen(keyName));
    vm_push(OBJ_VAL(key));
    bool found = table_get(&vm.globals, key, &lu);
    vm_pop();
    CHECK(found, label);
    if (found) {
        CHECK(IS_STRING(lu) && strcmp(AS_CSTRING(lu), "Salut") == 0, label);
    }

    free(src);
}

int const_index_test_main(void) {
    fprintf(stderr, "\n==== CONST INDEX 16 BITS (fix/1504) ====\n");

    /* --- 1. La table couvre tout l'enum -------------------------------- */
    int missing = 0;
    for (int op = 0; op < OP_COUNT; op++) {
        OpcodeShape shape;
        if (!chunk_opcode_shape((uint8_t)op, &shape)) {
            fprintf(stderr, "  FAIL opcode %d absent de chunk_opcode_shape\n", op);
            missing++;
        }
    }
    CHECK(missing == 0, "table_couvre_tout_l_enum");

    /* La largeur d'un index de constante est la meme pour tous : c'est
     * l'engagement structurel du chantier, pas un detail d'implementation.
     * Un opcode qui reintroduirait une forme courte se verrait ici.
     * Compte : 24 au chantier , 25 depuis l'ajout d'OP_ARRAY_FILL_STRUCT. */
    int withIdx = 0;
    for (int op = 0; op < OP_COUNT; op++) {
        OpcodeShape shape;
        if (!chunk_opcode_shape((uint8_t)op, &shape)) continue;
        if (shape.constIdx > 0) withIdx++;
    }
    CHECK(withIdx == EXPECTED_CONST_IDX_OPCODES,
          "compte_fige_des_opcodes_a_index_de_constante");
    if (withIdx != EXPECTED_CONST_IDX_OPCODES) {
        fprintf(stderr,
"\n"
"  ----------------------------------------------------------------------\n"
"  Ceci n'est pas une regression du compilateur : c'est une consigne.\n"
"\n"
"  %d opcodes portent un index de constante, ce controle en attend %d.\n"
"  Ce nombre est fige VOLONTAIREMENT (ADR_0150). Ajouter un opcode a index\n"
"  est legitime et attendu : il faut alors verifier, puis mettre a jour le\n"
"  compte. On ne remplace pas ce compteur par un invariant qui ne casserait\n"
"  plus -- un opcode ajoute hors du chemin commun ne rougit nulle part, il\n"
"  produit un bytecode desynchronise qui derive silencieusement, tres loin\n"
"  de sa cause.\n"
"\n"
"  Si vous venez d'ajouter un opcode, verifier ses quatre chemins :\n"
"    1. emission      : emit_const_op(...) dans src/compiler.c,\n"
"                       jamais un emit_byte() d'index ecrit a la main ;\n"
"    2. table de formes : une entree SHAPE(1, 0) dans chunk_opcode_shape()\n"
"                       (src/chunk.c) -- un index de constante, zero octet brut ;\n"
"    3. lecture VM    : READ_CONSTANT() / READ_STRING() dans src/vm.c, qui\n"
"                       passent par READ_CONST_IDX() -- jamais un READ_BYTE() nu ;\n"
"    4. desassemblage : constant_instruction() dans src/debug.c.\n"
"\n"
"  Les quatre sont conformes -> porter le compte a %d :\n"
"    src/internal_tests/const_index_test.c, #define EXPECTED_CONST_IDX_OPCODES\n"
"\n"
"  Un seul manque -> c'est une vraie regression : corriger l'opcode, pas le\n"
"  compte.\n"
"  ----------------------------------------------------------------------\n\n",
                withIdx, EXPECTED_CONST_IDX_OPCODES, withIdx);
    }

    /* --- 2 & 3. Compilation, marche, execution ------------------------- */
    char* src = build_source();
    CHECK(src != NULL, "source_generee");
    if (src == NULL) {
        fprintf(stderr, "\n==== RESULTS: %d passed, %d failed ====\n\n",
                g_pass, g_fail);
        return g_fail == 0 ? 1 : g_fail;
    }

    ObjFunction* decls = NULL;
    ObjFunction* body  = NULL;
    bool compiled = compile_in_project_split(src, "entree", NULL, 0,
                                             &decls, &body);
    CHECK(compiled, "compile_au_dela_de_256_constantes");

    if (compiled && decls != NULL && body != NULL) {
        vm_push(OBJ_VAL(decls));
        vm_push(OBJ_VAL(body));
        /* Plus de 256 entrees de pool : c'est bien l'ancien mur qui est
         * franchi, pas un test qui s'arrete avant. */
        CHECK(decls->chunk->constants.count > 256, "pool_depasse_256_entrees");
        CHECK(walk_ok(decls->chunk, "decls"), "marche_chunk_declarations");
        CHECK(walk_ok(body->chunk,  "body"),  "marche_chunk_corps");
        vm_pop();
        vm_pop();
    }

    /* Execution reelle : total doit valoir g0001 + g0100 + g0200. Une lecture
     * d'index tronquee donnerait la valeur d'une autre globale. */
    InterpretResult r = vm_interpret(src);
    CHECK(r == INTERPRET_OK, "execution_sans_erreur");

    Value total;
    ObjString* key = obj_copy_string("total", 5);
    vm_push(OBJ_VAL(key));
    bool found = table_get(&vm.globals, key, &total);
    vm_pop();
    CHECK(found, "globale_total_presente");
    if (found) {
        int64_t expected = 1 + GEN_GLOBALS / 2 + GEN_GLOBALS;
        CHECK(IS_INTEGER(total) && AS_INTEGER(total) == expected,
              "valeur_lue_apres_le_mur_est_la_bonne");
    }

    free(src);

    /* --- 4. Le site a opcode resolu a l'execution ---------------------- */
    check_multilingue_globale("mBas", 0,
                              "multilingue_globale_index_bas");
    check_multilingue_globale("mHaut", GEN_GLOBALS,
                              "multilingue_globale_au_dela_du_mur");

    fprintf(stderr, "\n==== RESULTS: %d cas, %d passed, %d failed ====\n\n",
            g_pass + g_fail, g_pass, g_fail);
    return g_fail;
}
