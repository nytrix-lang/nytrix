/*
 * Bounded software pipelining with a scalar prologue for canonical
 * two-block counted loops.
 *
 * The latch body is split into two stages by data dependence:
 *   - stage A: pure computations that depend only on the induction `iv`
 *     and loop-invariant values (never on carried state);
 *   - stage B: everything else (carried updates, the step, values that
 *     touch header PHIs other than `iv`).
 * If stage A is non-empty, self-contained, and never used outside the latch,
 * the loop is software-pipelined by prefetching stage A one iteration ahead:
 *
 *   Prologue:  A(0)                  (scalar prologue, iv := init)
 *   Header:    original PHIs plus a rotating PHI per stage-A value
 *   Latch:     B uses the rotating value; then A_next := A(iv+1) prefetch
 *
 * Correctness: each kernel trip runs B at the current `iv` using the
 * prefetched A value, then computes stage A for `iv+1`; stage A is pure and
 * independent of carried state, so overlapping it a cycle early is exact.
 * The epilogue is degenerate (the final prefetch is pure and dead).
 *
 * Constraints: canonical two-block natural loop, pure-scalar payload-free
 * latch body, LT guard on the SCEV induction, linear layout
 * preheader < header < latch, non-empty stage A, stage-A values unused
 * outside the latch.
 *
 * Transactional: rebuilt on a fresh buffer, committed only after
 * `nyir_verify` accepts; otherwise the function is byte-for-byte unchanged.
 */
#include "code/ir/opt/loop.h"

#include <limits.h>
#include <stdlib.h>

#define SP_MAX_BODY 96

static size_t sp_last_non_nop(const nyir_func_t *f, const nyir_cfg_t *cfg,
                              size_t block) {
  size_t end = cfg->block_end[block];
  while (end > cfg->block_start[block] && f->data[end - 1].op == NYIR_NOP)
    --end;
  return end;
}

static bool sp_payload_free(const nyir_inst_t *in) {
  return in && !in->symbol && !in->debug.file && !in->extra_args &&
         !in->arg_sizes && !in->phi_incoming;
}

static bool sp_body_pure(const nyir_inst_t *in) {
  if (!in || !sp_payload_free(in))
    return false;
  if (nyir_inst_effects(in) != NYIR_EFFECT_NONE)
    return false;
  /*
   * div/mod can fault in the final speculative prefetch.
   */
  switch (in->op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NYIR_SELECT_I64:
    return true;
  default:
    return false;
  }
}

/*
 * Deterministic fresh label above every label/branch operand/Pred-label.
 */
static int64_t sp_fresh_label(const nyir_func_t *f) {
  int64_t max_label = -1;
  for (size_t i = 0; i < f->len; ++i) {
    if ((f->data[i].op == NYIR_LABEL || f->data[i].op == NYIR_BR ||
         f->data[i].op == NYIR_BR_IF) &&
        f->data[i].imm > max_label)
      max_label = f->data[i].imm;
    for (size_t k = 0; k < f->data[i].phi_incoming_len; ++k)
      if (f->data[i].phi_incoming[k].predecessor_label > max_label)
        max_label = f->data[i].phi_incoming[k].predecessor_label;
  }
  return max_label == INT64_MAX ? -1 : max_label + 1;
}

