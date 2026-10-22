/*
 * ny-test: parallel test runner with suite partitioning (benchmark,
 * runtime, native, interop, error, std, repl, probe), caching, timing
 * reports, bench comparison, and failure capture/replay.
 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include "../tools/repo.h"
#include "base/args.h"
#include "base/json.h"
#include "base/util.h"
#include "cmd/fmt/ui.h"
#include "test.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#include <sys/sysinfo.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define NY_TEST_DEFAULT_TIMEOUT_SEC 60
#define NY_TEST_DEFAULT_BENCH_TIMEOUT_SEC 30
#define NY_TEST_MAX_TIMEOUT_SEC 300
#define NY_TEST_PARALLEL_TIMEOUT_GRACE_MS 1000.0
#define NY_TEST_WAIT_HEARTBEAT_MS 45000.0
#define NY_TEST_WORKER_MEMORY_GIB 2.5
#define NY_BENCH_WORKER_MEMORY_GIB 1.5
#define NY_TEST_HOST_MEMORY_RESERVE_GIB 8.0
#define NY_TEST_TIMEOUT_RC 124

#define NY_BENCH_MAX_RUNS 5
#define NY_BENCH_TIMEOUT_SEC NY_TEST_DEFAULT_BENCH_TIMEOUT_SEC
#define NY_BENCH_MAX_OUTPUT 8192

/*
 * Shared across the benchmark worker processes created with fork().
 */
static const char *bench_core_bc_path;

#ifndef _WIN32
/*
 * Each parallel fixture gets a private group so process-control tests cannot
 * signal the ny-test scheduler or unrelated fixtures.
 */
static void ny_test_isolate_child_group(void) {
  (void)setpgid(0, 0);
#ifdef __linux__
  /*
   * Do not leave a fixture compiler running if ny-test itself is killed.
   */
  (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
}

static void ny_test_kill_group(pid_t pid) {
  if (pid <= 0)
    return;
  if (getpgid(pid) == pid)
    (void)kill(-pid, SIGKILL);
  (void)kill(pid, SIGKILL);
}
#endif

typedef struct {
  double c_time_ms;
  double ny_time_ms;
  double ratio;
  double speedup;
  int c_exit;
  int ny_exit;
  int c_runs;
  int ny_runs;
  double c_min;
  double c_max;
  double ny_min;
  double ny_max;
  double c_stddev;
  double ny_stddev;
} BenchResult;

typedef struct {
  const char *name;
  const char *nshape_path;
  const char *c_source;
  const char *ny_source;
  BenchResult result;
} BenchmarkSpec;

static const char *disp_path(const char *p);
static double now_ms(void);
static int bench_full_report;
static void print_section(const char *name);
static uint64_t fnv1a_update(uint64_t h, const void *ptr, size_t n);
static uint64_t test_sig(const char *path, const char *bin,
                         const char *std_path, const char *std_bc);
static int path_lex_cmp(const void *a, const void *b);
static int
run_benchmarks(const char *bin, const char *std_path, const char *std_bc,
               const char *pattern, const char *opt_level, const char *tier,
               const char *engine, const char *target, int runs, int warmup,
               int timeout_sec, int verbose, int show_ir, int show_asm,
               int show_passes, int profile, int compare_llvm,
               int correctness_only, const char *out_csv, const char *out_json,
               const char *out_md, const char *compile_profile, int budget_fail,
               int fixture_workers, StrVec *files, StrVec *patterns);
static void apply_test_child_env(void);
static char *read_small_file(const char *path);
static char *read_whole_file(const char *path);
static const char *test_warn_arg(void);
static void push_test_warn_arg(char **argv, int *argc, int max);
static bool shape_path_is_nshape(const char *p);
static const char *shape_path_split(const char *p, char *file_out,
                                    size_t file_cap, const char **shape_out);
static int shape_block_span(const char *data, size_t data_len, const char *name,
                            size_t *out_start, size_t *out_end);
static char *shape_source_block(const char *shape_path, const char *name);
static char *materialize_shape_ny_source(const char *shape_path);
static void shape_remove_materialized(const char *path);
static int shape_materialize_sibling_dir(const char *root, const char *dir_path,
                                         const char *rel_prefix,
                                         const char *skip_file, int depth);
static int shape_materialize_tree_file(const char *root, const char *shape_file,
                                       const char *source, int depth,
                                       int include_siblings);
static char *shape_meta_string(const char *shape_path, const char *key);
static int shape_expected_exit_code(const char *shape_path);
static int shape_requests_jit(const char *shape_path);
static int split_flag_matrix_rows(char *s, char **out, int max);
static int native_backend_explicit(const char *flags);
static int native_only_explicit(const char *flags);
static int native_backend_is_llvm(const char *flags);
static int flags_contain_native_backend_word(const char *flags,
                                             const char *word);
static int path_is_probe_test(const char *p);
static int path_is_native_test(const char *p);
static int path_is_repl_test(const char *p);
static int path_is_stdlib_source(const char *p);
static int run_progress_selftest(const char *bin, int timeout_sec);
static int run_shape_generator_selftest(void);
static int make_test_capture_tmp(char *tmp, size_t tmp_len, const char *prefix);
static int bench_run_capture(char *const argv[], int timeout_sec,
                             const char *output_path);
static int test_command_available(const char *cmd);
static int ny_test_capture_path_ok(const char *path, char *resolved,
                                   size_t resolved_len);

/*
 * Fixture child memory policy: a single runaway compile must not take the
 * host down, so each fixture child runs under an optional RLIMIT_AS cap and
 * the scheduler evicts groups whose resident set exceeds the configured
 * limit.  Limits come from the environment; absent limits disable the cap.
 */
static double test_mem_limit_gib(void) {
  const char *raw = getenv("NYTRIX_TEST_MEM_LIMIT_GIB");
  if (!raw || !*raw)
    return 0.0;
  double value = atof(raw);
  return value > 0.0 ? value : 0.0;
}

static void apply_test_child_mem_limit(void) {
#ifndef _WIN32
  double gib = test_mem_limit_gib();
  if (gib > 0.0) {
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = (rlim_t)(gib * 1024.0 * 1024.0 * 1024.0);
    setrlimit(RLIMIT_AS, &rl);
  }
#else
  (void)0;
#endif
}

#ifdef __linux__
/*
 * Linux resident set of a process group (leader + direct children), in KiB.
 * Returns -1 when /proc gives us nothing (exited group).
 */
static long test_group_rss_kib(int pid) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  long total = 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      long kib = strtol(line + 6, NULL, 10);
      if (kib > total)
        total = kib;
    }
  }
  fclose(f);
  /*
   * Direct children of the leader (fixture compile helpers).
   */
  snprintf(path, sizeof(path), "/proc/%d/task/%d/children", pid, pid);
  FILE *kids = fopen(path, "r");
  if (kids) {
    long child = -1;
    while (fscanf(kids, "%ld", &child) == 1) {
      char cpath[64];
      snprintf(cpath, sizeof(cpath), "/proc/%ld/status", child);
      FILE *cf = fopen(cpath, "r");
      if (cf) {
        char cline[256];
        while (fgets(cline, sizeof(cline), cf)) {
          if (strncmp(cline, "VmRSS:", 6) == 0) {
            long kib = strtol(cline + 6, NULL, 10);
            if (kib > 0)
              total += kib;
            break;
          }
        }
        fclose(cf);
      }
    }
    fclose(kids);
  }
  return total > 0 ? total : -1;
}
#endif

/*
 * Available host memory in GiB (MemAvailable on Linux); -1 when unknown.
 */
static double test_mem_available_gib(void) {
#ifndef _WIN32
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return -1.0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemAvailable:", 13) == 0) {
      fclose(f);
      return (double)strtol(line + 13, NULL, 10) / (1024.0 * 1024.0);
    }
  }
  fclose(f);
  return -1.0;
#else
  return -1.0;
#endif
}

/*
 * Number of flags_matrix variants a shape fixture executes; rows are
 * ';'-separated inside the quoted value, and a fixture without a matrix
 * runs exactly one variant.
 */
static int test_flag_matrix_count(const char *path) {
  char *matrix = shape_meta_string(path, "flags_matrix");
  if (!matrix)
    return 1;
  char *rows[64];
  int count = split_flag_matrix_rows(matrix, rows, 64);
  free(matrix);
  return count > 0 ? count : 1;
}

static int run_debug_argv_capture(char *const argv[], int timeout_sec,
                                  int use_path_lookup, const char *output_path);

typedef struct {
  FILE *stream;
  char path[PATH_MAX];
  int saved_stdout;
  int saved_stderr;
} FailureOutputCapture;

static int test_fd_dup(int fd) {
#ifdef _WIN32
  return _dup(fd);
#else
  return dup(fd);
#endif
}

static int test_fd_dup2(int from, int to) {
#ifdef _WIN32
  return _dup2(from, to);
#else
  return dup2(from, to);
#endif
}

static void test_fd_close(int fd) {
#ifdef _WIN32
  _close(fd);
#else
  close(fd);
#endif
}

static int failure_output_capture_begin(FailureOutputCapture *capture) {
  if (!capture)
    return 0;
  memset(capture, 0, sizeof(*capture));
  capture->saved_stdout = -1;
  capture->saved_stderr = -1;
  int fd =
      make_test_capture_tmp(capture->path, sizeof(capture->path), "failures");
#ifndef _WIN32
  if (fd >= 0)
    close(fd);
#else
  (void)fd;
#endif
  capture->stream = fopen(capture->path, "w+b");
  if (!capture->stream)
    return 0;
  fflush(stdout);
  fflush(stderr);
  capture->saved_stdout = test_fd_dup(fileno(stdout));
  capture->saved_stderr = test_fd_dup(fileno(stderr));
  int stdout_redirected = 0;
  int stderr_redirected = 0;
  if (capture->saved_stdout >= 0 && capture->saved_stderr >= 0) {
    stdout_redirected =
        test_fd_dup2(fileno(capture->stream), fileno(stdout)) >= 0;
    if (stdout_redirected)
      stderr_redirected =
          test_fd_dup2(fileno(capture->stream), fileno(stderr)) >= 0;
  }
  if (!stdout_redirected || !stderr_redirected) {
    if (stdout_redirected && capture->saved_stdout >= 0)
      test_fd_dup2(capture->saved_stdout, fileno(stdout));
    if (stderr_redirected && capture->saved_stderr >= 0)
      test_fd_dup2(capture->saved_stderr, fileno(stderr));
    if (capture->saved_stdout >= 0)
      test_fd_close(capture->saved_stdout);
    if (capture->saved_stderr >= 0)
      test_fd_close(capture->saved_stderr);
    fclose(capture->stream);
    remove(capture->path);
    memset(capture, 0, sizeof(*capture));
    capture->saved_stdout = capture->saved_stderr = -1;
    return 0;
  }
  return 1;
}

static int failure_marker_line(const char *line) {
  return line && (strstr(line, "[✗/✗/✗]") || strstr(line, "[x/x/x]"));
}

static int failure_summary_line(const char *line) {
  if (!line)
    return 0;
  return strncmp(line, "Total:", 6) == 0 ||
         strncmp(line, "Peak fixture RSS:", 18) == 0 ||
         strstr(line, "Failure Replay") != NULL ||
         strstr(line, "failure(s)") != NULL ||
         strstr(line, "replaying ") != NULL ||
         strstr(line, "[debug] replaying") != NULL ||
         strstr(line, "[debug] diagnostic artifacts:") != NULL ||
         strstr(line, "[debug] core_pattern=") != NULL ||
         strstr(line, "no debugger found") != NULL ||
         strncmp(line, "  [trace]", 9) == 0 ||
         strncmp(line, "  [debugger]", 12) == 0 ||
         strncmp(line, "    trace:", 10) == 0 ||
         strncmp(line, "    artifacts:", 14) == 0 ||
         strncmp(line, "    emit:", 9) == 0 ||
         strstr(line, "[test] killed") != NULL ||
         strstr(line, "[wait] ") != NULL;
}

static void failure_output_capture_end(FailureOutputCapture *capture) {
  if (!capture->stream)
    return;
  fflush(stdout);
  fflush(stderr);
  test_fd_dup2(capture->saved_stdout, fileno(stdout));
  test_fd_dup2(capture->saved_stderr, fileno(stderr));
  test_fd_close(capture->saved_stdout);
  test_fd_close(capture->saved_stderr);
  rewind(capture->stream);

  char line[8192];
  int capturing = 0;
  while (fgets(line, sizeof(line), capture->stream)) {
    if (strncmp(line, "[replay output]", 15) == 0) {
      capturing = 1;
      continue;
    }
    if (capturing && !failure_marker_line(line))
      continue;
    if (failure_marker_line(line)) {
      fputs(line, stdout);
      capturing = 0;
      continue;
    }
    if (failure_summary_line(line))
      fputs(line, stdout);
  }
  fclose(capture->stream);
  remove(capture->path);
  capture->stream = NULL;
}
typedef struct {
  int tests;
  int passed;
  int sum_ms;
  int max_ms;
} SuiteStats;

typedef struct {
  char *path;
  int ms;
  const char *suite;
  const char *status;
} TimingRow;

typedef struct {
  TimingRow *items;
  size_t len;
  size_t cap;
} TimingVec;

static int timing_row_cmp_desc(const void *a, const void *b) {
  const TimingRow *ta = (const TimingRow *)a;
  const TimingRow *tb = (const TimingRow *)b;
  if (tb->ms == ta->ms)
    return 0;
  return (tb->ms > ta->ms) ? 1 : -1;
}

typedef struct {
  char *path;
  uint64_t sig;
  int ok;
  int dur_ms;
} CacheRow;

typedef struct {
  CacheRow *items;
  size_t len;
  size_t cap;
  size_t *ht_slot;
  size_t ht_cap;
  size_t ht_len;
} CacheDb;

static void cache_save(CacheDb *db, const char *path);

static uint32_t cache_hash_path(const char *s) { return ny_hash32_cstr(s); }

static void cache_ht_rehash(CacheDb *db) {
  size_t nc = db->ht_cap ? db->ht_cap * 2 : 512;
  while (nc <= db->len * 2 && nc <= SIZE_MAX / 2)
    nc *= 2;
  size_t *slots = malloc(nc * sizeof(*slots));
  if (!slots)
    return;
  for (size_t i = 0; i < nc; ++i)
    slots[i] = SIZE_MAX;
  size_t mask = nc - 1;
  for (size_t item = 0; item < db->len; ++item) {
    size_t pos = cache_hash_path(db->items[item].path) & mask;
    for (size_t probe = 0; probe < nc; ++probe) {
      if (slots[pos] == SIZE_MAX) {
        slots[pos] = item;
        break;
      }
      pos = (pos + 1) & mask;
    }
  }
  free(db->ht_slot);
  db->ht_slot = slots;
  db->ht_cap = nc;
  db->ht_len = db->len;
}

static void cache_ht_ensure(CacheDb *db) {
  if (!db->ht_slot || (db->ht_len + 1) * 2 >= db->ht_cap)
    cache_ht_rehash(db);
}

static size_t cache_ht_find(const CacheDb *db, const char *path) {
  if (!db->ht_slot || !db->ht_cap)
    return SIZE_MAX;
  size_t mask = db->ht_cap - 1;
  size_t pos = cache_hash_path(path) & mask;
  for (size_t probe = 0; probe < db->ht_cap; ++probe) {
    size_t item = db->ht_slot[pos];
    if (item == SIZE_MAX)
      return SIZE_MAX;
    if (item < db->len && strcmp(db->items[item].path, path) == 0)
      return item;
    pos = (pos + 1) & mask;
  }
  return SIZE_MAX;
}

static void cache_ht_insert(CacheDb *db, size_t item) {
  if (!db->ht_slot || item >= db->len)
    return;
  size_t mask = db->ht_cap - 1;
  size_t pos = cache_hash_path(db->items[item].path) & mask;
  for (size_t probe = 0; probe < db->ht_cap; ++probe) {
    if (db->ht_slot[pos] == SIZE_MAX) {
      db->ht_slot[pos] = item;
      db->ht_len++;
      return;
    }
    pos = (pos + 1) & mask;
  }
}

static void timings_push(TimingVec *v, const char *path, int ms,
                         const char *suite, const char *status) {
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 64;
    TimingRow *p = (TimingRow *)realloc(v->items, nc * sizeof(TimingRow));
    if (!p)
      return;
    v->items = p;
    v->cap = nc;
  }
  v->items[v->len].path = strdup(path ? path : "");
  v->items[v->len].ms = ms;
  v->items[v->len].suite = suite;
  v->items[v->len].status = status ? status : "pass";
  if (v->items[v->len].path)
    v->len++;
}

static void timings_free(TimingVec *v) {
  for (size_t i = 0; i < v->len; i++)
    free(v->items[i].path);
  free(v->items);
}

static void sv_push_unique(StrVec *v, const char *s) {
  if (!v || !s)
    return;
  for (size_t i = 0; i < v->len; i++) {
    if (strcmp(v->items[i], s) == 0)
      return;
  }
  sv_push(v, s);
}

static void cache_set(CacheDb *db, const char *path, uint64_t sig, int ok,
                      int dur_ms) {
  cache_ht_ensure(db);
  size_t existing = cache_ht_find(db, path);
  if (existing != SIZE_MAX) {
    db->items[existing].sig = sig;
    db->items[existing].ok = ok;
    db->items[existing].dur_ms = dur_ms;
    return;
  }
  if (db->len == db->cap) {
    size_t nc = db->cap ? db->cap * 2 : 256;
    CacheRow *p = (CacheRow *)realloc(db->items, nc * sizeof(CacheRow));
    if (!p)
      return;
    db->items = p;
    db->cap = nc;
  }
  db->items[db->len].path = strdup(path ? path : "");
  if (!db->items[db->len].path)
    return;
  db->items[db->len].sig = sig;
  db->items[db->len].ok = ok;
  db->items[db->len].dur_ms = dur_ms;
  db->len++;
  cache_ht_insert(db, db->len - 1);
}

static CacheRow *cache_find(CacheDb *db, const char *path) {
  cache_ht_ensure(db);
  size_t item = cache_ht_find(db, path);
  return item == SIZE_MAX ? NULL : &db->items[item];
}

static void cache_free(CacheDb *db) {
  for (size_t i = 0; i < db->len; i++)
    free(db->items[i].path);
  free(db->items);
  free(db->ht_slot);
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * Test paths are allowed to be materialized under a temporary root.  Keep
 * suite selection independent of that root so reorganizing etc/tests does not
 * silently turn explicit fixtures into an unselected stdlib bucket.
 */
static int test_path_in(const char *path, const char *root) {
  if (!path || !root)
    return 0;
  const char *p = strstr(path, "etc/tests/");
  if (!p)
    return 0;
  p += strlen("etc/tests/");
  return strncmp(p, root, strlen(root)) == 0;
}

typedef enum {
  TEST_DOMAIN_AUTO,
  TEST_DOMAIN_BENCHMARK,
  TEST_DOMAIN_RUNTIME,
  TEST_DOMAIN_NATIVE,
  TEST_DOMAIN_INTEROP,
  TEST_DOMAIN_ERROR,
  TEST_DOMAIN_PROBE,
  TEST_DOMAIN_REPL,
  TEST_DOMAIN_STDLIB
} TestDomain;

/*
 * Fixture metadata is the routing contract; paths are only a fallback for
 * legacy fixtures. `suite` is explicit, while `family` preserves existing
 * category metadata without forcing duplicate suite declarations everywhere.
 */
static TestDomain test_metadata_domain(const char *path) {
  if (!shape_path_is_nshape(path))
    return TEST_DOMAIN_AUTO;
  char *suite = shape_meta_string(path, "suite");
  char *family = suite ? NULL : shape_meta_string(path, "family");
  const char *value = suite ? suite : family;
  TestDomain domain = TEST_DOMAIN_AUTO;
  if (value) {
    if (strcmp(value, "benchmark") == 0 ||
        strncmp(value, "benchmark/", 10) == 0)
      domain = TEST_DOMAIN_BENCHMARK;
    else if (strcmp(value, "runtime") == 0 ||
             strncmp(value, "runtime/", 8) == 0)
      domain = TEST_DOMAIN_RUNTIME;
    else if (strcmp(value, "interop") == 0 ||
             strncmp(value, "interop/", 8) == 0 ||
             strncmp(value, "compiler/interop/", 17) == 0)
      domain = TEST_DOMAIN_INTEROP;
    else if (strcmp(value, "error") == 0 ||
             strncmp(value, "compiler-errors/", 16) == 0 ||
             strncmp(value, "verification/errors/", 20) == 0)
      domain = TEST_DOMAIN_ERROR;
    else if (strcmp(value, "probe") == 0 || strcmp(value, "probe-suite") == 0 ||
             strncmp(value, "probe-", 6) == 0 ||
             strncmp(value, "verification/probes/", 20) == 0)
      domain = TEST_DOMAIN_PROBE;
    else if (strcmp(value, "repl") == 0 || strncmp(value, "repl/", 5) == 0 ||
             strstr(value, "/repl") != NULL)
      domain = TEST_DOMAIN_REPL;
    else if (strcmp(value, "stdlib") == 0)
      domain = TEST_DOMAIN_STDLIB;
    else if (strcmp(value, "native") == 0 ||
             strncmp(value, "native-", 7) == 0 ||
             strcmp(value, "runtime-native") == 0 ||
             strcmp(value, "runtime-sanitize") == 0 ||
             strncmp(value, "compiler/", 9) == 0 ||
             strncmp(value, "verification/oracle/", 20) == 0)
      domain = TEST_DOMAIN_NATIVE;
  }
  free(suite);
  free(family);
  return domain;
}

static int test_path_is_language_domain(const char *path) {
  TestDomain domain = test_metadata_domain(path);
  if (domain == TEST_DOMAIN_ERROR || domain == TEST_DOMAIN_RUNTIME)
    return 1;
  return test_path_in(path, "lang/errors/") ||
         (test_path_in(path, "lang/") && !test_path_in(path, "lang/interop/"));
}

static int test_path_is_runtime_domain(const char *path) {
  TestDomain domain = test_metadata_domain(path);
  return domain == TEST_DOMAIN_RUNTIME ||
         test_path_in(path, "lang/concurrency/") ||
         test_path_in(path, "lang/execution/") ||
         test_path_in(path, "lang/memory/") ||
         test_path_in(path, "lang/values/");
}

static int test_path_is_benchmark(const char *path) {
  return test_metadata_domain(path) == TEST_DOMAIN_BENCHMARK ||
         test_path_in(path, "bench/");
}

/*
 * Collect every top-level `shape X {` block name in an .nshape file.
 * Returns the names in the order they appear, or 0 when the file has no
 * named shapes (single implicit shape / plain nshape).
 */
static size_t shape_names_in_file(const char *path, StrVec *names) {
  if (!path || !names)
    return 0;
  char *data = read_small_file(path);
  if (!data)
    return 0;
  size_t data_len = strlen(data);
  size_t count = 0;
  const char *end = data + data_len;
  const char *line = data;
  bool in_heredoc = false;
  char heredoc_marker[64] = {0};
  size_t heredoc_len = 0;
  while (line < end) {
    const char *line_end = memchr(line, '\n', (size_t)(end - line));
    if (!line_end)
      line_end = end;
    const char *s = line;
    while (s < line_end && (*s == ' ' || *s == '\t' || *s == '\r'))
      s++;

    if (in_heredoc) {
      const char *trim_end = line_end;
      while (trim_end > s && (trim_end[-1] == ' ' || trim_end[-1] == '\t' ||
                              trim_end[-1] == '\r'))
        trim_end--;
      if ((size_t)(trim_end - s) == heredoc_len &&
          memcmp(s, heredoc_marker, heredoc_len) == 0) {
        in_heredoc = false;
        heredoc_marker[0] = '\0';
        heredoc_len = 0;
      }
      line = line_end < end ? line_end + 1 : end;
      continue;
    }

    /*
     * Skip embedded source, where ordinary Ny declarations may also use
     * the word `shape` (for example, a struct declaration).
     */
    if ((size_t)(line_end - s) >= 6 && memcmp(s, "source", 6) == 0 &&
        (s + 6 == line_end || isspace((unsigned char)s[6]))) {
      const char *lt = ny_memmem(s, (size_t)(line_end - s), "<<", 2);
      if (lt) {
        const char *m = lt + 2;
        while (m < line_end && (*m == ' ' || *m == '\t'))
          m++;
        char quote = 0;
        if (m < line_end && (*m == '\'' || *m == '"'))
          quote = *m++;
        const char *ms = m;
        while (
            m < line_end &&
            (quote ? *m != quote : (*m != ' ' && *m != '\t' && *m != '\r'))) {
          m++;
        }
        if (m > ms && (size_t)(m - ms) < sizeof(heredoc_marker)) {
          heredoc_len = (size_t)(m - ms);
          memmove(heredoc_marker, ms, heredoc_len);
          heredoc_marker[heredoc_len] = '\0';
          in_heredoc = true;
        }
      }
      line = line_end < end ? line_end + 1 : end;
      continue;
    }

    if ((size_t)(line_end - s) >= 6 && memcmp(s, "shape ", 6) == 0) {
      const char *q = s + 6;
      const char *ns = q;
      while (q < line_end &&
             (isalnum((unsigned char)*q) || *q == '_' || *q == '-'))
        q++;
      if (q > ns) {
        size_t name_len = (size_t)(q - ns);
        while (q < line_end && (*q == ' ' || *q == '\t'))
          q++;
        if (q < line_end && *q == '{') {
          char *name = (char *)malloc(name_len + 1);
          if (name) {
            memmove(name, ns, name_len);
            name[name_len] = '\0';
            sv_push(names, name);
            count++;
          }
        }
      }
    }
    line = line_end < end ? line_end + 1 : end;
  }
  free(data);
  return count;
}

/*
 * Expand discovered .nshape files into per-shape test entries.  A file with
 * more than one `shape X {` block becomes one entry per shape, addressed as
 * `path:shape`; single-shape and .ny files stay untouched.
 */
static void expand_multi_shape_files(StrVec *files) {
  StrVec out = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    if (!nyt_ends_with(p, ".nshape")) {
      sv_push(&out, p);
      continue;
    }
    StrVec names = {0};
    size_t n = shape_names_in_file(p, &names);
    if (n <= 1) {
      sv_free(&names);
      sv_push(&out, p);
      continue;
    }
    for (size_t k = 0; k < names.len; k++) {
      size_t entry_len = strlen(p) + strlen(names.items[k]) + 2;
      char *entry = (char *)malloc(entry_len);
      if (entry) {
        snprintf(entry, entry_len, "%s:%s", p, names.items[k]);
        sv_push(&out, entry);
      }
    }
    sv_free(&names);
  }
  sv_free(files);
  *files = out;
}

#ifdef _WIN32
typedef HANDLE ny_test_proc_t;
#define NY_TEST_PROC_INVALID NULL

static int ny_test_proc_valid(ny_test_proc_t p) { return p != NULL; }
static int ny_test_proc_eq(ny_test_proc_t a, ny_test_proc_t b) {
  return a == b;
}
static void ny_test_proc_close(ny_test_proc_t p) {
  if (p)
    CloseHandle(p);
}

static int ny_cmd_append(char **buf, size_t *len, size_t *cap, const char *s,
                         size_t n) {
  if (*len + n + 1 > *cap) {
    size_t nc = *cap ? *cap * 2 : 256;
    while (*len + n + 1 > nc)
      nc *= 2;
    char *p = (char *)realloc(*buf, nc);
    if (!p)
      return 0;
    *buf = p;
    *cap = nc;
  }
  memmove(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static int ny_cmd_append_char(char **buf, size_t *len, size_t *cap, char c) {
  return ny_cmd_append(buf, len, cap, &c, 1);
}

static int ny_cmd_append_arg(char **buf, size_t *len, size_t *cap,
                             const char *arg) {
  const char *s = arg ? arg : "";
  int quote = *s == '\0';
  for (const char *p = s; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '"') {
      quote = 1;
      break;
    }
  }
  if (*len > 0 && !ny_cmd_append_char(buf, len, cap, ' '))
    return 0;
  if (!quote)
    return ny_cmd_append(buf, len, cap, s, strlen(s));
  if (!ny_cmd_append_char(buf, len, cap, '"'))
    return 0;
  size_t slashes = 0;
  for (const char *p = s; *p; p++) {
    if (*p == '\\') {
      slashes++;
      continue;
    }
    if (*p == '"') {
      for (size_t i = 0; i < slashes * 2 + 1; i++)
        if (!ny_cmd_append_char(buf, len, cap, '\\'))
          return 0;
      slashes = 0;
      if (!ny_cmd_append_char(buf, len, cap, '"'))
        return 0;
      continue;
    }
    while (slashes > 0) {
      if (!ny_cmd_append_char(buf, len, cap, '\\'))
        return 0;
      slashes--;
    }
    if (!ny_cmd_append_char(buf, len, cap, *p))
      return 0;
  }
  for (size_t i = 0; i < slashes * 2; i++)
    if (!ny_cmd_append_char(buf, len, cap, '\\'))
      return 0;
  return ny_cmd_append_char(buf, len, cap, '"');
}

static char *ny_test_build_cmdline(char *const argv[]) {
  char *cmd = NULL;
  size_t len = 0, cap = 0;
  for (int i = 0; argv && argv[i]; i++) {
    if (!ny_cmd_append_arg(&cmd, &len, &cap, argv[i])) {
      free(cmd);
      return NULL;
    }
  }
  return cmd ? cmd : strdup("");
}

#ifdef _WIN32
static int ny_test_path_has_sep(const char *path) {
  return path && (strchr(path, '/') || strchr(path, '\\'));
}

static int ny_test_file_exists(const char *path) {
  DWORD attr = GetFileAttributesA(path);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static const char *ny_test_resolve_app(char *arg0, char *buf, size_t buf_sz) {
  if (!arg0 || !*arg0 || !ny_test_path_has_sep(arg0))
    return NULL;
  snprintf(buf, buf_sz, "%s", arg0);
  if (ny_test_file_exists(buf))
    return buf;
  const char *slash = strrchr(arg0, '/');
  const char *backslash = strrchr(arg0, '\\');
  const char *base = slash;
  if (!base || (backslash && backslash > base))
    base = backslash;
  base = base ? base + 1 : arg0;
  if (!strchr(base, '.')) {
    snprintf(buf, buf_sz, "%s.exe", arg0);
    if (ny_test_file_exists(buf))
      return buf;
  }
  return NULL;
}
#endif

static ny_test_proc_t ny_test_spawn_argv(char *const argv[],
                                         const char *output_path, int quiet) {
  char app_buf[PATH_MAX];
  const char *app =
      ny_test_resolve_app(argv ? argv[0] : NULL, app_buf, sizeof(app_buf));
  char *cmd = ny_test_build_cmdline(argv);
  if (!cmd)
    return NY_TEST_PROC_INVALID;
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  memset(&pi, 0, sizeof(pi));
  si.cb = sizeof(si);
  SECURITY_ATTRIBUTES sa;
  memset(&sa, 0, sizeof(sa));
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE out = NULL;
  BOOL inherit = FALSE;
  if (output_path || quiet) {
    const char *path = output_path ? output_path : "NUL";
    DWORD disposition = output_path ? CREATE_ALWAYS : OPEN_EXISTING;
    out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      &sa, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
      if (output_path) {
        free(cmd);
        return NY_TEST_PROC_INVALID;
      }
      out = NULL;
    } else {
      si.dwFlags |= STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = out;
      si.hStdError = out;
      inherit = TRUE;
    }
  }
  apply_test_child_env();
  BOOL ok =
      CreateProcessA(app, cmd, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi);
  free(cmd);
  if (out)
    CloseHandle(out);
  if (!ok)
    return NY_TEST_PROC_INVALID;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

static int ny_test_wait_rc(ny_test_proc_t proc, int timeout_sec,
                           int *timed_out) {
  if (timed_out)
    *timed_out = 0;
  DWORD wait_ms = timeout_sec > 0 ? (DWORD)timeout_sec * 1000u : INFINITE;
  DWORD wr = WaitForSingleObject(proc, wait_ms);
  if (wr == WAIT_TIMEOUT) {
    if (timed_out)
      *timed_out = 1;
    TerminateProcess(proc, NY_TEST_TIMEOUT_RC);
    WaitForSingleObject(proc, INFINITE);
    return NY_TEST_TIMEOUT_RC;
  }
  if (wr != WAIT_OBJECT_0)
    return 127;
  DWORD code = 127;
  if (!GetExitCodeProcess(proc, &code))
    return 127;
  return (int)code;
}

static int ny_test_poll_done(ny_test_proc_t proc, int *status) {
  DWORD code = STILL_ACTIVE;
  if (!GetExitCodeProcess(proc, &code))
    return -1;
  if (code == STILL_ACTIVE)
    return 0;
  if (status)
    *status = (int)code;
  return 1;
}

#else
typedef pid_t ny_test_proc_t;
#define NY_TEST_PROC_INVALID ((pid_t) - 1)

static int ny_test_proc_valid(ny_test_proc_t p) { return p > 0; }
static int ny_test_proc_eq(ny_test_proc_t a, ny_test_proc_t b) {
  return a == b;
}
static void ny_test_proc_close(ny_test_proc_t p) { (void)p; }
#endif
static void trim_inplace(char *s);
static void error_meta_free(char *flags, char *expect);
static void read_error_meta(const char *path, char **flags_out,
                            char **expect_out);
static int split_words(char *s, char **out, int max);
static char *read_small_file(const char *path);
static int run_debug_argv(char *const argv[], int timeout_sec,
                          int use_path_lookup);
static int test_env_truthy(const char *name);

static bool test_collect_native(const char *root, StrVec *out,
                                bool benchmarks_only);

#include "elf.c"

static int test_env_truthy(const char *name) {
  return ny_env_is_truthy(getenv(name)) ? 1 : 0;
}

static int test_env_falsey(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return 0;
  return strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
         strcmp(v, "off") == 0 || strcmp(v, "no") == 0;
}

static void test_setenv_default(const char *name, const char *value) {
  if (!getenv(name))
    ny_setenv(name, value, 0);
}

static int test_ascii_symbols(void) { return ny_ui_ascii(); }

static const char *test_symbol(const char *sym) {
  if (!test_ascii_symbols() || !sym)
    return sym ? sym : "-";
  if (strcmp(sym, "✓") == 0)
    return "+";
  if (strcmp(sym, "✗") == 0)
    return "x";
  return sym;
}

static int make_test_capture_tmp(char *tmp, size_t tmp_len,
                                 const char *prefix) {
  if (!tmp || tmp_len == 0)
    return -1;
#ifdef _WIN32
  char tmp_dir[PATH_MAX];
  DWORD tmp_dir_len = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
  if (tmp_dir_len == 0 || tmp_dir_len >= sizeof(tmp_dir))
    snprintf(tmp_dir, sizeof(tmp_dir), ".\\");
  static volatile LONG tmp_seq = 0;
  LONG seq = InterlockedIncrement((volatile LONG *)&tmp_seq);
  snprintf(tmp, tmp_len, "%sny-%s-%lu-%lu-%ld.log", tmp_dir,
           prefix ? prefix : "test", (unsigned long)GetCurrentProcessId(),
           (unsigned long)GetTickCount(), (long)seq);
  return 0;
#else
  snprintf(tmp, tmp_len, "%s/ny-%s-%ld-XXXXXX", nyt_temp_dir(),
           prefix ? prefix : "test", (long)getpid());
  return mkstemp(tmp);
#endif
}

/*
 * Create a pathname for a child process to open, without keeping the
 * mkstemp descriptor in the long-lived test/benchmark runner.
 */
static int make_test_capture_path(char *tmp, size_t tmp_len,
                                  const char *prefix) {
  int fd = make_test_capture_tmp(tmp, tmp_len, prefix);
  if (fd < 0)
    return -1;
#ifndef _WIN32
  close(fd);
#endif
  return 0;
}

static void print_captured_test_output(const char *label, const char *path,
                                       const char *tmp) {
  if (!tmp || !*tmp)
    return;
  char *out = read_small_file(tmp);
  if (!out || !*out) {
    free(out);
    return;
  }
  printf("%s[%s]%s %s\n", nyt_clr(NYT_GRAY), label ? label : "test output",
         nyt_clr(NYT_RESET), disp_path(path));
  /*
   * Replay content: style the plain-fprintf failure lines (✗ results,
   * verify repros) the compiler emits without color support.
   */
  if (label && strncmp(label, "replay output", 13) == 0 && color_enabled()) {
    char *rp = out;
    while (rp && *rp) {
      char *nl = strchr(rp, '\n');
      size_t ll = nl ? (size_t)(nl - rp) : strlen(rp);
      const char *lc = "";
      if (ll >= 3 && strncmp(rp, "\xe2\x9c\x97", 3) == 0)
        lc = "\033[1;31m";
      else if (ll >= 12 && strncmp(rp, "native NYIR:", 12) == 0)
        lc = "\033[33m";
      else if (ll >= 13 && strncmp(rp, "nyir <failed>", 13) == 0)
        lc = "\033[33m";
      fprintf(stdout, "%.*s%s\n", (int)ll, rp, lc[0] ? "\033[0m" : "");
      if (!nl)
        break;
      rp = nl + 1;
    }
    free(out);
    return;
  }
  fputs(out, stdout);
  size_t n = strlen(out);
  if (n == 0 || out[n - 1] != '\n')
    putchar('\n');
  free(out);
}

typedef struct {
  char headline[1024];
  int headline_score;
  char first_frame[256];
  char last_frame[256];
  int frame_count;
} CompactDebugSummary;

static void compact_debug_plain(char *line) {
  if (!line)
    return;
  char *r = line;
  char *w = line;
  while (*r) {
    if ((unsigned char)r[0] == 0x1b && r[1] == '[') {
      r += 2;
      while (*r && !isalpha((unsigned char)*r) && *r != '@')
        r++;
      if (*r)
        r++;
      continue;
    }
    *w++ = *r++;
  }
  *w = '\0';
  trim_inplace(line);
}

static int compact_debug_headline_score(const char *line) {
  if (!line || !*line)
    return 0;
  if (strstr(line, "SegmentationFault") || strstr(line, "Segmentation fault"))
    return 100;
  if (strstr(line, "AbortError"))
    return 95;
  if (strstr(line, "PanicError"))
    return 90;
  if (strstr(line, "Nytrix assertion failed") ||
      strstr(line, "Assertion failed"))
    return 88;
  if (strstr(line, "timeout") || strstr(line, "timed out"))
    return 82;
  if (strstr(line, "Error:"))
    return 75;
  return 0;
}

static void compact_debug_frame_name(const char *line, char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  if (!line)
    return;
  const char *name = strstr(line, " in ");
  name = name ? name + 4 : line;
  snprintf(out, cap, "%s", name);
  if (strlen(out) > 220) {
    out[217] = '.';
    out[218] = '.';
    out[219] = '.';
    out[220] = '\0';
  }
}

static void compact_debug_note_line(CompactDebugSummary *summary,
                                    const char *input) {
  if (!summary || !input)
    return;
  char line[8192];
  snprintf(line, sizeof(line), "%s", input);
  compact_debug_plain(line);
  if (!*line)
    return;

  int repeated = 0;
  if (sscanf(line, "... previous frame repeated %d", &repeated) == 1) {
    if (repeated > 0)
      summary->frame_count += repeated;
    return;
  }
  if (strncmp(line, "at ", 3) == 0) {
    char frame[256];
    compact_debug_frame_name(line, frame, sizeof(frame));
    if (!summary->first_frame[0])
      snprintf(summary->first_frame, sizeof(summary->first_frame), "%s", frame);
    snprintf(summary->last_frame, sizeof(summary->last_frame), "%s", frame);
    summary->frame_count++;
    return;
  }

  int score = compact_debug_headline_score(line);
  if (score > summary->headline_score) {
    summary->headline_score = score;
    snprintf(summary->headline, sizeof(summary->headline), "%s", line);
  }
}

static void compact_debug_print_summary(const char *label, const char *path,
                                        const char *tmp, int rc) {
  CompactDebugSummary summary;
  memset(&summary, 0, sizeof(summary));
  if (tmp && *tmp) {
    char *out = read_small_file(tmp);
    if (out) {
      char *save = NULL;
      for (char *line = strtok_r(out, "\n", &save); line;
           line = strtok_r(NULL, "\n", &save))
        compact_debug_note_line(&summary, line);
      free(out);
    }
  }
  printf("  %s[%s]%s %s · %s=%d", nyt_clr(NYT_GRAY), label ? label : "replay",
         nyt_clr(NYT_RESET), disp_path(path),
         label && strcmp(label, "debugger") == 0 ? "exit" : "rc", rc);
  if (summary.headline[0])
    printf(" · %s", summary.headline);
  fputc('\n', stdout);
  if (summary.frame_count > 0) {
    printf("    trace: %s", summary.first_frame);
    if (summary.frame_count > 2) {
      printf(" → … → %s", summary.last_frame);
    } else if (summary.frame_count > 1 &&
               strcmp(summary.first_frame, summary.last_frame) != 0) {
      printf(" → %s", summary.last_frame);
    }
    if (summary.frame_count > 1)
      printf(" (%d frames)", summary.frame_count);
    fputc('\n', stdout);
  }
}

static void format_test_time(char *buf, size_t cap, int dur_ms) {
  if (!buf || cap == 0)
    return;
  if (dur_ms < 0)
    snprintf(buf, cap, "cache");
  else
    snprintf(buf, cap, "%dms", dur_ms);
}

static void print_test_progress_line(int pct, const char *a,
                                     const char *a_color, const char *b,
                                     const char *b_color, const char *c,
                                     const char *c_color,
                                     const char *time_label, const char *path,
                                     const char *suffix) {
  const char *aa = test_symbol(a);
  const char *bb = test_symbol(b);
  const char *cc = test_symbol(c);
  /*
   * One compact row per fixture in a scrolling log: percent, the three
   * route marks, a short duration (or "cached" for result-cache hits),
   * then the fixture path.  No per-line bar — progress is already the
   * percent; a bar per log line is noise, not information.
   */
  printf("%s%3d%%%s [%s%s%s/%s%s%s/%s%s%s] ", nyt_clr(NYT_GRAY), pct,
         nyt_clr(NYT_RESET), nyt_clr(a_color ? a_color : NYT_GRAY), aa,
         nyt_clr(NYT_RESET), nyt_clr(b_color ? b_color : NYT_GRAY), bb,
         nyt_clr(NYT_RESET), nyt_clr(c_color ? c_color : NYT_GRAY), cc,
         nyt_clr(NYT_RESET));
  if (time_label && *time_label)
    printf("%s%7s%s ", nyt_clr(NYT_GRAY), time_label, nyt_clr(NYT_RESET));
  else
    printf("%scached%s ", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("%s%s%s", nyt_clr(NYT_GRAY), disp_path(path), nyt_clr(NYT_RESET));
  if (suffix && *suffix)
    printf(" %s", suffix);
  fputc('\n', stdout);
  fflush(stdout);
}

/*
 * Compute the ny-full binary that sits beside the resolved --bin launcher
 * and export it as NYTRIX_NY_FULL_BIN so the "ny" launcher execs that exact
 * path directly for every fixture, instead of re-deriving it per-process
 * from argv[0]/proc-self-exe (which can fail for a materialized/relocated
 * fixture path) or falling through to a PATH search that may resolve to an
 * unrelated ny-full on the host. This is set once, from the same --bin the
 * runner was actually configured with, and inherited by every forked
 * fixture child. A test-run snapshot overrides the sibling path so a
 * concurrent build cannot replace the executable while fixtures are using it.
 */
static void export_ny_full_bin_for(const char *bin) {
  const char *snapshot = getenv("NYTRIX_TEST_NY_FULL_BIN");
  if (snapshot && *snapshot && ny_access(snapshot, 0) == 0) {
    ny_setenv("NYTRIX_NY_FULL_BIN", snapshot, 1);
    return;
  }
  if (!bin || !*bin)
    return;
  char resolved[PATH_MAX];
  const char *abs_bin = ny_realpath(bin, resolved) ? resolved : bin;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", abs_bin);
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  char *bslash = strrchr(dir, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
#endif
  const char *base = slash ? slash + 1 : dir;
  int debug = strstr(base, "_debug") != NULL;
  if (slash)
    *slash = '\0';
  else
    snprintf(dir, sizeof(dir), ".");
  char full[PATH_MAX];
#ifdef _WIN32
  snprintf(full, sizeof(full), "%s\\ny-full%s.exe", dir, debug ? "_debug" : "");
#else
  snprintf(full, sizeof(full), "%s/ny-full%s", dir, debug ? "_debug" : "");
#endif
  if (ny_access(full, 0) == 0)
    ny_setenv("NYTRIX_NY_FULL_BIN", full, 1);
}

static void export_shape_fixture_tools_for(const char *bin) {
  if (!getenv("NYTRIX_CMAKE_COMMAND"))
    ny_setenv("NYTRIX_CMAKE_COMMAND", "cmake", 0);
  if (!bin || !*bin)
    return;

  char resolved[PATH_MAX];
  const char *abs_bin = ny_realpath(bin, resolved) ? resolved : bin;
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", abs_bin);
  char *slash = strrchr(dir, '/');
#ifdef _WIN32
  char *bslash = strrchr(dir, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
#endif
  if (slash)
    *slash = '\0';
  else
    snprintf(dir, sizeof(dir), ".");

  /*
   * Repository fixtures need the source tree independently of the caller's
   * working directory. Test binaries built under build/<config> live two
   * directory levels below the checkout root.
   */
  char root[PATH_MAX];
  snprintf(root, sizeof(root), "%s", dir);
  char *root_slash = strrchr(root, '/');
#ifdef _WIN32
  char *root_bslash = strrchr(root, '\\');
  if (root_bslash && (!root_slash || root_bslash > root_slash))
    root_slash = root_bslash;
#endif
  if (root_slash) {
    *root_slash = '\0';
    root_slash = strrchr(root, '/');
#ifdef _WIN32
    root_bslash = strrchr(root, '\\');
    if (root_bslash && (!root_slash || root_bslash > root_slash))
      root_slash = root_bslash;
#endif
    if (root_slash) {
      *root_slash = '\0';
      char tests_dir[PATH_MAX];
      snprintf(tests_dir, sizeof(tests_dir), "%s/etc/tests", root);
      if (!getenv("NYTRIX_ROOT") && is_dir(tests_dir))
        ny_setenv("NYTRIX_ROOT", root, 0);
    }
  }

  /*
   * The test executable lives in the active CMake binary directory.
   */
  if (!getenv("NYTRIX_TEST_BUILD_ROOT") || !*getenv("NYTRIX_TEST_BUILD_ROOT"))
    ny_setenv("NYTRIX_TEST_BUILD_ROOT", dir, 0);

  char tool[PATH_MAX];
#ifdef _WIN32
  snprintf(tool, sizeof(tool), "%s\\ny-fmt.exe", dir);
#else
  snprintf(tool, sizeof(tool), "%s/ny-fmt", dir);
#endif
  if (!getenv("NYTRIX_TEST_FMT_BIN") && ny_access(tool, 0) == 0)
    ny_setenv("NYTRIX_TEST_FMT_BIN", tool, 0);
#ifdef _WIN32
  snprintf(tool, sizeof(tool), "%s\\ny-doc.exe", dir);
#else
  snprintf(tool, sizeof(tool), "%s/ny-doc", dir);
#endif
  if (!getenv("NYTRIX_TEST_DOC_BIN") && ny_access(tool, 0) == 0)
    ny_setenv("NYTRIX_TEST_DOC_BIN", tool, 0);
}

static void apply_test_child_env(void) {
  /*
   * Children write diagnostics into capture files (isatty false there);
   * keep their native ANSI colors when the runner itself is on a
   * terminal, so failure replay shows real compiler styling.
   */
  if (isatty(STDERR_FILENO) && !getenv("NO_COLOR"))
    ny_setenv("FORCE_COLOR", "1", 1);
  if (test_env_falsey("NYTRIX_TEST_CACHE") ||
      test_env_truthy("NYTRIX_TEST_NO_NATIVE_CACHE")) {
    ny_setenv("NYTRIX_JIT_CACHE", "0", 1);
    ny_setenv("NYTRIX_AOT_CACHE", "0", 1);
  }
  if (test_env_falsey("NYTRIX_STD_CACHE"))
    ny_setenv("NYTRIX_STD_CACHE", "0", 1);
}

static const char *test_warn_arg(void) {
  const char *level = getenv("NYTRIX_TEST_WARN");
  if (!level)
    level = "none";
  if (!*level || strcmp(level, "default") == 0)
    return NULL;
  static char arg[32];
  snprintf(arg, sizeof(arg), "--warn=%s", level);
  return arg;
}

static void push_test_warn_arg(char **argv, int *argc, int max) {
  const char *arg = test_warn_arg();
  if (arg && *argc < max - 1)
    argv[(*argc)++] = (char *)arg;
}

static void configure_test_cache_defaults(void) {
  /*
   * Normal runs reuse source- and configuration-keyed executable artifacts.
   * Cold runs (or an explicit =0) still exercise code generation every time.
   */
  if (!getenv("NYTRIX_TEST_EXEC_CACHE") && !test_env_truthy("NYTRIX_TEST_COLD"))
    ny_setenv("NYTRIX_TEST_EXEC_CACHE", "1", 1);
  if (test_env_truthy("NYTRIX_TEST_EXEC_CACHE") &&
      !test_env_truthy("NYTRIX_TEST_COLD")) {
    test_setenv_default("NYTRIX_TEST_NO_NATIVE_CACHE", "0");
    test_setenv_default("NYTRIX_JIT_CACHE", "1");
    test_setenv_default("NYTRIX_AOT_CACHE", "1");
    return;
  }

  if (getenv("NYTRIX_TEST_EXEC_CACHE") || test_env_truthy("NYTRIX_TEST_COLD")) {
    ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", "1", 1);
    ny_setenv("NYTRIX_JIT_CACHE", "0", 1);
    ny_setenv("NYTRIX_AOT_CACHE", "0", 1);
    return;
  }
  test_setenv_default("NYTRIX_TEST_NO_NATIVE_CACHE", "1");
  test_setenv_default("NYTRIX_JIT_CACHE", "0");
  test_setenv_default("NYTRIX_AOT_CACHE", "0");
}

static void enable_core_dumps(void) {
#if defined(__APPLE__) || defined(__linux__)
  struct rlimit lim;
  lim.rlim_cur = RLIM_INFINITY;
  lim.rlim_max = RLIM_INFINITY;
  (void)setrlimit(RLIMIT_CORE, &lim);
#endif
}

static void poll_sleep(void) {
  /*
   * 1ms poll cadence (matches perf.c): keeps fast fixtures snappy without
   * measurable CPU cost — waits are short-lived and wakeups are cheap vs
   * the 10ms granularity that added up to ~5ms mean latency per wait.
   */
#ifdef _WIN32
  Sleep(1);
#else
  struct timespec ts = {0, 1000000L};
  nanosleep(&ts, NULL);
#endif
}

static int test_archive_source_needs_m32(const char *src_path) {
  if (!src_path)
    return 0;
  const char *base = strrchr(src_path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(src_path, '\\');
  if (!base || (bslash && bslash > base))
    base = bslash;
#endif
  base = base ? base + 1 : src_path;
  size_t n = strlen(base);
  return (n >= 4 && strcmp(base + n - 4, "32.c") == 0) ||
         strstr(base, "32_") != NULL || strstr(base, "_32") != NULL ||
         strstr(base, "32-") != NULL || strstr(base, "-32") != NULL;
}

static int test_compile_archive_source(const char *cc, const char *src_path,
                                       const char *obj_path, int force_m32) {
  int use_m32 = force_m32 || test_archive_source_needs_m32(src_path);
  char *cc_argv[12];
  int cc_argc = 0;
  cc_argv[cc_argc++] = (char *)cc;
  if (use_m32)
    cc_argv[cc_argc++] = (char *)"-m32";
  cc_argv[cc_argc++] = (char *)"-c";
  cc_argv[cc_argc++] = (char *)"-fno-pic";
  cc_argv[cc_argc++] = (char *)"-fno-builtin";
  cc_argv[cc_argc++] = (char *)"-fno-inline";
  cc_argv[cc_argc++] = (char *)src_path;
  cc_argv[cc_argc++] = (char *)"-o";
  cc_argv[cc_argc++] = (char *)obj_path;
  cc_argv[cc_argc] = NULL;
  int cc_rc = run_debug_argv(cc_argv, 30, 1);
  return cc_rc == 0;
}

static void test_collect_archive_sibling_sources(const char *archive_path,
                                                 StrVec *out) {
  if (!archive_path || !out)
    return;
  size_t len = strlen(archive_path);
  if (len <= 2 || archive_path[len - 2] != '.' || archive_path[len - 1] != 'a')
    return;

  char dir[PATH_MAX];
  char stem[PATH_MAX];
  const char *base = strrchr(archive_path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(archive_path, '\\');
  if (!base || (bslash && bslash > base))
    base = bslash;
#endif
  if (base) {
    size_t dlen = (size_t)(base - archive_path);
    if (dlen >= sizeof(dir))
      return;
    memmove(dir, archive_path, dlen);
    dir[dlen] = '\0';
    base++;
  } else {
    snprintf(dir, sizeof(dir), ".");
    base = archive_path;
  }
  size_t blen = strlen(base);
  if (blen <= 2 || blen - 2 >= sizeof(stem))
    return;
  memmove(stem, base, blen - 2);
  stem[blen - 2] = '\0';
  for (char *p = stem; *p; ++p) {
    if (*p == '_')
      *p = '-';
  }

  char prefix[PATH_MAX];
  snprintf(prefix, sizeof(prefix), "%s-", stem);
  size_t plen = strlen(prefix);
  DIR *d = opendir(dir);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    const char *name = ent->d_name;
    if (strncmp(name, prefix, plen) != 0 || !nyt_ends_with(name, ".c"))
      continue;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (nyt_is_file(path))
      sv_push(out, path);
  }
  closedir(d);
  if (out->len > 1)
    qsort(out->items, out->len, sizeof(out->items[0]), path_lex_cmp);
}

/*
 * The checked-in archive fixtures keep source inputs beside their .nshape
 * files, while generated archives live below build/test-artifacts. Recreate
 * missing fixture archives from those canonical inputs when running from a
 * clean tree or a nonstandard CMake build directory.
 */
static void test_collect_fixture_archive_sources(const char *archive_path,
                                                 StrVec *out) {
  if (!archive_path || !out)
    return;
  const char *objects = strstr(archive_path, "objects/");
  if (!objects)
    return;
  const char *arch = objects + strlen("objects/");
  const char *arch_end = strchr(arch, '/');
  if (!arch_end || arch_end == arch || (size_t)(arch_end - arch) >= 32)
    return;
  char arch_name[32];
  memcpy(arch_name, arch, (size_t)(arch_end - arch));
  arch_name[arch_end - arch] = '\0';

  const char *base = strrchr(archive_path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(archive_path, '\\');
  if (!base || (bslash && bslash > base))
    base = bslash;
#endif
  base = base ? base + 1 : archive_path;
  size_t len = strlen(base);
  if (len <= 2 || strcmp(base + len - 2, ".a") != 0)
    return;
  char stem[PATH_MAX];
  if (len - 2 >= sizeof(stem))
    return;
  memcpy(stem, base, len - 2);
  stem[len - 2] = '\0';
  for (char *p = stem; *p; ++p)
    if (*p == '_')
      *p = '-';

  char source_dir[PATH_MAX];
  snprintf(source_dir, sizeof(source_dir), "etc/tests/code/formats/%s",
           arch_name);
  char exact[PATH_MAX];
  snprintf(exact, sizeof(exact), "%s/%s.c", source_dir, stem);
  if (nyt_is_file(exact)) {
    sv_push(out, exact);
    return;
  }

  char prefix[PATH_MAX];
  snprintf(prefix, sizeof(prefix), "%s-", stem);
  size_t prefix_len = strlen(prefix);
  DIR *dir = opendir(source_dir);
  if (!dir)
    return;
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strncmp(ent->d_name, prefix, prefix_len) != 0 ||
        !nyt_ends_with(ent->d_name, ".c"))
      continue;
    char source[PATH_MAX];
    snprintf(source, sizeof(source), "%s/%s", source_dir, ent->d_name);
    if (nyt_is_file(source))
      sv_push(out, source);
  }
  closedir(dir);
  if (out->len > 1)
    qsort(out->items, out->len, sizeof(out->items[0]), path_lex_cmp);
}

static int test_archive_sources_newer_than_archive(const char *archive_path,
                                                   const StrVec *srcs) {
  struct stat ast;
  if (!archive_path || !*archive_path || stat(archive_path, &ast) != 0)
    return 1;
  if (!srcs)
    return 0;
  for (size_t i = 0; i < srcs->len; ++i) {
    struct stat sst;
    if (srcs->items[i] && stat(srcs->items[i], &sst) == 0) {
      if (sst.st_mtime > ast.st_mtime)
        return 1;
    }
  }
  return 0;
}

static int test_build_missing_archive(const char *archive_path, int force_m32) {
  if (!archive_path || !*archive_path)
    return 0;
  size_t alen = strlen(archive_path);
  if (alen <= 2 || archive_path[alen - 2] != '.' ||
      archive_path[alen - 1] != 'a')
    return nyt_is_file(archive_path);

  StrVec srcs = {0};
  char single_src[PATH_MAX];
  snprintf(single_src, sizeof(single_src), "%s", archive_path);
  snprintf(single_src + alen - 2, sizeof(single_src) - alen + 2, ".c");
  if (nyt_is_file(single_src)) {
    sv_push(&srcs, single_src);
  } else {
    char alternate_src[PATH_MAX];
    snprintf(alternate_src, sizeof(alternate_src), "%s", single_src);
    char *base = strrchr(alternate_src, '/');
#ifdef _WIN32
    char *backslash = strrchr(alternate_src, '\\');
    if (!base || (backslash && backslash > base))
      base = backslash;
#endif
    for (char *p = base ? base + 1 : alternate_src; *p; ++p)
      if (*p == '_')
        *p = '-';
    if (nyt_is_file(alternate_src))
      sv_push(&srcs, alternate_src);
    else {
      test_collect_archive_sibling_sources(archive_path, &srcs);
      if (srcs.len == 0)
        test_collect_fixture_archive_sources(archive_path, &srcs);
    }
  }

  if (srcs.len == 0) {
    int exists = nyt_is_file(archive_path);
    sv_free(&srcs);
    return exists;
  }

  if (!test_archive_sources_newer_than_archive(archive_path, &srcs)) {
    sv_free(&srcs);
    return 1;
  }

  char archive_dir[PATH_MAX];
  snprintf(archive_dir, sizeof(archive_dir), "%s", archive_path);
  char *archive_base = strrchr(archive_dir, '/');
#ifdef _WIN32
  char *archive_backslash = strrchr(archive_dir, '\\');
  if (!archive_base || (archive_backslash && archive_backslash > archive_base))
    archive_base = archive_backslash;
#endif
  if (archive_base) {
    *archive_base = '\0';
    ny_ensure_dir_recursive(archive_dir);
  }

  const char *cc = getenv("CC");
  if (!cc || !*cc)
    cc = "cc";
  StrVec objs = {0};
  int ok = 1;
  for (size_t i = 0; i < srcs.len; ++i) {
    char obj_path[PATH_MAX];
    snprintf(obj_path, sizeof(obj_path), "%s/ny-ar-obj-%ld-%zu-XXXXXX",
             nyt_temp_dir(), (long)getpid(), i);
    int fd = mkstemp(obj_path);
    if (fd < 0) {
      ok = 0;
      break;
    }
    close(fd);
    if (!test_compile_archive_source(cc, srcs.items[i], obj_path, force_m32)) {
      remove(obj_path);
      ok = 0;
      break;
    }
    sv_push(&objs, obj_path);
  }

  if (ok && objs.len > 0) {
    char archive_tmp[PATH_MAX];
    snprintf(archive_tmp, sizeof(archive_tmp), "%s.tmp-XXXXXX", archive_path);
    int archive_fd = mkstemp(archive_tmp);
    if (archive_fd >= 0) {
      close(archive_fd);
      remove(archive_tmp);
    } else {
      ok = 0;
    }
    char **ar_argv = (char **)calloc(objs.len + 4, sizeof(char *));
    if (!ar_argv) {
      ok = 0;
    } else if (ok) {
      size_t argc = 0;
      ar_argv[argc++] = (char *)"ar";
      ar_argv[argc++] = (char *)"rcs";
      ar_argv[argc++] = archive_tmp;
      for (size_t i = 0; i < objs.len; ++i)
        ar_argv[argc++] = objs.items[i];
      ar_argv[argc] = NULL;
      ok = run_debug_argv(ar_argv, 30, 1) == 0 && nyt_is_file(archive_tmp);
      if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(archive_tmp, archive_path,
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
             0;
#else
        ok = rename(archive_tmp, archive_path) == 0;
#endif
      }
      if (!ok)
        remove(archive_tmp);
      free(ar_argv);
    }
  }

  for (size_t i = 0; i < objs.len; ++i)
    remove(objs.items[i]);
  sv_free(&objs);
  sv_free(&srcs);
  return ok;
}

static char *decode_shape_quoted_string(const char *start, size_t len);

#ifndef _WIN32
static int test_merge_link_archives(const StrVec *links, char *out,
                                    size_t out_len) {
  if (!links || links->len < 2 || !out || out_len == 0)
    return 0;
  snprintf(out, out_len, "%s/ny-internal-link-libs-%ld-%ld.a", nyt_temp_dir(),
           (long)getpid(), (long)now_ms());
  for (size_t i = 0; i < links->len; ++i)
    if (!links->items[i] || strchr(links->items[i], '\n') ||
        strchr(links->items[i], '\r'))
      return 0;
  FILE *ar = popen("ar -M", "w");
  if (!ar)
    return 0;
  fprintf(ar, "CREATE %s\n", out);
  for (size_t i = links->len; i > 0; --i)
    fprintf(ar, "ADDLIB %s\n", links->items[i - 1]);
  fputs("SAVE\nEND\n", ar);
  int status = pclose(ar);
  if (status != 0 || !nyt_is_file(out)) {
    remove(out);
    out[0] = '\0';
    return 0;
  }
  return 1;
}
#endif

static int object_link_run_check(const char *shape_path) {
  if (!shape_path || !shape_path_is_nshape(shape_path))
    return 0;
  char *expect_val = shape_meta_string(shape_path, "expect");
  bool host_aot_route =
      expect_val && strncmp(expect_val, "object_host_link_run_", 21) == 0;
  size_t prefix_len = host_aot_route ? 21 : 16;
  if (!expect_val ||
      (!host_aot_route && strncmp(expect_val, "object_link_run_", 16) != 0)) {
    free(expect_val);
    return 0;
  }
  const char *suffix = expect_val + prefix_len;
  ny_test_link_ret_kind_t ret_kind = NY_TEST_LINK_RET_I64;
  if (strncmp(suffix, "i64_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_I64;
    suffix += 4;
  } else if (strncmp(suffix, "f64_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_F64;
    suffix += 4;
  } else if (strncmp(suffix, "f32_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_F32;
    suffix += 4;
  } else if (strncmp(suffix, "i32_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_I32;
    suffix += 4;
  } else if (strncmp(suffix, "u32_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_U32;
    suffix += 4;
  } else if (strncmp(suffix, "i16_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_I16;
    suffix += 4;
  } else if (strncmp(suffix, "u16_", 4) == 0) {
    ret_kind = NY_TEST_LINK_RET_U16;
    suffix += 4;
  } else if (strncmp(suffix, "i8_", 3) == 0) {
    ret_kind = NY_TEST_LINK_RET_I8;
    suffix += 3;
  } else if (strncmp(suffix, "u8_", 3) == 0) {
    ret_kind = NY_TEST_LINK_RET_U8;
    suffix += 3;
  } else if (strncmp(suffix, "bool_", 5) == 0) {
    ret_kind = NY_TEST_LINK_RET_BOOL;
    suffix += 5;
  } else {
    free(expect_val);
    return 0;
  }
  if (!*suffix || strlen(suffix) > 31) {
    free(expect_val);
    return 0;
  }
  char expected_val[32];
  snprintf(expected_val, sizeof(expected_val), "%s", suffix);

  char *flags = shape_meta_string(shape_path, "flags");
  if (!flags) {
    free(expect_val);
    return 1;
  }
  char obj_path[PATH_MAX];
  obj_path[0] = '\0';
  char flags_buf[1024];
  snprintf(flags_buf, sizeof(flags_buf), "%s", flags);
  trim_inplace(flags_buf);
  char *flagv[64];
  int flagc = split_words(flags_buf, flagv, 64);
  for (int i = 0; i < flagc; ++i) {
    if (strcmp(flagv[i], "-o") == 0) {
      if (i + 1 < flagc)
        snprintf(obj_path, sizeof(obj_path), "%s", flagv[i + 1]);
      break;
    }
  }
  free(flags);
  if (!obj_path[0]) {
    fprintf(stderr,
            "object link/run: missing emitted object from token '-o <path>' in "
            "%s\n",
            disp_path(shape_path));
    free(expect_val);
    return 1;
  }
  if (!nyt_is_file(obj_path)) {
    free(expect_val);
    return 1;
  }

  /*
   * Collect all "link" entries (support multi-link for split archive tests
   * etc.)
   */
  StrVec links = {0};
  {
    char file[PATH_MAX];
    const char *shape = NULL;
    shape_path_split(shape_path, file, sizeof(file), &shape);
    char *data = read_small_file(file);
    if (data) {
      size_t data_len = strlen(data);
      size_t start = 0, end = data_len;
      if (shape && *shape &&
          !shape_block_span(data, data_len, shape, &start, &end)) {
        free(data);
      } else {
        char *line = data + start;
        char *region_end = data + end;
        while (line && line < region_end) {
          char *nxt = memchr(line, '\n', (size_t)(region_end - line));
          if (nxt)
            *nxt = '\0';
          char *p = line;
          trim_inplace(p);
          if (strncmp(p, "link ", 5) == 0) {
            p += 5;
            trim_inplace(p);
            if (*p == '"') {
              char *st = ++p;
              char *en = st;
              while (*en && (*en != '"' || (en > st && en[-1] == '\\')))
                en++;
              char *val = decode_shape_quoted_string(st, (size_t)(en - st));
              if (val && *val) {
                sv_push(&links, val);
                free(val);
              }
            } else if (*p) {
              sv_push(&links, p);
            }
          }
          line = nxt ? nxt + 1 : NULL;
        }
        free(data);
      }
    }
  }
  int force_m32 = 0;
  int force_aarch64 = 0;
  int force_x86_64 = 0;
  FILE *obj_file = fopen(obj_path, "rb");
  if (obj_file) {
    unsigned char elf_header_prefix[20] = {0};
    if (fread(elf_header_prefix, 1, sizeof(elf_header_prefix), obj_file) ==
            sizeof(elf_header_prefix) &&
        memcmp(elf_header_prefix, "\177ELF", 4) == 0 &&
        elf_header_prefix[5] == 1) {
      uint16_t machine = (uint16_t)elf_header_prefix[18] |
                         ((uint16_t)elf_header_prefix[19] << 8);
      if (elf_header_prefix[4] == 1)
        force_m32 = 1;
      else if (elf_header_prefix[4] == 2 && machine == 183)
        force_aarch64 = 1;
      else if (elf_header_prefix[4] == 2 && machine == 62)
        force_x86_64 = 1;
    }
    fclose(obj_file);
  }
  for (size_t i = 0; i < links.len; i++) {
    (void)test_build_missing_archive(links.items[i], force_m32);
  }

#ifdef _WIN32
  sv_free(&links);
  (void)ret_kind;
  (void)expected_val;
  free(expect_val);
  return 0;
#else
  char merged_archive[PATH_MAX] = {0};
  const char *first_archive = links.len ? links.items[0] : NULL;
  if (links.len > 1) {
    if (!test_merge_link_archives(&links, merged_archive,
                                  sizeof(merged_archive))) {
      sv_free(&links);
      free(expect_val);
      return 1;
    }
    first_archive = merged_archive;
  }
  /*
   * ELF32 objects must never fall through to the host linker.  On a 64-bit
   * Linux host that produces misleading architecture errors (and, for these
   * freestanding objects, an unrelated missing rt_main error).  The internal
   * ELF32 runner is the only valid execution path; return its unsupported
   * status as a test skip rather than attempting an incompatible fallback.
   */
  if (force_m32) {
    int internal_rc = test_internal_elf32_link_run(
        obj_path, ret_kind, expected_val, shape_path, first_archive);
    if (internal_rc == 0) {
      if (merged_archive[0])
        remove(merged_archive);
      sv_free(&links);
      free(expect_val);
      return 0;
    }
    if (internal_rc == 1) {
      if (merged_archive[0])
        remove(merged_archive);
      sv_free(&links);
      free(expect_val);
      return 1;
    }
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }

  if (force_aarch64) {
    int internal_rc = test_internal_aarch64_elf64_link_run(
        obj_path, ret_kind, expected_val, shape_path);
    if (internal_rc == 0) {
      if (merged_archive[0])
        remove(merged_archive);
      sv_free(&links);
      free(expect_val);
      return 0;
    }
    if (internal_rc == 1) {
      if (merged_archive[0])
        remove(merged_archive);
      sv_free(&links);
      free(expect_val);
      return 1;
    }
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (force_x86_64) {
    if (host_aot_route) {
      int internal_rc = test_internal_elf64_aot_host_link_run(
          obj_path, ret_kind, expected_val, shape_path, first_archive);
      if (merged_archive[0])
        remove(merged_archive);
      sv_free(&links);
      free(expect_val);
      if (internal_rc == 0 || internal_rc == 1)
        return internal_rc;
      fprintf(stderr, "object link/run: no host AOT oracle for %s\n",
              disp_path(shape_path));
      return 1;
    }
    sv_free(&links);
    free(expect_val);
    fprintf(stderr,
            "object link/run: x86-64 AOT route has no execution oracle: %s\n",
            disp_path(shape_path));
    return 1;
  }
  int internal_rc = test_internal_elf64_link_run(
      obj_path, ret_kind, expected_val, shape_path, first_archive);
  if (internal_rc == 0) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  /*
   * A native x86-64 object that the internal linker cannot handle should not
   * be sent to the C fallback when it contains runtime imports.
   */
  if (test_has_unsupported_external_runtime_symbol(obj_path)) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  internal_rc = test_internal_elf32_link_run(obj_path, ret_kind, expected_val,
                                             shape_path, first_archive);
  if (internal_rc == 0) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  /*
   * The internal linker is authoritative for native NYIR objects that use
   * runtime helpers.  Do not fall through to the host C linker: it cannot
   * resolve rt_* symbols and may also target a different ISA.
   */
  if (test_has_unsupported_external_runtime_symbol(obj_path)) {
    if (merged_archive[0])
      remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (merged_archive[0])
    remove(merged_archive);

#if !defined(__linux__)
  /*
   * ELF execution/link validation belongs to the Linux internal linker.
   * A Mach-O host linker cannot consume these cross-format objects.
   */
  sv_free(&links);
  free(expect_val);
  return 0;
#endif

  char harness_path[PATH_MAX];
  snprintf(harness_path, sizeof(harness_path), "%s/ny-link-run-%ld-XXXXXX.c",
           nyt_temp_dir(), (long)getpid());
  int hfd = mkstemps(harness_path, 2);
  if (hfd < 0) {
    free(expect_val);
    return 1;
  }
  FILE *hf = fdopen(hfd, "w");
  if (!hf) {
    close(hfd);
    remove(harness_path);
    free(expect_val);
    return 1;
  }
  if (ret_kind == NY_TEST_LINK_RET_F64)
    fprintf(
        hf,
        "#include <math.h>\nextern double rt_main(void);\n"
        "int main(void){double v=rt_main();return fabs(v-(%s))<1e-7?0:1;}\n",
        expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_F32)
    fprintf(
        hf,
        "#include <math.h>\nextern float rt_main(void);\n"
        "int main(void){float v=rt_main();return fabsf(v-(%sf))<1e-6f?0:1;}\n",
        expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I32)
    fprintf(hf,
            "extern int rt_main(void);\n"
            "int main(void){return rt_main()==(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U32)
    fprintf(hf,
            "extern unsigned int rt_main(void);\n"
            "int main(void){return rt_main()==(%sU)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I16)
    fprintf(hf,
            "extern short rt_main(void);\n"
            "int main(void){return rt_main()==(short)(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U16)
    fprintf(hf,
            "extern unsigned short rt_main(void);\n"
            "int main(void){return rt_main()==(unsigned short)(%sU)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I8)
    fprintf(hf,
            "extern signed char rt_main(void);\n"
            "int main(void){return rt_main()==(signed char)(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U8 || ret_kind == NY_TEST_LINK_RET_BOOL)
    fprintf(hf,
            "extern unsigned char rt_main(void);\n"
            "int main(void){return rt_main()==(unsigned char)(%sU)?0:1;}\n",
            expected_val);
  else
    fprintf(hf,
            "extern long rt_main(void);\n"
            "int main(void){return rt_main()==(%s)?0:1;}\n",
            expected_val);
  fclose(hf);

  const char *cc = getenv("CC");
  if (!cc || !*cc)
    cc = "cc";

  char exe_path[PATH_MAX];
  snprintf(exe_path, sizeof(exe_path), "%s/ny-link-run-exe-%ld-%ld",
           nyt_temp_dir(), (long)getpid(), (long)now_ms());

  char *link_argv[12];
  int link_argc = 0;
  link_argv[link_argc++] = (char *)cc;
#if defined(__linux__)
  /*
   * Native test objects are linked as fixed-address ELF images by the
   * test harness.  Most modern Linux compilers default to PIE, which can
   * make archive members with .text relocations print DT_TEXTREL warnings
   * even when the test passes.  Link the throwaway harness as non-PIE so
   * the native-object tests stay deterministic and quiet.
   */
  link_argv[link_argc++] = "-no-pie";
#endif
  link_argv[link_argc++] = obj_path;
  link_argv[link_argc++] = harness_path;
  for (size_t i = 0; i < links.len && link_argc < 10; i++) {
    link_argv[link_argc++] = links.items[i];
  }
  link_argv[link_argc++] = "-o";
  link_argv[link_argc++] = exe_path;
  if (ret_kind == NY_TEST_LINK_RET_F64 || ret_kind == NY_TEST_LINK_RET_F32)
    link_argv[link_argc++] = "-lm";
  link_argv[link_argc] = NULL;
  int link_rc = run_debug_argv(link_argv, 30, 1);
  if (link_rc != 0) {
    fprintf(stderr, "object link/run: cc link failed rc=%d for %s\n", link_rc,
            disp_path(shape_path));
    remove(harness_path);
    remove(exe_path);
    sv_free(&links);
    free(expect_val);
    return 1;
  }

  char *run_argv[] = {exe_path, NULL};
  int run_rc = run_debug_argv(run_argv, 30, 0);
  int run_ok = run_rc == 0;
  if (!run_ok)
    fprintf(stderr, "object link/run: executable failed rc=%d for %s\n", run_rc,
            disp_path(shape_path));

  remove(harness_path);
  remove(exe_path);
  sv_free(&links);
  free(expect_val);
  return run_ok ? 0 : 1;
#endif
}

static int split_flag_matrix_rows(char *s, char **out, int max) {
  int n = 0;
  char *p = s;
  while (p && *p && n < max) {
    while (*p == ';' || *p == '\n' || *p == '\r' || isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    out[n++] = p;
    while (*p && *p != ';' && *p != '\n' && *p != '\r')
      p++;
    if (*p)
      *p++ = '\0';
    trim_inplace(out[n - 1]);
    if (!out[n - 1][0])
      n--;
  }
  return n;
}

static void trim_inplace(char *s) {
  if (!s)
    return;
  char *p = s;
  while (*p && isspace((unsigned char)*p))
    p++;
  if (p != s)
    memmove(s, p, strlen(p) + 1);
  size_t n = strlen(s);
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    s[--n] = '\0';
}

static void error_meta_free(char *flags, char *expect) {
  free(flags);
  free(expect);
}

/*
 * Multi-shape discovery: a single .nshape file may contain several
 * `shape X { ... }` blocks.  The runner addresses one shape inside a file as
 * `path:shape`, so every consumer splits the string back into the file path
 * and the optional shape name (NULL when the path is a plain file).
 */
static const char *shape_path_split(const char *p, char *file_out,
                                    size_t file_cap, const char **shape_out) {
  *shape_out = NULL;
  if (!p) {
    file_out[0] = '\0';
    return file_out;
  }
  const char *colon = strrchr(p, ':');
  if (colon && colon > p && strchr(colon + 1, '/') == NULL &&
      strchr(colon + 1, ':') == NULL) {
    /*
     * Only treat the suffix as a shape selector when the file part is an
     * nshape/ny path (plain files never carry a ':' in this repo).
     */
    size_t flen = (size_t)(colon - p);
    bool file_is_shape =
        (flen >= 7 && memcmp(p + flen - 7, ".nshape", 7) == 0) ||
        (flen >= 3 && memcmp(p + flen - 3, ".ny", 3) == 0);
    if (file_is_shape && flen + 1 < file_cap) {
      memmove(file_out, p, flen);
      file_out[flen] = '\0';
      *shape_out = colon + 1;
      return file_out;
    }
  }
  snprintf(file_out, file_cap, "%s", p);
  return file_out;
}

/*
 * True when the file part of a (possibly `path:shape`) test path is .nshape.
 */
static bool shape_path_is_nshape(const char *p) {
  if (!p)
    return false;
  char file[PATH_MAX];
  const char *shape = NULL;
  shape_path_split(p, file, sizeof(file), &shape);
  return nyt_ends_with(file, ".nshape");
}

/*
 * Byte span of a named `shape X { ... }` block inside a .nshape file.  The
 * block is delimited by brace counting so nested braces inside a source
 * heredoc do not confuse the span.  Returns 1 and fills *out_start and
 * *out_end (offsets of '{' and matching '}') on success.
 */
static int shape_block_span(const char *data, size_t data_len, const char *name,
                            size_t *out_start, size_t *out_end) {
  if (!data || !name || !*name)
    return 0;
  char needle[160];
  snprintf(needle, sizeof(needle), "shape %s", name);
  size_t needle_len = strlen(needle);
  const char *p = data;
  const char *end = data + data_len;
  while (p < end) {
    const char *hit = ny_memmem(p, (size_t)(end - p), needle, needle_len);
    if (!hit)
      return 0;
    /*
     * must be a word-boundary match: preceding char is not ident-ish
     */
    if (hit == data || !(isalnum((unsigned char)hit[-1]) || hit[-1] == '_')) {
      const char *q = hit + needle_len;
      while (q < end && (*q == ' ' || *q == '\t'))
        q++;
      if (q < end && *q == '{') {
        *out_start = (size_t)(q - data);
        int depth = 1;
        const char *c = q + 1;
        for (; c < end; c++) {
          if (c == data || c[-1] == '\n') {
            const char *line_end = memchr(c, '\n', (size_t)(end - c));
            if (!line_end)
              line_end = end;
            const char *line = c;
            while (line < line_end && (*line == ' ' || *line == '\t'))
              line++;
            if ((size_t)(line_end - line) >= 7 &&
                memcmp(line, "source ", 7) == 0) {
              const char *open =
                  ny_memmem(line, (size_t)(line_end - line), "<<'", 3);
              const char *marker = open ? open + 3 : NULL;
              const char *quote =
                  marker ? memchr(marker, '\'', (size_t)(line_end - marker))
                         : NULL;
              if (marker && quote && quote != marker) {
                size_t marker_len = (size_t)(quote - marker);
                const char *body = line_end < end ? line_end + 1 : end;
                for (const char *body_line = body; body_line < end;) {
                  const char *body_end =
                      memchr(body_line, '\n', (size_t)(end - body_line));
                  if (!body_end)
                    body_end = end;
                  size_t body_len = (size_t)(body_end - body_line);
                  if (body_len && body_line[body_len - 1] == '\r')
                    body_len--;
                  if (body_len == marker_len &&
                      memcmp(body_line, marker, marker_len) == 0) {
                    c = body_end < end ? body_end : end - 1;
                    break;
                  }
                  body_line = body_end < end ? body_end + 1 : end;
                }
                if (c >= end)
                  break;
                if (c != line_end && c[-1] != '\n')
                  continue;
              }
            }
          }
          if (*c == '"' || *c == '\'') {
            char quote = *c;
            for (c++; c < end; c++) {
              if (*c == '\\' && c + 1 < end) {
                c++;
                continue;
              }
              if (*c == quote)
                break;
              if (*c == '\n')
                break;
            }
            continue;
          }
          if (*c == ';' && c + 1 < end && c[1] == ';') {
            while (c < end && *c != '\n')
              c++;
            continue;
          }
          if (*c == '{')
            depth++;
          else if (*c == '}') {
            depth--;
            if (depth == 0) {
              *out_end = (size_t)(c - data);
              return 1;
            }
          }
        }
        return 0;
      }
    }
    p = hit + needle_len;
  }
  return 0;
}

/*
 * Search a line-oriented region [start, end) of an nshape file for a
 * `key ...` field.  Stops at the first `source ` line (heredocs begin there).
 * Returns a freshly allocated value, or NULL when absent.
 */
static char *shape_meta_in_region(const char *data, size_t start, size_t end,
                                  const char *key) {
  size_t key_len = strlen(key);
  size_t i = start;
  while (i < end) {
    size_t line_end = i;
    while (line_end < end && data[line_end] != '\n')
      line_end++;
    size_t line_len = line_end - i;
    const char *line = data + i;
    if (line_len && line[line_len - 1] == '\r')
      line_len--;
    /*
     * trim leading space
     */
    size_t ls = 0;
    while (ls < line_len && (line[ls] == ' ' || line[ls] == '\t'))
      ls++;
    const char *tp = line + ls;
    size_t tlen = line_len - ls;
    if (tlen >= 7 && strncmp(tp, "source ", 7) == 0)
      break;
    /*
     * Recurse into named sub-blocks (budget { }, static_island { },
     * rep_hints { }) so keys inside them resolve like top-level keys.
     */
    if (tlen >= 2 && tp[tlen - 1] == '{') {
      size_t depth = 1;
      size_t j = line_end + 1;
      size_t close_at = end;
      while (j < end && depth > 0) {
        size_t je = j;
        while (je < end && data[je] != '\n')
          je++;
        for (size_t k = j; k < je; ++k) {
          if (data[k] == '{')
            depth++;
          else if (data[k] == '}' && --depth == 0) {
            close_at = k;
            break;
          }
        }
        if (depth == 0)
          break;
        j = je + 1;
      }
      if (depth == 0 && close_at > line_end + 1) {
        char *inner = shape_meta_in_region(data, line_end + 1, close_at, key);
        if (inner)
          return inner;
      }
      i = line_end + 1;
      continue;
    }
    if (tlen > key_len && strncmp(tp, key, key_len) == 0 &&
        isspace((unsigned char)tp[key_len])) {
      const char *vp = tp + key_len;
      while (vp < line + line_len && (*vp == ' ' || *vp == '\t'))
        vp++;
      if (vp < line + line_len && *vp == '"') {
        const char *vs = vp + 1;
        const char *ve = vs;
        while (ve < line + line_len &&
               (*ve != '"' || (ve > vs && ve[-1] == '\\')))
          ve++;
        return decode_shape_quoted_string(vs, (size_t)(ve - vs));
      }
      char *out = (char *)malloc((size_t)(line + line_len - vp) + 1);
      if (out) {
        memmove(out, vp, (size_t)(line + line_len - vp));
        out[(size_t)(line + line_len - vp)] = '\0';
        trim_inplace(out);
      }
      return out;
    }
    i = line_end + 1;
  }
  return NULL;
}

/*
 * Byte range covered by the file-level `defaults { ... }` block, when one
 * exists.  Used as a fallback source of metadata for every shape in the file.
 */
static int shape_defaults_span(const char *data, size_t data_len,
                               size_t *out_start, size_t *out_end) {
  const char *needle = "defaults {";
  const char *hit = ny_memmem(data, data_len, needle, strlen(needle));
  if (!hit)
    return 0;
  /*
   * word boundary: only match at line start (after whitespace)
   */
  const char *line = hit;
  while (line > data && line[-1] != '\n')
    line--;
  const char *lp = line;
  while (lp < hit && (*lp == ' ' || *lp == '\t'))
    lp++;
  if (lp != hit)
    return 0;
  size_t start = (size_t)(hit - data);
  int depth = 1;
  const char *c = hit + strlen(needle);
  const char *end = data + data_len;
  for (; c < end; c++) {
    if (*c == '{')
      depth++;
    else if (*c == '}') {
      depth--;
      if (depth == 0) {
        *out_start = start;
        *out_end = (size_t)(c - data);
        return 1;
      }
    }
  }
  return 0;
}

static char *shape_source_block(const char *shape_path, const char *name) {
  if (!shape_path || !name || !*name)
    return NULL;
  char file[PATH_MAX];
  const char *shape = NULL;
  shape_path_split(shape_path, file, sizeof(file), &shape);
  char *data = read_small_file(file);
  if (!data)
    return NULL;
  size_t data_len = strlen(data);
  size_t start = 0, end = data_len;
  if (shape && *shape) {
    if (!shape_block_span(data, data_len, shape, &start, &end)) {
      free(data);
      return NULL;
    }
  }
  char needle[128];
  if (strcmp(name, "python") == 0)
    snprintf(needle, sizeof(needle), "source ny generate python <<'");
  else
    snprintf(needle, sizeof(needle), "source %s <<'", name);
  size_t needle_len = strlen(needle);
  const char *p = data + start;
  const char *region_end = data + end;
  char file_needle[128];
  snprintf(file_needle, sizeof(file_needle), "source %s file \"", name);
  const char *file_hit =
      ny_memmem(p, (size_t)(region_end - p), file_needle, strlen(file_needle));
  if (file_hit) {
    const char *path_start = file_hit + strlen(file_needle);
    const char *path_end =
        memchr(path_start, '"', (size_t)(region_end - path_start));
    if (!path_end || path_end == path_start) {
      free(data);
      return NULL;
    }
    char *source_path = malloc((size_t)(path_end - path_start) + 1);
    if (!source_path) {
      free(data);
      return NULL;
    }
    memcpy(source_path, path_start, (size_t)(path_end - path_start));
    source_path[path_end - path_start] = '\0';
    char *source = read_whole_file(source_path);
    free(source_path);
    free(data);
    return source;
  }
  const char *hit = ny_memmem(p, (size_t)(region_end - p), needle, needle_len);
  if (!hit) {
    free(data);
    return NULL;
  }
  const char *marker = hit + needle_len;
  const char *marker_end = strchr(marker, '\'');
  const char *body = marker_end ? strchr(marker_end, '\n') : NULL;
  if (!marker_end || !body || marker_end == marker) {
    free(data);
    return NULL;
  }
  body++;
  size_t marker_len = (size_t)(marker_end - marker);
  for (const char *line = body; line && line < region_end;) {
    const char *next = strchr(line, '\n');
    size_t line_len =
        next ? (size_t)(next - line) : (size_t)(region_end - line);
    if (line_len && line[line_len - 1] == '\r')
      line_len--;
    if (line_len == marker_len && memcmp(line, marker, marker_len) == 0) {
      size_t n = (size_t)(line - body);
      char *out = (char *)malloc(n + 1);
      if (out) {
        memmove(out, body, n);
        out[n] = '\0';
      }
      free(data);
      return out;
    }
    line = next ? next + 1 : NULL;
  }
  free(data);
  return NULL;
}

static char *decode_shape_quoted_string(const char *start, size_t len) {
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    char c = start[i];
    if (c == '\\' && i + 1 < len) {
      char esc = start[++i];
      if (esc == 'n')
        out[o++] = '\n';
      else if (esc == 'r')
        out[o++] = '\r';
      else if (esc == 't')
        out[o++] = '\t';
      else
        out[o++] = esc;
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
  return out;
}

typedef struct {
  char *path;
  char *data;
} ShapeMetaFileCacheEntry;

/*
 * Test selection asks for several metadata keys on every fixture and repeats
 * those lookups during scheduling and reporting. Keep the small shape files
 * in memory for the lifetime of ny-test instead of reopening and rereading
 * the corpus for every key. The cap keeps unusual external test trees bounded.
 */
static char *shape_meta_file_cached(const char *path, int *cached) {
  enum { SHAPE_META_FILE_CACHE_CAP = 4096 };
  static ShapeMetaFileCacheEntry entries[SHAPE_META_FILE_CACHE_CAP];
  static size_t count;
  *cached = 0;
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(entries[i].path, path) == 0) {
      *cached = 1;
      return entries[i].data;
    }
  }
  char *data = read_small_file(path);
  if (count == SHAPE_META_FILE_CACHE_CAP)
    return data;
  char *key = strdup(path);
  if (!key)
    return data;
  entries[count++] = (ShapeMetaFileCacheEntry){.path = key, .data = data};
  *cached = 1;
  return data;
}

static char *shape_meta_string(const char *shape_path, const char *key) {
  if (!shape_path || !key || !*key)
    return NULL;
  char file[PATH_MAX];
  const char *shape = NULL;
  shape_path_split(shape_path, file, sizeof(file), &shape);
  int cached = 0;
  char *data = shape_meta_file_cached(file, &cached);
  if (!data)
    return NULL;
  size_t data_len = strlen(data);
  char *found = NULL;
  if (shape && *shape) {
    size_t start = 0, end = data_len;
    if (shape_block_span(data, data_len, shape, &start, &end))
      found = shape_meta_in_region(data, start, end, key);
    if (!found) {
      /*
       * fall back to the file-level defaults block
       */
      size_t dstart = 0, dend = 0;
      if (shape_defaults_span(data, data_len, &dstart, &dend))
        found = shape_meta_in_region(data, dstart, dend, key);
    }
  } else {
    found = shape_meta_in_region(data, 0, data_len, key);
  }
  if (!cached)
    free(data);
  return found;
}

static int shape_expected_exit_code(const char *shape_path) {
  char *value = shape_meta_string(shape_path, "expect_exit");
  if (!value)
    return 0;
  char *end = NULL;
  long code = strtol(value, &end, 10);
  int result = end && end != value && *end == '\0' && code >= 0 && code <= 255
                   ? (int)code
                   : 0;
  free(value);
  return result;
}

static int shape_requests_jit(const char *shape_path) {
  char *flags = shape_meta_string(shape_path, "flags");
  char *matrix = shape_meta_string(shape_path, "flags_matrix");
  int requested =
      (flags && strstr(flags, "--jit")) || (matrix && strstr(matrix, "--jit"));
  free(flags);
  free(matrix);
  return requested;
}

static void shape_remove_tree(const char *path) {
  if (!path || !*path)
    return;
#ifdef _WIN32
  char pattern[PATH_MAX];
  snprintf(pattern, sizeof(pattern), "%s\\*", path);
  WIN32_FIND_DATAA data;
  HANDLE find = FindFirstFileA(pattern, &data);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s\\%s", path, data.cFileName);
      if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        shape_remove_tree(child);
      else
        DeleteFileA(child);
    } while (FindNextFileA(find, &data));
    FindClose(find);
  }
  RemoveDirectoryA(path);
#else
  DIR *dir = opendir(path);
  if (!dir) {
    remove(path);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char child[PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
    if (n < 0 || (size_t)n >= sizeof(child))
      continue;
    struct stat st;
    if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode))
      shape_remove_tree(child);
    else
      remove(child);
  }
  closedir(dir);
  rmdir(path);
#endif
}

static void shape_remove_materialized(const char *path) {
  if (path && *path) {
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
#ifdef _WIN32
    char *backslash = strrchr(parent, '\\');
    if (!slash || (backslash && backslash > slash))
      slash = backslash;
#endif
    if (slash) {
      *slash = '\0';
      const char *leaf = strrchr(parent, '/');
#ifdef _WIN32
      const char *back_leaf = strrchr(parent, '\\');
      if (!leaf || (back_leaf && back_leaf > leaf))
        leaf = back_leaf;
#endif
      leaf = leaf ? leaf + 1 : parent;
      if (strncmp(leaf, "ny-shape-tree-", 14) == 0) {
        shape_remove_tree(parent);
        return;
      }
    }
  }
  shape_remove_tree(path);
}

static int shape_write_tree_file(const char *path, const char *source) {
  if (!path || !source)
    return 0;
  char dirs[PATH_MAX];
  snprintf(dirs, sizeof(dirs), "%s", path);
  char *slash = strrchr(dirs, '/');
#ifdef _WIN32
  char *backslash = strrchr(dirs, '\\');
  if (!slash || (backslash && backslash > slash))
    slash = backslash;
#endif
  if (slash) {
    *slash = '\0';
    for (char *p = dirs + 1; *p; ++p) {
      if (*p != '/' && *p != '\\')
        continue;
      char saved = *p;
      *p = '\0';
#ifdef _WIN32
      CreateDirectoryA(dirs, NULL);
#else
      mkdir(dirs, 0777);
#endif
      *p = saved;
    }
#ifdef _WIN32
    CreateDirectoryA(dirs, NULL);
#else
    mkdir(dirs, 0777);
#endif
  }
  FILE *f = fopen(path, "wb");
  if (!f)
    return 0;
  size_t len = strlen(source);
  int ok = fwrite(source, 1, len, f) == len && fclose(f) == 0;
  if (!ok)
    remove(path);
  return ok;
}

static int shape_materialize_tree_file(const char *root, const char *shape_file,
                                       const char *source, int depth,
                                       int include_siblings) {
  if (!root || !shape_file || depth > 16)
    return 0;
  char output_path[PATH_MAX];
  char output_name[PATH_MAX];
  /*
   * `shape_file` may carry a `path.nshape:shape` selector; file naming and
   * sibling walks must use the file part only.
   */
  char file_only[PATH_MAX];
  const char *unused_shape = NULL;
  shape_path_split(shape_file, file_only, sizeof(file_only), &unused_shape);
  const char *base = strrchr(file_only, '/');
#ifdef _WIN32
  const char *back_base = strrchr(file_only, '\\');
  if (!base || (back_base && back_base > base))
    base = back_base;
#endif
  base = base ? base + 1 : file_only;
  size_t base_len = strlen(base);
  if (base_len < 7 || strcmp(base + base_len - 7, ".nshape") != 0)
    return 0;
  base_len -= 7;
  if (base_len + 4 >= sizeof(output_name))
    return 0;
  memcpy(output_name, base, base_len);
  memcpy(output_name + base_len, ".ny", 4);
  if (snprintf(output_path, sizeof(output_path), "%s/%s", root, output_name) >=
      (int)sizeof(output_path))
    return 0;
  char *owned_source = NULL;
  if (!source) {
    owned_source = shape_source_block(shape_file, "ny");
    source = owned_source;
  }
  int ok = source && shape_write_tree_file(output_path, source);
  free(owned_source);
  if (!ok || !include_siblings)
    return ok;

  char dir_path[PATH_MAX];
  snprintf(dir_path, sizeof(dir_path), "%s", file_only);
  char *dir_end = strrchr(dir_path, '/');
#ifdef _WIN32
  char *back_dir_end = strrchr(dir_path, '\\');
  if (!dir_end || (back_dir_end && back_dir_end > dir_end))
    dir_end = back_dir_end;
#endif
  if (!dir_end)
    return ok;
  *dir_end = '\0';
  DIR *dir = opendir(dir_path);
  if (!dir)
    return ok;
  closedir(dir);
  if (!shape_materialize_sibling_dir(root, dir_path, "", file_only, depth + 1))
    ok = 0;
  return ok;
}

/*
 * Materialize every .nshape under dir_path into root, preserving the
 * relative subdirectory layout (use/local.nshape -> <root>/use/local.ny) so
 * embedded fixtures can `use "./sub/peer.ny"` exactly as they would next to
 * the source fixture. skip_file is the triggering fixture's own path and is
 * never materialized twice.
 */
static int shape_materialize_sibling_dir(const char *root, const char *dir_path,
                                         const char *rel_prefix,
                                         const char *skip_file, int depth) {
  if (!root || !dir_path || depth > 16)
    return 1;
  DIR *dir = opendir(dir_path);
  if (!dir)
    return 1;
  int ok = 1;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (entry->d_name[0] == '.')
      continue;
    char child[PATH_MAX];
    if (snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name) >=
        (int)sizeof(child))
      continue;
    char rel[PATH_MAX];
    if (snprintf(rel, sizeof(rel), "%s%s", rel_prefix, entry->d_name) >=
        (int)sizeof(rel))
      continue;
    if (is_dir(child)) {
      char nested_prefix[PATH_MAX];
      if (snprintf(nested_prefix, sizeof(nested_prefix), "%s/", rel) >=
          (int)sizeof(nested_prefix))
        continue;
      if (!shape_materialize_sibling_dir(root, child, nested_prefix, skip_file,
                                         depth + 1))
        ok = 0;
      continue;
    }
    size_t len = strlen(entry->d_name);
    if (len < 7 || strcmp(entry->d_name + len - 7, ".nshape") != 0)
      continue;
    if (strcmp(child, skip_file) == 0)
      continue;
    char out_path[PATH_MAX];
    if (snprintf(out_path, sizeof(out_path), "%s/%.*s.ny", root,
                 (int)(strlen(rel) - 7), rel) >= (int)sizeof(out_path))
      continue;
    char *source = shape_source_block(child, "ny");
    /*
     * Sibling directories also contain fixtures that intentionally have no
     * embedded Ny program (for example C-only probes). They are not module
     * sources and must not prevent materializing the Ny fixtures in the same
     * directory.
     */
    if (source && !shape_write_tree_file(out_path, source))
      ok = 0;
    free(source);
  }
  closedir(dir);
  return ok;
}

static int shape_skips_ci(const char *path) {
  if (!path || !shape_path_is_nshape(path))
    return 0;
  char *ci = shape_meta_string(path, "ci");
  int skip = ci && (strcmp(ci, "skip") == 0 || strcmp(ci, "optional") == 0);
#ifdef _WIN32
  skip |= ci && strcmp(ci, "windows-skip") == 0;
#endif
  free(ci);
  return skip;
}

static int test_headless_mode(void) {
#ifdef _WIN32
  return 0;
#else
  /*
   * CI probe fixtures marked `ci skip` require a hosted display/device.  A
   * local runner without DISPLAY should get the same deterministic skip as CI
   * instead of spending the fixture timeout probing Vulkan/X11.
   */
  const char *force = getenv("NYTRIX_HEADLESS");
  if (force && *force)
    return ny_env_is_truthy(force);
  return !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY");
#endif
}

static int shape_skips_headless(const char *path) {
  if (!path || !shape_path_is_nshape(path) || !test_headless_mode())
    return 0;
  char *family = shape_meta_string(path, "family");
  int skip = family && strcmp(family, "probe-suite") == 0;
  free(family);
  return skip;
}

static int test_host_loopback_bind_available(void) {
#if defined(__APPLE__) || defined(__linux__)
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int ok = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
#else
  return 1;
#endif
}

static int test_unix_socket_connects(const char *path) {
#if defined(__APPLE__) || defined(__linux__)
  if (!path || !*path ||
      strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
    return 0;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return 0;
  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, path);
  int ok = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
  close(fd);
  return ok;
#else
  (void)path;
  return 0;
#endif
}

static int test_host_audio_service_available(void) {
#ifdef _WIN32
  return 1;
#elif defined(__APPLE__)
  return access("/dev/audio", R_OK | W_OK) == 0;
#elif defined(__linux__)
  if (access("/dev/snd", R_OK | W_OK) == 0)
    return 1;
  const char *pulse_server = getenv("PULSE_SERVER");
  if (pulse_server && *pulse_server)
    return 1;
  const char *runtime = getenv("XDG_RUNTIME_DIR");
  char path[PATH_MAX];
  if (runtime && *runtime) {
    snprintf(path, sizeof(path), "%s/pulse/native", runtime);
    if (test_unix_socket_connects(path))
      return 1;
    snprintf(path, sizeof(path), "%s/pipewire-0", runtime);
    if (test_unix_socket_connects(path))
      return 1;
  }
  return 0;
#else
  return 1;
#endif
}

static char *materialize_shape_ny_source(const char *shape_path) {
  char *source = shape_source_block(shape_path, "ny");
  if (source) {
    char root[PATH_MAX];
#ifdef _WIN32
    if (!GetTempFileNameA("build", "nyt", 0, root)) {
      free(source);
      return NULL;
    }
    DeleteFileA(root);
    if (!CreateDirectoryA(root, NULL)) {
      free(source);
      return NULL;
    }
#else
    snprintf(root, sizeof(root), "%s/ny-shape-tree-%ld-XXXXXX", nyt_temp_dir(),
             (long)getpid());
    if (!mkdtemp(root)) {
      free(source);
      return NULL;
    }
#endif
    bool has_relative_import = strstr(source, "use \"./") != NULL ||
                               strstr(source, "use '../") != NULL ||
                               strstr(source, "use \"../") != NULL ||
                               strstr(source, "use './") != NULL;
    /*
     * Multi-part `module` declarations can resolve peer modules from sibling
     * embedded fixtures without spelling a relative `use` path. Materialize
     * that fixture tree as well so the loader sees the same module graph as
     * it does for ordinary .ny files.
     */
    bool has_module_declaration = false;
    for (const char *line = source; line && *line;) {
      const char *next = strchr(line, '\n');
      const char *end = next ? next : line + strlen(line);
      const char *p = line;
      while (p < end && isspace((unsigned char)*p))
        ++p;
      if (p + 6 <= end && memcmp(p, "module", 6) == 0 &&
          (p + 6 == end || isspace((unsigned char)p[6]))) {
        has_module_declaration = true;
        break;
      }
      line = next ? next + 1 : NULL;
    }
    int materialized = shape_materialize_tree_file(root, shape_path, source, 0,
                                                   has_relative_import ||
                                                       has_module_declaration);
    free(source);
    if (!materialized) {
      shape_remove_tree(root);
      return NULL;
    }
    char output[PATH_MAX];
    /*
     * The shape selector (`path.nshape:shape`) must not leak into the
     * materialized file name; derive it from the file part alone.
     */
    char file_only[PATH_MAX];
    const char *unused_shape = NULL;
    shape_path_split(shape_path, file_only, sizeof(file_only), &unused_shape);
    const char *base = strrchr(file_only, '/');
#ifdef _WIN32
    const char *back_base = strrchr(file_only, '\\');
    if (!base || (back_base && back_base > base))
      base = back_base;
#endif
    base = base ? base + 1 : file_only;
    size_t base_len = strlen(base);
    if (base_len < 7 || strcmp(base + base_len - 7, ".nshape") != 0) {
      shape_remove_tree(root);
      return NULL;
    }
    base_len -= 7;
    char output_name[PATH_MAX];
    if (base_len + 4 >= sizeof(output_name)) {
      shape_remove_tree(root);
      return NULL;
    }
    memcpy(output_name, base, base_len);
    memcpy(output_name + base_len, ".ny", 4);
    if (snprintf(output, sizeof(output), "%s/%s", root, output_name) >=
        (int)sizeof(output)) {
      shape_remove_tree(root);
      return NULL;
    }
    return strdup(output);
  }

  char *generator = shape_source_block(shape_path, "python");
  if (!generator) {
    fprintf(stderr,
            "ny-test: %s: missing 'source ny' or 'source python' block\n",
            disp_path(shape_path));
    return NULL;
  }
  const char *python = test_command_available("python3")  ? "python3"
                       : test_command_available("python") ? "python"
                                                          : NULL;
  if (!python) {
    fprintf(
        stderr,
        "ny-test: %s: Python generator requires python3 or python in PATH\n",
        disp_path(shape_path));
    return NULL;
  }

  char output_path[PATH_MAX];
  int output_fd =
      make_test_capture_tmp(output_path, sizeof(output_path), "shape-ny");
  if (output_fd < 0) {
    fprintf(stderr, "ny-test: %s: cannot create Python generator output\n",
            disp_path(shape_path));
    free(generator);
    return NULL;
  }
  close(output_fd);
  char script_path[PATH_MAX];
  int script_fd =
      make_test_capture_tmp(script_path, sizeof(script_path), "shape-py");
  if (script_fd < 0) {
    free(generator);
    remove(output_path);
    return NULL;
  }
#ifdef _WIN32
  FILE *script_file = fopen(script_path, "wb");
#else
  FILE *script_file = fdopen(script_fd, "wb");
#endif
  if (!script_file ||
      fwrite(generator, 1, strlen(generator), script_file) !=
          strlen(generator) ||
      fclose(script_file) != 0) {
#ifndef _WIN32
    if (!script_file)
      close(script_fd);
#endif
    free(generator);
    remove(script_path);
    remove(output_path);
    return NULL;
  }
  free(generator);
  char *argv[] = {(char *)python, script_path, NULL};
  int rc = bench_run_capture(argv, 30, output_path);
  remove(script_path);
  if (rc != 0) {
    char *detail = read_small_file(output_path);
    fprintf(stderr, "ny-test: %s: Python generator failed (rc=%d)%s%s\n",
            disp_path(shape_path), rc, detail && *detail ? ": " : "",
            detail && *detail ? detail : "");
    free(detail);
    remove(output_path);
    return NULL;
  }
  struct stat st;
  if (stat(output_path, &st) != 0 || st.st_size == 0) {
    fprintf(stderr, "ny-test: %s: Python generator produced no Ny source\n",
            disp_path(shape_path));
    remove(output_path);
    return NULL;
  }
  return strdup(output_path);
}

static void read_error_meta(const char *path, char **flags_out,
                            char **expect_out) {
  *flags_out = NULL;
  *expect_out = NULL;
  if (path && shape_path_is_nshape(path)) {
    *flags_out = shape_meta_string(path, "flags");
    *expect_out = shape_meta_string(path, "expect_message");
    return;
  }
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[2048];
  int scanned = 0;
  while (scanned++ < 48 && fgets(line, sizeof(line), f)) {
    char *p = line;
    trim_inplace(p);
    if (strncmp(p, ";;", 2) != 0)
      continue;
    p += 2;
    trim_inplace(p);
    if (strncmp(p, "flags:", 6) == 0) {
      p += 6;
      trim_inplace(p);
      free(*flags_out);
      *flags_out = strdup(p);
    } else if (strncmp(p, "expect:", 7) == 0) {
      p += 7;
      trim_inplace(p);
      free(*expect_out);
      *expect_out = strdup(p);
    }
  }
  fclose(f);
}

static int split_words(char *s, char **out, int max) {
  int n = 0;
  char *p = s;
  while (p && *p && n < max) {
    while (*p && isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    out[n++] = p;
    while (*p && !isspace((unsigned char)*p))
      p++;
    if (*p)
      *p++ = '\0';
  }
  return n;
}

static int append_arg(char **argv, int *argc, int max, char *arg) {
  if (!argv || !argc || *argc >= max - 1)
    return 0;
  argv[(*argc)++] = arg;
  argv[*argc] = NULL;
  return 1;
}

static void debug_replay_env(void) {
  ny_setenv("NYTRIX_PROGRESS", "0", 1);
  ny_setenv("NYTRIX_JIT_CACHE", "0", 1);
  ny_setenv("NYTRIX_AOT_CACHE", "0", 1);
  ny_setenv("NYTRIX_STD_CACHE", "0", 1);
  ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", "1", 1);
}

static int run_debug_argv_capture(char *const argv[], int timeout_sec,
                                  int use_path_lookup,
                                  const char *output_path) {
#ifdef _WIN32
  debug_replay_env();
  /*
   * Replay output feeds failure reports: keep the compiler's own
   * diagnostics colored when the runner is on a terminal (the capture
   * file flips isatty off, which would strip the styling).
   */
  if (isatty(STDERR_FILENO))
    ny_setenv("FORCE_COLOR", "1", 1);
  enable_core_dumps();
  (void)use_path_lookup;
  ny_test_proc_t proc = ny_test_spawn_argv(argv, output_path, 0);
  if (!ny_test_proc_valid(proc))
    return 127;
  int timed_out = 0;
  int rc = ny_test_wait_rc(proc, timeout_sec, &timed_out);
  ny_test_proc_close(proc);
  return timed_out ? NY_TEST_TIMEOUT_RC : rc;
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    ny_test_isolate_child_group();
    debug_replay_env();
    /*
     * Replay output feeds failure reports: keep the compiler's own
     * diagnostics colored when the runner is on a terminal (the capture
     * file flips isatty off, which would strip the styling).
     */
    if (isatty(STDERR_FILENO))
      ny_setenv("FORCE_COLOR", "1", 1);
    enable_core_dumps();
    int out_fd = output_path
                     ? open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                     : STDERR_FILENO;
    if (out_fd >= 0) {
      dup2(out_fd, STDOUT_FILENO);
      dup2(out_fd, STDERR_FILENO);
      if (out_fd != fileno(stderr))
        close(out_fd);
    }
    if (out_fd >= 0) {
      dup2(out_fd, STDOUT_FILENO);
      dup2(out_fd, STDERR_FILENO);
    }
    apply_test_child_mem_limit();
    if (use_path_lookup)
      execvp(argv[0], argv);
    else
      execv(argv[0], argv);
    _exit(127);
  }
  if (pid <= 0)
    return 127;
  int status = 0;
  double start_ms = now_ms();
  double timeout_ms = (double)timeout_sec * 1000.0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      return child_status_rc(status);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return 127;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      ny_test_kill_group(pid);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      return NY_TEST_TIMEOUT_RC;
    }
    poll_sleep();
  }
#endif
}

static int run_debug_argv(char *const argv[], int timeout_sec,
                          int use_path_lookup) {
  return run_debug_argv_capture(argv, timeout_sec, use_path_lookup, NULL);
}

static int test_command_available(const char *cmd) {
  if (!cmd || !*cmd)
    return 0;
#ifdef _WIN32
  char buf[PATH_MAX];
  DWORD n = SearchPathA(NULL, cmd, NULL, (DWORD)sizeof(buf), buf, NULL);
  if (n > 0 && n < sizeof(buf))
    return 1;
  char exe[PATH_MAX];
  snprintf(exe, sizeof(exe), "%s.exe", cmd);
  n = SearchPathA(NULL, exe, NULL, (DWORD)sizeof(buf), buf, NULL);
  return n > 0 && n < sizeof(buf);
#else
  if (strchr(cmd, '/'))
    return access(cmd, X_OK) == 0;
  const char *path = getenv("PATH");
  if (!path || !*path)
    return 0;
  char *copy = strdup(path);
  if (!copy)
    return 0;
  int ok = 0;
  for (char *p = copy; p && *p;) {
    char *colon = strchr(p, ':');
    if (colon)
      *colon = '\0';
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", *p ? p : ".", cmd);
    if (access(full, X_OK) == 0) {
      ok = 1;
      break;
    }
    if (!colon)
      break;
    p = colon + 1;
  }
  free(copy);
  return ok;
#endif
}

static const char *test_debugger_name(void) {
#ifdef __APPLE__
  if (test_command_available("lldb"))
    return "lldb";
  if (test_command_available("gdb"))
    return "gdb";
#elif defined(_WIN32)
  if (test_command_available("gdb"))
    return "gdb";
  if (test_command_available("lldb"))
    return "lldb";
#else
  if (test_command_available("gdb"))
    return "gdb";
  if (test_command_available("lldb"))
    return "lldb";
#endif
  return NULL;
}

static int test_debug_failures_enabled(void) {
  if (test_env_falsey("NYTRIX_TEST_DEBUG_FAILURES"))
    return 0;
  if (test_env_truthy("NYTRIX_TEST_DEBUG_FAILURES"))
    return 1;
  /*
   * Trace replay can rerun every compile-timeout fixture serially.  Keep it
   * explicit so a normal failure-only sweep reports its authoritative result
   * promptly; CI/debug sessions can opt in with NYTRIX_TEST_DEBUG_FAILURES=1.
   */
  return 0;
}

static int test_debug_verbose(void) {
  return test_env_truthy("NYTRIX_TEST_DEBUG_VERBOSE");
}

static int test_debugger_for_rc(int rc) {
  if (test_env_truthy("NYTRIX_TEST_DEBUGGER_ALL"))
    return rc != 0;
  if (rc == NY_TEST_TIMEOUT_RC)
    return 0;
  if (rc >= 128) {
    int signal = rc - 128;
    /*
     * GDB is useful for native faults; abort-based assertion/panic failures
     * already have a precise Nytrix trace and do not need a second replay.
     */
    if (signal == SIGSEGV || signal == SIGILL || signal == SIGFPE)
      return 1;
#ifdef SIGBUS
    if (signal == SIGBUS)
      return 1;
#endif
#ifdef SIGTRAP
    if (signal == SIGTRAP)
      return 1;
#endif
    return 0;
  }
  if (rc < 0)
    return 1;
  return 0;
}

static int test_is_ownership_error_path(const char *path) {
  return path && strstr(path, "etc/tests/lang/errors/ownership/") != NULL;
}

static int test_is_optional_system_stdlib(const char *path) {
  return path && (strncmp(path, "lib/os/ui/", 10) == 0 ||
                  strncmp(path, "lib/os/sound/", 13) == 0 ||
                  strcmp(path, "lib/os/ui/mod.ny") == 0 ||
                  strcmp(path, "lib/os/sound/mod.ny") == 0 ||
                  strcmp(path, "lib/os/clipboard.ny") == 0);
}

static int test_is_unsupported_native_host(const char *path) {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  /*
   * Result-oracle execution is host-native.  AArch64 optcheck fixtures
   * intentionally remain compile/link coverage here; executing their raw
   * code requires an AArch64 runner and cannot be emulated by the x86 JIT.
   */
  if (path && shape_path_is_nshape(path)) {
    char *flags = shape_meta_string(path, "flags");
    char *matrix = shape_meta_string(path, "flags_matrix");
    int aarch64 = (flags && strstr(flags, "--native-backend aarch64")) ||
                  (matrix && strstr(matrix, "--native-backend aarch64"));
    int oracle = (flags && strstr(flags, "--native-result-oracle")) ||
                 (matrix && strstr(matrix, "--native-result-oracle"));
    free(flags);
    free(matrix);
    if (aarch64 && oracle)
      return 1;
  }
  (void)path;
  return 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (!path || !test_path_in(path, "code/") ||
      test_path_in(path, "code/probes/"))
    return 0;
  return !test_path_in(path, "code/targets/aarch64/");
#else
  return path && test_path_in(path, "code/") &&
         !test_path_in(path, "code/probes/") &&
         !test_path_in(path, "code/targets/");
#endif
}

static int test_is_unsupported_native_platform(const char *path) {
  /*
   * Platform eligibility is fixture metadata, not a filename allow/deny
   * list.  The current corpus uses `ci` for hosted-device policy and flags
   * for cross-target compile-only coverage; neither needs another path rule.
   */
  (void)path;
  return 0;
}

static void gh_group_begin(const char *kind, const char *path) {
  if (test_env_truthy("GITHUB_ACTIONS"))
    printf("::group::%s: %s\n", kind ? kind : "debug", disp_path(path));
}

static void gh_group_end(void) {
  if (test_env_truthy("GITHUB_ACTIONS"))
    printf("::endgroup::\n");
}

static int build_trace_argv(char **argv, int max, const char *bin,
                            const char *path, const char *exec_path,
                            const char *std_path, const char *std_bc,
                            const char *matrix_flags, char *core_path_arg,
                            char *flags_buf, char **flags_out,
                            char **expect_out) {
  int argc = 0;
  char *flagv[32];
  int flagc = 0;
  int has_native_backend = 0;
  int explicit_executor = 0;
  char *expected_exit = NULL;
  if (flags_out)
    *flags_out = NULL;
  if (expect_out)
    *expect_out = NULL;
  flags_buf[0] = '\0';
  char policy_flags[1024] = {0};
  read_error_meta(path, flags_out, expect_out);
  expected_exit = shape_meta_string(path, "expect_exit");
  const char *base_flags = flags_out && *flags_out ? *flags_out : NULL;
  if ((base_flags && *base_flags) || (matrix_flags && *matrix_flags)) {
    snprintf(flags_buf, 1024, "%s%s%s", base_flags ? base_flags : "",
             base_flags && *base_flags && matrix_flags && *matrix_flags ? " "
                                                                        : "",
             matrix_flags ? matrix_flags : "");
    trim_inplace(flags_buf);
    snprintf(policy_flags, sizeof(policy_flags), "%s", flags_buf);
    has_native_backend = native_backend_explicit(flags_buf);
    explicit_executor =
        flags_contain_native_backend_word(flags_buf, "--nyir-run") ||
        flags_contain_native_backend_word(flags_buf, "--nyir-run-bin") ||
        flags_contain_native_backend_word(flags_buf, "--jit") ||
        flags_contain_native_backend_word(flags_buf, "--native") ||
        flags_contain_native_backend_word(flags_buf, "--run") ||
        flags_contain_native_backend_word(flags_buf, "-run");
    flagc = split_words(flags_buf, flagv, 32);
  }
  if (!append_arg(argv, &argc, max, (char *)bin) ||
      !append_arg(argv, &argc, max, "-trace"))
    return 0;
  const char *warn_arg = test_warn_arg();
  if (warn_arg && !append_arg(argv, &argc, max, (char *)warn_arg))
    return 0;
  if (std_path) {
    snprintf(core_path_arg, PATH_MAX + 16, "--core-path=%s", std_path);
    if (!append_arg(argv, &argc, max, core_path_arg))
      return 0;
  }
  if (std_bc) {
    if (!append_arg(argv, &argc, max, "--core-bc") ||
        !append_arg(argv, &argc, max, (char *)std_bc))
      return 0;
  }
  if (path_is_probe_test(path) && !explicit_executor) {
    if (!append_arg(argv, &argc, max, "--native-only"))
      return 0;
  } else if (path_is_native_test(path) && !has_native_backend) {
    if (!append_arg(argv, &argc, max, "--native-backend") ||
        !append_arg(argv, &argc, max, "x86_64"))
      return 0;
  }
  for (int i = 0; i < flagc; i++) {
    if (!append_arg(argv, &argc, max, flagv[i]))
      return 0;
  }
  /*
   * Fixtures with expect_exit assert the program's process status. Ensure
   * these fixtures actually execute; the default native test route below is
   * compile-only (--native-only), which silently turns their runtime check
   * into a successful compile returning zero.
   */
  if (expected_exit && !explicit_executor &&
      !append_arg(argv, &argc, max, "--native"))
    return 0;
  if (expected_exit && !explicit_executor &&
      !append_arg(argv, &argc, max, "--run"))
    return 0;
  if (expected_exit)
    explicit_executor = 1;
  free(expected_exit);
  int requested_native = 0;
  if (test_path_is_benchmark(path))
    requested_native = test_env_truthy("NYTRIX_TEST_BENCHMARK_NATIVE");
  else if (test_path_is_runtime_domain(path))
    requested_native = test_env_truthy("NYTRIX_TEST_RUNTIME_NATIVE") &&
                       !shape_requests_jit(path);
  else if (path_is_stdlib_source(path))
    requested_native = test_env_truthy("NYTRIX_TEST_STD_NATIVE");
  if (requested_native && !explicit_executor &&
      !shape_expects_compile_failure(path) &&
      !native_only_explicit(policy_flags) &&
      !native_backend_is_llvm(policy_flags) &&
      !flags_contain_native_backend_word(policy_flags, "--emit-asm") &&
      !append_arg(argv, &argc, max, "--native-only"))
    return 0;
  if (shape_expects_compile_failure(path) &&
      !native_only_explicit(policy_flags) &&
      !append_arg(argv, &argc, max, "--stop-after=opt"))
    return 0;
  if (path_is_native_test(path) && !explicit_executor &&
      !shape_expects_compile_failure(path) &&
      !native_only_explicit(policy_flags) &&
      !native_backend_is_llvm(policy_flags) &&
      !flags_contain_native_backend_word(policy_flags, "--emit-asm") &&
      !append_arg(argv, &argc, max, "--native-only"))
    return 0;
  if (test_is_ownership_error_path(path)) {
    if (!append_arg(argv, &argc, max, "--ownership-strict"))
      return 0;
  }
  if (!append_arg(argv, &argc, max, "--no-progress"))
    return 0;
  return append_arg(argv, &argc, max,
                    (char *)(exec_path && *exec_path ? exec_path : path));
}

static const char *test_diag_root(void) {
  const char *root = getenv("NYTRIX_TEST_DIAG_DIR");
  return root && *root ? root : "build/diagnostics/tests";
}

static void test_diag_safe_name(const char *path, char *out, size_t out_len) {
  if (!out || out_len == 0)
    return;
  size_t n = 0;
  const unsigned char *p =
      (const unsigned char *)(path && *path ? path : "fixture");
  while (*p && n + 1 < out_len) {
    unsigned char c = *p++;
    if (isalnum(c) || c == '.' || c == '-' || c == '_')
      out[n++] = (char)c;
    else
      out[n++] = '_';
  }
  if (n == 0)
    out[n++] = 'f';
  out[n] = '\0';
  if (n > 160) {
    out[157] = '.';
    out[158] = '.';
    out[159] = '.';
    out[160] = '\0';
  }
}

static void test_diag_write_argv(FILE *f, const char *label,
                                 char *const argv[]) {
  if (!f || !argv)
    return;
  fprintf(f, "%s:\n", label ? label : "argv");
  for (int i = 0; argv[i]; ++i)
    fprintf(f, "  [%d] %s\n", i, argv[i]);
}

static void test_diag_write_environment(FILE *f) {
  static const char *keys[] = {"PATH",
                               "HOME",
                               "TMPDIR",
                               "NYTRIX_ROOT",
                               "NYTRIX_NY_FULL_BIN",
                               "NYTRIX_CORE_PATH",
                               "NYTRIX_STD_PATH",
                               "NYTRIX_BUILD_STD_PATH",
                               "NYTRIX_TEST_CORE_BC",
                               "NYTRIX_TEST_BUILD_ROOT",
                               "NYTRIX_TEST_WIN64_CC",
                               "NYTRIX_TEST_WIN64_RUNNER",
                               "NYTRIX_TEST_CORE_BC_CHECKER",
                               "NYTRIX_TEST_NY_FULL_BIN",
                               "NYTRIX_STD_CACHE",
                               "NYTRIX_JIT_CACHE",
                               "NYTRIX_AOT_CACHE",
                               "NYTRIX_TEST_CACHE",
                               "NYTRIX_TEST_NO_NATIVE_CACHE",
                               "NYTRIX_TEST_TIMEOUT",
                               "NYTRIX_TEST_WARN",
                               "NYTRIX_TRACE",
                               "NYTRIX_TRACE_CALLS",
                               "NYTRIX_TRACE_VALUES",
                               "NYTRIX_TRACE_VERBOSE",
                               "NYTRIX_TRACE_FILTER",
                               "NYTRIX_TEST_DEBUG_VERBOSE",
                               "NYTRIX_NATIVE_BACKEND",
                               "NYTRIX_HOST_TRIPLE",
                               "NYTRIX_HOST_CFLAGS",
                               "NYTRIX_HOST_LDFLAGS",
                               "CC",
                               "CFLAGS",
                               "LDFLAGS",
                               "ASAN_OPTIONS",
                               "UBSAN_OPTIONS",
                               "LD_LIBRARY_PATH",
                               "DYLD_LIBRARY_PATH",
                               "DISPLAY",
                               "WAYLAND_DISPLAY",
                               NULL};
  if (!f)
    return;
  for (size_t i = 0; keys[i]; ++i) {
    const char *value = getenv(keys[i]);
    fprintf(f, "%s=%s\n", keys[i], value ? value : "");
  }
}

static int test_diag_make_run_dir(char *out, size_t out_len) {
  const char *root = test_diag_root();
  if (!root || !*root || !out || out_len == 0)
    return 0;
  ny_ensure_dir_recursive(root);
  snprintf(out, out_len, "%s/run-%ld-%ld", root, (long)getpid(),
           (long)now_ms());
  ny_ensure_dir_recursive(out);
  return access(out, F_OK) == 0;
}

static int build_emit_argv(char **argv, int max, char *const base[],
                           const char *asm_path, const char *ir_path,
                           char *asm_flag, size_t asm_flag_len, char *ir_flag,
                           size_t ir_flag_len) {
  if (!argv || !base || !asm_path || !ir_path || max < 8)
    return 0;
  int base_count = 0;
  while (base[base_count])
    base_count++;
  if (base_count < 2)
    return 0;
  int argc = 0;
  for (int i = 0; i < base_count - 1; ++i) {
    if (!append_arg(argv, &argc, max, base[i]))
      return 0;
  }
  if (!append_arg(argv, &argc, max, "-emit-only"))
    return 0;
  snprintf(asm_flag, asm_flag_len, "--emit-asm=%s", asm_path);
  snprintf(ir_flag, ir_flag_len, "--emit-ir=%s", ir_path);
  if (!append_arg(argv, &argc, max, asm_flag) ||
      !append_arg(argv, &argc, max, ir_flag) ||
      !append_arg(argv, &argc, max, base[base_count - 1]))
    return 0;
  return 1;
}

static int run_debugger_replay(const char *debugger, char *const trace_argv[],
                               int timeout_sec, const char *output_path) {
  char *argv[192];
  int argc = 0;
  bool is_lldb = strstr(debugger, "lldb") != NULL;
  if (!append_arg(argv, &argc, 192, (char *)debugger))
    return 127;
  if (is_lldb) {
    append_arg(argv, &argc, 192, "--batch");
    append_arg(argv, &argc, 192, "-o");
    append_arg(argv, &argc, 192, "run");
    append_arg(argv, &argc, 192, "-k");
    append_arg(argv, &argc, 192,
               test_debug_verbose() ? "thread list"
                                    : "thread backtrace all -c 12");
    if (test_debug_verbose()) {
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "register read");
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "frame info");
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "frame variable -T -L");
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "disassemble --frame");
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "image list");
      append_arg(argv, &argc, 192, "-k");
      append_arg(argv, &argc, 192, "memory region $pc");
    }
    append_arg(argv, &argc, 192, "--");
  } else {
    append_arg(argv, &argc, 192, "-q");
    append_arg(argv, &argc, 192, "--batch");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "run");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192,
               test_debug_verbose() ? "thread apply all bt full"
                                    : "thread apply all bt 12");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "info registers");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "x/i $pc");
    if (test_debug_verbose()) {
      append_arg(argv, &argc, 192, "-ex");
      append_arg(argv, &argc, 192, "x/32i $pc-32");
      append_arg(argv, &argc, 192, "-ex");
      append_arg(argv, &argc, 192, "disassemble /r $pc-64,$pc+128");
      append_arg(argv, &argc, 192, "-ex");
      append_arg(argv, &argc, 192, "info files");
      append_arg(argv, &argc, 192, "-ex");
      append_arg(argv, &argc, 192, "info sharedlibrary");
      append_arg(argv, &argc, 192, "-ex");
      append_arg(argv, &argc, 192, "info proc mappings");
    }
    append_arg(argv, &argc, 192, "--args");
  }
#ifdef _WIN32
  /*
   * _spawnv leaves the launcher waiting outside the compiler process.
   */
  const char *full_bin = getenv("NYTRIX_NY_FULL_BIN");
#endif
  for (int i = 0; trace_argv[i]; i++) {
    const char *arg = trace_argv[i];
#ifdef _WIN32
    if (i == 0 && full_bin && *full_bin)
      arg = full_bin;
#endif
    if (!append_arg(argv, &argc, 192, (char *)arg))
      return 127;
  }
  int debug_timeout = timeout_sec;
  if (test_debug_verbose()) {
    debug_timeout *= 3;
    if (debug_timeout < 30)
      debug_timeout = 30;
    if (debug_timeout > 120)
      debug_timeout = 120;
  } else {
    /*
     * Compact crash replay is a bounded hint, not a second full test run.
     */
    if (debug_timeout < 8)
      debug_timeout = 8;
    if (debug_timeout > 15)
      debug_timeout = 15;
  }
  int rc = run_debug_argv_capture(argv, debug_timeout, 1, output_path);
  return rc;
}

/*
 * Keep failure replay on the same per-fixture timeout as the scheduler.  The
 * declaration lives here because replay helpers precede the path-policy
 * implementation below.
 */
static int test_timeout_for_path(const char *path, int default_timeout_sec);

static void print_core_dump_config(void) {
  if (!test_debug_verbose())
    return;
#if defined(__linux__)
  FILE *f = fopen("/proc/sys/kernel/core_pattern", "r");
  if (f) {
    char line[512];
    if (fgets(line, sizeof(line), f)) {
      trim_inplace(line);
      printf("%s[debug]%s core_pattern=%s\n", nyt_clr(NYT_GRAY),
             nyt_clr(NYT_RESET), line);
    }
    fclose(f);
  }
#elif defined(__APPLE__)
  char path[PATH_MAX];
  size_t n = sizeof(path);
  if (sysctlbyname("kern.corefile", path, &n, NULL, 0) == 0 && path[0])
    printf("%s[debug]%s corefile=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET),
           path);
#elif defined(_WIN32)
  printf("%s[debug]%s Windows crash dumps depend on WER/local dump policy; "
         "debugger replay is used when available\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
#endif
}

static void debug_replay_failed_tests(StrVec *failed_paths, const char *bin,
                                      const char *std_path, const char *std_bc,
                                      int timeout_sec) {
  if (!failed_paths || failed_paths->len == 0 || !test_debug_failures_enabled())
    return;
  char diag_run_dir[PATH_MAX] = {0};
  int have_diag_dir =
      test_diag_make_run_dir(diag_run_dir, sizeof(diag_run_dir));
  print_section("Failure Replay");
  printf("  %zu failure(s) · raw diagnostics: %s\n", failed_paths->len,
         have_diag_dir ? diag_run_dir : "unavailable");
  print_core_dump_config();
  const char *debugger = test_debugger_name();
  if (!debugger)
    printf(
        "  debugger: unavailable; trace/IR/assembly artifacts still saved\n");
  for (size_t i = 0; i < failed_paths->len; i++) {
    const char *path = failed_paths->items[i];
    int fixture_timeout = test_timeout_for_path(path, timeout_sec);
    char *materialized_path = NULL;
    const char *exec_path = path;
    if (path && shape_path_is_nshape(path)) {
      materialized_path = materialize_shape_ny_source(path);
      if (!materialized_path) {
        printf("%s[debug]%s cannot materialize shape source for %s\n",
               nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), disp_path(path));
        continue;
      }
      exec_path = materialized_path;
    }

    char *matrix = path && shape_path_is_nshape(path)
                       ? shape_meta_string(path, "flags_matrix")
                       : NULL;
    char matrix_buf[4096] = {0};
    char *rows[64];
    int rowc = 0;
    if (matrix && *matrix) {
      snprintf(matrix_buf, sizeof(matrix_buf), "%s", matrix);
      rowc = split_flag_matrix_rows(matrix_buf, rows, 64);
    }
    int variants = rowc > 0 ? rowc : 1;
    for (int variant = 0; variant < variants; variant++) {
      const char *matrix_flags = rowc > 0 ? rows[variant] : NULL;
      char core_path_arg[PATH_MAX + 16];
      char flags_buf[1024];
      char *flags = NULL;
      char *expect = NULL;
      char *trace_argv[96];
      if (!build_trace_argv(trace_argv, 96, bin, path, exec_path, std_path,
                            std_bc, matrix_flags, core_path_arg, flags_buf,
                            &flags, &expect)) {
        printf("%s[debug]%s cannot build replay argv for %s\n",
               nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), disp_path(path));
        error_meta_free(flags, expect);
        continue;
      }
      char case_name[PATH_MAX];
      char case_dir[PATH_MAX] = {0};
      char source_path[PATH_MAX] = {0};
      char meta_path[PATH_MAX] = {0};
      char env_path[PATH_MAX] = {0};
      char trace_path[PATH_MAX] = {0};
      char compile_path[PATH_MAX] = {0};
      char asm_path[PATH_MAX] = {0};
      char ir_path[PATH_MAX] = {0};
      char gdb_path[PATH_MAX] = {0};
      int have_case_dir = 0;
      if (have_diag_dir) {
        test_diag_safe_name(path, case_name, sizeof(case_name));
        snprintf(case_dir, sizeof(case_dir), "%s/%03zu-%s-v%d", diag_run_dir,
                 i + 1, case_name, variant + 1);
        ny_ensure_dir_recursive(case_dir);
        have_case_dir = access(case_dir, F_OK) == 0;
        if (have_case_dir) {
          snprintf(source_path, sizeof(source_path), "%s/source.ny", case_dir);
          snprintf(meta_path, sizeof(meta_path), "%s/metadata.txt", case_dir);
          snprintf(env_path, sizeof(env_path), "%s/environment.txt", case_dir);
          snprintf(trace_path, sizeof(trace_path), "%s/trace.txt", case_dir);
          snprintf(compile_path, sizeof(compile_path), "%s/emit.log", case_dir);
          snprintf(asm_path, sizeof(asm_path), "%s/program.s", case_dir);
          snprintf(ir_path, sizeof(ir_path), "%s/program.ll", case_dir);
          snprintf(gdb_path, sizeof(gdb_path), "%s/gdb.txt", case_dir);
          if (exec_path && ny_copy_file(exec_path, source_path) != 0)
            source_path[0] = '\0';
          FILE *ef = fopen(env_path, "w");
          if (ef) {
            test_diag_write_environment(ef);
            fclose(ef);
          }
          FILE *mf = fopen(meta_path, "w");
          if (mf) {
            fprintf(mf, "fixture=%s\n", path ? path : "");
            fprintf(mf, "display=%s\n", disp_path(path));
            fprintf(mf, "source=%s\n", exec_path ? exec_path : "");
            fprintf(mf, "variant=%d\n", variant + 1);
            fprintf(mf, "matrix_flags=%s\n", matrix_flags ? matrix_flags : "");
            fprintf(mf, "fixture_flags=%s\n", flags ? flags : "");
            fprintf(mf, "expect=%s\n", expect ? expect : "");
            fprintf(mf, "binary=%s\n", bin ? bin : "");
            fprintf(mf, "timeout_sec=%d\n", fixture_timeout);
            test_diag_write_argv(mf, "trace_argv", trace_argv);
            fclose(mf);
          }
        }
      }
      if (matrix_flags && *matrix_flags)
        printf("%s[debug]%s replay flags: %s\n", nyt_clr(NYT_GRAY),
               nyt_clr(NYT_RESET), matrix_flags);
      gh_group_begin("trace replay", path);
      int trace_rc = run_debug_argv_capture(trace_argv, fixture_timeout, 0,
                                            have_case_dir ? trace_path : NULL);
      compact_debug_print_summary("trace", path,
                                  have_case_dir ? trace_path : NULL, trace_rc);
      gh_group_end();
      /*
       * A passing replay variant needs no second compiler invocation merely
       * to save assembly. Reserve emit artifacts for the variant that
       * actually reproduces the failure.
       */
      if (have_case_dir && trace_rc != 0) {
        char *emit_argv[112];
        char asm_flag[PATH_MAX + 32];
        char ir_flag[PATH_MAX + 32];
        if (build_emit_argv(emit_argv, 112, trace_argv, asm_path, ir_path,
                            asm_flag, sizeof(asm_flag), ir_flag,
                            sizeof(ir_flag))) {
          int emit_rc = run_debug_argv_capture(emit_argv, fixture_timeout, 0,
                                               compile_path);
          if (emit_rc != 0)
            printf("    emit: rc=%d (see emit.log)\n", emit_rc);
          FILE *mf = fopen(meta_path, "a");
          if (mf) {
            fprintf(mf, "emit_rc=%d\n", emit_rc);
            test_diag_write_argv(mf, "emit_argv", emit_argv);
            fclose(mf);
          }
        }
      }
      if (trace_rc != 0 && debugger && test_debugger_for_rc(trace_rc)) {
        gh_group_begin("debugger replay", path);
        int gdb_rc = run_debugger_replay(debugger, trace_argv, fixture_timeout,
                                         have_case_dir ? gdb_path : NULL);
        compact_debug_print_summary("debugger", path,
                                    have_case_dir ? gdb_path : NULL, gdb_rc);
        if (have_case_dir) {
          FILE *mf = fopen(meta_path, "a");
          if (mf) {
            fprintf(mf, "debugger=%s\n", debugger);
            fprintf(mf, "debugger_rc=%d\n", gdb_rc);
            fclose(mf);
          }
        }
        gh_group_end();
      }
      if (have_case_dir)
        printf("    artifacts: %s\n", case_dir);
      error_meta_free(flags, expect);
    }
    free(matrix);
    if (materialized_path) {
      shape_remove_materialized(materialized_path);
      free(materialized_path);
    }
  }
}

static char *read_small_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return strdup("");
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return strdup("");
  }
  long size = ftell(f);
  if (size < 0)
    size = 0;
  if (size > 262144)
    size = 262144;
  rewind(f);
  char *buf = (char *)calloc((size_t)size + 1, 1);
  if (!buf) {
    fclose(f);
    return strdup("");
  }
  if (size > 0)
    (void)fread(buf, 1, (size_t)size, f);
  fclose(f);
  return buf;
}

/*
 * Read a whole file (any size) into a heap string.  Used for diagnostic dumps
 * (NYIR text, assembly) that can exceed the small_file cap.
 */
static char *read_whole_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)calloc((size_t)size + 1, 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (size > 0)
    (void)fread(buf, 1, (size_t)size, f);
  fclose(f);
  return buf;
}

static int repl_output_has_failure(const char *out) {
  if (!out || !*out)
    return 0;
  return strstr(out, "[E") || strstr(out, " error:") ||
         strstr(out, "PanicError") || strstr(out, "SegmentationFault") ||
         strstr(out, "Nytrix trace");
}

static char *repl_fixture_paste_body(const char *src) {
  if (!src)
    return strdup("");
  size_t n = strlen(src);
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  size_t used = 0;
  const char *p = src;
  while (*p) {
    const char *next = strchr(p, '\n');
    const char *line_end = next ? next : p + strlen(p);
    size_t line_len = (size_t)(line_end - p);
    memmove(out + used, p, line_len);
    used += line_len;
    if (next)
      out[used++] = '\n';
    if (!next)
      break;
    p = next + 1;
  }
  out[used] = '\0';
  return out;
}

static char *repl_clean_output_text(const char *out) {
  if (!out) {
    char *empty = malloc(1);
    if (empty)
      empty[0] = '\0';
    return empty;
  }
  size_t cap = strlen(out) + 1;
  char *clean = malloc(cap ? cap : 1);
  if (!clean)
    return NULL;
  size_t n = 0;
  for (const char *s = out; *s && n + 1 < cap;) {
    unsigned char ch = (unsigned char)*s++;
    if (ch == 0x1b) {
      if (*s == '[') {
        s++;
        while (*s) {
          unsigned char c = (unsigned char)*s++;
          if (c >= 0x40 && c <= 0x7e)
            break;
        }
      }
      continue;
    }
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      clean[n++] = ' ';
      continue;
    }
    if (ch < 0x20 || ch == 0x7f)
      continue;
    clean[n++] = (char)ch;
  }
  clean[n] = '\0';
  return clean;
}

static int repl_output_has_expect_line(const char *out, const char *expect) {
  if (!out || !*out || !expect || !*expect)
    return 0;
  char *clean = repl_clean_output_text(out);
  if (!clean)
    return 0;
  int ok = 1;
  size_t clean_len = strlen(clean);
  const char *p = expect;
  while (*p && ok) {
    const char *end = strchr(p, '\n');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len) {
      size_t count = 0;
      const char *hit = clean;
      while ((hit = ny_memmem(hit, clean_len - (size_t)(hit - clean), p,
                              len)) != NULL) {
        count++;
        hit += len;
      }
      size_t required = 1;
      for (const char *prior = expect; prior < p;) {
        const char *prior_end = strchr(prior, '\n');
        size_t prior_len =
            prior_end ? (size_t)(prior_end - prior) : strlen(prior);
        if (prior_len == len && memcmp(prior, p, len) == 0)
          required++;
        if (!prior_end)
          break;
        prior = prior_end + 1;
      }
      ok = count >= required;
    }
    if (!end)
      break;
    p = end + 1;
  }
  free(clean);
  return ok;
}

#ifndef _WIN32
static pid_t repl_spawn_pty(char *const argv[], int *master_fd) {
  *master_fd = -1;
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0)
    return -1;
  if (grantpt(master) != 0 || unlockpt(master) != 0) {
    close(master);
    return -1;
  }
  char *slave_name = ptsname(master);
  if (!slave_name) {
    close(master);
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    apply_test_child_env();
    ny_setenv("NYTRIX_REPL_TEST_PASTE_SUBMIT", "1", 1);
    setsid();
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0)
      _exit(127);
    (void)ioctl(slave, TIOCSCTTY, 0);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
      close(slave);
    close(master);
    execv(argv[0], argv);
    _exit(127);
  }
  if (pid < 0) {
    close(master);
    return -1;
  }
  *master_fd = master;
  return pid;
}

static void repl_append_output(char **out, size_t *len, size_t *cap,
                               const char *buf, size_t n) {
  if (!out || !len || !cap || !buf || n == 0)
    return;
  if (*len + n + 1 > *cap) {
    size_t next = *cap ? *cap : 4096;
    while (*len + n + 1 > next)
      next *= 2;
    if (next > 262144)
      next = 262144;
    if (*len + n + 1 > next)
      n = next > *len + 1 ? next - *len - 1 : 0;
    char *p = (char *)realloc(*out, next);
    if (!p)
      return;
    *out = p;
    *cap = next;
  }
  if (n == 0)
    return;
  memmove(*out + *len, buf, n);
  *len += n;
  (*out)[*len] = '\0';
}

#endif

static int run_progress_selftest(const char *bin, int timeout_sec) {
  double start_ms = now_ms();
#ifdef _WIN32
  (void)bin;
  (void)timeout_sec;
  printf("progress selftest: skipped (pty unavailable on Windows)\n");
  return 0;
#else
  char src_path[PATH_MAX];
  snprintf(src_path, sizeof(src_path), "%s/ny-progress-selftest-%ld-XXXXXX.ny",
           nyt_temp_dir(), (long)getpid());
  int src_fd = mkstemps(src_path, 3);
  if (src_fd < 0) {
    printf("progress selftest: mkstemps failed\n");
    return 1;
  }
  const char src[] = "1 + 2 * 3\n";
  if (write(src_fd, src, sizeof(src) - 1) != (ssize_t)(sizeof(src) - 1)) {
    close(src_fd);
    remove(src_path);
    printf("progress selftest: source write failed\n");
    return 1;
  }
  close(src_fd);

  char *argv[12];
  int argc = 0;
  argv[argc++] = (char *)bin;
  argv[argc++] = "--progress";
  argv[argc++] = "-emit-only";
  argv[argc++] = src_path;
  argv[argc] = NULL;

  int master = -1;
  pid_t pid = repl_spawn_pty(argv, &master);
  if (pid <= 0 || master < 0) {
    remove(src_path);
    printf("progress selftest: pty spawn failed\n");
    return 1;
  }
  int flags = fcntl(master, F_GETFL, 0);
  if (flags >= 0)
    fcntl(master, F_SETFL, flags | O_NONBLOCK);

  char *out = NULL;
  size_t out_len = 0, out_cap = 0;
  int status = 0, timed_out = 0, exited = 0;
  double timeout_ms = (double)timeout_sec * 1000.0;
  while (!exited) {
    char buf[4096];
    ssize_t r = read(master, buf, sizeof(buf));
    if (r > 0)
      repl_append_output(&out, &out_len, &out_cap, buf, (size_t)r);
    else if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
      break;

    pid_t wr = waitpid(pid, &status, WNOHANG);
    if (wr == pid) {
      exited = 1;
      break;
    }
    if (wr < 0 && errno != EINTR)
      break;
    if (now_ms() - start_ms >= timeout_ms) {
      ny_test_kill_group(pid);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      timed_out = 1;
      break;
    }
    poll_sleep();
  }
  for (;;) {
    char buf[4096];
    ssize_t r = read(master, buf, sizeof(buf));
    if (r > 0)
      repl_append_output(&out, &out_len, &out_cap, buf, (size_t)r);
    else
      break;
  }
  close(master);
  remove(src_path);

  int rc = timed_out ? NY_TEST_TIMEOUT_RC : child_status_rc(status);
  int ok = rc == 0 && out && strstr(out, "nytrix compile") &&
           strstr(out, "completed") && !strstr(out, "Segmentation fault") &&
           !strstr(out, "Assertion failed") && !strstr(out, "JIT failed:");
  if (ok) {
    printf("progress selftest: passed in %dms\n", (int)(now_ms() - start_ms));
    free(out);
    return 0;
  }
  printf("progress selftest: failed rc=%d timed_out=%d\n", rc, timed_out);
  if (out && *out)
    fputs(out, stdout);
  free(out);
  remove(src_path);
  return 1;
#endif
}

static int run_repl_paste_case(const char *bin, const char *path,
                               const char *std_path, const char *std_bc,
                               int timeout_sec, int *dur_ms, char *why,
                               size_t why_len) {
  double start_ms = now_ms();
#ifdef _WIN32
  (void)bin;
  (void)path;
  (void)std_path;
  (void)std_bc;
  (void)timeout_sec;
  if (dur_ms)
    *dur_ms = (int)(now_ms() - start_ms);
  snprintf(why, why_len, "repl pty unavailable on Windows");
  return 2;
#else
  char *src = read_small_file(path);
  if (!src || !*src) {
    free(src);
    snprintf(why, why_len, "empty repl paste fixture");
    return 0;
  }
  char *expect = shape_meta_string(path, "repl_expect");
  char *commands = shape_meta_string(path, "repl_commands");
  if (commands && *commands && commands[strlen(commands) - 1] != '\n') {
    size_t n = strlen(commands);
    char *with_newline = (char *)realloc(commands, n + 2);
    if (!with_newline) {
      free(src);
      free(expect);
      free(commands);
      snprintf(why, why_len, "out of memory");
      return 0;
    }
    commands = with_newline;
    commands[n] = '\n';
    commands[n + 1] = '\0';
  }
  char *paste_src = repl_fixture_paste_body(src);
  if (!paste_src) {
    free(src);
    free(expect);
    free(commands);
    snprintf(why, why_len, "out of memory");
    return 0;
  }
  const char *prefix = "\033[200~";
  const char *suffix = "\033[201~\n";
  const char *quit_input = ":quit\n";
  const size_t quit_len = strlen(quit_input);
  size_t paste_len = strlen(prefix) + strlen(paste_src) + strlen(suffix);
  char *paste_input = (char *)malloc(paste_len + 1);
  if (!paste_input) {
    free(src);
    free(paste_src);
    free(expect);
    free(commands);
    snprintf(why, why_len, "out of memory");
    return 0;
  }
  snprintf(paste_input, paste_len + 1, "%s%s%s", prefix, paste_src, suffix);
  free(src);
  free(paste_src);

  char *argv[12];
  char core_path_arg[PATH_MAX + 16];
  int argc = 0;
  argv[argc++] = (char *)bin;
  argv[argc++] = "-i";
  push_test_warn_arg(argv, &argc, 12);
  if (std_path) {
    snprintf(core_path_arg, sizeof(core_path_arg), "--core-path=%s", std_path);
    argv[argc++] = core_path_arg;
  }
  if (std_bc) {
    argv[argc++] = "--core-bc";
    argv[argc++] = (char *)std_bc;
  }
  argv[argc] = NULL;

  int master = -1;
  pid_t pid = repl_spawn_pty(argv, &master);
  if (pid <= 0 || master < 0) {
    free(paste_input);
    free(commands);
    free(expect);
    snprintf(why, why_len, "pty spawn failed");
    return 0;
  }
  int flags = fcntl(master, F_GETFL, 0);
  if (flags >= 0)
    fcntl(master, F_SETFL, flags | O_NONBLOCK);

  char *out = NULL;
  size_t out_len = 0, out_cap = 0, sent_paste = 0, sent_quit = 0;
  size_t sent_commands = 0;
  size_t commands_len = commands ? strlen(commands) : 0;
  int status = 0, timed_out = 0, exited = 0;
  int quit_requested = 0;
  double paste_sent_ms = 0.0;
  double timeout_ms = (double)timeout_sec * 1000.0;
  double quit_grace_ms = timeout_ms * 0.25;
  if (quit_grace_ms < 3000.0)
    quit_grace_ms = 3000.0;
  if (quit_grace_ms > 10000.0)
    quit_grace_ms = 10000.0;
  while (!exited) {
    while (sent_paste < paste_len) {
      ssize_t w =
          write(master, paste_input + sent_paste, paste_len - sent_paste);
      if (w > 0) {
        sent_paste += (size_t)w;
        if (sent_paste == paste_len)
          paste_sent_ms = now_ms();
        continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        break;
      break;
    }
    /*
     * Let the pasted source finish before sending REPL meta commands.  The
     * terminal can otherwise deliver both buffers in one read, causing the
     * command text to be consumed as part of the bracketed paste.
     */
    if (commands_len && sent_paste == paste_len &&
        now_ms() - paste_sent_ms >= 100.0) {
      while (sent_commands < commands_len) {
        ssize_t w = write(master, commands + sent_commands,
                          commands_len - sent_commands);
        if (w > 0) {
          sent_commands += (size_t)w;
          continue;
        }
        if (w < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
          break;
        break;
      }
    }
    while (quit_requested && sent_quit < quit_len) {
      ssize_t w = write(master, quit_input + sent_quit, quit_len - sent_quit);
      if (w > 0) {
        sent_quit += (size_t)w;
        continue;
      }
      if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        break;
      break;
    }

    char buf[4096];
    for (;;) {
      ssize_t r = read(master, buf, sizeof(buf));
      if (r > 0) {
        repl_append_output(&out, &out_len, &out_cap, buf, (size_t)r);
        continue;
      }
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }

    if (!quit_requested && sent_paste == paste_len &&
        sent_commands == commands_len) {
      const char *cur = out ? out : "";
      /*
       * The fixture marker is printed by its final source statement.  Waiting
       * for a prompt as well is brittle across prompt/color modes and used to
       * burn the full 10s quit grace on every successful cold REPL fixture.
       */
      int saw_expect_now = repl_output_has_expect_line(cur, expect);
      if (saw_expect_now ||
          (paste_sent_ms > 0.0 && now_ms() - paste_sent_ms >= quit_grace_ms))
        quit_requested = 1;
    }

    pid_t wr = waitpid(pid, &status, WNOHANG);
    if (wr == pid) {
      exited = 1;
      break;
    }
    if (wr < 0 && errno != EINTR) {
      status = 127;
      exited = 1;
      break;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      timed_out = 1;
      ny_test_kill_group(pid);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      exited = 1;
      break;
    }
    poll_sleep();
  }
  char buf[4096];
  for (;;) {
    ssize_t r = read(master, buf, sizeof(buf));
    if (r > 0) {
      repl_append_output(&out, &out_len, &out_cap, buf, (size_t)r);
      continue;
    }
    if (r < 0 && errno == EINTR)
      continue;
    break;
  }
  close(master);
  free(paste_input);
  free(commands);
  if (dur_ms)
    *dur_ms = (int)(now_ms() - start_ms);
  int rc = timed_out ? NY_TEST_TIMEOUT_RC : child_status_rc(status);
  int saw_expect = repl_output_has_expect_line(out ? out : "", expect);
  int failed =
      timed_out || (!saw_expect && (repl_output_has_failure(out) || rc != 0));
  if (!failed && expect && !saw_expect) {
    snprintf(why, why_len, "missing repl output: %s", expect);
    failed = 1;
  }
  if (failed) {
    if (timed_out)
      snprintf(why, why_len, "timeout=%ds", timeout_sec);
    else if (!why[0])
      snprintf(why, why_len, "rc=%d", rc);
    free(expect);
    free(out);
    return 0;
  }
  free(expect);
  free(out);
  return 1;
#endif
}

static int run_error_case(const char *bin, const char *path,
                          const char *std_path, const char *std_bc,
                          int timeout_sec, const char *flags,
                          const char *expect, int *dur_ms, char *why,
                          size_t why_len) {
  char *materialized_path = NULL;
  const char *exec_path = path;
  if (path && shape_path_is_nshape(path)) {
    materialized_path = materialize_shape_ny_source(path);
    if (!materialized_path) {
      snprintf(why, why_len, "shape source ny block missing");
      return 0;
    }
    exec_path = materialized_path;
  }
  char flags_buf[1024];
  flags_buf[0] = '\0';
  if (flags && *flags) {
    snprintf(flags_buf, sizeof(flags_buf), "%s", flags);
    trim_inplace(flags_buf);
  }
  bool ownership_case =
      path && (strstr(path, "etc/tests/lang/errors/ownership/") != NULL ||
               strstr(path, "etc/tests/lang/memory/") != NULL);
  char *flagv[32];
  int flagc = split_words(flags_buf, flagv, 32);

  char *argv[80];
  char core_path_arg[PATH_MAX + 16];
  int argc = 0;
  argv[argc++] = (char *)bin;
  if (std_path) {
    snprintf(core_path_arg, sizeof(core_path_arg), "--core-path=%s", std_path);
    argv[argc++] = core_path_arg;
  }
  if (std_bc) {
    argv[argc++] = "--core-bc";
    argv[argc++] = (char *)std_bc;
  }
  for (int i = 0; i < flagc && argc < 76; i++)
    argv[argc++] = flagv[i];
  if (ownership_case && argc < 76)
    argv[argc++] = "--ownership-strict";
  argv[argc++] = (char *)exec_path;
  argv[argc] = NULL;

  char tmp[PATH_MAX];
#ifdef _WIN32
  char tmp_dir[PATH_MAX];
  DWORD tmp_len = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
  if (tmp_len == 0 || tmp_len >= sizeof(tmp_dir))
    snprintf(tmp_dir, sizeof(tmp_dir), ".\\");
  static volatile LONG err_seq = 0;
  LONG seq = InterlockedIncrement((volatile LONG *)&err_seq);
  snprintf(tmp, sizeof(tmp), "%sny-error-%lu-%lu-%ld.log", tmp_dir,
           (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
           (long)seq);
#else
  snprintf(tmp, sizeof(tmp), "%s/ny-error-%ld-XXXXXX", nyt_temp_dir(),
           (long)getpid());
  int fd = mkstemp(tmp);
  if (fd < 0) {
    snprintf(why, why_len, "mkstemp failed");
    if (materialized_path) {
      shape_remove_materialized(materialized_path);
      free(materialized_path);
    }
    return 0;
  }
#endif

  double start_ms = now_ms();
#ifdef _WIN32
  char *old_ownership = NULL;
  char *old_ownership_strict = NULL;
  int had_ownership = 0;
  int had_ownership_strict = 0;
  if (ownership_case) {
    const char *v = getenv("NYTRIX_OWNERSHIP");
    if (v) {
      old_ownership = strdup(v);
      had_ownership = 1;
    }
    v = getenv("NYTRIX_OWNERSHIP_STRICT");
    if (v) {
      old_ownership_strict = strdup(v);
      had_ownership_strict = 1;
    }
    ny_setenv("NYTRIX_OWNERSHIP", "1", 1);
    ny_setenv("NYTRIX_OWNERSHIP_STRICT", "1", 1);
  }
  ny_test_proc_t pid = ny_test_spawn_argv(argv, tmp, 0);
  if (ownership_case) {
    if (had_ownership)
      ny_setenv("NYTRIX_OWNERSHIP", old_ownership ? old_ownership : "", 1);
    else
      ny_unsetenv("NYTRIX_OWNERSHIP");
    if (had_ownership_strict)
      ny_setenv("NYTRIX_OWNERSHIP_STRICT",
                old_ownership_strict ? old_ownership_strict : "", 1);
    else
      ny_unsetenv("NYTRIX_OWNERSHIP_STRICT");
    free(old_ownership);
    free(old_ownership_strict);
  }
  if (!ny_test_proc_valid(pid)) {
    remove(tmp);
    if (materialized_path) {
      shape_remove_materialized(materialized_path);
      free(materialized_path);
    }
    snprintf(why, why_len, "spawn failed");
    return 0;
  }
  int timed_out = 0;
  int rc = ny_test_wait_rc(pid, timeout_sec, &timed_out);
  ny_test_proc_close(pid);
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    ny_test_isolate_child_group();
    apply_test_child_env();
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    if (ownership_case) {
      ny_setenv("NYTRIX_OWNERSHIP", "1", 1);
      ny_setenv("NYTRIX_OWNERSHIP_STRICT", "1", 1);
    }
    apply_test_child_mem_limit();
    execv(bin, argv);
    _exit(127);
  }

  if (pid <= 0) {
    close(fd);
    unlink(tmp);
    if (materialized_path) {
      shape_remove_materialized(materialized_path);
      free(materialized_path);
    }
    snprintf(why, why_len, "fork failed");
    return 0;
  }

  int status = 0;
  double timeout_ms = (double)timeout_sec * 1000.0;
  int timed_out = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      break;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      unlink(tmp);
      if (materialized_path) {
        shape_remove_materialized(materialized_path);
        free(materialized_path);
      }
      snprintf(why, why_len, "wait failed");
      return 0;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      timed_out = 1;
      ny_test_kill_group(pid);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      break;
    }
    poll_sleep();
  }
  int rc = child_status_rc(status);
#endif
  if (dur_ms)
    *dur_ms = (int)(now_ms() - start_ms);
#ifndef _WIN32
  close(fd);
#endif
  char *out = read_small_file(tmp);
  remove(tmp);
  if (materialized_path) {
    shape_remove_materialized(materialized_path);
    free(materialized_path);
  }

  if (timed_out) {
    snprintf(why, why_len, "timeout=%ds", timeout_sec);
    free(out);
    return 0;
  }

  if (rc == 0) {
    const char *last_output = out && *out ? out : "<empty>";
    for (const char *line = last_output; *line; ++line) {
      if (*line == '\n' && line[1])
        last_output = line + 1;
    }
    size_t last_output_len = strlen(last_output);
    while (last_output_len > 0 && (last_output[last_output_len - 1] == '\n' ||
                                   last_output[last_output_len - 1] == '\r'))
      last_output_len--;
    if (last_output_len > 180)
      last_output_len = 180;
    snprintf(why, why_len,
             "expected compile failure, got rc=0 (last output: %.*s)",
             (int)last_output_len, last_output);
    free(out);
    return 0;
  }

  if (expect && *expect && !strstr(out, expect)) {
    char *nl = strchr(out, '\n');
    if (nl)
      *nl = '\0';
    snprintf(why, why_len,
             "missing expected diagnostic: %s (rc=%d, first output: %.180s)",
             expect, rc, out && *out ? out : "<empty>");
    free(out);
    return 0;
  }

  free(out);
  return 1;
}

static int run_suite(const char *suite_name, StrVec *files, const char *bin,
                     const char *std_path, const char *std_bc, StrVec *patterns,
                     int jobs, int timeout_sec, int *passed, int *failed,
                     int fail_fast, int force_compile_fail, SuiteStats *stats,
                     SuiteStats *benchmark_stats, SuiteStats *runtime_stats,
                     SuiteStats *native_stats, SuiteStats *interop_stats,
                     SuiteStats *probe_stats, SuiteStats *std_stats,
                     CacheDb *cache, int use_cache, TimingVec *timings,
                     StrVec *failed_paths, const char *cache_path);

static const char *test_path_basename(const char *path) {
  const char *slash = strrchr(path, '/');
#ifdef _WIN32
  const char *backslash = strrchr(path, '\\');
  if (backslash && (!slash || backslash > slash))
    slash = backslash;
#endif
  return slash ? slash + 1 : path;
}

static bool test_path_contains(const char *path, const char *pattern) {
  for (const char *start = path;; ++start) {
    const char *at = start;
    const char *needle = pattern;
    while (*at && *needle) {
      char path_char = *at == '\\' ? '/' : *at;
      char pattern_char = *needle == '\\' ? '/' : *needle;
      if (path_char != pattern_char)
        break;
      ++at;
      ++needle;
    }
    if (!*needle)
      return true;
    if (!*start)
      return false;
  }
}

static int run_error_suite(StrVec *files, const char *bin, const char *std_path,
                           const char *std_bc, StrVec *patterns, int jobs,
                           int timeout_sec, int *passed, int *failed,
                           SuiteStats *stats, CacheDb *cache, int use_cache,
                           TimingVec *timings, StrVec *failed_paths,
                           int fail_fast, const char *cache_path) {
  if (!files || files->len == 0)
    return 0;
  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (test_path_contains(p, patterns->items[k])) {
          match = 1;
          break;
        }
      }
    }
    if (match)
      sv_push(&selected, p);
  }
  if (selected.len == 0) {
    sv_free(&selected);
    return 0;
  }
  qsort(selected.items, selected.len, sizeof(char *), path_lex_cmp);

  StrVec parallel = {0};
  StrVec legacy = {0};
  for (size_t i = 0; i < selected.len; i++) {
    const char *p = selected.items[i];
    if (shape_path_is_nshape(p))
      sv_push(&parallel, p);
    else
      sv_push(&legacy, p);
  }
  sv_free(&selected);

  int rc = 0;
  if (parallel.len > 0) {
    rc = run_suite("Error", &parallel, bin, std_path, std_bc, patterns, jobs,
                   timeout_sec, passed, failed, fail_fast, 1, stats, NULL, NULL,
                   NULL, NULL, NULL, NULL, cache, use_cache, timings,
                   failed_paths, cache_path);
  }
  sv_free(&parallel);
  if (legacy.len == 0) {
    sv_free(&legacy);
    return rc;
  }

  selected = legacy;
  print_section("Error");
  for (size_t i = 0; i < selected.len; i++) {
    const char *p = selected.items[i];
    char *flags = NULL, *expect = NULL;
    read_error_meta(p, &flags, &expect);
    char *matrix =
        shape_path_is_nshape(p) ? shape_meta_string(p, "flags_matrix") : NULL;
    if (!expect)
      expect = strdup("error");
    uint64_t sig = test_sig(p, bin, std_path, std_bc);
    const char *mode = "error-v2";
    sig = fnv1a_update(sig, mode, strlen(mode));
    if (flags)
      sig = fnv1a_update(sig, flags, strlen(flags));
    if (matrix && *matrix)
      sig = fnv1a_update(sig, matrix, strlen(matrix));
    if (expect)
      sig = fnv1a_update(sig, expect, strlen(expect));
    free(matrix);
    int pct = (int)(((i + 1) * 100) / (selected.len ? selected.len : 1));
    if (use_cache) {
      CacheRow *row = cache_find(cache, p);
      if (row && row->ok == 1 && row->sig == sig) {
        int dur = row->dur_ms;
        (*passed)++;
        if (stats) {
          stats->tests++;
          stats->passed++;
          stats->sum_ms += dur;
          if (dur > stats->max_ms)
            stats->max_ms = dur;
        }
        timings_push(timings, p, dur, "Error", "pass");
        char time_label[32];
        format_test_time(time_label, sizeof(time_label), -1);
        char matrix_suffix[48];
        const char *matrix_detail = NULL;
        int matrix_count = test_flag_matrix_count(p);
        if (matrix_count > 1) {
          snprintf(matrix_suffix, sizeof(matrix_suffix), "(%d variants)",
                   matrix_count);
          matrix_detail = matrix_suffix;
        }
        print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓",
                                 NYT_GREEN, time_label, p, matrix_detail);
        error_meta_free(flags, expect);
        continue;
      }
    }
    char why[512];
    why[0] = '\0';
    int dur = 0;
    int matrix_count = test_flag_matrix_count(p);
    if (matrix_count < 1)
      matrix_count = 1;
    char *matrix_value =
        shape_path_is_nshape(p) ? shape_meta_string(p, "flags_matrix") : NULL;
    char matrix_buf[4096] = {0};
    char *matrix_rows[64];
    int row_count = 0;
    if (matrix_value && *matrix_value) {
      snprintf(matrix_buf, sizeof(matrix_buf), "%s", matrix_value);
      row_count = split_flag_matrix_rows(matrix_buf, matrix_rows, 64);
    }
    free(matrix_value);
    int variants = row_count > 0 ? row_count : 1;
    int ok = 1;
    for (int variant = 0; variant < variants; variant++) {
      char variant_flags[1024];
      const char *row = row_count > 0 ? matrix_rows[variant] : NULL;
      snprintf(variant_flags, sizeof(variant_flags), "%s%s%s",
               flags ? flags : "", flags && *flags && row && *row ? " " : "",
               row ? row : "");
      int variant_dur = 0;
      char variant_why[512] = {0};
      if (!run_error_case(bin, p, std_path, std_bc, timeout_sec, variant_flags,
                          expect, &variant_dur, variant_why,
                          sizeof(variant_why))) {
        snprintf(why, sizeof(why), "variant %d/%d (%s): %s", variant + 1,
                 variants, row && *row ? row : "base flags",
                 variant_why[0] ? variant_why : "error mismatch");
        ok = 0;
        dur += variant_dur;
        break;
      }
      dur += variant_dur;
    }
    matrix_count = variants;
    if (ok) {
      (*passed)++;
      if (stats)
        stats->passed++;
      char time_label[32];
      format_test_time(time_label, sizeof(time_label), dur);
      char matrix_suffix[48];
      const char *matrix_detail = NULL;
      if (matrix_count > 1) {
        snprintf(matrix_suffix, sizeof(matrix_suffix), "(%d variants)",
                 matrix_count);
        matrix_detail = matrix_suffix;
      }
      print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓",
                               NYT_GREEN, time_label, p, matrix_detail);
    } else {
      (*failed)++;
      sv_push_unique(failed_paths, p);
      char time_label[32];
      char suffix[640];
      format_test_time(time_label, sizeof(time_label), dur);
      snprintf(suffix, sizeof(suffix), "(%s)", why[0] ? why : "error mismatch");
      print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗", NYT_RED,
                               time_label, p, suffix);
    }
    if (cache)
      cache_set(cache, p, sig, ok ? 1 : 0, dur);
    if (use_cache && cache)
      cache_save(cache, cache_path);
    if (stats) {
      stats->tests++;
      stats->sum_ms += dur;
      if (dur > stats->max_ms)
        stats->max_ms = dur;
    }
    timings_push(timings, p, dur, "Error",
                 ok ? "pass"
                    : (strncmp(why, "timeout=", 8) == 0 ? "timeout" : "fail"));
    error_meta_free(flags, expect);
    if (fail_fast && !ok) {
      printf("[fail-fast] stopped after %s; remaining fixtures were not run\n",
             disp_path(p));
      break;
    }
  }
  sv_free(&selected);
  return 0;
}

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static double host_ram_gib(void);
static double host_ram_available_gib(void);

static int auto_test_jobs(void) {
  long logical = ny_cpu_count();
  if (logical <= 0)
    logical = 1;
  /*
   * Use all logical CPUs unless the available-memory budget limits workers.
   */
  int jobs = logical;
  /*
   * Budget automatic workers against memory available right now. The latest
   * full suite peaked at 2.2 GiB per fixture; budget 2.5 GiB per worker and
   * reserve 8 GiB for the host. Explicit --jobs still passes through the
   * independent memory guard below.
   */
  double avail_gib = host_ram_available_gib();
  if (avail_gib > 0.0) {
    int ram_jobs = (int)((avail_gib - NY_TEST_HOST_MEMORY_RESERVE_GIB) /
                         NY_TEST_WORKER_MEMORY_GIB);
    if (ram_jobs < 1)
      ram_jobs = 1;
    if (jobs > ram_jobs)
      jobs = ram_jobs;
  }
  return jobs;
}

static int normalize_test_timeout(int timeout_sec) {
  if (timeout_sec <= 0)
    return NY_TEST_DEFAULT_TIMEOUT_SEC;
  if (timeout_sec > NY_TEST_MAX_TIMEOUT_SEC)
    return NY_TEST_MAX_TIMEOUT_SEC;
  return timeout_sec;
}

static uint64_t fnv1a_update(uint64_t h, const void *ptr, size_t n) {
  const unsigned char *p = (const unsigned char *)ptr;
  for (size_t i = 0; i < n; i++) {
    h ^= (uint64_t)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/*
 * Content hash of the compiler binary.  mtime/size alone can survive a
 * rebuild (coarse timestamps, same-size relinks), which would let the
 * result cache serve a stale row computed against an older compiler.
 */
static uint64_t file_content_hash(const char *p) {
  uint64_t h = 1469598103934665603ULL;
  if (!p)
    return 0;
  FILE *f = fopen(p, "rb");
  if (!f)
    return 0;
  unsigned char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    h = fnv1a_update(h, buf, n);
  fclose(f);
  return h;
}

static uint64_t test_sig(const char *path, const char *bin,
                         const char *std_path, const char *std_bc) {
  uint64_t h = 1469598103934665603ULL;
  static uint64_t bin_hash = 0;
  static bool bin_hash_ready = false;
  static const char *std_path_key = NULL;
  static const char *std_bc_key = NULL;
  static uint64_t std_path_hash = 0;
  static uint64_t std_bc_hash = 0;
  if (!bin_hash_ready) {
    bin_hash = file_content_hash(bin);
    bin_hash_ready = true;
  }
  if (std_path_key != std_path) {
    std_path_key = std_path;
    std_path_hash = file_content_hash(std_path);
  }
  if (std_bc_key != std_bc) {
    std_bc_key = std_bc;
    std_bc_hash = file_content_hash(std_bc);
  }
  /*
   * mtime/size are useful for cheap diagnostics, but are not a correctness
   * key: filesystems with coarse timestamps can preserve both across an
   * in-place edit.  Include content fingerprints for every input that can
   * affect the generated fixture, including the standard-library artifacts.
   * Those artifacts are immutable for one ny-test invocation, so hash each
   * once rather than rereading multi-megabyte bundles for every fixture.
   */
  uint64_t a = file_content_hash(path), b = bin_hash, c = std_path_hash,
           d = std_bc_hash;
  const char *mode = path_is_native_test(path) ? "native:x86_64" : "default";
  h = fnv1a_update(h, &a, sizeof(a));
  h = fnv1a_update(h, &b, sizeof(b));
  h = fnv1a_update(h, &bin_hash, sizeof(bin_hash));
  h = fnv1a_update(h, &c, sizeof(c));
  h = fnv1a_update(h, &d, sizeof(d));
  h = fnv1a_update(h, mode, strlen(mode));
  return h;
}

static void cache_load(CacheDb *db, const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[8192];
  while (fgets(line, sizeof(line), f)) {
    unsigned long long sig = 0;
    int ok = 0, ms = 0;
    char p[8192];
    p[0] = '\0';
    if (sscanf(line, "%llx\t%d\t%d\t%1023[^\n]", &sig, &ok, &ms, p) == 4)
      cache_set(db, p, (uint64_t)sig, ok, ms);
  }
  fclose(f);
}

static void cache_save(CacheDb *db, const char *path) {
  char tmp_path[PATH_MAX];
  if (!db || !path || !*path ||
      snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld", path,
               (long)getpid()) >= (int)sizeof(tmp_path))
    return;
  FILE *f = fopen(tmp_path, "w");
  if (!f)
    return;
  for (size_t i = 0; i < db->len; i++)
    fprintf(f, "%llx\t%d\t%d\t%s\n", (unsigned long long)db->items[i].sig,
            db->items[i].ok, db->items[i].dur_ms, db->items[i].path);
  if (fclose(f) != 0) {
    remove(tmp_path);
    return;
  }
#ifdef _WIN32
  remove(path);
#endif
  if (rename(tmp_path, path) != 0)
    remove(tmp_path);
}

static const char *disp_path(const char *p) {
  if (!p)
    return "";
  if (strncmp(p, "etc/tests/", 10) == 0)
    return p + 10;
  return p;
}

static const char *host_os_name(void) {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

static const char *host_arch_name(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  return "amd64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#else
  return "unknown";
#endif
}

static void host_cpu_name(char *buf, size_t cap) {
  if (!buf || cap == 0)
    return;
  snprintf(buf, cap, "unknown");
#ifdef _WIN32
  const char *id = getenv("PROCESSOR_IDENTIFIER");
  if (id && *id)
    snprintf(buf, cap, "%s", id);
#elif defined(__APPLE__)
  size_t n = cap;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &n, NULL, 0) != 0 || !*buf)
    snprintf(buf, cap, "unknown");
#elif defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "model name", 10) != 0 &&
        strncmp(line, "Hardware", 8) != 0)
      continue;
    char *colon = strchr(line, ':');
    if (!colon)
      continue;
    colon++;
    trim_inplace(colon);
    snprintf(buf, cap, "%s", colon);
    break;
  }
  fclose(f);
#endif
  char *core_suffix = strstr(buf, "-Core Processor");
  if (core_suffix) {
    char *start = core_suffix;
    while (start > buf && start[-1] >= '0' && start[-1] <= '9')
      start--;
    if (start > buf && start[-1] == ' ') {
      start[-1] = '\0';
      trim_inplace(buf);
      return;
    }
  }
  const char *suffixes[] = {" Processor", " CPU"};
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    size_t len = strlen(buf);
    size_t suffix_len = strlen(suffixes[i]);
    if (len > suffix_len && strcmp(buf + len - suffix_len, suffixes[i]) == 0) {
      buf[len - suffix_len] = '\0';
      trim_inplace(buf);
      break;
    }
  }
}

static void host_core_counts(int *physical, int *logical) {
  int l = (int)ny_cpu_count();
  int p = l > 1 ? l / 2 : l;
#ifdef _WIN32
  DWORD len = 0;
  GetLogicalProcessorInformation(NULL, &len);
  if (len > 0) {
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION *info =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION *)malloc(len);
    if (info && GetLogicalProcessorInformation(info, &len)) {
      int cores = 0;
      int threads = 0;
      size_t count = len / sizeof(*info);
      for (size_t i = 0; i < count; i++) {
        if (info[i].Relationship != RelationProcessorCore)
          continue;
        cores++;
        ULONG_PTR mask = info[i].ProcessorMask;
        while (mask) {
          threads += (int)(mask & 1u);
          mask >>= 1;
        }
      }
      if (cores > 0)
        p = cores;
      if (threads > 0)
        l = threads;
    }
    free(info);
  }
#elif defined(__APPLE__)
  int val = 0;
  size_t n = sizeof(val);
  if (sysctlbyname("hw.physicalcpu", &val, &n, NULL, 0) == 0 && val > 0)
    p = val;
  val = 0;
  n = sizeof(val);
  if (sysctlbyname("hw.logicalcpu", &val, &n, NULL, 0) == 0 && val > 0)
    l = val;
#elif defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f) {
    int cpu_cores = 0;
    int siblings = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
      if (strncmp(line, "cpu cores", 9) == 0) {
        char *colon = strchr(line, ':');
        int v = 0;
        if (colon && ny_parse_int(colon + 1, &v) && v > 0)
          cpu_cores = v;
      } else if (strncmp(line, "siblings", 8) == 0) {
        char *colon = strchr(line, ':');
        int v = 0;
        if (colon && ny_parse_int(colon + 1, &v) && v > 0)
          siblings = v;
      }
      if (cpu_cores > 0 && siblings > 0)
        break;
    }
    fclose(f);
    if (cpu_cores > 0 && siblings > 0 && l > 0) {
      int sockets = siblings > 0 ? l / siblings : 1;
      if (sockets < 1)
        sockets = 1;
      p = cpu_cores * sockets;
    }
  }
#endif
  if (p <= 0)
    p = l > 0 ? l : 1;
  if (l <= 0)
    l = p;
  if (physical)
    *physical = p;
  if (logical)
    *logical = l;
}

static double host_ram_gib(void);

/*
 * Memory currently available to new workloads (not total RAM).  Automatic
 * worker budgets must be sized against this value: a desktop already holds
 * several GiB, the host may run with no swap at all, and overshooting
 * available memory hangs the machine instead of paging.
 */
static double host_ram_available_gib(void) {
#ifdef _WIN32
  MEMORYSTATUSEX st;
  memset(&st, 0, sizeof(st));
  st.dwLength = sizeof(st);
  if (GlobalMemoryStatusEx(&st))
    return (double)st.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
#elif defined(__APPLE__)
  return host_ram_gib();
#elif defined(__linux__)
  FILE *f = fopen("/proc/meminfo", "r");
  if (f) {
    char ln[256];
    double avail_kib = 0.0;
    while (fgets(ln, sizeof(ln), f)) {
      if (strncmp(ln, "MemAvailable:", 13) == 0) {
        avail_kib = strtod(ln + 13, NULL);
        break;
      }
    }
    fclose(f);
    if (avail_kib > 0.0)
      return avail_kib / (1024.0 * 1024.0);
  }
#endif
  return 0.0;
}

static double host_ram_gib(void) {
#ifdef _WIN32
  MEMORYSTATUSEX st;
  memset(&st, 0, sizeof(st));
  st.dwLength = sizeof(st);
  if (GlobalMemoryStatusEx(&st))
    return (double)st.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
#elif defined(__APPLE__)
  uint64_t mem = 0;
  size_t n = sizeof(mem);
  if (sysctlbyname("hw.memsize", &mem, &n, NULL, 0) == 0)
    return (double)mem / (1024.0 * 1024.0 * 1024.0);
#elif defined(__linux__)
  struct sysinfo si;
  if (sysinfo(&si) == 0)
    return ((double)si.totalram * (double)si.mem_unit) /
           (1024.0 * 1024.0 * 1024.0);
#endif
  return 0.0;
}

static void print_host_line(int jobs) {
  char cpu[256];
  host_cpu_name(cpu, sizeof(cpu));
  int physical = 0;
  int logical = 0;
  host_core_counts(&physical, &logical);
  double ram = host_ram_gib();
  printf(
      "%s[host]%s os=%s arch=%s cpu=%s cores=%d/%d ram=%.1f GiB jobs=%d/%d\n",
      nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), host_os_name(), host_arch_name(),
      cpu, physical, logical, ram, jobs > 0 ? jobs : 1,
      logical > 0 ? logical : 1);
}

static int flags_contain_native_backend_word(const char *flags,
                                             const char *word) {
  if (!flags || !word || !*word)
    return 0;
  size_t n = strlen(word);
  const char *p = flags;
  while ((p = strstr(p, word)) != NULL) {
    int left_ok = (p == flags) || isspace((unsigned char)p[-1]);
    char right = p[n];
    int right_ok =
        right == '\0' || isspace((unsigned char)right) || right == '=';
    if (left_ok && right_ok)
      return 1;
    p += n;
  }
  return 0;
}

/*
 * Cross-target diagnostic fixtures (tier reports, emit-only matrices) are
 * compile contracts: the compiler must lower the same program for every
 * listed target even though it cannot link or execute a non-host object.
 * When the native-only executor asks for object emission that the target
 * cannot provide, succeed after the requested diagnostic artifact exists.
 */
static __attribute__((unused)) int
cross_target_diagnostic_emit_ok(const char *flags) {
  if (!flags || !*flags)
    return 0;
  int report =
      flags_contain_native_backend_word(flags, "--native-tier-report") ||
      flags_contain_native_backend_word(flags, "--nyir-dump") ||
      flags_contain_native_backend_word(flags, "--nyir-metadata") ||
      flags_contain_native_backend_word(flags, "--callgraph") ||
      flags_contain_native_backend_word(flags, "--emit-asm");
  if (!report)
    return 0;
  const char *dash = flags;
  while ((dash = strstr(dash, "--")) != NULL) {
    const char *eq = strchr(dash, '=');
    const char *space = strpbrk(dash, " \t");
    const char *end = space ? space : dash + strlen(dash);
    if (eq && eq < end && (size_t)(end - eq - 1) < 4000) {
      char path[4096];
      size_t len = (size_t)(end - eq - 1);
      memcpy(path, eq + 1, len);
      path[len] = '\0';
      FILE *f = fopen(path, "rb");
      if (f) {
        long size = -1;
        if (fseek(f, 0, SEEK_END) == 0)
          size = ftell(f);
        fclose(f);
        if (size > 0)
          return 1;
      }
    }
    dash += 2;
  }
  return 0;
}

static int native_backend_explicit(const char *flags) {
  return flags_contain_native_backend_word(flags, "--native-backend");
}

static int native_only_explicit(const char *flags) {
  return flags_contain_native_backend_word(flags, "--native-only");
}

static int native_backend_is_llvm(const char *flags) {
  char copy[1024];
  char *words[32];
  snprintf(copy, sizeof(copy), "%s", flags ? flags : "");
  int count = split_words(copy, words, 32);
  for (int i = 0; i < count; i++) {
    if (strncmp(words[i], "--native-backend=", 17) == 0 &&
        strcmp(words[i] + 17, "llvm") == 0)
      return 1;
    if (strcmp(words[i], "--native-backend") == 0 && i + 1 < count &&
        strcmp(words[i + 1], "llvm") == 0)
      return 1;
  }
  return 0;
}

static int path_is_probe_test(const char *p) {
  if (!p)
    return 0;
  TestDomain domain = test_metadata_domain(p);
  if (domain != TEST_DOMAIN_AUTO)
    return domain == TEST_DOMAIN_PROBE;
  if (shape_path_is_nshape(p)) {
    char *family = shape_meta_string(p, "family");
    int probe = family && (strcmp(family, "probe-suite") == 0 ||
                           strncmp(family, "probe-", 6) == 0);
    free(family);
    if (probe)
      return 1;
  }
  /*
   * Discovery may hand us a repository-relative display path, an absolute
   * path, or a materialized `.../etc/tests/...` path. Keep all probe spellings
   * on the native probe route instead of letting the initial execution fall
   * back to the legacy JIT while replay uses a different backend.
   */
  return strncmp(p, "probe/", 6) == 0 || strncmp(p, "probes/", 7) == 0 ||
         test_path_in(p, "code/probes/") || strstr(p, "/probe/") != NULL ||
         strstr(p, "/probes/") != NULL;
}

static int path_is_native_test(const char *p) {
  if (!p)
    return 0;
  TestDomain domain = test_metadata_domain(p);
  if (domain != TEST_DOMAIN_AUTO)
    return domain == TEST_DOMAIN_NATIVE;
  if (shape_path_is_nshape(p)) {
    char *family = shape_meta_string(p, "family");
    char *generator = shape_meta_string(p, "generator");
    char *backend = shape_meta_string(p, "backend");
    int native = (generator && strcmp(generator, "native") == 0) ||
                 (backend && (strcmp(backend, "native") == 0 ||
                              strcmp(backend, "aot") == 0)) ||
                 (family && (strncmp(family, "native", 6) == 0 ||
                             strncmp(family, "runtime-native", 14) == 0 ||
                             strcmp(family, "runtime-sanitize") == 0));
    free(family);
    free(generator);
    free(backend);
    return native;
  }
  return test_path_in(p, "lang/") || test_path_in(p, "code/");
}

static int path_is_repl_test(const char *p) {
  if (!p)
    return 0;
  TestDomain domain = test_metadata_domain(p);
  if (domain != TEST_DOMAIN_AUTO)
    return domain == TEST_DOMAIN_REPL;
  if (shape_path_is_nshape(p)) {
    char *family = shape_meta_string(p, "family");
    int repl = family && (strcmp(family, "repl") == 0 ||
                          strncmp(family, "repl/", 5) == 0 ||
                          strstr(family, "/repl") != NULL);
    free(family);
    if (repl)
      return 1;
  }
  return 0;
}

static int path_is_stdlib_source(const char *p) {
  if (!p)
    return 0;
  return strncmp(p, "lib/", 4) == 0 || strstr(p, "/lib/") != NULL;
}

static int test_timeout_for_path(const char *path, int default_timeout_sec) {
  int timeout = default_timeout_sec;
  char *declared =
      shape_path_is_nshape(path) ? shape_meta_string(path, "timeout") : NULL;
  if (declared && *declared) {
    int parsed = 0;
    if (ny_parse_int(declared, &parsed))
      timeout = parsed;
  }
  free(declared);
  const char *env =
      test_path_is_benchmark(path) ? getenv("NYTRIX_TEST_BENCH_TIMEOUT") : NULL;
  if (env && *env) {
    int parsed = 0;
    if (ny_parse_int(env, &parsed))
      timeout = parsed;
  }
  if (timeout < 1)
    timeout = 1;
  if (timeout > NY_TEST_MAX_TIMEOUT_SEC)
    timeout = NY_TEST_MAX_TIMEOUT_SEC;
  return timeout;
}

/*
 * UI and audio fixtures touch process-wide host resources (Vulkan/OpenGL,
 * windows, audio devices, and their driver caches).  Running those modules
 * beside another host-resource fixture can turn a healthy 2–5 second test
 * into a false per-fixture timeout.  Keep this policy in the scheduler rather
 * than relying on directory ordering or a larger global timeout.
 */
static int test_requires_host_exclusive(const char *p) {
  if (!p)
    return 0;
  return strncmp(p, "lib/os/ui/", 10) == 0 ||
         strncmp(p, "lib/os/sound/", 13) == 0;
}

static const char *suite_for_path(const char *p, const char *fallback) {
  TestDomain domain = test_metadata_domain(p);
  if (domain == TEST_DOMAIN_BENCHMARK || test_path_is_benchmark(p))
    return "Benchmark";
  if (domain == TEST_DOMAIN_ERROR)
    return "Runtime";
  if (domain == TEST_DOMAIN_RUNTIME || (p && (test_path_is_runtime_domain(p) ||
                                              test_path_is_language_domain(p))))
    return "Runtime";
  if (domain == TEST_DOMAIN_INTEROP || (p && test_path_in(p, "lang/interop/")))
    return "Interop";
  if (domain == TEST_DOMAIN_NATIVE ||
      (p && !test_path_is_runtime_domain(p) &&
       !test_path_is_language_domain(p) && !path_is_probe_test(p)))
    return "Native";
  if (path_is_probe_test(p))
    return "Probe";
  return fallback ? fallback : "Std";
}

static SuiteStats *stats_for_path(const char *p, SuiteStats *fallback,
                                  SuiteStats *benchmark, SuiteStats *runtime,
                                  SuiteStats *native, SuiteStats *interop,
                                  SuiteStats *probe, SuiteStats *std) {
  TestDomain domain = test_metadata_domain(p);
  if (domain == TEST_DOMAIN_BENCHMARK || test_path_is_benchmark(p))
    return benchmark ? benchmark : fallback;
  if (domain == TEST_DOMAIN_ERROR || domain == TEST_DOMAIN_RUNTIME ||
      (p &&
       (test_path_is_runtime_domain(p) || test_path_is_language_domain(p))))
    return runtime ? runtime : fallback;
  if (domain == TEST_DOMAIN_INTEROP || (p && test_path_in(p, "lang/interop/")))
    return interop ? interop : fallback;
  if (domain == TEST_DOMAIN_NATIVE ||
      (p && !test_path_is_runtime_domain(p) &&
       !test_path_is_language_domain(p) && !path_is_probe_test(p)))
    return native ? native : fallback;
  if (path_is_probe_test(p))
    return probe ? probe : fallback;
  return std ? std : fallback;
}

static int path_duration_hint(CacheDb *cache, const char *p) {
  CacheRow *row = cache ? cache_find(cache, p) : NULL;
  if (row && row->dur_ms > 0)
    return row->dur_ms;
  if (!p)
    return 0;
  /*
   * Let nshape fixtures provide a cold-cache scheduling estimate.  The
   * result cache remains authoritative once a fixture has run; this field
   * only helps the first run (and new fixtures) enter the longest-first
   * queue in a useful order.
   */
  if (shape_path_is_nshape(p)) {
    char *estimate = shape_meta_string(p, "estimate_ms");
    int parsed = 0;
    if (estimate && ny_parse_int(estimate, &parsed) && parsed > 0) {
      free(estimate);
      return parsed;
    }
    free(estimate);
  }
  if (test_path_is_benchmark(p))
    return 5000;
  if (strstr(p, "etc/tests/lang/values/bigint.nshape") ||
      strstr(p, "etc/tests/lang/types/inference/attr.nshape") ||
      strstr(p, "etc/tests/lang/interop/ny/sizeof.nshape") ||
      strstr(p, "etc/tests/lang/interop/ny/asm.nshape"))
    return 5000;
  if (strstr(p, "etc/tests/lang/comptime/comptime.nshape"))
    return 3500;
  return 200;
}

static CacheDb *g_sort_cache = NULL;

static int path_lex_cmp(const void *a, const void *b) {
  const char *pa = *(char *const *)a;
  const char *pb = *(char *const *)b;
  if (!pa)
    return pb ? 1 : 0;
  if (!pb)
    return -1;
  return strcmp(pa, pb);
}

static int path_fail_fast_cmp(const void *a, const void *b) {
  const char *pa = *(char *const *)a;
  const char *pb = *(char *const *)b;
  int pa_bench = test_path_is_benchmark(pa);
  int pb_bench = test_path_is_benchmark(pb);
  if (pa_bench != pb_bench)
    return pa_bench - pb_bench;
  return path_lex_cmp(a, b);
}

static int path_duration_cmp_desc(const void *a, const void *b) {
  const char *pa = *(char *const *)a;
  const char *pb = *(char *const *)b;
  int da = path_duration_hint(g_sort_cache, pa);
  int db = path_duration_hint(g_sort_cache, pb);
  if (da != db)
    return (db > da) ? 1 : -1;
  if (!pa)
    return pb ? 1 : 0;
  if (!pb)
    return -1;
  return strcmp(pa, pb);
}

static void print_section(const char *name) {
  nyt_subheading(name ? name : "Suite");
}

static void print_timing_suite_row(const char *name, const SuiteStats *stats,
                                   int compact) {
  if (!name || !stats || stats->tests <= 0)
    return;
  int average = stats->sum_ms / stats->tests;
  if (compact) {
    nyt_kv(name, "%d test%s · %d passed · %dms total · %dms avg · %dms max",
           stats->tests, stats->tests == 1 ? "" : "s", stats->passed,
           stats->sum_ms, average, stats->max_ms);
    return;
  }
  printf("%-9s %5d %5d %8dms %7dms %7dms\n", name, stats->tests, stats->passed,
         stats->sum_ms, average, stats->max_ms);
}

static int run_repl_suite(StrVec *files, const char *bin, const char *std_path,
                          const char *std_bc, StrVec *patterns, int timeout_sec,
                          int *passed, int *failed, SuiteStats *stats,
                          TimingVec *timings, StrVec *failed_paths,
                          int fail_fast) {
  if (!files || files->len == 0)
    return 0;
  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (test_path_contains(p, patterns->items[k])) {
          match = 1;
          break;
        }
      }
    }
    if (match)
      sv_push(&selected, p);
  }
  if (selected.len == 0) {
    sv_free(&selected);
    return 0;
  }

  print_section("Repl");
  for (size_t i = 0; i < selected.len; i++) {
    int dur = 0;
    char why[128] = {0};
    int fixture_timeout = test_timeout_for_path(selected.items[i], timeout_sec);
    int ok = run_repl_paste_case(bin, selected.items[i], std_path, std_bc,
                                 fixture_timeout, &dur, why, sizeof(why));
    if (ok == 2) {
      char time_label[32];
      char suffix[256];
      int pct = (int)(((i + 1) * 100) / (selected.len ? selected.len : 1));
      format_test_time(time_label, sizeof(time_label), dur);
      snprintf(suffix, sizeof(suffix), "(%s)", why[0] ? why : "skipped");
      print_test_progress_line(pct, "-", NYT_GRAY, "-", NYT_GRAY, "-", NYT_GRAY,
                               time_label, selected.items[i], suffix);
      timings_push(timings, selected.items[i], dur, "Repl", "skip");
      continue;
    }
    if (stats) {
      stats->tests++;
      stats->sum_ms += dur;
      if (dur > stats->max_ms)
        stats->max_ms = dur;
    }
    timings_push(timings, selected.items[i], dur, "Repl", ok ? "pass" : "fail");
    int pct = (int)(((i + 1) * 100) / (selected.len ? selected.len : 1));
    if (ok) {
      (*passed)++;
      if (stats)
        stats->passed++;
      char time_label[32];
      format_test_time(time_label, sizeof(time_label), dur);
      print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓",
                               NYT_GREEN, time_label, selected.items[i], NULL);
    } else {
      (*failed)++;
      sv_push_unique(failed_paths, selected.items[i]);
      char time_label[32];
      char suffix[256];
      format_test_time(time_label, sizeof(time_label), dur);
      snprintf(suffix, sizeof(suffix), "(%s)",
               why[0] ? why : "repl paste failed");
      print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗", NYT_RED,
                               time_label, selected.items[i], suffix);
      if (fail_fast) {
        printf(
            "[fail-fast] stopped after %s; remaining fixtures were not run\n",
            disp_path(selected.items[i]));
        break;
      }
    }
  }
  sv_free(&selected);
  return 0;
}

static void print_bench_summary(const char *path) {
  if (!path || !*path)
    return;
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "[BENCH]")) {
      printf("      %s%s%s", nyt_clr(NYT_CYAN), line + 8, nyt_clr(NYT_RESET));
    }
  }
  fclose(f);
}

static int run_suite(const char *suite_name, StrVec *files, const char *bin,
                     const char *std_path, const char *std_bc, StrVec *patterns,
                     int jobs, int timeout_sec, int *passed, int *failed,
                     int fail_fast, int force_compile_fail, SuiteStats *stats,
                     SuiteStats *benchmark_stats, SuiteStats *runtime_stats,
                     SuiteStats *native_stats, SuiteStats *interop_stats,
                     SuiteStats *probe_stats, SuiteStats *std_stats,
                     CacheDb *cache, int use_cache, TimingVec *timings,
                     StrVec *failed_paths, const char *cache_path) {
  if (std_bc && *std_bc)
    ny_setenv("NYTRIX_TEST_CORE_BC", std_bc, 1);
  if (!files || files->len == 0)
    return 0;

  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    /*
     * Generator-only fuzz shapes remain outside ny-test.
     */
    if (p && shape_path_is_nshape(p)) {
      char *source = shape_source_block(p, "ny");
      if (!source) {
        char *generator = shape_source_block(p, "python");
        int declared = generator != NULL;
        free(generator);
        if (!declared)
          continue;
      }
      free(source);
    }
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (test_path_contains(p, patterns->items[k])) {
          match = 1;
          break;
        }
      }
    }
    if (match)
      sv_push(&selected, p);
  }

  if (selected.len == 0) {
    sv_free(&selected);
    return 0;
  }
  /*
   * Start historically expensive fixtures first so parallel workers stay
   * balanced and the suite has less serial tail. Equal estimates use path
   * order for deterministic dispatch. Set NYTRIX_TEST_ORDER=stable to retain
   * lexical order for debugging; fail-fast keeps its failure-oriented order.
   */
  const char *test_order = getenv("NYTRIX_TEST_ORDER");
  if (!fail_fast && ((test_order && strcmp(test_order, "slowest") == 0) ||
                     (!test_order && jobs > 1))) {
    g_sort_cache = cache;
    qsort(selected.items, selected.len, sizeof(char *), path_duration_cmp_desc);
    g_sort_cache = NULL;
  } else {
    qsort(selected.items, selected.len, sizeof(char *),
          fail_fast ? path_fail_fast_cmp : path_lex_cmp);
  }

  print_section(suite_name);
  size_t total = selected.len;
  size_t completed = 0;
  double last_heartbeat_ms = now_ms();
  double heartbeat_interval_ms = NY_TEST_WAIT_HEARTBEAT_MS;
  for (size_t i = 0; i < selected.len; i++) {
    const char *p = selected.items[i];
    uint64_t sig = test_sig(p, bin, std_path, std_bc);
    if (use_cache) {
      CacheRow *row = cache_find(cache, p);
      if (row && row->ok == 1 && row->sig == sig) {
        SuiteStats *row_stats =
            stats_for_path(p, stats, benchmark_stats, runtime_stats,
                           native_stats, interop_stats, probe_stats, std_stats);
        const char *row_suite = suite_for_path(p, suite_name);
        completed++;
        int pct = (int)((completed * 100) / (total ? total : 1));
        int dur = row->dur_ms;
        (*passed)++;
        if (row_stats) {
          row_stats->tests++;
          row_stats->passed++;
          row_stats->sum_ms += dur;
          if (dur > row_stats->max_ms)
            row_stats->max_ms = dur;
        }
        timings_push(timings, p, dur, row_suite, "pass");
        char time_label[32];
        format_test_time(time_label, sizeof(time_label), -1);
        print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓",
                                 NYT_GREEN, time_label, p, NULL);
        free(selected.items[i]);
        selected.items[i] = NULL;
      }
    }
  }
  if (jobs < 1)
    jobs = 1;
  int failures_at_start = *failed;

  typedef struct {
    ny_test_proc_t pid;
    const char *path;
    char tmp_out[PATH_MAX];
    char materialized_path[PATH_MAX];
    double start_ms;
    int accumulated_ms;
    int matrix_variant;
    int matrix_count;
    int active;
    int timed_out;
    int timeout_sec;
    int expected_compile_fail;
    int native_oracle;
    char failure_detail[512];
  } Running;

  Running *run = (Running *)calloc((size_t)jobs, sizeof(Running));
  unsigned char *started =
      (unsigned char *)calloc(total ? total : 1, sizeof(unsigned char));
  if (!run || !started) {
    free(run);
    free(started);
    sv_free(&selected);
    return 1;
  }

  while (completed < total) {
    int active_count = 0;
    int exclusive_active = 0;
    for (int i = 0; i < jobs; i++) {
      if (!run[i].active)
        continue;
      active_count++;
      if (test_requires_host_exclusive(run[i].path))
        exclusive_active = 1;
    }
    for (int i = 0; i < jobs; i++) {
      if (fail_fast && *failed != failures_at_start)
        break;
      if (run[i].active)
        continue;
      size_t candidate = total;
      for (size_t k = 0; k < total; k++) {
        const char *queued = selected.items[k];
        if (!queued || started[k])
          continue;
        int exclusive = test_requires_host_exclusive(queued);
        if (exclusive_active || (exclusive && active_count > 0))
          continue;
        candidate = k;
        break;
      }
      if (candidate == total)
        break;
      const char *p = selected.items[candidate];
      started[candidate] = 1;
      run[i].tmp_out[0] = '\0';
      run[i].materialized_path[0] = '\0';
      run[i].failure_detail[0] = '\0';
      const char *exec_path = p;
      int is_bench = test_path_is_benchmark(p);
      if (p && shape_path_is_nshape(p)) {
        char *mat = materialize_shape_ny_source(p);
        if (!mat) {
          (*failed)++;
          sv_push_unique(failed_paths, p);
          completed++;
          continue;
        }
        snprintf(run[i].materialized_path, sizeof(run[i].materialized_path),
                 "%s", mat);
        free(mat);
        exec_path = run[i].materialized_path;
      }
      int capture_fd =
          make_test_capture_tmp(run[i].tmp_out, sizeof(run[i].tmp_out),
                                is_bench ? "bench" : "fixture");
#ifndef _WIN32
      if (capture_fd >= 0)
        close(capture_fd);
#endif
      if (capture_fd < 0) {
        if (run[i].materialized_path[0])
          shape_remove_materialized(run[i].materialized_path);
        (*failed)++;
        sv_push_unique(failed_paths, p);
        completed++;
        continue;
      }
      int expected_compile_fail =
          force_compile_fail || shape_expects_compile_failure(p);
      char *shape_flags =
          shape_path_is_nshape(p) ? shape_meta_string(p, "flags") : NULL;
      char *shape_matrix =
          shape_path_is_nshape(p) ? shape_meta_string(p, "flags_matrix") : NULL;
      int native_oracle =
          shape_flags && (strstr(shape_flags, "--native-result-oracle") ||
                          strstr(shape_flags, "--native-oracle-per-pass"));
      if (!native_oracle && shape_matrix)
        native_oracle = strstr(shape_matrix, "--native-result-oracle") ||
                        strstr(shape_matrix, "--native-oracle-per-pass");
      run[i].matrix_count = test_flag_matrix_count(p);
      run[i].matrix_variant = 0;
      run[i].accumulated_ms = 0;
      free(shape_flags);
      free(shape_matrix);
      ny_test_proc_t pid = run_one_start(bin, p, exec_path, std_path, std_bc,
                                         run[i].tmp_out, run[i].matrix_variant);
      run[i].expected_compile_fail = expected_compile_fail;
      run[i].native_oracle = native_oracle;
      if (!ny_test_proc_valid(pid)) {
        remove(run[i].tmp_out);
        if (run[i].materialized_path[0])
          shape_remove_materialized(run[i].materialized_path);
        (*failed)++;
        sv_push_unique(failed_paths, p);
        completed++;
        continue;
      }
      run[i].pid = pid;
      run[i].path = p;
      run[i].start_ms = now_ms();
      run[i].timed_out = 0;
      run[i].timeout_sec = test_timeout_for_path(p, timeout_sec);
      run[i].active = 1;
      active_count++;
      if (test_requires_host_exclusive(p))
        exclusive_active = 1;
    }
    /*
     * After the first failure, let already-started fixtures finish but do
     * not launch more. Exit once that in-flight batch has drained.
     */
    if (fail_fast && *failed != failures_at_start && active_count == 0)
      break;

    int st = 0;
    int timed_out = 0;
    ny_test_proc_t done = NY_TEST_PROC_INVALID;
#ifdef _WIN32
    for (int i = 0; i < jobs; i++) {
      if (!run[i].active)
        continue;
      int pr = ny_test_poll_done(run[i].pid, &st);
      if (pr > 0) {
        done = run[i].pid;
        break;
      }
      if (pr < 0) {
        st = 127;
        done = run[i].pid;
        break;
      }
    }
#else
    done = waitpid(-1, &st, WNOHANG);
    if (done < 0 && errno == EINTR)
      continue;
#endif
    if (!ny_test_proc_valid(done)) {
      double t = now_ms();
      if (t - last_heartbeat_ms >= heartbeat_interval_ms) {
        int active = 0;
        int oldest = -1;
        double oldest_ms = 0.0;
        for (int i = 0; i < jobs; i++) {
          if (!run[i].active)
            continue;
          active++;
          double elapsed_ms = t - run[i].start_ms;
          if (oldest < 0 || elapsed_ms > oldest_ms) {
            oldest = i;
            oldest_ms = elapsed_ms;
          }
        }
        if (active > 0 && oldest >= 0) {
          fprintf(stdout, "[wait] active=%d · oldest=%s %lds/%ds\n", active,
                  disp_path(run[oldest].path), (long)(oldest_ms / 1000.0),
                  run[oldest].timeout_sec);
          fflush(stdout);
        }
        last_heartbeat_ms = t;
        if (heartbeat_interval_ms < 180000.0)
          heartbeat_interval_ms *= 2.0;
        if (heartbeat_interval_ms > 180000.0)
          heartbeat_interval_ms = 180000.0;
      }
#ifdef __linux__
      /*
       * Memory watchdog: poll each active worker's process-group RSS and
       * kill the group as soon as it crosses the configured cap.  A fixture
       * whose compile or program runs away (infinite recursion with boxed
       * allocations, a JIT-tier blowup) otherwise grows until the global
       * OOM killer takes the whole machine down with it.  Runs on the same
       * cadence as poll_sleep, far faster than memory can spike safely.
       */
      {
        static double last_mem_check_ms = 0.0;
        double mem_cap = test_mem_limit_gib();
        if (mem_cap > 0.0 && t - last_mem_check_ms >= 500.0) {
          last_mem_check_ms = t;
          for (int i = 0; i < jobs; i++) {
            if (!run[i].active || run[i].timed_out)
              continue;
            long rss_kib = test_group_rss_kib(run[i].pid);
            if (rss_kib < 0)
              continue;
            double used_gib = (double)rss_kib / (1024.0 * 1024.0);
            if (used_gib > mem_cap) {
              fprintf(stderr,
                      "[test] killed fixture=%s: RSS %.1f GiB exceeded the "
                      "%.1f GiB fixture cap; set "
                      "NYTRIX_TEST_MEM_LIMIT_GIB or "
                      "NYTRIX_TEST_MEM_LIMIT_PERCENT to override\n",
                      disp_path(run[i].path), used_gib, mem_cap);
              fflush(stderr);
              run[i].timed_out = 1;
              ny_test_kill_group(run[i].pid);
              if (!ny_test_proc_valid(done))
                done = run[i].pid;
            }
          }
        }
      }
#endif
      /*
       * Terminate every expired worker in one deadline pass.  Killing and
       * reaping only the first one serialized simultaneous timeouts: a batch
       * of 30-second fixtures was reported at 40, 70, 100, ... seconds.
       */
      for (int i = 0; i < jobs; i++) {
        if (!run[i].active)
          continue;
        double fixture_timeout_ms = (double)run[i].timeout_sec * 1000.0 +
                                    NY_TEST_PARALLEL_TIMEOUT_GRACE_MS;
        if (run[i].timed_out || t - run[i].start_ms < fixture_timeout_ms)
          continue;
        run[i].timed_out = 1;
#ifdef _WIN32
        TerminateProcess(run[i].pid, NY_TEST_TIMEOUT_RC);
#else
        ny_test_kill_group(run[i].pid);
#endif
        if (!ny_test_proc_valid(done))
          done = run[i].pid;
      }
      if (ny_test_proc_valid(done)) {
#ifdef _WIN32
        WaitForSingleObject(done, INFINITE);
        st = NY_TEST_TIMEOUT_RC;
#else
        while (waitpid(done, &st, 0) < 0 && errno == EINTR) {
        }
#endif
      }
      if (!ny_test_proc_valid(done)) {
        poll_sleep();
        continue;
      }
    }
    for (int i = 0; i < jobs; i++) {
      if (!run[i].active || !ny_test_proc_eq(run[i].pid, done))
        continue;
      timed_out = run[i].timed_out;
      int rc = timed_out ? NY_TEST_TIMEOUT_RC : child_status_rc(st);
      int dur = (int)(now_ms() - run[i].start_ms);
      SuiteStats *row_stats =
          stats_for_path(run[i].path, stats, benchmark_stats, runtime_stats,
                         native_stats, interop_stats, probe_stats, std_stats);
      const char *row_suite = suite_for_path(run[i].path, suite_name);
      if (run[i].expected_compile_fail) {
        char *output = read_small_file(run[i].tmp_out);
        char *message = shape_meta_string(run[i].path, "expect_message");
        int compiler_rc = rc;
        const char *expected_message =
            message && *message ? message
                                : (force_compile_fail ? "error" : NULL);
        int matched = !timed_out && rc != 0 && output &&
                      (!expected_message || strstr(output, expected_message));
        if (matched) {
          rc = 0;
        } else {
          rc = 1;
          if (timed_out) {
            snprintf(run[i].failure_detail, sizeof(run[i].failure_detail),
                     "expected compile failure timed out");
          } else if (output && *output && expected_message &&
                     !strstr(output, expected_message)) {
            snprintf(run[i].failure_detail, sizeof(run[i].failure_detail),
                     "missing diagnostic '%.*s'; output: %.300s", 100,
                     expected_message, output);
          } else if (!output || !*output) {
            snprintf(run[i].failure_detail, sizeof(run[i].failure_detail),
                     "expected compiler diagnostic, got empty output");
          } else {
            const char *last_line = output;
            for (const char *line = output; *line; ++line) {
              if (*line == '\n' && line[1])
                last_line = line + 1;
            }
            snprintf(run[i].failure_detail, sizeof(run[i].failure_detail),
                     "expected compile failure, got rc=%d; last output: "
                     "%.300s",
                     compiler_rc, last_line);
          }
        }
        free(message);
        free(output);
      } else {
        /*
         * Native result-oracle fixtures deliberately return their raw i64
         * result as the process status, which is truncated by POSIX to an
         * 8-bit exit code.  The captured `ok=yes` oracle record is the
         * authoritative success signal; a nonzero signal-derived status is
         * never accepted.
         */
        if (rc != 0 && !timed_out && run[i].native_oracle &&
            run[i].tmp_out[0]) {
          char *output = read_whole_file(run[i].tmp_out);
          if (output && strstr(output, "native oracle function=rt_main") &&
              strstr(output, "ok=yes"))
            rc = 0;
          free(output);
        }
        int expected_exit = shape_expected_exit_code(run[i].path);
        if (expected_exit == -2)
          rc = 1;
        else if (expected_exit >= 0 && !timed_out) {
#ifndef _WIN32
          rc = WIFEXITED(st) && rc == expected_exit ? 0 : 1;
#else
          rc = rc == expected_exit ? 0 : 1;
#endif
          if (rc == 0) {
            char *message = shape_meta_string(run[i].path, "expect_message");
            if (message && *message) {
              char *output = read_small_file(run[i].tmp_out);
              rc = output && strstr(output, message) ? 0 : 1;
              free(output);
            }
            free(message);
          }
        }
        if (rc == 0)
          rc = object_link_run_check(run[i].path);
      }
      /*
       * Native-only entry functions may return their final expression as the
       * process status. Assertions establish success; preserve that sentinel
       * interpretation for every matrix row, not only on blocking retries.
       */
      if (rc == 3 && path_is_native_test(run[i].path))
        rc = 0;
      if (rc == 0 && run[i].matrix_variant + 1 < run[i].matrix_count) {
        run[i].accumulated_ms += dur;
        run[i].matrix_variant++;
        const char *next_exec_path = run[i].materialized_path[0]
                                         ? run[i].materialized_path
                                         : run[i].path;
        const char *next_capture_path =
            run[i].tmp_out[0] ? run[i].tmp_out : NULL;
        ny_test_proc_t next =
            run_one_start(bin, run[i].path, next_exec_path, std_path, std_bc,
                          next_capture_path, run[i].matrix_variant);
        if (ny_test_proc_valid(next)) {
          run[i].pid = next;
          run[i].start_ms = now_ms();
          run[i].timed_out = 0;
          continue;
        }
        rc = 127;
      }
      dur += run[i].accumulated_ms;
      if ((rc != 0 || test_env_truthy("NYTRIX_TEST_SHOW_PASS_OUTPUT")) &&
          run[i].tmp_out[0])
        print_captured_test_output("fixture output", run[i].path,
                                   run[i].tmp_out);
      completed++;
      last_heartbeat_ms = now_ms();
      heartbeat_interval_ms = NY_TEST_WAIT_HEARTBEAT_MS;
      int pct = (int)((completed * 100) / (total ? total : 1));
      if (rc == 0) {
        (*passed)++;
        if (row_stats)
          row_stats->passed++;
        char time_label[32];
        char matrix_suffix[48];
        const char *matrix_detail = NULL;
        if (run[i].matrix_count > 1) {
          snprintf(matrix_suffix, sizeof(matrix_suffix), "(%d variants)",
                   run[i].matrix_count);
          matrix_detail = matrix_suffix;
        }
        format_test_time(time_label, sizeof(time_label), dur);
        print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓",
                                 NYT_GREEN, time_label, run[i].path,
                                 matrix_detail);
        if (run[i].tmp_out[0]) {
          print_bench_summary(run[i].tmp_out);
          remove(run[i].tmp_out);
        }
        if (run[i].materialized_path[0])
          shape_remove_materialized(run[i].materialized_path);
      } else {
        (*failed)++;
        sv_push_unique(failed_paths, run[i].path);
        if (run[i].tmp_out[0])
          remove(run[i].tmp_out);
        if (run[i].materialized_path[0])
          shape_remove_materialized(run[i].materialized_path);
        if (timed_out || rc == NY_TEST_TIMEOUT_RC) {
          char time_label[32];
          char suffix[64];
          format_test_time(time_label, sizeof(time_label), dur);
          snprintf(suffix, sizeof(suffix), "(timeout=%ds)", run[i].timeout_sec);
          print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗",
                                   NYT_RED, time_label, run[i].path, suffix);
        } else {
          char time_label[32];
          char suffix[640];
          format_test_time(time_label, sizeof(time_label), dur);
          if (run[i].failure_detail[0])
            snprintf(suffix, sizeof(suffix), "(%s)", run[i].failure_detail);
          else
            snprintf(suffix, sizeof(suffix), "(rc=%d)", rc);
          print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗",
                                   NYT_RED, time_label, run[i].path, suffix);
        }
      }
      if (row_stats) {
        row_stats->tests++;
        row_stats->sum_ms += dur;
        if (dur > row_stats->max_ms)
          row_stats->max_ms = dur;
      }
      if (cache) {
        uint64_t sig = test_sig(run[i].path, bin, std_path, std_bc);
        cache_set(cache, run[i].path, sig, rc == 0 ? 1 : 0, dur);
        if (use_cache)
          cache_save(cache, cache_path);
      }
      timings_push(timings, run[i].path, dur, row_suite,
                   rc == 0 ? "pass"
                           : (timed_out || rc == NY_TEST_TIMEOUT_RC ? "timeout"
                                                                    : "fail"));
      if (fail_fast && rc != 0) {
        printf(
            "[fail-fast] stopped after %s; remaining fixtures were not run\n",
            disp_path(run[i].path));
        fflush(stdout);
      }
      ny_test_proc_close(run[i].pid);
      run[i].active = 0;
      break;
    }
  }
  free(run);
  free(started);
  sv_free(&selected);
  return 0;
}

static int shape_generator_write_fixture(char *path, size_t path_len,
                                         const char *text) {
  int fd = make_test_capture_tmp(path, path_len, "shape-generator-test");
#ifdef _WIN32
  FILE *f = fopen(path, "wb");
#else
  FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
#endif
  if (!f) {
#ifndef _WIN32
    if (fd >= 0)
      close(fd);
#endif
    return 0;
  }
  size_t n = strlen(text);
  return fwrite(text, 1, n, f) == n && fclose(f) == 0;
}

static int run_shape_generator_selftest(void) {
  static const char malformed[] = "shape malformed {\n  source ny generate "
                                  "python <<'PY'\nprint('broken')\n}\n";
  static const char missing[] =
      "shape missing {\n  expect compile_and_run\n}\n";
  static const char success[] =
      "shape python_generator {\n  source ny generate python <<'PY'\n"
      "print('use core')\nprint('print(\\\"quoted\\\\nline\\\")')\n"
      "PY\n}\n";
  char paths[3][PATH_MAX];
  if (!shape_generator_write_fixture(paths[0], sizeof(paths[0]), success) ||
      !shape_generator_write_fixture(paths[1], sizeof(paths[1]), malformed) ||
      !shape_generator_write_fixture(paths[2], sizeof(paths[2]), missing)) {
    fprintf(stderr, "shape generator selftest: fixture setup failed\n");
    return 1;
  }

  int ok = 1;
  char *generated = materialize_shape_ny_source(paths[0]);
  char *text = generated ? read_small_file(generated) : NULL;
  if (!text || strcmp(text, "use core\nprint(\"quoted\\nline\")\n") != 0) {
    fprintf(stderr, "shape generator selftest: generated source mismatch: %s\n",
            text ? text : "<missing>");
    ok = 0;
  }
  free(text);
  if (generated) {
    shape_remove_materialized(generated);
    free(generated);
  }

  char *program = shape_source_block(paths[1], "python");
  if (program) {
    fprintf(stderr,
            "shape generator selftest: malformed block was not rejected\n");
    ok = 0;
  }
  free(program);
  program = shape_source_block(paths[2], "python");
  if (program) {
    fprintf(stderr,
            "shape generator selftest: missing generator was not detected\n");
    ok = 0;
  }
  free(program);
  for (size_t i = 0; i < 3; i++)
    remove(paths[i]);
  printf("shape generator selftest: %s\n", ok ? "passed" : "failed");
  return ok ? 0 : 1;
}

static char *atoms_block_field(const char *block, const char *block_end,
                               const char *key) {
  size_t key_len = strlen(key);
  for (const char *line = block; line && line < block_end;) {
    const char *next = memchr(line, '\n', (size_t)(block_end - line));
    const char *lend = next ? next : block_end;
    while (line < lend && (*line == ' ' || *line == '\t'))
      line++;
    if ((size_t)(lend - line) > key_len && strncmp(line, key, key_len) == 0 &&
        (line[key_len] == ' ' || line[key_len] == '\t')) {
      const char *p = line + key_len;
      while (p < lend && (*p == ' ' || *p == '\t'))
        p++;
      if (p < lend && *p == '"') {
        p++;
        const char *end = p;
        while (end < lend && (*end != '"' || end[-1] == '\\'))
          end++;
        if (end <= lend)
          return decode_shape_quoted_string(p, (size_t)(end - p));
      }
      return NULL;
    }
    line = next ? next + 1 : NULL;
  }
  return NULL;
}

static int run_reader_atoms_nshape(const char *bin, const char *path) {
  if (!path || !*path)
    path = "etc/tests/lang/parsing/atoms.nshape";
  char *data = read_small_file(path);
  if (!data) {
    fprintf(stderr, "reader-atoms: cannot open %s\n", path);
    return 1;
  }
  char *atoms_start = NULL;
  for (char *line = data; line && *line; line = strchr(line, '\n') + 1) {
    const char *p = line;
    while (*p == ' ' || *p == '\t')
      p++;
    if (strncmp(p, "atoms", 5) == 0) {
      char *brace = strchr(line, '{');
      if (brace) {
        atoms_start = brace + 1;
        break;
      }
    }
  }
  if (!atoms_start) {
    fprintf(stderr, "reader-atoms: %s: missing 'atoms {' block\n", path);
    free(data);
    return 1;
  }
  int depth = 1;
  char *atoms_end = atoms_start;
  while (*atoms_end && depth > 0) {
    if (*atoms_end == '{')
      depth++;
    else if (*atoms_end == '}')
      depth--;
    atoms_end++;
  }

  int total = 0, passed = 0, failed = 0, xfailed = 0;
  char *cursor = atoms_start;
  while (cursor && cursor < atoms_end) {
    char *atom_line = strstr(cursor, "\n    atom ");
    if (!atom_line || atom_line >= atoms_end)
      break;
    atom_line += 10;
    char *name_end = strpbrk(atom_line, " {");
    if (!name_end)
      break;
    char name[128];
    size_t nlen = (size_t)(name_end - atom_line);
    if (nlen >= sizeof(name))
      nlen = sizeof(name) - 1;
    memmove(name, atom_line, nlen);
    name[nlen] = '\0';
    char *body = strchr(name_end, '{');
    if (!body || body >= atoms_end)
      break;
    body++;
    int adepth = 1;
    char *abody = body;
    while (abody < atoms_end && adepth > 0) {
      if (*abody == '{')
        adepth++;
      else if (*abody == '}')
        adepth--;
      abody++;
    }
    char *block_end = abody - 1;

    total++;
    char *source = atoms_block_field(body, block_end, "source");
    char *expect = atoms_block_field(body, block_end, "expect");
    char *diagnostic = atoms_block_field(body, block_end, "diagnostic");
    char *purpose = atoms_block_field(body, block_end, "purpose");

    if (!source || !*source) {
      fprintf(stderr, "FAIL: %s (%s) - missing source\n", name,
              purpose && *purpose ? purpose : "");
      failed++;
      free(source);
      free(expect);
      free(diagnostic);
      free(purpose);
      cursor = abody;
      continue;
    }

    int ok = 0;
    int xfail = 0;
    char *status = atoms_block_field(body, block_end, "status");
    xfail = status && strcmp(status, "known-bug") == 0;
    free(status);
    char out_path[PATH_MAX];
    out_path[0] = '\0';
#ifdef _WIN32
    FILE *of = fopen(out_path, "wb");
    (void)of;
#else
    int have_out_fd =
        make_test_capture_tmp(out_path, sizeof(out_path), "reader-atom");
    if (have_out_fd >= 0)
      close(have_out_fd);
#endif
    char cmd[8192];
    if (expect && *expect) {
      snprintf(cmd, sizeof(cmd), "%s -e '%s' > '%s' 2>&1", bin, source,
               out_path[0] ? out_path : "/dev/null");
      int rc = system(cmd);
      char *out = rc == 0 ? read_small_file(out_path) : NULL;
      if (out) {
        trim_inplace(out);
        ok = strcmp(out, expect) == 0;
      }
      if (!ok && !xfail)
        fprintf(stderr, "FAIL: %s (%s): rc=%d want=\"%s\" got=\"%s\"\n", name,
                purpose && *purpose ? purpose : "", rc, expect,
                out ? out : "<no output>");
      free(out);
    } else {
      snprintf(cmd, sizeof(cmd), "%s -e '%s' > /dev/null 2>&1", bin, source);
      int rc = system(cmd);
      if (diagnostic && *diagnostic)
        ok = rc != 0;
      else
        ok = rc == 0;
      if (!ok && !xfail)
        fprintf(stderr, "FAIL: %s (%s): rc=%d%s%s\n", name,
                purpose && *purpose ? purpose : "", rc,
                diagnostic && *diagnostic ? " expected diagnostic: " : "",
                diagnostic && *diagnostic ? diagnostic : "");
    }
    if (ok) {
      passed++;
    } else if (xfail) {
      xfailed++;
      printf("XFAIL: %s (%s)\n", name, purpose && *purpose ? purpose : "");
    } else {
      failed++;
    }
    free(source);
    free(expect);
    free(diagnostic);
    free(purpose);
    cursor = abody;
  }
  free(data);
  printf("Reader atoms: %d total | %d passed | %d failed | %d xfail (%s)\n",
         total, passed, failed, xfailed, path);
  return failed ? 1 : 0;
}

static const char *bench_dir_root(void);

/*
 * Benchmark discovery runs before any benchmark fixture is compiled. Keep
 * it in the runner so collection does not depend on starting another Ny
 * process (which itself needs the stdlib being benchmarked).
 */
static bool test_collect_add_file(const char *path, bool explicit_file,
                                  bool benchmarks_only, StrVec *out) {
  if (nyt_ends_with(path, ".nshape")) {
    sv_push(out, path);
    return true;
  }
  if (benchmarks_only || !nyt_ends_with(path, ".ny"))
    return true;
  if (!explicit_file) {
    char shape[PATH_MAX];
    size_t len = strlen(path);
    if (len < 3 || len - 3 + sizeof(".nshape") > sizeof(shape))
      return false;
    memcpy(shape, path, len - 3);
    memcpy(shape + len - 3, ".nshape", sizeof(".nshape"));
    if (nyt_is_file(shape))
      return true;
  }
  sv_push(out, path);
  return true;
}

static bool test_collect_tree(const char *dir, StrVec *out, int depth,
                              size_t *entries, bool benchmarks_only) {
  if (!dir || !out || !entries || depth > 256)
    return false;
#ifdef _WIN32
  char pattern[PATH_MAX];
  int pn = snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  if (pn < 0 || (size_t)pn >= sizeof(pattern))
    return false;
  WIN32_FIND_DATAA data;
  HANDLE find = FindFirstFileA(pattern, &data);
  if (find == INVALID_HANDLE_VALUE)
    return false;
  bool ok = true;
  do {
    const char *name = data.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || name[0] == '.')
      continue;
    if (++*entries > 100000) {
      ok = false;
      break;
    }
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s\\%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
      ok = false;
      break;
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
          !test_collect_tree(path, out, depth + 1, entries, benchmarks_only)) {
        ok = false;
        break;
      }
    } else if (!(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      if (!test_collect_add_file(path, false, benchmarks_only, out)) {
        ok = false;
        break;
      }
    }
  } while (FindNextFileA(find, &data));
  if (GetLastError() != ERROR_NO_MORE_FILES)
    ok = false;
  FindClose(find);
  return ok;
#else
  DIR *handle = opendir(dir);
  if (!handle)
    return false;
  bool ok = true;
  errno = 0;
  struct dirent *entry;
  while ((entry = readdir(handle)) != NULL) {
    const char *name = entry->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || name[0] == '.')
      continue;
    if (++*entries > 100000) {
      ok = false;
      break;
    }
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
      ok = false;
      break;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
      ok = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      if (!test_collect_tree(path, out, depth + 1, entries, benchmarks_only)) {
        ok = false;
        break;
      }
    } else if (S_ISREG(st.st_mode)) {
      if (!test_collect_add_file(path, false, benchmarks_only, out)) {
        ok = false;
        break;
      }
    }
    errno = 0;
  }
  if (errno != 0)
    ok = false;
  closedir(handle);
  return ok;
#endif
}

static bool test_collect_native(const char *dir, StrVec *out,
                                bool benchmarks_only) {
  if (!dir || !*dir || !out)
    return false;
  if (nyt_is_file(dir))
    return test_collect_add_file(dir, true, benchmarks_only, out);
  if (!is_dir(dir))
    return false;
  size_t entries = 0;
  if (!test_collect_tree(dir, out, 0, &entries, benchmarks_only))
    return false;
  if (out->len > 1)
    qsort(out->items, out->len, sizeof(out->items[0]), path_lex_cmp);
  return true;
}

static int run_list_bench(const char *root_dir) {
  const char *root = bench_dir_root();
  if (!root)
    root = ".";
  StrVec shapes = {0};
  char bench_dir[PATH_MAX];
  snprintf(bench_dir, sizeof(bench_dir), "%s/etc/tests/bench",
           root_dir && *root_dir ? root_dir : root);
  if (!test_collect_native(bench_dir, &shapes, true)) {
    nyt_err("ny-test",
            "native fixture discovery failed for benchmark directory: %s",
            bench_dir);
    sv_free(&shapes);
    return 1;
  }
  qsort(shapes.items, shapes.len, sizeof(char *), path_lex_cmp);
  for (size_t i = 0; i < shapes.len; ++i) {
    const char *base = test_path_basename(shapes.items[i]);
    char stem[128];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot)
      *dot = '\0';
    printf("%s\n", stem);
  }
  printf("%zu fixtures\n", shapes.len);
  sv_free(&shapes);
  return 0;
}

static int run_list_meta(const char *root_dir) {
  StrVec shapes = {0};
  const char *root = root_dir && *root_dir ? root_dir : "etc/tests/";
  if (!test_collect_native(root, &shapes, false)) {
    nyt_err("ny-test",
            "native fixture discovery failed for metadata directory: %s", root);
    sv_free(&shapes);
    return 1;
  }
  for (size_t i = 0; i < shapes.len; ++i) {
    char *m = shape_meta_string(shapes.items[i], "meta");
    if (m && *m) {
      printf("%s: %s\n", shapes.items[i], m);
    }
    free(m);
  }
  sv_free(&shapes);
  return 0;
}

int ny_test_main(int argc, char **argv) {
  double suite_started_ms = now_ms();
  const char *bin = "build/release/ny";
  const char *std_path = NULL;
  const char *std_bc = NULL;
  const char *triple = NULL;
  const char *emulator = NULL;
  int smoke = 0;
  int no_smoke = 0;
  int with_stdlib = 0;
  int jobs = 0;
  int jobs_explicit = 0;
  int timeout_sec = NY_TEST_DEFAULT_TIMEOUT_SEC;
  int phase_times = 0;
  int trace_ir = 0;
  int failures_only = 0;
  int fail_fast = 0;
  StrVec files = {0};
  StrVec patterns = {0};
  StrVec failed_paths = {0};
  TimingVec timings = {0};
  CacheDb cache = {0};
  char err[256];

  /*
   * Benchmark mode options
   */
  int bench_mode = 0;
  const char *bench_opt_level = "O3";
  const char *bench_tier = "opt";
  /*
   * The default report compares the native backend with its LLVM baseline.
   */
  const char *bench_engine = "native";
  const char *bench_target = "x86_64";
  const char *bench_compile_profile = "peak";
  const char *bench_cache = NULL;
  int bench_runs = 3;
  int bench_warmup = 1;
  /*
   * Keep benchmark and regular fixtures on the same short fail-fast budget;
   * callers can raise it explicitly with --bench-timeout when a long-running
   * correctness benchmark is intentional.
   */
  int bench_timeout = NY_TEST_DEFAULT_BENCH_TIMEOUT_SEC;
  int bench_verbose = 0;
  int bench_show_ir = 0;
  int bench_show_asm = 0;
  int bench_show_passes = 0;
  int bench_profile = 0;
  int bench_compare_llvm = 1;
  int bench_correctness = 0;
  const char *bench_output_csv = NULL;
  const char *bench_output_json = NULL;
  const char *bench_output_md = NULL;
  int bench_budget_fail = 0;

  const char *env_timeout = getenv("NYTRIX_TEST_TIMEOUT");
  if (env_timeout && *env_timeout) {
    int v = 0;
    if (ny_parse_int(env_timeout, &v))
      timeout_sec = normalize_test_timeout(v);
  }

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (ny_arg_match(a, "--help", "-h")) {
      nyt_heading("Nytrix Test Runner");
      nyt_subheading("Usage");
      printf("%susage:%s %sny test%s %s[options] [files ...]%s\n",
             nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
             nyt_clr(NYT_RESET), nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %sfuzzing:%s ny-test fuzz <command> [args]\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      nyt_subheading("Options");
      printf("  %s--bin BIN --jobs N --timeout SEC --pattern PAT%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--cold --no-cold (re-execute fixtures; keep reusable stdlib "
             "source cache)%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        NYTRIX_TEST_EXEC_CACHE=0 disables reusable execution "
             "artifacts%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--fail-fast (stop queueing fixtures after first failure)%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--smoke --no-smoke --with-stdlib --no-stdlib%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--std PATH --core-bc PATH --triple T --emulator CMD%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--bench [--bench-run N --bench-warmup N --bench-opt LVL "
             "--bench-tier TIER%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-engine aot|mcjit|orc|native|all (default "
             "native) --bench-target TARGET%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-cache cold|warm --bench-correctness%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-compiler NAME --bench-compile-profile "
             "PROFILE%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-timeout SEC --bench-verbose --bench-show-ir "
             "--bench-show-asm --bench-ny-verbose%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-show-passes --bench-profile "
             "--bench-hw-counters --bench-runtime-counters%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        NYTRIX_BENCH_DEBUG_FILTER=--dbg-filter=func:NAME "
             "selects pass-dump function (default rt_main)%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-compare-llvm --bench-no-compare-llvm%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--bench-out-csv PATH --bench-out-json PATH --bench-out-md "
             "PATH%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf(
          "  %s--bench-report-md PATH (sources, results, NYR/C artifacts)%s\n",
          nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--debug-failures --with-replay --debugger-all%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      nyt_subheading("Notes");
      nyt_kv("timeout", "all fixtures default to 60s; override with --timeout");
      nyt_kv("errors", "fixtures live under etc/tests/errors");
      return 0;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode,
                                        err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-test", "%s", err);
      sv_free(&patterns);
      return 2;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (ny_arg_match_with_value(a, "--bin")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        return 2;
      }
      bin = v;
    } else if (ny_arg_match_with_value(a, "--pattern")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      sv_push(&patterns, v);
    } else if (ny_arg_match_with_value(a, "--std")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      std_path = v;
    } else if (ny_arg_match_with_value(a, "--core-bc") ||
               ny_arg_match_with_value(a, "--std-bc")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      std_bc = v;
    } else if (ny_arg_match_with_value(a, "--triple")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      triple = v;
    } else if (ny_arg_match_with_value(a, "--emulator")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      emulator = v;
    } else if (ny_arg_match_with_value(a, "--jobs")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 0, 1024, &jobs, "jobs", err,
                           sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
      jobs_explicit = jobs > 0;
    } else if (ny_arg_match_with_value(a, "--timeout")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, NY_TEST_MAX_TIMEOUT_SEC,
                           &timeout_sec, "timeout", err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
    } else if (!strcmp(a, "--cold")) {
      ny_setenv("NYTRIX_TEST_COLD", "1", 1);
      ny_setenv("NYTRIX_TEST_CACHE", "0", 1);
      ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", "1", 1);
      ny_setenv("NYTRIX_JIT_CACHE", "0", 1);
      ny_setenv("NYTRIX_AOT_CACHE", "0", 1);
    } else if (!strcmp(a, "--no-cold"))
      ny_setenv("NYTRIX_TEST_COLD", "0", 1);
    else if (!strcmp(a, "--smoke"))
      smoke = 1;
    else if (!strcmp(a, "--no-smoke"))
      no_smoke = 1;
    else if (!strcmp(a, "--with-stdlib"))
      with_stdlib = 1;
    else if (!strcmp(a, "--no-stdlib"))
      with_stdlib = 0;
    else if (!strcmp(a, "--phase-times"))
      phase_times = 1;
    else if (!strcmp(a, "--trace-ir"))
      trace_ir = 1;
    else if (!strcmp(a, "--failures-only"))
      failures_only = 1;
    else if (!strcmp(a, "--fail-fast"))
      fail_fast = 1;
    else if (!strcmp(a, "--progress-selftest"))
      return run_progress_selftest(bin, timeout_sec);
    else if (!strcmp(a, "--shape-generator-selftest"))
      return run_shape_generator_selftest();
    else if (!strcmp(a, "--reader-atoms"))
      return run_reader_atoms_nshape(bin, i + 1 < argc ? argv[++i] : NULL);
    else if (!strcmp(a, "--list-bench"))
      return run_list_bench(i + 1 < argc ? argv[++i] : NULL);
    else if (!strcmp(a, "--list-meta"))
      return run_list_meta(i + 1 < argc ? argv[++i] : "etc/tests/");
    else if (!strcmp(a, "--debug-failures") || !strcmp(a, "--with-replay"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "1", 1);
    else if (!strcmp(a, "--no-debug-failures"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "0", 1);
    else if (!strcmp(a, "--debugger-all"))
      ny_setenv("NYTRIX_TEST_DEBUGGER_ALL", "1", 1);
    else if (!strcmp(a, "--bench") || !strcmp(a, "--bench-is")) {
      bench_mode = 1;
    } else if (ny_arg_match_with_value(a, "--bench-run")) {
      int v = 0;
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 64, &v, "bench-run", err,
                           sizeof(err)))
        return 2;
      bench_runs = v;
    } else if (ny_arg_match_with_value(a, "--bench-warmup")) {
      int v = 0;
      if (!ny_arg_take_int(a, &i, argc, argv, 0, 16, &v, "bench-warmup", err,
                           sizeof(err)))
        return 2;
      bench_warmup = v;
    } else if (ny_arg_match_with_value(a, "--bench-opt")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      bench_opt_level = v;
    } else if (ny_arg_match_with_value(a, "--bench-tier")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      bench_tier = v;
    } else if (ny_arg_match_with_value(a, "--bench-engine")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      if (strcmp(v, "aot") != 0 && strcmp(v, "mcjit") != 0 &&
          strcmp(v, "orc") != 0 && strcmp(v, "native") != 0 &&
          strcmp(v, "all") != 0) {
        nyt_err("ny-test",
                "--bench-engine: expected aot|mcjit|orc|native|all, got '%s'",
                v);
        return 2;
      }
      bench_engine = v;
    } else if (ny_arg_match_with_value(a, "--bench-cache")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      if (strcmp(v, "cold") != 0 && strcmp(v, "warm") != 0) {
        nyt_err("ny-test", "--bench-cache: expected cold|warm, got '%s'", v);
        return 2;
      }
      bench_cache = v;
      if (strcmp(v, "cold") == 0) {
        ny_setenv("NYTRIX_TEST_CACHE", "0", 1);
        ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", "1", 1);
        ny_setenv("NYTRIX_JIT_CACHE", "0", 1);
        ny_setenv("NYTRIX_AOT_CACHE", "0", 1);
      } else {
        ny_setenv("NYTRIX_TEST_CACHE", "1", 1);
        ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", "0", 1);
        ny_setenv("NYTRIX_JIT_CACHE", "1", 1);
        ny_setenv("NYTRIX_AOT_CACHE", "1", 1);
      }
    } else if (!strcmp(a, "--bench-correctness")) {
      bench_correctness = 1;
    } else if (!strcmp(a, "--bench-code-metrics")) {
      ny_setenv("NYTRIX_BENCH_CODE_METRICS", "1", 1);
    } else if (ny_arg_match_with_value(a, "--bench-compiler")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      ny_setenv("NYTRIX_BENCH_CC", v, 1);
    } else if (ny_arg_match_with_value(a, "--bench-target")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      bench_target = v;
    } else if (ny_arg_match_with_value(a, "--bench-compile-profile")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)))
        return 2;
      bench_compile_profile = v;
    } else if (ny_arg_match_with_value(a, "--bench-timeout")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 3600, &bench_timeout,
                           "bench-timeout", err, sizeof(err)))
        return 2;
    } else if (!strcmp(a, "--bench-verbose"))
      bench_verbose = 1;
    else if (!strcmp(a, "--bench-show-ir"))
      bench_show_ir = 1;
    else if (!strcmp(a, "--bench-show-asm"))
      bench_show_asm = 1;
    else if (!strcmp(a, "--bench-ny-verbose")) {
      bench_mode = 1;
      ny_setenv("NYTRIX_BENCH_NY_VERBOSE", "6", 1);
    } else if (!strcmp(a, "--bench-show-passes"))
      bench_show_passes = 1;
    else if (!strcmp(a, "--bench-profile"))
      bench_profile = 1;
    else if (!strcmp(a, "--bench-hw-counters")) {
      bench_mode = 1;
      ny_setenv("NYTRIX_BENCH_HW_COUNTERS", "1", 1);
    } else if (!strcmp(a, "--bench-runtime-counters")) {
      bench_mode = 1;
      ny_setenv("NYTRIX_BENCH_RUNTIME_COUNTERS", "1", 1);
    } else if (!strcmp(a, "--bench-compare-llvm"))
      bench_compare_llvm = 1;
    else if (!strcmp(a, "--bench-no-compare-llvm"))
      bench_compare_llvm = 0;
    else if (ny_arg_match_with_value(a, "--bench-out-csv")) {
      bench_mode = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_csv, err,
                             sizeof(err)))
        return 2;
    } else if (!strcmp(a, "--budget-fail"))
      bench_budget_fail = 1;
    else if (ny_arg_match_with_value(a, "--bench-out-json")) {
      bench_mode = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_json, err,
                             sizeof(err)))
        return 2;
    } else if (ny_arg_match_with_value(a, "--bench-out-md")) {
      bench_mode = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_md, err,
                             sizeof(err)))
        return 2;
    } else if (ny_arg_match_with_value(a, "--bench-report-md")) {
      bench_mode = 1;
      bench_full_report = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_md, err,
                             sizeof(err)))
        return 2;
    } else if (!strcmp(a, "--show-materialized")) {
      ny_setenv("NYTRIX_TEST_SHOW_MATERIALIZED", "1", 1);
    } else if (!strcmp(a, "--trace-shape-cmd")) {
      ny_setenv("NYTRIX_TEST_TRACE_SHAPE_CMD", "1", 1);
    } else if (!strcmp(a, "--diff-failure")) {
      ny_setenv("NYTRIX_TEST_DIFF_FAILURE", "1", 1);
    } else if (ny_arg_match_with_value(a, "--keep-artifacts")) {
      const char *dir = NULL;
      if (ny_arg_take_value(a, &i, argc, argv, &dir, err, sizeof(err))) {
        ny_setenv("NYTRIX_TEST_KEEP", dir, 1);
      }
    } else if (a[0] == '-') {
      nyt_err("ny-test", "unknown option: %s", a);
      sv_free(&patterns);
      return 2;
    } else {
      if (is_dir(a)) {
        if (!test_collect_native(a, &files, false)) {
          nyt_err("ny-test",
                  "native fixture discovery failed for directory: %s", a);
          sv_free(&patterns);
          sv_free(&files);
          return 1;
        }
      } else {
        sv_push(&files, a);
      }
    }
  }

  if (triple && *triple)
    ny_setenv("NYTRIX_HOST_TRIPLE", triple, 1);
  if (emulator && *emulator)
    ny_setenv("NYTRIX_TEST_EMULATOR", emulator, 1);
  if (phase_times)
    ny_setenv("NYTRIX_TEST_PHASE_TIMES", "1", 1);
  if (trace_ir)
    ny_setenv("NYTRIX_TEST_TRACE_IR", "1", 1);
  if (jobs > 0) {
    char jb[32];
    snprintf(jb, sizeof(jb), "%d", jobs);
    ny_setenv("NYTRIX_TEST_JOBS", jb, 1);
  }
  export_ny_full_bin_for(bin);
  export_shape_fixture_tools_for(bin);
  if (bench_mode && !bench_cache && !test_env_truthy("NYTRIX_TEST_COLD")) {
    test_setenv_default("NYTRIX_TEST_EXEC_CACHE", "1");
    test_setenv_default("NYTRIX_TEST_CACHE", "1");
  }
  if (bench_mode && (bench_output_csv || bench_output_json || bench_output_md))
    ny_setenv("NYTRIX_BENCH_CODE_METRICS", "1", 1);
  configure_test_cache_defaults();

  (void)bench_cache;
  if (bench_mode) {
    if (bench_engine && strcmp(bench_engine, "all") == 0) {
      /*
       * Sweep every execution engine in one run so all timings (aot, mcjit,
       * orc, native) are visible side by side.  Each engine gets its own table
       * with the same fixtures and C baseline; wall-time columns let you spot
       * engine-specific regressions directly.
       */
      if (bench_output_csv || bench_output_json || bench_output_md) {
        /*
         * Multi-engine runs produce per-engine tables; file outputs would
         * overwrite each other, so require a single engine for those.
         */
        nyt_err("ny-test", "bench: --bench-engine all cannot write "
                           "--bench-out-* files; use one engine");
        sv_free(&patterns);
        sv_free(&files);
        return 2;
      }
      const char *engines[] = {"aot", "mcjit", "orc", "native"};
      int all_rc = 0;
      for (size_t e = 0; e < sizeof(engines) / sizeof(engines[0]); e++) {
        printf("\n");
        int engine_rc = run_benchmarks(
            bin, std_path, std_bc, NULL, bench_opt_level, bench_tier,
            engines[e], bench_target, bench_runs, bench_warmup, bench_timeout,
            bench_verbose, bench_show_ir, bench_show_asm, bench_show_passes,
            bench_profile, bench_compare_llvm, bench_correctness, NULL, NULL,
            NULL, bench_compile_profile, bench_budget_fail,
            jobs_explicit ? jobs : 0, &files, &patterns);
        if (engine_rc != 0)
          all_rc = engine_rc;
      }
      sv_free(&patterns);
      sv_free(&files);
      return all_rc;
    }
    int bench_rc = run_benchmarks(
        bin, std_path, std_bc, NULL, bench_opt_level, bench_tier, bench_engine,
        bench_target, bench_runs, bench_warmup, bench_timeout, bench_verbose,
        bench_show_ir, bench_show_asm, bench_show_passes, bench_profile,
        bench_compare_llvm, bench_correctness, bench_output_csv,
        bench_output_json, bench_output_md, bench_compile_profile,
        bench_budget_fail, jobs_explicit ? jobs : 0, &files, &patterns);
    sv_free(&patterns);
    sv_free(&files);
    return bench_rc;
  }

  FailureOutputCapture failure_capture = {0};
  if (failures_only && !failure_output_capture_begin(&failure_capture)) {
    nyt_err("ny-test", "could not initialize --failures-only output capture");
    sv_free(&patterns);
    sv_free(&files);
    return 2;
  }

  const char *ws = getenv("NYTRIX_TEST_WITH_STDLIB");
  if (ws && *ws && (*ws != '0') && strcmp(ws, "false") != 0)
    with_stdlib = 1;

  if (files.len == 0) {
    /*
     * The corpus is organized by concern now; collect the single root so new
     * categories are automatically covered without another path allow-list.
     */
    if (!test_collect_native("etc/tests", &files, false) ||
        (with_stdlib && !test_collect_native("lib", &files, false))) {
      nyt_err("ny-test",
              "native fixture discovery failed while discovering tests");
      sv_free(&patterns);
      sv_free(&files);
      return 1;
    }
    expand_multi_shape_files(&files);
  } else {
    /*
     * Explicitly selected files may also be multi-shape files.
     */
    expand_multi_shape_files(&files);
  }
  if (files.len > 1)
    qsort(files.items, files.len, sizeof(char *), path_lex_cmp);

  if (jobs <= 0) {
    const char *ej = getenv("NYTRIX_TEST_JOBS");
    if (ej && *ej) {
      int v = 0;
      if (ny_parse_int(ej, &v))
        jobs = v;
    }
    if (jobs <= 0)
      jobs = auto_test_jobs();
  }
  /*
   * Even explicitly requested parallelism gets clamped to what memory can
   * hold. Native codegen reached ~3.2 GiB RSS; reserve 8 GiB for the host and
   * 0.2 GiB headroom per worker. Override with NYTRIX_TEST_NO_RAM_GUARD=1.
   */
  {
    double avail = test_mem_available_gib();
    if (avail > 0.0 && !getenv("NYTRIX_TEST_NO_RAM_GUARD")) {
      int mem_jobs = (int)((avail - NY_TEST_HOST_MEMORY_RESERVE_GIB) /
                           NY_TEST_WORKER_MEMORY_GIB);
      if (mem_jobs < 1)
        mem_jobs = 1;
      if (jobs > mem_jobs) {
        printf("[test] clamping parallel jobs %d -> %d: only %.1f GiB memory "
               "available at %.1f GiB/worker (override: "
               "NYTRIX_TEST_NO_RAM_GUARD=1)\n",
               jobs, mem_jobs, avail, NY_TEST_WORKER_MEMORY_GIB);
        jobs = mem_jobs;
      }
    }
  }

  int use_cache = 1;
  const char *cache_off = getenv("NYTRIX_TEST_CACHE");
  if (cache_off && (*cache_off == '0' || strcmp(cache_off, "false") == 0))
    use_cache = 0;

  int passed = 0, failed = 0;
  size_t limit = files.len;
  if (smoke && !no_smoke && limit > 64)
    limit = 64;
  const char *native_cache = getenv("NYTRIX_TEST_NO_NATIVE_CACHE");
  const char *jit_cache = getenv("NYTRIX_JIT_CACHE");
  const char *aot_cache = getenv("NYTRIX_AOT_CACHE");
  const char *std_cache = getenv("NYTRIX_STD_CACHE");
  bool native_cache_on = !(native_cache && (*native_cache == '1' ||
                                            strcmp(native_cache, "true") == 0));
  bool jit_cache_on =
      !(jit_cache && (*jit_cache == '0' || strcmp(jit_cache, "false") == 0));
  bool aot_cache_on =
      !(aot_cache && (*aot_cache == '0' || strcmp(aot_cache, "false") == 0));
  bool std_cache_on =
      !(std_cache && (*std_cache == '0' || strcmp(std_cache, "false") == 0));
  printf("%s[mode]%s real timeout=%ds result_cache=%s native_cache=%s "
         "jit_cache=%s aot_cache=%s "
         "std_cache=%s\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), timeout_sec,
         use_cache ? "on" : "off", native_cache_on ? "on" : "off",
         jit_cache_on ? "on" : "off", aot_cache_on ? "on" : "off",
         std_cache_on ? "on" : "off");
  print_host_line(jobs);
  {
    const char *self_check_bin = bin && *bin ? bin : "(unset)";
    const char *self_check_full = getenv("NYTRIX_NY_FULL_BIN");
    printf("%s[bin]%s launcher=%s ny-full=%s\n", nyt_clr(NYT_GRAY),
           nyt_clr(NYT_RESET), self_check_bin,
           self_check_full && *self_check_full
               ? self_check_full
               : "(no sibling ny-full; PATH fallback)");
  }
  const char *pj = getenv("NYTRIX_TEST_PROFILE_JSON");
  const char *td = getenv("NYTRIX_TEST_TRACE_DIR");
  if (pj && *pj)
    printf("%s[trace]%s profile_json=%s\n", nyt_clr(NYT_GRAY),
           nyt_clr(NYT_RESET), pj);
  if (td && *td)
    printf("%s[trace]%s trace_dir=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET),
           td);

  StrVec benchmark = {0}, runtime = {0}, native = {0}, interop = {0},
         repl = {0}, probe = {0}, error_tests = {0}, std = {0};
  size_t skipped_system_stdlib = 0;
  size_t skipped_native_host = 0;
  size_t skipped_native_platform = 0;
  size_t skipped_ci = 0;
  size_t skipped_web_browser = 0;
  size_t skipped_cmake_helper_sources = 0;
  size_t skipped_optional_win64 = 0;
  size_t skipped_optional_core_bc_checker = 0;
  size_t skipped_loopback = 0;
  size_t skipped_audio = 0;
  /*
   * `ci skip` marks hosted-device/GUI probes, not CPU coverage.  Keep them
   * out of the default suite even when a local DISPLAY is present; callers
   * that intentionally provide the required device can opt in explicitly.
   */
  const int include_ci_probes = test_env_truthy("NYTRIX_TEST_INCLUDE_CI");
  const int skip_system_stdlib =
      test_env_truthy("NYTRIX_TEST_SKIP_SYSTEM_STDLIB");
  const int loopback_bind_available = test_host_loopback_bind_available();
  const int audio_service_available = test_host_audio_service_available();
  SuiteStats sb = {0}, sr = {0}, sn = {0}, si = {0}, srepl = {0}, sp = {0},
             se = {0}, ss = {0};
  char cache_path[PATH_MAX];
  nyt_path_join(cache_path, sizeof(cache_path), nyt_default_cache_root_dir(),
                "test-results.tsv");
  /*
   * Result-cache bypasses (notably --cold) must still use prior durations as
   * scheduling hints. Cache hits remain gated by use_cache; loading estimates
   * does not skip or weaken any fixture execution.
   */
  cache_load(&cache, cache_path);
  for (size_t i = 0; i < limit; i++) {
    const char *p = files.items[i];
    TestDomain domain = test_metadata_domain(p);
    /*
     * Browser fixtures belong to the dedicated web-test runner. Honour other
     * ci skip/optional probes only when explicitly requested; these require a
     * hosted display or device. Unmarked CPU-only probes remain in this suite.
     * These are inputs to CMake artifact/expansion checks, rather than
     * standalone programs. The full make test gate runs their CMake checks.
     */
    if (!loopback_bind_available &&
        (strcmp(p,
                "etc/tests/lang/modules/http-server-request-reading.nshape") ==
             0 ||
         strcmp(p, "etc/tests/code/probes/remote-socket.nshape") == 0)) {
      skipped_loopback++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (!audio_service_available &&
               (strcmp(p,
                       "etc/tests/code/probes/platform-audio-unavailable.ny") ==
                    0 ||
                strcmp(p, "etc/tests/code/probes/platform-audio-playback.ny") ==
                    0)) {
      skipped_audio++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (strcmp(p, "etc/tests/lang/comptime/reflection/"
                         "semantic-effect-artifact.ny") == 0 ||
               strcmp(p, "etc/tests/code/formats/coff64/"
                         "aggregate-call-probe.ny") == 0) {
      skipped_cmake_helper_sources++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (strcmp(p, "etc/tests/code/formats/coff64/"
                         "win64-aggregate-runtime.nshape") == 0 &&
               (!getenv("NYTRIX_TEST_WIN64_CC") ||
                !*getenv("NYTRIX_TEST_WIN64_CC") ||
                !getenv("NYTRIX_TEST_WIN64_RUNNER") ||
                !*getenv("NYTRIX_TEST_WIN64_RUNNER"))) {
      /*
       * CMake registers this fixture only when both tools are available.
       */
      skipped_optional_win64++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (strcmp(p, "etc/tests/code/probes/cache/"
                         "core-bc-cache-selector.nshape") == 0 &&
               (!getenv("NYTRIX_TEST_CORE_BC_CHECKER") ||
                !*getenv("NYTRIX_TEST_CORE_BC_CHECKER"))) {
      /*
       * CMake registers this fixture only when it builds the checker tool.
       */
      skipped_optional_core_bc_checker++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (test_path_in(p, "lang/interop/web/")) {
      skipped_web_browser++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if ((shape_skips_ci(p) && !include_ci_probes) ||
               shape_skips_headless(p)) {
      skipped_ci++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (test_is_unsupported_native_platform(p)) {
      skipped_native_platform++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (test_is_unsupported_native_host(p)) {
      skipped_native_host++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else if (domain == TEST_DOMAIN_REPL || path_is_repl_test(p))
      sv_push(&repl, p);
    else if (domain == TEST_DOMAIN_BENCHMARK || test_path_is_benchmark(p))
      sv_push(&benchmark, p);
    else if (domain == TEST_DOMAIN_ERROR || test_path_in(p, "lang/errors/"))
      sv_push(&error_tests, p);
    else if (domain == TEST_DOMAIN_RUNTIME || test_path_is_runtime_domain(p) ||
             test_path_is_language_domain(p))
      sv_push(&runtime, p);
    else if (domain == TEST_DOMAIN_INTEROP || test_path_in(p, "lang/interop/"))
      sv_push(&interop, p);
    else if (domain == TEST_DOMAIN_NATIVE || test_path_in(p, "code/") ||
             test_path_in(p, "lang/")) {
      if (path_is_probe_test(p)) {
        sv_push(&probe, p);
      } else if (path_is_native_test(p)) {
        sv_push(&native, p);
      } else
        sv_push(&runtime, p);
    } else if (domain == TEST_DOMAIN_PROBE || path_is_probe_test(p))
      sv_push(&probe, p);
    else if (path_is_probe_test(p))
      sv_push(&probe, p);
    else if (test_path_in(p, "lang/errors/"))
      sv_push(&error_tests, p);
    else if (skip_system_stdlib && test_is_optional_system_stdlib(p)) {
      skipped_system_stdlib++;
      timings_push(&timings, p, 0, "Skipped", "skip");
    } else
      sv_push(&std, p);
  }

  if (skipped_system_stdlib > 0)
    printf("%s[note]%s skipped %zu optional system stdlib modules "
           "(UI/audio/clipboard)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_system_stdlib);
  if (skipped_native_host > 0)
    printf("%s[note]%s skipped %zu native execution fixtures for a different "
           "host architecture\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_native_host);
  if (skipped_native_platform > 0)
    printf("%s[note]%s skipped %zu native fixtures requiring another host "
           "object/runtime format\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_native_platform);
  if (skipped_ci > 0)
    printf("%s[note]%s skipped %zu nshape fixtures marked ci skip\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_ci);
  if (skipped_web_browser > 0)
    printf("%s[note]%s skipped %zu browser-only fixtures under "
           "etc/tests/lang/interop/web (run via ./make web-test)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_web_browser);
  if (skipped_cmake_helper_sources > 0)
    printf("%s[note]%s skipped %zu CMake-only fixture sources\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_cmake_helper_sources);
  if (skipped_optional_win64 > 0)
    printf("%s[note]%s skipped %zu Win64 runtime fixtures without the optional "
           "cross compiler and runner\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_optional_win64);
  if (skipped_optional_core_bc_checker > 0)
    printf("%s[note]%s skipped %zu core.bc cache fixtures without the optional "
           "checker tool\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET),
           skipped_optional_core_bc_checker);
  if (skipped_loopback > 0)
    printf("%s[note]%s skipped %zu loopback fixtures because this host denies "
           "binding a local socket\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_loopback);
  if (skipped_audio > 0)
    printf("%s[note]%s skipped %zu host audio fixtures because no usable audio "
           "device or PulseAudio/PipeWire socket is available\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_audio);

  StrVec selected_all = {0};
  for (size_t i = 0; i < benchmark.len; i++)
    sv_push(&selected_all, benchmark.items[i]);
  for (size_t i = 0; i < runtime.len; i++)
    sv_push(&selected_all, runtime.items[i]);
  for (size_t i = 0; i < native.len; i++)
    sv_push(&selected_all, native.items[i]);
  for (size_t i = 0; i < interop.len; i++)
    sv_push(&selected_all, interop.items[i]);
  for (size_t i = 0; i < probe.len; i++)
    sv_push(&selected_all, probe.items[i]);
  if (with_stdlib) {
    for (size_t i = 0; i < std.len; i++)
      sv_push(&selected_all, std.items[i]);
  }
  if (selected_all.len > 1)
    qsort(selected_all.items, selected_all.len, sizeof(char *), path_lex_cmp);

  StrVec grouped_tests[5] = {{0}};
  const char *group_names[] = {"Tests · lib", "Tests · lang", "Tests · code",
                               "Tests · bench", "Tests · other"};
  for (size_t i = 0; i < selected_all.len; i++) {
    const char *path = selected_all.items[i];
    size_t group = 4;
    if (test_path_is_benchmark(path))
      group = 3;
    else if (strncmp(path, "lib/", 4) == 0 || test_path_in(path, "lib/"))
      group = 0;
    else if (test_path_in(path, "lang/"))
      group = 1;
    else if (test_path_in(path, "code/"))
      group = 2;
    sv_push(&grouped_tests[group], path);
  }
  for (size_t i = 0; i < sizeof(grouped_tests) / sizeof(grouped_tests[0]);
       i++) {
    if (grouped_tests[i].len > 0 && (!fail_fast || !failed))
      run_suite(group_names[i], &grouped_tests[i], bin, std_path, std_bc,
                &patterns, jobs, timeout_sec, &passed, &failed, fail_fast, 0,
                NULL, &sb, &sr, &sn, &si, &sp, &ss, &cache, use_cache, &timings,
                &failed_paths, cache_path);
    sv_free(&grouped_tests[i]);
  }
  sv_free(&selected_all);
  if (!fail_fast || !failed)
    run_repl_suite(&repl, bin, std_path, std_bc, &patterns, timeout_sec,
                   &passed, &failed, &srepl, &timings, &failed_paths,
                   fail_fast);
  if (!fail_fast || !failed)
    run_error_suite(&error_tests, bin, std_path, std_bc, &patterns, jobs,
                    timeout_sec, &passed, &failed, &se, &cache, use_cache,
                    &timings, &failed_paths, fail_fast, cache_path);

  if (!with_stdlib && (files.len == 0 || std.len == 0))
    printf("%s[note]%s stdlib sweep disabled (use --with-stdlib or "
           "NYTRIX_TEST_WITH_STDLIB=1)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));

  if (use_cache)
    cache_save(&cache, cache_path);

  if (pj && *pj) {
    FILE *f = fopen(pj, "wb");
    if (f) {
      size_t profile_pass = 0, profile_fail = 0, profile_timeout = 0,
             profile_skip = 0;
      for (size_t i = 0; i < timings.len; i++) {
        const char *status = timings.items[i].status;
        if (status && strcmp(status, "skip") == 0)
          profile_skip++;
        else if (status && strcmp(status, "timeout") == 0)
          profile_timeout++;
        else if (status && strcmp(status, "fail") == 0)
          profile_fail++;
        else
          profile_pass++;
      }
      fprintf(f, "{\n");
      fprintf(f, "  \"version\": 1,\n");
      fprintf(f,
              "  \"counts\": {\"pass\": %zu, \"fail\": %zu, \"timeout\": %zu, "
              "\"skip\": %zu},\n",
              profile_pass, profile_fail, profile_timeout, profile_skip);
      fprintf(f, "  \"suites\": {\n");
      fprintf(f,
              "    \"Benchmark\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": "
              "%d, \"max_ms\": %d},\n",
              sb.tests, sb.passed, sb.sum_ms, sb.max_ms);
      fprintf(f,
              "    \"Runtime\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": "
              "%d, \"max_ms\": %d},\n",
              sr.tests, sr.passed, sr.sum_ms, sr.max_ms);
      fprintf(f,
              "    \"Native\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, "
              "\"max_ms\": %d},\n",
              sn.tests, sn.passed, sn.sum_ms, sn.max_ms);
      fprintf(f,
              "    \"Interop\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": "
              "%d, \"max_ms\": %d},\n",
              si.tests, si.passed, si.sum_ms, si.max_ms);
      fprintf(f,
              "    \"Repl\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, "
              "\"max_ms\": %d},\n",
              srepl.tests, srepl.passed, srepl.sum_ms, srepl.max_ms);
      fprintf(f,
              "    \"Probe\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, "
              "\"max_ms\": %d},\n",
              sp.tests, sp.passed, sp.sum_ms, sp.max_ms);
      fprintf(f,
              "    \"Error\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, "
              "\"max_ms\": %d},\n",
              se.tests, se.passed, se.sum_ms, se.max_ms);
      fprintf(f,
              "    \"Std\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, "
              "\"max_ms\": %d}\n",
              ss.tests, ss.passed, ss.sum_ms, ss.max_ms);
      fprintf(f, "  },\n");
      fprintf(f, "  \"timings\": [\n");
      for (size_t i = 0; i < timings.len; i++) {
        const TimingRow *t = &timings.items[i];

        fprintf(f,
                "    {\"path\": \"%s\", \"ms\": %d, \"suite\": \"%s\", "
                "\"status\": \"%s\"}%s\n",
                t->path ? t->path : "", t->ms, t->suite ? t->suite : "",
                t->status ? t->status : "pass",
                (i + 1 < timings.len) ? "," : "");
      }
      fprintf(f, "  ]\n");
      fprintf(f, "}\n");
      fclose(f);
    }
  }

  print_section("Timing Summary");
  int compact_timing = ny_ui_width() < 88;
  if (!compact_timing)
    printf("%sSuite      Tests  Pass    Total     Avg      Max%s\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
  print_timing_suite_row("Benchmark", &sb, compact_timing);
  print_timing_suite_row("Runtime", &sr, compact_timing);
  print_timing_suite_row("Native", &sn, compact_timing);
  print_timing_suite_row("Interop", &si, compact_timing);
  print_timing_suite_row("Repl", &srepl, compact_timing);
  print_timing_suite_row("Probe", &sp, compact_timing);
  print_timing_suite_row("Error", &se, compact_timing);
  print_timing_suite_row("Std", &ss, compact_timing);

  const int empty_filtered_run = patterns.len > 0 && passed + failed == 0;
  if (empty_filtered_run)
    fprintf(stderr, "No test fixtures matched the supplied filter.\n");

  if (timings.len > 0) {
    qsort(timings.items, timings.len, sizeof(TimingRow), timing_row_cmp_desc);
    size_t slow_count = 0;
    for (size_t i = 0; i < timings.len; i++) {
      const TimingRow *row = &timings.items[i];
      if (!row->status || strcmp(row->status, "skip") != 0) {
        slow_count++;
        if (slow_count == 1)
          printf("%s%s Slowest fixtures:%s\n", nyt_clr(NYT_GRAY),
                 ny_ui_icon("clock"), nyt_clr(NYT_RESET));
      }
    }
    if (slow_count > 8)
      slow_count = 8;
    size_t shown = 0;
    for (size_t i = 0; i < timings.len && shown < slow_count; i++) {
      const TimingRow *row = &timings.items[i];
      if (row->status && strcmp(row->status, "skip") == 0)
        continue;
      printf("  %zu. %6dms  %s [%s, %s]\n", ++shown, row->ms,
             disp_path(row->path), row->suite ? row->suite : "Suite",
             row->status ? row->status : "pass");
    }
  }

  nyt_rule(stdout);
#if defined(__APPLE__) || defined(__linux__)
  {
    /*
     * Surface the memory the suite actually used: ru_maxrss of reaped
     * children is the peak resident set any single fixture (and its
     * compile children) reached.
     */
    struct rusage ru;
    if (getrusage(RUSAGE_CHILDREN, &ru) == 0 && ru.ru_maxrss > 0) {
#ifdef __APPLE__
      const double bytes_per_unit = 1.0;
#else
      const double bytes_per_unit = 1024.0;
#endif
      printf("Peak fixture RSS: %.1f MiB\n",
             (double)ru.ru_maxrss * bytes_per_unit / (1024.0 * 1024.0));
    }
  }
#endif
  printf("%s%d total%s %s· %d passed%s · %s%d failed%s · %dms\n",
         nyt_clr(NYT_BOLD), passed + failed, nyt_clr(NYT_RESET),
         nyt_clr(NYT_GRAY), passed, nyt_clr(NYT_RESET),
         failed ? nyt_clr(NYT_RED) : nyt_clr(NYT_GRAY), failed,
         nyt_clr(NYT_RESET), (int)(now_ms() - suite_started_ms));
  if (failed)
    debug_replay_failed_tests(&failed_paths, bin, std_path, std_bc,
                              timeout_sec);
  sv_free(&benchmark);
  sv_free(&runtime);
  sv_free(&native);
  sv_free(&interop);
  sv_free(&repl);
  sv_free(&probe);
  sv_free(&error_tests);
  sv_free(&std);
  timings_free(&timings);
  cache_free(&cache);
  sv_free(&patterns);
  sv_free(&failed_paths);
  sv_free(&files);
  if (failures_only)
    failure_output_capture_end(&failure_capture);

  return (failed || empty_filtered_run) ? 1 : 0;
}

/*
 * Benchmark Comparison infrastructure
 *
 * `ny-test --bench [PATTERN ...]` times Ny's `-time` execution phase and the
 * generated wrapper around each host-C `main`, after checking route checksums.
 * Shapes without a C block report Ny timing only.
 *
 * Paths resolve dynamically: the bench directory comes from
 * `NYTRIX_SHARE_ROOT`/`NYTRIX_ROOT` when set and that subtree contains benches,
 * otherwise it is taken relative to the process CWD (the repository root).  No
 * absolute install prefix is assumed, so the tool also works when installed
 * system-wide (e.g. under /opt/nytrix).
 */

/*
 * Per-benchmark result row.
 */
typedef struct {
  char name[128];
  char path[PATH_MAX];
  double wall_ms;
  double ny_ms;
  double ny_compile_ms;
  double ny_link_ms;
  double ny_jit_compile_ms;
  double ny_jit_run_ms;
  double ny_opt_ms;
  double ny_total_ms;
  size_t ny_code_bytes;
  int ny_code_bytes_seen;
  size_t ny_specialization_code_bytes;
  size_t ny_specialization_code_functions;
  size_t ny_specialization_max_function_bytes;
  int ny_specialization_metrics_seen;
  size_t ny_peak_compiler_rss_kb;
  int ny_peak_compiler_rss_seen;
  size_t ny_runtime_calls;
  size_t ny_dynamic_ops;
  size_t ny_tag_checks;
  size_t ny_box_unbox;
  size_t ny_heap_allocations;
  size_t ny_bounds_checks;
  size_t ny_direct_calls;
  size_t ny_indirect_calls;
  size_t ny_unknown_effects;
  size_t ny_alias_unresolved;
  size_t ny_vector_attempted;
  size_t ny_vector_rejected;
  size_t ny_vectorized;
  size_t ny_spills;
  size_t ny_reloads;
  int ny_static_metrics_seen;
  uint64_t ny_hw_cycles;
  uint64_t ny_hw_instructions;
  uint64_t ny_hw_branches;
  uint64_t ny_hw_branch_misses;
  uint64_t ny_hw_cache_misses;
  int ny_hw_counters_seen;
  uint64_t llvm_hw_cycles;
  uint64_t llvm_hw_instructions;
  uint64_t llvm_hw_branches;
  uint64_t llvm_hw_branch_misses;
  uint64_t llvm_hw_cache_misses;
  int llvm_hw_counters_seen;
  size_t ny_runtime_alloc_count;
  size_t ny_runtime_realloc_count;
  int ny_runtime_counters_seen;
  double c_ms;
  double c_total_ms;
  double llvm_ms;
  double llvm_total_ms;
  int ny_runs;
  int c_runs;
  int llvm_runs;
  int ny_timer_fallback;
  int llvm_timer_fallback;
  int ny_fixture_timer;
  int llvm_fixture_timer;
  int c_fixture_timer;
  int ny_rc;
  int c_rc;
  int llvm_rc;
  int ny_failures;
  int c_failures;
  int llvm_failures;
  int checksum_ok;
  char checksum[128];
  char ny_checksum[128];
  char llvm_checksum[128];
  char c_checksum[128];
  char backend[32];
  char engine[32];
  char cache[24];
  double ny_min, ny_max, c_min, c_max, llvm_min, llvm_max;
  double ny_p95, c_p95, llvm_p95;
  double ny_dispersion_pct, c_dispersion_pct, llvm_dispersion_pct;
  double ny_noise_pct, c_noise_pct, llvm_noise_pct;
  int ny_unstable, c_unstable, llvm_unstable;
  double native_c_budget, native_llvm_budget;
  double compile_ms_budget;
  size_t code_bytes_budget;
  size_t specialization_code_bytes_budget;
  size_t specialization_function_bytes_budget;
  size_t compiler_rss_budget_kb;
  size_t heap_allocations_budget;
  size_t runtime_calls_budget;
  size_t bounds_checks_budget;
  size_t indirect_calls_budget;
  size_t unknown_effects_budget;
  size_t alias_unresolved_budget;
  size_t spills_budget;
  size_t reloads_budget;
  unsigned static_budget_mask;
  int budget_failed;
} BenchRow;

typedef struct {
  BenchRow *items;
  size_t len;
  size_t cap;
} BenchTable;

static void bench_write_full_report(const char *path, const BenchTable *table,
                                    const char *bin, const char *opt_level,
                                    const char *tier, const char *engine,
                                    const char *target, int compare_llvm);

/*
 * Keep verbose benchmark artifacts useful in Markdown without allowing one
 * generated function or pass dump to dominate the entire report.
 */
static void bench_report_block(FILE *report, const char *heading,
                               const char *fence, const char *text) {
  if (!report || !heading || !fence || !text)
    return;
  const size_t limit = 12u * 1024u;
  size_t len = strlen(text);
  size_t written = 0;
  fprintf(report, "\n### %s\n\n```%s\n", heading, fence);
  for (size_t i = 0; i < len && written < limit;) {
    unsigned char ch = (unsigned char)text[i++];
    if (ch == 0x1b && i < len) {
      unsigned char next = (unsigned char)text[i++];
      if (next == '[') {
        while (i < len) {
          unsigned char end = (unsigned char)text[i++];
          if (end >= 0x40 && end <= 0x7e)
            break;
        }
      } else if (next == ']') {
        while (i < len) {
          unsigned char end = (unsigned char)text[i++];
          if (end == '\a')
            break;
          if (end == 0x1b && i < len && text[i] == '\\') {
            i++;
            break;
          }
        }
      }
      continue;
    }
    if (ch < 0x20 && ch != '\n' && ch != '\r' && ch != '\t')
      continue;
    fputc(ch, report);
    written++;
  }
  if (written >= limit && len > written)
    fprintf(report,
            "\n[truncated: %zu of %zu bytes shown; use the focused compiler "
            "diagnostic command for the complete artifact]\n",
            written, len);
  fprintf(report, "\n```\n");
}

static void bench_rows_push(BenchTable *t, const BenchRow *r) {
  if (!t)
    return;
  if (t->len == t->cap) {
    size_t nc = t->cap ? t->cap * 2 : 32;
    BenchRow *p = (BenchRow *)realloc(t->items, nc * sizeof(*p));
    if (!p)
      return;
    t->items = p;
    t->cap = nc;
  }
  t->items[t->len++] = *r;
}

static void bench_rows_free(BenchTable *t) {
  free(t->items);
  t->items = NULL;
  t->len = 0;
  t->cap = 0;
}

/*
 * Locate the bench fixture directory without baking in an install prefix.
 */
static const char *bench_dir_root(void) {
  static char root[PATH_MAX];
  const char *envs[] = {getenv("NYTRIX_SHARE_ROOT"), getenv("NYTRIX_ROOT")};
  for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
    const char *e = envs[i];
    if (!e || !*e)
      continue;
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/etc/tests/bench", e);
    if (is_dir(probe)) {
      nyt_path_copy(root, sizeof(root), e);
      return root;
    }
  }
  if (is_dir("etc/tests/bench")) {
    if (getcwd(root, sizeof(root)))
      return root;
    return ".";
  }
  return NULL;
}

/*
 * A self-reported sample larger than this is usually a tagged/raw clock
 * mismatch (elapsed ≈ machine uptime), not a plausible micro-kernel time.
 */
#define BENCH_SAMPLE_MAX_MS (60.0 * 1000.0)

static double bench_parse_elapsed(const char *out) {
  if (!out)
    return -1.0;
  const char *ns = strstr(out, "bench_elapsed_ns=");
  if (!ns)
    ns = strstr(out, "bench_process_elapsed_ns=");
  if (!ns)
    ns = strstr(out, "elapsed_ns=");
  const char *ms = strstr(out, "elapsed_ms=");
  const char *k = ns ? ns : (ms ? ms : NULL);
  if (!k)
    return -1.0;
  k = strchr(k, '=');
  if (!k)
    return -1.0;
  k++;
  while (*k && isspace((unsigned char)*k))
    k++;
  if (!*k || !isdigit((unsigned char)*k))
    return -1.0;
  double v = 0.0;
  while (*k && isdigit((unsigned char)*k))
    v = v * 10.0 + (double)(*k++ - '0');
  v = ns ? v / 1e6 : v;
  if (v > BENCH_SAMPLE_MAX_MS)
    return -1.0;
  return v;
}
/*
 * Copy a line-oriented result marker such as `checksum=...` without
 * interpreting its payload.  Checksums are intentionally compared as text:
 * fixtures choose their own scalar/string representation.
 */
static int bench_parse_marker(const char *out, const char *label, char *dst,
                              size_t cap) {
  if (!out || !label || !dst || cap == 0)
    return 0;
  const char *p = out;
  size_t label_len = strlen(label);
  while ((p = strstr(p, label)) != NULL) {
    /*
     * Compiler diagnostics echo source lines, so a fixture line such as
     * print("checksum=", x) reaches this buffer as excerpt text.  A marker
     * inside a string literal (the preceding character is a quote) is source
     * text, never program output; skip it and keep scanning.
     */
    if (p > out && p[-1] == '"') {
      p += label_len;
      continue;
    }
    break;
  }
  if (!p)
    return 0;
  p += label_len;
  /*
   * Multi-argument prints join fields with a separator (`checksum= 12`), so
   * allow one separator's worth of whitespace between the label and the
   * payload.  The payload itself keeps interior spacing intact, and the
   * trailing trim preserves the existing end-of-line contract.
   */
  while (*p && isspace((unsigned char)*p) && *p != '\n' && *p != '\r')
    p++;
  const char *end = p;
  while (*end && *end != '\n' && *end != '\r')
    end++;
  while (end > p && isspace((unsigned char)end[-1]))
    end--;
  size_t len = (size_t)(end - p);
  if (len == 0 || len >= cap)
    return 0;
  memmove(dst, p, len);
  dst[len] = '\0';
  return 1;
}

static const char *bench_cache_state(void) {
  const char *result = getenv("NYTRIX_TEST_CACHE");
  const char *native = getenv("NYTRIX_TEST_NO_NATIVE_CACHE");
  const char *jit = getenv("NYTRIX_JIT_CACHE");
  const char *aot = getenv("NYTRIX_AOT_CACHE");
  if (result && (*result == '0' || strcmp(result, "false") == 0))
    return "cold";
  if ((native && (*native == '1' || strcmp(native, "true") == 0)) ||
      (jit && (*jit == '0' || strcmp(jit, "false") == 0)) ||
      (aot && (*aot == '0' || strcmp(aot, "false") == 0)))
    return "warm-no-exec";
  return "warm";
}

/*
 * Parse a compiler timing line such as `Optimization: 0.0123s`.
 */
static double bench_parse_phase_ms(const char *out, const char *label) {
  if (!out || !label)
    return -1.0;
  const char *p = strstr(out, label);
  if (!p)
    return -1.0;
  p += strlen(label);
  while (*p && isspace((unsigned char)*p))
    p++;
  char *end = NULL;
  double seconds = strtod(p, &end);
  if (end == p || seconds < 0.0)
    return -1.0;
  while (*end && isspace((unsigned char)*end))
    end++;
  if (*end != 's')
    return -1.0;
  return seconds * 1000.0;
}

static double bench_median(double *xs, int n) {
  if (n <= 0)
    return -1.0;
  for (int i = 0; i < n - 1; i++)
    for (int j = i + 1; j < n; j++)
      if (xs[j] < xs[i]) {
        double t = xs[i];
        xs[i] = xs[j];
        xs[j] = t;
      }
  if (n % 2)
    return xs[n / 2];
  return (xs[n / 2 - 1] + xs[n / 2]) / 2.0;
}

static double bench_noise_limit_pct(void) {
  const char *raw = getenv("NYTRIX_BENCH_MAX_NOISE_PCT");
  if (!raw || !*raw)
    return 20.0;
  char *end = NULL;
  double value = strtod(raw, &end);
  if (end == raw || value < 0.0 || value > 500.0)
    return 20.0;
  return value;
}

/*
 * Run argv, capturing stdout+stderr to output_path, waiting up to timeout_sec.
 * Returns child status rc (NY_TEST_TIMEOUT_RC on timeout).  Works on POSIX via
 * fork and on Windows via the spawn helper.
 *
 * Capture-path validation for child CREATE sites.  Normalize the path
 * lexically (the file usually does not exist yet, so realpath() is not
 * usable) and reject any form that escapes toward a parent directory:
 * absolute paths must stay absolute without ".." components, relative
 * paths must not climb above their base.  This is the single traversal
 * gate for bench/failure/replay capture files (static-analysis HIGH).
 */
static int ny_test_capture_path_ok(const char *path, char *resolved,
                                   size_t resolved_len) {
  if (!path || !*path)
    return 0;
  char base[PATH_MAX];
  const char *src = path;
  size_t used = 0;
  /*
   * Floor for the lexical cursor: the root slash for absolute inputs, or
   * the cwd prefix for relative ones — a ".." may never climb past it.
   */
  size_t floor_len = 1;
  if (path[0] != '/') {
    if (!getcwd(base, sizeof(base)))
      return 0;
    int n = snprintf(resolved, resolved_len, "%s", base);
    if (n < 0 || (size_t)n >= resolved_len)
      return 0;
    used = (size_t)n;
    floor_len = used;
  } else {
    resolved[0] = '/';
    resolved[1] = '\0';
    used = 1;
  }
  /*
   * Component-wise copy, resolving "." and ".." lexically.
   */
  const char *p = src;
  while (*p) {
    while (*p == '/')
      ++p;
    if (!*p)
      break;
    const char *seg = p;
    while (*p && *p != '/')
      ++p;
    size_t seg_len = (size_t)(p - seg);
    if (seg_len == 1 && seg[0] == '.')
      continue;
    if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
      /*
       * Pop one component; a climb past the floor (cwd prefix or the
       * absolute root) is the traversal rejection.
       */
      while (used > 0 && resolved[used - 1] != '/')
        --used;
      if (used == 0)
        return 0;
      --used; /* remove the slash */
      if (used < floor_len) {
        if (path[0] == '/' && used == 0) {
          used = 1;
          continue;
        }
        return 0;
      }
      if (used == 0 && resolved[0] == '/')
        used = 1;
      continue;
    }
    if (used + seg_len + 2 >= resolved_len)
      return 0;
    if (used > 0 && resolved[used - 1] != '/')
      resolved[used++] = '/';
    memmove(resolved + used, seg, seg_len);
    used += seg_len;
    resolved[used] = '\0';
  }
  if (used == 0)
    return 0;
  return 1;
}

static int bench_run_capture_usage(char *const argv[], int timeout_sec,
                                   const char *output_path,
                                   size_t *peak_rss_kb) {
  if (peak_rss_kb)
    *peak_rss_kb = 0;
  fflush(NULL);
#ifdef _WIN32
  ny_test_proc_t proc = ny_test_spawn_argv(argv, output_path, 0);
  if (!ny_test_proc_valid(proc))
    return 127;
  int timed_out = 0;
  int rc = ny_test_wait_rc(proc, timeout_sec, &timed_out);
  ny_test_proc_close(proc);
  return timed_out ? NY_TEST_TIMEOUT_RC : rc;
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    ny_test_isolate_child_group();
    apply_test_child_env();
    /*
     * Bench captures feed failure replay: keep the child's diagnostics
     * colored when the runner itself is on a terminal (the capture file
     * otherwise flips the compiler to no-color via the isatty check).
     */
    if (isatty(STDERR_FILENO))
      ny_setenv("FORCE_COLOR", "1", 1);
    if (output_path) {
      char resolved_path[PATH_MAX];
      if (!ny_test_capture_path_ok(output_path, resolved_path,
                                   sizeof(resolved_path)))
        _exit(126);
      int fd = open(resolved_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    } else {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }
    apply_test_child_mem_limit();
    execvp(argv[0], argv);
    _exit(127);
  }
  if (pid <= 0)
    return 127;
  int status = 0;
  double start_ms = now_ms();
  double timeout_ms = (double)timeout_sec * 1000.0;
  for (;;) {
#if defined(__APPLE__) || defined(__linux__)
    struct rusage usage;
    memset(&usage, 0, sizeof(usage));
    pid_t r = wait4(pid, &status, WNOHANG, &usage);
#else
    pid_t r = waitpid(pid, &status, WNOHANG);
#endif
    if (r == pid) {
#if defined(__APPLE__) || defined(__linux__)
      if (peak_rss_kb) {
        unsigned long long rss =
            usage.ru_maxrss > 0 ? (unsigned long long)usage.ru_maxrss : 0;
#ifdef __APPLE__
        rss /= 1024u; /* macOS reports bytes; Linux reports KiB. */
#endif
        *peak_rss_kb =
            rss > (unsigned long long)SIZE_MAX ? SIZE_MAX : (size_t)rss;
      }
#endif
      if (WIFEXITED(status))
        return WEXITSTATUS(status);
      if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
      return 128;
    }
    if (r < 0) {
      if (errno == EINTR)
        continue;
      return 127;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      kill(pid, SIGKILL);
#if defined(__APPLE__) || defined(__linux__)
      struct rusage usage;
      memset(&usage, 0, sizeof(usage));
      while (wait4(pid, &status, 0, &usage) < 0 && errno == EINTR) {
      }
      if (peak_rss_kb) {
        unsigned long long rss =
            usage.ru_maxrss > 0 ? (unsigned long long)usage.ru_maxrss : 0;
#ifdef __APPLE__
        rss /= 1024u;
#endif
        *peak_rss_kb =
            rss > (unsigned long long)SIZE_MAX ? SIZE_MAX : (size_t)rss;
      }
#else
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
#endif
      return NY_TEST_TIMEOUT_RC;
    }
    poll_sleep();
  }
#endif
}

static int bench_run_capture(char *const argv[], int timeout_sec,
                             const char *output_path) {
  return bench_run_capture_usage(argv, timeout_sec, output_path, NULL);
}

typedef struct {
  double values[64];
  size_t count;
  double median;
  double min;
  double max;
  double p95;
  double dispersion_pct;
  double noise_pct;
  int unstable;
  double wall_ms;
  int last_rc;
  int failure_count;
  int timeout_count;
  int timer_fallback_count;
  int fixture_timer_count;
  int checksum_seen;
  int checksum_consistent;
  char checksum[128];
  double compile_values[64];
  size_t compile_count;
  double compile_ms;
  double link_values[64];
  size_t link_count;
  double link_ms;
  double jit_compile_values[64];
  size_t jit_compile_count;
  double jit_compile_ms;
  double jit_run_values[64];
  size_t jit_run_count;
  double jit_run_ms;
  double opt_values[64];
  size_t opt_count;
  double opt_ms;
  size_t code_bytes;
  int code_bytes_seen;
  size_t specialization_code_bytes;
  size_t specialization_code_functions;
  size_t specialization_max_function_bytes;
  int specialization_metrics_seen;
  size_t peak_compiler_rss_kb;
  int peak_compiler_rss_seen;
  size_t runtime_calls;
  size_t dynamic_ops;
  size_t tag_checks;
  size_t box_unbox;
  size_t heap_allocations;
  size_t bounds_checks;
  size_t direct_calls;
  size_t indirect_calls;
  size_t unknown_effects;
  size_t alias_unresolved;
  size_t vector_attempted;
  size_t vector_rejected;
  size_t vectorized;
  size_t spills;
  size_t reloads;
  int static_metrics_seen;
  uint64_t hw_cycles;
  uint64_t hw_instructions;
  uint64_t hw_branches;
  uint64_t hw_branch_misses;
  uint64_t hw_cache_misses;
  int hw_counters_seen;
  size_t runtime_alloc_count;
  size_t runtime_realloc_count;
  int runtime_counters_seen;
} bench_stats_t;

static void bench_finalize_samples(bench_stats_t *s) {
  if (!s || s->count == 0)
    return;
  s->median = bench_median(s->values, (int)s->count);
  s->min = s->values[0];
  s->max = s->values[s->count - 1];
  if (s->median > 0.0)
    s->dispersion_pct = ((s->max - s->min) / s->median) * 100.0;
  if (s->count >= 5) {
    size_t rank = (95 * s->count + 99) / 100;
    if (rank == 0)
      rank = 1;
    if (rank > s->count)
      rank = s->count;
    s->p95 = s->values[rank - 1];

    /*
     * Instability must describe the typical sample spread, not a single
     * scheduler interruption.  With the usual 5-run benchmark, nearest-rank
     * p95 is literally the maximum sample; one involuntary deschedule could
     * therefore fail an otherwise stable benchmark.  Use scaled median
     * absolute deviation for the noise verdict while retaining p95/max for
     * reporting and diagnostics.  MAD is robust to isolated host jitter but
     * still grows when the benchmark itself is consistently variable.
     */
    double deviations[64];
    size_t dev_count = s->count < 64 ? s->count : 64;
    for (size_t i = 0; i < dev_count; ++i) {
      double delta = s->values[i] - s->median;
      deviations[i] = delta < 0.0 ? -delta : delta;
    }
    double mad = bench_median(deviations, (int)dev_count);
    if (s->median > 0.0)
      s->noise_pct = (1.4826 * mad / s->median) * 100.0;
    s->unstable = s->noise_pct > bench_noise_limit_pct();
  } else {
    s->p95 = -1.0;
  }
}

/*
 * Run `argv`, discard warm-ups, and collect the selected runtime timer plus
 * compiler timing over the requested samples. Ny prefers its `-time` phase;
 * C uses the generated wrapper marker.
 */
static bench_stats_t bench_measure_self_timed(char *const argv[],
                                              int timeout_sec, int warmup,
                                              int runs, int prefer_jit_timer) {
  bench_stats_t s = {.checksum_consistent = 1, .last_rc = 127};
  double wall_start = now_ms();
  /*
   * A successful process can occasionally omit both timing markers (for
   * example, when a concurrent compiler job interrupts phase reporting).
   * Retry those samples instead of publishing a misleading NO-TIME row. The
   * retry budget is bounded and does not hide nonzero exit statuses.
   */
  int total = warmup + (runs > 0 ? runs * 3 : 0);
  for (int i = 0;
       i < total && s.count < (size_t)(runs > 0 ? runs : 0) && s.count < 64;
       i++) {
    char tmp[PATH_MAX];
    if (make_test_capture_path(tmp, sizeof(tmp), "bench") < 0) {
      if (i >= warmup)
        s.failure_count++;
      continue;
    }
    int rc = bench_run_capture(argv, timeout_sec, tmp);
    char *out = read_small_file(tmp);
    remove(tmp);
    if (i < warmup) {
      free(out);
      continue;
    }
    /*
     * A Ny benchmark's final expression can intentionally be its checksum,
     * which the CLI exposes as a small process status. Treat that as a
     * completed run only when captured program output contains both a
     * checksum and a positive runtime marker. Signal exits, launch failures,
     * and incomplete output remain failures.
     */
    if (rc > 0 && rc < 128 && out) {
      char completed_checksum[sizeof(s.checksum)];
      bool completed_result =
          bench_parse_marker(out, "checksum=", completed_checksum,
                             sizeof(completed_checksum)) &&
          (bench_parse_elapsed(out) > 0.0 ||
           bench_parse_phase_ms(out, "JIT Run:") > 0.0);
      if (completed_result)
        rc = 0;
    }
    s.last_rc = rc;
    if (rc != 0) {
      s.failure_count++;
      if (rc == NY_TEST_TIMEOUT_RC)
        s.timeout_count++;
      if (getenv("NYTRIX_BENCH_SHOW_FAILURES")) {
        fprintf(stderr, "[bench-failure] rc=%d\n%s%s", rc, out ? out : "",
                out && *out && out[strlen(out) - 1] == '\n' ? "" : "\n");
      }
      free(out);
      continue;
    }
    if (out) {
      char checksum[sizeof(s.checksum)];
      if (bench_parse_marker(out, "checksum=", checksum, sizeof(checksum))) {
        if (!s.checksum_seen) {
          snprintf(s.checksum, sizeof(s.checksum), "%s", checksum);
          s.checksum_seen = 1;
        } else if (strcmp(s.checksum, checksum) != 0) {
          s.checksum_consistent = 0;
        }
      }
      bool fixture_timer = out && strstr(out, "bench_elapsed_ns=") != NULL;
      double self_timed_ms = bench_parse_elapsed(out);
      double jit_run_ms = bench_parse_phase_ms(out, "JIT Run:");
      double v = prefer_jit_timer && !fixture_timer && jit_run_ms > 0.0
                     ? jit_run_ms
                     : self_timed_ms;
      if (fixture_timer)
        s.fixture_timer_count++;
      if (test_env_truthy("NYTRIX_BENCH_TRACE_TIMING")) {
        const char *input = argv && argv[0] ? argv[0] : "<unknown>";
        for (int ai = 1; argv && argv[ai]; ++ai)
          if (strstr(argv[ai], ".ny"))
            input = argv[ai];
        fprintf(stderr,
                "[bench-timing] input=%s sample=%d rc=%d self_timed_ms=%.6g "
                "jit_run_ms=%.6g selected=%s\n",
                input, (int)s.count + 1, rc, self_timed_ms, jit_run_ms,
                fixture_timer
                    ? "fixture-marker"
                    : (prefer_jit_timer && jit_run_ms > 0.0
                           ? "ny--time"
                           : (self_timed_ms > 0.0 ? "self-timed" : "none")));
      }
      if (v <= 0.0 && jit_run_ms > 0.0) {
        v = jit_run_ms;
        s.timer_fallback_count++;
      }
      if (prefer_jit_timer && !fixture_timer && jit_run_ms <= 0.0 &&
          self_timed_ms > 0.0)
        s.timer_fallback_count++;
      if (v > 0.0 && s.count < 64)
        s.values[s.count++] = v;
      double compile_ms = bench_parse_phase_ms(out, "Total time:");
      /*
       * Total time includes JIT execution and teardown. Do not publish a
       * long-running kernel as a compiler-time regression.
       */
      if (compile_ms >= 0.0 && jit_run_ms >= 0.0)
        compile_ms -= jit_run_ms;
      if (compile_ms >= 0.0 && s.compile_count < 64)
        s.compile_values[s.compile_count++] = compile_ms;
      double link_ms = bench_parse_phase_ms(out, "Link:");
      if (link_ms >= 0.0 && s.link_count < 64)
        s.link_values[s.link_count++] = link_ms;
      double jit_compile_ms = bench_parse_phase_ms(out, "JIT Compile:");
      if (jit_compile_ms >= 0.0 && s.jit_compile_count < 64)
        s.jit_compile_values[s.jit_compile_count++] = jit_compile_ms;
      if (jit_run_ms >= 0.0 && s.jit_run_count < 64)
        s.jit_run_values[s.jit_run_count++] = jit_run_ms;
      double opt_ms = bench_parse_phase_ms(out, "Optimization:");
      if (opt_ms >= 0.0 && s.opt_count < 64)
        s.opt_values[s.opt_count++] = opt_ms;
    }
    free(out);
  }
  s.wall_ms = now_ms() - wall_start;
  bench_finalize_samples(&s);
  if (s.compile_count > 0)
    s.compile_ms = bench_median(s.compile_values, (int)s.compile_count);
  if (s.link_count > 0)
    s.link_ms = bench_median(s.link_values, (int)s.link_count);
  if (s.jit_compile_count > 0)
    s.jit_compile_ms =
        bench_median(s.jit_compile_values, (int)s.jit_compile_count);
  if (s.jit_run_count > 0)
    s.jit_run_ms = bench_median(s.jit_run_values, (int)s.jit_run_count);
  if (s.opt_count > 0)
    s.opt_ms = bench_median(s.opt_values, (int)s.opt_count);
  return s;
}

/*
 * Per-side measurement
 */

/*
 * Detect the host C compiler behind `NYTRIX_BENCH_CC` (or `CC`), defaulting to
 * a coherent `cc -O{N}` invocation.
 */
static const char *bench_cc(void) {
  const char *cc = getenv("NYTRIX_BENCH_CC");
  if (!cc || !*cc)
    cc = getenv("CC");
  if (!cc || !*cc)
    cc = "cc";
  return cc;
}

static const char *bench_opt_for(const char *opt_level) {
  static char buf[16];
  if (opt_level && *opt_level) {
    if (opt_level[0] == '-')
      return opt_level;
    if (opt_level[0] >= '0' && opt_level[0] <= '3' && opt_level[1] == '\0') {
      snprintf(buf, sizeof(buf), "-O%c", opt_level[0]);
      return buf;
    }
    if (opt_level[0] == '0' && opt_level[1] >= '0' && opt_level[1] <= '3' &&
        opt_level[2] == '\0') {
      snprintf(buf, sizeof(buf), "-O%c", opt_level[1]);
      return buf;
    }
    if ((opt_level[0] == 'O' || opt_level[0] == 'o') && opt_level[1] >= '0' &&
        opt_level[1] <= '3' && opt_level[2] == '\0') {
      snprintf(buf, sizeof(buf), "-O%c", opt_level[1]);
      return buf;
    }
    snprintf(buf, sizeof(buf), "-%s", opt_level);
    return buf;
  }
  return "-O3";
}

/*
 * Extract a `source c` block from an .nshape fixture.  Returns a heap-owning
 * NUL-terminated copy, or NULL when the block is absent.
 */
static char *bench_c_block(const char *shape_path) {
  return shape_source_block(shape_path, "c");
}

/*
 * Extract the `source ny` block and materialize it via the shared helper.
 */
static char *bench_materialize_ny(const char *shape_path) {
#ifndef _WIN32
  char *source = shape_source_block(shape_path, "ny");
  if (source && !strstr(source, "use \"./") && !strstr(source, "use './") &&
      !strstr(source, "use \"../") && !strstr(source, "use '../") &&
      !strstr(source, "module ")) {
    uint64_t key =
        fnv1a_update(UINT64_C(14695981039346656037), source, strlen(source));
    key = fnv1a_update(key, shape_path, strlen(shape_path));
    char dir[PATH_MAX], path[PATH_MAX], temporary[PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/bench-src",
                     nyt_default_cache_root_dir());
    if (n > 0 && (size_t)n < sizeof(dir)) {
      ny_ensure_dir_recursive(dir);
      n = snprintf(path, sizeof(path), "%s/%016" PRIx64 ".ny", dir, key);
      if (n > 0 && (size_t)n < sizeof(path)) {
        char *existing = read_whole_file(path);
        bool same = existing && strcmp(existing, source) == 0;
        free(existing);
        if (same) {
          free(source);
          return strdup(path);
        }
        n = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
        if (n > 0 && (size_t)n < sizeof(temporary)) {
          int fd = mkstemp(temporary);
          if (fd >= 0) {
            FILE *out = fdopen(fd, "wb");
            bool ok =
                out && fwrite(source, 1, strlen(source), out) == strlen(source);
            if (out) {
              if (fclose(out) != 0)
                ok = false;
            } else {
              close(fd);
            }
            if (ok && rename(temporary, path) == 0) {
              free(source);
              return strdup(path);
            }
            remove(temporary);
          }
        }
      }
    }
  }
  free(source);
#endif
  return materialize_shape_ny_source(shape_path);
}

static void bench_remove_materialized(const char *path) {
  /*
   * Shared immutable source paths keep #line identity and executable cache
   * keys stable across runs. Temporary multi-module trees remain disposable.
   */
  if (path && !strstr(path, "/bench-src/"))
    shape_remove_materialized(path);
}

static int bench_find_cc(char *out, size_t cap) {
  const char *cc = bench_cc();
  if (test_command_available(cc)) {
    nyt_path_copy(out, cap, cc);
    return 1;
  }
  if (ny_access(cc, X_OK) == 0) {
    nyt_path_copy(out, cap, cc);
    return 1;
  }
  return 0;
}

typedef struct {
  char os[32];
  char arch[32];
  char cpu_model[256];
  char target[64];
  char target_features[1024];
  char compiler_binary[PATH_MAX];
  char compiler_binary_fnv64[32];
  char c_compiler[PATH_MAX];
  char c_compiler_version[256];
  char c_flags[512];
  char compiler_revision[128];
  char opt_level[16];
  char profile[32];
  char tier[32];
  char engine[32];
  char cache[24];
  int samples;
  int warmups;
  int fixture_workers;
  int route_workers;
  int runtime_counters_enabled;
} BenchEnvironment;

static void bench_compiler_binary_fingerprint(const char *path, char *dst,
                                              size_t cap) {
  if (!dst || cap == 0)
    return;
  snprintf(dst, cap, "unavailable");
  if (!path || !*path)
    return;
  FILE *f = fopen(path, "rb");
  if (!f)
    return;
  uint64_t hash = UINT64_C(14695981039346656037);
  unsigned char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) != 0) {
    for (size_t i = 0; i < n; ++i) {
      hash ^= buf[i];
      hash *= UINT64_C(1099511628211);
    }
  }
  bool read_ok = !ferror(f);
  fclose(f);
  if (read_ok)
    snprintf(dst, cap, "fnv1a64:%016" PRIx64, hash);
}

static void bench_csv_string(FILE *f, const char *s) {
  fputc('"', f);
  for (const char *p = s ? s : ""; *p; ++p) {
    if (*p == '"')
      fputc('"', f);
    fputc(*p, f);
  }
  fputc('"', f);
}

static void bench_capture_first_line(char *const argv[], char *dst,
                                     size_t cap) {
  if (!dst || cap == 0)
    return;
  snprintf(dst, cap, "unknown");
  char tmp[PATH_MAX];
  if (make_test_capture_path(tmp, sizeof(tmp), "bench-meta") < 0)
    return;
  int rc = bench_run_capture(argv, 10, tmp);
  char *out = read_small_file(tmp);
  remove(tmp);
  if (rc != 0 || !out) {
    free(out);
    return;
  }
  char *end = strpbrk(out, "\r\n");
  if (end)
    *end = '\0';
  trim_inplace(out);
  if (*out)
    snprintf(dst, cap, "%s", out);
  free(out);
}

static void bench_target_features(char *dst, size_t cap) {
  if (!dst || cap == 0)
    return;
  snprintf(dst, cap, "unknown");
  const char *forced = getenv("NYTRIX_TARGET_FEATURES");
  if (forced && *forced) {
    snprintf(dst, cap, "%s", forced);
    return;
  }
#ifdef __APPLE__
  size_t n = cap;
  if (sysctlbyname("machdep.cpu.features", dst, &n, NULL, 0) == 0 && *dst)
    return;
#elif defined(__linux__)
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "flags", 5) != 0 && strncmp(line, "Features", 8) != 0)
      continue;
    char *colon = strchr(line, ':');
    if (!colon)
      continue;
    colon++;
    trim_inplace(colon);
    if (*colon)
      snprintf(dst, cap, "%s", colon);
    break;
  }
  fclose(f);
#elif defined(_WIN32)
  const char *id = getenv("PROCESSOR_IDENTIFIER");
  if (id && *id)
    snprintf(dst, cap, "%s", id);
#endif
}

static void bench_collect_environment(const char *root, const char *bin,
                                      const char *target, const char *opt_level,
                                      const char *tier, const char *engine,
                                      int samples, int warmups,
                                      const char *compile_profile,
                                      BenchEnvironment *env) {
  if (!env)
    return;
  memset(env, 0, sizeof(*env));
  snprintf(env->os, sizeof(env->os), "%s", host_os_name());
  snprintf(env->arch, sizeof(env->arch), "%s", host_arch_name());
  host_cpu_name(env->cpu_model, sizeof(env->cpu_model));
  snprintf(env->target, sizeof(env->target), "%s",
           target && *target ? target : "default");
  bench_target_features(env->target_features, sizeof(env->target_features));
  snprintf(env->compiler_binary, sizeof(env->compiler_binary), "%s",
           bin && *bin ? bin : "unknown");
  bench_compiler_binary_fingerprint(bin, env->compiler_binary_fnv64,
                                    sizeof(env->compiler_binary_fnv64));
  snprintf(env->opt_level, sizeof(env->opt_level), "%s",
           opt_level && *opt_level ? opt_level : "default");
  snprintf(env->profile, sizeof(env->profile), "%s",
           compile_profile && *compile_profile ? compile_profile : "default");
  snprintf(env->tier, sizeof(env->tier), "%s",
           tier && *tier ? tier : "default");
  snprintf(env->engine, sizeof(env->engine), "%s",
           engine && *engine ? engine : "aot");
  snprintf(env->cache, sizeof(env->cache), "%s", bench_cache_state());
  env->samples = samples;
  env->warmups = warmups;

  char cc[PATH_MAX];
  if (bench_find_cc(cc, sizeof(cc))) {
    snprintf(env->c_compiler, sizeof(env->c_compiler), "%s", cc);
    char *cc_argv[] = {cc, "--version", NULL};
    bench_capture_first_line(cc_argv, env->c_compiler_version,
                             sizeof(env->c_compiler_version));
  } else {
    snprintf(env->c_compiler, sizeof(env->c_compiler), "unknown");
    snprintf(env->c_compiler_version, sizeof(env->c_compiler_version),
             "unknown");
  }
  const char *extra = getenv("CFLAGS");
  snprintf(env->c_flags, sizeof(env->c_flags), "%s -march=native%s%s",
           bench_opt_for(opt_level), extra && *extra ? " " : "",
           extra && *extra ? extra : "");

  const char *rev = getenv("NYTRIX_REVISION");
  if (!rev || !*rev)
    rev = getenv("GIT_COMMIT");
  if (rev && *rev) {
    snprintf(env->compiler_revision, sizeof(env->compiler_revision), "%s", rev);
  } else if (root && *root) {
    char *git_argv[] = {"git", "-C", (char *)root, "rev-parse", "HEAD", NULL};
    bench_capture_first_line(git_argv, env->compiler_revision,
                             sizeof(env->compiler_revision));
  } else {
    snprintf(env->compiler_revision, sizeof(env->compiler_revision), "unknown");
  }
}

/*
 * Compile `c_source` into `exe_path` with the host cc at `opt_level`, plus any
 * per-fixture `cflags` and `ldflags` tokens read from shape metadata so C
 * references can supply include/compiler options and ordered link libraries.
 */
static unsigned long bench_next_temp_id(void) {
#ifdef _WIN32
  static volatile LONG sequence = 0;
  return (unsigned long)InterlockedIncrement(&sequence);
#else
  static volatile unsigned long sequence = 0;
  return __sync_add_and_fetch(&sequence, 1);
#endif
}

static char *bench_wrap_c_source(const char *c_source) {
  if (!c_source)
    return NULL;
  const char *main_def = strstr(c_source, "int main");
  const char *open = main_def ? strchr(main_def, '(') : NULL;
  const char *close = open ? strchr(open + 1, ')') : NULL;
  if (!close)
    return NULL;
  bool main_takes_args = false;
  for (const char *p = open + 1; p < close; ++p)
    if (!isspace((unsigned char)*p))
      main_takes_args = true;
  if (main_takes_args) {
    size_t arg_len = (size_t)(close - open - 1);
    if (arg_len >= 128)
      return NULL;
    char args[128];
    memcpy(args, open + 1, arg_len);
    args[arg_len] = '\0';
    trim_inplace(args);
    if (strcmp(args, "void") == 0)
      main_takes_args = false;
    else if (!strstr(args, "argc") || !strstr(args, "argv"))
      return NULL;
  }

  static const char prefix[] = "#define main ny_bench_program_main\n";
  static const char suffix_before_call[] =
      "\n#undef main\n#include <stdint.h>\n#include <stdio.h>\n"
      "#ifdef _WIN32\n"
      "#include <windows.h>\n"
      "static int64_t ny_bench_clock_ns(void) { LARGE_INTEGER c, f; "
      "QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); "
      "return (int64_t)((1000000000.0 * (double)c.QuadPart) / "
      "(double)f.QuadPart); }\n"
      "#else\n"
      "#include <time.h>\n"
      "static int64_t ny_bench_clock_ns(void) { struct timespec t; "
      "clock_gettime(CLOCK_MONOTONIC, &t); "
      "return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }\n"
      "#endif\n"
      "int main(int argc, char **argv) {\n"
      "  int64_t start = ny_bench_clock_ns();\n"
      "  int result = ny_bench_program_main";
  static const char call_args[] = "(argc, argv);\n";
  static const char call_no_args[] = "();\n";
  static const char suffix_after_call[] =
      "  int64_t elapsed = ny_bench_clock_ns() - start;\n"
      "  printf(\"bench_process_elapsed_ns=%lld\\n\", (long long)elapsed);\n"
      "  return result;\n"
      "}\n";
  const char *call = main_takes_args ? call_args : call_no_args;
  size_t source_len = strlen(c_source);
  size_t total = sizeof(prefix) - 1 + source_len + sizeof(suffix_before_call) -
                 1 + strlen(call) + sizeof(suffix_after_call);
  if (total < source_len)
    return NULL;
  char *wrapped = (char *)malloc(total);
  if (!wrapped)
    return NULL;
  size_t offset = 0;
  memcpy(wrapped + offset, prefix, sizeof(prefix) - 1);
  offset += sizeof(prefix) - 1;
  memcpy(wrapped + offset, c_source, source_len);
  offset += source_len;
  memcpy(wrapped + offset, suffix_before_call, sizeof(suffix_before_call) - 1);
  offset += sizeof(suffix_before_call) - 1;
  memcpy(wrapped + offset, call, strlen(call));
  offset += strlen(call);
  memcpy(wrapped + offset, suffix_after_call, sizeof(suffix_after_call));
  return wrapped;
}

static int bench_compile_c(const char *c_source, const char *exe_path,
                           const char *opt_level, const char *cflags,
                           const char *ldflags) {
  char *wrapped_source = bench_wrap_c_source(c_source);
  if (!wrapped_source)
    return 0;
  char src_tmp[PATH_MAX];
  snprintf(src_tmp, sizeof(src_tmp), "%s/ny-benchc-%ld-%ld-%lu.c",
           nyt_temp_dir(), (long)getpid(), (long)now_ms(),
           bench_next_temp_id());
  FILE *f = fopen(src_tmp, "wb");
  if (!f) {
    free(wrapped_source);
    return 0;
  }
  size_t wrapped_len = strlen(wrapped_source);
  int write_ok = fwrite(wrapped_source, 1, wrapped_len, f) == wrapped_len;
  int close_ok = fclose(f) == 0;
  free(wrapped_source);
  if (!write_ok || !close_ok) {
    remove(src_tmp);
    return 0;
  }
  char cc[PATH_MAX];
  if (!bench_find_cc(cc, sizeof(cc))) {
    remove(src_tmp);
    return 0;
  }

  char *cc_argv[32];
  int k = 0;
  cc_argv[k++] = (char *)cc;
  cc_argv[k++] = (char *)bench_opt_for(opt_level);
  cc_argv[k++] = "-march=native";
  if (cflags && *cflags) {
    char cflags_buf[512];
    if (strlen(cflags) >= sizeof(cflags_buf)) {
      remove(src_tmp);
      return 0;
    }
    snprintf(cflags_buf, sizeof(cflags_buf), "%s", cflags);
    trim_inplace(cflags_buf);
    char *flagv[12];
    int flagc = split_words(cflags_buf, flagv, 12);
    for (int i = 0; i < flagc && k < 20; i++)
      cc_argv[k++] = flagv[i];
  }
  cc_argv[k++] = (char *)src_tmp;
  if (ldflags && *ldflags) {
    char ldflags_buf[512];
    if (strlen(ldflags) >= sizeof(ldflags_buf)) {
      remove(src_tmp);
      return 0;
    }
    snprintf(ldflags_buf, sizeof(ldflags_buf), "%s", ldflags);
    trim_inplace(ldflags_buf);
    char *flagv[12];
    int flagc = split_words(ldflags_buf, flagv, 12);
    for (int i = 0; i < flagc && k < 28; i++)
      cc_argv[k++] = flagv[i];
  }
  cc_argv[k++] = "-o";
  cc_argv[k++] = (char *)exe_path;
  cc_argv[k++] = "-lm";
  cc_argv[k] = NULL;
  int rc = bench_run_capture(cc_argv, NY_BENCH_TIMEOUT_SEC, NULL);
  remove(src_tmp);
  return rc == 0;
}

static int bench_parse_size_value(const char *text, const char *key,
                                  size_t *out) {
  if (!text || !key || !*key || !out)
    return 0;
  const char *p = strstr(text, key);
  if (!p)
    return 0;
  p += strlen(key);
  if (!isdigit((unsigned char)*p))
    return 0;
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(p, &end, 10);
  if (errno != 0 || end == p || value > (unsigned long long)SIZE_MAX)
    return 0;
  *out = (size_t)value;
  return 1;
}

/*
 * Parse key=value from one named report line so repeated field names such as
 * spilled=/reloads= can be attributed to the intended metric family.
 */
static int bench_parse_report_size(const char *text, const char *line_prefix,
                                   const char *key, size_t *out) {
  if (!text || !line_prefix || !*line_prefix || !key || !*key || !out)
    return 0;
  const char *line = text;
  size_t prefix_len = strlen(line_prefix);
  while (line && *line) {
    const char *next = strchr(line, '\n');
    size_t line_len = next ? (size_t)(next - line) : strlen(line);
    if (line_len >= prefix_len && strncmp(line, line_prefix, prefix_len) == 0) {
      const char *p = line + prefix_len;
      const char *end_line = line + line_len;
      size_t key_len = strlen(key);
      while (p < end_line) {
        const char *hit = strstr(p, key);
        if (!hit || hit >= end_line || hit + key_len > end_line)
          break;
        if ((hit == line || isspace((unsigned char)hit[-1])) &&
            hit + key_len < end_line && isdigit((unsigned char)hit[key_len])) {
          errno = 0;
          char *end = NULL;
          unsigned long long value = strtoull(hit + key_len, &end, 10);
          if (errno == 0 && end != hit + key_len && end <= end_line &&
              value <= (unsigned long long)SIZE_MAX) {
            *out = (size_t)value;
            return 1;
          }
        }
        p = hit + key_len;
      }
      return 0;
    }
    line = next ? next + 1 : NULL;
  }
  return 0;
}

static int bench_runtime_counters_requested(void) {
  const char *v = getenv("NYTRIX_BENCH_RUNTIME_COUNTERS");
  if (!v || !*v)
    return 0;
  return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 &&
         strcmp(v, "off") != 0;
}

/*
 * Counter-only executions must agree with the measured kernel before their
 * telemetry is attributed to it. Missing checksums are invalid when timing
 * samples established one.
 */
static bool bench_counter_checksum_matches(const char *out,
                                           bench_stats_t *stats) {
  if (!stats->checksum_seen)
    return true;
  char checksum[sizeof(stats->checksum)];
  if (!bench_parse_marker(out, "checksum=", checksum, sizeof(checksum)) ||
      strcmp(checksum, stats->checksum) != 0) {
    stats->checksum_consistent = 0;
    return false;
  }
  return true;
}

static void bench_measure_runtime_counters(const char *exe_path,
                                           int timeout_sec,
                                           bench_stats_t *stats) {
  if (!exe_path || !stats || !bench_runtime_counters_requested())
    return;
  char capture_path[PATH_MAX];
  if (make_test_capture_path(capture_path, sizeof(capture_path),
                             "bench-runtime-counters") < 0)
    return;
  const char *old_env = getenv("NYTRIX_REPORT_RUNTIME_COUNTERS");
  char *saved_env = old_env ? strdup(old_env) : NULL;
  ny_setenv("NYTRIX_REPORT_RUNTIME_COUNTERS", "1", 1);
  char *argv[] = {(char *)exe_path, NULL};
  int rc = bench_run_capture(argv, timeout_sec, capture_path);
  if (saved_env) {
    ny_setenv("NYTRIX_REPORT_RUNTIME_COUNTERS", saved_env, 1);
    free(saved_env);
  } else {
    ny_unsetenv("NYTRIX_REPORT_RUNTIME_COUNTERS");
  }
  char *out = read_small_file(capture_path);
  remove(capture_path);
  if (rc != 0 || !out || !bench_counter_checksum_matches(out, stats)) {
    free(out);
    return;
  }
  size_t allocs = 0, reallocs = 0;
  int seen =
      bench_parse_report_size(out, "ny_runtime_counters ", "alloc=", &allocs) |
      bench_parse_report_size(out, "ny_runtime_counters ",
                              "realloc=", &reallocs);
  if (seen) {
    stats->runtime_alloc_count = allocs;
    stats->runtime_realloc_count = reallocs;
    stats->runtime_counters_seen = 1;
  }
  free(out);
}

static int bench_hw_counters_requested(void) {
  const char *v = getenv("NYTRIX_BENCH_HW_COUNTERS");
  if (!v || !*v)
    return 0;
  return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 &&
         strcmp(v, "off") != 0;
}

static int bench_parse_perf_counter(const char *text, const char *event,
                                    uint64_t *out) {
  if (!text || !event || !*event || !out)
    return 0;
  const char *line = text;
  while (line && *line) {
    const char *next = strchr(line, '\n');
    size_t line_len = next ? (size_t)(next - line) : strlen(line);
    /*
     * perf may qualify an event (cycles:u, instructions:k). Match its
     * name and accept the modifier separator as well as the CSV comma.
     */
    char event_name[128];
    size_t event_len = strlen(event);
    if (event_len < 2 || event_len >= sizeof(event_name))
      return 0;
    memcpy(event_name, event, event_len - 1);
    event_name[event_len - 1] = '\0';
    const char *hit = strstr(line, event_name);
    if (hit && hit + event_len - 1 < line + line_len &&
        (hit[event_len - 1] == ',' || hit[event_len - 1] == ':')) {
      const char *p = line;
      while (p < line + line_len && isspace((unsigned char)*p))
        ++p;
      if (p < line + line_len && isdigit((unsigned char)*p)) {
        errno = 0;
        char *end = NULL;
        unsigned long long value = strtoull(p, &end, 10);
        if (errno == 0 && end != p) {
          *out = (uint64_t)value;
          return 1;
        }
      }
    }
    line = next ? next + 1 : NULL;
  }
  return 0;
}

static void bench_measure_hw_counters(const char *exe_path, int timeout_sec,
                                      bench_stats_t *stats) {
#if defined(__linux__)
  if (!exe_path || !stats || !bench_hw_counters_requested() ||
      !test_command_available("perf"))
    return;
  char capture_path[PATH_MAX];
  if (make_test_capture_path(capture_path, sizeof(capture_path),
                             "bench-perf-stat") < 0)
    return;
  char *argv[] = {"perf",
                  "stat",
                  "--no-big-num",
                  "-x,",
                  "-e",
                  "cycles,instructions,branches,branch-misses,cache-misses",
                  "--",
                  (char *)exe_path,
                  NULL};
  int rc = bench_run_capture(argv, timeout_sec, capture_path);
  char *out = read_small_file(capture_path);
  remove(capture_path);
  if (rc != 0 || !out || !bench_counter_checksum_matches(out, stats)) {
    free(out);
    return;
  }
  int seen = 0;
  seen |= bench_parse_perf_counter(out, ",cycles,", &stats->hw_cycles);
  seen |=
      bench_parse_perf_counter(out, ",instructions,", &stats->hw_instructions);
  seen |= bench_parse_perf_counter(out, ",branches,", &stats->hw_branches);
  seen |= bench_parse_perf_counter(out, ",branch-misses,",
                                   &stats->hw_branch_misses);
  seen |=
      bench_parse_perf_counter(out, ",cache-misses,", &stats->hw_cache_misses);
  stats->hw_counters_seen = seen;
  free(out);
#else
  (void)exe_path;
  (void)timeout_sec;
  (void)stats;
#endif
}

/*
 * One report-only compile after timing samples records native static-island
 * facts and, when applicable, actual machine-code bytes. It does not
 * contaminate the measured compile/runtime samples.
 */
static void bench_measure_ny_code_size(const char *bin, const char *ny_path,
                                       const char *opt_level, const char *tier,
                                       const char *engine, const char *target,
                                       bool force_native_report,
                                       int timeout_sec, bench_stats_t *stats) {
  if (!bin || !ny_path || !stats)
    return;
  bool native_report =
      engine && (strcmp(engine, "aot") == 0 || strcmp(engine, "native") == 0);
  /*
   * Counter collectors execute the output. Native-only -o emits a relocatable
   * object, so use the existing linked native-backend AOT route for this
   * optional report compile. Timed samples remain unchanged.
   */
  bool executable_report =
      bench_hw_counters_requested() || bench_runtime_counters_requested();
  bool llvm_counter_report =
      target && strcmp(target, "llvm") == 0 && bench_hw_counters_requested();
  if (!native_report && !force_native_report && !llvm_counter_report)
    return;

  char report_path[PATH_MAX];
  char output_path[PATH_MAX];
  char capture_path[PATH_MAX];
  if (make_test_capture_path(report_path, sizeof(report_path),
                             "bench-size-report") < 0 ||
      make_test_capture_path(output_path, sizeof(output_path),
                             "bench-size-bin") < 0 ||
      make_test_capture_path(capture_path, sizeof(capture_path),
                             "bench-size-log") < 0)
    return;
  remove(report_path);
  remove(output_path);

  char report_arg[PATH_MAX + 32];
  snprintf(report_arg, sizeof(report_arg), "--native-tier-report=%s",
           report_path);
  bool tier_report_supported = !target || strcmp(target, "llvm") != 0;
  char *argv[28];
  int k = 0;
  argv[k++] = (char *)bin;
  if (tier && *tier && strcmp(tier, "opt") != 0)
    argv[k++] = (char *)tier;
  if (opt_level && *opt_level)
    argv[k++] = (char *)bench_opt_for(opt_level);
  if (engine && strcmp(engine, "native") == 0) {
    if (!executable_report)
      argv[k++] = "--native-only";
    if (target && *target) {
      argv[k++] = "--native-backend";
      argv[k++] = (char *)target;
    }
  } else if (target && *target) {
    argv[k++] = "--native-backend";
    argv[k++] = (char *)target;
  }
  if (bench_core_bc_path && *bench_core_bc_path) {
    argv[k++] = "--core-bc";
    argv[k++] = (char *)bench_core_bc_path;
  }
  if (tier_report_supported)
    argv[k++] = report_arg;
  argv[k++] = "-o";
  argv[k++] = output_path;
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;

  size_t peak_rss_kb = 0;
  int rc =
      bench_run_capture_usage(argv, timeout_sec, capture_path, &peak_rss_kb);
  if (peak_rss_kb > 0) {
    stats->peak_compiler_rss_kb = peak_rss_kb;
    stats->peak_compiler_rss_seen = 1;
  }
  if (rc == 0) {
    char *report = read_small_file(report_path);
    size_t bytes = 0;
    if (report && bench_parse_size_value(report, "code_bytes=", &bytes)) {
      stats->code_bytes = bytes;
      stats->code_bytes_seen = 1;
    }
    if (report) {
      size_t spec_bytes = 0, spec_functions = 0, spec_max_bytes = 0;
      int spec_seen =
          bench_parse_report_size(report, "machine_lowering ",
                                  "specialization_code_bytes=", &spec_bytes) |
          bench_parse_report_size(
              report, "machine_lowering ",
              "specialization_code_functions=", &spec_functions) |
          bench_parse_report_size(
              report, "machine_lowering ",
              "specialization_max_function_bytes=", &spec_max_bytes);
      if (spec_seen) {
        stats->specialization_code_bytes = spec_bytes;
        stats->specialization_code_functions = spec_functions;
        stats->specialization_max_function_bytes = spec_max_bytes;
        stats->specialization_metrics_seen = 1;
      }
      size_t gpr_spills = 0, fpr_spills = 0, vec_spills = 0;
      size_t gpr_reloads = 0, fpr_reloads = 0, vec_reloads = 0;
      int facts_seen = 0;
#define BENCH_PARSE_FACT(field, key)                                           \
  do {                                                                         \
    size_t value = 0;                                                          \
    if (bench_parse_report_size(report, "facts ", key, &value)) {              \
      stats->field = value;                                                    \
      facts_seen = 1;                                                          \
    }                                                                          \
  } while (0)
      BENCH_PARSE_FACT(runtime_calls, "runtime_calls=");
      BENCH_PARSE_FACT(dynamic_ops, "dynamic_ops=");
      BENCH_PARSE_FACT(tag_checks, "tag_checks=");
      BENCH_PARSE_FACT(box_unbox, "box_unbox=");
      BENCH_PARSE_FACT(heap_allocations, "heap_allocations=");
      BENCH_PARSE_FACT(bounds_checks, "bounds_checks=");
      BENCH_PARSE_FACT(direct_calls, "direct_calls=");
      BENCH_PARSE_FACT(indirect_calls, "indirect_calls=");
      BENCH_PARSE_FACT(unknown_effects, "unknown_effects=");
      BENCH_PARSE_FACT(alias_unresolved, "alias_unresolved=");
      BENCH_PARSE_FACT(vector_attempted, "vector_attempted=");
      BENCH_PARSE_FACT(vector_rejected, "vector_rejected=");
      BENCH_PARSE_FACT(vectorized, "vectorized=");
#undef BENCH_PARSE_FACT
      int spill_seen = bench_parse_report_size(report, "mach_regalloc ",
                                               "spilled=", &gpr_spills) |
                       bench_parse_report_size(report, "mach_fpr_regalloc ",
                                               "spilled=", &fpr_spills) |
                       bench_parse_report_size(report, "mach_vector_regalloc ",
                                               "spilled=", &vec_spills);
      int reload_seen = bench_parse_report_size(report, "mach_regalloc ",
                                                "reloads=", &gpr_reloads) |
                        bench_parse_report_size(report, "mach_fpr_regalloc ",
                                                "reloads=", &fpr_reloads) |
                        bench_parse_report_size(report, "mach_vector_regalloc ",
                                                "reloads=", &vec_reloads);
      if (spill_seen)
        stats->spills = gpr_spills + fpr_spills + vec_spills;
      if (reload_seen)
        stats->reloads = gpr_reloads + fpr_reloads + vec_reloads;
      stats->static_metrics_seen = facts_seen || spill_seen || reload_seen;
    }
    free(report);
  }
  if (rc == 0) {
    bench_measure_runtime_counters(output_path, timeout_sec, stats);
    bench_measure_hw_counters(output_path, timeout_sec, stats);
  }
  remove(report_path);
  remove(output_path);
  remove(capture_path);
}

static bool bench_shape_has_static_budget(const char *shape_path) {
  static const char *const keys[] = {
      "max_ny_heap_allocations",
      "max_ny_runtime_calls",
      "max_ny_bounds_checks",
      "max_ny_indirect_calls",
      "max_ny_unknown_effects",
      "max_ny_alias_unresolved",
      "max_ny_spills",
      "max_ny_reloads",
      "max_ny_code_bytes",
      "max_ny_specialization_code_bytes",
      "max_ny_specialization_function_bytes",
      "max_ny_peak_compiler_rss_kb",
  };
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    char *value = shape_meta_string(shape_path, keys[i]);
    if (value) {
      free(value);
      return true;
    }
  }
  return false;
}

/*
 * Run the Nytrix side of one bench fixture using the compiler's runtime phase
 * timer, then capture checksum and compile/optimization timings.
 */
static bench_stats_t bench_measure_ny(const char *bin, const char *shape_path,
                                      const char *opt_level, const char *tier,
                                      const char *engine, const char *target,
                                      int warmup, int runs, int timeout_sec) {
  char *ny_path = bench_materialize_ny(shape_path);
  if (!ny_path)
    return (bench_stats_t){.checksum_consistent = 1, .last_rc = 127};
  char *argv[24];
  char jit_arg[32];
  int k = 0;
  argv[k++] = (char *)bin;
  if (tier && *tier && strcmp(tier, "opt") != 0)
    argv[k++] = (char *)tier;
  if (opt_level && *opt_level)
    argv[k++] = (char *)bench_opt_for(opt_level);
  bool native_jit = engine && strcmp(engine, "native") == 0;
  bool use_jit = engine && strcmp(engine, "aot") != 0 && !native_jit;
  if (native_jit) {
    argv[k++] = "--native-only";
    if (target && *target) {
      argv[k++] = "--native-backend";
      argv[k++] = (char *)target;
    }
  } else if (use_jit) {
    argv[k++] = "--jit";
    snprintf(jit_arg, sizeof(jit_arg), "--jit-engine=%s", engine);
    argv[k++] = jit_arg;
  } else if (target && *target) {
    argv[k++] = "--native-backend";
    argv[k++] = (char *)target;
  }
  if (bench_core_bc_path && *bench_core_bc_path) {
    argv[k++] = "--core-bc";
    argv[k++] = (char *)bench_core_bc_path;
  }
  argv[k++] = "-time";
  if (!use_jit)
    argv[k++] = "-run";
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;
  bench_stats_t s =
      bench_measure_self_timed(argv, timeout_sec, warmup, runs, 1);
  bool static_budget = bench_shape_has_static_budget(shape_path);
  if (static_budget || test_env_truthy("NYTRIX_BENCH_CODE_METRICS") ||
      bench_hw_counters_requested() || bench_runtime_counters_requested())
    bench_measure_ny_code_size(bin, ny_path, opt_level, tier, engine, target,
                               static_budget, timeout_sec, &s);
  if (getenv("NYTRIX_TEST_SHOW_MATERIALIZED") &&
      strcmp(getenv("NYTRIX_TEST_SHOW_MATERIALIZED"), "0") != 0) {
    fprintf(stderr, "[materialized] %s\n", ny_path);
  } else {
    bench_remove_materialized(ny_path);
  }
  free(ny_path);
  return s;
}

/*
 * Build the leading Nytrix compiler argv given bench settings
 * (opt/tier/backend). Appends the file path and trailing flags separately by
 * callers.
 */
static int bench_ny_argv(char *argv[], const char *bin, const char *opt_level,
                         const char *tier, const char *engine,
                         const char *target) {
  static char jit_arg[32];
  int k = 0;
  argv[k++] = (char *)bin;
  if (tier && *tier && strcmp(tier, "opt") != 0)
    argv[k++] = (char *)tier;
  if (opt_level && *opt_level)
    argv[k++] = (char *)bench_opt_for(opt_level);
  bool native_jit = engine && strcmp(engine, "native") == 0;
  if (native_jit) {
    argv[k++] = "--native-only";
    if (target && *target) {
      argv[k++] = "--native-backend";
      argv[k++] = (char *)target;
    }
  } else if (engine && strcmp(engine, "aot") != 0) {
    argv[k++] = "--jit";
    snprintf(jit_arg, sizeof(jit_arg), "--jit-engine=%s", engine);
    argv[k++] = jit_arg;
  } else if (target && *target) {
    argv[k++] = "--native-backend";
    argv[k++] = (char *)target;
  }
  if (bench_core_bc_path && *bench_core_bc_path) {
    argv[k++] = "--core-bc";
    argv[k++] = (char *)bench_core_bc_path;
  }
  return k;
}

/*
 * Run one diagnostic compile.  The bench (opt/tier/backend) and `extra` flags
 * precede the input file, since they are compiler options rather than program
 * arguments.  Captures stdout+stderr to one temp file.  Returns the joined
 * output as a heap string or NULL.
 */
static char *bench_diag_capture(const char *bin, const char *opt_level,
                                const char *tier, const char *engine,
                                const char *target, char *const extra[],
                                const char *ny_path) {
  char *argv[40];
  int k = bench_ny_argv(argv, bin, opt_level, tier, engine, target);
  /*
   * Opt-in source-focused tracing is limited to these extra diagnostic
   * compiles.
   */
  const char *bench_ny_verbose = getenv("NYTRIX_BENCH_NY_VERBOSE");
  if (bench_ny_verbose && !strcmp(bench_ny_verbose, "6"))
    argv[k++] = "-vvvvvv";
  for (int i = 0; extra && extra[i]; i++) {
    if (k < 38)
      argv[k++] = extra[i];
  }
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;
  char tmp[PATH_MAX];
  if (make_test_capture_path(tmp, sizeof(tmp), "benchdiag") < 0)
    return NULL;
  int rc = bench_run_capture(argv, NY_BENCH_TIMEOUT_SEC, tmp);
  char *out = read_whole_file(tmp);
  remove(tmp);
  (void)rc;
  return out;
}

static bool bench_test_asm_function(const char *line, size_t line_len,
                                    char *name, size_t name_cap) {
  static const char global_prefix[] = ".globl";
  if (!line || !name)
    return false;
  const char *end_line = line + line_len;
  while (line < end_line && (*line == ' ' || *line == '\t'))
    ++line;
  if ((size_t)(end_line - line) <= sizeof(global_prefix) - 1 ||
      memcmp(line, global_prefix, sizeof(global_prefix) - 1) != 0)
    return false;
  const char *start = line + sizeof(global_prefix) - 1;
  while (start < end_line && (*start == ' ' || *start == '\t'))
    ++start;
  const char *end = start;
  while (end < end_line && *end != ' ' && *end != '\t' && *end != '\r' &&
         *end != '\n' && *end != ',')
    ++end;
  size_t len = (size_t)(end - start);
  static const char symbol_prefix[] = "ny_fn_";
  static const char core_prefix[] = "ny_fn_core.";
  if (len <= sizeof(symbol_prefix) - 1 || len >= name_cap ||
      memcmp(start, symbol_prefix, sizeof(symbol_prefix) - 1) != 0 ||
      (len >= sizeof(core_prefix) - 1 &&
       memcmp(start, core_prefix, sizeof(core_prefix) - 1) == 0))
    return false;
  memcpy(name, start, len);
  name[len] = '\0';
  return true;
}

static bool bench_test_asm_size(const char *line, size_t line_len,
                                const char *name) {
  static const char size_prefix[] = ".size";
  const char *end_line = line + line_len;
  while (line < end_line && (*line == ' ' || *line == '\t'))
    ++line;
  if ((size_t)(end_line - line) <= sizeof(size_prefix) - 1 ||
      memcmp(line, size_prefix, sizeof(size_prefix) - 1) != 0)
    return false;
  const char *start = line + sizeof(size_prefix) - 1;
  while (start < end_line && (*start == ' ' || *start == '\t'))
    ++start;
  size_t name_len = name ? strlen(name) : 0;
  return name_len && (size_t)(end_line - start) > name_len &&
         memcmp(start, name, name_len) == 0 &&
         (start[name_len] == ',' || start[name_len] == ' ' ||
          start[name_len] == '\t');
}

static void bench_print_test_assembly(const char *assembly) {
  if (!assembly)
    return;
  size_t total_bytes = strlen(assembly);
  size_t test_bytes = 0;
  size_t function_count = 0;
  char active_name[256] = {0};
  bool in_test_function = false;
  for (const char *line = assembly; *line;) {
    const char *newline = strchr(line, '\n');
    size_t line_len = newline ? (size_t)(newline + 1 - line) : strlen(line);
    char name[sizeof(active_name)];
    if (!in_test_function &&
        bench_test_asm_function(line, line_len, name, sizeof(name))) {
      memcpy(active_name, name, strlen(name) + 1);
      in_test_function = true;
    }
    if (in_test_function) {
      test_bytes += line_len;
      if (bench_test_asm_size(line, line_len, active_name)) {
        ++function_count;
        in_test_function = false;
      }
    }
    line += line_len;
  }
  printf("[test assembly: %zu user function%s, %zu/%zu bytes]\n",
         function_count, function_count == 1 ? "" : "s", test_bytes,
         total_bytes);
  in_test_function = false;
  active_name[0] = '\0';
  for (const char *line = assembly; *line;) {
    const char *newline = strchr(line, '\n');
    size_t line_len = newline ? (size_t)(newline + 1 - line) : strlen(line);
    char name[sizeof(active_name)];
    if (!in_test_function &&
        bench_test_asm_function(line, line_len, name, sizeof(name))) {
      memcpy(active_name, name, strlen(name) + 1);
      in_test_function = true;
    }
    if (in_test_function) {
      fwrite(line, 1, line_len, stdout);
      if (bench_test_asm_size(line, line_len, active_name))
        in_test_function = false;
    }
    line += line_len;
  }
}

static bool bench_line_starts(const char *line, size_t len,
                              const char *prefix) {
  while (len && (*line == ' ' || *line == '\t')) {
    ++line;
    --len;
  }
  size_t prefix_len = strlen(prefix);
  return len >= prefix_len && memcmp(line, prefix, prefix_len) == 0;
}

static bool bench_line_contains(const char *line, size_t len,
                                const char *text) {
  size_t text_len = strlen(text);
  if (text_len > len)
    return false;
  for (size_t i = 0; i <= len - text_len; ++i)
    if (memcmp(line + i, text, text_len) == 0)
      return true;
  return false;
}

static size_t bench_line_hash(const char *line, size_t len) {
  size_t hash = (size_t)1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i)
    hash = (hash ^ (unsigned char)line[i]) * (size_t)1099511628211ULL;
  return hash;
}

static bool bench_line_core_function(const char *line, size_t len) {
  return bench_line_contains(line, len, "▶") &&
         bench_line_contains(line, len, "core.");
}

static void bench_print_compact_diagnostics(const char *output) {
  if (!output)
    return;
  struct bench_seen_line {
    const char *text;
    size_t len;
    size_t hash;
  };
  struct bench_seen_line seen[4096] = {{0}};
  bool in_core_function = false;
  bool skip_module_list = false;
  size_t scope_hash = 0;
  for (const char *line = output; *line;) {
    const char *newline = strchr(line, '\n');
    size_t len = newline ? (size_t)(newline + 1 - line) : strlen(line);
    bool starts_function = bench_line_starts(line, len, "▶ ");
    bool starts_core = bench_line_core_function(line, len);
    bool ends_function = bench_line_contains(line, len, "◀ ");
    bool hide = in_core_function;
    if (starts_core) {
      in_core_function = true;
      hide = true;
    } else if (starts_function) {
      scope_hash = bench_line_hash(line, len);
    }
    if (bench_line_starts(line, len, "› Modules (") ||
        bench_line_starts(line, len, "› Standard library:") ||
        bench_line_starts(line, len, "› Standard library ready:") ||
        bench_line_starts(line, len, "Core load:") ||
        bench_line_starts(line, len, "Read file:") ||
        bench_line_starts(line, len, "Scan imports:")) {
      skip_module_list = bench_line_starts(line, len, "› Modules (");
      hide = true;
    } else if (skip_module_list && len >= 2 && line[0] == ' ' &&
               line[1] == ' ') {
      hide = true;
    } else {
      skip_module_list = false;
    }
    if (bench_line_contains(line, len, "pure-call:") ||
        bench_line_contains(line, len, "sig=core."))
      hide = true;
    if (!hide) {
      size_t hash = bench_line_hash(line, len) ^ scope_hash;
      size_t slot = hash % (sizeof(seen) / sizeof(seen[0]));
      bool duplicate = false;
      for (size_t probe = 0; probe < sizeof(seen) / sizeof(seen[0]); ++probe) {
        struct bench_seen_line *entry =
            &seen[(slot + probe) % (sizeof(seen) / sizeof(seen[0]))];
        if (!entry->text) {
          entry->text = line;
          entry->len = len;
          entry->hash = hash;
          break;
        }
        if (entry->hash == hash && entry->len == len &&
            memcmp(entry->text, line, len) == 0) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate)
        fwrite(line, 1, len, stdout);
    }
    if (ends_function) {
      in_core_function = false;
      scope_hash = 0;
    }
    line += len;
  }
}

/*
 * Run and print a labeled diagnostic block for one fixture in bench mode.
 */
static void bench_diag(const char *shape, int show_ir, int show_asm,
                       int show_passes, int profile, int compare_llvm,
                       const char *bin, const char *opt_level, const char *tier,
                       const char *engine, const char *target, FILE *report) {
  char *ny_path = bench_materialize_ny(shape);
  if (!ny_path) {
    if (report)
      fprintf(report, "\n### Compiler artifacts unavailable\n\n"
                      "Could not materialize the fixture's Nytrix source.\n");
    return;
  }
  if (report) {
    char *ny_source = read_whole_file(ny_path);
    if (ny_source) {
      bench_report_block(report, "Extracted Ny source", "ny", ny_source);
      free(ny_source);
    }
  }
  if (profile) {
    char *extra[] = {"-prof", NULL};
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra,
                                   ny_path);
    if (out) {
      printf("\n[phase profile: %s]\n", shape);
      bench_print_compact_diagnostics(out);
      if (report)
        bench_report_block(report, "Compiler profile", "text", out);
      free(out);
    } else if (report) {
      fprintf(report, "\n### Profile output unavailable\n\n"
                      "Diagnostic compiler output could not be captured.\n");
    }
  }
  if (show_passes || show_ir) {
    /*
     * Pass statistics and IR dumps live on the native NYIR pipeline;
     * without --native-only the JIT/LLVM path skips them entirely and
     * only warns, so force the native backend for diagnostic compiles.
     */
    char *extra[6];
    int n = 0;
    const char *debug_filter = getenv("NYTRIX_BENCH_DEBUG_FILTER");
    char default_filter[] = "--dbg-filter=func:rt_main";
    char debug_filter_arg[1024];
    extra[n++] = "--native-only";
    extra[n++] = "--nyir-dump-stats";
    /*
     * Bench diagnostics should explain the timed entry function by default;
     * opt into a different function with NYTRIX_BENCH_DEBUG_FILTER.
     */
    if (debug_filter && debug_filter[0]) {
      if (strncmp(debug_filter, "--dbg-filter=", 13) == 0) {
        extra[n++] = (char *)debug_filter;
      } else {
        snprintf(debug_filter_arg, sizeof(debug_filter_arg), "--dbg-filter=%s",
                 debug_filter);
        extra[n++] = debug_filter_arg;
      }
    } else {
      extra[n++] = default_filter;
    }
    if (show_ir)
      extra[n++] = "--nyir-dump";
    if (show_passes)
      extra[n++] = "--nyir-pass-stats";
    extra[n] = NULL;
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra,
                                   ny_path);
    if (out) {
      printf("\n[NYIR/pass diagnostics: %s]\n", shape);
      bench_print_compact_diagnostics(out);
      if (report)
        bench_report_block(report, "NYIR passes and optimization diagnostics",
                           "text", out);
      free(out);
    } else if (report) {
      fprintf(report,
              "\n### NYIR passes and optimization diagnostics unavailable\n\n"
              "Diagnostic compiler output could not be captured.\n");
    }
  }
  if (show_asm) {
    char asm_tmp[PATH_MAX];
    char flag[PATH_MAX + 32];
    snprintf(asm_tmp, sizeof(asm_tmp), "%s/ny-bench-asm-%ld-%ld-%lu.s",
             nyt_temp_dir(), (long)getpid(), (long)now_ms(),
             bench_next_temp_id());
    snprintf(flag, sizeof(flag), "--emit-asm=%s", asm_tmp);
    char *extra[] = {flag, NULL};
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra,
                                   ny_path);
    char *asmout = read_whole_file(asm_tmp);
    if (asmout) {
      printf("\n[text emitter assembly (not the timed object): %s]\n", shape);
      bench_print_test_assembly(asmout);
      if (report)
        bench_report_block(report, "NYR assembly (text emitter)", "asm",
                           asmout);
      free(asmout);
      remove(asm_tmp);
    } else if (report) {
      fprintf(report,
              "\n### NYR assembly (text emitter) unavailable\n\n"
              "Compiler exit/output: `%s`\n",
              out && *out ? "see diagnostic output below" : "no output");
      if (out && *out)
        fprintf(report, "\n```text\n%s\n```\n", out);
    }
    free(out);

    /*
     * --emit-asm uses the text machine emitter. The native AOT benchmark
     * executes the object writer, whose register allocation and instruction
     * choices can differ. Preserve a disassembly of that exact native object
     * as the comparison artifact.
     */
    char obj_tmp[PATH_MAX];
    char objdump_tmp[PATH_MAX];
    snprintf(obj_tmp, sizeof(obj_tmp), "%s/ny-bench-native-%ld-%ld-%lu.o",
             nyt_temp_dir(), (long)getpid(), (long)now_ms(),
             bench_next_temp_id());
    snprintf(objdump_tmp, sizeof(objdump_tmp),
             "%s/ny-bench-native-%ld-%ld-%lu.dump", nyt_temp_dir(),
             (long)getpid(), (long)now_ms(), bench_next_temp_id());
    char *obj_extra[] = {"-emit-only", "-o", obj_tmp, NULL};
    out = bench_diag_capture(bin, opt_level, tier, "native", target, obj_extra,
                             ny_path);
    if (access(obj_tmp, R_OK) == 0) {
      /*
       * Include user helpers as well as main: the hottest loop often lives
       * in a helper, and the text emitter above has different allocation.
       */
      char *objdump_argv[] = {"objdump", "-dr", obj_tmp, NULL};
      int objdump_rc =
          bench_run_capture(objdump_argv, NY_BENCH_TIMEOUT_SEC, objdump_tmp);
      char *objdump = objdump_rc == 0 ? read_whole_file(objdump_tmp) : NULL;
      if (objdump) {
        printf("\n[native AOT object disassembly: %s]\n", shape);
        printf("  machine code · %s · %zu bytes\n", objdump_tmp,
               strlen(objdump));
        if (report)
          bench_report_block(report, "NYR native AOT object disassembly", "asm",
                             objdump);
        free(objdump);
      } else {
        printf("\n[native AOT object: %s · %s; objdump unavailable (rc=%d)]\n",
               shape, obj_tmp, objdump_rc);
      }
    } else {
      printf("\n[native AOT object unavailable: %s]\n", shape);
      if (report) {
        fprintf(report,
                "\n### NYR native AOT object disassembly unavailable\n\n"
                "Native object emission failed.\n");
        if (out && *out)
          fprintf(report, "\n```text\n%s\n```\n", out);
      }
      if (out && *out)
        printf("%s\n", out);
    }
    free(out);
  }
  /*
   * Preserve the matched LLVM compiler output beside the native NYIR/asm
   * diagnostics. This makes backend comparisons reproducible from the same
   * materialized source and optimization settings used by the benchmark.
   */
  if (compare_llvm && (show_ir || show_asm)) {
    char llvm_ir_tmp[PATH_MAX];
    char llvm_asm_tmp[PATH_MAX];
    char ir_flag[PATH_MAX + 32];
    char asm_flag[PATH_MAX + 32];
    long pid = (long)getpid();
    long stamp = (long)now_ms();
    snprintf(llvm_ir_tmp, sizeof(llvm_ir_tmp),
             "%s/ny-bench-llvm-ir-%ld-%ld-%lu.ll", nyt_temp_dir(), pid, stamp,
             bench_next_temp_id());
    snprintf(llvm_asm_tmp, sizeof(llvm_asm_tmp),
             "%s/ny-bench-llvm-asm-%ld-%ld-%lu.s", nyt_temp_dir(), pid, stamp,
             bench_next_temp_id());
    snprintf(ir_flag, sizeof(ir_flag), "--emit-ir=%s", llvm_ir_tmp);
    snprintf(asm_flag, sizeof(asm_flag), "--emit-asm=%s", llvm_asm_tmp);
    char *extra[] = {"-emit-only", ir_flag, show_asm ? asm_flag : NULL, NULL};
    char *out = bench_diag_capture(bin, opt_level, tier, "mcjit", "llvm", extra,
                                   ny_path);
    char *llvm_ir = show_ir ? read_whole_file(llvm_ir_tmp) : NULL;
    char *llvm_asm = show_asm ? read_whole_file(llvm_asm_tmp) : NULL;
    if (llvm_ir || llvm_asm) {
      printf("\n[LLVM reference artifacts: %s]\n", shape);
      if (llvm_ir)
        printf("  optimized LLVM IR · %s · %zu bytes\n", llvm_ir_tmp,
               strlen(llvm_ir));
      if (llvm_asm)
        printf("  target assembly  · %s · %zu bytes\n", llvm_asm_tmp,
               strlen(llvm_asm));
      if (report && llvm_ir)
        bench_report_block(report, "Optimized LLVM IR", "llvm", llvm_ir);
      if (report && llvm_asm)
        bench_report_block(report, "LLVM assembly", "asm", llvm_asm);
      if (report && show_ir && !llvm_ir)
        fprintf(report, "\n### Optimized LLVM IR unavailable\n\n"
                        "LLVM did not emit the requested IR file.\n");
      if (report && show_asm && !llvm_asm)
        fprintf(report, "\n### LLVM assembly unavailable\n\n"
                        "LLVM did not emit the requested assembly file.\n");
    } else {
      printf("\n[LLVM reference artifacts unavailable: %s]\n", shape);
      if (out && *out)
        printf("%s\n", out);
      if (report) {
        fprintf(report, "\n### LLVM IR and assembly unavailable\n\n"
                        "LLVM emitted neither requested artifact.\n");
        if (out && *out)
          fprintf(report, "\n```text\n%s\n```\n", out);
      }
    }
    free(llvm_ir);
    free(llvm_asm);
    free(out);
  }
  bench_remove_materialized(ny_path);
  free(ny_path);
}

static void bench_write_c_assembly(FILE *report, const char *shape,
                                   const char *opt_level) {
  if (!report)
    return;
  char *c_src = bench_c_block(shape);
  if (!c_src) {
    fprintf(report,
            "\n### C assembly\n\nUnavailable: fixture has no C source.\n");
    return;
  }
  char *wrapped = bench_wrap_c_source(c_src);
  free(c_src);
  if (!wrapped) {
    fprintf(report, "\n### C assembly\n\nUnavailable: could not build the "
                    "benchmark wrapper.\n");
    return;
  }
  char src_tmp[PATH_MAX], asm_tmp[PATH_MAX], capture_tmp[PATH_MAX];
  snprintf(src_tmp, sizeof(src_tmp), "%s/ny-bench-report-%ld-%lu.c",
           nyt_temp_dir(), (long)getpid(), bench_next_temp_id());
  snprintf(asm_tmp, sizeof(asm_tmp), "%s/ny-bench-report-%ld-%lu.s",
           nyt_temp_dir(), (long)getpid(), bench_next_temp_id());
  snprintf(capture_tmp, sizeof(capture_tmp), "%s/ny-bench-report-%ld-%lu.log",
           nyt_temp_dir(), (long)getpid(), bench_next_temp_id());
  FILE *src = fopen(src_tmp, "wb");
  size_t wrapped_len = strlen(wrapped);
  int source_ok = src && fwrite(wrapped, 1, wrapped_len, src) == wrapped_len;
  if (src && fclose(src) != 0)
    source_ok = 0;
  if (!source_ok) {
    if (src)
      remove(src_tmp);
    fprintf(report,
            "\n### C assembly\n\nUnavailable: could not write C source.\n");
    free(wrapped);
    return;
  }
  bench_report_block(report, "Generated C benchmark source", "c", wrapped);
  free(wrapped);
  char cc[PATH_MAX];
  int rc = 127;
  if (bench_find_cc(cc, sizeof(cc))) {
    char *argv[32];
    int argc = 0;
    argv[argc++] = cc;
    argv[argc++] = (char *)bench_opt_for(opt_level);
    argv[argc++] = "-march=native";
    argv[argc++] = "-S";
    argv[argc++] = "-fverbose-asm";
    char *cflags = shape_meta_string(shape, "cflags");
    if (cflags && *cflags) {
      char cflags_buf[512];
      if (strlen(cflags) >= sizeof(cflags_buf)) {
        rc = 2;
      } else {
        snprintf(cflags_buf, sizeof(cflags_buf), "%s", cflags);
        trim_inplace(cflags_buf);
        char *flagv[12];
        int flagc = split_words(cflags_buf, flagv, 12);
        for (int i = 0; i < flagc && argc < 24; i++)
          argv[argc++] = flagv[i];
      }
    }
    argv[argc++] = src_tmp;
    argv[argc++] = "-o";
    argv[argc++] = asm_tmp;
    argv[argc] = NULL;
    if (rc != 2)
      rc = bench_run_capture(argv, NY_BENCH_TIMEOUT_SEC, capture_tmp);
    free(cflags);
  }
  char *assembly = rc == 0 ? read_whole_file(asm_tmp) : NULL;
  if (assembly) {
    bench_report_block(report, "C assembly", "asm", assembly);
    free(assembly);
  } else {
    char *diagnostic = read_whole_file(capture_tmp);
    fprintf(report, "\n### C assembly\n\nUnavailable (compiler exit %d).\n",
            rc);
    if (diagnostic && *diagnostic)
      fprintf(report, "\n```text\n%s\n```\n", diagnostic);
    free(diagnostic);
  }
  remove(src_tmp);
  remove(asm_tmp);
  remove(capture_tmp);
}

/*
 * Run the C side of a c-vs-ny fixture: extract the C block, compile it with
 * the host cc, and collect its self-reported elapsed.
 */
static bench_stats_t bench_measure_c(const char *shape_path,
                                     const char *opt_level, int warmup,
                                     int runs, int timeout_sec) {
  char *c_src = bench_c_block(shape_path);
  if (!c_src) {
    /*
     * Audit A3: a non-oracle fixture without a C side has no C result to
     * compare against. Fail loudly instead of silently passing; oracle
     * fixtures (which validate against a reference implementation rather
     * than a C port) are exempt.
     */
    bool is_oracle = shape_path && strstr(shape_path, "oracle") != NULL;
    if (!is_oracle)
      return (bench_stats_t){
          .checksum_consistent = 1, .last_rc = 127, .failure_count = 1};
    return (bench_stats_t){.checksum_consistent = 1, .last_rc = 0};
  }
  char *cflags = shape_meta_string(shape_path, "cflags");
  char *ldflags = shape_meta_string(shape_path, "ldflags");
  char *optional_ldflags = shape_meta_string(shape_path, "optional_ldflags");
  if ((!ldflags || !*ldflags) && optional_ldflags && *optional_ldflags) {
    char *probe_source = shape_source_block(shape_path, "c_probe");
    if (!probe_source)
      probe_source = ny_strdup("int main(void) { return 0; }");
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/ny-bench-link-probe-%ld-%lu",
             nyt_temp_dir(), (long)getpid(), bench_next_temp_id());
    bool available =
        probe_source &&
        bench_compile_c(probe_source, probe, opt_level, NULL, optional_ldflags);
    free(probe_source);
    remove(probe);
    if (!available) {
      free(cflags);
      free(ldflags);
      free(optional_ldflags);
      free(c_src);
      /*
       * Audit A3: same missing-C-result rule as above. When the optional
       * libraries are unavailable the C side cannot build; non-oracle
       * fixtures fail instead of silently passing without comparison.
       */
      bool is_oracle = shape_path && strstr(shape_path, "oracle") != NULL;
      if (!is_oracle)
        return (bench_stats_t){
            .checksum_consistent = 1, .last_rc = 127, .failure_count = 1};
      return (bench_stats_t){.checksum_consistent = 1, .last_rc = 0};
    }
    ldflags = optional_ldflags;
    optional_ldflags = NULL;
  }
  char exe[PATH_MAX];
  snprintf(exe, sizeof(exe), "%s/ny-bench-c-exe-%ld-%ld-%lu", nyt_temp_dir(),
           (long)getpid(), (long)now_ms(), bench_next_temp_id());
  if (!bench_compile_c(c_src, exe, opt_level, cflags, ldflags)) {
    free(cflags);
    free(ldflags);
    free(optional_ldflags);
    free(c_src);
    remove(exe);
    return (bench_stats_t){
        .checksum_consistent = 1, .last_rc = 127, .failure_count = 1};
  }
  free(cflags);
  free(ldflags);
  free(optional_ldflags);
  free(c_src);
  char *argv[2];
  argv[0] = exe;
  argv[1] = NULL;
  bench_stats_t s =
      bench_measure_self_timed(argv, timeout_sec, warmup, runs, 0);
  remove(exe);
  return s;
}
typedef struct {
  double wall_ms;
  bench_stats_t ny;
  bench_stats_t llvm;
  bench_stats_t c;
} bench_worker_result_t;

typedef struct {
  const char *shape;
  char name[128];
  bench_worker_result_t result;
} BenchJob;

static bench_worker_result_t
bench_measure_shape(const char *bin, const char *shape, const char *opt_level,
                    const char *tier, const char *engine, const char *target,
                    int compare_llvm, int warmup, int runs, int timeout_sec);

typedef struct {
  int tiny;
  int compact;
  int name;
  int status;
  int compile;
  int native;
  int llvm;
  int c_host;
  int ratio;
} BenchUiLayout;

static BenchUiLayout bench_ui_layout(void) {
  int width = ny_ui_width();
  if (width < 82) {
    return (BenchUiLayout){
        .tiny = 1, .name = 12, .status = 6, .native = 9, .c_host = 8};
  }
  if (width < 106) {
    return (BenchUiLayout){.compact = 1,
                           .name = 16,
                           .status = 7,
                           .native = 9,
                           .llvm = 9,
                           .c_host = 9,
                           .ratio = 8};
  }
  return (BenchUiLayout){.name = 20,
                         .status = 8,
                         .compile = 7,
                         .native = 8,
                         .llvm = 8,
                         .c_host = 8,
                         .ratio = 11};
}

static size_t bench_disp_width(const char *s) {
  size_t w = 0;
  for (; s && *s; s++)
    if (((unsigned char)*s & 0xC0) != 0x80)
      w++;
  return w;
}

static void bench_clip(const char *s, int width, char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  if (!s)
    s = "";
  if (width < 1)
    width = 1;
  if ((int)bench_disp_width(s) <= width) {
    snprintf(out, cap, "%s", s);
    return;
  }
  if (width == 1) {
    snprintf(out, cap, "~");
    return;
  }
  size_t used = 0;
  int cells = 0;
  while (s[used] && cells < width - 1 && used + 1 < cap) {
    unsigned char ch = (unsigned char)s[used];
    size_t bytes = (ch & 0x80) == 0 ? 1
                                    : ((ch & 0xE0) == 0xC0   ? 2
                                       : (ch & 0xF0) == 0xE0 ? 3
                                                             : 4);
    if (used + bytes >= cap)
      break;
    memcpy(out + used, s + used, bytes);
    used += bytes;
    cells++;
  }
  if (used + 1 >= cap)
    used = cap - 2;
  out[used++] = '~';
  out[used] = '\0';
}

static void bench_ms_text(double value, int valid, char *out, size_t cap);

static int bench_live_pair_comparable(const bench_stats_t *a,
                                      const bench_stats_t *b) {
  return a && b && a->count > 0 && b->count > 0 && !a->failure_count &&
         !b->failure_count && !a->timer_fallback_count &&
         !b->timer_fallback_count && !a->unstable && !b->unstable &&
         a->checksum_seen && b->checksum_seen && a->checksum_consistent &&
         b->checksum_consistent && a->checksum[0] && b->checksum[0] &&
         strcmp(a->checksum, b->checksum) == 0 && a->median >= 0.001 &&
         b->median >= 0.001;
}

static void bench_ratio_text(char *out, size_t cap, double ratio) {
  if (ratio > 0.0 && ratio < 0.01)
    snprintf(out, cap, "<0.01x");
  else
    snprintf(out, cap, "%.2fx", ratio);
}

static void bench_geomean_text(char *out, size_t cap, double log_sum,
                               size_t count) {
  if (!count) {
    snprintf(out, cap, "—");
    return;
  }
  char ratio[32];
  bench_ratio_text(ratio, sizeof(ratio), exp(log_sum / (double)count));
  snprintf(out, cap, "%s (n=%zu)", ratio, count);
}

static const char *bench_engine_label(const char *engine) {
  if (!engine || !*engine)
    return "aot";
  return strcmp(engine, "native") == 0 ? "NYR" : engine;
}

static void bench_live_ratio(char *out, size_t cap, double numerator,
                             double denominator, int valid) {
  if (!valid || denominator <= 0.0) {
    snprintf(out, cap, "—");
    return;
  }
  double ratio = numerator / denominator;
  bench_ratio_text(out, cap, ratio);
}

static void bench_live_mark_fallback(char *text, size_t cap, int fallback) {
  if (!fallback || !text || !*text || strcmp(text, "—") == 0)
    return;
  char marked[32];
  snprintf(marked, sizeof(marked), "~%s", text);
  snprintf(text, cap, "%s", marked);
}

static void bench_live_cell(char *out, size_t cap, const char *text, int width,
                            const char *color) {
  if (!out || cap == 0)
    return;
  if (!text)
    text = "—";
  int pad = width - (int)bench_disp_width(text);
  if (pad < 0)
    pad = 0;
  snprintf(out, cap, "%*s%s%s%s", pad, "", nyt_clr(color), text,
           nyt_clr(NYT_RESET));
}

static const char *bench_live_ratio_color(const char *ratio) {
  if (!ratio || !*ratio || strcmp(ratio, "—") == 0)
    return NYT_GRAY;
  char *end = NULL;
  double value = strtod(ratio, &end);
  if (end == ratio)
    return NYT_GRAY;
  return value <= 1.0 ? NYT_GREEN : (value <= 2.0 ? NYT_YELLOW : NYT_RED);
}

static void bench_live_fixture_result(const char *name,
                                      const bench_worker_result_t *result) {
  char compile[32], native[32], llvm[32], c_host[32];
  char native_c[32], llvm_c[32], native_llvm[32];
  bench_ms_text(result->ny.compile_ms, result->ny.compile_count > 0, compile,
                sizeof(compile));
  bench_ms_text(result->ny.median, result->ny.count > 0, native,
                sizeof(native));
  bench_ms_text(result->llvm.median, result->llvm.count > 0, llvm,
                sizeof(llvm));
  bench_ms_text(result->c.median, result->c.count > 0, c_host, sizeof(c_host));
  bench_live_mark_fallback(native, sizeof(native),
                           result->ny.timer_fallback_count > 0);
  bench_live_mark_fallback(llvm, sizeof(llvm),
                           result->llvm.timer_fallback_count > 0);
  int nc_ok = bench_live_pair_comparable(&result->ny, &result->c);
  int lc_ok = bench_live_pair_comparable(&result->llvm, &result->c);
  int nl_ok = bench_live_pair_comparable(&result->ny, &result->llvm);
  bench_live_ratio(native_c, sizeof(native_c), result->ny.median,
                   result->c.median, nc_ok);
  bench_live_ratio(llvm_c, sizeof(llvm_c), result->llvm.median,
                   result->c.median, lc_ok);
  bench_live_ratio(native_llvm, sizeof(native_llvm), result->ny.median,
                   result->llvm.median, nl_ok);
  const char *status = "ok";
  if (result->ny.failure_count || result->llvm.failure_count ||
      result->c.failure_count)
    status = "FAIL";
  else if ((result->ny.checksum_seen && !result->ny.checksum_consistent) ||
           (result->llvm.checksum_seen && !result->llvm.checksum_consistent) ||
           (result->c.checksum_seen && !result->c.checksum_consistent) ||
           (result->ny.checksum_seen && result->c.checksum_seen &&
            strcmp(result->ny.checksum, result->c.checksum) != 0) ||
           (result->ny.checksum_seen && result->llvm.checksum_seen &&
            strcmp(result->ny.checksum, result->llvm.checksum) != 0))
    status = "MISMATCH";
  else if (result->ny.unstable || result->llvm.unstable || result->c.unstable)
    status = "NOISY";
  else if (!result->ny.count)
    status = "NO-TIME";
  else if (!result->c.count)
    status = "NY/LLVM";
  else if (!result->ny.checksum_seen || !result->c.checksum_seen)
    status = "NO-CHECK";
  else if (result->ny.timer_fallback_count || result->llvm.timer_fallback_count)
    status = "TIMER-FB";
  const char *status_color =
      (strcmp(status, "ok") == 0) ? NYT_GREEN
      : (strcmp(status, "FAIL") == 0 || strcmp(status, "MISMATCH") == 0)
          ? NYT_RED
          : NYT_YELLOW;
  BenchUiLayout l = bench_ui_layout();
  char label[128], status_label[32];
  bench_clip(name, l.name, label, sizeof(label));
  bench_clip(status, l.status, status_label, sizeof(status_label));
  char nc_cell[64], lc_cell[64], nl_cell[64];
  char compile_cell[64], native_cell[64], llvm_cell[64], c_cell[64];
  bench_live_cell(nc_cell, sizeof(nc_cell), native_c, l.ratio,
                  bench_live_ratio_color(native_c));
  bench_live_cell(lc_cell, sizeof(lc_cell), llvm_c, l.ratio,
                  bench_live_ratio_color(llvm_c));
  bench_live_cell(nl_cell, sizeof(nl_cell), native_llvm, l.ratio,
                  bench_live_ratio_color(native_llvm));
  bench_live_cell(compile_cell, sizeof(compile_cell), compile, l.compile,
                  NYT_GRAY);
  bench_live_cell(native_cell, sizeof(native_cell), native, l.native,
                  NYT_RESET);
  bench_live_cell(llvm_cell, sizeof(llvm_cell), llvm, l.llvm, NYT_RESET);
  bench_live_cell(c_cell, sizeof(c_cell), c_host, l.c_host, NYT_RESET);
  char line[1024];
  int n = snprintf(line, sizeof(line), "%s%s%s%s%*s %s%s%s%*s",
                   nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), label,
                   nyt_clr(NYT_RESET), l.name - (int)bench_disp_width(label),
                   "", nyt_clr(status_color), status_label, nyt_clr(NYT_RESET),
                   l.status - (int)bench_disp_width(status_label), "");
  if (n > 0 && (size_t)n < sizeof(line)) {
    int added;
    if (l.tiny)
      added = snprintf(line + n, sizeof(line) - (size_t)n, " %s %s\n",
                       native_cell, c_cell);
    else if (l.compact)
      added =
          snprintf(line + n, sizeof(line) - (size_t)n, " %s %s %s %s %s %s\n",
                   native_cell, llvm_cell, c_cell, nc_cell, lc_cell, nl_cell);
    else
      added = snprintf(line + n, sizeof(line) - (size_t)n,
                       " %s %s %s %s %s %s %s\n", compile_cell, native_cell,
                       llvm_cell, c_cell, nc_cell, lc_cell, nl_cell);
    if (added > 0)
      n += added;
  }
  if (n <= 0)
    return;
  size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
#ifdef _WIN32
  fwrite(line, 1, len, stdout);
  fflush(stdout);
#else
  (void)write(STDOUT_FILENO, line, len);
#endif
}

static bench_worker_result_t
bench_measure_shape(const char *bin, const char *shape, const char *opt_level,
                    const char *tier, const char *engine, const char *target,
                    int compare_llvm, int warmup, int runs, int timeout_sec) {
  bench_worker_result_t result = {0};
  double wall_start = now_ms();
  result.ny = bench_measure_ny(bin, shape, opt_level, tier, engine, target,
                               warmup, runs, timeout_sec);
  if (compare_llvm) {
    const char *llvm_engine =
        (!engine || strcmp(engine, "aot") == 0 || strcmp(engine, "native") == 0)
            ? "mcjit"
            : engine;
    result.llvm = bench_measure_ny(bin, shape, opt_level, tier, llvm_engine,
                                   "llvm", warmup, runs, timeout_sec);
  }
  result.c = bench_measure_c(shape, opt_level, warmup, runs, timeout_sec);
  result.wall_ms = now_ms() - wall_start;
  return result;
}

static int bench_run_worker_jobs(BenchJob *jobs, size_t job_count,
                                 size_t worker_id, size_t worker_count,
                                 const char *bin, const char *opt_level,
                                 const char *tier, const char *engine,
                                 const char *target, int compare_llvm,
                                 int warmup, int runs, int timeout_sec,
                                 FILE *result_file) {
  for (size_t i = worker_id; i < job_count; i += worker_count) {
    bench_worker_result_t result =
        bench_measure_shape(bin, jobs[i].shape, opt_level, tier, engine, target,
                            compare_llvm, warmup, runs, timeout_sec);
    if (result_file) {
      if (fwrite(&i, sizeof(i), 1, result_file) != 1 ||
          fwrite(&result, sizeof(result), 1, result_file) != 1)
        return 0;
    } else {
      jobs[i].result = result;
    }
    bench_live_fixture_result(jobs[i].name, &result);
  }
  return 1;
}

static void bench_print_row(const BenchRow *r, int show_c, int show_llvm);
static int bench_native_c_comparable(const BenchRow *r);
static int bench_llvm_c_comparable(const BenchRow *r);
static int bench_native_llvm_comparable(const BenchRow *r);
static const char *bench_row_status(const BenchRow *r);

static double bench_shape_budget(const char *shape, const char *key) {
  char *raw = shape_meta_string(shape, key);
  if (!raw || !*raw) {
    free(raw);
    return 0.0;
  }
  char *end = NULL;
  double value = strtod(raw, &end);
  while (end && *end && isspace((unsigned char)*end))
    ++end;
  bool ok = end && end != raw && *end == '\0' && value > 0.0;
  free(raw);
  return ok ? value : 0.0;
}

static size_t bench_shape_size_budget(const char *shape, const char *key) {
  char *raw = shape_meta_string(shape, key);
  if (!raw || !*raw) {
    free(raw);
    return 0;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 10);
  while (end && *end && isspace((unsigned char)*end))
    ++end;
  bool ok = errno == 0 && end && end != raw && *end == '\0' && value > 0 &&
            value <= (unsigned long long)SIZE_MAX;
  free(raw);
  return ok ? (size_t)value : 0;
}

/*
 * Unlike time/code-size budgets, static-island quality limits deliberately
 * accept zero so fixtures can require allocation/helper/check-free optimized
 * paths. The boolean return distinguishes an explicit zero from no metadata.
 */
static int bench_shape_size_limit(const char *shape, const char *key,
                                  size_t *out) {
  if (out)
    *out = 0;
  if (!out)
    return 0;
  char *raw = shape_meta_string(shape, key);
  if (!raw || !*raw) {
    free(raw);
    return 0;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 10);
  while (end && *end && isspace((unsigned char)*end))
    ++end;
  bool ok = errno == 0 && end && end != raw && *end == '\0' &&
            value <= (unsigned long long)SIZE_MAX;
  free(raw);
  if (!ok)
    return 0;
  *out = (size_t)value;
  return 1;
}

static void bench_record_result(
    BenchTable *table, int *measured, int verbose, int show_ir, int show_asm,
    int show_passes, int profile, int compare_llvm, const char *bin,
    const char *opt_level, const char *tier, const char *engine,
    const char *target, const char *compile_profile, int warmup, int runs,
    int timeout_sec, const char *shape, const char *name,
    const bench_worker_result_t *result, int suppress_success_row) {
  if (show_ir || show_asm || show_passes || profile)
    bench_diag(shape, show_ir, show_asm, show_passes, profile, compare_llvm,
               bin, opt_level, tier, engine, target, NULL);
  BenchRow row = {0};
  snprintf(row.name, sizeof(row.name), "%s", name);
  snprintf(row.path, sizeof(row.path), "%s", shape);
  snprintf(row.backend, sizeof(row.backend), "%s",
           engine && strcmp(engine, "aot") != 0 && strcmp(engine, "native") != 0
               ? "llvm"
               : (target && *target ? target : "host"));
  snprintf(row.engine, sizeof(row.engine), "%s",
           engine && *engine ? engine : "aot");
  snprintf(row.cache, sizeof(row.cache), "%s", bench_cache_state());
  row.wall_ms = result->wall_ms;
  row.ny_ms = result->ny.median;
  row.ny_compile_ms = result->ny.compile_ms;
  row.ny_link_ms = result->ny.link_ms;
  row.ny_jit_compile_ms = result->ny.jit_compile_ms;
  row.ny_jit_run_ms = result->ny.jit_run_ms;
  row.ny_opt_ms = result->ny.opt_ms;
  row.ny_total_ms = result->ny.wall_ms;
  row.ny_code_bytes = result->ny.code_bytes;
  row.ny_code_bytes_seen = result->ny.code_bytes_seen;
  row.ny_specialization_code_bytes = result->ny.specialization_code_bytes;
  row.ny_specialization_code_functions =
      result->ny.specialization_code_functions;
  row.ny_specialization_max_function_bytes =
      result->ny.specialization_max_function_bytes;
  row.ny_specialization_metrics_seen = result->ny.specialization_metrics_seen;
  row.ny_peak_compiler_rss_kb = result->ny.peak_compiler_rss_kb;
  row.ny_peak_compiler_rss_seen = result->ny.peak_compiler_rss_seen;
  row.ny_runtime_calls = result->ny.runtime_calls;
  row.ny_dynamic_ops = result->ny.dynamic_ops;
  row.ny_tag_checks = result->ny.tag_checks;
  row.ny_box_unbox = result->ny.box_unbox;
  row.ny_heap_allocations = result->ny.heap_allocations;
  row.ny_bounds_checks = result->ny.bounds_checks;
  row.ny_direct_calls = result->ny.direct_calls;
  row.ny_indirect_calls = result->ny.indirect_calls;
  row.ny_unknown_effects = result->ny.unknown_effects;
  row.ny_alias_unresolved = result->ny.alias_unresolved;
  row.ny_vector_attempted = result->ny.vector_attempted;
  row.ny_vector_rejected = result->ny.vector_rejected;
  row.ny_vectorized = result->ny.vectorized;
  row.ny_spills = result->ny.spills;
  row.ny_reloads = result->ny.reloads;
  row.ny_static_metrics_seen = result->ny.static_metrics_seen;
  row.ny_hw_cycles = result->ny.hw_cycles;
  row.ny_hw_instructions = result->ny.hw_instructions;
  row.ny_hw_branches = result->ny.hw_branches;
  row.ny_hw_branch_misses = result->ny.hw_branch_misses;
  row.ny_hw_cache_misses = result->ny.hw_cache_misses;
  row.ny_hw_counters_seen = result->ny.hw_counters_seen;
  row.llvm_hw_cycles = result->llvm.hw_cycles;
  row.llvm_hw_instructions = result->llvm.hw_instructions;
  row.llvm_hw_branches = result->llvm.hw_branches;
  row.llvm_hw_branch_misses = result->llvm.hw_branch_misses;
  row.llvm_hw_cache_misses = result->llvm.hw_cache_misses;
  row.llvm_hw_counters_seen = result->llvm.hw_counters_seen;
  row.ny_runtime_alloc_count = result->ny.runtime_alloc_count;
  row.ny_runtime_realloc_count = result->ny.runtime_realloc_count;
  row.ny_runtime_counters_seen = result->ny.runtime_counters_seen;
  row.ny_runs = (int)result->ny.count;
  row.ny_timer_fallback = result->ny.timer_fallback_count > 0;
  row.ny_fixture_timer = result->ny.fixture_timer_count > 0;
  row.ny_rc = result->ny.last_rc;
  row.ny_failures = result->ny.failure_count;
  row.ny_min = result->ny.min;
  row.ny_max = result->ny.max;
  row.ny_p95 = result->ny.p95;
  row.ny_dispersion_pct = result->ny.dispersion_pct;
  row.ny_noise_pct = result->ny.noise_pct;
  row.ny_unstable = result->ny.unstable;
  row.llvm_ms = result->llvm.median;
  row.llvm_total_ms = result->llvm.wall_ms;
  row.llvm_runs = (int)result->llvm.count;
  row.llvm_timer_fallback = result->llvm.timer_fallback_count > 0;
  row.llvm_fixture_timer = result->llvm.fixture_timer_count > 0;
  row.llvm_rc = result->llvm.last_rc;
  row.llvm_failures = result->llvm.failure_count;
  row.llvm_min = result->llvm.min;
  row.llvm_max = result->llvm.max;
  row.llvm_p95 = result->llvm.p95;
  row.llvm_dispersion_pct = result->llvm.dispersion_pct;
  row.llvm_noise_pct = result->llvm.noise_pct;
  row.llvm_unstable = result->llvm.unstable;
  row.c_ms = result->c.median;
  row.c_total_ms = result->c.wall_ms;
  row.c_runs = (int)result->c.count;
  row.c_fixture_timer = result->c.fixture_timer_count > 0;
  row.c_rc = result->c.last_rc;
  row.c_failures = result->c.failure_count;
  row.c_min = result->c.min;
  row.c_max = result->c.max;
  row.c_p95 = result->c.p95;
  row.c_dispersion_pct = result->c.dispersion_pct;
  row.c_noise_pct = result->c.noise_pct;
  row.c_unstable = result->c.unstable;
  row.native_c_budget = bench_shape_budget(shape, "max_native_c_ratio");
  row.native_llvm_budget = bench_shape_budget(shape, "max_native_llvm_ratio");
  row.compile_ms_budget = bench_shape_budget(shape, "max_ny_compile_ms");
  row.code_bytes_budget = bench_shape_size_budget(shape, "max_ny_code_bytes");
  row.specialization_code_bytes_budget =
      bench_shape_size_budget(shape, "max_ny_specialization_code_bytes");
  row.specialization_function_bytes_budget =
      bench_shape_size_budget(shape, "max_ny_specialization_function_bytes");
  row.compiler_rss_budget_kb =
      bench_shape_size_budget(shape, "max_ny_peak_compiler_rss_kb");
  if (bench_shape_size_limit(shape, "max_ny_heap_allocations",
                             &row.heap_allocations_budget))
    row.static_budget_mask |= 1u << 0;
  if (bench_shape_size_limit(shape, "max_ny_runtime_calls",
                             &row.runtime_calls_budget))
    row.static_budget_mask |= 1u << 1;
  if (bench_shape_size_limit(shape, "max_ny_bounds_checks",
                             &row.bounds_checks_budget))
    row.static_budget_mask |= 1u << 2;
  if (bench_shape_size_limit(shape, "max_ny_indirect_calls",
                             &row.indirect_calls_budget))
    row.static_budget_mask |= 1u << 3;
  if (bench_shape_size_limit(shape, "max_ny_unknown_effects",
                             &row.unknown_effects_budget))
    row.static_budget_mask |= 1u << 4;
  if (bench_shape_size_limit(shape, "max_ny_alias_unresolved",
                             &row.alias_unresolved_budget))
    row.static_budget_mask |= 1u << 5;
  if (bench_shape_size_limit(shape, "max_ny_spills", &row.spills_budget))
    row.static_budget_mask |= 1u << 6;
  if (bench_shape_size_limit(shape, "max_ny_reloads", &row.reloads_budget))
    row.static_budget_mask |= 1u << 7;
  if (row.compile_ms_budget > 0.0 && row.ny_compile_ms > row.compile_ms_budget)
    row.budget_failed = 1;
  if (row.code_bytes_budget > 0 && row.ny_code_bytes_seen &&
      row.ny_code_bytes > row.code_bytes_budget)
    row.budget_failed = 1;
  if ((row.specialization_code_bytes_budget > 0 ||
       row.specialization_function_bytes_budget > 0) &&
      !row.ny_specialization_metrics_seen)
    row.budget_failed = 1;
  if (row.specialization_code_bytes_budget > 0 &&
      row.ny_specialization_code_bytes > row.specialization_code_bytes_budget)
    row.budget_failed = 1;
  if (row.specialization_function_bytes_budget > 0 &&
      row.ny_specialization_max_function_bytes >
          row.specialization_function_bytes_budget)
    row.budget_failed = 1;
  if (row.compiler_rss_budget_kb > 0 && row.ny_peak_compiler_rss_seen &&
      row.ny_peak_compiler_rss_kb > row.compiler_rss_budget_kb)
    row.budget_failed = 1;
  if (row.static_budget_mask && !row.ny_static_metrics_seen)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 0)) &&
      row.ny_heap_allocations > row.heap_allocations_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 1)) &&
      row.ny_runtime_calls > row.runtime_calls_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 2)) &&
      row.ny_bounds_checks > row.bounds_checks_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 3)) &&
      row.ny_indirect_calls > row.indirect_calls_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 4)) &&
      row.ny_unknown_effects > row.unknown_effects_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 5)) &&
      row.ny_alias_unresolved > row.alias_unresolved_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 6)) && row.ny_spills > row.spills_budget)
    row.budget_failed = 1;
  if ((row.static_budget_mask & (1u << 7)) &&
      row.ny_reloads > row.reloads_budget)
    row.budget_failed = 1;
  row.checksum_ok = 1;
  if (result->ny.checksum_seen)
    snprintf(row.ny_checksum, sizeof(row.ny_checksum), "%s",
             result->ny.checksum);
  if (result->llvm.checksum_seen)
    snprintf(row.llvm_checksum, sizeof(row.llvm_checksum), "%s",
             result->llvm.checksum);
  if (result->c.checksum_seen)
    snprintf(row.c_checksum, sizeof(row.c_checksum), "%s", result->c.checksum);
  const bench_stats_t *stats[] = {&result->ny, &result->llvm, &result->c};
  const char *reference = NULL;
  for (size_t i = 0; i < sizeof(stats) / sizeof(stats[0]); i++) {
    const bench_stats_t *s = stats[i];
    if (!s->checksum_seen)
      continue;
    if (!reference)
      reference = s->checksum;
    else if (strcmp(reference, s->checksum) != 0)
      row.checksum_ok = 0;
    if (!s->checksum_consistent)
      row.checksum_ok = 0;
  }
  if (reference)
    snprintf(row.checksum, sizeof(row.checksum), "%s", reference);
  if (row.native_c_budget > 0.0 && bench_native_c_comparable(&row) &&
      row.ny_ms / row.c_ms > row.native_c_budget)
    row.budget_failed = 1;
  if (row.native_llvm_budget > 0.0 && bench_native_llvm_comparable(&row) &&
      row.ny_ms / row.llvm_ms > row.native_llvm_budget)
    row.budget_failed = 1;
  if (row.ny_runs > 0 || row.llvm_runs > 0 || row.c_runs > 0 ||
      row.ny_rc == 0 || row.llvm_rc == 0 || row.c_rc == 0) {
    (*measured)++;
  }
  bench_rows_push(table, &row);
  if (!suppress_success_row || row.budget_failed)
    bench_print_row(&row, 1, compare_llvm);
  (void)verbose;
  (void)compile_profile;
  (void)warmup;
  (void)runs;
  (void)timeout_sec;
}

static const char *bench_ratio_color(double ratio) {
  if (ratio <= 0.0)
    return NYT_GRAY;
  if (ratio <= 1.0)
    return NYT_BOLD;
  if (ratio <= 2.0)
    return NYT_GREEN;
  if (ratio <= 3.0)
    return NYT_YELLOW;
  return NYT_RED;
}

static int bench_signal(double value) {
  /*
   * The fixtures time their kernels with a monotonic nanosecond clock.  A
   * 10us cutoff hid valid samples (and produced `n/a` ratios) for short but
   * repeatable kernels.  Keep ratios for samples of at least 1us; below that
   * the displayed status calls out the weak signal.
   */
  return value >= 0.001;
}

static int bench_row_unstable(const BenchRow *r) {
  return r && (r->ny_unstable || r->llvm_unstable || r->c_unstable);
}

static int bench_native_c_comparable(const BenchRow *r) {
  return r && r->checksum_ok && r->ny_runs > 0 && r->c_runs > 0 &&
         !r->ny_failures && !r->c_failures && !r->ny_timer_fallback &&
         !r->ny_unstable && !r->c_unstable && r->ny_checksum[0] &&
         r->c_checksum[0] && strcmp(r->ny_checksum, r->c_checksum) == 0 &&
         bench_signal(r->ny_ms) && bench_signal(r->c_ms);
}

static int bench_llvm_c_comparable(const BenchRow *r) {
  return r && r->checksum_ok && r->llvm_runs > 0 && r->c_runs > 0 &&
         !r->llvm_failures && !r->c_failures && !r->llvm_timer_fallback &&
         !r->llvm_unstable && !r->c_unstable && r->llvm_checksum[0] &&
         r->c_checksum[0] && strcmp(r->llvm_checksum, r->c_checksum) == 0 &&
         bench_signal(r->llvm_ms) && bench_signal(r->c_ms);
}

static int bench_native_llvm_comparable(const BenchRow *r) {
  return r && r->checksum_ok && r->ny_runs > 0 && r->llvm_runs > 0 &&
         !r->ny_failures && !r->llvm_failures && !r->ny_timer_fallback &&
         !r->llvm_timer_fallback && !r->ny_unstable && !r->llvm_unstable &&
         r->ny_checksum[0] && r->llvm_checksum[0] &&
         strcmp(r->ny_checksum, r->llvm_checksum) == 0 &&
         bench_signal(r->ny_ms) && bench_signal(r->llvm_ms);
}

static const char *bench_row_status(const BenchRow *r) {
  if (!r)
    return "unavailable";
  if (r->ny_failures || r->llvm_failures || r->c_failures)
    return "fail";
  if (!r->checksum_ok)
    return "mismatch";
  if (r->ny_timer_fallback || r->llvm_timer_fallback)
    return "timer-fallback";
  if (r->ny_runs <= 0)
    return "partial";
  if (!r->ny_checksum[0] || (r->c_runs > 0 && !r->c_checksum[0]) ||
      (r->llvm_runs > 0 && !r->llvm_checksum[0]))
    return "no-checksum";
  if (r->c_runs <= 0)
    return "ny-llvm-only";
  if (r->budget_failed)
    return "regression";
  if (bench_row_unstable(r))
    return "unstable";
  return "ok";
}

static void bench_left(const char *s, int width) {
  char clipped[128];
  bench_clip(s, width, clipped, sizeof(clipped));
  int pad = width - (int)bench_disp_width(clipped);
  printf("%s%*s", clipped, pad > 0 ? pad : 0, "");
}

static void bench_right(const char *s, int width) {
  int pad = width - (int)bench_disp_width(s);
  printf("%*s%s", pad > 0 ? pad : 0, "", s ? s : "");
}

static void bench_ms_text(double value, int valid, char *out, size_t cap) {
  if (!valid || value <= 0.0)
    snprintf(out, cap, "—");
  else if (value < 0.001)
    snprintf(out, cap, "<1µs");
  else if (value < 0.01)
    snprintf(out, cap, "%.1fµs", value * 1000.0);
  else if (value < 1.0)
    snprintf(out, cap, "%.0fµs", value * 1000.0);
  else if (value < 10.0)
    snprintf(out, cap, "%.2fms", value);
  else if (value < 100.0)
    snprintf(out, cap, "%.1fms", value);
  else if (value < 1000.0)
    snprintf(out, cap, "%.0fms", value);
  else
    snprintf(out, cap, "%.2fs", value / 1000.0);
}

static void bench_print_ratio(double ratio, int valid, int width) {
  if (!valid) {
    printf("%s", nyt_clr(NYT_GRAY));
    bench_right("—", width);
    printf("%s", nyt_clr(NYT_RESET));
    return;
  }
  char value[32];
  bench_ratio_text(value, sizeof(value), ratio);
  printf("%s", nyt_clr(bench_ratio_color(ratio)));
  bench_right(value, width);
  printf("%s", nyt_clr(NYT_RESET));
}

static void bench_header_cell(const char *text, int width) {
  printf("%s", nyt_clr(NYT_BOLD));
  bench_left(text, width);
  printf("%s", nyt_clr(NYT_RESET));
}

static void bench_header_right(const char *text, int width) {
  printf("%s", nyt_clr(NYT_BOLD));
  bench_right(text, width);
  printf("%s", nyt_clr(NYT_RESET));
}

static void bench_print_header(void) {
  BenchUiLayout l = bench_ui_layout();
  bench_header_cell("benchmark", l.name);
  putchar(' ');
  bench_header_cell("status", l.status);
  if (l.tiny) {
    putchar(' ');
    bench_header_right("NYR", l.native);
    putchar(' ');
    bench_header_right("C", l.c_host);
  } else if (l.compact) {
    putchar(' ');
    bench_header_right("NYR", l.native);
    putchar(' ');
    bench_header_right("LLVM", l.llvm);
    putchar(' ');
    bench_header_right("C", l.c_host);
    putchar(' ');
    bench_header_right("NYR/C", l.ratio);
    putchar(' ');
    bench_header_right("LLVM/C", l.ratio);
    putchar(' ');
    bench_header_right("LLVM/NYR", l.ratio);
  } else {
    putchar(' ');
    bench_header_right("compile", l.compile);
    putchar(' ');
    bench_header_right("NYR", l.native);
    putchar(' ');
    bench_header_right("LLVM", l.llvm);
    putchar(' ');
    bench_header_right("C", l.c_host);
    putchar(' ');
    bench_header_right("NYR/C", l.ratio);
    putchar(' ');
    bench_header_right("LLVM/C", l.ratio);
    putchar(' ');
    bench_header_right("LLVM/NYR", l.ratio);
  }
  putchar('\n');
}

static void bench_print_row(const BenchRow *r, int show_c, int show_llvm) {
  BenchUiLayout l = bench_ui_layout();
  char compile[32], native[32], llvm[32], c_host[32];
  bench_ms_text(r->ny_compile_ms, bench_signal(r->ny_compile_ms), compile,
                sizeof(compile));
  bench_ms_text(r->ny_ms, r->ny_runs > 0, native, sizeof(native));
  bench_ms_text(r->llvm_ms, r->llvm_runs > 0, llvm, sizeof(llvm));
  bench_ms_text(r->c_ms, r->c_runs > 0, c_host, sizeof(c_host));
  if (test_env_truthy("NYTRIX_BENCH_TRACE_TIMING"))
    fprintf(stderr,
            "[bench-row] %s native=%.6g runs=%d timer_fallback=%d display=%s "
            "llvm=%.6g runs=%d timer_fallback=%d display=%s\n",
            r->name, r->ny_ms, r->ny_runs, r->ny_timer_fallback, native,
            r->llvm_ms, r->llvm_runs, r->llvm_timer_fallback, llvm);
  if (r->ny_timer_fallback) {
    char marked[sizeof(native)];
    snprintf(marked, sizeof(marked), "~%s", native);
    snprintf(native, sizeof(native), "%s", marked);
  }
  if (r->llvm_timer_fallback) {
    char marked[sizeof(llvm)];
    snprintf(marked, sizeof(marked), "~%s", llvm);
    snprintf(llvm, sizeof(llvm), "%s", marked);
  }
  int native_ratio_valid = show_c && bench_native_c_comparable(r);
  int llvm_ratio_valid = show_llvm && bench_llvm_c_comparable(r);
  int native_llvm_ratio_valid = show_llvm && bench_native_llvm_comparable(r);
  double native_ratio = native_ratio_valid ? r->ny_ms / r->c_ms : 0.0;
  double llvm_ratio = llvm_ratio_valid ? r->llvm_ms / r->c_ms : 0.0;
  double native_llvm_ratio =
      native_llvm_ratio_valid ? r->ny_ms / r->llvm_ms : 0.0;
  bool is_fail =
      r->ny_failures || r->llvm_failures || r->c_failures || !r->checksum_ok;
  bool missing_native_time = r->ny_runs == 0 && r->ny_failures == 0;
  bool missing_llvm_time =
      show_llvm && r->llvm_runs == 0 && r->llvm_failures == 0;
  bool missing_checksum =
      !r->ny_checksum[0] || (r->c_runs > 0 && !r->c_checksum[0]) ||
      (show_llvm && r->llvm_runs > 0 && !r->llvm_checksum[0]);
  bool missing_c_time = r->c_runs == 0 && r->c_failures == 0;
  bool timer_fallback = r->ny_timer_fallback || r->llvm_timer_fallback;

  const char *status_str =
      is_fail
          ? (r->ny_failures || r->llvm_failures || r->c_failures ? "FAIL"
                                                                 : "MISMATCH")
          : (r->budget_failed
                 ? "REGRESS"
                 : (bench_row_unstable(r)
                        ? "NOISY"
                        : (missing_native_time
                               ? "NO-TIME"
                               : (missing_llvm_time
                                      ? "PARTIAL"
                                      : (missing_c_time
                                             ? "NY/LLVM"
                                             : (missing_checksum
                                                    ? "NO-CHECK"
                                                    : (timer_fallback
                                                           ? "TIMER-FB"
                                                           : "ok")))))));
  char status_label[16];
  snprintf(status_label, sizeof(status_label), "%s", status_str);
  const char *status_clr =
      is_fail ? NYT_RED
              : (r->budget_failed || missing_native_time || missing_llvm_time ||
                         missing_checksum || missing_c_time || timer_fallback ||
                         bench_row_unstable(r)
                     ? NYT_YELLOW
                     : NYT_GREEN);

  bench_left(r->name, l.name);
  putchar(' ');
  printf("%s", nyt_clr(status_clr));
  bench_left(status_label, l.status);
  printf("%s", nyt_clr(NYT_RESET));
  if (l.tiny) {
    putchar(' ');
    bench_right(native, l.native);
    putchar(' ');
    bench_right(c_host, l.c_host);
  } else if (l.compact) {
    putchar(' ');
    bench_right(native, l.native);
    putchar(' ');
    bench_right(llvm, l.llvm);
    putchar(' ');
    bench_right(c_host, l.c_host);
    putchar(' ');
    bench_print_ratio(native_ratio, native_ratio_valid, l.ratio);
    putchar(' ');
    bench_print_ratio(llvm_ratio, llvm_ratio_valid, l.ratio);
    putchar(' ');
    bench_print_ratio(native_llvm_ratio, native_llvm_ratio_valid, l.ratio);
  } else {
    putchar(' ');
    bench_right(compile, l.compile);
    putchar(' ');
    bench_right(native, l.native);
    putchar(' ');
    bench_right(llvm, l.llvm);
    putchar(' ');
    bench_right(c_host, l.c_host);
    putchar(' ');
    bench_print_ratio(native_ratio, native_ratio_valid, l.ratio);
    putchar(' ');
    bench_print_ratio(llvm_ratio, llvm_ratio_valid, l.ratio);
    putchar(' ');
    bench_print_ratio(native_llvm_ratio, native_llvm_ratio_valid, l.ratio);
  }
  printf("\n");
  if (!r->checksum_ok) {
    char detail[256];
    snprintf(detail, sizeof(detail), "checksum mismatch: %s | %s | %s",
             r->ny_checksum[0] ? r->ny_checksum : "—",
             r->llvm_checksum[0] ? r->llvm_checksum : "—",
             r->c_checksum[0] ? r->c_checksum : "—");
    char clipped[256];
    bench_clip(detail, ny_ui_width() - 6, clipped, sizeof(clipped));
    printf("  %s%s%s %s\n", nyt_clr(NYT_RED), ny_ui_mark("error"),
           nyt_clr(NYT_RESET), clipped);
  }
}
static void bench_print_hotspots(const BenchTable *t) {
  if (!t || t->len == 0)
    return;
  const BenchRow *compile = NULL;
  const BenchRow *runtime = NULL;
  const BenchRow *ratio = NULL;
  const BenchRow *llvm_gap = NULL;
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    if (bench_signal(r->ny_compile_ms) &&
        (!compile || r->ny_compile_ms > compile->ny_compile_ms))
      compile = r;
    if (bench_signal(r->ny_ms) && (!runtime || r->ny_ms > runtime->ny_ms))
      runtime = r;
    if (bench_native_c_comparable(r) &&
        (!ratio || r->ny_ms / r->c_ms > ratio->ny_ms / ratio->c_ms))
      ratio = r;
    if (bench_native_llvm_comparable(r) &&
        (!llvm_gap ||
         r->ny_ms / r->llvm_ms > llvm_gap->ny_ms / llvm_gap->llvm_ms))
      llvm_gap = r;
  }
  nyt_subheading("Hotspots");
  nyt_kv("slowest", "compile %s · runtime %s", compile ? compile->name : "—",
         runtime ? runtime->name : "—");
  if (ratio) {
    double gap = ratio->ny_ms / ratio->c_ms;
    char text[32];
    bench_ratio_text(text, sizeof(text), gap);
    nyt_kv("NYR/C gap", "%s (%s)", ratio->name, text);
  } else
    nyt_kv("NYR/C gap", "— (no comparable samples)");
  if (llvm_gap) {
    double gap = llvm_gap->ny_ms / llvm_gap->llvm_ms;
    char text[32];
    bench_ratio_text(text, sizeof(text), gap);
    nyt_kv("LLVM/NYR gap", "%s (%s)", llvm_gap->name, text);
  } else
    nyt_kv("LLVM/NYR gap", "— (no comparable samples)");
}

/*
 * A4 bench methodology: report the geomean of each ratio over comparable
 * fixtures twice — once over all comparable fixtures and once over the
 * subset with C >= 1 ms (where ~350 us fixed overhead no longer dominates).
 * Per-class (bench directory) lines give the data that per-class thresholds
 * are set from; classes without comparable fixtures print "—".
 */
#define BENCH_GEOMEAN_CLASSES 8
/*
 * Default per-fixture outlier ceiling for Native versus LLVM. A fixture with
 * an explicit max_native_llvm_ratio budget owns its workload-specific cap.
 */
#define BENCH_MAX_NATIVE_LLVM_RATIO 5.0

typedef struct {
  const char *name;
  double native_c_max;
  double llvm_c_max;
  double native_llvm_max;
} BenchGeomeanBudget;

typedef struct {
  const char *name;
  char native_c[80];
  char llvm_c[80];
  char native_llvm[80];
  char max_native_llvm[160];
  bool failed;
} BenchGeomeanGateRow;

/*
 * Class geomean ceilings are 1.6x the clean three-run full-suite baseline
 * recorded on 2026-10-11. The concurrency class uses 2x because its sole
 * C>=1ms fixture (`thread-ring`) varies substantially with host scheduling.
 *
 * The `startup` LLVM/C ceiling was recalibrated on 2026-10-14: the original
 * 0.05x value was set when the C>=1ms startup subset was dominated by floor
 * fixtures whose LLVM startup crushed the short C reference. Since then the
 * subset is `escape-loop` (its C reference does 1M malloc/free, ~8ms) plus
 * `inline` (~1.5ms), and those stably geomean to ~0.17x LLVM/C — above the
 * old 0.05x ceiling, so the gate failed deterministically whenever
 * `escape-loop`'s C time crossed the 1ms inclusion threshold. 0.30x restores
 * the documented 1.6x-baseline relationship while still catching a real
 * ~1.7x LLVM startup regression.
 */
static const BenchGeomeanBudget bench_geomean_budgets[] = {
    {"algorithms", 5.75, 1.92, 4.80},     {"concurrency", 1357.0, 1883.0, 1.44},
    {"datastructures", 8.18, 6.77, 1.94}, {"numeric", 115.03, 71.03, 2.60},
    {"startup", 0.42, 0.30, 13.03},       {"string", 5.20, 4.18, 2.00},
    {"overall", 9.22, 4.99, 2.96},
};

typedef struct {
  char name[32];
  double native_c_log_all;
  double llvm_c_log_all;
  double native_llvm_log_all;
  size_t native_c_n_all;
  size_t llvm_c_n_all;
  size_t native_llvm_n_all;
  double native_c_log_big;
  double llvm_c_log_big;
  double native_llvm_log_big;
  size_t native_c_n_big;
  size_t llvm_c_n_big;
  size_t native_llvm_n_big;
} BenchGeomeanClass;

static const char *bench_row_class(const BenchRow *r, char *out, size_t cap) {
  if (!out || cap == 0)
    return "?";
  out[0] = '\0';
  if (!r || !r->path[0])
    return "?";
  /*
   * Class is the bench directory holding the fixture, e.g. `startup` in
   * `etc/tests/bench/startup/escape-loop.nshape`.
   */
  const char *slash = strrchr(r->path, '/');
#ifdef _WIN32
  const char *bslash = strrchr(r->path, '\\');
  if (bslash && (!slash || bslash > slash))
    slash = bslash;
#endif
  if (!slash)
    return "?";
  size_t dir_end = (size_t)(slash - r->path);
  size_t dir_start = dir_end;
  while (dir_start > 0 && r->path[dir_start - 1] != '/' &&
#ifdef _WIN32
         r->path[dir_start - 1] != '\\' &&
#endif
         1)
    dir_start--;
  size_t len = dir_end - dir_start;
  if (len == 0 || len >= cap)
    return "?";
  memcpy(out, r->path + dir_start, len);
  out[len] = '\0';
  return out;
}

static void bench_grid_rule(const size_t *widths, size_t count) {
  printf("  +");
  for (size_t col = 0; col < count; ++col) {
    for (size_t i = 0; i < widths[col] + 2; ++i)
      putchar('-');
    putchar('+');
  }
  putchar('\n');
}

static void bench_grid_row(const char *const *cells, const size_t *widths,
                           size_t count) {
  printf("  |");
  for (size_t col = 0; col < count; ++col) {
    putchar(' ');
    bench_left(cells[col] ? cells[col] : "", (int)widths[col]);
    printf(" |");
  }
  putchar('\n');
}

static int bench_print_geomean(const BenchTable *t, bool enforce_budgets) {
  if (!t || t->len == 0)
    return 0;
  BenchGeomeanClass classes[BENCH_GEOMEAN_CLASSES];
  size_t nclasses = 0;
  memset(classes, 0, sizeof(classes));
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    char cls[32];
    const char *cname = bench_row_class(r, cls, sizeof(cls));
    BenchGeomeanClass *c = NULL;
    for (size_t k = 0; k < nclasses; k++) {
      if (strcmp(classes[k].name, cname) == 0) {
        c = &classes[k];
        break;
      }
    }
    if (!c) {
      if (nclasses >= BENCH_GEOMEAN_CLASSES)
        continue;
      c = &classes[nclasses++];
      snprintf(c->name, sizeof(c->name), "%s", cname);
    }
    int big = r->c_ms >= 1.0;
    if (bench_native_c_comparable(r)) {
      double ratio = r->ny_ms / r->c_ms;
      if (ratio > 0.0) {
        c->native_c_log_all += log(ratio);
        c->native_c_n_all++;
        if (big) {
          c->native_c_log_big += log(ratio);
          c->native_c_n_big++;
        }
      }
    }
    if (bench_llvm_c_comparable(r)) {
      double ratio = r->llvm_ms / r->c_ms;
      if (ratio > 0.0) {
        c->llvm_c_log_all += log(ratio);
        c->llvm_c_n_all++;
        if (big) {
          c->llvm_c_log_big += log(ratio);
          c->llvm_c_n_big++;
        }
      }
    }
    if (bench_native_llvm_comparable(r)) {
      double ratio = r->ny_ms / r->llvm_ms;
      if (ratio > 0.0) {
        c->native_llvm_log_all += log(ratio);
        c->native_llvm_n_all++;
        if (big) {
          c->native_llvm_log_big += log(ratio);
          c->native_llvm_n_big++;
        }
      }
    }
  }
  double all_nc = 0.0, all_lc = 0.0, all_nl = 0.0;
  double big_nc = 0.0, big_lc = 0.0, big_nl = 0.0;
  size_t all_nc_n = 0, all_lc_n = 0, all_nl_n = 0;
  size_t big_nc_n = 0, big_lc_n = 0, big_nl_n = 0;
  for (size_t k = 0; k < nclasses; k++) {
    all_nc += classes[k].native_c_log_all;
    all_lc += classes[k].llvm_c_log_all;
    all_nl += classes[k].native_llvm_log_all;
    all_nc_n += classes[k].native_c_n_all;
    all_lc_n += classes[k].llvm_c_n_all;
    all_nl_n += classes[k].native_llvm_n_all;
    big_nc += classes[k].native_c_log_big;
    big_lc += classes[k].llvm_c_log_big;
    big_nl += classes[k].native_llvm_log_big;
    big_nc_n += classes[k].native_c_n_big;
    big_lc_n += classes[k].llvm_c_n_big;
    big_nl_n += classes[k].native_llvm_n_big;
  }
  nyt_subheading("Geomean ratios · all comparable | C >= 1ms");
  const size_t gm_widths[] = {15, 30, 30, 30};
  const char *gm_headers[] = {"class", "NYR/C: all | >=1ms",
                              "LLVM/C: all | >=1ms", "LLVM/NYR: all | >=1ms"};
  bench_grid_rule(gm_widths, 4);
  bench_grid_row(gm_headers, gm_widths, 4);
  bench_grid_rule(gm_widths, 4);
  for (size_t k = 0; k < nclasses; k++) {
    char nc[64], lc[64], nl[64];
    char nc_all[32], nc_big[32], lc_all[32], lc_big[32], nl_all[32], nl_big[32];
    bench_geomean_text(nc_all, sizeof(nc_all), classes[k].native_c_log_all,
                       classes[k].native_c_n_all);
    bench_geomean_text(nc_big, sizeof(nc_big), classes[k].native_c_log_big,
                       classes[k].native_c_n_big);
    bench_geomean_text(lc_all, sizeof(lc_all), classes[k].llvm_c_log_all,
                       classes[k].llvm_c_n_all);
    bench_geomean_text(lc_big, sizeof(lc_big), classes[k].llvm_c_log_big,
                       classes[k].llvm_c_n_big);
    bench_geomean_text(nl_all, sizeof(nl_all), classes[k].native_llvm_log_all,
                       classes[k].native_llvm_n_all);
    bench_geomean_text(nl_big, sizeof(nl_big), classes[k].native_llvm_log_big,
                       classes[k].native_llvm_n_big);
    snprintf(nc, sizeof(nc), "%s | %s", nc_all, nc_big);
    snprintf(lc, sizeof(lc), "%s | %s", lc_all, lc_big);
    snprintf(nl, sizeof(nl), "%s | %s", nl_all, nl_big);
    const char *cells[] = {classes[k].name, nc, lc, nl};
    bench_grid_row(cells, gm_widths, 4);
  }
  char all_nc_s[64], all_lc_s[64], all_nl_s[64];
  char all_nc_all[32], all_nc_big[32], all_lc_all[32], all_lc_big[32],
      all_nl_all[32], all_nl_big[32];
  bench_geomean_text(all_nc_all, sizeof(all_nc_all), all_nc, all_nc_n);
  bench_geomean_text(all_nc_big, sizeof(all_nc_big), big_nc, big_nc_n);
  bench_geomean_text(all_lc_all, sizeof(all_lc_all), all_lc, all_lc_n);
  bench_geomean_text(all_lc_big, sizeof(all_lc_big), big_lc, big_lc_n);
  bench_geomean_text(all_nl_all, sizeof(all_nl_all), all_nl, all_nl_n);
  bench_geomean_text(all_nl_big, sizeof(all_nl_big), big_nl, big_nl_n);
  snprintf(all_nc_s, sizeof(all_nc_s), "%s | %s", all_nc_all, all_nc_big);
  snprintf(all_lc_s, sizeof(all_lc_s), "%s | %s", all_lc_all, all_lc_big);
  snprintf(all_nl_s, sizeof(all_nl_s), "%s | %s", all_nl_all, all_nl_big);
  const char *all_cells[] = {"overall", all_nc_s, all_lc_s, all_nl_s};
  bench_grid_rule(gm_widths, 4);
  bench_grid_row(all_cells, gm_widths, 4);
  bench_grid_rule(gm_widths, 4);
  nyt_subheading(enforce_budgets ? "Per-class geomean gates · C >= 1ms"
                                 : "Per-class geomean thresholds · advisory on "
                                   "partial or single-run sets");
  const char *headers[] = {"class",
                           "NYR/C (limit)",
                           "LLVM/C (limit)",
                           "LLVM/NYR (limit)",
                           "max LLVM/NYR (fixture)",
                           "result"};
  size_t widths[] = {
      bench_disp_width(headers[0]), bench_disp_width(headers[1]),
      bench_disp_width(headers[2]), bench_disp_width(headers[3]),
      bench_disp_width(headers[4]), bench_disp_width(headers[5])};
  const size_t budget_count =
      sizeof(bench_geomean_budgets) / sizeof(bench_geomean_budgets[0]);
  BenchGeomeanGateRow gate_rows[sizeof(bench_geomean_budgets) /
                                sizeof(bench_geomean_budgets[0])];
  int failed = 0;
  for (size_t b = 0; b < budget_count; ++b) {
    const BenchGeomeanBudget *budget = &bench_geomean_budgets[b];
    BenchGeomeanGateRow *row = &gate_rows[b];
    row->name = budget->name;
    size_t nc_n = 0, lc_n = 0, nl_n = 0;
    double nc_log = 0.0, lc_log = 0.0, nl_log = 0.0;
    double max_nl_ratio = 0.0;
    const char *max_nl_name = NULL;
    double max_nl_limit = BENCH_MAX_NATIVE_LLVM_RATIO;
    bool max_nl_failed = false;
    if (strcmp(budget->name, "overall") == 0) {
      nc_log = big_nc;
      lc_log = big_lc;
      nl_log = big_nl;
      nc_n = big_nc_n;
      lc_n = big_lc_n;
      nl_n = big_nl_n;
    } else {
      for (size_t k = 0; k < nclasses; ++k)
        if (strcmp(classes[k].name, budget->name) == 0) {
          nc_log = classes[k].native_c_log_big;
          lc_log = classes[k].llvm_c_log_big;
          nl_log = classes[k].native_llvm_log_big;
          nc_n = classes[k].native_c_n_big;
          lc_n = classes[k].llvm_c_n_big;
          nl_n = classes[k].native_llvm_n_big;
          break;
        }
    }
    /*
     * Fixture sample validity determines comparability. Ratios across
     * different workloads are heterogeneous and must not be trimmed merely
     * because a particular workload is much slower than its peers.
     */
    if (nc_n)
      nc_log /= (double)nc_n;
    if (lc_n)
      lc_log /= (double)lc_n;
    if (nl_n)
      nl_log /= (double)nl_n;
    for (size_t i = 0; i < t->len; ++i) {
      const BenchRow *r = &t->items[i];
      if (r->c_ms < 1.0 || !bench_native_llvm_comparable(r))
        continue;
      char cls[32];
      if (strcmp(budget->name, "overall") != 0 &&
          strcmp(bench_row_class(r, cls, sizeof(cls)), budget->name) != 0)
        continue;
      double ratio = r->ny_ms / r->llvm_ms;
      double ratio_limit = r->native_llvm_budget > 0.0
                               ? r->native_llvm_budget
                               : BENCH_MAX_NATIVE_LLVM_RATIO;
      if (ratio > ratio_limit)
        max_nl_failed = true;
      if (ratio > max_nl_ratio) {
        max_nl_ratio = ratio;
        max_nl_name = r->name;
        max_nl_limit = ratio_limit;
      }
    }
    bool row_failed = false;
    if (nc_n && exp(nc_log) > budget->native_c_max)
      row_failed = true;
    if (lc_n && exp(lc_log) > budget->llvm_c_max)
      row_failed = true;
    if (nl_n && exp(nl_log) > budget->native_llvm_max)
      row_failed = true;
    row_failed |= max_nl_failed;
    failed |= enforce_budgets && row_failed;
    row->failed = row_failed;
    char nc_ratio[32], lc_ratio[32], nl_ratio[32];
    if (nc_n)
      bench_ratio_text(nc_ratio, sizeof(nc_ratio), exp(nc_log));
    else
      snprintf(nc_ratio, sizeof(nc_ratio), "—");
    if (lc_n)
      bench_ratio_text(lc_ratio, sizeof(lc_ratio), exp(lc_log));
    else
      snprintf(lc_ratio, sizeof(lc_ratio), "—");
    if (nl_n)
      bench_ratio_text(nl_ratio, sizeof(nl_ratio), exp(nl_log));
    else
      snprintf(nl_ratio, sizeof(nl_ratio), "—");
    snprintf(row->native_c, sizeof(row->native_c), "%s (<= %.2fx)", nc_ratio,
             budget->native_c_max);
    snprintf(row->llvm_c, sizeof(row->llvm_c), "%s (<= %.2fx)", lc_ratio,
             budget->llvm_c_max);
    snprintf(row->native_llvm, sizeof(row->native_llvm), "%s (<= %.2fx)",
             nl_ratio, budget->native_llvm_max);
    if (max_nl_name) {
      char ratio[32];
      bench_ratio_text(ratio, sizeof(ratio), max_nl_ratio);
      snprintf(row->max_native_llvm, sizeof(row->max_native_llvm),
               "%s (%s; <= %.2fx)", ratio, max_nl_name, max_nl_limit);
    } else {
      snprintf(row->max_native_llvm, sizeof(row->max_native_llvm),
               "— (<= %.2fx)", BENCH_MAX_NATIVE_LLVM_RATIO);
    }
    size_t row_widths[] = {
        bench_disp_width(row->name),
        bench_disp_width(row->native_c),
        bench_disp_width(row->llvm_c),
        bench_disp_width(row->native_llvm),
        bench_disp_width(row->max_native_llvm),
        bench_disp_width(enforce_budgets ? (row->failed ? "FAIL" : "pass")
                                         : "advisory")};
    for (size_t col = 0; col < sizeof(widths) / sizeof(widths[0]); ++col)
      if (row_widths[col] > widths[col])
        widths[col] = row_widths[col];
  }
  bench_grid_rule(widths, sizeof(widths) / sizeof(widths[0]));
  bench_grid_row(headers, widths, sizeof(widths) / sizeof(widths[0]));
  bench_grid_rule(widths, sizeof(widths) / sizeof(widths[0]));
  for (size_t b = 0; b < budget_count; ++b) {
    const BenchGeomeanGateRow *row = &gate_rows[b];
    const char *result =
        enforce_budgets ? (row->failed ? "FAIL" : "pass") : "advisory";
    const char *cells[] = {row->name,        row->native_c,        row->llvm_c,
                           row->native_llvm, row->max_native_llvm, result};
    bench_grid_row(cells, widths, sizeof(widths) / sizeof(widths[0]));
  }
  bench_grid_rule(widths, sizeof(widths) / sizeof(widths[0]));
  return failed;
}

/*
 * Tiny kernels can be dominated by launch/runtime setup. The `startup`
 * fixture measures that floor independently for native, LLVM, and C using
 * the same benchmark runner. Keep the raw ratios above, and also report
 * ratios after subtracting those measured floors from both sides.
 */
typedef struct {
  char name[32];
  double native_c_log;
  double llvm_c_log;
  double native_llvm_log;
  size_t native_c_n;
  size_t llvm_c_n;
  size_t native_llvm_n;
} BenchFloorClass;

static void bench_print_floor_adjusted_geomean(const BenchTable *t) {
  if (!t || t->len == 0)
    return;
  const BenchRow *floor = NULL;
  for (size_t i = 0; i < t->len; ++i)
    if (strcmp(t->items[i].name, "startup") == 0) {
      floor = &t->items[i];
      break;
    }
  if (!floor || floor->ny_ms <= 0.0 || floor->llvm_ms <= 0.0 ||
      floor->c_ms <= 0.0) {
    nyt_subheading("Floor-adjusted ratios");
    puts("  unavailable: the benchmark set needs a successful `startup` "
         "floor fixture");
    return;
  }

  BenchFloorClass classes[BENCH_GEOMEAN_CLASSES];
  size_t nclasses = 0;
  memset(classes, 0, sizeof(classes));
  for (size_t i = 0; i < t->len; ++i) {
    const BenchRow *r = &t->items[i];
    if (r == floor)
      continue;
    char cls[32];
    const char *cname = bench_row_class(r, cls, sizeof(cls));
    BenchFloorClass *c = NULL;
    for (size_t k = 0; k < nclasses; ++k)
      if (strcmp(classes[k].name, cname) == 0) {
        c = &classes[k];
        break;
      }
    if (!c) {
      if (nclasses >= BENCH_GEOMEAN_CLASSES)
        continue;
      c = &classes[nclasses++];
      snprintf(c->name, sizeof(c->name), "%s", cname);
    }
    double ny = r->ny_ms - floor->ny_ms;
    double llvm = r->llvm_ms - floor->llvm_ms;
    double host = r->c_ms - floor->c_ms;
    if (ny > 0.0 && host > 0.0 && bench_native_c_comparable(r)) {
      c->native_c_log += log(ny / host);
      c->native_c_n++;
    }
    if (llvm > 0.0 && host > 0.0 && bench_llvm_c_comparable(r)) {
      c->llvm_c_log += log(llvm / host);
      c->llvm_c_n++;
    }
    if (ny > 0.0 && llvm > 0.0 && bench_native_llvm_comparable(r)) {
      c->native_llvm_log += log(ny / llvm);
      c->native_llvm_n++;
    }
  }
  nyt_subheading("Geomean ratios · measured startup floor subtracted");
  printf("  floor: NYR %.3fms · LLVM %.3fms · C %.3fms\n", floor->ny_ms,
         floor->llvm_ms, floor->c_ms);
  const size_t floor_widths[] = {15, 18, 18, 18};
  const char *floor_headers[] = {"class", "NYR/C", "LLVM/C", "LLVM/NYR"};
  bench_grid_rule(floor_widths, 4);
  bench_grid_row(floor_headers, floor_widths, 4);
  bench_grid_rule(floor_widths, 4);
  double nc_log = 0.0, lc_log = 0.0, nl_log = 0.0;
  size_t nc_n = 0, lc_n = 0, nl_n = 0;
  for (size_t k = 0; k < nclasses; ++k) {
    char nc[48], lc[48], nl[48];
    BenchFloorClass *c = &classes[k];
    bench_geomean_text(nc, sizeof(nc), c->native_c_log, c->native_c_n);
    bench_geomean_text(lc, sizeof(lc), c->llvm_c_log, c->llvm_c_n);
    bench_geomean_text(nl, sizeof(nl), c->native_llvm_log, c->native_llvm_n);
    const char *cells[] = {c->name, nc, lc, nl};
    bench_grid_row(cells, floor_widths, 4);
    nc_log += c->native_c_log;
    lc_log += c->llvm_c_log;
    nl_log += c->native_llvm_log;
    nc_n += c->native_c_n;
    lc_n += c->llvm_c_n;
    nl_n += c->native_llvm_n;
  }
  char nc[48], lc[48], nl[48];
  bench_geomean_text(nc, sizeof(nc), nc_log, nc_n);
  bench_geomean_text(lc, sizeof(lc), lc_log, lc_n);
  bench_geomean_text(nl, sizeof(nl), nl_log, nl_n);
  const char *all_cells[] = {"overall", nc, lc, nl};
  bench_grid_rule(floor_widths, 4);
  bench_grid_row(all_cells, floor_widths, 4);
  bench_grid_rule(floor_widths, 4);
}

static void bench_print_hw_counters(const BenchTable *t) {
  if (!t || !bench_hw_counters_requested())
    return;
  bool any = false;
  for (size_t i = 0; i < t->len; ++i)
    any |= t->items[i].ny_hw_counters_seen || t->items[i].llvm_hw_counters_seen;
  if (!any)
    return;
  nyt_subheading("Hardware counters · checksum-validated AOT runs");
  printf("  %-20s  %-38s  %-38s  %s\n", "fixture",
         "NYR: cycles / insns / br-miss / cache-miss",
         "LLVM: cycles / insns / br-miss / cache-miss", "cycles");
  for (size_t i = 0; i < t->len; ++i) {
    const BenchRow *r = &t->items[i];
    if (!r->ny_hw_counters_seen && !r->llvm_hw_counters_seen)
      continue;
    char native[96], llvm[96], ratio[16];
    if (r->ny_hw_counters_seen)
      snprintf(native, sizeof(native),
               "%" PRIu64 " / %" PRIu64 " / %" PRIu64 " / %" PRIu64,
               r->ny_hw_cycles, r->ny_hw_instructions, r->ny_hw_branch_misses,
               r->ny_hw_cache_misses);
    else
      snprintf(native, sizeof(native), "n/a");
    if (r->llvm_hw_counters_seen)
      snprintf(llvm, sizeof(llvm),
               "%" PRIu64 " / %" PRIu64 " / %" PRIu64 " / %" PRIu64,
               r->llvm_hw_cycles, r->llvm_hw_instructions,
               r->llvm_hw_branch_misses, r->llvm_hw_cache_misses);
    else
      snprintf(llvm, sizeof(llvm), "n/a");
    if (r->ny_hw_counters_seen && r->llvm_hw_counters_seen &&
        r->llvm_hw_cycles > 0 && r->llvm_hw_cycles < r->ny_hw_cycles)
      snprintf(ratio, sizeof(ratio), "%.2fx",
               (double)r->ny_hw_cycles / (double)r->llvm_hw_cycles);
    else
      snprintf(ratio, sizeof(ratio), "—");
    printf("  %-20s  %-38s  %-38s  %s\n", r->name, native, llvm, ratio);
  }
}

static void bench_write_csv(FILE *f, const BenchTable *t,
                            const BenchEnvironment *env) {
  fprintf(
      f,
      "benchmark,backend,engine,cache,status,checksum,ny_checksum,llvm_"
      "checksum,c_checksum,checksum_ok,ny_native_ms,ny_compile_ms,ny_link_ms,"
      "ny_opt_ms,ny_total_ms,ny_jit_compile_ms,ny_jit_run_ms,ny_code_bytes,ny_"
      "code_bytes_"
      "seen,"
      "ny_specialization_code_bytes,"
      "ny_specialization_code_functions,ny_specialization_max_function_bytes,"
      "ny_specialization_metrics_seen,ny_peak_compiler_rss_kb,ny_peak_compiler_"
      "rss_seen,ny_llvm_ms,llvm_total_ms,c_ms,c_total_ms,driver_wall_ms,native_"
      "ratio,llvm_ratio,native_llvm_ratio,native_ratio_valid,llvm_ratio_valid,"
      "native_llvm_ratio_valid,ny_rc,llvm_rc,c_rc,"
      "ny_failures,llvm_failures,c_"
      "failures,ny_runs,llvm_runs,c_runs,ny_min,ny_max,ny_p95,ny_dispersion_"
      "pct,ny_noise_pct,ny_unstable,llvm_min,llvm_max,llvm_p95,llvm_dispersion_"
      "pct,llvm_noise_pct,llvm_unstable,c_min,c_max,c_p95,c_dispersion_pct,c_"
      "noise_pct,c_unstable,native_c_budget,native_llvm_budget,compile_ms_"
      "budget,code_bytes_budget,specialization_code_bytes_budget,"
      "specialization_function_bytes_budget,compiler_rss_budget_kb,ny_static_"
      "metrics_seen,ny_runtime_calls,ny_dynamic_ops,ny_tag_checks,ny_box_unbox,"
      "ny_heap_allocations,ny_bounds_checks,ny_direct_calls,ny_indirect_calls,"
      "ny_unknown_effects,ny_alias_unresolved,ny_vector_attempted,ny_vector_"
      "rejected,ny_vectorized,ny_spills,ny_reloads,ny_hw_counters_seen,ny_hw_"
      "cycles,ny_hw_instructions,ny_hw_branches,ny_hw_branch_misses,ny_hw_"
      "cache_misses,llvm_hw_counters_seen,llvm_hw_cycles,llvm_hw_instructions,"
      "llvm_hw_branches,llvm_hw_branch_misses,llvm_hw_cache_misses,ny_runtime_"
      "counters_seen,ny_runtime_alloc_count,ny_runtime_realloc_count,static_"
      "budget_mask,heap_allocations_budget,runtime_calls_budget,bounds_checks_"
      "budget,indirect_calls_budget,unknown_effects_budget,alias_unresolved_"
      "budget,spills_budget,reloads_budget,budget_failed,ny_timing_source,llvm_"
      "timing_source,c_timing_source,os,arch,cpu_model,target,target_features,"
      "compiler_binary,"
      "compiler_binary_fnv64,compiler_revision,c_compiler,c_compiler_version,c_"
      "flags,opt_level,profile,tier,engine,cache_setting,samples,warmups,"
      "fixture_workers,route_workers,runtime_counters_enabled\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    int native_ratio_valid = bench_native_c_comparable(r);
    int llvm_ratio_valid = bench_llvm_c_comparable(r);
    int native_llvm_ratio_valid = bench_native_llvm_comparable(r);
    double native_ratio = native_ratio_valid ? r->ny_ms / r->c_ms : 0.0;
    double llvm_ratio = llvm_ratio_valid ? r->llvm_ms / r->c_ms : 0.0;
    double native_llvm_ratio =
        native_llvm_ratio_valid ? r->ny_ms / r->llvm_ms : 0.0;
    const char *status = bench_row_status(r);
    fprintf(
        f,
        "%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%zu,%d,%zu,%zu,%zu,%d,%zu,%d,%.4f,%"
        ".4f,%"
        ".4f,%.4f,%"
        ".4f,%.4f,%.4f,%.4f,%d,%d,%d,"
        "%d,%d,%d,%d,%d,%d,%d,%d,%d,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
        "%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%zu,%zu,%zu,%zu,"
        "%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
        "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
        "%d,%zu,%zu,"
        "%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%d,%s,%s,%s,",
        r->name, r->backend, r->engine, r->cache, status, r->checksum,
        r->ny_checksum, r->llvm_checksum, r->c_checksum, r->checksum_ok,
        r->ny_ms, r->ny_compile_ms, r->ny_link_ms, r->ny_opt_ms, r->ny_total_ms,
        r->ny_jit_compile_ms, r->ny_jit_run_ms, r->ny_code_bytes,
        r->ny_code_bytes_seen, r->ny_specialization_code_bytes,
        r->ny_specialization_code_functions,
        r->ny_specialization_max_function_bytes,
        r->ny_specialization_metrics_seen, r->ny_peak_compiler_rss_kb,
        r->ny_peak_compiler_rss_seen, r->llvm_ms, r->llvm_total_ms, r->c_ms,
        r->c_total_ms, r->wall_ms, native_ratio, llvm_ratio, native_llvm_ratio,
        native_ratio_valid, llvm_ratio_valid, native_llvm_ratio_valid, r->ny_rc,
        r->llvm_rc, r->c_rc, r->ny_failures, r->llvm_failures, r->c_failures,
        r->ny_runs, r->llvm_runs, r->c_runs, r->ny_min, r->ny_max, r->ny_p95,
        r->ny_dispersion_pct, r->ny_noise_pct, r->ny_unstable, r->llvm_min,
        r->llvm_max, r->llvm_p95, r->llvm_dispersion_pct, r->llvm_noise_pct,
        r->llvm_unstable, r->c_min, r->c_max, r->c_p95, r->c_dispersion_pct,
        r->c_noise_pct, r->c_unstable, r->native_c_budget,
        r->native_llvm_budget, r->compile_ms_budget, r->code_bytes_budget,
        r->specialization_code_bytes_budget,
        r->specialization_function_bytes_budget, r->compiler_rss_budget_kb,
        r->ny_static_metrics_seen, r->ny_runtime_calls, r->ny_dynamic_ops,
        r->ny_tag_checks, r->ny_box_unbox, r->ny_heap_allocations,
        r->ny_bounds_checks, r->ny_direct_calls, r->ny_indirect_calls,
        r->ny_unknown_effects, r->ny_alias_unresolved, r->ny_vector_attempted,
        r->ny_vector_rejected, r->ny_vectorized, r->ny_spills, r->ny_reloads,
        r->ny_hw_counters_seen, r->ny_hw_cycles, r->ny_hw_instructions,
        r->ny_hw_branches, r->ny_hw_branch_misses, r->ny_hw_cache_misses,
        r->llvm_hw_counters_seen, r->llvm_hw_cycles, r->llvm_hw_instructions,
        r->llvm_hw_branches, r->llvm_hw_branch_misses, r->llvm_hw_cache_misses,
        r->ny_runtime_counters_seen, r->ny_runtime_alloc_count,
        r->ny_runtime_realloc_count, r->static_budget_mask,
        r->heap_allocations_budget, r->runtime_calls_budget,
        r->bounds_checks_budget, r->indirect_calls_budget,
        r->unknown_effects_budget, r->alias_unresolved_budget, r->spills_budget,
        r->reloads_budget, r->budget_failed,
        r->ny_runs <= 0 ? "unavailable"
                        : (r->ny_fixture_timer ? "fixture-kernel"
                                               : (r->ny_timer_fallback
                                                      ? "fixture_timer_fallback"
                                                      : "ny--time")),
        r->llvm_runs <= 0
            ? "unavailable"
            : (r->llvm_fixture_timer
                   ? "fixture-kernel"
                   : (r->llvm_timer_fallback ? "fixture_timer_fallback"
                                             : "ny--time")),
        r->c_runs <= 0 ? "unavailable"
                       : (r->c_fixture_timer ? "fixture-kernel"
                                             : "generated-main-wrapper"));
    bench_csv_string(f, env ? env->os : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->arch : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->cpu_model : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->target : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->target_features : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->compiler_binary : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->compiler_binary_fnv64 : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->compiler_revision : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->c_compiler : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->c_compiler_version : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->c_flags : "unknown");
    fputc(',', f);
    bench_csv_string(f, env ? env->opt_level : "default");
    fputc(',', f);
    bench_csv_string(f, env ? env->profile : "default");
    fputc(',', f);
    bench_csv_string(f, env ? env->tier : "default");
    fputc(',', f);
    bench_csv_string(f, env ? env->engine : "default");
    fputc(',', f);
    bench_csv_string(f, env ? env->cache : "default");
    fprintf(f, ",%d,%d,%d,%d,%d\n", env ? env->samples : 0,
            env ? env->warmups : 0, env ? env->fixture_workers : 0,
            env ? env->route_workers : 0,
            env ? env->runtime_counters_enabled : 0);
  }
}

static void bench_json_string_field(FILE *f, const char *name,
                                    const char *value) {
  fprintf(f, "\"%s\":\"", name);
  for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p;
       ++p) {
    char escaped[7];
    size_t n = ny_json_escape_byte(*p, escaped);
    (void)fwrite(escaped, 1, n, f);
  }
  fputs("\",", f);
}

static void bench_write_json(FILE *f, const BenchTable *t,
                             const BenchEnvironment *env) {
  fprintf(f, "{\n");
  if (env) {
    fputs("  \"environment\": {", f);
    bench_json_string_field(f, "os", env->os);
    bench_json_string_field(f, "arch", env->arch);
    bench_json_string_field(f, "cpu_model", env->cpu_model);
    bench_json_string_field(f, "target", env->target);
    bench_json_string_field(f, "target_features", env->target_features);
    bench_json_string_field(f, "compiler_binary", env->compiler_binary);
    bench_json_string_field(f, "compiler_binary_fnv64",
                            env->compiler_binary_fnv64);
    bench_json_string_field(f, "c_compiler", env->c_compiler);
    bench_json_string_field(f, "c_compiler_version", env->c_compiler_version);
    bench_json_string_field(f, "c_flags", env->c_flags);
    bench_json_string_field(f, "compiler_revision", env->compiler_revision);
    bench_json_string_field(f, "opt_level", env->opt_level);
    bench_json_string_field(f, "profile", env->profile);
    bench_json_string_field(f, "tier", env->tier);
    bench_json_string_field(f, "engine", env->engine);
    bench_json_string_field(f, "cache", env->cache);
    fprintf(f,
            "\"fixture_workers\":%d,\"route_workers\":%d,"
            "\"measurement_mode\":\"%s\","
            "\"timing_source_scope\":\"per-row\","
            "\"runtime_counters_enabled\":%s,"
            "\"samples\":%d,\"warmups\":%d},\n",
            env->fixture_workers, env->route_workers,
            env->fixture_workers > 1 ? "parallel-fixtures-sequential-routes"
                                     : "isolated-serial",
            env->runtime_counters_enabled ? "true" : "false", env->samples,
            env->warmups);
  }
  fprintf(f, "  \"noise_threshold_pct\": %.3f,\n", bench_noise_limit_pct());
  fprintf(f, "  \"benchmarks\": [\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    int native_ratio_valid = bench_native_c_comparable(r);
    int llvm_ratio_valid = bench_llvm_c_comparable(r);
    int native_llvm_ratio_valid = bench_native_llvm_comparable(r);
    double native_ratio = native_ratio_valid ? r->ny_ms / r->c_ms : 0.0;
    double llvm_ratio = llvm_ratio_valid ? r->llvm_ms / r->c_ms : 0.0;
    double native_llvm_ratio =
        native_llvm_ratio_valid ? r->ny_ms / r->llvm_ms : 0.0;
    const char *status = bench_row_status(r);
    fputs("    {", f);
    bench_json_string_field(f, "name", r->name);
    bench_json_string_field(f, "backend", r->backend);
    bench_json_string_field(f, "engine", r->engine);
    bench_json_string_field(f, "cache", r->cache);
    bench_json_string_field(f, "status", status);
    bench_json_string_field(f, "checksum", r->checksum);
    bench_json_string_field(f, "ny_checksum", r->ny_checksum);
    bench_json_string_field(f, "llvm_checksum", r->llvm_checksum);
    bench_json_string_field(f, "c_checksum", r->c_checksum);
    fprintf(f, "\"checksum_ok\":%s,", r->checksum_ok ? "true" : "false");
    fprintf(
        f,
        "\"ny_native_ms\":%.4f,\"ny_compile_ms\":%.4f,\"ny_link_ms\":%.4f,"
        "\"ny_opt_ms\":%.4f,"
        "\"ny_total_ms\":%.4f,\"ny_jit_compile_ms\":%.4f,"
        "\"ny_jit_run_ms\":%.4f,\"ny_code_bytes\":%zu,\"ny_code_bytes_seen\":%"
        "s,"
        "\"ny_specialization_code_bytes\":%zu,\"ny_specialization_code_"
        "functions\":%zu,"
        "\"ny_specialization_max_function_bytes\":%zu,\"ny_specialization_"
        "metrics_seen\":%s,"
        "\"ny_peak_compiler_rss_kb\":%zu,\"ny_peak_compiler_rss_seen\":%s,"
        "\"ny_static_metrics_seen\":%s,\"ny_runtime_calls\":%zu,"
        "\"ny_dynamic_ops\":%zu,\"ny_tag_checks\":%zu,"
        "\"ny_box_unbox\":%zu,\"ny_heap_allocations\":%zu,"
        "\"ny_bounds_checks\":%zu,\"ny_direct_calls\":%zu,"
        "\"ny_indirect_calls\":%zu,\"ny_unknown_effects\":%zu,"
        "\"ny_alias_unresolved\":%zu,\"ny_vector_attempted\":%zu,"
        "\"ny_vector_rejected\":%zu,\"ny_vectorized\":%zu,"
        "\"ny_spills\":%zu,\"ny_reloads\":%zu,"
        "\"ny_hw_counters_seen\":%s,\"ny_hw_cycles\":%" PRIu64 ","
        "\"ny_hw_instructions\":%" PRIu64 ",\"ny_hw_branches\":%" PRIu64 ","
        "\"ny_hw_branch_misses\":%" PRIu64 ",\"ny_hw_cache_misses\":%" PRIu64
        ","
        "\"llvm_hw_counters_seen\":%s,\"llvm_hw_cycles\":%" PRIu64 ","
        "\"llvm_hw_instructions\":%" PRIu64 ",\"llvm_hw_branches\":%" PRIu64 ","
        "\"llvm_hw_branch_misses\":%" PRIu64
        ",\"llvm_hw_cache_misses\":%" PRIu64 ","
        "\"ny_runtime_counters_seen\":%s,\"ny_runtime_alloc_count\":%zu,"
        "\"ny_runtime_realloc_count\":%zu,"
        "\"ny_llvm_ms\":%.4f,\"llvm_total_ms\":%.4f,"
        "\"c_ms\":%.4f,\"c_total_ms\":%.4f,\"driver_wall_ms\":%.4f,"
        "\"native_ratio\":%.4f,\"llvm_ratio\":%.4f,"
        "\"native_llvm_ratio\":%.4f,\"native_ratio_valid\":%s,"
        "\"llvm_ratio_valid\":%s,\"native_llvm_ratio_valid\":%s,",
        r->ny_ms, r->ny_compile_ms, r->ny_link_ms, r->ny_opt_ms, r->ny_total_ms,
        r->ny_jit_compile_ms, r->ny_jit_run_ms, r->ny_code_bytes,
        r->ny_code_bytes_seen ? "true" : "false",
        r->ny_specialization_code_bytes, r->ny_specialization_code_functions,
        r->ny_specialization_max_function_bytes,
        r->ny_specialization_metrics_seen ? "true" : "false",
        r->ny_peak_compiler_rss_kb,
        r->ny_peak_compiler_rss_seen ? "true" : "false",
        r->ny_static_metrics_seen ? "true" : "false", r->ny_runtime_calls,
        r->ny_dynamic_ops, r->ny_tag_checks, r->ny_box_unbox,
        r->ny_heap_allocations, r->ny_bounds_checks, r->ny_direct_calls,
        r->ny_indirect_calls, r->ny_unknown_effects, r->ny_alias_unresolved,
        r->ny_vector_attempted, r->ny_vector_rejected, r->ny_vectorized,
        r->ny_spills, r->ny_reloads, r->ny_hw_counters_seen ? "true" : "false",
        r->ny_hw_cycles, r->ny_hw_instructions, r->ny_hw_branches,
        r->ny_hw_branch_misses, r->ny_hw_cache_misses,
        r->llvm_hw_counters_seen ? "true" : "false", r->llvm_hw_cycles,
        r->llvm_hw_instructions, r->llvm_hw_branches, r->llvm_hw_branch_misses,
        r->llvm_hw_cache_misses, r->ny_runtime_counters_seen ? "true" : "false",
        r->ny_runtime_alloc_count, r->ny_runtime_realloc_count, r->llvm_ms,
        r->llvm_total_ms, r->c_ms, r->c_total_ms, r->wall_ms, native_ratio,
        llvm_ratio, native_llvm_ratio, native_ratio_valid ? "true" : "false",
        llvm_ratio_valid ? "true" : "false",
        native_llvm_ratio_valid ? "true" : "false");
    fprintf(f,
            "\"ny_rc\":%d,\"llvm_rc\":%d,\"c_rc\":%d,\"ny_failures\":%d,"
            "\"llvm_failures\":%d,\"c_failures\":%d,\"ny_runs\":%d,"
            "\"llvm_runs\":%d,\"c_runs\":%d,",
            r->ny_rc, r->llvm_rc, r->c_rc, r->ny_failures, r->llvm_failures,
            r->c_failures, r->ny_runs, r->llvm_runs, r->c_runs);
    fprintf(
        f,
        "\"ny_min\":%.4f,\"ny_max\":%.4f,\"ny_p95\":%.4f,"
        "\"ny_dispersion_pct\":%.4f,\"ny_noise_pct\":%.4f,\"ny_unstable\":%s,"
        "\"llvm_min\":%.4f,\"llvm_max\":%.4f,\"llvm_p95\":%.4f,"
        "\"llvm_dispersion_pct\":%.4f,\"llvm_noise_pct\":%.4f,\"llvm_"
        "unstable\":%s,"
        "\"c_min\":%.4f,\"c_max\":%.4f,\"c_p95\":%.4f,"
        "\"c_dispersion_pct\":%.4f,\"c_noise_pct\":%.4f,\"c_unstable\":%s,"
        "\"native_c_budget\":%.4f,\"native_llvm_budget\":%.4f,"
        "\"compile_ms_budget\":%.4f,\"code_bytes_budget\":%zu,"
        "\"specialization_code_bytes_budget\":%zu,"
        "\"specialization_function_bytes_budget\":%zu,"
        "\"compiler_rss_budget_kb\":%zu,\"static_budget_mask\":%u,"
        "\"heap_allocations_budget\":%zu,\"runtime_calls_budget\":%zu,"
        "\"bounds_checks_budget\":%zu,\"indirect_calls_budget\":%zu,"
        "\"unknown_effects_budget\":%zu,\"alias_unresolved_budget\":%zu,"
        "\"spills_budget\":%zu,\"reloads_budget\":%zu,"
        "\"budget_failed\":%s,\"ny_timing_source\":\"%s\","
        "\"llvm_timing_source\":\"%s\",\"c_timing_source\":\"%s\","
        "\"ny_timer_fallback\":%s,"
        "\"llvm_timer_fallback\":%s}%s\n",
        r->ny_min, r->ny_max, r->ny_p95, r->ny_dispersion_pct, r->ny_noise_pct,
        r->ny_unstable ? "true" : "false", r->llvm_min, r->llvm_max,
        r->llvm_p95, r->llvm_dispersion_pct, r->llvm_noise_pct,
        r->llvm_unstable ? "true" : "false", r->c_min, r->c_max, r->c_p95,
        r->c_dispersion_pct, r->c_noise_pct, r->c_unstable ? "true" : "false",
        r->native_c_budget, r->native_llvm_budget, r->compile_ms_budget,
        r->code_bytes_budget, r->specialization_code_bytes_budget,
        r->specialization_function_bytes_budget, r->compiler_rss_budget_kb,
        r->static_budget_mask, r->heap_allocations_budget,
        r->runtime_calls_budget, r->bounds_checks_budget,
        r->indirect_calls_budget, r->unknown_effects_budget,
        r->alias_unresolved_budget, r->spills_budget, r->reloads_budget,
        r->budget_failed ? "true" : "false",
        r->ny_runs <= 0 ? "unavailable"
                        : (r->ny_fixture_timer ? "fixture-kernel"
                                               : (r->ny_timer_fallback
                                                      ? "fixture_timer_fallback"
                                                      : "ny--time")),
        r->llvm_runs <= 0
            ? "unavailable"
            : (r->llvm_fixture_timer
                   ? "fixture-kernel"
                   : (r->llvm_timer_fallback ? "fixture_timer_fallback"
                                             : "ny--time")),
        r->c_runs <= 0 ? "unavailable"
                       : (r->c_fixture_timer ? "fixture-kernel"
                                             : "generated-main-wrapper"),
        r->ny_timer_fallback ? "true" : "false",
        r->llvm_timer_fallback ? "true" : "false", (i + 1 < t->len) ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
}

static void bench_write_md(FILE *f, const BenchTable *t,
                           const BenchEnvironment *env) {
  if (env) {
    fprintf(f, "- OS/arch: `%s` / `%s`\n", env->os, env->arch);
    fprintf(f, "- CPU: `%s`\n", env->cpu_model);
    fprintf(f, "- target: `%s`\n", env->target);
    fprintf(f, "- target features: `%s`\n", env->target_features);
    fprintf(f, "- compiler binary: `%s`\n", env->compiler_binary);
    fprintf(f, "- compiler binary fingerprint: `%s`\n",
            env->compiler_binary_fnv64);
    fprintf(f, "- C compiler: `%s` — `%s`\n", env->c_compiler,
            env->c_compiler_version);
    fprintf(f, "- C flags: `%s`\n", env->c_flags);
    fprintf(f, "- compiler revision: `%s`\n", env->compiler_revision);
    fprintf(f,
            "- Ny settings: opt `%s`, profile `%s`, tier `%s`, engine `%s`, "
            "cache `%s`\n",
            env->opt_level, env->profile, env->tier,
            bench_engine_label(env->engine), env->cache);
    fprintf(f, "- samples: %d measured + %d warm-up\n", env->samples,
            env->warmups);
    fprintf(f, "- runtime-counter collection: %s\n",
            env->runtime_counters_enabled ? "enabled" : "disabled");
    fprintf(f,
            "- measurement mode: %d fixture worker(s), routes sequential per "
            "fixture; Ny `-time` phase and generated C-main wrapper\n",
            env->fixture_workers);
    fprintf(
        f,
        "- unstable threshold: scaled MAD > %.1f%% of median (5+ samples)\n\n",
        bench_noise_limit_pct());
  }
  fprintf(f, "| benchmark | backend | engine | status | NYR ms | NYR p95 "
             "| Ny noise | Ny code bytes | mono bytes | mono max fn bytes | "
             "compiler peak RSS KiB | Ny LLVM ms | LLVM p95 | LLVM noise | C "
             "ms | C p95 | C noise | NYR/C | LLVM/NYR |\n");
  fprintf(f, "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | "
             "---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
             "---: | ---: | ---: |\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    int native_ratio_valid = bench_native_c_comparable(r);
    int native_llvm_valid = bench_native_llvm_comparable(r);
    char native_ratio[32], native_llvm[32];
    if (native_ratio_valid)
      bench_ratio_text(native_ratio, sizeof(native_ratio), r->ny_ms / r->c_ms);
    else
      snprintf(native_ratio, sizeof(native_ratio), "—");
    if (native_llvm_valid)
      bench_ratio_text(native_llvm, sizeof(native_llvm), r->ny_ms / r->llvm_ms);
    else
      snprintf(native_llvm, sizeof(native_llvm), "—");
    const char *status = bench_row_status(r);
    fprintf(
        f,
        "| %s | %s | %s | %s | %.3f | %.3f | %.2f%% | %zu | %zu | %zu | %zu | "
        "%.3f | %.3f | %.2f%% | %.3f | %.3f | %.2f%% | %s | %s |\n",
        r->name, r->backend, bench_engine_label(r->engine), status, r->ny_ms,
        r->ny_p95, r->ny_noise_pct,
        r->ny_code_bytes_seen ? r->ny_code_bytes : 0,
        r->ny_specialization_metrics_seen ? r->ny_specialization_code_bytes : 0,
        r->ny_specialization_metrics_seen
            ? r->ny_specialization_max_function_bytes
            : 0,
        r->ny_peak_compiler_rss_seen ? r->ny_peak_compiler_rss_kb : 0,
        r->llvm_ms, r->llvm_p95, r->llvm_noise_pct, r->c_ms, r->c_p95,
        r->c_noise_pct, native_ratio, native_llvm);
  }
  fprintf(f, "\n| benchmark | alloc sites | runtime allocs | runtime reallocs "
             "| runtime helpers | bounds checks | dynamic ops | indirect calls "
             "| unknown effects | alias unresolved | spills | reloads | "
             "vectorized/attempted | NYR cycles | LLVM cycles | NYR "
             "instructions | LLVM instructions | NYR branch misses | LLVM "
             "branch misses | NYR cache misses | LLVM cache misses |\n");
  fprintf(f, "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
             "---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | "
             "---: | ---: | ---: |\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    char runtime_allocs[32], runtime_reallocs[32];
    char hw_cycles[32], hw_instructions[32], hw_branch_misses[32],
        hw_cache_misses[32];
    char llvm_hw_cycles[32], llvm_hw_instructions[32];
    char llvm_hw_branch_misses[32], llvm_hw_cache_misses[32];
    if (r->ny_runtime_counters_seen) {
      snprintf(runtime_allocs, sizeof(runtime_allocs), "%zu",
               r->ny_runtime_alloc_count);
      snprintf(runtime_reallocs, sizeof(runtime_reallocs), "%zu",
               r->ny_runtime_realloc_count);
    } else {
      snprintf(runtime_allocs, sizeof(runtime_allocs), "n/a");
      snprintf(runtime_reallocs, sizeof(runtime_reallocs), "n/a");
    }
    if (r->ny_hw_counters_seen) {
      snprintf(hw_cycles, sizeof(hw_cycles), "%" PRIu64, r->ny_hw_cycles);
      snprintf(hw_instructions, sizeof(hw_instructions), "%" PRIu64,
               r->ny_hw_instructions);
      snprintf(hw_branch_misses, sizeof(hw_branch_misses), "%" PRIu64,
               r->ny_hw_branch_misses);
      snprintf(hw_cache_misses, sizeof(hw_cache_misses), "%" PRIu64,
               r->ny_hw_cache_misses);
    } else {
      snprintf(hw_cycles, sizeof(hw_cycles), "n/a");
      snprintf(hw_instructions, sizeof(hw_instructions), "n/a");
      snprintf(hw_branch_misses, sizeof(hw_branch_misses), "n/a");
      snprintf(hw_cache_misses, sizeof(hw_cache_misses), "n/a");
    }
    if (r->llvm_hw_counters_seen) {
      snprintf(llvm_hw_cycles, sizeof(llvm_hw_cycles), "%" PRIu64,
               r->llvm_hw_cycles);
      snprintf(llvm_hw_instructions, sizeof(llvm_hw_instructions), "%" PRIu64,
               r->llvm_hw_instructions);
      snprintf(llvm_hw_branch_misses, sizeof(llvm_hw_branch_misses), "%" PRIu64,
               r->llvm_hw_branch_misses);
      snprintf(llvm_hw_cache_misses, sizeof(llvm_hw_cache_misses), "%" PRIu64,
               r->llvm_hw_cache_misses);
    } else {
      snprintf(llvm_hw_cycles, sizeof(llvm_hw_cycles), "n/a");
      snprintf(llvm_hw_instructions, sizeof(llvm_hw_instructions), "n/a");
      snprintf(llvm_hw_branch_misses, sizeof(llvm_hw_branch_misses), "n/a");
      snprintf(llvm_hw_cache_misses, sizeof(llvm_hw_cache_misses), "n/a");
    }
    if (!r->ny_static_metrics_seen)
      fprintf(f,
              "| %s | n/a | %s | %s | n/a | n/a | n/a | n/a | n/a | n/a | n/a "
              "| n/a | n/a | %s | %s | %s | %s | %s | %s | %s | %s |\n",
              r->name, runtime_allocs, runtime_reallocs, hw_cycles,
              llvm_hw_cycles, hw_instructions, llvm_hw_instructions,
              hw_branch_misses, llvm_hw_branch_misses, hw_cache_misses,
              llvm_hw_cache_misses);
    else
      fprintf(f,
              "| %s | %zu | %s | %s | %zu | %zu | %zu | %zu | %zu | %zu | %zu "
              "| %zu | %zu/%zu | %s | %s | %s | %s | %s | %s | %s | %s |\n",
              r->name, r->ny_heap_allocations, runtime_allocs, runtime_reallocs,
              r->ny_runtime_calls, r->ny_bounds_checks, r->ny_dynamic_ops,
              r->ny_indirect_calls, r->ny_unknown_effects,
              r->ny_alias_unresolved, r->ny_spills, r->ny_reloads,
              r->ny_vectorized, r->ny_vector_attempted, hw_cycles,
              llvm_hw_cycles, hw_instructions, llvm_hw_instructions,
              hw_branch_misses, llvm_hw_branch_misses, hw_cache_misses,
              llvm_hw_cache_misses);
  }
}

/*
 * `--bench-report-md` augments the ordinary results report with source and
 * compiler artifacts for every selected fixture.
 */
static void bench_write_full_report(const char *path, const BenchTable *table,
                                    const char *bin, const char *opt_level,
                                    const char *tier, const char *engine,
                                    const char *target, int compare_llvm) {
  if (!path || !*path || !table)
    return;
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  (void)compare_llvm;
  fprintf(f, "\n# Compact fixture diagnostics\n\n"
             "Each section includes the fixture, extracted Ny source, NYIR "
             "optimization diagnostics, NYR assembly, native object "
             "disassembly, and generated C source/assembly. Large diagnostic "
             "blocks are capped at 12 KiB. LLVM artifacts are omitted.\n");
  for (size_t i = 0; i < table->len; i++) {
    const BenchRow *row = &table->items[i];
    char *fixture = read_whole_file(row->path);
    fprintf(f, "\n## %s\n\n", row->name);
    fprintf(f,
            "- Fixture: `%s` · result: **%s**\n"
            "- NYR: rc `%d`, failures `%d`, runs `%d`, checksum `%s`\n"
            "- C: rc `%d`, failures `%d`, runs `%d`, checksum `%s`\n\n",
            row->path, bench_row_status(row), row->ny_rc, row->ny_failures,
            row->ny_runs, row->ny_checksum, row->c_rc, row->c_failures,
            row->c_runs, row->c_checksum);
    if (fixture) {
      bench_report_block(f, "Fixture and test expectations", "text", fixture);
      free(fixture);
    } else {
      fprintf(f, "Fixture source unavailable: `%s`\n", row->path);
    }
    fflush(stdout);
    int saved_stdout = test_fd_dup(fileno(stdout));
    int null_fd = open("/dev/null", O_WRONLY);
    if (saved_stdout >= 0 && null_fd >= 0)
      test_fd_dup2(null_fd, fileno(stdout));
    bench_diag(row->path, 1, 1, 1, 0, 0, bin, opt_level, tier, engine, target,
               f);
    fflush(stdout);
    if (saved_stdout >= 0) {
      test_fd_dup2(saved_stdout, fileno(stdout));
      test_fd_close(saved_stdout);
    }
    if (null_fd >= 0)
      test_fd_close(null_fd);
    bench_write_c_assembly(f, row->path, opt_level);
    fflush(f);
  }
  fclose(f);
}

static void bench_write_report_summary(FILE *f, const BenchTable *table,
                                       const BenchEnvironment *env) {
  if (env) {
    fprintf(f,
            "# Nytrix benchmark report\n\n"
            "- Host: `%s` / `%s` · `%s`\n"
            "- Compiler: `%s` · revision `%s`\n"
            "- Settings: opt `%s`, profile `%s`, tier `%s`, engine `%s`\n"
            "- Samples: %d measured + %d warm-up · fixture workers: %d\n\n",
            env->os, env->arch, env->cpu_model, env->compiler_binary,
            env->compiler_revision, env->opt_level, env->profile, env->tier,
            bench_engine_label(env->engine), env->samples, env->warmups,
            env->fixture_workers);
  }
  fprintf(f, "| Benchmark | Status | Compile ms | NYR ms | C ms | NYR/C |\n"
             "| --- | --- | ---: | ---: | ---: | ---: |\n");
  for (size_t i = 0; i < table->len; i++) {
    const BenchRow *r = &table->items[i];
    char ratio[32];
    if (bench_native_c_comparable(r))
      bench_ratio_text(ratio, sizeof(ratio), r->ny_ms / r->c_ms);
    else
      snprintf(ratio, sizeof(ratio), "—");
    fprintf(f, "| %s | %s | %.3f | %.3f | %.3f | %s |\n", r->name,
            bench_row_status(r), r->ny_compile_ms, r->ny_ms, r->c_ms, ratio);
  }
  fprintf(f, "\nNYR/C compares native Nytrix runtime with generated C; `—` "
             "means the fixture has no comparable C measurement.\n");
}

/*
 * Entrypoint
 */

/*
 * `--bench` driver.  Collects bench shapes (from the positional files when the
 * caller supplied some, otherwise from the default bench dir), filters by the
 * `--pattern` values, measures each, and reports.
 */
static int
run_benchmarks(const char *bin, const char *std_path, const char *std_bc,
               const char *pattern, const char *opt_level, const char *tier,
               const char *engine, const char *target, int runs, int warmup,
               int timeout_sec, int verbose, int show_ir, int show_asm,
               int show_passes, int profile, int compare_llvm,
               int correctness_only, const char *out_csv, const char *out_json,
               const char *out_md, const char *compile_profile, int budget_fail,
               int fixture_workers, StrVec *files, StrVec *patterns) {
  (void)budget_fail;
  (void)std_path;
  bench_core_bc_path = std_bc;
  if (correctness_only) {
    runs = 1;
    warmup = 0;
  }
  (void)pattern;

  const char *root = bench_dir_root();
  if (!root) {
    nyt_err("ny-test", "bench: no bench fixtures found (set NYTRIX_ROOT or run "
                       "from the repo)");
    return 2;
  }

  BenchEnvironment bench_env;
  bench_collect_environment(root, bin, target, opt_level, tier, engine, runs,
                            warmup, compile_profile, &bench_env);

  StrVec shapes = {0};
  {
    char bench_dir[PATH_MAX];
    snprintf(bench_dir, sizeof(bench_dir), "%s/etc/tests/bench", root);
    StrVec all = {0};
    if (!test_collect_native(bench_dir, &all, true)) {
      nyt_err("ny-test",
              "native fixture discovery failed for benchmark directory: %s",
              bench_dir);
      sv_free(&all);
      return 2;
    }
    qsort(all.items, all.len, sizeof(char *), path_lex_cmp);
    if (files && files->len > 0) {
      for (size_t i = 0; i < files->len; i++) {
        const char *want = files->items[i];
        if (nyt_ends_with(want, ".nshape")) {
          sv_push(&shapes, want);
          continue;
        }
        /*
         * Prefer a fixture whose stem is exactly the requested name. This
         * keeps `bench binary` from also selecting `binary-trees`, while
         * still allowing category/path substrings when no exact stem exists.
         */
        bool exact_stem = false;
        for (size_t j = 0; j < all.len; j++) {
          const char *base = test_path_basename(all.items[j]);
          char stem[128];
          snprintf(stem, sizeof(stem), "%s", base);
          char *dot = strrchr(stem, '.');
          if (dot)
            *dot = '\0';
          if (strcmp(stem, want) == 0) {
            exact_stem = true;
            break;
          }
        }
        size_t matches = 0;
        for (size_t j = 0; j < all.len; j++) {
          const char *base = test_path_basename(all.items[j]);
          char stem[128];
          snprintf(stem, sizeof(stem), "%s", base);
          char *dot = strrchr(stem, '.');
          if (dot)
            *dot = '\0';
          if ((exact_stem && strcmp(stem, want) == 0) ||
              (!exact_stem && strstr(all.items[j], want))) {
            sv_push(&shapes, all.items[j]);
            matches++;
          }
        }
        if (!matches)
          nyt_err("ny-test", want, ": no matching bench fixture");
      }
      qsort(shapes.items, shapes.len, sizeof(char *), path_lex_cmp);
      for (size_t i = 1; i < shapes.len; i++) {
        if (strcmp(shapes.items[i], shapes.items[i - 1]) == 0) {
          memmove(&shapes.items[i - 1], &shapes.items[i],
                  (shapes.len - i) * sizeof(char *));
          shapes.len--;
          i--;
        }
      }
    } else {
      int ci_mode = test_env_truthy("CI") ||
                    test_env_truthy("GITHUB_ACTIONS") || test_headless_mode();
      for (size_t j = 0; j < all.len; j++) {
        if (ci_mode && (shape_skips_ci(all.items[j]) ||
                        shape_skips_headless(all.items[j])))
          continue;
        sv_push(&shapes, all.items[j]);
      }
    }
    sv_free(&all);
  }
  if (shapes.len == 0) {
    nyt_err("ny-test", "bench: no .nshape benches found");
    sv_free(&shapes);
    return 2;
  }
  nyt_heading("Nytrix Benchmark Suite");
  {
    StrVec pats = {0};
    if (files)
      for (size_t i = 0; i < files->len; i++)
        if (!nyt_ends_with(files->items[i], ".nshape"))
          sv_push(&pats, files->items[i]);
    nyt_msg("DISCOVERY", NYT_CYAN, "%zu fixture%s available", shapes.len,
            shapes.len == 1 ? "" : "s");
    if (pats.len) {
      nyt_msg("FILTER", NYT_CYAN, "matching %zu pattern%s", pats.len,
              pats.len == 1 ? "" : "s");
      for (size_t i = 0; i < pats.len; i++)
        printf("    %s%s%s\n", nyt_clr(NYT_GRAY), pats.items[i],
               nyt_clr(NYT_RESET));
    }
    sv_free(&pats);
  }

  /*
   * Sort for deterministic output.
   */
  qsort(shapes.items, shapes.len, sizeof(char *), path_lex_cmp);

  BenchTable table = {0};
  int measured = 0;

  nyt_kv("compiler", "%s", disp_path(bin));
  nyt_kv("execution", "opt %s · profile %s · tier %s",
         opt_level ? opt_level : "default",
         compile_profile ? compile_profile : "default",
         tier ? tier : "default");
  nyt_kv("backend", "%s · engine %s · cache %s", target ? target : "default",
         bench_engine_label(engine), bench_cache_state());
  nyt_kv("samples", "%d run%s + %d warm-up%s", runs, runs == 1 ? "" : "s",
         warmup, warmup == 1 ? "" : "s");
  nyt_kv("legend",
         "<1µs = timer floor · ~ = fixture-timer fallback · — = unavailable");
  putchar('\n');
  bench_print_header();
  fflush(stdout);

  BenchJob *bench_jobs =
      (BenchJob *)calloc(shapes.len ? shapes.len : 1, sizeof(*bench_jobs));
  size_t bench_count = 0;
  for (size_t i = 0; i < shapes.len; i++) {
    const char *shape = shapes.items[i];
    const char *base = test_path_basename(shape);
    if (patterns && patterns->len > 0) {
      int matched = 0;
      for (size_t p = 0; p < patterns->len; p++)
        if (test_path_contains(shape, patterns->items[p]))
          matched = 1;
      if (!matched)
        continue;
    }
    BenchJob *job = &bench_jobs[bench_count++];
    job->shape = shape;
    snprintf(job->name, sizeof(job->name), "%s", base);
    size_t dot = strlen(job->name);
    while (dot > 0 && job->name[dot - 1] != '.')
      dot--;
    if (dot > 0)
      job->name[dot - 1] = '\0';
  }
  /*
   * Benchmark fixture concurrency is a throughput mode: it reduces suite
   * wall time but can make individual timings noisier. --jobs 1 gives isolated
   * measurements. The memory guard applies to automatic and explicit worker
   * counts so an aggressive request cannot overcommit the host.
   */
  int bench_jobs_limit = 1;
  if (fixture_workers > 0) {
    /*
     * Cap at the supported host scale; the result table is indexed by job.
     */
    bench_jobs_limit = fixture_workers > 96 ? 96 : fixture_workers;
  } else {
    /*
     * Isolated measurements are the default; parallelism is opt-in.
     */
    bench_jobs_limit = 1;
    /*
     * Explicit throughput overrides remain bounded by RAM below.
     */
    const char *jobs_env = getenv("NYTRIX_BENCH_JOBS");
    if (jobs_env && *jobs_env) {
      char *end = NULL;
      long requested = strtol(jobs_env, &end, 10);
      if (end != jobs_env && *end == '\0' && requested > 0)
        bench_jobs_limit = requested > 96 ? 96 : (int)requested;
    }
  }
#if defined(_WIN32)
  bench_jobs_limit = 1;
#endif
  if (bench_jobs_limit > (int)bench_count)
    bench_jobs_limit = (int)bench_count;
  if (bench_jobs_limit < 1)
    bench_jobs_limit = 1;
  if (!getenv("NYTRIX_TEST_NO_RAM_GUARD")) {
    double avail_gib = host_ram_available_gib();
    if (avail_gib > 0.0) {
      int ram_jobs = (int)((avail_gib - NY_TEST_HOST_MEMORY_RESERVE_GIB) /
                           NY_BENCH_WORKER_MEMORY_GIB);
      if (ram_jobs < 1)
        ram_jobs = 1;
      if (bench_jobs_limit > ram_jobs) {
        printf("[bench] limiting fixture workers %d -> %d: %.1f GiB memory "
               "available at %.1f GiB/worker\n",
               bench_jobs_limit, ram_jobs, avail_gib,
               NY_BENCH_WORKER_MEMORY_GIB);
        bench_jobs_limit = ram_jobs;
      }
    }
  }
  bench_env.route_workers = 1;
  bench_env.runtime_counters_enabled = bench_runtime_counters_requested();
  if (compile_profile && *compile_profile)
    ny_setenv("NYTRIX_OPT_PROFILE", compile_profile, 1);
  int actual_workers = 1;
  fflush(stdout);
#ifndef _WIN32
  typedef struct {
    pid_t pid;
    int worker_id;
    char output_path[PATH_MAX];
  } BenchChild;
  BenchChild *children = NULL;
  if (bench_jobs_limit > 1) {
    children =
        (BenchChild *)calloc((size_t)bench_jobs_limit - 1, sizeof(*children));
    if (!children)
      bench_jobs_limit = 1;
  }
  int child_count = 0;
  for (int worker = 1; worker < bench_jobs_limit; worker++) {
    int fd = make_test_capture_tmp(children[child_count].output_path,
                                   sizeof(children[child_count].output_path),
                                   "bench-worker-results");
    if (fd < 0)
      continue;
    close(fd);
    pid_t child = fork();
    if (child == 0) {
      FILE *out = fopen(children[child_count].output_path, "wb");
      if (!out)
        _exit(1);
      int ok = bench_run_worker_jobs(bench_jobs, bench_count, (size_t)worker,
                                     (size_t)bench_jobs_limit, bin, opt_level,
                                     tier, engine, target, compare_llvm, warmup,
                                     runs, timeout_sec, out);
      if (fclose(out) != 0)
        ok = 0;
      _exit(ok ? 0 : 1);
    }
    if (child < 0) {
      remove(children[child_count].output_path);
      continue;
    }
    children[child_count].pid = child;
    children[child_count].worker_id = worker;
    child_count++;
  }
  actual_workers += child_count;
  bench_run_worker_jobs(bench_jobs, bench_count, 0, (size_t)bench_jobs_limit,
                        bin, opt_level, tier, engine, target, compare_llvm,
                        warmup, runs, timeout_sec, NULL);
  for (int c = 0; c < child_count; c++) {
    int status = 0;
    pid_t waited;
    do {
      waited = waitpid(children[c].pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    int loaded = 0;
    FILE *in = waited == children[c].pid && WIFEXITED(status) &&
                       WEXITSTATUS(status) == 0
                   ? fopen(children[c].output_path, "rb")
                   : NULL;
    if (in) {
      size_t index = 0;
      size_t seen = 0;
      loaded = 1;
      while (fread(&index, sizeof(index), 1, in) == 1) {
        bench_worker_result_t result;
        if (index >= bench_count ||
            index % (size_t)bench_jobs_limit != (size_t)children[c].worker_id ||
            fread(&result, sizeof(result), 1, in) != 1) {
          loaded = 0;
          break;
        }
        bench_jobs[index].result = result;
        seen++;
      }
      size_t expected = 0;
      for (size_t i = (size_t)children[c].worker_id; i < bench_count;
           i += (size_t)bench_jobs_limit)
        expected++;
      if (ferror(in) || seen != expected)
        loaded = 0;
      fclose(in);
    }
    remove(children[c].output_path);
    if (!loaded)
      bench_run_worker_jobs(
          bench_jobs, bench_count, (size_t)children[c].worker_id,
          (size_t)bench_jobs_limit, bin, opt_level, tier, engine, target,
          compare_llvm, warmup, runs, timeout_sec, NULL);
  }
  for (int worker = 1; worker < bench_jobs_limit; worker++) {
    int started = 0;
    for (int c = 0; c < child_count; c++)
      started |= children[c].worker_id == worker;
    if (!started)
      bench_run_worker_jobs(bench_jobs, bench_count, (size_t)worker,
                            (size_t)bench_jobs_limit, bin, opt_level, tier,
                            engine, target, compare_llvm, warmup, runs,
                            timeout_sec, NULL);
  }
  free(children);
#else
  bench_run_worker_jobs(bench_jobs, bench_count, 0, 1, bin, opt_level, tier,
                        engine, target, compare_llvm, warmup, runs, timeout_sec,
                        NULL);
#endif
  bench_env.fixture_workers = actual_workers;
  for (size_t i = 0; i < bench_count; i++) {
    BenchJob *job = &bench_jobs[i];
    bench_record_result(&table, &measured, verbose, show_ir, show_asm,
                        show_passes, profile, compare_llvm, bin, opt_level,
                        tier, engine, target, compile_profile, warmup, runs,
                        timeout_sec, job->shape, job->name, &job->result, 1);
  }
  int failed = measured == 0;
  int unstable = 0;
  for (size_t i = 0; i < table.len; i++) {
    const BenchRow *r = &table.items[i];
    if (!r->checksum_ok || r->ny_failures || r->llvm_failures || r->c_failures)
      failed = 1;
    if (r->budget_failed) {
      if (budget_fail)
        failed = 1;
      else
        printf("%sbudget miss (not fatal without --budget-fail): %s%s\n",
               nyt_clr(NYT_YELLOW), r->name, nyt_clr(NYT_RESET));
    }
    if (!correctness_only && runs >= 5 && bench_row_unstable(r)) {
      unstable = 1;
      failed = 1;
    }
  }
  bench_print_hw_counters(&table);
  bench_print_hotspots(&table);
  bool enforce_geomean_budgets = runs >= 3 && table.len >= 60;
  if (bench_print_geomean(&table, enforce_geomean_budgets))
    failed = 1;
  bench_print_floor_adjusted_geomean(&table);
  if (out_csv) {
    FILE *f = fopen(out_csv, "w");
    if (f) {
      bench_write_csv(f, &table, &bench_env);
      fclose(f);
    }
  }
  if (out_json) {
    FILE *f = fopen(out_json, "w");
    if (f) {
      bench_write_json(f, &table, &bench_env);
      fclose(f);
    }
  }
  if (out_md) {
    FILE *f = fopen(out_md, "w");
    if (f) {
      if (bench_full_report)
        bench_write_report_summary(f, &table, &bench_env);
      else
        bench_write_md(f, &table, &bench_env);
      fclose(f);
    }
  }
  if (bench_full_report && out_md)
    bench_write_full_report(out_md, &table, bin, opt_level, tier, engine,
                            target, compare_llvm);
  bench_rows_free(&table);
  sv_free(&shapes);
  /*
   * The table is stdout while the final verdict is stderr.  Flush the table
   * before reporting a failure so redirected and CI logs keep their natural
   * order instead of splicing the verdict into a benchmark row.
   */
  fflush(stdout);
  if (unstable)
    nyt_err("ny-test", "bench: unstable repeated measurement exceeded the "
                       "configured noise threshold");
  else if (failed)
    nyt_err("ny-test",
            "bench: one or more components failed or checksums disagreed");
  return failed ? 1 : 0;
}
