#ifndef NY_COMPILER_UTIL_H
#define NY_COMPILER_UTIL_H

#include "base/compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// File I/O
char *ny_read_file(const char *path);
int ny_write_file(const char *path, const char *content, size_t len);
void ny_write_text_file(const char *path, const char *contents);
int ny_copy_file(const char *src, const char *dst);
int ny_ensure_dir(const char *path);
const char *ny_get_temp_dir(void);
void ny_join_path(char *out, size_t out_len, const char *dir, const char *name);

// String utilities
char *ny_strdup(const char *s);
bool ny_env_is_truthy(const char *value);
bool ny_env_enabled(const char *name);
void ny_str_list_append(char ***list, size_t *len, size_t *cap,
                        const char *str);
void ny_str_list_free(char **list, size_t count);

// Hash
#define NY_FNV1A64_OFFSET_BASIS UINT64_C(14695981039346656037)
#define NY_FNV1A64_PRIME UINT64_C(1099511628211)

uint64_t ny_fnv1a64(const void *data, size_t len, uint64_t seed);
uint64_t ny_fnv1a64_cstr(const char *s, uint64_t seed);
static inline uint64_t ny_hash64(const void *data, size_t len) {
  return ny_fnv1a64(data, len, NY_FNV1A64_OFFSET_BASIS);
}
static inline uint64_t ny_hash64_cstr(const char *s) {
  return ny_fnv1a64_cstr(s, NY_FNV1A64_OFFSET_BASIS);
}
static inline uint64_t ny_hash64_u64(uint64_t seed, uint64_t v) {
  return ny_fnv1a64(&v, sizeof(v), seed ? seed : NY_FNV1A64_OFFSET_BASIS);
}
int ny_levenshtein(const char *s1, const char *s2);

// Paths
const char *ny_src_root(void);
char *ny_get_executable_dir(void);

#endif
