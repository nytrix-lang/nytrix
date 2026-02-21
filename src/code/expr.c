#include "base/common.h"
#include "base/util.h"
#include "braun.h"
#include "llvm.h"
#include "nullnarrow.h"
#include "priv.h"
#include "rt/shared.h"
#ifndef _WIN32
#include <alloca.h>
#else
#include <malloc.h>
#endif
#include "jit.h"
#include <inttypes.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Support.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

LLVMValueRef expr_fail(codegen_t *cg, token_t tok, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  char msg[512];
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  ny_diag_error(tok, "%s", msg);
  cg->had_error = 1;
  return LLVMConstInt(cg->type_i64, 0, false);
}

static inline uint64_t ny_const_str_hash(const char *s, size_t len) {
  return ny_hash64(s, len);
}

typedef enum ny_ct_fast_kind_t {
  NY_CT_FAST_NONE,
  NY_CT_FAST_INT,
  NY_CT_FAST_BOOL,
  NY_CT_FAST_STR,
} ny_ct_fast_kind_t;

typedef struct ny_ct_fast_val_t {
  ny_ct_fast_kind_t kind;
  int64_t i;
  bool b;
  const char *s;
} ny_ct_fast_val_t;

static const char *ny_comptime_host_os_name(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#elif defined(__FreeBSD__)
  return "freebsd";
#elif defined(__NetBSD__)
  return "netbsd";
#elif defined(__OpenBSD__)
  return "openbsd";
#else
  return "unknown";
#endif
}

static const char *ny_comptime_host_arch_name(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

static bool ny_comptime_main_enabled(void) {
  return ny_env_is_truthy(getenv("NYTRIX_TEST_MODE"));
}

static bool ny_ct_fast_truthy(const ny_ct_fast_val_t *v, bool *out) {
  if (!v || !out)
    return false;
  if (v->kind == NY_CT_FAST_NONE) {
    *out = false;
    return true;
  }
  if (v->kind == NY_CT_FAST_BOOL) {
    *out = v->b;
    return true;
  }
  if (v->kind == NY_CT_FAST_INT) {
    *out = (v->i != 0);
    return true;
  }
  return false;
}

static const char *ny_ct_fast_callee_leaf_name(expr_t *callee) {
  expr_t *cur = callee;
  int guard = 0;
  while (cur && guard++ < 16) {
    if (cur->kind == NY_E_IDENT)
      return cur->as.ident.name;
    if (cur->kind == NY_E_MEMBER) {
      if (cur->as.member.name && *cur->as.member.name)
        return cur->as.member.name;
      cur = cur->as.member.target;
      continue;
    }
    break;
  }
  return NULL;
}

static bool ny_try_eval_comptime_expr_fast(expr_t *e, ny_ct_fast_val_t *out,
                                           int depth) {
  if (!e || !out || depth > 64)
    return false;

  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data ? e->as.literal.as.s.data : "";
      return true;
    }
    return false;

  case NY_E_UNARY: {
    ny_ct_fast_val_t r = {0};
    if (!ny_try_eval_comptime_expr_fast(e->as.unary.right, &r, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "!") == 0) {
      bool t = false;
      if (!ny_ct_fast_truthy(&r, &t))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = !t;
      return true;
    }
    if (strcmp(e->as.unary.op, "-") == 0 && r.kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = -r.i;
      return true;
    }
    return false;
  }

  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = {0};
    if (!ny_try_eval_comptime_expr_fast(e->as.logical.left, &l, depth + 1))
      return false;
    bool lt = false;
    if (!ny_ct_fast_truthy(&l, &lt))
      return false;
    if (strcmp(e->as.logical.op, "&&") == 0) {
      if (!lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = false;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(e->as.logical.right, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      if (lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = true;
        return true;
      }
      ny_ct_fast_val_t r = {0};
      bool rt = false;
      if (!ny_try_eval_comptime_expr_fast(e->as.logical.right, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      return true;
    }
    return false;
  }

  case NY_E_BINARY: {
    ny_ct_fast_val_t l = {0}, r = {0};
    if (!ny_try_eval_comptime_expr_fast(e->as.binary.left, &l, depth + 1) ||
        !ny_try_eval_comptime_expr_fast(e->as.binary.right, &r, depth + 1))
      return false;
    if (strcmp(e->as.binary.op, "==") == 0 ||
        strcmp(e->as.binary.op, "!=") == 0) {
      bool eq = false;
      if (l.kind == NY_CT_FAST_INT && r.kind == NY_CT_FAST_INT) {
        eq = (l.i == r.i);
      } else if (l.kind == NY_CT_FAST_BOOL && r.kind == NY_CT_FAST_BOOL) {
        eq = (l.b == r.b);
      } else if (l.kind == NY_CT_FAST_STR && r.kind == NY_CT_FAST_STR) {
        const char *ls = l.s ? l.s : "";
        const char *rs = r.s ? r.s : "";
        eq = strcmp(ls, rs) == 0;
      } else {
        eq = false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = (strcmp(e->as.binary.op, "==") == 0) ? eq : !eq;
      return true;
    }
    if (strcmp(e->as.binary.op, "+") == 0 && l.kind == NY_CT_FAST_INT &&
        r.kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = l.i + r.i;
      return true;
    }
    if (strcmp(e->as.binary.op, "-") == 0 && l.kind == NY_CT_FAST_INT &&
        r.kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = l.i - r.i;
      return true;
    }
    return false;
  }

  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = {0};
    bool ct = false;
    if (!ny_try_eval_comptime_expr_fast(e->as.ternary.cond, &c, depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct))
      return false;
    return ny_try_eval_comptime_expr_fast(ct ? e->as.ternary.true_expr
                                             : e->as.ternary.false_expr,
                                          out, depth + 1);
  }

  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    bool zero_arg = (e->as.call.args.len == 0);
    bool one_member_arg =
        (e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER);
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled();
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_comptime_host_os_name();
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_comptime_host_arch_name();
      return true;
    }
    return false;
  }

  default:
    return false;
  }
}

static bool ny_try_eval_comptime_fast(stmt_t *body, int64_t *out_tagged) {
  if (!body || !out_tagged)
    return false;

  expr_t *e = NULL;
  if (body->kind == NY_S_BLOCK) {
    if (body->as.block.body.len != 1)
      return false;
    stmt_t *s = body->as.block.body.data[0];
    if (!s)
      return false;
    if (s->kind == NY_S_RETURN) {
      e = s->as.ret.value;
    } else if (s->kind == NY_S_EXPR) {
      e = s->as.expr.expr;
    } else {
      return false;
    }
  } else if (body->kind == NY_S_RETURN) {
    e = body->as.ret.value;
  } else if (body->kind == NY_S_EXPR) {
    e = body->as.expr.expr;
  } else {
    return false;
  }
  if (!e)
    return false;

  ny_ct_fast_val_t v = {0};
  if (!ny_try_eval_comptime_expr_fast(e, &v, 0))
    return false;

  if (v.kind == NY_CT_FAST_BOOL) {
    *out_tagged = v.b ? 2 : 4;
    return true;
  }
  if (v.kind == NY_CT_FAST_INT) {
    *out_tagged = (int64_t)((((uint64_t)v.i) << 1) | 1u);
    return true;
  }
  return false;
}

typedef struct ny_ct_interp_var_t {
  const char *name;
  ny_ct_fast_val_t value;
} ny_ct_interp_var_t;

typedef struct ny_ct_interp_ctx_t {
  ny_ct_interp_var_t *vars;
  size_t len;
  size_t cap;
  size_t steps;
  size_t max_steps;
} ny_ct_interp_ctx_t;

static inline ny_ct_fast_val_t ny_ct_fast_none(void) {
  ny_ct_fast_val_t v = {.kind = NY_CT_FAST_NONE, .i = 0, .b = false, .s = NULL};
  return v;
}

static bool ny_ct_interp_step(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return false;
  if (++ctx->steps > ctx->max_steps)
    return false;
  return true;
}

static void ny_ct_interp_ctx_free(ny_ct_interp_ctx_t *ctx) {
  if (!ctx)
    return;
  free(ctx->vars);
  ctx->vars = NULL;
  ctx->len = 0;
  ctx->cap = 0;
}

static bool ny_ct_interp_ctx_clone(const ny_ct_interp_ctx_t *src,
                                   ny_ct_interp_ctx_t *dst) {
  if (!src || !dst)
    return false;
  memset(dst, 0, sizeof(*dst));
  dst->max_steps = src->max_steps;
  dst->steps = src->steps;
  if (src->len == 0)
    return true;
  dst->vars = malloc(sizeof(*dst->vars) * src->len);
  if (!dst->vars)
    return false;
  memcpy(dst->vars, src->vars, sizeof(*dst->vars) * src->len);
  dst->len = src->len;
  dst->cap = src->len;
  return true;
}

