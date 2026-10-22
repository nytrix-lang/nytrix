/*
 * Language-owned AST fallback for x86-64 native emission.
 *
 * x86-64 native encoder: machine-form -> object-code lowering for
 * the x86-64 backend including register allocation and instruction selection.
 * The machine-form and object encoders keep target-specific entry points
 * separate; their shared instruction invariants are checked by native
 * encoding regressions.
 */
#include "base/debug.h"
#include "base/trace.h"
#include "code/backend/internal.h"
#include "code/lang/parse/ast.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

bool nyr_x86_64_emit_rt_main(nyr_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, char *err, size_t err_len);

typedef struct {
  const char *name;
  int offset;
  bool known_const;
  int64_t const_value;
} ny_x64_local_t;

typedef struct {
  nyr_writer_t *w;
  const ny_native_target_info_t *target;
  const program_t *prog;
  ny_x64_local_t locals[256];
  size_t local_count;
  int frame_bytes;
  size_t label_id;
  int suppress_local_consts;
  bool emitted_return;
  bool raw_return;
  char epilogue_label[160];
  char func_label[128];
  char *err;
  size_t err_len;
} ny_x64_ctx_t;

static bool ny_x64_emit_expr_raw(ny_x64_ctx_t *ctx, const expr_t *e);
static bool ny_x64_expr_const(ny_x64_ctx_t *ctx, const expr_t *e, int64_t *out);

static void ny_x64_forget_local_consts(ny_x64_ctx_t *ctx) {
  if (!ctx)
    return;
  for (size_t i = 0; i < ctx->local_count; ++i)
    ctx->locals[i].known_const = false;
}

static bool ny_x64_ignored_top_level_stmt(const stmt_t *s) {
  return !s || s->kind == NY_S_USE || s->kind == NY_S_LINK ||
         s->kind == NY_S_INCLUDE || s->kind == NY_S_DEFINE ||
         s->kind == NY_S_EXPORT || s->kind == NY_S_FUNC ||
         s->kind == NY_S_MODULE;
}

static bool ny_x64_ident_symbol_ok(const char *s) {
  if (!s || !s[0])
    return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9') || *p == '_')
      continue;
    return false;
  }
  return true;
}

static const stmt_t *ny_x64_find_func(const program_t *prog, const char *name) {
  if (!prog || !name)
    return NULL;
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (s && s->kind == NY_S_FUNC && s->as.fn.name &&
        strcmp(s->as.fn.name, name) == 0)
      return s;
  }
  return NULL;
}

static bool ny_x64_func_label(char *buf, size_t buf_len, const char *name,
                              char *err, size_t err_len) {
  if (!ny_x64_ident_symbol_ok(name)) {
    nyr_set_error(
        err, err_len,
        "native x86_64 function name '%s' is not a simple native symbol",
        name ? name : "(null)");
    return false;
  }
  snprintf(buf, buf_len, "ny_fn_%s", name);
  return true;
}

static ny_x64_local_t *ny_x64_find_local(ny_x64_ctx_t *ctx, const char *name) {
  if (!ctx || !name)
    return NULL;
  for (size_t i = ctx->local_count; i > 0; --i) {
    ny_x64_local_t *l = &ctx->locals[i - 1];
    if (l->name && strcmp(l->name, name) == 0)
      return l;
  }
  return NULL;
}

static bool ny_x64_add_local(ny_x64_ctx_t *ctx, const char *name) {
  if (!name || name[0] == '\0' || strcmp(name, "_") == 0)
    return true;
  if (ny_x64_find_local(ctx, name))
    return true;
  if (ctx->local_count >= sizeof(ctx->locals) / sizeof(ctx->locals[0])) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 local limit exceeded");
    return false;
  }
  ctx->local_count++;
  ctx->locals[ctx->local_count - 1].name = name;
  ctx->locals[ctx->local_count - 1].offset = (int)((ctx->local_count + 1) * 8);
  return true;
}

