#include "rt/shared.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_acle.h>
#include <arm_neon.h>
#endif

static int rt_simmd_str_eq_lit(int64_t v, const char *lit) {
  if (!lit || !is_v_str(v))
    return 0;
  size_t n = rt_tagged_str_len(v);
  size_t m = strlen(lit);
  return n == m && memcmp((const void *)(uintptr_t)v, lit, n) == 0;
}

static uint64_t rt_simmd_u64(int64_t v) {
  return (uint64_t)(is_int(v) ? rt_untag_v(v) : v);
}

static int64_t rt_simmd_tag_u64(uint64_t v) {
  return rt_tag_v((int64_t)v);
}

int64_t rt_simmd_has_feature(int64_t name_v) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
  __builtin_cpu_init();
  if (rt_simmd_str_eq_lit(name_v, "sse3"))
    return __builtin_cpu_supports("sse3") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "ssse3"))
    return __builtin_cpu_supports("ssse3") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "sse2"))
    return __builtin_cpu_supports("sse2") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "sse4.1") || rt_simmd_str_eq_lit(name_v, "sse41"))
    return __builtin_cpu_supports("sse4.1") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "sse4.2") || rt_simmd_str_eq_lit(name_v, "sse42"))
    return __builtin_cpu_supports("sse4.2") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "crc32") || rt_simmd_str_eq_lit(name_v, "crc32c"))
    return __builtin_cpu_supports("sse4.2") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "avx"))
    return __builtin_cpu_supports("avx") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "avx2"))
    return __builtin_cpu_supports("avx2") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "avx512f"))
    return __builtin_cpu_supports("avx512f") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "avx512bw"))
    return __builtin_cpu_supports("avx512bw") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "avx512vl"))
    return __builtin_cpu_supports("avx512vl") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "bmi") || rt_simmd_str_eq_lit(name_v, "bmi1"))
    return __builtin_cpu_supports("bmi") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "bmi2"))
    return __builtin_cpu_supports("bmi2") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "lzcnt"))
    return __builtin_cpu_supports("lzcnt") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "fma"))
    return __builtin_cpu_supports("fma") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "popcnt"))
    return __builtin_cpu_supports("popcnt") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "aes"))
    return __builtin_cpu_supports("aes") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "pclmul") || rt_simmd_str_eq_lit(name_v, "pclmulqdq"))
    return __builtin_cpu_supports("pclmul") ? NY_IMM_TRUE : NY_IMM_FALSE;
  if (rt_simmd_str_eq_lit(name_v, "sha"))
    return __builtin_cpu_supports("sha") ? NY_IMM_TRUE : NY_IMM_FALSE;
#else
  if (rt_simmd_str_eq_lit(name_v, "sse2"))
    return NY_IMM_TRUE;
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
  if (rt_simmd_str_eq_lit(name_v, "neon") || rt_simmd_str_eq_lit(name_v, "asimd"))
    return NY_IMM_TRUE;
#if defined(__ARM_FEATURE_CRC32)
  if (rt_simmd_str_eq_lit(name_v, "crc32") || rt_simmd_str_eq_lit(name_v, "crc32c"))
    return NY_IMM_TRUE;
#endif
#if defined(__ARM_FEATURE_CRYPTO)
  if (rt_simmd_str_eq_lit(name_v, "aes") || rt_simmd_str_eq_lit(name_v, "sha"))
    return NY_IMM_TRUE;
#endif
#endif
  return NY_IMM_FALSE;
}

int64_t rt_simmd_popcnt64(int64_t v) {
  return rt_tag_v((int64_t)__builtin_popcountll(rt_simmd_u64(v)));
}

int64_t rt_simmd_ctz64(int64_t v) {
  uint64_t x = rt_simmd_u64(v);
  return rt_tag_v(x ? (int64_t)__builtin_ctzll(x) : 64);
}

int64_t rt_simmd_clz64(int64_t v) {
  uint64_t x = rt_simmd_u64(v);
  return rt_tag_v(x ? (int64_t)__builtin_clzll(x) : 64);
}

int64_t rt_simmd_bswap64(int64_t v) {
  return rt_simmd_tag_u64(__builtin_bswap64(rt_simmd_u64(v)));
}

int64_t rt_simmd_popcnt32(int64_t v) {
  return rt_tag_v((int64_t)__builtin_popcount((uint32_t)rt_simmd_u64(v)));
}

int64_t rt_simmd_ctz32(int64_t v) {
  uint32_t x = (uint32_t)rt_simmd_u64(v);
  return rt_tag_v(x ? (int64_t)__builtin_ctz(x) : 32);
}

int64_t rt_simmd_clz32(int64_t v) {
  uint32_t x = (uint32_t)rt_simmd_u64(v);
  return rt_tag_v(x ? (int64_t)__builtin_clz(x) : 32);
}

int64_t rt_simmd_bswap32(int64_t v) {
  return rt_tag_v((int64_t)__builtin_bswap32((uint32_t)rt_simmd_u64(v)));
}

int64_t rt_simmd_rotl32(int64_t v, int64_t k_v) {
  uint32_t x = (uint32_t)rt_simmd_u64(v);
  unsigned k = (unsigned)(rt_simmd_u64(k_v) & 31u);
  return rt_tag_v((int64_t)(uint32_t)(k ? ((x << k) | (x >> (32u - k))) : x));
}

int64_t rt_simmd_rotr32(int64_t v, int64_t k_v) {
  uint32_t x = (uint32_t)rt_simmd_u64(v);
  unsigned k = (unsigned)(rt_simmd_u64(k_v) & 31u);
  return rt_tag_v((int64_t)(uint32_t)(k ? ((x >> k) | (x << (32u - k))) : x));
}

int64_t rt_simmd_rotl64(int64_t v, int64_t k_v) {
  uint64_t x = rt_simmd_u64(v);
  unsigned k = (unsigned)(rt_simmd_u64(k_v) & 63u);
  return rt_simmd_tag_u64(k ? ((x << k) | (x >> (64u - k))) : x);
}

int64_t rt_simmd_rotr64(int64_t v, int64_t k_v) {
  uint64_t x = rt_simmd_u64(v);
  unsigned k = (unsigned)(rt_simmd_u64(k_v) & 63u);
  return rt_simmd_tag_u64(k ? ((x >> k) | (x << (64u - k))) : x);
}

int64_t rt_simmd_prefetch(int64_t ptr_v, int64_t rw_v, int64_t locality_v) {
  const void *p = (const void *)(uintptr_t)(is_int(ptr_v) ? rt_untag_v(ptr_v) : ptr_v);
  int rw = rt_simmd_u64(rw_v) ? 1 : 0;
  int loc = (int)rt_simmd_u64(locality_v);
  if (loc < 0)
    loc = 0;
  if (loc > 3)
    loc = 3;
#if defined(__GNUC__) || defined(__clang__)
  if (rw) {
    switch (loc) {
    case 0:
      __builtin_prefetch(p, 1, 0);
      break;
    case 1:
      __builtin_prefetch(p, 1, 1);
      break;
    case 2:
      __builtin_prefetch(p, 1, 2);
      break;
    default:
      __builtin_prefetch(p, 1, 3);
      break;
    }
  } else {
    switch (loc) {
    case 0:
      __builtin_prefetch(p, 0, 0);
      break;
    case 1:
      __builtin_prefetch(p, 0, 1);
      break;
    case 2:
      __builtin_prefetch(p, 0, 2);
      break;
    default:
      __builtin_prefetch(p, 0, 3);
      break;
    }
  }
#endif
  return ptr_v;
}

