#include "code/lang/code.h"
#include "code/lang/priv.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NY_COMPTIME_FUEL_DEFAULT INT64_C(1000000)

char *ny_ct_i64_to_dec(int64_t v) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%" PRId64, v);
  return ny_strdup(buf);
}

static bool ny_ct_is_space_char(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

static char *ny_ct_canonical_decimal(const char *raw) {
  if (!raw)
    return ny_strdup("0");
  const char *p = raw;
  while (ny_ct_is_space_char(*p))
    p++;
  bool neg = false;
  if (*p == '+' || *p == '-') {
    neg = *p == '-';
    p++;
  }
  while (*p == '0')
    p++;
  const char *digits = p;
  while (*p >= '0' && *p <= '9')
    p++;
  const char *tail = p;
  while (ny_ct_is_space_char(*tail))
    tail++;
  if (*tail != '\0')
    return NULL;
  size_t len = (size_t)(p - digits);
  if (len == 0)
    return ny_strdup("0");
  size_t out_len = len + (neg ? 1u : 0u);
  char *out = malloc(out_len + 1);
  if (!out)
    return NULL;
  size_t pos = 0;
  if (neg)
    out[pos++] = '-';
  memcpy(out + pos, digits, len);
  out[out_len] = '\0';
  return out;
}

bool ny_ct_fuel_consume(codegen_t *cg) {
  if (!cg)
    return true;
  if (cg->env_cache.comptime_fuel_total == 0) {
    const char *env = getenv("NYTRIX_COMPTIME_FUEL");
    int64_t limit = NY_COMPTIME_FUEL_DEFAULT;
    if (env && *env) {
      char *end = NULL;
      int64_t parsed = (int64_t)strtoll(env, &end, 10);
      if (end && *end == '\0' && parsed > 0)
        limit = parsed;
    }
    cg->env_cache.comptime_fuel_total = limit;
    cg->env_cache.comptime_fuel_remaining = limit;
  }
  if (cg->env_cache.comptime_fuel_remaining <= 0)
    return false;
  cg->env_cache.comptime_fuel_remaining--;
  return true;
}

bool ny_ct_fast_eval_unary(const char *op, const ny_ct_fast_val_t *r,
                           ny_ct_fast_val_t *out) {
  if (!op || !r || !out)
    return false;
  switch (op[0]) {
  case '!':
    if (!op[1]) {
      bool t = false;
      if (!ny_ct_fast_truthy(r, &t))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = !t;
      return true;
    }
    break;
  case '-':
    if (!op[1] && r->kind == NY_CT_FAST_FLOAT) {
      out->kind = NY_CT_FAST_FLOAT;
      out->f64 = -r->f64;
      return true;
    }
    if (!op[1] && r->kind == NY_CT_FAST_INT) {
      if (r->i == INT64_MIN)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = -r->i;
      return true;
    }
    break;
  case '~':
    if (!op[1] && r->kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = ~r->i;
      return true;
    }
    break;
  }
  return false;
}

static bool ny_ct_fast_int_op(const char *op, int64_t l, int64_t r,
                              ny_ct_fast_val_t *out) {
  switch (op[0]) {
  case '+':
    if (__builtin_add_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '-':
    if (__builtin_sub_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '*':
    if (op[1] == '*') {
      if (!ny_checked_small_pow_i64(l, r, &out->i))
        return false;
      out->kind = NY_CT_FAST_INT;
      return true;
    }
    if (__builtin_mul_overflow(l, r, &out->i) || !ny_small_int_fits_i64(out->i))
      return false;
    out->kind = NY_CT_FAST_INT;
    return true;
  case '/':
    if (r == 0 || (l == INT64_MIN && r == -1))
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = l / r;
    return true;
  case '%':
    if (r == 0 || (l == INT64_MIN && r == -1))
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = l % r;
    return true;
  case '<':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l <= r;
      return true;
    }
    if (op[1] == '<') {
      if (r < 0 || r >= 63)
        return false;
      int64_t shifted = (int64_t)((uint64_t)l << (unsigned)r);
      if (!ny_small_int_fits_i64(shifted))
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = shifted;
      return true;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = l < r;
    return true;
  case '>':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l >= r;
      return true;
    }
    if (op[1] == '>') {
      if (r < 0 || r >= 63)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = l >> r;
      return true;
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = l > r;
    return true;
  case '=':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l == r;
      return true;
    }
    break;
  case '!':
    if (op[1] == '=') {
      out->kind = NY_CT_FAST_BOOL;
      out->b = l != r;
      return true;
    }
    break;
  case '|':
    out->kind = NY_CT_FAST_INT;
    out->i = l | r;
    return true;
  case '&':
    out->kind = NY_CT_FAST_INT;
    out->i = l & r;
    return true;
  case '^':
    out->kind = NY_CT_FAST_INT;
    out->i = l ^ r;
    return true;
  }
  return false;
}

static bool ny_ct_numeric_eq(const ny_ct_fast_val_t *l,
                             const ny_ct_fast_val_t *r, bool *out) {
  if (!l || !r || !out)
    return false;
  bool lf = l->kind == NY_CT_FAST_FLOAT;
  bool rf = r->kind == NY_CT_FAST_FLOAT;
  if ((lf || rf) && (lf || l->kind == NY_CT_FAST_INT) &&
      (rf || r->kind == NY_CT_FAST_INT)) {
    double ld = lf ? l->f64 : (double)l->i;
    double rd = rf ? r->f64 : (double)r->i;
    *out = ld == rd;
    return true;
  }
  bool ln = l->kind == NY_CT_FAST_INT || l->kind == NY_CT_FAST_BIGINT;
  bool rn = r->kind == NY_CT_FAST_INT || r->kind == NY_CT_FAST_BIGINT;
  if (!ln || !rn)
    return false;
  char lbuf[64], rbuf[64];
  const char *ls = l->s;
  const char *rs = r->s;
  if (l->kind == NY_CT_FAST_INT) {
    snprintf(lbuf, sizeof(lbuf), "%" PRId64, l->i);
    ls = lbuf;
  }
  if (r->kind == NY_CT_FAST_INT) {
    snprintf(rbuf, sizeof(rbuf), "%" PRId64, r->i);
    rs = rbuf;
  }
  *out = strcmp(ls ? ls : "0", rs ? rs : "0") == 0;
  return true;
}

static bool ny_ct_fast_values_equal(const ny_ct_fast_val_t *l,
                                    const ny_ct_fast_val_t *r, bool *equal,
                                    unsigned depth) {
  if (!l || !r || !equal || depth > 256)
    return false;
  if (ny_ct_numeric_eq(l, r, equal))
    return true;
  if (l->kind != r->kind) {
    *equal = false;
    return true;
  }
  switch (l->kind) {
  case NY_CT_FAST_NONE:
    *equal = true;
    return true;
  case NY_CT_FAST_BOOL:
    *equal = l->b == r->b;
    return true;
  case NY_CT_FAST_STR:
    *equal = l->len == r->len &&
             (l->len == 0 || (l->s && r->s && memcmp(l->s, r->s, l->len) == 0));
    return true;
  case NY_CT_FAST_LIST:
  case NY_CT_FAST_TUPLE: {
    if (l->len != r->len) {
      *equal = false;
      return true;
    }
    if (l->len && (!l->items || !r->items))
      return false;
    for (size_t i = 0; i < l->len; ++i) {
      bool item_equal = false;
      if (!ny_ct_fast_values_equal(&l->items[i], &r->items[i], &item_equal,
                                   depth + 1))
        return false;
      if (!item_equal) {
        *equal = false;
        return true;
      }
    }
    *equal = true;
    return true;
  }
  case NY_CT_FAST_DICT: {
    if (l->len != r->len) {
      *equal = false;
      return true;
    }
    if (l->len && (!l->items || !r->items))
      return false;
    for (size_t i = 0; i < l->len; ++i) {
      bool key_equal = false, value_equal = false;
      if (!ny_ct_fast_values_equal(&l->items[i * 2], &r->items[i * 2],
                                   &key_equal, depth + 1) ||
          !ny_ct_fast_values_equal(&l->items[i * 2 + 1], &r->items[i * 2 + 1],
                                   &value_equal, depth + 1))
        return false;
      if (!key_equal || !value_equal) {
        *equal = false;
        return true;
      }
    }
    *equal = true;
    return true;
  }
  default:
    *equal = false;
    return true;
  }
}

bool ny_ct_fast_eval_binary(const char *op, const ny_ct_fast_val_t *l,
                            const ny_ct_fast_val_t *r, ny_ct_fast_val_t *out) {
  if (!op || !l || !r || !out)
    return false;
  if (strcmp(op, "+") == 0 && l->kind == NY_CT_FAST_STR &&
      r->kind == NY_CT_FAST_STR) {
    if ((l->len && !l->s) || (r->len && !r->s) || l->len > SIZE_MAX - r->len ||
        l->len + r->len == SIZE_MAX)
      return false;
    size_t len = l->len + r->len;
    char *joined = malloc(len + 1);
    if (!joined)
      return false;
    if (l->len)
      memcpy(joined, l->s, l->len);
    if (r->len)
      memcpy(joined + l->len, r->s, r->len);
    joined[len] = '\0';
    *out = (ny_ct_fast_val_t){
        .kind = NY_CT_FAST_STR, .s = joined, .len = len, .owns_s = true};
    return true;
  }
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    bool eq = false;
    if (!ny_ct_fast_values_equal(l, r, &eq, 0))
      return false;
    out->kind = NY_CT_FAST_BOOL;
    out->b = (strcmp(op, "==") == 0) ? eq : !eq;
    return true;
  }
  if ((l->kind == NY_CT_FAST_FLOAT || r->kind == NY_CT_FAST_FLOAT) &&
      (l->kind == NY_CT_FAST_FLOAT || l->kind == NY_CT_FAST_INT) &&
      (r->kind == NY_CT_FAST_FLOAT || r->kind == NY_CT_FAST_INT)) {
    double ld = l->kind == NY_CT_FAST_FLOAT ? l->f64 : (double)l->i;
    double rd = r->kind == NY_CT_FAST_FLOAT ? r->f64 : (double)r->i;
    switch (op[0]) {
    case '+':
      if (!op[1]) {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = ld + rd;
        return true;
      }
      break;
    case '-':
      if (!op[1]) {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = ld - rd;
        return true;
      }
      break;
    case '*':
      if (op[1] == '*') {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = pow(ld, rd);
        return true;
      } else {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = ld * rd;
        return true;
      }
      break;
    case '/':
      if (!op[1]) {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = ld / rd;
        return true;
      }
      break;
    case '%':
      if (!op[1]) {
        out->kind = NY_CT_FAST_FLOAT;
        out->f64 = fmod(ld, rd);
        return true;
      }
      break;
    case '<':
      if (op[1] == '=')
        out->b = ld <= rd;
      else if (op[1] == '<')
        break;
      else
        out->b = ld < rd;
      if (op[1] == '=' || !op[1]) {
        out->kind = NY_CT_FAST_BOOL;
        return true;
      }
      break;
    case '>':
      if (op[1] == '=')
        out->b = ld >= rd;
      else if (op[1] == '>')
        break;
      else
        out->b = ld > rd;
      if (op[1] == '=' || !op[1]) {
        out->kind = NY_CT_FAST_BOOL;
        return true;
      }
      break;
    }
    return false;
  }
  if (l->kind == NY_CT_FAST_INT && r->kind == NY_CT_FAST_INT)
    return ny_ct_fast_int_op(op, l->i, r->i, out);
  return false;
}

static bool ny_ct_range_len_raw(int64_t start, int64_t stop, int64_t step,
                                int64_t *out) {
  if (!out || step == 0)
    return false;
  *out = 0;
  if (step > 0) {
    if (start >= stop)
      return true;
    int64_t span = 0;
    if (__builtin_sub_overflow(stop, start, &span) || span <= 0)
      return false;
    *out = ((span - 1) / step) + 1;
    return true;
  }
  if (start <= stop)
    return true;
  int64_t span = 0;
  if (__builtin_sub_overflow(start, stop, &span) || span <= 0)
    return false;
  *out = ((span - 1) / -step) + 1;
  return true;
}

bool ny_ct_make_range(int64_t start, int64_t stop, int64_t step,
                      ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  if (step == 0)
    step = 1;
  int64_t len = 0;
  if (!ny_ct_range_len_raw(start, stop, step, &len) || len < 0 || len > 65536)
    return false;
  out->kind = NY_CT_FAST_RANGE;
  out->range_start = start;
  out->range_stop = stop;
  out->range_step = step;
  out->len = (size_t)len;
  out->items = NULL;
  return true;
}

bool ny_ct_seq_len(const ny_ct_fast_val_t *v, int64_t *out) {
  if (!v || !out)
    return false;
  if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE ||
      v->kind == NY_CT_FAST_DICT || v->kind == NY_CT_FAST_RANGE) {
    if (v->len > (size_t)INT64_MAX)
      return false;
    *out = (int64_t)v->len;
    return true;
  }
  return false;
}

