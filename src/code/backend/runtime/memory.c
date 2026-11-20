/*
 * Memory runtime: allocator, deallocator, realloc, zero-init, and
 * heap management primitives with ownership and GC integration hooks.
 */
#include "code/backend/runtime/shared.h"
void rt_str_forget_pointer(int64_t v);
#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

extern int64_t rt_copy_mem(int64_t dst, int64_t src, int64_t n);

/*
 * The float wrapper is defined before the general indexed store below.
 */
int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val);

int g_rt_zero_init = 1; /* default: always zero (current behavior) */
static int g_rt_zero_init_cached = 0;
static inline bool rt_zero_init_enabled(void) {
  if (g_rt_zero_init_cached)
    return g_rt_zero_init != 0;
  const char *env = getenv("NYTRIX_ZERO_INIT");
  g_rt_zero_init = !(env && (*env == '0' || strcmp(env, "false") == 0));
  g_rt_zero_init_cached = 1;
  return g_rt_zero_init != 0;
}

#ifdef _WIN32
#include <malloc.h>
#endif

static int g_mem_trace = -1;
static inline bool mem_trace_enabled(void) {
  if (g_mem_trace >= 0)
    return g_mem_trace != 0;
  const char *env = getenv("NYTRIX_MEM_TRACE");
  g_mem_trace = (env && (*env == '1' || strcmp(env, "true") == 0)) ? 1 : 0;
  return g_mem_trace != 0;
}

#ifndef NDEBUG
static uint64_t g_alloc = 0;
static uint64_t g_free = 0;
static uint64_t g_pool_hits = 0;

__attribute__((destructor)) static void rt_stats(void) {
  const char *env = getenv("NYTRIX_MEM_STATS");
  if (env && *env == '1') {
    fprintf(stderr, "\n━━━ Nytrix Runtime Stats ━━━\n");
    fprintf(stderr, "Allocated: %" PRIu64 " bytes\n", g_alloc);
    fprintf(stderr, "Freed:     %" PRIu64 " bytes\n", g_free);
    fprintf(stderr, "Leaked:    %" PRId64 " bytes\n",
            (int64_t)(g_alloc - g_free));
    fprintf(stderr, "Pool hits: %" PRIu64 " (%.1f%%)\n", g_pool_hits,
            g_alloc ? 100.0 * g_pool_hits / g_alloc : 0.0);
  }
}
#endif

atomic_uint_fast64_t g_ny_alloc_count = 0;
atomic_uint_fast64_t g_ny_realloc_count = 0;

static inline void *ny_aligned_alloc(size_t alignment, size_t size) {
#ifdef _WIN32
  return _aligned_malloc(size, alignment);
#else
  if (alignment <= _Alignof(max_align_t))
    return malloc(size);
  void *p = NULL;
  if (posix_memalign(&p, alignment, size) != 0)
    return NULL;
  return p;
#endif
}

static inline void ny_aligned_free(void *p) {
#ifdef _WIN32
  _aligned_free(p);
#else
  free(p);
#endif
}

/*
 * Finer segregated classes keep the 32-byte header while avoiding the former
 * power-of-two fragmentation cliff.  All entries retain 16-byte alignment.
 */
static const size_t g_pool_sizes[] = {32,   48,   64,   80,   96,   112,  128,
                                      160,  192,  224,  256,  320,  384,  448,
                                      512,  640,  768,  896,  1024, 1280, 1536,
                                      1792, 2048, 2560, 3072, 3584, 4096};
#define NUM_POOLS (sizeof(g_pool_sizes) / sizeof(g_pool_sizes[0]))
typedef struct mem_pool_node {
  struct mem_pool_node *next;
} mem_pool_node_t;
_Thread_local mem_pool_node_t *g_mem_pools[NUM_POOLS] = {0};

/*
 * Exact provenance for objects returned by the managed allocator.  GC objects
 * are tracked by the collector's object descriptors; this table covers the
 * separate 32-byte-header allocator without reading user-controlled memory.
 */
#define RT_MANAGED_PTR_TABLE_BITS 22u
#define RT_MANAGED_PTR_TABLE_SIZE (1u << RT_MANAGED_PTR_TABLE_BITS)
#define RT_MANAGED_PTR_TABLE_MASK (RT_MANAGED_PTR_TABLE_SIZE - 1u)
#define RT_MANAGED_PTR_TOMBSTONE UINTPTR_MAX
#define RT_MANAGED_PTR_REUSE_WINDOW 16u
static _Atomic(uintptr_t) rt_managed_ptr_table[RT_MANAGED_PTR_TABLE_SIZE];
/*
 * Per-thread answers of rt_heap_ptr_registry_has, both "yes" and "no". Each
 * cache slot has a generation shared by all threads; registering or
 * unregistering a value advances the generation of the slot the value maps
 * to, so every thread drops answers about it (and only those). The classifiers
 * ask about the same few hundred words again and again in a UI frame; most
 * are not managed, and every such miss used to be a load from a random place
 * in the 32 MiB table. (The previous 8-entry cache held only hits and was
 * cleared on every free.)
 */
#define RT_MANAGED_PTR_CACHE_BITS 11u
#define RT_MANAGED_PTR_CACHE_SIZE (1u << RT_MANAGED_PTR_CACHE_BITS)
typedef struct rt_managed_ptr_cache_entry {
  uintptr_t value;
  uint32_t gen;
  uint32_t has;
} rt_managed_ptr_cache_entry_t;
static _Thread_local rt_managed_ptr_cache_entry_t
    rt_managed_ptr_cache[RT_MANAGED_PTR_CACHE_SIZE];
static _Atomic(uint32_t) rt_managed_ptr_cache_gen[RT_MANAGED_PTR_CACHE_SIZE];

static inline size_t rt_managed_ptr_cache_slot(uintptr_t value) {
  return (size_t)(((uint64_t)value * UINT64_C(0x9E3779B97F4A7C15)) >>
                  (64u - RT_MANAGED_PTR_CACHE_BITS));
}

/*
 * The writer lock serializes generation updates. Publish the table change
 * with release semantics without a second locked read-modify-write.
 */
static inline void rt_managed_ptr_cache_invalidate(uintptr_t value) {
  _Atomic(uint32_t) *gen =
      &rt_managed_ptr_cache_gen[rt_managed_ptr_cache_slot(value)];
  uint32_t next = atomic_load_explicit(gen, memory_order_relaxed) + 1u;
  atomic_store_explicit(gen, next, memory_order_release);
}

static size_t rt_managed_ptr_slot(uintptr_t value) {
  /*
   * Managed payloads are aligned and commonly adjacent. The 32-byte granules
   * of one 4 KiB page map to one 128-slot run (the smallest managed block is
   * 48 bytes, so neighbours get distinct slots and share the run's cache
   * lines); pages map to runs through a multiplicative hash, which also
   * moves thread arenas 64 MiB apart away from each other. Mapping every
   * granule to the next slot made densely packed payloads one probe chain,
   * and every miss (a classifier asking about a non-managed word) walked it.
   */
  uint64_t page = (uint64_t)value >> 12;
  uint64_t run = (page * UINT64_C(0x9E3779B97F4A7C15)) >>
                 (64u - (RT_MANAGED_PTR_TABLE_BITS - 7u));
  return (size_t)((run << 7) | (((uint64_t)value >> 5) & 127u)) &
         RT_MANAGED_PTR_TABLE_MASK;
}

/*
 * Writers (register, unregister) serialize on this lock; lookups stay
 * lock-free. With one writer at a time a released slot can be returned to
 * "empty" whenever the slot after it is empty, so tombstones do not pile up:
 * a program that allocates and frees strings every frame would otherwise
 * fill the table with them, and every miss would walk the whole table.
 */
static atomic_flag rt_managed_ptr_lock = ATOMIC_FLAG_INIT;

static inline void rt_managed_ptr_writer_lock(void) {
  while (atomic_flag_test_and_set_explicit(&rt_managed_ptr_lock,
                                           memory_order_acquire)) {
  }
}

static inline void rt_managed_ptr_writer_unlock(void) {
  atomic_flag_clear_explicit(&rt_managed_ptr_lock, memory_order_release);
}

/*
 * Lowest and highest address ever registered. Most values the classifiers
 * ask about are integers (tagged or raw) far below every heap address; they
 * are answered here instead of by a miss in the 32 MiB table, which is a
 * cache miss each time.
 */
static _Atomic(uintptr_t) rt_managed_ptr_lo = UINTPTR_MAX;
static _Atomic(uintptr_t) rt_managed_ptr_hi = 0;
static void rt_managed_ptr_register(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  size_t tombstone = SIZE_MAX;
  rt_managed_ptr_writer_lock();
  /*
   * Writers hold the lock, so plain read-modify-write is enough; widen the
   * bounds before the entry is published.
   */
  if (value < atomic_load_explicit(&rt_managed_ptr_lo, memory_order_relaxed))
    atomic_store_explicit(&rt_managed_ptr_lo, value, memory_order_release);
  if (value > atomic_load_explicit(&rt_managed_ptr_hi, memory_order_relaxed))
    atomic_store_explicit(&rt_managed_ptr_hi, value, memory_order_release);
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_relaxed);
    if (seen == value) {
      rt_managed_ptr_writer_unlock();
      return;
    }
    if (seen == RT_MANAGED_PTR_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = slot;
    /*
     * The allocator hands out addresses that are not registered, so the
     * duplicate check only needs a short window. Scanning on to an empty slot
     * walked every tombstone left by earlier frees (97% of the time in a
     * defer-heavy loop).
     */
    if (!seen ||
        (tombstone != SIZE_MAX && probe >= RT_MANAGED_PTR_REUSE_WINDOW)) {
      size_t target = tombstone == SIZE_MAX ? slot : tombstone;
      atomic_store_explicit(&rt_managed_ptr_table[target], value,
                            memory_order_release);
      rt_managed_ptr_cache_invalidate(value);
      rt_managed_ptr_writer_unlock();
      return;
    }
  }
  if (tombstone != SIZE_MAX) {
    atomic_store_explicit(&rt_managed_ptr_table[tombstone], value,
                          memory_order_release);
    rt_managed_ptr_cache_invalidate(value);
  }
  rt_managed_ptr_writer_unlock();
}

static void rt_managed_ptr_unregister(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  rt_managed_ptr_writer_lock();
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_relaxed);
    if (!seen)
      break;
    if (seen != value)
      continue;
    size_t next = (slot + 1) & RT_MANAGED_PTR_TABLE_MASK;
    if (atomic_load_explicit(&rt_managed_ptr_table[next],
                             memory_order_relaxed) != 0) {
      atomic_store_explicit(&rt_managed_ptr_table[slot],
                            RT_MANAGED_PTR_TOMBSTONE, memory_order_release);
    } else {
      /*
       * Nothing probes past an empty slot: this one, and any run of
       * tombstones before it, can be empty again.
       */
      atomic_store_explicit(&rt_managed_ptr_table[slot], 0,
                            memory_order_release);
      size_t prev = (slot - 1) & RT_MANAGED_PTR_TABLE_MASK;
      while (atomic_load_explicit(&rt_managed_ptr_table[prev],
                                  memory_order_relaxed) ==
             RT_MANAGED_PTR_TOMBSTONE) {
        atomic_store_explicit(&rt_managed_ptr_table[prev], 0,
                              memory_order_release);
        prev = (prev - 1) & RT_MANAGED_PTR_TABLE_MASK;
      }
    }
    rt_managed_ptr_cache_invalidate(value);
    break;
  }
  rt_managed_ptr_writer_unlock();
}

bool rt_heap_ptr_registry_has(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return false;
  if (value < atomic_load_explicit(&rt_managed_ptr_lo, memory_order_acquire) ||
      value > atomic_load_explicit(&rt_managed_ptr_hi, memory_order_acquire))
    return false;
  size_t cache_slot = rt_managed_ptr_cache_slot(value);
  uint32_t gen = atomic_load_explicit(&rt_managed_ptr_cache_gen[cache_slot],
                                      memory_order_acquire);
  rt_managed_ptr_cache_entry_t *cached = &rt_managed_ptr_cache[cache_slot];
  if (cached->value == value && cached->gen == gen)
    return cached->has != 0;
  size_t start = rt_managed_ptr_slot(value);
  bool has = false;
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    uintptr_t seen = atomic_load_explicit(
        &rt_managed_ptr_table[(start + probe) & RT_MANAGED_PTR_TABLE_MASK],
        memory_order_acquire);
    if (seen == value) {
      has = true;
      break;
    }
    if (!seen)
      break;
  }
  /*
   * `gen` was read before the table: a change since then left a newer
   * generation, and this entry will not be used.
   */
  cached->value = value;
  cached->gen = gen;
  cached->has = has ? 1u : 0u;
  return has;
}

typedef struct ny_rc_entry {
  uintptr_t ptr;
  uint64_t count;
  struct ny_rc_entry *next;
} ny_rc_entry_t;

#define NY_RC_BUCKETS 4096u
static atomic_flag g_rc_lock = ATOMIC_FLAG_INIT;
static ny_rc_entry_t *g_rc_table[NY_RC_BUCKETS] = {0};
static int g_rc_enabled = -1;

void rt_rc_set_enabled(int enabled) { g_rc_enabled = enabled != 0; }

static inline bool rt_rc_enabled(void) {
  if (g_rc_enabled >= 0)
    return g_rc_enabled != 0;
  const char *policy = getenv("NYTRIX_HEAP_POLICY");
  const char *rc = getenv("NYTRIX_RC_GC");
  g_rc_enabled = ((policy && strcmp(policy, "rc") == 0) ||
                  (rc && (*rc == '1' || strcmp(rc, "true") == 0)))
                     ? 1
                     : 0;
  return g_rc_enabled != 0;
}

static inline void rt_rc_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_rc_lock, memory_order_acquire)) {
  }
}

static inline void rt_rc_unlock(void) {
  atomic_flag_clear_explicit(&g_rc_lock, memory_order_release);
}

static inline size_t rt_rc_bucket(uintptr_t ptr) {
  return (ptr >> 4) & (NY_RC_BUCKETS - 1u);
}

static ny_rc_entry_t *rt_rc_find_locked(uintptr_t ptr,
                                        ny_rc_entry_t ***prev_next) {
  size_t b = rt_rc_bucket(ptr);
  ny_rc_entry_t **slot = &g_rc_table[b];
  while (*slot) {
    if ((*slot)->ptr == ptr) {
      if (prev_next)
        *prev_next = slot;
      return *slot;
    }
    slot = &(*slot)->next;
  }
  if (prev_next)
    *prev_next = slot;
  return NULL;
}

static void rt_rc_adopt_new(int64_t ptr) {
  if (!rt_rc_enabled() || !ptr)
    return;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    entry->count = 1;
  } else {
    entry = (ny_rc_entry_t *)calloc(1, sizeof(*entry));
    if (entry) {
      entry->ptr = p;
      entry->count = 1;
      entry->next = *slot;
      *slot = entry;
    }
  }
  rt_rc_unlock();
}

static void rt_rc_forget(int64_t ptr) {
  if (!ptr)
    return;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    *slot = entry->next;
    free(entry);
  }
  rt_rc_unlock();
}

static inline int ny_mem_pool_slot(size_t total) {
  /*
   * ASan needs each managed block to have its own redzones and lifetime.
   */
  if (NY_WITH_ASAN)
    return -1;
  for (int i = 0; i < (int)NUM_POOLS; i++) {
    if (total <= g_pool_sizes[i])
      return i;
  }
  return -1;
}

/*
 * Safe heap (`--heap=safe` / NYTRIX_SAFE_HEAP=1).
 *
 * A guard-page allocator for the allocations a user program makes itself
 * (`malloc`, `zalloc`, the raw heap). Each block lives in its own mapping and
 * ends at a PROT_NONE page after rounding to 16-byte alignment. Instrumented
 * byte accesses also check the requested size, including alignment padding.
 * `free` makes the whole mapping inaccessible instead of recycling
 * it and keeps it in a quarantine, so a later load or store faults too, and
 * its address space is not reused until the quarantine evicts it. A fault
 * handler classifies the address (use-after-free, overflow, double free) and
 * exits with a clear message; an exit hook reports user blocks never freed.
 * Runtime-internal objects keep the normal allocator, so the cost is paid only
 * by allocations the program owns.
 */
#include <errno.h>
#include <pthread.h>
#ifndef _WIN32
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>
#include <ucontext.h>
#endif

typedef struct {
  uintptr_t user;     /* payload address; 0 = empty slot, 1 = tombstone */
  uintptr_t map;      /* start of the mapping */
  size_t map_len;     /* bytes mapped: data + guard + padding page */
  size_t prot_len;    /* data + guard bytes (the part made inaccessible) */
  size_t size;        /* requested payload size */
  uint8_t freed;      /* 1 after free (quarantined) */
  uint8_t user_block; /* allocated through a user-facing entry point */
} sh_entry_t;

#define SH_QUARANTINE_MAX 8192u
#define SH_QUARANTINE_BYTES ((size_t)512 << 20)

static pthread_mutex_t g_sh_lock = PTHREAD_MUTEX_INITIALIZER;
static sh_entry_t *g_sh_tab;
static size_t g_sh_cap;
static size_t g_sh_used;
static uintptr_t g_sh_quarantine[SH_QUARANTINE_MAX];
static size_t g_sh_q_head, g_sh_q_len, g_sh_q_bytes;
static size_t g_sh_live, g_sh_live_bytes;
static _Atomic int g_sh_state = -1; /* -1 unknown, 0 off, 1 on */
#ifdef _WIN32
static PVOID g_sh_handler;
#else
static int g_sh_handlers;
#endif

static bool sh_enabled(void) {
  int st = atomic_load_explicit(&g_sh_state, memory_order_relaxed);
  if (__builtin_expect(st >= 0, 1))
    return st == 1;
  const char *e = getenv("NYTRIX_SAFE_HEAP");
  int on = e && *e && strcmp(e, "0") != 0;
  atomic_store_explicit(&g_sh_state, on, memory_order_relaxed);
  return on;
}

/*
 * Allocation-failure injection for user allocators: with
 * NYTRIX_FAIL_ALLOC_AFTER=N, the (N+1)th and later `malloc`/`zalloc`/raw-heap
 * allocation requests, including raw reallocations, return 0, exactly as a
 * real out-of-memory would. Lets a program (and the test suite) prove its
 * `if !p` paths run.
 */
static _Atomic long g_oom_budget = -2; /* -2 unread, -1 off, >=0 remaining */

static bool rt_oom_inject(void) {
  long b = atomic_load_explicit(&g_oom_budget, memory_order_relaxed);
  if (__builtin_expect(b == -1, 1))
    return false;
  if (b == -2) {
    const char *e = getenv("NYTRIX_FAIL_ALLOC_AFTER");
    long v = e && *e ? strtol(e, NULL, 10) : -1;
    if (v < 0) {
      atomic_store_explicit(&g_oom_budget, -1, memory_order_relaxed);
      return false;
    }
    long expected = -2;
    atomic_compare_exchange_strong(&g_oom_budget, &expected, v);
    b = atomic_load_explicit(&g_oom_budget, memory_order_relaxed);
  }
  for (;;) {
    if (b <= 0)
      return true;
    if (atomic_compare_exchange_weak(&g_oom_budget, &b, b - 1))
      return false;
  }
}

bool rt_user_alloc_injected_failure(void) { return rt_oom_inject(); }

/*
 * Called from the lowered program's entry prologue under `--heap=safe`.
 */
int64_t rt_safe_heap_arm(void) {
  atomic_store_explicit(&g_sh_state, 1, memory_order_relaxed);
  return 0;
}

