/*
 * Runtime core: NyValue constructors, tagged-value ops, type checks,
 * comparison, hashing, printing, and the shared runtime dispatch table.
 */
#include "base/common.h"
#include "code/runtime/shared.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#endif

extern int64_t rt_lt(int64_t a, int64_t b);
extern int64_t rt_bigint_add(int64_t a, int64_t b);
extern int64_t rt_bigint_sub(int64_t a, int64_t b);
extern int64_t rt_bigint_mul(int64_t a, int64_t b);
extern int64_t rt_bigint_div(int64_t a, int64_t b);
extern int64_t rt_bigint_mod(int64_t a, int64_t b);
extern int64_t rt_bigint_from_i64_raw(int64_t value);
extern int64_t rt_magic_tbuf_elem_size(int64_t v);

/*
 * Raw typed buffers have no managed-heap header, so probing arbitrary dynamic
 * pointers for the 32-byte tbuf header is unsafe. Keep provenance for the
 * handles created by the raw allocator and only decode headers for registered
 * data pointers. Buffers are runtime-owned and intentionally never removed
 * from this bounded registry during a process lifetime.
 * The registry covers transient compiler-generated buffers as well as
 * long-lived containers.  Keep enough entries for the largest benchmark
 * allocation burst without turning a full registry into a false OOB result.
 */
#define RT_TBUF_HANDLE_LIMIT (1u << 22)
static _Atomic(uintptr_t) rt_tbuf_handles[RT_TBUF_HANDLE_LIMIT];

/*
 * Direct-mapped read cache for the registry.  Buffer data pointers are never
 * evicted from rt_tbuf_handles during a process lifetime, so a cached hit can
 * never go stale; the magic-header recheck at every decode site remains the
 * authority and defeats any stale/alias address.  This keeps the O(N) atomic
 * probe (up to 2^22 slots) out of the per-element access hot loop.
 */
#define RT_TBUF_HANDLE_CACHE_BITS 12
#define RT_TBUF_HANDLE_CACHE_SIZE (1u << RT_TBUF_HANDLE_CACHE_BITS)
#define RT_TBUF_HANDLE_CACHE_MASK (RT_TBUF_HANDLE_CACHE_SIZE - 1u)
static __thread uintptr_t rt_tbuf_handle_cache[RT_TBUF_HANDLE_CACHE_SIZE];

volatile uint64_t g_dbg_mincore_calls = 0;
volatile uint64_t g_dbg_readable_entries = 0;
volatile uint64_t g_dbg_readable_fast = 0;
volatile uint64_t g_dbg_readable_array_hit = 0;
volatile uint64_t g_dbg_enabled = 0;
volatile uint64_t g_dbg_caller_ret[DBG_CALLER_SLOTS] = {0};
volatile uintptr_t g_dbg_caller_ips[DBG_CALLER_SLOTS] = {0};

static void dbg_dump_readable(void) {
  fprintf(stderr,
          "DBG mincore=%llu entries=%llu fast=%llu array_hit=%llu\n",
          (unsigned long long)g_dbg_mincore_calls,
          (unsigned long long)g_dbg_readable_entries,
          (unsigned long long)g_dbg_readable_fast,
          (unsigned long long)g_dbg_readable_array_hit);
  for (int i = 0; i < DBG_CALLER_SLOTS; ++i) {
    if (g_dbg_caller_ret[i])
      fprintf(stderr, "DBG caller %04d: ip=0x%llx count=%llu\n", i,
              (unsigned long long)g_dbg_caller_ips[i],
              (unsigned long long)g_dbg_caller_ret[i]);
  }
}
static int dbg_dump_registered = 0;

void dbg_maybe_register(void) {
  if (!dbg_dump_registered) {
    dbg_dump_registered = 1;
    if (g_dbg_enabled)
      atexit(dbg_dump_readable);
  }
}

__attribute__((constructor)) static void rt_dbg_ctor(void) {
  g_dbg_enabled = rt_env_enabled("NYTRIX_DBG_MINCORE") ? 1 : 0;
  if (g_dbg_enabled)
    dbg_maybe_register();
}

rt_oracle_range_t g_rt_oracle_snapshot[RT_ORACLE_SNAPSHOT_MAX] = {{0, 0}};
uint32_t g_rt_oracle_snapshot_count = 0;
rt_oracle_range_t g_rt_oracle_extra[RT_ORACLE_EXTRA_MAX] = {{0, 0}};
_Atomic uint32_t g_rt_oracle_extra_count = 0;
_Atomic uintptr_t g_rt_oracle_lo = 0;
_Atomic uintptr_t g_rt_oracle_hi = 0;
_Atomic uint32_t g_rt_oracle_ready = 0;
_Atomic uint32_t g_rt_oracle_full = 0;
uintptr_t g_rt_oracle_gap = RT_ORACLE_GAP_DEFAULT;

void rt_map_oracle_init(void) {
  if (atomic_load_explicit(&g_rt_oracle_ready, memory_order_relaxed))
    return;
  const char *env_gap = getenv("NYTRIX_ORACLE_GAP");
  if (env_gap && *env_gap) {
    char *endp = NULL;
    unsigned long long v = strtoull(env_gap, &endp, 0);
    if (endp != env_gap)
      g_rt_oracle_gap = (uintptr_t)v * 1024ULL * 1024ULL;
  }
#ifdef __linux__
  FILE *f = fopen("/proc/self/maps", "r");
  if (f) {
    char line[512];
    uint32_t n = 0;
    while (n < RT_ORACLE_SNAPSHOT_MAX && fgets(line, sizeof line, f)) {
      unsigned long long a = 0, b = 0;
      char perms[8] = {0};
      if (sscanf(line, "%llx-%llx %7s", &a, &b, perms) >= 3 && perms[0] == 'r') {
        if (b > a) {
          g_rt_oracle_snapshot[n].start = (uintptr_t)a;
          g_rt_oracle_snapshot[n].end = (uintptr_t)b;
          n++;
        }
      }
    }
    fclose(f);
    for (uint32_t i = 1; i < n; ++i) {
      rt_oracle_range_t t = g_rt_oracle_snapshot[i];
      uint32_t j = i;
      while (j > 0 && g_rt_oracle_snapshot[j - 1].start > t.start) {
        g_rt_oracle_snapshot[j] = g_rt_oracle_snapshot[j - 1];
        j--;
      }
      g_rt_oracle_snapshot[j] = t;
    }
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (m == 0) {
        g_rt_oracle_snapshot[m++] = g_rt_oracle_snapshot[i];
      } else if (g_rt_oracle_snapshot[i].start <=
                 g_rt_oracle_snapshot[m - 1].end) {
        if (g_rt_oracle_snapshot[i].end > g_rt_oracle_snapshot[m - 1].end)
          g_rt_oracle_snapshot[m - 1].end = g_rt_oracle_snapshot[i].end;
      } else {
        g_rt_oracle_snapshot[m++] = g_rt_oracle_snapshot[i];
      }
    }
    g_rt_oracle_snapshot_count = m;
    if (m) {
      atomic_store_explicit(&g_rt_oracle_lo, g_rt_oracle_snapshot[0].start,
                            memory_order_release);
      atomic_store_explicit(&g_rt_oracle_hi, g_rt_oracle_snapshot[m - 1].end,
                            memory_order_release);
    }
  }
#endif
  atomic_store_explicit(&g_rt_oracle_ready, 1, memory_order_release);
}

void rt_map_oracle_add(uintptr_t start, size_t size) {
  if (!start || (uintptr_t)size > UINTPTR_MAX - start)
    return;
  uintptr_t s = start & ~(uintptr_t)4095u;
  uintptr_t e = (start + (uintptr_t)size + 4095u) & ~(uintptr_t)4095u;
  if (e <= s)
    return;
  static __thread uintptr_t dedup_s[RT_ORACLE_DEDUP] = {0};
  static __thread uintptr_t dedup_e[RT_ORACLE_DEDUP] = {0};
  static __thread uint32_t dedup_idx = 0;
  for (uint32_t i = 0; i < RT_ORACLE_DEDUP; ++i) {
    if (dedup_e[i] && s >= dedup_s[i] && e <= dedup_e[i])
      return;
    if (dedup_s[i] >= s && dedup_e[i] && dedup_e[i] <= e) {
      dedup_s[i] = 0;
      dedup_e[i] = 0;
    }
  }
  uint32_t slot = dedup_idx++;
  if (slot >= RT_ORACLE_DEDUP)
    slot %= RT_ORACLE_DEDUP;
  dedup_s[slot] = s;
  dedup_e[slot] = e;
  if (atomic_load_explicit(&g_rt_oracle_full, memory_order_relaxed))
    return;
  uint32_t cnt =
      atomic_load_explicit(&g_rt_oracle_extra_count, memory_order_relaxed);
  if (cnt >= RT_ORACLE_EXTRA_MAX) {
    atomic_store_explicit(&g_rt_oracle_full, 1, memory_order_release);
    return;
  }
  g_rt_oracle_extra[cnt].start = s;
  g_rt_oracle_extra[cnt].end = e;
  atomic_store_explicit(&g_rt_oracle_extra_count, cnt + 1,
                        memory_order_release);
  uintptr_t cur = atomic_load_explicit(&g_rt_oracle_hi, memory_order_relaxed);
  while (e > cur) {
    if (atomic_compare_exchange_weak_explicit(
            &g_rt_oracle_hi, &cur, e, memory_order_release,
            memory_order_relaxed))
      break;
  }
  cur = atomic_load_explicit(&g_rt_oracle_lo, memory_order_relaxed);
  while (s < cur) {
    if (atomic_compare_exchange_weak_explicit(
            &g_rt_oracle_lo, &cur, s, memory_order_release,
            memory_order_relaxed))
      break;
  }
}

static size_t rt_tbuf_handle_slot(uintptr_t data) {
  uint64_t x = (uint64_t)data;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  return (size_t)x & (RT_TBUF_HANDLE_LIMIT - 1u);
}

static size_t rt_tbuf_handle_cache_slot(uintptr_t data) {
  return ((data >> 4) ^ (data >> 12) ^ (data >> 21)) &
         RT_TBUF_HANDLE_CACHE_MASK;
}

static void rt_tbuf_cache_handle(uintptr_t data) {
  rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] = data;
}

static void rt_tbuf_register_handle(uintptr_t data) {
  if (!data)
    return;
  size_t start = rt_tbuf_handle_slot(data);
  for (size_t probe = 0; probe < RT_TBUF_HANDLE_LIMIT; ++probe) {
    size_t i = (start + probe) & (RT_TBUF_HANDLE_LIMIT - 1u);
    uintptr_t empty = 0;
    if (atomic_compare_exchange_weak_explicit(
            &rt_tbuf_handles[i], &empty, data, memory_order_acq_rel,
            memory_order_acquire) || empty == data) {
      rt_tbuf_cache_handle(data);
      return;
    }
  }
}

static void rt_tbuf_replace_handle(uintptr_t old_data, uintptr_t new_data) {
  (void)old_data;
  rt_tbuf_register_handle(new_data);
}

static bool rt_tbuf_known_handle(uintptr_t data) {
  if (!data)
    return false;
  if (rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] == data)
    return true;
  size_t start = rt_tbuf_handle_slot(data);
  for (size_t probe = 0; probe < RT_TBUF_HANDLE_LIMIT; ++probe) {
    size_t i = (start + probe) & (RT_TBUF_HANDLE_LIMIT - 1u);
    uintptr_t seen = atomic_load_explicit(&rt_tbuf_handles[i], memory_order_acquire);
    if (seen == data) {
      rt_tbuf_cache_handle(data);
      return true;
    }
    if (!seen)
      break;
  }
  if (rt_header_readable_cached(data - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)(data - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      rt_tbuf_cache_handle(data);
      return true;
    }
  }
  return false;
}

/*
 * BigInt entry points consume Ny's tagged scalar/object representation.  The
 * native backend intentionally keeps inferred integer locals raw, so a raw
 * zero/even value must be materialized before it is paired with a BigInt.
 * Managed objects are left untouched; this helper is only used on a path that
 * has already established that at least one operand is a BigInt.
 */
static int64_t rt_bigint_operand(int64_t value) {
  if (is_int(value) || rt_value_tag(value) == TAG_BIGINT)
    return value;
  if (is_v_flt(value) || is_v_str(value) ||
      (is_ptr(value) && is_heap_ptr(value)) ||
      rt_magic_tbuf_elem_size(value) > 0 || rt_value_tag(value) >= 100)
    return value;
  return rt_bigint_from_i64_raw(value);
}

int color_mode __attribute__((weak)) = 0;
int debug_enabled __attribute__((weak)) = 0;

int64_t rt_globals_ptr = 0;
static int ny_vec_dim_impl(int64_t v);

/*
 * Typed-buffer offsets are formed with pointer differences throughout the
 * runtime.  Keeping every capacity below PTRDIFF_MAX / element-size makes
 * those offsets representable before an allocation or an index calculation
 * can wrap, independently of the platform's (possibly larger) SIZE_MAX.
 */
static bool rt_tbuf_capacity_fits(int64_t capacity, int64_t elem_size) {
  return capacity >= 0 && elem_size > 0 &&
         (uint64_t)capacity <= (uint64_t)PTRDIFF_MAX / (uint64_t)elem_size &&
         (uint64_t)capacity <=
             (SIZE_MAX - RT_NATIVE_TBUF_HEADER) / (uint64_t)elem_size;
}

/*
 * LLVM-free native lowering uses this raw typed-buffer allocator directly.
 * The public std.core.tbuf API returns the data pointer. Its 32-byte header
 * is magic, count, element size, and capacity, in that order.
 */

int64_t rt_zalloc_raw(int64_t size) {
  if (size <= 0)
    size = 1;
  if ((uint64_t)size > SIZE_MAX)
    return 0;
  void *p = calloc(1, (size_t)size);
  if (!p)
    return 0;
  /*
   * Native raw allocations must not be mistaken for a traced-heap header.
   */
  rt_map_oracle_add((uintptr_t)p, (size_t)size);
  rt_heap_ptr_neg_cache_store((uintptr_t)p);
  rt_raw_ptr_register((int64_t)(uintptr_t)p);
  return (int64_t)(uintptr_t)p;
}

/*
 * Companion for rt_zalloc_raw: both arguments and result use the raw
 * native ABI, unlike rt_free which manages the tagged/traced heap.
 */
int64_t rt_zfree_raw(int64_t ptr) {
  if (ptr) {
    rt_raw_ptr_unregister(ptr);
    free((void *)(uintptr_t)ptr);
  }
  return 0;
}

int64_t rt_tbuf_new_raw(int64_t count, int64_t elem_size) {
  if (count < 0)
    count = 0;
  if (elem_size <= 0)
    elem_size = 1;
  int64_t capacity = count < 4 ? 4 : count;
  if (!rt_tbuf_capacity_fits(capacity, elem_size))
    return 0;
  size_t bytes = RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  unsigned char *base = calloc(1, bytes);
  if (!base)
    return 0;
  int64_t *hdr = (int64_t *)base;
  hdr[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
  hdr[1] = count;
  hdr[2] = elem_size;
  hdr[3] = capacity;
  rt_map_oracle_add((uintptr_t)base, bytes);
  rt_tbuf_register_handle((uintptr_t)(base + RT_NATIVE_TBUF_HEADER));
  return (int64_t)(uintptr_t)(base + RT_NATIVE_TBUF_HEADER);
}

int64_t rt_tbuf_append_raw(int64_t buffer, int64_t value,
                              int64_t is_string) {
  if (is_int(buffer)) {
    rt_panic(rt_alloc_string("append expects a list, got int"));
    return 0;
  }
  if (!buffer)
    return 0;
  int64_t *base = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)base[0] != NY_NATIVE_TBUF_MAGIC) {
    rt_panic(rt_alloc_string("append expects a list"));
    return 0;
  }
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = base[3];
  if (count >= 0 && count < capacity && elem_size == 8 && !is_string) {
    base[1] = count + 1;
    ((int64_t *)(uintptr_t)buffer)[count] = value;
    return buffer;
  }
  unsigned char *data = (unsigned char *)(uintptr_t)buffer;
  unsigned char *raw_base = data - RT_NATIVE_TBUF_HEADER;
  if (count < 0 || elem_size <= 0 || capacity < count ||
      (uint64_t)count >=
          (SIZE_MAX - RT_NATIVE_TBUF_HEADER) / (uint64_t)elem_size)
    return 0;
  int64_t new_count = count + 1;
  if (new_count > capacity) {
    int64_t new_capacity = capacity < 4 ? 4 : capacity;
    if (new_capacity > INT64_MAX / 2)
      new_capacity = INT64_MAX;
    else
      new_capacity *= 2;
    if (new_capacity < new_count ||
        !rt_tbuf_capacity_fits(new_capacity, elem_size))
      return 0;
    size_t old_bytes =
        RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
    size_t new_bytes =
        RT_NATIVE_TBUF_HEADER + (size_t)new_capacity * (size_t)elem_size;
    uintptr_t old_data = (uintptr_t)data;
    unsigned char *grown = NULL;
    if (is_heap_ptr((int64_t)(uintptr_t)raw_base)) {
      grown = realloc(raw_base, new_bytes);
    } else {
      grown = malloc(new_bytes);
      if (grown)
        memcpy(grown, raw_base, old_bytes);
    }
    if (!grown)
      return 0;
    memset(grown + old_bytes, 0, new_bytes - old_bytes);
    raw_base = grown;
    base = (int64_t *)grown;
    data = raw_base + RT_NATIVE_TBUF_HEADER;
    capacity = new_capacity;
    base[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
    base[3] = capacity;
    rt_tbuf_replace_handle(old_data, (uintptr_t)(raw_base + RT_NATIVE_TBUF_HEADER));
  }
  unsigned char *slot = data + (size_t)count * (size_t)elem_size;
  if (elem_size == 8 && !is_string) {
    base[1] = new_count;
    ((int64_t *)(uintptr_t)data)[count] = value;
    return (int64_t)(uintptr_t)data;
  }
  base[1] = new_count;
  memset(slot, 0, (size_t)elem_size);
  /*
   * elem_size may be smaller than sizeof(value) (1-, 2-, and 4-byte element
   * buffers are first-class); copy only what the slot holds so appends at
   * the final slot cannot spill past the allocation. Little-endian low
   * bytes match store8/store32-style truncation.
   */
  {
    size_t copy =
        (size_t)elem_size < sizeof(value) ? (size_t)elem_size : sizeof(value);
    memcpy(slot, &value, copy);
  }
  if (elem_size >= 24) {
    int64_t len =
        (is_string || rt_native_is_str(value)) && (uintptr_t)value >= 4096
            ? (int64_t)strlen((const char *)(uintptr_t)value)
            : 0;
    int64_t tag =
        (is_string || rt_native_is_str(value)) ? 121 : rt_value_tag(value);
    memcpy(slot + 8, &len, sizeof(len));
    memcpy(slot + 16, &tag, sizeof(tag));
  }
  return (int64_t)(uintptr_t)data;
}

/*
 * Dynamic `any` values use the tagged scalar ABI at the call boundary, while
 * native list slots store scalar integers raw.  Keep this conversion explicit
 * so an odd raw integer passed to the typed append path is never mistaken for
 * a tagged value.
 */
int64_t rt_tbuf_append_tagged(int64_t buffer, int64_t value,
                                  int64_t is_string) {
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      value = decoded;
  }
  /*
   * A dynamic append upgrades an elem-8 buffer to descriptor storage before
   * the first value loses its provenance. This is the only representation
   * that can safely carry raw integers, nil/bools, floats, and pointers in
   * one sequence.
   */
  if (buffer &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC && hdr[2] == 8) {
      int64_t count = hdr[1];
      int64_t out = rt_tbuf_new_raw(count, 24);
      if (!out)
        return 0;
      for (int64_t i = 0; i < count; ++i) {
        int64_t raw = ((int64_t *)(uintptr_t)buffer)[i];
        unsigned char *slot = (unsigned char *)(uintptr_t)out + (size_t)i * 24;
        int64_t tag = rt_value_tag(raw);
        if (tag == 1 || tag == 0) {
          memcpy(slot, &raw, sizeof(raw));
          tag = tag == 1 ? 1 : 0;
        } else {
          memcpy(slot, &raw, sizeof(raw));
        }
        memcpy(slot + 16, &tag, sizeof(tag));
      }
      buffer = out;
    }
  }
  int64_t original = value;
  bool untagged_int = false;
  if (!is_string && is_int(value) && !rt_native_is_str(value) &&
      !is_v_str(value) && !rt_magic_tbuf_elem_size(value) &&
      !rt_heap_object_ptr(value)) {
    value = rt_untag_v(value);
    untagged_int = true;
  }
  if (buffer &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC && hdr[2] >= 24) {
      int64_t out = rt_tbuf_append_raw(buffer, value, is_string);
      /*
       * append_raw stamps tag 1 ("untagged raw integer from a tagged int")
       * so index reads re-tag the payload.  A dynamic append of a
       * non-integer value (a list, dict, or string handle) must keep its
       * identity instead: re-tagging the handle doubled it and mapped[i]
       * returned raw addresses as decimals.
       */
      if (out && !untagged_int &&
          rt_header_readable_cached((uintptr_t)out - RT_NATIVE_TBUF_HEADER,
                                    RT_NATIVE_TBUF_HEADER)) {
        int64_t *ohdr = (int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER);
        if ((uint64_t)ohdr[0] == NY_NATIVE_TBUF_MAGIC && ohdr[2] >= 24 &&
            ohdr[1] > 0) {
          unsigned char *slot = (unsigned char *)(uintptr_t)out +
                                (size_t)(ohdr[1] - 1) * (size_t)ohdr[2];
          int64_t tag = is_string ? TAG_STR_CONST : rt_value_tag(original);
          if (tag == 1)
            tag = rt_value_tag(value);
          memcpy(slot + 16, &tag, sizeof(tag));
        }
      }
      return out;
    }
  }
  return rt_tbuf_append_raw(buffer, value, is_string);
}

int64_t rt_tbuf_append_i64_raw(int64_t buffer, int64_t value) {
  if (!buffer)
    return 0;
  int64_t *base = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)base[0] != NY_NATIVE_TBUF_MAGIC)
    return 0;
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = base[3];
  if (count < 0 || elem_size <= 0 || capacity < count)
    return 0;
  /*
   * An empty descriptor buffer is the representation used for an untyped
   * `[]`.  When its first append is the raw i64 operation, no dynamic
   * provenance is present yet; compact it to the scalar stride so hot loops
   * (such as fannkuch) avoid paying descriptor width and tag decoding for
   * every element.  append_any upgrades it back to descriptors if a later
   * heterogeneous value requires that representation.
   */
  if (count == 0 && elem_size == 24) {
    base[2] = elem_size = 8;
  }
  if (count >= 0 && count < capacity) {
    base[1] = count + 1;
    unsigned char *slot =
        (unsigned char *)(uintptr_t)buffer + (size_t)count * (size_t)elem_size;
    size_t copy =
        (size_t)elem_size < sizeof(value) ? (size_t)elem_size : sizeof(value);
    memset(slot, 0, (size_t)elem_size);
    memcpy(slot, &value, copy);
    /*
     * append_i64 carries an untagged native integer even when the buffer is
     * descriptor-sized.  Record that provenance explicitly: tag zero means
     * nil to the dynamic decoder, so raw integer zero must still be tagged as
     * an integer.
     */
    if (elem_size >= 24) {
      int64_t tag = 1;
      memcpy(slot + 16, &tag, sizeof(tag));
    }
    return buffer;
  }
  unsigned char *data = (unsigned char *)(uintptr_t)buffer;
  unsigned char *raw_base = data - RT_NATIVE_TBUF_HEADER;
  if ((uint64_t)count >=
      (SIZE_MAX - RT_NATIVE_TBUF_HEADER) / (uint64_t)elem_size)
    return 0;
  int64_t new_count = count + 1;
  int64_t new_capacity = capacity < 4 ? 4 : capacity;
  if (new_capacity > INT64_MAX / 2)
    new_capacity = INT64_MAX;
  else
    new_capacity *= 2;
  if (new_capacity < new_count ||
      !rt_tbuf_capacity_fits(new_capacity, elem_size))
    return 0;
  size_t old_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  size_t new_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)new_capacity * (size_t)elem_size;
  unsigned char *grown = NULL;
  if (is_heap_ptr((int64_t)(uintptr_t)raw_base)) {
    grown = realloc(raw_base, new_bytes);
  } else {
    grown = malloc(new_bytes);
    if (grown)
      memcpy(grown, raw_base, old_bytes);
  }
  if (!grown)
    return 0;
  memset(grown + old_bytes, 0, new_bytes - old_bytes);
  base = (int64_t *)grown;
  data = grown + RT_NATIVE_TBUF_HEADER;
  base[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
  base[1] = new_count;
  base[2] = elem_size;
  base[3] = new_capacity;
  unsigned char *slot = data + (size_t)count * (size_t)elem_size;
  size_t copy =
      (size_t)elem_size < sizeof(value) ? (size_t)elem_size : sizeof(value);
  memset(slot, 0, (size_t)elem_size);
  memcpy(slot, &value, copy);
  if (elem_size >= 24) {
    int64_t tag = 1;
    memcpy(slot + 16, &tag, sizeof(tag));
  }
  /*
   * realloc may move the buffer.  Every other growth site re-registers the
   * new data pointer; skipping it here left grown buffers unknown to
   * rt_tbuf_known_handle, so the next mapped[i]-style read missed the tbuf
   * path and panicked with "index_read out of range".
   */
  rt_tbuf_replace_handle((uintptr_t)buffer, (uintptr_t)data);
  return (int64_t)(uintptr_t)data;
}

