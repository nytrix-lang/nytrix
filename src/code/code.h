#pragma once

#include "base/intern.h"
#include "base/util.h"
#include "code/types.h"
#include "parse/ast.h"
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <llvm-c/ExecutionEngine.h>
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

#define NY_ALIAS_BLOOM_WORDS 256u

static inline void ny_alias_bloom_add(uint64_t bloom[NY_ALIAS_BLOOM_WORDS],
                                      uint64_t hash) {
  if (!bloom)
    return;
  unsigned b0 = (unsigned)(hash & 16383u);
  unsigned b1 = (unsigned)((hash >> 14) & 16383u);
  bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

static inline bool
ny_alias_bloom_maybe_has(const uint64_t bloom[NY_ALIAS_BLOOM_WORDS],
                         uint64_t hash) {
  if (!bloom)
    return false;
  unsigned b0 = (unsigned)(hash & 16383u);
  unsigned b1 = (unsigned)((hash >> 14) & 16383u);
  if ((bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
    return false;
  if ((bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
    return false;
  return true;
}

typedef struct import_alias_slot {
  uint64_t hash;
  size_t index;
  bool occupied;
} import_alias_slot;

typedef struct module_stmt_slot {
  uint64_t hash;
  const char *name;
  stmt_t *stmt;
  bool occupied;
} module_stmt_slot;

#define NY_REPORT_ERROR(cg, tok, ...)                                          \
  do {                                                                         \
    ny_diag_error(tok, __VA_ARGS__);                                           \
    (cg)->had_error = 1;                                                       \
  } while (0)

static inline int ny_param_required_arity(const ny_param_list *params,
                                          int arity, bool is_variadic) {
  int required = arity < 0 ? 0 : arity;
  if (!params || params->len == 0)
    return required;
  size_t consider = params->len;
  if (is_variadic && consider > 0)
    consider--;
  if ((int)consider > required)
    consider = (size_t)required;
  while (consider > 0 && params->data[consider - 1].def)
    consider--;
  return (int)consider;
}

static inline void ny_fun_sig_set_min_arity(fun_sig *sig,
                                            const ny_param_list *params) {
  if (!sig)
    return;
  sig->min_arity = ny_param_required_arity(params, sig->arity, sig->is_variadic);
  sig->min_arity_known = true;
}

static inline void ny_fun_sig_set_param_types(fun_sig *sig,
                                              const ny_param_list *params) {
  if (!sig || !params)
    return;
  for (size_t i = 0; i < params->len; i++)
    vec_push(&sig->param_types,
             params->data[i].type ? ny_strdup(params->data[i].type) : NULL);
}

static inline void ny_fun_sig_set_params(fun_sig *sig,
                                         const ny_param_list *params) {
  ny_fun_sig_set_min_arity(sig, params);
  ny_fun_sig_set_param_types(sig, params);
}

static inline void ny_fun_sig_init(fun_sig *sig, const char *name,
                                   LLVMTypeRef type, LLVMValueRef value,
                                   struct stmt_t *stmt, int arity,
                                   bool is_variadic, bool is_extern) {
  if (!sig)
    return;
  memset(sig, 0, sizeof(*sig));
  if (name) {
    ny_sym_id name_id = ny_intern_cstr(name);
    sig->name = name_id ? ny_intern_get(name_id) : ny_strdup(name);
  }
  sig->type = type;
  sig->value = value;
  sig->stmt_t = stmt;
  sig->source_file =
      (stmt && stmt->tok.filename) ? ny_strdup(stmt->tok.filename) : NULL;
  sig->arity = arity;
  sig->min_arity = arity < 0 ? 0 : arity;
  sig->min_arity_known = true;
  sig->is_variadic = is_variadic;
  sig->is_extern = is_extern;
  sig->is_native_abi = false;
  sig->effects = is_extern ? NY_FX_FFI : NY_FX_ALL;
  sig->args_escape = true;
  sig->args_mutated = true;
  sig->returns_alias = true;
  sig->effects_known = is_extern;
  sig->owned = true;
}

static inline void ny_fun_sig_free_members(fun_sig *sig) {
  if (!sig)
    return;
  if (sig->name && !ny_intern_contains_ptr(sig->name))
    free((void *)sig->name);
  if (sig->module_name)
    free((void *)sig->module_name);
  if (sig->source_file)
    free((void *)sig->source_file);
  if (sig->link_name)
    free((void *)sig->link_name);
  if (sig->return_type)
    free((void *)sig->return_type);
  if (sig->abi_return_type)
    free((void *)sig->abi_return_type);
  if (sig->inferred_return_type)
    free((void *)sig->inferred_return_type);
  for (size_t i = 0; i < sig->param_types.len; i++)
    free(sig->param_types.data[i]);
  vec_free(&sig->param_types);
  if (sig->returns_borrow)
    free((void *)sig->returns_borrow);
  for (size_t i = 0; i < sig->borrows.len; i++)
    free(sig->borrows.data[i]);
  vec_free(&sig->borrows);
  for (size_t i = 0; i < sig->consumes.len; i++)
    free(sig->consumes.data[i]);
  vec_free(&sig->consumes);
  for (size_t i = 0; i < sig->mutates.len; i++)
    free(sig->mutates.data[i]);
  vec_free(&sig->mutates);
  for (size_t i = 0; i < sig->releases.len; i++)
    free(sig->releases.data[i]);
  vec_free(&sig->releases);
  for (size_t i = 0; i < sig->forgets.len; i++)
    free(sig->forgets.data[i]);
  vec_free(&sig->forgets);
  memset(sig, 0, sizeof(*sig));
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
  return ny_name_set_has_hash(names->data, names->len, hashes->data,
                              hashes->len, bloom, name, hash);
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
  uint32_t intern_idx;
} intern_entry;

typedef struct codegen_t codegen_t;

typedef struct codegen_llvm_t {
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMBuilderRef alloca_builder;
  LLVMContextRef ctx;
  bool llvm_ctx_owned;
  LLVMExecutionEngineRef ee;
  LLVMValueRef setjmp_fn;
  LLVMTypeRef setjmp_ty;
} codegen_llvm_t;

typedef struct codegen_types_t {
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
} codegen_types_t;

typedef struct codegen_ffi_t {
  VEC(char *) defines;
  struct {
    const char *path;
    const char *prefix;
    const char *lib;
    bool is_std;
  } *includes;
  size_t includes_len;
  size_t includes_cap;
} codegen_ffi_t;

typedef struct codegen_env_cache_t {
  int speed_profile;
  int fast_all;
  int proven_int_cast_fast;
  int proven_int_mod_fast;
  int proven_int_branch_fast;
  int proven_int_branch_eq_fast;
  int proven_int_branch_ops_mask;
  int raw_int_expr_fast;
  int raw_int_expr_addsub_fast;
  int raw_int_expr_mul_fast;
  int raw_int_helpers;
  int untagged_int_list_storage;
  int const_string_global_init;
  int print_proven_int_fast;
  int print_proven_str_fast;
} codegen_env_cache_t;

typedef struct codegen_symbols_t {
  VEC(fun_sig) fun_sigs;
  VEC(binding) global_vars;
  VEC(string_intern) interns;
  intern_entry *intern_map;
  void *builtin_shadow_cache;
  size_t intern_map_cap;
  size_t intern_map_len;
  fun_sig *cached_fn_get;
  fun_sig *cached_fn_index_read;
  fun_sig *cached_fn_slice;
  fun_sig *cached_fn_list;
  fun_sig *cached_fn_append;
  fun_sig *cached_fn_dict;
  fun_sig *cached_fn_set;
  fun_sig *cached_fn_set_add;
  fun_sig *cached_fn_sub;
  fun_sig *cached_fn_not;
  fun_sig *cached_fn_eq;
  fun_sig *cached_fn_contains;
  fun_sig *cached_fn_flt_box;
  fun_sig *cached_fn_flt_unbox;
  fun_sig *cached_fn_len;
  fun_sig *cached_fn_str_concat;
  fun_sig *cached_fn_to_str;
  fun_sig *cached_fn_globals;
  fun_sig *cached_fn_kwarg;
  fun_sig *last_lambda_sig;
  const char *active_str_append_name;
  LLVMValueRef active_str_append_builder;
  bool active_str_append_used;
  int lambda_count;
  int static_int_list_count;
} codegen_symbols_t;

typedef struct codegen_debug_t {
  bool llvm_value_names;
  bool debug_symbols;
  bool ownership_enabled;
  bool ownership_strict;
  bool ownership_runtime_cleanup;
  bool rc_heap_enabled;
  const char *heap_policy;
  int debug_opt_level;
  const char *debug_opt_pipeline;
  const char *debug_main_file;
  const char *source_main_file;
  LLVMDIBuilderRef di_builder;
  LLVMMetadataRef di_cu;
  LLVMMetadataRef di_file;
  LLVMMetadataRef di_scope;
  LLVMMetadataRef di_loc;
  LLVMMetadataRef di_loc_file_scope;
  LLVMMetadataRef di_loc_file_parent;
  const char *di_loc_file_name;
  LLVMMetadataRef di_subroutine_type;
  LLVMMetadataRef di_type_any;
  LLVMMetadataRef di_type_bool;
  LLVMMetadataRef di_type_int;
  LLVMMetadataRef di_type_i8;
  LLVMMetadataRef di_type_i16;
  LLVMMetadataRef di_type_i32;
  LLVMMetadataRef di_type_i64;
  LLVMMetadataRef di_type_u8;
  LLVMMetadataRef di_type_u16;
  LLVMMetadataRef di_type_u32;
  LLVMMetadataRef di_type_u64;
  LLVMMetadataRef di_type_f32;
  LLVMMetadataRef di_type_f64;
  LLVMMetadataRef di_type_ny_value;
  LLVMMetadataRef di_type_void;
  LLVMMetadataRef di_type_ptr;
  int had_error;
} codegen_debug_t;

typedef struct codegen_imports_t {
  VEC(binding) aliases;
  VEC(binding) import_aliases;
  VEC(binding) user_import_aliases;
  assigned_hash_list import_alias_hashes;
  assigned_hash_list user_import_alias_hashes;
  uint64_t import_alias_bloom[NY_ALIAS_BLOOM_WORDS];
  uint64_t user_import_alias_bloom[NY_ALIAS_BLOOM_WORDS];
  import_alias_slot *module_alias_index;
  size_t module_alias_index_cap;
  size_t module_alias_index_len;
  import_alias_slot *import_alias_index;
  size_t import_alias_index_cap;
  import_alias_slot *user_import_alias_index;
  size_t user_import_alias_index_cap;
  module_stmt_slot *module_stmt_index;
  size_t module_stmt_index_cap;
  size_t module_stmt_index_len;
  void *module_stmt_lookup_cache;
  void *module_public_target_cache;
  void *use_alias_lookup_cache;
  VEC(char *) use_modules;
  VEC(char *) user_use_modules;
  VEC(char *) link_allowed_modules;
  VEC(char *) tagged_types;
  VEC(char *) links;
} codegen_imports_t;

typedef struct codegen_flow_t {
  const char *current_module_name;
  const char *current_fn_ret_type;
  const char *current_fn_returns_borrow;
  bool current_fn_returns_owned;
  stmt_t *current_fn_body;
  bool current_fn_native_abi;
  bool current_fn_attr_naked;
  bool current_fn_attr_tailcall;
  LLVMValueRef current_fn_value;
  unsigned tail_call_depth;
  bool thread_detach_stmt_call;
  size_t active_panic_envs;
  LLVMValueRef result_store_val;
  size_t func_root_idx;
  const char **assigned_names_data;
  size_t assigned_names_len;
  const uint64_t *assigned_name_hashes_data;
  size_t assigned_name_hashes_len;
  uint64_t assigned_names_bloom[4];
  const char **top_entry_blocked_names_data;
  size_t top_entry_blocked_names_len;
  const uint64_t *top_entry_blocked_hashes_data;
  size_t top_entry_blocked_hashes_len;
  uint64_t top_entry_blocked_bloom[4];
  bool top_entry_local_hoist_enabled;
  assigned_name_list lazy_emit_names;
  assigned_hash_list lazy_emit_hashes;
  uint64_t lazy_emit_bloom[4];
  ny_name_set lazy_emit_name_set;
  assigned_name_list lazy_emit_collected_names;
  assigned_hash_list lazy_emit_collected_hashes;
  uint64_t lazy_emit_collected_bloom[4];
  ny_name_set lazy_emit_collected_set;
  bool lazy_emit_stdlib_enabled;
  VEC(label_binding) labels;
} codegen_flow_t;

typedef struct codegen_options_t {
  bool trace_exec;
  bool trace_emit_disabled;
  bool comptime;
  bool strict_diagnostics;
  bool strict_types;
  bool auto_purity_infer;
  bool auto_memoize;
  bool auto_memoize_impure;
  uint64_t auto_memo_site_seq;
  bool is_repl;
  bool opt_enabled;
  bool opt_type_infer;
  bool opt_const_fold;
  bool opt_tail_call;
  bool opt_inline_small;
  bool opt_lazy_load;
  bool opt_sys_mode;
  bool opt_unsafe_arith;
  const char *type_solver;
  bool user_native_abi;
  bool skip_stdlib;
  bool emit_cached_stdlib_init;
  const char *emit_module_name;
  bool emit_module_decls_only;
  bool emit_script;
} codegen_options_t;

typedef struct codegen_registry_t {
  ny_operator_def_list operators;
  VEC(enum_def_t *) enums;
  VEC(layout_def_t *) layouts;
  ny_mono_specialization_list mono_specs;
  bool mono_emitting;
  bool mono_raw_expr_disabled;
  size_t mono_inline_body_uses;
  size_t mono_masked_range_uses;
  int64_t current_enum_val;
} codegen_registry_t;

typedef struct codegen_resources_t {
  program_t *prog;
  arena_t *arena;
  bool prog_owned;
  const char *source_string;
  const char *user_source;
  size_t user_source_len;
  VEC(arena_t *) extra_arenas;
  VEC(program_t *) extra_progs;
  codegen_t *parent;
  bool is_preparing;
  bool owned_metadata;
  void *sym_state;
} codegen_resources_t;

struct codegen_t {
  union {
    codegen_llvm_t llvm;
    struct {
      LLVMModuleRef module;
      LLVMBuilderRef builder;
      LLVMBuilderRef alloca_builder;
      LLVMContextRef ctx;
      bool llvm_ctx_owned;
      LLVMExecutionEngineRef ee;
      LLVMValueRef setjmp_fn;
      LLVMTypeRef setjmp_ty;
    };
  };
  union {
    codegen_resources_t resources;
    struct {
      program_t *prog;
      arena_t *arena;
      bool prog_owned;
      const char *source_string;
      const char *user_source;
      size_t user_source_len;
      VEC(arena_t *) extra_arenas;
      VEC(program_t *) extra_progs;
      codegen_t *parent;
      bool is_preparing;
      bool owned_metadata;
      void *sym_state;
    };
  };
  union {
    codegen_symbols_t symbols;
    struct {
      VEC(fun_sig) fun_sigs;
      VEC(binding) global_vars;
      VEC(string_intern) interns;
      intern_entry *intern_map;
      void *builtin_shadow_cache;
      size_t intern_map_cap;
      size_t intern_map_len;
      fun_sig *cached_fn_get;
      fun_sig *cached_fn_index_read;
      fun_sig *cached_fn_slice;
      fun_sig *cached_fn_list;
      fun_sig *cached_fn_append;
      fun_sig *cached_fn_dict;
      fun_sig *cached_fn_set;
      fun_sig *cached_fn_set_add;
      fun_sig *cached_fn_sub;
      fun_sig *cached_fn_not;
      fun_sig *cached_fn_eq;
      fun_sig *cached_fn_contains;
      fun_sig *cached_fn_flt_box;
      fun_sig *cached_fn_flt_unbox;
      fun_sig *cached_fn_len;
      fun_sig *cached_fn_str_concat;
      fun_sig *cached_fn_to_str;
      fun_sig *cached_fn_globals;
      fun_sig *cached_fn_kwarg;
      fun_sig *last_lambda_sig;
      const char *active_str_append_name;
      LLVMValueRef active_str_append_builder;
      bool active_str_append_used;
      int lambda_count;
      int static_int_list_count;
    };
  };
  union {
    codegen_types_t types;
    struct {
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
    };
  };
  codegen_ffi_t ffi;
  codegen_env_cache_t env_cache;
  union {
    codegen_debug_t debug;
    struct {
      bool llvm_value_names;
      bool debug_symbols;
      bool ownership_enabled;
      bool ownership_strict;
      bool ownership_runtime_cleanup;
      bool rc_heap_enabled;
      const char *heap_policy;
      int debug_opt_level;
      const char *debug_opt_pipeline;
      const char *debug_main_file;
      const char *source_main_file;
      LLVMDIBuilderRef di_builder;
      LLVMMetadataRef di_cu;
      LLVMMetadataRef di_file;
      LLVMMetadataRef di_scope;
      LLVMMetadataRef di_loc;
      LLVMMetadataRef di_loc_file_scope;
      LLVMMetadataRef di_loc_file_parent;
      const char *di_loc_file_name;
      LLVMMetadataRef di_subroutine_type;
      LLVMMetadataRef di_type_any;
      LLVMMetadataRef di_type_bool;
      LLVMMetadataRef di_type_int;
      LLVMMetadataRef di_type_i8;
      LLVMMetadataRef di_type_i16;
      LLVMMetadataRef di_type_i32;
      LLVMMetadataRef di_type_i64;
      LLVMMetadataRef di_type_u8;
      LLVMMetadataRef di_type_u16;
      LLVMMetadataRef di_type_u32;
      LLVMMetadataRef di_type_u64;
      LLVMMetadataRef di_type_f32;
      LLVMMetadataRef di_type_f64;
      LLVMMetadataRef di_type_ny_value;
      LLVMMetadataRef di_type_void;
      LLVMMetadataRef di_type_ptr;
      int had_error;
    };
  };
  union {
    codegen_imports_t imports;
    struct {
      VEC(binding) aliases;
      VEC(binding) import_aliases;
      VEC(binding) user_import_aliases;
      assigned_hash_list import_alias_hashes;
      assigned_hash_list user_import_alias_hashes;
      uint64_t import_alias_bloom[NY_ALIAS_BLOOM_WORDS];
      uint64_t user_import_alias_bloom[NY_ALIAS_BLOOM_WORDS];
      import_alias_slot *module_alias_index;
      size_t module_alias_index_cap;
      size_t module_alias_index_len;
      import_alias_slot *import_alias_index;
      size_t import_alias_index_cap;
      import_alias_slot *user_import_alias_index;
      size_t user_import_alias_index_cap;
      module_stmt_slot *module_stmt_index;
      size_t module_stmt_index_cap;
      size_t module_stmt_index_len;
      void *module_stmt_lookup_cache;
      void *module_public_target_cache;
      void *use_alias_lookup_cache;
      VEC(char *) use_modules;
      VEC(char *) user_use_modules;
      VEC(char *) link_allowed_modules;
      VEC(char *) tagged_types;
      VEC(char *) links;
    };
  };
  union {
    codegen_flow_t flow;
    struct {
      const char *current_module_name;
      const char *current_fn_ret_type;
      const char *current_fn_returns_borrow;
      bool current_fn_returns_owned;
      stmt_t *current_fn_body;
      bool current_fn_native_abi;
      bool current_fn_attr_naked;
      bool current_fn_attr_tailcall;
      LLVMValueRef current_fn_value;
      unsigned tail_call_depth;
      bool thread_detach_stmt_call;
      size_t active_panic_envs;
      LLVMValueRef result_store_val;
      size_t func_root_idx;
      const char **assigned_names_data;
      size_t assigned_names_len;
      const uint64_t *assigned_name_hashes_data;
      size_t assigned_name_hashes_len;
      uint64_t assigned_names_bloom[4];
      const char **top_entry_blocked_names_data;
      size_t top_entry_blocked_names_len;
      const uint64_t *top_entry_blocked_hashes_data;
      size_t top_entry_blocked_hashes_len;
      uint64_t top_entry_blocked_bloom[4];
      bool top_entry_local_hoist_enabled;
      assigned_name_list lazy_emit_names;
      assigned_hash_list lazy_emit_hashes;
      uint64_t lazy_emit_bloom[4];
      ny_name_set lazy_emit_name_set;
      assigned_name_list lazy_emit_collected_names;
      assigned_hash_list lazy_emit_collected_hashes;
      uint64_t lazy_emit_collected_bloom[4];
      ny_name_set lazy_emit_collected_set;
      bool lazy_emit_stdlib_enabled;
      VEC(label_binding) labels;
    };
  };
  union {
    codegen_options_t options;
    struct {
      bool trace_exec;
      bool trace_emit_disabled;
      bool comptime;
      bool strict_diagnostics;
      bool strict_types;
      bool auto_purity_infer;
      bool auto_memoize;
      bool auto_memoize_impure;
      uint64_t auto_memo_site_seq;
      bool is_repl;
      bool opt_enabled;
      bool opt_type_infer;
      bool opt_const_fold;
      bool opt_tail_call;
      bool opt_inline_small;
      bool opt_lazy_load;
      bool opt_sys_mode;
      bool opt_unsafe_arith;
      const char *type_solver;
      bool user_native_abi;
      bool skip_stdlib;
      bool emit_cached_stdlib_init;
      const char *emit_module_name;
      bool emit_module_decls_only;
      bool emit_script;
    };
  };
  union {
    codegen_registry_t registry;
    struct {
      ny_operator_def_list operators;
      VEC(enum_def_t *) enums;
      VEC(layout_def_t *) layouts;
      ny_mono_specialization_list mono_specs;
      bool mono_emitting;
      bool mono_raw_expr_disabled;
      size_t mono_inline_body_uses;
      size_t mono_masked_range_uses;
      int64_t current_enum_val;
    };
  };
};

void codegen_init(codegen_t *cg, program_t *prog, arena_t *arena,
                  const char *name);
void codegen_init_with_context(codegen_t *cg, program_t *prog, arena_t *arena,
                               LLVMModuleRef mod, LLVMContextRef ctx,
                               LLVMBuilderRef builder);
void codegen_prepare(codegen_t *cg);
void collect_sigs(codegen_t *cg, struct stmt_t *s);
void collect_use_modules(codegen_t *cg, struct stmt_t *s);
void codegen_repopulate_interns(codegen_t *cg);
void codegen_rebind_llvm_symbols(codegen_t *cg);
void codegen_emit(codegen_t *cg);
LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name);
void codegen_collect_links(codegen_t *cg, program_t *prog);
void codegen_dispose(codegen_t *cg);
char *normalize_module_name(const char *raw);
void ny_cg_emit_trace_exit(codegen_t *cg);
void ny_cg_emit_trace_return(codegen_t *cg, LLVMValueRef v,
                             const char *ret_type);
void ny_cg_emit_trace_return_void(codegen_t *cg);
void codegen_debug_init(codegen_t *cg, const char *main_file);
void codegen_debug_finalize(codegen_t *cg);
LLVMMetadataRef codegen_debug_subprogram(codegen_t *cg, LLVMValueRef func,
                                         const char *name, token_t tok);
void codegen_debug_variable(codegen_t *cg, const char *name,
                            const char *type_name, LLVMValueRef slot,
                            token_t tok, bool is_param, int param_idx,
                            bool is_slot);
void codegen_debug_global_variable(codegen_t *cg, const char *name,
                                   LLVMValueRef global, const char *type_name,
                                   token_t tok);
LLVMMetadataRef codegen_debug_push_block(codegen_t *cg, token_t tok);
void codegen_debug_pop_block(codegen_t *cg, LLVMMetadataRef prev_scope);
LLVMMetadataRef codegen_debug_loc_scope(codegen_t *cg, token_t tok);

static inline bool ny_sig_in_current_sigs(const codegen_t *cg,
                                          const fun_sig *sig) {
  if (!sig)
    return false;
  if (!cg || !cg->fun_sigs.data || cg->fun_sigs.len == 0)
    return false;
  const fun_sig *begin = cg->fun_sigs.data;
  const fun_sig *end = begin + cg->fun_sigs.len;
  if (!(sig >= begin && sig < end))
    return false;
  return true;
}

static inline bool ny_binding_is_valid(const codegen_t *cg, const binding *b) {
  if (!b)
    return false;
  if (!cg || !cg->global_vars.data || cg->global_vars.len == 0)
    return false;
  const binding *begin = cg->global_vars.data;
  const binding *end = begin + cg->global_vars.len;
  if (!(b >= begin && b < end))
    return false;
  return true;
}
