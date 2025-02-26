/*
** tea_object.c
** Teascript object model and functions
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define tea_object_c
#define TEA_CORE

#include "tea_memory.h"
#include "tea_object.h"
#include "tea_map.h"
#include "tea_string.h"
#include "tea_table.h"
#include "tea_value.h"
#include "tea_state.h"
#include "tea_vm.h"

TeaObject* tea_obj_allocate(TeaState* T, size_t size, TeaObjectType type)
{
    TeaObject* object = (TeaObject*)tea_mem_realloc(T, NULL, 0, size);
    object->type = type;
    object->is_marked = false;

    object->next = T->objects;
    T->objects = object;

#ifdef TEA_DEBUG_LOG_GC
    printf("%p allocate %zu for %s\n", (void*)object, size, tea_value_type(OBJECT_VAL(object)));
#endif

    return object;
}

TeaObjectBoundMethod* tea_obj_new_bound_method(TeaState* T, TeaValue receiver, TeaValue method)
{
    TeaObjectBoundMethod* bound = ALLOCATE_OBJECT(T, TeaObjectBoundMethod, OBJ_BOUND_METHOD);
    bound->receiver = receiver;
    bound->method = method;

    return bound;
}

TeaObjectInstance* tea_obj_new_instance(TeaState* T, TeaObjectClass* klass)
{
    TeaObjectInstance* instance = ALLOCATE_OBJECT(T, TeaObjectInstance, OBJ_INSTANCE);
    instance->klass = klass;
    tea_table_init(&instance->fields);

    return instance;
}

TeaObjectClass* tea_obj_new_class(TeaState* T, TeaObjectString* name, TeaObjectClass* superclass)
{
    TeaObjectClass* klass = ALLOCATE_OBJECT(T, TeaObjectClass, OBJ_CLASS);
    klass->name = name;
    klass->super = superclass;
    klass->constructor = NULL_VAL;
    tea_table_init(&klass->statics);
    tea_table_init(&klass->methods);

    return klass;
}

TeaObjectUserdata* tea_obj_new_userdata(TeaState* T, size_t size)
{
    TeaObjectUserdata* ud = ALLOCATE_OBJECT(T, TeaObjectUserdata, OBJ_USERDATA);
    
    if(size > 0)
    {
        ud->data = tea_mem_realloc(T, NULL, 0, size);
    }
    else
    {
        ud->data = NULL;
    }
    
    ud->size = size;

    return ud;
}

TeaObjectList* tea_obj_new_list(TeaState* T)
{
    TeaObjectList* list = ALLOCATE_OBJECT(T, TeaObjectList, OBJ_LIST);
    tea_init_value_array(&list->items);

    return list;
}

TeaObjectModule* tea_obj_new_module(TeaState* T, TeaObjectString* name)
{
    TeaValue module_val;
    if(tea_table_get(&T->modules, name, &module_val)) 
    {
        return AS_MODULE(module_val);
    }

    TeaObjectModule* module = ALLOCATE_OBJECT(T, TeaObjectModule, OBJ_MODULE);
    tea_table_init(&module->values);
    module->name = name;
    module->path = NULL;
    
    tea_vm_push(T, OBJECT_VAL(module));
    tea_table_set(T, &T->modules, name, OBJECT_VAL(module));
    tea_vm_pop(T, 1);

    return module;
}

TeaObjectFile* tea_obj_new_file(TeaState* T, TeaObjectString* path, TeaObjectString* type)
{
    TeaObjectFile* file = ALLOCATE_OBJECT(T, TeaObjectFile, OBJ_FILE);
    file->path = path;
    file->type = type;
    file->is_open = true;

    return file;
}

TeaObjectRange* tea_obj_new_range(TeaState* T, double start, double end, double step)
{
    TeaObjectRange* range = ALLOCATE_OBJECT(T, TeaObjectRange, OBJ_RANGE);
    range->start = start;
    range->end = end;
    range->step = step;

    return range;
}

static TeaObjectString* function_tostring(TeaState* T, TeaObjectFunction* function)
{
    if(function->name == NULL)
        return tea_string_literal(T, "<script>");

    return tea_string_literal(T, "<function>");
}

static TeaObjectString* list_tostring(TeaState* T, TeaObjectList* list)
{
    if(list->items.count == 0)
        return tea_string_literal(T, "[]");

    int size = 50;
    
    char* string = TEA_ALLOCATE(T, char, size);
    memcpy(string, "[", 1);
    int length = 1;

    for(int i = 0; i < list->items.count; i++) 
    {
        TeaValue value = list->items.values[i];

        char* element;
        int element_size;

#ifdef TEA_NAN_TAGGING
        if(value == OBJECT_VAL(list))
#else
        if(tea_value_equal(value, OBJECT_VAL(list)))
#endif
        {
            element = "[...]";
            element_size = 5;
        }
        else
        {
            TeaObjectString* s = tea_value_tostring(T, value);
            element = s->chars;
            element_size = s->length;
        }

        if(element_size > (size - length - 6)) 
        {
            int old_size = size;
            if(element_size > size) 
            {
                size = size + element_size * 2 + 6;
            } 
            else
            {
                size = size * 2 + 6;
            }

            string = TEA_GROW_ARRAY(T, char, string, old_size, size);
        }

        memcpy(string + length, element, element_size);
        length += element_size;

        if(i != list->items.count - 1) 
        {
            memcpy(string + length, ", ", 2);
            length += 2;
        }
    }

    memcpy(string + length, "]", 1);
    length += 1;
    string[length] = '\0';

    string = TEA_GROW_ARRAY(T, char, string, size, length + 1);

    return tea_string_take(T, string, length);
}

static TeaObjectString* map_tostring(TeaState* T, TeaObjectMap* map)
{
    if(map->count == 0)
        return tea_string_literal(T, "{}");
        
    int count = 0;
    int size = 50;

    char* string = TEA_ALLOCATE(T, char, size);
    memcpy(string, "{", 1);
    int length = 1;

    for(int i = 0; i < map->capacity; i++) 
    {
        TeaMapItem* item = &map->items[i];
        if(item->empty) 
        {
            continue;
        }

        count++;

        char* key;
        int key_size;
        
#ifdef TEA_NAN_TAGGING
        if(item->key == OBJECT_VAL(map))
#else
        if(tea_value_equal(item->key, OBJECT_VAL(map)))
#endif
        {
            key = "{...}";
            key_size = 5;
        }
        else
        {
            TeaObjectString* s = tea_value_tostring(T, item->key);
            key = s->chars;
            key_size = s->length;
        } 

        if(key_size > (size - length - key_size - 4)) 
        {
            int old_size = size;
            if(key_size > size) 
            {
                size += key_size * 2 + 4;
            } 
            else 
            {
                size *= 2 + 4;
            }

            string = TEA_GROW_ARRAY(T, char, string, old_size, size);
        }

        if(!IS_STRING(item->key))
        {
            memcpy(string + length, "[", 1);
            memcpy(string + length + 1, key, key_size);
            memcpy(string + length + 1 + key_size, "] = ", 4);
            length += 5 + key_size;
        }
        else 
        {
            memcpy(string + length, key, key_size);
            memcpy(string + length + key_size, " = ", 3);
            length += 3 + key_size;
        }

        char* element;
        int element_size;
        
#ifdef TEA_NAN_TAGGING
        if(item->value == OBJECT_VAL(map))
#else
        if(tea_value_equal(item->value, OBJECT_VAL(map)))
#endif
        {
            element = "{...}";
            element_size = 5;
        }
        else
        {
            TeaObjectString* s = tea_value_tostring(T, item->value);
            element = s->chars;
            element_size = s->length;
        } 

        if(element_size > (size - length - element_size - 6)) 
        {
            int old_size = size;
            if(element_size > size) 
            {
                size += element_size * 2 + 6;
            }
            else
            {
                size = size * 2 + 6;
            }

            string = TEA_GROW_ARRAY(T, char, string, old_size, size);
        }

        memcpy(string + length, element, element_size);
        length += element_size;

        if(count != map->count)
        {
            memcpy(string + length, ", ", 2);
            length += 2;
        }
    }

    memcpy(string + length, "}", 1);
    length += 1;
    string[length] = '\0';

    string = TEA_GROW_ARRAY(T, char, string, size, length + 1);

    return tea_string_take(T, string, length);
}

static TeaObjectString* range_tostring(TeaState* T, TeaObjectRange* range)
{
    char* start = tea_value_number_tostring(T, range->start)->chars;
    char* end = tea_value_number_tostring(T, range->end)->chars;

    int len = snprintf(NULL, 0, "%s...%s", start, end);
    char* string = TEA_ALLOCATE(T, char, len + 1);
    snprintf(string, len + 1, "%s...%s", start, end);

    return tea_string_take(T, string, len);
}

static TeaObjectString* module_tostring(TeaState* T, TeaObjectModule* module)
{
    int len = snprintf(NULL, 0, "<%s module>", module->name->chars);
    char* string = TEA_ALLOCATE(T, char, len + 1);
    snprintf(string, len + 1, "<%s module>", module->name->chars);

    return tea_string_take(T, string, len);
}

static TeaObjectString* class_tostring(TeaState* T, TeaObjectClass* klass)
{
    int len = snprintf(NULL, 0, "<%s>", klass->name->chars);
    char* string = TEA_ALLOCATE(T, char, len + 1);
    snprintf(string, len + 1, "<%s>", klass->name->chars);

    return tea_string_take(T, string, len);
}

static TeaObjectString* instance_tostring(TeaState* T, TeaObjectInstance* instance)
{
    int len = snprintf(NULL, 0, "<%s instance>", instance->klass->name->chars);
    char* string = TEA_ALLOCATE(T, char, len + 1);
    snprintf(string, len + 1, "<%s instance>", instance->klass->name->chars);

    return tea_string_take(T, string, len);
}

TeaObjectString* tea_obj_tostring(TeaState* T, TeaValue value)
{
    switch(OBJECT_TYPE(value))
    {
        case OBJ_FILE:
            return tea_string_literal(T, "<file>");
        case OBJ_BOUND_METHOD:
            return tea_string_literal(T, "<method>");
        case OBJ_NATIVE:
        {
            switch(AS_NATIVE(value)->type)
            {
                case NATIVE_PROPERTY:
                    return tea_string_literal(T, "<property>");
                case NATIVE_FUNCTION: 
                case NATIVE_METHOD: 
                    return tea_string_literal(T, "<function>");
            }
        }
        case OBJ_FUNCTION:
            return function_tostring(T, AS_FUNCTION(value));
        case OBJ_CLOSURE:
            return function_tostring(T, AS_CLOSURE(value)->function);
        case OBJ_LIST:
            return list_tostring(T, AS_LIST(value));
        case OBJ_MAP:
            return map_tostring(T, AS_MAP(value));
        case OBJ_RANGE:
            return range_tostring(T, AS_RANGE(value));
        case OBJ_MODULE:
            return module_tostring(T, AS_MODULE(value));
        case OBJ_CLASS:
            return class_tostring(T, AS_CLASS(value));
        case OBJ_INSTANCE:
            return instance_tostring(T, AS_INSTANCE(value));
        case OBJ_STRING:
            return AS_STRING(value);
        case OBJ_UPVALUE:
            return tea_string_literal(T, "<upvalue>");
        default:
            break;
    }
    return tea_string_literal(T, "unknown");
}

static bool range_equals(TeaObjectRange* a, TeaObjectRange* b)
{
    return a->start == b->start && a->end == b->end && a->step == b->step;
}

static bool list_equals(TeaObjectList* a, TeaObjectList* b)
{
    if(a->items.count != b->items.count)
    {
        return false;
    }

    for(int i = 0; i < a->items.count; ++i)
    {
        if(!tea_value_equal(a->items.values[i], b->items.values[i]))
        {
            return false;
        }
    }

    return true;
}

static bool map_equals(TeaObjectMap* a, TeaObjectMap* b)
{
    if(a->count != b->count)
    {
        return false;
    }

    if(a->count == 0)
    {
        return true;
    }

    for(int i = 0; i < a->capacity; i++)
    {
        TeaMapItem* item = &a->items[i];

        if(item->empty)
        {
            continue;
        }

        TeaValue value;
        if(!tea_map_get(b, item->key, &value))
        {
            return false;
        }

        if(!tea_value_equal(item->value, value))
        {
            return false;
        }
    }

    return true;
}

bool tea_obj_equal(TeaValue a, TeaValue b)
{
    if(OBJECT_TYPE(a) != OBJECT_TYPE(b)) return false;

    switch(OBJECT_TYPE(a))
    {
        case OBJ_RANGE:
            return range_equals(AS_RANGE(a), AS_RANGE(b));
        case OBJ_LIST:
            return list_equals(AS_LIST(a), AS_LIST(b));
        case OBJ_MAP:
            return map_equals(AS_MAP(a), AS_MAP(b));
        default:
            break;
    }
    return AS_OBJECT(a) == AS_OBJECT(b);
}

const char* tea_obj_type(TeaValue a)
{
    switch(OBJECT_TYPE(a))
    {
        case OBJ_USERDATA:
            return "userdata";
        case OBJ_UPVALUE:
            return "upvalue";
        case OBJ_FILE:
            return "file";
        case OBJ_RANGE:
            return "range";
        case OBJ_MODULE:
            return "module";
        case OBJ_CLASS:
            return "class";
        case OBJ_BOUND_METHOD:
            return "method";
        case OBJ_INSTANCE:
            return "instance";
        case OBJ_STRING:
            return "string";
        case OBJ_LIST:
            return "list";
        case OBJ_MAP:
            return "map";
        case OBJ_NATIVE:
        {
            switch(AS_NATIVE(a)->type)
            {
                case NATIVE_FUNCTION:
                case NATIVE_METHOD:
                    return "function";
                case NATIVE_PROPERTY:
                    return "property";
            }
        }
        case OBJ_CLOSURE:
        case OBJ_FUNCTION:
            return "function";
        default:
            break;
    }
    return "unknown";
}