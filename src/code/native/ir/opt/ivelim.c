/*
 * Induction-variable elimination: strength-reduces and removes
 * loop induction variables, replacing multiplies with additions.
 *
 * Closed-form accumulation (s = s + i, s = s + i*c, s = s + (i << sh),
 * s = s + (i + c), s = s + i*i, or s = s + i*i*i; i = i + k) is implemented
 * in closed_form_constant_sum() and driven from iv_transform_once().
 *
 * For linear accumulators (acc_power == 1), a constant scale and offset are
 * factored in before the closed-form sum is evaluated:
 *   effective_i0 = i0 * acc_scale + acc_offset
 *   effective_k  = k  * acc_scale
 * This covers s = s + i*c (acc_scale = c), s = s + (i << sh) (acc_scale = 1<<sh),
 * and s = s + (i + c) / s = s + c + i (acc_offset = c).
 *
 * Constant bounds fold to a compile-time constant for constant counter
 * init/step and linear, quadratic, or cubic accumulators.  Runtime bounds
 * accept constant non-zero init/step values with strict or inclusive
 * increasing comparisons and emit a clamped, parity-halved formula.
 * Bounds are resolved through COPY and pure compile-time arithmetic. The
 * header block is rewritten as `L0: formula; br <exit>` so the CFG stays
 * verifier-clean.
 *
 * See also: strength_reduce.c, loop_vectorize.c.
 */
#include "code/native/ir/opt/util.h"
#include "code/native/ir/internal.h"
#include "base/compat.h"
#include "base/common.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * Induction-variable elimination and strength reduction.
 *
 * Work from SSA PHIs rather than looking for impossible self-defining SSA
 * instructions.  Each transformation is performed on a clone and verified
 * before it replaces the input, so an unusual CFG simply misses the
 * optimization instead of corrupting IR.
 */

typedef struct {
  int iv;
  int init_v;
  bool init_is_const;
  int64_t init;
  int update_v;
  size_t phi_idx;
  size_t update_idx;
  int64_t step;
  int64_t pre_label;
  int64_t latch_label;
} basic_iv_t;

typedef struct {
  int64_t header_label;
  size_t header_idx;
  size_t back_idx;
  size_t header_block;
  size_t latch_block;
  basic_iv_t ivs[32];
  size_t iv_count;
} iv_loop_t;

static bool iv_const(const nyir_func_t *f, const int *defs, int v, int64_t *out) {
  if (!f || !defs || v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyir_inst_t *in = &f->data[(size_t)defs[v]];
  if (in->op != NYIR_CONST_I64)
    return false;
  if (out) *out = in->imm;
  return true;
}

/*
 * Resolve a value through COPY chains.  The front end and mem2reg leave
 * `mov` instructions between PHIs and their uses; matching through them
 * keeps every pattern below robust regardless of copy-propagation timing.
 */
static int iv_root_copy(const nyir_func_t *f, const int *defs, int value) {
  while (value >= 0 && value < f->next_value && defs[value] >= 0 &&
         f->data[(size_t)defs[value]].op == NYIR_COPY)
    value = f->data[(size_t)defs[value]].a;
  return value;
}

static int64_t wrap_mul64(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a * (uint64_t)b);
}

static int64_t wrap_add(int64_t a, int64_t b) {
  return (int64_t)((uint64_t)a + (uint64_t)b);
}

/*
 * Exact floor/ceil division by a positive k for the representable i64 range
 * used by the trip-count formulas below (no overflow on a >= -(2^63-1)).
 */
static int64_t iv_div_floor(int64_t a, int64_t k) {
  if (a >= 0)
    return a / k;
  return -((-(a + 1)) / k + 1);
}

static int64_t iv_div_ceil(int64_t a, int64_t k) {
  return -iv_div_floor(-a, k);
}

/*
 * Resolve a compile-time i64 expression through COPY and pure arithmetic
 * definitions.  This deliberately refuses operations whose runtime behavior
 * is not a total, unambiguous i64 constant (for example division by zero).
 */
static bool iv_const_fold_depth(const nyir_func_t *f, const int *defs, int v,
                                int depth, int64_t *out) {
  if (!f || !defs || depth > 16 || v < 0 || v >= f->next_value)
    return false;
  if (iv_const(f, defs, v, out))
    return true;
  int di = defs[v];
  if (di < 0 || (size_t)di >= f->len)
    return false;
  const nyir_inst_t *in = &f->data[(size_t)di];
  if (in->op == NYIR_COPY)
    return iv_const_fold_depth(f, defs, in->a, depth + 1, out);
  int64_t a = 0, b = 0;
  if (in->a < 0 || in->b < 0 ||
      !iv_const_fold_depth(f, defs, in->a, depth + 1, &a) ||
      !iv_const_fold_depth(f, defs, in->b, depth + 1, &b))
    return false;
  uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
  switch (in->op) {
  case NYIR_ADD_I64:
    *out = wrap_add(a, b);
    return true;
  case NYIR_SUB_I64:
    *out = (int64_t)(ua - ub);
    return true;
  case NYIR_MUL_I64:
    *out = wrap_mul64(a, b);
    return true;
  case NYIR_DIV_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a / b;
    return true;
  case NYIR_MOD_I64:
    if (b == 0 || (a == INT64_MIN && b == -1))
      return false;
    *out = a % b;
    return true;
  case NYIR_AND_I64:
    *out = (int64_t)(ua & ub);
    return true;
  case NYIR_OR_I64:
    *out = (int64_t)(ua | ub);
    return true;
  case NYIR_XOR_I64:
    *out = (int64_t)(ua ^ ub);
    return true;
  case NYIR_SHL_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = (int64_t)(ua << (unsigned)b);
    return true;
  case NYIR_SAR_I64:
    if (b < 0 || b >= 64)
      return false;
    *out = a < 0 ? (int64_t)(ua >> (unsigned)b) : a >> (unsigned)b;
    if (a < 0 && b != 0)
      *out |= (int64_t)(UINT64_MAX << (64u - (unsigned)b));
    return true;
  default:
    return false;
  }
}

static bool iv_const_fold(const nyir_func_t *f, const int *defs, int v,
                          int64_t *out) {
  return iv_const_fold_depth(f, defs, v, 0, out);
}

