#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool nir_collect_consts(const nyir_func_t *f, bool *known, int64_t *value) {
  if (!f || !known || !value)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_CONST_I64 || in->op == NYIR_CONST_F64 ||
         in->op == NYIR_CONST_F32) &&
        in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
               known[in->a]) {
      known[in->dst] = true;
      value[in->dst] = value[in->a];
    } else if (in->dst >= 0) {
      known[in->dst] = false;
    }
  }
  return true;
}

void nir_make_copy(nyir_inst_t *in, int src) {
  int dst = in->dst;
  *in = (nyir_inst_t){.op = NYIR_COPY, .dst = dst, .a = src, .b = -1};
}

void nir_make_const(nyir_inst_t *in, int64_t value) {
  int dst = in->dst;
  *in = (nyir_inst_t){.op = NYIR_CONST_I64,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .imm = value};
}

void nir_make_f64_const(nyir_inst_t *in, int64_t bitcast) {
  int dst = in->dst;
  *in = (nyir_inst_t){.op = NYIR_CONST_F64,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .imm = bitcast};
}

void nir_make_f32_const(nyir_inst_t *in, int64_t bitcast) {
  int dst = in->dst;
  *in = (nyir_inst_t){.op = NYIR_CONST_F32,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .imm = bitcast};
}

bool nir_cmp_same_value(nyir_cmp_t cmp, int64_t *out) {
  if (!out)
    return false;
  switch (cmp) {
  case NYIR_CMP_EQ:
  case NYIR_CMP_LE:
  case NYIR_CMP_GE:
    *out = 1;
    return true;
  case NYIR_CMP_NE:
  case NYIR_CMP_LT:
  case NYIR_CMP_GT:
    *out = 0;
    return true;
  }
  return false;
}

