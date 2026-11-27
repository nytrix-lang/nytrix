/*
 * Copy propagation: replaces uses of register-copy results with the
 * original source value when the copy chain is provably transparent.
 *
 * Straight-line functions use a linear alias map.  Functions with control
 * flow use a dominator-checked walk: a COPY dst <- src may only be folded
 * into a use when src is available at that use (src's definition dominates
 * the copy, and the copy dominates the use / the predecessor edge of a PHI
 * incoming value).  NYIR values are defined once, so in SSA form these
 * checks always succeed and the pass collapses the `mov` noise that
 * mem2reg and PHI simplification leave between PHIs and their uses.
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nir_alias_find(const int *alias, int v) {
  if (!alias || v < 0)
    return v;
  int cur = v;
  for (int depth = 0; depth < 64 && alias[cur] >= 0 && alias[cur] != cur;
       ++depth)
    cur = alias[cur];
  return cur;
}

/*
 * An annotated COPY can be an explicit one-word ABI class refinement.  It is
 * not a transparent alias even when the machine bits are unchanged.
 */
static bool nir_copy_has_class_contract(const nyr_func_t *f,
                                        const nyr_inst_t *in) {
  if (f && f->untagged_values && in && in->scalar_class == NYIR_SCALAR_I64_RAW)
    return false;
  return in && in->op == NYIR_COPY && in->scalar_class != NYIR_SCALAR_UNKNOWN;
}

static bool nir_copy_dst_calls_safe(const nyr_func_t *f, const int *defs,
                                    const unsigned char *call_used,
                                    const int *alias, const nyr_inst_t *in) {
  if (!call_used || !in || in->dst < 0 || !call_used[in->dst])
    return true;
  /*
   * Raw numeric scalars carry no ownership fact to contradict: the argument
   * is passed by value, and the per-position ABI class is unchanged when a
   * copy of the same scalar class is folded into the argument. Only object
   * and aggregate values need the ownership-preserving alias below.
   */
  if (in->scalar_class == NYIR_SCALAR_I64_RAW ||
      in->scalar_class == NYIR_SCALAR_F32 ||
      in->scalar_class == NYIR_SCALAR_F64)
    return true;
  /*
   * A call-argument alias may fold when its canonical producer carries an
   * ownership fact. The call site keeps its argument contract, and the
   * refresh below replaces the stale value-ownership fact with the producer's
   * fact. With unknown ownership, preserve the alias because the original
   * per-argument metadata cannot safely be reconstructed.
   */
  if (!f || !defs || !alias || in->dst >= f->next_value)
    return false;
  int source = nir_alias_find(alias, in->a);
  if (source < 0 || source >= f->next_value || defs[source] < 0)
    return false;
  return f->data[(size_t)defs[source]].semantic_ownership != NY_SEM_OWN_UNKNOWN;
}

/*
 * A class-contract COPY is still a transparent alias when it refines
 * nothing: the aliased source's own definition already carries the same
 * scalar class, so every consumer sees identical ABI classes with or
 * without the copy.  `vclass[v]` is the scalar class of v's definition
 * (NYIR_SCALAR_UNKNOWN when the def carries no class or v is an entry
 * value); only exact matches fold.
 */
static bool nir_copy_class_preserved(const nyr_scalar_class_t *vclass,
                                     const unsigned char *call_used,
                                     const int *alias, const nyr_inst_t *in) {
  if (!vclass || !alias || !in || in->scalar_class == NYIR_SCALAR_UNKNOWN)
    return false;
  /*
   * A copy whose destination feeds a call argument must not fold even when
   * the class matches: call sites carry per-argument ownership metadata
   * bound to the specific value, and rewriting the argument can contradict
   * it in ways post-rewrite refresh cannot always reconstruct.
   */
  if (call_used && in->dst >= 0 && call_used[in->dst] &&
      in->scalar_class != NYIR_SCALAR_I64_RAW &&
      in->scalar_class != NYIR_SCALAR_F32 &&
      in->scalar_class != NYIR_SCALAR_F64)
    return false;
  int src = nir_alias_find(alias, in->a);
  if (src < 0)
    return false;
  return vclass[src] == in->scalar_class;
}