static bool ny_x64_collect_stmt_locals(ny_x64_ctx_t *ctx, const stmt_t *s) {
  if (!s)
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!ny_x64_collect_stmt_locals(ctx, s->as.block.body.data[i]))
        return false;
    }
    return true;
  case NY_S_VAR:
    if (s->as.var.is_del) {
      nyr_set_error(
          ctx->err, ctx->err_len,
          "native x86_64 locals only support simple def/mut bindings");
      return false;
    }
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      if (!ny_x64_add_local(ctx, s->as.var.names.data[i]))
        return false;
    }
    return true;
  default:
    return true;
  }
}

static bool ny_x64_collect_func_locals(ny_x64_ctx_t *ctx, const stmt_t *fn) {
  if (!fn || fn->kind != NY_S_FUNC)
    return false;
  if (fn->as.fn.is_variadic || fn->as.fn.is_extern ||
      fn->as.fn.params.len > ctx->target->gp_arg_reg_count) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 only supports simple non-variadic "
                      "functions with up to %zu integer args",
                      ctx->target->gp_arg_reg_count);
    return false;
  }
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    if (!ny_x64_add_local(ctx, fn->as.fn.params.data[i].name))
      return false;
  }
  if (!ny_x64_collect_stmt_locals(ctx, fn->as.fn.body))
    return false;
  int raw = (int)(ctx->local_count * 8);
  ctx->frame_bytes = (raw + 15) & ~15;
  return true;
}

static bool ny_x64_collect_program_locals(ny_x64_ctx_t *ctx,
                                          const program_t *prog) {
  if (!prog)
    return false;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *s = prog->body.data[i];
    if (ny_x64_ignored_top_level_stmt(s))
      continue;
    if (!ny_x64_collect_stmt_locals(ctx, s))
      return false;
  }
  int raw = (int)(ctx->local_count * 8);
  ctx->frame_bytes = (raw + 15) & ~15;
  return true;
}

static bool ny_x64_tag_rax(ny_x64_ctx_t *ctx) {
  return nyr_put(ctx->w, "\tleaq\t1(,%rax,2), %rax\n");
}

static bool ny_x64_emit_literal_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (e->as.literal.kind != NY_LIT_INT || e->tok.kind == NY_T_NIL) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 integer path only supports int literals");
    return false;
  }
  return nyr_printf(ctx->w, "\tmovabsq\t$%" PRId64 ", %%rax\n",
                    e->as.literal.as.i);
}

static bool ny_x64_emit_const_rax(ny_x64_ctx_t *ctx, int64_t v) {
  return nyr_printf(ctx->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", v);
}

static bool ny_x64_emit_ident_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  ny_x64_local_t *l = ny_x64_find_local(ctx, e->as.ident.name);
  if (!l) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 unknown local '%s'",
                      e->as.ident.name ? e->as.ident.name : "(null)");
    return false;
  }
  if (l->known_const && !ctx->suppress_local_consts)
    return ny_x64_emit_const_rax(ctx, l->const_value);
  return nyr_printf(ctx->w, "\tmovq\t-%d(%%rbp), %%rax\n", l->offset);
}

static bool ny_x64_emit_unary_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (!e->as.unary.op || !e->as.unary.right) {
    nyr_set_error(ctx->err, ctx->err_len, "malformed unary expression");
    return false;
  }
  if (strcmp(e->as.unary.op, "+") == 0)
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right);
  if (strcmp(e->as.unary.op, "-") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           nyr_put(ctx->w, "\tnegq\t%rax\n");
  }
  if (strcmp(e->as.unary.op, "!") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           nyr_put(ctx->w, "\ttestq\t%rax, %rax\n"
                           "\tsete\t%al\n"
                           "\tmovzbq\t%al, %rax\n");
  }
  if (strcmp(e->as.unary.op, "~") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           nyr_put(ctx->w, "\tnotq\t%rax\n");
  }
  nyr_set_error(ctx->err, ctx->err_len,
                    "unsupported native unary operator '%s'", e->as.unary.op);
  return false;
}

