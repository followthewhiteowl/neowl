// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "chunk.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>

void chunk_init(Chunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    value_array_init(&chunk->constants);
}

void chunk_write(Chunk* chunk, uint8_t byte, int line) {
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code,
                                 oldCapacity, chunk->capacity);
        chunk->lines = GROW_ARRAY(int, chunk->lines,
                                  oldCapacity, chunk->capacity);
    }

    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

void chunk_insert_before(Chunk* chunk, int pos, const uint8_t* bytes,
                         int n, int line) {
    while (chunk->capacity < chunk->count + n) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(oldCapacity);
        chunk->code  = GROW_ARRAY(uint8_t, chunk->code,
                                  oldCapacity, chunk->capacity);
        chunk->lines = GROW_ARRAY(int, chunk->lines,
                                  oldCapacity, chunk->capacity);
    }
    int tail = chunk->count - pos;
    if (tail > 0) {
        memmove(chunk->code  + pos + n, chunk->code  + pos, tail);
        memmove(chunk->lines + pos + n, chunk->lines + pos, tail * sizeof(int));
    }
    memcpy(chunk->code + pos, bytes, n);
    for (int i = 0; i < n; i++) chunk->lines[pos + i] = line;
    chunk->count += n;
}

int chunk_add_constant(Chunk* chunk, Value value) {
    value_array_write(&chunk->constants, value);
    return chunk->constants.count - 1;
}

/* ================================================================
 * FORME DES OPCODES — table canonique unique (cf. chunk.h)
 *
 * SHAPE(nbIndexConstante, nbOctetsBruts)
 *
 * Tout index de constante fait OWL_CONST_IDX_BYTES octets, sans exception :
 * une taille d'instruction fixe par opcode est ce qui rend cette table, les
 * peepholes et le desassembleur tenables. Un opcode futur qui consomme un
 * index suit la meme convention — la variante « courte + longue » est
 * volontairement ecartee (cf. ).
 * ================================================================ */
#define SHAPE(nc, nr) \
    do { out->constIdx = (nc); out->raw = (nr); out->varRaw = 0; \
         return true; } while (0)

bool chunk_opcode_shape(uint8_t op, OpcodeShape* out) {
    switch (op) {
        /* --- Aucun opérande --- */
        case OP_NIL: case OP_TRUE: case OP_FALSE:
        case OP_ADD: case OP_SUBTRACT: case OP_MULTIPLY: case OP_DIVIDE:
        case OP_MODULO: case OP_POWER: case OP_NEGATE:
        case OP_INCREMENT: case OP_DECREMENT:
        case OP_EQUAL: case OP_NOT_EQUAL:
        case OP_GREATER: case OP_GREATER_EQUAL: case OP_LESS: case OP_LESS_EQUAL:
        case OP_NOT: case OP_AND_FULL: case OP_OR_FULL:
        case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR: case OP_BIT_NOT:
        case OP_BIT_SHL: case OP_BIT_SHR:
        case OP_CONCAT:
        case OP_RETURN:
        case OP_POP: case OP_DUP: case OP_SWAP:
        case OP_PRINT:
        case OP_DEEP_COPY: case OP_TO_UNDETECTABLE:
        case OP_ARRAY:
        case OP_ARRAY_MARK_GROUP:
        case OP_ARRAY_GET: case OP_ARRAY_SET: case OP_TABLE_CELL_DESC:
        case OP_ARRAY_GET_2D: case OP_ARRAY_SET_2D:
        case OP_ARRAY_LENGTH:
        case OP_ITER_KEY:
        case OP_END_TRY: case OP_THROW: case OP_RERAISE: case OP_EXC_RESTORE:
        case OP_INHERIT: case OP_GET_THIS:
        case OP_MOIMEME: case OP_MONCHAMP: case OP_MAFENETRE:
        case OP_ITER_BEGIN: case OP_ITER_TRACK_KEY: case OP_ITER_END:
        case OP_STRING_CHAR_AT: case OP_BUFFER_BYTE_SET:
        case OP_STRING_SLICE: case OP_STRING_SLICE_TO_END:
        case OP_STRING_SLICE_LEN:
        case OP_INDIRECT_GLOBAL_GET: case OP_INDIRECT_GLOBAL_SET:
        case OP_INDIRECT_FIELD_GET: case OP_INDIRECT_FIELD_SET:
        case OP_VERIFY_BOOL_COND:
            SHAPE(0, 0);

        /* --- 1 octet brut (slot local, compteur, mode…) --- */
        case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_APPEND_IN_PLACE:
        case OP_CALL:
        case OP_POPN:
        case OP_ARRAY_PRESIZE: case OP_ARRAY_GROWTH:
        case OP_ARRAY_FILL_DEFAULT:
        case OP_MARK_CLASS_KIND:
        case OP_ARRAY_PACK: case OP_ARRAY_UNPACK: case OP_COLLECTION:
        case OP_PROC_SET_HOOK:
        case OP_STRING_MATCH:
            SHAPE(0, 1);

        /* --- 2 octets bruts --- */
        case OP_OUT_PARAM_TO_LOCAL:              /* slot + idx */
            SHAPE(0, 2);

        /* --- Offset de saut 16 bits (opérande brut, pas un index de pool) --- */
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_LOOP:
        case OP_JUMP_IF_NIL: case OP_JUMP_IF_NOT_NIL:
        case OP_TRY:
            SHAPE(0, 2);

        /* ============================================================
         * INDEX DE CONSTANTE — les 24 opcodes du chantier .
         * ============================================================ */

        /* Lot 1 : globales — index 16 bits. */
        case OP_DEFINE_GLOBAL:
        case OP_GET_GLOBAL: case OP_GET_GLOBAL_CALLEE: case OP_SET_GLOBAL:
        case OP_GET_ELEMENT_GLOBAL: case OP_GET_TYPE_GLOBAL:
        case OP_APPEND_IN_PLACE_GLOBAL:
            SHAPE(1, 0);
        case OP_OUT_PARAM_TO_GLOBAL:             /* nameConst + idx */
            SHAPE(1, 1);

        /* Lot 2 : structures et champs. */
        case OP_ARRAY_TYPE_STRUCT:
        case OP_ARRAY_FILL_STRUCT:
        case OP_STRUCT_FIELD_ORDER:
        case OP_STRUCT_GET: case OP_STRUCT_SET:
        case OP_SET_FIELD_THIS:
            SHAPE(1, 0);

        /* Lot 3 : classes, methodes, appels. */
        case OP_CLASS: case OP_METHOD:
        case OP_STATIC_FIELD:
        case OP_IS_CLASS:
        case OP_CLASS_DECLARE_INTERFACE:
            SHAPE(1, 0);
        case OP_INVOKE: case OP_INVOKE_SAFE: case OP_INVOKE_SUPER:
            SHAPE(1, 1);                      /* nameConst + argCount */
        case OP_INVOKE_DIRECT:
            SHAPE(2, 1);                      /* classConst + nameConst + argCount */

        /* Lot 4 : enumerations et litteraux. */
        case OP_CONSTANT:
        case OP_ENUM_VALUE:
            SHAPE(1, 0);

        /* --- Taille variable : dimCount puis dimCount octets --- */
        case OP_ARRAY_PRESIZE_ND:
            out->constIdx = 0; out->raw = 1; out->varRaw = 1;
            return true;

        default:
            return false;
    }
}
#undef SHAPE

