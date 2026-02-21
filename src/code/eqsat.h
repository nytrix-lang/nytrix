#ifndef EQSAT_H
#define EQSAT_H

/*
 * Equality Saturation (EqSat) Engine
 *
 * Implements equality saturation for program optimization using e-graphs.
 * This is a pure algorithmic approach - no external dependencies.
 *
 * Key concepts:
 * - E-graphs: data structure representing equivalence classes of expressions
 * - Saturation: exhaustively apply rewrite rules until fixpoint
 * - Extraction: find lowest-cost equivalent expression
 *
 * Use cases:
 * - Algebraic simplification: x+0 → x, x*1 → x, x*0 → 0
 * - Constant folding: (2+3)*4 → 20
 * - Strength reduction: x*2 → x<<1, x/2 → x>>1
 * - Common subexpression elimination
 * - Associativity/commutativity optimization
 *
 * References:
 * - "egg: Fast and Extensible Equality Saturation" (Willsey et al. 2021)
 * - "Equality Saturation: A New Approach to Optimization" (Tate et al. 2009)
 */

#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* E-class ID: represents equivalence class */
typedef uint32_t eclass_id;
#define ECLASS_INVALID 0xFFFFFFFFu

/* E-node: operation in the expression */
typedef enum enode_op {
  OP_CONST, /* Constant value */
  OP_VAR,   /* Variable reference */
  OP_ADD,   /* Addition */
  OP_SUB,   /* Subtraction */
  OP_MUL,   /* Multiplication */
  OP_DIV,   /* Division (signed) */
  OP_UDIV,  /* Division (unsigned) */
  OP_REM,   /* Remainder (signed) */
  OP_UREM,  /* Remainder (unsigned) */
  OP_SHL,   /* Shift left */
  OP_LSHR,  /* Logical shift right */
  OP_ASHR,  /* Arithmetic shift right */
  OP_AND,   /* Bitwise AND */
  OP_OR,    /* Bitwise OR */
  OP_XOR,   /* Bitwise XOR */
  OP_NEG,   /* Negation */
  OP_NOT,   /* Bitwise NOT */
} enode_op;

typedef struct enode {
  enode_op op;
  union {
    uint64_t const_val; /* For OP_CONST */
    uint64_t var_id;    /* For OP_VAR (can store LLVMValueRef ptr) */
    struct {
      eclass_id left;  /* Left operand e-class */
      eclass_id right; /* Right operand e-class */
    } binary;
    eclass_id unary; /* For unary ops */
  } data;
  uint64_t hash; /* Hash for deduplication */
} enode;

/* E-class: equivalence class of expressions */
typedef struct eclass {
  eclass_id id;
  enode *nodes; /* All e-nodes in this class */
  size_t node_count;
  size_t node_capacity;
  eclass_id parent;    /* Union-find parent */
  int64_t cost;        /* Extraction cost */
  bool is_const;       /* True if proven constant */
  int64_t const_value; /* If is_const */
} eclass;

/* E-graph: collection of e-classes */
typedef struct egraph {
  eclass *classes;
  size_t class_count;
  size_t class_capacity;

  /* Hash table for deduplication */
  eclass_id *hash_table;
  size_t hash_table_size;

  /* Statistics */
  uint64_t rewrites_applied;
  uint64_t nodes_merged;
  uint64_t saturation_iterations;

  bool verbose;
} egraph;

/* Rewrite rule */
typedef struct rewrite_rule {
  const char *name;
  bool (*match)(egraph *g, eclass_id root, enode *pattern, eclass_id *bindings);
  eclass_id (*apply)(egraph *g, eclass_id *bindings);
} rewrite_rule;

/* Initialize e-graph */
void egraph_init(egraph *g);

/* Cleanup e-graph */
void egraph_dispose(egraph *g);

/* Add constant to e-graph */
/* Add constant */
eclass_id egraph_add_const(egraph *g, int64_t value);

/* Add variable (LLVMValueRef ptr) */
eclass_id egraph_add_var(egraph *g, uint64_t var_id);

/* Add binary operation */
eclass_id egraph_add_binary(egraph *g, enode_op op, eclass_id left,
                            eclass_id right);

/* Add unary operation */
eclass_id egraph_add_unary(egraph *g, enode_op op, eclass_id operand);

/* Union two e-classes (they are equivalent) */
void egraph_union(egraph *g, eclass_id a, eclass_id b);

/* Find canonical e-class (union-find) */
eclass_id egraph_find(egraph *g, eclass_id id);

/* Rebuild e-graph after unions (restore invariants) */
void egraph_rebuild(egraph *g);

/* Run equality saturation with standard rules */
void egraph_saturate(egraph *g, size_t max_iterations);

/* Extract best (lowest cost) expression from e-class */
enode *egraph_extract(egraph *g, eclass_id id);

/* Convert LLVM value to e-graph representation */
eclass_id egraph_from_llvm(egraph *g, LLVMValueRef val);

/* Convert e-graph back to LLVM IR */
LLVMValueRef egraph_to_llvm(egraph *g, eclass_id id, LLVMBuilderRef builder);

/* Get statistics */
void egraph_get_stats(egraph *g, uint64_t *rewrites, uint64_t *merges,
                      uint64_t *iters);

#endif /* EQSAT_H */
