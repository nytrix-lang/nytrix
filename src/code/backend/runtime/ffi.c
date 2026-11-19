/*
 * FFI runtime: foreign-function interface glue for calling native
 * C functions from Nytrix code with ABI-compliant argument marshalling.
 */
#include "code/backend/runtime/shared.h"
#include <inttypes.h>
#include <string.h>

#ifdef _WIN32

#else
#include <dlfcn.h>
#endif

#if UINTPTR_MAX == 0xffffffff
#define NY_NATIVE_RET0(fn) (int64_t)((intptr_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0)                            \
  (int64_t)((intptr_t (*)(intptr_t))(fn))((intptr_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                  \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t))(fn))((intptr_t)(a0), \
                                                    (intptr_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                         \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t))(fn))( \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                               \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t))(fn))( \
      (intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                                \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t))( \
      fn))((intptr_t)(a0), (intptr_t)(a1), (intptr_t)(a2), (intptr_t)(a3),    \
           (intptr_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                          \
  (int64_t)((intptr_t (*)(intptr_t, intptr_t, intptr_t, intptr_t, intptr_t, \
                          intptr_t))(fn))((intptr_t)(a0), (intptr_t)(a1),   \
                                          (intptr_t)(a2), (intptr_t)(a3),   \
                                          (intptr_t)(a4), (intptr_t)(a5))
#else
#define NY_NATIVE_RET0(fn) ((int64_t (*)(void))(fn))()
#define NY_NATIVE_RET1(fn, a0) ((int64_t (*)(int64_t))(fn))((int64_t)(a0))
#define NY_NATIVE_RET2(fn, a0, a1)                                    \
  ((int64_t (*)(int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1))
#define NY_NATIVE_RET3(fn, a0, a1, a2)                                         \
  ((int64_t (*)(int64_t, int64_t, int64_t))(fn))((int64_t)(a0), (int64_t)(a1), \
                                                 (int64_t)(a2))
#define NY_NATIVE_RET4(fn, a0, a1, a2, a3)                        \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(fn))(        \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3))
#define NY_NATIVE_RET5(fn, a0, a1, a2, a3, a4)                      \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(fn))( \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3),   \
      (int64_t)(a4))
#define NY_NATIVE_RET6(fn, a0, a1, a2, a3, a4, a5)                           \
  ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(fn))( \
      (int64_t)(a0), (int64_t)(a1), (int64_t)(a2), (int64_t)(a3),            \
      (int64_t)(a4), (int64_t)(a5))
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

static void *rt_dl_handle_ptr(int64_t handle) {
  if (NY_NATIVE_IS(handle))
    return NY_NATIVE_DECODE(handle);
  if (is_int(handle)) {
    int64_t unboxed = handle >> 1;
    if (NY_NATIVE_IS(unboxed))
      return NY_NATIVE_DECODE(unboxed);
    return (void *)(uintptr_t)unboxed;
  }
  return (void *)(uintptr_t)handle;
}

int64_t rt_dlsym(int64_t handle, int64_t name) {
  void *p = NULL;
  void *h = rt_dl_handle_ptr(handle);
  const char *nm = (!is_int(name)) ? (const char *)name
                                   : (const char *)(uintptr_t)(name >> 1);
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
  DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS;
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
  void *h = rt_dl_handle_ptr(handle);
#ifdef _WIN32
  if (!h)
    return -1;
  return FreeLibrary((HMODULE)h) ? 0 : -1;
#else
  return dlclose(h);
#endif
}
int64_t rt_ffi_untag_ptr(int64_t v) {
  if (is_int(v)) {
    int64_t raw = rt_untag_v(v);
    if (NY_NATIVE_IS(raw)) {
      uintptr_t pointer = (uintptr_t)NY_NATIVE_DECODE(raw);
      if (pointer > 0x1000)
        return (int64_t)pointer;
    }
    /*
     * Integer handles used as opaque scalar IDs (for example X11 atoms) are
     * valid pointer-shaped FFI arguments even though they are not heap
     * pointers. The tagged ABI argument must be unboxed before forwarding.
     */
    return raw;
  }
  if (NY_NATIVE_IS(v)) {
    uintptr_t pointer = (uintptr_t)NY_NATIVE_DECODE(v);
    if (pointer > 0x1000)
      return (int64_t)pointer;
  }
  return v;
}

#define UNTAG(x) rt_untag_v(x)

/*
 * Dynamic callback values are compact table IDs with ABI flags in the value
 * tag. Keeping code pointers in a side table avoids truncating high pointer
 * bits when flags are added to a shifted function address. IDs are interned
 * by code address and live for the process lifetime, matching callback values
 * that can escape into long-lived dynamic containers.
 */
static atomic_flag rt_dynamic_callable_lock = ATOMIC_FLAG_INIT;
static uintptr_t *rt_dynamic_callable_functions;
static uint64_t *rt_dynamic_callable_arg_masks;
static size_t rt_dynamic_callable_functions_len = 1;
static size_t rt_dynamic_callable_functions_cap;
static size_t *rt_dynamic_callable_buckets;
static size_t rt_dynamic_callable_buckets_cap;
static size_t rt_dynamic_callable_count;

/*
 * Captured closures keep ABI facts beside the code address without changing
 * the cross-target closure object layout.
 */
typedef struct {
  uintptr_t closure;
  uint64_t flags;
} rt_closure_callable_abi_t;
static rt_closure_callable_abi_t *rt_closure_callable_abis;
static size_t rt_closure_callable_abis_len;
static size_t rt_closure_callable_abis_cap;

static void rt_dynamic_callable_lock_acquire(void);
static void rt_dynamic_callable_lock_release(void);
static size_t rt_dynamic_callable_hash(uintptr_t fn);

/*
 * Called with the callable lock held. Closure addresses are nonzero, so an
 * empty address is also the open-addressed table's empty-slot marker.
 */
static rt_closure_callable_abi_t *
rt_dynamic_closure_abi_slot(uintptr_t closure) {
  if (!rt_closure_callable_abis_cap)
    return NULL;
  size_t at = rt_dynamic_callable_hash(closure) &
              (rt_closure_callable_abis_cap - 1);
  while (rt_closure_callable_abis[at].closure &&
         rt_closure_callable_abis[at].closure != closure)
    at = (at + 1) & (rt_closure_callable_abis_cap - 1);
  return &rt_closure_callable_abis[at];
}

static bool rt_dynamic_closure_abi_grow(void) {
  size_t old_cap = rt_closure_callable_abis_cap;
  size_t cap = old_cap ? old_cap * 2 : 128;
  if (cap < old_cap || cap > SIZE_MAX / sizeof(*rt_closure_callable_abis))
    return false;
  rt_closure_callable_abi_t *grown = calloc(cap, sizeof(*grown));
  if (!grown)
    return false;
  for (size_t i = 0; i < old_cap; ++i) {
    rt_closure_callable_abi_t entry = rt_closure_callable_abis[i];
    if (!entry.closure)
      continue;
    size_t at = rt_dynamic_callable_hash(entry.closure) & (cap - 1);
    while (grown[at].closure)
      at = (at + 1) & (cap - 1);
    grown[at] = entry;
  }
  free(rt_closure_callable_abis);
  rt_closure_callable_abis = grown;
  rt_closure_callable_abis_cap = cap;
  return true;
}

static bool rt_dynamic_callable_is_closure(int64_t fn, uintptr_t *closure_out) {
  if (!is_heap_ptr(fn))
    return false;
  uintptr_t base = (uintptr_t)rt_untag_v(fn);
  if (base < sizeof(int64_t) ||
      *(int64_t *)(base - sizeof(int64_t)) != TAG_CLOSURE)
    return false;
  if (closure_out)
    *closure_out = base;
  return true;
}

/*
 * Closure callables are heap objects: word 0 is the prepared code pointer,
 * word 1 is the captured environment.  NULL code/env makes this a pure
 * predicate (same verdict as rt_dynamic_callable_is_closure).
 * Defined near the other callable-prep helpers further below.
 */
static inline int64_t rt_prepare_raw_callable(int64_t f);

static bool rt_ffi_closure_split(int64_t f, int64_t *code, int64_t *env) {
  uintptr_t base;
  if (!rt_dynamic_callable_is_closure(f, &base))
    return false;
  if (code) {
    *code = rt_prepare_raw_callable(*(int64_t *)base);
    *env = *(int64_t *)(base + sizeof(int64_t));
  }
  return true;
}

/*
 * A closure is typically called from a loop, so the same closure reaches the
 * generic adapter thousands of times.  Every such call re-derives its ABI
 * facts from a lock-protected side table and canonicalizes the stored code
 * word again.  Remember the last resolved closure: the stored code word
 * validates the memo, because heap addresses are reusable while code words
 * are not, so a recycled allocation cannot inherit stale flags.
 */
typedef struct {
  uintptr_t closure;
  int64_t code_word;
  int64_t code;
  uint64_t flags;
} rt_closure_call_memo_t;
static _Thread_local rt_closure_call_memo_t rt_closure_call_memo;

static void rt_closure_call_memo_invalidate(uintptr_t closure) {
  if (rt_closure_call_memo.closure == closure)
    rt_closure_call_memo.closure = 0;
}

static uint64_t rt_dynamic_closure_abi_flags_at(uintptr_t closure) {
  uint64_t flags = 0;
  rt_dynamic_callable_lock_acquire();
  rt_closure_callable_abi_t *entry = rt_dynamic_closure_abi_slot(closure);
  if (entry && entry->closure)
    flags = entry->flags;
  rt_dynamic_callable_lock_release();
  return flags;
}

static uint64_t rt_closure_call_resolve(uintptr_t closure, int64_t *code,
                                        int64_t *env);

static uint64_t rt_dynamic_closure_abi_flags(int64_t fn) {
  uintptr_t closure = 0;
  if (!rt_dynamic_callable_is_closure(fn, &closure))
    return 0;
  int64_t code = 0, env = 0;
  return rt_closure_call_resolve(closure, &code, &env);
}

/*
 * Resolve one closure call: canonical code word plus ABI flags, memoized.
 * `closure` has already been validated by the caller.
 */
static uint64_t rt_closure_call_resolve(uintptr_t closure, int64_t *code,
                                        int64_t *env) {
  int64_t code_word = *(int64_t *)closure;
  if (rt_closure_call_memo.closure == closure &&
      rt_closure_call_memo.code_word == code_word) {
    *code = rt_closure_call_memo.code;
  } else {
    rt_closure_call_memo.closure = closure;
    rt_closure_call_memo.code_word = code_word;
    rt_closure_call_memo.code = rt_prepare_raw_callable(code_word);
    rt_closure_call_memo.flags = rt_dynamic_closure_abi_flags_at(closure);
    *code = rt_closure_call_memo.code;
  }
  *env = *(int64_t *)(closure + sizeof(int64_t));
  return rt_closure_call_memo.flags;
}

static bool rt_dynamic_closure_set_abi_flags(int64_t fn, uint64_t flags) {
  uintptr_t closure = 0;
  if (!rt_dynamic_callable_is_closure(fn, &closure))
    return false;
  rt_dynamic_callable_lock_acquire();
  rt_closure_callable_abi_t *entry = rt_dynamic_closure_abi_slot(closure);
  if (entry && entry->closure) {
    if (entry->flags == 0)
      entry->flags = flags;
    rt_dynamic_callable_lock_release();
    rt_closure_call_memo_invalidate(closure);
    return true;
  }
  if (rt_closure_callable_abis_len >= rt_closure_callable_abis_cap / 2) {
    if (!rt_dynamic_closure_abi_grow()) {
      rt_dynamic_callable_lock_release();
      return false;
    }
    entry = rt_dynamic_closure_abi_slot(closure);
  }
  *entry = (rt_closure_callable_abi_t){closure, flags};
  ++rt_closure_callable_abis_len;
  rt_dynamic_callable_lock_release();
  rt_closure_call_memo_invalidate(closure);
  return true;
}

static void rt_dynamic_callable_lock_acquire(void) {
  while (atomic_flag_test_and_set_explicit(&rt_dynamic_callable_lock,
                                           memory_order_acquire)) {
  }
}

static void rt_dynamic_callable_lock_release(void) {
  atomic_flag_clear_explicit(&rt_dynamic_callable_lock, memory_order_release);
}

static size_t rt_dynamic_callable_hash(uintptr_t fn) {
  uint64_t x = (uint64_t)fn;
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return (size_t)x;
}

static bool rt_dynamic_callable_rehash(size_t capacity) {
  size_t *buckets = calloc(capacity, sizeof(*buckets));
  if (!buckets)
    return false;
  for (size_t id = 1; id < rt_dynamic_callable_functions_len; ++id) {
    size_t at = rt_dynamic_callable_hash(rt_dynamic_callable_functions[id]) &
                (capacity - 1);
    while (buckets[at])
      at = (at + 1) & (capacity - 1);
    buckets[at] = id;
  }
  free(rt_dynamic_callable_buckets);
  rt_dynamic_callable_buckets = buckets;
  rt_dynamic_callable_buckets_cap = capacity;
  return true;
}

/*
 * Bits 58-63 are ABI flags (F64_RESULT is bit 58) and bits 0-2 the tag, so a
 * table ID occupies bits 3-57.  Masking bit 58 into the ID made every
 * f64-result callable decode to an out-of-range ID and dispatch to NULL.
 */
static inline uint64_t rt_dynamic_callable_id_bits(int64_t callable) {
  return (((uint64_t)callable) & ((UINT64_C(1) << 58) - 1)) >> 3;
}

int64_t rt_dynamic_callable_encode(void *fn, uint64_t flags) {
  if (!fn)
    return 0;
  uintptr_t address = (uintptr_t)fn;
  rt_dynamic_callable_lock_acquire();
  if (!rt_dynamic_callable_buckets_cap && !rt_dynamic_callable_rehash(128)) {
    rt_dynamic_callable_lock_release();
    return 0;
  }
  size_t at =
      rt_dynamic_callable_hash(address) & (rt_dynamic_callable_buckets_cap - 1);
  while (rt_dynamic_callable_buckets[at]) {
    size_t id = rt_dynamic_callable_buckets[at];
    if (rt_dynamic_callable_functions[id] == address) {
      uint64_t encoded = NY_DYNAMIC_CALLABLE_MARK | ((uint64_t)id << 3) |
                         NY_DYNAMIC_CALLABLE_TAG |
                         (flags & (NY_DYNAMIC_CALLABLE_BOOL_MARK |
                                   NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                                   NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK |
                                   NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
                                   NY_DYNAMIC_CALLABLE_F64_RESULT_MARK));
      rt_dynamic_callable_lock_release();
      return (int64_t)encoded;
    }
    at = (at + 1) & (rt_dynamic_callable_buckets_cap - 1);
  }
  if (rt_dynamic_callable_count >= rt_dynamic_callable_buckets_cap / 2) {
    if (rt_dynamic_callable_buckets_cap > SIZE_MAX / 2 ||
        !rt_dynamic_callable_rehash(rt_dynamic_callable_buckets_cap * 2)) {
      rt_dynamic_callable_lock_release();
      return 0;
    }
    at = rt_dynamic_callable_hash(address) &
         (rt_dynamic_callable_buckets_cap - 1);
    while (rt_dynamic_callable_buckets[at])
      at = (at + 1) & (rt_dynamic_callable_buckets_cap - 1);
  }
  if (rt_dynamic_callable_functions_len >= (UINT64_C(1) << 55) ||
      rt_dynamic_callable_functions_len == SIZE_MAX) {
    rt_dynamic_callable_lock_release();
    return 0;
  }
  if (rt_dynamic_callable_functions_len >= rt_dynamic_callable_functions_cap) {
    size_t cap = rt_dynamic_callable_functions_cap
                     ? rt_dynamic_callable_functions_cap * 2
                     : 64;
    if (cap < rt_dynamic_callable_functions_cap ||
        cap > SIZE_MAX / sizeof(*rt_dynamic_callable_functions)) {
      rt_dynamic_callable_lock_release();
      return 0;
    }
    uintptr_t *functions =
        realloc(rt_dynamic_callable_functions, cap * sizeof(*functions));
    if (!functions) {
      rt_dynamic_callable_lock_release();
      return 0;
    }
    rt_dynamic_callable_functions = functions;
    uint64_t *arg_masks =
        realloc(rt_dynamic_callable_arg_masks, cap * sizeof(*arg_masks));
    if (!arg_masks) {
      rt_dynamic_callable_lock_release();
      return 0;
    }
    memset(arg_masks + rt_dynamic_callable_functions_cap, 0,
           (cap - rt_dynamic_callable_functions_cap) * sizeof(*arg_masks));
    rt_dynamic_callable_arg_masks = arg_masks;
    rt_dynamic_callable_functions_cap = cap;
  }
  size_t id = rt_dynamic_callable_functions_len++;
  rt_dynamic_callable_functions[id] = address;
  rt_dynamic_callable_arg_masks[id] = 0;
  rt_dynamic_callable_buckets[at] = id;
  rt_dynamic_callable_count++;
  uint64_t encoded = NY_DYNAMIC_CALLABLE_MARK | ((uint64_t)id << 3) |
                     NY_DYNAMIC_CALLABLE_TAG |
                     (flags & (NY_DYNAMIC_CALLABLE_BOOL_MARK |
                               NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                               NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK |
                               NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
                               NY_DYNAMIC_CALLABLE_F64_RESULT_MARK));
  rt_dynamic_callable_lock_release();
  return (int64_t)encoded;
}

void *rt_dynamic_callable_decode(int64_t callable) {
  if (!NY_DYNAMIC_CALLABLE_IS(callable))
    return NULL;
  uint64_t id64 = rt_dynamic_callable_id_bits(callable);
  if (id64 == 0 || id64 > SIZE_MAX)
    return NULL;
  size_t id = (size_t)id64;
  static _Thread_local size_t cached_id;
  static _Thread_local void *cached_fn;
  if (cached_id == id)
    return cached_fn;
  rt_dynamic_callable_lock_acquire();
  void *fn = id < rt_dynamic_callable_functions_len
                 ? (void *)rt_dynamic_callable_functions[id]
                 : NULL;
  rt_dynamic_callable_lock_release();
  cached_id = id;
  cached_fn = fn;
  return fn;
}

static bool rt_dynamic_callable_store_arg_mask(int64_t callable,
                                               uint64_t arg_mask) {
  if (!NY_DYNAMIC_CALLABLE_IS(callable))
    return false;
  uint64_t id64 = rt_dynamic_callable_id_bits(callable);
  if (id64 == 0 || id64 > SIZE_MAX)
    return false;
  size_t id = (size_t)id64;
  rt_dynamic_callable_lock_acquire();
  bool valid = id < rt_dynamic_callable_functions_len;
  if (valid)
    rt_dynamic_callable_arg_masks[id] = arg_mask;
  rt_dynamic_callable_lock_release();
  return valid;
}

bool rt_dynamic_callable_has_arg_mask(int64_t callable) {
  if (!NY_DYNAMIC_CALLABLE_IS(callable))
    return false;
  uint64_t id64 = rt_dynamic_callable_id_bits(callable);
  if (id64 == 0 || id64 > SIZE_MAX)
    return false;
  size_t id = (size_t)id64;
  rt_dynamic_callable_lock_acquire();
  bool valid = id < rt_dynamic_callable_functions_len &&
               rt_dynamic_callable_arg_masks[id] != 0;
  rt_dynamic_callable_lock_release();
  return valid;
}

bool rt_dynamic_callable_uses_any_abi(int64_t callable) {
  return NY_DYNAMIC_CALLABLE_IS(callable) ||
         rt_dynamic_callable_is_closure(callable, NULL);
}

static uint64_t rt_dynamic_callable_arg_mask(int64_t callable) {
  if (!NY_DYNAMIC_CALLABLE_IS(callable))
    return 0;
  uint64_t id64 = rt_dynamic_callable_id_bits(callable);
  if (id64 == 0 || id64 > SIZE_MAX)
    return 0;
  size_t id = (size_t)id64;
  rt_dynamic_callable_lock_acquire();
  uint64_t arg_mask = id < rt_dynamic_callable_functions_len
                          ? rt_dynamic_callable_arg_masks[id]
                          : 0;
  rt_dynamic_callable_lock_release();
  return arg_mask;
}

static inline int64_t rt_prepare_raw_callable(int64_t f) {
  /*
   * A dlsym result is carried as NY_NATIVE_ENCODE through `any` values.
   * Generic C-call adapters receive that encoded word directly, unlike the
   * explicitly typed adapters that decode it themselves. Preserve genuine
   * raw callable addresses whose low bits happen to match the native tag.
   * A native pointer held in an `any` slot is itself tagged as an integer by
   * some lowering paths. Unbox that value before testing its native marker.
   */
  if (is_int(f)) {
    uintptr_t raw = (uintptr_t)f;
    int64_t unboxed = rt_untag_v(f);
    if (NY_NATIVE_IS(unboxed)) {
      uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
        return (int64_t)decoded;
    }
  }
  if (NY_NATIVE_IS(f)) {
    uintptr_t raw = (uintptr_t)f;
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(f);
    if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
      return (int64_t)decoded;
  }
  return rt_fix_fn_ptr(f);
}

static inline int64_t rt_prepare_raw_callable_with_hint(int64_t f,
                                                        uintptr_t code_hint) {
  /*
   * MCJIT can hand back a sign-extended 32-bit relocation for a function
   * pointer embedded in a constant expression.  The ordinary fixer sees its
   * own runtime caller, whose address range is unrelated to the JIT code.
   * When a generated callback invokes rt_call0, use that generated caller as
   * the address-space hint to restore the callback's high bits.
   */
  if (is_int(f)) {
    uintptr_t raw = (uintptr_t)f;
    int64_t unboxed = rt_untag_v(f);
    if (NY_NATIVE_IS(unboxed)) {
      uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
        return (int64_t)decoded;
    }
  }
  if (NY_NATIVE_IS(f)) {
    uintptr_t raw = (uintptr_t)f;
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(f);
    if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
      return (int64_t)decoded;
  }
  return rt_fix_fn_ptr_with_hint(f, code_hint);
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
  /*
   * A callable passed through an `any` formal can arrive as the ordinary
   * tagged-word encoding of its code address (address << INT_SHIFT | tag).
   * It is not a dynamic-callable ID yet, so recognize it only when the raw
   * word is not a readable address and untagging produces one. This keeps
   * negative integers and already canonical function pointers untouched.
   */
  if (is_int(fn)) {
    uintptr_t raw = (uintptr_t)fn;
    uintptr_t decoded = (uintptr_t)rt_untag_v(fn);
    if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
      return (int64_t)decoded;
    int64_t unboxed = rt_untag_v(fn);
    if (NY_NATIVE_IS(unboxed)) {
      uintptr_t native = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(native, 1))
        return (int64_t)native;
    }
  }
  /*
   * Some function values arrive from native lowering in NY_NATIVE's shifted
   * pointer form.  Decode that form only when the word itself is not a
   * readable address and its candidate payload is; executable addresses can
   * naturally have the same low marker bits and must remain untouched.
   */
  if (NY_NATIVE_IS(fn)) {
    uintptr_t raw = (uintptr_t)fn;
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(fn);
    if (!rt_addr_readable_safe(raw, 1) && rt_addr_readable_safe(decoded, 1))
      return (int64_t)decoded;
  }
  return fn;
}

