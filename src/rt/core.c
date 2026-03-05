#include "base/common.h"
#include "rt/shared.h"
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

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

#ifndef _WIN32
#define NY_JMP_BUF jmp_buf
#define NY_SETJMP(env) _setjmp(env)
#define NY_LONGJMP(env, val) _longjmp(env, val)
#else
#define NY_JMP_BUF jmp_buf
// On Windows, _setjmp takes two arguments.
#define NY_SETJMP(env) _setjmp(env, NULL)
#define NY_LONGJMP(env, val) longjmp(env, val)
#endif

typedef struct {
  int64_t fn;
  int64_t env;
} defer_t;

typedef VEC(defer_t) defer_vec;
defer_vec g_defer_stack = {0};

typedef struct {
  NY_JMP_BUF *env;
  size_t defer_base;
} panic_env_t;

typedef VEC(panic_env_t) panic_env_vec;
panic_env_vec g_panic_env_stack = {0};
int64_t g_panic_value = 0;
static int64_t g_trace_file = 0;
static int64_t g_trace_line = 1;
static int64_t g_trace_col = 1;
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

#define RT_PRINT_BUF_SIZE 65536
static char rt_print_buf[RT_PRINT_BUF_SIZE];
static uint32_t rt_print_pos = 0;
static int rt_stdout_is_tty = -1;

void __rt_print_flush(void) {
  if (rt_print_pos > 0) {
    fwrite(rt_print_buf, 1, rt_print_pos, stdout);
    rt_print_pos = 0;
  }
}

static inline void rt_maybe_flush_line(void) {
#ifdef _WIN32
  if (rt_stdout_is_tty < 0)
    rt_stdout_is_tty = _isatty(_fileno(stdout));
#else
  if (rt_stdout_is_tty < 0)
    rt_stdout_is_tty = isatty(fileno(stdout));
#endif
  if (rt_stdout_is_tty)
    __rt_print_flush();
}

static inline void rt_print_put(const char *s, size_t len) {
  if (rt_print_pos + len > RT_PRINT_BUF_SIZE) {
    __rt_print_flush();
    if (len > RT_PRINT_BUF_SIZE) {
      fwrite(s, 1, len, stdout);
      return;
    }
  }
  memcpy(rt_print_buf + rt_print_pos, s, len);
  rt_print_pos += (uint32_t)len;
}

int64_t __rt_print_str_raw(int64_t v) {
  if (!v)
    return 0;
  const char *s = (const char *)(uintptr_t)v;
  uintptr_t lp = (uintptr_t)v - 16;
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
  int64_t len = tagged_len >> 1;
  rt_print_put(s, (size_t)len);
  return v;
}

static const char rt_digit_pairs[] = "00010203040506070809"
                                     "10111213141516171819"
                                     "20212223242526272829"
                                     "30313233343536373839"
                                     "40414243444546474849"
                                     "50515253545556575859"
                                     "60616263646566676869"
                                     "70717273747576777879"
                                     "80818283848586878889"
                                     "90919293949596979899";

int64_t __rt_print_int(int64_t v) {
  int64_t val = (int64_t)(v >> 1);
  if (rt_print_pos + 24 >= RT_PRINT_BUF_SIZE)
    __rt_print_flush();

  if (val == 0) {
    rt_print_buf[rt_print_pos++] = '0';
    return v;
  }
  char *start = rt_print_buf + rt_print_pos;
  if (val < 0) {
    *start++ = '-';
    val = -val;
  }
  char tmp[24];
  char *p = tmp + sizeof(tmp);
  uint64_t abs_v = (uint64_t)val;
  while (abs_v >= 100) {
    unsigned r = (unsigned)(abs_v % 100);
    abs_v /= 100;
    *--p = rt_digit_pairs[r * 2 + 1];
    *--p = rt_digit_pairs[r * 2];
  }
  if (abs_v >= 10) {
    *--p = rt_digit_pairs[abs_v * 2 + 1];
    *--p = rt_digit_pairs[abs_v * 2];
  } else {
    *--p = (char)('0' + abs_v);
  }
  size_t len = (size_t)(tmp + sizeof(tmp) - p);
  memcpy(start, p, len);
  rt_print_pos = (uint32_t)(start - rt_print_buf + len);
  return v;
}

int64_t __rt_print_newline(void) {
  if (rt_print_pos >= RT_PRINT_BUF_SIZE)
    __rt_print_flush();
  rt_print_buf[rt_print_pos++] = '\n';
  rt_maybe_flush_line();
  return 1;
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
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))d.fn;
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
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))d.fn;
    if (f)
      f(d.env);
  }
  return 0;
}