static bool ny_x64_emit_binary_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  const char *op = e->as.binary.op;
  int64_t folded = 0;
  if (ny_x64_expr_const(ctx, e, &folded))
    return ny_x64_emit_const_rax(ctx, folded);
  if (!op || !e->as.binary.left || !e->as.binary.right) {
    nyr_set_error(ctx->err, ctx->err_len, "malformed binary expression");
    return false;
  }
  if (!ny_x64_emit_expr_raw(ctx, e->as.binary.right) ||
      !nyr_put(ctx->w, "\tpushq\t%rax\n") ||
      !ny_x64_emit_expr_raw(ctx, e->as.binary.left) ||
      !nyr_put(ctx->w, "\tpopq\t%r10\n"))
    return false;

  if (strcmp(op, "+") == 0)
    return nyr_put(ctx->w, "\taddq\t%r10, %rax\n");
  if (strcmp(op, "-") == 0)
    return nyr_put(ctx->w, "\tsubq\t%r10, %rax\n");
  if (strcmp(op, "*") == 0)
    return nyr_put(ctx->w, "\timulq\t%r10, %rax\n");
  if (strcmp(op, "/") == 0)
    return nyr_put(ctx->w, "\tcqto\n\tidivq\t%r10\n");
  if (strcmp(op, "%") == 0)
    return nyr_put(ctx->w, "\tcqto\n\tidivq\t%r10\n\tmovq\t%rdx, %rax\n");
  if (strcmp(op, "&") == 0)
    return nyr_put(ctx->w, "\tandq\t%r10, %rax\n");
  if (strcmp(op, "|") == 0)
    return nyr_put(ctx->w, "\torq\t%r10, %rax\n");
  if (strcmp(op, "^") == 0)
    return nyr_put(ctx->w, "\txorq\t%r10, %rax\n");
  if (strcmp(op, "<<") == 0)
    return nyr_put(ctx->w, "\tmovb\t%r10b, %cl\n\tshlq\t%cl, %rax\n");
  if (strcmp(op, ">>") == 0)
    return nyr_put(ctx->w, "\tmovb\t%r10b, %cl\n\tsarq\t%cl, %rax\n");
  if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 ||
      strcmp(op, ">=") == 0 || strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    const char *setcc = strcmp(op, "<") == 0    ? "setl"
                        : strcmp(op, "<=") == 0 ? "setle"
                        : strcmp(op, ">") == 0  ? "setg"
                        : strcmp(op, ">=") == 0 ? "setge"
                        : strcmp(op, "==") == 0 ? "sete"
                                                : "setne";
    return nyr_printf(
        ctx->w, "\tcmpq\t%%r10, %%rax\n\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
        setcc);
  }

  nyr_set_error(ctx->err, ctx->err_len,
                    "unsupported native binary operator '%s'", op);
  return false;
}

