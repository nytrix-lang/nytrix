/*
 * Ownership tracking module: handles borrow checking, move semantics,
 * drop deferrals, and lifetime diagnostics for Nytrix codegen.
 */
#ifndef NYTRIX_OWNERSHIP_H
#define NYTRIX_OWNERSHIP_H

#include "priv.h"

void stmt_ownership_check_live_borrows(codegen_t *cg, scope *scopes,
                                       size_t depth, binding *source,
                                       token_t tok, const char *action);
void stmt_ownership_check_returned_borrow(codegen_t *cg, scope *scopes,
                                          size_t depth, expr_t *e);
void stmt_ownership_apply_call_contracts(codegen_t *cg, scope *scopes,
                                         size_t depth, expr_t *call_expr);
void stmt_ownership_pre_store(codegen_t *cg, scope *scopes, size_t depth,
                              binding *dest, expr_t *rhs, token_t tok);
void stmt_ownership_post_store(codegen_t *cg, scope *scopes, size_t depth,
                               binding *dest, expr_t *rhs, token_t tok,
                               bool target_global);
binding *stmt_ownership_begin_return_transfer(codegen_t *cg, scope *scopes,
                                              size_t depth, expr_t *value,
                                              ny_owner_state_t *old_state);
void stmt_ownership_end_return_transfer(binding *b,
                                        ny_owner_state_t old_state);
void stmt_ownership_cleanup_scope(codegen_t *cg, scope *scopes, size_t depth);
void stmt_ownership_register_slot_defer(codegen_t *cg, scope *scopes,
                                        size_t depth, binding *b);
void stmt_ownership_diag(codegen_t *cg, token_t tok, const char *fmt, ...);
void stmt_ownership_release_source(codegen_t *cg, scope *scopes,
                                   size_t depth, expr_t *arg,
                                   bool forgotten);
void stmt_ownership_warn_use_after_move(codegen_t *cg, scope *scopes,
                                        size_t depth, expr_t *e);
expr_t *stmt_ownership_releases_arg(codegen_t *cg, expr_t *call_expr);
expr_t *stmt_ownership_forgets_arg(codegen_t *cg, expr_t *call_expr);

bool stmt_expr_is_mutating_name(const char *name);
bool stmt_expr_is_int_list_literal(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
bool stmt_expr_int_range(codegen_t *cg, scope *scopes, size_t depth, expr_t *e, int64_t *out_min, int64_t *out_max);

#endif /* NYTRIX_OWNERSHIP_H */
