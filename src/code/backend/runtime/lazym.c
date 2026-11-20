/*
 * libm entry points used by bignum <-> double conversion.
 *
 * A direct call to ldexp/frexp is a strong undefined reference, and --as-needed
 * keeps a library that answers one, so linking bigint.c or bigfloat.c put
 * libm.so.6 in DT_NEEDED for every AOT binary, including integer-only programs
 * that never convert a bignum to a double. Bind them on first use instead: the
 * dependency follows the first conversion, and the fallbacks keep the
 * conversion working (exactly, for normal-range values) on a host without
 * libm.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
static double rt_lm_ldexp(double x, int e) { return ldexp(x, e); }
static double rt_lm_frexp(double x, int *e) { return frexp(x, e); }
#else
#include <dlfcn.h>

static double (*g_rt_lm_ldexp)(double, int);
static double (*g_rt_lm_frexp)(double, int *);
static int g_rt_lm_bound;

static void rt_lm_bind(void) {
  if (g_rt_lm_bound)
    return;
  g_rt_lm_bound = 1;
  void *h = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
#ifdef __APPLE__
  if (!h)
    h = dlopen("libm.dylib", RTLD_NOW | RTLD_LOCAL);
#endif
  if (!h)
    return;
  *(void **)&g_rt_lm_ldexp = dlsym(h, "ldexp");
  *(void **)&g_rt_lm_frexp = dlsym(h, "frexp");
}

static double rt_lm_ldexp(double x, int e) {
  rt_lm_bind();
  if (g_rt_lm_ldexp)
    return g_rt_lm_ldexp(x, e);
  /*
   * Every step is a power-of-two scale, so normal results are exact.
   */
  while (e > 0) {
    x *= 2.0;
    --e;
  }
  while (e < 0) {
    x *= 0.5;
    ++e;
  }
  return x;
}

static double rt_lm_frexp(double x, int *e) {
  rt_lm_bind();
  if (g_rt_lm_frexp)
    return g_rt_lm_frexp(x, e);
  double ax = x < 0 ? -x : x;
  if (!(ax > 0.0) || ax != ax || ax > 1e308) {
    if (e)
      *e = 0;
    return x;
  }
  int n = 0;
  while (ax >= 1.0) {
    x *= 0.5;
    ax *= 0.5;
    ++n;
  }
  while (ax < 0.5) {
    x *= 2.0;
    ax *= 2.0;
    --n;
  }
  if (e)
    *e = n;
  return x;
}
#endif

double rt_big_ldexp(double x, int e) { return rt_lm_ldexp(x, e); }

double rt_big_frexp(double x, int *e) { return rt_lm_frexp(x, e); }

/*
 * The remaining libm entry points. Every one of these is a strong undefined
 * reference from the amalgamated runtime object, so the link keeps libm.so.6
 * in DT_NEEDED even when --gc-sections deletes the only call site. Resolving
 * on first use lets an integer-only program start without loading libm, while
 * a float program binds it the moment it needs it. A missing symbol is fatal
 * rather than silently wrong: these have no meaningful fallback.
 */
#if defined(_WIN32)
#define RT_LM_RESOLVE(sym, type) ((type) & sym)
#else
static void *g_rt_lm_handle;

static void *rt_lm_symbol(const char *name) {
  if (!g_rt_lm_handle) {
    g_rt_lm_handle = dlopen("libm.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!g_rt_lm_handle)
      g_rt_lm_handle = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
#ifdef __APPLE__
    if (!g_rt_lm_handle)
      g_rt_lm_handle = dlopen("libm.dylib", RTLD_NOW | RTLD_LOCAL);
#endif
  }
  void *sym = g_rt_lm_handle ? dlsym(g_rt_lm_handle, name) : NULL;
  if (!sym) {
    fprintf(stderr, "nytrix runtime: libm has no '%s'\n", name);
    abort();
  }
  return sym;
}
#define RT_LM_RESOLVE(sym, type) ((type)rt_lm_symbol(#sym))
#endif

double rt_libm_sqrt(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(sqrt, void *);
  return fn(x);
}

double rt_libm_pow(double a, double b) {
  static double (*fn)(double, double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(pow, void *);
  return fn(a, b);
}

double rt_libm_exp(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(exp, void *);
  return fn(x);
}

double rt_libm_log(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(log, void *);
  return fn(x);
}

double rt_libm_log2(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(log2, void *);
  return fn(x);
}

double rt_libm_log10(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(log10, void *);
  return fn(x);
}

double rt_libm_floor(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(floor, void *);
  return fn(x);
}

double rt_libm_ceil(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(ceil, void *);
  return fn(x);
}

double rt_libm_fmod(double a, double b) {
  static double (*fn)(double, double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(fmod, void *);
  return fn(a, b);
}

double rt_libm_atan2(double a, double b) {
  static double (*fn)(double, double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(atan2, void *);
  return fn(a, b);
}

double rt_libm_sin(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(sin, void *);
  return fn(x);
}

double rt_libm_cos(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(cos, void *);
  return fn(x);
}

double rt_libm_tan(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(tan, void *);
  return fn(x);
}

long long rt_libm_llround(double x) {
  static long long (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(llround, void *);
  return fn(x);
}

double rt_libm_asin(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(asin, void *);
  return fn(x);
}

double rt_libm_acos(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(acos, void *);
  return fn(x);
}

double rt_libm_atan(double x) {
  static double (*fn)(double);
  if (!fn)
    *(void **)&fn = RT_LM_RESOLVE(atan, void *);
  return fn(x);
}
