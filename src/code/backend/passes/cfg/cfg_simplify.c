/*
 * CFG simplification: constant-branch folding, empty-block merging,
 * and trampoline branch threading (BR→LABEL→BR chains flattened,
 * 16-hop max). Runs to fixed point.
 *
 * References:
 *  Skiena — The Algorithm Design Manual, 3rd Ed., Ch.15 §15.1–15.3.
 *  Cooper & Torczon — Engineering a Compiler, 2nd Ed., Ch.9 §9.3.
 *  Cytron, Ferrante, Rosen, Wegman, Zadeck — "Efficiently Computing
 *   SSA Form and the Control Dependence Graph" (TOPLAS 1991).
 */
#include "base/common.h"
#include "base/compat.h"
#include "base/debug.h"
#include "code/backend/core/nyr.h"
#include "code/backend/passes/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool cfg_simplify_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_CFG_SIMPLIFY");
}

/*
 * True when the block labelled `label` contains PHIs.  Valid NYIR places
 * PHIs contiguously between the LABEL and the first real instruction, so
 * scanning until the first non-NOP instruction suffices.  Unknown labels
 * report "has phi" so callers stay on the safe side.
 * Memoized label→pc map + has_phi cache for the trampoline-threading loop
 * below. The old code scanned all f->len instructions per hop per branch
 * (O(br × n × hops)) plus another O(n) has_phi scan each — dominant cost on
 * straight-line monsters like the 500-diamond DCE bench.
 */
typedef struct {
  ptrdiff_t *label_pc; /* indexed by label, -1 = absent */
  int64_t max_label;
  int8_t *has_phi; /* -1 unknown, 0 no, 1 yes; parallel to label_pc */
  struct cfg_sparse_label_t {
    int64_t label;
    ptrdiff_t pc;
    int8_t has_phi;
  } *sparse;
  size_t sparse_len;
} cfg_simplify_cache_t;

static void cfg_simplify_cache_init(cfg_simplify_cache_t *c) {
  c->label_pc = NULL;
  c->max_label = -1;
  c->has_phi = NULL;
  c->sparse = NULL;
  c->sparse_len = 0;
}

static void cfg_simplify_cache_free(cfg_simplify_cache_t *c) {
  free(c->label_pc);
  free(c->has_phi);
  free(c->sparse);
  c->label_pc = NULL;
  c->has_phi = NULL;
  c->sparse = NULL;
  c->sparse_len = 0;
}

static int cfg_sparse_label_compare(const void *left, const void *right) {
  const struct cfg_sparse_label_t *a = left;
  const struct cfg_sparse_label_t *b = right;
  if (a->label != b->label)
    return a->label < b->label ? -1 : 1;
  if (a->pc != b->pc)
    return a->pc < b->pc ? -1 : 1;
  return 0;
}

static ptrdiff_t cfg_sparse_label_find(const cfg_simplify_cache_t *c,
                                       int64_t label) {
  size_t lo = 0;
  size_t hi = c->sparse_len;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (c->sparse[mid].label <= label)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0 || c->sparse[lo - 1].label != label)
    return -1;
  return (ptrdiff_t)(lo - 1);
}

static ptrdiff_t cfg_simplify_label_pc(const cfg_simplify_cache_t *c,
                                       int64_t label) {
  if (label < 0)
    return -1;
  if (c->label_pc && label <= c->max_label)
    return c->label_pc[label];
  ptrdiff_t index = cfg_sparse_label_find(c, label);
  return index >= 0 ? c->sparse[index].pc : -1;
}

