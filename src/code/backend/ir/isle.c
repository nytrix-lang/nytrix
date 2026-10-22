/*
 * ISLE pattern matcher: instruction-selection rules expressed as
 * declarative patterns that match NYIR subgraphs for native lowering.
 */
#include "code/backend/ir/machine.h"
#include "code/backend/ir/opt/util.h"
#include <nyr/ir.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Typed rewrite/selection rules (ISLE-inspired shape, clean-room).
 * One shared applicator for NYIR text rewrites and machine form identity folds
 * so peepole (`nyr_apply_rules` / `ny_isle_apply_nir`) and machine form byte
 * encoding share the same rule table.
 */

typedef enum {
  NY_ISLE_MATCH_OP = 1,
  NY_ISLE_MATCH_IMM_B = 2,
  NY_ISLE_MATCH_SAME_AB = 4,
} nyr_isle_flags_t;

typedef struct {
  const char *name;
  nyr_op_t match_op;
  unsigned flags;
  int64_t imm_b;
  nyr_op_t emit_op;
  int64_t emit_imm;
  /*
   * cost: lower is better for extraction
   */
  int cost;
} nyr_isle_rule_t;

static const nyr_isle_rule_t nyr_isle_table[] = {
    {"add0", NYIR_ADD_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    {"mul1", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 1, NYIR_COPY,
     0, 1},
    {"mul0", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0,
     NYIR_CONST_I64, 0, 1},
    {"sub_self", NYIR_SUB_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_SAME_AB, 0,
     NYIR_CONST_I64, 0, 1},
    {"xor_self", NYIR_XOR_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_SAME_AB, 0,
     NYIR_CONST_I64, 0, 1},
    {"and0", NYIR_AND_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0,
     NYIR_CONST_I64, 0, 1},
    {"or0", NYIR_OR_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    /*
     * QBE/Cranelift-style strength identities (clean-room).
     */
    {"xor0", NYIR_XOR_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    {"and_neg1", NYIR_AND_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, -1,
     NYIR_COPY, 0, 1},
    {"or_neg1", NYIR_OR_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, -1,
     NYIR_CONST_I64, -1, 1},
    {"shl0", NYIR_SHL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    {"sar0", NYIR_SAR_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    {"sub0", NYIR_SUB_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 0, NYIR_COPY,
     0, 1},
    {"div1", NYIR_DIV_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 1, NYIR_COPY,
     0, 1},
    {"mod1", NYIR_MOD_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 1,
     NYIR_CONST_I64, 0, 1},
    /*
     * NOTE: no div_self/mod_self rules. x/x == 1 and x%y-style folds
     * delete the divide-by-zero trap when x may be 0 (hardware idiv
     * raises SIGFPE); folding them without a proven-nonzero divisor
     * range changes observable behavior, so they stay out of the shared
     * table (which has no range facts available on the machine path).
     */
    {"mul2", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 2,
     NYIR_SHL_I64, 1, 2},
    {"mul4", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 4,
     NYIR_SHL_I64, 2, 2},
    {"mul8", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 8,
     NYIR_SHL_I64, 3, 2},
    {"mul16", NYIR_MUL_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_IMM_B, 16,
     NYIR_SHL_I64, 4, 2},
    {"add_self", NYIR_ADD_I64, NY_ISLE_MATCH_OP | NY_ISLE_MATCH_SAME_AB, 0,
     NYIR_SHL_I64, 1, 2},
};

size_t ny_isle_rule_count(void) {
  return sizeof(nyr_isle_table) / sizeof(nyr_isle_table[0]);
}

const char *ny_isle_rule_name(size_t i) {
  return i < ny_isle_rule_count() ? nyr_isle_table[i].name : NULL;
}

int ny_isle_rule_cost(size_t i) {
  return i < ny_isle_rule_count() ? nyr_isle_table[i].cost : 1000;
}

/*
 * Map NYIR op to machine form opcode for shared identity folds.
 */
static nyr_mach_opcode_t nyr_isle_nir_to_mir_op(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return NYR_MACH_ADD;
  case NYIR_SUB_I64:
    return NYR_MACH_SUB;
  case NYIR_MUL_I64:
    return NYR_MACH_MUL;
  case NYIR_DIV_I64:
    return NYR_MACH_DIV;
  case NYIR_AND_I64:
    return NYR_MACH_AND;
  case NYIR_OR_I64:
    return NYR_MACH_OR;
  case NYIR_XOR_I64:
    return NYR_MACH_XOR;
  case NYIR_SHL_I64:
    return NYR_MACH_SHL;
  case NYIR_SAR_I64:
    return NYR_MACH_SAR;
  case NYIR_ROR32_I64:
    return NYR_MACH_ROR32;
  case NYIR_BSWAP_I64:
    return NYR_MACH_BSWAP64;
  case NYIR_BSWAP32_I64:
    return NYR_MACH_BSWAP32;
  case NYIR_COPY:
    return NYR_MACH_COPY;
  case NYIR_SQRT_F64:
    return NYR_MACH_SQRT;
  case NYIR_SIN_F64:
    return NYR_MACH_SIN;
  case NYIR_COS_F64:
    return NYR_MACH_COS;
  default:
    return NYR_MACH_NOP;
  }
}

/*
 * Find a CONST_I64 with value `sh` defined in the SAME basic block as
 * instruction `idx` (scanning back to the block's LABEL). A definition
 * earlier in the same block dominates this use; a constant from another
 * block only linearly precedes it and may not dominate at all, so the
 * search must not cross the label boundary.
 */
static int nyr_isle_same_block_const(const nyr_func_t *f, size_t idx,
                                    int64_t sh) {
  for (size_t j = idx; j-- > 0;) {
    nyr_op_t op = f->data[j].op;
    if (op == NYIR_NOP)
      continue;
    if (op == NYIR_LABEL)
      break;
    if (op == NYIR_CONST_I64 && f->data[j].dst >= 0 && f->data[j].imm == sh)
      return f->data[j].dst;
  }
  return -1;
}

/*
 * Apply one rule to a NYIR instruction when rhs const / same-operands match.
 */
static bool nyr_isle_try_nir_inst(nyr_func_t *f, size_t idx, nyr_inst_t *in,
                                 const bool *known, const int64_t *value) {
  if (!in || in->dst < 0 || in->a < 0)
    return false;
  for (size_t r = 0; r < ny_isle_rule_count(); ++r) {
    const nyr_isle_rule_t *rule = &nyr_isle_table[r];
    if (in->op != rule->match_op)
      continue;
    if (rule->flags & NY_ISLE_MATCH_SAME_AB) {
      if (in->b != in->a)
        continue;
      if (rule->emit_op == NYIR_CONST_I64) {
        nir_make_const(in, rule->emit_imm);
      } else if (rule->emit_op == NYIR_SHL_I64) {
        /*
         * a + a → a << 1 or a * 2^n → a << n: need a const for the shift
         * amount.  Use an existing CONST from the same block only;
         * otherwise skip — the strength reduce pass handles the MUL form.
         */
        int sh_v =
            nyr_isle_same_block_const(f, idx, (int64_t)(int)rule->emit_imm);
        if (sh_v >= 0) {
          in->op = NYIR_SHL_I64;
          in->b = sh_v;
          in->flags = 0;
          nyr_clear_derived_facts(in);
        } else {
          continue;
        }
      } else {
        nir_make_copy(in, in->a);
      }
      return true;
    }
    if ((rule->flags & NY_ISLE_MATCH_IMM_B) == 0)
      continue;
    if (in->b < 0 || !known || !value || !known[in->b] ||
        value[in->b] != rule->imm_b)
      continue;
    if (rule->emit_op == NYIR_CONST_I64) {
      nir_make_const(in, rule->emit_imm);
    } else if (rule->emit_op == NYIR_SHL_I64) {
      /*
       * mul by power-of-two: rewrite to SHL with a same-block const
       * shift amount; otherwise leave for strength_reduce.
       */
      int sh_v = nyr_isle_same_block_const(f, idx, (int64_t)(int)rule->emit_imm);
      if (sh_v >= 0) {
        in->op = NYIR_SHL_I64;
        in->b = sh_v;
        in->flags = 0;
        nyr_clear_derived_facts(in);
      } else {
        continue;
      }
    } else {
      nir_make_copy(in, in->a);
    }
    return true;
  }
  return false;
}

bool ny_isle_apply_nir(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = calloc((size_t)f->next_value, sizeof(int64_t));
  if (!known || !value) {
    free(known);
    free(value);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 &&
        (size_t)in->dst < (size_t)f->next_value) {
      known[in->dst] = true;
      value[in->dst] = in->imm;
    }
    (void)nyr_isle_try_nir_inst(f, i, in, known, value);
  }
  free(known);
  free(value);
  return true;
}

/*
 * Shared machine form identity fold using the same rule table (bytes path).
 */
bool nyr_isle_apply_mach(nyr_mach_func_t *mach) {
  if (!mach || mach->inst_len == 0)
    return true;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    nyr_mach_inst_t *in = &mach->insts[i];
    for (size_t r = 0; r < ny_isle_rule_count(); ++r) {
      const nyr_isle_rule_t *rule = &nyr_isle_table[r];
      nyr_mach_opcode_t mop = nyr_isle_nir_to_mir_op(rule->match_op);
      if (mop == NYR_MACH_NOP || in->opcode != mop)
        continue;
      if (rule->flags & NY_ISLE_MATCH_SAME_AB) {
        if (in->src0.kind != NYR_MACH_OPERAND_VREG ||
            in->src1.kind != NYR_MACH_OPERAND_VREG ||
            in->src0.as.reg != in->src1.as.reg)
          continue;
        /*
         * Machine-form identity rules like add_self (x+x -> x) are only
         * valid for integer types.  Float ADD(x,x) = 2*x, not x, so
         * skip when the destination vreg is float-typed.
         */
        if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
            (size_t)in->dst.as.reg < mach->vreg_len &&
            (mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F64 ||
             mach->vreg_types[in->dst.as.reg] == NYR_MACH_TYPE_F32))
          continue;
        if (rule->emit_op == NYIR_CONST_I64) {
          in->opcode = NYR_MACH_COPY;
          in->src0.kind = NYR_MACH_OPERAND_IMM;
          in->src0.as.imm = rule->emit_imm;
          in->src1.kind = NYR_MACH_OPERAND_NONE;
        } else if (rule->emit_op == NYIR_SHL_I64) {
          /*
           * add_self (x + x → x << 1): keep the shift semantics; the
           * previous fallthrough turned this into COPY x, computing x
           * instead of 2x.
           */
          in->opcode = NYR_MACH_SHL;
          in->src1.kind = NYR_MACH_OPERAND_IMM;
          in->src1.as.imm = rule->emit_imm;
        } else {
          in->opcode = NYR_MACH_COPY;
          in->src1.kind = NYR_MACH_OPERAND_NONE;
        }
        break;
      }
      if ((rule->flags & NY_ISLE_MATCH_IMM_B) == 0)
        continue;
      if (in->src1.kind != NYR_MACH_OPERAND_IMM ||
          in->src1.as.imm != rule->imm_b)
        continue;
      if (rule->emit_op == NYIR_CONST_I64) {
        in->opcode = NYR_MACH_COPY;
        in->src0.kind = NYR_MACH_OPERAND_IMM;
        in->src0.as.imm = rule->emit_imm;
        in->src1.kind = NYR_MACH_OPERAND_NONE;
      } else if (rule->emit_op == NYIR_SHL_I64) {
        in->opcode = NYR_MACH_SHL;
        in->src1.kind = NYR_MACH_OPERAND_IMM;
        in->src1.as.imm = rule->emit_imm;
      } else {
        in->opcode = NYR_MACH_COPY;
        in->src1.kind = NYR_MACH_OPERAND_NONE;
      }
      break;
    }
  }
  return true;
}

/*
 * Back-compat entry used by the optimizer.
 */
bool nyr_apply_rules(nyr_func_t *f) { return ny_isle_apply_nir(f); }
