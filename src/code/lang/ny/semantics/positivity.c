/*
 * Strict positivity of inductive families (enum declarations).
 *
 * A constructor argument may mention the family being defined only in
 * strictly positive positions: never inside the parameter list of a function
 * type, at any depth. Otherwise a value of the family can be built from a
 * function that consumes it, and a non-terminating term inhabits every type
 * (the classic `enum Bad { Mk(fn(Bad) int) }`).
 *
 * The check runs once every enum of the program is known, because the
 * occurrence can be hidden:
 *  - behind a type-level definition (`type F = fn(Bad) int`) - payload types
 *    are read from `enum_field_t.type_src`, where the parser expanded them;
 *  - in a result that is itself a function (`fn(int) fn(Bad) int`);
 *  - in another family of a mutual group (`A` mentions `B` negatively, `B`
 *    contains `A`): the family set is the strongly connected component;
 *  - through a parameter of another enum (`W(T<Bad>)` where `T<A>` uses `A`
 *    in a function domain): a family passed as an argument must be strictly
 *    positive in that enum's corresponding parameter.
 */
#include "code/lang/ny/gen/internal.h"
#include "code/lang/ny/parser/ast.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const stmt_t **enums;
  size_t len, cap;
  /* Reachability matrix over enums (mention graph), n*n bits as bytes. */
  unsigned char *reach;
} ny_pos_ctx_t;

static const char *pos_leaf(const char *name) {
  const char *dot = name ? strrchr(name, '.') : NULL;
  return dot ? dot + 1 : name;
}

static void pos_collect(ny_pos_ctx_t *c, const stmt_t *s, unsigned depth) {
  if (!s || depth > 64)
    return;
  if (s->kind == NY_S_ENUM && s->as.enu.name) {
    if (c->len == c->cap) {
      size_t cap = c->cap ? c->cap * 2 : 64;
      const stmt_t **grown = realloc(c->enums, cap * sizeof(*grown));
      if (!grown)
        return;
      c->enums = grown;
      c->cap = cap;
    }
    c->enums[c->len++] = s;
    return;
  }
  if (s->kind == NY_S_MODULE)
    for (size_t i = 0; i < s->as.module.body.len; ++i)
      pos_collect(c, s->as.module.body.data[i], depth + 1);
  else if (s->kind == NY_S_BLOCK)
    for (size_t i = 0; i < s->as.block.body.len; ++i)
      pos_collect(c, s->as.block.body.data[i], depth + 1);
}

static bool pos_ident_start(char ch) {
  return isalpha((unsigned char)ch) || ch == '_';
}
static bool pos_ident_char(char ch) {
  return isalnum((unsigned char)ch) || ch == '_' || ch == '.';
}

/* Index of the enum whose leaf name is `name[0..len)`, or -1. When several
 * modules declare the same leaf, the one in `prefer_file` wins. */
static long pos_find(const ny_pos_ctx_t *c, const char *name, size_t len,
                     const char *prefer_file) {
  long found = -1;
  for (size_t i = 0; i < c->len; ++i) {
    const char *leaf = pos_leaf(c->enums[i]->as.enu.name);
    if (strlen(leaf) != len || strncmp(leaf, name, len) != 0)
      continue;
    if (found < 0)
      found = (long)i;
    if (prefer_file && c->enums[i]->tok.filename &&
        strcmp(c->enums[i]->tok.filename, prefer_file) == 0)
      return (long)i;
  }
  return found;
}

/* Calls `fn` for each identifier in the text. */
#define POS_FOR_EACH_IDENT(text, len, body)                                    \
  for (size_t _i = 0; _i < (len);) {                                          \
    if (!pos_ident_start((text)[_i]) ||                                        \
        (_i > 0 && pos_ident_char((text)[_i - 1]))) {                          \
      _i++;                                                                    \
      continue;                                                                \
    }                                                                          \
    size_t _s = _i;                                                            \
    while (_i < (len) && pos_ident_char((text)[_i]))                           \
      _i++;                                                                    \
    const char *_id = (text) + _s;                                             \
    size_t _n = _i - _s;                                                       \
    const char *_dot = NULL;                                                   \
    for (size_t _k = 0; _k < _n; ++_k)                                         \
      if (_id[_k] == '.')                                                      \
        _dot = _id + _k;                                                       \
    if (_dot) {                                                                \
      _n -= (size_t)(_dot + 1 - _id);                                          \
      _id = _dot + 1;                                                          \
    }                                                                          \
    body                                                                       \
  }

/* The set of names the current check treats as "the family". */
typedef struct {
  const char *names[64];
  size_t lens[64];
  size_t len;
} pos_family_t;

static bool pos_mentions(const char *text, size_t len,
                         const pos_family_t *fam) {
  bool hit = false;
  POS_FOR_EACH_IDENT(text, len, {
    for (size_t f = 0; f < fam->len && !hit; ++f)
      if (fam->lens[f] == _n && strncmp(fam->names[f], _id, _n) == 0)
        hit = true;
  })
  return hit;
}

