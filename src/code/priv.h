#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "code/code.h"
#include <llvm-c/Core.h>

static inline const char *ny_llvm_name(codegen_t *cg, const char *name) {
  if (!name || !*name)
    return "";
  return (cg && cg->llvm_value_names) ? name : "";
}
#define NY_LLVM_NAME(cg, name) ny_llvm_name((cg), (name))

static inline LLVMBasicBlockRef ny_llvm_append_block(LLVMValueRef fn,
                                                     const char *name) {
  return LLVMAppendBasicBlockInContext(
      LLVMGetModuleContext(LLVMGetGlobalParent(fn)), fn, name);
}

void add_builtins(codegen_t *cg);
bool builtin_allowed_comptime(const char *name);

fun_sig *lookup_fun(codegen_t *cg, const char *name, uint64_t hash);
fun_sig *lookup_fun_exact(codegen_t *cg, const char *name);
fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc);
const char *resolve_import_alias(codegen_t *cg, const char *name);
binding *lookup_global(codegen_t *cg, const char *name);
binding *lookup_global_hash(codegen_t *cg, const char *name, uint64_t hash);
binding *lookup_global_exact(codegen_t *cg, const char *name);
fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc,
                          uint64_t hash);
binding *scope_lookup(scope *scopes, size_t depth, const char *name);
binding *scope_lookup_hash(scope *scopes, size_t depth, const char *name,
                           size_t name_len, uint64_t hash);
void scope_bind(codegen_t *cg, scope *scopes, size_t depth, const char *name,
                LLVMValueRef v, stmt_t *stmt, bool is_mut,
                const char *type_name, bool is_slot);
void scope_pop(scope *scopes, size_t *depth);
uint64_t ny_hash_name(const char *s, size_t len);
uint32_t ny_binding_name_len(binding *b);
uint64_t ny_binding_name_hash(binding *b);
void ny_scope_bloom_add(scope *sc, uint64_t hash);
bool ny_scope_bloom_maybe_has(const scope *sc, uint64_t hash);
void report_undef_symbol(codegen_t *cg, const char *name, token_t tok);
bool ny_diag_should_emit(const char *kind, token_t tok, const char *name);
bool ny_is_stdlib_tok(token_t tok);
bool ny_strict_error_enabled(codegen_t *cg, token_t tok);
void ny_diag_error(token_t tok, const char *fmt, ...);
void ny_diag_warning(token_t tok, const char *fmt, ...);
void ny_diag_hint(const char *fmt, ...);
void ny_diag_fix(const char *fmt, ...);
void ny_diag_note_tok(token_t tok, const char *fmt, ...);
/* Enhanced error reporting */
void ny_diag_error_with_context(token_t tok, const char *primary_msg,
                                const char *common_cause,
                                const char *fix_suggestion);
void ny_diag_type_mismatch(token_t tok, const char *expected, const char *got,
                           const char *context);
LLVMTypeRef resolve_type_name(codegen_t *cg, const char *name, token_t tok);
LLVMTypeRef resolve_abi_type_name(codegen_t *cg, const char *name, token_t tok);
bool ny_type_is_tagged(const char *name);
bool ny_is_native_abi_type_name(const char *name);
LLVMValueRef ny_coerce_to_abi(codegen_t *cg, LLVMValueRef v,
                              const char *type_name);
LLVMValueRef ny_box_abi_result(codegen_t *cg, LLVMValueRef v,
                               const char *type_name);
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
LLVMValueRef expr_fail(codegen_t *cg, token_t tok, const char *fmt, ...);
LLVMValueRef ny_is_tagged_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_untag_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_tag_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_is_float(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_unbox_float(codegen_t *cg, LLVMValueRef v);
bool ny_is_proven_int(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                      LLVMValueRef v);
bool ny_is_proven_bool(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                       LLVMValueRef v);

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);

LLVMValueRef gen_expr_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                             expr_t *e);
LLVMValueRef gen_binary(codegen_t *cg, scope *scopes, size_t depth,
                        const char *op, LLVMValueRef l, LLVMValueRef r,
                        expr_t *le, expr_t *re);
LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v);
LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len);
LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth,
                         ny_param_list params, stmt_t *body, bool is_variadic,
                         const char *return_type, const char *name_hint);
LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body);
bool ny_eval_comptime_if(codegen_t *cg, stmt_t *s, bool *truthy);
LLVMValueRef gen_call_expr(codegen_t *cg, scope *scopes, size_t depth,
                           expr_t *e);

void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s,
              size_t func_root, bool is_tail);
void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root);

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes,
              size_t depth, binding_list *captures);
void collect_sigs(codegen_t *cg, stmt_t *s);
void infer_pure_functions(codegen_t *cg);

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
bool ny_is_module_active(codegen_t *cg, const char *name);

void ny_sym_state_free(codegen_t *cg);

void codegen_emit_string_init(codegen_t *cg);
LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type);
void add_fn_string_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                        const char *val);
void add_fn_enum_attr(codegen_t *cg, LLVMValueRef fn, const char *name,
                      uint64_t val);
void ny_apply_base_fn_attrs(codegen_t *cg, LLVMValueRef fn);

#endif
