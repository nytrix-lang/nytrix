/*
 * Non-cryptographic hash primitives: FNV-1a, xxHash-style helpers,
 * and incremental hashing for symbol tables, cache keys, and AST nodes.
 */
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
  for (size_t i = 0; i < len; ++i) {
    const char *name = env_names[i];
    const char *value = name ? getenv(name) : NULL;
    h = ny_fnv1a64_cstr(name, h);
    h = ny_hash64_u64(h, value ? 1 : 0);
    if (value) {
      h = ny_hash64_u64(h, (uint64_t)strlen(value));
      h = ny_fnv1a64_cstr(value, h);
    }
  }
  return h;
}
