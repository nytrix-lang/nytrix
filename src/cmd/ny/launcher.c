/*
 * Thin ny entry: pure native-only -c arithmetic never loads LLVM/Z3/clang.
 * Everything else execs the full compiler (ny-full) next to this binary.
 */
#include "pureexpr.h"
#include "base/version.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef _WIN32
#include <libgen.h>
#include <unistd.h>
#else
#include <process.h>
#include <windows.h>
#endif

/*
 * Build path to ny-full beside `self` (mutates a copy of self for dirname).
 */
static int ny_full_path_from(const char *self, char *out, size_t out_len) {
  if (!self || !self[0] || !out || out_len < 16)
    return -1;
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", self) >= (int)sizeof(tmp))
    return -1;
#ifndef _WIN32
  char *dir = dirname(tmp);
  if (!dir || !*dir)
    return -1;
  return snprintf(out, out_len, "%s/ny-full", dir) >= (int)out_len ? -1 : 0;
#else
  char *slash = strrchr(tmp, '\\');
  char *slash2 = strrchr(tmp, '/');
  if (slash2 && (!slash || slash2 > slash))
    slash = slash2;
  if (slash)
    *slash = '\0';
  else
    tmp[0] = '\0';
  if (tmp[0])
    return snprintf(out, out_len, "%s\\ny-full.exe", tmp) >= (int)out_len ? -1
                                                                          : 0;
  return snprintf(out, out_len, "ny-full.exe") >= (int)out_len ? -1 : 0;
#endif
}

/*
 * Debug/sanitizer builds retain their suffixed executable names.
 */
static int ny_full_debug_path_from(const char *self, char *out,
                                   size_t out_len) {
  if (!self || !self[0] || !out || out_len < 22)
    return -1;
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", self) >= (int)sizeof(tmp))
    return -1;
#ifndef _WIN32
  char *dir = dirname(tmp);
  if (!dir || !*dir)
    return -1;
  return snprintf(out, out_len, "%s/ny-full_debug", dir) >= (int)out_len ? -1
                                                                         : 0;
#else
  char *slash = strrchr(tmp, '\\');
  char *slash2 = strrchr(tmp, '/');
  if (slash2 && (!slash || slash2 > slash))
    slash = slash2;
  if (slash)
    *slash = '\0';
  else
    tmp[0] = '\0';
  if (tmp[0])
    return snprintf(out, out_len, "%s\\ny-full_debug.exe", tmp) >= (int)out_len
               ? -1
               : 0;
  return snprintf(out, out_len, "ny-full_debug.exe") >= (int)out_len ? -1 : 0;
#endif
}

static int ny_sibling_tool_path_from(const char *self, const char *tool,
                                     char *out, size_t out_len) {
  if (!self || !*self || !tool || !*tool || !out || out_len < 16)
    return -1;
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", self) >= (int)sizeof(tmp))
    return -1;
#ifndef _WIN32
  char *dir = dirname(tmp);
  if (!dir || !*dir)
    return -1;
  return snprintf(out, out_len, "%s/%s", dir, tool) >= (int)out_len ? -1 : 0;
#else
  char *slash = strrchr(tmp, '\\');
  char *slash2 = strrchr(tmp, '/');
  if (slash2 && (!slash || slash2 > slash))
    slash = slash2;
  if (slash)
    *slash = '\0';
  else
    tmp[0] = '\0';
  return snprintf(out, out_len, "%s%s%s.exe", tmp, tmp[0] ? "\\" : "",
                  tool) >= (int)out_len
             ? -1
             : 0;
#endif
}

static const char *ny_native_tool_for(const char *command) {
  if (!command)
    return NULL;
  if (strcmp(command, "fmt") == 0 || strcmp(command, "format") == 0)
    return "ny-fmt";
  if (strcmp(command, "test") == 0 || strcmp(command, "tests") == 0)
    return "ny-test";
  if (strcmp(command, "perf") == 0 || strcmp(command, "bench") == 0)
    return "ny-perf";
  if (strcmp(command, "doc") == 0 || strcmp(command, "docs") == 0 ||
      strcmp(command, "web") == 0)
    return "ny-doc";
  if (strcmp(command, "make") == 0 || strcmp(command, "build") == 0)
    return "ny-make";
  return NULL;
}