/*
 * Apply the dynamic-callable representation in one place. Keep ABI policy in
 * the flags, so every public marker uses the same address conversion.
 */
static uint64_t rt_dynamic_callable_flags(int64_t fn);
static int64_t rt_dynamic_callable_with_flags(int64_t fn, uint64_t flags) {
  uintptr_t closure = 0;
  if (rt_dynamic_callable_is_closure(fn, &closure)) {
    /*
     * The closure's creating function owns its argument and result ABI.
     * Generic call sites must not infer a new result ABI and overwrite it.
     *
     * Flags latch once, at creation: rt_dynamic_closure_set_abi_flags()
     * refuses to overwrite a nonzero entry.  Call sites re-mark the same value
     * on every use, inside loops, so answer from the resolved memo instead of
     * taking the callable lock twice for a no-op.
     */
    int64_t code = 0, env = 0;
    if (rt_closure_call_resolve(closure, &code, &env) != 0)
      return fn;
    return rt_dynamic_closure_set_abi_flags(fn, flags) ? fn : 0;
  }
  fn = rt_dynamic_callable_base(fn);
  if (!fn)
    return 0;
  return rt_dynamic_callable_encode((void *)(uintptr_t)fn, flags);
}

static uint64_t rt_dynamic_callable_flags(int64_t fn) {
  if (rt_dynamic_callable_is_closure(fn, NULL))
    return rt_dynamic_closure_abi_flags(fn);
  if (!NY_DYNAMIC_CALLABLE_IS(fn))
    return 0;
  return (uint64_t)fn &
         (NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
          NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK |
          NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
          NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
}

uint64_t rt_dynamic_callable_abi_flags(int64_t fn) {
  return rt_dynamic_callable_flags(fn);
}

int64_t rt_mark_dynamic_callable(int64_t fn) {
  return rt_dynamic_callable_with_flags(fn, rt_dynamic_callable_flags(fn));
}

int64_t rt_mark_dynamic_bool_callable(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  return rt_dynamic_callable_with_flags(fn,
                                        flags | NY_DYNAMIC_CALLABLE_BOOL_MARK);
}

int64_t rt_mark_dynamic_bool_callable_tagged_args(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &= ~NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_BOOL_MARK |
              NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK);
}

int64_t rt_mark_dynamic_callable_tagged_args(int64_t fn) {
  /*
   * A raw-argument callback marker describes an intentional ABI contract.
   * Generic callback dispatch may add its own marker later, but must not
   * rewrite that callee to consume tagged arguments.
   */
  uint64_t flags = rt_dynamic_callable_flags(fn);
  bool closure = rt_dynamic_callable_is_closure(fn, NULL);
  if ((NY_DYNAMIC_CALLABLE_IS(fn) || (closure && flags != 0)) &&
      !(flags & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK))
    return fn;
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK);
}

int64_t rt_mark_dynamic_callable_arg_mask(int64_t fn, int64_t arg_mask) {
  int64_t marked =
      rt_dynamic_callable_with_flags(fn, rt_dynamic_callable_flags(fn));
  if (!marked ||
      !rt_dynamic_callable_store_arg_mask(marked, (uint64_t)arg_mask))
    return 0;
  return marked;
}

int64_t rt_mark_dynamic_callable_raw_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  if (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) {
    /*
     * A callable's known Boolean ABI is stronger evidence than the generic
     * result expectation at an any-typed call site. Preserve it so a raw
     * 0/1 predicate is not boxed as an integer and then decoded as a bool.
     */
    flags &= ~NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
    return rt_dynamic_callable_with_flags(fn, flags);
  }
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
}

