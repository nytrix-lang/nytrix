#ifndef NY_AST_H
#define NY_AST_H

#include "base/common.h"
#include "code/types.h"
#include "lex/lexer.h"

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

typedef enum lit_kind_t {
  NY_LIT_INT,
  NY_LIT_FLOAT,
  NY_LIT_BOOL,
  NY_LIT_STR
} lit_kind_t;

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
  NY_LIT_HINT_F32,
  NY_LIT_HINT_F64,
  NY_LIT_HINT_F128
} lit_type_hint_t;

typedef struct expr_t expr_t;
typedef struct stmt_t stmt_t;

typedef enum fstring_part_kind_t {
  NY_FSP_STR,
  NY_FSP_EXPR
} fstring_part_kind_t;
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
  stmt_t *conseq;
} match_arm_t;
typedef struct param_t {
  const char *name;
  const char *type; // Type constraint (optional)
  expr_t *def;      // optional default
} param_t;

typedef VEC(param_t) ny_param_list;
typedef VEC(expr_t *) ny_expr_list;
typedef VEC(match_arm_t) ny_match_arm_list;

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
  const char *name; // NULL if positional
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
      stmt_t *body; // block stmt_t
      bool is_variadic;
    } lambda;
    ny_expr_list list_like; // for list/tuple/set
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

typedef struct stmt_enum_item_t {
  const char *name;
  expr_t *value; // Optional explicit value
} stmt_enum_item_t;

typedef VEC(stmt_enum_item_t) ny_stmt_enum_item_list;

typedef struct stmt_enum_t {
  const char *name;
  ny_stmt_enum_item_list items;
} stmt_enum_t;

typedef enum stmt_kind_t {
  NY_S_BLOCK,
  NY_S_USE,
  NY_S_VAR,
  NY_S_EXPR,
  NY_S_IF,
  NY_S_WHILE,
  NY_S_FOR,
  NY_S_TRY,
  NY_S_FUNC,
  NY_S_EXTERN,
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
} stmt_kind_t;

typedef struct stmt_export_t {
  VEC(const char *) names;
} stmt_export_t;

typedef struct stmt_defer_t {
  struct stmt_t *body;
} stmt_defer_t;

typedef VEC(stmt_t *) ny_stmt_list;

typedef struct stmt_block_t {
  ny_stmt_list body;
} stmt_block_t;

typedef struct stmt_var_t {
  VEC(const char *) names;
  VEC(expr_t *) exprs;
  VEC(const char *) types; // Add types field
  bool is_decl;
  bool is_mut;
  bool is_undef;
  bool is_destructure;
} stmt_var_t;

typedef struct stmt_if_t {
  expr_t *test;
  stmt_t *conseq; // block
  stmt_t *alt;    // block optional
} stmt_if_t;

typedef struct stmt_while_t {
  expr_t *test;
  stmt_t *body; // block
} stmt_while_t;

typedef struct stmt_for_t {
  const char *iter_var;
  expr_t *iterable;
  stmt_t *body; // block
} stmt_for_t;

typedef struct stmt_try_t {
  stmt_t *body;    // block
  const char *err; // may be NULL
  stmt_t *handler; // block
} stmt_try_t;

typedef struct stmt_func_t {
  const char *name;
  const char *return_type; // Optional return type
  ny_param_list params;
  stmt_t *body;    // block
  const char *doc; // optional docstring
  bool is_variadic;
  const char *src_start;
  const char *src_end;
} stmt_func_t;

typedef struct stmt_extern_t {
  const char *name;
  const char *return_type;
  ny_param_list params;
  const char *link_name;
  bool is_variadic;
} stmt_extern_t;

typedef struct stmt_return_t {
  expr_t *value; // optional
} stmt_return_t;

typedef struct stmt_label_t {
  const char *name;
} stmt_label_t;
typedef struct stmt_goto_t {
  const char *name;
} stmt_goto_t;

typedef struct layout_field_t {
  const char *name;
  const char *type_name; // e.g. "u32"
  int width;             // Optional explicit alignment in bytes
} layout_field_t;

typedef VEC(layout_field_t) ny_layout_field_list;

typedef struct stmt_layout_t {
  const char *name;
  ny_layout_field_list fields;
  size_t align_override;
  size_t pack;
} stmt_layout_t;

typedef struct stmt_struct_t {
  const char *name;
  ny_layout_field_list fields;
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
  void *sema; // Add sema field (generic pointer for semantic info)
  union {
    stmt_block_t block;
    struct {
      const char *module;
      const char *alias;
      bool is_local;
      bool import_all;
      ny_use_item_list imports;
    } use;
    stmt_var_t var;
    struct {
      expr_t *expr;
    } expr;
    stmt_if_t iff;
    stmt_while_t whl;
    stmt_for_t fr;
    stmt_try_t tr;
    stmt_func_t fn;
    stmt_extern_t ext;
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
  } as;
};

typedef struct program_t {
  ny_stmt_list body;
  const char *doc; // optional module docstring
} program_t;

expr_t *expr_new(arena_t *arena_t, expr_kind_t kind, token_t tok);
stmt_t *stmt_new(arena_t *arena_t, stmt_kind_t kind, token_t tok);
void expr_free_members(expr_t *e);
void stmt_free_members(stmt_t *s);
void program_free(program_t *prog, arena_t *arena_t);

#endif
