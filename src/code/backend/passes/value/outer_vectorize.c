/*
 * Widen a loop over independent iterations to four i64 lanes (AVX2).
 *
 * Candidate: a loop M whose header PHIs are one +1 induction variable `i`
 * and add-reductions `r += v`, whose only exit is `i < N` with N invariant,
 * and whose body (which may contain inner loops) has no memory access or
 * call. Every other branch in M must be lane-uniform: it may depend on
 * loop-invariant values and on values computed only from them (for example
 * an inner loop counted by an invariant trip count).
 *
 * The pass clones M into a vector loop that runs four consecutive values of
 * `i` per iteration while `i < N && N - i > 3`, then enters the original
 * scalar loop for the remaining iterations. Uniform code stays scalar.
 * Varying code becomes VEC8 i64 operations:
 *
 *   add/sub/and/or/xor        packed op, uniform operands broadcast
 *   mul by small constant     shift-and-add with packed adds
 *   shl by small constant     repeated packed doubling
 *   sar by constant           VEC8_SAR_I64
 *   (x & 1) == 0 / != 0       lane mask (t - 1) / (0 - t)
 *   select(mask, a, b)        b ^ ((a ^ b) & mask)
 *
 * Reductions accumulate per lane and are added horizontally on exit, so the
 * wrapping i64 result equals the scalar sum in any order. The transform is
 * built on a clone and installed only after the verifier accepts it.
 */
#include "code/backend/passes/util.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define OV_REJECT(why)                                                          \
  do {                                                                          \
    if (getenv("NYTRIX_TRACE_OUTER_VEC"))                                       \
      fprintf(stderr, "outer-vectorize: reject %s (line %d) len=%zu hdr=%zu\n", \
              why, __LINE__, c->f->len, c->header);                             \
    return false;                                                               \
  } while (0)

enum {
  OV_MAX_LOOP_INSTS = 2048,
  OV_MAX_MUL = 64,
  OV_MAX_SHL = 8,
  OV_LANES = 4,
};

enum { OV_U = 0, OV_V = 1 };

typedef struct {
  nyr_func_t *f;
  nyr_cfg_t cfg;
  int *defs;
  bool *member;  /* blocks of M */
  uint8_t *cls;  /* per value: OV_U / OV_V */
  int *smap;     /* scalar clone of value */
  int *vmap;     /* vector clone of value (lanes 0-3) */
  int *vmap2;    /* second register of an interleaved value (lanes 4-7) */
  int il;        /* interleave: 1 or 2 four-lane registers per value */
  int half;      /* register half being emitted (0 or 1) */
  int *bcast;    /* hoisted broadcast of an outside-M value */
  int *uvmap;    /* vector shadow of a lane-uniform in-loop value */
  bool *need_uv; /* uniform in-loop values that vector code consumes */
  int value_cap;
  size_t header, latch, preheader, cond_block, exit_block;
  bool exit_fallthrough;
  int iv, iv_init, iv_next, cond_value, bound;
  int *red_phi, *red_init, *red_next, *red_vec;
  size_t red_len;
  int64_t label_base, vx_label, vp_label;
} ov_ctx_t;

static void ov_free(ov_ctx_t *c) {
  nyr_cfg_free(&c->cfg);
  free(c->defs);
  free(c->member);
  free(c->cls);
  free(c->smap);
  free(c->vmap);
  free(c->vmap2);
  free(c->bcast);
  free(c->uvmap);
  free(c->need_uv);
  free(c->red_phi);
  free(c->red_init);
  free(c->red_next);
  free(c->red_vec);
}

static const nyr_inst_t *ov_def(const ov_ctx_t *c, int v) {
  if (v < 0 || v >= c->f->next_value || c->defs[v] < 0)
    return NULL;
  return &c->f->data[c->defs[v]];
}

static int ov_root(const ov_ctx_t *c, int v) {
  for (int guard = 0; guard < 64; ++guard) {
    const nyr_inst_t *d = ov_def(c, v);
    if (!d || d->op != NYIR_COPY || d->a < 0)
      return v;
    v = d->a;
  }
  return v;
}

static bool ov_const(const ov_ctx_t *c, int v, int64_t *out) {
  const nyr_inst_t *d = ov_def(c, ov_root(c, v));
  if (!d || d->op != NYIR_CONST_I64)
    return false;
  *out = d->imm;
  return true;
}

static bool ov_in_loop(const ov_ctx_t *c, int v) {
  if (v < 0 || v >= c->f->next_value || c->defs[v] < 0)
    return false;
  size_t b = c->cfg.inst_block[c->defs[v]];
  return b < c->cfg.block_count && c->member[b];
}

static bool ov_block_by_label(const ov_ctx_t *c, int64_t label, size_t *out) {
  return nyr_cfg_block_for_label(&c->cfg, label, out);
}

static const nyr_inst_t *ov_terminator(const ov_ctx_t *c, size_t b,
                                       size_t *at) {
  size_t end = c->cfg.block_end[b];
  for (size_t i = end; i > c->cfg.block_start[b]; --i) {
    const nyr_inst_t *in = &c->f->data[i - 1];
    if (in->op == NYIR_NOP)
      continue;
    if (at)
      *at = i - 1;
    return in;
  }
  return NULL;
}

/*
 * Operands read by an instruction (excluding PHI incomings).
 */
