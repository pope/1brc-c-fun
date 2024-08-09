#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

int
main (int argc, char **argv)
{
  char *measurements_filename = argc == 2 ? argv[1] : "../measurements-1k.txt";
  FILE *file = fopen (measurements_filename, "r");
  if (file == NULL)
    {
      perror ("fopen");
      return EXIT_FAILURE;
    }

  unsigned long char_count = 0;
  unsigned long num_newlines = 0;
  char buf[1 << 12];
  while (!feof (file))
    {
      size_t len = fread (buf, sizeof (char), 1 << 12, file);
      for (size_t i = 0; i < len; i++)
        {
          char_count++;
          if (buf[i] == '\n')
            num_newlines++;
        }
    }

  printf ("Number of characters: %lu\n", char_count);
  printf ("Number of newlines: %lu\n", num_newlines);
}
