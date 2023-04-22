// tea_import.h
// Teascript module loading

#ifndef TEA_IMPORT_H
#define TEA_IMPORT_H

#include "tea.h"

#define TEA_MATH_MODULE "math"
void tea_import_math(TeaState* T);

#define TEA_TIME_MODULE "time"
void tea_import_time(TeaState* T);

#define TEA_OS_MODULE "os"
void tea_import_os(TeaState* T);

#define TEA_SYS_MODULE "sys"
void tea_import_sys(TeaState* T);

#define TEA_IO_MODULE "io"
void tea_import_io(TeaState* T);

#define TEA_RANDOM_MODULE "random"
void tea_import_random(TeaState* T);

void teaI_import_native_module(TeaState* T, int index);
int teaI_find_native_module(char* name, int length);

#endif