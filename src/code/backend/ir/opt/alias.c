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
#include "base/debug.h"
#include "code/backend/ir/opt/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Union-find with path compression and union-by-rank.
 */

typedef struct {
  int *parent;
  int *rank;
  int size;
} uf_t;

static bool uf_init(uf_t *uf, int n) {
  uf->parent = (int *)malloc((size_t)n * sizeof(int));
  uf->rank = (int *)calloc((size_t)n, sizeof(int));
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
  uf->rank = NULL;
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
  if (a == b)
    return;
  if (uf->rank[a] < uf->rank[b]) {
    int t = a;
    a = b;
    b = t;
  }
  uf->parent[b] = a;
  if (uf->rank[a] == uf->rank[b])
    uf->rank[a]++;
}

/*
 * Alias class assignment:
 * 0            = unknown / may-alias-anything
 * 1 .. nslots  = local slot address class
 * nslots+1 ..  = allocation site class (one per ALLOCA/ADDR_SYMBOL)
 */

static bool nyr_alias_trace_enabled(void) {
  return ny_dbg_is_enabled(NY_DBG_HEURISTIC) ||
         ny_trace_enabled("NY_TRACE_ALIAS");
}

static void nyr_alias_trace(const char *action, int value, uint32_t cls) {
  if (!nyr_alias_trace_enabled())
    return;
  if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
    ny_dbg_heuristic_event("ALIAS", "%s v%d -> class %u", action, value, cls);
  else
    ny_trace_line("ALIAS", "alias: %s v%d -> class %u", action, value, cls);
}

static bool nyr_op_has_single_memory_address(nyr_op_t op) {
  switch (op) {
  case NYIR_LOAD_I64:
  case NYIR_STORE_I64:
  case NYIR_VEC4_LOAD_F64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_LOAD_F32:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_LOAD_I64:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_LOAD_I64:
  case NYIR_VEC8_STORE_I64:
    return true;
  default:
    return false;
  }
}

static bool nyr_op_writes_through_first_operand(nyr_op_t op) {
  switch (op) {
  case NYIR_STORE_I64:
  case NYIR_VEC4_STORE_F64:
  case NYIR_VEC8_STORE_F32:
  case NYIR_VEC4_STORE_I64:
  case NYIR_VEC8_STORE_I64:
    return true;
  default:
    return false;
  }
}

/*
 * Main alias analysis pass: assigns alias_class to every NYIR value.
 *
 * Classes are propagated through COPY and arithmetic that derives
 * a pointer from a base (ADD_I64, SUB_I64) so that LICM and DSE
 * can see that "p + 8" aliases "p" when both come from the same
 * allocation root.
 */
