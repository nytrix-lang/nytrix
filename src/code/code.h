#pragma once

#include "ast/ast.h"
#include "code/types.h"
#include "base/util.h"
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/DebugInfo.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline void ny_name_bloom_add(uint64_t bloom[4], uint64_t hash) {
  if (!bloom)
    return;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((b0 >> 6) < 4)
    bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  if ((b1 >> 6) < 4)
    bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

static inline bool ny_name_bloom_maybe_has(const uint64_t bloom[4],
                                           uint64_t hash) {
  if (!bloom)
    return false;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((b0 >> 6) < 4) {
    if ((bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
      return false;
  }
  if ((b1 >> 6) < 4) {
    if ((bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
      return false;
  }
  return true;
}

#define NY_REPORT_ERROR(cg, tok, ...)                                          \
  do {                                                                         \
    ny_diag_error(tok, __VA_ARGS__);                                           \
    (cg)->had_error = 1;                                                       \
  } while (0)

static inline void ny_fun_sig_init(fun_sig *sig, const char *name,
                                   LLVMTypeRef type, LLVMValueRef value,
                                   struct stmt_t *stmt, int arity,
                                   bool is_variadic, bool is_extern) {
  if (!sig)
    return;
  memset(sig, 0, sizeof(*sig));
  sig->name = name ? ny_strdup(name) : NULL;
  sig->type = type;
  sig->value = value;
  sig->stmt_t = stmt;
  sig->arity = arity;
  sig->is_variadic = is_variadic;
  sig->is_extern = is_extern;
  sig->effects = NY_FX_ALL;
  sig->args_escape = true;
  sig->args_mutated = true;
  sig->returns_alias = true;
  sig->owned = true;
}

static inline void ny_fun_sig_free_members(fun_sig *sig) {
  if (!sig)
    return;
  free((void *)sig->name);
  if (sig->link_name)
    free((void *)sig->link_name);
  if (sig->return_type)
    free((void *)sig->return_type);
}

static inline bool
ny_name_set_has_hash(const char *const *names, size_t names_len,
                     const uint64_t *hashes, size_t hashes_len,
                     const uint64_t bloom[4], const char *name, uint64_t hash) {
  if (!names || !name || names_len == 0)
    return false;
  if (!ny_name_bloom_maybe_has(bloom, hash))
    return false;
  bool has_hashes = hashes && hashes_len == names_len;
  for (size_t i = 0; i < names_len; ++i) {
    if (has_hashes && hashes[i] != hash)
      continue;
    if (names[i] && strcmp(names[i], name) == 0)
      return true;
  }
  return false;
}

static inline bool assigned_name_has(const assigned_name_list *names,
                                      const assigned_hash_list *hashes,
                                      const char *name, uint64_t hash,
                                      const uint64_t bloom[4]) {
  if (!names || !hashes || !name || !names->data || !hashes->data)
    return false;
  return ny_name_set_has_hash(names->data, names->len, hashes->data, hashes->len,
                              bloom, name, hash);
}

static inline bool assigned_name_contains(const assigned_name_list *names,
                                          const assigned_hash_list *hashes,
                                          const uint64_t bloom[4],
                                          const char *name) {
  if (!name || !*name)
    return false;
  uint64_t hash = ny_hash64_cstr(name);
  return assigned_name_has(names, hashes, name, hash, bloom);
}

static inline void assigned_name_add(assigned_name_list *names,
                                     assigned_hash_list *hashes,
                                     uint64_t bloom[4], const char *name) {
  if (!names || !name || !*name)
    return;
  if (!hashes)
    return;
  uint64_t hash = ny_hash64_cstr(name);
  if (assigned_name_has(names, hashes, name, hash, bloom))
    return;
  vec_push(names, name);
  vec_push(hashes, hash);
  ny_name_bloom_add(bloom, hash);
}

typedef struct label_binding {
  const char *name;
  LLVMBasicBlockRef bb;
  size_t depth;
} label_binding;

typedef struct string_intern {
  const char *data;
  size_t len;
  uint64_t hash;
  LLVMValueRef val;
  LLVMValueRef gv;
  LLVMModuleRef module;
  void *alloc;
} string_intern;

typedef struct intern_entry {
  uint32_t intern_idx; // 1-based index into interns vector. 0 means empty.
} intern_entry;

typedef struct codegen_t {
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMBuilderRef alloca_builder;
  LLVMContextRef ctx;
  program_t *prog;
  arena_t *arena;
  bool prog_owned;
  VEC(fun_sig) fun_sigs;
  VEC(binding) global_vars;
  VEC(string_intern) interns;
  intern_entry *intern_map;
  size_t intern_map_cap;
  size_t intern_map_len;
  bool llvm_ctx_owned;
  LLVMExecutionEngineRef ee;
  LLVMValueRef setjmp_fn;
  LLVMTypeRef setjmp_ty;
  LLVMTypeRef type_i1;
  LLVMTypeRef type_i64;
  LLVMTypeRef type_i32;
  LLVMTypeRef type_i16;
  LLVMTypeRef type_i8;
  LLVMTypeRef type_i128;
  LLVMTypeRef type_u128;
  LLVMTypeRef type_u64;
  LLVMTypeRef type_u32;
  LLVMTypeRef type_u16;
  LLVMTypeRef type_u8;
  LLVMTypeRef type_f32;
  LLVMTypeRef type_f64;
  LLVMTypeRef type_f128;
  LLVMTypeRef type_i8ptr;
  LLVMTypeRef type_bool;
  fun_sig *cached_fn_get;
  fun_sig *cached_fn_slice;
  fun_sig *cached_fn_list;
  fun_sig *cached_fn_append;
  fun_sig *cached_fn_dict;
  fun_sig *cached_fn_dict_set;
  fun_sig *cached_fn_set;
  fun_sig *cached_fn_set_add;
  fun_sig *cached_fn_sub;
  fun_sig *cached_fn_not;
  fun_sig *cached_fn_eq;
  fun_sig *cached_fn_contains;
  fun_sig *cached_fn_flt_box;
  fun_sig *cached_fn_flt_unbox;
  fun_sig *cached_fn_str_concat;
  fun_sig *cached_fn_to_str;
  fun_sig *cached_fn_globals;
  fun_sig *cached_fn_kwarg;
  bool llvm_value_names;
  bool debug_symbols;
  int debug_opt_level;
  const char *debug_opt_pipeline;
  bool trace_exec;
  const char *debug_main_file;
  LLVMDIBuilderRef di_builder;
  LLVMMetadataRef di_cu;
  LLVMMetadataRef di_file;
  LLVMMetadataRef di_scope;
  LLVMMetadataRef di_subroutine_type;
  int had_error;
  int lambda_count;
  VEC(binding) aliases;
  VEC(binding) import_aliases;
  VEC(binding) user_import_aliases;
  VEC(char *) use_modules;
  VEC(char *) user_use_modules;
  VEC(char *) link_allowed_modules;
  const char *current_module_name;
  const char *current_fn_ret_type;
  bool current_fn_attr_naked;
  bool thread_detach_stmt_call;
  const char *source_string;
  bool comptime;
  bool strict_diagnostics;
  bool auto_purity_infer;
  bool auto_memoize;
  bool auto_memoize_impure;
  uint64_t auto_memo_site_seq;
  bool is_repl;
  LLVMValueRef result_store_val;
  size_t func_root_idx;
  const char **assigned_names_data;
  size_t assigned_names_len;
  const uint64_t *assigned_name_hashes_data;
  size_t assigned_name_hashes_len;
  uint64_t assigned_names_bloom[4];
  VEC(label_binding) labels;
  VEC(arena_t *) extra_arenas;
  VEC(program_t *) extra_progs;
  VEC(enum_def_t *) enums;
  VEC(layout_def_t *) layouts;
  int64_t current_enum_val;
  VEC(char *) links;
  struct codegen_t *parent;
  bool skip_stdlib;
  const char *emit_module_name;
  bool emit_module_decls_only;
  bool emit_script;
  bool is_preparing;
  bool owned_metadata;
  void *sym_state;
} codegen_t;

void codegen_init(codegen_t *cg, program_t *prog, arena_t *arena,
                  const char *name);
void codegen_init_with_context(codegen_t *cg, program_t *prog, arena_t *arena,
                               LLVMModuleRef mod, LLVMContextRef ctx,
                               LLVMBuilderRef builder);
void codegen_prepare(codegen_t *cg);
void codegen_emit(codegen_t *cg);
LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name);
void codegen_collect_links(codegen_t *cg, program_t *prog);
void codegen_dispose(codegen_t *cg);
char *normalize_module_name(const char *raw);
void codegen_debug_init(codegen_t *cg, const char *main_file);
void codegen_debug_finalize(codegen_t *cg);
LLVMMetadataRef codegen_debug_subprogram(codegen_t *cg, LLVMValueRef func,
                                         const char *name, token_t tok);
void codegen_debug_variable(codegen_t *cg, const char *name, LLVMValueRef slot,
                            token_t tok, bool is_param, int param_idx,
                            bool is_slot);
void codegen_debug_global_variable(codegen_t *cg, const char *name,
                                   LLVMValueRef global, token_t tok);
LLVMMetadataRef codegen_debug_push_block(codegen_t *cg, token_t tok);
void codegen_debug_pop_block(codegen_t *cg, LLVMMetadataRef prev_scope);
LLVMMetadataRef codegen_debug_loc_scope(codegen_t *cg, token_t tok);

static inline bool ny_sig_in_current_sigs(const codegen_t *cg,
                                          const fun_sig *sig) {
  if (!sig)
    return false;
  if (sig->is_stable)
    return true;
  if (!cg || !cg->fun_sigs.data || cg->fun_sigs.len == 0)
    return false;
  const fun_sig *begin = cg->fun_sigs.data;
  const fun_sig *end = begin + cg->fun_sigs.len;
  return sig >= begin && sig < end;
}

static inline bool ny_binding_is_valid(const codegen_t *cg,
                                       const binding *b) {
  if (!b)
    return false;
  if (b->is_stable)
    return true;
  if (!cg || !cg->global_vars.data || cg->global_vars.len == 0)
    return false;
  const binding *begin = cg->global_vars.data;
  const binding *end = begin + cg->global_vars.len;
  return b >= begin && b < end;
}
