#pragma once

#include "color.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char **items;
  size_t len;
  size_t cap;
} StrVec;

static inline void sv_push(StrVec *v, const char *s) {
  if (!v)
    return;
  if (v->len == v->cap) {
    size_t nc = v->cap ? v->cap * 2 : 32;
    char **p = (char **)realloc(v->items, nc * sizeof(char *));
    if (!p)
      return;
    v->items = p;
    v->cap = nc;
  }
  v->items[v->len] = strdup(s ? s : "");
  if (v->items[v->len])
    v->len++;
}

static inline void sv_free(StrVec *v) {
  if (!v)
    return;
  for (size_t i = 0; i < v->len; i++)
    free(v->items[i]);
  free(v->items);
  v->items = NULL;
  v->len = 0;
  v->cap = 0;
}

static inline const char *nyt_temp_dir(void) {
  static const char *envs[] = {"TMPDIR", "TEMP", "TMP"};
  for (size_t i = 0; i < sizeof(envs) / sizeof(envs[0]); i++) {
    const char *v = getenv(envs[i]);
    if (v && *v)
      return v;
  }
  return ".";
}

static inline void nyt_rule(FILE *out) {
  fprintf(out ? out : stdout, "%s----------------------------------------------------------------------%s\n",
          nyt_clr(NYT_GRAY), nyt_clr(NYT_RESET));
}

static inline void nyt_heading(const char *name) {
  (void)name;
  nyt_rule(stdout);
}

static inline void nyt_subheading(const char *name) {
  printf("\n%s%s%s\n", nyt_clr(NYT_BOLD), name ? name : "", nyt_clr(NYT_RESET));
}

static inline void nyt_kv(const char *key, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  printf("%s%-12s%s ", nyt_clr(NYT_GRAY), key ? key : "", nyt_clr(NYT_RESET));
  vprintf(fmt, ap);
  fputc('\n', stdout);
  va_end(ap);
}

static inline void nyt_vmsg(FILE *out, const char *label, const char *color, const char *fmt, va_list ap) {
  fprintf(out, "%s%s%s%s ", nyt_clr(color), nyt_clr(NYT_BOLD), label, nyt_clr(NYT_RESET));
  vfprintf(out, fmt, ap);
  fputc('\n', out);
}

static inline void nyt_msg(const char *label, const char *color, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  nyt_vmsg(stdout, label, color, fmt, ap);
  va_end(ap);
}

static inline void nyt_err(const char *tool, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s%s%s:%s ", nyt_clr(NYT_RED), nyt_clr(NYT_BOLD), tool ? tool : "ny-tool",
          nyt_clr(NYT_RESET));
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static inline void nyt_warn(const char *tool, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "%s%s%s:%s ", nyt_clr(NYT_YELLOW), nyt_clr(NYT_BOLD), tool ? tool : "ny-tool",
          nyt_clr(NYT_RESET));
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

static inline void nyt_section(const char *name) {
  nyt_heading(name);
}
