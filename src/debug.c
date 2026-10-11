// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "debug.h"
#include "object.h"
#include "value.h"

void debug_disassemble_chunk(Chunk* chunk, const char* name) {
    printf("== %s ==\n", name);
    for (int offset = 0; offset < chunk->count;) {
        offset = debug_disassemble_instruction(chunk, offset);
    }
}

static int simple_instruction(const char* name, int offset) {
    printf("%s\n", name);
    return offset + 1;
}

/* Lit l'index de constante n°`which` (0-based) d'une instruction, a la
 * largeur declaree par la table canonique. */
static int read_const_operand(Chunk* chunk, int offset, int which) {
    int at = offset + 1 + which * OWL_CONST_IDX_BYTES;
    return (chunk->code[at] << 8) | chunk->code[at + 1];
}

static const char* const_operand_text(Chunk* chunk, int idx) {
    if (idx < 0 || idx >= chunk->constants.count) return "<hors pool>";
    Value v = chunk->constants.values[idx];
    return IS_STRING(v) ? AS_STRING(v)->chars : "<non-chaine>";
}

static int constant_instruction(const char* name, Chunk* chunk, int offset) {
    int constant = read_const_operand(chunk, offset, 0);
    printf("%-16s %4d '", name, constant);
    if (constant >= 0 && constant < chunk->constants.count) {
        value_print(chunk->constants.values[constant]);
    }
    printf("'\n");
    return offset + chunk_opcode_size(chunk->code[offset], chunk, offset);
}

static int byte_instruction(const char* name, Chunk* chunk, int offset) {
    uint8_t slot = chunk->code[offset + 1];
    printf("%-16s %4d\n", name, slot);
    return offset + 2;
}

