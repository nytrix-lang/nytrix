/*
 * FFI runtime: foreign-function interface glue for calling native
 * C functions from Nytrix code with ABI-compliant argument marshalling.
 */
#include "code/runtime/shared.h"
#include <string.h>

#ifdef _WIN32

#else
#include <dlfcn.h>
#endif

#if UINTPTR_MAX == 0xffffffff
#define NY_NATIVE_RET0(fn) (int64_t)((intptr_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0) (int64_t)((intptr_t (*)(intptr_t))(fn))((intptr_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                                                 \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t))(fn))((intptr_t)(a0), (intptr_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                                                             \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t))(fn))((intptr_t)(a0), (intptr_t)(a1),      \
                                                              (intptr_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                                                         \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t))(fn))(                           \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                                                     \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t))(fn))(                 \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3), (intptr_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                                                 \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, intptr_t))(fn))(       \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3), (intptr_t)(a4),              \
      (intptr_t)(a5))
#else
#define NY_NATIVE_RET0(fn) ((int64_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0) ((int64_t (*)(int64_t))(fn))((int64_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                                                 \
  ((int64_t (*)(int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                                                             \
  ((int64_t (*)(int64_t, int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1), (int64_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                                                         \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1),            \
                                                          (int64_t)(a2), (int64_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                                                     \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(fn))(                                \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3), (int64_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                                                 \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(fn))(                       \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3), (int64_t)(a4), (int64_t)(a5))
#endif

#ifdef _WIN32
static int64_t rt_make_str_ffi(const char *s) {
  if (!s)
    return 0;
  size_t len = strlen(s);
  int64_t res = rt_malloc(((int64_t)len + 1) << 1 | 1);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
  memcpy((char *)(uintptr_t)res, s, len + 1);
  return res;
}
#endif

int64_t rt_tag_native(int64_t addr) {
  if (!addr)
    return 0;
  if (NY_NATIVE_IS(addr))
    return addr;
  return NY_NATIVE_ENCODE((void *)(uintptr_t)addr);
}

int64_t rt_dlopen(int64_t name, int64_t flags) {
  const char *p = NULL;
  if (name && !is_int(name))
    p = (const char *)name;
  else if (is_int(name) && (name >> 1) != 0)
    p = (const char *)(uintptr_t)(name >> 1);
#ifdef _WIN32
  (void)flags;
  if (!p)
    return 0;
  void *h = (void *)LoadLibraryA(p);
  return h ? NY_NATIVE_ENCODE(h) : 0;
#else
  void *h = dlopen(p, is_int(flags) ? (int)(flags >> 1) : (int)flags);
  return h ? NY_NATIVE_ENCODE(h) : 0;
#endif
}

int64_t rt_dlsym(int64_t handle, int64_t name) {
  void *p = NULL;
  void *h = NULL;
  if (NY_NATIVE_IS(handle)) {
    h = NY_NATIVE_DECODE(handle);
  } else if (is_int(handle)) {
    h = (void *)(uintptr_t)(handle >> 1);
  } else {
    h = (void *)(uintptr_t)handle;
  }
  const char *nm = (!is_int(name)) ? (const char *)name : (const char *)(uintptr_t)(name >> 1);
#ifdef _WIN32
  p = (void *)GetProcAddress((HMODULE)h, nm);
#else
  p = dlsym(h, nm);
#endif
  if (!p)
    return 0;
  return NY_NATIVE_ENCODE(p);
}

int64_t rt_dlerror(void) {
#ifdef _WIN32
  DWORD err = GetLastError();
  if (err == 0)
    return 0;
  LPSTR msg = NULL;
  DWORD flags =
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  if (!FormatMessageA(flags, NULL, err, 0, (LPSTR)&msg, 0, NULL) || !msg)
    return 0;
  int64_t s = rt_make_str_ffi(msg);
  LocalFree(msg);
  return s;
#else
  return (int64_t)dlerror();
#endif
}

int64_t rt_dlclose(int64_t handle) {
  void *h = NULL;
  if (NY_NATIVE_IS(handle)) {
    h = NY_NATIVE_DECODE(handle);
  } else if (is_int(handle)) {
    h = (void *)(uintptr_t)(handle >> 1);
  } else {
    h = (void *)(uintptr_t)handle;
  }
#ifdef _WIN32
  if (!h)
    return -1;
  return FreeLibrary((HMODULE)h) ? 0 : -1;
#else
  return dlclose(h);
#endif
}
int64_t rt_ffi_untag_ptr(int64_t v) { return rt_untag_v(v); }

#define UNTAG(x) rt_untag_v(x)

static inline int64_t rt_prepare_raw_callable(int64_t f) {
  return rt_fix_fn_ptr(f);
}

static int64_t rt_dynamic_callable_base(int64_t fn) {
  if (!fn)
    return 0;
  /*
   * Only strip an existing dynamic-callable encoding here.  The former
   * NY_NATIVE_DECODE branch fired on plain code addresses whose low three
   * bits equal NY_NATIVE_TAG (a raw lambda symbol like ...f76), so marking
   * halved the pointer and the callback dispatch jumped into address/64.
   * Native-encoded callables still work untouched: the call-time decode
   * path handles them after the dynamic-callable decode.
   */
  if (NY_DYNAMIC_CALLABLE_IS(fn))
    return (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(fn);
  return fn;
}

int64_t rt_mark_dynamic_callable(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_ENCODE((void *)(uintptr_t)fn) : 0;
}

int64_t rt_mark_dynamic_bool_callable(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_BOOL_ENCODE((void *)(uintptr_t)fn) : 0;
}

int64_t rt_mark_dynamic_bool_callable_tagged_args(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_ENCODE((void *)(uintptr_t)fn) |
                  NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK
            : 0;
}

int64_t rt_mark_dynamic_callable_tagged_args(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_ENCODE((void *)(uintptr_t)fn) |
                  NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK
            : 0;
}

int64_t rt_mark_dynamic_callable_raw_result(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_ENCODE((void *)(uintptr_t)fn) |
                  NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK
            : 0;
}

int64_t rt_mark_dynamic_callable_tagged_args_raw_result(int64_t fn) {
  fn = rt_dynamic_callable_base(fn);
  return fn ? NY_DYNAMIC_CALLABLE_ENCODE((void *)(uintptr_t)fn) |
                  NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                  NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK
            : 0;
}

/*
 * Native NYIR functions represent each `any` parameter as three i64 ABI
 * slots: value, dynamic length, and raw runtime tag.  The ordinary rt_callN
 * helpers intentionally preserve the legacy one-slot C ABI, so registry
 * callbacks need this explicit adapter.
 */
typedef int64_t (*rt_any_fn6)(int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn7)(int64_t, int64_t, int64_t, int64_t,
                                     int64_t, int64_t, int64_t);

static int64_t rt_any_len(int64_t value) {
  return rt_sequence_len_safe(value);
}

static int64_t rt_any_tag(int64_t value) {
  return rt_value_tag(value);
}

static int64_t rt_dynamic_callback_arg(int64_t value, bool raw_scalars) {
  /*
   * Native tbuf reads may box a raw string pointer as an integer-tagged
   * pointer. Dynamic string operations consume the pointer form, while
   * ordinary integers must remain tagged for any arithmetic.
   */
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      return decoded;
  }
  if (raw_scalars && is_int(value)) {
    int64_t raw = rt_untag_v(value);
    if (rt_native_is_str(raw))
      return raw;
    /*
     * Native lambda bodies still use the raw scalar ABI for inferred
     * parameters.  Dynamic callable marking describes the callable edge,
     * not a request to double-tag its integer argument.
     */
    return raw;
  }
  return value;
}

/*
 * Native callbacks cross a mixed raw/dynamic boundary. Comparison
 * instructions use raw 0/1, while dynamic arithmetic and managed objects
 * already carry canonical values. Normalize only the former here.
 */
static int64_t rt_any_callback_result(int64_t value) {
  if (value == NY_IMM_NIL || rt_is_bool_imm(value) || is_v_flt(value))
    return value;
  int64_t tag = rt_value_tag(value);
  /*
   * Native callback bodies return raw scalar integers.  `is_int` alone is
   * not a sufficient discriminator here: raw odd values look like tagged
   * integers to the low-bit predicate.  Preserve real heap/string handles,
   * and box every remaining scalar result for the caller's any ABI.
   */
  if (is_ptr(value) || tag >= 100 || rt_native_is_str(value))
    return value;
  return rt_tag_v(value);
}

static int64_t rt_dynamic_callback_result(int64_t value, bool returns_bool,
                                          bool raw_scalars,
                                          bool raw_result) {
  /*
   * A callback returning a native C-string must be promoted before it is
   * consumed by a managed string builder or concatenation routine.
   */
  if (!returns_bool && rt_native_is_str(value))
    return rt_cstr_to_str(value);
  if (returns_bool && (value == 0 || value == 1))
    /*
     * Native control-flow lowering consumes callback predicates as raw 0/1;
     * keep the predicate ABI distinct from an `any` boolean immediate.
     */
    return value ? 1 : 0;
  if (raw_result) {
    if (value == NY_IMM_NIL || rt_is_bool_imm(value) || is_v_flt(value) ||
        is_ptr(value) || is_heap_ptr(value) || rt_native_is_str(value) ||
        NY_NATIVE_IS(value))
      return value;
    return rt_tag_v(value);
  }
  if (!raw_scalars) {
    /*
     * Dynamic lambda bodies already return Ny values.  In particular, a
     * tagged integer must not be tagged a second time at the callback edge.
     */
    if (value == NY_IMM_NIL || rt_is_bool_imm(value) || is_int(value) ||
        is_ptr(value) || is_heap_ptr(value) || rt_native_is_str(value) ||
        NY_NATIVE_IS(value))
      return value;
    return rt_tag_v(value);
  }
  if (value == NY_IMM_NIL || rt_is_bool_imm(value) || is_v_flt(value))
    return value;
  int64_t tag = rt_value_tag(value);
  /*
   * Native callback bodies return raw scalar integers. `is_int` alone is
   * not a sufficient discriminator here: raw odd values look like tagged
   * integers to the low-bit predicate. Preserve real heap/string handles,
   * and box every remaining scalar result for the caller's any ABI.
   */
  if (is_ptr(value) || is_heap_ptr(value) || tag >= 100 ||
      rt_native_is_str(value) || NY_NATIVE_IS(value))
    return value;
  return rt_tag_v(value);
}

int64_t rt_call_any1(int64_t f, int64_t value) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  bool dynamic_callable = NY_DYNAMIC_CALLABLE_IS(f);
  bool dynamic_bool_callable = NY_DYNAMIC_CALLABLE_BOOL_IS(f);
  bool raw_result_callable = NY_DYNAMIC_CALLABLE_RAW_RESULT_IS(f);
  /*
   * The bool mark drives the raw 0/1 result ABI; only the plain bool mark
   * also unboxes integer arguments.  A tagged-args body consumes its
   * parameters through rt_any_* helpers and needs the canonical tagged
   * word (an unboxed 1 would re-classify as integer zero).
   */
  bool raw_scalar_callback_args = !NY_DYNAMIC_CALLABLE_TAGGED_ARGS_IS(f);
  if (dynamic_callable)
    f = (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(f);
  /*
   * A closure is a heap object, not a function pointer.  Do this check
   * before rt_fix_fn_ptr(), which is allowed to rewrite raw callable
   * addresses and would otherwise hide the closure header.
   */
  bool is_closure = false;
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    is_closure = *(int64_t *)(base - 8) == TAG_CLOSURE;
  }
  if (!is_closure)
    f = rt_prepare_raw_callable(f);
  int64_t len = rt_any_len(value);
  int64_t tag = rt_any_tag(value);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = rt_prepare_raw_callable(*(int64_t *)base);
      int64_t env = *(int64_t *)(base + 8);
      return rt_dynamic_callback_result(
          ((rt_any_fn7)code)(env, rt_dynamic_callback_arg(value, dynamic_bool_callable),
                                    len, tag, 0, 0, 0),
          dynamic_bool_callable, false,
          raw_result_callable);
    }
  }
  /*
   * A dynamic-callable value decodes to a RAW code address (the lowering
   * only marks plain `sym` pointers).  A raw address whose low three bits
   * equal NY_NATIVE_TAG must not take the native-encoded branch below:
   * decoding it again halved the pointer (map string jumped into
   * address/8) instead of calling the callback.
   */
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(
        ((rt_any_fn6)NY_NATIVE_DECODE(f))(value, len, tag, 0, 0, 0));
  /*
   * Named user functions keep their declared typed ABI.  Sequence adapters
   * enter through the dynamic boundary, so unbox scalar arguments before
   * entering a direct function pointer. Captured closures use the trampoline
   * above and deliberately receive the tagged value.
   */
  if (dynamic_callable)
    return rt_dynamic_callback_result(
        ((rt_any_fn6)(uintptr_t)f)(rt_dynamic_callback_arg(value, raw_scalar_callback_args),
                                          len, tag, 0, 0, 0),
        dynamic_bool_callable, raw_scalar_callback_args,
        raw_result_callable);
  return rt_any_callback_result(
      ((rt_any_fn6)(uintptr_t)f)(rt_any_to_i64(value), len, tag, 0, 0, 0));
}

int64_t rt_call_any2(int64_t f, int64_t left, int64_t right) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  bool dynamic_callable = NY_DYNAMIC_CALLABLE_IS(f);
  bool dynamic_bool_callable = NY_DYNAMIC_CALLABLE_BOOL_IS(f);
  bool raw_scalar_callback_args = !NY_DYNAMIC_CALLABLE_TAGGED_ARGS_IS(f);
  bool raw_result_callable = NY_DYNAMIC_CALLABLE_RAW_RESULT_IS(f);
  if (dynamic_callable)
    f = (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(f);
  bool is_closure = false;
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    is_closure = *(int64_t *)(base - 8) == TAG_CLOSURE;
  }
  if (!is_closure)
    f = rt_prepare_raw_callable(f);
  int64_t left_len = rt_any_len(left);
  int64_t left_tag = rt_any_tag(left);
  int64_t right_len = rt_any_len(right);
  int64_t right_tag = rt_any_tag(right);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = rt_prepare_raw_callable(*(int64_t *)base);
      int64_t env = *(int64_t *)(base + 8);
      return rt_dynamic_callback_result(
          ((rt_any_fn7)code)(env, rt_dynamic_callback_arg(left, false),
                                    left_len, left_tag,
                                    rt_dynamic_callback_arg(right, false),
                                    right_len, right_tag),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(
        ((rt_any_fn6)NY_NATIVE_DECODE(f))(left, left_len, left_tag,
                                                 right, right_len, right_tag));
  if (dynamic_callable) {
    return rt_dynamic_callback_result(
        ((rt_any_fn6)(uintptr_t)f)(
            rt_dynamic_callback_arg(left, raw_scalar_callback_args), left_len, left_tag,
            rt_dynamic_callback_arg(right, raw_scalar_callback_args), right_len, right_tag),
        dynamic_bool_callable, raw_scalar_callback_args,
        raw_result_callable);
  }
  return rt_any_callback_result(
      ((rt_any_fn6)(uintptr_t)f)(rt_any_to_i64(left), left_len,
                                        left_tag, right, right_len, right_tag));
}

int64_t rt_call0(int64_t f) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET0(NY_NATIVE_DECODE(f)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      int64_t result = ((int64_t (*)(int64_t))code)(env);
      /*
       * Zero-argument closures use the dynamic call surface.  Their native
       * bodies may return a raw scalar, so normalize that result exactly as
       * the one/two-argument callback trampolines do.
       */
      if (result != NY_IMM_NIL && !rt_is_bool_imm(result) &&
          !is_v_flt(result) && !is_int(result) && !is_ptr(result) &&
          !is_heap_ptr(result) && !rt_native_is_str(result) &&
          !NY_NATIVE_IS(result))
        return rt_tag_v(result);
      return result;
    }
  }
  return ((int64_t (*)(void))f)();
}

