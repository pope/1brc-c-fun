#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Defines for some of the rules of 1brc
#define MAX_STATIONS 10000
#define MAX_STATION_NAME_LENGTH 100

// There are at most 10k weather stations per the fules. Using 2^14 to give
// room to find openings and the power of 2 allows for a bit-AND instead of
// modulo.
#define TABLE_STATS_CAP (1UL << 14)

// If we assume that there will be max stations, and each station has the max
// name length, and all values that are printed out are 4 characters long, we
// would be reserving something 2^20 and 2^22. That said, that's worst case -
// and since we're trying to be fast, we can cheat with a smaller buffer size.
// Still should protect against an overflow.
#define OUTPUT_BUFSIZE (1UL << 14)

#define HASH_PRIME 31

////
// Arena code modified from
// https://github.com/lzace817/examples/tree/master/arena

#define ALIGN_DOWN(n, a) ((n) & ~((a)-1))
#define ALIGN_UP(n, a) ALIGN_DOWN ((n) + (a)-1, (a))
#define KNOB_MMAP_SIZE (10L * 1024 * 1024 * 1024)

typedef struct arena
{
  void *data;
  size_t capacity;
  size_t mapped_cap;
  size_t size;
} Arena;

static inline struct arena *
arena_new (void)
{
  long cap = sysconf (_SC_PAGE_SIZE);
  if (cap == -1)
    {
      perror ("arena");
      exit (EXIT_FAILURE);
    }

  Arena *result = mmap (0, KNOB_MMAP_SIZE, PROT_NONE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (result == MAP_FAILED)
    {
      perror ("arena");
      exit (EXIT_FAILURE);
    }

  int status = mprotect (result, cap, PROT_WRITE | PROT_READ);
  if (status == -1)
    {
      perror ("arena");
      exit (EXIT_FAILURE);
    }

  result->data = &result[1];
  result->size = 0;
  result->capacity = cap - sizeof (Arena);
  result->mapped_cap = cap;

  return result;
}

static inline void *
arena_alloc (struct arena *a, size_t size)
{
  assert (a);
  assert (size > 0);

  long cap = sysconf (_SC_PAGE_SIZE);
  if (cap == -1)
    {
      perror ("arena");
      exit (EXIT_FAILURE);
    }

  if (a->size + size > a->capacity)
    {
      // grow
      size_t grow_ammount = a->size + size - a->capacity;
      grow_ammount = ALIGN_UP (grow_ammount, cap);
      assert (a->mapped_cap + grow_ammount <= KNOB_MMAP_SIZE);
      a->mapped_cap += grow_ammount;
      a->capacity += grow_ammount;
      int status = mprotect (a, a->mapped_cap, PROT_WRITE | PROT_READ);
      if (status == -1)
        {
          perror ("arena");
          exit (EXIT_FAILURE);
        }
    }

  void *result = (char *)a->data + a->size;
  a->size += size;
  return result;
}
////

static inline size_t
simple_hash_string (char *str)
{
  size_t h = 0;
  while (*str)
    h = (h * HASH_PRIME) + (unsigned char)*str++;
  return h;
}

typedef struct stats
{
  long sum;
  size_t count;
  int min;
  int max;
  size_t key_len;
  char key[];
} Stats;

typedef struct stablstable
{
  Arena *a;
  Stats *stats[TABLE_STATS_CAP];
} StatsTable;

static inline Stats *
statstable_get (StatsTable *table, char *key, size_t key_len, size_t hash)
{
  assert (table != NULL);
  assert (key != NULL);
  assert (key_len > 0);

  size_t idx = hash & (TABLE_STATS_CAP - 1);
  Stats *found = table->stats[hash & idx];
  while (found != NULL && found->key_len != key_len
         && memcmp (found->key, key, key_len) != 0)
    {
      hash += 1;
      idx = hash & (TABLE_STATS_CAP - 1);
      found = table->stats[idx];
    }

  if (found == NULL)
    {
      size_t size = sizeof (Stats) + sizeof (char) * (key_len + 1);
      Stats *stat = arena_alloc (table->a, size);
      stat->max = INT_MIN;
      stat->min = INT_MAX;
      stat->sum = 0L;
      stat->count = 0UL;
      stat->key_len = key_len;
      memcpy (stat->key, key, key_len);
      stat->key[key_len] = 0;
      table->stats[idx] = stat;
      return stat;
    }

  return found;
}

static inline int
stats__cmp (const void *aa, const void *bb)
{
  const Stats *a = *(Stats **)aa;
  const Stats *b = *(Stats **)bb;

  if (a == NULL && b == NULL)
    return 0;
  if (b == NULL)
    return -1;
  if (a == NULL)
    return 1;

  return strcmp (a->key, b->key);
}

static inline StatsTable *
process (char *data, size_t data_len)
{
  assert (data);
  assert (data_len > 0);

  Arena *a = arena_new ();
  StatsTable *table = arena_alloc (a, sizeof (StatsTable));
  table->a = a;

  size_t s = 0;
  while (s < data_len && data[s] != 0)
    {
      // Get the key and hash, reusing the key buffer. Getting the hash here is
      // one less loop we need to do.
      size_t hash = 0;
      char *key = NULL;
      size_t key_len = 0;
      {
        size_t e = s;
        while (data[e] != ';')
          {
            hash = (hash * HASH_PRIME) + (unsigned char)data[e++];
          }

        key_len = e - s;
        assert (key_len < MAX_STATION_NAME_LENGTH);
        key = &data[s];

        s = e + 1;
      }

      int temp = 0;
      {
        int sign = 1;
        if (data[s] == '-')
          {
            sign = -1;
            s++;
          }

        assert (isdigit (data[s]));

        if (data[s + 1] == '.')
          {
            temp = ((data[s] * 10) + data[s + 2] - ('0' * 11)) * sign;
            s += 4; // Advance past newline
          }
        else
          {
            temp = ((data[s] * 100) + (data[s + 1] * 10) + data[s + 3]
                    - ('0' * 111))
                   * sign;
            s += 5; // Advance past newline
          }
      }

      assert (data[s - 1] == '\n');

      Stats *stats = statstable_get (table, key, key_len, hash);
      stats->count++;
      stats->sum += temp;
      stats->max = MAX (stats->max, temp);
      stats->min = MIN (stats->min, temp);
    }

  return table;
}

static inline size_t
statstable__stats_to_str (char *buf, size_t maxlen, const Stats *stats)
{
  float avg = (float)stats->sum / (float)stats->count;
  size_t len = snprintf (buf, maxlen, "%s=%.1f/%.1f/%.1f", stats->key,
                         (float)stats->min / 10.0, avg / 10.0,
                         (float)stats->max / 10.0);
  return MIN (maxlen, len);
}

static inline size_t
statstable_to_str (char *buf, size_t maxlen, const StatsTable *table)
{
  char *s = buf;
  if (maxlen >= 1)
    {
      *buf++ = '{';
      maxlen--;
    }
  {
    Stats *stats = table->stats[0];
    assert (stats);
    size_t n = statstable__stats_to_str (buf, maxlen, stats);
    buf += n;
    maxlen -= n;
  }
  for (size_t i = 1; i < TABLE_STATS_CAP; i++)
    {
      Stats *stats = table->stats[i];
      if (stats == NULL)
        // The data is sorted, so once we see a NULL, there's nothing left to
        // print.
        break;
      if (maxlen >= 2)
        {
          *buf++ = ',';
          *buf++ = ' ';
          maxlen -= 2;
        }
      size_t n = statstable__stats_to_str (buf, maxlen, stats);
      buf += n;
      maxlen -= n;
    }
  if (maxlen >= 2)
    {
      *buf++ = '}';
      *buf++ = '\n';
      maxlen -= 2;
    }

  *buf = 0;

  return buf - s - (maxlen == 0 ? 1 : 0);
}

int
main (int argc, char **argv)
{
  char output_buf[OUTPUT_BUFSIZE];

  // From https://github.com/dannyvankooten/1brc/blob/main/analyze.c.
  // Use a child process to do all of the work. The child then sends the data
  // over to the parent to be printed. While the parent is printing, the
  // child is cleaning up it's memory.
  //
  // Trying this using threads with OpenMP - where one thread prints data and
  // the other cleans up - didn't help. Actually forking did.
  int pipefd[2];
  if (pipe (pipefd) != 0)
    {
      perror ("pipe");
      return EXIT_FAILURE;
    }

  pid_t pid = fork ();
  if (pid > 0)
    {
      if (close (pipefd[1]) != 0)
        {
          perror ("close");
          return EXIT_FAILURE;
        }

      if (read (pipefd[0], &output_buf, OUTPUT_BUFSIZE) == -1)
        {
          perror ("read");
          return EXIT_FAILURE;
        }
      printf ("%s", output_buf);

      if (close (pipefd[0]) != 0)
        {
          perror ("close");
          return EXIT_FAILURE;
        }
      return EXIT_SUCCESS;
    }

  if (close (pipefd[0]) != 0)
    {
      perror ("close");
      return EXIT_FAILURE;
    }

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

  char *data
      = mmap (NULL, sb.st_size, PROT_READ, MAP_PRIVATE | MAP_NORESERVE, fd, 0);
  if (data == MAP_FAILED)
    {
      perror ("mmap");
      return EXIT_FAILURE;
    }
#ifdef _DEFAULT_SOURCE
  if (madvise (data, sb.st_size, MADV_WILLNEED | MADV_RANDOM) == -1)
    {
      perror ("madvise");
      return EXIT_FAILURE;
    }
#endif

  Arena *a = arena_new ();

  int batches = omp_get_max_threads ();
  assert (batches > 0);

  StatsTable **batch_res = arena_alloc (a, sizeof (StatsTable *) * batches);

#pragma omp parallel for
  for (int i = 0; i < batches; i++)
    {
      size_t s = i * (sb.st_size / batches);
      if (!(s == 0 || data[s - 1] == '\n'))
        {
          while (data[s] != '\n')
            s++;
          s++; // consume the newline
        }

      size_t e = MIN ((i + 1) * (sb.st_size / batches), sb.st_size);
      if (!(e == (size_t)sb.st_size || data[e] == '\n'))
        {
          while (data[e] != '\n')
            e++;
        }

      batch_res[i] = process (&data[s], e - s);
    }

  StatsTable *solution = batch_res[0];
  for (int i = 1; i < batches; i++)
    {
      StatsTable *table = batch_res[i];
      for (size_t j = 0; j < TABLE_STATS_CAP; j++)
        {
          Stats *stats = table->stats[j];
          if (stats == NULL)
            continue;

          Stats *update = statstable_get (solution, stats->key, stats->key_len,
                                          simple_hash_string (stats->key));
          update->sum += stats->sum;
          update->count += stats->count;
          update->max = MAX (update->max, stats->max);
          update->min = MIN (update->min, stats->min);
        }
    }

  qsort (solution->stats, TABLE_STATS_CAP, sizeof (Stats *), stats__cmp);
  size_t output_buf_len
      = statstable_to_str (output_buf, OUTPUT_BUFSIZE, solution);
  if (write (pipefd[1], output_buf, output_buf_len) == -1)
    {
      perror ("write");
      return EXIT_FAILURE;
    }

  if (close (pipefd[1]) != 0)
    {
      perror ("close");
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
