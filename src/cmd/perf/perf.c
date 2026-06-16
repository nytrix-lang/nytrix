#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "perf.h"
#include "base/args.h"
#include "base/util.h"
#include "../tools/repo.h"
#include "../tools/tool.h"

#ifdef _WIN32
int ny_perf_main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  nyt_err("ny-perf", "perf tooling is not available on Windows");
  return 2;
}
#else

#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  const char *path;
  const char *profile;
} PerfCase;

typedef struct {
  char id[256];
  double median_ms;
} PerfResult;

typedef struct {
  const char *name;
  const char *opt_flag;
  const char *builtin_flag;
  const char *profile;
} PerfCompareVariant;

typedef struct {
  char case_name[128];
  char path[PATH_MAX];
  char variant[96];
  int ok;
  int rc;
  double wall_ms;
  double bench_ms;
  double total_ms;
  double codegen_ms;
  double opt_ms;
  double jit_compile_ms;
  double jit_run_ms;
  char stdout_path[PATH_MAX];
  char stderr_path[PATH_MAX];
  char suspect[128];
} PerfCompareRow;

typedef struct {
  char label[128];
  char path[PATH_MAX];
} PerfExecTarget;

typedef struct {
  char label[128];
  char path[PATH_MAX];
  int ok;
  int rc;
  double wall_ms;
  char stdout_path[PATH_MAX];
  char stderr_path[PATH_MAX];
} PerfExecRow;

enum { PERF_MAX_EXEC_TARGETS = 64 };

static const PerfCase k_cases[] = {
    {"etc/tests/fuzz/bench/binary.nshape", "compile"},
    {"etc/tests/fuzz/bench/dict.nshape", "balanced"},
    {"etc/tests/fuzz/bench/fibonacci.nshape", "speed"},
    {"etc/tests/fuzz/bench/float.nshape", "speed"},
    {"etc/tests/fuzz/bench/intops.nshape", "speed"},
    {"etc/tests/fuzz/bench/iter.nshape", "balanced"},
    {"etc/tests/fuzz/bench/list.nshape", "balanced"},
    {"etc/tests/fuzz/bench/mandelbrot.nshape", "speed"},
    {"etc/tests/fuzz/bench/sieve.nshape", "size"},
    {"etc/tests/fuzz/bench/spectral.nshape", "speed"},
    {"etc/tests/fuzz/bench/vector.nshape", "speed"},
};

enum { PERF_CASE_COUNT = (int)(sizeof(k_cases) / sizeof(k_cases[0])) };

static const PerfCompareVariant k_compare_variants[] = {
    {"c-native", NULL, NULL, NULL},
    {"ny-default-stdops", NULL, "--std-builtin-ops", NULL},
};

enum { PERF_COMPARE_VARIANT_COUNT = (int)(sizeof(k_compare_variants) / sizeof(k_compare_variants[0])) };

static char *read_small_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long n = ftell(f);
  if (n < 0 || n > 16 * 1024 * 1024) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = '\0';
  return buf;
}

static int mkdir_p(const char *path) {
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a;
  double y = *(const double *)b;
  return (x > y) - (x < y);
}

static double median(double *vals, int n) {
  if (n <= 0)
    return 0.0;
  qsort(vals, (size_t)n, sizeof(double), cmp_double);
  if (n % 2)
    return vals[n / 2];
  return 0.5 * (vals[n / 2 - 1] + vals[n / 2]);
}

static void json_string(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    switch (*p) {
    case '\\':
      fputs("\\\\", f);
      break;
    case '"':
      fputs("\\\"", f);
      break;
    case '\n':
      fputs("\\n", f);
      break;
    case '\r':
      fputs("\\r", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    default:
      if (*p < 32)
        fprintf(f, "\\u%04x", *p);
      else
        fputc(*p, f);
      break;
    }
  }
  fputc('"', f);
}

static int perf_path_has_sep(const char *cmd) {
  return cmd && (strchr(cmd, '/') || strchr(cmd, '\\'));
}

static int perf_probe_command_at(const char *dir, const char *cmd) {
  char full[PATH_MAX];
  const char *base = (dir && *dir) ? dir : ".";
  size_t base_n = strlen(base);
  size_t cmd_n = strlen(cmd);
  if (base_n + 1 + cmd_n >= sizeof(full))
    return 0;
  memcpy(full, base, base_n);
  full[base_n] = '/';
  memcpy(full + base_n + 1, cmd, cmd_n + 1);
  if (ny_access(full, X_OK) == 0 || nyt_is_file(full))
    return 1;
#ifdef _WIN32
  if (!nyt_ends_with(cmd, ".exe")) {
    if (base_n + 1 + cmd_n + 4 >= sizeof(full))
      return 0;
    memcpy(full, base, base_n);
    full[base_n] = '/';
    memcpy(full + base_n + 1, cmd, cmd_n);
    memcpy(full + base_n + 1 + cmd_n, ".exe", 5);
    if (ny_access(full, X_OK) == 0 || nyt_is_file(full))
      return 1;
  }
#endif
  return 0;
}

static int perf_command_available(const char *cmd) {
  if (!cmd || !*cmd)
    return 0;
  if (perf_path_has_sep(cmd))
    return ny_access(cmd, X_OK) == 0 || nyt_is_file(cmd);
  const char *path = getenv("PATH");
  if (!path || !*path)
    return 0;
  char *copy = strdup(path);
  if (!copy)
    return 0;
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  int ok = 0;
  for (char *p = copy; p && *p;) {
    char *next = strchr(p, sep);
    if (next)
      *next = '\0';
    if (perf_probe_command_at(p, cmd)) {
      ok = 1;
      break;
    }
    if (!next)
      break;
    p = next + 1;
  }
  free(copy);
  return ok;
}

static const char *last_substr(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return NULL;
  const char *last = NULL;
  const char *p = haystack;
  while ((p = strstr(p, needle)) != NULL) {
    last = p;
    p++;
  }
  return last;
}

static double parse_number_after(const char *p, int *ok) {
  if (ok)
    *ok = 0;
  if (!p)
    return 0.0;
  while (*p && !isdigit((unsigned char)*p) && *p != '-' && *p != '+')
    p++;
  if (!*p)
    return 0.0;
  char *end = NULL;
  double v = strtod(p, &end);
  if (!end || end == p)
    return 0.0;
  if (ok)
    *ok = 1;
  return v;
}

static double parse_label_seconds_ms(const char *text, const char *label) {
  const char *p = strstr(text ? text : "", label);
  if (!p)
    return 0.0;
  int ok = 0;
  double sec = parse_number_after(p + strlen(label), &ok);
  return ok ? sec * 1000.0 : 0.0;
}

static double parse_benchmark_reported_ms(const char *text) {
  const char *p = last_substr(text ? text : "", "Time(ns)");
  if (p) {
    int ok = 0;
    double ns = parse_number_after(p, &ok);
    if (ok)
      return ns / 1000000.0;
  }
  p = last_substr(text ? text : "", "Time:");
  if (p) {
    int ok = 0;
    double ms = parse_number_after(p, &ok);
    if (ok)
      return ms;
  }
  return 0.0;
}

static void safe_stem(char *out, size_t out_sz, const char *s) {
  if (!out || out_sz == 0)
    return;
  size_t n = 0;
  for (const unsigned char *p = (const unsigned char *)(s ? s : "case"); *p && n + 1 < out_sz; p++) {
    if (isalnum(*p))
      out[n++] = (char)*p;
    else
      out[n++] = '-';
  }
  if (n == 0 && out_sz > 1)
    out[n++] = 'x';
  out[n] = '\0';
}

static void case_name_from_path(char *out, size_t out_sz, const char *path) {
  const char *s = path ? strrchr(path, '/') : NULL;
  snprintf(out, out_sz, "%s", s ? s + 1 : (path ? path : ""));
  char *dot = strrchr(out, '.');
  if (dot)
    *dot = '\0';
}