bool ny_ct_seq_item(const ny_ct_fast_val_t *v, int64_t idx,
                    ny_ct_fast_val_t *out) {
  if (!v || !out || idx < 0)
    return false;
  int64_t len = 0;
  if (!ny_ct_seq_len(v, &len) || idx >= len)
    return false;
  if (v->kind == NY_CT_FAST_DICT) {
    if (!v->items)
      return false;
    return ny_ct_fast_val_clone(&v->items[(size_t)idx * 2], out);
  }
  if (v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE) {
    if (!v->items)
      return false;
    return ny_ct_fast_val_clone(&v->items[idx], out);
  }
  if (v->kind == NY_CT_FAST_RANGE) {
    int64_t scaled = 0, item = 0;
    if (__builtin_mul_overflow(idx, v->range_step, &scaled) ||
        __builtin_add_overflow(v->range_start, scaled, &item) ||
        !ny_small_int_fits_i64(item))
      return false;
    out->kind = NY_CT_FAST_INT;
    out->i = item;
    return true;
  }
  return false;
}

bool ny_ct_make_seq(ny_ct_fast_kind_t kind, size_t len, ny_ct_fast_val_t *out) {
  if (!out ||
      (kind != NY_CT_FAST_LIST && kind != NY_CT_FAST_TUPLE &&
       kind != NY_CT_FAST_DICT) ||
      len > 65536 || (kind == NY_CT_FAST_DICT && len > 32768))
    return false;
  size_t item_count = kind == NY_CT_FAST_DICT ? len * 2 : len;
  ny_ct_fast_val_t *items = NULL;
  if (item_count > 0) {
    items = calloc(item_count, sizeof(*items));
    if (!items)
      return false;
  }
  out->kind = kind;
  out->len = len;
  out->items = items;
  return true;
}

typedef struct ny_ct_ptr_seen_t {
  void **items;
  size_t len;
  size_t cap;
} ny_ct_ptr_seen_t;

static bool ny_ct_seen_add(ny_ct_ptr_seen_t *seen, const void *ptr) {
  if (!seen || !ptr)
    return false;
  for (size_t i = 0; i < seen->len; i++) {
    if (seen->items[i] == ptr)
      return false;
  }
  if (seen->len == seen->cap) {
    size_t next_cap = seen->cap ? seen->cap * 2 : 16;
    void **grown = realloc(seen->items, next_cap * sizeof(*seen->items));
    if (!grown)
      return false;
    seen->items = grown;
    seen->cap = next_cap;
  }
  seen->items[seen->len++] = (void *)ptr;
  return true;
}

static void ny_ct_fast_val_free_seen(ny_ct_fast_val_t *v,
                                     ny_ct_ptr_seen_t *seen) {
  if (!v)
    return;
  if (v->kind == NY_CT_FAST_BIGINT && v->s && ny_ct_seen_add(seen, v->s))
    free((void *)v->s);
  if (v->kind == NY_CT_FAST_STR && v->owns_s && v->s &&
      ny_ct_seen_add(seen, v->s))
    free((void *)v->s);
  if ((v->kind == NY_CT_FAST_LIST || v->kind == NY_CT_FAST_TUPLE ||
       v->kind == NY_CT_FAST_DICT) &&
      v->items) {
    ny_ct_fast_val_t *items = v->items;
    size_t len = v->kind == NY_CT_FAST_DICT ? v->len * 2 : v->len;
    if (ny_ct_seen_add(seen, items)) {
      for (size_t i = 0; i < len; i++)
        ny_ct_fast_val_free_seen(&items[i], seen);
      free(items);
    }
  }
  *v = ny_ct_fast_none();
}

void ny_ct_fast_val_free(ny_ct_fast_val_t *v) {
  ny_ct_ptr_seen_t seen = {0};
  ny_ct_fast_val_free_seen(v, &seen);
  free(seen.items);
}

void ny_ct_fast_val_move(ny_ct_fast_val_t *dst, ny_ct_fast_val_t *src) {
  if (!dst || !src || dst == src)
    return;
  ny_ct_fast_val_free(dst);
  *dst = *src;
  *src = ny_ct_fast_none();
}

bool ny_ct_fast_val_clone(const ny_ct_fast_val_t *src, ny_ct_fast_val_t *dst) {
  if (!src || !dst)
    return false;
  ny_ct_fast_val_t out = *src;
  out.s = src->s;
  out.items = NULL;
  if (src->kind == NY_CT_FAST_BIGINT) {
    out.s = ny_strdup(src->s ? src->s : "0");
    if (!out.s)
      return false;
  } else if (src->kind == NY_CT_FAST_STR && src->owns_s) {
    if (src->len == SIZE_MAX)
      return false;
    char *copy = malloc(src->len + 1);
    if (!copy)
      return false;
    if (src->len)
      memcpy(copy, src->s, src->len);
    copy[src->len] = '\0';
    out.s = copy;
    out.owns_s = true;
  } else if (src->kind == NY_CT_FAST_LIST || src->kind == NY_CT_FAST_TUPLE ||
             src->kind == NY_CT_FAST_DICT) {
    if (src->len > 0 && !src->items)
      return false;
    if (src->kind == NY_CT_FAST_DICT) {
      if (src->len > SIZE_MAX / 2)
        return false;
      out.items = calloc(src->len * 2, sizeof(*out.items));
      if (src->len && !out.items)
        return false;
    } else if (!ny_ct_make_seq(src->kind, src->len, &out)) {
      return false;
    }
    size_t item_count = src->kind == NY_CT_FAST_DICT ? src->len * 2 : src->len;
    for (size_t i = 0; i < item_count; i++) {
      if (!ny_ct_fast_val_clone(&src->items[i], &out.items[i])) {
        ny_ct_fast_val_free(&out);
        return false;
      }
    }
  }
  *dst = out;
  return true;
}

bool ny_comptime_main_enabled(codegen_t *cg, token_t tok) {
  bool has_source = cg && cg->source_main_file && *cg->source_main_file;
  bool has_token_file = tok.filename && *tok.filename;
  if (has_source) {
    /*
     * Imported sources can carry synthetic tokens without a filename.  They
     * are still imported sources; the test-mode fallback is only meaningful
     * when the compilation itself has no source-file identity.
     */
    if (!has_token_file)
      return false;
    return ny_codegen_token_is_source_file(cg, tok);
  }
  return ny_env_enabled("NYTRIX_TEST_MODE");
}

static expr_t *ny_binding_var_init_expr_any(binding *b, const char *name) {
  if (!b || !name || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *n = var->names.data[i];
    if (n && strcmp(n, name) == 0)
      return var->exprs.data[i];
  }
  return NULL;
}

bool ny_try_eval_binding_comptime_const(codegen_t *cg, binding *b,
                                        const char *name, ny_ct_fast_val_t *out,
                                        int depth) {
  if (!b || b->is_mut || !name || !out || depth > 64)
    return false;

  expr_t *init = ny_binding_var_init_expr(b, name);
  if (init)
    return ny_try_eval_comptime_expr_fast(cg, init, out, depth + 1);

  init = ny_binding_var_init_expr_any(b, name);
  if (init && init->kind == NY_E_COMPTIME && init->as.comptime_expr.body)
    return ny_try_eval_comptime_fast_value(cg, init->as.comptime_expr.body,
                                           out);
  return false;
}

bool ny_ct_fast_eval_bigint_constructor_arg(const ny_ct_fast_val_t *arg,
                                            ny_ct_fast_val_t *out) {
  if (!arg || !out)
    return false;
  char *dec = NULL;
  if (arg->kind == NY_CT_FAST_INT) {
    dec = ny_ct_i64_to_dec(arg->i);
  } else if (arg->kind == NY_CT_FAST_STR || arg->kind == NY_CT_FAST_BIGINT) {
    dec = ny_ct_canonical_decimal(arg->s ? arg->s : "0");
  } else {
    return false;
  }
  if (!dec)
    return false;
  out->kind = NY_CT_FAST_BIGINT;
  out->s = dec;
  return true;
}

static bool ny_ct_fast_push_byte(int64_t *acc, int64_t byte) {
  if (!acc)
    return false;
  int64_t b = (int64_t)((uint64_t)byte & 255ULL);
  if (*acc > (INT64_MAX - b) / 256)
    return false;
  int64_t next = (*acc * 256) + b;
  if (!ny_small_int_fits_i64(next))
    return false;
  *acc = next;
  return true;
}

static char *ny_ct_dec_mul_add_byte(const char *dec, int64_t byte) {
  const char *src = (dec && *dec) ? dec : "0";
  size_t len = strlen(src);
  size_t cap = len + 8;
  char *buf = malloc(cap);
  if (!buf)
    return NULL;
  size_t pos = cap - 1;
  buf[pos] = '\0';
  int carry = (int)((uint64_t)byte & 255ULL);
  for (ssize_t i = (ssize_t)len - 1; i >= 0; --i) {
    char c = src[i];
    if (c < '0' || c > '9') {
      free(buf);
      return NULL;
    }
    int v = (c - '0') * 256 + carry;
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (v % 10));
    carry = v / 10;
  }
  while (carry > 0) {
    if (pos == 0) {
      free(buf);
      return NULL;
    }
    buf[--pos] = (char)('0' + (carry % 10));
    carry /= 10;
  }
  while (buf[pos] == '0' && buf[pos + 1] != '\0')
    pos++;
  char *out = ny_strdup(buf + pos);
  free(buf);
  return out;
}

static bool ny_ct_long_accum_byte(int64_t *small, bool *small_ok,
                                  char **big_dec, int64_t byte) {
  if (!small || !small_ok || !big_dec)
    return false;
  if (*small_ok) {
    int64_t prev = *small;
    if (ny_ct_fast_push_byte(small, byte))
      return true;
    *small_ok = false;
    *big_dec = ny_ct_i64_to_dec(prev);
    if (!*big_dec)
      return false;
  }
  char *next = ny_ct_dec_mul_add_byte(*big_dec, byte);
  if (!next)
    return false;
  free(*big_dec);
  *big_dec = next;
  return true;
}

static bool ny_ct_long_finish(int64_t small, bool small_ok, char *big_dec,
                              ny_ct_fast_val_t *out) {
  if (!out) {
    free(big_dec);
    return false;
  }
  if (small_ok) {
    free(big_dec);
    out->kind = NY_CT_FAST_INT;
    out->i = small;
    return true;
  }
  out->kind = NY_CT_FAST_BIGINT;
  if (!big_dec) {
    big_dec = ny_strdup("0");
    if (!big_dec)
      return false;
  }
  out->s = big_dec;
  return true;
}

