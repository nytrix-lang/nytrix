#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int nt_color_mode;

static inline bool nt_color_enabled(void) {
	if (nt_color_mode == 0) return false;
	if (nt_color_mode == 1) return true;
	static int enabled = -1;
	if (enabled != -1) return enabled;
	if (getenv("NO_COLOR")) {
		enabled = 0;
		return false;
	}
	enabled = isatty(STDERR_FILENO);
	return enabled;
}

#define NT_CLR_RESET  "\033[0m"
#define NT_CLR_BOLD   "\033[1m"
#define NT_CLR_RED    "\033[31m"
#define NT_CLR_GREEN  "\033[32m"
#define NT_CLR_YELLOW "\033[33m"
#define NT_CLR_BLUE   "\033[34m"
#define NT_CLR_MAGENTA "\033[35m"
#define NT_CLR_CYAN   "\033[36m"
#define NT_CLR_GRAY   "\033[90m"
#define NT_CLR_UNDER  "\033[4m"

static inline const char *nt_clr(const char *code) {
	return nt_color_enabled() ? code : "";
}

extern int nt_verbose_enabled;

#define NT_LOG_INFO(fmt, ...) \
	do { \
		if (nt_verbose_enabled) { \
			fprintf(stderr, "%s[*]%s " fmt, nt_clr(NT_CLR_CYAN), nt_clr(NT_CLR_RESET), ##__VA_ARGS__); \
		} \
	} while (0)
#define NT_LOG_ERR(fmt, ...)  fprintf(stderr, "%sError:%s " fmt, nt_clr(NT_CLR_RED), nt_clr(NT_CLR_RESET), ##__VA_ARGS__)
#define NT_LOG_WARN(fmt, ...) fprintf(stderr, "%sWarning:%s " fmt, nt_clr(NT_CLR_YELLOW), nt_clr(NT_CLR_RESET), ##__VA_ARGS__)
#define NT_LOG_SUCCESS(fmt, ...) fprintf(stderr, "%sSuccess:%s " fmt, nt_clr(NT_CLR_GREEN), nt_clr(NT_CLR_RESET), ##__VA_ARGS__)

extern int nt_debug_enabled;

#ifdef DEBUG
#define NT_LOG_DEBUG(fmt, ...) \
	do { \
		if (nt_debug_enabled) { \
			fprintf(stderr, "%s[DEBUG]%s " fmt, nt_clr(NT_CLR_GRAY), nt_clr(NT_CLR_RESET), ##__VA_ARGS__); \
		} \
	} while (0)
#else
#define NT_LOG_DEBUG(fmt, ...)
#endif

static inline char *nt_strndup(const char *s, size_t n) {
	char *r = (char *)malloc(n + 1);
	if (!r) {
		fprintf(stderr, "oom\n");
		exit(1);
	}
	memcpy(r, s, n);
	r[n] = '\0';
	return r;
}

// TODO: stop using macros
// Simple growable array for POD types.

#define NT_VEC(type)        \
	struct {                \
		type *data;         \
		size_t len, cap;    \
	}

#define nt_vec_push(vec, value)                                      \
	do {                                                             \
		if ((vec)->len == (vec)->cap) {                              \
			size_t new_cap = (vec)->cap ? (vec)->cap * 2 : 8;        \
			void *tmp = realloc((vec)->data, new_cap * sizeof(*(vec)->data)); \
			if (!tmp) {                                              \
				fprintf(stderr, "oom\n");                            \
				exit(1);                                             \
			}                                                        \
			(vec)->data = tmp;                                       \
			(vec)->cap = new_cap;                                    \
		}                                                            \
		(vec)->data[(vec)->len++] = (value);                         \
	} while (0)

#define nt_vec_free(vec)          \
	do {                          \
		free((vec)->data);        \
		(vec)->data = NULL;       \
		(vec)->len = (vec)->cap = 0; \
	} while (0)

// Arena tracking raw allocations for bulk free.
typedef struct nt_arena {
	void **items;
	size_t len, cap;
} nt_arena;

static inline void *nt_arena_alloc(nt_arena *a, size_t size) {
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

static inline char *nt_arena_strndup(nt_arena *a, const char *s, size_t n) {
	char *mem = (char *)nt_arena_alloc(a, n + 1);
	if (!mem) return NULL;
	memcpy(mem, s, n);
	mem[n] = '\0';
	return mem;
}

static inline void nt_arena_free(nt_arena *a) {
	if (!a) return;
	for (size_t i = 0; i < a->len; ++i) free(a->items[i]);
	free(a->items);
	a->items = NULL;
	a->len = a->cap = 0;
}
