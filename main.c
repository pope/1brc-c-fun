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
#include <sys/wait.h>
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
// and since we're trying to be fast, we can cheat with a larger buffer size.
// Still should protect against an overflow.
#define OUTPUT_BUFSIZE (1UL << 21)

// With this value, we have a perfect hash against all of the weather station
// entries.
#define HASH_PRIME 2333

////
// Arena code modified from
// https://github.com/lzace817/examples/tree/master/arena

#define ALIGN_DOWN(n, a) ((n) & ~((a) - 1))
#define ALIGN_UP(n, a) ALIGN_DOWN ((n) + (a) - 1, (a))
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

  int status = mprotect (result, (size_t)cap, PROT_WRITE | PROT_READ);
  if (status == -1)
    {
      perror ("arena");
      exit (EXIT_FAILURE);
    }

  result->data = &result[1];
  result->size = 0;
  result->capacity = (size_t)cap - sizeof (Arena);
  result->mapped_cap = (size_t)cap;

  return result;
}

static inline void *
arena_alloc (struct arena *a, size_t size)
{
  assert (a);
  assert (size > 0);

  size = ALIGN_UP (size, 8);

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
      grow_ammount = ALIGN_UP (grow_ammount, (size_t)cap);
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

typedef struct stats
{
  long sum;
  unsigned int count;
  short min;
  short max;
} Stats;

typedef struct station
{
  // TODO(pope): Write my own sort so that I don't need this
  unsigned int stats_index;
  unsigned int hash;
  char key[];
} Station;

typedef struct statsentry
{
  unsigned int hash;
  unsigned int idx;
} StatsEntry;

typedef struct statstable
{
  Arena *a;
  size_t size;
  StatsEntry entries[TABLE_STATS_CAP];
  Stats stats[MAX_STATIONS];
  Station *stations[MAX_STATIONS];
} StatsTable;

static inline StatsTable *
statstable_alloc (Arena *a)
{
  StatsTable *table = arena_alloc (a, sizeof (StatsTable));
  table->a = a;
  table->size = 0;
  memset (table->entries, 0, sizeof (table->entries));
  memset (table->stats, 0, sizeof (table->stats));
  memset (table->stations, 0, sizeof (table->stations));
  return table;
}

static inline Stats *
statstable_get (StatsTable *restrict table, const char *restrict key,
                unsigned short key_len, unsigned int hash)
{
  assert (table != NULL);
  assert (table->size * 2 < TABLE_STATS_CAP - 1);
  assert (key != NULL);
  assert (key_len > 0);
  assert (hash != 0);

  unsigned int i = hash & (TABLE_STATS_CAP - 1);
  while (table->entries[i].hash != 0 && table->entries[i].hash != hash)
    i = (i + 1) & (TABLE_STATS_CAP - 1);

  // New entry
  if (table->entries[i].hash == 0)
    {
      Stats *stat = &table->stats[table->size];
      stat->max = SHRT_MIN;
      stat->min = SHRT_MAX;
      stat->sum = 0L;
      stat->count = 0U;

      Station *station = arena_alloc (
          table->a, sizeof (Station) + sizeof (char) * (key_len + 1));
      station->stats_index = (unsigned int)table->size;
      station->hash = hash;
      memcpy (station->key, key, key_len);
      station->key[key_len] = 0;

      table->entries[i].idx = (unsigned int)table->size;
      table->entries[i].hash = hash;
      table->stations[table->size] = station;
      table->size++;

      assert (table->size < TABLE_STATS_CAP);

      return stat;
    }

#ifndef NDEBUG
  Station *station = table->stations[table->entries[i].idx];
  assert (key_len == strlen (station->key));
  assert (strncmp (key, station->key, key_len) == 0);
  assert (station->stats_index == table->entries[i].idx);
#endif

  return &table->stats[table->entries[i].idx];
}

static inline Stats *
statstable_find_by_hash (StatsTable *table, unsigned int hash)
{
  assert (table != NULL);
  assert (table->size * 2 < TABLE_STATS_CAP - 1);
  assert (hash != 0);

  unsigned int i = hash & (TABLE_STATS_CAP - 1);
  while (table->entries[i].hash != 0 && table->entries[i].hash != hash)
    i = (i + 1) & (TABLE_STATS_CAP - 1);

  // New entry
  if (table->entries[i].hash == 0)
    return NULL;
  return &table->stats[table->entries[i].idx];
}

static inline int
stations__cmp (const void *aa, const void *bb)
{
  const Station *a = *(Station *const *)aa;
  const Station *b = *(Station *const *)bb;
  int res = strcmp (a->key, b->key);
  assert (res != 0);
  return res;
}

static inline StatsTable *
process (char *restrict data, size_t data_len)
{
  assert (data);
  assert (data_len > 0);

  Arena *a = arena_new ();
  StatsTable *table = statstable_alloc (a);

  size_t s = 0;
  while (s < data_len && data[s] != 0)
    {
      // Get the key and hash, reusing the key buffer. Getting the hash here is
      // one less loop we need to do.
      unsigned int hash = 0;
      char *key = NULL;
      unsigned short key_len = 0;
      {
        size_t e = s;
        while (data[e] != ';')
          hash = (hash * HASH_PRIME) + (unsigned char)data[e++];

        assert (e - s < USHRT_MAX);
        assert (e - s < MAX_STATION_NAME_LENGTH);
        key_len = (unsigned short)(e - s);
        key = &data[s];

        s = e + 1;
      }

      short temp = 0;
      {
        short sign = 1;
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
statstable__stats_to_str (char *restrict buf, size_t maxlen,
                          const Station *restrict station,
                          const Stats *restrict stats)
{
  double avg = ((double)stats->sum / (double)stats->count) / 10.0;
  double min = (double)stats->min / 10.0;
  double max = (double)stats->max / 10.0;
  int len = snprintf (buf, maxlen, "%s=%.1f/%.1f/%.1f", station->key, min, avg,
                      max);
  assert (len >= 0);
  return MIN (maxlen, (size_t)len);
}

static inline size_t
statstable_to_str (char *restrict buf, size_t maxlen,
                   const StatsTable *restrict table)
{
  if (maxlen == 0)
    return 0;

  char *s = buf;
  char *end = buf + maxlen - 1; // leave 1 byte for null terminator

  if (buf < end)
    *buf++ = '{';

  if (table->size > 0)
    {
      Station *station = table->stations[0];
      const Stats *stats = &table->stats[station->stats_index];
      size_t n = statstable__stats_to_str (buf, (size_t)(end - buf), station,
                                           stats);
      buf += n;
    }

  for (size_t i = 1; i < table->size; i++)
    {
      if (buf < end)
        {
          if (end - buf >= 2)
            {
              *buf++ = ',';
              *buf++ = ' ';
            }
          else
            {
              buf = end;
            }
        }
      Station *station = table->stations[i];
      const Stats *stats = &table->stats[station->stats_index];
      size_t n = statstable__stats_to_str (buf, (size_t)(end - buf), station,
                                           stats);
      buf += n;
    }

  if (buf < end)
    {
      if (end - buf >= 2)
        {
          *buf++ = '}';
          *buf++ = '\n';
        }
      else
        {
          buf = end;
        }
    }

  *buf = '\0';
  return (size_t)(buf - s);
}

int
main (int argc, char **argv)
{
  char output_buf[OUTPUT_BUFSIZE];

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

  // Handle empty files early to prevent invalid mmap and batch logic crashes
  if (sb.st_size == 0)
    {
      printf ("{}\n");
      return EXIT_SUCCESS;
    }

  char *data = mmap (NULL, (size_t)sb.st_size, PROT_READ,
                     MAP_PRIVATE | MAP_NORESERVE, fd, 0);
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

  Arena *a = arena_new ();

#if defined(_OPENMP)
  int batches = omp_get_max_threads ();
  assert (batches > 0);
#else
  int batches = 1;
#endif

  // If input size is smaller than batches, limit threads/batches to prevent
  // division by zero or duplication
  if ((size_t)sb.st_size < (size_t)batches)
    batches = (int)sb.st_size;

  StatsTable **batch_res
      = arena_alloc (a, sizeof (StatsTable *) * (size_t)batches);

#pragma omp parallel for num_threads(batches) proc_bind(spread) default(none) \
    shared(batches, sb, data, batch_res)
  for (size_t i = 0; i < (size_t)batches; i++)
    {
      size_t s = i * ((size_t)sb.st_size / (size_t)batches);
      if (!(s == 0 || data[s - 1] == '\n'))
        {
          while (data[s] != '\n')
            s++;
          s++; // consume the newline
        }

      size_t e = MIN ((i + 1) * ((size_t)sb.st_size / (size_t)batches),
                      (size_t)sb.st_size);
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
      for (size_t j = 0; j < table->size; j++)
        {
          Station *station = table->stations[j];
          Stats *stats = &table->stats[j];
          assert (station != NULL);
          assert (station->stats_index == j);

          Stats *update = statstable_find_by_hash (solution, station->hash);
          if (update == NULL)
            update = statstable_get (solution, station->key,
                                     (unsigned short)strlen (station->key),
                                     station->hash);

          update->sum += stats->sum;
          update->count += stats->count;
          update->max = MAX (update->max, stats->max);
          update->min = MIN (update->min, stats->min);
        }
    }

  qsort (solution->stations, solution->size, sizeof (Station *),
         stations__cmp);

  size_t output_buf_len
      = statstable_to_str (output_buf, OUTPUT_BUFSIZE, solution);

  printf ("%.*s", (int)output_buf_len, output_buf);

  return EXIT_SUCCESS;
}
