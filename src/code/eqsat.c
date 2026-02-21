#include "eqsat.h"
#include "base/util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CLASS_CAPACITY 256
#define INITIAL_HASH_TABLE_SIZE 1024
#define MAX_SATURATION_ITERS 10

static uint64_t hash_enode(const enode *n) {
  uint64_t h = ny_fnv1a64(&n->op, sizeof(n->op), 0);
  switch (n->op) {
  case OP_CONST:
    h = ny_fnv1a64(&n->data.const_val, sizeof(n->data.const_val), h);
    break;
  case OP_VAR:
    h = ny_fnv1a64(&n->data.var_id, sizeof(n->data.var_id), h);
    break;
  case OP_NEG:
  case OP_NOT:
    h = ny_fnv1a64(&n->data.unary, sizeof(n->data.unary), h);
    break;
  default:
    h = ny_fnv1a64(&n->data.binary.left, sizeof(n->data.binary.left), h);
    h = ny_fnv1a64(&n->data.binary.right, sizeof(n->data.binary.right), h);
    break;
  }
  return h;
}

static bool enode_equal(const enode *a, const enode *b) {
  if (a->op != b->op)
    return false;
  switch (a->op) {
  case OP_CONST:
    return a->data.const_val == b->data.const_val;
  case OP_VAR:
    return a->data.var_id == b->data.var_id;
  case OP_NEG:
  case OP_NOT:
    return a->data.unary == b->data.unary;
  default:
    return a->data.binary.left == b->data.binary.left &&
           a->data.binary.right == b->data.binary.right;
  }
}

void egraph_init(egraph *g) {
  memset(g, 0, sizeof(*g));
  g->class_capacity = INITIAL_CLASS_CAPACITY;
  g->classes = calloc(g->class_capacity, sizeof(eclass));
  g->hash_table_size = INITIAL_HASH_TABLE_SIZE;
  g->hash_table = calloc(g->hash_table_size, sizeof(eclass_id));
  for (size_t i = 0; i < g->hash_table_size; i++) {
    g->hash_table[i] = ECLASS_INVALID;
  }
  g->verbose = (getenv("NYTRIX_EQSAT_VERBOSE") != NULL);
}

void egraph_dispose(egraph *g) {
  for (size_t i = 0; i < g->class_count; i++) {
    free(g->classes[i].nodes);
  }
  free(g->classes);
  free(g->hash_table);
  memset(g, 0, sizeof(*g));
}

static eclass_id egraph_new_class(egraph *g) {
  if (g->class_count >= g->class_capacity) {
    g->class_capacity *= 2;
    g->classes = realloc(g->classes, g->class_capacity * sizeof(eclass));
  }
  eclass_id id = g->class_count++;
  eclass *ec = &g->classes[id];
  memset(ec, 0, sizeof(*ec));
  ec->id = id;
  ec->parent = id;
  ec->cost = INT64_MAX;
  ec->node_capacity = 4;
  ec->nodes = malloc(ec->node_capacity * sizeof(enode));
  return id;
}

static void eclass_add_node(eclass *ec, const enode *n) {
  if (ec->node_count >= ec->node_capacity) {
    ec->node_capacity *= 2;
    ec->nodes = realloc(ec->nodes, ec->node_capacity * sizeof(enode));
  }
  ec->nodes[ec->node_count++] = *n;
}

eclass_id egraph_find(egraph *g, eclass_id id) {
  if (id >= g->class_count)
    return ECLASS_INVALID;
  eclass *ec = &g->classes[id];
  if (ec->parent != id) {
    ec->parent = egraph_find(g, ec->parent); /* Path compression */
  }
  return ec->parent;
}

static eclass_id egraph_lookup_hash(egraph *g, const enode *n) {
  uint64_t h = hash_enode(n);
  size_t idx = h % g->hash_table_size;
  for (size_t i = 0; i < g->hash_table_size; i++) {
    size_t probe = (idx + i) % g->hash_table_size;
    eclass_id cid = g->hash_table[probe];
    if (cid == ECLASS_INVALID)
      return ECLASS_INVALID;
    cid = egraph_find(g, cid);
    eclass *ec = &g->classes[cid];
    for (size_t j = 0; j < ec->node_count; j++) {
      if (enode_equal(&ec->nodes[j], n))
        return cid;
    }
  }
  return ECLASS_INVALID;
}