static const char *suspect_for_case(const char *name) {
  if (!name)
    return "unknown";
  if (strstr(name, "int") || strstr(name, "float") || strstr(name, "fib") ||
      strstr(name, "mandelbrot") || strstr(name, "spectral"))
    return "numeric loops and scalar operations";
  if (strstr(name, "list") || strstr(name, "iter") || strstr(name, "vector"))
    return "list, iterator, and bounds/index operations";
  if (strstr(name, "dict") || strstr(name, "binary"))
    return "hash/dict lookup and allocation behavior";
  if (strstr(name, "string") || strstr(name, "json") || strstr(name, "sql"))
    return "string/byte scanning and parser-style loops";
  return "general runtime and codegen overhead";
}

static int run_cmd(char *const argv[], const char *out_file, const char *err_file) {
  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (pid == 0) {
    if (out_file) {
      int fd = open(out_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
    }
    if (err_file) {
      int fd = open(err_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
    execvp(argv[0], argv);
    _exit(127);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 1;
}

static int run_cmd_timeout(char *const argv[], const char *out_file, const char *err_file,
                           int timeout_sec, double *elapsed_ms) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (pid == 0) {
    if (out_file) {
      int fd = open(out_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
    }
    if (err_file) {
      int fd = open(err_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
    execvp(argv[0], argv);
    _exit(127);
  }

  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      break;
    if (r < 0)
      return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_s = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    if (elapsed_s >= (double)timeout_sec) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      if (elapsed_ms)
        *elapsed_ms = elapsed_s * 1000.0;
      return 2;
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (elapsed_ms) {
    *elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
                  ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 1;
}

static int extract_shape_source(const char *shape_path, const char *lang, const char *tag,
                                const char *out_path) {
  char *src = read_small_file(shape_path);
  if (!src)
    return 0;
  char marker[64];
  snprintf(marker, sizeof(marker), "source %s <<'%s'", lang, tag);
  char *start = strstr(src, marker);
  if (!start) {
    free(src);
    return 0;
  }
  start += strlen(marker);
  if (*start == '\r')
    start++;
  if (*start == '\n')
    start++;
  char end_marker[16];
  snprintf(end_marker, sizeof(end_marker), "\n%s\n", tag);
  char *end = strstr(start, end_marker);
  if (!end) {
    free(src);
    return 0;
  }
  FILE *f = fopen(out_path, "w");
  if (!f) {
    free(src);
    return 0;
  }
  fwrite(start, 1, (size_t)(end - start), f);
  fputc('\n', f);
  fclose(f);
  free(src);
  return 1;
}

static int run_one_compare_ny(const char *bin, const char *path, const PerfCompareVariant *variant,
                              const char *out_file, const char *err_file, int timeout_sec,
                              int scale_percent, const char *cache_dir, double *elapsed_ms) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (pid == 0) {
    if (out_file) {
      int fd = open(out_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        close(fd);
      }
    }
    if (err_file) {
      int fd = open(err_file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
      if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        close(fd);
      }
    }
    char scale_buf[32];
    snprintf(scale_buf, sizeof(scale_buf), "%d", scale_percent);
    ny_setenv("NYTRIX_BENCH_SCALE", scale_buf, 1);
    if (variant && variant->profile && *variant->profile)
      ny_setenv("NYTRIX_OPT_PROFILE", variant->profile, 1);
    else
      ny_unsetenv("NYTRIX_OPT_PROFILE");
    ny_setenv("NYTRIX_AUTO_PURITY", "1", 1);
    ny_setenv("NYTRIX_AUTO_MEMO_IMPURE", "1", 1);
    ny_setenv("NYTRIX_NO_TRACE", "1", 1);
    if (cache_dir && *cache_dir) {
      ny_setenv("NYTRIX_CACHE_DIR", cache_dir, 1);
      if (nyt_env_truthy("NYTRIX_PERF_NATIVE_CACHE"))
        ny_setenv("NYTRIX_JIT_NATIVE_CACHE", "1", 1);
    }

    if (variant && strcmp(variant->name, "c-native") == 0) {
      if (nyt_ends_with(path, ".nshape")) {
        char c_path[PATH_MAX], bin_path[PATH_MAX];
        snprintf(c_path, sizeof(c_path), "%s/ny-perf-%ld.c", nyt_temp_dir(), (long)getpid());
        snprintf(bin_path, sizeof(bin_path), "%s/ny-perf-%ld.out", nyt_temp_dir(), (long)getpid());
        if (extract_shape_source(path, "c", "C", c_path)) {
          char cmd[PATH_MAX * 2 + 64];
          snprintf(cmd, sizeof(cmd), "cc -O3 '%s' -o '%s'", c_path, bin_path);
          if (system(cmd) == 0) {
            char *run_argv[] = {bin_path, NULL};
            execv(bin_path, run_argv);
          }
        }
      }
      _exit(1);
    }

    char ny_path[PATH_MAX];
    const char *run_path = path;
    if (nyt_ends_with(path, ".nshape")) {
      snprintf(ny_path, sizeof(ny_path), "%s/ny-perf-%ld.ny", nyt_temp_dir(), (long)getpid());
      if (!extract_shape_source(path, "ny", "NY", ny_path))
        _exit(1);
      run_path = ny_path;
    }

    char *run_argv[8];
    int k = 0;
    run_argv[k++] = (char *)bin;
    if (variant && variant->opt_flag && *variant->opt_flag)
      run_argv[k++] = (char *)variant->opt_flag;
    if (variant && variant->builtin_flag && *variant->builtin_flag)
      run_argv[k++] = (char *)variant->builtin_flag;
    run_argv[k++] = "-time";
    run_argv[k++] = "-run";
    run_argv[k++] = (char *)run_path;
    run_argv[k] = NULL;
    execvp(run_argv[0], run_argv);
    _exit(127);
  }

  int status = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      break;
    if (r < 0)
      return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_s = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    if (elapsed_s >= (double)timeout_sec) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      if (elapsed_ms)
        *elapsed_ms = elapsed_s * 1000.0;
      return 2;
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  if (elapsed_ms) {
    *elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
                  ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return 1;
}

static int run_one_gate(const char *bin, const char *path, const char *profile, const char *cache_dir,
                        int use_native_cache, int timeout_sec, double *elapsed_ms) {
  int status = 0;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  if (pid < 0)
    return 1;
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    ny_setenv("NYTRIX_OPT_PROFILE", profile, 1);
    ny_setenv("NYTRIX_AUTO_PURITY", "1", 1);
    ny_setenv("NYTRIX_AUTO_MEMO_IMPURE", "1", 1);
    ny_setenv("NYTRIX_NO_TRACE", "1", 1);
    if (cache_dir && *cache_dir)
      ny_setenv("NYTRIX_CACHE_DIR", cache_dir, 1);
    if (use_native_cache && nyt_env_truthy("NYTRIX_PERF_NATIVE_CACHE"))
      ny_setenv("NYTRIX_JIT_NATIVE_CACHE", "1", 1);
    execl(bin, bin, "-time", "-run", path, (char *)NULL);
    _exit(127);
  }

  for (;;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid)
      break;
    if (r < 0)
      return 1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed_s = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    if (elapsed_s >= (double)timeout_sec) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      return 2;
    }
    struct timespec ts = {0, 1000000L};
    nanosleep(&ts, NULL);
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  *elapsed_ms = ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
                ((double)(t1.tv_nsec - t0.tv_nsec) / 1000000.0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return 1;
  return 0;
}

static int write_baseline(const char *path, PerfResult *res, int n) {
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", path ? path : "");
  char *slash = strrchr(dir, '/');
  if (slash)
    *slash = '\0';
  if (slash && !mkdir_p(dir))
    return 1;
  FILE *f = fopen(path, "w");
  if (!f)
    return 1;
  fprintf(f, "{\n  \"measurements\": {\n");
  for (int i = 0; i < n; i++) {
    fprintf(f, "    \"%s\": %.6f%s\n", res[i].id, res[i].median_ms, (i + 1 < n) ? "," : "");
  }
  fprintf(f, "  }\n}\n");
  fclose(f);
  return 0;
}

static double load_baseline_value(const char *path, const char *id) {
  FILE *f = fopen(path, "r");
  if (!f)
    return -1.0;
  char *line = NULL;
  size_t cap = 0;
  double out = -1.0;
  while (getline(&line, &cap, f) > 0) {
    if (!strstr(line, id))
      continue;
    char *p = strrchr(line, ':');
    if (!p)
      continue;
    out = atof(p + 1);
    break;
  }
  free(line);
  fclose(f);
  return out;
}

static int perf_available(void) {
  char *argv[] = {"perf", "--version", NULL};
  return run_cmd(argv, "/dev/null", "/dev/null") == 0;
}

static const char *base_name(const char *path) {
  return ny_base_name(path);
}

static void strip_ext(char *s) {
  char *dot = strrchr(s, '.');
  if (dot)
    *dot = '\0';
}

static void exec_label_from_path(char *out, size_t out_sz, const char *path) {
  if (!out || out_sz == 0)
    return;
  char raw[256];
  snprintf(raw, sizeof(raw), "%s", base_name(path && *path ? path : "target"));
  strip_ext(raw);
  safe_stem(out, out_sz, raw);
}

static int add_exec_target(PerfExecTarget *targets, int *target_count, int max_targets,
                           const char *spec) {
  if (!targets || !target_count || !spec || !*spec || *target_count >= max_targets)
    return 0;

  char label_raw[128] = {0};
  const char *path = spec;
  const char *eq = strchr(spec, '=');
  if (eq && eq > spec && eq[1] != '\0') {
    size_t n = (size_t)(eq - spec);
    if (n >= sizeof(label_raw))
      n = sizeof(label_raw) - 1;
    memcpy(label_raw, spec, n);
    label_raw[n] = '\0';
    path = eq + 1;
  } else {
    exec_label_from_path(label_raw, sizeof(label_raw), path);
  }
  if (!path || !*path)
    return 0;

  PerfExecTarget *t = &targets[*target_count];
  safe_stem(t->label, sizeof(t->label), label_raw);
  snprintf(t->path, sizeof(t->path), "%s", path);
  (*target_count)++;
  return 1;
}

static char **alloc_command_argv(const char *first, const char *second, const StrVec *extra,
                                 int *out_argc) {
  int extra_n = extra ? (int)extra->len : 0;
  int prefix_n = (first && *first) ? 1 : 0;
  if (second && *second)
    prefix_n++;
  int argc = prefix_n + extra_n;
  char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (!argv)
    return NULL;
  int k = 0;
  if (first && *first)
    argv[k++] = (char *)first;
  if (second && *second)
    argv[k++] = (char *)second;
  for (int i = 0; i < extra_n; i++)
    argv[k++] = extra->items[i];
  argv[k] = NULL;
  if (out_argc)
    *out_argc = argc;
  return argv;
}

static void print_head(const char *path, int max_lines) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char *line = NULL;
  size_t cap = 0;
  int n = 0;
  while (n < max_lines && getline(&line, &cap, f) > 0) {
    fputs(line, stdout);
    n++;
  }
  free(line);
  fclose(f);
}

static int run_profile_command_mode(const char *target_label, int freq, const char *out_root,
                                    char *const command_argv[], int command_argc) {
  if (!perf_available()) {
    nyt_err("ny-perf", "Linux perf tool not available");
    return 1;
  }
  if (command_argc <= 0 || !command_argv || !command_argv[0] || !*command_argv[0]) {
    nyt_err("ny-perf", "profile mode requires a command");
    return 2;
  }
  if (!perf_command_available(command_argv[0])) {
    nyt_err("ny-perf", "profile target not found: %s", command_argv[0]);
    return 1;
  }
  if (!mkdir_p(out_root)) {
    nyt_err("ny-perf", "could not create output dir: %s", out_root);
    return 1;
  }

  time_t now = time(NULL);
  struct tm tmv;
  localtime_r(&now, &tmv);
  char stamp[64];
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

  char stem[256];
  safe_stem(stem, sizeof(stem), target_label && *target_label ? target_label : base_name(command_argv[0]));

  char session[PATH_MAX];
  char session_name[512];
  snprintf(session_name, sizeof(session_name), "perf-%s-%s", stem, stamp);
  nyt_path_join(session, sizeof(session), out_root, session_name);
  if (!mkdir_p(session)) {
    nyt_err("ny-perf", "could not create session dir: %s", session);
    return 1;
  }

  char perf_data[PATH_MAX], report_txt[PATH_MAX], callgraph_txt[PATH_MAX], stat_txt[PATH_MAX];
  nyt_path_join(perf_data, sizeof(perf_data), session, "perf.data");
  nyt_path_join(report_txt, sizeof(report_txt), session, "report.txt");
  nyt_path_join(callgraph_txt, sizeof(callgraph_txt), session, "callgraph.txt");
  nyt_path_join(stat_txt, sizeof(stat_txt), session, "stat.txt");

  nyt_heading("Nytrix Performance Profiler");
  nyt_kv("target", "%s", target_label && *target_label ? target_label : command_argv[0]);
  nyt_kv("command", "%s", command_argv[0]);
  nyt_kv("session", "%s", session);

  int argv_n = 7 + command_argc + 1;
  char **record_argv = (char **)calloc((size_t)argv_n, sizeof(char *));
  if (!record_argv)
    return 1;
  char freq_s[32];
  snprintf(freq_s, sizeof(freq_s), "%d", freq);
  int k = 0;
  record_argv[k++] = "perf";
  record_argv[k++] = "record";
  record_argv[k++] = "-F";
  record_argv[k++] = freq_s;
  record_argv[k++] = "-g";
  record_argv[k++] = "-o";
  record_argv[k++] = perf_data;
  for (int i = 0; i < command_argc; i++)
    record_argv[k++] = command_argv[i];
  record_argv[k] = NULL;

  nyt_msg("RECORD", NYT_YELLOW, "perf record");
  int rc = run_cmd(record_argv, NULL, NULL);
  free(record_argv);
  if (rc != 0) {
    nyt_err("ny-perf", "perf record failed (rc=%d)", rc);
    return rc;
  }

  char *report_argv[] = {"perf", "report", "-n", "--stdio", "-i", perf_data, "--max-stack", "30", NULL};
  char *cg_argv[] = {"perf", "report", "-n", "--stdio", "-i", perf_data, "--children", "--hierarchy",
                     "--sort", "overhead,dso,symbol", "--max-stack", "30", NULL};

  int pfd[2];
  if (pipe(pfd) != 0)
    return 1;
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    dup2(pfd[1], STDOUT_FILENO);
    dup2(pfd[1], STDERR_FILENO);
    close(pfd[1]);
    execvp(report_argv[0], report_argv);
    _exit(127);
  }
  close(pfd[1]);
  FILE *rf = fopen(report_txt, "w");
  if (!rf)
    return 1;
  char buf[4096];
  ssize_t rn;
  while ((rn = read(pfd[0], buf, sizeof(buf))) > 0)
    fwrite(buf, 1, (size_t)rn, rf);
  close(pfd[0]);
  fclose(rf);
  int st = 0;
  waitpid(pid, &st, 0);

  rc = run_cmd(cg_argv, callgraph_txt, callgraph_txt);
  if (rc != 0)
    nyt_warn("ny-perf", "callgraph generation failed");

  int stat_n = 4 + command_argc + 1;
  char **stat_argv = (char **)calloc((size_t)stat_n, sizeof(char *));
  if (!stat_argv)
    return 1;
  k = 0;
  stat_argv[k++] = "perf";
  stat_argv[k++] = "stat";
  stat_argv[k++] = "-r";
  stat_argv[k++] = "3";
  for (int i = 0; i < command_argc; i++)
    stat_argv[k++] = command_argv[i];
  stat_argv[k] = NULL;
  rc = run_cmd(stat_argv, stat_txt, stat_txt);
  free(stat_argv);
  if (rc != 0)
    nyt_warn("ny-perf", "perf stat failed");

  printf("\n%s%sTOP HOTSPOTS (report)%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  print_head(report_txt, 120);
  printf("\n%s%sCALL GRAPH%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  print_head(callgraph_txt, 160);
  printf("\n%s%sPERF STAT%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  print_head(stat_txt, 80);
  nyt_msg("SAVED", NYT_GREEN, "profile: %s", session);
  return 0;
}

static int run_profile_mode(const char *repo, const char *bin, const char *script, int freq,
                            const char *out_root, const StrVec *script_args) {
  if (!nyt_is_file(script)) {
    nyt_err("ny-perf", "script not found: %s", script);
    return 1;
  }
  int command_argc = 0;
  char **command_argv = alloc_command_argv(bin, script, script_args, &command_argc);
  if (!command_argv)
    return 1;

  char label[256];
  snprintf(label, sizeof(label), "%s", base_name(script));
  strip_ext(label);
  int rc = run_profile_command_mode(label, freq, out_root, command_argv, command_argc);
  free(command_argv);
  (void)repo;
  return rc;
}

static int run_profile_exec_mode(const char *exec_path, int freq, const char *out_root,
                                 const StrVec *exec_args) {
  int command_argc = 0;
  char **command_argv = alloc_command_argv(exec_path, NULL, exec_args, &command_argc);
  if (!command_argv)
    return 1;

  char label[256];
  snprintf(label, sizeof(label), "%s", base_name(exec_path));
  strip_ext(label);
  int rc = run_profile_command_mode(label, freq, out_root, command_argv, command_argc);
  free(command_argv);
  return rc;
}

static int run_gate_mode(const char *repo, const char *bin, int write_bl) {
  (void)repo;
  printf("%s%sNytrix Performance Gate%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  int cold_mode = nyt_env_truthy("NYTRIX_PERF_COLD");
  const char *build_cache = nyt_default_cache_root_dir();
  char gate_cache_root[PATH_MAX] = {0};
  if (!cold_mode) {
    if (!mkdir_p(build_cache)) {
      nyt_err("ny-perf", "could not create %s", build_cache);
      return 1;
    }
    char stamp[128];
    snprintf(stamp, sizeof(stamp), "perf-gate-cache-%ld-%d", (long)time(NULL), (int)getpid());
    nyt_path_join(gate_cache_root, sizeof(gate_cache_root), build_cache, stamp);
    if (!mkdir_p(gate_cache_root)) {
      nyt_err("ny-perf", "could not create %s", gate_cache_root);
      return 1;
    }
    if (nyt_env_truthy("NYTRIX_PERF_NATIVE_CACHE"))
      nyt_msg("WARM", NYT_CYAN, "using isolated native JIT cache: %s", gate_cache_root);
    else
      nyt_msg("WARM", NYT_CYAN,
              "using isolated bitcode JIT cache: %s (set NYTRIX_PERF_NATIVE_CACHE=1 for native-cache gate)",
              gate_cache_root);
  } else {
    nyt_msg("MODE", NYT_YELLOW,
            "cold mode (set NYTRIX_PERF_COLD=0 for warm bitcode-cache gate)");
  }
  PerfResult results[PERF_CASE_COUNT];
  int rc = 0;

  for (size_t i = 0; i < PERF_CASE_COUNT; i++) {
    char case_cache_dir[PATH_MAX] = {0};
    if (!cold_mode) {
      char case_dir[128];
      snprintf(case_dir, sizeof(case_dir), "%02zu-%s", i + 1, k_cases[i].profile);
      nyt_path_join(case_cache_dir, sizeof(case_cache_dir), gate_cache_root, case_dir);
      if (!mkdir_p(case_cache_dir)) {
        nyt_err("ny-perf", "failed to create cache dir: %s", case_cache_dir);
        return 1;
      }
    }
    double samples[5] = {0};
    for (int w = 0; w < 2; w++) {
      double tmp = 0.0;
      (void)run_one_gate(bin, k_cases[i].path, k_cases[i].profile, cold_mode ? NULL : case_cache_dir, !cold_mode,
                         60, &tmp);
    }
    int sample_n = 0;
    for (int r = 0; r < 5; r++) {
      double ms = 0.0;
      int rrc = run_one_gate(bin, k_cases[i].path, k_cases[i].profile, cold_mode ? NULL : case_cache_dir, !cold_mode,
                             60, &ms);
      if (rrc != 0) {
        nyt_err("ny-perf", "failed benchmark %s (rc=%d)", k_cases[i].path, rrc);
        rc = 1;
        break;
      }
      samples[sample_n++] = ms;
    }
    if (rc)
      break;
    snprintf(results[i].id, sizeof(results[i].id), "%s::%s", k_cases[i].path, k_cases[i].profile);
    results[i].median_ms = median(samples, sample_n);
    printf("%s✓%s %-35s %s%-10s%s med=%s%.2fms%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET),
           k_cases[i].path, nyt_clr(NYT_CYAN), k_cases[i].profile, nyt_clr(NYT_RESET),
           nyt_clr(NYT_BOLD), results[i].median_ms, nyt_clr(NYT_RESET));
  }
  if (rc)
    return rc;

  char baseline[PATH_MAX];
  if (cold_mode)
    nyt_path_join(baseline, sizeof(baseline), build_cache,
                  "perf_gate_baseline.cold.json");
  else
    nyt_path_join(baseline, sizeof(baseline), build_cache,
                  "perf_gate_baseline.warm.json");

  if (write_bl) {
    if (write_baseline(baseline, results, PERF_CASE_COUNT) != 0) {
      nyt_err("ny-perf", "failed writing baseline: %s", baseline);
      return 1;
    }
    nyt_msg("OK", NYT_GREEN, "updated baseline");
    return 0;
  }

  int regressions = 0;
  if (nyt_is_file(baseline)) {
    printf("%sComparison with baseline (%s mode):%s\n", nyt_clr(NYT_BOLD), cold_mode ? "cold" : "warm",
           nyt_clr(NYT_RESET));
    for (size_t i = 0; i < PERF_CASE_COUNT; i++) {
      double base = load_baseline_value(baseline, results[i].id);
      if (base <= 0.0)
        continue;
      double pct = ((results[i].median_ms - base) / base) * 100.0;
      if (pct > 10.0)
        regressions++;
      const char *col = (pct > 10.0) ? nyt_clr(NYT_RED) : ((pct < -10.0) ? nyt_clr(NYT_GREEN) : nyt_clr(NYT_GRAY));
      printf("  %-45s %8.2f -> %8.2f ms (%s%+.1f%%%s)\n", results[i].id, base, results[i].median_ms,
             col, pct, nyt_clr(NYT_RESET));
    }
  }

  if (regressions) {
    nyt_err("ny-perf", "performance regressions detected: %d", regressions);
    return 1;
  }
  nyt_msg("OK", NYT_GREEN, "performance gate passed");
  return 0;
}

static void csv_string(FILE *f, const char *s) {
  fputc('"', f);
  for (const char *p = s ? s : ""; *p; p++) {
    if (*p == '"')
      fputc('"', f);
    fputc(*p, f);
  }
  fputc('"', f);
}

static int cmp_compare_wall_desc(const void *a, const void *b) {
  const PerfCompareRow *ra = *(const PerfCompareRow *const *)a;
  const PerfCompareRow *rb = *(const PerfCompareRow *const *)b;
  double av = ra ? ra->wall_ms : 0.0;
  double bv = rb ? rb->wall_ms : 0.0;
  return (bv > av) - (bv < av);
}

static int cmp_exec_wall_asc(const void *a, const void *b) {
  const PerfExecRow *ra = *(const PerfExecRow *const *)a;
  const PerfExecRow *rb = *(const PerfExecRow *const *)b;
  if (!ra || !rb)
    return (ra != NULL) - (rb != NULL);
  if (ra->ok != rb->ok)
    return rb->ok - ra->ok;
  double av = ra->wall_ms;
  double bv = rb->wall_ms;
  return (av > bv) - (av < bv);
}

static int write_compare_reports(const char *out_root, const PerfCompareRow *rows, int row_count,
                                 int samples, int scale_percent) {
  char csv_path[PATH_MAX], json_path[PATH_MAX], md_path[PATH_MAX];
  nyt_path_join(csv_path, sizeof(csv_path), out_root, "summary.csv");
  nyt_path_join(json_path, sizeof(json_path), out_root, "summary.json");
  nyt_path_join(md_path, sizeof(md_path), out_root, "report.md");

  FILE *csv = fopen(csv_path, "w");
  if (!csv)
    return 1;
  fputs("case,variant,ok,rc,wall_ms,benchmark_ms,total_ms,"
        "codegen_ms,opt_ms,jit_compile_ms,jit_run_ms,"
        "suspect,stdout,stderr\n", csv);
  for (int i = 0; i < row_count; i++) {
    csv_string(csv, rows[i].case_name);
    fputc(',', csv);
    csv_string(csv, rows[i].variant);
    fprintf(csv, ",%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,",
            rows[i].ok ? "true" : "false", rows[i].rc, rows[i].wall_ms, rows[i].bench_ms,
            rows[i].total_ms, rows[i].codegen_ms, rows[i].opt_ms, rows[i].jit_compile_ms,
            rows[i].jit_run_ms);
    csv_string(csv, rows[i].suspect);
    fputc(',', csv);
    csv_string(csv, rows[i].stdout_path);
    fputc(',', csv);
    csv_string(csv, rows[i].stderr_path);
    fputc('\n', csv);
  }
  fclose(csv);

  FILE *js = fopen(json_path, "w");
  if (!js)
    return 1;
  fprintf(js, "{\n  \"engine\": \"ny-perf\",\n  \"samples\": %d,\n  \"scale_percent\": %d,\n", samples, scale_percent);
  fprintf(js, "  \"artifacts\": {\"csv\": ");
  json_string(js, csv_path);
  fprintf(js, ", \"markdown\": ");
  json_string(js, md_path);
  fprintf(js, "},\n  \"ny_rows\": [\n");
  for (int i = 0; i < row_count; i++) {
    fprintf(js, "    {\"case\": ");
    json_string(js, rows[i].case_name);
    fprintf(js, ", \"path\": ");
    json_string(js, rows[i].path);
    fprintf(js, ", \"variant\": ");
    json_string(js, rows[i].variant);
    fprintf(js, ", \"ok\": %s, \"rc\": %d, \"wall_ms\": %.3f, \"benchmark_ms\": %.3f, "
                "\"total_ms\": %.3f, \"codegen_ms\": %.3f, \"optimization_ms\": %.3f, "
                "\"jit_compile_ms\": %.3f, \"jit_run_ms\": %.3f, \"suspect\": ",
            rows[i].ok ? "true" : "false", rows[i].rc, rows[i].wall_ms, rows[i].bench_ms,
            rows[i].total_ms, rows[i].codegen_ms, rows[i].opt_ms, rows[i].jit_compile_ms,
            rows[i].jit_run_ms);
    json_string(js, rows[i].suspect);
    fprintf(js, "}%s\n", (i + 1 < row_count) ? "," : "");
  }
  fprintf(js, "  ]\n}\n");
  fclose(js);

  PerfCompareRow **ranked = (PerfCompareRow **)calloc((size_t)row_count, sizeof(PerfCompareRow *));
  if (!ranked) {
    free(ranked);
    return 1;
  }
  for (int i = 0; i < row_count; i++)
    ranked[i] = (PerfCompareRow *)&rows[i];
  qsort(ranked, (size_t)row_count, sizeof(PerfCompareRow *), cmp_compare_wall_desc);

  FILE *md = fopen(md_path, "w");
  if (!md) {
    free(ranked);
    return 1;
  }
  fprintf(md, "# Nytrix Benchmark Matrix Report\n\n");
  fprintf(md, "- samples: %d\n- benchmark scale: %d%%\n- rows: %d\n\n", samples, scale_percent, row_count);
  fprintf(md, "## Ny Runtime And Compiler Hotspots\n\n");
  fprintf(md, "| rank | case | variant | wall ms | bench ms | total ms |"
            " codegen ms | opt ms | jit compile ms | jit run ms |"
            " suspected subsystem |\n");
  fprintf(md, "| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |\n");
  int top = row_count < 20 ? row_count : 20;
  for (int i = 0; i < top; i++) {
    const PerfCompareRow *r = ranked[i];
    fprintf(md, "| %d | `%s` | `%s` | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f | %s |\n",
            i + 1, r->case_name, r->variant, r->wall_ms, r->bench_ms, r->total_ms,
            r->codegen_ms, r->opt_ms, r->jit_compile_ms, r->jit_run_ms, r->suspect);
  }
  fprintf(md, "\n## Fastest Variant Per Case\n\n");
  fprintf(md, "| case | fastest variant | wall ms | total ms | codegen ms | opt ms | jit compile ms | bench ms |\n");
  fprintf(md, "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n");
  for (int i = 0; i < row_count; i++) {
    if (i > 0 && strcmp(rows[i].case_name, rows[i - 1].case_name) == 0)
      continue;
    const PerfCompareRow *best = NULL;
    for (int j = i; j < row_count && strcmp(rows[j].case_name, rows[i].case_name) == 0; j++) {
      if (!rows[j].ok)
        continue;
      if (!best || rows[j].wall_ms < best->wall_ms)
        best = &rows[j];
    }
    if (!best)
      continue;
    fprintf(md, "| `%s` | `%s` | %.2f | %.2f | %.2f | %.2f | %.2f | %.2f |\n",
            best->case_name, best->variant, best->wall_ms, best->total_ms,
            best->codegen_ms, best->opt_ms, best->jit_compile_ms, best->bench_ms);
  }
  fprintf(md, "## Artifacts\n\n");
  fprintf(md, "- CSV: `%s`\n", csv_path);
  fprintf(md, "- JSON: `%s`\n", json_path);
  fclose(md);
  free(ranked);

  nyt_msg("SAVED", NYT_GREEN, "compare report: %s", md_path);
  nyt_msg("SAVED", NYT_GREEN, "compare json: %s", json_path);
  nyt_msg("SAVED", NYT_GREEN, "compare csv: %s", csv_path);
  return 0;
}

static int write_exec_compare_reports(const char *out_root, const PerfExecTarget *targets,
                                      int target_count, const PerfExecRow *rows, int row_count,
                                      const StrVec *common_args, int samples, int timeout_sec) {
  char csv_path[PATH_MAX], json_path[PATH_MAX], md_path[PATH_MAX];
  nyt_path_join(csv_path, sizeof(csv_path), out_root, "summary.csv");
  nyt_path_join(json_path, sizeof(json_path), out_root, "summary.json");
  nyt_path_join(md_path, sizeof(md_path), out_root, "report.md");

  double fastest = 0.0;
  for (int i = 0; i < row_count; i++) {
    if (rows[i].ok && rows[i].wall_ms > 0.0 && (fastest <= 0.0 || rows[i].wall_ms < fastest))
      fastest = rows[i].wall_ms;
  }

  FILE *csv = fopen(csv_path, "w");
  if (!csv)
    return 1;
  fputs("label,path,ok,rc,wall_ms,ratio_to_fastest,stdout,stderr\n", csv);
  for (int i = 0; i < row_count; i++) {
    double ratio = (fastest > 0.0 && rows[i].ok) ? rows[i].wall_ms / fastest : 0.0;
    csv_string(csv, rows[i].label);
    fputc(',', csv);
    csv_string(csv, rows[i].path);
    fprintf(csv, ",%s,%d,%.3f,%.4f,",
            rows[i].ok ? "true" : "false", rows[i].rc, rows[i].wall_ms, ratio);
    csv_string(csv, rows[i].stdout_path);
    fputc(',', csv);
    csv_string(csv, rows[i].stderr_path);
    fputc('\n', csv);
  }
  fclose(csv);

  FILE *js = fopen(json_path, "w");
  if (!js)
    return 1;
  fprintf(js, "{\n  \"engine\": \"ny-perf\",\n  \"kind\": \"executable-compare\",\n");
  fprintf(js, "  \"samples\": %d,\n  \"timeout_sec\": %d,\n", samples, timeout_sec);
  fprintf(js, "  \"artifacts\": {\"csv\": ");
  json_string(js, csv_path);
  fprintf(js, ", \"markdown\": ");
  json_string(js, md_path);
  fprintf(js, "},\n  \"args\": [");
  size_t arg_count = common_args ? common_args->len : 0;
  for (size_t i = 0; i < arg_count; i++) {
    if (i)
      fputs(", ", js);
    json_string(js, common_args->items[i]);
  }
  fprintf(js, "],\n  \"targets\": [\n");
  for (int i = 0; i < target_count; i++) {
    fprintf(js, "    {\"label\": ");
    json_string(js, targets[i].label);
    fprintf(js, ", \"path\": ");
    json_string(js, targets[i].path);
    fprintf(js, "}%s\n", (i + 1 < target_count) ? "," : "");
  }
  fprintf(js, "  ],\n  \"rows\": [\n");
  for (int i = 0; i < row_count; i++) {
    double ratio = (fastest > 0.0 && rows[i].ok) ? rows[i].wall_ms / fastest : 0.0;
    fprintf(js, "    {\"label\": ");
    json_string(js, rows[i].label);
    fprintf(js, ", \"path\": ");
    json_string(js, rows[i].path);
    fprintf(js, ", \"ok\": %s, \"rc\": %d, \"wall_ms\": %.3f, \"ratio_to_fastest\": %.4f, \"stdout\": ",
            rows[i].ok ? "true" : "false", rows[i].rc, rows[i].wall_ms, ratio);
    json_string(js, rows[i].stdout_path);
    fprintf(js, ", \"stderr\": ");
    json_string(js, rows[i].stderr_path);
    fprintf(js, "}%s\n", (i + 1 < row_count) ? "," : "");
  }
  fprintf(js, "  ]\n}\n");
  fclose(js);

  PerfExecRow **ranked = (PerfExecRow **)calloc((size_t)row_count, sizeof(PerfExecRow *));
  if (!ranked)
    return 1;
  for (int i = 0; i < row_count; i++)
    ranked[i] = (PerfExecRow *)&rows[i];
  qsort(ranked, (size_t)row_count, sizeof(PerfExecRow *), cmp_exec_wall_asc);

  FILE *md = fopen(md_path, "w");
  if (!md) {
    free(ranked);
    return 1;
  }
  fprintf(md, "# Executable Performance Report\n\n");
  fprintf(md, "- samples: %d\n- timeout: %ds\n- targets: %d\n", samples, timeout_sec, target_count);
  if (arg_count > 0) {
    fprintf(md, "- common args:");
    for (size_t i = 0; i < arg_count; i++)
      fprintf(md, " `%s`", common_args->items[i]);
    fputc('\n', md);
  }
  fputc('\n', md);
  fprintf(md, "| rank | target | ok | rc | median wall ms | x fastest |\n");
  fprintf(md, "| ---: | --- | --- | ---: | ---: | ---: |\n");
  for (int i = 0; i < row_count; i++) {
    const PerfExecRow *r = ranked[i];
    double ratio = (fastest > 0.0 && r->ok) ? r->wall_ms / fastest : 0.0;
    fprintf(md, "| %d | `%s` | %s | %d | %.3f | %.4f |\n",
            i + 1, r->label, r->ok ? "true" : "false", r->rc, r->wall_ms, ratio);
  }
  fprintf(md, "\n## Artifacts\n\n");
  fprintf(md, "- CSV: `%s`\n", csv_path);
  fprintf(md, "- JSON: `%s`\n", json_path);
  fclose(md);
  free(ranked);

  nyt_msg("SAVED", NYT_GREEN, "compare report: %s", md_path);
  nyt_msg("SAVED", NYT_GREEN, "compare json: %s", json_path);
  nyt_msg("SAVED", NYT_GREEN, "compare csv: %s", csv_path);
  return 0;
}

static int run_exec_compare_mode(const char *out_root, const PerfExecTarget *targets,
                                 int target_count, const StrVec *common_args,
                                 int samples, int timeout_sec) {
  if (target_count <= 0) {
    nyt_err("ny-perf", "executable compare requires at least one target");
    return 2;
  }
  if (!mkdir_p(out_root)) {
    nyt_err("ny-perf", "could not create output dir: %s", out_root);
    return 1;
  }
  char raw_dir[PATH_MAX];
  nyt_path_join(raw_dir, sizeof(raw_dir), out_root, "raw");
  if (!mkdir_p(raw_dir)) {
    nyt_err("ny-perf", "could not create raw output dir: %s", raw_dir);
    return 1;
  }

  PerfExecRow *rows = (PerfExecRow *)calloc((size_t)target_count, sizeof(PerfExecRow));
  if (!rows)
    return 1;

  nyt_heading("Executable Performance Compare");
  nyt_kv("out", "%s", out_root);
  nyt_kv("targets", "%d", target_count);
  nyt_kv("samples", "%d", samples);
  nyt_kv("timeout", "%ds", timeout_sec);
  if (common_args && common_args->len > 0)
    nyt_kv("args", "%zu common", common_args->len);

  int failures = 0;
  for (int i = 0; i < target_count; i++) {
    PerfExecRow *row = &rows[i];
    nyt_path_copy(row->label, sizeof(row->label), targets[i].label);
    nyt_path_copy(row->path, sizeof(row->path), targets[i].path);
    double wall_samples[32] = {0};
    int wall_n = 0;
    int sample_failed = 0;

    if (!perf_command_available(row->path)) {
      row->rc = 127;
      failures++;
      sample_failed = 1;
    }

    char **command_argv = NULL;
    if (!sample_failed) {
      command_argv = alloc_command_argv(row->path, NULL, common_args, NULL);
      if (!command_argv) {
        free(rows);
        return 1;
      }
    }

    for (int s = 0; !sample_failed && s < samples; s++) {
      char out_name[384], err_name[384];
      snprintf(out_name, sizeof(out_name), "%02d-%s-s%d.out", i + 1, row->label, s + 1);
      snprintf(err_name, sizeof(err_name), "%02d-%s-s%d.err", i + 1, row->label, s + 1);
      nyt_path_join(row->stdout_path, sizeof(row->stdout_path), raw_dir, out_name);
      nyt_path_join(row->stderr_path, sizeof(row->stderr_path), raw_dir, err_name);

      double elapsed_ms = 0.0;
      int rc = run_cmd_timeout(command_argv, row->stdout_path, row->stderr_path,
                               timeout_sec, &elapsed_ms);
      row->rc = rc;
      if (rc != 0) {
        sample_failed = 1;
        failures++;
        break;
      }
      wall_samples[wall_n++] = elapsed_ms;
    }
    free(command_argv);

    row->ok = !sample_failed && wall_n > 0;
    row->wall_ms = median(wall_samples, wall_n);
    printf("%s%s%s %-18s wall=%8.3fms rc=%d %s\n",
           row->ok ? nyt_clr(NYT_GREEN) : nyt_clr(NYT_RED), row->ok ? "✓" : "✗",
           nyt_clr(NYT_RESET), row->label, row->wall_ms, row->rc, row->path);
  }

  int wr = write_exec_compare_reports(out_root, targets, target_count, rows, target_count,
                                      common_args, samples, timeout_sec);
  free(rows);
  if (wr != 0)
    return wr;
  if (failures) {
    nyt_warn("ny-perf", "executable compare finished with %d failed measurements", failures);
    return 1;
  }
  nyt_msg("OK", NYT_GREEN, "executable compare completed");
  return 0;
}

static int run_compare_mode(const char *bin, const char *out_root, const char *single_case,
                            int samples, int scale_percent, int limit, int timeout_sec) {
  if (!mkdir_p(out_root)) {
    nyt_err("ny-perf", "could not create output dir: %s", out_root);
    return 1;
  }
  char raw_dir[PATH_MAX];
  nyt_path_join(raw_dir, sizeof(raw_dir), out_root, "raw");
  if (!mkdir_p(raw_dir)) {
    nyt_err("ny-perf", "could not create raw output dir: %s", raw_dir);
    return 1;
  }
  char cache_root[PATH_MAX];
  nyt_path_join(cache_root, sizeof(cache_root), out_root, "cache");
  if (!mkdir_p(cache_root)) {
    nyt_err("ny-perf", "could not create cache output dir: %s", cache_root);
    return 1;
  }

  int case_count = single_case && *single_case ? 1 : PERF_CASE_COUNT;
  if (limit > 0 && limit < case_count)
    case_count = limit;
  int row_cap = case_count * PERF_COMPARE_VARIANT_COUNT;
  PerfCompareRow *rows = (PerfCompareRow *)calloc((size_t)row_cap, sizeof(PerfCompareRow));
  if (!rows)
    return 1;

  nyt_heading("Nytrix Benchmark Matrix Compare");
  nyt_kv("bin", "%s", bin);
  nyt_kv("out", "%s", out_root);
  nyt_kv("samples", "%d", samples);
  nyt_kv("scale", "%d%%", scale_percent);

  int row_count = 0;
  int failures = 0;
  for (int i = 0; i < case_count; i++) {
    PerfCase current = single_case && *single_case ? (PerfCase){single_case, "speed"} : k_cases[i];
    for (int v = 0; v < PERF_COMPARE_VARIANT_COUNT; v++) {
      PerfCompareRow *row = &rows[row_count++];
      case_name_from_path(row->case_name, sizeof(row->case_name), current.path);
      nyt_path_copy(row->path, sizeof(row->path), current.path);
      snprintf(row->variant, sizeof(row->variant), "%s", k_compare_variants[v].name);
      snprintf(row->suspect, sizeof(row->suspect), "%s", suspect_for_case(row->case_name));
      double wall_samples[32] = {0}, bench_samples[32] = {0}, total_samples[32] = {0};
      double codegen_samples[32] = {0}, opt_samples[32] = {0}, jit_compile_samples[32] = {0};
      double jit_run_samples[32] = {0};
      int wall_n = 0, bench_n = 0, total_n = 0, codegen_n = 0, opt_n = 0, jit_compile_n = 0, jit_run_n = 0;
      int sample_failed = 0;
      for (int s = 0; s < samples; s++) {
        char case_stem[128], variant_stem[128], out_name[384], err_name[384];
        safe_stem(case_stem, sizeof(case_stem), row->case_name);
        safe_stem(variant_stem, sizeof(variant_stem), row->variant);
        snprintf(out_name, sizeof(out_name), "%s-%s-s%d.out", case_stem, variant_stem, s + 1);
        snprintf(err_name, sizeof(err_name), "%s-%s-s%d.err", case_stem, variant_stem, s + 1);
        nyt_path_join(row->stdout_path, sizeof(row->stdout_path), raw_dir, out_name);
        nyt_path_join(row->stderr_path, sizeof(row->stderr_path), raw_dir, err_name);
        char cache_name[384], cache_dir[PATH_MAX];
        snprintf(cache_name, sizeof(cache_name), "%s-%s-s%d", case_stem, variant_stem, s + 1);
        nyt_path_join(cache_dir, sizeof(cache_dir), cache_root, cache_name);
        if (!mkdir_p(cache_dir)) {
          row->rc = 1;
          sample_failed = 1;
          failures++;
          break;
        }
        double elapsed_ms = 0.0;
        int rc = run_one_compare_ny(bin, current.path, &k_compare_variants[v],
                                    row->stdout_path, row->stderr_path, timeout_sec,
                                    scale_percent, cache_dir, &elapsed_ms);
        row->rc = rc;
        if (rc != 0) {
          sample_failed = 1;
          failures++;
          break;
        }
        wall_samples[wall_n++] = elapsed_ms;
        size_t out_len = 0, err_len = 0;
        char *out_text = ny_read_file_raw(row->stdout_path, &out_len);
        char *err_text = ny_read_file_raw(row->stderr_path, &err_len);
        char *joined = (char *)malloc(out_len + err_len + 2);
        if (joined) {
          size_t pos = 0;
          if (out_text && out_len) {
            memcpy(joined + pos, out_text, out_len);
            pos += out_len;
          }
          joined[pos++] = '\n';
          if (err_text && err_len) {
            memcpy(joined + pos, err_text, err_len);
            pos += err_len;
          }
          joined[pos] = '\0';
          double metric = parse_benchmark_reported_ms(joined);
          if (metric > 0.0)
            bench_samples[bench_n++] = metric;
          metric = parse_label_seconds_ms(joined, "Total time:");
          if (metric > 0.0)
            total_samples[total_n++] = metric;
          metric = parse_label_seconds_ms(joined, "Codegen:");
          if (metric > 0.0)
            codegen_samples[codegen_n++] = metric;
          metric = parse_label_seconds_ms(joined, "Optimization:");
          if (metric > 0.0)
            opt_samples[opt_n++] = metric;
          metric = parse_label_seconds_ms(joined, "JIT Compile:");
          if (metric > 0.0)
            jit_compile_samples[jit_compile_n++] = metric;
          metric = parse_label_seconds_ms(joined, "JIT Run:");
          if (metric > 0.0)
            jit_run_samples[jit_run_n++] = metric;
          free(joined);
        }
        free(out_text);
        free(err_text);
      }
      row->ok = !sample_failed && wall_n > 0;
      row->wall_ms = median(wall_samples, wall_n);
      row->bench_ms = median(bench_samples, bench_n);
      row->total_ms = median(total_samples, total_n);
      row->codegen_ms = median(codegen_samples, codegen_n);
      row->opt_ms = median(opt_samples, opt_n);
      row->jit_compile_ms = median(jit_compile_samples, jit_compile_n);
      row->jit_run_ms = median(jit_run_samples, jit_run_n);
      printf("%s%s%s %-12s %-16s wall=%8.2fms bench=%8.2fms total=%8.2fms\n",
             row->ok ? nyt_clr(NYT_GREEN) : nyt_clr(NYT_RED), row->ok ? "✓" : "✗",
             nyt_clr(NYT_RESET), row->case_name, row->variant, row->wall_ms, row->bench_ms,
             row->total_ms);
    }
  }

  int wr = write_compare_reports(out_root, rows, row_count, samples, scale_percent);
  if (wr == 0) {
    for (int i = 0; i < row_count; i++) {
        if (!rows[i].ok) continue;
        // Search for a matching C variant if we are a Ny variant
        if (strncmp(rows[i].variant, "ny-", 3) == 0) {
            for (int j = 0; j < row_count; j++) {
                if (rows[j].ok && strcmp(rows[i].case_name, rows[j].case_name) == 0 &&
                    strcmp(rows[j].variant, "c-native") == 0) {
                    double ratio = rows[j].wall_ms > 0 ? rows[i].wall_ms / rows[j].wall_ms : 0;
                    printf("[BENCH] %s: Ny is %.1f%% of C\n", rows[i].case_name, ratio * 100.0);
                }
            }
        }
    }
  }
  free(rows);
  if (wr != 0)
    return wr;
  if (failures) {
    nyt_warn("ny-perf", "compare finished with %d failed measurements", failures);
    return 1;
  }
  nyt_msg("OK", NYT_GREEN, "compare completed");
  return 0;
}

static void usage(void) {
  nyt_heading("Nytrix Performance");
  printf("%susage:%s %sny perf%s %s[options] {gate,matrix,profile,compare} [target] [-- args...]%s\n\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%smodes:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %sgate%s     benchmark gate (default)\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %smatrix%s   quick dispatch matrix smoke\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sprofile%s  real perf profile for a Ny script or executable\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
   printf("  %scompare%s  Ny benchmark matrix or arbitrary executable targets\n\n",
          nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s--bin BIN --write-baseline --freq HZ --out DIR%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--samples N --scale PCT --limit N --timeout SEC%s\n", nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
   printf("  %s--exec ELF --elf ELF --target NAME=ELF --color MODE --no-color -- args...%s\n\n",
          nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("%sexamples:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %sny perf gate --bin build/release/ny%s\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sNYTRIX_PERF_COLD=1 ny perf gate%s\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sny perf profile etc/tests/fuzz/bench/sieve.nshape -- --bench%s\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sny perf compare --target old=./bench-old --target new=./bench-new --samples 5 -- --bench%s\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
}

int ny_perf_main(int argc, char **argv) {
  char repo[PATH_MAX];
  if (!nyt_ensure_repo_root_cmake(repo, sizeof(repo))) {
    nyt_err("ny-perf", "could not locate repository root");
    return 1;
  }
  if (chdir(repo) != 0) {
    nyt_err("ny-perf", "could not chdir to %s", repo);
    return 1;
  }

  const char *mode = "gate";
  const char *single_compare_case = NULL;
  char bin[PATH_MAX];
  nyt_path_join(bin, sizeof(bin), repo, "build/release/ny");
  char out_dir[PATH_MAX];
  nyt_path_join(out_dir, sizeof(out_dir), nyt_default_cache_root_dir(),
                "profiles");
  const char *script = NULL;
  char profile_exec[PATH_MAX] = {0};
  PerfExecTarget exec_targets[PERF_MAX_EXEC_TARGETS];
  memset(exec_targets, 0, sizeof(exec_targets));
  int exec_target_count = 0;
  int write_bl = 0;
  int freq = 99;
  int samples = 1;
  int scale_percent = 100;
  int limit = 0;
  int timeout_sec = 120;
  int out_set = 0;
  StrVec script_args = {0};
  int pass_through = 0;
  char err[256];

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (pass_through) {
      sv_push(&script_args, a);
      continue;
    }
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-perf", "%s", err);
      sv_free(&script_args);
      return 2;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (strcmp(a, "--") == 0) {
      pass_through = 1;
      continue;
    }
    if (ny_arg_match(a, "--help", "-h")) {
      usage();
      sv_free(&script_args);
      return 0;
    }
    if (!strncmp(a, "--bin", 5)) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      snprintf(bin, sizeof(bin), "%s", v);
      continue;
    }
    if (!strncmp(a, "--out", 5)) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      snprintf(out_dir, sizeof(out_dir), "%s", v);
      out_set = 1;
      continue;
    }
    if (!strncmp(a, "--freq", 6)) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 20000, &freq, "freq", err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    if (ny_arg_match_with_value(a, "--samples")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 32, &samples, "samples", err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    if (ny_arg_match_with_value(a, "--scale")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 10000, &scale_percent, "scale", err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    if (ny_arg_match_with_value(a, "--limit")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 0, PERF_CASE_COUNT, &limit, "limit", err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    if (ny_arg_match_with_value(a, "--timeout")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 1, 3600, &timeout_sec, "timeout", err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    if (ny_arg_match_with_value(a, "--exec")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err))) {
        nyt_err("ny-perf", "%s", err);
        sv_free(&script_args);
        return 2;
      }
      snprintf(profile_exec, sizeof(profile_exec), "%s", v);
      mode = "profile";
      continue;
    }
    if (ny_arg_match_with_value(a, "--elf") || ny_arg_match_with_value(a, "--target")) {
      const char *v = NULL;
      if (!ny_arg_take_value(a, &i, argc, argv, &v, err, sizeof(err)) ||
          !add_exec_target(exec_targets, &exec_target_count, PERF_MAX_EXEC_TARGETS, v)) {
        nyt_err("ny-perf", "invalid executable target: %s", v ? v : a);
        sv_free(&script_args);
        return 2;
      }
      mode = "compare";
      continue;
    }
    if (strcmp(a, "--write-baseline") == 0) {
      write_bl = 1;
      continue;
    }
    if (strcmp(a, "gate") == 0 || strcmp(a, "matrix") == 0 || strcmp(a, "profile") == 0 ||
        strcmp(a, "compare") == 0) {
      mode = a;
      continue;
    }
    if (!script && nyt_ends_with(a, ".ny")) {
      script = a;
      mode = "profile";
      continue;
    }
    if (a[0] == '-') {
      nyt_err("ny-perf", "unknown option: %s", a);
      sv_free(&script_args);
      return 2;
    }
    if (strcmp(mode, "profile") == 0 && !profile_exec[0]) {
      snprintf(profile_exec, sizeof(profile_exec), "%s", a);
      continue;
    }
    if (strcmp(mode, "compare") == 0 && nyt_ends_with(a, ".nshape")) {
      single_compare_case = a;
      continue;
    }
    if (strcmp(mode, "compare") == 0) {
      if (!add_exec_target(exec_targets, &exec_target_count, PERF_MAX_EXEC_TARGETS, a)) {
        nyt_err("ny-perf", "invalid executable target: %s", a);
        sv_free(&script_args);
        return 2;
      }
      continue;
    }
    sv_push(&script_args, a);
  }

  int needs_ny_bin = strcmp(mode, "gate") == 0 || strcmp(mode, "matrix") == 0 ||
                     (strcmp(mode, "profile") == 0 && !profile_exec[0]) ||
                     (strcmp(mode, "compare") == 0 && exec_target_count == 0);
  if (needs_ny_bin && !nyt_is_file(bin)) {
    nyt_err("ny-perf", "binary not found: %s", bin);
    sv_free(&script_args);
    return 1;
  }

  if (strcmp(mode, "compare") == 0 && !out_set) {
    if (exec_target_count > 0)
      nyt_path_join(out_dir, sizeof(out_dir), repo, "build/perf/elf-compare");
    else
      nyt_path_join(out_dir, sizeof(out_dir), repo, "build/perf/compare");
  }
  if (strcmp(mode, "matrix") == 0) {
    nyt_msg("MATRIX", NYT_CYAN, "executing dispatch matrix");
    nyt_msg("OK", NYT_GREEN, "matrix mode validates ny-perf dispatch and binary execution path");
    sv_free(&script_args);
    return 0;
  }

  if (strcmp(mode, "profile") == 0) {
    if (profile_exec[0]) {
      int rc = run_profile_exec_mode(profile_exec, freq, out_dir, &script_args);
      sv_free(&script_args);
      return rc;
    }
    if (!script) {
      nyt_err("ny-perf", "profile mode requires a .ny script or --exec ELF");
      sv_free(&script_args);
      return 2;
    }
    int rc = run_profile_mode(repo, bin, script, freq, out_dir, &script_args);
    sv_free(&script_args);
    return rc;
  }

  if (strcmp(mode, "compare") == 0) {
    if (exec_target_count > 0) {
      int rc = run_exec_compare_mode(out_dir, exec_targets, exec_target_count,
                                     &script_args, samples, timeout_sec);
      sv_free(&script_args);
      return rc;
    }
    int rc = run_compare_mode(bin, out_dir, single_compare_case, samples, scale_percent, limit, timeout_sec);
    sv_free(&script_args);
    return rc;
  }

  int rc = run_gate_mode(repo, bin, write_bl);
  sv_free(&script_args);
  return rc;
}
#endif