static int ov_operands(const nyr_inst_t *in, int out[6]) {
  int n = 0;
  const int ops[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  size_t count = sizeof(ops) / sizeof(ops[0]);
  /*
   * CONST/LABEL/BR keep immediates in imm; ALLOCA/ADDR_* have no inputs.
   */
  switch (in->op) {
  case NYIR_CONST_I64:
  case NYIR_LABEL:
  case NYIR_BR:
  case NYIR_NOP:
  case NYIR_PHI:
    return 0;
  case NYIR_BR_IF:
  case NYIR_COPY:
    count = 1;
    break;
  case NYIR_SELECT_I64:
    count = 3;
    break;
  default:
    count = 2;
    break;
  }
  for (size_t k = 0; k < count; ++k)
    if (ops[k] >= 0)
      out[n++] = ops[k];
  return n;
}

static bool ov_scalar_op_ok(const nyr_inst_t *in) {
  switch (in->op) {
  case NYIR_NOP:
  case NYIR_LABEL:
  case NYIR_BR:
  case NYIR_BR_IF:
  case NYIR_PHI:
  case NYIR_CONST_I64:
  case NYIR_COPY:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_CMP_I64:
  case NYIR_SELECT_I64:
    return in->extra_args_len == 0 &&
           (in->op == NYIR_NOP || in->op == NYIR_LABEL ||
            in->op == NYIR_BR || in->op == NYIR_BR_IF ||
            in->op == NYIR_PHI ||
            in->scalar_class == NYIR_SCALAR_I64_RAW ||
            in->scalar_class == NYIR_SCALAR_UNKNOWN);
  default:
    return false;
  }
}

/*
 * Analyse loop (latch -> header). Returns true when it is a candidate.
 */
static bool ov_analyse(ov_ctx_t *c, size_t latch, size_t header) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  c->header = header;
  memset(c->member, 0, cfg->block_count * sizeof(*c->member));
  if (!nyr_cfg_natural_loop_blocks(cfg, latch, header, c->member,
                                   cfg->block_count))
    OV_REJECT("ov_analyse");
  c->header = header;
  c->latch = latch;
  c->header = header;
  /*
   * One latch, one labeled outside predecessor.
   */
  size_t preheader = SIZE_MAX;
  for (size_t e = cfg->pred_offsets[header]; e < cfg->pred_offsets[header + 1];
       ++e) {
    size_t p = cfg->pred_blocks[e];
    if (c->member[p]) {
      if (p != latch)
        OV_REJECT("ov_analyse");
      continue;
    }
    if (preheader != SIZE_MAX)
      OV_REJECT("ov_analyse");
    preheader = p;
  }
  if (preheader == SIZE_MAX || cfg->block_label[preheader] < 0)
    OV_REJECT("ov_analyse");
  c->preheader = preheader;
  /*
   * A preheader holding the horizontal reductions marks the scalar
   * remainder loop of an earlier widening; never widen it again.
   */
  for (size_t i = cfg->block_start[preheader]; i < cfg->block_end[preheader];
       ++i)
    if (f->data[i].op == NYIR_VEC8_REDUCE_ADD_I64)
      OV_REJECT("ov_analyse");
  size_t insts = 0;
  /*
   * Every member block labeled; exactly one exit edge.
   */
  c->exit_block = c->cond_block = SIZE_MAX;
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!c->member[b])
      continue;
    /*
     * Unlabeled blocks are fallthrough-only; clones get labels and
     * explicit branches.
     */
    insts += cfg->block_end[b] - cfg->block_start[b];
    for (size_t e = cfg->succ_offsets[b]; e < cfg->succ_offsets[b + 1]; ++e) {
      size_t s = cfg->succ_blocks[e];
      if (c->member[s])
        continue;
      if (c->exit_block != SIZE_MAX)
        OV_REJECT("ov_analyse");
      c->exit_block = s;
      c->cond_block = b;
    }
  }
  if (c->exit_block == SIZE_MAX || insts > OV_MAX_LOOP_INSTS)
    OV_REJECT("ov_analyse");
  /*
   * The exiting block ends either `br.if cond -> inside; br -> exit` or
   * `br.if cond -> inside` falling through to the exit block.
   */
  size_t term_at = 0;
  const nyr_inst_t *term = ov_terminator(c, c->cond_block, &term_at);
  const nyr_inst_t *brif = NULL;
  c->exit_fallthrough = false;
  if (term && term->op == NYIR_BR && term_at > cfg->block_start[c->cond_block]) {
    size_t exit_check = 0;
    if (!ov_block_by_label(c, term->imm, &exit_check) ||
        exit_check != c->exit_block)
      OV_REJECT("ov_analyse");
    brif = &f->data[term_at - 1];
  } else if (term && term->op == NYIR_BR_IF &&
             c->cond_block + 1 == c->exit_block) {
    brif = term;
    c->exit_fallthrough = true;
  } else {
    OV_REJECT("ov_analyse");
  }
  size_t inside = 0;
  if (brif->op != NYIR_BR_IF || !ov_block_by_label(c, brif->imm, &inside) ||
      !c->member[inside])
    OV_REJECT("ov_analyse");
  c->cond_value = brif->a;
  const nyr_inst_t *cmp = ov_def(c, ov_root(c, brif->a));
  if (!cmp || cmp->op != NYIR_CMP_I64 || cmp->cmp != NYIR_CMP_LT)
    OV_REJECT("ov_analyse");
  /*
   * Header PHIs: one +1 IV and add reductions.
   */
  c->iv = -1;
  c->red_len = 0;
  int64_t pre_label = cfg->block_label[preheader];
  int64_t latch_label = cfg->block_label[latch];
  for (size_t i = cfg->block_start[header]; i < cfg->block_end[header]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;
    if (in->phi_incoming_len != 2)
      OV_REJECT("ov_analyse");
    int init = -1, next = -1;
    for (size_t k = 0; k < 2; ++k) {
      if (in->phi_incoming[k].predecessor_label == pre_label)
        init = in->phi_incoming[k].value;
      else if (in->phi_incoming[k].predecessor_label == latch_label)
        next = in->phi_incoming[k].value;
    }
    if (init < 0 || next < 0)
      OV_REJECT("ov_analyse");
    const nyr_inst_t *nd = ov_def(c, ov_root(c, next));
    if (!nd || nd->op != NYIR_ADD_I64)
      OV_REJECT("ov_analyse");
    int64_t step = 0;
    bool iv_like = (ov_root(c, nd->a) == in->dst && ov_const(c, nd->b, &step)) ||
                   (ov_root(c, nd->b) == in->dst && ov_const(c, nd->a, &step));
    if (iv_like && step == 1 && c->iv < 0) {
      c->iv = in->dst;
      c->iv_init = init;
      c->iv_next = ov_root(c, next);
      continue;
    }
    /*
     * Reduction: next = phi + v, phi used only by that add (via copies).
     */
    int other = ov_root(c, nd->a) == in->dst   ? nd->b
                : ov_root(c, nd->b) == in->dst ? nd->a
                                               : -1;
    if (other < 0)
      OV_REJECT("ov_analyse");
    c->red_phi[c->red_len] = in->dst;
    c->red_init[c->red_len] = init;
    c->red_next[c->red_len] = ov_root(c, next);
    if (++c->red_len >= 8)
      OV_REJECT("ov_analyse");
  }
  if (c->iv < 0)
    OV_REJECT("ov_analyse");
  int lhs = ov_root(c, cmp->a), rhs = cmp->b;
  if (lhs != c->iv || ov_in_loop(c, rhs))
    OV_REJECT("ov_analyse");
  c->bound = rhs;
  return true;
}

static bool ov_is_red_phi(const ov_ctx_t *c, int v, size_t *k) {
  for (size_t i = 0; i < c->red_len; ++i)
    if (c->red_phi[i] == v) {
      if (k)
        *k = i;
      return true;
    }
  return false;
}

static bool ov_is_red_next(const ov_ctx_t *c, int v, size_t *k) {
  for (size_t i = 0; i < c->red_len; ++i)
    if (c->red_next[i] == v) {
      if (k)
        *k = i;
      return true;
    }
  return false;
}

/*
 * Is a mask-producing compare: (x & 1) ==/!= 0.
 */
