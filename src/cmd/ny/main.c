#include "base/common.h"
#include "base/args.h"
#include "base/intern.h"
#include "base/loader.h"
#include "base/options.h"
#include "base/util.h"
#include "code/jit.h"
#include "cmd/ny/pkg.h"
#include "cmd/ny-tools/fmt.h"
#include "cmd/ny-tools/make.h"
#include "cmd/ny-tools/perf.h"
#include "cmd/ny-tools/test.h"
#include "cmd/ny-tools/web.h"
#include "parse/parser.h"
#include "rt/shared.h"
#include "wire/pipe.h"
#ifndef _WIN32
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#endif

extern int64_t rt_trace_dump(int64_t n);
extern int64_t rt_trace_get_call_stack(int64_t *fn, int64_t *f, int64_t *l, int count);
extern int g_trace_requested;
#ifdef _WIN32
extern int64_t rt_enable_vt(void);
#endif

#ifndef _WIN32
extern char **environ;
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define NY_HAS_ASAN 1
#endif
#endif

#ifdef NY_HAS_ASAN
const char *__lsan_default_suppressions() {
  return "leak:rt_malloc\n"
         "leak:rt_to_str\n"
         "leak:ny_strdup\n"
         "leak:vec_push_impl\n"
         "leak:LLVM\n"
         "leak:libnytrixrt.so\n"
         "leak:^rt_malloc$\n"
         "leak:^__flt_box_val$\n"
         "leak:^__add$\n";
}
#endif

/* Async-signal-safe string writer */
static void write_str(const char *s) {
  if (s)
    (void)write(STDERR_FILENO, s, strlen(s));
}

/* Async-signal-safe decimal writer */
static void write_dec(int64_t v) {
  if (v == 0) {
    write_str("0");
    return;
  }
  if (v < 0) {
    write_str("-");
    v = -v;
  }
  char buf[24];
  int i = 22;
  buf[23] = '\0';
  while (v > 0 && i >= 0) {
    buf[i--] = (char)('0' + (v % 10));
    v /= 10;
  }
  write_str(&buf[i + 1]);
}

#ifndef _WIN32
static void handle_timeout(int sig);
#endif

static void write_ny_signal_frame(int64_t file, int64_t line, int64_t col, int64_t func) {
  const char *file_ptr = (const char *)(uintptr_t)file;
  const char *func_ptr = (const char *)(uintptr_t)func;
  if (!file_ptr || !*file_ptr)
    return;
  write_str("  at ");
  write_str(file_ptr);
  write_str(":");
  write_dec(is_int(line) ? rt_untag_v(line) : 0);
  if (is_int(col)) {
    write_str(":");
    write_dec(rt_untag_v(col));
  }
  if (func_ptr && *func_ptr) {
    write_str(" in ");
    write_str(func_ptr);
  }
  write_str("\n");
}

static void write_ny_signal_repeat(int count) {
  if (count <= 0)
    return;
  write_str(clr(NY_CLR_GRAY));
  write_str("  ... previous frame repeated ");
  write_dec(count);
  write_str(count == 1 ? " more time\n" : " more times\n");
  write_str(clr(NY_CLR_RESET));
}

static void handle_segv(int sig) {
  signal(sig, SIG_DFL);

  const char *name = (sig == SIGSEGV)   ? "SegmentationFault"
                     : (sig == SIGABRT) ? "AbortError"
                     : (sig == SIGFPE)  ? "FloatingPointError"
#ifdef SIGBUS
                     : (sig == SIGBUS)  ? "BusError"
#endif
                     : (sig == SIGILL)  ? "IllegalInstructionError"
                                        : "SignalError";

  /* System backtrace entirely removed. backtrace() internally calls into
   * libgcc which attempts to allocate memory, causing a double-fault
   * crash when the heap is corrupted, preventing the Nytrix trace
   * from ever printing. */

  int64_t files[32], lines[32], cols[32], funcs[32];
  extern int64_t rt_trace_get_frames(int64_t *f, int64_t *l, int64_t *c, int64_t *fn, int count);
  int count = rt_trace_get_frames(files, lines, cols, funcs, 16);
  write_str("\nNytrix trace (most recent call last):\n");
  if (count == 0) {
    write_str(clr(NY_CLR_GRAY));
    write_str("  <trace unavailable; run with -trace for frames>\n");
    write_str(clr(NY_CLR_RESET));
  }
  int64_t last_file = 0, last_line = 0, last_func = 0;
  int repeats = 0;
  int have_last = 0;
  for (int i = count - 1; i >= 0; i--) {
    if (have_last && files[i] == last_file && lines[i] == last_line && funcs[i] == last_func) {
      repeats++;
      continue;
    }
    write_ny_signal_repeat(repeats);
    repeats = 0;
    write_ny_signal_frame(files[i], lines[i], cols[i], funcs[i]);
    last_file = files[i];
    last_line = lines[i];
    last_func = funcs[i];
    have_last = 1;
  }
  write_ny_signal_repeat(repeats);
  write_str(clr(NY_CLR_RED));
  write_str(name);
  write_str(clr(NY_CLR_RESET));
  write_str(": signal ");
  write_dec(sig);
  write_str("\n");
  _exit(128 + sig);
}

static void ny_install_signal_handlers(void) {
#ifndef _WIN32
  /* Use a dedicated alternate signal stack so the handler can run even when
   * the main stack or heap is corrupted. */
  (void)signal(SIGPIPE, SIG_IGN);
  static char g_sigstack[65536];
  stack_t ss = {.ss_sp = g_sigstack, .ss_size = sizeof(g_sigstack), .ss_flags = 0};
  (void)sigaltstack(&ss, NULL);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_segv;
  sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
  sigfillset(&sa.sa_mask);
  (void)sigaction(SIGSEGV, &sa, NULL);
  (void)sigaction(SIGABRT, &sa, NULL);
  (void)sigaction(SIGFPE, &sa, NULL);
#ifdef SIGBUS
  (void)sigaction(SIGBUS, &sa, NULL);
#endif
  (void)sigaction(SIGILL, &sa, NULL);
  (void)signal(SIGALRM, handle_timeout);
#else
  signal(SIGSEGV, handle_segv);
  signal(SIGABRT, handle_segv);
  signal(SIGFPE, handle_segv);
  signal(SIGILL, handle_segv);
#endif
}

static void ny_setenv_force(const char *key, const char *value) {
  if (!key || !*key || !value)
    return;
#ifdef _WIN32
  (void)_putenv_s(key, value);
#else
  (void)ny_setenv(key, value, 1);
#endif
}

static void ny_setenv_default(const char *key, const char *value) {
  if (!key || !*key || !value)
    return;
  const char *cur = getenv(key);
  if (!cur || !*cur)
    ny_setenv_force(key, value);
}

