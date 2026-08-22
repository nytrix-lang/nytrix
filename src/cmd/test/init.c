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

#include "test.h"
#include "base/args.h"
#include "base/util.h"
#include "../tools/repo.h"
#include "../tools/tool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef __linux__
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


#define NY_TEST_DEFAULT_TIMEOUT_SEC 90
#define NY_TEST_MAX_TIMEOUT_SEC 300
#define NY_TEST_PARALLEL_TIMEOUT_GRACE_MS 1000.0
#define NY_TEST_TIMEOUT_RC 124

#define NY_BENCH_MAX_RUNS 5
#define NY_BENCH_TIMEOUT_SEC 300
#define NY_BENCH_MAX_OUTPUT 8192

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
static void print_section(const char *name);
static uint64_t fnv1a_update(uint64_t h, const void *ptr, size_t n);
static uint64_t test_sig(const char *path, const char *bin, const char *std_path,
                         const char *std_bc);
static int path_lex_cmp(const void *a, const void *b);
static int run_benchmarks(const char *bin, const char *pattern, const char *opt_level,
                          const char *tier, const char *engine, const char *target, int runs,
                          int warmup, int timeout_sec, int verbose, int show_ir, int show_asm,
                          int show_passes, int profile, int compare_llvm, int correctness_only,
                          const char *out_csv, const char *out_json, const char *out_md,
                          const char *compile_profile, int budget_fail,
                          StrVec *files, StrVec *patterns);
static void apply_test_child_env(void);
static char *read_small_file(const char *path);
static const char *test_warn_arg(void);
static void push_test_warn_arg(char **argv, int *argc, int max);
static bool shape_path_is_nshape(const char *p);
static const char *shape_path_split(const char *p, char *file_out,
                                    size_t file_cap, const char **shape_out);
static int shape_block_span(const char *data, size_t data_len, const char *name,
                            size_t *out_start, size_t *out_end);
static char *shape_source_block(const char *shape_path, const char *name);
static char *materialize_shape_ny_source(const char *shape_path);
static char *shape_meta_string(const char *shape_path, const char *key);
static int native_backend_explicit(const char *flags);
static int path_is_native_test(const char *p);
static int path_is_stdlib_source(const char *p);
static int run_progress_selftest(const char *bin, int timeout_sec);
static int run_shape_generator_selftest(void);
static int make_test_capture_tmp(char *tmp, size_t tmp_len,
                                 const char *prefix);
static int bench_run_capture(char *const argv[], int timeout_sec,
                             const char *output_path);
static int test_command_available(const char *cmd);

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
  int fd = make_test_capture_tmp(capture->path, sizeof(capture->path), "failures");
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

static void failure_output_capture_end(FailureOutputCapture *capture) {
  if (!capture || !capture->stream)
    return;
  fflush(stdout);
  fflush(stderr);
  test_fd_dup2(capture->saved_stdout, fileno(stdout));
  test_fd_dup2(capture->saved_stderr, fileno(stderr));
  test_fd_close(capture->saved_stdout);
  test_fd_close(capture->saved_stderr);
  rewind(capture->stream);

  FILE *block = tmpfile();
  char line[8192];
  int capturing = 0;
  while (fgets(line, sizeof(line), capture->stream)) {
    if (strncmp(line, "[replay output]", 15) == 0) {
      capturing = 1;
      if (block) {
        fclose(block);
        block = tmpfile();
      }
    }
    if (capturing && block)
      fputs(line, block);
    if (!failure_marker_line(line))
      continue;
    if (capturing && block) {
      fflush(block);
      rewind(block);
      char chunk[8192];
      size_t n = 0;
      while ((n = fread(chunk, 1, sizeof(chunk), block)) > 0)
        fwrite(chunk, 1, n, stdout);
    } else {
      fputs(line, stdout);
    }
    putchar('\n');
    capturing = 0;
    if (block) {
      fclose(block);
      block = tmpfile();
    }
  }
  if (block)
    fclose(block);
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

static uint32_t cache_hash_path(const char *s) {
  return ny_hash32_cstr(s);
}

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

static void timings_push(TimingVec *v, const char *path, int ms, const char *suite) {
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

static void cache_set(CacheDb *db, const char *path, uint64_t sig, int ok, int dur_ms) {
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
  const char *p = data;
  const char *end = data + data_len;
  const char *needle = "shape ";
  size_t needle_len = 6;
  while (p < end) {
    const char *hit = ny_memmem(p, (size_t)(end - p), needle, needle_len);
    if (!hit)
      break;
    if (hit == data || !(isalnum((unsigned char)hit[-1]) || hit[-1] == '_')) {
      const char *q = hit + needle_len;
      const char *ns = q;
      while (q < end && (isalnum((unsigned char)*q) || *q == '_' || *q == '-'))
        q++;
      if (q > ns) {
        size_t name_len = (size_t)(q - ns);
        const char *r = q;
        while (r < end && (*r == ' ' || *r == '\t'))
          r++;
        if (r < end && *r == '{') {
          char *name = (char *)malloc(name_len + 1);
          if (name) {
            memcpy(name, ns, name_len);
            name[name_len] = '\0';
            sv_push(names, name);
            count++;
          }
        }
      }
    }
    p = hit + needle_len;
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
      char *entry = (char *)malloc(strlen(p) + strlen(names.items[k]) + 2);
      if (entry) {
        sprintf(entry, "%s:%s", p, names.items[k]);
        sv_push(&out, entry);
      }
    }
    sv_free(&names);
  }
  sv_free(files);
  *files = out;
}

static void collect_ny(const char *path, StrVec *out) {
  if (nyt_is_file(path)) {
    if (nyt_ends_with(path, ".ny") || nyt_ends_with(path, ".nshape"))
      sv_push(out, path);
    return;
  }
  if (!is_dir(path))
    return;
  /*
   * Strip trailing slash so path joins produce no double-slash.
   */
  size_t plen = strlen(path);
  while (plen > 1 && path[plen - 1] == '/')
    plen--;
  DIR *d = opendir(path);
  if (!d)
    return;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
      continue;
    if (ent->d_name[0] == '.')
      continue;
    char child[PATH_MAX];
    snprintf(child, sizeof(child), "%.*s/%s", (int)plen, path, ent->d_name);
    if (is_dir(child))
      collect_ny(child, out);
    else if (nyt_ends_with(child, ".ny") || nyt_ends_with(child, ".nshape"))
      sv_push(out, child);
  }
  closedir(d);
}

#ifdef _WIN32
typedef HANDLE ny_test_proc_t;
#define NY_TEST_PROC_INVALID NULL

static int ny_test_proc_valid(ny_test_proc_t p) { return p != NULL; }
static int ny_test_proc_eq(ny_test_proc_t a, ny_test_proc_t b) { return a == b; }
static void ny_test_proc_close(ny_test_proc_t p) {
  if (p)
    CloseHandle(p);
}

static int ny_cmd_append(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
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
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
  return 1;
}

static int ny_cmd_append_char(char **buf, size_t *len, size_t *cap, char c) {
  return ny_cmd_append(buf, len, cap, &c, 1);
}

