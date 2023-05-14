// tea_util.c
// Teascript utility functions

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tea_util.h"
#include "tea_state.h"
#include "tea_memory.h"

char* teaZ_read_file(TeaState* T, const char* path) 
{
    FILE* file = fopen(path, "rb");
    if(file == NULL) 
    {
        return NULL;
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char* buffer = TEA_ALLOCATE(T, char, file_size + 1);

    size_t bytesRead = fread(buffer, sizeof(char), file_size, file);
    if(bytesRead < file_size) 
    {
        TEA_FREE_ARRAY(T, char, buffer, file_size + 1);
        fprintf(stderr, "Could not read file \"%s\"\n", path);
        exit(74);
    }

    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

TeaObjectString* teaZ_dirname(TeaState* T, char* path, int len) 
{
    if(!len) 
    {
        return teaO_new_literal(T, ".");
    }

    char* sep = path + len;

    /* trailing slashes */
    while(sep != path) 
    {
        if(0 == IS_DIR_SEPARATOR (*sep))
            break;
        sep--;
    }

    /* first found */
    while(sep != path) 
    {
        if(IS_DIR_SEPARATOR (*sep))
            break;
        sep--;
    }

    /* trim again */
    while(sep != path) 
    {
        if(0 == IS_DIR_SEPARATOR (*sep))
            break;
        sep--;
    }

    if(sep == path && !IS_DIR_SEPARATOR(*sep)) 
    {
        return teaO_new_literal(T, ".");
    }

    len = sep - path + 1;

    return teaO_copy_string(T, path, len);
}

bool teaZ_resolve_path(char* directory, char* path, char* ret) 
{
    char buf[PATH_MAX];
    if(*path == DIR_SEPARATOR)
    {
        snprintf(buf, PATH_MAX, "%s", path);
    }
    else
    {
        snprintf(buf, PATH_MAX, "%s%c%s", directory, DIR_SEPARATOR, path);
    }

#ifdef _WIN32
    _fullpath(ret, buf, PATH_MAX);
#else
    if(realpath(buf, ret) == NULL) 
    {
        return false;
    }
#endif

    return true;
}

TeaObjectString* teaZ_get_directory(TeaState* T, char* source) 
{
    char res[PATH_MAX];
    if(!teaZ_resolve_path(".", source, res)) 
    {
        teaV_runtime_error(T, "Unable to resolve path '%s'", source);
        exit(1);
    }

    return teaZ_dirname(T, res, strlen(res));
}