static void egraph_insert_hash(egraph *g, eclass_id id, const enode *n) {
  uint64_t h = hash_enode(n);
  size_t idx = h % g->hash_table_size;

  for (size_t i = 0; i < g->hash_table_size; i++) {
    size_t probe = (idx + i) % g->hash_table_size;
    if (g->hash_table[probe] == ECLASS_INVALID) {
      g->hash_table[probe] = id;
      return;
    }
  }

  /* Hash table full, should rehash but for now just skip */
}

static eclass_id egraph_add_node(egraph *g, const enode *n) {
  /* Check if already exists */
  eclass_id existing = egraph_lookup_hash(g, n);
  if (existing != ECLASS_INVALID)
    return existing;

  /* Create new e-class */
  eclass_id id = egraph_new_class(g);
  eclass *ec = &g->classes[id];
  eclass_add_node(ec, n);

  /* Check if it's a constant */
  if (n->op == OP_CONST) {
    ec->is_const = true;
    ec->const_value = n->data.const_val;
    ec->cost = 1;
  } else if (n->op == OP_VAR) {
    ec->cost = 1;
  }

  egraph_insert_hash(g, id, n);
  return id;
}

eclass_id egraph_add_const(egraph *g, int64_t value) {
  enode n = {.op = OP_CONST, .data.const_val = value};
  return egraph_add_node(g, &n);
}

eclass_id egraph_add_var(egraph *g, uint64_t var_id) {
  enode n = {.op = OP_VAR, .data.var_id = var_id};
  return egraph_add_node(g, &n);
}

// ... (leave add_binary/unary/union/rebuild/rewrite rules as is)

/* LLVM Integration */

static enode_op llvm_op_map(LLVMOpcode op) {
  switch (op) {
  case LLVMAdd:
    return OP_ADD;
  case LLVMSub:
    return OP_SUB;
  case LLVMMul:
    return OP_MUL;
  case LLVMSDiv:
    return OP_DIV;
  case LLVMUDiv:
    return OP_UDIV;
  case LLVMSRem:
    return OP_REM;
  case LLVMURem:
    return OP_UREM;
  case LLVMShl:
    return OP_SHL;
  case LLVMLShr:
    return OP_LSHR;
  case LLVMAShr:
    return OP_ASHR;
  case LLVMAnd:
    return OP_AND;
  case LLVMOr:
    return OP_OR;
  case LLVMXor:
    return OP_XOR;
  default:
    return (enode_op)-1;
  }
}

eclass_id egraph_from_llvm(egraph *g, LLVMValueRef val) {
  if (LLVMIsAConstantInt(val)) {
    return egraph_add_const(g, LLVMConstIntGetSExtValue(val));
  }
  if (LLVMIsAInstruction(val)) {
    LLVMOpcode op = LLVMGetInstructionOpcode(val);
    enode_op eop = llvm_op_map(op);
    if (eop != (enode_op)-1) {
      eclass_id l = egraph_from_llvm(g, LLVMGetOperand(val, 0));
      eclass_id r = egraph_from_llvm(g, LLVMGetOperand(val, 1));
      return egraph_add_binary(g, eop, l, r);
    }
    // but generic binary op handling covers most.
  }
  return egraph_add_var(g, (uint64_t)(uintptr_t)val);
}

