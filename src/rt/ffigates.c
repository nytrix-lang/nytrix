
#define memset_manual(p, v, n)                                                                     \
  do {                                                                                             \
    unsigned char *_p = (unsigned char *)(p);                                                      \
    unsigned char _v = (unsigned char)(v);                                                         \
    size_t _n = (n);                                                                               \
    while (_n-- > 0)                                                                               \
      *_p++ = _v;                                                                                  \
  } while (0)

#include "rt/ffigates.h"
#include "rt/shared.h"
#include "base/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t rt_call0(int64_t f);
int64_t rt_call0_ptr(int64_t f);
int64_t rt_call1(int64_t f, int64_t a0);
int64_t rt_call1_ptr(int64_t f, int64_t a0);
int64_t rt_call2(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call2_ptr(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call2_ptr_u32(int64_t f, int64_t a0, int64_t a1);
int64_t rt_call3(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_u64_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_u32_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call3_ptr_ptr_u32(int64_t f, int64_t a0, int64_t a1, int64_t a2);
int64_t rt_call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call4_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call4_ptr_ptr_ptr_ptr_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t rt_call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4);
int64_t rt_call5_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4);
int64_t rt_call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5);
int64_t rt_call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6);
int64_t rt_call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7);
int64_t rt_call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                 int64_t a6, int64_t a7, int64_t a8);
int64_t rt_call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9);
int64_t rt_call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10);
int64_t rt_call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11);
int64_t rt_call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12);
int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13);
int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5,
                  int64_t a6, int64_t a7, int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                  int64_t a12, int64_t a13, int64_t a14);

nyFfiState_t gNyFfi = {0};

static int64_t stub_i_v(void *fn) { return ((int64_t (*)(void))fn)(); }

static int64_t stub_i_i(void *fn, int64_t a0) { return nyFfiFastII((int64_t)(uintptr_t)fn, a0); }

static int64_t stub_i_ii(void *fn, int64_t a0, int64_t a1) {
  return nyFfiFastIIi((int64_t)(uintptr_t)fn, a0, a1);
}

static int64_t stub_i_iii(void *fn, int64_t a0, int64_t a1, int64_t a2) {
  return nyFfiFastIIii((int64_t)(uintptr_t)fn, a0, a1, a2);
}

static int64_t stub_i_iiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fn)(a0, a1, a2, a3);
}

static int64_t stub_i_iiiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))fn)(a0, a1, a2, a3, a4);
}

static void stub_v_i(void *fn, int64_t a0) { ((void (*)(int64_t))fn)(a0); }

static void stub_v_ii(void *fn, int64_t a0, int64_t a1) {
  ((void (*)(int64_t, int64_t))fn)(a0, a1);
}

static void stub_v_iii(void *fn, int64_t a0, int64_t a1, int64_t a2) {
  ((void (*)(int64_t, int64_t, int64_t))fn)(a0, a1, a2);
}

static int64_t stub_p_p(void *fn, int64_t a0) {
  return (int64_t)((void *(*)(void *))fn)((void *)a0);
}

static int64_t stub_p_pp(void *fn, int64_t a0, int64_t a1) {
  return (int64_t)((void *(*)(void *, void *))fn)((void *)a0, (void *)a1);
}

static int64_t stub_p_pi(void *fn, int64_t a0, int64_t a1) {
  return (int64_t)((void *(*)(void *, int64_t))fn)((void *)a0, a1);
}

