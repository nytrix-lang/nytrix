#include "rt/shared.h"
#include <string.h>
#ifdef _WIN32
/* rt/shared.h already provides windows.h with WIN32_LEAN_AND_MEAN. */
#else
#include <dlfcn.h>
#endif

#if UINTPTR_MAX == 0xffffffff
#define NY_NATIVE_TAG 2
#define NY_NATIVE_MARK (1ULL << 63)
#define NY_NATIVE_IS(v)                                                        \
  ((((uint64_t)(v) & NY_NATIVE_MARK) != 0ULL) && (((v) & 3) == NY_NATIVE_TAG))
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(NY_NATIVE_MARK |                                                  \
             (((uint64_t)(uintptr_t)(p) << 2) | (uint64_t)NY_NATIVE_TAG)))
#define NY_NATIVE_DECODE(v)                                                    \
  ((void *)(uintptr_t)((((uint64_t)(v)) & ~NY_NATIVE_MARK) >> 2))
#define NY_NATIVE_RET0(fn) (int64_t)((intptr_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0)                                                 \
  (int64_t)((intptr_t (*)(intptr_t))(fn))((intptr_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                             \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t))(fn))((intptr_t)(a0),            \
                                                    (intptr_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                                         \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t))(fn))(                 \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                                     \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t))(fn))(       \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                                 \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t))(  \
      fn))((intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3),     \
           (intptr_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                             \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t,    \
                          intptr_t))(fn))((intptr_t)(a0), (intptr_t)(a1),      \
                                          (intptr_t)(a2), (intptr_t)(a3),      \
                                          (intptr_t)(a4), (intptr_t)(a5))
#else
#define NY_NATIVE_TAG 6
#define NY_NATIVE_IS(v) (((v) & 7) == NY_NATIVE_TAG)
#define NY_NATIVE_ENCODE(p)                                                    \
  ((int64_t)(((uint64_t)(uintptr_t)(p) << 3) | (uint64_t)NY_NATIVE_TAG))
#define NY_NATIVE_DECODE(v) ((void *)(uintptr_t)(((uint64_t)(v)) >> 3))
#define NY_NATIVE_RET0(fn) ((int64_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0) ((int64_t (*)(int64_t))(fn))((int64_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                             \
  ((int64_t (*)(int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                                         \
  ((int64_t (*)(int64_t, int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1), \
                                                 (int64_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                                     \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(fn))(                     \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                                 \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(fn))(            \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3),              \
      (int64_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                             \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(fn))(   \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3),              \
      (int64_t)(a4), (int64_t)(a5))
#endif

#ifdef _WIN32
static int64_t __make_str_ffi(const char *s) {
  if (!s)
    return 0;
  size_t len = strlen(s);
  int64_t res = __malloc(((int64_t)len + 1) << 1 | 1);
  if (!res)
    return 0;
  *(int64_t *)(uintptr_t)((char *)res - 8) = TAG_STR;
  *(int64_t *)(uintptr_t)((char *)res - 16) = ((int64_t)len << 1) | 1;
  strcpy((char *)(uintptr_t)res, s);
  return res;
}
#endif

int64_t __tag_native(int64_t addr) {
  if (is_int(addr))
    addr >>= 1;
  if (!addr)
    return 0;
  return NY_NATIVE_ENCODE((void *)(uintptr_t)addr);
}

int64_t __dlopen(int64_t name, int64_t flags) {
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

int64_t __dlsym(int64_t handle, int64_t name) {
  void *p = NULL;
  void *h = NY_NATIVE_IS(handle) ? NY_NATIVE_DECODE(handle)
                                 : (void *)(uintptr_t)handle;
#ifdef _WIN32
  p = (void *)GetProcAddress((HMODULE)h, (const char *)name);
#else
  p = dlsym(h, (const char *)name);
#endif
  if (!p)
    return 0;
  return NY_NATIVE_ENCODE(p);
}

int64_t __dlerror(void) {
#ifdef _WIN32
  DWORD err = GetLastError();
  if (err == 0)
    return 0;
  LPSTR msg = NULL;
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS;
  if (!FormatMessageA(flags, NULL, err, 0, (LPSTR)&msg, 0, NULL) || !msg)
    return 0;
  int64_t s = __make_str_ffi(msg);
  LocalFree(msg);
  return s;
#else
  return (int64_t)dlerror();
#endif
}

int64_t __dlclose(int64_t handle) {
  void *h = NY_NATIVE_IS(handle) ? NY_NATIVE_DECODE(handle)
                                 : (void *)(uintptr_t)handle;
#ifdef _WIN32
  if (!h)
    return -1;
  return FreeLibrary((HMODULE)h) ? 0 : -1;
#else
  return dlclose(h);
#endif
}
int64_t __ffi_untag_ptr(int64_t v) { return rt_untag_v(v); }

#define UNTAG(x) rt_untag_v(x)

int64_t __call0(int64_t f) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET0(NY_NATIVE_DECODE(f)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t))__mask_ptr(code))(env);
  }
  return ((int64_t (*)(void))__mask_ptr(f))();
}

int64_t __call1(int64_t f, int64_t a0) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t res_raw = NY_NATIVE_RET1(NY_NATIVE_DECODE(f), rt_untag_v(a0));
    // printf("DEBUG: __call1 native fn=%lx arg=%ld res_raw=%ld tagged=%ld\n",
    // f, rt_untag_v(a0), res_raw, rt_tag_v(res_raw));
    return rt_tag_v(res_raw);
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t))__mask_ptr(code))(env, a0);
  }
  return ((int64_t (*)(int64_t))__mask_ptr(f))(a0);
}