LLVMValueRef egraph_to_llvm(egraph *g, eclass_id id, LLVMBuilderRef builder) {
  enode *n = egraph_extract(g, id);
  if (!n)
    return NULL;

  switch (n->op) {
  case OP_CONST:
    return LLVMConstInt(LLVMInt64TypeInContext(LLVMGetGlobalContext()),
                        n->data.const_val, 0);
  case OP_VAR:
    return (LLVMValueRef)(uintptr_t)n->data.var_id;
  case OP_ADD:
    return LLVMBuildAdd(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_SUB:
    return LLVMBuildSub(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_MUL:
    return LLVMBuildMul(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_DIV:
    return LLVMBuildSDiv(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_UDIV:
    return LLVMBuildUDiv(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_REM:
    return LLVMBuildSRem(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_UREM:
    return LLVMBuildURem(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_SHL:
    return LLVMBuildShl(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_LSHR:
    return LLVMBuildLShr(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_ASHR:
    return LLVMBuildAShr(builder,
                         egraph_to_llvm(g, n->data.binary.left, builder),
                         egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_AND:
    return LLVMBuildAnd(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_OR:
    return LLVMBuildOr(builder, egraph_to_llvm(g, n->data.binary.left, builder),
                       egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_XOR:
    return LLVMBuildXor(builder,
                        egraph_to_llvm(g, n->data.binary.left, builder),
                        egraph_to_llvm(g, n->data.binary.right, builder), "");
  case OP_NEG:
    return LLVMBuildNeg(builder, egraph_to_llvm(g, n->data.unary, builder), "");
  case OP_NOT:
    return LLVMBuildNot(builder, egraph_to_llvm(g, n->data.unary, builder), "");
  }
  return NULL;
}

eclass_id egraph_add_binary(egraph *g, enode_op op, eclass_id left,
                            eclass_id right) {
  left = egraph_find(g, left);
  right = egraph_find(g, right);

  enode n = {.op = op, .data.binary = {left, right}};
  return egraph_add_node(g, &n);
}

eclass_id egraph_add_unary(egraph *g, enode_op op, eclass_id operand) {
  operand = egraph_find(g, operand);

  enode n = {.op = op, .data.unary = operand};
  return egraph_add_node(g, &n);
}

void egraph_union(egraph *g, eclass_id a, eclass_id b) {
  a = egraph_find(g, a);
  b = egraph_find(g, b);

  if (a == b)
    return;

  /* Union by rank: attach smaller to larger */
  eclass *ea = &g->classes[a];
  eclass *eb = &g->classes[b];

  if (ea->node_count < eb->node_count) {
    ea->parent = b;
    /* Merge nodes from a into b */
    for (size_t i = 0; i < ea->node_count; i++) {
      eclass_add_node(eb, &ea->nodes[i]);
    }
    /* Propagate constant information */
    if (ea->is_const && !eb->is_const) {
      eb->is_const = true;
      eb->const_value = ea->const_value;
    }
  } else {
    eb->parent = a;
    for (size_t i = 0; i < eb->node_count; i++) {
      eclass_add_node(ea, &eb->nodes[i]);
    }
    if (eb->is_const && !ea->is_const) {
      ea->is_const = true;
      ea->const_value = eb->const_value;
    }
  }

  g->nodes_merged++;
}

void egraph_rebuild(egraph *g) {
  /* Canonicalize all e-node operands */
  for (size_t i = 0; i < g->class_count; i++) {
    eclass *ec = &g->classes[i];
    if (ec->parent != ec->id)
      continue; /* Skip non-canonical classes */

    for (size_t j = 0; j < ec->node_count; j++) {
      enode *n = &ec->nodes[j];
      switch (n->op) {
      case OP_NEG:
      case OP_NOT:
        n->data.unary = egraph_find(g, n->data.unary);
        break;
      case OP_CONST:
      case OP_VAR:
        break;
      default:
        n->data.binary.left = egraph_find(g, n->data.binary.left);
        n->data.binary.right = egraph_find(g, n->data.binary.right);
        break;
      }
    }
  }
}

/* Algebraic rewrite rules */

/* x + 0 → x */
static bool try_add_zero(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_ADD) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      if (g->classes[right].is_const && g->classes[right].const_value == 0) {
        egraph_union(g, root, left);
        return true;
      }
      if (g->classes[left].is_const && g->classes[left].const_value == 0) {
        egraph_union(g, root, right);
        return true;
      }
    }
  }
  return false;
}

/* x * 0 → 0 */
static bool try_mul_zero(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_MUL) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      if ((g->classes[left].is_const && g->classes[left].const_value == 0) ||
          (g->classes[right].is_const && g->classes[right].const_value == 0)) {
        eclass_id zero = egraph_add_const(g, 0);
        egraph_union(g, root, zero);
        return true;
      }
    }
  }
  return false;
}

/* x * 1 → x */
static bool try_mul_one(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_MUL) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      if (g->classes[right].is_const && g->classes[right].const_value == 1) {
        egraph_union(g, root, left);
        return true;
      }
      if (g->classes[left].is_const && g->classes[left].const_value == 1) {
        egraph_union(g, root, right);
        return true;
      }
    }
  }
  return false;
}

/* x * 2 → x << 1 (strength reduction) */
static bool try_mul_power_of_two(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_MUL) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      /* Check if right is power of 2 */
      if (g->classes[right].is_const) {
        int64_t val = g->classes[right].const_value;
        if (val > 0 && (val & (val - 1)) == 0) {
          /* Count trailing zeros to get shift amount */
          int shift = 0;
          while (((val >> shift) & 1) == 0)
            shift++;
          eclass_id shift_amt = egraph_add_const(g, shift);
          eclass_id shl = egraph_add_binary(g, OP_SHL, left, shift_amt);
          egraph_union(g, root, shl);
          return true;
        }
      }

      /* Same for left operand */
      if (g->classes[left].is_const) {
        int64_t val = g->classes[left].const_value;
        if (val > 0 && (val & (val - 1)) == 0) {
          int shift = 0;
          while (((val >> shift) & 1) == 0)
            shift++;
          eclass_id shift_amt = egraph_add_const(g, shift);
          eclass_id shl = egraph_add_binary(g, OP_SHL, right, shift_amt);
          egraph_union(g, root, shl);
          return true;
        }
      }
    }
  }
  return false;
}

