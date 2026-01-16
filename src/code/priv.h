#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "code/code.h"
#include <llvm-c/Core.h>

// Scope and binding types
typedef VEC(binding) binding_list;
typedef VEC(char *) ny_str_list;
typedef VEC(char *) str_list;

typedef struct scope {
  VEC(binding) vars;
  VEC(stmt_t *) defers;
  LLVMBasicBlockRef break_bb;
  LLVMBasicBlockRef continue_bb;
} scope;

// Builtins (builtins.c)
void add_builtins(codegen_t *cg);
bool builtin_allowed_comptime(const char *name);

// Lookup (lookup.c)
fun_sig *lookup_fun(codegen_t *cg, const char *name);
fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc);
const char *resolve_impo_alias(codegen_t *cg, const char *name);
binding *lookup_global(codegen_t *cg, const char *name);
fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc);
binding *scope_lookup(scope *scopes, size_t depth, const char *name);
void bind(scope *scopes, size_t depth, const char *name, LLVMValueRef v,
          stmt_t *stmt_t);

// Expression generation (expr_t.c)
LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
LLVMValueRef gen_binary(codegen_t *cg, const char *op, LLVMValueRef l,
                        LLVMValueRef r);
LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v);
LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len);
LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body);

// Statement generation (stmt_t.c)
void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail);
void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root);

// Function generation (func.c)
void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures);
void collect_sigs(codegen_t *cg, stmt_t *s);

// Module handling (module.c)
void add_impo_alias(codegen_t *cg, const char *alias, const char *full_name);
void add_impo_alias_from_full(codegen_t *cg, const char *full_name);
stmt_t *find_module_stmt(stmt_t *s, const char *name);
bool module_has_expo_list(const stmt_t *mod);
void collect_module_exports(stmt_t *mod, str_list *exports);
void collect_module_defs(stmt_t *mod, str_list *exports);
void add_imports_from_prefix(codegen_t *cg, const char *mod);
void process_use_imports(codegen_t *cg, stmt_t *s);
void collect_use_aliases(codegen_t *cg, stmt_t *s);
void collect_use_modules(codegen_t *cg, stmt_t *s);
void process_exports(codegen_t *cg, stmt_t *s);
// Removed: add_exposed_imports, is_exposed (not found in current codegen_t.c,
// staying strict to existing code)

// Core utilities (core.c)
LLVMValueRef build_alloca(codegen_t *cg, const char *name);

#endif // CODEGEN_INTERNAL_H
