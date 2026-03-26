/* Manual memset to avoid IFUNC issues */
#define memset_manual(p, v, n)                                                                     \
  do {                                                                                             \
    unsigned char *_p = (unsigned char *)(p);                                                      \
    unsigned char _v = (unsigned char)(v);                                                         \
    size_t _n = (n);                                                                               \
    while (_n-- > 0)                                                                               \
      *_p++ = _v;                                                                                  \
  } while (0)

#include "base/common.h"
#include "rt/shared.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#ifdef rt_argc
#undef rt_argc
#endif
#ifdef rt_argv
#undef rt_argv
#endif
#endif

#ifndef _WIN32
extern char **environ;
#endif

extern int64_t rt_lt(int64_t a, int64_t b);

int color_mode __attribute__((weak)) = 0;
int debug_enabled __attribute__((weak)) = 0;

int64_t rt_globals_ptr = 1;

/* Robust trace control: only print trace frames when -trace was used */
int g_trace_requested = 0;
int g_trace_suspended = 0;
static int g_trace_env_ready = 0;
static bool g_trace_env_trace = false;
static bool g_trace_env_calls = false;
static bool g_trace_env_values = false;
static bool g_trace_env_verbose = false;
static bool g_trace_env_index_read = false;
static const char *g_trace_env_filter = NULL;

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
#ifdef _MSC_VER
#define NY_TLS __declspec(thread)
#else
#define NY_TLS _Thread_local
#endif
static NY_TLS int64_t g_trace_file = 0;
static NY_TLS int64_t g_trace_line = 1;
static NY_TLS int64_t g_trace_col = 1;
static NY_TLS int64_t g_trace_func = 0;
#define TRACE_RING 32
static NY_TLS int64_t g_trace_files[TRACE_RING] = {0};
static NY_TLS int64_t g_trace_lines[TRACE_RING] = {0};
static NY_TLS int64_t g_trace_cols[TRACE_RING] = {0};
static NY_TLS int64_t g_trace_funcs[TRACE_RING] = {0};
static NY_TLS size_t g_trace_len = 0;
static NY_TLS size_t g_trace_idx = 0;
static int g_index_read_probe_mode = -1;

#define RT_SSO_MAX 23
#define RT_SSO_SLOTS_PER_BLOCK 1024

typedef struct rt_sso_slot {
  uint64_t fake_magic;
  uint64_t fake_size;
  uint64_t len_tag;
  uint64_t tag;
  char bytes[RT_SSO_MAX + 1];
} rt_sso_slot_t;

typedef struct rt_sso_block {
  struct rt_sso_block *next;
  size_t used;
  rt_sso_slot_t slots[RT_SSO_SLOTS_PER_BLOCK];
} rt_sso_block_t;

static NY_TLS rt_sso_block_t *g_sso_strings = NULL;

static int64_t rt_alloc_small_string_len(const char *s, size_t len) {
  if (len > RT_SSO_MAX)
    return 0;
  if (!g_sso_strings || g_sso_strings->used >= RT_SSO_SLOTS_PER_BLOCK) {
    rt_sso_block_t *block = (rt_sso_block_t *)calloc(1, sizeof(*block));
    if (!block)
      return 0;
    block->next = g_sso_strings;
    g_sso_strings = block;
  }
  rt_sso_slot_t *slot = &g_sso_strings->slots[g_sso_strings->used++];
  slot->fake_magic = 0;
  slot->fake_size = 0;
  slot->len_tag = ((uint64_t)len << 1) | 1u;
  slot->tag = TAG_STR;
  memcpy(slot->bytes, s, len);
  slot->bytes[len] = '\0';
  return (int64_t)(uintptr_t)slot->bytes;
}

void rt_cleanup_small_strings(void) {
  rt_sso_block_t *block = g_sso_strings;
  g_sso_strings = NULL;
  while (block) {
    rt_sso_block_t *next = block->next;
    free(block);
    block = next;
  }
}

/* Live call stack - push on enter, pop on exit */
#define CALL_STACK_MAX 512
static NY_TLS int64_t g_cs_files[CALL_STACK_MAX];
static NY_TLS int64_t g_cs_lines[CALL_STACK_MAX];
static NY_TLS int64_t g_cs_funcs[CALL_STACK_MAX];
static NY_TLS size_t g_cs_depth = 0;

int64_t rt_trace_dump(int64_t count);

void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func, const char *prefix) {
  if (!is_v_str(file))
    return;
  const char *fname = (const char *)(uintptr_t)file;
  size_t flen = rt_tagged_str_len(file);
  int64_t l = is_int(line) ? rt_untag_v(line) : line;
  int64_t c = is_int(col) ? rt_untag_v(col) : col;

  const char *pre = prefix ? prefix : "";
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *c2 = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";

  fprintf(stderr, "%s%s%.*s:%s%ld:%ld%s", pre, c1, (int)flen, fname, c2, (long)l, (long)c, rs);
  if (is_v_str(func)) {
    const char *fn = (const char *)(uintptr_t)func;
    size_t fnlen = rt_tagged_str_len(func);
    const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
    fprintf(stderr, " (%sfn %.*s%s)", fnc, (int)fnlen, fn, rs);
  }
  fputc('\n', stderr);
}

static bool trace_func_matches(int64_t func, const char *prefix) {
  if (!prefix || !*prefix || !is_v_str(func))
    return false;
  const char *fn = (const char *)(uintptr_t)func;
  size_t fnlen = rt_tagged_str_len(func);
  size_t plen = strlen(prefix);
  return fnlen >= plen && strncmp(fn, prefix, plen) == 0;
}

static bool trace_is_internal_helper(int64_t func) {
  if (!is_v_str(func))
    return false;
  return trace_func_matches(func, "std.core.reflect._") ||
         trace_func_matches(func, "std.core.reflect.repr") ||
         trace_func_matches(func, "std.core.reflect.to_str") ||
         trace_func_matches(func, "std.core.reflect.type") ||
         trace_func_matches(func, "std.core.len") || trace_func_matches(func, "std.core.get") ||
         trace_func_matches(func, "std.core.put") || trace_func_matches(func, "std.core.append") ||
         trace_func_matches(func, "std.core.pop") || trace_func_matches(func, "std.core.extend") ||
         trace_func_matches(func, "std.core.contains") ||
         trace_func_matches(func, "std.core.slice") ||
         trace_func_matches(func, "std.core.error.panic");
}