/* End of the balanced region that starts at `open` (`(` or `<`). */
static size_t pos_close(const char *t, size_t len, size_t open) {
  int level = 0;
  for (size_t i = open; i < len; ++i) {
    if (t[i] == '(' || t[i] == '<')
      level++;
    else if (t[i] == ')' || t[i] == '>') {
      level--;
      if (level == 0)
        return i;
    }
  }
  return len;
}

/* Splits `t[0..len)` at top-level commas. */
static size_t pos_split(const char *t, size_t len, const char **parts,
                        size_t *lens, size_t max) {
  size_t n = 0, start = 0;
  int level = 0;
  for (size_t i = 0; i <= len && n < max; ++i) {
    if (i == len || (t[i] == ',' && level == 0)) {
      size_t s = start, e = i;
      while (s < e && t[s] == ' ')
        s++;
      while (e > s && t[e - 1] == ' ')
        e--;
      if (e > s) {
        parts[n] = t + s;
        lens[n++] = e - s;
      }
      start = i + 1;
      continue;
    }
    if (t[i] == '(' || t[i] == '<')
      level++;
    else if (t[i] == ')' || t[i] == '>')
      level--;
  }
  return n;
}

typedef struct {
  ny_pos_ctx_t *ctx;
  const char *file;
  /* (enum, parameter) pairs being checked, to cut recursion. */
  long stack_enum[32];
  size_t stack_param[32];
  size_t depth;
} pos_walk_t;

static bool pos_param_positive(pos_walk_t *w, long e, size_t param);

/*
 * True when every occurrence of the family in the type `t[0..len)` is
 * strictly positive.
 */
static bool pos_type_ok(pos_walk_t *w, const char *t, size_t len,
                        const pos_family_t *fam) {
  while (len > 0 && (*t == ' ' || *t == '?' || *t == '*')) {
    t++;
    len--;
  }
  while (len > 0 && t[len - 1] == ' ')
    len--;
  if (len == 0 || !pos_mentions(t, len, fam))
    return true;
  /* Parenthesised type. */
  if (*t == '(') {
    size_t close = pos_close(t, len, 0);
    if (close + 1 >= len)
      return pos_type_ok(w, t + 1, close > 0 ? close - 1 : 0, fam);
  }
  /* Function type: no mention in the domain; the result is checked. */
  if (len > 3 && strncmp(t, "fn", 2) == 0 && (t[2] == '(' || t[2] == ' ')) {
    size_t open = 2;
    while (open < len && t[open] == ' ')
      open++;
    if (open >= len || t[open] != '(')
      return false;
    size_t close = pos_close(t, len, open);
    if (close >= len)
      return false;
    if (pos_mentions(t + open + 1, close - open - 1, fam))
      return false;
    return pos_type_ok(w, t + close + 1, len - close - 1, fam);
  }
  /* Named type, possibly applied. */
  size_t n = 0;
  while (n < len && pos_ident_char(t[n]))
    n++;
  if (n == 0)
    return !pos_mentions(t, len, fam);
  size_t rest = n;
  while (rest < len && t[rest] == ' ')
    rest++;
  /* A dependent pair or proof proposition: every occurrence must be
   * positive data; reject a function type that mentions the family. */
  if (rest < len && (t[rest] == '<' || t[rest] == '(')) {
    size_t close = pos_close(t, len, rest);
    const char *inner = t + rest + 1;
    size_t inner_len = close > rest ? close - rest - 1 : 0;
    const char *parts[32];
    size_t lens[32];
    size_t argc = pos_split(inner, inner_len, parts, lens, 32);
    const char *head = t;
    size_t head_len = n;
    for (size_t k = 0; k < head_len; ++k)
      if (t[k] == '.') {
        head = t + k + 1;
        head_len = n - k - 1;
      }
    bool head_is_family = false;
    for (size_t f = 0; f < fam->len; ++f)
      if (fam->lens[f] == head_len &&
          strncmp(fam->names[f], head, head_len) == 0)
        head_is_family = true;
    long target = head_is_family ? -1 : pos_find(w->ctx, head, head_len,
                                                 w->file);
    for (size_t a = 0; a < argc; ++a) {
      if (!pos_mentions(parts[a], lens[a], fam))
        continue;
      if (!pos_type_ok(w, parts[a], lens[a], fam))
        return false;
      if (target >= 0 && !pos_param_positive(w, target, a))
        return false;
    }
    /* Whatever follows the application (`exists(T x) V`) is a type too. */
    if (close + 1 < len)
      return pos_type_ok(w, t + close + 1, len - close - 1, fam);
    return true;
  }
  return true;
}

/* True when parameter `param` of enum `e` occurs only strictly positively in
 * its constructors. */
