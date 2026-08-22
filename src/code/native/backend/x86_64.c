/*
 * x86-64 native encoder: machine-form -> object-code lowering for
 * the x86-64 backend including register allocation and instruction selection.
 * The machine-form and object encoders keep target-specific entry points
 * separate; their shared instruction invariants are checked by native
 * encoding regressions.
 */
#include "code/native/internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  const char *name;
  int offset;
  bool known_const;
  int64_t const_value;
} ny_x64_local_t;

typedef struct {
  ny_native_writer_t *w;
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
    ny_native_set_err(err, err_len,
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
    ny_native_set_err(ctx->err, ctx->err_len,
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
      ny_native_set_err(ctx->err, ctx->err_len,
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
  if (fn->as.fn.is_variadic || fn->as.fn.is_extern || fn->as.fn.params.len >
                                                    ctx->target->gp_arg_reg_count) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 only supports simple non-variadic functions with up to %zu integer args",
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
  return ny_native_put(ctx->w, "\tleaq\t1(,%rax,2), %rax\n");
}

static bool ny_x64_emit_literal_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (e->as.literal.kind != NY_LIT_INT || e->tok.kind == NY_T_NIL) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 integer path only supports int literals");
    return false;
  }
  return ny_native_printf(ctx->w, "\tmovabsq\t$%" PRId64 ", %%rax\n",
                          e->as.literal.as.i);
}

static bool ny_x64_emit_const_rax(ny_x64_ctx_t *ctx, int64_t v) {
  return ny_native_printf(ctx->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", v);
}

static bool ny_x64_emit_ident_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  ny_x64_local_t *l = ny_x64_find_local(ctx, e->as.ident.name);
  if (!l) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 unknown local '%s'",
                      e->as.ident.name ? e->as.ident.name : "(null)");
    return false;
  }
  if (l->known_const && !ctx->suppress_local_consts)
    return ny_x64_emit_const_rax(ctx, l->const_value);
  return ny_native_printf(ctx->w, "\tmovq\t-%d(%%rbp), %%rax\n", l->offset);
}

static bool ny_x64_emit_unary_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (!e->as.unary.op || !e->as.unary.right) {
    ny_native_set_err(ctx->err, ctx->err_len, "malformed unary expression");
    return false;
  }
  if (strcmp(e->as.unary.op, "+") == 0)
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right);
  if (strcmp(e->as.unary.op, "-") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           ny_native_put(ctx->w, "\tnegq\t%rax\n");
  }
  if (strcmp(e->as.unary.op, "!") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           ny_native_put(ctx->w,
                         "\ttestq\t%rax, %rax\n"
                         "\tsete\t%al\n"
                         "\tmovzbq\t%al, %rax\n");
  }
  if (strcmp(e->as.unary.op, "~") == 0) {
    return ny_x64_emit_expr_raw(ctx, e->as.unary.right) &&
           ny_native_put(ctx->w, "\tnotq\t%rax\n");
  }
  ny_native_set_err(ctx->err, ctx->err_len, "unsupported native unary operator '%s'",
                    e->as.unary.op);
  return false;
}

static bool ny_x64_emit_binary_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  const char *op = e->as.binary.op;
  int64_t folded = 0;
  if (ny_x64_expr_const(ctx, e, &folded))
    return ny_x64_emit_const_rax(ctx, folded);
  if (!op || !e->as.binary.left || !e->as.binary.right) {
    ny_native_set_err(ctx->err, ctx->err_len, "malformed binary expression");
    return false;
  }
  if (!ny_x64_emit_expr_raw(ctx, e->as.binary.right) ||
      !ny_native_put(ctx->w, "\tpushq\t%rax\n") ||
      !ny_x64_emit_expr_raw(ctx, e->as.binary.left) ||
      !ny_native_put(ctx->w, "\tpopq\t%r10\n"))
    return false;

  if (strcmp(op, "+") == 0)
    return ny_native_put(ctx->w, "\taddq\t%r10, %rax\n");
  if (strcmp(op, "-") == 0)
    return ny_native_put(ctx->w, "\tsubq\t%r10, %rax\n");
  if (strcmp(op, "*") == 0)
    return ny_native_put(ctx->w, "\timulq\t%r10, %rax\n");
  if (strcmp(op, "/") == 0)
    return ny_native_put(ctx->w, "\tcqto\n\tidivq\t%r10\n");
  if (strcmp(op, "%") == 0)
    return ny_native_put(ctx->w,
                         "\tcqto\n\tidivq\t%r10\n\tmovq\t%rdx, %rax\n");
  if (strcmp(op, "&") == 0)
    return ny_native_put(ctx->w, "\tandq\t%r10, %rax\n");
  if (strcmp(op, "|") == 0)
    return ny_native_put(ctx->w, "\torq\t%r10, %rax\n");
  if (strcmp(op, "^^") == 0)
    return ny_native_put(ctx->w, "\txorq\t%r10, %rax\n");
  if (strcmp(op, "<<") == 0)
    return ny_native_put(ctx->w, "\tmovb\t%r10b, %cl\n\tshlq\t%cl, %rax\n");
  if (strcmp(op, ">>") == 0)
    return ny_native_put(ctx->w, "\tmovb\t%r10b, %cl\n\tsarq\t%cl, %rax\n");
  if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
      strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
      strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
    const char *setcc = strcmp(op, "<") == 0    ? "setl"
                        : strcmp(op, "<=") == 0 ? "setle"
                        : strcmp(op, ">") == 0  ? "setg"
                        : strcmp(op, ">=") == 0 ? "setge"
                        : strcmp(op, "==") == 0 ? "sete"
                                                 : "setne";
    return ny_native_printf(ctx->w,
                            "\tcmpq\t%%r10, %%rax\n\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                            setcc);
  }

  ny_native_set_err(ctx->err, ctx->err_len,
                    "unsupported native binary operator '%s'", op);
  return false;
}

static bool ny_x64_expr_const(ny_x64_ctx_t *ctx, const expr_t *e, int64_t *out) {
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
    else if (strcmp(op, "^^") == 0)
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
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 only supports direct calls to local functions");
    return false;
  }
  const char *name = e->as.call.callee->as.ident.name;
  const stmt_t *fn = ny_x64_find_func(ctx->prog, name);
  if (!fn) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 unknown function '%s'",
                      name ? name : "(null)");
    return false;
  }
  if (e->as.call.args.len != fn->as.fn.params.len) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 call '%s' expects %zu args, got %zu",
                      name, fn->as.fn.params.len, e->as.call.args.len);
    return false;
  }
  if (e->as.call.args.len > ctx->target->gp_arg_reg_count) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 call '%s' has too many integer args", name);
    return false;
  }
  for (size_t i = 0; i < e->as.call.args.len; ++i) {
    if (e->as.call.args.data[i].name) {
      ny_native_set_err(ctx->err, ctx->err_len,
                        "native x86_64 calls do not support named args yet");
      return false;
    }
    if (!ny_x64_emit_expr_raw(ctx, e->as.call.args.data[i].val) ||
        !ny_native_put(ctx->w, "\tpushq\t%rax\n"))
      return false;
  }
  for (size_t i = e->as.call.args.len; i > 0; --i) {
    if (!ny_native_printf(ctx->w, "\tpopq\t%s\n",
                          ctx->target->gp_arg_regs[i - 1]))
      return false;
  }
  char label[128];
  if (!ny_x64_func_label(label, sizeof(label), name, ctx->err, ctx->err_len))
    return false;
  if (ctx->target->shadow_space_bytes > 0 &&
      !ny_native_printf(ctx->w, "\tsubq\t$%zu, %%rsp\n",
                        ctx->target->shadow_space_bytes))
    return false;
  if (!ny_native_printf(ctx->w, "\tcall\t%s%s\n", ctx->target->symbol_prefix,
                        label))
    return false;
  if (ctx->target->shadow_space_bytes > 0 &&
      !ny_native_printf(ctx->w, "\taddq\t$%zu, %%rsp\n",
                        ctx->target->shadow_space_bytes))
    return false;
  return true;
}

static bool ny_x64_emit_expr_raw(ny_x64_ctx_t *ctx, const expr_t *e) {
  if (!e) {
    ny_native_set_err(ctx->err, ctx->err_len, "missing expression");
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
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 debug path does not lower expression kind %d yet",
                      (int)e->kind);
    return false;
  }
}

static bool ny_x64_emit_stmt(ny_x64_ctx_t *ctx, const stmt_t *s);

static bool ny_x64_emit_branch_false(ny_x64_ctx_t *ctx, const expr_t *test,
                                     const char *false_label) {
  if (!test) {
    ny_native_set_err(ctx->err, ctx->err_len, "missing branch condition");
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
           ny_native_put(ctx->w, "\tpushq\t%rax\n") &&
           ny_x64_emit_expr_raw(ctx, test->as.binary.right) &&
           ny_native_put(ctx->w, "\tmovq\t%rax, %r10\n") &&
           ny_native_put(ctx->w, "\tpopq\t%rax\n") &&
           ny_native_put(ctx->w, "\tcmpq\t%r10, %rax\n") &&
           ny_native_printf(ctx->w, "\t%s\t%s\n", jfalse, false_label);
  }
  return ny_x64_emit_expr_raw(ctx, test) &&
         ny_native_put(ctx->w, "\ttestq\t%rax, %rax\n") &&
         ny_native_printf(ctx->w, "\tje\t%s\n", false_label);
}

static bool ny_x64_emit_var(ny_x64_ctx_t *ctx, const stmt_t *s) {
  const stmt_var_t *v = &s->as.var;
  if (v->is_del) {
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 locals only support simple def/mut bindings");
    return false;
  }
  for (size_t i = 0; i < v->names.len; ++i) {
    const char *name = v->names.data[i];
    if (!name || strcmp(name, "_") == 0)
      continue;
    if (i >= v->exprs.len || !v->exprs.data[i]) {
      ny_native_set_err(ctx->err, ctx->err_len,
                        "native x86_64 local '%s' needs an initializer", name);
      return false;
    }
    ny_x64_local_t *l = ny_x64_find_local(ctx, name);
    if (!l) {
      ny_native_set_err(ctx->err, ctx->err_len,
                        "native x86_64 local planner missed '%s'", name);
      return false;
    }
    if (!ny_x64_emit_expr_raw(ctx, v->exprs.data[i]) ||
        !ny_native_printf(ctx->w, "\tmovq\t%%rax, -%d(%%rbp)\n", l->offset))
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
  } else if (!ny_native_put(ctx->w, "\txorq\t%rax, %rax\n")) {
    return false;
  }
  ctx->emitted_return = true;
  return ny_native_printf(ctx->w, "\tjmp\t%s\n", ctx->epilogue_label);
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
  if (!ny_native_printf(ctx->w, "\tjmp\t%s\n%s:\n", end_label, else_label))
    return false;
  if (s->as.iff.alt && !ny_x64_emit_stmt(ctx, s->as.iff.alt))
    return false;
  ny_x64_forget_local_consts(ctx);
  return ny_native_printf(ctx->w, "%s:\n", end_label);
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
  if (!ny_native_printf(ctx->w, "%s:\n", head_label))
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
  return ny_native_printf(ctx->w, "\tjmp\t%s\n%s:\n", head_label, end_label);
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
    ny_native_set_err(ctx->err, ctx->err_len,
                      "native x86_64 debug path does not lower statement kind %d yet",
                      (int)s->kind);
    return false;
  }
}

static bool ny_x64_emit_prologue(ny_x64_ctx_t *ctx) {
  return ny_native_put(ctx->w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n") &&
         (ctx->frame_bytes <= 0 ||
          ny_native_printf(ctx->w, "\tsubq\t$%d, %%rsp\n", ctx->frame_bytes)) &&
         ny_native_put(ctx->w, "\txorq\t%rax, %rax\n");
}

static bool ny_x64_emit_epilogue(ny_x64_ctx_t *ctx) {
  if (!ny_native_printf(ctx->w, "%s:\n", ctx->epilogue_label))
    return false;
  if (!ctx->raw_return && !ny_x64_tag_rax(ctx))
    return false;
  return ny_native_put(ctx->w, "\tleave\n\tret\n");
}

static bool ny_x64_emit_func(ny_native_writer_t *w,
                             const ny_native_target_info_t *target,
                             const program_t *prog, const stmt_t *fn,
                             char *err, size_t err_len) {
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
    if (!ny_native_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else if (!ny_native_printf(w, "\t.type\t%s%s,@function\n",
                               target->symbol_prefix, label)) {
    return false;
  }
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", target->symbol_prefix,
                        label, target->symbol_prefix, label))
    return false;
  if (!ny_x64_emit_prologue(&ctx))
    return false;
  for (size_t i = 0; i < fn->as.fn.params.len; ++i) {
    ny_x64_local_t *l = ny_x64_find_local(&ctx, fn->as.fn.params.data[i].name);
    if (!l) {
      ny_native_set_err(err, err_len,
                        "native x86_64 parameter planner missed '%s'",
                        fn->as.fn.params.data[i].name);
      return false;
    }
    if (!ny_native_printf(w, "\tmovq\t%s, -%d(%%rbp)\n",
                          target->gp_arg_regs[i], l->offset))
      return false;
  }
  if (!ny_x64_emit_stmt(&ctx, fn->as.fn.body))
    return false;
  if (!ny_x64_emit_epilogue(&ctx))
    return false;
  if (strcmp(target->object_format, "macho") != 0) {
    if (!ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n",
                          target->symbol_prefix, label, target->symbol_prefix,
                          label))
      return false;
  }
  return true;
}