int64_t rt_tbuf_repeat(int64_t buffer, int64_t repeat_count) {
  if (!buffer)
    return 0;
  int64_t *base = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)base[0] != NY_NATIVE_TBUF_MAGIC)
    return 0;
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = base[3];
  if (count < 0 || elem_size <= 0 || capacity < count)
    return 0;
  if (repeat_count <= 0 || count == 0)
    return rt_tbuf_new_raw(0, elem_size);
  if (count > INT64_MAX / repeat_count)
    return 0;
  int64_t total_count = count * repeat_count;
  if (!rt_tbuf_capacity_fits(total_count, elem_size))
    return 0;
  /*
   * Repetition is also used by pattern matching and other dynamic consumers.
   * Promote raw scalar buffers to descriptor storage so the repeated result
   * retains per-element runtime kind information instead of exposing an
   * untyped elem-8 payload.
   */
  int64_t out_elem_size = elem_size == 8 ? 24 : elem_size;
  int64_t out = rt_tbuf_new_raw(total_count, out_elem_size);
  if (!out)
    return 0;
  if (out_elem_size != elem_size) {
    ((int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER))[1] = 0;
    int64_t out_count = 0;
    for (int64_t r = 0; r < repeat_count; ++r) {
      for (int64_t i = 0; i < count; ++i) {
        int64_t value = rt_tbuf_get(buffer, i, 0);
        if (!rt_tbuf_append_raw(out, value, rt_native_is_str(value)))
          return 0;
        out_count++;
      }
    }
    (void)out_count;
    return out;
  }
  size_t chunk = (size_t)count * (size_t)elem_size;
  unsigned char *src = (unsigned char *)(uintptr_t)buffer;
  unsigned char *dst = (unsigned char *)(uintptr_t)out;
  memcpy(dst, src, chunk);
  size_t filled = chunk;
  size_t total = (size_t)total_count * (size_t)elem_size;
  while (filled < total) {
    size_t copy = filled < total - filled ? filled : total - filled;
    memcpy(dst + filled, dst, copy);
    filled += copy;
  }
  return out;
}
int64_t rt_tbuf_extend(int64_t buffer, int64_t other) {
  if (!buffer || !other)
    return buffer;
  int64_t count = rt_tbuf_len_raw(other);
  for (int64_t i = 0; i < count; ++i)
    buffer = rt_tbuf_append_i64_raw(buffer, rt_tbuf_get(other, i, 0));
  return buffer;
}

int64_t rt_tbuf_concat_raw(int64_t left, int64_t right) {
  if (!left && !right)
    return 0;
  if (!left)
    return rt_tbuf_clone_raw(right);
  if (!right)
    return rt_tbuf_clone_raw(left);
  int64_t count_l = rt_tbuf_len_raw(left);
  int64_t count_r = rt_tbuf_len_raw(right);
  int64_t *base_l = (uintptr_t)left >= 4096
                        ? (int64_t *)((uintptr_t)left - RT_NATIVE_TBUF_HEADER)
                        : NULL;
  int64_t *base_r = (uintptr_t)right >= 4096
                        ? (int64_t *)((uintptr_t)right - RT_NATIVE_TBUF_HEADER)
                        : NULL;
  int64_t elem_size_l =
      (base_l && (uint64_t)base_l[0] == NY_NATIVE_TBUF_MAGIC) ? base_l[2] : 24;
  int64_t elem_size_r =
      (base_r && (uint64_t)base_r[0] == NY_NATIVE_TBUF_MAGIC) ? base_r[2] : 24;
  int64_t elem_size =
      (elem_size_l == 24 || elem_size_r == 24) ? 24 : elem_size_l;
  int64_t out = rt_tbuf_new_raw(0, elem_size);
  if (!out)
    return 0;
  for (int64_t i = 0; i < count_l; ++i) {
    int64_t val = rt_tbuf_get(left, i, 0);
    out = rt_tbuf_append_raw(out, val, rt_native_is_str(val));
  }
  for (int64_t i = 0; i < count_r; ++i) {
    int64_t val = rt_tbuf_get(right, i, 0);
    out = rt_tbuf_append_raw(out, val, rt_native_is_str(val));
  }
  return out;
}

static int64_t rt_set_add_in_place(int64_t set_value, int64_t key) {
  int64_t tag = 0;
  if (!is_ptr(set_value) || !rt_try_read_i64((uintptr_t)set_value - 8, &tag) ||
      tag != TAG_SET)
    return 0;
  int64_t cap = *(int64_t *)((char *)(uintptr_t)set_value + 8);
  if (is_int(cap))
    cap >>= 1;
  if (cap <= 0 || (cap & (cap - 1)) != 0)
    return set_value;
  int64_t count = *(int64_t *)((char *)(uintptr_t)set_value + 0);
  if (count * 10 >= cap * 7) {
    int64_t newcap = cap * 2;
    if (newcap > 0 && newcap <= (1LL << 30) &&
        newcap <= (INT64_MAX - 16) / 24) {
      int64_t ns = rt_malloc(rt_tag_v(16 + newcap * 24));
      if (ns) {
        *(int64_t *)((char *)(uintptr_t)ns - 8) = TAG_SET;
        *(int64_t *)((char *)(uintptr_t)ns + 0) = 0;
        *(int64_t *)((char *)(uintptr_t)ns + 8) = newcap;
        memset((char *)(uintptr_t)ns + 16, 0, (size_t)newcap * 24);
        for (int64_t i = 0; i < cap; ++i) {
          int64_t off = 16 + i * 24;
          if (*(int64_t *)((char *)(uintptr_t)set_value + off + 16) == 1)
            rt_set_add_in_place(
                ns, *(int64_t *)((char *)(uintptr_t)set_value + off));
        }
        rt_free(set_value);
        return rt_set_add_in_place(ns, key);
      }
    }
  }
  uint64_t hash = (uint64_t)key;
  if (rt_native_is_str(key)) {
    hash = 2166136261u;
    int64_t n = rt_cstr_len(key);
    const unsigned char *s = (const unsigned char *)(uintptr_t)key;
    for (int64_t i = 0; i < n; ++i)
      hash = ((hash ^ s[i]) * 16777619u) & 0x7fffffffu;
  }
  int64_t idx = (int64_t)(hash & (uint64_t)(cap - 1));
  uint64_t perturb = hash;
  for (int64_t probes = 0; probes < cap; ++probes) {
    int64_t off = 16 + idx * 24;
    int64_t state = *(int64_t *)((char *)(uintptr_t)set_value + off + 16);
    int64_t old = *(int64_t *)((char *)(uintptr_t)set_value + off);
    if (state == 1 &&
        (old == key || (rt_native_is_str(old) && rt_native_is_str(key) &&
                        rt_cstr_eq(old, key))))
      return set_value;
    if (state == 0 || state == 2) {
      *(int64_t *)((char *)(uintptr_t)set_value + off) = key;
      *(int64_t *)((char *)(uintptr_t)set_value + off + 8) = 1;
      *(int64_t *)((char *)(uintptr_t)set_value + off + 16) = 1;
      (*(int64_t *)((char *)(uintptr_t)set_value + 0))++;
      return set_value;
    }
    idx = (idx * 5 + 1 + (int64_t)(perturb >> 5)) & (cap - 1);
    perturb >>= 5;
  }
  return set_value;
}

static int64_t rt_add_raw_impl(int64_t left, int64_t right) {
  int64_t set_result = rt_set_add_in_place(left, right);
  if (set_result)
    return set_result;
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  if (dim_l > 0 && dim_r > 0)
    return rt_vec_add_raw(left, right);
  if (rt_native_is_str(left) || rt_native_is_str(right)) {
    return rt_cstr_concat(rt_native_is_str(left) ? left : rt_any_to_cstr(left),
                          rt_native_is_str(right) ? right
                                                  : rt_any_to_cstr(right));
  }
  if (left || right) {
    /*
     * Probe for the native tbuf header with the non-faulting readability
     * check (never dereference a packed integer or unrelated pointer): a
     * large odd-encoded i64 passes `>= 4096` but is not an address.
     */
    bool is_tbuf_l = false, is_tbuf_r = false;
    if ((uint64_t)left >= 4096 &&
        rt_header_readable_cached((uintptr_t)left - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER))
      is_tbuf_l = (uint64_t)((const int64_t *)((uintptr_t)left -
                                               RT_NATIVE_TBUF_HEADER))[0] ==
                  NY_NATIVE_TBUF_MAGIC;
    if ((uint64_t)right >= 4096 &&
        rt_header_readable_cached((uintptr_t)right - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER))
      is_tbuf_r = (uint64_t)((const int64_t *)((uintptr_t)right -
                                               RT_NATIVE_TBUF_HEADER))[0] ==
                  NY_NATIVE_TBUF_MAGIC;
    if (is_tbuf_l && !is_tbuf_r)
      return rt_tbuf_append_raw(left, right, rt_native_is_str(right));
    if ((is_tbuf_l && is_tbuf_r) || (!left && is_tbuf_r) ||
        (!right && is_tbuf_l)) {
      return rt_tbuf_concat_raw(left, right);
    }
  }
  return left + right;
}

/*
 * Typed native NYIR values are raw i64s; keep their arithmetic separate from
 * the tagged dynamic ABI used by any-valued LLVM/JIT calls.  Sharing the old
 * helper made tagged values 1 and 2 add as 3 rather than producing the boxed
 * value for integer 3.
 */
int64_t rt_raw_add(int64_t left, int64_t right) {
  /*
   * A value produced by a dynamic operation may promote to BigInt before the
   * surrounding expression is reclassified as raw.  Keep this mixed case on
   * the BigInt path instead of pointer arithmetic in the raw helper.
   */
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_add(rt_bigint_operand(left), rt_bigint_operand(right));
  return rt_add_raw_impl(left, right);
}

int64_t rt_any_to_i64(int64_t value) {
  if (is_v_flt(value))
    return (int64_t)rt_flt_unbox_double(value);
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      return decoded;
  }
  /*
   * Headerless JIT string constants may have an odd address. Check both
   * string representations before the low-bit integer test, otherwise a raw
   * C-string pointer is shifted as though it were tagged integer data.
   */
  if (rt_native_is_str(value) || is_v_str(value))
    return value;
  /*
   * Dynamic container handles can be odd addresses too.  Check the native
   * buffer/header forms before the low-bit tagged-integer test; otherwise a
   * form/list returned by a callback is mistaken for an integer and shifted.
   */
  if (rt_magic_tbuf_elem_size(value) > 0)
    return value;
  if (is_int(value))
    return rt_untag_v(value);
  if (rt_value_tag(value) == TAG_BIGINT)
    return rt_bigint_to_i64_raw(value);
  return value;
}

/*
 * Truthiness for a raw element-8 native sequence slot.  Raw zero is nil/zero,
 * while raw one is the integer 1 (not tagged zero); the ordinary dynamic
 * predicate cannot distinguish those representations after the slot load.
 */
int64_t rt_raw_truthy(int64_t value) {
  if (value == 0)
    return 0;
  if (rt_native_is_str(value) || is_v_str(value))
    return rt_cstr_len(value) > 0 ? 1 : 0;
  if (rt_magic_tbuf_elem_size(value) > 0)
    return 1;
  if (is_heap_ptr(value)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)value - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE)
      return *(int64_t *)((char *)(uintptr_t)value) != rt_tag_v(0) ? 1 : 0;
  }
  return 1;
}

int64_t rt_any_add(int64_t left, int64_t right) {
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_add(rt_bigint_operand(left), rt_bigint_operand(right));
  if (is_int(left) && is_int(right)) {
    int64_t l = rt_untag_v(left);
    int64_t r = rt_untag_v(right);
    int64_t sum = 0;
    if (!__builtin_add_overflow(l, r, &sum) && ny_small_int_fits_i64(sum))
      return rt_tag_v(sum);
    return rt_bigint_add(left, right);
  }
  return rt_add_raw_impl(left, right);
}

int64_t rt_bigint_neg_raw(int64_t value) {
  /*
   * Native literal lowering passes out-of-range integer literals as raw
   * signed values, while ordinary runtime values use Nytrix's tagged format.
   * Normalize that boundary before asking the BigInt helpers to negate it.
   */
  if (!is_int(value) && rt_value_tag(value) != TAG_BIGINT)
    value = rt_bigint_from_i64_raw(value);
  return rt_bigint_sub(rt_bigint_from_i64_raw(0), value);
}

int64_t rt_tbuf_slice(int64_t buffer, int64_t start, int64_t stop) {
  const int64_t step = 1;
  int64_t count = rt_tbuf_len_raw(buffer);
  if (step == 0)
    return rt_tbuf_new_raw(0, 24);
  if (start < 0)
    start += count;
  if (stop < 0)
    stop += count;
  if (step > 0) {
    if (start < 0)
      start = 0;
    if (start > count)
      start = count;
    if (stop < 0)
      stop = 0;
    if (stop > count)
      stop = count;
  } else {
    if (start >= count)
      start = count - 1;
    if (start < -1)
      start = -1;
    if (stop >= count)
      stop = count - 1;
    if (stop < -1)
      stop = -1;
  }
  int64_t out = rt_tbuf_new_raw(0, 24);
  if (!out)
    return 0;
  for (int64_t i = start; step > 0 ? i < stop : i > stop; i += step) {
    int64_t value = rt_tbuf_get(buffer, i, 0);
    out = rt_tbuf_append_raw(out, value, rt_native_is_str(value));
    if (!out)
      return 0;
  }
  return out;
}

int64_t rt_tbuf_len_raw(int64_t buffer) {
  if (!buffer)
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return hdr[1];
    }
  }
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      return is_int(len_v) ? (len_v >> 1) : len_v;
    }
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)buffer + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)buffer + 16);
      if (step > 0 && start < stop)
        return (stop - start + step - 1) / step;
      if (step < 0 && start > stop)
        return (start - stop - step - 1) / (-step);
      return 0;
    }
  }
  /*
   * Dynamic `any` strings share the `.len` surface with list values.
   */
  if (rt_native_is_str(buffer))
    return rt_cstr_len(buffer);
  return 0;
}
int64_t rt_tbuf_reserve(int64_t buffer, int64_t capacity) {
  if (!buffer || capacity < 0 ||
      !rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return buffer;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)hdr[0] != NY_NATIVE_TBUF_MAGIC)
    return buffer;
  int64_t count = hdr[1], elem_size = hdr[2], old_capacity = hdr[3];
  if (count < 0 || elem_size <= 0 || old_capacity < count ||
      capacity <= old_capacity || !rt_tbuf_capacity_fits(capacity, elem_size))
    return buffer;
  size_t old_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)old_capacity * (size_t)elem_size;
  size_t new_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  unsigned char *grown = NULL;
  if (is_heap_ptr((int64_t)(uintptr_t)hdr)) {
    grown = realloc((unsigned char *)hdr, new_bytes);
  } else {
    grown = malloc(new_bytes);
    if (grown)
      memcpy(grown, (unsigned char *)hdr, old_bytes);
  }
  if (!grown)
    return buffer;
  memset(grown + old_bytes, 0, new_bytes - old_bytes);
  int64_t *new_hdr = (int64_t *)grown;
  new_hdr[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
  new_hdr[3] = capacity;
  rt_tbuf_replace_handle((uintptr_t)buffer,
                         (uintptr_t)(grown + RT_NATIVE_TBUF_HEADER));
  return (int64_t)(uintptr_t)(grown + RT_NATIVE_TBUF_HEADER);
}
int64_t rt_tbuf_clear_raw(int64_t buffer) {
  if (!buffer)
    return buffer;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      hdr[1] = 0;
      return buffer;
    }
  }
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      *(int64_t *)((char *)(uintptr_t)buffer + 0) = 0;
      return buffer;
    }
  }
  return buffer;
}
int64_t rt_tbuf_eq_raw(int64_t left, int64_t right) {
  if (left == right)
    return NY_IMM_TRUE;
  if (!left || !right)
    return NY_IMM_FALSE;
  if (rt_native_is_str(left) || rt_native_is_str(right) || is_v_str(left) ||
      is_v_str(right)) {
    if ((rt_native_is_str(left) || is_v_str(left)) &&
        (rt_native_is_str(right) || is_v_str(right)))
      return rt_cstr_eq(left, right) ? NY_IMM_TRUE : NY_IMM_FALSE;
    return NY_IMM_FALSE;
  }
  if (rt_value_tag(left) == TAG_RANGE)
    left = rt_range_values_raw(left);
  if (rt_value_tag(right) == TAG_RANGE)
    right = rt_range_values_raw(right);
  int64_t len_l = rt_tbuf_len_raw(left);
  int64_t len_r = rt_tbuf_len_raw(right);
  int64_t elem_l = rt_magic_tbuf_elem_size(left);
  int64_t elem_r = rt_magic_tbuf_elem_size(right);
  if (len_l != len_r)
    return NY_IMM_FALSE;
  for (int64_t i = 0; i < len_l; ++i) {
    int64_t vl = rt_tbuf_get(left, i, 0);
    int64_t vr = rt_tbuf_get(right, i, 0);
    int64_t tag_l = 0, tag_r = 0;
    if (elem_l >= 24)
      memcpy(&tag_l,
             (unsigned char *)(uintptr_t)left + (size_t)i * (size_t)elem_l + 16,
             sizeof(tag_l));
    if (elem_r >= 24)
      memcpy(&tag_r,
             (unsigned char *)(uintptr_t)right + (size_t)i * (size_t)elem_r +
                 16,
             sizeof(tag_r));
    /*
     * Descriptor lists record the scalar payload and its raw tag separately;
     * elem-8 lists carry an untagged payload. Compare the payload domain
     * explicitly before the generic dynamic comparator, which cannot infer
     * whether an odd integer came from either layout.
     */
    if (tag_l == 1 && tag_r == 1) {
      if (elem_l >= 24 && elem_r >= 24 && vl == vr)
        continue;
      if (elem_l == 8 && elem_r >= 24 && rt_untag_v(vl) == vr)
        continue;
      if (elem_l >= 24 && elem_r == 8 && vl == rt_untag_v(vr))
        continue;
    }
    /*
     * A typed buffer can itself be an element of an elem-8 buffer.  Those
     * slots contain raw handles, so scalar normalization below must not turn
     * a nested sequence into an integer-like dynamic value.  Compare nested
     * buffers structurally here; this also keeps equality recursive for
     * chunk/window/enumerate results.
     */
    if (rt_magic_tbuf_elem_size(vl) > 0 && rt_magic_tbuf_elem_size(vr) > 0) {
      int64_t nested = rt_tbuf_eq_raw(vl, vr);
      if (nested != NY_IMM_TRUE)
        return NY_IMM_FALSE;
      continue;
    }
    /*
     * Mixed list producers may leave one elem-8 slot in raw form while the
     * other carries the public tagged integer.  Resolve that ambiguity before
     * the normalisation below (which otherwise tags an already-tagged odd
     * word a second time).
     */
    if (is_int(vl) && is_int(vr) &&
        (vl == rt_untag_v(vr) || vr == rt_untag_v(vl)))
      continue;
    /*
     * Elem-8 buffers carry raw scalar payloads, whereas descriptor buffers
     * return canonical dynamic values. Normalize only the unambiguous scalar
     * side before comparing mixed representations.
     */
    if (elem_l == 8 && !is_ptr(vl) && !rt_native_is_str(vl) && !is_v_flt(vl))
      vl = rt_tag_v(vl);
    if (elem_r == 8 && !is_ptr(vr) && !rt_native_is_str(vr) && !is_v_flt(vr))
      vr = rt_tag_v(vr);
    /*
     * A raw elem-8 integer zero and canonical tagged zero are the same
     * sequence element; outside sequence comparison zero remains nil.
     */
    if ((vl == 0 && vr == rt_tag_v(0)) || (vr == 0 && vl == rt_tag_v(0)) ||
        (is_int(vl) && is_int(vr) &&
         (rt_untag_v(vl) == vr || rt_untag_v(vr) == vl)) ||
        (!is_int(vl) && is_int(vr) && vl == rt_untag_v(vr)) ||
        (is_int(vl) && !is_int(vr) && rt_untag_v(vl) == vr))
      continue;
    if (rt_any_eq(vl, vr) != NY_IMM_TRUE)
      return NY_IMM_FALSE;
  }
  return NY_IMM_TRUE;
}
int64_t rt_tbuf_clone_raw(int64_t buffer) {
  if (!buffer)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)hdr[0] != NY_NATIVE_TBUF_MAGIC)
    return 0;
  int64_t count = hdr[1], elem_size = hdr[2];
  if (count < 0 || elem_size <= 0 ||
      (uint64_t)count > SIZE_MAX / (uint64_t)elem_size)
    return 0;
  int64_t out = rt_tbuf_new_raw(count, elem_size);
  if (out && count)
    memcpy((void *)(uintptr_t)out, (void *)(uintptr_t)buffer,
           (size_t)count * (size_t)elem_size);
  return out;
}
int64_t rt_tbuf_swap(int64_t buffer, int64_t left, int64_t right) {
  if (!buffer)
    return buffer;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (count < 0 || elem_size <= 0 ||
          (uint64_t)count > SIZE_MAX / (uint64_t)elem_size)
        return buffer;
      if (left < 0)
        left += count;
      if (right < 0)
        right += count;
      if (left < 0 || left >= count || right < 0 || right >= count ||
          left == right || (uint64_t)left > SIZE_MAX / (uint64_t)elem_size ||
          (uint64_t)right > SIZE_MAX / (uint64_t)elem_size)
        return buffer;
      unsigned char *data = (unsigned char *)(uintptr_t)buffer;
      size_t width = (size_t)elem_size;
      unsigned char *a = data + (size_t)left * width;
      unsigned char *b = data + (size_t)right * width;
      for (size_t i = 0; i < width; ++i) {
        unsigned char tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
      }
      return buffer;
    }
  }
  /*
   * Keep the bridge useful for legacy heap lists when it is called through
   * an embedding or mixed-ABI path.
   */
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t count = is_int(len_v) ? rt_untag_v(len_v) : len_v;
      if (left < 0)
        left += count;
      if (right < 0)
        right += count;
      if (left >= 0 && left < count && right >= 0 && right < count &&
          left != right) {
        int64_t *items = (int64_t *)((char *)(uintptr_t)buffer + 16);
        int64_t tmp = items[left];
        items[left] = items[right];
        items[right] = tmp;
      }
    }
  }
  return buffer;
}

int64_t rt_tbuf_set_i64_raw(int64_t buffer, int64_t index, int64_t value) {
  if (!buffer)
    return rt_tag_v(0);
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2], capacity = hdr[3];
      if (index < 0)
        index += count;
      if (index >= 0 && index < count && elem_size == 8) {
        ((int64_t *)(uintptr_t)buffer)[index] = value;
        return buffer;
      }
      /*
       * Indexed assignment is allowed to materialize an element inside the
       * list's reserved capacity.  `list(n)` is intentionally empty, but
       * set_idx(list(n), 0, value) is the useful preallocated-list spelling;
       * append remains the operation that grows a list past its capacity.
       */
      if (index >= 0 && index < capacity && elem_size >= 8 &&
          (uint64_t)index <= SIZE_MAX / (uint64_t)elem_size) {
        unsigned char *data = (unsigned char *)(uintptr_t)buffer;
        unsigned char *slot = data + (size_t)index * (size_t)elem_size;
        if (index >= count) {
          size_t gap = (size_t)(index - count) * (size_t)elem_size;
          memset(data + (size_t)count * (size_t)elem_size, 0, gap);
          hdr[1] = index + 1;
        }
        memcpy(slot, &value, sizeof(value));
        if (elem_size >= 24) {
          int64_t len = rt_native_is_str(value) ? rt_cstr_len(value) : 0;
          int64_t tag = rt_value_tag(value);
          memcpy(slot + 8, &len, sizeof(len));
          memcpy(slot + 16, &tag, sizeof(tag));
        }
      } else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "set index out of range: index=%" PRId64 ", size=%" PRId64,
                 index, count);
        rt_panic(rt_alloc_string(msg));
      }
      return buffer;
    }
  }
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
      if (index < 0)
        index += n;
      if (index >= 0 && index < n)
        *(int64_t *)((char *)(uintptr_t)buffer + 16 + (size_t)index * 8) =
            value;
      else {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "set index out of range: index=%" PRId64 ", size=%" PRId64,
                 index, n);
        rt_panic(rt_alloc_string(msg));
      }
      return buffer;
    }
  }
  /*
   * Generic set_idx promises the language integer zero on an unsupported
   * receiver.  Returning a raw null pointer is decoded as nil by the dynamic
   * equality path, so preserve the public tagged-zero result here.
   */
  return rt_tag_v(0);
}

/*
 * Store an explicitly-typed float in a descriptor tbuf.
 */
int64_t rt_tbuf_set_f64_bits(int64_t buffer, int64_t index, int64_t bits) {
  if (!buffer)
    return rt_tag_v(0);
  if (rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2], capacity = hdr[3];
      if (index < 0)
        index += count;
      if (index >= 0 && index < capacity && elem_size >= 8 &&
          (uint64_t)index <= SIZE_MAX / (uint64_t)elem_size) {
        unsigned char *data = (unsigned char *)(uintptr_t)buffer;
        unsigned char *slot = data + (size_t)index * (size_t)elem_size;
        if (index >= count) {
          size_t gap = (size_t)(index - count) * (size_t)elem_size;
          memset(data + (size_t)count * (size_t)elem_size, 0, gap);
          hdr[1] = index + 1;
        }
        memcpy(slot, &bits, sizeof(bits));
        if (elem_size >= 24) {
          int64_t len = 0, tag = TAG_FLOAT;
          memcpy(slot + 8, &len, sizeof(len));
          memcpy(slot + 16, &tag, sizeof(tag));
        }
        return buffer;
      }
      char msg[128];
      snprintf(msg, sizeof(msg),
               "set float index out of range: index=%" PRId64 ", size=%" PRId64,
               index, count);
      rt_panic(rt_alloc_string(msg));
      return buffer;
    }
  }
  return rt_tbuf_set_i64_raw(buffer, index, bits);
}
/*
 * Legacy primitive calls use the tagged language ABI.  Keep that entry point
 * separate from the raw NYIR helper so odd native indices remain unambiguous.
 */
