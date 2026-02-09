#include "code/priv.h"
#include "base/util.h"
#include "priv.h"
#include "std_symbols.h"
#include <alloca.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

LLVMValueRef build_alloca(codegen_t *cg, const char *name, LLVMTypeRef type) {
  LLVMBuilderRef b = LLVMCreateBuilderInContext(cg->ctx);
  LLVMValueRef f = LLVMGetBasicBlockParent(LLVMGetInsertBlock(cg->builder));
  if (!f) {
    LLVMDisposeBuilder(b);
    return NULL;
  }
  LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(f);
  LLVMValueRef first = LLVMGetFirstInstruction(entry);
  if (first)
    LLVMPositionBuilderBefore(b, first);
  else
    LLVMPositionBuilderAtEnd(b, entry);
  LLVMValueRef res = LLVMBuildAlloca(b, type, name);
  LLVMDisposeBuilder(b);
  return res;
}

void codegen_init_with_context(codegen_t *cg, program_t *prog,
                               struct arena_t *arena, LLVMModuleRef mod,
                               LLVMContextRef ctx, LLVMBuilderRef builder) {
  memset(cg, 0, sizeof(codegen_t));
  cg->ctx = ctx;
  cg->module = mod;
  cg->builder = builder;
  cg->prog = prog;
  cg->arena = arena;

  cg->comptime = false;
  cg->strict_diagnostics = getenv("NYTRIX_STRICT_DIAGNOSTICS") != NULL;
  cg->implicit_prelude = true;
  if (cg->strict_diagnostics)
    NY_LOG_V1("Strict diagnostics enabled (NYTRIX_STRICT_DIAGNOSTICS)\n");
  cg->type_i1 = LLVMInt1TypeInContext(cg->ctx);
  cg->type_i8 = LLVMInt8TypeInContext(cg->ctx);
  cg->type_i16 = LLVMInt16TypeInContext(cg->ctx);
  cg->type_i32 = LLVMInt32TypeInContext(cg->ctx);
  cg->type_i64 = LLVMInt64TypeInContext(cg->ctx);
  cg->type_i128 = LLVMInt128TypeInContext(cg->ctx);
  cg->type_u8 = cg->type_i8;
  cg->type_u16 = cg->type_i16;
  cg->type_u32 = cg->type_i32;
  cg->type_u64 = cg->type_i64;
  cg->type_u128 = cg->type_i128;
  cg->type_f32 = LLVMFloatTypeInContext(cg->ctx);
  cg->type_f64 = LLVMDoubleTypeInContext(cg->ctx);
  cg->type_f128 = LLVMFP128TypeInContext(cg->ctx);
  cg->type_bool = cg->type_i1;
  cg->type_i8ptr = LLVMPointerType(cg->type_i8, 0);

  cg->had_error = 0;
  cg->lambda_count = 0;
  add_builtins(cg);
}

void codegen_init(codegen_t *cg, program_t *prog, struct arena_t *arena,
                  const char *name) {
  NY_LOG_V1("Initializing codegen for module: %s\n", name);
  memset(cg, 0, sizeof(codegen_t));
  cg->prog = prog;
  cg->arena = arena;
  LLVMInitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  cg->ctx = LLVMContextCreate();
  cg->llvm_ctx_owned = true;
  cg->module = LLVMModuleCreateWithNameInContext(name, cg->ctx);
  cg->builder = LLVMCreateBuilderInContext(cg->ctx);
  cg->type_i1 = LLVMInt1TypeInContext(cg->ctx);
  cg->type_i8 = LLVMInt8TypeInContext(cg->ctx);
  cg->type_i16 = LLVMInt16TypeInContext(cg->ctx);
  cg->type_i32 = LLVMInt32TypeInContext(cg->ctx);
  cg->type_i64 = LLVMInt64TypeInContext(cg->ctx);
  cg->type_i128 = LLVMInt128TypeInContext(cg->ctx);
  cg->type_u8 = cg->type_i8;
  cg->type_u16 = cg->type_i16;
  cg->type_u32 = cg->type_i32;
  cg->type_u64 = cg->type_i64;
  cg->type_u128 = cg->type_i128;
  cg->type_f32 = LLVMFloatTypeInContext(cg->ctx);
  cg->type_f64 = LLVMDoubleTypeInContext(cg->ctx);
  cg->type_f128 = LLVMFP128TypeInContext(cg->ctx);
  cg->type_bool = cg->type_i1;
  cg->type_i8ptr = LLVMPointerType(cg->type_i8, 0);

  cg->strict_diagnostics = getenv("NYTRIX_STRICT_DIAGNOSTICS") != NULL;
  cg->implicit_prelude = true;
  if (cg->strict_diagnostics)
    NY_LOG_V1("Strict diagnostics enabled (NYTRIX_STRICT_DIAGNOSTICS)\n");
  add_builtins(cg);
  LLVMAddGlobal(cg->module, cg->type_i64, "__NYTRIX__");
}

