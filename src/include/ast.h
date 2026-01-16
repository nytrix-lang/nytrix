#ifndef NT_AST_H
#define NT_AST_H

#include "common.h"
#include "lexer.h"

typedef enum nt_expr_kind {
	NT_E_IDENT,
	NT_E_LITERAL,
	NT_E_UNARY,
	NT_E_BINARY,
	NT_E_LOGICAL,
	NT_E_TERNARY,
	NT_E_CALL,
	NT_E_MEMCALL,
	NT_E_INDEX,
	NT_E_LAMBDA,
	NT_E_FN,
	NT_E_LIST,
	NT_E_TUPLE,
	NT_E_DICT,
	NT_E_SET,
	NT_E_ASM,
	NT_E_COMPTIME,
	NT_E_FSTRING,
	NT_E_INFERRED_MEMBER,
	NT_E_EMBED,
	NT_E_MATCH,
} nt_expr_kind;

typedef enum nt_lit_kind { NT_LIT_INT, NT_LIT_FLOAT, NT_LIT_BOOL, NT_LIT_STR } nt_lit_kind;

typedef struct nt_expr nt_expr;
typedef struct nt_stmt nt_stmt;

typedef enum nt_fstring_part_kind { NT_FSP_STR, NT_FSP_EXPR } nt_fstring_part_kind;
typedef struct nt_fstring_part {
	nt_fstring_part_kind kind;
	union {
		struct { const char *data; size_t len; } s;
		nt_expr *e;
	} as;
} nt_fstring_part;
typedef NT_VEC(nt_fstring_part) nt_fstring_part_list;

typedef struct nt_match_arm {
	NT_VEC(struct nt_expr *) patterns;
	nt_stmt *conseq;
} nt_match_arm;
typedef struct nt_param {
	const char *name;
	const char *type; // Type constraint (optional)
	nt_expr *def;     // optional default
} nt_param;

typedef NT_VEC(nt_param) nt_param_list;
typedef NT_VEC(nt_expr *) nt_expr_list;
typedef NT_VEC(nt_match_arm) nt_match_arm_list;

typedef struct nt_stmt_match {
	nt_expr *test;
	nt_match_arm_list arms;
	nt_stmt *default_conseq;
} nt_stmt_match;

typedef struct nt_dict_pair { nt_expr *key; nt_expr *value; } nt_dict_pair;

typedef struct nt_call_arg {
	const char *name; // NULL if positional
	nt_expr *val;
} nt_call_arg;

typedef NT_VEC(nt_call_arg) nt_call_arg_list;

typedef struct nt_literal {
	nt_lit_kind kind;
	union {
		int64_t i;
		double f;
		bool b;
		struct {
			const char *data;
			size_t len;
		} s;
	} as;
} nt_literal;

typedef struct nt_expr_call {
	nt_expr *callee;
	nt_call_arg_list args;
} nt_expr_call;

typedef struct nt_expr_memcall {
	nt_expr *target;
	const char *name;
	nt_call_arg_list args;
} nt_expr_memcall;

typedef struct nt_expr_index {
	nt_expr *target;
	nt_expr *start;
	nt_expr *stop;
	nt_expr *step;
} nt_expr_index;

struct nt_expr {
	nt_expr_kind kind;
	nt_token tok;
	union {
		struct {
			const char *name;
		} ident;
		nt_literal literal;
		struct {
			const char *op;
			nt_expr *right;
		} unary;
		struct {
			const char *op;
			nt_expr *left;
			nt_expr *right;
		} binary;
		struct {
			const char *op;
			nt_expr *left;
			nt_expr *right;
		} logical;
		struct {
			nt_expr *cond;
			nt_expr *true_expr;
			nt_expr *false_expr;
		} ternary;
		nt_expr_call call;
		nt_expr_memcall memcall;
		nt_expr_index index;
		struct {
			const char *return_type;
			nt_param_list params;
			nt_stmt *body; // block stmt
			bool is_variadic;
		} lambda;
		nt_expr_list list_like; // for list/tuple/set
		struct {
			NT_VEC(nt_dict_pair) pairs;
		} dict;
		struct {
			const char *code;
			const char *constraints;
			nt_expr_list args;
		} as_asm;
		struct {
			struct nt_stmt *body;
		} comptime_expr;
		struct {
			nt_fstring_part_list parts;
		} fstring;
		struct {
			const char *name;
		} inferred_member;
		struct {
			const char *path;
		} embed;
		nt_stmt_match match;
	} as;
};