static bool insert_inst(nyir_func_t *f, size_t at, nyir_inst_t in) {
  if (!f || at > f->len || !nir_ensure_inst_space(f, 1))
    return false;
  memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = in;
  f->len++;
  return true;
}

static size_t find_label_idx(const nyir_func_t *f, int64_t label) {
  if (!f) return SIZE_MAX;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm == label)
      return i;
  return SIZE_MAX;
}

static size_t find_backedge_idx(const nyir_func_t *f, int64_t header_label,
                                int64_t latch_label) {
  size_t latch_idx = find_label_idx(f, latch_label);
  if (latch_idx == SIZE_MAX)
    return SIZE_MAX;
  for (size_t i = latch_idx + 1; i < f->len; ++i) {
    if (f->data[i].op == NYIR_LABEL)
      break;
    if (f->data[i].op == NYIR_BR && f->data[i].imm == header_label)
      return i;
  }
  return SIZE_MAX;
}

static size_t header_phi_end(const nyir_func_t *f, size_t header_idx) {
  size_t at = header_idx + 1;
  while (at < f->len && (f->data[at].op == NYIR_PHI || f->data[at].op == NYIR_NOP))
    at++;
  return at;
}

static bool iv_loop_members(const nyir_cfg_t *cfg, const iv_loop_t *lp,
                            bool *member) {
  return cfg && lp && member &&
         nyir_cfg_natural_loop_blocks(cfg, lp->latch_block, lp->header_block,
                                      member, cfg->block_count);
}

/*
 * The closed-form rewrite deletes [header_idx+1, exit_idx) and replaces the
 * header with the formula plus an unconditional branch to the exit.  That is
 * only safe when every block overlapping the deleted range is either a
 * natural-loop member (deleted with the loop) or pure glue reachable
 * exclusively from inside the loop: a label/NOP/branch-only block whose
 * predecessors are all loop members.  This is deliberately more precise than
 * a naive "members fill the whole span" comparison: the front end routinely
 * leaves an empty exit-gate block (L: br <exit>) between the loop header and
 * the latch, which a member==span check rejects even though deleting it with
 * the loop is sound.
 */
static bool iv_loop_removal_safe(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                 const bool *member, size_t header_idx,
                                 size_t exit_idx) {
  if (!f || !cfg || !member || header_idx >= exit_idx)
    return false;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    size_t bs = cfg->block_start[b];
    size_t be = cfg->block_end[b];
    if (be <= header_idx + 1 || bs >= exit_idx)
      continue; /* no overlap with the deleted range */
    if (member[b])
      continue; /* loop body: removed with the loop */
    /*
     * Non-member block inside the deletion range must be removable glue.
     */
    for (size_t e = cfg->pred_offsets[b]; e < cfg->pred_offsets[b + 1]; ++e)
      if (!member[cfg->pred_blocks[e]])
        return false;
    for (size_t i = bs; i < be; ++i) {
      nyir_op_t op = f->data[i].op;
      if (op != NYIR_LABEL && op != NYIR_NOP && op != NYIR_BR)
        return false;
    }
  }
  return true;
}

static bool find_loops(const nyir_func_t *f, const nyir_cfg_t *cfg,
                       const int *defs, iv_loop_t *loops, size_t *count) {
  if (!f || !cfg || !defs || !loops || !count)
    return false;
  *count = 0;
  for (size_t back = 0; back < f->len && *count < 32; ++back) {
    const nyir_inst_t *br = &f->data[back];
    if (br->op != NYIR_BR || br->imm < 0)
      continue;
    size_t latch = cfg->inst_block[back];
    size_t header = SIZE_MAX;
    for (size_t b = 0; b < cfg->block_count; ++b)
      if (cfg->block_label[b] == br->imm) { header = b; break; }
    if (header == SIZE_MAX || !nyir_cfg_is_backedge(cfg, latch, header)) {
      continue;
    }

    iv_loop_t lp = {.header_label = br->imm,
                    .header_idx = cfg->block_start[header],
                    .back_idx = back,
                    .header_block = header,
                    .latch_block = latch};
    bool *member = calloc(cfg->block_count, sizeof(*member));
    if (!member)
      return false;
    if (!iv_loop_members(cfg, &lp, member)) {
      free(member);
      return false;
    }
    int64_t latch_label = cfg->block_label[latch];
    for (size_t i = cfg->block_start[header];
         i < cfg->block_end[header] && lp.iv_count < 32; ++i) {
      const nyir_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || phi->dst < 0 || phi->phi_incoming_len != 2)
        continue;
      int init_v = -1, update_v = -1;
      int64_t pre_label = -1;
      for (size_t k = 0; k < 2; ++k) {
        if (phi->phi_incoming[k].predecessor_label == latch_label)
          update_v = phi->phi_incoming[k].value;
        else {
          init_v = phi->phi_incoming[k].value;
          pre_label = phi->phi_incoming[k].predecessor_label;
        }
      }
      if (init_v < 0 || update_v < 0 || update_v >= f->next_value || defs[update_v] < 0)
        continue;
      size_t ui = (size_t)defs[update_v];
      if (ui == i || ui >= f->len ||
          !member[cfg->inst_block[ui]])
        continue;
      const nyir_inst_t *up = &f->data[ui];
      int step_v = -1;
      int64_t step = 0;
      if (up->op == NYIR_ADD_I64 && up->dst == update_v) {
        if (iv_root_copy(f, defs, up->a) == phi->dst) step_v = up->b;
        else if (iv_root_copy(f, defs, up->b) == phi->dst) step_v = up->a;
      } else if (up->op == NYIR_SUB_I64 && up->dst == update_v &&
                 iv_root_copy(f, defs, up->a) == phi->dst) {
        step_v = up->b;
        if (iv_const(f, defs, step_v, &step))
          step = (int64_t)(0u - (uint64_t)step);
        step_v = -2; /* already decoded */
      }
      if (step_v == -1)
        continue;
      if (step_v >= 0 && !iv_const_fold(f, defs, step_v, &step))
        continue;
      if (step == 0)
        continue;
      basic_iv_t *iv = &lp.ivs[lp.iv_count++];
      *iv = (basic_iv_t){.iv = phi->dst, .init_v = init_v,
                         .update_v = update_v, .phi_idx = i,
                         .update_idx = ui, .step = step,
                         .pre_label = pre_label, .latch_label = latch_label};
      iv->init_is_const = iv_const_fold(f, defs, init_v, &iv->init);
    }
    if (lp.iv_count)
      loops[(*count)++] = lp;
    free(member);
  }
  return true;
}

