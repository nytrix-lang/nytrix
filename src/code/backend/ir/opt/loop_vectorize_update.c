/*
 * Element-wise f64 update vectorization with a runtime dependence guard.
 *
 * Candidate: an innermost two-block loop
 *
 *   header:  i = phi(pre: lo, latch: i + 1); if (i < hi) goto latch else exit
 *   latch:   x_k = load.f64 (q_k + i * 8)        (any number of loads)
 *            t   = fadd/fsub/fmul of x_k, loop-invariant f64 values, t
 *            store.f64 (p + i * 8), t            (exactly one store)
 *            goto header
 *
 * where p, q_k, lo, and hi are loop-invariant. This is the row update of
 * dense elimination (A[r][c] -= m * A[k][c]) and similar AXPY-style loops.
 * The general loop vectorizer rejects it: typed-buffer accesses there need a
 * constant trip count, and a dynamic store pins every other access on its
 * root. Both properties are unnecessary here:
 *
 *  - Memory safety. The vector body runs i = lo, lo + 4, ... while i + 4 <= hi,
 *    so every lane address is one the scalar loop itself accesses; the
 *    scalar loop then finishes the remainder.
 *  - Dependence. Within one four-lane chunk the vector code performs all
 *    loads before the store. That matches the scalar order unless the store
 *    of one lane feeds a load of a later lane in the same chunk, or of an
 *    earlier lane through a partial overlap. Both require 0 < |p - q| < 32
 *    bytes; |p - q| == 0 is the same element (load before store in both
 *    orders). A runtime guard checks every load base against p and falls back
 *    to the unchanged scalar loop otherwise.
 *
 * Lanes perform exactly the scalar operations in the same order, so results
 * are bit-identical. The transform is built on a clone and installed only
 * after the verifier accepts it.
 */
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FU_MAX_LOADS = 8, FU_MAX_OPS = 32 };

typedef struct {
  nyr_func_t *f;
  nyr_cfg_t cfg;
  int *defs;
  size_t header, latch, preheader;
  int iv, lo, hi, iv_next;
  int store_base, store_value;
  size_t store_at;
  int load_dst[FU_MAX_LOADS], load_base[FU_MAX_LOADS];
  size_t load_count;
  size_t op_at[FU_MAX_OPS];
  size_t op_count;
  int inv[FU_MAX_OPS * 2];
  size_t inv_count;
} fu_ctx_t;

static bool fu_trace(void) { return getenv("NYTRIX_TRACE_F64_UPDATE_VEC"); }

#define FU_REJECT(why)                                                         \
  do {                                                                         \
    if (fu_trace())                                                            \
      fprintf(stderr, "f64-update-vectorize: reject header=b%zu: %s\n",        \
              c->header, why);                                                 \
    return false;                                                              \
  } while (0)

static const nyr_inst_t *fu_def(const fu_ctx_t *c, int v) {
  if (v < 0 || v >= c->f->next_value || c->defs[v] < 0)
    return NULL;
  return &c->f->data[(size_t)c->defs[v]];
}

static bool fu_in_loop(const fu_ctx_t *c, int v) {
  if (v < 0 || v >= c->f->next_value || c->defs[v] < 0)
    return false; /* parameters and entry values are invariant */
  size_t b = c->cfg.inst_block[(size_t)c->defs[v]];
  return b == c->header || b == c->latch;
}

/* Follow transparent register copies. */
static int fu_resolve(const fu_ctx_t *c, int v) {
  for (int depth = 0; depth < 16; ++depth) {
    const nyr_inst_t *d = fu_def(c, v);
    if (!d || d->op != NYIR_COPY || d->a < 0)
      return v;
    v = d->a;
  }
  return v;
}

static bool fu_const(const fu_ctx_t *c, int v, int64_t *out) {
  const nyr_inst_t *d = fu_def(c, fu_resolve(c, v));
  if (!d || d->op != NYIR_CONST_I64)
    return false;
  *out = d->imm;
  return true;
}

