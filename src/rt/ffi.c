#include "rt/shared.h"
#include <dlfcn.h>

int64_t __dlopen(int64_t path, int64_t flags) {
  return (int64_t)dlopen((const char *)path,
                         is_int(flags) ? (int)(flags >> 1) : (int)flags);
}

int64_t __dlsym(int64_t handle, int64_t name) {
  void *p = dlsym((void *)handle, (const char *)name);
  if (!p)
    return 0;
  return (int64_t)(uintptr_t)p | 6; // Tag 6 for Native
}

int64_t __dlclose(int64_t handle) { return dlclose((void *)handle); }

int64_t __dlerror(void) { return (int64_t)dlerror(); }

int64_t __call0(int64_t fn) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(void))__mask_ptr(fn))());
  if ((fn & 7) == 2)
    return ((int64_t (*)(void))__mask_ptr(fn))();
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t))__mask_ptr(code))(env);
  }
  return ((int64_t (*)(void))fn)();
}

int64_t __call1(int64_t fn, int64_t a) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t))__mask_ptr(fn))(__untag(a)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t))__mask_ptr(fn))(a);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t))__mask_ptr(code))(env, a);
  }
  return ((int64_t (*)(int64_t))fn)(a);
}

int64_t __call2(int64_t fn, int64_t a, int64_t b) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t))__mask_ptr(fn))(__untag(a),
                                                                 __untag(b)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t))__mask_ptr(fn))(a, b);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t))__mask_ptr(code))(env, a,
                                                                      b);
  }
  return ((int64_t (*)(int64_t, int64_t))fn)(a, b);
}

int64_t __call3(int64_t fn, int64_t a, int64_t b, int64_t c) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c)));
  if ((fn & 7) == 2) {
    typedef int64_t (*f3)(int64_t, int64_t, int64_t);
    f3 target = (f3)__mask_ptr(fn);
    return target(a, b, c);
  }
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))__mask_ptr(code))(
        env, a, b, c);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t))fn)(a, b, c);
}

int64_t __call4(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t))__mask_ptr(
        fn))(__untag(a), __untag(b), __untag(c), __untag(d)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))__mask_ptr(fn))(
        a, b, c, d);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a, b, c, d);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fn)(a, b, c, d);
}

int64_t __call5(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))__mask_ptr(
            fn))(__untag(a), __untag(b), __untag(c), __untag(d), __untag(e)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(fn))(a, b, c, d, e);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a, b, c, d, e);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))fn)(a, b, c,
                                                                        d, e);
}

int64_t __call6(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, int64_t g) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e),
        __untag(g)));
  if ((fn & 7) == 2)
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(fn))(a, b, c, d, e, g);
  if (is_ptr(fn) && *(int64_t *)((uintptr_t)fn - 8) == 105) {
    int64_t code = *(int64_t *)((uintptr_t)fn);
    int64_t env = *(int64_t *)((uintptr_t)fn + 8);
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))__mask_ptr(code))(env, a, b, c, d, e, g);
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))fn)(a, b, c, d, e, g);
}

int64_t __call7(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, int64_t g, int64_t h) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h);
}

int64_t __call8(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, int64_t g, int64_t h, int64_t i) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h), __untag(i)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t) = (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i);
}

int64_t __call9(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                int64_t e, int64_t g, int64_t h, int64_t i, int64_t j) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t))__mask_ptr(fn))(
            __untag(a), __untag(b), __untag(c), __untag(d), __untag(e),
            __untag(g), __untag(h), __untag(i), __untag(j)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t) = (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j);
}

int64_t __call10(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t))__mask_ptr(fn))(
            __untag(a), __untag(b), __untag(c), __untag(d), __untag(e),
            __untag(g), __untag(h), __untag(i), __untag(j), __untag(k)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t) = (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k);
}

int64_t __call11(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k, int64_t l) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h), __untag(i), __untag(j), __untag(k), __untag(l)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t) = (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l);
}

int64_t __call12(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k, int64_t l, int64_t m) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h), __untag(i), __untag(j), __untag(k), __untag(l),
        __untag(m)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m);
}

int64_t __call13(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k, int64_t l, int64_t m, int64_t n) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h), __untag(i), __untag(j), __untag(k), __untag(l), __untag(m),
        __untag(n)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m, n);
}

int64_t __call14(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k, int64_t l, int64_t m, int64_t n, int64_t o) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(((int64_t (*)(
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))__mask_ptr(fn))(
        __untag(a), __untag(b), __untag(c), __untag(d), __untag(e), __untag(g),
        __untag(h), __untag(i), __untag(j), __untag(k), __untag(l), __untag(m),
        __untag(n), __untag(o)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t) =
      (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m, n, o);
}

int64_t __call15(int64_t fn, int64_t a, int64_t b, int64_t c, int64_t d,
                 int64_t e, int64_t g, int64_t h, int64_t i, int64_t j,
                 int64_t k, int64_t l, int64_t m, int64_t n, int64_t o,
                 int64_t p) {
  if (!fn)
    return 1;
  if ((fn & 7) == 6)
    return __tag(
        ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                      int64_t, int64_t, int64_t))__mask_ptr(fn))(
            __untag(a), __untag(b), __untag(c), __untag(d), __untag(e),
            __untag(g), __untag(h), __untag(i), __untag(j), __untag(k),
            __untag(l), __untag(m), __untag(n), __untag(o), __untag(p)));
  int64_t (*f)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t) = (void *)__mask_ptr(fn);
  return f(a, b, c, d, e, g, h, i, j, k, l, m, n, o, p);
}