int64_t rt_call0_ptr(int64_t f) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void))NY_NATIVE_DECODE(f))();
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call0_i32(int64_t f) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void))NY_NATIVE_DECODE(f))();
    return rt_tag_v((int64_t)res);
  }
  return rt_call0(f);
}

int64_t rt_call1(int64_t f, int64_t a0) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    int64_t res_raw = NY_NATIVE_RET1(NY_NATIVE_DECODE(f), v0);
    return rt_tag_v(res_raw);
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      int64_t raw = ((int64_t (*)(int64_t, int64_t))code)(env, a0);
      return ny_small_int_fits_i64(raw) ? rt_tag_v(raw) : raw;
    }
  }
  return ((int64_t (*)(int64_t))f)(a0);
}

int64_t rt_call1_ptr(int64_t f, int64_t a0) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *))NY_NATIVE_DECODE(f))((void *)(uintptr_t)rt_untag_v(a0));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call1_i64(int64_t f, int64_t a0) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
#if UINTPTR_MAX == 0xffffffff
    long long arg = (long long)rt_untag_v(a0);
    long long res = ((long long (*)(long long))NY_NATIVE_DECODE(f))(arg);
    return rt_tag_v((int64_t)res);
#else
    int64_t res_raw = ((int64_t (*)(int64_t))NY_NATIVE_DECODE(f))(rt_untag_v(a0));
    return rt_tag_v(res_raw);
