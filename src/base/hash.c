#include "base/hash.h"
#include "base/util.h"
#include <stdlib.h>

uint64_t ny_hash_u32v(uint64_t h, const unsigned *vals, size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_hash64_u64(h, (uint64_t)vals[i]);
  return h;
}

uint64_t ny_hash_u64v(uint64_t h, const uint64_t *vals, size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_hash64_u64(h, vals[i]);
  return h;
}

uint64_t ny_hash_cstrv(uint64_t h, const char *const *vals, size_t len) {
  if (!vals)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_fnv1a64_cstr(vals[i], h);
  return h;
}

uint64_t ny_hash_envv(uint64_t h, const char *const *env_names, size_t len) {
  if (!env_names)
    return h;
  for (size_t i = 0; i < len; ++i)
    h = ny_fnv1a64_cstr(getenv(env_names[i]), h);
  return h;
}