int64_t __set_panic_env(void *env_ptr) {
  panic_env_t pe = {(NY_JMP_BUF *)env_ptr, g_defer_stack.len};
  vec_push(&g_panic_env_stack, pe);
  return 0;
}

int64_t __clear_panic_env(void) {
  if (g_panic_env_stack.len > 0) {
    g_panic_env_stack.len--;
  }
  return 0;
}

int64_t __jmpbuf_size(void) { return (int64_t)sizeof(NY_JMP_BUF); }
int64_t __jmpbuf_align(void) { return (int64_t)_Alignof(NY_JMP_BUF); }
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
    while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r' ||
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

int64_t __trace_get_frames(int64_t *f, int64_t *l, int64_t *c, int64_t *fn,
                           int count) {
  if (g_trace_len == 0)
    return 0;
  int want = count;
  if (want > (int)g_trace_len)
    want = (int)g_trace_len;
  for (int i = 0; i < want; i++) {
    int idx = (int)((g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING);
    f[i] = g_trace_files[idx];
    l[i] = g_trace_lines[idx];
    c[i] = g_trace_cols[idx];
    fn[i] = g_trace_funcs[idx];
  }
  return (int64_t)want;
}

int64_t __trace_enter(int64_t func, int64_t file, int64_t line) {
  g_trace_func = func;
  g_trace_file = file;
  g_trace_line = line;
  g_trace_col = 1;
  trace_record(file, line, 1, func);
  return 0;
}

int64_t __trace_exit(void) { return 0; }

int64_t rt_trace_get_call_stack(int64_t *funcs, int64_t *files, int64_t *lines,
                                int max_count) {
  if (g_trace_len == 0)
    return 0;
  int want = max_count;
  if (want > (int)g_trace_len)
    want = (int)g_trace_len;
  for (int i = 0; i < want; i++) {
    int idx = (int)((g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING);
    if (files)
      files[i] = g_trace_files[idx];
    if (lines)
      lines[i] = g_trace_lines[idx];
    if (funcs)
      funcs[i] = g_trace_funcs[idx];
  }
  return (int64_t)want;
}

// Higher level panic logic below primitives

int64_t __argc_val = 1;
int64_t __envc_val = 1;
int64_t *__argv_ptr = NULL;
int64_t *__envp_ptr = NULL;

int64_t __set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
  static int curl_init = 0;
  if (!curl_init) {
    curl_global_init(CURL_GLOBAL_ALL);
    curl_init = 1;
  }
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

int64_t __argc(void) { return __argc_val; }
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
int64_t __is_ptr(int64_t v) { return is_ptr(v) ? 2 : 4; }
int64_t __is_ny_obj(int64_t v) { return is_ny_obj(v) ? 2 : 4; }
int64_t __is_str_obj(int64_t v) { return is_v_str(v) ? 2 : 4; }
int64_t __is_float_obj(int64_t v) { return is_v_flt(v) ? 2 : 4; }
int64_t __tagof(int64_t v) {
  if (v == 0)
    return 0;
  if (is_int(v))
    return rt_tag_v(1);
  if ((v & 7) == 6)
    return rt_tag_v(6);
  if (!is_ptr(v))
    return 0;
  if (!rt_addr_readable((uintptr_t)v - 8, 8))
    return 0;
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

/* __simd_mat4_mul(a, b, out) — column-major 4x4 float matrix multiply.
 * a, b, out are Nytrix list objects; elements [2..17] are the 16 floats.
 * Uses SSE2 when available; falls back to portable scalar.
 * Returns out. */
#if defined(__SSE2__) || defined(__aarch64__) || defined(_M_ARM64)
#if defined(__SSE2__)
#include <immintrin.h>
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  /* Each column of B is transformed by the full A */
  for (int col = 0; col < 4; col++) {
    __m128 bcol = _mm_loadu_ps(B + col * 4);
    __m128 r =
        _mm_mul_ps(_mm_loadu_ps(A + 0), _mm_shuffle_ps(bcol, bcol, 0x00));
    r = _mm_add_ps(
        r, _mm_mul_ps(_mm_loadu_ps(A + 4), _mm_shuffle_ps(bcol, bcol, 0x55)));
    r = _mm_add_ps(
        r, _mm_mul_ps(_mm_loadu_ps(A + 8), _mm_shuffle_ps(bcol, bcol, 0xAA)));
    r = _mm_add_ps(
        r, _mm_mul_ps(_mm_loadu_ps(A + 12), _mm_shuffle_ps(bcol, bcol, 0xFF)));
    _mm_storeu_ps(O + col * 4, r);
  }
}
#else /* NEON fallback */
#include <arm_neon.h>
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  for (int col = 0; col < 4; col++) {
    float32x4_t bcol = vld1q_f32(B + col * 4);
    float32x4_t r = vmulq_n_f32(vld1q_f32(A + 0), vgetq_lane_f32(bcol, 0));
    r = vmlaq_n_f32(r, vld1q_f32(A + 4), vgetq_lane_f32(bcol, 1));
    r = vmlaq_n_f32(r, vld1q_f32(A + 8), vgetq_lane_f32(bcol, 2));
    r = vmlaq_n_f32(r, vld1q_f32(A + 12), vgetq_lane_f32(bcol, 3));
    vst1q_f32(O + col * 4, r);
  }
}
#endif
#define NY_HAS_SIMD_MAT4 1
#else
#define NY_HAS_SIMD_MAT4 0
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++)
        s += A[k * 4 + r] * B[c * 4 + k];
      O[c * 4 + r] = s;
    }
  }
}
#endif

