#include "ast/ast.h"

expr_t *expr_new(arena_t *arena, expr_kind_t kind, token_t tok) {
  expr_t *e = (expr_t *)arena_alloc(arena, sizeof(expr_t));
  e->kind = kind;
  e->tok = tok;
  memset(&e->as, 0, sizeof(e->as));
  return e;
}

stmt_t *stmt_new(arena_t *arena, stmt_kind_t kind, token_t tok) {
  stmt_t *s = (stmt_t *)arena_alloc(arena, sizeof(stmt_t));
  s->kind = kind;
  s->tok = tok;
  memset(&s->as, 0, sizeof(s->as));
  return s;
}

static void free_expr(expr_t *e);
static void free_stmt(stmt_t *s);

static void free_expr_list(ny_expr_list *l) {
  for (size_t i = 0; i < l->len; ++i)
    free_expr(l->data[i]);
  vec_free(l);
}

static void free_stmt_list(ny_stmt_list *l) {
  for (size_t i = 0; i < l->len; ++i)
    free_stmt(l->data[i]);
  vec_free(l);
}

static void free_expr(expr_t *e) {
  if (!e)
    return;
  switch (e->kind) {
  case NY_E_UNARY:
    free_expr(e->as.unary.right);
    break;
  case NY_E_BINARY:
    free_expr(e->as.binary.left);
    free_expr(e->as.binary.right);
    break;
  case NY_E_LOGICAL:
    free_expr(e->as.logical.left);
    free_expr(e->as.logical.right);
    break;
  case NY_E_CALL:
    free_expr(e->as.call.callee);
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      free_expr(e->as.call.args.data[i].val);
    vec_free(&e->as.call.args);
    break;
  case NY_E_MEMCALL:
    free_expr(e->as.memcall.target);
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      free_expr(e->as.memcall.args.data[i].val);
    vec_free(&e->as.memcall.args);
    break;
  case NY_E_INDEX:
    free_expr(e->as.index.target);
    free_expr(e->as.index.start);
    free_expr(e->as.index.stop);
    free_expr(e->as.index.step);
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    for (size_t i = 0; i < e->as.lambda.params.len; ++i)
      free_expr(e->as.lambda.params.data[i].def);
    vec_free(&e->as.lambda.params);
    free_stmt(e->as.lambda.body);
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    free_expr_list(&e->as.list_like);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      free_expr(e->as.dict.pairs.data[i].key);
      free_expr(e->as.dict.pairs.data[i].value);
    }
    vec_free(&e->as.dict.pairs);
    break;
  case NY_E_ASM:
    free_expr_list(&e->as.as_asm.args);
    break;
  case NY_E_COMPTIME:
    free_stmt(e->as.comptime_expr.body);
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      if (e->as.fstring.parts.data[i].kind == NY_FSP_EXPR)
        free_expr(e->as.fstring.parts.data[i].as.e);
    }
    vec_free(&e->as.fstring.parts);
    break;
  default:
    break;
  }
}

static void free_stmt(stmt_t *s) {
  if (!s)
    return;
  switch (s->kind) {
  case NY_S_MODULE:
    free_stmt_list(&s->as.module.body);
    break;
  case NY_S_BLOCK:
    free_stmt_list(&s->as.block.body);
    break;
  case NY_S_VAR:
    vec_free(&s->as.var.names);
    free_expr(s->as.var.expr);
    break;
  case NY_S_EXPR:
    free_expr(s->as.expr.expr);
    break;
  case NY_S_IF:
    free_expr(s->as.iff.test);
    free_stmt(s->as.iff.conseq);
    free_stmt(s->as.iff.alt);
    break;
  case NY_S_WHILE:
    free_expr(s->as.whl.test);
    free_stmt(s->as.whl.body);
    break;
  case NY_S_FOR:
    free_expr(s->as.fr.iterable);
    free_stmt(s->as.fr.body);
    break;
  case NY_S_TRY:
    free_stmt(s->as.tr.body);
    free_stmt(s->as.tr.handler);
    break;
  case NY_S_FUNC:
    for (size_t i = 0; i < s->as.fn.params.len; ++i)
      free_expr(s->as.fn.params.data[i].def);
    vec_free(&s->as.fn.params);
    free_stmt(s->as.fn.body);
    break;
  case NY_S_RETURN:
    free_expr(s->as.ret.value);
    break;
  case NY_S_DEFER:
    free_stmt(s->as.de.body);
    break;
  case NY_S_LAYOUT:
    vec_free(&s->as.layout.fields);
    break;
  case NY_S_MATCH:
    free_expr(s->as.match.test);
    for (size_t i = 0; i < s->as.match.arms.len; ++i) {
      for (size_t j = 0; j < s->as.match.arms.data[i].patterns.len; ++j) {
        free_expr(s->as.match.arms.data[i].patterns.data[j]);
      }
      vec_free(&s->as.match.arms.data[i].patterns);
      free_stmt(s->as.match.arms.data[i].conseq);
    }
    vec_free(&s->as.match.arms);
    free_stmt(s->as.match.default_conseq);
    break;
  default:
    break;
  }
}

void program_free(program_t *prog, arena_t *arena) {
  if (prog) {
    free_stmt_list(&prog->body);
  }
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
  append(buf, len, cap, "{\"type\":\"literal\",\"kind\":");
  switch (l->kind) {
  case NY_LIT_INT:
    append(buf, len, cap, "\"int\",\"value\":%ld}", l->as.i);
    break;
  case NY_LIT_FLOAT:
    append(buf, len, cap, "\"float\",\"value\":%f}", l->as.f);
    break;
  case NY_LIT_BOOL:
    append(buf, len, cap, "\"bool\",\"value\":%s}", l->as.b ? "true" : "false");
    break;
  case NY_LIT_STR:
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
    append(buf, len, cap,
           "],\"undef\":%s,\"expr_t\":", s->as.var.is_undef ? "true" : "false");
    if (s->as.var.expr)
      dump_expr(s->as.var.expr, buf, len, cap);
    else
      append(buf, len, cap, "null");
    append(buf, len, cap, "}");
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
