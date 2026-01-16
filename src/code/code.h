#pragma once

#include "parse/parser.h"
#include <llvm-c/Core.h>

// Minimal LLVM IR generator using LLVM C API.
// Link with llvm-config --libs core native

typedef struct binding {
  const char *name;
  LLVMValueRef value;
  struct stmt_t *stmt_t;
} binding;

typedef struct fun_sig {
  const char *name;
  LLVMTypeRef type;
  LLVMValueRef value;
  stmt_t *stmt_t;
  int arity;
  bool is_variadic;
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
  VEC(binding) impo_aliases;
  VEC(char *) use_modules;
  const char *current_module_name;
  bool comptime;
  LLVMValueRef result_store_val;
  size_t func_root_idx;
} codegen_t;

void codegen_init(codegen_t *cg, program_t *prog, const char *name);
void codegen_init_with_context(codegen_t *cg, program_t *prog,
                               LLVMModuleRef mod, LLVMContextRef ctx,
                               LLVMBuilderRef builder);
void codegen_emit(codegen_t *cg);
LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name);
void codegen_dispose(codegen_t *cg);
void codegen_reset(codegen_t *cg);