static bool cfg_simplify_cache_build(const nyr_func_t *f,
                                     cfg_simplify_cache_t *c) {
  int64_t max_label = -1;
  size_t label_count = 0;
  for (size_t i = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0) {
      if (f->data[i].imm > max_label)
        max_label = f->data[i].imm;
      label_count++;
    } else if ((f->data[i].op == NYIR_BR || f->data[i].op == NYIR_BR_IF) &&
               f->data[i].imm > max_label) {
      max_label = f->data[i].imm;
    }
  size_t dense_limit =
      f->len <= (SIZE_MAX - 1024) / 4 ? f->len * 4 + 1024 : SIZE_MAX;
  if (max_label >= 0 && (uint64_t)max_label <= dense_limit) {
    size_t entries = 0;
    size_t pc_bytes = 0;
    size_t phi_bytes = 0;
    if (!ny_size_add_ok((size_t)max_label, 1, &entries) ||
        !ny_size_mul_ok(entries, sizeof(*c->label_pc), &pc_bytes) ||
        !ny_size_mul_ok(entries, sizeof(*c->has_phi), &phi_bytes))
      return false;
    c->label_pc = malloc(pc_bytes);
    c->has_phi = malloc(phi_bytes);
    if (!c->label_pc || !c->has_phi) {
      cfg_simplify_cache_free(c);
      return false;
    }
    for (size_t l = 0; l < entries; ++l) {
      c->label_pc[l] = -1;
      c->has_phi[l] = -1;
    }
    for (size_t i = 0; i < f->len; ++i)
      if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0 &&
          f->data[i].imm <= max_label)
        c->label_pc[f->data[i].imm] = (ptrdiff_t)i;
    c->max_label = max_label;
    return true;
  }
  if (label_count == 0)
    return true;
  size_t sparse_bytes = 0;
  if (!ny_size_mul_ok(label_count, sizeof(*c->sparse), &sparse_bytes))
    return false;
  c->sparse = malloc(sparse_bytes);
  if (!c->sparse)
    return false;
  for (size_t i = 0, index = 0; i < f->len; ++i)
    if (f->data[i].op == NYIR_LABEL && f->data[i].imm >= 0)
      c->sparse[index++] = (struct cfg_sparse_label_t){
          .label = f->data[i].imm, .pc = (ptrdiff_t)i, .has_phi = -1};
  c->sparse_len = label_count;
  qsort(c->sparse, c->sparse_len, sizeof(*c->sparse), cfg_sparse_label_compare);
  return true;
}

static bool cfg_simplify_block_has_phi_cached(const nyr_func_t *f,
                                              int64_t label,
                                              cfg_simplify_cache_t *c) {
  if (label < 0)
    return true;
  if (c->label_pc && label <= c->max_label) {
    if (c->has_phi[label] != -1)
      return c->has_phi[label] != 0;
    ptrdiff_t pc = c->label_pc[label];
    bool has = true;
    if (pc >= 0) {
      has = false;
      for (size_t k = (size_t)pc + 1; k < f->len; ++k) {
        if (f->data[k].op == NYIR_NOP)
          continue;
        has = (f->data[k].op == NYIR_PHI);
        break;
      }
    }
    c->has_phi[label] = has ? 1 : 0;
    return has;
  }
  ptrdiff_t sparse_index = cfg_sparse_label_find(c, label);
  if (sparse_index >= 0) {
    struct cfg_sparse_label_t *entry = &c->sparse[sparse_index];
    if (entry->has_phi != -1)
      return entry->has_phi != 0;
    bool has = false;
    for (size_t k = (size_t)entry->pc + 1; k < f->len; ++k) {
      if (f->data[k].op == NYIR_NOP)
        continue;
      has = f->data[k].op == NYIR_PHI;
      break;
    }
    entry->has_phi = has ? 1 : 0;
    return has;
  }
  /*
   * OOM fallback only: a missing cache entry must remain conservative.
   */
  size_t k = 0;
  while (k < f->len &&
         !(f->data[k].op == NYIR_LABEL && f->data[k].imm == label))
    ++k;
  if (k == f->len)
    return true;
  for (++k; k < f->len; ++k) {
    if (f->data[k].op == NYIR_NOP)
      continue;
    return f->data[k].op == NYIR_PHI;
  }
  return false;
}

static bool cfg_equal_scalar(const nyr_func_t *f, const int *defs, int a, int b,
                             unsigned depth) {
  if (a < 0 || b < 0 || a >= f->next_value || b >= f->next_value)
    return false;
  if (a == b)
    return true;
  if (depth == 8 || defs[a] < 0 || defs[b] < 0)
    return false;
  const nyr_inst_t *left = &f->data[defs[a]];
  const nyr_inst_t *right = &f->data[defs[b]];
  if (left->op == NYIR_COPY)
    return cfg_equal_scalar(f, defs, left->a, b, depth + 1);
  if (right->op == NYIR_COPY)
    return cfg_equal_scalar(f, defs, a, right->a, depth + 1);
  if (left->op != right->op || left->scalar_class != right->scalar_class ||
      left->flags != right->flags)
    return false;
  if (left->op == NYIR_CONST_I64)
    return left->imm == right->imm;
  if (left->op != NYIR_SHL_I64 && left->op != NYIR_SAR_I64 &&
      left->op != NYIR_AND_I64)
    return false;
  return cfg_equal_scalar(f, defs, left->a, right->a, depth + 1) &&
         cfg_equal_scalar(f, defs, left->b, right->b, depth + 1);
}

