#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  char *measurements_filename = argc == 2 ? argv[1] : "./measurements-1k.txt";
  int fd = open (measurements_filename, O_RDONLY);
  if (fd == -1)
    {
      perror ("open");
      return EXIT_FAILURE;
    }

  struct stat sb = { 0 };
  if (fstat (fd, &sb) == -1)
    {
      perror ("fstat");
      return EXIT_FAILURE;
    }

  size_t data_len = (size_t)sb.st_size;
  char *data
      = mmap (NULL, data_len, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
  if (data == MAP_FAILED)
    {
      perror ("mmap");
      return EXIT_FAILURE;
    }
#ifdef _DEFAULT_SOURCE
  if (madvise (data, (size_t)sb.st_size, MADV_WILLNEED | MADV_RANDOM) == -1)
    {
      perror ("madvise");
      return EXIT_FAILURE;
    }
#endif

  unsigned long char_count = 0;
  unsigned long num_newlines = 0;
#pragma omp parallel for reduction(+ : char_count, num_newlines)
  for (size_t i = 0; i < data_len; i++)
    {
      char_count++;
      if (data[i] == '\n')
        num_newlines++;
    }

  printf ("Number of characters: %lu\n", char_count);
  printf ("Number of newlines: %lu\n", num_newlines);
}
