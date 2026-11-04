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
#include "code/backend/ir/internal.h"
#include "code/backend/ir/opt/util.h"
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

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_BR_IF && in->a >= 0 && known && known[in->a] &&
        value[in->a] == 0 &&
        !cfg_simplify_block_has_phi_cached(f, in->imm, &cache)) {
      /*
       * Removing a false edge preserves lexical fallthrough; the dead target
       * cannot carry PHIs because that would require predecessor repair.
       */
      nyr_debug_loc_t debug = in->debug;
      *in = (nyr_inst_t){.op = NYIR_NOP,
                         .dst = -1,
                         .a = -1,
                         .b = -1,
                         .c = -1,
                         .d = -1,
                         .e = -1,
                         .f = -1,
                         .debug = debug};
    }
    if (in->op == NYIR_BR_IF && in->a >= 0 && known && known[in->a] &&
        value[in->a] != 0) {
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

  free(known);
  free(value);
  cfg_simplify_cache_free(&cache);
  return true;
}
