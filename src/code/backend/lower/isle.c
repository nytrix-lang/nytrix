/*
 * ISLE pattern matcher: instruction-selection rules expressed as
 * declarative patterns that match NYIR subgraphs for native lowering.
 */
#include "code/backend/lower/machine.h"
#include "code/backend/passes/util.h"
#include <nyr/ir.h>
#include <stddef.h>
#include <stdio.h>
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
 *
 * Follow COPY chains so an address base and an equivalent copy compare equal.
 * Only unique definitions are trusted; a multiply-defined value is left alone.
 */
static uint32_t mach_copy_root(const nyr_mach_func_t *m, const uint32_t *def,
                               size_t vreg_len, uint32_t v) {
  for (unsigned depth = 0; depth < 8 && v < vreg_len; ++depth) {
    size_t d = def[v];
    if (d == SIZE_MAX)
      break;
    const nyr_mach_inst_t *in = &m->insts[d];
    if (in->opcode != NYR_MACH_COPY ||
        in->src0.kind != NYR_MACH_OPERAND_VREG)
      break;
    v = in->src0.as.reg;
  }
  return v;
}

static bool mach_vreg_const(const nyr_mach_func_t *m, const uint32_t *def,
                            size_t vreg_len, uint32_t v, int64_t want) {
  for (unsigned depth = 0; depth < 8 && v < vreg_len; ++depth) {
    size_t d = def[v];
    if (d == SIZE_MAX)
      return false;
    const nyr_mach_inst_t *in = &m->insts[d];
    if (in->opcode == NYR_MACH_COPY &&
        in->src0.kind == NYR_MACH_OPERAND_IMM)
      return in->src0.as.imm == want;
    if (in->opcode == NYR_MACH_COPY &&
        in->src0.kind == NYR_MACH_OPERAND_VREG) {
      v = in->src0.as.reg;
      continue;
    }
    return false;
  }
  return false;
}

/*
 * Reassociate a scaled address so it reuses an existing base+scale value:
 *
 *   add  d, i, 1              (single use)
 *   lea  a, base, d, scale    ->   lea a, (base+scale), i, scale
 *
 * The replacement base must have a unique definition, equal base and constant
 * scale, and dominate the LEA. Only the entry block is trusted without a MIR
 * dominator tree: it dominates every block, so this cannot introduce a
 * use-before-def on any path. The killed ADD has no other use, so removing it
 * cannot change the value of flags consumed elsewhere (machine IR models
 * comparisons with explicit CMP vregs).
 */
