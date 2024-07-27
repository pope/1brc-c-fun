.PHONY: all clean

CFLAGS += -O3 -g -Wall -Wextra -pedantic -std=c99 -D_GNU_SOURCE
LDFLAGS += -fopenmp

all: main hash_research

main: main.c
hash_research: hash_research.c

clean:
	-rm main hash_research