static bool ny_ct_interp_get(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t *out) {
  if (!ctx || !name || !*name || !out)
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0) {
      *out = v->value;
      return true;
    }
  }
  return false;
}

static bool ny_ct_interp_set(ny_ct_interp_ctx_t *ctx, const char *name,
                             ny_ct_fast_val_t value) {
  if (!ctx || !name || !*name)
    return false;
  for (size_t i = ctx->len; i > 0; --i) {
    ny_ct_interp_var_t *v = &ctx->vars[i - 1];
    if (v->name && strcmp(v->name, name) == 0) {
      v->value = value;
      return true;
    }
  }
  if (ctx->len == ctx->cap) {
    size_t next_cap = ctx->cap ? (ctx->cap * 2) : 16;
    ny_ct_interp_var_t *grown =
        realloc(ctx->vars, sizeof(*ctx->vars) * next_cap);
    if (!grown)
      return false;
    ctx->vars = grown;
    ctx->cap = next_cap;
  }
  ctx->vars[ctx->len++] = (ny_ct_interp_var_t){.name = name, .value = value};
  return true;
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth);

static bool ny_ct_interp_eval_stmt(stmt_t *s, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *ret, bool *did_return,
                                   int depth) {
  if (!s || !ctx || !ret || !did_return)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (s->kind) {
  case NY_S_BLOCK: {
    for (size_t i = 0; i < s->as.block.body.len; i++) {
      if (!ny_ct_interp_eval_stmt(s->as.block.body.data[i], ctx, ret,
                                  did_return, depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    return true;
  }
  case NY_S_RETURN: {
    if (!s->as.ret.value) {
      *ret = ny_ct_fast_none();
    } else if (!ny_ct_interp_eval_expr(s->as.ret.value, ctx, ret, depth + 1)) {
      return false;
    }
    *did_return = true;
    return true;
  }
  case NY_S_EXPR: {
    ny_ct_fast_val_t tmp = ny_ct_fast_none();
    if (!s->as.expr.expr)
      return false;
    return ny_ct_interp_eval_expr(s->as.expr.expr, ctx, &tmp, depth + 1);
  }
  case NY_S_VAR: {
    if (s->as.var.is_destructure)
      return false;
    for (size_t i = 0; i < s->as.var.names.len; i++) {
      const char *name = s->as.var.names.data[i];
      if (!name || !*name)
        return false;
      ny_ct_fast_val_t v = ny_ct_fast_none();
      if (!s->as.var.is_undef) {
        expr_t *rhs = NULL;
        if (s->as.var.exprs.len == s->as.var.names.len &&
            i < s->as.var.exprs.len)
          rhs = s->as.var.exprs.data[i];
        else if (s->as.var.exprs.len > 0)
          rhs = s->as.var.exprs.data[0];
        if (rhs && !ny_ct_interp_eval_expr(rhs, ctx, &v, depth + 1))
          return false;
      }
      if (!ny_ct_interp_set(ctx, name, v))
        return false;
    }
    return true;
  }
  case NY_S_IF: {
    if (!s->as.iff.test)
      return false;
    ny_ct_fast_val_t cond = ny_ct_fast_none();
    bool truthy = false;
    if (!ny_ct_interp_eval_expr(s->as.iff.test, ctx, &cond, depth + 1) ||
        !ny_ct_fast_truthy(&cond, &truthy))
      return false;
    if (truthy) {
      if (s->as.iff.conseq)
        return ny_ct_interp_eval_stmt(s->as.iff.conseq, ctx, ret, did_return,
                                      depth + 1);
      return true;
    }
    if (s->as.iff.alt)
      return ny_ct_interp_eval_stmt(s->as.iff.alt, ctx, ret, did_return,
                                    depth + 1);
    return true;
  }
  case NY_S_WHILE: {
    if (!s->as.whl.test || !s->as.whl.body)
      return false;
    size_t guard = 0;
    while (1) {
      ny_ct_fast_val_t cond = ny_ct_fast_none();
      bool truthy = false;
      if (!ny_ct_interp_eval_expr(s->as.whl.test, ctx, &cond, depth + 1) ||
          !ny_ct_fast_truthy(&cond, &truthy))
        return false;
      if (!truthy)
        break;
      if (++guard > 100000)
        return false;
      if (!ny_ct_interp_eval_stmt(s->as.whl.body, ctx, ret, did_return,
                                  depth + 1))
        return false;
      if (*did_return)
        return true;
    }
    return true;
  }
  case NY_S_MACRO: {
    for (size_t i = 0; i < s->as.macro.args.len; i++) {
      ny_ct_fast_val_t arg = ny_ct_fast_none();
      if (!ny_ct_interp_eval_expr(s->as.macro.args.data[i], ctx, &arg,
                                  depth + 1)) {
        return false;
      }
    }
    if (!s->as.macro.body)
      return true;
    return ny_ct_interp_eval_stmt(s->as.macro.body, ctx, ret, did_return,
                                  depth + 1);
  }
  default:
    return false;
  }
}

static bool ny_ct_interp_eval_expr(expr_t *e, ny_ct_interp_ctx_t *ctx,
                                   ny_ct_fast_val_t *out, int depth) {
  if (!e || !ctx || !out)
    return false;
  if (depth > 256 || !ny_ct_interp_step(ctx))
    return false;
  switch (e->kind) {
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = e->as.literal.as.i;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_BOOL) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = e->as.literal.as.b;
      return true;
    }
    if (e->as.literal.kind == NY_LIT_STR) {
      out->kind = NY_CT_FAST_STR;
      out->s = e->as.literal.as.s.data;
      return true;
    }
    return false;
  case NY_E_IDENT: {
    const char *name = e->as.ident.name;
    if (!name || !*name)
      return false;
    if (strcmp(name, "none") == 0) {
      *out = ny_ct_fast_none();
      return true;
    }
    return ny_ct_interp_get(ctx, name, out);
  }
  case NY_E_UNARY: {
    ny_ct_fast_val_t r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.unary.right, ctx, &r, depth + 1))
      return false;
    if (strcmp(e->as.unary.op, "-") == 0 && r.kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = -r.i;
      return true;
    }
    if (strcmp(e->as.unary.op, "!") == 0) {
      bool t = false;
      if (!ny_ct_fast_truthy(&r, &t))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = !t;
      return true;
    }
    if (strcmp(e->as.unary.op, "~") == 0 && r.kind == NY_CT_FAST_INT) {
      out->kind = NY_CT_FAST_INT;
      out->i = ~r.i;
      return true;
    }
    return false;
  }
  case NY_E_LOGICAL: {
    ny_ct_fast_val_t l = ny_ct_fast_none();
    bool lt = false;
    if (!ny_ct_interp_eval_expr(e->as.logical.left, ctx, &l, depth + 1) ||
        !ny_ct_fast_truthy(&l, &lt))
      return false;
    if (strcmp(e->as.logical.op, "&&") == 0) {
      if (!lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = false;
        return true;
      }
      ny_ct_fast_val_t r = ny_ct_fast_none();
      bool rt = false;
      if (!ny_ct_interp_eval_expr(e->as.logical.right, ctx, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      return true;
    }
    if (strcmp(e->as.logical.op, "||") == 0) {
      if (lt) {
        out->kind = NY_CT_FAST_BOOL;
        out->b = true;
        return true;
      }
      ny_ct_fast_val_t r = ny_ct_fast_none();
      bool rt = false;
      if (!ny_ct_interp_eval_expr(e->as.logical.right, ctx, &r, depth + 1) ||
          !ny_ct_fast_truthy(&r, &rt))
        return false;
      out->kind = NY_CT_FAST_BOOL;
      out->b = rt;
      return true;
    }
    return false;
  }
  case NY_E_BINARY: {
    ny_ct_fast_val_t l = ny_ct_fast_none(), r = ny_ct_fast_none();
    if (!ny_ct_interp_eval_expr(e->as.binary.left, ctx, &l, depth + 1) ||
        !ny_ct_interp_eval_expr(e->as.binary.right, ctx, &r, depth + 1))
      return false;
    const char *op = e->as.binary.op;
    if (!op)
      return false;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) {
      bool eq = false;
      if (l.kind == NY_CT_FAST_INT && r.kind == NY_CT_FAST_INT) {
        eq = (l.i == r.i);
      } else if (l.kind == NY_CT_FAST_BOOL && r.kind == NY_CT_FAST_BOOL) {
        eq = (l.b == r.b);
      } else if (l.kind == NY_CT_FAST_NONE && r.kind == NY_CT_FAST_NONE) {
        eq = true;
      } else if (l.kind == NY_CT_FAST_STR && r.kind == NY_CT_FAST_STR) {
        eq = strcmp(l.s ? l.s : "", r.s ? r.s : "") == 0;
      } else {
        eq = false;
      }
      out->kind = NY_CT_FAST_BOOL;
      out->b = (strcmp(op, "==") == 0) ? eq : !eq;
      return true;
    }
    if (l.kind == NY_CT_FAST_INT && r.kind == NY_CT_FAST_INT) {
      if (strcmp(op, "+") == 0) {
        out->kind = NY_CT_FAST_INT;
        out->i = l.i + r.i;
        return true;
      }
      if (strcmp(op, "-") == 0) {
        out->kind = NY_CT_FAST_INT;
        out->i = l.i - r.i;
        return true;
      }
      if (strcmp(op, "*") == 0) {
        out->kind = NY_CT_FAST_INT;
        out->i = l.i * r.i;
        return true;
      }
      if (strcmp(op, "/") == 0) {
        if (r.i == 0)
          return false;
        out->kind = NY_CT_FAST_INT;
        out->i = l.i / r.i;
        return true;
      }
      if (strcmp(op, "%") == 0) {
        if (r.i == 0)
          return false;
        out->kind = NY_CT_FAST_INT;
        out->i = l.i % r.i;
        return true;
      }
      if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
          strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        bool b = false;
        if (strcmp(op, "<") == 0)
          b = l.i < r.i;
        else if (strcmp(op, "<=") == 0)
          b = l.i <= r.i;
        else if (strcmp(op, ">") == 0)
          b = l.i > r.i;
        else
          b = l.i >= r.i;
        out->kind = NY_CT_FAST_BOOL;
        out->b = b;
        return true;
      }
    }
    return false;
  }
  case NY_E_TERNARY: {
    ny_ct_fast_val_t c = ny_ct_fast_none();
    bool ct = false;
    if (!ny_ct_interp_eval_expr(e->as.ternary.cond, ctx, &c, depth + 1) ||
        !ny_ct_fast_truthy(&c, &ct))
      return false;
    return ny_ct_interp_eval_expr(ct ? e->as.ternary.true_expr
                                     : e->as.ternary.false_expr,
                                  ctx, out, depth + 1);
  }
  case NY_E_COMPTIME: {
    ny_ct_interp_ctx_t nested = {0};
    ny_ct_fast_val_t nested_ret = ny_ct_fast_none();
    bool did_return = false;
    if (!ny_ct_interp_ctx_clone(ctx, &nested))
      return false;
    bool ok = ny_ct_interp_eval_stmt(e->as.comptime_expr.body, &nested,
                                     &nested_ret, &did_return, depth + 1);
    ctx->steps = nested.steps;
    ny_ct_interp_ctx_free(&nested);
    if (!ok)
      return false;
    if (!did_return)
      nested_ret = ny_ct_fast_none();
    *out = nested_ret;
    return true;
  }
  case NY_E_CALL: {
    if (!e->as.call.callee)
      return false;
    const char *name = ny_ct_fast_callee_leaf_name(e->as.call.callee);
    if (!name)
      return false;
    bool zero_arg = (e->as.call.args.len == 0);
    bool one_member_arg =
        (e->as.call.args.len == 1 && e->as.call.callee->kind == NY_E_MEMBER);
    if (strcmp(name, "__main") == 0 && (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_BOOL;
      out->b = ny_comptime_main_enabled();
      return true;
    }
    if ((strcmp(name, "__os_name") == 0 || strcmp(name, "os") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_comptime_host_os_name();
      return true;
    }
    if ((strcmp(name, "__arch_name") == 0 || strcmp(name, "arch") == 0) &&
        (zero_arg || one_member_arg)) {
      out->kind = NY_CT_FAST_STR;
      out->s = ny_comptime_host_arch_name();
      return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool ny_ct_interp_to_tagged(const ny_ct_fast_val_t *v,
                                   int64_t *out_tagged) {
  if (!v || !out_tagged)
    return false;
  if (v->kind == NY_CT_FAST_NONE) {
    *out_tagged = 0;
    return true;
  }
  if (v->kind == NY_CT_FAST_BOOL) {
    *out_tagged = v->b ? 2 : 4;
    return true;
  }
  if (v->kind == NY_CT_FAST_INT) {
    *out_tagged = (int64_t)((((uint64_t)v->i) << 1) | 1u);
    return true;
  }
  return false;
}

static bool ny_try_eval_comptime_interp(stmt_t *body, int64_t *out_tagged) {
  if (!body || !out_tagged)
    return false;
  ny_ct_interp_ctx_t ctx = {0};
  ctx.max_steps = 500000;
  ny_ct_fast_val_t ret = ny_ct_fast_none();
  bool did_return = false;
  bool ok = ny_ct_interp_eval_stmt(body, &ctx, &ret, &did_return, 0);
  ny_ct_interp_ctx_free(&ctx);
  if (!ok)
    return false;
  if (!did_return)
    ret = ny_ct_fast_none();
  return ny_ct_interp_to_tagged(&ret, out_tagged);
}

static LLVMValueRef ny_try_host_platform_ident(codegen_t *cg,
                                               const char *name) {
  if (!cg || !name)
    return NULL;

  const char *os = ny_comptime_host_os_name();
  const char *arch = ny_comptime_host_arch_name();
  LLVMValueRef tag_true = LLVMConstInt(cg->type_i64, 2u, false);
  LLVMValueRef tag_false = LLVMConstInt(cg->type_i64, 4u, false);

  if (strcmp(name, "OS") == 0) {
    LLVMValueRef g = const_string_ptr(cg, os, strlen(os));
    return LLVMBuildLoad2(cg->builder, cg->type_i64, g, "host_os");
  }
  if (strcmp(name, "ARCH") == 0) {
    LLVMValueRef g = const_string_ptr(cg, arch, strlen(arch));
    return LLVMBuildLoad2(cg->builder, cg->type_i64, g, "host_arch");
  }
  if (strcmp(name, "IS_LINUX") == 0)
    return strcmp(os, "linux") == 0 ? tag_true : tag_false;
  if (strcmp(name, "IS_MACOS") == 0)
    return strcmp(os, "macos") == 0 ? tag_true : tag_false;
  if (strcmp(name, "IS_WINDOWS") == 0)
    return strcmp(os, "windows") == 0 ? tag_true : tag_false;
  if (strcmp(name, "IS_X86_64") == 0)
    return strcmp(arch, "x86_64") == 0 ? tag_true : tag_false;
  if (strcmp(name, "IS_AARCH64") == 0) {
    bool is_a64 = (strcmp(arch, "aarch64") == 0 || strcmp(arch, "arm64") == 0);
    return is_a64 ? tag_true : tag_false;
  }
  if (strcmp(name, "IS_ARM") == 0)
    return strcmp(arch, "arm") == 0 ? tag_true : tag_false;
  return NULL;
}

LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth,
                         ny_param_list params, stmt_t *body, bool is_variadic,
                         const char *return_type, const char *name_hint) {
  /* Capture All Visible Variables (scopes[1..depth]) */
  binding_list captures;
  vec_init(&captures);
  for (ssize_t i = 1; i <= (ssize_t)depth; i++) {
    for (size_t j = 0; j < scopes[i].vars.len; j++) {
      vec_push(&captures, scopes[i].vars.data[j]);
      // Mark the original variable as used since it's being captured
      scopes[i].vars.data[j].is_used = true;
    }
  }
  token_t closure_tok = body ? body->tok : (token_t){0};
  char name[64];
  if (name_hint && strncmp(name_hint, "__lambda", 8) == 0) {
    if (closure_tok.line > 0) {
      snprintf(name, sizeof(name), "%s_L%d_C%d_%d", name_hint, closure_tok.line,
               closure_tok.col, cg->lambda_count++);
    } else {
      snprintf(name, sizeof(name), "%s_%d", name_hint, cg->lambda_count++);
    }
  } else {
    snprintf(name, sizeof(name), "%s_%d", name_hint ? name_hint : "__lambda",
             cg->lambda_count++);
  }
  stmt_t sfn;
  memset(&sfn, 0, sizeof(sfn));
  sfn.kind = NY_S_FUNC;
  sfn.as.fn.name = strdup(name);
  sfn.as.fn.params = params;
  sfn.as.fn.body = body;
  sfn.as.fn.is_variadic = is_variadic;
  sfn.as.fn.return_type = return_type;
  // Copy location from body if possible
  if (body)
    sfn.tok = body->tok;
  scope sc[64] = {0};
  // heuristic.
  bool uses_env = captures.len > 0;
  if (name_hint && strcmp(name_hint, "__defer") == 0)
    uses_env = true;

  gen_func(cg, &sfn, name, sc, 0, uses_env ? &captures : NULL);
  free((void *)sfn.as.fn.name);
  LLVMValueRef lf = LLVMGetNamedFunction(cg->module, name);
  // Keep callable pointers raw. Ad-hoc low-bit tagging collides with
  // 32-bit runtime native tags (and ARM Thumb pointer semantics).
  LLVMValueRef fn_ptr_raw =
      LLVMBuildPtrToInt(cg->builder, lf, cg->type_i64, "");

  if (!uses_env) {
    /* No captures: return plain callable pointer */
    vec_free(&captures);
    return fn_ptr_raw;
  }
  /* Create Env */
  fun_sig *malloc_sig = lookup_fun(cg, "__malloc");
  if (!malloc_sig) {
    token_t tok = body ? body->tok : (token_t){0};
    return expr_fail(cg, tok, "__malloc required for closures");
  }
  LLVMValueRef env_alloc_size = LLVMConstInt(
      cg->type_i64, (uint64_t)(((uint64_t)captures.len * 8) << 1) | 1, false);
  LLVMValueRef env_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){env_alloc_size}, 1, "env");
  LLVMValueRef env_raw = LLVMBuildIntToPtr(
      cg->builder, env_ptr, LLVMPointerType(cg->type_i64, 0), "env_raw");
  for (size_t i = 0; i < captures.len; i++) {
    LLVMValueRef slot_val = captures.data[i].is_slot
                                ? LLVMBuildLoad2(cg->builder, cg->type_i64,
                                                 captures.data[i].value, "")
                                : captures.data[i].value;
    LLVMValueRef dst = LLVMBuildGEP2(
        cg->builder, cg->type_i64, env_raw,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, (uint64_t)i, false)}, 1,
        "");
    LLVMBuildStore(cg->builder, slot_val, dst);
  }
  /* Create Closure Object [Tag=105 | Code | Env] */
  LLVMValueRef cls_size =
      LLVMConstInt(cg->type_i64, ((uint64_t)16 << 1) | 1, false);
  LLVMValueRef cls_ptr =
      LLVMBuildCall2(cg->builder, malloc_sig->type, malloc_sig->value,
                     (LLVMValueRef[]){cls_size}, 1, "closure");
  LLVMValueRef cls_raw = LLVMBuildIntToPtr(
      cg->builder, cls_ptr, LLVMPointerType(cg->type_i64, 0), "");
  /* Set Tag -8 */
  LLVMValueRef tag_addr = LLVMBuildGEP2(
      cg->builder, LLVMInt8TypeInContext(cg->ctx),
      LLVMBuildBitCast(cg->builder, cls_raw,
                       LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0), ""),
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, -8, true)}, 1, "");
  LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 105, false),
                 LLVMBuildBitCast(cg->builder, tag_addr,
                                  LLVMPointerType(cg->type_i64, 0), ""));
  /* Store Code at 0 */
  LLVMBuildStore(cg->builder, fn_ptr_raw, cls_raw);
  /* Store Env at 8 */
  LLVMValueRef env_store_addr = LLVMBuildGEP2(
      cg->builder, cg->type_i64, cls_raw,
      (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false)}, 1, "");
  LLVMBuildStore(cg->builder, env_ptr, env_store_addr);
  vec_free(&captures);
  return cls_ptr;
}

LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v) {
  LLVMValueRef is_none =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 0, false), "is_none");
  LLVMValueRef is_false =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 4, false), "is_false");
  LLVMValueRef is_zero =
      LLVMBuildICmp(cg->builder, LLVMIntEQ, v,
                    LLVMConstInt(cg->type_i64, 1, false), "is_zero");
  return LLVMBuildNot(
      cg->builder,
      LLVMBuildOr(cg->builder, LLVMBuildOr(cg->builder, is_none, is_false, ""),
                  is_zero, ""),
      "to_bool");
}

LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len) {
  uint64_t key_hash = ny_const_str_hash(s, len);
  for (size_t i = cg->interns.len; i > 0; --i) {
    string_intern *in = &cg->interns.data[i - 1];
    if (in->module != cg->module)
      continue;
    if (in->len == len && in->hash == key_hash && memcmp(in->data, s, len) == 0)
      return in->val;
  }
  const char *final_s = s;
  size_t final_len = len;
  size_t header_size = 64;
  size_t tail_size = 16;
  size_t total_len = header_size + final_len + 1 + tail_size;
  char *obj_data = calloc(1, total_len);
  // Write Header
  // We do NOT write heap magic numbers (NY_MAGIC1/2) here.
  // If we did, the runtime would treat this as a heap pointer and strict bounds
  // checking (__check_oob) would forbid accessing header fields (like length
  // at -16). By leaving magics as 0, is_heap_ptr returns false, allowing
  // access.
  *(uint64_t *)(obj_data) = 0;                       // NY_MAGIC1;
  *(uint64_t *)(obj_data + 8) = (uint64_t)final_len; // Capacity
  *(uint64_t *)(obj_data + 16) = 0;                  // NY_MAGIC2;
  *(uint64_t *)(obj_data + 48) =
      ((uint64_t)final_len << 1) | 1; // Length at p-16 (tagged)
  *(uint64_t *)(obj_data + 56) = 241; // Tag at p-8 (TAG_STR)
  // Write Data
  memcpy(obj_data + header_size, final_s, final_len);
  obj_data[header_size + final_len] = '\0';
  // Write Tail
  uint64_t magic3 = NY_MAGIC3;
  memcpy(obj_data + header_size + final_len + 1, &magic3, sizeof(magic3));
  LLVMTypeRef arr_ty =
      LLVMArrayType(LLVMInt8TypeInContext(cg->ctx), (unsigned)total_len);
  LLVMValueRef g = LLVMAddGlobal(cg->module, arr_ty, ".str");
  LLVMSetInitializer(g, LLVMConstStringInContext(cg->ctx, obj_data,
                                                 (unsigned)total_len, true));
  LLVMSetGlobalConstant(g, true);
  LLVMSetLinkage(g, LLVMInternalLinkage);
  LLVMSetUnnamedAddr(g, true);
  LLVMSetAlignment(g, 64);
  // Store the global and metadata
  string_intern in = {.data = obj_data + header_size,
                      .len = final_len,
                      .hash = key_hash,
                      .val = g,
                      .gv = g,
                      .module = cg->module,
                      .alloc = obj_data};
  vec_push(&cg->interns, in);
  // Create a global i64 variable to hold the runtime pointer address
  // This is initialized to 0 but will be set in a runtime init function
  char ptr_name[128];
  snprintf(ptr_name, sizeof(ptr_name), ".str.runtime.%zu", cg->interns.len - 1);
  LLVMValueRef runtime_ptr_global =
      LLVMAddGlobal(cg->module, cg->type_i64, ptr_name);
  LLVMSetInitializer(runtime_ptr_global, LLVMConstInt(cg->type_i64, 0, false));
  LLVMSetLinkage(runtime_ptr_global, LLVMInternalLinkage);
  // Store this runtime pointer global in the intern struct
  cg->interns.data[cg->interns.len - 1].val = runtime_ptr_global;
  // Return the runtime pointer global (callers will load from it)
  return runtime_ptr_global;
}

