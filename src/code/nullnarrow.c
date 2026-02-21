#include "nullnarrow.h"
#include "priv.h"
#include <string.h>

static bool ny_expr_is_nil_literal_for_narrow(const expr_t *e) {
  return e && e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
         e->tok.kind == NY_T_NIL;
}

static bool ny_null_narrow_info_empty(const ny_null_narrow_info_t *info) {
  return !info || !info->name || (!info->true_nonnull && !info->false_nonnull);
}

static bool ny_null_narrow_info_add(ny_null_narrow_list_t *list,
                                    const char *name, bool true_nonnull,
                                    bool false_nonnull) {
  if (!list || !name || !*name || (!true_nonnull && !false_nonnull))
    return true;
  for (size_t i = 0; i < list->len; i++) {
    ny_null_narrow_info_t *it = &list->data[i];
    if (it->name && strcmp(it->name, name) == 0) {
      it->true_nonnull = it->true_nonnull || true_nonnull;
      it->false_nonnull = it->false_nonnull || false_nonnull;
      return true;
    }
  }
  ny_null_narrow_info_t item = {name, true_nonnull, false_nonnull};
  vec_push(list, item);
  return true;
}

static void ny_null_narrow_list_free2(ny_null_narrow_list_t *a,
                                      ny_null_narrow_list_t *b) {
  if (a)
    vec_free(a);
  if (b)
    vec_free(b);
}

static bool ny_null_narrow_merge_swapped(ny_null_narrow_list_t *dst,
                                         const ny_null_narrow_list_t *src) {
  if (!dst || !src || src->len == 0)
    return true;
  for (size_t i = 0; i < src->len; i++) {
    const ny_null_narrow_info_t *it = &src->data[i];
    if (ny_null_narrow_info_empty(it))
      continue;
    if (!ny_null_narrow_info_add(dst, it->name, it->false_nonnull,
                                 it->true_nonnull))
      return false;
  }
  return true;
}

static bool ny_null_narrow_merge_selected(ny_null_narrow_list_t *dst,
                                          const ny_null_narrow_list_t *src,
                                          bool select_true, bool select_false,
                                          bool out_true, bool out_false) {
  if (!dst || !src || src->len == 0)
    return true;
  for (size_t i = 0; i < src->len; i++) {
    const ny_null_narrow_info_t *it = &src->data[i];
    if (ny_null_narrow_info_empty(it))
      continue;
    bool keep = (select_true && it->true_nonnull) ||
                (select_false && it->false_nonnull);
    if (!keep)
      continue;
    if (!ny_null_narrow_info_add(dst, it->name, out_true, out_false))
      return false;
  }
  return true;
}

static bool ny_null_narrow_collect_logical(
    expr_t *left_expr, expr_t *right_expr, ny_null_narrow_list_t *out,
    bool select_true_nonnull, bool out_true_nonnull, bool out_false_nonnull);

static bool ny_null_narrow_collect_into(expr_t *test,
                                        ny_null_narrow_list_t *out);

bool ny_null_narrow_list_empty(const ny_null_narrow_list_t *list) {
  return !list || list->len == 0;
}

void ny_null_narrow_list_reset(ny_null_narrow_list_t *list) {
  if (!list)
    return;
  vec_free(list);
  vec_init(list);
}

bool ny_null_narrow_collect(expr_t *test, ny_null_narrow_list_t *out) {
  if (!out)
    return false;
  ny_null_narrow_list_reset(out);
  return ny_null_narrow_collect_into(test, out);
}

