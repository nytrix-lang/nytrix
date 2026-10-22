/*
 * Strength reduction: replaces expensive operations (mul, div, mod
 * by constants) with cheaper equivalent sequences (shift, add, lea).
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool nyr_strength_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) || ny_trace_enabled("NY_TRACE_SR");
}

/*
 * Value analysis can conservatively drop a singleton after loop motion even
 * though SSA still has an explicit CONST/COPY definition.  Strength
 * reduction runs late enough that this is common for loop invariant
 * divisors.  Recover the syntactic constant instead of leaving an expensive
 * hardware divide behind.  SSA has one definition per value, so the bounded
 * walk is unambiguous and does not require dominance assumptions.
 */
static bool nyr_strength_recover_const(const nyr_func_t *f,
                                       const nyr_value_fact_t *facts,
                                       int fact_count, int value,
                                       int64_t *out) {
  for (int depth = 0;
       f && out && value >= 0 && value < f->next_value && depth < 32; ++depth) {
    if (value < fact_count && nyr_value_fact_singleton(&facts[value], out))
      return true;
    const nyr_inst_t *definition = NULL;
    for (size_t i = 0; i < f->len; ++i) {
      if (f->data[i].dst == value) {
        definition = &f->data[i];
        break;
      }
    }
    if (!definition)
      return false;
    if (definition->op == NYIR_CONST_I64) {
      *out = definition->imm;
      return true;
    }
    if (definition->op != NYIR_COPY)
      return false;
    value = definition->a;
  }
  return false;
}

typedef struct {
  nyr_scalar_class_t scalar_class;
  uint8_t semantic_ownership;
  bool semantic_mutable;
  nyr_debug_loc_t debug;
  nyr_range_t range;
} nyr_strength_result_metadata_t;

static nyr_strength_result_metadata_t
nyr_strength_result_metadata(const nyr_inst_t *in) {
  return (nyr_strength_result_metadata_t){
      .scalar_class = in->scalar_class,
      .semantic_ownership = in->semantic_ownership,
      .semantic_mutable = in->semantic_mutable,
      .debug = in->debug,
      .range = in->range};
}

static void
nyr_strength_apply_result_metadata(nyr_inst_t *in,
                                   nyr_strength_result_metadata_t metadata) {
  in->scalar_class = metadata.scalar_class;
  in->semantic_ownership = metadata.semantic_ownership;
  in->semantic_mutable = metadata.semantic_mutable;
  in->debug = metadata.debug;
  in->range = metadata.range;
  /*
   * The old opcode's alias fact is not a proof for its replacement.
   */
  in->alias_class = 0;
}

static int nyr_strength_root_copy(const nyr_func_t *f, const int *defs,
                                  int value) {
  for (int depth = 0; f && defs && value >= 0 && value < f->next_value &&
                      defs[value] >= 0 && (size_t)defs[value] < f->len &&
                      f->data[defs[value]].op == NYIR_COPY && depth < 32;
       ++depth)
    value = f->data[defs[value]].a;
  return value;
}

static bool nyr_strength_const_value(const nyr_func_t *f, const int *defs,
                                     const nyr_value_fact_t *facts, int value,
                                     int64_t *out) {
  if (!f || !defs || !facts || !out || value < 0 || value >= f->next_value)
    return false;
  value = nyr_strength_root_copy(f, defs, value);
  if (value < 0 || value >= f->next_value)
    return false;
  return nyr_value_fact_singleton(&facts[value], out);
}

static void nyr_strength_count_uses(const nyr_func_t *f, int *uses) {
  if (!f || !uses)
    return;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < sizeof(operands) / sizeof(operands[0]); ++k)
      if (operands[k] >= 0 && operands[k] < f->next_value)
        uses[operands[k]]++;
    if (in->op == NYIR_CALL && in->extra_args)
      for (size_t k = 0; k < in->extra_args_len; ++k)
        if (in->extra_args[k] >= 0 && in->extra_args[k] < f->next_value)
          uses[in->extra_args[k]]++;
    if (in->op == NYIR_PHI && in->phi_incoming)
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value >= 0 &&
            in->phi_incoming[k].value < f->next_value)
          uses[in->phi_incoming[k].value]++;
  }
}