static void mach_reassociate_addresses(nyr_mach_func_t *m) {
  /*
   * Opt-in: the fold is valid but currently worsens register pressure across
   * whole modules (cmov-sort text 1664 -> 3632 bytes, 0.42 -> 3.76 ms), so it
   * must not run by default until reuse is restricted to bases that stay
   * resident. NYTRIX_NO_ADDR_REASSOC still force-disables it.
   */
  if (!getenv("NYTRIX_ADDR_REASSOC") || getenv("NYTRIX_NO_ADDR_REASSOC"))
    return;
  size_t nv = m->vreg_len, ni = m->inst_len;
  if (!nv || !ni || nv > 20000 || ni > 100000)
    return;
  uint32_t *def = malloc(nv * sizeof(*def));
  unsigned char *multi = calloc(nv, 1);
  uint32_t *uses = calloc(nv, sizeof(*uses));
  uint32_t *block = malloc(ni * sizeof(*block));
  /*
   * Declared before the first failure exit so `done` can always release
   * them; earlier they were only freed when their own allocation failed.
   */
  uint64_t *dom = NULL, *edge = NULL, *next = NULL;
  size_t words = 0;
  if (!def || !multi || !uses || !block)
    goto done;
  for (size_t i = 0; i < nv; ++i)
    def[i] = UINT32_MAX;
  for (size_t i = 0; i < ni; ++i)
    block[i] = UINT32_MAX;
  for (size_t b = 0; b < m->block_len; ++b) {
    const nyr_mach_block_t *blk = &m->blocks[b];
    for (size_t k = 0; k < blk->inst_count; ++k) {
      size_t i = blk->first_inst + k;
      if (i >= ni)
        continue;
      block[i] = (uint32_t)b;
      const nyr_mach_inst_t *in = &m->insts[i];
      if (in->dst.kind == NYR_MACH_OPERAND_VREG &&
          in->dst.as.reg < nv && in->opcode != NYR_MACH_STORE) {
        uint32_t v = in->dst.as.reg;
        if (def[v] != UINT32_MAX)
          multi[v] = 1;
        else
          def[v] = (uint32_t)i;
      }
      const nyr_mach_operand_t *ops[] = {&in->src0, &in->src1, &in->src2,
                                         &in->dst};
      size_t nops = in->opcode == NYR_MACH_STORE ? 4 : 3;
      for (size_t o = 0; o < nops; ++o)
        if (ops[o]->kind == NYR_MACH_OPERAND_VREG && ops[o]->as.reg < nv)
          ++uses[ops[o]->as.reg];
      for (size_t a = 0; a < in->args_len; ++a)
        if (in->args[a].kind == NYR_MACH_OPERAND_VREG &&
            in->args[a].as.reg < nv)
          ++uses[in->args[a].as.reg];
    }
  }
  /*
   * Build MIR dominators for the reuse base.  A callee/loop preheader such as
   * BB4 defines base+scale once and dominates every later block; entry-only
   * checking would miss it.  Successors are explicit branches plus fallthrough
   * when the block does not end in BR/RET/TRAP.
   */
  size_t nb = m->block_len;
  if (nb > 0 && nb <= 4096) {
    words = (nb + 63) / 64;
    dom = malloc(nb * words * sizeof(*dom));
    edge = calloc(nb * words, sizeof(*edge));
    next = malloc(words * sizeof(*next));
    if (!dom || !edge || !next) {
      free(dom);
      free(edge);
      free(next);
      dom = NULL;
    }
  }
  if (dom) {
    for (size_t b = 0; b < nb; ++b) {
      const nyr_mach_block_t *blk = &m->blocks[b];
      bool falls = true;
      for (size_t k = 0; k < blk->inst_count; ++k) {
        const nyr_mach_inst_t *in = &m->insts[blk->first_inst + k];
        if ((in->opcode == NYR_MACH_BR || in->opcode == NYR_MACH_BR_IF) &&
            in->src1.kind == NYR_MACH_OPERAND_BLOCK &&
            in->src1.as.block_index < nb) {
          size_t t = in->src1.as.block_index;
          edge[b * words + t / 64] |= UINT64_C(1) << (t % 64);
          if (in->opcode == NYR_MACH_BR)
            falls = false;
        } else if (in->opcode == NYR_MACH_RET || in->opcode == NYR_MACH_TRAP) {
          falls = false;
        }
      }
      if (falls && b + 1 < nb)
        edge[b * words + (b + 1) / 64] |= UINT64_C(1) << ((b + 1) % 64);
    }
    for (size_t b = 0; b < nb; ++b)
      for (size_t w = 0; w < words; ++w)
        dom[b * words + w] = ~UINT64_C(0);
    for (size_t w = 0; w < words; ++w)
      dom[w] = 0;
    dom[0] = 1;
    bool changed = true;
    while (changed) {
      changed = false;
      for (size_t b = 1; b < nb; ++b) {
        for (size_t w = 0; w < words; ++w)
          next[w] = ~UINT64_C(0);
        bool has_pred = false;
        for (size_t p = 0; p < nb; ++p) {
          if (!(edge[p * words + b / 64] & (UINT64_C(1) << (b % 64))))
            continue;
          has_pred = true;
          for (size_t w = 0; w < words; ++w)
            next[w] &= dom[p * words + w];
        }
        if (!has_pred)
          continue;
        next[b / 64] |= UINT64_C(1) << (b % 64);
        for (size_t w = 0; w < words; ++w)
          if (dom[b * words + w] != next[w]) {
            dom[b * words + w] = next[w];
            changed = true;
          }
      }
    }
  }
  for (size_t i = 0; i < ni; ++i) {
    nyr_mach_inst_t *in = &m->insts[i];
    if (in->opcode != NYR_MACH_LEA || in->dst.kind != NYR_MACH_OPERAND_VREG ||
        in->src0.kind != NYR_MACH_OPERAND_VREG ||
        in->src1.kind != NYR_MACH_OPERAND_VREG ||
        in->src2.kind != NYR_MACH_OPERAND_IMM)
      continue;
    int64_t scale = in->src2.as.imm;
    if (scale != 2 && scale != 4 && scale != 8)
      continue;
    uint32_t base = in->src0.as.reg, idx = in->src1.as.reg;
    if (base >= nv || idx >= nv || multi[idx] || uses[idx] != 1)
      continue;
    if (!m->vreg_types ||
        (m->vreg_types[base] != NYR_MACH_TYPE_I64 &&
         m->vreg_types[base] != NYR_MACH_TYPE_PTR) ||
        (m->vreg_types[idx] != NYR_MACH_TYPE_I64 &&
         m->vreg_types[idx] != NYR_MACH_TYPE_PTR))
      continue;
    uint32_t di = def[idx];
    if (di == UINT32_MAX || di >= ni)
      continue;
    const nyr_mach_inst_t *add = &m->insts[di];
    if (add->opcode != NYR_MACH_ADD || add->dst.kind != NYR_MACH_OPERAND_VREG ||
        add->dst.as.reg != idx || add->src0.kind != NYR_MACH_OPERAND_VREG)
      continue;
    bool adds_one =
        (add->src1.kind == NYR_MACH_OPERAND_IMM && add->src1.as.imm == 1) ||
        (add->src1.kind == NYR_MACH_OPERAND_VREG &&
         mach_vreg_const(m, def, nv, add->src1.as.reg, 1));
    if (!adds_one)
      continue;
    uint32_t inner = add->src0.as.reg;
    uint32_t base_root = mach_copy_root(m, def, nv, base);
    uint32_t reuse = UINT32_MAX;
    for (uint32_t cand = 0; cand < nv; ++cand) {
      if (multi[cand] || def[cand] == UINT32_MAX || def[cand] >= ni)
        continue;
      const nyr_mach_inst_t *pd = &m->insts[def[cand]];
      if (pd->opcode != NYR_MACH_ADD ||
          pd->dst.kind != NYR_MACH_OPERAND_VREG ||
          pd->dst.as.reg != cand ||
          pd->src0.kind != NYR_MACH_OPERAND_VREG ||
          pd->src1.kind != NYR_MACH_OPERAND_VREG)
        continue;
      if (!m->vreg_types || pd->src0.as.reg >= nv || pd->src1.as.reg >= nv ||
          inner >= nv)
        continue;
      if (m->vreg_types[pd->src0.as.reg] != NYR_MACH_TYPE_I64 &&
          m->vreg_types[pd->src0.as.reg] != NYR_MACH_TYPE_PTR)
        continue;
      if (m->vreg_types[inner] != NYR_MACH_TYPE_I64 &&
          m->vreg_types[inner] != NYR_MACH_TYPE_PTR)
        continue;
      if (mach_copy_root(m, def, nv, pd->src0.as.reg) != base_root)
        continue;
      if (!mach_vreg_const(m, def, nv, pd->src1.as.reg, scale))
        continue;
      size_t pdi = def[cand];
      size_t db = block[pdi], ub = block[i];
      bool dominates = db == ub && pdi < i;
      if (!dominates && dom && db < nb && ub < nb)
        dominates = (dom[ub * words + db / 64] >> (db % 64)) & 1;
      if (!dominates)
        continue;
      reuse = cand;
      break;
    }
    if (reuse == UINT32_MAX)
      continue;
    in->src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                                    .as.reg = reuse};
    in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_VREG,
                                    .as.reg = inner};
    nyr_mach_inst_t *dead = &m->insts[di];
    if (!getenv("NYTRIX_ADDR_REASSOC_KEEP_ADD")) {
      dead->opcode = NYR_MACH_NOP;
      dead->dst = (nyr_mach_operand_t){0};
      dead->src0 = (nyr_mach_operand_t){0};
      dead->src1 = (nyr_mach_operand_t){0};
      dead->src2 = (nyr_mach_operand_t){0};
      dead->args = NULL;
      dead->args_len = 0;
    }
    if (getenv("NYTRIX_ADDR_REASSOC_TRACE"))
      fprintf(stderr, "addr-reassoc: rewrote LEA at inst %zu\n", i);
    /*
     * Validate MIR after rewrite.
     */
    for (size_t vi = 0; vi < ni; ++vi) {
      const nyr_mach_inst_t *ci = &m->insts[vi];
      const nyr_mach_operand_t *co[] = {&ci->dst, &ci->src0, &ci->src1, &ci->src2};
      for (size_t c = 0; c < 4; ++c)
        if (co[c]->kind == NYR_MACH_OPERAND_VREG && co[c]->as.reg >= nv)
          fprintf(stderr, "addr-reassoc: INVALID vreg %u at inst %zu\n",
                  co[c]->as.reg, vi);
      for (size_t a = 0; a < ci->args_len; ++a)
        if (ci->args[a].kind == NYR_MACH_OPERAND_VREG &&
            ci->args[a].as.reg >= nv)
          fprintf(stderr, "addr-reassoc: INVALID arg vreg %u at inst %zu\n",
                  ci->args[a].as.reg, vi);
    }
  }
 done:
  free(def);
  free(multi);
  free(uses);
  free(block);
  free(dom);
  free(edge);
  free(next);
}

