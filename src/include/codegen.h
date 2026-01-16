#pragma once

#include <llvm-c/Core.h>
#include "parser.h"

// Minimal LLVM IR generator using LLVM C API.
// Link with llvm-config --libs core native

typedef struct binding {
	const char *name;
	LLVMValueRef value;
	struct nt_stmt *stmt;
} binding;

typedef struct fun_sig {
	const char *name;
	LLVMTypeRef type;
	LLVMValueRef value;
	nt_stmt *stmt;
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

typedef struct nt_codegen {
	LLVMModuleRef module;
	LLVMBuilderRef builder;
	LLVMContextRef ctx;
	nt_program *prog;
	NT_VEC(fun_sig) fun_sigs;
	NT_VEC(binding) global_vars;
	NT_VEC(string_intern) interns;
	bool llvm_ctx_owned;
	LLVMValueRef setjmp_fn;
	LLVMTypeRef setjmp_ty;
	LLVMTypeRef type_i64;
	LLVMTypeRef type_bool;
	int had_error;
	int lambda_count;
	NT_VEC(binding) aliases;
	NT_VEC(binding) import_aliases;
	NT_VEC(char *) use_modules;
	const char *current_mod;
	bool is_comptime;
	LLVMValueRef result_store;
	size_t func_root;
} nt_codegen;

void nt_codegen_init(nt_codegen *cg, nt_program *prog, const char *name);
void nt_codegen_init_with_context(nt_codegen *cg, nt_program *prog, LLVMModuleRef mod, LLVMContextRef ctx, LLVMBuilderRef builder);
void nt_codegen_emit(nt_codegen *cg);
LLVMValueRef nt_codegen_emit_script(nt_codegen *cg, const char *name);
void nt_codegen_dispose(nt_codegen *cg);
void nt_codegen_reset(nt_codegen *cg);
