#include <setjmp.h>
#include <stdlib.h>

#include "tea_common.h"
#include "tea_do.h"
#include "tea_vm.h"
#include "tea_debug.h"
#include "tea_compiler.h"

struct tea_longjmp
{
    struct tea_longjmp* previous;
    jmp_buf buf;
    volatile int status;
};

void teaD_append_callframe(TeaState* T, TeaObjectClosure* closure, TeaValue* start)
{
    TeaCallInfo* frame = T->ci++;
    frame->slots = start;
    frame->base = start;
    frame->closure = closure;
    frame->native = NULL;
    frame->ip = closure->function->chunk.code;
}

void realloc_ci(TeaState* T, int new_size)
{
    TeaCallInfo* oldci = T->base_ci;
    T->base_ci = (TeaCallInfo*)teaM_reallocate(T, T->base_ci, sizeof(TeaCallInfo) * T->ci_size, sizeof(TeaCallInfo) * new_size);
    T->ci_size = new_size;
    T->ci = T->base_ci + (T->ci - oldci);
    T->end_ci = T->base_ci + T->ci_size;
}

void teaD_ensure_callframe(TeaState* T)
{
    if(T->ci + 1 > T->end_ci)
    {
        realloc_ci(T, T->ci_size * 2);
    }
    if(T->ci_size > TEA_MAX_CALLS)
    {
        teaV_runtime_error(T, "Stack overflow");
    }
}

void teaD_ensure_stack(TeaState* T, int needed)
{
    if(T->stack_size >= needed) return;

	int capacity = (int)teaM_closest_power_of_two((int)needed);
	TeaValue* old_stack = T->stack;

	T->stack = (TeaValue*)teaM_reallocate(T, T->stack, sizeof(TeaValue) * T->stack_size, sizeof(TeaValue) * capacity);
	T->stack_size = capacity;

	if(T->stack != old_stack)
    {
        for(TeaCallInfo* ci = T->base_ci; ci <= T->ci; ci++)
        {
			ci->slots = T->stack + (ci->slots - old_stack);
			ci->base = T->stack + (ci->base - old_stack);
		}

		for(TeaObjectUpvalue* upvalue = T->open_upvalues; upvalue != NULL; upvalue = upvalue->next)
        {
			upvalue->location = T->stack + (upvalue->location - old_stack);
		}

		T->top = T->stack + (T->top - old_stack);
		T->base = T->stack + (T->base - old_stack);
	}
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

    teaD_ensure_callframe(T);

    int stack_size = (int)(T->top - T->stack);
    int needed = stack_size + closure->function->max_slots;
	teaD_ensure_stack(T, needed);

    teaD_append_callframe(T, closure, T->top - arg_count - 1);
}

static void callc(TeaState* T, TeaObjectNative* native, int arg_count)
{
    teaD_ensure_callframe(T);

    TeaCallInfo* frame = T->ci++;
    frame->closure = NULL;
    frame->ip = NULL;
    frame->native = native;
    frame->slots = T->top - arg_count - 1;
    frame->base = T->base;

    if(native->type > 0) 
        T->base = T->top - arg_count - 1;
    else 
        T->base = T->top - arg_count;

    native->fn(T);
    
    TeaValue res = T->top[-1];

    frame = --T->ci;

    T->base = frame->base;
    T->top = frame->slots;

    teaV_push(T, res);
}

void teaD_call_value(TeaState* T, TeaValue callee, uint8_t arg_count)
{
    if(IS_OBJECT(callee))
    {
        switch(OBJECT_TYPE(callee))
        {
            case OBJ_BOUND_METHOD:
            {
                TeaObjectBoundMethod* bound = AS_BOUND_METHOD(callee);
                T->top[-arg_count - 1] = bound->receiver;
                teaD_call_value(T, bound->method, arg_count);
                return;
            }
            case OBJ_CLASS:
            {
                TeaObjectClass* klass = AS_CLASS(callee);
                T->top[-arg_count - 1] = OBJECT_VAL(teaO_new_instance(T, klass));
                if(!IS_NULL(klass->constructor)) 
                {
                    teaD_call_value(T, klass->constructor, arg_count);
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
    teaD_call_value(T, func, arg_count);

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