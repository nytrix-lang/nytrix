#include "code/native/ir/internal.h"
#include "code/native/ir.h"
#include "base/compat.h"
#include "base/common.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nir_binary_fold(ny_nir_op_t op, int64_t a, int64_t b, int64_t *out) {
  switch (op) {
  case NY_NIR_ADD_I64:
    *out = a + b;
    return true;
  case NY_NIR_SUB_I64:
    *out = a - b;
    return true;
  case NY_NIR_MUL_I64:
    *out = a * b;
    return true;
  case NY_NIR_DIV_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a / b;
    return true;
  case NY_NIR_MOD_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a % b;
    return true;
  case NY_NIR_AND_I64:
    *out = a & b;
    return true;
  case NY_NIR_OR_I64:
    *out = a | b;
    return true;
  case NY_NIR_XOR_I64:
    *out = a ^ b;
    return true;
  case NY_NIR_SHL_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = (int64_t)((uint64_t)a << (unsigned)b);
    return true;
  case NY_NIR_SAR_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = a >> (unsigned)b;
    return true;
  default:
    return false;
  }
}

static bool nir_cmp_fold(ny_nir_cmp_t cmp, int64_t a, int64_t b, int64_t *out) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    *out = a == b;
    return true;
  case NY_NIR_CMP_NE:
    *out = a != b;
    return true;
  case NY_NIR_CMP_LT:
    *out = a < b;
    return true;
  case NY_NIR_CMP_LE:
    *out = a <= b;
    return true;
  case NY_NIR_CMP_GT:
    *out = a > b;
    return true;
  case NY_NIR_CMP_GE:
    *out = a >= b;
    return true;
  }
  return false;
}

bool ny_nir_const_fold(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  int64_t max_local = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL ||
         in->op == NYIR_ADDR_LOCAL) &&
        in->imm > max_local)
      max_local = in->imm;
  }
  bool *local_known = NULL;
  bool *local_addr_taken = NULL;
  int64_t *local_value = NULL;
  if (max_local >= 0) {
    local_known = (bool *)calloc((size_t)max_local + 1, sizeof(bool));
    local_addr_taken = (bool *)calloc((size_t)max_local + 1, sizeof(bool));
    local_value = (int64_t *)calloc((size_t)max_local + 1, sizeof(int64_t));
  }
  if (!known || !value ||
      (max_local >= 0 && (!local_known || !local_addr_taken || !local_value))) {
    free(known);
    free(value);
    free(local_known);
    free(local_addr_taken);
    free(local_value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NY_NIR_COPY && in->dst >= 0) {
      if (in->a >= 0 && known[in->a]) {
        in->op = NY_NIR_CONST_I64;
        in->imm = value[in->a];
        in->a = -1;
        in->b = -1;
        in->symbol = NULL;
        known[in->dst] = true;
        value[in->dst] = in->imm;
      } else {
        known[in->dst] = false;
      }
      continue;
    }
    if (in->op == NY_NIR_STORE_LOCAL && in->imm >= 0 && in->imm <= max_local) {
      if (in->a >= 0 && known[in->a]) {
        local_known[in->imm] = true;
        local_value[in->imm] = value[in->a];
      } else {
        local_known[in->imm] = false;
      }
      continue;
    }
    if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
        in->imm <= max_local && local_known[in->imm]) {
      in->op = NY_NIR_CONST_I64;
      in->a = -1;
      in->b = -1;
      in->imm = local_value[in->imm];
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = in->imm;
      continue;
    }
    if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 && in->imm <= max_local) {
      local_addr_taken[in->imm] = true;
      local_known[in->imm] = false;
      if (in->dst >= 0)
        known[in->dst] = false;
      continue;
    }
    if (in->op == NYIR_STORE_I64 && local_known && local_addr_taken) {
      for (int64_t local = 0; local <= max_local; ++local) {
        if (local_addr_taken[local])
          local_known[local] = false;
      }
    }
    if (in->op == NY_NIR_BR_IF && in->a >= 0 && known[in->a]) {
      if (value[in->a]) {
        in->op = NY_NIR_BR;
        in->a = -1;
        in->b = -1;
      } else {
        in->op = NY_NIR_NOP;
        in->a = -1;
        in->b = -1;
        in->imm = 0;
      }
    }
    if (in->op == NY_NIR_LABEL || in->op == NY_NIR_BR ||
        in->op == NY_NIR_BR_IF || in->op == NY_NIR_CALL) {
      if (local_known)
        memset(local_known, 0, ((size_t)max_local + 1) * sizeof(bool));
    }
    int64_t folded = 0;
    if (in->dst >= 0 && in->a >= 0 && in->b >= 0 && known[in->a] &&
        known[in->b] &&
        (nir_binary_fold(in->op, value[in->a], value[in->b], &folded) ||
         (in->op == NY_NIR_CMP_I64 &&
          nir_cmp_fold(in->cmp, value[in->a], value[in->b], &folded)))) {
      in->op = NY_NIR_CONST_I64;
      in->imm = folded;
      in->a = -1;
      in->b = -1;
      in->symbol = NULL;
      known[in->dst] = true;
      value[in->dst] = folded;
      continue;
    }
    if (in->dst >= 0)
      known[in->dst] = false;
  }
  free(known);
  free(value);
  free(local_known);
  free(local_addr_taken);
  free(local_value);
  return true;
}

