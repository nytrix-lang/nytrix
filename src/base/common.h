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

#if defined(__has_include)
#if __has_include("nytrix_version.h")
#include "nytrix_version.h"
#endif
#endif

#ifndef VERSION
#ifdef NYTRIX_VERSION
#define VERSION NYTRIX_VERSION
#else
#define VERSION "0.5.0+source"
#endif
#endif
#ifndef NYTRIX_VERSION
#define NYTRIX_VERSION VERSION
#define NYTRIX_VERSION_MAJOR 0
#define NYTRIX_VERSION_MINOR 5
#define NYTRIX_VERSION_PATCH 0
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
#define NY_CLR_RED "\033[31m"
#define NY_CLR_GREEN "\033[32m"
#define NY_CLR_YELLOW "\033[33m"
#define NY_CLR_BLUE "\033[34m"
#define NY_CLR_MAGENTA "\033[35m"
#define NY_CLR_CYAN "\033[36m"
#define NY_CLR_GRAY "\033[90m"
#define NY_CLR_UNDER "\033[4m"

static inline const char *clr(const char *code) { return color_enabled() ? code : ""; }

extern int verbose_enabled;
bool ny_log_should_emit(const char *fmt);

#define NY_LOG_V1(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 1 && ny_log_should_emit(fmt)) {                                         \
      fprintf(stderr, "%s[*]%s " fmt, clr(NY_CLR_CYAN), clr(NY_CLR_RESET), ##__VA_ARGS__);         \
    }                                                                                              \
  } while (0)

#define NY_LOG_V2(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 2 && ny_log_should_emit(fmt)) {                                         \
      fprintf(stderr, "%s[**]%s " fmt, clr(NY_CLR_MAGENTA), clr(NY_CLR_RESET), ##__VA_ARGS__);     \
    }                                                                                              \
  } while (0)

#define NY_LOG_V3(fmt, ...)                                                                        \
  do {                                                                                             \
    if (verbose_enabled >= 3 && ny_log_should_emit(fmt)) {                                         \
      fprintf(stderr, "%s[***]%s " fmt, clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), ##__VA_ARGS__);     \
    }                                                                                              \
  } while (0)

#define NY_LOG_INFO(fmt, ...) NY_LOG_V1(fmt, ##__VA_ARGS__)