static size_t sh_hash(uintptr_t p) {
  return (size_t)((p >> 4) * 0x9E3779B97F4A7C15ull);
}

static sh_entry_t *sh_find(uintptr_t user) {
  if (!g_sh_cap)
    return NULL;
  size_t i = sh_hash(user) & (g_sh_cap - 1);
  for (size_t n = 0; n < g_sh_cap; ++n, i = (i + 1) & (g_sh_cap - 1)) {
    sh_entry_t *e = &g_sh_tab[i];
    if (e->user == user)
      return e;
    if (e->user == 0)
      return NULL;
  }
  return NULL;
}

static bool sh_grow(void) {
  size_t cap = g_sh_cap ? g_sh_cap * 2 : 1024;
  sh_entry_t *tab = calloc(cap, sizeof(*tab));
  if (!tab)
    return false;
  size_t used = 0;
  for (size_t i = 0; i < g_sh_cap; ++i) {
    sh_entry_t *old = &g_sh_tab[i];
    if (old->user <= 1)
      continue;
    size_t j = sh_hash(old->user) & (cap - 1);
    while (tab[j].user)
      j = (j + 1) & (cap - 1);
    tab[j] = *old;
    ++used;
  }
  free(g_sh_tab);
  g_sh_tab = tab;
  g_sh_cap = cap;
  g_sh_used = used;
  return true;
}

static sh_entry_t *sh_insert(uintptr_t user) {
  if ((g_sh_used + 1) * 2 > g_sh_cap && !sh_grow())
    return NULL;
  size_t i = sh_hash(user) & (g_sh_cap - 1);
  while (g_sh_tab[i].user > 1)
    i = (i + 1) & (g_sh_cap - 1);
  if (g_sh_tab[i].user == 0)
    ++g_sh_used;
  g_sh_tab[i].user = user;
  return &g_sh_tab[i];
}

static const sh_entry_t *sh_find_address(uintptr_t addr) {
  const sh_entry_t *exact = addr > 1 ? sh_find(addr) : NULL;
  if (exact && exact->user > 1)
    return exact;
  for (size_t i = 0; i < g_sh_cap; ++i) {
    const sh_entry_t *e = &g_sh_tab[i];
    if (e->user > 1 && addr >= e->map && addr - e->map < e->prot_len)
      return e;
  }
  return NULL;
}

static void sh_message(const char *kind, uintptr_t addr, const sh_entry_t *e) {
  char buf[320];
  int n;
  if (e)
    n = snprintf(buf, sizeof(buf),
                 "Nytrix safe-heap: %s at %p (block %p, %zu byte(s))\n", kind,
                 (void *)addr, (void *)e->user, e->size);
  else
    n = snprintf(buf, sizeof(buf), "Nytrix safe-heap: %s at %p\n", kind,
                 (void *)addr);
  if (n > 0) {
    ssize_t ignored = write(2, buf, (size_t)n);
    (void)ignored;
  }
}

static bool sh_report_fault(uintptr_t addr) {
  const sh_entry_t *hit = sh_find_address(addr);
  if (!hit)
    return false;
  if (hit->freed)
    sh_message("use-after-free", addr, hit);
  else if (addr >= hit->user + hit->size)
    sh_message("heap-buffer-overflow", addr, hit);
  else
    sh_message("invalid access", addr, hit);
  _exit(134);
}

#ifdef _WIN32
static LONG CALLBACK sh_fault(EXCEPTION_POINTERS *exception) {
  const EXCEPTION_RECORD *record = exception->ExceptionRecord;
  if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      record->NumberParameters >= 2)
    sh_report_fault((uintptr_t)record->ExceptionInformation[1]);
  return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void sh_fault(int sig, siginfo_t *info, void *ctx) {
#if defined(__linux__) && defined(__x86_64__)
  if (getenv("NYTRIX_SAFE_HEAP_TRACE")) {
    const ucontext_t *uc = ctx;
    void *pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname)
      fprintf(stderr, "Nytrix safe-heap: fault pc=%p in %s+%#lx\n", pc,
              di.dli_sname, (unsigned long)((char *)pc - (char *)di.dli_saddr));
    else
      fprintf(stderr, "Nytrix safe-heap: fault pc=%p\n", pc);
  }
#else
  (void)ctx;
#endif
  if (!sh_report_fault((uintptr_t)info->si_addr))
    signal(sig, SIG_DFL);
}
#endif

static bool sh_install(void) {
#ifdef _WIN32
  if (!g_sh_handler)
    g_sh_handler = AddVectoredExceptionHandler(1, sh_fault);
  return g_sh_handler != NULL;
#else
  if (g_sh_handlers)
    return true;
  static char altstack[64 * 1024];
  stack_t ss = {.ss_sp = altstack, .ss_size = sizeof(altstack), .ss_flags = 0};
  sigaltstack(&ss, NULL);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = sh_fault;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  g_sh_handlers = 1;
  return true;
#endif
}

static void *sh_map(size_t size) {
#ifdef _WIN32
  return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void *m = mmap(NULL, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return m == MAP_FAILED ? NULL : m;
#endif
}

static bool sh_protect(void *m, size_t size) {
#ifdef _WIN32
  DWORD old_protection;
  return VirtualProtect(m, size, PAGE_NOACCESS, &old_protection) != 0;
#else
  return mprotect(m, size, PROT_NONE) == 0;
#endif
}

static void sh_unmap(void *m, size_t size) {
#ifdef _WIN32
  (void)size;
  VirtualFree(m, 0, MEM_RELEASE);
#else
  munmap(m, size);
#endif
}

static void sh_leak_report(void) {
#ifdef _WIN32
  RemoveVectoredExceptionHandler(g_sh_handler);
#endif
  size_t blocks = 0, bytes = 0;
  pthread_mutex_lock(&g_sh_lock);
  for (size_t i = 0; i < g_sh_cap; ++i) {
    const sh_entry_t *e = &g_sh_tab[i];
    if (e->user > 1 && !e->freed && e->user_block) {
      ++blocks;
      bytes += e->size;
    }
  }
  pthread_mutex_unlock(&g_sh_lock);
  if (blocks) {
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
                     "Nytrix safe-heap: leak: %zu allocation(s), %zu byte(s) "
                     "never freed\n",
                     blocks, bytes);
    if (n > 0) {
      ssize_t ignored = write(2, buf, (size_t)n);
      (void)ignored;
    }
  }
}

static void sh_evict_one(void) {
  uintptr_t user = g_sh_quarantine[g_sh_q_head];
  g_sh_q_head = (g_sh_q_head + 1) % SH_QUARANTINE_MAX;
  --g_sh_q_len;
  sh_entry_t *e = sh_find(user);
  if (!e)
    return;
  g_sh_q_bytes -= e->map_len;
  sh_unmap((void *)e->map, e->map_len);
  e->user = 1; /* tombstone */
}

/*
 * Allocate `size` bytes whose last byte abuts a guard page. `lead` extra
 * bytes are reserved before the payload (the managed-heap header).
 */
static void *sh_alloc(size_t size, size_t lead, bool user_block) {
  pthread_mutex_lock(&g_sh_lock);
  if (!sh_install()) {
    pthread_mutex_unlock(&g_sh_lock);
    return NULL;
  }
  static bool leak_hook;
  if (!leak_hook) {
    leak_hook = true;
    atexit(sh_leak_report);
  }
  size_t page = (size_t)ny_page_size();
  size_t body = (size + 15) & ~(size_t)15;
  size_t data = lead + body;
  size_t data_pages = (data + page - 1) / page;
  /*
   * [data pages][guard page][padding page]. The padding stays readable: the
   * runtime peeks a few bytes before arbitrary pointers (managed-string
   * detection), and another mapping placed right after this one must not
   * have that look-behind land in the guard.
   */
  size_t prot_len = (data_pages + 1) * page;
  size_t map_len = prot_len + page;
  void *m = sh_map(map_len);
  if (!m) {
    pthread_mutex_unlock(&g_sh_lock);
    return NULL;
  }
  uintptr_t guard = (uintptr_t)m + data_pages * page;
  if (!sh_protect((void *)guard, page)) {
    sh_unmap(m, map_len);
    pthread_mutex_unlock(&g_sh_lock);
    return NULL;
  }
  uintptr_t user = guard - body;
  sh_entry_t *e = sh_insert(user);
  if (!e) {
    sh_unmap(m, map_len);
    pthread_mutex_unlock(&g_sh_lock);
    return NULL;
  }
  e->map = (uintptr_t)m;
  e->map_len = map_len;
  e->prot_len = prot_len;
  e->size = size;
  e->freed = 0;
  e->user_block = user_block ? 1 : 0;
  ++g_sh_live;
  g_sh_live_bytes += size;
  pthread_mutex_unlock(&g_sh_lock);
  return (void *)user;
}

/*
 * `free(p)` passes the pointer through a variadic list, which stores it as a
 * tagged word (`p << 1 | 1`). Recover the real address when that decodes to a
 * safe-heap block; real blocks are 16-byte aligned, so an odd value is never
 * a safe-heap address itself.
 */
static int64_t sh_normalize(int64_t ptr) {
  if (!g_sh_cap || !(ptr & 1))
    return ptr;
  uintptr_t cand = (uintptr_t)ptr >> 1;
  pthread_mutex_lock(&g_sh_lock);
  bool known = sh_find(cand) != NULL;
  pthread_mutex_unlock(&g_sh_lock);
  return known ? (int64_t)cand : ptr;
}

static size_t sh_size_of(uintptr_t user) {
  pthread_mutex_lock(&g_sh_lock);
  sh_entry_t *e = sh_find(user);
  size_t n = e && !e->freed ? e->size : 0;
  pthread_mutex_unlock(&g_sh_lock);
  return n;
}

static bool sh_owns(uintptr_t user) {
  if (!g_sh_cap)
    return false;
  pthread_mutex_lock(&g_sh_lock);
  sh_entry_t *e = sh_find(user);
  bool owned = e && !e->freed;
  pthread_mutex_unlock(&g_sh_lock);
  return owned;
}

int64_t rt_safe_heap_byte_addr(int64_t base, int64_t offset) {
  uintptr_t address = (uintptr_t)base + (uintptr_t)offset;
  if (!sh_enabled())
    return (int64_t)address;
  pthread_mutex_lock(&g_sh_lock);
  const sh_entry_t *entry = sh_find_address((uintptr_t)base);
  if (!entry && address != (uintptr_t)base)
    entry = sh_find_address(address);
  bool invalid = entry && (entry->freed || address < entry->user ||
                           address - entry->user >= entry->size);
  if (!invalid) {
    pthread_mutex_unlock(&g_sh_lock);
    return (int64_t)address;
  }
  /*
   * Reporting must not retain a pointer into a table another thread can
   * resize once the mutex is released. No allocation on the failure path.
   */
  sh_entry_t failed = *entry;
  pthread_mutex_unlock(&g_sh_lock);
  sh_message(failed.freed ? "use-after-free" : "heap-buffer-overflow", address,
             &failed);
  _exit(134);
}

/*
 * 1 = released, 0 = not a safe-heap block. A double free exits.
 */
static int sh_release(uintptr_t user) {
  if (!g_sh_cap)
    return 0;
  pthread_mutex_lock(&g_sh_lock);
  sh_entry_t *e = sh_find(user);
  if (!e) {
    pthread_mutex_unlock(&g_sh_lock);
    return 0;
  }
  if (e->freed) {
    sh_message("double free", user, e);
    pthread_mutex_unlock(&g_sh_lock);
    _exit(134);
  }
  if (!sh_protect((void *)e->map, e->prot_len)) {
    sh_message("could not protect freed block", user, e);
    pthread_mutex_unlock(&g_sh_lock);
    _exit(134);
  }
  e->freed = 1;
  --g_sh_live;
  g_sh_live_bytes -= e->size;
  g_sh_q_bytes += e->map_len;
  g_sh_quarantine[(g_sh_q_head + g_sh_q_len) % SH_QUARANTINE_MAX] = user;
  ++g_sh_q_len;
  while (g_sh_q_len > SH_QUARANTINE_MAX - 1 ||
         g_sh_q_bytes > SH_QUARANTINE_BYTES)
    sh_evict_one();
  pthread_mutex_unlock(&g_sh_lock);
  return 1;
}

/*
 * A pointer given to free() that the safe heap already released.
 */
static void sh_check_double_free(uintptr_t user) {
  if (!g_sh_cap)
    return;
  pthread_mutex_lock(&g_sh_lock);
  const sh_entry_t *e = sh_find(user);
  bool freed = e && e->freed;
  if (freed)
    sh_message("double free", user, e);
  pthread_mutex_unlock(&g_sh_lock);
  if (freed)
    _exit(134);
}

/*
 * Managed-heap allocation (`__malloc`): 32-byte header, then the payload.
 */
static int64_t sh_traced_alloc(size_t body_in) {
  size_t body = (body_in + 15) & ~(size_t)15;
  char *user = sh_alloc(body_in, 32, true);
  if (!user)
    return 0;
  char *base = user - 32;
  *(uint64_t *)base = NY_MAGIC1;
  *(uint64_t *)(base + 8) = (uint64_t)((body << 1) | 1);
  *(uint64_t *)(base + 16) = 0;
  *(uint64_t *)(base + 24) = 0;
  int64_t res = (int64_t)(uintptr_t)user;
  atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  /*
   * Not registered in the map oracle: it probes page ends, which would touch
   * the guard page this block deliberately abuts.
   */
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
  return res;
}

/*
 * Segregated free lists recycle blocks that a program releases, but a
 * workload that keeps every result live (builders, benchmark loops) would
 * otherwise enter the general-purpose allocator once per object. Carve such
 * blocks from per-thread 1 MiB chunks instead. Chunk ranges are registered so
 * pool teardown never hands an interior pointer to free(); each chunk is
 * registered with the map oracle once, covering every block it contains.
 */
#define RT_BLOCK_CHUNK_BYTES ((size_t)1 << 20)
#define RT_BLOCK_CHUNK_MAX 2048u
typedef struct rt_block_chunk {
  uintptr_t start;
  uintptr_t end;
} rt_block_chunk_t;
static rt_block_chunk_t g_rt_block_chunks[RT_BLOCK_CHUNK_MAX];
static _Atomic size_t g_rt_block_chunk_n;
static atomic_flag g_rt_block_chunk_lock = ATOMIC_FLAG_INIT;
static _Thread_local unsigned char *g_rt_block_bump;
static _Thread_local size_t g_rt_block_left;

static bool rt_block_chunk_register(uintptr_t start, size_t bytes) {
  bool ok = false;
  while (atomic_flag_test_and_set_explicit(&g_rt_block_chunk_lock,
                                           memory_order_acquire)) {
  }
  size_t n = atomic_load_explicit(&g_rt_block_chunk_n, memory_order_relaxed);
  if (n < RT_BLOCK_CHUNK_MAX) {
    g_rt_block_chunks[n].start = start;
    g_rt_block_chunks[n].end = start + bytes;
    atomic_store_explicit(&g_rt_block_chunk_n, n + 1, memory_order_release);
    ok = true;
  }
  atomic_flag_clear_explicit(&g_rt_block_chunk_lock, memory_order_release);
  return ok;
}

static bool rt_block_chunk_owns(uintptr_t p) {
  size_t n = atomic_load_explicit(&g_rt_block_chunk_n, memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (p >= g_rt_block_chunks[i].start && p < g_rt_block_chunks[i].end)
      return true;
  return false;
}

static void *rt_block_bump_alloc(size_t bytes) {
  bytes = (bytes + 15u) & ~(size_t)15u;
  if (bytes == 0)
    return NULL;
  if (g_rt_block_left < bytes) {
    size_t chunk = RT_BLOCK_CHUNK_BYTES;
    if (chunk < bytes) {
      if (bytes > SIZE_MAX - (RT_BLOCK_CHUNK_BYTES - 1))
        return NULL;
      chunk = (bytes + RT_BLOCK_CHUNK_BYTES - 1) & ~(RT_BLOCK_CHUNK_BYTES - 1);
    }
    unsigned char *fresh = (unsigned char *)ny_aligned_alloc(16, chunk);
    if (!fresh)
      return NULL;
    if (!rt_block_chunk_register((uintptr_t)fresh, chunk)) {
      ny_aligned_free(fresh);
      return NULL;
    }
    rt_map_oracle_add((uintptr_t)fresh, chunk);
    g_rt_block_bump = fresh;
    g_rt_block_left = chunk;
  }
  unsigned char *p = g_rt_block_bump;
  g_rt_block_bump += bytes;
  g_rt_block_left -= bytes;
  return p;
}

/*
 * Two independent heap systems, NOT interconvertible:
 *
 *   GC-traced heap: rt_malloc / rt_malloc_uninit / rt_free / rt_realloc.
 *     Each allocation carries a 32-byte header (NY_MAGIC1 + body size); the
 *     returned pointer is base+32.  rt_free inspects the header, recycles
 *     small allocations through per-size-class pools, and frees large ones.
 *
 *   Raw heap: rt_malloc_raw / rt_free_raw.
 *     A plain malloc/free with no header and no GC participation, used for
 *     unmanaged FFI/native buffers.
 *
 * Crossing the boundary is an error: free_raw on a traced pointer would free
 * the wrong address (base+32), and rt_free on a raw pointer is a silent no-op
 * (leak).  rt_free_raw detects and re-routes traced pointers; callers must
 * keep the two heaps separate.
 */
static int64_t rt_malloc_impl(int64_t size, int zero_fill) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  if (n < 0)
    return 0;
  size_t body = (size_t)n;
  body = (body + 15) & ~15ULL;
  size_t total = body + 32;

  int slot = ny_mem_pool_slot(total);
  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
  void *p = NULL;
  bool bump = false;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = slot >= 0 ? rt_fc_block_alloc(fill_size) : NULL;
    if (!p && slot >= 0) {
      p = rt_block_bump_alloc(fill_size);
      bump = p != NULL;
    }
    if (!p)
      p = ny_aligned_alloc(16, fill_size);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  if (zero_fill && rt_zero_init_enabled())
    memset(p, 0, fill_size);

  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)((body << 1) | 1);
  /*
   * Object metadata must be initialized even when payload zeroing is off.
   * A recycled string header otherwise makes a fresh byte buffer look like
   * the previous string, including its old length.
   */
  *(uint64_t *)((char *)p + 16) = 0;
  *(uint64_t *)((char *)p + 24) = 0;

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  if (!bump)
    rt_map_oracle_add((uintptr_t)p, (size_t)total);
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
  rt_fc_track((uintptr_t)res, RT_FC_KIND_MANAGED, total);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n",
            (void *)(uintptr_t)res, body, total);
  }
  return res;
}

int64_t rt_malloc(int64_t size) { return rt_malloc_impl(size, 1); }

int64_t rt_malloc_uninit(int64_t size) { return rt_malloc_impl(size, 0); }

