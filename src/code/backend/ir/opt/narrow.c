/*
 * Range-based i32 narrowing pass: identifies 64-bit integer operations
 * whose operands and results are provably representable by 32-bit register
 * forms using value facts.
 */
#include "base/common.h"
#include "base/compat.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool in_i32_range(const nyr_range_t *r) {
  if (!r || !r->has_min || !r->has_max)
    return false;
  return r->min >= (int64_t)INT32_MIN && r->max <= (int64_t)INT32_MAX;
}

/*
 * Non-negative and within the full unsigned 32-bit range. This
 * is the range NYIR_INST_F_NARROW32 requires -- see its definition for why
 * signed-only ranges that include negatives aren't enough.
 */
static bool in_u32_safe_range(const nyr_range_t *r) {
  if (!r || !r->has_min || !r->has_max)
    return false;
  return r->min >= 0 && r->max <= (int64_t)UINT32_MAX;
}

static bool is_u32_mask_value(const int64_t *constants,
                              const unsigned char *is_constant, int value,
                              int next_value) {
  return constants && is_constant && value >= 0 && value < next_value &&
         is_constant[value] && constants[value] == (int64_t)UINT32_MAX;
}

static void count_uses(const nyr_inst_t *in, size_t num_values, size_t *uses) {
  const int scalar[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t i = 0; i < sizeof(scalar) / sizeof(scalar[0]); ++i)
    if (scalar[i] >= 0 && (size_t)scalar[i] < num_values)
      uses[scalar[i]]++;
  for (size_t i = 0; i < in->extra_args_len; ++i)
    if (in->extra_args[i] >= 0 && (size_t)in->extra_args[i] < num_values)
      uses[in->extra_args[i]]++;
  for (size_t i = 0; i < in->phi_incoming_len; ++i)
    if (in->phi_incoming[i].value >= 0 &&
        (size_t)in->phi_incoming[i].value < num_values)
      uses[in->phi_incoming[i].value]++;
}

static bool narrow32_op(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    return true;
  default:
    return false;
  }
}

/*
 * Known-width simplification for values that carry no runtime tag. C lowering
 * re-normalizes after every operation, so most masks and sign extensions are
 * redundant. w[v] bounds v to [0, 2^w) (64 means unknown); PHIs start empty
 * and only ever grow, so the fixed point is sound across loops.
 */
static void bits_widen_chain(nyr_func_t *f, const int *defs, int value,
                             unsigned depth) {
  if (value < 0 || value >= f->next_value || defs[value] < 0 || depth > 32)
    return;
  nyr_inst_t *in = &f->data[defs[value]];
  if (!(in->flags & NYIR_INST_F_NARROW32) || !narrow32_op(in->op))
    return;
  in->flags &= ~NYIR_INST_F_NARROW32;
  bits_widen_chain(f, defs, in->a, depth + 1);
  bits_widen_chain(f, defs, in->b, depth + 1);
}

static unsigned bits_of_const(int64_t value) {
  if (value < 0)
    return 64;
  return value ? 64u - (unsigned)__builtin_clzll((uint64_t)value) : 0u;
}

static unsigned bits_width_of(const nyr_func_t *f, const nyr_inst_t *in,
                              const unsigned char *w, const int64_t *constants,
                              const unsigned char *is_constant,
                              const unsigned char *iv_bits, const int *iv_init) {
  size_t nv = (size_t)f->next_value;
#define BITS_W(x) ((x) < 0 || (size_t)(x) >= nv ? 64u : (unsigned)w[x])
#define BITS_K(x) ((x) >= 0 && (size_t)(x) < nv && is_constant[x])
  unsigned a = BITS_W(in->a), b = BITS_W(in->b);
  switch (in->op) {
  case NYIR_CONST_I64:
    return bits_of_const(in->imm);
  case NYIR_COPY:
    return a;
  case NYIR_PHI: {
    if (in->dst >= 0 && (size_t)in->dst < nv && iv_bits[in->dst]) {
      unsigned start = BITS_W(iv_init[in->dst]);
      return start > iv_bits[in->dst] ? start : iv_bits[in->dst];
    }
    unsigned m = 0;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      unsigned v = BITS_W(in->phi_incoming[k].value);
      m = v > m ? v : m;
    }
    return m;
  }
  case NYIR_AND_I64:
    return a < b ? a : b;
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    return a > b ? a : b;
  case NYIR_ADD_I64: {
    unsigned m = a > b ? a : b;
    return m >= 63 ? 64u : m + 1;
  }
  case NYIR_MUL_I64:
    return a + b >= 64 ? 64u : a + b;
  case NYIR_SHL_I64:
    if (BITS_K(in->b) && constants[in->b] >= 0 && constants[in->b] < 64 &&
        a + (unsigned)constants[in->b] < 64)
      return a + (unsigned)constants[in->b];
    return 64;
  case NYIR_SAR_I64:
    if (a < 64 && BITS_K(in->b) && constants[in->b] >= 0 &&
        constants[in->b] < 64)
      return a > (unsigned)constants[in->b] ? a - (unsigned)constants[in->b]
                                            : 0u;
    return 64;
  case NYIR_ROR32_I64:
    return 32;
  case NYIR_CMP_I64:
  case NYIR_CMP_F64:
  case NYIR_CMP_F32:
    return 1;
  case NYIR_LOAD_I64:
    if (in->flags & NYIR_INST_F_MEM_BYTE)
      return 8;
    if (in->flags & NYIR_INST_F_MEM_U16)
      return 16;
    return 64;
  default:
    return 64;
  }