static bool sp_try_one(nyir_func_t *work, const nyir_scev_loop_t *loop) {
  if (!work || !loop || !loop->trip_count_known || loop->trip_count < 1 ||
      loop->step != 1 || loop->iv < 0)
    return false;

  nyir_cfg_t cfg = {0};
  bool *in_loop = NULL;
  bool *isA = NULL;
  int *map = NULL;
  bool *defined_in_body = NULL;
  bool *backed = NULL;
  int *ainit_by = NULL, *apref_by = NULL, *rot_by = NULL, *bfresh_by = NULL;
  int *body_src_idx = NULL;
  bool changed = false;
  nyir_inst_t *out = NULL;
  size_t olen = 0;

  if (!nyir_cfg_build(work, &cfg))
    goto out;
  if (loop->header_block >= cfg.block_count ||
      loop->latch_block >= cfg.block_count ||
      loop->preheader_block >= cfg.block_count ||
      loop->header_block == loop->latch_block)
    goto out;

  in_loop = calloc(cfg.block_count, sizeof(*in_loop));
  if (!in_loop ||
      !nyir_cfg_natural_loop_blocks(&cfg, loop->latch_block,
                                    loop->header_block, in_loop,
                                    cfg.block_count))
    goto out;
  size_t loop_blocks = 0;
  for (size_t b = 0; b < cfg.block_count; ++b)
    loop_blocks += in_loop[b] ? 1u : 0u;
  if (loop_blocks != 2 || !in_loop[loop->header_block] ||
      !in_loop[loop->latch_block])
    goto out;

  const size_t h = loop->header_block, l = loop->latch_block;
  const size_t p = loop->preheader_block;
  if (!(p < h && h < l))
    goto out;

  int64_t hlabel = cfg.block_label[h];
  int64_t llabel = cfg.block_label[l];
  int64_t plabel = cfg.block_label[p];
  if (hlabel < 0 || llabel < 0)
    goto out;

  size_t pend = sp_last_non_nop(work, &cfg, p);
  size_t hend = sp_last_non_nop(work, &cfg, h);
  size_t lend = sp_last_non_nop(work, &cfg, l);
  if (pend <= cfg.block_start[p] || hend <= cfg.block_start[h] ||
      lend <= cfg.block_start[l])
    goto out;
  nyir_inst_t *pterm = &work->data[pend - 1];
  const nyir_inst_t *hterm = &work->data[hend - 1];
  const nyir_inst_t *lterm = &work->data[lend - 1];
  if (pterm->op != NYIR_BR || pterm->imm != hlabel ||
      hterm->op != NYIR_BR_IF || lterm->op != NYIR_BR ||
      lterm->imm != hlabel)
    goto out;
  size_t taken = SIZE_MAX;
  for (size_t b = 0; b < cfg.block_count; ++b)
    if (cfg.block_label[b] == hterm->imm) {
      taken = b;
      break;
    }
  if (taken != l)
    goto out;

  /*
   * Header scan.
   */
  size_t phi_begin = cfg.block_start[h];
  if (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_LABEL)
    ++phi_begin;
  while (phi_begin < cfg.block_end[h] && work->data[phi_begin].op == NYIR_NOP)
    ++phi_begin;

  size_t next_value = (size_t)work->next_value;
  int phi_dst[32], phi_back[32];
  int phi_count = 0;
  bool saw_phi = false;
  int guard_iv = -1;
  bool have_guard = false;
  int iv_init = -1;
  for (size_t i = phi_begin; i < hend - 1; ++i) {
    nyir_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    if (in->op == NYIR_PHI) {
      saw_phi = true;
      if (in->dst < 0 || (size_t)in->dst >= next_value ||
          in->phi_incoming_len != 2 || phi_count >= 32)
        goto out;
      int back = -1, pre = -1;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        const nyir_phi_incoming_t *inc = &in->phi_incoming[k];
        if (inc->predecessor_label == llabel)
          back = inc->value;
        else if (inc->predecessor_label == plabel)
          pre = inc->value;
      }
      if (back < 0)
        goto out;
      phi_dst[phi_count] = in->dst;
      phi_back[phi_count] = back;
      if (in->dst == loop->iv)
        iv_init = pre;
      ++phi_count;
      continue;
    }
    if (in->op == NYIR_CMP_I64) {
      if (have_guard || in->cmp != NYIR_CMP_LT || in->a < 0 || in->b < 0)
        goto out;
      guard_iv = in->a;
      have_guard = true;
      continue;
    }
    goto out;
  }
  if (!saw_phi || !have_guard || guard_iv != loop->iv || iv_init < 0)
    goto out;

  /*
   * Latch body slice.
   */
  size_t body_begin = cfg.block_start[l];
  if (body_begin < cfg.block_end[l] && work->data[body_begin].op == NYIR_LABEL)
    ++body_begin;
  while (body_begin < lend - 1 && work->data[body_begin].op == NYIR_NOP)
    ++body_begin;
  size_t body_count = 0;
  for (size_t i = body_begin; i + 1 < lend; ++i)
    if (work->data[i].op != NYIR_NOP)
      ++body_count;
  if (body_count == 0 || body_count > SP_MAX_BODY)
    goto out;

  body_src_idx = calloc(body_count, sizeof(*body_src_idx));
  isA = calloc(next_value ? next_value : 1, sizeof(*isA));
  defined_in_body = calloc(next_value ? next_value : 1, sizeof(*defined_in_body));
  backed = calloc(next_value ? next_value : 1, sizeof(*backed));
  if (!body_src_idx || !isA || !defined_in_body || !backed)
    goto out;
  size_t bcnt = 0;
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    if (work->data[i].op == NYIR_NOP)
      continue;
    body_src_idx[bcnt++] = (int)i;
    if (work->data[i].dst >= 0 && (size_t)work->data[i].dst < next_value)
      defined_in_body[work->data[i].dst] = true;
  }
  for (size_t i = body_begin; i + 1 < lend; ++i) {
    if (work->data[i].op == NYIR_NOP)
      continue;
    if (!sp_body_pure(&work->data[i]))
      goto out;
  }

  for (int q = 0; q < phi_count; ++q)
    if (phi_back[q] >= 0 && (size_t)phi_back[q] < next_value)
      backed[phi_back[q]] = 1;
  for (int q = 0; q < phi_count; ++q)
    if (phi_dst[q] >= 0 && (size_t)phi_dst[q] < next_value)
      backed[phi_dst[q]] = 1;

  /*
   * Stage-A classification (single pass over body in SSA order).
   */
  for (size_t k = 0; k < bcnt; ++k) {
    const nyir_inst_t *in = &work->data[body_src_idx[k]];
    if (in->dst < 0 || (size_t)in->dst >= next_value ||
        backed[in->dst])
      continue;
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    bool okA = true;
    for (int o = 0; o < 6; ++o) {
      int u = ops[o];
      if (u < 0)
        continue;
      if (u == loop->iv)
        continue;
      if ((size_t)u < next_value && isA[u])
        continue;
      if (!defined_in_body[u]) {
        bool is_phi = false;
        for (int q = 0; q < phi_count; ++q)
          if (phi_dst[q] == u)
            is_phi = true;
        if (!is_phi)
          continue; /* loop-invariant leaf */
      }
      okA = false;
      break;
    }
    if (okA)
      isA[in->dst] = true;
  }

  int a_count = 0;
  for (size_t k = 0; k < bcnt; ++k) {
    const nyir_inst_t *in = &work->data[body_src_idx[k]];
    if (in->dst >= 0 && (size_t)in->dst < next_value && isA[in->dst])
      ++a_count;
  }
  if (a_count == 0)
    goto out;

  /*
   * Stage-A values must be unused outside the latch.
   */
  for (size_t i = 0; i < work->len; ++i) {
    if (cfg.inst_block[i] == l)
      continue;
    const nyir_inst_t *in = &work->data[i];
    if (in->op == NYIR_NOP)
      continue;
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (int o = 0; o < 6; ++o)
      if (ops[o] >= 0 && (size_t)ops[o] < next_value && isA[ops[o]])
        goto out;
  }

  map = malloc((next_value ? next_value : 1) * sizeof(*map));
  rot_by = malloc((next_value ? next_value : 1) * sizeof(*rot_by));
  ainit_by = malloc((next_value ? next_value : 1) * sizeof(*ainit_by));
  apref_by = malloc((next_value ? next_value : 1) * sizeof(*apref_by));
  bfresh_by = malloc((next_value ? next_value : 1) * sizeof(*bfresh_by));
  if (!map || !rot_by || !ainit_by || !apref_by || !bfresh_by)
    goto out;
  for (size_t i = 0; i < next_value; ++i) {
    map[i] = -1;
    rot_by[i] = -1;
    ainit_by[i] = -1;
    apref_by[i] = -1;
    bfresh_by[i] = -1;
  }

  /*
   * Fresh ids per A value and B value.
   */
  for (size_t k = 0; k < bcnt; ++k) {
    const nyir_inst_t *in = &work->data[body_src_idx[k]];
    int v = in->dst;
    if (v < 0 || (size_t)v >= next_value)
      continue;
    if (isA[v]) {
      if (work->next_value > INT_MAX - 3)
        goto out;
      rot_by[v] = work->next_value++;
      ainit_by[v] = work->next_value++;
      apref_by[v] = work->next_value++;
    } else {
      if (work->next_value == INT_MAX)
        goto out;
      bfresh_by[v] = work->next_value++;
    }
  }

  /*
   * B-clone dst wiring: original dst -> bfresh.
   */
  for (size_t k = 0; k < bcnt; ++k) {
    const nyir_inst_t *in = &work->data[body_src_idx[k]];
    if (in->dst >= 0 && (size_t)in->dst < next_value && !isA[in->dst])
      map[in->dst] = bfresh_by[in->dst];
  }

  /*
   * Header PHI backedge replacements.
   */
  int phi_new_back[32];
  for (int q = 0; q < phi_count; ++q) {
    phi_new_back[q] = bfresh_by[phi_back[q]];
    if (phi_new_back[q] < 0)
      goto out;
  }

  /*
   * Prologue label.
   */
  int64_t pl = sp_fresh_label(work);
  if (pl < 0)
    goto out;

  /*
   * ------------------------------------------------------------------
   */
  size_t out_cap = work->len * 4 + 64;
  out = malloc(out_cap * sizeof(*out));
  if (!out)
    goto out;
