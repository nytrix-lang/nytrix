/*
 * Versioned register promotion of a loop-invariant memory cell.
 *
 *   for (i = init; i < limit; i += step)      // step > 0
 *     *s = *s + f(q[i], ...);                  // other accesses: loads only
 *
 * keeps `*s` in memory because `q + i` may point into it, so every iteration
 * pays a store-to-load forwarding round trip (page-rank: ~8 cycles against
 * clang's 3). The pass emits a second copy of the loop that carries `*s` in
 * a PHI, loads it once before and stores it once after, and enters that copy
 * only when the loop runs and no other load can overlap the cell:
 *
 *   init < limit  &&  for every other load q_k + i:
 *     s + 8 <= q_k + init  ||  s >= q_k + limit + 8
 *
 * Otherwise the original loop runs unchanged. Shape, all conservative:
 *   - a two-block natural loop: header (PHIs, pure compare, br.if -> body)
 *     whose fall-through is a lone `br` to an exit without PHIs, and a
 *     straight-line body ending in `br header`; a dedicated preheader;
 *   - no calls or other side effects; every store is a full-width
 *     STORE_I64 to the same invariant address s, every access to s is
 *     full width, and every other access is a LOAD_I64 at `q + iv` with q
 *     invariant and iv the counted induction variable;
 *   - no value defined in the loop is used outside it.
 * Each rewrite runs on a clone and is kept only if it verifies.
 * NYTRIX_NO_STORE_PROMOTE=1 disables it.
 */
#include "code/backend/passes/util.h"
#include "code/backend/passes/verify/loop.h"
#include <stdlib.h>
#include <string.h>

enum { SP_MAX_LOADS = 8, SP_MAX_DONE = 64 };

static const unsigned sp_barrier =
    NYIR_EFFECT_CALL | NYIR_EFFECT_VOLATILE | NYIR_EFFECT_THREAD |
    NYIR_EFFECT_FFI | NYIR_EFFECT_IO | NYIR_EFFECT_ALLOCATION |
    NYIR_EFFECT_UNKNOWN_SIDE_EFFECT;

static const unsigned sp_narrow =
    NYIR_INST_F_MEM_BYTE | NYIR_INST_F_MEM_I32 | NYIR_INST_F_MEM_U16;

static nyr_inst_t sp_make(nyr_op_t op, int dst, int a, int b, int64_t imm) {
  nyr_inst_t in = {0};
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.c = in.d = in.e = in.f = -1;
  in.imm = imm;
  in.cmp = NYIR_CMP_EQ;
  in.effects = nyr_inst_effects(&in);
  return in;
}

static nyr_inst_t sp_cmp(int dst, nyr_cmp_t cmp, int a, int b) {
  nyr_inst_t in = sp_make(NYIR_CMP_I64, dst, a, b, 0);
  in.cmp = cmp;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  in.effects = nyr_inst_effects(&in);
  return in;
}

static bool sp_copy(const nyr_inst_t *src, nyr_inst_t *dst) {
  *dst = *src;
  dst->extra_args = NULL;
  dst->arg_sizes = NULL;
  dst->phi_incoming = NULL;
  if (src->extra_args_len || src->arg_sizes)
    return false;
  if (src->phi_incoming_len) {
    dst->phi_incoming =
        malloc(src->phi_incoming_len * sizeof(*dst->phi_incoming));
    if (!dst->phi_incoming)
      return false;
    memcpy(dst->phi_incoming, src->phi_incoming,
           src->phi_incoming_len * sizeof(*dst->phi_incoming));
  }
  return true;
}

typedef struct {
  nyr_inst_t *data;
  size_t len, cap;
} sp_buf_t;

static bool sp_push(sp_buf_t *b, nyr_inst_t in) {
  if (b->len == b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 32;
    nyr_inst_t *nd = realloc(b->data, nc * sizeof(*nd));
    if (!nd) {
      nyr_inst_discard(&in);
      return false;
    }
    b->data = nd, b->cap = nc;
  }
  b->data[b->len++] = in;
  return true;
}

static void sp_buf_free(sp_buf_t *b) {
  for (size_t i = 0; i < b->len; ++i)
    nyr_inst_discard(&b->data[i]);
  free(b->data);
  b->data = NULL;
  b->len = b->cap = 0;
}

