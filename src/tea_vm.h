// tea_vm.h
// Teascript virtual machine

#ifndef TEA_VM_H
#define TEA_VM_H

#include "tea_state.h"

void teaV_runtime_error(TeaState* T, const char* format, ...);
bool teaV_call_value(TeaState* T, TeaValue callee, uint8_t arg_count);
TeaInterpretResult teaV_run_interpreter(TeaState* T);
TeaInterpretResult teaV_interpret_module(TeaState* T, const char* module_name, const char* source);

#endif