/* v == i * 8, as shl(i, 3) or mul(i, 8). */
static bool fu_is_iv_bytes(const fu_ctx_t *c, int v) {
  const nyr_inst_t *d = fu_def(c, fu_resolve(c, v));
  int64_t k = 0;
  if (!d)
    return false;
  if (d->op == NYIR_SHL_I64)
    return fu_resolve(c, d->a) == c->iv && fu_const(c, d->b, &k) && k == 3;
  if (d->op == NYIR_MUL_I64)
    return (fu_resolve(c, d->a) == c->iv && fu_const(c, d->b, &k) && k == 8) ||
           (fu_resolve(c, d->b) == c->iv && fu_const(c, d->a, &k) && k == 8);
  return false;
}

/* v == base + i * 8 with an invariant base; returns the base. */
static int fu_address_base(const fu_ctx_t *c, int v) {
  const nyr_inst_t *d = fu_def(c, fu_resolve(c, v));
  if (!d || d->op != NYIR_ADD_I64)
    return -1;
  if (fu_is_iv_bytes(c, d->b) && !fu_in_loop(c, d->a))
    return d->a;
  if (fu_is_iv_bytes(c, d->a) && !fu_in_loop(c, d->b))
    return d->b;
  return -1;
}

static bool fu_plain_f64_memory(const nyr_inst_t *in) {
  return in->scalar_class == NYIR_SCALAR_F64 &&
         !(in->flags & (NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_U16 |
                        NYIR_INST_F_MEM_I32 | NYIR_INST_F_MEM_VOLATILE));
}

static int fu_load_index(const fu_ctx_t *c, int v) {
  for (size_t k = 0; k < c->load_count; ++k)
    if (c->load_dst[k] == v)
      return (int)k;
  return -1;
}

static bool fu_is_op_value(const fu_ctx_t *c, int v) {
  for (size_t k = 0; k < c->op_count; ++k)
    if (c->f->data[c->op_at[k]].dst == v)
      return true;
  return false;
}

/* An f64 operand the vector body can provide. */
static bool fu_operand_ok(fu_ctx_t *c, int v) {
  v = fu_resolve(c, v);
  if (fu_load_index(c, v) >= 0 || fu_is_op_value(c, v))
    return true;
  if (fu_in_loop(c, v))
    return false;
  const nyr_inst_t *d = fu_def(c, v);
  /* Invariant f64 values defined by an f64-class instruction or constant. */
  if (d && d->scalar_class != NYIR_SCALAR_F64 && d->op != NYIR_CONST_F64)
    return false;
  if (!d)
    return false;
  for (size_t k = 0; k < c->inv_count; ++k)
    if (c->inv[k] == v)
      return true;
  if (c->inv_count >= sizeof(c->inv) / sizeof(c->inv[0]))
    return false;
  c->inv[c->inv_count++] = v;
  return true;
}

