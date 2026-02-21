#ifndef NY_CODE_NULLNARROW_H
#define NY_CODE_NULLNARROW_H

#include "code/types.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct ny_null_narrow_info_t {
  const char *name;
  bool true_nonnull;
  bool false_nonnull;
} ny_null_narrow_info_t;

typedef VEC(ny_null_narrow_info_t) ny_null_narrow_list_t;

typedef struct ny_null_narrow_restore_t {
  binding *binding;
  const char *saved_type;
} ny_null_narrow_restore_t;

typedef VEC(ny_null_narrow_restore_t) ny_null_narrow_restore_list_t;

bool ny_null_narrow_list_empty(const ny_null_narrow_list_t *list);
void ny_null_narrow_list_reset(ny_null_narrow_list_t *list);

// Collect branch-sensitive null narrowing facts from a condition expression.
bool ny_null_narrow_collect(expr_t *test, ny_null_narrow_list_t *out);

// Collect narrowing facts that are valid when evaluating logical RHS.
bool ny_null_narrow_collect_logical_rhs(expr_t *left_expr, bool and_op,
                                        ny_null_narrow_list_t *out);

// Apply/restore branch-local narrowing to currently visible bindings.
void ny_null_narrow_apply(codegen_t *cg, scope *scopes, size_t depth,
                          const ny_null_narrow_list_t *narrow, bool true_branch,
                          ny_null_narrow_restore_list_t *applied);
void ny_null_narrow_restore(ny_null_narrow_restore_list_t *applied);

#endif // NY_CODE_NULLNARROW_H
