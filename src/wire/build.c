#include "wire/build.h"
#include "base/common.h"
#include "base/util.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#else
#include "base/compat.h"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int ny_builder_dwarf_version(void) {
#ifdef _WIN32
  return 0;
#else
  const char *v = getenv("NYTRIX_DWARF_VERSION");
  if (!v || !*v)
    return 5;
  char *end = NULL;
  long parsed = strtol(v, &end, 10);
  if (end == v || (end && *end != '\0') || parsed < 2 || parsed > 5)
    return 5;
  return (int)parsed;
#endif
}

static void ny_builder_dwarf_flag(char *buf, size_t buf_len, bool debug) {
  if (!buf || buf_len == 0)
    return;
#ifdef _WIN32
  (void)debug;
  snprintf(buf, buf_len, "-g0");
#else
  if (!debug) {
    snprintf(buf, buf_len, "-g0");
    return;
  }
  snprintf(buf, buf_len, "-gdwarf-%d", ny_builder_dwarf_version());
#endif
}

static void ny_free_host_pool(char *pool[], size_t pool_len) {
  if (!pool)
    return;
  for (size_t i = 0; i < pool_len; ++i)
    free(pool[i]);
}

#if defined(__arm__) && !defined(__aarch64__)
static const char *ny_builder_arm_float_abi_flag(void) {
  const char *abi = getenv("NYTRIX_ARM_FLOAT_ABI");
  if (abi && *abi) {
    if (strcmp(abi, "hard") == 0)
      return "-mfloat-abi=hard";
    if (strcmp(abi, "softfp") == 0)
      return "-mfloat-abi=softfp";
    if (strcmp(abi, "soft") == 0)
      return "-mfloat-abi=soft";
  }
  const char *env = getenv("NYTRIX_HOST_CFLAGS");
  if (env && *env) {
    if (strstr(env, "-mfloat-abi=hard"))
      return "-mfloat-abi=hard";
    if (strstr(env, "-mfloat-abi=softfp"))
      return "-mfloat-abi=softfp";
    if (strstr(env, "-mfloat-abi=soft"))
      return "-mfloat-abi=soft";
  }
  return "-mfloat-abi=hard";
}
#endif

static int ny_file_mtime(const char *path, time_t *out_mtime) {
  if (!path || !*path || !out_mtime)
    return -1;
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  *out_mtime = st.st_mtime;
  return 0;
}

#if !defined(__APPLE__) && !defined(_WIN32)
static bool ny_tool_in_path(const char *tool) {
  if (!tool || !*tool)
    return false;
  /*
   * Tool availability is queried in tight loops during test linking; cache
   * stable lookups to avoid repeatedly scanning large PATH values.
   */
  if (strcmp(tool, "ld.lld") == 0) {
    static int cached_ld_lld = -1;
    if (cached_ld_lld >= 0)
      return cached_ld_lld == 1;
    /* fall through to full lookup, then memoize */
    bool found_now = false;
    const char *path = getenv("PATH");
    if (!path || !*path) {
      cached_ld_lld = 0;
      return false;
    }
    char *buf = ny_strdup(path);
    if (!buf) {
      cached_ld_lld = 0;
      return false;
    }
    const char delim[2] = ":";
    char *save = NULL;
    for (char *tok = strtok_r(buf, delim, &save); tok;
         tok = strtok_r(NULL, delim, &save)) {
      if (!*tok)
        continue;
      char full[PATH_MAX];
      snprintf(full, sizeof(full), "%s/%s", tok, tool);
      if (access(full, X_OK) == 0 || access(full, F_OK) == 0) {
        found_now = true;
        break;
      }
    }
    free(buf);
    cached_ld_lld = found_now ? 1 : 0;
    return found_now;
  }
  const char *path = getenv("PATH");
  if (!path || !*path)
    return false;
  char *buf = ny_strdup(path);
  if (!buf)
    return false;
  const char delim[2] = ":";
  bool found = false;
  char *save = NULL;
  for (char *tok = strtok_r(buf, delim, &save); tok;
       tok = strtok_r(NULL, delim, &save)) {
    if (!*tok)
      continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", tok, tool);
    if (access(full, X_OK) == 0 || access(full, F_OK) == 0) {
      found = true;
      break;
    }
  }
  free(buf);
  return found;
}
#endif
static time_t ny_runtime_latest_dep_mtime(const char *root) {
  static char cached_root[PATH_MAX];
  static time_t cached_latest = 0;
  static int cached_valid = 0;
  if (root && *root && cached_valid && strcmp(cached_root, root) == 0)
    return cached_latest;

  static const char *const deps[] = {
      "src/rt/init.c",    "src/rt/ast.c",       "src/rt/core.c",
      "src/rt/ffi.c",     "src/rt/math.c",      "src/rt/memory.c",
      "src/rt/os.c",      "src/rt/string.c",    "src/rt/shared.h",
      "src/rt/runtime.h", "src/rt/defs.h",      "src/ast/ast.h",
      "src/ast/json.h",   "src/parse/parser.h", "src/lex/lexer.h",
      "src/code/types.h", "src/base/common.h",  "src/base/compat.h",
  };
  time_t latest = 0;
  char full[PATH_MAX];
  for (size_t i = 0; i < sizeof(deps) / sizeof(deps[0]); ++i) {
    snprintf(full, sizeof(full), "%s/%s", root, deps[i]);
    time_t mt = 0;
    if (ny_file_mtime(full, &mt) == 0 && mt > latest)
      latest = mt;
  }
  if (root && *root) {
    snprintf(cached_root, sizeof(cached_root), "%s", root);
    cached_latest = latest;
    cached_valid = 1;
  }
  return latest;
}