int64_t rt_mark_dynamic_callable_tagged_args_raw_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  if (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) {
    flags &= ~NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
    flags |= NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK;
    if ((NY_DYNAMIC_CALLABLE_IS(fn) ||
         (rt_dynamic_callable_is_closure(fn, NULL) &&
          rt_dynamic_callable_flags(fn) != 0)) &&
        !(rt_dynamic_callable_flags(fn) & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK))
      flags &= ~NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK;
    return rt_dynamic_callable_with_flags(fn, flags);
  }
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  if ((NY_DYNAMIC_CALLABLE_IS(fn) ||
       (rt_dynamic_callable_is_closure(fn, NULL) &&
        rt_dynamic_callable_flags(fn) != 0)) &&
      !(rt_dynamic_callable_flags(fn) & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK))
    return rt_dynamic_callable_with_flags(
        fn, flags | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
}

/*
 * Native NYIR and machine-code callbacks use one machine word per source
 * parameter. The legacy LLVM callback ABI still expands dynamic parameters
 * into value/length/tag triples, so native lowering adds this separate ABI
 * bit when it creates the callable marker.
 */
int64_t rt_mark_dynamic_callable_word_args(int64_t fn) {
  return rt_dynamic_callable_with_flags(
      fn, rt_dynamic_callable_flags(fn) | NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
}

int64_t rt_mark_dynamic_callable_word_args_raw_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  if (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) {
    flags &= ~NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
    return rt_dynamic_callable_with_flags(
        fn, flags | NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
  }
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
}

int64_t rt_mark_dynamic_callable_f64_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
}

int64_t rt_mark_dynamic_callable_word_args_f64_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  int64_t marked = rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return marked;
}

int64_t rt_mark_dynamic_callable_tagged_args_f64_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
}

int64_t rt_mark_dynamic_callable_tagged_word_args_f64_result(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
}

int64_t rt_mark_dynamic_callable_tagged_word_args(int64_t fn) {
  if (rt_dynamic_callable_is_closure(fn, NULL) &&
      rt_dynamic_callable_flags(fn) != 0 &&
      !(rt_dynamic_callable_flags(fn) & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK))
    return rt_mark_dynamic_callable_word_args(fn);
  return rt_dynamic_callable_with_flags(
      fn, rt_dynamic_callable_flags(fn) | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
}

int64_t rt_mark_dynamic_callable_tagged_word_args_raw_result(int64_t fn) {
  if (rt_dynamic_callable_is_closure(fn, NULL) &&
      rt_dynamic_callable_flags(fn) != 0 &&
      !(rt_dynamic_callable_flags(fn) & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK))
    return rt_mark_dynamic_callable_word_args_raw_result(fn);
  uint64_t flags = rt_dynamic_callable_flags(fn);
  if (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) {
    flags &= ~NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK;
    return rt_dynamic_callable_with_flags(
        fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
                NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
  }
  flags &=
      ~(NY_DYNAMIC_CALLABLE_BOOL_MARK | NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK);
}

int64_t rt_mark_dynamic_bool_callable_word_args(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &= ~(NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK |
             NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return rt_dynamic_callable_with_flags(fn,
                                        flags | NY_DYNAMIC_CALLABLE_BOOL_MARK |
                                            NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
}

int64_t rt_mark_dynamic_bool_callable_tagged_word_args(int64_t fn) {
  uint64_t flags = rt_dynamic_callable_flags(fn);
  flags &= ~(NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK |
             NY_DYNAMIC_CALLABLE_F64_RESULT_MARK);
  return rt_dynamic_callable_with_flags(
      fn, flags | NY_DYNAMIC_CALLABLE_BOOL_MARK |
              NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK |
              NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK);
}

/*
 * Native NYIR functions represent each `any` parameter as three i64 ABI
 * slots: value, dynamic length, and raw runtime tag.  The ordinary rt_callN
 * helpers intentionally preserve the legacy one-slot C ABI, so registry
 * callbacks need this explicit adapter.
 */
typedef int64_t (*rt_any_fn6)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t);
typedef int64_t (*rt_any_fn7)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t);
typedef int64_t (*rt_any_fn9)(int64_t, int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn10)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn12)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
typedef int64_t (*rt_any_fn13)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t);

static int64_t rt_any_len(int64_t value) { return rt_sequence_len_safe(value); }

static int64_t rt_any_tag(int64_t value) { return rt_value_tag(value); }

int64_t rt_dynamic_callback_normalize_arg(int64_t value, bool raw_scalars) {
  /*
   * Native tbuf reads may box a raw string pointer as an integer-tagged
   * pointer. Dynamic string operations consume the pointer form, while
   * ordinary integers must remain tagged for any arithmetic.
   */
  if (!is_v_str(value) && rt_native_is_str(value))
    return rt_cstr_to_str(value);
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      return decoded;
    if (is_v_str(decoded))
      return decoded;
    if (rt_native_is_str(decoded))
      return rt_cstr_to_str(decoded);
  }
  if (is_int(value)) {
    int64_t raw = rt_untag_v(value);
    if (is_v_str(raw))
      return raw;
    if (rt_native_is_str(raw))
      return rt_cstr_to_str(raw);
    if (!raw_scalars)
      return value;
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

int64_t rt_dynamic_callback_normalize_result(int64_t value, bool returns_bool,
                                             bool raw_scalars,
                                             bool raw_result) {
  /*
   * An explicit raw integer result contract outranks every payload-bit
   * predicate. Integers such as 2, 8, and 42 collide with pointer/boolean
   * encodings; recognizing them as values skips boxing and makes the
   * dynamic caller decode the raw result a second time.
   */
  if (raw_result)
    return rt_box_i64(value);
  /*
   * A callback returning a native C-string must be promoted before it is
   * consumed by a managed string builder or concatenation routine.
   * Tagged Ny callbacks already return canonical values.  Only the raw
   * native ABI needs C-string promotion; converting a managed SSO string as
   * a C pointer can re-enter the string accessor path.
   */
  if (!returns_bool && raw_scalars && rt_native_is_str(value) &&
      !is_v_str(value))
    return rt_cstr_to_str(value);
  if (returns_bool && (value == 0 || value == 1))
    /*
     * Dynamic calls return canonical Ny values, including booleans.
     */
    return value ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (!raw_scalars) {
    /*
     * Dynamic lambda bodies normally return canonical Ny values.  Imported
     * native bodies can, however, cross an inferred scalar return edge that
     * integer-tags a raw object address.  Decode exactly one integer layer
     * only when the payload has an authoritative runtime object/container
     * tag; ordinary tagged integers remain untouched.
     */
    if (is_int(value)) {
      int64_t decoded = rt_untag_v(value);
      int64_t decoded_tag = rt_value_tag(decoded);
      if (decoded_tag >= TAG_LIST && decoded_tag <= 255)
        return decoded;
    }
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

/*
 * Decode the callable's dynamic ABI once for every arity-specific adapter.
 * The value carries whether its body expects tagged arguments and how its
 * scalar result is represented; the machine code address is not reclassified
 * after this point.
 */
typedef struct {
  int64_t fn;
  bool dynamic;
  bool returns_bool;
  bool tagged_args;
  bool raw_result;
  bool f64_result;
  bool word_args;
  uint64_t arg_mask;
} rt_dynamic_callable_abi_t;

/*
 * Convert the boxed native words used by generic FFI calls into the machine
 * words expected by C. Ordinary integers and managed heap pointers retain the
 * established unboxing behavior; NY_NATIVE values become raw addresses.
 */
static int64_t rt_ffi_normalize_callable(int64_t fn) {
  if (is_int(fn)) {
    int64_t unboxed = rt_untag_v(fn);
    if (NY_NATIVE_IS(unboxed)) {
      uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      if (rt_addr_readable_safe(decoded, 1))
        return unboxed;
    }
  }
  return fn;
}

/*
 * Shared admission rail for C-function-pointer callables: normalize the word,
 * then reject dynamic callables and closures.  Returns nonzero with *f holding
 * the error word; zero means the value in *f is admitted.
 */
static inline int rt_ffi_require_c_fn(int64_t *f) {
  int rejected = 0;
  *f = rt_ffi_normalize_callable(*f);
  if (NY_DYNAMIC_CALLABLE_IS(*f) || rt_dynamic_callable_is_closure(*f, NULL)) {
    *f = rt_fail(rt_alloc_string("err.type"),
                 rt_alloc_string("core.os.ffi requires a C function pointer"));
    rejected = 1;
  }
  return rejected;
}

static int64_t rt_ffi_native_word(int64_t value) {
  int64_t raw = rt_untag_v(value);
  if (is_heap_ptr(raw))
    return (int64_t)(uintptr_t)rt_untag_v(raw);
  if (NY_NATIVE_IS(raw)) {
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(raw);
    if (rt_addr_readable_safe(decoded, 1))
      return (int64_t)decoded;
  }
  return raw;
}

static rt_dynamic_callable_abi_t rt_dynamic_callable_abi(int64_t fn) {
  if (rt_dynamic_callable_is_closure(fn, NULL)) {
    uint64_t flags = rt_dynamic_closure_abi_flags(fn);
    return (rt_dynamic_callable_abi_t){
        .fn = fn,
        .dynamic = false,
        .returns_bool = (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) != 0,
        .tagged_args = (flags & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK) != 0,
        .raw_result = (flags & NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK) != 0,
        .f64_result = (flags & NY_DYNAMIC_CALLABLE_F64_RESULT_MARK) != 0,
        .word_args = (flags & NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK) != 0,
        .arg_mask = 0,
    };
  }
  rt_dynamic_callable_abi_t abi = {
      .fn = fn,
      .dynamic = NY_DYNAMIC_CALLABLE_IS(fn),
      .returns_bool = NY_DYNAMIC_CALLABLE_BOOL_IS(fn),
      .tagged_args = NY_DYNAMIC_CALLABLE_TAGGED_ARGS_IS(fn),
      .raw_result = NY_DYNAMIC_CALLABLE_RAW_RESULT_IS(fn),
      .f64_result = (rt_dynamic_callable_flags(fn) &
                     NY_DYNAMIC_CALLABLE_F64_RESULT_MARK) != 0,
      .word_args = NY_DYNAMIC_CALLABLE_WORD_ARGS_IS(fn),
      .arg_mask = rt_dynamic_callable_arg_mask(fn),
  };
  if (!abi.dynamic && is_int(fn)) {
    int64_t unboxed = rt_untag_v(fn);
    if (NY_NATIVE_IS(unboxed)) {
      uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      if (rt_addr_readable_safe(decoded, 1))
        abi.fn = unboxed;
    }
  }
  if (abi.dynamic)
    abi.fn = rt_dynamic_callable_base(
        (int64_t)(uintptr_t)NY_DYNAMIC_CALLABLE_DECODE(fn));
  return abi;
}

static inline bool rt_dynamic_callback_prepare(int64_t *fn) {
  bool is_closure = rt_dynamic_callable_is_closure(*fn, NULL);
  if (!is_closure)
    *fn = rt_prepare_raw_callable(*fn);
  return is_closure;
}

/*
 * Tagged-value callbacks compiled by the native backend use one machine word
 * per source parameter. Keep this separate from the legacy LLVM callback ABI,
 * which expands dynamic formals into value/length/tag triplets.
 */
static int64_t rt_dynamic_callback_call_words(int64_t fn, const int64_t *args,
                                              size_t arity) {
  switch (arity) {
  case 0:
    return ((int64_t (*)(void))(uintptr_t)fn)();
  case 1:
    return ((int64_t (*)(int64_t))(uintptr_t)fn)(args[0]);
  case 2:
    return ((int64_t (*)(int64_t, int64_t))(uintptr_t)fn)(args[0], args[1]);
  case 3:
    return ((int64_t (*)(int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2]);
  case 4:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3]);
  case 5:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(
        uintptr_t)fn)(args[0], args[1], args[2], args[3], args[4]);
  case 6:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(
        uintptr_t)fn)(args[0], args[1], args[2], args[3], args[4], args[5]);
  case 7:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
  case 8:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
  case 9:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8]);
  case 10:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8], args[9]);
  case 11:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t))(
        uintptr_t)fn)(args[0], args[1], args[2], args[3], args[4], args[5],
                      args[6], args[7], args[8], args[9], args[10]);
  case 12:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(
        uintptr_t)fn)(args[0], args[1], args[2], args[3], args[4], args[5],
                      args[6], args[7], args[8], args[9], args[10], args[11]);
  case 13:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8], args[9], args[10], args[11], args[12]);
  case 14:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8], args[9], args[10], args[11], args[12], args[13]);
  case 15:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8], args[9], args[10], args[11], args[12], args[13], args[14]);
  case 16:
    return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                         int64_t, int64_t, int64_t, int64_t))(uintptr_t)fn)(
        args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        args[8], args[9], args[10], args[11], args[12], args[13], args[14],
        args[15]);
  default:
    return 0;
  }
}

/*
 * Expand the native parameter slots consumed by typed sequence arguments.
 */
static size_t rt_dynamic_callback_expand_word_args(int64_t env,
                                                   bool has_env,
                                                   const int64_t *args,
                                                   size_t arity,
                                                   bool tagged_args,
                                                   int64_t *words,
                                                   size_t capacity) {
  size_t count = 0;
  if (has_env) {
    if (count >= capacity)
      return 0;
    words[count++] = env;
  }
  for (size_t i = 0; i < arity; ++i) {
    int64_t value = rt_dynamic_callback_normalize_arg(args[i], !tagged_args);
    if (!tagged_args) {
      const int64_t *header = NULL;
      if (rt_tbuf_check(value, &header) == RT_TBUF_OK) {
        if (count + 2 > capacity)
          return 0;
        words[count++] = value;
        words[count++] = header[1];
        continue;
      }
      if (is_v_str(value)) {
        if (count + 3 > capacity)
          return 0;
        words[count++] = value;
        words[count++] = rt_any_len(value);
        words[count++] = rt_any_tag(value);
        continue;
      }
    }
    if (count >= capacity)
      return 0;
    words[count++] = value;
  }
  return count;
}