int64_t rt_tbuf_set_tagged(int64_t buffer, int64_t index, int64_t value) {
  int64_t raw_index = is_int(index) ? rt_untag_v(index) : index;
  if (buffer &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC && hdr[2] >= 24) {
      int64_t count = hdr[1], elem_size = hdr[2], capacity = hdr[3];
      if (raw_index < 0)
        raw_index += count;
      if (raw_index < 0 || raw_index >= capacity)
        return 0;
      unsigned char *slot = (unsigned char *)(uintptr_t)buffer +
                            (size_t)raw_index * (size_t)elem_size;
      int64_t tag = rt_value_tag(value);
      int64_t payload = value;
      /*
       * Descriptor slots carry a raw payload plus explicit metadata.  Keep
       * booleans as their immediate payloads; ordinary tagged integers are
       * untagged exactly once at this boundary.
       */
      if (tag == 1 && is_int(value) && value != NY_IMM_TRUE &&
          value != NY_IMM_FALSE)
        payload = rt_untag_v(value);
      if (tag == 0)
        payload = 0;
      int64_t len = (tag == TAG_STR || tag == TAG_STR_CONST) && value >= 4096
                        ? rt_cstr_len(value)
                        : 0;
      if (raw_index >= count) {
        size_t gap = (size_t)(raw_index - count) * (size_t)elem_size;
        memset((unsigned char *)(uintptr_t)buffer +
                   (size_t)count * (size_t)elem_size,
               0, gap);
        hdr[1] = raw_index + 1;
      }
      memcpy(slot, &payload, sizeof(payload));
      memcpy(slot + 8, &len, sizeof(len));
      memcpy(slot + 16, &tag, sizeof(tag));
      return buffer;
    }
  }
  return rt_tbuf_set_i64_raw(buffer, raw_index, value);
}
int64_t rt_tbuf_get(int64_t buffer, int64_t index, int64_t fallback) {
  if (is_int(buffer)) {
    rt_panic(rt_alloc_string("get expects a string, bytes, list, tuple, dict, "
                             "range, or vector, got int"));
    return fallback;
  }
  if (!buffer)
    return fallback;
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0)
        index += count;
      if (index >= 0 && index < count && elem_size == 8)
        return ((const int64_t *)(uintptr_t)buffer)[index];
      if (index >= 0 && index < count && elem_size >= 8 &&
          (uint64_t)index <= SIZE_MAX / (uint64_t)elem_size) {
        unsigned char *data = (unsigned char *)(uintptr_t)buffer;
        int64_t value = 0;
        int64_t tag = 0;
        memcpy(&value, data + (size_t)index * (size_t)elem_size, sizeof(value));
        /*
         * Dynamic descriptor lists keep f64 payload bits in slot 0 and the
         * runtime tag in slot 2.  Re-box float values at this ABI boundary;
         * callers of `any` APIs must receive a normal Ny float object, while
         * scalar 8-byte f64 buffers continue to return raw bits.
         */
        if (elem_size >= 24) {
          memcpy(&tag, data + (size_t)index * (size_t)elem_size + 16,
                 sizeof(tag));
          /*
           * Descriptor slots store raw payloads; the dynamic decoders
           * (dynamic tbuf reads) apply
           * tags for `any` consumers.  Boxing integer tags here would make
           * this accessor disagree with elem-8 reads and with
           * `rt_tbuf_index_read_raw` for the same element.
           */
          if (tag == TAG_FLOAT && !is_v_flt(value))
            return rt_flt_box_val(value);
        }
        return value;
      }
      return fallback;
    }
  }
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
      if (index < 0)
        index += n;
      if (index < 0 || index >= n)
        return fallback;
      return *(int64_t *)((char *)(uintptr_t)buffer + 16 + (size_t)index * 8);
    }
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)buffer + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)buffer + 16);
      int64_t n = 0;
      if (step > 0 && start < stop)
        n = (stop - start + step - 1) / step;
      else if (step < 0 && start > stop)
        n = (start - stop - step - 1) / (-step);
      if (index < 0 && n >= 0 && index >= -n)
        index += n;
      if (index < 0 || index >= n)
        return fallback;
      return start + index * step;
    }
  }
  return fallback;
}

int64_t rt_value_get_tagged(int64_t value, int64_t key, int64_t fallback);

/*
 * Dynamic receiver reads must decode raw elem-8 scalar slots before they
 * enter callback/any arithmetic.  Keep the typed accessor above raw and make
 * this boundary explicit, including the ordinary fallback contract.
 */
static int64_t ny_native_box_tbuf_any(int64_t value);
static int64_t rt_sequence_tag(int64_t v);

/* Dynamic-ABI element read for thread/async argument packing: elem-8
 * lists hold raw machine words, so scalar ints are tagged exactly once;
 * 24-byte descriptor slots already carry the tagged payload and are
 * returned as-is. */
int64_t rt_tbuf_dyn_elem(int64_t buffer, int64_t index,
                         int64_t want_dynamic) {
  if (!buffer)
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0)
        index += count;
      if (index < 0 || index >= count)
        return 0;
      int64_t value = 0;
      memcpy(&value, (void *)((uintptr_t)buffer + (size_t)index *
                              (size_t)elem_size), sizeof(value));
      if (elem_size >= 24) {
        int64_t tag = 0;
        memcpy(&tag, (void *)((uintptr_t)buffer + (size_t)index *
                              (size_t)elem_size + 16), sizeof(tag));
        if (tag == 0)
          return 0;
        if (tag == TAG_FLOAT)
          return is_v_flt(value) ? value : rt_flt_box_val(value);
        if (tag == 1 && !want_dynamic)
          return rt_untag_v(value);
        return value;
      }
      if (rt_native_is_str(value) || is_v_str(value) ||
          rt_heap_object_ptr(value))
        return value;
      if (want_dynamic)
        return rt_tag_v(value);
      return value;
    }
  }
  return 0;
}
int64_t rt_tbuf_get_any(int64_t buffer, int64_t index, int64_t fallback) {
  /* Native values crossing an `any` boundary may be compact encoded
   * pointers (tag 6), while the raw tbuf helpers expect the decoded address.
   * Decode only after validating the private header so ordinary tagged values
   * retain their normal semantics. */
  if (NY_NATIVE_IS(buffer) && NY_NATIVE_DECODE(buffer) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(buffer);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      buffer = decoded;
  }
  if (!buffer)
    return ny_native_box_tbuf_any(fallback);
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0)
        index += count;
      if (index < 0 || index >= count)
        return ny_native_box_tbuf_any(fallback);
      int64_t value = rt_tbuf_get(buffer, index, fallback);
      if (elem_size >= 24) {
        unsigned char *slot = (unsigned char *)(uintptr_t)buffer +
                              (size_t)index * (size_t)elem_size;
        int64_t tag = 0;
        memcpy(&tag, slot + 16, sizeof(tag));
        if (tag == 0)
          return 0;
        if (tag == TAG_FLOAT) {
          int64_t out = is_v_flt(value) ? value : rt_flt_box_val(value);
          return out;
        }
        if (tag == 1 || tag == 3)
          return rt_tag_v(value);
        return value;
      }
      /*
       * Raw magic-tbuf handles are not managed heap pointers, so `is_ptr`
       * intentionally rejects them.  They are nevertheless dynamic
       * container values and must cross an `any` read unchanged; tagging the
       * address turns it into a bogus integer and breaks nested lists.
       */
      return ny_native_box_tbuf_any(value);
    }
  }
  int64_t seq_tag = rt_sequence_tag(buffer);
  if (seq_tag == TAG_LIST || seq_tag == TAG_TUPLE) {
    int64_t raw = rt_load_item_fast(buffer, index);
    return ny_native_box_tbuf_any(raw);
  }
  if (seq_tag == TAG_RANGE)
    return rt_load_item_any(buffer, rt_tag_v(index));
  /*
   * Unknown `any` receivers can still be dictionaries, strings, ranges, or
   * managed sequences. Delegate those cases to the generic accessor.
   */
  int64_t result = rt_value_get_tagged(buffer, index, fallback);
  /* Unknown receivers are a miss, but the public any accessor still returns
   * the fallback in dynamic representation.  Normalize only the miss so
   * valid pointer/string/container results are left untouched. */
  return result == fallback ? ny_native_box_tbuf_any(result) : result;
}

int64_t rt_range_index_read_raw(int64_t range, int64_t index);

int64_t rt_tbuf_index_read_raw(int64_t buffer, int64_t index) {
  if (!buffer) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  if (NY_NATIVE_IS(buffer) && NY_NATIVE_DECODE(buffer) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(buffer);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      buffer = decoded;
  }
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index >= 0 && index < count) {
        if (elem_size == 8)
          return ((const int64_t *)(uintptr_t)buffer)[index];
        unsigned char *data = (unsigned char *)(uintptr_t)buffer;
        int64_t value = 0;
        int64_t tag = 0;
        memcpy(&value, data + (size_t)index * (size_t)elem_size,
               (size_t)elem_size < sizeof(value) ? (size_t)elem_size
                                                 : sizeof(value));
        if (elem_size == 24) {
          memcpy(&tag, data + (size_t)index * (size_t)elem_size + 16,
                 sizeof(tag));
          if (tag == TAG_FLOAT && !is_v_flt(value))
            return rt_flt_box_val(value);
        }
        return value;
      }
      rt_panic(rt_alloc_string("index_read out of range"));
      return 0;
    }
  }
  if (rt_native_is_str(buffer) || is_v_str(buffer)) {
    return rt_cstr_index_read_raw(buffer, index);
  }
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
      if (index < 0 && n >= 0 && index >= -n)
        index += n;
      if (index >= 0 && index < n)
        return *(int64_t *)((char *)(uintptr_t)buffer + 16 + (size_t)index * 8);
      rt_panic(rt_alloc_string("index_read out of range"));
      return 0;
    }
    if (tag == TAG_RANGE) {
      return rt_range_index_read_raw(buffer, index);
    }
    if (tag == TAG_BYTES) {
      return rt_bytes_index_read_raw(buffer, index);
    }
  }
  if (rt_sequence_tag(buffer) == TAG_RANGE) {
    return rt_range_index_read_raw(buffer, index);
  }
  rt_panic(rt_alloc_string("index_read out of range"));
  return 0;
}

/*
 * Dynamic callback/index boundary. Typed native reads return raw payloads;
 * this companion preserves the strict index error contract while using the
 * tbuf element width/tag metadata to produce a canonical any value.
 */
static int64_t rt_tbuf_index_any_impl(int64_t buffer, int64_t index) {
  if (!buffer) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index < 0 || index >= count || elem_size < 8) {
        rt_panic(rt_alloc_string("index_read out of range"));
        return 0;
      }
      unsigned char *slot = (unsigned char *)(uintptr_t)buffer +
                            (size_t)index * (size_t)elem_size;
      int64_t value = 0;
      memcpy(&value, slot,
             (size_t)elem_size < sizeof(value) ? (size_t)elem_size
                                               : sizeof(value));
      if (elem_size >= 24) {
        int64_t tag = 0;
        memcpy(&tag, slot + 16, sizeof(tag));
        if (tag == 0)
          return 0;
        if (tag == TAG_FLOAT)
          return is_v_flt(value) ? value : rt_flt_box_val(value);
        if (tag == 1 || tag == 3)
          return rt_tag_v(value);
        return value;
      }
      if (rt_native_is_str(value) || rt_value_tag(value) >= 100 ||
          is_v_flt(value))
        return value;
      return rt_tag_v(value);
    }
  }
  /*
   * Strings are valid sequence members at an `any` indexing boundary.  Their
   * bytes are already represented as canonical character values; route them
   * through the checked string accessor instead of treating the pointer as an
   * unknown heap object.
   */
  if (rt_native_is_str(buffer) || is_v_str(buffer))
    return rt_cstr_index_read_raw(buffer, index);
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE || tag == TAG_RANGE) {
      /*
       * Managed sequence slots use the raw legacy payload layout. Convert
       * through the single any decoder so raw integers are tagged exactly
       * once, while pointers/bools/nil remain unchanged.
       */
      return ny_native_box_tbuf_any(rt_load_item_fast(buffer, index));
    }
  }
  rt_panic(rt_alloc_string("index_read out of range"));
  return 0;
}

int64_t rt_tbuf_index_any(int64_t buffer, int64_t index) {
  return rt_tbuf_index_any_impl(buffer,
                               is_int(index) ? rt_untag_v(index) : index);
}

int64_t rt_tbuf_index_any_raw(int64_t buffer, int64_t index) {
  return rt_tbuf_index_any_impl(buffer, index);
}

int64_t rt_index_key_error(void) {
  rt_panic(rt_alloc_string("index_read expects an integer index"));
  return 0;
}
int64_t rt_tbuf_tag(int64_t buffer, int64_t index) {
  if (!buffer || index < 0)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)hdr[0] != NY_NATIVE_TBUF_MAGIC)
    return 0;
  int64_t count = hdr[1], elem_size = hdr[2];
  if (index >= count)
    return 0;
  if (elem_size == 24) {
    unsigned char *data = (unsigned char *)(uintptr_t)buffer;
    int64_t tag = 0;
    memcpy(&tag, data + (size_t)index * 24u + 16u, sizeof(tag));
    return tag;
  }
  return 3;
}

int64_t rt_tbuf_contains(int64_t buffer, int64_t item, int64_t is_string) {
  if (!buffer)
    return 0;
  if (rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t count = hdr[1];
      for (int64_t i = 0; i < count; ++i) {
        int64_t v = rt_tbuf_get(buffer, i, 0);
        if (is_string || rt_native_is_str(item) || rt_native_is_str(v)) {
          if (rt_cstr_eq(v, item))
            return 1;
        } else if (v == item) {
          return 1;
        }
      }
      return 0;
    }
  }
  return 0;
}

double rt_value_to_f64(int64_t value, int64_t tag) {
  if (tag == TAG_FLOAT) {
    double result = 0.0;
    memcpy(&result, &value, sizeof(result));
    return result;
  }
  return tag == 3 ? (double)value : 0.0;
}
int64_t rt_tbuf_pop_raw(int64_t buffer) {
  if (!buffer)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)hdr[0] != NY_NATIVE_TBUF_MAGIC)
    return 0;
  int64_t count = hdr[1], elem_size = hdr[2];
  if (count <= 0 || elem_size <= 0)
    return 0;
  unsigned char *data = (unsigned char *)(uintptr_t)buffer;
  unsigned char *slot = data + (size_t)(count - 1) * (size_t)elem_size;
  int64_t value = 0;
  size_t copy =
      (size_t)elem_size < sizeof(value) ? (size_t)elem_size : sizeof(value);
  memcpy(&value, slot, copy);
  hdr[1] = count - 1;
  memset(slot, 0, (size_t)elem_size);
  return value;
}

int64_t rt_tbuf_to_cstr(int64_t buffer) {
  int64_t count = rt_tbuf_len_raw(buffer);
  if (count < 0 || count > (INT64_MAX - 3) / 4)
    return 0;
  /*
   * Descriptor lists (elem_size 24) carry a per-slot runtime tag at +16.
   * Format each slot by that tag: a tagged-int payload must be untagged
   * before printing, otherwise a list built through the dynamic append ABI
   * renders its encoded words ([3, 5, 7] for [1, 2, 3]).
   */
  int64_t elem_size = 8;
  {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (buffer && (uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      elem_size = hdr[2];
  }
  size_t cap = (size_t)count * 4 + 3;
  char *out = (char *)malloc(cap);
  if (!out)
    return 0;
  size_t used = 0;
  out[used++] = '[';
  for (int64_t i = 0; i < count; ++i) {
    if (i)
      out[used++] = ',';
    if (i)
      out[used++] = ' ';
    int64_t value = rt_tbuf_get(buffer, i, 0);
    int64_t slot_tag = 0;
    int is_string_slot = 0;
    if (elem_size >= 24) {
      memcpy(&slot_tag,
             (void *)((uintptr_t)buffer + (size_t)i * (size_t)elem_size + 16),
             sizeof(slot_tag));
      value = *(int64_t *)((uintptr_t)buffer + (size_t)i * (size_t)elem_size);
      if (slot_tag == 121 || rt_native_is_str(value) || is_v_str(value))
        is_string_slot = 1;
      else if (slot_tag == 1 && is_int(value))
        value = rt_untag_v(value);
      else if (slot_tag == TAG_FLOAT)
        value = rt_flt_box_val(value);
    }
    char item[64];
    int n;
    if (is_string_slot || rt_native_is_str(value) || is_v_str(value)) {
      const char *s = (const char *)(uintptr_t)rt_any_to_cstr(value);
      size_t len = s ? strnlen(s, 4096) : 0;
      if (used + len + 2 >= cap) {
        size_t next = cap;
        while (used + len + 2 >= next)
          next *= 2;
        char *grown = (char *)realloc(out, next);
        if (!grown) {
          free(out);
          return 0;
        }
        out = grown;
        cap = next;
      }
      out[used++] = '"';
      memcpy(out + used, s, len);
      used += len;
      out[used++] = '"';
    } else if (elem_size < 24 && is_v_flt(value)) {
      n = snprintf(item, sizeof(item), "%g", rt_flt_unbox_double(value));
      if (n < 0 || used + (size_t)n + 1 >= cap) {
        free(out);
        return 0;
      }
      memcpy(out + used, item, (size_t)n);
      used += (size_t)n;
    } else {
      n = snprintf(item, sizeof(item), "%lld", (long long)value);
      if (n < 0 || used + (size_t)n + 1 >= cap) {
        free(out);
        return 0;
      }
      memcpy(out + used, item, (size_t)n);
      used += (size_t)n;
    }
  }
  out[used++] = ']';
  out[used] = '\0';
  int64_t result = rt_alloc_string_len(out, used);
  free(out);
  return result;
}
int64_t rt_load8_raw(int64_t addr, int64_t idx) {
  if (addr == 0 || idx < 0 || (uint64_t)idx > UINTPTR_MAX - (uintptr_t)addr)
    return 0;
  return *(const uint8_t *)((uintptr_t)addr + (uintptr_t)idx);
}

int64_t rt_bytes_get_raw(int64_t buffer, int64_t index, int64_t fallback) {
  if (!buffer || !rt_header_readable_cached((uintptr_t)buffer - 16, 16))
    return fallback;
  int64_t tag = *(int64_t *)((uintptr_t)buffer - 8);
  int64_t length = *(int64_t *)((uintptr_t)buffer - 16);
  length = is_int(length) ? length >> 1 : length;
  if (index < 0 && length >= 0 && index >= -length)
    index += length;
  if (tag != TAG_BYTES || length < 0 || index >= length)
    return fallback;
  return *(const uint8_t *)((uintptr_t)buffer + (uintptr_t)index);
}

int64_t rt_bytes_index_read_raw(int64_t buffer, int64_t index) {
  if (!buffer || !rt_header_readable_cached((uintptr_t)buffer - 16, 16)) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  int64_t tag = *(int64_t *)((uintptr_t)buffer - 8);
  int64_t length = *(int64_t *)((uintptr_t)buffer - 16);
  length = is_int(length) ? length >> 1 : length;
  if (index < 0 && length >= 0 && index >= -length)
    index += length;
  if (tag != TAG_BYTES || length < 0 || index < 0 || index >= length) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  return *(const uint8_t *)((uintptr_t)buffer + (uintptr_t)index);
}

int64_t rt_range_index_read_raw(int64_t range, int64_t index) {
  int64_t heap =
      rt_sequence_tag(range) == TAG_RANGE ? range : rt_heap_object_ptr(range);
  if (!heap || *(int64_t *)((char *)(uintptr_t)heap - 8) != TAG_RANGE) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  int64_t start = *(int64_t *)(uintptr_t)heap;
  int64_t stop = *(int64_t *)((char *)(uintptr_t)heap + 8);
  int64_t step = *(int64_t *)((char *)(uintptr_t)heap + 16);
  int64_t count = 0;
  if (step > 0 && start < stop)
    count = (stop - start + step - 1) / step;
  else if (step < 0 && start > stop)
    count = (start - stop - step - 1) / (-step);
  if (index < 0 && count >= 0 && index >= -count)
    index += count;
  if (index < 0 || index >= count) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return 0;
  }
  return start + index * step;
}

int64_t rt_store8_raw(int64_t addr, int64_t idx, int64_t value) {
  if (addr == 0 || idx < 0 || (uint64_t)idx > UINTPTR_MAX - (uintptr_t)addr)
    return value;
  *(uint8_t *)((uintptr_t)addr + (uintptr_t)idx) = (uint8_t)value;
  return value;
}

int64_t rt_bytes_set_raw(int64_t buffer, int64_t index, int64_t value) {
  if (!buffer || index < 0 ||
      !rt_header_readable_cached((uintptr_t)buffer - 16, 16))
    return buffer;
  int64_t tag = *(int64_t *)((uintptr_t)buffer - 8);
  int64_t length = *(int64_t *)((uintptr_t)buffer - 16);
  length = is_int(length) ? length >> 1 : length;
  if (tag == TAG_BYTES && length >= 0 && index < length)
    *(uint8_t *)((uintptr_t)buffer + (uintptr_t)index) = (uint8_t)value;
  return buffer;
}

int64_t rt_cstr_len(int64_t value) {
  if (!value)
    return 0;
  if (is_v_str(value))
    return (int64_t)rt_tagged_str_len(value);
  return (int64_t)strlen((const char *)(uintptr_t)value);
}

int64_t rt_bool_to_cstr(int64_t value) {
  return (int64_t)(uintptr_t)(value ? "true" : "false");
}

typedef struct {
  int64_t key;
  int64_t value;
  uint64_t hash;
  uint8_t key_is_string;
  uint8_t control;
} ny_native_dict_slot_t;

/*
 * SwissTable-style control bytes: 0 is empty, 1 is a tombstone, and occupied
 * slots carry a 7-bit H2 fingerprint with the high bit set.  Keeping H2 in
 * the slot's existing padding avoids a second metadata allocation.
 */
#define NY_NATIVE_DICT_EMPTY 0u
#define NY_NATIVE_DICT_TOMBSTONE 1u
#define NY_NATIVE_DICT_H2(hash) ((uint8_t)(0x80u | ((hash) >> 57)))

typedef struct {
  uint64_t magic;
  int64_t tag;
  int64_t length;
  int64_t capacity;
  ny_native_dict_slot_t *slots;
} ny_native_dict_t;

#define NY_NATIVE_DICT_MAGIC UINT64_C(0x4e59444943544d47)

static bool rt_set_layout(int64_t value) {
  uintptr_t p = (uintptr_t)value;
  if (p < 4096 || p > UINTPTR_MAX - 16)
    return false;
  int64_t tag = 0, count = 0, capacity = 0;
  if (!rt_try_read_i64(p - 8, &tag) || tag != TAG_SET ||
      !rt_try_read_i64(p, &count) || !rt_try_read_i64(p + 8, &capacity))
    return false;
  if (count < 0 || capacity < 8 || capacity > (1LL << 30) ||
      (capacity & (capacity - 1)) != 0 || count > capacity)
    return false;
  if ((uint64_t)capacity > (SIZE_MAX - 24u) / 24u)
    return false;
  return rt_addr_readable_safe(p + 16, (size_t)capacity * 24u) != 0;
}

static ny_native_dict_t *ny_native_dict_ptr(int64_t value) {
  uintptr_t address = (uintptr_t)value;
  if (address < 4096 || address >= UINT64_C(0x0000800000000000) ||
      address < 16 ||
      !rt_header_readable_cached(address - 16, sizeof(ny_native_dict_t)) ||
      !rt_addr_readable_safe(address - 16, sizeof(ny_native_dict_t)))
    return NULL;
  ny_native_dict_t *dict = (ny_native_dict_t *)(uintptr_t)(address - 16);
  if (dict->magic != NY_NATIVE_DICT_MAGIC)
    return NULL;
  /*
   * A 48-bit magic can appear transiently in uninitialized or reused memory
   * (sets/records share the surrounding pool).  Only a real dict has a sane
   * power-of-two capacity and a non-negative length; require those before
   * classifying a value as a dict so a stray byte coincidence never turns a
   * list/set/range into TAG_DICT (which crashes reflect._is_vecdict).
   */
  if (dict->capacity < 8 || dict->capacity > (1LL << 30) ||
      (dict->capacity & (dict->capacity - 1)) != 0 || dict->length < 0)
    return NULL;
  return dict;
}

static uint64_t ny_native_dict_hash(int64_t key, bool is_string) {
  if (is_string && key != 0) {
    const unsigned char *s = (const unsigned char *)(uintptr_t)key;
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < 4096 && s[i]; ++i)
      h = (h ^ s[i]) * UINT64_C(1099511628211);
    return h;
  }
  uint64_t x = (uint64_t)key;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  return x;
}