static bool ny_null_narrow_collect_into(expr_t *test,
                                        ny_null_narrow_list_t *out) {
  if (!test || !out)
    return false;
  if (test->kind == NY_E_BINARY && test->as.binary.op) {
    bool is_eq = strcmp(test->as.binary.op, "==") == 0;
    bool is_neq = strcmp(test->as.binary.op, "!=") == 0;
    if (!is_eq && !is_neq)
      return false;

    expr_t *left = test->as.binary.left;
    expr_t *right = test->as.binary.right;
    const char *name = NULL;
    if (left && left->kind == NY_E_IDENT &&
        ny_expr_is_nil_literal_for_narrow(right))
      name = left->as.ident.name;
    else if (right && right->kind == NY_E_IDENT &&
             ny_expr_is_nil_literal_for_narrow(left))
      name = right->as.ident.name;
    if (!name || !*name)
      return false;
    return ny_null_narrow_info_add(out, name, is_neq, is_eq);
  }

  if (test->kind == NY_E_UNARY && test->as.unary.op &&
      strcmp(test->as.unary.op, "!") == 0) {
    ny_null_narrow_list_t inner;
    vec_init(&inner);
    if (!ny_null_narrow_collect_into(test->as.unary.right, &inner) ||
        ny_null_narrow_list_empty(&inner)) {
      vec_free(&inner);
      return false;
    }
    if (!ny_null_narrow_merge_swapped(out, &inner)) {
      vec_free(&inner);
      return false;
    }
    vec_free(&inner);
    return !ny_null_narrow_list_empty(out);
  }

  if (test->kind == NY_E_LOGICAL && test->as.logical.op) {
    if (strcmp(test->as.logical.op, "&&") == 0)
      return ny_null_narrow_collect_logical(test->as.logical.left,
                                            test->as.logical.right, out, true,
                                            true, false);
    if (strcmp(test->as.logical.op, "||") == 0)
      return ny_null_narrow_collect_logical(test->as.logical.left,
                                            test->as.logical.right, out, false,
                                            false, true);
  }

  return false;
}

static bool ny_null_narrow_collect_logical(
    expr_t *left_expr, expr_t *right_expr, ny_null_narrow_list_t *out,
    bool select_true_nonnull, bool out_true_nonnull, bool out_false_nonnull) {
  ny_null_narrow_list_t left, right;
  vec_init(&left);
  vec_init(&right);
  bool has_left = ny_null_narrow_collect_into(left_expr, &left) &&
                  !ny_null_narrow_list_empty(&left);
  bool has_right = ny_null_narrow_collect_into(right_expr, &right) &&
                   !ny_null_narrow_list_empty(&right);
  if (!has_left && !has_right) {
    ny_null_narrow_list_free2(&left, &right);
    return false;
  }
  bool ok =
      ny_null_narrow_merge_selected(out, &left, select_true_nonnull && has_left,
                                    (!select_true_nonnull) && has_left,
                                    out_true_nonnull, out_false_nonnull) &&
      ny_null_narrow_merge_selected(out, &right,
                                    select_true_nonnull && has_right,
                                    (!select_true_nonnull) && has_right,
                                    out_true_nonnull, out_false_nonnull);
  ny_null_narrow_list_free2(&left, &right);
  return ok && !ny_null_narrow_list_empty(out);
}

bool ny_null_narrow_collect_logical_rhs(expr_t *left_expr, bool and_op,
                                        ny_null_narrow_list_t *out) {
  if (!out)
    return false;
  ny_null_narrow_list_reset(out);
  ny_null_narrow_list_t info;
  vec_init(&info);
  if (!ny_null_narrow_collect_into(left_expr, &info) ||
      ny_null_narrow_list_empty(&info)) {
    vec_free(&info);
    return false;
  }
  if (!ny_null_narrow_merge_selected(out, &info, and_op, !and_op, true,
                                     false)) {
    vec_free(&info);
    return false;
  }
  vec_free(&info);
  return !ny_null_narrow_list_empty(out);
}

void ny_null_narrow_apply(codegen_t *cg, scope *scopes, size_t depth,
                          const ny_null_narrow_list_t *narrow, bool true_branch,
                          ny_null_narrow_restore_list_t *applied) {
  if (!applied)
    return;
  vec_init(applied);
  if (!narrow || ny_null_narrow_list_empty(narrow))
    return;
  for (size_t i = 0; i < narrow->len; i++) {
    const ny_null_narrow_info_t *it = &narrow->data[i];
    bool enable = true_branch ? it->true_nonnull : it->false_nonnull;
    if (!enable || !it->name || !*it->name)
      continue;
    binding *b = scope_lookup(scopes, depth, it->name);
    if (!b)
      b = lookup_global(cg, it->name);
    if (!b || !b->type_name || b->type_name[0] != '?')
      continue;
    const char *base = b->type_name;
    while (*base == '?')
      base++;
    if (!*base)
      continue;
    ny_null_narrow_restore_t r = {b, b->type_name};
    b->type_name = base;
    vec_push(applied, r);
  }
}

void ny_null_narrow_restore(ny_null_narrow_restore_list_t *applied) {
  if (!applied)
    return;
  for (size_t i = 0; i < applied->len; i++) {
    ny_null_narrow_restore_t *r = &applied->data[i];
    if (r->binding && r->saved_type)
      r->binding->type_name = r->saved_type;
  }
  vec_free(applied);
}
