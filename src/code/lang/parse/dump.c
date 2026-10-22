#include "code/lang/parse/json.h"
#include <inttypes.h>
#include <string.h>

typedef enum {
  D_STMT,
  D_EXPR,
  D_PARAM,
  D_FIELD,
  D_VARIANT,
  D_ENUM_FIELD,
  D_ATTRIBUTE,
  D_ARM,
  D_TEXT
} dump_kind_t;
typedef struct {
  dump_kind_t kind;
  const void *ptr;
  const char *role;
} dump_node_t;
typedef struct dump_frame_t {
  const struct dump_frame_t *parent;
  bool last;
} dump_frame_t;
typedef struct {
  FILE *out;
  bool typed;
} dump_context_t;
typedef struct {
  dump_context_t *ctx;
  const dump_frame_t *parent;
  dump_node_t pending;
  bool ready;
  bool started;
} dump_children_t;

static void dump_node(dump_context_t *, dump_node_t, const dump_frame_t *,
                      bool);

/*
 * One pending child supplies correct final-branch markers without allocating
 * child arrays or traversing the tree a second time.
 */
static void dump_push(dump_children_t *c, dump_kind_t kind, const void *ptr,
                      const char *role) {
  if (!ptr)
    return;
  if (!c->started) {
    fputc('\n', c->ctx->out);
    c->started = true;
  }
  if (c->ready)
    dump_node(c->ctx, c->pending, c->parent, false);
  c->pending = (dump_node_t){kind, ptr, role};
  c->ready = true;
}
static void dump_finish(dump_children_t *c) {
  if (c->ready)
    dump_node(c->ctx, c->pending, c->parent, true);
}
static void dump_indent(FILE *out, const dump_frame_t *frame) {
  if (!frame || !frame->parent)
    return;
  dump_indent(out, frame->parent);
  fputs(frame->last ? "  " : "| ", out);
}
static void dump_string(FILE *out, const char *text, size_t len) {
  fputc('"', out);
  for (size_t i = 0; text && i < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    switch (ch) {
    case '\n':
      fputs("\\n", out);
      break;
    case '\r':
      fputs("\\r", out);
      break;
    case '\t':
      fputs("\\t", out);
      break;
    case '\\':
      fputs("\\\\", out);
      break;
    case '"':
      fputs("\\\"", out);
      break;
    default:
      if (ch < 32 || ch == 127)
        fprintf(out, "\\x%02x", ch);
      else
        fputc(ch, out);
      break;
    }
  }
  fputc('"', out);
}
static void dump_named(FILE *out, const char *key, const char *value) {
  if (!value || !*value)
    return;
  fprintf(out, " %s=", key);
  dump_string(out, value, strlen(value));
}
static void dump_location(FILE *out, token_t tok, const token_t *origin) {
  if (tok.filename)
    fprintf(out, " <%s:%d:%d>", tok.filename, tok.line, tok.col);
  if (origin && origin->filename)
    fprintf(out, " expanded-from=<%s:%d:%d>", origin->filename, origin->line,
            origin->col);
}
static void dump_semantic(dump_context_t *ctx, const ny_expr_semantic_t *sem) {
  if (!ctx->typed)
    return;
  dump_named(ctx->out, "type",
             sem->hm_type_name ? sem->hm_type_name : sem->resolved_type_name);
  if (sem->resolved)
    dump_named(ctx->out, "representation", ny_sem_rep_name(sem->rep));
  dump_named(ctx->out, "callee", sem->canonical_callee);
}
static void dump_params(dump_children_t *c, const ny_param_list *params) {
  for (size_t i = 0; i < params->len; ++i)
    dump_push(c, D_PARAM, &params->data[i], NULL);
}
static void dump_stmts(dump_children_t *c, const ny_stmt_list *stmts) {
  for (size_t i = 0; i < stmts->len; ++i)
    dump_push(c, D_STMT, stmts->data[i], NULL);
}
static void dump_exprs(dump_children_t *c, const ny_expr_list *exprs) {
  for (size_t i = 0; i < exprs->len; ++i)
    dump_push(c, D_EXPR, exprs->data[i], NULL);
}
static void dump_match(dump_children_t *c, const stmt_match_t *match) {
  dump_push(c, D_EXPR, match->test, "value");
  for (size_t i = 0; i < match->arms.len; ++i)
    dump_push(c, D_ARM, &match->arms.data[i], NULL);
  dump_push(c, D_STMT, match->default_conseq, "default");
}
static const char *dump_stmt_name(const stmt_t *s) {
  switch (s->kind) {
  case NY_S_FUNC:
    return s->as.fn.name;
  case NY_S_LEMMA:
    return s->as.lemma.name;
  case NY_S_EXTERN:
    return s->as.ext.name;
  case NY_S_LAYOUT:
    return s->as.layout.name;
  case NY_S_STRUCT:
    return s->as.struc.name;
  case NY_S_ENUM:
    return s->as.enu.name;
  case NY_S_MODULE:
    return s->as.module.name;
  case NY_S_MACRO:
    return s->as.macro.name;
  case NY_S_DEFINE:
    return s->as.def.name;
  case NY_S_REFINE:
    return s->as.refine.name;
  case NY_S_TRAIT:
    return s->as.trait.name;
  case NY_S_IMPL:
    return s->as.impl.type_name;
  case NY_S_OPERATOR:
    return s->as.oper.target;
  case NY_S_LABEL:
    return s->as.label.name;
  case NY_S_GOTO:
    return s->as.go.name;
  default:
    return NULL;
  }
}
static void dump_expr(dump_context_t *ctx, const expr_t *e,
                      dump_children_t *c) {
  static const char *const names[] = {"DeclRefExpr",
                                      "Literal",
                                      "UnaryExpr",
                                      "BinaryExpr",
                                      "LogicalExpr",
                                      "ConditionalExpr",
                                      "CallExpr",
                                      "MemberCallExpr",
                                      "IndexExpr",
                                      "LambdaExpr",
                                      "FunctionExpr",
                                      "ListExpr",
                                      "TupleExpr",
                                      "DictExpr",
                                      "SetExpr",
                                      "AsmExpr",
                                      "ComptimeExpr",
                                      "FStringExpr",
                                      "InferredMemberExpr",
                                      "EmbedExpr",
                                      "MatchExpr",
                                      "MemberExpr",
                                      "PointerTypeExpr",
                                      "DerefExpr",
                                      "SizeofExpr",
                                      "TryExpr",
                                      "QuoteExpr",
                                      "SpliceExpr",
                                      "ExistsExpr"};
  FILE *out = ctx->out;
  const char *kind = (unsigned)e->kind < sizeof(names) / sizeof(*names)
                         ? names[e->kind]
                         : "UnknownExpr";
  if (e->kind == NY_E_LITERAL) {
    static const char *const literals[] = {"IntegerLiteral", "FloatLiteral",
                                           "BoolLiteral", "StringLiteral"};
    if ((unsigned)e->as.literal.kind < sizeof(literals) / sizeof(*literals))
      kind = literals[e->as.literal.kind];
  }
  fputs(kind, out);
  dump_location(out, e->tok, e->expansion_origin);
  dump_semantic(ctx, &e->semantic);
#define EXPR(ptr, role) dump_push(c, D_EXPR, (ptr), (role))
#define STMT(ptr, role) dump_push(c, D_STMT, (ptr), (role))
  switch (e->kind) {
  case NY_E_IDENT:
    dump_named(out, "name", e->as.ident.name);
    break;
  case NY_E_LITERAL: {
    const literal_t *lit = &e->as.literal;
    fputs(" value=", out);
    switch (lit->kind) {
    case NY_LIT_INT:
      if (lit->numeric_text && lit->numeric_len)
        fwrite(lit->numeric_text, 1, lit->numeric_len, out);
      else
        fprintf(out, "%" PRId64, lit->as.i);
      break;
    case NY_LIT_FLOAT:
      fprintf(out, "%.17g", lit->as.f);
      break;
    case NY_LIT_BOOL:
      fputs(lit->as.b ? "true" : "false", out);
      break;
    case NY_LIT_STR:
      dump_string(out, lit->as.s.data, lit->as.s.len);
      break;
    }
    break;
  }
  case NY_E_UNARY:
    dump_named(out, "op", e->as.unary.op);
    EXPR(e->as.unary.right, "operand");
    break;
  case NY_E_BINARY:
  case NY_E_LOGICAL:
    dump_named(out, "op", e->as.binary.op);
    EXPR(e->as.binary.left, "left");
    EXPR(e->as.binary.right, "right");
    break;
  case NY_E_TERNARY:
    EXPR(e->as.ternary.cond, "condition");
    EXPR(e->as.ternary.true_expr, "then");
    EXPR(e->as.ternary.false_expr, "else");
    break;
  case NY_E_CALL:
    EXPR(e->as.call.callee, "callee");
    for (size_t i = 0; i < e->as.call.args.len; ++i)
      EXPR(e->as.call.args.data[i].val, e->as.call.args.data[i].name);
    break;
  case NY_E_MEMCALL:
    dump_named(out, "name", e->as.memcall.name);
    EXPR(e->as.memcall.target, "receiver");
    for (size_t i = 0; i < e->as.memcall.args.len; ++i)
      EXPR(e->as.memcall.args.data[i].val, e->as.memcall.args.data[i].name);
    break;
  case NY_E_INDEX:
    EXPR(e->as.index.target, "target");
    EXPR(e->as.index.start, "start");
    EXPR(e->as.index.stop, "stop");
    EXPR(e->as.index.step, "step");
    break;
  case NY_E_MEMBER:
    dump_named(out, "name", e->as.member.name);
    EXPR(e->as.member.target, "target");
    EXPR(e->as.member.computed_name, "name");
    break;
  case NY_E_PTR_TYPE:
    EXPR(e->as.ptr_type.target, "target");
    break;
  case NY_E_DEREF:
    EXPR(e->as.deref.target, "target");
    break;
  case NY_E_SIZEOF:
    dump_named(out, "type", e->as.szof.type_name);
    EXPR(e->as.szof.target, "target");
    break;
  case NY_E_TRY:
    EXPR(e->as.try_expr.target, "value");
    break;
  case NY_E_LAMBDA:
  case NY_E_FN:
    dump_named(out, "return", e->as.lambda.return_type);
    dump_params(c, &e->as.lambda.params);
    STMT(e->as.lambda.body, "body");
    break;
  case NY_E_LIST:
  case NY_E_TUPLE:
  case NY_E_SET:
    dump_exprs(c, &e->as.list_like);
    break;
  case NY_E_DICT:
    for (size_t i = 0; i < e->as.dict.pairs.len; ++i) {
      EXPR(e->as.dict.pairs.data[i].key, "key");
      EXPR(e->as.dict.pairs.data[i].value, "value");
    }
    break;
  case NY_E_ASM:
    dump_named(out, "code", e->as.as_asm.code);
    dump_named(out, "constraints", e->as.as_asm.constraints);
    dump_exprs(c, &e->as.as_asm.args);
    break;
  case NY_E_COMPTIME:
    STMT(e->as.comptime_expr.body, "body");
    break;
  case NY_E_FSTRING:
    for (size_t i = 0; i < e->as.fstring.parts.len; ++i) {
      const fstring_part_t *part = &e->as.fstring.parts.data[i];
      if (part->kind == NY_FSP_EXPR)
        EXPR(part->as.e, "interpolation");
      else
        dump_push(c, D_TEXT, part, NULL);
    }
    break;
  case NY_E_INFERRED_MEMBER:
    dump_named(out, "name", e->as.inferred_member.name);
    break;
  case NY_E_EMBED:
    dump_named(out, "path", e->as.embed.path);
    break;
  case NY_E_MATCH:
    dump_match(c, &e->as.match);
    break;
  case NY_E_QUOTE:
    STMT(e->as.quote.body, "body");
    EXPR(e->as.quote.expr, "expression");
    break;
  case NY_E_SPLICE:
    EXPR(e->as.splice.expr, "value");
    break;
  case NY_E_EXISTS:
    dump_named(out, "binding", e->as.exists.var);
    EXPR(e->as.exists.body, "predicate");
    break;
  }
}
static void dump_stmt(dump_context_t *ctx, const stmt_t *s,
                      dump_children_t *c) {
  static const char *const names[] = {
      "BlockStmt",    "UseDecl",      "VarDecl",
      "ExprStmt",     "IfStmt",       "GuardStmt",
      "WhileStmt",    "ForStmt",      "TryStmt",
      "FunctionDecl", "LemmaDecl",    "ExternDecl",
      "LinkDecl",     "ReturnStmt",   "LabelStmt",
      "DeferStmt",    "GotoStmt",     "BreakStmt",
      "ContinueStmt", "LayoutDecl",   "MatchStmt",
      "ModuleDecl",   "ExportDecl",   "StructDecl",
      "EnumDecl",     "TemplateDecl", "IncludeDecl",
      "DefineDecl",   "OperatorDecl", "ImplDecl",
      "RefineDecl",   "TraitDecl",    "CompoundAssignStmt"};
  FILE *out = ctx->out;
  const char *kind = (unsigned)s->kind < sizeof(names) / sizeof(*names)
                         ? names[s->kind]
                         : "UnknownStmt";
  if (s->kind == NY_S_VAR) {
    if (s->as.var.is_del)
      kind = "DeleteStmt";
    else if (!s->as.var.is_decl)
      kind = "AssignStmt";
  }
  fputs(kind, out);
  dump_location(out, s->tok, s->expansion_origin);
  dump_named(out, "name", dump_stmt_name(s));
  switch (s->kind) {
  case NY_S_BLOCK:
    dump_stmts(c, &s->as.block.body);
    break;
  case NY_S_USE:
    dump_named(out, "module", s->as.use.module);
    dump_named(out, "alias", s->as.use.alias);
    dump_named(out, "profile", s->as.use.profile);
    for (size_t i = 0; i < s->as.use.imports.len; ++i) {
      dump_named(out, "import", s->as.use.imports.data[i].name);
      dump_named(out, "as", s->as.use.imports.data[i].alias);
    }
    break;
  case NY_S_VAR:
    if (s->as.var.is_mut)
      fputs(" mutable", out);
    for (size_t i = 0; i < s->as.var.names.len; ++i) {
      dump_named(out, "binding", s->as.var.names.data[i]);
      if (i < s->as.var.types.len)
        dump_named(out, "declared-type", s->as.var.types.data[i]);
    }
    for (size_t i = 0; i < s->as.var.exprs.len; ++i)
      EXPR(s->as.var.exprs.data[i], "initializer");
    break;
  case NY_S_EXPR:
    EXPR(s->as.expr.expr, NULL);
    break;
  case NY_S_AUG:
    dump_named(out, "op", s->as.aug.op);
    EXPR(s->as.aug.target, "target");
    EXPR(s->as.aug.rhs, "value");
    break;
  case NY_S_IF:
    STMT(s->as.iff.init, "init");
    EXPR(s->as.iff.test, "condition");
    STMT(s->as.iff.conseq, "then");
    STMT(s->as.iff.alt, "else");
    break;
  case NY_S_GUARD:
    dump_named(out, "binding", s->as.guard.name);
    dump_named(out, "type", s->as.guard.type_name);
    EXPR(s->as.guard.value, "value");
    STMT(s->as.guard.fallback, "fallback");
    break;
  case NY_S_WHILE:
    STMT(s->as.whl.init, "init");
    EXPR(s->as.whl.test, "condition");
    EXPR(s->as.whl.invariant, "invariant");
    STMT(s->as.whl.body, "body");
    STMT(s->as.whl.update, "update");
    break;
  case NY_S_FOR:
    dump_named(out, "binding", s->as.fr.iter_var);
    dump_named(out, "index", s->as.fr.iter_index_var);
    STMT(s->as.fr.init, "init");
    EXPR(s->as.fr.cond, "condition");
    EXPR(s->as.fr.iterable, "iterable");
    STMT(s->as.fr.body, "body");
    STMT(s->as.fr.update, "update");
    break;
  case NY_S_TRY:
    dump_named(out, "error", s->as.tr.err);
    STMT(s->as.tr.body, "body");
    STMT(s->as.tr.handler, "handler");
    break;
  case NY_S_FUNC:
    dump_named(out, "return", s->as.fn.return_type);
    dump_semantic(ctx, &s->as.fn.return_semantic);
    dump_params(c, &s->as.fn.params);
    EXPR(s->as.fn.requires_cond, "requires");
    EXPR(s->as.fn.decreases_expr, "decreases");
    STMT(s->as.fn.body, "body");
    break;
  case NY_S_LEMMA:
    dump_params(c, &s->as.lemma.params);
    EXPR(s->as.lemma.proposition, "proposition");
    break;
  case NY_S_EXTERN:
    dump_named(out, "return", s->as.ext.return_type);
    dump_named(out, "link-name", s->as.ext.link_name);
    dump_params(c, &s->as.ext.params);
    break;
  case NY_S_RETURN:
    EXPR(s->as.ret.value, "value");
    break;
  case NY_S_DEFER:
    STMT(s->as.de.body, "body");
    break;
  case NY_S_LINK:
    dump_named(out, "library", s->as.link.lib);
    break;
  case NY_S_LAYOUT:
    fprintf(out, " pack=%zu align=%zu", s->as.layout.pack,
            s->as.layout.align_override);
    dump_params(c, &s->as.layout.deftype_params);
    for (size_t i = 0; i < s->as.layout.fields.len; ++i)
      dump_push(c, D_FIELD, &s->as.layout.fields.data[i], NULL);
    dump_stmts(c, &s->as.layout.methods);
    break;
  case NY_S_STRUCT:
    fprintf(out, " pack=%zu align=%zu", s->as.struc.pack,
            s->as.struc.align_override);
    for (size_t i = 0; i < s->as.struc.fields.len; ++i)
      dump_push(c, D_FIELD, &s->as.struc.fields.data[i], NULL);
    dump_stmts(c, &s->as.struc.methods);
    break;
  case NY_S_ENUM:
    for (size_t i = 0; i < s->as.enu.type_params.len; ++i)
      dump_named(out, "type-parameter", s->as.enu.type_params.data[i]);
    dump_named(out, "index-parameter", s->as.enu.index_param_name);
    for (size_t i = 0; i < s->as.enu.items.len; ++i)
      dump_push(c, D_VARIANT, &s->as.enu.items.data[i], NULL);
    break;
  case NY_S_MODULE:
    dump_stmts(c, &s->as.module.body);
    break;
  case NY_S_EXPORT:
    dump_named(out, "profile", s->as.exprt.profile);
    for (size_t i = 0; i < s->as.exprt.names.len; ++i)
      dump_named(out, "export", s->as.exprt.names.data[i]);
    break;
  case NY_S_MACRO:
    dump_exprs(c, &s->as.macro.args);
    STMT(s->as.macro.body, "body");
    break;
  case NY_S_INCLUDE:
    dump_named(out, "path", s->as.inc.path);
    dump_named(out, "prefix", s->as.inc.prefix);
    dump_named(out, "library", s->as.inc.lib);
    break;
  case NY_S_DEFINE:
    dump_named(out, "value", s->as.def.value);
    break;
  case NY_S_OPERATOR:
    dump_named(out, "op", s->as.oper.op);
    dump_named(out, "left", s->as.oper.left_type);
    dump_named(out, "right", s->as.oper.right_type);
    dump_named(out, "return", s->as.oper.return_type);
    break;
  case NY_S_IMPL:
    dump_named(out, "trait", s->as.impl.trait_name);
    dump_stmts(c, &s->as.impl.methods);
    dump_exprs(c, &s->as.impl.laws);
    break;
  case NY_S_REFINE:
    dump_named(out, "base", s->as.refine.base_type);
    dump_named(out, "binding", s->as.refine.param);
    EXPR(s->as.refine.predicate, "predicate");
    break;
  case NY_S_TRAIT:
    dump_stmts(c, &s->as.trait.methods);
    break;
  case NY_S_LABEL:
  case NY_S_GOTO:
  case NY_S_BREAK:
  case NY_S_CONTINUE:
    break;
  case NY_S_MATCH:
    dump_match(c, &s->as.match);
    break;
  }
  for (size_t i = 0; i < s->attributes.len; ++i)
    dump_push(c, D_ATTRIBUTE, &s->attributes.data[i], NULL);
}
static void dump_node(dump_context_t *ctx, dump_node_t node,
                      const dump_frame_t *parent, bool last) {
  dump_frame_t frame = {parent, last};
  dump_children_t children = {.ctx = ctx, .parent = &frame};
  FILE *out = ctx->out;
  dump_indent(out, parent);
  fputs(last ? "`-" : "|-", out);
  if (node.role)
    fprintf(out, "%s: ", node.role);
  switch (node.kind) {
  case D_STMT:
    dump_stmt(ctx, node.ptr, &children);
    break;
  case D_EXPR:
    dump_expr(ctx, node.ptr, &children);
    break;
  case D_PARAM: {
    const param_t *p = node.ptr;
    fputs("ParmDecl", out);
    dump_location(out, p->tok, p->expansion_origin);
    dump_named(out, "name", p->name);
    dump_named(out, "declared-type", p->type);
    dump_semantic(ctx, &p->semantic);
    dump_push(&children, D_EXPR, p->def, "default");
    break;
  }
  case D_FIELD: {
    const layout_field_t *f = node.ptr;
    fputs("FieldDecl", out);
    dump_location(out, f->tok, f->expansion_origin);
    dump_named(out, "name", f->name);
    dump_named(out, "type", f->type_name);
    if (f->width)
      fprintf(out, " align=%d", f->width);
    dump_push(&children, D_EXPR, f->array_len, "extent");
    dump_push(&children, D_EXPR, f->default_value, "default");
    break;
  }
  case D_VARIANT: {
    const stmt_enum_item_t *item = node.ptr;
    fputs("EnumVariantDecl", out);
    dump_location(out, item->tok, item->expansion_origin);
    dump_named(out, "name", item->name);
    for (size_t i = 0; i < item->fields.len; ++i)
      dump_push(&children, D_ENUM_FIELD, &item->fields.data[i], NULL);
    dump_push(&children, D_EXPR, item->value, "value");
    break;
  }
  case D_ENUM_FIELD: {
    const enum_field_t *f = node.ptr;
    fputs("PayloadDecl", out);
    dump_location(out, f->tok, f->expansion_origin);
    dump_named(out, "name", f->name);
    dump_named(out, "type", f->type_name);
    break;
  }
  case D_ATTRIBUTE: {
    const attribute_t *a = node.ptr;
    fputs("Attribute", out);
    dump_location(out, a->tok, NULL);
    dump_named(out, "name", a->name);
    dump_exprs(&children, &a->args);
    break;
  }
  case D_ARM: {
    const match_arm_t *arm = node.ptr;
    fputs("MatchArm", out);
    for (size_t i = 0; i < arm->patterns.len; ++i)
      dump_push(&children, D_EXPR, arm->patterns.data[i], "pattern");
    dump_push(&children, D_EXPR, arm->guard, "guard");
    dump_push(&children, D_STMT, arm->conseq, "body");
    break;
  }
  case D_TEXT: {
    const fstring_part_t *p = node.ptr;
    fputs("StringPart ", out);
    dump_string(out, p->as.s.data, p->as.s.len);
    break;
  }
  }
  if (!children.started)
    fputc('\n', out);
  dump_finish(&children);
}
#undef EXPR
#undef STMT

