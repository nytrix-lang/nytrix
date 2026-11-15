/* Fixed-width Bloom-filter helpers for compiler symbol/name tables. */
#ifndef NY_BASE_BLOOM_H
#define NY_BASE_BLOOM_H

#include <stdbool.h>
#include <stdint.h>

static inline void ny_name_bloom_add(uint64_t bloom[4], uint64_t hash) {
  if (!bloom)
    return;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((b0 >> 6) < 4)
    bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  if ((b1 >> 6) < 4)
    bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

static inline bool ny_name_bloom_maybe_has(const uint64_t bloom[4],
                                           uint64_t hash) {
  if (!bloom)
    return false;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((b0 >> 6) < 4) {
    if ((bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
      return false;
  }
  if ((b1 >> 6) < 4) {
    if ((bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
      return false;
  }
  return true;
}

#define NY_ALIAS_BLOOM_WORDS 256u

static inline void ny_alias_bloom_add(uint64_t bloom[NY_ALIAS_BLOOM_WORDS],
                                      uint64_t hash) {
  if (!bloom)
    return;
  unsigned b0 = (unsigned)(hash & 16383u);
  unsigned b1 = (unsigned)((hash >> 14) & 16383u);
  bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

static inline bool
ny_alias_bloom_maybe_has(const uint64_t bloom[NY_ALIAS_BLOOM_WORDS],
                         uint64_t hash) {
  if (!bloom)
    return false;
  unsigned b0 = (unsigned)(hash & 16383u);
  unsigned b1 = (unsigned)((hash >> 14) & 16383u);
  if ((bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
    return false;
  if ((bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
    return false;
  return true;
}

#endif