static bool ny_x64_emit_all_funcs(ny_native_writer_t *w,
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

bool ny_native_x86_64_emit_rt_main(ny_native_writer_t *w,
                                   const ny_native_target_info_t *target,
                                   const program_t *prog, char *err,
                                   size_t err_len) {
  const char *sym = target->symbol_prefix;
  ny_x64_ctx_t ctx = {
      .w = w, .target = target, .prog = prog, .err = err, .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label), ".Lny_rt_main_done");
  if (!ny_x64_collect_program_locals(&ctx, prog))
    return false;

  if (!ny_native_put(w, "# Nytrix native x86-64 backend output\n"))
    return false;
  if (!ny_native_printf(w, "# target=%s abi=%s object=%s red_zone=%s shadow_space=%zu\n",
                        target->target_name, target->abi_name,
                        target->object_format, target->red_zone ? "yes" : "no",
                        target->shadow_space_bytes))
    return false;
  if (!ny_native_put(w, "\t.text\n"))
    return false;
  if (!ny_x64_emit_all_funcs(w, target, prog, err, err_len))
    return false;
  if (strcmp(target->object_format, "macho") == 0) {
    if (!ny_native_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else {
    if (!ny_native_printf(w, "\t.type\t%srt_main,@function\n", sym))
      return false;
  }
  if (!ny_native_printf(w, "\t.globl\t%srt_main\n%srt_main:\n", sym, sym))
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
    ny_native_set_err(err, err_len,
                      "native x86_64 debug path needs a result expression or return");
    return false;
  }
  if (!ny_x64_emit_epilogue(&ctx))
    return false;
  if (strcmp(target->object_format, "macho") != 0) {
    if (!ny_native_printf(w, "\t.size\t%srt_main, .-%srt_main\n", sym, sym))
      return false;
  }
  return true;
}

/*
 * NYIR -> x86-64 instruction selection
 *
 * The NYIR is already optimized (constant-folded, copy-propagated,
 * DCE'd).  We map each live NYIR value to a dedicated stack slot so
 * that multi-use values survive across instruction boundaries.  The
 * optimizer has already removed most redundant slots.
 *
 * When one operand of a binop is CONST_I64 with a 32-bit immediate,
 * we emit the compact immediate form (e.g. addq $imm, %rax) instead
 * of the full load-load-operate-store sequence.
 */

#define NY_X64_NIR_MAX_SLOTS 4096

typedef struct {
  ny_native_writer_t *w;
  const ny_native_target_info_t *target;
  const nyir_func_t *nyir;
  int slot_offset[NY_X64_NIR_MAX_SLOTS];
  int frame_slots;
  int frame_bytes;
  int max_local_slot;
  bool value_f64[NY_X64_NIR_MAX_SLOTS];
  bool value_f32[NY_X64_NIR_MAX_SLOTS];
  bool local_f64[NY_X64_NIR_MAX_SLOTS];
  bool local_f32[NY_X64_NIR_MAX_SLOTS];
  /*
   * valmap[i] = index of the NYIR instruction that defines value i,
   * or -1 if not defined.  Used for immediate-operand detection.
   */
  int def_index[NY_X64_NIR_MAX_SLOTS];
  int use_count[NY_X64_NIR_MAX_SLOTS];
  bool fused_cmp[NY_X64_NIR_MAX_SLOTS];
  bool fused_const[NY_X64_NIR_MAX_SLOTS];
  int64_t fused_imm[NY_X64_NIR_MAX_SLOTS];
  bool is_leaf;
  const char *label_prefix;
  char epilogue_label[128];
  char *err;
  size_t err_len;
} ny_x64_nir_ctx_t;

static int ny_x64_nir_slot(ny_x64_nir_ctx_t *c, int value_id) {
  if (value_id < 0 || value_id >= c->nyir->next_value)
    return -1;
  int s = c->frame_slots + value_id;
  c->slot_offset[s] = (s + 1) * 8;
  return s;
}

static bool ny_x64_nir_inst_uses_value(const nyir_inst_t *in,
                                       int value_id) {
  const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
  for (size_t i = 0; i < sizeof(args) / sizeof(args[0]); ++i) {
    if (args[i] == value_id)
      return true;
  }
  if (in->op == NYIR_CALL) {
    for (size_t i = 0; i < in->extra_args_len; ++i) {
      if (in->extra_args[i] == value_id)
        return true;
    }
  }
  return false;
}

static void ny_x64_nir_compute_frame(ny_x64_nir_ctx_t *c) {
  int max_val = c->nyir->next_value;
  int max_local = c->nyir->param_count > 0
                      ? (int)c->nyir->param_count - 1
                      : -1;
  for (int v = 0; v < max_val && v < NY_X64_NIR_MAX_SLOTS; ++v) {
    c->def_index[v] = -1;
    c->use_count[v] = 0;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyir_inst_t *in = &c->nyir->data[i];
    if ((in->op == NYIR_LOAD_LOCAL || in->op == NYIR_STORE_LOCAL) &&
        (int)in->imm > max_local)
      max_local = (int)in->imm;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS)
      c->def_index[in->dst] = (int)i;
    /*
     * Count uses of input values for next-use fusion.
     */
    const int args[] = {in->a, in->b, in->c, in->d, in->e, in->f};
    for (size_t j = 0; j < sizeof(args) / sizeof(args[0]); ++j) {
      if (args[j] >= 0 && args[j] < NY_X64_NIR_MAX_SLOTS)
        c->use_count[args[j]]++;
    }
    if (in->op == NYIR_CALL && in->extra_args && in->extra_args_len > 0) {
      for (size_t j = 0; j < in->extra_args_len; ++j) {
        int a = in->extra_args[j];
        if (a >= 0 && a < NY_X64_NIR_MAX_SLOTS) c->use_count[a]++;
      }
    }
  }
  c->is_leaf = true;
  for (size_t i = 0; i < c->nyir->len; ++i) {
    if (c->nyir->data[i].op == NYIR_CALL) {
      c->is_leaf = false;
      break;
    }
  }
  c->max_local_slot = max_local + 1;
  c->frame_slots = c->max_local_slot;
  /*
   * Pre-compute slot offsets for all NYIR values.
   */
  for (int v = 0; v < max_val; ++v)
    ny_x64_nir_slot(c, v);
  int total = c->frame_slots + max_val;
  int raw = total * 8;
  /*
   * %rbx is no longer saved: after pushq %rbp, a 16-byte multiple keeps
   * %rsp SysV-aligned at every nested call.
   */
  c->frame_bytes = (raw + 15) & ~15;
}

static void ny_x64_nir_slot_str(ny_x64_nir_ctx_t *c, int slot, char *buf, size_t len) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS) { buf[0] = '\0'; return; }
  if (c->is_leaf) {
    snprintf(buf, len, "%d(%%rsp)", c->frame_bytes - (slot + 2) * 8);
  } else {
    int off = slot < c->frame_slots ? (slot + 1) * 8 : c->slot_offset[slot];
    snprintf(buf, len, "-%d(%%rbp)", off);
  }
}

/*
 * Check whether value_id is defined by a CONST_I64 with a 32-bit
 * sign-extended immediate.  Returns the immediate on success, or
 * leaves *out unchanged and returns false.
 */
static bool ny_x64_nir_try_const_i32(ny_x64_nir_ctx_t *c, int value_id,
                                     int64_t *out) {
  if (value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS)
    return false;
  int di = c->def_index[value_id];
  if (di < 0 || (size_t)di >= c->nyir->len)
    return false;
  const nyir_inst_t *def = &c->nyir->data[di];
  if (def->op != NYIR_CONST_I64 || def->dst != value_id)
    return false;
  int64_t v = def->imm;
  /*
   * 32-bit sign-extended immediate range.
   */
  if (v < INT32_MIN || v > INT32_MAX)
    return false;
  *out = v;
  return true;
}

/*
 * Check whether value_id is defined by a CONST_I64 with a shift-count
 * immediate (1..63).  Returns the count on success.
 */
static bool ny_x64_nir_try_shift_imm(ny_x64_nir_ctx_t *c, int value_id,
                                     int *out) {
  if (value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS)
    return false;
  int di = c->def_index[value_id];
  if (di < 0 || (size_t)di >= c->nyir->len)
    return false;
  const nyir_inst_t *def = &c->nyir->data[di];
  if (def->op != NYIR_CONST_I64 || def->dst != value_id)
    return false;
  int64_t v = def->imm;
  if (v < 1 || v > 63)
    return false;
  *out = (int)v;
  return true;
}

/*
 * Return the shift count only for a positive power-of-two divisor. Shift
 * operands accept any count; division and modulo must not confuse a literal
 * divisor such as 5 with a shift count of 5.
 */
static bool ny_x64_nir_try_pow2_divisor(ny_x64_nir_ctx_t *c, int value_id,
                                        int *out) {
  int64_t divisor = 0;
  if (!ny_x64_nir_try_const_i32(c, value_id, &divisor) || divisor <= 1 ||
      (divisor & (divisor - 1)) != 0)
    return false;
  int shift = 0;
  while (divisor > 1) {
    divisor >>= 1;
    shift++;
  }
  *out = shift;
  return true;
}

/*
 * Next-use fusion: check whether a CONST_I64 value with dst=value_id
 * has exactly one use, that use is within range, and the single consumer
 * is a binop/CMP/CALL that can absorb a 32-bit immediate via
 * try_const_i32.  Returns true when the CONST_I64 store can be skipped.
 */
static bool ny_x64_nir_can_fuse_const(ny_x64_nir_ctx_t *c, int value_id,
                                      size_t def_idx) {
  if (value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS)
    return false;
  if (c->use_count[value_id] != 1)
    return false;
  /*
   * Find the single consumer after def_idx.
   */
  for (size_t j = def_idx + 1; j < c->nyir->len; ++j) {
    const nyir_inst_t *u = &c->nyir->data[j];
    if (!ny_x64_nir_inst_uses_value(u, value_id))
      continue;
    switch (u->op) {
    case NYIR_ADD_I64: case NYIR_SUB_I64: case NYIR_MUL_I64:
    case NYIR_AND_I64: case NYIR_OR_I64: case NYIR_XOR_I64:
    case NYIR_SHL_I64: case NYIR_SAR_I64:
    case NYIR_CMP_I64:
    case NYIR_CALL:
      return true;
    default:
      return false;
    }
  }
  return false;
}

static bool ny_x64_nir_load(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovq\t%d(%%rsp), %%rax\n", off);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovq\t-%d(%%rbp), %%rax\n",
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_store(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovq\t%%rax, %d(%%rsp)\n", off);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovq\t%%rax, -%d(%%rbp)\n",
                          c->slot_offset[slot]);
}

/*
 * Load a value that may be a next-use-fused CONST_I64 whose home store was
 * skipped (see can_fuse_const).  Fused constants must be materialized from
 * their immediate; loading the never-written home slot would read garbage.
 */
static bool ny_x64_nir_load_value(ny_x64_nir_ctx_t *c, int value_id) {
  if (value_id >= 0 && value_id < NY_X64_NIR_MAX_SLOTS &&
      c->fused_const[value_id])
    return ny_native_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n",
                            c->fused_imm[value_id]);
  return ny_x64_nir_load(c, ny_x64_nir_slot(c, value_id));
}

static bool ny_x64_nir_load_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS ||
      xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovsd\t%d(%%rsp), %%xmm%d\n", off, xmm);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovsd\t-%d(%%rbp), %%xmm%d\n",
                          c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS ||
      xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovsd\t%%xmm%d, %d(%%rsp)\n", xmm, off);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovsd\t%%xmm%d, -%d(%%rbp)\n", xmm,
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_load_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS ||
      xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovss\t%d(%%rsp), %%xmm%d\n", off, xmm);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovss\t-%d(%%rbp), %%xmm%d\n",
                          c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS ||
      xmm < 0 || xmm > 15)
    return false;
  if (c->is_leaf) {
    int off = c->frame_bytes - (slot + 2) * 8;
    return ny_native_printf(c->w, "\tmovss\t%%xmm%d, %d(%%rsp)\n", xmm, off);
  }
  if (c->slot_offset[slot] <= 0) return false;
  return ny_native_printf(c->w, "\tmovss\t%%xmm%d, -%d(%%rbp)\n", xmm,
                          c->slot_offset[slot]);
}

