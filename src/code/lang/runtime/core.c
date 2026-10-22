/*
 * Runtime core: NyValue constructors, tagged-value ops, type checks,
 * comparison, hashing, printing, and the shared runtime dispatch table.
 */
#include "base/common.h"
#include "code/lang/runtime/shared.h"
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef _WIN32
#include <io.h>
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#include <immintrin.h>
#define NY_DICT_USE_SSE2 1
#endif

__thread uintptr_t rt_float_cache_keys[RT_FLOAT_CACHE_SIZE] = {0};
__thread uintptr_t rt_non_float_cache_keys[RT_FLOAT_CACHE_SIZE] = {0};

extern int64_t rt_lt(int64_t a, int64_t b);
extern int64_t rt_bigint_add(int64_t a, int64_t b);
extern int64_t rt_bigint_sub(int64_t a, int64_t b);
extern int64_t rt_bigint_mul(int64_t a, int64_t b);
extern int64_t rt_bigint_div(int64_t a, int64_t b);
extern int64_t rt_bigint_mod(int64_t a, int64_t b);
extern int64_t rt_bigint_from_i64_raw(int64_t value);
extern int64_t rt_magic_tbuf_elem_size(int64_t v);
static bool rt_word_is_plain_scalar(int64_t value);

/*
 * Raw typed buffers have no managed-heap header, so probing arbitrary dynamic
 * pointers for the 32-byte tbuf header is unsafe. Keep provenance for the
 * handles created by the raw allocator and only decode headers for registered
 * data pointers. A tbuf released through free_raw is removed from this bounded
 * registry; tombstones preserve open-addressing probe chains.
 * The registry covers transient compiler-generated buffers as well as
 * long-lived containers.  Keep enough entries for the largest benchmark
 * allocation burst without turning a full registry into a false OOB result.
 */
#define RT_TBUF_HANDLE_LIMIT (1u << 22)
static _Atomic(uintptr_t) rt_tbuf_handles[RT_TBUF_HANDLE_LIMIT];
#define RT_TBUF_HANDLE_FLAG_MASK ((uintptr_t)0x07u)
#define RT_TBUF_HANDLE_FREEABLE ((uintptr_t)0x01u)
#define RT_TBUF_HANDLE_STATIC ((uintptr_t)0x02u)
#define RT_TBUF_HANDLE_TOMBSTONE ((uintptr_t)0x03u)
static _Atomic uint64_t rt_tbuf_handle_epoch = 1;
/*
 * Changes only when a payload address is forwarded to a relocated buffer.
 */

/*
 * Direct-mapped read cache for the registry. Releasing any tbuf advances an
 * epoch so every thread invalidates stale cache entries before using one;
 * rt_tbuf_check remains the sole magic-header authority. This keeps the O(N)
 * atomic probe (up to 2^22 slots) out of typed element access.
 */
#define RT_TBUF_HANDLE_CACHE_BITS 12
#define RT_TBUF_HANDLE_CACHE_SIZE (1u << RT_TBUF_HANDLE_CACHE_BITS)
#define RT_TBUF_HANDLE_CACHE_MASK (RT_TBUF_HANDLE_CACHE_SIZE - 1u)
static __thread uintptr_t rt_tbuf_handle_cache[RT_TBUF_HANDLE_CACHE_SIZE];
static __thread uint64_t rt_tbuf_handle_cache_epoch;
/*
 * Repeated typed indexing commonly uses one buffer for an entire hot loop.
 */
static __thread uintptr_t rt_tbuf_last_fast_handle;
static __thread uint64_t rt_tbuf_last_fast_epoch;

rt_oracle_range_t g_rt_oracle_snapshot[RT_ORACLE_SNAPSHOT_MAX] = {{0, 0}};
uint32_t g_rt_oracle_snapshot_count = 0;
rt_oracle_range_t g_rt_oracle_extra[RT_ORACLE_EXTRA_MAX] = {{0, 0}};
_Atomic uint32_t g_rt_oracle_extra_count = 0;
_Atomic uintptr_t g_rt_oracle_lo = 0;
_Atomic uintptr_t g_rt_oracle_hi = 0;
_Atomic uint32_t g_rt_oracle_ready = 0;
_Atomic uint32_t g_rt_oracle_pending = 0;
_Atomic uint32_t g_rt_oracle_full = 0;
const uintptr_t g_rt_oracle_gap = RT_ORACLE_GAP_DEFAULT;

#ifdef __linux__
static bool rt_map_parse_hex(const char **cursor, uintptr_t *value) {
  const char *p = *cursor;
  uintptr_t result = 0;
  bool seen = false;
  for (;;) {
    unsigned char c = (unsigned char)*p;
    unsigned digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      break;
    if (result > (UINTPTR_MAX >> 4))
      return false;
    result = (result << 4) | digit;
    seen = true;
    ++p;
  }
  if (!seen)
    return false;
  *cursor = p;
  *value = result;
  return true;
}

/*
 * Only the address range and readable permission are needed from maps.
 */
static bool rt_map_parse_readable_line(const char *line,
                                       rt_oracle_range_t *range) {
  const char *p = line;
  uintptr_t start, end;
  if (!rt_map_parse_hex(&p, &start) || *p++ != '-' ||
      !rt_map_parse_hex(&p, &end) || (*p != ' ' && *p != '\t'))
    return false;
  while (*p == ' ' || *p == '\t')
    ++p;
  if (*p != 'r' || end <= start)
    return false;
  range->start = start;
  range->end = end;
  return true;
}
#endif

void rt_map_oracle_init(void) {
  if (atomic_load_explicit(&g_rt_oracle_ready, memory_order_relaxed))
    return;
#ifdef __linux__
  FILE *f = fopen("/proc/self/maps", "r");
  if (f) {
    char line[512];
    uint32_t n = 0;
    while (n < RT_ORACLE_SNAPSHOT_MAX && fgets(line, sizeof line, f)) {
      rt_oracle_range_t range;
      if (rt_map_parse_readable_line(line, &range))
        g_rt_oracle_snapshot[n++] = range;
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
  /*
   * Adjacent allocations usually share the most recently registered pages.
   * Check that same cached range before scanning the other dedup entries.
   */
  uint32_t recent = (dedup_idx - 1u) % RT_ORACLE_DEDUP;
  if (dedup_e[recent] && s >= dedup_s[recent] && e <= dedup_e[recent])
    return;
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
    if (atomic_compare_exchange_weak_explicit(&g_rt_oracle_hi, &cur, e,
                                              memory_order_release,
                                              memory_order_relaxed))
      break;
  }
  cur = atomic_load_explicit(&g_rt_oracle_lo, memory_order_relaxed);
  while (s < cur) {
    if (atomic_compare_exchange_weak_explicit(&g_rt_oracle_lo, &cur, s,
                                              memory_order_release,
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
  uint64_t epoch =
      atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire);
  if (rt_tbuf_handle_cache_epoch != epoch) {
    memset(rt_tbuf_handle_cache, 0, sizeof(rt_tbuf_handle_cache));
    rt_tbuf_handle_cache_epoch = epoch;
  }
  rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] = data;
}

static bool rt_tbuf_header_valid(const int64_t *header);

static void rt_tbuf_register_handle(uintptr_t data, bool freeable) {
  if (!data || (data & RT_TBUF_HANDLE_FLAG_MASK) != 0)
    return;
  uintptr_t value =
      data | (freeable ? RT_TBUF_HANDLE_FREEABLE : RT_TBUF_HANDLE_STATIC);
  size_t start = rt_tbuf_handle_slot(data);
  size_t first_tombstone = SIZE_MAX;
  for (size_t probe = 0; probe < RT_TBUF_HANDLE_LIMIT; ++probe) {
    size_t i = (start + probe) & (RT_TBUF_HANDLE_LIMIT - 1u);
    uintptr_t seen =
        atomic_load_explicit(&rt_tbuf_handles[i], memory_order_acquire);
    uintptr_t seen_data = seen & ~RT_TBUF_HANDLE_FLAG_MASK;
    if (seen_data == data && (seen & RT_TBUF_HANDLE_FLAG_MASK) != 0) {
      rt_tbuf_cache_handle(data);
      return;
    }
    if (seen == RT_TBUF_HANDLE_TOMBSTONE && first_tombstone == SIZE_MAX)
      first_tombstone = i;
    if (seen != 0)
      continue;
    size_t insert_at = first_tombstone == SIZE_MAX ? i : first_tombstone;
    uintptr_t expected =
        first_tombstone == SIZE_MAX ? 0 : RT_TBUF_HANDLE_TOMBSTONE;
    if (atomic_compare_exchange_strong_explicit(
            &rt_tbuf_handles[insert_at], &expected, value, memory_order_acq_rel,
            memory_order_acquire)) {
      rt_tbuf_cache_handle(data);
      return;
    }
    /*
     * A concurrent insertion/removal changed the probe chain; restart.
     */
    probe = (size_t)-1;
    first_tombstone = SIZE_MAX;
  }
  if (first_tombstone != SIZE_MAX) {
    uintptr_t expected = RT_TBUF_HANDLE_TOMBSTONE;
    if (atomic_compare_exchange_strong_explicit(
            &rt_tbuf_handles[first_tombstone], &expected, value,
            memory_order_acq_rel, memory_order_acquire))
      rt_tbuf_cache_handle(data);
  }
}

static bool rt_tbuf_registered_handle(uintptr_t data) {
  if (!data)
    return false;
  uint64_t epoch =
      atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire);
  if (rt_tbuf_handle_cache_epoch != epoch) {
    memset(rt_tbuf_handle_cache, 0, sizeof(rt_tbuf_handle_cache));
    rt_tbuf_handle_cache_epoch = epoch;
  }
  if (rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] == data &&
      atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire) ==
          epoch)
    return true;
  size_t start = rt_tbuf_handle_slot(data);
  for (size_t probe = 0; probe < RT_TBUF_HANDLE_LIMIT; ++probe) {
    size_t i = (start + probe) & (RT_TBUF_HANDLE_LIMIT - 1u);
    uintptr_t seen =
        atomic_load_explicit(&rt_tbuf_handles[i], memory_order_acquire);
    uintptr_t seen_data = seen & ~RT_TBUF_HANDLE_FLAG_MASK;
    if (seen_data == data && (seen & RT_TBUF_HANDLE_FLAG_MASK) != 0) {
      rt_tbuf_cache_handle(data);
      return true;
    }
    if (!seen)
      break;
  }
  return false;
}

static bool rt_tbuf_cached_handle(uintptr_t data) {
  if (!data)
    return false;
  uint64_t epoch =
      atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire);
  if (rt_tbuf_handle_cache_epoch != epoch) {
    memset(rt_tbuf_handle_cache, 0, sizeof(rt_tbuf_handle_cache));
    rt_tbuf_handle_cache_epoch = epoch;
  }
  return rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] == data &&
         atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire) ==
             epoch;
}

/*
 * Store hot paths may skip the registry lookup on a current cache hit, but
 * must not consume stale entries or change the refresh semantics of other
 * readers such as rt_len.
 */
static bool rt_tbuf_cached_handle_fast(uintptr_t data) {
  if (!data)
    return false;
  uint64_t epoch =
      atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire);
  if (rt_tbuf_last_fast_handle == data && rt_tbuf_last_fast_epoch == epoch)
    return atomic_load_explicit(&rt_tbuf_handle_epoch, memory_order_acquire) ==
           epoch;
  bool cached = rt_tbuf_handle_cache_epoch == epoch &&
                rt_tbuf_handle_cache[rt_tbuf_handle_cache_slot(data)] == data;
  if (!cached || atomic_load_explicit(&rt_tbuf_handle_epoch,
                                      memory_order_acquire) != epoch)
    return false;
  rt_tbuf_last_fast_handle = data;
  rt_tbuf_last_fast_epoch = epoch;
  return true;
}

/*
 * Remove a dynamically allocated tbuf before its storage is released.
 */
int rt_tbuf_release_handle(int64_t value) {
  uintptr_t data = (uintptr_t)value;
  if (!data || (data & RT_TBUF_HANDLE_FLAG_MASK) != 0)
    return 0;
  size_t start = rt_tbuf_handle_slot(data);
  for (size_t probe = 0; probe < RT_TBUF_HANDLE_LIMIT; ++probe) {
    size_t i = (start + probe) & (RT_TBUF_HANDLE_LIMIT - 1u);
    uintptr_t seen =
        atomic_load_explicit(&rt_tbuf_handles[i], memory_order_acquire);
    if (seen == 0)
      return 0;
    if ((seen & ~RT_TBUF_HANDLE_FLAG_MASK) != data)
      continue;
    bool freeable =
        (seen & RT_TBUF_HANDLE_FLAG_MASK) == RT_TBUF_HANDLE_FREEABLE;
    if (!atomic_compare_exchange_strong_explicit(
            &rt_tbuf_handles[i], &seen, RT_TBUF_HANDLE_TOMBSTONE,
            memory_order_acq_rel, memory_order_acquire))
      return 0;
    atomic_fetch_add_explicit(&rt_tbuf_handle_epoch, 1, memory_order_acq_rel);
    return freeable ? 1 : -1;
  }
  return 0;
}

/*
 * Native list values are passed as data pointers for the typed fast path, but
 * dynamic mutation may grow or widen the backing storage. Keep the old value
 * as a forwarding header so aliases observe the replacement instead of a
 * stale length/payload. This gives raw buffers reference semantics without
 * teaching individual library functions about particular callers.
 */
static int64_t rt_tbuf_resolve_handle(int64_t buffer) {
  const int64_t *header = NULL;
  if (rt_tbuf_check(buffer, &header) == RT_TBUF_OK)
    return (int64_t)((uintptr_t)header + RT_NATIVE_TBUF_HEADER);
  return buffer;
}

static void rt_tbuf_replace_handle(uintptr_t old_data, uintptr_t new_data) {
  if (old_data && new_data && old_data != new_data) {
    uintptr_t header = old_data - RT_NATIVE_TBUF_HEADER;
    if (rt_header_readable_cached(header, RT_NATIVE_TBUF_HEADER)) {
      int64_t *hdr = (int64_t *)header;
      if (rt_tbuf_header_valid(hdr)) {
        hdr[0] = (int64_t)NY_NATIVE_TBUF_FORWARD_MAGIC;
        hdr[1] = (int64_t)new_data;
      }
    }
  }
  rt_tbuf_register_handle(new_data, true);
}

/*
 * The fourth native-buffer header word is capacity.  Reserve its sign bit
 * for the sequence's outer tag; capacities are bounded to the positive
 * half-range by rt_tbuf_capacity_fits.  This keeps list and tuple values
 * distinguishable after they cross the raw native-buffer ABI without a
 * side-table or source-specific lowering rule.
 */
#define RT_TBUF_TUPLE_FLAG INT64_MIN
static int64_t rt_tbuf_capacity_word(int64_t word) { return word & INT64_MAX; }
static bool rt_tbuf_word_is_tuple(int64_t word) {
  return (word & RT_TBUF_TUPLE_FLAG) != 0;
}

static int64_t rt_sequence_tag(int64_t v);
static int64_t rt_list_copy_with_tag(int64_t src, int64_t tag);
typedef struct ny_native_dict_t ny_native_dict_t;
static ny_native_dict_t *ny_native_dict_ptr(int64_t value);
static bool rt_set_layout(int64_t value);

static bool rt_tbuf_known_handle(uintptr_t data) {
  return rt_tbuf_registered_handle(data);
}

int64_t rt_tbuf_register_static_handle(int64_t value) {
  if (value > 0)
    rt_tbuf_register_handle((uintptr_t)value, false);
  return value;
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
 * The public core.tbuf API returns the data pointer. Its 32-byte header
 * is magic, count, element size, and capacity, in that order.
 */

int64_t rt_zalloc_raw(int64_t size) {
  if (size <= 0)
    size = 1;
  void *p = calloc(1, (size_t)size);
  if (!p)
    return 0;
  /*
   * Native raw allocations must not be mistaken for a traced-heap header.
   */
  rt_map_oracle_add((uintptr_t)p, (size_t)size);
  rt_raw_ptr_register_range((int64_t)(uintptr_t)p, (size_t)size);
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

static int64_t rt_tbuf_new_impl(int64_t count, int64_t elem_size, int tracked,
                                bool zero_elements) {
  if (count < 0)
    count = 0;
  if (elem_size <= 0)
    elem_size = 1;
  int64_t capacity = count < 8 ? 8 : count;
  if (!rt_tbuf_capacity_fits(capacity, elem_size))
    return 0;
  size_t bytes = RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  unsigned char *base =
      zero_elements ? calloc(1, bytes) : (unsigned char *)malloc(bytes);
  if (!base)
    return 0;
  int64_t *hdr = (int64_t *)base;
  hdr[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
  hdr[1] = count;
  hdr[2] = elem_size;
  hdr[3] = capacity;
  if (tracked) {
    rt_map_oracle_add((uintptr_t)base, bytes);
  }
  if (tracked)
    rt_tbuf_register_handle((uintptr_t)(base + RT_NATIVE_TBUF_HEADER), true);
  return (int64_t)(uintptr_t)(base + RT_NATIVE_TBUF_HEADER);
}

int64_t rt_tbuf_new_raw(int64_t count, int64_t elem_size) {
  return rt_tbuf_new_impl(count, elem_size, 1, true);
}

/*
 * Typed buffers have a statically-known ptr ABI and are addressed directly by
 * load/store lowering.  They still carry the normal tbuf header (so tbuf_len
 * and tbuf_ptr remain valid), but do not need registration in the dynamic
 * handle oracle.  Avoiding those global tracking structures matters for
 * allocation-heavy numeric kernels such as HMAC/PBKDF2.
 *
 * Small typed buffers are compiler-facing raw workspaces.  They are not
 * registered with the dynamic handle oracle and the public tbuf API has no
 * per-buffer destructor, so their existing lifetime is already the lifetime
 * of the process/thread arena.  Serving the common small allocations from a
 * zero-filled TLS slab avoids repeated calloc metadata/locking in numeric
 * kernels (PBKDF2/HMAC is particularly allocation dense) without changing the
 * tracked/dynamic tbuf allocator or its realloc semantics.
 *
 * The slab is append-only: bytes are never recycled, which preserves the
 * previous allocation identity/lifetime.  Oversized or overflow-prone
 * requests continue through the ordinary calloc path.
 */
#define RT_TYPED_TBUF_SLAB_BYTES (1024u * 1024u)
#define RT_TYPED_TBUF_SLAB_MAX_ALLOC (64u * 1024u)

typedef struct rt_typed_tbuf_slab_chunk {
  struct rt_typed_tbuf_slab_chunk *next;
  size_t used;
  size_t capacity;
  max_align_t _align;
  unsigned char data[];
} rt_typed_tbuf_slab_chunk_t;

static _Thread_local rt_typed_tbuf_slab_chunk_t *rt_typed_tbuf_slab;

static void *rt_typed_tbuf_slab_alloc(size_t bytes) {
  const size_t align = _Alignof(max_align_t);
  if (bytes == 0 || bytes > RT_TYPED_TBUF_SLAB_MAX_ALLOC)
    return NULL;
  if (bytes > SIZE_MAX - (align - 1u))
    return NULL;
  bytes = (bytes + align - 1u) & ~(align - 1u);
  rt_typed_tbuf_slab_chunk_t *chunk = rt_typed_tbuf_slab;
  if (!chunk || chunk->capacity - chunk->used < bytes) {
    size_t capacity = RT_TYPED_TBUF_SLAB_BYTES;
    if (capacity < bytes)
      capacity = bytes;
    if (capacity > SIZE_MAX - sizeof(*chunk))
      return NULL;
    /*
     * calloc is intentional: every newly exposed typed-buffer byte must be
     * zero, matching i64buf_new/f64buf_new's public contract.
     */
    chunk = (rt_typed_tbuf_slab_chunk_t *)calloc(1, sizeof(*chunk) + capacity);
    if (!chunk)
      return NULL;
    chunk->capacity = capacity;
    chunk->next = rt_typed_tbuf_slab;
    rt_typed_tbuf_slab = chunk;
  }
  void *out = chunk->data + chunk->used;
  chunk->used += bytes;
  return out;
}

int64_t rt_tbuf_new_typed_raw(int64_t count, int64_t elem_size) {
  if (count < 0)
    count = 0;
  if (elem_size <= 0)
    elem_size = 1;
  int64_t capacity = count < 8 ? 8 : count;
  if (!rt_tbuf_capacity_fits(capacity, elem_size))
    return 0;
  size_t bytes = RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  unsigned char *base = (unsigned char *)rt_typed_tbuf_slab_alloc(bytes);
  if (!base)
    return rt_tbuf_new_impl(count, elem_size, 0, true);
  int64_t *hdr = (int64_t *)base;
  hdr[0] = (int64_t)NY_NATIVE_TBUF_MAGIC;
  hdr[1] = count;
  hdr[2] = elem_size;
  hdr[3] = capacity;
  return (int64_t)(uintptr_t)(base + RT_NATIVE_TBUF_HEADER);
}

int64_t rt_tbuf_append_raw(int64_t buffer, int64_t value, int64_t is_string) {
  buffer = rt_tbuf_resolve_handle(buffer);
  /*
   * Raw append is also used by older lowered member calls.  Recover string
   * provenance here before the elem-8 fast path, otherwise a string inserted
   * through that ABI is stored as an untyped machine word and later dynamic
   * indexing cannot distinguish it from an integer.
   */
  if (!is_string && (rt_native_is_str(value) || is_v_str(value)))
    is_string = 1;
  if (is_int(buffer)) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("invalid typed-buffer handle"));
    return 0;
  }
  if (!buffer)
    return 0;
  int64_t *base = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(base)) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("invalid typed-buffer handle"));
    return 0;
  }
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = rt_tbuf_capacity_word(base[3]);
  /*
   * A string is a dynamic value even when the caller selected the raw
   * append entry point.  An elem-8 buffer has no per-slot provenance, so
   * keeping the compact representation would make later typed/dynamic reads
   * guess whether the word is a string, float, or scalar.  Promote through
   * the canonical tagged path at this representation boundary.
   */
  if (elem_size == 8 && is_string)
    return rt_tbuf_append_tagged(buffer, value, 1);
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
    int64_t new_capacity = capacity < 8 ? 8 : capacity;
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
    bool keep_alias = rt_tbuf_known_handle(old_data);
    unsigned char *grown = NULL;
    if (keep_alias) {
      grown = malloc(new_bytes);
      if (grown)
        memcpy(grown, raw_base, old_bytes);
    } else if (is_heap_ptr((int64_t)(uintptr_t)raw_base)) {
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
    base[3] = (base[3] & RT_TBUF_TUPLE_FLAG) | capacity;
    if (keep_alias)
      rt_tbuf_replace_handle(old_data,
                             (uintptr_t)(raw_base + RT_NATIVE_TBUF_HEADER));
    else
      rt_tbuf_register_handle((uintptr_t)(raw_base + RT_NATIVE_TBUF_HEADER),
                              true);
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
        (is_string || rt_native_is_str(value)) ? 121 : rt_raw_word_tag(value);
    /*
     * A boxed float arrives as its heap-slot pointer; the descriptor payload
     * must hold the IEEE bits, exactly like rt_tbuf_set_tagged's TAG_FLOAT
     * branch.  Storing the pointer with a TAG_FLOAT stamp made every
     * dynamic append of an `any`-held float read back as
     * FromBits(pointer) (the it.chain corruption).
     */
    if (tag == TAG_FLOAT) {
      int64_t bits = rt_f64_bits(rt_flt_unbox_double(value));
      memcpy(slot, &bits, sizeof(bits));
    }
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
    if (rt_native_is_str(decoded) || is_v_str(decoded)) {
      value = decoded;
      is_string = 1;
    }
  }
  buffer = rt_tbuf_resolve_handle(buffer);
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_magic_tbuf_elem_size(decoded) > 0)
      value = decoded;
  }
  int64_t original = value;
  bool untagged_int = false;
  if (!is_string && is_int(value) && rt_word_is_plain_scalar(value)) {
    value = rt_untag_v(value);
    untagged_int = true;
  }
  /*
   * A dynamic append upgrades an elem-8 buffer to descriptor storage before
   * the first value loses its provenance. This is the only representation
   * that can safely carry raw integers, nil/bools, floats, and pointers in
   * one sequence.  Validate and read the buffer header once for both the
   * elem-8 promotion and the descriptor append below; the historical shape
   * re-probed the same header between the two branches.
   */
  if (buffer &&
      (rt_tbuf_known_handle((uintptr_t)buffer) ||
       rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr) && hdr[2] == 8 && !untagged_int) {
      int64_t count = hdr[1];
      int64_t out = rt_tbuf_new_raw(count, 24);
      if (!out)
        return 0;
      for (int64_t i = 0; i < count; ++i) {
        int64_t raw = ((int64_t *)(uintptr_t)buffer)[i];
        unsigned char *slot = (unsigned char *)(uintptr_t)out + (size_t)i * 24;
        /*
         * An elem-8 list reached through append_i64_raw contains raw native
         * integers by construction.  Do not infer the descriptor tag from
         * the payload's low bit: raw 1/3/5 are data, not tagged 0/1/2.
         */
        int64_t tag =
            (rt_native_is_str(raw) || is_v_str(raw)) ? TAG_STR_CONST : 3;
        memcpy(slot, &raw, sizeof(raw));
        memcpy(slot + 16, &tag, sizeof(tag));
      }
      rt_tbuf_replace_handle((uintptr_t)buffer, (uintptr_t)out);
      buffer = out;
      if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                     RT_NATIVE_TBUF_HEADER))
        return rt_tbuf_append_raw(buffer, value, is_string);
      hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    }
    if (rt_tbuf_header_valid(hdr) && hdr[2] >= 24) {
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
        if (rt_tbuf_header_valid(ohdr) && ohdr[2] >= 24 && ohdr[1] > 0) {
          unsigned char *slot = (unsigned char *)(uintptr_t)out +
                                (size_t)(ohdr[1] - 1) * (size_t)ohdr[2];
          int64_t tag =
              is_string ? TAG_STR_CONST
                        : (rt_is_bool_imm(original) ? NY_TBUF_TAG_BOOL
                                                    : rt_value_tag(original));
          if (tag == 1)
            tag = rt_value_tag(value);
          /*
           * append_raw stored `value` verbatim; a boxed float's word is its
           * heap-slot pointer, so a TAG_FLOAT stamp without converting the
           * payload read back as FromBits(pointer) (the it.chain float
           * corruption).  Mirror rt_tbuf_set_tagged: TAG_FLOAT slots hold
           * the IEEE bits.
           */
          if (tag == TAG_FLOAT) {
            int64_t bits = rt_f64_bits(rt_flt_unbox_double(original));
            memcpy(slot, &bits, sizeof(bits));
          }
          memcpy(slot + 16, &tag, sizeof(tag));
        }
      }
      return out;
    }
  }
  return rt_tbuf_append_raw(buffer, value, is_string);
}

/*
 * Compact lists are sufficient while every element is a raw scalar, but an
 * `any` list must retain provenance for values such as booleans and floats.
 * Promote the existing allocation once at that representation boundary so
 * later dynamic reads do not guess from the payload bits.
 */
static int64_t rt_tbuf_promote_descriptor(int64_t buffer) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer || !rt_tbuf_known_handle((uintptr_t)buffer) ||
      !rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return buffer;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(hdr) || hdr[2] != 8)
    return buffer;
  int64_t count = hdr[1];
  int64_t capacity = rt_tbuf_capacity_word(hdr[3]);
  if (count < 0 || capacity < count || !rt_tbuf_capacity_fits(capacity, 24))
    return buffer;
  int64_t out = rt_tbuf_new_raw(capacity, 24);
  if (!out)
    return buffer;
  int64_t *out_hdr = (int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER);
  out_hdr[1] = count;
  out_hdr[3] = capacity | (hdr[3] & RT_TBUF_TUPLE_FLAG);
  for (int64_t i = 0; i < count; ++i) {
    int64_t raw = ((int64_t *)(uintptr_t)buffer)[i];
    unsigned char *slot = (unsigned char *)(uintptr_t)out + (size_t)i * 24;
    int64_t tag = (rt_native_is_str(raw) || is_v_str(raw))
                      ? TAG_STR_CONST
                      : rt_raw_word_tag(raw);
    int64_t len =
        (rt_native_is_str(raw) || is_v_str(raw)) ? rt_cstr_len(raw) : 0;
    memcpy(slot, &raw, sizeof(raw));
    memcpy(slot + 8, &len, sizeof(len));
    memcpy(slot + 16, &tag, sizeof(tag));
  }
  rt_tbuf_replace_handle((uintptr_t)buffer, (uintptr_t)out);
  return out;
}

