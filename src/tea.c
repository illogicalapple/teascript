// tea.c
// Teascript standalone interpreter

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>

#include "tea.h"

static TeaState* global = NULL;

void tsignal(int id)
{
    tea_close(global);
    exit(0);
}

static void clear()
{
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
    system("clear");
#elif defined(_WIN32) || defined(_WIN64)
    system("cls");
#endif
}

static void repl(TeaState* T)
{
    global = T;
    signal(SIGINT, tsignal);

    char line[1024];
    while(true)
    {
        line:
        tea_write_string("> ", 2);

        if(!fgets(line, sizeof(line), stdin))
        {
            tea_write_line();
            break;
        }

        if(strcmp(line, "exit\n") == 0)
        {
            break;
        }
        
        if(strcmp(line, "clear\n") == 0)
        {
            clear();
            goto line;
        }

        tea_interpret(T, "<stdin>", line);
    }
}

static char* read_file(const char* path)
{
    FILE* file = fopen(path, "rb");
    if(file == NULL)
    {
        fprintf(stderr, "Could not open file \"%s\"\n", path);
        exit(74);
    }

    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    char* buffer = (char*)malloc(file_size + 1);
    if(buffer == NULL)
    {
        fprintf(stderr, "Not enough memory to read \"%s\"\n", path);
        exit(74);
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    if(bytes_read < file_size)
    {
        fprintf(stderr, "Could not read file \"%s\"\n", path);
        exit(74);
    }

    buffer[bytes_read] = '\0';

    fclose(file);

    return buffer;
}

static void run_file(TeaState* T, const char* path)
{
    char* source = read_file(path);

    TeaInterpretResult result = tea_interpret(T, path, source);
    free(source);

    if(result == TEA_COMPILE_ERROR)
        exit(65);
    if(result == TEA_RUNTIME_ERROR)
        exit(70);
}

void f(TeaState* T)
{
    printf(":: top = %d\n", tea_get_top(T));
    double a = tea_get_number(T, 0);
    double b = tea_get_number(T, 1);
    printf(":: a = %f\n:: b = %f\n", a, b);
    tea_push_number(T, a + b);
    printf(":: a + b = %f\n", tea_get_number(T, 2));
}

void h(TeaState* T)
{
    printf(":: top = %d\n", tea_get_top(T));
    printf(":: %s\n", tea_type_name(T, 0));

    printf(":: from H function\n");

    tea_push_range(T, 1, 2, 3);
}

void g(TeaState* T)
{
    printf(":: top = %d\n", tea_get_top(T));
    printf(":: %s %s\n", tea_get_string(T, 0), tea_get_string(T, 1));

    tea_push_cfunction(T, h);
    tea_new_list(T);
    tea_call(T, 1);
}

int main(int argc, const char* argv[])
{
    TeaState* T = tea_open();
    if(T == NULL)
    {
        fprintf(stderr, "Cannot create state: not enough memory");
        return EXIT_FAILURE;
    }
    tea_set_argv(T, argc, argv);

    printf(":: top = %d\n", tea_get_top(T));

    tea_push_cfunction(T, g);
    tea_push_string(T, "HELLO");
    tea_push_string(T, "WORLD");
    printf(":: top = %d\n", tea_get_top(T));
    tea_call(T, 2);

    printf(":: top = %d\n", tea_get_top(T));

    tea_push_cfunction(T, g);
    tea_push_string(T, "HELLO");
    tea_push_string(T, "WORLD");
    printf(":: top = %d\n", tea_get_top(T));
    tea_call(T, 2);

    printf(":: top = %d\n", tea_get_top(T));
    tea_pop(T, 2);
    printf(":: top = %d\n", tea_get_top(T));

    if(argc == 1)
    {
        tea_write_version();
        tea_set_repl(T, true);
        repl(T);
    }
    else if(argc >= 2)
    {
        run_file(T, argv[1]);
    }
    else
    {
        fprintf(stderr, "Usage: tea [path]\n");
        exit(64);
    }

    printf(":: top = %d\n", tea_get_top(T));
    printf(":: type = %s\n", tea_type_name(T, -1));

    tea_close(T);

    return EXIT_SUCCESS;
}