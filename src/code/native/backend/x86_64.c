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
  ctx->frame_bytes = ((raw + 15) & ~15) + 8;
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
  ctx->frame_bytes = ((raw + 15) & ~15) + 8;
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
  if (!ny_x64_emit_expr_raw(ctx, e->as.binary.left) ||
      !ny_native_put(ctx->w, "\tpushq\t%rax\n") ||
      !ny_x64_emit_expr_raw(ctx, e->as.binary.right) ||
      !ny_native_put(ctx->w, "\tmovq\t%rax, %rbx\n") ||
      !ny_native_put(ctx->w, "\tpopq\t%rax\n"))
    return false;

  if (strcmp(op, "+") == 0)
    return ny_native_put(ctx->w, "\taddq\t%rbx, %rax\n");
  if (strcmp(op, "-") == 0)
    return ny_native_put(ctx->w, "\tsubq\t%rbx, %rax\n");
  if (strcmp(op, "*") == 0)
    return ny_native_put(ctx->w, "\timulq\t%rbx, %rax\n");
  if (strcmp(op, "/") == 0)
    return ny_native_put(ctx->w, "\tcqto\n\tidivq\t%rbx\n");
  if (strcmp(op, "%") == 0)
    return ny_native_put(ctx->w,
                         "\tcqto\n\tidivq\t%rbx\n\tmovq\t%rdx, %rax\n");
  if (strcmp(op, "&") == 0)
    return ny_native_put(ctx->w, "\tandq\t%rbx, %rax\n");
  if (strcmp(op, "|") == 0)
    return ny_native_put(ctx->w, "\torq\t%rbx, %rax\n");
  if (strcmp(op, "^^") == 0)
    return ny_native_put(ctx->w, "\txorq\t%rbx, %rax\n");
  if (strcmp(op, "<<") == 0)
    return ny_native_put(ctx->w, "\tmovb\t%bl, %cl\n\tshlq\t%cl, %rax\n");
  if (strcmp(op, ">>") == 0)
    return ny_native_put(ctx->w, "\tmovb\t%bl, %cl\n\tsarq\t%cl, %rax\n");
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
                            "\tcmpq\t%%rbx, %%rax\n\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
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
           ny_native_put(ctx->w, "\tmovq\t%rax, %rbx\n") &&
           ny_native_put(ctx->w, "\tpopq\t%rax\n") &&
           ny_native_put(ctx->w, "\tcmpq\t%rbx, %rax\n") &&
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
  return ny_native_put(ctx->w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n\tpushq\t%rbx\n") &&
         (ctx->frame_bytes <= 0 ||
          ny_native_printf(ctx->w, "\tsubq\t$%d, %%rsp\n", ctx->frame_bytes)) &&
         ny_native_put(ctx->w, "\txorq\t%rax, %rax\n");
}