void nyFfiGates_init(void) {
  if (gNyFfi.initialized)
    return;
  memset_manual(&gNyFfi, 0, sizeof(gNyFfi));
  gNyFfi.cache.capacity = 256;
  gNyFfi.cache.entries =
      (nyFfiCacheEntry_t *)calloc(gNyFfi.cache.capacity, sizeof(nyFfiCacheEntry_t));
  gNyFfi.cache.count = 0;
  gNyFfi.gate_capacity = 64;
  gNyFfi.gates = (nyFfiGate_t *)malloc(gNyFfi.gate_capacity * sizeof(nyFfiGate_t));
  if (!gNyFfi.gates)
    gNyFfi.gate_capacity = 0;
  gNyFfi.gate_count = 0;
  nyFfiRegisterGate(NY_FFI_SIG_I_V, "i_v", (void *)stub_i_v);
  nyFfiRegisterGate(NY_FFI_SIG_I_I, "i_i", (void *)stub_i_i);
  nyFfiRegisterGate(NY_FFI_SIG_I_II, "i_ii", (void *)stub_i_ii);
  nyFfiRegisterGate(NY_FFI_SIG_I_III, "i_iii", (void *)stub_i_iii);
  nyFfiRegisterGate(NY_FFI_SIG_I_IIII, "i_iiii", (void *)stub_i_iiii);
  nyFfiRegisterGate(NY_FFI_SIG_I_IIIII, "i_iiiii", (void *)stub_i_iiiii);
  nyFfiRegisterGate(NY_FFI_SIG_V_I, "v_i", (void *)stub_v_i);
  nyFfiRegisterGate(NY_FFI_SIG_V_II, "v_ii", (void *)stub_v_ii);
  nyFfiRegisterGate(NY_FFI_SIG_V_III, "v_iii", (void *)stub_v_iii);
  nyFfiRegisterGate(NY_FFI_SIG_P_P, "p_p", (void *)stub_p_p);
  nyFfiRegisterGate(NY_FFI_SIG_P_PP, "p_pp", (void *)stub_p_pp);
  nyFfiRegisterGate(NY_FFI_SIG_P_PI, "p_pi", (void *)stub_p_pi);
  gNyFfi.initialized = true;
  gNyFfi.fast_calls_enabled = rt_env_enabled("NYTRIX_FFIG_FAST_CALLS");
}

void nyFfiGates_dispose(void) {
  if (!gNyFfi.initialized)
    return;
  for (size_t i = 0; i < gNyFfi.cache.count; i++) {
    free((void *)gNyFfi.cache.entries[i].name);
  }
  free(gNyFfi.cache.entries);
  free(gNyFfi.gates);
  memset_manual(&gNyFfi, 0, sizeof(gNyFfi));
}

void nyFfiRegisterGate(nyFfiSigKind_t sig, const char *name, void *stub) {
  if (gNyFfi.gate_count >= gNyFfi.gate_capacity) {
    size_t next_capacity = gNyFfi.gate_capacity ? gNyFfi.gate_capacity * 2 : 64;
    nyFfiGate_t *next = realloc(gNyFfi.gates, next_capacity * sizeof(*next));
    if (!next)
      return;
    gNyFfi.gates = next;
    gNyFfi.gate_capacity = next_capacity;
  }
  nyFfiGate_t *gate = &gNyFfi.gates[gNyFfi.gate_count++];
  gate->sig = sig;
  gate->name = name;
  gate->stub = stub;
}

void *nyFfiLookupGate(const char *name, nyFfiSigKind_t sig) {
  for (size_t i = 0; i < gNyFfi.gate_count; i++) {
    if (gNyFfi.gates[i].sig == sig && strcmp(gNyFfi.gates[i].name, name) == 0) {
      return gNyFfi.gates[i].stub;
    }
  }
  return NULL;
}

nyFfiSigKind_t nyFfiDetectSignature(const char *name, size_t argc) {
  (void)name;
  switch (argc) {
  case 0:
    return NY_FFI_SIG_I_V;
  case 1:
    return NY_FFI_SIG_I_I;
  case 2:
    return NY_FFI_SIG_I_II;
  case 3:
    return NY_FFI_SIG_I_III;
  case 4:
    return NY_FFI_SIG_I_IIII;
  case 5:
    return NY_FFI_SIG_I_IIIII;
  default:
    return NY_FFI_SIG_GENERIC;
  }
}