bool ny_ct_fast_bytes_long(const char *data, size_t len,
                           ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  for (size_t i = 0; i < len; i++) {
    unsigned char byte = data ? (unsigned char)data[i] : 0;
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, (int64_t)byte)) {
      free(big_dec);
      return false;
    }
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

static int ny_ct_fast_hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'A' && c <= 'F')
    return (int)(c - 'A' + 10);
  if (c >= 'a' && c <= 'f')
    return (int)(c - 'a' + 10);
  return 0;
}

bool ny_ct_fast_unhex_long(const char *data, size_t len,
                           ny_ct_fast_val_t *out) {
  if (!out)
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  size_t i = 0;
  if ((len & 1u) != 0) {
    int lo = ny_ct_fast_hex_nibble(data ? (unsigned char)data[0] : 0);
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, lo)) {
      free(big_dec);
      return false;
    }
    i = 1;
  }
  while (i < len) {
    int hi = ny_ct_fast_hex_nibble(data ? (unsigned char)data[i] : 0);
    int lo = ny_ct_fast_hex_nibble(data ? (unsigned char)data[i + 1] : 0);
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, (hi << 4) | lo)) {
      free(big_dec);
      return false;
    }
    i += 2;
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

static bool ny_ct_fast_literal_str_long(expr_t *e, ny_ct_fast_val_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  return ny_ct_fast_bytes_long(e->as.literal.as.s.data, e->as.literal.as.s.len,
                               out);
}

static bool ny_ct_fast_literal_unhex_long(expr_t *e, ny_ct_fast_val_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_STR)
    return false;
  return ny_ct_fast_unhex_long(e->as.literal.as.s.data, e->as.literal.as.s.len,
                               out);
}

static bool ny_ct_fast_list_long(codegen_t *cg, expr_t *e,
                                 ny_ct_fast_val_t *out, int depth) {
  if (!e || !out || (e->kind != NY_E_LIST && e->kind != NY_E_TUPLE))
    return false;
  int64_t small = 0;
  bool small_ok = true;
  char *big_dec = NULL;
  for (size_t i = 0; i < e->as.list_like.len; i++) {
    ny_ct_fast_val_t item = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.list_like.data[i], &item,
                                        depth + 1) ||
        item.kind != NY_CT_FAST_INT) {
      free(big_dec);
      ny_ct_fast_val_free(&item);
      return false;
    }
    if (!ny_ct_long_accum_byte(&small, &small_ok, &big_dec, item.i)) {
      free(big_dec);
      ny_ct_fast_val_free(&item);
      return false;
    }
    ny_ct_fast_val_free(&item);
  }
  return ny_ct_long_finish(small, small_ok, big_dec, out);
}

bool ny_ct_fast_eval_long_property(codegen_t *cg, expr_t *target,
                                   ny_ct_fast_val_t *out, int depth) {
  if (!target || !out || depth > 64)
    return false;
  if (target->kind == NY_E_LITERAL) {
    if (target->as.literal.kind == NY_LIT_INT && target->tok.kind != NY_T_NIL) {
      if (target->as.literal.integer_outside_i64)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = target->as.literal.as.i;
      return true;
    }
    if (target->as.literal.kind == NY_LIT_STR)
      return ny_ct_fast_literal_str_long(target, out);
  }
  if (target->kind == NY_E_LIST || target->kind == NY_E_TUPLE)
    return ny_ct_fast_list_long(cg, target, out, depth + 1);
  if (target->kind == NY_E_MEMBER && target->as.member.name) {
    if (strcmp(target->as.member.name, "unhex") == 0)
      return ny_ct_fast_literal_unhex_long(target->as.member.target, out);
    if (strcmp(target->as.member.name, "to_bytes") == 0)
      return ny_ct_fast_eval_long_property(cg, target->as.member.target, out,
                                           depth + 1);
  }
  ny_ct_fast_val_t v = {0};
  if (!ny_try_eval_comptime_expr_fast(cg, target, &v, depth + 1))
    return false;
  if (v.kind == NY_CT_FAST_INT || v.kind == NY_CT_FAST_BIGINT) {
    ny_ct_fast_val_move(out, &v);
    return true;
  }
  if (v.kind == NY_CT_FAST_STR) {
    bool ok = ny_ct_fast_bytes_long(v.s, v.s ? strlen(v.s) : 0, out);
    ny_ct_fast_val_free(&v);
    return ok;
  }
  ny_ct_fast_val_free(&v);
  return false;
}

bool ny_ct_fast_to_tagged(const ny_ct_fast_val_t *v, int64_t *out_tagged) {
  if (!v || !out_tagged)
    return false;
  if (v->kind == NY_CT_FAST_BOOL) {
    *out_tagged = v->b ? NY_IMM_TRUE : NY_IMM_FALSE;
    return true;
  }
  if (v->kind == NY_CT_FAST_INT) {
    if (!ny_small_int_fits_i64(v->i))
      return false;
    *out_tagged = (int64_t)((((uint64_t)v->i) << 1) | 1u);
    return true;
  }
  if (v->kind == NY_CT_FAST_NONE) {
    *out_tagged = NY_IMM_NIL;
    return true;
  }
  return false;
}

bool ny_ct_platform_ident(const char *name, ny_ct_fast_val_t *out) {
  if (!name || !out)
    return false;
  const char *os = ny_host_os_name();
  const char *arch = ny_host_arch_name();
  bool is_windows = strcmp(os, "windows") == 0;
  bool is_macos = strcmp(os, "macos") == 0;
  bool is_linux = strcmp(os, "linux") == 0;
  bool is_unix = !is_windows && strcmp(os, "unknown") != 0;
  bool is_x86_64 = strcmp(arch, "x86_64") == 0;
  bool is_x86 = strcmp(arch, "x86") == 0 || is_x86_64;
  bool is_aarch64 = strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0;
  bool is_arm = strcmp(arch, "arm") == 0 || is_aarch64;
  bool is_riscv = strcmp(arch, "riscv") == 0;

  if (strcmp(name, "OS") == 0) {
    out->kind = NY_CT_FAST_STR;
    out->s = os;
    out->len = strlen(os);
    return true;
  }
  if (strcmp(name, "ARCH") == 0) {
    out->kind = NY_CT_FAST_STR;
    out->s = arch;
    out->len = strlen(arch);
    return true;
  }

#define NY_CT_PLATFORM_BOOL(symbol, value)                                     \
  if (strcmp(name, symbol) == 0) {                                             \
    out->kind = NY_CT_FAST_BOOL;                                               \
    out->b = (value);                                                          \
    return true;                                                               \
  }

  NY_CT_PLATFORM_BOOL("linux", is_linux)
  NY_CT_PLATFORM_BOOL("LINUX", is_linux)
  NY_CT_PLATFORM_BOOL("IS_LINUX", is_linux)
  NY_CT_PLATFORM_BOOL("macos", is_macos)
  NY_CT_PLATFORM_BOOL("mac", is_macos)
  NY_CT_PLATFORM_BOOL("MACOS", is_macos)
  NY_CT_PLATFORM_BOOL("IS_MACOS", is_macos)
  NY_CT_PLATFORM_BOOL("windows", is_windows)
  NY_CT_PLATFORM_BOOL("IS_WINDOWS", is_windows)
  NY_CT_PLATFORM_BOOL("unix", is_unix)
  NY_CT_PLATFORM_BOOL("posix", is_unix)
  NY_CT_PLATFORM_BOOL("UNIX", is_unix)
  NY_CT_PLATFORM_BOOL("IS_UNIX", is_unix)
  NY_CT_PLATFORM_BOOL("x86_64", is_x86_64)
  NY_CT_PLATFORM_BOOL("x64", is_x86_64)
  NY_CT_PLATFORM_BOOL("IS_X86_64", is_x86_64)
  NY_CT_PLATFORM_BOOL("x86", is_x86)
  NY_CT_PLATFORM_BOOL("IS_X86", is_x86)
  NY_CT_PLATFORM_BOOL("aarch64", is_aarch64)
  NY_CT_PLATFORM_BOOL("arm64", is_aarch64)
  NY_CT_PLATFORM_BOOL("IS_AARCH64", is_aarch64)
  NY_CT_PLATFORM_BOOL("arm", is_arm)
  NY_CT_PLATFORM_BOOL("IS_ARM", is_arm)
  NY_CT_PLATFORM_BOOL("riscv", is_riscv)
  NY_CT_PLATFORM_BOOL("IS_RISCV", is_riscv)

#undef NY_CT_PLATFORM_BOOL
  return false;
}

const char *ny_ct_fast_callee_leaf_name(expr_t *callee) {
  expr_t *cur = callee;
  int guard = 0;
  while (cur && guard++ < 16) {
    if (cur->kind == NY_E_IDENT)
      return cur->as.ident.name;
    if (cur->kind == NY_E_MEMBER) {
      if (cur->as.member.name && *cur->as.member.name)
        return cur->as.member.name;
      cur = cur->as.member.target;
      continue;
    }
    break;
  }
  return NULL;
}

/*
 * A user-defined global function with a builtin's name is evaluated by the
 * bounded interpreter (which prefers user code) instead of the fast
 * intrinsic path.
 */
static bool ny_ct_fast_call_shadowed(codegen_t *cg, const char *name) {
  if (!cg || !name)
    return false;
  binding *b = lookup_global(cg, name);
  return b && b->stmt_t && b->stmt_t->kind == NY_S_FUNC &&
         !ny_is_stdlib_tok(b->stmt_t->tok);
}

static bool ny_ct_eval_comptime_expr_fast_uncached(codegen_t *cg, expr_t *e,
                                                   ny_ct_fast_val_t *out,
                                                   int depth);

/*
 * Resolve the binding that a closed NAME reference denotes, for memo keying.
 * Platform identifiers are represented directly by the codegen target and
 * therefore have no ordinary global binding. Immutable globals use their
 * binding identity so a redefinition yields a distinct key.
 */
static const void *ny_ct_memo_ident_binding(codegen_t *cg, const expr_t *e) {
  if (!cg || !e || e->kind != NY_E_IDENT || !e->as.ident.name)
    return NULL;
  if (ny_ct_platform_ident(e->as.ident.name, &(ny_ct_fast_val_t){0}))
    return NULL;
  binding *b = lookup_global(cg, e->as.ident.name);
  if (!b || b->is_mut)
    return NULL;
  return (const void *)b;
}

/*
 * Return the stable declaration identity only when the shared typing stage
 * proved that the function is pure. The bounded comptime interpreter handles
 * its body; unresolved, variadic, impure, and shadowed intrinsic calls stay
 * outside the expression memo.
 */
static const stmt_t *ny_ct_memo_pure_function(codegen_t *cg,
                                              const expr_t *call) {
  if (!cg || !call || call->kind != NY_E_CALL || !call->as.call.callee ||
      call->as.call.callee->kind != NY_E_IDENT ||
      !call->as.call.callee->as.ident.name)
    return NULL;
  const char *name = call->as.call.callee->as.ident.name;
  binding *b = lookup_global(cg, name);
  if (!b || !b->stmt_t || b->stmt_t->kind != NY_S_FUNC)
    return NULL;
  fun_sig *sig = lookup_fun_exact(cg, name);
  if (!sig || sig->stmt_t != b->stmt_t || !sig->is_pure || sig->is_variadic ||
      !b->stmt_t->as.fn.body ||
      call->as.call.args.len > b->stmt_t->as.fn.params.len)
    return NULL;
  return b->stmt_t;
}