/*
 * A comparison on the same SSA value is redundant on an exclusive dominated
 * edge. Do not propagate the fact onto the comparison definition itself:
 * other uses may be reached without taking that edge. PHI repair remains
 * owned by the existing constant-branch folding below.
 */
static int8_t *cfg_dominated_conditions(const nyr_func_t *f) {
  size_t comparisons = 0;
  for (size_t i = 0; i < f->len; ++i)
    comparisons +=
        f->data[i].op == NYIR_CMP_I64 &&
        (f->data[i].cmp == NYIR_CMP_EQ || f->data[i].cmp == NYIR_CMP_NE);
  if (comparisons < 2)
    return NULL;
  int *defs = nyr_build_defs(f);
  int8_t *facts = malloc(f->len * sizeof(*facts));
  nyr_cfg_t cfg = {0};
  if (!defs || !facts || !nyr_cfg_build(f, &cfg)) {
    free(defs);
    free(facts);
    nyr_cfg_free(&cfg);
    return NULL;
  }
  memset(facts, -1, f->len * sizeof(*facts));
  for (size_t block = 0; block < cfg.block_count; ++block) {
    if (!cfg.reachable[block])
      continue;
    size_t end = nyr_block_last_non_nop(f, &cfg, block);
    if (end == cfg.block_start[block])
      continue;
    const nyr_inst_t *branch = &f->data[end - 1];
    if (branch->op != NYIR_BR_IF || branch->a < 0 ||
        branch->a >= f->next_value || defs[branch->a] < 0)
      continue;
    const nyr_inst_t *cmp = &f->data[defs[branch->a]];
    if (cmp->op != NYIR_CMP_I64 ||
        (cmp->cmp != NYIR_CMP_EQ && cmp->cmp != NYIR_CMP_NE))
      continue;
    int ancestor = cfg.idom[block];
    while (ancestor >= 0 && (size_t)ancestor != block) {
      size_t parent = (size_t)ancestor;
      size_t parent_end = nyr_block_last_non_nop(f, &cfg, parent);
      if (parent_end > cfg.block_start[parent]) {
        const nyr_inst_t *guard = &f->data[parent_end - 1];
        if (guard->op == NYIR_BR_IF && guard->a >= 0 &&
            guard->a < f->next_value && defs[guard->a] >= 0) {
          const nyr_inst_t *prior = &f->data[defs[guard->a]];
          size_t taken = 0;
          if (prior->op == NYIR_CMP_I64 &&
              (prior->cmp == NYIR_CMP_EQ || prior->cmp == NYIR_CMP_NE) &&
              ((cfg_equal_scalar(f, defs, cmp->a, prior->a, 0) &&
                cfg_equal_scalar(f, defs, cmp->b, prior->b, 0)) ||
               (cfg_equal_scalar(f, defs, cmp->a, prior->b, 0) &&
                cfg_equal_scalar(f, defs, cmp->b, prior->a, 0))) &&
              nyr_cfg_block_for_label(&cfg, guard->imm, &taken) &&
              parent + 1 < cfg.block_count && taken != parent + 1) {
            bool on_true =
                cfg.pred_offsets[taken + 1] - cfg.pred_offsets[taken] == 1 &&
                cfg.pred_blocks[cfg.pred_offsets[taken]] == parent &&
                nyr_cfg_dominates(&cfg, taken, block);
            size_t fallthrough = parent + 1;
            bool on_false =
                cfg.pred_offsets[fallthrough + 1] -
                        cfg.pred_offsets[fallthrough] ==
                    1 &&
                cfg.pred_blocks[cfg.pred_offsets[fallthrough]] == parent &&
                nyr_cfg_dominates(&cfg, fallthrough, block);
            if (on_true != on_false) {
              facts[end - 1] = (int8_t)(on_true == (cmp->cmp == prior->cmp));
              break;
            }
          }
        }
      }
      if (cfg.idom[parent] == ancestor)
        break;
      ancestor = cfg.idom[parent];
    }
  }
  free(defs);
  nyr_cfg_free(&cfg);
  return facts;
}