int64_t rt_tbuf_append_i64_raw(int64_t buffer, int64_t value) {
  /*
   * Resolve forwarding aliases and retain the validated header from the same
   * check. Calling rt_tbuf_header_valid(base) after rt_tbuf_resolve_handle()
   * repeated the registry probe and full forwarding walk on every append.
   */
  int64_t *base = NULL;
  uintptr_t data_addr = (uintptr_t)buffer;
  if (data_addr > RT_NATIVE_TBUF_HEADER &&
      rt_tbuf_cached_handle_fast(data_addr)) {
    int64_t *cached = (int64_t *)(data_addr - RT_NATIVE_TBUF_HEADER);
    bool readable = true;
#if NY_WITH_ASAN
    readable = !__asan_region_is_poisoned(cached, RT_NATIVE_TBUF_HEADER);
#endif
    if (readable && (uint64_t)cached[0] == NY_NATIVE_TBUF_MAGIC &&
        cached[1] >= 0 && cached[2] > 0 &&
        rt_tbuf_capacity_word(cached[3]) >= cached[1])
      base = cached;
  }
  if (!base) {
    const int64_t *checked = NULL;
    if (rt_tbuf_check(buffer, &checked) != RT_TBUF_OK || !checked)
      return 0;
    base = (int64_t *)checked;
    buffer = (int64_t)((uintptr_t)checked + RT_NATIVE_TBUF_HEADER);
  }
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = rt_tbuf_capacity_word(base[3]);
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
  /*
   * An i64 fills an elem-8 slot completely. Avoid routing each hot append
   * through memset + memcpy when the direct store is the exact same write.
   */
  if (count < capacity && elem_size == (int64_t)sizeof(value)) {
    base[1] = count + 1;
    ((int64_t *)(uintptr_t)buffer)[count] = value;
    return buffer;
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
      int64_t tag = 3;
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
  int64_t new_capacity = capacity < 8 ? 8 : capacity;
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
  bool keep_alias = rt_tbuf_known_handle((uintptr_t)buffer);
  unsigned char *grown = NULL;
  if (keep_alias) {
    grown = malloc(new_bytes);
    if (grown)
      memcpy(grown, raw_base, old_bytes);
  } else if (is_heap_ptr((int64_t)(uintptr_t)raw_base)) {
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
  base[3] = (base[3] & RT_TBUF_TUPLE_FLAG) | new_capacity;
  unsigned char *slot = data + (size_t)count * (size_t)elem_size;
  size_t copy =
      (size_t)elem_size < sizeof(value) ? (size_t)elem_size : sizeof(value);
  memset(slot, 0, (size_t)elem_size);
  memcpy(slot, &value, copy);
  if (elem_size >= 24) {
    int64_t tag = 3;
    memcpy(slot + 16, &tag, sizeof(tag));
  }
  /*
   * realloc may move the buffer.  Every other growth site re-registers the
   * new data pointer; skipping it here left grown buffers unknown to
   * rt_tbuf_known_handle, so the next mapped[i]-style read missed the tbuf
   * path and panicked with "index_read out of range".
   */
  if (keep_alias)
    rt_tbuf_replace_handle((uintptr_t)buffer, (uintptr_t)data);
  else
    rt_tbuf_register_handle((uintptr_t)data, true);
  return (int64_t)(uintptr_t)data;
}

int64_t rt_tbuf_repeat(int64_t buffer, int64_t repeat_count) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  int64_t *base = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(base))
    return 0;
  int64_t count = base[1];
  int64_t elem_size = base[2];
  int64_t capacity = rt_tbuf_capacity_word(base[3]);
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
  buffer = rt_tbuf_resolve_handle(buffer);
  other = rt_tbuf_resolve_handle(other);
  if (!buffer || !other)
    return buffer;
  int64_t count = rt_tbuf_len_raw(other);
  for (int64_t i = 0; i < count; ++i) {
    int64_t item = rt_tbuf_get_any(other, i, 0);
    buffer = rt_tbuf_append_tagged(buffer, item, 1);
    if (!buffer)
      return 0;
  }
  return buffer;
}

int64_t rt_tbuf_concat_raw(int64_t left, int64_t right) {
  left = rt_tbuf_resolve_handle(left);
  right = rt_tbuf_resolve_handle(right);
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
      (base_l && rt_tbuf_header_valid(base_l)) ? base_l[2] : 24;
  int64_t elem_size_r =
      (base_r && rt_tbuf_header_valid(base_r)) ? base_r[2] : 24;
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
  int64_t heap = rt_heap_object_ptr(set_value);
  int64_t tag = 0;
  if (!heap || !rt_try_read_i64((uintptr_t)heap - 8, &tag) || tag != TAG_SET)
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
  bool key_is_string = rt_native_is_str(key) || is_v_str(key);
  if (key_is_string) {
    hash = 2166136261u;
    int64_t n = rt_cstr_len(key);
    const unsigned char *s = (const unsigned char *)(uintptr_t)key;
    for (int64_t i = 0; i < n; ++i)
      hash = ((hash ^ s[i]) * 16777619u) & 0x7fffffffu;
  }
  int64_t idx = (int64_t)(hash & (uint64_t)(cap - 1));
  for (int64_t probes = 0; probes < cap; ++probes) {
    int64_t off = 16 + idx * 24;
    int64_t state = *(int64_t *)((char *)(uintptr_t)set_value + off + 16);
    int64_t old = *(int64_t *)((char *)(uintptr_t)set_value + off);
    bool old_is_string = rt_native_is_str(old) || is_v_str(old);
    if (state == 1 && (old == key || (old_is_string && key_is_string &&
                                      rt_cstr_eq(old, key))))
      return set_value;
    if (state == 0 || state == 2) {
      *(int64_t *)((char *)(uintptr_t)set_value + off) = key;
      *(int64_t *)((char *)(uintptr_t)set_value + off + 8) = 1;
      *(int64_t *)((char *)(uintptr_t)set_value + off + 16) = 1;
      (*(int64_t *)((char *)(uintptr_t)set_value + 0))++;
      return set_value;
    }
    idx = (idx + 1) & (cap - 1);
  }
  return set_value;
}

/*
 * Public native boundary for the typed set method.  Keeping insertion,
 * resizing, hashing, and string equality in one runtime implementation makes
 * attached-method lowering use the same representation contract as `+` and
 * set constructors.
 */
int64_t rt_set_add_raw(int64_t set_value, int64_t key) {
  /*
   * A generic `set` local can arrive tagged even though its payload is a
   * managed set pointer.  The in-place helper mutates the decoded object but
   * returns its original argument; returning that tagged word under the
   * native typed-buffer ABI makes subsequent `.len` read the wrong value.
   */
  int64_t heap = rt_heap_object_ptr(set_value);
  int64_t result = heap ? rt_set_add_in_place(heap, key) : 0;
  return result;
}

static int64_t rt_add_raw_impl(int64_t left, int64_t right) {
  int64_t set_result = rt_set_add_in_place(left, right);
  if (set_result)
    return set_result;
  int dim_l = ny_vec_dim_impl(left);
  int dim_r = ny_vec_dim_impl(right);
  if (dim_l > 0 && dim_r > 0)
    return rt_vec_add_raw(left, right);
  /*
   * Dynamic sequence parameters arrive as managed strings after the native
   * call boundary has materialized their length/tag metadata.  Keep those
   * values on the managed string path: treating their addresses as raw
   * integers silently turns `+` into pointer arithmetic.  The string helper
   * also handles mixed managed/raw values through the common formatter.
   */
  if (is_v_str(left) || is_v_str(right))
    return rt_str_concat(left, right);
  if (rt_native_is_str(left) || rt_native_is_str(right)) {
    return rt_cstr_concat(rt_native_is_str(left) ? left : rt_any_to_cstr(left),
                          rt_native_is_str(right) ? right
                                                  : rt_any_to_cstr(right));
  }
  if (left || right) {
    bool is_tbuf_l = rt_tbuf_check(left, NULL) == RT_TBUF_OK;
    bool is_tbuf_r = rt_tbuf_check(right, NULL) == RT_TBUF_OK;
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
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_add(rt_bigint_operand(left), rt_bigint_operand(right));
  return rt_add_raw_impl(left, right);
}

int64_t rt_any_to_i64(int64_t value) {
  if (is_v_flt(value))
    return (int64_t)rt_flt_unbox_double(value);
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
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
   * Aligned pointer-shaped words already have the raw-i64 representation this
   * helper promises. Check heap BigInts first because they are the one managed
   * heap pointer that must be converted to a scalar. Native typed-buffer
   * handles are also even pointers, so validating their buffer header here is
   * unnecessary; keep the explicit validation below for odd handles, which
   * overlap the tagged-integer bit and cannot use the generic pointer test.
   */
  if (is_heap_ptr(value) && rt_value_tag(value) == TAG_BIGINT)
    return rt_bigint_to_i64_raw(value);
  if (is_any_ptr(value))
    return value;
  /*
   * Odd dynamic container handles overlap the tagged-integer bit. Check these
   * native buffer/header forms before the low-bit tagged-integer test;
   * otherwise a form/list returned by a callback is mistaken for an integer
   * and shifted.
   */
  if (rt_magic_tbuf_elem_size(value) > 0)
    return value;
  if (is_int(value))
    return rt_untag_v(value);
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
  if (!is_int(left) && rt_value_tag(left) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) + rt_any_to_f64(right));
  if (!is_int(right) && rt_value_tag(right) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) + rt_any_to_f64(right));
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_add(rt_bigint_operand(left), rt_bigint_operand(right));
  /*
   * Boxed floats can be pointer-shaped values with the low bit set.  They
   * must reach the float implementation before the tagged-integer test.
   */
  if (is_v_flt(left) || is_v_flt(right))
    return rt_add_raw_impl(left, right);
  bool left_raw_ptr =
      is_any_ptr(left) && !rt_native_is_str(left) && !is_v_str(left) &&
      rt_magic_tbuf_elem_size(left) <= 0 && !NY_DYNAMIC_CALLABLE_IS(left) &&
      !NY_NATIVE_IS(left) &&
      (rt_value_tag(left) == 0 || rt_value_tag(left) == TAG_CLOSURE);
  bool right_raw_ptr =
      is_any_ptr(right) && !rt_native_is_str(right) && !is_v_str(right) &&
      rt_magic_tbuf_elem_size(right) <= 0 && !NY_DYNAMIC_CALLABLE_IS(right) &&
      !NY_NATIVE_IS(right) &&
      (rt_value_tag(right) == 0 || rt_value_tag(right) == TAG_CLOSURE);
  if (left_raw_ptr && is_int(right))
    return rt_tag_v(left + rt_untag_v(right));
  if (is_int(left) && right_raw_ptr)
    return rt_tag_v(right + rt_untag_v(left));
  if (is_int(left) && is_int(right)) {
    int64_t l = rt_untag_v(left);
    int64_t r = rt_untag_v(right);
    int64_t sum = 0;
    if (!__builtin_add_overflow(l, r, &sum) && ny_small_int_fits_i64(sum)) {
      return rt_tag_v(sum);
    }
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
  buffer = rt_tbuf_resolve_handle(buffer);
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
  /*
   * A current handle-cache entry proves allocator provenance for this
   * canonical payload. Avoid repeated forwarding/registry checks for length
   * queries in typed-list loops, retaining the full resolver for other
   * representations and malformed headers.
   */
  uintptr_t data_addr = (uintptr_t)buffer;
  if (data_addr > RT_NATIVE_TBUF_HEADER &&
      rt_tbuf_cached_handle_fast(data_addr)) {
    const int64_t *hdr = (const int64_t *)(data_addr - RT_NATIVE_TBUF_HEADER);
    bool readable = true;
#if NY_WITH_ASAN
    readable = !__asan_region_is_poisoned(hdr, RT_NATIVE_TBUF_HEADER);
#endif
    if (readable && (uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC && hdr[1] >= 0 &&
        hdr[2] > 0 && rt_tbuf_capacity_word(hdr[3]) >= hdr[1])
      return hdr[1];
  }
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  /*
   * Static literal buffers are compiler-emitted tbufs too, but they are not
   * entered in the dynamic handle registry.  The readable-header check is
   * the safe provenance gate for both forms.
   */
  if (rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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
  if (rt_sequence_tag(buffer) == TAG_BYTES)
    return rt_len(buffer);
  return 0;
}
int64_t rt_tbuf_reserve(int64_t buffer, int64_t capacity) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer || capacity < 0 ||
      !rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return buffer;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(hdr))
    return buffer;
  int64_t count = hdr[1], elem_size = hdr[2];

  int64_t old_capacity = rt_tbuf_capacity_word(hdr[3]);
  int64_t outer_flag = hdr[3] & RT_TBUF_TUPLE_FLAG;
  if (count < 0 || elem_size <= 0 || old_capacity < count ||
      capacity <= old_capacity || !rt_tbuf_capacity_fits(capacity, elem_size))
    return buffer;
  size_t old_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)old_capacity * (size_t)elem_size;
  size_t new_bytes =
      RT_NATIVE_TBUF_HEADER + (size_t)capacity * (size_t)elem_size;
  bool keep_alias = rt_tbuf_known_handle((uintptr_t)buffer);
  unsigned char *grown = NULL;
  if (keep_alias) {
    grown = malloc(new_bytes);
    if (grown)
      memcpy(grown, (unsigned char *)hdr, old_bytes);
  } else if (is_heap_ptr((int64_t)(uintptr_t)hdr)) {
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
  new_hdr[3] = outer_flag | capacity;
  if (keep_alias)
    rt_tbuf_replace_handle((uintptr_t)buffer,
                           (uintptr_t)(grown + RT_NATIVE_TBUF_HEADER));
  else
    rt_tbuf_register_handle((uintptr_t)(grown + RT_NATIVE_TBUF_HEADER), true);
  return (int64_t)(uintptr_t)(grown + RT_NATIVE_TBUF_HEADER);
}
int64_t rt_tbuf_clear_raw(int64_t buffer) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return buffer;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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

/*
 * Clear the dynamic container surface without relying on which attached
 * overload the frontend selected.  In particular, tuples are immutable in
 * the language and therefore produce a fresh empty tuple rather than being
 * treated as mutable lists.
 */
int64_t rt_clear_raw(int64_t value) {
  int64_t tag = rt_sequence_tag(value);
  if (tag == TAG_TUPLE) {
    int64_t elem = rt_magic_tbuf_elem_size(value);
    int64_t out = rt_tbuf_new_raw(0, elem > 0 ? elem : 24);
    return out ? rt_list_as_tuple(out) : value;
  }
  if (tag == TAG_LIST)
    return rt_magic_tbuf_elem_size(value) > 0 ? rt_tbuf_clear_raw(value)
                                              : value;
  if (ny_native_dict_ptr(value))
    return rt_dict_clear_raw(value);
  if (rt_set_layout(value)) {
    int64_t cap = *(int64_t *)((uintptr_t)value + 8);
    if (is_int(cap))
      cap = rt_untag_v(cap);
    if (cap > 0 && (uint64_t)cap <= (SIZE_MAX - 16u) / 24u)
      memset((unsigned char *)(uintptr_t)value + 16, 0, (size_t)cap * 24u);
    *(int64_t *)(uintptr_t)value = 0;
    return value;
  }
  if (rt_native_is_str(value) || is_v_str(value))
    return rt_alloc_string_len("", 0);
  return value;
}
int64_t rt_tbuf_eq_raw(int64_t left, int64_t right) {
  left = rt_tbuf_resolve_handle(left);
  right = rt_tbuf_resolve_handle(right);
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
    int64_t item_eq = rt_any_eq(vl, vr);
    if (item_eq != NY_IMM_TRUE)
      return NY_IMM_FALSE;
  }
  return NY_IMM_TRUE;
}
int64_t rt_tbuf_clone_raw(int64_t buffer) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(hdr))
    return 0;
  int64_t count = hdr[1], elem_size = hdr[2];
  if (count < 0 || elem_size <= 0 ||
      (uint64_t)count > SIZE_MAX / (uint64_t)elem_size)
    return 0;
  int64_t out = rt_tbuf_new_raw(count, elem_size);
  if (out && count)
    memcpy((void *)(uintptr_t)out, (void *)(uintptr_t)buffer,
           (size_t)count * (size_t)elem_size);
  if (out) {
    int64_t *out_hdr = (int64_t *)((uintptr_t)out - RT_NATIVE_TBUF_HEADER);
    out_hdr[3] =
        (hdr[3] & RT_TBUF_TUPLE_FLAG) | rt_tbuf_capacity_word(out_hdr[3]);
  }
  return out;
}

/*
 * Clone at the dynamic sequence/object boundary.  The interpreter and native
 * backends both expose clone(any), so the operation must inspect the value's
 * representation rather than selecting one container ABI from a source-level
 * overload.
 */
int64_t rt_clone_raw(int64_t value) {
  int64_t seq_tag = rt_sequence_tag(value);
  if (seq_tag == TAG_LIST || seq_tag == TAG_TUPLE) {
    if (rt_magic_tbuf_elem_size(value) > 0)
      return rt_tbuf_clone_raw(value);
    return rt_list_copy_with_tag(value, seq_tag);
  }
  if (ny_native_dict_ptr(value))
    return rt_dict_clone_raw(value);
  return value;
}
int64_t rt_tbuf_swap(int64_t buffer, int64_t left, int64_t right) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return buffer;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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
    return 0;
  /*
   * Indexed stores dominate typed-list loops. A successful cache probe is
   * allocator provenance for this exact, canonical payload pointer, so check
   * the compact header in place and avoid three registry/cache probes through
   * rt_tbuf_known_handle + rt_tbuf_header_valid. Forwarding/stale or malformed
   * entries continue through the full resolver below.
   */
  uintptr_t data_addr = (uintptr_t)buffer;
  if (data_addr > RT_NATIVE_TBUF_HEADER &&
      rt_tbuf_cached_handle_fast(data_addr)) {
    int64_t *hdr = (int64_t *)(data_addr - RT_NATIVE_TBUF_HEADER);
    bool header_readable = true;
#if NY_WITH_ASAN
    header_readable = !__asan_region_is_poisoned(hdr, RT_NATIVE_TBUF_HEADER);
#endif
    if (header_readable && (uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC &&
        hdr[1] >= 0 && hdr[2] > 0 && rt_tbuf_capacity_word(hdr[3]) >= hdr[1]) {
      int64_t count = hdr[1], elem_size = hdr[2];
      int64_t capacity = rt_tbuf_capacity_word(hdr[3]);
      if (index < 0)
        index += count;
      if (index >= 0 && index < count && elem_size == 8) {
        ((int64_t *)data_addr)[index] = value;
        return buffer;
      }
      if (index >= 0 && index < capacity && elem_size >= 8 &&
          (uint64_t)index <= SIZE_MAX / (uint64_t)elem_size) {
        unsigned char *data = (unsigned char *)data_addr;
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
        return buffer;
      }
    }
  }
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1], elem_size = hdr[2];
      int64_t capacity = rt_tbuf_capacity_word(hdr[3]);
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
        rt_fail(rt_alloc_string("err.index"), rt_alloc_string(msg));
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
        rt_fail(rt_alloc_string("err.index"), rt_alloc_string(msg));
      }
      return buffer;
    }
  }
  /*
   * This raw entry returns a native buffer handle, so failure is raw null.
   */
  return 0;
}

/*
 * Store an explicitly-typed float in a descriptor tbuf.
 */
int64_t rt_tbuf_set_f64_bits(int64_t buffer, int64_t index, int64_t bits) {
  if (!buffer)
    return 0;
  if (rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      /*
       * A compact word buffer has no per-element type metadata.  Widen it
       * before writing f64 bits so dynamic reads can distinguish a float
       * from an integer with the same payload.
       */
      if (hdr[2] == 8) {
        buffer = rt_tbuf_promote_descriptor(buffer);
        hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
      }
      int64_t count = hdr[1], elem_size = hdr[2];
      int64_t capacity = rt_tbuf_capacity_word(hdr[3]);
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
      rt_fail(rt_alloc_string("err.index"), rt_alloc_string(msg));
      return buffer;
    }
  }
  /*
   * The legacy managed-list bridge stores language values, not raw f64
   * payloads.  Preserve its tagged-value contract.
   */
  return rt_store_item_fast(buffer, rt_tag_v(index), rt_flt_box_val(bits));
}
/*
 * Store a dynamically tagged value into a descriptor buffer.  The NYIR
 * boundary supplies a raw index; keeping index and value contracts separate
 * is required because odd raw indices are valid positions.
 */
int64_t rt_tbuf_set_tagged(int64_t buffer, int64_t index, int64_t value) {
  /*
   * Native NYIR callers pass a raw machine index.  Only the value is
   * dynamically tagged here; untagging an odd raw index turns 1 into 0 and
   * aliases adjacent descriptor slots.
   */
  int64_t raw_index = index;
  buffer = rt_tbuf_resolve_handle(buffer);
  /*
   * Native lowering may transport a pointer-like dynamic value in the
   * compact NY_NATIVE form. Descriptor slots store language values and need
   * the pointed-to string/container identity, not the transport word.
   */
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_native_is_str(decoded) && !is_v_str(decoded))
      value = rt_cstr_to_str(decoded);
    else if (rt_magic_tbuf_elem_size(decoded) > 0 ||
             rt_heap_object_ptr(decoded) || is_v_str(decoded))
      value = decoded;
  }
  if (buffer &&
      (rt_tbuf_known_handle((uintptr_t)buffer) ||
       rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr) && hdr[2] == 8) {
      if (is_int(value) && value != NY_IMM_TRUE && value != NY_IMM_FALSE)
        return rt_tbuf_set_i64_raw(buffer, raw_index, rt_untag_v(value));
      int64_t promoted = rt_tbuf_promote_descriptor(buffer);
      if (promoted != buffer || !rt_header_readable_cached(
                                    (uintptr_t)promoted - RT_NATIVE_TBUF_HEADER,
                                    RT_NATIVE_TBUF_HEADER))
        return rt_tbuf_set_tagged(promoted, raw_index, value);
      int64_t *promoted_hdr =
          (int64_t *)((uintptr_t)promoted - RT_NATIVE_TBUF_HEADER);
      if (rt_tbuf_header_valid(promoted_hdr) && promoted_hdr[2] >= 24)
        return rt_tbuf_set_tagged(promoted, raw_index, value);
    }
    if (rt_tbuf_header_valid(hdr) && hdr[2] >= 24) {
      int64_t count = hdr[1], elem_size = hdr[2];
      int64_t capacity = rt_tbuf_capacity_word(hdr[3]);
      if (raw_index < 0)
        raw_index += count;
      if (raw_index < 0 || raw_index >= capacity) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "set index out of range: index=%" PRId64 ", size=%" PRId64,
                 raw_index, count);
        rt_fail(rt_alloc_string("err.index"), rt_alloc_string(msg));
        return 0;
      }
      unsigned char *slot = (unsigned char *)(uintptr_t)buffer +
                            (size_t)raw_index * (size_t)elem_size;
      int64_t tag =
          rt_is_bool_imm(value) ? NY_TBUF_TAG_BOOL : rt_value_tag(value);
      int64_t payload = value;
      /*
       * Descriptor slots carry a raw payload plus explicit metadata.  Keep
       * booleans as their immediate payloads; ordinary tagged integers are
       * untagged exactly once at this boundary.  Dynamic float boxes are
       * unboxed to their raw IEEE bits inline.
       */
      if (tag == TAG_FLOAT) {
        payload = rt_f64_bits(rt_flt_unbox_double(value));
        if (getenv("NY_DBG_STORE"))
          fprintf(stderr, "[set_tagged f] val=%llx is_v_flt=%d payload=%llx\n",
                  (unsigned long long)value, (int)is_v_flt(value),
                  (unsigned long long)payload);
      } else if (tag == 1 && is_int(value) && value != NY_IMM_TRUE &&
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
  int64_t result = rt_tbuf_set_i64_raw(buffer, raw_index, value);
  return result ? result : rt_tag_v(0);
}
int64_t rt_tbuf_get(int64_t buffer, int64_t index, int64_t fallback) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (is_int(buffer)) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("get expects a string, bytes, list, tuple, dict, "
                            "range, or vector, got int"));
    return fallback;
  }
  if (!buffer)
    return fallback;
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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

/*
 * Dynamic-ABI element read for thread/async argument packing: elem-8
 * lists hold raw machine words, so scalar ints are tagged exactly once;
 * 24-byte descriptor slots already carry the tagged payload and are
 * returned as-is.
 */
int64_t rt_tbuf_dyn_elem(int64_t buffer, int64_t index, int64_t want_dynamic) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0)
        index += count;
      if (index < 0 || index >= count)
        return 0;
      int64_t value = 0;
      memcpy(&value,
             (void *)((uintptr_t)buffer + (size_t)index * (size_t)elem_size),
             sizeof(value));
      if (elem_size >= 24) {
        int64_t tag = 0;
        memcpy(&tag,
               (void *)((uintptr_t)buffer + (size_t)index * (size_t)elem_size +
                        16),
               sizeof(tag));
        if (tag == 0) {
          /*
           * A descriptor with an unknown tag can still contain a concrete
           * raw payload.  Native literal construction uses that state for
           * scalar and nested-list elements; only a zero payload is the
           * canonical nil value.
           */
          if (value == 0)
            return 0;
          if (rt_magic_tbuf_elem_size(value) > 0 || rt_native_is_str(value) ||
              is_v_str(value) || rt_heap_object_ptr(value))
            return value;
          return ny_native_box_tbuf_any(value);
        }
        if (tag == TAG_FLOAT)
          return is_v_flt(value) ? value : rt_flt_box_val(value);
        if (tag == TAG_STR || tag == TAG_STR_CONST)
          return rt_cstr_to_str(value);
        if ((tag == 1 || tag == 3) && want_dynamic)
          return rt_tag_v(value);
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
  buffer = rt_tbuf_resolve_handle(buffer);
  /*
   * Native values crossing an `any` boundary may be compact encoded
   * pointers (tag 6), while the raw tbuf helpers expect the decoded address.
   * Decode only after validating the private header so ordinary tagged values
   * retain their normal semantics.
   */
  if (NY_NATIVE_IS(buffer) && NY_NATIVE_DECODE(buffer) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(buffer);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
      buffer = decoded;
  }
  if (!buffer)
    return ny_native_box_tbuf_any(fallback);
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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
        if (tag == 0) {
          if (value == 0)
            return 0;
          if (rt_magic_tbuf_elem_size(value) > 0 || rt_native_is_str(value) ||
              is_v_str(value) || rt_heap_object_ptr(value))
            return value;
          return ny_native_box_tbuf_any(value);
        }
        if (tag == TAG_FLOAT) {
          int64_t out = is_v_flt(value) ? value : rt_flt_box_val(value);
          return out;
        }
        if (tag == NY_TBUF_TAG_BOOL)
          return value;
        if (tag == 121)
          return rt_cstr_to_str(value);
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
    return rt_tag_v(rt_range_index_read_raw(buffer, index));
  /*
   * Unknown `any` receivers can still be dictionaries, strings, ranges, or
   * managed sequences. Delegate those cases to the generic accessor.
   */
  int64_t result = rt_value_get_tagged(buffer, index, fallback);
  /*
   * Unknown receivers are a miss, but the public any accessor still returns
   * the fallback in dynamic representation.  Normalize only the miss so
   * valid pointer/string/container results are left untouched.
   */
  return result == fallback ? ny_native_box_tbuf_any(result) : result;
}

int64_t rt_range_index_read_raw(int64_t range, int64_t index);

