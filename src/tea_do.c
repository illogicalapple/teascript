#include <setjmp.h>
#include <stdlib.h>

#include "tea_common.h"
#include "tea_do.h"
#include "tea_vm.h"
#include "tea_compiler.h"

struct tea_longjmp
{
    struct tea_longjmp* previous;
    jmp_buf buf;
    volatile int status;
};

void realloc_ci(TeaState* T, int new_size)
{
    TeaCallInfo* old_ci = T->base_ci;
    T->base_ci = TEA_GROW_ARRAY(T, TeaCallInfo, T->base_ci, T->ci_size, new_size);
    T->ci_size = new_size;
    T->ci = T->base_ci + (T->ci - old_ci);
    T->end_ci = T->base_ci + T->ci_size - 1;
}

void teaD_grow_ci(TeaState* T)
{
    if(T->ci + 1 >= T->end_ci)
    {
        realloc_ci(T, T->ci_size * 2);
    }
    if(T->ci_size > TEA_MAX_CALLS)
    {
        teaV_runtime_error(T, "Stack overflow");
    }
}

static void correct_stack(TeaState* T, TeaValue* old_stack)
{
    for(TeaCallInfo* ci = T->base_ci; ci < T->ci; ci++)
    {
        ci->slots = (ci->slots - old_stack) + T->stack;
    }

    for(TeaObjectUpvalue* upvalue = T->open_upvalues; upvalue != NULL; upvalue = upvalue->next)
    {
        upvalue->location = (upvalue->location - old_stack) + T->stack;
    }

    T->top = (T->top - old_stack) + T->stack;
    T->base = (T->base - old_stack) + T->stack;
}

static void realloc_stack(TeaState* T, int new_size)
{
	TeaValue* old_stack = T->stack;
	T->stack = TEA_GROW_ARRAY(T, TeaValue, T->stack, T->stack_size, new_size);
	T->stack_size = new_size;
    T->stack_last = T->stack + new_size - 1;
    correct_stack(T, old_stack);
}

void teaD_grow_stack(TeaState* T, int n)
{
	if(n <= T->stack_size)
        realloc_stack(T, 2 * T->stack_size);
    else
        realloc_stack(T, T->stack_size + n);
}

static void call(TeaState* T, TeaObjectClosure* closure, int arg_count)
{
    if(arg_count < closure->function->arity)
    {
        if((arg_count + closure->function->variadic) == closure->function->arity)
        {
            // add missing variadic param ([])
            TeaObjectList* list = teaO_new_list(T);
            teaV_push(T, OBJECT_VAL(list));
            arg_count++;
        }
        else
        {
            teaV_runtime_error(T, "Expected %d arguments, but got %d", closure->function->arity, arg_count);
        }
    }
    else if(arg_count > closure->function->arity + closure->function->arity_optional)
    {
        if(closure->function->variadic)
        {
            int arity = closure->function->arity + closure->function->arity_optional;
            // +1 for the variadic param itself
            int varargs = arg_count - arity + 1;
            TeaObjectList* list = teaO_new_list(T);
            teaV_push(T, OBJECT_VAL(list));
            for(int i = varargs; i > 0; i--)
            {
                tea_write_value_array(T, &list->items, teaV_peek(T, i));
            }
            // +1 for the list pushed earlier on the stack
            T->top -= varargs + 1;
            teaV_push(T, OBJECT_VAL(list));
            arg_count = arity;
        }
        else
        {
            teaV_runtime_error(T, "Expected %d arguments, but got %d", closure->function->arity + closure->function->arity_optional, arg_count);
        }
    }
    else if(closure->function->variadic)
    {
        // last argument is the variadic arg
        TeaObjectList* list = teaO_new_list(T);
        teaV_push(T, OBJECT_VAL(list));
        tea_write_value_array(T, &list->items, teaV_peek(T, 1));
        T->top -= 2;
        teaV_push(T, OBJECT_VAL(list));
    }

    teaD_grow_ci(T);
    teaD_checkstack(T, closure->function->max_slots);

    TeaCallInfo* frame = T->ci++;
    frame->slots = T->top - arg_count - 1;
    frame->closure = closure;
    frame->native = NULL;
    frame->ip = closure->function->chunk.code;
}

