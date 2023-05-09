// tea_do.c
// Stack and Call structure of Teascript

#ifndef TEA_DO_H
#define TEA_DO_H

#include "tea_state.h"

typedef void (*TeaPFunction)(TeaState* T, void* ud);

void teaD_append_callframe(TeaState* T, TeaObjectClosure* closure, TeaValue* start);
void teaD_ensure_callframe(TeaState* T);

void teaD_ensure_stack(TeaState* T, int needed);

void teaD_call_value(TeaState* T, TeaValue callee, uint8_t arg_count);
void teaD_call(TeaState* T, TeaValue func, int arg_count);
int teaD_pcall(TeaState* T, TeaValue func, int arg_count);

void teaD_throw(TeaState* T, int code);

int teaD_runprotected(TeaState* T, TeaPFunction f, void* ud);

int teaD_protected_compiler(TeaState* T, TeaObjectModule* module, const char* source);

#endif