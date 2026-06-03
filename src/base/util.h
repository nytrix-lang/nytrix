#ifndef NY_COMPILER_UTIL_H
#define NY_COMPILER_UTIL_H

#include "base/compat.h"
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline char *ny_read_file_raw(const char *path, size_t *out_len) {
  if (!path)
    return NULL;
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *content = malloc((size_t)size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }
  size_t read = fread(content, 1, (size_t)size, f);
  content[read] = '\0';
  fclose(f);
  if (out_len)
    *out_len = read;
  return content;
}
char *ny_read_file(const char *path);
int ny_write_file(const char *path, const char *content, size_t len);
bool ny_write_if_changed(const char *path, const char *content, size_t len);
void ny_write_text_file(const char *path, const char *contents);
int ny_copy_file(const char *src, const char *dst);
int ny_ensure_dir(const char *path);
void ny_ensure_dir_recursive(const char *path);
const char *ny_get_temp_dir(void);
void ny_join_path(char *out, size_t out_len, const char *dir, const char *name);
bool ny_extract_line(const char *src, int line, const char **out_start, size_t *out_len);

char *ny_strdup(const char *s);
bool ny_env_is_truthy(const char *value);
bool ny_env_enabled(const char *name);
bool ny_env_enabled_default_on(const char *name);
const char *ny_env_str(const char *name);
const char *ny_env_str_nonempty(const char *name);
int ny_env_int(const char *name, int fallback);
int ny_env_int_range(const char *name, int fallback, int minv, int maxv);
static inline bool ny_path_readable(const char *path) {
  return path && *path && ny_access(path, R_OK) == 0;
}
static inline bool ny_symbol_path_char(int c) {
  return isalnum((unsigned char)c) || c == '_' || c == '.';
}
bool ny_compiler_asserts_enabled(void);
void ny_compiler_assert_fail(const char *file, int line, const char *func, const char *cond,
                             const char *fmt, ...);
void ny_str_list_append(char ***list, size_t *len, size_t *cap, const char *str);
void ny_str_list_free(char **list, size_t count);

#define NY_FNV1A64_OFFSET_BASIS UINT64_C(14695981039346656037)
#define NY_FNV1A64_PRIME UINT64_C(1099511628211)

uint64_t ny_fnv1a64(const void *data, size_t len, uint64_t seed);
uint64_t ny_fnv1a64_cstr(const char *s, uint64_t seed);
uint64_t ny_hash64_fast(const void *data, size_t len);
uint64_t ny_hash64_fast_cstr(const char *s);
static inline uint64_t ny_hash64(const void *data, size_t len) { return ny_hash64_fast(data, len); }
static inline uint64_t ny_hash64_cstr(const char *s) { return ny_hash64_fast_cstr(s); }
static inline uint64_t ny_hash64_u64(uint64_t seed, uint64_t v) {
  return ny_fnv1a64(&v, sizeof(v), seed ? seed : NY_FNV1A64_OFFSET_BASIS);
}

#define NY_COMPILER_ASSERT(cond, msg)                                                             \
  do {                                                                                            \
    if (ny_compiler_asserts_enabled() && !(cond))                                                \
      ny_compiler_assert_fail(__FILE__, __LINE__, __func__, #cond, "%s", (msg));                 \
  } while (0)

#define NY_COMPILER_ASSERTF(cond, fmt, ...)                                                       \
  do {                                                                                            \
    if (ny_compiler_asserts_enabled() && !(cond))                                                \
      ny_compiler_assert_fail(__FILE__, __LINE__, __func__, #cond, (fmt), ##__VA_ARGS__);       \
  } while (0)

static inline bool ny_add_range_ok(int64_t a, int64_t b, int64_t *out) {
  return !__builtin_add_overflow(a, b, out);
}
static inline bool ny_sub_range_ok(int64_t a, int64_t b, int64_t *out) {
  return !__builtin_sub_overflow(a, b, out);
}
static inline bool ny_mul_range_ok(int64_t a, int64_t b, int64_t *out) {
  return !__builtin_mul_overflow(a, b, out);
}

static inline bool ny_name_tail_is(const char *name, const char *tail) {
  if (!name || !tail) return false;
  size_t nl = strlen(name);
  size_t tl = strlen(tail);
  if (tl > nl) return false;
  if (strcmp(name + nl - tl, tail) != 0) return false;
  return tl == nl || name[nl - tl - 1] == '.';
}
static inline const char *ny_name_leaf(const char *name) {
  if (!name)
    return NULL;
  const char *dot = strrchr(name, '.');
  return dot ? dot + 1 : name;
}
static inline const char *ny_generic_type_leaf(const char *name) {
  if (!name)
    return NULL;
  while (*name == '?' || *name == '*')
    name++;
  const char *leaf = ny_name_leaf(name);
  return leaf ? leaf : name;
}
static inline bool ny_generic_type_base_is(const char *name, const char *base) {
  const char *leaf = ny_generic_type_leaf(name);
  const char *base_leaf = ny_generic_type_leaf(base);
  if (!leaf || !base_leaf)
    return false;
  const char *lt = strchr(leaf, '<');
  size_t leaf_len = lt ? (size_t)(lt - leaf) : strlen(leaf);
  const char *base_lt = strchr(base_leaf, '<');
  size_t base_len = base_lt ? (size_t)(base_lt - base_leaf) : strlen(base_leaf);
  return leaf_len == base_len && strncmp(leaf, base_leaf, leaf_len) == 0;
}
static inline char *ny_generic_type_arg_owned(const char *name, size_t index) {
  const char *leaf = ny_generic_type_leaf(name);
  if (!leaf)
    return NULL;
  const char *lt = strchr(leaf, '<');
  const char *gt = strrchr(leaf, '>');
  if (!lt || !gt || gt <= lt)
    return NULL;
  const char *start = lt + 1;
  size_t current = 0;
  int depth = 0;
  for (const char *p = start; p <= gt; ++p) {
    bool at_end = p == gt;
    if (!at_end) {
      if (*p == '<')
        depth++;
      else if (*p == '>')
        depth--;
      else if (*p != ',' || depth != 0)
        continue;
    }
    if (current == index) {
      while (start < p && (*start == ' ' || *start == '\t'))
        start++;
      while (p > start && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
      size_t n = (size_t)(p - start);
      char *out = malloc(n + 1);
      if (!out)
        return NULL;
      memcpy(out, start, n);
      out[n] = '\0';
      return out;
    }
    current++;
    start = p + 1;
  }
  return NULL;
}

int ny_levenshtein(const char *s1, const char *s2);
bool ny_log_should_emit(const char *fmt);

const char *ny_src_root(void);
const char *ny_default_cache_root_dir(void);
char *ny_get_executable_path(void);
char *ny_get_executable_dir(void);

#endif

void ny_print_snippet(const char *src, int line, int col, int len, const char *color);