static int64_t rt_dynamic_callback_call_f64_words(int64_t fn,
                                                  const int64_t *args,
                                                  size_t arity) {
#define NY_F64_TYPES_1 int64_t
#define NY_F64_TYPES_2 NY_F64_TYPES_1, int64_t
#define NY_F64_TYPES_3 NY_F64_TYPES_2, int64_t
#define NY_F64_TYPES_4 NY_F64_TYPES_3, int64_t
#define NY_F64_TYPES_5 NY_F64_TYPES_4, int64_t
#define NY_F64_TYPES_6 NY_F64_TYPES_5, int64_t
#define NY_F64_TYPES_7 NY_F64_TYPES_6, int64_t
#define NY_F64_TYPES_8 NY_F64_TYPES_7, int64_t
#define NY_F64_TYPES_9 NY_F64_TYPES_8, int64_t
#define NY_F64_TYPES_10 NY_F64_TYPES_9, int64_t
#define NY_F64_TYPES_11 NY_F64_TYPES_10, int64_t
#define NY_F64_TYPES_12 NY_F64_TYPES_11, int64_t
#define NY_F64_TYPES_13 NY_F64_TYPES_12, int64_t
#define NY_F64_TYPES_14 NY_F64_TYPES_13, int64_t
#define NY_F64_TYPES_15 NY_F64_TYPES_14, int64_t
#define NY_F64_TYPES_16 NY_F64_TYPES_15, int64_t
#define NY_F64_ARGS_1 args[0]
#define NY_F64_ARGS_2 NY_F64_ARGS_1, args[1]
#define NY_F64_ARGS_3 NY_F64_ARGS_2, args[2]
#define NY_F64_ARGS_4 NY_F64_ARGS_3, args[3]
#define NY_F64_ARGS_5 NY_F64_ARGS_4, args[4]
#define NY_F64_ARGS_6 NY_F64_ARGS_5, args[5]
#define NY_F64_ARGS_7 NY_F64_ARGS_6, args[6]
#define NY_F64_ARGS_8 NY_F64_ARGS_7, args[7]
#define NY_F64_ARGS_9 NY_F64_ARGS_8, args[8]
#define NY_F64_ARGS_10 NY_F64_ARGS_9, args[9]
#define NY_F64_ARGS_11 NY_F64_ARGS_10, args[10]
#define NY_F64_ARGS_12 NY_F64_ARGS_11, args[11]
#define NY_F64_ARGS_13 NY_F64_ARGS_12, args[12]
#define NY_F64_ARGS_14 NY_F64_ARGS_13, args[13]
#define NY_F64_ARGS_15 NY_F64_ARGS_14, args[14]
#define NY_F64_ARGS_16 NY_F64_ARGS_15, args[15]
#define NY_F64_CASE(N)                                                   \
  case N:                                                                \
    return rt_flt_box_double(                                            \
        ((double (*)(NY_F64_TYPES_##N))(uintptr_t)fn)(NY_F64_ARGS_##N));
  switch (arity) {
  case 0:
    return rt_flt_box_double(((double (*)(void))(uintptr_t)fn)());
    NY_F64_CASE(1)
    NY_F64_CASE(2)
    NY_F64_CASE(3)
    NY_F64_CASE(4)
    NY_F64_CASE(5)
    NY_F64_CASE(6)
    NY_F64_CASE(7)
    NY_F64_CASE(8)
    NY_F64_CASE(9)
    NY_F64_CASE(10)
    NY_F64_CASE(11)
    NY_F64_CASE(12)
    NY_F64_CASE(13)
    NY_F64_CASE(14)
    NY_F64_CASE(15)
    NY_F64_CASE(16)
  default:
    return rt_fail(rt_alloc_string("err.type"),
                   rt_alloc_string("floating callback ABI exceeds 16 words"));
  }
#undef NY_F64_CASE
#undef NY_F64_TYPES_1
#undef NY_F64_TYPES_2
#undef NY_F64_TYPES_3
#undef NY_F64_TYPES_4
#undef NY_F64_TYPES_5
#undef NY_F64_TYPES_6
#undef NY_F64_TYPES_7
#undef NY_F64_TYPES_8
#undef NY_F64_TYPES_9
#undef NY_F64_TYPES_10
#undef NY_F64_TYPES_11
#undef NY_F64_TYPES_12
#undef NY_F64_TYPES_13
#undef NY_F64_TYPES_14
#undef NY_F64_TYPES_15
#undef NY_F64_TYPES_16
#undef NY_F64_ARGS_1
#undef NY_F64_ARGS_2
#undef NY_F64_ARGS_3
#undef NY_F64_ARGS_4
#undef NY_F64_ARGS_5
#undef NY_F64_ARGS_6
#undef NY_F64_ARGS_7
#undef NY_F64_ARGS_8
#undef NY_F64_ARGS_9
#undef NY_F64_ARGS_10
#undef NY_F64_ARGS_11
#undef NY_F64_ARGS_12
#undef NY_F64_ARGS_13
#undef NY_F64_ARGS_14
#undef NY_F64_ARGS_15
#undef NY_F64_ARGS_16
}

static int64_t rt_dynamic_callback_call_f64(rt_dynamic_callable_abi_t abi,
                                            const int64_t *args, size_t arity) {
  int64_t fn = abi.fn;
  int64_t words[46];
  size_t count = 0;
  if (rt_dynamic_callback_prepare(&fn)) {
    int64_t code, env;
    if (!rt_ffi_closure_split(fn, &code, &env))
      return rt_fail(rt_alloc_string("err.type"),
                     rt_alloc_string("invalid floating callback closure"));
    fn = code;
    words[count++] = env;
  }
  for (size_t i = 0; i < arity; ++i) {
    words[count++] =
        rt_dynamic_callback_normalize_arg(args[i], !abi.tagged_args);
    if (!abi.word_args) {
      words[count++] = rt_any_len(args[i]);
      words[count++] = rt_any_tag(args[i]);
    }
  }
  return rt_dynamic_callback_call_f64_words(fn, words, count);
}

int64_t rt_call_any_mixed(int64_t fn, int64_t argc, int64_t argv_ptr) {
  if (argc < 1 || argc > 15 || !argv_ptr)
    return rt_fail(rt_alloc_string("err.type"),
                   rt_alloc_string("invalid mixed callback argument buffer"));
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(fn);
  if (!abi.dynamic || !abi.arg_mask || (abi.arg_mask >> (uint64_t)argc) != 0)
    return rt_fail(rt_alloc_string("err.type"),
                   rt_alloc_string("invalid mixed callback signature"));
  const int64_t *args = (const int64_t *)(uintptr_t)argv_ptr;
  int64_t words[16];
  size_t word_count = 0;
  for (int64_t i = 0; i < argc; ++i) {
    int64_t value = args[i];
    if ((abi.arg_mask & (UINT64_C(1) << i)) != 0) {
      if (word_count + 3 > sizeof(words) / sizeof(words[0]))
        return rt_fail(rt_alloc_string("err.type"),
                       rt_alloc_string("mixed callback ABI exceeds 16 words"));
      words[word_count++] = rt_dynamic_callback_normalize_arg(value, false);
      words[word_count++] = rt_any_len(value);
      words[word_count++] = rt_any_tag(value);
    } else {
      if (word_count >= sizeof(words) / sizeof(words[0]))
        return rt_fail(rt_alloc_string("err.type"),
                       rt_alloc_string("mixed callback ABI exceeds 16 words"));
      words[word_count++] = rt_any_to_i64(value);
    }
  }
  int64_t result = rt_dynamic_callback_call_words(abi.fn, words, word_count);
  return rt_dynamic_callback_normalize_result(result, abi.returns_bool, false,
                                              abi.raw_result);
}

int64_t rt_call_any0(int64_t fn) {
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(fn);
  if (!abi.dynamic && !rt_dynamic_callable_is_closure(fn, NULL))
    return rt_call0(fn);
  int64_t result;
  if (rt_dynamic_callable_is_closure(fn, NULL)) {
    uintptr_t base = (uintptr_t)rt_untag_v(fn);
    int64_t code = rt_prepare_raw_callable(*(int64_t *)base);
    int64_t env = *(int64_t *)(base + 8);
    if (abi.f64_result) {
      double f64 = ((double (*)(int64_t))(uintptr_t)code)(env);
      return rt_flt_box_double(f64);
    }
    result = ((int64_t (*)(int64_t))(uintptr_t)code)(env);
  } else {
    if (abi.f64_result) {
      double f64 = ((double (*)(void))(uintptr_t)abi.fn)();
      return rt_flt_box_double(f64);
    }
    result = rt_dynamic_callback_call_words(abi.fn, NULL, 0);
  }
  return rt_dynamic_callback_normalize_result(result, abi.returns_bool, false,
                                              abi.raw_result);
}

int64_t rt_call_any1(int64_t f, int64_t value) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  /*
   * Captured closures are the recurring shape in loop bodies, adapters and
   * partial applications.  Resolve their ABI facts once per closure and enter
   * the body directly instead of walking the generic classification rail
   * (locked flags lookup, repeated closure predicates, code canonicalization)
   * on every iteration.  The float-result case stays on the generic rail so
   * its boxing rules remain in one place.
   */
  uintptr_t closure = 0;
  if (rt_dynamic_callable_is_closure(f, &closure)) {
    int64_t code = 0, env = 0;
    uint64_t flags = rt_closure_call_resolve(closure, &code, &env);
    if ((flags & NY_DYNAMIC_CALLABLE_F64_RESULT_MARK) == 0) {
      bool returns_bool = (flags & NY_DYNAMIC_CALLABLE_BOOL_MARK) != 0;
      bool raw_result = (flags & NY_DYNAMIC_CALLABLE_RAW_RESULT_MARK) != 0;
      bool tagged_args = (flags & NY_DYNAMIC_CALLABLE_TAGGED_ARGS_MARK) != 0;
      if ((flags & NY_DYNAMIC_CALLABLE_WORD_ARGS_MARK) != 0) {
        int64_t args[] = {value};
        int64_t words[4];
        size_t word_count = rt_dynamic_callback_expand_word_args(
            env, true, args, 1, tagged_args, words,
            sizeof(words) / sizeof(words[0]));
        if (!word_count)
          return rt_fail(rt_alloc_string("err.type"),
                         rt_alloc_string("closure argument ABI overflow"));
        return rt_dynamic_callback_normalize_result(
            rt_dynamic_callback_call_words(code, words, word_count),
            returns_bool, false, raw_result);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn7)code)(env, value, rt_any_len(value), rt_any_tag(value),
                             0, 0, 0),
          returns_bool, false, raw_result);
    }
  }
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi, &value, 1);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  /*
   * The bool mark drives the raw 0/1 result ABI; only the plain bool mark
   * also unboxes integer arguments.  A tagged-args body consumes its
   * parameters through rt_any_* helpers and needs the canonical tagged
   * word (an unboxed 1 would re-classify as integer zero).
   */
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  /*
   * A closure is a heap object, not a function pointer.  Do this check
   * before rt_fix_fn_ptr(), which is allowed to rewrite raw callable
   * addresses and would otherwise hide the closure header.
   */
  rt_dynamic_callback_prepare(&f);
  int64_t len = rt_any_len(value);
  int64_t tag = rt_any_tag(value);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t args[] = {value};
        int64_t words[4];
        size_t word_count = rt_dynamic_callback_expand_word_args(
            env, true, args, 1, abi.tagged_args, words,
            sizeof(words) / sizeof(words[0]));
        if (!word_count)
          return rt_fail(rt_alloc_string("err.type"),
                         rt_alloc_string("closure argument ABI overflow"));
        int64_t result = rt_dynamic_callback_call_words(
            code, words, word_count);
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      int64_t callback_value =
          ((rt_any_fn7)code)(env, value, len, tag, 0, 0, 0);
      return rt_dynamic_callback_normalize_result(
          callback_value, dynamic_bool_callable, false, raw_result_callable);
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
    return rt_any_callback_result(((rt_any_fn6)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(value), len, tag, 0, 0, 0));
  /*
   * Named user functions keep their declared typed ABI.  Sequence adapters
   * enter through the dynamic boundary, so unbox scalar arguments before
   * entering a direct function pointer. Captured closures use the trampoline
   * above and deliberately receive the tagged value.
   */
  if (dynamic_callable && abi.word_args) {
    int64_t args[] = {value};
    int64_t words[3];
    size_t word_count = rt_dynamic_callback_expand_word_args(
        0, false, args, 1, tagged_args_callable, words,
        sizeof(words) / sizeof(words[0]));
    if (!word_count)
      return rt_fail(rt_alloc_string("err.type"),
                     rt_alloc_string("callback argument ABI overflow"));
    int64_t result = rt_dynamic_callback_call_words(f, words, word_count);
    return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                false, raw_result_callable);
  }
  if (dynamic_callable)
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn6)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(value, raw_scalar_callback_args),
            len, tag, 0, 0, 0),
        dynamic_bool_callable, false, raw_result_callable);
  return rt_any_callback_result(
      ((rt_any_fn6)(uintptr_t)f)(rt_any_to_i64(value), len, tag, 0, 0, 0));
}

