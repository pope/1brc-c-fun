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
  unsigned int hash;
  unsigned int key_len;
  char *key;
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
  Station stations[MAX_STATIONS];
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
statstable_get (StatsTable *table, char *key, unsigned int key_len,
                unsigned int hash)
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

      Station *station = &table->stations[table->size];
      station->hash = hash;
      station->key_len = key_len;
      station->key = key;

      table->entries[i].idx = (unsigned int)table->size;
      table->entries[i].hash = hash;
      table->size++;

      assert (table->size < TABLE_STATS_CAP);

      return stat;
    }

#ifndef NDEBUG
  Station *station = table->stations[table->entries[i].idx];
  assert (key_len == station->key_len);
  assert (strncmp (key, station->key, key_len) == 0);
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

static inline size_t
statstable__sort_partition (StatsTable *table, size_t low, size_t high)
{
  assert (table);
  assert (low < high);

  Stats tmpStats = { 0 };
  Station tmpStation = { 0 };
  Station *pivot = &table->stations[high];
  assert (pivot);
  size_t i = low;

  for (size_t j = low; j < high; j++)
    {
      Station *station = &table->stations[j];
      assert (station);
      if (strncmp (station->key, pivot->key,
                   MAX (station->key_len, pivot->key_len))
          <= 0)
        {
          tmpStats = table->stats[i];
          tmpStation = table->stations[i];
          table->stats[i] = table->stats[j];
          table->stations[i] = table->stations[j];
          table->stats[j] = tmpStats;
          table->stations[j] = tmpStation;
          i++;
        }
    }

  tmpStats = table->stats[i];
  tmpStation = table->stations[i];
  table->stats[i] = table->stats[high];
  table->stations[i] = table->stations[high];
  table->stats[high] = tmpStats;
  table->stations[high] = tmpStation;

  return i;
}

static inline void
statstable_sort (StatsTable *table, size_t low, size_t high)
{
  assert (table);

  if (low >= high)
    return;

  while (low < high)
    {
      size_t p = statstable__sort_partition (table, low, high);
      if (p == 0)
        return;
      statstable_sort (table, low, p - 1);
      low = p + 1;
    }
}

static inline StatsTable *
process (char *data, size_t data_len)
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
      unsigned int key_len = 0;
      {
        size_t e = s;
        while (data[e] != ';')
          hash = (hash * HASH_PRIME) + (unsigned char)data[e++];

        assert (e - s < USHRT_MAX);
        assert (e - s < MAX_STATION_NAME_LENGTH);
        key_len = (unsigned int)(e - s);
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
statstable__stats_to_str (char *buf, size_t maxlen, const Station *station,
                          const Stats *stats)
{
  double avg = ((double)stats->sum / (double)stats->count) / 10.0;
  double min = (double)stats->min / 10.0;
  double max = (double)stats->max / 10.0;
  int len = snprintf (buf, maxlen, "%.*s=%.1f/%.1f/%.1f", station->key_len,
                      station->key, min, avg, max);
  assert (len >= 0);
  return MIN (maxlen, (size_t)len);
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
    const Station *station = &table->stations[0];
    const Stats *stats = &table->stats[0];
    size_t n = statstable__stats_to_str (buf, maxlen, station, stats);
    buf += n;
    maxlen -= n;
  }
  for (size_t i = 1; i < table->size; i++)
    {
      const Station *station = &table->stations[i];
      const Stats *stats = &table->stats[i];
      assert (stats != NULL);
      if (maxlen >= 2)
        {
          *buf++ = ',';
          *buf++ = ' ';
          maxlen -= 2;
        }
      size_t n = statstable__stats_to_str (buf, maxlen, station, stats);
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

  assert (buf >= s);
  return (size_t)(buf - s) - (maxlen == 0 ? 1UL : 0UL);
}

int
main (int argc, char **argv)
{
  char output_buf[OUTPUT_BUFSIZE];

#ifndef NO_CHILD_PROCESS
  // From https://github.com/dannyvankooten/1brc/blob/main/analyze.c.
  // Use a child process to do all of the work. The child then sends the data
  // over to the parent to be printed. While the parent is printing, the
  // child is cleaning up it's memory.
  //
  // Trying this using threads with OpenMP - wsystemctl --user
  // import-environment QT_QPA_PLATFORMTHEMEhere one thread prints data and the
  // other cleans up - didn't help. Actually forking did.
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
#endif

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

  int batches = omp_get_max_threads ();
  assert (batches > 0);

  StatsTable **batch_res
      = arena_alloc (a, sizeof (StatsTable *) * (size_t)batches);

#pragma omp parallel for
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
          Station *station = &table->stations[j];
          Stats *stats = &table->stats[j];
          assert (station != NULL);

          Stats *update = statstable_find_by_hash (solution, station->hash);
          if (update == NULL)
            update = statstable_get (solution, station->key, station->key_len,
                                     station->hash);

          update->sum += stats->sum;
          update->count += stats->count;
          update->max = MAX (update->max, stats->max);
          update->min = MIN (update->min, stats->min);
        }
    }

  statstable_sort (solution, 0, solution->size - 1);
  size_t output_buf_len
      = statstable_to_str (output_buf, OUTPUT_BUFSIZE, solution);

#ifndef NO_CHILD_PROCESS
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
#else
  printf ("%s", output_buf);
#endif

  return EXIT_SUCCESS;
}