static void ny_setenv_force_many(const char *key, ...) {
  va_list ap;
  va_start(ap, key);
  while (key) {
    const char *value = va_arg(ap, const char *);
    if (!value)
      break;
    ny_setenv_force(key, value);
    key = va_arg(ap, const char *);
  }
  va_end(ap);
}

static void ny_setenv_default_many(const char *key, ...) {
  va_list ap;
  va_start(ap, key);
  while (key) {
    const char *value = va_arg(ap, const char *);
    if (!value)
      break;
    ny_setenv_default(key, value);
    key = va_arg(ap, const char *);
  }
  va_end(ap);
}

static void ny_env_append_unique(const char *name, const char *extra) {
  if (!name || !*name || !extra || !*extra)
    return;
  const char *cur = getenv(name);
  if (!cur || !*cur) {
    ny_setenv_force(name, extra);
    return;
  }
  if (strstr(cur, extra) != NULL)
    return;
  size_t ncur = strlen(cur);
  size_t nextra = strlen(extra);
  char *buf = (char *)malloc(ncur + 1 + nextra + 1);
  if (!buf)
    return;
  memcpy(buf, cur, ncur);
  buf[ncur] = ' ';
  memcpy(buf + ncur + 1, extra, nextra);
  buf[ncur + 1 + nextra] = '\0';
  ny_setenv_force(name, buf);
  free(buf);
}

static void ny_env_append_semicolon_unique(const char *name, const char *extra) {
  if (!name || !*name || !extra || !*extra)
    return;
  const char *cur = getenv(name);
  if (!cur || !*cur) {
    ny_setenv_force(name, extra);
    return;
  }
  if (strstr(cur, extra) != NULL)
    return;
  size_t ncur = strlen(cur);
  size_t nextra = strlen(extra);
  char *buf = (char *)malloc(ncur + 1 + nextra + 1);
  if (!buf)
    return;
  memcpy(buf, cur, ncur);
  buf[ncur] = ';';
  memcpy(buf + ncur + 1, extra, nextra);
  buf[ncur + 1 + nextra] = '\0';
  ny_setenv_force(name, buf);
  free(buf);
}

static bool ny_config_key_ok(const char *s, size_t n) {
  if (!s || n == 0)
    return false;
  unsigned char c0 = (unsigned char)s[0];
  if (!(isalpha(c0) || c0 == '_'))
    return false;
  for (size_t i = 1; i < n; ++i) {
    unsigned char c = (unsigned char)s[i];
    if (!(isalnum(c) || c == '_'))
      return false;
  }
  return true;
}

static char *ny_trim_span_dup(const char *s, size_t n) {
  while (n > 0 && isspace((unsigned char)*s)) {
    s++;
    n--;
  }
  while (n > 0 && isspace((unsigned char)s[n - 1]))
    n--;
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static void ny_strip_matching_quotes(char *s) {
  if (!s)
    return;
  size_t n = strlen(s);
  if (n < 2)
    return;
  if ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\'')) {
    memmove(s, s + 1, n - 2);
    s[n - 2] = '\0';
  }
}

static char *ny_config_expand_value(const char *value) {
  if (!value)
    return NULL;
  if (value[0] != '~' || (value[1] != '/' && value[1] != '\\' && value[1] != '\0'))
    return ny_strdup(value);
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (!home || !*home)
    home = getenv("APPDATA");
#else
  const char *home = getenv("HOME");
#endif
  if (!home || !*home)
    return ny_strdup(value);
  size_t nh = strlen(home);
  size_t nv = strlen(value);
  char *out = (char *)malloc(nh + nv);
  if (!out)
    return NULL;
  memcpy(out, home, nh);
  memcpy(out + nh, value + 1, nv);
  out[nh + nv - 1] = '\0';
  return out;
}

static void ny_config_path_join(char *out, size_t out_sz, const char *a, const char *b) {
  if (!out || out_sz == 0)
    return;
  if (!a || !*a) {
    snprintf(out, out_sz, "%s", b ? b : "");
    return;
  }
  size_t n = strlen(a);
  const char sep =
#ifdef _WIN32
      '\\';
#else
      '/';
#endif
  if (n > 0 && (a[n - 1] == '/' || a[n - 1] == '\\'))
    snprintf(out, out_sz, "%s%s", a, b ? b : "");
  else
    snprintf(out, out_sz, "%s%c%s", a, sep, b ? b : "");
}

static void ny_config_home(char *out, size_t out_sz) {
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    snprintf(out, out_sz, "%s", xdg);
    return;
  }
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (!home || !*home)
    home = getenv("APPDATA");
#else
  const char *home = getenv("HOME");
#endif
  if (home && *home)
    ny_config_path_join(out, out_sz, home, ".config");
  else
    out[0] = '\0';
}

static void ny_config_candidate(int idx, char *out, size_t out_sz) {
  out[0] = '\0';
  if (idx == 0) {
    const char *explicit_path = getenv("NYTRIX_CONFIG");
    if (!explicit_path || !*explicit_path)
      explicit_path = getenv("NY_CONFIG");
    snprintf(out, out_sz, "%s", explicit_path && *explicit_path ? explicit_path : "");
    return;
  }
  if (idx == 1) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd))) {
      char dir[4096];
      ny_config_path_join(dir, sizeof(dir), cwd, ".nytrix");
      ny_config_path_join(out, out_sz, dir, "config");
    }
    return;
  }
  if (idx == 2) {
    char cwd[4096];
    if (getcwd(cwd, sizeof(cwd)))
      ny_config_path_join(out, out_sz, cwd, "nytrix.config");
    return;
  }
  char base[4096];
  ny_config_home(base, sizeof(base));
  if (!base[0])
    return;
  if (idx == 3) {
    char dir[4096];
    ny_config_path_join(dir, sizeof(dir), base, "nytrix");
    ny_config_path_join(out, out_sz, dir, "config");
  } else if (idx == 4) {
    char dir[4096];
    ny_config_path_join(dir, sizeof(dir), base, "ny");
    ny_config_path_join(out, out_sz, dir, "config");
  }
}

static void ny_load_config_file(const char *path) {
  if (!path || !*path)
    return;
  char *txt = ny_read_file(path);
  if (!txt)
    return;
  bool loaded = false;
  char *p = txt;
  while (*p) {
    char *line = p;
    while (*p && *p != '\n')
      p++;
    size_t n = (size_t)(p - line);
    if (*p == '\n')
      p++;
    char *s = ny_trim_span_dup(line, n);
    if (!s)
      continue;
    if (!*s || *s == '#' || *s == ';') {
      free(s);
      continue;
    }
    loaded = true;
    if (strncmp(s, "export ", 7) == 0) {
      char *trimmed = ny_trim_span_dup(s + 7, strlen(s + 7));
      free(s);
      s = trimmed;
      if (!s)
        continue;
    }
    char *eq = strchr(s, '=');
    if (!eq) {
      free(s);
      continue;
    }
    char *key = ny_trim_span_dup(s, (size_t)(eq - s));
    char *val = ny_trim_span_dup(eq + 1, strlen(eq + 1));
    if (key && val && ny_config_key_ok(key, strlen(key))) {
      ny_strip_matching_quotes(val);
      char *expanded = ny_config_expand_value(val);
      ny_setenv_default(key, expanded ? expanded : val);
      free(expanded);
    }
    free(key);
    free(val);
    free(s);
  }
  if (loaded)
    ny_env_append_semicolon_unique("NYTRIX_CONFIG_LOADED", path);
  free(txt);
}