static bool ny_x64_expr_const(ny_x64_ctx_t *ctx, const expr_t *e,
                              int64_t *out) {
  if (!e || !out)
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind != NY_LIT_INT || e->tok.kind == NY_T_NIL)
      return false;
    *out = e->as.literal.as.i;
    return true;
  case NY_E_IDENT: {
    ny_x64_local_t *l = ny_x64_find_local(ctx, e->as.ident.name);
    if (!l || !l->known_const || ctx->suppress_local_consts)
      return false;
    *out = l->const_value;
    return true;
  }
  case NY_E_UNARY: {
    int64_t rv = 0;
    if (!e->as.unary.op || !ny_x64_expr_const(ctx, e->as.unary.right, &rv))
      return false;
    if (strcmp(e->as.unary.op, "+") == 0) {
      *out = rv;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0) {
      *out = -rv;
      return true;
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      *out = rv == 0;
      return true;
    }
    if (strcmp(e->as.unary.op, "~") == 0) {
      *out = ~rv;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    int64_t a = 0, b = 0;
    const char *op = e->as.binary.op;
    if (!op || !ny_x64_expr_const(ctx, e->as.binary.left, &a) ||
        !ny_x64_expr_const(ctx, e->as.binary.right, &b))
      return false;
    if (strcmp(op, "+") == 0)
      *out = a + b;
    else if (strcmp(op, "-") == 0)
      *out = a - b;
    else if (strcmp(op, "*") == 0)
      *out = a * b;
    else if (strcmp(op, "/") == 0) {
      if (b == 0)
        return false;
      *out = a / b;
    } else if (strcmp(op, "%") == 0) {
      if (b == 0)
        return false;
      *out = a % b;
    } else if (strcmp(op, "&") == 0)
      *out = a & b;
    else if (strcmp(op, "|") == 0)
      *out = a | b;
    else if (strcmp(op, "^") == 0)
      *out = a ^ b;
    else if (strcmp(op, "<<") == 0) {
      if (b < 0 || b >= 63)
        return false;
      *out = a << b;
    } else if (strcmp(op, ">>") == 0) {
      if (b < 0 || b >= 63)
        return false;
      *out = a >> b;
    } else if (strcmp(op, "<") == 0)
      *out = a < b;
    else if (strcmp(op, "<=") == 0)
      *out = a <= b;
    else if (strcmp(op, ">") == 0)
      *out = a > b;
    else if (strcmp(op, ">=") == 0)
      *out = a >= b;
    else if (strcmp(op, "==") == 0)
      *out = a == b;
    else if (strcmp(op, "!=") == 0)
      *out = a != b;
    else
      return false;
    return true;
  }
  default:
    return false;
  }
}