static bool ny_native_dict_resize(ny_native_dict_t *dict, int64_t capacity) {
  if (!dict || capacity < 8 || (capacity & (capacity - 1)) != 0)
    return false;
  ny_native_dict_slot_t *slots =
      (ny_native_dict_slot_t *)calloc((size_t)capacity, sizeof(*slots));
  if (!slots)
    return false;
  ny_native_dict_slot_t *old = dict->slots;
  int64_t old_cap = dict->capacity;
  dict->slots = slots;
  dict->capacity = capacity;
  dict->length = 0;
  if (old) {
    uint64_t mask = (uint64_t)capacity - 1;
    for (int64_t i = 0; i < old_cap; ++i) {
      if (old[i].control < 0x80u)
        continue;
      uint64_t pos = old[i].hash & mask;
      while (slots[pos].control >= 0x80u)
        pos = (pos + 1) & mask;
      slots[pos] = old[i];
      dict->length++;
    }
    free(old);
  }
  return true;
}

int64_t rt_dict_new_raw(int64_t capacity) {
  if (capacity < 8)
    capacity = 8;
  int64_t actual = 8;
  while (actual < capacity && actual <= INT64_MAX / 2)
    actual *= 2;
  ny_native_dict_t *dict = (ny_native_dict_t *)calloc(1, sizeof(*dict));
  if (!dict || !ny_native_dict_resize(dict, actual)) {
    free(dict);
    return 0;
  }
  dict->magic = NY_NATIVE_DICT_MAGIC;
  /*
   * Keep a managed-compatible tag at d-8 and expose length at d[0].
   */
  dict->tag = TAG_DICT;
  return (int64_t)(uintptr_t)((char *)dict + 16);
}

/*
 * Native set layout matches std.core.set: the returned payload points at
 * count/capacity/24-byte slots and the runtime tag lives at payload - 8.
 */
int64_t rt_set_new(int64_t capacity) {
  if (capacity < 8)
    capacity = 8;
  int64_t actual = 8;
  while (actual < capacity && actual <= INT64_MAX / 2)
    actual *= 2;
  if ((uint64_t)actual > (SIZE_MAX - 24u) / 24u)
    return 0;
  size_t bytes = 8u + 16u + (size_t)actual * 24u;
  unsigned char *base = (unsigned char *)calloc(1, bytes);
  if (!base)
    return 0;
  int64_t *payload = (int64_t *)(base + 8u);
  payload[-1] = TAG_SET;
  payload[0] = 0;
  payload[1] = actual;
  return (int64_t)(uintptr_t)payload;
}

int64_t rt_set_remove(int64_t value, int64_t key) {
  if (!value || (uint64_t)value < 4096 ||
      !rt_header_readable_cached((uintptr_t)value - 8, 8) ||
      *(int64_t *)((char *)(uintptr_t)value - 8) != TAG_SET)
    return value;
  int64_t count = *(int64_t *)((char *)(uintptr_t)value + 0);
  int64_t cap = *(int64_t *)((char *)(uintptr_t)value + 8);
  if (is_int(count))
    count = rt_untag_v(count);
  if (is_int(cap))
    cap = rt_untag_v(cap);
  if (cap <= 0 || cap > (1LL << 30))
    return value;
  for (int64_t i = 0; i < cap; ++i) {
    char *slot = (char *)(uintptr_t)value + 16 + i * 24;
    int64_t state = *(int64_t *)(slot + 16);
    if (state != 1)
      continue;
    int64_t stored = *(int64_t *)slot;
    if (stored == key ||
        ((rt_native_is_str(stored) || is_v_str(stored)) &&
         (rt_native_is_str(key) || is_v_str(key)) && rt_cstr_eq(stored, key))) {
      *(int64_t *)slot = 0;
      *(int64_t *)(slot + 8) = 0;
      *(int64_t *)(slot + 16) = 2;
      *(int64_t *)((char *)(uintptr_t)value + 0) = count > 0 ? count - 1 : 0;
      return value;
    }
  }
  return value;
}

static bool ny_native_dict_key_equal(int64_t a, bool a_string, int64_t b,
                                     bool b_string) {
  if (a == b)
    return true;
  if (a_string != b_string)
    return false;
  if (!a || !b)
    return false;
  return a_string &&
         strcmp((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0;
}

static ny_native_dict_slot_t *ny_native_dict_find(ny_native_dict_t *dict,
                                                  int64_t key,
                                                  bool key_is_string,
                                                  bool *found) {
  if (found)
    *found = false;
  if (!dict || !dict->slots || dict->capacity <= 0)
    return NULL;
  uint64_t mask = (uint64_t)dict->capacity - 1;
  uint64_t hash = ny_native_dict_hash(key, key_is_string);
  uint8_t h2 = NY_NATIVE_DICT_H2(hash);
  uint64_t pos = hash & mask;
  ny_native_dict_slot_t *first_tomb = NULL;
  for (int64_t i = 0; i < dict->capacity; ++i) {
    ny_native_dict_slot_t *slot = &dict->slots[pos];
    if (slot->control == NY_NATIVE_DICT_EMPTY)
      return first_tomb ? first_tomb : slot;
    if (slot->control >= 0x80u) {
      if (slot->control == h2 && slot->hash == hash &&
          ny_native_dict_key_equal(slot->key, slot->key_is_string, key,
                                   key_is_string)) {
        if (found)
          *found = true;
        return slot;
      }
    } else if (slot->control == NY_NATIVE_DICT_TOMBSTONE && !first_tomb) {
      first_tomb = slot;
    }
    pos = (pos + 1) & mask;
  }
  return first_tomb;
}

static int64_t rt_dict_find_off_table(int64_t t, int64_t cap, int64_t key);

static bool ny_native_managed_dict_ptr(int64_t v) {
  int64_t tag = 0;
  return (uint64_t)v >= 4096 && rt_try_read_i64((uintptr_t)v - 8, &tag) &&
         tag == TAG_DICT;
}

/*
 * String-key native bridges receive raw C-string pointers.  Legacy managed
 * dictionaries store keys as tagged Ny strings, so materialize the lookup
 * key only at this compatibility boundary before using the managed table.
 */
static int64_t ny_native_managed_string_key(int64_t key) {
  if (is_v_str(key))
    return key;
  if (!key || !rt_addr_readable_safe((uintptr_t)key, 1))
    return 0;
  return rt_alloc_string((const char *)(uintptr_t)key);
}

static int64_t ny_native_managed_dict_get(int64_t d, int64_t key,
                                          int64_t fallback) {
  int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)d + 8);
  cap_raw = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
  int64_t t = *(int64_t *)((char *)(uintptr_t)d + 16);
  t = is_int(t) ? (t >> 1) : t;
  if (cap_raw <= 0 || (uint64_t)t < 4096)
    return fallback;
  int64_t off = rt_dict_find_off_table(t, cap_raw, key);
  if (off < 0 || *(int64_t *)((char *)(uintptr_t)t + off + 16) != 1)
    return fallback;
  return *(int64_t *)((char *)(uintptr_t)t + off + 8);
}

static int64_t ny_native_managed_dict_has(int64_t d, int64_t key) {
  int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)d + 8);
  cap_raw = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
  int64_t t = *(int64_t *)((char *)(uintptr_t)d + 16);
  t = is_int(t) ? (t >> 1) : t;
  if (cap_raw <= 0 || (uint64_t)t < 4096)
    return 0;
  int64_t off = rt_dict_find_off_table(t, cap_raw, key);
  return off >= 0 && *(int64_t *)((char *)(uintptr_t)t + off + 16) == 1;
}

static int64_t ny_native_managed_dict_set(int64_t d, int64_t key,
                                          int64_t item) {
  int64_t count = is_int(*(int64_t *)((char *)(uintptr_t)d + 0))
                      ? (*(int64_t *)((char *)(uintptr_t)d + 0) >> 1)
                      : *(int64_t *)((char *)(uintptr_t)d + 0);
  int64_t cap_raw = is_int(*(int64_t *)((char *)(uintptr_t)d + 8))
                        ? (*(int64_t *)((char *)(uintptr_t)d + 8) >> 1)
                        : *(int64_t *)((char *)(uintptr_t)d + 8);
  int64_t t = is_int(*(int64_t *)((char *)(uintptr_t)d + 16))
                  ? (*(int64_t *)((char *)(uintptr_t)d + 16) >> 1)
                  : *(int64_t *)((char *)(uintptr_t)d + 16);
  if (cap_raw <= 0 || (uint64_t)t < 4096)
    return d;
  int64_t off = rt_dict_find_off_table(t, cap_raw, key);
  if (off < 0)
    return d;
  int64_t state = *(int64_t *)((char *)(uintptr_t)t + off + 16);
  *(int64_t *)((char *)(uintptr_t)t + off) = key;
  *(int64_t *)((char *)(uintptr_t)t + off + 8) = item;
  *(int64_t *)((char *)(uintptr_t)t + off + 16) = 1;
  if (state != 1)
    *(int64_t *)((char *)(uintptr_t)d + 0) = count + 1;
  return d;
}

static int64_t ny_native_dict_get_impl(int64_t value, int64_t key,
                                       int64_t fallback, bool key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    if (key_is_string)
      key = ny_native_managed_string_key(key);
    return key ? ny_native_managed_dict_get(value, key, fallback) : fallback;
  }
  if (!dict && !key_is_string && (uint64_t)value >= 4096) {
    uint64_t *hdr = (uint64_t *)((uintptr_t)value - 24);
    int64_t count = (int64_t)hdr[0];
    int64_t stride = (int64_t)hdr[1];
    if ((stride == 8 || stride == 24) && key >= 0 && key < count) {
      if (stride == 8) {
        int64_t *elems = (int64_t *)(uintptr_t)value;
        return elems[key];
      } else {
        int64_t *elems = (int64_t *)(uintptr_t)value;
        return elems[key * 3];
      }
    }
  }
  if (!dict && ny_native_managed_dict_ptr(value))
    return ny_native_managed_dict_get(value, key, fallback);
  bool found = false;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found);
  if (!found || !slot)
    return fallback;
  /*
   * Native lowering keeps typed integer results raw in the slot, while the
   * public dictionary API returns dynamic values.  Box only scalar integers
   * here; pointers, strings, floats, and container handles already carry
   * their own representation.  Raw 0 is excluded because the dynamic ABI
   * reserves it for nil: a stored nil must never re-emerge as tagged int 0.
   */
  if (slot->value != 0 && !NY_DYNAMIC_CALLABLE_IS(slot->value) &&
      !is_int(slot->value) &&
      rt_native_is_int(slot->value) && !rt_is_bool_imm(slot->value))
    return rt_tag_v(slot->value);
  return slot->value;
}

static int64_t ny_native_dict_set_impl(int64_t value, int64_t key, int64_t item,
                                       bool key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    if (key_is_string)
      key = ny_native_managed_string_key(key);
    if (key)
      ny_native_managed_dict_set(value, key, item);
    return value;
  }
  if (!dict) {
    if (ny_native_managed_dict_ptr(value))
      ny_native_managed_dict_set(value, key, item);
    return value;
  }
  if ((dict->length + 1) * 10 >= dict->capacity * 7 &&
      !ny_native_dict_resize(dict, dict->capacity * 2))
    return value;
  bool found = false;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found);
  if (!slot)
    return value;
  if (!found) {
    slot->key = key;
    slot->key_is_string = key_is_string ? 1 : 0;
    slot->hash = ny_native_dict_hash(key, key_is_string);
    slot->control = NY_NATIVE_DICT_H2(slot->hash);
    dict->length++;
  }
  slot->value = item;
  return value;
}

int64_t rt_dict_get_raw(int64_t value, int64_t key, int64_t fallback) {
  /*
   * Frontend method resolution can conservatively spell `any.get` as the
   * dictionary bridge.  Preserve the raw return contract for compact native
   * lists instead of probing their header as a hash table.
   */
  if (!ny_native_dict_ptr(value) && rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      /*
       * Native NYIR get calls carry a raw index.  Do not apply the tagged
       * integer untag here: odd index 1 must remain 1, not become 0.
       */
      return rt_tbuf_get_any(value, key, fallback);
  }
  /*
   * `any.get` can be conservatively lowered through the dictionary bridge.
   * Legacy heap lists/tuples are not dictionaries, but they still implement
   * the same public get surface; route them through the generic sequence
   * accessor instead of returning the dictionary fallback.
   */
  int64_t heap_value = rt_heap_object_ptr(value);
  if (heap_value) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_value - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)heap_value + 0);
      int64_t count = is_int(len_v) ? rt_untag_v(len_v) : len_v;
      int64_t idx = key;
      if (idx < 0)
        idx += count;
      if (idx < 0 || idx >= count)
        return fallback;
      return rt_load_item_any(value, rt_tag_v(idx));
    }
  }
  return ny_native_dict_get_impl(value, key, fallback,
                                 rt_native_is_str(key) != 0 || is_v_str(key));
}
int64_t rt_dict_get_i64_raw(int64_t value, int64_t key, int64_t fallback) {
  return ny_native_dict_get_impl(value, key, fallback, false);
}
int64_t rt_dict_get_str_raw(int64_t value, int64_t key, int64_t fallback) {
  if (key == 0)
    return ny_native_dict_get_impl(value, key, fallback, false);
  return ny_native_dict_get_impl(value, key, fallback, true);
}

static int64_t ny_native_box_tbuf_any(int64_t value) {
  /*
   * Raw JIT string pointers may have an odd address and therefore satisfy the
   * low-bit integer test. Preserve them before applying dynamic scalar
   * boxing to native tbuf reads.
   */
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      return decoded;
  }
  if (rt_native_is_str(value) || is_v_str(value))
    return value;
  /*
   * Element-8 scalar buffers store unboxed integers.  Do not use the
   * tagged-integer low bit to classify them: odd raw values are valid data
   * too.  Small numeric payloads are the unambiguous scalar representation;
   * larger values are retained for raw pointer/bit payload callers.
   */
  if (value >= -4096 && value <= 4096)
    return rt_tag_v(value);
  return value;
}

int64_t rt_value_get_index_raw(int64_t value, int64_t index,
                               int64_t fallback) {
  /*
   * The result is a dynamic value even though the index is raw.
   */
  if (rt_magic_tbuf_elem_size(value) > 0)
    return rt_tbuf_get_any(value, index, fallback);
  return rt_value_get_tagged(value, rt_tag_v(index), fallback);
}

int64_t rt_value_get_tagged(int64_t value, int64_t key, int64_t fallback) {
  /*
   * Native tbuf handles may have an odd address and therefore look like a
   * tagged integer to the legacy predicate.  Resolve their representation
   * before rejecting an integer receiver.
   */
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      value = decoded;
  }
  if (rt_magic_tbuf_elem_size(value) > 0) {
    int64_t idx = is_int(key) ? rt_untag_v(key) : key;
    return rt_tbuf_get_any(value, idx, fallback);
  }
  /*
   * Legacy managed list/tuple handles can also be odd pointers.  Validate
   * the heap object before the low-bit integer test, otherwise a nested list
   * read through `any.get` is rejected as an integer receiver.
   */
  int64_t heap_value = rt_heap_object_ptr(value);
  if (heap_value) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_value - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)heap_value + 0);
      int64_t count = is_int(len_v) ? rt_untag_v(len_v) : len_v;
      int64_t idx = is_int(key) ? rt_untag_v(key) : key;
      if (idx < 0)
        idx += count;
      if (idx < 0 || idx >= count)
        return fallback;
      return rt_load_item_any(value, rt_tag_v(idx));
    }
  }
  /* Registered malloc handles are opaque storage, even when their first
   * bytes happen to form a readable C string. Check provenance before the
   * low-bit integer and heuristic string tests. */
  if (rt_raw_ptr_registered(value))
    return fallback;
  if (is_int(value)) {
    rt_panic(rt_alloc_string("get expects a string, bytes, list, tuple, dict, "
                             "range, or vector, got int"));
    return fallback;
  }
  if (!value)
    return fallback;
  /* A raw malloc pointer is not a sequence just because bytes before it
   * happen to resemble a tbuf header. Reject it before heuristic length
   * probing and preserve the dynamic fallback ABI. */
  if ((is_ptr(value) ||
       ((uint64_t)value > NY_VALUE_PTR_MIN_ADDR &&
        ((uint64_t)value & NY_VALUE_PTR_TAG_MASK) == 0)) &&
      !rt_heap_object_ptr(value) &&
      !rt_native_is_str(value) && !ny_native_dict_ptr(value) &&
      !rt_tbuf_known_handle((uintptr_t)value))
    return fallback;
  /* Some foreign/opaque pointers arrive with a non-canonical low-bit
   * spelling.  They are still never valid Ny sequences; the dynamic get
   * contract must treat them as a miss when a fallback was supplied. */
  if ((uint64_t)value > NY_VALUE_PTR_MIN_ADDR &&
      is_int(value) && !rt_heap_object_ptr(value) &&
      !rt_native_is_str(value) && !ny_native_dict_ptr(value) &&
      !rt_tbuf_known_handle((uintptr_t)value))
    return fallback;
  /*
   * Prefer an authoritative native-buffer header over the heuristic string
   * classifier.  Descriptor/list payloads can begin with readable bytes that
   * otherwise look like a C string to the raw-pointer compatibility path.
   */
  if (rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t idx = is_int(key) ? rt_untag_v(key) : key;
      int64_t got = rt_tbuf_get(value, idx, fallback);
      /*
       * Typed native list calls can pass an unboxed odd index.  If the
       * tagged interpretation misses but the raw spelling is in range,
       * retry it explicitly; this avoids guessing from low-bit parity alone
       * at the dynamic `.get` boundary.
       */
      if (got == fallback && is_int(key) && key >= 0 && key < hdr[1] &&
          key != idx)
        got = rt_tbuf_get(value, key, fallback);
      return hdr[2] == 8 ? ny_native_box_tbuf_any(got) : got;
    }
  }
  if (rt_native_is_str(value)) {
    int64_t idx = is_int(key) ? rt_untag_v(key) : key;
    int64_t len = rt_cstr_len(value);
    if (idx < 0)
      idx += len;
    if (idx < 0 || idx >= len)
      return fallback;
    return rt_cstr_get_raw(value, idx);
  }
  /*
   * Check managed dictionaries before the native-table probe.  Their payload
   * can contain words matching the native magic, which otherwise diverts
   * dynamic `any.get` calls into an incompatible layout.
   */
  if (ny_native_managed_dict_ptr(value)) {
    return rt_dict_get_raw(value, key, fallback);
  }
  if (ny_native_dict_ptr(value))
    return rt_dict_get_raw(value, key, fallback);
  if (rt_has_tag(value, rt_tag_v(TAG_LIST)) == NY_IMM_TRUE ||
      rt_tbuf_len_raw(value) > 0)
    {
      int64_t idx = is_int(key) ? rt_untag_v(key) : key;
      /*
       * Managed lists store scalar payloads in their legacy slots.  Cross
       * the dynamic boundary through the canonical decoder exactly once;
       * returning rt_tbuf_get directly would expose raw 99 as tagged 49.
       */
      if (rt_sequence_tag(value) == TAG_LIST ||
          rt_sequence_tag(value) == TAG_TUPLE)
        return rt_load_item_any(value, idx);
      return rt_tbuf_get(value, idx, fallback);
    }
  int64_t heap_v = rt_heap_object_ptr(value);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    if (tag == TAG_BYTES) {
      int64_t len = *(int64_t *)((char *)(uintptr_t)heap_v - 16);
      len = is_int(len) ? (len >> 1) : len;
      if (key < 0)
        key += len;
      if (key >= 0 && key < len)
        return (uint8_t)*(char *)((uintptr_t)heap_v + key);
      return fallback;
    }
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)heap_v + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)heap_v + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)heap_v + 16);
      int64_t len = 0;
      if (step > 0 && start < stop)
        len = (stop - start + step - 1) / step;
      else if (step < 0 && start > stop)
        len = (start - stop - step - 1) / (-step);
      if (key < 0)
        key += len;
      if (key >= 0 && key < len)
        return start + key * step;
      return fallback;
    }
  }
  /* Unknown/raw pointer receivers use the same miss ABI as the dynamic
   * caller.  The native lowering has already normalized literal defaults at
   * this boundary, so preserve that representation instead of tagging it a
   * second time. */
  return fallback;
}

int64_t rt_dict_set_raw(int64_t value, int64_t key, int64_t item) {
  return ny_native_dict_set_impl(value, key, item,
                                 rt_native_is_str(key) != 0 || is_v_str(key));
}
int64_t rt_dict_set_i64_raw(int64_t value, int64_t key, int64_t item) {
  return ny_native_dict_set_impl(value, key, item, false);
}
int64_t rt_native_dict_set_raw_i64(int64_t value, int64_t key, int64_t key_len,
                                   int64_t key_tag, int64_t item,
                                   int64_t value_len, int64_t value_tag) {
  (void)key_len;
  (void)key_tag;
  (void)value_len;
  (void)value_tag;
  return ny_native_dict_set_impl(value, key, item, false);
}
int64_t rt_native_dict_set_fast_i64(int64_t value, int64_t key, int64_t key_len,
                                    int64_t key_tag, int64_t item,
                                    int64_t value_len, int64_t value_tag) {
  (void)key_len;
  (void)value_len;
  (void)value_tag;
  bool string_key = key_tag == 121 || rt_native_is_str(key);
  return ny_native_dict_set_impl(value, key, item, string_key);
}
int64_t rt_native_dict_set_native_i64(int64_t value, int64_t key,
                                      int64_t item) {
  return ny_native_dict_set_impl(value, key, item, false);
}
int64_t rt_native_dict_set_nir_i64(int64_t value, int64_t key, int64_t item) {
  return ny_native_dict_set_impl(value, key, item, rt_native_is_str(key) != 0);
}
int64_t rt_value_set_tagged(int64_t value, int64_t key, int64_t item) {
  /*
   * Untyped native calls may carry a typed-buffer handle through the compact
   * native pointer encoding.  Decode only after validating the private tbuf
   * header; arbitrary tagged values must continue through the normal paths.
   */
  if (NY_NATIVE_IS(value)) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER)) {
      int64_t *hdr = (int64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER);
      if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
        value = decoded;
    }
  }
  int64_t heap = rt_heap_object_ptr(value);
  if (heap && *(int64_t *)((char *)(uintptr_t)heap - 8) == TAG_BYTES)
    return rt_bytes_set_raw(heap, key, item);
  /*
   * Native dictionaries also carry the language dict tag.  Dispatch them
   * first: the managed bridge has a different payload layout and silently
   * drops writes when it is selected for a native dict.
   */
  if (ny_native_dict_ptr(value)) {
    /*
     * Native lowering may pass a literal dictionary key as a raw C-string
     * pointer.  It is not yet a managed string handle, but it is still a
     * string key; treating it as an integer key makes indexed writes land in
     * a different hash domain from `.get("key", ...)`.  Distinguish raw
     * strings from boxed numbers/containers using the same pointer facts as
     * the managed compatibility path.
     */
    bool raw_string_key = false;
    if (!is_int(key) && !NY_NATIVE_IS(key) &&
        rt_addr_readable_safe((uintptr_t)key, 1)) {
      unsigned char first = *(const unsigned char *)(uintptr_t)key;
      /*
       * Raw source keys are C strings.  Boxed numeric/container handles are
       * also readable pointers, but their first payload byte is not a
       * printable string lead in the normal ABI.
       */
      raw_string_key = first == 0 || (first >= 0x20 && first < 0x7f);
    }
    int64_t out = (rt_native_is_str(key) || raw_string_key)
                      ? rt_native_dict_set_str_compact(value, key, item)
                      : rt_native_dict_set_native_i64(value, key, item);
    return out;
  }
  if (ny_native_managed_dict_ptr(value)) {
    /*
     * Legacy LLVM emits literal dictionary keys as raw C-string addresses;
     * those addresses are not always recognizable by the tagged-string
     * probe.  A managed heap object is a real Ny key, while a non-tagged,
     * headerless readable pointer is the compatibility C-string case.
     */
    if (rt_native_is_str(key) ||
        (!is_int(key) && !NY_NATIVE_IS(key) && !rt_heap_object_ptr(key)))
      key = ny_native_managed_string_key(key);
    if (key)
      ny_native_managed_dict_set(value, key, item);
    return value;
  }
  /*
   * Generic `any` callers use the tagged scalar ABI for their key/value
   * arguments, while compact tbuf storage is raw.  Normalize only at this
   * concrete container boundary.
   */
  if (rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return rt_tbuf_set_i64_raw(value, is_int(key) ? rt_untag_v(key) : key,
                                 rt_any_to_i64(item));
  }
  return rt_tbuf_set_i64_raw(value, key, item);
}
int64_t rt_native_dict_set_str_compact(int64_t value, int64_t key,
                                       int64_t item) {
  if (key == 0)
    return ny_native_dict_set_impl(value, key, item, false);
  /*
   * Managed dictionaries are exposed through the dynamic ABI.  Values that
   * arrive from typed/native producers are raw integers and must be boxed at
   * this boundary (notably process file descriptors); preserve bool
   * immediates, which are already canonical dynamic values.  Raw 0 stays
   * untouched: the dynamic ABI reserves it for nil, and a raw-int producer
   * that needs to store an unambiguous scalar 0 must box it before the call.
   */
  if (ny_native_managed_dict_ptr(value) && item != 0 &&
      !NY_DYNAMIC_CALLABLE_IS(item) && !is_int(item) &&
      rt_native_is_int(item) && !rt_is_bool_imm(item))
    item = rt_tag_v(item);
  return ny_native_dict_set_impl(value, key, item, true);
}

int64_t rt_dict_set_str_raw(int64_t value, int64_t key, int64_t item) {
  return rt_native_dict_set_str_compact(value, key, item);
}

int64_t rt_dict_merge_raw(int64_t value, int64_t other_value) {
  ny_native_dict_t *other = ny_native_dict_ptr(other_value);
  if (!other || !other->slots)
    return value;
  for (int64_t i = 0; i < other->capacity; ++i) {
    ny_native_dict_slot_t *slot = &other->slots[i];
    if (slot->control >= 0x80u) {
      ny_native_dict_set_impl(value, slot->key, slot->value,
                              slot->key_is_string != 0);
    }
  }
  return value;
}