static bool ov_parity_mask(const ov_ctx_t *c, const nyr_inst_t *in, int *bit,
                           bool *eq) {
  if (in->op != NYIR_CMP_I64 ||
      (in->cmp != NYIR_CMP_EQ && in->cmp != NYIR_CMP_NE))
    return false;
  int64_t k = 0;
  int other = -1;
  if (ov_const(c, in->b, &k) && k == 0)
    other = in->a;
  else if (ov_const(c, in->a, &k) && k == 0)
    other = in->b;
  if (other < 0)
    return false;
  const nyr_inst_t *and = ov_def(c, ov_root(c, other));
  int64_t one = 0;
  if (!and || and->op != NYIR_AND_I64 ||
      !((ov_const(c, and->b, &one) && one == 1) ||
        (ov_const(c, and->a, &one) && one == 1)))
    return false;
  *bit = ov_root(c, other);
  *eq = in->cmp == NYIR_CMP_EQ;
  return true;
}

static bool ov_classify(ov_ctx_t *c) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  memset(c->cls, OV_U, (size_t)f->next_value);
  c->cls[c->iv] = OV_V;
  for (size_t k = 0; k < c->red_len; ++k)
    c->cls[c->red_phi[k]] = OV_V;
  bool changed = true;
  for (int round = 0; changed && round < 256; ++round) {
    changed = false;
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!c->member[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->dst < 0 || in->dst >= f->next_value || c->cls[in->dst] == OV_V)
          continue;
        bool v = false;
        if (in->op == NYIR_PHI) {
          for (size_t k = 0; k < in->phi_incoming_len; ++k) {
            int x = in->phi_incoming[k].value;
            v |= x >= 0 && x < f->next_value && c->cls[x] == OV_V;
          }
        } else {
          int ops[6];
          int n = ov_operands(in, ops);
          for (int k = 0; k < n; ++k)
            v |= ops[k] < f->next_value && c->cls[ops[k]] == OV_V;
        }
        if (v) {
          c->cls[in->dst] = OV_V;
          changed = true;
        }
      }
    }
  }
  /*
   * Legality over every instruction of M.
   */
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!c->member[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (!ov_scalar_op_ok(in))
        OV_REJECT("ov_classify");
      if (nyr_effective_effects(in) &
          ~(unsigned)(NYIR_EFFECT_CONTROL | NYIR_EFFECT_MAY_TRAP))
        OV_REJECT("ov_classify");
      if (in->op == NYIR_BR_IF) {
        int cond = in->a;
        if (cond >= 0 && c->cls[cond] == OV_V &&
            !(b == c->cond_block && cond == c->cond_value))
          OV_REJECT("ov_classify");
        continue;
      }
      if (in->dst < 0 || c->cls[in->dst] != OV_V)
        continue;
      if (in->dst == c->iv || ov_is_red_phi(c, in->dst, NULL))
        continue;
      int root = ov_root(c, in->dst);
      if (root == c->iv_next || ov_root(c, c->cond_value) == root)
        continue; /* scalar control, rebuilt from the base IV */
      if (ov_is_red_next(c, root, NULL))
        continue;
      switch (in->op) {
      case NYIR_PHI:
      case NYIR_COPY:
      case NYIR_ADD_I64:
      case NYIR_SUB_I64:
      case NYIR_AND_I64:
      case NYIR_OR_I64:
      case NYIR_XOR_I64:
        break;
      case NYIR_MUL_I64: {
        int64_t k = 0;
        bool a_const = ov_const(c, in->a, &k) && c->cls[in->b] == OV_V;
        bool b_const = !a_const && ov_const(c, in->b, &k) &&
                       c->cls[in->a] == OV_V;
        if ((!a_const && !b_const) || k < 0 || k > OV_MAX_MUL)
          OV_REJECT("ov_classify");
        break;
      }
      case NYIR_SHL_I64: {
        int64_t k = 0;
        if (!ov_const(c, in->b, &k) || k < 0 || k > OV_MAX_SHL)
          OV_REJECT("ov_classify");
        break;
      }
      case NYIR_SAR_I64: {
        int64_t k = 0;
        if (!ov_const(c, in->b, &k) || k < 0 || k > 63)
          OV_REJECT("ov_classify");
        break;
      }
      case NYIR_CMP_I64: {
        int bit = -1;
        bool eq = false;
        if (!ov_parity_mask(c, in, &bit, &eq))
          OV_REJECT("ov_classify");
        break;
      }
      case NYIR_SELECT_I64: {
        const nyr_inst_t *m = ov_def(c, ov_root(c, in->a));
        int bit = -1;
        bool eq = false;
        if (!m || !ov_parity_mask(c, m, &bit, &eq))
          OV_REJECT("ov_classify");
        break;
      }
      default:
        OV_REJECT("ov_classify");
      }
    }
  }
  /*
   * Uses: varying values stay inside M (header PHIs excepted, since the
   * scalar remainder loop recomputes them); a varying compare feeds only
   * SELECT conditions; control values feed only their scalar consumers.
   */
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    size_t b = cfg->inst_block[i];
    bool inside = b < cfg->block_count && c->member[b];
    int ops[6];
    int n = in->op == NYIR_PHI ? 0 : ov_operands(in, ops);
    for (size_t k = 0; in->op == NYIR_PHI && k < in->phi_incoming_len; ++k)
      if (n < 6)
        ops[n++] = in->phi_incoming[k].value;
    for (int k = 0; k < n; ++k) {
      int v = ops[k];
      if (v < 0 || v >= f->next_value || c->cls[v] != OV_V)
        continue;
      if (!ov_in_loop(c, v))
        continue;
      int root = ov_root(c, v);
      bool header_phi = root == c->iv || ov_is_red_phi(c, root, NULL);
      if (!inside && !header_phi)
        OV_REJECT("ov_classify");
      if (!inside)
        continue;
      const nyr_inst_t *def = ov_def(c, root);
      if (def && def->op == NYIR_CMP_I64 && root != ov_root(c, c->cond_value)) {
        bool mask_use = (in->op == NYIR_SELECT_I64 && k == 0) ||
                        in->op == NYIR_COPY;
        if (!mask_use)
          OV_REJECT("ov_classify");
      }
      if (root == ov_root(c, c->cond_value) &&
          !(in->op == NYIR_BR_IF || in->op == NYIR_COPY))
        OV_REJECT("ov_classify");
      if (root == c->iv_next &&
          !(in->op == NYIR_PHI || in->op == NYIR_COPY))
        OV_REJECT("ov_classify");
      /*
       * Reduction PHIs feed only their own next add (and copies).
       */
      size_t rk = 0;
      if (ov_is_red_phi(c, root, &rk) && in->op != NYIR_COPY &&
          !(in->dst >= 0 && ov_root(c, in->dst) == c->red_next[rk]))
        OV_REJECT("ov_classify");
    }
  }
  return true;
}

/*
 * emit
 */