static int nir_alias_find(const int *alias, int v) {
  if (!alias || v < 0)
    return v;
  int cur = v;
  for (int depth = 0; depth < 64 && alias[cur] >= 0 && alias[cur] != cur; ++depth)
    cur = alias[cur];
  return cur;
}

bool ny_nir_copy_prop(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  int *alias = (int *)malloc((size_t)f->next_value * sizeof(int));
  if (!alias)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    alias[i] = i;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->a >= 0)
      in->a = nir_alias_find(alias, in->a);
    if (in->b >= 0)
      in->b = nir_alias_find(alias, in->b);
    if (in->c >= 0)
      in->c = nir_alias_find(alias, in->c);
    if (in->d >= 0)
      in->d = nir_alias_find(alias, in->d);
    if (in->e >= 0)
      in->e = nir_alias_find(alias, in->e);
    if (in->f >= 0)
      in->f = nir_alias_find(alias, in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        in->extra_args[k] = nir_alias_find(alias, in->extra_args[k]);
    }
    if (in->dst >= 0) {
      if (in->op == NY_NIR_COPY && in->a >= 0)
        alias[in->dst] = nir_alias_find(alias, in->a);
      else
        alias[in->dst] = in->dst;
    }
  }
  free(alias);
  return true;
}

static bool nir_collect_consts(const ny_nir_func_t *f, bool *known,
                               int64_t *value) {
  if (!f || !known || !value)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_CONST_I64 && in->dst >= 0) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    } else if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
               known[in->a]) {
      known[in->dst] = true;
      value[in->dst] = value[in->a];
    } else if (in->dst >= 0) {
      known[in->dst] = false;
    }
  }
  return true;
}

static void nir_make_copy(ny_nir_inst_t *in, int src) {
  int dst = in->dst;
  *in = (ny_nir_inst_t){.op = NY_NIR_COPY, .dst = dst, .a = src, .b = -1};
}

static void nir_make_const(ny_nir_inst_t *in, int64_t value) {
  int dst = in->dst;
  *in = (ny_nir_inst_t){.op = NY_NIR_CONST_I64,
                        .dst = dst,
                        .a = -1,
                        .b = -1,
                        .imm = value};
}

static bool nir_cmp_same_value(ny_nir_cmp_t cmp, int64_t *out) {
  if (!out)
    return false;
  switch (cmp) {
  case NY_NIR_CMP_EQ:
  case NY_NIR_CMP_LE:
  case NY_NIR_CMP_GE:
    *out = 1;
    return true;
  case NY_NIR_CMP_NE:
  case NY_NIR_CMP_LT:
  case NY_NIR_CMP_GT:
    *out = 0;
    return true;
  }
  return false;
}