int64_t rt_tbuf_index_read_raw(int64_t buffer, int64_t index) {
  /*
   * Typed integer indexing dominates hot loops.  A current cache hit proves
   * this is a canonical registered payload, so a checked i64 header can avoid
   * resolving the handle and probing the registry on every element read.
   */
  uintptr_t data_addr = (uintptr_t)buffer;
  if (data_addr > RT_NATIVE_TBUF_HEADER &&
      rt_tbuf_cached_handle_fast(data_addr)) {
    int64_t *hdr = (int64_t *)(data_addr - RT_NATIVE_TBUF_HEADER);
    bool header_readable = true;
#if NY_WITH_ASAN
    header_readable = !__asan_region_is_poisoned(hdr, RT_NATIVE_TBUF_HEADER);
#endif
    if (header_readable && (uint64_t)hdr[0] == NY_NATIVE_TBUF_MAGIC &&
        hdr[1] >= 0 && hdr[2] == 8 && rt_tbuf_capacity_word(hdr[3]) >= hdr[1]) {
      int64_t count = hdr[1];
      if (index < 0 && index >= -count)
        index += count;
      if (index >= 0 && index < count)
        return ((const int64_t *)data_addr)[index];
      rt_fail(rt_alloc_string("err.index"),
              rt_alloc_string("index_read out of range"));
      return 0;
    }
  }
  /*
   * Resolve forwarding and native transport handles before probing payloads.
   */
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  if (NY_NATIVE_IS(buffer) && NY_NATIVE_DECODE(buffer) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(buffer);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
      buffer = decoded;
  }
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index >= 0 && index < count) {
        if (elem_size == 8) {
          return ((const int64_t *)(uintptr_t)buffer)[index];
        }
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
      rt_fail(rt_alloc_string("err.index"),
              rt_alloc_string("index_read out of range"));
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
      rt_fail(rt_alloc_string("err.index"),
              rt_alloc_string("index_read out of range"));
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
    return rt_tag_v(rt_range_index_read_raw(buffer, index));
  }
  rt_fail(rt_alloc_string("err.index"),
          rt_alloc_string("index_read out of range"));
  return 0;
}

/*
 * Read a homogeneous f64 sequence without passing its IEEE payload through
 * the tagged-value ABI.  Eight-byte typed buffers store the bits directly;
 * descriptor-backed dynamic buffers use the ordinary boxed representation.
 * Keeping this distinction in the runtime makes every compiler frontend and
 * backend agree on the same sequence contract.
 */
double rt_tbuf_index_f64(int64_t buffer, int64_t index) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (rt_tbuf_known_handle((uintptr_t)buffer)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index < 0 || index >= count)
        return 0.0;
      if (elem_size == 8) {
        int64_t bits = 0;
        memcpy(&bits,
               (const unsigned char *)(uintptr_t)buffer + (size_t)index * 8,
               sizeof(bits));
        double out = 0.0;
        memcpy(&out, &bits, sizeof(out));
        return out;
      }
      return rt_any_to_f64(rt_tbuf_index_read_raw(buffer, index));
    }
  }
  return rt_any_to_f64(rt_tbuf_get(buffer, index, 0));
}

/*
 * Dynamic callback/index boundary. Typed native reads return raw payloads;
 * this companion preserves the strict index error contract while using the
 * tbuf element width/tag metadata to produce a canonical any value.
 */
static int64_t rt_tbuf_index_any_impl(int64_t buffer, int64_t index) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  if (rt_tbuf_known_handle((uintptr_t)buffer) &&
      rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1], elem_size = hdr[2];
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index < 0 || index >= count || elem_size < 8) {
        rt_fail(rt_alloc_string("err.index"),
                rt_alloc_string("index_read out of range"));
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
        if (tag == 0) {
          if (value == 0)
            return 0;
          if (rt_magic_tbuf_elem_size(value) > 0 || rt_native_is_str(value) ||
              is_v_str(value) || rt_heap_object_ptr(value))
            return value;
          return ny_native_box_tbuf_any(value);
        }
        if (tag == TAG_FLOAT)
          return is_v_flt(value) ? value : rt_flt_box_val(value);
        if (tag == TAG_STR || tag == TAG_STR_CONST)
          return rt_cstr_to_str(value);
        if (tag == NY_TBUF_TAG_BOOL)
          return value;
        if (tag == 1 || tag == 3)
          return rt_tag_v(value);
        return value;
      }
      /*
       * Element-8 buffers contain raw machine words.  Do not use the
       * value-tag classifier here: odd raw integers intentionally satisfy
       * the tagged-integer bit test and can otherwise be mistaken for a
       * managed value.  Only values with independently verifiable pointer
       * provenance may cross unchanged; scalar words are boxed exactly once.
       * Classify the word in a single pass: the historical tail ran the
       * full string/float/heap gauntlet once inline and then again inside
       * ny_native_box_tbuf_any, doubling the probe cost of every read.
       */
      if (rt_word_is_plain_scalar(value))
        return rt_tag_v(value);
      if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
        int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
        if (rt_magic_tbuf_elem_size(decoded) > 0)
          return decoded;
      }
      if (rt_magic_tbuf_elem_size(value) > 0)
        return value;
      if (is_v_flt(value))
        return value;
      if (is_v_str(value))
        return value;
      if (rt_native_is_str(value))
        return rt_cstr_to_str(value);
      if (rt_heap_object_ptr(value))
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
    return rt_cstr_to_str(rt_cstr_index_read_raw(buffer, index));
  if (rt_sequence_tag(buffer) == TAG_BYTES)
    return rt_tag_v(rt_bytes_index_read_raw(buffer, index));
  if (is_ptr(buffer) && is_heap_ptr(buffer)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)buffer - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      /*
       * Managed lists/tuples do not have the native-tbuf header, so the
       * historical fast load below cannot validate the subscript.  Enforce
       * the same strict checked-index contract as native tbufs before touching
       * the payload; otherwise an out-of-range read silently observes adjacent
       * heap memory (often zero) instead of raising a catchable panic.
       */
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)buffer + 0);
      int64_t count = is_int(len_v) ? rt_untag_v(len_v) : len_v;
      if (index < 0 && count >= 0 && index >= -count)
        index += count;
      if (index < 0 || index >= count) {
        rt_fail(rt_alloc_string("err.index"),
                rt_alloc_string("index_read out of range"));
        return 0;
      }
      /*
       * Managed sequence slots use the raw legacy payload layout. Convert
       * through the single any decoder so raw integers are tagged exactly
       * once, while pointers/bools/nil remain unchanged.
       */
      return ny_native_box_tbuf_any(rt_load_item_fast(buffer, index));
    }
    if (tag == TAG_RANGE)
      return rt_tag_v(rt_range_index_read_raw(buffer, index));
  }
  rt_fail(rt_alloc_string("err.index"),
          rt_alloc_string("index_read out of range"));
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
  rt_fail(rt_alloc_string("err.type"),
          rt_alloc_string("index_read expects an integer index"));
  return 0;
}
int64_t rt_tbuf_tag(int64_t buffer, int64_t index) {
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer || index < 0)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(hdr))
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
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  if (rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t count = hdr[1];
      for (int64_t i = 0; i < count; ++i) {
        int64_t v = rt_tbuf_get(buffer, i, 0);
        bool item_is_string =
            is_string || is_v_str(item) || rt_native_is_str(item);
        bool value_is_string = is_v_str(v) || rt_native_is_str(v);
        if (item_is_string && value_is_string) {
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
  buffer = rt_tbuf_resolve_handle(buffer);
  if (!buffer)
    return 0;
  if (!rt_header_readable_cached((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER,
                                 RT_NATIVE_TBUF_HEADER))
    return 0;
  int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
  if (!rt_tbuf_header_valid(hdr))
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

/*
 * `pop(any)` must preserve the dynamic element representation.  The raw pop
 * entry point is for typed callers and cannot distinguish an integer payload
 * from a tagged value; read through the dynamic accessor before removing the
 * slot so descriptor tags and compact scalar boxing follow the same rules as
 * indexed reads.
 */
int64_t rt_tbuf_pop_any(int64_t buffer) {
  buffer = rt_tbuf_resolve_handle(buffer);
  int64_t count = rt_tbuf_len_raw(buffer);
  if (count <= 0)
    return 0;
  int64_t value = rt_tbuf_get_any(buffer, count - 1, 0);
  (void)rt_tbuf_pop_raw(buffer);
  return value;
}

int64_t rt_tbuf_to_cstr(int64_t buffer) {
  buffer = rt_tbuf_resolve_handle(buffer);
  int64_t count = rt_tbuf_len_raw(buffer);
  if (count < 0 || count > (INT64_MAX - 3) / 4)
    return 0;
  /*
   * Descriptor lists (elem_size 24) carry a per-slot runtime tag at +16.
   * Descriptor payloads are raw words. Tag 1 records a dynamic integer
   * origin and tag 3 a typed/raw integer; neither changes the number stored.
   */
  int64_t elem_size = 8;
  int is_tuple = 0;
  {
    int64_t *hdr = (int64_t *)((uintptr_t)buffer - RT_NATIVE_TBUF_HEADER);
    if (buffer && rt_tbuf_header_valid(hdr)) {
      elem_size = hdr[2];
      is_tuple = rt_tbuf_word_is_tuple(hdr[3]);
    }
  }
  /*
   * Worst-case item width is a float (%g renders up to ~24 bytes) plus
   * separator/quotes; the old count*4+3 estimate bailed to "" (empty
   * string) on ordinary float lists once a 19-digit fallback or a %g
   * rendering exceeded it.  Size for floats up front; the per-item
   * overflow checks below stay as a backstop.
   */
  size_t cap = (size_t)count * 32 + 3;
  char *out = (char *)malloc(cap);
  if (!out)
    return 0;
  size_t used = 0;
  out[used++] = is_tuple ? '(' : '[';
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
      else if (slot_tag == TAG_FLOAT)
        value = rt_flt_box_val(value);
    }
    char item[64];
    int n;
    if (is_string_slot || rt_native_is_str(value) || is_v_str(value)) {
      const char *s = (const char *)(uintptr_t)rt_any_to_cstr(value);
      size_t len = s ? strnlen(s, 4096) : 0;
      if (used + len + 3 >= cap) {
        size_t next = cap;
        while (used + len + 3 >= next)
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
    } else if (is_v_flt(value)) {
      /*
       * Float handles print with %g regardless of the slot flavor that
       * carried them: descriptor lists (elem_size >= 24) box TAG_FLOAT
       * payloads above, and 8-byte scalar buffers may also surface
       * handles.  The old elem_size < 24 gate pushed descriptor floats
       * into the %lld branch, which then bailed to "" on overflow.
       */
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
  out[used++] = is_tuple ? ')' : ']';
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
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  int64_t tag = *(int64_t *)((uintptr_t)buffer - 8);
  int64_t length = *(int64_t *)((uintptr_t)buffer - 16);
  length = is_int(length) ? length >> 1 : length;
  if (index < 0 && length >= 0 && index >= -length)
    index += length;
  if (tag != TAG_BYTES || length < 0 || index < 0 || index >= length) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  return *(const uint8_t *)((uintptr_t)buffer + (uintptr_t)index);
}

int64_t rt_range_index_read_raw(int64_t range, int64_t index) {
  int64_t heap =
      rt_sequence_tag(range) == TAG_RANGE ? range : rt_heap_object_ptr(range);
  if (!heap || *(int64_t *)((char *)(uintptr_t)heap - 8) != TAG_RANGE) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  int64_t start = *(int64_t *)(uintptr_t)heap;
  int64_t step = *(int64_t *)((char *)(uintptr_t)heap + 16);
  int64_t count = rt_range_len_raw(range);
  if (index < 0 && count >= 0 && index >= -count)
    index += count;
  if (index < 0 || index >= count) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return 0;
  }
  return start + index * step;
}

int64_t rt_range_len_raw(int64_t range) {
  int64_t heap =
      rt_sequence_tag(range) == TAG_RANGE ? range : rt_heap_object_ptr(range);
  if (!heap || *(int64_t *)((char *)(uintptr_t)heap - 8) != TAG_RANGE)
    return 0;
  int64_t start = *(int64_t *)(uintptr_t)heap;
  int64_t stop = *(int64_t *)((char *)(uintptr_t)heap + 8);
  int64_t step = *(int64_t *)((char *)(uintptr_t)heap + 16);
  if (step > 0) {
    if (start >= stop)
      return 0;
    return ((stop - start - 1) / step) + 1;
  }
  if (step < 0) {
    if (start <= stop)
      return 0;
    return ((start - stop - 1) / -step) + 1;
  }
  return 0;
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
  /*
   * Raw pointers to LLVM's static C-string literals are readable too, and
   * their preceding bytes can accidentally look like a Ny string header.
   * Only trust the tagged-string predicate after allocator/SSO provenance has
   * established that this word is a runtime string value.
   */
  if ((rt_heap_object_ptr(value) && is_v_str(value)) || rt_is_sso_string(value))
    return (int64_t)rt_tagged_str_len(value);
  uintptr_t raw = (uintptr_t)value;
  if (NY_NATIVE_IS(value) && !rt_addr_readable_safe(raw, 1)) {
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(value);
    if (!rt_addr_readable_safe(decoded, 1))
      return 0;
    raw = decoded;
  }
  return (int64_t)strlen((const char *)raw);
}

int64_t rt_bool_to_cstr(int64_t value) {
  /*
   * Native predicates use raw 0/1, while runtime/container predicates
   * return Ny's canonical boolean immediates (2=false, 8=true).  Accept both
   * representations at this formatting boundary instead of treating the
   * false immediate as truthy.
   */
  bool truth = value == 1 || value == NY_IMM_TRUE;
  return (int64_t)(uintptr_t)(truth ? "true" : "false");
}

typedef struct {
  int64_t key;
  int64_t value;
  uint32_t hash;
  /*
   * 0=int, 1=string, 2=nil. Nil is distinct from the integer key 0.
   */
  uint8_t key_is_string;
} ny_native_dict_slot_t;

/*
 * SwissTable-style control bytes: 0 is empty, 1 is a tombstone, and occupied
 * slots carry a 7-bit H2 fingerprint with the high bit set. Keeping H2 in
 * the slot's existing padding avoids a second metadata allocation.
 */
#define NY_NATIVE_DICT_EMPTY 0u
#define NY_NATIVE_DICT_TOMBSTONE 1u
#define NY_NATIVE_DICT_H2(hash) ((uint8_t)(0x80u | (((hash) >> 25) & 0x7fu)))

typedef struct ny_native_dict_t {
  uint64_t magic;
  int64_t tag;
  int64_t length;
  int64_t capacity;
  int64_t resize_at;
  ny_native_dict_slot_t *slots;
  uint8_t *ctrl;
  int64_t vector_dim;
} ny_native_dict_t;

static inline void ny_native_dict_set_ctrl(ny_native_dict_t *dict, size_t idx,
                                           uint8_t val) {
  dict->ctrl[idx] = val;
  if (idx < 16) {
    dict->ctrl[(size_t)dict->capacity + idx] = val;
    if (dict->capacity < 16 &&
        (size_t)dict->capacity * 2 + idx < (size_t)dict->capacity + 16) {
      dict->ctrl[(size_t)dict->capacity * 2 + idx] = val;
    }
  }
}

#define NY_NATIVE_DICT_MAGIC UINT64_C(0x4e59444943544d47)

/*
 * Runtime-created native dictionaries remain allocated for the process
 * lifetime. Remember their public payload handles so common typed dict
 * operations can validate provenance without a `mincore` probe on every
 * lookup/set. Unknown words still take the full checked-pointer path below.
 */
#define NY_NATIVE_DICT_REGISTRY_BITS 16u
#define NY_NATIVE_DICT_REGISTRY_SIZE (1u << NY_NATIVE_DICT_REGISTRY_BITS)
#define NY_NATIVE_DICT_REGISTRY_MASK (NY_NATIVE_DICT_REGISTRY_SIZE - 1u)
static _Atomic(uintptr_t) ny_native_dict_registry[NY_NATIVE_DICT_REGISTRY_SIZE];
static _Thread_local uintptr_t ny_native_dict_last_address;
static _Thread_local ny_native_dict_t *ny_native_dict_last;

static inline size_t ny_native_dict_registry_slot(uintptr_t value) {
  uint64_t x = (uint64_t)value;
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  x ^= x >> 31;
  return (size_t)x & NY_NATIVE_DICT_REGISTRY_MASK;
}

static ny_native_dict_t *ny_native_dict_registry_find(uintptr_t value) {
  if (!value)
    return NULL;
  size_t start = ny_native_dict_registry_slot(value);
  for (size_t probe = 0; probe < NY_NATIVE_DICT_REGISTRY_SIZE; ++probe) {
    uintptr_t seen = atomic_load_explicit(
        &ny_native_dict_registry[(start + probe) &
                                 NY_NATIVE_DICT_REGISTRY_MASK],
        memory_order_acquire);
    if (seen == value)
      return (ny_native_dict_t *)(value - 16);
    if (!seen)
      return NULL;
  }
  return NULL;
}

static void ny_native_dict_registry_add(uintptr_t value) {
  if (!value)
    return;
  size_t start = ny_native_dict_registry_slot(value);
  for (size_t probe = 0; probe < NY_NATIVE_DICT_REGISTRY_SIZE; ++probe) {
    _Atomic(uintptr_t) *slot =
        &ny_native_dict_registry[(start + probe) &
                                 NY_NATIVE_DICT_REGISTRY_MASK];
    uintptr_t seen = atomic_load_explicit(slot, memory_order_acquire);
    if (seen == value)
      return;
    if (seen)
      continue;
    uintptr_t empty = 0;
    if (atomic_compare_exchange_strong_explicit(
            slot, &empty, value, memory_order_acq_rel, memory_order_acquire))
      return;
    if (empty == value)
      return;
  }
}

static bool rt_set_layout(int64_t value) {
  uintptr_t p = (uintptr_t)value;
  if (p < 4096 || p > UINTPTR_MAX - 16 || !is_heap_ptr(value))
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
  /*
   * Native dictionaries are process-lifetime objects. Repeated typed stores
   * to one receiver can therefore bypass the registry hash and atomic probe
   * after the first validated lookup on each thread.
   */
  uintptr_t address = (uintptr_t)value;
  if (address < 4096 || address >= UINT64_C(0x0000800000000000) || address < 16)
    return NULL;
  if (address == ny_native_dict_last_address && ny_native_dict_last)
    return ny_native_dict_last;
  ny_native_dict_t *dict = ny_native_dict_registry_find(address);
  /*
   * Registry entries are installed only by rt_dict_new_raw, and native
   * dictionary objects have process lifetime.  A hit therefore proves both
   * the address and the private header provenance; avoid repeating the magic,
   * capacity, and length checks on every typed get/set in a hot loop.  Values
   * from any other source still take the fully checked compatibility path.
   */
  if (dict) {
    ny_native_dict_last_address = 0;
    ny_native_dict_last = dict;
    ny_native_dict_last_address = address;
    return dict;
  }
  if (!rt_header_readable_cached(address - 16, sizeof(ny_native_dict_t)))
    return NULL;
  dict = (ny_native_dict_t *)(uintptr_t)(address - 16);
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

/*
 * Keep the repeat-store cache check in the hot setter's caller. A miss still
 * takes the complete provenance-checked lookup above.
 */
static inline ny_native_dict_t *ny_native_dict_cached_ptr(int64_t value) {
  uintptr_t address = (uintptr_t)value;
  if (address == ny_native_dict_last_address && ny_native_dict_last)
    return ny_native_dict_last;
  return ny_native_dict_ptr(value);
}

int64_t rt_handle_valid(int64_t handle) {
  if (!handle)
    return 0;
  if (rt_tbuf_known_handle((uintptr_t)handle))
    return 1;
  if (ny_native_dict_ptr(handle) != NULL)
    return 1;
  if (is_ptr(handle) && is_heap_ptr(handle)) {
    if (rt_header_readable_cached((uintptr_t)handle - 8, 8))
      return 1;
  }
  return 0;
}

static uint32_t ny_native_dict_hash(int64_t key, uint8_t key_kind) {
  if (key_kind == 2)
    return (uint32_t)UINT64_C(0x9e3779b97f4a7c15);
  if (key_kind == 1 && key != 0) {
    const unsigned char *s = (const unsigned char *)(uintptr_t)key;
    /*
     * String keys are commonly short (identifiers and decimalized indices).
     * The 33x recurrence compiles to a shift/add sequence on common targets.
     */
    uint64_t h = UINT64_C(5381);
    for (size_t i = 0; i < 4096 && s[i]; ++i)
      h = h * UINT64_C(33) + s[i];
    return (uint32_t)h;
  }
  /*
   * Fibonacci hashing gives power-of-two tables a permutation of the low
   * key bits while spreading sequential keys across the table.  Taking the
   * high product word also distributes those keys' H2 fingerprints.  This
   * replaces the two-multiply avalanche path in the typed integer setter;
   * key equality still uses the original full-width value.
   */
  uint64_t mixed = (uint64_t)key * UINT64_C(11400714819323198485);
  return (uint32_t)(mixed >> 32);
}

static bool ny_native_dict_resize(ny_native_dict_t *dict, int64_t capacity) {
  if (!dict || capacity < 8 || (capacity & (capacity - 1)) != 0)
    return false;
  size_t slot_bytes = (size_t)capacity * sizeof(ny_native_dict_slot_t);
  size_t ctrl_bytes = (size_t)capacity + 16;
  char *buf = (char *)calloc(1, slot_bytes + ctrl_bytes);
  if (!buf)
    return false;
  ny_native_dict_slot_t *slots = (ny_native_dict_slot_t *)buf;
  uint8_t *ctrl = (uint8_t *)(buf + slot_bytes);
  ny_native_dict_slot_t *old_slots = dict->slots;
  uint8_t *old_ctrl = dict->ctrl;
  int64_t old_cap = dict->capacity;
  int64_t old_length = dict->length;
  dict->slots = slots;
  dict->ctrl = ctrl;
  dict->capacity = capacity;
  /*
   * Keep the existing integer load-factor rule exact while avoiding its
   * multiply/compare sequence on every insertion.
   */
  dict->resize_at = (capacity * 7 + 9) / 10 - 1;
  dict->length = 0;
  if (old_slots) {
    uint64_t mask = (uint64_t)capacity - 1;
    for (int64_t i = 0; i < old_cap; ++i) {
      uint8_t old_control = old_ctrl[i];
      if (old_control < 0x80u)
        continue;
      uint64_t pos = old_slots[i].hash & mask;
      while (ctrl[pos] >= 0x80u)
        pos = (pos + 1) & mask;
      slots[pos] = old_slots[i];
      ctrl[pos] = old_control;
    }
    /*
     * The SIMD probe reads up to 16 bytes from the starting control index.
     * Mirror the prefix once instead of branching on every rehashed slot.
     */
    size_t mirror_len = capacity < 16 ? (size_t)capacity : 16;
    for (size_t i = 0; i < mirror_len; ++i)
      ctrl[(size_t)capacity + i] = ctrl[i];
    /*
     * Rehash preserves the number of live entries. Avoid a memory increment
     * for every copied slot; the old length already accounts for tombstones.
     */
    dict->length = old_length;
    free(old_slots);
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
  uintptr_t payload = (uintptr_t)((char *)dict + 16);
  ny_native_dict_registry_add(payload);
  return (int64_t)payload;
}

int64_t rt_vec_mark(int64_t value, int64_t dimension) {
  int64_t handle =
      NY_NATIVE_IS(value) ? (int64_t)(uintptr_t)NY_NATIVE_DECODE(value) : value;
  ny_native_dict_t *dict = ny_native_dict_ptr(handle);
  if (!dict) {
    handle = rt_untag_v(value);
    dict = ny_native_dict_ptr(handle);
  }
  int64_t dim = rt_any_to_i64(dimension);
  if (dict && dim >= 2 && dim <= 4)
    dict->vector_dim = dim;
  return rt_tag_v(0);
}

int64_t rt_vec_mark_raw(int64_t value, int64_t dimension) {
  int64_t handle =
      NY_NATIVE_IS(value) ? (int64_t)(uintptr_t)NY_NATIVE_DECODE(value) : value;
  ny_native_dict_t *dict = ny_native_dict_ptr(handle);
  if (!dict) {
    handle = rt_untag_v(value);
    dict = ny_native_dict_ptr(handle);
  }
  if (dict && dimension >= 2 && dimension <= 4)
    dict->vector_dim = dimension;
  return 0;
}

/*
 * Native set layout matches core.set: the returned payload points at
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
  int64_t body = 16 + actual * 24;
  int64_t payload = rt_malloc(rt_tag_v(body));
  if (!payload)
    return 0;
  memset((void *)(uintptr_t)payload, 0, (size_t)body);
  ((int64_t *)(uintptr_t)payload)[-1] = TAG_SET;
  ((int64_t *)(uintptr_t)payload)[0] = 0;
  ((int64_t *)(uintptr_t)payload)[1] = actual;
  return payload;
}

/*
 * Construct a set from any sequence representation understood by the native
 * runtime. Native tbufs and legacy managed list/tuple values both enter this
 * boundary as one sequence handle; the runtime normalizes their element ABI
 * before inserting each key into the canonical set layout.
 */
int64_t rt_set_from_sequence(int64_t sequence) {
  if (rt_set_layout(sequence))
    return sequence;
  int64_t count = rt_tbuf_len_raw(sequence);
  if (count < 0)
    return 0;
  int64_t capacity = count > (INT64_MAX - 8) / 2 ? 8 : count * 2 + 8;
  int64_t set = rt_set_new(capacity);
  if (!set)
    return 0;
  int64_t elem_size = rt_magic_tbuf_elem_size(sequence);
  for (int64_t i = 0; i < count; ++i) {
    /*
     * Compact scalar buffers already store raw machine integers.  Reading
     * them through the dynamic accessor makes odd values look like tagged
     * integers (1 becomes 0, 3 becomes 1), collapsing distinct set keys.
     * Descriptor and legacy sequences do carry dynamic values and therefore
     * still require the normal unboxing boundary.
     */
    int64_t value = elem_size == 8 ? rt_tbuf_get(sequence, i, 0)
                                   : rt_value_get_index_raw(sequence, i, 0);
    if (elem_size != 8)
      value = rt_any_to_i64(value);
    set = rt_set_add_in_place(set, value);
    if (!set)
      return 0;
  }
  return set;
}

int64_t rt_set_remove(int64_t value, int64_t key) {
  if (!value || (uint64_t)value < 4096 ||
      !rt_header_readable_cached((uintptr_t)value - 8, 8) ||
      *(int64_t *)((char *)(uintptr_t)value - 8) != TAG_SET)
    return value;
  int64_t count = *(int64_t *)((char *)(uintptr_t)value + 0);
  int64_t cap = *(int64_t *)((char *)(uintptr_t)value + 8);
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

/*
 * Native NYIR carries managed set handles as raw pointers or tagged generic
 * values depending on their source. Normalize both at this native boundary,
 * then return the canonical set pointer for subsequent typed operations.
 */
int64_t rt_set_remove_raw(int64_t value, int64_t key) {
  int64_t heap = rt_heap_object_ptr(value);
  return heap ? rt_set_remove(heap, key) : 0;
}

static bool ny_native_dict_key_equal(int64_t a, uint8_t a_kind, int64_t b,
                                     uint8_t b_kind) {
  if (a_kind != b_kind)
    return false;
  if (a == b)
    return true;
  if (!a || !b)
    return false;
  return a_kind == 1 &&
         strcmp((const char *)(uintptr_t)a, (const char *)(uintptr_t)b) == 0;
}

static ny_native_dict_slot_t *
ny_native_dict_find(ny_native_dict_t *dict, int64_t key, uint8_t key_is_string,
                    bool *found, uint64_t *hash_out) {
  if (found)
    *found = false;
  if (!dict || !dict->slots || !dict->ctrl || dict->capacity <= 0)
    return NULL;
  uint64_t mask = (uint64_t)dict->capacity - 1;
  uint64_t hash = ny_native_dict_hash(key, key_is_string);
  if (hash_out)
    *hash_out = hash;
  uint8_t h2 = NY_NATIVE_DICT_H2(hash);
  uint64_t pos = hash & mask;
  ny_native_dict_slot_t *first_tomb = NULL;

#if defined(NY_DICT_USE_SSE2)
  __m128i target = _mm_set1_epi8((char)h2);
  __m128i empty_vec = _mm_setzero_si128();
  __m128i tomb_vec = _mm_set1_epi8((char)NY_NATIVE_DICT_TOMBSTONE);

  for (int64_t step = 0; step < dict->capacity; step += 16) {
    __m128i ctrl16 = _mm_loadu_si128((const __m128i *)(dict->ctrl + pos));

    __m128i match_vec = _mm_cmpeq_epi8(ctrl16, target);
    uint32_t match_mask = (uint32_t)_mm_movemask_epi8(match_vec);
    while (match_mask != 0) {
      int offset = __builtin_ctz(match_mask);
      uint64_t slot_idx = (pos + (uint64_t)offset) & mask;
      ny_native_dict_slot_t *slot = &dict->slots[slot_idx];
      if (slot->hash == hash &&
          ny_native_dict_key_equal(slot->key, slot->key_is_string, key,
                                   key_is_string)) {
        if (found)
          *found = true;
        return slot;
      }
      match_mask &= match_mask - 1;
    }

    __m128i empty_match = _mm_cmpeq_epi8(ctrl16, empty_vec);
    uint32_t empty_mask = (uint32_t)_mm_movemask_epi8(empty_match);
    if (empty_mask != 0) {
      if (first_tomb)
        return first_tomb;
      int offset = __builtin_ctz(empty_mask);
      uint64_t slot_idx = (pos + (uint64_t)offset) & mask;
      return &dict->slots[slot_idx];
    }

    if (!first_tomb) {
      __m128i tomb_match = _mm_cmpeq_epi8(ctrl16, tomb_vec);
      uint32_t tomb_mask = (uint32_t)_mm_movemask_epi8(tomb_match);
      if (tomb_mask != 0) {
        int offset = __builtin_ctz(tomb_mask);
        first_tomb = &dict->slots[(pos + (uint64_t)offset) & mask];
      }
    }

    pos = (pos + 16) & mask;
  }
#else
  uint64_t rep_h2 = 0x0101010101010101ULL * (uint64_t)h2;
  uint64_t rep_tomb =
      0x0101010101010101ULL * (uint64_t)NY_NATIVE_DICT_TOMBSTONE;

  for (int64_t step = 0; step < dict->capacity; step += 8) {
    uint64_t ctrl8 = *(const uint64_t *)(dict->ctrl + pos);

    uint64_t x_match = ctrl8 ^ rep_h2;
    uint64_t m_match =
        (x_match - 0x0101010101010101ULL) & ~x_match & 0x8080808080808080ULL;
    while (m_match != 0) {
      int bit = __builtin_ctzll(m_match);
      int offset = bit >> 3;
      uint64_t slot_idx = (pos + (uint64_t)offset) & mask;
      ny_native_dict_slot_t *slot = &dict->slots[slot_idx];
      if (slot->hash == hash &&
          ny_native_dict_key_equal(slot->key, slot->key_is_string, key,
                                   key_is_string)) {
        if (found)
          *found = true;
        return slot;
      }
      m_match &= ~(0x80ULL << bit);
    }

    uint64_t m_empty =
        (ctrl8 - 0x0101010101010101ULL) & ~ctrl8 & 0x8080808080808080ULL;
    if (m_empty != 0) {
      if (first_tomb)
        return first_tomb;
      int offset = __builtin_ctzll(m_empty) >> 3;
      return &dict->slots[(pos + (uint64_t)offset) & mask];
    }

    if (!first_tomb) {
      uint64_t x_tomb = ctrl8 ^ rep_tomb;
      uint64_t m_tomb =
          (x_tomb - 0x0101010101010101ULL) & ~x_tomb & 0x8080808080808080ULL;
      if (m_tomb != 0) {
        int offset = __builtin_ctzll(m_tomb) >> 3;
        first_tomb = &dict->slots[(pos + (uint64_t)offset) & mask];
      }
    }

    pos = (pos + 8) & mask;
  }
#endif

  return first_tomb;
}

static bool ny_native_dict_slice_key_equal(const unsigned char *stored,
                                           const unsigned char *slice,
                                           size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (stored[i] != slice[i])
      return false;
    if (stored[i] == 0)
      return true;
  }
  return stored[len] == 0;
}

static ny_native_dict_slot_t *
ny_native_dict_find_str_slice(ny_native_dict_t *dict,
                              const unsigned char *slice, size_t len,
                              uint32_t hash, bool *found) {
  if (found)
    *found = false;
  if (!dict || !dict->slots || !dict->ctrl || dict->capacity <= 0)
    return NULL;

  uint64_t mask = (uint64_t)dict->capacity - 1;
  uint8_t h2 = NY_NATIVE_DICT_H2(hash);
  uint64_t pos = hash & mask;
  ny_native_dict_slot_t *first_tomb = NULL;

  for (int64_t step = 0; step < dict->capacity; step += 8) {
    for (uint64_t offset = 0; offset < 8; ++offset) {
      uint64_t slot_idx = (pos + offset) & mask;
      uint8_t control = dict->ctrl[slot_idx];
      ny_native_dict_slot_t *slot = &dict->slots[slot_idx];
      if (control == h2 && slot->hash == hash && slot->key_is_string == 1 &&
          ny_native_dict_slice_key_equal(
              (const unsigned char *)(uintptr_t)slot->key, slice, len)) {
        if (found)
          *found = true;
        return slot;
      }
      if (control == 0)
        return first_tomb ? first_tomb : slot;
      if (control == NY_NATIVE_DICT_TOMBSTONE && !first_tomb)
        first_tomb = slot;
    }
    pos = (pos + 8) & mask;
  }
  return first_tomb;
}

/*
 * Integer-only SwissTable probe used by statically typed dictionary stores.
 */
static int64_t ny_native_dict_find_i64(ny_native_dict_t *dict, int64_t key,
                                       uint64_t hash, bool *found) {
  if (found)
    *found = false;
  if (!dict || !dict->slots || !dict->ctrl || dict->capacity <= 0)
    return -1;
  uint64_t mask = (uint64_t)dict->capacity - 1;
  uint8_t h2 = NY_NATIVE_DICT_H2(hash);
  uint64_t pos = hash & mask;
  int64_t first_tomb = -1;
#if defined(NY_DICT_USE_SSE2)
  __m128i target = _mm_set1_epi8((char)h2);
  __m128i empty_vec = _mm_setzero_si128();
  __m128i tomb_vec = _mm_set1_epi8((char)NY_NATIVE_DICT_TOMBSTONE);
  for (int64_t step = 0; step < dict->capacity; step += 16) {
    __m128i ctrl16 = _mm_loadu_si128((const __m128i *)(dict->ctrl + pos));
    uint32_t matches =
        (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl16, target));
    while (matches) {
      unsigned offset = (unsigned)__builtin_ctz(matches);
      size_t idx = (size_t)((pos + offset) & mask);
      ny_native_dict_slot_t *slot = &dict->slots[idx];
      if (slot->hash == hash && slot->key_is_string == 0 && slot->key == key) {
        if (found)
          *found = true;
        return (int64_t)idx;
      }
      matches &= matches - 1;
    }
    uint32_t empty =
        (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl16, empty_vec));
    if (empty) {
      if (first_tomb >= 0)
        return first_tomb;
      unsigned offset = (unsigned)__builtin_ctz(empty);
      return (int64_t)((pos + offset) & mask);
    }
    if (first_tomb < 0) {
      uint32_t tombs =
          (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(ctrl16, tomb_vec));
      if (tombs) {
        unsigned offset = (unsigned)__builtin_ctz(tombs);
        first_tomb = (int64_t)((pos + offset) & mask);
      }
    }
    pos = (pos + 16) & mask;
  }
#else
  for (int64_t step = 0; step < dict->capacity; ++step) {
    uint8_t control = dict->ctrl[pos];
    ny_native_dict_slot_t *slot = &dict->slots[pos];
    if (control == NY_NATIVE_DICT_EMPTY)
      return first_tomb >= 0 ? first_tomb : (int64_t)pos;
    if (control == NY_NATIVE_DICT_TOMBSTONE) {
      if (first_tomb < 0)
        first_tomb = (int64_t)pos;
    } else if (control == h2 && slot->hash == hash &&
               slot->key_is_string == 0 && slot->key == key) {
      if (found)
        *found = true;
      return (int64_t)pos;
    }
    pos = (pos + 1) & mask;
  }
#endif
  return first_tomb;
}

static int64_t ny_native_dict_set_i64_impl(ny_native_dict_t *dict,
                                           int64_t value, int64_t key,
                                           int64_t item) {
  if (!dict)
    return value;
  if (dict->length >= dict->resize_at &&
      !ny_native_dict_resize(dict, dict->capacity * 2))
    return value;
  uint64_t hash = ny_native_dict_hash(key, 0);
  uint64_t mask = (uint64_t)dict->capacity - 1;
  size_t pos = (size_t)(hash & mask);
  uint8_t h2 = NY_NATIVE_DICT_H2(hash);
  uint8_t control = dict->ctrl[pos];
  ny_native_dict_slot_t *slot = &dict->slots[pos];
  /*
   * Sequential integer-key tables commonly hit their first probe directly.
   * Avoid the grouped probe and its match/empty masks for that dominant
   * insertion case, while preserving the full search for collisions.
   */
  if (control == NY_NATIVE_DICT_EMPTY) {
    slot->key = key;
    slot->key_is_string = 0;
    slot->hash = hash;
    slot->value = item;
    ny_native_dict_set_ctrl(dict, pos, h2);
    dict->length++;
    return value;
  }
  if (control == h2 && slot->hash == hash && slot->key_is_string == 0 &&
      slot->key == key) {
    slot->value = item;
    return value;
  }
  bool found = false;
  int64_t slot_idx = ny_native_dict_find_i64(dict, key, hash, &found);
  if (slot_idx < 0)
    return value;
  slot = &dict->slots[slot_idx];
  if (!found) {
    slot->key = key;
    slot->key_is_string = 0;
    slot->hash = hash;
    ny_native_dict_set_ctrl(dict, (size_t)slot_idx, NY_NATIVE_DICT_H2(hash));
    dict->length++;
  }
  slot->value = item;
  return value;
}

static int64_t rt_dict_find_off_table(int64_t t, int64_t cap, int64_t key,
                                      bool tagged);

static bool ny_native_managed_dict_ptr(int64_t v) {
  int64_t heap = rt_heap_object_ptr(v);
  int64_t tag = 0;
  return heap && rt_try_read_i64((uintptr_t)heap - 8, &tag) && tag == TAG_DICT;
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

/*
 * Raw integer-key bridges use an unboxed i64 for compact native dictionaries.
 * Managed dictionaries use canonical Ny values for keys, including BigInt
 * boxing outside the immediate range.
 */
static int64_t ny_native_managed_i64_key(int64_t key) {
  return ny_value_box_i64(key);
}

static int64_t ny_native_managed_dict_get(int64_t d, int64_t key,
                                          int64_t fallback) {
  int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)d + 8);
  cap_raw = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
  int64_t t = *(int64_t *)((char *)(uintptr_t)d + 16);
  t = is_int(t) ? (t >> 1) : t;
  if (cap_raw <= 0 || (uint64_t)t < 4096)
    return fallback;
  int64_t off = rt_dict_find_off_table(
      t, cap_raw, key, is_int(*(int64_t *)((char *)(uintptr_t)d + 8)));
  if (off < 0 || *(int64_t *)((char *)(uintptr_t)t + off + 16) !=
                     (is_int(*(int64_t *)((char *)(uintptr_t)d + 8)) ? 3 : 1))
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
  int64_t off = rt_dict_find_off_table(
      t, cap_raw, key, is_int(*(int64_t *)((char *)(uintptr_t)d + 8)));
  return off >= 0 &&
         *(int64_t *)((char *)(uintptr_t)t + off + 16) ==
             (is_int(*(int64_t *)((char *)(uintptr_t)d + 8)) ? 3 : 1);
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
  int64_t off = rt_dict_find_off_table(
      t, cap_raw, key, is_int(*(int64_t *)((char *)(uintptr_t)d + 8)));
  if (off < 0)
    return d;
  int64_t state = *(int64_t *)((char *)(uintptr_t)t + off + 16);
  *(int64_t *)((char *)(uintptr_t)t + off) = key;
  *(int64_t *)((char *)(uintptr_t)t + off + 8) = item;
  bool tagged = is_int(*(int64_t *)((char *)(uintptr_t)d + 8));
  int64_t occupied = tagged ? rt_tag_v(1) : 1;
  *(int64_t *)((char *)(uintptr_t)t + off + 16) = occupied;
  if (state != occupied)
    *(int64_t *)((char *)(uintptr_t)d + 0) =
        tagged ? rt_tag_v(count + 1) : count + 1;
  return d;
}

typedef enum {
  NY_DICT_KEY_DYNAMIC_VALUE,
  NY_DICT_KEY_RAW_I64,
  NY_DICT_KEY_RAW_CSTR,
  NY_DICT_KEY_NIR_VALUE,
} ny_dict_key_rep_t;

/*
 * Normalize dictionary bridge keys once at the managed/native boundary.
 * Dynamic keys are already Ny values; raw integers need boxing only for a
 * managed dictionary; explicitly typed raw C strings need materializing only
 * for a managed dictionary; NIR keys preserve the raw-i64 fast path.
 */
static int64_t ny_native_dict_bridge_key(int64_t dict, int64_t key,
                                         ny_dict_key_rep_t rep,
                                         uint8_t *key_is_string) {
  bool managed = !ny_native_dict_ptr(dict) && ny_native_managed_dict_ptr(dict);
  uint8_t string_key = 0;
  switch (rep) {
  case NY_DICT_KEY_DYNAMIC_VALUE:
    string_key = key == NY_IMM_NIL
                     ? 2
                     : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
    break;
  case NY_DICT_KEY_RAW_I64:
    if (managed)
      key = ny_native_managed_i64_key(key);
    break;
  case NY_DICT_KEY_RAW_CSTR:
    string_key = key != 0 ? 1 : 0;
    if (string_key && is_v_str(key)) {
      /*
       * Native dictionary string slots store raw C-string pointers.  The
       * typed lowering path can feed a boxed Ny string to the raw-C-string
       * lookup bridge (for example the second `.set` in a chained dict
       * expression).  Normalize it to the string payload before hashing;
       * managed dictionaries continue to use the tagged key representation.
       */
      if (!managed) {
        int64_t payload = rt_heap_object_ptr(key);
        /*
         * Static string constants are readable C strings, but are not
         * allocator-owned objects and therefore have no managed payload.
         */
        if (payload)
          key = payload;
      }
    } else if (managed && string_key) {
      key = ny_native_managed_string_key(key);
    }
    break;
  case NY_DICT_KEY_NIR_VALUE:
    string_key = key == NY_IMM_NIL
                     ? 2
                     : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
    /*
     * NIR lowering already tags immediate integer keys so zero remains
     * distinct from nil. Only box an untagged raw integer at this boundary;
     * boxing a tagged key again makes set and get use different key values.
     */
    if (managed && string_key == 0 && !is_int(key))
      key = ny_native_managed_i64_key(key);
    break;
  }
  if (key_is_string)
    *key_is_string = string_key;
  return key;
}

typedef enum {
  NY_GET_IC_NONE = 0,
  NY_GET_IC_DICT,
  NY_GET_IC_TBUF,
  NY_GET_IC_LIST,
} ny_get_ic_kind_t;

typedef struct {
  uintptr_t caller;
  uint8_t kind;
  uint8_t key_is_string;
  uint16_t slot_idx;
  int64_t capacity;
  int64_t cached_key;
  uint64_t cached_hash;
} ny_get_ic_entry_t;

#define NY_GET_IC_SIZE 2048
static _Thread_local ny_get_ic_entry_t g_get_ic_table[NY_GET_IC_SIZE];

static int64_t ny_native_dict_get_impl(int64_t value, int64_t key,
                                       int64_t fallback,
                                       uint8_t key_is_string) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  size_t ic_idx = ((caller ^ (caller >> 4)) & (NY_GET_IC_SIZE - 1));
  ny_get_ic_entry_t *ic = &g_get_ic_table[ic_idx];

  /*
   * Monomorphic receiver layout cache fast path: bypass hashing and probing
   */
  if (ic->caller == caller && ic->kind == NY_GET_IC_DICT &&
      ic->key_is_string == (uint8_t)key_is_string) {
    if (value && (uint64_t)value >= 4096 && (((uintptr_t)value & 7) == 0)) {
      ny_native_dict_t *dict = (ny_native_dict_t *)(uintptr_t)(value - 16);
      if (dict->magic == NY_NATIVE_DICT_MAGIC &&
          dict->capacity == ic->capacity &&
          (size_t)ic->slot_idx < (size_t)dict->capacity) {
        ny_native_dict_slot_t *slot = &dict->slots[ic->slot_idx];
        if ((dict->ctrl[ic->slot_idx] & 0x80) &&
            ((slot->key_is_string == key_is_string && slot->key == key) ||
             (key_is_string == 1 && slot->key_is_string == 1 &&
              slot->hash == ic->cached_hash &&
              ny_native_dict_key_equal(slot->key, 1, key, 1)))) {
          if (slot->value != 0 && !NY_DYNAMIC_CALLABLE_IS(slot->value) &&
              !is_int(slot->value) && rt_native_is_int(slot->value) &&
              !rt_is_bool_imm(slot->value))
            return rt_tag_v(slot->value);
          return slot->value;
        }
      }
    }
  }

  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    if (key_is_string == 1)
      key = ny_native_managed_string_key(key);
    return (key || key_is_string == 2)
               ? ny_native_managed_dict_get(value, key, fallback)
               : fallback;
  }
  if (!dict && key_is_string == 0 && (uint64_t)value >= 4096) {
    /*
     * Typed-buffer fast path: the header and the addressed element must both
     * be readable before treating the word as a descriptor; elsewhere this
     * file routes such probes through the cached readability helpers.
     */
    if (!rt_header_readable_cached((uintptr_t)value - 24, 24))
      return fallback;
    uint64_t *hdr = (uint64_t *)((uintptr_t)value - 24);
    int64_t count = (int64_t)hdr[0];
    int64_t stride = (int64_t)hdr[1];
    if ((stride == 8 || stride == 24) && key >= 0 && key < count &&
        rt_addr_readable_safe((uintptr_t)value + (size_t)key * (size_t)stride,
                              (size_t)stride)) {
      if (stride == 8) {
        int64_t *elems = (int64_t *)(uintptr_t)value;
        return elems[key];
      } else {
        int64_t *elems = (int64_t *)(uintptr_t)value;
        return elems[key * 3];
      }
    }
  }
  bool found = false;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found, NULL);
  if (!found || !slot)
    return fallback;

  /*
   * Record stable layout/slot offset in monomorphic inline cache
   */
  if (dict && dict->capacity > 0) {
    ic->caller = caller;
    ic->kind = NY_GET_IC_DICT;
    ic->key_is_string = (uint8_t)key_is_string;
    ic->slot_idx = (uint16_t)(slot - dict->slots);
    ic->capacity = dict->capacity;
    ic->cached_key = key;
    ic->cached_hash = slot->hash;
  }

  /*
   * Native lowering keeps typed integer results raw in the slot, while the
   * public dictionary API returns dynamic values.  Box only scalar integers
   * here; pointers, strings, floats, and container handles already carry
   * their own representation.  Raw 0 is excluded because the dynamic ABI
   * reserves it for nil: a stored nil must never re-emerge as tagged int 0.
   */
  if (slot->value != 0 && !NY_DYNAMIC_CALLABLE_IS(slot->value) &&
      !is_int(slot->value) && rt_native_is_int(slot->value) &&
      !rt_is_bool_imm(slot->value))
    return rt_tag_v(slot->value);
  return slot->value;
}