static bool fu_analyse(fu_ctx_t *c, size_t latch, size_t header) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  c->header = header;
  c->latch = latch;
  c->load_count = c->op_count = c->inv_count = 0;
  c->store_base = c->store_value = -1;
  c->store_at = SIZE_MAX;
  if (latch == header)
    FU_REJECT("single-block loop");
  /* Exactly two blocks: header -> latch -> header, header exits. */
  if (cfg->succ_offsets[latch + 1] - cfg->succ_offsets[latch] != 1 ||
      cfg->succ_blocks[cfg->succ_offsets[latch]] != header)
    FU_REJECT("latch is not a single backedge block");
  if (cfg->pred_offsets[latch + 1] - cfg->pred_offsets[latch] != 1 ||
      cfg->pred_blocks[cfg->pred_offsets[latch]] != header)
    FU_REJECT("latch has another predecessor");
  size_t preheader = SIZE_MAX;
  for (size_t e = cfg->pred_offsets[header]; e < cfg->pred_offsets[header + 1];
       ++e) {
    size_t p = cfg->pred_blocks[e];
    if (p == latch)
      continue;
    if (preheader != SIZE_MAX)
      FU_REJECT("several outside predecessors");
    preheader = p;
  }
  if (preheader == SIZE_MAX || cfg->block_label[preheader] < 0 ||
      cfg->block_label[header] < 0 || cfg->block_label[latch] < 0)
    FU_REJECT("unlabeled preheader/header/latch");
  c->preheader = preheader;

  /* Header: one +1 induction PHI, its compare, the conditional branch. */
  c->iv = -1;
  int cond = -1;
  bool branch_to_latch = false;
  for (size_t i = cfg->block_start[header]; i < cfg->block_end[header]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    switch (in->op) {
    case NYIR_LABEL:
    case NYIR_NOP:
      break;
    case NYIR_PHI: {
      if (c->iv >= 0 || in->phi_incoming_len != 2)
        FU_REJECT("header PHI is not a lone induction");
      c->iv = in->dst;
      for (size_t k = 0; k < 2; ++k) {
        if (in->phi_incoming[k].predecessor_label == cfg->block_label[latch])
          c->iv_next = in->phi_incoming[k].value;
        else if (in->phi_incoming[k].predecessor_label ==
                 cfg->block_label[preheader])
          c->lo = in->phi_incoming[k].value;
        else
          FU_REJECT("induction PHI edge mismatch");
      }
      break;
    }
    case NYIR_COPY:
      if (fu_resolve(c, in->a) != c->iv)
        FU_REJECT("header copy of a non-induction value");
      break;
    case NYIR_CMP_I64:
      if (cond >= 0 || in->cmp != NYIR_CMP_LT ||
          fu_resolve(c, in->a) != c->iv || fu_in_loop(c, in->b))
        FU_REJECT("header compare is not i < invariant");
      cond = in->dst;
      c->hi = in->b;
      break;
    case NYIR_BR_IF:
      if (cond < 0 || fu_resolve(c, in->a) != cond ||
          in->imm != cfg->block_label[latch])
        FU_REJECT("header branch does not enter the latch");
      branch_to_latch = true;
      break;
    case NYIR_BR:
      if (!branch_to_latch)
        FU_REJECT("header branch shape");
      break;
    default:
      FU_REJECT("header has other work");
    }
  }
  if (c->iv < 0 || !branch_to_latch || c->lo < 0 || c->hi < 0)
    FU_REJECT("no canonical header");
  const nyr_inst_t *next = fu_def(c, c->iv_next);
  int64_t step = 0;
  if (!next || next->op != NYIR_ADD_I64 ||
      c->cfg.inst_block[(size_t)c->defs[c->iv_next]] != latch ||
      !((fu_resolve(c, next->a) == c->iv && fu_const(c, next->b, &step)) ||
        (fu_resolve(c, next->b) == c->iv && fu_const(c, next->a, &step))) ||
      step != 1)
    FU_REJECT("induction step is not +1");

  /* Latch body. */
  for (size_t i = cfg->block_start[latch]; i < cfg->block_end[latch]; ++i) {
    const nyr_inst_t *in = &f->data[i];
    switch (in->op) {
    case NYIR_LABEL:
    case NYIR_NOP:
    case NYIR_CONST_I64:
      break;
    case NYIR_COPY:
    case NYIR_SHL_I64:
    case NYIR_MUL_I64:
      /* Induction copies and byte offsets; checked through their uses. */
      if (in->op == NYIR_COPY && fu_resolve(c, in->a) != c->iv)
        FU_REJECT("latch copy of a non-induction value");
      if (in->op != NYIR_COPY && !fu_is_iv_bytes(c, in->dst))
        FU_REJECT("latch arithmetic is not an induction byte offset");
      break;
    case NYIR_ADD_I64:
      if (in->dst == c->iv_next)
        break;
      if (fu_address_base(c, in->dst) < 0)
        FU_REJECT("latch add is not base + i * 8");
      break;
    case NYIR_LOAD_I64:
      if (!fu_plain_f64_memory(in) || c->load_count >= FU_MAX_LOADS)
        FU_REJECT("load is not a plain f64 load");
      c->load_base[c->load_count] = fu_address_base(c, in->a);
      if (c->load_base[c->load_count] < 0)
        FU_REJECT("load address is not invariant base + i * 8");
      c->load_dst[c->load_count++] = in->dst;
      break;
    case NYIR_ADD_F64:
    case NYIR_SUB_F64:
    case NYIR_MUL_F64:
      if (c->op_count >= FU_MAX_OPS || !fu_operand_ok(c, in->a) ||
          !fu_operand_ok(c, in->b))
        FU_REJECT("f64 op operand is not vectorizable");
      c->op_at[c->op_count++] = i;
      break;
    case NYIR_STORE_I64:
      if (c->store_at != SIZE_MAX || !fu_plain_f64_memory(in))
        FU_REJECT("store is not the lone plain f64 store");
      c->store_base = fu_address_base(c, in->a);
      if (c->store_base < 0 || !fu_operand_ok(c, in->c))
        FU_REJECT("store address/value shape");
      c->store_value = fu_resolve(c, in->c);
      c->store_at = i;
      break;
    case NYIR_BR:
      if (in->imm != cfg->block_label[header])
        FU_REJECT("latch branch target");
      break;
    default:
      FU_REJECT("latch has unsupported work");
    }
  }
  if (c->store_at == SIZE_MAX || c->load_count == 0)
    FU_REJECT("no load/store stream");
  /* In-loop values must not escape: only header values can, by dominance. */
  for (size_t i = 0; i < f->len; ++i) {
    size_t b = cfg->inst_block[i];
    if (b == c->latch)
      continue;
    const nyr_inst_t *in = &f->data[i];
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < 6; ++k)
      if (ops[k] >= 0 && ops[k] < f->next_value && c->defs[ops[k]] >= 0 &&
          cfg->inst_block[(size_t)c->defs[ops[k]]] == c->latch &&
          ops[k] != c->iv_next)
        FU_REJECT("latch value escapes");
  }
  return true;
}