/*
 * Native C tools have independent binaries. Dispatch them from the thin
 * launcher before loading ny-full (LLVM, compiler state, and core library).
 */
static int ny_exec_native_tool(int argc, char **argv) {
  if (argc < 2 || !argv || !argv[0])
    return 0;
  int command_i = 0;
  int skip_next = 0;
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i] ? argv[i] : "";
    if (skip_next) {
      skip_next = 0;
      continue;
    }
    if (strcmp(arg, "--color") == 0 || strcmp(arg, "--color=") == 0) {
      skip_next = 1;
      continue;
    }
    if (strcmp(arg, "--no-color") == 0 || strncmp(arg, "--color=", 8) == 0 ||
        !*arg)
      continue;
    if (arg[0] == '-' || strcmp(arg, "--") == 0)
      return 0;
    command_i = i;
    break;
  }
  if (!command_i)
    return 0;
  const char *tool = ny_native_tool_for(argv[command_i]);
  if (!tool)
    return 0;

  char tool_path[PATH_MAX];
  int found = 0;
  if (ny_sibling_tool_path_from(argv[0], tool, tool_path,
                                sizeof(tool_path)) == 0) {
#ifndef _WIN32
    found = access(tool_path, X_OK) == 0;
#else
    found = GetFileAttributesA(tool_path) != INVALID_FILE_ATTRIBUTES;
#endif
  }
#ifndef _WIN32
  if (!found) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
      self[n] = '\0';
      if (ny_sibling_tool_path_from(self, tool, tool_path,
                                    sizeof(tool_path)) == 0)
        found = access(tool_path, X_OK) == 0;
    }
  }
#endif
  if (!found)
    return 0;

  char **tool_argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
  if (!tool_argv) {
    fprintf(stderr, "ny: out of memory dispatching %s\n", tool);
    return 1;
  }
  int count = 0;
  tool_argv[count++] = tool_path;
  for (int i = 1; i < argc; ++i) {
    if (i != command_i)
      tool_argv[count++] = argv[i];
  }
  tool_argv[count] = NULL;
#ifndef _WIN32
  execv(tool_path, tool_argv);
  fprintf(stderr, "ny: failed to exec %s: %s\n", tool_path, strerror(errno));
  free(tool_argv);
  return 1;
#else
  int rc = _spawnv(_P_WAIT, tool_path, (const char *const *)tool_argv);
  free(tool_argv);
  return rc < 0 ? 127 : 256 + rc;
#endif
}

/*
 * Stale-pair detection (audit A6): every binary embeds a build identity
 * (`ny_version_id`: version + HEAD + dirty + stdlib hash). Before
 * dispatching, ask the target for its identity and compare. A mismatch means
 * the launcher and the compiler (or their stdlib bundles) were built from
 * different sources — the exact failure mode where two binaries print the
 * same release version yet accept different syntax. Warn by default;
 * NYTRIX_REQUIRE_FRESH=1 refuses instead. NYTRIX_NO_FRESH_CHECK=1 skips the
 * check (test harnesses with controlled builds). Targets predating
 * --version-id report "unknown" and warn once. Returns 0 to proceed,
 * nonzero to refuse.
 */
static int ny_read_target_id(const char *target, char *out, size_t out_len) {
  if (!target || !target[0] || !out || out_len < 8)
    return -1;
  char cmd[PATH_MAX + 48];
  /*
   * Usage errors go to stderr; keep the probe quiet and parse stdout only.
   */
#ifdef _WIN32
  if (snprintf(cmd, sizeof(cmd), "\"%s\" --version-id 2>NUL", target) >=
      (int)sizeof(cmd))
    return -1;
#else
  if (snprintf(cmd, sizeof(cmd), "\"%s\" --version-id 2>/dev/null", target) >=
      (int)sizeof(cmd))
    return -1;
#endif
#ifdef _WIN32
  FILE *pipe = _popen(cmd, "r");
#else
  FILE *pipe = popen(cmd, "r");
#endif
  if (!pipe)
    return -1;
  size_t used = 0;
  int ch = 0;
  while (used + 1 < out_len && (ch = fgetc(pipe)) != EOF && ch != '\n' &&
         ch != '\r')
    out[used++] = (char)ch;
  out[used] = '\0';
#ifdef _WIN32
  int rc = _pclose(pipe);
#else
  int rc = pclose(pipe);
#endif
  if (rc != 0 || !used)
    return -1;
  return 0;
}

