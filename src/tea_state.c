// tea_state.c
// Teascript global state

#include <stdlib.h>
#include <string.h>

#include "tea_state.h"
#include "tea_compiler.h"
#include "tea_core.h"
#include "tea_vm.h"
#include "tea_util.h"
#include "tea_do.h"

static void free_state(TeaState* T)
{
    free(T);
}

static void free_stack(TeaState* T)
{
    TEA_FREE_ARRAY(T, TeaCallFrame, T->frames, T->frame_capacity);
    TEA_FREE_ARRAY(T, TeaValue, T->stack, T->stack_capacity);
}

static void init_stack(TeaState* T)
{
    T->stack = TEA_ALLOCATE(T, TeaValue, TEA_MIN_SLOTS);
    T->stack_capacity = TEA_MIN_SLOTS;
    T->base = T->top = T->stack;
    T->frames = TEA_ALLOCATE(T, TeaCallFrame, 8);
    T->frame_count = 0;
    T->frame_capacity = 8;
    T->open_upvalues = NULL;
}

TEA_API TeaState* tea_open()
{
    TeaState* T = (TeaState*)malloc(sizeof(*T));
    if(T == NULL) 
        return T;
    T->error_jump = NULL;
    T->objects = NULL;
    T->last_module = NULL;
    T->bytes_allocated = 0;
    T->next_gc = 1024 * 1024;
    init_stack(T);
    T->gray_stack = NULL;
    T->gray_count = 0;
    T->gray_capacity = 0;
    T->list_class = NULL;
    T->string_class = NULL;
    T->map_class = NULL;
    T->file_class = NULL;
    T->range_class = NULL;
    teaT_init(&T->modules);
    teaT_init(&T->globals);
    teaT_init(&T->constants);
    teaT_init(&T->strings);
    T->constructor_string = teaO_copy_string(T, "constructor", 11);
    T->repl_string = teaO_copy_string(T, "_", 1);
    T->repl = false;
    tea_open_core(T);
    return T;
}

TEA_API void tea_close(TeaState* T)
{
    T->constructor_string = NULL;
    T->repl_string = NULL;
    
    if(T->repl) 
        teaT_free(T, &T->constants);

    teaT_free(T, &T->modules);
    teaT_free(T, &T->globals);
    teaT_free(T, &T->constants);
    teaT_free(T, &T->strings);
    free_stack(T);
    teaM_free_objects(T);

#if defined(TEA_DEBUG_TRACE_MEMORY) || defined(TEA_DEBUG_FINAL_MEMORY)
    printf("total bytes lost: %zu\n", T->bytes_allocated);
#endif

    free_state(T);
}

TeaObjectClass* teaE_get_class(TeaState* T, TeaValue value)
{
    if(IS_OBJECT(value))
    {
        switch(OBJECT_TYPE(value))
        {
            case OBJ_LIST: return T->list_class;
            case OBJ_MAP: return T->map_class;
            case OBJ_STRING: return T->string_class;
            case OBJ_RANGE: return T->range_class;
            case OBJ_FILE: return T->file_class;
            default:;
        }
    }
    return NULL;
}

TEA_API TeaInterpretResult tea_interpret(TeaState* T, const char* module_name, const char* source)
{
    //return teaV_interpret_module(T, module_name, source);
    TeaObjectString* name = teaO_new_string(T, module_name);
    teaV_push(T, OBJECT_VAL(name));
    TeaObjectModule* module = teaO_new_module(T, name);
    teaV_pop(T, 1);

    teaV_push(T, OBJECT_VAL(module));
    module->path = teaZ_get_directory(T, (char*)module_name);
    teaV_pop(T, 1);
    
    TeaObjectFunction* function = teaY_compile(T, module, source);
    if(function == NULL)
        return TEA_COMPILE_ERROR;

    teaV_push(T, OBJECT_VAL(function));
    TeaObjectClosure* closure = teaO_new_closure(T, function);
    teaV_pop(T, 1);

    teaV_push(T, OBJECT_VAL(closure));
    //teaD_call_value(T, OBJECT_VAL(closure), 0);

    //return teaV_run(T);
    return teaD_pcall(T, OBJECT_VAL(closure), 0);
}