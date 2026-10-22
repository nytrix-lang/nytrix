/*
 * Peephole optimizer: local instruction-sequence rewriting for
 * common patterns — algebraic identities, bitwise simplifications,
 * branchless selects, and two-level cancellations (e.g. (x+y)-y → x,
 * (x^y)^y → x, (x-y)+y → x). Run at O1+, re-run after IRCE.
 *
 * References:
 *  Warren — Hacker's Delight, 2nd Ed. (2012).
 *   Ch.2: bit manipulation identities, branchless abs/min/max/sign.
 *   Ch.10: division by constants (Granlund-Montgomery algorithm).
 *  Granlund & Montgomery — "Division by Invariant Integers using
 *   Multiplication" (PLDI 1994). §3 unsigned, §4 signed, §5 modulo.
 *  Bansal & Aiken — "Automatic Generation of Peephole Superoptimizers"
 *   (ASPLOS 2006). Exhaustive enumeration, Alive2-style validation.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "base/parallel.h"
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  nyr_func_t *f;
  const bool *known;
  const int64_t *value;
  const unsigned char *kind;
  const nyr_value_fact_t *facts;
} nir_peephole_parallel_ctx_t;

/*
 * A def-map entry is only trusted while it still points at the instruction
 * that defines the value.  Inserting rewrites (nir_rewrite_shl/neg) shift
 * later indices and make cached entries stale; matching on a stale entry
 * turned algebraic identities into unsound cancellations (sha256 schedule
 * corruption).  Re-derive validity at each use instead.
 */
static const nyr_inst_t *nir_def_inst(const nyr_func_t *f, const int *defs,
                                       int value) {
  if (!defs || value < 0 || value >= f->next_value)
    return NULL;
  int idx = defs[value];
  if (idx < 0 || (size_t)idx >= f->len)
    return NULL;
  const nyr_inst_t *def = &f->data[idx];
  return def->dst == value ? def : NULL;
}

static bool nir_is_const_value(const nyr_func_t *f, const int *defs, int value,
                               int64_t expected) {
  const nyr_inst_t *def = nir_def_inst(f, defs, value);
  return def && def->op == NYIR_CONST_I64 && def->imm == expected;
}

static const nyr_inst_t *nir_boolean_compare_root(const nyr_func_t *f,
                                                   const int *defs, int value,
                                                   unsigned depth) {
  if (!f || !defs || depth > 16)
    return NULL;
  const nyr_inst_t *def = nir_def_inst(f, defs, value);
  if (!def)
    return NULL;
  if (def->op == NYIR_CMP_I64 || def->op == NYIR_CMP_F64 ||
      def->op == NYIR_CMP_F32)
    return def;
  if (def->op == NYIR_COPY)
    return nir_boolean_compare_root(f, defs, def->a, depth + 1);
  if (def->op == NYIR_AND_I64) {
    int other = -1;
    if (nir_is_const_value(f, defs, def->a, 1))
      other = def->b;
    else if (nir_is_const_value(f, defs, def->b, 1))
      other = def->a;
    if (other >= 0)
      return nir_boolean_compare_root(f, defs, other, depth + 1);
  }
  return NULL;
}

/*
 * Prove copy/PHI cycles boolean without assuming an unresolved backedge is
 * boolean. Every reachable definition must preserve {0,1}, and every node
 * must reach a real boolean producer. In particular, a closed undef cycle
 * cannot borrow a proof from an unrelated constant input of the root PHI.
 * The fixed cap bounds the local peephole cost; larger graphs stay unchanged.
 */
static bool nir_boolean_phi_value(const nyr_func_t *f, const int *defs,
                                  int value) {
  enum { MAX_BOOLEAN_VALUES = 64 };
  int values[MAX_BOOLEAN_VALUES] = {value};
  const nyr_inst_t *nodes[MAX_BOOLEAN_VALUES] = {0};
  bool grounded[MAX_BOOLEAN_VALUES] = {0};
  size_t count = 1;
  for (size_t i = 0; i < count; ++i) {
    const nyr_inst_t *in = nir_def_inst(f, defs, values[i]);
    if (!in)
      return false;
    nodes[i] = in;
    if ((in->op == NYIR_CONST_I64 && (in->imm == 0 || in->imm == 1)) ||
        nir_boolean_compare_root(f, defs, values[i], 0)) {
      grounded[i] = true;
      continue;
    }
    if (in->op != NYIR_COPY && in->op != NYIR_PHI)
      return false;
    size_t inputs = in->op == NYIR_COPY ? 1 : in->phi_incoming_len;
    if (!inputs)
      return false;
    for (size_t k = 0; k < inputs; ++k) {
      int input = in->op == NYIR_COPY ? in->a : in->phi_incoming[k].value;
      if (input < 0 || input >= f->next_value)
        return false;
      size_t n = 0;
      while (n < count && values[n] != input)
        ++n;
      if (n == count) {
        if (count == MAX_BOOLEAN_VALUES)
          return false;
        values[count++] = input;
      }
    }
  }
  bool changed;
  do {
    changed = false;
    for (size_t i = 0; i < count; ++i) {
      if (grounded[i])
        continue;
      const nyr_inst_t *in = nodes[i];
      size_t inputs = in->op == NYIR_COPY ? 1 : in->phi_incoming_len;
      for (size_t k = 0; k < inputs; ++k) {
        int input = in->op == NYIR_COPY ? in->a : in->phi_incoming[k].value;
        for (size_t n = 0; n < count; ++n) {
          if (values[n] == input && grounded[n]) {
            grounded[i] = true;
            changed = true;
            break;
          }
        }
        if (grounded[i])
          break;
      }
    }
  } while (changed);
  for (size_t i = 0; i < count; ++i)
    if (!grounded[i])
      return false;
  return true;
}