/*
 * Values used only as the address operand of scalar loads and stores,
 * directly or through one ADD_I64 whose result is itself used that way.
 * A scalar-class COPY of such a value refines nothing a consumer reads:
 * memory operands take machine addresses, not ABI classes. Returns NULL when
 * disabled (NYTRIX_NO_ADDR_COPY_FOLD=1) or on allocation failure.
 */
static void nir_mark_use(const nyr_inst_t *in, int slot, int v,
                         unsigned char *bad) {
  if (v < 0)
    return;
  bool addr = slot == 0 &&
              (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
              in->b != v && in->c != v && in->d != v;
  if (!addr)
    bad[v] = 1;
}

static void nir_mark_add_use(const nyr_inst_t *in, int slot, int v,
                             const unsigned char *ldst_bad, unsigned char *bad) {
  if (v < 0)
    return;
  if (slot == 0 && (in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64) &&
      in->b != v && in->c != v && in->d != v)
    return;
  if ((slot == 0 || slot == 1) && in->op == NYIR_ADD_I64 && in->a != in->b &&
      in->dst >= 0 && !ldst_bad[in->dst] && in->extra_args_len == 0)
    return;
  bad[v] = 1;
}

static unsigned char *nir_addr_only_values(const nyr_func_t *f) {
  static int disabled = -1;
  if (disabled < 0) {
    const char *env = getenv("NYTRIX_NO_ADDR_COPY_FOLD");
    disabled = env && *env && strcmp(env, "0") != 0;
  }
  if (disabled || !f || f->next_value <= 0)
    return NULL;
  size_t nv = (size_t)f->next_value;
  unsigned char *ldst_bad = (unsigned char *)calloc(nv, 1);
  unsigned char *bad = (unsigned char *)calloc(nv, 1);
  if (!ldst_bad || !bad) {
    free(ldst_bad);
    free(bad);
    return NULL;
  }
  for (int pass = 0; pass < 2; ++pass) {
    unsigned char *out = pass == 0 ? ldst_bad : bad;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
      for (int s = 0; s < 6; ++s) {
        if (ops[s] < 0 || (size_t)ops[s] >= nv)
          continue;
        if (pass == 0)
          nir_mark_use(in, s, ops[s], out);
        else
          nir_mark_add_use(in, s, ops[s], ldst_bad, out);
      }
      for (size_t k = 0; k < in->extra_args_len; ++k)
        if (in->extra_args[k] >= 0 && (size_t)in->extra_args[k] < nv)
          out[in->extra_args[k]] = 1;
      for (size_t k = 0; k < in->phi_incoming_len; ++k)
        if (in->phi_incoming[k].value >= 0 &&
            (size_t)in->phi_incoming[k].value < nv)
          out[in->phi_incoming[k].value] = 1;
    }
  }
  free(ldst_bad);
  for (size_t v = 0; v < nv; ++v)
    bad[v] = !bad[v];
  return bad;
}

/*
 * A class-contract COPY of a stack allocation whose result only forms
 * load/store addresses. Folding it lets the encoder address the frame
 * directly instead of keeping (and reloading) a retagged base per use site.
 */
static bool nir_copy_addr_only_alloca(const nyr_func_t *f, const int *defs,
                                      const unsigned char *addr_only,
                                      const int *alias, const nyr_inst_t *in) {
  if (!addr_only || !defs || !in || in->dst < 0 || !addr_only[in->dst])
    return false;
  int s = nir_alias_find(alias, in->a);
  if (s < 0 || s >= f->next_value || defs[s] < 0)
    return false;
  return f->data[(size_t)defs[s]].op == NYIR_ALLOCA;
}

/*
 * Call-argument identity stays fixed unless the producer has a known
 * ownership fact that can refresh the value metadata after rewriting. Scalar
 * class contracts remain tied to the explicit copy and are handled below.
 */