int64_t rt_malloc_i64(int64_t size) {
  if (size < 0)
    return 0;
  if (__builtin_expect(rt_oom_inject(), 0))
    return 0;
  if (sh_enabled())
    return sh_traced_alloc((size_t)size);
  size_t body = (size_t)size;
  body = (body + 15) & ~15ULL;
  size_t total = body + 32;

  int slot = ny_mem_pool_slot(total);
  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
  void *p = NULL;
  bool bump = false;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = slot >= 0 ? rt_fc_block_alloc(fill_size) : NULL;
    if (!p && slot >= 0) {
      p = rt_block_bump_alloc(fill_size);
      bump = p != NULL;
    }
    if (!p)
      p = ny_aligned_alloc(16, fill_size);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  if (rt_zero_init_enabled())
    memset(p, 0, fill_size);

  *(uint64_t *)p = NY_MAGIC1;
  *(uint64_t *)((char *)p + 8) = (uint64_t)((body << 1) | 1);
  /*
   * Object metadata must be initialized even when payload zeroing is off.
   * A recycled string header otherwise makes a fresh byte buffer look like
   * the previous string, including its old length.
   */
  *(uint64_t *)((char *)p + 16) = 0;
  *(uint64_t *)((char *)p + 24) = 0;

  int64_t res = (int64_t)(uintptr_t)((char *)p + 32);
  atomic_fetch_add_explicit(&g_ny_alloc_count, 1, memory_order_relaxed);
  if (!bump)
    rt_map_oracle_add((uintptr_t)p, (size_t)total);
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
  if (mem_trace_enabled() && total > 1024 * 1024) {
    fprintf(stderr, "[mem] large alloc %p (body=%zu, total=%zu)\n",
            (void *)(uintptr_t)res, body, total);
  }
  return res;
}

/*
 * The interpreter passes tagged integer offsets to runtime primitives.
 */
int64_t rt_ptr_add(int64_t ptr, int64_t offset) {
  return ptr + (is_int(offset) ? rt_untag_v(offset) : offset);
}

int64_t rt_ptr_sub(int64_t ptr, int64_t offset) {
  return ptr - (is_int(offset) ? rt_untag_v(offset) : offset);
}

/*
 * NYIR passes typed integer offsets unboxed, so it needs an explicit bridge.
 */
int64_t rt_ptr_add_i64(int64_t ptr, int64_t offset) { return ptr + offset; }
int64_t rt_ptr_sub_i64(int64_t ptr, int64_t offset) { return ptr - offset; }
typedef struct raw_ptr_node_slab raw_ptr_node_slab_t;

typedef struct raw_ptr_node {
  uintptr_t ptr;
  size_t size;
  struct raw_ptr_node *next;
  struct raw_ptr_node *range_prev;
  struct raw_ptr_node *range_next;
  raw_ptr_node_slab_t *slab;
} raw_ptr_node_t;

/*
 * Keep lookup chains short for the several-hundred-thousand live raw blocks,
 * but keep the table itself cache-resident: at 1<<18 the bucket array spans
 * 2 MiB and every register/unregister pair missed the whole cache hierarchy,
 * which dominated allocation-heavy programs (gcbench spent 42% of its cycles
 * there). 1<<16 spans 512 KiB, stays in L2, and still averages about one node
 * per bucket for a few hundred thousand live blocks.
 */
#define RAW_PTR_HASH_BUCKETS (1u << 16)
#define RAW_PTR_SLAB_CAPACITY 256u
struct raw_ptr_node_slab {
  raw_ptr_node_slab_t *free_prev;
  raw_ptr_node_slab_t *free_next;
  raw_ptr_node_t *free_nodes;
  size_t free_count;
  raw_ptr_node_t nodes[RAW_PTR_SLAB_CAPACITY];
};

static atomic_flag g_raw_ptr_lock = ATOMIC_FLAG_INIT;
static raw_ptr_node_t *g_raw_ptr_table[RAW_PTR_HASH_BUCKETS] = {0};
static raw_ptr_node_t *g_raw_ptr_ranges = NULL;
static raw_ptr_node_slab_t *g_raw_ptr_free_slabs = NULL;
static size_t g_raw_ptr_slab_count = 0;

static inline void raw_ptr_lock(void) {
  while (atomic_flag_test_and_set_explicit(&g_raw_ptr_lock,
                                           memory_order_acquire)) {
  }
}

static inline void raw_ptr_unlock(void) {
  atomic_flag_clear_explicit(&g_raw_ptr_lock, memory_order_release);
}

static inline size_t raw_ptr_bucket(uintptr_t p) {
  return ((p >> 4) ^ (p >> 12)) & (RAW_PTR_HASH_BUCKETS - 1);
}

/*
 * Caller holds g_raw_ptr_lock.
 */
static void raw_ptr_range_insert(raw_ptr_node_t *node) {
  node->range_prev = NULL;
  node->range_next = g_raw_ptr_ranges;
  if (g_raw_ptr_ranges)
    g_raw_ptr_ranges->range_prev = node;
  g_raw_ptr_ranges = node;
}

/*
 * Caller holds g_raw_ptr_lock.
 */
static void raw_ptr_range_remove(raw_ptr_node_t *node) {
  if (node->range_prev)
    node->range_prev->range_next = node->range_next;
  else if (g_raw_ptr_ranges == node)
    g_raw_ptr_ranges = node->range_next;
  if (node->range_next)
    node->range_next->range_prev = node->range_prev;
  node->range_prev = NULL;
  node->range_next = NULL;
}

/*
 * Caller holds g_raw_ptr_lock.
 */
static void raw_ptr_slab_remove_free(raw_ptr_node_slab_t *slab) {
  if (slab->free_prev)
    slab->free_prev->free_next = slab->free_next;
  else
    g_raw_ptr_free_slabs = slab->free_next;
  if (slab->free_next)
    slab->free_next->free_prev = slab->free_prev;
  slab->free_prev = NULL;
  slab->free_next = NULL;
}

/*
 * Caller holds g_raw_ptr_lock.
 */
static void raw_ptr_slab_add_free(raw_ptr_node_slab_t *slab) {
  slab->free_prev = NULL;
  slab->free_next = g_raw_ptr_free_slabs;
  if (g_raw_ptr_free_slabs)
    g_raw_ptr_free_slabs->free_prev = slab;
  g_raw_ptr_free_slabs = slab;
}

/*
 * Caller holds g_raw_ptr_lock.
 */
static raw_ptr_node_t *raw_ptr_node_acquire(void) {
  raw_ptr_node_slab_t *slab = g_raw_ptr_free_slabs;
  if (!slab) {
    slab = (raw_ptr_node_slab_t *)malloc(sizeof(*slab));
    if (!slab)
      return NULL;
    slab->free_prev = NULL;
    slab->free_next = NULL;
    slab->free_nodes = NULL;
    slab->free_count = RAW_PTR_SLAB_CAPACITY;
    for (size_t i = 0; i < RAW_PTR_SLAB_CAPACITY; ++i) {
      raw_ptr_node_t *node = &slab->nodes[i];
      node->slab = slab;
      node->next = slab->free_nodes;
      slab->free_nodes = node;
    }
    raw_ptr_slab_add_free(slab);
    ++g_raw_ptr_slab_count;
  }

  raw_ptr_node_t *node = slab->free_nodes;
  slab->free_nodes = node->next;
  --slab->free_count;
  if (slab->free_count == 0)
    raw_ptr_slab_remove_free(slab);
  return node;
}

static void raw_ptr_register(uintptr_t p, size_t size, bool fresh) {
  if (!p)
    return;
  size_t b = raw_ptr_bucket(p);
  raw_ptr_lock();
  if (!fresh) {
    for (raw_ptr_node_t *old = g_raw_ptr_table[b]; old; old = old->next) {
      if (old->ptr == p) {
        if (size > old->size) {
          if (!old->size)
            raw_ptr_range_insert(old);
          old->size = size;
        }
        raw_ptr_unlock();
        return;
      }
    }
  }
  raw_ptr_node_t *node = raw_ptr_node_acquire();
  if (node) {
    node->ptr = p;
    node->size = size;
    node->next = g_raw_ptr_table[b];
    node->range_prev = NULL;
    node->range_next = NULL;
    g_raw_ptr_table[b] = node;
    if (size)
      raw_ptr_range_insert(node);
  }
  raw_ptr_unlock();
}

void rt_raw_ptr_register(int64_t ptr) {
  raw_ptr_register((uintptr_t)ptr, 0, false);
}

void rt_raw_ptr_register_range(int64_t ptr, size_t size) {
  raw_ptr_register((uintptr_t)ptr, size, false);
}

static bool raw_ptr_unregister(uintptr_t p) {
  if (!p)
    return false;
  size_t b = raw_ptr_bucket(p);
  raw_ptr_lock();
  raw_ptr_node_t **slot = &g_raw_ptr_table[b];
  raw_ptr_node_t *match = NULL;
  raw_ptr_node_slab_t *empty_slab = NULL;
  while (*slot && (*slot)->ptr != p)
    slot = &(*slot)->next;
  if (*slot)
    match = *slot;
  if (match) {
    *slot = match->next;
    if (match->size)
      raw_ptr_range_remove(match);
    raw_ptr_node_slab_t *slab = match->slab;
    if (slab->free_count == 0)
      raw_ptr_slab_add_free(slab);
    match->next = slab->free_nodes;
    slab->free_nodes = match;
    ++slab->free_count;
    if (slab->free_count == RAW_PTR_SLAB_CAPACITY && g_raw_ptr_slab_count > 1) {
      raw_ptr_slab_remove_free(slab);
      --g_raw_ptr_slab_count;
      empty_slab = slab;
    }
  }
  bool found = match != NULL;
  raw_ptr_unlock();
  if (empty_slab)
    free(empty_slab);
  return found;
}

bool rt_raw_ptr_unregister(int64_t ptr) {
  return raw_ptr_unregister((uintptr_t)ptr);
}

bool rt_raw_ptr_registered(int64_t ptr) {
  uintptr_t p = (uintptr_t)ptr;
  if (!p)
    return false;
  bool found = false;
  raw_ptr_lock();
  size_t b = raw_ptr_bucket(p);
  for (raw_ptr_node_t *node = g_raw_ptr_table[b]; node; node = node->next) {
    if (p == node->ptr) {
      found = true;
      break;
    }
  }
  if (!found) {
    for (raw_ptr_node_t *node = g_raw_ptr_ranges; node;
         node = node->range_next) {
      if (p >= node->ptr && p - node->ptr < node->size) {
        found = true;
        break;
      }
    }
  }
  raw_ptr_unlock();
  return found;
}

static bool rt_raw_ptr_is_allocation_base(int64_t ptr) {
  uintptr_t p = (uintptr_t)ptr;
  if (!p)
    return false;
  bool found = false;
  raw_ptr_lock();
  size_t b = raw_ptr_bucket(p);
  for (raw_ptr_node_t *node = g_raw_ptr_table[b]; node; node = node->next) {
    if (p == node->ptr) {
      found = node->size != 0;
      break;
    }
  }
  raw_ptr_unlock();
  return found;
}

/*
 * Shared hook for user allocators defined elsewhere (`zalloc`): returns the
 * finished allocation, 0 for an injected failure, or -1 when neither the safe
 * heap nor failure injection applies and the caller allocates as usual.
 */
int64_t rt_user_alloc_hook(int64_t size) {
  if (__builtin_expect(rt_oom_inject(), 0))
    return 0;
  if (!sh_enabled())
    return -1;
  size_t n = size <= 0 ? 1 : (size_t)size;
  void *sp = sh_alloc(n, 0, true);
  if (!sp)
    return 0;
  raw_ptr_register((uintptr_t)sp, n, true);
  return (int64_t)(uintptr_t)sp;
}

int64_t rt_malloc_raw(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  return rt_malloc_raw_i64(n);
}

int64_t rt_calloc_raw(int64_t size) {
  int64_t n = is_int(size) ? (size >> 1) : size;
  return rt_calloc_raw_i64(n);
}

/*
 * Native NYIR passes unboxed integer arguments, so raw allocation sizes must
 * not be classified from their low bit: an odd byte count is still raw.
 */
int64_t rt_malloc_raw_i64(int64_t size) {
  int64_t n = size;
  if (n <= 0)
    n = 1;
  if (__builtin_expect(rt_oom_inject(), 0))
    return 0;
  if (sh_enabled()) {
    void *sp = sh_alloc((size_t)n, 0, true);
    if (!sp)
      return 0;
    memset(sp, 0xA5, (size_t)n); /* uninitialized-read canary */
    raw_ptr_register((uintptr_t)sp, (size_t)n, true);
    return (int64_t)(uintptr_t)sp;
  }
  void *p = malloc((size_t)n);
  if (!p)
    return 0;
  rt_map_oracle_add((uintptr_t)p, (size_t)n);
  /*
   * malloc cannot return the base of any still-live allocation. Skip the
   * duplicate scan used by the public registration API; this path owns a
   * freshly allocated base and inserts it under the registry lock.
   */
  raw_ptr_register((uintptr_t)p, (size_t)n, true);
  return (int64_t)(uintptr_t)p;
}

int64_t rt_calloc_raw_i64(int64_t size) {
  int64_t n = size;
  if (n <= 0)
    n = 1;
  if (__builtin_expect(rt_oom_inject(), 0))
    return 0;
  if (sh_enabled()) {
    void *sp = sh_alloc((size_t)n, 0, true);
    if (!sp)
      return 0;
    raw_ptr_register((uintptr_t)sp, (size_t)n, true);
    return (int64_t)(uintptr_t)sp;
  }
  void *p = calloc(1, (size_t)n);
  if (!p)
    return 0;
  rt_map_oracle_add((uintptr_t)p, (size_t)n);
  raw_ptr_register((uintptr_t)p, (size_t)n, true);
  return (int64_t)(uintptr_t)p;
}

int64_t rt_ptr_key(int64_t ptr) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "0x%016" PRIx64, (uint64_t)(uintptr_t)ptr);
  if (n <= 0)
    return rt_alloc_string("0x0000000000000000");
  if ((size_t)n >= sizeof(buf))
    n = (int)sizeof(buf) - 1;
  return rt_alloc_string_len(buf, (size_t)n);
}

static int64_t rt_free_direct(int64_t ptr) {
  ptr = sh_normalize(ptr);
  if (g_sh_cap) {
    sh_check_double_free((uintptr_t)ptr);
    if (sh_owns((uintptr_t)ptr)) {
      if (is_heap_ptr(ptr))
        rt_managed_ptr_unregister((uintptr_t)ptr);
      else
        raw_ptr_unregister((uintptr_t)ptr);
      return sh_release((uintptr_t)ptr) > 0;
    }
  }
  /*
   * Invalidate interned-string entries before the pool can recycle this
   * address. Keep this in the common release path so rt_release_owned and
   * ordinary rt_free obey the same lifetime rule.
   */
  rt_str_forget_pointer(ptr);
  if (is_heap_ptr(ptr)) {
    rt_managed_ptr_unregister((uintptr_t)ptr);
    void *base = (char *)(uintptr_t)ptr - 32;
    if (mem_trace_enabled()) {
      size_t body = *(uint64_t *)((char *)base + 8);
      if (body & 1)
        body >>= 1;
      if (body > 1024 * 1024) {
        fprintf(stderr, "[mem] large free  %p (body=%zu)\n",
                (void *)(uintptr_t)ptr, body);
      }
    }
    size_t body = *(uint64_t *)((char *)base + 8);
    if (body & 1)
      body >>= 1;
    size_t total = ((body + 15) & ~15ULL) + 32;
    int slot = ny_mem_pool_slot(total);
    if (slot >= 0) {
      mem_pool_node_t *node = (mem_pool_node_t *)base;
      node->next = g_mem_pools[slot];
      g_mem_pools[slot] = node;
      return 1;
    }
#ifndef _WIN32
    rt_oracle_before_release(base);
#endif
    ny_aligned_free(base);
    return 1;
  } else if (raw_ptr_unregister((uintptr_t)ptr)) {
    rt_oracle_free((void *)(uintptr_t)ptr);
    return 1;
  } else if (is_v_flt(ptr)) {
    rt_flt_free(ptr);
    return 1;
  }
  return 0;
}

#ifdef __linux__
#include <fcntl.h>
/*
 * Writing a byte from `p` into a pipe makes the kernel read it the way the
 * program would: device memory mapped with VM_IO/VM_PFNMAP copies fine,
 * while an unmapped or PROT_NONE page fails with EFAULT. The byte is read
 * back at once; with several threads probing, any byte drains any other.
 */
int rt_addr_probe_copy(uintptr_t p) {
  static _Atomic int fds_state = 0; /* 0 = unopened, 1 = open, -1 = failed */
  static int fds[2] = {-1, -1};
  static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;
  unsigned char vec = 0;
  /*
   * Unmapped is the common failure; answer it without the pipe.
   */
  if (mincore((void *)(p & ~(uintptr_t)4095u), 4096, &vec) != 0)
    return 0;
  int state = atomic_load_explicit(&fds_state, memory_order_acquire);
  if (state == 0) {
    pthread_mutex_lock(&open_lock);
    state = atomic_load_explicit(&fds_state, memory_order_relaxed);
    if (state == 0) {
      state = pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0 ? 1 : -1;
      atomic_store_explicit(&fds_state, state, memory_order_release);
    }
    pthread_mutex_unlock(&open_lock);
  }
  if (state < 0)
    return 0; /* keep the old, conservative answer */
  int saved_errno = errno;
  int ok = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    ssize_t wrote = write(fds[1], (const void *)p, 1);
    if (wrote == 1) {
      ok = 1;
      break;
    }
    if (errno != EAGAIN)
      break;
    /*
     * Full from other threads' bytes: drain and try once more.
     */
    unsigned char sink[256];
    while (read(fds[0], sink, sizeof(sink)) > 0) {
    }
  }
  if (ok) {
    unsigned char back;
    (void)read(fds[0], &back, 1);
  }
  errno = saved_errno;
  return ok;
}
#endif

/*
 * Bumped by every free so per-thread address caches can drop stale bases.
 */
_Atomic uint64_t rt_mem_free_epoch = 0;

int64_t rt_free(int64_t ptr) {
  atomic_fetch_add_explicit(&rt_mem_free_epoch, 1, memory_order_release);
  ptr = sh_normalize(ptr);
  if (g_sh_cap)
    sh_check_double_free((uintptr_t)ptr);
  if (is_heap_ptr(ptr))
    rt_rc_forget(ptr);
  return rt_free_direct(ptr);
}

int64_t rt_free_raw(int64_t ptr) {
  atomic_fetch_add_explicit(&rt_mem_free_epoch, 1, memory_order_release);
  ptr = sh_normalize(ptr);
  if (!ptr)
    return 1;
  if (g_sh_cap) {
    sh_check_double_free((uintptr_t)ptr);
    if (sh_owns((uintptr_t)ptr)) {
      if (is_heap_ptr(ptr))
        rt_managed_ptr_unregister((uintptr_t)ptr);
      else
        raw_ptr_unregister((uintptr_t)ptr);
      return sh_release((uintptr_t)ptr) > 0;
    }
  }
  if (raw_ptr_unregister((uintptr_t)ptr)) {
    rt_oracle_free((void *)(uintptr_t)ptr);
    return 1;
  }
  int tbuf_release = rt_tbuf_release_handle(ptr);
  if (tbuf_release > 0) {
    rt_oracle_free((void *)((uintptr_t)ptr - RT_NATIVE_TBUF_HEADER));
    return 1;
  }
  if (tbuf_release < 0) {
    fprintf(stderr,
            "[mem] free_raw ignored a statically registered typed buffer\n");
    return 0;
  }
  /*
   * Provenance comes from allocator registries; pointer contents are never
   * consulted to distinguish the two allocation families.
   */
  if (is_heap_ptr(ptr)) {
    fprintf(stderr,
            "[mem] cross-system free: GC-traced pointer passed to free_raw; "
            "rerouting to rt_free\n");
    return rt_free(ptr);
  }
  return 0;
}

int64_t rt_retain_owned(int64_t ptr) {
  if (!rt_rc_enabled() || !is_heap_ptr(ptr))
    return ptr;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (entry) {
    entry->count++;
  } else {
    entry = (ny_rc_entry_t *)calloc(1, sizeof(*entry));
    if (entry) {
      entry->ptr = p;
      entry->count = 1;
      entry->next = *slot;
      *slot = entry;
    }
  }
  rt_rc_unlock();
  return ptr;
}

