#ifndef NY_AST_H
#define NY_AST_H

#include "base/common.h"
#include "code/types.h"
#include "parse/lexer.h"

typedef enum expr_kind_t {
  NY_E_IDENT,
  NY_E_LITERAL,
  NY_E_UNARY,
  NY_E_BINARY,
  NY_E_LOGICAL,
  NY_E_TERNARY,
  NY_E_CALL,
  NY_E_MEMCALL,
  NY_E_INDEX,
  NY_E_LAMBDA,
  NY_E_FN,
  NY_E_LIST,
  NY_E_TUPLE,
  NY_E_DICT,
  NY_E_SET,
  NY_E_ASM,
  NY_E_COMPTIME,
  NY_E_FSTRING,
  NY_E_INFERRED_MEMBER,
  NY_E_EMBED,
  NY_E_MATCH,
  NY_E_MEMBER,
  NY_E_PTR_TYPE,
  NY_E_DEREF,
  NY_E_SIZEOF,
  NY_E_TRY,
} expr_kind_t;

typedef enum lit_kind_t { NY_LIT_INT, NY_LIT_FLOAT, NY_LIT_BOOL, NY_LIT_STR } lit_kind_t;

typedef enum lit_type_hint_t {
  NY_LIT_HINT_NONE = 0,
  NY_LIT_HINT_I8,
  NY_LIT_HINT_I16,
  NY_LIT_HINT_I32,
  NY_LIT_HINT_I64,
  NY_LIT_HINT_U8,
  NY_LIT_HINT_U16,
  NY_LIT_HINT_U32,
  NY_LIT_HINT_U64,
  NY_LIT_HINT_I128,
  NY_LIT_HINT_U128,
  NY_LIT_HINT_F32,
  NY_LIT_HINT_F64,
  NY_LIT_HINT_F128
} lit_type_hint_t;

typedef struct expr_t expr_t;
typedef struct stmt_t stmt_t;

typedef enum fstring_part_kind_t { NY_FSP_STR, NY_FSP_EXPR } fstring_part_kind_t;
typedef struct fstring_part_t {
  fstring_part_kind_t kind;
  union {
    struct {
      const char *data;
      size_t len;
    } s;
    expr_t *e;
  } as;
} fstring_part_t;
typedef VEC(fstring_part_t) ny_fstring_part_list;

typedef struct match_arm_t {
  VEC(struct expr_t *) patterns;
  struct expr_t *guard;
  stmt_t *conseq;
} match_arm_t;
typedef struct param_t {
  const char *name;
  const char *type;
  expr_t *def;
} param_t;

typedef VEC(param_t) ny_param_list;
typedef VEC(expr_t *) ny_expr_list;
typedef VEC(match_arm_t) ny_match_arm_list;

typedef struct attribute_t {
  const char *name;
  token_t tok;
  ny_expr_list args;
} attribute_t;
typedef VEC(attribute_t) ny_attribute_list;

typedef struct stmt_match_t {
  expr_t *test;
  ny_match_arm_list arms;
  stmt_t *default_conseq;
} stmt_match_t;

typedef struct dict_pair_t {
  expr_t *key;
  expr_t *value;
} dict_pair_t;

typedef struct call_arg_t {
  const char *name;
  expr_t *val;
} call_arg_t;

typedef VEC(call_arg_t) ny_call_arg_list;

typedef struct literal_t {
  lit_kind_t kind;
  lit_type_hint_t hint;
  bool hint_explicit;
  union {
    int64_t i;
    double f;
    bool b;
    struct {
      const char *data;
      size_t len;
    } s;
  } as;
} literal_t;

typedef struct expr_call_t {
  expr_t *callee;
  ny_call_arg_list args;
} expr_call_t;

typedef struct expr_memcall_t {
  expr_t *target;
  const char *name;
  ny_call_arg_list args;
} expr_memcall_t;

typedef struct expr_index_t {
  expr_t *target;
  expr_t *start;
  expr_t *stop;
  expr_t *step;
} expr_index_t;

typedef struct expr_member_t {
  expr_t *target;
  const char *name;
} expr_member_t;

typedef struct expr_ptr_type_t {
  expr_t *target;
} expr_ptr_type_t;