static bool sp_invariant(const nyr_cfg_t *cfg, const int *defs, size_t h,
                         size_t body, int v) {
  if (v < 0)
    return false;
  if (defs[v] < 0)
    return true;
  size_t blk = cfg->inst_block[(size_t)defs[v]];
  return blk != h && blk != body;
}

/* Look through COPY retags: two copies of one pointer name one cell. */
static int sp_root(const nyr_func_t *f, const int *defs, int v) {
  for (int depth = 0; depth < 8 && v >= 0 && v < f->next_value; ++depth) {
    int at = defs[v];
    if (at < 0 || f->data[at].op != NYIR_COPY || f->data[at].a < 0)
      break;
    v = f->data[at].a;
  }
  return v;
}

/* Splice `n` instructions at `at`. */
static bool sp_splice(nyr_func_t *f, size_t at, const nyr_inst_t *ins,
                      size_t n) {
  if (!n)
    return true;
  if (!nir_ensure_inst_space(f, n))
    return false;
  memmove(&f->data[at + n], &f->data[at], (f->len - at) * sizeof(*f->data));
  memcpy(&f->data[at], ins, n * sizeof(*ins));
  f->len += n;
  return true;
}

/*
 * Try one loop. Returns 1 when rewritten, 0 when not a candidate, -1 on
 * allocation failure. `f` is a scratch clone.
 */
