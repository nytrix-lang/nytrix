#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Strength reduction: multiply/divide by known power-of-2            */
/*                                                                    */
/* x * 2^n  →  x << n                                                 */
/* x / 2^n  →  x >> n  (only for positive range; we use SAR which     */
/*                        matches C truncation toward -inf for neg.)  */
/* x % 2^n  →  x & (2^n - 1)  (for unsigned / non-negative)           */
/*                                                                    */
/* This is a single-pass peephole that runs AFTER const_fold has      */
/* propagated constants, so most multiplications by literal powers    */
/* already have CONST_I64 as operand b.                               */
/* ------------------------------------------------------------------ */

bool nyir_strength_reduce(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  nyir_value_fact_t *facts =
      calloc((size_t)f->next_value, sizeof(*facts));
  if (!facts || !nyir_analyze_values(f, facts, (size_t)f->next_value,
                                       NULL, 0)) {
    free(facts);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    /* x + x → x << 1 (same SSA value or load_local pair of one local). */
    if (in->op == NYIR_ADD_I64 &&
        nir_operands_same_value(f, in->a, in->b, i)) {
      int saved_dst = in->dst;
      int saved_a = in->a;
      int shift_value = f->next_value++;
      if (!nir_ensure_inst_space(f, 1)) {
        free(facts);
        return false;
      }
      memmove(&f->data[i + 1], &f->data[i],
              (f->len - i) * sizeof(nyir_inst_t));
      f->len++;
      f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                    .dst = shift_value,
                                    .a = -1,
                                    .b = -1,
                                    .imm = 1};
      f->data[i + 1] = (nyir_inst_t){.op = NYIR_SHL_I64,
                                         .dst = saved_dst,
                                         .a = saved_a,
                                         .b = shift_value,
                                         .effects = NYIR_EFFECT_NONE};
      i++;
      continue;
    }
    if (in->op == NYIR_MUL_I64) {
      /* Check if either operand is CONST_I64 with a power of 2. */
      int64_t imm = 0;
      int other = -1;
      int operands[2] = {in->b, in->a};
      int counterparts[2] = {in->a, in->b};
      for (size_t operand = 0; operand < 2 && other < 0; ++operand) {
        int value = operands[operand];
        if (value < 0 || value >= (int)f->next_value)
          continue;
        /* Look back for the defining constant. Multiplication is
         * commutative, so accept either `x * 2^n` or `2^n * x`. */
        for (size_t j = i; j > 0; --j) {
          const nyir_inst_t *def = &f->data[j - 1];
          if (def->op == NYIR_CONST_I64 && def->dst == value) {
            imm = def->imm;
            other = counterparts[operand];
            break;
          }
          if (def->dst == value)
            break;
        }
      }
      if (imm > 1 && (imm & (imm - 1)) == 0 && other >= 0) {
        int shift = 0;
        int64_t v = imm;
        while (v > 1) { v >>= 1; shift++; }
        /* Save all operands before growing/moving data: realloc may invalidate
         * `in`, which previously produced dangling value IDs at O2/O3. */
        int saved_dst = in->dst;
        int shift_value = f->next_value++;
        if (!nir_ensure_inst_space(f, 1)) {
          free(facts);
          return false;
        }
        /* Shift instructions [i..len) down by 1. */
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(nyir_inst_t));
        f->len++;
        f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                      .dst = shift_value,
                                      .a = -1,
                                      .b = -1,
                                      .imm = (int64_t)shift};
        f->data[i + 1] = (nyir_inst_t){.op = NYIR_SHL_I64,
                                           .dst = saved_dst,
                                           .a = other,
                                           .b = shift_value,
                                           .effects = NYIR_EFFECT_NONE};
        i++; /* skip the SHL we just inserted */
        continue;
      }
      /* LEA-style: x * (2^k + 1) → (x << k) + x for k in {1,2,3}
       * (factors 3, 5, 9). Wrapping mul matches under Nytrix i64 wrap. */
      if ((imm == 3 || imm == 5 || imm == 9) && other >= 0) {
        int shift_amt = imm == 3 ? 1 : imm == 5 ? 2 : 3;
        int saved_dst = in->dst;
        int saved_a = other;
        int shift_imm = f->next_value++;
        int shift_res = f->next_value++;
        if (!nir_ensure_inst_space(f, 2)) {
          free(facts);
          return false;
        }
        memmove(&f->data[i + 2], &f->data[i],
                (f->len - i) * sizeof(nyir_inst_t));
        f->len += 2;
        f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                      .dst = shift_imm,
                                      .a = -1,
                                      .b = -1,
                                      .imm = shift_amt};
        f->data[i + 1] = (nyir_inst_t){.op = NYIR_SHL_I64,
                                           .dst = shift_res,
                                           .a = saved_a,
                                           .b = shift_imm,
                                           .effects = NYIR_EFFECT_NONE};
        f->data[i + 2] = (nyir_inst_t){.op = NYIR_ADD_I64,
                                           .dst = saved_dst,
                                           .a = shift_res,
                                           .b = saved_a,
                                           .effects = NYIR_EFFECT_NONE};
        i += 2;
        continue;
      }
    }
    if (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) {
      const nyir_range_t *dividend_range = &facts[in->a].range;
      /* Loads intentionally do not carry an immutable range in NYIR. For a
       * local whose latest store dominates this straight-line use, recover the
       * stored value's fact without pretending this is general MemorySSA. */
      for (size_t j = i; !dividend_range->has_min && j > 0; --j) {
        const nyir_inst_t *def = &f->data[j - 1];
        if (def->dst != in->a)
          continue;
        if (def->op != NYIR_LOAD_LOCAL)
          break;
        for (size_t k = j - 1; k > 0; --k) {
          const nyir_inst_t *store = &f->data[k - 1];
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
      if (!dividend_range->has_min || dividend_range->min < 0)
        continue;
      int64_t divisor = 0;
      for (size_t j = i; j > 0; --j) {
        const nyir_inst_t *def = &f->data[j - 1];
        if (def->dst == in->b) {
          if (def->op == NYIR_CONST_I64)
            divisor = def->imm;
          break;
        }
      }
      if (divisor > 1 && (divisor & (divisor - 1)) == 0) {
        int64_t replacement = divisor - 1;
        if (in->op == NYIR_DIV_I64) {
          replacement = 0;
          for (int64_t v = divisor; v > 1; v >>= 1)
            ++replacement;
        }
        int saved_dst = in->dst;
        int saved_a = in->a;
        nyir_op_t replacement_op =
            in->op == NYIR_DIV_I64 ? NYIR_SAR_I64 : NYIR_AND_I64;
        int imm_value = f->next_value++;
        if (f->len + 1 > f->cap) {
          size_t new_cap = f->cap ? f->cap * 2 : 64;
          nyir_inst_t *new_data = realloc(f->data,
                                             new_cap * sizeof(*new_data));
          if (!new_data) {
            free(facts);
            return false;
          }
          f->data = new_data;
          f->cap = new_cap;
        }
        memmove(&f->data[i + 1], &f->data[i],
                (f->len - i) * sizeof(*f->data));
        ++f->len;
        f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                      .dst = imm_value, .a = -1, .b = -1,
                                      .imm = replacement};
        f->data[i + 1] = (nyir_inst_t){.op = replacement_op,
                                          .dst = saved_dst, .a = saved_a,
                                          .b = imm_value};
        ++i;
      } else if (divisor > 1) {
        /* Constant-divisor magic number for unsigned division.
         * For unsigned x / d where d is a known positive constant,
         * compute magic multiplier m and shift s such that
         * (x * m) >> s == x / d. */
        int64_t p = 63;
        uint64_t nc = ((uint64_t)-1) - ((uint64_t)(-divisor)) % (uint64_t)divisor;
        uint64_t q1 = ((uint64_t)1 << p) / nc;
        uint64_t r1 = ((uint64_t)1 << p) - q1 * nc;
        uint64_t q2 = ((uint64_t)1 << p) / (uint64_t)divisor;
        uint64_t r2 = ((uint64_t)1 << p) - q2 * (uint64_t)divisor;
        uint64_t delta;
        do {
          p++;
          q1 = 2 * q1;
          r1 = 2 * r1;
          if (r1 >= nc) {
            q1++;
            r1 -= nc;
          }
          q2 = 2 * q2;
          r2 = 2 * r2;
          if (r2 >= (uint64_t)divisor) {
            q2++;
            r2 -= (uint64_t)divisor;
          }
          delta = (uint64_t)divisor - r2;
        } while (q1 < delta || (q1 == delta && r1 == 0));
        uint64_t magic = q2 + 1;
        unsigned shift = (unsigned)p - 63;
        if (magic <= (uint64_t)INT64_MAX && shift < 64) {
          int saved_dst = in->dst;
          int saved_a = in->a;
          int magic_val = f->next_value++;
          int shift_val = f->next_value++;
          if (!nir_ensure_inst_space(f, 3)) {
            free(facts);
            return false;
          }
          memmove(&f->data[i + 3], &f->data[i],
                  (f->len - i) * sizeof(*f->data));
          f->len += 3;
          f->data[i] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                        .dst = magic_val, .a = -1, .b = -1,
                                        .imm = (int64_t)magic};
          f->data[i + 1] = (nyir_inst_t){.op = NYIR_CONST_I64,
                                            .dst = shift_val, .a = -1, .b = -1,
                                            .imm = (int64_t)shift};
          f->data[i + 2] = (nyir_inst_t){.op = NYIR_MUL_I64,
                                            .dst = saved_dst, .a = saved_a,
                                            .b = magic_val};
          f->data[i + 3] = (nyir_inst_t){.op = NYIR_SAR_I64,
                                            .dst = saved_dst, .a = saved_dst,
                                            .b = shift_val};
          i += 3;
        }
      }
    }
  }
  free(facts);
  return true;
}