typedef struct expr_deref_t {
  expr_t *target;
} expr_deref_t;

typedef struct expr_sizeof_t {
  expr_t *target;
  const char *type_name;
  bool is_type;
} expr_sizeof_t;

typedef struct expr_try_t {
  expr_t *target;
} expr_try_t;

struct expr_t {
  expr_kind_t kind;
  token_t tok;
  union {
    struct {
      const char *name;
      ny_sym_id sym_id;
      uint64_t hash;
    } ident;
    literal_t literal;
    struct {
      const char *op;
      expr_t *right;
    } unary;
    struct {
      const char *op;
      expr_t *left;
      expr_t *right;
    } binary;
    struct {
      const char *op;
      expr_t *left;
      expr_t *right;
    } logical;
    struct {
      expr_t *cond;
      expr_t *true_expr;
      expr_t *false_expr;
    } ternary;
    expr_call_t call;
    expr_memcall_t memcall;
    expr_index_t index;
    expr_member_t member;
    expr_ptr_type_t ptr_type;
    expr_deref_t deref;
    expr_sizeof_t szof;
    expr_try_t try_expr;
    struct {
      const char *return_type;
      ny_param_list params;
      stmt_t *body;
      bool is_variadic;
    } lambda;
    ny_expr_list list_like;
    struct {
      VEC(dict_pair_t) pairs;
    } dict;
    struct {
      const char *code;
      const char *constraints;
      ny_expr_list args;
    } as_asm;
    struct {
      struct stmt_t *body;
    } comptime_expr;
    struct {
      ny_fstring_part_list parts;
    } fstring;
    struct {
      const char *name;
    } inferred_member;
    struct {
      const char *path;
    } embed;
    stmt_match_t match;
  } as;
};

typedef struct enum_field_t {
  const char *name;
  const char *type_name;
} enum_field_t;

typedef VEC(enum_field_t) ny_enum_field_list;
typedef VEC(const char *) ny_type_param_list;

typedef struct stmt_enum_item_t {
  const char *name;
  expr_t *value;
  ny_enum_field_list fields;
} stmt_enum_item_t;

typedef VEC(stmt_enum_item_t) ny_stmt_enum_item_list;

typedef struct stmt_enum_t {
  const char *name;
  ny_type_param_list type_params;
  ny_stmt_enum_item_list items;
} stmt_enum_t;

typedef enum stmt_kind_t {
  NY_S_BLOCK,
  NY_S_USE,
  NY_S_VAR,
  NY_S_EXPR,
  NY_S_IF,
  NY_S_GUARD,
  NY_S_WHILE,
  NY_S_FOR,
  NY_S_TRY,
  NY_S_FUNC,
  NY_S_EXTERN,
  NY_S_LINK,
  NY_S_RETURN,
  NY_S_LABEL,
  NY_S_DEFER,
  NY_S_GOTO,
  NY_S_BREAK,
  NY_S_CONTINUE,
  NY_S_LAYOUT,
  NY_S_MATCH,
  NY_S_MODULE,
  NY_S_EXPORT,
  NY_S_STRUCT,
  NY_S_ENUM,
  NY_S_MACRO,
  NY_S_INCLUDE,
  NY_S_DEFINE,
  NY_S_OPERATOR,
  NY_S_IMPL,
} stmt_kind_t;

typedef struct stmt_export_t {
  VEC(const char *) names;
  const char *profile;
  bool is_internal;
} stmt_export_t;

typedef struct stmt_defer_t {
  struct stmt_t *body;
} stmt_defer_t;

typedef VEC(stmt_t *) ny_stmt_list;

typedef struct stmt_block_t {
  ny_stmt_list body;
  bool transparent;
} stmt_block_t;

typedef struct stmt_var_t {
  VEC(const char *) names;
  VEC(expr_t *) exprs;
  VEC(const char *) types;
  bool is_decl;
  bool is_mut;
  bool is_del;
  bool is_destructure;
} stmt_var_t;

typedef struct stmt_if_t {
  expr_t *test;
  stmt_t *conseq;
  stmt_t *alt;
  stmt_t *init;
} stmt_if_t;