static bool nir_boolean_value(const nyr_func_t *f, const int *defs, int value,
                              unsigned depth) {
  if (!f || !defs || depth > 16)
    return false;
  if (nir_is_const_value(f, defs, value, 0) ||
      nir_is_const_value(f, defs, value, 1))
    return true;
  const nyr_inst_t *def = nir_def_inst(f, defs, value);
  if (def && def->op == NYIR_COPY)
    return nir_boolean_value(f, defs, def->a, depth + 1);
  if (def && def->op == NYIR_PHI)
    return nir_boolean_phi_value(f, defs, value);
  return nir_boolean_compare_root(f, defs, value, depth) != NULL;
}

static nyr_cmp_t nir_invert_i64_cmp(nyr_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return NYIR_CMP_NE;
  case NYIR_CMP_NE:
    return NYIR_CMP_EQ;
  case NYIR_CMP_LT:
    return NYIR_CMP_GE;
  case NYIR_CMP_LE:
    return NYIR_CMP_GT;
  case NYIR_CMP_GT:
    return NYIR_CMP_LE;
  case NYIR_CMP_GE:
    return NYIR_CMP_LT;
  default:
    return cmp;
  }
}

static bool nir_simplify_boolean_compare(nyr_func_t *f, const int *defs,
                                         nyr_inst_t *in) {
  if (!f || !defs || !in || in->op != NYIR_CMP_I64 ||
      (in->cmp != NYIR_CMP_EQ && in->cmp != NYIR_CMP_NE))
    return false;
  int boolean = -1;
  int64_t constant = -1;
  const nyr_inst_t *a_def = nir_def_inst(f, defs, in->a);
  const nyr_inst_t *b_def = nir_def_inst(f, defs, in->b);
  if (b_def && b_def->op == NYIR_CONST_I64) {
    boolean = in->a;
    constant = b_def->imm;
  } else if (a_def && a_def->op == NYIR_CONST_I64) {
    boolean = in->b;
    constant = a_def->imm;
  }
  if (boolean < 0 || !nir_boolean_value(f, defs, boolean, 0))
    return false;
  if (constant != 0 && constant != 1) {
    nir_make_const(in, in->cmp == NYIR_CMP_NE ? 1 : 0);
    return true;
  }
  const nyr_inst_t *root = nir_boolean_compare_root(f, defs, boolean, 0);
  if (!root || root->op != NYIR_CMP_I64)
    return false; /* Keep PHIs and ordered float predicates explicit. */
  in->a = root->a;
  in->b = root->b;
  if ((in->cmp == NYIR_CMP_EQ && constant == 0) ||
      (in->cmp == NYIR_CMP_NE && constant == 1))
    in->cmp = nir_invert_i64_cmp(root->cmp);
  else
    in->cmp = root->cmp;
  return true;
}

static bool nir_value_single_user(const nyr_func_t *f, int value,
                                  size_t *user) {
  size_t count = 0;
  if (!f || value < 0)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    const int operands[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t n = 0; n < sizeof(operands) / sizeof(operands[0]); ++n) {
      if (operands[n] == value) {
        if (++count > 1)
          return false;
        if (user)
          *user = i;
      }
    }
    for (size_t n = 0; n < in->extra_args_len; ++n) {
      if (in->extra_args[n] == value) {
        if (++count > 1)
          return false;
        if (user)
          *user = i;
      }
    }
    for (size_t n = 0; n < in->phi_incoming_len; ++n) {
      if (in->phi_incoming[n].value == value) {
        if (++count > 1)
          return false;
        if (user)
          *user = i;
      }
    }
  }
  return count == 1;
}

static bool nir_factor_select_common_add(nyr_func_t *f, const int *defs,
                                         bool *changed) {
  if (!f || !defs || !changed)
    return false;
  *changed = false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *select = &f->data[i];
    /*
     * If-conversion may leave the result class unknown even when both arms
     * are proven raw i64 additions. The arm checks below establish that the
     * rewritten SELECT result is raw; retain the guard against tagged values.
     */
    if (select->op != NYIR_SELECT_I64 ||
        (select->scalar_class != NYIR_SCALAR_I64_RAW &&
         select->scalar_class != NYIR_SCALAR_UNKNOWN))
      continue;
    int arms[2] = {select->b, select->c};
    const nyr_inst_t *add[2] = {NULL, NULL};
    for (unsigned n = 0; n < 2; ++n) {
      if (arms[n] < 0 || arms[n] >= f->next_value || defs[arms[n]] < 0)
        break;
      add[n] = nir_def_inst(f, defs, arms[n]);
      if (!add[n] || add[n]->op != NYIR_ADD_I64 ||
          add[n]->scalar_class != NYIR_SCALAR_I64_RAW) {
        add[n] = NULL;
        break;
      }
    }
    if (!add[0] || !add[1])
      continue;
    int common = -1, arm_value[2] = {-1, -1};
    const int lhs[2] = {add[0]->a, add[1]->a};
    const int rhs[2] = {add[0]->b, add[1]->b};
    if (lhs[0] == lhs[1]) {
      common = lhs[0];
      arm_value[0] = rhs[0];
      arm_value[1] = rhs[1];
    } else if (lhs[0] == rhs[1]) {
      common = lhs[0];
      arm_value[0] = rhs[0];
      arm_value[1] = lhs[1];
    } else if (rhs[0] == lhs[1]) {
      common = rhs[0];
      arm_value[0] = lhs[0];
      arm_value[1] = rhs[1];
    } else if (rhs[0] == rhs[1]) {
      common = rhs[0];
      arm_value[0] = lhs[0];
      arm_value[1] = lhs[1];
    }
    if (common < 0 || arm_value[0] < 0 || arm_value[1] < 0)
      continue;

    nyr_inst_t folded_select = *select;
    if (f->next_value < 0 || f->next_value > INT_MAX - 2)
      continue;
    if (!nir_ensure_inst_space(f, 2))
      return false;
    const nyr_debug_loc_t select_debug = folded_select.debug;
    const uint8_t semantic_ownership = folded_select.semantic_ownership;
    const bool semantic_mutable = folded_select.semantic_mutable;
    const int selected = f->next_value++;
    const int sum = f->next_value++;
    memmove(&f->data[i + 2], &f->data[i], (f->len - i) * sizeof(*f->data));
    f->len += 2;
    folded_select.dst = selected;
    folded_select.b = arm_value[0];
    folded_select.c = arm_value[1];
    folded_select.scalar_class = NYIR_SCALAR_I64_RAW;
    folded_select.semantic_ownership = NY_SEM_OWN_UNKNOWN;
    folded_select.semantic_mutable = false;
    nyr_clear_derived_facts(&folded_select);
    f->data[i] = folded_select;
    f->data[i + 1] = (nyr_inst_t){.op = NYIR_ADD_I64,
                                   .dst = sum,
                                   .a = selected,
                                   .b = common,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .effects = NYIR_EFFECT_NONE,
                                   .scalar_class = NYIR_SCALAR_I64_RAW,
                                   .debug = select_debug};
    nyr_inst_t *copy = &f->data[i + 2];
    const nyr_debug_loc_t debug = copy->debug;
    const int dst = copy->dst;
    *copy = (nyr_inst_t){.op = NYIR_COPY,
                          .dst = dst,
                          .a = sum,
                          .b = -1,
                          .c = -1,
                          .d = -1,
                          .e = -1,
                          .f = -1,
                          .effects = NYIR_EFFECT_NONE,
                          .scalar_class = NYIR_SCALAR_I64_RAW,
                          .semantic_ownership = semantic_ownership,
                          .semantic_mutable = semantic_mutable,
                          .debug = debug};
    nyr_clear_derived_facts(copy);
    nyr_refresh_metadata(f);
    *changed = true;
    return true;
  }
  return true;
}