/*
 * Replace the canonical, constant-bound loop
 *
 *   i = phi(0, i + 1); sum = phi(0, sum + i)
 *   if i >= N: exit; otherwise backedge
 *
 * with a fall-through preheader-to-exit path returning N*(N-1)/2.  This is
 * intentionally narrow: deleting the whole header/body range keeps PHI
 * predecessor sets valid, unlike replacing the accumulator PHI in place.
 */
static bool closed_form_constant_sum(nyir_func_t *f, const nyir_cfg_t *cfg,
                                    const iv_loop_t *lp, const int *defs) {
  if (!f || !cfg || !lp || !defs || lp->header_idx >= lp->back_idx)
    return false;
  const nyir_inst_t *branch = NULL;
  const nyir_inst_t *cmp = NULL;
  size_t exit_idx = SIZE_MAX;
  for (size_t i = lp->header_idx; i < lp->back_idx; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op != NYIR_BR_IF || in->a < 0 || in->imm < 0 ||
        in->a >= f->next_value || defs[in->a] < 0)
      continue;
    const nyir_inst_t *candidate = &f->data[(size_t)defs[in->a]];
    if (candidate->op != NYIR_CMP_I64)
      continue;
    size_t target_idx = find_label_idx(f, in->imm);
    const nyir_inst_t *exit_branch = in;
    if (target_idx != SIZE_MAX && i + 1 < lp->back_idx &&
        f->data[i + 1].op == NYIR_BR) {
      size_t fallthrough_exit = find_label_idx(f, f->data[i + 1].imm);
      if (fallthrough_exit != SIZE_MAX && fallthrough_exit > lp->back_idx) {
        target_idx = fallthrough_exit;
        exit_branch = &f->data[i + 1];
      }
    }
    if (target_idx == SIZE_MAX || target_idx <= lp->header_idx)
      continue;
    exit_idx = target_idx;
    branch = exit_branch;
    cmp = candidate;
    break;
  }
  if (exit_idx == SIZE_MAX)
    return false;

  /*
   * Deleting [header_idx+1, exit_idx) is only safe when every block inside
   * that range is loop body or glue reachable exclusively from the loop.
   * (This replaces the old member==span "contiguous layout" test, which
   * wrongly rejected the empty exit-gate block the front end leaves between
   * the header and the latch.)
   */
  {
    bool *member = calloc(cfg->block_count, sizeof(*member));
    if (!member)
      return false;
    bool member_ok = nyir_cfg_natural_loop_blocks(cfg, lp->latch_block,
                                                  lp->header_block, member,
                                                  cfg->block_count) &&
                     iv_loop_removal_safe(f, cfg, member, lp->header_idx,
                                          exit_idx);
    free(member);
    if (!member_ok)
      return false;
  }

  /*
   * The loop counter has a constant init and step.  The bound is either a
   * compile-time expression or an SSA value, and the runtime path supports
   * increasing strict/inclusive upper bounds.
   */
  const basic_iv_t *index = NULL;
  bool index_on_a = false;
  int64_t cmp_const = 0;
  int cmp_value = -1;
  bool runtime_bound = false;
  for (size_t i = 0; i < lp->iv_count; ++i) {
    const basic_iv_t *iv = &lp->ivs[i];
    if (!iv->init_is_const || iv->step == 0)
      continue;
    if (iv_root_copy(f, defs, cmp->a) == iv->iv) {
      index = iv;
      index_on_a = true;
      if (iv_const_fold(f, defs, cmp->b, &cmp_const))
        break;
      cmp_value = cmp->b;
      runtime_bound = true;
      break;
    }
    if (iv_root_copy(f, defs, cmp->b) == iv->iv) {
      index = iv;
      index_on_a = false;
      if (iv_const_fold(f, defs, cmp->a, &cmp_const))
        break;
      cmp_value = cmp->a;
      runtime_bound = true;
      break;
    }
  }
  /*
   * Runtime formulas support increasing counters with either strict or
   * inclusive upper bounds.  The initial value and step remain constants,
   * while the bound is an SSA value.
   */
  if (!index || index->step <= 0 ||
      (runtime_bound &&
       (!index_on_a || (cmp->cmp != NYIR_CMP_LT &&
                        cmp->cmp != NYIR_CMP_LE))))
    return false;
  if (runtime_bound && (cmp_value < 0 || cmp_value >= f->next_value))
    return false;
  /*
   * Iteration count.  For a counter increasing by k > 0, only the
   * `while i < N` / `while i <= N` forms terminate; their mirrored forms
   * (N < i, N <= i) either never run or loop forever and are never
   * eliminated.  With init i0 and bound N:
   *   i <  N / N > i  -> ceil((N-i0)/k)   iterations
   *   i <= N / N >= i -> floor((N-i0)/k) + 1
   * clamped to zero when the counter starts past the bound.
   */
  int64_t trips = 0;
  int64_t i0 = index->init;
  int64_t k = index->step;
  if (runtime_bound && i0 < 0)
    return false;
  if (!runtime_bound) {
    if (i0 < 0 || cmp_const < 0)
      return false;
    switch (cmp->cmp) {
    case NYIR_CMP_LT:
      trips = index_on_a ? iv_div_ceil(cmp_const - i0, k) : 0;
      break;
    case NYIR_CMP_LE:
      trips = index_on_a ? iv_div_floor(cmp_const - i0, k) + 1 : 0;
      break;
    case NYIR_CMP_GT:
      trips = index_on_a ? 0 : iv_div_ceil(cmp_const - i0, k);
      break;
    case NYIR_CMP_GE:
      trips = index_on_a ? 0 : iv_div_floor(cmp_const - i0, k) + 1;
      break;
    default:
      return false;
    }
    if (trips <= 0)
      return false;
  }
  int64_t bound = trips;

  int acc_value = -1;
  int64_t acc_init = 0;
  int acc_power = 0;
  int64_t acc_scale = 1;
  int64_t acc_offset = 0;
  for (size_t i = lp->header_idx + 1; i < header_phi_end(f, lp->header_idx);
       ++i) {
    const nyir_inst_t *phi = &f->data[i];
    if (phi->op != NYIR_PHI || phi->dst == index->iv ||
        phi->phi_incoming_len != 2)
      continue;
    int init = -1, update = -1;
    for (size_t k = 0; k < phi->phi_incoming_len; ++k) {
      if (phi->phi_incoming[k].predecessor_label == index->latch_label)
        update = phi->phi_incoming[k].value;
      else
        init = phi->phi_incoming[k].value;
    }
    int64_t initial = 0;
    if (init < 0 || update < 0 || !iv_const(f, defs, init, &initial) ||
        update >= f->next_value || defs[update] < 0)
      continue;
    const nyir_inst_t *add = &f->data[(size_t)defs[update]];
    if (add->op != NYIR_ADD_I64 || add->dst != update)
      continue;
    int other = -1;
    int64_t extra_c = 0;
    int acc_root = phi->dst;
    if (iv_root_copy(f, defs, add->a) == acc_root && add->b >= 0)
      other = add->b;
    else if (iv_root_copy(f, defs, add->b) == acc_root && add->a >= 0)
      other = add->a;
    else if (add->a >= 0 && add->a < f->next_value && defs[add->a] >= 0 &&
             f->data[(size_t)defs[add->a]].op == NYIR_ADD_I64) {
      const nyir_inst_t *inner = &f->data[(size_t)defs[add->a]];
      if (iv_root_copy(f, defs, inner->a) == acc_root && inner->b >= 0 &&
          iv_const(f, defs, inner->b, &extra_c)) {
        other = add->b;
      } else if (iv_root_copy(f, defs, inner->b) == acc_root && inner->a >= 0 &&
                 iv_const(f, defs, inner->a, &extra_c)) {
        other = add->b;
      }
    } else if (add->b >= 0 && add->b < f->next_value && defs[add->b] >= 0 &&
               f->data[(size_t)defs[add->b]].op == NYIR_ADD_I64) {
      const nyir_inst_t *inner = &f->data[(size_t)defs[add->b]];
      if (iv_root_copy(f, defs, inner->a) == acc_root && inner->b >= 0 &&
          iv_const(f, defs, inner->b, &extra_c)) {
        other = add->a;
      } else if (iv_root_copy(f, defs, inner->b) == acc_root && inner->a >= 0 &&
                 iv_const(f, defs, inner->a, &extra_c)) {
        other = add->a;
      }
    }
    if (other < 0)
      continue;
    int other_root = iv_root_copy(f, defs, other);
    if (other_root == index->iv) {
      /*
       * s = s + i
       */
      acc_value = phi->dst;
      acc_init = initial;
      acc_power = 1;
      acc_scale = 1;
      acc_offset = extra_c;
      break;
    }
    if (other < f->next_value && defs[other] >= 0) {
      const nyir_inst_t *term = &f->data[(size_t)defs[other]];
      if (term->op == NYIR_ADD_I64 &&
          ((iv_root_copy(f, defs, term->a) == index->iv && term->b >= 0) ||
           (iv_root_copy(f, defs, term->b) == index->iv && term->a >= 0))) {
        int const_op =
            iv_root_copy(f, defs, term->a) == index->iv ? term->b : term->a;
        int64_t c = 0;
        if (iv_const(f, defs, const_op, &c)) {
          /*
           * s = s + (i + c)
           */
          acc_value = phi->dst;
          acc_init = initial;
          acc_power = 1;
          acc_scale = 1;
          acc_offset = wrap_add(extra_c, c);
          break;
        }
      } else if (term->op == NYIR_MUL_I64 &&
                 ((iv_root_copy(f, defs, term->a) == index->iv && term->b >= 0) ||
                  (iv_root_copy(f, defs, term->b) == index->iv && term->a >= 0))) {
        int other_op =
            iv_root_copy(f, defs, term->a) == index->iv ? term->b : term->a;
        int64_t c = 0;
        if (iv_const(f, defs, other_op, &c)) {
          /*
           * s = s + i * c
           */
          acc_value = phi->dst;
          acc_init = initial;
          acc_power = 1;
          acc_scale = c;
          acc_offset = extra_c;
          break;
        }
        if (iv_root_copy(f, defs, other_op) == index->iv) {
          /*
           * s = s + i*i
           */
          acc_value = phi->dst;
          acc_init = initial;
          acc_power = 2;
          break;
        }
        if (other_op < f->next_value && defs[other_op] >= 0) {
          const nyir_inst_t *square_def = &f->data[(size_t)defs[other_op]];
          if (square_def->op == NYIR_MUL_I64 &&
              (iv_root_copy(f, defs, square_def->a) == index->iv &&
               iv_root_copy(f, defs, square_def->b) == index->iv)) {
            /*
             * s = s + i*i*i
             */
            acc_value = phi->dst;
            acc_init = initial;
            acc_power = 3;
            break;
          }
        }
      } else if (term->op == NYIR_SHL_I64 &&
                 iv_root_copy(f, defs, term->a) == index->iv && term->b >= 0) {
        int64_t shift = 0;
        if (iv_const(f, defs, term->b, &shift) && shift >= 0 && shift < 62) {
          /*
           * s = s + (i << shift)
           */
          acc_value = phi->dst;
          acc_init = initial;
          acc_power = 1;
          acc_scale = (int64_t)1 << shift;
          acc_offset = extra_c;
          break;
        }
      }
    }
  }

  if (acc_value < 0)
    return false;
  /*
   * Runtime formulas support linear accumulators (acc_power == 1) with any
   * constant initial value: the formula emits acc_init as a CONST_I64 and
   * adds it to the closed-form sum.  Quadratic/cubic sums require a
   * constant trip count because the higher-order closed forms need T at
   * compile time.
   */
  if (runtime_bound && acc_power != 1)
    return false;

  int64_t i0_for_trips = i0;
  if (acc_power == 1) {
    i0 = wrap_add(wrap_mul64(i0, acc_scale), acc_offset);
    k = wrap_mul64(k, acc_scale);
  }

  nyir_inst_t formula[32] = {0};
  size_t formula_count = 0;
  int result_value = -1;
  if (!runtime_bound) {
    int64_t T = bound;
    /*
     * T*(T-1)/2 with the even factor halved first (exact mod 2^64).
     */
    int64_t hl = T, hr = T - 1;
    if ((hl & 1) == 0)
      hl /= 2;
    else
      hr /= 2;
    int64_t sum_j = wrap_mul64(hl, hr);
    int64_t total;
    if (acc_power >= 2) {
      /*
       * s = s + i^p, p in {2, 3}.  Expand i = i0 + k*j and use the
       * closed forms for sum(j^2) and sum(j^3).
       */
      if (T > ((int64_t)1 << 62))
        return false;
      int64_t f1 = T, f2 = T - 1, f3 = 2 * T - 1;
      if ((f1 & 1) == 0)
        f1 /= 2;
      else if ((f2 & 1) == 0)
        f2 /= 2;
      else
        f3 /= 2;
      if (f1 % 3 == 0)
        f1 /= 3;
      else if (f2 % 3 == 0)
        f2 /= 3;
      else
        f3 /= 3;
      int64_t sum_j2 = wrap_mul64(wrap_mul64(f1, f2), f3);
      int64_t i0_sq = wrap_mul64(i0, i0);
      int64_t k_sq = wrap_mul64(k, k);
      total = acc_init;
      if (acc_power == 2) {
        total = wrap_add(total, wrap_mul64(T, i0_sq));
        total = wrap_add(total,
                         wrap_mul64(wrap_mul64(i0, 2),
                                    wrap_mul64(k, sum_j)));
        total = wrap_add(total, wrap_mul64(k_sq, sum_j2));
      } else {
        int64_t sum_j3 = wrap_mul64(sum_j, sum_j);
        int64_t i0_cube = wrap_mul64(i0_sq, i0);
        int64_t k_cube = wrap_mul64(k_sq, k);
        total = wrap_add(total, wrap_mul64(T, i0_cube));
        total = wrap_add(
            total, wrap_mul64(wrap_mul64(wrap_mul64(i0_sq, k), 3),
                              sum_j));
        total = wrap_add(
            total, wrap_mul64(wrap_mul64(wrap_mul64(i0, k_sq), 3),
                              sum_j2));
        total = wrap_add(total, wrap_mul64(k_cube, sum_j3));
      }
    } else {
      /*
       * s = s + i: closed form is s0 + T*i0 + k*sum_j.
       */
      total = acc_init;
      total = wrap_add(total, wrap_mul64(T, i0));
      total = wrap_add(total, wrap_mul64(k, sum_j));
    }
    result_value = f->next_value++;
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = result_value,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = total};
  } else {
    int zero = f->next_value++;
    int one = f->next_value++;
    int two = f->next_value++;
    int step_value = f->next_value++;
    int init_value_for_trips = f->next_value++;
    int init_value = f->next_value++;
    int diff = f->next_value++;
    int in_range = f->next_value++;
    int mask = f->next_value++;
    int clamped = f->next_value++;
    int raw_trips = f->next_value++;
    int remainder = f->next_value++;
    int has_remainder = f->next_value++;
    int normalized_trips = f->next_value++;
    int trips = f->next_value++;
    int trips_minus_one = f->next_value++;
    int half = f->next_value++;
    int even_prod = f->next_value++;
    int odd_half = f->next_value++;
    int odd_prod = f->next_value++;
    int bit = f->next_value++;
    int delta = f->next_value++;
    int weighted = f->next_value++;
    int sum_j = f->next_value++;
    int step_term = f->next_value++;
    int init_term = f->next_value++;
    int acc_value_const = f->next_value++;
    int base_sum = f->next_value++;
    result_value = f->next_value++;
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = zero,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 0};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = one,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = two,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = 2};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = step_value,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = k};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = init_value_for_trips,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = i0_for_trips};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = init_value,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = i0};
    /*
     * The trip count uses the *original* loop counter initial value
     * (i0_for_trips), not the accumulator-adjusted i0.  When acc_offset
     * is nonzero (e.g. sum += 200 + i), the adjusted i0 would shift the
     * trip-count window by the offset, computing max(0, N-200) instead of
     * N and zeroing the result for small bounds.
     */
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_SUB_I64, .dst = diff,
                      .a = cmp_value, .b = init_value_for_trips, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    /*
     * `diff = N - i0` wraps when N is far below i0 (e.g. i0 = 2^62 and
     * N = INT64_MIN), which would make a `diff > 0` test report a positive
     * trip count for a loop that never runs.  Test N against i0 directly
     * instead: N - i0 cannot overflow whenever the direct comparison says
     * the loop runs, so the clamped diff is always genuine there.
     */
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CMP_I64, .dst = in_range,
                      .a = cmp_value, .b = init_value_for_trips, .c = -1, .d = -1,
                      .e = -1, .f = -1, .cmp = cmp->cmp == NYIR_CMP_LT
                                                     ? NYIR_CMP_GT
                                                     : NYIR_CMP_GE};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_SUB_I64, .dst = mask,
                      .a = zero, .b = in_range, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = clamped,
                      .a = diff, .b = mask, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    if (cmp->cmp == NYIR_CMP_LT) {
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_DIV_I64, .dst = raw_trips,
                        .a = clamped, .b = step_value, .c = -1, .d = -1,
                        .e = -1, .f = -1};
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_MOD_I64, .dst = remainder,
                        .a = clamped, .b = step_value, .c = -1, .d = -1,
                        .e = -1, .f = -1};
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_CMP_I64, .dst = has_remainder,
                        .a = remainder, .b = zero, .c = -1, .d = -1,
                        .e = -1, .f = -1, .cmp = NYIR_CMP_NE};
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = normalized_trips,
                        .a = raw_trips, .b = has_remainder, .c = -1, .d = -1,
                        .e = -1, .f = -1};
    } else {
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_DIV_I64, .dst = raw_trips,
                        .a = clamped, .b = step_value, .c = -1, .d = -1,
                        .e = -1, .f = -1};
      formula[formula_count++] =
          (nyir_inst_t){.op = NYIR_ADD_I64, .dst = normalized_trips,
                        .a = raw_trips, .b = one, .c = -1, .d = -1,
                        .e = -1, .f = -1};
    }
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = trips,
                      .a = normalized_trips, .b = mask, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_SUB_I64, .dst = trips_minus_one,
                      .a = trips, .b = one, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_DIV_I64, .dst = half,
                      .a = trips, .b = two, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_MUL_I64, .dst = even_prod,
                      .a = half, .b = trips_minus_one, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_DIV_I64, .dst = odd_half,
                      .a = trips_minus_one, .b = two, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_MUL_I64, .dst = odd_prod,
                      .a = trips, .b = odd_half, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_AND_I64, .dst = bit,
                      .a = trips, .b = one, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_SUB_I64, .dst = delta,
                      .a = odd_prod, .b = even_prod, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_MUL_I64, .dst = weighted,
                      .a = bit, .b = delta, .c = -1, .d = -1, .e = -1,
                      .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_ADD_I64, .dst = sum_j,
                      .a = even_prod, .b = weighted, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_MUL_I64, .dst = step_term,
                      .a = step_value, .b = sum_j, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_MUL_I64, .dst = init_term,
                      .a = trips, .b = init_value, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_CONST_I64, .dst = acc_value_const,
                      .a = -1, .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                      .imm = acc_init};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_ADD_I64, .dst = base_sum,
                      .a = acc_value_const, .b = init_term, .c = -1, .d = -1,
                      .e = -1, .f = -1};
    formula[formula_count++] =
        (nyir_inst_t){.op = NYIR_ADD_I64, .dst = result_value,
                      .a = base_sum, .b = step_term, .c = -1, .d = -1,
                      .e = -1, .f = -1};
  }

  /*
   * Bail if the exit block feeds a PHI from the removed loop: any value that
   * entered through the header's exit branch is a header PHI result whose
   * definition disappears with the loop body.  Only the accumulator, which
   * is rewritten to the closed-form constant below, can survive.
   */
  for (size_t i = exit_idx + 1; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL)
      break;
    if (in->op == NYIR_PHI)
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value != acc_value)
          return false;
  }

  /*
   * Free the header PHI incoming arrays before the loop body is shifted out;
   * otherwise the memmove below leaks every header PHI's heap edges.
   */
  for (size_t i = lp->header_idx + 1; i < header_phi_end(f, lp->header_idx); ++i) {
    nyir_inst_t *phi = &f->data[i];
    if (phi->op == NYIR_PHI && phi->phi_incoming) {
      free(phi->phi_incoming);
      phi->phi_incoming = NULL;
      phi->phi_incoming_len = 0;
    }
  }

  /*
   * Replace the loop with: L0: <formula>; br <exit label>, followed by the
   * exit block shifted into place.  The header label is kept (the preheader
   * still falls through to it) and the explicit branch keeps the exit block
   * reachable, so the CFG stays fully connected for the verifier; the loop
   * body [header_idx+1, exit_idx) is removed.
   */
  int64_t exit_label = branch->imm;
  size_t body_removed = exit_idx - (lp->header_idx + 1);
  size_t tail_len = f->len - exit_idx;
  if (!nir_ensure_inst_space(f, formula_count + 1))
    return false;
  memmove(&f->data[lp->header_idx + formula_count + 2], &f->data[exit_idx],
          tail_len * sizeof(*f->data));
  memcpy(&f->data[lp->header_idx + 1], formula,
         formula_count * sizeof(*formula));
  f->data[lp->header_idx + formula_count + 1] =
      (nyir_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                    .c = -1, .d = -1, .e = -1, .f = -1, .imm = exit_label,
                    .effects = NYIR_EFFECT_CONTROL};
  f->len = f->len - body_removed + formula_count + 1;

  /*
   * The exit block now sits right after the constant + branch.  Fix its
   * PHIs: the latch incoming edge is gone with the loop, and any value that
   * entered from the latch (the accumulator) is now the closed-form
   * constant from the preheader edge.
   */
  size_t exit_now = lp->header_idx + formula_count + 2;
  if (exit_now < f->len && f->data[exit_now].op == NYIR_LABEL)
    exit_now++;
  for (size_t i = exit_now; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL)
      break; /* past the exit block */
    if (in->op != NYIR_PHI)
      continue;
    size_t out = 0;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      if (in->phi_incoming[k].predecessor_label == index->latch_label)
        continue; /* latch edge died with the loop */
      if (in->phi_incoming[k].value == acc_value)
        in->phi_incoming[k].value = result_value;
      if (out != k)
        in->phi_incoming[out] = in->phi_incoming[k];
      out++;
    }
    if (out == 0) {
      /*
       * The exit value was only reachable through the loop: rewrite the
       * PHI to the closed-form constant so downstream code stays valid.
       */
      nyir_inst_t *phi = in;
      int dst = phi->dst;
      free(phi->phi_incoming);
      *phi = (nyir_inst_t){.op = NYIR_COPY, .dst = dst, .a = result_value,
                           .b = -1, .c = -1, .d = -1, .e = -1, .f = -1,
                           .effects = NYIR_EFFECT_NONE};
    } else if (out < in->phi_incoming_len) {
      nyir_phi_incoming_t *keep = malloc(out * sizeof(*keep));
      if (!keep)
        return false;
      memcpy(keep, in->phi_incoming, out * sizeof(*keep));
      free(in->phi_incoming);
      in->phi_incoming = keep;
      in->phi_incoming_len = out;
    }
  }

  /*
   * Redirect every remaining use of the accumulator to the constant.
   */
  if (!nyir_replace_all_uses(f, acc_value, result_value))
    return false;
  return true;
}