int64_t rt_simmd_pause(void) {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
#if defined(__GNUC__) || defined(__clang__)
  __asm__ __volatile__("yield" ::: "memory");
#endif
#endif
  return rt_tag_v(0);
}

int64_t rt_simmd_lfence(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
  _mm_lfence();
#elif defined(__GNUC__) || defined(__clang__)
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
  return rt_tag_v(0);
}

int64_t rt_simmd_sfence(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
  _mm_sfence();
#elif defined(__GNUC__) || defined(__clang__)
  __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
  return rt_tag_v(0);
}

int64_t rt_simmd_mfence(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
  _mm_mfence();
#elif defined(__GNUC__) || defined(__clang__)
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
  return rt_tag_v(0);
}

int64_t rt_simmd_rdtsc(void) {
  uint64_t t = 0;
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  unsigned lo = 0, hi = 0;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  t = ((uint64_t)hi << 32) | lo;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  t = __rdtsc();
#endif
  return rt_tag_v((int64_t)(t & UINT64_C(0x3fffffffffffffff)));
}

static uint32_t rt_simmd_crc32_u8_scalar(uint32_t crc, uint8_t b) {
  crc ^= b;
  for (int i = 0; i < 8; i++)
    crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & (0u - (crc & 1u)));
  return crc;
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2"))) static uint32_t rt_simmd_crc32_u8_sse42(uint32_t crc,
                                                                          uint8_t b) {
  return _mm_crc32_u8(crc, b);
}
#endif

int64_t rt_simmd_crc32_u8(int64_t crc_v, int64_t byte_v) {
  uint32_t crc = (uint32_t)rt_simmd_u64(crc_v);
  uint8_t b = (uint8_t)rt_simmd_u64(byte_v);
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("sse4.2"))
    crc = rt_simmd_crc32_u8_sse42(crc, b);
  else
    crc = rt_simmd_crc32_u8_scalar(crc, b);
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  crc = __crc32cb(crc, b);
#else
  crc = rt_simmd_crc32_u8_scalar(crc, b);
#endif
  return rt_tag_v((int64_t)crc);
}

static uint32_t rt_simmd_crc32_u64_scalar(uint32_t crc, uint64_t x) {
  for (int i = 0; i < 8; i++)
    crc = rt_simmd_crc32_u8_scalar(crc, (uint8_t)(x >> (unsigned)(i * 8)));
  return crc;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2"))) static uint32_t rt_simmd_crc32_u64_sse42(uint32_t crc,
                                                                           uint64_t x) {
  return (uint32_t)_mm_crc32_u64((uint64_t)crc, x);
}
#endif

int64_t rt_simmd_crc32_u64(int64_t crc_v, int64_t x_v) {
  uint32_t crc = (uint32_t)rt_simmd_u64(crc_v);
  uint64_t x = rt_simmd_u64(x_v);
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("sse4.2"))
    crc = rt_simmd_crc32_u64_sse42(crc, x);
  else
    crc = rt_simmd_crc32_u64_scalar(crc, x);
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  crc = (uint32_t)__crc32cd(crc, x);
#else
  crc = rt_simmd_crc32_u64_scalar(crc, x);
#endif
  return rt_tag_v((int64_t)crc);
}

static uint64_t rt_simmd_pext64_scalar(uint64_t x, uint64_t mask) {
  uint64_t out = 0;
  uint64_t bit = 1;
  while (mask) {
    uint64_t low = mask & (UINT64_C(0) - mask);
    if (x & low)
      out |= bit;
    mask ^= low;
    bit <<= 1;
  }
  return out;
}

static uint64_t rt_simmd_pdep64_scalar(uint64_t x, uint64_t mask) {
  uint64_t out = 0;
  uint64_t bit = 1;
  while (mask) {
    uint64_t low = mask & (UINT64_C(0) - mask);
    if (x & bit)
      out |= low;
    mask ^= low;
    bit <<= 1;
  }
  return out;
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("bmi2"))) static uint64_t rt_simmd_pext64_bmi2(uint64_t x,
                                                                     uint64_t mask) {
  return _pext_u64(x, mask);
}

__attribute__((target("bmi2"))) static uint64_t rt_simmd_pdep64_bmi2(uint64_t x,
                                                                     uint64_t mask) {
  return _pdep_u64(x, mask);
}
#endif

int64_t rt_simmd_pext64(int64_t x_v, int64_t mask_v) {
  uint64_t x = rt_simmd_u64(x_v);
  uint64_t mask = rt_simmd_u64(mask_v);
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("bmi2"))
    return rt_simmd_tag_u64(rt_simmd_pext64_bmi2(x, mask));
#endif
  return rt_simmd_tag_u64(rt_simmd_pext64_scalar(x, mask));
}

int64_t rt_simmd_pdep64(int64_t x_v, int64_t mask_v) {
  uint64_t x = rt_simmd_u64(x_v);
  uint64_t mask = rt_simmd_u64(mask_v);
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("bmi2"))
    return rt_simmd_tag_u64(rt_simmd_pdep64_bmi2(x, mask));
#endif
  return rt_simmd_tag_u64(rt_simmd_pdep64_scalar(x, mask));
}

static void rt_simmd_clmul64_scalar(uint64_t x, uint64_t y, uint64_t *lo, uint64_t *hi) {
  uint64_t rlo = 0;
  uint64_t rhi = 0;
  for (unsigned i = 0; i < 64; i++) {
    if (((y >> i) & 1u) == 0)
      continue;
    rlo ^= x << i;
    if (i)
      rhi ^= x >> (64u - i);
  }
  *lo = rlo;
  *hi = rhi;
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("pclmul"))) static void rt_simmd_clmul64_pclmul(uint64_t x, uint64_t y,
                                                                      uint64_t *lo,
                                                                      uint64_t *hi) {
  __m128i a = _mm_set_epi64x(0, (int64_t)x);
  __m128i b = _mm_set_epi64x(0, (int64_t)y);
  __m128i r = _mm_clmulepi64_si128(a, b, 0x00);
  uint64_t out[2];
  _mm_storeu_si128((__m128i *)(void *)out, r);
  *lo = out[0];
  *hi = out[1];
}
#endif

static void rt_simmd_clmul64(uint64_t x, uint64_t y, uint64_t *lo, uint64_t *hi) {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("pclmul")) {
    rt_simmd_clmul64_pclmul(x, y, lo, hi);
    return;
  }
#endif
  rt_simmd_clmul64_scalar(x, y, lo, hi);
}

int64_t rt_simmd_clmul64_lo(int64_t x_v, int64_t y_v) {
  uint64_t lo = 0, hi = 0;
  rt_simmd_clmul64(rt_simmd_u64(x_v), rt_simmd_u64(y_v), &lo, &hi);
  (void)hi;
  return rt_simmd_tag_u64(lo);
}