static void ny_runtime_cache_path(char *out, size_t out_len, const char *cc,
                                  const char *root, bool debug,
                                  const char *llvm_include_arg) {
  const char *tmp = ny_get_temp_dir();
  const char *host_flags = getenv("NYTRIX_HOST_CFLAGS");
  const char *arm_float_abi = getenv("NYTRIX_ARM_FLOAT_ABI");
  const char *cache_rev = "rtcache-v3";
  char dwarf_key[16];
  if (debug)
    snprintf(dwarf_key, sizeof(dwarf_key), "d%d", ny_builder_dwarf_version());
  else
    snprintf(dwarf_key, sizeof(dwarf_key), "d0");
  char key[PATH_MAX * 2];
  snprintf(key, sizeof(key), "%s|%s|%d|%s|%s|%s|%s|%s", cc ? cc : "",
           root ? root : "", debug ? 1 : 0,
           llvm_include_arg ? llvm_include_arg : "",
           host_flags ? host_flags : "", arm_float_abi ? arm_float_abi : "",
           dwarf_key, cache_rev);
  uint64_t h = ny_hash64(key, strlen(key));
#ifdef _WIN32
  snprintf(out, out_len, "%s/ny_rt_cache_%016llx_%s.obj", tmp,
           (unsigned long long)h, debug ? "dbg" : "rel");
#else
  snprintf(out, out_len, "%s/ny_rt_cache_%016llx_%s.o", tmp,
           (unsigned long long)h, debug ? "dbg" : "rel");
#endif
}

static bool ny_try_restore_runtime_cache(const char *cache_obj,
                                         const char *out_runtime,
                                         const char *root) {
  if (!cache_obj || !*cache_obj || !out_runtime || !*out_runtime || !root ||
      !*root)
    return false;
  if (access(cache_obj, R_OK) != 0)
    return false;
  time_t cache_mt = 0;
  if (ny_file_mtime(cache_obj, &cache_mt) != 0)
    return false;
  time_t dep_mt = ny_runtime_latest_dep_mtime(root);
  if (dep_mt > 0 && cache_mt < dep_mt)
    return false;
  return ny_copy_file(cache_obj, out_runtime) == 0;
}

static void ny_update_runtime_cache(const char *cache_obj,
                                    const char *out_runtime) {
  if (!cache_obj || !*cache_obj || !out_runtime || !*out_runtime)
    return;
  (void)ny_copy_file(out_runtime, cache_obj);
}

#ifdef _WIN32
static int is_msvc_cc(const char *cc) {
  if (!cc || !*cc)
    return 0;
  const char *base = strrchr(cc, '/');
  if (!base)
    base = strrchr(cc, '\\');
  base = base ? base + 1 : cc;
  if (strcasecmp(base, "cl") == 0 || strcasecmp(base, "cl.exe") == 0)
    return 1;
  if (strcasecmp(base, "clang-cl") == 0 ||
      strcasecmp(base, "clang-cl.exe") == 0)
    return 1;
  return 0;
}

