#include "ast/ast.h"
#include <stdio.h>
#include <stdlib.h>

expr_t *expr_new(arena_t *arena, expr_kind_t kind, token_t tok) {
  if (!arena) {
    NY_LOG_DEBUG("expr_new called with NULL arena!\n");
  }
  expr_t *e = (expr_t *)arena_alloc(arena, sizeof(expr_t));
  e->kind = kind;
  e->tok = tok;
  memset(&e->as, 0, sizeof(e->as));
  return e;
}

stmt_t *stmt_new(arena_t *arena, stmt_kind_t kind, token_t tok) {
  if (!arena) {
    NY_LOG_DEBUG("stmt_new called with NULL arena!\n");
  }
  stmt_t *s = (stmt_t *)arena_alloc(arena, sizeof(stmt_t));

  s->kind = kind;
  s->tok = tok;
  memset(&s->as, 0, sizeof(s->as));
  return s;
}

void expr_free_members(expr_t *e) { (void)e; }
void stmt_free_members(stmt_t *s) { (void)s; }

void program_free(program_t *prog, arena_t *arena) {
  (void)prog;
  arena_free(arena);
  free(arena);
}

#include "ast/json.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// External runtime functions
extern int64_t __malloc(int64_t n);
extern int64_t __free(int64_t p);
extern int64_t __realloc(int64_t p, int64_t n);

static void dump_expr(expr_t *e, char **buf, size_t *len, size_t *cap);
static void dump_stmt(stmt_t *s, char **buf, size_t *len, size_t *cap);

static void append(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (*len + n + 1 > *cap) {
    *cap = (*len + n + 1) * 2;
    *buf = (char *)(uintptr_t)__realloc((int64_t)(uintptr_t)*buf, *cap);
  }
  va_start(ap, fmt);
  vsnprintf(*buf + *len, n + 1, fmt, ap);
  va_end(ap);
  *len += n;
}

static void dump_literal(literal_t *l, char **buf, size_t *len, size_t *cap) {
  const char *hint = NULL;
  switch (l->hint) {
  case NY_LIT_HINT_I8:
    hint = "i8";
    break;
  case NY_LIT_HINT_I16:
    hint = "i16";
    break;
  case NY_LIT_HINT_I32:
    hint = "i32";
    break;
  case NY_LIT_HINT_I64:
    hint = "i64";
    break;
  case NY_LIT_HINT_U8:
    hint = "u8";
    break;
  case NY_LIT_HINT_U16:
    hint = "u16";
    break;
  case NY_LIT_HINT_U32:
    hint = "u32";
    break;
  case NY_LIT_HINT_U64:
    hint = "u64";
    break;
  case NY_LIT_HINT_F32:
    hint = "f32";
    break;
  case NY_LIT_HINT_F64:
    hint = "f64";
    break;
  case NY_LIT_HINT_F128:
    hint = "f128";
    break;
  case NY_LIT_HINT_NONE:
  default:
    hint = NULL;
    break;
  }

  append(buf, len, cap, "{\"type\":\"literal\",\"kind\":");
  switch (l->kind) {
  case NY_LIT_INT:
    if (hint)
      append(buf, len, cap, "\"int\",\"hint\":\"%s\",\"value\":%ld}", hint,
             l->as.i);
    else
      append(buf, len, cap, "\"int\",\"value\":%ld}", l->as.i);
    break;
  case NY_LIT_FLOAT:
    if (hint)
      append(buf, len, cap, "\"float\",\"hint\":\"%s\",\"value\":%f}", hint,
             l->as.f);
    else
      append(buf, len, cap, "\"float\",\"value\":%f}", l->as.f);
    break;
  case NY_LIT_BOOL:
    if (hint)
      append(buf, len, cap, "\"bool\",\"hint\":\"%s\",\"value\":%s}", hint,
             l->as.b ? "true" : "false");
    else
      append(buf, len, cap, "\"bool\",\"value\":%s}",
             l->as.b ? "true" : "false");
    break;
  case NY_LIT_STR:
    if (hint)
      append(buf, len, cap, "\"string\",\"hint\":\"%s\",\"value\":\"", hint);
    else
      append(buf, len, cap, "\"string\",\"value\":\"");
    for (size_t i = 0; i < l->as.s.len; ++i) {
      char c = l->as.s.data[i];
      if (c == '"')
        append(buf, len, cap, "\\\"");
      else if (c == '\\')
        append(buf, len, cap, "\\\\");
      else if (c == '\n')
        append(buf, len, cap, "\\n");
      else
        append(buf, len, cap, "%c", c);
    }
    append(buf, len, cap, "\"}");
    break;
  }
}