static void rt_trace_dump_filtered(size_t want) {
  if (!g_trace_requested || g_trace_len == 0)
    return;
  size_t shown = 0;
  for (size_t i = 0; i < g_trace_len && shown < want; i++) {
    size_t idx = (g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING;
    int64_t func = g_trace_funcs[idx];
    if (trace_is_internal_helper(func))
      continue;
    print_trace_entry(g_trace_files[idx], g_trace_lines[idx], g_trace_cols[idx], func, "  at ");
    shown++;
  }
  if (shown == 0) {
    rt_trace_dump(((int64_t)want << 1) | 1);
  }
}

#define RT_PRINT_BUF_SIZE 65536
static char rt_print_buf[RT_PRINT_BUF_SIZE];
static uint32_t rt_print_pos = 0;
static int rt_stdout_is_tty = -1;

int64_t rt_print_flush(void) {
  if (rt_print_pos > 0) {
    fwrite(rt_print_buf, 1, (size_t)rt_print_pos, stdout);
    rt_print_pos = 0;
  }
  return 1;
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
    rt_print_flush();
}

static inline void rt_print_put(const char *s, size_t len) {
  if (rt_print_pos + len > RT_PRINT_BUF_SIZE) {
    rt_print_flush();
    if (len > RT_PRINT_BUF_SIZE) {
      fwrite(s, 1, len, stdout);
      return;
    }
  }
  memcpy(rt_print_buf + rt_print_pos, s, len);
  rt_print_pos += (uint32_t)len;
}

int64_t rt_write_buffered(int64_t fd, int64_t buf, int64_t len) {
  intptr_t rfd = (fd & 1) ? (fd >> 1) : (intptr_t)fd;
  intptr_t rlen = (len & 1) ? (len >> 1) : (intptr_t)len;
  if (rfd != 1) {
    extern int64_t rt_write_off(int64_t fd, int64_t buf, int64_t len, int64_t off);
    return rt_write_off(fd, buf, len, 0);
  }
  char *ptr = (char *)(uintptr_t)rt_untag_v(buf);
  rt_print_put(ptr, (size_t)rlen);
  return len;
}

int64_t rt_print_str_raw(int64_t v) {
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

int64_t rt_print_int(int64_t v) {
  int64_t val = (int64_t)(v >> 1);
  if (rt_print_pos + 24 >= RT_PRINT_BUF_SIZE)
    rt_print_flush();

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

int64_t rt_print_newline(void) {
  if (rt_print_pos >= RT_PRINT_BUF_SIZE)
    rt_print_flush();
  rt_print_buf[rt_print_pos++] = '\n';
  rt_maybe_flush_line();
  return 1;
}

int64_t rt_alloc_string_len(const char *s, size_t len) {
  if (!s)
    return 0;
  if (len <= RT_SSO_MAX) {
    int64_t small = rt_alloc_small_string_len(s, len);
    if (small)
      return small;
  }
  int64_t p = rt_malloc((int64_t)((len + 1) * sizeof(char) << 1) | 1);
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
  memcpy((void *)(uintptr_t)p, s, len);
  ((char *)(uintptr_t)p)[len] = '\0';
  return p;
}

int64_t rt_alloc_string(const char *s) {
  if (!s)
    return 0;
  return rt_alloc_string_len(s, strlen(s));
}

int64_t rt_cstr_to_str(int64_t p_v) {
  int64_t raw = p_v;
  if (NY_NATIVE_IS(p_v))
    raw = (int64_t)(uintptr_t)NY_NATIVE_DECODE(p_v);
  if (!raw)
    return 0;
  return rt_alloc_string((const char *)(uintptr_t)raw);
}

static void trace_print_loc(void) {
  print_trace_entry(g_trace_file, g_trace_line, g_trace_col, g_trace_func, "[trace] ");
}

static void trace_record(int64_t file, int64_t line, int64_t col, int64_t func) {
  g_trace_files[g_trace_idx] = file;
  g_trace_lines[g_trace_idx] = line;
  g_trace_cols[g_trace_idx] = col;
  g_trace_funcs[g_trace_idx] = func;
  g_trace_idx = (g_trace_idx + 1) % TRACE_RING;
  if (g_trace_len < TRACE_RING)
    g_trace_len++;
}

static bool trace_env_enabled_uncached(const char *name) {
  const char *env = getenv(name);
  return env && *env && strcmp(env, "0") != 0;
}

void rt_trace_refresh_env(void) {
  g_trace_env_trace = trace_env_enabled_uncached("NYTRIX_TRACE");
  g_trace_env_calls = trace_env_enabled_uncached("NYTRIX_TRACE_CALLS");
  g_trace_env_values = trace_env_enabled_uncached("NYTRIX_TRACE_VALUES");
  g_trace_env_verbose = trace_env_enabled_uncached("NYTRIX_TRACE_VERBOSE");
  g_trace_env_index_read = trace_env_enabled_uncached("NYTRIX_INDEX_READ_PARITY");
  g_trace_env_filter = getenv("NYTRIX_TRACE_FILTER");
  g_trace_env_ready = 1;
  g_index_read_probe_mode = g_trace_env_index_read ? 1 : 0;
}

static void trace_env_ensure(void) {
  if (!g_trace_env_ready)
    rt_trace_refresh_env();
}

static bool trace_env_enabled(const char *name) {
  trace_env_ensure();
  if (strcmp(name, "NYTRIX_TRACE") == 0)
    return g_trace_env_trace;
  if (strcmp(name, "NYTRIX_TRACE_CALLS") == 0)
    return g_trace_env_calls;
  if (strcmp(name, "NYTRIX_TRACE_VALUES") == 0)
    return g_trace_env_values;
  if (strcmp(name, "NYTRIX_TRACE_VERBOSE") == 0)
    return g_trace_env_verbose;
  if (strcmp(name, "NYTRIX_INDEX_READ_PARITY") == 0)
    return g_trace_env_index_read;
  return trace_env_enabled_uncached(name);
}

static bool index_read_probe_enabled_raw(void) {
  if (g_index_read_probe_mode >= 0)
    return g_index_read_probe_mode != 0;
  g_index_read_probe_mode = trace_env_enabled("NYTRIX_INDEX_READ_PARITY") ? 1 : 0;
  return g_index_read_probe_mode != 0;
}

int64_t rt_index_read_probe_enabled(void) {
  return index_read_probe_enabled_raw() ? NY_IMM_TRUE : NY_IMM_FALSE;
}

int64_t rt_index_read_probe(int64_t tag, int64_t idx, int64_t path) {
  if (!index_read_probe_enabled_raw())
    return 0;
  int64_t raw_tag = is_int(tag) ? rt_untag_v(tag) : tag;
  int64_t raw_idx = is_int(idx) ? rt_untag_v(idx) : idx;
  int64_t raw_path = is_int(path) ? rt_untag_v(path) : path;
  const char *path_name = (raw_path == 1) ? "fast" : "slow";
  fprintf(stderr, "[parity:index] tag=%" PRId64 " index=%" PRId64 " path=%s\n", raw_tag, raw_idx,
          path_name);
  return 0;
}

static bool trace_locations_enabled(void) { return trace_env_enabled("NYTRIX_TRACE_VERBOSE"); }

static bool trace_values_enabled(void) { return trace_env_enabled("NYTRIX_TRACE_VALUES"); }

static bool trace_calls_enabled(void) {
  return trace_env_enabled("NYTRIX_TRACE_CALLS") || trace_values_enabled() ||
         trace_locations_enabled();
}

static bool trace_filter_matches_token(const char *func, size_t fnlen, const char *tok,
                                       size_t toklen) {
  if (!func || !tok || toklen == 0)
    return false;
  if (toklen == 1 && tok[0] == '*')
    return true;
  if (toklen > 1 && tok[toklen - 1] == '*') {
    toklen--;
    return fnlen >= toklen && strncmp(func, tok, toklen) == 0;
  }
  for (size_t i = 0; i + toklen <= fnlen; ++i) {
    if (memcmp(func + i, tok, toklen) == 0)
      return true;
  }
  return false;
}

static bool trace_filter_allows(int64_t func) {
  trace_env_ensure();
  const char *filter = g_trace_env_filter;
  if (!filter || !*filter)
    return true;
  if (!is_v_str(func))
    return false;
  const char *fn = (const char *)(uintptr_t)func;
  size_t fnlen = rt_tagged_str_len(func);
  const char *p = filter;
  while (*p) {
    while (*p && (isspace((unsigned char)*p) || *p == ','))
      p++;
    const char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != ',')
      p++;
    size_t toklen = (size_t)(p - start);
    if (trace_filter_matches_token(fn, fnlen, start, toklen))
      return true;
  }
  return false;
}

static bool trace_should_print_func(int64_t func) {
  if (g_trace_suspended)
    return false;
  if (!trace_env_enabled("NYTRIX_TRACE") && !g_trace_requested)
    return false;
  if (trace_is_internal_helper(func))
    return false;
  return trace_filter_allows(func);
}

static void trace_print_indent(size_t depth) {
  size_t spaces = depth * 2;
  if (spaces > 40)
    spaces = 40;
  for (size_t i = 0; i < spaces; ++i)
    fputc(' ', stderr);
}

static void trace_print_call(int64_t file, int64_t line, int64_t func, size_t depth) {
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *c2 = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  trace_print_indent(depth);
  fprintf(stderr, "%s[trace]%s -> ", c1, rs);
  if (is_v_str(func)) {
    const char *fn = (const char *)(uintptr_t)func;
    size_t fnlen = rt_tagged_str_len(func);
    fprintf(stderr, "%s%.*s%s", fnc, (int)fnlen, fn, rs);
  } else {
    fprintf(stderr, "<anon>");
  }
  if (is_v_str(file)) {
    const char *fname = (const char *)(uintptr_t)file;
    size_t flen = rt_tagged_str_len(file);
    long l = (long)(is_int(line) ? rt_untag_v(line) : line);
    fprintf(stderr, " %s@ %.*s:%ld%s", c2, (int)flen, fname, l, rs);
  }
  fputc('\n', stderr);
}

static void trace_print_return_prefix(int64_t func, size_t depth) {
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  trace_print_indent(depth);
  fprintf(stderr, "%s[trace]%s <- ", c1, rs);
  if (is_v_str(func)) {
    const char *fn = (const char *)(uintptr_t)func;
    size_t fnlen = rt_tagged_str_len(func);
    fprintf(stderr, "%s%.*s%s", fnc, (int)fnlen, fn, rs);
  } else {
    fprintf(stderr, "<anon>");
  }
}

static void trace_print_return_raw_suffix(const char *value, size_t len) {
  if (value && len > 0)
    fprintf(stderr, " = %.*s", (int)len, value);
  fputc('\n', stderr);
}

static void trace_print_return_tagged_value(int64_t v) {
  extern int64_t rt_to_str(int64_t v);
  int64_t s_obj = rt_to_str(v);
  if (!is_v_str(s_obj)) {
    trace_print_return_raw_suffix("<value>", 7);
    return;
  }
  const char *s = (const char *)(uintptr_t)s_obj;
  size_t len = rt_tagged_str_len(s_obj);
  trace_print_return_raw_suffix(s, len);
}

static void trace_print_return_i64_value(int64_t v, bool is_unsigned) {
  char buf[64];
  int len = is_unsigned ? snprintf(buf, sizeof(buf), "%" PRIu64, (uint64_t)v)
                        : snprintf(buf, sizeof(buf), "%" PRId64, v);
  if (len < 0)
    len = 0;
  trace_print_return_raw_suffix(buf, (size_t)len);
}

static void trace_print_return_bool_value(int64_t v) {
  trace_print_return_raw_suffix(v ? "true" : "false", v ? 4 : 5);
}

static void trace_print_return_ptr_value(int64_t v) {
  char buf[64];
  int len = snprintf(buf, sizeof(buf), "0x%" PRIx64, (uint64_t)v);
  if (len < 0)
    len = 0;
  trace_print_return_raw_suffix(buf, (size_t)len);
}

static void trace_print_return_f64_bits_value(int64_t bits) {
  char buf[96];
  double d = 0.0;
  uint64_t u = (uint64_t)bits;
  memcpy(&d, &u, sizeof(d));
  int len = snprintf(buf, sizeof(buf), "%g", d);
  if (len < 0)
    len = 0;
  trace_print_return_raw_suffix(buf, (size_t)len);
}

int64_t rt_globals_get(void) { return rt_globals_ptr; }
int64_t rt_globals_set(int64_t p) {
  rt_globals_ptr = p;
  return p;
}

int64_t rt_fix_fn_ptr(int64_t fn) {
#if UINTPTR_MAX > 0xffffffff
  uint64_t raw = (uint64_t)fn;
  uint64_t hi = raw >> 32;
  if (hi == 0ULL || hi == 0xffffffffULL) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    uint64_t ra_hi = ((uint64_t)ra) >> 32;
    if (ra_hi != 0ULL && ra_hi != 0xffffffffULL) {
      uint64_t fixed = (ra_hi << 32) | (raw & 0xffffffffULL);
      if (fixed != raw && rt_addr_readable((uintptr_t)fixed, 1))
        return (int64_t)fixed;
    }
  }
#endif
  return fn;
}

int64_t rt_push_defer(int64_t fn, int64_t env) {
  fn = rt_fix_fn_ptr(fn);
  vec_push(&g_defer_stack, ((defer_t){fn, env}));
  return 0;
}

int64_t rt_pop_run_defer(void) {
  if (g_defer_stack.len > 0) {
    defer_t d = g_defer_stack.data[--g_defer_stack.len];
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))d.fn;
    if (f)
      f(d.env);
  }
  return 0;
}

