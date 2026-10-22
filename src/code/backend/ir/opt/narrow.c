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