static int ov_emit(nyr_func_t *f, nyr_op_t op, int a, int b, int c2,
                   int64_t imm, nyr_cmp_t cmp) {
  nyr_inst_t in = {.op = op, .dst = -1, .a = a, .b = b, .c = c2, .d = -1,
                   .e = -1, .f = -1, .imm = imm, .cmp = cmp};
  if (op == NYIR_ADD_I64 || op == NYIR_SUB_I64 || op == NYIR_CMP_I64 ||
      op == NYIR_AND_I64 || op == NYIR_CONST_I64)
    in.scalar_class = NYIR_SCALAR_I64_RAW;
  return nyr_ir_emit(f, in);
}

static bool ov_emit_ctl(nyr_func_t *f, nyr_op_t op, int a, int64_t label) {
  nyr_inst_t in = {.op = op, .dst = -1, .a = a, .b = -1, .c = -1, .d = -1,
                   .e = -1, .f = -1, .imm = label};
  size_t before = f->len;
  nyr_ir_emit(f, in);
  return f->len == before + 1;
}

static int ov_const_value(nyr_func_t *f, int64_t k) {
  return ov_emit(f, NYIR_CONST_I64, -1, -1, -1, k, NYIR_CMP_EQ);
}

static int ov_set1(nyr_func_t *f, int scalar) {
  return ov_emit(f, NYIR_VEC8_SET1_I64, scalar, -1, -1, 0, NYIR_CMP_EQ);
}

static int ov_vbin(nyr_func_t *f, nyr_op_t op, int a, int b) {
  return ov_emit(f, op, a, b, -1, 0, NYIR_CMP_EQ);
}

static nyr_op_t ov_vec_op(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
    return NYIR_VEC8_ADD_I64;
  case NYIR_SUB_I64:
    return NYIR_VEC8_SUB_I64;
  case NYIR_AND_I64:
    return NYIR_VEC8_AND_I64;
  case NYIR_OR_I64:
    return NYIR_VEC8_OR_I64;
  case NYIR_XOR_I64:
    return NYIR_VEC8_XOR_I64;
  default:
    return NYIR_NOP;
  }
}

/*
 * Scalar clone of a value: mapped if defined in M, else itself.
 */
static int ov_s(const ov_ctx_t *c, int v) {
  if (v < 0)
    return v;
  if (v < c->value_cap && c->smap[v] >= 0)
    return c->smap[v];
  return v;
}

/*
 * Vector form of operand v inside the clone.
 */
static int ov_vec(ov_ctx_t *c, int v) {
  nyr_func_t *f = c->f;
  if (v < 0)
    return -1;
  if (v < c->value_cap && c->cls[v] == OV_V)
    return c->half ? c->vmap2[v] : c->vmap[v];
  /*
   * Uniform: shadow, hoisted broadcast, or a broadcast at the use.
   */
  if (v < c->value_cap && c->uvmap && c->uvmap[v] >= 0)
    return c->uvmap[v];
  if (!ov_in_loop(c, v) && v < c->value_cap && c->bcast[v] >= 0)
    return c->bcast[v];
  return ov_set1(f, ov_s(c, v));
}

/*
 * x * k with packed adds (k >= 0, small).
 */
static int ov_vmul_const(ov_ctx_t *c, int x, int64_t k) {
  nyr_func_t *f = c->f;
  if (k == 0)
    return ov_set1(f, ov_const_value(f, 0));
  int acc = -1, pow = x;
  for (int64_t bit = k; bit; bit >>= 1) {
    if (bit & 1)
      acc = acc < 0 ? pow : ov_vbin(f, NYIR_VEC8_ADD_I64, acc, pow);
    if (bit >> 1)
      pow = ov_vbin(f, NYIR_VEC8_ADD_I64, pow, pow);
  }
  return acc;
}

static int64_t ov_clone_label(const ov_ctx_t *c, int64_t label) {
  size_t b = 0;
  if (!ov_block_by_label(c, label, &b) || !c->member[b])
    return label;
  return c->label_base + (int64_t)b;
}

typedef struct {
  size_t inst;  /* phi instruction index in f (clone) */
  int value;    /* original phi value */
  bool shadow;  /* vector shadow of a uniform phi */
  int half;     /* register half of an interleaved vector phi */
} ov_phi_fix_t;

static int *ov_vm(ov_ctx_t *c, int h) { return h ? c->vmap2 : c->vmap; }

/*
 * Uniform ops that have an exact packed form for a vector shadow.
 */
static bool ov_shadow_op(const ov_ctx_t *c, const nyr_inst_t *in) {
  int64_t k = 0;
  switch (in->op) {
  case NYIR_PHI:
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
    return true;
  case NYIR_MUL_I64:
    return (ov_const(c, in->b, &k) || ov_const(c, in->a, &k)) && k >= 0 &&
           k <= OV_MAX_MUL;
  case NYIR_SHL_I64:
    return ov_const(c, in->b, &k) && k >= 0 && k <= OV_MAX_SHL;
  case NYIR_SAR_I64:
    return ov_const(c, in->b, &k) && k >= 0 && k <= 63;
  default:
    return false;
  }
}

/*
 * Lane-uniform values that change inside M (an inner induction variable and
 * values derived from it) would otherwise be re-broadcast at every vector
 * use. Give them a packed shadow computed alongside the scalar, which keeps
 * driving control flow.
 */
static void ov_mark_shadows(ov_ctx_t *c) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  bool changed = true;
  for (int round = 0; changed && round < 256; ++round) {
    changed = false;
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!c->member[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->dst < 0 || in->dst >= c->value_cap)
          continue;
        bool vary = c->cls[in->dst] == OV_V;
        bool shadow = !vary && c->need_uv[in->dst];
        if (!vary && !shadow)
          continue;
        int ops[6];
        int n = in->op == NYIR_PHI ? 0 : ov_operands(in, ops);
        for (size_t k = 0; in->op == NYIR_PHI && k < in->phi_incoming_len; ++k)
          if (n < 6)
            ops[n++] = in->phi_incoming[k].value;
        for (int k = 0; k < n; ++k) {
          int v = ops[k];
          if (v < 0 || v >= c->value_cap || c->cls[v] != OV_U ||
              !ov_in_loop(c, v) || c->need_uv[v])
            continue;
          const nyr_inst_t *d = ov_def(c, v);
          if (!d || !ov_shadow_op(c, d))
            continue;
          c->need_uv[v] = true;
          changed = true;
        }
      }
    }
  }
}