int64_t rt_run_defers_to(int64_t target_len_v) {
  size_t target_len = (size_t)(is_int(target_len_v) ? (target_len_v >> 1) : target_len_v);
  while (g_defer_stack.len > target_len) {
    defer_t d = g_defer_stack.data[--g_defer_stack.len];
    int64_t (*f)(int64_t) = (int64_t (*)(int64_t))d.fn;
    if (f)
      f(d.env);
  }
  return 0;
}

int64_t rt_set_panic_env(void *env_ptr) {
  panic_env_t pe = {(NY_JMP_BUF *)env_ptr, g_defer_stack.len};
  vec_push(&g_panic_env_stack, pe);
  return 0;
}

int64_t rt_clear_panic_env(void) {
  if (g_panic_env_stack.len > 0) {
    g_panic_env_stack.len--;
  }
  return 0;
}

int64_t rt_jmpbuf_size(void) { return (int64_t)sizeof(NY_JMP_BUF); }
int64_t rt_jmpbuf_align(void) { return (int64_t)_Alignof(NY_JMP_BUF); }
int64_t rt_get_panic_val(void) { return g_panic_value; }

int64_t rt_trace_loc(int64_t file, int64_t line, int64_t col) {
  g_trace_file = file;
  g_trace_line = line;
  g_trace_col = col;
  if (g_trace_suspended)
    return 0;
  trace_record(file, line, col, g_trace_func);
  if (trace_locations_enabled() && is_v_str(g_trace_func) && trace_should_print_func(g_trace_func))
    trace_print_loc();
  return 0;
}

