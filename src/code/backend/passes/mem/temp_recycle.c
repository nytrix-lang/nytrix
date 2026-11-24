/*
 * Recycle non-escaping BigInt/BigFloat temporaries and loop accumulators by
 * inserting guarded rt_free_temp* calls in post-inline NYIR before backend
 * lowering (shared by NYR x86 and LLVM). Gated by NYTRIX_NO_TEMP_RECYCLE=1.
 */
#include "base/trace.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tr_disabled = -1;
static int g_tr_trace = -1;

static inline bool tr_disabled(void) {
  if (__builtin_expect(g_tr_disabled < 0, 0)) {
    const char *e = getenv("NYTRIX_NO_TEMP_RECYCLE");
    g_tr_disabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return g_tr_disabled != 0;
}

static inline bool tr_trace_enabled(void) {
  if (__builtin_expect(g_tr_trace < 0, 0)) {
    const char *e = getenv("NYTRIX_TRACE_TEMP_RECYCLE");
    g_tr_trace = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
  }
  return g_tr_trace != 0;
}

static bool tr_is_fresh_producer(const nyr_inst_t *in) {
  if (!in || in->op != NYIR_CALL || in->dst < 0 || !in->symbol)
    return false;
  static const char *const PRODUCERS[] = {
      "rt_bigint_add",         "rt_bigint_sub",
      "rt_bigint_mul",         "rt_bigint_div",
      "rt_bigint_mod",         "rt_bigint_pow",
      "rt_bigint_neg_raw",     "rt_bigint_neg",
      "rt_bigint_abs",         "rt_bigint_and",
      "rt_bigint_or",          "rt_bigint_xor",
      "rt_bigint_shl",         "rt_bigint_shr",
      "rt_bigint_gcd",         "rt_bigint_mul_div_small",
      "rt_bigint_from_i64_raw","rt_bigint_from_int",
      "rt_bigint_from_str",    "rt_bigfloat_add",
      "rt_bigfloat_sub",       "rt_bigfloat_mul",
      "rt_bigfloat_div",       "rt_bigfloat_sqrt",
      "rt_bigfloat_neg",       "rt_bigfloat_abs",
      "rt_bigfloat_pow_int",   "rt_bigfloat_pow_int_raw",
      "rt_bigfloat_from_value","rt_bigfloat_from_value_raw",
  };
  for (size_t i = 0; i < sizeof(PRODUCERS) / sizeof(PRODUCERS[0]); ++i) {
    if (strcmp(in->symbol, PRODUCERS[i]) == 0)
      return true;
  }
  return false;
}

static bool tr_is_alias_forwarder(const nyr_inst_t *in) {
  if (!in || in->dst < 0)
    return false;
  if (in->op == NYIR_COPY)
    return in->a >= 0;
  if (in->op != NYIR_CALL || (in->flags & NYIR_INST_F_INDIRECT) ||
      !in->symbol || in->imm < 1 || in->a < 0)
    return false;
  return strcmp(in->symbol, "rt_any_to_i64") == 0 ||
         strcmp(in->symbol, "rt_untag_i64") == 0 ||
         strcmp(in->symbol, "rt_box_i64") == 0 ||
         strcmp(in->symbol, "rt_tag") == 0;
}

static bool tr_is_safe_rt_symbol(const char *sym) {
  if (!sym)
    return false;
  if (strncmp(sym, "rt_bigint_", 10) == 0 ||
      strncmp(sym, "rt_bigfloat_", 12) == 0 ||
      strncmp(sym, "rt_free_temp", 12) == 0)
    return true;
  static const char *const EXTRA[] = {
      "rt_len", "rt_value_tag", "rt_native_has_tag", "rt_f64_to_i64",
  };
  for (size_t i = 0; i < sizeof(EXTRA) / sizeof(EXTRA[0]); ++i) {
    if (strcmp(sym, EXTRA[i]) == 0)
      return true;
  }
  return false;
}

static bool tr_is_borrowing_use(const nyr_inst_t *in, int v) {
  if (!in)
    return false;
  switch (in->op) {
  case NYIR_CMP_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_BR_IF:
    return true;
  case NYIR_CALL:
    if ((in->flags & NYIR_INST_F_INDIRECT) || !in->symbol)
      return false;
    (void)v;
    return tr_is_safe_rt_symbol(in->symbol) || tr_is_alias_forwarder(in);
  default:
    return false;
  }
}

static inline bool tr_is_terminator(nyr_op_t op) {
  return op == NYIR_BR || op == NYIR_BR_IF || op == NYIR_RET;
}

static size_t tr_block_tail_pos(const nyr_func_t *f, const nyr_cfg_t *cfg,
                                size_t block) {
  size_t s = cfg->block_start[block];
  size_t e = cfg->block_end[block];
  for (size_t i = s; i < e; ++i) {
    if (tr_is_terminator(f->data[i].op))
      return i;
  }
  return e;
}

static size_t tr_block_phi_pos(const nyr_func_t *f, const nyr_cfg_t *cfg,
                               size_t block) {
  size_t s = cfg->block_start[block];
  size_t e = cfg->block_end[block];
  if (s < e && f->data[s].op == NYIR_LABEL)
    return s + 1;
  return s;
}

typedef struct {
  size_t pos;
  uint32_t seq;
  nyr_inst_t inst;
} tr_edit_t;

typedef struct {
  tr_edit_t *items;
  size_t len;
  size_t cap;
  uint32_t next_seq;
} tr_edit_vec_t;

static void tr_edits_free(tr_edit_vec_t *v, bool free_owned_payloads) {
  if (!v)
    return;
  if (free_owned_payloads) {
    for (size_t i = 0; i < v->len; ++i)
      free(v->items[i].inst.phi_incoming);
  }
  free(v->items);
  v->items = NULL;
  v->len = v->cap = 0;
}

static bool tr_edits_push(tr_edit_vec_t *v, size_t pos, nyr_inst_t inst) {
  if (v->len == v->cap) {
    size_t ncap = v->cap ? v->cap * 2u : 32u;
    tr_edit_t *next = (tr_edit_t *)realloc(v->items, ncap * sizeof(*next));
    if (!next)
      return false;
    v->items = next;
    v->cap = ncap;
  }
  v->items[v->len++] = (tr_edit_t){.pos = pos, .seq = v->next_seq++, .inst = inst};
  return true;
}

static int tr_edit_cmp_desc(const void *pa, const void *pb) {
  const tr_edit_t *a = (const tr_edit_t *)pa;
  const tr_edit_t *b = (const tr_edit_t *)pb;
  if (a->pos != b->pos)
    return a->pos > b->pos ? -1 : 1;
  if (a->seq != b->seq)
    return a->seq > b->seq ? -1 : 1;
  return 0;
}

static bool tr_apply_edits(nyr_func_t *f, tr_edit_vec_t *v) {
  if (!v || v->len == 0)
    return true;
  if (!nir_ensure_inst_space(f, v->len))
    return false;
  qsort(v->items, v->len, sizeof(v->items[0]), tr_edit_cmp_desc);
  for (size_t i = 0; i < v->len; ++i) {
    size_t pos = v->items[i].pos;
    if (pos > f->len)
      return false;
    memmove(&f->data[pos + 1], &f->data[pos],
            (f->len - pos) * sizeof(f->data[0]));
    f->data[pos] = v->items[i].inst;
    f->len++;
  }
  return true;
}

static nyr_inst_t tr_make_base(nyr_op_t op, int dst) {
  nyr_inst_t in = {0};
  in.op = op;
  in.dst = dst;
  in.a = in.b = in.c = in.d = in.e = in.f = -1;
  in.cmp = NYIR_CMP_EQ;
  in.effects = nyr_inst_effects(&in);
  return in;
}

static nyr_inst_t tr_make_const(nyr_func_t *f, int64_t val, int *out_v) {
  int dst = f->next_value++;
  nyr_inst_t in = tr_make_base(NYIR_CONST_I64, dst);
  in.imm = val;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  *out_v = dst;
  return in;
}

static nyr_inst_t tr_make_phi2(nyr_func_t *f, int64_t l0, int v0, int64_t l1,
                               int v1, int *out_v) {
  nyr_phi_incoming_t *inc =
      (nyr_phi_incoming_t *)malloc(2u * sizeof(nyr_phi_incoming_t));
  if (!inc) {
    *out_v = -1;
    return tr_make_base(NYIR_NOP, -1);
  }
  inc[0] = (nyr_phi_incoming_t){.predecessor_label = l0, .value = v0};
  inc[1] = (nyr_phi_incoming_t){.predecessor_label = l1, .value = v1};
  int dst = f->next_value++;
  nyr_inst_t in = tr_make_base(NYIR_PHI, dst);
  in.phi_incoming = inc;
  in.phi_incoming_len = 2;
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  *out_v = dst;
  return in;
}

static nyr_inst_t tr_make_free_call(nyr_func_t *f, const int *guards,
                                    size_t guard_count, int ptr_v, bool is_loop,
                                    int owned_v) {
  const char *name = "rt_free_temp";
  int argc = 1;
  int args[4] = {ptr_v, -1, -1, -1};
  if (is_loop) {
    name = "rt_free_temp_loop";
    argc = 4;
    args[1] = guards[0];
    args[2] = guards[1];
    args[3] = owned_v;
  } else if (guard_count == 1) {
    name = "rt_free_temp_guard1";
    argc = 2;
    args[1] = guards[0];
  } else if (guard_count == 2) {
    name = "rt_free_temp_guard2";
    argc = 3;
    args[1] = guards[0];
    args[2] = guards[1];
  } else if (guard_count >= 3) {
    name = "rt_free_temp_guard3";
    argc = 4;
    args[1] = guards[0];
    args[2] = guards[1];
    args[3] = guards[2];
  }
  int dst = f->next_value++;
  nyr_inst_t in = tr_make_base(NYIR_CALL, dst);
  in.flags = NYIR_INST_F_EXTERN | NYIR_INST_F_EFFECTS_KNOWN;
  in.effects =
      NYIR_EFFECT_CALL | NYIR_EFFECT_READ_MEMORY | NYIR_EFFECT_WRITE_MEMORY;
  in.symbol = nyr_func_own_symbol_copy(f, name);
  in.imm = argc;
  in.a = args[0];
  in.b = args[1];
  in.c = args[2];
  in.d = args[3];
  in.scalar_class = NYIR_SCALAR_I64_RAW;
  return in;
}

#define TR_MAX_CLOSURE 64

typedef struct {
  int vals[TR_MAX_CLOSURE];
  size_t val_count;
  size_t users[TR_MAX_CLOSURE * 2u];
  size_t user_count;
} tr_closure_t;

static bool tr_collect_closure(const nyr_func_t *f, const nyr_use_def_t *ud,
                               int root, tr_closure_t *out) {
  if (!f || !ud || root < 0 || (size_t)root >= ud->value_count)
    return false;
  out->val_count = 0;
  out->user_count = 0;
  out->vals[out->val_count++] = root;
  for (size_t qi = 0; qi < out->val_count; ++qi) {
    int cur = out->vals[qi];
    if (cur < 0 || (size_t)cur >= ud->value_count)
      return false;
    size_t u0 = ud->offsets[cur];
    size_t u1 = ud->offsets[cur + 1];
    for (size_t k = u0; k < u1; ++k) {
      size_t ui = ud->users[k];
      if (ui >= f->len)
        return false;
      const nyr_inst_t *u = &f->data[ui];
      if (tr_is_alias_forwarder(u) && u->a == cur) {
        bool seen = false;
        for (size_t p = 0; p < out->val_count; ++p)
          if (out->vals[p] == u->dst) {
            seen = true;
            break;
          }
        if (!seen) {
          if (out->val_count >= TR_MAX_CLOSURE)
            return false;
          out->vals[out->val_count++] = u->dst;
        }
        if (u->op == NYIR_COPY)
          continue;
      }
      bool seen_u = false;
      for (size_t p = 0; p < out->user_count; ++p)
        if (out->users[p] == ui) {
          seen_u = true;
          break;
        }
      if (!seen_u) {
        if (out->user_count >= TR_MAX_CLOSURE * 2u)
          return false;
        out->users[out->user_count++] = ui;
      }
    }
  }
  return true;
}

static bool tr_closure_contains(const tr_closure_t *cl, int val) {
  for (size_t i = 0; i < cl->val_count; ++i)
    if (cl->vals[i] == val)
      return true;
  return false;
}

static void tr_add_guard(int *guards, size_t *count, int val, int exclude) {
  if (val < 0 || val == exclude || *count >= 4)
    return;
  for (size_t i = 0; i < *count; ++i)
    if (guards[i] == val)
      return;
  guards[(*count)++] = val;
}

static bool tr_value_dominates_point(const nyr_cfg_t *cfg, const int *defs,
                                     int v, size_t blk, size_t before_pos) {
  if (v < 0 || defs[v] < 0)
    return false;
  size_t di = (size_t)defs[v];
  size_t dblk = cfg->inst_block[di];
  if (dblk == blk)
    return di < before_pos;
  return nyr_cfg_dominates(cfg, dblk, blk);
}

/*
 * Verify whether `src_blk` precedes `target_blk` on all acyclic paths inside
 * the loop body `member` (with header `header` cut off so backedges don't loop).
 */
static bool tr_block_precedes_in_loop(const nyr_cfg_t *cfg, size_t header,
                                      const bool *member, size_t use_blk,
                                      size_t cut_blk, size_t cut_pos,
                                      size_t use_pos) {
  if (use_blk == cut_blk)
    return use_pos < cut_pos;
  /* Start DFS/BFS from cut_blk's successors inside member \ {header}:
   * if use_blk is NOT reachable after cut_blk, then use_blk always runs
   * strictly before cut_blk along iteration flow. */
  bool visited[256];
  size_t stack[256];
  if (cfg->block_count > 256)
    return nyr_cfg_dominates(cfg, use_blk, cut_blk);
  memset(visited, 0, cfg->block_count * sizeof(bool));
  size_t sp = 0;
  for (size_t si = cfg->succ_offsets[cut_blk]; si < cfg->succ_offsets[cut_blk + 1];
       ++si) {
    size_t sb = cfg->succ_blocks[si];
    if (sb != header && member[sb] && !visited[sb]) {
      visited[sb] = true;
      stack[sp++] = sb;
    }
  }
  while (sp > 0) {
    size_t cur = stack[--sp];
    if (cur == use_blk)
      return false;
    for (size_t si = cfg->succ_offsets[cur]; si < cfg->succ_offsets[cur + 1];
         ++si) {
      size_t sb = cfg->succ_blocks[si];
      if (sb != header && member[sb] && !visited[sb]) {
        visited[sb] = true;
        stack[sp++] = sb;
      }
    }
  }
  return nyr_cfg_dominates(cfg, use_blk, cut_blk) || !visited[use_blk];
}

static bool tr_find_header_preds(const nyr_cfg_t *cfg, size_t header,
                                 const bool *member, size_t *out_pre,
                                 size_t *out_latch) {
  size_t p0 = cfg->pred_offsets[header];
  size_t p1 = cfg->pred_offsets[header + 1];
  if (p1 - p0 != 2)
    return false;
  size_t a = cfg->pred_blocks[p0];
  size_t b = cfg->pred_blocks[p0 + 1];
  bool a_in = member[a];
  bool b_in = member[b];
  if (a_in == b_in)
    return false;
  size_t pre = a_in ? b : a;
  size_t latch = a_in ? a : b;
  if (!nyr_cfg_dominates(cfg, header, latch) ||
      !nyr_cfg_dominates(cfg, pre, header))
    return false;
  if (cfg->succ_offsets[latch + 1] - cfg->succ_offsets[latch] != 1)
    return false;
  *out_pre = pre;
  *out_latch = latch;
  return true;
}

static bool tr_match_phi2_labels(const nyr_inst_t *phi, int64_t l0, int64_t l1,
                                 int *v0, int *v1) {
  if (!phi || phi->op != NYIR_PHI || phi->phi_incoming_len != 2)
    return false;
  if (phi->phi_incoming[0].predecessor_label == l0 &&
      phi->phi_incoming[1].predecessor_label == l1) {
    *v0 = phi->phi_incoming[0].value;
    *v1 = phi->phi_incoming[1].value;
    return true;
  }
  if (phi->phi_incoming[0].predecessor_label == l1 &&
      phi->phi_incoming[1].predecessor_label == l0) {
    *v0 = phi->phi_incoming[1].value;
    *v1 = phi->phi_incoming[0].value;
    return true;
  }
  return false;
}

static bool tr_global_sym_safe_in_func(const nyr_func_t *f,
                                       const nyr_use_def_t *ud, const int *defs,
                                       const char *sym) {
  if (!sym || !sym[0])
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_ADDR_SYMBOL || !in->symbol ||
        strcmp(in->symbol, sym) != 0 || in->dst < 0)
      continue;
    int v = in->dst;
    size_t u0 = ud->offsets[v];
    size_t u1 = ud->offsets[v + 1];
    for (size_t k = u0; k < u1; ++k) {
      size_t ui = ud->users[k];
      const nyr_inst_t *u = &f->data[ui];
      if (u->op == NYIR_LOAD_I64 && u->a == v)
        continue;
      if (u->op == NYIR_STORE_I64 && u->a == v && u->c != v)
        continue;
      return false;
    }
  }
  (void)defs;
  return true;
}

