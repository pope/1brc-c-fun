.PHONY: all clean

CFLAGS += -O3 -g -Wall -Wextra -pedantic -std=c2x -m64 -march=native -mtune=native
CFLAGS += -D_DEFAULT_SOURCE
CFLAGS += -DNDEBUG
# CFLAGS += -DNO_CHILD_PROCESS

LDFLAGS += -fopenmp

all: main hash_research

main: main.c
hash_research: hash_research.c

clean:
	-rm -rf main hash_research *.dSYM

compile_commands.json: Makefile
	make clean && bear -- make
