/*
 * Alias Analysis: flow-insensitive union-find points-to analysis.
 *
 * Assigns alias classes to memory-producing SSA values using a
 * union-find on must-alias equivalences. Each LOAD_I64/STORE_I64
 * uses the alias_class of its address operand; operations that
 * provably produce disjoint memory objects use distinct class roots.
 *
 * The pass annotates every instruction's alias_class field so that
 * downstream passes (LICM, store sinking, redundant load elim, DSE,
 * GVN-PRE) can perform alias disambiguation without re-running
 * expensive analysis.
 *
 * Alias class 0 is reserved for the "may-alias-anything" class.
 * Every distinct non-escaping local slot is assigned its own class.
 * Values derived from ADDR_LOCAL of different slots get different
 * classes. Values derived from ALLOCA get unique allocation classes.
 * Constants and arithmetic produce value-only results (class 0).
 *
 * References:
 *  Andersen — "Program Analysis and Specialization for the C
 *   Programming Language" (PhD thesis, DIKU 1994). Inclusion-based
 *   points-to analysis.
 *  Steensgaard — "Points-to Analysis in Almost Linear Time"
 *   (POPL 1996). Unification-based, near-linear alias analysis.
 *  Hardekopf & Lin — "The Ant and the Grasshopper: Fast and
 *   Accurate Points-to Analysis" (PLDI 2007). Hybrid approach.
 */
#include "code/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ------------------------------------------------------------------
 * Union-find with path compression and union-by-rank.
 * ------------------------------------------------------------------
 */

typedef struct {
  int *parent;
  int *rank;
  int  size;
} uf_t;

static bool uf_init(uf_t *uf, int n) {
  uf->parent = (int *)malloc((size_t)n * sizeof(int));
  uf->rank   = (int *)calloc((size_t)n, sizeof(int));
  if (!uf->parent || !uf->rank) {
    free(uf->parent);
    free(uf->rank);
    return false;
  }
  for (int i = 0; i < n; ++i)
    uf->parent[i] = i;
  uf->size = n;
  return true;
}

static void uf_free(uf_t *uf) {
  free(uf->parent);
  free(uf->rank);
  uf->parent = NULL;
  uf->rank   = NULL;
}

static int uf_find(uf_t *uf, int x) {
  while (uf->parent[x] != x) {
    uf->parent[x] = uf->parent[uf->parent[x]]; /* path halving */
    x = uf->parent[x];
  }
  return x;
}

static void uf_union(uf_t *uf, int a, int b) {
  a = uf_find(uf, a);
  b = uf_find(uf, b);
  if (a == b) return;
  if (uf->rank[a] < uf->rank[b]) { int t = a; a = b; b = t; }
  uf->parent[b] = a;
  if (uf->rank[a] == uf->rank[b]) uf->rank[a]++;
}

/*
 * ------------------------------------------------------------------
 * Alias class assignment:
 * 0            = unknown / may-alias-anything
 * 1 .. nslots  = local slot address class
 * nslots+1 ..  = allocation site class (one per ALLOCA/ADDR_SYMBOL)
 * ------------------------------------------------------------------
 */

static bool nyir_alias_trace_enabled(void) {
  return ny_trace_enabled("NY_TRACE_ALIAS");
}

static void nyir_alias_trace(const char *action, int value, uint32_t cls) {
  if (!nyir_alias_trace_enabled()) return;
  ny_trace_line("ALIAS", "alias: %s v%d -> class %u", action, value, cls);
}

/*
 * Main alias analysis pass: assigns alias_class to every NYIR value.
 *
 * Classes are propagated through COPY and arithmetic that derives
 * a pointer from a base (ADD_I64, SUB_I64) so that LICM and DSE
 * can see that "p + 8" aliases "p" when both come from the same
 * allocation root.
 */