#endif
  }
  return rt_call1(f, a0);
}

int64_t rt_call1_u32(int64_t f, int64_t a0) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    uint32_t arg = (uint32_t)rt_untag_v(a0);
    uint32_t res = ((uint32_t (*)(uint32_t))NY_NATIVE_DECODE(f))(arg);
    return rt_tag_v((int64_t)res);
  }
  return rt_call1(f, a0);
}

int64_t rt_call2(int64_t f, int64_t a0, int64_t a1) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    if (is_heap_ptr(v1))
      v1 = (int64_t)(uintptr_t)rt_untag_v(v1);
    return rt_tag_v(NY_NATIVE_RET2(NY_NATIVE_DECODE(f), v0, v1));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      int64_t raw = ((int64_t (*)(int64_t, int64_t, int64_t))code)(
          env, a0, a1);
      return ny_small_int_fits_i64(raw) ? rt_tag_v(raw) : raw;
    }
  }
  return ((int64_t (*)(int64_t, int64_t))f)(a0, a1);
}

int64_t rt_call2_ptr(int64_t f, int64_t a0, int64_t a1) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call2_ptr_u32(int64_t f, int64_t a0, int64_t a1) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint32_t)rt_untag_v(a1));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call3(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);
    int64_t v2 = rt_untag_v(a2);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    if (is_heap_ptr(v1))
      v1 = (int64_t)(uintptr_t)rt_untag_v(v1);
    if (is_heap_ptr(v2))
      v2 = (int64_t)(uintptr_t)rt_untag_v(v2);
    return rt_tag_v(NY_NATIVE_RET3(NY_NATIVE_DECODE(f), v0, v1, v2));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))code)(env, a0, a1, a2);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t))f)(a0, a1, a2);
}