#undef BITS_W
#undef BITS_K
}

/*
 * x + 1 cannot overflow i32 when x is a loop PHI and the header's taken edge,
 * the only way into the block holding the add, already proved x < some i32.
 */
static bool bits_guarded_iv_add(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                const int *defs, const unsigned char *in32,
                                const unsigned char *is_constant,
                                const int64_t *constants, size_t at) {
  const nyr_inst_t *add = &f->data[at];
  size_t nv = (size_t)f->next_value;
  for (int side = 0; side < 2; ++side) {
    int iv = side ? add->b : add->a, step = side ? add->a : add->b;
    if (iv < 0 || (size_t)iv >= nv || step < 0 || (size_t)step >= nv ||
        !is_constant[step] || constants[step] != 1 || defs[iv] < 0)
      continue;
    const nyr_inst_t *phi = &f->data[defs[iv]];
    if (phi->op != NYIR_PHI)
      continue;
    size_t header = cfg->inst_block[(size_t)defs[iv]];
    size_t end = cfg->block_start[header];
    while (end < f->len && cfg->inst_block[end] == header)
      ++end;
    for (size_t i = cfg->block_start[header]; i < end; ++i) {
      const nyr_inst_t *br = &f->data[i];
      if (br->op != NYIR_BR_IF || br->a < 0 || (size_t)br->a >= nv ||
          defs[br->a] < 0)
        continue;
      const nyr_inst_t *cmp = &f->data[defs[br->a]];
      if (cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT || cmp->a != iv ||
          cmp->b < 0 || (size_t)cmp->b >= nv || !in32[cmp->b])
        continue;
      for (size_t b = 0; b < cfg->block_count; ++b) {
        if (cfg->block_label[b] != br->imm)
          continue;
        if (cfg->pred_offsets[b + 1] - cfg->pred_offsets[b] == 1 &&
            cfg->pred_blocks[cfg->pred_offsets[b]] == header &&
            nyr_cfg_dominates(cfg, b, cfg->inst_block[at]))
          return true;
      }
    }
  }
  return false;
}

/*
 * x - 1 cannot underflow i32 when x is a loop PHI and the only way into the
 * block holding the subtraction went through `x >= K` with K above INT32_MIN.
 */
static bool bits_guarded_iv_sub(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                const int *defs,
                                const unsigned char *is_constant,
                                const int64_t *constants, size_t at) {
  const nyr_inst_t *sub = &f->data[at];
  size_t nv = (size_t)f->next_value;
  int iv = sub->a, step = sub->b;
  if (iv < 0 || (size_t)iv >= nv || step < 0 || (size_t)step >= nv ||
      !is_constant[step] || constants[step] != 1 || defs[iv] < 0 ||
      f->data[defs[iv]].op != NYIR_PHI)
    return false;
  size_t header = cfg->inst_block[(size_t)defs[iv]];
  size_t end = cfg->block_start[header];
  while (end < f->len && cfg->inst_block[end] == header)
    ++end;
  for (size_t i = cfg->block_start[header]; i < end; ++i) {
    const nyr_inst_t *br = &f->data[i];
    if (br->op != NYIR_BR_IF || br->a < 0 || (size_t)br->a >= nv ||
        defs[br->a] < 0)
      continue;
    const nyr_inst_t *cmp = &f->data[defs[br->a]];
    if (cmp->op != NYIR_CMP_I64 ||
        (cmp->cmp != NYIR_CMP_GE && cmp->cmp != NYIR_CMP_GT) || cmp->a != iv ||
        cmp->b < 0 || (size_t)cmp->b >= nv || !is_constant[cmp->b] ||
        constants[cmp->b] <= INT32_MIN || constants[cmp->b] > INT32_MAX)
      continue;
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (cfg->block_label[b] != br->imm)
        continue;
      if (cfg->pred_offsets[b + 1] - cfg->pred_offsets[b] == 1 &&
          cfg->pred_blocks[cfg->pred_offsets[b]] == header &&
          nyr_cfg_dominates(cfg, b, cfg->inst_block[at]))
        return true;
    }
  }
  return false;
}