static int sp_try(nyr_func_t *f, const nyr_scev_loop_t *L) {
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg))
    return 0;
  int *defs = nyr_build_defs(f);
  int result = 0;
  sp_buf_t tail = {0}, pre = {0};
  int *map = NULL;
  if (!defs)
    goto out;
  size_t h = L->header_block, body = L->latch_block, p = L->preheader_block;
  if (h >= cfg.block_count || body >= cfg.block_count ||
      p >= cfg.block_count || h == body || p == h || p == body)
    goto out;
  /* Header: exactly the preheader and the body as predecessors. */
  if (cfg.pred_offsets[h + 1] - cfg.pred_offsets[h] != 2)
    goto out;
  for (size_t k = cfg.pred_offsets[h]; k < cfg.pred_offsets[h + 1]; ++k)
    if (cfg.pred_blocks[k] != p && cfg.pred_blocks[k] != body)
      goto out;
  /* Preheader: single successor, labelled. */
  if (cfg.succ_offsets[p + 1] - cfg.succ_offsets[p] != 1 ||
      cfg.block_label[p] < 0 || cfg.block_label[h] < 0 ||
      cfg.block_label[body] < 0)
    goto out;
  /* Body: single predecessor (the header), back edge to the header. */
  if (cfg.pred_offsets[body + 1] - cfg.pred_offsets[body] != 1)
    goto out;
  size_t body_end = nyr_block_last_non_nop(f, &cfg, body);
  if (body_end <= cfg.block_start[body] ||
      f->data[body_end - 1].op != NYIR_BR ||
      f->data[body_end - 1].imm != cfg.block_label[h])
    goto out;
  /* Header ends in br.if -> body; its fall-through is a lone br to exit. */
  size_t h_end = nyr_block_last_non_nop(f, &cfg, h);
  if (h_end <= cfg.block_start[h] || f->data[h_end - 1].op != NYIR_BR_IF ||
      f->data[h_end - 1].imm != cfg.block_label[body] || h + 1 >= cfg.block_count)
    goto out;
  size_t x = h + 1;
  size_t x_end = nyr_block_last_non_nop(f, &cfg, x);
  if (cfg.block_label[x] >= 0 || x_end != cfg.block_start[x] + 1 ||
      f->data[cfg.block_start[x]].op != NYIR_BR)
    goto out;
  int64_t exit_label = f->data[cfg.block_start[x]].imm;
  for (size_t b = 0; b < cfg.block_count; ++b) {
    if (cfg.block_label[b] != exit_label)
      continue;
    for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i)
      if (f->data[i].op == NYIR_PHI)
        goto out;
  }
  if (L->step <= 0 || L->predicate != NYIR_CMP_LT || L->iv < 0)
    goto out;
  int init_v = L->init_value, limit_v = L->limit_value;
  if (init_v < 0 || limit_v < 0)
    goto out;
  /* Header contents: label, PHIs, pure scalar ops. */
  for (size_t i = cfg.block_start[h]; i + 1 < h_end; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_LABEL || in->op == NYIR_PHI || in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64 ||
        (nyr_effective_effects(in) & ~NYIR_EFFECT_MAY_TRAP))
      goto out;
  }
  /* Body: find the cell and the other loads. */
  int cell = -1;
  int load_q[SP_MAX_LOADS];
  size_t nloads = 0;
  const nyr_inst_t *cell_load = NULL;
  for (size_t i = cfg.block_start[body]; i + 1 < body_end; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (nyr_effective_effects(in) & sp_barrier)
      goto out;
    if (in->op == NYIR_STORE_I64) {
      if ((in->flags & sp_narrow) || in->a < 0 ||
          !sp_invariant(&cfg, defs, h, body, in->a) ||
          (cell >= 0 && sp_root(f, defs, cell) != sp_root(f, defs, in->a)))
        goto out;
      if (cell < 0)
        cell = in->a;
    } else if (in->op != NYIR_LOAD_I64 && in->op != NYIR_LABEL &&
               in->op != NYIR_NOP &&
               (nyr_effective_effects(in) &
                (NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY))) {
      goto out; /* vector or other memory operations */
    }
  }
  if (cell < 0)
    goto out;
  for (size_t i = cfg.block_start[body]; i + 1 < body_end; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_LOAD_I64)
      continue;
    if (in->a >= 0 && sp_root(f, defs, in->a) == sp_root(f, defs, cell)) {
      if ((in->flags & sp_narrow) || !sp_invariant(&cfg, defs, h, body, in->a))
        goto out;
      cell_load = in;
      continue;
    }
    if (in->a < 0 || defs[in->a] < 0 || nloads == SP_MAX_LOADS)
      goto out;
    const nyr_inst_t *ad = &f->data[defs[in->a]];
    int q = -1;
    if (ad->op == NYIR_ADD_I64 && ad->a == L->iv)
      q = ad->b;
    else if (ad->op == NYIR_ADD_I64 && ad->b == L->iv)
      q = ad->a;
    if (q < 0 || !sp_invariant(&cfg, defs, h, body, q))
      goto out;
    size_t k = 0;
    while (k < nloads && load_q[k] != q)
      ++k;
    if (k == nloads)
      load_q[nloads++] = q;
  }
  if (!cell_load)
    goto out;
  /* No loop value escapes. */
  for (size_t i = 0; i < f->len; ++i) {
    size_t blk = cfg.inst_block[i];
    if (blk == h || blk == body)
      continue;
    const nyr_inst_t *in = &f->data[i];
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int s = 0; s < 6; ++s)
      if (ops[s] >= 0 && defs[ops[s]] >= 0 &&
          (cfg.inst_block[defs[ops[s]]] == h ||
           cfg.inst_block[defs[ops[s]]] == body))
        goto out;
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v >= 0 && defs[v] >= 0 &&
          (cfg.inst_block[defs[v]] == h || cfg.inst_block[defs[v]] == body))
        goto out;
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v >= 0 && defs[v] >= 0 &&
          (cfg.inst_block[defs[v]] == h || cfg.inst_block[defs[v]] == body))
        goto out;
    }
  }

  /* ---- Rewrite ---- */
  int64_t base_label = nyr_fresh_label(f);
  int64_t lab_pp = base_label, lab_hp = base_label + 1,
          lab_bp = base_label + 2, lab_pn = base_label + 3;
  int64_t lab_p = cfg.block_label[p], lab_h = cfg.block_label[h],
          lab_b = cfg.block_label[body];
  int nv0 = f->next_value;
  map = malloc((size_t)nv0 * sizeof(int));
  if (!map) {
    result = -1;
    goto out;
  }
  for (int v = 0; v < nv0; ++v)
    map[v] = v;
  for (size_t i = cfg.block_start[h]; i < cfg.block_end[body]; ++i) {
    size_t blk = cfg.inst_block[i];
    if ((blk == h || blk == body) && f->data[i].dst >= 0)
      map[f->data[i].dst] = f->next_value++;
  }