static bool ny_x64_emit_epilogue(ny_x64_ctx_t *ctx) {
  if (!ny_native_printf(ctx->w, "%s:\n", ctx->epilogue_label))
    return false;
  if (!ctx->raw_return && !ny_x64_tag_rax(ctx))
    return false;
  return ny_native_put(ctx->w, "\tmovq\t-8(%rbp), %rbx\n\tleave\n\tret\n");
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

/* ------------------------------------------------------------------ */
/* NYIR -> x86-64 instruction selection                                */
/*                                                                    */
/* The NYIR is already optimized (constant-folded, copy-propagated,   */
/* DCE'd).  We map each live NYIR value to a dedicated stack slot so  */
/* that multi-use values survive across instruction boundaries.  The   */
/* optimizer has already removed most redundant slots.               */
/* ------------------------------------------------------------------ */

#define NY_X64_NIR_MAX_SLOTS 4096

typedef struct {
  ny_native_writer_t *w;
  const ny_native_target_info_t *target;
  const ny_nir_func_t *nir;
  int slot_offset[NY_X64_NIR_MAX_SLOTS];
  int frame_slots;
  int frame_bytes;
  int max_local_slot;
  bool value_f64[NY_X64_NIR_MAX_SLOTS];
  bool value_f32[NY_X64_NIR_MAX_SLOTS];
  bool local_f64[NY_X64_NIR_MAX_SLOTS];
  bool local_f32[NY_X64_NIR_MAX_SLOTS];
  char epilogue_label[128];
  char *err;
  size_t err_len;
} ny_x64_nir_ctx_t;

static int ny_x64_nir_slot(ny_x64_nir_ctx_t *c, int value_id) {
  if (value_id < 0 || value_id >= c->nir->next_value)
    return -1;
  int s = c->frame_slots + value_id;
  c->slot_offset[s] = (s + 1) * 8;
  return s;
}

static void ny_x64_nir_compute_frame(ny_x64_nir_ctx_t *c) {
  int max_val = c->nir->next_value;
  int max_local = -1;
  for (size_t i = 0; i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if ((in->op == NY_NIR_LOAD_LOCAL || in->op == NY_NIR_STORE_LOCAL) &&
        (int)in->imm > max_local)
      max_local = (int)in->imm;
  }
  c->max_local_slot = max_local + 1;
  c->frame_slots = c->max_local_slot;
  /* Pre-compute slot offsets for all NYIR values. */
  for (int v = 0; v < max_val; ++v)
    ny_x64_nir_slot(c, v);
  int total = c->frame_slots + max_val;
  int raw = total * 8;
  c->frame_bytes = ((raw + 15) & ~15) + 8;
}

static bool ny_x64_nir_load(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0)
    return false;
  return ny_native_printf(c->w, "\tmovq\t-%d(%%rbp), %%rax\n",
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_store(ny_x64_nir_ctx_t *c, int slot) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0)
    return false;
  return ny_native_printf(c->w, "\tmovq\t%%rax, -%d(%%rbp)\n",
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_load_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0 ||
      xmm < 0 || xmm > 15)
    return false;
  return ny_native_printf(c->w, "\tmovsd\t-%d(%%rbp), %%xmm%d\n",
                          c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0 ||
      xmm < 0 || xmm > 15)
    return false;
  return ny_native_printf(c->w, "\tmovsd\t%%xmm%d, -%d(%%rbp)\n", xmm,
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_load_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0 ||
      xmm < 0 || xmm > 15)
    return false;
  return ny_native_printf(c->w, "\tmovss\t-%d(%%rbp), %%xmm%d\n",
                          c->slot_offset[slot], xmm);
}

static bool ny_x64_nir_store_xmm_f32(ny_x64_nir_ctx_t *c, int slot, int xmm) {
  if (slot < 0 || slot >= NY_X64_NIR_MAX_SLOTS || c->slot_offset[slot] <= 0 ||
      xmm < 0 || xmm > 15)
    return false;
  return ny_native_printf(c->w, "\tmovss\t%%xmm%d, -%d(%%rbp)\n", xmm,
                          c->slot_offset[slot]);
}

static bool ny_x64_nir_op_is_f64(ny_nir_op_t op) {
  return op == NYIR_CONST_F64 || op == NYIR_ADD_F64 ||
         op == NYIR_SUB_F64 || op == NYIR_MUL_F64 ||
         op == NYIR_DIV_F64 || op == NYIR_I64_TO_F64 ||
         op == NYIR_F32_TO_F64;
}

static bool ny_x64_nir_op_is_f32(ny_nir_op_t op) {
  return op == NYIR_CONST_F32 || op == NYIR_ADD_F32 ||
         op == NYIR_SUB_F32 || op == NYIR_MUL_F32 ||
         op == NYIR_DIV_F32 || op == NYIR_I64_TO_F32 ||
         op == NYIR_F64_TO_F32;
}