static bool dump_in_file(token_t tok, const char *file) {
  if (!file || !*file)
    return true;
  if (!tok.filename)
    return false;
  size_t len = strlen(file);
  return strcmp(tok.filename, file) == 0 ||
         (strncmp(tok.filename, file, len) == 0 && tok.filename[len] == ':');
}
static void dump_select(dump_children_t *roots, const stmt_t *s,
                        const char *file, const char *filter) {
  if (!s || !dump_in_file(s->tok, file))
    return;
  bool match = !filter || !*filter;
  const char *name = dump_stmt_name(s);
  if (!match && name && s->kind != NY_S_GOTO && s->kind != NY_S_LABEL)
    match = strstr(name, filter) != NULL;
  if (!match && s->kind == NY_S_VAR && s->as.var.is_decl)
    for (size_t i = 0; i < s->as.var.names.len; ++i)
      if (strstr(s->as.var.names.data[i], filter)) {
        match = true;
        break;
      }
  if (match) {
    dump_push(roots, D_STMT, s, NULL);
    return;
  }
  const ny_stmt_list *nested = NULL;
  switch (s->kind) {
  case NY_S_MODULE:
    nested = &s->as.module.body;
    break;
  case NY_S_BLOCK:
    nested = &s->as.block.body;
    break;
  case NY_S_IMPL:
    nested = &s->as.impl.methods;
    break;
  case NY_S_LAYOUT:
    nested = &s->as.layout.methods;
    break;
  case NY_S_STRUCT:
    nested = &s->as.struc.methods;
    break;
  case NY_S_TRAIT:
    nested = &s->as.trait.methods;
    break;
  case NY_S_FUNC:
    dump_select(roots, s->as.fn.body, file, filter);
    break;
  default:
    break;
  }
  if (nested)
    for (size_t i = 0; i < nested->len; ++i)
      dump_select(roots, nested->data[i], file, filter);
}
void ny_ast_dump_tree(FILE *out, const program_t *prog, const char *filename,
                      const char *filter, bool typed) {
  dump_context_t ctx = {out, typed};
  dump_frame_t root = {NULL, true};
  dump_children_t children = {.ctx = &ctx, .parent = &root, .started = true};
  fprintf(out, "%s <%s>\n", typed ? "TypedProgram" : "Program",
          filename ? filename : "all sources");
  if (prog)
    for (size_t i = 0; i < prog->body.len; ++i)
      dump_select(&children, prog->body.data[i], filename, filter);
  dump_finish(&children);
}