/*
 * A counter PHI of `init` and `iv + step` whose only path to that add runs
 * through the header's `iv < K` edge stays within [init, K + step - 1]. On
 * success, reports the bit width of that upper bound and the init value.
 */
static bool bits_bounded_iv(const nyr_func_t *f, const nyr_cfg_t *cfg,
                            const int *defs, const unsigned char *is_constant,
                            const int64_t *constants, size_t at,
                            unsigned *bits_out, int *init_out) {
  const nyr_inst_t *phi = &f->data[at];
  size_t nv = (size_t)f->next_value;
  if (phi->op != NYIR_PHI || phi->phi_incoming_len != 2 || phi->dst < 0)
    return false;
  size_t header = cfg->inst_block[at];
  size_t end = cfg->block_start[header];
  while (end < f->len && cfg->inst_block[end] == header)
    ++end;
  for (int side = 0; side < 2; ++side) {
    int update = phi->phi_incoming[side].value;
    int init = phi->phi_incoming[1 - side].value;
    if (update < 0 || (size_t)update >= nv || init < 0 ||
        (size_t)init >= nv || defs[update] < 0)
      continue;
    const nyr_inst_t *add = &f->data[defs[update]];
    /*
     * C re-normalizes the sum to int. The bound below stays under 2^31, so
     * that extension is an identity and the add underneath is what counts.
     */
    bool extended = false;
    if (add->op == NYIR_SAR_I64 && add->a >= 0 && (size_t)add->a < nv &&
        defs[add->a] >= 0 && add->b >= 0 && (size_t)add->b < nv &&
        is_constant[add->b] && constants[add->b] == 32) {
      const nyr_inst_t *shl = &f->data[defs[add->a]];
      if (shl->op == NYIR_SHL_I64 && shl->b >= 0 && (size_t)shl->b < nv &&
          is_constant[shl->b] && constants[shl->b] == 32 && shl->a >= 0 &&
          (size_t)shl->a < nv && defs[shl->a] >= 0) {
        add = &f->data[defs[shl->a]];
        extended = true;
      }
    }
    if (add->op != NYIR_ADD_I64)
      continue;
    int step = add->a == phi->dst ? add->b : add->b == phi->dst ? add->a : -1;
    if (step < 0 || (size_t)step >= nv || !is_constant[step] ||
        constants[step] <= 0 || constants[step] > INT32_MAX)
      continue;
    for (size_t i = cfg->block_start[header]; i < end; ++i) {
      const nyr_inst_t *br = &f->data[i];
      if (br->op != NYIR_BR_IF || br->a < 0 || (size_t)br->a >= nv ||
          defs[br->a] < 0)
        continue;
      const nyr_inst_t *cmp = &f->data[defs[br->a]];
      if (cmp->op != NYIR_CMP_I64 ||
          (cmp->cmp != NYIR_CMP_LT && cmp->cmp != NYIR_CMP_LE) ||
          cmp->a != phi->dst || cmp->b < 0 || (size_t)cmp->b >= nv ||
          !is_constant[cmp->b] || constants[cmp->b] < 0 ||
          constants[cmp->b] > INT32_MAX)
        continue;
      for (size_t b = 0; b < cfg->block_count; ++b) {
        if (cfg->block_label[b] != br->imm)
          continue;
        if (cfg->pred_offsets[b + 1] - cfg->pred_offsets[b] != 1 ||
            cfg->pred_blocks[cfg->pred_offsets[b]] != header ||
            !nyr_cfg_dominates(cfg, b, cfg->inst_block[(size_t)defs[update]]))
          continue;
        int64_t top = constants[cmp->b] + constants[step] -
                      (cmp->cmp == NYIR_CMP_LT ? 1 : 0);
        if (extended && top > INT32_MAX)
          continue;
        *bits_out = bits_of_const(top);
        *init_out = init;
        return *bits_out != 0 && *bits_out < 64;
      }
    }
  }
  return false;
}