/*
 * Memo keys combine expression structure, resolved global bindings, and the
 * owning codegen context. The latter is stable for one compilation and owns
 * target/platform facts and comptime options. The memo admits:
 *   - literals, unary/binary/logical/ternary operators, list, tuple, dict,
 *     and index/slice forms over admitted operands;
 *   - NAME references resolving to an immutable global whose initializer is
 *     itself memo-closed or a constant comptime body (the binding pointer
 *     joins the key; platform idents stay out);
 *   - plain calls to deterministic value intrinsics (len, range, range2, Z)
 *     over closed args;
 *   - member `.get(key[, default])` calls on closed receivers (dictionary,
 *     string, list, tuple, range), in both the parsed member-call form and
 *     the legacy member-callee call form; evaluation cannot invoke user code.
 * Stateful random calls, user-code calls/member methods, computed member
 * names, and named arguments stay excluded.
 */
static bool ny_ct_expr_memo_closed(codegen_t *cg, const expr_t *e, int depth) {
  if (!e || depth > 64)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    return e->as.literal.kind == NY_LIT_INT ||
           e->as.literal.kind == NY_LIT_FLOAT ||
           e->as.literal.kind == NY_LIT_BOOL ||
           e->as.literal.kind == NY_LIT_STR || e->tok.kind == NY_T_NIL;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    ny_ct_fast_val_t probe = ny_ct_fast_none();
    if (!name || !cg)
      return false;
    if (ny_ct_platform_ident(name, &probe))
      return true;
    binding *b = lookup_global(cg, name);
    if (!b || b->is_mut)
      return false;
    expr_t *init = ny_binding_var_init_expr(b, name);
    if (init) {
      if (init->kind == NY_E_COMPTIME)
        return init->as.comptime_expr.body != NULL;
      return ny_ct_expr_memo_closed(cg, init, depth + 1);
    }
    init = ny_binding_var_init_expr_any(b, name);
    return init && init->kind == NY_E_COMPTIME &&
           init->as.comptime_expr.body != NULL;
  }
  case NY_E_UNARY:
    return ny_ct_expr_memo_closed(cg, e->as.unary.right, depth + 1);
  case NY_E_BINARY:
    return ny_ct_expr_memo_closed(cg, e->as.binary.left, depth + 1) &&
           ny_ct_expr_memo_closed(cg, e->as.binary.right, depth + 1);
  case NY_E_LOGICAL:
    return ny_ct_expr_memo_closed(cg, e->as.logical.left, depth + 1) &&
           ny_ct_expr_memo_closed(cg, e->as.logical.right, depth + 1);
  case NY_E_TERNARY:
    return ny_ct_expr_memo_closed(cg, e->as.ternary.cond, depth + 1) &&
           ny_ct_expr_memo_closed(cg, e->as.ternary.true_expr, depth + 1) &&
           ny_ct_expr_memo_closed(cg, e->as.ternary.false_expr, depth + 1);
  case NY_E_LIST:
  case NY_E_TUPLE:
    for (size_t i = 0; i < e->as.list_like.len; ++i)
      if (!ny_ct_expr_memo_closed(cg, e->as.list_like.data[i], depth + 1))
        return false;
    return true;
  case NY_E_INDEX:
    return e->as.index.target &&
           ny_ct_expr_memo_closed(cg, e->as.index.target, depth + 1) &&
           (!e->as.index.start ||
            ny_ct_expr_memo_closed(cg, e->as.index.start, depth + 1)) &&
           (!e->as.index.stop ||
            ny_ct_expr_memo_closed(cg, e->as.index.stop, depth + 1)) &&
           (!e->as.index.step ||
            ny_ct_expr_memo_closed(cg, e->as.index.step, depth + 1));
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i)
      if (!ny_ct_expr_memo_closed(cg, e->as.dict.pairs.data[i].key,
                                  depth + 1) ||
          !ny_ct_expr_memo_closed(cg, e->as.dict.pairs.data[i].value,
                                  depth + 1))
        return false;
    return true;
  case NY_E_CALL: {
    if (e->as.call.deferred_comptime_reflection || !e->as.call.callee)
      return false;
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      if (e->as.call.args.data[i].name || !e->as.call.args.data[i].val ||
          !ny_ct_expr_memo_closed(cg, e->as.call.args.data[i].val, depth + 1))
        return false;
    const expr_t *callee = e->as.call.callee;
    if (callee->kind == NY_E_IDENT) {
      const char *name = callee->as.ident.name;
      if (!name)
        return false;
      bool is_len = strcmp(name, "len") == 0;
      bool is_range = strcmp(name, "range") == 0;
      bool is_range2 = strcmp(name, "range2") == 0;
      bool is_bigint = strcmp(name, "Z") == 0;
      bool is_main = strcmp(name, "__main") == 0;
      bool is_os = strcmp(name, "os") == 0 || strcmp(name, "__os_name") == 0;
      bool is_arch =
          strcmp(name, "arch") == 0 || strcmp(name, "__arch_name") == 0;
      if (is_len && e->as.call.args.len == 1) {
        return !ny_ct_fast_call_shadowed(cg, name);
      }
      if (is_range && e->as.call.args.len >= 1 && e->as.call.args.len <= 3) {
        return !ny_ct_fast_call_shadowed(cg, name);
      }
      if ((is_range2 && e->as.call.args.len >= 2 && e->as.call.args.len <= 3) ||
          (is_bigint && e->as.call.args.len == 1)) {
        return !ny_ct_fast_call_shadowed(cg, name);
      }
      /*
       * Target queries and registered refinements are evaluated directly.
       * Their meaning depends on target/typing context, so keep them outside
       * this expression cache.
       */
      if (is_main || is_os || is_arch || ny_lookup_refine(cg, name))
        return false;
      if (!ny_ct_memo_pure_function(cg, e))
        return false;
      for (size_t i = 0; i < e->as.call.args.len; ++i)
        if (e->as.call.args.data[i].name)
          return false;
      return true;
    }
    if (callee->kind == NY_E_MEMBER) {
      if (callee->as.member.computed_name || !callee->as.member.name ||
          strcmp(callee->as.member.name, "get") != 0 ||
          (e->as.call.args.len != 1 && e->as.call.args.len != 2) ||
          !callee->as.member.target)
        return false;
      return ny_ct_expr_memo_closed(cg, callee->as.member.target, depth + 1);
    }
    return false;
  }
  case NY_E_MEMCALL:
    /*
     * The parsed form of `receiver.get(key[, default])`. Other member calls
     * (notably `map`) may run user code and stay excluded.
     */
    if (!e->as.memcall.name || strcmp(e->as.memcall.name, "get") != 0 ||
        (e->as.memcall.args.len != 1 && e->as.memcall.args.len != 2) ||
        !e->as.memcall.target)
      return false;
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      if (e->as.memcall.args.data[i].name || !e->as.memcall.args.data[i].val ||
          !ny_ct_expr_memo_closed(cg, e->as.memcall.args.data[i].val,
                                  depth + 1))
        return false;
    return ny_ct_expr_memo_closed(cg, e->as.memcall.target, depth + 1);
  case NY_E_MEMBER:
    return !e->as.member.computed_name && e->as.member.name &&
           strcmp(e->as.member.name, "long") == 0 && e->as.member.target &&
           ny_ct_expr_memo_closed(cg, e->as.member.target, depth + 1);
  default:
    return false;
  }
}

static uint64_t ny_ct_memo_hash_bytes(uint64_t hash, const void *data,
                                      size_t len) {
  const unsigned char *bytes = data;
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint64_t ny_ct_memo_hash_expr(codegen_t *cg, const expr_t *e) {
  uint64_t hash = ny_ct_memo_hash_bytes(UINT64_C(1469598103934665603), &e->kind,
                                        sizeof(e->kind));
  switch (e->kind) {
  case NY_E_IDENT: {
    if (!e->as.ident.name)
      return hash;
    const void *binding = ny_ct_memo_ident_binding(cg, e);
    hash =
        ny_ct_memo_hash_bytes(hash, e->as.ident.name, strlen(e->as.ident.name));
    return ny_ct_memo_hash_bytes(hash, &binding, sizeof(binding));
  }
  case NY_E_LITERAL: {
    const literal_t *literal = &e->as.literal;
    hash = ny_ct_memo_hash_bytes(hash, &literal->kind, sizeof(literal->kind));
    hash = ny_ct_memo_hash_bytes(hash, &literal->hint, sizeof(literal->hint));
    hash = ny_ct_memo_hash_bytes(hash, &literal->hint_explicit,
                                 sizeof(literal->hint_explicit));
    hash = ny_ct_memo_hash_bytes(hash, &e->tok.kind, sizeof(e->tok.kind));
    if (literal->kind == NY_LIT_INT) {
      hash = ny_ct_memo_hash_bytes(hash, &literal->integer_outside_i64,
                                   sizeof(literal->integer_outside_i64));
      hash = ny_ct_memo_hash_bytes(hash, &literal->as.i, sizeof(literal->as.i));
    } else if (literal->kind == NY_LIT_FLOAT) {
      hash = ny_ct_memo_hash_bytes(hash, &literal->as.f, sizeof(literal->as.f));
    } else if (literal->kind == NY_LIT_BOOL) {
      hash = ny_ct_memo_hash_bytes(hash, &literal->as.b, sizeof(literal->as.b));
    } else if (literal->kind == NY_LIT_STR) {
      hash = ny_ct_memo_hash_bytes(hash, &literal->as.s.len,
                                   sizeof(literal->as.s.len));
      hash = ny_ct_memo_hash_bytes(hash, literal->as.s.data, literal->as.s.len);
    }
    return hash;
  }
  case NY_E_UNARY:
    hash = ny_ct_memo_hash_bytes(hash, e->as.unary.op, strlen(e->as.unary.op));
    return ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.unary.right)},
        sizeof(uint64_t));
  case NY_E_BINARY:
    hash =
        ny_ct_memo_hash_bytes(hash, e->as.binary.op, strlen(e->as.binary.op));
    hash = ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.binary.left)},
        sizeof(uint64_t));
    return ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.binary.right)},
        sizeof(uint64_t));
  case NY_E_LOGICAL:
    hash =
        ny_ct_memo_hash_bytes(hash, e->as.logical.op, strlen(e->as.logical.op));
    hash = ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.logical.left)},
        sizeof(uint64_t));
    return ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.logical.right)},
        sizeof(uint64_t));
  case NY_E_TERNARY:
    hash = ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.ternary.cond)},
        sizeof(uint64_t));
    hash = ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.ternary.true_expr)},
        sizeof(uint64_t));
    return ny_ct_memo_hash_bytes(
        hash, &(uint64_t){ny_ct_memo_hash_expr(cg, e->as.ternary.false_expr)},
        sizeof(uint64_t));
  case NY_E_LIST:
  case NY_E_TUPLE:
    hash = ny_ct_memo_hash_bytes(hash, &e->as.list_like.len,
                                 sizeof(e->as.list_like.len));
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      uint64_t child = ny_ct_memo_hash_expr(cg, e->as.list_like.data[i]);
      hash = ny_ct_memo_hash_bytes(hash, &child, sizeof(child));
    }
    return hash;
  case NY_E_INDEX: {
    uint64_t target = ny_ct_memo_hash_expr(cg, e->as.index.target);
    hash = ny_ct_memo_hash_bytes(hash, &target, sizeof(target));
    const expr_t *parts[] = {e->as.index.start, e->as.index.stop,
                             e->as.index.step};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
      bool present = parts[i] != NULL;
      hash = ny_ct_memo_hash_bytes(hash, &present, sizeof(present));
      if (present) {
        uint64_t part = ny_ct_memo_hash_expr(cg, parts[i]);
        hash = ny_ct_memo_hash_bytes(hash, &part, sizeof(part));
      }
    }
    return hash;
  }
  case NY_E_DICT:
    hash = ny_ct_memo_hash_bytes(hash, &e->as.dict.pairs.len,
                                 sizeof(e->as.dict.pairs.len));
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      uint64_t key = ny_ct_memo_hash_expr(cg, e->as.dict.pairs.data[i].key);
      uint64_t value = ny_ct_memo_hash_expr(cg, e->as.dict.pairs.data[i].value);
      hash = ny_ct_memo_hash_bytes(hash, &key, sizeof(key));
      hash = ny_ct_memo_hash_bytes(hash, &value, sizeof(value));
    }
    return hash;
  case NY_E_CALL: {
    const expr_t *callee = e->as.call.callee;
    const char *name = NULL;
    uint8_t shape = 0; /* 0 = plain builtin callee, 1 = member method call */
    if (callee && callee->kind == NY_E_IDENT && callee->as.ident.name) {
      name = callee->as.ident.name;
    } else if (callee && callee->kind == NY_E_MEMBER &&
               !callee->as.member.computed_name && callee->as.member.name) {
      name = callee->as.member.name;
      shape = 1;
    }
    if (!name)
      return hash;
    /*
     * The callee/method name precedes the argument structure so distinct
     * builtins and methods never share a hash prefix.
     */
    hash = ny_ct_memo_hash_bytes(hash, &shape, sizeof(shape));
    hash = ny_ct_memo_hash_bytes(hash, name, strlen(name));
    if (shape == 0 && strcmp(name, "__main") == 0) {
      bool has_filename = e->tok.filename && *e->tok.filename;
      hash = ny_ct_memo_hash_bytes(hash, &has_filename, sizeof(has_filename));
      if (has_filename)
        hash = ny_ct_memo_hash_bytes(hash, e->tok.filename,
                                     strlen(e->tok.filename));
    }
    if (shape == 0) {
      refine_def_t *ref = ny_lookup_refine(cg, name);
      hash = ny_ct_memo_hash_bytes(hash, &ref, sizeof(ref));
      const stmt_t *fn = ny_ct_memo_pure_function(cg, e);
      hash = ny_ct_memo_hash_bytes(hash, &fn, sizeof(fn));
    }
    if (shape == 1 && callee->as.member.target) {
      uint64_t target = ny_ct_memo_hash_expr(cg, callee->as.member.target);
      hash = ny_ct_memo_hash_bytes(hash, &target, sizeof(target));
    }
    hash = ny_ct_memo_hash_bytes(hash, &e->as.call.args.len,
                                 sizeof(e->as.call.args.len));
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      uint64_t arg = ny_ct_memo_hash_expr(cg, e->as.call.args.data[i].val);
      hash = ny_ct_memo_hash_bytes(hash, &arg, sizeof(arg));
    }
    return hash;
  }
  case NY_E_MEMCALL: {
    if (!e->as.memcall.name || !e->as.memcall.target)
      return hash;
    uint8_t shape = 2; /* receiver method call */
    hash = ny_ct_memo_hash_bytes(hash, &shape, sizeof(shape));
    hash = ny_ct_memo_hash_bytes(hash, e->as.memcall.name,
                                 strlen(e->as.memcall.name));
    uint64_t target = ny_ct_memo_hash_expr(cg, e->as.memcall.target);
    hash = ny_ct_memo_hash_bytes(hash, &target, sizeof(target));
    hash = ny_ct_memo_hash_bytes(hash, &e->as.memcall.args.len,
                                 sizeof(e->as.memcall.args.len));
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      uint64_t arg = ny_ct_memo_hash_expr(cg, e->as.memcall.args.data[i].val);
      hash = ny_ct_memo_hash_bytes(hash, &arg, sizeof(arg));
    }
    return hash;
  }
  case NY_E_MEMBER: {
    bool computed = e->as.member.computed_name;
    hash = ny_ct_memo_hash_bytes(hash, &computed, sizeof(computed));
    if (e->as.member.name)
      hash = ny_ct_memo_hash_bytes(hash, e->as.member.name,
                                   strlen(e->as.member.name));
    uint64_t target = ny_ct_memo_hash_expr(cg, e->as.member.target);
    return ny_ct_memo_hash_bytes(hash, &target, sizeof(target));
  }
  default:
    return hash;
  }
}

