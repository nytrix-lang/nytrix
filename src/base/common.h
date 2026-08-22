#pragma once

#include "base/compat.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#endif

static inline const char *ny_tail_name(const char *name) {
  const char *tail = name ? strrchr(name, '.') : NULL;
  return tail ? tail + 1 : name;
}

typedef enum {
  NY_BUILTIN_ALLOC_NONE = 0,
  NY_BUILTIN_ALLOC_MALLOC,
  NY_BUILTIN_ALLOC_CALLOC,
  NY_BUILTIN_ALLOC_REALLOC,
  NY_BUILTIN_ALLOC_FREE,
} ny_builtin_alloc_kind_t;

static inline ny_builtin_alloc_kind_t ny_builtin_alloc_kind(const char *name) {
  const char *leaf = ny_tail_name(name);
  if (!leaf)
    return NY_BUILTIN_ALLOC_NONE;
  if (strncmp(leaf, "__", 2) == 0)
    leaf += 2;
  if (strcmp(leaf, "malloc") == 0 || strcmp(leaf, "zalloc") == 0)
    return NY_BUILTIN_ALLOC_MALLOC;
  if (strcmp(leaf, "calloc") == 0)
    return NY_BUILTIN_ALLOC_CALLOC;
  if (strcmp(leaf, "realloc") == 0)
    return NY_BUILTIN_ALLOC_REALLOC;
  if (strcmp(leaf, "free") == 0)
    return NY_BUILTIN_ALLOC_FREE;
  return NY_BUILTIN_ALLOC_NONE;
}

#if defined(__has_include)
#if __has_include("nytrix_version.h")
#include "nytrix_version.h"
#endif
#endif

#ifndef VERSION
#ifdef NYTRIX_VERSION
#define VERSION NYTRIX_VERSION
#else
#define VERSION "0.9.0+source"
#endif
#endif
#ifndef NYTRIX_VERSION
#define NYTRIX_VERSION VERSION
#define NYTRIX_VERSION_MAJOR 0
#define NYTRIX_VERSION_MINOR 9
#define NYTRIX_VERSION_PATCH 0
#define NYTRIX_VERSION_BUILD 0
#define NYTRIX_VERSION_COMMIT "source"
#define NYTRIX_VERSION_SOURCE "source"
#define NYTRIX_VERSION_DIRTY 0
#endif

typedef enum {
  STD_MODE_DEFAULT,
  STD_MODE_NONE,
  STD_MODE_FULL,
  STD_MODE_MINIMAL,
  STD_MODE_BC,
} std_mode_t;

extern int color_mode;

static inline bool ny_env_truthy(const char *v) {
  return v && *v && strcmp(v, "0") != 0 && strcasecmp(v, "false") != 0 &&
         strcasecmp(v, "off") != 0 && strcasecmp(v, "no") != 0 && strcasecmp(v, "never") != 0;
}

static inline bool ny_color_mode_value(const char *mode, bool *out) {
  if (!mode || !*mode)
    return false;
  if (strcmp(mode, "always") == 0 || strcmp(mode, "on") == 0 || strcmp(mode, "1") == 0 ||
      strcasecmp(mode, "true") == 0 || strcasecmp(mode, "yes") == 0) {
    *out = true;
    return true;
  }
  if (strcmp(mode, "never") == 0 || strcmp(mode, "off") == 0 || strcmp(mode, "0") == 0 ||
      strcasecmp(mode, "false") == 0 || strcasecmp(mode, "no") == 0) {
    *out = false;
    return true;
  }
  return false;
}

static inline bool color_enabled(void) {
  if (color_mode == 0)
    return false;
  if (color_mode == 1)
    return true;
  static int enabled = -1;
  if (enabled != -1)
    return enabled != 0;
  bool env_color = false;
  if (ny_color_mode_value(getenv("NYTRIX_COLOR"), &env_color) ||
      ny_color_mode_value(getenv("NYTRIX_TOOL_COLOR"), &env_color)) {
    enabled = env_color ? 1 : 0;
    return env_color;
  }
  if (getenv("NO_COLOR")) {
    enabled = 0;
    return false;
  }
  if (ny_env_truthy(getenv("CLICOLOR_FORCE")) || ny_env_truthy(getenv("FORCE_COLOR"))) {
    enabled = 1;
    return true;
  }
  const char *term_program = getenv("TERM_PROGRAM");
  if (term_program && strcmp(term_program, "vscode") == 0) {
    enabled = 1;
    return true;
  }
  enabled = isatty(STDOUT_FILENO);
  return enabled != 0;
}

