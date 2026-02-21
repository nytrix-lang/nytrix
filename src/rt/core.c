#include "base/common.h"
#include "rt/shared.h"
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdlib.h>

#ifdef _WIN32
#ifdef __argc
#undef __argc
#endif
#ifdef __argv
#undef __argv
#endif
#endif

#ifndef _WIN32
extern char **environ;
#endif

int color_mode __attribute__((weak)) = 0;
int debug_enabled __attribute__((weak)) = 0;

int64_t g_globals_ptr = 1; // tagged (0)

typedef struct {
  int64_t fn;
  int64_t env;
} defer_t;

typedef VEC(defer_t) defer_vec;
static defer_vec g_defer_stack = {0};

typedef struct {
  jmp_buf *env;
  size_t defer_base;
} panic_env_t;

typedef VEC(panic_env_t) panic_env_vec;
static panic_env_vec g_panic_env_stack = {0};
static int64_t g_panic_value = 0;
static int64_t g_trace_file = 0;
static int64_t g_trace_line = 1; // tagged
static int64_t g_trace_col = 1;  // tagged
static int64_t g_trace_func = 0;
#define TRACE_RING 32
static int64_t g_trace_files[TRACE_RING] = {0};
static int64_t g_trace_lines[TRACE_RING] = {0};
static int64_t g_trace_cols[TRACE_RING] = {0};
static int64_t g_trace_funcs[TRACE_RING] = {0};
static size_t g_trace_len = 0;
static size_t g_trace_idx = 0;
static int g_trace_print = -1;

static size_t trace_str_len(int64_t v) {
  if (!is_v_str(v))
    return 0;
  uintptr_t lp = (uintptr_t)v - 16;
  if (!rt_addr_readable(lp, sizeof(int64_t)))
    return 0;
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
  if (!is_int(tagged_len))
    return 0;
  return (size_t)(tagged_len >> 1);
}

static void trace_print_loc(void) {
  if (!is_v_str(g_trace_file))
    return;
  const char *file = (const char *)(uintptr_t)g_trace_file;
  size_t flen = trace_str_len(g_trace_file);
  int64_t line = is_int(g_trace_line) ? rt_untag_v(g_trace_line) : 0;
  int64_t col = is_int(g_trace_col) ? rt_untag_v(g_trace_col) : 0;
  fprintf(stderr, "[trace] %.*s:%ld:%ld", (int)flen, file, (long)line,
          (long)col);
  if (is_v_str(g_trace_func)) {
    const char *fn = (const char *)(uintptr_t)g_trace_func;
    size_t fnlen = trace_str_len(g_trace_func); // Fixed here
    fprintf(stderr, " (fn %.*s)", (int)fnlen, fn);
  }
  fputc('\n', stderr);
}

static void trace_record(int64_t file, int64_t line, int64_t col,
                         int64_t func) {
  g_trace_files[g_trace_idx] = file;
  g_trace_lines[g_trace_idx] = line;
  g_trace_cols[g_trace_idx] = col;
  g_trace_funcs[g_trace_idx] = func;
  g_trace_idx = (g_trace_idx + 1) % TRACE_RING;
  if (g_trace_len < TRACE_RING)
    g_trace_len++;
}

static bool trace_should_print(void) {
  if (g_trace_print >= 0)
    return g_trace_print != 0;
  const char *env = getenv("NYTRIX_TRACE_VERBOSE");
  g_trace_print = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
  return g_trace_print != 0;
}

int64_t __globals(void) { return g_globals_ptr; }
int64_t __set_globals(int64_t p) {
  g_globals_ptr = p;
  return p;
}

int64_t __push_defer(int64_t fn, int64_t env) {
  vec_push(&g_defer_stack, ((defer_t){fn, env}));
  return 0;
}

int64_t __pop_run_defer(void) {
  if (g_defer_stack.len > 0) {
    defer_t d = g_defer_stack.data[--g_defer_stack.len];
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))__mask_ptr(d.fn);
    if (f)
      f(d.env);
  }
  return 0;
}

int64_t __run_defers_to(int64_t target_len_v) {
  size_t target_len =
      (size_t)(is_int(target_len_v) ? (target_len_v >> 1) : target_len_v);
  while (g_defer_stack.len > target_len) {
    defer_t d = g_defer_stack.data[--g_defer_stack.len];
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))__mask_ptr(d.fn);
    if (f)
      f(d.env);
  }
  return 0;
}

int64_t __set_panic_env(int64_t env_ptr) {
  panic_env_t pe = {(jmp_buf *)(uintptr_t)env_ptr, g_defer_stack.len};
  vec_push(&g_panic_env_stack, pe);
  return 0;
}

int64_t __clear_panic_env(void) {
  if (g_panic_env_stack.len > 0) {
    g_panic_env_stack.len--;
  }
  return 0;
}