int64_t rt_release_owned(int64_t ptr) {
  if (!rt_rc_enabled())
    return rt_free(ptr);
  if (!is_heap_ptr(ptr)) {
    if (is_v_flt(ptr))
      return rt_free(ptr);
    return 0;
  }
  uintptr_t p = (uintptr_t)ptr;
  bool should_free = false;
  rt_rc_lock();
  ny_rc_entry_t **slot = NULL;
  ny_rc_entry_t *entry = rt_rc_find_locked(p, &slot);
  if (!entry) {
    should_free = true;
  } else if (entry->count <= 1) {
    *slot = entry->next;
    free(entry);
    should_free = true;
  } else {
    entry->count--;
  }
  rt_rc_unlock();
  return should_free ? rt_free_direct(ptr) : 1;
}

int64_t rt_rc_count(int64_t ptr) {
  if (!rt_rc_enabled() || !is_heap_ptr(ptr))
    return 0;
  uint64_t count = 0;
  uintptr_t p = (uintptr_t)ptr;
  rt_rc_lock();
  ny_rc_entry_t *entry = rt_rc_find_locked(p, NULL);
  if (entry)
    count = entry->count;
  rt_rc_unlock();
  return (int64_t)((count << 1) | 1u);
}

int64_t rt_drop_owned(int64_t ptr) { return rt_release_owned(ptr); }

int64_t rt_drop_owned_slot(int64_t slot_ptr) {
  if (!is_ptr(slot_ptr))
    return 0;
  int64_t *slot = (int64_t *)(uintptr_t)slot_ptr;
  int64_t v = *slot;
  *slot = 0;
  return rt_drop_owned(v);
}

int64_t rt_pool_release(void) {
  int64_t freed_count = 0;
  for (size_t i = 0; i < NUM_POOLS; i++) {
    mem_pool_node_t *node = g_mem_pools[i];
    g_mem_pools[i] = NULL;
    while (node) {
      mem_pool_node_t *next = node->next;
      if (rt_block_chunk_owns((uintptr_t)node))
        ; /* carved from a bump chunk; released with the chunk */
      else if (rt_fc_pool_owns(node))
        rt_fc_block_free(node, g_pool_sizes[i]);
      else
        ny_aligned_free(node);
      freed_count++;
      node = next;
    }
  }
  return rt_tag_v(freed_count);
}

/*
 * Native AOT entries can return directly to the host CRT. Report requested
 * counters at process exit as well as explicit runtime cleanup, once only.
 */
static atomic_bool g_ny_counter_reported = ATOMIC_VAR_INIT(false);
static void rt_report_runtime_counters(void) {
  const char *counter_report = getenv("NYTRIX_REPORT_RUNTIME_COUNTERS");
  if (counter_report && *counter_report && strcmp(counter_report, "0") != 0 &&
      strcmp(counter_report, "false") != 0 &&
      strcmp(counter_report, "off") != 0) {
    if (atomic_exchange_explicit(&g_ny_counter_reported, true,
                                 memory_order_relaxed))
      return;
    uint_fast64_t allocs =
        atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
    uint_fast64_t reallocs =
        atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed);
    fprintf(stderr,
            "ny_runtime_counters alloc=%" PRIuFAST64 " realloc=%" PRIuFAST64
            "\n",
            allocs, reallocs);
  }
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void
rt_report_runtime_counters_at_exit(void) {
  rt_report_runtime_counters();
}
#endif

int64_t rt_runtime_cleanup(void) {
  extern int64_t rt_print_flush(void);
  rt_print_flush();
  rt_async_cleanup();
  rt_report_runtime_counters();
  const char *emit_report = getenv("NYTRIX_EMIT_REPORT");
  if (emit_report && *emit_report) {
    FILE *out = fopen(emit_report, "ab");
    if (out) {
      extern atomic_uint_fast64_t g_ny_gc_alloc_count;
      uint_fast64_t allocs =
          atomic_load_explicit(&g_ny_alloc_count, memory_order_relaxed);
      uint_fast64_t gc_allocs =
          atomic_load_explicit(&g_ny_gc_alloc_count, memory_order_relaxed);
      allocs = UINT_FAST64_MAX - allocs < gc_allocs ? UINT_FAST64_MAX
                                                    : allocs + gc_allocs;
      fprintf(out, "runtime.allocation_calls=%" PRIuFAST64 "\n", allocs);
      fprintf(out, "runtime.reallocation_calls=%" PRIuFAST64 "\n",
              atomic_load_explicit(&g_ny_realloc_count, memory_order_relaxed));
      rt_trace_profile_write(out);
      nyGcWriteTelemetry(out);
      fclose(out);
    } else {
      fprintf(stderr, "runtime report: cannot append %s: %s\n", emit_report,
              strerror(errno));
    }
  }
  const char *mem_stats = getenv("NYTRIX_MEM_STATS");
  if (mem_stats && *mem_stats && strcmp(mem_stats, "0") != 0 &&
      strcmp(mem_stats, "false") != 0 && strcmp(mem_stats, "off") != 0)
    nyGcWriteTelemetry(stderr);
  rt_cleanup_args();
  rt_cleanup_small_strings();
  rt_pool_release();
  return 0;
}

int64_t rt_realloc(int64_t p_val, int64_t newsz) {
  RT_UNTAG_INT(newsz);
  if (newsz < 0)
    newsz = 0;
  if (!is_heap_ptr(p_val))
    return rt_malloc(newsz << 1 | 1);
  size_t old_cap = *(uint64_t *)((char *)(uintptr_t)p_val - 24);
  if (old_cap & 1)
    old_cap >>= 1;
  if ((size_t)newsz <= old_cap)
    return p_val;

  /*
   * A safe-heap block has no pool class: grow by moving, so the old mapping
   * is released (quarantined) and a stale pointer to it faults.
   */
  if (g_sh_cap && sh_owns((uintptr_t)p_val)) {
    int64_t moved = sh_traced_alloc((size_t)newsz);
    if (!moved)
      return 0;
    memcpy((void *)(uintptr_t)moved, (void *)(uintptr_t)p_val, old_cap);
    *(int64_t *)((char *)(uintptr_t)moved - 16) =
        *(int64_t *)((char *)(uintptr_t)p_val - 16);
    *(int64_t *)((char *)(uintptr_t)moved - 8) =
        *(int64_t *)((char *)(uintptr_t)p_val - 8);
    atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
    rt_free(p_val);
    return moved;
  }

  /*
   * A pooled allocation owns the whole size-class chunk, even though the
   * public heap-size header records only the requested body.  Growth that
   * remains in that same class is therefore genuinely in place: publish the
   * larger logical capacity and zero the newly exposed bytes when required.
   */
  size_t old_total = ((old_cap + 15) & ~15ULL) + 32;
  size_t new_body = ((size_t)newsz + 15) & ~15ULL;
  size_t new_total = new_body + 32;
  int old_slot = ny_mem_pool_slot(old_total);
  int new_slot = ny_mem_pool_slot(new_total);
  if (old_slot >= 0 && old_slot == new_slot) {
    if (rt_zero_init_enabled())
      memset((char *)(uintptr_t)p_val + old_cap, 0, (size_t)newsz - old_cap);
    *(uint64_t *)((char *)(uintptr_t)p_val - 24) =
        (uint64_t)((new_body << 1) | 1u);
    atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
    return p_val;
  }

  int64_t res = rt_malloc(newsz << 1 | 1);
  if (!res)
    return 0;

  memcpy((void *)(uintptr_t)res, (void *)(uintptr_t)p_val, old_cap);

  *(int64_t *)((char *)(uintptr_t)res - 16) =
      *(int64_t *)((char *)(uintptr_t)p_val - 16);
  *(int64_t *)((char *)(uintptr_t)res - 8) =
      *(int64_t *)((char *)(uintptr_t)p_val - 8);

  atomic_fetch_add_explicit(&g_ny_realloc_count, 1, memory_order_relaxed);
  rt_free(p_val);
  return res;
}

int64_t rt_realloc_raw(int64_t p, int64_t size) {
  if (size <= 0) {
    if (is_heap_ptr(p))
      rt_free(p);
    else
      rt_free_raw(p);
    return 0;
  }
  /*
   * Explicit __malloc users retain managed allocation ownership.
   */
  if (is_heap_ptr(p))
    return rt_realloc(p, rt_tag_v(size < 0 ? 0 : size));
  if (g_sh_cap && sh_owns((uintptr_t)p)) {
    size_t old = sh_size_of((uintptr_t)p);
    int64_t moved = rt_malloc_raw_i64(size);
    if (!moved)
      return 0;
    memcpy((void *)(uintptr_t)moved, (void *)(uintptr_t)p,
           old < (size_t)size ? old : (size_t)size);
    rt_free_raw(p);
    return moved;
  }
  /*
   * Native malloc/free own ordinary C allocations, without the managed
   * heap prefix. Keep realloc in that same allocation family and preserve
   * the old bytes; rt_realloc would replace an untracked pointer with a
   * fresh managed allocation that rt_zfree_raw cannot release.
   */
  if (is_int(p) || (p && (p < 4096 || !rt_addr_readable((uintptr_t)p, 1))))
    return 0;
  if (__builtin_expect(rt_oom_inject(), 0))
    return 0;
  if (p && !rt_raw_ptr_is_allocation_base(p)) {
    /*
     * An addressable pointer outside both registries is an ordinary libc
     * allocation (e.g. the result of an `extern` calloc). libc realloc
     * accepts it directly; failing here made every realloc of an
     * extern-allocated buffer return NULL on every route
     * (alloc-provenance oracle).
     */
    void *foreign = realloc((void *)(uintptr_t)p, (size_t)size);
    if (foreign)
      rt_raw_ptr_register_range((int64_t)(uintptr_t)foreign, (size_t)size);
    return (int64_t)(uintptr_t)foreign;
  }
  void *grown = rt_oracle_realloc((void *)(uintptr_t)p, (size_t)size);
  if (grown) {
    if (p)
      raw_ptr_unregister((uintptr_t)p);
    rt_raw_ptr_register_range((int64_t)(uintptr_t)grown, (size_t)size);
  }
  return (int64_t)(uintptr_t)grown;
}

int64_t rt_memcpy(int64_t dst, int64_t src, int64_t n) {
  RT_UNTAG_INT(n);
  if (n <= 0)
    return dst;
  if (!rt_check_oob("memcpy_dst", dst, 0, (size_t)n))
    return dst;
  if (!rt_check_oob("memcpy_src", src, 0, (size_t)n))
    return dst;
  rt_copy_mem(dst, src, (n << 1) | 1);
  return dst;
}

int64_t rt_memset(int64_t dst, int64_t v, int64_t n) {
  RT_UNTAG_INT(v);
  RT_UNTAG_INT(n);
  if (n > 0) {
    unsigned char *p = (unsigned char *)(uintptr_t)dst;
    unsigned char c = (unsigned char)(v & 0xff);
    while (n-- > 0)
      *p++ = c;
  }
  return dst;
}

int64_t rt_memcmp(int64_t a, int64_t b, int64_t n) {
  RT_UNTAG_INT(n);
  if (n <= 0)
    return 0;
  int res = 0;
  const char *s1 = (const char *)(uintptr_t)a;
  const char *s2 = (const char *)(uintptr_t)b;
  for (size_t i = 0; i < (size_t)n; ++i) {
    if (s1[i] != s2[i]) {
      res = s1[i] - s2[i];
      break;
    }
  }
  /*
   * Keep the libc-compatible raw comparison result.  This primitive is used
   * as a byte-level predicate and its public contract is the conventional
   * negative/zero/positive result; tagging here makes equal buffers appear as
   * one to native callers while the legacy backend returns zero.
   */
  return (int64_t)res;
}

/*
 * Native NYIR passes scalar arguments unboxed. Keep that ABI explicit rather
 * than guessing whether an odd length is raw or tagged at runtime.
 */
int64_t rt_memcpy_raw(int64_t dst, int64_t src, int64_t n) {
  if (n <= 0)
    return dst;
  if (!rt_check_oob("memcpy_dst", dst, 0, (size_t)n) ||
      !rt_check_oob("memcpy_src", src, 0, (size_t)n))
    return dst;
  rt_copy_mem(dst, src, (n << 1) | 1);
  return dst;
}

int64_t rt_memset_raw(int64_t dst, int64_t v, int64_t n) {
  if (n <= 0)
    return dst;
  unsigned char *p = (unsigned char *)(uintptr_t)dst;
  unsigned char c = (unsigned char)(v & 0xff);
  while (n-- > 0)
    *p++ = c;
  return dst;
}

int64_t rt_memcmp_raw(int64_t a, int64_t b, int64_t n) {
  if (n <= 0)
    return 0;
  const unsigned char *s1 = (const unsigned char *)(uintptr_t)a;
  const unsigned char *s2 = (const unsigned char *)(uintptr_t)b;
  for (size_t i = 0; i < (size_t)n; ++i) {
    if (s1[i] != s2[i])
      return (int64_t)s1[i] - (int64_t)s2[i];
  }
  return 0;
}

static inline int rt_try_load8_str(int64_t addr, int64_t idx, int64_t *out) {
  if (!out || idx < 0 || !is_v_str(addr))
    return 0;
  uintptr_t lp = (uintptr_t)addr - 16u;
  if (!rt_addr_readable(lp, sizeof(int64_t)))
    return 0;
  int64_t tagged_len = 0;
  memcpy(&tagged_len, (const void *)lp, sizeof(tagged_len));
  if (!is_int(tagged_len))
    return 0;
  size_t len = (size_t)(tagged_len >> 1);
  if ((size_t)idx >= len)
    return 0;
  *out =
      (((int64_t)*((const uint8_t *)(uintptr_t)addr + (size_t)idx)) << 1) | 1;
  return 1;
}

int64_t rt_load8_idx(int64_t addr, int64_t idx) {
  RT_UNTAG_INT(idx);
  int64_t str_val = 0;
  if (rt_try_load8_str(addr, idx, &str_val))
    return str_val;
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 1 > sz)
      return 1;
  } else if (!rt_check_oob("load8", addr, idx, 1)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_access_ok(p, 1))
    return 1;
  int64_t val = (((int64_t)*(uint8_t *)p) << 1) | 1;
  return val;
}

/*
 * Safe byte access for a dynamic string value. Native lowering passes the
 * string representation directly, while rt_any_to_cstr accepts both the
 * tagged VM representation and native string pointers.
 */
int64_t rt_str_byte(int64_t value, int64_t index) {
  if (index < 0)
    return 0;
  if (!is_v_str(value))
    return 0;
  const char *s = (const char *)(uintptr_t)value;
  size_t len = (size_t)rt_cstr_len(value);
  if ((size_t)index >= len)
    return 0;
  return (unsigned char)s[index];
}

int64_t rt_load16_idx(int64_t addr, int64_t idx) {
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 2 > sz)
      return 1;
  } else if (!rt_check_oob("load16", addr, idx, 2)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_access_ok(p, 2))
    return 1;
  uint16_t v = 0;
  if ((p & (uintptr_t)1u) == 0) {
    v = *(const uint16_t *)p;
  } else {
    memcpy(&v, (const void *)p, sizeof(v));
  }
  return (((int64_t)v) << 1) | 1;
}

int64_t rt_load32_idx(int64_t addr, int64_t idx) {
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 1;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 4 > sz)
      return 1;
  } else if (!rt_check_oob("load32", addr, idx, 4)) {
    return 1;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 1;
  if (!hdr && !heap && !rt_addr_access_ok(p, 4))
    return 1;
  uint32_t v = 0;
  if ((p & (uintptr_t)3u) == 0) {
    v = *(const uint32_t *)p;
  } else {
    memcpy(&v, (const void *)p, sizeof(v));
  }
  return (((int64_t)v) << 1) | 1;
}

double rt_load32_f64(int64_t addr, int64_t idx) {
  int64_t tagged = rt_load32_idx(addr, idx);
  uint32_t bits = (uint32_t)(rt_untag_v(tagged) & UINT32_MAX);
  float value;
  memcpy(&value, &bits, sizeof(value));
  return (double)value;
}

int64_t rt_store32_f64(int64_t addr, int64_t idx, double value) {
  float narrowed = (float)value;
  uint32_t bits = 0;
  memcpy(&bits, &narrowed, sizeof(bits));
  return rt_store32_idx(addr, idx, rt_tag_v((int64_t)bits));
}

int64_t rt_load64_idx(int64_t addr, int64_t idx) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = rt_get_heap_size_known(addr);
      if ((size_t)off + 8 > sz)
        return 0;
    } else if (off < -32) {
      return 0;
    }
    uintptr_t p = (uintptr_t)((intptr_t)addr + off);
    if (p < 0x1000)
      return 0;
    if ((p & (uintptr_t)7u) == 0) {
      return *(const int64_t *)p;
    }
    int64_t v = 0;
    memcpy(&v, (const void *)p, sizeof(v));
    return v;
  }
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return 0;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return 0;
  } else if (!rt_check_oob("load64", addr, idx, 8)) {
    return 0;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return 0;
  if (!hdr && !heap && !rt_addr_access_ok(p, 8))
    return 0;
  if ((p & (uintptr_t)7u) == 0) {
    return *(const int64_t *)p;
  }
  int64_t v = 0;
  memcpy(&v, (const void *)p, sizeof(v));
  return v;
}

int64_t rt_store8_idx(int64_t addr, int64_t idx, int64_t val) {
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 1 > sz)
      return val;
  } else if (!rt_check_oob("store8", addr, idx, 1)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_access_ok(p, 1))
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  *(uint8_t *)p = (uint8_t)v;
  return val;
}