int64_t __call1_i64(int64_t f, int64_t a0) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
#if UINTPTR_MAX == 0xffffffff
    long long arg = (long long)rt_untag_v(a0);
    long long res = ((long long (*)(long long))NY_NATIVE_DECODE(f))(arg);
    return rt_tag_v((int64_t)res);
#else
    int64_t res_raw =
        ((int64_t (*)(int64_t))NY_NATIVE_DECODE(f))(rt_untag_v(a0));
    return rt_tag_v(res_raw);
#endif
  }
  return __call1(f, a0);
}

int64_t __call2(int64_t f, int64_t a0, int64_t a1) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        NY_NATIVE_RET2(NY_NATIVE_DECODE(f), rt_untag_v(a0), rt_untag_v(a1)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t))__mask_ptr(code))(env, a0,
                                                                      a1);
  }
  return ((int64_t (*)(int64_t, int64_t))__mask_ptr(f))(a0, a1);
}

int64_t __call3(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET3(NY_NATIVE_DECODE(f), rt_untag_v(a0),
                                   rt_untag_v(a1), rt_untag_v(a2)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t))__mask_ptr(f))(a0, a1, a2);
}

int64_t __call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET4(NY_NATIVE_DECODE(f), rt_untag_v(a0),
                                   rt_untag_v(a1), rt_untag_v(a2),
                                   rt_untag_v(a3)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))__mask_ptr(f))(
      a0, a1, a2, a3);
}

int64_t __call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET5(NY_NATIVE_DECODE(f), rt_untag_v(a0),
                                   rt_untag_v(a1), rt_untag_v(a2),
                                   rt_untag_v(a3), rt_untag_v(a4)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3, a4);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))__mask_ptr(
      f))(a0, a1, a2, a3, a4);
}

int64_t __call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET6(
        NY_NATIVE_DECODE(f), rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2),
        rt_untag_v(a3), rt_untag_v(a4), rt_untag_v(a5)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3, a4,
                                                    a5);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5);
}

int64_t __call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
        rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t))__mask_ptr(code))(env, a0, a1, a2,
                                                             a3, a4, a5, a6);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5, a6);
}