static int ny_cmd_append_arg(char **buf, size_t *len, size_t *cap, const char *arg) {
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

static ny_test_proc_t ny_test_spawn_argv(char *const argv[], const char *output_path, int quiet) {
#ifdef _WIN32
  char app_buf[PATH_MAX];
  const char *app = ny_test_resolve_app(argv ? argv[0] : NULL, app_buf, sizeof(app_buf));
#endif
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
    out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, disposition,
                      FILE_ATTRIBUTE_NORMAL, NULL);
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
  BOOL ok = CreateProcessA(app, cmd, NULL, NULL, inherit, 0, NULL, NULL, &si, &pi);
  free(cmd);
  if (out)
    CloseHandle(out);
  if (!ok)
    return NY_TEST_PROC_INVALID;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

static int ny_test_wait_rc(ny_test_proc_t proc, int timeout_sec, int *timed_out) {
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
#define NY_TEST_PROC_INVALID ((pid_t)-1)

static int ny_test_proc_valid(ny_test_proc_t p) { return p > 0; }
static int ny_test_proc_eq(ny_test_proc_t a, ny_test_proc_t b) { return a == b; }
static void ny_test_proc_close(ny_test_proc_t p) { (void)p; }
#endif
static void trim_inplace(char *s);
static void error_meta_free(char *flags, char *expect);
static void read_error_meta(const char *path, char **flags_out, char **expect_out);
static int split_words(char *s, char **out, int max);
static char *read_small_file(const char *path);
static int run_debug_argv(char *const argv[], int timeout_sec, int use_path_lookup);
static int test_env_truthy(const char *name);

#include "elf.c"


static int test_env_truthy(const char *name) {
  return ny_env_is_truthy(getenv(name)) ? 1 : 0;
}

static int test_env_falsey(const char *name) {
  const char *v = getenv(name);
  if (!v || !*v)
    return 0;
  return strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0 ||
         strcmp(v, "no") == 0;
}

static void test_setenv_default(const char *name, const char *value) {
  if (!getenv(name))
    ny_setenv(name, value, 0);
}

static int retry_trace_enabled(void) {
  if (test_env_falsey("NYTRIX_TEST_RETRY_TRACE"))
    return 0;
  if (test_env_truthy("NYTRIX_TEST_RETRY_TRACE"))
    return 1;
  return 1;
}

static int show_pass_output_enabled(void) {
  return test_env_truthy("NYTRIX_TEST_SHOW_PASS_OUTPUT");
}

static int test_ascii_symbols(void) {
  const char *v = getenv("NYTRIX_UI_SYMBOLS");
  if (!v || !*v)
    v = getenv("NYTRIX_ASCII");
  if (!v || !*v)
    return 0;
  return strcmp(v, "ascii") == 0 || strcmp(v, "plain") == 0 || strcmp(v, "text") == 0 ||
         strcmp(v, "safe") == 0 || ny_env_is_truthy(v);
}

static const char *test_symbol(const char *sym) {
  if (!test_ascii_symbols() || !sym)
    return sym ? sym : "-";
  if (strcmp(sym, "✓") == 0)
    return "+";
  if (strcmp(sym, "✗") == 0)
    return "x";
  return sym;
}

static int make_test_capture_tmp(char *tmp, size_t tmp_len, const char *prefix) {
  if (!tmp || tmp_len == 0)
    return -1;
#ifdef _WIN32
  char tmp_dir[PATH_MAX];
  DWORD tmp_dir_len = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
  if (tmp_dir_len == 0 || tmp_dir_len >= sizeof(tmp_dir))
    snprintf(tmp_dir, sizeof(tmp_dir), ".\\");
  static volatile LONG tmp_seq = 0;
  LONG seq = InterlockedIncrement((volatile LONG *)&tmp_seq);
  snprintf(tmp, tmp_len, "%sny-%s-%lu-%lu-%ld.log", tmp_dir, prefix ? prefix : "test",
           (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), (long)seq);
  return 0;
#else
  snprintf(tmp, tmp_len, "%s/ny-%s-%ld-XXXXXX", nyt_temp_dir(), prefix ? prefix : "test",
           (long)getpid());
  return mkstemp(tmp);
#endif
}

static void print_captured_test_output(const char *label, const char *path, const char *tmp) {
  if (!tmp || !*tmp)
    return;
  char *out = read_small_file(tmp);
  if (!out || !*out) {
    free(out);
    return;
  }
  printf("%s[%s]%s %s\n", nyt_clr(NYT_GRAY), label ? label : "test output",
         nyt_clr(NYT_RESET), disp_path(path));
  fputs(out, stdout);
  size_t n = strlen(out);
  if (n == 0 || out[n - 1] != '\n')
    putchar('\n');
  free(out);
}

static void format_test_time(char *buf, size_t cap, int dur_ms) {
  if (!buf || cap == 0)
    return;
  if (dur_ms < 0)
    snprintf(buf, cap, "cache");
  else
    snprintf(buf, cap, "%dms", dur_ms);
}

static void print_test_progress_line(int pct, const char *a, const char *a_color,
                                     const char *b, const char *b_color,
                                     const char *c, const char *c_color,
                                     const char *time_label, const char *path,
                                     const char *suffix) {
  char fallback[32];
  if (!time_label || !*time_label) {
    format_test_time(fallback, sizeof(fallback), 0);
    time_label = fallback;
  }
  const char *aa = test_symbol(a);
  const char *bb = test_symbol(b);
  const char *cc = test_symbol(c);
  printf("%s%3d%%%s [%s%s%s/%s%s%s/%s%s%s] %s%8s%s %s",
         nyt_clr(NYT_GRAY), pct, nyt_clr(NYT_RESET),
         nyt_clr(a_color ? a_color : NYT_GRAY), aa, nyt_clr(NYT_RESET),
         nyt_clr(b_color ? b_color : NYT_GRAY), bb, nyt_clr(NYT_RESET),
         nyt_clr(c_color ? c_color : NYT_GRAY), cc, nyt_clr(NYT_RESET),
         nyt_clr(NYT_GRAY), time_label, nyt_clr(NYT_RESET), disp_path(path));
  if (suffix && *suffix)
    printf(" %s", suffix);
  fputc('\n', stdout);
}

static void apply_test_child_env(void) {
  if (test_env_falsey("NYTRIX_TEST_CACHE") || test_env_truthy("NYTRIX_TEST_NO_NATIVE_CACHE")) {
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
  if (test_env_truthy("NYTRIX_TEST_EXEC_CACHE")) {
    test_setenv_default("NYTRIX_TEST_NO_NATIVE_CACHE", "0");
    test_setenv_default("NYTRIX_JIT_CACHE", "1");
    test_setenv_default("NYTRIX_AOT_CACHE", "1");
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
#ifdef _WIN32
  Sleep(10);
#else
  struct timespec ts = {0, 10000000L};
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

static void test_collect_archive_sibling_sources(const char *archive_path, StrVec *out) {
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
    memcpy(dir, archive_path, dlen);
    dir[dlen] = '\0';
    base++;
  } else {
    snprintf(dir, sizeof(dir), ".");
    base = archive_path;
  }
  size_t blen = strlen(base);
  if (blen <= 2 || blen - 2 >= sizeof(stem))
    return;
  memcpy(stem, base, blen - 2);
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

static int test_archive_sources_newer_than_archive(const char *archive_path, const StrVec *srcs) {
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
  if (alen <= 2 || archive_path[alen - 2] != '.' || archive_path[alen - 1] != 'a')
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
    else
      test_collect_archive_sibling_sources(archive_path, &srcs);
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
    if (fd < 0) { ok = 0; break; }
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
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
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
  snprintf(out, out_len, "%s/ny-internal-link-libs-%ld-%ld.a",
           nyt_temp_dir(), (long)getpid(), (long)now_ms());
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
  if (!expect_val || strncmp(expect_val, "object_link_run_", 16) != 0) {
    free(expect_val);
    return 0;
  }
  const char *suffix = expect_val + 16;
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
    fprintf(stderr, "object link/run: missing emitted object from token '-o <path>' in %s\n",
            disp_path(shape_path));
    free(expect_val);
    return 1;
  }
  if (!nyt_is_file(obj_path)) {
    free(expect_val);
    return 1;
  }

  /*
   * Collect all "link" entries (support multi-link for split archive tests etc.)
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
      if (shape && *shape && !shape_block_span(data, data_len, shape, &start, &end)) {
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
  FILE *obj_file = fopen(obj_path, "rb");
  if (obj_file) {
    unsigned char ident[5] = {0};
    if (fread(ident, 1, sizeof(ident), obj_file) == sizeof(ident) &&
        memcmp(ident, "\177ELF", 4) == 0 && ident[4] == 1)
      force_m32 = 1;
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
  int internal_rc = test_internal_aarch64_elf64_link_run(
      obj_path, ret_kind, expected_val, shape_path);
  if (internal_rc == 0) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  internal_rc = test_internal_elf64_link_run(obj_path, ret_kind, expected_val, shape_path,
                                                  first_archive);
  if (internal_rc == 0) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  internal_rc = test_internal_elf32_link_run(obj_path, ret_kind, expected_val, shape_path,
                                              first_archive);
  if (internal_rc == 0) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    if (merged_archive[0]) remove(merged_archive);
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  if (merged_archive[0]) remove(merged_archive);

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
    fprintf(hf, "#include <math.h>\nextern double rt_main(void);\n"
                "int main(void){double v=rt_main();return fabs(v-(%s))<1e-7?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_F32)
    fprintf(hf, "#include <math.h>\nextern float rt_main(void);\n"
                "int main(void){float v=rt_main();return fabsf(v-(%sf))<1e-6f?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I32)
    fprintf(hf, "extern int rt_main(void);\n"
                "int main(void){return rt_main()==(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U32)
    fprintf(hf, "extern unsigned int rt_main(void);\n"
                "int main(void){return rt_main()==(%sU)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I16)
    fprintf(hf, "extern short rt_main(void);\n"
                "int main(void){return rt_main()==(short)(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U16)
    fprintf(hf, "extern unsigned short rt_main(void);\n"
                "int main(void){return rt_main()==(unsigned short)(%sU)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_I8)
    fprintf(hf, "extern signed char rt_main(void);\n"
                "int main(void){return rt_main()==(signed char)(%s)?0:1;}\n",
            expected_val);
  else if (ret_kind == NY_TEST_LINK_RET_U8 || ret_kind == NY_TEST_LINK_RET_BOOL)
    fprintf(hf, "extern unsigned char rt_main(void);\n"
                "int main(void){return rt_main()==(unsigned char)(%sU)?0:1;}\n",
            expected_val);
  else
    fprintf(hf, "extern long rt_main(void);\n"
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
    fprintf(stderr, "object link/run: cc link failed rc=%d for %s\n",
            link_rc, disp_path(shape_path));
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
    fprintf(stderr, "object link/run: executable failed rc=%d for %s\n",
            run_rc, disp_path(shape_path));

  remove(harness_path);
  remove(exe_path);
  sv_free(&links);
  free(expect_val);
  return run_ok ? 0 : 1;
#endif
}

static int run_one_blocking_once(const char *bin, const char *path, const char *std_path,
                                 const char *std_bc, int timeout_sec, int trace_exec,
                                 const char *matrix_flags) {
  char *materialized_path = NULL;
  const char *exec_path = path;
  if (shape_path_is_nshape(path)) {
    materialized_path = materialize_shape_ny_source(path);
    if (!materialized_path)
      return 127;
    exec_path = materialized_path;
  }
  char flags_buf[1024];
  char *flags = NULL;
  char *expect = NULL;
  read_error_meta(path, &flags, &expect);
  flags_buf[0] = '\0';
  char *flagv[32];
  int flagc = 0;
  int has_native_backend = 0;
  if ((flags && *flags) || (matrix_flags && *matrix_flags)) {
    snprintf(flags_buf, sizeof(flags_buf), "%s%s%s",
             flags ? flags : "",
             (flags && *flags && matrix_flags && *matrix_flags) ? " " : "",
             matrix_flags ? matrix_flags : "");
    trim_inplace(flags_buf);
    has_native_backend = native_backend_explicit(flags_buf);
    flagc = split_words(flags_buf, flagv, 32);
  }

  char *argv[80];
  int argc = 0;
  argv[argc++] = (char *)bin;
  if (trace_exec)
    argv[argc++] = "-trace";
  push_test_warn_arg(argv, &argc, 80);
  if (path_is_stdlib_source(path)) {
    argv[argc++] = "--stop-after=opt";
    argv[argc++] = "--parallel=off";
  }
  if (std_path) {
    argv[argc++] = "--std";
    argv[argc++] = (char *)std_path;
  }
  if (std_bc) {
    argv[argc++] = "--std-bc";
    argv[argc++] = (char *)std_bc;
  }
  if (path_is_native_test(path) && !has_native_backend && argc < 78) {
    argv[argc++] = "--native-backend";
    argv[argc++] = "x86_64";
  }
  for (int i = 0; i < flagc && argc < 76; i++)
    argv[argc++] = flagv[i];
  if (trace_exec) {
    argv[argc++] = "--no-progress";
    argv[argc++] = "--color=never";
  }
  argv[argc++] = (char *)exec_path;
  argv[argc] = NULL;

  char tmp[PATH_MAX];
  tmp[0] = '\0';
  int capture_fd = make_test_capture_tmp(tmp, sizeof(tmp), trace_exec ? "replay" : "retry");
  if (capture_fd < 0) {
    if (materialized_path) {
      remove(materialized_path);
      free(materialized_path);
    }
    error_meta_free(flags, expect);
    return 127;
  }

  int rc = 127;
  int timed_out = 0;
#ifdef _WIN32
  ny_test_proc_t pid = ny_test_spawn_argv(argv, tmp, 0);
  if (!ny_test_proc_valid(pid)) {
    remove(tmp);
    if (materialized_path) {
      remove(materialized_path);
      free(materialized_path);
    }
    error_meta_free(flags, expect);
    return 127;
  }
  rc = ny_test_wait_rc(pid, timeout_sec, &timed_out);
  ny_test_proc_close(pid);
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    apply_test_child_env();
    dup2(capture_fd, STDOUT_FILENO);
    dup2(capture_fd, STDERR_FILENO);
    close(capture_fd);
    execv(bin, argv);
    _exit(127);
  }
  if (pid <= 0) {
    close(capture_fd);
    remove(tmp);
    if (materialized_path) {
      remove(materialized_path);
      free(materialized_path);
    }
    error_meta_free(flags, expect);
    return 127;
  }
  close(capture_fd);
  int status = 0;
  double start_ms = now_ms();
  double timeout_ms = (double)timeout_sec * 1000.0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      rc = child_status_rc(status);
      break;
    }
    if (r < 0) {
      if (errno == EINTR)
        continue;
      rc = 127;
      break;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      timed_out = 1;
      rc = NY_TEST_TIMEOUT_RC;
      break;
    }
    poll_sleep();
  }
#endif
  if (timed_out)
    rc = NY_TEST_TIMEOUT_RC;
  if (rc != 0 || show_pass_output_enabled())
    print_captured_test_output(trace_exec ? "replay output" : "retry output", path, tmp);
  remove(tmp);
  if (materialized_path) {
    remove(materialized_path);
    free(materialized_path);
  }
  if (rc == 0)
    rc = object_link_run_check(path);
  error_meta_free(flags, expect);
  return rc;
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

static int run_one_blocking(const char *bin, const char *path, const char *std_path, const char *std_bc,
                            int timeout_sec, int trace_exec) {
  char *matrix = (path && shape_path_is_nshape(path))
                     ? shape_meta_string(path, "flags_matrix")
                     : NULL;
  if (!matrix || !*matrix) {
    free(matrix);
    return run_one_blocking_once(bin, path, std_path, std_bc, timeout_sec, trace_exec, NULL);
  }

  char matrix_buf[4096];
  snprintf(matrix_buf, sizeof(matrix_buf), "%s", matrix);
  free(matrix);
  char *rows[64];
  int rowc = split_flag_matrix_rows(matrix_buf, rows, 64);
  if (rowc <= 0)
    return run_one_blocking_once(bin, path, std_path, std_bc, timeout_sec, trace_exec, NULL);

  for (int i = 0; i < rowc; i++) {
    int rc = run_one_blocking_once(bin, path, std_path, std_bc, timeout_sec, trace_exec, rows[i]);
    if (rc != 0)
      return rc;
  }
  return 0;
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
      memcpy(file_out, p, flen);
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
        memcpy(out, vp, (size_t)(line + line_len - vp));
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
  snprintf(needle, sizeof(needle), "source %s <<'", name);
  size_t needle_len = strlen(needle);
  const char *p = data + start;
  const char *region_end = data + end;
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
    size_t line_len = next ? (size_t)(next - line) : (size_t)(region_end - line);
    if (line_len && line[line_len - 1] == '\r')
      line_len--;
    if (line_len == marker_len && memcmp(line, marker, marker_len) == 0) {
      size_t n = (size_t)(line - body);
      char *out = (char *)malloc(n + 1);
      if (out) {
        memcpy(out, body, n);
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

static char *shape_meta_string(const char *shape_path, const char *key) {
  if (!shape_path || !key || !*key)
    return NULL;
  char file[PATH_MAX];
  const char *shape = NULL;
  shape_path_split(shape_path, file, sizeof(file), &shape);
  char *data = read_small_file(file);
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
  free(data);
  return found;
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

static char *shape_embedded_awk_block(const char *shape_path, int *declared,
                                      char *why, size_t why_len) {
  if (declared)
    *declared = 0;
  char *data = read_small_file(shape_path);
  if (!data)
    return NULL;
  const char *prefix = "source ny generate ";
  char *line = data;
  while (line && *line) {
    char *next = strchr(line, '\n');
    size_t line_len = next ? (size_t)(next - line) : strlen(line);
    char *p = line;
    while ((size_t)(p - line) < line_len && isspace((unsigned char)*p))
      p++;
    if ((size_t)(p - line) + strlen(prefix) <= line_len &&
        memcmp(p, prefix, strlen(prefix)) == 0) {
      if (declared)
        *declared = 1;
      char *spec = p + strlen(prefix);
      if ((size_t)(spec - line) + 6 > line_len || memcmp(spec, "awk <<", 6) != 0) {
        snprintf(why, why_len, "expected generator 'awk' after 'source ny generate'");
        free(data);
        return NULL;
      }
      char *quote = spec + 6;
      if (quote >= line + line_len || *quote != '\'' ||
          line + line_len <= quote + 2 || line[line_len - 1] != '\'') {
        snprintf(why, why_len, "malformed embedded AWK block opener");
        free(data);
        return NULL;
      }
      char *marker = quote + 1;
      size_t marker_len = (size_t)((line + line_len - 1) - marker);
      if (!marker_len) {
        snprintf(why, why_len, "embedded AWK block marker is empty");
        free(data);
        return NULL;
      }
      char *body = next ? next + 1 : NULL;
      for (char *body_line = body; body_line && *body_line;) {
        char *body_next = strchr(body_line, '\n');
        size_t body_len = body_next ? (size_t)(body_next - body_line) : strlen(body_line);
        if (body_len && body_line[body_len - 1] == '\r')
          body_len--;
        if (body_len == marker_len && memcmp(body_line, marker, marker_len) == 0) {
          size_t n = (size_t)(body_line - body);
          char *out = malloc(n + 1);
          if (out) {
            memcpy(out, body, n);
            out[n] = '\0';
          }
          free(data);
          return out;
        }
        body_line = body_next ? body_next + 1 : NULL;
      }
      snprintf(why, why_len, "unterminated embedded AWK block");
      free(data);
      return NULL;
    }
    line = next ? next + 1 : NULL;
  }
  free(data);
  return NULL;
}

static char *shape_write_temp_source(const char *source) {
  char tmp[PATH_MAX];
#ifdef _WIN32
  if (!GetTempFileNameA("build", "nys", 0, tmp))
    return NULL;
  FILE *f = fopen(tmp, "wb");
#else
  snprintf(tmp, sizeof(tmp), "%s/ny-shape-%ld-XXXXXX", nyt_temp_dir(), (long)getpid());
  int fd = mkstemp(tmp);
  if (fd < 0)
    return NULL;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    remove(tmp);
    return NULL;
  }
#endif
  if (!f)
    return NULL;
  size_t n = strlen(source);
  int ok = fwrite(source, 1, n, f) == n && fclose(f) == 0;
  if (!ok) {
    remove(tmp);
    return NULL;
  }
  return strdup(tmp);
}

static char *materialize_shape_ny_source(const char *shape_path) {
  char *source = shape_source_block(shape_path, "ny");
  if (source) {
    char *path = shape_write_temp_source(source);
    free(source);
    return path;
  }

  int declared = 0;
  char why[256] = {0};
  char *program = shape_embedded_awk_block(shape_path, &declared, why, sizeof(why));
  if (!program) {
    fprintf(stderr, "ny-test: %s: %s\n", disp_path(shape_path),
            why[0] ? why : "missing 'source ny' block or embedded AWK generator");
    return NULL;
  }
  if (!test_command_available("awk")) {
    fprintf(stderr, "ny-test: %s: embedded AWK generator requires 'awk' in PATH\n",
            disp_path(shape_path));
    free(program);
    return NULL;
  }

  char program_path[PATH_MAX];
  char output_path[PATH_MAX];
  int program_fd = make_test_capture_tmp(program_path, sizeof(program_path), "shape-awk");
  int output_fd = make_test_capture_tmp(output_path, sizeof(output_path), "shape-ny");
#ifdef _WIN32
  FILE *f = program_fd >= 0 ? fopen(program_path, "wb") : NULL;
#else
  if (output_fd >= 0)
    close(output_fd);
  FILE *f = program_fd >= 0 ? fdopen(program_fd, "wb") : NULL;
#endif
  if (!f) {
#ifndef _WIN32
    if (program_fd >= 0)
      close(program_fd);
#endif
    fprintf(stderr, "ny-test: %s: cannot create embedded AWK program file\n",
            disp_path(shape_path));
    free(program);
    if (output_fd >= 0)
      remove(output_path);
    return NULL;
  }
  size_t program_len = strlen(program);
  int wrote = fwrite(program, 1, program_len, f) == program_len && fclose(f) == 0;
  free(program);
  if (!wrote) {
    fprintf(stderr, "ny-test: %s: cannot write embedded AWK program\n",
            disp_path(shape_path));
    remove(program_path);
    remove(output_path);
    return NULL;
  }

#ifdef _WIN32
  const char *empty_input = "NUL";
#else
  const char *empty_input = "/dev/null";
#endif
  char *argv[] = {"awk", "-f", program_path, (char *)empty_input, NULL};
  int rc = bench_run_capture(argv, 30, output_path);
  remove(program_path);
  if (rc != 0) {
    char *detail = read_small_file(output_path);
    fprintf(stderr, "ny-test: %s: embedded AWK generator failed (rc=%d)%s%s\n",
            disp_path(shape_path), rc, detail && *detail ? ": " : "",
            detail && *detail ? detail : "");
    free(detail);
    remove(output_path);
    return NULL;
  }
  struct stat st;
  if (stat(output_path, &st) != 0 || st.st_size == 0) {
    fprintf(stderr, "ny-test: %s: embedded AWK generator produced no Ny source\n",
            disp_path(shape_path));
    remove(output_path);
    return NULL;
  }
  return strdup(output_path);
}

static void read_error_meta(const char *path, char **flags_out, char **expect_out) {
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

static int run_debug_argv(char *const argv[], int timeout_sec, int use_path_lookup) {
  debug_replay_env();
  fflush(NULL);
#ifdef _WIN32
  (void)use_path_lookup;
  ny_test_proc_t proc = ny_test_spawn_argv(argv, NULL, 0);
  if (!ny_test_proc_valid(proc))
    return 127;
  int timed_out = 0;
  int rc = ny_test_wait_rc(proc, timeout_sec, &timed_out);
  ny_test_proc_close(proc);
  return timed_out ? NY_TEST_TIMEOUT_RC : rc;
#else
  ny_test_proc_t pid = fork();
  if (pid == 0) {
    debug_replay_env();
    enable_core_dumps();
    dup2(STDOUT_FILENO, STDERR_FILENO);
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
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      return NY_TEST_TIMEOUT_RC;
    }
    poll_sleep();
  }
#endif
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
  if (test_command_available("lldb"))
    return "lldb";
  if (test_command_available("gdb"))
    return "gdb";
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
  return test_env_truthy("GITHUB_ACTIONS");
}

static int test_debugger_for_rc(int rc) {
  if (test_env_truthy("NYTRIX_TEST_DEBUGGER_ALL"))
    return rc != 0;
  if (rc == NY_TEST_TIMEOUT_RC)
    return 0;
  if (rc >= 128)
    return 1;
  if (rc < 0)
    return 1;
  return 0;
}

static int test_is_ownership_error_path(const char *path) {
  return path && strstr(path, "etc/tests/errors/ownership/") != NULL;
}

static int test_is_optional_system_stdlib(const char *path) {
  return path &&
         (strncmp(path, "lib/os/ui/", 10) == 0 ||
          strncmp(path, "lib/os/sound/", 13) == 0 ||
          strcmp(path, "lib/os/ui/mod.ny") == 0 ||
          strcmp(path, "lib/os/sound/mod.ny") == 0 ||
          strcmp(path, "lib/os/clipboard.ny") == 0);
}

static int test_is_unsupported_native_host(const char *path) {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  (void)path;
  return 0;
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (!path || strncmp(path, "etc/tests/native/", 17) != 0)
    return 0;
  return strncmp(path, "etc/tests/native/aarch64/", 25) != 0 &&
         strncmp(path, "etc/tests/native/target/", 24) != 0;
#else
  return path && strncmp(path, "etc/tests/native/", 17) == 0;
#endif
}

static int test_is_unsupported_native_platform(const char *path) {
#if !defined(__linux__)
  if (path &&
      (strncmp(path, "etc/tests/native/elf32/link/", 28) == 0 ||
       strncmp(path, "etc/tests/native/elf64/link/", 28) == 0 ||
       strncmp(path, "etc/tests/native/aarch64/link/", 30) == 0))
    return 1;
#elif !defined(__aarch64__)
  if (path && strncmp(path, "etc/tests/native/aarch64/link/", 30) == 0) {
    const char *emulator = getenv("NYTRIX_TEST_EMULATOR");
    if (!emulator || !*emulator)
      emulator = "qemu-aarch64";
    if (!test_command_available(emulator))
      return 1;
  }
#endif
#ifdef _WIN32
  if (path &&
      (strcmp(path, "etc/tests/native/c/internal-byvalue-param-import-lowering.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/native-only-register-aggregate-param.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/native-only-two-eightbyte-return.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/transitive-layout-import.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/internal-aggregate-pointer-import-lowering.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/internal-bitfield.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/internal-byvalue-aggregate-import-lowering.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/internal-variadic-import-lowering.nshape") == 0 ||
       strcmp(path, "etc/tests/native/c/native-only-aggregate-return.nshape") == 0 ||
       strcmp(path, "etc/tests/native/nyir/asm-native-only.nshape") == 0 ||
       strcmp(path, "etc/tests/shapes/probes/sys/interact-process.nshape") == 0 ||
       strcmp(path, "etc/tests/shapes/probes/sys/vterm-key-state-open.nshape") == 0))
    return 1;
#endif
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

static int build_trace_argv(char **argv, int max, const char *bin, const char *path,
                            const char *exec_path, const char *std_path, const char *std_bc,
                            const char *matrix_flags, char *flags_buf,
                            char **flags_out, char **expect_out) {
  int argc = 0;
  char *flagv[32];
  int flagc = 0;
  int has_native_backend = 0;
  if (flags_out)
    *flags_out = NULL;
  if (expect_out)
    *expect_out = NULL;
  flags_buf[0] = '\0';
  read_error_meta(path, flags_out, expect_out);
  const char *base_flags = flags_out && *flags_out ? *flags_out : NULL;
  if ((base_flags && *base_flags) || (matrix_flags && *matrix_flags)) {
    snprintf(flags_buf, 1024, "%s%s%s", base_flags ? base_flags : "",
             base_flags && *base_flags && matrix_flags && *matrix_flags ? " " : "",
             matrix_flags ? matrix_flags : "");
    trim_inplace(flags_buf);
    has_native_backend = native_backend_explicit(flags_buf);
    flagc = split_words(flags_buf, flagv, 32);
  }
  if (!append_arg(argv, &argc, max, (char *)bin) ||
      !append_arg(argv, &argc, max, "-trace"))
    return 0;
  const char *warn_arg = test_warn_arg();
  if (warn_arg && !append_arg(argv, &argc, max, (char *)warn_arg))
    return 0;
  if (std_path) {
    if (!append_arg(argv, &argc, max, "--std") ||
        !append_arg(argv, &argc, max, (char *)std_path))
      return 0;
  }
  if (std_bc) {
    if (!append_arg(argv, &argc, max, "--std-bc") ||
        !append_arg(argv, &argc, max, (char *)std_bc))
      return 0;
  }
  if (path_is_native_test(path) && !has_native_backend) {
    if (!append_arg(argv, &argc, max, "--native-backend") ||
        !append_arg(argv, &argc, max, "x86_64"))
      return 0;
  }
  for (int i = 0; i < flagc; i++) {
    if (!append_arg(argv, &argc, max, flagv[i]))
      return 0;
  }
  if (test_is_ownership_error_path(path)) {
    if (!append_arg(argv, &argc, max, "--ownership-strict"))
      return 0;
  }
  if (!append_arg(argv, &argc, max, "--no-progress") ||
      !append_arg(argv, &argc, max, "--color=never"))
    return 0;
  return append_arg(argv, &argc, max, (char *)(exec_path && *exec_path ? exec_path : path));
}

static void run_debugger_replay(const char *debugger, char *const trace_argv[], int timeout_sec) {
  char *argv[192];
  int argc = 0;
  bool is_lldb = strstr(debugger, "lldb") != NULL;
  if (!append_arg(argv, &argc, 192, (char *)debugger))
    return;
  if (is_lldb) {
    append_arg(argv, &argc, 192, "--batch");
    append_arg(argv, &argc, 192, "-o");
    append_arg(argv, &argc, 192, "run");
    append_arg(argv, &argc, 192, "-k");
    append_arg(argv, &argc, 192, "thread list");
    append_arg(argv, &argc, 192, "-k");
    append_arg(argv, &argc, 192, "thread backtrace all");
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
    append_arg(argv, &argc, 192, "--");
  } else {
    append_arg(argv, &argc, 192, "-q");
    append_arg(argv, &argc, 192, "--batch");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "run");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "thread apply all bt full");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "info registers");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "x/i $pc");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "info files");
    append_arg(argv, &argc, 192, "-ex");
    append_arg(argv, &argc, 192, "info sharedlibrary");
    append_arg(argv, &argc, 192, "--args");
  }
  for (int i = 0; trace_argv[i]; i++) {
    if (!append_arg(argv, &argc, 192, trace_argv[i]))
      return;
  }
  int debug_timeout = timeout_sec * 3;
  if (debug_timeout < 30)
    debug_timeout = 30;
  if (debug_timeout > 120)
    debug_timeout = 120;
  int rc = run_debug_argv(argv, debug_timeout, 1);
  printf("debugger replay exit status: %d\n", rc);
}

static void print_core_dump_config(void) {
#if defined(__linux__)
  FILE *f = fopen("/proc/sys/kernel/core_pattern", "r");
  if (f) {
    char line[512];
    if (fgets(line, sizeof(line), f)) {
      trim_inplace(line);
      printf("%s[debug]%s core_pattern=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), line);
    }
    fclose(f);
  }
#elif defined(__APPLE__)
  char path[PATH_MAX];
  size_t n = sizeof(path);
  if (sysctlbyname("kern.corefile", path, &n, NULL, 0) == 0 && path[0])
    printf("%s[debug]%s corefile=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), path);
#elif defined(_WIN32)
  printf("%s[debug]%s Windows crash dumps depend on WER/local dump policy; debugger replay is used when available\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
#endif
}

static void debug_replay_failed_tests(StrVec *failed_paths, const char *bin, const char *std_path,
                                      const char *std_bc, int timeout_sec) {
  if (!failed_paths || failed_paths->len == 0 || !test_debug_failures_enabled())
    return;
  print_section("Failure Replay");
  printf("%s[debug]%s replaying %zu failed test%s with -trace; debugger runs on crash exits; "
         "core dumps enabled where supported\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), failed_paths->len,
         failed_paths->len == 1 ? "" : "s");
  print_core_dump_config();
  const char *debugger = test_debugger_name();
  if (!debugger)
    printf("%s[debug]%s no debugger found in PATH; trace replay only\n", nyt_clr(NYT_GRAY),
           nyt_clr(NYT_RESET));
  for (size_t i = 0; i < failed_paths->len; i++) {
    const char *path = failed_paths->items[i];
    char *materialized_path = NULL;
    const char *exec_path = path;
    if (path && shape_path_is_nshape(path)) {
      materialized_path = materialize_shape_ny_source(path);
      if (!materialized_path) {
        printf("%s[debug]%s cannot materialize shape source for %s\n", nyt_clr(NYT_GRAY),
               nyt_clr(NYT_RESET), disp_path(path));
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
      char flags_buf[1024];
      char *flags = NULL;
      char *expect = NULL;
      char *trace_argv[96];
      if (!build_trace_argv(trace_argv, 96, bin, path, exec_path, std_path, std_bc,
                            matrix_flags, flags_buf, &flags, &expect)) {
        printf("%s[debug]%s cannot build replay argv for %s\n", nyt_clr(NYT_GRAY),
               nyt_clr(NYT_RESET), disp_path(path));
        error_meta_free(flags, expect);
        continue;
      }
      if (matrix_flags && *matrix_flags)
        printf("%s[debug]%s replay flags: %s\n", nyt_clr(NYT_GRAY),
               nyt_clr(NYT_RESET), matrix_flags);
      gh_group_begin("trace replay", path);
      int trace_rc = run_debug_argv(trace_argv, timeout_sec, 0);
      printf("trace replay exit status: %d\n", trace_rc);
      gh_group_end();
      if (trace_rc != 0 && debugger && test_debugger_for_rc(trace_rc)) {
        gh_group_begin("debugger replay", path);
        run_debugger_replay(debugger, trace_argv, timeout_sec);
        gh_group_end();
      }
      error_meta_free(flags, expect);
    }
    free(matrix);
    if (materialized_path) {
      remove(materialized_path);
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

static char *repl_fixture_expect(const char *src) {
  if (!src)
    return NULL;
  const char *p = src;
  for (int line = 0; *p && line < 32; ++line) {
    const char *next = strchr(p, '\n');
    size_t len = next ? (size_t)(next - p) : strlen(p);
    const char marker[] = ";; repl-expect:";
    if (len >= sizeof(marker) - 1 && strncmp(p, marker, sizeof(marker) - 1) == 0) {
      const char *start = p + sizeof(marker) - 1;
      while ((size_t)(start - p) < len && isspace((unsigned char)*start))
        start++;
      const char *end = p + len;
      while (end > start && isspace((unsigned char)end[-1]))
        end--;
      if (end <= start)
        return NULL;
      size_t n = (size_t)(end - start);
      char *out = (char *)malloc(n + 1);
      if (!out)
        return NULL;
      memcpy(out, start, n);
      out[n] = '\0';
      return out;
    }
    if (!next)
      break;
    p = next + 1;
  }
  return NULL;
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
    if (!(line_len >= 8 && strncmp(p, ";; repl-", 8) == 0)) {
      memcpy(out + used, p, line_len);
      used += line_len;
      if (next)
        out[used++] = '\n';
    }
    if (!next)
      break;
    p = next + 1;
  }
  out[used] = '\0';
  return out;
}

static void repl_clean_output_line(const char *line_start, const char *line_end,
                                   char *clean, size_t clean_cap) {
  size_t clean_len = 0;
  if (!clean || clean_cap == 0)
    return;
  for (const char *s = line_start; s < line_end && clean_len + 1 < clean_cap;) {
    unsigned char ch = (unsigned char)*s++;
    if (ch == 0x1b) {
      if (s < line_end && *s == '[') {
        s++;
        while (s < line_end) {
          unsigned char c = (unsigned char)*s++;
          if (c >= 0x40 && c <= 0x7e)
            break;
        }
      }
      continue;
    }
    if (ch < 0x20 || ch == 0x7f)
      continue;
    clean[clean_len++] = (char)ch;
  }
  clean[clean_len] = '\0';
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

static int repl_output_has_expect_substring_clean(const char *out,
                                                  const char *expect) {
  if (!out || !*out || !expect || !*expect)
    return 0;
  char *clean = repl_clean_output_text(out);
  if (!clean)
    return 0;
  int ok = strstr(clean, expect) != NULL;
  free(clean);
  return ok;
}

static int repl_output_has_expect_line(const char *out, const char *expect) {
  if (!out || !*out || !expect || !*expect)
    return 0;
  size_t expect_len = strlen(expect);
  const char *p = out;
  while (*p) {
    const char *line_start = p;
    while (*p && *p != '\n' && *p != '\r')
      p++;
    const char *line_end = p;
    while (*p == '\n' || *p == '\r')
      p++;

    char clean[256];
    repl_clean_output_line(line_start, line_end, clean, sizeof(clean));

    char *start = clean;
    while (*start && isspace((unsigned char)*start))
      start++;
    char *end = clean + strlen(clean);
    while (end > start && isspace((unsigned char)end[-1]))
      *--end = '\0';
    if ((size_t)(end - start) == expect_len && memcmp(start, expect, expect_len) == 0)
      return 1;
  }
  return repl_output_has_expect_substring_clean(out, expect);
}

static int repl_output_has_expect_then_prompt(const char *out, const char *expect) {
  if (!out || !*out || !expect || !*expect)
    return 0;
  size_t expect_len = strlen(expect);
  int seen_expect = 0;
  const char *p = out;
  while (*p) {
    const char *line_start = p;
    while (*p && *p != '\n' && *p != '\r')
      p++;
    const char *line_end = p;
    while (*p == '\n' || *p == '\r')
      p++;

    char clean[256];
    repl_clean_output_line(line_start, line_end, clean, sizeof(clean));
    char *start = clean;
    while (*start && isspace((unsigned char)*start))
      start++;
    char *end = clean + strlen(clean);
    while (end > start && isspace((unsigned char)end[-1]))
      *--end = '\0';
    if (seen_expect && strstr(start, "ny>"))
      return 1;
    if ((size_t)(end - start) == expect_len && memcmp(start, expect, expect_len) == 0)
      seen_expect = 1;
  }
  return 0;
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
  memcpy(*out + *len, buf, n);
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
      kill(pid, SIGKILL);
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
  char *expect = repl_fixture_expect(src);
  char *paste_src = repl_fixture_paste_body(src);
  if (!paste_src) {
    free(src);
    free(expect);
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
    snprintf(why, why_len, "out of memory");
    return 0;
  }
  snprintf(paste_input, paste_len + 1, "%s%s%s", prefix, paste_src, suffix);
  free(src);
  free(paste_src);

  char *argv[12];
  int argc = 0;
  argv[argc++] = (char *)bin;
  argv[argc++] = "-i";
  push_test_warn_arg(argv, &argc, 12);
  if (std_path) {
    argv[argc++] = "--std";
    argv[argc++] = (char *)std_path;
  }
  if (std_bc) {
    argv[argc++] = "--std-bc";
    argv[argc++] = (char *)std_bc;
  }
  argv[argc] = NULL;

  int master = -1;
  pid_t pid = repl_spawn_pty(argv, &master);
  if (pid <= 0 || master < 0) {
    free(paste_input);
    snprintf(why, why_len, "pty spawn failed");
    return 0;
  }
  int flags = fcntl(master, F_GETFL, 0);
  if (flags >= 0)
    fcntl(master, F_SETFL, flags | O_NONBLOCK);

  char *out = NULL;
  size_t out_len = 0, out_cap = 0, sent_paste = 0, sent_quit = 0;
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
      ssize_t w = write(master, paste_input + sent_paste, paste_len - sent_paste);
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

    if (!quit_requested && sent_paste == paste_len) {
      const char *cur = out ? out : "";
      int saw_expect_now = repl_output_has_expect_then_prompt(cur, expect);
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
      kill(pid, SIGKILL);
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
  if (dur_ms)
    *dur_ms = (int)(now_ms() - start_ms);
  int rc = timed_out ? NY_TEST_TIMEOUT_RC : child_status_rc(status);
  int saw_expect = repl_output_has_expect_line(out ? out : "", expect);
  int failed = timed_out ||
               (!saw_expect && (repl_output_has_failure(out) || rc != 0));
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

static int run_error_case(const char *bin, const char *path, const char *std_path,
                          const char *std_bc, int timeout_sec, const char *flags,
                          const char *expect, int *dur_ms, char *why, size_t why_len) {
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
  bool ownership_case = path && strstr(path, "etc/tests/errors/ownership/") != NULL;
  char *flagv[32];
  int flagc = split_words(flags_buf, flagv, 32);

  char *argv[80];
  int argc = 0;
  argv[argc++] = (char *)bin;
  if (std_path) {
    argv[argc++] = "--std";
    argv[argc++] = (char *)std_path;
  }
  if (std_bc) {
    argv[argc++] = "--std-bc";
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
           (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), (long)seq);
#else
  snprintf(tmp, sizeof(tmp), "%s/ny-error-%ld-XXXXXX", nyt_temp_dir(), (long)getpid());
  int fd = mkstemp(tmp);
  if (fd < 0) {
    snprintf(why, why_len, "mkstemp failed");
    if (materialized_path) {
      remove(materialized_path);
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
      ny_setenv("NYTRIX_OWNERSHIP_STRICT", old_ownership_strict ? old_ownership_strict : "", 1);
    else
      ny_unsetenv("NYTRIX_OWNERSHIP_STRICT");
    free(old_ownership);
    free(old_ownership_strict);
  }
  if (!ny_test_proc_valid(pid)) {
    remove(tmp);
    if (materialized_path) {
      remove(materialized_path);
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
    apply_test_child_env();
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
    if (ownership_case) {
      ny_setenv("NYTRIX_OWNERSHIP", "1", 1);
      ny_setenv("NYTRIX_OWNERSHIP_STRICT", "1", 1);
    }
    execv(bin, argv);
    _exit(127);
  }

  if (pid <= 0) {
    close(fd);
    unlink(tmp);
    if (materialized_path) {
      remove(materialized_path);
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
        remove(materialized_path);
        free(materialized_path);
      }
      snprintf(why, why_len, "wait failed");
      return 0;
    }
    if (now_ms() - start_ms >= timeout_ms) {
      timed_out = 1;
      kill(pid, SIGKILL);
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
    remove(materialized_path);
    free(materialized_path);
  }

  if (timed_out) {
    snprintf(why, why_len, "timeout=%ds", timeout_sec);
    free(out);
    return 0;
  }

  if (rc == 0) {
    snprintf(why, why_len, "expected compile failure, got rc=0");
    free(out);
    return 0;
  }

  if (expect && *expect && !strstr(out, expect)) {
    char *nl = strchr(out, '\n');
    if (nl)
      *nl = '\0';
    snprintf(why, why_len, "missing expected diagnostic: %s (rc=%d, first output: %.180s)",
             expect, rc, out && *out ? out : "<empty>");
    free(out);
    return 0;
  }

  free(out);
  return 1;
}

static int run_error_suite(StrVec *files, const char *bin, const char *std_path,
                           const char *std_bc, StrVec *patterns, int timeout_sec, int *passed,
                           int *failed, SuiteStats *stats, CacheDb *cache, int use_cache,
                           TimingVec *timings, StrVec *failed_paths) {
  if (!files || files->len == 0)
    return 0;
  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (strstr(p, patterns->items[k])) {
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

  print_section("Error");
  for (size_t i = 0; i < selected.len; i++) {
    const char *p = selected.items[i];
    char *flags = NULL, *expect = NULL;
    read_error_meta(p, &flags, &expect);
    if (!expect)
      expect = strdup("error");
    uint64_t sig = test_sig(p, bin, std_path, std_bc);
    const char *mode = "error-v2";
    sig = fnv1a_update(sig, mode, strlen(mode));
    if (flags)
      sig = fnv1a_update(sig, flags, strlen(flags));
    if (expect)
      sig = fnv1a_update(sig, expect, strlen(expect));
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
        timings_push(timings, p, dur, "Error");
        char time_label[32];
        format_test_time(time_label, sizeof(time_label), -1);
        print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓", NYT_GREEN,
                                 time_label, p, NULL);
        error_meta_free(flags, expect);
        continue;
      }
    }
    char why[512];
    why[0] = '\0';
    int dur = 0;
    int ok = run_error_case(bin, p, std_path, std_bc, timeout_sec, flags, expect, &dur,
                                   why, sizeof(why));
    if (ok) {
      (*passed)++;
      if (stats)
        stats->passed++;
      char time_label[32];
      format_test_time(time_label, sizeof(time_label), dur);
      print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓", NYT_GREEN,
                               time_label, p, NULL);
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
    if (stats) {
      stats->tests++;
      stats->sum_ms += dur;
      if (dur > stats->max_ms)
        stats->max_ms = dur;
    }
    timings_push(timings, p, dur, "Error");
    error_meta_free(flags, expect);
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

static int auto_test_jobs(void) {
  long logical = ny_cpu_count();
  if (logical <= 0)
    logical = 1;
  int jobs = (int)(logical * 0.5);
  if (logical >= 2 && jobs < 2)
    jobs = 2;
  if (jobs < 1)
    jobs = 1;
  double ram_gib = host_ram_gib();
  if (ram_gib > 0.0) {
    int ram_jobs = (int)(ram_gib / 6.0);
    if (ram_jobs < 1)
      ram_jobs = 1;
    if (jobs > ram_jobs)
      jobs = ram_jobs;
  }
  if (jobs > 32)
    jobs = 32;
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

static uint64_t file_sig(const char *p) {
  struct stat st;
  if (!p || stat(p, &st) != 0)
    return 0;
  uint64_t h = 1469598103934665603ULL;
  h = fnv1a_update(h, p, strlen(p));
  h = fnv1a_update(h, &st.st_mtime, sizeof(st.st_mtime));
  h = fnv1a_update(h, &st.st_size, sizeof(st.st_size));
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

static uint64_t test_sig(const char *path, const char *bin, const char *std_path, const char *std_bc) {
  uint64_t h = 1469598103934665603ULL;
  static uint64_t bin_hash = 0;
  static bool bin_hash_ready = false;
  if (!bin_hash_ready) {
    bin_hash = file_content_hash(bin);
    bin_hash_ready = true;
  }
  uint64_t a = file_sig(path), b = file_sig(bin), c = file_sig(std_path), d = file_sig(std_bc);
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
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  for (size_t i = 0; i < db->len; i++)
    fprintf(f, "%llx\t%d\t%d\t%s\n", (unsigned long long)db->items[i].sig, db->items[i].ok,
            db->items[i].dur_ms, db->items[i].path);
  fclose(f);
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
    if (strncmp(line, "model name", 10) != 0 && strncmp(line, "Hardware", 8) != 0)
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
    return ((double)si.totalram * (double)si.mem_unit) / (1024.0 * 1024.0 * 1024.0);
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
  printf("%s[host]%s os=%s arch=%s cpu=%s cores=%d/%d ram=%.1f GiB jobs=%d/%d\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), host_os_name(), host_arch_name(), cpu,
         physical, logical, ram, jobs > 0 ? jobs : 1, logical > 0 ? logical : 1);
}

static int flags_contain_native_backend_word(const char *flags, const char *word) {
  if (!flags || !word || !*word)
    return 0;
  size_t n = strlen(word);
  const char *p = flags;
  while ((p = strstr(p, word)) != NULL) {
    int left_ok = (p == flags) || isspace((unsigned char)p[-1]);
    char right = p[n];
    int right_ok = right == '\0' || isspace((unsigned char)right) || right == '=';
    if (left_ok && right_ok)
      return 1;
    p += n;
  }
  return 0;
}

static int native_backend_explicit(const char *flags) {
  return flags_contain_native_backend_word(flags, "--native-backend");
}

static int path_is_probe_test(const char *p) {
  return p && (strncmp(p, "probe/", 6) == 0 || strncmp(p, "probes/", 7) == 0 ||
               strstr(p, "/probe/") != NULL || strstr(p, "/probes/") != NULL);
}

static int path_is_native_test(const char *p) {
  return p && strncmp(p, "etc/tests/native/", 17) == 0;
}

static int path_is_stdlib_source(const char *p) {
  if (!p)
    return 0;
  return strncmp(p, "lib/", 4) == 0 || strstr(p, "/lib/") != NULL;
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
  if (p && strncmp(p, "etc/tests/bench/", 16) == 0)
    return "Benchmark";
  if (p && strncmp(p, "etc/tests/runtime/", 18) == 0)
    return "Runtime";
  if (p && strncmp(p, "etc/tests/native/", 17) == 0)
    return "Native";
  if (p && strncmp(p, "etc/tests/interop/", 18) == 0)
    return "Interop";
  if (path_is_probe_test(p))
    return "Probe";
  return fallback ? fallback : "Std";
}

static SuiteStats *stats_for_path(const char *p, SuiteStats *fallback, SuiteStats *benchmark,
                                  SuiteStats *runtime, SuiteStats *native,
                                  SuiteStats *interop, SuiteStats *probe, SuiteStats *std) {
  if (p && strncmp(p, "etc/tests/bench/", 16) == 0)
    return benchmark ? benchmark : fallback;
  if (p && strncmp(p, "etc/tests/runtime/", 18) == 0)
    return runtime ? runtime : fallback;
  if (p && strncmp(p, "etc/tests/native/", 17) == 0)
    return native ? native : fallback;
  if (p && strncmp(p, "etc/tests/interop/", 18) == 0)
    return interop ? interop : fallback;
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
  if (strncmp(p, "etc/tests/bench/", 16) == 0)
    return 5000;
  if (strstr(p, "etc/tests/runtime/values/bigint.ny") || strstr(p, "etc/tests/runtime/language/attr.ny") || strstr(p, "etc/tests/interop/ny/sizeof.ny") ||
      strstr(p, "etc/tests/interop/ny/asm.ny"))
    return 5000;
  if (strstr(p, "etc/tests/runtime/compiler/comptime.ny"))
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
  const char *title = name ? name : "Suite";
  int title_len = (int)strlen(title);
  int inner = title_len + 6;
  int width = 64;
  if (inner > width - 2)
    width = inner + 2;
  int left = (width - inner) / 2;
  int right = width - inner - left;
  printf("%s", nyt_clr(NYT_GRAY));
  for (int i = 0; i < left; i++)
    fputc('-', stdout);
  printf("%s [ %s%s%s ] %s", nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), title, nyt_clr(NYT_RESET),
         nyt_clr(NYT_GRAY));
  for (int i = 0; i < right; i++)
    fputc('-', stdout);
  printf("%s\n", nyt_clr(NYT_RESET));
}

static int run_repl_suite(StrVec *files, const char *bin, const char *std_path,
                          const char *std_bc, StrVec *patterns,
                          int timeout_sec, int *passed, int *failed,
                          SuiteStats *stats, TimingVec *timings,
                          StrVec *failed_paths) {
  if (!files || files->len == 0)
    return 0;
  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (strstr(p, patterns->items[k])) {
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
    int ok = run_repl_paste_case(bin, selected.items[i], std_path, std_bc,
                                 timeout_sec, &dur, why, sizeof(why));
#ifdef __APPLE__
    int macos_replay = 0;
    if (!ok) {
      int replay_rc = run_one_blocking(bin, selected.items[i], std_path, std_bc,
                                       timeout_sec, 1);
      if (replay_rc == 0) {
        ok = 1;
        macos_replay = 1;
      }
    }
#endif
    if (ok == 2) {
      char time_label[32];
      char suffix[256];
      int pct = (int)(((i + 1) * 100) / (selected.len ? selected.len : 1));
      format_test_time(time_label, sizeof(time_label), dur);
      snprintf(suffix, sizeof(suffix), "(%s)", why[0] ? why : "skipped");
      print_test_progress_line(pct, "-", NYT_GRAY, "-", NYT_GRAY, "-", NYT_GRAY,
                               time_label, selected.items[i], suffix);
      continue;
    }
    if (stats) {
      stats->tests++;
      stats->sum_ms += dur;
      if (dur > stats->max_ms)
        stats->max_ms = dur;
    }
    timings_push(timings, selected.items[i], dur, "Repl");
    int pct = (int)(((i + 1) * 100) / (selected.len ? selected.len : 1));
    if (ok) {
      (*passed)++;
      if (stats)
        stats->passed++;
#ifdef __APPLE__
      if (macos_replay) {
        char time_label[32];
        format_test_time(time_label, sizeof(time_label), dur);
        print_test_progress_line(pct, "~", NYT_YELLOW, "✓", NYT_GREEN, "✓", NYT_GREEN,
                                 time_label, selected.items[i], NULL);
        continue;
      }
#endif
      char time_label[32];
      format_test_time(time_label, sizeof(time_label), dur);
      print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓", NYT_GREEN,
                               time_label, selected.items[i], NULL);
    } else {
      (*failed)++;
      sv_push_unique(failed_paths, selected.items[i]);
      char time_label[32];
      char suffix[256];
      format_test_time(time_label, sizeof(time_label), dur);
      snprintf(suffix, sizeof(suffix), "(%s)", why[0] ? why : "repl paste failed");
      print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗", NYT_RED,
                               time_label, selected.items[i], suffix);
    }
  }
  sv_free(&selected);
  return 0;
}

static void print_bench_summary(const char *path) {
  if (!path || !*path) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    if (strstr(line, "[BENCH]")) {
      printf("      %s%s%s", nyt_clr(NYT_CYAN), line + 8, nyt_clr(NYT_RESET));
    }
  }
  fclose(f);
}

static int run_suite(const char *suite_name, StrVec *files, const char *bin, const char *std_path,
                     const char *std_bc, StrVec *patterns, int jobs, int timeout_sec, int *passed,
                     int *failed, SuiteStats *stats, SuiteStats *benchmark_stats,
                     SuiteStats *runtime_stats, SuiteStats *native_stats,
                     SuiteStats *interop_stats, SuiteStats *probe_stats, SuiteStats *std_stats,
                     CacheDb *cache, int use_cache, TimingVec *timings, StrVec *failed_paths) {
  if (!files || files->len == 0)
    return 0;

  StrVec selected = {0};
  for (size_t i = 0; i < files->len; i++) {
    const char *p = files->items[i];
    /*
     * Generator-only fuzz shapes remain outside ny-test.  Executable shapes
     * provide either a literal Ny block or an embedded AWK Ny generator.
     */
    if (p && shape_path_is_nshape(p)) {
      char *source = shape_source_block(p, "ny");
      if (!source) {
        int declared = 0;
        char why[128] = {0};
        char *program = shape_embedded_awk_block(p, &declared, why, sizeof(why));
        free(program);
        if (!declared)
          continue;
      }
      free(source);
    }
    int match = 1;
    if (patterns && patterns->len > 0) {
      match = 0;
      for (size_t k = 0; k < patterns->len; k++) {
        if (strstr(p, patterns->items[k])) {
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
  g_sort_cache = cache;
  qsort(selected.items, selected.len, sizeof(char *), path_duration_cmp_desc);
  g_sort_cache = NULL;

  print_section(suite_name);
  size_t total = selected.len;
  size_t completed = 0;
  for (size_t i = 0; i < selected.len; i++) {
    const char *p = selected.items[i];
    uint64_t sig = test_sig(p, bin, std_path, std_bc);
    if (use_cache) {
      CacheRow *row = cache_find(cache, p);
      if (row && row->ok == 1 && row->sig == sig) {
        SuiteStats *row_stats =
            stats_for_path(p, stats, benchmark_stats, runtime_stats, native_stats,
                           interop_stats, probe_stats, std_stats);
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
        timings_push(timings, p, dur, row_suite);
        char time_label[32];
        format_test_time(time_label, sizeof(time_label), -1);
        print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓", NYT_GREEN,
                                 time_label, p, NULL);
        free(selected.items[i]);
        selected.items[i] = NULL;
      }
    }
  }
  if (jobs < 1)
    jobs = 1;

  typedef struct {
    ny_test_proc_t pid;
    const char *path;
    char tmp_out[PATH_MAX];
    char materialized_path[PATH_MAX];
    double start_ms;
    int active;
  } Running;

  Running *run = (Running *)calloc((size_t)jobs, sizeof(Running));
  unsigned char *started = (unsigned char *)calloc(total ? total : 1, sizeof(unsigned char));
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
      const char *exec_path = p;
      int is_bench = strncmp(p, "etc/tests/bench/", 16) == 0;
      if (p && shape_path_is_nshape(p)) {
        char *mat = materialize_shape_ny_source(p);
        if (!mat) {
          (*failed)++;
          sv_push_unique(failed_paths, p);
          completed++;
          continue;
        }
        snprintf(run[i].materialized_path, sizeof(run[i].materialized_path), "%s", mat);
        free(mat);
        exec_path = run[i].materialized_path;
      }
      if (is_bench) {
        make_test_capture_tmp(run[i].tmp_out, sizeof(run[i].tmp_out), "bench");
      }
      ny_test_proc_t pid = run_one_start(bin, exec_path, std_path, std_bc, is_bench ? run[i].tmp_out : NULL);
      if (!ny_test_proc_valid(pid)) {
        if (run[i].materialized_path[0])
          remove(run[i].materialized_path);
        (*failed)++;
        sv_push_unique(failed_paths, p);
        completed++;
        continue;
      }
      run[i].pid = pid;
      run[i].path = p;
      run[i].start_ms = now_ms();
      run[i].active = 1;
      active_count++;
      if (test_requires_host_exclusive(p))
        exclusive_active = 1;
    }

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
      double timeout_ms =
          (double)timeout_sec * 1000.0 + NY_TEST_PARALLEL_TIMEOUT_GRACE_MS;
      for (int i = 0; i < jobs; i++) {
        if (!run[i].active)
          continue;
        if (t - run[i].start_ms < timeout_ms)
          continue;
        timed_out = 1;
        done = run[i].pid;
#ifdef _WIN32
        TerminateProcess(done, NY_TEST_TIMEOUT_RC);
        WaitForSingleObject(done, INFINITE);
        st = NY_TEST_TIMEOUT_RC;
#else
        kill(done, SIGKILL);
        while (waitpid(done, &st, 0) < 0 && errno == EINTR) {
        }
#endif
        break;
      }
      if (!ny_test_proc_valid(done)) {
        poll_sleep();
        continue;
      }
    }
    for (int i = 0; i < jobs; i++) {
      if (!run[i].active || !ny_test_proc_eq(run[i].pid, done))
        continue;
      int rc = timed_out ? NY_TEST_TIMEOUT_RC : child_status_rc(st);
      int dur = (int)(now_ms() - run[i].start_ms);
      SuiteStats *row_stats =
          stats_for_path(run[i].path, stats, benchmark_stats, runtime_stats, native_stats,
                         interop_stats, probe_stats, std_stats);
      const char *row_suite = suite_for_path(run[i].path, suite_name);
      int retried = 0;
      if (rc == 0)
        rc = object_link_run_check(run[i].path);
      if (rc != 0 && !timed_out) {
        int retry_rc =
            run_one_blocking(bin, run[i].path, std_path, std_bc, timeout_sec, retry_trace_enabled());
        retried = 1;
        if (retry_rc == 0)
          rc = 0;
        else if (retry_rc == NY_TEST_TIMEOUT_RC)
          rc = retry_rc;
      }
      completed++;
      int pct = (int)((completed * 100) / (total ? total : 1));
      if (rc == 0) {
        (*passed)++;
        if (row_stats)
          row_stats->passed++;
        if (retried) {
          char time_label[32];
          format_test_time(time_label, sizeof(time_label), dur);
          print_test_progress_line(pct, "~", NYT_YELLOW, "✓", NYT_GREEN, "✓", NYT_GREEN,
                                   time_label, run[i].path, NULL);
        } else {
          char time_label[32];
          format_test_time(time_label, sizeof(time_label), dur);
          print_test_progress_line(pct, "✓", NYT_GREEN, "✓", NYT_GREEN, "✓", NYT_GREEN,
                                   time_label, run[i].path, NULL);
        }
        if (run[i].tmp_out[0]) {
          print_bench_summary(run[i].tmp_out);
          remove(run[i].tmp_out);
        }
        if (run[i].materialized_path[0])
          remove(run[i].materialized_path);
      } else {
        (*failed)++;
        sv_push_unique(failed_paths, run[i].path);
        if (run[i].materialized_path[0])
          remove(run[i].materialized_path);
        if (timed_out || rc == NY_TEST_TIMEOUT_RC) {
          char time_label[32];
          char suffix[64];
          format_test_time(time_label, sizeof(time_label), dur);
          snprintf(suffix, sizeof(suffix), "(timeout=%ds)", timeout_sec);
          print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗", NYT_RED,
                                   time_label, run[i].path, suffix);
        } else {
          char time_label[32];
          char suffix[64];
          format_test_time(time_label, sizeof(time_label), dur);
          snprintf(suffix, sizeof(suffix), "(rc=%d)", rc);
          print_test_progress_line(pct, "✗", NYT_RED, "✗", NYT_RED, "✗", NYT_RED,
                                   time_label, run[i].path, suffix);
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
      }
      timings_push(timings, run[i].path, dur, row_suite);
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
  static const char success[] =
      "shape embedded_awk {\n"
      "  source ny generate awk <<'AWK'\n"
      "BEGIN { print \"use std.core\"; print \"print(\\\"quoted\\\\nline\\\")\" }\n"
      "AWK\n}\n";
  static const char malformed[] =
      "shape malformed {\n  source ny generate awk <<'AWK'\nBEGIN { print 1 }\n}\n";
  static const char missing[] = "shape missing {\n  expect compile_and_run\n}\n";
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
  if (!text || strcmp(text, "use std.core\nprint(\"quoted\\nline\")\n") != 0) {
    fprintf(stderr, "shape generator selftest: generated source mismatch\n");
    ok = 0;
  }
  free(text);
  if (generated) {
    remove(generated);
    free(generated);
  }

  int declared = 0;
  char why[128] = {0};
  char *program = shape_embedded_awk_block(paths[1], &declared, why, sizeof(why));
  if (program || !declared || !strstr(why, "unterminated")) {
    fprintf(stderr, "shape generator selftest: malformed block was not rejected\n");
    ok = 0;
  }
  free(program);
  declared = 0;
  why[0] = '\0';
  program = shape_embedded_awk_block(paths[2], &declared, why, sizeof(why));
  if (program || declared) {
    fprintf(stderr, "shape generator selftest: missing generator was not detected\n");
    ok = 0;
  }
  free(program);
  for (size_t i = 0; i < 3; i++)
    remove(paths[i]);
  printf("shape generator selftest: %s\n", ok ? "passed" : "failed");
  return ok ? 0 : 1;
}

static char *atoms_block_field(const char *block, const char *block_end, const char *key) {
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
    path = "etc/tests/runtime/reader/atoms.nshape";
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
    memcpy(name, atom_line, nlen);
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
    int have_out_fd = make_test_capture_tmp(out_path, sizeof(out_path), "reader-atom");
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
  printf("Reader atoms: %d total | %d passed | %d failed | %d xfail (%s)\n", total,
         passed, failed, xfailed, path);
  return failed ? 1 : 0;
}

static const char *bench_dir_root(void);
static int run_list_bench(const char *root_dir) {
  const char *root = bench_dir_root();
  if (!root)
    root = ".";
  StrVec shapes = {0};
  char bench_dir[PATH_MAX];
  snprintf(bench_dir, sizeof(bench_dir), "%s/etc/tests/bench",
           root_dir && *root_dir ? root_dir : root);
  collect_ny(bench_dir, &shapes);
  qsort(shapes.items, shapes.len, sizeof(char *), path_lex_cmp);
  for (size_t i = 0; i < shapes.len; ++i) {
    const char *base = strrchr(shapes.items[i], '/');
    base = base ? base + 1 : shapes.items[i];
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
  collect_ny(root_dir && *root_dir ? root_dir : "etc/tests/", &shapes);
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
  int timeout_sec = NY_TEST_DEFAULT_TIMEOUT_SEC;
  int phase_times = 0;
  int trace_ir = 0;
  int failures_only = 0;
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
  const char *bench_opt_level = "O2";
  const char *bench_tier = "opt";
  const char *bench_engine = "mcjit";
  const char *bench_target = "x86_64";
  const char *bench_compile_profile = "peak";
  const char *bench_cache = NULL;
  int bench_runs = 1;
  int bench_warmup = 0;
  int bench_timeout = 15;
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
      printf("%susage:%s %sny test%s %s[options] [files ...]%s\n\n",
             nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
      printf("  %s--bin BIN --jobs N --timeout SEC --pattern PAT%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--smoke --no-smoke --with-stdlib --no-stdlib%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--std PATH --std-bc PATH --triple T --emulator CMD%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--bench [--bench-run N --bench-warmup N --bench-opt LVL --bench-tier TIER%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-engine aot|mcjit|orc|native|all (default mcjit) --bench-target TARGET%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-cache cold|warm --bench-correctness%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-compiler NAME --bench-compile-profile PROFILE%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-timeout SEC --bench-verbose --bench-show-ir --bench-show-asm%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-show-passes --bench-profile --bench-hw-counters --bench-runtime-counters%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s        --bench-compare-llvm --bench-no-compare-llvm%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--bench-out-csv PATH --bench-out-json PATH --bench-out-md PATH%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("%snotes:%s timeout defaults to 60s and is capped at 300s; error tests live under %setc/tests/errors%s\n",
             nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
      return 0;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
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
    } else if (ny_arg_match_with_value(a, "--std-bc")) {
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
      if (!ny_arg_take_int(a, &i, argc, argv, 0, 1024, &jobs, "jobs", err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
    } else if (ny_arg_match_with_value(a, "--timeout")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, NY_TEST_MAX_TIMEOUT_SEC, &timeout_sec, "timeout",
                           err, sizeof(err))) {
        nyt_err("ny-test", "%s", err);
        sv_free(&patterns);
        return 2;
      }
    } else if (!strcmp(a, "--smoke"))
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
    else if (!strcmp(a, "--debug-failures"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "1", 1);
    else if (!strcmp(a, "--no-debug-failures"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "0", 1);
    else if (!strcmp(a, "--debugger-all"))
      ny_setenv("NYTRIX_TEST_DEBUGGER_ALL", "1", 1);
    else if (!strcmp(a, "--bench") || !strcmp(a, "--bench-is")) {
      bench_mode = 1;
    } else if (ny_arg_match_with_value(a, "--bench-run")) {
      int v = 0;
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 64, &v, "bench-run", err, sizeof(err)))
        return 2;
      bench_runs = v;
    } else if (ny_arg_match_with_value(a, "--bench-warmup")) {
      int v = 0;
      if (!ny_arg_take_int(a, &i, argc, argv, 0, 16, &v, "bench-warmup", err, sizeof(err)))
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
      if (strcmp(v, "aot") != 0 && strcmp(v, "mcjit") != 0 && strcmp(v, "orc") != 0 &&
          strcmp(v, "native") != 0 && strcmp(v, "all") != 0) {
        nyt_err("ny-test", "--bench-engine: expected aot|mcjit|orc|native|all, got '%s'", v);
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
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 3600, &bench_timeout, "bench-timeout", err,
                           sizeof(err)))
        return 2;
    } else if (!strcmp(a, "--bench-verbose"))
      bench_verbose = 1;
    else if (!strcmp(a, "--bench-show-ir"))
      bench_show_ir = 1;
    else if (!strcmp(a, "--bench-show-asm"))
      bench_show_asm = 1;
    else if (!strcmp(a, "--bench-show-passes"))
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
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_csv, err, sizeof(err)))
        return 2;
    }    else if (!strcmp(a, "--budget-fail"))
      bench_budget_fail = 1;
    else if (ny_arg_match_with_value(a, "--bench-out-json")) {
      bench_mode = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_json, err, sizeof(err)))
        return 2;
    } else if (ny_arg_match_with_value(a, "--bench-out-md")) {
      bench_mode = 1;
      if (!ny_arg_take_value(a, &i, argc, argv, &bench_output_md, err, sizeof(err)))
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
      if (is_dir(a))
        collect_ny(a, &files);
      else
        sv_push(&files, a);
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
        nyt_err("ny-test", "bench: --bench-engine all cannot write --bench-out-* files; use one engine");
        sv_free(&patterns);
        sv_free(&files);
        return 2;
      }
      const char *engines[] = {"aot", "mcjit", "orc", "native"};
      int all_rc = 0;
      for (size_t e = 0; e < sizeof(engines) / sizeof(engines[0]); e++) {
        printf("\n");
        int engine_rc = run_benchmarks(bin, NULL, bench_opt_level, bench_tier, engines[e],
                                       bench_target, bench_runs, bench_warmup, bench_timeout,
                                       bench_verbose, bench_show_ir, bench_show_asm,
                                       bench_show_passes, bench_profile, bench_compare_llvm,
                                       bench_correctness, NULL, NULL, NULL,
                                       bench_compile_profile, bench_budget_fail,
                                       &files, &patterns);
        if (engine_rc != 0)
          all_rc = engine_rc;
      }
      sv_free(&patterns);
      sv_free(&files);
      return all_rc;
    }
    int bench_rc = run_benchmarks(bin, NULL, bench_opt_level, bench_tier, bench_engine,
                                  bench_target, bench_runs, bench_warmup, bench_timeout,
                                  bench_verbose, bench_show_ir, bench_show_asm,
                                  bench_show_passes, bench_profile, bench_compare_llvm,
                                  bench_correctness, bench_output_csv, bench_output_json,
                                  bench_output_md, bench_compile_profile, bench_budget_fail,
                                  &files, &patterns);
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
    collect_ny("etc/tests/runtime", &files);
    collect_ny("etc/tests/errors", &files);
    collect_ny("etc/tests/bench", &files);
    collect_ny("etc/tests/native", &files);
    collect_ny("etc/tests/interop", &files);
    collect_ny("etc/tests/shapes", &files);
    collect_ny("etc/tests/runtime/reader", &files);
    if (with_stdlib)
      collect_ny("lib", &files);
    expand_multi_shape_files(&files);
  } else {
    /*
     * Explicitly selected files may also be multi-shape files.
     */
    expand_multi_shape_files(&files);
  }

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
  bool native_cache_on = !(native_cache && (*native_cache == '1' || strcmp(native_cache, "true") == 0));
  bool jit_cache_on = !(jit_cache && (*jit_cache == '0' || strcmp(jit_cache, "false") == 0));
  bool aot_cache_on = !(aot_cache && (*aot_cache == '0' || strcmp(aot_cache, "false") == 0));
  bool std_cache_on = !(std_cache && (*std_cache == '0' || strcmp(std_cache, "false") == 0));
  printf("%s[mode]%s real timeout=%ds result_cache=%s native_cache=%s jit_cache=%s aot_cache=%s "
         "std_cache=%s\n",
         nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), timeout_sec, use_cache ? "on" : "off",
         native_cache_on ? "on" : "off", jit_cache_on ? "on" : "off",
         aot_cache_on ? "on" : "off", std_cache_on ? "on" : "off");
  print_host_line(jobs);
  const char *pj = getenv("NYTRIX_TEST_PROFILE_JSON");
  const char *td = getenv("NYTRIX_TEST_TRACE_DIR");
  if (pj && *pj)
    printf("%s[trace]%s profile_json=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), pj);
  if (td && *td)
    printf("%s[trace]%s trace_dir=%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), td);

  StrVec benchmark = {0}, runtime = {0}, native = {0}, interop = {0}, repl = {0}, probe = {0}, error_tests = {0}, std = {0};
  size_t skipped_system_stdlib = 0;
  size_t skipped_native_host = 0;
  size_t skipped_native_platform = 0;
  size_t skipped_ci = 0;
  size_t skipped_web_browser = 0;
  const int ci_mode = test_env_truthy("CI") || test_env_truthy("GITHUB_ACTIONS");
  const int skip_system_stdlib =
      test_env_truthy("NYTRIX_TEST_SKIP_SYSTEM_STDLIB");
  SuiteStats sb = {0}, sr = {0}, sn = {0}, si = {0}, srepl = {0}, sp = {0}, se = {0}, ss = {0};
  char cache_path[PATH_MAX];
  nyt_path_join(cache_path, sizeof(cache_path), nyt_default_cache_root_dir(),
                "test-results.tsv");
  if (use_cache)
    cache_load(&cache, cache_path);
  for (size_t i = 0; i < limit; i++) {
    const char *p = files.items[i];
    if (ci_mode && shape_skips_ci(p))
      skipped_ci++;
    else if (test_is_unsupported_native_platform(p))
      skipped_native_platform++;
    else if (test_is_unsupported_native_host(p))
      skipped_native_host++;
    else if (strncmp(p, "etc/tests/bench/", 16) == 0)
      sv_push(&benchmark, p);
    else if (strncmp(p, "etc/tests/runtime/", 18) == 0)
      sv_push(&runtime, p);
    else if (strncmp(p, "etc/tests/native/", 17) == 0) {
      if (strncmp(p, "etc/tests/native/web/", 21) == 0)
        skipped_web_browser++;
      else
        sv_push(&native, p);
    }
    else if (strncmp(p, "etc/tests/interop/", 18) == 0)
      sv_push(&interop, p);
    else if (path_is_probe_test(p))
      sv_push(&probe, p);
    else if (strncmp(p, "etc/tests/errors/", 17) == 0)
      sv_push(&error_tests, p);
    else if (skip_system_stdlib && test_is_optional_system_stdlib(p))
      skipped_system_stdlib++;
    else
      sv_push(&std, p);
  }

  if (skipped_system_stdlib > 0)
    printf("%s[note]%s skipped %zu optional system stdlib modules (UI/audio/clipboard)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_system_stdlib);
  if (skipped_native_host > 0)
    printf("%s[note]%s skipped %zu native execution fixtures for a different host architecture\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_native_host);
  if (skipped_native_platform > 0)
    printf("%s[note]%s skipped %zu native fixtures requiring another host object/runtime format\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_native_platform);
  if (skipped_ci > 0)
    printf("%s[note]%s skipped %zu nshape fixtures marked ci skip\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_ci);
  if (skipped_web_browser > 0)
    printf("%s[note]%s skipped %zu browser-only fixtures under etc/tests/native/web (run via ./make web-test)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET), skipped_web_browser);

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

  run_suite("Tests", &selected_all, bin, std_path, std_bc, &patterns, jobs, timeout_sec, &passed,
            &failed, NULL, &sb, &sr, &sn, &si, &sp, &ss, &cache, use_cache, &timings, &failed_paths);
  sv_free(&selected_all);
  run_repl_suite(&repl, bin, std_path, std_bc, &patterns, timeout_sec, &passed, &failed,
                 &srepl, &timings, &failed_paths);
  run_error_suite(&error_tests, bin, std_path, std_bc, &patterns, timeout_sec, &passed,
                  &failed, &se, &cache, use_cache, &timings, &failed_paths);

  if (!with_stdlib && (files.len == 0 || std.len == 0))
    printf("%s[note]%s stdlib sweep disabled (use --with-stdlib or NYTRIX_TEST_WITH_STDLIB=1)\n",
           nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));

  if (use_cache)
    cache_save(&cache, cache_path);

  if (pj && *pj) {
    FILE *f = fopen(pj, "wb");
    if (f) {
      fprintf(f, "{\n");
      fprintf(f, "  \"version\": 1,\n");
      fprintf(f, "  \"suites\": {\n");
      fprintf(f, "    \"Benchmark\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n", sb.tests,
              sb.passed, sb.sum_ms, sb.max_ms);
      fprintf(f, "    \"Runtime\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n", sr.tests,
              sr.passed, sr.sum_ms, sr.max_ms);
      fprintf(f, "    \"Native\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n", sn.tests,
              sn.passed, sn.sum_ms, sn.max_ms);
      fprintf(f, "    \"Interop\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n", si.tests,
              si.passed, si.sum_ms, si.max_ms);
      fprintf(f, "    \"Repl\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n",
              srepl.tests, srepl.passed, srepl.sum_ms, srepl.max_ms);
      fprintf(f, "    \"Probe\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n", sp.tests,
              sp.passed, sp.sum_ms, sp.max_ms);
      fprintf(f, "    \"Error\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d},\n",
              se.tests, se.passed, se.sum_ms, se.max_ms);
      fprintf(f, "    \"Std\": {\"tests\": %d, \"passed\": %d, \"sum_ms\": %d, \"max_ms\": %d}\n", ss.tests,
              ss.passed, ss.sum_ms, ss.max_ms);
      fprintf(f, "  },\n");
      fprintf(f, "  \"timings\": [\n");
      for (size_t i = 0; i < timings.len; i++) {
        const TimingRow *t = &timings.items[i];

        fprintf(f, "    {\"path\": \"%s\", \"ms\": %d, \"suite\": \"%s\"}%s\n", t->path ? t->path : "",
                t->ms, t->suite ? t->suite : "", (i + 1 < timings.len) ? "," : "");
      }
      fprintf(f, "  ]\n");
      fprintf(f, "}\n");
      fclose(f);
    }
  }

  print_section("Timing Summary");
  printf("%sSuite      Tests  Pass    Total     Avg      Max%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
  if (sb.tests > 0)
    printf("Benchmark  %5d %5d %8dms %7dms %7dms\n", sb.tests, sb.passed, sb.sum_ms,
           sb.tests ? (sb.sum_ms / sb.tests) : 0, sb.max_ms);
  if (sr.tests > 0)
    printf("Runtime    %5d %5d %8dms %7dms %7dms\n", sr.tests, sr.passed, sr.sum_ms,
           sr.tests ? (sr.sum_ms / sr.tests) : 0, sr.max_ms);
  if (sn.tests > 0)
    printf("Native     %5d %5d %8dms %7dms %7dms\n", sn.tests, sn.passed, sn.sum_ms,
           sn.tests ? (sn.sum_ms / sn.tests) : 0, sn.max_ms);
  if (si.tests > 0)
    printf("Interop    %5d %5d %8dms %7dms %7dms\n", si.tests, si.passed, si.sum_ms,
           si.tests ? (si.sum_ms / si.tests) : 0, si.max_ms);
  if (srepl.tests > 0)
    printf("Repl       %5d %5d %8dms %7dms %7dms\n", srepl.tests, srepl.passed, srepl.sum_ms,
           srepl.tests ? (srepl.sum_ms / srepl.tests) : 0, srepl.max_ms);
  if (sp.tests > 0)
    printf("Probe      %5d %5d %8dms %7dms %7dms\n", sp.tests, sp.passed, sp.sum_ms,
           sp.tests ? (sp.sum_ms / sp.tests) : 0, sp.max_ms);
  if (se.tests > 0)
    printf("Error      %5d %5d %8dms %7dms %7dms\n", se.tests, se.passed, se.sum_ms,
           se.tests ? (se.sum_ms / se.tests) : 0, se.max_ms);
  if (ss.tests > 0)
    printf("Std        %5d %5d %8dms %7dms %7dms\n", ss.tests, ss.passed, ss.sum_ms,
           ss.tests ? (ss.sum_ms / ss.tests) : 0, ss.max_ms);

  if (timings.len > 0) {
    printf("%sTop slow tests:%s\n", nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
    qsort(timings.items, timings.len, sizeof(TimingRow), timing_row_cmp_desc);
    size_t top = timings.len < 8 ? timings.len : 8;
    for (size_t i = 0; i < top; i++)
      printf("  %zu. %6dms  %s [%s]\n", i + 1, timings.items[i].ms, disp_path(timings.items[i].path),
             timings.items[i].suite ? timings.items[i].suite : "Suite");
  }

  nyt_rule(stdout);
  printf("Total: %d | %s%d passed%s | %s%d failed%s in %dms\n", passed + failed,
         nyt_clr(NYT_GREEN), passed, nyt_clr(NYT_RESET), failed ? nyt_clr(NYT_RED) : nyt_clr(NYT_GREEN),
         failed, nyt_clr(NYT_RESET), (int)(now_ms() - suite_started_ms));
  if (failed)
    debug_replay_failed_tests(&failed_paths, bin, std_path, std_bc, timeout_sec);
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

  return failed ? 1 : 0;
}


/*
 * Benchmark Comparison infrastructure
 *
 * `ny-test --bench [PATTERN ...]` runs the bench .nshape fixtures and compares
 * the self-reported `elapsed_ns=` value of the Nytrix compile-and-run against
 * the side-by-side C translation (c-vs-ny fixtures) built with the host cc.
 * Shapes without a C block report timing only.
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

static double bench_parse_elapsed(const char *out) {
  if (!out)
    return -1.0;
  const char *ns = strstr(out, "elapsed_ns=");
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
  return ns ? v / 1e6 : v;
}
/*
 * Copy a line-oriented result marker such as `checksum=...` without
 * interpreting its payload.  Checksums are intentionally compared as text:
 * fixtures choose their own scalar/string representation.
 */
static int bench_parse_marker(const char *out, const char *label, char *dst, size_t cap) {
  if (!out || !label || !dst || cap == 0)
    return 0;
  const char *p = strstr(out, label);
  if (!p)
    return 0;
  p += strlen(label);
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
  memcpy(dst, p, len);
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
 */
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
    apply_test_child_env();
    if (output_path) {
      int fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        *peak_rss_kb = rss > (unsigned long long)SIZE_MAX
                           ? SIZE_MAX
                           : (size_t)rss;
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
        *peak_rss_kb = rss > (unsigned long long)SIZE_MAX
                           ? SIZE_MAX
                           : (size_t)rss;
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
  int checksum_seen;
  int checksum_consistent;
  char checksum[128];
  double compile_values[64];
  size_t compile_count;
  double compile_ms;
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
    if (s->median > 0.0)
      s->noise_pct = ((s->p95 - s->median) / s->median) * 100.0;
    s->unstable = s->noise_pct > bench_noise_limit_pct();
  } else {
    s->p95 = -1.0;
  }
}

/*
 * Run `argv`, discard warm-ups, and measure self-reported runtime plus
 * compiler timing over the requested samples.
 */
static bench_stats_t bench_measure_self_timed(char *const argv[], int timeout_sec, int warmup,
                                              int runs) {
  bench_stats_t s = {.checksum_consistent = 1, .last_rc = 127};
  double wall_start = now_ms();
  int total = warmup + runs;
  for (int i = 0; i < total && s.count < 64; i++) {
    char tmp[PATH_MAX];
    if (make_test_capture_tmp(tmp, sizeof(tmp), "bench") < 0) {
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
    s.last_rc = rc;
    if (rc != 0) {
      s.failure_count++;
      if (rc == NY_TEST_TIMEOUT_RC)
        s.timeout_count++;
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
      double v = bench_parse_elapsed(out);
      if (v >= 0.0 && s.count < 64)
        s.values[s.count++] = v;
      double compile_ms = bench_parse_phase_ms(out, "Total time:");
      if (compile_ms >= 0.0 && s.compile_count < 64)
        s.compile_values[s.compile_count++] = compile_ms;
      double jit_compile_ms = bench_parse_phase_ms(out, "JIT Compile:");
      if (jit_compile_ms >= 0.0 && s.jit_compile_count < 64)
        s.jit_compile_values[s.jit_compile_count++] = jit_compile_ms;
      double jit_run_ms = bench_parse_phase_ms(out, "JIT Run:");
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
  if (s.jit_compile_count > 0)
    s.jit_compile_ms = bench_median(s.jit_compile_values, (int)s.jit_compile_count);
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
  return "-O2";
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
  return materialize_shape_ny_source(shape_path);
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
  char c_compiler[PATH_MAX];
  char c_compiler_version[256];
  char c_flags[512];
  char compiler_revision[128];
} BenchEnvironment;

static void bench_capture_first_line(char *const argv[], char *dst, size_t cap) {
  if (!dst || cap == 0)
    return;
  snprintf(dst, cap, "unknown");
  char tmp[PATH_MAX];
  if (make_test_capture_tmp(tmp, sizeof(tmp), "bench-meta") < 0)
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

static void bench_collect_environment(const char *root, const char *target,
                                      const char *opt_level, BenchEnvironment *env) {
  if (!env)
    return;
  memset(env, 0, sizeof(*env));
  snprintf(env->os, sizeof(env->os), "%s", host_os_name());
  snprintf(env->arch, sizeof(env->arch), "%s", host_arch_name());
  host_cpu_name(env->cpu_model, sizeof(env->cpu_model));
  snprintf(env->target, sizeof(env->target), "%s", target && *target ? target : "default");
  bench_target_features(env->target_features, sizeof(env->target_features));

  char cc[PATH_MAX];
  if (bench_find_cc(cc, sizeof(cc))) {
    snprintf(env->c_compiler, sizeof(env->c_compiler), "%s", cc);
    char *cc_argv[] = {cc, "--version", NULL};
    bench_capture_first_line(cc_argv, env->c_compiler_version,
                             sizeof(env->c_compiler_version));
  } else {
    snprintf(env->c_compiler, sizeof(env->c_compiler), "unknown");
    snprintf(env->c_compiler_version, sizeof(env->c_compiler_version), "unknown");
  }
  const char *extra = getenv("CFLAGS");
  snprintf(env->c_flags, sizeof(env->c_flags), "%s -march=native%s%s",
           bench_opt_for(opt_level), extra && *extra ? " " : "", extra && *extra ? extra : "");

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
 * per-fixture `cflags` tokens (e.g. "-pthread", "-lz", "-lregex") read from
 * the shape meta so C references that need extra libraries link correctly.
 */
static int bench_compile_c(const char *c_source, const char *exe_path, const char *opt_level,
                           const char *cflags) {
  char src_tmp[PATH_MAX];
  snprintf(src_tmp, sizeof(src_tmp), "%s/ny-benchc-%ld-%ld.c", nyt_temp_dir(), (long)getpid(),
           (long)now_ms());
  FILE *f = fopen(src_tmp, "wb");
  if (!f)
    return 0;
  fwrite(c_source, 1, strlen(c_source), f);
  fclose(f);
  char cc[PATH_MAX];
  if (!bench_find_cc(cc, sizeof(cc))) {
    remove(src_tmp);
    return 0;
  }

  char *cc_argv[24];
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

static void bench_measure_runtime_counters(const char *exe_path, int timeout_sec,
                                           bench_stats_t *stats) {
  if (!exe_path || !stats || !bench_runtime_counters_requested())
    return;
  char capture_path[PATH_MAX];
  if (make_test_capture_tmp(capture_path, sizeof(capture_path),
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
  if (rc != 0 || !out) {
    free(out);
    return;
  }
  size_t allocs = 0, reallocs = 0;
  int seen =
      bench_parse_report_size(out, "ny_runtime_counters ", "alloc=", &allocs) |
      bench_parse_report_size(out, "ny_runtime_counters ", "realloc=", &reallocs);
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
    const char *hit = strstr(line, event);
    if (hit && hit < line + line_len) {
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
  if (make_test_capture_tmp(capture_path, sizeof(capture_path), "bench-perf-stat") < 0)
    return;
  char *argv[] = {"perf", "stat", "--no-big-num", "-x,", "-e",
                  "cycles,instructions,branches,branch-misses,cache-misses",
                  "--", (char *)exe_path, NULL};
  int rc = bench_run_capture(argv, timeout_sec, capture_path);
  char *out = read_small_file(capture_path);
  remove(capture_path);
  if (rc != 0 || !out) {
    free(out);
    return;
  }
  int seen = 0;
  seen |= bench_parse_perf_counter(out, ",cycles,", &stats->hw_cycles);
  seen |= bench_parse_perf_counter(out, ",instructions,", &stats->hw_instructions);
  seen |= bench_parse_perf_counter(out, ",branches,", &stats->hw_branches);
  seen |= bench_parse_perf_counter(out, ",branch-misses,", &stats->hw_branch_misses);
  seen |= bench_parse_perf_counter(out, ",cache-misses,", &stats->hw_cache_misses);
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
                                       bool force_native_report, int timeout_sec,
                                       bench_stats_t *stats) {
  if (!bin || !ny_path || !stats)
    return;
  bool native_report = engine &&
                       (strcmp(engine, "aot") == 0 || strcmp(engine, "native") == 0);
  if (!native_report && !force_native_report)
    return;
  if (target && strcmp(target, "llvm") == 0)
    return;

  char report_path[PATH_MAX];
  char output_path[PATH_MAX];
  char capture_path[PATH_MAX];
  if (make_test_capture_tmp(report_path, sizeof(report_path), "bench-size-report") < 0 ||
      make_test_capture_tmp(output_path, sizeof(output_path), "bench-size-bin") < 0 ||
      make_test_capture_tmp(capture_path, sizeof(capture_path), "bench-size-log") < 0)
    return;
  remove(report_path);
  remove(output_path);

  char report_arg[PATH_MAX + 32];
  snprintf(report_arg, sizeof(report_arg), "--native-tier-report=%s", report_path);
  char *argv[28];
  int k = 0;
  argv[k++] = (char *)bin;
  if (tier && *tier && strcmp(tier, "opt") != 0)
    argv[k++] = (char *)tier;
  if (opt_level && *opt_level)
    argv[k++] = (char *)bench_opt_for(opt_level);
  if (engine && strcmp(engine, "native") == 0) {
    argv[k++] = "--native-only";
    if (target && *target) {
      argv[k++] = "--native-backend";
      argv[k++] = (char *)target;
    }
  } else if (target && *target) {
    argv[k++] = "--native-backend";
    argv[k++] = (char *)target;
  }
  argv[k++] = report_arg;
  argv[k++] = "-o";
  argv[k++] = output_path;
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;

  size_t peak_rss_kb = 0;
  int rc = bench_run_capture_usage(argv, timeout_sec, capture_path, &peak_rss_kb);
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
          bench_parse_report_size(report, "machine_lowering ",
                                  "specialization_code_functions=", &spec_functions) |
          bench_parse_report_size(report, "machine_lowering ",
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
#define BENCH_PARSE_FACT(field, key)                                      \
      do {                                                                 \
        size_t value = 0;                                                  \
        if (bench_parse_report_size(report, "facts ", key, &value)) {    \
          stats->field = value;                                            \
          facts_seen = 1;                                                  \
        }                                                                  \
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
      int spill_seen =
          bench_parse_report_size(report, "mach_regalloc ", "spilled=",
                                  &gpr_spills) |
          bench_parse_report_size(report, "mach_fpr_regalloc ", "spilled=",
                                  &fpr_spills) |
          bench_parse_report_size(report, "mach_vector_regalloc ", "spilled=",
                                  &vec_spills);
      int reload_seen =
          bench_parse_report_size(report, "mach_regalloc ", "reloads=",
                                  &gpr_reloads) |
          bench_parse_report_size(report, "mach_fpr_regalloc ", "reloads=",
                                  &fpr_reloads) |
          bench_parse_report_size(report, "mach_vector_regalloc ", "reloads=",
                                  &vec_reloads);
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
      "max_ny_heap_allocations", "max_ny_runtime_calls",
      "max_ny_bounds_checks", "max_ny_indirect_calls",
      "max_ny_unknown_effects", "max_ny_alias_unresolved", "max_ny_spills",
      "max_ny_reloads",
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
 * Run the Nytrix side of one bench fixture: materialize the ny source, invoke
 * the `bin` compiler on it (which compiles and runs, printing elapsed_ns), and
 * collect the self-reported elapsed.
 */
static bench_stats_t bench_measure_ny(const char *bin, const char *shape_path,
                                      const char *opt_level, const char *tier,
                                      const char *engine, const char *target,
                                      const char *compile_profile, int warmup, int runs,
                                      int timeout_sec) {
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
  if (compile_profile && *compile_profile)
    ny_setenv("NYTRIX_OPT_PROFILE", compile_profile, 1);
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
  argv[k++] = "-time";
  if (!use_jit && !native_jit)
    argv[k++] = "-run";
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;
  bench_stats_t s = bench_measure_self_timed(argv, timeout_sec, warmup, runs);
  bench_measure_ny_code_size(bin, ny_path, opt_level, tier, engine, target,
                             bench_shape_has_static_budget(shape_path),
                             timeout_sec, &s);
  remove(ny_path);
  free(ny_path);
  return s;
}

/*
 * Build the leading Nytrix compiler argv given bench settings (opt/tier/backend).
 * Appends the file path and trailing flags separately by callers.
 */
static int bench_ny_argv(char *argv[], const char *bin, const char *opt_level,
                         const char *tier, const char *engine, const char *target) {
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
  return k;
}

/*
 * Run one diagnostic compile.  The bench (opt/tier/backend) and `extra` flags
 * precede the input file, since they are compiler options rather than program
 * arguments.  Captures stdout+stderr to one temp file.  Returns the joined
 * output as a heap string or NULL.
 */
static char *bench_diag_capture(const char *bin, const char *opt_level, const char *tier,
                                const char *engine, const char *target, char *const extra[],
                                const char *ny_path) {
  char *argv[40];
  int k = bench_ny_argv(argv, bin, opt_level, tier, engine, target);
  for (int i = 0; extra && extra[i]; i++) {
    if (k < 38)
      argv[k++] = extra[i];
  }
  argv[k++] = (char *)ny_path;
  argv[k] = NULL;
  char tmp[PATH_MAX];
  if (make_test_capture_tmp(tmp, sizeof(tmp), "benchdiag") < 0)
    return NULL;
  int rc = bench_run_capture(argv, NY_BENCH_TIMEOUT_SEC, tmp);
  char *out = read_whole_file(tmp);
  remove(tmp);
  (void)rc;
  return out;
}

/*
 * Run and print a labeled diagnostic block for one fixture in bench mode.
 */
static void bench_diag(const char *shape, int show_ir, int show_asm, int show_passes,
                       int profile, int compare_llvm, const char *bin, const char *opt_level,
                       const char *tier, const char *engine, const char *target) {
  char *ny_path = bench_materialize_ny(shape);
  if (!ny_path)
    return;
  if (profile) {
    char *extra[] = {"-prof", NULL};
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra, ny_path);
    if (out) {
      printf("\n[phase profile: %s]\n", shape);
      printf("%s\n", out);
      free(out);
    }
  }
  if (show_passes || show_ir) {
    char *extra[3];
    int n = 0;
    extra[n++] = "--nyir-dump-stats";
    if (show_ir)
      extra[n++] = "--nyir-dump";
    if (show_passes)
      extra[n++] = "--nyir-pass-stats";
    extra[n] = NULL;
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra, ny_path);
    if (out) {
      printf("\n[NYIR/pass diagnostics: %s]\n", shape);
      printf("%s\n", out);
      free(out);
    }
  }
  if (show_asm) {
    char asm_tmp[PATH_MAX];
    char flag[PATH_MAX + 32];
    snprintf(asm_tmp, sizeof(asm_tmp), "%s/ny-bench-asm-%ld-%ld.s", nyt_temp_dir(),
             (long)getpid(), (long)now_ms());
    snprintf(flag, sizeof(flag), "--emit-asm=%s", asm_tmp);
    char *extra[] = {flag, NULL};
    char *out = bench_diag_capture(bin, opt_level, tier, engine, target, extra, ny_path);
    char *asmout = read_whole_file(asm_tmp);
    if (asmout) {
      printf("\n[assembly: %s (%zu bytes)]\n", shape, strlen(asmout));
      printf("%s\n", asmout);
      free(asmout);
      remove(asm_tmp);
    }
    free(out);
  }
  (void)compare_llvm;
  remove(ny_path);
  free(ny_path);
}

/*
 * Run the C side of a c-vs-ny fixture: extract the C block, compile it with
 * the host cc, and collect its self-reported elapsed.
 */
static bench_stats_t bench_measure_c(const char *shape_path, const char *opt_level, int warmup,
                                     int runs, int timeout_sec) {
  char *c_src = bench_c_block(shape_path);
  if (!c_src)
    return (bench_stats_t){.checksum_consistent = 1, .last_rc = 0};
  char *cflags = shape_meta_string(shape_path, "cflags");
  char exe[PATH_MAX];
  snprintf(exe, sizeof(exe), "%s/ny-bench-c-exe-%ld-%ld", nyt_temp_dir(), (long)getpid(),
           (long)now_ms());
  if (!bench_compile_c(c_src, exe, opt_level, cflags)) {
    free(cflags);
    free(c_src);
    remove(exe);
    return (bench_stats_t){.checksum_consistent = 1, .last_rc = 127, .failure_count = 1};
  }
  free(cflags);
  free(c_src);
  char *argv[2];
  argv[0] = exe;
  argv[1] = NULL;
  bench_stats_t s = bench_measure_self_timed(argv, timeout_sec, warmup, runs);
  remove(exe);
  return s;
}
#ifndef _WIN32
static int bench_stats_write_fd(int fd, const bench_stats_t *stats) {
  const unsigned char *p = (const unsigned char *)stats;
  size_t left = sizeof(*stats);
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += (size_t)n;
    left -= (size_t)n;
  }
  return 1;
}

static int bench_stats_read_fd(int fd, bench_stats_t *stats) {
  unsigned char *p = (unsigned char *)stats;
  size_t left = sizeof(*stats);
  while (left > 0) {
    ssize_t n = read(fd, p, left);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += (size_t)n;
    left -= (size_t)n;
  }
  return 1;
}
#endif

typedef enum {
  BENCH_COMPONENT_NY,
  BENCH_COMPONENT_LLVM,
  BENCH_COMPONENT_C,
} bench_component_t;

static bench_stats_t bench_measure_component(const char *bin, const char *shape,
                                             const char *opt_level, const char *tier,
                                             const char *engine, const char *target,
                                             const char *compile_profile, int warmup, int runs,
                                             int timeout_sec, bench_component_t component) {
  if (component == BENCH_COMPONENT_C)
    return bench_measure_c(shape, opt_level, warmup, runs, timeout_sec);
  return bench_measure_ny(bin, shape, opt_level, tier, engine, target, compile_profile, warmup,
                          runs, timeout_sec);
}

typedef struct {
  double wall_ms;
  bench_stats_t ny;
  bench_stats_t llvm;
  bench_stats_t c;
} bench_worker_result_t;

static bench_worker_result_t bench_measure_shape(const char *bin, const char *shape,
                                                 const char *opt_level, const char *tier,
                                                 const char *engine, const char *target,
                                                 const char *compile_profile, int compare_llvm,
                                                 int warmup, int runs, int timeout_sec) {
  bench_worker_result_t result = {0};
  double wall_start = now_ms();
#ifndef _WIN32
  int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
  pid_t pids[3] = {0};
  bench_component_t components[3] = {BENCH_COMPONENT_NY, BENCH_COMPONENT_C,
                                     BENCH_COMPONENT_C};
  int component_count = 1;
  if (compare_llvm)
    components[component_count++] = BENCH_COMPONENT_LLVM;
  components[component_count++] = BENCH_COMPONENT_C;
  bool parallel_ok = true;
  for (int i = 0; i < component_count; i++) {
    if (pipe(pipes[i]) != 0) {
      parallel_ok = false;
      break;
    }
    pids[i] = fork();
    if (pids[i] < 0) {
      parallel_ok = false;
      break;
    }
    if (pids[i] == 0) {
      close(pipes[i][0]);
      const char *component_target =
          components[i] == BENCH_COMPONENT_LLVM ? "llvm" : target;
      const char *component_engine =
          components[i] == BENCH_COMPONENT_LLVM &&
                  (!engine || strcmp(engine, "aot") == 0 ||
                   strcmp(engine, "native") == 0)
              ? "mcjit"
              : engine;
      bench_stats_t stats =
          bench_measure_component(bin, shape, opt_level, tier, component_engine,
                                  component_target, compile_profile, warmup, runs,
                                  timeout_sec, components[i]);
      (void)bench_stats_write_fd(pipes[i][1], &stats);
      close(pipes[i][1]);
      _exit(0);
    }
    close(pipes[i][1]);
  }
  if (parallel_ok) {
    for (int i = 0; i < component_count; i++) {
      bench_stats_t stats = {0};
      bool read_ok = bench_stats_read_fd(pipes[i][0], &stats);
      close(pipes[i][0]);
      waitpid(pids[i], NULL, 0);
      if (!read_ok)
        continue;
      if (components[i] == BENCH_COMPONENT_NY)
        result.ny = stats;
      else if (components[i] == BENCH_COMPONENT_LLVM)
        result.llvm = stats;
      else
        result.c = stats;
    }
  } else {
    for (int i = 0; i < component_count; i++) {
      if (pids[i] > 0)
        waitpid(pids[i], NULL, 0);
      close(pipes[i][0]);
      close(pipes[i][1]);
    }
  }
  if (!parallel_ok) {
#endif
    result.ny = bench_measure_ny(bin, shape, opt_level, tier, engine, target,
                                 compile_profile, warmup, runs, timeout_sec);
    if (compare_llvm) {
      const char *llvm_engine =
          (!engine || strcmp(engine, "aot") == 0 || strcmp(engine, "native") == 0)
              ? "mcjit"
              : engine;
      result.llvm = bench_measure_ny(bin, shape, opt_level, tier, llvm_engine, "llvm",
                                     compile_profile, warmup, runs, timeout_sec);
    }
    result.c = bench_measure_c(shape, opt_level, warmup, runs, timeout_sec);
#ifndef _WIN32
  }
#endif
  result.wall_ms = now_ms() - wall_start;
  return result;
}

#ifndef _WIN32
static int bench_write_result(int fd, const bench_worker_result_t *result) {
  const unsigned char *p = (const unsigned char *)result;
  size_t left = sizeof(*result);
  while (left > 0) {
    ssize_t n = write(fd, p, left);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += (size_t)n;
    left -= (size_t)n;
  }
  return 1;
}

static int bench_read_result(int fd, bench_worker_result_t *result) {
  unsigned char *p = (unsigned char *)result;
  size_t left = sizeof(*result);
  while (left > 0) {
    ssize_t n = read(fd, p, left);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 0;
    p += (size_t)n;
    left -= (size_t)n;
  }
  return 1;
}
#endif

static void bench_print_row(const BenchRow *r, int show_c, int show_llvm);

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

static void bench_record_result(BenchTable *table, int *measured, int verbose,
                                int show_ir, int show_asm, int show_passes, int profile,
                                int compare_llvm, const char *bin, const char *opt_level,
                                const char *tier, const char *engine, const char *target,
                                const char *compile_profile, int warmup, int runs,
                                int timeout_sec, const char *shape, const char *name,
                                const bench_worker_result_t *result) {
  if (show_ir || show_asm || show_passes || profile)
    bench_diag(shape, show_ir, show_asm, show_passes, profile, compare_llvm, bin, opt_level,
               tier, engine, target);
  BenchRow row = {0};
  snprintf(row.name, sizeof(row.name), "%s", name);
  snprintf(row.path, sizeof(row.path), "%s", shape);
  snprintf(row.backend, sizeof(row.backend), "%s",
           engine && strcmp(engine, "aot") != 0 && strcmp(engine, "native") != 0
               ? "llvm"
               : (target && *target ? target : "host"));
  snprintf(row.engine, sizeof(row.engine), "%s", engine && *engine ? engine : "aot");
  snprintf(row.cache, sizeof(row.cache), "%s", bench_cache_state());
  row.wall_ms = result->wall_ms;
  row.ny_ms = result->ny.median;
  row.ny_compile_ms = result->ny.compile_ms;
  row.ny_jit_compile_ms = result->ny.jit_compile_ms;
  row.ny_jit_run_ms = result->ny.jit_run_ms;
  row.ny_opt_ms = result->ny.opt_ms;
  row.ny_total_ms = result->ny.wall_ms;
  row.ny_code_bytes = result->ny.code_bytes;
  row.ny_code_bytes_seen = result->ny.code_bytes_seen;
  row.ny_specialization_code_bytes = result->ny.specialization_code_bytes;
  row.ny_specialization_code_functions = result->ny.specialization_code_functions;
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
  row.ny_runtime_alloc_count = result->ny.runtime_alloc_count;
  row.ny_runtime_realloc_count = result->ny.runtime_realloc_count;
  row.ny_runtime_counters_seen = result->ny.runtime_counters_seen;
  row.ny_runs = (int)result->ny.count;
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
  if (row.native_c_budget > 0.0 && row.ny_runs > 0 && row.c_runs > 0 &&
      row.ny_ms >= 0.01 && row.c_ms >= 0.01 &&
      row.ny_ms / row.c_ms > row.native_c_budget)
    row.budget_failed = 1;
  if (row.native_llvm_budget > 0.0 && row.ny_runs > 0 && row.llvm_runs > 0 &&
      row.ny_ms >= 0.01 && row.llvm_ms >= 0.01 &&
      row.ny_ms / row.llvm_ms > row.native_llvm_budget)
    row.budget_failed = 1;
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
  if ((row.static_budget_mask & (1u << 7)) && row.ny_reloads > row.reloads_budget)
    row.budget_failed = 1;
  row.checksum_ok = 1;
  if (result->ny.checksum_seen)
    snprintf(row.ny_checksum, sizeof(row.ny_checksum), "%s", result->ny.checksum);
  if (result->llvm.checksum_seen)
    snprintf(row.llvm_checksum, sizeof(row.llvm_checksum), "%s", result->llvm.checksum);
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
  if (row.ny_runs > 0 || row.llvm_runs > 0 || row.c_runs > 0 ||
      row.ny_rc == 0 || row.llvm_rc == 0 || row.c_rc == 0) {
    (*measured)++;
  }
  bench_rows_push(table, &row);
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
  return value >= 0.01;
}

static int bench_row_unstable(const BenchRow *r) {
  return r && (r->ny_unstable || r->llvm_unstable || r->c_unstable);
}

static size_t bench_disp_width(const char *s) {
  size_t w = 0;
  for (; s && *s; s++)
    if (((unsigned char)*s & 0xC0) != 0x80)
      w++;
  return w;
}

static void bench_cell(const char *s, int width) {
  int pad = width - (int)bench_disp_width(s);
  printf("%*s%s ", pad > 0 ? pad : 0, "", s);
}

static void bench_ms_text(double value, int valid, char *out, size_t cap) {
  if (!valid || value <= 0.0)
    snprintf(out, cap, "n/a");
  else if (value < 0.01)
    snprintf(out, cap, "<10µs");
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

static void bench_print_ratio(double ratio, int valid) {
  if (!valid) {
    printf("%s%8s%s", nyt_clr(NYT_GRAY), "n/a", nyt_clr(NYT_RESET));
    return;
  }
  char value[32];
  snprintf(value, sizeof(value), "%.2fx", ratio);
  int pad = 8 - (int)strlen(value);
  printf("%*s%s%s%s", pad > 0 ? pad : 0, "", nyt_clr(bench_ratio_color(ratio)), value,
         nyt_clr(NYT_RESET));
}

static void bench_print_row(const BenchRow *r, int show_c, int show_llvm) {
  char compile[32], opt[32], native[32], llvm[32], c_host[32];
  bench_ms_text(r->ny_compile_ms, bench_signal(r->ny_compile_ms), compile, sizeof(compile));
  bench_ms_text(r->ny_opt_ms, bench_signal(r->ny_opt_ms), opt, sizeof(opt));
  bench_ms_text(r->ny_ms, r->ny_runs > 0, native, sizeof(native));
  bench_ms_text(r->llvm_ms, r->llvm_runs > 0, llvm, sizeof(llvm));
  bench_ms_text(r->c_ms, r->c_runs > 0, c_host, sizeof(c_host));
  int native_ratio_valid = show_c && r->c_runs > 0 && bench_signal(r->ny_ms) &&
                           bench_signal(r->c_ms);
  int llvm_ratio_valid = show_llvm && r->c_runs > 0 && r->llvm_runs > 0 &&
                         bench_signal(r->llvm_ms) && bench_signal(r->c_ms);
  double native_ratio = native_ratio_valid ? r->ny_ms / r->c_ms : 0.0;
  double llvm_ratio = llvm_ratio_valid ? r->llvm_ms / r->c_ms : 0.0;
  bool is_fail = r->ny_failures || r->llvm_failures || r->c_failures ||
                 !r->checksum_ok;
  const char *status_str =
      is_fail ? (r->ny_failures || r->llvm_failures || r->c_failures
                     ? "FAIL"
                     : "MISMATCH")
              : (r->budget_failed ? "REGRESS" : (bench_row_unstable(r) ? "NOISY" : "ok"));
  const char *status_clr =
      is_fail ? NYT_RED : (r->budget_failed ? NYT_YELLOW : NYT_GREEN);

  printf("%-22s %s%-6s%s ", r->name, nyt_clr(status_clr), status_str,
         nyt_clr(NYT_RESET));
  bench_cell(compile, 10);
  bench_cell(opt, 9);
  bench_cell(native, 11);
  bench_cell(llvm, 11);
  bench_cell(c_host, 10);
  bench_print_ratio(native_ratio, native_ratio_valid);
  printf(" ");
  bench_print_ratio(llvm_ratio, llvm_ratio_valid);
  if (!r->checksum_ok)
    printf(" %s(MISMATCH: %s|%s|%s)%s", nyt_clr(NYT_RED),
           r->ny_checksum, r->llvm_checksum, r->c_checksum, nyt_clr(NYT_RESET));
  printf("\n");
}
static void bench_print_hotspots(const BenchTable *t) {
  if (!t || t->len == 0)
    return;
  const BenchRow *compile = NULL;
  const BenchRow *opt = NULL;
  const BenchRow *runtime = NULL;
  const BenchRow *ratio = NULL;
  const BenchRow *llvm_gap = NULL;
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    if (bench_signal(r->ny_compile_ms) &&
        (!compile || r->ny_compile_ms > compile->ny_compile_ms))
      compile = r;
    if (bench_signal(r->ny_opt_ms) && (!opt || r->ny_opt_ms > opt->ny_opt_ms))
      opt = r;
    if (bench_signal(r->ny_ms) && (!runtime || r->ny_ms > runtime->ny_ms))
      runtime = r;
    if (r->c_runs > 0 && bench_signal(r->ny_ms) && bench_signal(r->c_ms) &&
        (!ratio || r->ny_ms / r->c_ms > ratio->ny_ms / ratio->c_ms))
      ratio = r;
    if (r->llvm_runs > 0 && bench_signal(r->ny_ms) && bench_signal(r->llvm_ms) &&
        (!llvm_gap || r->ny_ms / r->llvm_ms > llvm_gap->ny_ms / llvm_gap->llvm_ms))
      llvm_gap = r;
  }
  printf("\n%sSlowest observed paths:%s compile=%s, opt=%s, runtime=%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), compile ? compile->name : "n/a",
         opt ? opt->name : "n/a", runtime ? runtime->name : "n/a");
  if (ratio) {
    double gap = ratio->ny_ms / ratio->c_ms;
    printf("%slargest native/C gap:%s %s (%s%.2fx%s)\n", nyt_clr(NYT_BOLD),
           nyt_clr(NYT_RESET), ratio->name, nyt_clr(bench_ratio_color(gap)), gap,
           nyt_clr(NYT_RESET));
  } else
    printf("%slargest native/C gap:%s n/a (low-signal or missing C baseline)\n",
           nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  if (llvm_gap) {
    double gap = llvm_gap->ny_ms / llvm_gap->llvm_ms;
    printf("%slargest native/LLVM gap:%s %s (%s%.2fx%s)\n", nyt_clr(NYT_BOLD),
           nyt_clr(NYT_RESET), llvm_gap->name, nyt_clr(bench_ratio_color(gap)), gap,
           nyt_clr(NYT_RESET));
  } else
    printf("%slargest native/LLVM gap:%s n/a (low-signal or missing LLVM baseline)\n",
           nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
}


static void bench_write_csv(FILE *f, const BenchTable *t) {
  fprintf(f, "benchmark,backend,engine,cache,status,checksum,ny_checksum,llvm_checksum,c_checksum,checksum_ok,ny_native_ms,ny_compile_ms,ny_opt_ms,ny_total_ms,ny_code_bytes,ny_code_bytes_seen,ny_specialization_code_bytes,ny_specialization_code_functions,ny_specialization_max_function_bytes,ny_specialization_metrics_seen,ny_peak_compiler_rss_kb,ny_peak_compiler_rss_seen,ny_llvm_ms,llvm_total_ms,c_ms,c_total_ms,driver_wall_ms,native_ratio,llvm_ratio,ny_rc,llvm_rc,c_rc,ny_failures,llvm_failures,c_failures,ny_runs,llvm_runs,c_runs,ny_min,ny_max,ny_p95,ny_dispersion_pct,ny_noise_pct,ny_unstable,llvm_min,llvm_max,llvm_p95,llvm_dispersion_pct,llvm_noise_pct,llvm_unstable,c_min,c_max,c_p95,c_dispersion_pct,c_noise_pct,c_unstable,native_c_budget,native_llvm_budget,compile_ms_budget,code_bytes_budget,specialization_code_bytes_budget,specialization_function_bytes_budget,compiler_rss_budget_kb,ny_static_metrics_seen,ny_runtime_calls,ny_dynamic_ops,ny_tag_checks,ny_box_unbox,ny_heap_allocations,ny_bounds_checks,ny_direct_calls,ny_indirect_calls,ny_unknown_effects,ny_alias_unresolved,ny_vector_attempted,ny_vector_rejected,ny_vectorized,ny_spills,ny_reloads,ny_hw_counters_seen,ny_hw_cycles,ny_hw_instructions,ny_hw_branches,ny_hw_branch_misses,ny_hw_cache_misses,ny_runtime_counters_seen,ny_runtime_alloc_count,ny_runtime_realloc_count,static_budget_mask,heap_allocations_budget,runtime_calls_budget,bounds_checks_budget,indirect_calls_budget,unknown_effects_budget,alias_unresolved_budget,spills_budget,reloads_budget,budget_failed\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    double native_ratio = (r->c_ms > 0.0) ? r->ny_ms / r->c_ms : 0.0;
    double llvm_ratio = (r->c_ms > 0.0) ? r->llvm_ms / r->c_ms : 0.0;
    const char *status = (r->ny_failures || r->llvm_failures || r->c_failures) ? "fail"
                         : (r->budget_failed ? "regression" : (bench_row_unstable(r) ? "unstable" : "ok"));
    fprintf(f,
            "%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,"
            "%.4f,%.4f,%.4f,%.4f,%zu,%d,%zu,%zu,%zu,%d,%zu,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,"
            "%d,%d,%d,%d,%d,%d,%d,%d,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,%zu,%zu,%zu,%zu,"
            "%d,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,"
            "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ","
            "%d,%zu,%zu,"
            "%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%d\n",
            r->name, r->backend, r->engine, r->cache, status, r->checksum,
            r->ny_checksum, r->llvm_checksum, r->c_checksum, r->checksum_ok,
            r->ny_ms, r->ny_compile_ms, r->ny_opt_ms, r->ny_total_ms,
            r->ny_code_bytes, r->ny_code_bytes_seen,
            r->ny_specialization_code_bytes, r->ny_specialization_code_functions,
            r->ny_specialization_max_function_bytes, r->ny_specialization_metrics_seen,
            r->ny_peak_compiler_rss_kb, r->ny_peak_compiler_rss_seen, r->llvm_ms,
            r->llvm_total_ms, r->c_ms, r->c_total_ms, r->wall_ms, native_ratio, llvm_ratio,
            r->ny_rc, r->llvm_rc, r->c_rc, r->ny_failures, r->llvm_failures, r->c_failures,
            r->ny_runs, r->llvm_runs, r->c_runs,
            r->ny_min, r->ny_max, r->ny_p95, r->ny_dispersion_pct, r->ny_noise_pct,
            r->ny_unstable, r->llvm_min, r->llvm_max, r->llvm_p95,
            r->llvm_dispersion_pct, r->llvm_noise_pct, r->llvm_unstable, r->c_min, r->c_max,
            r->c_p95, r->c_dispersion_pct, r->c_noise_pct, r->c_unstable,
            r->native_c_budget, r->native_llvm_budget, r->compile_ms_budget,
            r->code_bytes_budget, r->specialization_code_bytes_budget,
            r->specialization_function_bytes_budget, r->compiler_rss_budget_kb,
            r->ny_static_metrics_seen, r->ny_runtime_calls, r->ny_dynamic_ops,
            r->ny_tag_checks, r->ny_box_unbox, r->ny_heap_allocations,
            r->ny_bounds_checks, r->ny_direct_calls, r->ny_indirect_calls,
            r->ny_unknown_effects, r->ny_alias_unresolved, r->ny_vector_attempted,
            r->ny_vector_rejected, r->ny_vectorized, r->ny_spills, r->ny_reloads,
            r->ny_hw_counters_seen, r->ny_hw_cycles, r->ny_hw_instructions,
            r->ny_hw_branches, r->ny_hw_branch_misses, r->ny_hw_cache_misses,
            r->ny_runtime_counters_seen, r->ny_runtime_alloc_count,
            r->ny_runtime_realloc_count, r->static_budget_mask,
            r->heap_allocations_budget,
            r->runtime_calls_budget, r->bounds_checks_budget,
            r->indirect_calls_budget, r->unknown_effects_budget,
            r->alias_unresolved_budget, r->spills_budget, r->reloads_budget,
            r->budget_failed);
  }
}

static void bench_write_json(FILE *f, const BenchTable *t, const BenchEnvironment *env) {
  fprintf(f, "{\n");
  if (env) {
    fprintf(f,
            "  \"environment\": {\"os\":\"%s\",\"arch\":\"%s\","
            "\"cpu_model\":\"%s\",\"target\":\"%s\",\"target_features\":\"%s\","
            "\"c_compiler\":\"%s\",\"c_compiler_version\":\"%s\","
            "\"c_flags\":\"%s\",\"compiler_revision\":\"%s\"},\n",
            env->os, env->arch, env->cpu_model, env->target, env->target_features,
            env->c_compiler, env->c_compiler_version, env->c_flags, env->compiler_revision);
  }
  fprintf(f, "  \"noise_threshold_pct\": %.3f,\n", bench_noise_limit_pct());
  fprintf(f, "  \"benchmarks\": [\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    double native_ratio = (r->c_ms > 0.0) ? r->ny_ms / r->c_ms : 0.0;
    double llvm_ratio = (r->c_ms > 0.0) ? r->llvm_ms / r->c_ms : 0.0;
    const char *status = (r->ny_failures || r->llvm_failures || r->c_failures) ? "fail"
                         : (r->budget_failed ? "regression" : (bench_row_unstable(r) ? "unstable" : "ok"));
    fprintf(f,
            "    {\"name\":\"%s\",\"backend\":\"%s\",\"engine\":\"%s\","
            "\"cache\":\"%s\",\"status\":\"%s\",\"checksum\":\"%s\","
            "\"ny_checksum\":\"%s\",\"llvm_checksum\":\"%s\",\"c_checksum\":\"%s\","
            "\"checksum_ok\":%s,",
            r->name, r->backend, r->engine, r->cache, status, r->checksum, r->ny_checksum,
            r->llvm_checksum, r->c_checksum, r->checksum_ok ? "true" : "false");
    fprintf(f,
            "\"ny_native_ms\":%.4f,\"ny_compile_ms\":%.4f,\"ny_opt_ms\":%.4f,"
            "\"ny_total_ms\":%.4f,\"ny_code_bytes\":%zu,\"ny_code_bytes_seen\":%s,"
            "\"ny_specialization_code_bytes\":%zu,\"ny_specialization_code_functions\":%zu,"
            "\"ny_specialization_max_function_bytes\":%zu,\"ny_specialization_metrics_seen\":%s,"
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
            "\"ny_hw_branch_misses\":%" PRIu64 ",\"ny_hw_cache_misses\":%" PRIu64 ","
            "\"ny_runtime_counters_seen\":%s,\"ny_runtime_alloc_count\":%zu,"
            "\"ny_runtime_realloc_count\":%zu,"
            "\"ny_llvm_ms\":%.4f,\"llvm_total_ms\":%.4f,"
            "\"c_ms\":%.4f,\"c_total_ms\":%.4f,\"driver_wall_ms\":%.4f,"
            "\"native_ratio\":%.4f,\"llvm_ratio\":%.4f,",
            r->ny_ms, r->ny_compile_ms, r->ny_opt_ms, r->ny_total_ms,
            r->ny_code_bytes, r->ny_code_bytes_seen ? "true" : "false",
            r->ny_specialization_code_bytes, r->ny_specialization_code_functions,
            r->ny_specialization_max_function_bytes,
            r->ny_specialization_metrics_seen ? "true" : "false",
            r->ny_peak_compiler_rss_kb, r->ny_peak_compiler_rss_seen ? "true" : "false",
            r->ny_static_metrics_seen ? "true" : "false", r->ny_runtime_calls,
            r->ny_dynamic_ops, r->ny_tag_checks, r->ny_box_unbox,
            r->ny_heap_allocations, r->ny_bounds_checks, r->ny_direct_calls,
            r->ny_indirect_calls, r->ny_unknown_effects, r->ny_alias_unresolved,
            r->ny_vector_attempted, r->ny_vector_rejected, r->ny_vectorized,
            r->ny_spills, r->ny_reloads, r->ny_hw_counters_seen ? "true" : "false",
            r->ny_hw_cycles, r->ny_hw_instructions, r->ny_hw_branches,
            r->ny_hw_branch_misses, r->ny_hw_cache_misses,
            r->ny_runtime_counters_seen ? "true" : "false",
            r->ny_runtime_alloc_count, r->ny_runtime_realloc_count, r->llvm_ms,
            r->llvm_total_ms, r->c_ms,
            r->c_total_ms, r->wall_ms, native_ratio, llvm_ratio);
    fprintf(f,
            "\"ny_rc\":%d,\"llvm_rc\":%d,\"c_rc\":%d,\"ny_failures\":%d,"
            "\"llvm_failures\":%d,\"c_failures\":%d,\"ny_runs\":%d,"
            "\"llvm_runs\":%d,\"c_runs\":%d,",
            r->ny_rc, r->llvm_rc, r->c_rc, r->ny_failures, r->llvm_failures, r->c_failures,
            r->ny_runs, r->llvm_runs, r->c_runs);
    fprintf(f,
            "\"ny_min\":%.4f,\"ny_max\":%.4f,\"ny_p95\":%.4f,"
            "\"ny_dispersion_pct\":%.4f,\"ny_noise_pct\":%.4f,\"ny_unstable\":%s,"
            "\"llvm_min\":%.4f,\"llvm_max\":%.4f,\"llvm_p95\":%.4f,"
            "\"llvm_dispersion_pct\":%.4f,\"llvm_noise_pct\":%.4f,\"llvm_unstable\":%s,"
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
            "\"budget_failed\":%s}%s\n",
            r->ny_min, r->ny_max, r->ny_p95, r->ny_dispersion_pct, r->ny_noise_pct,
            r->ny_unstable ? "true" : "false", r->llvm_min, r->llvm_max, r->llvm_p95,
            r->llvm_dispersion_pct, r->llvm_noise_pct, r->llvm_unstable ? "true" : "false",
            r->c_min, r->c_max, r->c_p95, r->c_dispersion_pct, r->c_noise_pct,
            r->c_unstable ? "true" : "false", r->native_c_budget, r->native_llvm_budget,
            r->compile_ms_budget, r->code_bytes_budget,
            r->specialization_code_bytes_budget,
            r->specialization_function_bytes_budget, r->compiler_rss_budget_kb,
            r->static_budget_mask, r->heap_allocations_budget,
            r->runtime_calls_budget, r->bounds_checks_budget,
            r->indirect_calls_budget, r->unknown_effects_budget,
            r->alias_unresolved_budget, r->spills_budget, r->reloads_budget,
            r->budget_failed ? "true" : "false", (i + 1 < t->len) ? "," : "");
  }
  fprintf(f, "  ]\n}\n");
}

static void bench_write_md(FILE *f, const BenchTable *t, const BenchEnvironment *env) {
  if (env) {
    fprintf(f, "- OS/arch: `%s` / `%s`\n", env->os, env->arch);
    fprintf(f, "- CPU: `%s`\n", env->cpu_model);
    fprintf(f, "- target: `%s`\n", env->target);
    fprintf(f, "- target features: `%s`\n", env->target_features);
    fprintf(f, "- C compiler: `%s` — `%s`\n", env->c_compiler, env->c_compiler_version);
    fprintf(f, "- C flags: `%s`\n", env->c_flags);
    fprintf(f, "- compiler revision: `%s`\n", env->compiler_revision);
    fprintf(f, "- unstable threshold: p95-median > %.1f%% of median (5+ samples)\n\n",
            bench_noise_limit_pct());
  }
  fprintf(f, "| benchmark | backend | engine | status | Ny native ms | Ny p95 | Ny noise | Ny code bytes | mono bytes | mono max fn bytes | compiler peak RSS KiB | Ny LLVM ms | LLVM p95 | LLVM noise | C ms | C p95 | C noise | native/C | native/LLVM |\n");
  fprintf(f, "| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    double native_ratio = (r->c_ms > 0.0) ? r->ny_ms / r->c_ms : 0.0;
    double native_llvm = (r->llvm_ms > 0.0) ? r->ny_ms / r->llvm_ms : 0.0;
    const char *status = (r->ny_failures || r->llvm_failures || r->c_failures) ? "fail"
                         : (r->budget_failed ? "regression" : (bench_row_unstable(r) ? "unstable" : "ok"));
    fprintf(f, "| %s | %s | %s | %s | %.3f | %.3f | %.2f%% | %zu | %zu | %zu | %zu | %.3f | %.3f | %.2f%% | %.3f | %.3f | %.2f%% | %.2fx | %.2fx |\n",
            r->name, r->backend, r->engine, status, r->ny_ms, r->ny_p95, r->ny_noise_pct,
            r->ny_code_bytes_seen ? r->ny_code_bytes : 0,
            r->ny_specialization_metrics_seen ? r->ny_specialization_code_bytes : 0,
            r->ny_specialization_metrics_seen ? r->ny_specialization_max_function_bytes : 0,
            r->ny_peak_compiler_rss_seen ? r->ny_peak_compiler_rss_kb : 0,
            r->llvm_ms, r->llvm_p95,
            r->llvm_noise_pct, r->c_ms, r->c_p95, r->c_noise_pct,
            native_ratio, native_llvm);
  }
  fprintf(f, "\n| benchmark | alloc sites | runtime allocs | runtime reallocs | runtime helpers | bounds checks | dynamic ops | indirect calls | unknown effects | alias unresolved | spills | reloads | vectorized/attempted | cycles | instructions | branch misses | cache misses |\n");
  fprintf(f, "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (size_t i = 0; i < t->len; i++) {
    const BenchRow *r = &t->items[i];
    char runtime_allocs[32], runtime_reallocs[32];
    char hw_cycles[32], hw_instructions[32], hw_branch_misses[32], hw_cache_misses[32];
    if (r->ny_runtime_counters_seen) {
      snprintf(runtime_allocs, sizeof(runtime_allocs), "%zu", r->ny_runtime_alloc_count);
      snprintf(runtime_reallocs, sizeof(runtime_reallocs), "%zu", r->ny_runtime_realloc_count);
    } else {
      snprintf(runtime_allocs, sizeof(runtime_allocs), "n/a");
      snprintf(runtime_reallocs, sizeof(runtime_reallocs), "n/a");
    }
    if (r->ny_hw_counters_seen) {
      snprintf(hw_cycles, sizeof(hw_cycles), "%" PRIu64, r->ny_hw_cycles);
      snprintf(hw_instructions, sizeof(hw_instructions), "%" PRIu64, r->ny_hw_instructions);
      snprintf(hw_branch_misses, sizeof(hw_branch_misses), "%" PRIu64, r->ny_hw_branch_misses);
      snprintf(hw_cache_misses, sizeof(hw_cache_misses), "%" PRIu64, r->ny_hw_cache_misses);
    } else {
      snprintf(hw_cycles, sizeof(hw_cycles), "n/a");
      snprintf(hw_instructions, sizeof(hw_instructions), "n/a");
      snprintf(hw_branch_misses, sizeof(hw_branch_misses), "n/a");
      snprintf(hw_cache_misses, sizeof(hw_cache_misses), "n/a");
    }
    if (!r->ny_static_metrics_seen)
      fprintf(f, "| %s | n/a | %s | %s | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | n/a | %s | %s | %s | %s |\n",
              r->name, runtime_allocs, runtime_reallocs,
              hw_cycles, hw_instructions, hw_branch_misses, hw_cache_misses);
    else
      fprintf(f, "| %s | %zu | %s | %s | %zu | %zu | %zu | %zu | %zu | %zu | %zu | %zu | %zu/%zu | %s | %s | %s | %s |\n",
              r->name, r->ny_heap_allocations, runtime_allocs, runtime_reallocs,
              r->ny_runtime_calls, r->ny_bounds_checks, r->ny_dynamic_ops, r->ny_indirect_calls,
              r->ny_unknown_effects, r->ny_alias_unresolved, r->ny_spills,
              r->ny_reloads, r->ny_vectorized, r->ny_vector_attempted,
              hw_cycles, hw_instructions, hw_branch_misses, hw_cache_misses);
  }
}

/*
 * Entrypoint
 */

/*
 * `--bench` driver.  Collects bench shapes (from the positional files when the
 * caller supplied some, otherwise from the default bench dir), filters by the
 * `--pattern` values, measures each, and reports.
 */
static int run_benchmarks(const char *bin, const char *pattern, const char *opt_level,
                          const char *tier, const char *engine, const char *target, int runs,
                          int warmup, int timeout_sec, int verbose, int show_ir, int show_asm,
                          int show_passes, int profile, int compare_llvm, int correctness_only,
                          const char *out_csv, const char *out_json, const char *out_md,
                          const char *compile_profile, int budget_fail,
                          StrVec *files, StrVec *patterns) {
  (void)budget_fail;
  if (correctness_only) {
    runs = 1;
    warmup = 0;
  }
  (void)pattern;

  const char *root = bench_dir_root();
  if (!root) {
    nyt_err("ny-test", "bench: no bench fixtures found (set NYTRIX_ROOT or run from the repo)");
    return 2;
  }

  BenchEnvironment bench_env;
  bench_collect_environment(root, target, opt_level, &bench_env);

  StrVec shapes = {0};
  {
    char bench_dir[PATH_MAX];
    snprintf(bench_dir, sizeof(bench_dir), "%s/etc/tests/bench", root);
    StrVec all = {0};
    collect_ny(bench_dir, &all);
    qsort(all.items, all.len, sizeof(char *), path_lex_cmp);
    if (files && files->len > 0) {
      for (size_t i = 0; i < files->len; i++) {
        const char *want = files->items[i];
        if (nyt_ends_with(want, ".nshape")) {
          sv_push(&shapes, want);
          continue;
        }
        size_t matches = 0;
        for (size_t j = 0; j < all.len; j++) {
          const char *base = strrchr(all.items[j], '/');
          base = base ? base + 1 : all.items[j];
          char stem[128];
          snprintf(stem, sizeof(stem), "%s", base);
          char *dot = strrchr(stem, '.');
          if (dot)
            *dot = '\0';
          if (strstr(all.items[j], want) || strcmp(stem, want) == 0) {
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
      int ci_mode = test_env_truthy("CI") || test_env_truthy("GITHUB_ACTIONS");
      for (size_t j = 0; j < all.len; j++) {
        if (ci_mode && shape_skips_ci(all.items[j]))
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
  {
    StrVec pats = {0};
    if (files)
      for (size_t i = 0; i < files->len; i++)
        if (!nyt_ends_with(files->items[i], ".nshape"))
          sv_push(&pats, files->items[i]);
    printf("%sbench:%s %zu fixture%s%s", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET),
           shapes.len, shapes.len == 1 ? "" : "s",
           pats.len ? "" : "\n");
    if (pats.len) {
      printf(" matching ");
      for (size_t i = 0; i < pats.len; i++)
        printf("%s'%s'%s", i ? ", " : "", pats.items[i],
               i + 1 < pats.len ? "" : "\n");
    }
    sv_free(&pats);
  }

  /*
   * Sort for deterministic output.
   */
  qsort(shapes.items, shapes.len, sizeof(char *), path_lex_cmp);

  BenchTable table = {0};
  int measured = 0;

  nyt_heading("Nytrix Benchmark Suite");
  printf("%scompiler:%s %s%s%s | %sopt:%s %s%s%s | %sprofile:%s %s%s%s | %stier:%s %s%s%s | %sengine:%s %s%s%s | %sbackend:%s %s%s%s | %scache:%s %s%s%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), disp_path(bin),
         nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         opt_level ? opt_level : "default", nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD),
         nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         compile_profile ? compile_profile : "default", nyt_clr(NYT_RESET),
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         tier ? tier : "default", nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD),
         nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), engine ? engine : "aot",
         nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN),
         target ? target : "default", nyt_clr(NYT_RESET), nyt_clr(NYT_BOLD),
         nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), bench_cache_state(), nyt_clr(NYT_RESET));
  printf("%sruns:%s %s%d%s + %s%d%s warm-up\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET),
         nyt_clr(NYT_CYAN), runs, nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), warmup,
         nyt_clr(NYT_RESET));
  printf("%s%-22s %-6s %10s %9s %11s %11s %11s %8s %8s%s\n",
         nyt_clr(NYT_BOLD), "benchmark", "status", "compiler", "opt",
         "Ny native", "Ny LLVM", "C (host)", "native/C", "LLVM/C",
         nyt_clr(NYT_RESET));

  typedef struct {
    const char *shape;
    char name[128];
    bench_worker_result_t result;
    int fd;
    int ok;
#ifndef _WIN32
    pid_t pid;
#endif
  } BenchJob;
  BenchJob *bench_jobs = (BenchJob *)calloc(shapes.len ? shapes.len : 1, sizeof(*bench_jobs));
  size_t bench_count = 0;
  for (size_t i = 0; i < shapes.len; i++) {
    const char *shape = shapes.items[i];
    const char *base = strrchr(shape, '/');
    base = base ? base + 1 : shape;
    if (patterns && patterns->len > 0) {
      int matched = 0;
      for (size_t p = 0; p < patterns->len; p++)
        if (strstr(base, patterns->items[p]))
          matched = 1;
      if (!matched)
        continue;
    }
    BenchJob *job = &bench_jobs[bench_count++];
    job->fd = -1;
#ifndef _WIN32
    job->pid = -1;
#endif
    job->shape = shape;
    snprintf(job->name, sizeof(job->name), "%s", base);
    size_t dot = strlen(job->name);
    while (dot > 0 && job->name[dot - 1] != '.')
      dot--;
    if (dot > 0)
      job->name[dot - 1] = '\0';
  }
  int bench_jobs_limit = auto_test_jobs() * 2;
  if (bench_jobs_limit < 1)
    bench_jobs_limit = 1;
  if (bench_jobs_limit > 32)
    bench_jobs_limit = 32;
  if (bench_jobs_limit > (int)bench_count)
    bench_jobs_limit = (int)bench_count;
  printf("%s%d%s parallel fixture worker%s\n", nyt_clr(NYT_CYAN), bench_jobs_limit,
         nyt_clr(NYT_RESET), bench_jobs_limit == 1 ? "" : "s");
  for (size_t first = 0; first < bench_count; first += (size_t)bench_jobs_limit) {
    size_t count = bench_count - first;
    if (count > (size_t)bench_jobs_limit)
      count = (size_t)bench_jobs_limit;
#ifndef _WIN32
    for (size_t j = 0; j < count; j++) {
      BenchJob *job = &bench_jobs[first + j];
      int pipefd[2] = {-1, -1};
      if (pipe(pipefd) < 0) {
        job->ok = 0;
        continue;
      }
      fflush(NULL);
      pid_t pid = fork();
      if (pid == 0) {
        close(pipefd[0]);
        bench_worker_result_t result =
            bench_measure_shape(bin, job->shape, opt_level, tier, engine, target,
                                compile_profile, compare_llvm, warmup, runs, timeout_sec);
        int ok = bench_write_result(pipefd[1], &result);
        close(pipefd[1]);
        _exit(ok ? 0 : 1);
      }
      close(pipefd[1]);
      if (pid < 0) {
        close(pipefd[0]);
        job->ok = 0;
      } else {
        job->fd = pipefd[0];
        job->pid = pid;
      }
    }
    for (size_t j = 0; j < count; j++) {
      BenchJob *job = &bench_jobs[first + j];
      job->ok = job->fd >= 0 && bench_read_result(job->fd, &job->result);
      if (job->fd >= 0)
        close(job->fd);
      if (job->pid > 0)
        waitpid(job->pid, NULL, 0);
      if (job->ok) {
        bench_record_result(&table, &measured, verbose, show_ir, show_asm, show_passes,
                            profile, compare_llvm, bin, opt_level, tier, engine, target,
                            compile_profile, warmup, runs, timeout_sec, job->shape, job->name,
                            &job->result);
      } else {
        bench_worker_result_t failed = {0};
        failed.ny.last_rc = failed.llvm.last_rc = failed.c.last_rc = 127;
        failed.ny.failure_count = failed.llvm.failure_count = failed.c.failure_count = 1;
        bench_record_result(&table, &measured, verbose, show_ir, show_asm, show_passes,
                            profile, compare_llvm, bin, opt_level, tier, engine, target,
                            compile_profile, warmup, runs, timeout_sec, job->shape, job->name,
                            &failed);
      }
    }
#else
    for (size_t j = 0; j < count; j++) {
      BenchJob *job = &bench_jobs[first + j];
      job->result = bench_measure_shape(bin, job->shape, opt_level, tier, engine, target,
                                        compile_profile, compare_llvm, warmup, runs,
                                        timeout_sec);
      bench_record_result(&table, &measured, verbose, show_ir, show_asm, show_passes,
                          profile, compare_llvm, bin, opt_level, tier, engine, target,
                          compile_profile, warmup, runs, timeout_sec, job->shape, job->name,
                          &job->result);
    }
#endif
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
  bench_print_hotspots(&table);
  if (out_csv) {
    FILE *f = fopen(out_csv, "w");
    if (f) {
      bench_write_csv(f, &table);
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
      bench_write_md(f, &table, &bench_env);
      fclose(f);
    }
  }
  bench_rows_free(&table);
  sv_free(&shapes);
  if (unstable)
    nyt_err("ny-test", "bench: unstable repeated measurement exceeded the configured noise threshold");
  else if (failed)
    nyt_err("ny-test", "bench: one or more components failed or checksums disagreed");
  return failed ? 1 : 0;
}