static void dump_expr(expr_t *e, char **buf, size_t *len, size_t *cap) {
  if (!e) {
    append(buf, len, cap, "null");
    return;
  }
  switch (e->kind) {
  case NY_E_IDENT:
    append(buf, len, cap, "{\"type\":\"ident\",\"name\":\"%s\"}",
           e->as.ident.name);
    break;
  case NY_E_LITERAL:
    dump_literal(&e->as.literal, buf, len, cap);
    break;
  case NY_E_UNARY:
    append(buf, len, cap,
           "{\"type\":\"unary\",\"op\":\"%s\",\"right\":", e->as.unary.op);
    dump_expr(e->as.unary.right, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_E_BINARY:
    append(buf, len, cap,
           "{\"type\":\"binary\",\"op\":\"%s\",\"left\":", e->as.binary.op);
    dump_expr(e->as.binary.left, buf, len, cap);
    append(buf, len, cap, ",\"right\":");
    dump_expr(e->as.binary.right, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_E_LOGICAL:
    append(buf, len, cap,
           "{\"type\":\"logical\",\"op\":\"%s\",\"left\":", e->as.logical.op);
    dump_expr(e->as.logical.left, buf, len, cap);
    append(buf, len, cap, ",\"right\":");
    dump_expr(e->as.logical.right, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_E_CALL:
    append(buf, len, cap, "{\"type\":\"call\",\"callee\":");
    dump_expr(e->as.call.callee, buf, len, cap);
    append(buf, len, cap, ",\"args\":[");
    for (size_t i = 0; i < e->as.call.args.len; ++i) {
      // For now, ignoring arg name in JSON output to preserve structure
      dump_expr(e->as.call.args.data[i].val, buf, len, cap);
      if (i < e->as.call.args.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_E_MEMCALL:
    append(buf, len, cap, "{\"type\":\"memcall\",\"target\":");
    dump_expr(e->as.memcall.target, buf, len, cap);
    append(buf, len, cap, ",\"name\":\"%s\",\"args\":[", e->as.memcall.name);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i) {
      dump_expr(e->as.memcall.args.data[i].val, buf, len, cap);
      if (i < e->as.memcall.args.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_E_INDEX:
    append(buf, len, cap, "{\"type\":\"index\",\"target\":");
    dump_expr(e->as.index.target, buf, len, cap);
    append(buf, len, cap, ",\"start\":");
    dump_expr(e->as.index.start, buf, len, cap);
    append(buf, len, cap, ",\"stop\":");
    dump_expr(e->as.index.stop, buf, len, cap);
    append(buf, len, cap, ",\"step\":");
    dump_expr(e->as.index.step, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_E_LAMBDA:
    append(buf, len, cap, "{\"type\":\"lambda\",\"params\":[");
    for (size_t i = 0; i < e->as.lambda.params.len; ++i) {
      append(buf, len, cap, "{\"name\":\"%s\"}",
             e->as.lambda.params.data[i].name);
      if (i < e->as.lambda.params.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "] ,\"body\":");
    dump_stmt(e->as.lambda.body, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    append(buf, len, cap, "{\"type\":\"%s\",\"elements\":[",
           e->kind == NY_E_LIST ? "list"
                                : (e->kind == NY_E_TUPLE ? "tuple" : "set"));
    for (size_t i = 0; i < e->as.list_like.len; ++i) {
      dump_expr(e->as.list_like.data[i], buf, len, cap);
      if (i < e->as.list_like.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_E_DICT:
    append(buf, len, cap, "{\"type\":\"dict\",\"pairs\":[");
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      append(buf, len, cap, "{\"key\":");
      dump_expr(e->as.dict.pairs.data[i].key, buf, len, cap);
      append(buf, len, cap, ",\"value\":");
      dump_expr(e->as.dict.pairs.data[i].value, buf, len, cap);
      append(buf, len, cap, "}");
      if (i < e->as.dict.pairs.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_E_ASM:
    append(
        buf, len, cap,
        "{\"type\":\"asm\",\"code\":\"%s\",\"constraints\":\"%s\",\"args\":[",
        e->as.as_asm.code, e->as.as_asm.constraints);
    for (size_t i = 0; i < e->as.as_asm.args.len; ++i) {
      dump_expr(e->as.as_asm.args.data[i], buf, len, cap);
      if (i < e->as.as_asm.args.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  default:
    append(buf, len, cap, "{\"type\":\"unknown\"}");
    break;
  }
}

static void dump_stmt(stmt_t *s, char **buf, size_t *len, size_t *cap) {
  if (!s) {
    append(buf, len, cap, "null");
    return;
  }
  switch (s->kind) {
  case NY_S_BLOCK:
    append(buf, len, cap, "{\"type\":\"block\",\"body\":[");
    for (size_t i = 0; i < s->as.block.body.len; ++i) {
      dump_stmt(s->as.block.body.data[i], buf, len, cap);
      if (i < s->as.block.body.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_S_USE:
    append(buf, len, cap, "{\"type\":\"use\",\"module\":\"%s\"",
           s->as.use.module);
    if (s->as.use.alias) {
      append(buf, len, cap, ",\"alias\":\"%s\"", s->as.use.alias);
    }
    if (s->as.use.is_local) {
      append(buf, len, cap, ",\"local\":true");
    }
    append(buf, len, cap, "}");
    break;
  case NY_S_VAR:
    append(buf, len, cap, "{\"type\":\"var\",\"names\":[");
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      append(buf, len, cap, "\"%s\"", s->as.var.names.data[i]);
      if (i < s->as.var.names.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "],\"undef\":%s,\"exprs\":[",
           s->as.var.is_undef ? "true" : "false");
    for (size_t i = 0; i < s->as.var.exprs.len; ++i) {
      dump_expr(s->as.var.exprs.data[i], buf, len, cap);
      if (i < s->as.var.exprs.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "]}");
    break;
  case NY_S_EXPR:
    append(buf, len, cap, "{\"type\":\"expr_stmt\",\"expr_t\":");
    dump_expr(s->as.expr.expr, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_IF:
    append(buf, len, cap, "{\"type\":\"if\",\"test\":");
    dump_expr(s->as.iff.test, buf, len, cap);
    append(buf, len, cap, ",\"conseq\":");
    dump_stmt(s->as.iff.conseq, buf, len, cap);
    append(buf, len, cap, ",\"alt\":");
    dump_stmt(s->as.iff.alt, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_WHILE:
    append(buf, len, cap, "{\"type\":\"while\",\"test\":");
    dump_expr(s->as.whl.test, buf, len, cap);
    append(buf, len, cap, ",\"body\":");
    dump_stmt(s->as.whl.body, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_FOR:
    append(buf, len, cap,
           "{\"type\":\"for\",\"var\":\"%s\",\"iterable\":", s->as.fr.iter_var);
    dump_expr(s->as.fr.iterable, buf, len, cap);
    append(buf, len, cap, ",\"body\":");
    dump_stmt(s->as.fr.body, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_TRY:
    append(buf, len, cap, "{\"type\":\"try\",\"body\":");
    dump_stmt(s->as.tr.body, buf, len, cap);
    append(buf, len, cap, ",\"err\":\"%s\",\"handler\":",
           s->as.tr.err ? s->as.tr.err : "null");
    dump_stmt(s->as.tr.handler, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_FUNC:
    append(buf, len, cap, "{\"type\":\"func\",\"name\":\"%s\",\"params\":[",
           s->as.fn.name);
    for (size_t i = 0; i < s->as.fn.params.len; ++i) {
      append(buf, len, cap, "{\"name\":\"%s\"}", s->as.fn.params.data[i].name);
      if (i < s->as.fn.params.len - 1)
        append(buf, len, cap, ",");
    }
    append(buf, len, cap, "] ,\"body\":");
    dump_stmt(s->as.fn.body, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_RETURN:
    append(buf, len, cap, "{\"type\":\"return\",\"value\":");
    dump_expr(s->as.ret.value, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_LABEL:
    append(buf, len, cap, "{\"type\":\"label\",\"name\":\"%s\"}",
           s->as.label.name);
    break;
  case NY_S_GOTO:
    append(buf, len, cap, "{\"type\":\"goto\",\"name\":\"%s\"}", s->as.go.name);
    break;
  case NY_S_DEFER:
    append(buf, len, cap, "{\"type\":\"defer\",\"body\":");
    dump_stmt(s->as.de.body, buf, len, cap);
    append(buf, len, cap, "}");
    break;
  case NY_S_BREAK:
    append(buf, len, cap, "{\"type\":\"break\"}");
    break;
  case NY_S_CONTINUE:
    append(buf, len, cap, "{\"type\":\"continue\"}");
    break;
  default:
    append(buf, len, cap, "{\"type\":\"unknown_stmt\"}");
    break;
  }
}

char *ny_ast_to_json(program_t *prog) {
  size_t len = 0, cap = 1024;
  char *buf = (char *)(uintptr_t)__malloc(cap);
  append(&buf, &len, &cap, "[");
  for (size_t i = 0; i < prog->body.len; ++i) {
    dump_stmt(prog->body.data[i], &buf, &len, &cap);
    if (i < prog->body.len - 1)
      append(&buf, &len, &cap, ",");
  }
  append(&buf, &len, &cap, "]");
  return buf;
}