LLVMValueRef ny_is_tagged_int(codegen_t *cg, LLVMValueRef v) {
  if (!v)
    return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, false);
  LLVMTypeRef ty = LLVMTypeOf(v);
  if (!ty || LLVMGetTypeKind(ty) != LLVMIntegerTypeKind ||
      LLVMGetIntTypeWidth(ty) != 64) {
    return LLVMConstInt(LLVMInt1TypeInContext(cg->ctx), 0, false);
  }
  LLVMValueRef one = LLVMConstInt(cg->type_i64, 1, false);
  LLVMValueRef lsb = LLVMBuildAnd(cg->builder, v, one, "int_lsb");
  return LLVMBuildICmp(cg->builder, LLVMIntEQ, lsb, one, "is_tagged_int");
}

LLVMValueRef ny_untag_int(codegen_t *cg, LLVMValueRef v) {
  return LLVMBuildAShr(cg->builder, v, LLVMConstInt(cg->type_i64, 1, false),
                       "untag_int");
}

LLVMValueRef ny_tag_int(codegen_t *cg, LLVMValueRef v) {
  LLVMValueRef sh =
      LLVMBuildShl(cg->builder, v, LLVMConstInt(cg->type_i64, 1, false), "");
  return LLVMBuildOr(cg->builder, sh, LLVMConstInt(cg->type_i64, 1, false),
                     "tag_int");
}

static bool ny_helper_sig_in_current_sigs(const codegen_t *cg,
                                          const fun_sig *sig) {
  if (!cg || !sig || !cg->fun_sigs.data || cg->fun_sigs.len == 0)
    return false;
  const fun_sig *begin = cg->fun_sigs.data;
  const fun_sig *end = begin + cg->fun_sigs.len;
  return sig >= begin && sig < end;
}