void nyr_bits_simplify(nyr_func_t *f) {
  if (!f || f->next_value <= 0 || !f->len)
    return;
  size_t nv = (size_t)f->next_value;
  unsigned char *w = malloc(nv);
  unsigned char *in32 = malloc(nv);
  nyr_cfg_t cfg = {0};
  bool have_cfg = nyr_has_control_flow(f) && nyr_cfg_build(f, &cfg);
  int64_t *constants = calloc(nv, sizeof(*constants));
  unsigned char *is_constant = calloc(nv, 1);
  int *defs = malloc(nv * sizeof(*defs));
  nyr_value_fact_t *facts = calloc(nv, sizeof(*facts));
  unsigned char *iv_bits = calloc(nv, 1);
  int *iv_init = calloc(nv, sizeof(*iv_init));
  bool have_facts = false;
  if (!w || !in32 || !constants || !is_constant || !defs || !facts ||
      !iv_bits || !iv_init)
    goto done;
  memset(w, 64, nv);
  memset(defs, -1, nv * sizeof(*defs));
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || (size_t)in->dst >= nv)
      continue;
    w[in->dst] = 0;
    defs[in->dst] = (int)i;
    if (in->op == NYIR_CONST_I64) {
      constants[in->dst] = in->imm;
      is_constant[in->dst] = 1;
    }
  }
  for (size_t i = 0; have_cfg && i < f->len; ++i) {
    unsigned bits = 0;
    int init = -1;
    if (cfg.reachable[cfg.inst_block[i]] &&
        bits_bounded_iv(f, &cfg, defs, is_constant, constants, i, &bits,
                        &init)) {
      iv_bits[f->data[i].dst] = (unsigned char)bits;
      iv_init[f->data[i].dst] = init;
    }
  }
  bool converged = false;
  /*
   * A loop counter grows one bit per round, so widen it after a few.
   */
  for (unsigned round = 0; round < 64 && !converged; ++round) {
    converged = true;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= nv)
        continue;
      unsigned value =
          bits_width_of(f, in, w, constants, is_constant, iv_bits, iv_init);
      if (value > w[in->dst]) {
        w[in->dst] = (unsigned char)(round >= 3 ? 64u : value);
        converged = false;
      }
    }
  }
  if (!converged)
    goto done;
  have_facts = nyr_analyze_values(f, facts, nv, NULL, 0);
  /*
   * in32[v]: v is exactly its own sign-extended low word. Optimistic over
   * PHIs, so a loop counter holds as long as every incoming value does.
   */
  memset(in32, 0, nv);
#define BITS_IS_SEXT(in)                                                       \
  ((in)->op == NYIR_SAR_I64 && BITS_CONST((in)->b, 32) && (in)->a >= 0 &&      \
   defs[(in)->a] >= 0 && f->data[defs[(in)->a]].op == NYIR_SHL_I64 &&          \
   BITS_CONST(f->data[defs[(in)->a]].b, 32))
#define BITS_CONST(x, c) ((x) >= 0 && (size_t)(x) < nv && is_constant[x] &&     \
                          constants[x] == (c))
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].dst >= 0 && (size_t)f->data[i].dst < nv)
      in32[f->data[i].dst] = 1;
  bool in32_changed = true;
  for (unsigned round = 0; round < 32 && in32_changed; ++round) {
    in32_changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->dst < 0 || (size_t)in->dst >= nv || !in32[in->dst])
        continue;
      bool value = w[in->dst] <= 31 || BITS_IS_SEXT(in);
      if (!value && in->op == NYIR_CONST_I64)
        value = in->imm >= INT32_MIN && in->imm <= INT32_MAX;
      else if (!value && in->op == NYIR_COPY)
        value = in->a >= 0 && (size_t)in->a < nv && in32[in->a];
      else if (!value && in->op == NYIR_PHI) {
        value = in->phi_incoming_len > 0;
        for (size_t k = 0; value && k < in->phi_incoming_len; ++k) {
          int v = in->phi_incoming[k].value;
          value = v >= 0 && (size_t)v < nv && in32[v];
        }
      } else if (!value && in->op == NYIR_ADD_I64 && have_cfg)
        value = bits_guarded_iv_add(f, &cfg, defs, in32, is_constant, constants,
                                    i);
      else if (!value && in->op == NYIR_SUB_I64 && have_cfg && in->a >= 0 &&
               (size_t)in->a < nv && in32[in->a])
        value = bits_guarded_iv_sub(f, &cfg, defs, is_constant, constants, i);
      if (!value) {
        in32[in->dst] = 0;
        in32_changed = true;
      }
    }
  }
  if (in32_changed)
    memset(in32, 0, nv);