int64_t rt_dict_clone_raw(int64_t value) {
  ny_native_dict_t *src = ny_native_dict_ptr(value);
  if (!src)
    return value;
  int64_t dst_val = rt_dict_new_raw(src->capacity);
  ny_native_dict_t *dst = ny_native_dict_ptr(dst_val);
  if (!dst || !dst->slots || !src->slots)
    return dst_val;
  memcpy(dst->slots, src->slots,
         (size_t)src->capacity * sizeof(ny_native_dict_slot_t));
  dst->length = src->length;
  return dst_val;
}

static int ny_vec_dim_impl(int64_t v) {
  if (!v || is_int(v) || is_v_flt(v))
    return 0;
  if (ny_native_dict_ptr(v) != NULL) {
    int64_t type_val = rt_dict_get_str_raw(v, (int64_t)(uintptr_t)"__type", 0);
    const char *s = (const char *)(uintptr_t)type_val;
    if (s) {
      if (strcmp(s, "vec2") == 0 || strcmp(s, "Vector2") == 0)
        return 2;
      if (strcmp(s, "vec3") == 0 || strcmp(s, "Vector3") == 0)
        return 3;
      if (strcmp(s, "vec4") == 0 || strcmp(s, "Vector4") == 0)
        return 4;
    }
  }
  return 0;
}

static double ny_vec_get_coord(int64_t v, const char *coord) {
  int64_t val = rt_dict_get_str_raw(v, (int64_t)(uintptr_t)coord, 0);
  return rt_any_to_f64(val);
}

static int64_t ny_vec_new_from_coords(int dim, double x, double y, double z,
                                      double w) {
  const char *tname = dim == 2 ? "vec2" : (dim == 3 ? "vec3" : "vec4");
  int64_t d = rt_dict_new_raw(8);
  rt_native_dict_set_str_compact(d, (int64_t)(uintptr_t)"__type",
                                 (int64_t)(uintptr_t)tname);
  rt_native_dict_set_str_compact(d, (int64_t)(uintptr_t)"x",
                                 rt_flt_box_double(x));
  rt_native_dict_set_str_compact(d, (int64_t)(uintptr_t)"y",
                                 rt_flt_box_double(y));
  if (dim >= 3)
    rt_native_dict_set_str_compact(d, (int64_t)(uintptr_t)"z",
                                   rt_flt_box_double(z));
  if (dim >= 4)
    rt_native_dict_set_str_compact(d, (int64_t)(uintptr_t)"w",
                                   rt_flt_box_double(w));
  return d;
}

double rt_vec_dot_raw(int64_t left, int64_t right) {
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  int dim = dim_l < dim_r ? dim_l : dim_r;
  if (dim <= 0)
    return 0.0;
  double dot = ny_vec_get_coord(left, "x") * ny_vec_get_coord(right, "x") +
               ny_vec_get_coord(left, "y") * ny_vec_get_coord(right, "y");
  if (dim >= 3)
    dot += ny_vec_get_coord(left, "z") * ny_vec_get_coord(right, "z");
  if (dim >= 4)
    dot += ny_vec_get_coord(left, "w") * ny_vec_get_coord(right, "w");
  return dot;
}

int64_t rt_vec_mul_scalar_raw(int64_t v, double s) {
  int dim = ny_vec_dim_impl(v);
  if (dim <= 0)
    return v;
  double x = ny_vec_get_coord(v, "x") * s;
  double y = ny_vec_get_coord(v, "y") * s;
  double z = dim >= 3 ? ny_vec_get_coord(v, "z") * s : 0.0;
  double w = dim >= 4 ? ny_vec_get_coord(v, "w") * s : 0.0;
  return ny_vec_new_from_coords(dim, x, y, z, w);
}

int64_t rt_vec_div_scalar_raw(int64_t v, double s) {
  int dim = ny_vec_dim_impl(v);
  if (dim <= 0 || s == 0.0)
    return v;
  double inv = 1.0 / s;
  double x = ny_vec_get_coord(v, "x") * inv;
  double y = ny_vec_get_coord(v, "y") * inv;
  double z = dim >= 3 ? ny_vec_get_coord(v, "z") * inv : 0.0;
  double w = dim >= 4 ? ny_vec_get_coord(v, "w") * inv : 0.0;
  return ny_vec_new_from_coords(dim, x, y, z, w);
}

int64_t rt_vec_add_raw(int64_t left, int64_t right) {
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  int dim = dim_l > dim_r ? dim_l : dim_r;
  if (dim <= 0)
    return left;
  double x = ny_vec_get_coord(left, "x") + ny_vec_get_coord(right, "x");
  double y = ny_vec_get_coord(left, "y") + ny_vec_get_coord(right, "y");
  double z = dim >= 3 ? ny_vec_get_coord(left, "z") + ny_vec_get_coord(right, "z") : 0.0;
  double w = dim >= 4 ? ny_vec_get_coord(left, "w") + ny_vec_get_coord(right, "w") : 0.0;
  return ny_vec_new_from_coords(dim, x, y, z, w);
}

int64_t rt_vec_sub_raw(int64_t left, int64_t right) {
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  int dim = dim_l > dim_r ? dim_l : dim_r;
  if (dim <= 0)
    return left;
  double x = ny_vec_get_coord(left, "x") - ny_vec_get_coord(right, "x");
  double y = ny_vec_get_coord(left, "y") - ny_vec_get_coord(right, "y");
  double z = dim >= 3 ? ny_vec_get_coord(left, "z") - ny_vec_get_coord(right, "z") : 0.0;
  double w = dim >= 4 ? ny_vec_get_coord(left, "w") - ny_vec_get_coord(right, "w") : 0.0;
  return ny_vec_new_from_coords(dim, x, y, z, w);
}

int64_t rt_vec_div_component_raw(int64_t left, int64_t right) {
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  int dim = dim_l > dim_r ? dim_l : dim_r;
  if (dim <= 0)
    return left;
  double rx = ny_vec_get_coord(right, "x");
  double ry = ny_vec_get_coord(right, "y");
  double rz = ny_vec_get_coord(right, "z");
  double rw = ny_vec_get_coord(right, "w");
  double x = rx != 0.0 ? ny_vec_get_coord(left, "x") / rx : 0.0;
  double y = ry != 0.0 ? ny_vec_get_coord(left, "y") / ry : 0.0;
  double z = dim >= 3 && rz != 0.0 ? ny_vec_get_coord(left, "z") / rz : 0.0;
  double w = dim >= 4 && rw != 0.0 ? ny_vec_get_coord(left, "w") / rw : 0.0;
  return ny_vec_new_from_coords(dim, x, y, z, w);
}

int64_t rt_any_mul(int64_t left, int64_t right) {
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_mul(rt_bigint_operand(left), rt_bigint_operand(right));
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  if (dim_l > 0 && dim_r > 0) {
    double dot = rt_vec_dot_raw(left, right);
    return rt_flt_box_double(dot);
  }
  if (dim_l > 0) {
    double s = rt_any_to_f64(right);
    return rt_vec_mul_scalar_raw(left, s);
  }
  if (dim_r > 0) {
    double s = rt_any_to_f64(left);
    return rt_vec_mul_scalar_raw(right, s);
  }
  if (is_int(left) && is_int(right)) {
    int64_t l = rt_untag_v(left);
    int64_t r = rt_untag_v(right);
    int64_t prod = 0;
    if (!__builtin_mul_overflow(l, r, &prod) && ny_small_int_fits_i64(prod))
      return rt_tag_v(prod);
    return rt_bigint_mul(left, right);
  }
  return 0;
}

int64_t rt_any_div(int64_t left, int64_t right) {
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_div(rt_bigint_operand(left), rt_bigint_operand(right));
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  if (dim_l > 0 && dim_r > 0)
    return rt_vec_div_component_raw(left, right);
  if (dim_l > 0) {
    double s = rt_any_to_f64(right);
    return rt_vec_div_scalar_raw(left, s);
  }
  if (is_int(left) && is_int(right)) {
    int64_t r = rt_untag_v(right);
    if (r == 0) {
      rt_panic(rt_alloc_string("division by zero"));
      return 0;
    }
    return rt_tag_v(rt_untag_v(left) / r);
  }
  return 0;
}

int64_t rt_any_mod(int64_t left, int64_t right) {
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_mod(rt_bigint_operand(left), rt_bigint_operand(right));
  if (is_int(left) && is_int(right)) {
    int64_t r = rt_untag_v(right);
    if (r == 0) {
      rt_panic(rt_alloc_string("modulo by zero"));
      return 0;
    }
    return rt_tag_v(rt_untag_v(left) % r);
  }
  return 0;
}


/*
 * The native representation keeps sequence values as raw magic-tbuf handles,
 * while any-return boundaries may box the same data as a legacy heap
 * TAG_LIST/TAG_TUPLE object (elements at +16, tagged length at +0).  The two
 * forms compare element-wise so an `any` list equals the identical raw list
 * (and vice versa), mirroring the cross-form string handling above.  Returns
 * -2 when either operand is not a recognizable sequence form.
 */
static int64_t rt_legacy_seq_count(int64_t v) {
  if (!is_ptr(v) || !is_heap_ptr(v))
    return -1;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
  if (tag != TAG_LIST && tag != TAG_TUPLE)
    return -1;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)v + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  return n >= 0 ? n : -1;
}

int64_t rt_magic_tbuf_elem_size(int64_t v) {
  if (!v || v < 4096)
    return -1;
  if (!rt_tbuf_known_handle((uintptr_t)v))
    return -1;
  if (!rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return -1;
  int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
  if ((uint64_t)hdr[0] != NY_NATIVE_TBUF_MAGIC || hdr[1] < 0 || hdr[2] <= 0)
    return -1;
  return hdr[2];
}

static int64_t rt_seq_value_eq(int64_t left, int64_t right) {
  int64_t lelem = rt_magic_tbuf_elem_size(left);
  int64_t relem = rt_magic_tbuf_elem_size(right);
  bool left_magic = lelem > 0;
  bool right_magic = relem > 0;
  int64_t lcount =
      left_magic ? *((int64_t *)((uintptr_t)left - RT_NATIVE_TBUF_HEADER) + 1)
                 : rt_legacy_seq_count(left);
  int64_t rcount =
      right_magic ? *((int64_t *)((uintptr_t)right - RT_NATIVE_TBUF_HEADER) + 1)
                  : rt_legacy_seq_count(right);
  if (lcount < 0 || rcount < 0)
    return -2;
  if (lcount != rcount)
    return NY_IMM_FALSE;
  for (int64_t i = 0; i < lcount; ++i) {
    int64_t vl = 0, vr = 0;
    int64_t tag_l = 0, tag_r = 0;
    if (left_magic) {
      unsigned char *d = (unsigned char *)(uintptr_t)left;
      size_t w = (size_t)lelem < sizeof(vl) ? (size_t)lelem : sizeof(vl);
      memcpy(&vl, d + (size_t)i * (size_t)lelem, w);
      if (lelem >= 24) {
        memcpy(&tag_l, d + (size_t)i * 24u + 16u, sizeof(tag_l));
        if (tag_l == TAG_FLOAT && !is_v_flt(vl))
          vl = rt_flt_box_val(vl);
      }
    } else {
      vl = *(int64_t *)((char *)(uintptr_t)left + 16 + (size_t)i * 8);
    }
    if (right_magic) {
      unsigned char *d = (unsigned char *)(uintptr_t)right;
      size_t w = (size_t)relem < sizeof(vr) ? (size_t)relem : sizeof(vr);
      memcpy(&vr, d + (size_t)i * (size_t)relem, w);
      if (relem >= 24) {
        memcpy(&tag_r, d + (size_t)i * 24u + 16u, sizeof(tag_r));
        if (tag_r == TAG_FLOAT && !is_v_flt(vr))
          vr = rt_flt_box_val(vr);
      }
    } else {
      vr = *(int64_t *)((char *)(uintptr_t)right + 16 + (size_t)i * 8);
    }
    bool l_str = tag_l == TAG_STR || tag_l == TAG_STR_CONST || tag_l == 121 ||
                 rt_native_is_str(vl) || is_v_str(vl);
    bool r_str = tag_r == TAG_STR || tag_r == TAG_STR_CONST || tag_r == 121 ||
                 rt_native_is_str(vr) || is_v_str(vr);
    if (l_str && r_str) {
      if (!rt_cstr_eq(vl, vr))
        return NY_IMM_FALSE;
      continue;
    }
    if (rt_any_eq(vl, vr) != NY_IMM_TRUE)
      return NY_IMM_FALSE;
  }
  return NY_IMM_TRUE;
}

int64_t rt_any_eq(int64_t left, int64_t right) {
  if (left == right)
    return NY_IMM_TRUE;
  if (!left || !right)
    return NY_IMM_FALSE;
  /* Structural equality must compare the payloads of compact native handles,
   * not their tagged pointer spellings. */
  if (NY_NATIVE_IS(left) && NY_NATIVE_DECODE(left) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(left);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      left = decoded;
  }
  if (NY_NATIVE_IS(right) && NY_NATIVE_DECODE(right) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(right);
    if (rt_header_readable_cached((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER) &&
        *(uint64_t *)((uintptr_t)decoded - RT_NATIVE_TBUF_HEADER) ==
            NY_NATIVE_TBUF_MAGIC)
      right = decoded;
  }
  if (rt_value_tag(left) == TAG_BIGINT ||
      rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_cmp_raw(rt_bigint_operand(left),
                                rt_bigint_operand(right)) == 0
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  /*
   * Dynamic values can mix raw native C-string pointers (packed list slots)
   * with managed/tagged Ny strings (any parameters).  Both are strings, but
   * the native classifier intentionally distinguishes their storage forms;
   * require either representation here before delegating to the length-aware
   * string comparator.
   */
  if ((rt_native_is_str(left) || is_v_str(left)) &&
      (rt_native_is_str(right) || is_v_str(right))) {
    return rt_cstr_eq(left, right) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  /*
   * Magic tbuf handles are raw, aligned addresses.  In particular they are
   * even, so the scalar predicate below can mistake them for native integers
   * before the generic pointer/sequence path gets a chance to inspect them.
   * Give container identity structural semantics first.
   */
  int64_t left_tbuf_elem = rt_magic_tbuf_elem_size(left);
  int64_t right_tbuf_elem = rt_magic_tbuf_elem_size(right);
  if (left_tbuf_elem > 0 && right_tbuf_elem > 0)
    return rt_tbuf_eq_raw(left, right);
  if (is_v_flt(left) || is_v_flt(right)) {
    if (!is_v_flt(left) || !is_v_flt(right))
      return NY_IMM_FALSE;
    return rt_flt_unbox_double(left) == rt_flt_unbox_double(right)
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  }
  if (rt_native_is_int(left) && rt_native_is_int(right)) {
    int64_t l = is_int(left) ? rt_untag_v(left) : left;
    int64_t r = is_int(right) ? rt_untag_v(right) : right;
    if (l == r)
      return NY_IMM_TRUE;
    /*
     * Native scalar returns can cross an `any` comparison without a boxing
     * instruction.  When one operand is raw and the other is tagged, compare
     * the raw spelling against the other's decoded payload as a compatibility
     * bridge; preserve ordinary tagged-vs-tagged inequality.
     */
    if ((!is_int(left) && is_int(right) && left == r) ||
        (is_int(left) && !is_int(right) && right == l))
      return NY_IMM_TRUE;
    return NY_IMM_FALSE;
  }
  /*
   * A range is a sequence value, not its backing object.  Native lowering
   * materializes ranges for iteration, so equality must use the same value
   * semantics as the interpreter when the other operand is a list/buffer.
   * Heap ranges use the ordinary object tag; rt_value_tag is deliberately
   * conservative for dynamic native values and can return zero here.
   */
  if (rt_sequence_tag(left) == TAG_RANGE) {
    int64_t values = rt_range_values_raw(left);
    return rt_any_eq(values, right);
  }
  if (rt_sequence_tag(right) == TAG_RANGE) {
    int64_t values = rt_range_values_raw(right);
    return rt_any_eq(left, values);
  }
  if ((uintptr_t)left >= 4096 && (uintptr_t)right >= 4096) {
    uintptr_t hdr_addr_l = (uintptr_t)left - RT_NATIVE_TBUF_HEADER;
    uintptr_t hdr_addr_r = (uintptr_t)right - RT_NATIVE_TBUF_HEADER;
    bool readable_l =
        rt_header_readable_cached(hdr_addr_l, RT_NATIVE_TBUF_HEADER);
    bool readable_r =
        rt_header_readable_cached(hdr_addr_r, RT_NATIVE_TBUF_HEADER);
    int64_t *hdr_l = readable_l ? (int64_t *)hdr_addr_l : NULL;
    int64_t *hdr_r = readable_r ? (int64_t *)hdr_addr_r : NULL;
    if (readable_l && readable_r &&
        (uint64_t)hdr_l[0] == NY_NATIVE_TBUF_MAGIC &&
        (uint64_t)hdr_r[0] == NY_NATIVE_TBUF_MAGIC)
      return rt_tbuf_eq_raw(left, right);
    ny_native_dict_t *dict_l = ny_native_dict_ptr(left);
    ny_native_dict_t *dict_r = ny_native_dict_ptr(right);
    if (dict_l && dict_r) {
      if (dict_l->length != dict_r->length)
        return NY_IMM_FALSE;
      for (int64_t i = 0; i < dict_l->capacity; ++i) {
        if (dict_l->slots[i].control >= 0x80u) {
          bool found = false;
          ny_native_dict_slot_t *s_r =
              ny_native_dict_find(dict_r, dict_l->slots[i].key,
                                  dict_l->slots[i].key_is_string != 0, &found);
          if (!found || !s_r ||
              rt_any_eq(dict_l->slots[i].value, s_r->value) != NY_IMM_TRUE)
            return NY_IMM_FALSE;
        }
      }
      return NY_IMM_TRUE;
    }
    /*
     * A boxed any list (legacy TAG_LIST/TAG_TUPLE) can be compared with a raw
     * native tbuf (or another boxed list) built from identical data; neither
     * operand is a magic tbuf here, so fall back to the cross-form sequence
     * comparison.  `-2` means no side is a recognizable sequence form, in
     * which case the comparison is not a list comparison.
     */
    int64_t seq_eq = rt_seq_value_eq(left, right);
    if (seq_eq != -2)
      return seq_eq;
  }
  return NY_IMM_FALSE;
}

static int64_t ny_native_dict_has_impl(int64_t value, int64_t key,
                                       bool key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    if (key_is_string)
      key = ny_native_managed_string_key(key);
    if (!key)
      return 0;
    return ny_native_managed_dict_has(value, key) ? 1 : 0;
  }
  bool found = false;
  (void)ny_native_dict_find(dict, key, key_is_string, &found);
  return found ? 1 : 0;
}
int64_t rt_dict_has_raw(int64_t value, int64_t key) {
  return ny_native_dict_has_impl(value, key, rt_native_is_str(key) != 0);
}
int64_t rt_dict_has_str_raw(int64_t value, int64_t key) {
  return ny_native_dict_has_impl(value, key, true);
}

int64_t rt_dict_len_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  return dict ? dict->length : 0;
}

int64_t rt_len(int64_t value) {
  if (!value)
    return 0;
  if (rt_native_is_str(value))
    return rt_cstr_len(value);
  if (is_v_str(value))
    return (int64_t)rt_tagged_str_len(value);
  if (rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return hdr[1];
  }
  if (rt_has_tag(value, rt_tag_v(TAG_LIST)) == NY_IMM_TRUE)
    return rt_tbuf_len_raw(value);
  if (rt_set_layout(value)) {
    int64_t count = *(int64_t *)((uintptr_t)value);
    return is_int(count) ? rt_untag_v(count) : count;
  }
  if (ny_native_dict_ptr(value) != NULL) {
    int64_t type_val =
        rt_dict_get_str_raw(value, (int64_t)(uintptr_t)"__type", 0);
    const char *s = (const char *)(uintptr_t)type_val;
    if (s) {
      if (strcmp(s, "vec2") == 0 || strcmp(s, "Vector2") == 0)
        return 2;
      if (strcmp(s, "vec3") == 0 || strcmp(s, "Vector3") == 0)
        return 3;
      if (strcmp(s, "vec4") == 0 || strcmp(s, "Vector4") == 0)
        return 4;
    }
    /* Native dictionaries can also carry the generic dict tag.  Read their
     * SwissTable count before the managed-dictionary header probe below. */
    return rt_dict_len_raw(value);
  }
  if (ny_native_managed_dict_ptr(value)) {
    int64_t count = *(int64_t *)(uintptr_t)value;
    return is_int(count) ? rt_untag_v(count) : count;
  }
  int64_t dict_len = rt_dict_len_raw(value);
  if (dict_len > 0)
    return dict_len;
  int64_t heap_v = rt_heap_object_ptr(value);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    if (tag == TAG_BYTES) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)heap_v - 16);
      return is_int(len_v) ? (len_v >> 1) : len_v;
    }
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)heap_v + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)heap_v + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)heap_v + 16);
      if (step == 0)
        return 0;
      if (step > 0) {
        if (stop <= start)
          return 0;
        return (stop - start + step - 1) / step;
      } else {
        if (stop >= start)
          return 0;
        return (start - stop - step - 1) / (-step);
      }
    }
  }
  return 0;
}

int64_t rt_len_strict(int64_t value) {
  /* Match the public len error contract for dynamic member access. */
  if (value == 0) {
    rt_panic(rt_alloc_string("len expects a sequence, got int"));
    return 0;
  }
  /* `any` erases the dictionary/list representation before this bridge is
   * selected.  Test authoritative container layouts before the low-bit
   * immediate-integer predicate; managed dictionaries are commonly odd
   * addresses and were being rejected as integers. */
  if (ny_native_dict_ptr(value))
    return rt_len(value);
  if (ny_native_managed_dict_ptr(value)) {
    int64_t count = *(int64_t *)(uintptr_t)value;
    return is_int(count) ? rt_untag_v(count) : count;
  }
  if (rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER))
    return rt_len(value);
  int64_t heap = rt_heap_object_ptr(value);
  if (heap) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE || tag == TAG_BYTES ||
        tag == TAG_RANGE || tag == TAG_DICT || tag == TAG_SET)
      return rt_len(value);
  }
  if (is_int(value) && !rt_native_is_str(value) && !is_v_str(value)) {
    rt_panic(rt_alloc_string("len expects a sequence, got int"));
    return 0;
  }
  return rt_len(value);
}

int64_t rt_sequence_len_safe(int64_t value) {
  if (!value || is_int(value) || is_v_flt(value))
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return hdr[1];
  }
  if (ny_native_dict_ptr(value) != NULL) {
    return rt_len(value);
  }
  int64_t heap = rt_heap_object_ptr(value);
  if (heap) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap - 8);
    if (tag == TAG_BYTES || tag == TAG_RANGE || tag == TAG_DICT ||
        tag == TAG_SET || tag == TAG_STR || tag == TAG_STR_CONST ||
        tag == TAG_LIST || tag == TAG_TUPLE) {
      return rt_len(value);
    }
    if ((value & 1) && rt_addr_readable_safe((uintptr_t)value, 1))
      return (int64_t)strlen((const char *)(uintptr_t)value);
  }
  if (rt_native_is_str(value))
    return (int64_t)strlen((const char *)(uintptr_t)value);
  return 0;
}

int64_t rt_sequence_len_raw(int64_t value) {
  if (is_int(value)) {
    rt_panic(rt_alloc_string("len expects sequence or collection, got int"));
    return 0;
  }
  if (!value) {
    rt_panic(rt_alloc_string("len expects sequence or collection, got nil"));
    return 0;
  }
  /*
   * Sets are collection values with a count/capacity header rather than a
   * sequence buffer.  Dynamic `.len` must expose that count instead of
   * falling through to the zero-length dictionary/string compatibility path.
   */
  if (rt_set_layout(value)) {
    int64_t count = *(int64_t *)((uintptr_t)value);
    return is_int(count) ? rt_untag_v(count) : count;
  }
  int64_t len = rt_sequence_len_safe(value);
  if (len > 0)
    return len;
  if (rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return hdr[1];
  }
  if (ny_native_dict_ptr(value) != NULL || rt_native_is_str(value))
    return 0;
  rt_panic(rt_alloc_string("len expects sequence or collection"));
  return 0;
}

static int64_t ny_native_dict_delete_impl(int64_t value, int64_t key,
                                          bool key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  bool found = false;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found);
  if (found && slot) {
    slot->control = NY_NATIVE_DICT_TOMBSTONE;
    slot->key = 0;
    slot->value = 0;
    slot->key_is_string = 0;
    dict->length--;
  }
  return value;
}
int64_t rt_dict_delete_raw(int64_t value, int64_t key) {
  return ny_native_dict_delete_impl(
      value, key, rt_native_is_str(key) != 0 || is_v_str(key));
}
int64_t rt_dict_delete_str_raw(int64_t value, int64_t key) {
  return ny_native_dict_delete_impl(value, key, true);
}

int64_t rt_dict_clear_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict || !dict->slots)
    return value;
  memset(dict->slots, 0, (size_t)dict->capacity * sizeof(*dict->slots));
  dict->length = 0;
  return value;
}