void nyFfiCache_insert(const char *name, void *addr, nyFfiSigKind_t sig) {
  if (!name || !*name || !gNyFfi.cache.entries || !gNyFfi.cache.capacity)
    return;
  uint64_t hash = ny_hash64_cstr(name);
  for (size_t i = 0; i < gNyFfi.cache.count; ++i) {
    nyFfiCacheEntry_t *entry = &gNyFfi.cache.entries[i];
    if (entry->name_hash == hash && entry->sig == sig &&
        strcmp(entry->name, name) == 0) {
      entry->address = addr;
      entry->gate = nyFfiLookupGate(entry->name, sig);
      return;
    }
  }
  if (gNyFfi.cache.count >= gNyFfi.cache.capacity) {
    size_t keep = gNyFfi.cache.capacity / 2;
    size_t drop = gNyFfi.cache.capacity - keep;
    for (size_t i = 0; i < drop; ++i)
      free((void *)gNyFfi.cache.entries[i].name);
    memmove(gNyFfi.cache.entries, &gNyFfi.cache.entries[gNyFfi.cache.capacity - keep],
            keep * sizeof(nyFfiCacheEntry_t));
    gNyFfi.cache.count = keep;
  }

  char *owned_name = strdup(name);
  if (!owned_name)
    return;
  nyFfiCacheEntry_t *entry = &gNyFfi.cache.entries[gNyFfi.cache.count++];
  entry->name = owned_name;
  entry->name_hash = hash;
  entry->address = addr;
  entry->sig = sig;
  entry->gate = nyFfiLookupGate(owned_name, sig);
  entry->call_count = 0;
  entry->last_call_time = 0;
}

nyFfiCacheEntry_t *nyFfiCache_lookup(const char *name) {
  uint64_t hash = ny_hash64_cstr(name);

  for (size_t i = 0; i < gNyFfi.cache.count; i++) {
    if (gNyFfi.cache.entries[i].name_hash == hash &&
        strcmp(gNyFfi.cache.entries[i].name, name) == 0) {
      gNyFfi.cache.entries[i].call_count++;
      gNyFfi.cache.hits++;
      return &gNyFfi.cache.entries[i];
    }
  }

  gNyFfi.cache.misses++;
  return NULL;
}

void nyFfiCache_remove(const char *name) {
  uint64_t hash = ny_hash64_cstr(name);

  for (size_t i = 0; i < gNyFfi.cache.count; i++) {
    if (gNyFfi.cache.entries[i].name_hash == hash &&
        strcmp(gNyFfi.cache.entries[i].name, name) == 0) {
      free((void *)gNyFfi.cache.entries[i].name);
      gNyFfi.cache.entries[i] = gNyFfi.cache.entries[--gNyFfi.cache.count];
      return;
    }
  }
}

int64_t nyFfiCallIV(void *fn) {
  gNyFfi.stats.fast_calls++;
  return stub_i_v(fn);
}

int64_t nyFfiCallII(void *fn, int64_t a0) {
  gNyFfi.stats.fast_calls++;
  return stub_i_i(fn, a0);
}

int64_t nyFfiCallIIi(void *fn, int64_t a0, int64_t a1) {
  gNyFfi.stats.fast_calls++;
  return stub_i_ii(fn, a0, a1);
}

int64_t nyFfiCallIIii(void *fn, int64_t a0, int64_t a1, int64_t a2) {
  gNyFfi.stats.fast_calls++;
  return stub_i_iii(fn, a0, a1, a2);
}

int64_t nyFfiCallIIiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  gNyFfi.stats.fast_calls++;
  return stub_i_iiii(fn, a0, a1, a2, a3);
}

int64_t nyFfiCallIIiiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4) {
  gNyFfi.stats.fast_calls++;
  return stub_i_iiiii(fn, a0, a1, a2, a3, a4);
}

void nyFfiCallVI(void *fn, int64_t a0) {
  gNyFfi.stats.fast_calls++;
  stub_v_i(fn, a0);
}

void nyFfiCallVIi(void *fn, int64_t a0, int64_t a1) {
  gNyFfi.stats.fast_calls++;
  stub_v_ii(fn, a0, a1);
}

void nyFfiCallVIii(void *fn, int64_t a0, int64_t a1, int64_t a2) {
  gNyFfi.stats.fast_calls++;
  stub_v_iii(fn, a0, a1, a2);
}

int64_t nyFfiCallPP(void *fn, int64_t a0) {
  gNyFfi.stats.fast_calls++;
  return stub_p_p(fn, a0);
}