bool nyr_cfg_simplify(nyr_func_t *f) {
  if (!f)
    return true;

  bool *known = NULL;
  int64_t *value = NULL;
  if (f->next_value > 0) {
    known = (bool *)calloc((size_t)f->next_value, sizeof(bool));
    value = (int64_t *)calloc((size_t)f->next_value, sizeof(int64_t));
    if (!known || !value) {
      free(known);
      free(value);
      return false;
    }
    if (!nir_collect_consts(f, known, value)) {
      free(known);
      free(value);
      return false;
    }
  }

  cfg_simplify_cache_t cache;
  cfg_simplify_cache_init(&cache);
  /*
   * Best-effort: a sparse sorted index avoids allocations proportional to
   * label values; linear scans remain only as an allocation-failure fallback.
   */
  (void)cfg_simplify_cache_build(f, &cache);
  int8_t *dominated = cfg_dominated_conditions(f);

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    bool condition_known =
        in->op == NYIR_BR_IF && in->a >= 0 && known && known[in->a];
    int64_t condition = condition_known ? value[in->a] : 0;
    if (!condition_known && dominated && dominated[i] >= 0) {
      condition_known = true;
      condition = dominated[i];
    }
    if (in->op == NYIR_BR_IF && condition_known && condition == 0 &&
        !cfg_simplify_block_has_phi_cached(f, in->imm, &cache)) {
      /*
       * Removing a false edge preserves lexical fallthrough; the dead target
       * cannot carry PHIs because that would require predecessor repair.
       */
      nyr_debug_loc_t debug = in->debug;
      nyr_inst_discard(in);
      in->debug = debug;
    }
    if (in->op == NYIR_BR_IF && condition_known && condition != 0) {
      /*
       * A constant true condition keeps the explicit target edge and drops
       * the lexical fallthrough edge.  The target remains the same PHI
       * predecessor; only a PHI at the fallthrough block could need repair.
       * Most canonical conditionals put an unlabeled BR on that edge, where
       * PHIs cannot occur.  Keep labeled PHI fallthrough blocks conservative.
       */
      size_t fallthrough = nir_next_non_nop(f, i + 1);
      bool fallthrough_has_phi =
          fallthrough < f->len && f->data[fallthrough].op == NYIR_LABEL &&
          cfg_simplify_block_has_phi_cached(f, f->data[fallthrough].imm,
                                            &cache);
      if (!fallthrough_has_phi) {
        nyr_debug_loc_t debug = in->debug;
        *in = (nyr_inst_t){.op = NYIR_BR,
                           .dst = -1,
                           .a = -1,
                           .b = -1,
                           .c = -1,
                           .d = -1,
                           .e = -1,
                           .f = -1,
                           .imm = in->imm,
                           .debug = debug};
        if (cfg_simplify_trace_enabled())
          ny_dbg_heuristic_event("CFG_SIMPLIFY",
                                 "fold constant-true branch @%zu to label=%lld",
                                 i, (long long)in->imm);
      }
    }
    /*
     * Linear suffix deletion is intentionally omitted here.  A terminator's
     * following instructions can still belong to a fallthrough region after
     * block reordering; DCE handles unreachable value computations safely.
     */
  }

  /*
   * Trampoline branch threading: flatten chains of BR -> LABEL -> BR
   * so branch targets point directly to the final destination block.
   *
   * An edge retargeted past a trampoline changes the destination block's
   * predecessor set, which would leave PHI predecessor labels stale (the
   * same hazard that keeps nyr_jump_thread disabled).  Threading is
   * therefore skipped whenever the destination block contains PHIs.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR || in->op == NYIR_BR_IF) {
      int64_t target = in->imm;
      int64_t original_target = target;
      int hops = 0;
      for (int hop = 0; hop < 16; ++hop) {
        int64_t next_target = -1;
        ptrdiff_t found = cfg_simplify_label_pc(&cache, target);
        if (!cache.label_pc && !cache.sparse) {
          for (size_t k = 0; k < f->len; ++k)
            if (f->data[k].op == NYIR_LABEL && f->data[k].imm == target) {
              found = (ptrdiff_t)k;
              break;
            }
        }
        if (found >= 0) {
          size_t next = nir_next_non_nop(f, (size_t)found + 1);
          if (next < f->len && f->data[next].op == NYIR_BR &&
              f->data[next].imm != target) {
            if (!cfg_simplify_block_has_phi_cached(f, f->data[next].imm,
                                                   &cache))
              next_target = f->data[next].imm;
            else if (cfg_simplify_trace_enabled())
              ny_dbg_heuristic_event(
                  "CFG_SIMPLIFY",
                  "stop trampoline at label=%lld: final target label=%lld "
                  "has PHIs, so predecessor retargeting is unsafe",
                  (long long)target, (long long)f->data[next].imm);
          }
        }
        if (next_target >= 0 && next_target != target) {
          target = next_target;
          hops++;
        } else
          break;
      }
      in->imm = target;
      if (hops && cfg_simplify_trace_enabled())
        ny_dbg_heuristic_event("CFG_SIMPLIFY",
                               "thread branch @%zu label=%lld -> label=%lld "
                               "across %d trampoline(s)"
                               "%s",
                               i, (long long)original_target, (long long)target,
                               hops,
                               hops == 16 ? " (16-hop budget reached)" : "");
    }
  }

  /*
   * Merge single-predecessor fallthrough blocks (`BR L; LABEL L` where L has
   * no other predecessors and no PHIs). If-conversion leaves trivial
   * unconditional join blocks inside loop bodies; merging them collapses the
   * loop body back into a single block for vectorization and regalloc.
   */
  for (int pass = 0; pass < 8; ++pass) {
    nyr_cfg_t mcfg = {0};
    if (!nyr_cfg_build(f, &mcfg))
      break;
    bool merged = false;
    for (size_t b = 0; b + 1 < mcfg.block_count; ++b) {
      if (!mcfg.reachable[b])
        continue;
      size_t succ_n = mcfg.succ_offsets[b + 1] - mcfg.succ_offsets[b];
      if (succ_n != 1 || mcfg.succ_blocks[mcfg.succ_offsets[b]] != b + 1)
        continue;
      size_t s = b + 1;
      if (mcfg.pred_offsets[s + 1] - mcfg.pred_offsets[s] != 1 ||
          mcfg.pred_blocks[mcfg.pred_offsets[s]] != b)
        continue;
      int64_t s_label = mcfg.block_label[s];
      if (s_label < 0)
        continue;
      /* Target block must begin with LABEL s_label and contain no PHIs. */
      size_t s_start = mcfg.block_start[s];
      size_t s_end = mcfg.block_end[s];
      if (s_start >= s_end || f->data[s_start].op != NYIR_LABEL ||
          f->data[s_start].imm != s_label)
        continue;
      size_t s_first = nir_next_non_nop(f, s_start + 1);
      if (s_first < s_end && f->data[s_first].op == NYIR_PHI)
        continue;
      /* Check block b ends with an unconditional BR s_label. */
      size_t b_end = nyr_block_last_non_nop(f, &mcfg, b);
      if (b_end <= mcfg.block_start[b])
        continue;
      size_t br_pc = b_end - 1;
      if (f->data[br_pc].op != NYIR_BR || f->data[br_pc].imm != s_label)
        continue;
      /* Ensure no other BR/BR_IF anywhere targets s_label (even dead ones). */
      bool other_ref = false;
      bool phi_ref = false;
      for (size_t k = 0; k < f->len; ++k) {
        const nyr_inst_t *ki = &f->data[k];
        if (k != br_pc && (ki->op == NYIR_BR || ki->op == NYIR_BR_IF) &&
            ki->imm == s_label) {
          other_ref = true;
          break;
        }
        if (ki->op == NYIR_PHI && ki->phi_incoming) {
          for (size_t pi = 0; pi < ki->phi_incoming_len; ++pi)
            if (ki->phi_incoming[pi].predecessor_label == s_label) {
              phi_ref = true;
              break;
            }
        }
      }
      if (other_ref)
        continue;
      int64_t b_label = mcfg.block_label[b];
      if (phi_ref && b_label < 0)
        continue;
      if (phi_ref) {
        /* Refuse if any PHI referencing s_label already has b_label. */
        bool dup = false;
        for (size_t k = 0; k < f->len && !dup; ++k) {
          const nyr_inst_t *ki = &f->data[k];
          if (ki->op != NYIR_PHI || !ki->phi_incoming)
            continue;
          bool has_s = false, has_b = false;
          for (size_t pi = 0; pi < ki->phi_incoming_len; ++pi) {
            if (ki->phi_incoming[pi].predecessor_label == s_label)
              has_s = true;
            if (ki->phi_incoming[pi].predecessor_label == b_label)
              has_b = true;
          }
          if (has_s && has_b)
            dup = true;
        }
        if (dup)
          continue;
        for (size_t k = 0; k < f->len; ++k) {
          nyr_inst_t *ki = &f->data[k];
          if (ki->op != NYIR_PHI || !ki->phi_incoming)
            continue;
          for (size_t pi = 0; pi < ki->phi_incoming_len; ++pi)
            if (ki->phi_incoming[pi].predecessor_label == s_label)
              ki->phi_incoming[pi].predecessor_label = b_label;
        }
      }
      nyr_inst_discard(&f->data[br_pc]);
      nyr_inst_discard(&f->data[s_start]);
      merged = true;
      break;
    }
    nyr_cfg_free(&mcfg);
    if (!merged)
      break;
  }

  free(known);
  free(value);
  free(dominated);
  cfg_simplify_cache_free(&cache);
  return true;
}
