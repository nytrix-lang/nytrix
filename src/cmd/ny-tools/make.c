#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "make.h"
#include "args.h"
#include "repo.h"
#include "tool.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
  const char *cmds[64];
  int cmd_count;
  const char *unknown[512];
  int unknown_count;
  int had_unknown_nonflag;
  int jobs;
  int verbose;
  int help;
  int version;
  int profile;
  int is_debug;
} MakeArgs;

typedef struct {
  const char *name;
  const char *value;
  int overwrite;
} EnvPair;

static void apply_child_env(const EnvPair *envs, int env_count) {
  for (int i = 0; i < env_count; ++i) {
    if (envs[i].name && envs[i].value)
      ny_setenv(envs[i].name, envs[i].value, envs[i].overwrite);
  }
}

static int run_cmd_env(char *const argv[], const EnvPair *envs, int env_count) {
  fflush(stdout);
  fflush(stderr);
#ifdef _WIN32
  apply_child_env(envs, env_count);
  intptr_t rc = _spawnvp(_P_WAIT, argv[0], (const char *const *)argv);
  return rc < 0 ? 127 : (int)rc;
#else
  pid_t pid = fork();
  if (pid == 0) {
    apply_child_env(envs, env_count);
    execvp(argv[0], argv);
    _exit(127);
  }
  if (pid < 0)
    return 127;
  int st = 0;
  if (waitpid(pid, &st, 0) < 0)
    return 127;
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  if (WIFSIGNALED(st))
    return 128 + WTERMSIG(st);
  return 1;
#endif
}

static int run_cmd(char *const argv[]) { return run_cmd_env(argv, NULL, 0); }

static int make_lstat(const char *path, struct stat *st) {
#ifdef _WIN32
  return stat(path, st);
#else
  return lstat(path, st);
#endif
}

static int make_mkdir(const char *path) {
#ifdef _WIN32
  return mkdir(path);
#else
  return mkdir(path, 0777);
#endif
}

static int build_line_progress(const char *line, const char **rest) {
  if (!line || line[0] != '[')
    return 0;
  const char *slash = strchr(line, '/');
  const char *end = strchr(line, ']');
  if (!slash || !end || slash > end)
    return 0;
  if (rest) {
    *rest = end + 1;
    while (**rest == ' ')
      (*rest)++;
  }
  return 1;
}

static const char *build_line_color(const char *rest) {
  if (!rest)
    return NYT_RESET;
  if (strncmp(rest, "FAILED", 6) == 0 || strstr(rest, " error:") || strstr(rest, " Error:"))
    return NYT_RED;
  if (strncmp(rest, "Linking", 7) == 0)
    return NYT_GREEN;
  if (strncmp(rest, "Building", 8) == 0)
    return NYT_CYAN;
  if (strncmp(rest, "Bundling", 8) == 0 || strncmp(rest, "Generating", 10) == 0)
    return NYT_MAGENTA;
  if (strncmp(rest, "Re-checking", 11) == 0 || strncmp(rest, "ninja:", 6) == 0)
    return NYT_YELLOW;
  return NYT_RESET;
}

static void print_build_output_line(const char *line) {
  if (!line) {
    return;
  }
  char buf[8192];
  snprintf(buf, sizeof(buf), "%s", line);
  size_t n = strlen(buf);
  int had_newline = 0;
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
    buf[--n] = '\0';
    had_newline = 1;
  }
  if (!nyt_color_enabled()) {
    fputs(buf, stdout);
    if (had_newline)
      fputc('\n', stdout);
    return;
  }

  const char *rest = buf;
  if (build_line_progress(buf, &rest)) {
    const char *slash = strchr(buf, '/');
    const char *end = strchr(buf, ']');
    fprintf(stdout, "%s[%s%.*s%s/%s%.*s%s]%s ", nyt_clr(NYT_GRAY), nyt_clr(NYT_CYAN),
            (int)(slash - buf - 1), buf + 1, nyt_clr(NYT_GRAY), nyt_clr(NYT_BOLD),
            (int)(end - slash - 1), slash + 1, nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
  }
  fprintf(stdout, "%s%s%s", nyt_clr(build_line_color(rest)), rest, nyt_clr(NYT_RESET));
  if (had_newline)
    fputc('\n', stdout);
}