int64_t rt_call_any2(int64_t f, int64_t left, int64_t right) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi, (int64_t[]){left, right}, 2);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_scalar_callback_args = !tagged_args_callable;
  bool raw_result_callable = abi.raw_result;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t left_len = rt_any_len(left);
  int64_t left_tag = rt_any_tag(left);
  int64_t right_len = rt_any_len(right);
  int64_t right_tag = rt_any_tag(right);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t args[] = {left, right};
        int64_t words[7];
        size_t word_count = rt_dynamic_callback_expand_word_args(
            env, true, args, 2, abi.tagged_args, words,
            sizeof(words) / sizeof(words[0]));
        if (!word_count)
          return rt_fail(rt_alloc_string("err.type"),
                         rt_alloc_string("closure argument ABI overflow"));
        int64_t result = rt_dynamic_callback_call_words(
            code, words, word_count);
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      int64_t callback_value = ((rt_any_fn7)code)(
          env, rt_dynamic_callback_normalize_arg(left, false), left_len,
          left_tag, rt_dynamic_callback_normalize_arg(right, false), right_len,
          right_tag);
      return rt_dynamic_callback_normalize_result(
          callback_value, dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn6)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(left), left_len, left_tag, rt_any_to_i64(right),
        right_len, right_tag));
  if (dynamic_callable) {
    /*
     * word_args describes how arguments arrive at this bridge. Ny callbacks
     * with `any` formals still use the value/length/tag callee ABI, so tagged
     * callbacks must expand each incoming word back to that triplet below.
     */
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[2] = {left, right};
      int64_t words[6];
      size_t word_count = rt_dynamic_callback_expand_word_args(
          0, false, args, 2, tagged_args_callable, words,
          sizeof(words) / sizeof(words[0]));
      if (!word_count)
        return rt_fail(rt_alloc_string("err.type"),
                       rt_alloc_string("callback argument ABI overflow"));
      int64_t result = rt_dynamic_callback_call_words(f, words, word_count);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * Named LLVM functions keep their declared machine-word argument ABI.
     * `rt_call_any2` has no closure environment to expand, so an untagged
     * callable must receive the same raw argument pair used by `rt_call_any3`
     * and the one-argument adapter.
     */
    if (!is_closure && !tagged_args_callable) {
      int64_t result = ((int64_t (*)(int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(left, true),
          rt_dynamic_callback_normalize_arg(right, true));
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn6)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(left, raw_scalar_callback_args),
            left_len, left_tag,
            rt_dynamic_callback_normalize_arg(right, raw_scalar_callback_args),
            right_len, right_tag),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(
      ((rt_any_fn6)(uintptr_t)f)(rt_any_to_i64(left), left_len, left_tag,
                                 rt_any_to_i64(right), right_len, right_tag));
}

int64_t rt_call_any3(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi, (int64_t[]){a0, a1, a2}, 3);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn10)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn9)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[3] = {a0, a1, a2};
      for (size_t i = 0; i < 3; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 3);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * A plain (non-closure) function value keeps the ordinary native ABI:
     * one raw i64 slot per parameter, ints untagged and handles passed
     * through (tui_canvas_loop's draw(c,z,w,h) read w=nil, h=-1 when entered
     * with the value/len/tag triple layout).  Heap closures keep the
     * closure-prologue triple layout.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn9)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn9)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(
      ((rt_any_fn9)(uintptr_t)f)(rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1),
                                 l1, t1, rt_any_to_i64(a2), l2, t2));
}

int64_t rt_call_any4(int64_t f, int64_t a0, int64_t a1, int64_t a2,
                     int64_t a3) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi, (int64_t[]){a0, a1, a2, a3}, 4);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn13)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn12)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[4] = {a0, a1, a2, a3};
      for (size_t i = 0; i < 4; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 4);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn12)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn12)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn12)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3));
}

typedef int64_t (*rt_any_fn15)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn18)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn21)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t);
typedef int64_t (*rt_any_fn24)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn27)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
typedef int64_t (*rt_any_fn30)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn33)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn36)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t);
typedef int64_t (*rt_any_fn39)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn42)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
typedef int64_t (*rt_any_fn45)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
/*
 * Generic dynamic-callback dispatch for higher arities (rt_call_any5..15).
 * Generated from the rt_call_any4 template: closure bodies receive
 * (env, value, len, tag) per argument; named/native callables receive
 * (value, len, tag) per argument; plain non-closure function values keep
 * the ordinary native raw-arg ABI.
 */
typedef int64_t (*rt_any_fn16)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t);
typedef int64_t (*rt_any_fn19)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn22)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
typedef int64_t (*rt_any_fn25)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn28)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn31)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t);
typedef int64_t (*rt_any_fn34)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn37)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t);
typedef int64_t (*rt_any_fn40)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn43)(int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t, int64_t, int64_t,
                               int64_t, int64_t, int64_t);
typedef int64_t (*rt_any_fn46)(
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
    int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

int64_t rt_call_any5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi, (int64_t[]){a0, a1, a2, a3, a4},
                                        5);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn16)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn15)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[5] = {a0, a1, a2, a3, a4};
      for (size_t i = 0; i < 5; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 5);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))(
          uintptr_t)f)(rt_dynamic_callback_normalize_arg(a0, true),
                       rt_dynamic_callback_normalize_arg(a1, true),
                       rt_dynamic_callback_normalize_arg(a2, true),
                       rt_dynamic_callback_normalize_arg(a3, true),
                       rt_dynamic_callback_normalize_arg(a4, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn15)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn15)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn15)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4));
}

int64_t rt_call_any6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi,
                                        (int64_t[]){a0, a1, a2, a3, a4, a5}, 6);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn19)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn18)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[6] = {a0, a1, a2, a3, a4, a5};
      for (size_t i = 0; i < 6; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 6);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return (
          (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))(
              uintptr_t)f)(rt_dynamic_callback_normalize_arg(a0, true),
                           rt_dynamic_callback_normalize_arg(a1, true),
                           rt_dynamic_callback_normalize_arg(a2, true),
                           rt_dynamic_callback_normalize_arg(a3, true),
                           rt_dynamic_callback_normalize_arg(a4, true),
                           rt_dynamic_callback_normalize_arg(a5, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn18)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn18)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn18)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5));
}

int64_t rt_call_any7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5, int64_t a6) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6}, 7);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5, a6};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn22)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn21)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[7] = {a0, a1, a2, a3, a4, a5, a6};
      for (size_t i = 0; i < 7; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 7);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn21)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn21)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn21)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6));
}

int64_t rt_call_any8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5, int64_t a6, int64_t a7) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7}, 8);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5, a6, a7};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn25)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn24)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
      for (size_t i = 0; i < 8; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 8);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn24)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn24)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn24)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7));
}

int64_t rt_call_any9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                     int64_t a8) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8}, 9);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5, a6, a7, a8};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn28)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn27)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[9] = {a0, a1, a2, a3, a4, a5, a6, a7, a8};
      for (size_t i = 0; i < 9; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 9);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn27)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn27)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn27)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8));
}

int64_t rt_call_any10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9}, 10);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn31)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn30)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[10] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9};
      for (size_t i = 0; i < 10; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 10);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true),
          rt_dynamic_callback_normalize_arg(a9, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn30)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn30)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn30)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9));
}

int64_t rt_call_any11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9, int64_t a10) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10}, 11);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  int64_t l10 = rt_any_len(a10), t10 = rt_any_tag(a10);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn34)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn33)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
        rt_any_to_i64(a10), l10, t10));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[11] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10};
      for (size_t i = 0; i < 11; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 11);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t))(
          uintptr_t)f)(rt_dynamic_callback_normalize_arg(a0, true),
                       rt_dynamic_callback_normalize_arg(a1, true),
                       rt_dynamic_callback_normalize_arg(a2, true),
                       rt_dynamic_callback_normalize_arg(a3, true),
                       rt_dynamic_callback_normalize_arg(a4, true),
                       rt_dynamic_callback_normalize_arg(a5, true),
                       rt_dynamic_callback_normalize_arg(a6, true),
                       rt_dynamic_callback_normalize_arg(a7, true),
                       rt_dynamic_callback_normalize_arg(a8, true),
                       rt_dynamic_callback_normalize_arg(a9, true),
                       rt_dynamic_callback_normalize_arg(a10, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn33)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn33)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9,
            rt_dynamic_callback_normalize_arg(a10, raw_scalar_callback_args),
            l10, t10),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn33)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
      rt_any_to_i64(a10), l10, t10));
}

int64_t rt_call_any12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9, int64_t a10, int64_t a11) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11}, 12);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  int64_t l10 = rt_any_len(a10), t10 = rt_any_tag(a10);
  int64_t l11 = rt_any_len(a11), t11 = rt_any_tag(a11);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3,  a4, a5,
                           a6,  a7, a8, a9, a10, a11};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn37)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn36)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
        rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[12] = {a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11};
      for (size_t i = 0; i < 12; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 12);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true),
          rt_dynamic_callback_normalize_arg(a9, true),
          rt_dynamic_callback_normalize_arg(a10, true),
          rt_dynamic_callback_normalize_arg(a11, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn36)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn36)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9,
            rt_dynamic_callback_normalize_arg(a10, raw_scalar_callback_args),
            l10, t10,
            rt_dynamic_callback_normalize_arg(a11, raw_scalar_callback_args),
            l11, t11),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn36)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
      rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11));
}

int64_t rt_call_any13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                      int64_t a12) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi, (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12},
        13);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  int64_t l10 = rt_any_len(a10), t10 = rt_any_tag(a10);
  int64_t l11 = rt_any_len(a11), t11 = rt_any_tag(a11);
  int64_t l12 = rt_any_len(a12), t12 = rt_any_tag(a12);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2, a3,  a4,  a5,
                           a6,  a7, a8, a9, a10, a11, a12};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn40)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn39)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
        rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
        rt_any_to_i64(a12), l12, t12));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[13] = {a0, a1, a2, a3,  a4,  a5, a6,
                          a7, a8, a9, a10, a11, a12};
      for (size_t i = 0; i < 13; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 13);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true),
          rt_dynamic_callback_normalize_arg(a9, true),
          rt_dynamic_callback_normalize_arg(a10, true),
          rt_dynamic_callback_normalize_arg(a11, true),
          rt_dynamic_callback_normalize_arg(a12, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn39)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn39)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9,
            rt_dynamic_callback_normalize_arg(a10, raw_scalar_callback_args),
            l10, t10,
            rt_dynamic_callback_normalize_arg(a11, raw_scalar_callback_args),
            l11, t11,
            rt_dynamic_callback_normalize_arg(a12, raw_scalar_callback_args),
            l12, t12),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn39)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
      rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
      rt_any_to_i64(a12), l12, t12));
}

int64_t rt_call_any14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                      int64_t a12, int64_t a13) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(
        abi,
        (int64_t[]){a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13},
        14);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callback_prepare(&f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  int64_t l10 = rt_any_len(a10), t10 = rt_any_tag(a10);
  int64_t l11 = rt_any_len(a11), t11 = rt_any_tag(a11);
  int64_t l12 = rt_any_len(a12), t12 = rt_any_tag(a12);
  int64_t l13 = rt_any_len(a13), t13 = rt_any_tag(a13);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2,  a3,  a4,  a5, a6,
                           a7,  a8, a9, a10, a11, a12, a13};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn43)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12,
              rt_dynamic_callback_normalize_arg(a13, false), l13, t13),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn42)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
        rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
        rt_any_to_i64(a12), l12, t12, rt_any_to_i64(a13), l13, t13));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[14] = {a0, a1, a2, a3,  a4,  a5,  a6,
                          a7, a8, a9, a10, a11, a12, a13};
      for (size_t i = 0; i < 14; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 14);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true),
          rt_dynamic_callback_normalize_arg(a9, true),
          rt_dynamic_callback_normalize_arg(a10, true),
          rt_dynamic_callback_normalize_arg(a11, true),
          rt_dynamic_callback_normalize_arg(a12, true),
          rt_dynamic_callback_normalize_arg(a13, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn42)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12,
              rt_dynamic_callback_normalize_arg(a13, false), l13, t13),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn42)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9,
            rt_dynamic_callback_normalize_arg(a10, raw_scalar_callback_args),
            l10, t10,
            rt_dynamic_callback_normalize_arg(a11, raw_scalar_callback_args),
            l11, t11,
            rt_dynamic_callback_normalize_arg(a12, raw_scalar_callback_args),
            l12, t12,
            rt_dynamic_callback_normalize_arg(a13, raw_scalar_callback_args),
            l13, t13),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn42)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
      rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
      rt_any_to_i64(a12), l12, t12, rt_any_to_i64(a13), l13, t13));
}