static const char *normalize_cc(const char *cc) {
  static char buf[PATH_MAX];
  if (!cc)
    return NULL;
  while (*cc == ' ' || *cc == '\t')
    cc++;
  if (!*cc)
    return NULL;

  size_t len = strlen(cc);
  while (len > 0 && isspace((unsigned char)cc[len - 1]))
    len--;
  if (len == 0)
    return NULL;
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  memcpy(buf, cc, len);
  buf[len] = '\0';

  if (buf[0] == '"' || buf[0] == '\'') {
    char quote = buf[0];
    size_t i = 1, out = 0;
    while (i < len && buf[i] != quote && out + 1 < sizeof(buf))
      buf[out++] = buf[i++];
    buf[out] = '\0';
    return out ? buf : NULL;
  }

  /*
   * Keep absolute paths with spaces intact (e.g. "C:\\Program Files\\...").
   * Fall back to first token parsing for values like "clang
   * -fcolor-diagnostics".
   */
  if (access(buf, F_OK) == 0)
    return buf;

  /*
   * Handle unquoted absolute Windows paths with spaces. Some env setups pass
   * values like: C:\Program Files\LLVM\bin\clang.exe
   */
  if ((strchr(buf, '\\') || strchr(buf, '/') ||
       (isalpha((unsigned char)buf[0]) && buf[1] == ':'))) {
    for (size_t j = 0; buf[j]; ++j) {
      if (buf[j] == '.' && tolower((unsigned char)buf[j + 1]) == 'e' &&
          tolower((unsigned char)buf[j + 2]) == 'x' &&
          tolower((unsigned char)buf[j + 3]) == 'e') {
        size_t end = j + 4;
        if (buf[end] == '\0' || isspace((unsigned char)buf[end]) ||
            buf[end] == '"' || buf[end] == '\'' || buf[end] == ';' ||
            buf[end] == ',') {
          char saved = buf[end];
          buf[end] = '\0';
          if (access(buf, F_OK) == 0)
            return buf;
          buf[end] = saved;
        }
      }
    }
  }

  size_t i = 0;
  while (buf[i] && !isspace((unsigned char)buf[i]))
    i++;
  buf[i] = '\0';
  return i ? buf : NULL;
}

static int has_ext(const char *s, const char *ext) {
  if (!s || !ext)
    return 0;
  size_t sl = strlen(s);
  size_t el = strlen(ext);
  if (sl < el)
    return 0;
  return strcasecmp(s + sl - el, ext) == 0;
}

static int is_path_like(const char *s) {
  if (!s || !*s)
    return 0;
  if (strchr(s, '\\') || strchr(s, '/'))
    return 1;
  if (isalpha((unsigned char)s[0]) && s[1] == ':')
    return 1;
  return 0;
}

static int has_llvm_c_core(const char *inc) {
  if (!inc || !*inc)
    return 0;
  char probe[PATH_MAX];
  snprintf(probe, sizeof(probe), "%s/llvm-c/Core.h", inc);
  if (access(probe, F_OK) == 0)
    return 1;
#ifdef _WIN32
  snprintf(probe, sizeof(probe), "%s\\llvm-c\\Core.h", inc);
  if (access(probe, F_OK) == 0)
    return 1;
#endif
  return 0;
}

static const char *find_llvm_include_dir(void) {
  static char buf[PATH_MAX];
  const char *env = getenv("NYTRIX_LLVM_HEADERS");
  if (env && *env && has_llvm_c_core(env))
    return env;
  env = getenv("NYTRIX_LLVM_INCLUDE");
  if (env && *env && has_llvm_c_core(env))
    return env;
  env = getenv("LLVM_ROOT");
  if (env && *env) {
    snprintf(buf, sizeof(buf), "%s\\include", env);
    if (has_llvm_c_core(buf))
      return buf;
  }
  const char *root = ny_src_root();
  if (root && *root) {
    snprintf(buf, sizeof(buf), "%s/build/third_party/llvm/headers/include",
             root);
    if (has_llvm_c_core(buf))
      return buf;
  }
#ifdef _WIN32
  static const char *const defaults[] = {
      "C:\\PROGRA~1\\LLVM\\include",
      "C:\\PROGRA~2\\LLVM\\include",
      "C:\\Program Files\\LLVM\\include",
      "C:\\Program Files (x86)\\LLVM\\include",
  };
  for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
    if (has_llvm_c_core(defaults[i]))
      return defaults[i];
  }
#endif
  return NULL;
}

static int spawn_with_host_flags(const char *const base[], const char *env,
                                 char *pool[], size_t *pool_len) {
  (void)env;
  (void)pool;
  (void)pool_len;
  return ny_exec_spawn(base);
}
#else
static void append_host_flags(const char *env, const char *argv[], size_t *idx,
                              size_t max, char *pool[], size_t *pool_len) {
  if (env && *env) {
    char *copy = ny_strdup(env);
    if (copy) {
      char *tok = strtok(copy, " \t");
      while (tok && *idx + 1 < max) {
        argv[(*idx)++] = tok;
        tok = strtok(NULL, " \t");
      }
      if (*pool_len < 16)
        pool[(*pool_len)++] = copy;
      else
        free(copy);
    }
  }
#if !defined(__APPLE__) && !defined(_WIN32)
  if (ny_env_enabled("NYTRIX_GPROF") && *idx + 1 < max) {
    bool seen_pg = false;
    for (size_t i = 0; i < *idx; ++i) {
      if (argv[i] && strcmp(argv[i], "-pg") == 0) {
        seen_pg = true;
        break;
      }
    }
    if (!seen_pg)
      argv[(*idx)++] = "-pg";
  }
#endif
}