#undef BITS_CONST
#define BITS_DEF(x)                                                            \
  ((x) >= 0 && (size_t)(x) < nv && defs[x] >= 0 ? &f->data[defs[x]] : NULL)
#define BITS_CONST(x, c) ((x) >= 0 && (size_t)(x) < nv && is_constant[x] &&     \
                          constants[x] == (c))
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0)
      continue;
    if (in->op == NYIR_AND_I64) {
      for (int side = 0; side < 2; ++side) {
        int mask = side ? in->a : in->b, data = side ? in->b : in->a;
        if (mask < 0 || (size_t)mask >= nv || !is_constant[mask] ||
            data < 0 || (size_t)data >= nv)
          continue;
        int64_t m = constants[mask];
        if (m <= 0 || (m & (m + 1)) != 0 || w[data] > bits_of_const(m))
          continue;
        nir_make_copy(in, data);
        break;
      }
    } else if (in->op == NYIR_SAR_I64 && BITS_CONST(in->b, 32)) {
      const nyr_inst_t *shl = BITS_DEF(in->a);
      if (shl && shl->op == NYIR_SHL_I64 && BITS_CONST(shl->b, 32) &&
          shl->a >= 0 && (size_t)shl->a < nv &&
          (in32[shl->a] || w[shl->a] <= 31 ||
           (have_facts && in_i32_range(&facts[shl->a].range)))) {
        /*
         * The low-word-only computation now has to produce the full value.
         */
        int source = shl->a;
        bits_widen_chain(f, defs, source, 0);
        nir_make_copy(in, source);
      }
    } else if (in->op == NYIR_OR_I64) {
      for (int side = 0; side < 2; ++side) {
        const nyr_inst_t *sar = BITS_DEF(side ? in->b : in->a);
        const nyr_inst_t *hi = BITS_DEF(side ? in->a : in->b);
        if (!sar || !hi || sar->op != NYIR_SAR_I64 || sar->a < 0 ||
            (size_t)sar->a >= nv || sar->b < 0 || (size_t)sar->b >= nv ||
            !is_constant[sar->b] || w[sar->a] > 32)
          continue;
        int64_t n = constants[sar->b];
        if (n < 1 || n > 31)
          continue;
        if (hi->op == NYIR_AND_I64) {
          const nyr_inst_t *inner = BITS_DEF(hi->a);
          int mask = hi->b;
          if (!inner || inner->op != NYIR_SHL_I64) {
            inner = BITS_DEF(hi->b);
            mask = hi->a;
          }
          if (!inner || inner->op != NYIR_SHL_I64 ||
              !BITS_CONST(mask, INT64_C(0xffffffff)))
            continue;
          hi = inner;
        } else {
          continue;
        }
        if (hi->a != sar->a || !BITS_CONST(hi->b, 32 - n))
          continue;
        int x = sar->a, count = sar->b;
        in->op = NYIR_ROR32_I64;
        in->a = x;
        in->b = count;
        in->flags &= ~NYIR_INST_F_NARROW32;
        in->effects = NYIR_EFFECT_NONE;
        nyr_clear_derived_facts(in);
        break;
      }
    }
  }
#undef BITS_DEF
#undef BITS_CONST
#undef BITS_IS_SEXT
done:
  free(w);
  free(in32);
  if (have_cfg)
    nyr_cfg_free(&cfg);
  free(constants);
  free(is_constant);
  free(defs);
  free(facts);
  free(iv_bits);
  free(iv_init);
}