static int ny_check_fresh(const char *target) {
  const char *skip = getenv("NYTRIX_NO_FRESH_CHECK");
  if (skip && *skip && strcmp(skip, "0") != 0)
    return 0;
  if (!target || !target[0])
    return 0;
  const char *own = ny_version_id();
  const char *strict = getenv("NYTRIX_REQUIRE_FRESH");
  int refuse = strict && *strict && strcmp(strict, "0") != 0;
  char theirs[1024];
  if (ny_read_target_id(target, theirs, sizeof(theirs)) != 0) {
    fprintf(stderr, "ny: warning: cannot verify build identity of '%s' "
                    "(predates --version-id); continuing\n",
            target);
    if (refuse) {
      fprintf(stderr, "ny: refusing (NYTRIX_REQUIRE_FRESH=1)\n");
      return 127;
    }
    return 0;
  }
  if (strcmp(own, theirs) != 0) {
    fprintf(stderr,
            "ny: warning: stale toolchain pair: launcher [%s] vs '%s' [%s]; "
            "rebuild/install both from the same tree\n",
            own, target, theirs);
    if (refuse) {
      fprintf(stderr, "ny: refusing (NYTRIX_REQUIRE_FRESH=1)\n");
      return 127;
    }
  }
  return 0;
}

static int ny_exec_full(int argc, char **argv) {
  char full[PATH_MAX];
  full[0] = '\0';

  /*
   * 0) Explicit override: the test runner (and anyone else who already
   * knows the exact build) sets NYTRIX_NY_FULL_BIN to the ny-full sibling
   * of the --bin it was configured with. Honor that before guessing from
   * argv[0]/proc-self-exe or falling back to a PATH search, so fixture
   * children always exec the build the caller actually asked for instead
   * of whatever "ny-full" happens to resolve to on the host.
   */
  {
    const char *override = getenv("NYTRIX_NY_FULL_BIN");
    if (override && override[0]) {
#ifndef _WIN32
      if (access(override, X_OK) == 0) {
        int fresh = ny_check_fresh(override);
        if (fresh != 0)
          return fresh;
        execv(override, argv);
      }
#else
      errno = 0;
      int rc = _spawnv(_P_WAIT, override, (const char *const *)argv);
      if (rc >= 0 || errno == 0)
        return rc;
#endif
    }
  }

#ifndef _WIN32
  /*
   * 1) dirname(argv[0])/ny-full
   */
  if (argv[0] && argv[0][0] &&
      ny_full_path_from(argv[0], full, sizeof(full)) == 0 &&
      access(full, X_OK) == 0) {
    int fresh = ny_check_fresh(full);
    if (fresh != 0)
      return fresh;
    execv(full, argv);
  }
  if (argv[0] && argv[0][0] &&
      ny_full_debug_path_from(argv[0], full, sizeof(full)) == 0 &&
      access(full, X_OK) == 0) {
    int fresh = ny_check_fresh(full);
    if (fresh != 0)
      return fresh;
    execv(full, argv);
  }
  /*
   * 2) dirname(/proc/self/exe)/ny-full — robust when invoked via PATH
   */
  {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
      self[n] = '\0';
      if (ny_full_path_from(self, full, sizeof(full)) == 0 &&
          access(full, X_OK) == 0) {
        int fresh = ny_check_fresh(full);
        if (fresh != 0)
          return fresh;
        execv(full, argv);
      }
      if (ny_full_debug_path_from(self, full, sizeof(full)) == 0 &&
          access(full, X_OK) == 0) {
        int fresh = ny_check_fresh(full);
        if (fresh != 0)
          return fresh;
        execv(full, argv);
      }
    }
  }
  /*
   * 3) PATH search
   */
  execvp("ny-full", argv);
  execvp("ny-full_debug", argv);
  fprintf(stderr, "ny: failed to exec ny-full: %s\n", strerror(errno));
  (void)argc;
  return 127;
