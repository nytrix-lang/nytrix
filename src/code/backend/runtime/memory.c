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
static _Atomic(uint64_t) rt_managed_ptr_epoch;
#define RT_MANAGED_PTR_CACHE_SIZE 8u
typedef struct rt_managed_ptr_cache_entry {
  uintptr_t value;
  uint64_t epoch;
} rt_managed_ptr_cache_entry_t;
static _Thread_local rt_managed_ptr_cache_entry_t
    rt_managed_ptr_cache[RT_MANAGED_PTR_CACHE_SIZE];

static size_t rt_managed_ptr_slot(uintptr_t value) {
  /*
   * Managed payloads are aligned and commonly adjacent. Keep their low
   * address bits local in the table, folding higher regions into the slot
   * instead of scattering every allocation onto an unrelated cache line.
   * Exact keys and open-addressing probes still resolve collisions.
   */
  uint64_t x = (uint64_t)value >> 4;
  /*
   * Thread arenas sit 64 MiB apart, exactly one table size in this key, so
   * XOR-folding the region left them on the same slots and linear probes grew
   * with every thread. An odd multiplier moves each region far away while
   * adjacent payloads stay adjacent.
   */
  x += (x >> RT_MANAGED_PTR_TABLE_BITS) * UINT64_C(0x9E3779B97F4A7C15);
  return (size_t)x & RT_MANAGED_PTR_TABLE_MASK;
}

static void rt_managed_ptr_register(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  size_t tombstone = SIZE_MAX;
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_acquire);
    if (seen == value)
      return;
    if (seen == RT_MANAGED_PTR_TOMBSTONE && tombstone == SIZE_MAX)
      tombstone = slot;
    /*
     * The allocator hands out addresses that are not registered, so the
     * duplicate check only needs a short window. Scanning on to an empty slot
     * walked every tombstone left by earlier frees (97% of the time in a
     * defer-heavy loop).
     */
    if (tombstone != SIZE_MAX && probe >= RT_MANAGED_PTR_REUSE_WINDOW) {
      uintptr_t reusable = RT_MANAGED_PTR_TOMBSTONE;
      if (atomic_compare_exchange_strong_explicit(
              &rt_managed_ptr_table[tombstone], &reusable, value,
              memory_order_release, memory_order_relaxed))
        return;
      probe = (size_t)-1;
      tombstone = SIZE_MAX;
      continue;
    }
    if (!seen) {
      size_t target = tombstone == SIZE_MAX ? slot : tombstone;
      uintptr_t expected = tombstone == SIZE_MAX ? 0 : RT_MANAGED_PTR_TOMBSTONE;
      if (atomic_compare_exchange_strong_explicit(
              &rt_managed_ptr_table[target], &expected, value,
              memory_order_release, memory_order_relaxed))
        return;
      probe = (size_t)-1;
      tombstone = SIZE_MAX;
    }
  }
  if (tombstone != SIZE_MAX) {
    uintptr_t expected = RT_MANAGED_PTR_TOMBSTONE;
    (void)atomic_compare_exchange_strong_explicit(
        &rt_managed_ptr_table[tombstone], &expected, value,
        memory_order_release, memory_order_relaxed);
  }
}

static void rt_managed_ptr_unregister(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return;
  size_t start = rt_managed_ptr_slot(value);
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    size_t slot = (start + probe) & RT_MANAGED_PTR_TABLE_MASK;
    uintptr_t seen =
        atomic_load_explicit(&rt_managed_ptr_table[slot], memory_order_acquire);
    if (!seen)
      return;
    if (seen == value) {
      if (atomic_compare_exchange_strong_explicit(
              &rt_managed_ptr_table[slot], &seen, RT_MANAGED_PTR_TOMBSTONE,
              memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&rt_managed_ptr_epoch, 1,
                                  memory_order_release);
        return;
      }
      probe = (size_t)-1;
    }
  }
}

bool rt_heap_ptr_registry_has(uintptr_t value) {
  if (!value || value == RT_MANAGED_PTR_TOMBSTONE)
    return false;
  size_t start = rt_managed_ptr_slot(value);
  uint64_t epoch =
      atomic_load_explicit(&rt_managed_ptr_epoch, memory_order_acquire);
  rt_managed_ptr_cache_entry_t *cached =
      &rt_managed_ptr_cache[start & (RT_MANAGED_PTR_CACHE_SIZE - 1u)];
  if (cached->value == value && cached->epoch == epoch)
    return true;
  for (size_t probe = 0; probe < RT_MANAGED_PTR_TABLE_SIZE; ++probe) {
    uintptr_t seen = atomic_load_explicit(
        &rt_managed_ptr_table[(start + probe) & RT_MANAGED_PTR_TABLE_MASK],
        memory_order_acquire);
    if (seen == value) {
      cached->value = value;
      cached->epoch = epoch;
      return true;
    }
    if (!seen)
      return false;
  }
  return false;
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
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

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
static int g_sh_handlers;

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

static void sh_fault(int sig, siginfo_t *info, void *ctx) {
  if (getenv("NYTRIX_SAFE_HEAP_TRACE")) {
    ucontext_t *uc = (ucontext_t *)ctx;
#if defined(__x86_64__)
    void *pc = (void *)uc->uc_mcontext.gregs[REG_RIP];
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname)
      fprintf(stderr, "Nytrix safe-heap: fault pc=%p in %s+%#lx\n", pc,
              di.dli_sname, (unsigned long)((char *)pc - (char *)di.dli_saddr));
    else
      fprintf(stderr, "Nytrix safe-heap: fault pc=%p\n", pc);
#else
    (void)uc;
#endif
  }
  uintptr_t addr = (uintptr_t)info->si_addr;
  const sh_entry_t *hit = sh_find_address(addr);
  if (!hit) {
    signal(sig, SIG_DFL);
    return; /* not ours: re-fault into the default action */
  }
  if (hit->freed)
    sh_message("use-after-free", addr, hit);
  else if (addr >= hit->user + hit->size)
    sh_message("heap-buffer-overflow", addr, hit);
  else
    sh_message("invalid access", addr, hit);
  _exit(134);
}