static bool ny_x64_emit_call_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (!e->as.call.callee || e->as.call.callee->kind != NY_E_IDENT) {
    nyr_set_error(
        ctx->err, ctx->err_len,
        "native x86_64 only supports direct calls to local functions");
    return false;
  }
  const char *name = e->as.call.callee->as.ident.name;
  const stmt_t *fn = ny_x64_find_func(ctx->prog, name);
  if (!fn) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 unknown function '%s'",
                      name ? name : "(null)");
    return false;
  }
  size_t argc = e->as.call.args.len;
  const call_arg_t *ordered[32] = {0};
  call_arg_t defaults[32] = {0};
  bool has_named = false;
  for (size_t i = 0; i < argc; ++i)
    has_named = has_named || e->as.call.args.data[i].name != NULL;
  if (has_named) {
    if (fn->as.fn.params.len > 32) {
      nyr_set_error(ctx->err, ctx->err_len,
                        "native x86_64 named call has too many parameters");
      return false;
    }
    bool assigned[32] = {0};
    size_t next_pos = 0;
    for (size_t i = 0; i < argc; ++i) {
      const call_arg_t *arg = &e->as.call.args.data[i];
      size_t idx = next_pos;
      if (arg->name) {
        idx = fn->as.fn.params.len;
        for (size_t j = 0; j < fn->as.fn.params.len; ++j) {
          if (fn->as.fn.params.data[j].name &&
              strcmp(fn->as.fn.params.data[j].name, arg->name) == 0) {
            idx = j;
            break;
          }
        }
        if (idx == fn->as.fn.params.len) {
          nyr_set_error(ctx->err, ctx->err_len,
                            "native x86_64 unknown named argument '%s'",
                            arg->name);
          return false;
        }
      } else {
        while (idx < fn->as.fn.params.len && assigned[idx])
          ++idx;
        if (idx == fn->as.fn.params.len) {
          nyr_set_error(ctx->err, ctx->err_len,
                            "native x86_64 call has too many arguments");
          return false;
        }
        next_pos = idx + 1;
      }
      if (assigned[idx]) {
        nyr_set_error(
            ctx->err, ctx->err_len, "native x86_64 duplicate argument '%s'",
            fn->as.fn.params.data[idx].name ? fn->as.fn.params.data[idx].name
                                            : "<positional>");
        return false;
      }
      ordered[idx] = arg;
      assigned[idx] = true;
    }
    for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
      if (ordered[i])
        continue;
      defaults[i].val = fn->as.fn.params.data[i].def;
      ordered[i] = &defaults[i];
    }
    argc = fn->as.fn.params.len;
  } else {
    for (size_t i = 0; i < argc; ++i)
      ordered[i] = &e->as.call.args.data[i];
  }
  if (argc != fn->as.fn.params.len) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 call '%s' expects %zu args, got %zu", name,
                      fn->as.fn.params.len, argc);
    return false;
  }
  if (argc > ctx->target->gp_arg_reg_count) {
    nyr_set_error(ctx->err, ctx->err_len,
                      "native x86_64 call '%s' has too many integer args",
                      name);
    return false;
  }
  for (size_t i = 0; i < argc; ++i) {
    if (!ordered[i]->val) {
      if (!nyr_put(ctx->w, "\txorq\t%rax, %rax\n"))
        return false;
    } else if (!ny_x64_emit_expr_raw(ctx, ordered[i]->val)) {
      return false;
    }
    if (!nyr_put(ctx->w, "\tpushq\t%rax\n"))
      return false;
  }
  for (size_t i = argc; i > 0; --i) {
    if (!nyr_printf(ctx->w, "\tpopq\t%s\n", ctx->target->gp_arg_regs[i - 1]))
      return false;
  }
  char label[128];
  if (!ny_x64_func_label(label, sizeof(label), name, ctx->err, ctx->err_len))
    return false;
  if (ctx->target->shadow_space_bytes > 0 &&
      !nyr_printf(ctx->w, "\tsubq\t$%zu, %%rsp\n",
                  ctx->target->shadow_space_bytes))
    return false;
  if (!nyr_printf(ctx->w, "\tcall\t%s%s\n", ctx->target->symbol_prefix, label))
    return false;
  if (ctx->target->shadow_space_bytes > 0 &&
      !nyr_printf(ctx->w, "\taddq\t$%zu, %%rsp\n",
                  ctx->target->shadow_space_bytes))
    return false;
  return true;
}

static bool ny_x64_emit_expr_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (!e) {
    nyr_set_error(ctx->err, ctx->err_len, "missing expression");
    return false;
  }
  int64_t folded = 0;
  if (ny_x64_expr_const(ctx, e, &folded))
    return ny_x64_emit_const_rax(ctx, folded);
  switch (e->kind) {
  case NY_E_IDENT:
    return ny_x64_emit_ident_raw(ctx, e);
  case NY_E_LITERAL:
    return ny_x64_emit_literal_raw(ctx, e);
  case NY_E_UNARY:
    return ny_x64_emit_unary_raw(ctx, e);
  case NY_E_BINARY:
    return ny_x64_emit_binary_raw(ctx, e);
  case NY_E_CALL:
    return ny_x64_emit_call_raw(ctx, e);
  default:
    nyr_set_error(
        ctx->err, ctx->err_len,
        "native x86_64 debug path does not lower expression kind %d yet",
        (int)e->kind);
    return false;
  }
}

static bool ny_x64_emit_stmt(ny_x64_ctx_t *ctx, const stmt_t *s);

