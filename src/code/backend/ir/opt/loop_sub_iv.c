/*
 * Loop strength reduction for `inv - i` against an induction variable.
 *
 * For a header PHI i = phi(preheader: i0, latch: i + c) with constant c, a
 * loop-body value t = inv - i (inv defined outside the loop) is the second
 * induction variable
 *
 *   q = phi(preheader: inv - i0, latch: q - c)
 *
 * Both PHIs advance together on the backedge only, so q == inv - i at every
 * point where t could be observed, including after the loop. The identity is
 * exact for wrapping i64 arithmetic. The rewrite trades a per-iteration
 * subtraction of a live invariant for an in-place decrement: two-address
 * targets (x86-64) otherwise copy `inv` before every subtraction.
 */
#include "code/backend/ir/opt/util.h"
#include <stdlib.h>
#include <string.h>

enum { LSI_MAX_REWRITES = 16 };

static int lsi_root(const nyr_func_t *f, const int *defs, int v) {
  for (int depth = 0; depth < 16 && v >= 0 && v < f->next_value; ++depth) {
    int at = defs[v];
    if (at < 0 || f->data[at].op != NYIR_COPY || f->data[at].a < 0)
      return v;
    v = f->data[at].a;
  }
  return v;
}

static bool lsi_const(const nyr_func_t *f, const int *defs, int v,
                      int64_t *out) {
  v = lsi_root(f, defs, v);
  if (v < 0 || v >= f->next_value || defs[v] < 0)
    return false;
  const nyr_inst_t *d = &f->data[defs[v]];
  if (d->op != NYIR_CONST_I64)
    return false;
  *out = d->imm;
  return true;
}

static bool lsi_dominates(const nyr_cfg_t *cfg, size_t a, size_t b) {
  if (!cfg->dominators || a >= cfg->block_count || b >= cfg->block_count)
    return false;
  const uint64_t *row = cfg->dominators + b * cfg->dominator_words;
  return (row[a / 64] >> (a % 64)) & 1u;
}

static bool lsi_plain_i64(const nyr_inst_t *in) {
  return in->dst >= 0 && in->a >= 0 && in->b >= 0 && in->flags == 0 &&
         in->extra_args_len == 0 &&
         nyr_effective_effects(in) == NYIR_EFFECT_NONE &&
         (in->scalar_class == NYIR_SCALAR_I64_RAW ||
          in->scalar_class == NYIR_SCALAR_UNKNOWN);
}

/* Insert `in` at index `at`, shifting the rest of the function down. */
static bool lsi_insert(nyr_func_t *f, size_t at, nyr_inst_t in) {
  if (!nir_ensure_inst_space(f, 1))
    return false;
  memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
  f->data[at] = in;
  f->len++;
  return true;
}

static nyr_inst_t lsi_inst(nyr_op_t op, int dst, int a, int b,
                           nyr_scalar_class_t cls, nyr_debug_loc_t debug) {
  return (nyr_inst_t){.op = op,
                      .dst = dst,
                      .a = a,
                      .b = b,
                      .c = -1,
                      .d = -1,
                      .e = -1,
                      .f = -1,
                      .effects = NYIR_EFFECT_NONE,
                      .scalar_class = cls,
                      .debug = debug};
}

/* Index just before the block's terminator (BR/BR_IF/RET), or its end. */
static size_t lsi_before_terminator(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                    size_t block) {
  size_t end = cfg->block_end[block];
  size_t at = end;
  while (at > cfg->block_start[block]) {
    nyr_op_t op = f->data[at - 1].op;
    if (op == NYIR_NOP || op == NYIR_BR || op == NYIR_BR_IF ||
        op == NYIR_RET) {
      --at;
      continue;
    }
    break;
  }
  return at;
}

/*
 * One rewrite: returns 1 on change, 0 when nothing applies, -1 on failure.
 */