typedef struct stmt_guard_t {
  const char *type_name;
  const char *name;
  expr_t *value;
  stmt_t *fallback;
} stmt_guard_t;

typedef struct stmt_while_t {
  expr_t *test;
  stmt_t *body;
  stmt_t *update;
  stmt_t *init;
  bool attr_unroll;
  bool attr_vectorize;
  bool attr_nounroll;
} stmt_while_t;

typedef struct stmt_for_t {
  const char *iter_var; /* iterator-style: loop variable name */
  const char *iter_index_var; /* optional iterator-style index name */
  expr_t *iterable;     /* iterator-style: iterable expression */
  bool iter_by_index;   /* iterator-style: bind index instead of element */
  stmt_t *body;         /* both styles: loop body */
  /* C-style fields (NULL for iterator-style): */
  stmt_t *init;   /* C-style: initialization statement */
  expr_t *cond;   /* C-style: condition expression */
  stmt_t *update; /* C-style: update statement (++i, etc.) */
  bool attr_unroll;
  bool attr_vectorize;
  bool attr_nounroll;
} stmt_for_t;

typedef struct stmt_try_t {
  stmt_t *body;
  const char *err;
  stmt_t *handler;
} stmt_try_t;

typedef struct stmt_func_t {
  const char *name;
  const char *return_type;
  ny_param_list params;
  stmt_t *body;
  const char *doc;
  bool is_variadic;
  const char *src_start;
  const char *src_end;
  bool attr_naked;
  bool attr_jit;
  bool attr_thread;
  bool attr_async_effects;
  bool attr_pure;
  bool attr_cache;
  bool attr_inline;
  bool attr_noinline;
  bool attr_readnone;
  bool attr_readonly;
  bool attr_writeonly;
  bool attr_argmemonly;
  bool attr_nounwind;
  bool attr_mustprogress;
  bool attr_willreturn;
  bool attr_cold;
  bool attr_hot;
  bool attr_flatten;
  bool attr_tailcall;
  bool attr_sys;
  bool attr_nogc;
  bool attr_consteval;
  bool attr_constant_time;
  bool attr_accel;
  const char *attr_accel_target;
  bool attr_returns_owned;
  const char *attr_returns_borrow;
  ny_str_list attr_borrows;
  ny_str_list attr_consumes;
  ny_str_list attr_mutates;
  ny_str_list attr_releases;
  ny_str_list attr_forgets;
  bool is_extern;
  const char *link_name;
  bool attrs_resolved;
  bool effect_contract_known;
  uint32_t effect_contract_mask;
  bool body_summary_known;
  bool body_has_try;
  bool body_has_label_or_goto;
} stmt_func_t;

typedef struct stmt_extern_t {
  const char *name;
  const char *return_type;
  ny_param_list params;
  const char *link_name;
  bool is_variadic;
} stmt_extern_t;

typedef struct stmt_include_t {
  const char *path;
  const char *prefix;
  const char *lib;
  bool is_std;
} stmt_include_t;

typedef struct stmt_define_t {
  const char *name;
  const char *value;
} stmt_define_t;

typedef struct stmt_operator_t {
  const char *op;
  const char *left_type;
  const char *right_type;
  const char *return_type;
  const char *target;
} stmt_operator_t;

typedef struct stmt_impl_t {
  const char *type_name;
  ny_stmt_list methods;
} stmt_impl_t;

typedef struct stmt_return_t {
  expr_t *value;
} stmt_return_t;

typedef struct stmt_label_t {
  const char *name;
} stmt_label_t;
typedef struct stmt_goto_t {
  const char *name;
} stmt_goto_t;

typedef enum stmt_sema_kind_t {
  NY_STMT_SEMA_NONE = 0,
  NY_STMT_SEMA_LAYOUT,
  NY_STMT_SEMA_FUNC,
  NY_STMT_SEMA_VAR,
  NY_STMT_SEMA_ENUM,
} stmt_sema_kind_t;

typedef struct layout_field_t {
  const char *name;
  const char *type_name;
  int width;
  expr_t *default_value;
  const char *default_src;
} layout_field_t;

typedef VEC(layout_field_t) ny_layout_field_list;