int64_t rt_store16_idx(int64_t addr, int64_t idx, int64_t val) {
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 2 > sz)
      return val;
  } else if (!rt_check_oob("store16", addr, idx, 2)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_access_ok(p, 2))
    return val;
  int64_t v = (val & 1) ? (val >> 1) : val;
  uint16_t raw = (uint16_t)v;
  if ((p & (uintptr_t)1u) == 0) {
    *(uint16_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

/*
 * Pages that raw stores have found readable, per thread, for stores through
 * pointers into foreign memory. Vertex writes into a mapped GPU buffer pass
 * a different pointer for every vertex and walk the buffer page by page;
 * each store repeated the heap-registry lookup and a readability probe that
 * costs a syscall for memory mapped after the oracle's snapshot (one fifth
 * of a UI frame). A store may skip both when:
 *  - the pointer's page is not registered with the map oracle: every
 *    managed, list and dictionary allocation registers its pages, so no
 *    runtime object, and no bounds to check, can begin there (the safe heap
 *    and the GC heap do not register theirs; the shortcut is off while
 *    either is in use);
 *  - every page written was found readable by a full check on this thread
 *    recently. Entries are dropped after a bounded number of uses, as in the
 *    probe cache, so a page unmapped since is noticed.
 */
#define RT_RAW_STORE_PAGES 64u
#define RT_RAW_STORE_REVALIDATE 65536u
static __thread uintptr_t rt_raw_store_pages[RT_RAW_STORE_PAGES];
static __thread uint32_t rt_raw_store_uses;

static inline bool rt_raw_store_page_cached(uintptr_t page) {
  return rt_raw_store_pages[page & (RT_RAW_STORE_PAGES - 1u)] == page + 1u;
}

static inline bool rt_raw_store_fast(int64_t addr, int64_t idx, size_t n) {
  uintptr_t a = (uintptr_t)addr;
  uintptr_t q = a + (uintptr_t)idx;
  if (idx < 0 || a < 0x1000 || q < a || q > UINTPTR_MAX - n)
    return false;
  if (__builtin_expect(++rt_raw_store_uses >= RT_RAW_STORE_REVALIDATE, 0)) {
    memset(rt_raw_store_pages, 0, sizeof(rt_raw_store_pages));
    rt_raw_store_uses = 0;
    return false;
  }
  uintptr_t first = q >> 12, last = (q + n - 1u) >> 12;
  if (!rt_raw_store_page_cached(first) ||
      (last != first && !rt_raw_store_page_cached(last)))
    return false;
  if (g_sh_cap ||
      atomic_load_explicit(&g_rt_gc_heap_used, memory_order_relaxed))
    return false;
  return !rt_oracle_page_known(a >> 12);
}

/*
 * After a full check passed for a non-object pointer: remember the pages.
 */
static inline void rt_raw_store_note(uintptr_t q, size_t n) {
  uintptr_t first = q >> 12, last = (q + n - 1u) >> 12;
  for (uintptr_t pg = first; pg <= last; ++pg)
    rt_raw_store_pages[pg & (RT_RAW_STORE_PAGES - 1u)] = pg + 1u;
}

int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val) {
  RT_UNTAG_INT(idx);
  if (rt_raw_store_fast(addr, idx, 4)) {
    int64_t v = (val & 1) ? (val >> 1) : val;
    uint32_t raw = (uint32_t)v;
    memcpy((void *)((uintptr_t)addr + (uintptr_t)idx), &raw, sizeof(raw));
    return val;
  }
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 4 > sz)
      return val;
  } else if (!rt_check_oob("store32", addr, idx, 4)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_access_ok(p, 4))
    return val;
  if (!hdr && !heap)
    rt_raw_store_note(p, 4);
  int64_t v = (val & 1) ? (val >> 1) : val;
  uint32_t raw = (uint32_t)v;
  if ((p & (uintptr_t)3u) == 0) {
    *(uint32_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

int64_t rt_store64_idx(int64_t addr, int64_t idx, int64_t val) {
  if (is_int(idx) && is_heap_ptr(addr)) {
    intptr_t off = (intptr_t)(idx >> 1);
    if (off >= 0) {
      size_t sz = rt_get_heap_size_known(addr);
      if ((size_t)off + 8 > sz)
        return val;
    } else if (off < -32) {
      return val;
    }
    uintptr_t p = (uintptr_t)((intptr_t)addr + off);
    if (p < 0x1000)
      return val;
    int64_t raw = val;
    if (off == -8 && is_int(raw)) {
      int64_t u = raw >> 1;
      if (u >= 100 && u <= 255)
        raw = u;
    } else if (off == -16 && !is_int(raw)) {
      raw = rt_tag_v(raw);
    }
    if ((p & (uintptr_t)7u) == 0) {
      *(int64_t *)p = raw;
    } else {
      memcpy((void *)p, &raw, sizeof(raw));
    }
    return val;
  }
  RT_UNTAG_INT(idx);
  bool hdr = (idx < 0);
  bool heap = is_heap_ptr(addr);
  if (hdr) {
    if ((intptr_t)idx < -32)
      return val;
  } else if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return val;
  } else if (!rt_check_oob("store64", addr, idx, 8)) {
    return val;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000)
    return val;
  if (!hdr && !heap && !rt_addr_access_ok(p, 8))
    return val;
  int64_t raw = val;
  if (idx == -8 && is_int(raw)) {
    int64_t u = raw >> 1;
    if (u >= 100 && u <= 255)
      raw = u;
  } else if (idx == -16 && !is_int(raw)) {
    raw = rt_tag_v(raw);
  }
  if ((p & (uintptr_t)7u) == 0) {
    *(int64_t *)p = raw;
  } else {
    memcpy((void *)p, &raw, sizeof(raw));
  }
  return val;
}

int64_t rt_load64_h(int64_t p, int64_t i) {
  int64_t raw = rt_load64_idx(p, i);
  return rt_tag_v(raw);
}

int64_t rt_load32_h(int64_t p, int64_t i) { return rt_load32_idx(p, i); }

int64_t rt_store64_h(int64_t p, int64_t i, int64_t v) {
  int64_t raw = rt_untag_v(v);
  return rt_store64_idx(p, i, raw);
}

static _Atomic int64_t *rt_atomic_i64_slot(int64_t addr, int64_t idx) {
  RT_UNTAG_INT(idx);
  if ((intptr_t)idx < 0)
    return NULL;
  bool heap = is_heap_ptr(addr);
  if (heap) {
    size_t sz = rt_get_heap_size_known(addr);
    if ((size_t)idx + 8 > sz)
      return NULL;
  } else if (!rt_check_oob("atomic64", addr, idx, 8)) {
    return NULL;
  }
  uintptr_t p = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
  if (p < 0x1000 || (p & (uintptr_t)7u) != 0)
    return NULL;
  if (!heap && !rt_addr_access_ok(p, 8))
    return NULL;
  return (_Atomic int64_t *)p;
}

static inline int64_t rt_atomic_i64_delta(int64_t value) {
  /*
   * Cells hold TAGGED language words: every other operation in this family
   * (constructor, __atomic_store64, exchange, CAS) passes language words
   * through unmodified, so a tagged integer cell of n is the word (2n+1).
   * A raw fetch_add of the untagged magnitude would clear the tag bit and
   * desynchronise the payload (cell(0)=1 +1 -> 2, which reads back as the
   * false immediate instead of 1). Adding the doubled magnitude preserves
   * the tag: (2n+1) + 2k == 2(n+k)+1.
   */
  return is_int(value) ? (int64_t)((uint64_t)rt_untag_v(value) << 1) : value;
}

int64_t rt_atomic_load64(int64_t addr, int64_t idx) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_load_explicit(slot, memory_order_seq_cst);
}

int64_t rt_atomic_store64(int64_t addr, int64_t idx, int64_t value) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return value;
  atomic_store_explicit(slot, value, memory_order_seq_cst);
  return value;
}

int64_t rt_atomic_add64(int64_t addr, int64_t idx, int64_t delta) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_fetch_add_explicit(slot, rt_atomic_i64_delta(delta),
                                   memory_order_seq_cst);
}

int64_t rt_atomic_sub64(int64_t addr, int64_t idx, int64_t delta) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_fetch_sub_explicit(slot, rt_atomic_i64_delta(delta),
                                   memory_order_seq_cst);
}

int64_t rt_atomic_exchange64(int64_t addr, int64_t idx, int64_t value) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return 0;
  return atomic_exchange_explicit(slot, value, memory_order_seq_cst);
}

int64_t rt_atomic_cas64(int64_t addr, int64_t idx, int64_t expected,
                        int64_t desired) {
  _Atomic int64_t *slot = rt_atomic_i64_slot(addr, idx);
  if (!slot)
    return NY_IMM_FALSE;
  int64_t exp = expected;
  return atomic_compare_exchange_strong_explicit(
             slot, &exp, desired, memory_order_seq_cst, memory_order_seq_cst)
             ? NY_IMM_TRUE
             : NY_IMM_FALSE;
}

/*
 * Heap collector.
 *
 * Lists, dictionaries and strings have no owner in the language: a function
 * returns a fresh list, the caller reads four numbers from it and drops it.
 * Without a collector every such temporary stays allocated, which is harmless
 * in a batch program and unbounded in a program that draws frames.
 *
 * The program calls rt_heap_collect at a point where its own stack is
 * shallow (the end of a frame). The first call starts recording the objects
 * the calling thread allocates ("young" objects). A later call frees every
 * young object that no memory refers to and forgets the rest ("old").
 *
 * Reachability is conservative: any aligned word that holds an address
 * inside a young object, directly or as a tagged integer, keeps it alive. No
 * compiler support is needed, so the collector is correct for code that keeps
 * objects in raw buffers, in C structures, or in registers.
 *
 * A collection does not read the whole process. Linux records which pages
 * were written since the last request (soft-dirty bits), and a reference to
 * a young object can only live on such a page: the object did not exist
 * when the others were last written. Young objects themselves are skipped
 * until something else reaches them. Every so often a full collection scans
 * all writable memory and also reconsiders the old objects.
 *
 * Other threads started by the runtime are parked with a signal for the
 * duration of the scan; their registers are on their stacks while parked.
 * Platforms without this machinery never start recording and behave as
 * before.
 */
__thread int g_rt_fc_tracking = 0;

#if defined(__linux__) && defined(__GLIBC__) && !NY_WITH_ASAN

#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#if defined(PAGEMAP_SCAN) && defined(UFFD_FEATURE_WP_ASYNC) &&                 \
    defined(UFFD_USER_MODE_ONLY) && defined(SYS_userfaultfd)
#define FC_HAVE_WRITE_TRACKING 1
#else
#define FC_HAVE_WRITE_TRACKING 0
#endif
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <ucontext.h>

typedef struct fc_obj {
  uintptr_t ptr;
  uint32_t kind;
  uint32_t mark;
} fc_obj_t;

typedef struct fc_range {
  uintptr_t start;
  uintptr_t end;
  uint32_t obj;
} fc_range_t;

/*
 * The memory one candidate owns: one block, or two for a dictionary.
 */
typedef struct fc_extent {
  uintptr_t r[4];
  uint32_t count;
} fc_extent_t;

typedef struct fc_buf {
  void *data;
  size_t bytes;
} fc_buf_t;

enum {
  FC_BUF_YOUNG,
  FC_BUF_OLD,
  FC_BUF_CAND,
  FC_BUF_RANGES,
  FC_BUF_STACK,
  FC_BUF_MAPS,
  FC_BUF_PAGE,
  FC_BUF_PAGEMAP,
  FC_BUF_EXTENTS,
  FC_BUF_SORT,
  FC_BUF_FILTER,
  FC_BUF_PENDING,
  FC_BUF_PAGEBITS,
  FC_BUF_WRITTEN,     /* pages written since the collector started */
  FC_BUF_WRITTEN_NEW, /* pages the current pass found written */
  FC_BUF_WRITTEN_TMP, /* merge scratch */
  FC_BUF_COUNT
};

#define FC_PAGE_BATCH 64u
#define FC_MAX_EXCLUDES 320u
#define FC_MAX_THREADS 256u
#define FC_LARGE_OBJECT ((size_t)8 << 20)

static fc_buf_t fc_bufs[FC_BUF_COUNT];
static size_t fc_young_n, fc_old_n, fc_old_after_major;
static size_t fc_pending_n, fc_pending_pos; /* unreachable, not yet freed */
static size_t fc_young_bytes, fc_promoted_bytes;
static size_t fc_minor_threshold = (size_t)2 << 20;
static size_t fc_major_threshold = (size_t)48 << 20;
static size_t fc_young_limit = (size_t)256 << 20;
static uint64_t fc_last_end_ns;
static uint64_t fc_kernel_ns; /* kernel time of the last collection */
static int fc_state;      /* 0 = not started, 1 = recording, -1 = disabled */
static int fc_soft_dirty; /* 0 = untested, 1 = usable, -1 = unavailable */
static int fc_in_collect;
static uint64_t fc_stat_collections, fc_stat_major, fc_stat_freed_objects,
    fc_stat_freed_bytes, fc_stat_last_ns, fc_stat_max_ns;
static size_t fc_trace_pages, fc_trace_words;
static int fc_trace_level;
static uint64_t fc_trace_t[6];
static uintptr_t
    fc_stack_low; /* lowest address of the recording thread's stack */
static uintptr_t fc_excludes[FC_MAX_EXCLUDES][2];
static size_t fc_exclude_n;

static pthread_mutex_t fc_threads_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t fc_threads[FC_MAX_THREADS];
static size_t fc_thread_n;
static int fc_thread_overflow;
static _Atomic int fc_parking;
static _Atomic int fc_parked;
static int fc_signal;