static bool value_only_used_by(const nyir_use_def_t *uses, int value,
                               size_t only_user) {
  if (!uses || value < 0 || (size_t)value >= uses->value_count)
    return false;
  size_t begin = uses->offsets[value], end = uses->offsets[value + 1];
  if (begin == end)
    return false;
  for (size_t i = begin; i < end; ++i)
    if (uses->users[i] != only_user)
      return false;
  return true;
}

static bool iv_uses_local_to_loop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                                  const bool *member,
                                  const nyir_use_def_t *uses, int value,
                                  const iv_loop_t *lp, size_t update_idx) {
  if (!f || !cfg || !member || !uses || !lp || value < 0 ||
      (size_t)value >= uses->value_count)
    return false;
  for (size_t u = uses->offsets[value]; u < uses->offsets[value + 1]; ++u) {
    size_t user = uses->users[u];
    if (user == update_idx)
      continue;
    if (user >= f->len || !member[cfg->inst_block[user]] ||
        f->data[user].op == NYIR_PHI)
      return false;
  }
  return true;
}

/*
 * Eliminate one redundant recurrence.
 */
static int eliminate_redundant_once(nyir_func_t *f, const nyir_cfg_t *cfg,
                                    const iv_loop_t *lp) {
  nyir_use_def_t uses = {0};
  bool *member = calloc(cfg->block_count, sizeof(*member));
  if (!member || !iv_loop_members(cfg, lp, member) ||
      !nyir_build_use_def(f, &uses)) {
    free(member);
    return -1;
  }
  for (size_t i = 0; i < lp->iv_count; ++i) {
    for (size_t j = i + 1; j < lp->iv_count; ++j) {
      const basic_iv_t *a = &lp->ivs[i], *b = &lp->ivs[j];
      if (a->step != b->step)
        continue;
      bool same_init = a->init_v == b->init_v ||
                       (a->init_is_const && b->init_is_const && a->init == b->init);
      if (same_init) {
        if (!nyir_replace_all_uses(f, b->iv, a->iv) ||
            !nyir_replace_all_uses(f, b->update_v, a->update_v)) {
          nyir_use_def_free(&uses);
          free(member);
          return -1;
        }
        nyir_erase_instruction(f, b->phi_idx);
        nyir_erase_instruction(f, b->update_idx);
        nyir_use_def_free(&uses);
        free(member);
        return 1;
      }
      if (!a->init_is_const || !b->init_is_const ||
          !value_only_used_by(&uses, b->update_v, b->phi_idx) ||
          !iv_uses_local_to_loop(f, cfg, member, &uses, b->iv, lp,
                                 b->update_idx))
        continue;

      int64_t delta = (int64_t)((uint64_t)b->init - (uint64_t)a->init);
      int delta_v = f->next_value++;
      int adjusted = f->next_value++;
      size_t at = header_phi_end(f, lp->header_idx);
      if (!nir_ensure_inst_space(f, 2)) {
        nyir_use_def_free(&uses);
        return -1;
      }
      nyir_inst_t c = {.op = NYIR_CONST_I64, .dst = delta_v, .a = -1, .b = -1,
                       .c = -1, .d = -1, .e = -1, .f = -1, .imm = delta};
      nyir_inst_t add = {.op = NYIR_ADD_I64, .dst = adjusted, .a = a->iv,
                         .b = delta_v, .c = -1, .d = -1, .e = -1, .f = -1,
                         .effects = NYIR_EFFECT_NONE};
      memmove(&f->data[at + 2], &f->data[at], (f->len - at) * sizeof(*f->data));
      f->data[at] = c;
      f->data[at + 1] = add;
      f->len += 2;
      /*
       * Indices at/after insertion moved by two.
       */
      size_t b_phi = b->phi_idx >= at ? b->phi_idx + 2 : b->phi_idx;
      size_t b_update = b->update_idx >= at ? b->update_idx + 2 : b->update_idx;
      if (!nyir_replace_all_uses(f, b->iv, adjusted)) {
        nyir_use_def_free(&uses);
        return -1;
      }
      nyir_erase_instruction(f, b_phi);
      nyir_erase_instruction(f, b_update);
      nyir_use_def_free(&uses);
      free(member);
      return 1;
    }
  }
  nyir_use_def_free(&uses);
  free(member);
  return 0;
}