static bool ny_ct_memo_expr_equal(codegen_t *cg, const expr_t *a,
                                  const expr_t *b) {
  if (a == b)
    return true;
  if (!a || !b || a->kind != b->kind)
    return false;
  switch (a->kind) {
  case NY_E_IDENT:
    return a->as.ident.name && b->as.ident.name &&
           strcmp(a->as.ident.name, b->as.ident.name) == 0 &&
           ny_ct_memo_ident_binding(cg, a) == ny_ct_memo_ident_binding(cg, b);
  case NY_E_LITERAL: {
    const literal_t *x = &a->as.literal, *y = &b->as.literal;
    if (x->kind != y->kind || x->hint != y->hint ||
        x->hint_explicit != y->hint_explicit || a->tok.kind != b->tok.kind)
      return false;
    if (x->kind == NY_LIT_INT)
      return x->integer_outside_i64 == y->integer_outside_i64 &&
             x->as.i == y->as.i;
    if (x->kind == NY_LIT_FLOAT)
      return memcmp(&x->as.f, &y->as.f, sizeof(x->as.f)) == 0;
    if (x->kind == NY_LIT_BOOL)
      return x->as.b == y->as.b;
    if (x->kind == NY_LIT_STR)
      return x->as.s.len == y->as.s.len &&
             (x->as.s.len == 0 ||
              memcmp(x->as.s.data, y->as.s.data, x->as.s.len) == 0);
    return false;
  }
  case NY_E_UNARY:
    return strcmp(a->as.unary.op, b->as.unary.op) == 0 &&
           ny_ct_memo_expr_equal(cg, a->as.unary.right, b->as.unary.right);
  case NY_E_BINARY:
    return strcmp(a->as.binary.op, b->as.binary.op) == 0 &&
           ny_ct_memo_expr_equal(cg, a->as.binary.left, b->as.binary.left) &&
           ny_ct_memo_expr_equal(cg, a->as.binary.right, b->as.binary.right);
  case NY_E_LOGICAL:
    return strcmp(a->as.logical.op, b->as.logical.op) == 0 &&
           ny_ct_memo_expr_equal(cg, a->as.logical.left, b->as.logical.left) &&
           ny_ct_memo_expr_equal(cg, a->as.logical.right, b->as.logical.right);
  case NY_E_TERNARY:
    return ny_ct_memo_expr_equal(cg, a->as.ternary.cond, b->as.ternary.cond) &&
           ny_ct_memo_expr_equal(cg, a->as.ternary.true_expr,
                                 b->as.ternary.true_expr) &&
           ny_ct_memo_expr_equal(cg, a->as.ternary.false_expr,
                                 b->as.ternary.false_expr);
  case NY_E_LIST:
  case NY_E_TUPLE:
    if (a->as.list_like.len != b->as.list_like.len)
      return false;
    for (size_t i = 0; i < a->as.list_like.len; ++i)
      if (!ny_ct_memo_expr_equal(cg, a->as.list_like.data[i],
                                 b->as.list_like.data[i]))
        return false;
    return true;
  case NY_E_INDEX:
    return ny_ct_memo_expr_equal(cg, a->as.index.target, b->as.index.target) &&
           ny_ct_memo_expr_equal(cg, a->as.index.start, b->as.index.start) &&
           ny_ct_memo_expr_equal(cg, a->as.index.stop, b->as.index.stop) &&
           ny_ct_memo_expr_equal(cg, a->as.index.step, b->as.index.step);
  case NY_E_DICT:
    if (a->as.dict.pairs.len != b->as.dict.pairs.len)
      return false;
    for (size_t i = 0; i < a->as.dict.pairs.len; ++i)
      if (!ny_ct_memo_expr_equal(cg, a->as.dict.pairs.data[i].key,
                                 b->as.dict.pairs.data[i].key) ||
          !ny_ct_memo_expr_equal(cg, a->as.dict.pairs.data[i].value,
                                 b->as.dict.pairs.data[i].value))
        return false;
    return true;
  case NY_E_CALL: {
    const expr_t *ca = a->as.call.callee;
    const expr_t *cb = b->as.call.callee;
    if (!ca || !cb || ca->kind != cb->kind ||
        a->as.call.args.len != b->as.call.args.len)
      return false;
    if (ca->kind == NY_E_IDENT) {
      if (!ca->as.ident.name || !cb->as.ident.name ||
          strcmp(ca->as.ident.name, cb->as.ident.name) != 0)
        return false;
      if (strcmp(ca->as.ident.name, "__main") == 0 &&
          ((!a->tok.filename && b->tok.filename) ||
           (a->tok.filename && !b->tok.filename) ||
           (a->tok.filename && b->tok.filename &&
            strcmp(a->tok.filename, b->tok.filename) != 0)))
        return false;
      if (ny_lookup_refine(cg, ca->as.ident.name) !=
          ny_lookup_refine(cg, cb->as.ident.name))
        return false;
      if (ny_ct_memo_pure_function(cg, a) != ny_ct_memo_pure_function(cg, b))
        return false;
    } else if (ca->kind == NY_E_MEMBER) {
      if (ca->as.member.computed_name || cb->as.member.computed_name ||
          !ca->as.member.name || !cb->as.member.name ||
          strcmp(ca->as.member.name, cb->as.member.name) != 0 ||
          !ny_ct_memo_expr_equal(cg, ca->as.member.target,
                                 cb->as.member.target))
        return false;
    } else {
      return false;
    }
    for (size_t i = 0; i < a->as.call.args.len; ++i)
      if (!ny_ct_memo_expr_equal(cg, a->as.call.args.data[i].val,
                                 b->as.call.args.data[i].val))
        return false;
    return true;
  }
  case NY_E_MEMCALL: {
    if (!a->as.memcall.name || !b->as.memcall.name ||
        strcmp(a->as.memcall.name, b->as.memcall.name) != 0 ||
        a->as.memcall.args.len != b->as.memcall.args.len ||
        !ny_ct_memo_expr_equal(cg, a->as.memcall.target, b->as.memcall.target))
      return false;
    for (size_t i = 0; i < a->as.memcall.args.len; ++i)
      if (!ny_ct_memo_expr_equal(cg, a->as.memcall.args.data[i].val,
                                 b->as.memcall.args.data[i].val))
        return false;
    return true;
  }
  case NY_E_MEMBER:
    return a->as.member.computed_name == b->as.member.computed_name &&
           ((!a->as.member.name && !b->as.member.name) ||
            (a->as.member.name && b->as.member.name &&
             strcmp(a->as.member.name, b->as.member.name) == 0)) &&
           ny_ct_memo_expr_equal(cg, a->as.member.target, b->as.member.target);
  default:
    return false;
  }
}

/*
 * Match the language string index contract: count UTF-8 scalar values, not
 * bytes, and reject malformed encodings so the ordinary evaluator retains
 * responsibility for its diagnostics.
 */
static size_t ny_ct_utf8_width(const unsigned char *s, size_t remaining) {
  if (!s || !remaining)
    return 0;
  unsigned char a = s[0];
  if (a <= 0x7f)
    return 1;
  if (a >= 0xc2 && a <= 0xdf)
    return remaining >= 2 && (s[1] & 0xc0) == 0x80 ? 2 : 0;
  if (remaining >= 3 && a == 0xe0)
    return s[1] >= 0xa0 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 3 && a >= 0xe1 && a <= 0xec)
    return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 3 && a == 0xed)
    return s[1] >= 0x80 && s[1] <= 0x9f && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 3 && a >= 0xee && a <= 0xef)
    return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 ? 3 : 0;
  if (remaining >= 4 && a == 0xf0)
    return s[1] >= 0x90 && s[1] <= 0xbf && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  if (remaining >= 4 && a >= 0xf1 && a <= 0xf3)
    return (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  if (remaining >= 4 && a == 0xf4)
    return s[1] >= 0x80 && s[1] <= 0x8f && (s[2] & 0xc0) == 0x80 &&
                   (s[3] & 0xc0) == 0x80
               ? 4
               : 0;
  return 0;
}