#define NY_CLR_RESET "\033[0m"
#define NY_CLR_BOLD "\033[1m"
#define NY_CLR_BLACK "\033[30m"
#define NY_CLR_RED "\033[31m"
#define NY_CLR_GREEN "\033[32m"
#define NY_CLR_YELLOW "\033[33m"
#define NY_CLR_BLUE "\033[34m"
#define NY_CLR_MAGENTA "\033[35m"
#define NY_CLR_CYAN "\033[36m"
#define NY_CLR_WHITE "\033[37m"
#define NY_CLR_BRIGHT_BLACK "\033[90m"
#define NY_CLR_BRIGHT_RED "\033[91m"
#define NY_CLR_BRIGHT_GREEN "\033[92m"
#define NY_CLR_BRIGHT_YELLOW "\033[93m"
#define NY_CLR_BRIGHT_BLUE "\033[94m"
#define NY_CLR_BRIGHT_MAGENTA "\033[95m"
#define NY_CLR_BRIGHT_CYAN "\033[96m"
#define NY_CLR_BRIGHT_WHITE "\033[97m"
#define NY_CLR_GRAY NY_CLR_BRIGHT_BLACK
#define NY_CLR_UNDER "\033[4m"

static inline const char *clr(const char *code) { return color_enabled() ? code : ""; }

extern int verbose_enabled;
bool ny_log_should_emit(const char *fmt);
void ny_progress_stderr_lock(void);
void ny_progress_stderr_unlock(void);

#define NY_LOG_WITH_PROGRESS_LOCK(body)                                                            \
  do {                                                                                             \
    ny_progress_stderr_lock();                                                                     \
    do {                                                                                           \
      body                                                                                         \
    } while (0);                                                                                   \
    ny_progress_stderr_unlock();                                                                   \
  } while (0)

#define NY_LOG_V1(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 1 && ny_log_should_emit(fmt)) {                                         \
      NY_LOG_WITH_PROGRESS_LOCK(                                                                   \
          fprintf(stderr, "%s[*]%s " fmt, clr(NY_CLR_CYAN), clr(NY_CLR_RESET), ##__VA_ARGS__););   \
    }                                                                                              \
  } while (0)

#define NY_LOG_V2(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 2 && ny_log_should_emit(fmt)) {                                         \
      NY_LOG_WITH_PROGRESS_LOCK(fprintf(stderr, "%s[**]%s " fmt, clr(NY_CLR_MAGENTA),              \
                                        clr(NY_CLR_RESET), ##__VA_ARGS__););                       \
    }                                                                                              \
  } while (0)

#define NY_LOG_V3(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 3 && ny_log_should_emit(fmt)) {                                         \
      NY_LOG_WITH_PROGRESS_LOCK(fprintf(stderr, "%s[***]%s " fmt, clr(NY_CLR_YELLOW),              \
                                        clr(NY_CLR_RESET), ##__VA_ARGS__););                       \
    }                                                                                              \
  } while (0)

#define NY_LOG_INFO(fmt, ...) NY_LOG_V1(fmt, ##__VA_ARGS__)