#define SP_PUSH(inst_)                                \
  do {                                                \
    if (olen == out_cap) {                            \
      size_t nc = out_cap * 2;                        \
      nyir_inst_t *no = realloc(out, nc * sizeof(*no)); \
      if (!no) goto out;                              \
      out = no;                                       \
      out_cap = nc;                                   \
    }                                                 \
    out[olen++] = (inst_);                            \
  } while (0)

  for (size_t b = 0; b < cfg.block_count; ++b) {
    if (b == h) {
      /*
       * prologue block
       */
      {
        nyir_inst_t t = {0};
        t.op = NYIR_LABEL;
        t.dst = t.a = t.b = t.c = t.d = t.e = t.f = -1;
        t.imm = pl;
        SP_PUSH(t);
      }
      for (size_t k = 0; k < bcnt; ++k) {
        const nyir_inst_t *src = &work->data[body_src_idx[k]];
        int v = src->dst;
        if (v < 0 || (size_t)v >= next_value || !isA[v])
          continue;
        nyir_inst_t n = *src;
        n.dst = ainit_by[v];
        const int ops[6] = {src->a, src->b, src->c, src->d, src->e, src->f};
        int m[6];
        for (int o = 0; o < 6; ++o) {
          int u = ops[o];
          m[o] = u;
          if (u < 0)
            continue;
          if (u == loop->iv)
            m[o] = iv_init;
          else if ((size_t)u < next_value && isA[u] && ainit_by[u] >= 0)
            m[o] = ainit_by[u];
        }
        n.a = m[0]; n.b = m[1]; n.c = m[2]; n.d = m[3]; n.e = m[4]; n.f = m[5];
        n.flags = 0;
        n.effects = NYIR_EFFECT_NONE;
        n.symbol = NULL;
        SP_PUSH(n);
      }
      {
        nyir_inst_t t = {0};
        t.op = NYIR_BR;
        t.dst = t.a = t.b = t.c = t.d = t.e = t.f = -1;
        t.imm = hlabel;
        t.effects = NYIR_EFFECT_CONTROL;
        SP_PUSH(t);
      }
      for (size_t i = cfg.block_start[h]; i < phi_begin; ++i)
        SP_PUSH(work->data[i]);
      /*
       * header block: original PHIs, rotating PHIs, guard, br_if
       */
      for (size_t i = phi_begin; i < hend - 1; ++i) {
        const nyir_inst_t *in = &work->data[i];
        if (in->op == NYIR_NOP)
          continue;
        if (in->op == NYIR_PHI) {
          nyir_inst_t n = *in;
          for (size_t kk = 0; kk < n.phi_incoming_len; ++kk) {
            if (n.phi_incoming[kk].predecessor_label == llabel) {
              int nb = -1;
              for (int q = 0; q < phi_count; ++q)
                if (phi_dst[q] == n.dst)
                  nb = phi_new_back[q];
              if (nb >= 0)
                n.phi_incoming[kk].value = nb;
            } else if (n.phi_incoming[kk].predecessor_label == plabel) {
              n.phi_incoming[kk].predecessor_label = pl;
            }
          }
          SP_PUSH(n);
          continue;
        }
        if (in->op == NYIR_CMP_I64) {
          for (size_t k = 0; k < bcnt; ++k) {
            const nyir_inst_t *s2 = &work->data[body_src_idx[k]];
            int v = s2->dst;
            if (v < 0 || (size_t)v >= next_value || !isA[v])
              continue;
            nyir_inst_t phi = {0};
            phi.op = NYIR_PHI;
            phi.dst = rot_by[v];
            phi.a = phi.b = phi.c = phi.d = phi.e = phi.f = -1;
            nyir_phi_incoming_t *inc = calloc(2, sizeof(*inc));
            if (!inc)
              goto out;
            inc[0].predecessor_label = pl;
            inc[0].value = ainit_by[v];
            inc[1].predecessor_label = llabel;
            inc[1].value = apref_by[v];
            phi.phi_incoming = inc;
            phi.phi_incoming_len = 2;
            SP_PUSH(phi);
          }
          SP_PUSH(*in);
          continue;
        }
        SP_PUSH(*in);
      }
      SP_PUSH(*hterm);
      continue;
    }

    if (b == l) {
      /*
       * B clone
       */
      for (size_t i = cfg.block_start[l]; i < body_begin; ++i)
        SP_PUSH(work->data[i]);
      for (size_t k = 0; k < bcnt; ++k) {
        const nyir_inst_t *src = &work->data[body_src_idx[k]];
        int v = src->dst;
        if (v < 0 || (size_t)v >= next_value || isA[v])
          continue;
        nyir_inst_t n = *src;
        n.dst = bfresh_by[v];
        const int ops[6] = {src->a, src->b, src->c, src->d, src->e, src->f};
        int m[6];
        for (int o = 0; o < 6; ++o) {
          int u = ops[o];
          m[o] = u;
          if (u < 0)
            continue;
          if ((size_t)u < next_value && isA[u]) {
            m[o] = rot_by[u];
          } else if ((size_t)u < next_value && map[u] >= 0) {
            m[o] = map[u];
          }
        }
        n.a = m[0]; n.b = m[1]; n.c = m[2]; n.d = m[3]; n.e = m[4]; n.f = m[5];
        n.flags = 0;
        n.effects = NYIR_EFFECT_NONE;
        n.symbol = NULL;
        SP_PUSH(n);
      }
      /*
       * A prefetch: iv -> iv+1 (the iv phi's fresh back value).
       */
      int ivN = phi_new_back[0];
      for (int q = 0; q < phi_count; ++q)
        if (phi_dst[q] == loop->iv)
          ivN = phi_new_back[q];
      if (ivN < 0)
        goto out;
      for (size_t k = 0; k < bcnt; ++k) {
        const nyir_inst_t *src = &work->data[body_src_idx[k]];
        int v = src->dst;
        if (v < 0 || (size_t)v >= next_value || !isA[v])
          continue;
        nyir_inst_t n = *src;
        n.dst = apref_by[v];
        const int ops[6] = {src->a, src->b, src->c, src->d, src->e, src->f};
        int m[6];
        for (int o = 0; o < 6; ++o) {
          int u = ops[o];
          m[o] = u;
          if (u < 0)
            continue;
          if (u == loop->iv)
            m[o] = ivN;
          else if ((size_t)u < next_value && isA[u])
            m[o] = apref_by[u];
        }
        n.a = m[0]; n.b = m[1]; n.c = m[2]; n.d = m[3]; n.e = m[4]; n.f = m[5];
        n.flags = 0;
        n.effects = NYIR_EFFECT_NONE;
        n.symbol = NULL;
        SP_PUSH(n);
      }
      SP_PUSH(*lterm);
      continue;
    }

    if (b == p) {
      for (size_t i = cfg.block_start[p]; i < pend; ++i) {
        nyir_inst_t n = work->data[i];
        if (i + 1 == pend && n.op == NYIR_BR)
          n.imm = pl;
        SP_PUSH(n);
      }
      continue;
    }

    for (size_t i = cfg.block_start[b]; i < cfg.block_end[b]; ++i)
      SP_PUSH(work->data[i]);
  }

  /*
   * Verify and commit atomically.
   */
  {
    nyir_inst_t *saved = work->data;
    size_t saved_len = work->len, saved_cap = work->cap;
    work->data = out;
    work->len = olen;
    work->cap = out_cap;
    char err[256] = {0};
    if (nyir_verify(work, err, sizeof(err))) {
      free(saved);
      free(backed);
      free(body_src_idx);
      free(defined_in_body);
      free(isA);
      free(map);
      free(rot_by);
      free(ainit_by);
      free(apref_by);
      free(bfresh_by);
      free(in_loop);
      nyir_cfg_free(&cfg);
      return true;
    }
    work->data = saved;
    work->len = saved_len;
    work->cap = saved_cap;
  }