static unsigned char *nir_call_arg_values(const nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return NULL;
  unsigned char *used = calloc((size_t)f->next_value, sizeof(*used));
  if (!used)
    return NULL;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_CALL)
      continue;
    const int ops[6] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t k = 0; k < 6; ++k)
      if (ops[k] >= 0 && ops[k] < f->next_value)
        used[ops[k]] = 1;
    for (size_t k = 0; k < in->extra_args_len; ++k)
      if (in->extra_args[k] >= 0 && in->extra_args[k] < f->next_value)
        used[in->extra_args[k]] = 1;
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0 &&
          in->phi_incoming[k].value < f->next_value)
        used[in->phi_incoming[k].value] = 1;
  }
  return used;
}

/*
 * Per-value scalar class of the defining instruction, including fixed-point
 * inference for untagged PHIs whose non-self incoming values all share one
 * exact raw numeric scalar class (i64-raw / f64 / f32). Refining those PHIs
 * lets copy propagation collapse redundant `[abi=i64-raw]` mov noise after
 * mem2reg without weakening ownership contracts on dynamic/object values.
 */
static nyr_scalar_class_t *nir_value_classes(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return NULL;
  nyr_scalar_class_t *vclass = calloc((size_t)f->next_value, sizeof(*vclass));
  if (!vclass)
    return NULL;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < f->next_value)
      vclass[in->dst] = in->scalar_class;
  }
  bool changed = true;
  for (int round = 0; changed && round < 8; ++round) {
    changed = false;
    for (size_t i = 0; i < f->len; ++i) {
      nyr_inst_t *in = &f->data[i];
      if (in->op != NYIR_PHI || in->dst < 0 || in->dst >= f->next_value ||
          vclass[in->dst] != NYIR_SCALAR_UNKNOWN || !in->phi_incoming ||
          in->phi_incoming_len == 0)
        continue;
      nyr_scalar_class_t joined = NYIR_SCALAR_UNKNOWN;
      bool valid = true;
      for (size_t k = 0; k < in->phi_incoming_len; ++k) {
        int iv = in->phi_incoming[k].value;
        if (iv == in->dst)
          continue;
        if (iv < 0 || iv >= f->next_value) {
          valid = false;
          break;
        }
        nyr_scalar_class_t ic = vclass[iv];
        if (ic != NYIR_SCALAR_I64_RAW && ic != NYIR_SCALAR_F64 &&
            ic != NYIR_SCALAR_F32) {
          valid = false;
          break;
        }
        if (joined == NYIR_SCALAR_UNKNOWN) {
          joined = ic;
        } else if (joined != ic) {
          valid = false;
          break;
        }
      }
      if (valid && joined != NYIR_SCALAR_UNKNOWN) {
        vclass[in->dst] = joined;
        in->scalar_class = joined;
        changed = true;
      }
    }
  }
  return vclass;
}

static void nir_call_refresh_argument_ownership(nyr_func_t *f, nyr_inst_t *in,
                                                const int *defs) {
  if (!f || !in || in->op != NYIR_CALL || !defs)
    return;
  int args[NYIR_CALL_MAX_ARGS];
  int argc = 0;
  if (!nyr_call_args(in, f->next_value, args, NYIR_CALL_MAX_ARGS, &argc, NULL,
                     0))
    return;
  for (int i = 0; i < argc; ++i) {
    int value = args[i];
    if (value < 0 || value >= f->next_value || defs[value] < 0)
      continue;
    uint8_t ownership = f->data[(size_t)defs[value]].semantic_ownership;
    if (ownership != NY_SEM_OWN_UNKNOWN)
      nyr_call_arg_ownership_set(in, (size_t)i, ownership);
  }
}

static int nir_alias_for_use(const int *alias, const size_t *alias_blk,
                             const nyr_cfg_t *cfg, int value, size_t use_blk) {
  if (value < 0)
    return value;
  int source = nir_alias_find(alias, value);
  /*
   * A copy already scanned in this block precedes the use: the walk is in
   * program order, so same-block availability only needs the block match.
   */
  if (source == value || !alias_blk || alias_blk[value] == SIZE_MAX ||
      alias_blk[value] == use_blk ||
      nyr_cfg_dominates(cfg, alias_blk[value], use_blk))
    return source;
  return value;
}