static int lsi_rewrite_once(nyr_func_t *f) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return 0;
  int *defs = nyr_build_defs(f);
  bool *member = calloc(cfg.block_count ? cfg.block_count : 1, sizeof(bool));
  int result = 0;
  if (!defs || !member)
    goto done;
  for (size_t latch = 0; latch < cfg.block_count && !result; ++latch) {
    for (size_t e = cfg.succ_offsets[latch];
         e < cfg.succ_offsets[latch + 1] && !result; ++e) {
      if (!cfg.backedge_edges[e])
        continue;
      size_t header = cfg.succ_blocks[e];
      memset(member, 0, cfg.block_count * sizeof(bool));
      if (!nyr_cfg_natural_loop_blocks(&cfg, latch, header, member,
                                       cfg.block_count))
        continue;
      /* Exactly one predecessor outside the loop: the preheader. */
      size_t pre = SIZE_MAX, outside = 0;
      for (size_t k = cfg.pred_offsets[header];
           k < cfg.pred_offsets[header + 1]; ++k)
        if (!member[cfg.pred_blocks[k]]) {
          pre = cfg.pred_blocks[k];
          ++outside;
        }
      size_t latches = 0;
      for (size_t k = cfg.pred_offsets[header];
           k < cfg.pred_offsets[header + 1]; ++k)
        latches += member[cfg.pred_blocks[k]];
      if (outside != 1 || latches != 1 || cfg.block_label[header] < 0 ||
          cfg.block_label[latch] < 0)
        continue;
      int64_t pre_label = cfg.block_label[pre];
      int64_t latch_label = cfg.block_label[latch];
      /* Candidate IVs: header PHIs with a +c step. */
      for (size_t pi = cfg.block_start[header];
           pi < cfg.block_end[header] && !result; ++pi) {
        const nyr_inst_t *phi = &f->data[pi];
        if (phi->op == NYIR_LABEL || phi->op == NYIR_NOP)
          continue;
        if (phi->op != NYIR_PHI)
          break;
        if (phi->phi_incoming_len != 2 || phi->dst < 0)
          continue;
        int init = -1, next = -1;
        for (size_t k = 0; k < 2; ++k) {
          if (phi->phi_incoming[k].predecessor_label == pre_label)
            init = phi->phi_incoming[k].value;
          else if (phi->phi_incoming[k].predecessor_label == latch_label)
            next = phi->phi_incoming[k].value;
        }
        if (init < 0 || next < 0)
          continue;
        int iv = phi->dst;
        int nr = lsi_root(f, defs, next);
        if (nr < 0 || defs[nr] < 0)
          continue;
        const nyr_inst_t *step_in = &f->data[defs[nr]];
        int64_t step = 0;
        if (step_in->op != NYIR_ADD_I64 || !lsi_plain_i64(step_in) ||
            !((lsi_root(f, defs, step_in->a) == iv &&
               lsi_const(f, defs, step_in->b, &step)) ||
              (lsi_root(f, defs, step_in->b) == iv &&
               lsi_const(f, defs, step_in->a, &step))))
          continue;
        /* A loop-body t = inv - iv with inv available in the preheader. */
        for (size_t ti = 0; ti < f->len && !result; ++ti) {
          const nyr_inst_t *t = &f->data[ti];
          if (t->op != NYIR_SUB_I64 || !lsi_plain_i64(t) ||
              !member[cfg.inst_block[ti]] ||
              lsi_root(f, defs, t->b) != iv)
            continue;
          int inv = t->a;
          if (inv < 0 || inv >= f->next_value || defs[inv] < 0)
            continue;
          size_t inv_block = cfg.inst_block[defs[inv]];
          if (member[inv_block] || !lsi_dominates(&cfg, inv_block, pre))
            continue;
          if (f->next_value > INT32_MAX - 5)
            goto done;
          nyr_scalar_class_t cls = t->scalar_class;
          nyr_debug_loc_t debug = t->debug;
          int t_value = t->dst;
          int q0 = f->next_value++, q = f->next_value++;
          int cst = f->next_value++, qn = f->next_value++;
          size_t latch_at = lsi_before_terminator(f, &cfg, latch);
          size_t header_at = pi + 1;
          while (header_at < cfg.block_end[header] &&
                 f->data[header_at].op == NYIR_PHI)
            ++header_at;
          size_t pre_at = lsi_before_terminator(f, &cfg, pre);
          if (!(latch_at > header_at && header_at > pre_at)) {
            f->next_value -= 4;
            continue;
          }
          nyr_phi_incoming_t *inc = malloc(2 * sizeof(*inc));
          if (!inc) {
            result = -1;
            goto done;
          }
          inc[0] = (nyr_phi_incoming_t){.predecessor_label = pre_label,
                                        .value = q0};
          inc[1] = (nyr_phi_incoming_t){.predecessor_label = latch_label,
                                        .value = qn};
          nyr_inst_t q_phi = {.op = NYIR_PHI,
                              .dst = q,
                              .a = -1,
                              .b = -1,
                              .c = -1,
                              .d = -1,
                              .e = -1,
                              .f = -1,
                              .scalar_class = cls,
                              .phi_incoming = inc,
                              .phi_incoming_len = 2,
                              .debug = debug};
          nyr_inst_t c_in = lsi_inst(NYIR_CONST_I64, cst, -1, -1, cls, debug);
          c_in.imm = step;
          /* t's uses move to q below; drop its definition so it cannot match
           * again (index ti precedes no insertion that runs before this). */
          nyr_inst_discard(&f->data[ti]);
          f->data[ti] = (nyr_inst_t){.op = NYIR_NOP,
                                     .dst = -1,
                                     .a = -1,
                                     .b = -1,
                                     .c = -1,
                                     .d = -1,
                                     .e = -1,
                                     .f = -1};
          /* Highest index first so earlier positions stay valid. */
          if (!lsi_insert(f, latch_at,
                          lsi_inst(NYIR_SUB_I64, qn, q, cst, cls, debug)) ||
              !lsi_insert(f, latch_at, c_in) ||
              !lsi_insert(f, header_at, q_phi) ||
              !lsi_insert(f, pre_at,
                          lsi_inst(NYIR_SUB_I64, q0, inv, init, cls, debug))) {
            result = -1;
            goto done;
          }
          if (!nyr_replace_all_uses(f, t_value, q)) {
            result = -1;
            goto done;
          }
          result = 1;
        }
      }
    }
  }
done:
  free(defs);
  free(member);
  nyr_cfg_free(&cfg);
  return result;
}

bool nyr_loop_sub_iv(nyr_func_t *f) {
  if (!f || !f->len || !nyr_has_loop(f) || getenv("NYTRIX_NO_LOOP_SUB_IV"))
    return true;
  for (int n = 0; n < LSI_MAX_REWRITES; ++n) {
    int r = lsi_rewrite_once(f);
    if (r < 0)
      return false;
    if (r == 0)
      break;
  }
  return true;
}