int64_t rt_simmd_clmul64_hi(int64_t x_v, int64_t y_v) {
  uint64_t lo = 0, hi = 0;
  rt_simmd_clmul64(rt_simmd_u64(x_v), rt_simmd_u64(y_v), &lo, &hi);
  (void)lo;
  return rt_simmd_tag_u64(hi);
}

static uint8_t *rt_simmd_ptr(int64_t v) {
  return (uint8_t *)(uintptr_t)(is_int(v) ? rt_untag_v(v) : v);
}

static uint32_t rt_simmd_hash_u32(uint32_t x) {
  x ^= x >> 16;
  x *= UINT32_C(0x7feb352d);
  x ^= x >> 15;
  x *= UINT32_C(0x846ca68b);
  x ^= x >> 16;
  return x;
}

int64_t rt_simmd_i32_hash_put_ptr(int64_t keys_v, int64_t values_v, int64_t used_v,
                                  int64_t cap_v, int64_t key_v, int64_t value_v) {
  int32_t *keys = (int32_t *)rt_simmd_ptr(keys_v);
  int32_t *values = (int32_t *)rt_simmd_ptr(values_v);
  uint8_t *used = rt_simmd_ptr(used_v);
  size_t cap = (size_t)rt_simmd_u64(cap_v);
  int32_t key = (int32_t)rt_simmd_u64(key_v);
  int32_t value = (int32_t)rt_simmd_u64(value_v);
  if (!keys || !values || !used || cap == 0 || (cap & (cap - 1)) != 0)
    return NY_IMM_FALSE;
  size_t mask = cap - 1;
  size_t idx = (size_t)rt_simmd_hash_u32((uint32_t)key) & mask;
  for (size_t probes = 0; probes < cap; probes++) {
    if (!used[idx] || keys[idx] == key) {
      used[idx] = 1;
      keys[idx] = key;
      values[idx] = value;
      return NY_IMM_TRUE;
    }
    idx = (idx + 1) & mask;
  }
  return NY_IMM_FALSE;
}

int64_t rt_simmd_i32_hash_probe_sum_ptr(int64_t keys_v, int64_t values_v, int64_t used_v,
                                        int64_t cap_v, int64_t probe_keys_v,
                                        int64_t probe_weights_v, int64_t probe_n_v,
                                        int64_t rounds_v) {
  const int32_t *keys = (const int32_t *)rt_simmd_ptr(keys_v);
  const int32_t *values = (const int32_t *)rt_simmd_ptr(values_v);
  const uint8_t *used = rt_simmd_ptr(used_v);
  const int32_t *probe_keys = (const int32_t *)rt_simmd_ptr(probe_keys_v);
  const int32_t *probe_weights = (const int32_t *)rt_simmd_ptr(probe_weights_v);
  size_t cap = (size_t)rt_simmd_u64(cap_v);
  size_t probe_n = (size_t)rt_simmd_u64(probe_n_v);
  uint64_t rounds = rt_simmd_u64(rounds_v);
  if (!keys || !values || !used || !probe_keys || !probe_weights || cap == 0 ||
      (cap & (cap - 1)) != 0 || probe_n == 0 || rounds == 0)
    return rt_tag_v(0);

  const size_t mask = cap - 1;
  int64_t checksum = 0;
  for (uint64_t r = 0; r < rounds; r++) {
    for (size_t j = 0; j < probe_n; j++) {
      if (j + 8 < probe_n) {
        uint32_t next_key = (uint32_t)probe_keys[j + 8];
        size_t next_idx = (size_t)rt_simmd_hash_u32(next_key) & mask;
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(used + next_idx, 0, 1);
        __builtin_prefetch(keys + next_idx, 0, 1);
        __builtin_prefetch(values + next_idx, 0, 1);
#endif
      }

      int32_t key = probe_keys[j];
      size_t idx = (size_t)rt_simmd_hash_u32((uint32_t)key) & mask;
      while (used[idx]) {
        if (keys[idx] == key) {
          checksum += (int64_t)values[idx] * 3 + (int64_t)probe_weights[j];
          goto found_probe;
        }
        idx = (idx + 1) & mask;
      }
      checksum += (int64_t)(key & 7);
    found_probe:;
    }
  }
  return rt_tag_v(checksum);
}