int chunk_opcode_size(uint8_t op, const Chunk* chunk, int offset) {
    OpcodeShape s;
    if (!chunk_opcode_shape(op, &s)) return -1;
    int idxBytes = OWL_CONST_IDX_BYTES;
    int size = 1 + s.constIdx * idxBytes + s.raw;
    if (s.varRaw) {
        /* OP_ARRAY_PRESIZE_ND : l'octet dimCount annonce autant d'octets. */
        if (offset + 1 >= chunk->count) return -1;
        size += chunk->code[offset + 1];
    }
    return size;
}

bool chunk_opcode_has_const_index(uint8_t op) {
    OpcodeShape s;
    if (!chunk_opcode_shape(op, &s)) return false;
    return s.constIdx > 0;
}

bool chunk_walk_is_consistent(const Chunk* chunk) {
    int i = 0;
    while (i < chunk->count) {
        int sz = chunk_opcode_size(chunk->code[i], chunk, i);
        if (sz <= 0) return false;
        i += sz;
    }
    return i == chunk->count;
}

int chunk_last_instruction_offset(const Chunk* chunk, int from) {
    if (from < 0 || from > chunk->count) return -1;
    int i = from;
    int last = -1;
    while (i < chunk->count) {
        int sz = chunk_opcode_size(chunk->code[i], chunk, i);
        if (sz <= 0) return -1;
        last = i;
        i += sz;
    }
    /* Atterrissage hors de count : `from` n'etait pas une frontiere, la marche
     * a decode des operandes comme des opcodes. Aucune reponse a donner. */
    if (i != chunk->count) return -1;
    return last;
}

bool chunk_slice_has_jumps(const Chunk* chunk, int offset, int len) {
    if (len <= 0) return false;
    if (offset < 0 || offset + len > chunk->count) return true;

    int end = offset + len;
    int i = offset;
    while (i < end) {
        uint8_t op = chunk->code[i];
        switch (op) {
            case OP_JUMP:
            case OP_JUMP_IF_FALSE:
            case OP_JUMP_IF_NIL:
            case OP_JUMP_IF_NOT_NIL:
            case OP_LOOP:
            case OP_TRY:
                return true;
            default: break;
        }
        int sz = chunk_opcode_size(op, chunk, i);
        if (sz <= 0) {
            /* Opcode inconnu : on rejette par sécurité, le slice peut
             * contenir des jumps qu'on saurait pas relocaliser. */
            return true;
        }
        i += sz;
    }
    return false;
}
