#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define STBDS_SIZE_T_BITS ((sizeof (size_t)) * 8)
#define STBDS_ROTATE_LEFT(val, n)                                             \
  (((val) << (n)) | ((val) >> (STBDS_SIZE_T_BITS - (n))))
#define STBDS_ROTATE_RIGHT(val, n)                                            \
  (((val) >> (n)) | ((val) << (STBDS_SIZE_T_BITS - (n))))

static size_t
stbds_hash_string (char *str, size_t seed)
{
  size_t hash = seed;
  while (*str)
    hash = STBDS_ROTATE_LEFT (hash, 9) + (unsigned char)*str++;

  // Thomas Wang 64-to-32 bit mix function, hopefully also works in 32 bits
  hash ^= seed;
  hash = (~hash) + (hash << 18);
  hash ^= hash ^ STBDS_ROTATE_RIGHT (hash, 31);
  hash = hash * 21;
  hash ^= hash ^ STBDS_ROTATE_RIGHT (hash, 11);
  hash += (hash << 6);
  hash ^= STBDS_ROTATE_RIGHT (hash, 22);
  return hash + seed;
}

static unsigned int
simple_hash_string (char *str)
{
  unsigned int h = 0;
  while (*str)
    h = (h * 31) + (unsigned char)*str++;
  return h;
}

int
main (void)
{
  char *data;
  {
    int fd = open ("./weather-stations.txt", O_RDONLY);
    if (fd == -1)
      {
        perror ("open");
        return EXIT_FAILURE;
      }

    struct stat sb = { 0 };
    int ok = fstat (fd, &sb);
    if (ok == -1)
      {
        perror ("fstat");
        return EXIT_FAILURE;
      }

    data = mmap (NULL, (size_t)sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
      {
        perror ("mmap");
        return EXIT_FAILURE;
      }
  }

  char buf[1024];
  size_t s = 0;
  while (data[s] != 0)
    {
      size_t e = s;
      while (data[e] != '\n')
        e++;

      size_t len = e - s;
      assert (len < 1024);
      strncpy (buf, data + s, len);
      buf[len] = 0;

      printf ("stbds\t\"%s\"\t%zu\t%zu\n", buf,
              stbds_hash_string (buf, 2147483587),
              stbds_hash_string (buf, 2147483587) & ((1UL << 16) - 1));
      printf ("smpl\t\"%s\"\t%u\t%u\n", buf, simple_hash_string (buf),
              simple_hash_string (buf) & ((1U << 17) - 1));

      s = e + 1;
    }

  return EXIT_SUCCESS;
}
