#include <stdlib.h>

#include "vm/tea_chunk.h"
#include "memory/tea_memory.h"
#include "vm/tea_vm.h"
#include "util/tea_array.h"

void tea_init_chunk(TeaChunk* chunk)
{
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    tea_init_value_array(&chunk->constants);

    chunk->line_count = 0;
    chunk->line_capacity = 0;
    chunk->lines = NULL;
}

void tea_free_chunk(TeaChunk* chunk)
{
    FREE_ARRAY(uint8_t, chunk->code, chunk->capacity);
    FREE_ARRAY(int, chunk->lines, chunk->capacity);
    tea_free_value_array(&chunk->constants);
    tea_init_chunk(chunk);
    
    FREE_ARRAY(TeaLineStart, chunk->lines, chunk->line_capacity);
}

void tea_write_chunk(TeaChunk* chunk, uint8_t byte, int line)
{
    if(chunk->capacity < chunk->count + 1)
    {
        int old_capacity = chunk->capacity;
        chunk->capacity = GROW_CAPACITY(old_capacity);
        chunk->code = GROW_ARRAY(uint8_t, chunk->code, old_capacity, chunk->capacity);
    }

    chunk->code[chunk->count] = byte;
    chunk->count++;
    
    // See if we're still on the same line
    if(chunk->line_count > 0 && chunk->lines[chunk->line_count - 1].line == line)
    {
        return;
    }

    // Append a new TeaLineStart
    if(chunk->line_capacity < chunk->line_count + 1)
    {
        int old_capacity = chunk->line_capacity;
        chunk->line_capacity = GROW_CAPACITY(old_capacity);
        chunk->lines = GROW_ARRAY(TeaLineStart, chunk->lines, old_capacity, chunk->line_capacity);
    }

    TeaLineStart* line_start = &chunk->lines[chunk->line_count++];
    line_start->offset = chunk->count - 1;
    line_start->line = line;
}

// Add a constant to the array and get the index back
void tea_write_constant(TeaChunk* chunk, TeaValue value, int line)
{
    int index = tea_add_constant(chunk, value);

    // Check if the constant fits inside a single chunk
    if(index < 256)     // Short OPCODE
    {
        tea_write_chunk(chunk, OP_CONSTANT, line);
        tea_write_chunk(chunk, (uint8_t)index, line);
    }
    else    // Long OPCODE
    {
        tea_write_chunk(chunk, OP_CONSTANT_LONG, line);
        tea_write_chunk(chunk, (uint8_t)(index & 0xff), line);
        tea_write_chunk(chunk, (uint8_t)((index >> 8) & 0xff), line);
        tea_write_chunk(chunk, (uint8_t)((index >> 16) & 0xff), line);
    }
}

int tea_add_constant(TeaChunk* chunk, TeaValue value)
{
    tea_push(value);
    tea_write_value_array(&chunk->constants, value);
    tea_pop();

    return chunk->constants.count - 1;
}

int tea_get_line(TeaChunk* chunk, int instruction)
{
    int start = 0;
    int end = chunk->line_count - 1;

    for (;;) 
    {
        int mid = (start + end) / 2;
        TeaLineStart* line = &chunk->lines[mid];
        if(instruction < line->offset) 
        {
            end = mid - 1;
        } 
        else if(mid == chunk->line_count - 1 || instruction < chunk->lines[mid + 1].offset) 
        {
            return line->line;
        } 
        else 
        {
            start = mid + 1;
        }
    }
}