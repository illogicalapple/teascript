// tea_memory.c
// Teascript gc and memory functions

#include <stdlib.h>

#include "tea.h"

#include "tea_common.h"
#include "tea_memory.h"
#include "tea_state.h"
#include "tea_array.h"

#ifdef TEA_DEBUG_LOG_GC
#include <stdio.h>
#include "tea_debug.h"
#endif

void* teaM_reallocate(TeaState* T, void* pointer, size_t old_size, size_t new_size)
{
    T->bytes_allocated += new_size - old_size;

#ifdef TEA_DEBUG_TRACE_MEMORY
    printf("total bytes allocated: %zu\nnew allocation: %zu\nold allocation: %zu\n\n", T->bytes_allocated, new_size, old_size);
#endif

    if(new_size > old_size)
    {
#ifdef TEA_DEBUG_STRESS_GC
        tea_collect_garbage(T);
#endif

        if(T->bytes_allocated > T->next_gc)
        {
            tea_collect_garbage(T);
        }
    }

    if(new_size == 0)
    {
        free(pointer);
        return NULL;
    }

    void* result = realloc(pointer, new_size);

    if(result == NULL)
        exit(1);

    return result;
}

// http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2Float
int teaM_closest_power_of_two(int n)
{
	n--;
	n |= n >> 1;
	n |= n >> 2;
	n |= n >> 4;
	n |= n >> 8;
	n |= n >> 16;
	n++;

	return n;
}