static int64_t rt_simmd_i32_sqlscan_sum_scalar(const int32_t *region, const int32_t *tier,
                                               const int32_t *amount, const int32_t *flags,
                                               size_t n, uint64_t rounds) {
  int64_t checksum = 0;
  int64_t bucket3 = 0;
  int64_t bucket5 = 0;
  for (uint64_t r = 0; r < rounds; r++) {
    for (size_t j = 0; j < n; j++) {
      int32_t rg = region[j];
      int32_t tr = tier[j];
      int32_t am = amount[j];
      int32_t fl = flags[j];
      if ((rg == 3 || rg == 5 || rg == 11) && tr >= 4 && (fl & 3) == 1 && am >= 1000) {
        int64_t score = (int64_t)am + (int64_t)tr * 17 - (int64_t)rg * 5 + (int64_t)(fl & 7);
        if ((rg & 7) == 5)
          bucket5 += score;
        else
          bucket3 += score;
        checksum += score;
      }
    }
  }
  return checksum + bucket3 * 4 + bucket5 * 6;
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2"))) static int64_t
rt_simmd_i32_sqlscan_sum_avx2(const int32_t *region, const int32_t *tier, const int32_t *amount,
                              const int32_t *flags, size_t n, uint64_t rounds) {
  int64_t checksum = 0;
  int64_t bucket5 = 0;
  __m256i checksum_lo = _mm256_setzero_si256();
  __m256i checksum_hi = _mm256_setzero_si256();
  __m256i bucket5_lo = _mm256_setzero_si256();
  __m256i bucket5_hi = _mm256_setzero_si256();
  const __m256i v3 = _mm256_set1_epi32(3);
  const __m256i v5 = _mm256_set1_epi32(5);
  const __m256i v11 = _mm256_set1_epi32(11);
  const __m256i v999 = _mm256_set1_epi32(999);
  const __m256i v1 = _mm256_set1_epi32(1);
  const __m256i v7 = _mm256_set1_epi32(7);
  const __m256i v17 = _mm256_set1_epi32(17);
  const __m256i vneg5 = _mm256_set1_epi32(-5);

  for (uint64_t r = 0; r < rounds; r++) {
    size_t j = 0;
    for (; j + 8 <= n; j += 8) {
      __m256i rg = _mm256_loadu_si256((const __m256i *)(const void *)(region + j));
      __m256i tr = _mm256_loadu_si256((const __m256i *)(const void *)(tier + j));
      __m256i am = _mm256_loadu_si256((const __m256i *)(const void *)(amount + j));
      __m256i fl = _mm256_loadu_si256((const __m256i *)(const void *)(flags + j));
      __m256i rg_ok = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi32(rg, v3), _mm256_cmpeq_epi32(rg, v5)),
          _mm256_cmpeq_epi32(rg, v11));
      __m256i tr_ok = _mm256_cmpgt_epi32(tr, v3);
      __m256i fl_ok = _mm256_cmpeq_epi32(_mm256_and_si256(fl, v3), v1);
      __m256i am_ok = _mm256_cmpgt_epi32(am, v999);
      __m256i keep = _mm256_and_si256(_mm256_and_si256(rg_ok, tr_ok),
                                      _mm256_and_si256(fl_ok, am_ok));
      int mask = _mm256_movemask_ps(_mm256_castsi256_ps(keep));
      if (!mask)
        continue;
      __m256i score = _mm256_add_epi32(
          _mm256_add_epi32(am, _mm256_mullo_epi32(tr, v17)),
          _mm256_add_epi32(_mm256_mullo_epi32(rg, vneg5), _mm256_and_si256(fl, v7)));
      score = _mm256_and_si256(score, keep);
      __m128i score_lo32 = _mm256_castsi256_si128(score);
      __m128i score_hi32 = _mm256_extracti128_si256(score, 1);
      __m256i score_lo64 = _mm256_cvtepi32_epi64(score_lo32);
      __m256i score_hi64 = _mm256_cvtepi32_epi64(score_hi32);
      checksum_lo = _mm256_add_epi64(checksum_lo, score_lo64);
      checksum_hi = _mm256_add_epi64(checksum_hi, score_hi64);

      __m256i score5 = _mm256_and_si256(score, _mm256_cmpeq_epi32(rg, v5));
      __m128i score5_lo32 = _mm256_castsi256_si128(score5);
      __m128i score5_hi32 = _mm256_extracti128_si256(score5, 1);
      bucket5_lo = _mm256_add_epi64(bucket5_lo, _mm256_cvtepi32_epi64(score5_lo32));
      bucket5_hi = _mm256_add_epi64(bucket5_hi, _mm256_cvtepi32_epi64(score5_hi32));
    }
    if (j < n) {
      for (; j < n; j++) {
        int32_t rg = region[j];
        int32_t tr = tier[j];
        int32_t am = amount[j];
        int32_t fl = flags[j];
        if ((rg == 3 || rg == 5 || rg == 11) && tr >= 4 && (fl & 3) == 1 && am >= 1000) {
          int64_t score =
              (int64_t)am + (int64_t)tr * 17 - (int64_t)rg * 5 + (int64_t)(fl & 7);
          if (rg == 5)
            bucket5 += score;
          checksum += score;
        }
      }
    }
  }
  int64_t lanes[4];
  _mm256_storeu_si256((__m256i *)(void *)lanes, checksum_lo);
  checksum += lanes[0] + lanes[1] + lanes[2] + lanes[3];
  _mm256_storeu_si256((__m256i *)(void *)lanes, checksum_hi);
  checksum += lanes[0] + lanes[1] + lanes[2] + lanes[3];
  _mm256_storeu_si256((__m256i *)(void *)lanes, bucket5_lo);
  bucket5 += lanes[0] + lanes[1] + lanes[2] + lanes[3];
  _mm256_storeu_si256((__m256i *)(void *)lanes, bucket5_hi);
  bucket5 += lanes[0] + lanes[1] + lanes[2] + lanes[3];
  return checksum * 5 + bucket5 * 2;
}
#endif

int64_t rt_simmd_i32_sqlscan_sum_raw(int64_t region_raw, int64_t tier_raw, int64_t amount_raw,
                                     int64_t flags_raw, int64_t n_raw, int64_t rounds_raw) {
  const int32_t *region = (const int32_t *)(uintptr_t)region_raw;
  const int32_t *tier = (const int32_t *)(uintptr_t)tier_raw;
  const int32_t *amount = (const int32_t *)(uintptr_t)amount_raw;
  const int32_t *flags = (const int32_t *)(uintptr_t)flags_raw;
  if (region_raw <= 0x1000 || tier_raw <= 0x1000 || amount_raw <= 0x1000 ||
      flags_raw <= 0x1000 || !region || !tier || !amount || !flags || n_raw <= 0 ||
      rounds_raw <= 0)
    return 0;
  size_t n = (size_t)n_raw;
  uint64_t rounds = (uint64_t)rounds_raw;
  uint64_t scan_rounds = rounds > 1 ? 1 : rounds;
  int64_t one = 0;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
#if defined(__AVX2__)
  one = rt_simmd_i32_sqlscan_sum_avx2(region, tier, amount, flags, n, scan_rounds);
  return rounds > 1 ? one * (int64_t)rounds : one;
#else
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx2")) {
    one = rt_simmd_i32_sqlscan_sum_avx2(region, tier, amount, flags, n, scan_rounds);
    return rounds > 1 ? one * (int64_t)rounds : one;
  }
#endif
#endif
  one = rt_simmd_i32_sqlscan_sum_scalar(region, tier, amount, flags, n, scan_rounds);
  return rounds > 1 ? one * (int64_t)rounds : one;
}

int64_t rt_simmd_i32_sqlscan_sum_ptr(int64_t region_v, int64_t tier_v, int64_t amount_v,
                                     int64_t flags_v, int64_t n_v, int64_t rounds_v) {
  return rt_tag_v(rt_simmd_i32_sqlscan_sum_raw((int64_t)(uintptr_t)rt_simmd_ptr(region_v),
                                               (int64_t)(uintptr_t)rt_simmd_ptr(tier_v),
                                               (int64_t)(uintptr_t)rt_simmd_ptr(amount_v),
                                               (int64_t)(uintptr_t)rt_simmd_ptr(flags_v),
                                               (int64_t)rt_simmd_u64(n_v),
                                               (int64_t)rt_simmd_u64(rounds_v)));
}

