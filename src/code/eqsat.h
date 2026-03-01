#ifndef EQSAT_H
#define EQSAT_H

#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t eclass_id;
#define ECLASS_INVALID 0xFFFFFFFFu

typedef enum enode_op {
  OP_CONST,
  OP_VAR,
  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_UDIV,
  OP_REM,
  OP_UREM,
  OP_SHL,
  OP_LSHR,
  OP_ASHR,
  OP_AND,
  OP_OR,
  OP_XOR,
  OP_NEG,
  OP_NOT,
} enode_op;

typedef struct enode {
  enode_op op;
  union {
    uint64_t const_val;
    uint64_t var_id;
    struct {
      eclass_id left;
      eclass_id right;
    } binary;
    eclass_id unary;
  } data;
  uint64_t hash;
} enode;

typedef struct eclass {
  eclass_id id;
  enode *nodes;
  size_t node_count;
  size_t node_capacity;
  eclass_id parent;
  int64_t cost;
  bool is_const;
  int64_t const_value;
} eclass;

typedef struct egraph {
  eclass *classes;
  size_t class_count;
  size_t class_capacity;
  size_t node_count;
  eclass_id *hash_table;
  size_t hash_table_size;
  uint64_t rewrites_applied;
  uint64_t nodes_merged;
  uint64_t saturation_iterations;
  bool verbose;
} egraph;

typedef struct rewrite_rule {
  const char *name;
  bool (*match)(egraph *g, eclass_id root, enode *pattern, eclass_id *bindings);
  eclass_id (*apply)(egraph *g, eclass_id *bindings);
} rewrite_rule;

void egraph_init(egraph *g);

void egraph_dispose(egraph *g);

eclass_id egraph_add_const(egraph *g, int64_t value);

eclass_id egraph_add_var(egraph *g, uint64_t var_id);

eclass_id egraph_add_binary(egraph *g, enode_op op, eclass_id left,
                            eclass_id right);

eclass_id egraph_add_unary(egraph *g, enode_op op, eclass_id operand);

void egraph_union(egraph *g, eclass_id a, eclass_id b);

eclass_id egraph_find(egraph *g, eclass_id id);

void egraph_rebuild(egraph *g);

void egraph_saturate(egraph *g, size_t max_iterations);

enode *egraph_extract(egraph *g, eclass_id id);

eclass_id egraph_from_llvm(egraph *g, LLVMValueRef val);

LLVMValueRef egraph_to_llvm(egraph *g, eclass_id id, LLVMBuilderRef builder);

void egraph_get_stats(egraph *g, uint64_t *rewrites, uint64_t *merges,
                      uint64_t *iters);

#endif