static int fu_emit(nyr_func_t *f, nyr_op_t op, int a, int b, int c2,
                   int64_t imm, nyr_cmp_t cmp, nyr_scalar_class_t cls) {
  nyr_inst_t in = {.op = op, .dst = -1, .a = a, .b = b, .c = c2, .d = -1,
                   .e = -1, .f = -1, .imm = imm, .cmp = cmp,
                   .scalar_class = cls};
  return nyr_ir_emit(f, in);
}

static int fu_i64(nyr_func_t *f, nyr_op_t op, int a, int b, nyr_cmp_t cmp) {
  return fu_emit(f, op, a, b, -1, 0, cmp, NYIR_SCALAR_I64_RAW);
}

static int fu_const_value(nyr_func_t *f, int64_t k) {
  return fu_emit(f, NYIR_CONST_I64, -1, -1, -1, k, NYIR_CMP_EQ,
                 NYIR_SCALAR_I64_RAW);
}

static bool fu_ctl(nyr_func_t *f, nyr_op_t op, int a, int64_t label) {
  nyr_inst_t in = {.op = op, .dst = -1, .a = a, .b = -1, .c = -1, .d = -1,
                   .e = -1, .f = -1, .imm = label};
  size_t before = f->len;
  nyr_ir_emit(f, in);
  return f->len == before + 1;
}

