/*
 * Range-based i32 narrowing pass: identifies 64-bit integer operations
 * whose operands and results are provably within [INT32_MIN, INT32_MAX]
 * using value facts, enabling machine codegen to emit 32-bit register forms.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool in_i32_range(const nyir_range_t *r) {
  if (!r || !r->has_min || !r->has_max)
    return false;
  return r->min >= (int64_t)INT32_MIN && r->max <= (int64_t)INT32_MAX;
}

/*
 * Stricter than in_i32_range: non-negative and within [0, INT32_MAX]. This
 * is the range NYIR_INST_F_NARROW32 requires -- see its definition for why
 * a merely-signed-i32-safe range isn't enough.
 */
static bool in_u32_safe_range(const nyir_range_t *r) {
  if (!r || !r->has_min || !r->has_max)
    return false;
  return r->min >= 0 && r->max <= (int64_t)INT32_MAX;
}

static bool narrow32_op(nyir_op_t op) {
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

bool nyir_narrow(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  nyir_value_fact_t *facts =
      (nyir_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!facts)
    return false;

  if (!nyir_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(facts);
    return false;
  }

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= f->next_value)
      continue;

    /*
     * Check if operands and destination fit in i32 range
     */
    if (in->a >= 0 && in->a < f->next_value && !in_i32_range(&facts[in->a].range))
      continue;
    if (in->b >= 0 && in->b < f->next_value && !in_i32_range(&facts[in->b].range))
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
    if (narrow32_op(in->op) &&
        (in->a < 0 || in->a >= f->next_value || in_u32_safe_range(&facts[in->a].range)) &&
        (in->b < 0 || in->b >= f->next_value || in_u32_safe_range(&facts[in->b].range)) &&
        in_u32_safe_range(&facts[in->dst].range))
      in->flags |= NYIR_INST_F_NARROW32;
  }

  free(facts);
  return true;
}