int64_t rt_call3_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call3_ptr_u64_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint64_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint64_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call3_ptr_u32_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint32_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint32_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call3_ptr_ptr_u32(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, uint32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (uint32_t)rt_untag_v(a2));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);
    int64_t v2 = rt_untag_v(a2);
    int64_t v3 = rt_untag_v(a3);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    if (is_heap_ptr(v1))
      v1 = (int64_t)(uintptr_t)rt_untag_v(v1);
    if (is_heap_ptr(v2))
      v2 = (int64_t)(uintptr_t)rt_untag_v(v2);
    if (is_heap_ptr(v3))
      v3 = (int64_t)(uintptr_t)rt_untag_v(v3);
    return rt_tag_v(NY_NATIVE_RET4(NY_NATIVE_DECODE(f), v0, v1, v2, v3));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))code)(env, a0, a1, a2, a3);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3);
}

int64_t rt_call4_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call3_ptr_u64_ptr_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, uint64_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint64_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint64_t, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (uint64_t)rt_untag_v(a1),
      (void *)(uintptr_t)rt_untag_v(a2));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call4_ptr_ptr_ptr_ptr_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, void *, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
      (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call4_ptr_u32_u64_ptr_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, uint32_t, uint64_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint32_t)rt_untag_v(a1), (uint64_t)rt_untag_v(a2),
        (void *)(uintptr_t)rt_untag_v(a3));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint32_t, uint64_t, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (uint32_t)rt_untag_v(a1), (uint64_t)rt_untag_v(a2),
      (void *)(uintptr_t)rt_untag_v(a3));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call4_ptr_u64_ptr_ptr_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, uint64_t, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (uint64_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint64_t, void *, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (uint64_t)rt_untag_v(a1),
      (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call4_ptr_ptr_ptr_u64_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (uint64_t)rt_untag_v(a3));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
      (void *)(uintptr_t)rt_untag_v(a2), (uint64_t)rt_untag_v(a3));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call5_ptr_ptr_ptr_u64_i32_i32(int64_t f, int64_t a0, int64_t a1, int64_t a2,
                                         int64_t a3, int64_t a4) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t, int32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (uint64_t)rt_untag_v(a3),
        (int32_t)rt_untag_v(a4));
    return rt_tag_v((int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t, int32_t))(uintptr_t)f)(
      (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
      (void *)(uintptr_t)rt_untag_v(a2), (uint64_t)rt_untag_v(a3), (int32_t)rt_untag_v(a4));
  return rt_tag_v((int64_t)res);
}