int64_t rt_call_any15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                      int64_t a12, int64_t a13, int64_t a14) {
  if (!f || (uintptr_t)f < 0x1000)
    return 0;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  if (abi.f64_result)
    return rt_dynamic_callback_call_f64(abi,
                                        (int64_t[]){a0, a1, a2, a3, a4, a5, a6,
                                                    a7, a8, a9, a10, a11, a12,
                                                    a13, a14},
                                        15);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool tagged_args_callable = abi.tagged_args;
  bool raw_result_callable = abi.raw_result;
  bool raw_scalar_callback_args = !tagged_args_callable;
  f = abi.fn;
  bool is_closure = rt_dynamic_callable_is_closure(f, NULL);
  if (!is_closure)
    f = rt_prepare_raw_callable(f);
  int64_t l0 = rt_any_len(a0), t0 = rt_any_tag(a0);
  int64_t l1 = rt_any_len(a1), t1 = rt_any_tag(a1);
  int64_t l2 = rt_any_len(a2), t2 = rt_any_tag(a2);
  int64_t l3 = rt_any_len(a3), t3 = rt_any_tag(a3);
  int64_t l4 = rt_any_len(a4), t4 = rt_any_tag(a4);
  int64_t l5 = rt_any_len(a5), t5 = rt_any_tag(a5);
  int64_t l6 = rt_any_len(a6), t6 = rt_any_tag(a6);
  int64_t l7 = rt_any_len(a7), t7 = rt_any_tag(a7);
  int64_t l8 = rt_any_len(a8), t8 = rt_any_tag(a8);
  int64_t l9 = rt_any_len(a9), t9 = rt_any_tag(a9);
  int64_t l10 = rt_any_len(a10), t10 = rt_any_tag(a10);
  int64_t l11 = rt_any_len(a11), t11 = rt_any_tag(a11);
  int64_t l12 = rt_any_len(a12), t12 = rt_any_tag(a12);
  int64_t l13 = rt_any_len(a13), t13 = rt_any_tag(a13);
  int64_t l14 = rt_any_len(a14), t14 = rt_any_tag(a14);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      if (abi.word_args) {
        int64_t words[] = {env, a0, a1, a2,  a3,  a4,  a5,  a6,
                           a7,  a8, a9, a10, a11, a12, a13, a14};
        for (size_t i = 1; i < sizeof(words) / sizeof(words[0]); ++i)
          words[i] =
              rt_dynamic_callback_normalize_arg(words[i], !abi.tagged_args);
        int64_t result = rt_dynamic_callback_call_words(
            code, words, sizeof(words) / sizeof(words[0]));
        return rt_dynamic_callback_normalize_result(
            result, dynamic_bool_callable, false, raw_result_callable);
      }
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn46)code)(
              env, rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12,
              rt_dynamic_callback_normalize_arg(a13, false), l13, t13,
              rt_dynamic_callback_normalize_arg(a14, false), l14, t14),
          dynamic_bool_callable, false, raw_result_callable);
    }
  }
  if (NY_NATIVE_IS(f) && !dynamic_callable)
    return rt_any_callback_result(((rt_any_fn45)NY_NATIVE_DECODE(f))(
        rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
        l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
        rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
        l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
        rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
        rt_any_to_i64(a12), l12, t12, rt_any_to_i64(a13), l13, t13,
        rt_any_to_i64(a14), l14, t14));
  if (dynamic_callable) {
    if (abi.word_args && !tagged_args_callable) {
      int64_t args[15] = {a0, a1, a2,  a3,  a4,  a5,  a6, a7,
                          a8, a9, a10, a11, a12, a13, a14};
      for (size_t i = 0; i < 15; ++i)
        args[i] = rt_dynamic_callback_normalize_arg(args[i],
                                                    raw_scalar_callback_args);
      int64_t result = rt_dynamic_callback_call_words(f, args, 15);
      return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                  false, raw_result_callable);
    }
    /*
     * See rt_call_any3: plain function values keep the native raw shape.
     */
    if (!is_closure && !tagged_args_callable)
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t))(uintptr_t)f)(
          rt_dynamic_callback_normalize_arg(a0, true),
          rt_dynamic_callback_normalize_arg(a1, true),
          rt_dynamic_callback_normalize_arg(a2, true),
          rt_dynamic_callback_normalize_arg(a3, true),
          rt_dynamic_callback_normalize_arg(a4, true),
          rt_dynamic_callback_normalize_arg(a5, true),
          rt_dynamic_callback_normalize_arg(a6, true),
          rt_dynamic_callback_normalize_arg(a7, true),
          rt_dynamic_callback_normalize_arg(a8, true),
          rt_dynamic_callback_normalize_arg(a9, true),
          rt_dynamic_callback_normalize_arg(a10, true),
          rt_dynamic_callback_normalize_arg(a11, true),
          rt_dynamic_callback_normalize_arg(a12, true),
          rt_dynamic_callback_normalize_arg(a13, true),
          rt_dynamic_callback_normalize_arg(a14, true));
    if (!is_closure)
      return rt_dynamic_callback_normalize_result(
          ((rt_any_fn45)(uintptr_t)f)(
              rt_dynamic_callback_normalize_arg(a0, false), l0, t0,
              rt_dynamic_callback_normalize_arg(a1, false), l1, t1,
              rt_dynamic_callback_normalize_arg(a2, false), l2, t2,
              rt_dynamic_callback_normalize_arg(a3, false), l3, t3,
              rt_dynamic_callback_normalize_arg(a4, false), l4, t4,
              rt_dynamic_callback_normalize_arg(a5, false), l5, t5,
              rt_dynamic_callback_normalize_arg(a6, false), l6, t6,
              rt_dynamic_callback_normalize_arg(a7, false), l7, t7,
              rt_dynamic_callback_normalize_arg(a8, false), l8, t8,
              rt_dynamic_callback_normalize_arg(a9, false), l9, t9,
              rt_dynamic_callback_normalize_arg(a10, false), l10, t10,
              rt_dynamic_callback_normalize_arg(a11, false), l11, t11,
              rt_dynamic_callback_normalize_arg(a12, false), l12, t12,
              rt_dynamic_callback_normalize_arg(a13, false), l13, t13,
              rt_dynamic_callback_normalize_arg(a14, false), l14, t14),
          dynamic_bool_callable, false, raw_result_callable);
    return rt_dynamic_callback_normalize_result(
        ((rt_any_fn45)(uintptr_t)f)(
            rt_dynamic_callback_normalize_arg(a0, raw_scalar_callback_args), l0,
            t0, rt_dynamic_callback_normalize_arg(a1, raw_scalar_callback_args),
            l1, t1,
            rt_dynamic_callback_normalize_arg(a2, raw_scalar_callback_args), l2,
            t2, rt_dynamic_callback_normalize_arg(a3, raw_scalar_callback_args),
            l3, t3,
            rt_dynamic_callback_normalize_arg(a4, raw_scalar_callback_args), l4,
            t4, rt_dynamic_callback_normalize_arg(a5, raw_scalar_callback_args),
            l5, t5,
            rt_dynamic_callback_normalize_arg(a6, raw_scalar_callback_args), l6,
            t6, rt_dynamic_callback_normalize_arg(a7, raw_scalar_callback_args),
            l7, t7,
            rt_dynamic_callback_normalize_arg(a8, raw_scalar_callback_args), l8,
            t8, rt_dynamic_callback_normalize_arg(a9, raw_scalar_callback_args),
            l9, t9,
            rt_dynamic_callback_normalize_arg(a10, raw_scalar_callback_args),
            l10, t10,
            rt_dynamic_callback_normalize_arg(a11, raw_scalar_callback_args),
            l11, t11,
            rt_dynamic_callback_normalize_arg(a12, raw_scalar_callback_args),
            l12, t12,
            rt_dynamic_callback_normalize_arg(a13, raw_scalar_callback_args),
            l13, t13,
            rt_dynamic_callback_normalize_arg(a14, raw_scalar_callback_args),
            l14, t14),
        dynamic_bool_callable, false, raw_result_callable);
  }
  return rt_any_callback_result(((rt_any_fn45)(uintptr_t)f)(
      rt_any_to_i64(a0), l0, t0, rt_any_to_i64(a1), l1, t1, rt_any_to_i64(a2),
      l2, t2, rt_any_to_i64(a3), l3, t3, rt_any_to_i64(a4), l4, t4,
      rt_any_to_i64(a5), l5, t5, rt_any_to_i64(a6), l6, t6, rt_any_to_i64(a7),
      l7, t7, rt_any_to_i64(a8), l8, t8, rt_any_to_i64(a9), l9, t9,
      rt_any_to_i64(a10), l10, t10, rt_any_to_i64(a11), l11, t11,
      rt_any_to_i64(a12), l12, t12, rt_any_to_i64(a13), l13, t13,
      rt_any_to_i64(a14), l14, t14));
}

int64_t rt_call0(int64_t f) {
  uintptr_t code_hint = (uintptr_t)__builtin_return_address(0);
  f = rt_ffi_normalize_callable(f);
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;

  /*
   * Zero-argument callbacks use the same marked callable encoding as
   * rt_call_any1/2.  Decode it before any raw-pointer/native-tag heuristic:
   * otherwise a marked lambda is invoked at the encoded high-bit address,
   * making thunk() correctness depend on generated function alignment.
   */
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  bool dynamic_callable = abi.dynamic;
  bool dynamic_bool_callable = abi.returns_bool;
  bool raw_result_callable = abi.raw_result;
  f = abi.fn;

  bool is_closure = rt_dynamic_callable_is_closure(f, NULL);
  if (!is_closure && NY_NATIVE_IS(f) && !dynamic_callable &&
      !rt_addr_readable_safe((uintptr_t)f, 1))
    return rt_tag_v(NY_NATIVE_RET0(NY_NATIVE_DECODE(f)));
  if (!is_closure)
    f = rt_prepare_raw_callable_with_hint(f, code_hint);
  if (is_closure) {
    intptr_t base = (intptr_t)rt_untag_v(f);
    int64_t code =
        rt_prepare_raw_callable_with_hint(*(int64_t *)base, code_hint);
    int64_t env = *(int64_t *)(base + 8);
    if (abi.f64_result) {
      double f64 = ((double (*)(int64_t))code)(env);
      return rt_flt_box_double(f64);
    }
    int64_t result = ((int64_t (*)(int64_t))code)(env);
    return rt_dynamic_callback_normalize_result(result, dynamic_bool_callable,
                                                false, raw_result_callable);
  }
  if (abi.f64_result) {
    double f64 = ((double (*)(void))f)();
    return rt_flt_box_double(f64);
  }
  int64_t result = ((int64_t (*)(void))f)();
  return dynamic_callable
             ? rt_dynamic_callback_normalize_result(
                   result, dynamic_bool_callable, false, raw_result_callable)
             : result;
}

/*
 * Public core.os.ffi entry points must accept C pointers only. Keep the
 * callback-capable rt_call0 dispatcher for internal runtime users, and put
 * the stricter policy at the FFI boundary.
 */
int64_t rt_ffi_call0(int64_t f) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  return rt_call0(f);
}