static int fu_phi(nyr_func_t *f, int64_t l0, int v0, int64_t l1, int v1,
                  size_t *at) {
  nyr_phi_incoming_t *inc = malloc(2 * sizeof(*inc));
  if (!inc)
    return -1;
  inc[0] = (nyr_phi_incoming_t){.predecessor_label = l0, .value = v0};
  inc[1] = (nyr_phi_incoming_t){.predecessor_label = l1, .value = v1};
  nyr_inst_t phi = {.op = NYIR_PHI, .dst = -1, .a = -1, .b = -1, .c = -1,
                    .d = -1, .e = -1, .f = -1, .phi_incoming = inc,
                    .phi_incoming_len = 2,
                    .scalar_class = NYIR_SCALAR_I64_RAW};
  if (at)
    *at = f->len;
  return nyr_ir_emit(f, phi);
}

static nyr_op_t fu_vec_op(nyr_op_t op) {
  return op == NYIR_ADD_F64   ? NYIR_VEC4_ADD_F64
         : op == NYIR_SUB_F64 ? NYIR_VEC4_SUB_F64
                              : NYIR_VEC4_MUL_F64;
}

static bool fu_transform(fu_ctx_t *c) {
  nyr_func_t *f = c->f;
  const nyr_cfg_t *cfg = &c->cfg;
  int64_t base = nyr_fresh_label(f);
  if (base < 0)
    return false;
  /*
   * Instructions after a BR_IF form their own (unlabeled) CFG block, so the
   * two edges into the resume block leave through labeled trampolines:
   * vps (guard failed) and vhx (vector loop done).
   */
  const int64_t vp = base, vh = base + 1, vb = base + 2, vx = base + 3,
                vps = base + 4, vhx = base + 5;
  const int64_t pre_label = cfg->block_label[c->preheader];
  const int64_t header_label = cfg->block_label[c->header];
  int orig_values = f->next_value;
  int *vmap = malloc((size_t)orig_values * sizeof(*vmap));
  if (!vmap)
    return false;
  for (int v = 0; v < orig_values; ++v)
    vmap[v] = -1;
  bool ok = true;

  /* Vector preheader: trip, end, dependence guard, broadcasts. */
  ok = fu_ctl(f, NYIR_LABEL, -1, vp);
  int n = fu_i64(f, NYIR_SUB_I64, c->hi, c->lo, NYIR_CMP_EQ);
  int guard = fu_i64(f, NYIR_CMP_I64, n, fu_const_value(f, 3), NYIR_CMP_GT);
  int store_base = fu_resolve(c, c->store_base);
  for (size_t k = 0; ok && k < c->load_count; ++k) {
    int q = fu_resolve(c, c->load_base[k]);
    if (q == store_base)
      continue;
    int d = fu_i64(f, NYIR_SUB_I64, store_base, q, NYIR_CMP_EQ);
    int same = fu_i64(f, NYIR_CMP_I64, d, fu_const_value(f, 0), NYIR_CMP_EQ);
    int far_up =
        fu_i64(f, NYIR_CMP_I64, d, fu_const_value(f, 31), NYIR_CMP_GT);
    int far_down =
        fu_i64(f, NYIR_CMP_I64, d, fu_const_value(f, -31), NYIR_CMP_LT);
    int any = fu_i64(f, NYIR_OR_I64,
                     fu_i64(f, NYIR_OR_I64, same, far_up, NYIR_CMP_EQ),
                     far_down, NYIR_CMP_EQ);
    guard = fu_i64(f, NYIR_AND_I64, guard, any, NYIR_CMP_EQ);
  }
  int vend = fu_i64(f, NYIR_ADD_I64, c->lo,
                    fu_i64(f, NYIR_AND_I64, n, fu_const_value(f, -4),
                           NYIR_CMP_EQ),
                    NYIR_CMP_EQ);
  for (size_t k = 0; ok && k < c->inv_count; ++k) {
    int v = c->inv[k];
    if (v >= orig_values)
      continue;
    vmap[v] = fu_emit(f, NYIR_VEC4_SET1_F64, v, -1, -1, 0, NYIR_CMP_EQ,
                      NYIR_SCALAR_UNKNOWN);
    ok = vmap[v] >= 0;
  }
  ok = ok && fu_ctl(f, NYIR_BR_IF, guard, vh) && fu_ctl(f, NYIR_BR, -1, vps);
  ok = ok && fu_ctl(f, NYIR_LABEL, -1, vps) && fu_ctl(f, NYIR_BR, -1, vx);

  /* Vector header. */
  ok = ok && fu_ctl(f, NYIR_LABEL, -1, vh);
  size_t viv_at = SIZE_MAX;
  int viv = ok ? fu_phi(f, vp, c->lo, vb, -1, &viv_at) : -1;
  int vcond = fu_i64(f, NYIR_CMP_I64, viv, vend, NYIR_CMP_LT);
  ok = ok && viv >= 0 && fu_ctl(f, NYIR_BR_IF, vcond, vb) &&
       fu_ctl(f, NYIR_BR, -1, vhx);
  ok = ok && fu_ctl(f, NYIR_LABEL, -1, vhx) && fu_ctl(f, NYIR_BR, -1, vx);

  /* Vector body: loads, packed ops, the store. */
  ok = ok && fu_ctl(f, NYIR_LABEL, -1, vb);
  int bytes = fu_i64(f, NYIR_SHL_I64, viv, fu_const_value(f, 3), NYIR_CMP_EQ);
  for (size_t k = 0; ok && k < c->load_count; ++k) {
    int addr = fu_i64(f, NYIR_ADD_I64, c->load_base[k], bytes, NYIR_CMP_EQ);
    vmap[c->load_dst[k]] = fu_emit(f, NYIR_VEC4_LOAD_F64, addr, -1, -1, 0,
                                   NYIR_CMP_EQ, NYIR_SCALAR_F64);
    ok = addr >= 0 && vmap[c->load_dst[k]] >= 0;
  }
  for (size_t k = 0; ok && k < c->op_count; ++k) {
    nyr_inst_t in = f->data[c->op_at[k]];
    int a = fu_resolve(c, in.a), b = fu_resolve(c, in.b);
    if (a >= orig_values || b >= orig_values || vmap[a] < 0 || vmap[b] < 0) {
      ok = false;
      break;
    }
    vmap[in.dst] = fu_emit(f, fu_vec_op(in.op), vmap[a], vmap[b], -1, 0,
                           NYIR_CMP_EQ, NYIR_SCALAR_F64);
    ok = vmap[in.dst] >= 0;
  }
  if (ok) {
    int addr = fu_i64(f, NYIR_ADD_I64, c->store_base, bytes, NYIR_CMP_EQ);
    int value = c->store_value < orig_values ? vmap[c->store_value] : -1;
    nyr_inst_t st = {.op = NYIR_VEC4_STORE_F64, .dst = -1, .a = addr,
                     .b = value, .c = -1, .d = -1, .e = -1, .f = -1,
                     .scalar_class = NYIR_SCALAR_F64};
    size_t before = f->len;
    ok = addr >= 0 && value >= 0;
    if (ok) {
      nyr_ir_emit(f, st);
      ok = f->len == before + 1;
      if (ok)
        f->data[f->len - 1].dst = -1; /* stores define no value */
    }
  }
  int viv_next = fu_i64(f, NYIR_ADD_I64, viv, fu_const_value(f, 4),
                        NYIR_CMP_EQ);
  ok = ok && viv_next >= 0 && fu_ctl(f, NYIR_BR, -1, vh);
  if (ok)
    f->data[viv_at].phi_incoming[1].value = viv_next;

  /* Vector exit: resume the scalar loop where the vector loop stopped. */
  ok = ok && fu_ctl(f, NYIR_LABEL, -1, vx);
  int resume = ok ? fu_phi(f, vps, c->lo, vhx, viv, NULL) : -1;
  ok = ok && resume >= 0 && fu_ctl(f, NYIR_BR, -1, header_label);
  free(vmap);
  if (!ok)
    return false;

  /* The scalar header now enters from the vector exit. */
  for (size_t i = cfg->block_start[c->header]; i < cfg->block_end[c->header];
       ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI)
      continue;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].predecessor_label == pre_label) {
        in->phi_incoming[k].predecessor_label = vx;
        in->phi_incoming[k].value = resume;
      }
  }
  /* Retarget the preheader edge into the vector preheader. */
  bool retargeted = false, explicit_branch = false;
  for (size_t i = cfg->block_start[c->preheader];
       i < cfg->block_end[c->preheader]; ++i) {
    nyr_inst_t *in = &f->data[i];
    if ((in->op == NYIR_BR || in->op == NYIR_BR_IF) &&
        in->imm == header_label) {
      in->imm = vp;
      retargeted = true;
    }
    if (in->op == NYIR_BR)
      explicit_branch = true;
  }
  if (!explicit_branch) {
    /* Fallthrough into the header: add an explicit branch. */
    size_t at = cfg->block_end[c->preheader];
    if (!nir_ensure_inst_space(f, 1))
      return false;
    memmove(&f->data[at + 1], &f->data[at], (f->len - at) * sizeof(*f->data));
    f->len += 1;
    f->data[at] = (nyr_inst_t){.op = NYIR_BR, .dst = -1, .a = -1, .b = -1,
                               .c = -1, .d = -1, .e = -1, .f = -1, .imm = vp};
    f->data[at].effects = nyr_inst_effects(&f->data[at]);
    retargeted = true;
  }
  return retargeted;
}

