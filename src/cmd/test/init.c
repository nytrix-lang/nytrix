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

static double now_ms(void);
static const char *disp_path(const char *p);
static void print_section(const char *name);
static uint64_t fnv1a_update(uint64_t h, const void *ptr, size_t n);
static uint64_t test_sig(const char *path, const char *bin, const char *std_path,
                         const char *std_bc);
static int path_lex_cmp(const void *a, const void *b);
static void apply_test_child_env(void);
static const char *test_warn_arg(void);
static void push_test_warn_arg(char **argv, int *argc, int max);
static char *shape_source_block(const char *shape_path, const char *name);
static char *materialize_shape_ny_source(const char *shape_path);
static char *shape_meta_string(const char *shape_path, const char *key);
static int native_backend_explicit(const char *flags);
static int path_is_native_runtime_test(const char *p);
static int run_progress_selftest(const char *bin, int timeout_sec);

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
} CacheDb;

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
  for (size_t i = 0; i < db->len; i++) {
    if (strcmp(db->items[i].path, path) == 0) {
      db->items[i].sig = sig;
      db->items[i].ok = ok;
      db->items[i].dur_ms = dur_ms;
      return;
    }
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
}

static CacheRow *cache_find(CacheDb *db, const char *path) {
  for (size_t i = 0; i < db->len; i++)
    if (strcmp(db->items[i].path, path) == 0)
      return &db->items[i];
  return NULL;
}