#define NY_LOG_ERR(fmt, ...)                                                                       \
  do {                                                                                             \
    if (ny_log_should_emit(fmt)) {                                                                 \
      NY_LOG_WITH_PROGRESS_LOCK(                                                                   \
          fprintf(stderr, "%sError:%s " fmt, clr(NY_CLR_RED), clr(NY_CLR_RESET), ##__VA_ARGS__);); \
    }                                                                                              \
  } while (0)

#define NY_LOG_WARN(fmt, ...)                                                                      \
  do {                                                                                             \
    if (ny_log_should_emit(fmt)) {                                                                 \
      NY_LOG_WITH_PROGRESS_LOCK(fprintf(stderr, "%sWarning:%s " fmt, clr(NY_CLR_YELLOW),           \
                                        clr(NY_CLR_RESET), ##__VA_ARGS__););                       \
    }                                                                                              \
  } while (0)

#define NY_LOG_SUCCESS(fmt, ...)                                                                   \
  do {                                                                                             \
    NY_LOG_WITH_PROGRESS_LOCK(fprintf(stderr, "%sSuccess:%s " fmt, clr(NY_CLR_GREEN),              \
                                      clr(NY_CLR_RESET), ##__VA_ARGS__););                         \
  } while (0)

extern int debug_enabled;

#ifdef DEBUG
#define NY_LOG_DEBUG(fmt, ...)                                                                     \
  do {                                                                                             \
    if (debug_enabled) {                                                                           \
      NY_LOG_WITH_PROGRESS_LOCK(fprintf(stderr, "%s[DEBUG]%s " fmt, clr(NY_CLR_GRAY),              \
                                        clr(NY_CLR_RESET), ##__VA_ARGS__););                       \
    }                                                                                              \
  } while (0)
#else
#define NY_LOG_DEBUG(fmt, ...)                                                                     \
  do {                                                                                             \
    (void)0;                                                                                       \
  } while (0)
#endif

static inline bool ny_strndup_try(const char *s, size_t n, char **out) {
  if (!out)
    return false;
  *out = NULL;
  if (!s)
    return false;
  size_t bytes = 0;
  if (!ny_size_add_ok(n, 1, &bytes))
    return false;
  char *r = (char *)malloc(bytes);
  if (!r)
    return false;
  memcpy(r, s, n);
  r[n] = '\0';
  *out = r;
  return true;
}

static inline char *ny_strndup(const char *s, size_t n) {
  char *r = NULL;
  if (!ny_strndup_try(s, n, &r)) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  return r;
}

/* Legacy vector macros below retain their fatal allocation contract.  New
 * code that can recover from allocation failure must use the explicit
 * ny_vec_*_try helpers, which leave vector storage unchanged on failure. */
static inline bool ny_vec_reserve_try(void **data, size_t *cap,
                                      size_t new_cap, size_t elem_size) {
  if (!data || !cap || elem_size == 0)
    return false;
  if (new_cap <= *cap)
    return true;
  size_t bytes = 0;
  if (!ny_size_mul_ok(new_cap, elem_size, &bytes))
    return false;
  void *grown = realloc(*data, bytes);
  if (!grown)
    return false;
  *data = grown;
  *cap = new_cap;
  return true;
}

static inline bool ny_vec_push_try(void **data, size_t *len, size_t *cap,
                                   size_t elem_size, const void *value) {
  if (!data || !len || !cap || elem_size == 0 || !value || *len > *cap)
    return false;
  size_t offset = 0;
  if (!ny_size_mul_ok(*len, elem_size, &offset))
    return false;
  if (*len == *cap) {
    if (*cap > SIZE_MAX / 2)
      return false;
    size_t next = *cap ? *cap * 2 : 8;
    if (!ny_vec_reserve_try(data, cap, next, elem_size))
      return false;
  }
  memcpy((unsigned char *)*data + offset, value, elem_size);
  ++*len;
  return true;
}

#define vec_reserve_try(vec, new_cap) \
  ny_vec_reserve_try((void **)&(vec)->data, &(vec)->cap, (new_cap), \
                     sizeof(*(vec)->data))

#define vec_reserve_arena_try(arena, vec, new_cap) \
  ny_vec_reserve_arena_try((arena), (void **)&(vec)->data, (vec)->len, \
                           &(vec)->cap, (new_cap), sizeof(*(vec)->data))

#define VEC(type)                                                                                  \
  struct {                                                                                         \
    type *data;                                                                                    \
    size_t len, cap;                                                                               \
  }

#define vec_reserve(vec, new_cap) \
  do { \
    if (!vec_reserve_try((vec), (new_cap))) { \
      fprintf(stderr, "oom\n"); \
      exit(1); \
    } \
  } while (0)

#define vec_reserve_arena(arena, vec, new_cap) \
  do { \
    if (!vec_reserve_arena_try((arena), (vec), (new_cap))) { \
      fprintf(stderr, "oom\n"); \
      exit(1); \
    } \
  } while (0)

/* Capacity growth is checked before both doubling and byte-size conversion. */

/*
 * vec_push / vec_push_arena / vec_insert_arena grow via the reserve macros and
 * then assign directly, rather than calling ny_vec_push_try: that helper takes
 * `const void *value`, which would force `&(value)` and break rvalue arguments
 * such as `vec_push(v, make_item())`.
 */

#define vec_push(vec, value) \
  do { \
    if ((vec)->len == (vec)->cap) { \
      if ((vec)->cap > SIZE_MAX / 2) { \
        fprintf(stderr, "oom\n"); \
        exit(1); \
      } \
      vec_reserve((vec), (vec)->cap ? (vec)->cap * 2 : 8); \
    } \
    (vec)->data[(vec)->len++] = (value); \
  } while (0)

#define vec_free(vec)                                                                              \
  do {                                                                                             \
    free((vec)->data);                                                                             \
    (vec)->data = NULL;                                                                            \
    (vec)->len = (vec)->cap = 0;                                                                   \
  } while (0)

#define vec_init(vec)                                                                              \
  do {                                                                                             \
    (vec)->data = NULL;                                                                            \
    (vec)->len = (vec)->cap = 0;                                                                   \
  } while (0)

#define vec_push_arena(arena, vec, value) \
  do { \
    if ((vec)->len == (vec)->cap) { \
      if ((vec)->cap > SIZE_MAX / 2) { \
        fprintf(stderr, "oom\n"); \
        exit(1); \
      } \
      vec_reserve_arena((arena), (vec), (vec)->cap ? (vec)->cap * 2 : 8); \
    } \
    (vec)->data[(vec)->len++] = (value); \
  } while (0)

#define vec_insert_arena(arena, vec, idx, value) \
  do { \
    if ((vec)->len == (vec)->cap) { \
      if ((vec)->cap > SIZE_MAX / 2) { \
        fprintf(stderr, "oom\n"); \
        exit(1); \
      } \
      vec_reserve_arena((arena), (vec), (vec)->cap ? (vec)->cap * 2 : 8); \
    } \
    size_t _idx = (idx); \
    if (_idx < (vec)->len) { \
      memmove((vec)->data + _idx + 1, (vec)->data + _idx, \
              ((vec)->len - _idx) * sizeof(*(vec)->data)); \
    } \
    (vec)->data[_idx] = (value); \
    (vec)->len++; \
  } while (0)

typedef struct arena_region_t {
  struct arena_region_t *next;
  size_t cap;
  size_t off;
  unsigned char data[];
} arena_region_t;

typedef struct arena_t {
  arena_region_t *regions;
  arena_region_t *current;
  arena_region_t *last;
  size_t region_size;
  void *expr_pool;
  size_t expr_pool_left;
  void *stmt_pool;
  size_t stmt_pool_left;
} arena_t;

#ifndef NY_ARENA_BLOCK_SIZE
#define NY_ARENA_BLOCK_SIZE (64 * 1024)
#endif

static inline size_t arena_align_up_size(size_t v, size_t align) {
  if (align == 0 || (align & (align - 1)) != 0 ||
      v > SIZE_MAX - (align - 1))
    return SIZE_MAX;
  return (v + (align - 1)) & ~(align - 1);
}

static inline uintptr_t arena_align_up_ptr(uintptr_t v, size_t align) {
  if (align == 0 || (align & (align - 1)) != 0 ||
      v > UINTPTR_MAX - (uintptr_t)(align - 1))
    return UINTPTR_MAX;
  return (v + (uintptr_t)(align - 1)) & ~(uintptr_t)(align - 1);
}

static inline arena_region_t *arena_region_new(size_t payload_cap) {
  size_t total = 0;
  if (!ny_size_add_ok(sizeof(arena_region_t), payload_cap, &total))
    return NULL;
  arena_region_t *r = (arena_region_t *)calloc(1, total);
  if (!r)
    return NULL;
  r->cap = payload_cap;
  return r;
}

static inline void arena_push_region(arena_t *a, arena_region_t *r) {
  if (!a->regions) {
    a->regions = r;
    a->last = r;
  } else {
    a->last->next = r;
    a->last = r;
  }
  a->current = r;
}

static inline bool ny_arena_alloc_aligned_try(arena_t *a, size_t size,
                                              size_t align, void **out) {
  if (!out)
    return false;
  *out = NULL;
  if (size == 0)
    size = 1;
  if (align < sizeof(void *))
    align = sizeof(void *);
  if ((align & (align - 1)) != 0)
    align = sizeof(max_align_t);

  size_t rounded = arena_align_up_size(size, align);
  if (rounded == SIZE_MAX)
    return false;
  if (!a) {
    *out = calloc(1, rounded);
    return *out != NULL;
  }

  size_t default_region = a->region_size ? a->region_size : NY_ARENA_BLOCK_SIZE;
  for (arena_region_t *r = a->current; r; r = r->next) {
    uintptr_t base = (uintptr_t)(r->data + r->off);
    uintptr_t aligned = arena_align_up_ptr(base, align);
    if (aligned == UINTPTR_MAX)
      return false;
    size_t aligned_off = (size_t)(aligned - (uintptr_t)r->data);
    if (aligned_off <= r->cap && rounded <= r->cap - aligned_off) {
      *out = r->data + aligned_off;
      r->off = aligned_off + rounded;
      a->current = r;
      memset(*out, 0, rounded);
      return true;
    }
    a->current = r->next;
  }

  size_t region_cap = default_region;
  size_t required = 0;
  if (!ny_size_add_ok(rounded, align, &required))
    return false;
  if (required > region_cap)
    region_cap = required;
  arena_region_t *r = arena_region_new(region_cap);
  if (!r)
    return false;
  uintptr_t aligned = arena_align_up_ptr((uintptr_t)r->data, align);
  if (aligned == UINTPTR_MAX) {
    free(r);
    return false;
  }
  size_t aligned_off = (size_t)(aligned - (uintptr_t)r->data);
  if (aligned_off > r->cap || rounded > r->cap - aligned_off) {
    free(r);
    return false;
  }
  arena_push_region(a, r);
  *out = r->data + aligned_off;
  r->off = aligned_off + rounded;
  memset(*out, 0, rounded);
  return true;
}

static inline void *arena_alloc_aligned(arena_t *a, size_t size, size_t align) {
  void *out = NULL;
  if (!ny_arena_alloc_aligned_try(a, size, align, &out)) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  return out;
}

static inline void *arena_alloc(arena_t *a, size_t size) {
  return arena_alloc_aligned(a, size, sizeof(max_align_t));
}
static inline bool ny_vec_reserve_arena_try(
    arena_t *arena, void **data, size_t len, size_t *cap, size_t new_cap,
    size_t elem_size) {
  if (!data || !cap || elem_size == 0 || len > *cap)
    return false;
  if (new_cap <= *cap)
    return true;
  size_t bytes = 0;
  if (!ny_size_mul_ok(new_cap, elem_size, &bytes))
    return false;
  size_t copied_bytes = 0;
  if (!ny_size_mul_ok(len, elem_size, &copied_bytes))
    return false;
  void *new_data = NULL;
  if (!ny_arena_alloc_aligned_try(arena, bytes, sizeof(max_align_t),
                                  &new_data))
    return false;
  if (*data && copied_bytes)
    memcpy(new_data, *data, copied_bytes);
  *data = new_data;
  *cap = new_cap;
  return true;
}


static inline bool ny_arena_strndup_try(arena_t *a, const char *s, size_t n,
                                        char **out) {
  if (!out)
    return false;
  *out = NULL;
  if (!s)
    return false;
  size_t bytes = 0;
  if (!ny_size_add_ok(n, 1, &bytes))
    return false;
  char *mem = NULL;
  if (!ny_arena_alloc_aligned_try(a, bytes, sizeof(max_align_t),
                                  (void **)&mem))
    return false;
  memcpy(mem, s, n);
  mem[n] = '\0';
  *out = mem;
  return true;
}

static inline char *arena_strndup(arena_t *a, const char *s, size_t n) {
  char *mem = NULL;
  if (!ny_arena_strndup_try(a, s, n, &mem)) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  return mem;
}

static inline void arena_reset(arena_t *a) {
  if (!a)
    return;
  for (arena_region_t *r = a->regions; r; r = r->next)
    r->off = 0;
  a->current = a->regions;
  a->expr_pool = NULL;
  a->expr_pool_left = 0;
  a->stmt_pool = NULL;
  a->stmt_pool_left = 0;
}

static inline void arena_free(arena_t *a) {
  if (!a)
    return;
  arena_region_t *r = a->regions;
  while (r) {
    arena_region_t *next = r->next;
    free(r);
    r = next;
  }
  memset(a, 0, sizeof(*a));
}