static bool tr_collect_cases(nyr_func_t *f, const nyr_cfg_t *cfg,
                             const int *defs, const nyr_use_def_t *ud,
                             tr_edit_vec_t *edits) {
  size_t c1_cnt = 0, c2_cnt = 0, c3_cnt = 0;
  bool *claimed_def = (bool *)calloc(f->len ? f->len : 1, sizeof(bool));
  bool *member =
      cfg->block_count ? (bool *)calloc(cfg->block_count, sizeof(bool)) : NULL;
  if (!claimed_def || (cfg->block_count && !member)) {
    free(claimed_def);
    free(member);
    return false;
  }

  /*
   * Case 2 (loop accumulator PHIs) & Case 3 (overwritten loop globals):
   * Inspect each unique natural loop header with a single backedge latch.
   */
  for (size_t h = 0; h < cfg->block_count; ++h) {
    size_t latch_found = SIZE_MAX;
    size_t latch_edges = 0;
    for (size_t pi = cfg->pred_offsets[h]; pi < cfg->pred_offsets[h + 1];
         ++pi) {
      size_t p = cfg->pred_blocks[pi];
      if (nyr_cfg_is_backedge(cfg, p, h)) {
        latch_found = p;
        latch_edges++;
      }
    }
    if (latch_edges != 1 || latch_found == SIZE_MAX)
      continue;
    if (!nyr_cfg_natural_loop_blocks(cfg, latch_found, h, member,
                                     cfg->block_count))
      continue;
    size_t pre = SIZE_MAX, latch = SIZE_MAX;
    if (!tr_find_header_preds(cfg, h, member, &pre, &latch))
      continue;
    int64_t pre_lbl = cfg->block_label[pre];
    int64_t latch_lbl = cfg->block_label[latch];
    size_t phi_pos = tr_block_phi_pos(f, cfg, h);
    size_t pre_tail = tr_block_tail_pos(f, cfg, pre);
    size_t latch_tail = tr_block_tail_pos(f, cfg, latch);

    /* Case 2: scan PHIs in loop header `h`. */
    for (size_t i = cfg->block_start[h]; i < cfg->block_end[h]; ++i) {
      const nyr_inst_t *phi = &f->data[i];
      if (phi->op != NYIR_PHI || phi->dst < 0)
        continue;
      int init_v = -1, next_v = -1;
      if (!tr_match_phi2_labels(phi, pre_lbl, latch_lbl, &init_v, &next_v))
        continue;
      if (init_v < 0 || next_v < 0)
        continue;

      int next_root = nyr_strip_copy_chain(f, defs, next_v, 0);
      if (next_root < 0 || defs[next_root] < 0)
        continue;
      const nyr_inst_t *next_def = &f->data[defs[next_root]];
      size_t next_blk = cfg->inst_block[defs[next_root]];
      if (!member[next_blk])
        continue;

      bool unconditional = tr_is_fresh_producer(next_def);
      bool conditional = false;
      int merge_unch_val = -1, merge_upd_val = -1;
      int64_t upd_lbl = -1;
      size_t upd_blk = SIZE_MAX, merge_blk = SIZE_MAX;
      if (!unconditional && next_def->op == NYIR_PHI &&
          next_def->phi_incoming_len == 2) {
        merge_blk = next_blk;
        int64_t ml0 = next_def->phi_incoming[0].predecessor_label;
        int mv0 = next_def->phi_incoming[0].value;
        int64_t ml1 = next_def->phi_incoming[1].predecessor_label;
        int mv1 = next_def->phi_incoming[1].value;
        int r0 = nyr_strip_copy_chain(f, defs, mv0, 0);
        int r1 = nyr_strip_copy_chain(f, defs, mv1, 0);
        if (r0 == phi->dst && r1 >= 0 && defs[r1] >= 0 &&
            member[cfg->inst_block[defs[r1]]] &&
            tr_is_fresh_producer(&f->data[defs[r1]]) &&
            nyr_cfg_block_for_label(cfg, ml1, &upd_blk) && member[upd_blk] &&
            merge_blk == latch) {
          conditional = true;
          merge_unch_val = mv0;
          merge_upd_val = mv1;
          upd_lbl = ml1;
          (void)ml0;
        } else if (r1 == phi->dst && r0 >= 0 && defs[r0] >= 0 &&
                   member[cfg->inst_block[defs[r0]]] &&
                   tr_is_fresh_producer(&f->data[defs[r0]]) &&
                   nyr_cfg_block_for_label(cfg, ml0, &upd_blk) &&
                   member[upd_blk] && merge_blk == latch) {
          conditional = true;
          merge_unch_val = mv1;
          merge_upd_val = mv0;
          upd_lbl = ml0;
          (void)ml1;
        }
      }
      if (!unconditional && !conditional)
        continue;

      /* Verify all in-loop uses of phi (via copy closure) borrow and precede
       * latch_tail. */
      tr_closure_t cl;
      if (!tr_collect_closure(f, ud, phi->dst, &cl))
        continue;
      bool uses_ok = true;
      for (size_t u = 0; u < cl.user_count; ++u) {
        size_t ui = cl.users[u];
        size_t ublk = cfg->inst_block[ui];
        if (!member[ublk])
          continue; /* live exit use outside loop is valid! */
        const nyr_inst_t *uinst = &f->data[ui];
        if (conditional && ui == (size_t)defs[next_root] &&
            (merge_unch_val == phi->dst || tr_closure_contains(&cl, merge_unch_val)))
          continue;
        if (!tr_is_borrowing_use(uinst, phi->dst)) {
          uses_ok = false;
          break;
        }
        if (!tr_block_precedes_in_loop(cfg, h, member, ublk, latch, latch_tail,
                                       ui)) {
          uses_ok = false;
          break;
        }
      }
      if (!uses_ok)
        continue;

      /* Verify init_v and next_v dominate latch_tail. */
      if (!tr_value_dominates_point(cfg, defs, init_v, latch, latch_tail) ||
          !tr_value_dominates_point(cfg, defs, next_v, latch, latch_tail))
        continue;

      if (unconditional) {
        int c0 = -1, c1 = -1, owned_phi = -1;
        tr_edits_push(edits, pre_tail, tr_make_const(f, 0, &c0));
        tr_edits_push(edits, latch_tail, tr_make_const(f, 1, &c1));
        tr_edits_push(edits, phi_pos,
                      tr_make_phi2(f, pre_lbl, c0, latch_lbl, c1, &owned_phi));
        int guards[2] = {init_v, next_v};
        tr_edits_push(edits, latch_tail,
                      tr_make_free_call(f, guards, 2, phi->dst, true, owned_phi));
        c2_cnt++;
      } else {
        int c0 = -1, c1 = -1, owned_phi = -1, owned_merge = -1;
        int upd_tail = (int)tr_block_tail_pos(f, cfg, upd_blk);
        size_t merge_phi_pos = tr_block_phi_pos(f, cfg, merge_blk);
        int64_t unch_lbl = (upd_lbl == next_def->phi_incoming[0].predecessor_label)
                               ? next_def->phi_incoming[1].predecessor_label
                               : next_def->phi_incoming[0].predecessor_label;
        /* Allocate owned_phi dst upfront to break circularity with owned_merge */
        owned_phi = f->next_value++;
        owned_merge = f->next_value++;
        (void)merge_upd_val;
        tr_edits_push(edits, pre_tail, tr_make_const(f, 0, &c0));
        tr_edits_push(edits, (size_t)upd_tail, tr_make_const(f, 1, &c1));

        nyr_phi_incoming_t *h_inc =
            (nyr_phi_incoming_t *)malloc(2u * sizeof(nyr_phi_incoming_t));
        nyr_phi_incoming_t *m_inc =
            (nyr_phi_incoming_t *)malloc(2u * sizeof(nyr_phi_incoming_t));
        if (!h_inc || !m_inc) {
          free(h_inc);
          free(m_inc);
          free(claimed_def);
          free(member);
          return false;
        }
        h_inc[0] = (nyr_phi_incoming_t){pre_lbl, c0};
        h_inc[1] = (nyr_phi_incoming_t){latch_lbl, owned_merge};
        nyr_inst_t h_phi = tr_make_base(NYIR_PHI, owned_phi);
        h_phi.phi_incoming = h_inc;
        h_phi.phi_incoming_len = 2;
        h_phi.scalar_class = NYIR_SCALAR_I64_RAW;

        m_inc[0] = (nyr_phi_incoming_t){unch_lbl, owned_phi};
        m_inc[1] = (nyr_phi_incoming_t){upd_lbl, c1};
        nyr_inst_t m_phi = tr_make_base(NYIR_PHI, owned_merge);
        m_phi.phi_incoming = m_inc;
        m_phi.phi_incoming_len = 2;
        m_phi.scalar_class = NYIR_SCALAR_I64_RAW;

        tr_edits_push(edits, phi_pos, h_phi);
        tr_edits_push(edits, merge_phi_pos, m_phi);
        int guards[2] = {init_v, next_v};
        tr_edits_push(edits, latch_tail,
                      tr_make_free_call(f, guards, 2, phi->dst, true, owned_phi));
        c2_cnt++;
      }
    }

    /* Case 3: overwritten global store st.i64 sym, fresh_prod inside loop L */
    bool loop_calls_safe = true;
    for (size_t b = 0; b < cfg->block_count && loop_calls_safe; ++b) {
      if (!member[b])
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *in = &f->data[i];
        if (in->op == NYIR_CALL &&
            ((in->flags & NYIR_INST_F_INDIRECT) ||
             !(in->flags & NYIR_INST_F_EXTERN) ||
             !tr_is_safe_rt_symbol(in->symbol))) {
          loop_calls_safe = false;
          break;
        }
      }
    }
    if (!loop_calls_safe)
      continue;

    const char *seen_syms[16];
    size_t seen_sym_n = 0;
    for (size_t b = 0; b < cfg->block_count; ++b) {
      if (!member[b] || !nyr_cfg_dominates(cfg, b, latch))
        continue;
      for (size_t i = cfg->block_start[b]; i < cfg->block_end[b]; ++i) {
        const nyr_inst_t *st = &f->data[i];
        if (st->op != NYIR_STORE_I64 || st->a < 0 || st->c < 0 ||
            defs[st->a] < 0)
          continue;
        const nyr_inst_t *addr_def = &f->data[defs[st->a]];
        if (addr_def->op != NYIR_ADDR_SYMBOL || !addr_def->symbol)
          continue;
        const char *sym = addr_def->symbol;
        bool already = false;
        for (size_t k = 0; k < seen_sym_n; ++k)
          if (strcmp(seen_syms[k], sym) == 0) {
            already = true;
            break;
          }
        if (already)
          continue;
        int prod_v = nyr_strip_copy_chain(f, defs, st->c, 0);
        if (prod_v < 0 || defs[prod_v] < 0)
          continue;
        size_t prod_idx = (size_t)defs[prod_v];
        if (!member[cfg->inst_block[prod_idx]] || claimed_def[prod_idx])
          continue;
        const nyr_inst_t *prod = &f->data[prod_idx];
        if (!tr_is_fresh_producer(prod))
          continue;
        if (!tr_value_dominates_point(cfg, defs, prod_v, latch, latch_tail))
          continue;

        /* Ensure sym is only loaded/stored safely across function AND has 0
         * loads and only this store inside loop L. */
        if (!tr_global_sym_safe_in_func(f, ud, defs, sym))
          continue;
        size_t sym_stores = 0, sym_loads = 0;
        for (size_t lb = 0; lb < cfg->block_count; ++lb) {
          if (!member[lb])
            continue;
          for (size_t li = cfg->block_start[lb]; li < cfg->block_end[lb]; ++li) {
            const nyr_inst_t *lin = &f->data[li];
            if ((lin->op == NYIR_LOAD_I64 || lin->op == NYIR_STORE_I64) &&
                lin->a >= 0 && defs[lin->a] >= 0) {
              const nyr_inst_t *la = &f->data[defs[lin->a]];
              if (la->op == NYIR_ADDR_SYMBOL && la->symbol &&
                  strcmp(la->symbol, sym) == 0) {
                if (lin->op == NYIR_LOAD_I64)
                  sym_loads++;
                else
                  sym_stores++;
              }
            }
          }
        }
        if (sym_loads != 0 || sym_stores != 1)
          continue;

        /* Verify every use of prod only feeds `st` or borrows inside `b` < i. */
        tr_closure_t cl;
        if (!tr_collect_closure(f, ud, prod_v, &cl))
          continue;
        bool prod_ok = true;
        for (size_t u = 0; u < cl.user_count; ++u) {
          size_t ui = cl.users[u];
          if (ui == i)
            continue;
          if (cfg->inst_block[ui] != b || ui >= i ||
              !tr_is_borrowing_use(&f->data[ui], prod_v)) {
            prod_ok = false;
            break;
          }
        }
        if (!prod_ok)
          continue;

        /* Build guards against prod (and up to 2 dominating inputs if any) */
        int guards[4];
        size_t gc = 0;
        tr_add_guard(guards, &gc, prod_v, -1);
        if (prod->imm >= 1 && prod->a >= 0 &&
            tr_value_dominates_point(cfg, defs, prod->a, b, i + 1))
          tr_add_guard(guards, &gc, prod->a, -1);
        if (prod->imm >= 2 && prod->b >= 0 &&
            tr_value_dominates_point(cfg, defs, prod->b, b, i + 1))
          tr_add_guard(guards, &gc, prod->b, -1);
        if (gc > 3)
          continue;

        if (seen_sym_n < 16)
          seen_syms[seen_sym_n++] = sym;
        int c0 = -1, prev_phi = -1;
        tr_edits_push(edits, pre_tail, tr_make_const(f, 0, &c0));
        tr_edits_push(edits, phi_pos,
                      tr_make_phi2(f, pre_lbl, c0, latch_lbl, prod_v, &prev_phi));
        tr_edits_push(edits, i + 1,
                      tr_make_free_call(f, guards, gc, prev_phi, false, -1));
        c3_cnt++;
      }
    }
  }

  /*
   * Case 1: single-block non-escaping dead BigInt/BigFloat temps.
   */
  for (size_t b = 0; b < cfg->block_count; ++b) {
    size_t tail = tr_block_tail_pos(f, cfg, b);
    for (size_t i = cfg->block_start[b]; i < tail; ++i) {
      if (claimed_def[i])
        continue;
      const nyr_inst_t *def = &f->data[i];
      if (!tr_is_fresh_producer(def))
        continue;
      tr_closure_t cl;
      if (!tr_collect_closure(f, ud, def->dst, &cl) || cl.user_count == 0)
        continue;
      bool ok = true;
      size_t last_use = i;
      for (size_t u = 0; u < cl.user_count; ++u) {
        size_t ui = cl.users[u];
        if (cfg->inst_block[ui] != b || ui <= i || ui >= tail ||
            !tr_is_borrowing_use(&f->data[ui], def->dst)) {
          ok = false;
          break;
        }
        if (ui > last_use)
          last_use = ui;
      }
      if (!ok || last_use <= i)
        continue;
      int guards[4];
      size_t gc = 0;
      if (def->imm >= 1 && def->a >= 0)
        tr_add_guard(guards, &gc, def->a, def->dst);
      if (def->imm >= 2 && def->b >= 0)
        tr_add_guard(guards, &gc, def->b, def->dst);
      const nyr_inst_t *lu = &f->data[last_use];
      if (lu->op == NYIR_CALL && lu->dst >= 0 && lu->symbol &&
          (strncmp(lu->symbol, "rt_bigint_", 10) == 0 ||
           strncmp(lu->symbol, "rt_bigfloat_", 12) == 0))
        tr_add_guard(guards, &gc, lu->dst, def->dst);
      if (gc > 3)
        continue;
      tr_edits_push(edits, last_use + 1,
                    tr_make_free_call(f, guards, gc, def->dst, false, -1));
      claimed_def[i] = true;
      c1_cnt++;
    }
  }

  free(claimed_def);
  free(member);
  if (tr_trace_enabled() && (c1_cnt || c2_cnt || c3_cnt)) {
    fprintf(stderr, "[temp-recycle] case1=%zu case2=%zu case3=%zu\n", c1_cnt,
            c2_cnt, c3_cnt);
  }
  return true;
}