static void cache_free(CacheDb *db) {
  for (size_t i = 0; i < db->len; i++)
    free(db->items[i].path);
  free(db->items);
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void collect_ny(const char *path, StrVec *out) {
  if (nyt_is_file(path)) {
    if (nyt_ends_with(path, ".ny") || nyt_ends_with(path, ".nshape"))
      sv_push(out, path);
    return;
  }
  if (!is_dir(path))
    return;
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
    snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
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
         strstr(base, "32_") != NULL || strstr(base, "_32") != NULL;
}

static int test_compile_archive_source(const char *cc, const char *src_path,
                                       const char *obj_path) {
  int use_m32 = test_archive_source_needs_m32(src_path);
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
  if (cc_rc != 0 && use_m32) {
    cc_argc = 0;
    cc_argv[cc_argc++] = (char *)cc;
    cc_argv[cc_argc++] = (char *)"-c";
    cc_argv[cc_argc++] = (char *)"-fno-pic";
    cc_argv[cc_argc++] = (char *)"-fno-builtin";
    cc_argv[cc_argc++] = (char *)"-fno-inline";
    cc_argv[cc_argc++] = (char *)src_path;
    cc_argv[cc_argc++] = (char *)"-o";
    cc_argv[cc_argc++] = (char *)obj_path;
    cc_argv[cc_argc] = NULL;
    cc_rc = run_debug_argv(cc_argv, 30, 1);
  }
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

  char prefix[PATH_MAX];
  snprintf(prefix, sizeof(prefix), "%s_", stem);
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

static int test_build_missing_archive(const char *archive_path) {
  if (!archive_path || !*archive_path)
    return 0;
  size_t alen = strlen(archive_path);
  if (alen <= 2 || archive_path[alen - 2] != '.' || archive_path[alen - 1] != 'a')
    return nyt_is_file(archive_path);

  StrVec srcs = {0};
  char single_src[PATH_MAX];
  snprintf(single_src, sizeof(single_src), "%s", archive_path);
  snprintf(single_src + alen - 2, sizeof(single_src) - alen + 2, ".c");
  if (nyt_is_file(single_src))
    sv_push(&srcs, single_src);
  else
    test_collect_archive_sibling_sources(archive_path, &srcs);

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
    if (!test_compile_archive_source(cc, srcs.items[i], obj_path)) {
      remove(obj_path);
      ok = 0;
      break;
    }
    sv_push(&objs, obj_path);
  }

  if (ok && objs.len > 0) {
    remove(archive_path);
    char **ar_argv = (char **)calloc(objs.len + 4, sizeof(char *));
    if (!ar_argv) {
      ok = 0;
    } else {
      size_t argc = 0;
      ar_argv[argc++] = (char *)"ar";
      ar_argv[argc++] = (char *)"rcs";
      ar_argv[argc++] = (char *)archive_path;
      for (size_t i = 0; i < objs.len; ++i)
        ar_argv[argc++] = objs.items[i];
      ar_argv[argc] = NULL;
      ok = run_debug_argv(ar_argv, 30, 1) == 0 && nyt_is_file(archive_path);
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

static int object_link_run_check(const char *shape_path) {
  if (!shape_path || !nyt_ends_with(shape_path, ".nshape"))
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

  // Collect all "link" entries (support multi-link for split archive tests etc.)
  StrVec links = {0};
  {
    char *data = read_small_file(shape_path);
    if (data) {
      char *line = data;
      while (line && *line) {
        char *nxt = strchr(line, '\n');
        if (nxt) *nxt = '\0';
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
  for (size_t i = 0; i < links.len; i++) {
    (void)test_build_missing_archive(links.items[i]);
  }

#ifdef _WIN32
  sv_free(&links);
  (void)ret_kind;
  (void)expected_val;
  free(expect_val);
  return 0;
#else
  const char *first_archive = links.len ? links.items[0] : NULL;
  int internal_rc = test_internal_elf64_link_run(obj_path, ret_kind, expected_val, shape_path,
                                                  first_archive);
  if (internal_rc == 0) {
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    sv_free(&links);
    free(expect_val);
    return 1;
  }
  internal_rc = test_internal_elf32_link_run(obj_path, ret_kind, expected_val, shape_path,
                                              first_archive);
  if (internal_rc == 0) {
    sv_free(&links);
    free(expect_val);
    return 0;
  }
  if (internal_rc == 1) {
    sv_free(&links);
    free(expect_val);
    return 1;
  }

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
  /* Native test objects are linked as fixed-address ELF images by the
     test harness.  Most modern Linux compilers default to PIE, which can
     make archive members with .text relocations print DT_TEXTREL warnings
     even when the test passes.  Link the throwaway harness as non-PIE so
     the native-object tests stay deterministic and quiet. */
  link_argv[link_argc++] = "-no-pie";
#endif
  link_argv[link_argc++] = obj_path;
  link_argv[link_argc++] = harness_path;
  for (size_t i = 0; i < links.len && link_argc < 10; i++) {
    link_argv[link_argc++] = links.items[i];
  }
  link_argv[link_argc++] = "-o";
  link_argv[link_argc++] = exe_path;
  if (ret_kind == NY_TEST_LINK_RET_F64)
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
  if (nyt_ends_with(path, ".nshape")) {
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
  if (std_path) {
    argv[argc++] = "--std";
    argv[argc++] = (char *)std_path;
  }
  if (std_bc) {
    argv[argc++] = "--std-bc";
    argv[argc++] = (char *)std_bc;
  }
  if (path_is_native_runtime_test(path) && !has_native_backend && argc < 78) {
    argv[argc++] = "--native-backend";
    argv[argc++] = "x86_64";
  }
  for (int i = 0; i < flagc && argc < 76; i++)
    argv[argc++] = flagv[i];
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
  char *matrix = (path && nyt_ends_with(path, ".nshape"))
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

static char *shape_source_block(const char *shape_path, const char *name) {
  if (!shape_path || !name || !*name)
    return NULL;
  char *data = read_small_file(shape_path);
  if (!data)
    return NULL;
  char needle[128];
  snprintf(needle, sizeof(needle), "source %s <<'", name);
  char *p = strstr(data, needle);
  if (!p) {
    free(data);
    return NULL;
  }
  char *marker = p + strlen(needle);
  char *marker_end = strchr(marker, '\'');
  char *body = marker_end ? strchr(marker_end, '\n') : NULL;
  if (!marker_end || !body || marker_end == marker) {
    free(data);
    return NULL;
  }
  body++;
  size_t marker_len = (size_t)(marker_end - marker);
  for (char *line = body; line && *line;) {
    char *next = strchr(line, '\n');
    size_t line_len = next ? (size_t)(next - line) : strlen(line);
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
  char *data = read_small_file(shape_path);
  if (!data)
    return NULL;
  size_t key_len = strlen(key);
  for (char *line = data; line && *line;) {
    char *next = strchr(line, '\n');
    if (next)
      *next = '\0';
    char *p = line;
    trim_inplace(p);
    if (strncmp(p, "source ", 7) == 0)
      break;
    if (strncmp(p, key, key_len) == 0 && isspace((unsigned char)p[key_len])) {
      p += key_len;
      trim_inplace(p);
      if (*p == '"') {
        char *start = ++p;
        char *end = start;
        while (*end && (*end != '"' || (end > start && end[-1] == '\\')))
          end++;
        char *out = decode_shape_quoted_string(start, (size_t)(end - start));
        free(data);
        return out;
      }
      char *out = strdup(p);
      free(data);
      return out;
    }
    line = next ? next + 1 : NULL;
  }
  free(data);
  return NULL;
}

static char *materialize_shape_ny_source(const char *shape_path) {
  char *source = shape_source_block(shape_path, "ny");
  if (!source)
    return NULL;
  char tmp[PATH_MAX];
#ifdef _WIN32
  char tmp_dir[PATH_MAX];
  DWORD tmp_len = GetTempPathA((DWORD)sizeof(tmp_dir), tmp_dir);
  if (tmp_len == 0 || tmp_len >= sizeof(tmp_dir))
    snprintf(tmp_dir, sizeof(tmp_dir), ".\\");
  static volatile LONG shape_seq = 0;
  LONG seq = InterlockedIncrement((volatile LONG *)&shape_seq);
  snprintf(tmp, sizeof(tmp), "%sny-shape-%lu-%lu-%ld.ny", tmp_dir,
           (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(), (long)seq);
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    free(source);
    return NULL;
  }
#else
  snprintf(tmp, sizeof(tmp), "%s/ny-shape-%ld-XXXXXX", nyt_temp_dir(), (long)getpid());
  int fd = mkstemp(tmp);
  if (fd < 0) {
    free(source);
    return NULL;
  }
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    remove(tmp);
    free(source);
    return NULL;
  }
#endif
  fwrite(source, 1, strlen(source), f);
  fclose(f);
  free(source);
  return strdup(tmp);
}

static void read_error_meta(const char *path, char **flags_out, char **expect_out) {
  *flags_out = NULL;
  *expect_out = NULL;
  if (path && nyt_ends_with(path, ".nshape")) {
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

static int test_is_error_path(const char *path) {
  return path && strncmp(path, "etc/tests/fuzz/errors/", 22) == 0;
}

static int test_is_ownership_error_path(const char *path) {
  return path && strstr(path, "etc/tests/fuzz/errors/ownership/") != NULL;
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
                            char *flags_buf, char **flags_out, char **expect_out) {
  int argc = 0;
  char *flagv[32];
  int flagc = 0;
  if (flags_out)
    *flags_out = NULL;
  if (expect_out)
    *expect_out = NULL;
  flags_buf[0] = '\0';
  if (test_is_error_path(path)) {
    read_error_meta(path, flags_out, expect_out);
    if (flags_out && *flags_out) {
      snprintf(flags_buf, 1024, "%s", *flags_out);
      trim_inplace(flags_buf);
      flagc = split_words(flags_buf, flagv, 32);
    }
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
  if (path_is_native_runtime_test(path)) {
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
    append_arg(argv, &argc, 192, "frame variable --show-types --show-location");
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
    char flags_buf[1024];
    char *flags = NULL;
    char *expect = NULL;
    char *materialized_path = NULL;
    const char *exec_path = path;
    if (path && nyt_ends_with(path, ".nshape")) {
      materialized_path = materialize_shape_ny_source(path);
      if (!materialized_path) {
        printf("%s[debug]%s cannot materialize shape source for %s\n", nyt_clr(NYT_GRAY),
               nyt_clr(NYT_RESET), disp_path(path));
        continue;
      }
      exec_path = materialized_path;
    }
    char *trace_argv[96];
    if (!build_trace_argv(trace_argv, 96, bin, path, exec_path, std_path, std_bc,
                          flags_buf, &flags, &expect)) {
      printf("%s[debug]%s cannot build replay argv for %s\n", nyt_clr(NYT_GRAY),
             nyt_clr(NYT_RESET), disp_path(path));
      if (materialized_path) {
        remove(materialized_path);
        free(materialized_path);
      }
      error_meta_free(flags, expect);
      continue;
    }
    gh_group_begin("trace replay", path);
    int trace_rc = run_debug_argv(trace_argv, timeout_sec, 0);
    printf("trace replay exit status: %d\n", trace_rc);
    gh_group_end();
    if (trace_rc != 0 && debugger && test_debugger_for_rc(trace_rc)) {
      gh_group_begin("debugger replay", path);
      run_debugger_replay(debugger, trace_argv, timeout_sec);
      gh_group_end();
    }
    if (materialized_path) {
      remove(materialized_path);
      free(materialized_path);
    }
    error_meta_free(flags, expect);
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
  if (path && nyt_ends_with(path, ".nshape")) {
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
  bool ownership_case = path && strstr(path, "etc/tests/fuzz/errors/ownership/") != NULL;
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

static int auto_test_jobs(void) {
  long logical = ny_cpu_count();
  if (logical <= 0)
    logical = 1;
  int jobs = (int)(logical * 0.5);
  if (logical >= 2 && jobs < 2)
    jobs = 2;
  if (jobs < 1)
    jobs = 1;
  if (jobs > 24)
    jobs = 24;
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

static uint64_t test_sig(const char *path, const char *bin, const char *std_path, const char *std_bc) {
  uint64_t h = 1469598103934665603ULL;
  uint64_t a = file_sig(path), b = file_sig(bin), c = file_sig(std_path), d = file_sig(std_bc);
  const char *mode = path_is_native_runtime_test(path) ? "native:x86_64" : "default";
  h = fnv1a_update(h, &a, sizeof(a));
  h = fnv1a_update(h, &b, sizeof(b));
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
        if (colon && atoi(colon + 1) > 0)
          cpu_cores = atoi(colon + 1);
      } else if (strncmp(line, "siblings", 8) == 0) {
        char *colon = strchr(line, ':');
        if (colon && atoi(colon + 1) > 0)
          siblings = atoi(colon + 1);
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
  return p && (strncmp(p, "probe/", 6) == 0 || strstr(p, "/probe/") != NULL);
}

static int path_is_native_runtime_test(const char *p) {
  return p && strncmp(p, "etc/tests/rt/native/", 20) == 0;
}

static const char *suite_for_path(const char *p, const char *fallback) {
  if (p && strncmp(p, "etc/tests/fuzz/bench/", 21) == 0)
    return "Benchmark";
  if (p && strncmp(p, "etc/tests/rt/", 13) == 0)
    return "Runtime";
  if (path_is_probe_test(p))
    return "Probe";
  return fallback ? fallback : "Std";
}

static SuiteStats *stats_for_path(const char *p, SuiteStats *fallback, SuiteStats *benchmark,
                                  SuiteStats *runtime, SuiteStats *probe, SuiteStats *std) {
  if (p && strncmp(p, "etc/tests/fuzz/bench/", 21) == 0)
    return benchmark ? benchmark : fallback;
  if (p && strncmp(p, "etc/tests/rt/", 13) == 0)
    return runtime ? runtime : fallback;
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
  if (strncmp(p, "etc/tests/fuzz/bench/", 21) == 0)
    return 5000;
  if (strstr(p, "rt/bigint.ny") || strstr(p, "rt/attr.ny") || strstr(p, "rt/sizeof.ny") ||
      strstr(p, "rt/asm.ny"))
    return 5000;
  if (strstr(p, "rt/comptime.ny"))
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
                     SuiteStats *runtime_stats, SuiteStats *probe_stats, SuiteStats *std_stats,
                     CacheDb *cache, int use_cache, TimingVec *timings, StrVec *failed_paths) {
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
            stats_for_path(p, stats, benchmark_stats, runtime_stats, probe_stats, std_stats);
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
  if (!run) {
    sv_free(&selected);
    return 1;
  }

  size_t launched = 0;
  while (completed < total) {
    for (int i = 0; i < jobs && launched < total; i++) {
      if (run[i].active)
        continue;
      const char *p = selected.items[launched++];
      if (!p)
        continue;
      run[i].tmp_out[0] = '\0';
      run[i].materialized_path[0] = '\0';
      const char *exec_path = p;
      int is_bench = strncmp(p, "etc/tests/fuzz/bench/", 21) == 0;
      if (p && nyt_ends_with(p, ".nshape")) {
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
          stats_for_path(run[i].path, stats, benchmark_stats, runtime_stats, probe_stats, std_stats);
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
  sv_free(&selected);
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
  StrVec files = {0};
  StrVec patterns = {0};
  StrVec failed_paths = {0};
  TimingVec timings = {0};
  CacheDb cache = {0};
  char err[256];

  const char *env_timeout = getenv("NYTRIX_TEST_TIMEOUT");
  if (env_timeout && *env_timeout)
    timeout_sec = normalize_test_timeout(atoi(env_timeout));

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
      printf("  %s--phase-times --trace-ir --debug-failures --debugger-all%s\n",
             nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("  %s--color MODE --no-color%s\n\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
      printf("%snotes:%s timeout defaults to 60s and is capped at 300s; error tests live under %setc/tests/fuzz/errors%s\n",
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
    else if (!strcmp(a, "--progress-selftest"))
      return run_progress_selftest(bin, timeout_sec);
    else if (!strcmp(a, "--debug-failures"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "1", 1);
    else if (!strcmp(a, "--no-debug-failures"))
      ny_setenv("NYTRIX_TEST_DEBUG_FAILURES", "0", 1);
    else if (!strcmp(a, "--debugger-all"))
      ny_setenv("NYTRIX_TEST_DEBUGGER_ALL", "1", 1);
    else if (a[0] == '-') {
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

  const char *ws = getenv("NYTRIX_TEST_WITH_STDLIB");
  if (ws && *ws && (*ws != '0') && strcmp(ws, "false") != 0)
    with_stdlib = 1;

  if (files.len == 0) {
    collect_ny("etc/tests/rt", &files);
    collect_ny("etc/tests/fuzz/errors", &files);
    collect_ny("etc/tests/fuzz/bench", &files);
    if (with_stdlib)
      collect_ny("lib", &files);
  }

  if (jobs <= 0) {
    const char *ej = getenv("NYTRIX_TEST_JOBS");
    if (ej && *ej)
      jobs = atoi(ej);
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

  StrVec benchmark = {0}, runtime = {0}, repl = {0}, probe = {0}, error_tests = {0}, std = {0};
  SuiteStats sb = {0}, sr = {0}, srepl = {0}, sp = {0}, se = {0}, ss = {0};
  char cache_path[PATH_MAX];
  nyt_path_join(cache_path, sizeof(cache_path), nyt_default_cache_root_dir(),
                "test-results.tsv");
  if (use_cache)
    cache_load(&cache, cache_path);
  for (size_t i = 0; i < limit; i++) {
    const char *p = files.items[i];
    if (strncmp(p, "etc/tests/fuzz/bench/", 21) == 0)
      sv_push(&benchmark, p);
    else if (strncmp(p, "etc/tests/rt/", 13) == 0)
      sv_push(&runtime, p);
    else if (path_is_probe_test(p))
      sv_push(&probe, p);
    else if (strncmp(p, "etc/tests/fuzz/errors/", 22) == 0)
      sv_push(&error_tests, p);
    else
      sv_push(&std, p);
  }

  StrVec selected_all = {0};
  for (size_t i = 0; i < benchmark.len; i++)
    sv_push(&selected_all, benchmark.items[i]);
  for (size_t i = 0; i < runtime.len; i++)
    sv_push(&selected_all, runtime.items[i]);
  for (size_t i = 0; i < probe.len; i++)
    sv_push(&selected_all, probe.items[i]);
  if (with_stdlib) {
    for (size_t i = 0; i < std.len; i++)
      sv_push(&selected_all, std.items[i]);
  }

  run_suite("Tests", &selected_all, bin, std_path, std_bc, &patterns, jobs, timeout_sec, &passed,
            &failed, NULL, &sb, &sr, &sp, &ss, &cache, use_cache, &timings, &failed_paths);
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
  sv_free(&repl);
  sv_free(&probe);
  sv_free(&error_tests);
  sv_free(&std);
  timings_free(&timings);
  cache_free(&cache);
  sv_free(&patterns);
  sv_free(&failed_paths);
  sv_free(&files);
  return failed ? 1 : 0;
}
