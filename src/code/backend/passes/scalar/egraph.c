/*
 * Equality-saturation e-graph over NYIR pure scalar integer operations.
 *
 * A real e-class / e-node implementation (egg-style):
 *
 *   - every value id used by the current basic block maps to an e-node, so a
 *     whole block's pure arithmetic is canonicalized together;
 *   - a declarative algebraic rule pool is applied to saturation (bounded by
 *     iterations), so a rewrite that enables another one happens in the pass;
 *   - cost-directed extraction chooses the cheapest realizable member of each
 *     e-class and rewrites the block instructions in place.
 *
 * Correctness / integration contract:
 *   - Only pure integer ALU/COPY nodes participate.  Memory, calls, traps,
 *     control flow, PHIs, floats and locals are leaves.
 *   - Extraction only references values materialized earlier in the same block
 *     (or block-entry leaves/constants), so SSA dominance is preserved.
 *   - The function is rebuilt on a scratch buffer and committed only after
 *     `nyr_ir_verify` accepts it; otherwise the original is returned unchanged.
 *   - A value is rewritten only when its extracted form is strictly cheaper
 *     than the original; rewritten instructions carry no semantic flags.
 */
#include "base/compat.h"
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/passes/verify/loop.h"
#include "code/backend/passes/util.h"
#include <limits.h>
#include <nyr/ir.h>
#include <stdlib.h>
#include <string.h>

#define NY_EG_LEAF (-1)
#define NY_EG_MAX_ITERS 48
#define NY_EG_NODE_FUEL 2048

/*
 * Statistics only; functions are optimized concurrently.
 */
static _Atomic unsigned long long ny_eg_runs;
static _Atomic unsigned long long ny_eg_fuel_exhausted;

void nyr_egraph_stats(unsigned long long *runs,
                       unsigned long long *fuel_exhausted) {
  if (runs)
    *runs = ny_eg_runs;
  if (fuel_exhausted)
    *fuel_exhausted = ny_eg_fuel_exhausted;
}

typedef struct {
  int op; /* nyr_op_t, or NY_EG_LEAF */
  int64_t imm;
  int nchild; /* 0 | 1 | 2 */
  int child0; /* canonical e-class handles (node indices), or -1 */
  int child1;
} ny_eg_node_t;

typedef struct {
  ny_eg_node_t *nodes;
  int n, cap;
  int *uf;
  /*
   * Union-by-rank.  Every rebuild canonicalizes all child pointers, so long
   * parent chains would be walked repeatedly; rank keeps them shallow.
   */
  unsigned char *rank;
  /*
   * Open-addressing table of canonical node indices (-1 empty).  Interning
   * used to scan every node, which made saturation quadratic in the block
   * size before any rule fired; the table makes it constant time and is
   * rebuilt whenever unions move canonical children.
   */
  int *hash;
  int hash_cap;
} ny_egraph_t;

static void eg_free_graph(ny_egraph_t *g) {
  if (!g)
    return;
  free(g->nodes);
  free(g->uf);
  free(g->rank);
  free(g->hash);
  g->nodes = NULL;
  g->uf = NULL;
  g->rank = NULL;
  g->hash = NULL;
  g->hash_cap = 0;
  g->n = 0;
  g->cap = 0;
}

static int eg_find(ny_egraph_t *g, int n) {
  int root = n;
  while (g->uf[root] != root)
    root = g->uf[root];
  while (g->uf[n] != n) {
    int next = g->uf[n];
    g->uf[n] = root;
    n = next;
  }
  return root;
}

static void eg_union(ny_egraph_t *g, int a, int b) {
  a = eg_find(g, a);
  b = eg_find(g, b);
  if (a == b)
    return;
  if (g->rank[a] < g->rank[b]) {
    g->uf[a] = b;
  } else if (g->rank[a] > g->rank[b]) {
    g->uf[b] = a;
  } else {
    g->uf[b] = a;
    if (g->rank[a] < 255)
      ++g->rank[a];
  }
}

static int eg_new(ny_egraph_t *g, int op, int64_t imm, int nchild, int c0,
                  int c1) {
  if (g->n >= NY_EG_NODE_FUEL)
    return -1;
  if (g->n == g->cap) {
    int nc = g->cap ? g->cap * 2 : 64;
    ny_eg_node_t *nn = ny_realloc_array(g->nodes, (size_t)nc, sizeof(*nn));
    if (!nn)
      return -1;
    g->nodes = nn;
    int *nu = ny_realloc_array(g->uf, (size_t)nc, sizeof(*nu));
    if (!nu)
      return -1;
    g->uf = nu;
    unsigned char *nr = ny_realloc_array(g->rank, (size_t)nc, sizeof(*nr));
    if (!nr)
      return -1;
    g->rank = nr;
    memset(g->rank + g->cap, 0, (size_t)(nc - g->cap));
    /*
     * Only commit the new capacity once every array grew; growing
     * `nodes` alone left cap stale and made the failing insert look
     * like fuel exhaustion on the next call.
     */
    g->cap = nc;
  }
  int i = g->n++;
  g->nodes[i].op = op;
  g->nodes[i].imm = imm;
  g->nodes[i].nchild = nchild;
  g->nodes[i].child0 = c0;
  g->nodes[i].child1 = c1;
  g->uf[i] = i;
  return i;
}

static uint64_t eg_hash_mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}

static uint64_t eg_node_hash(int op, int64_t imm, int nchild, int c0, int c1) {
  uint64_t h = eg_hash_mix((uint64_t)(uint32_t)op ^ (uint64_t)imm);
  h = eg_hash_mix(h ^ ((uint64_t)(uint32_t)(c0 + 1) * 3u));
  h = eg_hash_mix(h ^ ((uint64_t)(uint32_t)(c1 + 1) * 17u));
  return h ^ ((uint64_t)nchild << 59);
}

static bool eg_hash_reset(ny_egraph_t *g, int want) {
  int cap = 16;
  while (cap < want && cap < (1 << 24))
    cap <<= 1;
  if (g->hash_cap != cap) {
    int *h = ny_realloc_array(g->hash, (size_t)cap, sizeof(*h));
    if (!h)
      return false;
    g->hash = h;
    g->hash_cap = cap;
  }
  for (int i = 0; i < g->hash_cap; ++i)
    g->hash[i] = -1;
  return true;
}