int64_t rt_call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET5(NY_NATIVE_DECODE(f), rt_untag_v(a0), rt_untag_v(a1),
                                   rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))code)(env, a0, a1,
                                                                                       a2, a3, a4);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4);
}

int64_t rt_call5_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, void *, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_untag_v(a0), (void *)(uintptr_t)rt_untag_v(a1),
        (void *)(uintptr_t)rt_untag_v(a2), (void *)(uintptr_t)rt_untag_v(a3),
        (void *)(uintptr_t)rt_untag_v(a4));
    return res ? NY_NATIVE_ENCODE(res) : 0;
  }
  return 0;
}

int64_t rt_call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                 int64_t a5) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET6(NY_NATIVE_DECODE(f), rt_untag_v(a0), rt_untag_v(a1),
                                   rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4), rt_untag_v(a5)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4,
                                                                                a5);
}

int64_t rt_call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6);
}

int64_t rt_call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7);
}

int64_t rt_call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7, int64_t a8) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

int64_t rt_call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7, a8,
                                                            a9);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

int64_t rt_call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9),
        rt_untag_v(a10)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t))code)(env, a0, a1, a2, a3, a4, a5,
                                                                     a6, a7, a8, a9, a10);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

int64_t rt_call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9),
        rt_untag_v(a10), rt_untag_v(a11)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8,
                                                              a9, a10, a11);
}