static bool ny_x64_emit_branch_false(ny_x64_ctx_t *ctx, const expr_t *test,
                                     const char *false_label) {
  if (!test) {
    nyr_set_error(ctx->err, ctx->err_len, "missing branch condition");
    return false;
  }
  if (test->kind == NY_E_BINARY && test->as.binary.op &&
      (strcmp(test->as.binary.op, "<") == 0 ||
       strcmp(test->as.binary.op, "<=") == 0 ||
       strcmp(test->as.binary.op, ">") == 0 ||
       strcmp(test->as.binary.op, ">=") == 0 ||
       strcmp(test->as.binary.op, "==") == 0 ||
       strcmp(test->as.binary.op, "!=") == 0)) {
    const char *op = test->as.binary.op;
    const char *jfalse = strcmp(op, "<") == 0    ? "jge"
                         : strcmp(op, "<=") == 0 ? "jg"
                         : strcmp(op, ">") == 0  ? "jle"
                         : strcmp(op, ">=") == 0 ? "jl"
                         : strcmp(op, "==") == 0 ? "jne"
                                                 : "je";
    return ny_x64_emit_expr_raw(ctx, test->as.binary.left) &&
           nyr_put(ctx->w, "\tpushq\t%rax\n") &&
           ny_x64_emit_expr_raw(ctx, test->as.binary.right) &&
           nyr_put(ctx->w, "\tmovq\t%rax, %r10\n") &&
           nyr_put(ctx->w, "\tpopq\t%rax\n") &&
           nyr_put(ctx->w, "\tcmpq\t%r10, %rax\n") &&
           nyr_printf(ctx->w, "\t%s\t%s\n", jfalse, false_label);
  }
  return ny_x64_emit_expr_raw(ctx, test) &&
         nyr_put(ctx->w, "\ttestq\t%rax, %rax\n") &&
         nyr_printf(ctx->w, "\tje\t%s\n", false_label);
}

static bool ny_x64_emit_var(ny_x64_ctx_t *ctx, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_del) {
    nyr_set_error(
        ctx->err, ctx->err_len,
        "native x86_64 locals only support simple def/mut bindings");
    return false;
  }
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i]) {
      nyr_set_error(ctx->err, ctx->err_len,
                        "native x86_64 local '%s' needs an initializer", name);
      return false;
    }
    ny_x64_local_t *l = ny_x64_find_local(ctx, name);
    if (!l) {
      nyr_set_error(ctx->err, ctx->err_len,
                        "native x86_64 local planner missed '%s'", name);
      return false;
    }
    if (!ny_x64_emit_expr_raw(ctx, v->exprs.data[i]) ||
        !nyr_printf(ctx->w, "\tmovq\t%%rax, -%d(%%rbp)\n", l->offset))
      return false;
    int64_t cv = 0;
    if (ny_x64_expr_const(ctx, v->exprs.data[i], &cv)) {
      l->known_const = true;
      l->const_value = cv;
    } else {
      l->known_const = false;
    }
  }
  return true;
}

static bool ny_x64_emit_return(ny_x64_ctx_t *ctx, const stmt_t *s) {
  if (s->as.ret.value) {
    if (!ny_x64_emit_expr_raw(ctx, s->as.ret.value))
      return false;
  } else if (!nyr_put(ctx->w, "\txorq\t%rax, %rax\n")) {
    return false;
  }
  ctx->emitted_return = true;
  return nyr_printf(ctx->w, "\tjmp\t%s\n", ctx->epilogue_label);
}

static bool ny_x64_emit_if(ny_x64_ctx_t *ctx, const stmt_t *s) {
  char else_label[64];
  char end_label[64];
  size_t id = ctx->label_id++;
  int64_t cv = 0;
  if (s->as.iff.init && !ny_x64_emit_stmt(ctx, s->as.iff.init))
    return false;
  if (ny_x64_expr_const(ctx, s->as.iff.test, &cv)) {
    if (cv)
      return ny_x64_emit_stmt(ctx, s->as.iff.conseq);
    return s->as.iff.alt ? ny_x64_emit_stmt(ctx, s->as.iff.alt) : true;
  }
  snprintf(else_label, sizeof(else_label), ".Lny_if_else_%zu", id);
  snprintf(end_label, sizeof(end_label), ".Lny_if_end_%zu", id);
  if (!ny_x64_emit_branch_false(ctx, s->as.iff.test, else_label))
    return false;
  if (!ny_x64_emit_stmt(ctx, s->as.iff.conseq))
    return false;
  if (!nyr_printf(ctx->w, "\tjmp\t%s\n%s:\n", end_label, else_label))
    return false;
  if (s->as.iff.alt && !ny_x64_emit_stmt(ctx, s->as.iff.alt))
    return false;
  ny_x64_forget_local_consts(ctx);
  return nyr_printf(ctx->w, "%s:\n", end_label);
}