bool nyr_isle_apply_mach(nyr_mach_func_t *mach) {
  if (!mach || mach->inst_len == 0)
    return true;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    nyr_mach_inst_t *in = &mach->insts[i];
    /*
     * The rule table describes scalar integer identities. Vector and float
     * values share the generic machine opcodes (vector x + x is a packed
     * add, not a scalar shift), so leave every non-integer result alone.
     */
    if (in->dst.kind != NYR_MACH_OPERAND_VREG ||
        (size_t)in->dst.as.reg >= mach->vreg_len ||
        (mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_I64 &&
         mach->vreg_types[in->dst.as.reg] != NYR_MACH_TYPE_PTR))
      continue;
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
          in->src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM};
          in->src0.as.imm = rule->emit_imm;
          in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_NONE};
        } else if (rule->emit_op == NYIR_SHL_I64) {
          /*
           * add_self (x + x → x << 1): keep the shift semantics; the
           * previous fallthrough turned this into COPY x, computing x
           * instead of 2x.
           */
          in->opcode = NYR_MACH_SHL;
          in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM};
          in->src1.as.imm = rule->emit_imm;
        } else {
          in->opcode = NYR_MACH_COPY;
          in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_NONE};
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
        in->src0 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM};
        in->src0.as.imm = rule->emit_imm;
        in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_NONE};
      } else if (rule->emit_op == NYIR_SHL_I64) {
        in->opcode = NYR_MACH_SHL;
        in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_IMM};
        in->src1.as.imm = rule->emit_imm;
      } else {
        in->opcode = NYR_MACH_COPY;
        in->src1 = (nyr_mach_operand_t){.kind = NYR_MACH_OPERAND_NONE};
      }
      break;
    }
  }
  mach_reassociate_addresses(mach);
  return true;
}

/*
 * Back-compat entry used by the optimizer.
 */
bool nyr_apply_rules(nyr_func_t *f) { return ny_isle_apply_nir(f); }