static void ny_load_default_config(void) {
  for (int i = 0; i < 5; ++i) {
    char path[4096];
    ny_config_candidate(i, path, sizeof(path));
    if (path[0])
      ny_load_config_file(path);
  }
}

static void ny_global_cleanup(void) {
  parser_global_cleanup();
  ny_intern_cleanup();
}

static void ny_unsetenv_force(const char *key) {
  if (!key || !*key)
    return;
#ifdef _WIN32
  (void)_putenv_s(key, "");
#else
  (void)ny_unsetenv(key);
#endif
}

static void ny_unsetenv_keys(const char *const *keys) {
  for (size_t i = 0; keys && keys[i]; ++i)
    ny_unsetenv_force(keys[i]);
}

typedef struct {
  int writes;
} ny_env_config_t;

static void ny_env_config_set(ny_env_config_t *cfg, const char *key, const char *value) {
  ny_setenv_force(key, value);
  if (cfg && key && *key && value)
    cfg->writes++;
}

static void ny_env_config_unset(ny_env_config_t *cfg, const char *key) {
  ny_unsetenv_force(key);
  if (cfg && key && *key)
    cfg->writes++;
}

static void ny_env_config_set_bool(ny_env_config_t *cfg, const char *key, bool value) {
  ny_env_config_set(cfg, key, value ? "1" : "0");
}

static void ny_env_config_set_int(ny_env_config_t *cfg, const char *key, int value) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", value);
  ny_env_config_set(cfg, key, buf);
}

static void ny_clear_policy_env_overrides(void) {
  static const char *const keys[] = {"NYTRIX_EFFECT_REQUIRE_KNOWN",
                                     "NYTRIX_ALIAS_REQUIRE_KNOWN",
                                     "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE",
                                     "NYTRIX_GPU_MODE",
                                     "NYTRIX_GPU_BACKEND",
                                     "NYTRIX_GPU_OFFLOAD",
                                     "NYTRIX_GPU_MIN_WORK",
                                     "NYTRIX_GPU_ASYNC",
                                     "NYTRIX_GPU_FAST_MATH",
                                     "NYTRIX_ACCEL_TARGET",
                                     "NYTRIX_ACCEL_OBJECT",
                                     "NYTRIX_PARALLEL_MODE",
                                     "NYTRIX_PARALLEL_THREADS",
                                     "NYTRIX_PARALLEL_MIN_WORK",
                                     "NYTRIX_GPROF",
                                     "NYTRIX_STD_BUILTIN_OPS",
                                     "NYTRIX_GC",
                                     "NYTRIX_HEAP_POLICY",
                                     "NYTRIX_RC_GC",
                                     "NYTRIX_OWNERSHIP",
                                     "NYTRIX_OWNERSHIP_STRICT",
                                     "NYTRIX_DEBUG_LOCALS",
                                     "NYTRIX_DWARF_VERSION",
                                     "NYTRIX_DWARF_SPLIT_INLINING",
                                     "NYTRIX_DWARF_PROFILE_INFO",
                                     NULL};
  ny_unsetenv_keys(keys);
}

static char **ny_current_envp(char **fallback_envp) {
#ifdef _WIN32
  return _environ ? _environ : fallback_envp;
#else
  return environ ? environ : fallback_envp;
#endif
}

static bool ny_input_file_looks_like_engine(const char *path) {
  if (!path || !*path)
    return false;
  const char *base = path;
  for (const char *p = path; *p; ++p) {
    if (*p == '/' || *p == '\\')
      base = p + 1;
  }
  return strcmp(base, "engine.ny") == 0;
}

static char *ny_join_path_file(const char *dir, const char *file) {
  if (!dir || !*dir || !file || !*file)
    return NULL;
  size_t nd = strlen(dir);
  size_t nf = strlen(file);
  bool need_sep = nd > 0 && dir[nd - 1] != '/' && dir[nd - 1] != '\\';
  char *out = (char *)malloc(nd + (need_sep ? 1 : 0) + nf + 1);
  if (!out)
    return NULL;
  memcpy(out, dir, nd);
  size_t pos = nd;
  if (need_sep)
    out[pos++] = '/';
  memcpy(out + pos, file, nf);
  out[pos + nf] = '\0';
  return out;
}

static char *ny_commas_to_pipes(const char *raw) {
  if (!raw || !*raw)
    return NULL;
  size_t n = strlen(raw);
  char *out = (char *)malloc(n + 1);
  if (!out)
    return NULL;
  for (size_t i = 0; i < n; ++i)
    out[i] = raw[i] == ',' ? '|' : raw[i];
  out[n] = '\0';
  return out;
}

static char *ny_ui_default_gui_dump_path(const char *shot) {
  const char *prefix = "fb/ui_gui_";
  const char *suffix = ".png";
  if (!shot || !*shot)
    shot = "shot";
  size_t np = strlen(prefix);
  size_t ns = strlen(shot);
  size_t nx = strlen(suffix);
  char *out = (char *)malloc(np + ns + nx + 1);
  if (!out)
    return NULL;
  memcpy(out, prefix, np);
  for (size_t i = 0; i < ns; ++i) {
    char c = shot[i];
    out[np + i] = (c == '/' || c == '\\' || c == ':' || c == ' ') ? '_' : c;
  }
  memcpy(out + np + ns, suffix, nx);
  out[np + ns + nx] = '\0';
  char *path = ny_join_path_file(ny_default_cache_root_dir(), out);
  free(out);
  return path;
}

static char *ny_ui_default_profile_dir(void) {
  char buf[160];
#ifdef _WIN32
  int pid = _getpid();
#else
  int pid = (int)getpid();
#endif
  snprintf(buf, sizeof(buf), "profiles/native_%d", pid);
  return ny_join_path_file(ny_default_cache_root_dir(), buf);
}

static int ny_mkdir_one(const char *path) {
  if (!path || !*path)
    return 0;
#ifdef _WIN32
  if (_mkdir(path) == 0 || errno == EEXIST)
    return 1;
#else
  if (mkdir(path, 0775) == 0 || errno == EEXIST)
    return 1;
#endif
  return 0;
}

