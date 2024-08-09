.PHONY: all clean

CFLAGS += -O3 -g -Wall -Wextra -pedantic -std=c99 -m64 -march=native -mtune=native
CFLAGS += -D_DEFAULT_SOURCE
CFLAGS += -DNDEBUG

LDFLAGS += -fopenmp

READTESTS = fgets fread fread_omp mmap mmap_omp

all: $(READTESTS)

fgets: fgets.c
fread: fread.c
fread_omp: fread_omp.c
mmap: mmap.c
mmap_omp: mmap_omp.c

clean:
	-rm -rf $(READTESTS)
