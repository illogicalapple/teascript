CC = gcc
CFLAGS =
MODE = debug

ifeq ($(MODE), debug)
    CFLAGS += -g -O0
else
    CFLAGS += -O2 -s
endif

SRC_FILES = $(wildcard src/*.c)
OBJ_FILES = $(patsubst %.c, %.o, $(SRC_FILES))

tea: $(OBJ_FILES)
	$(CC) $(CFLAGS) -o $@ $^ -lm

%.o: %.c src/*.h
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

.PHONY: all
all: tea