#ifndef CODEGEN_INTERNAL_H
#define CODEGEN_INTERNAL_H

#include "code/code.h"
#include "rt/shared.h"
#include <llvm-c/Core.h>
#include <stdlib.h>
#include <string.h>

static inline const char *ny_llvm_name(codegen_t *cg, const char *name) {
  if (!name || !*name)
    return "";
  return (cg && cg->llvm_value_names) ? name : "";
}
#define NY_LLVM_NAME(cg, name) ny_llvm_name((cg), (name))

#define NY_SEMA_ASSERT(stmt, kind)                                                               \
  do {                                                                                           \
    const stmt_t *ny_sema_stmt__ = (stmt);                                                       \
    NY_COMPILER_ASSERTF(!ny_sema_stmt__ || ny_sema_stmt__->sema_kind == (kind),                  \
                        "sema kind mismatch: got %d expected %d",                                \
                        ny_sema_stmt__ ? (int)ny_sema_stmt__->sema_kind : -1, (int)(kind));      \
  } while (0)

static inline const char *ny_host_os_name(void) {
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

static inline const char *ny_host_arch_name(void) {
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

static inline LLVMBasicBlockRef ny_llvm_append_block(LLVMValueRef fn, const char *name) {
  return LLVMAppendBasicBlockInContext(LLVMGetModuleContext(LLVMGetGlobalParent(fn)), fn, name);
}

/* ── LLVM helpers ─────────────────────────────────────────── */
static inline LLVMBasicBlockRef ny_cur_block(codegen_t *cg) {
  return LLVMGetInsertBlock(cg->builder);
}
static inline LLVMValueRef ny_cur_fn(codegen_t *cg) {
  return LLVMGetBasicBlockParent(ny_cur_block(cg));
}
static inline LLVMValueRef ny_has_terminator(codegen_t *cg) {
  return LLVMGetBasicBlockTerminator(ny_cur_block(cg));
}
static inline LLVMValueRef ny_get_named_fn(codegen_t *cg, const char *n) {
  return LLVMGetNamedFunction(cg->module, n);
}
static inline LLVMTypeRef ny_i1_ty(codegen_t *cg) { return LLVMInt1TypeInContext(cg->ctx); }
static inline LLVMTypeRef ny_i8_ty(codegen_t *cg) { return LLVMInt8TypeInContext(cg->ctx); }
static inline LLVMTypeRef ny_ptr_i64_ty(codegen_t *cg) { return LLVMPointerType(cg->type_i64, 0); }
static inline const char *ny_type_leaf(const char *type_name) {
  if (!type_name)
    return NULL;
  while (*type_name == '?')
    type_name++;
  const char *dot = strrchr(type_name, '.');
  return dot ? dot + 1 : type_name;
}
static inline bool ny_type_is(const char *type_name, const char *want_tail) {
  const char *leaf = ny_type_leaf(type_name);
  return leaf && want_tail && strcmp(leaf, want_tail) == 0;
}
static inline bool ny_stmt_is_bare_std_use(const stmt_t *s) {
  if (!s || s->kind != NY_S_USE)
    return false;
  if (s->as.use.import_all || s->as.use.imports.len != 0 || s->as.use.alias || s->as.use.profile)
    return false;
  return s->as.use.module && strcmp(s->as.use.module, "std") == 0;
}
static inline const char *ny_sig_param_type(fun_sig *sig, size_t idx) {
  if (!sig || !sig->stmt_t)
    return NULL;
  if (sig->stmt_t->kind == NY_S_FUNC)
    return idx < sig->stmt_t->as.fn.params.len ? sig->stmt_t->as.fn.params.data[idx].type : NULL;
  if (sig->stmt_t->kind == NY_S_EXTERN)
    return idx < sig->stmt_t->as.ext.params.len ? sig->stmt_t->as.ext.params.data[idx].type : NULL;
  return NULL;
}
static inline bool ny_codegen_speed_profile_enabled(codegen_t *cg) {
  if (cg && cg->env_cache.speed_profile != 0)
    return cg->env_cache.speed_profile == 1;
  const char *p = getenv("NYTRIX_OPT_PROFILE");
  bool enabled = p && (strcmp(p, "speed") == 0 || strcmp(p, "peak") == 0);
  if (cg)
    cg->env_cache.speed_profile = enabled ? 1 : -1;
  return enabled;
}
static inline bool ny_fast_path_enabled(codegen_t *cg, const char *env_name) {
  if (cg) {
    if (cg->env_cache.fast_all == 0) {
      cg->env_cache.fast_all = ny_env_enabled("NYTRIX_FAST_ALL_PROFILES") ? 1 : -1;
    }
    if (cg->env_cache.fast_all == 1)
      return true;

    int *cache_ptr = NULL;
    if (strcmp(env_name, "NYTRIX_PROVEN_INT_CAST_FAST") == 0)
      cache_ptr = &cg->env_cache.proven_int_cast_fast;
    else if (strcmp(env_name, "NYTRIX_PROVEN_INT_MOD_FAST") == 0)
      cache_ptr = &cg->env_cache.proven_int_mod_fast;
    else if (strcmp(env_name, "NYTRIX_PROVEN_INT_BRANCH_FAST") == 0)
      cache_ptr = &cg->env_cache.proven_int_branch_fast;
    else if (strcmp(env_name, "NYTRIX_RAW_INT_EXPR_FAST") == 0)
      cache_ptr = &cg->env_cache.raw_int_expr_fast;
    else if (strcmp(env_name, "NYTRIX_RAW_INT_HELPERS") == 0)
      cache_ptr = &cg->env_cache.raw_int_helpers;
    else if (strcmp(env_name, "NYTRIX_UNTAGGED_INT_LIST_STORAGE") == 0)
      cache_ptr = &cg->env_cache.untagged_int_list_storage;
    else if (strcmp(env_name, "NYTRIX_CONST_STRING_GLOBAL_INIT") == 0)
      cache_ptr = &cg->env_cache.const_string_global_init;
    else if (strcmp(env_name, "NYTRIX_PRINT_PROVEN_INT_FAST") == 0)
      cache_ptr = &cg->env_cache.print_proven_int_fast;
    else if (strcmp(env_name, "NYTRIX_PRINT_PROVEN_STR_FAST") == 0)
      cache_ptr = &cg->env_cache.print_proven_str_fast;

    if (cache_ptr) {
      if (*cache_ptr == 0) {
        bool enabled = ny_env_enabled(env_name);
        if (!enabled &&
            (strcmp(env_name, "NYTRIX_PRINT_PROVEN_INT_FAST") == 0 ||
             strcmp(env_name, "NYTRIX_PRINT_PROVEN_STR_FAST") == 0)) {
          enabled = ny_codegen_speed_profile_enabled(cg);
        }
        *cache_ptr = enabled ? 1 : -1;
      }
      return *cache_ptr == 1;
    }
  }

  return ny_env_enabled("NYTRIX_FAST_ALL_PROFILES") ||
         ny_env_enabled(env_name) ||
         ((strcmp(env_name, "NYTRIX_PRINT_PROVEN_INT_FAST") == 0 ||
           strcmp(env_name, "NYTRIX_PRINT_PROVEN_STR_FAST") == 0) &&
          ny_codegen_speed_profile_enabled(cg));
}
static inline bool ny_expr_literal_i64(expr_t *e, int64_t *out) {
  if (!e || e->kind != NY_E_LITERAL || e->as.literal.kind != NY_LIT_INT || e->tok.kind == NY_T_NIL)
    return false;
  if (out)
    *out = e->as.literal.as.i;
  return true;
}
static inline bool ny_expr_is_list_or_tuple_lit(expr_t *e) {
  return e && (e->kind == NY_E_LIST || e->kind == NY_E_TUPLE);
}
static inline bool ny_expr_call_args(expr_t *e, call_arg_t **args, size_t *len) {
  if (!e)
    return false;
  if (e->kind == NY_E_CALL) {
    if (args)
      *args = e->as.call.args.data;
    if (len)
      *len = e->as.call.args.len;
    return true;
  }
  if (e->kind == NY_E_MEMCALL) {
    if (args)
      *args = e->as.memcall.args.data;
    if (len)
      *len = e->as.memcall.args.len;
    return true;
  }
  return false;
}
static inline expr_t *ny_binding_var_init_expr(binding *b, const char *name) {
  if (!b || !name || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *n = var->names.data[i];
    if (n && strcmp(n, name) == 0)
      return var->exprs.data[i];
  }
  return NULL;
}
static inline expr_t *ny_binding_static_indexable_lit(binding *b) {
  if (!b || b->is_mut || !b->stmt_t || b->stmt_t->kind != NY_S_VAR)
    return NULL;
  stmt_var_t *var = &b->stmt_t->as.var;
  for (size_t i = 0; i < var->names.len && i < var->exprs.len; ++i) {
    const char *name = var->names.data[i];
    expr_t *init = var->exprs.data[i];
    if (name && b->name && strcmp(name, b->name) == 0 && ny_expr_is_list_or_tuple_lit(init))
      return init;
  }
  return NULL;
}
static inline ssize_t ny_enum_member_field_index(enum_member_def_t *mem, const char *name) {
  if (!mem || !name)
    return -1;
  for (size_t i = 0; i < mem->fields.len; i++) {
    if (mem->fields.data[i].name && strcmp(mem->fields.data[i].name, name) == 0)
      return (ssize_t)i;
  }
  return -1;
}
static inline size_t ny_sig_min_arity(const fun_sig *sig) {
  if (!sig)
    return 0;
  size_t min_arity = (size_t)(sig->arity < 0 ? 0 : sig->arity);
  if (sig->min_arity_known) {
    if (sig->min_arity < 0)
      return 0;
    if (sig->min_arity > sig->arity)
      return min_arity;
    return (size_t)sig->min_arity;
  }
  if (sig->stmt_t && sig->stmt_t->kind == NY_S_FUNC)
    min_arity =
        (size_t)ny_param_required_arity(&sig->stmt_t->as.fn.params, sig->arity,
                                        sig->is_variadic);
  else if (sig->stmt_t && sig->stmt_t->kind == NY_S_EXTERN)
    min_arity =
        (size_t)ny_param_required_arity(&sig->stmt_t->as.ext.params, sig->arity,
                                        sig->is_variadic);
  return min_arity;
}
static inline bool ny_sig_allows_zero_arg_property(fun_sig *sig) {
  return sig && !sig->is_variadic && sig->arity >= 1 && ny_sig_min_arity(sig) <= 1;
}
static inline LLVMValueRef ny_fix_fn_ptr_codegen(codegen_t *cg, LLVMValueRef raw) {
  LLVMValueRef fix = LLVMGetNamedFunction(cg->module, "rt_fix_fn_ptr");
  if (!fix) {
    LLVMTypeRef args[1] = {cg->type_i64};
    LLVMTypeRef fn_ty = LLVMFunctionType(cg->type_i64, args, 1, false);
    fix = LLVMAddFunction(cg->module, "rt_fix_fn_ptr", fn_ty);
  }
  return LLVMBuildCall2(cg->builder, LLVMGlobalGetValueType(fix), fix,
                        (LLVMValueRef[]){raw}, 1, NY_LLVM_NAME(cg, "fix_fn_ptr"));
}
static inline LLVMValueRef ny_ptr2i64(codegen_t *cg, LLVMValueRef v, const char *n) {
  LLVMValueRef raw = LLVMBuildPtrToInt(cg->builder, v, cg->type_i64, NY_LLVM_NAME(cg, n));
  if (v && LLVMIsAFunction(v))
    raw = ny_fix_fn_ptr_codegen(cg, raw);
  return raw;
}
static inline LLVMValueRef ny_i642ptr(codegen_t *cg, LLVMValueRef v, const char *n) {
  return LLVMBuildIntToPtr(cg->builder, v, ny_ptr_i64_ty(cg), NY_LLVM_NAME(cg, n));
}
static inline LLVMValueRef ny_bitcast(codegen_t *cg, LLVMValueRef v, LLVMTypeRef t, const char *n) {
  return LLVMBuildBitCast(cg->builder, v, t, NY_LLVM_NAME(cg, n));
}
static inline LLVMValueRef ny_phi(codegen_t *cg, LLVMTypeRef t, const char *n) {
  return LLVMBuildPhi(cg->builder, t, NY_LLVM_NAME(cg, n));
}
static inline void ny_phi_add(LLVMValueRef p, LLVMValueRef v, LLVMBasicBlockRef b) {
  LLVMAddIncoming(p, &v, &b, 1);
}
static inline LLVMValueRef ny_cbool(codegen_t *cg, int v) {
  return LLVMConstInt(ny_i1_ty(cg), !!v, false);
}

void add_builtins(codegen_t *cg);
bool builtin_allowed_comptime(const char *name);

fun_sig *lookup_fun(codegen_t *cg, const char *name, uint64_t hash);
fun_sig *lookup_fun_exact(codegen_t *cg, const char *name);
fun_sig *lookup_use_module_fun(codegen_t *cg, const char *name, size_t argc);
const char *resolve_import_alias(codegen_t *cg, const char *name);
const char *ny_resolve_used_module_export_alias(codegen_t *cg, const char *name);
binding *lookup_global(codegen_t *cg, const char *name);
binding *lookup_global_hash(codegen_t *cg, const char *name, uint64_t hash);
binding *lookup_global_exact(codegen_t *cg, const char *name);
fun_sig *resolve_overload(codegen_t *cg, const char *name, size_t argc, uint64_t hash);
binding *scope_lookup(scope *scopes, size_t depth, const char *name);
binding *scope_lookup_hash(scope *scopes, size_t depth, const char *name, size_t name_len,
                           uint64_t hash);
binding *scope_lookup_hash_no_mark(scope *scopes, size_t depth, const char *name,
                                   size_t name_len, uint64_t hash);
binding *lookup_binding_hash(codegen_t *cg, scope *scopes, size_t depth, const char *name,
                             size_t name_len, uint64_t hash);
bool ny_builtin_name_shadowed_by_user_symbol(codegen_t *cg, scope *scopes, size_t depth,
                                             const char *name, size_t name_len,
                                             uint64_t hash);
const char *ny_builtin_surface_name_for_callee(expr_t *callee, size_t *out_len,
                                               uint64_t *out_hash);
binding *lookup_binding_hash_no_mark(scope *scopes, size_t depth, const char *name,
                                     size_t name_len, uint64_t hash);
void scope_bind(codegen_t *cg, scope *scopes, size_t depth, const char *name, LLVMValueRef v,
                stmt_t *stmt, bool is_mut, const char *type_name, bool is_slot);
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
bool ny_emit_module_match(codegen_t *cg, const char *cur_mod);
bool ny_lazy_emit_stdlib_var_needed(codegen_t *cg, stmt_t *s,
                                    const char *cur_mod);
void ny_diag_configure(int warn_level, bool compact_mode);
void ny_diag_error(token_t tok, const char *fmt, ...);
void ny_diag_warning(token_t tok, const char *fmt, ...);
void ny_diag_error_code(token_t tok, int code, const char *fmt, ...);
void ny_diag_warning_code(token_t tok, int code, const char *fmt, ...);
void ny_diag_hint(const char *fmt, ...);
void ny_diag_fix(const char *fmt, ...);
void ny_diag_note_tok(token_t tok, const char *fmt, ...);
void ny_emit_trace_loc_force(codegen_t *cg, token_t tok);
/* Enhanced error reporting */
void ny_diag_error_with_context(token_t tok, const char *primary_msg, const char *common_cause,
                                const char *fix_suggestion);
void ny_diag_type_mismatch(token_t tok, const char *expected, const char *got, const char *context);
LLVMTypeRef resolve_type_name(codegen_t *cg, const char *name, token_t tok);
LLVMTypeRef resolve_abi_type_name(codegen_t *cg, const char *name, token_t tok);
bool ny_type_is_tagged(const char *name);
bool ny_is_native_abi_type_name(const char *name);
void ny_register_tagged_type(codegen_t *cg, const char *name);
bool ny_lookup_tagged_type(codegen_t *cg, const char *name);
LLVMValueRef ny_coerce_to_abi(codegen_t *cg, LLVMValueRef v, const char *type_name);
LLVMValueRef ny_coerce_to_abi_proven_int(codegen_t *cg, LLVMValueRef v, const char *type_name,
                                         bool proven_int);
LLVMValueRef ny_box_abi_result(codegen_t *cg, LLVMValueRef v, const char *type_name);
const char *infer_expr_type(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
bool ny_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth, const char *want,
                             expr_t *expr);
bool ensure_expr_type_compatible(codegen_t *cg, scope *scopes, size_t depth, const char *want,
                                 expr_t *expr, token_t tok, const char *ctx);
layout_def_t *lookup_layout(codegen_t *cg, const char *name);
type_layout_t resolve_raw_layout(codegen_t *cg, const char *name, token_t tok);
const char *codegen_qname(codegen_t *cg, const char *name, const char *module_name);
void ny_dbg_loc(codegen_t *cg, token_t tok);
enum_member_def_t *lookup_enum_member(codegen_t *cg, const char *name);
enum_member_def_t *lookup_enum_member_owner(codegen_t *cg, const char *name, enum_def_t **out_enum);
char *codegen_full_name(codegen_t *cg, expr_t *e, arena_t *a);
static inline char *ny_adt_member_call_full_name(codegen_t *cg, expr_t *e) {
  if (!cg || !e)
    return NULL;
  if (e->kind == NY_E_CALL && e->as.call.callee)
    return codegen_full_name(cg, e->as.call.callee, cg->arena);
  if (e->kind == NY_E_MEMCALL && e->as.memcall.target && e->as.memcall.name) {
    char *target = codegen_full_name(cg, e->as.memcall.target, cg->arena);
    if (!target)
      return NULL;
    size_t a = strlen(target), b = strlen(e->as.memcall.name);
    char *out = arena_alloc(cg->arena, a + b + 2);
    memcpy(out, target, a);
    out[a] = '.';
    memcpy(out + a + 1, e->as.memcall.name, b + 1);
    return out;
  }
  return NULL;
}
LLVMValueRef expr_fail(codegen_t *cg, token_t tok, const char *fmt, ...);
LLVMValueRef ny_is_tagged_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_untag_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_tag_int(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_is_float(codegen_t *cg, LLVMValueRef v);
LLVMValueRef ny_unbox_float(codegen_t *cg, LLVMValueRef v);
bool ny_module_target_is_apple_arm64(LLVMModuleRef module);
bool ny_is_proven_int(codegen_t *cg, scope *scopes, size_t depth, expr_t *e, LLVMValueRef v);
bool ny_is_proven_bool(codegen_t *cg, scope *scopes, size_t depth, expr_t *e, LLVMValueRef v);

LLVMValueRef gen_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
LLVMValueRef gen_expr_list_stack_alloc(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
LLVMValueRef ny_try_member_property_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);

LLVMValueRef gen_expr_as_f64(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
LLVMValueRef ny_try_fast_f64buf_load_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                                            expr_t *e);
LLVMValueRef ny_try_native_call_as_f64(codegen_t *cg, scope *scopes, size_t depth,
                                       expr_t *e);
bool ny_build_mono_raw_int_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e,
                                LLVMValueRef *raw_out, LLVMValueRef *ok_out);
LLVMValueRef ny_try_direct_llvm_intrinsic(codegen_t *cg, scope *scopes, size_t depth,
                                          expr_t *e, const char *callee_name,
                                          bool shadowed, expr_call_t *c);
bool ny_gencall_type_is_nullable(const char *type_name);
bool ny_gencall_type_is(const char *type_name, const char *want_tail);
bool ny_gencall_type_is_real_number(const char *type_name);
bool ny_gencall_type_is_integer_number(const char *type_name);
bool ny_gencall_type_is_bigint(const char *type_name);
bool ny_gencall_type_is_number(const char *type_name);
bool ny_gencall_type_is_ordered_number(const char *type_name);
bool ny_gencall_check_math_contract(codegen_t *cg, scope *scopes,
                                    size_t depth, fun_sig *sig, expr_t *arg);
int ny_gencall_vec_type_dim(const char *type_name);
bool ny_gencall_type_is_vec(const char *type_name);
const char *ny_gencall_attached_owner(const char *type_name);
fun_sig *ny_gencall_lookup_attached_method(codegen_t *cg,
                                           const char *type_name,
                                           const char *method_name);
int ny_gencall_known_obj_tag(const char *type_name);
int ny_gencall_known_tagof(const char *type_name);
bool ny_gencall_type_is_known_obj(const char *type_name);
bool ny_gencall_type_is_known_non_obj(const char *type_name);
LLVMValueRef gen_binary(codegen_t *cg, scope *scopes, size_t depth, const char *op, LLVMValueRef l,
                        LLVMValueRef r, expr_t *le, expr_t *re);
LLVMValueRef to_bool(codegen_t *cg, LLVMValueRef v);
LLVMValueRef const_string_ptr(codegen_t *cg, const char *s, size_t len);
LLVMValueRef gen_closure(codegen_t *cg, scope *scopes, size_t depth, ny_param_list params,
                         stmt_t *body, bool is_variadic, const char *return_type,
                         const char *name_hint);
LLVMValueRef gen_comptime_eval(codegen_t *cg, stmt_t *body);
bool ny_eval_comptime_if(codegen_t *cg, stmt_t *s, bool *truthy);
LLVMValueRef gen_call_expr(codegen_t *cg, scope *scopes, size_t depth, expr_t *e);
void ny_lazy_emit_prepare_reachable(codegen_t *cg);
void ny_lazy_emit_demand_referenced(codegen_t *cg, scope *gsc, size_t gd,
                                    const char *phase);

void gen_stmt(codegen_t *cg, scope *scopes, size_t *depth, stmt_t *s, size_t func_root,
              bool is_tail);
void emit_defers(codegen_t *cg, scope *scopes, size_t depth, size_t func_root);
void collect_labels(codegen_t *cg, LLVMValueRef func, stmt_t *s, size_t depth);

void gen_func(codegen_t *cg, stmt_t *fn, const char *name, scope *scopes, size_t depth,
              binding_list *captures);
void collect_sigs(codegen_t *cg, stmt_t *s);
void infer_pure_functions(codegen_t *cg);

void add_import_alias(codegen_t *cg, const char *alias, const char *full_name);
void add_import_alias_from_full(codegen_t *cg, const char *full_name);
const char *ny_lookup_import_alias_indexed(codegen_t *cg, bool user_only,
                                           const char *alias,
                                           size_t alias_len,
                                           uint64_t alias_hash);
void ny_alias_lookup_cache_clear(codegen_t *cg);
stmt_t *find_module_stmt(stmt_t *s, const char *name);
bool module_has_export_list(const stmt_t *mod);
void collect_module_exports(stmt_t *mod, str_list *exports, const char *profile);
void collect_module_defs(stmt_t *mod, str_list *exports);
void add_imports_from_prefix(codegen_t *cg, bool user_use, const char *mod);
void process_default_core_imports(codegen_t *cg);
void process_use_imports(codegen_t *cg, stmt_t *s);
void collect_use_aliases(codegen_t *cg, stmt_t *s);
void collect_use_modules(codegen_t *cg, stmt_t *s);
void process_exports(codegen_t *cg, stmt_t *s);
bool ny_is_module_active(codegen_t *cg, const char *name);
const char *ny_lookup_module_alias(codegen_t *cg, scope *scopes, size_t depth,
                                   const char *name, size_t name_len,
                                   uint64_t name_hash);
bool ny_resolve_module_expr_path(codegen_t *cg, scope *scopes, size_t depth,
                                 expr_t *e, char *out, size_t out_cap);
bool ny_resolve_module_function_path(codegen_t *cg, const char *module_name,
                                     const char *member, char *out,
                                     size_t out_cap);
bool ny_codegen_token_is_source_file(codegen_t *cg, token_t tok);
bool ny_codegen_stmt_is_source_file(codegen_t *cg, stmt_t *s);
bool ny_program_has_explicit_main_entry(codegen_t *cg, program_t *prog);

void ny_sym_state_free(codegen_t *cg);
void ny_lookup_prof_register_atexit(void);
void ny_lookup_prof_note_pipeline_ms(double ms);

void codegen_emit_string_init(codegen_t *cg);
LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type);
void add_fn_string_attr(codegen_t *cg, LLVMValueRef fn, const char *name, const char *val);
void add_fn_enum_attr(codegen_t *cg, LLVMValueRef fn, const char *name, uint64_t val);
void ny_apply_rt_fn_attrs(codegen_t *cg, LLVMValueRef fn);
void ny_apply_decl_fn_attrs(codegen_t *cg, LLVMValueRef fn, stmt_t *fn_stmt);
void ny_apply_longjmp_fn_attrs(codegen_t *cg, LLVMValueRef fn);

/* ── Position & branching ───────────────────────────────────── */

static inline void ny_pos(codegen_t *cg, LLVMBasicBlockRef bb) {
  LLVMPositionBuilderAtEnd(cg->builder, bb);
}

static inline LLVMValueRef ny_br(codegen_t *cg, LLVMBasicBlockRef dest) {
  return LLVMBuildBr(cg->builder, dest);
}

static inline LLVMValueRef ny_cond_br(codegen_t *cg, LLVMValueRef cond, LLVMBasicBlockRef tb,
                                      LLVMBasicBlockRef fb) {
  return LLVMBuildCondBr(cg->builder, cond, tb, fb);
}

/* ── Loads & Stores ─────────────────────────────────────────── */

static inline LLVMValueRef ny_load(codegen_t *cg, LLVMValueRef ptr, const char *name) {
  return LLVMBuildLoad2(cg->builder, cg->type_i64, ptr, NY_LLVM_NAME(cg, name));
}

static inline LLVMValueRef ny_load_type(codegen_t *cg, LLVMTypeRef ty, LLVMValueRef ptr,
                                        const char *name) {
  return LLVMBuildLoad2(cg->builder, ty, ptr, NY_LLVM_NAME(cg, name));
}

static inline LLVMValueRef ny_store(codegen_t *cg, LLVMValueRef ptr, LLVMValueRef val) {
  return LLVMBuildStore(cg->builder, val, ptr);
}

/* ── Globals ────────────────────────────────────────────────── */

static inline LLVMValueRef ny_get_global(codegen_t *cg, const char *name) {
  return LLVMGetNamedGlobal(cg->module, name);
}

/* ── Constants ──────────────────────────────────────────────── */

static inline LLVMValueRef ny_c0(codegen_t *cg) { return LLVMConstInt(cg->type_i64, 0, false); }

static inline LLVMValueRef ny_c1(codegen_t *cg) { return LLVMConstInt(cg->type_i64, 1, false); }

static inline LLVMValueRef ny_cnil(codegen_t *cg) { return LLVMConstInt(cg->type_i64, NY_IMM_NIL, false); }

static inline LLVMValueRef ny_ctrue(codegen_t *cg) {
  return LLVMConstInt(cg->type_i64, NY_IMM_TRUE, false);
}

static inline LLVMValueRef ny_cfalse(codegen_t *cg) {
  return LLVMConstInt(cg->type_i64, NY_IMM_FALSE, false);
}

static inline LLVMValueRef ny_ci(codegen_t *cg, uint64_t v) {
  return LLVMConstInt(cg->type_i64, v, false);
}

/* ── Type helpers ───────────────────────────────────────────── */

static inline int ny_is_i64(codegen_t *cg, LLVMValueRef v) { return LLVMTypeOf(v) == cg->type_i64; }

static inline int ny_is_ptr(codegen_t *cg, LLVMValueRef v) {
  (void)cg;
  return LLVMGetTypeKind(LLVMTypeOf(v)) == LLVMPointerTypeKind;
}

static inline int ny_is_i1(codegen_t *cg, LLVMValueRef v) {
  return LLVMTypeOf(v) == LLVMInt1TypeInContext(cg->ctx);
}

static inline LLVMTypeRef ny_ptr_i64(codegen_t *cg) { return LLVMPointerType(cg->type_i64, 0); }

/* ── Conversions ────────────────────────────────────────────── */

/* ── Comparisons ────────────────────────────────────────────── */

static inline LLVMValueRef ny_icmp(codegen_t *cg, LLVMIntPredicate pred, LLVMValueRef lhs,
                                   LLVMValueRef rhs, const char *name) {
  return LLVMBuildICmp(cg->builder, pred, lhs, rhs, ny_llvm_name(cg, name));
}

#define ny_eq(cg, a, b, n) ny_icmp(cg, LLVMIntEQ, a, b, n)
#define ny_ne(cg, a, b, n) ny_icmp(cg, LLVMIntNE, a, b, n)
#define ny_slt(cg, a, b, n) ny_icmp(cg, LLVMIntSLT, a, b, n)
#define ny_sle(cg, a, b, n) ny_icmp(cg, LLVMIntSLE, a, b, n)
#define ny_sgt(cg, a, b, n) ny_icmp(cg, LLVMIntSGT, a, b, n)
#define ny_sge(cg, a, b, n) ny_icmp(cg, LLVMIntSGE, a, b, n)
#define ny_ult(cg, a, b, n) ny_icmp(cg, LLVMIntULT, a, b, n)
#define ny_ugt(cg, a, b, n) ny_icmp(cg, LLVMIntUGT, a, b, n)

/* ── Arithmetic ─────────────────────────────────────────────── */

static inline LLVMValueRef ny_add(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAdd(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_sub(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildSub(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_mul(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildMul(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_shl(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildShl(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_ashr(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAShr(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_and(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildAnd(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_or(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildOr(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_xor(codegen_t *cg, LLVMValueRef a, LLVMValueRef b, const char *n) {
  return LLVMBuildXor(cg->builder, a, b, ny_llvm_name(cg, n));
}

static inline LLVMValueRef ny_select(codegen_t *cg, LLVMValueRef cond, LLVMValueRef t,
                                     LLVMValueRef f, const char *n) {
  return LLVMBuildSelect(cg->builder, cond, t, f, ny_llvm_name(cg, n));
}

/* ── Calls ──────────────────────────────────────────────────── */

static inline LLVMValueRef ny_call0(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn) {
  return LLVMBuildCall2(cg->builder, ft, fn, NULL, 0, "");
}

static inline LLVMValueRef ny_call1(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn,
                                    LLVMValueRef a0) {
  return LLVMBuildCall2(cg->builder, ft, fn, &a0, 1, "");
}

static inline LLVMValueRef ny_call2(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn, LLVMValueRef a0,
                                    LLVMValueRef a1) {
  LLVMValueRef args[2] = {a0, a1};
  return LLVMBuildCall2(cg->builder, ft, fn, args, 2, "");
}

static inline LLVMValueRef ny_call3(codegen_t *cg, LLVMTypeRef ft, LLVMValueRef fn, LLVMValueRef a0,
                                    LLVMValueRef a1, LLVMValueRef a2) {
  LLVMValueRef args[3] = {a0, a1, a2};
  return LLVMBuildCall2(cg->builder, ft, fn, args, 3, "");
}

/* ── GEP / indexing ─────────────────────────────────────────── */

static inline LLVMValueRef ny_gep1(codegen_t *cg, LLVMTypeRef ty, LLVMValueRef ptr,
                                   LLVMValueRef idx, const char *name) {
  return LLVMBuildGEP2(cg->builder, ty, ptr, &idx, 1, ny_llvm_name(cg, name));
}

static inline LLVMValueRef ny_gep2(codegen_t *cg, LLVMTypeRef ty, LLVMValueRef ptr, LLVMValueRef i0,
                                   LLVMValueRef i1, const char *name) {
  LLVMValueRef idx[2] = {i0, i1};
  return LLVMBuildGEP2(cg->builder, ty, ptr, idx, 2, ny_llvm_name(cg, name));
}

/* ── PHI nodes ──────────────────────────────────────────────── */

/* ── Block creation ─────────────────────────────────────────── */

static inline LLVMBasicBlockRef ny_bb(codegen_t *cg, const char *name) {
  return ny_llvm_append_block(LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder)), name);
}

static inline LLVMBasicBlockRef ny_bb_fn(LLVMValueRef fn, const char *name) {
  return ny_llvm_append_block(fn, name);
}

/* ── Metadata (LLVM 21+ API) ────────────────────────────────── */

static inline void ny_loop_unroll_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMContextRef ctx = cg->ctx;
  unsigned kind = LLVMGetMDKindIDInContext(ctx, "llvm.loop", 9);
  LLVMMetadataRef s = LLVMMDStringInContext2(ctx, "llvm.loop.unroll.full", 21);
  LLVMMetadataRef md = LLVMMDNodeInContext2(ctx, &s, 1);
  LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ctx, md));
}

static inline void ny_loop_nounroll_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMContextRef ctx = cg->ctx;
  unsigned kind = LLVMGetMDKindIDInContext(ctx, "llvm.loop", 9);
  LLVMMetadataRef s = LLVMMDStringInContext2(ctx, "llvm.loop.unroll.disable", 24);
  LLVMMetadataRef md = LLVMMDNodeInContext2(ctx, &s, 1);
  LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ctx, md));
}

static inline void ny_loop_vectorize_hint(codegen_t *cg, LLVMValueRef branch) {
  LLVMContextRef ctx = cg->ctx;
  unsigned kind = LLVMGetMDKindIDInContext(ctx, "llvm.loop", 9);
  LLVMMetadataRef s = LLVMMDStringInContext2(ctx, "llvm.loop.vectorize.enable",
                                             sizeof("llvm.loop.vectorize.enable") - 1);
  LLVMMetadataRef v = LLVMValueAsMetadata(ny_cbool(cg, 1));
  LLVMMetadataRef ops[2] = {s, v};
  LLVMMetadataRef md = LLVMMDNodeInContext2(ctx, ops, 2);
  LLVMSetMetadata(branch, kind, LLVMMetadataAsValue(ctx, md));
}

#endif /* CODEGEN_INTERNAL_H */