static void ny_mkdir_p(const char *path) {
  if (!path || !*path)
    return;
  char *buf = strdup(path);
  if (!buf)
    return;
  size_t n = strlen(buf);
  while (n > 1 && (buf[n - 1] == '/' || buf[n - 1] == '\\'))
    buf[--n] = '\0';
  char *p = buf;
  if (p[0] == '/' || p[0] == '\\')
    p++;
#ifdef _WIN32
  if (n >= 3 && buf[1] == ':' && (buf[2] == '/' || buf[2] == '\\'))
    p = buf + 3;
#endif
  for (; *p; ++p) {
    if (*p != '/' && *p != '\\')
      continue;
    char sep = *p;
    *p = '\0';
    if (buf[0] != '\0')
      (void)ny_mkdir_one(buf);
    *p = sep;
  }
  (void)ny_mkdir_one(buf);
  free(buf);
}

static void ny_mkdir_p_parent(const char *path) {
  if (!path || !*path)
    return;
  char *buf = strdup(path);
  if (!buf)
    return;
  char *last = NULL;
  for (char *p = buf; *p; ++p) {
    if (*p == '/' || *p == '\\')
      last = p;
  }
  if (last && last != buf) {
    *last = '\0';
    ny_mkdir_p(buf);
  }
  free(buf);
}

static void ny_setenv_keyval(const char *raw) {
  if (!raw || !*raw)
    return;
  const char *eq = strchr(raw, '=');
  if (!eq || eq == raw)
    return;
  size_t nk = (size_t)(eq - raw);
  char *key = (char *)malloc(nk + 1);
  if (!key)
    return;
  memcpy(key, raw, nk);
  key[nk] = '\0';
  ny_setenv_force(key, eq + 1);
  free(key);
}

static void ny_enable_ui_auto_dump_env(void) {
  ny_setenv_force_many("NYTRIX_AUTO_DUMP", "1", "NYTRIX_AUTO_DUMP_IMMEDIATE", "1",
                       "NYTRIX_AUTO_DUMP_EXIT", "1", "NYTRIX_FAST", "0", NULL);
  ny_setenv_default_many("NYTRIX_AUTO_DUMP_MIN_ELAPSED_SEC", "0", "NY_PNG_ENCODE_LEVEL", "1",
                         NULL);
}

static void ny_enable_ui_profile_env(const char *profile_dir, bool render_trace) {
  ny_setenv_force_many("NY_SCENE_PROFILE_TRACE", "1", "NY_UI_PROFILE_TRACE", "1",
                       "NY_UI_PROFILE_DEEP", "1", "NY_UI_PROFILE_DUMP", "1",
                       "NY_VK_PROFILE_TRACE", "1", "NY_VK_PROFILE_DUMP", "1", NULL);
  if (profile_dir && *profile_dir) {
    char *ui_path = ny_join_path_file(profile_dir, "ui_profile.jsonl");
    char *vk_path = ny_join_path_file(profile_dir, "vk_profile.jsonl");
    if (ui_path) {
      ny_setenv_force("NY_UI_PROFILE_DUMP_PATH", ui_path);
      free(ui_path);
    }
    if (vk_path) {
      ny_setenv_force("NY_VK_PROFILE_DUMP_PATH", vk_path);
      free(vk_path);
    }
  }
  if (render_trace) {
    ny_setenv_force("NYTRIX_VK_MARKERS", "1");
    ny_setenv_default_many("NY_UI_FRAME_PRINT_EVERY", "1", "NY_VK_PROFILE_EVERY", "1", NULL);
  } else {
    ny_setenv_default_many("NY_UI_FRAME_PRINT_EVERY", "120", "NY_VK_PROFILE_EVERY", "120", NULL);
  }
}

static void ny_enable_ui_nosurface_headless_env(void) {
  static const char *const unset[] = {"DISPLAY", "WAYLAND_DISPLAY", NULL};
  ny_setenv_force_many("NY_UI_HEADLESS", "1", "NY_UI_BACKEND", "none",
                       "NYTRIX_VK_ALLOW_HEADLESS", "1", NULL);
  ny_unsetenv_keys(unset);
}

static void ny_enable_ui_surface_headless_env(void) {
  ny_setenv_force_many("NY_UI_HEADLESS", "1", "NYTRIX_VK_ALLOW_HEADLESS", "0", NULL);
  ny_setenv_default_many("NY_UI_HEADLESS_GUI", "1", "NY_UI_HEADLESS_MATCH_WINDOW", "1", NULL);
}

static void ny_enable_ui_nosurface_bench_env(bool profile) {
  ny_enable_ui_nosurface_headless_env();
  ny_setenv_force_many("NY_UI_REAL_HEADLESS_SIM", "1", "NY_UI_BENCH", "1", "NY_UI_FPS_LOG",
                       profile ? "0" : "1", NULL);
  if (profile)
    ny_setenv_force("NY_UI_BENCH_PROFILE", "1");
  ny_setenv_default_many("NY_UI_MSAA", "4", "NYTRIX_VK_NOSURFACE_LOAD_PASS", "1",
                         "NYTRIX_VK_NOSURFACE_REPLAY", "1", NULL);
}

typedef struct {
  const char *dump_path;
  const char *dump_dir;
  const char *dump_models;
  const char *gui_shot;
  const char *profile_dir;
  bool want_dump;
  bool frame_hash;
  bool frame_hash_no_skybox;
  bool dump_all;
  bool dump_missing;
  bool ms_profile;
  bool render_trace;
  bool nosurface_profile;
  bool post_autofit;
  bool post_lookat;
  char *default_dump_path;
  char *default_dump_dir;
  char *default_profile_dir;
} ny_ui_bridge_state_t;

static int ny_ui_bridge_take_value(const char *a, int *i, int argc, char **argv,
                                   const char **out, const char *fallback_error) {
  char err[160];
  err[0] = '\0';
  if (ny_arg_take_value(a, i, argc, argv, out, err, sizeof(err)))
    return 0;
  fprintf(stderr, "ny: %s\n", err[0] ? err : fallback_error);
  return 2;
}