static void ny_x64_nir_classify_values(ny_x64_nir_ctx_t *c) {
  if (!c || !c->nir)
    return;
  for (size_t i = 0; i < c->nir->len; ++i) {
    const ny_nir_inst_t *in = &c->nir->data[i];
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (ny_x64_nir_op_is_f64(in->op) ||
         (in->op == NY_NIR_CALL && (in->flags & NY_NIR_INST_F_RET_F64))))
      c->value_f64[in->dst] = true;
    if (in->dst >= 0 && in->dst < NY_X64_NIR_MAX_SLOTS &&
        (ny_x64_nir_op_is_f32(in->op) ||
         ((in->flags & NYIR_INST_F_RET_F32) &&
          in->op == NY_NIR_CALL)))
      c->value_f32[in->dst] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < c->nir->len; ++i) {
      const ny_nir_inst_t *in = &c->nir->data[i];
      if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < NY_X64_NIR_MAX_SLOTS && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->value_f64[in->a] && !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_COPY && in->dst >= 0 && in->a >= 0 &&
          in->dst < NY_X64_NIR_MAX_SLOTS && in->a < NY_X64_NIR_MAX_SLOTS &&
          c->value_f32[in->a] && !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f64[in->imm] &&
                 !c->value_f64[in->dst]) {
        c->value_f64[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->local_f32[in->imm] &&
                 !c->value_f32[in->dst]) {
        c->value_f32[in->dst] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->dst] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_LOAD_LOCAL && in->dst >= 0 && in->imm >= 0 &&
                 in->dst < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->dst] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a] &&
                 !c->local_f64[in->imm]) {
        c->local_f64[in->imm] = true;
        changed = true;
      } else if (in->op == NY_NIR_STORE_LOCAL && in->a >= 0 && in->imm >= 0 &&
                 in->a < NY_X64_NIR_MAX_SLOTS &&
                 in->imm < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a] &&
                 !c->local_f32[in->imm]) {
        c->local_f32[in->imm] = true;
        changed = true;
      }
      if ((in->op == NYIR_ADD_F64 || in->op == NYIR_SUB_F64 ||
           in->op == NYIR_MUL_F64 || in->op == NYIR_DIV_F64)) {
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

static const char *ny_x64_nir_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return "sete";
  case NY_NIR_CMP_NE:
    return "setne";
  case NY_NIR_CMP_LT:
    return "setl";
  case NY_NIR_CMP_LE:
    return "setle";
  case NY_NIR_CMP_GT:
    return "setg";
  case NY_NIR_CMP_GE:
    return "setge";
  }
  return "sete";
}

static const char *ny_x64_nir_f64_setcc(ny_nir_cmp_t cmp) {
  switch (cmp) {
  case NY_NIR_CMP_EQ:
    return "sete";
  case NY_NIR_CMP_NE:
    return "setne";
  case NY_NIR_CMP_LT:
    return "setb";
  case NY_NIR_CMP_LE:
    return "setbe";
  case NY_NIR_CMP_GT:
    return "seta";
  case NY_NIR_CMP_GE:
    return "setae";
  }
  return "sete";
}

