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


int64_t g_globals_ptr = 1;

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
static int64_t g_trace_line = 1;
static int64_t g_trace_col = 1;
static int64_t g_trace_func = 0;
#define TRACE_RING 256
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

void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func,
                       const char *prefix) {
  if (!is_v_str(file))
    return;
  const char *fname = (const char *)(uintptr_t)file;
  size_t flen = trace_str_len(file);
  int64_t l = is_int(line) ? rt_untag_v(line) : line;
  int64_t c = is_int(col) ? rt_untag_v(col) : col;

  const char *pre = prefix ? prefix : "";
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *c2 = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";

  fprintf(stderr, "%s%s%.*s:%s%ld:%ld%s", pre, c1, (int)flen, fname, c2,
          (long)l, (long)c, rs);
  if (is_v_str(func)) {
    const char *fn = (const char *)(uintptr_t)func;
    size_t fnlen = trace_str_len(func);
    const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
    fprintf(stderr, " (%sfn %.*s%s)", fnc, (int)fnlen, fn, rs);
  }
  fputc('\n', stderr);
}

int64_t __rt_alloc_string_len(const char *s, size_t len) {
  if (!s)
    return 0;
  int64_t p = __malloc((int64_t)((len + 1) * sizeof(char) << 1) | 1);
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
  memcpy((void *)(uintptr_t)p, s, len);
  ((char *)(uintptr_t)p)[len] = '\0';
  return p;
}

int64_t __rt_alloc_string(const char *s) {
  if (!s)
    return 0;
  return __rt_alloc_string_len(s, strlen(s));
}