static int spawn_with_host_flags(const char *const base[], const char *env,
                                 char *pool[], size_t *pool_len) {
  const size_t max = 128;
  const char *argv[128];
  size_t idx = 0;
  while (base[idx]) {
    argv[idx] = base[idx];
    idx++;
  }
  argv[idx] = NULL;
  append_host_flags(env, argv, &idx, max, pool, pool_len);
  argv[idx] = NULL;
  return ny_exec_spawn(argv);
}
#endif

const char *ny_builder_choose_cc(void) {
#ifdef _WIN32
  const char *cc = normalize_cc(getenv("NYTRIX_CC"));
  if (!cc)
    cc = normalize_cc(getenv("CC"));
#else
  const char *cc = getenv("NYTRIX_CC");
  if (!cc)
    cc = getenv("CC");
#endif
  if (cc && *cc) {
#ifdef _WIN32
    /* Avoid flaky WindowsApps aliases and broken quoted command values. */
    int valid_cc = 0;
    if (!strstr(cc, "WindowsApps") && !strstr(cc, "windowsapps")) {
      if (!is_path_like(cc) || access(cc, F_OK) == 0)
        valid_cc = 1;
    }
    if (valid_cc)
      return cc;
    cc = NULL;
#else
    return cc;
#endif
  }
  if (!cc) {
#ifdef _WIN32
    static const char *const win_clang_candidates[] = {
        "C:\\PROGRA~1\\LLVM\\bin\\clang.exe",
        "C:\\PROGRA~2\\LLVM\\bin\\clang.exe",
        "C:\\PROGRA~1\\LLVM\\bin\\clang-cl.exe",
        "C:\\PROGRA~2\\LLVM\\bin\\clang-cl.exe",
        "C:\\Program Files\\LLVM\\bin\\clang.exe",
        "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
        "C:\\Program Files\\LLVM\\bin\\clang-cl.exe",
        "C:\\Program Files (x86)\\LLVM\\bin\\clang-cl.exe",
    };
    const char *llvm_root = getenv("LLVM_ROOT");
    static char llvm_root_candidates[2][PATH_MAX];
    size_t llvm_root_count = 0;
    if (llvm_root && *llvm_root) {
      snprintf(llvm_root_candidates[llvm_root_count++], PATH_MAX,
               "%s\\bin\\clang.exe", llvm_root);
      snprintf(llvm_root_candidates[llvm_root_count++], PATH_MAX,
               "%s\\bin\\clang-cl.exe", llvm_root);
    }
    for (size_t i = 0; i < llvm_root_count; ++i) {
      if (access(llvm_root_candidates[i], F_OK) == 0)
        return llvm_root_candidates[i];
    }
    for (size_t i = 0;
         i < sizeof(win_clang_candidates) / sizeof(win_clang_candidates[0]);
         ++i) {
      if (access(win_clang_candidates[i], F_OK) == 0)
        return win_clang_candidates[i];
    }
    cc = "clang";
#else
    cc = "clang";
#endif
  }
  return cc;
}

int ny_exec_spawn(const char *const argv[]) {
#ifdef _WIN32
  const char *a0 = argv[0] ? argv[0] : "";
  int rc = -1;
  if (is_path_like(a0) && access(a0, F_OK) == 0)
    rc = _spawnv(_P_WAIT, a0, (const char *const *)argv);
  else
    rc = _spawnvp(_P_WAIT, a0, (const char *const *)argv);
  if (rc < 0 && errno == ENOENT) {
    const char *base = strrchr(a0, '/');
    if (!base)
      base = strrchr(a0, '\\');
    base = base ? base + 1 : a0;
    char base_buf[PATH_MAX];
    snprintf(base_buf, sizeof(base_buf), "%s", base);
    char *name = base_buf;
    while (*name && isspace((unsigned char)*name))
      name++;
    if (*name == '"' || *name == '\'') {
      char q = *name++;
      char *endq = strchr(name, q);
      if (endq)
        *endq = '\0';
    }
    char *ws = strpbrk(name, " \t");
    if (ws)
      *ws = '\0';
    if (strcasecmp(name, "clang") == 0 || strcasecmp(name, "clang.exe") == 0 ||
        strcasecmp(name, "clang-cl") == 0 ||
        strcasecmp(name, "clang-cl.exe") == 0) {
      static const char *const win_clang_candidates[] = {
          "C:\\PROGRA~1\\LLVM\\bin\\clang.exe",
          "C:\\PROGRA~2\\LLVM\\bin\\clang.exe",
          "C:\\PROGRA~1\\LLVM\\bin\\clang-cl.exe",
          "C:\\PROGRA~2\\LLVM\\bin\\clang-cl.exe",
          "C:\\Program Files\\LLVM\\bin\\clang.exe",
          "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe",
          "C:\\Program Files\\LLVM\\bin\\clang-cl.exe",
          "C:\\Program Files (x86)\\LLVM\\bin\\clang-cl.exe",
      };
      const char *argv2[128];
      size_t k = 0;
      while (argv[k] && k + 1 < (sizeof(argv2) / sizeof(argv2[0]))) {
        argv2[k] = argv[k];
        k++;
      }
      argv2[k] = NULL;
      for (size_t i = 0;
           i < sizeof(win_clang_candidates) / sizeof(win_clang_candidates[0]);
           ++i) {
        if (access(win_clang_candidates[i], F_OK) != 0)
          continue;
        argv2[0] = win_clang_candidates[i];
        rc = _spawnv(_P_WAIT, argv2[0], (const char *const *)argv2);
        if (rc >= 0)
          return rc;
      }
    }
  }
  if (rc < 0) {
    perror("_spawnvp");
    return -1;
  }
  return rc;
#else
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }
  if (pid == 0) {
    execvp(argv[0], (char *const *)argv);
    perror("execvp");
    _exit(127);
  }
  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    perror("waitpid");
    return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "process %s terminated by signal %d\n", argv[0],
            WTERMSIG(status));
    return -1;
  }
  return status;