out:
  if (out) {
    for (size_t i = 0; i < olen; ++i)
      if (out[i].op == NYIR_PHI && out[i].dst >= 0 &&
          (size_t)out[i].dst >= next_value)
        free(out[i].phi_incoming);
  }
  free(out);
  free(backed);
  free(body_src_idx);
  free(defined_in_body);
  free(isA);
  free(map);
  free(rot_by);
  free(ainit_by);
  free(apref_by);
  free(bfresh_by);
  free(in_loop);
  nyir_cfg_free(&cfg);
  return changed;
}

bool nyir_softpipe_rewrite(nyir_func_t *f) {
  if (!f || f->len == 0)
    return true;

  nyir_scev_info_t info = {0};
  if (!nyir_scev_analyze(f, &info))
    return false;

  for (size_t i = 0; i < info.count; ++i) {
    const nyir_scev_loop_t *loop = &info.loops[i];
    if (!loop->trip_count_known || loop->trip_count < 1 ||
        loop->limit_value < 0 || loop->step != 1)
      continue;

    nyir_func_t candidate = {0};
    if (!nyir_func_clone(f, &candidate)) {
      nyir_scev_free(&info);
      return false;
    }
    if (sp_try_one(&candidate, loop)) {
      char err[256] = {0};
      if (nyir_verify(&candidate, err, sizeof(err))) {
        nyir_func_free(f);
        *f = candidate;
        nyir_scev_free(&info);
        return true;
      }
    }
    nyir_func_free(&candidate);
  }

  nyir_scev_free(&info);
  return true;
}

#undef SP_PUSH