static void ny_x64_nir_classify_values(ny_x64_nir_ctx_t *c) {
  if (!c || !c->nyir)
    return;
  size_t param_count = c->nyir->param_count < NY_X64_NIR_MAX_SLOTS
                           ? c->nyir->param_count
                           : NY_X64_NIR_MAX_SLOTS;
  for (size_t i = 0; i < param_count; ++i) {
    c->local_f64[i] = c->nyir->param_types[i] == NYIR_PARAM_F64;
    c->local_f32[i] = c->nyir->param_types[i] == NYIR_PARAM_F32;
  }
  for (size_t i = 0; i < c->nyir->len; ++i) {
    const nyir_inst_t *in = &c->nyir->data[i];
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (nyir_op_is_f64(in->op) ||
         (in->op == NYIR_CALL && (in->flags & NYIR_INST_F_RET_F64))))
      c->value_f64[in->dst] = true;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (nyir_op_is_f32(in->op) ||
         ((in->flags & NYIR_INST_F_RET_F32) &&
          in->op == NYIR_CALL)))
      c->value_f32[in->dst] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < c->nyir->len; ++i) {
      const nyir_inst_t *in = &c->nyir->data[i];
      if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < NY_X64_NIR_MAX_SLOTS && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->value_f64[in->a] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < NY_X64_NIR_MAX_SLOTS && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->value_f32[in->a] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f64[in->imm] &&
                 !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f32[in->imm] &&
                 !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->dst] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->dst] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NYIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      }
      if ((in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
           in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64 ||
           in->op == NYIR_SQRT_F64 || in->op == NYIR_SIN_F64 ||
           in->op == NYIR_COS_F64)) {
        if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f64[in->a]) {
          c->value_f64[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f64[in->b]) {
          c->value_f64[in->b] = true;
          changed = true;
        }
      }
      if ((in->op == NYIR_ADD_F32 || in->op == NYIR_SUB_F32 ||
           in->op == NYIR_MUL_F32 || in->op == NYIR_DIV_F32)) {
        if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f32[in->a]) {
          c->value_f32[in->a] = true;
          changed = true;
        }
        if (in->b >= 0 && in->b < NY_X64_NIR_MAX_SLOTS &&
            !c->value_f32[in->b]) {
          c->value_f32[in->b] = true;
          changed = true;
        }
      }
    }
  }
}

static const char *ny_x64_nir_setcc(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "sete";
  case NYIR_CMP_NE:
    return "setne";
  case NYIR_CMP_LT:
    return "setl";
  case NYIR_CMP_LE:
    return "setle";
  case NYIR_CMP_GT:
    return "setg";
  case NYIR_CMP_GE:
    return "setge";
  }
  return "sete";
}

static const char *ny_x64_nir_jcc(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ: return "je";
  case NYIR_CMP_NE: return "jne";
  case NYIR_CMP_LT: return "jl";
  case NYIR_CMP_LE: return "jle";
  case NYIR_CMP_GT: return "jg";
  case NYIR_CMP_GE: return "jge";
  }
  return "jne";
}

static const char *ny_x64_nir_f64_setcc(nyir_cmp_t cmp) {
  switch (cmp) {
  case NYIR_CMP_EQ:
    return "sete";
  case NYIR_CMP_NE:
    return "setne";
  case NYIR_CMP_LT:
    return "setb";
  case NYIR_CMP_LE:
    return "setbe";
  case NYIR_CMP_GT:
    return "seta";
  case NYIR_CMP_GE:
    return "setae";
  }
  return "sete";
}

/*
 * Check whether value_id is defined by a CONST_I64 with a full 64-bit
 * immediate.  Unlike try_const_i32 this accepts any magnitude; only usable by
 * sequences that materialize a full imm64 (e.g. magic-number division).
 */
static bool ny_x64_nir_try_const_i64(ny_x64_nir_ctx_t *c, int value_id,
                                     int64_t *out) {
  if (value_id < 0 || value_id >= NY_X64_NIR_MAX_SLOTS)
    return false;
  int di = c->def_index[value_id];
  if (di < 0 || (size_t)di >= c->nyir->len)
    return false;
  const nyir_inst_t *def = &c->nyir->data[di];
  if (def->op != NYIR_CONST_I64 || def->dst != value_id)
    return false;
  if (out)
    *out = def->imm;
  return true;
}

/*
 * Granlund-Montgomery signed division magic for a positive divisor ad > 1
 * (Hacker's Delight 10-3, 64-bit form).  Returns the magic multiplier M (a
 * signed 64-bit value; negative means the top bit is set) and post-shift s
 * such that, for every int64 x:
 *   q = mulhs(x, M); if (M < 0) q += x; q >>= s; q -= (x >> 63);
 * equals x / ad (C truncation toward zero).
 */