static void callc(TeaState* T, TeaObjectNative* native, int arg_count)
{
    teaD_grow_ci(T);
    teaD_checkstack(T, TEA_MIN_SLOTS);

    TeaCallInfo* frame = T->ci++;
    frame->slots = T->top - arg_count - 1;
    frame->closure = NULL;
    frame->native = native;
    frame->ip = NULL;

    if(native->type > 0) 
        T->base = T->top - arg_count - 1;
    else 
        T->base = T->top - arg_count;

    native->fn(T);
    
    TeaValue res = T->top[-1];

    frame = --T->ci;

    T->base = frame->slots;
    T->top = frame->slots;

    teaV_push(T, res);
}

void teaD_precall(TeaState* T, TeaValue callee, uint8_t arg_count)
{
    if(IS_OBJECT(callee))
    {
        switch(OBJECT_TYPE(callee))
        {
            case OBJ_BOUND_METHOD:
            {
                TeaObjectBoundMethod* bound = AS_BOUND_METHOD(callee);
                T->top[-arg_count - 1] = bound->receiver;
                teaD_precall(T, bound->method, arg_count);
                return;
            }
            case OBJ_CLASS:
            {
                TeaObjectClass* klass = AS_CLASS(callee);
                T->top[-arg_count - 1] = OBJECT_VAL(teaO_new_instance(T, klass));
                if(!IS_NULL(klass->constructor)) 
                {
                    teaD_precall(T, klass->constructor, arg_count);
                }
                else if(arg_count != 0)
                {
                    teaV_runtime_error(T, "Expected 0 arguments but got %d", arg_count);
                }
                return;
            }
            case OBJ_CLOSURE:
                call(T, AS_CLOSURE(callee), arg_count);
                return;
            case OBJ_NATIVE:
                callc(T, AS_NATIVE(callee), arg_count);
                return;
            default:
                break; // Non-callable object type
        }
    }

    teaV_runtime_error(T, "%s is not callable", teaL_type(callee));
}

struct PCall
{
    TeaValue func;
    int arg_count;
};

static void f_call(TeaState* T, void* ud)
{
    struct PCall* c = (struct PCall*)ud;
    teaD_call(T, c->func, c->arg_count);
}

void teaD_call(TeaState* T, TeaValue func, int arg_count)
{
    if(++T->nccalls >= TEA_MAX_CCALLS)
    {
        puts("C stack overflow");
        teaD_throw(T, TEA_RUNTIME_ERROR);
    }
    teaD_precall(T, func, arg_count);

    if(IS_CLOSURE(func))
    {
        teaV_run(T);
    }
    T->nccalls--;
}

static void restore_stack_limit(TeaState* T)
{
    T->stack_last = T->stack + T->stack_size - 1;
    if(T->ci_size > TEA_MAX_CALLS)
    {
        int inuse = (T->ci - T->base_ci);
        if(inuse + 1 < TEA_MAX_CALLS)
        {
            realloc_ci(T, TEA_MAX_CALLS);
        }
    }
}

int teaD_pcall(TeaState* T, TeaValue func, int arg_count)
{
    int status;
    struct PCall c;
    c.func = func;
    c.arg_count = arg_count;
    status = teaD_runprotected(T, f_call, &c);
    if(status != 0)
    {
        T->top = T->base = T->stack;
        T->ci = T->base_ci;
        T->open_upvalues = NULL;
        restore_stack_limit(T);
    }
    return status;
}

void teaD_throw(TeaState* T, int code)
{
    if(T->error_jump)
    {
        T->error_jump->status = code;
        longjmp(T->error_jump->buf, 1);
    }
    else
    {
        T->panic(T);
        exit(EXIT_FAILURE);
    }
}

int teaD_runprotected(TeaState* T, TeaPFunction f, void* ud)
{
    struct tea_longjmp tj;
    tj.status = 0;
    tj.previous = T->error_jump;
    T->error_jump = &tj;
    if(setjmp(tj.buf) == 0)
        (*f)(T, ud);
    T->error_jump = tj.previous;
    return tj.status;
}

struct PCompiler
{
    TeaObjectModule* module;
    const char* source;
};

static void f_compiler(TeaState* T, void* ud)
{
    struct PCompiler* c;
    TeaObjectFunction* function;
    TeaObjectClosure* closure;

    c = (struct PCompiler*)(ud);

    function = teaY_compile(T, c->module, c->source);
    closure = teaO_new_closure(T, function);
    teaV_push(T, OBJECT_VAL(closure));
}

int teaD_protected_compiler(TeaState* T, TeaObjectModule* module, const char* source)
{
    struct PCompiler c;
    int status;
    c.module = module;
    c.source = source;
    status = teaD_runprotected(T, f_compiler, &c);
    return status;
}