static bool ov_transform(ov_ctx_t *c) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  size_t orig_len = f->len;
  int orig_values = f->next_value;
  c->value_cap = orig_values;
  c->smap = malloc((size_t)orig_values * sizeof(int));
  c->vmap = malloc((size_t)orig_values * sizeof(int));
  c->vmap2 = malloc((size_t)orig_values * sizeof(int));
  c->bcast = malloc((size_t)orig_values * sizeof(int));
  c->uvmap = malloc((size_t)orig_values * sizeof(int));
  c->need_uv = calloc((size_t)orig_values, sizeof(bool));
  ov_phi_fix_t *fixes = calloc(3 * orig_len + 1, sizeof(*fixes));
  if (!c->smap || !c->vmap || !c->vmap2 || !c->bcast || !c->uvmap ||
      !c->need_uv || !fixes) {
    free(fixes);
    return false;
  }
  for (int v = 0; v < orig_values; ++v)
    c->smap[v] = c->vmap[v] = c->vmap2[v] = c->bcast[v] = c->uvmap[v] = -1;
  c->half = 0;
  ov_mark_shadows(c);
  size_t nfix = 0;
  int64_t base = nyr_fresh_label(f);
  if (base < 0) {
    free(fixes);
    return false;
  }
  c->label_base = base;
  c->vx_label = base + (int64_t)cfg->block_count;
  c->vp_label = c->vx_label + 1;

  /*
   * Collect loop-invariant values that varying code uses, so their
   * broadcasts can be hoisted into the vector preheader. Their ids are
   * assigned now; the instructions are emitted in the preheader block at
   * the end (it is placed last in the array but first in the CFG).
   */
  bool *need_bcast = calloc((size_t)orig_values, sizeof(bool));
  if (!need_bcast) {
    free(fixes);
    return false;
  }
  for (size_t b = 0; b < cfg->block_count; ++b) {
    if (!c->member[b])
      continue;
    for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
      const nyr_inst_t *in = &f->data[i];
      bool vary = in->dst >= 0 && c->cls[in->dst] == OV_V;
      bool shadow = in->dst >= 0 && in->dst < orig_values &&
                    c->need_uv[in->dst];
      if (!vary && !shadow)
        continue;
      int ops[6];
      int n = in->op == NYIR_PHI ? 0 : ov_operands(in, ops);
      for (size_t k = 0; in->op == NYIR_PHI && k < in->phi_incoming_len; ++k)
        if (n < 6)
          ops[n++] = in->phi_incoming[k].value;
      for (int k = 0; k < n; ++k)
        if (ops[k] >= 0 && ops[k] < orig_values && c->cls[ops[k]] == OV_U &&
            !ov_in_loop(c, ops[k]))
          need_bcast[ops[k]] = true;
    }
  }
  for (int v = 0; v < orig_values; ++v)
    if (need_bcast[v])
      c->bcast[v] = f->next_value++;
  int lanes = f->next_value++;
  int vzero = f->next_value++;
  int lanes2 = -1;
  free(need_bcast);

  /*
   * Vector preheader: lane indices, zero, hoisted broadcasts.
   */
  bool vp_ok = ov_emit_ctl(f, NYIR_LABEL, -1, c->vp_label);
  if (vp_ok) {
    bool ok = true;
    nyr_inst_t alloca_in = {.op = NYIR_ALLOCA, .dst = -1, .a = -1, .b = -1,
                            .c = -1, .d = -1, .e = -1, .f = -1,
                            .imm = 8 * OV_LANES};
    int base_addr = nyr_ir_emit(f, alloca_in);
    for (int lane = 0; ok && lane < OV_LANES; ++lane) {
      int addr = lane == 0 ? base_addr
                           : ov_emit(f, NYIR_ADD_I64, base_addr,
                                     ov_const_value(f, 8 * lane), -1, 0,
                                     NYIR_CMP_EQ);
      nyr_inst_t st = {.op = NYIR_STORE_I64, .dst = -1, .a = addr, .b = -1,
                       .c = ov_const_value(f, lane), .d = -1, .e = -1,
                       .f = -1};
      size_t before = f->len;
      nyr_ir_emit(f, st);
      ok = f->len == before + 1;
    }
    nyr_inst_t ld = {.op = NYIR_VEC8_LOAD_I64, .dst = lanes, .a = base_addr,
                     .b = -1, .c = -1, .d = -1, .e = -1, .f = -1};
    nyr_ir_emit(f, ld);
    if (c->il == 2)
      lanes2 = ov_vbin(f, NYIR_VEC8_ADD_I64, lanes,
                       ov_set1(f, ov_const_value(f, OV_LANES)));
    nyr_inst_t z = {.op = NYIR_VEC8_SET1_I64, .dst = vzero,
                    .a = ov_const_value(f, 0), .b = -1, .c = -1, .d = -1,
                    .e = -1, .f = -1};
    nyr_ir_emit(f, z);
    for (int v = 0; v < orig_values; ++v) {
      if (c->bcast[v] < 0)
        continue;
      nyr_inst_t s1 = {.op = NYIR_VEC8_SET1_I64, .dst = c->bcast[v], .a = v,
                       .b = -1, .c = -1, .d = -1, .e = -1, .f = -1};
      nyr_ir_emit(f, s1);
    }
    vp_ok = ok && ov_emit_ctl(f, NYIR_BR, -1,
                              c->label_base + (int64_t)c->header);
  }
  if (!vp_ok || (c->il == 2 && lanes2 < 0)) {
    free(fixes);
    return false;
  }

  /*
   * Clone M's blocks in array order.
   */
  int ivs = -1, ivv = -1;
  size_t ivs_phi_at = 0, *red_phi_at = calloc(c->red_len + 1, sizeof(size_t));
  if (!red_phi_at) {
    free(fixes);
    return false;
  }
  bool ok = true;
  for (size_t b = 0; ok && b < cfg->block_count; ++b) {
    if (!c->member[b])
      continue;
    ok = ov_emit_ctl(f, NYIR_LABEL, -1, c->label_base + (int64_t)b);
    bool terminated = false;
    bool after_phis = false;
    for (size_t i = cfg->block_start[b]; ok && i < cfg->block_end[b]; ++i) {
      const nyr_inst_t in = f->data[i];
      if (in.op == NYIR_LABEL || in.op == NYIR_NOP)
        continue;
      if (b == c->header && !after_phis && in.op != NYIR_PHI) {
        after_phis = true;
        /*
         * ivv = {ivs, ivs+1, ivs+2, ivs+3}
         */
        int ivb = ov_set1(f, ivs);
        ivv = ov_vbin(f, NYIR_VEC8_ADD_I64, ivb, lanes);
        c->vmap[c->iv] = ivv;
        if (c->il == 2)
          c->vmap2[c->iv] = ov_vbin(f, NYIR_VEC8_ADD_I64, ivb, lanes2);
        c->smap[c->iv] = ivs;
      }
      if (in.op == NYIR_PHI) {
        nyr_phi_incoming_t *inc =
            malloc(in.phi_incoming_len * sizeof(*inc));
        if (!inc) {
          ok = false;
          break;
        }
        for (size_t k = 0; k < in.phi_incoming_len; ++k) {
          inc[k].predecessor_label =
              b == c->header &&
                      in.phi_incoming[k].predecessor_label ==
                          cfg->block_label[c->preheader]
                  ? c->vp_label
                  : ov_clone_label(c, in.phi_incoming[k].predecessor_label);
          inc[k].value = -1; /* patched after all values exist */
        }
        nyr_inst_t phi = {.op = NYIR_PHI, .dst = -1, .a = -1, .b = -1,
                          .c = -1, .d = -1, .e = -1, .f = -1,
                          .phi_incoming = inc,
                          .phi_incoming_len = in.phi_incoming_len,
                          .scalar_class = in.scalar_class};
        size_t at = f->len;
        int dst = nyr_ir_emit(f, phi);
        if (dst < 0) {
          ok = false;
          break;
        }
        fixes[nfix].inst = at;
        fixes[nfix].value = in.dst;
        ++nfix;
        size_t rk = 0;
        bool red = false;
        if (in.dst == c->iv) {
          ivs = dst;
          ivs_phi_at = at;
        } else if ((red = ov_is_red_phi(c, in.dst, &rk)) ||
                   c->cls[in.dst] == OV_V) {
          if (red)
            red_phi_at[rk] = at;
          c->vmap[in.dst] = dst;
          if (c->il == 2) {
            nyr_phi_incoming_t *inc2 =
                malloc(in.phi_incoming_len * sizeof(*inc2));
            if (!inc2) {
              ok = false;
              break;
            }
            memcpy(inc2, inc, in.phi_incoming_len * sizeof(*inc2));
            nyr_inst_t phi2 = phi;
            phi2.phi_incoming = inc2;
            size_t at2 = f->len;
            int dst2 = nyr_ir_emit(f, phi2);
            if (dst2 < 0) {
              ok = false;
              break;
            }
            c->vmap2[in.dst] = dst2;
            fixes[nfix].inst = at2;
            fixes[nfix].value = in.dst;
            fixes[nfix].half = 1;
            ++nfix;
          }
        } else {
          c->smap[in.dst] = dst;
          if (c->need_uv[in.dst]) {
            nyr_phi_incoming_t *vinc =
                malloc(in.phi_incoming_len * sizeof(*vinc));
            if (!vinc) {
              ok = false;
              break;
            }
            memcpy(vinc, inc, in.phi_incoming_len * sizeof(*vinc));
            nyr_inst_t vphi = phi;
            vphi.phi_incoming = vinc;
            vphi.scalar_class = NYIR_SCALAR_UNKNOWN;
            size_t vat = f->len;
            int vdst = nyr_ir_emit(f, vphi);
            if (vdst < 0) {
              ok = false;
              break;
            }
            c->uvmap[in.dst] = vdst;
            fixes[nfix].inst = vat;
            fixes[nfix].value = in.dst;
            fixes[nfix].shadow = true;
            ++nfix;
          }
        }
        continue;
      }
      if (in.op == NYIR_BR || in.op == NYIR_BR_IF) {
        int64_t target = in.imm;
        size_t tb = 0;
        bool to_exit = ov_block_by_label(c, target, &tb) && !c->member[tb];
        int64_t nt = to_exit ? c->vx_label : ov_clone_label(c, target);
        int cond = in.op == NYIR_BR_IF ? ov_s(c, in.a) : -1;
        ok = ov_emit_ctl(f, in.op, cond, nt);
        if (in.op == NYIR_BR)
          terminated = true;
        continue;
      }
      int root = in.dst >= 0 ? ov_root(c, in.dst) : -1;
      /*
       * Loop control rebuilt on the scalar base IV.
       */
      if (in.dst >= 0 && root == ov_root(c, c->cond_value) &&
          in.op == NYIR_CMP_I64) {
        int bound = ov_s(c, c->bound);
        int c1 = ov_emit(f, NYIR_CMP_I64, ivs, bound, -1, 0, NYIR_CMP_LT);
        int rem = ov_emit(f, NYIR_SUB_I64, bound, ivs, -1, 0, NYIR_CMP_EQ);
        int three = ov_const_value(f, OV_LANES * c->il - 1);
        int c2 = ov_emit(f, NYIR_CMP_I64, rem, three, -1, 0, NYIR_CMP_GT);
        c->smap[in.dst] = ov_emit(f, NYIR_AND_I64, c1, c2, -1, 0,
                                  NYIR_CMP_EQ);
        continue;
      }
      if (in.dst >= 0 && root == c->iv_next && in.op == NYIR_ADD_I64) {
        int four = ov_const_value(f, OV_LANES * c->il);
        c->smap[in.dst] =
            ov_emit(f, NYIR_ADD_I64, ivs, four, -1, 0, NYIR_CMP_EQ);
        continue;
      }
      size_t rk = 0;
      if (in.dst >= 0 && ov_is_red_next(c, root, &rk) &&
          in.op == NYIR_ADD_I64) {
        int red = c->red_phi[rk];
        int other = ov_root(c, in.a) == red ? in.b : in.a;
        for (c->half = 0; c->half < c->il; ++c->half)
          ov_vm(c, c->half)[in.dst] =
              ov_vbin(f, NYIR_VEC8_ADD_I64, ov_vm(c, c->half)[red],
                      ov_vec(c, other));
        c->half = 0;
        continue;
      }
      if (in.op == NYIR_COPY) {
        if (in.a >= 0 && in.a < orig_values && c->cls[in.a] == OV_V) {
          c->vmap[in.dst] = c->vmap[in.a];
          c->vmap2[in.dst] = c->vmap2[in.a];
          if (c->smap[in.a] >= 0)
            c->smap[in.dst] = c->smap[in.a];
        } else {
          c->smap[in.dst] =
              ov_emit(f, NYIR_COPY, ov_s(c, in.a), -1, -1, 0, NYIR_CMP_EQ);
          if (c->need_uv[in.dst] && in.a >= 0)
            c->uvmap[in.dst] = ov_vec(c, in.a);
        }
        continue;
      }
      if (in.dst < 0 || c->cls[in.dst] == OV_U) {
        nyr_inst_t copy = in;
        copy.dst = -1;
        copy.a = ov_s(c, in.a);
        copy.b = ov_s(c, in.b);
        copy.c = ov_s(c, in.c);
        copy.d = ov_s(c, in.d);
        copy.e = ov_s(c, in.e);
        copy.f = ov_s(c, in.f);
        copy.symbol = NULL;
        nyr_clear_derived_facts(&copy);
        int d = nyr_ir_emit(f, copy);
        if (in.dst >= 0)
          c->smap[in.dst] = d;
        if (in.dst >= 0 && c->need_uv[in.dst]) {
          int sh = -1;
          int64_t k = 0;
          switch (in.op) {
          case NYIR_CONST_I64:
            sh = ov_set1(f, d);
            break;
          case NYIR_ADD_I64:
          case NYIR_SUB_I64:
          case NYIR_AND_I64:
          case NYIR_OR_I64:
          case NYIR_XOR_I64:
            sh = ov_vbin(f, ov_vec_op(in.op), ov_vec(c, in.a),
                         ov_vec(c, in.b));
            break;
          case NYIR_MUL_I64: {
            int x = ov_const(c, in.b, &k) ? in.a : in.b;
            if (x == in.b)
              (void)ov_const(c, in.a, &k);
            sh = ov_vmul_const(c, ov_vec(c, x), k);
            break;
          }
          case NYIR_SHL_I64:
            (void)ov_const(c, in.b, &k);
            sh = ov_vec(c, in.a);
            for (int64_t t = 0; t < k; ++t)
              sh = ov_vbin(f, NYIR_VEC8_ADD_I64, sh, sh);
            break;
          case NYIR_SAR_I64:
            (void)ov_const(c, in.b, &k);
            sh = ov_emit(f, NYIR_VEC8_SAR_I64, ov_vec(c, in.a), -1, -1, k,
                         NYIR_CMP_EQ);
            break;
          default:
            break;
          }
          if (sh >= 0)
            c->uvmap[in.dst] = sh;
        }
        continue;
      }
      /*
       * Varying arithmetic.
       *
       * (x >> 63) & 1 (the sign bit of a signed division bias) is one
       * logical shift: AVX2 has VPSRLQ but no VPSRAQ.
       */
      int sign_src = -1;
      if (in.op == NYIR_AND_I64) {
        int64_t k = 0, sh = 0;
        int sv = ov_const(c, in.b, &k) && k == 1   ? in.a
                 : ov_const(c, in.a, &k) && k == 1 ? in.b
                                                   : -1;
        const nyr_inst_t *sd =
            sv >= 0 && sv < orig_values && c->cls[sv] == OV_V ? ov_def(c, sv)
                                                              : NULL;
        if (sd && sd->op == NYIR_SAR_I64 && ov_const(c, sd->b, &sh) &&
            sh == 63)
          sign_src = sd->a;
      }
      for (c->half = 0; ok && c->half < c->il; ++c->half) {
      int out = -1;
      if (sign_src >= 0) {
        out = ov_emit(f, NYIR_VEC8_SHR_I64, ov_vec(c, sign_src), -1, -1, 63,
                      NYIR_CMP_EQ);
        if (out < 0)
          ok = false;
        else
          ov_vm(c, c->half)[in.dst] = out;
        continue;
      }
      switch (in.op) {
      case NYIR_ADD_I64:
      case NYIR_SUB_I64:
      case NYIR_AND_I64:
      case NYIR_OR_I64:
      case NYIR_XOR_I64:
        out = ov_vbin(f, ov_vec_op(in.op), ov_vec(c, in.a), ov_vec(c, in.b));
        break;
      case NYIR_MUL_I64: {
        int64_t k = 0;
        int x = ov_const(c, in.a, &k) && c->cls[in.b] == OV_V ? in.b : in.a;
        if (x == in.a)
          (void)ov_const(c, in.b, &k);
        out = ov_vmul_const(c, ov_vec(c, x), k);
        break;
      }
      case NYIR_SHL_I64: {
        int64_t k = 0;
        (void)ov_const(c, in.b, &k);
        out = ov_vec(c, in.a);
        for (int64_t s = 0; s < k; ++s)
          out = ov_vbin(f, NYIR_VEC8_ADD_I64, out, out);
        break;
      }
      case NYIR_SAR_I64: {
        int64_t k = 0;
        (void)ov_const(c, in.b, &k);
        out = ov_emit(f, NYIR_VEC8_SAR_I64, ov_vec(c, in.a), -1, -1, k,
                      NYIR_CMP_EQ);
        break;
      }
      case NYIR_CMP_I64: {
        int bit = -1;
        bool eq = false;
        (void)ov_parity_mask(c, &in, &bit, &eq);
        int t = ov_vec(c, bit);
        out = eq ? ov_vbin(f, NYIR_VEC8_SUB_I64, t,
                           ov_set1(f, ov_const_value(f, 1)))
                 : ov_vbin(f, NYIR_VEC8_SUB_I64,
                           ov_set1(f, ov_const_value(f, 0)), t);
        break;
      }
      case NYIR_SELECT_I64: {
        int mask = ov_vec(c, in.a);
        int yes = ov_vec(c, in.b), no = ov_vec(c, in.c);
        int diff = ov_vbin(f, NYIR_VEC8_XOR_I64, yes, no);
        out = ov_vbin(f, NYIR_VEC8_XOR_I64, no,
                      ov_vbin(f, NYIR_VEC8_AND_I64, diff, mask));
        break;
      }
      default:
        ok = false;
        break;
      }
      if (out < 0)
        ok = false;
      else
        ov_vm(c, c->half)[in.dst] = out;
      }
      c->half = 0;
    }
    /*
     * Make fallthrough explicit: the clone order is not the original.
     */
    if (ok && !terminated) {
      size_t last = 0;
      const nyr_inst_t *t = ov_terminator(c, b, &last);
      bool ends_br = t && t->op == NYIR_BR;
      if (!ends_br) {
        size_t next = b + 1;
        if (b == c->cond_block && c->exit_fallthrough) {
          ok = ov_emit_ctl(f, NYIR_BR, -1, c->vx_label);
          continue;
        }
        if (next >= cfg->block_count || !c->member[next]) {
          ok = false;
          break;
        }
        ok = ov_emit_ctl(f, NYIR_BR, -1, c->label_base + (int64_t)next);
      }
    }
  }
  /*
   * Patch cloned PHI incomings.
   */
  for (size_t k = 0; ok && k < nfix; ++k) {
    nyr_inst_t *phi = &f->data[fixes[k].inst];
    const nyr_inst_t *orig = &f->data[c->defs[fixes[k].value]];
    bool vary = c->cls[fixes[k].value] == OV_V;
    for (size_t e = 0; e < phi->phi_incoming_len; ++e) {
      int v = orig->phi_incoming[e].value;
      int nv = -1;
      if (fixes[k].shadow) {
        if (v >= 0 && v < orig_values && c->uvmap[v] >= 0)
          nv = c->uvmap[v];
        else if (v >= 0 && v < orig_values && !ov_in_loop(c, v) &&
                 c->bcast[v] >= 0)
          nv = c->bcast[v];
      } else if (fixes[k].inst == ivs_phi_at) {
        nv = phi->phi_incoming[e].predecessor_label == c->vp_label
                 ? c->iv_init
                 : ov_s(c, v);
      } else if (vary) {
        size_t rk = 0;
        if (ov_is_red_phi(c, fixes[k].value, &rk) &&
            phi->phi_incoming[e].predecessor_label == c->vp_label)
          nv = vzero;
        else if (v >= 0 && v < orig_values && c->cls[v] == OV_V)
          nv = ov_vm(c, fixes[k].half)[v];
        else if (v >= 0 && !ov_in_loop(c, v) && v < orig_values &&
                 c->bcast[v] >= 0)
          nv = c->bcast[v];
        else if (v >= 0 && v < orig_values && c->uvmap[v] >= 0)
          nv = c->uvmap[v];
        else
          nv = -1; /* uniform in-loop value into a vector PHI: unsupported */
      } else {
        nv = ov_s(c, v);
      }
      if (nv < 0) {
        ok = false;
        break;
      }
      phi->phi_incoming[e].value = nv;
    }
  }
  free(red_phi_at);
  free(fixes);
  if (!ok)
    return false;
  /*
   * Vector exit: horizontal reductions, then the scalar remainder loop.
   */
  int *finals = calloc(c->red_len + 1, sizeof(int));
  if (!finals)
    return false;
  ok = ov_emit_ctl(f, NYIR_LABEL, -1, c->vx_label);
  for (size_t k = 0; ok && k < c->red_len; ++k) {
    finals[k] = ov_emit(f, NYIR_VEC8_REDUCE_ADD_I64, c->red_init[k],
                        c->vmap[c->red_phi[k]], -1, 0, NYIR_CMP_EQ);
    if (c->il == 2)
      finals[k] = ov_emit(f, NYIR_VEC8_REDUCE_ADD_I64, finals[k],
                          c->vmap2[c->red_phi[k]], -1, 0, NYIR_CMP_EQ);
  }
  ok = ok && ov_emit_ctl(f, NYIR_BR, -1, cfg->block_label[c->header]);
  /*
   * Scalar loop now starts where the vector loop stopped.
   */
  for (size_t i = cfg->block_start[c->header];
       ok && i < cfg->block_end[c->header]; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      if (in->phi_incoming[k].predecessor_label !=
          cfg->block_label[c->preheader])
        continue;
      in->phi_incoming[k].predecessor_label = c->vx_label;
      size_t rk = 0;
      if (in->dst == c->iv)
        in->phi_incoming[k].value = ivs;
      else if (ov_is_red_phi(c, in->dst, &rk))
        in->phi_incoming[k].value = finals[rk];
      else
        ok = false;
    }
  }
  free(finals);
  if (!ok)
    return false;
  /*
   * Retarget the preheader edge into the vector preheader.
   */
  size_t term_at = 0;
  const nyr_inst_t *pt = ov_terminator(c, c->preheader, &term_at);
  int64_t hl = cfg->block_label[c->header];
  bool retargeted = false;
  for (size_t i = cfg->block_start[c->preheader];
       i < cfg->block_end[c->preheader]; ++i) {
    nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) && in->imm == hl) {
      in->imm = c->vp_label;
      retargeted = true;
    }
  }
  if (!pt || pt->op != NYIR_BR) {
    /*
     * Fallthrough into the header: insert an explicit branch.
     */
    size_t at = cfg->block_end[c->preheader];
    if (!nir_ensure_inst_space(f, 1))
      return false;
    memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->len += 1;
    f->data[at] = (nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                               .c = -1, .d = -1, .e = -1, .f = -1,
                               .imm = c->vp_label};
    f->data[at].effects = nyr_inst_effects(&f->data[at]);
    retargeted = true;
  }
  (void)term_at;
  return retargeted;
}

