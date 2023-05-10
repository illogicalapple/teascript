// tea_vm.c
// Teascript virtual machine

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "tea_common.h"
#include "tea_compiler.h"
#include "tea_debug.h"
#include "tea_object.h"
#include "tea_memory.h"
#include "tea_vm.h"
#include "tea_util.h"
#include "tea_utf.h"
#include "tea_import.h"
#include "tea_do.h"

void teaV_runtime_error(TeaState* T, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for(TeaCallInfo* frame = T->ci - 1; frame >= T->base_ci; frame--)
    {
        // Skip stack trace for C functions
        if(frame->closure == NULL) continue;

        TeaObjectFunction* function = frame->closure->function;
        size_t instruction = frame->ip - function->chunk.code - 1;
        fprintf(stderr, "[line %d] in ", teaK_getline(&function->chunk, instruction));
        if(function->name == NULL)
        {
            fprintf(stderr, "script\n");
        }
        else
        {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    teaD_throw(T, TEA_RUNTIME_ERROR);
}

static void invoke_from_class(TeaState* T, TeaObjectClass* klass, TeaObjectString* name, int arg_count)
{
    TeaValue method;
    if(!teaT_get(&klass->methods, name, &method))
    {
        teaV_runtime_error(T, "Undefined property '%s'", name->chars);
    }

    teaD_call_value(T, method, arg_count);
}

static void invoke(TeaState* T, TeaValue receiver, TeaObjectString* name, int arg_count)
{
    if(IS_OBJECT(receiver))
    {
        switch(OBJECT_TYPE(receiver))
        {
            case OBJ_MODULE:
            {
                TeaObjectModule* module = AS_MODULE(receiver);

                TeaValue value;
                if(teaT_get(&module->values, name, &value)) 
                {
                    teaD_call_value(T, value, arg_count);
                    return;
                }

                teaV_runtime_error(T, "Undefined property '%s' in '%s' module", name->chars, module->name->chars);
            }
            case OBJ_INSTANCE:
            {
                TeaObjectInstance* instance = AS_INSTANCE(receiver);

                TeaValue value;
                if(teaT_get(&instance->fields, name, &value))
                {
                    T->top[-arg_count - 1] = value;
                    teaD_call_value(T, value, arg_count);
                    return;
                }

                if(teaT_get(&instance->klass->methods, name, &value)) 
                {
                    teaD_call_value(T, value, arg_count);
                    return;
                }

                teaV_runtime_error(T, "Undefined property '%s'", name->chars);
            }
            case OBJ_CLASS:
            {
                TeaObjectClass* klass = AS_CLASS(receiver);
                TeaValue method;
                if(teaT_get(&klass->methods, name, &method)) 
                {
                    if(IS_NATIVE(method) || AS_CLOSURE(method)->function->type != TYPE_STATIC) 
                    {
                        teaV_runtime_error(T, "'%s' is not static. Only static methods can be invoked directly from a class", name->chars);
                    }

                    teaD_call_value(T, method, arg_count);
                    return;
                }

                teaV_runtime_error(T, "Undefined property '%s'", name->chars);
            }
            default:
            {
                TeaObjectClass* type = teaE_get_class(T, receiver);
                if(type != NULL)
                {
                    TeaValue value;
                    if(teaT_get(&type->methods, name, &value)) 
                    {
                        teaD_call_value(T, value, arg_count);
                        return;
                    }

                    teaV_runtime_error(T, "%s has no method %s()", teaO_type(receiver), name->chars);
                }
            }
        }
    }

    teaV_runtime_error(T, "Only objects have methods, %s given", teaL_type(receiver));
}

static void bind_method(TeaState* T, TeaObjectClass* klass, TeaObjectString* name)
{
    TeaValue method;
    if(!teaT_get(&klass->methods, name, &method))
    {
        teaV_runtime_error(T, "Undefined property '%s'", name->chars);
    }

    TeaObjectBoundMethod* bound = teaO_new_bound_method(T, teaV_peek(T, 0), method);
    teaV_pop(T, 1);
    teaV_push(T, OBJECT_VAL(bound));
}

static void in_(TeaState* T, TeaValue object, TeaValue value)
{
    if(IS_OBJECT(object))
    {
        switch(OBJECT_TYPE(object))
        {
            case OBJ_STRING:
            {
                if(!IS_STRING(value))
                {
                    teaV_pop(T, 2);
                    teaV_push(T, FALSE_VAL);
                    return;
                }

                TeaObjectString* string = AS_STRING(object);
                TeaObjectString* sub = AS_STRING(value);

                if(sub == string)
                {
                    teaV_pop(T, 2);
                    teaV_push(T, TRUE_VAL);
                    return;
                }

                teaV_pop(T, 2);
                teaV_push(T, BOOL_VAL(strstr(string->chars, sub->chars) != NULL));
                return;
            }
            case OBJ_RANGE:
            {
                if(!IS_NUMBER(value))
                {
                    teaV_push(T, FALSE_VAL);
                    return;
                }

                double number = AS_NUMBER(value);
                TeaObjectRange* range = AS_RANGE(object);
                int start = range->start;
                int end = range->end;

                if(number < start || number > end)
                {
                    teaV_pop(T, 2);
                    teaV_push(T, FALSE_VAL);
                    return;
                }

                teaV_pop(T, 2);
                teaV_push(T, TRUE_VAL);
                return;
            }
            case OBJ_LIST:
            {
                TeaObjectList* list = AS_LIST(object);

                for(int i = 0; i < list->items.count; i++) 
                {
                    if(teaL_equal(list->items.values[i], value)) 
                    {
                        teaV_pop(T, 2);
                        teaV_push(T, TRUE_VAL);
                        return;
                    }
                }

                teaV_pop(T, 2);
                teaV_push(T, FALSE_VAL);
                return;
            }
            case OBJ_MAP:
            {
                TeaObjectMap* map = AS_MAP(object);
                TeaValue _;

                teaV_pop(T, 2);
                teaV_push(T, BOOL_VAL(teaO_map_get(map, value, &_)));
                return;
            }
            default:
                break;
        }
    }

    teaV_runtime_error(T, "%s is not an iterable", teaL_type(object));
}

static void subscript(TeaState* T, TeaValue index_value, TeaValue subscript_value)
{
    if(IS_OBJECT(subscript_value))
    {
        switch(OBJECT_TYPE(subscript_value))
        {
            case OBJ_RANGE:
            {
                if(!IS_NUMBER(index_value)) 
                {
                    teaV_runtime_error(T, "Range index must be a number");
                }

                TeaObjectRange* range = AS_RANGE(subscript_value);
                double index = AS_NUMBER(index_value);

                // Calculate the length of the range
                double len = (range->end - range->start) / range->step;

                // Allow negative indexes
                if(index < 0)
                {
                    index = len + index;
                }

                if(index >= 0 && index < len) 
                {
                    teaV_pop(T, 2);
                    teaV_push(T, NUMBER_VAL(range->start + index * range->step));
                    return;
                }

                teaV_runtime_error(T, "Range index out of bounds");
            }
            case OBJ_LIST:
            {
                if(!IS_NUMBER(index_value)) 
                {
                    teaV_runtime_error(T, "List index must be a number");
                }

                TeaObjectList* list = AS_LIST(subscript_value);
                int index = AS_NUMBER(index_value);

                // Allow negative indexes
                if(index < 0)
                {
                    index = list->items.count + index;
                }

                if(index >= 0 && index < list->items.count) 
                {
                    teaV_pop(T, 2);
                    teaV_push(T, list->items.values[index]);
                    return;
                }

                teaV_runtime_error(T, "List index out of bounds");
            }
            case OBJ_MAP:
            {
                TeaObjectMap* map = AS_MAP(subscript_value);
                if(!teaO_is_valid_key(index_value))
                {
                    teaV_runtime_error(T, "Map key isn't hashable");
                }

                TeaValue value;
                teaV_pop(T, 2);
                if(teaO_map_get(map, index_value, &value))
                {
                    teaV_push(T, value);
                    return;
                }

                teaV_runtime_error(T, "Key does not exist within map");
            }
            case OBJ_STRING:
            {
                if(!IS_NUMBER(index_value)) 
                {
                    teaV_runtime_error(T, "String index must be a number (got %s)", teaL_type(index_value));
                }

                TeaObjectString* string = AS_STRING(subscript_value);
                int index = AS_NUMBER(index_value);
                int real_length = teaU_length(string);

                // Allow negative indexes
                if(index < 0)
                {
                    index = real_length + index;
                }

                if(index >= 0 && index < string->length)
                {
                    teaV_pop(T, 2);
                    TeaObjectString* c = teaU_code_point_at(T, string, teaU_char_offset(string->chars, index));
                    teaV_push(T, OBJECT_VAL(c));
                    return;
                }

                teaV_runtime_error(T, "String index out of bounds");
            }
            default:
                break;
        }
    }
    
    teaV_runtime_error(T, "%s is not subscriptable", teaL_type(subscript_value));
}

static void subscript_store(TeaState* T, TeaValue item_value, TeaValue index_value, TeaValue subscript_value, bool assign)
{
    if(IS_OBJECT(subscript_value))
    {
        switch(OBJECT_TYPE(subscript_value))
        {
            case OBJ_LIST:
            {
                if(!IS_NUMBER(index_value)) 
                {
                    teaV_runtime_error(T, "List index must be a number (got %s)", teaL_type(index_value));
                }

                TeaObjectList* list = AS_LIST(subscript_value);
                int index = AS_NUMBER(index_value);

                if(index < 0)
                {
                    index = list->items.count + index;
                }

                if(index >= 0 && index < list->items.count) 
                {
                    if(assign)
                    {
                        list->items.values[index] = item_value;
                        teaV_pop(T, 3);
                        teaV_push(T, item_value);
                    }
                    else
                    {
                        T->top[-1] = list->items.values[index];
                        teaV_push(T, item_value);
                    }
                    return;
                }

                teaV_runtime_error(T, "List index out of bounds");
            }
            case OBJ_MAP:
            {
                TeaObjectMap* map = AS_MAP(subscript_value);
                if(!teaO_is_valid_key(index_value))
                {
                    teaV_runtime_error(T, "Map key isn't hashable");
                }

                if(assign)
                {
                    teaO_map_set(T, map, index_value, item_value);
                    teaV_pop(T, 3);
                    teaV_push(T, item_value);
                }
                else
                {
                    TeaValue map_value;
                    if(!teaO_map_get(map, index_value, &map_value))
                    {
                        teaV_runtime_error(T, "Key does not exist within the map");
                    }
                    T->top[-1] = map_value;
                    teaV_push(T, item_value);
                }
                return;
            }
            default:
                break;
        }
    }

    teaV_runtime_error(T, "%s does not support item assignment", teaL_type(subscript_value));
}

static void get_property(TeaState* T, TeaValue receiver, TeaObjectString* name, bool dopop)
{
    if(!IS_OBJECT(receiver))
    {
        teaV_runtime_error(T, "Only objects have properties");
    }

    switch(OBJECT_TYPE(receiver))
    {
        case OBJ_INSTANCE:
        {
            TeaObjectInstance* instance = AS_INSTANCE(receiver);
            
            TeaValue value;
            if(teaT_get(&instance->fields, name, &value))
            {
                if(dopop)
                {
                    teaV_pop(T, 1); // Instance
                }
                teaV_push(T, value);
                return;
            }

            bind_method(T, instance->klass, name);

            TeaObjectClass* klass = instance->klass;
            while(klass != NULL) 
            {
                if(teaT_get(&klass->statics, name, &value))
                {
                    if(dopop)
                    {
                        teaV_pop(T, 1); // Instance
                    }
                    teaV_push(T, value);
                    return;
                }

                klass = klass->super;
            }

            teaV_runtime_error(T, "'%s' instance has no property: '%s'", instance->klass->name->chars, name->chars);
        }
        case OBJ_CLASS:
        {
            TeaObjectClass* klass = AS_CLASS(receiver);
            TeaObjectClass* klass_store = klass;

            while(klass != NULL) 
            {
                TeaValue value;
                if(teaT_get(&klass->statics, name, &value) || teaT_get(&klass->methods, name, &value))
                {
                    if(dopop)
                    {
                        teaV_pop(T, 1); // Class
                    }
                    teaV_push(T, value);
                    return;
                }

                klass = klass->super;
            }

            teaV_runtime_error(T, "'%s' class has no property: '%s'.", klass_store->name->chars, name->chars);
        }
        case OBJ_MODULE:
        {
            TeaObjectModule* module = AS_MODULE(receiver);

            TeaValue value;
            if(teaT_get(&module->values, name, &value)) 
            {
                if(dopop)
                {
                    teaV_pop(T, 1); // Module
                }
                teaV_push(T, value);
                return;
            }

            teaV_runtime_error(T, "'%s' module has no property: '%s'", module->name->chars, name->chars);
        }
        case OBJ_MAP:
        {
            TeaObjectMap* map = AS_MAP(receiver);

            TeaValue value;
            if(teaO_map_get(map, OBJECT_VAL(name), &value))
            {
                if(dopop)
                {
                    teaV_pop(T, 1);
                }
                teaV_push(T, value);
                return;
            }
            else
            {
                goto try;
            }
            
            teaV_runtime_error(T, "map has no property: '%s'", name->chars);
        }
        default:
        {
            try:
            TeaObjectClass* type = teaE_get_class(T, receiver);
            if(type != NULL)
            {
                TeaValue value;
                if(teaT_get(&type->methods, name, &value)) 
                {
                    if(IS_NATIVE_PROPERTY(value))
                    {
                        teaD_call_value(T, value, 0);
                    }
                    else
                    {
                        teaV_pop(T, 1);
                        teaV_push(T, value);
                    }
                    return;
                }
            }
            break;
        }
    }
    teaV_runtime_error(T, "%s has no property '%s'", teaO_type(receiver), name->chars);
}

static void set_property(TeaState* T, TeaObjectString* name, TeaValue receiver, TeaValue item)
{
    if(IS_OBJECT(receiver))
    {
        switch(OBJECT_TYPE(receiver))
        {
            case OBJ_INSTANCE:
            {
                TeaObjectInstance* instance = AS_INSTANCE(receiver);
                teaT_set(T, &instance->fields, name, item);
                teaV_pop(T, 2);
                teaV_push(T, item);
                return;
            }
            case OBJ_CLASS:
            {
                TeaObjectClass* klass = AS_CLASS(receiver);
                teaT_set(T, &klass->statics, name, item);
                teaV_pop(T, 2);
                teaV_push(T, item);
                return;
            }
            case OBJ_MAP:
            {
                TeaObjectMap* map = AS_MAP(receiver);
                teaO_map_set(T, map, OBJECT_VAL(name), item);
                teaV_pop(T, 2);
                teaV_push(T, item);
                return;
            }
            case OBJ_MODULE:
            {
                TeaObjectModule* module = AS_MODULE(receiver);
                teaT_set(T, &module->values, name, item);
                teaV_pop(T, 2);
                teaV_push(T, item);
                return;
            }
            default:
                break;
        }
    }

    teaV_runtime_error(T, "Cannot set property on type %s", teaL_type(receiver));
}

static TeaObjectUpvalue* capture_upvalue(TeaState* T, TeaValue* local)
{
    TeaObjectUpvalue* prev_upvalue = NULL;
    TeaObjectUpvalue* upvalue = T->open_upvalues;
    while(upvalue != NULL && upvalue->location > local)
    {
        prev_upvalue = upvalue;
        upvalue = upvalue->next;
    }

    if(upvalue != NULL && upvalue->location == local)
    {
        return upvalue;
    }

    TeaObjectUpvalue* created_upvalue = teaO_new_upvalue(T, local);
    created_upvalue->next = upvalue;

    if(prev_upvalue == NULL)
    {
        T->open_upvalues = created_upvalue;
    }
    else
    {
        prev_upvalue->next = created_upvalue;
    }

    return created_upvalue;
}

static void close_upvalues(TeaState* T, TeaValue* last)
{
    while(T->open_upvalues != NULL && T->open_upvalues->location >= last)
    {
        TeaObjectUpvalue* upvalue = T->open_upvalues;
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        T->open_upvalues = upvalue->next;
    }
}

static void define_method(TeaState* T, TeaObjectString* name)
{
    TeaValue method = teaV_peek(T, 0);
    TeaObjectClass* klass = AS_CLASS(teaV_peek(T, 1));
    teaT_set(T, &klass->methods, name, method);
    if(name == T->constructor_string) klass->constructor = method;
    teaV_pop(T, 1);
}

static void concatenate(TeaState* T)
{
    TeaObjectString* b = AS_STRING(teaV_peek(T, 0));
    TeaObjectString* a = AS_STRING(teaV_peek(T, 1));

    int length = a->length + b->length;
    char* chars = TEA_ALLOCATE(T, char, length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    TeaObjectString* result = teaO_take_string(T, chars, length);
    teaV_pop(T, 2);
    teaV_push(T, OBJECT_VAL(result));
}

static void repeat(TeaState* T)
{
    TeaObjectString* string;
    int n;

    if(IS_STRING(teaV_peek(T, 0)) && IS_NUMBER(teaV_peek(T, 1)))
    {
        string = AS_STRING(teaV_peek(T, 0));
        n = AS_NUMBER(teaV_peek(T, 1));
    }
    else if(IS_NUMBER(teaV_peek(T, 0)) && IS_STRING(teaV_peek(T, 1)))
    {
        n = AS_NUMBER(teaV_peek(T, 0));
        string = AS_STRING(teaV_peek(T, 1));
    }

    if(n <= 0)
    {
        TeaObjectString* s = teaO_copy_string(T, "", 0);
        teaV_pop(T, 2);
        teaV_push(T, OBJECT_VAL(s));
        return;
    }
    else if(n == 1)
    {
        teaV_pop(T, 2);
        teaV_push(T, OBJECT_VAL(string));
        return;
    }

    int length = string->length;
    char* chars = TEA_ALLOCATE(T, char, (n * length) + 1);

    int i; 
    char* p;
    for(i = 0, p = chars; i < n; ++i, p += length)
    {
        memcpy(p, string->chars, length);
    }
    *p = '\0';

    TeaObjectString* result = teaO_take_string(T, chars, strlen(chars));
    teaV_pop(T, 2);
    teaV_push(T, OBJECT_VAL(result));
}

void teaV_run(TeaState* T)
{
    register TeaCallInfo* frame;
    register TeaChunk* current_chunk;

    register uint8_t* ip;
    register TeaValue* slots;
    register TeaObjectUpvalue** upvalues;

#define PUSH(value) (teaV_push(T, value))
#define POP() (teaV_pop(T, 1))
#define PEEK(distance) (teaV_peek(T, distance))
#define DROP(amount) (teaV_pop(T, amount))
#define READ_BYTE() (*ip++)
#define READ_SHORT() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

#define STORE_FRAME (frame->ip = ip)
#define READ_FRAME() \
    do \
    { \
        frame = T->ci - 1; \
        current_chunk = &frame->closure->function->chunk; \
	    ip = frame->ip; \
	    slots = frame->slots; \
	    upvalues = frame->closure == NULL ? NULL : frame->closure->upvalues; \
    } \
    while(false) \

#define READ_CONSTANT() (current_chunk->constants.values[READ_BYTE()])
#define READ_STRING() AS_STRING(READ_CONSTANT())

#define RUNTIME_ERROR(...) \
    do \
    { \
        STORE_FRAME; \
        teaV_runtime_error(T, __VA_ARGS__); \
        READ_FRAME(); \
        DISPATCH(); \
    } \
    while(false)

#define INVOKE_METHOD(a, b, name, arg_count) \
    do \
    { \
        TeaObjectString* method_name = teaO_copy_string(T, name, strlen(name)); \
        TeaValue method; \
        if(((IS_INSTANCE(a) && IS_INSTANCE(b)) || IS_INSTANCE(a)) && teaT_get(&AS_INSTANCE(a)->klass->methods, method_name, &method)) \
        { \
            STORE_FRAME; \
            teaD_call_value(T, method, arg_count); \
            READ_FRAME(); \
            DISPATCH(); \
        } \
        else if(IS_INSTANCE(b) && teaT_get(&AS_INSTANCE(b)->klass->methods, method_name, &method)) \
        { \
            STORE_FRAME; \
            teaD_call_value(T, method, arg_count); \
            READ_FRAME(); \
            DISPATCH(); \
        } \
        else \
        { \
            RUNTIME_ERROR("Undefined '%s' overload", name); \
        } \
    } \
    while(false);

#define BINARY_OP(value_type, op, op_string, type) \
    do \
    { \
        if(IS_NUMBER(PEEK(0)) && IS_NUMBER(PEEK(1))) \
        { \
            type b = AS_NUMBER(POP()); \
            type a = AS_NUMBER(PEEK(0)); \
            T->top[-1] = value_type(a op b); \
        } \
        else if(IS_INSTANCE(PEEK(1)) || IS_INSTANCE(PEEK(0))) \
        { \
            TeaValue a = PEEK(1); \
            TeaValue b = PEEK(0); \
            DROP(1); \
            PUSH(a); \
            PUSH(b); \
            INVOKE_METHOD(a, b, op_string, 2); \
        } \
        else \
        { \
            RUNTIME_ERROR("Attempt to use %s operator with %s and %s", op_string, teaL_type(PEEK(1)), teaL_type(PEEK(0))); \
        } \
    } \
    while(false)

#ifdef TEA_DEBUG_TRACE_EXECUTION
    #define TRACE_INSTRUCTIONS() \
        do \
        { \
            teaG_dump_stack(T); \
            teaG_dump_instruction(T, current_chunk, (int)(ip - current_chunk->code)); \
        } \
        while(false)
#else
    #define TRACE_INSTRUCTIONS() do { } while(false)
#endif

#ifdef TEA_COMPUTED_GOTO
    static void* dispatch_table[] = {
        #define OPCODE(name, _) &&OP_##name
        #include "tea_opcodes.h"
        #undef OPCODE
    };

    #define DISPATCH() \
        do \
        { \
            TRACE_INSTRUCTIONS(); \
            goto *dispatch_table[instruction = READ_BYTE()]; \
        } \
        while(false)

    #define INTREPRET_LOOP  DISPATCH();
    #define CASE_CODE(name) OP_##name
#else
    #define INTREPRET_LOOP \
        loop: \
            switch(instruction = READ_BYTE())

    #define DISPATCH() goto loop

    #define CASE_CODE(name) case OP_##name
#endif

    READ_FRAME();
    
    while(true)
    {
        uint8_t instruction;
        INTREPRET_LOOP
        {
            CASE_CODE(CONSTANT):
            {
                PUSH(READ_CONSTANT());
                DISPATCH();
            }
            CASE_CODE(NULL):
            {
                PUSH(NULL_VAL);
                DISPATCH();
            }
            CASE_CODE(TRUE):
            {
                PUSH(TRUE_VAL);
                DISPATCH();
            }
            CASE_CODE(FALSE):
            {
                PUSH(FALSE_VAL);
                DISPATCH();
            }
            CASE_CODE(DUP):
            {
                PUSH(PEEK(0));
                DISPATCH();
            }
            CASE_CODE(POP):
            {
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(POP_REPL):
            {
                TeaValue value = PEEK(0);
                if(!IS_NULL(value))
                {
                    teaT_set(T, &T->globals, T->repl_string, value);
                    TeaObjectString* string = teaL_tostring(T, value);
                    PUSH(OBJECT_VAL(string));
                    tea_write_string(string->chars, string->length);
                    tea_write_line();
                    DROP(1);
                }
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(GET_LOCAL):
            {
                PUSH(slots[READ_BYTE()]);
                DISPATCH();
            }
            CASE_CODE(SET_LOCAL):
            {
                uint8_t slot = READ_BYTE();
                slots[slot] = PEEK(0);
                DISPATCH();
            }
            CASE_CODE(GET_GLOBAL):
            {
                TeaObjectString* name = READ_STRING();
                TeaValue value;
                if(!teaT_get(&T->globals, name, &value))
                {
                    RUNTIME_ERROR("Undefined variable '%s'", name->chars);
                }
                PUSH(value);
                DISPATCH();
            }
            CASE_CODE(SET_GLOBAL):
            {
                TeaObjectString* name = READ_STRING();
                if(teaT_set(T, &T->globals, name, PEEK(0)))
                {
                    teaT_delete(&T->globals, name);
                    RUNTIME_ERROR("Undefined variable '%s'", name->chars);
                }
                DISPATCH();
            }
            CASE_CODE(GET_MODULE):
            {
                TeaObjectString* name = READ_STRING();
                TeaValue value;
                if(!teaT_get(&frame->closure->function->module->values, name, &value))
                {
                    RUNTIME_ERROR("Undefined variable '%s'", name->chars);
                }
                PUSH(value);
                DISPATCH();
            }
            CASE_CODE(SET_MODULE):
            {
                TeaObjectString* name = READ_STRING();
                if(teaT_set(T, &frame->closure->function->module->values, name, PEEK(0)))
                {
                    teaT_delete(&frame->closure->function->module->values, name);
                    RUNTIME_ERROR("Undefined variable '%s'", name->chars);
                }
                DISPATCH();
            }
            CASE_CODE(DEFINE_OPTIONAL):
            {
                int arity = READ_BYTE();
                int arity_optional = READ_BYTE();
                int arg_count = T->top - slots - arity_optional - 1;

                // Temp array while we shuffle the stack
                // Cannot have more than 255 args to a function, so
                // we can define this with a constant limit
                TeaValue values[255];
                int index;

                for(index = 0; index < arity_optional + arg_count; index++)
                {
                    values[index] = POP();
                }

                --index;

                for(int i = 0; i < arg_count; i++)
                {
                    PUSH(values[index - i]);
                }

                // Calculate how many "default" values are required
                int remaining = arity + arity_optional - arg_count;

                // Push any "default" values back onto the stack
                for(int i = remaining; i > 0; i--)
                {
                    PUSH(values[i - 1]);
                }
                DISPATCH();
            }
            CASE_CODE(DEFINE_GLOBAL):
            {
                TeaObjectString* name = READ_STRING();
                teaT_set(T, &T->globals, name, PEEK(0));
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(DEFINE_MODULE):
            {
                TeaObjectString* name = READ_STRING();
                teaT_set(T, &frame->closure->function->module->values, name, PEEK(0));
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(GET_UPVALUE):
            {
                uint8_t slot = READ_BYTE();
                PUSH(*upvalues[slot]->location);
                DISPATCH();
            }
            CASE_CODE(SET_UPVALUE):
            {
                uint8_t slot = READ_BYTE();
                *upvalues[slot]->location = PEEK(0);
                DISPATCH();
            }
            CASE_CODE(GET_PROPERTY):
            {
                TeaValue receiver = PEEK(0);
                TeaObjectString* name = READ_STRING();
                STORE_FRAME;
                get_property(T, receiver, name, true);
                DISPATCH();
            }
            CASE_CODE(GET_PROPERTY_NO_POP):
            {
                TeaValue receiver = PEEK(0);
                TeaObjectString* name = READ_STRING();
                STORE_FRAME;
                get_property(T, receiver, name, false);
                DISPATCH();
            }
            CASE_CODE(SET_PROPERTY):
            {
                TeaObjectString* name = READ_STRING();
                TeaValue receiver = PEEK(1);
                TeaValue item = PEEK(0);
                STORE_FRAME;
                set_property(T, name, receiver, item);
                DISPATCH();
            }
            CASE_CODE(GET_SUPER):
            {
                TeaObjectString* name = READ_STRING();
                TeaObjectClass* superclass = AS_CLASS(POP());
                STORE_FRAME;
                bind_method(T, superclass, name);
                DISPATCH();
            }
            CASE_CODE(RANGE):
            {
                TeaValue c = POP();
                TeaValue b = POP();
                TeaValue a = POP();

                if(!IS_NUMBER(a) || !IS_NUMBER(b) || !IS_NUMBER(c)) 
                {
                    RUNTIME_ERROR("Range operands must be numbers");
                }

                PUSH(OBJECT_VAL(teaO_new_range(T, AS_NUMBER(a), AS_NUMBER(b), AS_NUMBER(c))));
                DISPATCH();
            }
            CASE_CODE(LIST):
            {
                // Stack before: [item1, item2, ..., itemN] and after: [list]
                uint8_t item_count = READ_BYTE();
                TeaObjectList* list = teaO_new_list(T);

                PUSH(OBJECT_VAL(list)); // So list isn't sweeped by GC when appending the list
                // Add items to list
                for(int i = item_count; i > 0; i--)
                {
                    if(IS_RANGE(PEEK(i)))
                    {
                        TeaObjectRange* range = AS_RANGE(PEEK(i));

                        int start = range->start;
                        int end = range->end;
                        int step = range->step;

                        if(step > 0)
                        {
                            for(int i = start; i < end; i += step)
                            {
                                tea_write_value_array(T, &list->items, NUMBER_VAL(i));
                            }
                        }
                        else if(step < 0)
                        {
                            for(int i = end + step; i >= 0; i += step)
                            {
                                tea_write_value_array(T, &list->items, NUMBER_VAL(i));
                            }
                        }
                    }
                    else
                    {
                        tea_write_value_array(T, &list->items, PEEK(i));
                    }
                }
                
                // Pop items from stack
                T->top -= item_count + 1;

                PUSH(OBJECT_VAL(list));
                DISPATCH();
            }
            CASE_CODE(UNPACK_LIST):
            {
                uint8_t var_count = READ_BYTE();

                if(!IS_LIST(PEEK(0)))
                {
                    RUNTIME_ERROR("Can only unpack lists");
                }

                TeaObjectList* list = AS_LIST(POP());

                if(var_count != list->items.count) 
                {
                    if(var_count < list->items.count)
                    {
                        RUNTIME_ERROR("Too many values to unpack");
                    } 
                    else
                    {
                        RUNTIME_ERROR("Not enough values to unpack");
                    }
                }

                for(int i = 0; i < list->items.count; i++)
                {
                    PUSH(list->items.values[i]);
                }

                DISPATCH();
            }
            CASE_CODE(UNPACK_REST_LIST):
            {
                uint8_t var_count = READ_BYTE();
                uint8_t rest_pos = READ_BYTE();

                if(!IS_LIST(PEEK(0)))
                {
                    RUNTIME_ERROR("Can only unpack lists");
                }

                TeaObjectList* list = AS_LIST(POP());

                if(var_count > list->items.count)
                {
                    RUNTIME_ERROR("Not enough values to unpack");
                }

                for(int i = 0; i < list->items.count; i++)
                {
                    if(i == rest_pos)
                    {
                        TeaObjectList* rest_list = teaO_new_list(T);
                        PUSH(OBJECT_VAL(rest_list));
                        int j;
                        for(j = i; j < list->items.count - (var_count - rest_pos) + 1; j++)
                        {
                            tea_write_value_array(T, &rest_list->items, list->items.values[j]);
                        }
                        i = j - 1;
                    }
                    else
                    {
                        PUSH(list->items.values[i]);
                    }
                }

                DISPATCH();
            }
            CASE_CODE(ENUM):
            {
                uint8_t item_count = READ_BYTE();
                TeaObjectMap* enum_ = teaO_new_map(T);

                PUSH(OBJECT_VAL(enum_));

                double counter = 0;

                for(int i = item_count * 2; i > 0; i -= 2)
                {
                    TeaValue name = PEEK(i);
                    TeaValue value = PEEK(i - 1);

                    if(IS_NULL(value))
                    {
                        value = NUMBER_VAL(counter);
                    }
                    else if(IS_NUMBER(value))
                    {
                        double num = AS_NUMBER(value);
                        counter = num;
                    }

                    teaO_map_set(T, enum_, name, value);

                    counter++;
                }

                T->top -= item_count * 2 + 1;

                PUSH(OBJECT_VAL(enum_));
                DISPATCH();
            }
            CASE_CODE(MAP):
            {
                uint8_t item_count = READ_BYTE();
                TeaObjectMap* map = teaO_new_map(T);

                PUSH(OBJECT_VAL(map));

                for(int i = item_count * 2; i > 0; i -= 2)
                {
                    if(!teaO_is_valid_key(PEEK(i)))
                    {
                        RUNTIME_ERROR("Map key isn't hashable");
                    }

                    teaO_map_set(T, map, PEEK(i), PEEK(i - 1));
                }

                T->top -= item_count * 2 + 1;

                PUSH(OBJECT_VAL(map));
                DISPATCH();
            }
            CASE_CODE(SUBSCRIPT):
            {
                // Stack before: [list, index] and after: [index(list, index)]
                TeaValue index = PEEK(0);
                TeaValue list = PEEK(1);
                if(IS_INSTANCE(list))
                {
                    DROP(1);
                    PUSH(index);
                    PUSH(NULL_VAL);             
                    INVOKE_METHOD(list, NULL_VAL, "[]", 2);
                    DISPATCH();
                }
                STORE_FRAME;
                subscript(T, index, list);
                DISPATCH();
            }
            CASE_CODE(SUBSCRIPT_STORE):
            {
                // Stack before list: [list, index, item] and after: [item]
                TeaValue item = PEEK(0);
                TeaValue index = PEEK(1);
                TeaValue list = PEEK(2);
                if(IS_INSTANCE(list))
                {
                    DROP(2);
                    PUSH(index);
                    PUSH(item);
                    INVOKE_METHOD(list, NULL_VAL, "[]", 2);
                    DISPATCH();
                }
                STORE_FRAME;
                subscript_store(T, item, index, list, true);
                DISPATCH();
            }
            CASE_CODE(SUBSCRIPT_PUSH):
            {
                // Stack before list: [list, index, item] and after: [item]
                TeaValue item = PEEK(0);
                TeaValue index = PEEK(1);
                TeaValue list = PEEK(2);
                STORE_FRAME;
                subscript_store(T, item, index, list, false);
                DISPATCH();
            }
            CASE_CODE(IS):
            {
                TeaValue instance = PEEK(1);
                TeaValue klass = PEEK(0);

                if(!IS_CLASS(klass))
                {
                    RUNTIME_ERROR("Right operand must be a class");
                }

                if(!IS_INSTANCE(instance))
                {
                    DROP(2); // Drop the instance and class
                    PUSH(FALSE_VAL);
                    DISPATCH();
                }

                TeaObjectClass* instance_klass = AS_INSTANCE(instance)->klass;
                TeaObjectClass* type = AS_CLASS(klass);
                bool found = false;

                while(instance_klass != NULL)
                {
                    if(instance_klass == type)
                    {
                        found = true;
                        break;
                    }

                    instance_klass = (TeaObjectClass*)instance_klass->super;
                }
                
                DROP(2); // Drop the instance and class
                PUSH(BOOL_VAL(found));

                DISPATCH();
            }
            CASE_CODE(IN):
            {
                TeaValue object = PEEK(0);
                TeaValue value = PEEK(1);
                STORE_FRAME;
                in_(T, object, value);
                DISPATCH();
            }
            CASE_CODE(EQUAL):
            {
                if(IS_INSTANCE(PEEK(1)) || IS_INSTANCE(PEEK(0)))
                {
                    TeaValue a = PEEK(1);
                    TeaValue b = PEEK(0);
                    DROP(1);
                    PUSH(a);
                    PUSH(b);
                    INVOKE_METHOD(a, b, "==", 2);
                    DISPATCH();
                }
                
                TeaValue b = POP();
                TeaValue a = POP();
                PUSH(BOOL_VAL(teaL_equal(a, b)));
                DISPATCH();
            }
            CASE_CODE(GREATER):
            {
                BINARY_OP(BOOL_VAL, >, ">", double);
                DISPATCH();
            }
            CASE_CODE(GREATER_EQUAL):
            {
                BINARY_OP(BOOL_VAL, >=, ">=", double);
                DISPATCH();
            }
            CASE_CODE(LESS):
            {
                BINARY_OP(BOOL_VAL, <, "<", double);
                DISPATCH();
            }
            CASE_CODE(LESS_EQUAL):
            {
                BINARY_OP(BOOL_VAL, <=, "<=", double);
                DISPATCH();
            }
            CASE_CODE(ADD):
            {
                if(IS_STRING(PEEK(0)) && IS_STRING(PEEK(1)))
                {
                    concatenate(T);
                }
                else if(IS_LIST(PEEK(0)) && IS_LIST(PEEK(1)))
                {
                    TeaObjectList* l2 = AS_LIST(PEEK(0));
                    TeaObjectList* l1 = AS_LIST(PEEK(1));

                    for(int i = 0; i < l2->items.count; i++)
                    {
                        tea_write_value_array(T, &l1->items, l2->items.values[i]);
                    }

                    DROP(2);

                    PUSH(OBJECT_VAL(l1));
                }
                else if(IS_MAP(PEEK(0)) && IS_MAP(PEEK(1)))
                {
                    TeaObjectMap* m2 = AS_MAP(PEEK(0));
                    TeaObjectMap* m1 = AS_MAP(PEEK(1));

                    teaO_map_add_all(T, m2, m1);

                    DROP(2);

                    PUSH(OBJECT_VAL(m1));
                }
                else
                {
                    BINARY_OP(NUMBER_VAL, +, "+", double);
                }
                DISPATCH();
            }
            CASE_CODE(SUBTRACT):
            {
                BINARY_OP(NUMBER_VAL, -, "-", double);
                DISPATCH();
            }
            CASE_CODE(MULTIPLY):
            {
                if((IS_STRING(PEEK(0)) && IS_NUMBER(PEEK(1))) || (IS_NUMBER(PEEK(0)) && IS_STRING(PEEK(1))))
                {
                    repeat(T);
                }
                else
                {
                    BINARY_OP(NUMBER_VAL, *, "*", double);
                }
                DISPATCH();
            }
            CASE_CODE(DIVIDE):
            {
                BINARY_OP(NUMBER_VAL, /, "/", double);
                DISPATCH();
            }
            CASE_CODE(MOD):
            {
                TeaValue a = PEEK(1);
                TeaValue b = PEEK(0);

                if(IS_NUMBER(a) && IS_NUMBER(b))
                {
                    DROP(1);
                    T->top[-1] = (NUMBER_VAL(fmod(AS_NUMBER(a), AS_NUMBER(b))));
                    DISPATCH();
                }

                INVOKE_METHOD(a, b, "%", 1);
                DISPATCH();
            }
            CASE_CODE(POW):
            {
                TeaValue a = PEEK(1);
                TeaValue b = PEEK(0);

                if(IS_NUMBER(a) && IS_NUMBER(b))
                {
                    DROP(1);
                    T->top[-1] = (NUMBER_VAL(pow(AS_NUMBER(a), AS_NUMBER(b))));
                    DISPATCH();
                }

                INVOKE_METHOD(a, b, "**", 1);
                DISPATCH();
            }
            CASE_CODE(BAND):
            {
                BINARY_OP(NUMBER_VAL, &, "&", int);
                DISPATCH();
            }
            CASE_CODE(BOR):
            {
                BINARY_OP(NUMBER_VAL, |, "|", int);
                DISPATCH();
            }
            CASE_CODE(BNOT):
            {
                if(!IS_NUMBER(PEEK(0)))
                {
                    RUNTIME_ERROR("Operand must be a number");
                }
                PUSH(NUMBER_VAL(~((int)AS_NUMBER(POP()))));
                DISPATCH();
            }
            CASE_CODE(BXOR):
            {
                BINARY_OP(NUMBER_VAL, ^, "^", int);
                DISPATCH();
            }
            CASE_CODE(LSHIFT):
            {
                BINARY_OP(NUMBER_VAL, <<, "<<", int);
                DISPATCH();
            }
            CASE_CODE(RSHIFT):
            {
                BINARY_OP(NUMBER_VAL, >>, ">>", int);
                DISPATCH();
            }
            CASE_CODE(AND):
            {
                uint16_t offset = READ_SHORT();
                
                if(teaO_is_falsey(PEEK(0)))
                {
                    ip += offset;
                }
                else
                {
                    DROP(1);
                }

                DISPATCH();
            }
            CASE_CODE(OR):
            {
                uint16_t offset = READ_SHORT();
                
                if(teaO_is_falsey(PEEK(0)))
                {
                    DROP(1);
                }
                else
                {
                    ip += offset;
                }

                DISPATCH();
            }
            CASE_CODE(NOT):
            {
                PUSH(BOOL_VAL(teaO_is_falsey(POP())));
                DISPATCH();
            }
            CASE_CODE(NEGATE):
            {
                if(IS_INSTANCE(PEEK(0)))
                {
                    TeaValue a = PEEK(0);
                    PUSH(a);
                    PUSH(NULL_VAL);
                    INVOKE_METHOD(a, NULL_VAL, "-", 2);
                    DISPATCH();
                }

                if(!IS_NUMBER(PEEK(0)))
                {
                    RUNTIME_ERROR("Operand must be a number");
                }
                PUSH(NUMBER_VAL(-AS_NUMBER(POP())));
                DISPATCH();
            }
            CASE_CODE(MULTI_CASE):
            {
                int count = READ_BYTE();
                TeaValue switch_value = PEEK(count + 1);
                TeaValue case_value = POP();
                for(int i = 0; i < count; i++)
                {
                    if(teaL_equal(switch_value, case_value))
                    {
                        i++;
                        while(i <= count)
                        {
                            DROP(1);
                            i++;   
                        }
                        break;
                    }
                    case_value = POP();
                }
                PUSH(case_value);
                DISPATCH();
            }
            CASE_CODE(COMPARE_JUMP):
            {
                uint16_t offset = READ_SHORT();
                TeaValue a = POP();
                if(!teaL_equal(PEEK(0), a))
                {
                    ip += offset;
                }
                else
                {
                    DROP(1);
                }
                DISPATCH();
            }
            CASE_CODE(JUMP):
            {
                uint16_t offset = READ_SHORT();
                ip += offset;
                DISPATCH();
            }
            CASE_CODE(JUMP_IF_FALSE):
            {
                uint16_t offset = READ_SHORT();
                if(teaO_is_falsey(PEEK(0)))
                {
                    ip += offset;
                }
                DISPATCH();
            }
            CASE_CODE(JUMP_IF_NULL):
            {
                uint16_t offset = READ_SHORT();
                if(IS_NULL(PEEK(0)))
                {
                    ip += offset;
                }
                DISPATCH();
            }
            CASE_CODE(LOOP):
            {
                uint16_t offset = READ_SHORT();
                ip -= offset;
                DISPATCH();
            }
            CASE_CODE(CALL):
            {
                int arg_count = READ_BYTE();
                STORE_FRAME;
                teaD_call_value(T, PEEK(arg_count), arg_count);
                READ_FRAME();
                DISPATCH();
            }
            CASE_CODE(INVOKE):
            {
                TeaObjectString* method = READ_STRING();
                int arg_count = READ_BYTE();
                STORE_FRAME;
                invoke(T, PEEK(arg_count), method, arg_count);
                READ_FRAME();
                DISPATCH();
            }
            CASE_CODE(SUPER):
            {
                TeaObjectString* method = READ_STRING();
                int arg_count = READ_BYTE();
                TeaObjectClass* superclass = AS_CLASS(POP());
                STORE_FRAME;
                invoke_from_class(T, superclass, method, arg_count);
                READ_FRAME();
                DISPATCH();
            }
            CASE_CODE(CLOSURE):
            {
                TeaObjectFunction* function = AS_FUNCTION(READ_CONSTANT());
                TeaObjectClosure* closure = teaO_new_closure(T, function);
                PUSH(OBJECT_VAL(closure));
                
                for(int i = 0; i < closure->upvalue_count; i++)
                {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if(is_local)
                    {
                        closure->upvalues[i] = capture_upvalue(T, frame->slots + index);
                    }
                    else
                    {
                        closure->upvalues[i] = upvalues[index];
                    }
                }
                DISPATCH();
            }
            CASE_CODE(CLOSE_UPVALUE):
            {
                close_upvalues(T, T->top - 1);
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(RETURN):
            {
                TeaValue result = POP();
                close_upvalues(T, slots);
                STORE_FRAME;
                T->ci--;
                if(T->ci == T->base_ci)
                {
                    DROP(1);
                    //PUSH(result);
                    return;
                }

                TeaCallInfo* cframe = T->ci - 1;
                if(cframe->closure == NULL)
                {
                    PUSH(result);
                    //printf("OP_RETURN : %s\n", teaL_tostring(T, T->top[-1])->chars);
                    return;
                }
                T->top = slots;
                PUSH(result);
                READ_FRAME();
                DISPATCH();
            }
            CASE_CODE(CLASS):
            {
                PUSH(OBJECT_VAL(teaO_new_class(T, READ_STRING(), NULL)));
                DISPATCH();
            }
            CASE_CODE(SET_CLASS_VAR):
            {
                TeaObjectClass* klass = AS_CLASS(PEEK(1));
                TeaObjectString* key = READ_STRING();

                teaT_set(T, &klass->statics, key, PEEK(0));
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(INHERIT):
            {
                TeaValue super = PEEK(1);

                if(!IS_CLASS(super))
                {
                    RUNTIME_ERROR("Superclass must be a class");
                }

                TeaObjectClass* superclass = AS_CLASS(super);
                TeaObjectClass* klass = AS_CLASS(PEEK(0));
                if(klass == superclass)
                {
                    RUNTIME_ERROR("A class can't inherit from itself");
                }
                klass->super = superclass;
                
                teaT_add_all(T, &superclass->methods, &klass->methods);
                teaT_add_all(T, &superclass->statics, &klass->statics);
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(METHOD):
            {
                define_method(T, READ_STRING());
                DISPATCH();
            }
            CASE_CODE(EXTENSION_METHOD):
            {
                if(!IS_CLASS(PEEK(1)))
                {
                    RUNTIME_ERROR("Cannot assign extension method to %s", teaL_type(PEEK(1)));
                }
                define_method(T, READ_STRING());
                DROP(1);
                DISPATCH();
            }
            CASE_CODE(IMPORT):
            {
                TeaObjectString* file_name = READ_STRING();
                TeaValue module_value;

                // If we have imported this file already, skip
                if(teaT_get(&T->modules, file_name, &module_value)) 
                {
                    T->last_module = AS_MODULE(module_value);
                    PUSH(NULL_VAL);
                    DISPATCH();
                }

                char path[PATH_MAX];
                if(!teaZ_resolve_path(frame->closure->function->module->path->chars, file_name->chars, path))
                {
                    RUNTIME_ERROR("Could not open file \"%s\"", file_name->chars);
                }

                char* source = teaZ_read_file(T, path);

                if(source == NULL) 
                {
                    RUNTIME_ERROR("Could not open file \"%s\"", file_name->chars);
                }

                TeaObjectString* path_obj = teaO_copy_string(T, path, strlen(path));
                TeaObjectModule* module = teaO_new_module(T, path_obj);
                module->path = teaZ_dirname(T, path, strlen(path));
                T->last_module = module;

                //TeaObjectFunction* function = teaY_compile(T, module, source);
                int status = teaD_protected_compiler(T, module, source);
                TEA_FREE_ARRAY(T, char, source, strlen(source) + 1);

                if(status != 0)
                    teaD_throw(T, TEA_COMPILE_ERROR);

                //if(function == NULL) return TEA_COMPILE_ERROR;
                //TeaObjectClosure* closure = teaO_new_closure(T, function);
                //PUSH(OBJECT_VAL(closure));

                STORE_FRAME;
                teaD_call_value(T, T->top[-1], 0);
                READ_FRAME();

                DISPATCH();
            }
            CASE_CODE(IMPORT_VARIABLE):
            {
                PUSH(OBJECT_VAL(T->last_module));
                DISPATCH();
            }
            CASE_CODE(IMPORT_FROM):
            {
                int var_count = READ_BYTE();

                for(int i = 0; i < var_count; i++) 
                {
                    TeaValue module_variable;
                    TeaObjectString* variable = READ_STRING();

                    if(!teaT_get(&T->last_module->values, variable, &module_variable)) 
                    {
                        RUNTIME_ERROR("%s can't be found in module %s", variable->chars, T->last_module->name->chars);
                    }

                    PUSH(module_variable);
                }

                DISPATCH();
            }
            CASE_CODE(IMPORT_END):
            {
                T->last_module = frame->closure->function->module;
                DISPATCH();
            }
            CASE_CODE(IMPORT_NATIVE):
            {
                int index = READ_BYTE();
                TeaObjectString* file_name = READ_STRING();

                TeaValue module_val;
                // If the module is already imported, skip
                if(teaT_get(&T->modules, file_name, &module_val))
                {
                    T->last_module = AS_MODULE(module_val);
                    PUSH(module_val);
                    DISPATCH();
                }

                teaI_import_native_module(T, index);
                TeaValue module = T->top[-1];
                printf("::: MOD %s\n", teaL_type(module));
                
                if(IS_CLOSURE(module)) 
                {
                    STORE_FRAME;
                    teaD_call_value(T, module, 0);
                    READ_FRAME();

                    teaT_get(&T->modules, file_name, &module);
                    T->last_module = AS_MODULE(module);
                }

                DISPATCH();
            }
            CASE_CODE(IMPORT_NATIVE_VARIABLE):
            {
                TeaObjectString* file_name = READ_STRING();
                int var_count = READ_BYTE();

                TeaObjectModule* module;

                TeaValue module_val;
                if(teaT_get(&T->modules, file_name, &module_val)) 
                {
                    module = AS_MODULE(module_val);
                } 

                for(int i = 0; i < var_count; i++) 
                {
                    TeaObjectString* variable = READ_STRING();

                    TeaValue module_variable;
                    if(!teaT_get(&module->values, variable, &module_variable)) 
                    {
                        RUNTIME_ERROR("%s can't be found in module %s", variable->chars, module->name->chars);
                    }

                    PUSH(module_variable);
                }

                DISPATCH();
            }
            CASE_CODE(END):
            {
                DISPATCH();
            }
        }
    }
}
#undef PUSH
#undef POP
#undef PEEK
#undef DROP
#undef STORE_FRAME
#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
#undef READ_STRING
#undef BINARY_OP
#undef BINARY_OP_FUNCTION
#undef RUNTIME_ERROR

TeaInterpretResult teaV_interpret_module(TeaState* T, const char* module_name, const char* source)
{
    TeaObjectString* name = teaO_new_string(T, module_name);
    teaV_push(T, OBJECT_VAL(name));
    TeaObjectModule* module = teaO_new_module(T, name);
    teaV_pop(T, 1);

    teaV_push(T, OBJECT_VAL(module));
    module->path = teaZ_get_directory(T, (char*)module_name);
    teaV_pop(T, 1);
    
    /*TeaObjectFunction* function = teaY_compile(T, module, source);
    if(function == NULL)
        return TEA_COMPILE_ERROR;

    teaV_push(T, OBJECT_VAL(function));
    TeaObjectClosure* closure = teaO_new_closure(T, function);
    teaV_pop(T, 1);

    teaV_push(T, OBJECT_VAL(closure));*/
    int status = teaD_protected_compiler(T, module, source);
    if(status != 0)
        return TEA_COMPILE_ERROR;

    teaD_call(T, T->top[-1], 0);
}