/*
 * Find the canonical node with this key.  Returns its index, or -1 with *slot
 * set to the probe position where a node with this key belongs.
 */
static int eg_hash_probe(const ny_egraph_t *g, int op, int64_t imm, int nchild,
                         int c0, int c1, int *slot) {
  if (!g->hash_cap)
    return -1;
  int mask = g->hash_cap - 1;
  int i = (int)(eg_node_hash(op, imm, nchild, c0, c1) & (uint64_t)mask);
  for (int probes = 0; probes < g->hash_cap; ++probes) {
    int at = g->hash[i];
    if (at < 0) {
      if (slot)
        *slot = i;
      return -1;
    }
    const ny_eg_node_t *n = &g->nodes[at];
    int n0 = n->nchild > 0 ? n->child0 : -1;
    int n1 = n->nchild > 1 ? n->child1 : -1;
    if (n->op == op && n->imm == imm && n->nchild == nchild && n0 == c0 &&
        n1 == c1) {
      if (slot)
        *slot = i;
      return at;
    }
    i = (i + 1) & mask;
  }
  return -1;
}

/*
 * Insert a canonical node unless an equal one is already present.  Returns the
 * equal node's index or -1 when this node was stored.
 */
static int eg_hash_store(ny_egraph_t *g, int node) {
  const ny_eg_node_t *n = &g->nodes[node];
  int slot = -1;
  int found = eg_hash_probe(g, n->op, n->imm, n->nchild, n->child0, n->child1,
                            &slot);
  if (found >= 0)
    return found;
  if (slot >= 0)
    g->hash[slot] = node;
  return -1;
}

/*
 * Rebuild the table from the current canonical nodes, re-canonicalizing every
 * child pointer on the way.  Any table mapping the stale index is discarded.
 */
static bool eg_hash_rehash(ny_egraph_t *g, int want) {
  if (!eg_hash_reset(g, want))
    return false;
  for (int i = 0; i < g->n; ++i) {
    if (eg_find(g, i) != i)
      continue;
    ny_eg_node_t *n = &g->nodes[i];
    if (n->nchild > 0)
      n->child0 = eg_find(g, n->child0);
    if (n->nchild > 1)
      n->child1 = eg_find(g, n->child1);
    eg_hash_store(g, i);
  }
  return true;
}

static int eg_intern(ny_egraph_t *g, int op, int64_t imm, int nchild, int c0,
                     int c1) {
  if (nchild > 0)
    c0 = eg_find(g, c0);
  if (nchild > 1)
    c1 = eg_find(g, c1);
  int slot = -1;
  int found = eg_hash_probe(g, op, imm, nchild, c0, c1, &slot);
  if (found >= 0)
    return found;
  int node = eg_new(g, op, imm, nchild, c0, c1);
  if (node < 0)
    return -1;
  if (slot < 0) {
    /*
     * Table saturated (or never built): rebuild it, then re-probe.
     */
    if (!eg_hash_rehash(g, g->n * 4 + 32))
      return -1;
    if (eg_hash_probe(g, op, imm, nchild, c0, c1, &slot) >= 0 || slot < 0)
      return node;
  }
  g->hash[slot] = node;
  return node;
}

/*
 * Batched congruence closure (egg-style rebuild): canonicalize every root's
 * children, then merge roots that have become structurally identical.  The
 * old saturation loop compared all pairs of nodes on every iteration, which is
 * quadratic in the block size; this is linear per round and additionally
 * catches equalities that only become visible after an earlier merge.
 *
 * Returns the number of merges, or -1 when the table cannot be allocated.
 * A merge made late in a round can leave other table entries stale; those are
 * re-derived on the next round (or next iteration), so a stale entry can only
 * delay a merge, never invent one.
 */
static int eg_rebuild(ny_egraph_t *g) {
  int merges = 0;
  for (int round = 0; round < 8; ++round) {
    if (!eg_hash_rehash(g, g->n * 4 + 32))
      return -1;
    int local = 0;
    for (int i = 0; i < g->n; ++i) {
      if (eg_find(g, i) != i)
        continue;
      ny_eg_node_t *n = &g->nodes[i];
      int c0 = n->nchild > 0 ? eg_find(g, n->child0) : -1;
      int c1 = n->nchild > 1 ? eg_find(g, n->child1) : -1;
      n->child0 = c0;
      n->child1 = c1;
      int other = eg_hash_store(g, i);
      if (other >= 0 && eg_find(g, other) != eg_find(g, i)) {
        eg_union(g, i, other);
        ++local;
      }
    }
    merges += local;
    if (!local)
      break;
  }
  return merges;
}

static int eg_class_for_leaf(ny_egraph_t *g, int value_id) {
  return eg_find(g, eg_intern(g, NY_EG_LEAF, value_id, 0, -1, -1));
}

static int eg_class_for_const(ny_egraph_t *g, int64_t imm) {
  return eg_find(g, eg_intern(g, NYIR_CONST_I64, imm, 0, -1, -1));
}

static bool eg_class_has_const(const ny_egraph_t *g, int c, int64_t imm) {
  for (int i = 0; i < g->n; ++i) {
    if (eg_find((ny_egraph_t *)g, i) != eg_find((ny_egraph_t *)g, c))
      continue;
    if (g->nodes[i].op == NYIR_CONST_I64 && g->nodes[i].imm == imm)
      return true;
  }
  return false;
}

static int64_t eg_class_first_const(const ny_egraph_t *g, int c) {
  for (int i = 0; i < g->n; ++i) {
    if (eg_find((ny_egraph_t *)g, i) != eg_find((ny_egraph_t *)g, c))
      continue;
    if (g->nodes[i].op == NYIR_CONST_I64)
      return g->nodes[i].imm;
  }
  return 0;
}

/*
 * Known non-negativity of e-classes.  A class is non-negative when one of its
 * members provably is: the e-graph only equates values that really are equal,
 * so a proven member transfers its fact to every form in the class.  `and`
 * with a non-negative constant is the case that matters for type-punning
 * code; COPY forwards the fact, constants carry it directly.
 *
 * `nn` is indexed by node and only ever set on canonical roots; the result is
 * a fixpoint because a class can gain a proven member after another class
 * becomes non-negative.
 */