/* 1: transformed one loop, 0: none, -1: error. */
static int fu_try(nyr_func_t *f) {
  fu_ctx_t c = {.f = f};
  if (f->next_value <= 0 || !nyr_cfg_build(f, &c.cfg))
    return 0;
  c.defs = nyr_build_defs(f);
  if (!c.defs) {
    nyr_cfg_free(&c.cfg);
    return -1;
  }
  int result = 0;
  for (size_t latch = 0; latch < c.cfg.block_count && result == 0; ++latch) {
    for (size_t e = c.cfg.succ_offsets[latch];
         e < c.cfg.succ_offsets[latch + 1] && result == 0; ++e) {
      if (!c.cfg.backedge_edges[e] ||
          !fu_analyse(&c, latch, c.cfg.succ_blocks[e]))
        continue;
      nyr_func_t work = {0};
      if (!nyr_ir_func_clone(f, &work)) {
        result = -1;
        break;
      }
      c.f = &work;
      bool done = fu_transform(&c);
      c.f = f;
      char err[256] = {0};
      if (done && nyr_ir_verify(&work, err, sizeof(err))) {
        nyr_func_replace(f, &work);
        result = 1;
      } else {
        if (fu_trace()) {
          fprintf(stderr, "f64-update-vectorize: rejected (%s)\n",
                  done ? err : "transform");
          if (getenv("NYTRIX_TRACE_F64_UPDATE_VEC_DUMP"))
            nyr_dump(stderr, &work, "f64-update-rejected");
        }
        nyr_ir_func_free(&work);
      }
    }
  }
  free(c.defs);
  nyr_cfg_free(&c.cfg);
  return result;
}

bool nyr_loop_vectorize_f64_update(nyr_func_t *f) {
  const char *on = getenv("NYTRIX_F64_UPDATE_VECTORIZE");
  if (on && (strcmp(on, "0") == 0 || strcmp(on, "off") == 0))
    return true;
  if (!f || f->len == 0 || !nyr_has_loop(f))
    return true;
  for (int n = 0; n < 8; ++n) {
    int r = fu_try(f);
    if (r < 0)
      return false;
    if (r == 0)
      break;
    if (fu_trace())
      fprintf(stderr, "f64-update-vectorize: widened a loop (len=%zu)\n",
              f->len);
  }
  return true;
}