/* x - x → 0 */
static bool try_sub_same(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_SUB) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      if (left == right) {
        eclass_id zero = egraph_add_const(g, 0);
        egraph_union(g, root, zero);
        return true;
      }
    }
  }
  return false;
}

/* Constant folding */
static bool try_const_fold(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];

  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];

    /* Binary ops with two constants */
    if (n->op >= OP_ADD && n->op <= OP_XOR) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      if (g->classes[left].is_const && g->classes[right].is_const) {
        int64_t lval = g->classes[left].const_value;
        int64_t rval = g->classes[right].const_value;
        int64_t result = 0;
        bool valid = true;

        switch (n->op) {
        case OP_ADD:
          result = lval + rval;
          break;
        case OP_SUB:
          result = lval - rval;
          break;
        case OP_MUL:
          result = lval * rval;
          break;
        case OP_DIV:
          if (rval != 0)
            result = lval / rval;
          else
            valid = false;
          break;
        case OP_REM:
          if (rval != 0)
            result = lval % rval;
          else
            valid = false;
          break;
        case OP_SHL:
          result = lval << rval;
          break;
        case OP_LSHR:
          result = (uint64_t)lval >> rval;
          break;
        case OP_ASHR:
          result = lval >> rval;
          break;
        case OP_AND:
          result = lval & rval;
          break;
        case OP_OR:
          result = lval | rval;
          break;
        case OP_XOR:
          result = lval ^ rval;
          break;
        default:
          valid = false;
        }

        if (valid) {
          eclass_id const_id = egraph_add_const(g, result);
          egraph_union(g, root, const_id);
          return true;
        }
      }
    }

    /* Unary ops with constant */
    if (n->op == OP_NEG || n->op == OP_NOT) {
      eclass_id operand = egraph_find(g, n->data.unary);
      if (g->classes[operand].is_const) {
        int64_t val = g->classes[operand].const_value;
        int64_t result = (n->op == OP_NEG) ? -val : ~val;
        eclass_id const_id = egraph_add_const(g, result);
        egraph_union(g, root, const_id);
        return true;
      }
    }
  }

  return false;
}

/* Commutativity: x + y = y + x */
static bool try_commutativity(egraph *g, eclass_id root) {
  eclass *ec = &g->classes[egraph_find(g, root)];

  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];

    /* Only for commutative ops */
    if (n->op == OP_ADD || n->op == OP_MUL || n->op == OP_AND ||
        n->op == OP_OR || n->op == OP_XOR) {
      eclass_id left = egraph_find(g, n->data.binary.left);
      eclass_id right = egraph_find(g, n->data.binary.right);

      /* Create commuted version */
      eclass_id commuted = egraph_add_binary(g, n->op, right, left);
      if (egraph_find(g, commuted) != egraph_find(g, root)) {
        egraph_union(g, root, commuted);
        return true;
      }
    }
  }

  return false;
}

