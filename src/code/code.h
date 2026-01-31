#pragma once

#include "parse/parser.h"
#include <llvm-c/Core.h>
#include <stdbool.h>
#include <stddef.h>

// Minimal LLVM IR generator using LLVM C API.
// Link with llvm-config --libs core native

typedef struct binding {
  const char *name;
  LLVMValueRef value;
  struct stmt_t *stmt_t;
  bool is_mut;
  bool is_used;
} binding;

typedef struct fun_sig {
  const char *name;
  LLVMTypeRef type;
  LLVMValueRef value;
  struct stmt_t *stmt_t;
  int arity;
  bool is_variadic;
  bool is_extern;
  const char *link_name;
  const char *return_type;
} fun_sig;

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
  LLVMTypeRef type_i64;
  LLVMTypeRef type_bool;
  int had_error;
  int lambda_count;
  VEC(binding) aliases;
  VEC(binding) import_aliases;
  VEC(binding) user_import_aliases;
  VEC(char *) use_modules;
  VEC(char *) user_use_modules;
  const char *current_module_name;
  const char *source_string;
  bool comptime;
  bool strict_diagnostics;
  bool implicit_prelude;
  LLVMValueRef result_store_val;
  size_t func_root_idx;
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
