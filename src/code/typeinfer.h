#ifndef NY_CODE_TYPEINFER_H
#define NY_CODE_TYPEINFER_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct typeinfer_var_slot {
  const char *name;
  bool is_i64_proven;
  bool is_f64_proven;
  bool is_used_in_dynamic;
  bool escapes;
  ny_type_t *type;
} typeinfer_var_slot_t;

/* Type inference context for tracking proven i64/f64 facts */
typedef struct typeinfer_ctx {
  typeinfer_var_slot_t *vars;
  size_t var_names_cap;
  size_t var_names_len;
  ny_type_arena_t type_arena;
  size_t type_unify_errors;
  int *hash_table;      /* Maps hash to index in var_names */
  size_t hash_cap;      /* Capacity of the hash table */
  bool changed;         /* Track if any proofs changed in the current pass */
  bool formal_hm_enabled;
  scope *scopes;
  size_t func_depth;
  codegen_t *cg;
} typeinfer_ctx_t;

/* Check if a variable escapes */
bool typeinfer_escapes(typeinfer_ctx_t *ctx, const char *name);

/* Mark a variable as escaping */
void typeinfer_mark_escape(typeinfer_ctx_t *ctx, const char *name);

/* Initialize type inference context */
void typeinfer_ctx_init(typeinfer_ctx_t *ctx, size_t max_vars, scope *scopes, codegen_t *cg);

/* Dispose type inference context */
void typeinfer_ctx_dispose(typeinfer_ctx_t *ctx);

/* Add a variable to the inference context */
void typeinfer_add_var(typeinfer_ctx_t *ctx, const char *name);

/* Mark a variable as proven i64 */
void typeinfer_mark_i64(typeinfer_ctx_t *ctx, const char *name);

/* Mark a variable as proven f64 */
void typeinfer_mark_f64(typeinfer_ctx_t *ctx, const char *name);

/* Mark a variable as used in dynamic context (needs tags) */
void typeinfer_mark_dynamic(typeinfer_ctx_t *ctx, const char *name);

/* Check if a variable is proven i64 */
bool typeinfer_is_i64(typeinfer_ctx_t *ctx, const char *name);

/* Check if a variable is proven f64 */
bool typeinfer_is_f64(typeinfer_ctx_t *ctx, const char *name);

/* Check if a variable needs dynamic tagging */
bool typeinfer_needs_dynamic(typeinfer_ctx_t *ctx, const char *name);

/* Run type inference on a function body */
void typeinfer_func_body(typeinfer_ctx_t *ctx, stmt_t *body);

/* Walk a statement for type inference (exported for top-level code) */
void typeinfer_walk_stmt(typeinfer_ctx_t *ctx, stmt_t *s);

/* Apply inferred types to scope bindings */
void typeinfer_apply_to_scopes(typeinfer_ctx_t *ctx, scope *scopes, size_t depth);

/* Quick check: is this expression provably i64? */
bool typeinfer_expr_is_i64(typeinfer_ctx_t *ctx, expr_t *e);

/* Quick check: is this expression provably f64? */
bool typeinfer_expr_is_f64(typeinfer_ctx_t *ctx, expr_t *e);

/* Emit a compact JSON summary of the current lightweight type facts. */
char *typeinfer_program_summary_json(program_t *prog, const char *source_name, bool include_std);

#endif
