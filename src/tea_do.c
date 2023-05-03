#include <setjmp.h>
#include <stdlib.h>

#include "tea_do.h"
#include "tea_vm.h"
#include "tea_debug.h"

struct tea_longjmp
{
    struct tea_longjmp* previous;
    jmp_buf buf;
    volatile int status;
};

void teaD_append_callframe(TeaState* T, TeaObjectClosure* closure, TeaValue* start)
{
    TeaCallFrame* frame = &T->frames[T->frame_count++];
    frame->slots = start;
    frame->base = start;
    frame->closure = closure;
    frame->ip = closure->function->chunk.code;
}

void teaD_ensure_callframe(TeaState* T)
{
    if(T->frame_count + 1 > T->frame_capacity)
    {
        int max = T->frame_capacity * 2;
        T->frames = (TeaCallFrame*)teaM_reallocate(T, T->frames, sizeof(TeaCallFrame) * T->frame_capacity, sizeof(TeaCallFrame) * max);
        T->frame_capacity = max;
    }
}

void teaD_ensure_stack(TeaState* T, int needed)
{
    if(T->stack_capacity >= needed) return;

	int capacity = (int)tea_closest_power_of_two((int)needed);
	TeaValue* old_stack = T->stack;

	T->stack = (TeaValue*)teaM_reallocate(T, T->stack, sizeof(TeaValue) * T->stack_capacity, sizeof(TeaValue) * capacity);
	T->stack_capacity = capacity;

	if(T->stack != old_stack)
    {
		for(int i = 0; i < T->frame_capacity; i++)
        {
			TeaCallFrame* frame = &T->frames[i];
			frame->slots = T->stack + (frame->slots - old_stack);
		}

		for(TeaObjectUpvalue* upvalue = T->open_upvalues; upvalue != NULL; upvalue = upvalue->next)
        {
			upvalue->location = T->stack + (upvalue->location - old_stack);
		}

		T->top = T->stack + (T->top - old_stack);
	}
}

static bool call(TeaState* T, TeaObjectClosure* closure, int arg_count)
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
            return false;
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
            return false;
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

    if(T->frame_count == 1000)
    {
        teaV_runtime_error(T, "Stack overflow");
        return false;
    }

    teaD_ensure_callframe(T);

    int stack_size = (int)(T->top - T->stack);
    int needed = stack_size + closure->function->max_slots;
	teaD_ensure_stack(T, needed);

    teaD_append_callframe(T, closure, T->top - arg_count - 1);

    return true;
}

static bool callc(TeaState* T, TeaObjectNative* native, int arg_count)
{
    teaD_ensure_callframe(T);

    TeaCallFrame* frame = &T->frames[T->frame_count++];
    frame->closure = NULL;
    frame->ip = NULL;
    frame->native = native;
    frame->slots = T->top - arg_count - 1;
    frame->base = T->base;

    if(native->type == NATIVE_METHOD || native->type == NATIVE_PROPERTY) 
        T->base = T->top - arg_count - 1;
    else 
        T->base = T->top - arg_count;

    native->fn(T);
    
    TeaValue res = T->top[-1];

    frame = &T->frames[--T->frame_count];

    T->base = frame->base;
    T->top = frame->slots;

    teaV_push(T, res);

    return true;
}

bool teaD_call_value(TeaState* T, TeaValue callee, uint8_t arg_count)
{
    if(IS_OBJECT(callee))
    {
        switch(OBJECT_TYPE(callee))
        {
            case OBJ_BOUND_METHOD:
            {
                TeaObjectBoundMethod* bound = AS_BOUND_METHOD(callee);
                T->top[-arg_count - 1] = bound->receiver;
                return teaD_call_value(T, bound->method, arg_count);
            }
            case OBJ_CLASS:
            {
                TeaObjectClass* klass = AS_CLASS(callee);
                T->top[-arg_count - 1] = OBJECT_VAL(teaO_new_instance(T, klass));
                if(!IS_NULL(klass->constructor)) 
                {
                    return teaD_call_value(T, klass->constructor, arg_count);
                }
                else if(arg_count != 0)
                {
                    teaV_runtime_error(T, "Expected 0 arguments but got %d", arg_count);
                    return false;
                }
                return true;
            }
            case OBJ_CLOSURE:
                return call(T, AS_CLOSURE(callee), arg_count);
            case OBJ_NATIVE:
                return callc(T, AS_NATIVE(callee), arg_count);
            default:
                break; // Non-callable object type
        }
    }

    teaV_runtime_error(T, "%s is not callable", teaL_type(callee));
    return false;
}

struct Call
{
    TeaValue func;
    int arg_count;
};

static void f_call(TeaState* T, void* ud)
{
    struct Call* c = (struct Call*)ud;
    teaD_call(T, c->func, c->arg_count);
}

void teaD_call(TeaState* T, TeaValue func, int arg_count)
{
    bool status = teaD_call_value(T, func, arg_count);

    if(status && IS_CLOSURE(func))
    {
        teaV_run(T);
    }
}

int teaD_pcall(TeaState* T, TeaValue func, int arg_count)
{
    int status;
    struct Call c;
    c.func = func;
    c.arg_count = arg_count;
    status = teaD_runprotected(T, f_call, &c);
    if(status != 0)
    {
        T->top = T->base = T->stack;
        T->frame_count = 0;
        T->open_upvalues = NULL;
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
        puts("panic");
        exit(1);
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