int64_t rt_trace_func(int64_t name) {
  g_trace_func = name;
  return 0;
}

int64_t rt_trace_last_file(void) { return g_trace_file; }
int64_t rt_trace_last_line(void) { return g_trace_line; }
int64_t rt_trace_last_col(void) { return g_trace_col; }
int64_t rt_trace_last_func(void) { return g_trace_func; }

static void print_rt_snippet(int64_t file_ptr, int64_t line_ptr, int64_t col_ptr) {
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
    while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r' || buf[blen - 1] == ' ')) {
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

int64_t rt_trace_dump(int64_t count) {
  if (g_trace_len == 0)
    return 0;
  size_t want = (size_t)(is_int(count) ? rt_untag_v(count) : count);
  if (want == 0 || want > g_trace_len)
    want = g_trace_len;

  // Print newest first
  for (size_t i = 0; i < want; i++) {
    size_t idx = (g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING;
    print_trace_entry(g_trace_files[idx], g_trace_lines[idx], g_trace_cols[idx], g_trace_funcs[idx],
                      "  at ");
  }
  return 0;
}

int64_t rt_trace_get_frames(int64_t *f, int64_t *l, int64_t *c, int64_t *fn, int count) {
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

int64_t rt_trace_enter(int64_t func, int64_t file, int64_t line) {
  g_trace_func = func;
  g_trace_file = file;
  g_trace_line = line;
  g_trace_col = 1;
  if (g_trace_suspended)
    return 0;
  trace_record(file, line, 1, func);
  /* push onto live call stack */
  if (g_cs_depth < CALL_STACK_MAX) {
    g_cs_files[g_cs_depth] = file;
    g_cs_lines[g_cs_depth] = line;
    g_cs_funcs[g_cs_depth] = func;
    g_cs_depth++;
  }
  if (trace_calls_enabled() && trace_should_print_func(func)) {
    size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
    trace_print_call(file, line, func, depth);
  }
  return 0;
}

int64_t rt_trace_exit(void) {
  if (g_trace_suspended)
    return 0;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (trace_calls_enabled() && !trace_values_enabled() && trace_should_print_func(func)) {
    trace_print_return_prefix(func, depth);
    fputc('\n', stderr);
  }
  if (g_cs_depth > 0) {
    g_cs_depth--;
    if (g_cs_depth > 0) {
      g_trace_func = g_cs_funcs[g_cs_depth - 1];
    } else {
      g_trace_func = 0;
    }
  }
  return 0;
}

int64_t rt_trace_ret_void(void) {
  if (g_trace_suspended)
    return 0;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return 0;
  trace_print_return_prefix(func, depth);
  fputc('\n', stderr);
  return 0;
}

int64_t rt_trace_ret_tagged(int64_t v) {
  if (g_trace_suspended)
    return v;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return v;
  trace_print_return_prefix(func, depth);
  trace_print_return_tagged_value(v);
  return v;
}

int64_t rt_trace_ret_i64(int64_t v) {
  if (g_trace_suspended)
    return v;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return v;
  trace_print_return_prefix(func, depth);
  trace_print_return_i64_value(v, false);
  return v;
}

int64_t rt_trace_ret_u64(int64_t v) {
  if (g_trace_suspended)
    return v;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return v;
  trace_print_return_prefix(func, depth);
  trace_print_return_i64_value(v, true);
  return v;
}

int64_t rt_trace_ret_bool(int64_t v) {
  if (g_trace_suspended)
    return v;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return v;
  trace_print_return_prefix(func, depth);
  trace_print_return_bool_value(v);
  return v;
}

int64_t rt_trace_ret_ptr(int64_t v) {
  if (g_trace_suspended)
    return v;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return v;
  trace_print_return_prefix(func, depth);
  trace_print_return_ptr_value(v);
  return v;
}

int64_t rt_trace_ret_f64_bits(int64_t bits) {
  if (g_trace_suspended)
    return bits;
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return bits;
  trace_print_return_prefix(func, depth);
  trace_print_return_f64_bits_value(bits);
  return bits;
}

int64_t rt_trace_get_call_stack(int64_t *funcs, int64_t *files, int64_t *lines, int max_count) {
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

int64_t rt_argc_val = 1;
int64_t rt_envc_val = 1;
int64_t *rt_argv_ptr = NULL;
int64_t *rt_envp_ptr = NULL;
static int rt_native_argc_val = 0;
static char **rt_native_argv_ptr = NULL;
static char **rt_native_envp_ptr = NULL;

static atomic_flag g_env_snapshot_lock = ATOMIC_FLAG_INIT;

static inline void rt_env_snapshot_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_env_snapshot_lock, memory_order_acquire)) {
  }
}

static inline void rt_env_snapshot_unlock(void) {
  atomic_flag_clear_explicit(&g_env_snapshot_lock, memory_order_release);
}

static void rt_free_env_snapshot_unlocked(void) {
  if (!rt_envp_ptr)
    return;
  int envc = (rt_envc_val >> 1);
  for (int i = 0; i < envc; i++) {
    if (rt_envp_ptr[i])
      rt_free(rt_envp_ptr[i]);
  }
  rt_free((int64_t)(uintptr_t)rt_envp_ptr);
  rt_envp_ptr = NULL;
  rt_envc_val = 1;
}

static int rt_build_env_snapshot_unlocked(char **src_envp) {
  int env_count = 0;
  if (src_envp) {
    while (src_envp[env_count])
      env_count++;
  }
  int64_t *next =
      (int64_t *)(uintptr_t)rt_malloc(((int64_t)(env_count + 1) * sizeof(int64_t) << 1) | 1);
  if (!next)
    return -1;
  memset_manual(next, 0, (env_count + 1) * sizeof(int64_t));
  for (int i = 0; i < env_count; i++) {
    next[i] = rt_alloc_string(src_envp[i]);
  }
  rt_free_env_snapshot_unlocked();
  rt_envc_val = (env_count << 1) | 1;
  rt_envp_ptr = next;
  return 0;
}

static void rt_ensure_env_snapshot(void) {
  if (rt_envp_ptr)
    return;
  char **live_envp = NULL;
#ifdef _WIN32
  live_envp = _environ;
#else
  live_envp = rt_native_envp_ptr ? rt_native_envp_ptr : environ;
#endif
  rt_env_snapshot_lock();
  if (!rt_envp_ptr)
    (void)rt_build_env_snapshot_unlocked(live_envp);
  rt_env_snapshot_unlock();
}

static int rt_build_argv_snapshot(char **src_argv, int argc) {
  if (argc < 0)
    argc = 0;
  int64_t *next =
      (int64_t *)(uintptr_t)rt_malloc(((int64_t)(argc + 1) * sizeof(int64_t) << 1) | 1);
  if (!next)
    return -1;
  memset_manual(next, 0, (argc + 1) * sizeof(int64_t));
  for (int i = 0; i < argc; i++)
    next[i] = (src_argv && src_argv[i]) ? rt_alloc_string(src_argv[i]) : 0;
  rt_argv_ptr = next;
  return 0;
}

static void rt_ensure_argv_snapshot(void) {
  if (rt_argv_ptr)
    return;
  (void)rt_build_argv_snapshot(rt_native_argv_ptr, rt_native_argc_val);
}

int64_t rt_set_args(int64_t argc, int64_t argv_ptr, int64_t envp_ptr) {
  rt_cleanup_args();
  rt_argc_val = (argc << 1) | 1;
  rt_native_argc_val = (int)argc;
  rt_native_argv_ptr = (char **)argv_ptr;
  rt_native_envp_ptr = (char **)envp_ptr;
  if (rt_build_argv_snapshot(rt_native_argv_ptr, rt_native_argc_val) != 0)
    return -1;
  char **old_envp = rt_native_envp_ptr;
#ifndef _WIN32
  if (!old_envp)
    old_envp = environ;
#endif
  rt_env_snapshot_lock();
  int env_ok = rt_build_env_snapshot_unlocked(old_envp);
  rt_env_snapshot_unlock();
  if (env_ok != 0)
    return -1;
  return 0;
}

int _ny_aot_set_args(int argc, char **argv_ptr, char **envp_ptr) {
  if (rt_argv_ptr || rt_envp_ptr)
    rt_cleanup_args();
  rt_argc_val = ((int64_t)argc << 1) | 1;
  rt_native_argc_val = argc;
  rt_native_argv_ptr = argv_ptr;
  rt_native_envp_ptr = envp_ptr;
  return 0;
}

void rt_cleanup_args(void) {
  if (!rt_argv_ptr && !rt_envp_ptr) {
    rt_argc_val = 1;
    rt_native_argc_val = 0;
    rt_native_argv_ptr = NULL;
    rt_native_envp_ptr = NULL;
    return;
  }
  if (rt_argv_ptr) {
    int argc = (rt_argc_val >> 1);
    for (int i = 0; i < argc; i++) {
      if (rt_argv_ptr[i])
        rt_free(rt_argv_ptr[i]);
    }
    rt_free((int64_t)(uintptr_t)rt_argv_ptr);
    rt_argv_ptr = NULL;
  }
  rt_env_snapshot_lock();
  rt_free_env_snapshot_unlocked();
  rt_env_snapshot_unlock();
  rt_argc_val = 1;
  rt_native_argc_val = 0;
  rt_native_argv_ptr = NULL;
  rt_native_envp_ptr = NULL;
}

int64_t rt_argc(void) { return rt_argc_val; }
int64_t rt_envc(void) {
  rt_ensure_env_snapshot();
  return rt_envc_val;
}
int64_t rt_envp(void) {
  rt_ensure_env_snapshot();
  return (int64_t)rt_envp_ptr;
}
int64_t rt_argvp(void) {
  rt_ensure_argv_snapshot();
  return (int64_t)rt_argv_ptr;
}

int64_t rt_argv(int64_t i) {
  if (!is_int(i))
    return 0;
  rt_ensure_argv_snapshot();
  int idx = (int)(i >> 1);
  if (idx < 0 || idx >= (rt_argc_val >> 1) || !rt_argv_ptr)
    return 0;
  int64_t raw = rt_argv_ptr[idx];
  if (!raw)
    return 0;
  return rt_alloc_string((const char *)(uintptr_t)raw);
}

int64_t rt_tag(int64_t v) { return rt_tag_v(v); }
int64_t rt_untag(int64_t v) { return rt_untag_v(v); }
int64_t rt_is_int(int64_t v) { return is_int(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_ptr(int64_t v) { return is_ptr(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_ny_obj(int64_t v) { return is_ny_obj(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_str_obj(int64_t v) { return is_v_str(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_float_obj(int64_t v) { return is_v_flt(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
static int64_t rt_runtime_tag_raw(const char *s, size_t n) {
  if (!s)
    return 0;
  if (n == 3 && memcmp(s, "nil", 3) == 0)
    return 0;
  if (n == 3 && memcmp(s, "int", 3) == 0)
    return 1;
  if (n == 7 && memcmp(s, "ffi_ptr", 7) == 0)
    return 6;
  if (n == 4 && memcmp(s, "list", 4) == 0)
    return TAG_LIST;
  if (n == 4 && memcmp(s, "dict", 4) == 0)
    return TAG_DICT;
  if (n == 3 && memcmp(s, "set", 3) == 0)
    return TAG_SET;
  if (n == 5 && memcmp(s, "tuple", 5) == 0)
    return TAG_TUPLE;
  if (n == 2 && memcmp(s, "ok", 2) == 0)
    return TAG_OK;
  if (n == 3 && memcmp(s, "err", 3) == 0)
    return TAG_ERR;
  if (n == 5 && memcmp(s, "range", 5) == 0)
    return TAG_RANGE;
  if (n == 7 && memcmp(s, "closure", 7) == 0)
    return TAG_CLOSURE;
  if (n == 3 && memcmp(s, "ptr", 3) == 0)
    return TAG_CLOSURE;
  if (n == 5 && memcmp(s, "float", 5) == 0)
    return TAG_FLOAT;
  if (n == 7 && memcmp(s, "complex", 7) == 0)
    return TAG_COMPLEX;
  if (n == 3 && memcmp(s, "str", 3) == 0)
    return TAG_STR;
  if (n == 9 && memcmp(s, "str_const", 9) == 0)
    return TAG_STR_CONST;
  if (n == 5 && memcmp(s, "bytes", 5) == 0)
    return TAG_BYTES;
  if (n == 6 && memcmp(s, "bigint", 6) == 0)
    return TAG_BIGINT;
  if (n == 5 && memcmp(s, "kwarg", 5) == 0)
    return TAG_KWARG;
  return 0;
}

int64_t rt_runtime_tag(int64_t name) {
  if (!is_v_str(name))
    return rt_tag_v(0);
  const char *s = (const char *)(uintptr_t)name;
  size_t n = rt_tagged_str_len(name);
  return rt_tag_v(rt_runtime_tag_raw(s, n));
}

int64_t rt_has_tag(int64_t v, int64_t tag_v) {
  int64_t want = is_int(tag_v) ? (tag_v >> 1) : tag_v;
  if (want == TAG_FLOAT)
    return is_v_flt(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    uintptr_t p = (uintptr_t)heap_v;
    int64_t tag = *(int64_t *)((char *)p - 8);
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    return (tag >= 100 && tag <= 255 && tag == want) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (!is_ptr(v) || ((v)&7) != 0)
    return NY_IMM_FALSE;
  if (want != TAG_STR && want != TAG_STR_CONST)
    return NY_IMM_FALSE;
  if (want == TAG_STR_CONST && rt_const_str_cache_hit((uintptr_t)v))
    return NY_IMM_TRUE;
  if (rt_non_str_cache_hit((uintptr_t)v))
    return NY_IMM_FALSE;
  if (rt_header_readable_cached((uintptr_t)v - 8, 8)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
    if (tag != TAG_STR && tag != TAG_STR_CONST)
      rt_non_str_cache_store((uintptr_t)v);
    return ((tag == TAG_STR || tag == TAG_STR_CONST) && tag == want) ? NY_IMM_TRUE
                                                                     : NY_IMM_FALSE;
  }
  return NY_IMM_FALSE;
}
int64_t rt_tagof(int64_t v) {
  if (v == 0)
    return 0;
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    uintptr_t p = (uintptr_t)heap_v;
    int64_t tag = *(int64_t *)((char *)p - 8);
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    return rt_tag_v(tag);
  }
  if (is_int(v))
    return rt_tag_v(1);
  if ((v & 7) == 6)
    return rt_tag_v(6);
  if (is_v_flt(v))
    return rt_tag_v(TAG_FLOAT);
  if (!is_ptr(v))
    return 0;
  uintptr_t p = (uintptr_t)v;
  if (rt_const_str_cache_hit(p))
    return rt_tag_v(TAG_STR_CONST);
  if (rt_non_str_cache_hit(p))
    return 0;
  if (!rt_header_readable_cached(p - 8, 8))
    return 0;
  int64_t tag = *(int64_t *)((char *)p - 8);
  if (tag == TAG_STR_CONST)
    rt_const_str_cache_store(p);
  if (tag == TAG_STR || tag == TAG_STR_CONST)
    return rt_tag_v(tag);
  rt_non_str_cache_store(p);
  return 0;
}

int64_t rt_init_str(int64_t p, int64_t n_v) {
  if (!p)
    return 0;
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0)
    n = 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)p - 16) = rt_tag_v(n);
  return p;
}

int64_t rt_bytes_new(int64_t n_v) {
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0)
    n = 0;
  int64_t p = rt_malloc(rt_tag_v(n));
  if (!p)
    return 0;
  memset((void *)(uintptr_t)p, 0, (size_t)n);
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_BYTES;
  *(int64_t *)((char *)(uintptr_t)p - 16) = rt_tag_v(n);
  return p;
}

int64_t rt_kwarg_new(int64_t key, int64_t value) {
  int64_t p = rt_malloc(rt_tag_v(16));
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_KWARG;
  *(int64_t *)((char *)(uintptr_t)p + 0) = key;
  *(int64_t *)((char *)(uintptr_t)p + 8) = value;
  return p;
}

int64_t rt_errno_val = 1;
int64_t rt_errno(void) { return (int64_t)((errno << 1) | 1); }

int64_t rt_copy_mem(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  if (n <= 0)
    return dst;
  memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
  return dst;
}

extern atomic_uint_fast64_t g_ny_alloc_count;
extern atomic_uint_fast64_t g_ny_realloc_count;
atomic_uint_fast64_t g_ny_dict_probe_count = 0;

int64_t rt_inc_ny_counter(int64_t idx_v) {
  int64_t idx = rt_untag_v(idx_v);
  if (idx == 0)
    atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  else if (idx == 1)
    atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
  else if (idx == 2)
    atomic_fetch_add_explicit(&g_ny_dict_probe_count, 1, memory_order_relaxed);
  return idx_v;
}

int64_t rt_get_ny_counter(int64_t idx_v) {
  int64_t idx = rt_untag_v(idx_v);
  uint64_t val = 0;
  if (idx == 0)
    val = atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
  else if (idx == 1)
    val = atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed);
  else if (idx == 2)
    val = atomic_load_explicit(&g_ny_dict_probe_count, memory_order_relaxed);
  return rt_tag_v((int64_t)val);
}

int64_t rt_mat4_to_buffer(int64_t m_lst, int64_t buf_ptr) {
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

int64_t rt_mat4_from_buffer(int64_t m_lst, int64_t buf_ptr) {
  if (!is_ptr(m_lst) || !is_ptr(buf_ptr))
    return m_lst;
  const float *buf = (const float *)(uintptr_t)buf_ptr;
  for (int i = 0; i < 16; i++) {
    double dv = (double)buf[i];
    int64_t bits;
    memcpy(&bits, &dv, 8);
    int64_t boxed = rt_flt_box_val(bits);
    *(int64_t *)((char *)(uintptr_t)m_lst + 16 + (i + 2) * 8) = boxed;
  }
  return m_lst;
}

int64_t rt_result_alloc(int64_t tag, int64_t v) {
  int64_t sz = (int64_t)sizeof(int64_t);
  int64_t res = rt_malloc(((int64_t)sz << 1) | 1);
  if (!res)
    return 0;
  *(int64_t *)((char *)(uintptr_t)res - 8) = tag;
  *(int64_t *)((char *)(uintptr_t)res - 16) = (sz << 1) | 1;
  *(int64_t *)(uintptr_t)res = v;
  return res;
}

int64_t rt_result_ok(int64_t v) { return rt_result_alloc(TAG_OK, v); }

int64_t rt_result_err(int64_t e) { return rt_result_alloc(TAG_ERR, e); }

int64_t rt_is_ok(int64_t v) { return is_v_ok(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_err(int64_t v) { return is_v_err(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_unwrap(int64_t v) {
  if (is_v_ok(v) || is_v_err(v)) {
    return *(int64_t *)(uintptr_t)v;
  }
  return v;
}
int64_t rt_list_new(int64_t n_v) {
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0)
    n = 0;
  // Standard layout: 16 bytes header (length, capacity) + n * 8 bytes data
  int64_t p = rt_malloc(16 + n * 8);
  if (!p)
    return 0;
  // Standard Nytrix tags are at p-8
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_LIST;
  *(int64_t *)((char *)(uintptr_t)p + 0) = 1;            // Length = 0 (tagged: (0<<1)|1 = 1)
  *(int64_t *)((char *)(uintptr_t)p + 8) = (n << 1) | 1; // Capacity = n (tagged)

  return p;
}

int64_t rt_list_as_tuple(int64_t lst) {
  if (!is_ptr(lst) || !is_heap_ptr(lst))
    return lst;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
  if (tag == TAG_LIST || tag == TAG_TUPLE)
    *(int64_t *)((char *)(uintptr_t)lst - 8) = TAG_TUPLE;
  return lst;
}

int64_t rt_append(int64_t lst, int64_t val) {
  if (!is_ptr(lst))
    return lst;
  if (rt_tagof(lst) != ((TAG_LIST << 1) | 1))
    return lst;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  int64_t cap_v = *(int64_t *)((char *)(uintptr_t)lst + 8);
  int64_t cap = is_int(cap_v) ? (cap_v >> 1) : cap_v;

  if (n >= cap) {
    int64_t new_cap = cap == 0 ? 8 : (cap * 2);
    int64_t new_p = rt_malloc(16 + new_cap * 8);
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

int64_t rt_load_item(int64_t lst, int64_t i_v) { return rt_load_item_fast(lst, i_v); }

int64_t rt_store_item(int64_t lst, int64_t i_v, int64_t val) { return _rt_store_item_fast(lst, i_v, val); }

int64_t rt_load_item_fast(int64_t lst, int64_t i_v) { return _rt_load_item_fast(lst, i_v); }

int64_t rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  return _rt_store_item_fast(lst, i_v, val);
}

static int rt_sort_list_cmp(const void *ap, const void *bp) {
  int64_t a = *(const int64_t *)ap;
  int64_t b = *(const int64_t *)bp;
  if (rt_lt(a, b) == NY_IMM_TRUE)
    return -1;
  if (rt_lt(b, a) == NY_IMM_TRUE)
    return 1;
  return 0;
}

int64_t rt_sort_list(int64_t lst) {
  if (!is_ptr(lst))
    return lst;
  if (!is_heap_ptr(lst))
    return lst;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
  if (tag != TAG_LIST && tag != TAG_TUPLE)
    return lst;
  int64_t tagged_len = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
  if (n > 1)
    qsort((void *)((char *)(uintptr_t)lst + 16), (size_t)n, sizeof(int64_t), rt_sort_list_cmp);
  return lst;
}

static int rt_sort_char_cmp(const void *ap, const void *bp) {
  unsigned char a = *(const unsigned char *)ap;
  unsigned char b = *(const unsigned char *)bp;
  return (a > b) - (a < b);
}

static int64_t rt_sequence_tag(int64_t v) {
  if (!is_ptr(v) || !is_heap_ptr(v))
    return 0;
  return *(int64_t *)((char *)(uintptr_t)v - 8);
}

static int64_t rt_tagged_raw_i64(int64_t v) { return is_int(v) ? (v >> 1) : v; }

int64_t rt_range_new(int64_t start_v, int64_t stop_v, int64_t step_v) {
  int64_t start = rt_tagged_raw_i64(start_v);
  int64_t stop = rt_tagged_raw_i64(stop_v);
  int64_t step = rt_tagged_raw_i64(step_v);
  if (step == 0)
    step = 1;
  int64_t obj = rt_malloc(rt_tag_v(24));
  if (!obj)
    return 0;
  *(int64_t *)((char *)(uintptr_t)obj - 8) = TAG_RANGE;
  *(int64_t *)((char *)(uintptr_t)obj + 0) = rt_tag_v(start);
  *(int64_t *)((char *)(uintptr_t)obj + 8) = rt_tag_v(stop);
  *(int64_t *)((char *)(uintptr_t)obj + 16) = rt_tag_v(step);
  return obj;
}

static int64_t rt_list_copy_with_tag(int64_t src, int64_t tag) {
  int64_t n = rt_tagged_raw_i64(*(int64_t *)((char *)(uintptr_t)src + 0));
  if (n < 0)
    n = 0;
  int64_t out = rt_list_new(rt_tag_v(n));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(n);
  if (n > 0)
    memcpy((char *)(uintptr_t)out + 16, (char *)(uintptr_t)src + 16, (size_t)n * sizeof(int64_t));
  *(int64_t *)((char *)(uintptr_t)out - 8) = tag;
  return out;
}

static int64_t rt_range_len_raw(int64_t start, int64_t stop, int64_t step) {
  if (step == 0)
    return 0;
  if (step > 0) {
    if (start >= stop)
      return 0;
    return ((stop - start - 1) / step) + 1;
  }
  if (start <= stop)
    return 0;
  return ((start - stop - 1) / -step) + 1;
}

static int64_t rt_range_to_list(int64_t rng) {
  size_t hsz = rt_get_heap_size(rng);
  if (hsz < 24 || hsz > 32)
    return 0;
  int64_t start = rt_tagged_raw_i64(*(int64_t *)((char *)(uintptr_t)rng + 0));
  int64_t stop = rt_tagged_raw_i64(*(int64_t *)((char *)(uintptr_t)rng + 8));
  int64_t step = rt_tagged_raw_i64(*(int64_t *)((char *)(uintptr_t)rng + 16));
  int64_t n = rt_range_len_raw(start, stop, step);
  if (n < 0)
    n = 0;
  int64_t out = rt_list_new(rt_tag_v(n));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(n);
  int64_t cur = start;
  for (int64_t i = 0; i < n; i++) {
    *(int64_t *)((char *)(uintptr_t)out + 16 + i * 8) = rt_tag_v(cur);
    cur += step;
  }
  return out;
}

static int64_t rt_sorted_string_copy(int64_t s) {
  size_t len = rt_tagged_str_len(s);
  if (len <= 1)
    return s;
  char *buf = (char *)malloc(len);
  if (!buf)
    return s;
  memcpy(buf, (const void *)(uintptr_t)s, len);
  qsort(buf, len, sizeof(char), rt_sort_char_cmp);
  int64_t out = rt_alloc_string_len(buf, len);
  free(buf);
  return out ? out : s;
}

int64_t rt_sort_any(int64_t xs) {
  if (is_v_str(xs))
    return rt_sorted_string_copy(xs);
  int64_t tag = rt_sequence_tag(xs);
  if (tag == TAG_LIST)
    return rt_sort_list(xs);
  if (tag == TAG_TUPLE) {
    int64_t out = rt_list_copy_with_tag(xs, TAG_TUPLE);
    return out ? rt_sort_list(out) : xs;
  }
  if (tag == TAG_RANGE) {
    int64_t out = rt_range_to_list(xs);
    if (!out)
      return xs;
    return rt_sort_list(out);
  }
  return xs;
}

int64_t rt_sorted_any(int64_t xs) {
  if (is_v_str(xs))
    return rt_sorted_string_copy(xs);
  int64_t tag = rt_sequence_tag(xs);
  if (tag == TAG_LIST || tag == TAG_TUPLE) {
    int64_t out = rt_list_copy_with_tag(xs, TAG_LIST);
    return out ? rt_sort_list(out) : xs;
  }
  if (tag == TAG_RANGE) {
    int64_t out = rt_range_to_list(xs);
    if (!out)
      return xs;
    return rt_sort_list(out);
  }
  return rt_sort_any(xs);
}

/* Fast list-length read: reads the tagged length word at lst+0 and returns the
 * raw (untagged) element count.  Avoids the full rt_load64_idx bounds-check +
 * rt_addr_readable machinery when all we need is the length. */
int64_t rt_list_len(int64_t lst) {
  if (!is_ptr(lst))
    return 1; /* tagged 0 */
  int64_t tagged = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return tagged; /* caller uses tagged value directly in Nytrix arithmetic */
}

/* Fast list-length write: stores `n` (tagged integer) at lst+0. */
int64_t rt_list_set_len(int64_t lst, int64_t n) {
  if (!is_ptr(lst))
    return 0;
  int64_t tagged = is_int(n) ? n : rt_tag(n);
  *(int64_t *)((char *)(uintptr_t)lst + 0) = tagged;
  return tagged;
}

static bool rt_msg_in(const char *msg, size_t msg_len, const char *const *items,
                      size_t count) {
  for (size_t i = 0; i < count; ++i) {
    size_t want_len = strlen(items[i]);
    if (msg_len == want_len && memcmp(msg, items[i], want_len) == 0)
      return true;
  }
  return false;
}

static void print_panic_msg(int64_t msg_ptr) {
  const char *red = color_mode ? clr(NY_CLR_RED) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";

  if (is_int(msg_ptr)) {
    fprintf(stderr, "%sPanicError:%s %" PRId64 "\n", red, rs, (int64_t)rt_untag_v(msg_ptr));
  } else if (is_v_str(msg_ptr)) {
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    size_t msg_len = rt_tagged_str_len(msg_ptr);
    const char *kind = "PanicError";
    static const char *const zero_division_msgs[] = {
        "division by zero", "bigint division by zero", "modulo by zero",
        "bigint modulo by zero"};
    if (rt_msg_in(msg, msg_len, zero_division_msgs,
                  sizeof(zero_division_msgs) / sizeof(zero_division_msgs[0]))) {
      kind = "ZeroDivisionError";
    }
    fprintf(stderr, "%s%s:%s %.*s\n", red, kind, rs, (int)msg_len, msg);
  } else if (is_v_err(msg_ptr)) {
    int64_t err = rt_unwrap(msg_ptr);
    fprintf(stderr, "%sNytrixError:%s ", red, rs);
    print_panic_msg(err);
  } else {
    fprintf(stderr, "%sPanicError:%s 0x%" PRIx64 "\n", red, rs, (uint64_t)msg_ptr);
  }
}

static void print_ny_trace_frame(int64_t file, int64_t line, int64_t col, int64_t func) {
  if (!is_v_str(file))
    return;
  const char *fname = (const char *)(uintptr_t)file;
  size_t flen = rt_tagged_str_len(file);
  long l = (long)(is_int(line) ? rt_untag_v(line) : line);
  fprintf(stderr, "  at %.*s:%ld", (int)flen, fname, l);
  if (is_int(col))
    fprintf(stderr, ":%ld", (long)rt_untag_v(col));
  if (is_v_str(func)) {
    const char *fn = (const char *)(uintptr_t)func;
    size_t fnlen = rt_tagged_str_len(func);
    fprintf(stderr, " in %.*s", (int)fnlen, fn);
  }
  fputc('\n', stderr);
}

static void print_ny_trace_repeat(size_t count) {
  if (count == 0)
    return;
  const char *gray = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  fprintf(stderr, "%s  ... previous frame repeated %zu more time%s%s\n", gray, count,
          count == 1 ? "" : "s", rs);
}

int64_t rt_get_backtrace(int64_t count_v) {
  if (g_cs_depth == 0)
    return rt_list_new(0);
  size_t want = (size_t)(is_int(count_v) ? rt_untag_v(count_v) : count_v);
  if (want == 0 || want > g_cs_depth)
    want = g_cs_depth;
  int64_t lst = rt_list_new(is_int(want) ? (int64_t)want : rt_tag((int64_t)want));
  for (size_t i = 0; i < want; i++) {
    size_t idx = g_cs_depth - 1 - i; /* innermost first */
    int64_t frame = rt_list_new(rt_tag(3));
    rt_append(frame, g_cs_files[idx]);
    rt_append(frame, g_cs_lines[idx]);
    rt_append(frame, g_cs_funcs[idx]);
    rt_append(lst, frame);
  }
  return lst;
}

int64_t rt_panic(int64_t msg_ptr) {
  rt_print_flush();
  bool has_env = (g_panic_env_stack.len > 0);
  if (has_env) {
    g_panic_value = msg_ptr;
    panic_env_t pe = g_panic_env_stack.data[g_panic_env_stack.len - 1];
    rt_run_defers_to((int64_t)((pe.defer_base << 1) | 1));
    NY_LONGJMP(*pe.env, 1);
  }
  fputc('\n', stderr);
  fprintf(stderr, "Nytrix trace (most recent call last):\n");
  bool printed = false;
  if (g_cs_depth > 0) {
    size_t limit = g_cs_depth < 64 ? g_cs_depth : 64;
    size_t start = g_cs_depth > limit ? g_cs_depth - limit : 0;
    int64_t last_file = 0, last_line = 0, last_func = 0;
    size_t repeats = 0;
    bool have_last = false;
    for (size_t idx = start; idx < g_cs_depth; idx++) {
      if (trace_is_internal_helper(g_cs_funcs[idx]))
        continue;
      if (have_last && g_cs_files[idx] == last_file && g_cs_lines[idx] == last_line &&
          g_cs_funcs[idx] == last_func) {
        repeats++;
        continue;
      }
      print_ny_trace_repeat(repeats);
      repeats = 0;
      print_ny_trace_frame(g_cs_files[idx], g_cs_lines[idx], 1, g_cs_funcs[idx]);
      last_file = g_cs_files[idx];
      last_line = g_cs_lines[idx];
      last_func = g_cs_funcs[idx];
      have_last = true;
      printed = true;
    }
    print_ny_trace_repeat(repeats);
  } else if (g_trace_requested && g_trace_len > 0) {
    size_t avail = g_trace_len < TRACE_RING ? g_trace_len : TRACE_RING;
    size_t want = avail < 10 ? avail : 10;
    size_t start = avail > want ? avail - want : 0;
    int64_t last_file = 0, last_line = 0, last_func = 0;
    size_t repeats = 0;
    bool have_last = false;
    for (size_t i = 0; i < want; i++) {
      size_t idx = (g_trace_idx + TRACE_RING - avail + start + i) % TRACE_RING;
      if (trace_is_internal_helper(g_trace_funcs[idx]))
        continue;
      if (have_last && g_trace_files[idx] == last_file && g_trace_lines[idx] == last_line &&
          g_trace_funcs[idx] == last_func) {
        repeats++;
        continue;
      }
      print_ny_trace_repeat(repeats);
      repeats = 0;
      print_ny_trace_frame(g_trace_files[idx], g_trace_lines[idx], g_trace_cols[idx],
                           g_trace_funcs[idx]);
      last_file = g_trace_files[idx];
      last_line = g_trace_lines[idx];
      last_func = g_trace_funcs[idx];
      have_last = true;
      printed = true;
    }
    print_ny_trace_repeat(repeats);
  }
  if (!printed && is_v_str(g_trace_file)) {
    print_ny_trace_frame(g_trace_file, g_trace_line, g_trace_col, g_trace_func);
  }
  if (is_v_str(g_trace_file)) {
    print_rt_snippet(g_trace_file, g_trace_line, g_trace_col);
  }
  print_panic_msg(msg_ptr);
  fprintf(stderr, "\n");
  exit(1);
}

int64_t rt_breakpoint(void) {
#if defined(__x86_64__) || defined(__i386__)
  __asm__ volatile("int3");
#elif defined(__aarch64__)
  __asm__ volatile("brk #0");
#elif defined(__arm__)
  __asm__ volatile("bkpt #0");
#else
  raise(SIGTRAP);
#endif
  return 0;
}
