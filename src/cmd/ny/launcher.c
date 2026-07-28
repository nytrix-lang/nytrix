/* Thin ny entry: pure native-only -c arithmetic never loads LLVM/Z3/clang.
 * Everything else execs the full compiler (ny-full) next to this binary. */
#include "cmd/ny/pureexpr.h"

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

/* Build path to ny-full beside `self` (mutates a copy of self for dirname). */
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

/* Debug/sanitizer builds retain their suffixed executable names. */
static int ny_full_debug_path_from(const char *self, char *out, size_t out_len) {
  if (!self || !self[0] || !out || out_len < 22)
    return -1;
  char tmp[PATH_MAX];
  if (snprintf(tmp, sizeof(tmp), "%s", self) >= (int)sizeof(tmp))
    return -1;
#ifndef _WIN32
  char *dir = dirname(tmp);
  if (!dir || !*dir)
    return -1;
  return snprintf(out, out_len, "%s/ny-full_debug", dir) >= (int)out_len ? -1 : 0;
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
    return snprintf(out, out_len, "%s\\ny-full_debug.exe", tmp) >= (int)out_len ? -1
                                                                                : 0;
  return snprintf(out, out_len, "ny-full_debug.exe") >= (int)out_len ? -1 : 0;
#endif
}

static int ny_exec_full(int argc, char **argv) {
  char full[PATH_MAX];
  full[0] = '\0';

#ifndef _WIN32
  /* 1) dirname(argv[0])/ny-full */
  if (argv[0] && argv[0][0] &&
      ny_full_path_from(argv[0], full, sizeof(full)) == 0) {
    execv(full, argv);
  }
  if (argv[0] && argv[0][0] &&
      ny_full_debug_path_from(argv[0], full, sizeof(full)) == 0) {
    execv(full, argv);
  }
  /* 2) dirname(/proc/self/exe)/ny-full — robust when invoked via PATH */
  {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
      self[n] = '\0';
      if (ny_full_path_from(self, full, sizeof(full)) == 0)
        execv(full, argv);
      if (ny_full_debug_path_from(self, full, sizeof(full)) == 0)
        execv(full, argv);
    }
  }
  /* 3) PATH search */
  execvp("ny-full", argv);
  execvp("ny-full_debug", argv);
  fprintf(stderr, "ny: failed to exec ny-full: %s\n", strerror(errno));
  (void)argc;
  return 127;
#else
  /* Windows has no exec replacement primitive.  _execv may report success
   * once ny-full has been launched, which loses its eventual compiler exit
   * status to callers such as ny-test.  Spawn synchronously instead so `ny`
   * remains a transparent launcher. */
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
  fprintf(stderr, "ny: failed to exec ny-full (place ny-full.exe next to ny)\n");
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
    /* Malformed "pure" match edge → full compiler (may still reject). */
  }
  return ny_exec_full(argc, argv);
}