int64_t rt_call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
            rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9),
            rt_untag_v(a10), rt_untag_v(a11), rt_untag_v(a12)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6,
                                                                       a7, a8, a9, a10, a11, a12);
}

int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
            rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9),
            rt_untag_v(a10), rt_untag_v(a11), rt_untag_v(a12), rt_untag_v(a13)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13, int64_t a14) {
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3), rt_untag_v(a4),
        rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7), rt_untag_v(a8), rt_untag_v(a9),
        rt_untag_v(a10), rt_untag_v(a11), rt_untag_v(a12), rt_untag_v(a13), rt_untag_v(a14)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    if (*(int64_t *)(base - 8) == TAG_CLOSURE) {
      int64_t code = *(int64_t *)base;
      code = rt_prepare_raw_callable(code);
      int64_t env = *(int64_t *)(base + 8);
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11,
                                          a12, a13, a14);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

static inline int64_t rt_void_arg(int64_t a) {
  int64_t v = rt_untag_v(a);
  if (is_heap_ptr(v))
    v = (int64_t)(uintptr_t)rt_untag_v(v);
  return v;
}

int64_t rt_call0_void(int64_t f) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(void))NY_NATIVE_DECODE(f))();
    return 1;
  }
  rt_call0(f);
  return 1;
}

