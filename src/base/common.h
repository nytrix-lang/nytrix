#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef enum {
  STD_MODE_DEFAULT,
  STD_MODE_NONE,
  STD_MODE_FULL,
  STD_MODE_MINIMAL,
} std_mode_t;

extern int color_mode;

static inline bool color_enabled(void) {
  if (color_mode == 0)
    return false;
  if (color_mode == 1)
    return true;

  static int enabled = -1;
  if (enabled != -1)
    return enabled != 0;

  if (getenv("NO_COLOR")) {
    enabled = 0;
    return false;
  }

  enabled = isatty(STDERR_FILENO);
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

static inline const char *clr(const char *code) {
  return color_enabled() ? code : "";
}

extern int verbose_enabled;

#define NY_LOG_V1(fmt, ...)                                                    \
  do {                                                                         \
    if (verbose_enabled >= 1) {                                                \
      fprintf(stderr, "%s[*]%s " fmt, clr(NY_CLR_CYAN), clr(NY_CLR_RESET),     \
              ##__VA_ARGS__);                                                  \
    }                                                                          \
  } while (0)

#define NY_LOG_V2(fmt, ...)                                                    \
  do {                                                                         \
    if (verbose_enabled >= 2) {                                                \
      fprintf(stderr, "%s[**]%s " fmt, clr(NY_CLR_MAGENTA), clr(NY_CLR_RESET), \
              ##__VA_ARGS__);                                                  \
    }                                                                          \
  } while (0)

#define NY_LOG_V3(fmt, ...)                                                    \
  do {                                                                         \
    if (verbose_enabled >= 3) {                                                \
      fprintf(stderr, "%s[***]%s " fmt, clr(NY_CLR_YELLOW), clr(NY_CLR_RESET), \
              ##__VA_ARGS__);                                                  \
    }                                                                          \
  } while (0)

#define NY_LOG_INFO(fmt, ...) NY_LOG_V1(fmt, ##__VA_ARGS__)

#define NY_LOG_ERR(fmt, ...)                                                   \
  do {                                                                         \
    fprintf(stderr, "%sError:%s " fmt, clr(NY_CLR_RED), clr(NY_CLR_RESET),     \
            ##__VA_ARGS__);                                                    \
  } while (0)

#define NY_LOG_WARN(fmt, ...)                                                  \
  do {                                                                         \
    fprintf(stderr, "%sWarning:%s " fmt, clr(NY_CLR_YELLOW),                   \
            clr(NY_CLR_RESET), ##__VA_ARGS__);                                 \
  } while (0)

#define NY_LOG_SUCCESS(fmt, ...)                                               \
  do {                                                                         \
    fprintf(stderr, "%sSuccess:%s " fmt, clr(NY_CLR_GREEN), clr(NY_CLR_RESET), \
            ##__VA_ARGS__);                                                    \
  } while (0)

extern int debug_enabled;

#ifdef DEBUG
#define NY_LOG_DEBUG(fmt, ...)                                                 \
  do {                                                                         \
    if (debug_enabled) {                                                       \
      fprintf(stderr, "%s[DEBUG]%s " fmt, clr(NY_CLR_GRAY), clr(NY_CLR_RESET), \
              ##__VA_ARGS__);                                                  \
    }                                                                          \
  } while (0)
#else
#define NY_LOG_DEBUG(fmt, ...)                                                 \
  do {                                                                         \
    (void)0;                                                                   \
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

// Simple growable array for POD types.

#define VEC(type)                                                              \
  struct {                                                                     \
    type *data;                                                                \
    size_t len, cap;                                                           \
  }

#define vec_push(vec, value)                                                   \
  do {                                                                         \
    if ((vec)->len == (vec)->cap) {                                            \
      size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;                        \
      void *tmp = realloc((vec)->data, new_cap * sizeof(*(vec)->data));        \
      if (!tmp) {                                                              \
        fprintf(stderr, "oom\n");                                              \
        exit(1);                                                               \
      }                                                                        \
      (vec)->data = tmp;                                                       \
      (vec)->cap = new_cap;                                                    \
    }                                                                          \
    (vec)->data[(vec)->len++] = (value);                                       \
  } while (0)

#define vec_free(vec)                                                          \
  do {                                                                         \
    free((vec)->data);                                                         \
    (vec)->data = NULL;                                                        \
    (vec)->len = (vec)->cap = 0;                                               \
  } while (0)

#define vec_init(vec)                                                          \
  do {                                                                         \
    (vec)->data = NULL;                                                        \
    (vec)->len = (vec)->cap = 0;                                               \
  } while (0)

#define vec_push_arena(arena, vec, value)                                      \
  do {                                                                         \
    if ((vec)->len == (vec)->cap) {                                            \
      size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;                        \
      void *new_data = arena_alloc(arena, new_cap * sizeof(*(vec)->data));     \
      if ((vec)->data)                                                         \
        memcpy(new_data, (vec)->data, (vec)->len * sizeof(*(vec)->data));      \
      (vec)->data = new_data;                                                  \
      (vec)->cap = new_cap;                                                    \
    }                                                                          \
    (vec)->data[(vec)->len++] = (value);                                       \
  } while (0)
// Arena tracking raw allocations for bulk free.
typedef struct arena_t {
  void **items;
  size_t len, cap;
} arena_t;

static inline void *arena_alloc(arena_t *a, size_t size) {
  void *mem = calloc(1, size);
  if (!mem) {
    fprintf(stderr, "oom\n");
    exit(1);
  }
  if (a) {
    if (a->len == a->cap) {
      size_t new_cap = a->cap ? a->cap * 2 : 8;
      void **tmp = realloc(a->items, new_cap * sizeof(void *));
      if (!tmp) {
        fprintf(stderr, "oom\n");
        exit(1);
      }
      a->items = tmp;
      a->cap = new_cap;
    }
    a->items[a->len++] = mem;
  }
  return mem;
}

static inline char *arena_strndup(arena_t *a, const char *s, size_t n) {
  char *mem = (char *)arena_alloc(a, n + 1);
  memcpy(mem, s, n);
  mem[n] = '\0';
  return mem;
}

static inline void arena_free(arena_t *a) {
  if (!a)
    return;
  for (size_t i = 0; i < a->len; ++i)
    free(a->items[i]);
  free(a->items);
  a->items = NULL;
  a->len = a->cap = 0;
}