/*
 * `x % 2 != 0` is exactly `(x & 1) != 0` for every two's-complement i64,
 * including negative values.  Only rewrite a private modulo result whose
 * entire use chain is copies ending at this predicate; the modulo value
 * itself differs for negative odd inputs, so replacing it more broadly would
 * be incorrect.  Insert the mask constant in the modulo's own block to make
 * its dominance unconditional.
 */
static bool nir_simplify_mod_parity_compare(nyr_func_t *f, const int *defs,
                                            const nyr_inst_t *cmp,
                                            size_t cmp_idx) {
  if (!f || !defs || !cmp || cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_NE)
    return false;
  int value = -1;
  if (nir_is_const_value(f, defs, cmp->a, 0))
    value = cmp->b;
  else if (nir_is_const_value(f, defs, cmp->b, 0))
    value = cmp->a;
  if (value < 0)
    return false;

  size_t mod_idx = SIZE_MAX;
  size_t expected_user = cmp_idx;
  for (unsigned depth = 0; depth < 16; ++depth) {
    const nyr_inst_t *def = nir_def_inst(f, defs, value);
    if (!def)
      return false;
    size_t user_idx = SIZE_MAX;
    if (!nir_value_single_user(f, value, &user_idx) ||
        user_idx != expected_user)
      return false;
    if (def->op == NYIR_COPY) {
      if (f->data[user_idx].op == NYIR_PHI)
        return false;
      expected_user = (size_t)(def - f->data);
      value = def->a;
      continue;
    }
    if (def->op != NYIR_MOD_I64 || def->a < 0 || def->b < 0 ||
        !(nir_is_const_value(f, defs, def->b, 2) ||
          nir_is_const_value(f, defs, def->b, -2)))
      return false;
    mod_idx = (size_t)(def - f->data);
    break;
  }
  if (mod_idx == SIZE_MAX || mod_idx >= cmp_idx)
    return false;

  int dividend = f->data[mod_idx].a;
  int mask = f->next_value++;
  if (!nir_ensure_inst_space(f, 1)) {
    --f->next_value;
    return false;
  }
  memmove(&f->data[mod_idx + 1], &f->data[mod_idx],
          (f->len - mod_idx) * sizeof(*f->data));
  ++f->len;
  f->data[mod_idx] = (nyr_inst_t){.op = NYIR_CONST_I64,
                                   .dst = mask,
                                   .a = -1,
                                   .b = -1,
                                   .c = -1,
                                   .d = -1,
                                   .e = -1,
                                   .f = -1,
                                   .imm = 1};
  nyr_inst_t *mod = &f->data[mod_idx + 1];
  mod->op = NYIR_AND_I64;
  mod->a = dividend;
  mod->b = mask;
  mod->effects = NYIR_EFFECT_NONE;
  nyr_clear_derived_facts(mod);
  return true;
}

static bool nir_is_ror32_count(const nyr_func_t *f, const int *defs,
                               const bool *known, const int64_t *value,
                               int count) {
  if (count < 0 || count >= f->next_value)
    return false;
  if (known[count])
    return value[count] >= 0 && value[count] < 32;
  const nyr_inst_t *def = nir_def_inst(f, defs, count);
  return def && def->op == NYIR_AND_I64 &&
         (nir_is_const_value(f, defs, def->a, 31) ||
          nir_is_const_value(f, defs, def->b, 31));
}