const char *codegen_qname(codegen_t *cg, const char *name,
                          const char *cur_mod) {
  (void)cg;
  if (!cur_mod || !*cur_mod)
    return name;
  size_t mlen = strlen(cur_mod);
  // Check if already prefixed
  if (strncmp(name, cur_mod, mlen) == 0 && name[mlen] == '.')
    return name;
  static char
      buf[1024]; // Note: not thread safe, but codegen is single threaded
  snprintf(buf, sizeof(buf), "%s.%s", cur_mod, name);
  return ny_strdup(buf);
}

void emit_top_functions(codegen_t *cg, stmt_t *s, scope *gsc, size_t gd,
                        const char *cur_mod) {
  if (s->kind == NY_S_FUNC) {
    cg->current_module_name = cur_mod;
    cg->current_module_name = cur_mod;
    const char *final_name = codegen_qname(cg, s->as.fn.name, cur_mod);
    gen_func(cg, s, final_name, gsc, gd, NULL);
  } else if (s->kind == NY_S_MODULE) {
    for (size_t i = 0; i < s->as.module.body.len; i++)
      emit_top_functions(cg, s->as.module.body.data[i], gsc, gd,
                         s->as.module.name);
  }
}

void codegen_emit(codegen_t *cg) {
  scope gsc[64] = {0};
  size_t gd = 0;
  // Collect module aliases and use-modules before function bodies are emitted
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    collect_use_aliases(cg, cg->prog->body.data[i]);
    collect_use_modules(cg, cg->prog->body.data[i]);
  }
  // First pass: collect all signatures (including nested modules)
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    stmt_t *s = cg->prog->body.data[i];

    collect_sigs(cg, s);
  }
  // Process exports to create aliases
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    process_exports(cg, cg->prog->body.data[i]);
  }
  // Process explicit imports after sigs/exports are known
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    process_use_imports(cg, cg->prog->body.data[i]);
  }
  // Second pass: emit function bodies
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    emit_top_functions(cg, cg->prog->body.data[i], gsc, gd, NULL);
  }
}