int64_t rt_call1_void(int64_t f, int64_t a0) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t))NY_NATIVE_DECODE(f))(rt_void_arg(a0));
    return 1;
  }
  rt_call1(f, a0);
  return 1;
}
int64_t rt_call1_u32_void(int64_t f, int64_t a0) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    uint32_t arg = (uint32_t)rt_untag_v(a0);
    ((void (*)(uint32_t))NY_NATIVE_DECODE(f))(arg);
    return 1;
  }
  return rt_call1(f, a0);
}
int64_t rt_call2_void(int64_t f, int64_t a0, int64_t a1) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t))NY_NATIVE_DECODE(f))(rt_void_arg(a0), rt_void_arg(a1));
    return 1;
  }
  rt_call2(f, a0, a1);
  return 1;
}
int64_t rt_call3_void(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(rt_void_arg(a0), rt_void_arg(a1),
                                                               rt_void_arg(a2));
    return 1;
  }
  rt_call3(f, a0, a1, a2);
  return 1;
}
int64_t rt_call4_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3));
    return 1;
  }
  rt_call4(f, a0, a1, a2, a3);
  return 1;
}

int64_t rt_call4_ptr_ptr_ptr_ptr_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    void *p0 = (void *)(uintptr_t)rt_untag_v(a0);
    void *p1 = (void *)(uintptr_t)rt_untag_v(a1);
    void *p2 = (void *)(uintptr_t)rt_untag_v(a2);
    void *p3 = (void *)(uintptr_t)rt_untag_v(a3);
    ((void (*)(void *, void *, void *, void *))NY_NATIVE_DECODE(f))(p0, p1, p2, p3);
    return 1;
  }
  return 1;
}

