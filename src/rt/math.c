#include "rt/shared.h"

static uint64_t __rng_state = 0x123456789ABCDEF0ULL;
static int __rng_forced_prng = 0;

int64_t __srand(int64_t s) {
  __rng_state = (uint64_t)(s >> 1);
  __rng_forced_prng = 1;
  return s;
}

int64_t __rand64(void) {
  uint64_t val = 0;
  int ok = 0;
#if defined(__x86_64__)
  if (!__rng_forced_prng) {
    __asm__ volatile("rdrand %0; setc %b1" : "=r"(val), "=q"(ok));
  }
#endif
  if (!ok) { // TODO proper way for non arch
    __rng_state += 0x9e3779b97f4a7c15ULL;
    uint64_t z = __rng_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    val = z ^ (z >> 31);
  }
  uint64_t res = ((uint64_t)(val & 0x3FFFFFFFFFFFFFFFULL) << 1) | 1ULL;
  return (int64_t)res;
}

// Thread-local slab allocator for floats
// Slots are 16 bytes: [next_ptr (8) | unused (8)] when free
//                     [tag (8)      | value (8)] when allocated
// Tag is at offset -8 relative to returned pointer.
// Value is at offset 0 relative to returned pointer.
// So:
// [ ... header (not used for floats) ... ]
// Slot:
// [ Word 0 ] [ Word 1 ]
//
// When allocated:
// Word 0: TAG_FLOAT (stored at p - 8)
// Word 1: double bits (stored at p)
//
// When free:
// Word 0: next free pointer
// Word 1: unused

static __thread void *g_flt_cache = NULL;

static void *__flt_alloc_slot(void) {
  if (g_flt_cache) {
    void *p = g_flt_cache;
    g_flt_cache = *(void **)p;
    return p;
  }
  // Allocate a new chunk (4KB)
  size_t chunk_size = 4096;
  char *chunk = (char *)__malloc((int64_t)((chunk_size << 1) | 1));
  if (!chunk)
    return NULL;

  // We can't use the standard header of the chunk because __malloc puts its own
  // heavy headers. We just treat the payload as a raw buffer of slots.
  // __malloc returns a pointer to the payload.

  // Link all slots in the new chunk
  // Each slot is 16 bytes.
  // Note: Since __malloc returns 64-byte aligned pointers, we are good on alignment.

  size_t slot_count = chunk_size / 16;
  // We skip the first 64 bytes if we were doing manual page management, but here
  // __malloc handles the page. We just use the payload.
  // Wait, __malloc returns a Nytrix object. It has headers at -64.
  // We are using `__malloc` from the runtime, which is a heavy allocator.
  // This means our slab blocks are themselves heavy objects. This is fine,
  // we just leak them until program exit (or some future GC sweep).
  // The goal is to make individual float allocations cheap.

  // Link slots 0 to N-2 to point to the next one.
  for (size_t i = 0; i < slot_count - 1; i++) {
    void *curr = chunk + (i * 16);
    void *next = chunk + ((i + 1) * 16);
    *(void **)curr = next;
  }
  *(void **)(chunk + ((slot_count - 1) * 16)) = NULL;

  g_flt_cache = (void *)chunk;

  // Pop one immediately
  void *p = g_flt_cache;
  g_flt_cache = *(void **)p;
  return p;
}

void __flt_free(int64_t v) {
  if (!v) return;
  // v points to the value. The slot starts at v - 8.
  void *slot = (void *)((char *)(uintptr_t)v - 8);
  *(void **)slot = g_flt_cache;
  g_flt_cache = slot;
}

int64_t __flt_box_val(int64_t bits) {
  void *slot = __flt_alloc_slot();
  if (!slot) return 0; // Should panic or handle OOM

  // Slot layout: [ Word 0 ] [ Word 1 ]
  // Word 0 is at `slot`. Word 1 is at `slot + 8`.
  // We return `slot + 8`.
  // Tag goes at `slot` (offset -8 from return).
  // Value goes at `slot + 8` (offset 0 from return).

  *(int64_t *)slot = TAG_FLOAT;
  memcpy((char *)slot + 8, &bits, 8);

  return (int64_t)(uintptr_t)((char *)slot + 8);
}