static bool ny_ui_bridge_handle_toggle(ny_ui_bridge_state_t *st, const char *a) {
  if (ny_arg_match(a, "--dump", NULL)) {
    st->want_dump = true;
    return true;
  }
  if (ny_arg_match(a, "--frame-hash", NULL) || ny_arg_match(a, "--fbhash", NULL)) {
    st->want_dump = true;
    st->frame_hash = true;
    return true;
  }
  if (ny_arg_match(a, "--frame-hash-no-skybox", NULL)) {
    st->frame_hash_no_skybox = true;
    return true;
  }
  if (ny_arg_match(a, "--dump-all", NULL)) {
    st->dump_all = true;
    return true;
  }
  if (ny_arg_match(a, "--dump-missing", NULL)) {
    st->dump_missing = true;
    return true;
  }
  if (ny_arg_match(a, "--fb", NULL)) {
    st->dump_all = true;
    ny_enable_ui_surface_headless_env();
    return true;
  }
  if (ny_arg_match(a, "--ms-profile", NULL) || ny_arg_match(a, "--msprofile", NULL) ||
      ny_arg_match(a, "--profile-ms", NULL)) {
    st->ms_profile = true;
    return true;
  }
  if (ny_arg_match(a, "--render-trace", NULL)) {
    st->render_trace = true;
    return true;
  }
  if (ny_arg_match(a, "--frame-trace", NULL)) {
    ny_setenv_force("NY_GFX_FRAME_TRACE", "1");
    return true;
  }
  if (ny_arg_match(a, "--autofit", NULL)) {
    st->post_autofit = true;
    return true;
  }
  if (ny_arg_match(a, "--lookat", NULL)) {
    st->post_lookat = true;
    return true;
  }
  if (ny_arg_match(a, "--validation", NULL)) {
    ny_setenv_force("NY_VK_VALIDATION", "1");
    return true;
  }
  if (ny_arg_match(a, "--headless", NULL)) {
    ny_enable_ui_nosurface_headless_env();
    return true;
  }
  if (ny_arg_match(a, "--headless-sim", NULL) || ny_arg_match(a, "--real-headless", NULL)) {
    ny_enable_ui_nosurface_bench_env(false);
    return true;
  }
  if (ny_arg_match(a, "--nosurface-profile", NULL)) {
    ny_enable_ui_nosurface_bench_env(true);
    st->nosurface_profile = true;
    return true;
  }
  if (ny_arg_match(a, "--compare-headless", NULL) ||
      ny_arg_match(a, "--surfaced-headless", NULL)) {
    ny_enable_ui_surface_headless_env();
    return true;
  }
  if (ny_arg_match(a, "--fast-material-batch", NULL) || ny_arg_match(a, "--fast-batch", NULL)) {
    ny_setenv_force("NY_UI_VERTEX_MATERIAL_BATCH", "1");
    return true;
  }
  return false;
}

static int ny_ui_bridge_handle_value(ny_ui_bridge_state_t *st, const char *a, int *i, int argc,
                                     char **argv, bool *handled) {
  const char *v = NULL;
  *handled = false;
  if (ny_arg_match_with_value(a, "--set")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --set");
    if (rc != 0)
      return rc;
    ny_setenv_keyval(v);
    return 0;
  }
  if (ny_arg_match_with_value(a, "--timeout")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --timeout");
    if (rc != 0)
      return rc;
    ny_setenv_force("NY_UI_TIMEOUT", v);
    return 0;
  }
  if (ny_arg_match_with_value(a, "--dump-path")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --dump-path");
    if (rc != 0)
      return rc;
    st->dump_path = v;
    st->want_dump = true;
    return 0;
  }
  if (ny_arg_match_with_value(a, "--dump-dir")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --dump-dir");
    if (rc != 0)
      return rc;
    st->dump_dir = v;
    return 0;
  }
  if (ny_arg_match_with_value(a, "--dump-models")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --dump-models");
    if (rc != 0)
      return rc;
    st->dump_models = v;
    st->dump_all = true;
    return 0;
  }
  if (ny_arg_match_with_value(a, "--dump-settle-frames")) {
    *handled = true;
    int rc =
        ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --dump-settle-frames");
    if (rc != 0)
      return rc;
    ny_setenv_force("NYTRIX_AUTO_DUMP_DELAY_FRAMES", v);
    ny_setenv_force("NY_UI_BATCH_DUMP_SETTLE_FRAMES", v);
    return 0;
  }
  if (ny_arg_match_with_value(a, "--dump-png-level")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --dump-png-level");
    if (rc != 0)
      return rc;
    ny_setenv_force("NY_PNG_ENCODE_LEVEL", v);
    return 0;
  }
  if (ny_arg_match_with_value(a, "--gui-shot")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --gui-shot");
    if (rc != 0)
      return rc;
    st->gui_shot = v;
    st->want_dump = true;
    return 0;
  }
  if (ny_arg_match_with_value(a, "--gui-layout")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --gui-layout");
    if (rc != 0)
      return rc;
    ny_setenv_force("NY_UI_GUI_LAYOUT", v);
    return 0;
  }
  if (ny_arg_match_with_value(a, "--profile-dir")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for --profile-dir");
    if (rc != 0)
      return rc;
    st->profile_dir = v;
    return 0;
  }
  if (ny_arg_match_with_value(a, "--expect-frame-hash") ||
      ny_arg_match_with_value(a, "--expect-fb-hash") ||
      ny_arg_match_with_value(a, "--check-frame-hash")) {
    *handled = true;
    int rc = ny_ui_bridge_take_value(a, i, argc, argv, &v, "missing value for frame hash");
    if (rc != 0)
      return rc;
    ny_setenv_force("NY_UI_EXPECT_FRAME_HASH", v);
    st->want_dump = true;
    st->frame_hash = true;
    return 0;
  }
  return 0;
}

static void ny_ui_bridge_resolve_defaults(ny_ui_bridge_state_t *st) {
  if ((!st->dump_path || !*st->dump_path) && st->frame_hash) {
    st->default_dump_path = ny_join_path_file(ny_default_cache_root_dir(), "fb_hash/frame.png");
    if (st->default_dump_path)
      st->dump_path = st->default_dump_path;
  } else if ((!st->dump_path || !*st->dump_path) && st->gui_shot && *st->gui_shot) {
    st->default_dump_path = ny_ui_default_gui_dump_path(st->gui_shot);
    if (st->default_dump_path)
      st->dump_path = st->default_dump_path;
  } else if ((!st->dump_path || !*st->dump_path) && st->want_dump) {
    st->default_dump_path = ny_join_path_file(ny_default_cache_root_dir(), "fb/fb_dump.png");
    if (st->default_dump_path)
      st->dump_path = st->default_dump_path;
  }
  if ((!st->dump_dir || !*st->dump_dir) && st->dump_all) {
    st->default_dump_dir = ny_join_path_file(ny_default_cache_root_dir(), "fb");
    if (st->default_dump_dir)
      st->dump_dir = st->default_dump_dir;
  }
  if ((st->ms_profile || st->render_trace || st->nosurface_profile) &&
      (!st->profile_dir || !*st->profile_dir)) {
    st->default_profile_dir = ny_ui_default_profile_dir();
    if (st->default_profile_dir)
      st->profile_dir = st->default_profile_dir;
  }
}