#define SP_MAP(v) ((v) >= 0 && (v) < nv0 ? map[(v)] : (v))
  /* Preheader dispatch: checks, br.if -> promoted copy, new label Pn. */
  {
    int eight = f->next_value++;
    if (!sp_push(&pre, sp_make(NYIR_CONST_I64, eight, -1, -1, 8)))
      goto oom;
    int ok_v = f->next_value++;
    if (!sp_push(&pre, sp_cmp(ok_v, NYIR_CMP_LT, init_v, limit_v)))
      goto oom;
    int s8 = f->next_value++;
    if (!sp_push(&pre, sp_make(NYIR_ADD_I64, s8, cell, eight, 0)))
      goto oom;
    for (size_t k = 0; k < nloads; ++k) {
      int lo = f->next_value++, hi0 = f->next_value++, hi = f->next_value++;
      int below = f->next_value++, above = f->next_value++,
          disjoint = f->next_value++, acc = f->next_value++;
      if (!sp_push(&pre, sp_make(NYIR_ADD_I64, lo, load_q[k], init_v, 0)) ||
          !sp_push(&pre, sp_make(NYIR_ADD_I64, hi0, load_q[k], limit_v, 0)) ||
          !sp_push(&pre, sp_make(NYIR_ADD_I64, hi, hi0, eight, 0)) ||
          !sp_push(&pre, sp_cmp(below, NYIR_CMP_LE, s8, lo)) ||
          !sp_push(&pre, sp_cmp(above, NYIR_CMP_GE, cell, hi)) ||
          !sp_push(&pre, sp_make(NYIR_OR_I64, disjoint, below, above, 0)) ||
          !sp_push(&pre, sp_make(NYIR_AND_I64, acc, ok_v, disjoint, 0)))
        goto oom;
      ok_v = acc;
    }
    if (!sp_push(&pre, sp_make(NYIR_BR_IF, -1, ok_v, -1, lab_pp)) ||
        !sp_push(&pre, sp_make(NYIR_LABEL, -1, -1, -1, lab_pn)))
      goto oom;
  }
  /* Promoted copy, appended after the function body. */
  int acc_phi = f->next_value++;
  int acc0 = f->next_value++;
  {
    if (!sp_push(&tail, sp_make(NYIR_LABEL, -1, -1, -1, lab_pp)))
      goto oom;
    nyr_inst_t ld;
    if (!sp_copy(cell_load, &ld))
      goto oom;
    ld.dst = acc0;
    ld.a = cell;
    if (!sp_push(&tail, ld) ||
        !sp_push(&tail, sp_make(NYIR_BR, -1, -1, -1, lab_hp)))
      goto oom;
  }
  size_t acc_phi_at = 0;
  for (size_t i = cfg.block_start[h]; i < h_end; ++i) {
    const nyr_inst_t *in = &f->data[i];
    nyr_inst_t c;
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_LABEL) {
      if (!sp_push(&tail, sp_make(NYIR_LABEL, -1, -1, -1, lab_hp)))
        goto oom;
      /* The cell's PHI goes first among the PHIs. */
      nyr_inst_t phi = sp_make(NYIR_PHI, acc_phi, -1, -1, 0);
      phi.scalar_class = cell_load->scalar_class;
      phi.phi_incoming = malloc(2 * sizeof(*phi.phi_incoming));
      if (!phi.phi_incoming)
        goto oom;
      phi.phi_incoming_len = 2;
      phi.phi_incoming[0] =
          (nyr_phi_incoming_t){.predecessor_label = lab_pp, .value = acc0};
      phi.phi_incoming[1] =
          (nyr_phi_incoming_t){.predecessor_label = lab_bp, .value = -1};
      acc_phi_at = tail.len;
      if (!sp_push(&tail, phi))
        goto oom;
      continue;
    }
    if (!sp_copy(in, &c))
      goto oom;
    c.dst = SP_MAP(c.dst);
    c.a = SP_MAP(c.a), c.b = SP_MAP(c.b), c.c = SP_MAP(c.c);
    c.d = SP_MAP(c.d), c.e = SP_MAP(c.e), c.f = SP_MAP(c.f);
    for (size_t k = 0; k < c.phi_incoming_len; ++k) {
      if (c.phi_incoming[k].predecessor_label == lab_p)
        c.phi_incoming[k].predecessor_label = lab_pp;
      else if (c.phi_incoming[k].predecessor_label == lab_b) {
        c.phi_incoming[k].predecessor_label = lab_bp;
        c.phi_incoming[k].value = SP_MAP(c.phi_incoming[k].value);
      }
    }
    if (c.op == NYIR_BR_IF)
      c.imm = lab_bp;
    if (!sp_push(&tail, c))
      goto oom;
  }
  /* Exit path of the copy: write the cell back, then leave. */
  {
    nyr_inst_t st = sp_make(NYIR_STORE_I64, -1, cell, -1, 0);
    const nyr_inst_t *store_tmpl = NULL;
    for (size_t i = cfg.block_start[body]; i + 1 < body_end; ++i)
      if (f->data[i].op == NYIR_STORE_I64)
        store_tmpl = &f->data[i];
    if (!store_tmpl || !sp_copy(store_tmpl, &st))
      goto oom;
    st.a = cell;
    st.c = acc_phi;
    if (!sp_push(&tail, st) ||
        !sp_push(&tail, sp_make(NYIR_BR, -1, -1, -1, exit_label)))
      goto oom;
  }
  /* Body copy: loads of the cell read the running value, stores update it. */
  int cur = acc_phi;
  for (size_t i = cfg.block_start[body]; i < body_end; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_LABEL) {
      if (!sp_push(&tail, sp_make(NYIR_LABEL, -1, -1, -1, lab_bp)))
        goto oom;
      continue;
    }
    if (in->op == NYIR_STORE_I64 && in->a >= 0 &&
        sp_root(f, defs, in->a) == sp_root(f, defs, cell)) {
      cur = SP_MAP(in->c);
      continue;
    }
    nyr_inst_t c;
    if (in->op == NYIR_LOAD_I64 && in->a >= 0 &&
        sp_root(f, defs, in->a) == sp_root(f, defs, cell)) {
      c = sp_make(NYIR_COPY, SP_MAP(in->dst), cur, -1, 0);
      c.scalar_class = in->scalar_class;
      c.debug = in->debug;
      c.effects = nyr_inst_effects(&c);
    } else {
      if (!sp_copy(in, &c))
        goto oom;
      c.dst = SP_MAP(c.dst);
      c.a = SP_MAP(c.a), c.b = SP_MAP(c.b), c.c = SP_MAP(c.c);
      c.d = SP_MAP(c.d), c.e = SP_MAP(c.e), c.f = SP_MAP(c.f);
      if (c.op == NYIR_BR)
        c.imm = lab_hp;
    }
    if (!sp_push(&tail, c))
      goto oom;
  }
  tail.data[acc_phi_at].phi_incoming[1].value = cur;