int64_t nyFfiCallPPp(void *fn, int64_t a0, int64_t a1) {
  gNyFfi.stats.fast_calls++;
  return stub_p_pp(fn, a0, a1);
}

int64_t nyFfiCallPPi(void *fn, int64_t a0, int64_t a1) {
  gNyFfi.stats.fast_calls++;
  return stub_p_pi(fn, a0, a1);
}

int64_t nyFfiCallPII(void *fn, int64_t a0, int64_t a1) {
  gNyFfi.stats.fast_calls++;
  return ((int64_t (*)(int64_t, int64_t))fn)(a0, a1);
}

int64_t nyFfiCallPIIi(void *fn, int64_t a0, int64_t a1, int64_t a2) {
  gNyFfi.stats.fast_calls++;
  return ((int64_t (*)(int64_t, int64_t, int64_t))fn)(a0, a1, a2);
}

int64_t nyFfiCallPIIii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  gNyFfi.stats.fast_calls++;
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fn)(a0, a1, a2, a3);
}

int64_t nyFfiCallGeneric(void *fn, int64_t *args, size_t argc) {
  gNyFfi.stats.slow_calls++;
  switch (argc) {
  case 0:
    return rt_call0((int64_t)fn);
  case 1:
    return rt_call1((int64_t)fn, args[0]);
  case 2:
    return rt_call2((int64_t)fn, args[0], args[1]);
  case 3:
    return rt_call3((int64_t)fn, args[0], args[1], args[2]);
  case 4:
    return rt_call4((int64_t)fn, args[0], args[1], args[2], args[3]);
  case 5:
    return rt_call5((int64_t)fn, args[0], args[1], args[2], args[3], args[4]);
  case 6:
    return rt_call6((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5]);
  case 7:
    return rt_call7((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
  case 8:
    return rt_call8((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                    args[7]);
  case 9:
    return rt_call9((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                    args[7], args[8]);
  case 10:
    return rt_call10((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9]);
  case 11:
    return rt_call11((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9], args[10]);
  case 12:
    return rt_call12((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9], args[10], args[11]);
  case 13:
    return rt_call13((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9], args[10], args[11], args[12]);
  case 14:
    return rt_call14((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9], args[10], args[11], args[12], args[13]);
  case 15:
    return rt_call15((int64_t)fn, args[0], args[1], args[2], args[3], args[4], args[5], args[6],
                     args[7], args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
  default:
    return 0;
  }
}

void nyFfiDumpStats(FILE *out) {
  if (!out)
    out = stderr;

  fprintf(out, "\n=== FFI Call Gate Statistics ===\n");
  fprintf(out, "Cache entries:         %zu\n", gNyFfi.cache.count);
  fprintf(out, "Cache hits:            %llu\n", (unsigned long long)gNyFfi.cache.hits);
  fprintf(out, "Cache misses:          %llu\n", (unsigned long long)gNyFfi.cache.misses);
  fprintf(out, "Fast calls:            %llu\n", (unsigned long long)gNyFfi.stats.fast_calls);
  fprintf(out, "Slow calls:            %llu\n", (unsigned long long)gNyFfi.stats.slow_calls);
  fprintf(out, "Registered gates:      %zu\n", gNyFfi.gate_count);
  fprintf(out, "Fast calls enabled:    %s\n", gNyFfi.fast_calls_enabled ? "yes" : "no");

  if (gNyFfi.cache.hits + gNyFfi.cache.misses > 0) {
    double hit_rate = 100.0 * gNyFfi.cache.hits / (gNyFfi.cache.hits + gNyFfi.cache.misses);
    fprintf(out, "Cache hit rate:        %.1f%%\n", hit_rate);
  }

  if (gNyFfi.stats.fast_calls + gNyFfi.stats.slow_calls > 0) {
    double fast_rate =
        100.0 * gNyFfi.stats.fast_calls / (gNyFfi.stats.fast_calls + gNyFfi.stats.slow_calls);
    fprintf(out, "Fast call ratio:       %.1f%%\n", fast_rate);
  }
}