static void eg_nonneg_roots(ny_egraph_t *g, bool *nn) {
  for (int i = 0; i < g->n; ++i)
    nn[i] = false;
  for (int round = 0; round <= g->n; ++round) {
    bool changed = false;
    for (int i = 0; i < g->n; ++i) {
      if (nn[i] || eg_find(g, i) != i)
        continue;
      const ny_eg_node_t *n = &g->nodes[i];
      bool value = false;
      if (n->op == NYIR_CONST_I64)
        value = n->imm >= 0;
      else if (n->op == NYIR_AND_I64 && n->nchild == 2)
        value = nn[eg_find(g, n->child0)] || nn[eg_find(g, n->child1)];
      else if (n->op == NYIR_COPY && n->nchild == 1)
        value = nn[eg_find(g, n->child0)];
      if (value) {
        nn[i] = true;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
}

static bool eg_class_nonneg(ny_egraph_t *g, const bool *nn, int c) {
  int canon = eg_find(g, c);
  for (int i = 0; i < g->n; ++i)
    if (nn[i] && eg_find(g, i) == canon)
      return true;
  return false;
}

static bool eg_class_const_value(const ny_egraph_t *g, int c, int64_t *out) {
  int canon = eg_find((ny_egraph_t *)g, c);
  for (int i = 0; i < g->n; ++i) {
    if (eg_find((ny_egraph_t *)g, i) != canon)
      continue;
    if (g->nodes[i].op == NYIR_CONST_I64) {
      *out = g->nodes[i].imm;
      return true;
    }
  }
  return false;
}

/*
 * Divisor of the form 2^k with k in [1, 62].
 */
static bool eg_pow2_shift(int64_t divisor, int *shift) {
  if (divisor <= 0 || (divisor & (divisor - 1)) != 0)
    return false;
  int k = 0;
  for (int64_t v = divisor; v > 1; v >>= 1)
    ++k;
  if (k < 1 || k > 62)
    return false;
  *shift = k;
  return true;
}

static bool eg_facts_enabled(void) {
  const char *env = getenv("NY_EGRAPH_FACTS");
  if (!env || !*env)
    return true;
  if (*env == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0)
    return false;
  return true;
}

/*
 * Analysis-guarded refinement rules.  These are NOT equalities in general:
 * NYIR division truncates toward zero while SAR rounds toward negative
 * infinity, so `x / 2^k` equals `x >> k` only for x >= 0, and `x % 2^k`
 * equals `x & (2^k - 1)` only for x >= 0.  The analysis fact is the guard, so
 * these rules run only after the non-negativity fixpoint and never when the
 * operand's sign is unknown.
 *
 * The divisor is a nonzero constant, so neither side traps where the other
 * does: the rewrite removes no fault and adds none.
 *
 * Returns the number of unions applied, or -1 when the graph is out of fuel.
 */
static int eg_apply_fact_rules(ny_egraph_t *g, const bool *nn) {
  int unions = 0;
  for (int i = 0; i < g->n; ++i) {
    if (eg_find(g, i) != i)
      continue;
    const ny_eg_node_t *n = &g->nodes[i];
    if ((n->op != NYIR_DIV_I64 && n->op != NYIR_MOD_I64) || n->nchild != 2)
      continue;
    int64_t divisor = 0;
    if (!eg_class_const_value(g, n->child1, &divisor) || divisor <= 0 ||
        (divisor & (divisor - 1)) != 0)
      continue;
    int shift = 0;
    for (int64_t v = divisor; v > 1; v >>= 1)
      ++shift;
    if (shift < 1 || shift > 62)
      continue;
    int xc = eg_find(g, n->child0);
    if (!eg_class_nonneg(g, nn, xc))
      continue;
    int rhs = n->op == NYIR_DIV_I64
                  ? eg_intern(g, NYIR_SAR_I64, 0, 2, xc,
                              eg_class_for_const(g, shift))
                  : eg_intern(g, NYIR_AND_I64, 0, 2, xc,
                              eg_class_for_const(g, divisor - 1));
    if (rhs < 0)
      return -1;
    if (eg_find(g, rhs) != i) {
      eg_union(g, i, rhs);
      ++unions;
    }
  }
  return unions;
}

/*
 * Rule pool
 */

enum { EG_RH_LIT = 1, EG_RH_COPY, EG_RH_OP };

typedef struct {
  nyr_op_t lhs;
  int c0_kind; /* 0 = var, 1 = const */
  int64_t c0_imm;
  int c1_kind;
  int64_t c1_imm;
  int same; /* both children same class */
  int rhs_kind;
  nyr_op_t rhs_op;
  int rhs_child;
  int64_t rhs_imm;
} ny_eg_rule_t;

static const ny_eg_rule_t ny_eg_rules[] = {
    /*
     * add: x+0 -> x (both orders); x+x -> shl(x,1)
     */
    {NYIR_ADD_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_ADD_I64, 1, 0, 0, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 1, 0},
    {NYIR_ADD_I64, 0, 0, 0, 0, 1, EG_RH_OP, NYIR_SHL_I64, 0, 1},
    /*
     * sub: x-0 -> x ; x-x -> 0
     */
    {NYIR_SUB_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_SUB_I64, 0, 0, 0, 0, 1, EG_RH_LIT, NYIR_OP_COUNT, 0, 0},
    /*
     * mul: x*1 -> x ; x*0 -> 0 (both orders)
     */
    {NYIR_MUL_I64, 0, 0, 1, 1, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_MUL_I64, 1, 1, 0, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 1, 0},
    {NYIR_MUL_I64, 0, 0, 1, 0, 0, EG_RH_LIT, NYIR_OP_COUNT, 0, 0},
    {NYIR_MUL_I64, 1, 0, 0, 0, 0, EG_RH_LIT, NYIR_OP_COUNT, 1, 0},
    /*
     * strength
     */
    {NYIR_MUL_I64, 0, 0, 1, 2, 0, EG_RH_OP, NYIR_SHL_I64, 0, 1},
    {NYIR_MUL_I64, 1, 2, 0, 0, 0, EG_RH_OP, NYIR_SHL_I64, 1, 1},
    {NYIR_MUL_I64, 0, 0, 1, 4, 0, EG_RH_OP, NYIR_SHL_I64, 0, 2},
    {NYIR_MUL_I64, 1, 4, 0, 0, 0, EG_RH_OP, NYIR_SHL_I64, 1, 2},
    {NYIR_MUL_I64, 0, 0, 1, 8, 0, EG_RH_OP, NYIR_SHL_I64, 0, 3},
    {NYIR_MUL_I64, 1, 8, 0, 0, 0, EG_RH_OP, NYIR_SHL_I64, 1, 3},
    {NYIR_MUL_I64, 0, 0, 1, 16, 0, EG_RH_OP, NYIR_SHL_I64, 0, 4},
    {NYIR_MUL_I64, 1, 16, 0, 0, 0, EG_RH_OP, NYIR_SHL_I64, 1, 4},
    /*
     * Idempotence: x&x -> x ; x|x -> x.  Both have two variable children that
     * must be the same class, so the rule pattern's `same` flag is the check.
     */
    {NYIR_AND_I64, 0, 0, 0, 0, 1, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_OR_I64, 0, 0, 0, 0, 1, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    /*
     * and: &0 -> 0 ; &-1 -> x
     */
    {NYIR_AND_I64, 0, 0, 1, 0, 0, EG_RH_LIT, NYIR_OP_COUNT, 0, 0},
    {NYIR_AND_I64, 1, 0, 0, 0, 0, EG_RH_LIT, NYIR_OP_COUNT, 1, 0},
    {NYIR_AND_I64, 0, 0, 1, -1, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_AND_I64, 1, -1, 0, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 1, 0},
    /*
     * or: |0 -> x ; |-1 -> -1
     */
    {NYIR_OR_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_OR_I64, 1, 0, 0, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 1, 0},
    {NYIR_OR_I64, 0, 0, 1, -1, 0, EG_RH_LIT, NYIR_OP_COUNT, 0, -1},
    {NYIR_OR_I64, 1, -1, 0, 0, 0, EG_RH_LIT, NYIR_OP_COUNT, 1, -1},
    /*
     * xor: ^0 -> x ; x^x -> 0
     */
    {NYIR_XOR_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_XOR_I64, 1, 0, 0, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 1, 0},
    {NYIR_XOR_I64, 0, 0, 0, 0, 1, EG_RH_LIT, NYIR_OP_COUNT, 0, 0},
    /*
     * shifts: shl(x,0) -> x ; sar(x,0) -> x
     */
    {NYIR_SHL_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_SAR_I64, 0, 0, 1, 0, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    /*
     * div/mod by 1 (divisor is non-zero, so no trap)
     */
    {NYIR_DIV_I64, 0, 0, 1, 1, 0, EG_RH_COPY, NYIR_OP_COUNT, 0, 0},
    {NYIR_MOD_I64, 0, 0, 1, 1, 0, EG_RH_LIT, NYIR_OP_COUNT, 0, 0},
};

static size_t ny_eg_rule_count(void) {
  return sizeof(ny_eg_rules) / sizeof(ny_eg_rules[0]);
}

static bool eg_is_i64_scalar(nyr_op_t op) {
  switch (op) {
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_MUL_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
  case NYIR_DIV_I64:
  case NYIR_MOD_I64:
    return true;
  default:
    return false;
  }
}

static bool eg_inst_pure_scalar(const nyr_inst_t *in) {
  if (!in || in->dst < 0)
    return false;
  if (in->op == NYIR_COPY)
    return in->a >= 0;
  if (!eg_is_i64_scalar(in->op))
    return false;
  if (nyr_inst_effects(in) != NYIR_EFFECT_NONE)
    return false;
  return in->a >= 0 && in->b >= 0;
}
static int eg_op_cost(int op, size_t loop_depth, uint64_t trip_hint) {
  int cost = 0;
  switch (op) {
  case NYIR_COPY:
  case NYIR_CONST_I64:
  case NY_EG_LEAF:
    cost = 1;
    break;
  case NYIR_ADD_I64:
  case NYIR_SUB_I64:
  case NYIR_AND_I64:
  case NYIR_OR_I64:
  case NYIR_XOR_I64:
  case NYIR_SHL_I64:
  case NYIR_SAR_I64:
    cost = 3;
    break;
  case NYIR_MUL_I64:
    cost = 4;
    break;
  default:
    cost = 5;
    break;
  }
  if (loop_depth > 0 && trip_hint > 1 &&
      (op == NYIR_MUL_I64 || op == NYIR_DIV_I64 || op == NYIR_MOD_I64)) {
    uint64_t scaled = trip_hint > 1024 ? 1024 : trip_hint;
    size_t bonus = loop_depth * (size_t)(1 + scaled / 256);
    cost += bonus > 8 ? 8 : (int)bonus;
  }
  return cost;
}

static void eg_block_scev_hint(const nyr_cfg_t *cfg,
                               const nyr_scev_info_t *scev, size_t block,
                               size_t *depth_out, uint64_t *trip_out) {
  size_t depth = 0;
  uint64_t trip = 0;
  if (cfg && scev && block < cfg->block_count) {
    for (size_t i = 0; i < scev->count; ++i) {
      const nyr_scev_loop_t *loop = &scev->loops[i];
      if (!loop->trip_count_known || loop->trip_count < 2 ||
          loop->header_block >= cfg->block_count ||
          loop->latch_block >= cfg->block_count ||
          !nyr_cfg_dominates(cfg, loop->header_block, block) ||
          !nyr_cfg_dominates(cfg, block, loop->latch_block))
        continue;
      ++depth;
      if (loop->trip_count > trip)
        trip = loop->trip_count;
    }
  }
  if (depth_out)
    *depth_out = depth;
  if (trip_out)
    *trip_out = trip;
}

/*
 * Extraction bookkeeping
 */

typedef struct {
  int class_id;     /* canonical class, or -1 */
  int defined;      /* pure scalar def in the current block */
  int materialized; /* extraction finished */
  int cost;         /* cost of the materialized representative */
  int forced_reuse; /* exact dominating computation reused across blocks */
  int rewrite;      /* plan a rewrite */
  nyr_op_t out_op;
  int out_a; /* operand value id, or -1 */
  int out_b; /* operand value id, or -1 */
  int fresh; /* !=0 materialize a fresh const (imm in out_cimm) */
  int64_t out_cimm;
} ny_eg_vm_t;

static bool eg_inst_pure_scalar(const nyr_inst_t *in);

static void eg_compute_dominated_reuse(const nyr_func_t *f,
                                       const nyr_cfg_t *cfg, int *reuse) {
  if (!f || !cfg || !reuse)
    return;
  for (size_t value = 0; value < (size_t)f->next_value; ++value)
    reuse[value] = -1;
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (!eg_inst_pure_scalar(in) || in->dst < 0 || in->dst >= f->next_value)
      continue;
    size_t use_block = cfg->inst_block[i];
    for (size_t j = 0; j < i; ++j) {
      const nyr_inst_t *candidate = &f->data[j];
      if (!eg_inst_pure_scalar(candidate) || candidate->op != in->op ||
          candidate->a != in->a || candidate->b != in->b || candidate->dst < 0)
        continue;
      size_t def_block = cfg->inst_block[j];
      bool dominates = def_block == use_block
                           ? j < i
                           : nyr_cfg_dominates(cfg, def_block, use_block);
      if (dominates) {
        reuse[in->dst] = candidate->dst;
        if (ny_trace_enabled("NY_EGRAPH_TRACE"))
          ny_trace_line("EGRAPH", "reuse value=%d from=%d blocks=%zu->%zu",
                        in->dst, candidate->dst, def_block, use_block);
        break;
      }
    }
  }
}

/*
 * Ensure the operand value `q` is registered (const class or leaf).
 */
static int eg_class_for_value(ny_egraph_t *g, ny_eg_vm_t *vm, int q,
                              const bool *is_const, const int64_t *const_imm,
                              size_t next_value) {
  if (q >= 0 && (size_t)q < next_value && is_const[q]) {
    int c = eg_class_for_const(g, const_imm[q]);
    vm[q].class_id = c;
    vm[q].materialized = 1;
    vm[q].cost = 0;
    return c;
  }
  int c = eg_class_for_leaf(g, q);
  vm[q].class_id = c;
  vm[q].materialized = 1;
  vm[q].cost = 0;
  return c;
}
/*
 * Resolve an e-class to an operand expression.  Returns a value id already
 * materialized, -2 with *out_imm set when the class carries a constant that
 * must be materialized fresh, or -1 when not realizable.
 */
static int eg_resolve_child(const ny_egraph_t *g, const ny_eg_vm_t *vm,
                            size_t next_value, const int *def_at,
                            size_t cur_idx, int goal, int cc,
                            int64_t *out_imm) {
  for (int q = 0; q < (int)next_value; ++q) {
    if (q == goal || vm[q].class_id < 0 || !vm[q].materialized)
      continue;
    /*
     * A replacement child must be defined BEFORE the instruction being
     * rewritten. Value ids mostly follow textual order, but passes that
     * insert constants mid-block allocate high ids for early positions,
     * so id order alone proves nothing; check the definition index.
     */
    if (!def_at || def_at[q] < 0 || (size_t)def_at[q] >= cur_idx)
      continue;
    if (eg_find((ny_egraph_t *)g, vm[q].class_id) ==
        eg_find((ny_egraph_t *)g, cc))
      return q;
  }
  if (eg_class_has_const(g, cc, eg_class_first_const(g, cc))) {
    *out_imm = eg_class_first_const(g, cc);
    return -2;
  }
  return -1;
}

/*
 * Find the cheapest realizable member of class `gc` for value `goal`.
 * Returns 0 and sets *best and *best_cost when a strictly-cheaper form exists,
 * 0 with *best == -1 when the original is cheapest, and -1 when nothing is
 * realizable.
 */
static int eg_extract(const ny_egraph_t *g, const ny_eg_vm_t *vm, int goal,
                      size_t next_value, const int *def_at, size_t cur_idx,
                      int gc, int orig_cost, size_t loop_depth,
                      uint64_t trip_hint, int *best, int *best_cost) {
  int target = eg_find((ny_egraph_t *)g, gc);
  int best_node = -1;
  int min_cost = INT_MAX;
  for (int i = 0; i < g->n; ++i) {
    if (eg_find((ny_egraph_t *)g, i) != target)
      continue;
    const ny_eg_node_t *n = &g->nodes[i];
    int cost = 0;
    bool ok = true;
    switch (n->op) {
    case NYIR_CONST_I64:
      cost = 1;
      break;
    case NY_EG_LEAF: {
      int src = (int)n->imm;
      if (src == goal || src < 0 || src >= (int)next_value) {
        ok = false;
        break;
      }
      cost = vm[src].materialized ? vm[src].cost : 1;
      break;
    }
    default:
      if (n->op == NYIR_COPY || n->nchild >= 1) {
        int c0 = eg_find((ny_egraph_t *)g, n->child0);
        int c1 = n->nchild > 1 ? eg_find((ny_egraph_t *)g, n->child1) : -1;
        if (c0 == target) {
          ok = false;
          break;
        }
        int64_t ign = 0;
        int a0 = eg_resolve_child(g, vm, next_value, def_at, cur_idx, goal, c0,
                                  &ign);
        if (a0 < 0 || a0 == -2) {
          ok = false;
          break;
        }
        cost = eg_op_cost(n->op, loop_depth, trip_hint) + vm[a0].cost;
        if (n->nchild > 1) {
          if (c1 == target) {
            ok = false;
            break;
          }
          int64_t imm1 = 0;
          int a1 = eg_resolve_child(g, vm, next_value, def_at, cur_idx, goal,
                                    c1, &imm1);
          if (a1 < 0 && a1 != -2) { /* -1 is impossible; -2 is a fresh const */
            ok = false;
            break;
          }
          cost += (a1 == -2) ? 0 : vm[a1].cost;
        }
      } else {
        ok = false;
      }
      break;
    }
    if (!ok)
      continue;
    if (cost < min_cost || (cost == min_cost && i < best_node)) {
      min_cost = cost;
      best_node = i;
    }
  }
  if (best_node < 0 || min_cost >= orig_cost) {
    *best = -1;
    *best_cost = orig_cost;
    return 0;
  }
  *best = best_node;
  *best_cost = min_cost;
  return 0;
}

/*
 * Pass
 */

bool nyr_egraph_rewrite(nyr_func_t *f) {
  if (!f || f->len < 2 || f->next_value <= 0)
    return true;
  const char *env = getenv("NY_EGRAPH");
  if (env && (*env == '0' || strcmp(env, "false") == 0))
    return true;
  ++ny_eg_runs;
  nyr_cfg_t cfg = {0};
  if (!nyr_cfg_build(f, &cfg)) {
    return false;
  }
  nyr_scev_info_t scev = {0};
  (void)nyr_scev_analyze(f, &scev);

  size_t next_value = (size_t)f->next_value;

  /*
   * Map value ids defined by CONST_I64 to their immediates.
   */
  int64_t *const_imm =
      ny_malloc_array(next_value > 0 ? next_value : 1, sizeof(*const_imm));
  bool *is_const =
      ny_calloc_array(next_value > 0 ? next_value : 1, sizeof(*is_const));
  if (!const_imm || !is_const) {
    free(const_imm);
    nyr_scev_free(&scev);
    free(is_const);
    nyr_cfg_free(&cfg);
    return false;
  }
  for (size_t i = 0; i < f->len; ++i) {
    const nyr_inst_t *in = &f->data[i];
    if (in->op == NYIR_CONST_I64 && in->dst >= 0 && in->dst < (int)next_value) {
      is_const[in->dst] = true;
      const_imm[in->dst] = in->imm;
    }
  }
  int *dominated_reuse = ny_malloc_array(next_value > 0 ? next_value : 1,
                                         sizeof(*dominated_reuse));
  if (!dominated_reuse) {
    nyr_scev_free(&scev);
    free(const_imm);
    free(is_const);
    nyr_cfg_free(&cfg);
    return false;
  }
  eg_compute_dominated_reuse(f, &cfg, dominated_reuse);

  size_t out_cap = (size_t)f->cap * 2 + 64;
  nyr_inst_t *out = ny_malloc_array(out_cap, sizeof(*out));
  if (!out) {
    free(dominated_reuse);
    nyr_scev_free(&scev);
    nyr_cfg_free(&cfg);
    return false;
  }
  size_t out_len = 0;
#define EG_PUSH(inst_)                                          \
  {                                                             \
    nyr_inst_t _p = (inst_);                                    \
    if (out_len == out_cap) {                                   \
      size_t _nc = out_cap * 2;                                 \
      nyr_inst_t *_n = ny_realloc_array(out, _nc, sizeof(*_n)); \
      if (!_n)                                                  \
        goto eg_oom;                                            \
      out = _n;                                                 \
      out_cap = _nc;                                            \
    }                                                           \
    out[out_len++] = _p;                                        \
  }

  /*
   * The reuse map is dominance-aware, so the output remains in original CFG
   * order while parent materializations are still available to descendants.
   */
  for (size_t b = 0; b < cfg.block_count; ++b) {
    size_t start = cfg.block_start[b], end = cfg.block_end[b];
    size_t loop_depth = 0;
    uint64_t trip_hint = 0;
    eg_block_scev_hint(&cfg, &scev, b, &loop_depth, &trip_hint);
    bool any = false;
    bool has_forced_reuse = false;
    int pure_scalars = 0;
    for (size_t i = start; i < end; ++i) {
      if (eg_inst_pure_scalar(&f->data[i])) {
        any = true;
        ++pure_scalars;
        if (f->data[i].dst >= 0 && dominated_reuse[f->data[i].dst] >= 0)
          has_forced_reuse = true;
      }
    }
    if (!any || (pure_scalars < 2 && !has_forced_reuse)) {
      for (size_t i = start; i < end; ++i)
        EG_PUSH(f->data[i]);
      continue;
    }

    ny_eg_vm_t *vm =
        ny_calloc_array(next_value > 0 ? next_value : 1, sizeof(*vm));
    if (!vm)
      goto eg_oom;
    for (size_t i = 0; i < next_value; ++i)
      vm[i].class_id = -1;
    for (size_t i = start; i < end; ++i)
      if (eg_inst_pure_scalar(&f->data[i]))
        vm[f->data[i].dst].defined = 1;
    for (size_t i = start; i < end; ++i) {
      if (eg_inst_pure_scalar(&f->data[i])) {
        vm[f->data[i].dst].forced_reuse = dominated_reuse[f->data[i].dst];
      }
    }

    ny_egraph_t g = {0};
    if (eg_new(&g, NY_EG_LEAF, 0, 0, -1, -1) < 0) {
      eg_free_graph(&g);
      free(vm);
      goto eg_oom;
    }

    /*
     * Build e-nodes in def order (pure scalars only).
     */
    for (size_t i = start; i < end; ++i) {
      const nyr_inst_t *in = &f->data[i];
      if (!eg_inst_pure_scalar(in))
        continue;
      int v = in->dst;
      int a = in->op == NYIR_COPY ? in->a : in->a;
      int bb = in->op == NYIR_COPY ? -1 : in->b;
      int nch = in->op == NYIR_COPY ? 1 : 2;
      if (vm[a].class_id < 0)
        eg_class_for_value(&g, vm, a, is_const, const_imm, next_value);
      if (nch == 2 && vm[bb].class_id < 0)
        eg_class_for_value(&g, vm, bb, is_const, const_imm, next_value);
      int ca = vm[a].class_id;
      int cb = nch == 2 ? vm[bb].class_id : -1;
      int node = eg_intern(&g, in->op, 0, nch, ca, cb);
      if (node < 0) {
        eg_free_graph(&g);
        free(vm);
        goto eg_oom;
      }
      vm[v].class_id = eg_find(&g, node);
    }

    eg_class_for_const(&g, 0);
    eg_class_for_const(&g, 1);
    eg_class_for_const(&g, -1);
    eg_class_for_const(&g, 2);
    eg_class_for_const(&g, 4);
    eg_class_for_const(&g, 8);
    eg_class_for_const(&g, 16);

    /*
     * Bound pathological blocks: emit unchanged and process the next.
     */
    if (g.n >= NY_EG_NODE_FUEL) {
      ++ny_eg_fuel_exhausted;
      ny_dbg_heuristic_event(
          "egraph",
          "block pc=%zu..%zu skipped: node budget %d reached before saturation",
          start, end, NY_EG_NODE_FUEL);
      for (size_t i = start; i < end; ++i)
        EG_PUSH(f->data[i]);
      eg_free_graph(&g);
      free(vm);
      continue;
    }

    /*
     * Saturate.
     *
     * Equality saturation with batched congruence closure.  Rules are applied
     * once per iteration and the e-graph is then canonicalized by one rebuild:
     * every root's children are re-canonicalized, and roots that became
     * identical merge through the hash-consed table.  The previous loop
     * compared every pair of nodes on every iteration, which is quadratic in
     * the block size and was the slowest compile path for large pure blocks.
     */
    bool fuel_exhausted = false;
    bool facts_enabled = eg_facts_enabled();
    bool *nn = facts_enabled ? ny_calloc_array(NY_EG_NODE_FUEL, 1) : NULL;
    if (facts_enabled && !nn) {
      eg_free_graph(&g);
      free(vm);
      goto eg_oom;
    }
    for (int iter = 0; iter < NY_EG_MAX_ITERS; ++iter) {
      int unions = 0;
      for (int i = 0; i < g.n; ++i) {
        if (fuel_exhausted)
          break;
        const ny_eg_node_t *n = &g.nodes[i];
        if (!eg_is_i64_scalar(n->op) || n->nchild != 2)
          continue;
        int ci = eg_find(&g, i);
        int c0 = eg_find(&g, n->child0);
        int c1 = eg_find(&g, n->child1);
        for (size_t r = 0; r < ny_eg_rule_count(); ++r) {
          const ny_eg_rule_t *rule = &ny_eg_rules[r];
          if ((int)rule->lhs != n->op)
            continue;
          if (rule->same && c0 != c1)
            continue;
          if (rule->c0_kind != 0 && !eg_class_has_const(&g, c0, rule->c0_imm))
            continue;
          if (rule->c1_kind != 0 && !eg_class_has_const(&g, c1, rule->c1_imm))
            continue;
          int var_class = rule->c0_kind == 0 ? c0 : c1;
          int made = 0;
          if (rule->rhs_kind == EG_RH_COPY) {
            int vc = eg_find(&g, var_class);
            if (vc != ci) {
              eg_union(&g, ci, vc);
              ++made;
            }
          } else if (rule->rhs_kind == EG_RH_LIT) {
            int lc = eg_class_for_const(&g, rule->rhs_imm);
            if (lc != ci) {
              eg_union(&g, ci, lc);
              ++made;
            }
          } else {
            int cc = eg_class_for_const(&g, rule->rhs_imm);
            int rnode = eg_intern(&g, rule->rhs_op, 0, 2, var_class, cc);
            if (rnode < 0) {
              fuel_exhausted = true;
              break;
            }
            int rc = eg_find(&g, rnode);
            if (rc != ci) {
              eg_union(&g, ci, rc);
              ++made;
            }
          }
          unions += made;
        }
      }
      int merged = eg_rebuild(&g);
      if (merged < 0) {
        fuel_exhausted = true;
        break;
      }
      unions += merged;
      if (facts_enabled) {
        eg_nonneg_roots(&g, nn);
        int fact_unions = eg_apply_fact_rules(&g, nn);
        if (fact_unions < 0) {
          fuel_exhausted = true;
          break;
        }
        if (fact_unions) {
          unions += fact_unions;
          if (eg_rebuild(&g) < 0) {
            fuel_exhausted = true;
            break;
          }
        }
      }
      if (unions == 0)
        break;
    }
    if (fuel_exhausted)
      ++ny_eg_fuel_exhausted;
    /*
     * Extraction + plan. def_at[q] records WHERE each value is defined in
     * this block so a planned replacement can never reference a value
     * defined later in the same block (use-before-def).
     */
    int *def_at =
        ny_malloc_array(next_value > 0 ? next_value : 1, sizeof(*def_at));
    if (!def_at) {
      eg_free_graph(&g);
      free(vm);
      goto eg_oom;
    }
    for (size_t q = 0; q < next_value; ++q)
      def_at[q] = -1;
    for (size_t i = start; i < end; ++i)
      if (f->data[i].dst >= 0 && (size_t)f->data[i].dst < next_value)
        def_at[f->data[i].dst] = (int)i;

    for (size_t i = start; i < end; ++i) {
      const nyr_inst_t *in = &f->data[i];
      /*
       * Analysis-guarded strength reduction for trap-free division.  DIV and
       * MOD are not e-nodes (they may fault), so they cannot pick up a
       * refined shift/mask form through extraction; they are planned here
       * against the same non-negativity fact.  The divisor is a nonzero
       * constant, so no observable trap is added or removed: x / 2^k is
       * x >> k and x % 2^k is x & (2^k - 1) exactly when x >= 0, which is
       * what the fact proves.
       */
      if (facts_enabled && in->a >= 0 && in->b >= 0 &&
          (in->op == NYIR_DIV_I64 || in->op == NYIR_MOD_I64) &&
          (size_t)in->b < next_value && is_const[in->b] &&
          (size_t)in->a < next_value && vm[in->a].class_id >= 0) {
        int shift = 0;
        int64_t divisor = const_imm[in->b];
        if (eg_pow2_shift(divisor, &shift) &&
            eg_class_nonneg(&g, nn, vm[in->a].class_id)) {
          int v = in->dst;
          if (v >= 0 && (size_t)v < next_value) {
            vm[v].rewrite = 1;
            vm[v].materialized = 1;
            vm[v].cost = 1;
            vm[v].fresh = 1;
            vm[v].out_op =
                in->op == NYIR_DIV_I64 ? NYIR_SAR_I64 : NYIR_AND_I64;
            vm[v].out_a = in->a;
            vm[v].out_b = -1;
            vm[v].out_cimm = in->op == NYIR_DIV_I64 ? (int64_t)shift
                                                    : (divisor - 1);
            continue;
          }
        }
      }
      if (!eg_inst_pure_scalar(in))
        continue;
      int v = in->dst;
      if (v >= 0 && (size_t)v < next_value && vm[v].forced_reuse >= 0) {
        vm[v].rewrite = 1;
        vm[v].out_op = NYIR_COPY;
        vm[v].out_a = vm[v].forced_reuse;
        if (ny_trace_enabled("NY_EGRAPH_TRACE"))
          ny_trace_line("EGRAPH", "forced plan value=%d from=%d", v,
                        vm[v].forced_reuse);
        vm[v].out_b = -1;
        vm[v].materialized = 1;
        vm[v].cost = 1;
        continue;
      }
      if (v < 0 || (size_t)v >= next_value || vm[v].class_id < 0) {
        vm[v].materialized = 1;
        vm[v].cost = eg_op_cost(in->op, loop_depth, trip_hint);
        continue;
      }
      int64_t ignored_imm = 0;
      int existing = eg_resolve_child(&g, vm, next_value, def_at, i, v,
                                      vm[v].class_id, &ignored_imm);
      if (existing >= 0) {
        vm[v].rewrite = 1;
        vm[v].out_op = NYIR_COPY;
        vm[v].out_a = existing;
        vm[v].out_b = -1;
        vm[v].materialized = 1;
        vm[v].cost = vm[existing].cost;
        continue;
      }
      int orig_cost = eg_op_cost(in->op, loop_depth, trip_hint);
      int best = -1, best_cost = INT_MAX;
      int extract_rc =
          eg_extract(&g, vm, v, next_value, def_at, i, vm[v].class_id,
                     orig_cost, loop_depth, trip_hint, &best, &best_cost);
      if (extract_rc != 0) {
        ny_dbg_heuristic_event("egraph",
                               "keep v%d op=%s cost=%d: no strictly cheaper "
                               "realizable representative",
                               v, nyr_op_name(in->op), orig_cost);
        vm[v].materialized = 1;
        vm[v].cost = orig_cost;
        continue;
      }
      if (best < 0) {
        vm[v].materialized = 1;
        vm[v].cost = orig_cost;
        continue;
      }
      const ny_eg_node_t *rn = &g.nodes[best];
      const char *selected_op =
          rn->op == NY_EG_LEAF ? "leaf" : nyr_op_name((nyr_op_t)rn->op);
      ny_dbg_heuristic_event("egraph",
                             "choose v%d op=%s cost=%d -> op=%s cost=%d "
                             "(loop-depth=%zu trip-hint=%llu)",
                             v, nyr_op_name(in->op), orig_cost, selected_op,
                             best_cost, loop_depth,
                             (unsigned long long)trip_hint);
      vm[v].cost = best_cost;
      vm[v].materialized = 1;
      if (rn->op == NYIR_CONST_I64) {
        vm[v].rewrite = 1;
        vm[v].out_op = NYIR_CONST_I64;
        vm[v].out_a = -1;
        vm[v].out_b = -1;
        vm[v].out_cimm = rn->imm;
      } else if (rn->op == NY_EG_LEAF) {
        vm[v].rewrite = 1;
        vm[v].out_op = NYIR_COPY;
        vm[v].out_a = (int)rn->imm;
        vm[v].out_b = -1;
      } else {
        int c0 = eg_find(&g, rn->child0);
        int64_t ign = 0;
        int a0 = eg_resolve_child(&g, vm, next_value, def_at, i, v, c0, &ign);
        int a1 = -1;
        int64_t cc1 = 0;
        int fresh = 0;
        if (rn->nchild > 1) {
          int c1 = eg_find(&g, rn->child1);
          int r1 = eg_resolve_child(&g, vm, next_value, def_at, i, v, c1, &cc1);
          if (r1 == -2) {
            a1 = -1;
            fresh = 1;
          } else if (r1 >= 0) {
            a1 = r1;
          }
        }
        if (a0 < 0 || (rn->nchild > 1 && a1 < 0 && !fresh)) {
          vm[v].rewrite = 0;
          continue;
        }
        vm[v].rewrite = 1;
        vm[v].out_op = rn->op;
        vm[v].out_a = a0;
        vm[v].out_b = a1;
        vm[v].fresh = fresh;
        vm[v].out_cimm = cc1;
      }
    }

    /*
     * Emit the block.
     */
    for (size_t i = start; i < end; ++i) {
      const nyr_inst_t *in = &f->data[i];
      /*
       * The plan is keyed by destination value, so it also covers the DIV/MOD
       * strength reductions above, which are not pure-scalar e-nodes.
       */
      if (in->dst < 0 || (size_t)in->dst >= next_value ||
          !vm[in->dst].rewrite) {
        EG_PUSH(*in);
        continue;
      }
      int v = in->dst;
      nyr_inst_t n = *in;
      n.op = vm[v].out_op;
      n.flags = 0;
      n.symbol = NULL;
      n.effects = NYIR_EFFECT_NONE;
      if (vm[v].out_op == NYIR_CONST_I64) {
        nir_make_const(&n, vm[v].out_cimm);
      } else {
        if (vm[v].fresh) {
          int fresh_v = f->next_value++;
          nyr_inst_t c = {0};
          c.op = NYIR_CONST_I64;
          c.dst = fresh_v;
          c.a = -1;
          c.b = -1;
          c.imm = vm[v].out_cimm;
          c.flags = 0;
          c.effects = NYIR_EFFECT_NONE;
          EG_PUSH(c);
          n.a = vm[v].out_a;
          n.b = fresh_v;
        } else {
          n.a = vm[v].out_a;
          n.b = vm[v].out_b;
        }
        n.imm = 0;
        /*
         * E-graph extraction may change the address-producing expression.
         * Do not carry the old expression's alias proof across the rewrite;
         * the next alias-analysis run can derive a fresh class.
         */
        n.alias_class = 0;
      }
      EG_PUSH(n);
    }

    free(def_at);
    free(nn);
    nn = NULL;
    eg_free_graph(&g);
    free(vm);
  }

  nyr_cfg_free(&cfg);

  {
    nyr_func_t candidate = *f;
    candidate.data = out;
    candidate.len = out_len;
    candidate.cap = out_cap;
    candidate.next_value = f->next_value;
    char err[256] = {0};
    if (nyr_ir_verify(&candidate, err, sizeof(err))) {
      free(f->data);
      f->data = candidate.data;
      f->len = candidate.len;
      f->cap = candidate.cap;
      f->next_value = candidate.next_value;
      out = NULL; /* Ownership transferred to f->data, do not free in eg_oom */
    } else {
      if (ny_dbg_is_enabled(NY_DBG_HEURISTIC))
        ny_dbg_heuristic_event("EGRAPH", "candidate rejected: %s",
                               err[0] ? err : "unknown verifier error");
      if (ny_trace_enabled("NY_EGRAPH_TRACE") &&
          ny_dbg_function_selected(NULL))
        nyr_dump(stderr, &candidate, "egraph-candidate");
    }
    goto eg_oom;
  }

eg_oom:
  free(out);
  out = NULL;
  nyr_scev_free(&scev);
  free(dominated_reuse);
  dominated_reuse = NULL;
  free(is_const);
  is_const = NULL;
  free(const_imm);
  const_imm = NULL;
  nyr_cfg_free(&cfg);
  return true;

#undef EG_PUSH
}