bool nyr_narrow(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  nyr_value_fact_t *facts =
      (nyr_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  int64_t *constants =
      (int64_t *)calloc((size_t)f->next_value, sizeof(*constants));
  unsigned char *is_constant =
      (unsigned char *)calloc((size_t)f->next_value, sizeof(*is_constant));
  size_t *uses = (size_t *)calloc((size_t)f->next_value, sizeof(*uses));
  size_t *low_uses = (size_t *)calloc((size_t)f->next_value, sizeof(*low_uses));
  unsigned char *low_needed =
      (unsigned char *)calloc((size_t)f->next_value, sizeof(*low_needed));
  if (!facts || !constants || !is_constant || !uses || !low_uses ||
      !low_needed) {
    free(facts);
    free(constants);
    free(is_constant);
    free(uses);
    free(low_uses);
    free(low_needed);
    return false;
  }

  if (!nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(facts);
    free(constants);
    free(is_constant);
    free(uses);
    free(low_uses);
    free(low_needed);
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < f->next_value && in->op == NYIR_CONST_I64) {
      constants[in->dst] = in->imm;
      is_constant[in->dst] = 1;
    }
    count_uses(in, (size_t)f->next_value, uses);
  }

  /*
   * A mask at a value boundary permits earlier add/sub/mul/bitwise nodes to
   * run modulo 2^32 when every use of each narrowed result remains inside the
   * same low-word expression. Walk backward through SSA definitions, stopping
   * at PHIs, calls, loads, and all other operations that are not modular ALU.
   * Values with an escaping wide use retain their 64-bit instruction.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_AND_I64)
      continue;
    bool mask_a =
        is_u32_mask_value(constants, is_constant, in->a, f->next_value);
    bool mask_b =
        is_u32_mask_value(constants, is_constant, in->b, f->next_value);
    if (mask_a == mask_b)
      continue;
    int data = mask_a ? in->b : in->a;
    if (data >= 0 && data < f->next_value) {
      low_needed[data] = 1;
      low_uses[data]++;
      in->flags |= NYIR_INST_F_NARROW32;
    }
  }
  for (size_t i = f->len; i > 0; --i) {
    nyr_inst_t *in = &f->data[i - 1];
    if (in->dst < 0 || in->dst >= f->next_value || !low_needed[in->dst] ||
        !narrow32_op(in->op) || uses[in->dst] != low_uses[in->dst])
      continue;
    in->flags |= NYIR_INST_F_NARROW32;
    if (in->a >= 0 && in->a < f->next_value) {
      low_needed[in->a] = 1;
      low_uses[in->a]++;
    }
    if (in->b >= 0 && in->b < f->next_value) {
      low_needed[in->b] = 1;
      low_uses[in->b]++;
    }
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value)
      continue;

    bool exact_u32_mask =
        in->op == NYIR_AND_I64 &&
        (is_u32_mask_value(constants, is_constant, in->a, f->next_value) ||
         is_u32_mask_value(constants, is_constant, in->b, f->next_value));

    /*
     * Check if operands and destination fit in i32 range
     */
    if (!exact_u32_mask && in->a >= 0 && in->a < f->next_value &&
        !in_i32_range(&facts[in->a].range) &&
        !in_u32_safe_range(&facts[in->a].range))
      continue;
    if (!exact_u32_mask && in->b >= 0 && in->b < f->next_value &&
        !in_i32_range(&facts[in->b].range) &&
        !in_u32_safe_range(&facts[in->b].range))
      continue;

    if (in_i32_range(&facts[in->dst].range)) {
      /*
       * Both operands and result are provably 32-bit. Annotate range metadata.
       */
      in->range.has_min = facts[in->dst].range.has_min;
      in->range.has_max = facts[in->dst].range.has_max;
      in->range.min = facts[in->dst].range.min;
      in->range.max = facts[in->dst].range.max;
    }

    /*
     * Non-negative-only subset: safe for the emitter to drop to 32-bit
     * register forms (see NYIR_INST_F_NARROW32). Re-checks operand facts
     * directly rather than in->range, since in->range above may hold a
     * signed-but-negative-capable range that this flag must not use.
     */
    bool operands_u32_safe = (in->a < 0 || in->a >= f->next_value ||
                              in_u32_safe_range(&facts[in->a].range)) &&
                             (in->b < 0 || in->b >= f->next_value ||
                              in_u32_safe_range(&facts[in->b].range));
    if (narrow32_op(in->op) &&
        (exact_u32_mask ||
         (operands_u32_safe && in_u32_safe_range(&facts[in->dst].range))))
      in->flags |= NYIR_INST_F_NARROW32;
  }

  free(facts);
  free(constants);
  free(is_constant);
  free(uses);
  free(low_uses);
  free(low_needed);
  return true;
}