static bool nir_cmp_range_fold(ny_nir_cmp_t cmp, const ny_nir_range_t *a,
                               const ny_nir_range_t *b, int64_t *out) {
  if (!a || !b || !out || !a->has_min || !a->has_max || !b->has_min ||
      !b->has_max)
    return false;
  bool disjoint = a->max < b->min || b->max < a->min;
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    if (disjoint) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_NE:
    if (disjoint) {
      *out = 1;
      return true;
    }
    return false;
  case NY_NIR_CMP_LT:
    if (a->max < b->min) {
      *out = 1;
      return true;
    }
    if (a->min >= b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_LE:
    if (a->max <= b->min) {
      *out = 1;
      return true;
    }
    if (a->min > b->max) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_GT:
    if (a->min > b->max) {
      *out = 1;
      return true;
    }
    if (a->max <= b->min) {
      *out = 0;
      return true;
    }
    return false;
  case NY_NIR_CMP_GE:
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

bool ny_nir_peephole(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  ny_nir_value_fact_t *facts =
      (ny_nir_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !facts) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  if (!nir_collect_consts(f, known, value) ||
      !ny_nir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    bool ak = known[in->a];
    bool bk = known[in->b];
    int64_t av = ak ? value[in->a] : 0;
    int64_t bv = bk ? value[in->b] : 0;
    switch (in->op) {
    case NY_NIR_ADD_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_SUB_I64:
      if (in->a == in->b)
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      break;
    case NY_NIR_MUL_I64:
      if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 1)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_DIV_I64:
      if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_MOD_I64:
      if (bk && (bv == 1 || bv == -1))
        nir_make_const(in, 0);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_AND_I64:
      if (in->a == in->b)
        nir_make_copy(in, in->a);
      else if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == -1)
        nir_make_copy(in, in->a);
      else if (ak && av == -1)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_OR_I64:
      if (in->a == in->b)
        nir_make_copy(in, in->a);
      else if ((bk && bv == -1) || (ak && av == -1))
        nir_make_const(in, -1);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_XOR_I64:
      if (in->a == in->b)
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      break;
    case NY_NIR_SHL_I64:
    case NY_NIR_SAR_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_const(in, 0);
      break;
    case NY_NIR_CMP_I64: {
      int64_t folded = 0;
      if (in->a == in->b) {
        if (nir_cmp_same_value(in->cmp, &folded))
          nir_make_const(in, folded);
      } else if (in->a >= 0 && in->b >= 0 &&
                 nir_cmp_range_fold(in->cmp, &facts[in->a].range,
                                    &facts[in->b].range, &folded)) {
        nir_make_const(in, folded);
      }
      break;
    }
    default:
      break;
    }
  }
  free(known);
  free(value);
  free(facts);
  return true;
}

static size_t nir_next_non_nop(const ny_nir_func_t *f, size_t start) {
  if (!f)
    return 0;
  for (size_t i = start; i < f->len; ++i) {
    if (f->data[i].op != NY_NIR_NOP)
      return i;
  }
  return f->len;
}

bool ny_nir_cfg_simplify(ny_nir_func_t *f) {
  if (!f)
    return true;

  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return false;
    }
    if (!nir_collect_consts(f, known, value)) {
      free(known);
      free(value);
      return false;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_BR_IF && in->a >= 0 && known && known[in->a]) {
      int64_t target = in->imm;
      *in = value[in->a] != 0
                ? (ny_nir_inst_t){.op = NY_NIR_BR,
                                  .dst = -1,
                                  .a = -1,
                                  .b = -1,
                                  .imm = target}
                : (ny_nir_inst_t){.op = NY_NIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
    if (in->op != NY_NIR_BR)
      continue;
    size_t next = nir_next_non_nop(f, i + 1);
    if (next < f->len && f->data[next].op == NY_NIR_LABEL &&
        f->data[next].imm == in->imm) {
      *in = (ny_nir_inst_t){.op = NY_NIR_NOP, .dst = -1, .a = -1, .b = -1};
    }
  }
  free(known);
  free(value);
  return true;
}

bool ny_nir_dce(ny_nir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *used = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  if (!used)
    return false;

  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const ny_nir_inst_t *in = &f->data[i];
    if ((in->op == NY_NIR_LABEL || in->op == NY_NIR_BR ||
         in->op == NY_NIR_BR_IF) &&
        in->imm >= 0 && in->imm > max_label)
      max_label = in->imm;
  }
  bool *label_referenced = NULL;
  if (max_label >= 0 && (uint64_t)max_label <= (uint64_t)f->len * 4u + 1024u) {
    label_referenced =
        (bool *)calloc((size_t)max_label + 1u, sizeof(*label_referenced));
    if (!label_referenced) {
      free(used);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i) {
      const ny_nir_inst_t *in = &f->data[i];
      if ((in->op == NY_NIR_BR || in->op == NY_NIR_BR_IF) && in->imm >= 0 &&
          in->imm <= max_label)
        label_referenced[in->imm] = true;
    }
  }
