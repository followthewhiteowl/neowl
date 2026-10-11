// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef owl_debug_h
#define owl_debug_h

#include "chunk.h"

void debug_disassemble_chunk(Chunk* chunk, const char* name);
int debug_disassemble_instruction(Chunk* chunk, int offset);

#endif