/*
 * Number of UTF-8 scalar values in a closed string value; rejects malformed
 * encodings so the ordinary evaluator retains its diagnostics.
 */
bool ny_ct_fast_string_scalar_len(const ny_ct_fast_val_t *string,
                                  int64_t *out) {
  if (!string || !out || string->kind != NY_CT_FAST_STR ||
      (string->len && !string->s))
    return false;
  int64_t count = 0;
  for (size_t offset = 0; offset < string->len;) {
    size_t width = ny_ct_utf8_width((const unsigned char *)string->s + offset,
                                    string->len - offset);
    if (!width || count == INT64_MAX)
      return false;
    offset += width;
    ++count;
  }
  *out = count;
  return true;
}

bool ny_ct_fast_string_index(const ny_ct_fast_val_t *string, int64_t index,
                             ny_ct_fast_val_t *out) {
  int64_t count = 0;
  if (!string || !out || !ny_ct_fast_string_scalar_len(string, &count))
    return false;
  int64_t resolved = index;
  if (resolved < 0)
    resolved += count;
  if (resolved < 0 || resolved >= count)
    return false;
  size_t offset = 0;
  for (int64_t i = 0; i < resolved; ++i)
    offset += ny_ct_utf8_width((const unsigned char *)string->s + offset,
                               string->len - offset);
  size_t width = ny_ct_utf8_width((const unsigned char *)string->s + offset,
                                  string->len - offset);
  char *character = malloc(width + 1);
  if (!character)
    return false;
  memcpy(character, string->s + offset, width);
  character[width] = '\0';
  *out = (ny_ct_fast_val_t){
      .kind = NY_CT_FAST_STR, .s = character, .len = width, .owns_s = true};
  return true;
}

bool ny_ct_fast_slice_sequence(const ny_ct_fast_val_t *target,
                               const ny_ct_fast_val_t *start_value,
                               const ny_ct_fast_val_t *stop_value,
                               const ny_ct_fast_val_t *step_value,
                               bool stop_is_omitted, ny_ct_fast_val_t *out) {
  if (!target || !out ||
      (target->kind != NY_CT_FAST_LIST && target->kind != NY_CT_FAST_TUPLE) ||
      (target->len && !target->items) || target->len > 65536)
    return false;
  int64_t len = (int64_t)target->len;
  int64_t step = 1;
  if (step_value) {
    if (step_value->kind != NY_CT_FAST_INT)
      return false;
    step = step_value->i;
  }
  if (step == 0)
    return false;
  bool has_start = start_value != NULL;
  bool has_stop = stop_value != NULL && !stop_is_omitted;
  if ((has_start && start_value->kind != NY_CT_FAST_INT) ||
      (has_stop && stop_value->kind != NY_CT_FAST_INT))
    return false;

  int64_t start = step > 0 ? 0 : len - 1;
  int64_t stop = step > 0 ? len : -1;
  if (has_start) {
    start = start_value->i;
    if (start < 0)
      start += len;
    if (step > 0) {
      if (start < 0)
        start = 0;
      if (start > len)
        start = len;
    } else {
      if (start < 0)
        start = -1;
      if (start >= len)
        start = len - 1;
    }
  }
  if (has_stop) {
    stop = stop_value->i;
    if (stop < 0)
      stop += len;
    if (step > 0) {
      if (stop < 0)
        stop = 0;
      if (stop > len)
        stop = len;
    } else {
      if (stop < 0)
        stop = -1;
      if (stop >= len)
        stop = len - 1;
    }
  }

  size_t count = 0;
  for (int64_t at = start; step > 0 ? at < stop : at > stop;) {
    if (count == 65536)
      return false;
    ++count;
    if ((step > 0 && at > INT64_MAX - step) ||
        (step < 0 && at < INT64_MIN - step))
      break;
    at += step;
  }

  ny_ct_fast_val_t result = ny_ct_fast_none();
  if (!ny_ct_make_seq(target->kind, count, &result))
    return false;
  int64_t at = start;
  for (size_t i = 0; i < count; ++i) {
    if (at < 0 || at >= len ||
        !ny_ct_fast_val_clone(&target->items[(size_t)at], &result.items[i])) {
      ny_ct_fast_val_free(&result);
      return false;
    }
    if (i + 1 < count) {
      if ((step > 0 && at > INT64_MAX - step) ||
          (step < 0 && at < INT64_MIN - step)) {
        ny_ct_fast_val_free(&result);
        return false;
      }
      at += step;
    }
  }
  ny_ct_fast_val_move(out, &result);
  return true;
}

/*
 * Shared container lookup for the member-call `.get` forms: dictionary keys
 * match by structural equality; strings index by UTF-8 scalar value and the
 * sequences by element, both with negative-index resolution. A missing key
 * or out-of-range index yields the explicit default, else the language zero
 * value, mirroring the bounded interpreter.
 */
static bool ny_ct_fast_container_get(const ny_ct_fast_val_t *container,
                                     const ny_ct_fast_val_t *key,
                                     const ny_ct_fast_val_t *fallback,
                                     ny_ct_fast_val_t *out) {
  if (!container || !key || !out)
    return false;
  if (container->kind == NY_CT_FAST_DICT) {
    for (size_t i = 0; i < container->len; ++i) {
      bool equal = false;
      if (!ny_ct_fast_values_equal(&container->items[i * 2], key, &equal, 0))
        return false;
      if (!equal)
        continue;
      return container->items &&
             ny_ct_fast_val_clone(&container->items[i * 2 + 1], out);
    }
    if (fallback)
      return ny_ct_fast_val_clone(fallback, out);
    *out = (ny_ct_fast_val_t){.kind = NY_CT_FAST_INT, .i = 0};
    return true;
  }
  int64_t len = 0;
  if (container->kind == NY_CT_FAST_STR) {
    if (key->kind != NY_CT_FAST_INT ||
        !ny_ct_fast_string_scalar_len(container, &len))
      return false;
  } else if (container->kind == NY_CT_FAST_LIST ||
             container->kind == NY_CT_FAST_TUPLE ||
             container->kind == NY_CT_FAST_RANGE) {
    if (key->kind != NY_CT_FAST_INT || !ny_ct_seq_len(container, &len))
      return false;
  } else {
    return false;
  }
  int64_t resolved = key->i;
  if (resolved < 0 && resolved < -len)
    resolved = len;
  else if (resolved < 0)
    resolved += len;
  if (resolved < 0 || resolved >= len) {
    if (fallback)
      return ny_ct_fast_val_clone(fallback, out);
    *out = (ny_ct_fast_val_t){.kind = NY_CT_FAST_INT, .i = 0};
    return true;
  }
  if (container->kind == NY_CT_FAST_STR)
    return ny_ct_fast_string_index(container, resolved, out);
  return ny_ct_seq_item(container, resolved, out);
}

/*
 * The memo key is the complete semantic identity of the expression: its
 * structure (including resolved NAME bindings) and nothing else. Recursion
 * depth is only an entry guard, so the same expression shares one entry at
 * every depth.
 */
static bool ny_ct_expr_memo_read(codegen_t *cg, const expr_t *expr,
                                 ny_ct_fast_val_t *out) {
  if (!cg || !out || !ny_ct_expr_memo_closed(cg, expr, 0))
    return false;
  codegen_env_cache_t *cache = &cg->env_cache;
  const void *binding = ny_ct_memo_ident_binding(cg, expr);
  uint64_t key_hash = ny_ct_memo_hash_expr(cg, expr);
  for (size_t i = 0; i < cache->comptime_expr_memo_len; ++i) {
    codegen_comptime_expr_memo_entry_t *entry = &cache->comptime_expr_memo[i];
    if (!entry->occupied || entry->key_hash != key_hash ||
        entry->binding != binding ||
        !ny_ct_memo_expr_equal(cg, entry->expr, expr))
      continue;
    memset(out, 0, sizeof(*out));
    if (!ny_ct_fast_val_clone(&entry->value, out))
      return false;
    if (getenv("NYTRIX_DBG_FASTCALL"))
      fprintf(stderr, "[memread] kind=%d i=%lld\n", (int)out->kind,
              (long long)(out->kind == NY_CT_FAST_INT ? out->i : -1));
    return true;
  }
  return false;
}

static void ny_ct_expr_memo_write(codegen_t *cg, const expr_t *expr,
                                  const ny_ct_fast_val_t *value) {
  if (!cg || !value || !ny_ct_expr_memo_closed(cg, expr, 0))
    return;
  codegen_env_cache_t *cache = &cg->env_cache;
  const void *binding = ny_ct_memo_ident_binding(cg, expr);
  uint64_t key_hash = ny_ct_memo_hash_expr(cg, expr);
  ny_ct_fast_val_t owned_value = {0};
  if (!ny_ct_fast_val_clone(value, &owned_value))
    return;
  for (size_t i = 0; i < cache->comptime_expr_memo_len; ++i) {
    codegen_comptime_expr_memo_entry_t *entry = &cache->comptime_expr_memo[i];
    if (entry->occupied && entry->key_hash == key_hash &&
        entry->binding == binding &&
        ny_ct_memo_expr_equal(cg, entry->expr, expr)) {
      ny_ct_fast_val_free(&entry->value);
      entry->value = owned_value;
      return;
    }
  }
  size_t slot;
  if (cache->comptime_expr_memo_len < NY_COMPTIME_EXPR_MEMO_CAPACITY) {
    slot = cache->comptime_expr_memo_len++;
  } else {
    slot = cache->comptime_expr_memo_next;
    cache->comptime_expr_memo_next =
        (slot + 1) % NY_COMPTIME_EXPR_MEMO_CAPACITY;
  }
  codegen_comptime_expr_memo_entry_t *entry = &cache->comptime_expr_memo[slot];
  if (entry->occupied)
    ny_ct_fast_val_free(&entry->value);
  if (getenv("NYTRIX_DBG_FASTCALL"))
    fprintf(stderr, "[memwrite] kind=%d i=%lld\n", (int)value->kind,
            (long long)(value->kind == NY_CT_FAST_INT ? value->i : -1));
  *entry = (codegen_comptime_expr_memo_entry_t){.expr = expr,
                                                .binding = binding,
                                                .key_hash = key_hash,
                                                .value = owned_value,
                                                .occupied = true};
}

bool ny_try_eval_comptime_expr_fast(codegen_t *cg, expr_t *e,
                                    ny_ct_fast_val_t *out, int depth) {
  if (!e || !out || depth > 64)
    return false;
  if (cg && ny_ct_expr_memo_read(cg, e, out))
    return ny_ct_fuel_consume(cg);
  if (!ny_ct_eval_comptime_expr_fast_uncached(cg, e, out, depth))
    return false;
  ny_ct_expr_memo_write(cg, e, out);
  return true;
}