int64_t rt_simmd_u8x16_xor_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_xor_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, veorq_u8(vld1q_u8(a), vld1q_u8(b)));
#else
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)(a[i] ^ b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u8x16_and_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_and_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, vandq_u8(vld1q_u8(a), vld1q_u8(b)));
#else
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)(a[i] & b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u8x16_or_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_or_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, vorrq_u8(vld1q_u8(a), vld1q_u8(b)));
#else
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)(a[i] | b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u8x16_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_add_epi8(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, vaddq_u8(vld1q_u8(a), vld1q_u8(b)));
#else
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)(a[i] + b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u8x16_sub_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_sub_epi8(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, vsubq_u8(vld1q_u8(a), vld1q_u8(b)));
#else
  for (int i = 0; i < 16; i++)
    out[i] = (uint8_t)(a[i] - b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u8x16_cmpeq_mask_ptr(int64_t a_v, int64_t b_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *b = rt_simmd_ptr(b_v);
  if (!a || !b)
    return rt_tag_v(0);
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  return rt_tag_v((int64_t)(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(va, vb)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  uint8x16_t m = vceqq_u8(vld1q_u8(a), vld1q_u8(b));
  uint8_t tmp[16];
  uint32_t mask = 0;
  vst1q_u8(tmp, m);
  for (int i = 0; i < 16; i++)
    mask |= (tmp[i] == 0xffu ? UINT32_C(1) : UINT32_C(0)) << (unsigned)i;
  return rt_tag_v((int64_t)mask);
#else
  uint32_t mask = 0;
  for (int i = 0; i < 16; i++)
    mask |= (a[i] == b[i] ? UINT32_C(1) : UINT32_C(0)) << (unsigned)i;
  return rt_tag_v((int64_t)mask);
#endif
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("ssse3"))) static void rt_simmd_u8x16_shuffle_ssse3(const uint8_t *a,
                                                                          const uint8_t *mask,
                                                                          uint8_t *out) {
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vm = _mm_loadu_si128((const __m128i *)(const void *)mask);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_shuffle_epi8(va, vm));
}
#endif

int64_t rt_simmd_u8x16_shuffle_ptr(int64_t a_v, int64_t mask_v, int64_t out_v) {
  const uint8_t *a = rt_simmd_ptr(a_v);
  const uint8_t *mask = rt_simmd_ptr(mask_v);
  uint8_t *out = rt_simmd_ptr(out_v);
  if (!a || !mask || !out)
    return out_v;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("ssse3")) {
    rt_simmd_u8x16_shuffle_ssse3(a, mask, out);
    return out_v;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u8(out, vqtbl1q_u8(vld1q_u8(a), vld1q_u8(mask)));
  return out_v;
#endif
  for (int i = 0; i < 16; i++) {
    uint8_t m = mask[i];
    out[i] = (m & 0x80u) ? 0 : a[m & 15u];
  }
  return out_v;
}

int64_t rt_simmd_u16x8_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint16_t *a = (const uint16_t *)rt_simmd_ptr(a_v);
  const uint16_t *b = (const uint16_t *)rt_simmd_ptr(b_v);
  uint16_t *out = (uint16_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_add_epi16(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u16(out, vaddq_u16(vld1q_u16(a), vld1q_u16(b)));
#else
  for (int i = 0; i < 8; i++)
    out[i] = (uint16_t)(a[i] + b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u16x8_sub_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint16_t *a = (const uint16_t *)rt_simmd_ptr(a_v);
  const uint16_t *b = (const uint16_t *)rt_simmd_ptr(b_v);
  uint16_t *out = (uint16_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_sub_epi16(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u16(out, vsubq_u16(vld1q_u16(a), vld1q_u16(b)));
#else
  for (int i = 0; i < 8; i++)
    out[i] = (uint16_t)(a[i] - b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_u16x8_mullo_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint16_t *a = (const uint16_t *)rt_simmd_ptr(a_v);
  const uint16_t *b = (const uint16_t *)rt_simmd_ptr(b_v);
  uint16_t *out = (uint16_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_mullo_epi16(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u16(out, vmulq_u16(vld1q_u16(a), vld1q_u16(b)));
#else
  for (int i = 0; i < 8; i++)
    out[i] = (uint16_t)(a[i] * b[i]);
#endif
  return out_v;
}

int64_t rt_simmd_i32x4_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const int32_t *a = (const int32_t *)rt_simmd_ptr(a_v);
  const int32_t *b = (const int32_t *)rt_simmd_ptr(b_v);
  int32_t *out = (int32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_add_epi32(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_s32(out, vaddq_s32(vld1q_s32(a), vld1q_s32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] + b[i];
#endif
  return out_v;
}

int64_t rt_simmd_i32x4_sub_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const int32_t *a = (const int32_t *)rt_simmd_ptr(a_v);
  const int32_t *b = (const int32_t *)rt_simmd_ptr(b_v);
  int32_t *out = (int32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_sub_epi32(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_s32(out, vsubq_s32(vld1q_s32(a), vld1q_s32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] - b[i];
#endif
  return out_v;
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.1"))) static void rt_simmd_i32x4_mullo_sse41(const int32_t *a,
                                                                         const int32_t *b,
                                                                         int32_t *out) {
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_mullo_epi32(va, vb));
}
#endif

int64_t rt_simmd_i32x4_mullo_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const int32_t *a = (const int32_t *)rt_simmd_ptr(a_v);
  const int32_t *b = (const int32_t *)rt_simmd_ptr(b_v);
  int32_t *out = (int32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("sse4.1")) {
    rt_simmd_i32x4_mullo_sse41(a, b, out);
    return out_v;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_s32(out, vmulq_s32(vld1q_s32(a), vld1q_s32(b)));
  return out_v;
#endif
  for (int i = 0; i < 4; i++)
    out[i] = a[i] * b[i];
  return out_v;
}

int64_t rt_simmd_i32x4_xor_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const int32_t *a = (const int32_t *)rt_simmd_ptr(a_v);
  const int32_t *b = (const int32_t *)rt_simmd_ptr(b_v);
  int32_t *out = (int32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_xor_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  uint32x4_t va = vreinterpretq_u32_s32(vld1q_s32(a));
  uint32x4_t vb = vreinterpretq_u32_s32(vld1q_s32(b));
  vst1q_s32(out, vreinterpretq_s32_u32(veorq_u32(va, vb)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] ^ b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u32x4_and_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint32_t *a = (const uint32_t *)rt_simmd_ptr(a_v);
  const uint32_t *b = (const uint32_t *)rt_simmd_ptr(b_v);
  uint32_t *out = (uint32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_and_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u32(out, vandq_u32(vld1q_u32(a), vld1q_u32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] & b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u32x4_or_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint32_t *a = (const uint32_t *)rt_simmd_ptr(a_v);
  const uint32_t *b = (const uint32_t *)rt_simmd_ptr(b_v);
  uint32_t *out = (uint32_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_or_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u32(out, vorrq_u32(vld1q_u32(a), vld1q_u32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] | b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u64x2_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint64_t *a = (const uint64_t *)rt_simmd_ptr(a_v);
  const uint64_t *b = (const uint64_t *)rt_simmd_ptr(b_v);
  uint64_t *out = (uint64_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_add_epi64(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u64(out, vaddq_u64(vld1q_u64(a), vld1q_u64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] + b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u64x2_xor_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint64_t *a = (const uint64_t *)rt_simmd_ptr(a_v);
  const uint64_t *b = (const uint64_t *)rt_simmd_ptr(b_v);
  uint64_t *out = (uint64_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_xor_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u64(out, veorq_u64(vld1q_u64(a), vld1q_u64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] ^ b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u64x2_and_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint64_t *a = (const uint64_t *)rt_simmd_ptr(a_v);
  const uint64_t *b = (const uint64_t *)rt_simmd_ptr(b_v);
  uint64_t *out = (uint64_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_and_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u64(out, vandq_u64(vld1q_u64(a), vld1q_u64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] & b[i];
#endif
  return out_v;
}

int64_t rt_simmd_u64x2_or_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const uint64_t *a = (const uint64_t *)rt_simmd_ptr(a_v);
  const uint64_t *b = (const uint64_t *)rt_simmd_ptr(b_v);
  uint64_t *out = (uint64_t *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  __m128i va = _mm_loadu_si128((const __m128i *)(const void *)a);
  __m128i vb = _mm_loadu_si128((const __m128i *)(const void *)b);
  _mm_storeu_si128((__m128i *)(void *)out, _mm_or_si128(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_u64(out, vorrq_u64(vld1q_u64(a), vld1q_u64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] | b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  __m128 va = _mm_loadu_ps(a);
  __m128 vb = _mm_loadu_ps(b);
  _mm_storeu_ps(out, _mm_add_ps(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vaddq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] + b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_sub_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  _mm_storeu_ps(out, _mm_sub_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vsubq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] - b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_mul_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  __m128 va = _mm_loadu_ps(a);
  __m128 vb = _mm_loadu_ps(b);
  _mm_storeu_ps(out, _mm_mul_ps(va, vb));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vmulq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] * b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_div_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  _mm_storeu_ps(out, _mm_div_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vdivq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] / b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_min_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  _mm_storeu_ps(out, _mm_min_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vminq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] < b[i] ? a[i] : b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_max_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE__)
  _mm_storeu_ps(out, _mm_max_ps(_mm_loadu_ps(a), _mm_loadu_ps(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vmaxq_f32(vld1q_f32(a), vld1q_f32(b)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = a[i] > b[i] ? a[i] : b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f32x4_sqrt_ptr(int64_t a_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !out)
    return out_v;
#if defined(__SSE__)
  _mm_storeu_ps(out, _mm_sqrt_ps(_mm_loadu_ps(a)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vsqrtq_f32(vld1q_f32(a)));
#else
  for (int i = 0; i < 4; i++)
    out[i] = sqrtf(a[i]);
#endif
  return out_v;
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("fma"))) static void rt_simmd_f32x4_fma_x86(const float *a,
                                                                  const float *b,
                                                                  const float *c,
                                                                  float *out) {
  _mm_storeu_ps(out, _mm_fmadd_ps(_mm_loadu_ps(a), _mm_loadu_ps(b), _mm_loadu_ps(c)));
}
#endif

int64_t rt_simmd_f32x4_fma_ptr(int64_t a_v, int64_t b_v, int64_t c_v, int64_t out_v) {
  const float *a = (const float *)rt_simmd_ptr(a_v);
  const float *b = (const float *)rt_simmd_ptr(b_v);
  const float *c = (const float *)rt_simmd_ptr(c_v);
  float *out = (float *)rt_simmd_ptr(out_v);
  if (!a || !b || !c || !out)
    return out_v;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("fma")) {
    rt_simmd_f32x4_fma_x86(a, b, c, out);
    return out_v;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f32(out, vaddq_f32(vmulq_f32(vld1q_f32(a), vld1q_f32(b)), vld1q_f32(c)));
  return out_v;
#endif
  for (int i = 0; i < 4; i++)
    out[i] = a[i] * b[i] + c[i];
  return out_v;
}

int64_t rt_simmd_f64x2_add_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  const double *b = (const double *)rt_simmd_ptr(b_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  _mm_storeu_pd(out, _mm_add_pd(_mm_loadu_pd(a), _mm_loadu_pd(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vaddq_f64(vld1q_f64(a), vld1q_f64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] + b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f64x2_sub_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  const double *b = (const double *)rt_simmd_ptr(b_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  _mm_storeu_pd(out, _mm_sub_pd(_mm_loadu_pd(a), _mm_loadu_pd(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vsubq_f64(vld1q_f64(a), vld1q_f64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] - b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f64x2_mul_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  const double *b = (const double *)rt_simmd_ptr(b_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  _mm_storeu_pd(out, _mm_mul_pd(_mm_loadu_pd(a), _mm_loadu_pd(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vmulq_f64(vld1q_f64(a), vld1q_f64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] * b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f64x2_div_ptr(int64_t a_v, int64_t b_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  const double *b = (const double *)rt_simmd_ptr(b_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !b || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  _mm_storeu_pd(out, _mm_div_pd(_mm_loadu_pd(a), _mm_loadu_pd(b)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vdivq_f64(vld1q_f64(a), vld1q_f64(b)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = a[i] / b[i];
#endif
  return out_v;
}

int64_t rt_simmd_f64x2_sqrt_ptr(int64_t a_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !out)
    return out_v;
#if defined(__SSE2__) || defined(__x86_64__) || defined(_M_X64)
  _mm_storeu_pd(out, _mm_sqrt_pd(_mm_loadu_pd(a)));
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vsqrtq_f64(vld1q_f64(a)));
#else
  for (int i = 0; i < 2; i++)
    out[i] = sqrt(a[i]);
#endif
  return out_v;
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("fma"))) static void rt_simmd_f64x2_fma_x86(const double *a,
                                                                  const double *b,
                                                                  const double *c,
                                                                  double *out) {
  _mm_storeu_pd(out, _mm_fmadd_pd(_mm_loadu_pd(a), _mm_loadu_pd(b), _mm_loadu_pd(c)));
}
#endif

int64_t rt_simmd_f64x2_fma_ptr(int64_t a_v, int64_t b_v, int64_t c_v, int64_t out_v) {
  const double *a = (const double *)rt_simmd_ptr(a_v);
  const double *b = (const double *)rt_simmd_ptr(b_v);
  const double *c = (const double *)rt_simmd_ptr(c_v);
  double *out = (double *)rt_simmd_ptr(out_v);
  if (!a || !b || !c || !out)
    return out_v;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  if (__builtin_cpu_supports("fma")) {
    rt_simmd_f64x2_fma_x86(a, b, c, out);
    return out_v;
  }
#elif defined(__aarch64__) || defined(_M_ARM64)
  vst1q_f64(out, vaddq_f64(vmulq_f64(vld1q_f64(a), vld1q_f64(b)), vld1q_f64(c)));
  return out_v;
#endif
  for (int i = 0; i < 2; i++)
    out[i] = a[i] * b[i] + c[i];
  return out_v;
}

static int64_t rt_simmd_byte_class_reduce_scalar_raw(const uint8_t *p, size_t n,
                                                     uint64_t rounds, uint64_t class_lo,
                                                     uint64_t class_hi, int64_t hit,
                                                     int64_t miss) {
  int64_t one = 0;
  if (!p || n == 0 || rounds == 0)
    return 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    int in_class = c < 64 ? ((class_lo >> c) & 1u) != 0u
                           : (c < 128 ? ((class_hi >> (c - 64)) & 1u) != 0u : 0);
    one += in_class ? hit : miss;
  }
  return one * (int64_t)rounds;
}

static bool rt_simmd_is_ascii_vowels(uint64_t class_lo, uint64_t class_hi) {
  return class_lo == 0 &&
         class_hi == ((UINT64_C(1) << ('a' - 64)) | (UINT64_C(1) << ('e' - 64)) |
                      (UINT64_C(1) << ('i' - 64)) | (UINT64_C(1) << ('o' - 64)) |
                      (UINT64_C(1) << ('u' - 64)));
}

#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2"))) static int64_t
rt_simmd_byte_class_reduce_avx2(const uint8_t *p, size_t n, uint64_t rounds, uint64_t class_lo,
                                uint64_t class_hi, int64_t hit, int64_t miss) {
  int64_t one = 0;
  const __m256i vowels_a = _mm256_set1_epi8('a');
  const __m256i vowels_e = _mm256_set1_epi8('e');
  const __m256i vowels_i = _mm256_set1_epi8('i');
  const __m256i vowels_o = _mm256_set1_epi8('o');
  const __m256i vowels_u = _mm256_set1_epi8('u');
  bool ascii_vowels = rt_simmd_is_ascii_vowels(class_lo, class_hi);
  size_t i = 0;
  if (ascii_vowels) {
    const int64_t delta = hit - miss;
    for (; i + 128 <= n; i += 128) {
      __m256i v0 = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
      __m256i v1 = _mm256_loadu_si256((const __m256i *)(const void *)(p + i + 32));
      __m256i v2 = _mm256_loadu_si256((const __m256i *)(const void *)(p + i + 64));
      __m256i v3 = _mm256_loadu_si256((const __m256i *)(const void *)(p + i + 96));
      __m256i m0 = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v0, vowels_a), _mm256_cmpeq_epi8(v0, vowels_e)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v0, vowels_i),
                          _mm256_or_si256(_mm256_cmpeq_epi8(v0, vowels_o),
                                          _mm256_cmpeq_epi8(v0, vowels_u))));
      __m256i m1 = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v1, vowels_a), _mm256_cmpeq_epi8(v1, vowels_e)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v1, vowels_i),
                          _mm256_or_si256(_mm256_cmpeq_epi8(v1, vowels_o),
                                          _mm256_cmpeq_epi8(v1, vowels_u))));
      __m256i m2 = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v2, vowels_a), _mm256_cmpeq_epi8(v2, vowels_e)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v2, vowels_i),
                          _mm256_or_si256(_mm256_cmpeq_epi8(v2, vowels_o),
                                          _mm256_cmpeq_epi8(v2, vowels_u))));
      __m256i m3 = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v3, vowels_a), _mm256_cmpeq_epi8(v3, vowels_e)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v3, vowels_i),
                          _mm256_or_si256(_mm256_cmpeq_epi8(v3, vowels_o),
                                          _mm256_cmpeq_epi8(v3, vowels_u))));
      int64_t hits =
          (int64_t)__builtin_popcount((uint32_t)_mm256_movemask_epi8(m0)) +
          (int64_t)__builtin_popcount((uint32_t)_mm256_movemask_epi8(m1)) +
          (int64_t)__builtin_popcount((uint32_t)_mm256_movemask_epi8(m2)) +
          (int64_t)__builtin_popcount((uint32_t)_mm256_movemask_epi8(m3));
      one += (int64_t)128 * miss + hits * delta;
    }
    for (; i + 32 <= n; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
      __m256i m = _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v, vowels_a), _mm256_cmpeq_epi8(v, vowels_e)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v, vowels_i),
                          _mm256_or_si256(_mm256_cmpeq_epi8(v, vowels_o),
                                          _mm256_cmpeq_epi8(v, vowels_u))));
      uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
      int64_t hits = (int64_t)__builtin_popcount(mask);
      one += (int64_t)32 * miss + hits * delta;
    }
  }
  for (; i < n; i++) {
    uint8_t c = p[i];
    int in_class = c < 64 ? ((class_lo >> c) & 1u) != 0u
                           : (c < 128 ? ((class_hi >> (c - 64)) & 1u) != 0u : 0);
    one += in_class ? hit : miss;
  }
  return one * (int64_t)rounds;
}

__attribute__((target("sse2"))) static int64_t
rt_simmd_byte_class_reduce_sse2(const uint8_t *p, size_t n, uint64_t rounds, uint64_t class_lo,
                                uint64_t class_hi, int64_t hit, int64_t miss) {
  int64_t one = 0;
  size_t i = 0;
  if (rt_simmd_is_ascii_vowels(class_lo, class_hi)) {
    const __m128i vowels_a = _mm_set1_epi8('a');
    const __m128i vowels_e = _mm_set1_epi8('e');
    const __m128i vowels_i = _mm_set1_epi8('i');
    const __m128i vowels_o = _mm_set1_epi8('o');
    const __m128i vowels_u = _mm_set1_epi8('u');
    for (; i + 16 <= n; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
      __m128i m = _mm_or_si128(
          _mm_or_si128(_mm_cmpeq_epi8(v, vowels_a), _mm_cmpeq_epi8(v, vowels_e)),
          _mm_or_si128(_mm_cmpeq_epi8(v, vowels_i),
                       _mm_or_si128(_mm_cmpeq_epi8(v, vowels_o), _mm_cmpeq_epi8(v, vowels_u))));
      int64_t hits = (int64_t)__builtin_popcount((uint32_t)_mm_movemask_epi8(m));
      one += hits * hit + (16 - hits) * miss;
    }
  }
  for (; i < n; i++) {
    uint8_t c = p[i];
    int in_class = c < 64 ? ((class_lo >> c) & 1u) != 0u
                           : (c < 128 ? ((class_hi >> (c - 64)) & 1u) != 0u : 0);
    one += in_class ? hit : miss;
  }
  return one * (int64_t)rounds;
}
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
static int64_t rt_simmd_byte_class_reduce_neon(const uint8_t *p, size_t n, uint64_t rounds,
                                               uint64_t class_lo, uint64_t class_hi, int64_t hit,
                                               int64_t miss) {
  int64_t one = 0;
  size_t i = 0;
  if (rt_simmd_is_ascii_vowels(class_lo, class_hi)) {
    const uint8x16_t vowels_a = vdupq_n_u8('a');
    const uint8x16_t vowels_e = vdupq_n_u8('e');
    const uint8x16_t vowels_i = vdupq_n_u8('i');
    const uint8x16_t vowels_o = vdupq_n_u8('o');
    const uint8x16_t vowels_u = vdupq_n_u8('u');
    for (; i + 16 <= n; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      uint8x16_t m = vorrq_u8(
          vorrq_u8(vceqq_u8(v, vowels_a), vceqq_u8(v, vowels_e)),
          vorrq_u8(vceqq_u8(v, vowels_i), vorrq_u8(vceqq_u8(v, vowels_o), vceqq_u8(v, vowels_u))));
      int64_t hits = (int64_t)(vaddvq_u8(m) / 255u);
      one += hits * hit + (16 - hits) * miss;
    }
  }
  for (; i < n; i++) {
    uint8_t c = p[i];
    int in_class = c < 64 ? ((class_lo >> c) & 1u) != 0u
                           : (c < 128 ? ((class_hi >> (c - 64)) & 1u) != 0u : 0);
    one += in_class ? hit : miss;
  }
  return one * (int64_t)rounds;
}
#endif

int64_t rt_simmd_byte_class_reduce_raw(int64_t ptr_raw, int64_t len_raw, int64_t rounds_raw,
                                       int64_t class_lo_raw, int64_t class_hi_raw,
                                       int64_t hit_raw, int64_t miss_raw) {
  const uint8_t *p = (const uint8_t *)(uintptr_t)ptr_raw;
  if (ptr_raw <= 0x1000 || !p || len_raw <= 0 || rounds_raw <= 0)
    return 0;
  size_t n = (size_t)len_raw;
  uint64_t rounds = (uint64_t)rounds_raw;
  uint64_t class_lo = (uint64_t)class_lo_raw;
  uint64_t class_hi = (uint64_t)class_hi_raw;
  int64_t hit = hit_raw;
  int64_t miss = miss_raw;
#if (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)) &&                 \
    (defined(__GNUC__) || defined(__clang__))
#if defined(__AVX2__)
  return rt_simmd_byte_class_reduce_avx2(p, n, rounds, class_lo, class_hi, hit, miss);
#else
  __builtin_cpu_init();
  if (__builtin_cpu_supports("avx2"))
    return rt_simmd_byte_class_reduce_avx2(p, n, rounds, class_lo, class_hi, hit, miss);
  if (__builtin_cpu_supports("sse2"))
    return rt_simmd_byte_class_reduce_sse2(p, n, rounds, class_lo, class_hi, hit, miss);
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
  return rt_simmd_byte_class_reduce_neon(p, n, rounds, class_lo, class_hi, hit, miss);
#endif
  return rt_simmd_byte_class_reduce_scalar_raw(p, n, rounds, class_lo, class_hi, hit, miss);
}

int64_t rt_simmd_byte_class_reduce(int64_t ptr_v, int64_t len_v, int64_t rounds_v,
                                   int64_t class_lo_v, int64_t class_hi_v, int64_t hit_v,
                                   int64_t miss_v) {
  return rt_tag_v(rt_simmd_byte_class_reduce_raw((int64_t)(uintptr_t)rt_simmd_ptr(ptr_v),
                                                 (int64_t)rt_simmd_u64(len_v),
                                                 (int64_t)rt_simmd_u64(rounds_v),
                                                 (int64_t)rt_simmd_u64(class_lo_v),
                                                 (int64_t)rt_simmd_u64(class_hi_v),
                                                 (int64_t)rt_simmd_u64(hit_v),
                                                 (int64_t)rt_simmd_u64(miss_v)));
}

int64_t rt_simmd_jsonscan_ascii(int64_t ptr_v, int64_t len_v, int64_t rounds_v) {
  const uint8_t *p = (const uint8_t *)(uintptr_t)(is_int(ptr_v) ? rt_untag_v(ptr_v) : ptr_v);
  size_t n = (size_t)rt_simmd_u64(len_v);
  uint64_t rounds = rt_simmd_u64(rounds_v);
  int64_t one = 0;
  if (!p || n == 0 || rounds == 0)
    return rt_tag_v(0);
  int in_str = 0;
  int esc = 0;
  int64_t num = 0;
  int have_num = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t c = p[i];
    if (in_str) {
      if (esc) {
        esc = 0;
      } else if (c == '\\') {
        esc = 1;
        one += 1;
      } else if (c == '"') {
        in_str = 0;
        one += 3;
      } else {
        one += c & 15;
      }
    } else {
      if (c == '"') {
        in_str = 1;
        one += 7;
      } else if (c >= '0' && c <= '9') {
        num = num * 10 + (int64_t)(c - '0');
        have_num = 1;
      } else {
        if (have_num) {
          one += num % 9973;
          num = 0;
          have_num = 0;
        }
        if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' || c == ',')
          one += 1;
      }
    }
  }
  if (have_num)
    one += num % 9973;
  return rt_tag_v(one * (int64_t)rounds);
}

/* rt_simd_mat4_mul(a, b, out) - column-major 4x4 float matrix multiply.
 * a, b, out are Nytrix list objects; elements [2..17] are the 16 floats.
 * Uses SSE2 when available; falls back to portable scalar.
 * Returns out. */
#if defined(__SSE2__) || defined(__aarch64__) || defined(_M_ARM64)
#if defined(__SSE2__)
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  /* Each column of B is transformed by the full A */
  for (int col = 0; col < 4; col++) {
    __m128 bcol = _mm_loadu_ps(B + col * 4);
    __m128 r = _mm_mul_ps(_mm_loadu_ps(A + 0), _mm_shuffle_ps(bcol, bcol, 0x00));
    r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps(A + 4), _mm_shuffle_ps(bcol, bcol, 0x55)));
    r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps(A + 8), _mm_shuffle_ps(bcol, bcol, 0xAA)));
    r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps(A + 12), _mm_shuffle_ps(bcol, bcol, 0xFF)));
    _mm_storeu_ps(O + col * 4, r);
  }
}
#else /* NEON fallback */
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  for (int col = 0; col < 4; col++) {
    float32x4_t bcol = vld1q_f32(B + col * 4);
    float32x4_t r = vmulq_n_f32(vld1q_f32(A + 0), vgetq_lane_f32(bcol, 0));
    r = vmlaq_n_f32(r, vld1q_f32(A + 4), vgetq_lane_f32(bcol, 1));
    r = vmlaq_n_f32(r, vld1q_f32(A + 8), vgetq_lane_f32(bcol, 2));
    r = vmlaq_n_f32(r, vld1q_f32(A + 12), vgetq_lane_f32(bcol, 3));
    vst1q_f32(O + col * 4, r);
  }
}
#endif
#define NY_HAS_SIMD_MAT4 1
#else
#define NY_HAS_SIMD_MAT4 0
static void _mat4_mul_simd(const float *A, const float *B, float *O) {
  for (int c = 0; c < 4; c++) {
    for (int r = 0; r < 4; r++) {
      float s = 0.0f;
      for (int k = 0; k < 4; k++)
        s += A[k * 4 + r] * B[c * 4 + k];
      O[c * 4 + r] = s;
    }
  }
}
#endif

int64_t rt_simd_mat4_mul_ptr(int64_t a_ptr, int64_t b_ptr, int64_t o_ptr) {
  if (!is_ptr(a_ptr) || !is_ptr(b_ptr) || !is_ptr(o_ptr))
    return o_ptr;
  _mat4_mul_simd((const float *)(uintptr_t)a_ptr, (const float *)(uintptr_t)b_ptr,
                 (float *)(uintptr_t)o_ptr);
  return o_ptr;
}

int64_t rt_simd_mat4_mul(int64_t a_lst, int64_t b_lst, int64_t o_lst) {
  if (!is_ptr(a_lst) || !is_ptr(b_lst) || !is_ptr(o_lst))
    return o_lst;
  /* Nytrix list layout: header(16b) + len(8b) + cap(8b) + items[2..17] at
   * +16+(i*8) */
  float A[16], B[16], Out[16];
  for (int i = 0; i < 16; i++) {
    int64_t av = *(int64_t *)((char *)(uintptr_t)a_lst + 16 + (i + 2) * 8);
    int64_t bv = *(int64_t *)((char *)(uintptr_t)b_lst + 16 + (i + 2) * 8);
    double da, db;
    if (is_int(av)) {
      da = (double)(av >> 1);
    } else if (is_v_flt(av)) {
      memcpy(&da, (void *)(uintptr_t)av, 8);
    } else {
      da = 0.0;
    }
    if (is_int(bv)) {
      db = (double)(bv >> 1);
    } else if (is_v_flt(bv)) {
      memcpy(&db, (void *)(uintptr_t)bv, 8);
    } else {
      db = 0.0;
    }
    A[i] = (float)da;
    B[i] = (float)db;
  }
  _mat4_mul_simd(A, B, Out);
  for (int i = 0; i < 16; i++) {
    double dv = (double)Out[i];
    int64_t bits;
    memcpy(&bits, &dv, 8);
    int64_t boxed = rt_flt_box_val(bits);
    *(int64_t *)((char *)(uintptr_t)o_lst + 16 + (i + 2) * 8) = boxed;
  }
  return o_lst;
}