static int64_t ny_native_dict_set_native_impl(ny_native_dict_t *dict,
                                              int64_t value, int64_t key,
                                              int64_t item,
                                              uint8_t key_is_string) {
  if (!dict)
    return value;
  if (dict->length >= dict->resize_at &&
      !ny_native_dict_resize(dict, dict->capacity * 2))
    return value;
  bool found = false;
  uint64_t hash = 0;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found, &hash);
  if (!slot)
    return value;
  if (!found) {
    slot->key = key;
    slot->key_is_string = key_is_string;
    slot->hash = hash;
    ny_native_dict_set_ctrl(dict, (size_t)(slot - dict->slots),
                            NY_NATIVE_DICT_H2(hash));
    dict->length++;
  }
  slot->value = item;
  return value;
}

static int64_t ny_native_dict_set_impl(int64_t value, int64_t key, int64_t item,
                                       uint8_t key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (dict)
    return ny_native_dict_set_native_impl(dict, value, key, item,
                                          key_is_string);
  if (ny_native_managed_dict_ptr(value)) {
    if (key_is_string == 1)
      key = ny_native_managed_string_key(key);
    if (key || key_is_string == 2)
      ny_native_managed_dict_set(value, key, item);
  }
  return value;
}

static bool ny_native_word_is_space(unsigned char c) {
  return c == 32 || (c >= 9 && c <= 13);
}

int64_t rt_dict_word_frequency_raw(int64_t value) {
  int64_t result = rt_dict_new_raw(8);
  if (!result || !rt_is_str(value))
    return result;

  ny_native_dict_t *dict = ny_native_dict_ptr(result);
  const unsigned char *text = (const unsigned char *)(uintptr_t)value;
  int64_t byte_len = rt_cstr_len(value);
  if (!dict || byte_len <= 0)
    return result;
  size_t len = (size_t)byte_len;
  size_t start = 0;
  size_t end = len;
  while (start < end && ny_native_word_is_space(text[start]))
    ++start;
  while (end > start && ny_native_word_is_space(text[end - 1]))
    --end;

  size_t scan = start;
  while (scan < end) {
    while (scan < end && text[scan] == 32)
      ++scan;
    if (scan == end)
      break;

    size_t token_start = scan;
    while (scan < end && text[scan] != 32)
      ++scan;
    size_t token_end = scan;
    while (token_start < token_end &&
           ny_native_word_is_space(text[token_start]))
      ++token_start;
    while (token_end > token_start &&
           ny_native_word_is_space(text[token_end - 1]))
      --token_end;
    if (token_start == token_end)
      continue;

    const unsigned char *token = text + token_start;
    size_t token_len = token_end - token_start;
    uint32_t hash = 5381;
    for (size_t i = 0; i < token_len && i < 4096 && token[i]; ++i)
      hash = hash * 33u + token[i];

    if (dict->length >= dict->resize_at &&
        !ny_native_dict_resize(dict, dict->capacity * 2))
      return 0;
    bool found = false;
    ny_native_dict_slot_t *slot =
        ny_native_dict_find_str_slice(dict, token, token_len, hash, &found);
    if (!slot)
      return 0;
    if (found) {
      slot->value = rt_tag_v(rt_untag_v(slot->value) + 1);
      continue;
    }
    if (token_len >= (size_t)INT64_MAX)
      return 0;
    int64_t key = rt_malloc_i64((int64_t)token_len + 1);
    if (!key)
      return 0;
    memcpy((void *)(uintptr_t)key, token, token_len);
    ((char *)(uintptr_t)key)[token_len] = '\0';
    rt_init_str_raw(key, (int64_t)token_len);
    slot->key = key;
    slot->key_is_string = 1;
    slot->hash = hash;
    slot->value = rt_tag_v(1);
    ny_native_dict_set_ctrl(dict, (size_t)(slot - dict->slots),
                            NY_NATIVE_DICT_H2(hash));
    ++dict->length;
  }
  return result;
}

static int64_t ny_native_unbox_tbuf_fallback(int64_t value);

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
    if (rt_tbuf_header_valid(hdr))
      /*
       * Native NYIR get calls carry a raw index.  Do not apply the tagged
       * integer untag here: odd index 1 must remain 1, not become 0.
       */
      return rt_tbuf_get_any(value, key,
                             ny_native_unbox_tbuf_fallback(fallback));
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
  uint8_t key_is_string = 0;
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_DYNAMIC_VALUE,
                                  &key_is_string);
  return ny_native_dict_get_impl(value, key, fallback, key_is_string);
}
int64_t rt_dict_get_i64_raw(int64_t value, int64_t key, int64_t fallback) {
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_RAW_I64, NULL);
  return ny_native_dict_get_impl(value, key, fallback, false);
}
int64_t rt_dict_get_str_raw(int64_t value, int64_t key, int64_t fallback) {
  uint8_t key_is_string = 0;
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_RAW_CSTR,
                                  &key_is_string);
  return ny_native_dict_get_impl(value, key, fallback, key_is_string);
}