int64_t rt_dict_keys_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    int64_t n = *(int64_t *)((char *)(uintptr_t)value + 0);
    int64_t cap = *(int64_t *)((char *)(uintptr_t)value + 8);
    int64_t table = *(int64_t *)((char *)(uintptr_t)value + 16);
    if (is_int(n))
      n = n >> 1;
    if (is_int(cap))
      cap = cap >> 1;
    if (is_int(table))
      table = table >> 1;
    int64_t out = rt_tbuf_new_raw(n, 24);
    if (!out || n <= 0 || cap <= 0 || (uint64_t)table < 4096)
      return out;
    int64_t pos = 0;
    for (int64_t i = 0; i < cap && pos < n; ++i) {
      int64_t off = i * 24;
      if (*(int64_t *)((char *)(uintptr_t)table + off + 16) == 1) {
        int64_t key = *(int64_t *)((char *)(uintptr_t)table + off);
        char *elem = (char *)(uintptr_t)out + pos * 24;
        *(int64_t *)elem = key;
        *(int64_t *)(elem + 8) = rt_native_is_str(key) ? rt_cstr_len(key) : 0;
        *(int64_t *)(elem + 16) = rt_value_tag(key);
        ++pos;
      }
    }
    return out;
  }
  int64_t len = dict ? dict->length : 0;
  int64_t list_val = rt_tbuf_new_raw(len, 24);
  if (!dict || len <= 0 || !list_val)
    return list_val;
  int64_t pos = 0;
  for (int64_t i = 0; i < dict->capacity && pos < len; ++i) {
    if (dict->slots[i].control >= 0x80u) {
      int64_t k = dict->slots[i].key;
      bool is_s = dict->slots[i].key_is_string;
      int64_t tag = is_s ? 121 : rt_value_tag(k);
      int64_t k_len = is_s ? rt_cstr_len(k) : 0;
      char *elem = (char *)(uintptr_t)list_val + pos * 24;
      *(int64_t *)elem = k;
      *(int64_t *)(elem + 8) = k_len;
      *(int64_t *)(elem + 16) = tag;
      pos++;
    }
  }
  return list_val;
}

int64_t rt_dict_values_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  int64_t len = dict ? dict->length : 0;
  int64_t list_val = rt_tbuf_new_raw(len, 24);
  if (!dict || len <= 0 || !list_val)
    return list_val;
  int64_t pos = 0;
  for (int64_t i = 0; i < dict->capacity && pos < len; ++i) {
    if (dict->slots[i].control >= 0x80u) {
      int64_t v = dict->slots[i].value;
      bool is_s = rt_native_is_str(v);
      int64_t tag = is_s ? 121 : rt_value_tag(v);
      /*
       * Dictionary slots use the public tagged scalar ABI, while descriptor
       * tbufs store a raw payload plus an explicit tag.  Decode exactly once
       * at this boundary; copying `21` for integer `10` makes enumeration
       * expose the encoding instead of the value.
       */
      if (!is_s && is_int(v)) {
        v = rt_untag_v(v);
        tag = 1;
      }
      int64_t v_len = (rt_native_is_str(v) || is_v_str(v)) ? rt_cstr_len(v) : 0;
      char *elem = (char *)(uintptr_t)list_val + pos * 24;
      *(int64_t *)elem = v;
      *(int64_t *)(elem + 8) = v_len;
      *(int64_t *)(elem + 16) = tag;
      pos++;
    }
  }
  return list_val;
}

int64_t rt_dict_items_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  int64_t len = dict ? dict->length : 0;
  int64_t out = rt_tbuf_new_raw(len, 8);
  if (!dict || len <= 0 || !out)
    return out;
  int64_t pos = 0;
  for (int64_t i = 0; i < dict->capacity && pos < len; ++i) {
    ny_native_dict_slot_t *slot = &dict->slots[i];
    if (slot->control < 0x80u)
      continue;
    int64_t pair = rt_tbuf_new_raw(2, 24);
    if (!pair)
      return out;
    int64_t k = slot->key;
    bool is_s = slot->key_is_string;
    int64_t k_tag = is_s ? 121 : rt_value_tag(k);
    int64_t k_len = is_s ? rt_cstr_len(k) : 0;
    char *elem0 = (char *)(uintptr_t)pair;
    *(int64_t *)elem0 = k;
    *(int64_t *)(elem0 + 8) = k_len;
    *(int64_t *)(elem0 + 16) = k_tag;

    int64_t v = slot->value;
    bool v_is_s = rt_native_is_str(v);
    int64_t v_tag = v_is_s ? 121 : rt_value_tag(v);
    if (!v_is_s && is_int(v)) {
      v = rt_untag_v(v);
      v_tag = 1;
    }
    int64_t v_len = (v_is_s || is_v_str(v)) ? rt_cstr_len(v) : 0;
    char *elem1 = (char *)(uintptr_t)pair + 24;
    *(int64_t *)elem1 = v;
    *(int64_t *)(elem1 + 8) = v_len;
    *(int64_t *)(elem1 + 16) = v_tag;

    ((int64_t *)(uintptr_t)out)[pos++] = pair;
  }
  return out;
}

int64_t rt_contains_raw(int64_t container, int64_t item) {
  if (is_int(container)) {
    rt_panic(rt_alloc_string("contains expects a string, list, tuple, dict, "
                             "set, range, or vector, got int"));
    return NY_IMM_FALSE;
  }
  if (!container)
    return NY_IMM_FALSE;
  if ((rt_native_is_str(container) || is_v_str(container)) &&
      (rt_native_is_str(item) || is_v_str(item)))
    return strstr((const char *)(uintptr_t)container,
                  (const char *)(uintptr_t)item)
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  if ((rt_value_tag(container) == TAG_STR ||
       rt_value_tag(container) == TAG_STR_CONST) &&
      (rt_value_tag(item) == TAG_STR ||
       rt_value_tag(item) == TAG_STR_CONST))
    return strstr((const char *)(uintptr_t)container,
                  (const char *)(uintptr_t)item)
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  if (ny_native_dict_ptr(container) != NULL) {
    return rt_dict_has_raw(container, item);
  }
  /*
   * Prefer the managed set header over a speculative tbuf-header probe.
   */
  int64_t managed_v = rt_heap_object_ptr(container);
  int64_t managed_tag = 0;
  if (!managed_v && is_ptr(container) &&
      (((uint64_t)container) & NY_VALUE_PTR_TAG_MASK) == 0 &&
      rt_try_read_i64((uintptr_t)container - 8, &managed_tag) &&
      managed_tag == TAG_SET)
    managed_v = container;
  if (managed_v &&
      (managed_tag == TAG_SET ||
       *(int64_t *)((char *)(uintptr_t)managed_v - 8) == TAG_SET)) {
    int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)managed_v + 8);
    int64_t cap = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
    for (int64_t i = 0; i < cap; ++i) {
      int64_t off = 16 + i * 24;
      int64_t st = *(int64_t *)((char *)(uintptr_t)managed_v + off + 16);
      if (st == 1 || st == 3) {
        int64_t k = *(int64_t *)((char *)(uintptr_t)managed_v + off);
        if (rt_cstr_eq(k, item) || k == item)
          return NY_IMM_TRUE;
      }
    }
    return NY_IMM_FALSE;
  }
  if (rt_header_readable_cached((uintptr_t)container - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)container - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return rt_tbuf_contains(container, item, rt_native_is_str(item))
                 ? NY_IMM_TRUE
                 : NY_IMM_FALSE;
    }
  }
  int64_t heap_v = rt_heap_object_ptr(container);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    if (tag == TAG_SET) {
      int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)heap_v + 8);
      int64_t cap = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
      for (int64_t i = 0; i < cap; ++i) {
        int64_t off = 16 + i * 24;
        int64_t st = *(int64_t *)((char *)(uintptr_t)heap_v + off + 16);
        if (st == 1 || st == 3) {
          int64_t k = *(int64_t *)((char *)(uintptr_t)heap_v + off);
          if (rt_cstr_eq(k, item) || k == item)
            return NY_IMM_TRUE;
        }
      }
      return NY_IMM_FALSE;
    }
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)heap_v + 0);
      int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
      for (int64_t i = 0; i < n; ++i) {
        int64_t elem = *(int64_t *)((char *)(uintptr_t)heap_v + 16 + i * 8);
        if (rt_native_is_str(item) ? rt_cstr_eq(elem, item) : (elem == item))
          return NY_IMM_TRUE;
      }
      return NY_IMM_FALSE;
    }
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)heap_v + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)heap_v + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)heap_v + 16);
      int64_t val = is_int(item) ? (item >> 1) : item;
      if (step > 0 && val >= start && val < stop && ((val - start) % step) == 0)
        return NY_IMM_TRUE;
      if (step < 0 && val <= start && val > stop &&
          ((start - val) % (-step)) == 0)
        return NY_IMM_TRUE;
      return NY_IMM_FALSE;
    }
  }
  return NY_IMM_FALSE;
}

int g_trace_requested = 0;
int g_trace_suspended = 0;
static int g_trace_env_ready = 0;
static bool g_trace_env_trace = false;
static bool g_trace_env_calls = false;
static bool g_trace_env_values = false;
static bool g_trace_env_verbose = false;
static bool g_trace_env_index_read = false;
static char g_trace_env_filter_buf[256] = {0};
static const char *g_trace_env_filter = NULL;

#ifndef _WIN32
#define NY_JMP_BUF jmp_buf
#define NY_SETJMP(env) _setjmp(env)
#define NY_LONGJMP(env, val) _longjmp(env, val)
#else
#define NY_JMP_BUF jmp_buf

#define NY_SETJMP(env) setjmp(env)
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
    rt_map_oracle_add((uintptr_t)block, sizeof(*block));
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
  /*
   * Clear the intern table and hash cache before freeing the SSO blocks
   * they point into, so no stale pointer can survive the free.
   */
  rt_str_intern_clear();
  while (block) {
    rt_sso_block_t *next = block->next;
    free(block);
    block = next;
  }
}

#define CALL_STACK_MAX 512
static NY_TLS int64_t g_cs_files[CALL_STACK_MAX];
static NY_TLS int64_t g_cs_lines[CALL_STACK_MAX];
static NY_TLS int64_t g_cs_funcs[CALL_STACK_MAX];
static NY_TLS size_t g_cs_depth = 0;

int64_t rt_trace_dump(int64_t count);

void print_trace_entry(int64_t file, int64_t line, int64_t col, int64_t func,
                       const char *prefix) {
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

  fprintf(stderr, "%s%s%.*s:%s%ld:%ld%s", pre, c1, (int)flen, fname, c2,
          (long)l, (long)c, rs);
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
         trace_func_matches(func, "std.core.len") ||
         trace_func_matches(func, "std.core.get") ||
         trace_func_matches(func, "std.core.put") ||
         trace_func_matches(func, "std.core.append") ||
         trace_func_matches(func, "std.core.pop") ||
         trace_func_matches(func, "std.core.extend") ||
         trace_func_matches(func, "std.core.contains") ||
         trace_func_matches(func, "std.core.slice") ||
         trace_func_matches(func, "std.core.error.panic");
}

#define RT_PRINT_BUF_SIZE 8192
static char rt_print_buf[RT_PRINT_BUF_SIZE];
static uint32_t rt_print_pos = 0;
static int rt_stdout_is_tty = -1;

#ifdef _WIN32
extern int64_t rt_write_stdout_console(const char *ptr, size_t len);
#endif

int64_t rt_print_flush(void) {
  if (rt_print_pos > 0) {
#ifdef _WIN32
    if (rt_write_stdout_console(rt_print_buf, (size_t)rt_print_pos) >= 0) {
      rt_print_pos = 0;
      fflush(stdout);
      return rt_tag_v(1);
    }
#endif
    fwrite(rt_print_buf, 1, (size_t)rt_print_pos, stdout);
    rt_print_pos = 0;
  }
  fflush(stdout);
  return rt_tag_v(1);
}

static inline void rt_maybe_flush_line(void) {
#ifdef _WIN32
  if (rt_stdout_is_tty < 0)
    rt_stdout_is_tty = _isatty(_fileno(stdout));
#else
  if (rt_stdout_is_tty < 0)
    rt_stdout_is_tty = isatty(fileno(stdout));
#endif
  (void)rt_stdout_is_tty;
  rt_print_flush();
}

