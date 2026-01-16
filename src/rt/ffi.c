#include "rt/shared.h"
#include <dlfcn.h>

int64_t rt_dlopen(int64_t path, int64_t flags) {
  return (int64_t)dlopen((const char *)path,
                         is_int(flags) ? (int)(flags >> 1) : (int)flags);
}

int64_t rt_dlsym(int64_t handle, int64_t name) {
  void *p = dlsym((void *)handle, (const char *)name);
  if (!p)
    return 0;
  return (int64_t)(uintptr_t)p | 6; // Tag 6 for Native
}

int64_t rt_dlclose(int64_t handle) { return dlclose((void *)handle); }

int64_t rt_dlerror(void) { return (int64_t)dlerror(); }

int64_t rt_call0(int64_t fn) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(void))rt_mask_ptr(fn))());
  if ((fn & 7) == 2)
    return ((int64_t (*)(void))rt_mask_ptr(fn))();
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t))rt_mask_ptr(code))(env);
  }
  return ((int64_t (*)(void))fn)();
}

int64_t rt_call1(int64_t fn, int64_t a) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(int64_t))rt_mask_ptr(fn))(rt_untag(a)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t))rt_mask_ptr(fn))(a);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t))rt_mask_ptr(code))(env, a);
  }
  return ((int64_t (*)(int64_t))fn)(a);
}

int64_t rt_call2(int64_t fn, int64_t a, int64_t b) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(int64_t, int64_t))rt_mask_ptr(fn))(
        rt_untag(a), rt_untag(b)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t))rt_mask_ptr(fn))(a, b);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t))rt_mask_ptr(code))(env, a,
                                                                       b);
  }
  return ((int64_t (*)(int64_t, int64_t))fn)(a, b);
}

int64_t rt_call3(int64_t fn, int64_t a, int64_t b, int64_t c) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(int64_t, int64_t, int64_t))rt_mask_ptr(fn))(
        rt_untag(a), rt_untag(b), rt_untag(c)));
  if ((fn & 7) == 2) {
    typedef int64_t (*f3)(int64_t, int64_t, int64_t);
    f3 target = (f3)rt_mask_ptr(fn);
    return target(a, b, c);
  }
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(code))(
        env, a, b, c);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t))fn)(a, b, c);
}

int64_t rt_call4(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(
        fn))(rt_untag(a), rt_untag(b), rt_untag(c), rt_untag(d)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))rt_mask_ptr(fn))(
        a, b, c, d);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                         int64_t))rt_mask_ptr(code))(env, a, b, c, d);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fn)(a, b, c, d);
}

int64_t rt_call5(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return rt_tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                                int64_t))rt_mask_ptr(fn))(
        rt_untag(a), rt_untag(b), rt_untag(c), rt_untag(d), rt_untag(e)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                         int64_t))rt_mask_ptr(fn))(a, b, c, d, e);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))rt_mask_ptr(code))(env, a, b, c, d, e);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))fn)(a, b, c,
                                                                        d, e);
}

int64_t rt_call6(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g) {
  if (!fn)
    return 1;
  if ((fn & 7) == 2 || (fn & 7) == 6) {
    int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
        (void *)rt_mask_ptr(fn);
    return f(a, b, c, d, e, g);
  }
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)fn;
  return f(a, b, c, d, e, g);
}

int64_t rt_call7(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h);
}

int64_t rt_call8(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t) = (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i);
}

int64_t rt_call9(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t) = (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j);
}

int64_t rt_call10(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                  int64_t k) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k);
}

int64_t rt_call11(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                  int64_t k, int64_t l) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t) = (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l);
}

int64_t rt_call12(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                  int64_t k, int64_t l, int64_t m) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m);
}

int64_t rt_call13(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                  int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                  int64_t k, int64_t l, int64_t m, int64_t n) {
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)rt_mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m, n);
}