static bool ny_x64_emit_while(ny_x64_ctx_t *ctx, const stmt_t *s) {
  char head_label[64];
  char end_label[64];
  size_t id = ctx->label_id++;
  int64_t cv = 0;
  if (s->as.whl.init && !ny_x64_emit_stmt(ctx, s->as.whl.init))
    return false;
  if (ny_x64_expr_const(ctx, s->as.whl.test, &cv) && !cv)
    return true;
  snprintf(head_label, sizeof(head_label), ".Lny_while_head_%zu", id);
  snprintf(end_label, sizeof(end_label), ".Lny_while_end_%zu", id);
  if (!nyr_printf(ctx->w, "%s:\n", head_label))
    return false;
  ctx->suppress_local_consts++;
  if (!ny_x64_emit_branch_false(ctx, s->as.whl.test, end_label))
    return false;
  if (!ny_x64_emit_stmt(ctx, s->as.whl.body))
    return false;
  if (s->as.whl.update && !ny_x64_emit_stmt(ctx, s->as.whl.update))
    return false;
  ctx->suppress_local_consts--;
  ny_x64_forget_local_consts(ctx);
  return nyr_printf(ctx->w, "\tjmp\t%s\n%s:\n", head_label, end_label);
}

static bool ny_x64_emit_stmt(ny_x64_ctx_t *ctx, const stmt_t *s) {
  if (ny_x64_ignored_top_level_stmt(s))
    return true;
  switch (s->kind) {
  case NY_S_BLOCK:
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      if (!ny_x64_emit_stmt(ctx, s->as.block.body.data[i]))
        return false;
      if (ctx->emitted_return)
        break;
    }
    return true;
  case NY_S_VAR:
    return ny_x64_emit_var(ctx, s);
  case NY_S_EXPR:
    return ny_x64_emit_expr_raw(ctx, s->as.expr.expr);
  case NY_S_IF:
    return ny_x64_emit_if(ctx, s);
  case NY_S_WHILE:
    return ny_x64_emit_while(ctx, s);
  case NY_S_RETURN:
    return ny_x64_emit_return(ctx, s);
  default:
    nyr_set_error(
        ctx->err, ctx->err_len,
        "native x86_64 debug path does not lower statement kind %d yet",
        (int)s->kind);
    return false;
  }
}