static int ov_try(nyr_func_t *f) {
  ov_ctx_t c = {0};
  c.f = f;
  if (f->next_value <= 0 || !nyr_cfg_build(f, &c.cfg))
    return 0;
  c.defs = nyr_build_defs(f);
  c.member = calloc(c.cfg.block_count + 1, sizeof(bool));
  c.cls = malloc((size_t)f->next_value + 1);
  c.red_phi = malloc(8 * sizeof(int));
  c.red_init = malloc(8 * sizeof(int));
  c.red_next = malloc(8 * sizeof(int));
  c.red_vec = malloc(8 * sizeof(int));
  if (!c.defs || !c.member || !c.cls || !c.red_phi || !c.red_init ||
      !c.red_next || !c.red_vec) {
    ov_free(&c);
    return -1;
  }
  int result = 0;
  for (size_t latch = 0; latch < c.cfg.block_count && result == 0; ++latch) {
    for (size_t e = c.cfg.succ_offsets[latch];
         e < c.cfg.succ_offsets[latch + 1] && result == 0; ++e) {
      if (!c.cfg.backedge_edges[e])
        continue;
      if (!ov_analyse(&c, latch, c.cfg.succ_blocks[e]) || !ov_classify(&c))
        continue;
      nyr_func_t work = {0};
      if (!nyr_ir_func_clone(f, &work)) {
        result = -1;
        break;
      }
      nyr_func_t *saved = c.f;
      c.f = &work;
      /*
       * NYTRIX_OUTER_VEC_INTERLEAVE=2: two independent four-lane registers
       * per value hide the latency of loop-carried lane recurrences.
       */
      const char *il = getenv("NYTRIX_OUTER_VEC_INTERLEAVE");
      c.il = il && strcmp(il, "2") == 0 ? 2 : 1;
      bool done = ov_transform(&c);
      c.f = saved;
      char err[256] = {0};
      if (done && nyr_ir_verify(&work, err, sizeof(err))) {
        if (getenv("NYTRIX_TRACE_OUTER_VEC_DUMP"))
          nyr_dump(stderr, &work, "outer-vectorize-accepted");
        nyr_func_replace(f, &work);
        result = 1;
      } else {
        if (getenv("NYTRIX_TRACE_OUTER_VEC"))
        {
          fprintf(stderr, "outer-vectorize: rejected (%s)\n",
                  done ? err : "transform");
          if (getenv("NYTRIX_TRACE_OUTER_VEC_DUMP"))
            nyr_dump(stderr, &work, "outer-vectorize-rejected");
        }
        nyr_ir_func_free(&work);
      }
      free(c.smap);
      free(c.vmap);
      free(c.vmap2);
      free(c.bcast);
      free(c.uvmap);
      free(c.need_uv);
      c.smap = c.vmap = c.vmap2 = c.bcast = c.uvmap = NULL;
      c.need_uv = NULL;
    }
  }
  ov_free(&c);
  return result;
}

bool nyr_outer_loop_vectorize(nyr_func_t *f) {
  /*
   * On by default. The machine-form route keeps the widened loop's
   * recurrences and invariants in vector registers; the in-process NYIR route
   * lowers four-lane PHIs through 32-byte homes (nyr_phi_elim).
   * NYTRIX_OUTER_VECTORIZE=0 (or off/false) disables the transform.
   */
  const char *on = getenv("NYTRIX_OUTER_VECTORIZE");
  if (on && (strcmp(on, "0") == 0 || strcmp(on, "off") == 0 ||
             strcmp(on, "false") == 0))
    return true;
  if (!f || f->len == 0 || !nyr_has_loop(f))
    return true;
  if (getenv("NYTRIX_TRACE_OUTER_VEC_DUMP"))
    nyr_dump(stderr, f, "outer-vectorize-input");
  for (int n = 0; n < 4; ++n) {
    int r = ov_try(f);
    if (r < 0)
      return false;
    if (r == 0)
      break;
    if (getenv("NYTRIX_TRACE_OUTER_VEC"))
      fprintf(stderr, "outer-vectorize: widened a loop 4x (len=%zu)\n", f->len);
  }
  return true;
}