static void ny_ui_bridge_apply_env(ny_ui_bridge_state_t *st) {
  if (st->want_dump || st->frame_hash) {
    ny_enable_ui_auto_dump_env();
    if (st->dump_path && *st->dump_path) {
      ny_mkdir_p_parent(st->dump_path);
      ny_setenv_force("NYTRIX_AUTO_DUMP_PATH", st->dump_path);
    }
  }
  if (st->frame_hash) {
    ny_setenv_force("NY_UI_PRINT_FRAME_HASH", "1");
    ny_setenv_default("NY_UI_STATIC_WORLD_FAST", "1");
    ny_setenv_default("NY_UI_FRAME_HASH_LOCK", "1");
    if (st->frame_hash_no_skybox)
      ny_setenv_force("NY_UI_PROOF_SKYBOX", "0");
    else
      ny_setenv_default("NY_UI_PROOF_SKYBOX", "1");
  }
  if (st->dump_dir && *st->dump_dir) {
    ny_mkdir_p(st->dump_dir);
    ny_setenv_force("NY_UI_DUMP_DIR", st->dump_dir);
    if (st->dump_all)
      ny_setenv_force("NY_UI_BATCH_DUMP_DIR", st->dump_dir);
  }
  if (st->dump_all) {
    ny_setenv_force("NY_UI_BATCH_DUMP_ALL", "1");
    ny_setenv_default("NY_UI_BATCH_FAST_ENV", "1");
    ny_setenv_default("NY_TEX_DISABLE_WRITE", "1");
    ny_setenv_default("NY_GLTF_PREDECODE_BATCH", "0");
    ny_setenv_default("NY_GLTF_LOAD_ANIM_FIT", "0");
    ny_setenv_default("NY_UI_DUMP_FOV", "70");
    ny_setenv_default("NY_UI_DUMP_FIT_FILL", "0.92");
    ny_setenv_default("NY_UI_DUMP_FIT_DIST_SCALE", "0.88");
  }
  if (st->dump_missing)
    ny_setenv_force("NY_UI_BATCH_DUMP_MISSING", "1");
  if (st->dump_models && *st->dump_models) {
    char *models = ny_commas_to_pipes(st->dump_models);
    if (models) {
      ny_setenv_force("NY_UI_BATCH_DUMP_LIST", models);
      free(models);
    }
  }
  if (st->gui_shot && *st->gui_shot) {
    ny_setenv_force("NY_UI_GUI_PROBE", "1");
    ny_setenv_force("NY_UI_GUI_SHOT", st->gui_shot);
    ny_setenv_force("NY_UI_GUI_AUTO_DUMP_EXIT", "1");
    if (st->dump_path && *st->dump_path)
      ny_setenv_force("NY_UI_GUI_AUTO_DUMP", st->dump_path);
  }
  if (st->profile_dir && *st->profile_dir)
    ny_mkdir_p(st->profile_dir);
  if (st->ms_profile || st->render_trace)
    ny_enable_ui_profile_env(st->profile_dir, st->render_trace);
  if (st->nosurface_profile && st->profile_dir && *st->profile_dir) {
    char *bench_path = ny_join_path_file(st->profile_dir, "bench_profile.jsonl");
    if (bench_path) {
      ny_setenv_force("NY_UI_BENCH_PROFILE_DUMP_PATH", bench_path);
      free(bench_path);
    }
  }
  if (st->post_autofit)
    ny_env_append_semicolon_unique("NY_UI_POST_LOAD_CMD", "autofit");
  if (st->post_lookat)
    ny_env_append_semicolon_unique("NY_UI_POST_LOAD_CMD", "lookat");

  ny_setenv_force("NYTRIX_RUN_ARG_BRIDGE", "1");
}

static int ny_bridge_ui_script_args_to_env(const ny_options *opt, int argc, char **argv) {
  if (!opt || !opt->input_file || !ny_input_file_looks_like_engine(opt->input_file))
    return 0;

  ny_ui_bridge_state_t st = {0};
  for (int i = opt->file_arg_idx + 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a || !*a)
      continue;
    if (ny_ui_bridge_handle_toggle(&st, a))
      continue;
    bool handled = false;
    int rc = ny_ui_bridge_handle_value(&st, a, &i, argc, argv, &handled);
    if (rc != 0)
      return rc;
    if (handled)
      continue;
  }

  ny_ui_bridge_resolve_defaults(&st);
  ny_ui_bridge_apply_env(&st);
  free(st.default_dump_path);
  free(st.default_dump_dir);
  free(st.default_profile_dir);
  return 0;
}