static int run_cmd_build_output(char *const argv[]) {
  fflush(stdout);
  fflush(stderr);
#ifdef _WIN32
  return run_cmd(argv);
#else
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return run_cmd(argv);

  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    execvp(argv[0], argv);
    _exit(127);
  }
  close(pipefd[1]);
  if (pid < 0) {
    close(pipefd[0]);
    return 127;
  }

  FILE *stream = fdopen(pipefd[0], "r");
  if (stream) {
    char line[8192];
    while (fgets(line, sizeof(line), stream))
      print_build_output_line(line);
    fclose(stream);
  } else {
    close(pipefd[0]);
  }

  int st = 0;
  if (waitpid(pid, &st, 0) < 0)
    return 127;
  if (WIFEXITED(st))
    return WEXITSTATUS(st);
  if (WIFSIGNALED(st))
    return 128 + WTERMSIG(st);
  return 1;
#endif
}

static void build_kind_dir(char *out, size_t out_sz, const char *root, const char *kind) {
  char rel[64];
  snprintf(rel, sizeof(rel), "build/%s", kind);
  nyt_path_join(out, out_sz, root, rel);
}

static int rm_rf(const char *path) {
  struct stat st;
  if (!path || make_lstat(path, &st) != 0)
    return 0;

  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path);
    if (!d)
      return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      char child[PATH_MAX];
      snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
      if (rm_rf(child) != 0) {
        closedir(d);
        return -1;
      }
    }
    closedir(d);
    return rmdir(path);
  }
  return unlink(path);
}

static int mkdir_p(const char *path) {
  if (!path || !*path)
    return 0;
  char tmp[PATH_MAX];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      if (make_mkdir(tmp) != 0 && errno != EEXIST)
        return 0;
      *p = '/';
    }
  }
  if (make_mkdir(tmp) != 0 && errno != EEXIST)
    return 0;
  return 1;
}

static double linux_mem_total_gib(void) {
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f)
    return 0.0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, "MemTotal:", 9) == 0) {
      long kib = 0;
      if (sscanf(line + 9, "%ld", &kib) == 1) {
        fclose(f);
        return (double)kib / (1024.0 * 1024.0);
      }
    }
  }
  fclose(f);
  return 0.0;
}

static int resolve_jobs(int requested, int *out_jobs, char *note, size_t note_sz) {
  if (requested > 0) {
    *out_jobs = requested;
    snprintf(note, note_sz, "user-specified");
    return 1;
  }
  long logical = ny_cpu_count();
  if (logical <= 0)
    logical = 1;
  double mem_gib = linux_mem_total_gib();
  int jobs = (int)(logical * 0.75);
  if (jobs < 1)
    jobs = 1;
  int mem_cap = mem_gib > 0.0 ? (int)(mem_gib / 1.5) : 999;
  if (mem_cap < 1)
    mem_cap = 1;
  if (jobs > mem_cap && mem_cap < 999) {
    jobs = mem_cap;
    snprintf(note, note_sz, "auto jobs=%d capped by RAM (%.1f GiB); override with -j or NYTRIX_BUILD_JOBS",
             jobs, mem_gib);
  } else {
    snprintf(note, note_sz,
             "auto jobs=%d using 75%% of %ld cores (RAM=%.1f GiB); override with -j or NYTRIX_BUILD_JOBS",
             jobs, logical, mem_gib);
  }
  const char *env_jobs = getenv("NYTRIX_BUILD_JOBS");
  if (env_jobs && *env_jobs) {
    int v = atoi(env_jobs);
    if (v > 0) {
      jobs = v;
      snprintf(note, note_sz, "jobs=%d from NYTRIX_BUILD_JOBS", jobs);
    }
  }
  *out_jobs = jobs;
  return 1;
}

static int is_known_command(const char *s) {
  static const char *cmds[] = {"all",   "bin",      "fmt",   "std",   "std_bc", "test", "repl",
                               "fuzz",  "docs",     "install", "uninstall", "clean", "debug",
                               "tidy",  "perf",     "gprof", "asan",  "ubsan", "optcheck",
                               "analyze", "check",  "fb",    "ny",    "run", "release"};
  for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    if (strcmp(s, cmds[i]) == 0)
      return 1;
  }
  return 0;
}

