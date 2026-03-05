#ifndef NY_HASH_H
#define NY_HASH_H

#include <stddef.h>
#include <stdint.h>

uint64_t ny_hash_u32v(uint64_t h, const unsigned *vals, size_t len);
uint64_t ny_hash_u64v(uint64_t h, const uint64_t *vals, size_t len);
uint64_t ny_hash_cstrv(uint64_t h, const char *const *vals, size_t len);
uint64_t ny_hash_envv(uint64_t h, const char *const *env_names, size_t len);

#endif