static bool pos_param_positive(pos_walk_t *w, long e, size_t param) {
  const stmt_t *s = w->ctx->enums[e];
  if (param >= s->as.enu.type_params.len)
    return true; /* an index, not a parameter: never in a function domain */
  for (size_t i = 0; i < w->depth; ++i)
    if (w->stack_enum[i] == e && w->stack_param[i] == param)
      return true; /* coinductive hypothesis for nested occurrences */
  if (w->depth >= 32)
    return false;
  w->stack_enum[w->depth] = e;
  w->stack_param[w->depth] = param;
  w->depth++;
  pos_family_t fam = {.len = 1};
  fam.names[0] = s->as.enu.type_params.data[param];
  fam.lens[0] = strlen(fam.names[0]);
  bool ok = true;
  for (size_t i = 0; ok && i < s->as.enu.items.len; ++i) {
    const stmt_enum_item_t *item = &s->as.enu.items.data[i];
    for (size_t f = 0; ok && f < item->fields.len; ++f) {
      const enum_field_t *field = &item->fields.data[f];
      const char *src = field->type_src ? field->type_src : field->type_name;
      if (src)
        ok = pos_type_ok(w, src, strlen(src), &fam);
    }
  }
  w->depth--;
  return ok;
}

static void pos_build_reach(ny_pos_ctx_t *c) {
  size_t n = c->len;
  c->reach = calloc(n > 0 ? n * n : 1, 1);
  if (!c->reach)
    return;
  for (size_t i = 0; i < n; ++i) {
    const stmt_t *s = c->enums[i];
    for (size_t k = 0; k < s->as.enu.items.len; ++k) {
      const stmt_enum_item_t *item = &s->as.enu.items.data[k];
      for (size_t f = 0; f < item->fields.len; ++f) {
        const enum_field_t *field = &item->fields.data[f];
        const char *src = field->type_src ? field->type_src : field->type_name;
        if (!src)
          continue;
        size_t len = strlen(src);
        POS_FOR_EACH_IDENT(src, len, {
          long j = pos_find(c, _id, _n, s->tok.filename);
          if (j >= 0)
            c->reach[i * n + (size_t)j] = 1;
        })
      }
    }
  }
  /* Transitive closure (Warshall); enum counts are small. */
  for (size_t k = 0; k < n; ++k)
    for (size_t i = 0; i < n; ++i)
      if (c->reach[i * n + k])
        for (size_t j = 0; j < n; ++j)
          if (c->reach[k * n + j])
            c->reach[i * n + j] = 1;
}

bool ny_check_enum_positivity(const program_t *prog) {
  if (!prog)
    return true;
  ny_pos_ctx_t c = {0};
  for (size_t i = 0; i < prog->body.len; ++i)
    pos_collect(&c, prog->body.data[i], 0);
  if (c.len == 0)
    return true;
  bool any_fn_payload = false;
  for (size_t i = 0; i < c.len && !any_fn_payload; ++i) {
    const stmt_t *s = c.enums[i];
    for (size_t k = 0; k < s->as.enu.items.len && !any_fn_payload; ++k)
      for (size_t f = 0; f < s->as.enu.items.data[k].fields.len; ++f) {
        const char *src = s->as.enu.items.data[k].fields.data[f].type_src;
        if (src && strstr(src, "fn")) {
          any_fn_payload = true;
          break;
        }
      }
  }
  /* No payload mentions a function type anywhere: nothing can be negative. */
  if (!any_fn_payload) {
    free(c.enums);
    return true;
  }
  pos_build_reach(&c);
  bool ok = true;
  size_t n = c.len;
  for (size_t i = 0; i < n; ++i) {
    const stmt_t *s = c.enums[i];
    pos_family_t fam = {0};
    for (size_t j = 0; j < n && fam.len < 64; ++j) {
      bool same = i == j || (c.reach && c.reach[i * n + j] &&
                             c.reach[j * n + i]);
      if (!same)
        continue;
      fam.names[fam.len] = pos_leaf(c.enums[j]->as.enu.name);
      fam.lens[fam.len] = strlen(fam.names[fam.len]);
      fam.len++;
    }
    pos_walk_t w = {.ctx = &c, .file = s->tok.filename};
    for (size_t k = 0; k < s->as.enu.items.len; ++k) {
      const stmt_enum_item_t *item = &s->as.enu.items.data[k];
      for (size_t f = 0; f < item->fields.len; ++f) {
        const enum_field_t *field = &item->fields.data[f];
        const char *src = field->type_src;
        if (!src || pos_type_ok(&w, src, strlen(src), &fam))
          continue;
        ny_diag_error(field->tok,
                      "enum payload breaks strict positivity: '%s' occurs in "
                      "a function parameter of '%s.%s' (%s)",
                      pos_leaf(s->as.enu.name), pos_leaf(s->as.enu.name),
                      item->name ? item->name : "?", src);
        ok = false;
      }
    }
  }
  free(c.reach);
  free(c.enums);
  return ok;
}