static void ny_x64_nir_sdiv_magic(uint64_t ad, int64_t *magic_out,
                                  unsigned *shift_out) {
  const uint64_t two63 = (uint64_t)1 << 63;
  uint64_t anc = two63 - 1 - (two63 % ad);
  unsigned p = 63;
  uint64_t q1 = two63 / anc;
  uint64_t r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / ad;
  uint64_t r2 = two63 - q2 * ad;
  uint64_t delta;
  do {
    p++;
    q1 <<= 1;
    r1 <<= 1;
    if (r1 >= anc) { q1++; r1 -= anc; }
    q2 <<= 1;
    r2 <<= 1;
    if (r2 >= ad)  { q2++; r2 -= ad; }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  *magic_out = (int64_t)(q2 + 1);
  *shift_out = p - 64;
}

/*
 * Emit the magic-reciprocal sequence for x / divisor (or x % divisor) where
 * divisor is a known non-power-of-2 constant and in->a holds the i64 dividend.
 * Leaves the result in %rax and stores it to in->dst.
 */
static bool ny_x64_nir_emit_sdiv_magic(ny_x64_nir_ctx_t *c,
                                       const nyir_inst_t *in,
                                       int64_t divisor, bool want_rem) {
  uint64_t ad = divisor >= 0 ? (uint64_t)divisor
                             : (uint64_t)(-(divisor + 1)) + 1;
  int64_t magic = 0;
  unsigned s = 0;
  ny_x64_nir_sdiv_magic(ad, &magic, &s);
  bool negate = divisor < 0;
  if (!ny_x64_nir_load_value(c, in->a) ||
      !ny_native_put(c->w, "\tmovq\t%rax, %r9\n") ||
      !ny_native_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%r10\n", magic) ||
      !ny_native_put(c->w, "\timulq\t%r10\n\tmovq\t%rdx, %rax\n"))
    return false;
  if (magic < 0 && !ny_native_put(c->w, "\taddq\t%r9, %rax\n"))
    return false;
  if (s > 0 && !ny_native_printf(c->w, "\tsarq\t$%u, %%rax\n", s))
    return false;
  if (!ny_native_put(c->w,
                     "\tmovq\t%r9, %rcx\n\tsarq\t$63, %rcx\n\tsubq\t%rcx, %rax\n"))
    return false;
  if (negate && !ny_native_put(c->w, "\tnegq\t%rax\n"))
    return false;
  if (want_rem) {
    if (!ny_native_put(c->w, "\tmovq\t%rax, %r10\n\tmovq\t%r9, %rax\n"))
      return false;
    if ((int64_t)(int32_t)ad == (int64_t)ad) {
      if (!ny_native_printf(c->w, "\timulq\t$%" PRId64 ", %%r10, %%r10\n",
                            (int64_t)ad))
        return false;
    } else if (!ny_native_printf(c->w,
                                 "\tmovabsq\t$%" PRId64 ", %%r11\n\timulq\t%%r11, %%r10\n",
                                 (int64_t)ad)) {
      return false;
    }
    if (!ny_native_put(c->w, "\tsubq\t%r10, %rax\n"))
      return false;
  }
  return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
}

static bool ny_x64_nir_emit_inst(ny_x64_nir_ctx_t *c,
                                  const nyir_inst_t *in, size_t idx) {
  switch (in->op) {
  case NYIR_NOP:
    return true;
  case NYIR_CONST_I64:
    if (in->dst < 0)
      return true;
    ny_x64_nir_slot(c, in->dst);
    if (in->imm >= INT32_MIN && in->imm <= INT32_MAX &&
        ny_x64_nir_can_fuse_const(c, in->dst, idx)) {
      c->fused_const[in->dst] = true;
      c->fused_imm[in->dst] = in->imm;
      return true;
    }
    return ny_native_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
    if (in->dst < 0)
      return true;
    ny_x64_nir_slot(c, in->dst);
    return ny_native_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_COPY:
    if (in->dst < 0)
      return true;
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_LOAD_LOCAL:
    if (in->dst < 0)
      return true;
    /*
     * Load from a local (param/mut) slot into a value slot.
     */
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: load.local invalid slot %" PRId64, in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    return ny_x64_nir_load(c, (int)in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ADDR_LOCAL:
    if (in->dst < 0)
      return true;
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: addr.local invalid slot %" PRId64,
                        in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    /*
     * Leaf functions do not establish %rbp.  Use the same slot spelling as
     * loads/stores so addr_of(local) aliases the actual O0 frame slot.
     */
    char addr[64];
    ny_x64_nir_slot_str(c, (int)in->imm, addr, sizeof(addr));
    return ny_native_printf(c->w, "\tleaq\t%s, %%rax\n", addr) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ADDR_SYMBOL:
    if (in->dst < 0)
      return true;
    if (!in->symbol || !in->symbol[0]) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: addr.symbol missing symbol name");
      return false;
    }
    return ny_native_printf(c->w, "\tleaq\t%s(%%rip), %%rax\n", in->symbol) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ALLOCA:
    if (in->dst < 0)
      return true;
    return ny_native_printf(c->w, "\tsubq\t$%" PRId64 ", %%rsp\n\tandq\t$-16, %%rsp\n\tmovq\t%%rsp, %%rax\n", in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_COPY_STRUCT:
    if (in->imm <= 0)
      return true;
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rsi\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rdi\n") &&
           ny_native_printf(c->w, "\tmovq\t$%" PRId64 ", %%rcx\n\trep movsb\n", in->imm);
  case NYIR_CAPTURE_RET:
    if (in->dst < 0)
      return true;
    switch (in->imm) {
    case 0:
      if (!ny_native_put(c->w, "\tmovq\t%rdx, %rax\n"))
        return false;
      break;
    case 1:
      break;
    case 2:
      if (!ny_native_put(c->w, "\tmovq\t%xmm0, %rax\n"))
        return false;
      break;
    case 3:
      if (!ny_native_put(c->w, "\tmovq\t%xmm1, %rax\n"))
        return false;
      break;
    default:
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: invalid capture.ret selector");
      return false;
    }
    return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_VEC4_ADD_F64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovapd\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\taddpd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SUB_F64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovapd\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tsubpd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_MUL_F64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovapd\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tmulpd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_DIV_F64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovapd\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tdivpd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_FMA_F64: {
    int aslot = ny_x64_nir_slot(c, in->a);
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, aslot, 0) &&
           ny_native_printf(c->w, "\tmovapd\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->c), 2) &&
           ny_native_put(c->w, "\tmulpd\t%xmm1, %xmm0\n\taddpd\t%xmm2, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SET1_F64: {
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_put(c->w, "\tmovddup\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_LOAD_F64: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovupd\t(%rax), %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_STORE_F64: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 0) &&
           ny_native_put(c->w, "\tmovupd\t%xmm0, (%rax)\n");
  }
  case NYIR_VEC4_SHUFFLE_F64: {
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tshufpd\t$%" PRId64 ", %%xmm0, %%xmm0\n", in->imm & 3) &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_REDUCE_ADD_F64: {
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 0) &&
           ny_native_put(c->w, "\tmovapd\t%xmm0, %xmm1\n"
                               "\tshufpd\t$1, %xmm1, %xmm1\n"
                               "\taddsd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 1) &&
           ny_native_put(c->w, "\taddsd\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_ADD_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\taddps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_SUB_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tsubps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_MUL_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tmulps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_DIV_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tdivps\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_FMA_F32: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovaps\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->c), 2) &&
           ny_native_put(c->w, "\tmulps\t%xmm1, %xmm0\n\taddps\t%xmm2, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_SET1_F32: {
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_put(c->w, "\tshufps\t$0, %xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_LOAD_F32: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovups\t(%rax), %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC8_STORE_F32: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->b), 0) &&
           ny_native_put(c->w, "\tmovups\t%xmm0, (%rax)\n");
  }
  case NYIR_VEC8_SHUFFLE_F32: {
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tshufps\t$%" PRId64 ", %%xmm0, %%xmm0\n", in->imm & 0xff) &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SET1_I64: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovq\t%rax, %xmm0\n"
                               "\tpunpcklqdq\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_ADD_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovdqa\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tpaddq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SUB_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovdqa\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tpsubq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_AND_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovdqa\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tpand\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_OR_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovdqa\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tpor\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_XOR_I64: {
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\tmovdqa\t-%d(%%rbp), %%xmm1\n", c->slot_offset[bslot]) &&
           ny_native_put(c->w, "\tpxor\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_LOAD_I64: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovdqu\t(%rax), %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_STORE_I64: {
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 0) &&
           ny_native_put(c->w, "\tmovdqu\t%xmm0, (%rax)\n");
  }
  case NYIR_VEC4_SHL_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, &shift))
      return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
             ny_native_printf(c->w, "\tmovdqa\t%%xmm0, %%xmm1\n\tmovq\t$%d, %%xmm2\n"
                                    "\tpsllq\t%%xmm2, %%xmm0\n", shift) &&
             ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %xmm1\n") &&
           ny_native_put(c->w, "\tshufps\t$0xe0, %xmm1, %xmm1\n") &&
           ny_native_put(c->w, "\tpsllq\t%xmm1, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_VEC4_SAR_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, &shift))
      return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
             ny_native_printf(c->w, "\tmovq\t$%d, %%xmm1\n", shift) &&
             ny_native_put(c->w, "\tshufps\t$0xe0, %xmm1, %xmm1\n") &&
             ny_native_put(c->w, "\tvpsraq\t%xmm1, %xmm0, %xmm0\n") &&
             ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %xmm1\n") &&
           ny_native_put(c->w, "\tshufps\t$0xe0, %xmm1, %xmm1\n") &&
           ny_native_put(c->w, "\tvpsraq\t%xmm1, %xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_STORE_LOCAL:
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: store.local invalid slot %" PRId64, in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    return ny_x64_nir_load_value(c, in->a) &&
           ny_x64_nir_store(c, (int)in->imm);
  case NYIR_LOAD_I64:
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovq\t(%rax), %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_STORE_I64:
    return ny_x64_nir_load_value(c, in->c) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovq\t%r10, (%rax)\n");
  case NYIR_ADD_I64: {
    int64_t imm = 0;
    /*
     * addq $imm, %rax  (sign-extended 32-bit)
     */
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\taddq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\taddq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\taddq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_SUB_I64: {
    int64_t imm = 0;
    /*
     * subq $imm, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tsubq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tsubq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_MUL_I64: {
    int64_t imm = 0;
    /*
     * Power-of-2 strength reduction: a * 2^n → shlq $n, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, &imm) && imm > 0 && (imm & (imm - 1)) == 0) {
      int shift = 0;
      int64_t v = imm;
      while (v > 1) { v >>= 1; shift++; }
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    if (ny_x64_nir_try_const_i32(c, in->a, &imm) && imm > 0 && (imm & (imm - 1)) == 0) {
      int shift = 0;
      int64_t v = imm;
      while (v > 1) { v >>= 1; shift++; }
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    /*
     * imulq $imm, %rax, %rax
     */
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\timulq\t$%" PRId64 ", %%rax, %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\timulq\t$%" PRId64 ", %%rax, %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\timulq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_DIV_I64: {
    int shift = 0;
    if (ny_x64_nir_try_pow2_divisor(c, in->b, &shift)) {
      int64_t mask = (1LL << shift) - 1;
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_native_put(c->w, "\tsarq\t$63, %rax\n") &&
             ny_native_printf(c->w, "\tandq\t$%" PRId64 ", %%rax\n", mask) &&
             ny_native_put(c->w, "\taddq\t%rcx, %rax\n") &&
             ny_native_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    int64_t d = 0;
    if (ny_x64_nir_try_const_i64(c, in->b, &d) && d != 0 && d != 1 &&
        d != -1) {
      uint64_t ad = d >= 0 ? (uint64_t)d : (uint64_t)(-(d + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return ny_x64_nir_emit_sdiv_magic(c, in, d, false);
    }
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tcqto\n\tidivq\t%r10\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_MOD_I64: {
    int shift = 0;
    if (ny_x64_nir_try_pow2_divisor(c, in->b, &shift)) {
      int64_t mask = (1LL << shift) - 1;
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_put(c->w, "\tmovq\t%rax, %rcx\n") &&
             ny_native_put(c->w, "\tmovq\t%rax, %rdx\n") &&
             ny_native_put(c->w, "\tsarq\t$63, %rdx\n") &&
             ny_native_printf(c->w, "\tandq\t$%" PRId64 ", %%rdx\n", mask) &&
             ny_native_put(c->w, "\taddq\t%rdx, %rax\n") &&
             ny_native_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             ny_native_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_native_put(c->w, "\tsubq\t%rax, %rcx\n") &&
             ny_native_put(c->w, "\tmovq\t%rcx, %rax\n") &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    int64_t d = 0;
    if (ny_x64_nir_try_const_i64(c, in->b, &d) && d != 0 && d != 1 &&
        d != -1) {
      uint64_t ad = d >= 0 ? (uint64_t)d : (uint64_t)(-(d + 1)) + 1;
      if ((ad & (ad - 1)) != 0)
        return ny_x64_nir_emit_sdiv_magic(c, in, d, true);
    }
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tcqto\n\tidivq\t%r10\n\tmovq\t%rdx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_AND_I64: {
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tandq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\tandq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tandq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_OR_I64: {
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\torq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\torq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\torq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_XOR_I64: {
    int64_t imm = 0;
    if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\txorq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    if (ny_x64_nir_try_const_i32(c, in->a, &imm))
      return ny_x64_nir_load_value(c, in->b) &&
             ny_native_printf(c->w, "\txorq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\txorq\t%r10, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_SHL_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, &shift))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tshlq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovb\t%r10b, %cl\n\tshlq\t%cl, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_SAR_I64: {
    int shift = 0;
    if (ny_x64_nir_try_shift_imm(c, in->b, &shift))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tsarq\t$%d, %%rax\n", shift) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tmovb\t%r10b, %cl\n\tsarq\t%cl, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64: {
    const char *insn = in->op == NYIR_ADD_F64 ? "addsd" :
                       in->op == NYIR_SUB_F64 ? "subsd" :
                       in->op == NYIR_MUL_F64 ? "mulsd" : "divsd";
    int bslot = ny_x64_nir_slot(c, in->b);
    char baddr[64];
    ny_x64_nir_slot_str(c, bslot, baddr, sizeof(baddr));
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\t%s\t%s, %%xmm0\n", insn, baddr) &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F64:
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tcvtsi2sdq\t%rax, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_SQRT_F64:
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_put(c->w, "\tsqrtsd\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_SIN_F64:
  case NYIR_COS_F64: {
    char addr[64];
    ny_x64_nir_slot_str(c, ny_x64_nir_slot(c, in->a), addr, sizeof(addr));
    return ny_native_printf(c->w, "\tfldl\t%s\n\tf%s\n\tfstpl\t-%d(%%rbp)\n",
                            addr, in->op == NYIR_SIN_F64 ? "sin" : "cos",
                            ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32: {
    const char *insn = in->op == NYIR_ADD_F32 ? "addss" :
                       in->op == NYIR_SUB_F32 ? "subss" :
                       in->op == NYIR_MUL_F32 ? "mulss" : "divss";
    int bslot = ny_x64_nir_slot(c, in->b);
    char baddr[64];
    ny_x64_nir_slot_str(c, bslot, baddr, sizeof(baddr));
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\t%s\t%s, %%xmm0\n", insn, baddr) &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F32:
    return ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tcvtsi2ssq\t%rax, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_F32_TO_F64:
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_put(c->w, "\tcvtss2sd\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_F64_TO_F32:
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_put(c->w, "\tcvtsd2ss\t%xmm0, %xmm0\n") &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
   case NYIR_CMP_I64: {
     if (c->fused_cmp[in->dst])
       return true;
     int64_t imm = 0;
     /*
      * cmpq $imm, %rax + setcc
      */
     if (ny_x64_nir_try_const_i32(c, in->b, &imm))
      return ny_x64_nir_load_value(c, in->a) &&
             ny_native_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", imm) &&
             ny_native_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                             ny_x64_nir_setcc(in->cmp)) &&
             ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    return ny_x64_nir_load_value(c, in->b) &&
           ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
           ny_x64_nir_load_value(c, in->a) &&
           ny_native_put(c->w, "\tcmpq\t%r10, %rax\n") &&
           ny_native_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                           ny_x64_nir_setcc(in->cmp)) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_CMP_F64: {
    char unordered[96];
    char done[96];
    snprintf(unordered, sizeof(unordered), ".Lnyir_%s_fcmp_unordered_%d",
             c->label_prefix, in->dst);
    snprintf(done, sizeof(done), ".Lnyir_%s_fcmp_done_%d",
             c->label_prefix, in->dst);
    int unordered_result = in->cmp == NYIR_CMP_NE ? 1 : 0;
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->b), 1) &&
           ny_native_put(c->w, "\tucomisd\t%xmm1, %xmm0\n") &&
           ny_native_printf(c->w, "\tjp\t%s\n", unordered) &&
           ny_native_printf(c->w, "\t%s\t%%al\n\tjmp\t%s\n", ny_x64_nir_f64_setcc(in->cmp), done) &&
           ny_native_printf(c->w, "%s:\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, %%rax\n",
                            unordered, unordered_result, done) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_CMP_F32: {
    char unordered[96];
    char done[96];
    snprintf(unordered, sizeof(unordered), ".Lnyir_%s_fcmp_unordered_%d",
             c->label_prefix, in->dst);
    snprintf(done, sizeof(done), ".Lnyir_%s_fcmp_done_%d",
             c->label_prefix, in->dst);
    int unordered_result = in->cmp == NYIR_CMP_NE ? 1 : 0;
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->b), 1) &&
           ny_native_put(c->w, "\tucomiss\t%xmm1, %xmm0\n") &&
           ny_native_printf(c->w, "\tjp\t%s\n", unordered) &&
           ny_native_printf(c->w, "\t%s\t%%al\n\tjmp\t%s\n",
                            ny_x64_nir_f64_setcc(in->cmp), done) &&
           ny_native_printf(c->w, "%s:\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, %%rax\n",
                            unordered, unordered_result, done) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  }
  case NYIR_BOUNDS_CHECK: {
    char ok[128];
    snprintf(ok, sizeof(ok), ".Lnyir_%s_bounds_ok_%zu",
             c->label_prefix, idx);
    if (!ny_x64_nir_load_value(c, in->b))
      return false;
    if (in->c >= 0) {
      if (!ny_x64_nir_load_value(c, in->c) ||
          !ny_native_put(c->w, "\tmovq\t%rax, %r10\n") ||
          !ny_x64_nir_load_value(c, in->b) ||
          !ny_native_put(c->w, "\tcmpq\t%r10, %rax\n"))
        return false;
    } else if (!ny_native_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n",
                                 in->imm)) {
      return false;
    }
    return ny_native_printf(c->w, "\tjb\t%s\n\tud2\n%s:\n", ok, ok);
  }
  case NYIR_LABEL:
    return ny_native_printf(c->w, ".Lnyir_%s_L%" PRId64 ":\n",
                            c->label_prefix, in->imm);
  case NYIR_BR:
    return ny_native_printf(c->w, "\tjmp\t.Lnyir_%s_L%" PRId64 "\n",
                            c->label_prefix, in->imm);
   case NYIR_BR_IF: {
     if (in->a >= 0 && in->a < NY_X64_NIR_MAX_SLOTS) {
       int di = c->def_index[in->a];
       if (di >= 0 && (size_t)di < c->nyir->len) {
         const nyir_inst_t *cmp = &c->nyir->data[di];
         if (cmp->op == NYIR_CMP_I64 && cmp->dst == in->a) {
           c->fused_cmp[in->a] = true;
           int64_t bimm = 0;
           bool b_is_imm = ny_x64_nir_try_const_i32(c, cmp->b, &bimm);
           if (b_is_imm)
             return ny_x64_nir_load_value(c, cmp->a) &&
                    ny_native_printf(c->w, "\tcmpq\t$%" PRId64 ", %%rax\n", bimm) &&
                    ny_native_printf(c->w, "\t%s\t.Lnyir_%s_L%" PRId64 "\n",
                                     ny_x64_nir_jcc(cmp->cmp), c->label_prefix,
                                     in->imm);
           return ny_x64_nir_load_value(c, cmp->b) &&
                  ny_native_put(c->w, "\tmovq\t%rax, %r10\n") &&
                  ny_x64_nir_load_value(c, cmp->a) &&
                  ny_native_printf(c->w, "\tcmpq\t%%r10, %%rax\n") &&
                  ny_native_printf(c->w, "\t%s\t.Lnyir_%s_L%" PRId64 "\n",
                                   ny_x64_nir_jcc(cmp->cmp), c->label_prefix,
                                   in->imm);
         }
       }
     }
     return ny_x64_nir_load_value(c, in->a) &&
            ny_native_put(c->w, "\ttestq\t%rax, %rax\n") &&
            ny_native_printf(c->w, "\tjne\t.Lnyir_%s_L%" PRId64 "\n",
                             c->label_prefix, in->imm);
   }
  case NYIR_RET: {
    /*
     * Load return value into %rax (skip if -1 = void return).
     */
      if (in->a >= 0) {
      if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (!ny_x64_nir_load_value(c, in->a)) {
        return false;
      }
    } else {
      if (!ny_native_put(c->w, "\txorq\t%rax, %rax\n"))
        return false;
    }
    return ny_native_printf(c->w, "\tjmp\t%s\n", c->epilogue_label);
  }
  case NYIR_CALL: {
    int argc = (int)in->imm;
    const char *sym_name = in->symbol ? in->symbol : "<null>";
    if (in->symbol && strcmp(in->symbol, "rt_native_sqrt_f64") == 0 && argc == 1 && in->a >= 0 && in->dst >= 0) {
      char val_str[64], dst_str[64];
      ny_x64_nir_slot_str(c, ny_x64_nir_slot(c, in->a), val_str, sizeof(val_str));
      ny_x64_nir_slot_str(c, ny_x64_nir_slot(c, in->dst), dst_str, sizeof(dst_str));
      return ny_native_printf(c->w, "\tmovsd\t%s, %%xmm0\n\tsqrtsd\t%%xmm0, %%xmm0\n\tmovsd\t%%xmm0, %s\n", val_str, dst_str);
    }
    if (argc < 0 || argc > NYIR_CALL_MAX_ARGS) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: call exceeds the maximum supported argument count");
      return false;
    }
    int arg_vals[NYIR_CALL_MAX_ARGS];
    arg_vals[0] = in->a;
    if (argc > 1) arg_vals[1] = in->b;
    if (argc > 2) arg_vals[2] = in->c;
    if (argc > 3) arg_vals[3] = in->d;
    if (argc > 4) arg_vals[4] = in->e;
    if (argc > 5) arg_vals[5] = in->f;
    for (int i = 6; i < argc; ++i)
      arg_vals[i] = (in->extra_args && (size_t)(i - 6) < in->extra_args_len)
                        ? in->extra_args[i - 6]
                        : -1;
    for (int i = 0; i < argc; ++i) {
      if (arg_vals[i] < 0)
        return false;
    }
    bool arg_f64[NYIR_CALL_MAX_ARGS] = {0};
    bool arg_f32[NYIR_CALL_MAX_ARGS] = {0};
    int gp_index[NYIR_CALL_MAX_ARGS];
    int sse_index[NYIR_CALL_MAX_ARGS];
    int agg_gp[NYIR_CALL_MAX_ARGS][2];
    int agg_sse[NYIR_CALL_MAX_ARGS][2];
    bool agg_in_regs[NYIR_CALL_MAX_ARGS] = {0};
    int gp = 0;
    int sse = 0;
    int stack_argc = 0;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
      agg_gp[i][0] = agg_gp[i][1] = -1;
      agg_sse[i][0] = agg_sse[i][1] = -1;
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        arg_f64[i] = false;
        arg_f32[i] = false;
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        unsigned gp_need = 0, sse_need = 0;
        bool register_eligible = true;
        for (int chunk = 0; chunk < 2; ++chunk) {
          unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
          gp_need += cls == NYIR_ARG_CLASS_INTEGER;
          sse_need += cls == NYIR_ARG_CLASS_SSE;
          if (cls != NYIR_ARG_CLASS_NONE &&
              cls != NYIR_ARG_CLASS_INTEGER &&
              cls != NYIR_ARG_CLASS_SSE)
            register_eligible = false;
        }
        if (register_eligible && size <= 16 && gp + (int)gp_need <= 6 &&
            sse + (int)sse_need <= 8) {
          for (int chunk = 0; chunk < 2; ++chunk) {
            unsigned cls = NYIR_ARG_AGG_CLASS(in->arg_sizes[i], chunk);
            if (cls == NYIR_ARG_CLASS_INTEGER)
              agg_gp[i][chunk] = gp++;
            else if (cls == NYIR_ARG_CLASS_SSE)
              agg_sse[i][chunk] = sse++;
          }
          agg_in_regs[i] = true;
        } else {
          stack_argc += (int)((size + 7) / 8);
        }
        continue;
      }
      arg_f64[i] = arg_vals[i] < NY_X64_NIR_MAX_SLOTS && c->value_f64[arg_vals[i]];
      arg_f32[i] = arg_vals[i] < NY_X64_NIR_MAX_SLOTS && c->value_f32[arg_vals[i]];
      if (arg_f64[i] || arg_f32[i]) {
        if (sse < 8)
          sse_index[i] = sse++;
        else
          stack_argc++;
      } else {
        if (gp < 6)
          gp_index[i] = gp++;
        else
          stack_argc++;
      }
    }

    /*
     * Stack-passed args permanently move %rsp for
     * the duration of the call; pad to an even slot count so %rsp is
     * 16-byte aligned at the `call` instruction (shadow space, if any, is
     * itself a multiple of 16).
     */
    int pad = stack_argc % 2;
    if (pad && !ny_native_put(c->w, "\tsubq\t$8, %rsp\n"))
      return false;
    /*
     * Push stack args highest-index first so the first stack arg ends up
     * closest to the top of stack (lowest address) at call time.
     */
    for (int i = argc - 1; i >= 0; --i) {
      if (gp_index[i] >= 0 || sse_index[i] >= 0 || agg_in_regs[i])
        continue;
      if (in->arg_sizes && in->arg_sizes[i] > 0) {
        uint32_t size = NYIR_ARG_AGG_SIZE(in->arg_sizes[i]);
        int slots = (int)((size + 7) / 8);
        if (!ny_native_printf(c->w, "\tsubq\t$%d, %%rsp\n", slots * 8))
          return false;
        if (!ny_x64_nir_load_value(c, arg_vals[i]))
          return false;
        if (!ny_native_put(c->w, "\tmovq\t%rax, %rsi\n\tmovq\t%rsp, %rdi\n") ||
            !ny_native_printf(c->w, "\tmovq\t$%" PRIu32 ", %%rcx\n\trep movsb\n", size))
          return false;
        continue;
      }
      if (arg_f64[i]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !ny_native_put(c->w, "\tsubq\t$8, %rsp\n\tmovsd\t%xmm0, (%rsp)\n"))
          return false;
      } else if (arg_f32[i]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !ny_native_put(c->w, "\tsubq\t$8, %rsp\n\tmovss\t%xmm0, (%rsp)\n"))
          return false;
      } else if (!ny_x64_nir_load_value(c, arg_vals[i]) ||
                 !ny_native_put(c->w, "\tpushq\t%rax\n")) {
        return false;
      }
    }
    for (int i = 0; i < argc; ++i) {
      if (agg_in_regs[i]) {
        if (!ny_x64_nir_load_value(c, arg_vals[i]))
          return false;
        for (int chunk = 0; chunk < 2; ++chunk) {
          int off = chunk * 8;
          if (agg_gp[i][chunk] >= 0) {
            if (!ny_native_printf(c->w, "\tmovq\t%d(%%rax), %s\n", off,
                                  c->target->gp_arg_regs[agg_gp[i][chunk]]))
              return false;
          } else if (agg_sse[i][chunk] >= 0 &&
                     !ny_native_printf(c->w, "\tmovq\t%d(%%rax), %%xmm%d\n",
                                       off, agg_sse[i][chunk])) {
            return false;
          }
        }
      } else if (sse_index[i] >= 0) {
        if (arg_f32[i]) {
          if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]), sse_index[i]))
            return false;
        } else if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]), sse_index[i])) {
          return false;
        }
      } else if (gp_index[i] >= 0) {
        int64_t carg_imm = 0;
        if (ny_x64_nir_try_const_i32(c, arg_vals[i], &carg_imm)) {
          if (!ny_native_printf(c->w, "\tmovq\t$%" PRId64 ", %s\n",
                                carg_imm, c->target->gp_arg_regs[gp_index[i]]))
            return false;
        } else {
          if (!ny_x64_nir_load_value(c, arg_vals[i]) ||
              !ny_native_printf(c->w, "\tmovq\t%%rax, %s\n",
                                c->target->gp_arg_regs[gp_index[i]]))
            return false;
        }
      }
    }
    if (c->target->shadow_space_bytes > 0 &&
        !ny_native_printf(c->w, "\tsubq\t$%zu, %%rsp\n",
                          c->target->shadow_space_bytes))
      return false;
    char fn_label[256];
    if (in->flags & NYIR_INST_F_EXTERN) {
      snprintf(fn_label, sizeof(fn_label), "%s%s",
               c->target->symbol_prefix, sym_name);
    } else {
      snprintf(fn_label, sizeof(fn_label), NY_FMT_FN,
               c->target->symbol_prefix, sym_name);
    }
    if (!ny_native_printf(c->w, "\tcall\t%s\n", fn_label))
      return false;
    if (c->target->shadow_space_bytes > 0 &&
        !ny_native_printf(c->w, "\taddq\t$%zu, %%rsp\n",
                          c->target->shadow_space_bytes))
      return false;
    if (stack_argc + pad > 0 &&
        !ny_native_printf(c->w, "\taddq\t$%d, %%rsp\n",
                          (stack_argc + pad) * 8))
      return false;
    if (in->dst >= 0) {
      if (in->dst < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->dst])
        return ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
      if (in->dst < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->dst])
        return ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
      return ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
    }
    return true;
  }
  default:
    ny_native_set_err(c->err, c->err_len,
                      "nyir x86-64: unsupported opcode %s",
                      nyir_op_name(in->op));
    return false;
  }
}

bool ny_native_x86_64_emit_nir(ny_native_writer_t *w,
                               const ny_native_target_info_t *target,
                               const nyir_func_t *nyir,
                               const char *func_name,
                               bool tag_return,
                               char *err, size_t err_len) {
  if (!w || !target || !nyir)
    return false;

  const char *name = func_name && func_name[0] ? func_name : "rt_main";

  ny_x64_nir_ctx_t ctx = {.w = w,
                          .target = target,
                          .nyir = nyir,
                          .frame_bytes = 0,
                          .max_local_slot = 0,
                          .label_prefix = name,
                          .err = err,
                          .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           ".Lnyir_epilogue_%s", name);
  memset(ctx.slot_offset, 0, sizeof(ctx.slot_offset));
  memset(ctx.fused_cmp, 0, sizeof(ctx.fused_cmp));
  ny_x64_nir_compute_frame(&ctx);
  ny_x64_nir_classify_values(&ctx);

  const char *sym = target->symbol_prefix;

  /*
   * Function header.
   */
  if (strcmp(target->object_format, "macho") == 0) {
    if (!ny_native_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else if (!ny_native_printf(w, "\t.type\t%s%s,@function\n", sym, name)) {
    return false;
  }
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name))
    return false;

  /*
   * Prologue: save rbp, allocate frame.
   */
  if (ctx.is_leaf) {
    if (ctx.frame_bytes > 0 &&
        !ny_native_printf(w, "\tsubq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
  } else {
    if (!ny_native_put(w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n"))
      return false;
    if (ctx.frame_bytes > 0 &&
        !ny_native_printf(w, "\tsubq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
  }

  /*
   * Detect parameters: locals loaded before any store to that slot.
   */
  bool *param_init = NULL;
  bool *stored = NULL;
  int max_local = ctx.max_local_slot;
  if (strcmp(name, "rt_main") != 0 && max_local > 0) {
    param_init = (bool *)calloc((size_t)max_local, sizeof(bool));
    stored = (bool *)calloc((size_t)max_local, sizeof(bool));
    if (nyir->param_count > 0 && nyir->param_count <= (size_t)max_local) {
      for (size_t i = 0; i < nyir->param_count; ++i)
        param_init[i] = true;
    } else {
      for (size_t i = 0; nyir && i < nyir->len && param_init && stored; ++i) {
        const nyir_inst_t *in = &nyir->data[i];
        int lid = (int)in->imm;
        if (in->op == NYIR_STORE_LOCAL && lid >= 0 && lid < max_local)
          stored[lid] = true;
        else if (in->op == NYIR_LOAD_LOCAL && lid >= 0 && lid < max_local &&
                 !stored[lid])
          param_init[lid] = true;
      }
    }
    int gp = 0;
    int sse = 0;
    int stack = 0;
    for (int i = 0; i < max_local; ++i) {
      if (!param_init || !param_init[i])
        continue;
      bool is_f64 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f64[i];
      bool is_f32 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f32[i];
      char dst[64];
      ny_x64_nir_slot_str(&ctx, i, dst, sizeof(dst));
      if ((is_f64 || is_f32) && sse < 8) {
        if (!ny_native_printf(w, "\t%s\t%%xmm%d, %s\n",
                              is_f32 ? "movss" : "movsd", sse, dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        sse++;
      } else if (!is_f64 && !is_f32 && gp < 6) {
        if (!ny_native_printf(w, "\tmovq\t%s, %s\n",
                              target->gp_arg_regs[gp], dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        gp++;
      } else {
        /*
         * Stack-passed parameters sit above the return address at entry.
         * A leaf frame has no %rbp save, so address it from the post-frame
         * %rsp there; non-leaf functions retain the conventional %rbp form.
         */
        int src_off = ctx.is_leaf
                          ? ctx.frame_bytes + 8 +
                                (int)target->shadow_space_bytes + stack * 8
                          : 16 + (int)target->shadow_space_bytes + stack * 8;
        const char *src_base = ctx.is_leaf ? "%rsp" : "%rbp";
        if (!ny_native_printf(w, "\tmovq\t%d(%s), %%rax\n", src_off, src_base) ||
            !ny_native_printf(w, "\tmovq\t%%rax, %s\n", dst)) {
          free(param_init);
          free(stored);
          return false;
        }
        stack++;
      }
    }
    free(param_init);
    free(stored);
  }

  /*
   * Emit each NYIR instruction.
   */
  for (size_t i = 0; i < nyir->len; ++i) {
    if (!ny_x64_nir_emit_inst(&ctx, &nyir->data[i], i)) {
      fprintf(stderr, "native NYIR repro (x86-64 emit failed):\n");
      nyir_dump(stderr, nyir, name);
      return false;
    }
  }

  /*
   * Epilogue.
   */
  if (!ny_native_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return && !ny_native_put(w, "\tleaq\t1(,%rax,2), %rax\n"))
    return false;
  if (ctx.is_leaf) {
    if (ctx.frame_bytes > 0 &&
        !ny_native_printf(w, "\taddq\t$%d, %%rsp\n", ctx.frame_bytes))
      return false;
    if (!ny_native_put(w, "\tret\n"))
      return false;
  } else {
    if (!ny_native_put(w, "\tleave\n\tret\n"))
      return false;
  }

  if (strcmp(target->object_format, "macho") != 0) {
    if (!ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
      return false;
  }
  return true;
}

/*
 * machine form scalar text emitter
 *
 * This is deliberately small and conservative: it is the first live
 * consumer of finalized machine form, not a second full backend.  The
 * established NYIR emitter remains the fallback for floats, pointers,
 * calls, aggregates, vectors, and any unsupported target shape.
 */

static bool ny_x64_mach_is_gpr(const ny_mach_func_t *mach,
                              const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         (mach->vreg_types[op->as.reg] == NY_MACH_TYPE_I64 ||
          mach->vreg_types[op->as.reg] == NY_MACH_TYPE_PTR);
}

static bool ny_x64_mach_is_i64(const ny_mach_func_t *mach,
                              const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NY_MACH_TYPE_I64;
}

static bool ny_x64_mach_is_ptr(const ny_mach_func_t *mach,
                              const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NY_MACH_TYPE_PTR;
}

static bool ny_x64_mach_is_f64(const ny_mach_func_t *mach,
                              const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NY_MACH_TYPE_F64;
}

static bool ny_x64_mach_is_f32(const ny_mach_func_t *mach,
                              const ny_mach_operand_t *op) {
  return op && op->kind == NY_MACH_OPERAND_VREG &&
         op->as.reg < mach->vreg_len &&
         mach->vreg_types[op->as.reg] == NY_MACH_TYPE_F32;
}

static bool ny_x64_mach_is_float(const ny_mach_func_t *mach,
                                const ny_mach_operand_t *op) {
  return ny_x64_mach_is_f64(mach, op) || ny_x64_mach_is_f32(mach, op);
}

/*
 * Formal parameters occupy a dense frame-slot prefix. Infer its end from the
 * highest slot read before its first write, preserving unused leading params.
 */
static bool ny_x64_mach_params(const ny_mach_func_t *mach, bool *params,
                              size_t *gp_count, size_t *fp_count) {
  if (!mach || !gp_count || !fp_count || (!params && mach->frame_slot_len))
    return false;
  bool *stored = calloc(mach->frame_slot_len, sizeof(*stored));
  if (mach->frame_slot_len && !stored)
    return false;
  size_t prefix = mach->param_count;
  if (prefix > mach->frame_slot_len) {
    free(stored);
    return false;
  }
  for (size_t i = 0; prefix == 0 && i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    if (in->opcode == NY_MACH_STORE && in->dst.kind == NY_MACH_OPERAND_FRAME) {
      if (in->dst.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      stored[in->dst.as.frame_index] = true;
    } else if (in->opcode == NY_MACH_LOAD &&
               in->src0.kind == NY_MACH_OPERAND_FRAME) {
      if (in->src0.as.frame_index >= mach->frame_slot_len) {
        free(stored);
        return false;
      }
      if (!stored[in->src0.as.frame_index] &&
          in->src0.as.frame_index + 1 > prefix)
        prefix = in->src0.as.frame_index + 1;
    }
  }
  for (size_t i = 0; i < prefix; ++i)
    params[i] = true;
  *gp_count = 0;
  *fp_count = 0;
  for (size_t i = 0; i < prefix; ++i) {
    if (mach->frame_slots[i].type == NY_MACH_TYPE_F32 ||
        mach->frame_slots[i].type == NY_MACH_TYPE_F64)
      ++*fp_count;
    else
      ++*gp_count;
  }
  free(stored);
  return true;
}

static bool ny_x64_mach_call_supported(const ny_mach_func_t *mach,
                                      const ny_native_target_info_t *target,
                                      const ny_mach_inst_t *in) {
  if (!mach || !target || !in || in->arg_sizes)
    return false;
  for (size_t arg = 0; arg < in->args_len; ++arg) {
    if (!ny_x64_mach_is_float(mach, &in->args[arg]) &&
        !ny_x64_mach_is_gpr(mach, &in->args[arg]))
      return false;
  }
  return target->abi == NY_NATIVE_ABI_SYSV ||
         target->abi == NY_NATIVE_ABI_WIN64;
}

static bool ny_x64_mach_scalar_supported(const ny_mach_func_t *mach,
                                        const ny_native_target_info_t *target,
                                        const char *name) {
  if (!mach || !target || !name)
    return false;
  bool *params = calloc(mach->frame_slot_len, sizeof(*params));
  if (mach->frame_slot_len && !params)
    return false;
  size_t gp_params = 0, fp_params = 0;
  bool params_ok = ny_x64_mach_params(mach, params, &gp_params, &fp_params);
  free(params);
  if (!params_ok ||
      (target->abi != NY_NATIVE_ABI_SYSV &&
       target->abi != NY_NATIVE_ABI_WIN64))
    return false;
  for (size_t v = 0; v < mach->vreg_len; ++v)
    if (mach->vreg_types[v] != NY_MACH_TYPE_I64 &&
        mach->vreg_types[v] != NY_MACH_TYPE_PTR &&
        mach->vreg_types[v] != NY_MACH_TYPE_F64 &&
        mach->vreg_types[v] != NY_MACH_TYPE_F32)
      return false;
  for (size_t s = 0; s < mach->frame_slot_len; ++s)
    if (mach->frame_slots[s].type != NY_MACH_TYPE_I64 &&
        mach->frame_slots[s].type != NY_MACH_TYPE_F64 &&
        mach->frame_slots[s].type != NY_MACH_TYPE_F32)
      return false;
  for (size_t i = 0; i < mach->inst_len; ++i) {
    const ny_mach_inst_t *in = &mach->insts[i];
    switch (in->opcode) {
    case NY_MACH_COPY:
      if ((!ny_x64_mach_is_gpr(mach, &in->dst) &&
           !ny_x64_mach_is_float(mach, &in->dst)) ||
          (in->src0.kind != NY_MACH_OPERAND_IMM &&
           ((ny_x64_mach_is_f64(mach, &in->dst) &&
             !ny_x64_mach_is_f64(mach, &in->src0)) ||
            (ny_x64_mach_is_f32(mach, &in->dst) &&
             !ny_x64_mach_is_f32(mach, &in->src0)) ||
            (!ny_x64_mach_is_float(mach, &in->dst) &&
             !ny_x64_mach_is_gpr(mach, &in->src0)))))
        return false;
      break;
    case NY_MACH_LEA:
      if (!ny_x64_mach_is_ptr(mach, &in->dst) ||
          (in->src0.kind != NY_MACH_OPERAND_FRAME &&
           in->src0.kind != NY_MACH_OPERAND_SYMBOL))
        return false;
      break;
    case NY_MACH_CONVERT:
      if (!((ny_x64_mach_is_f64(mach, &in->dst) &&
             (ny_x64_mach_is_i64(mach, &in->src0) ||
              ny_x64_mach_is_f32(mach, &in->src0))) ||
            (ny_x64_mach_is_f32(mach, &in->dst) &&
             (ny_x64_mach_is_i64(mach, &in->src0) ||
              ny_x64_mach_is_f64(mach, &in->src0)))))
        return false;
      break;
    case NY_MACH_SQRT:
    case NY_MACH_SIN:
    case NY_MACH_COS:
      if (!ny_x64_mach_is_f64(mach, &in->dst) ||
          !ny_x64_mach_is_f64(mach, &in->src0))
        return false;
      break;
    case NY_MACH_LOAD:
      if ((!ny_x64_mach_is_i64(mach, &in->dst) &&
           !ny_x64_mach_is_float(mach, &in->dst)) ||
          (in->src0.kind != NY_MACH_OPERAND_FRAME &&
           !ny_x64_mach_is_ptr(mach, &in->src0) &&
           !ny_x64_mach_is_i64(mach, &in->src0)))
        return false;
      break;
    case NY_MACH_STORE:
      if ((in->dst.kind != NY_MACH_OPERAND_FRAME &&
           !ny_x64_mach_is_ptr(mach, &in->dst) &&
           !ny_x64_mach_is_i64(mach, &in->dst)) ||
          (!ny_x64_mach_is_i64(mach, &in->src0) &&
           !ny_x64_mach_is_float(mach, &in->src0)))
        return false;
      break;
    case NY_MACH_ADD: case NY_MACH_SUB: case NY_MACH_MUL: case NY_MACH_DIV:
    case NY_MACH_MOD: case NY_MACH_AND: case NY_MACH_OR: case NY_MACH_XOR:
    case NY_MACH_SHL: case NY_MACH_SAR:
      if (!((ny_x64_mach_is_i64(mach, &in->dst) &&
             ny_x64_mach_is_i64(mach, &in->src0) &&
             ny_x64_mach_is_i64(mach, &in->src1)) ||
            ((in->opcode == NY_MACH_ADD || in->opcode == NY_MACH_SUB ||
              in->opcode == NY_MACH_MUL || in->opcode == NY_MACH_DIV) &&
             ny_x64_mach_is_f64(mach, &in->dst) &&
             ny_x64_mach_is_f64(mach, &in->src0) &&
             ny_x64_mach_is_f64(mach, &in->src1)) ||
            ((in->opcode == NY_MACH_ADD || in->opcode == NY_MACH_SUB ||
              in->opcode == NY_MACH_MUL || in->opcode == NY_MACH_DIV) &&
             ny_x64_mach_is_f32(mach, &in->dst) &&
             ny_x64_mach_is_f32(mach, &in->src0) &&
             ny_x64_mach_is_f32(mach, &in->src1))))
        return false;
      break;
    case NY_MACH_CMP:
      if (!(ny_x64_mach_is_i64(mach, &in->dst) &&
            ((ny_x64_mach_is_i64(mach, &in->src0) &&
              ny_x64_mach_is_i64(mach, &in->src1)) ||
             (ny_x64_mach_is_f64(mach, &in->src0) &&
              ny_x64_mach_is_f64(mach, &in->src1)) ||
             (ny_x64_mach_is_f32(mach, &in->src0) &&
              ny_x64_mach_is_f32(mach, &in->src1))) &&
            in->condition != NY_MACH_COND_ALWAYS))
        return false;
      break;
    case NY_MACH_RET:
      if (in->src0.kind != NY_MACH_OPERAND_NONE &&
          !ny_x64_mach_is_gpr(mach, &in->src0) &&
          !ny_x64_mach_is_float(mach, &in->src0))
        return false;
      break;
    case NY_MACH_BR:
      if (in->src1.kind != NY_MACH_OPERAND_BLOCK)
        return false;
      break;
    case NY_MACH_BR_IF:
      if (!ny_x64_mach_is_gpr(mach, &in->src0) ||
          in->src1.kind != NY_MACH_OPERAND_BLOCK)
        return false;
      break;
    case NY_MACH_TRAP:
      break;
    case NY_MACH_CALL:
      if ((in->dst.kind != NY_MACH_OPERAND_NONE &&
           !ny_x64_mach_is_gpr(mach, &in->dst) &&
           !ny_x64_mach_is_float(mach, &in->dst)) ||
          in->src0.kind != NY_MACH_OPERAND_SYMBOL ||
          !ny_x64_mach_call_supported(mach, target, in))
        return false;
      break;
    default:
      return false;
    }
  }
  return true;
}

static int ny_x64_mach_offset(const ny_mach_func_t *mach,
                             const ny_mach_operand_t *op) {
  if (op->kind == NY_MACH_OPERAND_FRAME)
    return (int)(op->as.frame_index + 1) * 8;
  return (int)(mach->frame_slot_len + op->as.reg + 1) * 8;
}

static const char *ny_x64_mach_setcc(ny_mach_cond_t condition) {
  switch (condition) {
  case NY_MACH_COND_EQ: return "sete";
  case NY_MACH_COND_NE: return "setne";
  case NY_MACH_COND_LT: return "setl";
  case NY_MACH_COND_LE: return "setle";
  case NY_MACH_COND_GT: return "setg";
  case NY_MACH_COND_GE: return "setge";
  default: return NULL;
  }
}

static const char *ny_x64_mach_float_setcc(ny_mach_cond_t condition) {
  switch (condition) {
  case NY_MACH_COND_EQ: return "sete";
  case NY_MACH_COND_NE: return "setne";
  case NY_MACH_COND_LT: return "setb";
  case NY_MACH_COND_LE: return "setbe";
  case NY_MACH_COND_GT: return "seta";
  case NY_MACH_COND_GE: return "setae";
  default: return NULL;
  }
}

/*
 * Emit `movq -off(%rbp), %rax` unless %rax already holds this vreg slot, as
 * tracked by rax_cached_off (set only when the previous instruction stored or
 * loaded that slot and nothing since clobbered %rax). Only vreg operands are
 * cached: frame slots can be address-taken and are always reloaded.
 */
static bool ny_x64_scalar_emit_rax_load(ny_native_writer_t *w,
                                        const ny_mach_operand_t *op, int off,
                                        int rax_cached_off) {
  if (op->kind == NY_MACH_OPERAND_VREG && rax_cached_off == off)
    return true;
  return ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n", off);
}

/*
 * 32-bit counterpart of ny_x64_scalar_emit_rax_load, for ny_mach_inst_t
 * narrow32 ops (see its field comment). A movl into %eax zero-extends the
 * upper 32 bits of %rax on real hardware, so the %rax cache stays a fully
 * valid 64-bit value afterward -- callers may keep treating
 * rax_cached_off the same way after a narrow load as after a full movq.
 */
static bool ny_x64_scalar_emit_eax_load(ny_native_writer_t *w,
                                        const ny_mach_operand_t *op, int off,
                                        int rax_cached_off) {
  if (op->kind == NY_MACH_OPERAND_VREG && rax_cached_off == off)
    return true;
  return ny_native_printf(w, "\tmovl\t-%d(%%rbp), %%eax\n", off);
}

bool ny_native_x86_64_emit_mach_scalar(ny_native_writer_t *w,
                                      const ny_native_target_info_t *target,
                                      const ny_mach_func_t *mach,
                                      const char *func_name, bool tag_return,
                                      char *err, size_t err_len) {
  const char *name = func_name && func_name[0] ? func_name : "rt_main";
  if (!w || !target || !ny_x64_mach_scalar_supported(mach, target, name))
    return false;
  if (tag_return) {
    for (size_t i = 0; i < mach->inst_len; ++i)
      if (mach->insts[i].opcode == NY_MACH_RET &&
          ny_x64_mach_is_float(mach, &mach->insts[i].src0))
        return false;
  }
  int frame_bytes = (int)((mach->frame_slot_len + mach->vreg_len) * 8);
  frame_bytes = (frame_bytes + 15) & ~15;
  const char *sym = target->symbol_prefix;
  char epilogue[160];
  snprintf(epilogue, sizeof(epilogue), ".Lny_mach_epilogue_%s", name);
  if ((strcmp(target->object_format, "macho") == 0 &&
       !ny_native_put(w, "\t.p2align 4, 0x90\n")) ||
      (strcmp(target->object_format, "macho") != 0 &&
       !ny_native_printf(w, "\t.type\t%s%s,@function\n", sym, name)) ||
      !ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n\tpushq\t%%rbp\n\tmovq\t%%rsp, %%rbp\n",
                        sym, name, sym, name) ||
      (frame_bytes && !ny_native_printf(w, "\tsubq\t$%d, %%rsp\n", frame_bytes)))
    return false;
  if (strcmp(name, "rt_main") != 0 && mach->frame_slot_len) {
    bool *params = calloc(mach->frame_slot_len, sizeof(*params));
    if (!params)
      return false;
    size_t gp_params = 0, fp_params = 0;
    if (!ny_x64_mach_params(mach, params, &gp_params, &fp_params)) {
      free(params);
      return false;
    }
    size_t arg = 0, fp_arg = 0, ordinal = 0, stack_arg = 0;
    for (size_t slot = 0; slot < mach->frame_slot_len; ++slot)
      if (params[slot]) {
        bool is_f32 = mach->frame_slots[slot].type == NY_MACH_TYPE_F32;
        bool is_f64 = mach->frame_slots[slot].type == NY_MACH_TYPE_F64;
        bool from_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? ordinal >= target->gp_arg_reg_count
                              : (is_f32 || is_f64)
                                    ? fp_arg >= target->fp_arg_reg_count
                                    : arg >= target->gp_arg_reg_count;
        size_t incoming = target->abi == NY_NATIVE_ABI_WIN64
                              ? 16 + ordinal * 8
                              : 16 + stack_arg * 8;
        bool saved = false;
        if (from_stack) {
          const char *move = is_f32 ? "movss" : is_f64 ? "movsd" : "movq";
          saved = ny_native_printf(w, "\t%s\t%zu(%%rbp), %s\n\t%s\t%s, -%zu(%%rbp)\n",
                                   move, incoming, is_f32 || is_f64 ? "%xmm0" : "%rax",
                                   move, is_f32 || is_f64 ? "%xmm0" : "%rax",
                                   (slot + 1) * 8);
          ++stack_arg;
        } else if (is_f32) {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : fp_arg;
          saved = ny_native_printf(w, "\tmovss\t%s, -%zu(%%rbp)\n",
                                   target->fp_arg_regs[index], (slot + 1) * 8);
        } else if (is_f64) {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : fp_arg;
          saved = ny_native_printf(w, "\tmovsd\t%s, -%zu(%%rbp)\n",
                                   target->fp_arg_regs[index], (slot + 1) * 8);
        } else {
          size_t index = target->abi == NY_NATIVE_ABI_WIN64 ? ordinal : arg;
          saved = ny_native_printf(w, "\tmovq\t%s, -%zu(%%rbp)\n",
                                   target->gp_arg_regs[index], (slot + 1) * 8);
        }
        if (!saved) {
          free(params);
          return false;
        }
      if (target->abi == NY_NATIVE_ABI_WIN64)
        ++ordinal;
      else if (is_f32 || is_f64)
        ++fp_arg;
      else
        ++arg;
      }
    free(params);
  }
  /*
   * Tracks the mach-offset of the vreg whose value currently sits live in
   * %rax within the current block, or -1 if unknown/stale. Only vregs are
   * ever cached here: frame slots can be address-taken (aliased through a
   * pointer), vregs by construction cannot be, so caching them is always
   * safe. Reset at every block boundary since %rax's contents on entry to
   * a block with multiple predecessors can't be assumed.
   */
  int rax_cached_off = -1;
  for (size_t block = 0; block < mach->block_len; ++block) {
    if (!ny_native_printf(w, ".Lny_mach_%s_%zu:\n", name, block))
      return false;
    rax_cached_off = -1;
    const ny_mach_block_t *b = &mach->blocks[block];
    for (size_t n = 0; n < b->inst_count; ++n) {
      const ny_mach_inst_t *in = &mach->insts[b->first_inst + n];
      int dst = ny_x64_mach_offset(mach, &in->dst);
      int a = ny_x64_mach_offset(mach, &in->src0);
      int b_off = ny_x64_mach_offset(mach, &in->src1);
      switch (in->opcode) {
      case NY_MACH_COPY:
        if (ny_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = ny_x64_mach_is_f32(mach, &in->dst);
          if (in->src0.kind == NY_MACH_OPERAND_IMM) {
            if (!ny_native_printf(w, "\tmovabsq\t$%" PRId64 ", %%rax\n\tmovq\t%%rax, %%xmm0\n",
                                  in->src0.as.imm)) return false;
          } else if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n",
                                       f32 ? "movss" : "movsd", a)) {
            return false;
          }
          if (!ny_native_printf(w, "\t%s\t%%xmm0, -%d(%%rbp)\n",
                                f32 ? "movss" : "movsd", dst)) return false;
          break;
        }
        if (in->src0.kind == NY_MACH_OPERAND_IMM) {
          if (!ny_native_printf(w, "\tmovabsq\t$%" PRId64 ", %%rax\n", in->src0.as.imm)) return false;
        } else if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (!ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst)) return false;
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG
                             ? dst
                             : (in->src0.kind == NY_MACH_OPERAND_VREG ? a : -1);
        break;
      case NY_MACH_LEA:
        if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
          if (!ny_native_printf(w, "\tleaq\t-%d(%%rbp), %%rax\n", a)) return false;
        } else if (!ny_native_printf(w, "\tleaq\t%s(%%rip), %%rax\n",
                                     in->src0.as.symbol)) {
          return false;
        }
        if (!ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst)) return false;
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NY_MACH_CONVERT:
        if (ny_x64_mach_is_f64(mach, &in->dst)) {
          if (ny_x64_mach_is_i64(mach, &in->src0)) {
            if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n\tcvtsi2sdq\t%%rax, %%xmm0\n", a)) return false;
          } else if (!ny_native_printf(w, "\tmovss\t-%d(%%rbp), %%xmm0\n\tcvtss2sd\t%%xmm0, %%xmm0\n", a)) {
            return false;
          }
          if (!ny_native_printf(w, "\tmovsd\t%%xmm0, -%d(%%rbp)\n", dst)) return false;
        } else {
          if (ny_x64_mach_is_i64(mach, &in->src0)) {
            if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n\tcvtsi2ssq\t%%rax, %%xmm0\n", a)) return false;
          } else if (!ny_native_printf(w, "\tmovsd\t-%d(%%rbp), %%xmm0\n\tcvtsd2ss\t%%xmm0, %%xmm0\n", a)) {
            return false;
          }
          if (!ny_native_printf(w, "\tmovss\t%%xmm0, -%d(%%rbp)\n", dst)) return false;
        }
        rax_cached_off = -1;
        break;
      case NY_MACH_SQRT:
        if (!ny_native_printf(w, "\tmovsd\t-%d(%%rbp), %%xmm0\n\tsqrtsd\t%%xmm0, %%xmm0\n\tmovsd\t%%xmm0, -%d(%%rbp)\n", a, dst))
          return false;
        rax_cached_off = -1;
        break;
      case NY_MACH_LOAD:
        if (ny_x64_mach_is_float(mach, &in->dst)) {
          const char *move = ny_x64_mach_is_f32(mach, &in->dst) ? "movss" : "movsd";
          if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
            if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n", move, a, move, dst)) return false;
          } else if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n\t%s\t(%%rax), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n", a, move, move, dst)) return false;
          rax_cached_off = -1;
          break;
        }
        if (in->src0.kind == NY_MACH_OPERAND_FRAME) {
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        } else {
          if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
          if (in->byte_width) {
            if (!ny_native_printf(w, "\tmovzbl\t(%%rax), %%eax\n")) return false;
          } else if (!ny_native_printf(w, "\tmovq\t(%%rax), %%rax\n")) {
            return false;
          }
        }
        if (!ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst)) return false;
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NY_MACH_STORE:
        if (ny_x64_mach_is_float(mach, &in->src0)) {
          const char *move = ny_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
            if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n", move, a, move, dst)) return false;
          } else if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rax\n\tmovq\t-%d(%%rbp), %%rcx\n\t%s\t(%%rax), %%xmm0\n\t%s\t%%xmm0, (%%rcx)\n", a, dst, move, move)) return false;
          rax_cached_off = -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (in->dst.kind == NY_MACH_OPERAND_FRAME) {
          if (!ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst)) return false;
        } else if (in->byte_width) {
          if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rcx\n\tmovb\t%%al, (%%rcx)\n", dst))
            return false;
        } else if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rcx\n\tmovq\t%%rax, (%%rcx)\n", dst)) {
          return false;
        }
        rax_cached_off = in->src0.kind == NY_MACH_OPERAND_VREG ? a : -1;
        break;
      case NY_MACH_ADD: case NY_MACH_SUB: case NY_MACH_AND: case NY_MACH_OR: case NY_MACH_XOR:
      case NY_MACH_MUL:
        if (ny_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = ny_x64_mach_is_f32(mach, &in->dst);
          const char *op = in->opcode == NY_MACH_ADD ? "addsd" :
                           in->opcode == NY_MACH_SUB ? "subsd" : "mulsd";
          if (f32) op = in->opcode == NY_MACH_ADD ? "addss" :
                        in->opcode == NY_MACH_SUB ? "subss" : "mulss";
          if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                                f32 ? "movss" : "movsd", a, op, b_off,
                                f32 ? "movss" : "movsd", dst)) return false;
          rax_cached_off = -1;
          break;
        }
        if (in->narrow32) {
          /*
           * Non-negative i32-safe operands/result (see ny_mach_inst_t's
           * narrow32 field): 32-bit load + op zero-extends on write, and
           * the store below is still the full 64-bit %rax, so this is
           * bit-identical to the 64-bit path below but a byte shorter
           * per instruction (no REX.W).
           */
          if (!ny_x64_scalar_emit_eax_load(w, &in->src0, a, rax_cached_off)) return false;
          { const char *op = in->opcode == NY_MACH_ADD ? "addl" : in->opcode == NY_MACH_SUB ? "subl" :
                             in->opcode == NY_MACH_AND ? "andl" : in->opcode == NY_MACH_OR ? "orl" :
                             in->opcode == NY_MACH_XOR ? "xorl" : "imull";
            if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%eax\n\tmovq\t%%rax, -%d(%%rbp)\n", op, b_off, dst)) return false; }
          rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        { const char *op = in->opcode == NY_MACH_ADD ? "addq" : in->opcode == NY_MACH_SUB ? "subq" :
                           in->opcode == NY_MACH_AND ? "andq" : in->opcode == NY_MACH_OR ? "orq" :
                           in->opcode == NY_MACH_XOR ? "xorq" : "imulq";
          if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%rax\n\tmovq\t%%rax, -%d(%%rbp)\n", op, b_off, dst)) return false; }
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NY_MACH_DIV: case NY_MACH_MOD:
        if (ny_x64_mach_is_float(mach, &in->dst)) {
          bool f32 = ny_x64_mach_is_f32(mach, &in->dst);
          if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t%%xmm0, -%d(%%rbp)\n",
                                f32 ? "movss" : "movsd", a,
                                f32 ? "divss" : "divsd", b_off,
                                f32 ? "movss" : "movsd", dst)) return false;
          rax_cached_off = -1;
          break;
        }
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (!ny_native_printf(w, "\tcqto\n\tidivq\t-%d(%%rbp)\n\tmovq\t%%%s, -%d(%%rbp)\n",
                              b_off, in->opcode == NY_MACH_DIV ? "rax" : "rdx", dst)) return false;
        rax_cached_off = in->opcode == NY_MACH_DIV &&
                                 in->dst.kind == NY_MACH_OPERAND_VREG
                             ? dst
                             : -1;
        break;
      case NY_MACH_SHL: case NY_MACH_SAR:
        if (!ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %%rcx\n\t%s\t%%cl, %%rax\n\tmovq\t%%rax, -%d(%%rbp)\n",
                              b_off, in->opcode == NY_MACH_SHL ? "shlq" : "sarq", dst)) return false;
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
        break;
      case NY_MACH_CMP: {
        if (ny_x64_mach_is_float(mach, &in->src0)) {
          const char *cc = ny_x64_mach_float_setcc(in->condition);
          const char *cmp = ny_x64_mach_is_f32(mach, &in->src0) ? "ucomiss" : "ucomisd";
          const char *move = ny_x64_mach_is_f32(mach, &in->src0) ? "movss" : "movsd";
          int unordered_result = in->condition == NY_MACH_COND_NE ? 1 : 0;
          char unordered[192];
          char done[192];
          snprintf(unordered, sizeof(unordered), ".Lny_mach_fcmp_unordered_%s_%u",
                   name, in->dst.as.reg);
          snprintf(done, sizeof(done), ".Lny_mach_fcmp_done_%s_%u", name,
                   in->dst.as.reg);
          if (!cc || !ny_native_printf(w,
              "\t%s\t-%d(%%rbp), %%xmm0\n\t%s\t-%d(%%rbp), %%xmm1\n\t%s\t%%xmm1, %%xmm0\n\tjp\t%s\n\t%s\t%%al\n\tjmp\t%s\n%s:\n\tmovb\t$%d, %%al\n%s:\n\tmovzbq\t%%al, %%rax\n\tmovq\t%%rax, -%d(%%rbp)\n",
              move, a, move, b_off, cmp, unordered, cc, done, unordered,
              unordered_result, done, dst)) return false;
          rax_cached_off = -1;
          break;
        }
        const char *cc = ny_x64_mach_setcc(in->condition);
        if (!cc || !ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (!cc || !ny_native_printf(w, "\tcmpq\t-%d(%%rbp), %%rax\n\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n\tmovq\t%%rax, -%d(%%rbp)\n", b_off, cc, dst)) return false;
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG ? dst : -1;
        break;
      }
      case NY_MACH_BR:
        if (!ny_native_printf(w, "\tjmp\t.Lny_mach_%s_%u\n", name, in->src1.as.block_index)) return false;
        break;
      case NY_MACH_BR_IF:
        if (!ny_native_printf(w, "\tcmpq\t$0, -%d(%%rbp)\n\tjne\t.Lny_mach_%s_%u\n", a, name, in->src1.as.block_index)) return false;
        break;
      case NY_MACH_TRAP:
        /*
         * ud2 — raises #UD, terminating the process.
         */
        if (!ny_native_printf(w, "\tud2\n")) return false;
        break;
      case NY_MACH_CALL:
        if (in->src0.kind == NY_MACH_OPERAND_SYMBOL &&
            in->src0.as.symbol &&
            strcmp(in->src0.as.symbol, "rt_native_sqrt_f64") == 0 &&
            in->args_len == 1) {
          int arg_off = ny_x64_mach_offset(mach, &in->args[0]);
          if (!ny_native_printf(w, "\tmovsd\t-%d(%%rbp), %%xmm0\n\tsqrtsd\t%%xmm0, %%xmm0\n\tmovsd\t%%xmm0, -%d(%%rbp)\n", arg_off, dst))
            return false;
          rax_cached_off = -1;
          break;
        }
        { size_t gp = 0, fp = 0, stack_args = 0;
        for (size_t arg = 0; arg < in->args_len; ++arg) {
          bool is_float = ny_x64_mach_is_float(mach, &in->args[arg]);
          bool on_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? arg >= target->gp_arg_reg_count
                              : is_float ? fp >= target->fp_arg_reg_count
                                         : gp >= target->gp_arg_reg_count;
          if (on_stack)
            ++stack_args;
          else if (target->abi == NY_NATIVE_ABI_SYSV && is_float)
            ++fp;
          else if (target->abi == NY_NATIVE_ABI_SYSV)
            ++gp;
        }
        size_t call_bytes = target->shadow_space_bytes + stack_args * 8;
        size_t stack_bytes = (call_bytes + 15) & ~(size_t)15;
        if (stack_bytes &&
            !ny_native_printf(w, "\tsubq\t$%zu, %%rsp\n", stack_bytes))
          return false;
        gp = fp = 0;
        size_t stack_arg = 0;
        for (size_t arg = 0; arg < in->args_len; ++arg) {
          int arg_off = ny_x64_mach_offset(mach, &in->args[arg]);
          size_t gp_index = target->abi == NY_NATIVE_ABI_WIN64 ? arg : gp;
          size_t fp_index = target->abi == NY_NATIVE_ABI_WIN64 ? arg : fp;
          bool is_f32 = ny_x64_mach_is_f32(mach, &in->args[arg]);
          bool is_f64 = ny_x64_mach_is_f64(mach, &in->args[arg]);
          bool on_stack = target->abi == NY_NATIVE_ABI_WIN64
                              ? arg >= target->gp_arg_reg_count
                              : (is_f32 || is_f64)
                                    ? fp >= target->fp_arg_reg_count
                                    : gp >= target->gp_arg_reg_count;
          if (on_stack) {
            size_t offset = target->shadow_space_bytes + stack_arg * 8;
            const char *move = is_f32 ? "movss" : is_f64 ? "movsd" : "movq";
            const char *scratch = is_f32 || is_f64 ? "%xmm0" : "%rax";
            if (!ny_native_printf(w, "\t%s\t-%d(%%rbp), %s\n\t%s\t%s, %zu(%%rsp)\n",
                                  move, arg_off, scratch, move, scratch, offset))
              return false;
            ++stack_arg;
          } else if (is_f32) {
            if (!ny_native_printf(w, "\tmovss\t-%d(%%rbp), %s\n", arg_off,
                                  target->fp_arg_regs[fp_index])) return false;
            if (target->abi == NY_NATIVE_ABI_SYSV)
              ++fp;
          } else if (is_f64) {
            if (!ny_native_printf(w, "\tmovsd\t-%d(%%rbp), %s\n", arg_off,
                                  target->fp_arg_regs[fp_index])) return false;
            if (target->abi == NY_NATIVE_ABI_SYSV)
              ++fp;
          } else if (!ny_native_printf(w, "\tmovq\t-%d(%%rbp), %s\n", arg_off,
                                       target->gp_arg_regs[gp_index])) return false;
          else if (target->abi == NY_NATIVE_ABI_SYSV)
            ++gp;
        }
        if (!ny_native_printf(w, "\tcall\t%s%s%s\n", target->symbol_prefix,
                              in->call_is_extern ? "" : "ny_fn_",
                              in->src0.as.symbol)) return false;
        if (stack_bytes &&
            !ny_native_printf(w, "\taddq\t$%zu, %%rsp\n", stack_bytes)) return false;
        if (ny_x64_mach_is_f32(mach, &in->dst)) {
          if (!ny_native_printf(w, "\tmovss\t%%xmm0, -%d(%%rbp)\n", dst)) return false;
        } else if (ny_x64_mach_is_f64(mach, &in->dst)) {
          if (!ny_native_printf(w, "\tmovsd\t%%xmm0, -%d(%%rbp)\n", dst)) return false;
        } else if (in->dst.kind != NY_MACH_OPERAND_NONE &&
                   !ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst)) return false;
        }
        rax_cached_off = in->dst.kind == NY_MACH_OPERAND_VREG &&
                                 !ny_x64_mach_is_float(mach, &in->dst)
                             ? dst
                             : -1;
        break;
      case NY_MACH_RET:
        if (ny_x64_mach_is_float(mach, &in->src0)) {
          if (ny_x64_mach_is_f32(mach, &in->src0)) {
            if (!ny_native_printf(w, "\tmovss\t-%d(%%rbp), %%xmm0\n\tmovd\t%%xmm0, %%eax\n", a)) return false;
          } else if (!ny_native_printf(w, "\tmovsd\t-%d(%%rbp), %%xmm0\n\tmovq\t%%xmm0, %%rax\n", a)) return false;
        } else if (in->src0.kind != NY_MACH_OPERAND_NONE &&
            !ny_x64_scalar_emit_rax_load(w, &in->src0, a, rax_cached_off)) return false;
        if (!ny_native_printf(w, "\tjmp\t%s\n", epilogue)) return false;
        rax_cached_off = -1;
        break;
      default:
        return false;
      }
    }
  }
  if (!ny_native_printf(w, "%s:\n", epilogue) ||
      (tag_return && !ny_native_put(w, "\tleaq\t1(,%rax,2), %rax\n")) ||
      !ny_native_put(w, "\tleave\n\tret\n"))
    return false;
  if (strcmp(target->object_format, "macho") != 0 &&
      !ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
    return false;
  (void)err; (void)err_len;
  return true;
}