#endif
}

bool ny_builder_compile_runtime(const char *cc, const char *out_runtime,
                                const char *out_ast, bool debug, bool profile) {
  const char *root = ny_src_root();
  static char include_arg[PATH_MAX + 12];
  static char llvm_include_arg[PATH_MAX + 12];
  static char runtime_src[PATH_MAX];
  static char ast_src[PATH_MAX];
  char dwarf_flag[16];
  char cache_obj[PATH_MAX];
  snprintf(runtime_src, sizeof(runtime_src), "%s/src/rt/init.c", root);
  snprintf(ast_src, sizeof(ast_src), "%s/src/rt/ast.c", root);
  ny_builder_dwarf_flag(dwarf_flag, sizeof(dwarf_flag), debug);
#if defined(__arm__) && !defined(__aarch64__)
  const char *arm_float_abi_flag = ny_builder_arm_float_abi_flag();
#endif
#ifdef _WIN32
  bool msvc = is_msvc_cc(cc);
  if (msvc) {
    snprintf(include_arg, sizeof(include_arg), "/I%s/src", root);
    const char *llvm_inc = find_llvm_include_dir();
    if (llvm_inc && *llvm_inc)
      snprintf(llvm_include_arg, sizeof(llvm_include_arg), "/I%s", llvm_inc);
    else
      snprintf(llvm_include_arg, sizeof(llvm_include_arg), "%s", include_arg);
    ny_runtime_cache_path(cache_obj, sizeof(cache_obj), cc, root, debug,
                          llvm_include_arg);
    if (!out_ast && ny_try_restore_runtime_cache(cache_obj, out_runtime, root))
      return true;
    static char out_arg[PATH_MAX + 8];
    snprintf(out_arg, sizeof(out_arg), "/Fo%s", out_runtime);
    const char *const runtime_args[] = {cc,
                                        "/nologo",
                                        "/std:c11",
                                        debug ? "/Od" : "/O2",
                                        "/MD",
                                        "/D_CRT_SECURE_NO_WARNINGS",
                                        "/D_CRT_NONSTDC_NO_WARNINGS",
                                        "/DNYTRIX_RUNTIME_ONLY",
                                        include_arg,
                                        llvm_include_arg,
                                        "/c",
                                        runtime_src,
                                        out_arg,
                                        profile ? "/Gh" : NULL,
                                        NULL};
    if (verbose_enabled >= 2) {
      fprintf(stderr, "[**] Spawning runtime build:");
      for (int j = 0; runtime_args[j]; j++)
        fprintf(stderr, " %s", runtime_args[j]);
      fprintf(stderr, "\n");
    }
    int rc = ny_exec_spawn(runtime_args);
    if (rc != 0) {
      NY_LOG_ERR("Runtime compilation failed (exit=%d)\n", rc);
      return false;
    }
    if (!out_ast)
      ny_update_runtime_cache(cache_obj, out_runtime);
    if (out_ast) {
      static char out_ast_arg[PATH_MAX + 8];
      snprintf(out_ast_arg, sizeof(out_ast_arg), "/Fo%s", out_ast);
      const char *const ast_args[] = {cc,
                                      "/nologo",
                                      "/std:c11",
                                      debug ? "/Od" : "/O2",
                                      "/MD",
                                      "/D_CRT_SECURE_NO_WARNINGS",
                                      "/D_CRT_NONSTDC_NO_WARNINGS",
                                      include_arg,
                                      llvm_include_arg,
                                      "/c",
                                      ast_src,
                                      out_ast_arg,
                                      NULL};
      rc = ny_exec_spawn(ast_args);
      if (rc != 0) {
        NY_LOG_ERR("Runtime AST compilation failed (exit=%d)\n", rc);
        return false;
      }
    }
    return true;
  }
#endif
  snprintf(include_arg, sizeof(include_arg), "-I%s/src", root);
#ifdef _WIN32
  {
    const char *llvm_inc = find_llvm_include_dir();
    if (llvm_inc && *llvm_inc)
      snprintf(llvm_include_arg, sizeof(llvm_include_arg), "-I%s", llvm_inc);
    else
      snprintf(llvm_include_arg, sizeof(llvm_include_arg), "%s", include_arg);
  }
#else
  snprintf(llvm_include_arg, sizeof(llvm_include_arg), "%s", include_arg);
#endif
  ny_runtime_cache_path(cache_obj, sizeof(cache_obj), cc, root, debug,
                        llvm_include_arg);
  if (!out_ast && ny_try_restore_runtime_cache(cache_obj, out_runtime, root))
    return true;
#if defined(__APPLE__) || defined(_WIN32)
  const char *const runtime_args[] = {
      cc,
      "-std=gnu11",
      debug ? "-g3" : "-O2",
      dwarf_flag,
      debug ? "-fno-omit-frame-pointer" : "-fomit-frame-pointer",
      debug ? "-fno-optimize-sibling-calls" : "-foptimize-sibling-calls",
#if defined(__arm__) && !defined(__aarch64__)
      arm_float_abi_flag,
#endif
#if !defined(_WIN32)
      "-fPIC",
#endif
      "-fvisibility=hidden",
      "-ffunction-sections",
      "-fdata-sections",
#ifdef _WIN32
      "-D_CRT_SECURE_NO_WARNINGS",
      "-D_CRT_NONSTDC_NO_WARNINGS",
#endif
      "-DNYTRIX_RUNTIME_ONLY",
      include_arg,
      llvm_include_arg,
      "-c",
      runtime_src,
      "-o",
      out_runtime,
      profile ? "-pg" : NULL,
      NULL};
#else
  const char *const runtime_args[] = {
      cc,
      "-std=gnu11",
      debug ? "-g3" : "-O2",
      dwarf_flag,
      debug ? "-fno-omit-frame-pointer" : "-fomit-frame-pointer",
      debug ? "-fno-optimize-sibling-calls" : "-foptimize-sibling-calls",
#if defined(__arm__) && !defined(__aarch64__)
      arm_float_abi_flag,
#endif
      "-fno-pie",
      "-fvisibility=hidden",
      "-ffunction-sections",
      "-fdata-sections",
      "-DNYTRIX_RUNTIME_ONLY",
      include_arg,
      llvm_include_arg,
      "-c",
      runtime_src,
      "-o",
      out_runtime,
      profile ? "-pg" : NULL,
      NULL};
#endif
  if (verbose_enabled >= 2) {
    fprintf(stderr, "[**] Spawning runtime build:");
    for (int j = 0; runtime_args[j]; j++)
      fprintf(stderr, " %s", runtime_args[j]);
    fprintf(stderr, "\n");
  }
  char *host_pool[16];
  size_t pool_len = 0;
  int rc = spawn_with_host_flags(runtime_args, getenv("NYTRIX_HOST_CFLAGS"),
                                 host_pool, &pool_len);
  ny_free_host_pool(host_pool, pool_len);
  if (rc != 0) {
    NY_LOG_ERR("Runtime compilation failed (exit=%d)\n", rc);
    return false;
  }
  if (!out_ast)
    ny_update_runtime_cache(cache_obj, out_runtime);
  if (out_ast) {
    const char *const ast_args[] = {
        cc,
        "-std=gnu11",
        debug ? "-g3" : "-Os",
        dwarf_flag,
        debug ? "-fno-omit-frame-pointer" : "-fomit-frame-pointer",
        debug ? "-fno-optimize-sibling-calls" : "-foptimize-sibling-calls",
#if defined(__arm__) && !defined(__aarch64__)
        arm_float_abi_flag,
#endif
#if !defined(_WIN32)
        "-fPIC",
#endif
#if !defined(__APPLE__) && !defined(_WIN32)
        "-fno-pie",
#endif
        "-fvisibility=hidden",
        "-ffunction-sections",
        "-fdata-sections",
#ifdef _WIN32
        "-D_CRT_SECURE_NO_WARNINGS",
        "-D_CRT_NONSTDC_NO_WARNINGS",
#endif
        include_arg,
        llvm_include_arg,
        "-c",
        ast_src,
        "-o",
        out_ast,
        NULL};
    char *ast_pool[16];
    size_t ast_pool_len = 0;
    rc = spawn_with_host_flags(ast_args, getenv("NYTRIX_HOST_CFLAGS"), ast_pool,
                               &ast_pool_len);
    ny_free_host_pool(ast_pool, ast_pool_len);
    if (rc != 0) {
      NY_LOG_ERR("Runtime AST compilation failed (exit=%d)\n", rc);
      return false;
    }
  }
  return true;
}