#undef SP_MAP
  /* Header PHIs now see Pn instead of P. */
  for (size_t i = cfg.block_start[h]; i < h_end; ++i)
    for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k)
      if (f->data[i].phi_incoming[k].predecessor_label == lab_p)
        f->data[i].phi_incoming[k].predecessor_label = lab_pn;
  /* Insert: tail at the end, dispatch at the preheader's tail. */
  size_t p_end = nyr_block_last_non_nop(f, &cfg, p);
  size_t p_at = p_end;
  if (p_end > cfg.block_start[p] && f->data[p_end - 1].op == NYIR_BR)
    p_at = p_end - 1;
  if (!sp_splice(f, f->len, tail.data, tail.len))
    goto oom;
  free(tail.data);
  tail = (sp_buf_t){0};
  if (!sp_splice(f, p_at, pre.data, pre.len))
    goto oom;
  free(pre.data);
  pre = (sp_buf_t){0};
  (void)lab_h;
  result = 1;
  goto out;
oom:
  result = -1;
out:
  sp_buf_free(&tail);
  sp_buf_free(&pre);
  free(map);
  free(defs);
  nyr_cfg_free(&cfg);
  return result;
}

bool nyr_loop_store_promote(nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_STORE_PROMOTE");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || f->len < 8 || f->next_value <= 0 || !nyr_has_loop(f))
    return true;
  int64_t done[SP_MAX_DONE];
  size_t ndone = 0;
  for (int round = 0; round < 8; ++round) {
    nyr_scev_info_t info = {0};
    if (!nyr_scev_analyze(f, &info))
      return true;
    bool changed = false;
    for (size_t i = 0; i < info.count && !changed; ++i) {
      const nyr_scev_loop_t *L = &info.loops[i];
      bool seen = false;
      for (size_t k = 0; k < ndone; ++k)
        seen |= done[k] == L->header_label;
      if (seen || ndone == SP_MAX_DONE)
        continue;
      done[ndone++] = L->header_label;
      nyr_func_t cand = {0};
      if (!nyr_ir_func_clone(f, &cand))
        break;
      int r = sp_try(&cand, L);
      char err[256] = {0};
      if (r == 1 && nyr_ir_verify(&cand, err, sizeof(err))) {
        nyr_func_replace(f, &cand);
        changed = true;
      } else {
        nyr_ir_func_free(&cand);
      }
    }
    nyr_scev_free(&info);
    if (!changed)
      break;
  }
  return true;
}