bool nir_cmp_range_fold(nyir_cmp_t cmp, const nyir_range_t *a,
                        const nyir_range_t *b, int64_t *out) {
  if (!a || !b || !out || !a->has_min || !a->has_max || !b->has_min ||
      !b->has_max)
    return false;
  bool disjoint = a->max < b->min || b->max < a->min;
  switch (cmp) {
  case NYIR_CMP_EQ:
    if (disjoint) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_NE:
    if (disjoint) {
      *out = 1;
      return true;
    }
    return false;
  case NYIR_CMP_LT:
    if (a->max < b->min) {
      *out = 1;
      return true;
    }
    if (a->min >= b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_LE:
    if (a->max <= b->min) {
      *out = 1;
      return true;
    }
    if (a->min > b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_GT:
    if (a->min > b->max) {
      *out = 1;
      return true;
    }
    if (a->max <= b->min) {
      *out = 0;
      return true;
    }
    return false;
  case NYIR_CMP_GE:
    if (a->min >= b->max) {
      *out = 1;
      return true;
    }
    if (a->max < b->min) {
      *out = 0;
      return true;
    }
    return false;
  }
  return false;
}

bool nir_range_excludes_zero(const nyir_range_t *r) {
  if (!r)
    return false;
  if (r->has_min && r->min > 0)
    return true;
  if (r->has_max && r->max < 0)
    return true;
  return false;
}

bool nir_recover_load_local_range(const nyir_func_t *f,
                                  const nyir_value_fact_t *facts, int value,
                                  size_t at, nyir_range_t *out) {
  if (!f || !facts || !out || value < 0 || value >= f->next_value)
    return false;
  for (size_t j = at; j > 0; --j) {
    const nyir_inst_t *def = &f->data[j - 1];
    if (def->dst != value)
      continue;
    if (def->op != NYIR_LOAD_LOCAL)
      return false;
    for (size_t k = j - 1; k > 0; --k) {
      const nyir_inst_t *store = &f->data[k - 1];
      if (store->op == NYIR_STORE_LOCAL && store->imm == def->imm) {
        if (store->a >= 0 && store->a < f->next_value &&
            (facts[store->a].range.has_min || facts[store->a].range.has_max)) {
          *out = facts[store->a].range;
          return true;
        }
        return false;
      }
      if (store->op == NYIR_LABEL || store->op == NYIR_BR ||
          store->op == NYIR_BR_IF || store->op == NYIR_CALL)
        return false;
    }
    return false;
  }
  return false;
}

bool nir_value_range_at(const nyir_func_t *f,
                        const nyir_value_fact_t *facts, int value, size_t at,
                        nyir_range_t *out) {
  if (!f || !facts || !out || value < 0 || value >= f->next_value)
    return false;
  if (facts[value].range.has_min || facts[value].range.has_max) {
    *out = facts[value].range;
    return true;
  }
  return nir_recover_load_local_range(f, facts, value, at, out);
}

bool nir_operands_same_value(const nyir_func_t *f, int a, int b, size_t at) {
  if (a >= 0 && a == b)
    return true;
  if (!f || a < 0 || b < 0)
    return false;
  size_t ia = 0;
  size_t ib = 0;
  bool have_a = false;
  bool have_b = false;
  for (size_t j = at; j > 0 && (!have_a || !have_b); --j) {
    const nyir_inst_t *def = &f->data[j - 1];
    if (!have_a && def->dst == a) {
      ia = j - 1;
      have_a = true;
    }
    if (!have_b && def->dst == b) {
      ib = j - 1;
      have_b = true;
    }
  }
  if (!have_a || !have_b)
    return false;
  const nyir_inst_t *da = &f->data[ia];
  const nyir_inst_t *db = &f->data[ib];
  if (da->op == NYIR_COPY && da->a == b)
    return true;
  if (db->op == NYIR_COPY && db->a == a)
    return true;
  if (da->op == NYIR_COPY && db->op == NYIR_COPY && da->a >= 0 &&
      da->a == db->a)
    return true;
  if (da->op == NYIR_LOAD_LOCAL && db->op == NYIR_LOAD_LOCAL &&
      da->imm == db->imm) {
    size_t lo = ia < ib ? ia : ib;
    size_t hi = ia < ib ? ib : ia;
    int64_t local = da->imm;
    for (size_t k = lo + 1; k < hi; ++k) {
      const nyir_inst_t *mid = &f->data[k];
      if (mid->op == NYIR_STORE_LOCAL && mid->imm == local)
        return false;
      if (mid->op == NYIR_LABEL || mid->op == NYIR_BR ||
          mid->op == NYIR_BR_IF || mid->op == NYIR_CALL)
        return false;
    }
    return true;
  }
  return false;
}

bool nir_range_excludes_int64_min(const nyir_range_t *r) {
  return r && r->has_min && r->min > INT64_MIN;
}

int nir_find_block_const0(const nyir_func_t *f, size_t at) {
  if (!f)
    return -1;
  int found = -1;
  for (size_t j = 0; j < at; ++j) {
    const nyir_inst_t *in = &f->data[j];
    if (in->op == NYIR_LABEL) {
      found = -1;
      continue;
    }
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 && in->imm == 0)
      found = in->dst;
  }
  return found;
}

bool nir_rewrite_neg(nyir_func_t *f, size_t *i, int src) {
  if (!f || !i || *i >= f->len || src < 0)
    return false;
  int dst = f->data[*i].dst;
  if (dst < 0)
    return false;
  int zero = nir_find_block_const0(f, *i);
  if (zero < 0) {
    zero = f->next_value++;
    if (f->len + 1 > f->cap) {
      size_t new_cap = f->cap ? f->cap * 2 : 64;
      if (new_cap < f->cap || new_cap > SIZE_MAX / sizeof(nyir_inst_t))
        return false;
      nyir_inst_t *new_data =
          (nyir_inst_t *)realloc(f->data, new_cap * sizeof(nyir_inst_t));
      if (!new_data)
        return false;
      f->data = new_data;
      f->cap = new_cap;
    }
    memmove(&f->data[*i + 1], &f->data[*i],
            (f->len - *i) * sizeof(nyir_inst_t));
    f->len++;
    f->data[*i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                  .dst = zero,
                                  .a = -1,
                                  .b = -1,
                                  .imm = 0};
    (*i)++;
  }
  f->data[*i] = (nyir_inst_t){.op = NYIR_SUB_I64,
                                .dst = dst,
                                .a = zero,
                                .b = src,
                                .effects = NYIR_EFFECT_NONE};
  return true;
}

size_t nir_next_non_nop(const nyir_func_t *f, size_t start) {
  if (!f)
    return 0;
  for (size_t i = start; i < f->len; ++i) {
    if (f->data[i].op != NYIR_NOP)
      return i;
  }
  return f->len;
}

bool nir_remap_value(const int *map, int map_len, int value, int *out) {
  if (!out)
    return false;
  if (value < 0) {
    *out = value;
    return true;
  }
  if (!map || value >= map_len || map[value] < 0)
    return false;
  *out = map[value];
  return true;
}

bool nir_ensure_inst_space(nyir_func_t *f, size_t extra) {
  if (!f)
    return false;
  if (f->len + extra <= f->cap)
    return true;
  size_t new_cap = f->cap ? f->cap : 64;
  while (new_cap < f->len + extra)
    new_cap *= 2;
  nyir_inst_t *new_data =
      (nyir_inst_t *)realloc(f->data, new_cap * sizeof(nyir_inst_t));
  if (!new_data)
    return false;
  f->data = new_data;
  f->cap = new_cap;
  return true;
}

bool nyir_has_control_flow(const nyir_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op == NYIR_LABEL || op == NYIR_BR || op == NYIR_BR_IF)
      return true;
  }
  return false;
}

bool nyir_has_loop(const nyir_func_t *f) {
  if (!f || f->len == 0)
    return false;
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return false;
  bool has = false;
  if (cfg.backedge_edges) {
    for (size_t p = 0; p < cfg.block_count && !has; ++p) {
      for (size_t e = cfg.succ_offsets[p]; e < cfg.succ_offsets[p + 1]; ++e) {
        if (cfg.backedge_edges[e]) {
          has = true;
          break;
        }
      }
    }
  }
  nyir_cfg_free(&cfg);
  return has;
}