static bool nir_peephole_parallel_task(size_t i, void *opaque) {
  nir_peephole_parallel_ctx_t *ctx = (nir_peephole_parallel_ctx_t *)opaque;
  nyr_inst_t *in = &ctx->f->data[i];
  if (in->dst < 0 || in->a < 0 || in->b < 0)
    return true;
  /*
   * Word-mode awareness: integer identity folds (ADD 0, AND -1, MUL 1,
   * etc.) must only fire on i64 constants (kind==1).  A float bit pattern
   * that happens to equal 0/-1/1.0-bits must not fold an integer op, and
   * an integer 0 must not fold a float op.  kind==2 is f64 bits, 3 is f32.
   */
  unsigned char akind = ctx->kind ? ctx->kind[in->a] : 0;
  unsigned char bkind = ctx->kind ? ctx->kind[in->b] : 0;
  bool ak = ctx->known[in->a] && (!ctx->kind || akind == 1);
  bool bk = ctx->known[in->b] && (!ctx->kind || bkind == 1);
  bool akf64 = ctx->known[in->a] && ctx->kind && akind == 2;
  bool bkf64 = ctx->known[in->b] && ctx->kind && bkind == 2;
  bool akf32 = ctx->known[in->a] && ctx->kind && akind == 3;
  bool bkf32 = ctx->known[in->b] && ctx->kind && bkind == 3;
  /*
   * When kind tracking is absent (legacy), fall back to untyped known.
   */
  if (!ctx->kind) {
    akf64 = ak;
    bkf64 = bk;
    akf32 = ak;
    bkf32 = bk;
  }
  int64_t av = ak ? ctx->value[in->a] : 0;
  int64_t bv = bk ? ctx->value[in->b] : 0;
  int64_t avf64 = akf64 ? ctx->value[in->a] : 0;
  int64_t bvf64 = bkf64 ? ctx->value[in->b] : 0;
  int64_t avf32 = akf32 ? ctx->value[in->a] : 0;
  int64_t bvf32 = bkf32 ? ctx->value[in->b] : 0;
  switch (in->op) {
  case NYIR_ADD_I64:
    if (bk && bv == 0)
      nir_make_copy(in, in->a);
    else if (ak && av == 0)
      nir_make_copy(in, in->b);
    break;
  case NYIR_SUB_I64:
    if (in->a == in->b)
      nir_make_const(in, 0);
    else if (bk && bv == 0)
      nir_make_copy(in, in->a);
    break;
  case NYIR_MUL_I64:
    if ((bk && bv == 0) || (ak && av == 0))
      nir_make_const(in, 0);
    else if (bk && bv == 1)
      nir_make_copy(in, in->a);
    else if (ak && av == 1)
      nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_I64: {
    if (bk && bv == 1) {
      nir_make_copy(in, in->a);
    } else if (ak && av == 0 && bk && bv != 0) {
      nir_make_const(in, 0);
    } else if (in->a == in->b) {
      nyr_range_t range = {0};
      if ((ak && av != 0) ||
          (ctx->facts &&
           nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range) &&
           nir_range_excludes_zero(&range)))
        nir_make_const(in, 1);
    }
    break;
  }
  case NYIR_MOD_I64: {
    if (bk && bv == 1) {
      nir_make_const(in, 0);
    } else if (bk && bv == -1) {
      nyr_range_t range = {0};
      bool safe = ak && av != INT64_MIN;
      if (!safe && ctx->facts &&
          nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range))
        safe = nir_range_excludes_int64_min(&range);
      if (safe)
        nir_make_const(in, 0);
    } else if (ak && av == 0 && bk && bv != 0) {
      nir_make_const(in, 0);
    } else if (in->a == in->b) {
      nyr_range_t range = {0};
      if ((ak && av != 0) ||
          (ctx->facts &&
           nir_value_range_at(ctx->f, ctx->facts, in->a, i, &range) &&
           nir_range_excludes_zero(&range)))
        nir_make_const(in, 0);
    }
    break;
  }
  case NYIR_AND_I64:
    if (in->a == in->b)
      nir_make_copy(in, in->a);
    else if ((bk && bv == 0) || (ak && av == 0))
      nir_make_const(in, 0);
    else if (bk && bv == -1)
      nir_make_copy(in, in->a);
    else if (ak && av == -1)
      nir_make_copy(in, in->b);
    break;
  case NYIR_OR_I64:
    if (in->a == in->b)
      nir_make_copy(in, in->a);
    else if ((bk && bv == -1) || (ak && av == -1))
      nir_make_const(in, -1);
    else if (bk && bv == 0)
      nir_make_copy(in, in->a);
    else if (ak && av == 0)
      nir_make_copy(in, in->b);
    break;
  case NYIR_XOR_I64:
    if (in->a == in->b)
      nir_make_const(in, 0);
    else if (bk && bv == 0)
      nir_make_copy(in, in->a);
    else if (ak && av == 0)
      nir_make_copy(in, in->b);
    break;
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    if (bk && bv == 0)
      nir_make_copy(in, in->a);
    else if (ak && av == 0)
      nir_make_const(in, 0);
    break;
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
    /*
     * x - (+0.0) == x exactly, including x = -0.0/NaN/Inf. Adding
     * (+0.0) preserves x only when signed zeros agree (-0.0 + +0.0 is
     * +0.0), so the add forms are fast-math only.
     */
    if (in->op == NYIR_SUB_F64) {
      if (bkf64 && bvf64 == INT64_C(0))
        nir_make_copy(in, in->a);
    } else if ((bkf64 && bvf64 == INT64_C(0)) ||
               (akf64 && avf64 == INT64_C(0))) {
      if (nyr_fast_math())
        nir_make_copy(in, bkf64 ? in->a : in->b);
    }
    break;
  case NYIR_MUL_F64:
    /*
     * x * (+0.0) is +0.0 only for finite nonzero x; NaN/Inf/-signs differ.
     */
    if (((bkf64 && bvf64 == INT64_C(0)) || (akf64 && avf64 == INT64_C(0))) &&
        nyr_fast_math())
      nir_make_f64_const(in, INT64_C(0));
    else if (bkf64 && bvf64 == INT64_C(4607182418800017408))
      nir_make_copy(in, in->a);
    else if (akf64 && avf64 == INT64_C(4607182418800017408))
      nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_F64:
    if (bkf64 && bvf64 == INT64_C(4607182418800017408))
      nir_make_copy(in, in->a);
    break;
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
    /*
     * See the f64 note: sub is exact, add is fast-math only.
     */
    if (in->op == NYIR_SUB_F32) {
      if (bkf32 && bvf32 == 0)
        nir_make_copy(in, in->a);
    } else if ((bkf32 && bvf32 == 0) || (akf32 && avf32 == 0)) {
      if (nyr_fast_math())
        nir_make_copy(in, bkf32 ? in->a : in->b);
    }
    break;
  case NYIR_MUL_F32:
    if (((bkf32 && bvf32 == 0) || (akf32 && avf32 == 0)) && nyr_fast_math())
      nir_make_f32_const(in, 0);
    else if (bkf32 && bvf32 == INT64_C(1065353216))
      nir_make_copy(in, in->a);
    else if (akf32 && avf32 == INT64_C(1065353216))
      nir_make_copy(in, in->b);
    break;
  case NYIR_DIV_F32:
    if (bkf32 && bvf32 == INT64_C(1065353216))
      nir_make_copy(in, in->a);
    break;
  default:
    break;
  }
  return true;
}