static char *ny_native_i64_decimal_key(int64_t value, char out[22]) {
  bool negative = value < 0;
  uint64_t magnitude = negative ? 0 - (uint64_t)value : (uint64_t)value;
  char *end = out + 22;
  *--end = '\0';
  do {
    *--end = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (negative)
    *--end = '-';
  return end;
}

int64_t rt_dict_get_i64_str_raw(int64_t value, int64_t key, int64_t fallback) {
  ny_native_dict_t *native = ny_native_dict_ptr(value);
  int64_t key_heap = rt_heap_object_ptr(key);
  bool bigint_key =
      key_heap &&
      *(const int64_t *)((uintptr_t)key_heap - sizeof(int64_t)) == TAG_BIGINT;
  if (native && !bigint_key) {
    /*
     * The key only needs to stay alive for this lookup. Formatting it into a
     * stack buffer avoids allocating a managed string for every integer-key
     * probe; stored dictionary keys retain their owned strings from insertion.
     */
    char key_buf[22];
    char *decimal = ny_native_i64_decimal_key(key, key_buf);
    return ny_native_dict_get_impl(value, (int64_t)(uintptr_t)decimal, fallback,
                                   1);
  }
  int64_t string = rt_i64_to_cstr_raw(key);
  if (!string)
    return fallback;
  if (native) {
    /*
     * rt_i64_to_cstr_raw returns a managed string object. Native dictionary
     * string slots store its C-string payload, so avoid the generic tagged
     * string classification at this known conversion boundary.
     */
    int64_t payload = rt_heap_object_ptr(string);
    if (payload)
      string = payload;
    return ny_native_dict_get_impl(value, string, fallback, 1);
  }
  return rt_dict_get_str_raw(value, string, fallback);
}
int64_t rt_dict_set_i64_str_raw(int64_t value, int64_t key, int64_t item) {
  int64_t string = rt_i64_to_cstr_raw(key);
  if (!string)
    return value;
  if (ny_native_dict_ptr(value)) {
    int64_t payload = rt_heap_object_ptr(string);
    if (payload)
      string = payload;
    return ny_native_dict_set_impl(value, string, item, 1);
  }
  return rt_native_dict_set_str_compact(value, string, item);
}

static int64_t ny_native_box_tbuf_any(int64_t value) {
  /*
   * Callable handles passed through an `any` fallback are already wrapped
   * with their dynamic-call ABI metadata. Preserve that wrapper instead of
   * treating its encoded word as an integer payload.
   */
  if (NY_DYNAMIC_CALLABLE_IS(value))
    return value;
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
  if (rt_word_is_plain_scalar(value))
    return rt_tag_v(value);
  /*
   * A native buffer stored in an elem-8 sequence is already a raw handle.
   * Preserve it as a container when it crosses the dynamic boundary; treating
   * the aligned address as an integer corrupts nested sequence reads.
   */
  if (rt_magic_tbuf_elem_size(value) > 0)
    return value;
  /*
   * Legacy managed lists store canonical boxed floats directly in their
   * slots.  Unlike compact native tbufs, they carry no side metadata, so
   * preserve the float handle before treating the word as a raw integer.
   */
  if (is_v_flt(value))
    return value;
  if (rt_native_is_str(value) || is_v_str(value))
    return value;
  /*
   * Element-8 sequence slots are raw scalar payloads at this boundary.  The
   * low bit is not provenance: a perfectly valid raw value such as 19999 is
   * odd and would otherwise be decoded as the tagged integer 9999 by the
   * next any consumer. Pointer-like values were filtered above, so every
   * remaining word must be boxed exactly once, regardless of magnitude.
   */
  return rt_tag_v(value);
}

/*
 * `rt_value_get_tagged` receives an already dynamic fallback, while
 * `rt_tbuf_get_any` expects a raw scalar fallback and boxes misses itself.
 * Convert only tagged integers at that boundary; booleans are immediates.
 */
static int64_t ny_native_unbox_tbuf_fallback(int64_t value) {
  return !rt_is_bool_imm(value) && is_int(value) ? rt_untag_v(value) : value;
}

/*
 * Fused plain-scalar memo for the elem-8 dynamic boundary.  Boxed reads and
 * dynamic appends classify the same machine words through the string, float,
 * tbuf, and heap classifiers on every element; for words that answer "plain
 * scalar" the full gauntlet reduces to one direct-mapped lookup.  Only the
 * negative answer is cached, and it carries exactly the stability assumption
 * the existing per-address negative caches (rt_non_str_cache,
 * rt_non_float_cache) already make: a word classified
 * as a non-pointer keeps that answer until the same address is reclassified
 * by a fresh probe through those caches.  Container/pointer answers are never
 * cached here, so forwarding and reallocation stay authoritative.
 *
 * Elem-8 slot reads classify every word through this memo.  Loop workloads
 * walk tens of thousands of distinct raw integers (indices, checksums,
 * LCG states); an 8k direct-mapped table thrashed on exactly that range
 * (values 0..40000 collided ~4x per slot), re-running the full
 * string/heap/header probe gauntlet on most reads.  64k entries keep the
 * whole sub-65536 integer range resident at 512 KiB per thread, in line
 * with the other runtime memo tables.
 */
#define RT_PLAIN_SCALAR_BITS 16
#define RT_PLAIN_SCALAR_SIZE (1u << RT_PLAIN_SCALAR_BITS)
#define RT_PLAIN_SCALAR_MASK (RT_PLAIN_SCALAR_SIZE - 1u)
static __thread uintptr_t rt_plain_scalar_keys[RT_PLAIN_SCALAR_SIZE];

static inline size_t rt_plain_scalar_slot(uintptr_t w) {
  uint64_t x = (uint64_t)w;
  x ^= x >> 33;
  x *= UINT64_C(0xff51afd7ed558ccd);
  x ^= x >> 33;
  return (size_t)x & RT_PLAIN_SCALAR_MASK;
}

static inline bool rt_plain_scalar_cached(uintptr_t w) {
  return w >= 4096 && rt_plain_scalar_keys[rt_plain_scalar_slot(w)] == w;
}

static inline void rt_plain_scalar_store(uintptr_t w) {
  if (w >= 4096)
    rt_plain_scalar_keys[rt_plain_scalar_slot(w)] = w;
}

/*
 * Single classification pass for a machine word at a dynamic boundary:
 * true when the word carries no pointer/float/string provenance and must be
 * boxed as a raw scalar.  Callers that already established `is_int` may skip
 * the float branch implicitly (tagged ints are odd, boxed floats are not).
 */
static bool rt_word_is_plain_scalar(int64_t value) {
  if ((uint64_t)value < 4096)
    return true;

  if (rt_plain_scalar_cached((uintptr_t)value))
    return true;
  if (NY_NATIVE_IS(value))
    return false;
  if (rt_magic_tbuf_elem_size(value) > 0)
    return false;
  if (is_v_flt(value))
    return false;
  if (is_v_str(value) || rt_native_is_str(value))
    return false;
  if (rt_heap_object_ptr(value))
    return false;
  rt_plain_scalar_store((uintptr_t)value);
  return true;
}

/*
 * Generic `any.get()` returns a tagged value. Native dictionaries can also be
 * populated by typed callers that store a plain raw scalar, so box those
 * results at this dynamic boundary while preserving already-tagged values,
 * booleans, pointers, and the caller's fallback.
 */
static int64_t ny_native_dict_dynamic_result(int64_t value, int64_t fallback) {
  if (value == fallback || value == 0 || rt_is_bool_imm(value) ||
      NY_DYNAMIC_CALLABLE_IS(value))
    return value;
  /*
   * Native string pointers may be odd, so test their provenance before the
   * low-bit tagged-integer predicate.
   */
  if (NY_NATIVE_IS(value) || rt_native_is_str(value) || is_v_str(value) ||
      is_v_flt(value) || rt_magic_tbuf_elem_size(value) > 0 ||
      rt_heap_object_ptr(value) || ny_native_dict_ptr(value))
    return value;
  if (is_int(value))
    return value;
  return rt_word_is_plain_scalar(value) ? rt_tag_v(value) : value;
}

int64_t rt_value_get_index_raw(int64_t value, int64_t index, int64_t fallback) {
  value = rt_tbuf_resolve_handle(value);
  /*
   * The result is a dynamic value even though the index is raw.
   * A string is a sequence at the source level, but its pointer payload can
   * happen to resemble a native-buffer header.  Classify it before the
   * header probe so `s[2]` reads one UTF-8 character instead of treating the
   * suffix `s[2:]` as a buffer slice.
   */
  if (rt_native_is_str(value) || is_v_str(value)) {
    int64_t length = rt_cstr_len(value);
    if (index < 0)
      index += length;
    if (index < 0 || index >= length)
      return fallback;
    return rt_cstr_to_str(rt_cstr_index_read_raw(value, index));
  }
  if (rt_sequence_tag(value) == TAG_BYTES) {
    if (!rt_header_readable_cached((uintptr_t)value - 16, 16))
      return fallback;
    int64_t length = *(int64_t *)((uintptr_t)value - 16);
    length = is_int(length) ? rt_untag_v(length) : length;
    if (index < 0)
      index += length;
    if (index < 0 || index >= length)
      return fallback;
    int64_t item = rt_bytes_index_read_raw(value, index);
    return rt_tag_v(item);
  }
  if (rt_magic_tbuf_elem_size(value) > 0) {
    int64_t length = rt_tbuf_len_raw(value);
    if (index < 0)
      index += length;
    if (index < 0 || index >= length)
      return fallback;
    return rt_tbuf_get_any(value, index,
                           ny_native_unbox_tbuf_fallback(fallback));
  }
  return rt_value_get_tagged(value, rt_tag_v(index), fallback);
}

int64_t rt_value_get_tagged(int64_t value, int64_t key, int64_t fallback) {
  uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  size_t ic_idx = ((caller ^ (caller >> 4)) & (NY_GET_IC_SIZE - 1));
  ny_get_ic_entry_t *ic = &g_get_ic_table[ic_idx];

  /*
   * Monomorphic receiver layout cache fast path
   */
  if (ic->caller == caller) {
    if (ic->kind == NY_GET_IC_DICT) {
      if (value && (uint64_t)value >= 4096 && (((uintptr_t)value & 7) == 0)) {
        ny_native_dict_t *dict = (ny_native_dict_t *)(uintptr_t)(value - 16);
        if (dict->magic == NY_NATIVE_DICT_MAGIC &&
            dict->capacity == ic->capacity &&
            (size_t)ic->slot_idx < (size_t)dict->capacity) {
          ny_native_dict_slot_t *slot = &dict->slots[ic->slot_idx];
          if ((dict->ctrl[ic->slot_idx] & 0x80) &&
              ((slot->key_is_string == 2 && key == 0 &&
                ic->key_is_string == 2) ||
               (slot->key_is_string == ic->key_is_string && slot->key == key) ||
               (ic->key_is_string == 1 && slot->key_is_string == 1 &&
                slot->hash == ic->cached_hash &&
                ny_native_dict_key_equal(slot->key, 1, key, 1)))) {
            if (slot->value != 0 && !NY_DYNAMIC_CALLABLE_IS(slot->value) &&
                !is_int(slot->value) && rt_native_is_int(slot->value) &&
                !rt_is_bool_imm(slot->value))
              return rt_tag_v(slot->value);
            return ny_native_dict_dynamic_result(slot->value, fallback);
          }
        }
      }
    } else if (ic->kind == NY_GET_IC_TBUF) {
      if (value && (uint64_t)value >= 4096 &&
          rt_magic_tbuf_elem_size(value) > 0) {
        int64_t idx = is_int(key) ? rt_untag_v(key) : key;
        return rt_tbuf_get_any(value, idx,
                               ny_native_unbox_tbuf_fallback(fallback));
      }
    }
  }

  /*
   * Fast path: monomorphic native dictionary receiver bypasses multi-type probe
   * ladder
   */
  if (value && (uint64_t)value >= 4096 && (((uintptr_t)value & 7) == 0)) {
    ny_native_dict_t *nd = ny_native_dict_ptr(value);
    if (nd) {
      uint8_t is_s =
          key == NY_IMM_NIL
              ? 2
              : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
      /*
       * Dynamic indexing carries integer keys in tagged-value form, while
       * the native SwissTable stores canonical raw i64 keys. Normalize the
       * key before entering the raw dictionary lookup (the explicit dict
       * bridges perform this conversion themselves).
       */
      key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_DYNAMIC_VALUE,
                                      &is_s);
      int64_t got =
          (key || is_s == 2)
              ? ny_native_dict_dynamic_result(
                    ny_native_dict_get_impl(value, key, fallback, is_s),
                    fallback)
              : fallback;
      return got;
    }
  }

  /*
   * Native tbuf handles may have an odd address and therefore look like a
   * tagged integer to the legacy predicate.  Resolve their representation
   * before rejecting an integer receiver.
   */
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
      value = decoded;
  }
  if (rt_magic_tbuf_elem_size(value) > 0) {
    ic->caller = caller;
    ic->kind = NY_GET_IC_TBUF;
    int64_t idx = is_int(key) ? rt_untag_v(key) : key;
    return rt_tbuf_get_any(value, idx, ny_native_unbox_tbuf_fallback(fallback));
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
    /*
     * Handle managed ranges before the low-bit integer test below: the
     * generic managed-value pointer spelling is not guaranteed to be even.
     */
    if (tag == TAG_RANGE) {
      int64_t start = *(int64_t *)((char *)(uintptr_t)heap_value + 0);
      int64_t stop = *(int64_t *)((char *)(uintptr_t)heap_value + 8);
      int64_t step = *(int64_t *)((char *)(uintptr_t)heap_value + 16);
      int64_t idx = is_int(key) ? rt_untag_v(key) : key;
      int64_t count = 0;
      if (step > 0 && start < stop)
        count = (stop - start + step - 1) / step;
      else if (step < 0 && start > stop)
        count = (start - stop - step - 1) / (-step);
      if (idx < 0)
        idx += count;
      if (idx < 0 || idx >= count)
        return fallback;
      return rt_tag_v(start + idx * step);
    }
  }
  /*
   * Registered malloc handles are opaque storage, even when their first
   * bytes happen to form a readable C string. Check provenance before the
   * low-bit integer and heuristic string tests.
   */
  if (rt_raw_ptr_registered(value))
    return fallback;
  if (is_int(value)) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("get expects a string, bytes, list, tuple, dict, "
                            "range, or vector, got int"));
    return fallback;
  }
  if (!value)
    return fallback;
  /*
   * A raw malloc pointer is not a sequence just because bytes before it
   * happen to resemble a tbuf header. Reject it before heuristic length
   * probing and preserve the dynamic fallback ABI.
   */
  if ((is_ptr(value) || ((uint64_t)value > NY_VALUE_PTR_MIN_ADDR &&
                         ((uint64_t)value & NY_VALUE_PTR_TAG_MASK) == 0)) &&
      !rt_heap_object_ptr(value) && !rt_native_is_str(value) &&
      !ny_native_dict_ptr(value) && !rt_tbuf_known_handle((uintptr_t)value))
    return fallback;
  /*
   * Some foreign/opaque pointers arrive with a non-canonical low-bit
   * spelling.  They are still never valid Ny sequences; the dynamic get
   * contract must treat them as a miss when a fallback was supplied.
   */
  if ((uint64_t)value > NY_VALUE_PTR_MIN_ADDR && is_int(value) &&
      !rt_heap_object_ptr(value) && !rt_native_is_str(value) &&
      !ny_native_dict_ptr(value) && !rt_tbuf_known_handle((uintptr_t)value))
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
    if (rt_tbuf_header_valid(hdr)) {
      int64_t idx = is_int(key) ? rt_untag_v(key) : key;
      int64_t raw_fallback = ny_native_unbox_tbuf_fallback(fallback);
      int64_t got = rt_tbuf_get(value, idx, raw_fallback);
      /*
       * Typed native list calls can pass an unboxed odd index.  If the
       * tagged interpretation misses but the raw spelling is in range,
       * retry it explicitly; this avoids guessing from low-bit parity alone
       * at the dynamic `.get` boundary.
       */
      if (got == raw_fallback && is_int(key) && key >= 0 && key < hdr[1] &&
          key != idx)
        got = rt_tbuf_get(value, key, raw_fallback);
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
      rt_tbuf_len_raw(value) > 0) {
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
      /*
       * rt_value_get_tagged receives its key in the dynamic ABI.
       */
      if (is_int(key))
        key = rt_untag_v(key);
      int64_t len = 0;
      if (step > 0 && start < stop)
        len = (stop - start + step - 1) / step;
      else if (step < 0 && start > stop)
        len = (start - stop - step - 1) / (-step);
      if (key < 0)
        key += len;
      if (key >= 0 && key < len)
        return rt_tag_v(start + key * step);
      return fallback;
    }
  }
  /*
   * Unknown/raw pointer receivers use the same miss ABI as the dynamic
   * caller.  The native lowering has already normalized literal defaults at
   * this boundary, so preserve that representation instead of tagging it a
   * second time.
   */
  return fallback;
}

int64_t rt_dict_set_raw(int64_t value, int64_t key, int64_t item) {
  uint8_t key_is_string = 0;
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_DYNAMIC_VALUE,
                                  &key_is_string);
  return ny_native_dict_set_impl(value, key, item, key_is_string);
}
int64_t rt_dict_set_i64_raw(int64_t value, int64_t key, int64_t item) {
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_RAW_I64, NULL);
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
  uint8_t string_key = key_tag == 121 || rt_native_is_str(key) ? 1 : 0;
  return ny_native_dict_set_impl(value, key, item, string_key);
}
int64_t rt_native_dict_set_native_i64(int64_t value, int64_t key,
                                      int64_t item) {
  return ny_native_dict_set_impl(value, key, item, false);
}
int64_t rt_native_dict_set_nir_i64(int64_t value, int64_t key, int64_t item) {
  /*
   * Native-only lowering represents transient to_str()/literal keys as raw
   * NUL-terminated pointers.  Classify them in the same string hash domain
   * as rt_dict_get_raw; otherwise inserts hash the pointer address while
   * lookups hash the bytes and only accidental/final entries can be found.
   */
  ny_native_dict_t *native = ny_native_dict_ptr(value);
  if (native) {
    uint8_t string_key =
        key == NY_IMM_NIL
            ? 2
            : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
    return ny_native_dict_set_native_impl(native, value, key, item, string_key);
  }
  uint8_t string_key = 0;
  key =
      ny_native_dict_bridge_key(value, key, NY_DICT_KEY_NIR_VALUE, &string_key);
  /*
   * Managed dictionary slots contain dynamic values. Native NIR producers
   * keep inferred integer locals unboxed, so normalize scalar values only
   * when this bridge targets a managed dictionary.
   */
  if (ny_native_managed_dict_ptr(value) && item != 0 &&
      !NY_DYNAMIC_CALLABLE_IS(item) && !is_int(item) &&
      rt_native_is_int(item) && !rt_is_bool_imm(item))
    item = rt_tag_v(item);
  return ny_native_dict_set_impl(value, key, item, string_key);
}

/*
 * Native lowering uses this entry only when semantic analysis proves the
 * stored value is an unboxed integer. Keep boxing inside the runtime call so
 * hot dictionary update loops do not emit a separate rt_tag call per item.
 */
int64_t rt_native_dict_set_nir_i64_raw_int(int64_t value, int64_t key,
                                           int64_t item) {
  ny_native_dict_t *native = ny_native_dict_ptr(value);
  item = rt_tag_v(item);
  if (native) {
    uint8_t string_key =
        key == NY_IMM_NIL
            ? 2
            : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
    return ny_native_dict_set_native_impl(native, value, key, item, string_key);
  }
  uint8_t string_key = 0;
  key =
      ny_native_dict_bridge_key(value, key, NY_DICT_KEY_NIR_VALUE, &string_key);
  return ny_native_dict_set_impl(value, key, item, string_key);
}

/*
 * Lowering selects this bridge only when both the key and value are proven
 * raw integers. Skip dynamic key classification on the native dictionary
 * hot path; preserve the checked bridge for other dictionary layouts.
 * Keep the checked compatibility path out of the native integer setter's hot
 * block. Native dictionaries are the common case after NYIR lowers a literal
 * dictionary, while this fallback must retain managed-dictionary key boxing.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((cold, noinline))
#endif
static int64_t ny_native_dict_set_nir_i64_raw_int_key_fallback(int64_t value,
                                                               int64_t key,
                                                               int64_t item) {
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_NIR_VALUE, NULL);
  return ny_native_dict_set_impl(value, key, rt_tag_v(item), false);
}

int64_t rt_native_dict_set_nir_i64_raw_int_key(int64_t value, int64_t key,
                                               int64_t item) {
  ny_native_dict_t *native = ny_native_dict_cached_ptr(value);
  if (native)
    return ny_native_dict_set_i64_impl(native, value, key, rt_tag_v(item));
  return ny_native_dict_set_nir_i64_raw_int_key_fallback(value, key, item);
}

int64_t rt_value_set_tagged(int64_t value, int64_t key, int64_t item) {
  /*
   * Dynamic nil is the zero word.  It is not a tbuf handle and must fail
   * before the compatibility setter probes storage adjacent to `value`.
   */
  if (value == 0)
    return rt_tag_v(0);
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
      if (rt_tbuf_header_valid(hdr))
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
    int64_t out = (rt_native_is_str(key) || is_v_str(key))
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
  value = rt_tbuf_resolve_handle(value);
  if (rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      int64_t result =
          rt_tbuf_set_tagged(value, is_int(key) ? rt_untag_v(key) : key, item);
      return result ? result : rt_tag_v(0);
    }
  }
  int64_t result = rt_tbuf_set_i64_raw(value, key, item);
  return result ? result : rt_tag_v(0);
}
int64_t rt_native_dict_set_str_compact(int64_t value, int64_t key,
                                       int64_t item) {
  if (ny_native_dict_ptr(value) && is_v_str(key)) {
    int64_t payload = rt_heap_object_ptr(key);
    if (payload)
      key = payload;
  }
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
    if (other->ctrl[i] >= 0x80u) {
      ny_native_dict_set_impl(value, slot->key, slot->value,
                              slot->key_is_string);
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
  if (dst->ctrl && src->ctrl)
    memcpy(dst->ctrl, src->ctrl, (size_t)src->capacity + 16);
  dst->length = src->length;
  dst->vector_dim = src->vector_dim;
  return dst_val;
}

static int ny_vec_dim_impl(int64_t v) {
  if (!v || is_int(v) || is_v_flt(v))
    return 0;
  int64_t handle =
      NY_NATIVE_IS(v) ? (int64_t)(uintptr_t)NY_NATIVE_DECODE(v) : v;
  ny_native_dict_t *dict = ny_native_dict_ptr(handle);
  if (!dict) {
    handle = rt_untag_v(v);
    dict = ny_native_dict_ptr(handle);
  }
  return dict && dict->vector_dim >= 2 && dict->vector_dim <= 4
             ? (int)dict->vector_dim
             : 0;
}

static double ny_vec_get_coord(int64_t v, const char *coord) {
  int64_t val = rt_dict_get_str_raw(v, (int64_t)(uintptr_t)coord, 0);
  return rt_any_to_f64(val);
}

static int64_t ny_vec_new_from_coords(int dim, double x, double y, double z,
                                      double w) {
  const char *tname = dim == 2 ? "vec2" : (dim == 3 ? "vec3" : "vec4");
  int64_t d = rt_dict_new_raw(8);
  ny_native_dict_t *dict = ny_native_dict_ptr(d);
  if (dict)
    dict->vector_dim = dim;
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
  double z = dim >= 3
                 ? ny_vec_get_coord(left, "z") + ny_vec_get_coord(right, "z")
                 : 0.0;
  double w = dim >= 4
                 ? ny_vec_get_coord(left, "w") + ny_vec_get_coord(right, "w")
                 : 0.0;
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
  double z = dim >= 3
                 ? ny_vec_get_coord(left, "z") - ny_vec_get_coord(right, "z")
                 : 0.0;
  double w = dim >= 4
                 ? ny_vec_get_coord(left, "w") - ny_vec_get_coord(right, "w")
                 : 0.0;
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
  if (!is_int(left) && rt_value_tag(left) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) * rt_any_to_f64(right));
  if (!is_int(right) && rt_value_tag(right) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) * rt_any_to_f64(right));
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
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
  if (!is_int(left) && rt_value_tag(left) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) / rt_any_to_f64(right));
  if (!is_int(right) && rt_value_tag(right) == TAG_FLOAT)
    return rt_flt_box_double(rt_any_to_f64(left) / rt_any_to_f64(right));
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
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
      rt_fail(rt_alloc_string("err.div_zero"),
              rt_alloc_string("division by zero"));
      return 0;
    }
    return rt_tag_v(rt_untag_v(left) / r);
  }
  return 0;
}