int64_t __flt_box_val32(int64_t bits32) {
  uint32_t raw = 0;
  if (bits32 & 1) {
    raw = (uint32_t)(bits32 >> 1);
  } else {
    raw = (uint32_t)bits32;
  }
  float f = 0.0f;
  memcpy(&f, &raw, sizeof(f));
  double d = (double)f;
  int64_t bits64 = 0;
  memcpy(&bits64, &d, sizeof(bits64));
  return __flt_box_val(bits64);
}

int64_t __flt_unbox_val(int64_t v) {
  if (v & 1) {
    double d = (double)(v >> 1);
    int64_t res;
    memcpy(&res, &d, 8);
    return res;
  }
  /* is_v_flt already confirms the tag at v-8; skip rt_addr_readable. */
  if (is_v_flt(v)) {
    int64_t bits;
    memcpy(&bits, (const void *)(uintptr_t)v, sizeof(bits));
    return bits;
  }
  return 0;
}

int64_t __flt_unbox_val32(int64_t v) {
  double d = 0;
  if (v & 1) {
    d = (double)(v >> 1);
  } else if (is_v_flt(v)) {
    memcpy(&d, (const void *)(uintptr_t)v, sizeof(d));
  }
  float f = (float)d;
  uint32_t b;
  memcpy(&b, &f, 4);
  return (int64_t)b << 1 | 1;
}

int64_t __flt_from_int(int64_t v) {
  if (is_int(v)) {
    double d = (double)(v >> 1);
    int64_t res;
    memcpy(&res, &d, 8);
    return res;
  }
  return 0;
}

int64_t __flt_to_int(int64_t v) {
  int64_t b = __flt_unbox_val(v);
  double d;
  memcpy(&d, &b, 8);
  int64_t i = (int64_t)d;
  return rt_tag_v((int64_t)i);
}

int64_t __flt_trunc(int64_t v) { return __flt_to_int(v); }

#define FLT_OP(name, op)                                                       \
  int64_t __flt_##name(int64_t a, int64_t b) {                                 \
    double da, db;                                                             \
    int64_t ba = __flt_unbox_val(a);                                           \
    int64_t bb = __flt_unbox_val(b);                                           \
    memcpy(&da, &ba, 8);                                                       \
    memcpy(&db, &bb, 8);                                                       \
    double r = da op db;                                                       \
    int64_t rr;                                                                \
    memcpy(&rr, &r, 8);                                                        \
    return __flt_box_val(rr);                                                  \
  }

FLT_OP(add, +)
FLT_OP(sub, -)
FLT_OP(mul, *)
FLT_OP(div, /)

#define FLT_CMP(name, op)                                                      \
  int64_t __flt_##name(int64_t a, int64_t b) {                                 \
    double da, db;                                                             \
    int64_t ba = __flt_unbox_val(a);                                           \
    int64_t bb = __flt_unbox_val(b);                                           \
    memcpy(&da, &ba, 8);                                                       \
    memcpy(&db, &bb, 8);                                                       \
    return (da op db) ? 2 : 4;                                                 \
  }

FLT_CMP(lt, <)
FLT_CMP(gt, >)
FLT_CMP(le, <=)
FLT_CMP(ge, >=)
FLT_CMP(eq, ==)

int64_t __add(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (int64_t)((uint64_t)a + (uint64_t)b - 1);
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_add(a, b);
  if (is_v_str(a) && is_v_str(b))
    return __str_concat(a, b);
  if (is_any_ptr(a) && is_int(b))
    return a + (b >> 1);
  if (is_int(a) && is_any_ptr(b))
    return b + (a >> 1);
  return 1;
}