static void ny_apply_cli_env_config(ny_env_config_t *env, ny_options *opt, bool trace_requested) {
  if (opt->safe_mode) {
    opt->verify_module = true;
    ny_env_config_set(env, "NYTRIX_STRICT_DIAGNOSTICS", "1");
    if (opt->strict_types)
      ny_env_config_set(env, "NYTRIX_STRICT_TYPES", "1");
    ny_env_config_set(env, "NYTRIX_AUTO_PURITY", "1");
    ny_env_config_set(env, "NYTRIX_EFFECT_REQUIRE_KNOWN", "1");
    ny_env_config_set(env, "NYTRIX_ALIAS_REQUIRE_KNOWN", "1");
    ny_env_config_set(env, "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE", "1");
  }

  if (opt->trace_exec || trace_requested) {
    ny_env_config_set(env, "NYTRIX_TRACE", "1");
    g_trace_requested = 1;
    if (opt->verbose)
      fprintf(stderr, "NYTRIX: tracing active\n");
  } else {
    ny_env_config_unset(env, "NYTRIX_TRACE");
  }
  if (opt->effect_require_known)
    ny_env_config_set(env, "NYTRIX_EFFECT_REQUIRE_KNOWN", "1");
  if (opt->alias_require_known)
    ny_env_config_set(env, "NYTRIX_ALIAS_REQUIRE_KNOWN", "1");
  if (opt->alias_require_no_escape)
    ny_env_config_set(env, "NYTRIX_ALIAS_REQUIRE_NO_ESCAPE", "1");
  if (opt->strict_types)
    ny_env_config_set(env, "NYTRIX_STRICT_TYPES", "1");
  else if (opt->strict_types_explicit)
    ny_env_config_set(env, "NYTRIX_STRICT_TYPES", "0");
  if (opt->gprof >= 0)
    ny_env_config_set_bool(env, "NYTRIX_GPROF", opt->gprof != 0);
  if (opt->opt_profile)
    ny_env_config_set(env, "NYTRIX_OPT_PROFILE", opt->opt_profile);
  if (opt->opt_profile && strcmp(opt->opt_profile, "peak") == 0) {
    ny_env_append_unique(
        "NYTRIX_HOST_CFLAGS",
        "-Ofast -march=native -mtune=native -fno-math-errno -fno-trapping-math -funroll-loops");
    ny_env_append_unique("NYTRIX_HOST_LDFLAGS", "-march=native -mtune=native");
#if !(defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__)))
    ny_env_config_set(env, "NYTRIX_JIT_FAST_ISEL", "1");
#endif
    ny_env_config_set(env, "NYTRIX_JIT_CODEGEN_OPT", "aggressive");
    ny_env_config_set(env, "NYTRIX_OPT_DCE", "1");
    ny_env_config_set(env, "NYTRIX_UNSAFE_FIXNUM", "1");
    ny_env_config_set(env, "NYTRIX_RAW_INT_SLOT_EXPR_FAST", "1");
    ny_env_config_set(env, "NYTRIX_AUTO_VECTORIZE_LOOPS", "1");
  }
  if (opt->profiler_mode) {
    ny_env_config_set(env, "NYTRIX_PROFILER", "1");
    ny_env_config_set(env, "NYTRIX_JIT_PERF_MAP", "1");
    ny_env_config_set(env, "NYTRIX_MEM_STATS", "1");
  }
  if (opt->std_builtin_ops >= 0)
    ny_env_config_set_bool(env, "NYTRIX_STD_BUILTIN_OPS", opt->std_builtin_ops != 0);
  if (opt->compiler_asserts >= 0)
    ny_env_config_set_bool(env, "NYTRIX_COMPILER_ASSERTS", opt->compiler_asserts != 0);
  if (opt->debug_locals >= 0)
    ny_env_config_set_bool(env, "NYTRIX_DEBUG_LOCALS", opt->debug_locals != 0);
  if (opt->dwarf_version >= 2 && opt->dwarf_version <= 5)
    ny_env_config_set_int(env, "NYTRIX_DWARF_VERSION", opt->dwarf_version);
  if (opt->dwarf_split_inlining >= 0)
    ny_env_config_set_bool(env, "NYTRIX_DWARF_SPLIT_INLINING",
                           opt->dwarf_split_inlining != 0);
  if (opt->dwarf_profile_info >= 0)
    ny_env_config_set_bool(env, "NYTRIX_DWARF_PROFILE_INFO", opt->dwarf_profile_info != 0);
  if (opt->host_cflags)
    ny_env_config_set(env, "NYTRIX_HOST_CFLAGS", opt->host_cflags);
  if (opt->host_ldflags)
    ny_env_config_set(env, "NYTRIX_HOST_LDFLAGS", opt->host_ldflags);
  if (opt->host_triple)
    ny_env_config_set(env, "NYTRIX_HOST_TRIPLE", opt->host_triple);
  if (opt->arm_float_abi)
    ny_env_config_set(env, "NYTRIX_ARM_FLOAT_ABI", opt->arm_float_abi);
  if (opt->gpu_mode)
    ny_env_config_set(env, "NYTRIX_GPU_MODE", opt->gpu_mode);
  if (opt->gpu_backend)
    ny_env_config_set(env, "NYTRIX_GPU_BACKEND", opt->gpu_backend);
  if (opt->gpu_offload)
    ny_env_config_set(env, "NYTRIX_GPU_OFFLOAD", opt->gpu_offload);
  if (opt->gpu_min_work > 0)
    ny_env_config_set_int(env, "NYTRIX_GPU_MIN_WORK", opt->gpu_min_work);
  if (opt->gpu_async >= 0)
    ny_env_config_set_bool(env, "NYTRIX_GPU_ASYNC", opt->gpu_async != 0);
  if (opt->gpu_fast_math >= 0)
    ny_env_config_set_bool(env, "NYTRIX_GPU_FAST_MATH", opt->gpu_fast_math != 0);
  if (opt->accel_target)
    ny_env_config_set(env, "NYTRIX_ACCEL_TARGET", opt->accel_target);
  if (opt->accel_object)
    ny_env_config_set(env, "NYTRIX_ACCEL_OBJECT", opt->accel_object);
  if (opt->parallel_mode)
    ny_env_config_set(env, "NYTRIX_PARALLEL_MODE", opt->parallel_mode);
  if (opt->thread_count > 0)
    ny_env_config_set_int(env, "NYTRIX_PARALLEL_THREADS", opt->thread_count);
  if (opt->parallel_min_work > 0)
    ny_env_config_set_int(env, "NYTRIX_PARALLEL_MIN_WORK", opt->parallel_min_work);

  ny_env_config_set_bool(env, "NYTRIX_GC", opt->enable_gc);
  ny_env_config_set(env, "NYTRIX_HEAP_POLICY", ny_heap_policy_name(opt->heap_policy));
  ny_env_config_set_bool(env, "NYTRIX_RC_GC", opt->heap_policy == NY_HEAP_RC);
  ny_env_config_set_bool(env, "NYTRIX_OWNERSHIP", opt->ownership);
  ny_env_config_set_bool(env, "NYTRIX_OWNERSHIP_STRICT", opt->ownership_strict);
}

static bool ny_argv_has_flag(int argc, char **argv, const char *flag) {
  if (!flag || !*flag)
    return false;
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a)
      continue;
    /* Don't stop at -- for trace flags, we want global visibility */
    if (ny_arg_match(a, flag, NULL))
      return true;
  }
  return false;
}

static int ny_find_unified_command_index(int argc, char **argv, int *out_rc) {
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a || !*a)
      continue;
    int color_mode = -2;
    int color_idx = i;
    char err[256];
    int color_rc = ny_arg_consume_color(&color_idx, argc, argv, &color_mode, err, sizeof(err));
    if (color_rc < 0) {
      fprintf(stderr, "%sny:%s %s\n", clr(NY_CLR_RED), clr(NY_CLR_RESET),
              err[0] ? err : "invalid color option");
      if (out_rc)
        *out_rc = 2;
      return -1;
    }
    if (color_rc > 0) {
      ny_arg_apply_color_mode(color_mode);
      i = color_idx;
      continue;
    }
    if (strcmp(a, "--") == 0 || a[0] == '-')
      return 0;
    return i;
  }
  return 0;
}

static int ny_dispatch_tool_from(int (*fn)(int, char **), int argc, char **argv, int cmd_i) {
  return fn(argc - cmd_i, argv + cmd_i);
}

static int ny_dispatch_pkg_alias(int argc, char **argv, int cmd_i) {
  int sub_argc = argc - cmd_i + 1;
  char **sub_argv = (char **)calloc((size_t)sub_argc + 1, sizeof(char *));
  if (!sub_argv) {
    fprintf(stderr, "%sny:%s oom\n", clr(NY_CLR_RED), clr(NY_CLR_RESET));
    return 1;
  }
  sub_argv[0] = argv[0];
  for (int i = cmd_i; i < argc; ++i)
    sub_argv[i - cmd_i + 1] = argv[i];
  int rc = ny_pkg_main(sub_argc, sub_argv);
  free(sub_argv);
  return rc;
}