int64_t __call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6, int64_t a7) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
            rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2, a3, a4, a5, a6, a7);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5,
                                                        a6, a7);
}

int64_t __call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
            rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
            rt_untag_v(a8)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2, a3, a4, a5, a6, a7, a8);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t))__mask_ptr(f))(
      a0, a1, a2, a3, a4, a5, a6, a7, a8);
}

int64_t __call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
            rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
            rt_untag_v(a8), rt_untag_v(a9)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3, a4, a5,
                                                    a6, a7, a8, a9);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t))__mask_ptr(f))(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

int64_t __call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
        rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
        rt_untag_v(a8), rt_untag_v(a9), rt_untag_v(a10)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3, a4, a5,
                                                    a6, a7, a8, a9, a10);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t,
                       int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5, a6, a7,
                                               a8, a9, a10);
}

int64_t __call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
        rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
        rt_untag_v(a8), rt_untag_v(a9), rt_untag_v(a10), rt_untag_v(a11)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a0, a1, a2, a3, a4, a5,
                                                    a6, a7, a8, a9, a10, a11);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5, a6, a7,
                                               a8, a9, a10, a11);
}

int64_t __call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
        rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
        rt_untag_v(a8), rt_untag_v(a9), rt_untag_v(a10), rt_untag_v(a11),
        rt_untag_v(a12)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))__mask_ptr(f))(a0, a1, a2, a3, a4, a5, a6, a7,
                                               a8, a9, a10, a11, a12);
}

int64_t __call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                 int64_t a13) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
            rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
            rt_untag_v(a8), rt_untag_v(a9), rt_untag_v(a10), rt_untag_v(a11),
            rt_untag_v(a12), rt_untag_v(a13)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t))__mask_ptr(f))(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

int64_t __call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                 int64_t a9, int64_t a10, int64_t a11, int64_t a12, int64_t a13,
                 int64_t a14) {
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
            rt_untag_v(a0), rt_untag_v(a1), rt_untag_v(a2), rt_untag_v(a3),
            rt_untag_v(a4), rt_untag_v(a5), rt_untag_v(a6), rt_untag_v(a7),
            rt_untag_v(a8), rt_untag_v(a9), rt_untag_v(a10), rt_untag_v(a11),
            rt_untag_v(a12), rt_untag_v(a13), rt_untag_v(a14)));
  }
  if (is_heap_ptr(f) && *(int64_t *)((uintptr_t)f - 8) == 105) {
    int64_t code = *(int64_t *)f;
    int64_t env = *(int64_t *)(f + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t))__mask_ptr(f))(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

void __call0_void(int64_t f) { __call0(f); }
void __call1_void(int64_t f, int64_t a0) { __call1(f, a0); }
void __call2_void(int64_t f, int64_t a0, int64_t a1) { __call2(f, a0, a1); }
void __call3_void(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  __call3(f, a0, a1, a2);
}
void __call4_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  __call4(f, a0, a1, a2, a3);
}
void __call5_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4) {
  __call5(f, a0, a1, a2, a3, a4);
}
void __call6_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5) {
  __call6(f, a0, a1, a2, a3, a4, a5);
}
void __call7_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6) {
  __call7(f, a0, a1, a2, a3, a4, a5, a6);
}
void __call8_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7) {
  __call8(f, a0, a1, a2, a3, a4, a5, a6, a7);
}
void __call9_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  __call9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8);
}
void __call10_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                   int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                   int64_t a9) {
  __call10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

void __call4f_void(int64_t f, int64_t a, int64_t b, int64_t c, int64_t d) {
  if (!f)
    return;
  double da = __flt_unbox_val(a);
  double db = __flt_unbox_val(b);
  double dc = __flt_unbox_val(c);
  double dd = __flt_unbox_val(d);
  ((void (*)(double, double, double, double))__mask_ptr(f))(da, db, dc, dd);
}