static const rt_ffi_callback_abi_decl_t rt_ffi_callback_abi_decls[] = {

    {.symbol = "rt_call0_i32",
     .source_arity = 0,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call0_ptr",
     .source_arity = 0,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call0_void",
     .source_arity = 0,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call10_void",
     .source_arity = 10,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr =
         {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call1_f32_void",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_F32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call1_i64",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_I64},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_I64,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call1_ptr",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call1_u32",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_U32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_U32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call1_u32_void",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_U32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call1_void",
     .source_arity = 1,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call2_f32_void",
     .source_arity = 2,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_F32, RT_FFI_CALLBACK_REPR_F32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call2_ptr",
     .source_arity = 2,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call2_ptr_u32",
     .source_arity = 2,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call2_void",
     .source_arity = 2,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call3_f32_void",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_F32, RT_FFI_CALLBACK_REPR_F32,
                  RT_FFI_CALLBACK_REPR_F32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call3_ptr",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call3_ptr_ptr_u32",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call3_ptr_u32_ptr",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U32,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call3_ptr_u64_ptr",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U64,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call3_ptr_u64_ptr_i32",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U64,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call3_void",
     .source_arity = 3,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_f32_void",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_F32, RT_FFI_CALLBACK_REPR_F32,
                  RT_FFI_CALLBACK_REPR_F32, RT_FFI_CALLBACK_REPR_F32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_ptr",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call4_ptr_ptr_ptr_ptr_i32",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_ptr_ptr_ptr_ptr_void",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_ptr_ptr_ptr_u64_i32",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U64},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_ptr_u32_u64_ptr_i32",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U32,
                  RT_FFI_CALLBACK_REPR_U64, RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_ptr_u64_ptr_ptr_i32",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U64,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call4_void",
     .source_arity = 4,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call5_ptr",
     .source_arity = 5,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED},
    {.symbol = "rt_call5_ptr_ptr_ptr_u64_i32_i32",
     .source_arity = 5,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER,
                  RT_FFI_CALLBACK_REPR_RAW_POINTER, RT_FFI_CALLBACK_REPR_U64,
                  RT_FFI_CALLBACK_REPR_I32},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE,
                       RT_FFI_CALLBACK_OWNERSHIP_NONE},
     .result_repr = RT_FFI_CALLBACK_REPR_I32,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call5_void",
     .source_arity = 5,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr = {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
                  RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call6_void",
     .source_arity = 6,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr =
         {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call7_void",
     .source_arity = 7,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr =
         {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call8_void",
     .source_arity = 8,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr =
         {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},
    {.symbol = "rt_call9_void",
     .source_arity = 9,
     .callable_repr = RT_FFI_CALLBACK_REPR_RAW_POINTER,
     .arg_repr =
         {RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD, RT_FFI_CALLBACK_REPR_DYNAMIC_WORD,
          RT_FFI_CALLBACK_REPR_DYNAMIC_WORD},
     .arg_ownership = {RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
                       RT_FFI_CALLBACK_OWNERSHIP_BORROWED},
     .result_repr = RT_FFI_CALLBACK_REPR_VOID,
     .callable_ownership = RT_FFI_CALLBACK_OWNERSHIP_BORROWED,
     .result_ownership = RT_FFI_CALLBACK_OWNERSHIP_NONE},

};

const rt_ffi_callback_abi_decl_t *rt_ffi_callback_abi_decl(const char *symbol) {
  if (!symbol)
    return NULL;
  size_t lo = 0;
  size_t hi =
      sizeof(rt_ffi_callback_abi_decls) / sizeof(rt_ffi_callback_abi_decls[0]);
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int order = strcmp(symbol, rt_ffi_callback_abi_decls[mid].symbol);
    if (order == 0)
      return &rt_ffi_callback_abi_decls[mid];
    if (order < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return NULL;
}

static int64_t rt_ffi_callback_arg(const rt_ffi_callback_abi_decl_t *decl,
                                   size_t index, int64_t value) {
  if (!decl || index >= decl->source_arity)
    return 0;
  switch ((rt_ffi_callback_repr_t)decl->arg_repr[index]) {
  case RT_FFI_CALLBACK_REPR_RAW_POINTER: {
    uintptr_t arg = (uintptr_t)value;
    bool readable = rt_addr_readable_safe(arg, 1);
    if (!readable && NY_NATIVE_IS(value))
      arg = (uintptr_t)NY_NATIVE_DECODE(value);
    else if (!readable && is_int(value)) {
      int64_t unboxed = rt_untag_v(value);
      if (NY_NATIVE_IS(unboxed))
        arg = (uintptr_t)NY_NATIVE_DECODE(unboxed);
      else
        arg = (uintptr_t)unboxed;
    }
    return (int64_t)arg;
  }
  case RT_FFI_CALLBACK_REPR_RAW_I64:
  case RT_FFI_CALLBACK_REPR_I32:
  case RT_FFI_CALLBACK_REPR_U32:
  case RT_FFI_CALLBACK_REPR_U64:
    return rt_untag_v(value);
  case RT_FFI_CALLBACK_REPR_DYNAMIC_WORD: {
    return rt_ffi_native_word(value);
  }
  case RT_FFI_CALLBACK_REPR_F32:
    return rt_flt_unbox_val(value);
  case RT_FFI_CALLBACK_REPR_UNSPECIFIED:
  case RT_FFI_CALLBACK_REPR_VOID:
    return 0;
  }
  return 0;
}

static float rt_ffi_callback_f32_arg(const rt_ffi_callback_abi_decl_t *decl,
                                     size_t index, int64_t value) {
  int64_t bits = rt_ffi_callback_arg(decl, index, value);
  double unboxed = 0.0;
  memcpy(&unboxed, &bits, sizeof(unboxed));
  return (float)unboxed;
}

static int64_t rt_ffi_callback_result(const rt_ffi_callback_abi_decl_t *decl,
                                      int64_t value) {
  if (!decl)
    return 0;
  switch ((rt_ffi_callback_repr_t)decl->result_repr) {
  case RT_FFI_CALLBACK_REPR_RAW_POINTER:
    return value ? NY_NATIVE_ENCODE((void *)(uintptr_t)value) : 0;
  case RT_FFI_CALLBACK_REPR_RAW_I64:
  case RT_FFI_CALLBACK_REPR_I32:
  case RT_FFI_CALLBACK_REPR_U32:
  case RT_FFI_CALLBACK_REPR_U64:
    return rt_tag_v(value);
  case RT_FFI_CALLBACK_REPR_DYNAMIC_WORD:
    return value;
  case RT_FFI_CALLBACK_REPR_UNSPECIFIED:
  case RT_FFI_CALLBACK_REPR_F32:
  case RT_FFI_CALLBACK_REPR_VOID:
    return 1;
  }
  return 0;
}

int64_t rt_call0_ptr(int64_t f) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 0 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void))NY_NATIVE_DECODE(f))();
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call0_i32(int64_t f) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 0 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void))NY_NATIVE_DECODE(f))();
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  return rt_call0(f);
}

int64_t rt_call1(int64_t f, int64_t a0) {
  f = rt_ffi_normalize_callable(f);
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  rt_dynamic_callable_abi_t abi = rt_dynamic_callable_abi(f);
  f = abi.fn;
  if (NY_NATIVE_IS(f) && !abi.dynamic) {
    int64_t v0 = rt_ffi_native_word(a0);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    int64_t res_raw = NY_NATIVE_RET1(NY_NATIVE_DECODE(f), v0);
    return rt_tag_v(res_raw);
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      int64_t raw = ((int64_t (*)(int64_t, int64_t))code)(env, a0);
      return rt_dynamic_callback_normalize_result(raw, abi.returns_bool, false,
                                                  abi.raw_result);
    }
  }
  int64_t arg = abi.dynamic && !abi.tagged_args
                    ? rt_dynamic_callback_normalize_arg(a0, true)
                    : a0;
  int64_t result = ((int64_t (*)(int64_t))f)(arg);
  return rt_dynamic_callback_normalize_result(result, abi.returns_bool, false,
                                              abi.raw_result);
}

int64_t rt_ffi_call1(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  return rt_call1(f, a0);
}

int64_t rt_call1_ptr(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    uintptr_t arg = (uintptr_t)rt_ffi_callback_arg(abi, 0, a0);
    void *res = ((void *(*)(void *))NY_NATIVE_DECODE(f))((void *)arg);
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call1_i64(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_I64 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_I64)
    return 1;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
#if UINTPTR_MAX == 0xffffffff
    long long arg = (long long)rt_ffi_callback_arg(abi, 0, a0);
    long long res = ((long long (*)(long long))NY_NATIVE_DECODE(f))(arg);
    return rt_ffi_callback_result(abi, (int64_t)res);
#else
    int64_t res_raw = ((int64_t (*)(int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0));
    return rt_ffi_callback_result(abi, res_raw);
#endif
  }
  return rt_call1(f, a0);
}

int64_t rt_call1_u32(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_U32)
    return 1;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    uint32_t arg = (uint32_t)rt_ffi_callback_arg(abi, 0, a0);
    uint32_t res = ((uint32_t (*)(uint32_t))NY_NATIVE_DECODE(f))(arg);
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  return rt_call1(f, a0);
}

int64_t rt_call2(int64_t f, int64_t a0, int64_t a1) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_ffi_native_word(a0);
    int64_t v1 = rt_ffi_native_word(a1);

    if (is_heap_ptr(v0))
      v0 = (int64_t)(uintptr_t)rt_untag_v(v0);
    if (is_heap_ptr(v1))
      v1 = (int64_t)(uintptr_t)rt_untag_v(v1);
    return rt_tag_v(NY_NATIVE_RET2(NY_NATIVE_DECODE(f), v0, v1));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      int64_t raw = ((int64_t (*)(int64_t, int64_t, int64_t))code)(env, a0, a1);
      return rt_dynamic_callback_normalize_result(raw, false, false, false);
    }
  }
  return ((int64_t (*)(int64_t, int64_t))f)(a0, a1);
}

int64_t rt_call2_ptr(int64_t f, int64_t a0, int64_t a1) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 2 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call2_ptr_u32(int64_t f, int64_t a0, int64_t a1) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 2 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (uint32_t)rt_ffi_callback_arg(abi, 1, a1));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call3(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_ffi_native_word(a0);
    int64_t v1 = rt_ffi_native_word(a1);
    int64_t v2 = rt_ffi_native_word(a2);

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
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))code)(env, a0,
                                                                     a1, a2);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t))f)(a0, a1, a2);
}

int64_t rt_call3_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call3_ptr_u64_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint64_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (uint64_t)rt_ffi_callback_arg(abi, 1, a1),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call3_ptr_u32_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, uint32_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (uint32_t)rt_ffi_callback_arg(abi, 1, a1),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call3_ptr_ptr_u32(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  if (NY_NATIVE_IS(f)) {
    void *res = ((void *(*)(void *, void *, uint32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
        (uint32_t)rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
  }
  return 0;
}

int64_t rt_call4(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int64_t v0 = rt_ffi_native_word(a0);
    int64_t v1 = rt_ffi_native_word(a1);
    int64_t v2 = rt_ffi_native_word(a2);
    int64_t v3 = rt_ffi_native_word(a3);

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
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))f)(a0, a1, a2, a3);
}

int64_t rt_call4_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2,
                     int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->callable_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->callable_ownership != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_ownership != RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_ownership[0] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[1] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[2] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[3] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  void *fn = NY_NATIVE_IS(f) ? NY_NATIVE_DECODE(f) : (void *)(uintptr_t)f;
  void *res = ((void *(*)(void *, void *, void *, void *))fn)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
  return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
}

int64_t rt_call3_ptr_u64_ptr_i32(int64_t f, int64_t a0, int64_t a1,
                                 int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, uint64_t, void *))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (uint64_t)rt_ffi_callback_arg(abi, 1, a1),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint64_t, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (uint64_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call4_ptr_ptr_ptr_ptr_i32(int64_t f, int64_t a0, int64_t a1,
                                     int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res =
        ((int32_t (*)(void *, void *, void *, void *))NY_NATIVE_DECODE(f))(
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call4_ptr_u32_u64_ptr_i32(int64_t f, int64_t a0, int64_t a1,
                                     int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res =
        ((int32_t (*)(void *, uint32_t, uint64_t, void *))NY_NATIVE_DECODE(f))(
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
            (uint32_t)rt_ffi_callback_arg(abi, 1, a1),
            (uint64_t)rt_ffi_callback_arg(abi, 2, a2),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint32_t, uint64_t, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (uint32_t)rt_ffi_callback_arg(abi, 1, a1),
      (uint64_t)rt_ffi_callback_arg(abi, 2, a2),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call4_ptr_u64_ptr_ptr_i32(int64_t f, int64_t a0, int64_t a1,
                                     int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res =
        ((int32_t (*)(void *, uint64_t, void *, void *))NY_NATIVE_DECODE(f))(
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
            (uint64_t)rt_ffi_callback_arg(abi, 1, a1),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, uint64_t, void *, void *))(uintptr_t)f)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (uint64_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call4_ptr_ptr_ptr_u64_i32(int64_t f, int64_t a0, int64_t a1,
                                     int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res =
        ((int32_t (*)(void *, void *, void *, uint64_t))NY_NATIVE_DECODE(f))(
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
            (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
            (uint64_t)rt_ffi_callback_arg(abi, 3, a3));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t))(uintptr_t)f)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
      (uint64_t)rt_ffi_callback_arg(abi, 3, a3));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call5_ptr_ptr_ptr_u64_i32_i32(int64_t f, int64_t a0, int64_t a1,
                                         int64_t a2, int64_t a3, int64_t a4) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 5 ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_U64 ||
      abi->arg_repr[4] != RT_FFI_CALLBACK_REPR_I32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_I32)
    return 1;
  if (!f)
    return 1;
  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t,
                                int32_t))NY_NATIVE_DECODE(f))(
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
        (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
        (uint64_t)rt_ffi_callback_arg(abi, 3, a3),
        (int32_t)rt_ffi_callback_arg(abi, 4, a4));
    return rt_ffi_callback_result(abi, (int64_t)res);
  }
  int32_t res = ((int32_t (*)(void *, void *, void *, uint64_t, int32_t))(
      uintptr_t)f)((void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
                   (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
                   (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
                   (uint64_t)rt_ffi_callback_arg(abi, 3, a3),
                   (int32_t)rt_ffi_callback_arg(abi, 4, a4));
  return rt_ffi_callback_result(abi, (int64_t)res);
}

int64_t rt_call5(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(
        NY_NATIVE_RET5(NY_NATIVE_DECODE(f), rt_ffi_native_word(a0),
                       rt_ffi_native_word(a1), rt_ffi_native_word(a2),
                       rt_ffi_native_word(a3), rt_ffi_native_word(a4)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4);
}

int64_t rt_call5_ptr(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                     int64_t a4) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 5 ||
      abi->callable_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->callable_ownership != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->result_ownership != RT_FFI_CALLBACK_OWNERSHIP_CALLER_DEFINED ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_repr[4] != RT_FFI_CALLBACK_REPR_RAW_POINTER ||
      abi->arg_ownership[0] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[1] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[2] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[3] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED ||
      abi->arg_ownership[4] != RT_FFI_CALLBACK_OWNERSHIP_BORROWED)
    return 0;
  if (!f)
    return 0;
  if ((uintptr_t)(f) < 0x1000)
    return 0;
  void *fn = NY_NATIVE_IS(f) ? NY_NATIVE_DECODE(f) : (void *)(uintptr_t)f;
  void *res = ((void *(*)(void *, void *, void *, void *, void *))fn)(
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3),
      (void *)(uintptr_t)rt_ffi_callback_arg(abi, 4, a4));
  return rt_ffi_callback_result(abi, (int64_t)(uintptr_t)res);
}

int64_t rt_call6(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(NY_NATIVE_RET6(
        NY_NATIVE_DECODE(f), rt_ffi_native_word(a0), rt_ffi_native_word(a1),
        rt_ffi_native_word(a2), rt_ffi_native_word(a3), rt_ffi_native_word(a4),
        rt_ffi_native_word(a5)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5);
}

int64_t rt_call7(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t))code)(env, a0, a1, a2, a3, a4, a5,
                                                   a6);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))f)(a0, a1, a2, a3, a4, a5, a6);
}

int64_t rt_call8(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v((
        (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t))code)(env, a0, a1, a2, a3,
                                                            a4, a5, a6, a7);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7);
}