#define NIR_LABEL_REFERENCED(label)                                            \
  (label_referenced && (label) >= 0 && (label) <= max_label                    \
       ? label_referenced[(size_t)(label)]                                     \
       : ny_nir_label_referenced(f, (label)))

  bool reachable = true;
  bool fallthrough = true;
  bool first_inst = true;
  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (in->op == NY_NIR_LABEL) {
      reachable = first_inst || fallthrough || NIR_LABEL_REFERENCED(in->imm);
      fallthrough = reachable;
    }
    if (!reachable && in->op != NY_NIR_LABEL) {
      ny_nir_inst_discard(in);
      first_inst = false;
      continue;
    }
    if (in->op == NY_NIR_RET || in->op == NY_NIR_BR) {
      reachable = false;
      fallthrough = false;
    } else if (in->op != NY_NIR_NOP) {
      fallthrough = reachable;
    }
    first_inst = false;
  }

  for (size_t i = f->len; i > 0; --i) {
    ny_nir_inst_t *in = &f->data[i - 1];
    bool side_effect = in->effects != NY_NIR_EFFECT_NONE ||
                       in->op == NY_NIR_RET || in->op == NY_NIR_BR ||
                       in->op == NY_NIR_BR_IF;
    if (in->op == NY_NIR_LABEL)
      side_effect = NIR_LABEL_REFERENCED(in->imm);
    bool keep = side_effect || (in->dst >= 0 && used[in->dst]);
    if (!keep) {
      ny_nir_inst_discard(in);
      continue;
    }
    if (in->a >= 0)
      used[in->a] = true;
    if (in->b >= 0)
      used[in->b] = true;
    if (in->c >= 0)
      used[in->c] = true;
    if (in->d >= 0)
      used[in->d] = true;
    if (in->e >= 0)
      used[in->e] = true;
    if (in->f >= 0)
      used[in->f] = true;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        used[in->extra_args[k]] = true;
    }
  }
  free(label_referenced);
  free(used);
#undef NIR_LABEL_REFERENCED
  return true;
}