static bool fc_buf_reserve(int which, size_t bytes) {
  fc_buf_t *b = &fc_bufs[which];
  if (b->bytes >= bytes)
    return true;
  size_t want = b->bytes ? b->bytes : ((size_t)1 << 16);
  while (want < bytes) {
    if (want > SIZE_MAX / 2)
      return false;
    want *= 2;
  }
  void *fresh = mmap(NULL, want, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (fresh == MAP_FAILED)
    return false;
  if (b->data) {
    memcpy(fresh, b->data, b->bytes);
    munmap(b->data, b->bytes);
  }
  b->data = fresh;
  b->bytes = want;
  return true;
}

/*
 * Collector heap. Blocks for lists and dictionaries come from mapped chunks
 * that are never returned to the system, in fixed size classes with one free
 * list each. Three things follow that the C allocator cannot give:
 * a free block is all zero except one link word stored complemented, so
 * freed memory never looks like a reference; addresses are high, where no
 * small integer or text fragment collides with them; and the pages stay
 * mapped, so what the runtime's address oracle learned stays true.
 * Only the recording thread allocates; any thread may free.
 */
#define FC_CHUNK_BYTES ((size_t)4 << 20)
#define FC_POOL_MAX ((size_t)64 << 10)
#define FC_POOL_CLASSES 112u
#define FC_MAX_CHUNKS 1024u
static uintptr_t fc_chunks[FC_MAX_CHUNKS];
static _Atomic size_t fc_chunk_n;
static uintptr_t fc_chunk_lo = UINTPTR_MAX, fc_chunk_hi;
static uintptr_t fc_bump, fc_bump_end;
static uintptr_t fc_pool_free[FC_POOL_CLASSES];
static _Atomic(uintptr_t) fc_foreign_free;

/*
 * 16-byte steps to 1 KiB, then eight steps per doubling.
 */
static inline unsigned fc_pool_class(size_t bytes, size_t *rounded) {
  if (bytes <= 1024) {
    size_t r = (bytes + 15u) & ~(size_t)15u;
    if (r < 16)
      r = 16;
    *rounded = r;
    return (unsigned)(r / 16u) - 1u; /* 0..63 */
  }
  unsigned shift =
      63u - (unsigned)__builtin_clzll((unsigned long long)(bytes - 1u));
  size_t step = (size_t)1 << (shift - 3u);
  size_t r = (bytes + step - 1u) & ~(step - 1u);
  *rounded = r;
  size_t base = (size_t)1 << shift;
  unsigned idx = r == base * 2u ? 8u : (unsigned)((r - base) / step);
  return 64u + (shift - 10u) * 8u + idx - 1u;
}

static inline bool fc_pool_owns(uintptr_t p) {
  if (p < fc_chunk_lo || p >= fc_chunk_hi)
    return false;
  size_t n = atomic_load_explicit(&fc_chunk_n, memory_order_acquire);
  for (size_t i = 0; i < n; ++i)
    if (p - fc_chunks[i] < FC_CHUNK_BYTES)
      return true;
  return false;
}

bool rt_fc_pool_owns(const void *ptr) { return fc_pool_owns((uintptr_t)ptr); }

static void fc_pool_drain_foreign(void) {
  uintptr_t node =
      atomic_exchange_explicit(&fc_foreign_free, 0, memory_order_acq_rel);
  while (node) {
    uintptr_t *words = (uintptr_t *)node;
    uintptr_t next = ~words[0];
    unsigned cls = (unsigned)words[1];
    words[1] = 0;
    words[0] = ~fc_pool_free[cls];
    fc_pool_free[cls] = node;
    node = next;
  }
}

void *rt_fc_block_alloc(size_t bytes) {
  if (!g_rt_fc_tracking || bytes == 0 || bytes > FC_POOL_MAX)
    return NULL;
  size_t rounded = 0;
  unsigned cls = fc_pool_class(bytes, &rounded);
  if (cls >= FC_POOL_CLASSES)
    return NULL;
  uintptr_t block = fc_pool_free[cls];
  if (!block && atomic_load_explicit(&fc_foreign_free, memory_order_relaxed)) {
    fc_pool_drain_foreign();
    block = fc_pool_free[cls];
  }
  if (block) {
    uintptr_t *words = (uintptr_t *)block;
    fc_pool_free[cls] = ~words[0];
    words[0] = 0;
    return (void *)block;
  }
  if (fc_bump_end - fc_bump < rounded) {
    size_t n = atomic_load_explicit(&fc_chunk_n, memory_order_relaxed);
    if (n >= FC_MAX_CHUNKS)
      return NULL;
    void *chunk = mmap(NULL, FC_CHUNK_BYTES, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (chunk == MAP_FAILED)
      return NULL;
    rt_map_oracle_add((uintptr_t)chunk, FC_CHUNK_BYTES);
    fc_chunks[n] = (uintptr_t)chunk;
    if ((uintptr_t)chunk < fc_chunk_lo)
      fc_chunk_lo = (uintptr_t)chunk;
    if ((uintptr_t)chunk + FC_CHUNK_BYTES > fc_chunk_hi)
      fc_chunk_hi = (uintptr_t)chunk + FC_CHUNK_BYTES;
    atomic_store_explicit(&fc_chunk_n, n + 1, memory_order_release);
    fc_bump = (uintptr_t)chunk;
    fc_bump_end = fc_bump + FC_CHUNK_BYTES;
  }
  block = fc_bump;
  fc_bump += rounded;
  return (void *)block;
}

void rt_fc_block_free(void *ptr, size_t bytes) {
  if (!ptr)
    return;
  if (!fc_pool_owns((uintptr_t)ptr)) {
    /*
     * From the C allocator. While the collector runs, do not leave
     * references behind in memory it will read as roots.
     */
    if (fc_state > 0 && bytes <= ((size_t)1 << 20))
      memset(ptr, 0, bytes);
    rt_oracle_free(ptr);
    return;
  }
  size_t rounded = 0;
  unsigned cls = fc_pool_class(bytes, &rounded);
  memset(ptr, 0, rounded);
  uintptr_t *words = (uintptr_t *)ptr;
  if (g_rt_fc_tracking) {
    words[0] = ~fc_pool_free[cls];
    fc_pool_free[cls] = (uintptr_t)ptr;
    return;
  }
  /*
   * Another thread: hand the block to the owner.
   */
  words[1] = cls;
  uintptr_t head = atomic_load_explicit(&fc_foreign_free, memory_order_acquire);
  do {
    words[0] = ~head;
  } while (!atomic_compare_exchange_weak_explicit(
      &fc_foreign_free, &head, (uintptr_t)ptr, memory_order_acq_rel,
      memory_order_acquire));
}

void rt_fc_track_slow(uintptr_t ptr, uint32_t kind, size_t bytes) {
  if (fc_in_collect || !ptr)
    return;
  if (!fc_buf_reserve(FC_BUF_YOUNG, (fc_young_n + 1) * sizeof(fc_obj_t)))
    return;
  fc_obj_t *young = (fc_obj_t *)fc_bufs[FC_BUF_YOUNG].data;
  young[fc_young_n].ptr = ptr;
  young[fc_young_n].kind = kind;
  young[fc_young_n].mark = 0;
  ++fc_young_n;
  fc_young_bytes += bytes ? bytes : 96;
}

void rt_fc_thread_enter(void) {
  pthread_mutex_lock(&fc_threads_lock);
  if (fc_thread_n < FC_MAX_THREADS)
    fc_threads[fc_thread_n++] = pthread_self();
  else
    fc_thread_overflow = 1;
  pthread_mutex_unlock(&fc_threads_lock);
}

void rt_fc_thread_exit(void) {
  pthread_t self = pthread_self();
  pthread_mutex_lock(&fc_threads_lock);
  for (size_t i = 0; i < fc_thread_n; ++i) {
    if (pthread_equal(fc_threads[i], self)) {
      fc_threads[i] = fc_threads[--fc_thread_n];
      break;
    }
  }
  pthread_mutex_unlock(&fc_threads_lock);
}

static void fc_park_handler(int sig) {
  (void)sig;
  int saved = errno;
  atomic_fetch_add_explicit(&fc_parked, 1, memory_order_release);
  while (atomic_load_explicit(&fc_parking, memory_order_acquire)) {
    struct timespec ts = {0, 50000};
    nanosleep(&ts, NULL);
  }
  atomic_fetch_sub_explicit(&fc_parked, 1, memory_order_release);
  errno = saved;
}

static uint64_t fc_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/*
 * Parks every runtime thread but the caller. Returns false, with nothing
 * parked, when one of them does not answer. Holds fc_threads_lock on
 * success so no thread starts or ends while the world is stopped.
 */
static bool fc_stop_world(void) {
  pthread_mutex_lock(&fc_threads_lock);
  if (fc_thread_overflow) {
    pthread_mutex_unlock(&fc_threads_lock);
    return false;
  }
  if (!fc_signal) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fc_park_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    int sig = SIGRTMIN + 6;
    if (sig > SIGRTMAX || sigaction(sig, &sa, NULL) != 0) {
      pthread_mutex_unlock(&fc_threads_lock);
      return false;
    }
    fc_signal = sig;
  }
  /*
   * A handler from the previous collection may still be on its way out.
   */
  uint64_t deadline = fc_now_ns() + UINT64_C(200000000);
  while (atomic_load_explicit(&fc_parked, memory_order_acquire) != 0) {
    if (fc_now_ns() > deadline) {
      pthread_mutex_unlock(&fc_threads_lock);
      return false;
    }
    sched_yield();
  }
  atomic_store_explicit(&fc_parking, 1, memory_order_release);
  pthread_t self = pthread_self();
  int sent = 0;
  for (size_t i = 0; i < fc_thread_n; ++i) {
    if (pthread_equal(fc_threads[i], self))
      continue;
    if (pthread_kill(fc_threads[i], fc_signal) == 0)
      ++sent;
  }
  while (atomic_load_explicit(&fc_parked, memory_order_acquire) < sent) {
    if (fc_now_ns() > deadline) {
      atomic_store_explicit(&fc_parking, 0, memory_order_release);
      pthread_mutex_unlock(&fc_threads_lock);
      return false;
    }
    sched_yield();
  }
  return true;
}

static void fc_start_world(void) {
  atomic_store_explicit(&fc_parking, 0, memory_order_release);
  pthread_mutex_unlock(&fc_threads_lock);
}

/*
 * LSD radix sort by start address into `scratch` and back: four 11-bit
 * digits cover the 44 significant bits of a 16-byte-aligned user address.
 * No allocation and no recursion while the world is stopped.
 */
static void fc_sort_ranges(fc_range_t *v, fc_range_t *scratch, size_t n) {
  if (n < 2)
    return;
  fc_range_t *src = v, *dst = scratch;
  for (unsigned pass = 0; pass < 4; ++pass) {
    unsigned shift = 4u + pass * 11u;
    size_t count[2048];
    memset(count, 0, sizeof(count));
    for (size_t i = 0; i < n; ++i)
      ++count[(src[i].start >> shift) & 2047u];
    size_t sum = 0;
    for (size_t d = 0; d < 2048; ++d) {
      size_t next = sum + count[d];
      count[d] = sum;
      sum = next;
    }
    for (size_t i = 0; i < n; ++i)
      dst[count[(src[i].start >> shift) & 2047u]++] = src[i];
    fc_range_t *t = src;
    src = dst;
    dst = t;
  }
  /*
   * Four passes: the result is back in `v`. Finish the bits a digit sort
   * does not see (the low four, and anything above bit 47).
   */
  for (size_t i = 1; i < n; ++i) {
    fc_range_t key = v[i];
    size_t j = i;
    while (j > 0 && v[j - 1].start > key.start) {
      v[j] = v[j - 1];
      --j;
    }
    v[j] = key;
  }
}

typedef struct fc_ctx {
  fc_obj_t *cand;
  size_t cand_n;
  fc_range_t *ranges;
  size_t range_n;
  uintptr_t lo, hi; /* [lo, hi) covers every candidate range */
  /*
   * The same, split: candidates in the collector heap and candidates in the
   * C heap sit far apart, and almost no stray word falls in either.
   */
  uintptr_t lo_a, span_a, lo_b, span_b;
  /*
   * One bit per 4 KiB page of each span: set where a candidate has memory.
   * Small enough to stay in cache, and it turns away the commonest stray
   * word, a pointer to something else in the same heap.
   */
  const uint64_t *pages_a, *pages_b;
  fc_extent_t *extents;
  /*
   * Hashed set of the 64-byte granules candidates occupy. Most words that
   * land in the span point at something else; this rejects them without a
   * search. A false positive only costs the search.
   */
  uint64_t *filter;
  uint32_t *stack;
  size_t stack_n;
  size_t marked;
} fc_ctx_t;

/*
 * Index of the first range whose end is above `addr`.
 */
static size_t fc_range_lower(const fc_ctx_t *c, uintptr_t addr) {
  size_t lo = 0, hi = c->range_n;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c->ranges[mid].end <= addr)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

#define FC_FILTER_BITS 23u
static inline uint64_t fc_filter_bit(uintptr_t addr) {
  return (((uint64_t)addr >> 6) * UINT64_C(0x9E3779B97F4A7C15)) >>
         (64u - FC_FILTER_BITS);
}

static __attribute__((noinline)) void fc_mark_addr(fc_ctx_t *c,
                                                   uintptr_t addr) {
  uint64_t bit = fc_filter_bit(addr);
  if (!((c->filter[bit >> 6] >> (bit & 63u)) & 1u))
    return;
  size_t i = fc_range_lower(c, addr);
  if (i >= c->range_n || c->ranges[i].start > addr)
    return;
  fc_obj_t *o = &c->cand[c->ranges[i].obj];
  if (o->mark)
    return;
  o->mark = 1;
  ++c->marked;
  c->stack[c->stack_n++] = c->ranges[i].obj;
}

/*
 * A reference is the address itself, or the address carried as a tagged
 * integer ((p << 1) | 1, what an integer-typed slot holding a pointer turns
 * into at a dynamic boundary), or the native encoding.
 */
static inline bool fc_in_spans(const fc_ctx_t *c, uintptr_t w) {
  return w - c->lo_a < c->span_a || w - c->lo_b < c->span_b;
}

static inline bool fc_page_bit(const uint64_t *bits, uintptr_t offset) {
  uintptr_t page = offset >> 12;
  return bits && ((bits[page >> 6] >> (page & 63u)) & 1u);
}

/*
 * In a span and on a page that holds a candidate.
 */
static inline bool fc_candidate_page(const fc_ctx_t *c, uintptr_t w) {
  if (w - c->lo_a < c->span_a)
    return !c->pages_a || fc_page_bit(c->pages_a, w - c->lo_a);
  if (w - c->lo_b < c->span_b)
    return !c->pages_b || fc_page_bit(c->pages_b, w - c->lo_b);
  return false;
}

static inline void fc_scan_word(fc_ctx_t *c, uintptr_t w) {
  if (fc_in_spans(c, w)) {
    if (fc_candidate_page(c, w))
      fc_mark_addr(c, w);
    return;
  }
  if (w & 1u) {
    uintptr_t untagged = w >> 1;
    if (fc_candidate_page(c, untagged))
      fc_mark_addr(c, untagged);
    return;
  }
  if (NY_NATIVE_IS(w)) {
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(w);
    if (fc_candidate_page(c, decoded))
      fc_mark_addr(c, decoded);
  }
}

/*
 * The hot loop. Root memory is mostly pixels, vertices and text, where
 * whether a word is odd is a coin toss, so the three forms a reference can
 * take are tested without branching and only a word that lands in a
 * candidate span leaves the loop body.
 */
static void fc_scan_words(fc_ctx_t *c, const uintptr_t *words, size_t n) {
  const uintptr_t lo_a = c->lo_a, span_a = c->span_a;
  const uintptr_t lo_b = c->lo_b, span_b = c->span_b;
  for (size_t i = 0; i < n; ++i) {
    uintptr_t w = words[i];
    uintptr_t untagged = w >> 1;
    uintptr_t decoded = (uintptr_t)NY_NATIVE_DECODE(w);
    unsigned hit = (unsigned)(w - lo_a < span_a) |
                   (unsigned)(w - lo_b < span_b) |
                   (unsigned)(untagged - lo_a < span_a) |
                   (unsigned)(untagged - lo_b < span_b) |
                   (unsigned)(decoded - lo_a < span_a) |
                   (unsigned)(decoded - lo_b < span_b);
    if (__builtin_expect(hit, 0))
      fc_scan_word(c, w);
  }
}

/*
 * Scans live memory [s, e) that belongs to no candidate and no excluded
 * table. `copy` holds the bytes of [base, ...).
 */
static void fc_scan_root_piece(fc_ctx_t *c, uintptr_t base,
                               const unsigned char *copy, uintptr_t s,
                               uintptr_t e) {
  s = (s + 7u) & ~(uintptr_t)7u;
  e &= ~(uintptr_t)7u;
  if (s >= e)
    return;
  if (s >= c->hi || e <= c->lo) {
    fc_trace_words += (e - s) / sizeof(uintptr_t);
    fc_scan_words(c, (const uintptr_t *)(copy + (s - base)),
                  (e - s) / sizeof(uintptr_t));
    return;
  }
  /*
   * Walk the candidate ranges that overlap the piece once, in order.
   */
  size_t i = fc_range_lower(c, s);
  while (s < e) {
    uintptr_t stop = e;
    if (i < c->range_n) {
      if (c->ranges[i].start <= s) {
        uintptr_t after = (c->ranges[i].end + 7u) & ~(uintptr_t)7u;
        ++i;
        if (after > s)
          s = after;
        continue;
      }
      if (c->ranges[i].start < stop)
        stop = c->ranges[i].start & ~(uintptr_t)7u;
    }
    if (stop > s) {
      fc_trace_words += (stop - s) / sizeof(uintptr_t);
      fc_scan_words(c, (const uintptr_t *)(copy + (s - base)),
                    (stop - s) / sizeof(uintptr_t));
      s = stop;
    } else {
      s += 8u;
    }
  }
}

/*
 * Root memory is read in place. A thread the collector does not control (a
 * driver's, the JIT's) can unmap a page between the page listing and the
 * read, so the scan runs under a fault guard: a fault skips to the next page
 * instead of ending the process.
 * volatile: the only reader is the signal handler, which the compiler
 * cannot see; without it the store before the scan loop is removed.
 */
static __thread sigjmp_buf *volatile fc_fault_jmp;
static __thread volatile uintptr_t fc_fault_addr;
static struct sigaction fc_old_segv, fc_old_bus;

static void fc_fault_handler(int sig, siginfo_t *info, void *uctx) {
  if (fc_fault_jmp) {
    fc_fault_addr = (uintptr_t)info->si_addr;
    siglongjmp(*fc_fault_jmp, 1);
  }
  const struct sigaction *old = sig == SIGBUS ? &fc_old_bus : &fc_old_segv;
  if (old->sa_flags & SA_SIGINFO) {
    if (old->sa_sigaction) {
      old->sa_sigaction(sig, info, uctx);
      return;
    }
  } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
    old->sa_handler(sig);
    return;
  }
  /*
   * No earlier handler: restore the default and let the fault recur.
   */
  signal(sig, SIG_DFL);
}

static void fc_fault_guard_install(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = fc_fault_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &fc_old_segv);
  sigaction(SIGBUS, &sa, &fc_old_bus);
}

static void fc_fault_guard_remove(void) {
  sigaction(SIGSEGV, &fc_old_segv, NULL);
  sigaction(SIGBUS, &fc_old_bus, NULL);
}

static __attribute__((noinline)) void fc_scan_root_run(fc_ctx_t *c, uintptr_t s,
                                                       uintptr_t e) {
  volatile uintptr_t cursor = s;
  sigjmp_buf jmp;
  /*
   * The handler runs with SA_NODEFER and an empty mask: nothing to restore.
   */
  if (sigsetjmp(jmp, 0) != 0) {
    /*
     * The page at the fault is gone; continue after it.
     */
    uintptr_t at = fc_fault_addr;
    uintptr_t next =
        ((at >= cursor && at < e ? at : cursor) & ~(uintptr_t)4095u) + 4096u;
    cursor = next;
  }
  fc_fault_jmp = &jmp;
  while (cursor < e) {
    uintptr_t cur = cursor;
    /*
     * A few pages at a time, so a fault loses little.
     */
    uintptr_t piece_end = (cur & ~(uintptr_t)4095u) + 16u * 4096u;
    if (piece_end > e)
      piece_end = e;
    /*
     * Drop the parts that are runtime tables or collector storage.
     */
    bool skip = false;
    for (size_t i = 0; i < fc_exclude_n; ++i) {
      if (fc_excludes[i][0] <= cur && cur < fc_excludes[i][1]) {
        cursor = fc_excludes[i][1];
        skip = true;
        break;
      }
      if (fc_excludes[i][0] > cur && fc_excludes[i][0] < piece_end)
        piece_end = fc_excludes[i][0];
    }
    if (skip)
      continue;
    fc_scan_root_piece(c, cur, (const unsigned char *)cur, cur, piece_end);
    cursor = piece_end;
  }
  fc_fault_jmp = NULL;
}

/*
 * Scans the pages [rs, re) of root memory. Most written pages belong to the
 * runtime's own tables; those are stepped over here rather than inside the
 * guarded scan.
 */
static void fc_scan_root_span(fc_ctx_t *c, uintptr_t rs, uintptr_t re) {
  fc_trace_pages += (re - rs) >> 12;
  while (rs < re) {
    uintptr_t next_excl = re;
    bool inside = false;
    for (size_t x = 0; x < fc_exclude_n; ++x) {
      if (fc_excludes[x][0] <= rs && rs < fc_excludes[x][1]) {
        rs = fc_excludes[x][1];
        inside = true;
        break;
      }
      if (fc_excludes[x][0] > rs && fc_excludes[x][0] < next_excl)
        next_excl = fc_excludes[x][0];
    }
    if (inside)
      continue;
    fc_scan_root_run(c, rs, next_excl);
    rs = next_excl;
  }
}

static uintptr_t fc_parse_hex(const char **p) {
  uintptr_t v = 0;
  for (;; ++*p) {
    char ch = **p;
    unsigned d;
    if (ch >= '0' && ch <= '9')
      d = (unsigned)(ch - '0');
    else if (ch >= 'a' && ch <= 'f')
      d = (unsigned)(ch - 'a') + 10u;
    else
      break;
    v = (v << 4) | d;
  }
  return v;
}

#define FC_PM_SOFT_DIRTY (UINT64_C(1) << 55)
#define FC_PM_SWAPPED (UINT64_C(1) << 62)
#define FC_PM_PRESENT (UINT64_C(1) << 63)

/*
 * Reads /proc/self/maps into the maps buffer; returns its length or 0.
 */
static size_t fc_read_maps(void) {
  int maps_fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (maps_fd < 0)
    return 0;
  char *maps = (char *)fc_bufs[FC_BUF_MAPS].data;
  size_t cap = fc_bufs[FC_BUF_MAPS].bytes;
  size_t len = 0;
  for (;;) {
    ssize_t got = read(maps_fd, maps + len, cap - 1 - len);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      break;
    len += (size_t)got;
    if (len >= cap - 1) {
      close(maps_fd);
      return 0;
    }
  }
  close(maps_fd);
  maps[len] = '\0';
  return len;
}

#if FC_HAVE_WRITE_TRACKING
/*
 * Write tracking through userfaultfd (Linux 6.7 and later). Mappings are
 * registered for asynchronous write protection; one PAGEMAP_SCAN call then
 * returns the pages written since the previous call and protects them again,
 * atomically. Unlike the soft-dirty interface there is no separate pass over
 * every page of the process to clear bits and no per-page record to read,
 * which matters when the process is large (a program run in-process by the
 * JIT shares its address space with the compiler). Pages in mappings that
 * are not registered (created since the last registration, or of a kind
 * that cannot be) are always treated as written.
 */
static int fc_uffd = -1;
static int fc_pagemap_fd = -1;
static bool fc_register_due;
static unsigned fc_register_age;
static size_t fc_trace_unregistered_pages;

static size_t fc_written_n, fc_written_new_n;
static bool fc_written_ok;
static long fc_tracked_pass(fc_ctx_t *c, bool scan);

static void fc_uffd_register_all(void) {
  size_t len = fc_read_maps();
  if (!len)
    return;
  const char *maps = (const char *)fc_bufs[FC_BUF_MAPS].data;
  const char *p = maps;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol)
      eol = maps + len;
    uintptr_t start = fc_parse_hex(&p);
    if (*p == '-') {
      ++p;
      uintptr_t end = fc_parse_hex(&p);
      const char *perms = *p == ' ' ? p + 1 : p;
      if (perms[0] == 'r' && perms[1] == 'w' && perms[3] == 'p' &&
          end > start) {
        /*
         * Anonymous memory only: inode 0 and no path, or a bracketed name
         * such as [heap] and [stack].
         */
        const char *q = perms + 4;
        for (int field = 0; field < 3 && q < eol; ++field) {
          while (q < eol && *q == ' ')
            ++q;
          while (q < eol && *q != ' ')
            ++q;
        }
        while (q < eol && *q == ' ')
          ++q;
        if (q == eol || *q == '[') {
          struct uffdio_register reg;
          memset(&reg, 0, sizeof(reg));
          reg.range.start = start;
          reg.range.len = end - start;
          reg.mode = UFFDIO_REGISTER_MODE_WP;
          (void)ioctl(fc_uffd, UFFDIO_REGISTER, &reg);
        }
      }
    }
    p = *eol ? eol + 1 : eol;
  }
  fc_register_due = false;
}

static bool fc_uffd_start(void) {
  int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | UFFD_USER_MODE_ONLY);
  if (fd < 0)
    return false;
  struct uffdio_api api;
  memset(&api, 0, sizeof(api));
  api.api = UFFD_API;
  api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
  int pm = -1;
  if (ioctl(fd, UFFDIO_API, &api) != 0 ||
      !(api.features & UFFD_FEATURE_WP_ASYNC) ||
      (pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC)) < 0) {
    close(fd);
    return false;
  }
  /*
   * The scan interface must exist as well.
   */
  struct pm_scan_arg probe;
  memset(&probe, 0, sizeof(probe));
  probe.size = sizeof(probe);
  probe.start = (uintptr_t)&probe & ~(uintptr_t)4095u;
  probe.end = probe.start + 4096u;
  probe.category_mask = PAGE_IS_PRESENT;
  probe.return_mask = PAGE_IS_PRESENT;
  struct page_region one;
  probe.vec = (uintptr_t)&one;
  probe.vec_len = 1;
  if (ioctl(pm, PAGEMAP_SCAN, &probe) < 0) {
    close(pm);
    close(fd);
    return false;
  }
  fc_uffd = fd;
  fc_pagemap_fd = pm;
  if (!fc_buf_reserve(FC_BUF_MAPS, (size_t)8 << 20) ||
      !fc_buf_reserve(FC_BUF_PAGEMAP, (size_t)1 << 16)) {
    close(pm);
    close(fd);
    fc_uffd = fc_pagemap_fd = -1;
    return false;
  }
  fc_uffd_register_all();
  /*
   * Protect everything now, before the first object is recorded, so the
   * first collection reads only what the program wrote since and the
   * written set starts empty.
   */
  fc_written_ok = false;
  if (fc_tracked_pass(NULL, false) >= 0) {
    fc_written_ok = fc_buf_reserve(FC_BUF_WRITTEN_NEW, (size_t)1 << 16);
    fc_written_n = fc_written_new_n = 0;
  }
  return true;
}

