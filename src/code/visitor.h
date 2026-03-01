#pragma once

#include "ast/ast.h"
#include <stdbool.h>

typedef struct ny_visitor_t ny_visitor_t;

typedef bool (*ny_visit_expr_pre_fn)(ny_visitor_t *v, expr_t *e);
typedef void (*ny_visit_expr_post_fn)(ny_visitor_t *v, expr_t *e);
typedef bool (*ny_visit_stmt_pre_fn)(ny_visitor_t *v, stmt_t *s);
typedef void (*ny_visit_stmt_post_fn)(ny_visitor_t *v, stmt_t *s);

struct ny_visitor_t {
  void *ctx;
  ny_visit_expr_pre_fn visit_expr_pre;
  ny_visit_expr_post_fn visit_expr_post;
  ny_visit_stmt_pre_fn visit_stmt_pre;
  ny_visit_stmt_post_fn visit_stmt_post;
};

void ny_visit_expr(ny_visitor_t *v, expr_t *e);
void ny_visit_stmt(ny_visitor_t *v, stmt_t *s);