int64_t rt_call9(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                 int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v((
        (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7),
        rt_ffi_native_word(a8)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6,
                                                     a7, a8);
}

int64_t rt_call10(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v((
        (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
}

int64_t rt_call11(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9), rt_ffi_native_word(a10)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7,
                                          a8, a9, a10);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
}

int64_t rt_call12(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t, int64_t, int64_t, int64_t,
                                  int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9), rt_ffi_native_word(a10),
        rt_ffi_native_word(a11)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t))code)(env, a0, a1, a2, a3, a4, a5, a6, a7,
                                          a8, a9, a10, a11);
    }
  }
  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
}

int64_t rt_call13(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v(((int64_t (*)(
        int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
        int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9), rt_ffi_native_word(a10),
        rt_ffi_native_word(a11), rt_ffi_native_word(a12)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10,
                                   a11, a12);
}

int64_t rt_call14(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                  int64_t a13) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v((
        (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9), rt_ffi_native_word(a10),
        rt_ffi_native_word(a11), rt_ffi_native_word(a12),
        rt_ffi_native_word(a13)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t))f)(a0, a1, a2, a3, a4, a5, a6, a7, a8,
                                            a9, a10, a11, a12, a13);
}

int64_t rt_call15(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                  int64_t a4, int64_t a5, int64_t a6, int64_t a7, int64_t a8,
                  int64_t a9, int64_t a10, int64_t a11, int64_t a12,
                  int64_t a13, int64_t a14) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  if (!f)
    return 1;

  if ((uintptr_t)(f) < 0x1000)
    return 1;
  if (NY_NATIVE_IS(f)) {
    return rt_tag_v((
        (int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                     int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_native_word(a0), rt_ffi_native_word(a1), rt_ffi_native_word(a2),
        rt_ffi_native_word(a3), rt_ffi_native_word(a4), rt_ffi_native_word(a5),
        rt_ffi_native_word(a6), rt_ffi_native_word(a7), rt_ffi_native_word(a8),
        rt_ffi_native_word(a9), rt_ffi_native_word(a10),
        rt_ffi_native_word(a11), rt_ffi_native_word(a12),
        rt_ffi_native_word(a13), rt_ffi_native_word(a14)));
  }
  f = rt_prepare_raw_callable(f);
  if (is_heap_ptr(f)) {
    int64_t code, env;
    if (rt_ffi_closure_split(f, &code, &env)) {
      return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                           int64_t, int64_t, int64_t, int64_t))code)(
          env, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
  }

  return ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
                       int64_t, int64_t, int64_t))f)(
      a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
}

int64_t rt_call0_void(int64_t f) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 0 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(void))NY_NATIVE_DECODE(f))();
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call0(f);
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call1_void(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_DYNAMIC_WORD ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t))NY_NATIVE_DECODE(f))(rt_ffi_callback_arg(abi, 0, a0));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call1(f, a0);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call1_u32_void(int64_t f, int64_t a0) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_U32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f)
    return 1;
  if (NY_NATIVE_IS(f)) {
    uint32_t arg = (uint32_t)rt_ffi_callback_arg(abi, 0, a0);
    ((void (*)(uint32_t))NY_NATIVE_DECODE(f))(arg);
    return 1;
  }
  return rt_call1(f, a0);
}
int64_t rt_call2_void(int64_t f, int64_t a0, int64_t a1) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 2 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call2(f, a0, a1);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call3_void(int64_t f, int64_t a0, int64_t a1, int64_t a2) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call3(f, a0, a1, a2);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call4_void(int64_t f, int64_t a0, int64_t a1, int64_t a2,
                      int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call4(f, a0, a1, a2, a3);
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call4_ptr_ptr_ptr_ptr_void(int64_t f, int64_t a0, int64_t a1,
                                      int64_t a2, int64_t a3) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f)
    return rt_ffi_callback_result(abi, 0);
  if ((uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    void *p0 = (void *)(uintptr_t)rt_ffi_callback_arg(abi, 0, a0);
    void *p1 = (void *)(uintptr_t)rt_ffi_callback_arg(abi, 1, a1);
    void *p2 = (void *)(uintptr_t)rt_ffi_callback_arg(abi, 2, a2);
    void *p3 = (void *)(uintptr_t)rt_ffi_callback_arg(abi, 3, a3);
    ((void (*)(void *, void *, void *, void *))NY_NATIVE_DECODE(f))(p0, p1, p2,
                                                                    p3);
    return rt_ffi_callback_result(abi, 0);
  }
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call5_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 5 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t))NY_NATIVE_DECODE(
        f))(rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
            rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
            rt_ffi_callback_arg(abi, 4, a4));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call5(f, a0, a1, a2, a3, a4);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call6_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 6 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
        rt_ffi_callback_arg(abi, 4, a4), rt_ffi_callback_arg(abi, 5, a5));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call6(f, a0, a1, a2, a3, a4, a5);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call7_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 7 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
        rt_ffi_callback_arg(abi, 4, a4), rt_ffi_callback_arg(abi, 5, a5),
        rt_ffi_callback_arg(abi, 6, a6));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call7(f, a0, a1, a2, a3, a4, a5, a6);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call8_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 8 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
        rt_ffi_callback_arg(abi, 4, a4), rt_ffi_callback_arg(abi, 5, a5),
        rt_ffi_callback_arg(abi, 6, a6), rt_ffi_callback_arg(abi, 7, a7));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call8(f, a0, a1, a2, a3, a4, a5, a6, a7);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call9_void(int64_t f, int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                      int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                      int64_t a8) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 9 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
        rt_ffi_callback_arg(abi, 4, a4), rt_ffi_callback_arg(abi, 5, a5),
        rt_ffi_callback_arg(abi, 6, a6), rt_ffi_callback_arg(abi, 7, a7),
        rt_ffi_callback_arg(abi, 8, a8));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call9(f, a0, a1, a2, a3, a4, a5, a6, a7, a8);
  return rt_ffi_callback_result(abi, 0);
}
int64_t rt_call10_void(int64_t f, int64_t a0, int64_t a1, int64_t a2,
                       int64_t a3, int64_t a4, int64_t a5, int64_t a6,
                       int64_t a7, int64_t a8, int64_t a9) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 10 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  if (!f || (uintptr_t)(f) < 0x1000)
    return rt_ffi_callback_result(abi, 0);
  if (NY_NATIVE_IS(f)) {
    ((void (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t,
               int64_t, int64_t, int64_t))NY_NATIVE_DECODE(f))(
        rt_ffi_callback_arg(abi, 0, a0), rt_ffi_callback_arg(abi, 1, a1),
        rt_ffi_callback_arg(abi, 2, a2), rt_ffi_callback_arg(abi, 3, a3),
        rt_ffi_callback_arg(abi, 4, a4), rt_ffi_callback_arg(abi, 5, a5),
        rt_ffi_callback_arg(abi, 6, a6), rt_ffi_callback_arg(abi, 7, a7),
        rt_ffi_callback_arg(abi, 8, a8), rt_ffi_callback_arg(abi, 9, a9));
    return rt_ffi_callback_result(abi, 0);
  }
  rt_call10(f, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
  return rt_ffi_callback_result(abi, 0);
}

static inline void *rt_ffi_fn(int64_t f) {
  if (!f)
    return NULL;
  if ((uintptr_t)f < 0x1000)
    return NULL;
  return NY_NATIVE_IS(f) ? NY_NATIVE_DECODE(f) : (void *)(uintptr_t)f;
}

int64_t rt_call1_f32_void(int64_t f, int64_t a) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 1 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return rt_ffi_callback_result(abi, 0);
  ((void (*)(float))fn)(rt_ffi_callback_f32_arg(abi, 0, a));
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call2_f32_void(int64_t f, int64_t a, int64_t b) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 2 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return rt_ffi_callback_result(abi, 0);
  ((void (*)(float, float))fn)(rt_ffi_callback_f32_arg(abi, 0, a),
                               rt_ffi_callback_f32_arg(abi, 1, b));
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call3_f32_void(int64_t f, int64_t a, int64_t b, int64_t c) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 3 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return rt_ffi_callback_result(abi, 0);
  ((void (*)(float, float, float))fn)(rt_ffi_callback_f32_arg(abi, 0, a),
                                      rt_ffi_callback_f32_arg(abi, 1, b),
                                      rt_ffi_callback_f32_arg(abi, 2, c));
  return rt_ffi_callback_result(abi, 0);
}

int64_t rt_call4_f32_void(int64_t f, int64_t a, int64_t b, int64_t c,
                          int64_t d) {
  if (rt_ffi_require_c_fn(&f))
    return f;
  const rt_ffi_callback_abi_decl_t *abi = rt_ffi_callback_abi_decl(__func__);
  if (!abi || abi->source_arity != 4 ||
      abi->arg_repr[0] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[1] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[2] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->arg_repr[3] != RT_FFI_CALLBACK_REPR_F32 ||
      abi->result_repr != RT_FFI_CALLBACK_REPR_VOID)
    return 1;
  void *fn = rt_ffi_fn(f);
  if (!fn)
    return rt_ffi_callback_result(abi, 0);
  ((void (*)(float, float, float, float))fn)(
      rt_ffi_callback_f32_arg(abi, 0, a), rt_ffi_callback_f32_arg(abi, 1, b),
      rt_ffi_callback_f32_arg(abi, 2, c), rt_ffi_callback_f32_arg(abi, 3, d));
  return rt_ffi_callback_result(abi, 0);
}

#include <zlib.h>

/*
 * A direct call here is a strong undefined reference, and --as-needed keeps a
 * library that answers one even when --gc-sections drops the call site later.
 * That put libz.so.1 in DT_NEEDED for every AOT binary, so programs that never
 * compress paid a DSO load and its relocations at startup. Bind lazily
 * instead: the dependency follows first use, and a host without libz gets a
 * zlib error from the wrappers rather than a load-time one.
 */
typedef int (*rt_zlib_uncompress_fn)(Bytef *, uLongf *, const Bytef *, uLong);
typedef int (*rt_zlib_compress2_fn)(Bytef *, uLongf *, const Bytef *, uLong,
                                    int);
typedef uLong (*rt_zlib_bound_fn)(uLong);

static struct {
  rt_zlib_uncompress_fn uncompress;
  rt_zlib_compress2_fn compress2;
  rt_zlib_bound_fn compress_bound;
  int tried;
} g_rt_zlib;

static void rt_zlib_resolve(void) {
  if (g_rt_zlib.tried)
    return;
  g_rt_zlib.tried = 1;
#if defined(_WIN32)
  return;
#else
  void *h = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h)
    h = dlopen("libz.so", RTLD_NOW | RTLD_LOCAL);
#ifdef __APPLE__
  if (!h)
    h = dlopen("libz.dylib", RTLD_NOW | RTLD_LOCAL);
#endif
  if (!h)
    return;
  *(void **)&g_rt_zlib.uncompress = dlsym(h, "uncompress");
  *(void **)&g_rt_zlib.compress2 = dlsym(h, "compress2");
  *(void **)&g_rt_zlib.compress_bound = dlsym(h, "compressBound");
#endif
}

int64_t rt_zlib_uncompress(int64_t dest, int64_t destLen_p, int64_t src,
                           int64_t srcLen) {
  rt_zlib_resolve();
  if (!g_rt_zlib.uncompress)
    return rt_tag_v((int64_t)Z_STREAM_ERROR);
  uLongf *dlen = (uLongf *)(uintptr_t)rt_untag_v(destLen_p);
  int res = g_rt_zlib.uncompress((Bytef *)(uintptr_t)rt_untag_v(dest), dlen,
                                 (const Bytef *)(uintptr_t)rt_untag_v(src),
                                 (uLong)rt_untag_v(srcLen));
  return rt_tag_v((int64_t)res);
}

int64_t rt_zlib_compress(int64_t dest, int64_t destLen_p, int64_t src,
                         int64_t srcLen, int64_t level) {
  rt_zlib_resolve();
  if (!g_rt_zlib.compress2)
    return rt_tag_v((int64_t)Z_STREAM_ERROR);
  uLongf *dlen = (uLongf *)(uintptr_t)rt_untag_v(destLen_p);
  int res = g_rt_zlib.compress2((Bytef *)(uintptr_t)rt_untag_v(dest), dlen,
                                (const Bytef *)(uintptr_t)rt_untag_v(src),
                                (uLong)rt_untag_v(srcLen),
                                (int)rt_untag_v(level));
  return rt_tag_v((int64_t)res);
}

int64_t rt_zlib_compress_str(int64_t src, int64_t srcLen, int64_t level) {
  rt_zlib_resolve();
  if (!g_rt_zlib.compress_bound || !g_rt_zlib.compress2)
    return 0;
  const Bytef *src_p = (const Bytef *)(uintptr_t)rt_untag_v(src);
  uLong src_len = (uLong)rt_untag_v(srcLen);
  uLongf out_len = g_rt_zlib.compress_bound(src_len);
  int64_t out = rt_malloc((int64_t)(((uint64_t)out_len + 1u) << 1) | 1);
  if (!out)
    return 0;
  int res = g_rt_zlib.compress2((Bytef *)(uintptr_t)out, &out_len, src_p,
                                src_len, (int)rt_untag_v(level));
  if (res != Z_OK) {
    rt_free(out);
    return 0;
  }
  *(int64_t *)((char *)(uintptr_t)out - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)out - 16) = ((int64_t)out_len << 1) | 1;
  ((char *)(uintptr_t)out)[out_len] = '\0';
  return out;
}

int64_t rt_zlib_bound(int64_t n) {
  rt_zlib_resolve();
  if (!g_rt_zlib.compress_bound)
    return 0;
  return rt_tag_v((int64_t)g_rt_zlib.compress_bound((uLong)rt_untag_v(n)));
}