bool nyir_alias_analysis(nyir_func_t *f) {
  if (!f || f->next_value <= 0)
    return true;

  int nvals  = f->next_value;
  int nslots = (int)nyir_max_local(f);

  /*
   * Each value gets a slot in the union-find. Values derived from
   * distinct non-escaping locals or distinct allocation sites are
   * placed in separate initial classes.
   *
   * Layout:
   *   [0 .. nvals-1]       SSA value slots
   *   [nvals .. nvals+nslots-1] local slot anchor nodes
   */
  int total = nvals + nslots + 1; /* +1 for unknown/may-alias */
  uf_t uf;
  if (!uf_init(&uf, total))
    return false;

  /*
   * Map: value -> initial alias_class seed before union-find.
   */
  uint32_t *seed = (uint32_t *)calloc((size_t)nvals, sizeof(uint32_t));
  if (!seed) {
    uf_free(&uf);
    return false;
  }

  /*
   * Escape analysis: locals whose address is taken or passed to calls
   * may be aliased through unknown pointers. Escaped locals are merged
   * into the unknown class (0).
   */
  bool *escaped = NULL;
  if (nslots > 0) {
    escaped = (bool *)calloc((size_t)nslots, sizeof(bool));
    if (!escaped) {
      free(seed);
      uf_free(&uf);
      return false;
    }
    for (size_t i = 0; i < f->len; ++i) {
      const nyir_inst_t *in = &f->data[i];
      if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
          (size_t)in->imm < (size_t)nslots) {
        /*
         * Check if this address is passed to any call or stored.
         */
        int addr_val = in->dst;
        for (size_t j = i + 1; j < f->len; ++j) {
          const nyir_inst_t *use = &f->data[j];
          bool used_as_arg = false;
          if (use->op == NYIR_CALL) {
            if (use->a == addr_val || use->b == addr_val ||
                use->c == addr_val || use->d == addr_val ||
                use->e == addr_val || use->f == addr_val)
              used_as_arg = true;
            if (!used_as_arg && use->extra_args) {
              for (size_t k = 0; k < use->extra_args_len; ++k)
                if (use->extra_args[k] == addr_val) { used_as_arg = true; break; }
            }
          }
          if (used_as_arg || use->op == NYIR_STORE_I64) {
            if (use->a == addr_val)
              escaped[in->imm] = true;
          }
        }
      }
    }
  }

  /*
   * First pass: assign initial classes.
   *   ADDR_LOCAL slot S → merged with anchor nvals + S (unless escaped)
   *   ALLOCA           → unique class = value index
   *   ADDR_SYMBOL      → unique class = value index
   *   COPY             → same class as source (union later)
   *   ADD/SUB_I64      → same class as base pointer operand (union later)
   *   others           → class 0 (unknown / scalar / non-pointer)
   */
  int next_unique = nvals + nslots; /* high water mark for fresh classes */

  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= nvals)
      continue;

    switch (in->op) {
    case NYIR_ADDR_LOCAL:
      if (in->imm >= 0 && (size_t)in->imm < (size_t)nslots &&
          !escaped[in->imm]) {
        /*
         * Merge this SSA value with the local-slot anchor.
         */
        int anchor = nvals + (int)in->imm;
        uf_union(&uf, in->dst, anchor);
        seed[in->dst] = (uint32_t)(in->imm + 1); /* 1-based local class */
      }
      /*
       * Escaped: stays class 0 (may-alias-anything).
       */
      break;

    case NYIR_ALLOCA:
      /*
       * Each allocation site is its own class root.
       */
      seed[in->dst] = (uint32_t)(++next_unique);
      break;

    case NYIR_ADDR_SYMBOL: {
      /*
       * Symbol addresses are allocation sites only when the symbols differ.
       * Two ADDR_SYMBOL instructions for the same global/string literal are
       * the same address.  Treating every instruction as a fresh allocation
       * lets store sinking move an initialization past a load from that
       * global, which is a miscompile at O3.
       */
      int same = -1;
      if (in->symbol) {
        for (size_t j = 0; j < i; ++j) {
          const nyir_inst_t *prior = &f->data[j];
          if (prior->op == NYIR_ADDR_SYMBOL && prior->dst >= 0 &&
              prior->symbol && strcmp(prior->symbol, in->symbol) == 0) {
            same = prior->dst;
            break;
          }
        }
      }
      if (same >= 0)
        uf_union(&uf, in->dst, same);
      seed[in->dst] = (uint32_t)(++next_unique);
      break;
    }

    case NYIR_COPY:
      if (in->a >= 0 && in->a < nvals)
        uf_union(&uf, in->dst, in->a);
      break;

    case NYIR_ADD_I64:
    case NYIR_SUB_I64:
      /*
       * Pointer arithmetic inherits the class of the base pointer
       * operand. Heuristic: use operand 'a' as the base.
       */
      if (in->a >= 0 && in->a < nvals)
        uf_union(&uf, in->dst, in->a);
      break;

    default:
      /*
       * Scalar ops, constants, float ops, etc.: class 0 (no alias).
       */
      break;
    }
  }

  /*
   * Second pass: assign final alias_class from union-find roots and
   * compact to a dense numbering by root index.
   *
   * Build a root → compact_class mapping.
   */
  int *root_to_class = (int *)calloc((size_t)total, sizeof(int));
  if (!root_to_class) {
    free(seed); free(escaped); uf_free(&uf);
    return false;
  }
  int next_class = 1; /* 0 = unknown */

  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if (in->dst < 0 || in->dst >= nvals)
      continue;

    /*
     * Pointer-class values have a non-zero seed or are in the same
     * UF set as a local anchor.
     */
    int root = uf_find(&uf, in->dst);
    bool is_local_anchor = (root >= nvals && root < nvals + nslots);
    bool has_seed = (in->dst < nvals && seed[in->dst] != 0);

    /*
     * Check if any value in the same class as this one is a pointer.
     * Simple heuristic: if root is an anchor or the value itself has
     * a non-zero seed, assign a stable class.
     */
    if (!is_local_anchor && !has_seed) {
      /*
       * Check root's class from a local anchor member.
       */
      for (int s = 0; s < nslots && !is_local_anchor; ++s) {
        if (uf_find(&uf, nvals + s) == root)
          is_local_anchor = true;
      }
    }

    uint32_t cls = 0;
    if (is_local_anchor || has_seed || seed[root < nvals ? root : 0] != 0) {
      if (root_to_class[root] == 0)
        root_to_class[root] = next_class++;
      cls = (uint32_t)root_to_class[root];
    }

    if (in->alias_class != cls) {
      nyir_alias_trace("assign", in->dst, cls);
      in->alias_class = cls;
    }
  }

  /*
   * Propagate alias_class to memory operations (LOAD_I64 / STORE_I64)
   * from their address operands, so downstream passes can compare
   * alias_class directly without re-querying the UF.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyir_inst_t *in = &f->data[i];
    if ((in->op == NYIR_LOAD_I64 || in->op == NYIR_STORE_I64 ||
         in->op == NYIR_VEC4_LOAD_F64 || in->op == NYIR_VEC4_STORE_F64 ||
         in->op == NYIR_VEC8_LOAD_F32 || in->op == NYIR_VEC8_STORE_F32) &&
        in->a >= 0 && in->a < nvals) {
      int root = uf_find(&uf, in->a);
      uint32_t cls = (root >= 0 && root < total && root_to_class[root])
                         ? (uint32_t)root_to_class[root]
                         : 0;
      if (in->alias_class != cls) {
        nyir_alias_trace("mem-annotate", in->a, cls);
        in->alias_class = cls;
      }
    }
  }

  free(root_to_class);
  free(seed);
  free(escaped);
  uf_free(&uf);
  return true;
}

/*
 * Alias-aware store sinking pass: sinks stores past non-aliasing loads
 * and independent operations using flow-insensitive points-to alias classes.
 */