int64_t rt_any_mod(int64_t left, int64_t right) {
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_mod(rt_bigint_operand(left), rt_bigint_operand(right));
  if (is_int(left) && is_int(right)) {
    int64_t r = rt_untag_v(right);
    if (r == 0) {
      rt_fail(rt_alloc_string("err.div_zero"),
              rt_alloc_string("modulo by zero"));
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

rt_tbuf_error_t rt_tbuf_check(int64_t v, const int64_t **header_out) {
  if (header_out)
    *header_out = NULL;
  /*
   * Encoded native values are decoded only when the resulting handle has
   * allocator provenance; no memory contents participate in that decision.
   */
  if (NY_NATIVE_IS(v) && NY_NATIVE_DECODE(v) != NULL) {
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(v);
    if (rt_tbuf_registered_handle(decoded))
      v = (int64_t)decoded;
  }
  if (!v || v < 4096 || !rt_tbuf_registered_handle((uintptr_t)v))
    return RT_TBUF_INVALID_HANDLE;
  for (unsigned depth = 0; depth < 8; ++depth) {
    /*
     * Registration is the allocation-provenance check: only runtime-created
     * tbuf payloads are registered, and release removes that entry before
     * freeing the allocation. Re-probing the mapped pages here used mincore
     * on every list append and dominated dynamic-list workloads. Keep the
     * sanitizer poison check in instrumented builds, where allocator
     * redzones need separate validation.
     */
    if (!rt_tbuf_registered_handle((uintptr_t)v))
      return RT_TBUF_INVALID_HANDLE;
    uintptr_t header_addr = (uintptr_t)v - RT_NATIVE_TBUF_HEADER;
#if NY_WITH_ASAN
    if (__asan_region_is_poisoned((void *)header_addr, RT_NATIVE_TBUF_HEADER))
      return RT_TBUF_INVALID_HANDLE;
#endif
    int64_t *hdr = (int64_t *)header_addr;
    uint64_t magic = (uint64_t)hdr[0];
    if (magic == NY_NATIVE_TBUF_FORWARD_MAGIC) {
      int64_t next = hdr[1];
      if (next < 4096 || next == v ||
          !rt_tbuf_registered_handle((uintptr_t)next))
        return RT_TBUF_CORRUPT_HEADER;
      v = next;
      continue;
    }
    if (magic != NY_NATIVE_TBUF_MAGIC)
      return RT_TBUF_INVALID_HANDLE;
    if (hdr[1] < 0 || hdr[2] <= 0 || rt_tbuf_capacity_word(hdr[3]) < hdr[1])
      return RT_TBUF_CORRUPT_HEADER;
    if (header_out)
      *header_out = hdr;
    return RT_TBUF_OK;
  }
  return RT_TBUF_CORRUPT_HEADER;
}

int64_t rt_tbuf_direct_elem8_ptr_raw(int64_t buffer) {
  /*
   * The compiler uses this accessor in the proven elem-8 indexing path.  A
   * current handle-cache entry establishes allocator provenance for the
   * canonical payload, so validate its compact header directly instead of
   * running the general forwarding/registry walk on every array access.
   */
  uintptr_t data_addr = (uintptr_t)buffer;
  if (data_addr > RT_NATIVE_TBUF_HEADER &&
      rt_tbuf_cached_handle_fast(data_addr)) {
    const int64_t *fast_header =
        (const int64_t *)(data_addr - RT_NATIVE_TBUF_HEADER);
    bool readable = true;
#if NY_WITH_ASAN
    readable = !__asan_region_is_poisoned(fast_header, RT_NATIVE_TBUF_HEADER);
#endif
    if (readable && (uint64_t)fast_header[0] == NY_NATIVE_TBUF_MAGIC &&
        fast_header[1] >= 0 && fast_header[2] == 8 &&
        rt_tbuf_capacity_word(fast_header[3]) >= fast_header[1]) {
      return buffer;
    }
  }
  const int64_t *header = NULL;
  if (rt_tbuf_check(buffer, &header) != RT_TBUF_OK || header[2] != 8)
    return 0;
  uintptr_t data = (uintptr_t)header + RT_NATIVE_TBUF_HEADER;
  /*
   * Direct loads are valid only for the canonical payload pointer. A native
   * transport word or a stale forwarding pointer must use the checked read
   * accessor instead.
   */
  if ((uintptr_t)buffer != data)
    return 0;
  return (int64_t)data;
}

static bool rt_tbuf_header_valid(const int64_t *header) {
  uintptr_t address = (uintptr_t)header;
  if (!header || address > UINTPTR_MAX - RT_NATIVE_TBUF_HEADER)
    return false;
  const int64_t *checked = NULL;
  return rt_tbuf_check((int64_t)(address + RT_NATIVE_TBUF_HEADER), &checked) ==
             RT_TBUF_OK &&
         checked == header;
}

int64_t rt_magic_tbuf_elem_size(int64_t v) {
  const int64_t *header = NULL;
  return rt_tbuf_check(v, &header) == RT_TBUF_OK ? header[2] : -1;
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
  /*
   * Structural equality must compare the payloads of compact native handles,
   * not their tagged pointer spellings.
   */
  if (NY_NATIVE_IS(left) && NY_NATIVE_DECODE(left) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(left);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
      left = decoded;
  }
  if (NY_NATIVE_IS(right) && NY_NATIVE_DECODE(right) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(right);
    if (rt_tbuf_check(decoded, NULL) == RT_TBUF_OK)
      right = decoded;
  }
  if (rt_value_tag(left) == TAG_BIGINT || rt_value_tag(right) == TAG_BIGINT)
    return rt_bigint_cmp_raw(rt_bigint_operand(left),
                             rt_bigint_operand(right)) == 0
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
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
  /*
   * Dynamic equality recognizes only explicitly represented Ny strings.
   * Typed C-string operands use rt_cstr_eq directly at their call edge.
   */
  bool left_string = is_v_str(left) || rt_native_is_str(left);
  bool right_string = is_v_str(right) || rt_native_is_str(right);
  if (left_string && right_string) {
    return rt_cstr_eq(left, right) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
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
    if (readable_l && readable_r && rt_tbuf_header_valid(hdr_l) &&
        rt_tbuf_header_valid(hdr_r))
      return rt_tbuf_eq_raw(left, right);
    ny_native_dict_t *dict_l = ny_native_dict_ptr(left);
    ny_native_dict_t *dict_r = ny_native_dict_ptr(right);
    if (dict_l && dict_r) {
      if (dict_l->length != dict_r->length)
        return NY_IMM_FALSE;
      for (int64_t i = 0; i < dict_l->capacity; ++i) {
        if (dict_l->ctrl[i] >= 0x80u) {
          bool found = false;
          ny_native_dict_slot_t *s_r =
              ny_native_dict_find(dict_r, dict_l->slots[i].key,
                                  dict_l->slots[i].key_is_string, &found, NULL);
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
                                       uint8_t key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict && ny_native_managed_dict_ptr(value)) {
    if (key_is_string == 1)
      key = ny_native_managed_string_key(key);
    if (!key)
      return 0;
    return ny_native_managed_dict_has(value, key) ? 1 : 0;
  }
  bool found = false;
  (void)ny_native_dict_find(dict, key, key_is_string, &found, NULL);
  return found ? 1 : 0;
}
int64_t rt_dict_has_raw(int64_t value, int64_t key) {
  /*
   * This is the generic dictionary `has` bridge selected for both raw
   * integer and dynamic keys.  The frontend can route `d.contains("x")`
   * here after method lowering, even though the source-level key is a
   * tagged string.  Keep the integer fast path, but classify string objects
   * before normalizing integer representations so lookup uses the same hash
   * domain as insertion and `get`.
   */
  uint8_t key_is_string =
      key == NY_IMM_NIL ? 2
                        : (rt_native_is_str(key) != 0 || is_v_str(key) ? 1 : 0);
  if (key_is_string == 0 && !ny_native_dict_ptr(value) &&
      ny_native_managed_dict_ptr(value))
    key = ny_native_managed_i64_key(key);
  return ny_native_dict_has_impl(value, key, key_is_string);
}
int64_t rt_dict_has_str_raw(int64_t value, int64_t key) {
  /*
   * Match the normalization used by rt_dict_get_str_raw and string-key
   * insertion.  A tagged Ny string and a raw C-string literal can refer to
   * the same logical key, but hashing the tagged object pointer as bytes
   * probes a different bucket and incorrectly reports the key absent.
   */
  uint8_t key_is_string = 0;
  key = ny_native_dict_bridge_key(value, key, NY_DICT_KEY_RAW_CSTR,
                                  &key_is_string);
  return ny_native_dict_has_impl(value, key, key_is_string);
}

int64_t rt_dict_len_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  return dict ? dict->length : 0;
}

int64_t rt_set_len_raw(int64_t value) {
  if (!rt_set_layout(value))
    return 0;
  return *(int64_t *)(uintptr_t)value;
}

int64_t rt_len(int64_t value) {
  if (!value)
    return 0;
  /*
   * Tbuf capacity shares this word position with heap string tags. A buffer
   * whose capacity equals TAG_STR or TAG_STR_CONST must be recognized from
   * its registered handle before the generic tagged-string predicate.
   */
  if (rt_tbuf_cached_handle((uintptr_t)value)) {
    const int64_t *header = NULL;
    if (rt_tbuf_check(value, &header) == RT_TBUF_OK)
      return header[1];
  }
  /*
   * Managed sequences have an authoritative tag and count. Do not probe
   * foreign buffer/vector layouts for an allocator-owned list or dict.
   */
  int64_t managed = rt_heap_object_ptr(value);
  if (managed) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)managed - 8);
    switch (tag) {
    case TAG_LIST:
    case TAG_TUPLE:
    case TAG_DICT: {
      int64_t count = *(int64_t *)(uintptr_t)managed;
      return is_int(count) ? rt_untag_v(count) : count;
    }
    case TAG_STR:
    case TAG_STR_CONST:
    case TAG_BYTES:
    case TAG_RANGE:
    case TAG_SET:
      break;
    default:
      /*
       * Numeric, callable, result, and record objects are not sequences.
       * Their surrounding allocator bytes cannot define a foreign layout.
       */
      return 0;
    }
  }
  /*
   * Small tagged strings also satisfy rt_native_is_str().  Their address is
   * the inline byte payload, not a raw C-string pointer with an allocator
   * header, so read their encoded length before the native-string fallback.
   */
  if (is_v_str(value))
    return (int64_t)rt_tagged_str_len(value);
  if (rt_native_is_str(value)) {
    /*
     * Typed buffers are raw pointers and can look like native C strings.
     * Probe the adjacent header only on this ambiguous path, avoiding a
     * registry lookup for ordinary strings and non-string values.
     */
    if ((uintptr_t)value >= RT_NATIVE_TBUF_HEADER &&
        rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                  RT_NATIVE_TBUF_HEADER)) {
      int64_t *header = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
      if (rt_tbuf_header_valid(header))
        return header[1];
      if ((uint64_t)header[0] == NY_NATIVE_TBUF_FORWARD_MAGIC) {
        int64_t resolved = rt_tbuf_resolve_handle(value);
        if (resolved != value &&
            rt_header_readable_cached((uintptr_t)resolved -
                                          RT_NATIVE_TBUF_HEADER,
                                      RT_NATIVE_TBUF_HEADER)) {
          int64_t *resolved_header =
              (int64_t *)((uintptr_t)resolved - RT_NATIVE_TBUF_HEADER);
          if (rt_tbuf_header_valid(resolved_header))
            return resolved_header[1];
        }
      }
    }
    return rt_cstr_len(value);
  }
  /*
   * A dynamic boundary may leave the original buffer value pointing at a
   * forwarding header after a list promotion or growth.  Resolve it before
   * checking the header, just like the other tbuf readers; requiring the
   * original word to be a registered data pointer made `len(xs)` return zero
   * for valid forwarded handles.
   */
  int64_t tbuf = rt_tbuf_resolve_handle(value);
  if (tbuf >= RT_NATIVE_TBUF_HEADER &&
      rt_header_readable_cached((uintptr_t)tbuf - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)tbuf - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      return hdr[1];
    }
  }
  if (rt_has_tag(value, rt_tag_v(TAG_LIST)) == NY_IMM_TRUE)
    return rt_tbuf_len_raw(value);
  if (rt_set_layout(value)) {
    int64_t count = *(int64_t *)((uintptr_t)value);
    return count;
  }
  int vector_dim = ny_vec_dim_impl(value);
  if (vector_dim)
    return vector_dim;
  if (ny_native_dict_ptr(value) != NULL) {
    int64_t vector_type =
        rt_dict_get_str_raw(value, (int64_t)(uintptr_t)"__type", 0);
    if (rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"vec2") ||
        rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"Vector2"))
      return 2;
    if (rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"vec3") ||
        rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"Vector3"))
      return 3;
    if (rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"vec4") ||
        rt_cstr_eq(vector_type, (int64_t)(uintptr_t)"Vector4"))
      return 4;
    /*
     * Native dictionaries can also carry the generic dict tag.  Read their
     * SwissTable count before the managed-dictionary header probe below.
     * Only the standard vector type names override dictionary key count.
     */
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
  /*
   * Match the public len error contract for dynamic member access.
   */
  if (value == 0) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("len expects a sequence, got int"));
    return 0;
  }
  /*
   * `any` erases the dictionary/list representation before this bridge is
   * selected.  Test authoritative container layouts before the low-bit
   * immediate-integer predicate; managed dictionaries are commonly odd
   * addresses and were being rejected as integers.
   */
  if (ny_native_dict_ptr(value))
    return rt_len(value);
  if (rt_set_layout(value))
    return rt_len(value);
  if (ny_native_managed_dict_ptr(value)) {
    int64_t count = *(int64_t *)(uintptr_t)value;
    return is_int(count) ? rt_untag_v(count) : count;
  }
  if (rt_tbuf_known_handle((uintptr_t)value) &&
      rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    return rt_len(value);
  }
  int64_t heap = rt_heap_object_ptr(value);
  if (heap) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE || tag == TAG_BYTES ||
        tag == TAG_RANGE || tag == TAG_DICT || tag == TAG_SET)
      return rt_len(value);
  }
  if (is_int(value) && !rt_native_is_str(value) && !is_v_str(value)) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("len expects a sequence, got int"));
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
    if (rt_tbuf_header_valid(hdr))
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
    rt_fail(rt_alloc_string("err.runtime"),
            rt_alloc_string("len expects sequence or collection, got int"));
    return 0;
  }
  if (!value) {
    rt_fail(rt_alloc_string("err.runtime"),
            rt_alloc_string("len expects sequence or collection, got nil"));
    return 0;
  }
  /*
   * Empty typed buffers have a valid zero length.  The generic safe-length
   * path uses zero for both "empty" and "not a sequence", so check the
   * authoritative typed-buffer header before interpreting that result.
   */
  const int64_t *tbuf_header = NULL;
  if (rt_tbuf_check(value, &tbuf_header) == RT_TBUF_OK)
    return tbuf_header[1];
  /*
   * Sets are collection values with a count/capacity header rather than a
   * sequence buffer.  Dynamic `.len` must expose that count instead of
   * falling through to the zero-length dictionary/string compatibility path.
   */
  if (rt_set_layout(value)) {
    int64_t count = *(int64_t *)((uintptr_t)value);
    return count;
  }
  int64_t len = rt_sequence_len_safe(value);
  if (len > 0)
    return len;
  if (rt_header_readable_cached((uintptr_t)value - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr))
      return hdr[1];
  }
  if (ny_native_dict_ptr(value) != NULL || rt_native_is_str(value))
    return 0;
  rt_fail(rt_alloc_string("err.runtime"),
          rt_alloc_string("len expects sequence or collection"));
  return 0;
}

static int64_t ny_native_dict_delete_impl(int64_t value, int64_t key,
                                          uint8_t key_is_string) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  bool found = false;
  ny_native_dict_slot_t *slot =
      ny_native_dict_find(dict, key, key_is_string, &found, NULL);
  if (found && slot) {
    slot->key = 0;
    slot->value = 0;
    slot->key_is_string = 0;
    ny_native_dict_set_ctrl(dict, (size_t)(slot - dict->slots),
                            NY_NATIVE_DICT_TOMBSTONE);
    dict->length--;
  }
  return value;
}
int64_t rt_dict_delete_raw(int64_t value, int64_t key) {
  /*
   * Native table insertion and this raw-key bridge share the unboxed key
   * domain. Tagging here changes odd keys and makes deletion miss.
   */
  return ny_native_dict_delete_impl(value, key, false);
}
int64_t rt_dict_delete_str_raw(int64_t value, int64_t key) {
  return ny_native_dict_delete_impl(value, key, true);
}

/*
 * Generic delete/remove boundary used when native lowering cannot prove
 * whether an `any` receiver is a set or dictionary. Preserve the runtime
 * layout check here instead of treating every unknown object as a dict.
 */
static int64_t ny_native_managed_dict_delete(int64_t value, int64_t key) {
  if (!ny_native_managed_dict_ptr(value) ||
      !rt_addr_readable_safe((uintptr_t)value, 24))
    return value;
  int64_t cap_word = *(int64_t *)((char *)(uintptr_t)value + 8);
  bool tagged = is_int(cap_word);
  int64_t cap = tagged ? cap_word >> 1 : cap_word;
  int64_t table_word = *(int64_t *)((char *)(uintptr_t)value + 16);
  int64_t table = is_int(table_word) ? table_word >> 1 : table_word;
  if (cap <= 0 || cap > (int64_t)((SIZE_MAX - 24u) / 24u) ||
      (uint64_t)table < 4096 ||
      !rt_addr_readable_safe((uintptr_t)table, (size_t)cap * 24u))
    return value;
  int64_t off = rt_dict_find_off_table(table, cap, key, tagged);
  if (off < 0)
    return value;
  int64_t *state_slot = (int64_t *)((char *)(uintptr_t)table + off + 16);
  int64_t state = *state_slot;
  if (tagged && state)
    state = rt_untag_v(state);
  if (state != 1)
    return value;
  *(int64_t *)((char *)(uintptr_t)table + off) = 0;
  *(int64_t *)((char *)(uintptr_t)table + off + 8) = 0;
  *state_slot = tagged ? rt_tag_v(2) : 2;
  int64_t count_word = *(int64_t *)((char *)(uintptr_t)value);
  int64_t count = is_int(count_word) ? count_word >> 1 : count_word;
  *(int64_t *)((char *)(uintptr_t)value) =
      tagged ? rt_tag_v(count > 0 ? count - 1 : 0)
             : (count > 0 ? count - 1 : 0);
  return value;
}

int64_t rt_any_collection_remove_raw(int64_t value, int64_t key) {
  int64_t heap = rt_heap_object_ptr(value);
  int64_t container = heap ? heap : value;
  if (rt_set_layout(container))
    return rt_set_remove_raw(container, key);

  uint8_t key_is_string = 0;
  key = ny_native_dict_bridge_key(container, key, NY_DICT_KEY_DYNAMIC_VALUE,
                                  &key_is_string);
  if (ny_native_dict_ptr(container))
    return ny_native_dict_delete_impl(container, key, key_is_string);
  if (ny_native_managed_dict_ptr(container))
    return ny_native_managed_dict_delete(container, key);
  return value;
}