bool nyir_has_local_mem(const nyir_func_t *f) {
  if (!f)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    nyir_op_t op = f->data[i].op;
    if (op == NYIR_LOAD_LOCAL || op == NYIR_STORE_LOCAL ||
        op == NYIR_ADDR_LOCAL)
      return true;
  }
  return false;
}

size_t nyir_count_nops(const nyir_func_t *f) {
  size_t n = 0;
  if (!f)
    return 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_NOP)
      n++;
  return n;
}

bool nyir_compact_if_sparse(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;
  size_t nops = nyir_count_nops(f);
  if (nops >= 2 && (nops * 8 >= f->len || nops * 2 >= f->len))
    return nyir_compact(f);
  return true;
}

size_t nyir_block_count(const nyir_func_t *f) {
  if (!f || f->len == 0)
    return 0;
  size_t count = 1;
  for (size_t i = 1; i < f->len; ++i) {
    nyir_op_t prev = f->data[i - 1].op;
    if (f->data[i].op == NYIR_LABEL || prev == NYIR_BR ||
        prev == NYIR_BR_IF || prev == NYIR_RET)
      ++count;
  }
  return count;
}

uint64_t nyir_debug_fingerprint(const nyir_func_t *f) {
  uint64_t hash = UINT64_C(1469598103934665603);
  if (!f)
    return hash;
#define NIR_HASH(v)                                                            \
  do {                                                                         \
    hash ^= (uint64_t)(v);                                                     \
    hash *= UINT64_C(1099511628211);                                           \
  } while (0)
  NIR_HASH(f->len);
  NIR_HASH(f->next_value);
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    NIR_HASH(in->op);
    NIR_HASH(in->cmp);
    NIR_HASH(in->dst);
    NIR_HASH(in->a);
    NIR_HASH(in->b);
    NIR_HASH(in->c);
    NIR_HASH(in->d);
    NIR_HASH(in->e);
    NIR_HASH(in->f);
    NIR_HASH(in->imm);
    NIR_HASH(in->flags);
    NIR_HASH(in->effects);
    NIR_HASH(in->extra_args_len);
    NIR_HASH(in->phi_incoming_len);
    for (size_t k = 0; k < in->extra_args_len; ++k)
      NIR_HASH(in->extra_args[k]);
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      NIR_HASH(in->phi_incoming[k].predecessor_label);
      NIR_HASH(in->phi_incoming[k].value);
    }
  }
#undef NIR_HASH
  return hash;
}

void nyir_pass_tag(char *buf, size_t n, const char *pass_name) {
  if (!buf || n < 4) {
    if (buf && n)
      buf[0] = '\0';
    return;
  }
  size_t j = 0;
  buf[j++] = '<';
  for (size_t i = 0; pass_name && pass_name[i] && j + 2 < n; ++i) {
    char c = pass_name[i];
    if (c == '_' || c == ' ')
      c = '-';
    else if (c == '(' || c == ')')
      continue;
    buf[j++] = c;
  }
  buf[j++] = '>';
  buf[j] = '\0';
}

/* Fold a float binary operation when both operands are constant.  Returns true
 * and writes the bit-pattern result into *out_imm and *out_kind when the fold
 * is valid (no div-by-zero). */
bool nyir_float_fold_binary(nyir_op_t op, int64_t a_imm, int64_t b_imm,
                              unsigned char kind, int64_t *out_imm,
                              unsigned char *out_kind) {
  if (kind != 2 && kind != 3)
    return false;
  bool is_f32 = kind == 3;
  double av, bv, r = 0.0;
  if (is_f32) {
    uint32_t au = (uint32_t)(unsigned)a_imm;
    uint32_t bu = (uint32_t)(unsigned)b_imm;
    float af, bf;
    memcpy(&af, &au, sizeof(af));
    memcpy(&bf, &bu, sizeof(bf));
    av = (double)af;
    bv = (double)bf;
  } else {
    memcpy(&av, &a_imm, sizeof(av));
    memcpy(&bv, &b_imm, sizeof(bv));
  }
  bool ok = false;
  switch (op) {
  case NYIR_ADD_F64:
  case NYIR_ADD_F32:
    r = av + bv;
    ok = true;
    break;
  case NYIR_SUB_F64:
  case NYIR_SUB_F32:
    r = av - bv;
    ok = true;
    break;
  case NYIR_MUL_F64:
  case NYIR_MUL_F32:
    r = av * bv;
    ok = true;
    break;
  case NYIR_DIV_F64:
    if (bv != 0.0) {
      r = av / bv;
      ok = true;
    }
    break;
  case NYIR_DIV_F32:
    if ((float)bv != 0.0f) {
      r = (double)((float)av / (float)bv);
      ok = true;
    }
    break;
  default:
    break;
  }
  if (!ok)
    return false;
  if (is_f32) {
    float rf = (float)r;
    uint32_t bits = 0;
    memcpy(&bits, &rf, sizeof(bits));
    *out_imm = (int64_t)bits;
    *out_kind = 3;
  } else {
    memcpy(out_imm, &r, sizeof(*out_imm));
    *out_kind = 2;
  }
  return true;
}