int64_t __simd_mat4_mul_ptr(int64_t a_ptr, int64_t b_ptr, int64_t o_ptr) {
  if (!is_ptr(a_ptr) || !is_ptr(b_ptr) || !is_ptr(o_ptr))
    return o_ptr;
  _mat4_mul_simd((const float *)(uintptr_t)a_ptr,
                 (const float *)(uintptr_t)b_ptr, (float *)(uintptr_t)o_ptr);
  return o_ptr;
}

int64_t __simd_mat4_mul(int64_t a_lst, int64_t b_lst, int64_t o_lst) {
  if (!is_ptr(a_lst) || !is_ptr(b_lst) || !is_ptr(o_lst))
    return o_lst;
  /* Nytrix list layout: header(16b) + len(8b) + cap(8b) + items[2..17] at
   * +16+(i*8) */
  float A[16], B[16], Out[16];
  for (int i = 0; i < 16; i++) {
    int64_t av = *(int64_t *)((char *)(uintptr_t)a_lst + 16 + (i + 2) * 8);
    int64_t bv = *(int64_t *)((char *)(uintptr_t)b_lst + 16 + (i + 2) * 8);
    double da, db;
    if (is_int(av)) {
      da = (double)(av >> 1);
    } else if (is_v_flt(av)) {
      memcpy(&da, (void *)(uintptr_t)av, 8);
    } else {
      da = 0.0;
    }
    if (is_int(bv)) {
      db = (double)(bv >> 1);
    } else if (is_v_flt(bv)) {
      memcpy(&db, (void *)(uintptr_t)bv, 8);
    } else {
      db = 0.0;
    }
    A[i] = (float)da;
    B[i] = (float)db;
  }
  _mat4_mul_simd(A, B, Out);
  for (int i = 0; i < 16; i++) {
    double dv = (double)Out[i];
    int64_t bits;
    memcpy(&bits, &dv, 8);
    int64_t boxed = __flt_box_val(bits);
    *(int64_t *)((char *)(uintptr_t)o_lst + 16 + (i + 2) * 8) = boxed;
  }
  return o_lst;
}

int64_t __mat4_to_buffer(int64_t m_lst, int64_t buf_ptr) {
  if (!is_ptr(m_lst) || !is_ptr(buf_ptr))
    return buf_ptr;
  float *buf = (float *)(uintptr_t)buf_ptr;
  for (int i = 0; i < 16; i++) {
    int64_t v = *(int64_t *)((char *)(uintptr_t)m_lst + 16 + (i + 2) * 8);
    double dv;
    if (is_int(v)) {
      dv = (double)(v >> 1);
    } else if (is_v_flt(v)) {
      memcpy(&dv, (void *)(uintptr_t)v, 8);
    } else {
      dv = 0.0;
    }
    buf[i] = (float)dv;
  }
  return buf_ptr;
}