bool ny_builder_link(const char *cc, const char *obj_path,
                     const char *runtime_obj, const char *runtime_ast_obj,
                     const char *const extra_objs[], size_t extra_count,
                     const char *const link_dirs[], size_t link_dir_count,
                     const char *const link_libs[], size_t link_lib_count,
                     const char *output_path, bool link_strip, bool debug,
                     bool profile) {
  const size_t max_args = 128;
  const char *argv[max_args];
  size_t idx = 0;
#if defined(__arm__) && !defined(__aarch64__)
  const char *arm_float_abi_flag = ny_builder_arm_float_abi_flag();
#endif
#ifdef _WIN32
  (void)link_strip;
  bool msvc = is_msvc_cc(cc);
  if (msvc) {
    char *dyn_args[64];
    size_t dyn_count = 0;
    static char out_arg[PATH_MAX + 8];
    snprintf(out_arg, sizeof(out_arg), "/Fe:%s", output_path);
    argv[idx++] = cc;
    argv[idx++] = "/nologo";
    if (debug)
      argv[idx++] = "/DEBUG";
    argv[idx++] = obj_path;
    if (runtime_obj)
      argv[idx++] = runtime_obj;
    if (runtime_ast_obj)
      argv[idx++] = runtime_ast_obj;
    for (size_t i = 0; i < extra_count && idx + 8 < max_args; ++i) {
      argv[idx++] = extra_objs[i];
    }
    argv[idx++] = out_arg;
    argv[idx++] = "/link";
    for (size_t i = 0; i < link_dir_count && idx + 2 < max_args; ++i) {
      const char *ld = link_dirs[i];
      if (!ld)
        continue;
      if (strncmp(ld, "/LIBPATH:", 9) == 0) {
        argv[idx++] = ld;
      } else if (ld[0] == '-' && ld[1] == 'L') {
        char tmp[PATH_MAX + 16];
        snprintf(tmp, sizeof(tmp), "/LIBPATH:%s", ld + 2);
        dyn_args[dyn_count] = ny_strdup(tmp);
        argv[idx++] = dyn_args[dyn_count++];
      } else if (is_path_like(ld)) {
        char tmp[PATH_MAX + 16];
        snprintf(tmp, sizeof(tmp), "/LIBPATH:%s", ld);
        dyn_args[dyn_count] = ny_strdup(tmp);
        argv[idx++] = dyn_args[dyn_count++];
      } else {
        argv[idx++] = ld;
      }
    }
    for (size_t i = 0; i < link_lib_count && idx + 2 < max_args; ++i) {
      const char *lib = link_libs[i];
      if (!lib)
        continue;
      if (strncmp(lib, "/DEFAULTLIB:", 12) == 0) {
        argv[idx++] = lib;
      } else if (lib[0] == '-' && lib[1] == 'l') {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s.lib", lib + 2);
        dyn_args[dyn_count] = ny_strdup(tmp);
        argv[idx++] = dyn_args[dyn_count++];
      } else if (has_ext(lib, ".lib") || has_ext(lib, ".obj") ||
                 has_ext(lib, ".dll") || is_path_like(lib)) {
        argv[idx++] = lib;
      } else {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s.lib", lib);
        dyn_args[dyn_count] = ny_strdup(tmp);
        argv[idx++] = dyn_args[dyn_count++];
      }
    }
    argv[idx++] = "ws2_32.lib";
    argv[idx] = NULL;
    int rc = ny_exec_spawn(argv);
    for (size_t i = 0; i < dyn_count; i++)
      free(dyn_args[i]);
    if (rc != 0) {
      NY_LOG_ERR("Linking failed (exit=%d)\n", rc);
      return false;
    }
    return true;
  }
#endif
  argv[idx++] = cc;
  if (debug)
    argv[idx++] = "-g3";
  if (profile)
    argv[idx++] = "-pg";
#if !defined(__APPLE__) && !defined(_WIN32)
  /*
   * Prefer lld when available: AOT link is a major per-file cost in tests and
   * CLI workflows. This cuts link latency noticeably on large std modules.
   */
  const char *lld_env = getenv("NYTRIX_USE_LLD");
  bool use_lld =
      lld_env ? ny_env_is_truthy(lld_env) : ny_tool_in_path("ld.lld");
  if (use_lld)
    argv[idx++] = "-fuse-ld=lld";
#endif
#if defined(__APPLE__)
  bool enable_mac_pie = ny_env_enabled("NYTRIX_MAC_PIE");
  if (enable_mac_pie) {
    argv[idx++] = "-fPIE";
    argv[idx++] = "-Wl,-pie";
  }
#else
#if !defined(_WIN32)
  argv[idx++] = "-no-pie";
#endif
#endif
  argv[idx++] = obj_path;
#if defined(__arm__) && !defined(__aarch64__)
  argv[idx++] = arm_float_abi_flag;
#endif
  if (runtime_obj)
    argv[idx++] = runtime_obj;
  if (runtime_ast_obj)
    argv[idx++] = runtime_ast_obj;
  const char *shared_rt_path = NULL;
  for (size_t i = 0; i < extra_count; ++i) {
    if (idx + 12 >= max_args)
      break;
    argv[idx++] = extra_objs[i];
    /* Remember the first .so so we can add an rpath */
    if (!shared_rt_path) {
      const char *p = extra_objs[i];
      const char *dot = strrchr(p, '.');
      if (dot && strcmp(dot, ".so") == 0) {
        shared_rt_path = p;
      }
    }
  }
  for (size_t i = 0; i < link_dir_count; ++i) {
    if (idx + 1 >= max_args)
      break;
    argv[idx++] = link_dirs[i];
  }
#if !defined(__APPLE__) && !defined(_WIN32)
  argv[idx++] = debug ? "-Wl,--build-id" : "-Wl,--build-id=none";
  if (!debug)
    argv[idx++] = "-Wl,--gc-sections";
  argv[idx++] = "-Wl,-O1";
  argv[idx++] = "-Wl,--no-as-needed";
  if (link_strip)
    argv[idx++] = "-Wl,--strip-all";
#elif defined(__APPLE__)
  if (link_strip)
    argv[idx++] = "-Wl,-dead_strip";
#endif
  if (shared_rt_path) {
#ifndef _WIN32
    static char rpath_buf[PATH_MAX];
    const char *slash = strrchr(shared_rt_path, '/');
    if (slash) {
      size_t len = (size_t)(slash - shared_rt_path);
      if (len >= sizeof(rpath_buf))
        len = sizeof(rpath_buf) - 1;
      memcpy(rpath_buf, shared_rt_path, len);
      rpath_buf[len] = '\0';
      static char rpath_arg[PATH_MAX + 16];
#ifdef __APPLE__
      snprintf(rpath_arg, sizeof(rpath_arg), "-Wl,-rpath,%s", rpath_buf);
#else
      snprintf(rpath_arg, sizeof(rpath_arg), "-Wl,-rpath,%s", rpath_buf);
#endif
      argv[idx++] = rpath_arg;
      static char ldir_arg[PATH_MAX + 4];
      snprintf(ldir_arg, sizeof(ldir_arg), "-L%s", rpath_buf);
      argv[idx++] = ldir_arg;
      argv[idx++] = "-lnytrixrt";
    }
#endif
  }
  argv[idx++] = "-o";
  argv[idx++] = output_path;
  bool link_readline = false;
#ifndef _WIN32
  link_readline = ny_env_enabled("NYTRIX_LINK_READLINE");
#endif
#ifndef _WIN32
  argv[idx++] = "-lm";
#endif
#ifndef _WIN32
  argv[idx++] = "-pthread";
  argv[idx++] = "-lpthread";
#endif
  if (link_readline)
    argv[idx++] = "-lreadline";
#if !defined(__APPLE__) && !defined(_WIN32)
  argv[idx++] = "-ldl";
#endif
#ifdef _WIN32
  argv[idx++] = "-lws2_32";
#endif
  char *shared_buf = NULL;
  const char *shared_env = getenv("NYTRIX_SHARED_LIBS");
  const char *shared_libs[16];
  size_t shared_count = 0;
  if (shared_env) {
    shared_buf = ny_strdup(shared_env);
    if (shared_buf) {
      char *token_t = strtok(shared_buf, ":, ");
      while (token_t && shared_count < 16) {
        shared_libs[shared_count++] = token_t;
        token_t = strtok(NULL, ":, ");
      }
    }
  }
  for (size_t i = 0; i < shared_count; ++i) {
    argv[idx++] = shared_libs[i];
  }
  if (shared_buf)
    free(shared_buf);
  for (size_t i = 0; i < link_lib_count; ++i) {
    if (idx + 1 >= max_args)
      break;
    argv[idx++] = link_libs[i];
  }
#if !defined(__APPLE__) && !defined(_WIN32)
  argv[idx++] = "-Wl,--as-needed";
#endif
  argv[idx] = NULL;
  char *host_pool[16];
  size_t pool_len = 0;
  int rc = spawn_with_host_flags(argv, getenv("NYTRIX_HOST_LDFLAGS"), host_pool,
                                 &pool_len);
  ny_free_host_pool(host_pool, pool_len);
  if (rc != 0) {
    NY_LOG_ERR("Linking failed (exit=%d)\n", rc);
    return false;
  }
  return true;
}

bool ny_builder_strip(const char *path) {
  if (!path)
    return false;
#ifdef _WIN32
  (void)path;
  return true;
#else
  const char *const argv[] = {"strip", "-s", path, NULL};
  int rc = ny_exec_spawn(argv);
  if (rc != 0) {
    NY_LOG_ERR("strip %s failed (exit=%d)\n", path, rc);
    return false;
  }
  return true;
#endif
}