/*
 * Pages written since the collector started, as sorted, disjoint
 * [start, end) pairs. Every recorded object was allocated after the start,
 * so a reference to one can only be in memory written since then, or in
 * memory that is not tracked. A full collection reads these pages instead
 * of every page of the process: a program run in-process by the JIT shares
 * the address space with gigabytes of compiler memory that it never writes.
 * `fc_written_ok` is false when the set could not be kept (out of memory,
 * or tracking did not start with everything protected); full collections
 * then read everything.
 */
static void fc_written_note(uintptr_t start, uintptr_t end) {
  if (!fc_written_ok)
    return;
  uintptr_t *v = (uintptr_t *)fc_bufs[FC_BUF_WRITTEN_NEW].data;
  if (fc_written_new_n > 0 && v[2 * fc_written_new_n - 1] == start) {
    v[2 * fc_written_new_n - 1] = end;
    return;
  }
  if (!fc_buf_reserve(FC_BUF_WRITTEN_NEW,
                      (fc_written_new_n + 1) * 2 * sizeof(uintptr_t))) {
    fc_written_ok = false;
    return;
  }
  v = (uintptr_t *)fc_bufs[FC_BUF_WRITTEN_NEW].data;
  v[2 * fc_written_new_n] = start;
  v[2 * fc_written_new_n + 1] = end;
  ++fc_written_new_n;
}

/*
 * Folds the pages of the last pass into the set. Both lists are sorted.
 */
static void fc_written_merge(void) {
  size_t a_n = fc_written_n, b_n = fc_written_new_n;
  fc_written_new_n = 0;
  if (!fc_written_ok || b_n == 0)
    return;
  if (!fc_buf_reserve(FC_BUF_WRITTEN_TMP,
                      (a_n + b_n) * 2 * sizeof(uintptr_t))) {
    fc_written_ok = false;
    return;
  }
  const uintptr_t *a = (const uintptr_t *)fc_bufs[FC_BUF_WRITTEN].data;
  const uintptr_t *b = (const uintptr_t *)fc_bufs[FC_BUF_WRITTEN_NEW].data;
  uintptr_t *out = (uintptr_t *)fc_bufs[FC_BUF_WRITTEN_TMP].data;
  size_t i = 0, j = 0, n = 0;
  while (i < a_n || j < b_n) {
    const uintptr_t *r;
    if (j >= b_n || (i < a_n && a[2 * i] <= b[2 * j]))
      r = &a[2 * i++];
    else
      r = &b[2 * j++];
    if (n > 0 && r[0] <= out[2 * n - 1]) {
      if (r[1] > out[2 * n - 1])
        out[2 * n - 1] = r[1];
    } else {
      out[2 * n] = r[0];
      out[2 * n + 1] = r[1];
      ++n;
    }
  }
  fc_buf_t swap = fc_bufs[FC_BUF_WRITTEN];
  fc_bufs[FC_BUF_WRITTEN] = fc_bufs[FC_BUF_WRITTEN_TMP];
  fc_bufs[FC_BUF_WRITTEN_TMP] = swap;
  fc_written_n = n;
}

/*
 * One PAGEMAP_SCAN pass over [from, to); every returned region is scanned
 * as root memory when `scan` is set and added to the written set when
 * `record` is. Returns pages seen, or -1.
 */
static long fc_pagemap_pass_range(fc_ctx_t *c, uint64_t from, uint64_t to,
                                  uint64_t flags, uint64_t mask, uint64_t anyof,
                                  uint64_t inverted, bool scan, bool record) {
  struct page_region *regions =
      (struct page_region *)fc_bufs[FC_BUF_PAGEMAP].data;
  size_t cap = fc_bufs[FC_BUF_PAGEMAP].bytes / sizeof(*regions);
  const uint64_t top = to;
  uint64_t at = from;
  long pages = 0;
  while (at < top) {
    struct pm_scan_arg arg;
    memset(&arg, 0, sizeof(arg));
    arg.size = sizeof(arg);
    arg.flags = flags;
    arg.start = at;
    arg.end = top;
    arg.vec = (uintptr_t)regions;
    arg.vec_len = cap;
    arg.category_mask = mask;
    arg.category_anyof_mask = anyof;
    arg.category_inverted = inverted;
    arg.return_mask = PAGE_IS_PRESENT;
    uint64_t t_io = fc_now_ns();
    long n = ioctl(fc_pagemap_fd, PAGEMAP_SCAN, &arg);
    fc_kernel_ns += fc_now_ns() - t_io;
    if (n < 0)
      return -1;
    for (long i = 0; i < n; ++i) {
      pages += (long)((regions[i].end - regions[i].start) >> 12);
      if (record)
        fc_written_note((uintptr_t)regions[i].start, (uintptr_t)regions[i].end);
      if (scan)
        fc_scan_root_span(c, (uintptr_t)regions[i].start,
                          (uintptr_t)regions[i].end);
    }
    if (arg.walk_end <= at || arg.walk_end >= top)
      break;
    at = arg.walk_end;
  }
  return pages;
}

static long fc_pagemap_pass(fc_ctx_t *c, uint64_t flags, uint64_t mask,
                            uint64_t anyof, uint64_t inverted, bool scan) {
  return fc_pagemap_pass_range(c, 4096, (UINT64_C(1) << 47) - 4096u, flags,
                               mask, anyof, inverted, scan, false);
}

/*
 * The tracked pass: pages of registered mappings written since the last
 * pass, which it protects again. Only populated pages: an untouched page
 * carries no reference, and once written it is populated without the
 * protection bit, so the next pass reports it. (Asking for unpopulated
 * pages as well makes the kernel report every hole of a sparse mapping,
 * such as the compiler's freed thread arenas, and reading those holes then
 * maps zero pages that every later pass has to walk.)
 */
static long fc_tracked_pass(fc_ctx_t *c, bool scan) {
  return fc_pagemap_pass_range(
      c, 4096, (UINT64_C(1) << 47) - 4096u, PM_SCAN_WP_MATCHING,
      PAGE_IS_WRITTEN | PAGE_IS_FILE | PAGE_IS_WPALLOWED,
      PAGE_IS_PRESENT | PAGE_IS_SWAPPED, PAGE_IS_FILE, scan, true);
}

static bool fc_scan_roots_uffd(fc_ctx_t *c, bool dirty_only) {
  if (fc_register_due)
    fc_uffd_register_all();
  /*
   * Written pages of registered mappings; protects them again. A full
   * collection reads everything below, but still needs the protection
   * renewed so the next one starts from now.
   */
  uint64_t t_a = fc_now_ns();
  long written = fc_tracked_pass(c, dirty_only);
  fc_written_merge();
  if (written < 0)
    return false;
  uint64_t t_b = fc_now_ns();
  long other;
  if (!dirty_only && fc_written_ok) {
    /*
     * Full collection: every page written since the start that is still
     * populated, then everything in mappings that are not tracked.
     */
    const uintptr_t *w = (const uintptr_t *)fc_bufs[FC_BUF_WRITTEN].data;
    long since = 0;
    for (size_t i = 0; i < fc_written_n && since >= 0; ++i) {
      long got = fc_pagemap_pass_range(c, w[2 * i], w[2 * i + 1], 0, 0,
                                       PAGE_IS_PRESENT | PAGE_IS_SWAPPED, 0,
                                       true, false);
      since = got < 0 ? -1 : since + got;
    }
    if (since < 0)
      return false;
    other = fc_pagemap_pass(c, 0, PAGE_IS_WPALLOWED | PAGE_IS_FILE,
                            PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                            PAGE_IS_WPALLOWED | PAGE_IS_FILE, true);
    if (fc_trace_level > 1)
      fprintf(stderr,
              "[heap-collect]   full: %ld written-since-start pages in %zu "
              "ranges\n",
              since, fc_written_n);
  } else if (dirty_only) {
    /*
     * Everything present in mappings that are not tracked.
     */
    other = fc_pagemap_pass(c, 0, PAGE_IS_WPALLOWED | PAGE_IS_FILE,
                            PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                            PAGE_IS_WPALLOWED | PAGE_IS_FILE, true);
    /*
     * New mappings appear all the time (allocator arenas, thread stacks,
     * the collector's own heap); pick them up every few collections.
     */
    if (other > 256 && ++fc_register_age >= 8) {
      fc_register_due = true;
      fc_register_age = 0;
    }
    fc_trace_unregistered_pages = other > 0 ? (size_t)other : 0;
  } else {
    other =
        fc_pagemap_pass(c, 0, PAGE_IS_FILE, PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                        PAGE_IS_FILE, true);
  }
  if (fc_trace_level > 1)
    fprintf(stderr,
            "[heap-collect]   tracked pass %.2f ms (%ld pages), other pass "
            "%.2f ms (%ld pages), in the kernel %.2f ms\n",
            (double)(t_b - t_a) / 1e6, written,
            (double)(fc_now_ns() - t_b) / 1e6, other,
            (double)fc_kernel_ns / 1e6);
  return other >= 0;
}

#else
static int fc_uffd = -1;
static bool fc_uffd_start(void) { return false; }
static bool fc_scan_roots_uffd(fc_ctx_t *c, bool dirty_only) {
  (void)c;
  (void)dirty_only;
  return false;
}
#endif

/*
 * Scans every writable private mapping; with `dirty_only`, only the pages
 * written since the soft-dirty bits were last cleared.
 */
static bool fc_scan_roots_legacy(fc_ctx_t *c, bool dirty_only) {
  size_t len = fc_read_maps();
  if (!len)
    return false;
  char *maps = (char *)fc_bufs[FC_BUF_MAPS].data;
  int pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  if (pm_fd < 0)
    return false;
  uint64_t *pm = (uint64_t *)fc_bufs[FC_BUF_PAGEMAP].data;
  size_t pm_cap = fc_bufs[FC_BUF_PAGEMAP].bytes / sizeof(uint64_t);
  bool ok = true;
  const char *p = maps;
  while (*p && ok) {
    const char *line = p;
    const char *eol = strchr(p, '\n');
    if (!eol)
      eol = maps + len;
    uintptr_t start = fc_parse_hex(&p);
    if (*p != '-') {
      p = *eol ? eol + 1 : eol;
      continue;
    }
    ++p;
    uintptr_t end = fc_parse_hex(&p);
    const char *perms = *p == ' ' ? p + 1 : p;
    bool wanted = perms[0] == 'r' && perms[1] == 'w' && perms[3] == 'p';
    if (wanted) {
      /*
       * Device memory is not ordinary storage; never read it.
       */
      for (const char *q = line; q + 6 <= eol; ++q) {
        if (memcmp(q, " /dev/", 6) == 0) {
          wanted = false;
          break;
        }
      }
    }
    p = *eol ? eol + 1 : eol;
    if (!wanted || end <= start)
      continue;
    size_t marked_before = c->marked;
    size_t pages_before = fc_trace_pages, words_before = fc_trace_words;
    for (uintptr_t at = start; at < end && ok;) {
      size_t pages = (end - at) >> 12;
      if (pages > pm_cap)
        pages = pm_cap;
      ssize_t got = pread(pm_fd, pm, pages * sizeof(uint64_t),
                          (off_t)((at >> 12) * sizeof(uint64_t)));
      if (got <= 0) {
        ok = false;
        break;
      }
      size_t have = (size_t)got / sizeof(uint64_t);
      size_t i = 0;
      while (i < have) {
        uint64_t need = dirty_only ? FC_PM_SOFT_DIRTY : 0;
        if (!(pm[i] & (FC_PM_PRESENT | FC_PM_SWAPPED)) ||
            (pm[i] & need) != need) {
          ++i;
          continue;
        }
        size_t j = i + 1;
        while (j < have && (pm[j] & (FC_PM_PRESENT | FC_PM_SWAPPED)) &&
               (pm[j] & need) == need)
          ++j;
        fc_scan_root_span(c, at + ((uintptr_t)i << 12),
                          at + ((uintptr_t)j << 12));
        i = j;
      }
      at += (uintptr_t)have << 12;
    }
    if (fc_trace_level == 2 &&
        (c->marked > marked_before || fc_trace_pages - pages_before > 64))
      fprintf(stderr,
              "[heap-collect]   %zu marked, %zu pages, %zu words from %.*s\n",
              c->marked - marked_before, fc_trace_pages - pages_before,
              fc_trace_words - words_before, (int)(eol - line), line);
  }
  close(pm_fd);
  return ok;
}

static bool fc_scan_roots(fc_ctx_t *c, bool dirty_only) {
  if (fc_uffd >= 0)
    return fc_scan_roots_uffd(c, dirty_only);
  return fc_scan_roots_legacy(c, dirty_only);
}

static bool fc_clear_soft_dirty(void) {
  int fd = open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  ssize_t wrote = write(fd, "4", 1);
  close(fd);
  return wrote == 1;
}

/*
 * One-time check that the kernel really tracks soft-dirty pages.
 */
static bool fc_probe_soft_dirty(void) {
  volatile char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED)
    return false;
  page[0] = 1;
  page[4096] = 1;
  bool ok = fc_clear_soft_dirty();
  int fd = ok ? open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC) : -1;
  if (fd < 0)
    ok = false;
  if (ok) {
    page[4096] = 2;
    uint64_t bits[2] = {0, 0};
    if (pread(fd, bits, sizeof(bits),
              (off_t)(((uintptr_t)page >> 12) * sizeof(uint64_t))) !=
        (ssize_t)sizeof(bits))
      ok = false;
    else
      ok = (bits[0] & FC_PM_PRESENT) && !(bits[0] & FC_PM_SOFT_DIRTY) &&
           (bits[1] & FC_PM_SOFT_DIRTY);
  }
  if (fd >= 0)
    close(fd);
  munmap((void *)page, 8192);
  return ok;
}

#define FC_MAX_CACHE_REPORTERS 32u
static void (*fc_cache_reporters[FC_MAX_CACHE_REPORTERS])(
    void (*add)(uintptr_t, uintptr_t));
static size_t fc_cache_reporter_n;

void rt_fc_register_cache_reporter(void (*report)(void (*add)(uintptr_t,
                                                              uintptr_t))) {
  for (size_t i = 0; i < fc_cache_reporter_n; ++i)
    if (fc_cache_reporters[i] == report)
      return;
  if (fc_cache_reporter_n < FC_MAX_CACHE_REPORTERS)
    fc_cache_reporters[fc_cache_reporter_n++] = report;
}

static void fc_add_exclude(uintptr_t start, uintptr_t end) {
  if (fc_exclude_n < FC_MAX_EXCLUDES && end > start) {
    fc_excludes[fc_exclude_n][0] = start;
    fc_excludes[fc_exclude_n][1] = end;
    ++fc_exclude_n;
  }
}

static size_t fc_managed_total(uintptr_t ptr) {
  uint64_t body = *(const uint64_t *)(ptr - 24);
  if (body & 1u)
    body >>= 1;
  return (size_t)((body + 15u) & ~UINT64_C(15)) + 32u;
}

/*
 * Managed blocks that are language values. A block without one of these
 * tags is memory the program asked for with malloc and frees itself.
 */
static inline bool fc_managed_has_references(int64_t tag) {
  return tag == TAG_OK || tag == TAG_ERR;
}

static inline bool fc_managed_collectable(int64_t tag) {
  return tag == TAG_STR || tag == TAG_BYTES || tag == TAG_BIGINT ||
         fc_managed_has_references(tag);
}

static void fc_free_object(const fc_obj_t *o) {
  switch (o->kind) {
  case RT_FC_KIND_LIST:
    rt_fc_list_free(o->ptr);
    break;
  case RT_FC_KIND_DICT:
    rt_fc_dict_free(o->ptr);
    break;
  case RT_FC_KIND_MANAGED:
    rt_free((int64_t)o->ptr);
    break;
  case RT_FC_KIND_SSO:
    rt_fc_sso_free(o->ptr);
    break;
  default:
    break;
  }
}

