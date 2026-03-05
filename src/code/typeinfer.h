#ifndef NY_CODE_TYPEINFER_H
#define NY_CODE_TYPEINFER_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

/* Type inference context for tracking proven i64 types */
typedef struct typeinfer_ctx {
  const char **var_names;
  size_t var_names_cap;
  size_t var_names_len;
  bool *is_i64_proven;
  bool *is_f64_proven;
  bool *is_used_in_dynamic;
  scope *scopes;
  size_t func_depth;
  codegen_t *cg;
} typeinfer_ctx_t;

/* Initialize type inference context */
void typeinfer_ctx_init(typeinfer_ctx_t *ctx, size_t max_vars, scope *scopes,
                        codegen_t *cg);

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
void typeinfer_apply_to_scopes(typeinfer_ctx_t *ctx, scope *scopes,
                               size_t depth);

/* Quick check: is this expression provably i64? */
bool typeinfer_expr_is_i64(typeinfer_ctx_t *ctx, expr_t *e);

/* Quick check: is this expression provably f64? */
bool typeinfer_expr_is_f64(typeinfer_ctx_t *ctx, expr_t *e);

#endif