typedef enum nt_stmt_kind {
	NT_S_BLOCK,
	NT_S_USE,
	NT_S_VAR,
	NT_S_EXPR,
	NT_S_IF,
	NT_S_WHILE,
	NT_S_FOR,
	NT_S_TRY,
	NT_S_FUNC,
	NT_S_RETURN,
	NT_S_LABEL,
	NT_S_DEFER,
	NT_S_GOTO,
	NT_S_BREAK,
	NT_S_CONTINUE,
	NT_S_LAYOUT,
	NT_S_MATCH,
	NT_S_MODULE,
	NT_S_EXPORT,
} nt_stmt_kind;

typedef struct nt_stmt_export {
	NT_VEC(const char *) names;
} nt_stmt_export;

typedef struct nt_stmt_defer {
	struct nt_stmt *body;
} nt_stmt_defer;

typedef NT_VEC(nt_stmt *) nt_stmt_list;

typedef struct nt_stmt_block {
	nt_stmt_list body;
} nt_stmt_block;

typedef struct nt_stmt_var {
	NT_VEC(const char *) names;
	nt_expr *expr;
	bool is_decl;
	bool is_undef;
} nt_stmt_var;

typedef struct nt_stmt_if {
	nt_expr *test;
	nt_stmt *conseq; // block
	nt_stmt *alt;    // block optional
} nt_stmt_if;

typedef struct nt_stmt_while {
	nt_expr *test;
	nt_stmt *body; // block
} nt_stmt_while;

typedef struct nt_stmt_for {
	const char *iter_var;
	nt_expr *iterable;
	nt_stmt *body; // block
} nt_stmt_for;

typedef struct nt_stmt_try {
	nt_stmt *body;    // block
	const char *err;  // may be NULL
	nt_stmt *handler; // block
} nt_stmt_try;

typedef struct nt_stmt_func {
	const char *name;
	const char *return_type; // Optional return type
	nt_param_list params;
	nt_stmt *body; // block
	const char *doc; // optional docstring
	bool is_variadic;
	const char *src_start;
	const char *src_end;
} nt_stmt_func;

typedef struct nt_stmt_return {
	nt_expr *value; // optional
} nt_stmt_return;

typedef struct nt_stmt_label { const char *name; } nt_stmt_label;
typedef struct nt_stmt_goto { const char *name; } nt_stmt_goto;

typedef struct nt_layout_field {
	const char *name;
	const char *type_name; // e.g. "u32"
	int width; // 1, 2, 4, 8 bytes
} nt_layout_field;

typedef NT_VEC(nt_layout_field) nt_layout_field_list;

typedef struct nt_stmt_layout {
	const char *name;
	nt_layout_field_list fields;
} nt_stmt_layout;

typedef struct nt_use_item {
	const char *name;
	const char *alias;
} nt_use_item;

typedef NT_VEC(nt_use_item) nt_use_item_list;

struct nt_stmt {
	nt_stmt_kind kind;
	nt_token tok;
	union {
		nt_stmt_block block;
		struct {
			const char *module;
			const char *alias;
			bool is_local;
			bool import_all;
			nt_use_item_list imports;
		} use;
		nt_stmt_var var;
		struct {
			nt_expr *expr;
		} expr;
		nt_stmt_if iff;
		nt_stmt_while whl;
		nt_stmt_for fr;
		nt_stmt_try tr;
		nt_stmt_func fn;
		nt_stmt_return ret;
		nt_stmt_label label;
		nt_stmt_goto go;
		nt_stmt_defer de;
		nt_stmt_layout layout;
		nt_stmt_match match;
		struct {
			const char *name;
			nt_stmt_list body;
			bool export_all;
			const char *src_start;
			const char *src_end;
		} module;
		nt_stmt_export exprt;
	} as;
};

typedef struct nt_program {
	nt_stmt_list body;
	const char *doc; // optional module docstring
} nt_program;

nt_expr *nt_expr_new(nt_arena *arena, nt_expr_kind kind, nt_token tok);
nt_stmt *nt_stmt_new(nt_arena *arena, nt_stmt_kind kind, nt_token tok);
void nt_program_free(nt_program *prog, nt_arena *arena);

#endif