static void trace_print_loc(void) {
  print_trace_entry(g_trace_file, g_trace_line, g_trace_col, g_trace_func,
                    "[trace] ");
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

static void print_rt_snippet(int64_t file_ptr, int64_t line_ptr,
                             int64_t col_ptr) {
  if (!is_v_str(file_ptr))
    return;
  const char *file = (const char *)(uintptr_t)file_ptr;
  int64_t line = is_int(line_ptr) ? rt_untag_v(line_ptr) : line_ptr;
  int64_t col = is_int(col_ptr) ? rt_untag_v(col_ptr) : col_ptr;
  if (line <= 0)
    return;

  FILE *f = fopen(file, "r");
  if (!f)
    return;

  char buf[1024];
  int curr = 1;
  while (curr < line && fgets(buf, sizeof(buf), f)) {
    curr++;
  }

  if (curr == line && fgets(buf, sizeof(buf), f)) {
    size_t blen = strlen(buf);
    while (blen > 0 &&
           (buf[blen - 1] == '\n' || buf[blen - 1] == '\r' ||
            buf[blen - 1] == ' ')) {
      buf[--blen] = '\0';
    }

    const char *gray = color_mode ? clr(NY_CLR_GRAY) : "";
    const char *red = color_mode ? clr(NY_CLR_RED) : "";
    const char *rs = color_mode ? clr(NY_CLR_RESET) : "";

    fprintf(stderr, "%s%4d | %s%s\n", gray, (int)line, rs, buf);
    fprintf(stderr, "%s     | %s", gray, rs);
    for (int i = 1; i < col; i++) {
      if (i <= (int)blen && buf[i - 1] == '\t')
        fputc('\t', stderr);
      else
        fputc(' ', stderr);
    }
    fprintf(stderr, "%s^%s\n", red, rs);
  }
  fclose(f);
}

int64_t __trace_dump(int64_t count) {
  if (g_trace_len == 0)
    return 0;
  size_t want = (size_t)(is_int(count) ? rt_untag_v(count) : count);
  if (want == 0 || want > g_trace_len)
    want = g_trace_len;
  
  // Print newest first
  for (size_t i = 0; i < want; i++) {
    size_t idx = (g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING;
    print_trace_entry(g_trace_files[idx], g_trace_lines[idx], g_trace_cols[idx],
                      g_trace_funcs[idx], "  at ");
  }
  return 0;
}

// Higher level panic logic below primitives

int64_t __argc_val = 1;
int64_t __envc_val = 1;
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
    __argv_ptr[i] = old_argv[i] ? __rt_alloc_string(old_argv[i]) : 0;
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
    __envp_ptr[i] = __rt_alloc_string(old_envp[i]);
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
  return __rt_alloc_string((const char *)(uintptr_t)raw);
}

int64_t __tag(int64_t v) { return rt_tag_v(v); }
int64_t __untag(int64_t v) { return rt_untag_v(v); }
int64_t __is_int(int64_t v) { return is_int(v) ? 2 : 4; }
int64_t __is_ptr(int64_t v) {
  return is_ptr(v) ? 2 : 4;
}
int64_t __is_ny_obj(int64_t v) { return is_ny_obj(v) ? 2 : 4; }
int64_t __is_str_obj(int64_t v) { return is_v_str(v) ? 2 : 4; }
int64_t __is_float_obj(int64_t v) { return is_v_flt(v) ? 2 : 4; }
int64_t __tagof(int64_t v) {
  if (v == 0) return 0;
  if (is_int(v)) return rt_tag_v(1);
  if ((v & 7) == 6) return rt_tag_v(6);
  if (!is_ptr(v)) return 0;
  if (!rt_addr_readable((uintptr_t)v - 8, 8)) return 0;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  return rt_tag_v(tag);
}

int64_t __errno_val = 1;
int64_t __errno(void) { return (int64_t)((errno << 1) | 1); }

int64_t __copy_mem(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return dst;
  memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
  return dst;
}

int64_t __rt_result_alloc(int64_t tag, int64_t v) {
  int64_t sz = (int64_t)sizeof(int64_t);
  int64_t res = __malloc(((int64_t)sz << 1) | 1);
  if (!res)
    return 0;
  *(int64_t *)((char *)(uintptr_t)res - 8) = tag;
  *(int64_t *)((char *)(uintptr_t)res - 16) = (sz << 1) | 1;
  *(int64_t *)(uintptr_t)res = v;
  return res;
}

int64_t __result_ok(int64_t v) { return __rt_result_alloc(TAG_OK, v); }

int64_t __result_err(int64_t e) { return __rt_result_alloc(TAG_ERR, e); }

int64_t __is_ok(int64_t v) { return is_v_ok(v) ? 2 : 4; }
int64_t __is_err(int64_t v) { return is_v_err(v) ? 2 : 4; }
int64_t __unwrap(int64_t v) {
  if (is_v_ok(v) || is_v_err(v)) {
    return *(int64_t *)(uintptr_t)v;
  }
  return v;
}
int64_t __list_new(int64_t n_v) {
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0) n = 0;
  // Standard layout: 16 bytes header (length, capacity) + n * 8 bytes data
  int64_t p = __malloc(16 + n * 8);
  if (!p) return 0;
  // Standard Nytrix tags are at p-8
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_LIST;
  *(int64_t *)((char *)(uintptr_t)p + 0) = 1; // Length = 0 (tagged: (0<<1)|1 = 1)
  *(int64_t *)((char *)(uintptr_t)p + 8) = (n << 1) | 1; // Capacity = n (tagged)

  return p;
}

int64_t __append(int64_t lst, int64_t val) {
  if (!is_ptr(lst)) return lst;
  if (__tagof(lst) != ((TAG_LIST << 1) | 1)) return lst;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  int64_t cap_v = *(int64_t *)((char *)(uintptr_t)lst + 8);
  int64_t cap = is_int(cap_v) ? (cap_v >> 1) : cap_v;

  if (n >= cap) {
    int64_t new_cap = cap == 0 ? 8 : (cap * 2);
    int64_t new_p = __malloc(16 + new_cap * 8);
    if (!new_p) return lst;
    *(int64_t *)((char *)(uintptr_t)new_p - 8) = TAG_LIST;
    *(int64_t *)((char *)(uintptr_t)new_p + 0) = len_v;
    *(int64_t *)((char *)(uintptr_t)new_p + 8) = (new_cap << 1) | 1;
    memcpy((char *)(uintptr_t)new_p + 16, (char *)(uintptr_t)lst + 16, n * 8);
    lst = new_p;
  }

  *(int64_t *)((char *)(uintptr_t)lst + 16 + n * 8) = val;
  *(int64_t *)((char *)(uintptr_t)lst + 0) = ((n + 1) << 1) | 1;
  return lst;
}