void egraph_saturate(egraph *g, size_t max_iterations) {
  if (max_iterations == 0)
    max_iterations = MAX_SATURATION_ITERS;

  for (size_t iter = 0; iter < max_iterations; iter++) {
    size_t rewrites_this_iter = 0;

    /* Apply all rewrite rules to all e-classes */
    for (size_t i = 0; i < g->class_count; i++) {
      eclass_id canonical = egraph_find(g, i);
      if (canonical != i)
        continue; /* Skip non-canonical */

      if (try_add_zero(g, i))
        rewrites_this_iter++;
      if (try_mul_zero(g, i))
        rewrites_this_iter++;
      if (try_mul_one(g, i))
        rewrites_this_iter++;
      if (try_mul_power_of_two(g, i))
        rewrites_this_iter++;
      if (try_sub_same(g, i))
        rewrites_this_iter++;
      if (try_const_fold(g, i))
        rewrites_this_iter++;
      if (try_commutativity(g, i))
        rewrites_this_iter++;
    }

    g->rewrites_applied += rewrites_this_iter;
    g->saturation_iterations++;

    /* Rebuild after all rewrites */
    egraph_rebuild(g);

    if (rewrites_this_iter == 0) {
      if (g->verbose)
        fprintf(stderr, "[eqsat] saturated after %zu iterations\n", iter + 1);
      break;
    }
  }
}

/* Extract best expression */
static int64_t compute_cost(egraph *g, eclass_id id) {
  id = egraph_find(g, id);
  eclass *ec = &g->classes[id];

  if (ec->cost != INT64_MAX)
    return ec->cost;

  /* Constants and vars cost 1 */
  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    if (n->op == OP_CONST || n->op == OP_VAR) {
      ec->cost = 1;
      return 1;
    }
  }

  /* Find min cost among all nodes */
  int64_t min_cost = INT64_MAX;

  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    int64_t cost = 1; /* Base cost of operation */

    if (n->op == OP_NEG || n->op == OP_NOT) {
      cost += compute_cost(g, n->data.unary);
    } else if (n->op >= OP_ADD && n->op <= OP_XOR) {
      cost += compute_cost(g, n->data.binary.left);
      cost += compute_cost(g, n->data.binary.right);

      /* Penalize expensive operations */
      if (n->op == OP_DIV || n->op == OP_UDIV || n->op == OP_REM ||
          n->op == OP_UREM) {
        cost += 10;
      } else if (n->op == OP_MUL) {
        cost += 3;
      }
    }

    if (cost < min_cost)
      min_cost = cost;
  }

  ec->cost = min_cost;
  return min_cost;
}

enode *egraph_extract(egraph *g, eclass_id id) {
  id = egraph_find(g, id);
  eclass *ec = &g->classes[id];

  /* First compute all costs */
  compute_cost(g, id);

  /* Find node with minimum cost */
  enode *best = NULL;
  int64_t best_cost = INT64_MAX;

  for (size_t i = 0; i < ec->node_count; i++) {
    enode *n = &ec->nodes[i];
    int64_t cost = 1;

    if (n->op == OP_NEG || n->op == OP_NOT) {
      cost += compute_cost(g, n->data.unary);
    } else if (n->op >= OP_ADD && n->op <= OP_XOR) {
      cost += compute_cost(g, n->data.binary.left);
      cost += compute_cost(g, n->data.binary.right);

      if (n->op == OP_DIV || n->op == OP_UDIV || n->op == OP_REM ||
          n->op == OP_UREM) {
        cost += 10;
      } else if (n->op == OP_MUL) {
        cost += 3;
      }
    }

    if (cost < best_cost) {
      best_cost = cost;
      best = n;
    }
  }

  return best;
}

void egraph_get_stats(egraph *g, uint64_t *rewrites, uint64_t *merges,
                      uint64_t *iters) {
  if (rewrites)
    *rewrites = g->rewrites_applied;
  if (merges)
    *merges = g->nodes_merged;
  if (iters)
    *iters = g->saturation_iterations;
}
