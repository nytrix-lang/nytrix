#include "rt/shared.h"
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

int64_t rt_call0(int64_t f) {
  if (!f)
    return 1;
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
      return ((int64_t (*)(int64_t))code)(env);
    }
  }
  return ((int64_t (*)(void))f)();
}

/* Pointer-returning native calls.
 *
 * The generic rt_callN paths tag return values as Ny integers (rt_tag_v),
 * which is correct for numeric returns but destroys raw pointers (Z3, etc).
 * These helpers return NY-native-tagged handles so Ny code can safely pass
 * them back into further native calls (arguments are untagged via rt_untag_v).
 */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    /* `rt_untag_v()` already decodes native handles. Re-checking the untagged
     * value with NY_NATIVE_IS would misclassify ordinary integers such as 110
     * ('n') because their low bits also end in 0b110. */
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
      return ((int64_t (*)(int64_t, int64_t))code)(env, a0);
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);
    /* Do not run NY_NATIVE_IS on already-untagged integers here. Native
     * handles are decoded by rt_untag_v(), while plain ints must stay plain
     * ints for foreign APIs such as FreeType glyph lookup. */
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
      return ((int64_t (*)(int64_t, int64_t, int64_t))code)(env, a0, a1);
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);
    int64_t v2 = rt_untag_v(a2);
    /* If any argument is a heap pointer, treat it as a raw pointer when
     * calling foreign functions. Native handles have already been untagged by
     * rt_untag_v(); only actual heap values need extra unwrapping here. */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_untag_v(a0);
    int64_t v1 = rt_untag_v(a1);
    int64_t v2 = rt_untag_v(a2);
    int64_t v3 = rt_untag_v(a3);
    /* If any argument is a heap pointer, treat them as raw pointers (do not
     * pass Ny header-tagged values as integers to foreign functions). */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* For raw function pointers, untag arguments to get raw values */
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6,
                                                                       a7, a8, a9, a10, a11, a12);
}

int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13) {
  if (!f)
    return 1;
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* For raw function pointers, untag arguments to get raw values */
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13, int64_t a14) {
  if (!f)
    return 1;
  /* Guard: reject tagged ints / tiny values that are not valid fn ptrs */
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
  /* For raw function pointers, untag arguments to get raw values */
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

int64_t rt_call0_void(int64_t f) { return rt_call0(f); }
int64_t rt_call1_void(int64_t f, int64_t a0) { return rt_call1(f, a0); }
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
int64_t rt_call2_void(int64_t f, int64_t a0, int64_t a1) { return rt_call2(f, a0, a1); }
int64_t rt_call3_void(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  return rt_call3(f, a0, a1, a2);
}
int64_t rt_call4_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  return rt_call4(f, a0, a1, a2, a3);
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
  return rt_call5(f, a0, a1, a2, a3, a4);
}
int64_t rt_call6_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5) {
  return rt_call6(f, a0, a1, a2, a3, a4, a5);
}
int64_t rt_call7_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6) {
  return rt_call7(f, a0, a1, a2, a3, a4, a5, a6);
}
int64_t rt_call8_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6, int64_t a7) {
  return rt_call8(f, a0, a1, a2, a3, a4, a5, a6, a7);
}
int64_t rt_call9_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                      int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  return rt_call9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8);
}
int64_t rt_call10_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4,
                       int64_t a5, int64_t a6, int64_t a7, int64_t a8, int64_t a9) {
  return rt_call10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
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

int64_t rt_call4_f32_void(int64_t f, int64_t a, int64_t b, int64_t c, int64_t d) {
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
  float fa = (float)da;
  float fb = (float)db;
  float fc = (float)dc;
  float fd = (float)dd;
  if (NY_NATIVE_IS(f)) {
    ((void (*)(float, float, float, float))NY_NATIVE_DECODE(f))(fa, fb, fc, fd);
  } else {
    ((void (*)(float, float, float, float))f)(fa, fb, fc, fd);
  }
  return 1;
}

// Zlib conventions

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