static void sh_install(void) {
  if (g_sh_handlers)
    return;
  g_sh_handlers = 1;
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
}

static void sh_leak_report(void) {
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
  munmap((void *)e->map, e->map_len);
  e->user = 1; /* tombstone */
}

/*
 * Allocate `size` bytes whose last byte abuts a guard page. `lead` extra
 * bytes are reserved before the payload (the managed-heap header).
 */
static void *sh_alloc(size_t size, size_t lead, bool user_block) {
  pthread_mutex_lock(&g_sh_lock);
  sh_install();
  static bool leak_hook;
  if (!leak_hook) {
    leak_hook = true;
    atexit(sh_leak_report);
  }
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
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
  void *m = mmap(NULL, map_len, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) {
    pthread_mutex_unlock(&g_sh_lock);
    return NULL;
  }
  uintptr_t guard = (uintptr_t)m + data_pages * page;
  mprotect((void *)guard, page, PROT_NONE);
  uintptr_t user = guard - body;
  sh_entry_t *e = sh_insert(user);
  if (!e) {
    munmap(m, map_len);
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
  mprotect((void *)e->map, e->prot_len, PROT_NONE);
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
  void *p = NULL;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = ny_aligned_alloc(16, (slot >= 0) ? g_pool_sizes[slot] : total);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
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
  rt_map_oracle_add((uintptr_t)p, (size_t)total);
  rt_managed_ptr_register((uintptr_t)res);
  rt_rc_adopt_new(res);
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
  void *p = NULL;
  if (slot >= 0 && g_mem_pools[slot]) {
    p = g_mem_pools[slot];
    g_mem_pools[slot] = g_mem_pools[slot]->next;
  } else {
    p = ny_aligned_alloc(16, (slot >= 0) ? g_pool_sizes[slot] : total);
  }

  if (__builtin_expect(!p, 0))
    return 0;

  size_t fill_size = (slot >= 0) ? g_pool_sizes[slot] : total;
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
 * Keep lookup chains short for the several-hundred-thousand live raw blocks.
 */
#define RAW_PTR_HASH_BUCKETS (1u << 18)
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
    ny_aligned_free(base);
    return 1;
  } else if (raw_ptr_unregister((uintptr_t)ptr)) {
    free((void *)(uintptr_t)ptr);
    return 1;
  } else if (is_v_flt(ptr)) {
    rt_flt_free(ptr);
    return 1;
  }
  return 0;
}

/* Bumped by every free so per-thread address caches can drop stale bases. */
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
    free((void *)(uintptr_t)ptr);
    return 1;
  }
  int tbuf_release = rt_tbuf_release_handle(ptr);
  if (tbuf_release > 0) {
    free((void *)((uintptr_t)ptr - RT_NATIVE_TBUF_HEADER));
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
  void *grown = realloc((void *)(uintptr_t)p, (size_t)size);
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
  if (!is_v_str(value) && !rt_native_is_str(value))
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
 * The last raw (non-heap) base a 32-bit store went through after its full
 * checks, per thread. Vertex writes into a mapped GPU buffer repeat the same
 * base with growing offsets; for those, the heap registry lookup and the
 * oob probe answered the same thing every time (7% of a UI frame).
 */
static __thread uintptr_t rt_store32_raw_base;
static __thread uint64_t rt_store32_raw_epoch;

int64_t rt_store32_idx(int64_t addr, int64_t idx, int64_t val) {
  RT_UNTAG_INT(idx);
  if ((uintptr_t)addr == rt_store32_raw_base && idx >= 0 &&
      rt_store32_raw_epoch ==
          atomic_load_explicit(&rt_mem_free_epoch, memory_order_acquire)) {
    uintptr_t q = (uintptr_t)((intptr_t)addr + (intptr_t)idx);
    if (q >= 0x1000 && rt_probe_cache_hit(q, 4)) {
      int64_t v = (val & 1) ? (val >> 1) : val;
      uint32_t raw = (uint32_t)v;
      memcpy((void *)q, &raw, sizeof(raw));
      return val;
    }
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
  if (!hdr && !heap) {
    rt_store32_raw_base = (uintptr_t)addr;
    rt_store32_raw_epoch =
        atomic_load_explicit(&rt_mem_free_epoch, memory_order_acquire);
  }
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