/*
 * Prove that `value` reaches `root` only through single-use COPYs. This lets
 * the combiner replace a nested arithmetic chain without leaving its original
 * intermediate work live for another consumer.
 */
static bool nyr_strength_single_use_copy_path(const nyr_func_t *f,
                                              const int *defs, const int *uses,
                                              int value, int root) {
  if (!f || !defs || !uses || value < 0 || root < 0)
    return false;
  for (int depth = 0; depth < 32; ++depth) {
    if (value < 0 || value >= f->next_value || uses[value] != 1)
      return false;
    if (value == root)
      return true;
    int di = defs[value];
    if (di < 0 || (size_t)di >= f->len || f->data[di].dst != value ||
        f->data[di].op != NYIR_COPY)
      return false;
    value = f->data[di].a;
  }
  return false;
}

/*
 * Collapse `(x + c) * 2 + x` to `x * 3 + 2*c` when each intermediate has a
 * single consumer. All operations are wrapping i64 arithmetic, so the
 * identity holds modulo 2^64, including overflow. The smaller expression
 * gives instruction selection one multiply/add pair instead of three
 * arithmetic operations and avoids preserving the old intermediate homes.
 */
static bool nyr_strength_combine_scaled_add(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  size_t value_count = (size_t)f->next_value;
  int *defs = nyr_build_defs(f);
  int *uses = calloc(value_count, sizeof(*uses));
  nyr_value_fact_t *facts = calloc(value_count, sizeof(*facts));
  if (!defs || !uses || !facts ||
      !nyr_analyze_values(f, facts, value_count, NULL, 0)) {
    free(defs);
    free(uses);
    free(facts);
    return false;
  }
  nyr_strength_count_uses(f, uses);
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *sum = &f->data[i];
    if (sum->op != NYIR_ADD_I64 || sum->dst < 0 || sum->a < 0 || sum->b < 0)
      continue;

    int final_ops[2] = {sum->a, sum->b};
    bool matched = false;
    int x = -1;
    int64_t doubled_addend = 0;
    int scaled_def_idx = -1;
    for (int side = 0; side < 2 && !matched; ++side) {
      int scaled_value = final_ops[side];
      int other_value = final_ops[1 - side];
      int scaled_root = nyr_strength_root_copy(f, defs, scaled_value);
      int other_root = nyr_strength_root_copy(f, defs, other_value);
      if (scaled_root < 0 || scaled_root >= f->next_value || other_root < 0 ||
          other_root >= f->next_value)
        continue;
      int scaled_di = defs[scaled_root];
      if (scaled_di < 0 || (size_t)scaled_di >= i)
        continue;
      const nyr_inst_t *scaled = &f->data[scaled_di];
      int plus_value = -1;
      int64_t factor = 0;
      if (scaled->op == NYIR_SHL_I64 &&
          nyr_strength_const_value(f, defs, facts, scaled->b, &factor) &&
          factor == 1) {
        plus_value = scaled->a;
      } else if (scaled->op == NYIR_MUL_I64 &&
                 nyr_strength_const_value(f, defs, facts, scaled->b, &factor) &&
                 factor == 2) {
        plus_value = scaled->a;
      } else if (scaled->op == NYIR_MUL_I64 &&
                 nyr_strength_const_value(f, defs, facts, scaled->a, &factor) &&
                 factor == 2) {
        plus_value = scaled->b;
      }
      int plus_root = nyr_strength_root_copy(f, defs, plus_value);
      if (plus_root < 0 || plus_root >= f->next_value || other_root < 0 ||
          other_root >= f->next_value)
        continue;
      int plus_di = defs[plus_root];
      if (plus_di < 0 || (size_t)plus_di >= (size_t)scaled_di ||
          f->data[plus_di].op != NYIR_ADD_I64)
        continue;
      const nyr_inst_t *plus = &f->data[plus_di];
      int plus_ops[2] = {plus->a, plus->b};
      for (int constant_side = 0; constant_side < 2; ++constant_side) {
        int64_t addend = 0;
        if (!nyr_strength_const_value(f, defs, facts, plus_ops[constant_side],
                                      &addend))
          continue;
        int candidate_x =
            nyr_strength_root_copy(f, defs, plus_ops[1 - constant_side]);
        if (candidate_x != other_root)
          continue;
        if (!nyr_strength_single_use_copy_path(f, defs, uses, scaled_value,
                                               scaled_root) ||
            !nyr_strength_single_use_copy_path(f, defs, uses, plus_value,
                                               plus_root) ||
            uses[scaled_root] != 1 || uses[plus_root] != 1)
          continue;
        uint64_t bits = (uint64_t)addend * 2u;
        memcpy(&doubled_addend, &bits, sizeof(doubled_addend));
        x = candidate_x;
        scaled_def_idx = scaled_di;
        matched = true;
        break;
      }
    }
    if (!matched || x < 0 || scaled_def_idx < 0 || f->next_value > INT_MAX - 3)
      continue;

    int old_dst = sum->dst;
    nyr_strength_result_metadata_t metadata = nyr_strength_result_metadata(sum);
    int multiply_by_three = f->next_value++;
    int three = f->next_value++;
    int two_c = f->next_value++;
    if (!nir_ensure_inst_space(f, 3)) {
      free(defs);
      free(uses);
      free(facts);
      return false;
    }
    memmove(&f->data[i + 4], &f->data[i + 1],
            (f->len - i - 1) * sizeof(*f->data));
    f->len += 3;
    f->data[i] = (nyr_inst_t){
        .op = NYIR_CONST_I64,
        .dst = three,
        .a = -1,
        .b = -1,
        .imm = 3,
        .range = {.has_min = true, .has_max = true, .min = 3, .max = 3}};
    f->data[i + 1] = (nyr_inst_t){.op = NYIR_MUL_I64,
                                  .dst = multiply_by_three,
                                  .a = x,
                                  .b = three,
                                  .effects = NYIR_EFFECT_NONE};
    nyr_strength_apply_result_metadata(
        &f->data[i + 1],
        nyr_strength_result_metadata(&f->data[scaled_def_idx]));
    f->data[i + 1].range = (nyr_range_t){0};
    f->data[i + 2] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = two_c,
                                  .a = -1,
                                  .b = -1,
                                  .imm = doubled_addend,
                                  .range = {.has_min = true,
                                            .has_max = true,
                                            .min = doubled_addend,
                                            .max = doubled_addend}};
    f->data[i + 3] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                  .dst = old_dst,
                                  .a = multiply_by_three,
                                  .b = two_c,
                                  .effects = NYIR_EFFECT_NONE};
    nyr_strength_apply_result_metadata(&f->data[i + 3], metadata);
    if (nyr_strength_trace_enabled())
      ny_dbg_heuristic_event(
          "STRENGTH_REDUCE",
          "accept reassociation @%zu dst=v%d: (v%d + c)*2 + v%d -> 3*v%d + 2*c",
          i, old_dst, x, x, x);
    break; /* defs/use counts index the pre-rewrite instruction stream. */
  }
  free(defs);
  free(uses);
  free(facts);
  return true;
}