int64_t rt_call5_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3), rt_void_arg(a4));
    return 1;
  }
  rt_call5(f, a0, a1, a2, a3, a4);
  return 1;
}
int64_t rt_call6_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3), rt_void_arg(a4),
        rt_void_arg(a5));
    return 1;
  }
  rt_call6(f, a0, a1, a2, a3, a4, a5);
  return 1;
}
int64_t rt_call7_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3), rt_void_arg(a4),
        rt_void_arg(a5), rt_void_arg(a6));
    return 1;
  }
  rt_call7(f, a0, a1, a2, a3, a4, a5, a6);
  return 1;
}
int64_t rt_call8_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6, int64_t a7) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))
         NY_NATIVE_DECODE(f))(rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3),
                              rt_void_arg(a4), rt_void_arg(a5), rt_void_arg(a6), rt_void_arg(a7));
    return 1;
  }
  rt_call8(f, a0, a1, a2, a3, a4, a5, a6, a7);
  return 1;
}
int64_t rt_call9_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  if (!f || (uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))
         NY_NATIVE_DECODE(f))(rt_void_arg(a0), rt_void_arg(a1), rt_void_arg(a2), rt_void_arg(a3),
                              rt_void_arg(a4), rt_void_arg(a5), rt_void_arg(a6), rt_void_arg(a7),
                              rt_void_arg(a8));
    return 1;
  }
  rt_call9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8);
  return 1;
}
int64_t rt_call10_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t a5, int64_t a6, int64_t a7, int64_t a8, int64_t a9) {
  return rt_call10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

static inline void *rt_ffi_fn(int64_t f) {
  if (!f)
    return NULL;
  if ((uintptr_t)f < 0x1000)
    return NULL;
  return NY_NATIVE_IS(f) ? NY_NATIVE_DECODE(f) : (void *)(uintptr_t)f;
}

static inline float rt_ffi_f32_arg(int64_t v) {
  double d = 0.0;
  int64_t bits = rt_flt_unbox_val(v);
  memcpy(&d, &bits, 8);
  return (float)d;
}

int64_t rt_call4f_void(int64_t f, int64_t a, int64_t b, int64_t c, int64_t d) {
  if (!f)
    return 1;
  double da, db, dc, dd;
  int64_t ba = rt_flt_unbox_val(a);
  int64_t bb = rt_flt_unbox_val(b);
  int64_t bc = rt_flt_unbox_val(c);
  int64_t bd = rt_flt_unbox_val(d);
  memcpy(&da, &ba, 8);
  memcpy(&db, &bb, 8);
  memcpy(&dc, &bc, 8);
  memcpy(&dd, &bd, 8);
  ((void (*)(double, double, double, double))f)(da, db, dc, dd);
  return 1;
}

int64_t rt_call1_f32_void(int64_t f, int64_t a) {
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return 1;
  ((void (*)(float))fn)(rt_ffi_f32_arg(a));
  return 1;
}

int64_t rt_call2_f32_void(int64_t f, int64_t a, int64_t b) {
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return 1;
  ((void (*)(float, float))fn)(rt_ffi_f32_arg(a), rt_ffi_f32_arg(b));
  return 1;
}

int64_t rt_call3_f32_void(int64_t f, int64_t a, int64_t b, int64_t c) {
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return 1;
  ((void (*)(float, float, float))fn)(rt_ffi_f32_arg(a), rt_ffi_f32_arg(b), rt_ffi_f32_arg(c));
  return 1;
}

int64_t rt_call4_f32_void(int64_t f, int64_t a, int64_t b, int64_t c, int64_t d) {
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return 1;
  ((void (*)(float, float, float, float))fn)(rt_ffi_f32_arg(a), rt_ffi_f32_arg(b),
                                             rt_ffi_f32_arg(c), rt_ffi_f32_arg(d));
  return 1;
}

#include <zlib.h>

int64_t rt_zlib_uncompress(int64_t dest, int64_t destLen_p, int64_t src, int64_t srcLen) {
  uLongf *dlen = (uLongf *)(uintptr_t)rt_untag_v(destLen_p);
  int res = uncompress((Bytef *)(uintptr_t)rt_untag_v(dest), dlen,
                       (const Bytef *)(uintptr_t)rt_untag_v(src), (uLong)rt_untag_v(srcLen));
  return rt_tag_v((int64_t)res);
}

int64_t rt_zlib_compress(int64_t dest, int64_t destLen_p, int64_t src, int64_t srcLen,
                         int64_t level) {
  uLongf *dlen = (uLongf *)(uintptr_t)rt_untag_v(destLen_p);
  int res = compress2((Bytef *)(uintptr_t)rt_untag_v(dest), dlen,
                      (const Bytef *)(uintptr_t)rt_untag_v(src), (uLong)rt_untag_v(srcLen),
                      (int)rt_untag_v(level));
  return rt_tag_v((int64_t)res);
}

int64_t rt_zlib_compress_str(int64_t src, int64_t srcLen, int64_t level) {
  const Bytef *src_p = (const Bytef *)(uintptr_t)rt_untag_v(src);
  uLong src_len = (uLong)rt_untag_v(srcLen);
  uLongf out_len = compressBound(src_len);
  int64_t out = rt_malloc((int64_t)(((uint64_t)out_len + 1u) << 1) | 1);
  if (!out)
    return 0;
  int res = compress2((Bytef *)(uintptr_t)out, &out_len, src_p, src_len, (int)rt_untag_v(level));
  if (res != Z_OK) {
    rt_free(out);
    return 0;
  }
  *(int64_t *)((char *)(uintptr_t)out - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)out - 16) = ((int64_t)out_len << 1) | 1;
  ((char *)(uintptr_t)out)[out_len] = '\0';
  return out;
}

int64_t rt_zlib_bound(int64_t n) { return rt_tag_v((int64_t)compressBound((uLong)rt_untag_v(n))); }