typedef struct stmt_layout_t {
  const char *name;
  ny_layout_field_list fields;
  ny_stmt_list methods;
  size_t align_override;
  size_t pack;
  const char *flavor;
} stmt_layout_t;

typedef struct stmt_struct_t {
  const char *name;
  ny_layout_field_list fields;
  ny_stmt_list methods;
  size_t align_override;
  size_t pack;
} stmt_struct_t;

typedef struct use_item_t {
  const char *name;
  const char *alias;
} use_item_t;

typedef VEC(use_item_t) ny_use_item_list;

struct stmt_t {
  stmt_kind_t kind;
  token_t tok;
  void *sema;
  stmt_sema_kind_t sema_kind;
  ny_attribute_list attributes;
  union {
    stmt_block_t block;
    struct {
      const char *module;
      const char *alias;
      const char *profile;
      bool is_local;
      bool import_all;
      ny_use_item_list imports;
    } use;
    stmt_var_t var;
    struct {
      expr_t *expr;
    } expr;
    stmt_if_t iff;
    stmt_guard_t guard;
    stmt_while_t whl;
    stmt_for_t fr;
    stmt_try_t tr;
    stmt_func_t fn;
    stmt_extern_t ext;
    struct {
      const char *lib;
    } link;
    stmt_return_t ret;
    stmt_label_t label;
    stmt_goto_t go;
    stmt_defer_t de;
    stmt_layout_t layout;
    stmt_struct_t struc;
    stmt_match_t match;
    struct {
      const char *name;
      ny_stmt_list body;
      bool export_all;
      const char *src_start;
      const char *src_end;
      const char *path;
    } module;
    stmt_export_t exprt;
    stmt_enum_t enu;
    struct {
      const char *name;
      ny_expr_list args;
      stmt_t *body;
    } macro;
    stmt_include_t inc;
    stmt_define_t def;
    stmt_operator_t oper;
    stmt_impl_t impl;
  } as;
};

typedef struct ny_diag_rule_t {
  const char *name;
  const char *call_name;
  int arg_index;
  bool reject_non_literal;
  const char *message;
  const char *fix;
} ny_diag_rule_t;
typedef VEC(ny_diag_rule_t) ny_diag_rule_list;

typedef struct program_t {
  ny_stmt_list body;
  ny_diag_rule_list diagnostic_rules;
  const char *doc;
  const char *raw_src; /* Original source code for -c inline compilation */
  size_t raw_src_len;
} program_t;

static inline bool ny_expr_ident_is_name(const expr_t *e, const char *name) {
  return e && e->kind == NY_E_IDENT && e->as.ident.name && name &&
         strcmp(e->as.ident.name, name) == 0;
}

static inline bool ny_expr_is_wildcard_ident(const expr_t *e) {
  return ny_expr_ident_is_name(e, "_");
}

static inline bool ny_expr_is_nil_literal(const expr_t *e) {
  return e && e->kind == NY_E_LITERAL && e->as.literal.kind == NY_LIT_INT &&
         e->tok.kind == NY_T_NIL;
}

static inline bool ny_expr_is_zero_arg_call_named(const expr_t *e, const char *name) {
  return e && e->kind == NY_E_CALL &&
         ny_expr_ident_is_name(e->as.call.callee, name) &&
         e->as.call.args.len == 0;
}

static inline bool ny_program_has_top_zero_arg_call_named(const program_t *prog,
                                                          const char *name) {
  if (!prog)
    return false;
  for (size_t i = 0; i < prog->body.len; ++i) {
    stmt_t *stmt = prog->body.data[i];
    if (stmt && stmt->kind == NY_S_EXPR &&
        ny_expr_is_zero_arg_call_named(stmt->as.expr.expr, name))
      return true;
  }
  return false;
}

expr_t *expr_new(arena_t *arena_t, expr_kind_t kind, token_t tok);
stmt_t *stmt_new(arena_t *arena_t, stmt_kind_t kind, token_t tok);
void expr_free_members(expr_t *e);
void stmt_free_members(stmt_t *s);
void program_free(program_t *prog, arena_t *arena_t);
void ny_ast_verify_program(program_t *prog, const char *phase);

#endif