int64_t __mat4_from_buffer(int64_t m_lst, int64_t buf_ptr) {
  if (!is_ptr(m_lst) || !is_ptr(buf_ptr))
    return m_lst;
  const float *buf = (const float *)(uintptr_t)buf_ptr;
  for (int i = 0; i < 16; i++) {
    double dv = (double)buf[i];
    int64_t bits;
    memcpy(&bits, &dv, 8);
    int64_t boxed = __flt_box_val(bits);
    *(int64_t *)((char *)(uintptr_t)m_lst + 16 + (i + 2) * 8) = boxed;
  }
  return m_lst;
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
  if (n < 0)
    n = 0;
  // Standard layout: 16 bytes header (length, capacity) + n * 8 bytes data
  int64_t p = __malloc(16 + n * 8);
  if (!p)
    return 0;
  // Standard Nytrix tags are at p-8
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_LIST;
  *(int64_t *)((char *)(uintptr_t)p + 0) =
      1; // Length = 0 (tagged: (0<<1)|1 = 1)
  *(int64_t *)((char *)(uintptr_t)p + 8) =
      (n << 1) | 1; // Capacity = n (tagged)

  return p;
}

int64_t __append(int64_t lst, int64_t val) {
  if (!is_ptr(lst))
    return lst;
  if (__tagof(lst) != ((TAG_LIST << 1) | 1))
    return lst;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  int64_t cap_v = *(int64_t *)((char *)(uintptr_t)lst + 8);
  int64_t cap = is_int(cap_v) ? (cap_v >> 1) : cap_v;

  if (n >= cap) {
    int64_t new_cap = cap == 0 ? 8 : (cap * 2);
    int64_t new_p = __malloc(16 + new_cap * 8);
    if (!new_p)
      return lst;
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
  if (!is_ptr(lst))
    return 0;
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
  if (!is_ptr(lst))
    return 1; /* tagged 0 */
  int64_t tagged = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return tagged; /* caller uses tagged value directly in Nytrix arithmetic */
}

/* Fast list-length write: stores `n` (tagged integer) at lst+0. */
int64_t __list_set_len(int64_t lst, int64_t n) {
  if (!is_ptr(lst))
    return 0;
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
  __rt_print_flush();
  bool has_env = (g_panic_env_stack.len > 0);
  if (!has_env) {
    fputc('\n', stderr);
  }
  print_panic_msg(msg_ptr);

  /* Print Nytrix Call Stack */
  if (g_trace_len > 0) {
    const char *cyan = color_mode ? clr(NY_CLR_CYAN) : "";
    const char *rs = color_mode ? clr(NY_CLR_RESET) : "";

    fprintf(stderr, "\n%s--- Nytrix Call Stack (%zu) ---%s\n", cyan,
            g_trace_len, rs);

    /* Print from oldest to newest */
    size_t start = (g_trace_len < TRACE_RING) ? 0 : g_trace_idx;
    size_t count = g_trace_len;
    int frame_num = (int)(g_trace_len - 1);

    for (size_t i = 0; i < count; i++) {
      size_t idx = (start + i) % TRACE_RING;
      int64_t file = g_trace_files[idx];
      int64_t line = g_trace_lines[idx];
      int64_t col = g_trace_cols[idx];
      int64_t func = g_trace_funcs[idx];

      if (is_v_str(file)) {
        const char *fname = (const char *)(uintptr_t)file;
        size_t flen = trace_str_len(file);
        int64_t l = is_int(line) ? rt_untag_v(line) : line;
        int64_t c = is_int(col) ? rt_untag_v(col) : col;

        fprintf(stderr, "  %s#%d%s %.*s:%ld:%ld", cyan, frame_num - (int)i, rs,
                (int)flen, fname, (long)l, (long)c);

        if (is_v_str(func)) {
          const char *fn = (const char *)(uintptr_t)func;
          size_t fnlen = trace_str_len(func);
          fprintf(stderr, " (fn %.*s)", (int)fnlen, fn);
        }
        fputc('\n', stderr);

        /* Print snippet for recent frames */
        if (i >= count - 5 && i < count) {
          print_rt_snippet(file, line, col);
        }
      }
    }
  }

  /* Print last location with snippet */
  if (is_v_str(g_trace_file)) {
    const char *cyan = color_mode ? clr(NY_CLR_CYAN) : "";
    const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
    fprintf(stderr, "\n%sLast Nytrix location:%s ", cyan, rs);
    print_trace_entry(g_trace_file, g_trace_line, g_trace_col, g_trace_func,
                      "");
    print_rt_snippet(g_trace_file, g_trace_line, g_trace_col);
  }

  if (has_env) {
    g_panic_value = msg_ptr;
    panic_env_t pe = g_panic_env_stack.data[g_panic_env_stack.len - 1];
    __run_defers_to((int64_t)((pe.defer_base << 1) | 1));
    NY_LONGJMP(*pe.env, 1);
  }
  fprintf(stderr, "\n");
  exit(1);
}