int64_t rt_dict_clear_raw(int64_t value) {
  ny_native_dict_t *dict = ny_native_dict_ptr(value);
  if (!dict || !dict->slots)
    return value;
  memset(dict->slots, 0, (size_t)dict->capacity * sizeof(*dict->slots));
  if (dict->ctrl)
    memset(dict->ctrl, 0, (size_t)dict->capacity + 16);
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
        bool is_s = rt_native_is_str(key) || is_v_str(key);
        int64_t tag =
            is_s ? 121
                 : (rt_is_bool_imm(key) ? NY_TBUF_TAG_BOOL : rt_value_tag(key));
        if (!is_s && is_int(key) && !rt_is_bool_imm(key)) {
          key = rt_untag_v(key);
          tag = 1;
        }
        char *elem = (char *)(uintptr_t)out + pos * 24;
        *(int64_t *)elem = key;
        *(int64_t *)(elem + 8) = is_s ? rt_cstr_len(key) : 0;
        *(int64_t *)(elem + 16) = tag;
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
    if (dict->ctrl[i] >= 0x80u) {
      int64_t k = dict->slots[i].key;
      bool is_s = dict->slots[i].key_is_string;
      int64_t tag =
          is_s ? 121 : (rt_is_bool_imm(k) ? NY_TBUF_TAG_BOOL : rt_value_tag(k));
      if (!is_s && is_int(k) && !rt_is_bool_imm(k)) {
        k = rt_untag_v(k);
        tag = 1;
      }
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
    if (dict->ctrl[i] >= 0x80u) {
      int64_t v = dict->slots[i].value;
      bool is_s = rt_native_is_str(v);
      int64_t tag =
          is_s ? 121 : (rt_is_bool_imm(v) ? NY_TBUF_TAG_BOOL : rt_value_tag(v));
      /*
       * Dictionary slots use the public tagged scalar ABI, while descriptor
       * tbufs store a raw payload plus an explicit tag.  Decode exactly once
       * at this boundary; copying `21` for integer `10` makes enumeration
       * expose the encoding instead of the value.
       */
      if (!is_s && is_int(v) && !rt_is_bool_imm(v)) {
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
    if (dict->ctrl[i] < 0x80u)
      continue;
    int64_t pair = rt_tbuf_new_raw(2, 24);
    if (!pair)
      return out;
    int64_t k = slot->key;
    bool is_s = slot->key_is_string;
    int64_t k_tag =
        is_s ? 121 : (rt_is_bool_imm(k) ? NY_TBUF_TAG_BOOL : rt_value_tag(k));
    if (!is_s && is_int(k) && !rt_is_bool_imm(k)) {
      k = rt_untag_v(k);
      k_tag = 1;
    }
    int64_t k_len = is_s ? rt_cstr_len(k) : 0;
    char *elem0 = (char *)(uintptr_t)pair;
    *(int64_t *)elem0 = k;
    *(int64_t *)(elem0 + 8) = k_len;
    *(int64_t *)(elem0 + 16) = k_tag;

    int64_t v = slot->value;
    bool v_is_s = rt_native_is_str(v);
    int64_t v_tag =
        v_is_s ? 121 : (rt_is_bool_imm(v) ? NY_TBUF_TAG_BOOL : rt_value_tag(v));
    if (!v_is_s && is_int(v) && !rt_is_bool_imm(v)) {
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
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string("contains expects a string, list, tuple, dict, "
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
      (rt_value_tag(item) == TAG_STR || rt_value_tag(item) == TAG_STR_CONST))
    return strstr((const char *)(uintptr_t)container,
                  (const char *)(uintptr_t)item)
               ? NY_IMM_TRUE
               : NY_IMM_FALSE;
  if (ny_native_dict_ptr(container) != NULL) {
    return rt_dict_has_raw(container, item) ? NY_IMM_TRUE : NY_IMM_FALSE;
  }
  /*
   * Prefer the managed set header over a speculative tbuf-header probe.
   */
  int64_t managed_v = rt_heap_object_ptr(container);
  int64_t managed_tag =
      managed_v ? *(int64_t *)((char *)(uintptr_t)managed_v - 8) : 0;
  if (managed_v && managed_tag == TAG_SET) {
    int64_t cap_raw = *(int64_t *)((char *)(uintptr_t)managed_v + 8);
    int64_t cap = is_int(cap_raw) ? (cap_raw >> 1) : cap_raw;
    for (int64_t i = 0; i < cap; ++i) {
      int64_t off = 16 + i * 24;
      int64_t st = *(int64_t *)((char *)(uintptr_t)managed_v + off + 16);
      if (st == 1 || st == 3) {
        int64_t k = *(int64_t *)((char *)(uintptr_t)managed_v + off);
        bool key_is_string = is_v_str(k) || rt_native_is_str(k);
        bool item_is_string = is_v_str(item) || rt_native_is_str(item);
        if (k == item ||
            (key_is_string && item_is_string && rt_cstr_eq(k, item)))
          return NY_IMM_TRUE;
      }
    }
    return NY_IMM_FALSE;
  }
  if (rt_header_readable_cached((uintptr_t)container - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)container - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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
          bool key_is_string = is_v_str(k) || rt_native_is_str(k);
          bool item_is_string = is_v_str(item) || rt_native_is_str(item);
          if (k == item ||
              (key_is_string && item_is_string && rt_cstr_eq(k, item)))
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
        bool elem_is_string = is_v_str(elem) || rt_native_is_str(elem);
        bool item_is_string = is_v_str(item) || rt_native_is_str(item);
        if (elem == item ||
            (elem_is_string && item_is_string && rt_cstr_eq(elem, item)))
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
static size_t g_trace_repeat_threshold = 1;
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

int64_t rt_alloc_small_string_len(const char *s, size_t len) {
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
  slot->fake_magic = NY_SSO_STRING_MAGIC;
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
static NY_TLS size_t g_trace_arg_counts[CALL_STACK_MAX];
static NY_TLS size_t g_trace_profile_index[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_start[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_cpu_start[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_rss_start[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_rss_sample_counter;
static NY_TLS uint64_t g_trace_profile_child_ns[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_child_cpu_ns[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_alloc_start[CALL_STACK_MAX];
static NY_TLS uint64_t g_trace_profile_gc_alloc_start[CALL_STACK_MAX];
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
  return trace_func_matches(func, "core.reflect._") ||
         trace_func_matches(func, "core.reflect.repr") ||
         trace_func_matches(func, "core.reflect.to_str") ||
         trace_func_matches(func, "core.reflect.type") ||
         trace_func_matches(func, "core.len") ||
         trace_func_matches(func, "core.get") ||
         trace_func_matches(func, "core.put") ||
         trace_func_matches(func, "core.append") ||
         trace_func_matches(func, "core.pop") ||
         trace_func_matches(func, "core.extend") ||
         trace_func_matches(func, "core.contains") ||
         trace_func_matches(func, "core.slice") ||
         trace_func_matches(func, "core.error.panic");
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
  intptr_t rfd = (intptr_t)rt_untag_v(fd);
  intptr_t rlen = (intptr_t)rt_untag_v(len);
  if (rfd != 1) {
    extern int64_t rt_write_off(int64_t fd, int64_t buf, int64_t len,
                                int64_t off);
    return rt_write_off(fd, buf, len, 0);
  }
  if (rlen < 0)
    return rt_tag_v(-1);
  if (rlen == 0)
    return len;
  uintptr_t raw_buf = (uintptr_t)buf;
  if (!rt_addr_readable_safe(raw_buf, (size_t)rlen)) {
    if (NY_NATIVE_IS(buf))
      raw_buf = (uintptr_t)NY_NATIVE_DECODE(buf);
    else if (is_int(buf))
      raw_buf = (uintptr_t)rt_untag_v(buf);
  }
  if (!rt_check_oob("sys_write", (int64_t)raw_buf, 0, (size_t)rlen))
    return rt_tag_v(-1);
  char *ptr = (char *)raw_buf;
  rt_print_put(ptr, (size_t)rlen);
  return len;
}

/*
 * NYIR calls typed scalar arguments unboxed. Give it a distinct raw entry so
 * odd raw descriptors and lengths are never inferred from their low bits.
 */
int64_t rt_write_buffered_raw(int64_t fd, int64_t buf, int64_t len) {
  if (fd < 0 || len < 0)
    return -1;
  if (len == 0)
    return 0;
  if (fd != 1) {
    extern int64_t rt_write_off_raw(int64_t fd, int64_t buf, int64_t len,
                                    int64_t off);
    return rt_write_off_raw(fd, buf, len, 0);
  }
  if (!rt_check_oob("sys_write", buf, 0, (size_t)len))
    return -1;
  rt_print_put((const char *)(uintptr_t)buf, (size_t)len);
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

/*
 * Two-part rt_alloc_string_len: joins a and b into one managed string
 * without a scratch copy. Either part may be NULL/empty.
 */
int64_t rt_alloc_string_len_ptr(const char *a, size_t a_len, const char *b,
                                size_t b_len) {
  if (!a)
    a = "";
  if (!b)
    b = "";
  if (a_len == 0)
    return rt_alloc_string_len(b, b_len);
  if (b_len == 0)
    return rt_alloc_string_len(a, a_len);
  if (a_len > SIZE_MAX - b_len - 1)
    return 0;
  size_t total = a_len + b_len;
  char *joined = malloc(total + 1);
  if (!joined)
    return 0;
  memcpy(joined, a, a_len);
  memcpy(joined + a_len, b, b_len);
  joined[total] = '\0';
  int64_t res = rt_alloc_string_len(joined, total);
  free(joined);
  return res;
}

int64_t rt_alloc_string(const char *s) {
  if (!s)
    return 0;
  return rt_alloc_string_len(s, strlen(s));
}

int rt_checked_cstr_len(const char *text, size_t max_bytes, size_t *out_len) {
  if (!text || !out_len || max_bytes == 0)
    return 0;
  uintptr_t base = (uintptr_t)text;
  if (base < 0x1000 || base > UINTPTR_MAX - max_bytes)
    return 0;

  size_t scanned = 0;
  while (scanned < max_bytes) {
    uintptr_t current = base + scanned;
    size_t page_left = 4096u - (current & 4095u);
    size_t chunk = max_bytes - scanned;
    if (chunk > page_left)
      chunk = page_left;
    if (chunk > 256u)
      chunk = 256u;

    if (rt_addr_readable_safe(current, chunk)) {
      const char *nul = memchr((const void *)current, '\0', chunk);
      if (nul) {
        *out_len = (size_t)(nul - text);
        return 1;
      }
      scanned += chunk;
      continue;
    }

    /*
     * The range may be inside a mapped page but outside an ASAN allocation.
     * Narrow to byte reads so a short, valid string before the poisoned tail
     * remains accepted without touching the inaccessible byte.
     */
    size_t i = 0;
    for (; i < chunk; ++i) {
      uintptr_t byte = current + i;
      if (!rt_addr_readable_safe(byte, 1))
        return 0;
      if (*(const char *)byte == '\0') {
        *out_len = scanned + i;
        return 1;
      }
    }
    scanned += i;
    if (i != chunk)
      return 0;
  }
  return 0;
}

int64_t rt_cstr_to_str(int64_t p_v) {
  if (p_v == NY_IMM_NIL || p_v == 0)
    return NY_IMM_NIL;
  int64_t raw = p_v;
  bool native_candidate = NY_NATIVE_IS(p_v);
  /*
   * Raw pointers can share the native-handle marker bits, so prefer a
   * directly readable address.  FFI pointer results use the shifted native
   * representation; decode those only when the original word is not itself
   * readable.  The explicit cstr_to_str call authorizes reading the decoded
   * address, and the bounded terminator check below validates the contents.
   */
  if (native_candidate) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(p_v);
    if (!rt_addr_readable_safe((uintptr_t)p_v, 1) &&
        rt_addr_readable_safe((uintptr_t)decoded, 1))
      raw = decoded;
  }
  /*
   * C-string conversion is also used at statically-known `str` ABI
   * boundaries. A managed string can have marker bits that also match the
   * native-handle encoding, so after resolving any unreadable native handle,
   * validate the resulting pointer as a counted string before falling back
   * to NUL-terminated C-string handling.
   * `p_v` is also used for raw pointers to static LLVM string constants.
   * Header probing such a pointer can mistake adjacent constant data for a
   * tagged-string header. Managed strings and SSO values have explicit
   * runtime provenance; other readable addresses go through the C-string
   * conversion below.
   */
  if ((rt_heap_object_ptr(raw) && is_v_str(raw)) || rt_is_sso_string(raw))
    return raw;
  /*
   * JIT string constants are not required to be word-aligned: LLVM may pass
   * the address of a byte string whose low bit is set.  Test readable-pointer
   * provenance before interpreting the low bit as Ny's integer tag.
   */
  if (is_int(raw) && (!rt_addr_readable_safe((uintptr_t)raw, 1) ||
                      (uintptr_t)raw <= NY_VALUE_PTR_MIN_ADDR))
    raw = rt_untag_v(raw);
  if (!raw || (uintptr_t)raw <= NY_VALUE_PTR_MIN_ADDR)
    return NY_IMM_NIL;
  /*
   * C-string conversion is an ABI boundary: callers may pass a raw pointer
   * from an untyped value.  Validate the first byte and require a bounded NUL
   * terminator before handing it to the managed string allocator.
   */
  if (!rt_addr_readable_safe((uintptr_t)raw, 1))
    return NY_IMM_NIL;
  size_t n = 0;
  if (!rt_checked_cstr_len((const char *)(uintptr_t)raw, 1u << 20, &n))
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

static size_t trace_repeat_threshold_from_env(void) {
  const char *raw = getenv("NYTRIX_TRACE_REPEAT_THRESHOLD");
  if (!raw || !*raw)
    return 1;
  char *end = NULL;
  unsigned long long value = strtoull(raw, &end, 10);
  if (end == raw || *end != '\0' || value == 0)
    return 1;
  if (value > SIZE_MAX)
    return SIZE_MAX;
  return (size_t)value;
}

void rt_trace_refresh_env(void) {
  g_trace_env_trace = trace_env_enabled_uncached("NYTRIX_TRACE");
  g_trace_env_calls = trace_env_enabled_uncached("NYTRIX_TRACE_CALLS");
  g_trace_env_values = trace_env_enabled_uncached("NYTRIX_TRACE_VALUES");
  g_trace_env_verbose = trace_env_enabled_uncached("NYTRIX_TRACE_VERBOSE");
  g_trace_env_index_read =
      trace_env_enabled_uncached("NYTRIX_INDEX_READ_PARITY");
  g_trace_repeat_threshold = trace_repeat_threshold_from_env();
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
 * Runtime call tracing can visit the same helper thousands of times in a
 * tight loop or a deep recursion, and the unwinding return records repeat
 * the same way.  Keep the first record of a run, collapse immediately
 * adjacent identical records (calls keyed on function and location, returns
 * also on the rendered value), and flush the total count before a different
 * record so call nesting stays readable.  A run that spans several stack
 * depths is a recursion, which is reported with its depth range instead of
 * one line per level.
 */

typedef struct {
  char *data;
  size_t len, cap;
} trace_str_t;

static void trace_str_addf(trace_str_t *s, const char *fmt, ...) {
  if (!s)
    return;
  va_list ap;
  va_start(ap, fmt);
  va_list copy;
  va_copy(copy, ap);
  int need = vsnprintf(NULL, 0, fmt, copy);
  va_end(copy);
  if (need < 0) {
    va_end(ap);
    return;
  }
  if (s->len + (size_t)need + 1 > s->cap) {
    size_t cap = s->cap ? s->cap : 128;
    while (cap < s->len + (size_t)need + 1)
      cap *= 2;
    char *grown = realloc(s->data, cap);
    if (!grown) {
      va_end(ap);
      return;
    }
    s->data = grown;
    s->cap = cap;
  }
  vsnprintf(s->data + s->len, (size_t)need + 1, fmt, ap);
  s->len += (size_t)need;
  va_end(ap);
}

static trace_str_t g_trace_pending; /* record under construction */
static bool g_trace_pending_open;
static int g_trace_pending_kind; /* 0 = call, 1 = return */
static size_t g_trace_pending_depth;

static trace_str_t g_trace_run_msg; /* last committed record message */
static int g_trace_run_kind;
static size_t g_trace_run_min_depth;
static size_t g_trace_run_max_depth;
static size_t g_trace_run_repeats;
static bool g_trace_run_valid;
static bool g_trace_run_flush_registered;

static const char *trace_run_kind_word(int kind) {
  return kind == 0 ? "call" : "return";
}

static void trace_flush_run(void) {
  if (!g_trace_run_valid) {
    g_trace_run_repeats = 0;
    return;
  }
  if (g_trace_run_repeats) {
    size_t total = g_trace_run_repeats + 1;
    if (total < g_trace_repeat_threshold) {
      for (size_t i = 0; i < g_trace_run_repeats; ++i) {
        trace_print_indent(g_trace_run_max_depth);
        fprintf(stderr, "%s\n", g_trace_run_msg.data);
      }
      g_trace_run_repeats = 0;
      g_trace_run_valid = false;
      return;
    }
    trace_print_indent(g_trace_run_max_depth);
    if (g_trace_run_max_depth != g_trace_run_min_depth)
      fprintf(stderr,
              "[trace] ... previous %s repeated x%zu (recursive, "
              "depth %zu..%zu)\n",
              trace_run_kind_word(g_trace_run_kind), total,
              g_trace_run_min_depth, g_trace_run_max_depth);
    else
      fprintf(stderr, "[trace] ... previous %s repeated x%zu\n",
              trace_run_kind_word(g_trace_run_kind), total);
  }
  g_trace_run_repeats = 0;
  g_trace_run_valid = false;
}

static void trace_commit_pending(void) {
  if (!g_trace_pending_open)
    return;
  g_trace_pending_open = false;
  bool same = g_trace_run_valid && g_trace_run_kind == g_trace_pending_kind &&
              g_trace_run_msg.len == g_trace_pending.len &&
              (g_trace_pending.len == 0 ||
               memcmp(g_trace_run_msg.data, g_trace_pending.data,
                      g_trace_pending.len) == 0);
  if (same) {
    ++g_trace_run_repeats;
    if (g_trace_pending_depth < g_trace_run_min_depth)
      g_trace_run_min_depth = g_trace_pending_depth;
    if (g_trace_pending_depth > g_trace_run_max_depth)
      g_trace_run_max_depth = g_trace_pending_depth;
    g_trace_pending.len = 0;
    return;
  }
  trace_flush_run();
  trace_print_indent(g_trace_pending_depth);
  if (g_trace_pending.len)
    fwrite(g_trace_pending.data, 1, g_trace_pending.len, stderr);
  fputc('\n', stderr);
  trace_str_t tmp = {0};
  trace_str_addf(&tmp, "%.*s", (int)g_trace_pending.len, g_trace_pending.data);
  free(g_trace_run_msg.data);
  g_trace_run_msg = tmp;
  g_trace_run_kind = g_trace_pending_kind;
  g_trace_run_min_depth = g_trace_pending_depth;
  g_trace_run_max_depth = g_trace_pending_depth;
  g_trace_run_repeats = 0;
  g_trace_run_valid = true;
  g_trace_pending.len = 0;
}

/*
 * Per-function call counts for the end-of-run "top calls" summary.
 */
#define NY_TRACE_TOP_MAX 256
typedef struct {
  char name[96];
  size_t count;
  uint64_t inclusive_ns;
  uint64_t exclusive_ns;
  uint64_t cpu_ns;
  uint64_t exclusive_cpu_ns;
  uint64_t sampled_rss_delta_bytes;
  uint64_t sampled_rss_peak_bytes;
  uint64_t allocations;
} ny_trace_top_t;
static ny_trace_top_t g_trace_tops[NY_TRACE_TOP_MAX];
static size_t g_trace_top_len;
static atomic_flag g_trace_profile_lock = ATOMIC_FLAG_INIT;
static atomic_int g_trace_profile_active = -1;

static bool ny_trace_profile_enabled(void) {
  int active =
      atomic_load_explicit(&g_trace_profile_active, memory_order_relaxed);
  if (active < 0) {
    const char *env = getenv("NYTRIX_PROFILER");
    int initial = env && *env && strcmp(env, "0") != 0 ? 1 : 0;
    int expected = -1;
    atomic_compare_exchange_strong_explicit(&g_trace_profile_active, &expected,
                                            initial, memory_order_relaxed,
                                            memory_order_relaxed);
    active =
        atomic_load_explicit(&g_trace_profile_active, memory_order_relaxed);
  }
  return active != 0;
}

static void ny_trace_profile_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_trace_profile_lock,
                                           memory_order_acquire)) {
  }
}

static void ny_trace_profile_unlock(void) {
  atomic_flag_clear_explicit(&g_trace_profile_lock, memory_order_release);
}

static uint64_t ny_trace_profile_now(void) {
  extern int64_t rt_ticks_ns(void);
  int64_t now = rt_ticks_ns();
  if (is_int(now))
    now = rt_untag_v(now);
  return now > 0 ? (uint64_t)now : 0;
}

static uint64_t ny_trace_profile_rss_bytes(void) {
#ifdef __linux__
  FILE *statm = fopen("/proc/self/statm", "r");
  unsigned long total_pages = 0, resident_pages = 0;
  if (statm) {
    int ok = fscanf(statm, "%lu %lu", &total_pages, &resident_pages);
    fclose(statm);
    if (ok == 2) {
      long page_size = sysconf(_SC_PAGESIZE);
      if (page_size > 0)
        return (uint64_t)resident_pages * (uint64_t)page_size;
    }
  }
#else
  (void)0;
#endif
  return 0;
}

static uint64_t ny_trace_profile_cpu_now(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
  struct timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
#endif
  return (uint64_t)((double)clock() * (1000000000.0 / CLOCKS_PER_SEC));
}

static int ny_trace_top_cmp(const void *pa, const void *pb) {
  const ny_trace_top_t *a = pa, *b = pb;
  const char *mode = getenv("NYTRIX_PROFILE_MODE");
  if (mode && strcmp(mode, "verbose") == 0) {
    if (a->count != b->count)
      return a->count > b->count ? -1 : 1;
  } else {
    if (a->exclusive_cpu_ns != b->exclusive_cpu_ns)
      return a->exclusive_cpu_ns > b->exclusive_cpu_ns ? -1 : 1;
  }
  return strcmp(a->name, b->name);
}

/*
 * Trace names cross the same tagged/native string boundary as ordinary
 * callable arguments.  Keep that representation normalization in one place
 * so counters and human-readable trace output cannot disagree.
 */
static bool ny_trace_string_view(int64_t value, const char **out,
                                 size_t *out_len) {
  if (!out || !out_len || !value)
    return false;
  if (is_v_str(value)) {
    *out = (const char *)(uintptr_t)value;
    *out_len = rt_tagged_str_len(value);
    return true;
  }
  if (NY_NATIVE_IS(value) && NY_NATIVE_DECODE(value) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(value);
    if (rt_native_is_str(decoded))
      value = decoded;
  }
  if (!rt_native_is_str(value))
    return false;
  *out = (const char *)(uintptr_t)value;
  *out_len = (size_t)rt_cstr_len(value);
  return true;
}

static void ny_trace_top_summary(void) {
  ny_trace_profile_lock();
  if (!g_trace_top_len) {
    ny_trace_profile_unlock();
    return;
  }
  qsort(g_trace_tops, g_trace_top_len, sizeof(*g_trace_tops), ny_trace_top_cmp);
  fprintf(stderr, "[trace] hottest frames (top %zu of %zu): ",
          g_trace_top_len < 10 ? g_trace_top_len : (size_t)10, g_trace_top_len);
  size_t n = g_trace_top_len < 10 ? g_trace_top_len : (size_t)10;
  for (size_t i = 0; i < n; ++i)
    fprintf(stderr, "%s%s=%zu%s", i ? ", " : "", g_trace_tops[i].name,
            g_trace_tops[i].count, i + 1 < n ? "" : "\n");
  for (size_t i = n; i < g_trace_top_len; ++i) {
    if (g_trace_tops[i].count != g_trace_tops[n - 1].count)
      break;
    fprintf(stderr, ", %s=%zu", g_trace_tops[i].name, g_trace_tops[i].count);
  }
  if (n)
    fputc('\n', stderr);
  if (getenv("NYTRIX_PROFILER")) {
    const char *mode = getenv("NYTRIX_PROFILE_MODE");
    size_t limit = mode && strcmp(mode, "verbose") == 0   ? g_trace_top_len
                   : mode && strcmp(mode, "compact") == 0 ? 5
                                                          : 12;
    if (limit > g_trace_top_len)
      limit = g_trace_top_len;
    fprintf(stderr,
            "[profile] mode=%s sort=%s function calls wall_inclusive_ns "
            "wall_exclusive_ns thread_cpu_ns thread_cpu_exclusive_ns "
            "allocator_calls sampled_rss_delta_bytes sampled_rss_peak_bytes\n",
            mode ? mode : "smart",
            mode && strcmp(mode, "verbose") == 0 ? "calls" : "exclusive_cpu");
    for (size_t i = 0; i < limit; ++i)
      fprintf(stderr,
              "[profile] %s %zu %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
              " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
              g_trace_tops[i].name, g_trace_tops[i].count,
              g_trace_tops[i].inclusive_ns, g_trace_tops[i].exclusive_ns,
              g_trace_tops[i].cpu_ns, g_trace_tops[i].exclusive_cpu_ns,
              g_trace_tops[i].allocations,
              g_trace_tops[i].sampled_rss_delta_bytes,
              g_trace_tops[i].sampled_rss_peak_bytes);
  }
  ny_trace_profile_unlock();
}

static size_t ny_trace_top_record(int64_t func) {
  const char *fn = NULL;
  size_t fnlen = 0;
  if (!ny_trace_string_view(func, &fn, &fnlen))
    return SIZE_MAX;
  if (fnlen >= sizeof(g_trace_tops[0].name))
    fnlen = sizeof(g_trace_tops[0].name) - 1;
  for (size_t i = 0; i < g_trace_top_len; ++i) {
    if (strncmp(g_trace_tops[i].name, fn, fnlen) == 0 &&
        g_trace_tops[i].name[fnlen] == '\0') {
      ++g_trace_tops[i].count;
      return i;
    }
  }
  if (g_trace_top_len < NY_TRACE_TOP_MAX) {
    ny_trace_top_t *e = &g_trace_tops[g_trace_top_len++];
    memcpy(e->name, fn, fnlen);
    e->name[fnlen] = '\0';
    e->count = 1;
    return g_trace_top_len - 1;
  }
  return SIZE_MAX;
}

int64_t rt_trace_count(int64_t func) {
  const char *name = NULL;
  size_t name_len = 0;
  if (!ny_trace_string_view(func, &name, &name_len))
    return rt_tag_v(0);
  ny_trace_profile_lock();
  for (size_t i = 0; i < g_trace_top_len; ++i) {
    if (strlen(g_trace_tops[i].name) == name_len &&
        memcmp(g_trace_tops[i].name, name, name_len) == 0) {
      int64_t result = rt_tag_v((int64_t)g_trace_tops[i].count);
      ny_trace_profile_unlock();
      return result;
    }
  }
  ny_trace_profile_unlock();
  return rt_tag_v(0);
}

static int64_t rt_trace_profile_metric(int64_t func, unsigned metric) {
  const char *name = NULL;
  size_t name_len = 0;
  if (!ny_trace_string_view(func, &name, &name_len))
    return rt_tag_v(0);
  int64_t result = 0;
  ny_trace_profile_lock();
  for (size_t i = 0; i < g_trace_top_len; ++i) {
    ny_trace_top_t *entry = &g_trace_tops[i];
    if (strlen(entry->name) != name_len ||
        memcmp(entry->name, name, name_len) != 0)
      continue;
    uint64_t value = metric == 0   ? entry->inclusive_ns
                     : metric == 1 ? entry->exclusive_ns
                     : metric == 3 ? entry->cpu_ns
                     : metric == 4 ? entry->exclusive_cpu_ns
                     : metric == 5 ? entry->sampled_rss_delta_bytes
                     : metric == 6 ? entry->sampled_rss_peak_bytes
                                   : entry->allocations;
    result = rt_tag_v((int64_t)(value > INT64_MAX ? INT64_MAX : value));
    break;
  }
  ny_trace_profile_unlock();
  return result;
}

int64_t rt_trace_inclusive_ns(int64_t func) {
  return rt_trace_profile_metric(func, 0);
}
int64_t rt_trace_exclusive_ns(int64_t func) {
  return rt_trace_profile_metric(func, 1);
}
int64_t rt_trace_cpu_ns(int64_t func) {
  return rt_trace_profile_metric(func, 3);
}
int64_t rt_trace_exclusive_cpu_ns(int64_t func) {
  return rt_trace_profile_metric(func, 4);
}
int64_t rt_trace_sampled_rss_delta(int64_t func) {
  return rt_trace_profile_metric(func, 5);
}
int64_t rt_trace_sampled_rss_peak(int64_t func) {
  return rt_trace_profile_metric(func, 6);
}
int64_t rt_trace_allocations(int64_t func) {
  return rt_trace_profile_metric(func, 2);
}

int64_t rt_trace_inclusive_ns_raw(int64_t func) {
  int64_t value = rt_trace_inclusive_ns(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_exclusive_ns_raw(int64_t func) {
  int64_t value = rt_trace_exclusive_ns(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_cpu_ns_raw(int64_t func) {
  int64_t value = rt_trace_cpu_ns(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_exclusive_cpu_ns_raw(int64_t func) {
  int64_t value = rt_trace_exclusive_cpu_ns(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_sampled_rss_delta_raw(int64_t func) {
  int64_t value = rt_trace_sampled_rss_delta(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_sampled_rss_peak_raw(int64_t func) {
  int64_t value = rt_trace_sampled_rss_peak(func);
  return is_int(value) ? rt_untag_v(value) : value;
}
int64_t rt_trace_allocations_raw(int64_t func) {
  int64_t value = rt_trace_allocations(func);
  return is_int(value) ? rt_untag_v(value) : value;
}

void rt_trace_profile_write(FILE *out) {
  if (!out)
    return;
  ny_trace_profile_lock();
  fprintf(out, "runtime.profile.format=nytrix-function-v2\n");
  for (size_t i = 0; i < g_trace_top_len; ++i)
    fprintf(
        out,
        "runtime.profile function=%s calls=%zu inclusive_ns=%" PRIu64
        " exclusive_ns=%" PRIu64 " cpu_ns=%" PRIu64 " exclusive_cpu_ns=%" PRIu64
        " allocations=%" PRIu64 " sampled_rss_delta_bytes=%" PRIu64
        " sampled_rss_peak_bytes=%" PRIu64 "\n",
        g_trace_tops[i].name, g_trace_tops[i].count,
        g_trace_tops[i].inclusive_ns, g_trace_tops[i].exclusive_ns,
        g_trace_tops[i].cpu_ns, g_trace_tops[i].exclusive_cpu_ns,
        g_trace_tops[i].allocations, g_trace_tops[i].sampled_rss_delta_bytes,
        g_trace_tops[i].sampled_rss_peak_bytes);
  ny_trace_profile_unlock();
}

int64_t rt_trace_count_raw(int64_t func) {
  int64_t value = rt_trace_count(func);
  return is_int(value) ? rt_untag_v(value) : value;
}

int64_t rt_trace_reset(void) {
  atomic_store_explicit(&g_trace_profile_active, 1, memory_order_relaxed);
  ny_trace_profile_lock();
  memset(g_trace_tops, 0, sizeof(g_trace_tops));
  g_trace_top_len = 0;
  for (size_t i = 0; i < g_cs_depth; ++i) {
    g_trace_profile_index[i] = SIZE_MAX;
    g_trace_profile_child_ns[i] = 0;
  }
  ny_trace_profile_unlock();
  return rt_tag_v(0);
}

int64_t rt_trace_reset_raw(void) { return rt_untag_v(rt_trace_reset()); }

static void trace_print_call(int64_t file, int64_t line, int64_t func,
                             size_t depth) {
  if (!g_trace_run_flush_registered) {
    atexit(trace_flush_run);
    atexit(ny_trace_top_summary);
    g_trace_run_flush_registered = true;
  }
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *c2 = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  g_trace_pending_kind = 0;
  g_trace_pending_depth = depth;
  g_trace_pending.len = 0;
  g_trace_pending_open = true;
  trace_str_addf(&g_trace_pending, "%s[trace]%s -> ", c1, rs);
  const char *fn = NULL;
  size_t fnlen = 0;
  if (ny_trace_string_view(func, &fn, &fnlen)) {
    trace_str_addf(&g_trace_pending, "%s%.*s%s", fnc, (int)fnlen, fn, rs);
  } else {
    trace_str_addf(&g_trace_pending, "<anon>");
  }
  const char *fname = NULL;
  size_t flen = 0;
  if (ny_trace_string_view(file, &fname, &flen)) {
    long l = (long)(is_int(line) ? rt_untag_v(line) : line);
    trace_str_addf(&g_trace_pending, " %s@ %.*s:%ld%s", c2, (int)flen, fname, l,
                   rs);
  }
  trace_commit_pending();
}

static void trace_print_return_prefix(int64_t func, size_t depth) {
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *fnc = color_mode ? clr(NY_CLR_YELLOW) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  g_trace_pending_kind = 1;
  g_trace_pending_depth = depth;
  g_trace_pending.len = 0;
  g_trace_pending_open = true;
  trace_str_addf(&g_trace_pending, "%s[trace]%s <- ", c1, rs);
  const char *fn = NULL;
  size_t fnlen = 0;
  if (ny_trace_string_view(func, &fn, &fnlen)) {
    trace_str_addf(&g_trace_pending, "%s%.*s%s", fnc, (int)fnlen, fn, rs);
  } else {
    trace_str_addf(&g_trace_pending, "<anon>");
  }
}

static void trace_print_return_raw_suffix(const char *value, size_t len) {
  if (value && len > 0)
    trace_str_addf(&g_trace_pending, " = %.*s", (int)len, value);
  trace_commit_pending();
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
  /*
   * Defer callbacks arrive as raw machine-code addresses.  Function symbols
   * are not guaranteed to have the low tag bit clear, so the language-value
   * `is_ptr` predicate can silently discard valid callbacks at odd addresses.
   * Validate this API's raw function-pointer contract directly.
   */
  fn = (uintptr_t)fn > NY_VALUE_PTR_MIN_ADDR && !NY_DYNAMIC_CALLABLE_IS(fn)
           ? rt_fix_fn_ptr(fn)
           : 0;
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

int64_t rt_defer_len(void) { return (int64_t)g_defer_stack.len; }

int rt_defer_pop_raw(int64_t *fn_out, int64_t *env_out) {
  if (g_defer_stack.len == 0)
    return 0;
  defer_t d = g_defer_stack.data[--g_defer_stack.len];
  if (fn_out)
    *fn_out = d.fn;
  if (env_out)
    *env_out = d.env;
  return 1;
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

/*
 * The NYIR VM's language-panic protocol records the payload of a compiled
 * rt_panic raise without executing the runtime's longjmp (the interpreter
 * unwinds to the enclosing try instead).
 */
void rt_set_panic_val(int64_t value) { g_panic_value = value; }

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
  /*
   * Counters are an observability primitive, not a rendering side effect:
   * record every entered frame even when call-line output is disabled or
   * filtered.
   */
  size_t profile_index = SIZE_MAX;
  ny_trace_profile_lock();
  profile_index = ny_trace_top_record(func);
  ny_trace_profile_unlock();
  trace_record(file, line, 1, func);

  if (g_cs_depth < CALL_STACK_MAX) {
    g_cs_files[g_cs_depth] = file;
    g_cs_lines[g_cs_depth] = line;
    g_cs_funcs[g_cs_depth] = func;
    g_trace_arg_counts[g_cs_depth] = 0;
    bool profile_enabled = ny_trace_profile_enabled();
    g_trace_profile_index[g_cs_depth] =
        profile_enabled ? profile_index : SIZE_MAX;
    g_trace_profile_start[g_cs_depth] =
        profile_enabled ? ny_trace_profile_now() : 0;
    g_trace_profile_cpu_start[g_cs_depth] =
        profile_enabled ? ny_trace_profile_cpu_now() : 0;
    g_trace_profile_rss_start[g_cs_depth] =
        profile_enabled && (++g_trace_profile_rss_sample_counter % 256) == 0
            ? ny_trace_profile_rss_bytes()
            : 0;
    g_trace_profile_child_ns[g_cs_depth] = 0;
    g_trace_profile_child_cpu_ns[g_cs_depth] = 0;
    if (profile_enabled) {
      extern atomic_uint_fast64_t g_ny_alloc_count;
      extern atomic_uint_fast64_t g_ny_gc_alloc_count;
      g_trace_profile_alloc_start[g_cs_depth] =
          atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
      g_trace_profile_gc_alloc_start[g_cs_depth] =
          atomic_load_explicit(&g_ny_gc_alloc_count, memory_order_relaxed);
    }
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
  if (g_cs_depth > 0 && g_trace_profile_index[g_cs_depth - 1] != SIZE_MAX) {
    size_t frame = g_cs_depth - 1;
    uint64_t end = ny_trace_profile_now();
    uint64_t cpu_end = ny_trace_profile_cpu_now();
    uint64_t rss_end =
        g_trace_profile_rss_start[frame] ? ny_trace_profile_rss_bytes() : 0;
    uint64_t start = g_trace_profile_start[frame];
    uint64_t elapsed = end >= start ? end - start : 0;
    uint64_t child = g_trace_profile_child_ns[frame];
    uint64_t exclusive = elapsed >= child ? elapsed - child : 0;
    uint64_t cpu_start = g_trace_profile_cpu_start[frame];
    uint64_t cpu_elapsed = cpu_end >= cpu_start ? cpu_end - cpu_start : 0;
    uint64_t cpu_child = g_trace_profile_child_cpu_ns[frame];
    uint64_t cpu_exclusive =
        cpu_elapsed >= cpu_child ? cpu_elapsed - cpu_child : 0;
    extern atomic_uint_fast64_t g_ny_alloc_count;
    extern atomic_uint_fast64_t g_ny_gc_alloc_count;
    uint64_t alloc_end =
        atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
    uint64_t gc_alloc_end =
        atomic_load_explicit(&g_ny_gc_alloc_count, memory_order_relaxed);
    uint64_t alloc_start = g_trace_profile_alloc_start[frame];
    uint64_t gc_alloc_start = g_trace_profile_gc_alloc_start[frame];
    size_t index = g_trace_profile_index[frame];
    if (index < NY_TRACE_TOP_MAX) {
      ny_trace_profile_lock();
      if (index < g_trace_top_len) {
        g_trace_tops[index].inclusive_ns += elapsed;
        g_trace_tops[index].exclusive_ns += exclusive;
        g_trace_tops[index].cpu_ns += cpu_elapsed;
        g_trace_tops[index].exclusive_cpu_ns += cpu_exclusive;
        uint64_t rss_start = g_trace_profile_rss_start[frame];
        uint64_t rss_delta = rss_end > rss_start ? rss_end - rss_start : 0;
        g_trace_tops[index].sampled_rss_delta_bytes += rss_delta;
        if (rss_delta > g_trace_tops[index].sampled_rss_peak_bytes)
          g_trace_tops[index].sampled_rss_peak_bytes = rss_delta;
        g_trace_tops[index].allocations +=
            (alloc_end >= alloc_start ? alloc_end - alloc_start : 0) +
            (gc_alloc_end >= gc_alloc_start ? gc_alloc_end - gc_alloc_start
                                            : 0);
      }
      ny_trace_profile_unlock();
    }
    if (frame > 0) {
      g_trace_profile_child_ns[frame - 1] += elapsed;
      g_trace_profile_child_cpu_ns[frame - 1] += cpu_elapsed;
    }
  }
  /*
   * A repeated call record must be closed at the matching return even
   * when value tracing suppresses the ordinary return line.  Otherwise
   * the held repeat is printed after a later, unrelated call and the
   * trace nesting is misleading.
   */
  if (trace_calls_enabled())
    trace_flush_run();
  if (trace_calls_enabled() && !trace_values_enabled() &&
      trace_should_print_func(func)) {
    trace_print_return_prefix(func, depth);
    trace_commit_pending();
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
  trace_commit_pending();
  return rt_tag_v(0);
}

/*
 * Raw NYIR adapters for the zero-argument trace diagnostics: the native
 * path compares their results against plain integers while the VM consumes
 * the tagged variants above.
 */
int64_t rt_trace_ret_void_raw(void) {
  (void)rt_trace_ret_void();
  return 0;
}

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
  /*
   * The builtin signature is untyped, so the native call boundary delivers
   * the tagged scalar; decode once so identity returns the plain value.
   */
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

/*
 * Native NYIR scalar arguments are already raw i64 words. Keep raw entry
 * points separate from the interpreter-facing tagged helpers above so odd
 * values are never mistaken for tagged immediates and shifted a second time.
 */
int64_t rt_trace_ret_i64_raw(int64_t v) {
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

int64_t rt_trace_ret_u64_raw(int64_t v) {
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
  /*
   * Untyped builtin signature: the native boundary tags scalar literals, so
   * decode once (a boxed integer zero would read as 1, not NULL).
   */
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

static bool trace_arg_enabled(size_t *index, size_t *depth) {
  if (g_trace_suspended || !trace_values_enabled() || g_cs_depth == 0)
    return false;
  int64_t func = g_cs_funcs[g_cs_depth - 1];
  if (!trace_should_print_func(func))
    return false;
  if (index)
    *index = g_trace_arg_counts[g_cs_depth - 1]++;
  if (depth)
    *depth = g_cs_depth - 1;
  return true;
}

static void trace_print_arg_prefix(size_t depth, size_t index) {
  const char *c1 = color_mode ? clr(NY_CLR_CYAN) : "";
  const char *c2 = color_mode ? clr(NY_CLR_GRAY) : "";
  const char *rs = color_mode ? clr(NY_CLR_RESET) : "";
  trace_print_indent(depth);
  fprintf(stderr, "%s[trace]%s arg[%zu]%s = ", c1, rs, index, c2);
}

static void trace_print_arg_tagged_value(int64_t value) {
  extern int64_t rt_to_str(int64_t v);
  if (is_v_str(value)) {
    const char *p = (const char *)(uintptr_t)value;
    fprintf(stderr, "%.*s", (int)rt_tagged_str_len(value), p);
    fputc('\n', stderr);
    return;
  }
  if (rt_native_is_str(value)) {
    const char *p = (const char *)(uintptr_t)value;
    fprintf(stderr, "%.*s", (int)rt_cstr_len(value), p);
    fputc('\n', stderr);
    return;
  }
  int64_t str = rt_to_str(value);
  if (is_v_str(str)) {
    const char *p = (const char *)(uintptr_t)str;
    fprintf(stderr, "%.*s", (int)rt_tagged_str_len(str), p);
  } else
    fputs("<value>", stderr);
  fputc('\n', stderr);
}

int64_t rt_trace_arg_tagged(int64_t value) {
  size_t index = 0, depth = 0;
  if (trace_arg_enabled(&index, &depth)) {
    trace_print_arg_prefix(depth, index);
    trace_print_arg_tagged_value(value);
  }
  return value;
}

int64_t rt_trace_arg_i64(int64_t value) {
  size_t index = 0, depth = 0;
  if (trace_arg_enabled(&index, &depth)) {
    trace_print_arg_prefix(depth, index);
    fprintf(stderr, "%" PRId64 "\n", value);
  }
  return value;
}

int64_t rt_trace_arg_f64_bits(int64_t bits) {
  size_t index = 0, depth = 0;
  if (trace_arg_enabled(&index, &depth)) {
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    trace_print_arg_prefix(depth, index);
    fprintf(stderr, "%.17g\n", value);
  }
  return bits;
}

int64_t rt_trace_arg_f64(double value) {
  int64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  return rt_trace_arg_f64_bits(bits);
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
int64_t rt_box_i64(int64_t raw) { return ny_value_box_i64(raw); }
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
    if (rt_tbuf_header_valid(hdr)) {
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
  /*
   * Native dictionaries and native tbuf-backed sequences are first-class
   * Nytrix values even though they do not use the legacy traced-heap header.
   * Reflection/printing gates structural formatting on __is_ny_obj; treating
   * these handles as foreign pointers sends them to __to_str as if they were
   * C strings, which can scan arbitrary container payload memory indefinitely.
   */
  if (ny_native_dict_ptr(v) != NULL)
    return NY_IMM_TRUE;
  if (v && rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                     RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr))
      return NY_IMM_TRUE;
  }
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
  /*
   * rt_native_is_str() already performs the guarded headerless-C-string
   * probe after excluding native tbufs, dicts, floats, and heap objects.
   * Treating every remaining non-heap pointer as a string misclassified
   * native list handles as strings in the REPL/JIT path.
   */
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
   * The public primitive is also used through the generated untyped wrapper
   * `runtime_tag_raw(x)`.  In that form a literal arrives as a canonical
   * tagged Ny string rather than the raw C-string pointer used by the direct
   * intrinsic.  Decode both representations at this ABI boundary.
   */
  if (is_v_str(name))
    return rt_runtime_tag_raw((const char *)(uintptr_t)name,
                              rt_tagged_str_len(name));
  if (rt_native_is_str(name))
    return rt_runtime_tag_raw((const char *)(uintptr_t)name, rt_cstr_len(name));
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
  /*
   * Typed native containers may cross a dynamic return/global boundary in
   * their compact native-handle encoding. Resolve only validated tbuf handles
   * before classifying immediates or managed heap tags.
   */
  v = rt_tbuf_resolve_handle(v);
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
    if (rt_tbuf_header_valid(hdr)) {
      int64_t actual = rt_tbuf_word_is_tuple(hdr[3]) ? TAG_TUPLE : TAG_LIST;
      return (want == actual) ? NY_IMM_TRUE : NY_IMM_FALSE;
    }
  }
  if (want == TAG_DICT || want == TAG_SET) {
    ny_native_dict_t *dict = ny_native_dict_ptr(v);
    if (dict)
      return want == TAG_DICT ? NY_IMM_TRUE : NY_IMM_FALSE;
    if (want == TAG_SET && rt_set_layout(v))
      return NY_IMM_TRUE;
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
  /*
   * `__has_tag` is a language predicate, so both native and interpreter
   * lowering must return the canonical tagged Ny boolean.  The shared
   * predicate already accepts raw and boxed tag arguments and knows about
   * managed objects, native buffers, and native dictionaries.
   * Runtime object tags occupy the declared 100..150 range.  Preserve a raw
   * tag in that range; only the boxed form (for example 203 for tag 101) is
   * unboxed here.  Testing `is_int` alone is insufficient because raw odd
   * tags deliberately share the integer low-bit pattern.
   */
  if ((tag < TAG_LIST || tag > TAG_KWARG) && is_int(tag))
    tag = rt_untag_v(tag);
  return rt_has_tag(v, rt_tag_v(tag));
}
int64_t rt_value_tag(int64_t v) {
  if (v == 0)
    return 0;
  /*
   * Dynamic callable IDs are valid language values stored in descriptor
   * buffers. Classify them as closure-like so indexed reads preserve the ID
   * instead of treating the opaque word as scalar tag 3 and tagging it as an
   * integer on the way back out.
   */
  if (NY_DYNAMIC_CALLABLE_IS(v))
    return TAG_CLOSURE;
  /*
   * Async tasks are opaque runtime handles, not strings or scalar immediates.
   * Their addresses can satisfy both heuristic predicates.
   */
  if (rt_async_is_handle(v))
    return TAG_CLOSURE;
  /*
   * Live managed-object provenance is stronger than the foreign header
   * probes used for native buffers, dictionaries, and static strings.
   */
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    if (tag >= 100 && tag <= 255)
      return tag == TAG_STR_CONST ? TAG_STR : tag;
  }
  /*
   * Raw native handles may be odd and therefore satisfy the scalar low-bit
   * test.  Resolve authoritative native representations before classifying
   * immediates, so nested tbufs crossing an any callback remain containers.
   */
  if (NY_NATIVE_IS(v) && NY_NATIVE_DECODE(v) != NULL) {
    int64_t decoded = (int64_t)(uintptr_t)NY_NATIVE_DECODE(v);
    if (rt_magic_tbuf_elem_size(decoded) > 0) {
      int64_t *hdr = (int64_t *)(uintptr_t)(decoded - RT_NATIVE_TBUF_HEADER);
      return rt_tbuf_word_is_tuple(hdr[3]) ? TAG_TUPLE : TAG_LIST;
    }
  }
  if (rt_magic_tbuf_elem_size(v) > 0) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    return rt_tbuf_word_is_tuple(hdr[3]) ? TAG_TUPLE : TAG_LIST;
  }
  if (rt_native_is_str(v))
    return TAG_STR;
  if (is_int(v))
    return 1;
  if (ny_native_dict_ptr(v) != NULL)
    return TAG_DICT;
  if (rt_set_layout(v))
    return TAG_SET;
  const int64_t *tbuf_header = NULL;
  if (rt_tbuf_check(v, &tbuf_header) == RT_TBUF_OK)
    return rt_tbuf_word_is_tuple(tbuf_header[3]) ? TAG_TUPLE : TAG_LIST;
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
  if (NY_DYNAMIC_CALLABLE_IS(value))
    return TAG_CLOSURE;
  if (rt_native_is_str(value) || is_v_str(value))
    return TAG_STR;
  if (rt_magic_tbuf_elem_size(value) > 0) {
    int64_t *hdr = (int64_t *)((uintptr_t)value - RT_NATIVE_TBUF_HEADER);
    return rt_tbuf_word_is_tuple(hdr[3]) ? TAG_TUPLE : TAG_LIST;
  }
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
  if (rt_is_bool_imm(value))
    return (int64_t)(uintptr_t)"bool";
  switch (rt_value_tag(value)) {
  case 0:
    name = "nil";
    break;
  /*
   * `rt_value_tag` and the public runtime-tag table use 1 for the integer
   * kind.  Native raw scalar slots may also contain 3, but that is the
   * tagged value for integer zero and is classified by `is_int` above.
   */
  case 1:
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
  /*
   * A registered typed-buffer handle is never a string.  Check the exact
   * handle before generic pointer classification can inspect its payload.
   */
  if (rt_tbuf_known_handle((uintptr_t)v))
    return 0;
  if (NY_NATIVE_IS(v))
    /*
     * Encoded native handles carry typed buffers, never strings; the shifted
     * payload would otherwise be probed as a stray C-string address below.
     */
    return 0;
  if (is_v_flt(v))
    return 0;
  if (is_v_str(v))
    return 1;
  /*
   * Only allocator-owned heap objects have an inspectable runtime tag.
   */
  int64_t heap_v = rt_heap_object_ptr(v);
  if (heap_v) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)heap_v - 8);
    return (tag == TAG_STR || tag == TAG_STR_CONST) ? 1 : 0;
  }
  /*
   * Raw pointers require an explicit conversion at a typed FFI/string edge.
   */
  return 0;
}

/*
 * Kept for already-emitted native modules.  New lowering emits rt_is_str.
 */
int64_t rt_native_is_str(int64_t v) { return rt_is_str(v); }

int64_t rt_native_is_dict(int64_t v) {
  return ny_native_dict_ptr(v) != NULL ? NY_IMM_TRUE : NY_IMM_FALSE;
}

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
  if (rt_set_layout(v))
    return rt_tag_v(TAG_SET);
  if (rt_header_readable_cached((uintptr_t)v - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
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

/*
 * The primitive exported by core.primitives has an explicitly raw
 * machine ABI.  Its length is unboxed at the native boundary; odd raw
 * lengths must never be guessed to be tagged values.
 */
int64_t rt_init_str_raw(int64_t p, int64_t n) {
  if (NY_NATIVE_IS(p))
    p = (int64_t)(uintptr_t)NY_NATIVE_DECODE(p);
  if (!p || n < 0)
    return p;
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

/*
 * The historical `__copy_mem` entry receives a tagged dynamic length from the
 * interpreter ABI.  Raw native/stdlib wrappers need an explicit companion:
 * odd raw lengths are indistinguishable from tagged integers if a callee
 * guesses from the low bit.  Keep the two boundaries separate so a 141-byte
 * PNG chunk cannot be mistaken for the tagged value 70.
 */
int64_t rt_copy_mem_raw(int64_t dst, int64_t src, int64_t n) {
  if (n <= 0)
    return dst;
  memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, (size_t)n);
  return dst;
}

int64_t rt_copy_mem(int64_t dst, int64_t src, int64_t n) {
  if (is_int(n))
    n >>= 1;
  return rt_copy_mem_raw(dst, src, n);
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

int64_t rt_runtime_alloc_count(void) {
  extern atomic_uint_fast64_t g_ny_alloc_count;
  extern atomic_uint_fast64_t g_ny_gc_alloc_count;
  uint64_t count =
      atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
  uint64_t gc_count =
      atomic_load_explicit(&g_ny_gc_alloc_count, memory_order_relaxed);
  count = UINT64_MAX - count < gc_count ? UINT64_MAX : count + gc_count;
  return rt_tag_v((int64_t)(count > INT64_MAX ? INT64_MAX : count));
}

int64_t rt_runtime_alloc_count_raw(void) {
  return rt_untag_v(rt_runtime_alloc_count());
}

int64_t rt_runtime_realloc_count(void) {
  extern atomic_uint_fast64_t g_ny_realloc_count;
  return rt_tag_v(
      (int64_t)atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed));
}

int64_t rt_runtime_realloc_count_raw(void) {
  return rt_untag_v(rt_runtime_realloc_count());
}

int64_t rt_request_native_oracle(void) { return rt_tag_v(0); }
int64_t rt_request_native_oracle_raw(void) { return 0; }
int64_t rt_request_tv_checks(int64_t trials) {
  (void)trials;
  return rt_tag_v(0);
}
int64_t rt_request_tv_checks_raw(int64_t trials) {
  (void)trials;
  return 0;
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
int64_t rt_result_unwrap_or_any(int64_t v, int64_t fallback) {
  return is_v_ok(v) ? rt_unwrap(v) : fallback;
}
int64_t rt_result_unwrap_or_raw(int64_t v, int64_t fallback) {
  if (is_v_ok(v)) {
    return rt_any_to_i64(rt_unwrap(v));
  }
  return fallback;
}
int64_t rt_varargs_new(int64_t n_v) {
  int64_t n = is_int(n_v) ? (n_v >> 1) : n_v;
  if (n < 0)
    n = 0;
  if (n > (INT64_MAX - 16) / 8)
    return 0;
  int64_t out = rt_malloc(rt_tag_v(16 + n * 8));
  if (!out)
    return 0;
  *(int64_t *)((char *)(uintptr_t)out - 8) = TAG_LIST;
  *(int64_t *)((char *)(uintptr_t)out + 0) = rt_tag_v(0);
  memset((char *)(uintptr_t)out + 8, 0, (size_t)n * 8 + 8);
  return out;
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

/*
 * Compiler-only allocation for counted-fill loops. Those loops write every
 * element through raw i64 stores before the list can be observed, so clearing
 * the payload is unnecessary. Keep ordinary sized lists zero-initialized.
 */
int64_t rt_list_new_sized_uninit(int64_t n) {
  if (n < 0)
    n = 0;
  return rt_tbuf_new_impl(n, 8, 1, false);
}

int64_t rt_list_as_tuple(int64_t lst) {
  if (rt_tbuf_known_handle((uintptr_t)lst) &&
      rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      hdr[3] = rt_tbuf_capacity_word(hdr[3]) | RT_TBUF_TUPLE_FLAG;
      return lst;
    }
  }
  if (!is_ptr(lst) || !is_heap_ptr(lst))
    return lst;
  int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
  if (tag == TAG_LIST || tag == TAG_TUPLE)
    *(int64_t *)((char *)(uintptr_t)lst - 8) = TAG_TUPLE;
  return lst;
}

int64_t rt_append(int64_t lst, int64_t val) {
  lst = rt_tbuf_resolve_handle(lst);
  if (rt_tbuf_known_handle((uintptr_t)lst) &&
      rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *base = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(base)) {
      /*
       * `__append` is the dynamic/any append boundary.  Its scalar payload
       * arrives in the tagged VM representation, even when the destination
       * still has the compact elem-8 layout.  Route every native buffer
       * through the tagged adapter so the adapter can decode that payload
       * once and upgrade elem-8 storage to descriptors when necessary.
       * Typed/raw append callers use the explicit tbuf raw entry points.
       */
      return rt_tbuf_append_tagged(lst, val,
                                   rt_native_is_str(val) || is_v_str(val));
    }
  }
  int64_t managed_lst = rt_heap_object_ptr(lst);
  if (!managed_lst ||
      *(int64_t *)((char *)(uintptr_t)managed_lst - 8) != TAG_LIST) {
    rt_fail(rt_alloc_string("err.type"),
            rt_alloc_string(is_int(lst) ? "append expects list, got int"
                                        : "append expects list"));
    return lst;
  }
  lst = managed_lst;
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
    if (rt_tbuf_header_valid(hdr))
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
    if (rt_tbuf_header_valid(hdr)) {
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
  /*
   * Raw i64 keys crossing into a managed dictionary use the BigInt
   * representation when they exceed the immediate range. Keep their hash in
   * the same numeric domain as tagged small integers; rt_eq performs the
   * corresponding value comparison during probing.
   */
  if (rt_value_tag(key) == TAG_BIGINT)
    return (uint64_t)rt_bigint_to_i64_raw(key);
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

static int64_t rt_dict_find_off_table(int64_t t, int64_t cap, int64_t key,
                                      bool tagged) {
  if (cap <= 0 || !t)
    return -1;
  uint64_t mask = (uint64_t)(cap - 1);
  uint64_t idx = rt_dict_hash_raw(key) & mask;
  int64_t first_tomb = -1;
  for (int64_t i = 0; i < cap; i++) {
    int64_t off = (int64_t)idx * 24;
    int64_t state = *(int64_t *)((char *)(uintptr_t)t + off + 16);
    if (tagged && state)
      state = rt_untag_v(state);
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
  int64_t off = rt_dict_find_off_table(t, cap, key, false);
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
  /*
   * Dynamic callers may carry a native tbuf in its compact handle form.
   * Resolve that representation before selecting the sequence ABI; checking
   * the encoded word directly falls through to the legacy list path and
   * loses nested values only when the handle crosses an `any` boundary.
   */
  lst = rt_tbuf_resolve_handle(lst);
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
    if (rt_tbuf_header_valid(hdr))
      return rt_tbuf_get(lst, i_v, 0);
  }
  return _rt_load_item_fast(lst, i_v);
}

int64_t rt_store_item_fast(int64_t lst, int64_t i_v, int64_t val) {
  if (!lst)
    return 0;
  /*
   * A previous dynamic store may have widened this list and left a
   * forwarding header at the original handle. Resolve it before inspecting
   * the element width or writing the next literal slot.
   */
  lst = rt_tbuf_resolve_handle(lst);
  /*
   * This public helper is the language-value ABI used by the interpreter and
   * legacy LLVM path: its index is a tagged Ny integer.  Native lowering uses
   * the explicit *_raw entry point below, where odd raw positions remain
   * valid indices.
   */
  int64_t raw_i = is_int(i_v) ? rt_untag_v(i_v) : i_v;
  int64_t raw_val = is_int(val) ? rt_untag_v(val) : val;
  int64_t tagged_val =
      (is_int(val) || rt_is_bool_imm(val) || is_v_flt(val) || is_v_str(val) ||
       rt_native_is_str(val) || rt_heap_object_ptr(val) || NY_NATIVE_IS(val) ||
       !ny_small_int_fits_i64(val))
          ? val
          : rt_tag_v(val);
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    /*
     * Legacy LLVM builds list literals through this language-value entry
     * point.  An elem-8 list has no per-slot type metadata, so a pointer-like
     * value (especially a string passed through an `any` parameter) cannot
     * safely share that representation with its neighboring raw integers.
     * Promote before storing the first dynamic value; the descriptor setter
     * records its runtime tag and the promotion reclassifies prior scalars.
     */
    bool dynamic_element = rt_is_bool_imm(val) || rt_native_is_str(val) ||
                           is_v_str(val) || is_v_flt(val) ||
                           rt_heap_object_ptr(val) != 0;
    if (rt_tbuf_header_valid(hdr) && hdr[2] == 8 && dynamic_element) {
      lst = rt_tbuf_promote_descriptor(lst);
      hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    }
    if (rt_tbuf_header_valid(hdr) && hdr[2] >= 24)
      return rt_tbuf_set_tagged(lst, raw_i, val) ? tagged_val : 0;
    if (rt_tbuf_header_valid(hdr))
      return rt_tbuf_set_i64_raw(lst, raw_i, raw_val) ? tagged_val : 0;
  }
  return _rt_store_item_fast(lst, raw_i, tagged_val);
}

/*
 * The private `__store_item_fast` primitive has a raw NYIR contract: its
 * index and scalar payload are already unboxed.  Keep that contract separate
 * from the public/dynamic store above; testing `is_int` cannot distinguish a
 * raw odd payload such as 1 from tagged language integer zero.
 */
int64_t rt_store_item_fast_raw(int64_t lst, int64_t index, int64_t value) {
  if (!lst)
    return 0;
  lst = rt_tbuf_resolve_handle(lst);
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      bool dynamic_element = rt_is_bool_imm(value) || rt_native_is_str(value) ||
                             is_v_str(value) || is_v_flt(value) ||
                             rt_heap_object_ptr(value) != 0 ||
                             NY_NATIVE_IS(value);
      if (is_v_flt(value)) {
        int64_t bits = rt_f64_bits(rt_flt_unbox_double(value));
        return rt_tbuf_set_f64_bits(lst, index, bits) ? value : 0;
      }
      if (hdr[2] == 8 && dynamic_element) {
        lst = rt_tbuf_promote_descriptor(lst);
        hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
      }
      if (hdr[2] >= 24) {
        int64_t tagged = (rt_is_bool_imm(value) || rt_native_is_str(value) ||
                          is_v_str(value) || rt_heap_object_ptr(value) ||
                          NY_NATIVE_IS(value))
                             ? value
                             : rt_tag_v(value);
        return rt_tbuf_set_tagged(lst, index, tagged) ? value : 0;
      }
      return rt_tbuf_set_i64_raw(lst, index, value) ? value : 0;
    }
  }
  return _rt_store_item_fast(lst, index, value);
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
    if (rt_tbuf_header_valid(hdr)) {
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
  if (rt_magic_tbuf_elem_size(v) > 0) {
    int64_t *hdr = (int64_t *)((uintptr_t)v - RT_NATIVE_TBUF_HEADER);
    return rt_tbuf_word_is_tuple(hdr[3]) ? TAG_TUPLE : TAG_LIST;
  }
  if (!is_ptr(v) || !is_heap_ptr(v))
    return 0;
  return *(int64_t *)((char *)(uintptr_t)v - 8);
}

static int64_t rt_tagged_raw_i64(int64_t v) { return is_int(v) ? (v >> 1) : v; }

static int64_t rt_range_new_impl(int64_t start, int64_t stop, int64_t step) {
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

int64_t rt_range_new(int64_t start_v, int64_t stop_v, int64_t step_v) {
  /*
   * This entry is the language-value ABI: decode each operand independently.
   * Native callers use rt_range_new_raw, so representation cannot be guessed
   * from the parity of an unrelated operand.
   */
  int64_t start = is_int(start_v) ? rt_untag_v(start_v) : start_v;
  int64_t stop = is_int(stop_v) ? rt_untag_v(stop_v) : stop_v;
  int64_t step = is_int(step_v) ? rt_untag_v(step_v) : step_v;
  return rt_range_new_impl(start, stop, step);
}

int64_t rt_range_new_raw(int64_t start, int64_t stop, int64_t step) {
  return rt_range_new_impl(start, stop, step);
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
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return (int64_t)(uintptr_t)"";
  }
  const char *s = (const char *)(uintptr_t)str_v;
  size_t len = strlen(s);
  int64_t idx = idx_v;
  if (idx < 0 && (uint64_t)(-idx) <= len)
    idx += (int64_t)len;
  if (idx < 0 || (size_t)idx >= len) {
    rt_fail(rt_alloc_string("err.index"),
            rt_alloc_string("index_read out of range"));
    return (int64_t)(uintptr_t)"";
  }
  unsigned char c = (unsigned char)s[idx];
  return (int64_t)(uintptr_t)g_single_char_table[c];
}

size_t rt_utf8_valid_width(const unsigned char *s, size_t remaining) {
  if (!s || remaining == 0)
    return 0;
  unsigned char a = s[0];
  if (a <= 0x7f)
    return 1;
  if (a >= 0xc2 && a <= 0xdf)
    return remaining >= 2 && (s[1] & 0xc0) == 0x80 ? 2 : 0;
  if (remaining >= 3 && a == 0xe0)
    return s[1] >= 0xa0 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 3 && ((a >= 0xe1 && a <= 0xec) || (a >= 0xee && a <= 0xef)))
    return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 3 && a == 0xed)
    return s[1] >= 0x80 && s[1] <= 0x9f && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 4 && a == 0xf0)
    return s[1] >= 0x90 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  if (remaining >= 4 && a >= 0xf1 && a <= 0xf3)
    return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  if (remaining >= 4 && a == 0xf4)
    return s[1] >= 0x80 && s[1] <= 0x8f && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  return 0;
}

/*
 * String slicing treats an invalid byte as one replacement-width character,
 * matching core.str.utf8_len's recovery policy.
 */
static size_t rt_utf8_width(const unsigned char *s, size_t remaining) {
  size_t width = rt_utf8_valid_width(s, remaining);
  return width ? width : 1;
}

int64_t rt_cstr_slice(int64_t str_ptr, int64_t start, int64_t stop) {
  if (!str_ptr)
    return rt_alloc_string_len("", 0);
  const char *src = (const char *)(uintptr_t)str_ptr;
  size_t bytes = is_v_str(str_ptr) ? rt_tagged_str_len(str_ptr) : strlen(src);
  const unsigned char *raw = (const unsigned char *)src;
  int64_t count = 0;
  for (size_t pos = 0; pos < bytes;) {
    size_t width = rt_utf8_width(raw + pos, bytes - pos);
    pos += width <= bytes - pos ? width : 1;
    count++;
  }
  if (start < 0)
    start += count;
  if (stop < 0)
    stop += count;
  if (start < 0)
    start = 0;
  if (stop < 0)
    stop = 0;
  if (start > count)
    start = count;
  if (stop > count)
    stop = count;
  if (start >= stop)
    return rt_alloc_string_len("", 0);

  size_t begin_byte = 0, end_byte = 0;
  int64_t cp = 0;
  for (size_t pos = 0; pos < bytes && cp < stop;) {
    if (cp == start)
      begin_byte = pos;
    size_t width = rt_utf8_width(raw + pos, bytes - pos);
    pos += width <= bytes - pos ? width : 1;
    cp++;
    if (cp == stop)
      end_byte = pos;
  }
  if (end_byte < begin_byte)
    return rt_alloc_string_len("", 0);
  return rt_alloc_string_len(src + begin_byte, end_byte - begin_byte);
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

static int64_t rt_range_count_raw(int64_t start, int64_t stop, int64_t step) {
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
  int64_t n = rt_range_count_raw(start, stop, step);
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
  /*
   * Keep list predicates, length reads, and indexed reads consistent for
   * encoded typed-list values returned through a dynamic boundary.
   */
  lst = rt_tbuf_resolve_handle(lst);
  if (lst && rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                       RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr))
      return rt_tag_v(hdr[1]);
  }
  if (!is_ptr(lst))
    return 0;
  if (is_heap_ptr(lst)) {
    int64_t tag = *(int64_t *)((char *)(uintptr_t)lst - 8);
    if (tag == TAG_LIST || tag == TAG_TUPLE) {
      int64_t len_v = *(int64_t *)((char *)(uintptr_t)lst + 0);
      return is_int(len_v) ? len_v : rt_tag_v(len_v);
    }
  }
  int64_t tagged = *(int64_t *)((char *)(uintptr_t)lst + 0);
  return (tagged & 1) ? tagged : rt_tag_v(tagged);
}

int64_t rt_list_set_len_raw(int64_t lst, int64_t n) {
  if (!lst)
    return 0;
  /*
   * Typed-buffer values use the raw pointer ABI and need not carry the
   * managed-value pointer tag. Resolve and validate them through the handle
   * registry before applying the managed-list pointer check below.
   */
  lst = rt_tbuf_resolve_handle(lst);
  const int64_t *tbuf_header = NULL;
  if (rt_tbuf_check(lst, &tbuf_header) == RT_TBUF_OK) {
    ((int64_t *)tbuf_header)[1] = n < 0 ? 0 : n;
    return lst;
  }
  if (!is_ptr(lst))
    return 0;
  /*
   * Legacy managed lists store a tagged length.
   */
  *(int64_t *)((char *)(uintptr_t)lst + 0) = rt_tag_v(n < 0 ? 0 : n);
  return lst;
}

int64_t rt_list_set_len(int64_t lst, int64_t n) {
  lst = rt_tbuf_resolve_handle(lst);
  if (lst && rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                       RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      hdr[1] = is_int(n) ? rt_untag_v(n) : n;
      return lst;
    }
  }
  if (!is_ptr(lst))
    return 0;
  if (rt_header_readable_cached((uintptr_t)lst - RT_NATIVE_TBUF_HEADER,
                                RT_NATIVE_TBUF_HEADER)) {
    int64_t *hdr = (int64_t *)((uintptr_t)lst - RT_NATIVE_TBUF_HEADER);
    if (rt_tbuf_header_valid(hdr)) {
      /*
       * The public helper receives a language integer, while the native
       * tbuf header deliberately stores its count as a raw machine value.
       * Keeping the conversion at this ABI boundary prevents list.len from
       * tagging an already-tagged count a second time.
       */
      hdr[1] = is_int(n) ? rt_untag_v(n) : n;
      return lst;
    }
  }
  /*
   * Managed lists store their length in the language integer ABI.  Calls from
   * generated Nytrix code already pass a tagged integer, while a few native
   * callers may pass a raw count.  Preserve an existing tag instead of
   * tagging it a second time.
   */
  int64_t tagged = is_int(n) ? n : rt_tag_v(n);
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

/*
 * Normalized failure entry (C5): kind strings mirror lib/error.ny's
 * ERR_* vocabulary ("err.index", "err.div_zero", …) so every backend emits
 * the same kind + message. The panic message becomes "<kind>: <message>",
 * which is exactly what error_message(exception(kind, msg)) produces, so
 * catch sites see one shape across interpreter, JIT, and native routes.
 */
int64_t rt_fail(int64_t kind_ptr, int64_t msg_ptr) {
  const char *k = NULL, *m = NULL;
  if (rt_native_is_str(kind_ptr) || is_v_str(kind_ptr))
    k = (const char *)(uintptr_t)kind_ptr;
  if (rt_native_is_str(msg_ptr) || is_v_str(msg_ptr))
    m = (const char *)(uintptr_t)msg_ptr;
  char buf[512];
  size_t kl = k ? strlen(k) : 0;
  size_t ml = m ? strlen(m) : 0;
  if (kl > 200)
    kl = 200;
  if (ml > 280)
    ml = 280;
  size_t used = 0;
  if (kl) {
    memcpy(buf, k, kl);
    used = kl;
  }
  buf[used++] = ':';
  if (ml) {
    buf[used++] = ' ';
    memcpy(buf + used, m, ml);
    used += ml;
  }
  buf[used] = '\0';
  rt_panic(rt_alloc_string(buf));
  return 0;
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
