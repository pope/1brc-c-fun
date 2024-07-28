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

#define HASH_PRIME 37

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
  size_t used;
} Arena;

static inline struct arena *
arena_new (void)
{
  long cap = sysconf (_SC_PAGE_SIZE);
  if (cap == -1)
    {
      perror ("arena");
      exit (1);
    }

  Arena *result = mmap (0, KNOB_MMAP_SIZE, PROT_NONE,
                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (result == MAP_FAILED)
    {
      perror ("arena");
      exit (1);
    }

  int status = mprotect (result, cap, PROT_WRITE | PROT_READ);
  if (status == -1)
    {
      perror ("arena");
      exit (1);
    }

  result->data = &result[1];
  result->used = 0;
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
      exit (1);
    }

  int start = a->used;
  void *result = (char *)a->data + start;
  if (start + size > a->capacity)
    {
      // grow
      size_t grow_ammount = start + size - a->capacity;
      grow_ammount = ALIGN_UP (grow_ammount, cap);
      assert (a->mapped_cap + grow_ammount <= KNOB_MMAP_SIZE);
      a->mapped_cap += grow_ammount;
      a->capacity += grow_ammount;
      int status = mprotect (a, a->mapped_cap, PROT_WRITE | PROT_READ);
      if (status == -1)
        {
          perror ("arena");
          exit (1);
        }
    }
  a->used = start + size;

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

static inline void
statstable__print_stats (Stats *stats)
{
  float avg = (float)stats->sum / (float)stats->count;
  printf ("%s=%.1f/%.1f/%.1f", stats->key, (float)stats->min / 10.0,
          avg / 10.0, (float)stats->max / 10.0);
}

static inline void
statstable_print (StatsTable *table)
{
  printf ("{");
  {
    Stats *stats = table->stats[0];
    assert (stats);
    statstable__print_stats (stats);
  }
  for (size_t i = 1; i < TABLE_STATS_CAP; i++)
    {
      Stats *stats = table->stats[i];
      if (stats == NULL)
        break;
      printf (", ");
      statstable__print_stats (stats);
    }
  printf ("}\n");
}

void
single_core_run (char *data, size_t data_len)
{
  StatsTable *table = process (data, data_len);
  qsort (table->stats, TABLE_STATS_CAP, sizeof (Stats *), stats__cmp);
  statstable_print (table);
}

void
multi_core_run (Arena *a, char *data, size_t data_len)
{
  int batches = omp_get_max_threads ();
  assert (batches > 0);

  StatsTable **batch_res = arena_alloc (a, sizeof (StatsTable *) * batches);

#pragma omp parallel for
  for (int i = 0; i < batches; i++)
    {
      size_t s = i * (data_len / batches);
      if (!(s == 0 || data[s - 1] == '\n'))
        {
          while (data[s] != '\n')
            s++;
          s++; // consume the newline
        }

      size_t e = MIN ((i + 1) * (data_len / batches), data_len);
      if (!(e == data_len || data[e] == '\n'))
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
  statstable_print (solution);
}

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
  int ok = fstat (fd, &sb);
  if (ok == -1)
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
  ok = madvise (data, sb.st_size, MADV_WILLNEED | MADV_RANDOM);
  if (ok == -1)
    {
      perror ("madvise");
      return EXIT_FAILURE;
    }
#endif

  // single_core_run (data, sb.st_size);
  {
    Arena *a = arena_new ();
    multi_core_run (a, data, sb.st_size);
  }
}
