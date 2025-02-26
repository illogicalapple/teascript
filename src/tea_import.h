/*
** tea_import.h
** Teascript import loading
*/

#ifndef TEA_IMPORT_H
#define TEA_IMPORT_H

#include "tea_state.h"
#include "tea_object.h"

void tea_import_relative(TeaState* T, TeaObjectString* mod, TeaObjectString* path_name);
void tea_import_logical(TeaState* T, TeaObjectString* name);

#endif