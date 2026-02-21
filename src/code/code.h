#pragma once

#include "ast/ast.h"    // For stmt_t, program_t, ny_param_list
#include "code/types.h" // For forward declarations and new semantic types
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/DebugInfo.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// Minimal LLVM IR generator using LLVM C API.
// Link with llvm-config --libs core native

/* Shared tiny bloom + hashed-name lookup helpers used by codegen passes. */
static inline void ny_name_bloom_add(uint64_t bloom[4], uint64_t hash) {
  if (!bloom)
    return;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  bloom[b0 >> 6] |= (uint64_t)1u << (b0 & 63u);
  bloom[b1 >> 6] |= (uint64_t)1u << (b1 & 63u);
}

static inline bool ny_name_bloom_maybe_has(const uint64_t bloom[4],
                                           uint64_t hash) {
  if (!bloom)
    return false;
  unsigned b0 = (unsigned)(hash & 255u);
  unsigned b1 = (unsigned)((hash >> 8) & 255u);
  if ((bloom[b0 >> 6] & ((uint64_t)1u << (b0 & 63u))) == 0)
    return false;
  if ((bloom[b1 >> 6] & ((uint64_t)1u << (b1 & 63u))) == 0)
    return false;
  return true;
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

typedef struct label_binding {
  const char *name;
  LLVMBasicBlockRef bb;
  size_t depth;
} label_binding;
// Removed binding and fun_sig definitions
typedef struct string_intern {
  const char *data;
  size_t len;
  uint64_t hash;
  LLVMValueRef val;
  LLVMValueRef gv;
  LLVMModuleRef module;
  void *alloc;
} string_intern;

struct braun_ssa_context;

typedef struct codegen_t {
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMBuilderRef alloca_builder;
  LLVMContextRef ctx;
  program_t *prog;
  arena_t *arena;
  struct braun_ssa_context *braun;
  bool prog_owned;
  VEC(fun_sig) fun_sigs;
  VEC(binding) global_vars;
  VEC(string_intern) interns;
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
  /* Hot helper cache: avoids repeated symbol lookup chains in codegen. */
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
  // Add enum related fields back, now that enum_def_t is defined in types_fwd.h
  VEC(enum_def_t *) enums;
  VEC(layout_def_t *) layouts;
  int64_t current_enum_val;
  struct codegen_t *parent;
  bool skip_stdlib;
} codegen_t;

void codegen_init(codegen_t *cg, program_t *prog, arena_t *arena,
                  const char *name);
void codegen_init_with_context(codegen_t *cg, program_t *prog, arena_t *arena,
                               LLVMModuleRef mod, LLVMContextRef ctx,
                               LLVMBuilderRef builder);
void codegen_emit(codegen_t *cg);
LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name);
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