static bool ny_x64_emit_prologue(ny_x64_ctx_t *ctx) {
  return nyr_put(ctx->w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n") &&
         (ctx->frame_bytes <= 0 ||
          nyr_printf(ctx->w, "\tsubq\t$%d, %%rsp\n", ctx->frame_bytes)) &&
         nyr_put(ctx->w, "\txorq\t%rax, %rax\n");
}

static bool ny_x64_emit_epilogue(ny_x64_ctx_t *ctx) {
  if (!nyr_printf(ctx->w, "%s:\n", ctx->epilogue_label))
    return false;
  if (!ctx->raw_return && !ny_x64_tag_rax(ctx))
    return false;
  return nyr_put(ctx->w, "\tleave\n\tret\n");
}

static bool ny_x64_emit_func(nyr_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, const stmt_t *fn, char *err,
                             size_t err_len) {
  char label[128];
  if (!ny_x64_func_label(label, sizeof(label), fn->as.fn.name, err, err_len))
    return false;
  ny_x64_ctx_t ctx = {.w = w,
                      .target = target,
                      .prog = prog,
                      .raw_return = true,
                      .err = err,
                      .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label), ".L%s_done", label);
  if (!ny_x64_collect_func_locals(&ctx, fn))
    return false;
  if (strcmp(target->object_format, "macho") == 0) {
    if (!nyr_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else if (!nyr_printf(w, "\t.type\t%s%s,@function\n", target->symbol_prefix,
                         label)) {
    return false;
  }
  if (!nyr_printf(w, "\t.globl\t%s%s\n%s%s:\n", target->symbol_prefix, label,
                  target->symbol_prefix, label))
    return false;
  if (!ny_x64_emit_prologue(&ctx))
    return false;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    ny_x64_local_t *l = ny_x64_find_local(&ctx, fn->as.fn.params.data[i].name);
    if (!l) {
      nyr_set_error(err, err_len,
                        "native x86_64 parameter planner missed '%s'",
                        fn->as.fn.params.data[i].name);
      return false;
    }
    if (!nyr_printf(w, "\tmovq\t%s, -%d(%%rbp)\n", target->gp_arg_regs[i],
                    l->offset))
      return false;
  }
  if (!ny_x64_emit_stmt(&ctx, fn->as.fn.body))
    return false;
  if (!ny_x64_emit_epilogue(&ctx))
    return false;
  if (strcmp(target->object_format, "macho") != 0) {
    if (!nyr_printf(w, "\t.size\t%s%s, .-%s%s\n", target->symbol_prefix, label,
                    target->symbol_prefix, label))
      return false;
  }
  return true;
}

static bool ny_x64_emit_all_funcs(nyr_writer_t *w,
                                  const ny_native_target_info_t *target,
                                  const program_t *prog, char *err,
                                  size_t err_len) {
  for (size_t i = 0; i < prog->body.len; ++i) {
    const stmt_t *s = prog->body.data[i];
    if (!s || s->kind != NY_S_FUNC)
      continue;
    if (!ny_x64_emit_func(w, target, prog, s, err, err_len))
      return false;
  }
  return true;
}

bool nyr_x86_64_emit_rt_main(nyr_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, char *err, size_t err_len) {
  const char *sym = target->symbol_prefix;
  ny_x64_ctx_t ctx = {
      .w = w, .target = target, .prog = prog, .err = err, .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label), ".Lny_rt_main_done");
  if (!ny_x64_collect_program_locals(&ctx, prog))
    return false;

  if (!nyr_put(w, "# Nytrix native x86-64 backend output\n"))
    return false;
  if (!nyr_printf(w,
                  "# target=%s abi=%s object=%s red_zone=%s shadow_space=%zu\n",
                  target->target_name, target->abi_name, target->object_format,
                  target->red_zone ? "yes" : "no", target->shadow_space_bytes))
    return false;
  if (!nyr_put(w, "\t.text\n"))
    return false;
  if (!ny_x64_emit_all_funcs(w, target, prog, err, err_len))
    return false;
  if (strcmp(target->object_format, "macho") == 0) {
    if (!nyr_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else {
    if (!nyr_printf(w, "\t.type\t%srt_main,@function\n", sym))
      return false;
  }
  if (!nyr_printf(w, "\t.globl\t%srt_main\n%srt_main:\n", sym, sym))
    return false;
  if (!ny_x64_emit_prologue(&ctx))
    return false;
  for (size_t i = 0; i < prog->body.len; ++i) {
    if (!ny_x64_emit_stmt(&ctx, prog->body.data[i]))
      return false;
    if (ctx.emitted_return)
      break;
  }
  if (!ctx.emitted_return && prog->body.len == 0) {
    nyr_set_error(
        err, err_len,
        "native x86_64 debug path needs a result expression or return");
    return false;
  }
  if (!ny_x64_emit_epilogue(&ctx))
    return false;
  if (strcmp(target->object_format, "macho") != 0) {
    if (!nyr_printf(w, "\t.size\t%srt_main, .-%srt_main\n", sym, sym))
      return false;
  }
  return true;
}