int64_t __sub(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (int64_t)((uint64_t)a - (uint64_t)b + 1);
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_sub(a, b);
  if (is_any_ptr(a) && is_int(b))
    return a - (b >> 1);
  if (is_any_ptr(a) && is_any_ptr(b))
    return (int64_t)(((uint64_t)(a - b) << 1) | 1);
  return 1;
}

int64_t __mul(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (int64_t)((((uint64_t)(a >> 1) * (uint64_t)(b >> 1)) << 1) | 1);
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_mul(a, b);
  return 0;
}

int64_t __div(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b)) {
    int64_t vb = b >> 1;
    if (vb == 0)
      return 0;
    return (int64_t)((((uint64_t)(a >> 1) / (uint64_t)vb) << 1) | 1);
  }
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_div(a, b);
  return 0;
}

int64_t __mod(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b)) {
    int64_t vb = b >> 1;
    if (vb == 0)
      return 1;
    return (int64_t)((((uint64_t)(a >> 1) % (uint64_t)vb) << 1) | 1);
  }
  return b ? a % b : 1;
}

int64_t __eq(int64_t a, int64_t b) {
  if (a == b)
    return 2;
  if ((a == 0 && b == 1) || (a == 1 && b == 0))
    return 2;
  if ((a & 1) != (b & 1))
    return 4;
  if (is_ptr(a) && is_ptr(b)) {
    if (a <= 4 || b <= 4)
      return 4;
    if (is_v_flt(a) || is_v_flt(b))
      return __flt_eq(a, b);
    if (is_v_str(a) && is_v_str(b)) {
      uintptr_t la_p = (uintptr_t)a - 16;
      uintptr_t lb_p = (uintptr_t)b - 16;
      if (!rt_addr_readable(la_p, sizeof(int64_t)) ||
          !rt_addr_readable(lb_p, sizeof(int64_t)))
        return 4;
      int64_t la_tagged = 0, lb_tagged = 0;
      memcpy(&la_tagged, (const void *)la_p, sizeof(la_tagged));
      memcpy(&lb_tagged, (const void *)lb_p, sizeof(lb_tagged));
      if (!is_int(la_tagged) || !is_int(lb_tagged))
        return 4;
      if (la_tagged < 0 || lb_tagged < 0)
        return 4;
      size_t la = (size_t)(la_tagged >> 1);
      size_t lb = (size_t)(lb_tagged >> 1);
      if (la != lb)
        return 4;
      if (la == 0)
        return 2;
      if (!rt_addr_readable((uintptr_t)a, la) ||
          !rt_addr_readable((uintptr_t)b, lb))
        return 4;
      return memcmp((const void *)(uintptr_t)a, (const void *)(uintptr_t)b,
                    la) == 0
                 ? 2
                 : 4;
    }
  }
  return 4;
}

int64_t __lt(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) < (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_lt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a < b ? 2 : 4;
  return 4;
}
int64_t __le(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) <= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_le(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a <= b ? 2 : 4;
  return 4;
}
int64_t __gt(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) > (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_gt(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a > b ? 2 : 4;
  return 4;
}
int64_t __ge(int64_t a, int64_t b) {
  if (is_int(a) && is_int(b))
    return (a >> 1) >= (b >> 1) ? 2 : 4;
  if (is_v_flt(a) || is_v_flt(b))
    return __flt_ge(a, b);
  if (is_ptr(a) && is_ptr(b))
    return a >= b ? 2 : 4;
  return 4;
}

int64_t __and(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) &
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __or(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) |
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __xor(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) ^
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __shl(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a)
                     << (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __shr(int64_t a, int64_t b) {
  return (int64_t)((((uint64_t)(a & 1 ? a >> 1 : a) >>
                     (uint64_t)(b & 1 ? b >> 1 : b)))
                       << 1 |
                   1);
}
int64_t __not(int64_t a) {
  return (int64_t)(((~(uint64_t)(a & 1 ? a >> 1 : a)) << 1) | 1);
}