bool nyr_peephole(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  bool *known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
  int64_t *value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
  unsigned char *kind = (unsigned char *)calloc((size_t)f->next_value, 1);
  nyr_value_fact_t *facts =
      (nyr_value_fact_t *)calloc((size_t)f->next_value, sizeof(*facts));
  if (!known || !value || !kind || !facts) {
    free(known);
    free(value);
    free(kind);
    free(facts);
    return false;
  }
  if (!nir_collect_consts_kind(f, known, value, kind) ||
      !nyr_analyze_values(f, facts, (size_t)f->next_value, NULL, 0)) {
    free(known);
    free(value);
    free(kind);
    free(facts);
    return false;
  }
  nir_peephole_parallel_ctx_t parallel_ctx = {f, known, value, kind, facts};
  if (!ny_parallel_for(f->len, f->len, nir_peephole_parallel_task,
                       &parallel_ctx)) {
    free(known);
    free(value);
    free(facts);
    return false;
  }
  int *defs = nyr_build_defs(f);
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->a < 0 || in->b < 0)
      continue;
    bool ak = known[in->a] && kind[in->a] == 1;
    bool bk = known[in->b] && kind[in->b] == 1;
    bool akf64 = known[in->a] && kind[in->a] == 2;
    bool bkf64 = known[in->b] && kind[in->b] == 2;
    bool akf32 = known[in->a] && kind[in->a] == 3;
    bool bkf32 = known[in->b] && kind[in->b] == 3;
    int64_t av = ak ? value[in->a] : 0;
    int64_t bv = bk ? value[in->b] : 0;
    int64_t avf64 = akf64 ? value[in->a] : 0;
    int64_t bvf64 = bkf64 ? value[in->b] : 0;
    int64_t avf32 = akf32 ? value[in->a] : 0;
    int64_t bvf32 = bkf32 ? value[in->b] : 0;
    switch (in->op) {
    case NYIR_ADD_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_copy(in, in->b);
      else if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 &&
               in->b < f->next_value) {
        const nyr_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyr_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_SUB_I64 && da->b == in->b) {
          nir_make_copy(in, da->a);
          break;
        }
        if (db && db->op == NYIR_SUB_I64 && db->b == in->a) {
          nir_make_copy(in, db->a);
          break;
        }
      }
      break;
    case NYIR_SUB_I64:
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_const(in, 0);
      else if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 &&
               in->b < f->next_value) {
        const nyr_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyr_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_ADD_I64) {
          if (da->a == in->b) {
            nir_make_copy(in, da->b);
            break;
          }
          if (da->b == in->b) {
            nir_make_copy(in, da->a);
            break;
          }
        }
        if (db && db->op == NYIR_SUB_I64 && db->a == in->a) {
          nir_make_copy(in, db->b);
          break;
        }
      }
      break;
    case NYIR_MUL_I64:
      if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 1)
        nir_make_copy(in, in->b);
      else if (bk && bv == -1) {
        /*
         * x * -1 == 0 - x under Nytrix's wrapping i64 mul.
         */
        if (!nir_rewrite_neg(f, &i, in->a)) {
          free(known);
          free(value);
          free(facts);
          return false;
        }
        continue;
      } else if (ak && av == -1) {
        if (!nir_rewrite_neg(f, &i, in->b)) {
          free(known);
          free(value);
          free(facts);
          return false;
        }
        continue;
      } else if (bk && bv > 1 && (bv & (bv - 1)) == 0) {
        /*
         * x * power_of_2 == x << log2(power_of_2).
         * Only for shift amounts in [1, 62] to stay within defined i64
         * shift behavior.
         */
        int shift = 0;
        int64_t v = bv;
        while (v > 1) {
          v >>= 1;
          shift++;
        }
        if (shift >= 1 && shift <= 62) {
          if (!nir_rewrite_shl(f, &i, in->a, shift)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      } else if (ak && av > 1 && (av & (av - 1)) == 0) {
        int shift = 0;
        int64_t v = av;
        while (v > 1) {
          v >>= 1;
          shift++;
        }
        if (shift >= 1 && shift <= 62) {
          if (!nir_rewrite_shl(f, &i, in->b, shift)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      }
      break;
    case NYIR_DIV_I64: {
      if (bk && bv == 1)
        nir_make_copy(in, in->a);
      else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        nyr_range_t range = {0};
        if ((ak && av != 0) ||
            (nir_value_range_at(f, facts, in->a, i, &range) &&
             nir_range_excludes_zero(&range)))
          nir_make_const(in, 1);
      } else if (bk && bv == -1) {
        /*
         * x / -1 == 0 - x only when INT64_MIN is impossible (trap case).
         */
        nyr_range_t range = {0};
        bool safe = ak && av != INT64_MIN;
        if (!safe && nir_value_range_at(f, facts, in->a, i, &range))
          safe = nir_range_excludes_int64_min(&range);
        if (safe) {
          if (!nir_rewrite_neg(f, &i, in->a)) {
            free(known);
            free(value);
            free(facts);
            return false;
          }
          continue;
        }
      }
      break;
    }
    case NYIR_MOD_I64: {
      if (bk && bv == 1)
        nir_make_const(in, 0);
      else if (bk && bv == -1) {
        nyr_range_t range = {0};
        bool safe = ak && av != INT64_MIN;
        if (!safe && nir_value_range_at(f, facts, in->a, i, &range))
          safe = nir_range_excludes_int64_min(&range);
        if (safe)
          nir_make_const(in, 0);
      } else if (ak && av == 0 && bk && bv != 0)
        nir_make_const(in, 0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        nyr_range_t range = {0};
        if ((ak && av != 0) ||
            (nir_value_range_at(f, facts, in->a, i, &range) &&
             nir_range_excludes_zero(&range)))
          nir_make_const(in, 0);
      }
      break;
    }
    case NYIR_AND_I64:
      if (defs && bk && bv == 1 && nir_boolean_value(f, defs, in->a, 0)) {
        nir_make_copy(in, in->a);
        break;
      }
      if (defs && ak && av == 1 && nir_boolean_value(f, defs, in->b, 0)) {
        nir_make_copy(in, in->b);
        break;
      }
      /*
       * A 32-bit rotate is sound only when both the input and complete result
       * are explicitly governed by the low-32 mask.  The count is either a
       * proven in-range constant or normalized with & 31; any partial or
       * cast-dependent shape remains ordinary i64 shifts and OR.
       */
      if (defs) {
        int or_value = -1;
        if (bk && bv == 0xffffffffLL)
          or_value = in->a;
        else if (ak && av == 0xffffffffLL)
          or_value = in->b;
        const nyr_inst_t *or_inst = nir_def_inst(f, defs, or_value);
        if (or_inst && or_inst->op == NYIR_OR_I64) {
          const nyr_inst_t *left = nir_def_inst(f, defs, or_inst->a);
          const nyr_inst_t *right = nir_def_inst(f, defs, or_inst->b);
          const nyr_inst_t *shr = left && left->op == NYIR_SAR_I64     ? left
                                   : right && right->op == NYIR_SAR_I64 ? right
                                                                        : NULL;
          const nyr_inst_t *shl = left && left->op == NYIR_SHL_I64     ? left
                                   : right && right->op == NYIR_SHL_I64 ? right
                                                                        : NULL;
          const nyr_inst_t *complement =
              shl ? nir_def_inst(f, defs, shl->b) : NULL;
          bool constant_complement =
              shr && shl && shr->b >= 0 && shr->b < f->next_value &&
              shl->b >= 0 && shl->b < f->next_value && known[shr->b] &&
              known[shl->b] && value[shr->b] >= 0 && value[shr->b] < 32 &&
              value[shl->b] == 32 - value[shr->b];
          /*
           * Every dereference below needs shr non-NULL: complement is
           * derived from shl alone, so an OR of SHL with a non-SAR operand
           * reaches the `complement->b == shr->b` test with shr == NULL
           * (sha256's rotr crashed exactly there).
           */
          bool symbolic_complement =
              shr && complement && complement->op == NYIR_SUB_I64 &&
              nir_is_const_value(f, defs, complement->a, 32) &&
              complement->b == shr->b;
          if (shr && shl && nir_operands_same_value(f, shr->a, shl->a, i) &&
              nir_is_ror32_count(f, defs, known, value, shr->b) &&
              (constant_complement || symbolic_complement)) {
            in->op = NYIR_ROR32_I64;
            in->a = shr->a;
            in->b = shr->b;
            in->flags &= ~NYIR_INST_F_NARROW32;
            in->effects = NYIR_EFFECT_NONE;
            nyr_clear_derived_facts(in);
            break;
          }
        }
      }
      if (nir_operands_same_value(f, in->a, in->b, i))
        nir_make_copy(in, in->a);
      else if ((bk && bv == 0) || (ak && av == 0))
        nir_make_const(in, 0);
      else if (bk && bv == -1)
        nir_make_copy(in, in->a);
      else if (ak && av == -1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_OR_I64:
    case NYIR_XOR_I64:
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (in->op == NYIR_OR_I64)
          nir_make_copy(in, in->a);
        else
          nir_make_const(in, 0);
        break;
      }
      if (bk && bv == 0) {
        nir_make_copy(in, in->a);
        break;
      }
      if (ak && av == 0) {
        nir_make_copy(in, in->b);
        break;
      }
      if (in->op == NYIR_OR_I64 && ((bk && bv == -1) || (ak && av == -1))) {
        nir_make_const(in, -1);
        break;
      }
      if (in->op == NYIR_XOR_I64 && defs && in->a >= 0 &&
          in->a < f->next_value && in->b >= 0 && in->b < f->next_value) {
        const nyr_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyr_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && da->op == NYIR_XOR_I64) {
          if (da->b == in->b) {
            nir_make_copy(in, da->a);
            break;
          }
          if (da->a == in->b) {
            nir_make_copy(in, da->b);
            break;
          }
        }
        if (db && db->op == NYIR_XOR_I64) {
          if (db->b == in->a) {
            nir_make_copy(in, db->a);
            break;
          }
          if (db->a == in->a) {
            nir_make_copy(in, db->b);
            break;
          }
        }
      }
      if (defs && in->a >= 0 && in->a < f->next_value && in->b >= 0 &&
          in->b < f->next_value) {
        const nyr_inst_t *da = nir_def_inst(f, defs, in->a);
        const nyr_inst_t *db = nir_def_inst(f, defs, in->b);
        if (da && db) {
          if ((da->op == NYIR_SAR_I64 && db->op == NYIR_SHL_I64) ||
              (da->op == NYIR_SHL_I64 && db->op == NYIR_SAR_I64)) {
            const nyr_inst_t *shr = da->op == NYIR_SAR_I64 ? da : db;
            const nyr_inst_t *shl = da->op == NYIR_SHL_I64 ? da : db;
            int shr_src = shr->a;
            int shl_src = shl->a;
            int64_t shr_amt = shr->b >= 0 && known[shr->b] ? value[shr->b] : -1;
            int64_t shl_amt = shl->b >= 0 && known[shl->b] ? value[shl->b] : -1;
            /*
             * Only fuse a full-width rotate: shr_amt + shl_amt must equal
             * the 64-bit register width.  A 32-bit pair (e.g. x>>7|x<<25)
             * leaves its high contributions above bit 31, which the
             * following & 0xFFFFFFFF relies on seeing — folding it into a
             * single rorq silently drops the wrapped bits and changes the
             * result (broke sha256's message schedule).
             *
             * SAR sign-fills, so (x SAR s) | (x SHL (64-s)) is only a true
             * rotate when x is provably non-negative; for negative sources
             * the OR cannot clear the propagated sign bits.
             */
            bool src_nonneg = false;
            if (shr_src >= 0 && shr_src < f->next_value) {
              nyr_range_t rr = facts[shr_src].range;
              if (!rr.has_min || !rr.has_max)
                (void)nir_value_range_at(f, facts, shr_src, i, &rr);
              src_nonneg = rr.has_min && rr.min >= 0;
            }
            if (src_nonneg && nir_operands_same_value(f, shr_src, shl_src, i) &&
                shr_amt > 0 && shl_amt > 0 && shr_amt + shl_amt == 64) {
              in->op = NYIR_ROR_I64;
              in->a = shr_src;
              in->b = shr->b;
              nyr_clear_derived_facts(in);
              /*
               * No F_NARROW32 here: the verifier only allows that flag on
               * ADD/SUB/MUL/AND/OR/XOR/DIV/MOD, and no backend consumes it
               * for rotates — annotating made verify reject the function.
               */
              in->effects = NYIR_EFFECT_NONE;
              break;
            }
          }
        }
      }
      break;
    case NYIR_SHL_I64:
    case NYIR_SAR_I64:
      if (bk && bv == 0)
        nir_make_copy(in, in->a);
      else if (ak && av == 0)
        nir_make_const(in, 0);
      break;
    case NYIR_CMP_I64: {
      if (nir_simplify_mod_parity_compare(f, defs, in, i))
        break;
      if (nir_simplify_boolean_compare(f, defs, in))
        break;
      int64_t folded = 0;
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (nir_cmp_same_value(in->cmp, &folded))
          nir_make_const(in, folded);
      } else if (in->a >= 0 && in->b >= 0) {
        nyr_range_t ra = {0};
        nyr_range_t rb = {0};
        const nyr_range_t *pa = &facts[in->a].range;
        const nyr_range_t *pb = &facts[in->b].range;
        /*
         * A global interval can be broader than the interval on this CFG
         * path. Always ask for the instruction-local range so branch guards
         * can narrow known (but imprecise) values as well as unknown ones.
         */
        if (nir_value_range_at(f, facts, in->a, i, &ra))
          pa = &ra;
        if (nir_value_range_at(f, facts, in->b, i, &rb))
          pb = &rb;
        if (nir_cmp_range_fold(in->cmp, pa, pb, &folded))
          nir_make_const(in, folded);
      }
      break;
    }
    /*
     * Float identity folds.  Constants are stored as int64 bitcasts.
     */
#define NY_F64_BITCAST_0 INT64_C(0)
#define NY_F64_BITCAST_1 INT64_C(4607182418800017408)
#define NY_F64_BITCAST_N1 INT64_C(-4616189618054758400)
    case NYIR_ADD_F64:
      /*
       * x + (+0.0) is x only for non-negative-zero x; fast-math only.
       */
      if (nyr_fast_math() && ((bkf64 && bvf64 == NY_F64_BITCAST_0) ||
                              (akf64 && avf64 == NY_F64_BITCAST_0)))
        nir_make_copy(in, bkf64 ? in->a : in->b);
      break;
    case NYIR_SUB_F64:
      /*
       * x - x is +0.0 only for finite x (NaN/Inf give NaN): fast-math.
       */
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (nyr_fast_math())
          nir_make_f64_const(in, NY_F64_BITCAST_0);
      } else if (bkf64 && bvf64 == NY_F64_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F64:
      /*
       * x * (+0.0) is +0.0 only for finite nonzero x: fast-math.
       */
      if (((bkf64 && bvf64 == NY_F64_BITCAST_0) ||
           (akf64 && avf64 == NY_F64_BITCAST_0)) &&
          nyr_fast_math())
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (bkf64 && bvf64 == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (akf64 && avf64 == NY_F64_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F64:
      if (bkf64 && bvf64 == NY_F64_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (nyr_fast_math() && akf64 && avf64 == NY_F64_BITCAST_0 && bkf64 &&
               bvf64 != NY_F64_BITCAST_0)
        nir_make_f64_const(in, NY_F64_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        /*
         * 0/0 and Inf/Inf are NaN: fast-math only.
         */
        if (nyr_fast_math())
          nir_make_f64_const(in, NY_F64_BITCAST_1);
      }
      break;
      /*
       * Float32 identity folds.
       */
#define NY_F32_BITCAST_0 INT64_C(0)
#define NY_F32_BITCAST_1 INT64_C(1065353216)
    case NYIR_ADD_F32:
      /*
       * See the f64 note: fast-math only.
       */
      if (nyr_fast_math() && ((bkf32 && bvf32 == NY_F32_BITCAST_0) ||
                              (akf32 && avf32 == NY_F32_BITCAST_0)))
        nir_make_copy(in, bkf32 ? in->a : in->b);
      break;
    case NYIR_SUB_F32:
      if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (nyr_fast_math())
          nir_make_f32_const(in, NY_F32_BITCAST_0);
      } else if (bkf32 && bvf32 == NY_F32_BITCAST_0)
        nir_make_copy(in, in->a);
      break;
    case NYIR_MUL_F32:
      if (((bkf32 && bvf32 == NY_F32_BITCAST_0) ||
           (akf32 && avf32 == NY_F32_BITCAST_0)) &&
          nyr_fast_math())
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (bkf32 && bvf32 == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (akf32 && avf32 == NY_F32_BITCAST_1)
        nir_make_copy(in, in->b);
      break;
    case NYIR_DIV_F32:
      if (bkf32 && bvf32 == NY_F32_BITCAST_1)
        nir_make_copy(in, in->a);
      else if (nyr_fast_math() && akf32 && avf32 == NY_F32_BITCAST_0 && bkf32 &&
               bvf32 != NY_F32_BITCAST_0)
        nir_make_f32_const(in, NY_F32_BITCAST_0);
      else if (nir_operands_same_value(f, in->a, in->b, i)) {
        if (nyr_fast_math())
          nir_make_f32_const(in, NY_F32_BITCAST_1);
      }
      break;
    default:
      break;
    }
  }
  free(defs);
  free(known);
  free(value);
  free(kind);
  free(facts);

  /*
   * Select-arm rewrites can expose new SSA definitions, so rebuild the def
   * map after each rewrite instead of using the earlier peephole map.
   */
  for (;;) {
    int *select_defs = nyr_build_defs(f);
    if (!select_defs)
      return false;
    bool changed = false;
    bool ok = nir_factor_select_common_add(f, select_defs, &changed);
    free(select_defs);
    if (!ok)
      return false;
    if (!changed)
      break;
  }

  /*
   * Reciprocal division pass: transforms multiple divisions by the same
   * divisor within a basic block into one reciprocal division +
   * multiplications. fl(x * fl(1/d)) double-rounds, so this is only equivalent
   * to x/d under fast-math semantics. Replacing x / d with x * (1 / d) changes
   * IEEE rounding and exceptional behavior. Keep reciprocal factoring behind
   * the same explicit opt-in as the other reassociating floating-point
   * rewrites.
   */
  if (nyr_fast_math()) {
    for (size_t bb_start = 0; bb_start < f->len;) {
      size_t bb_end = f->len;
      for (size_t j = bb_start; j < f->len; ++j) {
        nyr_op_t op = f->data[j].op;
        if (j > bb_start && (op == NYIR_LABEL || op == NYIR_BR ||
                             op == NYIR_BR_IF || op == NYIR_RET)) {
          bb_end = j;
          break;
        }
        if (op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET) {
          bb_end = j + 1;
          break;
        }
      }

      for (size_t j = bb_start; j < bb_end; ++j) {
        if (f->data[j].op != NYIR_DIV_F64 || f->data[j].b < 0)
          continue;
        int divisor = f->data[j].b;

        size_t count = 0;
        for (size_t k = j; k < bb_end; ++k) {
          if (f->data[k].op == NYIR_DIV_F64 && f->data[k].b == divisor)
            count++;
        }

        if (count >= 2) {
          const nyr_debug_loc_t origin_debug = f->data[j].debug;
          if (f->len + 2 > f->cap) {
            size_t new_cap = f->cap ? f->cap * 2 : 64;
            while (new_cap < f->len + 2)
              new_cap *= 2;
            nyr_inst_t *new_data =
                (nyr_inst_t *)realloc(f->data, new_cap * sizeof(nyr_inst_t));
            if (!new_data)
              return false;
            f->data = new_data;
            f->cap = new_cap;
          }

          int v_one = f->next_value++;
          int v_inv = f->next_value++;

          memmove(&f->data[j + 2], &f->data[j],
                  (f->len - j) * sizeof(nyr_inst_t));
          f->len += 2;
          bb_end += 2;

          f->data[j] = (nyr_inst_t){.op = NYIR_CONST_F64,
                                     .dst = v_one,
                                     .a = -1,
                                     .b = -1,
                                     .imm = INT64_C(4607182418800017408),
                                     .scalar_class = NYIR_SCALAR_F64,
                                     .debug = origin_debug};
          f->data[j + 1] = (nyr_inst_t){.op = NYIR_DIV_F64,
                                         .dst = v_inv,
                                         .a = v_one,
                                         .b = divisor,
                                         .effects = NYIR_EFFECT_NONE,
                                         .scalar_class = NYIR_SCALAR_F64,
                                         .debug = origin_debug};

          for (size_t k = j + 2; k < bb_end; ++k) {
            if (f->data[k].op == NYIR_DIV_F64 && f->data[k].b == divisor) {
              f->data[k].op = NYIR_MUL_F64;
              f->data[k].b = v_inv;
              nyr_clear_derived_facts(&f->data[k]);
              f->data[k].effects = nyr_inst_effects(&f->data[k]);
            }
          }
          j += 1;
        }
      }

      bb_start = bb_end;
    }
  }
  return true;
}