static int parse_args(int argc, char **argv, MakeArgs *o) {
  memset(o, 0, sizeof(*o));
  o->jobs = 0;
  char err[256];
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    int color_mode = -2;
    int color_idx = i;
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      nyt_err("ny-make", "%s", err);
      return 0;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (ny_arg_match(a, "--help", "-h")) {
      o->help = 1;
      continue;
    }
    if (strcmp(a, "--version") == 0) {
      o->version = 1;
      continue;
    }
    if (strcmp(a, "--profile") == 0) {
      o->profile = 1;
      continue;
    }
    if (ny_arg_match_with_value(a, "--jobs") || ny_arg_match(a, NULL, "-j")) {
      if (!ny_arg_take_int(a, &i, argc, argv, 0, 4096, &o->jobs, "jobs", err, sizeof(err))) {
        nyt_err("ny-make", "%s", err);
        return 0;
      }
      continue;
    }
    if (ny_arg_match(a, "--verbose", "-v")) {
      o->verbose = 1;
      continue;
    }
    if (strcmp(a, "ny") == 0) {
      o->cmds[o->cmd_count++] = "ny";
      for (int k = i + 1; k < argc; k++)
        o->unknown[o->unknown_count++] = argv[k];
      break;
    }
    if (is_known_command(a)) {
      if (strcmp(a, "debug") == 0) {
        o->is_debug = 1;
      } else if (strcmp(a, "release") == 0) {
        o->is_debug = 0;
      } else if (strcmp(a, "run") == 0) {
        o->cmds[o->cmd_count++] = "ny";
      } else {
        o->cmds[o->cmd_count++] = a;
      }
      continue;
    }
    o->unknown[o->unknown_count++] = a;
    if (a[0] != '-')
      o->had_unknown_nonflag = 1;
  }

  if (o->cmd_count == 0) {
    if (o->unknown_count > 0 && !o->had_unknown_nonflag)
      o->cmds[o->cmd_count++] = "ny";
    else if (o->unknown_count == 0)
      o->cmds[o->cmd_count++] = "all";
  }
  return 1;
}