static bool ny_try_unified_tool(int argc, char **argv, int *out_rc) {
  if (argc < 2 || !argv)
    return false;
  int cmd_i = ny_find_unified_command_index(argc, argv, out_rc);
  if (cmd_i < 0)
    return true;
  if (cmd_i == 0)
    return false;
  const char *cmd = argv[cmd_i];
  if (strcmp(cmd, "fmt") == 0 || strcmp(cmd, "format") == 0) {
    *out_rc = ny_dispatch_tool_from(ny_fmt_main, argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "test") == 0 || strcmp(cmd, "tests") == 0) {
    *out_rc = ny_dispatch_tool_from(ny_test_main, argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "perf") == 0 || strcmp(cmd, "bench") == 0) {
    *out_rc = ny_dispatch_tool_from(ny_perf_main, argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "doc") == 0 || strcmp(cmd, "docs") == 0 || strcmp(cmd, "web") == 0) {
    *out_rc = ny_dispatch_tool_from(ny_web_main, argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "make") == 0 || strcmp(cmd, "build") == 0) {
    *out_rc = ny_dispatch_tool_from(ny_make_main, argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "pkg") == 0) {
    *out_rc = ny_pkg_main(argc - cmd_i, argv + cmd_i);
    return true;
  }
  if (strcmp(cmd, "new") == 0) {
    *out_rc = ny_new_main(argc - cmd_i, argv + cmd_i);
    return true;
  }
  if (strcmp(cmd, "get") == 0 || strcmp(cmd, "install") == 0) {
    *out_rc = cmd_i == 1 ? ny_pkg_main(argc, argv) : ny_dispatch_pkg_alias(argc, argv, cmd_i);
    return true;
  }
  if (strcmp(cmd, "lsp") == 0) {
    fprintf(stderr, "%sny:%s LSP remains a separate binary; run `ny-lsp`.\n", clr(NY_CLR_YELLOW),
            clr(NY_CLR_RESET));
    *out_rc = 2;
    return true;
  }
  return false;
}

#ifndef _WIN32
static double g_timeout = 0;
static void handle_timeout(int sig) {
  (void)sig;
  fprintf(stderr, "\n\033[1;31mERROR: Execution timed out after %.2f seconds.\033[0m\n", g_timeout);
  exit(124);
}
#endif

int main(int argc, char **argv, char **envp) {
  ny_load_default_config();
  int unified_rc = 0;
  if (ny_try_unified_tool(argc, argv, &unified_rc))
    return unified_rc;

  bool trace_requested =
      ny_argv_has_flag(argc, argv, "-trace") || ny_argv_has_flag(argc, argv, "--trace");
  if (!getenv("NYTRIX_SHARE_ROOT") || !*getenv("NYTRIX_SHARE_ROOT")) {
    const char *share_root = ny_src_root();
    if (share_root && *share_root)
      ny_setenv_force("NYTRIX_SHARE_ROOT", share_root);
  }
  /* Activate tracing as early as possible for JIT visibility */
  /* Set the robust trace flag BEFORE installing signal handlers */
  if (trace_requested) {
    ny_setenv_force("NYTRIX_TRACE", "1");
    g_trace_requested = 1;
    rt_trace_refresh_env();
  }
  ny_intern_init();
  atexit(ny_global_cleanup);
  if (g_trace_requested) {
    ny_jit_init_native_once();
  }
  ny_install_signal_handlers();
  /* Strictly tie tracing to the -trace flag presence */
  if (trace_requested) {
    ny_setenv_force("NYTRIX_TRACE", "1");
  } else {
    ny_unsetenv_force("NYTRIX_TRACE");
  }
  rt_trace_refresh_env();

  ny_options opt;
  ny_options_init(&opt);
  ny_options_parse(&opt, argc, argv);
  verbose_enabled = opt.verbose;
  color_mode = opt.color_mode;

  if (opt.mode == NY_MODE_REPL && !opt.safe_mode) {
    bool effect_policy_explicit = ny_argv_has_flag(argc, argv, "--effect-require-known") ||
                                  ny_argv_has_flag(argc, argv, "--no-effect-require-known");
    bool alias_policy_explicit = ny_argv_has_flag(argc, argv, "--alias-require-known") ||
                                 ny_argv_has_flag(argc, argv, "--no-alias-require-known");
    bool alias_escape_explicit = ny_argv_has_flag(argc, argv, "--alias-require-no-escape");
    if (!effect_policy_explicit)
      opt.effect_require_known = false;
    if (!alias_policy_explicit)
      opt.alias_require_known = false;
    if (!alias_escape_explicit)
      opt.alias_require_no_escape = false;
  }
  ny_clear_policy_env_overrides();
  ny_env_config_t cli_env = {0};
  ny_apply_cli_env_config(&cli_env, &opt, trace_requested);
  rt_trace_refresh_env();
#ifdef _WIN32
  if (opt.color_mode != 0)
    (void)rt_enable_vt();
#endif
  int ui_arg_bridge_rc = ny_bridge_ui_script_args_to_env(&opt, argc, argv);
  if (ui_arg_bridge_rc != 0)
    return ui_arg_bridge_rc;

  /* Match the common interpreter shape: `ny` starts an interactive REPL when
   * stdin is a terminal, and runs stdin as REPL batch input when piped. */
  if (!opt.command_string && !opt.input_file && opt.mode != NY_MODE_REPL &&
      opt.mode != NY_MODE_HELP && opt.mode != NY_MODE_VERSION &&
      opt.mode != NY_MODE_BUNDLE && opt.mode != NY_MODE_CLEAN_CACHE) {
    opt.mode = NY_MODE_REPL;
  }

  char **runtime_envp = ny_current_envp(envp);
  if (opt.input_file) {
    int s_argc = argc - opt.file_arg_idx;
    char **s_argv = &argv[opt.file_arg_idx];
    rt_set_args((int64_t)s_argc, (int64_t)(uintptr_t)s_argv, (int64_t)(uintptr_t)runtime_envp);
  } else if (opt.command_string) {
    static char *eval_argv[] = {(char *)"nytrix", NULL};
    rt_set_args(1, (int64_t)(uintptr_t)eval_argv, (int64_t)(uintptr_t)runtime_envp);
  } else if (opt.mode == NY_MODE_REPL) {
    if (opt.file_arg_idx > 0 && opt.file_arg_idx < argc) {
      int r_argc = argc - opt.file_arg_idx;
      char **r_argv = &argv[opt.file_arg_idx];
      rt_set_args((int64_t)r_argc, (int64_t)(uintptr_t)r_argv, (int64_t)(uintptr_t)runtime_envp);
    } else {
      static char *repl_argv[] = {(char *)"nytrix", NULL};
      rt_set_args(1, (int64_t)(uintptr_t)repl_argv, (int64_t)(uintptr_t)runtime_envp);
    }
  } else {
    rt_set_args((int64_t)argc, (int64_t)(uintptr_t)argv, (int64_t)(uintptr_t)runtime_envp);
  }
  if (opt.timeout > 0) {
#ifndef _WIN32
    g_timeout = opt.timeout;
    alarm((unsigned int)opt.timeout);
#endif
  }
  int exit_code = ny_pipeline_run(&opt);
  ny_options_free(&opt);
  ny_std_free_modules();
  rt_runtime_cleanup();
  return exit_code;
}