static fun_sig *ny_helper_lookup(codegen_t *cg, fun_sig **cache_slot,
                                 const char *const *names, size_t names_len) {
  if (!cg || !names || names_len == 0)
    return NULL;
  if (cache_slot && *cache_slot &&
      ny_helper_sig_in_current_sigs(cg, *cache_slot))
    return *cache_slot;
  for (size_t i = 0; i < names_len; ++i) {
    const char *name = names[i];
    if (!name || !*name)
      continue;
    fun_sig *sig = lookup_fun(cg, name);
    if (sig) {
      if (cache_slot)
        *cache_slot = sig;
      return sig;
    }
  }
  return NULL;
}

#define NY_DEFINE_EXPR_HELPER(fn_name, cache_field, ...)                       \
  static fun_sig *fn_name(codegen_t *cg) {                                     \
    static const char *const k_names[] = {__VA_ARGS__};                        \
    return ny_helper_lookup(cg, &cg->cache_field, k_names,                     \
                            sizeof(k_names) / sizeof(k_names[0]));             \
  }

NY_DEFINE_EXPR_HELPER(ny_helper_sub, cached_fn_sub, "__sub")
NY_DEFINE_EXPR_HELPER(ny_helper_not, cached_fn_not, "__not")
NY_DEFINE_EXPR_HELPER(ny_helper_slice, cached_fn_slice, "slice")
NY_DEFINE_EXPR_HELPER(ny_helper_get, cached_fn_get, "get", "std.core.get",
                      "std.core.reflect.get", "dict_get")
NY_DEFINE_EXPR_HELPER(ny_helper_list, cached_fn_list, "list", "std.core.list")
NY_DEFINE_EXPR_HELPER(ny_helper_dict, cached_fn_dict, "dict", "std.core.dict")
NY_DEFINE_EXPR_HELPER(ny_helper_dict_set, cached_fn_dict_set, "dict_set",
                      "std.core.dict_set")
NY_DEFINE_EXPR_HELPER(ny_helper_set, cached_fn_set, "set", "std.core.set")
NY_DEFINE_EXPR_HELPER(ny_helper_set_add, cached_fn_set_add, "set_add",
                      "std.core.set_add")
NY_DEFINE_EXPR_HELPER(ny_helper_flt_box, cached_fn_flt_box, "__flt_box_val")
NY_DEFINE_EXPR_HELPER(ny_helper_str_concat, cached_fn_str_concat,
                      "__str_concat")
NY_DEFINE_EXPR_HELPER(ny_helper_to_str, cached_fn_to_str, "__to_str")

#undef NY_DEFINE_EXPR_HELPER

LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body) {
  int64_t fast_tagged = 0;
  if (ny_try_eval_comptime_fast(body, &fast_tagged)) {
    return LLVMConstInt(cg->type_i64, (uint64_t)fast_tagged, false);
  }

  char *err = NULL;
  LLVMBasicBlockRef prev_bb =
      cg->builder ? LLVMGetInsertBlock(cg->builder) : NULL;

  // 1. Snapshot current module state for isolated comptime evaluation.
  LLVMMemoryBufferRef bitcode = LLVMWriteBitcodeToMemoryBuffer(cg->module);

  // 2. Parse into an isolated context.
  bool ctm_ctx_owned = true;
  LLVMContextRef ctm_ctx = LLVMContextCreate();
  LLVMModuleRef mod = NULL;
  if (LLVMParseBitcodeInContext(ctm_ctx, bitcode, &mod, &err) != 0) {
    NY_LOG_WARN(
        "Comptime snapshot parse failed; trying AST interpreter fallback: %s\n",
        err ? err : "unknown error");
    if (err) {
      LLVMDisposeMessage(err);
      err = NULL;
    }
    int64_t interp_tagged = 0;
    if (ny_try_eval_comptime_interp(body, &interp_tagged)) {
      if (prev_bb)
        LLVMPositionBuilderAtEnd(cg->builder, prev_bb);
      LLVMDisposeMemoryBuffer(bitcode);
      LLVMContextDispose(ctm_ctx);
      return LLVMConstInt(cg->type_i64, (uint64_t)interp_tagged, true);
    }
    if (prev_bb)
      LLVMPositionBuilderAtEnd(cg->builder, prev_bb);
    LLVMDisposeMemoryBuffer(bitcode);
    LLVMContextDispose(ctm_ctx);
    return expr_fail(cg, body->tok, "failed to parse bitcode snapshot");
  }
  LLVMDisposeMemoryBuffer(bitcode);

  char entry_name[64];
  static int ctm_count = 0;
  sprintf(entry_name, "__ctm_entry_%d", ctm_count++);

  // Any function that is currently incomplete (not terminated) is likely the
  // one containing the comptime block or one of its parents in a recursive
  // build. We must convert these to declarations (remove bodies) to avoid
  // LLVM verification errors.
  for (LLVMValueRef f = LLVMGetFirstFunction(mod); f;
       f = LLVMGetNextFunction(f)) {
    if (strcmp(LLVMGetValueName(f), entry_name) == 0)
      continue;
    bool broken = false;
    for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb;
         bb = LLVMGetNextBasicBlock(bb)) {
      if (!LLVMGetBasicBlockTerminator(bb)) {
        broken = true;
        break;
      }
    }
    if (broken) {
      LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f);
      while (bb) {
        LLVMBasicBlockRef next = LLVMGetNextBasicBlock(bb);
        LLVMDeleteBasicBlock(bb);
        bb = next;
      }
    }
  }

  // 4. Normalize debug metadata in the snapshot before JIT.
#ifdef _WIN32
  /*
   * COFF + MCJIT can fail with IMAGE_REL_AMD64_ADDR32NB relocations when
   * transient comptime snapshot modules carry debug metadata.
   */
  LLVMStripModuleDebugInfo(mod);
#else
  LLVMDIBuilderRef snapshot_dib = LLVMCreateDIBuilder(mod);
  LLVMDIBuilderFinalize(snapshot_dib);
  LLVMDisposeDIBuilder(snapshot_dib);
#endif

  // 5. Verification Check (now should pass)
  char *verify_err = NULL;
  if (LLVMVerifyModule(mod, LLVMReturnStatusAction, &verify_err) != 0) {
    NY_LOG_WARN("Comptime snapshot module verification failed: %s\n",
                verify_err);
    LLVMDisposeMessage(verify_err);
  }

  // 6. Setup temporary codegen context.
  LLVMBuilderRef bld = LLVMCreateBuilderInContext(ctm_ctx);
  codegen_t tcg;
  codegen_init_with_context(&tcg, cg->prog, cg->arena, mod, ctm_ctx, bld);
  tcg.llvm_ctx_owned = false;
  tcg.parent = cg;
  tcg.comptime = true;
  tcg.debug_symbols = cg->debug_symbols;
  tcg.di_builder = NULL;

  LLVMValueRef entry_fn = LLVMAddFunction(
      mod, entry_name, LLVMFunctionType(tcg.type_i64, NULL, 0, 0));
  LLVMPositionBuilderAtEnd(
      bld, LLVMAppendBasicBlockInContext(ctm_ctx, entry_fn, "e"));

  if (tcg.debug_symbols) {
    ny_dbg_loc(&tcg, body->tok);
  }

  scope sc[64] = {0};
  size_t d = 0;
  gen_stmt(&tcg, sc, &d, body, 0, true);
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(bld)))
    LLVMBuildRet(bld, LLVMConstInt(tcg.type_i64, 1, false));

  // 7. JIT-execute the snapshotted module with MCJIT
  ny_jit_init_native_once();

  LLVMExecutionEngineRef ee;
  struct LLVMMCJITCompilerOptions jopt;
  LLVMInitializeMCJITCompilerOptions(&jopt, sizeof(jopt));
  jopt.OptLevel = 0;
#ifdef _WIN32
  jopt.CodeModel = LLVMCodeModelLarge;
#endif

  if (LLVMCreateMCJITCompilerForModule(&ee, mod, &jopt, sizeof(jopt), &err) !=
      0) {
    codegen_dispose(&tcg);
    if (!ctm_ctx_owned && mod)
      LLVMDisposeModule(mod);
    if (ctm_ctx_owned && ctm_ctx)
      LLVMContextDispose(ctm_ctx);
    if (prev_bb)
      LLVMPositionBuilderAtEnd(cg->builder, prev_bb);
    return expr_fail(cg, body->tok, "comptime JIT init failed: %s",
                     err ? err : "unknown error");
  }
  tcg.ee = ee;

  // 8. Dynamic Symbol Mapping
  // Register built-in runtime symbols first; this is required on Windows
  // where many symbols are not exported for dynamic symbol lookup.
  register_jit_symbols(ee, mod, &tcg);
  ny_jit_map_unresolved_symbols(ee, mod, entry_name);

  uint64_t saddr = LLVMGetFunctionAddress(ee, entry_name);
  int64_t res = 0;
  if (saddr) {
    int64_t (*ctm_fn)(void) = (int64_t (*)(void))saddr;
    res = ctm_fn();
  }

  tcg.module = NULL; // EE owns this module now.
  codegen_dispose(&tcg);
  if (ctm_ctx_owned && ctm_ctx)
    LLVMContextDispose(ctm_ctx);

  if (prev_bb)
    LLVMPositionBuilderAtEnd(cg->builder, prev_bb);

  if (!saddr)
    return expr_fail(cg, body->tok,
                     "comptime JIT entry point discovery failed");

  if (!is_int(res) && !is_ptr(res)) {
    return expr_fail(cg, body->tok,
                     "comptime must return tagged int64 or bool");
  }
  return LLVMConstInt(cg->type_i64, (uint64_t)res, true);
}