#else
  /*
   * Windows has no exec replacement primitive.  _execv may report success
   * once ny-full has been launched, which loses its eventual compiler exit
   * status to callers such as ny-test.  Spawn synchronously instead so `ny`
   * remains a transparent launcher.
   */
  if (argv[0] && argv[0][0] &&
      ny_full_path_from(argv[0], full, sizeof(full)) == 0) {
    errno = 0;
    int rc = _spawnv(_P_WAIT, full, (const char *const *)argv);
    if (rc >= 0 || errno == 0)
      return rc;
  }
  if (argv[0] && argv[0][0] &&
      ny_full_debug_path_from(argv[0], full, sizeof(full)) == 0) {
    errno = 0;
    int rc = _spawnv(_P_WAIT, full, (const char *const *)argv);
    if (rc >= 0 || errno == 0)
      return rc;
  }
  {
    char self[PATH_MAX];
    DWORD n = GetModuleFileNameA(NULL, self, (DWORD)sizeof(self));
    if (n > 0 && n < sizeof(self) &&
        ny_full_path_from(self, full, sizeof(full)) == 0) {
      errno = 0;
      int rc = _spawnv(_P_WAIT, full, (const char *const *)argv);
      if (rc >= 0 || errno == 0)
        return rc;
    }
    if (n > 0 && n < sizeof(self) &&
        ny_full_debug_path_from(self, full, sizeof(full)) == 0) {
      errno = 0;
      int rc = _spawnv(_P_WAIT, full, (const char *const *)argv);
      if (rc >= 0 || errno == 0)
        return rc;
    }
  }
  errno = 0;
  int rc = _spawnvp(_P_WAIT, "ny-full.exe", (const char *const *)argv);
  if (rc >= 0 || errno == 0)
    return rc;
  errno = 0;
  rc = _spawnvp(_P_WAIT, "ny-full_debug.exe", (const char *const *)argv);
  if (rc >= 0 || errno == 0)
    return rc;
  fprintf(stderr,
          "ny: failed to exec ny-full (place ny-full.exe next to ny)\n");
  (void)argc;
  return 127;
#endif
}

int main(int argc, char **argv) {
  const char *expr = NULL;
  if (ny_pure_native_c_match(argc, argv, &expr)) {
    int64_t v = 0;
    if (ny_pure_expr_eval(expr, &v))
      return (int)((uint64_t)v & 0xffu);
    /*
     * Malformed "pure" match edge → full compiler (may still reject).
     */
  }
  if (argc == 2 && argv[1] && strcmp(argv[1], "--version-id") == 0) {
    /*
     * Report both sides of the dispatch pair so a stale install is obvious
     * without running anything.
     */
    char full[PATH_MAX];
    char theirs[1024];
    const char *target = NULL;
    const char *override = getenv("NYTRIX_NY_FULL_BIN");
    printf("ny %s\n", ny_version_id());
    if (override && override[0])
      target = override;
    else if (argv[0] && argv[0][0] &&
             ny_full_path_from(argv[0], full, sizeof(full)) == 0)
      target = full;
    if ((!target || !target[0])) {
#ifndef _WIN32
      char self[PATH_MAX];
      ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
      if (n > 0) {
        self[n] = '\0';
        if (ny_full_path_from(self, full, sizeof(full)) == 0)
          target = full;
      }
#else
      (void)full;
#endif
    }
    if (target && target[0] &&
        ny_read_target_id(target, theirs, sizeof(theirs)) == 0)
      printf("ny-full %s\n", theirs);
    else
      printf("ny-full unknown (predates --version-id or unreachable)\n");
    return 0;
  }
  int native_tool_rc = ny_exec_native_tool(argc, argv);
#ifdef _WIN32
  /*
   * Windows uses spawn, tagged above so child exit 0 is still distinguishable
   * from an invocation that should fall through to ny-full.
   */
  if (native_tool_rc >= 256)
    return native_tool_rc - 256;
  if (native_tool_rc != 0)
    return native_tool_rc;
#else
  /*
   * POSIX exec replaces this process; a nonzero return means a real error.
   */
  if (native_tool_rc != 0)
    return native_tool_rc;
#endif
  return ny_exec_full(argc, argv);
}