#define NY_LOG_ERR(fmt, ...)                                                                       \
  do {                                                                                             \
    if (ny_log_should_emit(fmt)) {                                                                 \
      fprintf(stderr, "%sError:%s " fmt, clr(NY_CLR_RED), clr(NY_CLR_RESET), ##__VA_ARGS__);       \
    }                                                                                              \
  } while (0)

#define NY_LOG_WARN(fmt, ...)                                                                      \
  do {                                                                                             \
    if (ny_log_should_emit(fmt)) {                                                                 \
      fprintf(stderr, "%sWarning:%s " fmt, clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), ##__VA_ARGS__);  \
    }                                                                                              \
  } while (0)

#define NY_LOG_SUCCESS(fmt, ...)                                                                   \
  do {                                                                                             \
    fprintf(stderr, "%sSuccess:%s " fmt, clr(NY_CLR_GREEN), clr(NY_CLR_RESET), ##__VA_ARGS__);     \
  } while (0)

extern int debug_enabled;

#ifdef DEBUG
#define NY_LOG_DEBUG(fmt, ...)                                                                     \
  do {                                                                                             \
    if (debug_enabled) {                                                                           \
      fprintf(stderr, "%s[DEBUG]%s " fmt, clr(NY_CLR_GRAY), clr(NY_CLR_RESET), ##__VA_ARGS__);     \
    }                                                                                              \
  } while (0)
#else
#define NY_LOG_DEBUG(fmt, ...)                                                                     \
  do {                                                                                             \
    (void)0;                                                                                       \
  } while (0)
#endif

static inline char *ny_strndup(const char *s, size_t n) {
  char *r = (char *)malloc(n + 1);
  if (!r) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  memcpy(r, s, n);
  r[n] = '\0';
  return r;
}

#define VEC(type)                                                                                  \
  struct {                                                                                         \
    type *data;                                                                                    \
    size_t len, cap;                                                                               \
  }

#define vec_reserve(vec, new_cap)                                                                  \
  do {                                                                                             \
    if ((new_cap) > (vec)->cap) {                                                                  \
      void *tmp = realloc((vec)->data, (new_cap) * sizeof(*(vec)->data));                          \
      if (!tmp) {                                                                                  \
        fprintf(stderr, "oom\n");                                                                  \
        exit(1);                                                                                   \
      }                                                                                            \
      (vec)->data = tmp;                                                                           \
      (vec)->cap = (new_cap);                                                                      \
    }                                                                                              \
  } while (0)

#define vec_reserve_arena(arena, vec, new_cap)                                                     \
  do {                                                                                             \
    if ((new_cap) > (vec)->cap) {                                                                  \
      void *new_data = arena_alloc((arena), (new_cap) * sizeof(*(vec)->data));                     \
      if ((vec)->data)                                                                             \
        memcpy(new_data, (vec)->data, (vec)->len * sizeof(*(vec)->data));                          \
      (vec)->data = new_data;                                                                      \
      (vec)->cap = (new_cap);                                                                      \
    }                                                                                              \
  } while (0)

#define vec_push(vec, value)                                                                       \
  do {                                                                                             \
    if ((vec)->len == (vec)->cap) {                                                                \
      size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;                                            \
      void *tmp = realloc((vec)->data, new_cap * sizeof(*(vec)->data));                            \
      if (!tmp) {                                                                                  \
        fprintf(stderr, "oom\n");                                                                  \
        exit(1);                                                                                   \
      }                                                                                            \
      (vec)->data = tmp;                                                                           \
      (vec)->cap = new_cap;                                                                        \
    }                                                                                              \
    (vec)->data[(vec)->len++] = (value);                                                           \
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

#define vec_push_arena(arena, vec, value)                                                          \
  do {                                                                                             \
    if ((vec)->len == (vec)->cap) {                                                                \
      size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;                                            \
      void *new_data = arena_alloc(arena, new_cap * sizeof(*(vec)->data));                         \
      if ((vec)->data)                                                                             \
        memcpy(new_data, (vec)->data, (vec)->len * sizeof(*(vec)->data));                          \
      (vec)->data = new_data;                                                                      \
      (vec)->cap = new_cap;                                                                        \
    }                                                                                              \
    (vec)->data[(vec)->len++] = (value);                                                           \
  } while (0)

#define vec_insert_arena(arena, vec, idx, value)                                                   \
  do {                                                                                             \
    if ((vec)->len == (vec)->cap) {                                                                \
      size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;                                            \
      void *new_data = arena_alloc(arena, new_cap * sizeof(*(vec)->data));                         \
      if ((vec)->data)                                                                             \
        memcpy(new_data, (vec)->data, (vec)->len * sizeof(*(vec)->data));                          \
      (vec)->data = new_data;                                                                      \
      (vec)->cap = new_cap;                                                                        \
    }                                                                                              \
    size_t _idx = (idx);                                                                           \
    if (_idx < (vec)->len) {                                                                       \
      memmove((vec)->data + _idx + 1, (vec)->data + _idx,                                          \
              ((vec)->len - _idx) * sizeof(*(vec)->data));                                         \
    }                                                                                              \
    (vec)->data[_idx] = (value);                                                                   \
    (vec)->len++;                                                                                  \
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
  return (v + (align - 1)) & ~(align - 1);
}

static inline uintptr_t arena_align_up_ptr(uintptr_t v, size_t align) {
  return (v + (uintptr_t)(align - 1)) & ~(uintptr_t)(align - 1);
}

static inline arena_region_t *arena_region_new(size_t payload_cap) {
  arena_region_t *r = (arena_region_t *)calloc(1, sizeof(*r) + payload_cap);
  if (!r) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
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

static inline void *arena_alloc_aligned(arena_t *a, size_t size, size_t align) {
  if (size == 0)
    size = 1;
  if (align < sizeof(void *))
    align = sizeof(void *);
  if ((align & (align - 1)) != 0)
    align = sizeof(max_align_t);

  if (!a) {
    size_t rounded = arena_align_up_size(size, align);
    void *mem = calloc(1, rounded);
    if (!mem) {
      fprintf(stderr, "oom\n");
      exit(1);
    }
    return mem;
  }

  size_t rounded = arena_align_up_size(size, align);
  size_t default_region = a->region_size ? a->region_size : NY_ARENA_BLOCK_SIZE;
  arena_region_t *r = a->current;
  while (r) {
    uintptr_t base = (uintptr_t)(r->data + r->off);
    uintptr_t aligned = arena_align_up_ptr(base, align);
    size_t aligned_off = (size_t)(aligned - (uintptr_t)r->data);
    if (aligned_off <= r->cap && rounded <= r->cap - aligned_off) {
      void *mem = r->data + aligned_off;
      r->off = aligned_off + rounded;
      memset(mem, 0, rounded);
      return mem;
    }
    r = r->next;
    a->current = r;
  }

  size_t region_cap = default_region;
  if (rounded + align > region_cap)
    region_cap = rounded + align;
  r = arena_region_new(region_cap);
  arena_push_region(a, r);
  uintptr_t base = (uintptr_t)r->data;
  uintptr_t aligned = arena_align_up_ptr(base, align);
  size_t aligned_off = (size_t)(aligned - (uintptr_t)r->data);
  void *mem = r->data + aligned_off;
  r->off = aligned_off + rounded;
  memset(mem, 0, rounded);
  return mem;
}

static inline void *arena_alloc(arena_t *a, size_t size) {
  return arena_alloc_aligned(a, size, sizeof(max_align_t));
}

static inline char *arena_strndup(arena_t *a, const char *s, size_t n) {
  char *mem = (char *)arena_alloc(a, n + 1);
  memcpy(mem, s, n);
  mem[n] = '\0';
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