static bool find_block_by_label(const nyir_cfg_t *cfg, int64_t label, size_t *out) {
  if (!cfg) return false;
  for (size_t b = 0; b < cfg->block_count; ++b)
    if (cfg->block_label[b] == label) {
      if (out) *out = b;
      return true;
    }
  return false;
}

/*
 * Strength-reduce one d = iv * K into a derived PHI recurrence:
 *   d0 = init*K; d = phi(d0,d.next); d.next = d + step*K.
 */
static int strength_reduce_once(nyir_func_t *f, const nyir_cfg_t *cfg,
                                const int *defs, const iv_loop_t *lp) {
  nyir_use_def_t uses = {0};
  bool *member = calloc(cfg->block_count, sizeof(*member));
  if (!member || !iv_loop_members(cfg, lp, member) ||
      !nyir_build_use_def(f, &uses)) {
    free(member);
    return -1;
  }
  for (size_t v = 0; v < lp->iv_count; ++v) {
    const basic_iv_t *iv = &lp->ivs[v];
    if (!iv->init_is_const)
      continue;
    for (size_t idx = 0; idx < f->len; ++idx) {
      if (!member[cfg->inst_block[idx]])
        continue;
      const nyir_inst_t *in = &f->data[idx];
      if (idx == iv->update_idx || in->op != NYIR_MUL_I64 || in->dst < 0)
        continue;
      int factor_v = -1;
      if (iv_root_copy(f, defs, in->a) == iv->iv) factor_v = in->b;
      else if (iv_root_copy(f, defs, in->b) == iv->iv) factor_v = in->a;
      else continue;
      int64_t factor = 0;
      int old_dst = in->dst;
      if (!iv_const(f, defs, factor_v, &factor) || factor == 0 || factor == 1 || factor == -1)
        continue;
      /*
       * The old scalar result may be used outside the loop only where the
       * header value dominates; normal SSA permits that, but keeping this
       * local makes the transform independent of exit-PHI conventions.
       */
      bool local = true;
      for (size_t u = uses.offsets[old_dst]; u < uses.offsets[old_dst + 1]; ++u) {
        size_t user = uses.users[u];
        if (user <= idx || user >= f->len ||
            !member[cfg->inst_block[user]]) {
          local = false;
          break;
        }
      }
      if (!local)
        continue;

      size_t pre_block = SIZE_MAX;
      if (!find_block_by_label(cfg, iv->pre_label, &pre_block))
        continue;
      size_t pre_at = cfg->block_end[pre_block];
      if (pre_at > 0) {
        nyir_op_t tail = f->data[pre_at - 1].op;
        if (tail == NYIR_BR || tail == NYIR_BR_IF || tail == NYIR_RET)
          pre_at--;
      }

      int init_c = f->next_value++;
      int step_c = f->next_value++;
      int dphi = f->next_value++;
      int dnext = f->next_value++;
      int64_t d0 = wrap_mul64(iv->init, factor);
      int64_t ds = wrap_mul64(iv->step, factor);
      if (!nir_ensure_inst_space(f, 4)) {
        nyir_use_def_free(&uses);
        free(member);
        return -1;
      }
      nyir_inst_t c0 = {.op = NYIR_CONST_I64, .dst = init_c, .a = -1, .b = -1,
                        .c = -1, .d = -1, .e = -1, .f = -1, .imm = d0};
      nyir_inst_t cs = {.op = NYIR_CONST_I64, .dst = step_c, .a = -1, .b = -1,
                        .c = -1, .d = -1, .e = -1, .f = -1, .imm = ds};
      if (!insert_inst(f, pre_at, c0) || !insert_inst(f, pre_at + 1, cs)) {
        nyir_use_def_free(&uses);
        free(member);
        return -1;
      }

      size_t hidx = find_label_idx(f, lp->header_label);
      if (hidx == SIZE_MAX) { nyir_use_def_free(&uses); return -1; }
      size_t phi_at = header_phi_end(f, hidx);
      nyir_phi_incoming_t *incoming = malloc(2 * sizeof(*incoming));
      if (!incoming) { nyir_use_def_free(&uses); return -1; }
      incoming[0] = (nyir_phi_incoming_t){.predecessor_label = iv->pre_label,
                                          .value = init_c};
      incoming[1] = (nyir_phi_incoming_t){.predecessor_label = iv->latch_label,
                                          .value = dnext};
      nyir_inst_t phi = {.op = NYIR_PHI, .dst = dphi, .a = -1, .b = -1,
                         .c = -1, .d = -1, .e = -1, .f = -1,
                         .phi_incoming = incoming, .phi_incoming_len = 2};
      if (!insert_inst(f, phi_at, phi)) {
        free(incoming);
        nyir_use_def_free(&uses);
        free(member);
        return -1;
      }

      /*
       * Find the original multiply by destination after the insertions.
       */
      size_t mul_idx = SIZE_MAX;
      for (size_t k = 0; k < f->len; ++k)
        if (f->data[k].dst == old_dst && f->data[k].op == NYIR_MUL_I64) {
          mul_idx = k; break;
        }
      if (mul_idx == SIZE_MAX || !nyir_replace_all_uses(f, old_dst, dphi)) {
        nyir_use_def_free(&uses);
        free(member);
        return -1;
      }
      nyir_erase_instruction(f, mul_idx);

      size_t back = find_backedge_idx(f, lp->header_label, iv->latch_label);
      if (back == SIZE_MAX) { nyir_use_def_free(&uses); free(member); return -1; }
      nyir_inst_t next = {.op = NYIR_ADD_I64, .dst = dnext, .a = dphi,
                          .b = step_c, .c = -1, .d = -1, .e = -1, .f = -1,
                          .effects = NYIR_EFFECT_NONE};
      if (!insert_inst(f, back, next)) {
        nyir_use_def_free(&uses);
        free(member);
        return -1;
      }
      nyir_use_def_free(&uses);
      free(member);
      return 1;
    }
  }
  nyir_use_def_free(&uses);
  free(member);
  return 0;
}

