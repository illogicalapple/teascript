// tea_chunk.h
// Teascript chunks

#ifndef TEA_CHUNK_H
#define TEA_CHUNK_H

#include "tea_common.h"
#include "tea_value.h"

typedef enum
{
    #define OPCODE(name, _) OP_##name
    #include "tea_opcodes.h"
    #undef OPCODE
} TeaOpCode;

typedef struct
{
    int offset;
    int line;
} TeaLineStart;

typedef struct
{
    int count;
    int capacity;
    uint8_t* code;
    TeaValueArray constants;
    int line_count;
    int line_capacity;
    TeaLineStart* lines;
} TeaChunk;

void tea_init_chunk(TeaChunk* chunk);
void tea_free_chunk(TeaState* T, TeaChunk* chunk);
void tea_write_chunk(TeaState* T, TeaChunk* chunk, uint8_t byte, int line);
int tea_add_constant(TeaState* T, TeaChunk* chunk, TeaValue value);
int tea_getline(TeaChunk* chunk, int instruction);

#endif