LLVMValueRef codegen_emit_script(codegen_t *cg, const char *name) {
  cg->current_module_name = NULL;
  LLVMValueRef fn = LLVMGetNamedFunction(cg->module, name);
  if (fn)
    return fn;
  fn = LLVMAddFunction(cg->module, name,
                       LLVMFunctionType(cg->type_i64, NULL, 0, 0));
  LLVMMetadataRef prev_scope = cg->di_scope;
  if (cg->debug_symbols && cg->di_builder) {
    token_t tok = {0};
    tok.filename = cg->debug_main_file ? cg->debug_main_file : "<inline>";
    tok.line = 1;
    tok.col = 1;
    LLVMMetadataRef sp = codegen_debug_subprogram(cg, fn, name, tok);
    if (sp)
      cg->di_scope = sp;
  }
  LLVMBasicBlockRef cur = LLVMGetInsertBlock(cg->builder);
  // Create two blocks: init (for internal setup) and body (for user code)
  LLVMBasicBlockRef init_block = LLVMAppendBasicBlock(fn, "init");
  LLVMBasicBlockRef body_block = LLVMAppendBasicBlock(fn, "body");
  // 1. Generate user code first (into body_block) to discover all strings
  LLVMPositionBuilderAtEnd(cg->builder, body_block);
  scope sc[64] = {0};
  size_t d = 0;
  for (size_t i = 0; i < cg->prog->body.len; i++) {
    if (cg->prog->body.data[i]->kind != NY_S_FUNC) {
      gen_stmt(cg, sc, &d, cg->prog->body.data[i], 0, false);
    }
  }
  // If the user code didn't terminate, add a return logic
  if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(cg->builder))) {
    LLVMBuildRet(cg->builder, LLVMConstInt(cg->type_i64, 1, false));
  }
  // 2. Now fill the init block
  LLVMPositionBuilderAtEnd(cg->builder, init_block);
  if (cg->debug_symbols && cg->di_builder) {
    LLVMSetCurrentDebugLocation2(cg->builder, NULL);
  }
  // Initialize ALL runtime string pointers found
  LLVMTypeRef i8_ptr_ty = LLVMPointerType(LLVMInt8TypeInContext(cg->ctx), 0);
  LLVMTypeRef asm_func_ty =
      LLVMFunctionType(i8_ptr_ty, (LLVMTypeRef[]){i8_ptr_ty}, 1, false);
  LLVMValueRef identity_asm =
      LLVMConstInlineAsm(asm_func_ty, "", "=r,0", true, false);
  for (size_t i = 0; i < cg->interns.len; i++) {
    LLVMValueRef str_array_global = cg->interns.data[i].gv;
    LLVMValueRef runtime_ptr_global = cg->interns.data[i].val;
    if (!str_array_global || !runtime_ptr_global)
      continue;
    if (LLVMGetTypeKind(LLVMTypeOf(runtime_ptr_global)) != LLVMPointerTypeKind)
      continue;
    // 1. Bitcast global to i8*
    LLVMValueRef global_i8_ptr =
        LLVMBuildBitCast(cg->builder, str_array_global, i8_ptr_ty, "");
    // 2. Pass through identity ASM to prevent constant folding
    LLVMValueRef runtime_base =
        LLVMBuildCall2(cg->builder, asm_func_ty, identity_asm,
                       (LLVMValueRef[]){global_i8_ptr}, 1, "");
    // 3. GEP offset 64 bytes
    LLVMValueRef indices[] = {LLVMConstInt(cg->type_i64, 64, 0)};
    LLVMValueRef str_data_ptr =
        LLVMBuildInBoundsGEP2(cg->builder, LLVMInt8TypeInContext(cg->ctx),
                              runtime_base, indices, 1, "");
    // 4. Convert to int and store
    LLVMValueRef str_data_int =
        LLVMBuildPtrToInt(cg->builder, str_data_ptr, cg->type_i64, "");
    LLVMBuildStore(cg->builder, str_data_int, runtime_ptr_global);
  }
  // Jump from init to body
  LLVMBuildBr(cg->builder, body_block);
  // Cleanup
  vec_free(&sc[0].defers);
  vec_free(&sc[0].vars);
  // Restore builder to end of function (just in case caller expects it)
  if (cur) {
    LLVMPositionBuilderAtEnd(cg->builder, cur);
  }
  cg->di_scope = prev_scope;
  return fn;
}

void codegen_dispose(codegen_t *cg) {
  if (!cg)
    return;
  codegen_debug_finalize(cg);
  if (cg->builder) {
    LLVMDisposeBuilder(cg->builder);
    cg->builder = NULL;
  }
  if (cg->llvm_ctx_owned) {
    if (cg->module) {
      LLVMDisposeModule(cg->module);
      cg->module = NULL;
    }
    if (cg->ctx) {
      LLVMContextDispose(cg->ctx);
      cg->ctx = NULL;
    }
  }

  // fun_sig names/link_names may be shared across aliases and module exports;
  // avoid freeing here to prevent double-free on shared pointers.
  // Names/aliases may be shared or arena-backed; avoid freeing to prevent
  // double-free across reused compilation contexts.
  for (size_t i = 0; i < cg->use_modules.len; i++) {
    free((void *)cg->use_modules.data[i]);
  }
  for (size_t i = 0; i < cg->user_use_modules.len; i++) {
    free((void *)cg->user_use_modules.data[i]);
  }
  for (size_t i = 0; i < cg->layouts.len; i++) {
    layout_def_t *def = cg->layouts.data[i];
    if (!def)
      continue;
    vec_free(&def->fields);
  }
  vec_free(&cg->fun_sigs);
  vec_free(&cg->global_vars);
  vec_free(&cg->interns);
  vec_free(&cg->aliases);
  vec_free(&cg->import_aliases);
  vec_free(&cg->user_import_aliases);
  vec_free(&cg->use_modules);
  vec_free(&cg->user_use_modules);
  vec_free(&cg->layouts);
  if (cg->prog && cg->prog_owned) {
    program_free(cg->prog, (arena_t *)cg->arena);
    free(cg->prog);
  }
}

void codegen_reset(codegen_t *cg) { (void)cg; }