/*
 * Return 1 for one transform, 0 for none, -1 for allocation/build failure.
 */
static int iv_transform_once(nyir_func_t *f) {
  nyir_cfg_t cfg = {0};
  if (!nyir_cfg_build(f, &cfg))
    return -1;
  int *defs = nyir_build_defs(f);
  if (!defs) { nyir_cfg_free(&cfg); return -1; }
  iv_loop_t loops[32];
  size_t loop_count = 0;
  if (!find_loops(f, &cfg, defs, loops, &loop_count)) {
    free(defs); nyir_cfg_free(&cfg); return -1;
  }

  for (size_t i = 0; i < loop_count; ++i) {
    int rc = eliminate_redundant_once(f, &cfg, &loops[i]);
    if (rc != 0) { free(defs); nyir_cfg_free(&cfg); return rc; }
  }
  for (size_t i = 0; i < loop_count; ++i) {
    int rc = strength_reduce_once(f, &cfg, defs, &loops[i]);
    if (rc != 0) { free(defs); nyir_cfg_free(&cfg); return rc; }
  }
  for (size_t i = 0; i < loop_count; ++i) {
    bool fired = closed_form_constant_sum(f, &cfg, &loops[i], defs);
    if (fired) {
      free(defs);
      nyir_cfg_free(&cfg);
      return 1;
    }
  }
  free(defs);
  nyir_cfg_free(&cfg);
  return 0;
}

bool nyir_iv_elim(nyir_func_t *f) {
  if (!f || f->len < 8 || f->next_value <= 0)
    return true;

  nyir_func_t work = {0};
  if (!nyir_func_clone(f, &work))
    return false;
  bool changed = false;
  for (unsigned round = 0; round < 64; ++round) {
    nyir_func_t candidate = {0};
    if (!nyir_func_clone(&work, &candidate)) {
      nyir_func_free(&work);
      return false;
    }
    int rc = iv_transform_once(&candidate);
    if (rc < 0) {
      nyir_func_free(&candidate);
      nyir_func_free(&work);
      return false;
    }
    if (rc == 0) {
      nyir_func_free(&candidate);
      break;
    }
    char err[256] = {0};
    if (!nyir_verify(&candidate, err, sizeof(err))) {
      nyir_func_free(&candidate);
      break; /* reject this conservative opportunity */
    }
    nyir_func_free(&work);
    work = candidate;
    changed = true;
  }

  if (!changed) {
    nyir_func_free(&work);
    return true;
  }
  nyir_func_free(f);
  *f = work;
  return true;
}
