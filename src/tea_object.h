/*
** tea_object.h
** Teascript object model and functions
*/

#ifndef TEA_OBJECT_H
#define TEA_OBJECT_H

#include <stdio.h>

#include "tea.h"
#include "tea_def.h"
#include "tea_memory.h"
#include "tea_chunk.h"
#include "tea_table.h"
#include "tea_value.h"

#define OBJECT_TYPE(value) (AS_OBJECT(value)->type)

#define IS_USERDATA(value) tea_obj_istype(value, OBJ_USERDATA)
#define IS_NATIVE(value) tea_obj_istype(value, OBJ_NATIVE)
#define IS_RANGE(value) tea_obj_istype(value, OBJ_RANGE)
#define IS_FILE(value) tea_obj_istype(value, OBJ_FILE)
#define IS_MODULE(value) tea_obj_istype(value, OBJ_MODULE)
#define IS_LIST(value) tea_obj_istype(value, OBJ_LIST)
#define IS_MAP(value) tea_obj_istype(value, OBJ_MAP)
#define IS_BOUND_METHOD(value) tea_obj_istype(value, OBJ_BOUND_METHOD)
#define IS_CLASS(value) tea_obj_istype(value, OBJ_CLASS)
#define IS_CLOSURE(value) tea_obj_istype(value, OBJ_CLOSURE)
#define IS_FUNCTION(value) tea_obj_istype(value, OBJ_FUNCTION)
#define IS_INSTANCE(value) tea_obj_istype(value, OBJ_INSTANCE)
#define IS_STRING(value) tea_obj_istype(value, OBJ_STRING)

#define AS_USERDATA(value) ((TeaObjectUserdata*)AS_OBJECT(value))
#define AS_NATIVE(value) ((TeaObjectNative*)AS_OBJECT(value))
#define AS_RANGE(value) ((TeaObjectRange*)AS_OBJECT(value))
#define AS_FILE(value) ((TeaObjectFile*)AS_OBJECT(value))
#define AS_MODULE(value) ((TeaObjectModule*)AS_OBJECT(value))
#define AS_LIST(value) ((TeaObjectList*)AS_OBJECT(value))
#define AS_MAP(value) ((TeaObjectMap*)AS_OBJECT(value))
#define AS_BOUND_METHOD(value) ((TeaObjectBoundMethod*)AS_OBJECT(value))
#define AS_CLASS(value) ((TeaObjectClass*)AS_OBJECT(value))
#define AS_CLOSURE(value) ((TeaObjectClosure*)AS_OBJECT(value))
#define AS_FUNCTION(value) ((TeaObjectFunction*)AS_OBJECT(value))
#define AS_INSTANCE(value) ((TeaObjectInstance*)AS_OBJECT(value))
#define AS_STRING(value) ((TeaObjectString*)AS_OBJECT(value))
#define AS_CSTRING(value) (((TeaObjectString*)AS_OBJECT(value))->chars)

#define ALLOCATE_OBJECT(T, type, object_type) (type*)tea_obj_allocate(T, sizeof(type), object_type)

typedef enum
{
    OBJ_USERDATA,
    OBJ_STRING,
    OBJ_RANGE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_MODULE,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_CLASS,
    OBJ_INSTANCE,
    OBJ_BOUND_METHOD,
    OBJ_LIST,
    OBJ_MAP,
    OBJ_FILE,
} TeaObjectType;

typedef enum
{
    TYPE_FUNCTION,
    TYPE_CONSTRUCTOR,
    TYPE_STATIC,
    TYPE_METHOD,
    TYPE_SCRIPT
} TeaFunctionType;

struct TeaObject
{
    TeaObjectType type;
    bool is_marked;
    struct TeaObject* next;
};

typedef struct
{
    TeaObject obj;
    double start;
    double end;
    double step;
} TeaObjectRange;

struct TeaObjectFile
{
    TeaObject obj;
    FILE* file;
    TeaObjectString* path;
    TeaObjectString* type;
    int is_open;
};