static bool ny_ct_eval_comptime_expr_fast_uncached(codegen_t *cg, expr_t *e,
                                                   ny_ct_fast_val_t *out,
                                                   int depth) {
  if (!e || !out || depth > 64)
    return false;
  if (!ny_ct_fuel_consume(cg))
    return false;

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->tok.kind == NY_T_NIL) {
      out->kind = NY_CT_FAST_NONE;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_INT) {
      if (e->as.literal.integer_outside_i64)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      out->kind = NY_CT_FAST_FLOAT;
      out->f64 = e->as.literal.as.f;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      out->len = e->as.literal.as.s.len;
      return true;
    }
    return false;

  case NY_E_LIST:
  case NY_E_TUPLE: {
    ny_ct_fast_kind_t kind =
        e->kind == NY_E_LIST ? NY_CT_FAST_LIST : NY_CT_FAST_TUPLE;
    if (!ny_ct_make_seq(kind, e->as.list_like.len, out))
      return false;
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      ny_ct_fast_val_t item = ny_ct_fast_none();
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.list_like.data[i], &item,
                                          depth + 1)) {
        ny_ct_fast_val_free(&item);
        ny_ct_fast_val_free(out);
        *out = ny_ct_fast_none();
        return false;
      }
      ny_ct_fast_val_move(&out->items[i], &item);
    }
    return true;
  }

  case NY_E_DICT: {
    size_t pair_count = e->as.dict.pairs.len;
    if (!ny_ct_make_seq(NY_CT_FAST_DICT, pair_count, out))
      return false;
    /*
     * `len` counts live pairs, while the allocation reserves one slot per
     * source pair. Update it as pairs are committed so cleanup never visits
     * uninitialized capacity.
     */
    out->len = 0;
    for (size_t i = 0; i < pair_count; ++i) {
      ny_ct_fast_val_t key = ny_ct_fast_none();
      ny_ct_fast_val_t value = ny_ct_fast_none();
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.dict.pairs.data[i].key,
                                          &key, depth + 1) ||
          !ny_try_eval_comptime_expr_fast(cg, e->as.dict.pairs.data[i].value,
                                          &value, depth + 1)) {
        ny_ct_fast_val_free(&key);
        ny_ct_fast_val_free(&value);
        ny_ct_fast_val_free(out);
        *out = ny_ct_fast_none();
        return false;
      }
      size_t found = out->len;
      for (size_t j = 0; j < out->len; ++j) {
        bool equal = false;
        if (!ny_ct_fast_values_equal(&out->items[j * 2], &key, &equal, 0)) {
          ny_ct_fast_val_free(&key);
          ny_ct_fast_val_free(&value);
          ny_ct_fast_val_free(out);
          *out = ny_ct_fast_none();
          return false;
        }
        if (equal) {
          found = j;
          break;
        }
      }
      if (found < out->len) {
        ny_ct_fast_val_free(&key);
        ny_ct_fast_val_free(&out->items[found * 2 + 1]);
        ny_ct_fast_val_move(&out->items[found * 2 + 1], &value);
      } else {
        ny_ct_fast_val_move(&out->items[out->len * 2], &key);
        ny_ct_fast_val_move(&out->items[out->len * 2 + 1], &value);
        ++out->len;
      }
    }
    return true;
  }

  case NY_E_INDEX: {
    if (!e->as.index.target ||
        (!e->as.index.start && !e->as.index.stop && !e->as.index.step))
      return false;
    bool slicing = !e->as.index.start || e->as.index.stop || e->as.index.step;
    if (slicing) {
      ny_ct_fast_val_t target = ny_ct_fast_none();
      ny_ct_fast_val_t start = ny_ct_fast_none();
      ny_ct_fast_val_t stop = ny_ct_fast_none();
      ny_ct_fast_val_t step = ny_ct_fast_none();
      const ny_ct_fast_val_t *start_arg = NULL;
      const ny_ct_fast_val_t *stop_arg = NULL;
      const ny_ct_fast_val_t *step_arg = NULL;
      bool stop_is_omitted = false;
      bool ok = ny_try_eval_comptime_expr_fast(cg, e->as.index.target, &target,
                                               depth + 1);
      if (ok && e->as.index.start) {
        ok = ny_try_eval_comptime_expr_fast(cg, e->as.index.start, &start,
                                            depth + 1);
        start_arg = &start;
      }
      if (ok && e->as.index.stop) {
        stop_is_omitted = e->as.index.stop->kind == NY_E_LITERAL &&
                          e->as.index.stop->as.literal.kind == NY_LIT_INT &&
                          e->as.index.stop->as.literal.as.i == 0x3fffffff &&
                          e->as.index.stop->tok.kind != NY_T_NUMBER;
        if (!stop_is_omitted) {
          ok = ny_try_eval_comptime_expr_fast(cg, e->as.index.stop, &stop,
                                              depth + 1);
          stop_arg = &stop;
        }
      }
      if (ok && e->as.index.step) {
        ok = ny_try_eval_comptime_expr_fast(cg, e->as.index.step, &step,
                                            depth + 1);
        step_arg = &step;
      }
      if (ok)
        ok = ny_ct_fast_slice_sequence(&target, start_arg, stop_arg, step_arg,
                                       stop_is_omitted, out);
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&start);
      ny_ct_fast_val_free(&stop);
      ny_ct_fast_val_free(&step);
      return ok;
    }
    ny_ct_fast_val_t target = ny_ct_fast_none();
    ny_ct_fast_val_t index = ny_ct_fast_none();
    bool ok = ny_try_eval_comptime_expr_fast(cg, e->as.index.target, &target,
                                             depth + 1) &&
              ny_try_eval_comptime_expr_fast(cg, e->as.index.start, &index,
                                             depth + 1);
    if (!ok) {
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&index);
      return false;
    }
    if (target.kind == NY_CT_FAST_STR) {
      if (index.kind != NY_CT_FAST_INT) {
        ny_ct_fast_val_free(&target);
        ny_ct_fast_val_free(&index);
        return false;
      }
      ok = ny_ct_fast_string_index(&target, index.i, out);
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&index);
      return ok;
    }
    if (target.kind == NY_CT_FAST_LIST || target.kind == NY_CT_FAST_TUPLE) {
      if (index.kind != NY_CT_FAST_INT) {
        ny_ct_fast_val_free(&target);
        ny_ct_fast_val_free(&index);
        return false;
      }
      int64_t resolved = index.i;
      if (resolved < 0)
        resolved += (int64_t)target.len;
      if (resolved < 0 || (uint64_t)resolved >= target.len || !target.items) {
        ny_ct_fast_val_free(&target);
        ny_ct_fast_val_free(&index);
        return false;
      }
      ok = ny_ct_fast_val_clone(&target.items[(size_t)resolved], out);
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&index);
      return ok;
    }
    if (target.kind == NY_CT_FAST_DICT) {
      bool found = false;
      for (size_t i = 0; i < target.len; ++i) {
        bool equal = false;
        if (!ny_ct_fast_values_equal(&target.items[i * 2], &index, &equal, 0)) {
          ny_ct_fast_val_free(&target);
          ny_ct_fast_val_free(&index);
          return false;
        }
        if (!equal)
          continue;
        found = true;
        ok = ny_ct_fast_val_clone(&target.items[i * 2 + 1], out);
        break;
      }
      ny_ct_fast_val_free(&target);
      ny_ct_fast_val_free(&index);
      return found && ok;
    }
    ny_ct_fast_val_free(&target);
    ny_ct_fast_val_free(&index);
    return false;
  }

  case NY_E_IDENT:
    if (ny_ct_platform_ident(e->as.ident.name, out))
      return true;
    if (cg) {
      binding *b = lookup_global(cg, e->as.ident.name);
      if (ny_try_eval_binding_comptime_const(cg, b, e->as.ident.name, out,
                                             depth + 1))
        return true;
    }
    return false;

  case NY_E_UNARY: {
    ny_ct_fast_val_t r = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.unary.right, &r, depth + 1))
      return false;
    bool ok = ny_ct_fast_eval_unary(e->as.unary.op, &r, out);
    ny_ct_fast_val_free(&r);
    return ok;
  }

  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.left, &l, depth + 1))
      return false;
    bool lt = false;
    if (!ny_ct_fast_truthy(&l, &lt)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    if (strcmp(e->as.logical.op, "&&") == 0) {
      ny_ct_fast_val_free(&l);
      if (!lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = false;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.right, &r,
                                          depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      ny_ct_fast_val_free(&l);
      if (lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = true;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.logical.right, &r,
                                          depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt)) {
        ny_ct_fast_val_free(&r);
        return false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      ny_ct_fast_val_free(&r);
      return true;
    }
    ny_ct_fast_val_free(&l);
    return false;
  }

  case NY_E_BINARY: {
    ny_ct_fast_val_t l = {0}, r = {0};
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.binary.left, &l, depth + 1))
      return false;
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.binary.right, &r,
                                        depth + 1)) {
      ny_ct_fast_val_free(&l);
      return false;
    }
    bool ok = ny_ct_fast_eval_binary(e->as.binary.op, &l, &r, out);
    ny_ct_fast_val_free(&l);
    ny_ct_fast_val_free(&r);
    return ok;
  }

  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = {0};
    bool ct = false;
    if (!ny_try_eval_comptime_expr_fast(cg, e->as.ternary.cond, &c,
                                        depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct)) {
      ny_ct_fast_val_free(&c);
      return false;
    }
    ny_ct_fast_val_free(&c);
    return ny_try_eval_comptime_expr_fast(
        cg, ct ? e->as.ternary.true_expr : e->as.ternary.false_expr, out,
        depth + 1);
  }

  case NY_E_MEMBER:
    if (e->as.member.name && strcmp(e->as.member.name, "long") == 0)
      return ny_ct_fast_eval_long_property(cg, e->as.member.target, out,
                                           depth + 1);
    return false;

  case NY_E_COMPTIME:
    return ny_try_eval_comptime_fast_value(cg, e->as.comptime_expr.body, out);

  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    /*
     * Dictionary and sequence lookup is pure when the receiver and key are
     * closed. Besides supporting ordinary comptime queries, evaluating the
     * receiver through the shared fast evaluator lets structurally equal
     * receiver expressions reuse the owned-value memo safely. Missing keys
     * and out-of-range indexes fall back to the explicit default, then to
     * the language zero value.
     */
    if (strcmp(name, "get") == 0 && e->as.call.callee->kind == NY_E_MEMBER &&
        (e->as.call.args.len == 1 || e->as.call.args.len == 2)) {
      ny_ct_fast_val_t container = ny_ct_fast_none();
      ny_ct_fast_val_t key = ny_ct_fast_none();
      bool ok =
          ny_try_eval_comptime_expr_fast(
              cg, e->as.call.callee->as.member.target, &container, depth + 1) &&
          ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[0].val, &key,
                                         depth + 1);
      if (ok && container.kind == NY_CT_FAST_DICT) {
        bool matched = false;
        bool cloned = false;
        for (size_t i = 0; i < container.len; ++i) {
          bool equal = false;
          if (!ny_ct_fast_values_equal(&container.items[i * 2], &key, &equal,
                                       0)) {
            ok = false;
            break;
          }
          if (!equal)
            continue;
          matched = true;
          cloned = ny_ct_fast_val_clone(&container.items[i * 2 + 1], out);
          break;
        }
        if (ok && !matched) {
          if (e->as.call.args.len == 2) {
            ny_ct_fast_val_t fallback = ny_ct_fast_none();
            if (!ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[1].val,
                                                &fallback, depth + 1)) {
              ny_ct_fast_val_free(&fallback);
              ok = false;
            } else {
              ny_ct_fast_val_move(out, &fallback);
            }
          } else {
            *out = (ny_ct_fast_val_t){.kind = NY_CT_FAST_INT, .i = 0};
          }
        } else if (ok && matched && !cloned) {
          ok = false;
        }
        ny_ct_fast_val_free(&container);
        ny_ct_fast_val_free(&key);
        return ok;
      }
      if (ok && (container.kind == NY_CT_FAST_STR ||
                 container.kind == NY_CT_FAST_LIST ||
                 container.kind == NY_CT_FAST_TUPLE)) {
        if (key.kind == NY_CT_FAST_INT) {
          int64_t len = 0;
          ok = container.kind == NY_CT_FAST_STR
                   ? ny_ct_fast_string_scalar_len(&container, &len)
                   : ny_ct_seq_len(&container, &len);
          if (ok) {
            int64_t resolved = key.i;
            if (resolved < 0 && resolved < -len)
              resolved = len;
            else if (resolved < 0)
              resolved += len;
            if (resolved < 0 || resolved >= len) {
              if (e->as.call.args.len == 2) {
                ny_ct_fast_val_t fallback = ny_ct_fast_none();
                if (!ny_try_eval_comptime_expr_fast(cg,
                                                    e->as.call.args.data[1].val,
                                                    &fallback, depth + 1)) {
                  ny_ct_fast_val_free(&fallback);
                  ok = false;
                } else {
                  ny_ct_fast_val_move(out, &fallback);
                }
              } else {
                *out = (ny_ct_fast_val_t){.kind = NY_CT_FAST_INT, .i = 0};
              }
            } else if (container.kind == NY_CT_FAST_STR) {
              ok = ny_ct_fast_string_index(&container, resolved, out);
            } else {
              ok = ny_ct_seq_item(&container, resolved, out);
            }
          }
        } else {
          ok = false;
        }
        ny_ct_fast_val_free(&container);
        ny_ct_fast_val_free(&key);
        return ok;
      }
      ny_ct_fast_val_free(&container);
      ny_ct_fast_val_free(&key);
      return false;
    }
    bool zero_arg = (e->as.call.args.len == 0);
    bool one_member_arg =
        (e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER);
    if (strcmp(name, "Z") == 0 && e->as.call.args.len == 1) {
      ny_ct_fast_val_t arg = {0};
      if (!ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[0].val, &arg,
                                          depth + 1))
        return false;
      bool ok = ny_ct_fast_eval_bigint_constructor_arg(&arg, out);
      ny_ct_fast_val_free(&arg);
      return ok;
    }
    if (strcmp(name, "len") == 0 && e->as.call.args.len == 1 &&
        !e->as.call.args.data[0].name && !ny_ct_fast_call_shadowed(cg, name)) {
      /*
       * Mirrors the bounded interpreter: strings report their byte length,
       * sequences and dictionaries their element/pair count.
       */
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      int64_t len = 0;
      bool ok = ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[0].val,
                                               &arg, depth + 1);
      if (ok && arg.kind == NY_CT_FAST_STR) {
        if (arg.len > (size_t)INT64_MAX)
          ok = false;
        else
          len = (int64_t)arg.len;
      } else if (ok) {
        ok = ny_ct_seq_len(&arg, &len);
      }
      ny_ct_fast_val_free(&arg);
      if (!ok)
        return false;
      out->kind = NY_CT_FAST_INT;
      out->i = len;
      return true;
    }
    if (strcmp(name, "range") == 0 && e->as.call.args.len >= 1 &&
        e->as.call.args.len <= 3 && !ny_ct_fast_call_shadowed(cg, name)) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      bool ok = true;
      for (size_t i = 0; ok && i < e->as.call.args.len; ++i) {
        if (e->as.call.args.data[i].name ||
            !ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[i].val,
                                            &vals[i], depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT)
          ok = false;
      }
      if (ok)
        ok = e->as.call.args.len == 1 ? ny_ct_make_range(0, vals[0].i, 1, out)
             : e->as.call.args.len == 2
                 ? ny_ct_make_range(vals[0].i, vals[1].i, 1, out)
                 : ny_ct_make_range(vals[0].i, vals[1].i, vals[2].i, out);
      for (size_t j = 0; j < 3; ++j)
        ny_ct_fast_val_free(&vals[j]);
      return ok;
    }
    if (strcmp(name, "range2") == 0 && e->as.call.args.len >= 2 &&
        e->as.call.args.len <= 3 && !ny_ct_fast_call_shadowed(cg, name)) {
      ny_ct_fast_val_t vals[3] = {ny_ct_fast_none(), ny_ct_fast_none(),
                                  ny_ct_fast_none()};
      bool ok = true;
      for (size_t i = 0; ok && i < e->as.call.args.len; ++i) {
        if (e->as.call.args.data[i].name ||
            !ny_try_eval_comptime_expr_fast(cg, e->as.call.args.data[i].val,
                                            &vals[i], depth + 1) ||
            vals[i].kind != NY_CT_FAST_INT)
          ok = false;
      }
      if (ok)
        ok = ny_ct_make_range(vals[0].i, vals[1].i,
                              e->as.call.args.len == 3 ? vals[2].i : 1, out);
      for (size_t i = 0; i < 3; ++i)
        ny_ct_fast_val_free(&vals[i]);
      return ok;
    }
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled(cg, e->tok);
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_os_name();
      out->len = strlen(out->s ? out->s : "");
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_host_arch_name();
      out->len = strlen(out->s ? out->s : "");
      return true;
    }
    refine_def_t *ref = ny_lookup_refine(cg, name);
    if (ref && e->as.call.args.len == 1) {
      int res = 0;
      if (ny_refine_eval_predicate(cg, NULL, 0, ref,
                                   e->as.call.args.data[0].val, &res)) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = (res == 1);
        return true;
      }
    }
    if (ny_ct_memo_pure_function(cg, e))
      return ny_ct_interp_eval_pure_call(cg, e, out);
    return false;
  }

  case NY_E_MEMCALL: {
    /*
     * `receiver.get(key[, default])` is pure over closed receivers. This
     * mirrors the bounded interpreter, including named `key=`/`default=`
     * arguments, and lets the memo serve structurally equal lookups. Other
     * member calls (notably `map`) may run user code and stay on the
     * interpreter.
     */
    if (!e->as.memcall.name || strcmp(e->as.memcall.name, "get") != 0 ||
        (e->as.memcall.args.len != 1 && e->as.memcall.args.len != 2) ||
        !e->as.memcall.target)
      return false;
    ny_ct_fast_val_t container = ny_ct_fast_none();
    ny_ct_fast_val_t key = ny_ct_fast_none();
    ny_ct_fast_val_t fallback = {.kind = NY_CT_FAST_INT, .i = 0};
    bool have_index = false;
    bool have_default = false;
    bool ok = ny_try_eval_comptime_expr_fast(cg, e->as.memcall.target,
                                             &container, depth + 1);
    for (size_t i = 0; ok && i < e->as.memcall.args.len; ++i) {
      const call_arg_t *arg = &e->as.memcall.args.data[i];
      ny_ct_fast_val_t *slot = NULL;
      if (!arg->name) {
        if (!have_index) {
          slot = &key;
          have_index = true;
        } else if (!have_default) {
          slot = &fallback;
          have_default = true;
        }
      } else if (strcmp(arg->name, "key") == 0 && !have_index) {
        slot = &key;
        have_index = true;
      } else if (strcmp(arg->name, "default") == 0 && !have_default) {
        slot = &fallback;
        have_default = true;
      }
      ok = slot && arg->val &&
           ny_try_eval_comptime_expr_fast(cg, arg->val, slot, depth + 1);
    }
    if (ok && have_index)
      ok = ny_ct_fast_container_get(&container, &key,
                                    have_default ? &fallback : NULL, out);
    if (getenv("NYTRIX_DBG_FASTCALL"))
      fprintf(stderr,
              "[memget] ok=%d ckind=%d keykind=%d outkind=%d outi=%lld\n",
              (int)ok, (int)container.kind, (int)key.kind, (int)out->kind,
              (long long)(out->kind == NY_CT_FAST_INT ? out->i : -1));
    ny_ct_fast_val_free(&container);
    ny_ct_fast_val_free(&key);
    ny_ct_fast_val_free(&fallback);
    return ok;
  }

  case NY_E_EXISTS: {
    const char *k_name = e->as.exists.var;
    expr_t *body = e->as.exists.body;
    if (!k_name || !body)
      return false;
    bool found_witness = false;
    for (int64_t cand = -500; cand <= 500; cand++) {
      expr_t lit_k;
      memset(&lit_k, 0, sizeof(lit_k));
      lit_k.kind = NY_E_LITERAL;
      lit_k.tok = e->tok;
      lit_k.as.literal.kind = NY_LIT_INT;
      lit_k.as.literal.as.i = cand;
      /*
       * The fast evaluator memoizes by AST address within a codegen arena.
       * Reusing this stack address across candidates makes every substituted
       * binder read the first candidate's cached literal instead of its
       * current value. Arena-backed nodes remain immutable and uniquely
       * identified for the lifetime of the memo.
       */
      expr_t *witness = &lit_k;
      if (cg && cg->arena) {
        witness = arena_alloc(cg->arena, sizeof(*witness));
        *witness = lit_k;
      }
      expr_t *sub = ny_expr_subst(cg ? cg->arena : NULL, body, k_name, witness);
      ny_ct_fast_val_t res = ny_ct_fast_none();
      if (ny_try_eval_comptime_expr_fast(cg, sub, &res, depth + 1)) {
        if (res.kind == NY_CT_FAST_BOOL && res.b) {
          found_witness = true;
          break;
        }
      }
    }
    out->kind = NY_CT_FAST_BOOL;
    out->b = found_witness;
    return true;
  }

  default:
    return false;
  }
}