static bool ny_x64_nir_emit_inst(ny_x64_nir_ctx_t *c,
                                  const ny_nir_inst_t *in) {
  switch (in->op) {
  case NY_NIR_NOP:
    return true;
  case NY_NIR_CONST_I64:
  case NYIR_CONST_F64:
  case NYIR_CONST_F32:
    if (in->dst < 0)
      return true;
    c->slot_offset[ny_x64_nir_slot(c, in->dst)] = 0; /* ensure allocated */
    ny_x64_nir_slot(c, in->dst);
    return ny_native_printf(c->w, "\tmovabsq\t$%" PRId64 ", %%rax\n", in->imm) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_COPY:
    if (in->dst < 0)
      return true;
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_LOAD_LOCAL:
    if (in->dst < 0)
      return true;
    /* Load from a local (param/mut) slot into a value slot. */
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
    return ny_native_printf(c->w, "\tleaq\t-%d(%%rbp), %%rax\n",
                            c->slot_offset[in->imm]) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_STORE_LOCAL:
    if (in->imm < 0 || (int)in->imm >= c->max_local_slot) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: store.local invalid slot %" PRId64, in->imm);
      return false;
    }
    c->slot_offset[in->imm] = (in->imm + 1) * 8;
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_x64_nir_store(c, (int)in->imm);
  case NYIR_LOAD_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tmovq\t(%rax), %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_STORE_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->c)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpopq\t%rbx\n\tmovq\t%rbx, (%rax)\n");
  case NY_NIR_ADD_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\taddq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_SUB_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tsubq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_MUL_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\timulq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_DIV_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tcqto\n\tidivq\t%rbx\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_MOD_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tcqto\n\tidivq\t%rbx\n\tmovq\t%rdx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_AND_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tandq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_OR_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\torq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_XOR_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\txorq\t%rbx, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_SHL_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tmovb\t%bl, %cl\n\tshlq\t%cl, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NY_NIR_SAR_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tmovb\t%bl, %cl\n\tsarq\t%cl, %rax\n") &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_ADD_F64:
  case NYIR_SUB_F64:
  case NYIR_MUL_F64:
  case NYIR_DIV_F64: {
    const char *insn = in->op == NYIR_ADD_F64 ? "addsd" :
                       in->op == NYIR_SUB_F64 ? "subsd" :
                       in->op == NYIR_MUL_F64 ? "mulsd" : "divsd";
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\t%s\t-%d(%%rbp), %%xmm0\n", insn,
                            c->slot_offset[bslot]) &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tcvtsi2sdq\t%rax, %xmm0\n") &&
           ny_x64_nir_store_xmm(c, ny_x64_nir_slot(c, in->dst), 0);
  case NYIR_ADD_F32:
  case NYIR_SUB_F32:
  case NYIR_MUL_F32:
  case NYIR_DIV_F32: {
    const char *insn = in->op == NYIR_ADD_F32 ? "addss" :
                       in->op == NYIR_SUB_F32 ? "subss" :
                       in->op == NYIR_MUL_F32 ? "mulss" : "divss";
    int bslot = ny_x64_nir_slot(c, in->b);
    return ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0) &&
           ny_native_printf(c->w, "\t%s\t-%d(%%rbp), %%xmm0\n", insn,
                            c->slot_offset[bslot]) &&
           ny_x64_nir_store_xmm_f32(c, ny_x64_nir_slot(c, in->dst), 0);
  }
  case NYIR_I64_TO_F32:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
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
  case NY_NIR_CMP_I64:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\tpushq\t%rax\n") &&
           ny_x64_nir_load(c, ny_x64_nir_slot(c, in->b)) &&
           ny_native_put(c->w, "\tmovq\t%rax, %rbx\n\tpopq\t%rax\n") &&
           ny_native_put(c->w, "\tcmpq\t%rbx, %rax\n") &&
           ny_native_printf(c->w, "\t%s\t%%al\n\tmovzbq\t%%al, %%rax\n",
                           ny_x64_nir_setcc(in->cmp)) &&
           ny_x64_nir_store(c, ny_x64_nir_slot(c, in->dst));
  case NYIR_CMP_F64: {
    char unordered[96];
    char done[96];
    snprintf(unordered, sizeof(unordered), ".Lny_nir_fcmp_unordered_%d", in->dst);
    snprintf(done, sizeof(done), ".Lny_nir_fcmp_done_%d", in->dst);
    int unordered_result = in->cmp == NY_NIR_CMP_NE ? 1 : 0;
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
    snprintf(unordered, sizeof(unordered), ".Lny_nir_fcmp_unordered_%d", in->dst);
    snprintf(done, sizeof(done), ".Lny_nir_fcmp_done_%d", in->dst);
    int unordered_result = in->cmp == NY_NIR_CMP_NE ? 1 : 0;
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
  case NY_NIR_LABEL:
    return ny_native_printf(c->w, ".Lny_nir_L%" PRId64 ":\n", in->imm);
  case NY_NIR_BR:
    return ny_native_printf(c->w, "\tjmp\t.Lny_nir_L%" PRId64 "\n", in->imm);
  case NY_NIR_BR_IF:
    return ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a)) &&
           ny_native_put(c->w, "\ttestq\t%rax, %rax\n") &&
           ny_native_printf(c->w, "\tjne\t.Lny_nir_L%" PRId64 "\n", in->imm);
  case NY_NIR_RET: {
    /* Load return value into %rax (skip if -1 = void return). */
      if (in->a >= 0) {
      if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f64[in->a]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (in->a < NY_X64_NIR_MAX_SLOTS && c->value_f32[in->a]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, in->a), 0))
          return false;
      } else if (!ny_x64_nir_load(c, ny_x64_nir_slot(c, in->a))) {
        return false;
      }
    } else {
      if (!ny_native_put(c->w, "\txorq\t%rax, %rax\n"))
        return false;
    }
    return ny_native_printf(c->w, "\tjmp\t%s\n", c->epilogue_label);
  }
  case NY_NIR_CALL: {
    int argc = (int)in->imm;
    const char *sym_name = in->symbol ? in->symbol : "<null>";
    if (argc < 0 || argc > NY_NIR_CALL_MAX_ARGS) {
      ny_native_set_err(c->err, c->err_len,
                        "nyir x86-64: call exceeds the maximum supported argument count");
      return false;
    }
    int arg_vals[NY_NIR_CALL_MAX_ARGS];
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
    bool arg_f64[NY_NIR_CALL_MAX_ARGS] = {0};
    bool arg_f32[NY_NIR_CALL_MAX_ARGS] = {0};
    int gp_index[NY_NIR_CALL_MAX_ARGS];
    int sse_index[NY_NIR_CALL_MAX_ARGS];
    int gp = 0;
    int sse = 0;
    int stack_argc = 0;
    for (int i = 0; i < argc; ++i) {
      gp_index[i] = -1;
      sse_index[i] = -1;
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

    /* Stack-passed args permanently move %rsp for
     * the duration of the call; pad to an even slot count so %rsp is
     * 16-byte aligned at the `call` instruction (shadow space, if any, is
     * itself a multiple of 16). */
    int pad = stack_argc % 2;
    if (pad && !ny_native_put(c->w, "\tsubq\t$8, %rsp\n"))
      return false;
    /* Push stack args highest-index first so the first stack arg ends up
     * closest to the top of stack (lowest address) at call time. */
    for (int i = argc - 1; i >= 0; --i) {
      if (gp_index[i] >= 0 || sse_index[i] >= 0)
        continue;
      if (arg_f64[i]) {
        if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !ny_native_put(c->w, "\tsubq\t$8, %rsp\n\tmovsd\t%xmm0, (%rsp)\n"))
          return false;
      } else if (arg_f32[i]) {
        if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]), 0) ||
            !ny_native_put(c->w, "\tsubq\t$8, %rsp\n\tmovss\t%xmm0, (%rsp)\n"))
          return false;
      } else if (!ny_x64_nir_load(c, ny_x64_nir_slot(c, arg_vals[i])) ||
                 !ny_native_put(c->w, "\tpushq\t%rax\n")) {
        return false;
      }
    }
    for (int i = 0; i < argc; ++i) {
      if (sse_index[i] >= 0) {
        if (arg_f32[i]) {
          if (!ny_x64_nir_load_xmm_f32(c, ny_x64_nir_slot(c, arg_vals[i]), sse_index[i]))
            return false;
        } else if (!ny_x64_nir_load_xmm(c, ny_x64_nir_slot(c, arg_vals[i]), sse_index[i])) {
          return false;
        }
      } else if (gp_index[i] >= 0) {
        if (!ny_x64_nir_load(c, ny_x64_nir_slot(c, arg_vals[i])) ||
            !ny_native_printf(c->w, "\tmovq\t%%rax, %s\n",
                              c->target->gp_arg_regs[gp_index[i]]))
          return false;
      }
    }
    if (c->target->shadow_space_bytes > 0 &&
        !ny_native_printf(c->w, "\tsubq\t$%zu, %%rsp\n",
                          c->target->shadow_space_bytes))
      return false;
    char fn_label[256];
    if (in->flags & NY_NIR_INST_F_EXTERN) {
      snprintf(fn_label, sizeof(fn_label), "%s%s",
               c->target->symbol_prefix, sym_name);
    } else {
      snprintf(fn_label, sizeof(fn_label), "%s%sny_fn_%s",
               c->target->symbol_prefix,
               c->target->symbol_prefix[0] ? "" : "",
               sym_name);
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
                      ny_nir_op_name(in->op));
    return false;
  }
}