static LLVMValueRef gen_expr_unary(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.unary.right);
  if (!r)
    return expr_fail(cg, e->tok, "failed to evaluate unary operand");
  ny_dbg_loc(cg, e->tok);
  if (strcmp(e->as.unary.op, "!") == 0)
    return LLVMBuildSelect(cg->builder, to_bool(cg, r),
                           LLVMConstInt(cg->type_i64, 4, false),
                           LLVMConstInt(cg->type_i64, 2, false), "");
  if (strcmp(e->as.unary.op, "-") == 0) {
    fun_sig *s = ny_helper_sub(cg);
    if (!s || !s->type || !s->value)
      return expr_fail(cg, e->tok, "builtin __sub missing");
    LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
    LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(fn, "un.int.fast");
    LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(fn, "un.runtime.slow");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(fn, "un.merge");
    LLVMBuildCondBr(cg->builder, ny_is_tagged_int(cg, r), fast_bb, slow_bb);

    LLVMPositionBuilderAtEnd(cg->builder, fast_bb);
    LLVMValueRef raw = ny_untag_int(cg, r);
    LLVMValueRef neg = LLVMBuildSub(
        cg->builder, LLVMConstInt(cg->type_i64, 0, false), raw, "neg_int");
    LLVMValueRef fast_value = ny_tag_int(cg, neg);
    LLVMBasicBlockRef fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, slow_bb);
    LLVMValueRef slow_value = LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){LLVMConstInt(cg->type_i64, 1, false), r}, 2, "");
    LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "un_result");
    LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
    LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
    LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
    return phi;
  }
  if (strcmp(e->as.unary.op, "~") == 0) {
    fun_sig *s = ny_helper_not(cg);
    if (!s || !s->type || !s->value)
      return expr_fail(cg, e->tok, "builtin __not missing");
    LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef fn = LLVMGetBasicBlockParent(entry_bb);
    LLVMBasicBlockRef fast_bb = LLVMAppendBasicBlock(fn, "not.int.fast");
    LLVMBasicBlockRef slow_bb = LLVMAppendBasicBlock(fn, "not.runtime.slow");
    LLVMBasicBlockRef merge_bb = LLVMAppendBasicBlock(fn, "not.merge");
    LLVMBuildCondBr(cg->builder, ny_is_tagged_int(cg, r), fast_bb, slow_bb);

    LLVMPositionBuilderAtEnd(cg->builder, fast_bb);
    LLVMValueRef raw = ny_untag_int(cg, r);
    LLVMValueRef inv = LLVMBuildNot(cg->builder, raw, "not_int");
    LLVMValueRef fast_value = ny_tag_int(cg, inv);
    LLVMBasicBlockRef fast_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, slow_bb);
    LLVMValueRef slow_value = LLVMBuildCall2(cg->builder, s->type, s->value,
                                             (LLVMValueRef[]){r}, 1, "");
    LLVMBasicBlockRef slow_done_bb = LLVMGetInsertBlock(cg->builder);
    LLVMBuildBr(cg->builder, merge_bb);

    LLVMPositionBuilderAtEnd(cg->builder, merge_bb);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "not_result");
    LLVMValueRef incoming_vals[2] = {fast_value, slow_value};
    LLVMBasicBlockRef incoming_bbs[2] = {fast_done_bb, slow_done_bb};
    LLVMAddIncoming(phi, incoming_vals, incoming_bbs, 2);
    return phi;
  }
  return expr_fail(cg, e->tok, "unsupported unary operator '%s'",
                   e->as.unary.op);
}

static LLVMValueRef gen_expr_index(codegen_t *cg, scope *scopes, size_t depth,
                                   expr_t *e) {
  if (e->as.index.stop || e->as.index.step || !e->as.index.start) {
    fun_sig *s = ny_helper_slice(cg);
    if (!s)
      return expr_fail(cg, e->tok, "slice operation requires 'slice'");
    LLVMValueRef start = e->as.index.start
                             ? gen_expr(cg, scopes, depth, e->as.index.start)
                             : LLVMConstInt(cg->type_i64, 1, false); // 0 tagged
    LLVMValueRef stop =
        e->as.index.stop
            ? gen_expr(cg, scopes, depth, e->as.index.stop)
            : LLVMConstInt(cg->type_i64, ((0x3fffffffULL) << 1) | 1, false);
    LLVMValueRef step = e->as.index.step
                            ? gen_expr(cg, scopes, depth, e->as.index.step)
                            : LLVMConstInt(cg->type_i64, 3, false); // 1 tagged
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(
        cg->builder, s->type, s->value,
        (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target), start,
                         stop, step},
        4, "");
  }
  fun_sig *s = ny_helper_get(cg);
  if (!s)
    return expr_fail(cg, e->tok, "index operation requires 'get'");
  ny_dbg_loc(cg, e->tok);
  return LLVMBuildCall2(
      cg->builder, s->type, s->value,
      (LLVMValueRef[]){gen_expr(cg, scopes, depth, e->as.index.target),
                       gen_expr(cg, scopes, depth, e->as.index.start)},
      2, "");
}

static LLVMValueRef gen_expr_list_like(codegen_t *cg, scope *scopes,
                                       size_t depth, expr_t *e) {
  fun_sig *ls = ny_helper_list(cg);
  fun_sig *st = lookup_fun(cg, "__store64_idx");
  if (!ls || !st)
    return expr_fail(cg, e->tok,
                     "list literal requires list/__store64_idx helpers");
  ny_dbg_loc(cg, e->tok);
  size_t item_count = e->as.list_like.len;
  size_t list_cap = item_count > 0 ? item_count : 1;
  LLVMValueRef vl = LLVMBuildCall2(
      cg->builder, ls->type, ls->value,
      (LLVMValueRef[]){
          LLVMConstInt(cg->type_i64, (((uint64_t)list_cap << 1) | 1u), false)},
      1, "");
  for (size_t i = 0; i < item_count; i++) {
    LLVMValueRef item = gen_expr(cg, scopes, depth, e->as.list_like.data[i]);
    uint64_t tagged_off = ((((uint64_t)16 + (uint64_t)i * 8u) << 1) | 1u);
    ny_dbg_loc(cg, e->tok);
    (void)LLVMBuildCall2(
        cg->builder, st->type, st->value,
        (LLVMValueRef[]){vl, LLVMConstInt(cg->type_i64, tagged_off, false),
                         item},
        3, "");
  }
  ny_dbg_loc(cg, e->tok);
  (void)LLVMBuildCall2(
      cg->builder, st->type, st->value,
      (LLVMValueRef[]){vl, LLVMConstInt(cg->type_i64, 1, false),
                       LLVMConstInt(cg->type_i64,
                                    (((uint64_t)item_count << 1) | 1u), false)},
      3, "");
  return vl;
}

static LLVMValueRef gen_expr_dict(codegen_t *cg, scope *scopes, size_t depth,
                                  expr_t *e) {
  fun_sig *ds = ny_helper_dict(cg);
  fun_sig *ss = ny_helper_dict_set(cg);
  if (!ds || !ss)
    return expr_fail(cg, e->tok, "dict literal requires dict/dict_set helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef dl = LLVMBuildCall2(
      cg->builder, ds->type, ds->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.dict.pairs.len << 2) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.dict.pairs.len; i++)
    LLVMBuildCall2(
        cg->builder, ss->type, ss->value,
        (LLVMValueRef[]){
            dl, gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].key),
            gen_expr(cg, scopes, depth, e->as.dict.pairs.data[i].value)},
        3, "");
  return dl;
}

static LLVMValueRef gen_expr_set(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e) {
  fun_sig *ss = ny_helper_set(cg);
  fun_sig *as = ny_helper_set_add(cg);
  if (!ss || !as)
    return expr_fail(cg, e->tok, "set literal requires set/set_add helpers");
  ny_dbg_loc(cg, e->tok);
  LLVMValueRef sl = LLVMBuildCall2(
      cg->builder, ss->type, ss->value,
      (LLVMValueRef[]){LLVMConstInt(
          cg->type_i64, ((uint64_t)e->as.list_like.len << 1) | 1, false)},
      1, "");
  for (size_t i = 0; i < e->as.list_like.len; i++)
    LLVMBuildCall2(cg->builder, as->type, as->value,
                   (LLVMValueRef[]){sl, gen_expr(cg, scopes, depth,
                                                 e->as.list_like.data[i])},
                   2, "");
  return sl;
}

