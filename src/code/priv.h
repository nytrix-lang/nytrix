#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "code/code.h"
#include <llvm-c/Core.h>

// Builtins (builtins.c)
void add_builtins(codegen_t *cg);
bool builtin_allowed_comptime(const char *name);

// Lookup (lookup.c)
fun_sig *lookup_fun(codegen_t *cg, const char *name);
fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc);
const char *resolve_import_alias(codegen_t *cg, const char *name);
binding *lookup_global(codegen_t *cg, const char *name);
fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc);
binding *scope_lookup(scope *scopes, size_t depth, const char *name);
void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v,
          stmt_t *stmt, bool is_mut, const char *type_name);
void scope_pop(scope *scopes, size_t *depth);
void report_undef_symbol(codegen_t *cg, const char *name, token_t tok);
bool ny_diag_should_emit(const char *kind, token_t tok, const char *name);
bool ny_is_stdlib_tok(token_t tok);
bool ny_strict_error_enabled(codegen_t *cg, token_t tok);
void ny_diag_error(token_t tok, const char *fmt, ...);
void ny_diag_warning(token_t tok, const char *fmt, ...);
void ny_diag_hint(const char *fmt, ...);
void ny_diag_fix(const char *fmt, ...);
void ny_diag_note_tok(token_t tok, const char *fmt, ...);
LLVMTypeRef resolve_type_name(codegen_t *cg, const char *name, token_t tok);
LLVMTypeRef resolve_abi_type_name(codegen_t *cg, const char *name, token_t tok);
const char *infer_expr_type(codegen_t *cg, scope *scopes, size_t depth,
                            expr_t *e);
bool ensure_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth,
                                 const char *want, expr_t *expr, token_t tok,
                                 const char *ctx);
layout_def_t *lookup_layout(codegen_t *cg, const char *name);
type_layout_t resolve_raw_layout(codegen_t *cg, const char *name, token_t tok);
const char *codegen_qname(codegen_t *cg, const char *name,
                          const char *module_name);
void ny_dbg_loc(codegen_t *cg, token_t tok);
enum_member_def_t *lookup_enum_member(codegen_t *cg, const char *name);
enum_member_def_t *lookup_enum_member_owner(codegen_t *cg, const char *name,
                                            enum_def_t **out_enum);
char *codegen_full_name(codegen_t *cg, expr_t *e, arena_t *a);

// Expression generation (expr_t.c)
LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
LLVMValueRef gen_binary(codegen_t *cg, const char *op, LLVMValueRef l,
                        LLVMValueRef r);
LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v);
LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len);
LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth,
                         ny_param_list params, stmt_t *body, bool is_variadic,
                         const char *return_type, const char *name_hint);
LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body);
LLVMValueRef gen_call_expr(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e);

// Statement generation (stmt_t.c)
void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail);
void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root);

// Function generation (func.c)
void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures);
void collect_sigs(codegen_t *cg, stmt_t *s);

// Module handling (module.c)
void add_import_alias(codegen_t *cg, const char *alias, const char *full_name);
void add_import_alias_from_full(codegen_t *cg, const char *full_name);
stmt_t *find_module_stmt(stmt_t *s, const char *name);
bool module_has_export_list(const stmt_t *mod);
void collect_module_exports(stmt_t *mod, str_list *exports);
void collect_module_defs(stmt_t *mod, str_list *exports);
void add_imports_from_prefix(codegen_t *cg, const char *mod);
void process_use_imports(codegen_t *cg, stmt_t *s);
void collect_use_aliases(codegen_t *cg, stmt_t *s);
void collect_use_modules(codegen_t *cg, stmt_t *s);
void process_exports(codegen_t *cg, stmt_t *s);

// Core utilities (core.c)
LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type);

#endif // CODEGEN_INTERNAL_H