bool ny_native_x86_64_emit_nir(ny_native_writer_t *w,
                               const ny_native_target_info_t *target,
                               const ny_nir_func_t *nir,
                               const char *func_name,
                               bool tag_return,
                               char *err, size_t err_len) {
  if (!w || !target || !nir)
    return false;

  const char *name = func_name && func_name[0] ? func_name : "rt_main";

  ny_x64_nir_ctx_t ctx = {.w = w,
                          .target = target,
                          .nir = nir,
                          .frame_bytes = 0,
                          .max_local_slot = 0,
                          .err = err,
                          .err_len = err_len};
  snprintf(ctx.epilogue_label, sizeof(ctx.epilogue_label),
           ".Lny_nir_epilogue_%s", name);
  memset(ctx.slot_offset, 0, sizeof(ctx.slot_offset));
  ny_x64_nir_compute_frame(&ctx);
  ny_x64_nir_classify_values(&ctx);

  const char *sym = target->symbol_prefix;

  /* Function header. */
  if (strcmp(target->object_format, "macho") == 0) {
    if (!ny_native_put(w, "\t.p2align 4, 0x90\n"))
      return false;
  } else if (!ny_native_printf(w, "\t.type\t%s%s,@function\n", sym, name)) {
    return false;
  }
  if (!ny_native_printf(w, "\t.globl\t%s%s\n%s%s:\n", sym, name, sym, name))
    return false;

  /* Prologue: save rbp, rbx, allocate frame. */
  if (!ny_native_put(w, "\tpushq\t%rbp\n\tmovq\t%rsp, %rbp\n\tpushq\t%rbx\n"))
    return false;
  if (ctx.frame_bytes > 0 &&
      !ny_native_printf(w, "\tsubq\t$%d, %%rsp\n", ctx.frame_bytes))
    return false;

  /* Detect parameters: locals loaded before any store to that slot. */
  bool *param_init = NULL;
  bool *stored = NULL;
  int max_local = ctx.max_local_slot;
  if (strcmp(name, "rt_main") != 0 && max_local > 0) {
    param_init = (bool *)calloc((size_t)max_local, sizeof(bool));
    stored = (bool *)calloc((size_t)max_local, sizeof(bool));
    for (size_t i = 0; nir && i < nir->len && param_init && stored; ++i) {
      const ny_nir_inst_t *in = &nir->data[i];
      int lid = (int)in->imm;
      if (in->op == NY_NIR_STORE_LOCAL && lid >= 0 && lid < max_local)
        stored[lid] = true;
      else if (in->op == NY_NIR_LOAD_LOCAL && lid >= 0 && lid < max_local &&
               !stored[lid])
        param_init[lid] = true;
    }
    int gp = 0;
    int sse = 0;
    int stack = 0;
    for (int i = 0; i < max_local; ++i) {
      if (!param_init || !param_init[i])
        continue;
      bool is_f64 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f64[i];
      bool is_f32 = i < NY_X64_NIR_MAX_SLOTS && ctx.local_f32[i];
      if ((is_f64 || is_f32) && sse < 8) {
        int off = (i + 1) * 8;
        if (!ny_native_printf(w, "\t%s\t%%xmm%d, -%d(%%rbp)\n",
                              is_f32 ? "movss" : "movsd", sse, off)) {
          free(param_init);
          free(stored);
          return false;
        }
        sse++;
      } else if (!is_f64 && !is_f32 && gp < 6) {
        int off = (i + 1) * 8;
        if (!ny_native_printf(w, "\tmovq\t%s, -%d(%%rbp)\n",
                              target->gp_arg_regs[gp], off)) {
          free(param_init);
          free(stored);
          return false;
        }
        gp++;
      } else {
        /* Stack-passed parameter (index >= gp_arg_reg_count): the caller
         * left it above the return address (and any shadow space) at
         * function entry, so load it via %rbp and re-home it into the
         * callee's own local slot like a register parameter. */
        int src_off = 16 + (int)target->shadow_space_bytes +
                      stack * 8;
        int dst_off = (i + 1) * 8;
        if (!ny_native_printf(w, "\tmovq\t%d(%%rbp), %%rax\n", src_off) ||
            !ny_native_printf(w, "\tmovq\t%%rax, -%d(%%rbp)\n", dst_off)) {
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

  /* Emit each NYIR instruction. */
  for (size_t i = 0; i < nir->len; ++i) {
    if (!ny_x64_nir_emit_inst(&ctx, &nir->data[i])) {
      fprintf(stderr, "native NYIR repro (x86-64 emit failed):\n");
      ny_nir_dump(stderr, nir, name);
      return false;
    }
  }

  /* Epilogue. */
  if (!ny_native_printf(w, "%s:\n", ctx.epilogue_label))
    return false;
  if (tag_return && !ny_native_put(w, "\tleaq\t1(,%rax,2), %rax\n"))
    return false;
  if (!ny_native_put(w, "\tmovq\t-8(%rbp), %rbx\n\tleave\n\tret\n"))
    return false;

  if (strcmp(target->object_format, "macho") != 0) {
    if (!ny_native_printf(w, "\t.size\t%s%s, .-%s%s\n", sym, name, sym, name))
      return false;
  }
  return true;
}