static int jump_instruction(const char* name, int sign, Chunk* chunk, int offset) {
    uint16_t jump = (uint16_t)(chunk->code[offset + 1] << 8);
    jump |= chunk->code[offset + 2];
    printf("%-16s %4d -> %d\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

int debug_disassemble_instruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:      return constant_instruction("OP_CONSTANT", chunk, offset);
        case OP_NIL:           return simple_instruction("OP_NIL", offset);
        case OP_TRUE:          return simple_instruction("OP_TRUE", offset);
        case OP_FALSE:         return simple_instruction("OP_FALSE", offset);
        case OP_ADD:           return simple_instruction("OP_ADD", offset);
        case OP_SUBTRACT:      return simple_instruction("OP_SUBTRACT", offset);
        case OP_MULTIPLY:      return simple_instruction("OP_MULTIPLY", offset);
        case OP_DIVIDE:        return simple_instruction("OP_DIVIDE", offset);
        case OP_MODULO:        return simple_instruction("OP_MODULO", offset);
        case OP_POWER:         return simple_instruction("OP_POWER", offset);
        case OP_NEGATE:        return simple_instruction("OP_NEGATE", offset);
        case OP_INCREMENT:     return simple_instruction("OP_INCREMENT", offset);
        case OP_DECREMENT:     return simple_instruction("OP_DECREMENT", offset);
        case OP_EQUAL:         return simple_instruction("OP_EQUAL", offset);
        case OP_NOT_EQUAL:     return simple_instruction("OP_NOT_EQUAL", offset);
        case OP_GREATER:       return simple_instruction("OP_GREATER", offset);
        case OP_GREATER_EQUAL: return simple_instruction("OP_GREATER_EQUAL", offset);
        case OP_LESS:          return simple_instruction("OP_LESS", offset);
        case OP_LESS_EQUAL:    return simple_instruction("OP_LESS_EQUAL", offset);
        case OP_NOT:           return simple_instruction("OP_NOT", offset);
        case OP_AND_FULL:      return simple_instruction("OP_AND_FULL", offset);
        case OP_OR_FULL:       return simple_instruction("OP_OR_FULL", offset);
        case OP_CONCAT:        return simple_instruction("OP_CONCAT", offset);
        case OP_DEFINE_GLOBAL: return constant_instruction("OP_DEF_GLOBAL", chunk, offset);
        case OP_GET_GLOBAL:        return constant_instruction("OP_GET_GLOBAL", chunk, offset);
        case OP_GET_GLOBAL_CALLEE: return constant_instruction("OP_GET_GLOBAL_CALLEE", chunk, offset);
        case OP_GET_ELEMENT_GLOBAL: return constant_instruction("OP_GET_ELEMENT_GLOBAL", chunk, offset);
        case OP_GET_TYPE_GLOBAL:   return constant_instruction("OP_GET_TYPE_GLOBAL", chunk, offset);
        case OP_SET_GLOBAL:        return constant_instruction("OP_SET_GLOBAL", chunk, offset);
        case OP_GET_LOCAL:     return byte_instruction("OP_GET_LOCAL", chunk, offset);
        case OP_SET_LOCAL:     return byte_instruction("OP_SET_LOCAL", chunk, offset);
        case OP_APPEND_IN_PLACE: return byte_instruction("OP_APPEND_INPLACE", chunk, offset);
        case OP_APPEND_IN_PLACE_GLOBAL: return constant_instruction("OP_APPEND_INPL_G", chunk, offset);
        case OP_JUMP:          return jump_instruction("OP_JUMP", 1, chunk, offset);
        case OP_JUMP_IF_FALSE: return jump_instruction("OP_JUMP_FALSE", 1, chunk, offset);
        case OP_LOOP:          return jump_instruction("OP_LOOP", -1, chunk, offset);
        case OP_CALL:          return byte_instruction("OP_CALL", chunk, offset);
        case OP_RETURN:        return simple_instruction("OP_RETURN", offset);
        case OP_POP:           return simple_instruction("OP_POP", offset);
        case OP_POPN:          return byte_instruction("OP_POPN", chunk, offset);
        case OP_DUP:           return simple_instruction("OP_DUP", offset);
        case OP_SWAP:          return simple_instruction("OP_SWAP", offset);
        case OP_PRINT:         return simple_instruction("OP_PRINT", offset);
        case OP_TO_UNDETECTABLE: return simple_instruction("OP_TO_UNDETECTABLE", offset);
        case OP_ARRAY:         return simple_instruction("OP_ARRAY", offset);
        case OP_ARRAY_PRESIZE: return byte_instruction("OP_ARRAY_PRESIZE", chunk, offset);
        case OP_ARRAY_GROWTH: return byte_instruction("OP_ARRAY_GROWTH", chunk, offset);
        case OP_ARRAY_PRESIZE_ND: {
            int dimCount = chunk->code[offset + 1];
            printf("%-22s %d dims:", "OP_ARRAY_PRESIZE_ND", dimCount);
            for (int k = 0; k < dimCount; k++) {
                printf(" %d", chunk->code[offset + 2 + k]);
            }
            printf("\n");
            return offset + 2 + dimCount;
        }
        case OP_ARRAY_TYPE_STRUCT: return constant_instruction("OP_ARRAY_TYPE_STRUCT", chunk, offset);
        case OP_ARRAY_FILL_DEFAULT: return byte_instruction("OP_ARRAY_FILL_DEFAULT", chunk, offset);
        case OP_ARRAY_FILL_STRUCT: return constant_instruction("OP_ARRAY_FILL_STRUCT", chunk, offset);
        case OP_ARRAY_MARK_GROUP: return simple_instruction("OP_ARRAY_MARK_GROUP", offset);
        case OP_STRUCT_FIELD_ORDER: return constant_instruction("OP_STRUCT_FIELD_ORDER", chunk, offset);
        case OP_ARRAY_GET:     return simple_instruction("OP_ARRAY_GET", offset);
        case OP_TABLE_CELL_DESC: return simple_instruction("OP_TABLE_CELL_DESC", offset);
        case OP_ARRAY_SET:     return simple_instruction("OP_ARRAY_SET", offset);
        case OP_ARRAY_GET_2D:  return simple_instruction("OP_ARRAY_GET_2D", offset);
        case OP_ARRAY_SET_2D:  return simple_instruction("OP_ARRAY_SET_2D", offset);
        case OP_ARRAY_LENGTH:  return simple_instruction("OP_ARRAY_LENGTH", offset);
        case OP_STRUCT_GET:    return constant_instruction("OP_STRUCT_GET", chunk, offset);
        case OP_STRUCT_SET:    return constant_instruction("OP_STRUCT_SET", chunk, offset);
        case OP_TRY:           return jump_instruction("OP_TRY", 1, chunk, offset);
        case OP_END_TRY:       return simple_instruction("OP_END_TRY", offset);
        case OP_EXC_RESTORE:   return simple_instruction("OP_EXC_RESTORE", offset);
        case OP_THROW:         return simple_instruction("OP_THROW", offset);
        case OP_CLASS:         return constant_instruction("OP_CLASS", chunk, offset);
        case OP_METHOD:        return constant_instruction("OP_METHOD", chunk, offset);
        case OP_ENUM_VALUE:    return constant_instruction("OP_ENUM_VALUE", chunk, offset);
        case OP_STATIC_FIELD:  return constant_instruction("OP_STATIC_FIELD", chunk, offset);
        case OP_MARK_CLASS_KIND: return byte_instruction("OP_MARK_CLASS_KIND", chunk, offset);
        case OP_ITER_BEGIN:      return simple_instruction("OP_ITER_BEGIN", offset);
        case OP_ITER_TRACK_KEY:  return simple_instruction("OP_ITER_TRACK_KEY", offset);
        case OP_ITER_END:        return simple_instruction("OP_ITER_END", offset);
        case OP_OUT_PARAM_TO_LOCAL: {
            printf("%-22s slot=%d idx=%d\n", "OP_OUT_PARAM_TO_LOCAL",
                   chunk->code[offset+1], chunk->code[offset+2]);
            return offset + 3;
        }
        case OP_OUT_PARAM_TO_GLOBAL: {
            int size = chunk_opcode_size(instruction, chunk, offset);
            printf("%-22s name=%d idx=%d\n", "OP_OUT_PARAM_TO_GLOBAL",
                   read_const_operand(chunk, offset, 0),
                   chunk->code[offset + size - 1]);
            return offset + size;
        }
        case OP_ARRAY_PACK:    return byte_instruction("OP_ARRAY_PACK", chunk, offset);
        case OP_ARRAY_UNPACK:  return byte_instruction("OP_ARRAY_UNPACK", chunk, offset);
        case OP_COLLECTION:    return byte_instruction("OP_COLLECTION", chunk, offset);
        case OP_INVOKE:
        case OP_INVOKE_SAFE:
        case OP_INVOKE_SUPER: {
            int size = chunk_opcode_size(instruction, chunk, offset);
            int nameIdx = read_const_operand(chunk, offset, 0);
            uint8_t argCount = chunk->code[offset + size - 1];
            printf("%-16s %4d '%s' (%d args)\n",
                   instruction == OP_INVOKE      ? "OP_INVOKE" :
                   instruction == OP_INVOKE_SAFE ? "OP_INVOKE_SAFE"
                                                 : "OP_INVOKE_SUPER",
                   nameIdx, const_operand_text(chunk, nameIdx), argCount);
            return offset + size;
        }
        case OP_INHERIT:       return simple_instruction("OP_INHERIT", offset);
        case OP_GET_THIS:      return simple_instruction("OP_GET_THIS", offset);
        case OP_MOIMEME:       return simple_instruction("OP_MOIMEME", offset);
        case OP_MONCHAMP:      return simple_instruction("OP_MONCHAMP", offset);
        case OP_MAFENETRE:     return simple_instruction("OP_MAFENETRE", offset);
        case OP_SET_FIELD_THIS: return constant_instruction("OP_SET_FLD_THIS", chunk, offset);
        case OP_INVOKE_DIRECT: {
            int size = chunk_opcode_size(instruction, chunk, offset);
            int classIdx = read_const_operand(chunk, offset, 0);
            int nameIdx  = read_const_operand(chunk, offset, 1);
            uint8_t argCount = chunk->code[offset + size - 1];
            printf("%-16s class=%d '%s' meth=%d '%s' (%d args)\n",
                   "OP_INVOKE_DIRECT",
                   classIdx, const_operand_text(chunk, classIdx),
                   nameIdx,  const_operand_text(chunk, nameIdx),
                   argCount);
            return offset + size;
        }
        case OP_IS_CLASS:      return constant_instruction("OP_IS_CLASS", chunk, offset);
        case OP_CLASS_DECLARE_INTERFACE:
            return constant_instruction("OP_CLASS_DECL_IFACE", chunk, offset);
        /* --- Aucun operande (SHAPE(0,0)) --- */
        case OP_BIT_AND:       return simple_instruction("OP_BIT_AND", offset);
        case OP_BIT_OR:        return simple_instruction("OP_BIT_OR", offset);
        case OP_BIT_XOR:       return simple_instruction("OP_BIT_XOR", offset);
        case OP_BIT_NOT:       return simple_instruction("OP_BIT_NOT", offset);
        case OP_BIT_SHL:       return simple_instruction("OP_BIT_SHL", offset);
        case OP_BIT_SHR:       return simple_instruction("OP_BIT_SHR", offset);
        case OP_DEEP_COPY:     return simple_instruction("OP_DEEP_COPY", offset);
        case OP_ITER_KEY:      return simple_instruction("OP_ITER_KEY", offset);
        case OP_RERAISE:       return simple_instruction("OP_RERAISE", offset);
        case OP_STRING_CHAR_AT: return simple_instruction("OP_STRING_CHAR_AT", offset);
        case OP_BUFFER_BYTE_SET: return simple_instruction("OP_BUFFER_BYTE_SET", offset);
        case OP_STRING_SLICE:  return simple_instruction("OP_STRING_SLICE", offset);
        case OP_STRING_SLICE_TO_END: return simple_instruction("OP_STRING_SLICE_TO_END", offset);
        case OP_STRING_SLICE_LEN: return simple_instruction("OP_STRING_SLICE_LEN", offset);
        case OP_INDIRECT_GLOBAL_GET: return simple_instruction("OP_INDIRECT_GLOBAL_GET", offset);
        case OP_INDIRECT_GLOBAL_SET: return simple_instruction("OP_INDIRECT_GLOBAL_SET", offset);
        case OP_INDIRECT_FIELD_GET: return simple_instruction("OP_INDIRECT_FIELD_GET", offset);
        case OP_INDIRECT_FIELD_SET: return simple_instruction("OP_INDIRECT_FIELD_SET", offset);
        case OP_VERIFY_BOOL_COND: return simple_instruction("OP_VERIFY_BOOL_COND", offset);
        /* --- 1 octet brut (SHAPE(0,1)) --- */
        case OP_PROC_SET_HOOK: return byte_instruction("OP_PROC_SET_HOOK", chunk, offset);
        case OP_STRING_MATCH:  return byte_instruction("OP_STRING_MATCH", chunk, offset);
        /* --- Saut 16 bits vers l'avant (SHAPE(0,2)) --- */
        case OP_JUMP_IF_NIL:   return jump_instruction("OP_JUMP_IF_NIL", 1, chunk, offset);
        case OP_JUMP_IF_NOT_NIL: return jump_instruction("OP_JUMP_IF_NOT_NIL", 1, chunk, offset);
        default: {
            /* Opcode sans rendu dedie : on affiche le numero brut mais on
             * avance de la taille canonique, sinon la suite du desassemblage
             * se decale et affiche des operandes comme des instructions. */
            int size = chunk_opcode_size(instruction, chunk, offset);
            printf("Unknown opcode %d\n", instruction);
            return offset + (size > 0 ? size : 1);
        }
    }
}