bool ny_try_eval_comptime_fast_value(codegen_t *cg, stmt_t *body,
                                     ny_ct_fast_val_t *out) {
  if (!body || !out)
    return false;
  if (!ny_ct_fuel_consume(cg))
    return false;

  if (body->kind == NY_S_BLOCK) {
    ny_ct_fast_val_t res = ny_ct_fast_none();
    for (size_t i = 0; i < body->as.block.body.len; ++i) {
      stmt_t *s = body->as.block.body.data[i];
      if (!s)
        continue;
      if (s->kind == NY_S_FUNC || s->kind == NY_S_EXTERN ||
          s->kind == NY_S_LINK || s->kind == NY_S_MODULE ||
          s->kind == NY_S_STRUCT || s->kind == NY_S_LAYOUT ||
          s->kind == NY_S_ENUM || s->kind == NY_S_MACRO ||
          s->kind == NY_S_USE || s->kind == NY_S_EXPORT ||
          s->kind == NY_S_OPERATOR)
        continue;
      ny_ct_fast_val_t next = ny_ct_fast_none();
      if (!ny_try_eval_comptime_fast_value(cg, s, &next)) {
        ny_ct_fast_val_free(&res);
        return false;
      }
      ny_ct_fast_val_move(&res, &next);
    }
    ny_ct_fast_val_move(out, &res);
    return true;
  }

  if (body->kind == NY_S_IF) {
    ny_ct_fast_val_t cond_v = {0};
    bool truthy = false;
    if (ny_try_eval_comptime_expr_fast(cg, body->as.iff.test, &cond_v, 0) &&
        ny_ct_fast_truthy(&cond_v, &truthy)) {
      ny_ct_fast_val_free(&cond_v);
      stmt_t *branch = truthy ? body->as.iff.conseq : body->as.iff.alt;
      if (branch)
        return ny_try_eval_comptime_fast_value(cg, branch, out);
      ny_ct_fast_val_free(out);
      *out = ny_ct_fast_none();
      return true;
    }
    ny_ct_fast_val_free(&cond_v);
    return false;
  }

  expr_t *e = NULL;
  if (body->kind == NY_S_RETURN)
    e = body->as.ret.value;
  else if (body->kind == NY_S_EXPR)
    e = body->as.expr.expr;
  else
    return false;

  if (!e) {
    ny_ct_fast_val_free(out);
    *out = ny_ct_fast_none();
    return true;
  }
  return ny_try_eval_comptime_expr_fast(cg, e, out, 0);
}