bool nyir_sroa_unified(nyir_func_t *f);

static bool alias_sink_stores(nyir_func_t *f) {
  if (!f || f->len < 2 || f->next_value <= 0)
    return false;
  uint32_t *value_alias =
      (uint32_t *)calloc((size_t)f->next_value, sizeof(*value_alias));
  if (!value_alias)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyir_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < f->next_value)
      value_alias[in->dst] = in->alias_class;
  }

  bool changed = false;
  for (size_t i = 0; i + 1 < f->len; ++i) {
    nyir_inst_t *st = &f->data[i];
    if (st->op != NYIR_STORE_I64)
      continue;
    int addr = st->a;
    int val = st->c;
    if (addr < 0 || val < 0 || addr >= f->next_value)
      continue;
    uint32_t st_alias = value_alias[addr];
    if (!st_alias)
      continue;

    /*
     * Look ahead in the same basic block to see if store can sink past
     * non-aliasing loads/computations.
     */
    size_t target_k = SIZE_MAX;
    for (size_t k = i + 1; k < f->len; ++k) {
      nyir_inst_t *next = &f->data[k];
      if (next->op == NYIR_LABEL || next->op == NYIR_BR ||
          next->op == NYIR_BR_IF || next->op == NYIR_RET ||
          next->op == NYIR_CALL)
        break;
      if (next->op == NYIR_LOAD_I64) {
        int l_addr = next->a;
        if (l_addr >= 0 && l_addr < f->next_value) {
          uint32_t l_alias = value_alias[l_addr];
          if (l_alias == st_alias || l_alias == 0)
            break; /* Potential alias conflict */
        } else {
          break;
        }
      } else if (next->op == NYIR_STORE_I64) {
        int other_addr = next->a;
        if (other_addr >= 0 && other_addr < f->next_value) {
          uint32_t other_alias = value_alias[other_addr];
          if (other_alias == st_alias || other_alias == 0)
            break;
        } else {
          break;
        }
      }
      /*
       * If next instruction reads the value being stored, can't sink past
       */
      if (next->a == val || next->b == val || next->c == val || next->d == val)
        break;
      target_k = k;
    }
    if (target_k != SIZE_MAX && target_k > i) {
      nyir_inst_t tmp = *st;
      for (size_t m = i; m < target_k; ++m)
        f->data[m] = f->data[m + 1];
      f->data[target_k] = tmp;
      changed = true;
    }
  }
  free(value_alias);
  return changed;
}

bool nyir_alias_store_sink(nyir_func_t *f) {
  if (!f || f->len < 2)
    return true;

  (void)nyir_sroa_unified(f);
  (void)alias_sink_stores(f);
  return true;
}