int64_t __load_item(int64_t lst, int64_t i_v) {
  return __rt_load_item_fast(lst, i_v);
}

int64_t __store_item(int64_t lst, int64_t i_v, int64_t val) {
  if (!is_ptr(lst)) return 0;
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8) = val;
  return val;
}

int64_t __load_item_fast(int64_t lst, int64_t i_v) {
  return __rt_load_item_fast(lst, i_v);
}

int64_t __store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  int64_t i = is_int(i_v) ? (i_v >> 1) : i_v;
  *(int64_t *)((char *)(uintptr_t)lst + 16 + i * 8) = val;
  return val;
}

/* Fast list-length read: reads the tagged length word at lst+0 and returns the
 * raw (untagged) element count.  Avoids the full __load64_idx bounds-check +
 * rt_addr_readable machinery when all we need is the length. */
int64_t __list_len(int64_t lst) {
  if (!is_ptr(lst)) return 1; /* tagged 0 */
  int64_t tagged = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return tagged; /* caller uses tagged value directly in Nytrix arithmetic */
}

/* Fast list-length write: stores `n` (tagged integer) at lst+0. */
int64_t __list_set_len(int64_t lst, int64_t n) {
  if (!is_ptr(lst)) return 0;
  *(int64_t *)((char *)(uintptr_t)lst + 0) = n;
  return n;
}

static void print_panic_msg(int64_t msg_ptr) {
  const char *red = color_mode ? clr(NY_CLR_RED) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  
  if (is_int(msg_ptr)) {
    fprintf(stderr, "%sPanic (int):%s %" PRId64 "\n", red, rs,
            (int64_t)rt_untag_v(msg_ptr));
  } else if (is_v_str(msg_ptr)) {
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    size_t msg_len = trace_str_len(msg_ptr);
    fprintf(stderr, "%sPanic:%s %.*s\n", red, rs, (int)msg_len, msg);
  } else if (is_v_err(msg_ptr)) {
    int64_t err = __unwrap(msg_ptr);
    fprintf(stderr, "%sPanic (err):%s ", red, rs);
    print_panic_msg(err);
  } else {
    fprintf(stderr, "%sPanic (raw):%s 0x%" PRIx64 "\n", red, rs,
            (uint64_t)msg_ptr);
  }
}

int64_t __get_backtrace(int64_t count_v) {
  if (g_trace_len == 0)
    return __list_new(0);
  size_t want = (size_t)(is_int(count_v) ? rt_untag_v(count_v) : count_v);
  if (want == 0 || want > g_trace_len)
    want = g_trace_len;
  int64_t lst = __list_new(is_int(want) ? (int64_t)want : __tag((int64_t)want));
  size_t start = (g_trace_idx + TRACE_RING - want) % TRACE_RING;
  for (size_t i = 0; i < want; i++) {
    size_t idx = (start + i) % TRACE_RING;
    int64_t frame = __list_new(__tag(4));
    __append(frame, g_trace_files[idx]);
    __append(frame, g_trace_lines[idx]);
    __append(frame, g_trace_cols[idx]);
    __append(frame, g_trace_funcs[idx]);
    __append(lst, frame);
  }
  return lst;
}

int64_t __panic(int64_t msg_ptr) {
  if (g_panic_env_stack.len > 0) {
    g_panic_value = msg_ptr;
    panic_env_t pe = g_panic_env_stack.data[g_panic_env_stack.len - 1];
    __run_defers_to((int64_t)((pe.defer_base << 1) | 1));
    longjmp(*pe.env, 1);
  }
  
  fputc('\n', stderr);
  print_panic_msg(msg_ptr);

  if (is_v_str(g_trace_file)) {
    print_trace_entry(g_trace_file, g_trace_line, g_trace_col, g_trace_func,
                      "  at ");
    print_rt_snippet(g_trace_file, g_trace_line, g_trace_col);
  }
  
  if (g_trace_len > 0) {
    const char *cyan = color_mode ? clr(NY_CLR_CYAN) : "";
    const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
    fprintf(stderr, "\n%sLast Nytrix frames:%s\n", cyan, rs);
    __trace_dump(((int64_t)10 << 1) | 1); // last 10
  }
  
  fprintf(stderr, "\n");
  exit(1);
}