/*
 * Strength reduction: multiply/divide by known power-of-2
 *
 * x * 2^n  →  x << n
 * x / 2^n  →  x >> n  (only with a proven non-negative dividend; otherwise
 * preserve signed truncation semantics with the original division)
 * x % 2^n  →  x & (2^n - 1)  (for unsigned / non-negative)
 *
 * This is a single-pass peephole that runs AFTER const_fold has
 * propagated constants, so most multiplications by literal powers
 * already have CONST_I64 as operand b.
 */

bool nyr_strength_reduce(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  if (!nyr_strength_combine_scaled_add(f))
    return false;
  int fact_count = f->next_value;
  nyr_value_fact_t *facts = calloc((size_t)fact_count, sizeof(*facts));
  int *uses = calloc((size_t)fact_count, sizeof(*uses));
  if (!facts || !uses ||
      !nyr_analyze_values(f, facts, (size_t)fact_count, NULL, 0)) {
    free(facts);
    free(uses);
    return false;
  }
  nyr_strength_count_uses(f, uses);
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    /*
     * x + x → x << 1 (same SSA value or load_local pair of one local).
     */
    if (in->op == NYIR_ADD_I64 && nir_operands_same_value(f, in->a, in->b, i)) {
      int saved_dst = in->dst;
      int saved_a = in->a;
      nyr_strength_result_metadata_t metadata =
          nyr_strength_result_metadata(in);
      if (nyr_strength_trace_enabled())
        ny_dbg_heuristic_event("STRENGTH_REDUCE",
                               "accept add @%zu dst=v%d: same SSA operand v%d; "
                               "replace x+x with x<<1",
                               i, saved_dst, saved_a);
      int shift_value = f->next_value++;
      if (!nir_ensure_inst_space(f, 1)) {
        free(facts);
        free(uses);
        return false;
      }
      memmove(&f->data[i + 1], &f->data[i], (f->len - i) * sizeof(nyr_inst_t));
      f->len++;
      f->data[i] = (nyr_inst_t){
          .op = NYIR_CONST_I64, .dst = shift_value, .a = -1, .b = -1, .imm = 1};
      f->data[i + 1] = (nyr_inst_t){.op = NYIR_SHL_I64,
                                    .dst = saved_dst,
                                    .a = saved_a,
                                    .b = shift_value,
                                    .effects = NYIR_EFFECT_NONE};
      nyr_strength_apply_result_metadata(&f->data[i + 1], metadata);
      i++;
      continue;
    }
    if (in->op == NYIR_MUL_I64) {
      /*
       * Check if either operand is CONST_I64 with a power of 2.
       */
      int64_t imm = 0;
      int other = -1;
      int operands[2] = {in->b, in->a};
      int counterparts[2] = {in->a, in->b};
      for (size_t operand = 0; operand < 2 && other < 0; ++operand) {
        int value = operands[operand];
        if (value < 0 || value >= fact_count)
          continue;
        /*
         * Accept values proven singleton by analysis, not only a nearby
         * CONST_I64 definition. This lets copies/PHIs/range refinement feed
         * the same strength reductions without another backwards IR scan.
         */
        if (nyr_value_fact_singleton(&facts[value], &imm))
          other = counterparts[operand];
      }
      if (imm > 1 && (imm & (imm - 1)) == 0 && other >= 0) {
        int shift = 0;
        int64_t v = imm;
        while (v > 1) {
          v >>= 1;
          shift++;
        }
        /*
         * Save all operands before growing/moving data: realloc may invalidate
         * `in`, which previously produced dangling value IDs at O2/O3.
         */
        int saved_dst = in->dst;
        nyr_strength_result_metadata_t metadata =
            nyr_strength_result_metadata(in);
        if (nyr_strength_trace_enabled())
          ny_dbg_heuristic_event(
              "STRENGTH_REDUCE",
              "accept mul @%zu dst=v%d: operand v%d is proven singleton %lld; "
              "power-of-two exponent %d makes shift equivalent",
              i, saved_dst, other, (long long)imm, shift);
        int shift_value = f->next_value++;
        if (!nir_ensure_inst_space(f, 1)) {
          free(facts);
          free(uses);
          return false;
        }
        /*
         * Shift instructions [i..len) down by 1.
         */
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(nyr_inst_t));
        f->len++;
        f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = shift_value,
                                  .a = -1,
                                  .b = -1,
                                  .imm = (int64_t)shift};
        f->data[i + 1] = (nyr_inst_t){.op = NYIR_SHL_I64,
                                      .dst = saved_dst,
                                      .a = other,
                                      .b = shift_value,
                                      .effects = NYIR_EFFECT_NONE};
        nyr_strength_apply_result_metadata(&f->data[i + 1], metadata);
        i++; /* skip the SHL we just inserted */
        continue;
      }
      /*
       * Expand factors 5 and 9 into shift plus add unless the result feeds
       * one subtraction. In that case retain MUL_I64 so x86 can select a
       * single LEA for the scaled value instead of separate shift and add.
       */
      bool scaled_mul_feeds_sub = false;
      if ((imm == 5 || imm == 9) && other >= 0 &&
          (size_t)in->dst < (size_t)fact_count && uses[in->dst] == 1) {
        for (size_t user = i + 1; user < f->len; ++user) {
          const nyr_inst_t *use = &f->data[user];
          if (use->op == NYIR_SUB_I64 &&
              (use->a == in->dst || use->b == in->dst)) {
            scaled_mul_feeds_sub = true;
            break;
          }
        }
      }
      if ((imm == 5 || imm == 9) && other >= 0 && !scaled_mul_feeds_sub) {
        int shift_amt = imm == 5 ? 2 : 3;
        int saved_dst = in->dst;
        int saved_a = other;
        nyr_strength_result_metadata_t metadata =
            nyr_strength_result_metadata(in);
        if (nyr_strength_trace_enabled())
          ny_dbg_heuristic_event(
              "STRENGTH_REDUCE",
              "accept mul @%zu dst=v%d: constant %lld = 2^%d + 1; "
              "expand to shift plus add",
              i, saved_dst, (long long)imm, shift_amt);
        int shift_imm = f->next_value++;
        int shift_res = f->next_value++;
        if (!nir_ensure_inst_space(f, 2)) {
          free(facts);
          free(uses);
          return false;
        }
        memmove(&f->data[i + 2], &f->data[i],
                (f->len - i) * sizeof(nyr_inst_t));
        f->len += 2;
        f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = shift_imm,
                                  .a = -1,
                                  .b = -1,
                                  .imm = shift_amt};
        f->data[i + 1] = (nyr_inst_t){.op = NYIR_SHL_I64,
                                      .dst = shift_res,
                                      .a = saved_a,
                                      .b = shift_imm,
                                      .effects = NYIR_EFFECT_NONE};
        f->data[i + 2] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                      .dst = saved_dst,
                                      .a = shift_res,
                                      .b = saved_a,
                                      .effects = NYIR_EFFECT_NONE};
        nyr_strength_apply_result_metadata(&f->data[i + 2], metadata);
        i += 2;
        continue;
      }
      /*
       * Sub-style: x * (2^k - 1) → (x << k) - x for k in {3,4,5,6}
       * (factors 7, 15, 31, 63).
       */
      if ((imm == 7 || imm == 15 || imm == 31 || imm == 63) && other >= 0) {
        int shift_amt = imm == 7 ? 3 : imm == 15 ? 4 : imm == 31 ? 5 : 6;
        int saved_dst = in->dst;
        int saved_a = other;
        nyr_strength_result_metadata_t metadata =
            nyr_strength_result_metadata(in);
        if (nyr_strength_trace_enabled())
          ny_dbg_heuristic_event(
              "STRENGTH_REDUCE",
              "accept mul @%zu dst=v%d: constant %lld = 2^%d - 1; "
              "expand to shift minus operand",
              i, saved_dst, (long long)imm, shift_amt);
        int shift_imm = f->next_value++;
        int shift_res = f->next_value++;
        if (!nir_ensure_inst_space(f, 2)) {
          free(facts);
          free(uses);
          return false;
        }
        memmove(&f->data[i + 2], &f->data[i],
                (f->len - i) * sizeof(nyr_inst_t));
        f->len += 2;
        f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                  .dst = shift_imm,
                                  .a = -1,
                                  .b = -1,
                                  .imm = shift_amt};
        f->data[i + 1] = (nyr_inst_t){.op = NYIR_SHL_I64,
                                      .dst = shift_res,
                                      .a = saved_a,
                                      .b = shift_imm,
                                      .effects = NYIR_EFFECT_NONE};
        f->data[i + 2] = (nyr_inst_t){.op = NYIR_SUB_I64,
                                      .dst = saved_dst,
                                      .a = shift_res,
                                      .b = saved_a,
                                      .effects = NYIR_EFFECT_NONE};
        nyr_strength_apply_result_metadata(&f->data[i + 2], metadata);
        i += 2;
        continue;
      }
    }
    if (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) {
      const nyr_range_t *dividend_range = &facts[in->a].range;
      /*
       * Loads intentionally do not carry an immutable range in NYIR. For a
       * local whose latest store dominates this straight-line use, recover the
       * stored value's fact without pretending this is general MemorySSA.
       */
      for (size_t j = i; !dividend_range->has_min && j > 0; --j) {
        const nyr_inst_t *def = &f->data[j - 1];
        if (def->dst != in->a)
          continue;
        if (def->op != NYIR_LOAD_LOCAL)
          break;
        for (size_t k = j - 1; k > 0; --k) {
          const nyr_inst_t *store = &f->data[k - 1];
          if (store->op == NYIR_STORE_LOCAL && store->imm == def->imm) {
            if (store->a >= 0 && store->a < f->next_value)
              dividend_range = &facts[store->a].range;
            break;
          }
          if (store->op == NYIR_LABEL || store->op == NYIR_BR ||
              store->op == NYIR_BR_IF || store->op == NYIR_CALL)
            break;
        }
        break;
      }
      bool dividend_nonnegative =
          dividend_range->has_min && dividend_range->min >= 0;
      int64_t divisor = 0;
      if (!nyr_strength_recover_const(f, facts, fact_count, in->b, &divisor))
        continue;
      bool power_of_two = divisor > 1 && (divisor & (divisor - 1)) == 0;
      if (!dividend_nonnegative && in->op == NYIR_MOD_I64 && power_of_two) {
        if (nyr_strength_trace_enabled())
          ny_dbg_heuristic_event(
              "STRENGTH_REDUCE",
              "reject %s @%zu: divisor=%lld is power-of-two but no "
              "nonnegative dividend proof; modulo mask would change signed "
              "semantics",
              in->op == NYIR_DIV_I64 ? "div" : "mod", i, (long long)divisor);
        continue;
      }
      if (power_of_two) {
        int64_t replacement = divisor - 1;
        int shift = 0;
        for (int64_t v = divisor; v > 1; v >>= 1)
          ++shift;
        if (in->op == NYIR_DIV_I64) {
          replacement = shift;
        }
        int saved_dst = in->dst;
        int saved_a = in->a;
        nyr_strength_result_metadata_t metadata =
            nyr_strength_result_metadata(in);
        if (in->op == NYIR_DIV_I64 && !dividend_nonnegative) {
          /*
           * Signed division truncates toward zero, unlike SAR for negative
           * inputs. Add (x < 0 ? divisor - 1 : 0) before the arithmetic shift:
           * (x + ((x >> 63) & (divisor - 1))) >> log2(divisor).
           */
          if (nyr_strength_trace_enabled())
            ny_dbg_heuristic_event(
                "STRENGTH_REDUCE",
                "accept signed div @%zu dst=v%d: divisor=%lld; bias negative "
                "inputs before SAR",
                i, saved_dst, (long long)divisor);
          int sign_shift = f->next_value++;
          int mask_value = f->next_value++;
          int quotient_shift = f->next_value++;
          int sign_value = f->next_value++;
          int bias_value = f->next_value++;
          int adjusted_value = f->next_value++;
          if (!nir_ensure_inst_space(f, 6)) {
            free(facts);
            free(uses);
            return false;
          }
          memmove(&f->data[i + 7], &f->data[i + 1],
                  (f->len - i - 1) * sizeof(*f->data));
          f->len += 6;
          f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = sign_shift,
                                    .a = -1,
                                    .b = -1,
                                    .imm = 63};
          f->data[i + 1] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                        .dst = mask_value,
                                        .a = -1,
                                        .b = -1,
                                        .imm = divisor - 1};
          f->data[i + 2] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                        .dst = quotient_shift,
                                        .a = -1,
                                        .b = -1,
                                        .imm = shift};
          f->data[i + 3] = (nyr_inst_t){.op = NYIR_SAR_I64,
                                        .dst = sign_value,
                                        .a = saved_a,
                                        .b = sign_shift};
          f->data[i + 4] = (nyr_inst_t){.op = NYIR_AND_I64,
                                        .dst = bias_value,
                                        .a = sign_value,
                                        .b = mask_value};
          f->data[i + 5] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                        .dst = adjusted_value,
                                        .a = saved_a,
                                        .b = bias_value};
          f->data[i + 6] = (nyr_inst_t){.op = NYIR_SAR_I64,
                                        .dst = saved_dst,
                                        .a = adjusted_value,
                                        .b = quotient_shift};
          nyr_strength_apply_result_metadata(&f->data[i + 6], metadata);
          i += 6;
        } else {
          nyr_op_t replacement_op =
              in->op == NYIR_DIV_I64 ? NYIR_SAR_I64 : NYIR_AND_I64;
          if (nyr_strength_trace_enabled())
            ny_dbg_heuristic_event(
                "STRENGTH_REDUCE",
                "accept %s @%zu dst=v%d: divisor=%lld and dividend range "
                "proves nonnegative; replace with %s %lld",
                in->op == NYIR_DIV_I64 ? "div" : "mod", i, saved_dst,
                (long long)divisor, in->op == NYIR_DIV_I64 ? "SAR" : "AND",
                (long long)replacement);
          int imm_value = f->next_value++;
          if (!nir_ensure_inst_space(f, 1)) {
            free(facts);
            free(uses);
            return false;
          }
          memmove(&f->data[i + 1], &f->data[i],
                  (f->len - i) * sizeof(*f->data));
          ++f->len;
          f->data[i] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                    .dst = imm_value,
                                    .a = -1,
                                    .b = -1,
                                    .imm = replacement};
          f->data[i + 1] = (nyr_inst_t){.op = replacement_op,
                                        .dst = saved_dst,
                                        .a = saved_a,
                                        .b = imm_value};
          nyr_strength_apply_result_metadata(&f->data[i + 1], metadata);
          ++i;
        }
      } else if (divisor > 1) {
        /*
         * Non-power-of-two constant divisors are left as DIV/MOD for now.
         *
         * A Granlund-Montgomery magic-number rewrite for this case needs
         * (a) the high half of a 128-bit product (no NYIR mulhi opcode yet),
         * (b) a LOGICAL shift of the product (NYIR_SAR_I64 sign-extends, and
         *     the unsigned magic product may set the sign bit), and
         * (c) the "add" indicator: for divisors such as 7 the canonical
         *     magicu() result requires (x + 1) * m >> s, not x * m >> s.
         * An earlier attempt at this rewrite used the low 64 bits of the
         * product, SAR, and a naive 2*r1 update whose overflow zeroed r1 and
         * made the search loop spin forever on real IR (repro: gcbench's
         * rt_main at --nyir-dump with PHIs preserved, divisor 3).  It never
         * emitted correct code for any divisor, so it is removed rather than
         * patched; revisit with NYIR_SHR_I64 + NYIR_MULHI_I64 in place.
         */
        if (nyr_strength_trace_enabled())
          ny_dbg_heuristic_event(
              "STRENGTH_REDUCE",
              "keep %s @%zu: divisor=%lld is not a power of two; constant-"
              "divisor lowering needs logical shift and high-half multiply "
              "NYIR ops",
              in->op == NYIR_DIV_I64 ? "div" : "mod", i, (long long)divisor);
      }
    }
  }
  free(facts);
  free(uses);
  return true;
}
