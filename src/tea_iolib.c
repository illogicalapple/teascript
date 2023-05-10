// tea_io.c
// Teascript io module

#include "stdio.h"

#include "tea.h"

#include "tea_import.h"
#include "tea_core.h"
#include "tea_vm.h"

static void create_stdfile(TeaState* T, FILE* f, const char* name, const char* mode)
{
    TeaObjectFile* file = teaO_new_file(T, teaO_new_string(T, ""), teaO_new_string(T, mode));
    file->file = f;
    file->is_open = -1;

    teaV_push(T, OBJECT_VAL(file));

    tea_set_key(T, 0, name);
}

static const TeaModule io_module[] = {
    { "stdin", NULL },
    { "stdout", NULL },
    { "stderr", NULL },
    { NULL, NULL }
};

void tea_import_io(TeaState* T)
{
    tea_create_module(T, TEA_IO_MODULE, io_module);
    create_stdfile(T, stdout, "stdout", "w");
    create_stdfile(T, stdin, "stdin", "r");
    create_stdfile(T, stderr, "stderr", "w");
}