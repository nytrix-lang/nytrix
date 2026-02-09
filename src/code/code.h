#pragma once

#include "ast/ast.h"        // For stmt_t, program_t, ny_param_list
#include "code/types.h" // For forward declarations and new semantic types
#include <llvm-c/Core.h>
#include <llvm-c/DebugInfo.h>
#include <stdbool.h>
#include <stddef.h>

// Minimal LLVM IR generator using LLVM C API.
// Link with llvm-config --libs core native

typedef struct label_binding {
  const char *name;
  LLVMBasicBlockRef bb;
  size_t depth;
} label_binding;
// Removed binding and fun_sig definitions
typedef struct string_intern {
  const char *data;
  size_t len;
  LLVMValueRef val;
  LLVMValueRef gv;
  void *alloc;
} string_intern;

typedef struct codegen_t {
  LLVMModuleRef module;
  LLVMBuilderRef builder;
  LLVMContextRef ctx;
  program_t *prog;
  arena_t *arena;
  bool prog_owned;
  VEC(fun_sig) fun_sigs;
  VEC(binding) global_vars;
  VEC(string_intern) interns;
  bool llvm_ctx_owned;
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
  bool debug_symbols;
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
  const char *source_string;
  bool comptime;
  bool strict_diagnostics;
  bool implicit_prelude;
  LLVMValueRef result_store_val;
  size_t func_root_idx;
  VEC(label_binding) labels;
  VEC(arena_t *) extra_arenas;
  VEC(program_t *) extra_progs;
  // Add enum related fields back, now that enum_def_t is defined in types_fwd.h
  VEC(enum_def_t *) enums;
  VEC(layout_def_t *) layouts;
  int64_t current_enum_val;
} codegen_t;

void codegen_init(codegen_t *cg, program_t *prog, arena_t *arena,
                  const char *name);
void codegen_init_with_context(codegen_t *cg, program_t *prog, arena_t *arena,
                               LLVMModuleRef mod, LLVMContextRef ctx,
                               LLVMBuilderRef builder);
void codegen_emit(codegen_t *cg);
LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name);
void codegen_dispose(codegen_t *cg);
void codegen_reset(codegen_t *cg);
char *normalize_module_name(const char *raw);
void codegen_debug_init(codegen_t *cg, const char *main_file);
void codegen_debug_finalize(codegen_t *cg);
LLVMMetadataRef codegen_debug_subprogram(codegen_t *cg, LLVMValueRef func,
                                         const char *name, token_t tok);