bool nyr_temp_recycle(nyr_func_t *f) {
  if (!f || f->len == 0 || tr_disabled())
    return false;
  bool has_candidate = false;
  for (size_t i = 0; i < f->len; ++i) {
    if (tr_is_fresh_producer(&f->data[i])) {
      has_candidate = true;
      break;
    }
  }
  if (!has_candidate)
    return false;

  nyr_func_t work = {0};
  if (!nyr_ir_func_clone(f, &work))
    return false;
  nyr_cfg_t cfg = {0};
  int *defs = NULL;
  nyr_use_def_t ud = {0};
  tr_edit_vec_t edits = {0};
  bool applied = false;

  if (!nyr_cfg_build(&work, &cfg) || !(defs = nyr_build_defs(&work)) ||
      !nyr_build_use_def(&work, &ud))
    goto fail;

  if (!tr_collect_cases(&work, &cfg, defs, &ud, &edits)) {
    tr_edits_free(&edits, true);
    goto fail;
  }
  if (edits.len == 0) {
    tr_edits_free(&edits, false);
    goto fail;
  }
  nyr_use_def_free(&ud);
  free(defs);
  defs = NULL;
  nyr_cfg_free(&cfg);

  if (!tr_apply_edits(&work, &edits)) {
    tr_edits_free(&edits, true);
    goto fail;
  }
  tr_edits_free(&edits, false);

  char verr[256] = {0};
  if (!nyr_ir_verify(&work, verr, sizeof(verr))) {
    if (tr_trace_enabled())
      fprintf(stderr, "[temp-recycle] verify failed: %s\n", verr);
    goto fail;
  }
  nyr_func_replace(f, &work);
  applied = true;
  return true;

fail:
  nyr_use_def_free(&ud);
  free(defs);
  nyr_cfg_free(&cfg);
  if (!applied)
    nyr_ir_func_free(&work);
  return false;
}