typedef struct
{
    TeaObject obj;
    TeaObjectString* name;
    TeaObjectString* path;
    TeaTable values;
} TeaObjectModule;

typedef struct
{
    TeaObject obj;
    int arity;
    int arity_optional;
    int variadic;
    int upvalue_count;
    int max_slots;
    TeaChunk chunk;
    TeaFunctionType type;
    TeaObjectString* name;
    TeaObjectModule* module;
} TeaObjectFunction;

typedef enum
{
    NATIVE_FUNCTION,
    NATIVE_METHOD,
    NATIVE_PROPERTY
} TeaNativeType;

typedef struct
{
    TeaObject obj;
    TeaNativeType type;
    TeaCFunction fn;
} TeaObjectNative;

struct TeaObjectString
{
    TeaObject obj;
    int length;
    char* chars;
    uint32_t hash;
};

typedef struct
{
    TeaObject obj;
    void* data;
    size_t size;
} TeaObjectUserdata;

typedef struct
{
    TeaObject obj;
    TeaValueArray items;
} TeaObjectList;

typedef struct 
{
    TeaValue key;
    TeaValue value;
    bool empty;
} TeaMapItem;

typedef struct
{
    TeaObject obj;
    int count;
    int capacity;
    TeaMapItem* items;
} TeaObjectMap;

typedef struct TeaObjectUpvalue
{
    TeaObject obj;
    TeaValue* location;
    TeaValue closed;
    struct TeaObjectUpvalue* next;
} TeaObjectUpvalue;

typedef struct
{
    TeaObject obj;
    TeaObjectFunction* function;
    TeaObjectUpvalue** upvalues;
    int upvalue_count;
} TeaObjectClosure;

typedef struct TeaObjectClass
{
    TeaObject obj;
    TeaObjectString* name;
    struct TeaObjectClass* super;
    TeaValue constructor;
    TeaTable statics;
    TeaTable methods;
} TeaObjectClass;

typedef struct
{
    TeaObject obj;
    TeaObjectClass* klass;
    TeaTable fields;
} TeaObjectInstance;

typedef struct
{
    TeaObject obj;
    TeaValue receiver;
    TeaValue method;
} TeaObjectBoundMethod;

TeaObject* tea_obj_allocate(TeaState* T, size_t size, TeaObjectType type);

TeaObjectBoundMethod* tea_obj_new_bound_method(TeaState* T, TeaValue receiver, TeaValue method);
TeaObjectInstance* tea_obj_new_instance(TeaState* T, TeaObjectClass* klass);
TeaObjectClass* tea_obj_new_class(TeaState* T, TeaObjectString* name, TeaObjectClass* superclass);

TeaObjectUserdata* tea_obj_new_userdata(TeaState* T, size_t size);

TeaObjectList* tea_obj_new_list(TeaState* T);

TeaObjectModule* tea_obj_new_module(TeaState* T, TeaObjectString* name);
TeaObjectFile* tea_obj_new_file(TeaState* T, TeaObjectString* path, TeaObjectString* type);
TeaObjectRange* tea_obj_new_range(TeaState* T, double start, double end, double step);

TeaObjectString* tea_obj_tostring(TeaState* T, TeaValue value);
bool tea_obj_equal(TeaValue a, TeaValue b);
const char* tea_obj_type(TeaValue a);

static inline bool tea_obj_istype(TeaValue value, TeaObjectType type)
{
    return IS_OBJECT(value) && AS_OBJECT(value)->type == type;
}

static inline bool tea_obj_isfalse(TeaValue value)
{
    return  IS_NULL(value) || 
            (IS_BOOL(value) && !AS_BOOL(value)) || 
            (IS_NUMBER(value) && AS_NUMBER(value) == 0) || 
            (IS_STRING(value) && AS_CSTRING(value)[0] == '\0') || 
            (IS_LIST(value) && AS_LIST(value)->items.count == 0) ||
            (IS_MAP(value) && AS_MAP(value)->count == 0);
}

#endif