/*
 * Dominator-sound copy propagation for functions with control flow.
 *
 * alias[v]  = source value v may be replaced by, or -1.
 * alias_blk[v] = block of the COPY that established alias[v] (SIZE_MAX for
 *                entry values, which dominate every block).
 *
 * A use is only rewritten when the aliased source is provably available:
 *   - instruction operands: the copy's block dominates the use's block, or
 *     the use follows the copy in the same block;
 *   - PHI incoming values: the copy's block is the edge's predecessor or
 *     dominates it (the value flows from that predecessor).
 */
static bool copy_prop_cfg(nyr_func_t *f) {
  int *defs = nyr_build_defs(f);
  nyr_scalar_class_t *vclass = nir_value_classes(f);
  unsigned char *call_used = nir_call_arg_values(f);
  unsigned char *addr_only = nir_addr_only_values(f);
  nyr_cfg_t cfg = {0};
  if (!defs || !vclass || !call_used || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    free(vclass);
    free(call_used);
    free(addr_only);
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t nv = (size_t)f->next_value;
  int stk_alias[256];
  size_t stk_alias_blk[256];
  int *alias = nv <= 256 ? stk_alias : (int *)malloc(nv * sizeof(int));
  size_t *alias_blk =
      nv <= 256 ? stk_alias_blk : (size_t *)malloc(nv * sizeof(size_t));
  if (!alias || !alias_blk) {
    if (nv > 256) {
      free(alias);
      free(alias_blk);
    }
    free(defs);
    free(vclass);
    free(call_used);
    free(addr_only);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (int i = 0; i < f->next_value; ++i) {
    alias[i] = -1;
    alias_blk[i] = SIZE_MAX;
  }
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->dst < 0) {
      bool asm_goto = in->op == NYIR_INLINE_ASM &&
                      (in->imm == -14 || in->imm == -82 || in->imm == -83);
      /*
       * Branches and stores have no result, but their operands still obey
       * the same availability rule as result-producing instructions.  The
       * old unconditional rewrite could pull an alias established on one
       * branch into a non-dominated successor and change loop/control state.
       */
      size_t blk = cfg.inst_block[i];
      if (in->a >= 0)
        in->a = nir_alias_for_use(alias, alias_blk, &cfg, in->a, blk);
      if (in->b >= 0)
        in->b = nir_alias_for_use(alias, alias_blk, &cfg, in->b, blk);
      if (!asm_goto && in->c >= 0)
        in->c = nir_alias_for_use(alias, alias_blk, &cfg, in->c, blk);
      if (in->d >= 0)
        in->d = nir_alias_for_use(alias, alias_blk, &cfg, in->d, blk);
      if (in->e >= 0)
        in->e = nir_alias_for_use(alias, alias_blk, &cfg, in->e, blk);
      if (in->f >= 0)
        in->f = nir_alias_for_use(alias, alias_blk, &cfg, in->f, blk);
      for (size_t k = 0; k < in->extra_args_len; ++k)
        in->extra_args[k] =
            nir_alias_for_use(alias, alias_blk, &cfg, in->extra_args[k], blk);
      nir_call_refresh_argument_ownership(f, in, defs);
      continue;
    }
    size_t blk = cfg.inst_block[i];
    bool asm_goto = in->op == NYIR_INLINE_ASM &&
                    (in->imm == -14 || in->imm == -82 || in->imm == -83);
    /*
     * Fold operands whose aliased source is available here.
     */
    if (in->a >= 0) {
      int s = nir_alias_find(alias, in->a);
      if (s != in->a && (alias_blk[in->a] == SIZE_MAX ||
                         alias_blk[in->a] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->a], blk)))
        in->a = s;
    }
    if (in->b >= 0) {
      int s = nir_alias_find(alias, in->b);
      if (s != in->b && (alias_blk[in->b] == SIZE_MAX ||
                         alias_blk[in->b] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->b], blk)))
        in->b = s;
    }
    if (!asm_goto && in->c >= 0) {
      int s = nir_alias_find(alias, in->c);
      if (s != in->c && (alias_blk[in->c] == SIZE_MAX ||
                         alias_blk[in->c] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->c], blk)))
        in->c = s;
    }
    if (in->d >= 0) {
      int s = nir_alias_find(alias, in->d);
      if (s != in->d && (alias_blk[in->d] == SIZE_MAX ||
                         alias_blk[in->d] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->d], blk)))
        in->d = s;
    }
    if (in->e >= 0) {
      int s = nir_alias_find(alias, in->e);
      if (s != in->e && (alias_blk[in->e] == SIZE_MAX ||
                         alias_blk[in->e] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->e], blk)))
        in->e = s;
    }
    if (in->f >= 0) {
      int s = nir_alias_find(alias, in->f);
      if (s != in->f && (alias_blk[in->f] == SIZE_MAX ||
                         alias_blk[in->f] == blk ||
                         nyr_cfg_dominates(&cfg, alias_blk[in->f], blk)))
        in->f = s;
    }
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      int v = in->extra_args[k];
      if (v < 0)
        continue;
      int s = nir_alias_find(alias, v);
      if (s != v && (alias_blk[v] == SIZE_MAX || alias_blk[v] == blk ||
                     nyr_cfg_dominates(&cfg, alias_blk[v], blk)))
        in->extra_args[k] = s;
    }
    nir_call_refresh_argument_ownership(f, in, defs);
    /*
     * PHI incoming values are used on the predecessor edge, so the aliased
     * source must be available at the end of that predecessor block.
     */
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v < 0)
        continue;
      int s = nir_alias_find(alias, v);
      if (s == v)
        continue;
      size_t pred_blk = SIZE_MAX;
      for (size_t e = cfg.pred_offsets[blk]; e < cfg.pred_offsets[blk + 1];
           ++e) {
        size_t pred = cfg.pred_blocks[e];
        if (cfg.block_label[pred] == in->phi_incoming[k].predecessor_label) {
          pred_blk = pred;
          break;
        }
      }
      if (pred_blk == SIZE_MAX)
        continue;
      if (alias_blk[v] == SIZE_MAX ||
          nyr_cfg_dominates(&cfg, alias_blk[v], pred_blk) ||
          alias_blk[v] == pred_blk)
        in->phi_incoming[k].value = s;
    }

    if (in->op == NYIR_COPY && in->a >= 0 &&
        nir_copy_dst_calls_safe(f, defs, call_used, alias, in) &&
        (!nir_copy_has_class_contract(f, in) ||
         nir_copy_class_preserved(vclass, call_used, alias, in) ||
         nir_copy_addr_only_alloca(f, defs, addr_only, alias, in))) {
      int s = nir_alias_find(alias, in->a);
      bool safe = false;
      size_t src_blk = SIZE_MAX;
      if (defs[s] < 0) {
        safe = true; /* entry value: available everywhere */
      } else {
        src_blk = cfg.inst_block[(size_t)defs[s]];
        safe = nyr_cfg_dominates(&cfg, src_blk, blk);
      }
      if (safe) {
        alias[in->dst] = s;
        alias_blk[in->dst] = src_blk == SIZE_MAX ? SIZE_MAX : blk;
      } else {
        alias[in->dst] = -1;
        alias_blk[in->dst] = SIZE_MAX;
      }
    } else {
      if (in->op == NYIR_COPY && in->a >= 0 &&
          nir_copy_has_class_contract(f, in) &&
          ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event(
            "COPY_PROP",
            "v%d <- v%d retained: explicit scalar-class contract=%d", in->dst,
            in->a, in->scalar_class);
      alias[in->dst] = -1;
      alias_blk[in->dst] = SIZE_MAX;
    }
  }
  /*
   * A loop header's PHI precedes the copy in its latch, so the walk above saw
   * no alias for the back edge. Resolve PHI edges now that all copies are known.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op != NYIR_PHI || !cfg.reachable[cfg.inst_block[i]])
      continue;
    size_t blk = cfg.inst_block[i];
    for (size_t k = 0; k < in->phi_incoming_len; ++k) {
      int v = in->phi_incoming[k].value;
      if (v < 0)
        continue;
      int s = nir_alias_find(alias, v);
      if (s < 0 || s == v)
        continue;
      size_t pred_blk = SIZE_MAX;
      for (size_t e = cfg.pred_offsets[blk]; e < cfg.pred_offsets[blk + 1];
           ++e) {
        size_t pred = cfg.pred_blocks[e];
        if (cfg.block_label[pred] == in->phi_incoming[k].predecessor_label) {
          pred_blk = pred;
          break;
        }
      }
      if (pred_blk != SIZE_MAX &&
          (alias_blk[v] == SIZE_MAX || alias_blk[v] == pred_blk ||
           nyr_cfg_dominates(&cfg, alias_blk[v], pred_blk)))
        in->phi_incoming[k].value = s;
    }
  }
  if (nv > 256) {
    free(alias);
    free(alias_blk);
  }
  free(defs);
  free(vclass);
  free(call_used);
  free(addr_only);
  nyr_cfg_free(&cfg);
  return true;
}

bool nyr_copy_prop(nyr_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;
  /*
   * The linear alias map below is sound only for straight-line NYIR; for
   * control flow the dominator-checked walk above keeps every rewrite on a
   * path where the copied source is actually available.
   */
  if (nyr_has_control_flow(f))
    return copy_prop_cfg(f);
  size_t nv = (size_t)f->next_value;
  int stk_alias[256];
  int *alias = nv <= 256 ? stk_alias : (int *)malloc(nv * sizeof(int));
  nyr_scalar_class_t *vclass = nir_value_classes(f);
  unsigned char *call_used = nir_call_arg_values(f);
  int *defs = nyr_build_defs(f);
  unsigned char *addr_only = nir_addr_only_values(f);
  if (!alias || !vclass || !call_used || !defs) {
    if (nv > 256)
      free(alias);
    free(vclass);
    free(call_used);
    free(defs);
    free(addr_only);
    return false;
  }
  for (int i = 0; i < f->next_value; ++i)
    alias[i] = i;
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    bool asm_goto = in->op == NYIR_INLINE_ASM &&
                    (in->imm == -14 || in->imm == -82 || in->imm == -83);
    if (in->a >= 0)
      in->a = nir_alias_find(alias, in->a);
    if (in->b >= 0)
      in->b = nir_alias_find(alias, in->b);
    if (!asm_goto && in->c >= 0)
      in->c = nir_alias_find(alias, in->c);
    if (in->d >= 0)
      in->d = nir_alias_find(alias, in->d);
    if (in->e >= 0)
      in->e = nir_alias_find(alias, in->e);
    if (in->f >= 0)
      in->f = nir_alias_find(alias, in->f);
    for (size_t k = 0; k < in->extra_args_len; ++k) {
      if (in->extra_args[k] >= 0)
        in->extra_args[k] = nir_alias_find(alias, in->extra_args[k]);
    }
    for (size_t k = 0; k < in->phi_incoming_len; ++k)
      if (in->phi_incoming[k].value >= 0)
        in->phi_incoming[k].value =
            nir_alias_find(alias, in->phi_incoming[k].value);
    nir_call_refresh_argument_ownership(f, in, defs);
    if (in->dst >= 0) {
      if (in->op == NYIR_COPY && in->a >= 0 &&
          nir_copy_dst_calls_safe(f, defs, call_used, alias, in) &&
          (!nir_copy_has_class_contract(f, in) ||
           nir_copy_class_preserved(vclass, call_used, alias, in) ||
           nir_copy_addr_only_alloca(f, defs, addr_only, alias, in)))
        alias[in->dst] = nir_alias_find(alias, in->a);
      else {
        if (nir_copy_has_class_contract(f, in) &&
            ny_dbg_is_enabled(NY_DBG_HEURISTIC))
          ny_dbg_heuristic_event(
              "COPY_PROP",
              "v%d <- v%d retained: explicit scalar-class contract=%d", in->dst,
              in->a, in->scalar_class);
        alias[in->dst] = in->dst;
      }
    }
  }
  if (nv > 256)
    free(alias);
  free(vclass);
  free(call_used);
  free(defs);
  free(addr_only);
  return true;
}