static __attribute__((noinline)) int64_t fc_collect(bool major,
                                                    uintptr_t stack_mark) {
  uint64_t t0 = fc_now_ns();
  fc_obj_t *young = (fc_obj_t *)fc_bufs[FC_BUF_YOUNG].data;
  fc_obj_t *old = (fc_obj_t *)fc_bufs[FC_BUF_OLD].data;
  size_t total = fc_young_n + (major ? fc_old_n : 0);
  if (total == 0 || total > UINT32_MAX / 2)
    return 0;
  if (!fc_buf_reserve(FC_BUF_CAND, total * sizeof(fc_obj_t)) ||
      !fc_buf_reserve(FC_BUF_RANGES, total * 2 * sizeof(fc_range_t)) ||
      !fc_buf_reserve(FC_BUF_STACK, total * sizeof(uint32_t)) ||
      !fc_buf_reserve(FC_BUF_OLD, (fc_old_n + total) * sizeof(fc_obj_t)) ||
      !fc_buf_reserve(FC_BUF_MAPS, (size_t)8 << 20) ||
      !fc_buf_reserve(FC_BUF_PAGEMAP, (size_t)1 << 16) ||
      !fc_buf_reserve(FC_BUF_EXTENTS, total * sizeof(fc_extent_t)) ||
      !fc_buf_reserve(FC_BUF_SORT, total * 2 * sizeof(fc_range_t)) ||
      !fc_buf_reserve(FC_BUF_FILTER, ((size_t)1 << FC_FILTER_BITS) / 8u) ||
      !fc_buf_reserve(FC_BUF_PENDING, total * sizeof(fc_obj_t)))
    return 0;
  old = (fc_obj_t *)fc_bufs[FC_BUF_OLD].data;
  if (!fc_stop_world())
    return 0;
  fc_trace_pages = fc_trace_words = 0;
  fc_kernel_ns = 0;
  fc_trace_t[0] = fc_now_ns();

  fc_ctx_t c;
  memset(&c, 0, sizeof(c));
  c.cand = (fc_obj_t *)fc_bufs[FC_BUF_CAND].data;
  c.ranges = (fc_range_t *)fc_bufs[FC_BUF_RANGES].data;
  c.stack = (uint32_t *)fc_bufs[FC_BUF_STACK].data;
  c.extents = (fc_extent_t *)fc_bufs[FC_BUF_EXTENTS].data;
  c.lo = UINTPTR_MAX;

  /*
   * Candidates: every recorded object that is still what it was recorded
   * as. Anything else (released by the program, not a collectable kind,
   * too large) is dropped from the books and treated as plain memory.
   */
  for (int pass = 0; pass < (major ? 2 : 1); ++pass) {
    const fc_obj_t *src = pass == 0 ? young : old;
    size_t n = pass == 0 ? fc_young_n : fc_old_n;
    for (size_t i = 0; i < n; ++i) {
      uintptr_t r[4];
      size_t nr = 0;
      uintptr_t ptr = src[i].ptr;
      if (src[i].kind == RT_FC_KIND_LIST) {
        if (rt_fc_list_range(ptr, &r[0], &r[1], pass != 0))
          nr = 1;
      } else if (src[i].kind == RT_FC_KIND_DICT) {
        if (rt_fc_dict_ranges(ptr, r))
          nr = 2;
      } else if (src[i].kind == RT_FC_KIND_SSO) {
        if (rt_fc_sso_range(ptr, &r[0], &r[1]))
          nr = 1;
      } else if (src[i].kind == RT_FC_KIND_MANAGED) {
        if (rt_heap_ptr_registry_has(ptr) &&
            fc_managed_collectable(*(const int64_t *)(ptr - 8))) {
          r[0] = ptr - 32;
          r[1] = r[0] + fc_managed_total(ptr);
          nr = 1;
        }
      }
      if (nr == 0)
        continue;
      bool large = false;
      for (size_t k = 0; k < nr; ++k)
        if (r[2 * k + 1] - r[2 * k] >= FC_LARGE_OBJECT)
          large = true;
      if (large)
        continue;
      uint32_t idx = (uint32_t)c.cand_n++;
      c.cand[idx] = src[i];
      c.cand[idx].mark = 0;
      c.extents[idx].count = (uint32_t)nr;
      memcpy(c.extents[idx].r, r, nr * 2 * sizeof(uintptr_t));
      for (size_t k = 0; k < nr; ++k) {
        fc_range_t *out = &c.ranges[c.range_n++];
        out->start = r[2 * k];
        out->end = r[2 * k + 1];
        out->obj = idx;
        if (out->start < c.lo)
          c.lo = out->start;
        if (out->end > c.hi)
          c.hi = out->end;
      }
    }
  }
  fc_trace_t[1] = fc_now_ns();
  fc_sort_ranges(c.ranges, (fc_range_t *)fc_bufs[FC_BUF_SORT].data, c.range_n);
  fc_trace_t[2] = fc_now_ns();
  /*
   * The same address recorded twice (released and reused) yields identical
   * ranges; keep one and mark the duplicate object so it is not freed
   * twice. Overlap of any other kind means the books are wrong: keep all.
   */
  bool sane = true;
  size_t w = 0;
  for (size_t i = 0; i < c.range_n; ++i) {
    if (w > 0 && c.ranges[i].start < c.ranges[w - 1].end) {
      if (c.ranges[i].start == c.ranges[w - 1].start &&
          c.ranges[i].end == c.ranges[w - 1].end &&
          c.cand[c.ranges[i].obj].ptr == c.cand[c.ranges[w - 1].obj].ptr &&
          c.cand[c.ranges[i].obj].kind == c.cand[c.ranges[w - 1].obj].kind) {
        c.cand[c.ranges[i].obj].mark = 2; /* duplicate record */
        continue;
      }
      sane = false;
      break;
    }
    c.ranges[w++] = c.ranges[i];
  }
  c.range_n = w;

  int64_t freed_bytes = 0;
  bool scanned = false;
  if (sane && c.range_n > 0) {
    c.filter = (uint64_t *)fc_bufs[FC_BUF_FILTER].data;
    memset(c.filter, 0, ((size_t)1 << FC_FILTER_BITS) / 8u);
    uintptr_t a_lo = UINTPTR_MAX, a_hi = 0, b_lo = UINTPTR_MAX, b_hi = 0;
    for (size_t i = 0; i < c.range_n; ++i) {
      if (c.ranges[i].start >= fc_chunk_lo && c.ranges[i].end <= fc_chunk_hi) {
        if (c.ranges[i].start < a_lo)
          a_lo = c.ranges[i].start;
        if (c.ranges[i].end > a_hi)
          a_hi = c.ranges[i].end;
      } else {
        if (c.ranges[i].start < b_lo)
          b_lo = c.ranges[i].start;
        if (c.ranges[i].end > b_hi)
          b_hi = c.ranges[i].end;
      }
    }
    c.lo_a = a_hi ? a_lo & ~(uintptr_t)4095u : 0;
    c.span_a = a_hi ? a_hi - c.lo_a : 0;
    c.lo_b = b_hi ? b_lo & ~(uintptr_t)4095u : 0;
    c.span_b = b_hi ? b_hi - c.lo_b : 0;
    /*
     * Page bitmaps for both spans (skipped for an absurdly wide span).
     */
    size_t words_a = ((c.span_a >> 12) + 64u) / 64u;
    size_t words_b = ((c.span_b >> 12) + 64u) / 64u;
    if (words_a + words_b <= ((size_t)64 << 20) / 8u &&
        fc_buf_reserve(FC_BUF_PAGEBITS, (words_a + words_b) * 8u)) {
      uint64_t *bits = (uint64_t *)fc_bufs[FC_BUF_PAGEBITS].data;
      memset(bits, 0, (words_a + words_b) * 8u);
      for (size_t i = 0; i < c.range_n; ++i) {
        bool in_a = c.span_a && c.ranges[i].start - c.lo_a < c.span_a;
        uint64_t *dst = in_a ? bits : bits + words_a;
        uintptr_t base = in_a ? c.lo_a : c.lo_b;
        for (uintptr_t pg = (c.ranges[i].start - base) >> 12;
             pg <= (c.ranges[i].end - 1u - base) >> 12; ++pg)
          dst[pg >> 6] |= UINT64_C(1) << (pg & 63u);
      }
      c.pages_a = bits;
      c.pages_b = bits + words_a;
    }
    for (size_t i = 0; i < c.range_n; ++i) {
      for (uintptr_t g = c.ranges[i].start >> 6;
           g <= (c.ranges[i].end - 1u) >> 6; ++g) {
        uint64_t bit = fc_filter_bit(g << 6);
        c.filter[bit >> 6] |= UINT64_C(1) << (bit & 63u);
      }
    }
  }
  if (sane && c.range_n > 0) {
    fc_exclude_n = 0;
    for (int i = 0; i < FC_BUF_COUNT; ++i)
      if (fc_bufs[i].data)
        fc_add_exclude((uintptr_t)fc_bufs[i].data,
                       (uintptr_t)fc_bufs[i].data + fc_bufs[i].bytes);
    fc_add_exclude((uintptr_t)rt_managed_ptr_table,
                   (uintptr_t)rt_managed_ptr_table +
                       sizeof(rt_managed_ptr_table));
    fc_add_exclude((uintptr_t)rt_managed_ptr_cache,
                   (uintptr_t)rt_managed_ptr_cache +
                       sizeof(rt_managed_ptr_cache));
    /*
     * Everything below the caller's frame on this stack is dead: frames
     * that already returned (full of stale references to this frame's
     * temporaries) and the collector's own.
     */
    if (fc_stack_low && fc_stack_low < stack_mark)
      fc_add_exclude(fc_stack_low, stack_mark);
    else
      fc_add_exclude(stack_mark - ((size_t)256 << 10), stack_mark);
    uintptr_t intern_start = 0, intern_end = 0;
    rt_fc_intern_table(&intern_start, &intern_end);
    fc_add_exclude(intern_start, intern_end);
    rt_fc_str_hash_cache(&intern_start, &intern_end);
    fc_add_exclude(intern_start, intern_end);
    rt_fc_int_str_cache(&intern_start, &intern_end);
    fc_add_exclude(intern_start, intern_end);
    /*
     * Per-thread classification caches: addresses, not references.
     */
#define FC_EXCLUDE_ARRAY(a)                                                    \
  fc_add_exclude((uintptr_t)(a), (uintptr_t)(a) + sizeof(a))
    for (size_t i = 0; i < fc_cache_reporter_n; ++i)
      fc_cache_reporters[i](fc_add_exclude);
    FC_EXCLUDE_ARRAY(rt_float_cache_keys);
    FC_EXCLUDE_ARRAY(rt_non_float_cache_keys);
    FC_EXCLUDE_ARRAY(rt_const_str_cache_keys);
    FC_EXCLUDE_ARRAY(rt_non_str_cache_keys);
    FC_EXCLUDE_ARRAY(g_rt_sso_probe_cache);
    FC_EXCLUDE_ARRAY(g_rt_probe_cache);
#undef FC_EXCLUDE_ARRAY
    fc_exclude_n += rt_fc_core_excludes(&fc_excludes[fc_exclude_n],
                                        FC_MAX_EXCLUDES - fc_exclude_n);
    bool dirty_only = !major && (fc_uffd >= 0 || fc_soft_dirty > 0);
    fc_fault_guard_install();
    scanned = fc_scan_roots(&c, dirty_only);
    fc_trace_t[3] = fc_now_ns();
    if (scanned) {
      while (c.stack_n > 0) {
        uint32_t idx = c.stack[--c.stack_n];
        /*
         * Strings, byte buffers and big integers hold no references.
         */
        if (c.cand[idx].kind == RT_FC_KIND_SSO)
          continue;
        if (c.cand[idx].kind == RT_FC_KIND_MANAGED &&
            !fc_managed_has_references(*(const int64_t *)(c.cand[idx].ptr - 8)))
          continue;
        if (c.cand[idx].kind == RT_FC_KIND_LIST) {
          /*
           * A forwarded list refers only to its current storage.
           */
          const uint64_t *hdr = (const uint64_t *)(c.cand[idx].ptr - 32);
          if (hdr[0] == UINT64_C(0x4e5954464f525744)) {
            uintptr_t target = rt_fc_list_forward_target(c.cand[idx].ptr);
            if (target) {
              if (fc_in_spans(&c, target))
                fc_mark_addr(&c, target);
              continue;
            }
          }
        }
        const fc_extent_t *x = &c.extents[idx];
        for (size_t k = 0; k < x->count; ++k) {
          uintptr_t s0 = (x->r[2 * k] + 7u) & ~(uintptr_t)7u;
          uintptr_t e0 = x->r[2 * k + 1] & ~(uintptr_t)7u;
          if (e0 > s0)
            fc_scan_words(&c, (const uintptr_t *)s0,
                          (e0 - s0) / sizeof(uintptr_t));
        }
      }
    }
    fc_fault_guard_remove();
  }
  fc_trace_t[4] = fc_now_ns();
  /*
   * Forget which pages were written. Nothing young exists from here until
   * the caller's thread allocates again, so no reference to a young object
   * can be written before the bits are clear.
   */
  if (scanned && fc_uffd < 0 && fc_soft_dirty > 0 && !fc_clear_soft_dirty())
    fc_soft_dirty = -1;
  fc_trace_t[5] = fc_now_ns();
  /*
   * Without the scan interface the kernel's share is not measured; the
   * clearing pass is about a quarter of it.
   */
  if (fc_uffd < 0)
    fc_kernel_ns = (fc_trace_t[5] - fc_trace_t[4]) * 4u;
  fc_start_world();

  size_t kept = major ? 0 : fc_old_n;
  size_t survivors_bytes = 0;
  fc_obj_t *pending = (fc_obj_t *)fc_bufs[FC_BUF_PENDING].data;
  fc_pending_n = fc_pending_pos = 0;
  if (scanned) {
    for (size_t i = 0; i < c.cand_n; ++i) {
      fc_obj_t *o = &c.cand[i];
      if (o->mark == 2)
        continue;
      size_t bytes = 0;
      for (size_t k = 0; k < c.extents[i].count; ++k)
        bytes += c.extents[i].r[2 * k + 1] - c.extents[i].r[2 * k];
      if (o->mark == 1) {
        old[kept] = *o;
        old[kept].mark = 0;
        ++kept;
        survivors_bytes += bytes;
        continue;
      }
      /*
       * Unreachable. Freed in slices by fc_sweep, not here: releasing tens
       * of thousands of objects is the longest part of a collection.
       */
      pending[fc_pending_n++] = *o;
      freed_bytes += (int64_t)bytes;
      ++fc_stat_freed_objects;
    }
  } else {
    /*
     * Nothing could be proved: keep every candidate.
     */
    for (size_t i = 0; i < c.cand_n; ++i) {
      if (c.cand[i].mark == 2)
        continue;
      old[kept] = c.cand[i];
      old[kept].mark = 0;
      ++kept;
    }
    survivors_bytes = fc_young_bytes;
  }
  fc_old_n = kept;
  fc_young_n = 0;
  fc_young_bytes = 0;
  fc_promoted_bytes = major ? 0 : fc_promoted_bytes + survivors_bytes;
  if (major)
    fc_old_after_major = fc_old_n;
  fc_stat_freed_bytes += (uint64_t)freed_bytes;
  ++fc_stat_collections;
  if (major)
    ++fc_stat_major;
  fc_last_end_ns = fc_now_ns();
  fc_stat_last_ns = fc_last_end_ns - t0;
  if (fc_stat_last_ns > fc_stat_max_ns)
    fc_stat_max_ns = fc_stat_last_ns;
  if (fc_trace_level > 0)
    fprintf(stderr,
            "[heap-collect] %s candidates=%zu marked=%zu freed=%lld bytes "
            "old=%zu scanned=%d %.2f ms (ranges %.2f sort %.2f roots %.2f "
            "mark %.2f clear %.2f queue %.2f; %zu pages, %zu words)\n",
            major ? "major" : "minor", c.cand_n, c.marked,
            (long long)freed_bytes, fc_old_n, (int)scanned,
            (double)fc_stat_last_ns / 1e6,
            (double)(fc_trace_t[1] - fc_trace_t[0]) / 1e6,
            (double)(fc_trace_t[2] - fc_trace_t[1]) / 1e6,
            (double)(fc_trace_t[3] - fc_trace_t[2]) / 1e6,
            (double)(fc_trace_t[4] - fc_trace_t[3]) / 1e6,
            (double)(fc_trace_t[5] - fc_trace_t[4]) / 1e6,
            (double)(fc_now_ns() - fc_trace_t[5]) / 1e6, fc_trace_pages,
            fc_trace_words);
  return freed_bytes;
}

/*
 * Frees queued unreachable objects: all of them, or as many as fit in
 * `budget_ns`. Returns true when the queue is empty.
 */
static bool fc_sweep(uint64_t budget_ns) {
  if (fc_pending_pos >= fc_pending_n)
    return true;
  const fc_obj_t *pending = (const fc_obj_t *)fc_bufs[FC_BUF_PENDING].data;
  uint64_t deadline = budget_ns ? fc_now_ns() + budget_ns : 0;
  fc_in_collect = 1;
  while (fc_pending_pos < fc_pending_n) {
    size_t stop = fc_pending_pos + 256;
    if (stop > fc_pending_n)
      stop = fc_pending_n;
    for (; fc_pending_pos < stop; ++fc_pending_pos)
      fc_free_object(&pending[fc_pending_pos]);
    if (deadline && fc_now_ns() >= deadline)
      break;
  }
  fc_in_collect = 0;
  if (fc_pending_pos < fc_pending_n)
    return false;
  fc_pending_n = fc_pending_pos = 0;
  return true;
}

static bool fc_start(void) {
  const char *off = getenv("NYTRIX_HEAP_COLLECT");
  if (off && (*off == '0' || strcmp(off, "off") == 0)) {
    fc_state = -1;
    return false;
  }
  const char *trace = getenv("NYTRIX_HEAP_COLLECT_TRACE");
  fc_trace_level = trace ? atoi(trace) : 0;
  const char *minor = getenv("NYTRIX_HEAP_COLLECT_BYTES");
  if (minor && atoll(minor) > 0)
    fc_minor_threshold = (size_t)atoll(minor);
  const char *major = getenv("NYTRIX_HEAP_COLLECT_MAJOR_BYTES");
  if (major && atoll(major) > 0)
    fc_major_threshold = (size_t)atoll(major);
  /*
   * The runtime's address oracle remembers pages it has seen allocations
   * on. Keep freed memory mapped so those answers stay true.
   *
   * Finding written pages costs the kernel time for every resident page of
   * the process. A program started in-process by the JIT shares it with the
   * compiler, whose freed memory is still resident: hand that back first.
   */
  malloc_trim(0);
  mallopt(M_TRIM_THRESHOLD, 1 << 30);
  mallopt(M_MMAP_THRESHOLD, 32 << 20);
  const char *track = getenv("NYTRIX_HEAP_COLLECT_TRACK");
  if (!(track && strcmp(track, "soft-dirty") == 0) &&
      !(track && strcmp(track, "none") == 0))
    (void)fc_uffd_start();
  if (fc_uffd < 0 && !(track && strcmp(track, "none") == 0))
    fc_soft_dirty = fc_probe_soft_dirty() ? 1 : -1;
  else if (fc_uffd < 0)
    fc_soft_dirty = -1;
  if (fc_uffd < 0 && fc_soft_dirty < 0)
    fc_minor_threshold *= 8;
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void *stack_addr = NULL;
    size_t stack_size = 0;
    if (pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0)
      fc_stack_low = (uintptr_t)stack_addr;
    pthread_attr_destroy(&attr);
  }
  fc_state = 1;
  g_rt_fc_tracking = 1;
  return true;
}

/*
 * mode 0: frame boundary; starts recording on the first call and collects
 *         when enough young objects have accumulated.
 * mode 1: collect young objects now.  mode 2: full collection now.
 * mode 10..15: statistics (collections, full collections, objects freed,
 *         bytes freed, last pause in microseconds, recorded old objects).
 * Returns the bytes freed by this call, or the statistic.
 */
int64_t rt_heap_collect_raw(int64_t mode) {
  switch (mode) {
  case 10:
    return (int64_t)fc_stat_collections;
  case 11:
    return (int64_t)fc_stat_major;
  case 12:
    return (int64_t)fc_stat_freed_objects;
  case 13:
    return (int64_t)fc_stat_freed_bytes;
  case 14:
    return (int64_t)(fc_stat_last_ns / 1000u);
  case 15:
    return (int64_t)fc_old_n;
  case 16:
    return (int64_t)(fc_stat_max_ns / 1000u);
  default:
    break;
  }
  if (mode < 0 || mode > 2 || fc_state < 0)
    return 0;
  if (fc_state == 0) {
    (void)fc_start();
    return 0;
  }
  if (!g_rt_fc_tracking || fc_in_collect)
    return 0; /* only the recording thread collects */
  /*
   * What the last collection found unreachable is freed a slice per frame.
   * Until that is done its memory still holds old references, so the next
   * collection waits for it.
   */
  if (!fc_sweep(mode == 0 ? UINT64_C(700000) : 0))
    return 0;
  /*
   * A full collection when enough has been promoted, by bytes or by count:
   * objects that outlive one collection and die later are found only there.
   */
  bool major = mode == 2 || fc_promoted_bytes >= fc_major_threshold ||
               fc_old_n >= fc_old_after_major * 2 + 8192;
  if (mode == 0 && !major && fc_young_bytes < fc_minor_threshold)
    return 0;
  /*
   * Asking the kernel which pages were written costs time for every
   * resident page of the process, whatever the amount of garbage: well
   * under a millisecond in a standalone program, tens of milliseconds next
   * to the JIT compiler's heap. Space collections out so that this fixed
   * part takes at most a fortieth of the time, unless young objects pile up
   * past the limit. (The rest of a collection grows with the young objects
   * themselves, so pacing on it would only make each collection larger.)
   */
  if (mode == 0 && fc_young_bytes < fc_young_limit &&
      fc_now_ns() - fc_last_end_ns < fc_kernel_ns * 40u)
    return 0;
  /*
   * Spill callee-saved registers: a reference may live only there.
   */
  ucontext_t regs;
  getcontext(&regs);
  fc_in_collect = 1;
  int64_t freed = fc_collect(major, (uintptr_t)&regs);
  fc_in_collect = 0;
  if (mode != 0)
    (void)fc_sweep(0);
  return freed;
}

#else

void rt_fc_track_slow(uintptr_t ptr, uint32_t kind, size_t bytes) {
  (void)ptr;
  (void)kind;
  (void)bytes;
}
void rt_fc_thread_enter(void) {}
void rt_fc_thread_exit(void) {}
void *rt_fc_block_alloc(size_t bytes) {
  (void)bytes;
  return NULL;
}
void rt_fc_block_free(void *ptr, size_t bytes) {
  (void)bytes;
  rt_oracle_free(ptr);
}
bool rt_fc_pool_owns(const void *ptr) {
  (void)ptr;
  return false;
}
#if defined(__linux__) && defined(__GNUC__)
void rt_fc_register_cache_reporter(void (*report)(void (*add)(uintptr_t,
                                                              uintptr_t))) {
  (void)report;
}
#endif
int64_t rt_heap_collect_raw(int64_t mode) {
  (void)mode;
  return 0;
}

#endif

int64_t rt_heap_collect(int64_t mode) {
  return rt_tag_v(rt_heap_collect_raw(rt_any_to_i64(mode)));
}