static inline void rt_print_put(const char *s, size_t len) {
  if (rt_print_pos + len > RT_PRINT_BUF_SIZE) {
    rt_print_flush();
    if (len > RT_PRINT_BUF_SIZE) {
#ifdef _WIN32
      if (rt_write_stdout_console(s, len) >= 0) {
        fflush(stdout);
        return;
      }
#endif
      fwrite(s, 1, len, stdout);
      fflush(stdout);
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
    extern int64_t rt_write_off(int64_t fd, int64_t buf, int64_t len,
                                int64_t off);
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

/*
 * Pure-native path: null-terminated C string pointer (no Nytrix string
 * header).
 */
int64_t rt_print_cstr(int64_t p) {
  if (!p)
    return 0;
  const char *s = (const char *)(uintptr_t)p;
  size_t len = 0;
  while (s[len])
    ++len;
  rt_print_put(s, len);
  return p;
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

int64_t rt_print_i64_raw(int64_t val) {
  if (rt_print_pos + 24 >= RT_PRINT_BUF_SIZE)
    rt_print_flush();

  if (val == 0) {
    rt_print_buf[rt_print_pos++] = '0';
    return val;
  }
  char *start = rt_print_buf + rt_print_pos;
  uint64_t abs_v;
  if (val < 0) {
    *start++ = '-';
    abs_v = 0 - (uint64_t)val;
  } else {
    abs_v = (uint64_t)val;
  }
  char tmp[24];
  char *p = tmp + sizeof(tmp);
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
  return val;
}

int64_t rt_print_f64_raw(double d) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", d);
  if (n < 0)
    return 0;
  if ((size_t)n >= sizeof(buf))
    n = (int)sizeof(buf) - 1;
  rt_print_put(buf, (size_t)n);
  return 0;
}

int64_t rt_print_int(int64_t v) {
  rt_print_i64_raw((int64_t)(v >> 1));
  return v;
}

/*
 * Type-dispatched print: handles every Nytrix value correctly.
 * Used by the native backend and any path where the static type is
 * unknown.
 */
int64_t rt_print_value(int64_t v) {
  /*
   * nil
   */
  if (rt_is_nil_imm(v)) {
    rt_print_put("nil", 3);
    return v;
  }
  /*
   * booleans
   */
  if (rt_is_true_imm(v)) {
    rt_print_put("true", 4);
    return v;
  }
  if (rt_is_false_imm(v)) {
    rt_print_put("false", 5);
    return v;
  }
  /*
   * tagged integer — fast inline path
   */
  if (is_int(v)) {
    return rt_print_int(v);
  }
  /*
   * string
   */
  if (is_v_str(v)) {
    return rt_print_str_raw(v);
  }
  /*
   * pointer-tagged objects
   */
  if (is_ptr(v)) {
    if (is_v_flt(v)) {
      double d;
      memcpy(&d, (const void *)(uintptr_t)v, 8);
      char buf[64];
      int n = snprintf(buf, sizeof(buf), "%g", d);
      rt_print_put(buf, (size_t)n);
      return v;
    }
    if (is_heap_ptr(v)) {
      int64_t tag = *(int64_t *)((char *)(uintptr_t)v - 8);
      if (tag == TAG_COMPLEX) {
        double re = 0.0, im = 0.0;
        memcpy(&re, (const void *)(uintptr_t)v, 8);
        memcpy(&im, (const void *)((uintptr_t)v + 8), 8);
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "%g%+gi", re, im);
        rt_print_put(buf, (size_t)n);
        return v;
      }
      if (tag == TAG_BIGINT) {
        extern int64_t rt_bigint_to_str(int64_t);
        int64_t s = rt_bigint_to_str(v);
        if (s)
          rt_print_str_raw(s);
        return v;
      }
      if (tag == TAG_BIGFLOAT) {
        extern int64_t rt_bigfloat_to_str(int64_t);
        int64_t s = rt_bigfloat_to_str(v);
        if (s)
          rt_print_str_raw(s);
        return v;
      }
      char buf[80];
      int n = snprintf(buf, sizeof(buf), "<ptr 0x%lx tag=%ld>",
                       (unsigned long)v, (long)tag);
      rt_print_put(buf, (size_t)n);
      return v;
    }
    /*
     * native/ffi pointer
     */
    if (NY_NATIVE_IS(v)) {
      char buf[64];
      int n = snprintf(buf, sizeof(buf), "<ffi_ptr 0x%lx>",
                       (unsigned long)(uintptr_t)NY_NATIVE_DECODE(v));
      rt_print_put(buf, (size_t)n);
      return v;
    }
    /*
     * function pointer
     */
    if ((v & 3) == 2) {
      char buf[64];
      int n =
          snprintf(buf, sizeof(buf), "<fn 0x%lx>", (unsigned long)(v & ~3ULL));
      rt_print_put(buf, (size_t)n);
      return v;
    }
  }
  /*
   * fallback
   */
  rt_print_put("nil", 3);
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
  int64_t existing = rt_str_intern_lookup(s, len);
  if (existing)
    return existing;
  if (len <= RT_SSO_MAX) {
    int64_t small = rt_alloc_small_string_len(s, len);
    if (small) {
      rt_str_intern_insert(s, len, small);
      return small;
    }
  }
  int64_t p = rt_malloc((int64_t)((len + 1) * sizeof(char) << 1) | 1);
  if (!p)
    return 0;
  *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
  *(int64_t *)((char *)(uintptr_t)p - 16) = ((int64_t)len << 1) | 1;
  memcpy((void *)(uintptr_t)p, s, len);
  ((char *)(uintptr_t)p)[len] = '\0';
  rt_str_intern_insert(s, len, p);
  return p;
}

int64_t rt_alloc_string(const char *s) {
  if (!s)
    return 0;
  return rt_alloc_string_len(s, strlen(s));
}

int64_t rt_cstr_to_str(int64_t p_v) {
  if (p_v == NY_IMM_NIL || p_v == 0)
    return NY_IMM_NIL;
  int64_t raw = p_v;
  if (NY_NATIVE_IS(p_v))
    raw = (int64_t)(uintptr_t)NY_NATIVE_DECODE(p_v);
  else if (is_int(p_v))
    raw = rt_untag_v(p_v);
  if (!raw || (uintptr_t)raw <= NY_VALUE_PTR_MIN_ADDR)
    return NY_IMM_NIL;
  /*
   * C-string conversion is an ABI boundary: callers may pass a raw pointer
   * from an untyped value.  Validate the first byte and require a bounded NUL
   * terminator before handing it to the managed string allocator.
   */
  if (!rt_addr_readable_safe((uintptr_t)raw, 1))
    return NY_IMM_NIL;
  size_t n = strnlen((const char *)(uintptr_t)raw, 1u << 20);
  if (n >= (1u << 20))
    return NY_IMM_NIL;
  return rt_alloc_string_len((const char *)(uintptr_t)raw, n);
}

static void trace_print_loc(void) {
  print_trace_entry(g_trace_file, g_trace_line, g_trace_col, g_trace_func,
                    "[trace] ");
}

static void trace_record(int64_t file, int64_t line, int64_t col,
                         int64_t func) {
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

static bool trace_master_enabled(void) {
  const char *all = getenv("NY_TRACE_ALL");
  if (all && *all && strcmp(all, "0") != 0)
    return true;
  return false;
}

void rt_trace_refresh_env(void) {
  g_trace_env_trace =
      trace_env_enabled_uncached("NYTRIX_TRACE") || trace_master_enabled();
  g_trace_env_calls = trace_env_enabled_uncached("NYTRIX_TRACE_CALLS") ||
                      trace_master_enabled();
  g_trace_env_values = trace_env_enabled_uncached("NYTRIX_TRACE_VALUES") ||
                       trace_master_enabled();
  g_trace_env_verbose = trace_env_enabled_uncached("NYTRIX_TRACE_VERBOSE") ||
                        trace_master_enabled();
  g_trace_env_index_read =
      trace_env_enabled_uncached("NYTRIX_INDEX_READ_PARITY");
  const char *filter = getenv("NYTRIX_TRACE_FILTER");
  if (filter && *filter) {
    snprintf(g_trace_env_filter_buf, sizeof(g_trace_env_filter_buf), "%s",
             filter);
    g_trace_env_filter = g_trace_env_filter_buf;
  } else {
    g_trace_env_filter_buf[0] = '\0';
    g_trace_env_filter = NULL;
  }
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
  g_index_read_probe_mode =
      trace_env_enabled("NYTRIX_INDEX_READ_PARITY") ? 1 : 0;
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
  fprintf(stderr, "[parity:index] tag=%" PRId64 " index=%" PRId64 " path=%s\n",
          raw_tag, raw_idx, path_name);
  return 0;
}

static bool trace_locations_enabled(void) {
  return trace_env_enabled("NYTRIX_TRACE_VERBOSE");
}

static bool trace_values_enabled(void) {
  return trace_env_enabled("NYTRIX_TRACE_VALUES");
}

static bool trace_calls_enabled(void) {
  return trace_env_enabled("NYTRIX_TRACE_CALLS") || trace_values_enabled() ||
         trace_locations_enabled();
}

static bool trace_filter_matches_token(const char *func, size_t fnlen,
                                       const char *tok, size_t toklen) {
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

/*
 * Runtime call tracing can visit the same helper thousands of times in
 * a tight loop.  Keep the first record, collapse only immediately
 * adjacent identical call records, and flush the count before a return
 * or a different call so call nesting remains readable.
 */
static int64_t g_trace_last_call_file;
static int64_t g_trace_last_call_line;
static int64_t g_trace_last_call_func;
static size_t g_trace_last_call_depth;
static size_t g_trace_call_repeats;
static bool g_trace_have_last_call;
static bool g_trace_call_flush_registered;

static void trace_flush_call_repeats(void) {
  if (!g_trace_have_last_call)
    return;
  if (g_trace_call_repeats) {
    trace_print_indent(g_trace_last_call_depth);
    fprintf(stderr, "[trace] ... previous call repeated x%zu\n",
            g_trace_call_repeats + 1);
  }
  g_trace_call_repeats = 0;
  g_trace_have_last_call = false;
}

static void trace_print_call(int64_t file, int64_t line, int64_t func,
                             size_t depth) {
  if (!g_trace_call_flush_registered) {
    atexit(trace_flush_call_repeats);
    g_trace_call_flush_registered = true;
  }
  bool same = g_trace_have_last_call && g_trace_last_call_depth == depth &&
              g_trace_last_call_file == file &&
              g_trace_last_call_line == line && g_trace_last_call_func == func;
  if (same) {
    ++g_trace_call_repeats;
    return;
  }
  if (g_trace_have_last_call)
    trace_flush_call_repeats();
  g_trace_have_last_call = true;
  g_trace_last_call_file = file;
  g_trace_last_call_line = line;
  g_trace_last_call_func = func;
  g_trace_last_call_depth = depth;
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
  trace_flush_call_repeats();
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

int64_t rt_globals_get(void) {
  if (!ny_native_dict_ptr(rt_globals_ptr))
    rt_globals_ptr = rt_dict_new_raw(16);
  return rt_globals_ptr;
}
int64_t rt_globals_set(int64_t p) {
  rt_globals_ptr = p;
  return p;
}

int64_t rt_fix_fn_ptr(int64_t fn) {
  if (!fn || !is_ptr(fn))
    return fn;
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
  fn = is_ptr(fn) ? rt_fix_fn_ptr(fn) : 0;
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
  size_t target_len =
      (size_t)(is_int(target_len_v) ? (target_len_v >> 1) : target_len_v);
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

int64_t rt_jmpbuf_size(void) { return rt_tag_v((int64_t)sizeof(NY_JMP_BUF)); }
int64_t rt_jmpbuf_align(void) {
  return rt_tag_v((int64_t)_Alignof(NY_JMP_BUF));
}
int64_t rt_get_panic_val(void) { return g_panic_value; }

int64_t rt_trace_loc(int64_t file, int64_t line, int64_t col) {
  g_trace_file = file;
  g_trace_line = line;
  g_trace_col = col;
  if (g_trace_suspended)
    return rt_tag_v(0);
  trace_record(file, line, col, g_trace_func);
  if (trace_locations_enabled() && is_v_str(g_trace_func) &&
      trace_should_print_func(g_trace_func))
    trace_print_loc();
  return rt_tag_v(0);
}

int64_t rt_trace_func(int64_t name) {
  g_trace_func = name;
  return rt_tag_v(0);
}

/*
 * Raw NYIR adapters for the diagnostics builtins.  The native path compares
 * their results against plain integers ("__trace_func(...) == 0"), while the
 * VM consumes the tagged variants above; untagging keeps both ABIs honest.
 */
int64_t rt_trace_func_raw(int64_t name) {
  return rt_untag_v(rt_trace_func(name));
}

int64_t rt_trace_loc_raw(int64_t file, int64_t line, int64_t col) {
  return rt_untag_v(rt_trace_loc(file, line, col));
}

int64_t rt_trace_enter(int64_t func, int64_t file, int64_t line);

int64_t rt_trace_enter_raw(int64_t func, int64_t file, int64_t line) {
  return rt_untag_v(rt_trace_enter(func, file, line));
}

int64_t rt_print_flush_raw(void) { return rt_untag_v(rt_print_flush()); }

int64_t rt_trace_last_file(void) { return g_trace_file; }
int64_t rt_trace_last_line(void) { return g_trace_line; }
int64_t rt_trace_last_col(void) { return g_trace_col; }
int64_t rt_trace_last_func(void) { return g_trace_func; }

static void print_rt_snippet(int64_t file_ptr, int64_t line_ptr,
                             int64_t col_ptr) {
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
    while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r' ||
                        buf[blen - 1] == ' ')) {
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
    return rt_tag_v(0);
  size_t want = (size_t)(is_int(count) ? rt_untag_v(count) : count);
  if (want == 0 || want > g_trace_len)
    want = g_trace_len;

  for (size_t i = 0; i < want; i++) {
    size_t idx = (g_trace_idx + TRACE_RING - 1 - i) % TRACE_RING;
    print_trace_entry(g_trace_files[idx], g_trace_lines[idx], g_trace_cols[idx],
                      g_trace_funcs[idx], "  at ");
  }
  return rt_tag_v(0);
}

int64_t rt_trace_get_frames(int64_t *f, int64_t *l, int64_t *c, int64_t *fn,
                            int count) {
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
    return rt_tag_v(0);
  trace_record(file, line, 1, func);

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
  return rt_tag_v(0);
}

int64_t rt_trace_exit(void) {
  if (g_trace_suspended)
    return rt_tag_v(0);
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  /*
   * A repeated call record must be closed at the matching return even
   * when value tracing suppresses the ordinary return line.  Otherwise
   * the held repeat is printed after a later, unrelated call and the
   * trace nesting is misleading.
   */
  if (trace_calls_enabled())
    trace_flush_call_repeats();
  if (trace_calls_enabled() && !trace_values_enabled() &&
      trace_should_print_func(func)) {
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
  return rt_tag_v(0);
}

int64_t rt_trace_ret_void(void) {
  if (g_trace_suspended)
    return rt_tag_v(0);
  int64_t func = g_cs_depth > 0 ? g_cs_funcs[g_cs_depth - 1] : g_trace_func;
  size_t depth = g_cs_depth > 0 ? g_cs_depth - 1 : 0;
  if (!trace_values_enabled() || !trace_should_print_func(func))
    return rt_tag_v(0);
  trace_print_return_prefix(func, depth);
  fputc('\n', stderr);
  return rt_tag_v(0);
}

/* Raw NYIR adapters for the zero-argument trace diagnostics: the native
 * path compares their results against plain integers while the VM consumes
 * the tagged variants above. */
int64_t rt_trace_ret_void_raw(void) { return rt_untag_v(rt_trace_ret_void()); }

int64_t rt_trace_exit_raw(void) { return rt_untag_v(rt_trace_exit()); }

int64_t rt_trace_dump_raw(int64_t count) {
  return rt_untag_v(rt_trace_dump(count));
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
  /* The builtin signature is untyped, so the native call boundary delivers
   * the tagged scalar; decode once so identity returns the plain value. */
  if (is_int(v))
    v = rt_untag_v(v);
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
  if (is_int(v))
    v = rt_untag_v(v);
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
  /* Untyped builtin signature: the native boundary tags scalar literals, so
   * decode once (a boxed integer zero would read as 1, not NULL). */
  if (is_int(v))
    v = rt_untag_v(v);
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
  if (is_int(bits))
    bits = rt_untag_v(bits);
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

int64_t rt_trace_get_call_stack(int64_t *funcs, int64_t *files, int64_t *lines,
                                int max_count) {
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
static int rt_process_argc = 0;
static char **rt_process_argv = NULL;
static char **rt_process_envp = NULL;

static atomic_flag g_env_snapshot_lock = ATOMIC_FLAG_INIT;

static inline void rt_env_snapshot_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_env_snapshot_lock,
                                           memory_order_acquire)) {
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
  int64_t *next = (int64_t *)(uintptr_t)rt_malloc(
      ((int64_t)(env_count + 1) * sizeof(int64_t) << 1) | 1);
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
  live_envp = rt_process_envp ? rt_process_envp : environ;
#endif
  rt_env_snapshot_lock();
  if (!rt_envp_ptr)
    (void)rt_build_env_snapshot_unlocked(live_envp);
  rt_env_snapshot_unlock();
}

static int rt_build_argv_snapshot(char **src_argv, int argc) {
  if (argc < 0)
    argc = 0;
  int64_t *next = (int64_t *)(uintptr_t)rt_malloc(
      ((int64_t)(argc + 1) * sizeof(int64_t) << 1) | 1);
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
  (void)rt_build_argv_snapshot(rt_process_argv, rt_process_argc);
}

int64_t rt_set_args_raw(int argc, char **argv_ptr, char **envp_ptr) {
  rt_cleanup_args();
  rt_argc_val = ((int64_t)argc << 1) | 1;
  rt_process_argc = argc;
  rt_process_argv = argv_ptr;
  rt_process_envp = envp_ptr;
  if (rt_build_argv_snapshot(rt_process_argv, rt_process_argc) != 0)
    return -1;
  char **old_envp = rt_process_envp;
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

int64_t rt_set_args(int64_t argc_v, int64_t argv_ptr_v, int64_t envp_ptr_v) {
  int64_t argc = is_int(argc_v) ? rt_untag_v(argc_v) : argc_v;
  int64_t argv_ptr = (is_int(argv_ptr_v) || NY_NATIVE_IS(argv_ptr_v))
                         ? rt_untag_v(argv_ptr_v)
                         : argv_ptr_v;
  int64_t envp_ptr = (is_int(envp_ptr_v) || NY_NATIVE_IS(envp_ptr_v))
                         ? rt_untag_v(envp_ptr_v)
                         : envp_ptr_v;
  if (argc < 0 || argc > INT_MAX)
    return -1;
  if ((argv_ptr && (uintptr_t)argv_ptr <= NY_VALUE_PTR_MIN_ADDR) ||
      (envp_ptr && (uintptr_t)envp_ptr <= NY_VALUE_PTR_MIN_ADDR))
    return -1;
  return rt_set_args_raw((int)argc, (char **)(uintptr_t)argv_ptr,
                         (char **)(uintptr_t)envp_ptr);
}

int _ny_aot_set_args(int argc, char **argv_ptr, char **envp_ptr) {
  if (rt_argv_ptr || rt_envp_ptr)
    rt_cleanup_args();
  rt_argc_val = ((int64_t)argc << 1) | 1;
  rt_process_argc = argc;
  rt_process_argv = argv_ptr;
  rt_process_envp = envp_ptr;
  return 0;
}

void rt_cleanup_args(void) {
  if (!rt_argv_ptr && !rt_envp_ptr) {
    rt_argc_val = 1;
    rt_process_argc = 0;
    rt_process_argv = NULL;
    rt_process_envp = NULL;
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
  rt_process_argc = 0;
  rt_process_argv = NULL;
  rt_process_envp = NULL;
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

/*
 * Native code uses raw C-string pointers, while the interpreter may
 * pass a tagged/heap string value.  Keep environment lookup at this
 * boundary so native callers never have to walk a runtime-owned pointer
 * table.
 */
int64_t rt_env_get(int64_t key) {
  if (!key)
    return 0;
  uintptr_t kp = (uintptr_t)key;
  if (NY_NATIVE_IS(key))
    kp = (uintptr_t)NY_NATIVE_DECODE(key);
  else if (is_int(key))
    kp = (uintptr_t)rt_untag_v(key);
  else {
    int64_t hp = rt_heap_object_ptr(key);
    if (hp)
      kp = (uintptr_t)hp;
  }
  if (kp <= NY_VALUE_PTR_MIN_ADDR)
    return 0;
  size_t len = rt_cstr_len((int64_t)kp);
  if (len == 0 || len > 4096)
    return 0;
  char name[4097];
  memcpy(name, (const void *)kp, len);
  name[len] = '\0';
  const char *value = getenv(name);
  return value ? rt_alloc_string(value) : 0;
}
int64_t rt_argvp(void) {
  rt_ensure_argv_snapshot();
  return (int64_t)rt_argv_ptr;
}

int64_t rt_argv_get(int64_t index) {
  rt_ensure_argv_snapshot();
  if (index < 0 || index >= (rt_argc_val >> 1) || !rt_argv_ptr)
    return 0;
  int64_t raw = rt_argv_ptr[index];
  return raw ? rt_alloc_string((const char *)(uintptr_t)raw) : 0;
}

int64_t rt_args_raw(void) {
  rt_ensure_argv_snapshot();
  int64_t count = rt_argc_val >> 1;
  int64_t out = rt_tbuf_new_raw(count, 24);
  if (!out)
    return 0;
  for (int64_t i = 0; i < count; ++i)
    rt_tbuf_set_i64_raw(out, i, rt_argv_get(i));
  return out;
}

int64_t rt_envc_raw(void) {
  rt_ensure_env_snapshot();
  return rt_envc_val >> 1;
}

int64_t rt_atoi(int64_t value) {
  if (!value)
    return 0;
  uintptr_t p = (uintptr_t)value;
  /*
   * LLVM/JIT string constants are raw pointers and may happen to end in
   * the native tag bits because their global alignment is only one
   * byte.  Test the pointer representation before decoding tagged
   * native values.
   */
  if (rt_native_is_str(value)) {
    p = (uintptr_t)value;
  } else if (NY_NATIVE_IS(value))
    p = (uintptr_t)NY_NATIVE_DECODE(value);
  else {
    int64_t hp = rt_heap_object_ptr(value);
    if (hp)
      p = (uintptr_t)hp;
  }
  if (p <= NY_VALUE_PTR_MIN_ADDR)
    return 0;
  const char *s = (const char *)p;
  char *end = NULL;
  long long parsed = strtoll(s, &end, 10);
  return end == s ? 0 : (int64_t)parsed;
}

double rt_atof(int64_t value) {
  if (!value)
    return 0.0;
  uintptr_t p = (uintptr_t)value;
  /*
   * See rt_atoi: raw C-string pointers must win over low-bit tag
   * heuristics when LLVM places a constant at an address ending in 0x6.
   */
  if (rt_native_is_str(value))
    p = (uintptr_t)value;
  else if (NY_NATIVE_IS(value))
    p = (uintptr_t)NY_NATIVE_DECODE(value);
  else {
    int64_t hp = rt_heap_object_ptr(value);
    if (hp)
      p = (uintptr_t)hp;
  }
  if (p <= NY_VALUE_PTR_MIN_ADDR)
    return 0.0;
  const char *s = (const char *)p;
  char *end = NULL;
  double parsed = strtod(s, &end);
  return end == s ? 0.0 : parsed;
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
int64_t rt_is_nil(int64_t v) {
  return rt_is_nil_imm(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
int64_t rt_is_int(int64_t v) { return is_int(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_is_ptr(int64_t v) { return is_ptr(v) ? NY_IMM_TRUE : NY_IMM_FALSE; }
int64_t rt_native_is_int(int64_t v) {
  if (is_int(v))
    return 1;
  if ((uint64_t)v <= 4096)
    return 1;
  if (is_v_flt(v))
    return 0;
  if (ny_native_dict_ptr(v) != NULL)
    return 0;
  if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return 0;
    }
  }
  if (is_heap_ptr(v))
    return 0;
  if (rt_native_is_str(v))
    return 0;
  return 1;
}
int64_t rt_is_ny_obj(int64_t v) {
  return is_ny_obj(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
int64_t rt_is_str_obj(int64_t v) {
  if (!v)
    return NY_IMM_FALSE;
  if (is_v_str(v))
    return NY_IMM_TRUE;
  /*
   * Native panic/error paths may expose an owned C-string pointer
   * rather than a tagged string immediate.  It is still a valid string
   * object at the public dynamic boundary and must be accepted by
   * `is_str`, `type`, and string predicates.
   */
  if (rt_native_is_str(v))
    return NY_IMM_TRUE;
  if (is_int(v))
    return NY_IMM_FALSE;
  if (is_ptr(v) && !is_heap_ptr(v))
    return NY_IMM_TRUE;
  return NY_IMM_FALSE;
}
int64_t rt_is_float_obj(int64_t v) {
  return is_v_flt(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
static int64_t rt_runtime_tag_raw(const char *s, size_t n) {
  return rt_runtime_tag_raw_name(s, n);
}

int64_t rt_runtime_tag(int64_t name) {
  if (!is_v_str(name))
    return rt_tag_v(0);
  const char *s = (const char *)(uintptr_t)name;
  size_t n = rt_tagged_str_len(name);
  return rt_tag_v(rt_runtime_tag_raw(s, n));
}

int64_t rt_runtime_tag_raw_value(int64_t name) {
  /*
   * JIT string constants are byte-addressed and can have an odd
   * address, so low-bit integer tagging is not a valid rejection test
   * at this boundary.
   */
  if (!name || !rt_addr_mapped((uintptr_t)name, 1))
    return 0;
  const char *s = (const char *)(uintptr_t)name;
  size_t n = 0;
  /*
   * Native lowering normally passes an interned C string.  Keep this
   * ABI boundary defensive: generated code may pass an unknown value
   * while probing a dynamic type, and strlen must never walk arbitrary
   * memory.
   */
  while (n < 256) {
    if (!rt_addr_mapped((uintptr_t)s + n, 1))
      return 0;
    if (s[n] == '\0')
      break;
    ++n;
  }
  if (n == 256)
    return 0;
  return rt_runtime_tag_raw(s, n);
}

int64_t rt_has_tag(int64_t v, int64_t tag_v) {
  int64_t want = is_int(tag_v) ? (tag_v >> 1) : tag_v;
  if (is_int(v))
    return (want == 1) ? NY_IMM_TRUE : NY_IMM_FALSE;
  if ((uint64_t)v < 4096)
    return (want == 0) ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (want == TAG_FLOAT)
    return is_v_flt(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (v == 0)
    return want == 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_tbuf_known_handle((uintptr_t)v) &&
      rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return (want == TAG_LIST) ? NY_IMM_TRUE : NY_IMM_FALSE;
    }
  }
  if (want == TAG_DICT || want == TAG_SET) {
    ny_native_dict_t *dict = ny_native_dict_ptr(v);
    if (dict)
      return want == TAG_DICT ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    uintptr_t p = (uintptr_t)heap_v;
    int64_t tag = *(int64_t *)((char *)p - 8);
    if (tag == TAG_STR_CONST)
      rt_const_str_cache_store(p);
    return (tag >= 100 && tag <= 255 && tag == want) ? NY_IMM_TRUE
                                                     : NY_IMM_FALSE;
  }
  if (!is_ptr(v) || ((v) & 7) != 0)
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
    return ((tag == TAG_STR || tag == TAG_STR_CONST) && tag == want)
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  }
  return NY_IMM_FALSE;
}

int64_t rt_native_has_tag(int64_t v, int64_t tag) {
  /*
   * Native dynamic values may arrive in compact encoded-handle form.  Decode
   * only handles whose private header is actually a tbuf; otherwise retain
   * the original value for the ordinary scalar/object checks below.
   */
  if (NY_NATIVE_IS(v) && NY_NATIVE_DECODE(v) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(v);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      v = decoded;
  }
  if (is_int(v))
    return tag == 1 ? NY_IMM_TRUE : NY_IMM_FALSE;
  if ((uint64_t)v < 4096)
    return tag == 0 ? NY_IMM_TRUE : NY_IMM_FALSE;
  /*
   * Native buffers are list values, never managed objects.  Reject every
   * other tag before falling through to the managed object tag reader.
   */
  if (tag == TAG_LIST &&
      rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return tag == TAG_LIST ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (tag == TAG_DICT) {
    return ny_native_dict_ptr(v) != NULL ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (tag == TAG_LIST) {
    if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER)) {
      int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
      if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
        return NY_IMM_TRUE;
    }
    return rt_has_tag(v, rt_tag_v(TAG_LIST));
  }
  if (tag == TAG_SET)
    return is_valid_heap_ptr(v) && rt_set_layout(v) ? NY_IMM_TRUE
                                                     : NY_IMM_FALSE;
  if (tag == TAG_STR || tag == TAG_STR_CONST) {
    return rt_native_is_str(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  if (!is_valid_heap_ptr(v))
    return NY_IMM_FALSE;
  return rt_has_tag(v, rt_tag_v(tag)) == NY_IMM_TRUE ? NY_IMM_TRUE
                                                     : NY_IMM_FALSE;
}
int64_t rt_value_tag(int64_t v) {
  if (v == 0)
    return 0;
  /* Async tasks are opaque runtime handles, not strings or scalar immediates.
   * Their addresses can satisfy both heuristic predicates. */
  if (rt_async_is_handle(v))
    return TAG_CLOSURE;
  /*
   * Raw native handles may be odd and therefore satisfy the scalar low-bit
   * test.  Resolve authoritative native representations before classifying
   * immediates, so nested tbufs crossing an any callback remain containers.
   */
  if (NY_NATIVE_IS(v) && NY_NATIVE_DECODE(v) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(v);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      return TAG_LIST;
  }
  if (rt_magic_tbuf_elem_size(v) > 0)
    return TAG_LIST;
  if (rt_native_is_str(v))
    return TAG_STR;
  if (is_int(v))
    return 1;
  if (ny_native_dict_ptr(v) != NULL)
    return TAG_DICT;
  if (rt_set_layout(v))
    return TAG_SET;
  /*
   * Managed objects have their authoritative tag at payload - 8.  Check
   * it before probing the wider native-tbuf header: allocator bytes
   * preceding a set/range can coincidentally contain the tbuf magic and
   * misclassify the object as a list when it crosses an `any` ABI
   * boundary.
   */
  int64_t heap_v = rt_heap_object_ptr(v);
  int64_t direct_tag = 0;
  if (!heap_v && is_ptr(v) && (((uint64_t)v) & NY_VALUE_PTR_TAG_MASK) == 0 &&
      rt_try_read_i64((uintptr_t)v - 8, &direct_tag) && direct_tag >= 100 &&
      direct_tag <= 255)
    heap_v = v;
  if (heap_v) {
    int64_t tag =
        direct_tag ? direct_tag : *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    if (tag >= 100 && tag <= 255)
      return tag;
  }
  if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return TAG_LIST;
    }
  }
  if (rt_has_tag(v, rt_tag_v(TAG_LIST)) == NY_IMM_TRUE)
    return TAG_LIST;
  if (is_v_flt(v))
    return TAG_FLOAT;
  if (rt_is_str(v))
    return 121;
  if (is_heap_ptr(v) || rt_value_is_ptr(v))
    return TAG_CLOSURE;
  return 3;
}

int64_t rt_tag_or_raw_int(int64_t value) {
  int64_t tag = rt_value_tag(value);
  return tag == 0 ? 1 : tag;
}

/*
 * Classify a raw machine word for a descriptor slot whose payload was
 * already unboxed (rt_any_to_i64) at the store.  rt_value_tag cannot be
 * reused here: an odd raw integer satisfies the tagged-int probe and gets
 * tag 1 ("payload is tagged"), so a later raw consumer untapped the value
 * (1 -> 0, 17 -> 8) and mapcat-style flattening lost scalar leaves.
 */
int64_t rt_raw_word_tag(int64_t value) {
  if (value == 0)
    return 0;
  if (rt_native_is_str(value) || is_v_str(value))
    return TAG_STR;
  if (rt_magic_tbuf_elem_size(value) > 0)
    return TAG_LIST;
  if (is_ptr(value) && is_heap_ptr(value)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)value - 8);
    if (tag >= 100 && tag <= 255)
      return tag;
  }
  if (is_heap_ptr(value) || rt_value_is_ptr(value))
    return TAG_CLOSURE;
  return 3;
}

int64_t rt_type_name(int64_t value) {
  const char *name = "unknown";
  switch (rt_value_tag(value)) {
  case 0:
    name = "nil";
    break;
  case 3:
    name = "int";
    break;
  case TAG_FLOAT:
    name = "float";
    break;
  case TAG_STR:
  case TAG_STR_CONST:
    name = "str";
    break;
  case TAG_LIST:
    name = "list";
    break;
  case TAG_DICT:
    name = "dict";
    break;
  case TAG_SET:
    name = "set";
    break;
  case TAG_TUPLE:
    name = "tuple";
    break;
  case TAG_RANGE:
    name = "range";
    break;
  case TAG_BYTES:
    name = "bytes";
    break;
  case TAG_BIGINT:
    name = "bigint";
    break;
  case TAG_BIGFLOAT:
    name = "bigfloat";
    break;
  case TAG_COMPLEX:
    name = "complex";
    break;
  case TAG_CLOSURE:
    name = "ptr";
    break;
  default:
    break;
  }
  return (int64_t)(uintptr_t)name;
}

int64_t rt_type_name_tagged(int64_t value, int64_t tag) {
  if (tag == NY_IMM_TRUE || tag == NY_IMM_FALSE)
    return (int64_t)(uintptr_t)"bool";
  return rt_type_name(value);
}
int64_t rt_is_str(int64_t v) {
  if (rt_async_is_handle(v))
    return 0;
  if (v == 0 || (uint64_t)v <= 4096 ||
      (uint64_t)v >= UINT64_C(0x0000800000000000))
    return 0;
  if (is_v_flt(v))
    return 0;
  if (ny_native_dict_ptr(v) != NULL)
    return 0;
  if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return 0;
  }
  if (rt_has_tag(v, rt_tag_v(TAG_LIST)) == NY_IMM_TRUE)
    return 0;
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    return (tag == TAG_STR || tag == TAG_STR_CONST) ? 1 : 0;
  }
  /*
   * A large raw integer is not a C string merely because it falls in
   * the userspace address range.  Only classify headerless JIT/AOT
   * constants as strings when their payload address is actually
   * readable.
   */
  return rt_addr_readable_safe((uintptr_t)v, 1) ? 1 : 0;
}

/*
 * Kept for already-emitted native modules.  New lowering emits rt_is_str.
 */
int64_t rt_native_is_str(int64_t v) { return rt_is_str(v); }

int64_t rt_value_is_ptr(int64_t v) {
  if (v == 0 || (uint64_t)v <= 4096 ||
      (uint64_t)v >= UINT64_C(0x0000800000000000))
    return 0;
  if (is_v_flt(v))
    return 0;
  if (is_int(v))
    return 0;
  return 1;
}

int64_t rt_native_is_ptr(int64_t v) { return rt_value_is_ptr(v); }
int64_t rt_tagof(int64_t v) {
  if (v == 0)
    return 0;
  if (ny_native_dict_ptr(v) != NULL)
    return rt_tag_v(TAG_DICT);
  if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      return rt_tag_v(TAG_LIST);
    }
  }
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

int64_t rt_init_str(int64_t p, int64_t n) {
  if (!p)
    return 0;
  if (is_int(p))
    p = rt_untag_v(p);
  if (is_int(n))
    n >>= 1;
  if (n < 0)
    n = 0;
  if (is_ptr(p) && is_heap_ptr(p)) {
    *(int64_t *)((char *)(uintptr_t)p - 8) = TAG_STR;
    *(int64_t *)((char *)(uintptr_t)p - 16) = rt_tag_v(n);
  }
  return p;
}

int64_t rt_bytes_new(int64_t n) {
  if (is_int(n))
    n >>= 1;
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

/*
 * NYIR passes scalar lengths untagged.  Keep the public rt_bytes_new
 * entry compatible with the language ABI and expose a raw-length
 * companion for unified native lowering and its LLVM emitter.
 */
int64_t rt_bytes_new_raw(int64_t length) {
  if (length < 0)
    length = 0;
  return rt_bytes_new(rt_tag_v(length));
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
int64_t rt_is_err(int64_t v) {
  return is_v_err(v) ? NY_IMM_TRUE : NY_IMM_FALSE;
}
int64_t rt_unwrap(int64_t v) {
  if (is_v_ok(v) || is_v_err(v)) {
    return *(int64_t *)(uintptr_t)v;
  }
  return v;
}

/*
 * Result payloads retain the language-value ABI, while NYIR scalar
 * slots are raw. Decode a tagged scalar before returning it to
 * generated native code.
 */
int64_t rt_result_unwrap_raw(int64_t v) { return rt_any_to_i64(rt_unwrap(v)); }
int64_t rt_result_unwrap_or_raw(int64_t v, int64_t fallback) {
  if (is_v_ok(v)) {
    return rt_any_to_i64(rt_unwrap(v));
  }
  return fallback;
}
int64_t rt_list_new(int64_t n_v) {
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0)
    n = 0;

  /*
   * `list(n)` allocates an EMPTY list with capacity n.  Use the native
   * tbuf layout (the same shape `[]` and append-able lists carry) so
   * that append / get / len / contains all work; the previous managed
   * heap layout (tag at payload - 8, odd-encoded length) was rejected
   * by rt_tbuf_append_raw, which clamped any `list(n)` list to
   * length zero.
   */
  int64_t out = rt_tbuf_new_raw(n, 8);
  if (!out)
    return 0;
  /*
   * tbuf_new seeds the length with the requested count; list() is
   * empty.
   */
  int64_t *hdr = (int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER);
  hdr[1] = 0;
  return out;
}

int64_t rt_list_new_raw(int64_t capacity) {
  if (capacity < 0)
    capacity = 0;
  int64_t out = rt_tbuf_new_raw(capacity, 8);
  if (!out)
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER);
  hdr[1] = 0;
  return out;
}

/*
 * Pre-sized list for typed `list<int>` declarations.  Unlike
 * `rt_list_new`, which allocates an empty (length=0) list suitable for
 * append-only use, this variant keeps the length at `n` so that indexed
 * writes and reads at positions 0..n-1 are valid immediately.  Both use
 * the same tbuf layout; the difference is whether `hdr[1]` is zeroed.
 */
int64_t rt_list_new_sized(int64_t n) {
  if (n < 0)
    n = 0;
  return rt_tbuf_new_raw(n, 8);
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
  if (!lst)
    return lst;
  if ((uint64_t)lst >= 4096) {
    int64_t *base = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)base[0] == NY_NATIVE_TBUF_MAGIC) {
      /*
       * All benchmark scalar lists use 8-byte raw elements.  Do not run
       * the conservative pointer/string probe for every integer append;
       * besides being unnecessary it turns a linear builder loop into a
       * very costly runtime-boundary hot path.  Descriptor (24-byte)
       * lists still use the tagged/string-aware path below.
       */
      if (base[2] == 8 && !rt_magic_tbuf_elem_size(val) &&
          !rt_native_is_str(val) && !is_v_str(val) &&
          !rt_heap_object_ptr(val))
        return rt_tbuf_append_i64_raw(lst, val);
      /*
       * Descriptor buffers carry the dynamic any ABI.  Normalize tagged
       * integer payloads exactly once before storing them.
       */
      return rt_tbuf_append_tagged(lst, val, rt_native_is_str(val));
    }
  }
  if (!is_ptr(lst))
    return lst;
  if (rt_tagof(lst) != ((TAG_LIST << 1) | 1))
    return lst;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  int64_t cap_v = *(int64_t *)((char *)(uintptr_t)lst + 8);
  int64_t cap = is_int(cap_v) ? (cap_v >> 1) : cap_v;
  if (n >= cap) {
    int64_t new_cap = cap == 0 ? 16 : (cap < 1024 ? cap * 2 : cap + (cap >> 1));
    int64_t new_p = rt_malloc_uninit(16 + new_cap * 8);
    if (!new_p)
      return lst;
    *(int64_t *)((char *)(uintptr_t)new_p - 8) = TAG_LIST;
    *(int64_t *)((char *)(uintptr_t)new_p + 0) = len_v;
    *(int64_t *)((char *)(uintptr_t)new_p + 8) = (new_cap << 1) | 1;
    if (n > 0)
      memcpy((char *)(uintptr_t)new_p + 16, (char *)(uintptr_t)lst + 16,
             (size_t)n * 8);
    lst = new_p;
  }

  *(int64_t *)((char *)(uintptr_t)lst + 16 + n * 8) = val;
  *(int64_t *)((char *)(uintptr_t)lst + 0) = ((n + 1) << 1) | 1;
  return lst;
}

int64_t rt_list_reserve(int64_t lst, int64_t cap_v) {
  int64_t requested = is_int(cap_v) ? (cap_v >> 1) : cap_v;
  if (requested >= 0 &&
      rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return rt_tbuf_reserve(lst, requested);
  }
  if (!is_ptr(lst))
    return lst;
  if (rt_tagof(lst) != ((TAG_LIST << 1) | 1))
    return lst;
  int64_t want = is_int(cap_v) ? (cap_v >> 1) : cap_v;
  if (want <= 0)
    return lst;
  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(len_v) ? (len_v >> 1) : len_v;
  int64_t cap_v0 = *(int64_t *)((char *)(uintptr_t)lst + 8);
  int64_t cap = is_int(cap_v0) ? (cap_v0 >> 1) : cap_v0;
  if (cap >= want)
    return lst;
  int64_t new_p = rt_malloc_uninit(16 + want * 8);
  if (!new_p)
    return lst;
  *(int64_t *)((char *)(uintptr_t)new_p - 8) = TAG_LIST;
  *(int64_t *)((char *)(uintptr_t)new_p + 0) = len_v;
  *(int64_t *)((char *)(uintptr_t)new_p + 8) = rt_tag_v(want);
  if (n > 0)
    memcpy((char *)(uintptr_t)new_p + 16, (char *)(uintptr_t)lst + 16,
           (size_t)n * 8);
  return new_p;
}

int64_t rt_list_sum_int_range(int64_t lst, int64_t start_v, int64_t stop_v) {
  if (lst && rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                       RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t len = hdr[1];
      /*
       * Native tbuf calls use unboxed i64 indices.  The boxed list path
       * below is the interpreter/legacy ABI and deliberately keeps
       * tagged indices.
       */
      int64_t start = start_v;
      int64_t stop = stop_v;
      if (start < 0)
        start = 0;
      if (stop > len)
        stop = len;
      int64_t sum = 0;
      for (int64_t i = start; i < stop; ++i) {
        int64_t item = rt_tbuf_get(lst, i, 0);
        sum += item;
      }
      return rt_tag_v(sum);
    }
  }
  if (!is_ptr(lst) || !is_heap_ptr(lst))
    return rt_tag_v(0);
  int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
  if (tag != TAG_LIST && tag != TAG_TUPLE)
    return rt_tag_v(0);

  int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t len = rt_untag_v(len_v);
  int64_t start = rt_untag_v(start_v);
  int64_t stop = rt_untag_v(stop_v);
  if (start < 0)
    start = 0;
  if (stop > len)
    stop = len;
  if (stop <= start)
    return rt_tag_v(0);

  int64_t sum = 0;
  int64_t *items = (int64_t *)((char *)(uintptr_t)lst + 16);
  for (int64_t i = start; i < stop; ++i) {
    int64_t v = items[i];
    if (is_int(v)) {
      sum += v >> NY_VALUE_INT_SHIFT;
    } else if (is_v_flt(v)) {
      extern int64_t rt_flt_to_int(int64_t v);
      sum += rt_untag_v(rt_flt_to_int(v));
    } else if (is_ptr(v) && is_heap_ptr(v) &&
               *(int64_t *)((char *)(uintptr_t)v - 8) == TAG_BIGINT) {
      extern int64_t rt_bigint_to_int(int64_t v);
      sum += rt_untag_v(rt_bigint_to_int(v));
    } else if (NY_NATIVE_IS(v)) {
      sum += rt_untag_v(v);
    }
  }
  return rt_tag_v(sum);
}

static inline int64_t rt_dict_raw_i64(int64_t v) {
  return is_int(v) ? (v >> 1) : v;
}

static inline uint64_t rt_dict_hash_mix64(uint64_t bits) {
  bits ^= bits >> 33;
  bits *= 0xff51afd7ed558ccdULL;
  bits ^= bits >> 33;
  bits *= 0xc4ceb9fe1a85ec53ULL;
  bits ^= bits >> 33;
  return bits & 2147483647u;
}

static uint64_t rt_dict_hash_raw(int64_t key) {
  if (is_int(key))
    return (uint64_t)(key >> 1);
  if (rt_is_nil_imm(key))
    return 0x4e494cULL;
  if (key == NY_IMM_FALSE)
    return 0x46414c5345ULL;
  if (is_v_str(key))
    return rt_untag_v(rt_str_hash(key));
  if (is_v_flt(key)) {
    uint64_t bits = (uint64_t)_rt_flt_unbox_val(key);
    if (bits == 0x8000000000000000ULL)
      bits = 0;
    if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL &&
        (bits & 0x000fffffffffffffULL) != 0)
      bits = 0x7ff8000000000000ULL;
    return rt_dict_hash_mix64(bits);
  }
  if (is_ptr(key))
    return (((uint64_t)key) >> 3) & 2147483647u;
  return 0;
}

static inline int rt_dict_key_eq_fast(int64_t a, int64_t b) {
  return a == b || rt_eq(a, b) == NY_IMM_TRUE;
}

static inline int64_t rt_dict_table_base(int64_t d) {
  return *(int64_t *)((char *)(uintptr_t)d + 16);
}

static int64_t rt_dict_find_off_table(int64_t t, int64_t cap, int64_t key) {
  if (cap <= 0 || !t)
    return -1;
  uint64_t mask = (uint64_t)(cap - 1);
  uint64_t idx = rt_dict_hash_raw(key) & mask;
  int64_t first_tomb = -1;
  for (int64_t i = 0; i < cap; i++) {
    int64_t off = (int64_t)idx * 24;
    int64_t state = *(int64_t *)((char *)(uintptr_t)t + off + 16);
    if (!state)
      return first_tomb >= 0 ? first_tomb : off;
    if (state == 1) {
      int64_t slot_key = *(int64_t *)((char *)(uintptr_t)t + off);
      if (rt_dict_key_eq_fast(slot_key, key))
        return off;
    } else if (state == 2 && first_tomb < 0) {
      first_tomb = off;
    }
    idx = (idx + 1) & mask;
  }
  return first_tomb;
}

static void rt_dict_insert_raw_table(int64_t t, int64_t cap, int64_t key,
                                     int64_t value) {
  int64_t off = rt_dict_find_off_table(t, cap, key);
  if (off < 0)
    return;
  *(int64_t *)((char *)(uintptr_t)t + off) = key;
  *(int64_t *)((char *)(uintptr_t)t + off + 8) = value;
  *(int64_t *)((char *)(uintptr_t)t + off + 16) = 1;
}

int64_t rt_dict_reserve(int64_t d, int64_t additional_v) {
  ny_native_dict_t *native = ny_native_dict_ptr(d);
  if (native) {
    int64_t additional =
        is_int(additional_v) ? (additional_v >> 1) : additional_v;
    int64_t want = native->length + (additional > 0 ? additional : 0);
    int64_t cap = native->capacity;
    int64_t need = cap < 8 ? 8 : cap;
    while (need < want * 2 && need <= INT64_MAX / 2)
      need *= 2;
    if (need > cap)
      ny_native_dict_resize(native, need);
    return d;
  }
  if (!is_ptr(d) || !is_heap_ptr(d))
    return d;
  if (*(int64_t *)((char *)(uintptr_t)d - 8) != TAG_DICT)
    return d;
  int64_t additional =
      is_int(additional_v) ? (additional_v >> 1) : additional_v;
  if (additional <= 0)
    return d;
  int64_t count = rt_dict_raw_i64(*(int64_t *)((char *)(uintptr_t)d + 0));
  int64_t cap = rt_dict_raw_i64(*(int64_t *)((char *)(uintptr_t)d + 8));
  int64_t want_count = count + additional;
  if (want_count < count)
    return d;
  int64_t min_cap = want_count > (INT64_MAX / 2) ? INT64_MAX : want_count * 2;
  int64_t want_cap = 8;
  while (want_cap > 0 && want_cap < min_cap && want_cap <= (INT64_MAX / 2))
    want_cap *= 2;
  if (want_cap <= 0 || cap >= want_cap)
    return d;
  int64_t t = rt_dict_table_base(d);
  int64_t nt = rt_malloc(16 + want_cap * 24);
  if (!nt)
    return d;
  *(int64_t *)((char *)(uintptr_t)nt - 8) = TAG_DICT_TBL;
  for (int64_t i = 0; i < cap; i++) {
    int64_t off = (int64_t)i * 24;
    int64_t state = *(int64_t *)((char *)(uintptr_t)t + off + 16);
    if (state == 1) {
      int64_t key = *(int64_t *)((char *)(uintptr_t)t + off);
      int64_t value = *(int64_t *)((char *)(uintptr_t)t + off + 8);
      rt_dict_insert_raw_table(nt, want_cap, key, value);
    }
  }
  *(int64_t *)((char *)(uintptr_t)d + 8) = want_cap;
  *(int64_t *)((char *)(uintptr_t)d + 16) = nt;
  return d;
}

int64_t rt_load_item(int64_t lst, int64_t i_v) {
  return rt_load_item_fast(lst, i_v);
}

int64_t rt_load_item_any(int64_t lst, int64_t i_v) {
  int64_t index = is_int(i_v) ? rt_untag_v(i_v) : i_v;
  if (!lst)
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)lst))
    return rt_tbuf_index_any_raw(lst, index);
  int64_t raw = rt_load_item_fast(lst, index);
  /*
   * Managed list slots contain canonical tagged values.  Raw scalar slots
   * produced by the legacy LLVM list builder are boxed at this one explicit
   * dynamic boundary; pointers, strings, booleans, and nil pass unchanged.
   */
  if (is_int(raw) || rt_is_bool_imm(raw) || rt_native_is_str(raw) ||
      is_v_str(raw) || is_ptr(raw) || !raw)
    return raw;
  return rt_tag_v(raw);
}

int64_t rt_store_item(int64_t lst, int64_t i_v, int64_t val) {
  return rt_store_item_fast(lst, i_v, val);
}

int64_t rt_load_item_fast(int64_t lst, int64_t i_v) {
  if (!lst)
    return 0;
  if (rt_sequence_tag(lst) == TAG_RANGE ||
      (rt_heap_object_ptr(lst) &&
       *(int64_t *)((char *)(uintptr_t)rt_heap_object_ptr(lst) - 8) ==
           TAG_RANGE))
    return rt_range_index_read_raw(lst, i_v);
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return rt_tbuf_get(lst, i_v, 0);
  }
  return _rt_load_item_fast(lst, i_v);
}

int64_t rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  if (!lst)
    return 0;
  int64_t raw_i = is_int(i_v) ? rt_untag_v(i_v) : i_v;
  int64_t raw_val = is_int(val) ? rt_untag_v(val) : val;
  int64_t tagged_val =
      (is_int(val) || !ny_small_int_fits_i64(val)) ? val : rt_tag_v(val);
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC && hdr[2] >= 24)
      return rt_tbuf_set_tagged(lst, raw_i, val) ? tagged_val : 0;
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return rt_tbuf_set_i64_raw(lst, raw_i, raw_val) ? tagged_val : 0;
  }
  return _rt_store_item_fast(lst, raw_i, tagged_val);
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