int64_t __jmpbuf_size(void) { return (int64_t)sizeof(jmp_buf); }
int64_t __get_panic_val(void) { return g_panic_value; }

int64_t __trace_loc(int64_t file, int64_t line, int64_t col) {
  g_trace_file = file;
  g_trace_line = line;
  g_trace_col = col;
  trace_record(file, line, col, g_trace_func);
  if (trace_should_print())
    trace_print_loc();
  return 0;
}

int64_t __trace_func(int64_t name) {
  g_trace_func = name;
  return 0;
}

int64_t __trace_last_file(void) { return g_trace_file; }
int64_t __trace_last_line(void) { return g_trace_line; }
int64_t __trace_last_col(void) { return g_trace_col; }
int64_t __trace_last_func(void) { return g_trace_func; }

int64_t __trace_dump(int64_t count) {
  if (g_trace_len == 0)
    return 0;
  size_t want = (size_t)(is_int(count) ? rt_untag_v(count) : count);
  if (want == 0 || want > g_trace_len)
    want = g_trace_len;
  size_t start = (g_trace_idx + TRACE_RING - want) % TRACE_RING;
  for (size_t i = 0; i < want; i++) {
    size_t idx = (start + i) % TRACE_RING;
    int64_t file = g_trace_files[idx];
    int64_t line = g_trace_lines[idx];
    int64_t col = g_trace_cols[idx];
    int64_t func = g_trace_funcs[idx];
    if (!is_v_str(file))
      continue;
    const char *fname = (const char *)(uintptr_t)file;
    size_t flen = trace_str_len(file);
    int64_t l = is_int(line) ? rt_untag_v(line) : 0;
    int64_t c = is_int(col) ? rt_untag_v(col) : 0;
    fprintf(stderr, "  at %.*s:%ld:%ld", (int)flen, fname, (long)l, (long)c);
    if (is_v_str(func)) {
      const char *fn = (const char *)(uintptr_t)func;
      size_t fnlen = trace_str_len(func); // Fixed here
      fprintf(stderr, " (fn %.*s)", (int)fnlen, fn);
    }
    fputc('\n', stderr);
  }
  return 0;
}

int64_t __panic(int64_t msg_ptr) {
  if (g_panic_env_stack.len > 0) {
    g_panic_value = msg_ptr;
    panic_env_t pe = g_panic_env_stack.data[g_panic_env_stack.len - 1];
    // Run defers up to the catch block
    __run_defers_to((int64_t)((pe.defer_base << 1) | 1));
    longjmp(*pe.env, 1);
  }
  if (is_v_str(g_trace_file)) {
    fprintf(stderr, "Panic location: ");
    trace_print_loc();
  }
  // Make panic function robust
  if (is_int(msg_ptr)) {
    fprintf(stderr, "Panic: <integer value> %" PRId64 " (raw)\n",
            (int64_t)msg_ptr);
  } else if (is_v_str(msg_ptr)) { // This implies is_heap_ptr(msg_ptr)
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    size_t msg_len = trace_str_len(msg_ptr);
    fprintf(stderr, "Panic: %.*s\n", (int)msg_len, msg);
  } else {
    // pointers)
    fprintf(stderr, "Panic: <unknown type> %" PRIx64 " (raw)\n",
            (uint64_t)msg_ptr);
  }
  exit(1);
}

int64_t __argc_val = 1; // tagged 0
int64_t __envc_val = 1; // tagged 0
int64_t *__argv_ptr = NULL;
int64_t *__envp_ptr = NULL;

int64_t __set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
  __cleanup_args();
  __argc_val = (argc << 1) | 1;
  __argv_ptr = (int64_t *)(uintptr_t)__malloc(
      ((int64_t)(argc + 1) * sizeof(int64_t) << 1) | 1);
  if (!__argv_ptr)
    return -1;
  memset(__argv_ptr, 0, (argc + 1) * sizeof(int64_t));
  char **old_argv = (char **)argv_ptr;
  for (int i = 0; i < argc; i++) {
    if (old_argv[i]) {
      size_t len = strlen(old_argv[i]);
      int64_t p = __malloc((int64_t)((len + 1) * sizeof(char) << 1) | 1);
      if (!p)
        return -1;
      *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
      *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
      __copy_mem((int64_t)(uintptr_t)p, (int64_t)(uintptr_t)old_argv[i],
                 (int64_t)((len + 1) * sizeof(char) << 1) | 1);
      __argv_ptr[i] = p;
    } else {
      __argv_ptr[i] = 0;
    }
  }
  char **old_envp = (char **)envp_ptr;
#ifndef _WIN32
  if (!old_envp)
    old_envp = environ;
