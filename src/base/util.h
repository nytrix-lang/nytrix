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
#include <sys/stat.h>

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
char *ny_read_url(const char *url);
int ny_write_file(const char *path, const char *content, size_t len);
bool ny_write_if_changed(const char *path, const char *content, size_t len);
void ny_write_text_file(const char *path, const char *contents);
int ny_copy_file(const char *src, const char *dst);
static inline int ny_ensure_dir(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) == -1) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
  }
  return 0;
}

static inline void ny_ensure_dir_recursive(const char *path) {
  if (!path || !*path)
    return;
  char tmp[1024];
  snprintf(tmp, sizeof(tmp), "%s", path);
  size_t len = strlen(tmp);
  if (len == 0)
    return;
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    ny_ensure_dir(tmp);
    *p = '/';
  }
  ny_ensure_dir(tmp);
}

const char *ny_get_temp_dir(void);
static inline void ny_join_path(char *out, size_t out_len, const char *dir, const char *name) {
  if (!out || out_len == 0)
    return;
  if (!dir || !*dir) {
    snprintf(out, out_len, "%s", name ? name : "");
    return;
  }
  size_t dlen = strlen(dir);
  int needs_sep = (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\') ? 1 : 0;
  if (needs_sep)
    snprintf(out, out_len, "%s/%s", dir, name ? name : "");
  else
    snprintf(out, out_len, "%s%s", dir, name ? name : "");
}
bool ny_extract_line(const char *src, int line, const char **out_start, size_t *out_len);

char *ny_strdup(const char *s);
static inline bool ny_env_is_truthy(const char *value) {
  if (!value || !*value)
    return false;
  if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "False") == 0 ||
      strcmp(value, "FALSE") == 0 || strcmp(value, "off") == 0 || strcmp(value, "OFF") == 0 ||
      strcmp(value, "no") == 0 || strcmp(value, "NO") == 0) {
    return false;
  }
  return true;
}
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
#define NY_FNV1A32_OFFSET_BASIS UINT32_C(2166136261)
#define NY_FNV1A32_PRIME UINT32_C(16777619)

uint64_t ny_fnv1a64(const void *data, size_t len, uint64_t seed);
uint64_t ny_fnv1a64_cstr(const char *s, uint64_t seed);
uint64_t ny_hash64_fast(const void *data, size_t len);
uint64_t ny_hash64_fast_cstr(const char *s);
static inline uint64_t ny_hash64(const void *data, size_t len) { return ny_hash64_fast(data, len); }
static inline uint64_t ny_hash64_cstr(const char *s) { return ny_hash64_fast_cstr(s); }
static inline uint32_t ny_hash32_cstr(const char *s) {
  uint32_t h = NY_FNV1A32_OFFSET_BASIS;
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; ++p) {
    h ^= *p;
    h *= NY_FNV1A32_PRIME;
  }
  return h;
}
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

static inline bool ny_has_suffix(const char *str, const char *suffix) {
  size_t a = strlen(str), b = strlen(suffix);
  return a >= b && memcmp(str + a - b, suffix, b) == 0;
}

static inline const char *ny_base_name(const char *path) {
  if (!path || !*path)
    return ".";
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

/* Returns heap-allocated joined path; caller must free. */
static inline char *ny_path_join_alloc(const char *dir, const char *name) {
  if (!dir || !*dir) return name && *name ? ny_strdup(name) : ny_strdup(".");
  size_t dlen = strlen(dir);
  int needs_sep = (dir[dlen - 1] != '/' && dir[dlen - 1] != '\\') ? 1 : 0;
  size_t nlen = name ? strlen(name) : 0;
  char *out = (char *)malloc(dlen + (size_t)needs_sep + nlen + 1);
  if (!out) return NULL;
  memcpy(out, dir, dlen);
  if (needs_sep) out[dlen] = '/';
  memcpy(out + dlen + (size_t)needs_sep, name ? name : "", nlen + 1);
  return out;
}

/* Writes directory portion of path into out (modifies in-place if out==path). */
static inline void ny_dir_name(char *out, size_t out_len, const char *path) {
  if (!out || out_len == 0) return;
  if (!path || !*path) { snprintf(out, out_len, "."); return; }
  snprintf(out, out_len, "%s", path);
  char *slash = strrchr(out, '/');
#ifdef _WIN32
  char *bslash = strrchr(out, '\\');
  if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
  if (slash) {
    /* Preserve trailing slash for root paths */
    if (slash == out) {
      out[1] = '\0';
    } else {
      *slash = '\0';
    }
  } else {
    snprintf(out, out_len, ".");
  }
}

static inline bool ny_is_ident_char(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
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

void ny_complexity_note(const char *file, int line, const char *func,
                        const char *category, const char *detail);

#define NY_COMPLEXITY_NOTE(cat, detail) \
  ny_complexity_note(__FILE__, __LINE__, __func__, (cat), (detail))

const char *ny_src_root(void);
const char *ny_default_cache_root_dir(void);
char *ny_get_executable_path(void);
char *ny_get_executable_dir(void);

#endif

void ny_print_snippet(const char *src, int line, int col, int len, const char *color);