static int rt_sort_raw_i64_cmp(const void *ap, const void *bp) {
  int64_t a = *(const int64_t *)ap;
  int64_t b = *(const int64_t *)bp;
  return (a > b) - (a < b);
}

int64_t rt_sort_list(int64_t lst) {
  if (!is_ptr(lst))
    return lst;
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      int64_t n = hdr[1];
      if (n > 1 && hdr[2] == (int64_t)sizeof(int64_t))
        qsort((void *)(uintptr_t)lst, (size_t)n, sizeof(int64_t),
              rt_sort_raw_i64_cmp);
      return lst;
    }
  }
  if (!is_heap_ptr(lst))
    return lst;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
  if (tag != TAG_LIST && tag != TAG_TUPLE)
    return lst;
  int64_t tagged_len = *(int64_t *)((char *)(uintptr_t)lst + 0);
  int64_t n = is_int(tagged_len) ? (tagged_len >> 1) : tagged_len;
  if (n > 1)
    qsort((void *)((char *)(uintptr_t)lst + 16), (size_t)n, sizeof(int64_t),
          rt_sort_list_cmp);
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
  /*
   * The unified native caller supplies raw integers, while the legacy
   * language ABI supplies tagged integers.  A tagged integer is always
   * odd; native ranges commonly include an even zero/start value, so
   * preserve the raw call when any scalar is even.
   */
  bool raw_abi =
      ((start_v & 1) == 0) || ((stop_v & 1) == 0) || ((step_v & 1) == 0);
  int64_t start =
      raw_abi ? start_v : (is_int(start_v) ? rt_untag_v(start_v) : start_v);
  int64_t stop =
      raw_abi ? stop_v : (is_int(stop_v) ? rt_untag_v(stop_v) : stop_v);
  int64_t step =
      raw_abi ? step_v : (is_int(step_v) ? rt_untag_v(step_v) : step_v);
  if (step == 0)
    step = 1;
  int64_t obj = rt_malloc(rt_tag_v(24));
  if (!obj)
    return 0;
  *(int64_t *)((char *)(uintptr_t)obj - 8) = TAG_RANGE;
  *(int64_t *)((char *)(uintptr_t)obj + 0) = start;
  *(int64_t *)((char *)(uintptr_t)obj + 8) = stop;
  *(int64_t *)((char *)(uintptr_t)obj + 16) = step;
  return obj;
}

int64_t rt_range_values_raw(int64_t range) {
  /*
   * rt_range_new returns the payload pointer directly.  Do not require
   * the managed-value unboxing helper here: native calls receive that
   * raw handle and the helper may intentionally reject it as a tagged
   * value.
   */
  int64_t heap =
      rt_sequence_tag(range) == TAG_RANGE ? range : rt_heap_object_ptr(range);
  if (!heap || *(int64_t *)((char *)(uintptr_t)heap - 8) != TAG_RANGE)
    return rt_tbuf_new_raw(0, 8);
  int64_t start = *(int64_t *)(uintptr_t)heap;
  int64_t stop = *(int64_t *)((char *)(uintptr_t)heap + 8);
  int64_t step = *(int64_t *)((char *)(uintptr_t)heap + 16);
  int64_t count = 0;
  if (step > 0 && start < stop)
    count = (stop - start + step - 1) / step;
  else if (step < 0 && start > stop)
    count = (start - stop - step - 1) / (-step);
  int64_t out = rt_tbuf_new_raw(count, 8);
  if (!out)
    return 0;
  /*
   * Eight-byte native tbufs are raw scalar slots.  Tagging these
   * integers makes 1 indistinguishable from raw zero at the dynamic
   * comparison ABI.
   */
  for (int64_t i = 0, value = start; i < count; ++i, value += step)
    ((int64_t *)(uintptr_t)out)[i] = value;
  return out;
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
    memcpy((char *)(uintptr_t)out + 16, (char *)(uintptr_t)src + 16,
           (size_t)n * sizeof(int64_t));
  *(int64_t *)((char *)(uintptr_t)out - 8) = tag;
  return out;
}

int64_t rt_adt_alloc(int64_t nfields, int64_t tag) {
  int64_t n = is_int(nfields) ? rt_untag_v(nfields) : nfields;
  int64_t t = is_int(tag) ? rt_untag_v(tag) : tag;
  if (n < 0)
    n = 0;
  /*
   * The allocator size is encoded as a shifted byte count.  Reject
   * hostile field counts before either the multiplication or the
   * encoding can wrap.
   */
  if (n > INT64_MAX / (int64_t)sizeof(int64_t))
    return 0;
  int64_t bytes = n * sizeof(int64_t);
  if ((uint64_t)bytes > (UINT64_MAX >> 1))
    return 0;
  int64_t p = rt_malloc(((uint64_t)bytes << 1) | 1u);
  if (p) {
    *(int64_t *)((char *)(uintptr_t)p - 8) = t;
  }
  return p;
}

int64_t rt_adt_tag(int64_t v) { return ny_value_heap_tag(v); }

static char g_single_char_table[256][2];
static bool g_single_char_table_inited = false;

static void init_single_char_table(void) {
  if (!g_single_char_table_inited) {
    for (int i = 0; i < 256; i++) {
      g_single_char_table[i][0] = (char)i;
      g_single_char_table[i][1] = '\0';
    }
    g_single_char_table_inited = true;
  }
}

int64_t rt_cstr_get_raw(int64_t str_v, int64_t idx_v) {
  init_single_char_table();
  if (!str_v)
    return (int64_t)(uintptr_t)"";
  const char *s = (const char *)(uintptr_t)str_v;
  size_t len = strlen(s);
  /*
   * NYIR loop indexes are native/raw integers, including odd values.
   */
  int64_t idx = idx_v;
  if (idx < 0 && (uint64_t)(-idx) <= len)
    idx += (int64_t)len;
  if (idx < 0 || (size_t)idx >= len)
    return (int64_t)(uintptr_t)"";
  unsigned char c = (unsigned char)s[idx];
  return (int64_t)(uintptr_t)g_single_char_table[c];
}

int64_t rt_cstr_index_read_raw(int64_t str_v, int64_t idx_v) {
  init_single_char_table();
  if (!str_v) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return (int64_t)(uintptr_t)"";
  }
  const char *s = (const char *)(uintptr_t)str_v;
  size_t len = strlen(s);
  int64_t idx = idx_v;
  if (idx < 0 && (uint64_t)(-idx) <= len)
    idx += (int64_t)len;
  if (idx < 0 || (size_t)idx >= len) {
    rt_panic(rt_alloc_string("index_read out of range"));
    return (int64_t)(uintptr_t)"";
  }
  unsigned char c = (unsigned char)s[idx];
  return (int64_t)(uintptr_t)g_single_char_table[c];
}

int64_t rt_cstr_repeat(int64_t str_ptr, int64_t count) {
  if (!str_ptr || count <= 0)
    return rt_alloc_string_len("", 0);
  const char *src = (const char *)(uintptr_t)str_ptr;
  size_t length = is_v_str(str_ptr) ? rt_tagged_str_len(str_ptr) : strlen(src);
  if (length == 0)
    return str_ptr;
  if ((uint64_t)count > SIZE_MAX / length ||
      length * (size_t)count > SIZE_MAX - 1)
    return 0;
  size_t total = length * (size_t)count;
  char *out = malloc(total + 1);
  if (!out)
    return 0;
  for (int64_t i = 0; i < count; ++i)
    memcpy(out + (size_t)i * length, src, length);
  out[total] = '\0';
  int64_t value = rt_alloc_string_len(out, total);
  free(out);
  return value;
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
  bool native_cstr = !is_v_str(s) && rt_native_is_str(s);
  size_t len = native_cstr ? (size_t)rt_cstr_len(s) : rt_tagged_str_len(s);
  if (len <= 1)
    return native_cstr ? rt_alloc_string_len((const char *)(uintptr_t)s, len)
                       : s;
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
  if (is_v_str(xs) || rt_native_is_str(xs))
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
  if (is_v_str(xs) || rt_native_is_str(xs))
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

int64_t rt_list_len(int64_t lst) {
  if (!is_ptr(lst))
    return 0;
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC)
      return hdr[1];
  }
  if (is_heap_ptr(lst)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
      return is_int(len_v) ? (len_v >> 1) : len_v;
    }
  }
  int64_t tagged = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return (tagged & 1) ? (tagged >> 1) : tagged;
}

int64_t rt_list_set_len(int64_t lst, int64_t n) {
  if (!is_ptr(lst))
    return 0;
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if ((uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC) {
      hdr[1] = n;
      return lst;
    }
  }
  int64_t tagged = rt_tag_v(n);
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
  const char *file =
      is_v_str(g_trace_file) ? (const char *)(uintptr_t)g_trace_file : NULL;
  int64_t line = g_trace_line;
  int64_t col = g_trace_col;
  const char *loc_fmt = "";
  char loc_buf[256];
  if (file && line > 0) {
    int n = snprintf(loc_buf, sizeof(loc_buf), " at %s:%" PRId64 ":%" PRId64,
                     file, line, col);
    if (n > 0 && (size_t)n < sizeof(loc_buf))
      loc_fmt = loc_buf;
  }

  if (is_int(msg_ptr)) {
    fprintf(stderr, "%sPanicError:%s %" PRId64 "%s\n", red, rs,
            (int64_t)rt_untag_v(msg_ptr), loc_fmt);
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
    fprintf(stderr, "%s%s:%s %.*s%s\n", red, kind, rs, (int)msg_len, msg,
            loc_fmt);
  } else if (rt_native_is_str(msg_ptr)) {
    const char *msg = (const char *)(uintptr_t)msg_ptr;
    size_t msg_len = strlen(msg);
    const char *kind = "PanicError";
    fprintf(stderr, "%s%s:%s %.*s%s\n", red, kind, rs, (int)msg_len, msg,
            loc_fmt);
  } else if (is_v_err(msg_ptr)) {
    int64_t err = rt_unwrap(msg_ptr);
    fprintf(stderr, "%sNytrixError:%s ", red, rs);
    print_panic_msg(err);
  } else {
    fprintf(stderr, "%sPanicError:%s 0x%" PRIx64 "%s\n", red, rs,
            (uint64_t)msg_ptr, loc_fmt);
  }
}

static void print_runtime_debug_help(void) {
  const char *gray = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  fprintf(stderr,
          "%s\nDebug help:%s rerun with -trace, or set NYTRIX_TRACE=1 "
          "NYTRIX_TRACE_CALLS=1 NYTRIX_TRACE_VALUES=1 "
          "NYTRIX_TRACE_VERBOSE=1.\n",
          gray, rs);
  fprintf(stderr,
          "%sDebug filter:%s set "
          "NYTRIX_TRACE_FILTER=module_or_function_tail to "
          "limit very noisy traces.\n",
          gray, rs);
  fprintf(stderr,
          "%sImport/debug help:%s set NYTRIX_DIAG_UNDEF=1 for unresolved "
          "symbols, "
          "NYTRIX_TRACE_IMPORTS=1 for import loading, NYTRIX_STD_CACHE=0 "
          "to "
          "bypass "
          "std cache.\n",
          gray, rs);
}

static void print_ny_trace_frame(int64_t file, int64_t line, int64_t col,
                                 int64_t func) {
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
  fprintf(stderr, "%s  ... previous frame repeated %zu more time%s%s\n", gray,
          count, count == 1 ? "" : "s", rs);
}

int64_t rt_get_backtrace(int64_t count_v) {
  if (g_cs_depth == 0)
    return rt_list_new(0);
  size_t want = (size_t)(is_int(count_v) ? rt_untag_v(count_v) : count_v);
  if (want == 0 || want > g_cs_depth)
    want = g_cs_depth;
  int64_t lst =
      rt_list_new(is_int(want) ? (int64_t)want : rt_tag((int64_t)want));
  for (size_t i = 0; i < want; i++) {
    size_t idx = g_cs_depth - 1 - i;
    int64_t frame = rt_list_new(rt_tag(3));
    rt_append(frame, g_cs_files[idx]);
    rt_append(frame, g_cs_lines[idx]);
    rt_append(frame, g_cs_funcs[idx]);
    rt_append(lst, frame);
  }
  return lst;
}

int64_t rt_panic(int64_t msg_ptr) {
  if (msg_ptr && rt_native_is_str(msg_ptr))
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
      if (have_last && g_cs_files[idx] == last_file &&
          g_cs_lines[idx] == last_line && g_cs_funcs[idx] == last_func) {
        repeats++;
        continue;
      }
      print_ny_trace_repeat(repeats);
      repeats = 0;
      print_ny_trace_frame(g_cs_files[idx], g_cs_lines[idx], 1,
                           g_cs_funcs[idx]);
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
      if (have_last && g_trace_files[idx] == last_file &&
          g_trace_lines[idx] == last_line && g_trace_funcs[idx] == last_func) {
        repeats++;
        continue;
      }
      print_ny_trace_repeat(repeats);
      repeats = 0;
      print_ny_trace_frame(g_trace_files[idx], g_trace_lines[idx],
                           g_trace_cols[idx], g_trace_funcs[idx]);
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
  print_runtime_debug_help();
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
