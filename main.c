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

// There are <42k weather stations. Using 2^16 to give room to find openings
// and the power of 2 allows for a bit-AND instead of modulo.
#define TABLE_STATS_CAP 65536UL

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

struct arena *
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

void *
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

size_t
simple_hash_string (char *str)
{
  size_t h = 0;
  while (*str)
    h = (h * 31) + (unsigned char)*str++;
  return h;
}

typedef struct stats
{
  int sum;
  int count;
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

Stats *
statstable_get (StatsTable *table, char *key, size_t key_len)
{
  assert (table != NULL);
  assert (key != NULL);
  assert (key_len > 0);

  for (size_t idx = simple_hash_string (key) & (TABLE_STATS_CAP - 1);
       idx < TABLE_STATS_CAP; idx++)
    {
      Stats *found = table->stats[idx];

      if (found == NULL)
        {
          size_t size = sizeof (Stats) + sizeof (char) * (key_len + 1);
          Stats *stat = arena_alloc (table->a, size);
          stat->max = INT_MIN;
          stat->min = INT_MAX;
          stat->sum = 0;
          stat->count = 0;
          stat->key_len = key_len;
          strncpy (stat->key, key, key_len);
          table->stats[idx] = stat;
          return stat;
        }

      if (found->key_len == key_len && strncmp (found->key, key, key_len) == 0)
        {
          return found;
        }
    }

  assert (NULL); // There should have been enough space
  return NULL;
}

size_t
statstable__sort_partition (StatsTable *table, size_t low, size_t high)
{
  assert (table);
  assert (low < high);

  Stats *tmp = NULL;
  Stats *pivot = table->stats[high];
  size_t i = low;

  if (pivot == NULL)
    {
      for (size_t j = low; j < high; j++)
        {
          tmp = table->stats[i];
          table->stats[i] = table->stats[j];
          table->stats[j] = tmp;
          i++;
        }
    }
  else
    {
      for (size_t j = low; j < high; j++)
        {
          Stats *stats = table->stats[j];
          if (stats != NULL && strcmp (stats->key, pivot->key) <= 0)
            {
              tmp = table->stats[i];
              table->stats[i] = table->stats[j];
              table->stats[j] = tmp;
              i++;
            }
        }
    }

  tmp = table->stats[i];
  table->stats[i] = table->stats[high];
  table->stats[high] = tmp;
  return i;
}

void
statstable_sort (StatsTable *table, size_t low, size_t high)
{
  assert (table);

  while (low < high)
    {
      size_t p = statstable__sort_partition (table, low, high);
      statstable_sort (table, low, p - 1);
      low = p + 1;
    }
}

void
process (StatsTable *table, char *data, size_t data_len)
{
  assert (table);
  assert (data);
  assert (data_len > 0);

  char key[64]; // Max city name lenght is 29 characters. So 64 should be more
                // than enough.

  size_t s = 0;
  while (s < data_len && data[s] != 0)
    {
      // Get the key, reusing the key buffer
      {
        size_t e = s;
        while (data[e] != ';')
          e++;

        size_t len = e - s;
        assert (len < 64);

        strncpy (key, data + s, len);
        key[len] = 0;

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
            temp = ((data[s] - '0') * 10) + (data[s + 2] - '0');
            s += 3; // Advance to newline
          }
        else
          {
            temp = ((data[s] - '0') * 100) + ((data[s + 1] - '0') * 10)
                   + (data[s + 3] - '0');
            s += 4; // Advance to newline
          }
        temp *= sign;
      }

      assert (data[s] == '\n');
      s++;

      Stats *stats = statstable_get (table, key, strlen (key));
      stats->count++;
      stats->sum += temp;
      stats->max = MAX (stats->max, temp);
      stats->min = MIN (stats->min, temp);
    }
}

inline void
statstable__print_stats (Stats *stats)
{
  int avg = stats->sum / stats->count;
  printf ("%s=%.1f/%.1f/%.1f", stats->key, stats->min / 10.0, avg / 10.0,
          stats->max / 10.0);
}

void
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
single_core_run (Arena *a, char *data, size_t data_len)
{
  StatsTable *table = arena_alloc (a, sizeof (StatsTable));
  assert (table);
  table->a = a;

  process (table, data, data_len);
  statstable_sort (table, 0, TABLE_STATS_CAP - 1);
  statstable_print (table);
}

void
multi_core_run (Arena *a, char *data, size_t data_len)
{
  int batches = omp_get_max_threads ();
  assert (batches > 0);

  StatsTable *batch_res = arena_alloc (a, sizeof (StatsTable) * batches);
  memset (batch_res, 0, sizeof (StatsTable) * batches);

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

      batch_res[i].a = arena_new ();
      process (&batch_res[i], &data[s], e - s);
    }

  StatsTable solution = batch_res[0];
  for (int i = 1; i < batches; i++)
    {
      StatsTable table = batch_res[i];
      for (size_t j = 0; j < TABLE_STATS_CAP; j++)
        {
          Stats *stats = table.stats[j];
          if (stats == NULL)
            continue;

          Stats *update
              = statstable_get (&solution, stats->key, stats->key_len);
          update->sum += stats->sum;
          update->count += stats->count;
          update->max = MAX (update->max, stats->max);
          update->min = MIN (update->min, stats->min);
        }
    }

  statstable_sort (&solution, 0, TABLE_STATS_CAP - 1);
  statstable_print (&solution);
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

  Arena *a = arena_new ();
  // single_core_run (a, data, sb.st_size);
  multi_core_run (a, data, sb.st_size);
}
