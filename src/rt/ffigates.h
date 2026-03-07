#ifndef NYTRIX_RT_FFI_GATES_H
#define NYTRIX_RT_FFI_GATES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* FFI Signature kinds */
typedef enum nyFfiSigKind {
  NY_FFI_SIG_GENERIC = 0,
  NY_FFI_SIG_I_V,     /* int func(void) */
  NY_FFI_SIG_I_I,     /* int func(int) */
  NY_FFI_SIG_I_II,    /* int func(int, int) */
  NY_FFI_SIG_I_III,   /* int func(int, int, int) */
  NY_FFI_SIG_I_IIII,  /* int func(int, int, int, int) */
  NY_FFI_SIG_I_IIIII, /* int func(int, int, int, int, int) */
  NY_FFI_SIG_V_I,     /* void func(int) */
  NY_FFI_SIG_V_II,    /* void func(int, int) */
  NY_FFI_SIG_V_III,   /* void func(int, int, int) */
  NY_FFI_SIG_P_P,     /* ptr func(ptr) */
  NY_FFI_SIG_P_PP,    /* ptr func(ptr, ptr) */
  NY_FFI_SIG_P_PI,    /* ptr func(ptr, int) */
  NY_FFI_SIG_I_PI,    /* int func(ptr, int) */
  NY_FFI_SIG_I_PII,   /* int func(ptr, int, int) */
  NY_FFI_SIG_I_PIII,  /* int func(ptr, int, int, int) */
} nyFfiSigKind_t;

/* FFI Cache entry */
typedef struct nyFfiCacheEntry {
  const char *name;
  uint64_t name_hash;
  void *address;
  nyFfiSigKind_t sig;
  void *gate;
  uint64_t call_count;
  uint64_t last_call_time;
} nyFfiCacheEntry_t;

/* FFI Cache */
typedef struct nyFfiCache {
  nyFfiCacheEntry_t *entries;
  size_t count;
  size_t capacity;
  uint64_t hits;
  uint64_t misses;
} nyFfiCache_t;

/* FFI Statistics */
typedef struct nyFfiStats {
  uint64_t fast_calls;
  uint64_t slow_calls;
} nyFfiStats_t;

/* FFI Gate function pointer */
typedef int64_t (*nyFfiGateFn)(void *fn, int64_t *args, size_t argc);

/* FFI Gate entry */
typedef struct nyFfiGate {
  nyFfiSigKind_t sig;
  const char *name;
  void *stub;
} nyFfiGate_t;

/* FFI State */
typedef struct nyFfiState {
  nyFfiCache_t cache;
  nyFfiGate_t *gates;
  size_t gate_count;
  size_t gate_capacity;

  nyFfiStats_t stats;

  bool initialized;
  bool fast_calls_enabled;
} nyFfiState_t;

/* Global FFI state */
extern nyFfiState_t gNyFfi;

/* Initialize FFI gates */
void nyFfiGates_init(void);

/* Shutdown FFI gates */
void nyFfiGates_dispose(void);

/* Register a call gate */
void nyFfiRegisterGate(nyFfiSigKind_t sig, const char *name, void *stub);

/* Lookup a gate by name */
void *nyFfiLookupGate(const char *name, nyFfiSigKind_t sig);

/* Cache operations */
void nyFfiCache_insert(const char *name, void *addr, nyFfiSigKind_t sig);
nyFfiCacheEntry_t *nyFfiCache_lookup(const char *name);
void nyFfiCache_remove(const char *name);

/* Detect signature from function name */
nyFfiSigKind_t nyFfiDetectSignature(const char *name, size_t argc);

/* Fast call stubs */
int64_t nyFfiCallIV(void *fn);
int64_t nyFfiCallII(void *fn, int64_t a0);
int64_t nyFfiCallIIi(void *fn, int64_t a0, int64_t a1);
int64_t nyFfiCallIIii(void *fn, int64_t a0, int64_t a1, int64_t a2);
int64_t nyFfiCallIIiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);
int64_t nyFfiCallIIiiii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3, int64_t a4);

void nyFfiCallVI(void *fn, int64_t a0);
void nyFfiCallVIi(void *fn, int64_t a0, int64_t a1);
void nyFfiCallVIii(void *fn, int64_t a0, int64_t a1, int64_t a2);

int64_t nyFfiCallPP(void *fn, int64_t a0);
int64_t nyFfiCallPPp(void *fn, int64_t a0, int64_t a1);
int64_t nyFfiCallPPi(void *fn, int64_t a0, int64_t a1);

int64_t nyFfiCallPII(void *fn, int64_t a0, int64_t a1);
int64_t nyFfiCallPIIi(void *fn, int64_t a0, int64_t a1, int64_t a2);
int64_t nyFfiCallPIIii(void *fn, int64_t a0, int64_t a1, int64_t a2, int64_t a3);

/* Generic fallback */
int64_t nyFfiCallGeneric(void *fn, int64_t *args, size_t argc);

/* Fast inline calls (for JIT) */
static inline int64_t nyFfiFastII(int64_t fn, int64_t a0) { return ((int64_t (*)(int64_t))fn)(a0); }

static inline int64_t nyFfiFastIIi(int64_t fn, int64_t a0, int64_t a1) {
  return ((int64_t (*)(int64_t, int64_t))fn)(a0, a1);
}

static inline int64_t nyFfiFastIIii(int64_t fn, int64_t a0, int64_t a1, int64_t a2) {
  return ((int64_t (*)(int64_t, int64_t, int64_t))fn)(a0, a1, a2);
}

static inline int64_t nyFfiFastPII(int64_t fn, int64_t ptr, int64_t idx) {
  return ((int64_t (*)(int64_t, int64_t))fn)(ptr, idx);
}

/* Dump statistics */
void nyFfiDumpStats(FILE *out);

#endif /* NYTRIX_RT_FFI_GATES_H */