static void print_help(void) {
  nyt_heading("Nytrix Build Tool");
  printf("%susage:%s %sny make%s %s[commands...] [options]%s\n",
         nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET),
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("\n%scommands:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %sall%s        configure, build ny/std/tools\n", nyt_clr(NYT_CYAN),
         nyt_clr(NYT_RESET));
  printf("  %sbin%s        build the ny executable\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sfmt/check%s  run ny fmt / parse checks\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %stest%s       run native test matrix\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sdocs%s       build documentation portal\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sperf%s       run performance gates\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sinstall%s    install ny and ny-lsp; tools are available as ny <tool>\n",
         nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  %sclean%s      remove build outputs\n", nyt_clr(NYT_CYAN), nyt_clr(NYT_RESET));
  printf("  more: std, std_bc, repl, run, fuzz, tidy, gprof, asan, ubsan, optcheck, analyze, fb, ny\n");
  printf("\n%soptions:%s\n", nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET));
  printf("  %s-j, --jobs N%s                  parallel build jobs\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s-v, --verbose%s                 print subcommands\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s-h, --help%s                    show this help\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s--version%s                     print version\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s--profile%s                     enable profile-oriented build flags\n",
         nyt_clr(NYT_GREEN), nyt_clr(NYT_RESET));
  printf("  %s--color {auto,always,never}%s   control colored output\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
  printf("  %s--no-color%s                    disable colored output\n", nyt_clr(NYT_GREEN),
         nyt_clr(NYT_RESET));
}

static int ensure_cmake_configure(const char *root, const char *kind) {
  char bdir[PATH_MAX];
  build_kind_dir(bdir, sizeof(bdir), root, kind);
  if (!mkdir_p(bdir)) {
    nyt_err("ny-make", "failed to create build dir: %s", bdir);
    return 1;
  }
  char cache[PATH_MAX];
  nyt_path_join(cache, sizeof(cache), bdir, "CMakeCache.txt");
  if (nyt_is_file(cache))
    return 0;

  nyt_msg("CONFIG", NYT_CYAN, "cmake configure (%s)", kind);
  char cfg[16];
  snprintf(cfg, sizeof(cfg), "%s", strcmp(kind, "debug") == 0 ? "Debug" : "Release");
  char *argv[] = {"cmake", "-S", (char *)root, "-B", bdir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE", cfg, NULL};
  int rc = run_cmd_build_output(argv);
  if (rc != 0)
    return rc;
  nyt_msg("OK", NYT_GREEN, "cmake (%s) complete", kind);
  return 0;
}

static int cmake_build(const char *root, const char *kind, const char **targets, int target_count, int jobs) {
  char bdir[PATH_MAX];
  build_kind_dir(bdir, sizeof(bdir), root, kind);
  if (ensure_cmake_configure(root, kind) != 0)
    return 1;
  printf("%s%sBUILD%s build %s:", nyt_clr(NYT_CYAN), nyt_clr(NYT_BOLD), nyt_clr(NYT_RESET), kind);
  for (int i = 0; i < target_count; i++)
    printf(" %s", targets[i]);
  printf("\n");
  char jobs_s[32];
  snprintf(jobs_s, sizeof(jobs_s), "%d", jobs > 0 ? jobs : 1);
  char *argv[128];
  int k = 0;
  argv[k++] = "cmake";
  argv[k++] = "--build";
  argv[k++] = bdir;
  argv[k++] = "--target";
  for (int i = 0; i < target_count; i++)
    argv[k++] = (char *)targets[i];
  argv[k++] = "-j";
  argv[k++] = jobs_s;
  argv[k] = NULL;
  int rc = run_cmd_build_output(argv);
  if (rc == 0) {
    nyt_msg("OK", NYT_GREEN, "build %s complete", kind);
  }
  return rc;
}

#ifndef _WIN32
static int cmake_cache_value(const char *bdir, const char *key, char *out, size_t out_sz) {
  if (!bdir || !key || !out || out_sz == 0)
    return 0;
  out[0] = '\0';
  char cache[PATH_MAX];
  nyt_path_join(cache, sizeof(cache), bdir, "CMakeCache.txt");
  FILE *f = fopen(cache, "r");
  if (!f)
    return 0;
  char line[PATH_MAX * 2];
  size_t key_len = strlen(key);
  int ok = 0;
  while (fgets(line, sizeof(line), f)) {
    if (strncmp(line, key, key_len) != 0 || line[key_len] != ':')
      continue;
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    eq++;
    eq[strcspn(eq, "\r\n")] = '\0';
    snprintf(out, out_sz, "%s", eq);
    ok = 1;
    break;
  }
  fclose(f);
  return ok;
}

static int path_tree_writable(const char *path) {
  if (!path || !*path)
    return 0;
  char cur[PATH_MAX];
  snprintf(cur, sizeof(cur), "%s", path);
  for (;;) {
    struct stat st;
    if (stat(cur, &st) == 0)
      return access(cur, W_OK | X_OK) == 0;
    char *slash = strrchr(cur, '/');
    if (!slash)
      return 0;
    if (slash == cur)
      slash[1] = '\0';
    else
      *slash = '\0';
    if (strcmp(cur, "/") == 0)
      return access(cur, W_OK | X_OK) == 0;
  }
}

static int install_prefix_writable(const char *prefix) {
  const char *p = (prefix && *prefix) ? prefix : "/usr/local";
  const char *leaves[] = {"bin", "lib", "share"};
  for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
    char dest[PATH_MAX];
    nyt_path_join(dest, sizeof(dest), p, leaves[i]);
    if (!path_tree_writable(dest))
      return 0;
  }
  return 1;
}
#endif

static int resolve_tool_bin(const char *root, const char *kind, const char *name, char *out, size_t out_sz) {
  if (out && out_sz)
    out[0] = '\0';
  char p1[PATH_MAX], p2[PATH_MAX];
  snprintf(p1, sizeof(p1), "%s/build/%s/%s", root, kind, name);
  snprintf(p2, sizeof(p2), "%s/build/release/%s", root, name);
  if (ny_access(p1, X_OK) == 0) {
    snprintf(out, out_sz, "%s", p1);
    return 1;
  }
  if (ny_access(p2, X_OK) == 0) {
    snprintf(out, out_sz, "%s", p2);
    return 1;
  }
  return 0;
}

static const char *unified_tool_name(const char *name) {
  if (!name)
    return "";
  if (strcmp(name, "ny-fmt") == 0)
    return "fmt";
  if (strcmp(name, "ny-perf") == 0)
    return "perf";
  if (strcmp(name, "ny-test") == 0)
    return "test";
  if (strcmp(name, "ny-doc") == 0)
    return "doc";
  if (strcmp(name, "ny-make") == 0)
    return "make";
  return name;
}

static void tool_launch_path(const char *root, const char *bin, char *out, size_t out_sz) {
  if (!root || !*root || !bin || !*bin) {
    snprintf(out, out_sz, "%s", bin ? bin : "");
    return;
  }
  size_t root_len = strlen(root);
  if (strncmp(bin, root, root_len) == 0 && (bin[root_len] == '/' || bin[root_len] == '\0')) {
    snprintf(out, out_sz, ".%s", bin + root_len);
    return;
  }
  snprintf(out, out_sz, "%s", bin);
}

static int run_ny_tool(const char *root, const char *kind, const char *name, const char *fixed_arg,
                       const char **unknown, int unknown_count) {
  char bin[PATH_MAX];
  int unified = 0;
  if (!resolve_tool_bin(root, kind, name, bin, sizeof(bin))) {
    if (strcmp(name, "ny") != 0 && resolve_tool_bin(root, kind, "ny", bin, sizeof(bin)))
      unified = 1;
  }
  if (!bin[0]) {
    nyt_err("ny-make", "tool not found: %s", name);
    return 1;
  }
  char launch[PATH_MAX];
  tool_launch_path(root, bin, launch, sizeof(launch));
  char *argv[640];
  int k = 0;
  argv[k++] = launch;
  if (unified)
    argv[k++] = (char *)unified_tool_name(name);
  if (fixed_arg && *fixed_arg)
    argv[k++] = (char *)fixed_arg;
  for (int i = 0; i < unknown_count; i++)
    argv[k++] = (char *)unknown[i];
  argv[k] = NULL;
  EnvPair envs[6];
  int envc = 0;
  if (strcmp(name, "ny") == 0) {
    int interactive = (fixed_arg && strcmp(fixed_arg, "-i") == 0) ||
                      (unknown_count > 0 && strcmp(unknown[0], "-i") == 0);
    if (!interactive && nyt_env_truthy("NYTRIX_MAKE_USE_PREBUILT_STD")) {
      char bdir[PATH_MAX], std_path[PATH_MAX];
      build_kind_dir(bdir, sizeof(bdir), root, kind);
      nyt_path_join(std_path, sizeof(std_path), bdir, "std.ny");
      if (ny_access(std_path, R_OK) == 0) {
        envs[envc++] = (EnvPair){"NYTRIX_STD_PATH", std_path, 0};
        envs[envc++] = (EnvPair){"NYTRIX_BUILD_STD_PATH", std_path, 0};
        envs[envc++] = (EnvPair){"NYTRIX_STD_PREBUILT", std_path, 0};
      }
    }
    envs[envc++] = (EnvPair){"NYTRIX_STD_CACHE", "1", 0};
    envs[envc++] = (EnvPair){"NYTRIX_STD_BC_CACHE_AUTO", "1", 0};
    envs[envc++] = (EnvPair){"NYTRIX_JIT_CACHE", "1", 0};
  }
  return run_cmd_env(argv, envs, envc);
}

static int run_test_tool(const char *root, const char *kind, int jobs, const char **unknown,
                         int unknown_count) {
  char ny_bin[PATH_MAX];
  if (!resolve_tool_bin(root, kind, "ny", ny_bin, sizeof(ny_bin))) {
    nyt_err("ny-make", "ny binary not found");
    return 1;
  }

  char trace_dir[PATH_MAX];
  nyt_path_join(trace_dir, sizeof(trace_dir), nyt_default_cache_root_dir(),
                "test-trace");
  (void)rm_rf(trace_dir);
  (void)mkdir_p(trace_dir);
  char profile_json[PATH_MAX];
  nyt_path_join(profile_json, sizeof(profile_json), trace_dir, "profile.json");
  ny_setenv("NYTRIX_TEST_TRACE_DIR", trace_dir, 1);
  ny_setenv("NYTRIX_TEST_PROFILE_JSON", profile_json, 1);
  ny_setenv("NYTRIX_TEST_INCLUDE_BENCHMARK", "1", 1);
  ny_setenv("NYTRIX_TEST_NATIVE", "1", 1);
  ny_setenv("NYTRIX_TEST_AOT_REUSE_NATIVE", "0", 1);
  ny_setenv("NYTRIX_TEST_BENCHMARK_NATIVE", "1", 1);
  ny_setenv("NYTRIX_TEST_RUNTIME_NATIVE", "1", 1);
  ny_setenv("NYTRIX_TEST_STD_NATIVE", "1", 1);
  ny_setenv("NYTRIX_TEST_BENCHMARK_REPL", "1", 1);
  ny_setenv("NYTRIX_TEST_RUNTIME_REPL", "1", 1);
  ny_setenv("NYTRIX_TEST_STD_REPL", "1", 1);
  int cold = nyt_env_truthy("NYTRIX_TEST_COLD");
  int exec_cache = nyt_env_truthy("NYTRIX_TEST_EXEC_CACHE");
  ny_setenv("NYTRIX_TEST_CACHE", cold ? "0" : "1", 1);
  ny_setenv("NYTRIX_TEST_NO_NATIVE_CACHE", (cold || !exec_cache) ? "1" : "0", 1);
  ny_setenv("NYTRIX_AOT_CACHE", (!cold && exec_cache) ? "1" : "0", 1);
  ny_setenv("NYTRIX_JIT_CACHE", (!cold && exec_cache) ? "1" : "0", 1);
  if (cold)
    ny_setenv("NYTRIX_STD_CACHE", "0", 1);
  else if (!getenv("NYTRIX_STD_CACHE"))
    ny_setenv("NYTRIX_STD_CACHE", "1", 1);

  nyt_msg("TEST", NYT_MAGENTA,
          "make test: full matrix with jit/repl/native and benchmarks; result_cache %s, "
          "exec_cache %s, std_cache %s (set NYTRIX_TEST_EXEC_CACHE=1 to enable binary caches)",
          cold ? "off" : "on", (!cold && exec_cache) ? "on" : "off",
          (getenv("NYTRIX_STD_CACHE") && strcmp(getenv("NYTRIX_STD_CACHE"), "0") == 0) ? "off" : "on");
  if (jobs > 0) {
    nyt_msg("RUN", NYT_CYAN, "tests: bin=ny jobs=%d timeout=auto", jobs);
  } else {
    nyt_msg("RUN", NYT_CYAN, "tests: bin=ny jobs=auto timeout=auto");
  }

  char jobs_s[32];
  snprintf(jobs_s, sizeof(jobs_s), "%d", jobs);
  char ny_test_bin[PATH_MAX];
  int unified_test = 0;
  if (!resolve_tool_bin(root, kind, "ny-test", ny_test_bin, sizeof(ny_test_bin))) {
    snprintf(ny_test_bin, sizeof(ny_test_bin), "%s", ny_bin);
    unified_test = 1;
  }
  char *argv[900];
  int k = 0;
  argv[k++] = ny_test_bin;
  if (unified_test)
    argv[k++] = "test";
  argv[k++] = "--bin";
  argv[k++] = ny_bin;
  argv[k++] = "--jobs";
  argv[k++] = jobs_s;
  for (int i = 0; i < unknown_count; i++)
    argv[k++] = (char *)unknown[i];
  argv[k] = NULL;
  return run_cmd(argv);
}

static int resolve_test_jobs(int cli_jobs) {
  const char *env_jobs = getenv("NYTRIX_TEST_JOBS");
  if (env_jobs && *env_jobs) {
    int v = atoi(env_jobs);
    if (v >= 0)
      return v;
  }
  if (cli_jobs > 0)
    return cli_jobs;
  long logical = ny_cpu_count();
  if (logical <= 0)
    logical = 1;
  int jobs = (int)(logical * 0.33);
  if (jobs < 1)
    jobs = 1;
  double mem_gib = linux_mem_total_gib();
  if (mem_gib > 0.0) {
    int mem_cap = (int)(mem_gib / 2.0);
    if (mem_cap < 1)
      mem_cap = 1;
    if (jobs > mem_cap)
      jobs = mem_cap;
  }
  if (jobs > 24)
    jobs = 24;
  return jobs;
}

static int run_self_subcommand(const char *root, const char *kind, const char *cmd, int jobs,
                               const char **unknown, int unknown_count) {
  char self_bin[PATH_MAX];
  int unified_make = 0;
  if (!resolve_tool_bin(root, kind, "ny-make", self_bin, sizeof(self_bin))) {
    if (!resolve_tool_bin(root, kind, "ny", self_bin, sizeof(self_bin))) {
      nyt_err("ny-make", "ny binary not found");
      return 1;
    }
    unified_make = 1;
  }
  char jobs_s[32];
  snprintf(jobs_s, sizeof(jobs_s), "%d", jobs);
  char *argv[900];
  int k = 0;
  argv[k++] = self_bin;
  if (unified_make)
    argv[k++] = "make";
  argv[k++] = (char *)cmd;
  if (jobs > 0) {
    argv[k++] = "-j";
    argv[k++] = jobs_s;
  }
  for (int i = 0; i < unknown_count; i++)
    argv[k++] = (char *)unknown[i];
  argv[k] = NULL;
  return run_cmd(argv);
}

static void append_env_flag(const char *name, const char *extra) {
  const char *cur = getenv(name);
  char merged[2048];
  if (cur && *cur)
    snprintf(merged, sizeof(merged), "%s %s", cur, extra);
  else
    snprintf(merged, sizeof(merged), "%s", extra);
  ny_setenv(name, merged, 1);
}

static int run_uninstall_manifest(const char *root, const char *kind) {
  char manifest[PATH_MAX];
  char bdir[PATH_MAX];
  build_kind_dir(bdir, sizeof(bdir), root, kind);
  nyt_path_join(manifest, sizeof(manifest), bdir, "install_manifest.txt");
  FILE *f = fopen(manifest, "r");
  if (!f) {
    nyt_err("ny-make", "install manifest not found: %s", manifest);
    return 1;
  }
  int removed = 0;
  int failed = 0;
  char line[PATH_MAX * 2];
  while (fgets(line, sizeof(line), f)) {
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    if (n == 0)
      continue;
    if (rm_rf(line) == 0)
      removed++;
    else
      failed++;
  }
  fclose(f);
  nyt_msg(failed ? "WARN" : "OK", failed ? NYT_YELLOW : NYT_GREEN, "uninstalled (%d removed, %d failed)",
          removed, failed);
  return failed ? 1 : 0;
}

static int cmd_needs_build(const char *cmd) {
  return strcmp(cmd, "clean") != 0 && strcmp(cmd, "uninstall") != 0;
}

static int run_sanitizer_tests(const char *root, const char *kind, const char *name,
                               const char *cflags, const char *ldflags, int jobs,
                               const char **unknown, int unknown_count) {
  append_env_flag("NYTRIX_HOST_CFLAGS", cflags);
  append_env_flag("NYTRIX_HOST_LDFLAGS", ldflags);
  ny_setenv("NYTRIX_SKIP_OPTIONAL_GATES", "1", 1);
  ny_setenv("NYTRIX_TEST_CACHE", "0", 1);
  char bdir[PATH_MAX];
  char build_name[64];
  snprintf(build_name, sizeof(build_name), "build/%s", name);
  nyt_path_join(bdir, sizeof(bdir), root, build_name);
  ny_setenv("BUILD_DIR", bdir, 1);
  return run_self_subcommand(root, kind, "test", jobs, unknown, unknown_count);
}

static const char *fmt_flag_for_cmd(const char *cmd) {
  if (strcmp(cmd, "analyze") == 0)
    return "--analyze";
  if (strcmp(cmd, "check") == 0)
    return "--check";
  if (strcmp(cmd, "tidy") == 0)
    return "--tidy";
  return NULL;
}

static void build_targets_for_cmd(const char *cmd, const char ***targets, int *target_count) {
  static const char *all[] = {"ny",      "std",    "ny-fmt", "ny-perf",
                              "ny-test", "ny-doc", "ny-make"};
  static const char *test[] = {"ny", "ny-test"};
  static const char *fmt[] = {"ny-fmt"};
  static const char *docs[] = {"ny", "std", "ny-doc"};
  static const char *std[] = {"std"};
  static const char *perf[] = {"ny", "ny-perf"};
  static const char *ny[] = {"ny"};
#ifdef _WIN32
  static const char *install[] = {"ny",      "ny-lsp",  "std",    "ny-fmt",
                                  "ny-perf", "ny-test", "ny-doc", "ny-make"};
#else
  static const char *install[] = {"ny",      "ny-lsp", "std",     "ny-fmt", "ny-perf",
                                  "ny-test", "ny-doc", "ny-make", "nytrixrt"};
#endif

  if (strcmp(cmd, "all") == 0 || strcmp(cmd, "bin") == 0) {
    *targets = all;
    *target_count = (int)(sizeof(all) / sizeof(all[0]));
  } else if (strcmp(cmd, "test") == 0) {
    *targets = test;
    *target_count = (int)(sizeof(test) / sizeof(test[0]));
  } else if (strcmp(cmd, "fmt") == 0 || strcmp(cmd, "analyze") == 0 ||
             strcmp(cmd, "check") == 0 || strcmp(cmd, "tidy") == 0) {
    *targets = fmt;
    *target_count = (int)(sizeof(fmt) / sizeof(fmt[0]));
  } else if (strcmp(cmd, "docs") == 0) {
    *targets = docs;
    *target_count = (int)(sizeof(docs) / sizeof(docs[0]));
  } else if (strcmp(cmd, "std") == 0 || strcmp(cmd, "std_bc") == 0) {
    *targets = std;
    *target_count = (int)(sizeof(std) / sizeof(std[0]));
  } else if (strcmp(cmd, "install") == 0) {
    *targets = install;
    *target_count = (int)(sizeof(install) / sizeof(install[0]));
  } else if (strcmp(cmd, "perf") == 0) {
    *targets = perf;
    *target_count = (int)(sizeof(perf) / sizeof(perf[0]));
  } else {
    *targets = ny;
    *target_count = (int)(sizeof(ny) / sizeof(ny[0]));
  }
}

int ny_make_main(int argc, char **argv) {
  MakeArgs a;
  if (!parse_args(argc, argv, &a))
    return 2;
  if (a.cmd_count == 0 && a.had_unknown_nonflag) {
    nyt_err("ny-make", "unknown command or target");
    for (int i = 0; i < a.unknown_count; i++)
      fprintf(stderr, "  %s\n", a.unknown[i]);
    nyt_warn("ny-make", "run `ny make --help` for supported commands");
    return 2;
  }
  if (a.help) {
    print_help();
    return 0;
  }
  if (a.cmd_count == 0) {
    print_help();
    return 0;
  }
  if (a.version) {
    puts("Nytrix Build Tool 0.1");
    return 0;
  }

  char root[PATH_MAX];
  if (!nyt_ensure_repo_root_cmake(root, sizeof(root))) {
    nyt_err("ny-make", "could not locate repository root");
    return 1;
  }
  if (chdir(root) != 0) {
    nyt_err("ny-make", "could not chdir to %s", root);
    return 1;
  }

  int jobs = 0;
  char jobs_note[256];
  resolve_jobs(a.jobs, &jobs, jobs_note, sizeof(jobs_note));
  if (jobs_note[0])
    nyt_msg("HOST", NYT_MAGENTA, "%s", jobs_note);

  const char *kind = a.is_debug ? "debug" : "release";

  for (int i = 0; i < a.cmd_count; i++) {
    const char *cmd = a.cmds[i];

    if (strcmp(cmd, "clean") == 0) {
      char bdir[PATH_MAX];
      nyt_path_join(bdir, sizeof(bdir), root, "build");
      (void)rm_rf(bdir);
      nyt_msg("CLEAN", NYT_MAGENTA, "removed %s", bdir);
      continue;
    }

    if (cmd_needs_build(cmd)) {
      const char **targets = NULL;
      int target_count = 0;
      build_targets_for_cmd(cmd, &targets, &target_count);
      int rc = cmake_build(root, kind, targets, target_count, jobs);
      if (rc != 0)
        return rc;
    }

    if (strcmp(cmd, "all") == 0 || strcmp(cmd, "bin") == 0) {
      continue;
    } else if (strcmp(cmd, "test") == 0) {
      int test_jobs = resolve_test_jobs(a.jobs);
      int rc = run_test_tool(root, kind, test_jobs, a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "fmt") == 0 || strcmp(cmd, "analyze") == 0 ||
               strcmp(cmd, "check") == 0 || strcmp(cmd, "tidy") == 0) {
      int rc = run_ny_tool(root, kind, "ny-fmt", fmt_flag_for_cmd(cmd), a.unknown,
                           a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "perf") == 0) {
      int rc = run_ny_tool(root, kind, "ny-perf", NULL, a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "docs") == 0) {
      char std_path[PATH_MAX];
      char bdir[PATH_MAX];
      build_kind_dir(bdir, sizeof(bdir), root, kind);
      nyt_path_join(std_path, sizeof(std_path), bdir, "std.ny");
      char out_dir[PATH_MAX];
      nyt_path_join(out_dir, sizeof(out_dir), root, "build/docs");
      const char *docs_args[640];
      int docs_argc = 0;
      docs_args[docs_argc++] = std_path;
      docs_args[docs_argc++] = "-o";
      docs_args[docs_argc++] = out_dir;
      for (int i = 0; i < a.unknown_count && docs_argc < 639; i++)
        docs_args[docs_argc++] = a.unknown[i];
      docs_args[docs_argc] = NULL;
      int rc = run_ny_tool(root, kind, "ny-doc", NULL, docs_args, docs_argc);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "install") == 0) {
      char bdir[PATH_MAX];
      build_kind_dir(bdir, sizeof(bdir), root, kind);
#ifndef _WIN32
      char prefix[PATH_MAX] = "/usr/local";
      (void)cmake_cache_value(bdir, "CMAKE_INSTALL_PREFIX", prefix, sizeof(prefix));
      if (geteuid() != 0 && !install_prefix_writable(prefix)) {
        nyt_err("ny-make", "install prefix %s is not writable; run: sudo ny make install",
                prefix);
        return 1;
      }
#endif
      char *argv2[] = {"cmake", "--install", bdir, NULL};
      int rc = run_cmd_build_output(argv2);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "uninstall") == 0) {
      int rc = run_uninstall_manifest(root, kind);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "repl") == 0) {
      int rc = run_ny_tool(root, kind, "ny", "-i", a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "std") == 0 || strcmp(cmd, "std_bc") == 0) {
      continue;
    } else if (strcmp(cmd, "ny") == 0) {
      int rc = a.unknown_count == 0 ? run_ny_tool(root, kind, "ny", "-i", NULL, 0)
                                    : run_ny_tool(root, kind, "ny", NULL, a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "gprof") == 0) {
      int rc = run_ny_tool(root, kind, "ny-perf", "profile", a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "asan") == 0) {
      int rc = run_sanitizer_tests(root, kind, "asan",
                                   "-fsanitize=address -fno-omit-frame-pointer -g3",
                                   "-fsanitize=address", jobs, a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "ubsan") == 0) {
      int rc = run_sanitizer_tests(
          root, kind, "ubsan",
          "-fsanitize=undefined -fno-omit-frame-pointer -g3 -fno-sanitize-recover=undefined",
          "-fsanitize=undefined", jobs, a.unknown, a.unknown_count);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "fuzz") == 0) {
      const char *fargs[] = {"--smoke", NULL};
      int rc = run_ny_tool(root, kind, "ny-test", NULL, fargs, 1);
      if (rc != 0)
        return rc;
    } else if (strcmp(cmd, "optcheck") == 0 || strcmp(cmd, "fb") == 0) {
      nyt_err("ny-make", "command '%s' is not yet ported to native C path", cmd);
      return 2;
    } else {
      nyt_err("ny-make", "unsupported command: %s", cmd);
      return 2;
    }
  }

  return 0;
}