static bool nir_remap_value(const int *map, int map_len, int value, int *out) {
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

bool ny_nir_compact(ny_nir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  size_t out = 0;
  for (size_t i = 0; i < f->len; ++i) {
    if (f->data[i].op == NY_NIR_NOP)
      continue;
    if (out != i)
      f->data[out] = f->data[i];
    out++;
  }
  f->len = out;

  if (f->next_value <= 0)
    return true;

  int *map = (int *)malloc((size_t)f->next_value * sizeof(*map));
  if (!map)
    return false;
  for (int i = 0; i < f->next_value; ++i)
    map[i] = -1;

  int next = 0;
  for (size_t i = 0; i < f->len; ++i) {
    int dst = f->data[i].dst;
    if (dst >= 0) {
      if (dst >= f->next_value) {
        free(map);
        return false;
      }
      map[dst] = next++;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    ny_nir_inst_t *in = &f->data[i];
    if (!nir_remap_value(map, f->next_value, in->dst, &in->dst) ||
        !nir_remap_value(map, f->next_value, in->a, &in->a) ||
        !nir_remap_value(map, f->next_value, in->b, &in->b) ||
        !nir_remap_value(map, f->next_value, in->c, &in->c) ||
        !nir_remap_value(map, f->next_value, in->d, &in->d) ||
        !nir_remap_value(map, f->next_value, in->e, &in->e) ||
        !nir_remap_value(map, f->next_value, in->f, &in->f)) {
      free(map);
      return false;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (!nir_remap_value(map, f->next_value, in->extra_args[k],
                           &in->extra_args[k])) {
        free(map);
        return false;
      }
    }
  }
  f->next_value = next;
  free(map);
  return true;
}

static bool timed_pass(ny_nir_func_t *f, bool (*pass)(ny_nir_func_t *),
                       double *out_ms) {
  ny_tick_t t0 = ny_ticks_now();
  bool ok = pass(f);
  if (ok)
    ny_nir_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  return ok;
}

static const char *pass_names[9] = {
    "const_fold (1)", "peephole",    "copy_prop", "const_fold (2)",
    "cfg_simplify",   "dce",         "cfg_simplify (2)", "compact",
    "total",
};

const char *ny_nir_opt_pass_name(int pass) {
  if (pass < 0 || pass >= 9)
    return "?";
  return pass_names[pass];
}

bool ny_nir_optimize_with_stats(ny_nir_func_t *f, ny_nir_opt_stats_t *stats) {
  if (stats) {
    memset(stats, 0, sizeof(*stats));
    ny_nir_collect_stats(f, &stats->before_insts, &stats->before_values,
                         stats->before_ops, NYIR_OP_COUNT);
  }
  double *t = stats ? stats->pass_time_ms : NULL;
  bool ok =
      timed_pass(f, ny_nir_const_fold, t ? &t[0] : NULL) &&
      timed_pass(f, ny_nir_peephole, t ? &t[1] : NULL) &&
      timed_pass(f, ny_nir_copy_prop, t ? &t[2] : NULL) &&
      timed_pass(f, ny_nir_const_fold, t ? &t[3] : NULL) &&
      timed_pass(f, ny_nir_cfg_simplify, t ? &t[4] : NULL) &&
      timed_pass(f, ny_nir_dce, t ? &t[5] : NULL) &&
      timed_pass(f, ny_nir_cfg_simplify, t ? &t[6] : NULL) &&
      timed_pass(f, ny_nir_compact, t ? &t[7] : NULL);
  if (stats) {
    ny_nir_collect_stats(f, &stats->after_insts, &stats->after_values,
                         stats->after_ops, NYIR_OP_COUNT);
    stats->pass_time_ms[8] = 0;
    for (int i = 0; i < 8; ++i)
      stats->pass_time_ms[8] += stats->pass_time_ms[i];
  }
  if (ok)
    ny_nir_refresh_metadata(f);
  if (verbose_enabled >= 1 && stats && stats->pass_time_ms[8] > 0.001) {
    size_t removed = stats->before_insts - stats->after_insts;
    double pct = stats->before_insts > 0
                     ? 100.0 * (double)removed / stats->before_insts
                     : 0.0;
    fprintf(stderr, "nyir opt: %zu->%zu insts (-%zu, %.1f%%) in %.2fms",
            stats->before_insts, stats->after_insts, removed, pct,
            stats->pass_time_ms[8]);
    if (verbose_enabled >= 2) {
      for (int i = 0; i < 8; ++i)
        fprintf(stderr, " %s=%.2fms", pass_names[i], stats->pass_time_ms[i]);
    }
    fputc('\n', stderr);
  }
  return ok;
}

bool ny_nir_optimize(ny_nir_func_t *f) {
  return ny_nir_optimize_with_stats(f, NULL);
}

static bool timed_pass_verified(ny_nir_func_t *f, bool (*pass)(ny_nir_func_t *),
                                 double *out_ms, FILE *dump, int pass_idx,
                                 bool *ok) {
  ny_tick_t t0 = ny_ticks_now();
  *ok = pass(f);
  if (*ok)
    ny_nir_refresh_metadata(f);
  if (out_ms)
    *out_ms = ny_ticks_elapsed_ms(t0);
  if (dump && f->len > 0) {
    char name[64];
    snprintf(name, sizeof(name), "<after-%s>", ny_nir_opt_pass_name(pass_idx));
    ny_nir_dump(dump, f, name);
  }
  if (*ok) {
    char vbuf[256] = {0};
    if (!ny_nir_verify(f, vbuf, sizeof(vbuf))) {
      if (dump)
        fprintf(dump, "nyir verify FAILED after %s: %s\n",
                ny_nir_opt_pass_name(pass_idx), vbuf);
      *ok = false;
    }
  }
  return *ok;
}

bool ny_nir_optimize_debug(ny_nir_func_t *f, FILE *dump,
                           ny_nir_opt_stats_t *stats) {
  if (!dump)
    dump = stderr;

  /* Dump before first pass. */
  if (f->len > 0)
    ny_nir_dump(dump, f, "<before-optimize>");

  /* Run with per-pass timing, dump, and verify checkpoints. */
  double *t = stats ? stats->pass_time_ms : NULL;
  bool ok = true;
  ok = ok && timed_pass_verified(f, ny_nir_const_fold, t ? &t[0] : NULL,
                                  dump, 0, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_peephole, t ? &t[1] : NULL,
                                  dump, 1, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_copy_prop, t ? &t[2] : NULL,
                                  dump, 2, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_const_fold, t ? &t[3] : NULL,
                                  dump, 3, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_cfg_simplify, t ? &t[4] : NULL,
                                  dump, 4, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_dce, t ? &t[5] : NULL,
                                  dump, 5, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_cfg_simplify, t ? &t[6] : NULL,
                                  dump, 6, &ok);
  ok = ok && timed_pass_verified(f, ny_nir_compact, t ? &t[7] : NULL,
                                  dump, 7, &ok);
  if (stats) {
    if (!t) {
      /* timing was not collected during passes due to NULL stats; collect now */
      t = stats->pass_time_ms;
      memset(stats, 0, sizeof(*stats));
      ny_nir_collect_stats(f, &stats->after_insts, &stats->after_values,
                           stats->after_ops, NYIR_OP_COUNT);
    }
    stats->pass_time_ms[8] = 0;
    for (int i = 0; i < 8; ++i)
      stats->pass_time_ms[8] += stats->pass_time_ms[i];
  }

  if (ok)
    ny_nir_refresh_metadata(f);

  /* Print pass timings. */
  if (stats && stats->pass_time_ms[8] > 0.001) {
    fprintf(dump, "nyir pass timing:");
    for (int i = 0; i < 9; ++i) {
      if (i < 8 || stats->pass_time_ms[i] > 0.001)
        fprintf(dump, " %s=%.2fms", ny_nir_opt_pass_name(i),
                stats->pass_time_ms[i]);
    }
    fputc('\n', dump);
  }

  return ok;
}