#endif
  int env_count = 0;
  if (old_envp) {
    while (old_envp[env_count])
      env_count++;
  }
  __envc_val = (env_count << 1) | 1;
  __envp_ptr = (int64_t *)(uintptr_t)__malloc(
      ((int64_t)(env_count + 1) * sizeof(int64_t) << 1) | 1);
  if (!__envp_ptr)
    return -1;
  memset(__envp_ptr, 0, (env_count + 1) * sizeof(int64_t));
  for (int i = 0; i < env_count; i++) {
    size_t len = strlen(old_envp[i]);
    int64_t p = __malloc((int64_t)((len + 1) * sizeof(char) << 1) | 1);
    if (!p)
      return -1;
    *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
    *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
    __copy_mem((int64_t)(uintptr_t)p, (int64_t)(uintptr_t)old_envp[i],
               (int64_t)((len + 1) * sizeof(char) << 1) | 1);
    __envp_ptr[i] = p;
  }
  return 0;
}

void __cleanup_args(void) {
  if (__argv_ptr) {
    int argc = (__argc_val >> 1);
    for (int i = 0; i < argc; i++) {
      if (__argv_ptr[i])
        __free(__argv_ptr[i]);
    }
    __free((int64_t)(uintptr_t)__argv_ptr);
    __argv_ptr = NULL;
  }
  if (__envp_ptr) {
    int envc = (__envc_val >> 1);
    for (int i = 0; i < envc; i++) {
      if (__envp_ptr[i])
        __free(__envp_ptr[i]);
    }
    __free((int64_t)(uintptr_t)__envp_ptr);
    __envp_ptr = NULL;
  }
  __argc_val = 1;
  __envc_val = 1;
}

int64_t ny_rt_argc(void) { return __argc_val; }
int64_t __envc(void) { return __envc_val; }
int64_t __envp(void) { return (int64_t)__envp_ptr; }
int64_t ny_rt_argvp(void) { return (int64_t)__argv_ptr; }

int64_t ny_rt_argv(int64_t i) {
  if (!is_int(i))
    return 0;
  int idx = (int)(i >> 1);
  if (idx < 0 || idx >= (__argc_val >> 1))
    return 0;
  int64_t raw = __argv_ptr[idx];
  if (!raw)
    return 0;
  const char *s = (const char *)(uintptr_t)raw;
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)(len + 1) * sizeof(char) << 1) | 1);
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)res - 16) = ((int64_t)len << 1) | 1;
  __copy_mem((int64_t)(uintptr_t)res, (int64_t)(uintptr_t)s,
             (int64_t)((len + 1) * sizeof(char) << 1) | 1);
  return res;
}

int64_t __tag(int64_t v) { return rt_tag_v(v); }
int64_t __untag(int64_t v) { return rt_untag_v(v); }
int64_t __is_int(int64_t v) { return is_int(v) ? 2 : 4; }
int64_t __is_ptr(int64_t v) { return is_ptr(v) ? 2 : 4; }
int64_t __is_ny_obj(int64_t v) { return is_ny_obj(v) ? 2 : 4; }
int64_t __is_str_obj(int64_t v) { return is_v_str(v) ? 2 : 4; }
int64_t __is_float_obj(int64_t v) { return is_v_flt(v) ? 2 : 4; }
int64_t __tagof(int64_t v) {
  if (!is_ptr(v))
    return 0;
  uintptr_t tp = (uintptr_t)v - 8;
  if (!rt_addr_readable(tp, sizeof(int64_t)))
    return 0;
  int64_t tag = 0;
  memcpy(&tag, (const void *)tp, sizeof(tag));
  return tag;
}

int64_t __errno_val = 1;
int64_t __errno(void) { return (int64_t)((errno << 1) | 1); }

int64_t __copy_mem(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return dst;
  char *d = (char *)(uintptr_t)dst;
  const char *s = (const char *)(uintptr_t)src;
  for (int64_t i = 0; i < n; i++) {
    d[i] = s[i];
  }
  return dst;
}

int64_t __result_ok(int64_t v) {
  int64_t res = __malloc(((int64_t)8 << 1) | 1);
  if (!res)
    return 0;
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_OK;
  *(int64_t *)((char *)(uintptr_t)res - 16) = (8 << 1) | 1;
  *(int64_t *)(uintptr_t)res = v;
  return res;
}

int64_t __result_err(int64_t e) {
  int64_t res = __malloc(((int64_t)8 << 1) | 1);
  if (!res)
    return 0;
  *(int64_t *)((char *)(uintptr_t)res - 8) = TAG_ERR;
  *(int64_t *)((char *)(uintptr_t)res - 16) = (8 << 1) | 1;
  *(int64_t *)(uintptr_t)res = e;
  return res;
}

int64_t __is_ok(int64_t v) { return is_v_ok(v) ? 2 : 4; }
int64_t __is_err(int64_t v) { return is_v_err(v) ? 2 : 4; }
int64_t __unwrap(int64_t v) {
  if (is_v_ok(v) || is_v_err(v)) {
    return *(int64_t *)(uintptr_t)v;
  }
  return v;
}