bool nyr_alias_analysis(nyr_func_t *f) {
  if (!f)
    return true;
  if (f->len && !f->data)
    return false;
  /*
   * Alias classes are derived facts, not sticky annotations. Clear the prior
   * generation before computing this one so changed or newly unknown address
   * operands cannot retain a stale nonzero class.
   */
  for (size_t i = 0; i < f->len; ++i)
    f->data[i].alias_class = 0;
  if (f->next_value <= 0)
    return true;

  int nvals = f->next_value;
  int nslots = (int)nyr_max_local(f);

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
   * Escape analysis: locals whose address reaches a call, return, pointer
   * store, or unknown write sink may be reached through an unknown alias.
   * Escaped locals are merged into the unknown class (0).
   */
  bool *escaped = NULL;
  if (nslots > 0) {
    escaped = (bool *)calloc((size_t)nslots, sizeof(bool));
    bool *escape_values = (bool *)calloc((size_t)nvals, sizeof(bool));
    if (!escaped || !escape_values) {
      free(escaped);
      free(escape_values);
      free(seed);
      uf_free(&uf);
      return false;
    }
    bool malformed_call_args = false;
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_CALL) {
        const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
        for (size_t k = 0; k < sizeof(args) / sizeof(args[0]); ++k)
          if (args[k] >= 0 && args[k] < nvals)
            escape_values[args[k]] = true;
        if (in->extra_args_len && !in->extra_args) {
          malformed_call_args = true;
        } else {
          for (size_t k = 0; k < in->extra_args_len; ++k) {
            int arg = in->extra_args[k];
            if (arg >= 0 && arg < nvals)
              escape_values[arg] = true;
          }
        }
      }
      if (nyr_op_writes_through_first_operand(in->op) && in->a >= 0 &&
          in->a < nvals)
        escape_values[in->a] = true;
      /*
       * A pointer value can leave the current activation through either a
       * return or a memory store. Track the stored value separately from the
       * destination address: the latter is an access, while the former may
       * become reachable by an unknown caller or alias.
       */
      if (in->op == NYIR_STORE_I64 && in->c >= 0 && in->c < nvals)
        escape_values[in->c] = true;
      if (in->op == NYIR_RET && in->a >= 0 && in->a < nvals)
        escape_values[in->a] = true;
    }
    /*
     * Propagate escape requirements backward through the same pointer
     * preserving operations used by the alias-class analysis. NYIR SSA
     * definitions precede ordinary uses, so one reverse scan reaches the
     * complete COPY/ADD/SUB chain in linear time. Loads, PHIs, and call
     * results deliberately do not inherit source provenance.
     */
    for (size_t i = f->len; i > 0; --i) {
      const nyr_inst_t *in = &f->data[i - 1];
      if (in->dst < 0 || in->dst >= nvals || !escape_values[in->dst])
        continue;
      if ((in->op == NYIR_COPY || in->op == NYIR_ADD_I64 ||
           in->op == NYIR_SUB_I64) &&
          in->a >= 0 && in->a < nvals)
        escape_values[in->a] = true;
    }
    if (malformed_call_args)
      memset(escaped, 1, (size_t)nslots * sizeof(*escaped));
    for (size_t i = 0; i < f->len; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (in->op == NYIR_ADDR_LOCAL && in->imm >= 0 &&
          (size_t)in->imm < (size_t)nslots && in->dst >= 0 && in->dst < nvals &&
          escape_values[in->dst])
        escaped[in->imm] = true;
    }
    free(escape_values);
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
    const nyr_inst_t *in = &f->data[i];
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
          const nyr_inst_t *prior = &f->data[j];
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
    free(seed);
    free(escaped);
    uf_free(&uf);
    return false;
  }
  int next_class = 1; /* 0 = unknown */

  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
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
      nyr_alias_trace("assign", in->dst, cls);
      in->alias_class = cls;
    }
  }

  /*
   * Propagate alias_class to memory operations (LOAD_I64 / STORE_I64)
   * from their address operands, so downstream passes can compare
   * alias_class directly without re-querying the UF.
   */
  for (size_t i = 0; i < f->len; ++i) {
    nyr_inst_t *in = &f->data[i];
    if (nyr_op_has_single_memory_address(in->op) && in->a >= 0 &&
        in->a < nvals) {
      int root = uf_find(&uf, in->a);
      uint32_t cls = (root >= 0 && root < total && root_to_class[root])
                         ? (uint32_t)root_to_class[root]
                         : 0;
      if (in->alias_class != cls) {
        nyr_alias_trace("mem-annotate", in->a, cls);
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

bool nyr_sroa_unified(nyr_func_t *f);

static bool alias_sink_stores(nyr_func_t *f) {
  if (!f || f->len < 2 || f->next_value <= 0)
    return false;
  uint32_t *value_alias =
      (uint32_t *)calloc((size_t)f->next_value, sizeof(*value_alias));
  if (!value_alias)
    return false;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->dst >= 0 && in->dst < f->next_value)
      value_alias[in->dst] = in->alias_class;
  }

  bool changed = false;
  for (size_t i = 0; i + 1 < f->len; ++i) {
    nyr_inst_t *st = &f->data[i];
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
      nyr_inst_t *next = &f->data[k];
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
      nyr_inst_t tmp = *st;
      for (size_t m = i; m < target_k; ++m)
        f->data[m] = f->data[m + 1];
      f->data[target_k] = tmp;
      changed = true;
    }
  }
  free(value_alias);
  return changed;
}

bool nyr_alias_store_sink(nyr_func_t *f) {
  if (!f || f->len < 2)
    return true;

  (void)nyr_sroa_unified(f);
  (void)alias_sink_stores(f);
  return true;
}
