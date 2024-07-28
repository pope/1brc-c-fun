.PHONY: all clean

CFLAGS += -O3 -g -Wall -Wextra -pedantic -std=c2x -m64 -march=native -mtune=native -DNDEBUG
LDFLAGS += -fopenmp

all: main hash_research

main: main.c
hash_research: hash_research.c

clean:
	-rm -rf main hash_research *.dSYM