static LLVMValueRef gen_expr_logical(codegen_t *cg, scope *scopes, size_t depth,
                                     expr_t *e) {
  bool and = strcmp(e->as.logical.op, "&&") == 0;
  ny_null_narrow_list_t rhs_narrow;
  vec_init(&rhs_narrow);
  bool narrow_rhs =
      ny_null_narrow_collect_logical_rhs(e->as.logical.left, and, &rhs_narrow);
  LLVMValueRef left =
      to_bool(cg, gen_expr(cg, scopes, depth, e->as.logical.left));
  LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
  LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
  LLVMBasicBlockRef rhs_bb = LLVMAppendBasicBlock(f, "lrhs"),
                    end_bb = LLVMAppendBasicBlock(f, "lend");
  ny_dbg_loc(cg, e->tok);
  if (and)
    LLVMBuildCondBr(cg->builder, left, rhs_bb, end_bb);
  else
    LLVMBuildCondBr(cg->builder, left, end_bb, rhs_bb);
  LLVMPositionBuilderAtEnd(cg->builder, rhs_bb);
  ny_null_narrow_restore_list_t rhs_applied;
  if (narrow_rhs)
    ny_null_narrow_apply(cg, scopes, depth, &rhs_narrow, true, &rhs_applied);
  LLVMValueRef rv = gen_expr(cg, scopes, depth, e->as.logical.right);
  if (narrow_rhs)
    ny_null_narrow_restore(&rhs_applied);
  vec_free(&rhs_narrow);
  LLVMBuildBr(cg->builder, end_bb);
  LLVMBasicBlockRef rend_bb = LLVMGetInsertBlock(cg->builder);
  LLVMPositionBuilderAtEnd(cg->builder, end_bb);
  LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "");
  LLVMAddIncoming(phi,
                  (LLVMValueRef[]){and ? LLVMConstInt(cg->type_i64, 4, false)
                                       : LLVMConstInt(cg->type_i64, 2, false),
                                   rv},
                  (LLVMBasicBlockRef[]){cur_bb, rend_bb}, 2);
  return phi;
}

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e) {

  // Check for dead code - don't generate instructions if block is terminated
  if (cg->builder) {
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    if (cur_bb && LLVMGetBasicBlockTerminator(cur_bb)) {
      return LLVMGetUndef(cg->type_i64);
    }
  }
  if (!e || cg->had_error)
    return LLVMConstInt(cg->type_i64, 0, false);
  ny_braun_mark_current_block(cg);
  ny_dbg_loc(cg, e->tok);

  // fprintf(stderr, "DEBUG: gen_expr kind %d tok %d\n", e->kind, e->tok.kind);

  switch (e->kind) {
  case NY_E_COMPTIME:
    return gen_comptime_eval(cg, e->as.comptime_expr.body);
  case NY_E_LITERAL:
    if (e->as.literal.kind == NY_LIT_INT)
      return LLVMConstInt(cg->type_i64, ((uint64_t)e->as.literal.as.i << 1) | 1,
                          true);
    if (e->as.literal.kind == NY_LIT_BOOL)
      return LLVMConstInt(cg->type_i64, e->as.literal.as.b ? 2 : 4, false);
    if (e->as.literal.kind == NY_LIT_STR) {
      // Get the runtime pointer global for this string
      LLVMValueRef str_runtime_global =
          const_string_ptr(cg, e->as.literal.as.s.data, e->as.literal.as.s.len);
      // Load the pointer value (will be initialized by string init function)
      return LLVMBuildLoad2(cg->builder, cg->type_i64, str_runtime_global,
                            "str_ptr");
    }
    if (e->as.literal.kind == NY_LIT_FLOAT) {
      fun_sig *box_sig = ny_helper_flt_box(cg);
      if (!box_sig) {
        return expr_fail(cg, e->tok, "__flt_box_val not found");
      }
      double fval_d = e->as.literal.as.f;
      if (e->as.literal.hint == NY_LIT_HINT_F32) {
        float f32 = (float)fval_d;
        fval_d = (double)f32;
      }
      LLVMValueRef fval =
          LLVMConstReal(LLVMDoubleTypeInContext(cg->ctx), fval_d);
      return LLVMBuildCall2(cg->builder, box_sig->type, box_sig->value,
                            (LLVMValueRef[]){LLVMBuildBitCast(
                                cg->builder, fval, cg->type_i64, "")},
                            1, "");
    }
    return LLVMConstInt(cg->type_i64, 0, false);
  case NY_E_IDENT: {
    binding *b = scope_lookup(scopes, depth, e->as.ident.name);
    if (b) {
      b->is_used = true;
      if (cg->braun && braun_ssa_is_tracked(cg->braun, e->as.ident.name)) {
        LLVMValueRef v = braun_ssa_read_var(cg->braun, e->as.ident.name);
        if (v)
          return v;
      }
      if (b->is_slot) {
        return LLVMBuildLoad2(cg->builder, cg->type_i64, b->value, "");
      }
      return b->value;
    }
    binding *gb = lookup_global(cg, e->as.ident.name);
    if (gb) {
      gb->is_used = true;
      if (gb->is_slot) {
        return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
      }
      return gb->value;
    }
    fun_sig *s = lookup_fun(cg, e->as.ident.name);
    if (s) {
      LLVMValueRef sv = s->value;
      // Return raw callable pointer as int. Do not apply ad-hoc low-bit tags:
      // on 32-bit targets, tag `2` collides with NY_NATIVE_TAG and causes
      // __callN/__thread dispatch to treat Nytrix functions as native pointers.
      return LLVMBuildPtrToInt(cg->builder, sv, cg->type_i64, "");
    }

    // NEW: Try resolving as an unqualified enum member
    enum_member_def_t *emd = lookup_enum_member(cg, e->as.ident.name);
    if (emd) {
      return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1, true);
    }

    LLVMValueRef host_ident = ny_try_host_platform_ident(cg, e->as.ident.name);
    if (host_ident)
      return host_ident;

    report_undef_symbol(cg, e->as.ident.name, e->tok);
    return LLVMConstInt(cg->type_i64, 0, false);
  }
  case NY_E_UNARY: {
    return gen_expr_unary(cg, scopes, depth, e);
  }
  case NY_E_BINARY: {
    LLVMValueRef l = gen_expr(cg, scopes, depth, e->as.binary.left);
    LLVMValueRef r = gen_expr(cg, scopes, depth, e->as.binary.right);
    ny_dbg_loc(cg, e->tok);
    return gen_binary(cg, e->as.binary.op, l, r);
  }
  case NY_E_CALL:
  case NY_E_MEMCALL:
    return gen_call_expr(cg, scopes, depth, e);
  case NY_E_INDEX: {
    return gen_expr_index(cg, scopes, depth, e);
  }
  case NY_E_LIST:
  case NY_E_TUPLE: {
    return gen_expr_list_like(cg, scopes, depth, e);
  }
  case NY_E_DICT: {
    return gen_expr_dict(cg, scopes, depth, e);
  }
  case NY_E_SET: {
    return gen_expr_set(cg, scopes, depth, e);
  }
  case NY_E_PTR_TYPE:
    return LLVMConstInt(cg->type_i64, 0, false);
  case NY_E_DEREF: {
    LLVMValueRef ptr = gen_expr(cg, scopes, depth, e->as.deref.target);
    // Low level: treat ptr as raw address
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef raw_ptr =
        LLVMBuildIntToPtr(cg->builder, ptr, cg->type_i8ptr, "raw_ptr");
    return LLVMBuildLoad2(cg->builder, cg->type_i64, raw_ptr, "deref");
  }
  case NY_E_MEMBER: {
    // First, attempt to resolve as a static enum member or qualified global.
    char *full_name = codegen_full_name(cg, e, cg->arena);
    if (full_name) {
      enum_member_def_t *emd = lookup_enum_member(cg, full_name);
      if (emd) {
        return LLVMConstInt(cg->type_i64, ((uint64_t)emd->value << 1) | 1,
                            true);
      }
      binding *gb = lookup_global(cg, full_name);
      if (gb) {
        gb->is_used = true;
        if (gb->is_slot)
          return LLVMBuildLoad2(cg->builder, cg->type_i64, gb->value, "");
        return gb->value;
      }
    }

    LLVMValueRef target = gen_expr(cg, scopes, depth, e->as.member.target);
    LLVMValueRef key_str_global =
        const_string_ptr(cg, e->as.member.name, strlen(e->as.member.name));
    LLVMValueRef key_str =
        LLVMBuildLoad2(cg->builder, cg->type_i64, key_str_global, "");

    fun_sig *get_sig = ny_helper_get(cg);
    if (!get_sig) {
      return expr_fail(
          cg, e->tok,
          "Member access on a dynamic object requires the 'get' function.");
    }

    // Assume `get` can take a default value as a third argument.
    LLVMValueRef args[] = {target, key_str};
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, get_sig->type, get_sig->value, args, 2,
                          "");
  }
  case NY_E_SIZEOF: {
    const char *type_name = NULL;
    if (e->as.szof.is_type)
      type_name = e->as.szof.type_name;
    if (!type_name && e->as.szof.target &&
        e->as.szof.target->kind == NY_E_IDENT) {
      type_name = e->as.szof.target->as.ident.name;
    }
    if (!type_name) {
      ny_diag_error(e->tok, "sizeof expects a type name");
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 1, false);
    }
    type_layout_t tl = resolve_raw_layout(cg, type_name, e->tok);
    if (!tl.is_valid) {
      cg->had_error = 1;
      return LLVMConstInt(cg->type_i64, 1, false);
    }
    uint64_t sz = (uint64_t)tl.size;
    return LLVMConstInt(cg->type_i64, (sz << 1) | 1ULL, false);
  }
  case NY_E_LOGICAL: {
    return gen_expr_logical(cg, scopes, depth, e);
  }
  case NY_E_TERNARY: {
    LLVMValueRef cond =
        to_bool(cg, gen_expr(cg, scopes, depth, e->as.ternary.cond));
    LLVMBasicBlockRef cur_bb = LLVMGetInsertBlock(cg->builder);
    LLVMValueRef f = LLVMGetBasicBlockParent(cur_bb);
    LLVMBasicBlockRef true_bb = LLVMAppendBasicBlock(f, "tern_true");
    LLVMBasicBlockRef false_bb = LLVMAppendBasicBlock(f, "tern_false");
    LLVMBasicBlockRef end_bb = LLVMAppendBasicBlock(f, "tern_end");
    ny_dbg_loc(cg, e->tok);
    LLVMBuildCondBr(cg->builder, cond, true_bb, false_bb);
    LLVMPositionBuilderAtEnd(cg->builder, true_bb);
    LLVMValueRef true_val =
        gen_expr(cg, scopes, depth, e->as.ternary.true_expr);
    LLVMBuildBr(cg->builder, end_bb);
    LLVMBasicBlockRef true_end_bb = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, false_bb);
    LLVMValueRef false_val =
        gen_expr(cg, scopes, depth, e->as.ternary.false_expr);
    LLVMBuildBr(cg->builder, end_bb);
    LLVMBasicBlockRef false_end_bb = LLVMGetInsertBlock(cg->builder);
    LLVMPositionBuilderAtEnd(cg->builder, end_bb);
    ny_dbg_loc(cg, e->tok);
    LLVMValueRef phi = LLVMBuildPhi(cg->builder, cg->type_i64, "tern");
    LLVMAddIncoming(phi, (LLVMValueRef[]){true_val, false_val},
                    (LLVMBasicBlockRef[]){true_end_bb, false_end_bb}, 2);
    return phi;
  }
  case NY_E_ASM: {
    unsigned nargs = e->as.as_asm.args.len;
    LLVMValueRef llvm_args[nargs > 0 ? nargs : 1];
    LLVMTypeRef arg_types[nargs > 0 ? nargs : 1];
    for (unsigned i = 0; i < nargs; ++i) {
      llvm_args[i] = gen_expr(cg, scopes, depth, e->as.as_asm.args.data[i]);
      arg_types[i] = cg->type_i64;
    }
    LLVMTypeRef func_type =
        LLVMFunctionType(cg->type_i64, arg_types, nargs, false);
    LLVMValueRef asm_val = LLVMConstInlineAsm(
        func_type, e->as.as_asm.code, e->as.as_asm.constraints, true, false);
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, func_type, asm_val, llvm_args, nargs,
                          "");
  }
  case NY_E_FSTRING: {
    // Empty string init
    LLVMValueRef empty_runtime_global = const_string_ptr(cg, "", 0);
    LLVMValueRef res =
        LLVMBuildLoad2(cg->builder, cg->type_i64, empty_runtime_global, "");
    fun_sig *cs = ny_helper_str_concat(cg), *ts = ny_helper_to_str(cg);
    for (size_t i = 0; i < e->as.fstring.parts.len; i++) {
      fstring_part_t p = e->as.fstring.parts.data[i];
      LLVMValueRef pv;
      if (p.kind == NY_FSP_STR) {
        LLVMValueRef part_runtime_global =
            const_string_ptr(cg, p.as.s.data, p.as.s.len);
        pv = LLVMBuildLoad2(cg->builder, cg->type_i64, part_runtime_global, "");
      } else {
        pv = LLVMBuildCall2(
            cg->builder, ts->type, ts->value,
            (LLVMValueRef[]){gen_expr(cg, scopes, depth, p.as.e)}, 1, "");
      }
      ny_dbg_loc(cg, e->tok);
      res = LLVMBuildCall2(cg->builder, cs->type, cs->value,
                           (LLVMValueRef[]){res, pv}, 2, "");
    }
    return res;
  }
  case NY_E_LAMBDA:
  case NY_E_FN: {
    /* Capture All Visible Variables (scopes[1..depth]) */
    return gen_closure(cg, scopes, depth, e->as.lambda.params,
                       e->as.lambda.body, e->as.lambda.is_variadic,
                       e->as.lambda.return_type, "__lambda");
  }
  case NY_E_EMBED: {
    const char *fname = e->as.embed.path;
    // fprintf(stderr, "DEBUG: embed opening '%s'\n", fname);
    FILE *f = fopen(fname, "rb");
    if (!f) {
      char cwd[1024];
      if (!getcwd(cwd, sizeof(cwd)))
        cwd[0] = '\0';
      return expr_fail(cg, e->tok,
                       "failed to open file for embed: %s (cwd: %s)", fname,
                       cwd);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
      fclose(f);
      return expr_fail(cg, e->tok, "OOM reading file for embed");
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
      free(buf);
      fclose(f);
      return expr_fail(cg, e->tok, "failed to read file for embed: %s", fname);
    }
    fclose(f);
    LLVMValueRef g = const_string_ptr(cg, buf, (size_t)size);
    free(buf);
    return LLVMBuildLoad2(cg->builder, cg->type_i64, g, "embed_ptr");
  }
  case NY_E_TRY: {
    LLVMValueRef res = gen_expr(cg, scopes, depth, e->as.unary.right);
    LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
    LLVMBasicBlockRef ok_bb = LLVMAppendBasicBlock(f, "try_ok");
    LLVMBasicBlockRef err_bb = LLVMAppendBasicBlock(f, "try_err");

    fun_sig *is_ok_sig = lookup_fun(cg, "__is_ok");
    if (!is_ok_sig) {
      return expr_fail(cg, e->tok, "__is_ok not found for '?' operator");
    }
    LLVMValueRef is_ok = LLVMBuildCall2(cg->builder, is_ok_sig->type,
                                        is_ok_sig->value, &res, 1, "");
    ny_dbg_loc(cg, e->tok);
    LLVMBuildCondBr(cg->builder, to_bool(cg, is_ok), ok_bb, err_bb);

    LLVMPositionBuilderAtEnd(cg->builder, err_bb);
    // return res (which is the error result)
    if (cg->result_store_val) {
      LLVMBuildStore(cg->builder, res, cg->result_store_val);
    } else {
      emit_defers(cg, scopes, depth, cg->func_root_idx);
      LLVMBuildRet(cg->builder, res);
    }
    // We need a dummy terminator if there are instructions after this try

    LLVMPositionBuilderAtEnd(cg->builder, ok_bb);
    // Unwrap value
    fun_sig *unwrap_sig = lookup_fun(cg, "__unwrap");
    if (!unwrap_sig) {
      return expr_fail(cg, e->tok, "__unwrap not found for '?' operator");
    }
    ny_dbg_loc(cg, e->tok);
    return LLVMBuildCall2(cg->builder, unwrap_sig->type, unwrap_sig->value,
                          &res, 1, "unwrapped");
  }
  case NY_E_MATCH: {
    LLVMValueRef old_store = cg->result_store_val;
    LLVMValueRef slot = build_alloca(cg, "match_res", cg->type_i64);
    LLVMBuildStore(cg->builder, LLVMConstInt(cg->type_i64, 1, false), slot);
    cg->result_store_val = slot;
    stmt_t fake = {.kind = NY_S_MATCH, .as.match = e->as.match, .tok = e->tok};
    size_t d = depth;
    gen_stmt(cg, scopes, &d, &fake, cg->func_root_idx, true);
    cg->result_store_val = old_store;
    return LLVMBuildLoad2(cg->builder, cg->type_i64, slot, "");
  }
  default: {
    return expr_fail(cg, e->tok,
                     "unsupported expression kind %d (token kind %d)", e->kind,
                     e->tok.kind);
  }
  }
}
